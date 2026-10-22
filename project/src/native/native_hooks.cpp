// Interception of the game's statically linked Direct3D (Xbox 360 XDK) library.
//
// Addresses and argument registers come from the static analysis in HLE_PLAN.md
// ("Decisões de 16/10/2026"). Operations that would produce GPU commands are replaced with native
// renderer calls; the command buffer kickoff never reaches a ring buffer, and the "GPU" fences and
// flip completion the library waits on are signalled here, where the native work is submitted.

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/fifastreet3_init.h"
#include "gfx_system.h"
#include "renderer.h"
#include "../pc_settings.h"

REX_EXTERN(__imp__sub_82507AE8);  // CreateVertexShader(container) -> object
REX_EXTERN(__imp__sub_825079D8);  // CreatePixelShader(container) -> object
REX_EXTERN(__imp__sub_8250AB58);  // Swap(device, front buffer texture, ...)
REX_EXTERN(__imp__sub_82505678);  // one iteration of the D3D library's GPU wait loops
REX_EXTERN(sub_8250A518);         // flip completion (normally run by the GPU interrupt)

namespace fs3_performance {
double FramesPerSecond() { return fs3gfx::Renderer::Get().frames_per_second(); }
}

using fs3gfx::Renderer;

void UpdateMenuButton(uint8_t* base);  // menu_hooks.cpp

namespace {

// Holds the flip to the presentation interval (in vertical blanks, 60 Hz on the console), as the
// display would.
// With the D3D library in GPU-less mode nothing else waits for the vertical blank.
void PaceFlip(uint32_t interval) {
  using clock = std::chrono::steady_clock;
  static clock::time_point next = clock::now();
  if (!interval) return;
  const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(interval / pcs::GuestRefreshHz()));
  next += period;
  auto now = clock::now();
  if (next <= now) {
    if (now - next > period * 2) next = now;  // fell behind (loading): don't run fast to catch up
    return;
  }
  // Sleep coarsely, then spin the last stretch for an even cadence.
  if (next - now > std::chrono::milliseconds(2)) std::this_thread::sleep_until(next - std::chrono::milliseconds(2));
  while (clock::now() < next) std::this_thread::yield();
}

}  // namespace

REX_HOOK_RAW(sub_82507AE8) {
  uint32_t container = ctx.r3.u32;
  __imp__sub_82507AE8(ctx, base);
  Renderer::Get().RegisterShader(base, ctx.r3.u32, container);
}

REX_HOOK_RAW(sub_825079D8) {
  uint32_t container = ctx.r3.u32;
  __imp__sub_825079D8(ctx, base);
  Renderer::Get().RegisterShader(base, ctx.r3.u32, container);
}

// DrawVertices(device, primitive, start vertex, vertex count)
REX_HOOK_RAW(sub_824FF990) {
  fs3gfx::DrawCall call{ctx.r3.u32, ctx.r4.u32, 0, ctx.r5.u32, ctx.r6.u32, false};
  Renderer::Get().Draw(base, call);
}

// DrawIndexedVertices(device, primitive, base vertex, start index, index count)
REX_HOOK_RAW(sub_824FFD78) {
  fs3gfx::DrawCall call{ctx.r3.u32, ctx.r4.u32, int32_t(ctx.r5.u32), ctx.r6.u32, ctx.r7.u32, true};
  Renderer::Get().Draw(base, call);
}

// Clear(device, flags, rect, color, f1 = z, stencil)
REX_HOOK_RAW(sub_82508D68) {
  fs3gfx::ClearCall call{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, float(ctx.f1.f64), ctx.r7.u32};
  Renderer::Get().Clear(base, call);
}

// Resolve(device, flags, source rect, dest texture, dest point, level, slice, clear color,
//         f1 = clear z, stack: clear stencil)
REX_HOOK_RAW(sub_82509560) {
  uint32_t stencil = rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, ctx.r1.u32 + 0x64));
  fs3gfx::ResolveCall call{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                           ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, float(ctx.f1.f64), stencil};
  Renderer::Get().Resolve(base, call);
}

// BeginExport / EndExport around memexport draws: cache coherency only on the GPU.
REX_HOOK_RAW(sub_824FDAB0) { (void)ctx; (void)base; }
REX_HOOK_RAW(sub_824FDBB8) { Renderer::Get().EndExport(base, ctx.r5.u32); }

// Command buffer kickoff: nothing consumes a ring buffer any more. Everything submitted so far
// has been executed natively, so every inserted fence is reached.
REX_HOOK_RAW(sub_824FE658) { Renderer::Get().SignalFences(base, ctx.r3.u32); }

REX_HOOK_RAW(sub_82505678) {
  uint32_t device = rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, ctx.r3.u32));
  Renderer::Get().SignalFences(base, device);
  __imp__sub_82505678(ctx, base);
}

REX_HOOK_RAW(sub_8250AB58) {
  Renderer::Get().SetFrontBuffer(base, ctx.r4.u32);
  __imp__sub_8250AB58(ctx, base);
}

// Queue the flip for the GPU (presentation-interval mode): present natively and complete the flip
// the way the GPU's interrupt callback would.
REX_HOOK_RAW(sub_8250A6B0) {
  uint32_t device = ctx.r3.u32;
  uint32_t front = ctx.r4.u32;
  Renderer::Get().Present(base);
  Renderer::Get().SignalFences(base, device);
  int32_t mode = int32_t(rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, device + 0x350C)));
  uint32_t interval = (mode == 0 || mode == 1) ? 1 : mode == 2 ? 2 : mode == 4 ? 3 : mode == INT32_MIN ? 0 : 1;
  static int32_t logged_mode = -12345;
  static int mode_logs = 0;
  if (mode != logged_mode && mode_logs < 40) {
    ++mode_logs;
    logged_mode = mode;
    REXLOG_INFO("[native-gfx] presentation interval mode {} -> {} vblank(s)", mode, interval);
  }
  uint32_t flags = (rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, device + 0x2E44)) >> 23) & 0x7F;
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = (front & 0xFFFFF000u) | (interval << 8) | flags;
  sub_8250A518(frame, base);
  *rex::memory::GuestPtr(base, device + 0x2ABD) |= 0x10;
  UpdateMenuButton(base);
  PaceFlip(interval);
}
