// Native graphics system for FIFA Street 3: replaces the SDK's Xenos GPU emulation plugin.
//
// What the guest still needs from "the GPU" side of the kernel is small: the display controller
// registers it polls over MMIO, the vertical blank interrupt, and somewhere to present frames.
// No command processor exists - the game's Direct3D library is intercepted at the API level
// (native_hooks.cpp) and drawn by fs3gfx::Renderer, so nothing reads the ring buffer.
#pragma once

#include <atomic>
#include <memory>

#include <rex/system/interfaces/graphics.h>
#include <rex/system/xobject.h>

namespace rex::system {
class XHostThread;
}
namespace rex::ui::d3d12 {
class D3D12Provider;
}

namespace fs3gfx {
using rex::X_STATUS;

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeGraphicsSystem();
  ~NativeGraphicsSystem() override;

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                         rex::system::KernelState* kernel_state) override;
  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override;
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }
  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  void Shutdown() override;

  rex::ui::d3d12::D3D12Provider* d3d12_provider() const { return provider_.get(); }

  // Dispatches the guest graphics interrupt on the calling (guest-capable) thread.
  void DispatchInterrupt(uint32_t source, uint32_t cpu);

 private:
  static uint32_t ReadRegisterThunk(void* ppc_context, NativeGraphicsSystem* self, uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, NativeGraphicsSystem* self, uint32_t addr, uint32_t value);

  std::unique_ptr<rex::ui::d3d12::D3D12Provider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::ui::WindowedAppContext* app_context_ = nullptr;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  std::atomic<uint32_t> interrupt_callback_{0};
  std::atomic<uint32_t> interrupt_data_{0};
  std::atomic<bool> vblank_running_{false};
  rex::system::object_ref<rex::system::XHostThread> vblank_thread_;
};

// The single instance (created by the app in OnPreSetup).
NativeGraphicsSystem* GraphicsSystem();

}  // namespace fs3gfx
