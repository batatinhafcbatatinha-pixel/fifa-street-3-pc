// Native D3D11 renderer for the game's Direct3D (Xbox 360) API calls. See renderer.h.

#define NOMINMAX
#include "renderer.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/runtime.h>
#include <rex/system/xvideo.h>
#include <rex/ui/window.h>

#include "pc_game_info.h"

#include "guest_texture.h"
#include "xenos_hlsl.h"

using Microsoft::WRL::ComPtr;

namespace fs3gfx {

namespace {

constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPhysicalSize = 0x20000000;
constexpr uint32_t kPageCount = kPhysicalSize >> kPageShift;

inline uint32_t Be32(const uint8_t* p) { return _byteswap_ulong(*reinterpret_cast<const uint32_t*>(p)); }
inline uint16_t Be16(const uint8_t* p) { return _byteswap_ushort(*reinterpret_cast<const uint16_t*>(p)); }
inline float BeF(const uint8_t* p) {
  uint32_t v = Be32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
// Host pointer of a guest virtual address (the 0xE0000000 view is offset by a page on Windows).
inline const uint8_t* G(uint8_t* base, uint32_t address) { return rex::memory::GuestPtr(base, address); }
// Guest virtual address of physical memory (0xA0000000/0xC0000000/0xE0000000 views) -> physical.
inline uint32_t ToPhys(uint32_t v) { return (((v >> 20) + 0x200) & 0x1000) + (v & 0x1FFFFFFF); }

// Texture fetch constants are copied from texture headers, which hold CPU virtual addresses for the
// base and mip surfaces; textures are keyed and read by physical address.
void NormalizeFetch(uint32_t f[6]) {
  if (uint32_t a = f[1] & 0xFFFFF000u; a >= kPhysicalSize) f[1] = (f[1] & 0xFFFu) | (ToPhys(a) & 0x1FFFF000u);
  if (uint32_t a = f[5] & 0xFFFFF000u; a >= kPhysicalSize) f[5] = (f[5] & 0xFFFu) | (ToPhys(a) & 0x1FFFF000u);
}

uint64_t HashBytes(const void* data, size_t size, uint64_t h = 0xCBF29CE484222325ull) {
  auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; i++) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

// Reader for the guest D3D device structure.
struct Dev {
  uint8_t* base;
  uint32_t addr;
  uint32_t u32(uint32_t off) const { return Be32(G(base, addr + off)); }
  float f32(uint32_t off) const { return BeF(G(base, addr + off)); }
  uint8_t u8(uint32_t off) const { return *G(base, addr + off); }
  // Shadow of GPU register r, kept by the D3D library.
  uint32_t reg(uint32_t r) const {
    uint32_t off;
    if (r >= 0x2380) off = 0x2A50 + (r - 0x2380) * 4;
    else if (r >= 0x2300) off = 0x29B8 + (r - 0x2300) * 4;
    else if (r >= 0x2280) off = 0x2964 + (r - 0x2280) * 4;
    else if (r >= 0x2200) off = 0x2934 + (r - 0x2200) * 4;
    else if (r >= 0x2180) off = 0x2920 + (r - 0x2180) * 4;
    else if (r >= 0x2100) off = 0x28CC + (r - 0x2100) * 4;
    else off = 0x2880 + (r - 0x2000) * 4;
    return u32(off);
  }
  float regf(uint32_t r) const {
    uint32_t v = reg(r);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
};

struct XeState {
  uint32_t bools[8];
  uint32_t loops[32];
  uint32_t vfetch[192];
  uint32_t strides[16];
  uint32_t tex[32][4];
  float tex_dim[32][4];
  float ndc[4];
  float alpha[4];
  float rt_bias[4];
  uint32_t flags[4];
  float ucp[6][4];
  float zxform[4];  // Xenos viewport z scale/offset, applied in the vertex shader
};
static_assert(sizeof(XeState) == 2192, "XeState must match the HLSL cbuffer");

struct ShaderEntry {
  ParsedShader parsed;
  // Pixel shader variants per parameter generation register (SQ_CONTEXT_MISC; 0xFF = disabled).
  struct PsVariant {
    ComPtr<ID3D11PixelShader> ps;
    TranslatedShader info;
    bool failed = false;
  };
  std::unordered_map<uint32_t, PsVariant> ps;
  // Vertex shader variants per vertex declaration.
  struct VsVariant {
    ComPtr<ID3D11VertexShader> vs;
    TranslatedShader info;
    bool failed = false;
  };
  std::unordered_map<uint64_t, VsVariant> vs;
};

struct RenderTarget {
  ComPtr<ID3D11Texture2D> tex;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11RenderTargetView> rtv_gamma;  // 8_8_8_8_GAMMA writes (sRGB encode); SRV reads raw
  ComPtr<ID3D11DepthStencilView> dsv;
  ComPtr<ID3D11ShaderResourceView> srv;
  uint32_t width = 0, height = 0, samples = 1;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool depth = false;
  float scale = 1.0f;  // host texels per guest texel (internal resolution)
};

struct TextureEntry {
  TexFetch fetch;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;         // host format (unsigned view)
  DXGI_FORMAT format_signed = DXGI_FORMAT_UNKNOWN;  // view when every component is signed
  uint32_t width = 1, height = 1, depth = 1, levels = 1;
  bool is_3d = false, is_cube = false;
  ComPtr<ID3D11Resource> tex;
  ComPtr<ID3D11ShaderResourceView> srv, srv_signed;              // natural dimension
  ComPtr<ID3D11ShaderResourceView> srv_array, srv_array_signed;  // 2D/stacked as a Texture2DArray
  std::unordered_map<uint32_t, ComPtr<ID3D11RenderTargetView>> rtv;  // per array slice
  uint64_t gen_sum = ~0ull;  // page write generation sum at upload/resolve time
  uint32_t base = 0, base_bytes = 0, mip = 0, mip_bytes = 0;
  bool can_generate_mips = false;
  float scale = 1.0f;  // resolve destinations: that of the targets resolved into it
  bool renderable = false;
};

DXGI_FORMAT Typeless(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_TYPELESS;
    case DXGI_FORMAT_R8G8_UNORM: return DXGI_FORMAT_R8G8_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R16G16_UNORM: return DXGI_FORMAT_R16G16_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_SNORM: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_BC4_UNORM: return DXGI_FORMAT_BC4_TYPELESS;
    case DXGI_FORMAT_BC5_UNORM: return DXGI_FORMAT_BC5_TYPELESS;
    default: return f;
  }
}

bool IsBlockCompressed(DXGI_FORMAT f) {
  return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM);
}

DXGI_FORMAT ColorRtFormat(uint32_t f) {
  switch (f) {
    case 0: case 1: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 2: case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 3: case 12: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 4: return DXGI_FORMAT_R16G16_SNORM;
    case 5: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case 6: return DXGI_FORMAT_R16G16_FLOAT;
    case 7: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 14: return DXGI_FORMAT_R32_FLOAT;
    case 15: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

D3D11_BLEND BlendFactor(uint32_t f, bool alpha) {
  switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 4: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 5: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 6: return D3D11_BLEND_SRC_ALPHA;
    case 7: return D3D11_BLEND_INV_SRC_ALPHA;
    case 8: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 9: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 10: return D3D11_BLEND_DEST_ALPHA;
    case 11: return D3D11_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D11_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D11_BLEND_SRC_ALPHA_SAT;
    default: return D3D11_BLEND_ONE;
  }
}

D3D11_BLEND_OP BlendOp(uint32_t op) {
  switch (op) {
    case 1: return D3D11_BLEND_OP_SUBTRACT;
    case 2: return D3D11_BLEND_OP_MIN;
    case 3: return D3D11_BLEND_OP_MAX;
    case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: return D3D11_BLEND_OP_ADD;
  }
}

D3D11_TEXTURE_ADDRESS_MODE AddressMode(uint32_t m) {
  switch (m) {
    case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 2: case 4: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 3: case 5: case 7: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default: return D3D11_TEXTURE_ADDRESS_BORDER;
  }
}

const char* kUtilityHlsl = R"HLSL(
cbuffer CopyParams : register(b0) { int4 xe_src_origin; int4 xe_dst_origin; float4 xe_scale; };
float4 vs_fullscreen(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
Texture2D<float4> xe_src : register(t0);
Texture2DMS<float4> xe_src_ms : register(t0);
float4 ps_copy(float4 p : SV_Position) : SV_Target {
  int2 c = int2(p.xy) - xe_dst_origin.xy + xe_src_origin.xy;
  float4 v = xe_src.Load(int3(c, 0)) * xe_scale.x;
  return xe_src_origin.w != 0 ? v.bgra : v;
}
float4 ps_copy_ms(float4 p : SV_Position) : SV_Target {
  int2 c = int2(p.xy) - xe_dst_origin.xy + xe_src_origin.xy;
  float4 s = 0.0;
  for (int i = 0; i < xe_src_origin.z; i++) s += xe_src_ms.Load(c, i);
  s = s / float(xe_src_origin.z) * xe_scale.x;
  return xe_src_origin.w != 0 ? s.bgra : s;
}
float4 ps_copy_depth(float4 p : SV_Position) : SV_Target {
  int2 c = int2(p.xy) - xe_dst_origin.xy + xe_src_origin.xy;
  return xe_src.Load(int3(c, 0)).rrrr;
}
float4 ps_copy_depth_ms(float4 p : SV_Position) : SV_Target {
  int2 c = int2(p.xy) - xe_dst_origin.xy + xe_src_origin.xy;
  return xe_src_ms.Load(c, 0).rrrr;
}
)HLSL";

const char* kRectGsHlsl = R"HLSL(
struct XeInterp {
  float4 pos : SV_Position; float4 v[24] : TEXCOORD0; float psize : PSIZE;
  float4 clip0 : SV_ClipDistance0; float2 clip1 : SV_ClipDistance1;
};
XeInterp xe_combine(XeInterp b, XeInterp c, XeInterp a) {
  XeInterp o;
  o.pos = b.pos + c.pos - a.pos;
  [unroll] for (int k = 0; k < 24; k++) o.v[k] = b.v[k] + c.v[k] - a.v[k];
  o.psize = a.psize;
  o.clip0 = b.clip0 + c.clip0 - a.clip0;
  o.clip1 = b.clip1 + c.clip1 - a.clip1;
  return o;
}
[maxvertexcount(4)]
void main(triangle XeInterp v[3], inout TriangleStream<XeInterp> s) {
  // The corner is the vertex opposite the longest edge.
  float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
  float d0 = dot(p1 - p2, p1 - p2), d1 = dot(p0 - p2, p0 - p2), d2 = dot(p0 - p1, p0 - p1);
  int c = (d0 >= d1 && d0 >= d2) ? 0 : (d1 >= d2 ? 1 : 2);
  int i0 = c == 0 ? 1 : 0, i1 = c == 2 ? 1 : 2;
  s.Append(v[c]);
  s.Append(v[i0]);
  s.Append(v[i1]);
  s.Append(xe_combine(v[i0], v[i1], v[c]));
}
)HLSL";

// FXC can crash on unusual input (seen in the BF2 port); a crash inside it must not take the game
// down, so it runs under SEH and a fault counts as a failed compile.
HRESULT CompileGuarded(pD3DCompile compile, const std::string& src, const char* entry, const char* target,
                       UINT flags, ID3DBlob** code, ID3DBlob** errors) {
  HRESULT hr = E_FAIL;
  __try {
    hr = compile(src.data(), src.size(), "fs3", nullptr, nullptr, entry, target, flags, 0, code, errors);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    hr = E_FAIL;
  }
  return hr;
}

}  // namespace

struct Renderer::Impl {
  rex::memory::Memory* memory = nullptr;
  uint8_t* phys = nullptr;

  ComPtr<ID3D11Device1> dev;
  ComPtr<ID3D11DeviceContext1> ctx;
  // Own swap chain on the SDK window (created at the first present).
  ComPtr<IDXGISwapChain1> swap;
  ComPtr<ID3D11RenderTargetView> back_rtv;
  uint32_t back_w = 0, back_h = 0;
  HWND hwnd = nullptr;
  pD3DCompile compile = nullptr;
  std::recursive_mutex mutex;
  // Internal resolution (PC settings, fixed at boot): render targets and resolve destinations are
  // `scale` times their guest size when at least 640 wide (the scene, half-resolution effects,
  // shadow maps); smaller ones (reduction chains, probes) keep it. Everything else stays in guest
  // pixels.
  float scale = 1.0f;
  float ScaleFor(uint32_t width) const { return width >= 640 ? scale : 1.0f; }
  static uint32_t ScaledSize(uint32_t v, float s) {
    return std::max<uint32_t>(1, uint32_t(std::ceil(float(v) * s - 0.001f)));
  }
  static LONG ScaledCoord(LONG v, float s) { return LONG(std::lround(float(v) * s)); }
  static D3D11_RECT ScaledRect(const D3D11_RECT& r, float s) {
    return {ScaledCoord(r.left, s), ScaledCoord(r.top, s), ScaledCoord(r.right, s), ScaledCoord(r.bottom, s)};
  }
  bool ready = false;

  // Guest physical memory mirror.
  ComPtr<ID3D11Buffer> mem_buf;
  ComPtr<ID3D11ShaderResourceView> mem_srv;
  ComPtr<ID3D11UnorderedAccessView> mem_uav;
  std::vector<std::atomic<uint32_t>> write_gen;  // incremented by the CPU write watch
  std::vector<uint32_t> mirror_gen;              // generation uploaded to the mirror
  std::map<uint32_t, std::pair<uint32_t, uint64_t>> exported;  // memexport output: start -> (end, frame)
  void* invalidation_handle = nullptr;

  ComPtr<ID3D11Buffer> cb_vs_float, cb_ps_float, cb_vs_state, cb_ps_state, cb_copy;
  ComPtr<ID3D11Buffer> index_ring;
  uint32_t index_ring_size = 16 << 20, index_ring_pos = 0;

  std::unordered_map<uint32_t, std::shared_ptr<ShaderEntry>> shaders;  // by guest object
  std::unordered_map<uint64_t, std::shared_ptr<ShaderEntry>> shaders_by_hash;
  std::unordered_map<uint64_t, RenderTarget> render_targets;
  std::unordered_map<uint64_t, TextureEntry> textures;
  std::unordered_map<uint64_t, ComPtr<ID3D11SamplerState>> samplers;
  std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> blend_states;
  std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> depth_states;
  std::unordered_map<uint64_t, ComPtr<ID3D11RasterizerState>> raster_states;

  ComPtr<ID3D11VertexShader> vs_fullscreen;
  ComPtr<ID3D11PixelShader> ps_copy, ps_copy_ms, ps_copy_depth, ps_copy_depth_ms;
  ComPtr<ID3D11GeometryShader> gs_rect;
  ComPtr<ID3D11ShaderResourceView> dummy_2d, dummy_3d, dummy_cube, dummy_array;
  ComPtr<ID3D11SamplerState> point_sampler;

  uint32_t front_fetch[6] = {};
  bool have_front = false;
  std::unordered_set<uint64_t> warned;

  // Frame pacing statistics.
  std::atomic<double> fps{0.0};
  std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
  uint32_t window_frames = 0;
  uint64_t frames = 0;

  // Diagnostics, off by default (environment): FS3_GFX_LOG=1 logs per-second counters,
  // FS3_DUMP_EVERY=<n> writes every n-th presented frame to fs3-frame-<n>.bmp next to the exe.
  bool log_stats = false;
  uint32_t dump_every = 0;
  bool dump_hlsl = false;
  bool trace_dumps = false;
  bool debug_nodepth = false;  // FS3_DEBUG_NODEPTH=1: depth/stencil tests off
  uint32_t debug_ps = 0;  // FS3_DEBUG_PS=1: draws output magenta; =2: texture 0 at v[0] (sampling check)
  ComPtr<ID3D11PixelShader> ps_magenta;  // FS3_TRACE_DUMPS=1: also write render target 0 after each traced draw        // FS3_DUMP_HLSL=1: write every translated shader next to the exe
  uint64_t trace_frame = ~0ull;  // FS3_TRACE_FRAME=<n>: log every operation of frame n, dump its resolves
  double trace_at = -1.0;
  uint64_t trace_vs = 0;  // FS3_TRACE_VS=<hex hash>: trace the frame after the first draw with this vertex shader
  uint32_t trace_vs_draws = 0;        // FS3_TRACE_AT=<s>: trace the first frame after s seconds instead
  std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
  uint32_t trace_index = 0;
  bool tracing() const { return frames == trace_frame; }
  struct Stats {
    uint32_t draws = 0, submitted = 0, clears = 0, resolves = 0, presents = 0, exports = 0, export_ends = 0;
  } stats;
  std::chrono::steady_clock::time_point stats_start = std::chrono::steady_clock::now();

  void WarnOnce(uint64_t key, const char* what, uint32_t value) {
    if (warned.insert(key).second) REXLOG_WARN("[native-gfx] {} {:08X}", what, value);
  }

  static std::string Format(const char* fmt, unsigned long long a, unsigned long long b = 0) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), fmt, a, b);
    return buf;
  }
  static void DumpText(const std::string& path, const std::string& text) {
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
      std::fwrite(text.data(), 1, text.size(), fp);
      std::fclose(fp);
    }
  }

  // --- compile ---
  ComPtr<ID3DBlob> Compile(const std::string& src, const char* entry, const char* target, std::string* err) {
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = CompileGuarded(compile, src, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, &code, &errors);
    if (FAILED(hr)) {
      code.Reset();
      errors.Reset();
      // FXC's optimizer fails some looped shaders (X4000); unoptimized output is correct.
      hr = CompileGuarded(compile, src, entry, target, D3DCOMPILE_SKIP_OPTIMIZATION, &code, &errors);
    }
    if (FAILED(hr)) {
      if (err && errors) *err = static_cast<const char*>(errors->GetBufferPointer());
      return nullptr;
    }
    return code;
  }

  bool Init() {
    if (const char* v = std::getenv("FS3_GFX_LOG")) log_stats = *v && *v != '0';
    if (const char* v = std::getenv("FS3_DUMP_EVERY")) dump_every = uint32_t(std::strtoul(v, nullptr, 10));
    if (const char* v = std::getenv("FS3_TRACE_FRAME")) trace_frame = std::strtoull(v, nullptr, 10);
    if (const char* v = std::getenv("FS3_TRACE_AT")) trace_at = std::strtod(v, nullptr);
    if (const char* v = std::getenv("FS3_TRACE_VS")) trace_vs = std::strtoull(v, nullptr, 16);
    if (const char* v = std::getenv("FS3_DUMP_HLSL")) dump_hlsl = *v && *v != '0';
    if (const char* v = std::getenv("FS3_TRACE_DUMPS")) trace_dumps = *v && *v != '0';
    if (const char* v = std::getenv("FS3_DEBUG_NODEPTH")) debug_nodepth = *v && *v != '0';
    if (const char* v = std::getenv("FS3_DEBUG_PS")) debug_ps = uint32_t(std::strtoul(v, nullptr, 10));
    HMODULE fxc = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!fxc) return false;
    compile = reinterpret_cast<pD3DCompile>(GetProcAddress(fxc, "D3DCompile"));
    if (!compile) return false;
    // PC settings: graphics card chosen in the settings menu (null = Windows default).
    pcs::Configure(PcGameInfo(), false);
    IDXGIAdapter1* adapter = pcs::Adapter();
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1};
    ComPtr<ID3D11Device> d;
    ComPtr<ID3D11DeviceContext> c;
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   0, levels, 1, D3D11_SDK_VERSION, &d, nullptr, &c);
    if (adapter) adapter->Release();
    scale = pcs::ResolutionScale();
    if (scale != 1.0f) REXLOG_INFO("[native-gfx] internal resolution x{:.3f}", scale);
    if (FAILED(hr)) {
      REXLOG_ERROR("[native-gfx] D3D11CreateDevice failed {:08X}", uint32_t(hr));
      return false;
    }
    d.As(&dev);
    c.As(&ctx);
    if (!dev || !ctx) return false;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = kPhysicalSize;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &mem_buf))) {
      REXLOG_ERROR("[native-gfx] unable to create the 512 MiB guest memory buffer");
      return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = kPhysicalSize / 4;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    dev->CreateShaderResourceView(mem_buf.Get(), &sd, &mem_srv);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = kPhysicalSize / 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    dev->CreateUnorderedAccessView(mem_buf.Get(), &ud, &mem_uav);
    write_gen = std::vector<std::atomic<uint32_t>>(kPageCount);
    for (auto& g : write_gen) g.store(1, std::memory_order_relaxed);
    mirror_gen.assign(kPageCount, 0);

    auto make_cb = [&](uint32_t size, ComPtr<ID3D11Buffer>& out) {
      D3D11_BUFFER_DESC d = {};
      d.ByteWidth = (size + 15) & ~15u;
      d.Usage = D3D11_USAGE_DYNAMIC;
      d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      dev->CreateBuffer(&d, nullptr, &out);
    };
    make_cb(4096, cb_vs_float);
    make_cb(4096, cb_ps_float);
    make_cb(sizeof(XeState), cb_vs_state);
    make_cb(sizeof(XeState), cb_ps_state);
    make_cb(48, cb_copy);
    D3D11_BUFFER_DESC ib = {};
    ib.ByteWidth = index_ring_size;
    ib.Usage = D3D11_USAGE_DYNAMIC;
    ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ib.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&ib, nullptr, &index_ring);

    std::string err;
    std::string util = kUtilityHlsl;
    auto vsb = Compile(util, "vs_fullscreen", "vs_5_0", &err);
    auto psb = Compile(util, "ps_copy", "ps_5_0", &err);
    auto psmb = Compile(util, "ps_copy_ms", "ps_5_0", &err);
    auto psdb = Compile(util, "ps_copy_depth", "ps_5_0", &err);
    auto psdmb = Compile(util, "ps_copy_depth_ms", "ps_5_0", &err);
    auto gsb = Compile(kRectGsHlsl, "main", "gs_5_0", &err);
    if (!vsb || !psb || !psmb || !psdb || !psdmb || !gsb) {
      REXLOG_ERROR("[native-gfx] utility shader compile failed: {}", err);
      return false;
    }
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_fullscreen);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_copy);
    dev->CreatePixelShader(psmb->GetBufferPointer(), psmb->GetBufferSize(), nullptr, &ps_copy_ms);
    dev->CreatePixelShader(psdb->GetBufferPointer(), psdb->GetBufferSize(), nullptr, &ps_copy_depth);
    dev->CreatePixelShader(psdmb->GetBufferPointer(), psdmb->GetBufferSize(), nullptr, &ps_copy_depth_ms);
    dev->CreateGeometryShader(gsb->GetBufferPointer(), gsb->GetBufferSize(), nullptr, &gs_rect);
    if (debug_ps) {
      std::string src = "#define DEBUG " + std::to_string(debug_ps) + "\n";
      if (debug_ps == 1) {
        src += "float4 main(float4 p : SV_Position) : SV_Target { return float4(1.0, 0.0, 1.0, 1.0); }";
      } else {
        src += "struct I { float4 pos : SV_Position; float4 v[24] : TEXCOORD0; };\n"
               "Texture2D<float4> t : register(t1); SamplerState s : register(s0);\n"
               "float4 main(I i) : SV_Target {\n"
               "  if (DEBUG == 3) return float4(saturate(i.v[0].xy), isnan(i.v[0].x) ? 1.0 : 0.0, 1.0);\n"
               "  uint w, h, l; t.GetDimensions(0, w, h, l);\n"
               "  if (DEBUG == 4) return float4(w / 1280.0, h / 720.0, l / 8.0, 1.0);\n"
               "  if (DEBUG == 5) return float4(t.Load(int3(i.v[0].xy * float2(w, h), 0)).rgb, 1.0);\n"
               "  return float4(t.Sample(s, i.v[0].xy).rgb, 1.0);\n}\n";
      }
      auto mb = Compile(src, "main", "ps_5_0", &err);
      if (mb) dev->CreatePixelShader(mb->GetBufferPointer(), mb->GetBufferSize(), nullptr, &ps_magenta);
    }

    // Dummy textures for bindings whose texture can't be created.
    uint32_t white[6] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    D3D11_SUBRESOURCE_DATA init[6];
    for (auto& i : init) i = {white, 4, 4};
    D3D11_TEXTURE2D_DESC td = {1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ComPtr<ID3D11Texture2D> t2;
    dev->CreateTexture2D(&td, init, &t2);
    dev->CreateShaderResourceView(t2.Get(), nullptr, &dummy_2d);
    D3D11_SHADER_RESOURCE_VIEW_DESC ad = {};
    ad.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ad.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    ad.Texture2DArray.MipLevels = 1;
    ad.Texture2DArray.ArraySize = 1;
    dev->CreateShaderResourceView(t2.Get(), &ad, &dummy_array);
    td.ArraySize = 6;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    ComPtr<ID3D11Texture2D> tc;
    dev->CreateTexture2D(&td, init, &tc);
    dev->CreateShaderResourceView(tc.Get(), nullptr, &dummy_cube);
    D3D11_TEXTURE3D_DESC t3d = {1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ComPtr<ID3D11Texture3D> t3;
    dev->CreateTexture3D(&t3d, init, &t3);
    dev->CreateShaderResourceView(t3.Get(), nullptr, &dummy_3d);
    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&smp, &point_sampler);
    ready = true;
    return true;
  }

  static std::pair<uint32_t, uint32_t> OnInvalidate(void* context, uint32_t start, uint32_t length, bool) {
    auto* self = static_cast<Impl*>(context);
    uint32_t first = start >> kPageShift;
    uint32_t last = std::min<uint64_t>((uint64_t(start) + length + (1u << kPageShift) - 1) >> kPageShift, kPageCount);
    for (uint32_t p = first; p < last; p++) self->write_gen[p].fetch_add(1, std::memory_order_relaxed);
    // Only the pages marked dirty here may be unwatched: (0, UINT32_MAX) lets the memory system
    // drop the watch of the whole heap, and later CPU writes elsewhere would go unnoticed.
    return {first << kPageShift, (last - first) << kPageShift};
  }

  void Watch(uint32_t start, uint32_t size) {
    if (memory && size) memory->EnablePhysicalMemoryAccessCallbacks(start, size, true, false);
  }

  uint64_t GenSum(uint32_t start, uint32_t size) const {
    uint64_t s = 0;
    uint32_t first = start >> kPageShift;
    uint32_t last = std::min<uint64_t>((uint64_t(start) + size + (1u << kPageShift) - 1) >> kPageShift, kPageCount);
    for (uint32_t p = first; p < last; p++) s += write_gen[p].load(std::memory_order_relaxed);
    return s;
  }

  // Makes the mirror of [start, start + size) current.
  void EnsureMirror(uint32_t start, uint32_t size) {
    if (!size || start >= kPhysicalSize) return;
    size = std::min(size, kPhysicalSize - start);
    uint32_t first = start >> kPageShift;
    uint32_t last = (start + size + (1u << kPageShift) - 1) >> kPageShift;
    uint32_t p = first;
    while (p < last) {
      if (mirror_gen[p] == write_gen[p].load(std::memory_order_relaxed)) {
        p++;
        continue;
      }
      uint32_t run = p;
      while (run < last && mirror_gen[run] != write_gen[run].load(std::memory_order_relaxed)) {
        mirror_gen[run] = write_gen[run].load(std::memory_order_relaxed);
        run++;
      }
      uint32_t a = p << kPageShift, b = run << kPageShift;
      Watch(a, b - a);
      UploadMirror(a, b);
      p = run;
    }
  }

  // Uploads [a, b) of guest memory to the mirror, except bytes only the GPU has (memexport output):
  // export streams rarely start on a page boundary, and the CPU writing the rest of such a page
  // must not wipe the exported vertices.
  void UploadMirror(uint32_t a, uint32_t b) {
    auto upload = [&](uint32_t from, uint32_t to) {
      if (from >= to) return;
      D3D11_BOX box = {from, 0, 0, to, 1, 1};
      ctx->UpdateSubresource(mem_buf.Get(), 0, &box, phys + from, 0, 0);
    };
    auto it = exported.upper_bound(a);
    if (it != exported.begin()) --it;
    uint32_t cur = a;
    for (; it != exported.end() && it->first < b && cur < b; ++it) {
      uint32_t s = it->first, e = it->second.first;
      if (e <= cur) continue;
      upload(cur, std::min(s, b));
      cur = std::max(cur, e);
    }
    upload(cur, b);
  }

  // GPU-written ranges (memexport) are current in the mirror; record that.
  void MarkMirrorCurrent(uint32_t start, uint32_t size) {
    if (!size || start >= kPhysicalSize) return;
    size = std::min(size, kPhysicalSize - start);
    exported[start] = {start + size, frames};
    // Bring the CPU-written rest of the boundary pages up to date before calling them current.
    EnsureMirror(start, size);
    uint32_t first = start >> kPageShift;
    uint32_t last = std::min<uint32_t>((start + size + (1u << kPageShift) - 1) >> kPageShift, kPageCount);
    for (uint32_t p = first; p < last; p++) mirror_gen[p] = write_gen[p].load(std::memory_order_relaxed);
    Watch(first << kPageShift, (last - first) << kPageShift);
  }

  // --- shaders ---
  void RegisterShader(uint8_t* base, uint32_t object, uint32_t container) {
    const uint8_t* c = G(base, container);
    uint32_t size = Be32(c + 4) + Be32(c + 8);
    if ((Be32(c) & 0xFFFFFF00u) != 0x102A1100u || size > (8u << 20)) {
      WarnOnce(0x100000000ull | container, "unrecognized shader container at", container);
      return;
    }
    auto entry = std::make_shared<ShaderEntry>();
    std::string err;
    if (!ParseShaderContainer(c, size, entry->parsed, &err)) {
      REXLOG_WARN("[native-gfx] shader {:08X}: {}", container, err);
      return;
    }
    auto it = shaders_by_hash.find(entry->parsed.hash);
    if (it != shaders_by_hash.end()) entry = it->second;
    else shaders_by_hash[entry->parsed.hash] = entry;
    shaders[object] = entry;
  }

  ShaderEntry::PsVariant* GetPixelShader(ShaderEntry& e, uint32_t param_gen) {
    auto& v = e.ps[param_gen];
    if (v.ps || v.failed) return v.ps ? &v : nullptr;
    std::string err;
    ParsedShader parsed = e.parsed;
    parsed.ps_param_gen_reg = param_gen;
    if (!TranslateShader(parsed, nullptr, v.info, &err)) {
      v.failed = true;
      return nullptr;
    }
    if (dump_hlsl) DumpText(Format("fs3-%016llX-%02llX.ps.hlsl", e.parsed.hash, param_gen), v.info.hlsl);
    auto code = Compile(v.info.hlsl, "main", "ps_5_0", &err);
    if (!code || FAILED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &v.ps))) {
      REXLOG_ERROR("[native-gfx] pixel shader {:016X} failed: {}", e.parsed.hash, err);
      v.failed = true;
      return nullptr;
    }
    return &v;
  }

  ShaderEntry::VsVariant* GetVertexShader(ShaderEntry& e, const std::vector<VertexInputElement>& inputs,
                                          uint64_t decl_hash) {
    auto& v = e.vs[decl_hash];
    if (v.vs || v.failed) return v.vs ? &v : nullptr;
    std::string err;
    if (!TranslateShader(e.parsed, &inputs, v.info, &err)) {
      v.failed = true;
      return nullptr;
    }
    if (dump_hlsl) DumpText(Format("fs3-%016llX-%016llX.vs.hlsl", e.parsed.hash, decl_hash), v.info.hlsl);
    auto code = Compile(v.info.hlsl, "main", "vs_5_0", &err);
    if (!code || FAILED(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &v.vs))) {
      REXLOG_ERROR("[native-gfx] vertex shader {:016X} failed: {}", e.parsed.hash, err);
      v.failed = true;
      return nullptr;
    }
    return &v;
  }

  // --- render targets ---
  RenderTarget* GetRenderTarget(uint32_t info, uint32_t surface_info, uint32_t width, uint32_t height, bool depth) {
    uint32_t base = info & 0xFFF;
    uint32_t format = depth ? (info >> 16) & 1 : (info >> 16) & 0xF;
    // Formats with the same storage share the EDRAM target (games switch between them at one
    // base, e.g. 3 and 12): 0/1 (gamma is a view), 2/10, 3/12.
    if (!depth) format = format == 1 ? 0 : format == 10 ? 2 : format == 12 ? 3 : format;
    uint32_t pitch = surface_info & 0x3FFF;
    uint32_t msaa = (surface_info >> 16) & 3;
    uint32_t samples = msaa == 0 ? 1 : (msaa == 1 ? 2 : 4);
    width = std::max({width, pitch, 1u});
    height = std::max(height, 1u);
    uint64_t key = uint64_t(base) | (uint64_t(format) << 12) | (uint64_t(pitch) << 16) | (uint64_t(msaa) << 30) |
                   (uint64_t(depth) << 32);
    auto& rt = render_targets[key];
    if (rt.tex && rt.width >= width && rt.height >= height) return &rt;
    RenderTarget n;
    n.width = std::max(width, rt.width);
    n.height = std::max(height, rt.height);
    n.samples = samples;
    n.depth = depth;
    D3D11_TEXTURE2D_DESC td = {};
    n.scale = ScaleFor(n.width);
    td.Width = ScaledSize(n.width, n.scale);
    td.Height = ScaledSize(n.height, n.scale);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc = {samples, 0};
    td.Usage = D3D11_USAGE_DEFAULT;
    if (depth) {
      n.format = format ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
      td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    } else {
      n.format = ColorRtFormat(format);
      td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    }
    td.Format = n.format == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_TYPELESS : n.format;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &n.tex))) return nullptr;
    if (depth) {
      D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
      dd.Format = format ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
      dd.ViewDimension = samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
      dev->CreateDepthStencilView(n.tex.Get(), &dd, &n.dsv);
      D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = format ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      sd.ViewDimension = samples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MipLevels = 1;
      dev->CreateShaderResourceView(n.tex.Get(), &sd, &n.srv);
      ctx->ClearDepthStencilView(n.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    } else {
      D3D11_RENDER_TARGET_VIEW_DESC rd = {};
      rd.Format = n.format;
      rd.ViewDimension = samples > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
      dev->CreateRenderTargetView(n.tex.Get(), &rd, &n.rtv);
      if (n.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        dev->CreateRenderTargetView(n.tex.Get(), &rd, &n.rtv_gamma);
      }
      D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = n.format;
      sd.ViewDimension = samples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MipLevels = 1;
      dev->CreateShaderResourceView(n.tex.Get(), &sd, &n.srv);
      float zero[4] = {0, 0, 0, 0};
      ctx->ClearRenderTargetView(n.rtv.Get(), zero);
    }
    if (rt.tex && rt.samples == n.samples && rt.scale == n.scale) {
      D3D11_BOX box = {0, 0, 0, ScaledSize(rt.width, rt.scale), ScaledSize(rt.height, rt.scale), 1};
      ctx->CopySubresourceRegion(n.tex.Get(), 0, 0, 0, 0, rt.tex.Get(), 0, &box);
    }
    rt = std::move(n);
    return &rt;
  }

  // Logical size of a guest D3D surface object.
  static void SurfaceSize(uint8_t* base, uint32_t surface, uint32_t& w, uint32_t& h) {
    uint32_t v = Be32(G(base, surface + 0x24));
    w = (v >> 18) + 1;
    h = ((v >> 3) & 0x7FFF) + 1;
  }

  // --- textures ---
  static uint64_t TextureKey(const TexFetch& f) {
    // Everything that changes the layout or interpretation of the guest data (not the sampler).
    uint32_t k[5] = {f.d[0] & 0xFFC00003u, f.d[1] & 0xFFFFFFFFu, f.d[2], f.d[4] & 0x3C0u, f.d[5] & 0xFFFFFE00u};
    return HashBytes(k, sizeof(k));
  }

  // `target_scale`: for a resolve destination, the internal resolution of the source target.
  TextureEntry* GetTexture(const TexFetch& f, bool for_render, float target_scale = 1.0f) {
    DXGI_FORMAT format = HostTextureFormat(f.format(), false);
    if (format == DXGI_FORMAT_UNKNOWN || !f.base_address()) return nullptr;
    uint64_t key = TextureKey(f);
    auto& e = textures[key];
    bool renderable = !IsBlockCompressed(format) && f.dimension() != 2;
    float want = for_render && renderable && f.dimension() != 3 ? target_scale : 1.0f;
    if (e.tex && !(for_render && !e.renderable && renderable) && !(for_render && e.scale != want)) return &e;
    e = TextureEntry{};
    e.fetch = f;
    e.format = format;
    e.format_signed = HostTextureFormat(f.format(), true);
    e.width = f.width();
    e.height = f.height();
    e.depth = f.depth();
    e.is_3d = f.dimension() == 2;
    e.is_cube = f.dimension() == 3;
    e.renderable = renderable;
    e.scale = want;
    uint32_t longest = std::max({e.width, e.height, e.is_3d ? e.depth : 1u});
    uint32_t max_levels = 1;
    while ((1u << max_levels) <= longest) max_levels++;
    e.levels = f.mip_address() ? std::min(f.mip_max_level() + 1, max_levels) : 1;
    TextureMemoryRanges(f, e.base, e.base_bytes, e.mip, e.mip_bytes);
    DXGI_FORMAT storage = e.format_signed != e.format ? Typeless(e.format) : e.format;
    uint32_t w = e.width, h = e.height;
    if (IsBlockCompressed(format)) {
      w = (w + 3) & ~3u;
      h = (h + 3) & ~3u;
    }
    if (e.is_3d) {
      D3D11_TEXTURE3D_DESC td = {w, h, e.depth, e.levels, storage, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
      ComPtr<ID3D11Texture3D> t;
      if (FAILED(dev->CreateTexture3D(&td, nullptr, &t))) {
        textures.erase(key);
        return nullptr;
      }
      e.tex = t;
    } else {
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = ScaledSize(w, e.scale);
      td.Height = ScaledSize(h, e.scale);
      td.MipLevels = e.levels;
      td.ArraySize = e.depth;
      td.Format = storage;
      td.SampleDesc = {1, 0};
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (renderable ? D3D11_BIND_RENDER_TARGET : 0);
      td.MiscFlags = e.is_cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
      // Resolves write the top level only; the rest of a resolved chain is regenerated.
      if (renderable && e.levels > 1) td.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
      ComPtr<ID3D11Texture2D> t;
      if (FAILED(dev->CreateTexture2D(&td, nullptr, &t))) {
        td.MiscFlags &= ~UINT(D3D11_RESOURCE_MISC_GENERATE_MIPS);
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &t))) {
          textures.erase(key);
          return nullptr;
        }
      }
      e.can_generate_mips = (td.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) != 0;
      e.tex = t;
    }
    auto make_views = [&](DXGI_FORMAT fmt, ComPtr<ID3D11ShaderResourceView>& natural,
                          ComPtr<ID3D11ShaderResourceView>& array) {
      D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = fmt;
      if (e.is_3d) {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D.MipLevels = UINT(-1);
      } else if (e.is_cube) {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MipLevels = UINT(-1);
      } else {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;  // a 2D fetch of a stack reads layer 0
        sd.Texture2D.MipLevels = UINT(-1);
      }
      dev->CreateShaderResourceView(e.tex.Get(), &sd, &natural);
      if (!e.is_3d && !e.is_cube) {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MostDetailedMip = 0;
        sd.Texture2DArray.MipLevels = UINT(-1);
        sd.Texture2DArray.FirstArraySlice = 0;
        sd.Texture2DArray.ArraySize = e.depth;
        dev->CreateShaderResourceView(e.tex.Get(), &sd, &array);
      }
    };
    make_views(e.format, e.srv, e.srv_array);
    if (e.format_signed != e.format) make_views(e.format_signed, e.srv_signed, e.srv_array_signed);
    e.gen_sum = ~0ull;
    return &e;
  }

  uint64_t EntryGenSum(const TextureEntry& e) const {
    return GenSum(e.base, e.base_bytes) + (e.mip_bytes ? GenSum(e.mip, e.mip_bytes) : 0);
  }

  // Uploads every level from guest memory if the CPU changed it since the last upload/resolve.
  void RefreshTexture(TextureEntry& e) {
    uint64_t sum = EntryGenSum(e);
    if (sum == e.gen_sum) return;
    if (e.scale != 1.0f) {
      // The CPU rewrote a resolve destination: back to a guest-size texture for its data (the
      // next resolve there scales it again). `e` is the same map slot, rebuilt in place.
      TexFetch f = e.fetch;
      e.tex.Reset();
      if (!GetTexture(f, false) || !e.tex) return;
    }
    e.gen_sum = sum;
    Watch(e.base, e.base_bytes);
    if (e.mip_bytes) Watch(e.mip, e.mip_bytes);
    HostTexture host;
    if (!DecodeTexture(phys, e.fetch, host)) {
      WarnOnce(0x800000000ull | e.fetch.format(), "texture decode failed, format", e.fetch.format());
      return;
    }
    uint32_t layers = e.is_3d ? 1 : e.depth;
    uint32_t levels = std::min(e.levels, host.levels);
    for (uint32_t l = 0; l < layers; l++) {
      for (uint32_t m = 0; m < levels; m++) {
        const HostTexture::Sub& sub = host.subs[size_t(l) * host.levels + m];
        if (sub.data.empty()) continue;
        ctx->UpdateSubresource(e.tex.Get(), D3D11CalcSubresource(m, l, e.levels), nullptr, sub.data.data(),
                               sub.row_pitch, sub.slice_pitch);
      }
    }
  }

  ID3D11SamplerState* GetSampler(const uint32_t f[6]) {
    uint32_t key32[3] = {f[0] & 0x7FFC00, f[3] & 0xFFF80000, f[4] & 0x3FF003FC};
    // PC settings: anisotropic filtering forced on filtered (linear mip) fetches.
    int forced = pcs::Anisotropy();
    uint64_t key = HashBytes(key32, sizeof(key32)) ^ (f[5] & 3) ^ (uint64_t(forced) << 56);
    auto& s = samplers[key];
    if (s) return s.Get();
    D3D11_SAMPLER_DESC d = {};
    uint32_t mag = (f[3] >> 19) & 3, min = (f[3] >> 21) & 3, mip = (f[3] >> 23) & 3, aniso = (f[3] >> 25) & 7;
    bool lmag = mag == 1, lmin = min == 1, lmip = mip == 1;
    if (forced > 1 && lmag && lmin && lmip)
      aniso = std::max<uint32_t>(aniso == 7 ? 0 : aniso, uint32_t(std::countr_zero(unsigned(forced))) + 1);
    if (aniso > 1) {
      d.Filter = D3D11_FILTER_ANISOTROPIC;
      d.MaxAnisotropy = std::min(16u, 1u << (aniso - 1));
    } else {
      d.Filter = D3D11_ENCODE_BASIC_FILTER(lmin ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                           lmag ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                           lmip ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                           D3D11_FILTER_REDUCTION_TYPE_STANDARD);
      d.MaxAnisotropy = 1;
    }
    d.AddressU = AddressMode((f[0] >> 10) & 7);
    d.AddressV = AddressMode((f[0] >> 13) & 7);
    d.AddressW = AddressMode((f[0] >> 16) & 7);
    int32_t bias = int32_t(f[4] << 10) >> 22;
    d.MipLODBias = float(bias) / 32.0f;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    float bc = (f[5] & 3) == 1 ? 1.0f : 0.0f;
    d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = bc;
    d.MinLOD = float((f[4] >> 2) & 0xF);
    d.MaxLOD = mip == 2 ? d.MinLOD : float((f[4] >> 6) & 0xF);
    if (d.MaxLOD < d.MinLOD) d.MaxLOD = d.MinLOD;
    dev->CreateSamplerState(&d, &s);
    return s.Get();
  }

  static TexFetch ReadFetch(const Dev& d, uint32_t slot) {
    TexFetch f;
    for (int i = 0; i < 6; i++) f.d[i] = d.u32(0x480 + slot * 24 + i * 4);
    NormalizeFetch(f.d);
    return f;
  }

  void FillTexState(XeState& st, const Dev& d) {
    for (uint32_t s = 0; s < 32; s++) {
      TexFetch f = ReadFetch(d, s);
      st.tex[s][0] = f.swizzle() | (((f.d[0] >> 2) & 0xFF) << 12);
      int32_t exp = int32_t(f.d[3] << 13) >> 26;
      float scale = std::ldexp(1.0f, exp);
      std::memcpy(&st.tex[s][1], &scale, 4);
      // A 3D fetch reads a real volume or a stack of 2D layers; the shader picks the view.
      st.tex[s][2] = f.dimension() == 2 ? 0u : 1u;
      st.tex_dim[s][0] = float(f.width());
      st.tex_dim[s][1] = float(f.height());
      st.tex_dim[s][2] = float(f.depth());
      st.tex_dim[s][3] = 1.0f;
    }
  }

  // Texture binding i -> t(1 + i); for 3D fetches also the stacked view at t(kStackedBase + i).
  static constexpr uint32_t kMaxBindings = 15;
  static constexpr uint32_t kStackedBase = kStackedTextureBase;

  void BindTextures(const Dev& d, const std::vector<TextureBinding>& bindings, bool pixel) {
    ID3D11ShaderResourceView* srvs[kStackedBase - 1 + kMaxBindings] = {};
    ID3D11SamplerState* smps[kMaxBindings] = {};
    size_t n = std::min<size_t>(bindings.size(), kMaxBindings);
    for (size_t i = 0; i < n; i++) {
      uint32_t dim = bindings[i].dimension;
      TexFetch f = ReadFetch(d, bindings[i].fetch_slot);
      ID3D11ShaderResourceView*& natural = srvs[i];
      ID3D11ShaderResourceView** stacked = dim == 2 ? &srvs[kStackedBase - 1 + i] : nullptr;
      natural = dim == 3 ? dummy_cube.Get() : dim == 2 ? dummy_3d.Get() : dummy_2d.Get();
      if (stacked) *stacked = dummy_array.Get();
      smps[i] = GetSampler(f.d);
      if (f.type() != 2) continue;  // not a valid texture fetch constant
      bool all_signed = ((f.d[0] >> 2) & 0xFF) == 0x55;
      // Instruction dimension vs. texture: 3D fetches take volumes and stacks, cube fetches cubes,
      // 1D/2D fetches 1D/2D textures (layer 0 of a stack).
      bool fits = dim == 3 ? f.dimension() == 3 : dim == 2 ? f.dimension() == 2 || f.dimension() == 1
                                                           : f.dimension() <= 1;
      if (!fits) continue;
      TextureEntry* e = GetTexture(f, false);
      if (!e) {
        WarnOnce(0x200000000ull | f.format(), "unsupported texture format", f.format());
        continue;
      }
      RefreshTexture(*e);
      bool use_signed = all_signed && e->srv_signed;
      if (dim == 2 && !e->is_3d) {
        *stacked = use_signed ? e->srv_array_signed.Get() : e->srv_array.Get();
      } else {
        natural = use_signed ? e->srv_signed.Get() : e->srv.Get();
      }
    }
    if (pixel) {
      ctx->PSSetShaderResources(1, UINT(std::size(srvs)), srvs);
      ctx->PSSetSamplers(0, kMaxBindings, smps);
    } else {
      ctx->VSSetShaderResources(1, UINT(std::size(srvs)), srvs);
      ctx->VSSetSamplers(0, kMaxBindings, smps);
    }
  }

  void Upload(ID3D11Buffer* b, const void* data, size_t size) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      std::memcpy(m.pData, data, size);
      ctx->Unmap(b, 0);
    }
  }

  void UploadFloatConstants(ID3D11Buffer* cb, const Dev& d, uint32_t offset, const ParsedShader& s) {
    uint32_t c[1024];
    const uint8_t* src = G(d.base, d.addr + offset);
    for (int i = 0; i < 1024; i++) c[i] = Be32(src + i * 4);
    for (auto& def : s.float_defs) std::memcpy(&c[def.index * 4], def.value, 16);
    Upload(cb, c, sizeof(c));
  }

  // --- render state ---
  struct TargetSetup {
    RenderTarget* color[4] = {};
    ID3D11RenderTargetView* color_rtv[4] = {};  // view matching the bound format
    RenderTarget* depth = nullptr;
    uint32_t width = 1, height = 1;
    uint32_t samples = 1;
    float rt_bias[4] = {1, 1, 1, 1};
    float scale = 1.0f;  // internal resolution of the bound targets
  };

  // With predicated tiling the bound surfaces are one EDRAM tile (e.g. 640x384) while draws and
  // resolves address the whole frame. Draws execute once here, so targets get the logical size:
  // at least the surface, the viewport registers and the device's viewport.
  static void LogicalSize(const Dev& d, uint32_t& w, uint32_t& h) {
    float ex = 0, ey = 0;
    uint32_t vte = d.reg(0x2206);
    if ((vte & 1) && (vte & 2)) ex = d.regf(0x2110) + std::fabs(d.regf(0x210F));
    if ((vte & 4) && (vte & 8)) ey = d.regf(0x2112) + std::fabs(d.regf(0x2111));
    ex = std::max(ex, d.f32(0x3160) + d.f32(0x3168));
    ey = std::max(ey, d.f32(0x3164) + d.f32(0x316C));
    if (ex > 0 && ex <= 8192) w = std::max(w, uint32_t(std::ceil(ex)));
    if (ey > 0 && ey <= 8192) h = std::max(h, uint32_t(std::ceil(ey)));
  }

  void SetupTargets(const Dev& d, TargetSetup& t, bool depth_only) {
    uint32_t surface_info = d.reg(0x2000);
    for (uint32_t i = 0; i < 4; i++) {
      uint32_t surf = d.u32(0x3090 + i * 4);
      if (!surf || depth_only) continue;
      uint32_t info = d.reg(i == 0 ? 0x2001 : 0x2002 + i);
      uint32_t w, h;
      SurfaceSize(d.base, surf, w, h);
      LogicalSize(d, w, h);
      t.color[i] = GetRenderTarget(info, surface_info, w, h, false);
      if (t.color[i])
        t.color_rtv[i] = ((info >> 16) & 0xF) == 1 && t.color[i]->rtv_gamma ? t.color[i]->rtv_gamma.Get()
                                                                            : t.color[i]->rtv.Get();
      int32_t bias = int32_t(info << 6) >> 26;
      t.rt_bias[i] = std::ldexp(1.0f, bias);
      uint32_t fmt = (info >> 16) & 0xF;
      if (fmt == 4 || fmt == 5) t.rt_bias[i] *= 1.0f / 32.0f;  // fixed-point -32..32 stored as SNORM
      if (i == 0) {
        t.width = w;
        t.height = h;
      }
    }
    uint32_t ds = d.u32(0x30A0);
    if (ds) {
      uint32_t w, h;
      SurfaceSize(d.base, ds, w, h);
      LogicalSize(d, w, h);
      t.depth = GetRenderTarget(d.reg(0x2002), surface_info, w, h, true);
      if (!t.color[0]) {
        t.width = w;
        t.height = h;
      }
    }
    // A render target and depth buffer bound together must have the same sample count and size.
    uint32_t samples = 0;
    for (auto* c : t.color)
      if (c) samples = c->samples;
    if (t.depth && samples && t.depth->samples != samples) t.depth = nullptr;
    t.samples = samples ? samples : (t.depth ? t.depth->samples : 1);
    ID3D11RenderTargetView* rtvs[4] = {};
    uint32_t minw = UINT32_MAX, minh = UINT32_MAX;
    for (int i = 0; i < 4; i++) {
      if (!t.color[i]) continue;
      rtvs[i] = t.color_rtv[i];
      minw = std::min(minw, t.color[i]->width);
      minh = std::min(minh, t.color[i]->height);
    }
    if (t.depth && minw != UINT32_MAX && (t.depth->width < minw || t.depth->height < minh)) {
      // Grow the depth buffer to cover the color targets.
      uint32_t dsurf = d.u32(0x30A0);
      (void)dsurf;
      t.depth = GetRenderTarget(d.reg(0x2002), surface_info, minw, minh, true);
    }
    t.scale = t.depth ? t.depth->scale : 1.0f;
    for (int i = 3; i >= 0; i--)
      if (t.color[i]) t.scale = t.color[i]->scale;
    ctx->OMSetRenderTargets(4, rtvs, t.depth ? t.depth->dsv.Get() : nullptr);
  }

  void ApplyStates(const Dev& d, const TargetSetup& t, bool memexport_only) {
    // Blend.
    D3D11_BLEND_DESC bd = {};
    bd.IndependentBlendEnable = TRUE;
    // RB_COLORCONTROL.ALPHA_TO_MASK_ENABLE (only meaningful on multisampled targets).
    bd.AlphaToCoverageEnable = ((d.reg(0x2202) >> 4) & 1) && t.samples > 1;
    uint32_t mask = d.reg(0x2104);
    const uint32_t blend_regs[4] = {0x2201, 0x2209, 0x220A, 0x220B};
    for (int i = 0; i < 4; i++) {
      uint32_t b = d.reg(blend_regs[i]);
      auto& rt = bd.RenderTarget[i];
      rt.SrcBlend = BlendFactor(b & 31, false);
      rt.BlendOp = BlendOp((b >> 5) & 7);
      rt.DestBlend = BlendFactor((b >> 8) & 31, false);
      rt.SrcBlendAlpha = BlendFactor((b >> 16) & 31, true);
      rt.BlendOpAlpha = BlendOp((b >> 21) & 7);
      rt.DestBlendAlpha = BlendFactor((b >> 24) & 31, true);
      rt.BlendEnable = !(rt.SrcBlend == D3D11_BLEND_ONE && rt.DestBlend == D3D11_BLEND_ZERO &&
                         rt.BlendOp == D3D11_BLEND_OP_ADD && rt.SrcBlendAlpha == D3D11_BLEND_ONE &&
                         rt.DestBlendAlpha == D3D11_BLEND_ZERO && rt.BlendOpAlpha == D3D11_BLEND_OP_ADD);
      rt.RenderTargetWriteMask = memexport_only ? 0 : uint8_t((mask >> (4 * i)) & 0xF);
    }
    uint64_t bkey = HashBytes(&bd, sizeof(bd));
    auto& bs = blend_states[bkey];
    if (!bs) dev->CreateBlendState(&bd, &bs);
    float factor[4] = {d.regf(0x2105), d.regf(0x2106), d.regf(0x2107), d.regf(0x2108)};
    ctx->OMSetBlendState(bs.Get(), factor, d.reg(0x2312) & 0xFFFF ? d.reg(0x2312) : 0xFFFFFFFF);

    // Depth / stencil.
    uint32_t dc = d.reg(0x2200);
    uint32_t sref = d.reg(0x210D);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = (dc >> 1) & 1;
    dd.DepthWriteMask = ((dc >> 2) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_FUNC(((dc >> 4) & 7) + 1);
    dd.StencilEnable = dc & 1;
    dd.StencilReadMask = uint8_t((sref >> 8) & 0xFF);
    dd.StencilWriteMask = uint8_t((sref >> 16) & 0xFF);
    dd.FrontFace.StencilFunc = D3D11_COMPARISON_FUNC(((dc >> 8) & 7) + 1);
    dd.FrontFace.StencilFailOp = D3D11_STENCIL_OP(((dc >> 11) & 7) + 1);
    dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP(((dc >> 14) & 7) + 1);
    dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP(((dc >> 17) & 7) + 1);
    if ((dc >> 7) & 1) {
      dd.BackFace.StencilFunc = D3D11_COMPARISON_FUNC(((dc >> 20) & 7) + 1);
      dd.BackFace.StencilFailOp = D3D11_STENCIL_OP(((dc >> 23) & 7) + 1);
      dd.BackFace.StencilPassOp = D3D11_STENCIL_OP(((dc >> 26) & 7) + 1);
      dd.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP(((dc >> 29) & 7) + 1);
    } else {
      dd.BackFace = dd.FrontFace;
    }
    if (!t.depth || debug_nodepth) {
      dd.DepthEnable = FALSE;
      dd.StencilEnable = FALSE;
    }
    uint64_t dkey = HashBytes(&dd, sizeof(dd));
    auto& ds = depth_states[dkey];
    if (!ds) dev->CreateDepthStencilState(&dd, &ds);
    ctx->OMSetDepthStencilState(ds.Get(), sref & 0xFF);

    // Rasterizer.
    uint32_t mode = d.reg(0x2205);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = ((mode >> 3) & 3) == 1 && ((mode >> 5) & 7) == 1 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    bool cf = mode & 1, cb = (mode >> 1) & 1;
    rd.CullMode = cf && cb ? D3D11_CULL_NONE : cf ? D3D11_CULL_FRONT : cb ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    rd.FrontCounterClockwise = ((mode >> 2) & 1) == 0;
    if ((mode >> 11) & 1) {
      float scale = d.regf(0x2380), offset = d.regf(0x2381);
      float units = std::fabs(offset) < 1.0f ? offset * 16777216.0f : offset;
      rd.DepthBias = int(units);
      rd.SlopeScaledDepthBias = scale * (1.0f / 16.0f);
    }
    rd.DepthClipEnable = ((d.reg(0x2204) >> 16) & 1) == 0;
    rd.ScissorEnable = TRUE;
    rd.MultisampleEnable = t.samples > 1;
    uint64_t rkey = HashBytes(&rd, sizeof(rd));
    auto& rs = raster_states[rkey];
    if (!rs) dev->CreateRasterizerState(&rd, &rs);
    ctx->RSSetState(rs.Get());
  }

  void ViewportAndNdc(const Dev& d, const TargetSetup& t, float ndc[4], float zxform[4]) {
    uint32_t vte = d.reg(0x2206);
    D3D11_VIEWPORT vp = {0, 0, float(t.width), float(t.height), 0.0f, 1.0f};
    ndc[0] = 1.0f;
    ndc[1] = 1.0f;
    ndc[2] = 0.0f;
    ndc[3] = 0.0f;
    float vw = float(t.width), vh = float(t.height);
    if (vte & 1) {
      float xs = d.regf(0x210F), xo = d.regf(0x2110), ys = d.regf(0x2111), yo = d.regf(0x2112);
      float zs = d.regf(0x2113), zo = d.regf(0x2114);
      vp.TopLeftX = xo - std::fabs(xs);
      vp.Width = 2.0f * std::fabs(xs);
      vp.TopLeftY = yo - std::fabs(ys);
      vp.Height = 2.0f * std::fabs(ys);
      if (xs < 0) ndc[0] = -1.0f;
      if (ys > 0) ndc[1] = -1.0f;
      // z = z * scale + offset is done by the vertex shader: games use inverted ranges (reverse
      // depth), which a D3D11 viewport can't express.
      zxform[0] = (vte & (1 << 4)) ? zs : 1.0f;
      zxform[1] = (vte & (1 << 5)) ? zo : 0.0f;
      vw = std::max(vp.Width, 1.0f);
      vh = std::max(vp.Height, 1.0f);
    } else {
      // Positions are already in screen space: map them to NDC of the full target.
      ndc[0] = 2.0f / vw;
      ndc[1] = -2.0f / vh;
      ndc[2] = -1.0f;
      ndc[3] = 1.0f;
    }
    // Direct3D 9 pixel centers (PA_SU_VTX_CNTL.pix_center == 0) are half a pixel up-left.
    if ((d.reg(0x2302) & 1) == 0) {
      ndc[2] -= 1.0f / vw;
      ndc[3] += 1.0f / vh;
    }
    vp.TopLeftX *= t.scale;
    vp.TopLeftY *= t.scale;
    vp.Width *= t.scale;
    vp.Height *= t.scale;
    ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc = {0, 0, LONG(t.width), LONG(t.height)};
    if (d.u32(0x2E48)) {
      sc.left = std::max<LONG>(sc.left, LONG(d.u32(0x317C)));
      sc.top = std::max<LONG>(sc.top, LONG(d.u32(0x3180)));
      sc.right = std::min<LONG>(sc.right, LONG(d.u32(0x3184)));
      sc.bottom = std::min<LONG>(sc.bottom, LONG(d.u32(0x3188)));
    }
    sc = ScaledRect(sc, t.scale);
    ctx->RSSetScissorRects(1, &sc);
  }

  void Draw(uint8_t* base, const DrawCall& call) {
    stats.draws++;
    Dev d{base, call.device};
    uint32_t vs_obj = d.u32(0x3190), ps_obj = d.u32(0x318C), decl = d.u32(0x2E24);
    if (!vs_obj || !call.count) return;
    auto vit = shaders.find(vs_obj);
    if (vit == shaders.end()) {
      WarnOnce(0x300000000ull | vs_obj, "draw with an unregistered vertex shader object", vs_obj);
      return;
    }
    ShaderEntry& vse = *vit->second;
    // Vertex declaration -> resolved inputs.
    std::vector<VertexInputElement> inputs;
    uint64_t decl_hash = 0;
    uint32_t stream_mask = 0;
    if (decl) {
      uint32_t count = std::min(Be32(G(base, decl + 0x18)), 64u);
      decl_hash = HashBytes(G(base, decl + 0x34), count * 12);
      for (auto& ve : vse.parsed.vertex_elements) {
        VertexInputElement in;
        in.fetch_address = ve.fetch_address;
        for (uint32_t i = 0; i < count; i++) {
          const uint8_t* e = G(base, decl + 0x34 + i * 12);
          if (e[9] == ve.usage && e[10] == ve.usage_index) {
            in.present = true;
            in.stream = Be16(e) & 15;
            in.offset_bytes = Be16(e + 2);
            in.decl_type = Be32(e + 4);
            stream_mask |= 1u << in.stream;
            break;
          }
        }
        inputs.push_back(in);
      }
    }
    if (trace_vs && vse.parsed.hash == trace_vs && ++trace_vs_draws >= 20 && trace_frame == ~0ull) {
      trace_frame = frames + 1;
      trace_vs = 0;
    }
    auto* vsv = GetVertexShader(vse, inputs, decl_hash);
    if (!vsv) return;
    ShaderEntry* pse = nullptr;
    if (ps_obj) {
      auto pit = shaders.find(ps_obj);
      if (pit != shaders.end()) pse = pit->second.get();
      else WarnOnce(0x400000000ull | ps_obj, "draw with an unregistered pixel shader object", ps_obj);
    }
    // Pixel parameter generation (position/face into a register): SQ_PROGRAM_CNTL.PARAM_GEN and
    // SQ_CONTEXT_MISC.PARAM_GEN_POS.
    uint32_t param_gen = (d.reg(0x2180) >> 18) & 1 ? (d.reg(0x2181) >> 8) & 0xFF : 0xFF;
    ShaderEntry::PsVariant* psv = pse ? GetPixelShader(*pse, param_gen) : nullptr;
    ID3D11PixelShader* ps = psv ? psv->ps.Get() : nullptr;
    bool memexport = vsv->info.uses_memexport;
    if (memexport) stats.exports++;
    uint32_t edram_mode = d.reg(0x2208) & 7;
    bool depth_only = edram_mode == 5;

    // Fetch constants; vertex fetch addresses are CPU virtual addresses like texture ones.
    uint32_t vfetch[192];
    for (int i = 0; i < 192; i++) vfetch[i] = d.u32(0x480 + i * 4);
    for (uint32_t k = 0; k < 96; k++) {
      if ((vfetch[k / 3 * 6] & 3) == 2) continue;  // part of a texture fetch constant
      uint32_t& w0 = vfetch[k * 2];
      if ((w0 & 3) == 3 && (w0 & ~3u) >= kPhysicalSize) w0 = (ToPhys(w0 & ~3u) & 0x1FFFFFFCu) | 3u;
    }
    // Guest memory used by the vertex streams.
    for (uint32_t s = 0; s < 16; s++) {
      if (!(stream_mask & (1u << s))) continue;
      uint32_t slot = 95 - s;
      EnsureMirror(vfetch[slot * 2] & 0x1FFFFFFC, vfetch[slot * 2 + 1] & 0x03FFFFFC);
    }

    TargetSetup t;
    SetupTargets(d, t, depth_only || (memexport && !ps));
    float ndc[4], zxform[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    ViewportAndNdc(d, t, ndc, zxform);
    ApplyStates(d, t, memexport && !ps);
    if (tracing()) {
      D3D11_VIEWPORT vp;
      UINT nvp = 1;
      ctx->RSGetViewports(&nvp, &vp);
      uint32_t s0 = 95;
      REXLOG_INFO("fs3trace: #{} draw prim {} n {} idx {} base {} start {} vs {:016X} ps {:016X} decl {} rt0 {}x{} f{} "
                  "ds {} vp {} {} {}x{} z {}-{} ndc {} {} {} {} vte {:X} mode {:X} dc {:X} blend {:08X} mask {:X} "
                  "cc {:X} edram {} vb {:08X}/{} st {} tex {}/{}",
                  trace_index++, call.primitive, call.count, call.indexed, call.base_vertex, call.start,
                  vse.parsed.hash, pse ? pse->parsed.hash : 0, inputs.size(), t.width, t.height,
                  (d.reg(0x2001) >> 16) & 0xF, t.depth ? 1 : 0, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height,
                  vp.MinDepth, vp.MaxDepth, ndc[0], ndc[1], ndc[2], ndc[3], d.reg(0x2206), d.reg(0x2205),
                  d.reg(0x2200), d.reg(0x2201), d.reg(0x2104), d.reg(0x2202), edram_mode, vfetch[s0 * 2],
                  (vfetch[s0 * 2 + 1] & 0x03FFFFFC), d.u8(0x30E8), vsv->info.textures.size(),
                  psv ? psv->info.textures.size() : 0);
      auto log_tex = [&](const std::vector<TextureBinding>& b, const char* stage) {
        for (auto& tb : b) {
          TexFetch f = ReadFetch(d, tb.fetch_slot);
          REXLOG_INFO("fs3trace:   {} slot {} dim {} -> type {} fmt {} {}x{}x{} fdim {} base {:08X} mip {:08X} tiled {} "
                      "swz {:03X} signs {:02X}",
                      stage, tb.fetch_slot, tb.dimension, f.type(), f.format(), f.width(), f.height(), f.depth(),
                      f.dimension(), f.base_address(), f.mip_address(), f.tiled(), f.swizzle(),
                      (f.d[0] >> 2) & 0xFF);
          if (trace_dumps) {
            if (TextureEntry* te = GetTexture(f, false)) {
              char path[64];
              std::snprintf(path, sizeof(path), "fs3-frame-tex%03u-%s.bmp", trace_index - 1, stage);
              DumpTexture(*te, path);
            }
          }
          const uint8_t* tp = phys + (f.base_address() & 0x1FFFFFFF);
          REXLOG_INFO("fs3trace:     data {:08X} {:08X} {:08X} {:08X} endian {} pitch {} d {:08X} {:08X} {:08X} {:08X} "
                      "{:08X} {:08X}",
                      Be32(tp), Be32(tp + 4), Be32(tp + 8), Be32(tp + 12), f.endian(), f.pitch_texels(), f.d[0],
                      f.d[1], f.d[2], f.d[3], f.d[4], f.d[5]);
        }
      };
      D3D11_RECT sc;
      UINT nsc = 1;
      ctx->RSGetScissorRects(&nsc, &sc);
      REXLOG_INFO("fs3trace:   zxform {} {} clip_cntl {:08X} zs {} zo {}", zxform[0], zxform[1], d.reg(0x2204),
                  d.regf(0x2113), d.regf(0x2114));
      REXLOG_INFO("fs3trace:   scissor {} {},{}-{},{} dev-vp {} {} {} {} rt0 host {}x{} samples {} ds host {}x{}",
                  d.u32(0x2E48), sc.left, sc.top, sc.right, sc.bottom, d.f32(0x3160), d.f32(0x3164), d.f32(0x3168),
                  d.f32(0x316C), t.color[0] ? t.color[0]->width : 0, t.color[0] ? t.color[0]->height : 0, t.samples,
                  t.depth ? t.depth->width : 0, t.depth ? t.depth->height : 0);
      for (uint32_t r = 0; r < 4; r++)
        REXLOG_INFO("fs3trace:   vc{} {} {} {} {}", r, d.f32(0x780 + r * 16), d.f32(0x784 + r * 16),
                    d.f32(0x788 + r * 16), d.f32(0x78C + r * 16));
      for (auto& def : vse.parsed.float_defs)
        REXLOG_INFO("fs3trace:   vs def c{} {} {} {} {}", def.index, std::bit_cast<float>(def.value[0]),
                    std::bit_cast<float>(def.value[1]), std::bit_cast<float>(def.value[2]),
                    std::bit_cast<float>(def.value[3]));
      if (call.count) {
        uint32_t vb = vfetch[95 * 2] & 0x1FFFFFFC;
        const uint8_t* v0 = phys + vb + (call.indexed ? 0 : call.start) * d.u8(0x30E8) * 4;
        uint32_t stride = d.u8(0x30E8) * 4;
        for (uint32_t k = 0; k < std::min(call.count, 6u); k++) {
          const uint8_t* vk = v0 + k * stride;
          REXLOG_INFO("fs3trace:   v{} {} {} {} | {:08X} {:08X} {:08X}", k, BeF(vk), BeF(vk + 4), BeF(vk + 8),
                      Be32(vk + 12), Be32(vk + 16), Be32(vk + 20));
        }
      }
      for (uint32_t sidx = 0; sidx < 16; sidx++) {
        if (!(stream_mask & (1u << sidx))) continue;
        uint32_t vb = vfetch[(95 - sidx) * 2] & 0x1FFFFFFC, vsize = vfetch[(95 - sidx) * 2 + 1] & 0x03FFFFFC;
        uint64_t covered = 0;
        for (auto& [es, ev] : exported) {
          uint32_t lo = std::max(es, vb), hi = std::min(ev.first, vb + vsize);
          if (hi > lo) covered += hi - lo;
        }
        bool zero = vsize >= 64 && std::all_of(phys + vb, phys + vb + 64, [](uint8_t v) { return v == 0; });
        if (covered || zero)
          REXLOG_INFO("fs3trace:   stream {} {:08X}+{} exported {} bytes{}", sidx, vb, vsize, covered,
                      zero ? " (zero in guest memory)" : "");
      }
      for (size_t i = 0; i < inputs.size(); i++)
        REXLOG_INFO("fs3trace:   in {} vfetch@{} present {} stream {} off {} type {:08X}", i, inputs[i].fetch_address,
                    inputs[i].present, inputs[i].stream, inputs[i].offset_bytes, inputs[i].decl_type);
      log_tex(vsv->info.textures, "vs");
      if (psv) log_tex(psv->info.textures, "ps");
    }

    // Constants.
    UploadFloatConstants(cb_vs_float.Get(), d, 0x780, vse.parsed);
    if (pse) UploadFloatConstants(cb_ps_float.Get(), d, 0x1780, pse->parsed);
    XeState st = {};
    for (int i = 0; i < 8; i++) st.bools[i] = d.u32(0x2780 + i * 4);
    for (int i = 0; i < 32; i++) st.loops[i] = d.u32(0x27A0 + i * 4);
    std::memcpy(st.vfetch, vfetch, sizeof(vfetch));
    for (int s = 0; s < 16; s++) st.strides[s] = d.u8(0x30E8 + s);
    FillTexState(st, d);
    std::memcpy(st.ndc, ndc, sizeof(ndc));
    // Vertex pulling base (SV_VertexID doesn't include the start/base vertex of the draw).
    int32_t vertex_base = call.indexed ? call.base_vertex : int32_t(call.start) + call.base_vertex;
    std::memcpy(&zxform[2], &vertex_base, 4);
    std::memcpy(st.zxform, zxform, sizeof(zxform));
    std::memcpy(st.rt_bias, t.rt_bias, sizeof(t.rt_bias));
    st.alpha[3] = 1.0f / t.scale;  // host pixels -> guest pixels (pixel position parameter)
    // User clip planes: PA_CL_CLIP_CNTL.UCP_ENA_n, planes kept by the D3D library at +0x281C.
    uint32_t clip_cntl = d.reg(0x2204);
    for (uint32_t i = 0; i < 6; i++)
      for (uint32_t k = 0; k < 4; k++) st.ucp[i][k] = (clip_cntl >> i) & 1 ? d.f32(0x281C + i * 16 + k * 4) : 0.0f;
    XeState vst = st;
    for (auto& l : vse.parsed.loop_defs) vst.loops[l.index] = l.value;
    Upload(cb_vs_state.Get(), &vst, sizeof(vst));
    if (pse) {
      uint32_t cc = d.reg(0x2202);
      st.alpha[0] = d.regf(0x210E);
      st.alpha[1] = float(cc & 7);
      st.alpha[2] = ((cc >> 3) & 1) ? 1.0f : 0.0f;
      for (auto& l : pse->parsed.loop_defs) st.loops[l.index] = l.value;
      Upload(cb_ps_state.Get(), &st, sizeof(st));
      if (tracing())
        REXLOG_INFO("fs3trace:   ps state rt_bias {} {} alpha {} {} {} info {:08X} pc {:08X} misc {:08X} tex0 {:08X} {}",
                    st.rt_bias[0], st.rt_bias[1], st.alpha[0], st.alpha[1], st.alpha[2], d.reg(0x2001),
                    d.reg(0x2180), d.reg(0x2181), st.tex[0][0], st.tex_dim[0][0]);
    }

    // Shaders and resources.
    ctx->VSSetShader(vsv->vs.Get(), nullptr, 0);
    ID3D11Buffer* vcbs[2] = {cb_vs_float.Get(), cb_vs_state.Get()};
    ctx->VSSetConstantBuffers(0, 2, vcbs);
    ID3D11ShaderResourceView* mem = memexport ? nullptr : mem_srv.Get();
    ctx->VSSetShaderResources(0, 1, &mem);
    BindTextures(d, vsv->info.textures, false);
    ctx->PSSetShader(debug_ps && ps && ps_magenta ? ps_magenta.Get() : ps, nullptr, 0);
    if (psv) {
      ID3D11Buffer* pcbs[2] = {cb_ps_float.Get(), cb_ps_state.Get()};
      ctx->PSSetConstantBuffers(0, 2, pcbs);
      BindTextures(d, psv->info.textures, true);
    }
    if (memexport) {
      ID3D11RenderTargetView* rtvs[4] = {};
      for (int i = 0; i < 4; i++)
        if (t.color[i]) rtvs[i] = t.color_rtv[i];
      ID3D11UnorderedAccessView* uav = mem_uav.Get();
      ctx->OMSetRenderTargetsAndUnorderedAccessViews(4, rtvs, t.depth ? t.depth->dsv.Get() : nullptr, 4, 1, &uav,
                                                     nullptr);
    }

    // Primitive topology and indices.
    uint32_t prim = call.primitive;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool convert = false;
    switch (prim) {
      case 1: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
      case 2: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
      case 3: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
      case 4: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
      case 6: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
      case 8: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
      case 5: case 13: convert = true; break;
      default:
        WarnOnce(0x500000000ull | prim, "unsupported primitive type", prim);
        Unbind(memexport);
        return;
    }
    // Export-only draws: every vertex must run the VS, but D3D11 may skip the vertices of an
    // incomplete trailing primitive (count not a multiple of 3), leaving stale exported vertices.
    bool export_only = memexport && !ps;
    if (export_only) {
      topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
      convert = false;
    }
    ctx->GSSetShader(prim == 8 && !export_only ? gs_rect.Get() : nullptr, nullptr, 0);
    ctx->IASetPrimitiveTopology(topo);
    ctx->IASetInputLayout(nullptr);

    if (!call.indexed && !convert) {
      ctx->Draw(call.count, 0);
      stats.submitted++;
    } else {
      // Indices: copied from guest memory (byte swapped), or generated for fans / quads.
      std::vector<uint32_t> idx;
      idx.reserve(call.count);
      if (call.indexed) {
        uint32_t ib = d.u32(0x308C);
        if (!ib) {
          Unbind(memexport);
          return;
        }
        uint32_t w0 = Be32(G(base, ib));
        bool is32 = (w0 >> 31) != 0;
        uint32_t addr = Be32(G(base, ib + 0x18));
        const uint8_t* p = G(base, addr) + uint64_t(call.start) * (is32 ? 4 : 2);
        for (uint32_t i = 0; i < call.count; i++) idx.push_back(is32 ? Be32(p + i * 4) : Be16(p + i * 2));
        // Primitive reset (PA_SU_SC_MODE_CNTL.MULTI_PRIM_IB_ENA): the reset index restarts a strip.
        // Indices are widened to 32 bits here, so it becomes the 32-bit strip cut value.
        if (tracing()) {
          uint32_t max_index = 0;
          for (uint32_t v : idx) max_index = std::max(max_index, v);
          REXLOG_INFO("fs3trace:   indices {} max {} (+base {}) stride {} -> {} bytes", idx.size(), max_index,
                      call.base_vertex, d.u8(0x30E8) * 4,
                      (uint64_t(max_index) + call.base_vertex + 1) * d.u8(0x30E8) * 4);
        }
        if (((d.reg(0x2205) >> 21) & 1) && (prim == 3 || prim == 6)) {
          uint32_t reset = d.reg(0x2103) & (is32 ? 0xFFFFFFu : 0xFFFFu);
          for (auto& v : idx)
            if ((v & (is32 ? 0xFFFFFFu : 0xFFFFu)) == reset) v = 0xFFFFFFFFu;
        }
      } else {
        for (uint32_t i = 0; i < call.count; i++) idx.push_back(i);
      }
      if (export_only) {
      } else if (prim == 5) {
        std::vector<uint32_t> l;
        for (uint32_t i = 1; i + 1 < idx.size(); i++) {
          l.push_back(idx[0]);
          l.push_back(idx[i]);
          l.push_back(idx[i + 1]);
        }
        idx.swap(l);
      } else if (prim == 13) {
        std::vector<uint32_t> l;
        for (uint32_t i = 0; i + 3 < idx.size(); i += 4) {
          uint32_t q[6] = {idx[i], idx[i + 1], idx[i + 2], idx[i], idx[i + 2], idx[i + 3]};
          l.insert(l.end(), q, q + 6);
        }
        idx.swap(l);
      }
      if (convert) ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      uint32_t bytes = uint32_t(idx.size() * 4);
      if (!bytes || bytes > index_ring_size) {
        Unbind(memexport);
        return;
      }
      D3D11_MAP map = D3D11_MAP_WRITE_NO_OVERWRITE;
      if (index_ring_pos + bytes > index_ring_size) {
        index_ring_pos = 0;
        map = D3D11_MAP_WRITE_DISCARD;
      }
      D3D11_MAPPED_SUBRESOURCE m;
      if (FAILED(ctx->Map(index_ring.Get(), 0, map, 0, &m))) {
        Unbind(memexport);
        return;
      }
      std::memcpy(static_cast<uint8_t*>(m.pData) + index_ring_pos, idx.data(), bytes);
      ctx->Unmap(index_ring.Get(), 0);
      ctx->IASetIndexBuffer(index_ring.Get(), DXGI_FORMAT_R32_UINT, index_ring_pos);
      index_ring_pos += bytes;
      ctx->DrawIndexed(uint32_t(idx.size()), 0, 0);
      stats.submitted++;
    }
    Unbind(memexport);
    if (memexport) MarkExportStreams(d);
    if (tracing() && memexport) TraceExport(d);
    if (tracing() && trace_dumps && t.color[0] && !t.color[0]->depth && trace_index < 400) {
      char path[64];
      std::snprintf(path, sizeof(path), "fs3-frame-draw%03u.bmp", trace_index - 1);
      DumpTexture2D(t.color[0]->tex.Get(), t.color[0]->format, t.color[0]->width, t.color[0]->height, path);
    }
  }

  // A memexport shader writes through stream descriptors kept in vertex shader float constants
  // (x: dword address, y: 2^23 + index, z: format/endian, w: 2^23 + element count). Every stream
  // in the constants is GPU-written memory now; the EndExport resource doesn't name all of them.
  void MarkExportStreams(const Dev& d) {
    for (uint32_t c = 0; c < 256; c++) {
      uint32_t x = d.u32(0x780 + c * 16), y = d.u32(0x784 + c * 16), z = d.u32(0x788 + c * 16),
               w = d.u32(0x78C + c * 16);
      if (((y >> 23) & 0x1FF) != 0x96 || ((w >> 23) & 0x1FF) != 0x96) continue;
      uint32_t size;
      switch ((z >> 8) & 0x3F) {
        case 38: size = 16; break;
        case 37: case 32: case 26: size = 8; break;
        case 36: case 31: case 25: case 16: case 17: case 7: case 6: case 14: size = 4; break;
        case 30: size = 2; break;
        default: continue;
      }
      uint32_t base = (x & 0x3FFFFFFF) << 2, count = w & 0x7FFFFF;
      if (!count || base >= kPhysicalSize) continue;
      MarkMirrorCurrent(base, uint32_t(std::min<uint64_t>(uint64_t(count) * size, kPhysicalSize - base)));
    }
  }

  // Diagnostics: GPU mirror vs guest memory at the export stream of c0 after a memexport draw.
  void TraceExport(const Dev& d) {
    uint32_t a = d.u32(0x780), y = d.u32(0x784);
    if (((y >> 23) & 0x1FF) != 0x96) return;
    uint32_t addr = (a & 0x3FFFFFFF) << 2;
    if (addr + 64 > kPhysicalSize) return;
    D3D11_BUFFER_DESC bd = {64, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ComPtr<ID3D11Buffer> staging;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &staging))) return;
    D3D11_BOX box = {addr, 0, 0, addr + 64, 1, 1};
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, mem_buf.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    const uint8_t* g = static_cast<const uint8_t*>(m.pData);
    REXLOG_INFO("fs3trace:   export @{:08X} gpu {:08X} {:08X} {:08X} {:08X} cpu {:08X} {:08X} {:08X} {:08X}", addr,
                Be32(g), Be32(g + 4), Be32(g + 8), Be32(g + 12), Be32(phys + addr), Be32(phys + addr + 4),
                Be32(phys + addr + 8), Be32(phys + addr + 12));
    ctx->Unmap(staging.Get(), 0);
  }

  void Unbind(bool memexport) {
    ID3D11ShaderResourceView* none[kStackedBase + kMaxBindings] = {};
    ctx->VSSetShaderResources(0, UINT(std::size(none)), none);
    ctx->PSSetShaderResources(0, UINT(std::size(none)), none);
    ctx->GSSetShader(nullptr, nullptr, 0);
    if (memexport) {
      ID3D11UnorderedAccessView* uav = nullptr;
      ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, 4,
                                                     1, &uav, nullptr);
    }
  }

  void Clear(uint8_t* base, const ClearCall& c) {
    stats.clears++;
    Dev d{base, c.device};
    TargetSetup t;
    SetupTargets(d, t, false);
    D3D11_RECT r = {0, 0, LONG(t.width), LONG(t.height)};
    // Clip to the viewport and (when enabled) the scissor, like the D3D library does.
    float vx = d.f32(0x3160), vy = d.f32(0x3164), vw = d.f32(0x3168), vh = d.f32(0x316C);
    r.left = std::max<LONG>(r.left, LONG(vx));
    r.top = std::max<LONG>(r.top, LONG(vy));
    r.right = std::min<LONG>(r.right, LONG(vx + vw));
    r.bottom = std::min<LONG>(r.bottom, LONG(vy + vh));
    if (c.rect) {
      r.left = std::max<LONG>(r.left, LONG(int32_t(Be32(G(base, c.rect)))));
      r.top = std::max<LONG>(r.top, LONG(int32_t(Be32(G(base, c.rect + 4)))));
      r.right = std::min<LONG>(r.right, LONG(int32_t(Be32(G(base, c.rect + 8)))));
      r.bottom = std::min<LONG>(r.bottom, LONG(int32_t(Be32(G(base, c.rect + 12)))));
    }
    if (d.u32(0x2E48)) {
      r.left = std::max<LONG>(r.left, LONG(d.u32(0x317C)));
      r.top = std::max<LONG>(r.top, LONG(d.u32(0x3180)));
      r.right = std::min<LONG>(r.right, LONG(d.u32(0x3184)));
      r.bottom = std::min<LONG>(r.bottom, LONG(d.u32(0x3188)));
    }
    if (tracing())
      REXLOG_INFO("fs3trace: #{} clear flags {:X} rect {},{}-{},{} rt0 {}x{} color {:08X} z {}", trace_index++, c.flags,
                  r.left, r.top, r.right, r.bottom, t.width, t.height, c.color ? Be32(G(base, c.color)) : 0, c.depth);
    if (r.right <= r.left || r.bottom <= r.top) return;
    float color[4] = {0, 0, 0, 0};
    if (c.color)
      for (int i = 0; i < 4; i++) color[i] = BeF(G(base, c.color + i * 4));
    r = ScaledRect(r, t.scale);
    for (int i = 0; i < 4; i++) {
      if ((c.flags & (1u << i)) && t.color[i]) ctx->ClearView(t.color_rtv[i], color, &r, 1);
    }
    if ((c.flags & 0x30) && t.depth) {
      UINT f = ((c.flags & 0x10) ? D3D11_CLEAR_DEPTH : 0) | ((c.flags & 0x20) ? D3D11_CLEAR_STENCIL : 0);
      ctx->ClearDepthStencilView(t.depth->dsv.Get(), f, c.depth, uint8_t(c.stencil));
    }
  }

  void Resolve(uint8_t* base, const ResolveCall& c) {
    stats.resolves++;
    Dev d{base, c.device};
    uint32_t src_index = c.flags & 7;
    bool from_depth = src_index == 4;
    uint32_t surf = from_depth ? d.u32(0x30A0) : d.u32(0x3090 + src_index * 4);
    if (!surf) return;
    uint32_t sw, sh;
    SurfaceSize(base, surf, sw, sh);
    // Tiled frames resolve tile rectangles of the logical frame.
    if (c.source_rect) {
      sw = std::max<uint32_t>(sw, std::min<uint32_t>(Be32(G(base, c.source_rect + 8)), 8192));
      sh = std::max<uint32_t>(sh, std::min<uint32_t>(Be32(G(base, c.source_rect + 12)), 8192));
    }
    RenderTarget* src = GetRenderTarget(Be32(G(base, surf + 0x1C)), Be32(G(base, surf + 0x18)), sw, sh, from_depth);
    if (!src) return;
    if (tracing())
      REXLOG_INFO("fs3trace: resolve flags {:X} level {} slice {} src surf {:08X} info {:08X}/{:08X} {}x{} dest {:08X}",
                  c.flags, c.level, c.slice, surf, Be32(G(base, surf + 0x1C)), Be32(G(base, surf + 0x18)), sw, sh,
                  c.dest_texture);
    if (c.dest_texture) {
      TexFetch f;
      for (int i = 0; i < 6; i++) f.d[i] = Be32(G(base, c.dest_texture + 0x1C + i * 4));
      NormalizeFetch(f.d);
      TextureEntry* dst = c.level == 0 ? GetTexture(f, true, src->scale) : nullptr;
      if (dst && dst->renderable) {
        int32_t sx = 0, sy = 0, ex = int32_t(sw), ey = int32_t(sh);
        if (c.source_rect) {
          sx = int32_t(Be32(G(base, c.source_rect)));
          sy = int32_t(Be32(G(base, c.source_rect + 4)));
          ex = int32_t(Be32(G(base, c.source_rect + 8)));
          ey = int32_t(Be32(G(base, c.source_rect + 12)));
        }
        int32_t dx = 0, dy = 0;
        if (c.dest_point) {
          dx = int32_t(Be32(G(base, c.dest_point)));
          dy = int32_t(Be32(G(base, c.dest_point + 4)));
        }
        uint32_t slice = std::min(c.slice, dst->depth - 1);
        auto& rtv = dst->rtv[slice];
        if (!rtv) {
          D3D11_RENDER_TARGET_VIEW_DESC rd = {};
          rd.Format = dst->format;
          if (dst->depth > 1) {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.FirstArraySlice = slice;
            rd.Texture2DArray.ArraySize = 1;
          } else {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
          }
          dev->CreateRenderTargetView(dst->tex.Get(), &rd, &rtv);
        }
        if (rtv) {
          // The resolve stores 8_8_8_8 in memory as BGRA (games sample it with a swizzle that swaps
          // red and blue back); keep host textures in the same component order.
          int32_t swap = !from_depth && src->format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                         (f.format() == 6 || f.format() == 62);
          // Texel-for-texel copy: the destination has the source's scale (GetTexture).
          const float rscale = src->scale;
          int32_t cb[12] = {ScaledCoord(sx, rscale), ScaledCoord(sy, rscale), int32_t(src->samples), swap, ScaledCoord(dx, rscale),
                            ScaledCoord(dy, rscale), 0, 0};
          float scale = std::ldexp(1.0f, int32_t(c.flags) >> 26);
          std::memcpy(&cb[8], &scale, 4);
          Upload(cb_copy.Get(), cb, sizeof(cb));
          ID3D11RenderTargetView* r = rtv.Get();
          ctx->OMSetRenderTargets(1, &r, nullptr);
          D3D11_VIEWPORT vp = {0, 0, float(ScaledSize(dst->width, rscale)), float(ScaledSize(dst->height, rscale)), 0, 1};
          ctx->RSSetViewports(1, &vp);
          D3D11_RECT sc = ScaledRect({dx, dy, dx + (ex - sx), dy + (ey - sy)}, rscale);
          ctx->RSSetScissorRects(1, &sc);
          D3D11_RASTERIZER_DESC rd = {};
          rd.FillMode = D3D11_FILL_SOLID;
          rd.CullMode = D3D11_CULL_NONE;
          rd.ScissorEnable = TRUE;
          rd.DepthClipEnable = TRUE;
          uint64_t rkey = HashBytes(&rd, sizeof(rd));
          auto& rs = raster_states[rkey];
          if (!rs) dev->CreateRasterizerState(&rd, &rs);
          ctx->RSSetState(rs.Get());
          ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
          ctx->OMSetDepthStencilState(nullptr, 0);
          ctx->IASetInputLayout(nullptr);
          ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          ctx->VSSetShader(vs_fullscreen.Get(), nullptr, 0);
          ctx->GSSetShader(nullptr, nullptr, 0);
          bool ms = src->samples > 1;
          ctx->PSSetShader(from_depth ? (ms ? ps_copy_depth_ms.Get() : ps_copy_depth.Get())
                                      : (ms ? ps_copy_ms.Get() : ps_copy.Get()),
                           nullptr, 0);
          ID3D11Buffer* cbp = cb_copy.Get();
          ctx->PSSetConstantBuffers(0, 1, &cbp);
          ID3D11ShaderResourceView* s = src->srv.Get();
          ctx->PSSetShaderResources(0, 1, &s);
          ctx->Draw(3, 0);
          ID3D11ShaderResourceView* none = nullptr;
          ctx->PSSetShaderResources(0, 1, &none);
          ctx->OMSetRenderTargets(0, nullptr, nullptr);
          // The texture now holds GPU data newer than guest memory; CPU writes invalidate it.
          if (dst->can_generate_mips) ctx->GenerateMips(dst->srv.Get());
          if (tracing()) {
            REXLOG_INFO("fs3trace: #{} resolve flags {:X} src {}x{} fmt {} samples {} rect {},{}-{},{} -> {:08X} fmt {} "
                        "{}x{} at {},{}",
                        trace_index, c.flags, sw, sh, (Be32(G(base, surf + 0x1C)) >> 16) & 0xF, src->samples, sx, sy,
                        ex, ey, f.base_address(), f.format(), dst->width, dst->height, dx, dy);
            char path[64];
            std::snprintf(path, sizeof(path), "fs3-frame-res%03u.bmp", trace_index);
            DumpTexture(*dst, path);
            trace_index++;
          }
          dst->gen_sum = EntryGenSum(*dst);
          Watch(dst->base, dst->base_bytes);
        }
      } else if (dst == nullptr) {
        WarnOnce(0x600000000ull | f.format(), "resolve to an unsupported destination format", f.format());
      }
    }
    if (c.flags & 0x100 && !from_depth && src->rtv) {
      float color[4] = {0, 0, 0, 0};
      if (c.clear_color)
        for (int i = 0; i < 4; i++) color[i] = BeF(G(base, c.clear_color + i * 4));
      ctx->ClearRenderTargetView(src->rtv.Get(), color);
    }
    if (c.flags & 0x200) {
      uint32_t ds = d.u32(0x30A0);
      if (ds) {
        uint32_t w, h;
        SurfaceSize(base, ds, w, h);
        RenderTarget* depth = GetRenderTarget(d.reg(0x2002), d.reg(0x2000), w, h, true);
        if (depth) ctx->ClearDepthStencilView(depth->dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                              c.clear_depth, uint8_t(c.clear_stencil));
      }
    }
  }

  void LogStats(const char* front) {
    stats.presents++;
    auto now = std::chrono::steady_clock::now();
    if (!log_stats || now - stats_start < std::chrono::seconds(1)) return;
    REXLOG_INFO("[native-gfx] presents {} draws {} (submitted {}) clears {} resolves {} memexport {}/{} front {}",
                stats.presents, stats.draws, stats.submitted, stats.clears, stats.resolves, stats.exports,
                stats.export_ends, front);
    stats = Stats{};
    stats_start = now;
  }

  // Writes the top level of a texture as a 24-bit BMP (diagnostics).
  void DumpTexture(TextureEntry& e, const std::string& path) {
    if (e.is_3d) return;
    ComPtr<ID3D11Texture2D> src;
    e.tex.As(&src);
    DumpTexture2D(src.Get(), e.format, e.width, e.height, path);
  }

  void DumpTexture2D(ID3D11Texture2D* source, DXGI_FORMAT format, uint32_t width, uint32_t height,
                     const std::string& path) {
    ComPtr<ID3D11Texture2D> src = source;
    D3D11_TEXTURE2D_DESC td;
    src->GetDesc(&td);
    if (td.SampleDesc.Count > 1) {
      // Resolve multisampled targets first.
      D3D11_TEXTURE2D_DESC rd = td;
      rd.SampleDesc = {1, 0};
      rd.BindFlags = 0;
      rd.MiscFlags = 0;
      ComPtr<ID3D11Texture2D> resolved;
      if (FAILED(dev->CreateTexture2D(&rd, nullptr, &resolved))) return;
      ctx->ResolveSubresource(resolved.Get(), 0, src.Get(), 0, format);
      src = resolved;
      td = rd;
    }
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) return;
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, src.Get(), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    uint32_t w = std::min(width, td.Width), h = std::min(height, td.Height), row = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> bmp(54 + size_t(row) * h, 0);
    auto put32 = [&](size_t o, uint32_t v) { std::memcpy(&bmp[o], &v, 4); };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, uint32_t(bmp.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, uint32_t(-int32_t(h)));
    bmp[26] = 1;
    bmp[28] = 24;
    if (format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC3_UNORM || format == DXGI_FORMAT_BC2_UNORM) {
      // Color part of BC1/2/3 blocks (alpha ignored).
      uint32_t block = format == DXGI_FORMAT_BC1_UNORM ? 8 : 16;
      for (uint32_t by = 0; by < (h + 3) / 4; by++) {
        const uint8_t* rowp = static_cast<const uint8_t*>(m.pData) + size_t(by) * m.RowPitch;
        for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
          const uint8_t* b = rowp + bx * block + (block - 8);
          uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
          float pal[4][3];
          auto rgb = [](uint16_t c, float* o) {
            o[0] = ((c >> 11) & 31) / 31.0f;
            o[1] = ((c >> 5) & 63) / 63.0f;
            o[2] = (c & 31) / 31.0f;
          };
          rgb(c0, pal[0]);
          rgb(c1, pal[1]);
          for (int k = 0; k < 3; k++) {
            pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
            pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
          }
          uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | (uint32_t(b[7]) << 24);
          for (uint32_t t = 0; t < 16; t++) {
            uint32_t px = bx * 4 + (t & 3), py = by * 4 + (t >> 2);
            if (px >= w || py >= h) continue;
            const float* c = pal[(idx >> (t * 2)) & 3];
            uint8_t* o = &bmp[54 + size_t(py) * row + px * 3];
            for (int k = 0; k < 3; k++) o[2 - k] = uint8_t(c[k] * 255.0f + 0.5f);
          }
        }
      }
    }
    for (uint32_t y = 0; y < h; y++) {
      const uint8_t* in = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
      for (uint32_t x = 0; x < w; x++) {
        float c[3] = {0, 0, 0};
        if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
          uint32_t v;
          std::memcpy(&v, in + x * 4, 4);
          for (int k = 0; k < 3; k++) c[k] = float((v >> (10 * k)) & 0x3FF) / 1023.0f;
        } else if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
          for (int k = 0; k < 3; k++) c[k] = in[x * 4 + k] / 255.0f;
        } else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
          for (int k = 0; k < 3; k++) {
            uint16_t hv;
            std::memcpy(&hv, in + x * 8 + k * 2, 2);
            uint32_t e5 = (hv >> 10) & 31, m = hv & 1023;
            c[k] = e5 ? std::ldexp(1.0f + m / 1024.0f, int(e5) - 15) : std::ldexp(m / 1024.0f, -14);
          }
        } else {
          continue;
        }
        uint8_t* o = &bmp[54 + size_t(y) * row + x * 3];
        for (int k = 0; k < 3; k++) o[2 - k] = uint8_t(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
      }
    }
    ctx->Unmap(staging.Get(), 0);
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
      std::fwrite(bmp.data(), 1, bmp.size(), fp);
      std::fclose(fp);
    }
  }

  void Present(uint8_t* base) {
    (void)base;
    if (tracing()) REXLOG_INFO("fs3trace: present (frame {})", frames);
    if (trace_at >= 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count() >= trace_at) {
      trace_frame = frames + 1;
      trace_at = -1.0;
    }
    frames++;
    trace_index = 0;
    // Export streams not rewritten for a few frames have been released.
    for (auto it = exported.begin(); it != exported.end();)
      it = frames - it->second.second > 4 ? exported.erase(it) : std::next(it);
    trace_vs_draws = 0;
    window_frames++;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - window_start).count();
    if (elapsed >= 1.0) {
      fps.store(window_frames / elapsed);
      window_frames = 0;
      window_start = now;
    }
    if (!have_front) {
      LogStats("none");
      return;
    }
    TexFetch f;
    std::memcpy(f.d, front_fetch, sizeof(f.d));
    TextureEntry* e = GetTexture(f, false);
    LogStats(e ? "ok" : "unsupported");
    if (!e) {
      WarnOnce(0x700000000ull | f.format(), "unsupported front buffer format", f.format());
      return;
    }
    RefreshTexture(*e);
    if (dump_every && frames % dump_every == 0) {
      char path[64];
      std::snprintf(path, sizeof(path), "fs3-frame-%06llu.bmp", static_cast<unsigned long long>(frames));
      DumpTexture(*e, path);
    }
    // Final picture, settings menu, vsync and frame pacing: pc_settings.cpp.
    pcs::Picture picture;
    picture.srv = e->srv.Get();
    // The console's display scaler stretches the front buffer (e.g. 1024x600) to the video
    // mode: show it with the video mode's shape, not the buffer's.
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    picture.width = mode.display_width ? float(mode.display_width) : 1280.0f;
    picture.height = mode.display_height ? float(mode.display_height) : 720.0f;
    if (frames < 3 || frames % 3600 == 0)
      REXLOG_INFO("[native-gfx] front buffer {}x{} shown as {}x{}", e->width, e->height, picture.width,
                  picture.height);
    // Front buffers come from resolves, so 8_8_8_8 ones are in BGRA order (see Resolve).
    picture.swap_rb = e->format == DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!EnsureSwapChain()) return;
    pcs::Present(dev.Get(), ctx.Get(), swap.Get(), picture,
                 [this](ID3D11RenderTargetView** rtv, uint32_t* w, uint32_t* h) {
                   if (!EnsureSwapChain()) return false;
                   *rtv = back_rtv.Get();
                   *w = back_w;
                   *h = back_h;
                   return true;
                 });
  }

  // The swap chain on the SDK window, sized to its client area.
  bool EnsureSwapChain() {
    if (!hwnd) {
      auto* runtime = rex::Runtime::instance();
      if (auto* window = runtime ? runtime->display_window() : nullptr)
        hwnd = static_cast<HWND>(window->GetNativeWindowHandle());
      if (!hwnd) return false;
    }
    RECT rc;
    if (!GetClientRect(hwnd, &rc)) return false;
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1), h = std::max<LONG>(rc.bottom - rc.top, 1);
    if (swap && back_rtv && w == back_w && h == back_h) return true;
    if (!swap) {
      ComPtr<IDXGIDevice> dxgi_device;
      ComPtr<IDXGIAdapter> adapter;
      ComPtr<IDXGIFactory2> factory;
      dev.As(&dxgi_device);
      dxgi_device->GetAdapter(&adapter);
      adapter->GetParent(IID_PPV_ARGS(&factory));
      DXGI_SWAP_CHAIN_DESC1 d{};
      d.Width = w;
      d.Height = h;
      d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.SampleDesc.Count = 1;
      d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      d.BufferCount = 2;
      d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      d.Flags = pcs::SwapChainFlags();
      if (FAILED(factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &d, nullptr, nullptr, &swap))) {
        REXLOG_ERROR("[native-gfx] swap chain creation failed");
        return false;
      }
      factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    } else {
      back_rtv.Reset();
      ctx->OMSetRenderTargets(0, nullptr, nullptr);
      ctx->Flush();
      if (FAILED(swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, pcs::SwapChainFlags()))) return false;
    }
    ComPtr<ID3D11Texture2D> bb;
    swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    dev->CreateRenderTargetView(bb.Get(), nullptr, &back_rtv);
    back_w = w;
    back_h = h;
    return back_rtv != nullptr;
  }
};

Renderer& Renderer::Get() {
  static Renderer renderer;
  return renderer;
}

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

bool Renderer::Initialize() {
  std::lock_guard lock(impl_->mutex);
  return impl_->Init();
}

void Renderer::AttachMemory(rex::memory::Memory* memory) {
  std::lock_guard lock(impl_->mutex);
  impl_->memory = memory;
  impl_->phys = memory->physical_membase();
  impl_->invalidation_handle = memory->RegisterPhysicalMemoryInvalidationCallback(&Impl::OnInvalidate, impl_.get());
}

void Renderer::Shutdown() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->memory && impl_->invalidation_handle) {
    impl_->memory->UnregisterPhysicalMemoryInvalidationCallback(impl_->invalidation_handle);
    impl_->invalidation_handle = nullptr;
  }
  impl_->ready = false;
}

bool Renderer::ready() const { return impl_->ready && impl_->phys; }

void Renderer::RegisterShader(uint8_t* base, uint32_t object, uint32_t container) {
  if (!object || !container) return;
  std::lock_guard lock(impl_->mutex);
  impl_->RegisterShader(base, object, container);
}

void Renderer::Draw(uint8_t* base, const DrawCall& call) {
  if (!ready()) return;
  std::lock_guard lock(impl_->mutex);
  impl_->Draw(base, call);
}

void Renderer::Clear(uint8_t* base, const ClearCall& call) {
  if (!ready()) return;
  std::lock_guard lock(impl_->mutex);
  impl_->Clear(base, call);
}

void Renderer::Resolve(uint8_t* base, const ResolveCall& call) {
  if (!ready()) return;
  std::lock_guard lock(impl_->mutex);
  impl_->Resolve(base, call);
}

void Renderer::EndExport(uint8_t* base, uint32_t resource) {
  if (!ready() || !resource) return;
  std::lock_guard lock(impl_->mutex);
  uint32_t w0 = Be32(G(base, resource));
  impl_->stats.export_ends++;
  if ((w0 & 0xF) != 1) return;  // vertex buffer
  uint32_t addr = Be32(G(base, resource + 0x18)) & 0xFFFFFFFC;
  uint32_t size = Be32(G(base, resource + 0x1C)) & 0x03FFFFFC;
  if (impl_->tracing()) REXLOG_INFO("fs3trace:   end export {:08X}+{}", ToPhys(addr), size);
  impl_->MarkMirrorCurrent(ToPhys(addr), size);
}

void Renderer::SetFrontBuffer(uint8_t* base, uint32_t texture) {
  std::lock_guard lock(impl_->mutex);
  for (int i = 0; i < 6; i++) impl_->front_fetch[i] = Be32(G(base, texture + 0x1C + i * 4));
  NormalizeFetch(impl_->front_fetch);
  impl_->have_front = true;
}

void Renderer::Present(uint8_t* base) {
  if (!ready()) return;
  std::lock_guard lock(impl_->mutex);
  impl_->Present(base);
}

void Renderer::EnableGpulessMode(uint8_t* base, uint32_t device) {
  if (device) *rex::memory::GuestPtr(base, device + 0x2ABD) |= 0x02;
}

void Renderer::SignalFences(uint8_t* base, uint32_t device) {
  if (!device) return;
  EnableGpulessMode(base, device);
  uint32_t counter = Be32(G(base, device + 0x2A9C));
  uint32_t target = Be32(G(base, device + 0x2A90));
  if (target) *reinterpret_cast<uint32_t*>(rex::memory::GuestPtr(base, target)) = _byteswap_ulong(counter);
}

double Renderer::frames_per_second() const { return impl_->fps.load(); }

}  // namespace fs3gfx
