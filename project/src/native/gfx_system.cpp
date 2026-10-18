#include "gfx_system.h"

#include <algorithm>
#include <chrono>

#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

#include "renderer.h"

namespace fs3gfx {

namespace {
NativeGraphicsSystem* g_system = nullptr;
}

NativeGraphicsSystem* GraphicsSystem() { return g_system; }

NativeGraphicsSystem::NativeGraphicsSystem() { g_system = this; }

NativeGraphicsSystem::~NativeGraphicsSystem() {
  Shutdown();
  if (g_system == this) g_system = nullptr;
}

rex::ui::GraphicsProvider* NativeGraphicsSystem::provider() const { return provider_.get(); }

X_STATUS NativeGraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext* app_context) {
  if (presenter_) return X_STATUS_SUCCESS;
  if (!provider_) {
    provider_ = rex::ui::d3d12::D3D12Provider::Create();
    if (!provider_) {
      REXLOG_ERROR("[native-gfx] Direct3D 12 is not available");
      return X_STATUS_UNSUCCESSFUL;
    }
  }
  app_context_ = app_context;
  auto loss = [](bool, bool) { rex::FatalError("Graphics device lost"); };
  if (app_context_) {
    app_context_->CallInUIThreadSynchronous([this, loss]() { presenter_ = provider_->CreatePresenter(loss); });
  } else {
    presenter_ = provider_->CreatePresenter(loss);
  }
  if (!presenter_) {
    REXLOG_ERROR("[native-gfx] unable to create the presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  if (!Renderer::Get().Initialize(provider_.get(), presenter_.get())) {
    REXLOG_ERROR("[native-gfx] native renderer initialization failed");
    return X_STATUS_UNSUCCESSFUL;
  }
  REXLOG_INFO("[native-gfx] native Direct3D renderer ready (no GPU emulation)");
  return X_STATUS_SUCCESS;
}

X_STATUS NativeGraphicsSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                                             rex::system::KernelState* kernel_state) {
  dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;
  Renderer::Get().AttachMemory(function_dispatcher->memory());

  // Display controller registers polled by the D3D library (interrupt status, scanline, size).
  function_dispatcher->memory()->AddVirtualMappedRange(
      0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
      reinterpret_cast<rex::runtime::MMIOReadCallback>(ReadRegisterThunk),
      reinterpret_cast<rex::runtime::MMIOWriteCallback>(WriteRegisterThunk));

  vblank_running_ = true;
  vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        double hz = std::max(1.0, double(float(mode.refresh_rate)));
        auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / hz));
        auto next = std::chrono::steady_clock::now() + period;
        while (vblank_running_) {
          auto now = std::chrono::steady_clock::now();
          if (now >= next) {
            DispatchInterrupt(0, 2);
            next += period;
            if (now - next > period * 4) next = now + period;  // don't burst after a stall
          } else {
            rex::thread::Sleep(std::chrono::milliseconds(1));
          }
        }
        return 0;
      }));
  vblank_thread_->set_name("Native VBlank");
  vblank_thread_->Create();
  return X_STATUS_SUCCESS;
}

void NativeGraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_data_ = user_data;
  interrupt_callback_ = callback;
  Renderer::Get().SetGuestDevice(user_data);
  // The callback's data is the D3D device: run its library in GPU-less mode (see renderer.h).
  if (user_data && kernel_state_)
    Renderer::Get().EnableGpulessMode(kernel_state_->memory()->virtual_membase(), user_data);
  REXLOG_INFO("[native-gfx] graphics interrupt callback {:08X} device {:08X}", callback, user_data);
}

void NativeGraphicsSystem::DispatchInterrupt(uint32_t source, uint32_t cpu) {
  uint32_t callback = interrupt_callback_.load();
  if (!callback || !dispatcher_) return;
  auto* thread = rex::system::XThread::GetCurrentThread();
  if (!thread) return;
  thread->SetActiveCpu(uint8_t(cpu));
  uint64_t args[] = {source, interrupt_data_.load()};
  dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
}

uint32_t NativeGraphicsSystem::ReadRegisterThunk(void*, NativeGraphicsSystem*, uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;
  rex::system::X_VIDEO_MODE mode;
  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C:  // D1MODE_V_COUNTER
      rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
      return std::min(uint32_t(mode.display_height), 0xFFFu);
    case 0x1951:  // display interrupt status: vblank
      return 1;
    case 0x1961:  // D1MODE_VIEWPORT_SIZE
      rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
      return (std::min(uint32_t(mode.display_width), 0xFFFu) << 16) | std::min(uint32_t(mode.display_height), 0xFFFu);
    default:
      return 0;
  }
}

void NativeGraphicsSystem::WriteRegisterThunk(void*, NativeGraphicsSystem*, uint32_t, uint32_t) {
  // Scanout address / ring write pointer writes have no consumer: presentation is native.
}

void NativeGraphicsSystem::Shutdown() {
  if (vblank_thread_) {
    vblank_running_ = false;
    vblank_thread_->Wait(0, 0, 0, nullptr);
    vblank_thread_.reset();
  }
  Renderer::Get().Shutdown();
  if (presenter_) {
    if (app_context_) app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    presenter_.reset();
  }
  provider_.reset();
}

}  // namespace fs3gfx
