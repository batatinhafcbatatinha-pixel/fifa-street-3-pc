// Offline check of the Xenos -> HLSL translator: parses every extracted shader container,
// translates it and compiles the HLSL with FXC (D3DCompile). Does not run the game.
// Usage: fs3_shader_test <dir with *.bin> [--dump <outdir>]

#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../src/native/xenos_hlsl.h"

using namespace fs3gfx;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: fs3_shader_test <dir> [--dump <outdir>]\n");
    return 2;
  }
  std::filesystem::path dir = argv[1];
  std::filesystem::path dump;
  if (argc >= 4 && std::string(argv[2]) == "--dump") dump = argv[3];
  HMODULE fxc = LoadLibraryW(L"d3dcompiler_47.dll");
  if (!fxc) {
    std::printf("d3dcompiler_47.dll not found\n");
    return 1;
  }
  auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(fxc, "D3DCompile"));
  int total = 0, parsed = 0, ok = 0, memexport = 0, general = 0;
  int dims[4] = {}, psize = 0, kill = 0, depth = 0;
  for (auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() != ".bin") continue;
    total++;
    std::ifstream f(entry.path(), std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    ParsedShader ps;
    std::string err;
    if (!ParseShaderContainer(data.data(), data.size(), ps, &err)) {
      std::printf("PARSE FAIL %s: %s\n", entry.path().filename().string().c_str(), err.c_str());
      continue;
    }
    parsed++;
    std::vector<VertexInputElement> inputs;
    for (size_t i = 0; i < ps.vertex_elements.size(); i++) {
      VertexInputElement in;
      in.fetch_address = ps.vertex_elements[i].fetch_address;
      in.present = true;
      in.stream = uint32_t(i % 2);
      in.offset_bytes = uint32_t(i * 16);
      in.decl_type = i % 3 == 0 ? 0x1A23A6 : (i % 3 == 1 ? 0x2A2187 : 0x2C235F);  // FLOAT4, DEC3N, FLOAT16_2
      inputs.push_back(in);
    }
    TranslatedShader ts;
    if (!TranslateShader(ps, ps.stage == ShaderStage::kVertex ? &inputs : nullptr, ts, &err)) {
      std::printf("TRANSLATE FAIL %s: %s\n", entry.path().filename().string().c_str(), err.c_str());
      continue;
    }
    if (ts.uses_memexport) memexport++;
    for (auto& t : ts.textures) dims[t.dimension & 3]++;
    if (ts.hlsl.find("oPts.x =") != std::string::npos) psize++;
    if (ts.uses_kill) kill++;
    if (ts.writes_depth) depth++;
    if (ts.hlsl.find("xe_npc") != std::string::npos) general++;
    const char* target = ps.stage == ShaderStage::kVertex ? "vs_5_0" : "ps_5_0";
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(ts.hlsl.data(), ts.hlsl.size(), entry.path().filename().string().c_str(), nullptr, nullptr,
                         "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
    if (FAILED(hr)) {
      // Same fallback as the renderer: FXC's optimizer fails some looped shaders (X4000).
      if (errors) errors->Release();
      errors = nullptr;
      hr = compile(ts.hlsl.data(), ts.hlsl.size(), entry.path().filename().string().c_str(), nullptr, nullptr, "main",
                   target, D3DCOMPILE_SKIP_OPTIMIZATION, 0, &code, &errors);
    }
    if (!dump.empty()) {
      std::filesystem::create_directories(dump);
      std::ofstream(dump / (entry.path().stem().string() + (ps.stage == ShaderStage::kVertex ? ".vs.hlsl" : ".ps.hlsl")))
          << ts.hlsl;
    }
    if (FAILED(hr)) {
      std::printf("COMPILE FAIL %s (%s): %s\n", entry.path().filename().string().c_str(), target,
                  errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    } else {
      ok++;
    }
    if (code) code->Release();
    if (errors) errors->Release();
  }
  std::printf("total %d parsed %d compiled %d memexport %d general-cf %d\n", total, parsed, ok, memexport, general);
  std::printf("texture bindings: 1D %d 2D %d 3D/stacked %d cube %d; point size %d, kill %d, depth %d\n", dims[0],
              dims[1], dims[2], dims[3], psize, kill, depth);
  return ok == total ? 0 : 1;
}
