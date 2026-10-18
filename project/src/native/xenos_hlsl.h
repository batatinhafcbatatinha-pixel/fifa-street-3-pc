// Xbox 360 (Xenos) shader microcode -> HLSL (Shader Model 5) translator for the native renderer.
//
// Input is the XDK shader container the game hands to CreateVertexShader / CreatePixelShader
// (magic 0x102A1100 vertex / 0x102A1101 pixel). Output is HLSL that the D3D11 renderer compiles
// with FXC. Binding model (shared by every translated shader, see native_renderer.cpp):
//   b0  XeFloat   float4 XC[256]      stage float constants (shader literals already overlaid)
//   b1  XeState   bools, loops, vertex fetch constants, stream strides, per-texture info, misc
//   t0  XeMem     ByteAddressBuffer   guest physical memory mirror (vertex pulling)
//   u4  XeMemRW   RWByteAddressBuffer same memory for memexport shaders (also used for their fetches)
//   t1+i / s(i)   texture binding i of TranslatedShader::textures
//   t17+i         3D bindings: the texture as a Texture2DArray (stacked 2D); XTex[slot].z picks
// Interpolators use one fixed struct (XeInterp) so every VS links with every PS.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fs3gfx {

enum class ShaderStage : uint32_t { kVertex = 0, kPixel = 1 };

struct ShaderVertexElement {
  uint32_t fetch_address;  // index of the vfetch instruction (in 3-dword units)
  uint32_t usage;          // D3DDECLUSAGE
  uint32_t usage_index;
};

struct ShaderInterpolator {
  uint32_t reg;  // VS: export index; PS: input register
  uint32_t usage;
  uint32_t usage_index;
};

struct ShaderFloatDef {
  uint32_t index;  // stage-relative float constant register
  uint32_t value[4];
};

struct ShaderLoopDef {
  uint32_t index;  // loop constant 0..31
  uint32_t value;
};

struct ParsedShader {
  ShaderStage stage = ShaderStage::kVertex;
  std::vector<uint32_t> ucode;  // host-endian dwords
  std::vector<ShaderVertexElement> vertex_elements;
  std::vector<ShaderInterpolator> interpolators;
  std::vector<ShaderFloatDef> float_defs;
  std::vector<ShaderLoopDef> loop_defs;
  uint32_t ps_param_gen_reg = 0xFF;
  uint32_t ps_outputs = 0;  // bit0..3 color 0..3, bit4 depth
  uint64_t hash = 0;        // of the whole container
};

bool ParseShaderContainer(const uint8_t* data, size_t size, ParsedShader& out, std::string* error);

// Resolved vertex input for one vfetch of a vertex shader, from the bound vertex declaration.
struct VertexInputElement {
  uint32_t fetch_address = 0;
  bool present = false;  // declaration has no matching element -> (0,0,0,1)
  uint32_t stream = 0;
  uint32_t offset_bytes = 0;
  uint32_t decl_type = 0;  // Xbox 360 D3DDECLTYPE
};

constexpr uint32_t kStackedTextureBase = 17;

struct TextureBinding {
  uint32_t fetch_slot;  // texture fetch constant 0..31
  uint32_t dimension;   // 0 1D, 1 2D, 2 3D/stacked, 3 cube (instruction dimension)
};

struct TranslatedShader {
  std::string hlsl;
  std::vector<TextureBinding> textures;  // binding i -> t(1+i), s(i)
  bool uses_memexport = false;
  bool writes_depth = false;
  bool uses_kill = false;
  uint32_t color_outputs = 0;
};

// vs_inputs is required for vertex shaders (one entry per vertex element of the shader).
bool TranslateShader(const ParsedShader& shader, const std::vector<VertexInputElement>* vs_inputs,
                     TranslatedShader& out, std::string* error);

// Interpolator slot (0..23) used for a D3DDECLUSAGE/index pair in XeInterp, or -1.
int InterpolatorSlot(uint32_t usage, uint32_t usage_index);
constexpr uint32_t kInterpolatorSlots = 24;

}  // namespace fs3gfx
