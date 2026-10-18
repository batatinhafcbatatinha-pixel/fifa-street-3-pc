// Native Direct3D 11 renderer for FIFA Street 3 (D3D11On12 on the SDK's D3D12 device, so the SDK
// presenter and ImGui overlay keep working).
//
// The game's statically linked Direct3D 9 (Xbox 360) library keeps its CPU-side state in the guest
// device structure; native_hooks.cpp intercepts the operations that would have produced GPU
// commands (draws, clears, resolves, the flip) and calls into this renderer, which reads the API
// state from the device and issues equivalent host draws. No PM4 command stream is generated or
// interpreted.
#pragma once

#include <cstdint>
#include <memory>

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class Presenter;
}
namespace rex::ui::d3d12 {
class D3D12Provider;
}

namespace fs3gfx {

struct DrawCall {
  uint32_t device;      // guest D3D device address
  uint32_t primitive;   // xenos::PrimitiveType
  int32_t base_vertex;  // added to every index (VGT_INDX_OFFSET)
  uint32_t start;       // start index (indexed) or start vertex (non-indexed)
  uint32_t count;
  bool indexed;
};

struct ClearCall {
  uint32_t device;
  uint32_t flags;          // bit i: render target i, 0x10 depth, 0x20 stencil
  uint32_t rect;           // guest D3DRECT* or 0
  uint32_t color;          // guest float[4]* or 0
  float depth;
  uint32_t stencil;
};

struct ResolveCall {
  uint32_t device;
  uint32_t flags;          // 0-3 render target, 4 depth; 0x100 clear color, 0x200 clear depth/stencil
  uint32_t source_rect;    // guest D3DRECT* or 0
  uint32_t dest_texture;   // guest D3DBaseTexture* or 0
  uint32_t dest_point;     // guest D3DPOINT* or 0
  uint32_t level;
  uint32_t slice;
  uint32_t clear_color;    // guest float[4]* or 0
  float clear_depth;
  uint32_t clear_stencil;
};

class Renderer {
 public:
  static Renderer& Get();

  bool Initialize(rex::ui::d3d12::D3D12Provider* provider, rex::ui::Presenter* presenter);
  void AttachMemory(rex::memory::Memory* memory);
  void SetGuestDevice(uint32_t device) { guest_device_ = device; }
  void Shutdown();
  bool ready() const;

  // Called from the game's thread with `base` = guest virtual membase.
  void RegisterShader(uint8_t* base, uint32_t object, uint32_t container);
  void Draw(uint8_t* base, const DrawCall& call);
  void Clear(uint8_t* base, const ClearCall& call);
  void Resolve(uint8_t* base, const ResolveCall& call);
  void EndExport(uint8_t* base, uint32_t resource);
  // Front buffer texture header (guest address of the D3D texture) of the frame being flipped.
  void SetFrontBuffer(uint8_t* base, uint32_t texture);
  void Present(uint8_t* base);
  // Marks every GPU fence the guest D3D library inserted as reached.
  void SignalFences(uint8_t* base, uint32_t device);
  // The D3D library has a mode for running without a GPU (device+0x2ABD bit 0x02, also what it
  // falls back to after a GPU hang): waits return at once, kickoffs skip the ring and fences are
  // written by the CPU when inserted. Work is executed natively as it is issued, so that is exact.
  void EnableGpulessMode(uint8_t* base, uint32_t device);

  double frames_per_second() const;

 private:
  Renderer();
  ~Renderer();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint32_t guest_device_ = 0;
};

}  // namespace fs3gfx
