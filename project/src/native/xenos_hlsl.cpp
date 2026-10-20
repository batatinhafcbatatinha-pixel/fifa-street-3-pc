// Xenos microcode -> HLSL SM5 translator. See xenos_hlsl.h for the binding model.
// Instruction semantics follow rex/graphics/format/ucode.h (Xenia, BSD).

#include "xenos_hlsl.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include <rex/graphics/format/ucode.h>

namespace fs3gfx {

using namespace rex::graphics;
using namespace rex::graphics::ucode;

namespace {

uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; i++) {
    h ^= data[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

std::string Format(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n < 0) return {};
  if (size_t(n) < sizeof(buf)) return std::string(buf, size_t(n));
  std::string big(size_t(n) + 1, '\0');
  va_start(args, fmt);
  vsnprintf(big.data(), big.size(), fmt, args);
  va_end(args);
  big.resize(size_t(n));
  return big;
}

const char kComp[] = {'x', 'y', 'z', 'w'};

}  // namespace

int InterpolatorSlot(uint32_t usage, uint32_t usage_index) {
  switch (usage) {
    case 5:  // TEXCOORD
      return usage_index < 16 ? int(usage_index) : -1;
    case 10:  // COLOR
      return usage_index < 2 ? int(16 + usage_index) : -1;
    case 3:  // NORMAL
      return 18;
    case 6:  // TANGENT
      return 19;
    case 7:  // BINORMAL
      return 20;
    case 0:  // POSITION (as interpolator)
      return 21;
    case 11:  // FOG
      return 22;
    default:
      return 23;
  }
}

bool ParseShaderContainer(const uint8_t* data, size_t size, ParsedShader& out, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (size < 0x24) return fail("container too small");
  uint32_t flags = be32(data);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) return fail("bad container magic");
  out = ParsedShader{};
  out.stage = (flags & 1) ? ShaderStage::kVertex : ShaderStage::kPixel;
  out.hash = Fnv1a64(data, size);
  uint32_t virtual_size = be32(data + 4);
  uint32_t definition_offset = be32(data + 0x14);
  uint32_t shader_offset = be32(data + 0x18);
  if (shader_offset + 0x20 > size) return fail("shader header out of range");
  const uint8_t* sh = data + shader_offset;
  uint32_t physical_offset = be32(sh + 0);
  uint32_t code_size = be32(sh + 4);
  uint32_t field_c = be32(sh + 0xC);
  uint32_t interpolator_info = be32(sh + 0x14);
  size_t code_start = size_t(virtual_size) + physical_offset;
  if (code_start + code_size > size || (code_size % 12) != 0) return fail("microcode out of range");
  out.ucode.resize(code_size / 4);
  for (size_t i = 0; i < out.ucode.size(); i++) out.ucode[i] = be32(data + code_start + i * 4);

  uint32_t interpolator_count = (interpolator_info >> 5) & 0x1F;
  if (out.stage == ShaderStage::kVertex) {
    uint32_t skip = be32(sh + 0x18);
    uint32_t element_count = be32(sh + 0x1C);
    size_t list = shader_offset + 0x24;
    size_t needed = list + size_t(skip + element_count + interpolator_count) * 4;
    if (needed > size) return fail("vertex element table out of range");
    for (uint32_t i = 0; i < element_count; i++) {
      uint32_t v = be32(data + list + size_t(skip + i) * 4);
      out.vertex_elements.push_back({v & 0xFFF, (v >> 12) & 0xF, (v >> 16) & 0xF});
    }
    for (uint32_t i = 0; i < interpolator_count; i++) {
      uint32_t v = be32(data + list + size_t(skip + element_count + i) * 4);
      out.interpolators.push_back({i, (v >> 4) & 0xF, v & 0xF});
    }
  } else {
    out.ps_outputs = be32(sh + 0x1C);
    out.ps_param_gen_reg = (field_c >> 8) & 0xFF;
    size_t list = shader_offset + 0x20;
    if (list + size_t(interpolator_count) * 4 > size) return fail("interpolator table out of range");
    for (uint32_t i = 0; i < interpolator_count; i++) {
      uint32_t v = be32(data + list + size_t(i) * 4);
      out.interpolators.push_back({(v >> 8) & 0xF, (v >> 4) & 0xF, v & 0xF});
    }
  }

  if (definition_offset) {
    size_t p = size_t(definition_offset) + 5 * 4;
    auto dword = [&](size_t at) -> uint32_t { return at + 4 <= size ? be32(data + at) : 0; };
    // Float definitions: {u16 register, u16 component count, u32 physical offset}, 0-terminated.
    while (p + 8 <= size && dword(p) != 0) {
      uint32_t reg = be16(data + p);
      uint32_t count = be16(data + p + 2);
      uint32_t phys = dword(p + 4);
      size_t values = size_t(virtual_size) + phys;
      for (uint32_t i = 0; i < (count + 3) / 4; i++) {
        if (values + 16 > size) break;
        ShaderFloatDef def;
        uint32_t index = reg + i;
        if (out.stage == ShaderStage::kPixel && index >= 256) index -= 256;
        def.index = index & 0xFF;
        for (int c = 0; c < 4; c++) def.value[c] = be32(data + values + c * 4);
        out.float_defs.push_back(def);
        values += 16;
      }
      p += 8;
    }
    p += 4;
    // Integer (loop) definitions: {u16 register, u16 count, u32 values[count]} + padding dword.
    while (p + 4 <= size && dword(p) != 0) {
      uint32_t reg = be16(data + p);
      uint32_t count = be16(data + p + 2);
      for (uint32_t i = 0; i < count; i++) {
        if (p + 4 + (i + 1) * 4 > size) break;
        uint32_t value = be32(data + p + 4 + i * 4);
        int32_t index = (int32_t(reg) - 8992) / 4 + int32_t(i);
        if (index >= 0 && index < 32) out.loop_defs.push_back({uint32_t(index), value});
      }
      p += 8 + size_t(count) * 4;
    }
  }
  return true;
}

namespace {

constexpr uint32_t kExportAddress = 32;
constexpr uint32_t kExportData0 = 33;

struct DeclTypeInfo {
  uint32_t format;  // xenos::VertexFormat
  uint32_t endian;
  bool is_signed;
  bool is_integer;
  uint32_t swizzle;  // 4 x 3 bits
};

DeclTypeInfo DecodeDeclType(uint32_t t) {
  DeclTypeInfo i;
  i.format = t & 0x3F;
  i.endian = (t >> 6) & 3;
  i.is_signed = ((t >> 8) & 1) != 0;
  i.is_integer = ((t >> 9) & 1) != 0;
  i.swizzle = (t >> 10) & 0xFFF;
  return i;
}

class Translator {
 public:
  Translator(const ParsedShader& s, const std::vector<VertexInputElement>* inputs, TranslatedShader& out)
      : s_(s), inputs_(inputs), out_(out) {
    is_ps_ = s.stage == ShaderStage::kPixel;
  }

  bool Run(std::string* error);

 private:
  // --- emission helpers ---
  void Line(const std::string& text) {
    body_.append(size_t(indent_) * 2, ' ');
    body_ += text;
    body_ += '\n';
  }

  std::string Temp(uint32_t index, bool relative) const {
    if (temps_array_) {
      if (relative) return Format("R[(%u + aL) & 63]", index & 63);
      return Format("R[%u]", index & 63);
    }
    return Format("r%u", index & 63);
  }

  std::string ConstRef(uint32_t index, bool addressed, bool a0_relative) const {
    if (!addressed) return Format("XC[%u]", index & 255);
    return Format("xe_c(%d + %s)", int(index & 255), a0_relative ? "a0" : "aL");
  }

  uint32_t BindTexture(uint32_t slot, uint32_t dimension) {
    for (size_t i = 0; i < out_.textures.size(); i++) {
      if (out_.textures[i].fetch_slot == slot && out_.textures[i].dimension == dimension) return uint32_t(i);
    }
    out_.textures.push_back({slot, dimension});
    return uint32_t(out_.textures.size() - 1);
  }

  void Prescan();
  void EmitExec(uint32_t address, uint32_t count, uint32_t sequence);
  void EmitAlu(const AluInstruction& alu);
  void EmitVertexFetch(const VertexFetchInstruction& vf, uint32_t address);
  void EmitTextureFetch(const TextureFetchInstruction& tf);
  void EmitCfInstruction(uint32_t index, const ControlFlowInstruction& cf, bool general);
  std::string AluOperand(const AluInstruction& alu, uint32_t i) const;
  std::string BuildPrelude() const;
  std::string BuildEntry() const;

  const ParsedShader& s_;
  const std::vector<VertexInputElement>* inputs_;
  TranslatedShader& out_;
  bool is_ps_ = false;
  bool temps_array_ = false;
  bool uses_memexport_ = false;
  bool uses_kill_ = false;
  bool uses_cube_ = false;
  std::vector<ControlFlowInstruction> cf_;
  std::string body_;
  int indent_ = 2;
  std::set<uint32_t> exported_interpolators_;
  bool exports_position_ = false;
  bool exports_point_size_ = false;
};

void Translator::Prescan() {
  // Control flow: CF instructions are packed two per three dwords at the start of the ucode,
  // up to the lowest exec address.
  uint32_t limit = uint32_t(s_.ucode.size());
  for (uint32_t i = 0; i * 3 + 2 < limit; i++) {
    ControlFlowInstruction pair[2];
    UnpackControlFlowInstructions(&s_.ucode[i * 3], pair);
    for (auto& cf : pair) {
      cf_.push_back(cf);
      switch (cf.opcode()) {
        case ControlFlowOpcode::kExec:
        case ControlFlowOpcode::kExecEnd:
          limit = std::min(limit, cf.exec.address() * 3);
          break;
        case ControlFlowOpcode::kCondExec:
        case ControlFlowOpcode::kCondExecEnd:
        case ControlFlowOpcode::kCondExecPredClean:
        case ControlFlowOpcode::kCondExecPredCleanEnd:
          limit = std::min(limit, cf.cond_exec.address() * 3);
          break;
        case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredEnd:
          limit = std::min(limit, cf.cond_exec_pred.address() * 3);
          break;
        default:
          break;
      }
    }
  }
  // Relative register use anywhere forces the temp array.
  for (auto& cf : cf_) {
    if (!IsControlFlowOpcodeExec(cf.opcode())) continue;
    uint32_t address = cf.exec.address(), count = cf.exec.count(), seq = cf.exec.sequence();
    for (uint32_t i = 0; i < count; i++, seq >>= 2) {
      uint32_t at = (address + i) * 3;
      if (at + 2 >= s_.ucode.size()) break;
      const uint32_t* w = &s_.ucode[at];
      if (seq & 1) {
        FetchInstruction f;
        std::memcpy(&f, w, sizeof(f));
        if (f.is_dest_relative() || f.is_src_relative()) temps_array_ = true;
      } else {
        AluInstruction a;
        std::memcpy(&a, w, sizeof(a));
        if (a.is_vector_dest_relative() || (a.is_scalar_dest_relative() && !a.is_export())) temps_array_ = true;
        for (uint32_t k = 1; k <= 3; k++) {
          if (a.src_is_temp(k) && AluInstruction::is_src_temp_relative(a.src_reg(k))) temps_array_ = true;
        }
        if (a.is_export() && !is_ps_) {
          uint32_t d = a.vector_dest();
          if (d < 16) exported_interpolators_.insert(d);
          if (d == 62) exports_position_ = true;
          if (d == 63) exports_point_size_ = true;
        }
        if (a.is_export() && (a.vector_dest() == kExportAddress || (a.vector_dest() >= kExportData0 &&
                                                                    a.vector_dest() < kExportData0 + 5))) {
          uses_memexport_ = true;
        }
      }
    }
  }
}

std::string Translator::AluOperand(const AluInstruction& alu, uint32_t i) const {
  uint32_t reg = alu.src_reg(i);
  std::string base;
  bool abs;
  if (alu.src_is_temp(i)) {
    base = Temp(AluInstruction::src_temp_reg(reg), AluInstruction::is_src_temp_relative(reg));
    abs = AluInstruction::is_src_temp_value_absolute(reg);
  } else {
    base = ConstRef(reg, alu.src_const_is_addressed(i), alu.is_const_address_register_relative());
    abs = alu.abs_constants();
  }
  uint32_t swz = alu.src_swizzle(i);
  std::string sw = ".";
  for (uint32_t c = 0; c < 4; c++) sw += kComp[AluInstruction::GetSwizzledComponentIndex(swz, c)];
  std::string v = abs ? "abs(" + base + ")" : base;
  v += sw;
  if (alu.src_negate(i)) v = "(-" + v + ")";
  return v;
}

void Translator::EmitAlu(const AluInstruction& alu) {
  bool predicated = alu.is_predicated();
  if (predicated) {
    Line(Format("if (p0 == %s) {", alu.predicate_condition() ? "true" : "false"));
    indent_++;
  } else {
    Line("{");
    indent_++;
  }
  AluVectorOpcode vop = alu.vector_opcode();
  AluScalarOpcode sop = alu.scalar_opcode();
  bool is_export = alu.is_export();
  uint32_t vmask = alu.GetVectorOpResultWriteMask();
  uint32_t smask = alu.GetScalarOpResultWriteMask();

  // Which vector ops have effects even with an empty write mask.
  bool vector_side_effect = (vop >= AluVectorOpcode::kSetpEqPush && vop <= AluVectorOpcode::kKillNe) ||
                            vop == AluVectorOpcode::kMaxA;
  bool do_vector = vmask != 0 || vector_side_effect || (is_export && alu.vector_write_mask() != 0);
  bool vector_sets_p0 = vop >= AluVectorOpcode::kSetpEqPush && vop <= AluVectorOpcode::kSetpGePush;
  bool scalar_sets_p0 = sop >= AluScalarOpcode::kSetpEq && sop <= AluScalarOpcode::kSetpRstr;

  // Operands are all read before anything is written (co-issue).
  if (do_vector) {
    std::string a = AluOperand(alu, 1);
    std::string b = AluOperand(alu, 2);
    std::string c = AluOperand(alu, 3);
    std::string e;
    switch (vop) {
      case AluVectorOpcode::kAdd: e = a + " + " + b; break;
      case AluVectorOpcode::kMul: e = "xe_mul(" + a + ", " + b + ")"; break;
      case AluVectorOpcode::kMax: e = "xe_max(" + a + ", " + b + ")"; break;
      case AluVectorOpcode::kMin: e = "xe_min(" + a + ", " + b + ")"; break;
      case AluVectorOpcode::kSeq: e = "float4(" + a + " == " + b + ")"; break;
      case AluVectorOpcode::kSgt: e = "float4(" + a + " > " + b + ")"; break;
      case AluVectorOpcode::kSge: e = "float4(" + a + " >= " + b + ")"; break;
      case AluVectorOpcode::kSne: e = "float4(" + a + " != " + b + ")"; break;
      case AluVectorOpcode::kFrc: e = "frac(" + a + ")"; break;
      case AluVectorOpcode::kTrunc: e = "trunc(" + a + ")"; break;
      case AluVectorOpcode::kFloor: e = "floor(" + a + ")"; break;
      case AluVectorOpcode::kMad: e = "xe_mul(" + a + ", " + b + ") + " + c; break;
      case AluVectorOpcode::kCndEq: e = "xe_sel(" + a + " == 0.0, " + b + ", " + c + ")"; break;
      case AluVectorOpcode::kCndGe: e = "xe_sel(" + a + " >= 0.0, " + b + ", " + c + ")"; break;
      case AluVectorOpcode::kCndGt: e = "xe_sel(" + a + " > 0.0, " + b + ", " + c + ")"; break;
      case AluVectorOpcode::kDp4: e = "xe_dp4(" + a + ", " + b + ").xxxx"; break;
      case AluVectorOpcode::kDp3: e = "xe_dp3(" + a + ", " + b + ").xxxx"; break;
      case AluVectorOpcode::kDp2Add: e = "(xe_dp2(" + a + ", " + b + ") + (" + c + ").x).xxxx"; break;
      case AluVectorOpcode::kCube: e = "xe_cube(" + a + ", " + b + ")"; uses_cube_ = true; break;
      case AluVectorOpcode::kMax4: e = "xe_max4(" + a + ")"; break;
      case AluVectorOpcode::kSetpEqPush:
      case AluVectorOpcode::kSetpNePush:
      case AluVectorOpcode::kSetpGtPush:
      case AluVectorOpcode::kSetpGePush: {
        const char* op = vop == AluVectorOpcode::kSetpEqPush   ? "=="
                         : vop == AluVectorOpcode::kSetpNePush ? "!="
                         : vop == AluVectorOpcode::kSetpGtPush ? ">"
                                                                : ">=";
        Line("float4 xe_pa = " + a + ", xe_pb = " + b + ";");
        Line(Format("bool xe_vp = xe_pa.w == 0.0 && xe_pb.w %s 0.0;", op));
        e = Format("((xe_pa.x == 0.0 && xe_pb.x %s 0.0) ? 0.0 : xe_pa.x + 1.0).xxxx", op);
        break;
      }
      case AluVectorOpcode::kKillEq:
      case AluVectorOpcode::kKillGt:
      case AluVectorOpcode::kKillGe:
      case AluVectorOpcode::kKillNe: {
        const char* op = vop == AluVectorOpcode::kKillEq   ? "=="
                         : vop == AluVectorOpcode::kKillGt ? ">"
                         : vop == AluVectorOpcode::kKillGe ? ">="
                                                            : "!=";
        Line(Format("bool xe_vkill = any(%s %s %s);", a.c_str(), op, b.c_str()));
        e = "float4(xe_vkill.xxxx)";
        break;
      }
      case AluVectorOpcode::kDst:
        Line("float4 xe_da = " + a + ", xe_db = " + b + ";");
        e = "float4(1.0, xe_mul(xe_da.y, xe_db.y), xe_da.z, xe_db.w)";
        break;
      case AluVectorOpcode::kMaxA:
        Line("float4 xe_ma = " + a + ", xe_mb = " + b + ";");
        Line("int xe_new_a0 = (int)clamp(floor(xe_ma.w + 0.5), -256.0, 255.0);");
        e = "xe_max(xe_ma, xe_mb)";
        break;
      default:
        e = "float4(0.0, 0.0, 0.0, 0.0)";
        break;
    }
    if (alu.vector_clamp()) e = "saturate(" + e + ")";
    Line("float4 xe_v = " + e + ";");
  }

  bool do_scalar = sop != AluScalarOpcode::kRetainPrev || smask != 0;
  if (do_scalar) {
    // Scalar operands: a = W-relative component, b = X-relative component of src3.
    std::string a, b;
    bool const_scalar = sop >= AluScalarOpcode::kMulsc0 && sop <= AluScalarOpcode::kSubsc1;
    uint32_t swz = alu.src_swizzle(3);
    char ca = kComp[((swz >> 6) + 3) & 3];
    char cb = kComp[swz & 3];
    if (const_scalar) {
      std::string cst = ConstRef(alu.src_reg(3), alu.src_const_is_addressed(3), alu.is_const_address_register_relative());
      std::string tmp = Temp(alu.scalar_const_reg_op_src_temp_reg(), false);
      if (alu.abs_constants()) {
        cst = "abs(" + cst + ")";
        tmp = "abs(" + tmp + ")";
      }
      a = cst + "." + ca;
      b = tmp + "." + cb;
      if (alu.src_negate(3)) {
        a = "(-" + a + ")";
        b = "(-" + b + ")";
      }
    } else {
      std::string src;
      bool abs;
      uint32_t reg = alu.src_reg(3);
      if (alu.src_is_temp(3)) {
        src = Temp(AluInstruction::src_temp_reg(reg), AluInstruction::is_src_temp_relative(reg));
        abs = AluInstruction::is_src_temp_value_absolute(reg);
      } else {
        src = ConstRef(reg, alu.src_const_is_addressed(3), alu.is_const_address_register_relative());
        abs = alu.abs_constants();
      }
      if (abs) src = "abs(" + src + ")";
      a = src + "." + ca;
      b = src + "." + cb;
      if (alu.src_negate(3)) {
        a = "(-" + a + ")";
        b = "(-" + b + ")";
      }
    }
    Line("float xe_sa = " + a + ", xe_sb = " + b + ";");
    std::string e;
    bool sets_p0 = false;
    switch (sop) {
      case AluScalarOpcode::kAdds: e = "xe_sa + xe_sb"; break;
      case AluScalarOpcode::kAddsPrev: e = "xe_sa + xe_ps"; break;
      case AluScalarOpcode::kMuls: e = "xe_mul(xe_sa, xe_sb)"; break;
      case AluScalarOpcode::kMulsPrev: e = "xe_mul(xe_sa, xe_ps)"; break;
      case AluScalarOpcode::kMulsPrev2:
        e = "((xe_ps == -XE_FLT_MAX || !xe_finite(xe_ps) || !xe_finite(xe_sb) || xe_sb <= 0.0) ? -XE_FLT_MAX : xe_mul(xe_sa, xe_ps))";
        break;
      case AluScalarOpcode::kMaxs: e = "(xe_sa >= xe_sb ? xe_sa : xe_sb)"; break;
      case AluScalarOpcode::kMins: e = "(xe_sa < xe_sb ? xe_sa : xe_sb)"; break;
      case AluScalarOpcode::kSeqs: e = "float(xe_sa == 0.0)"; break;
      case AluScalarOpcode::kSgts: e = "float(xe_sa > 0.0)"; break;
      case AluScalarOpcode::kSges: e = "float(xe_sa >= 0.0)"; break;
      case AluScalarOpcode::kSnes: e = "float(xe_sa != 0.0)"; break;
      case AluScalarOpcode::kFrcs: e = "frac(xe_sa)"; break;
      case AluScalarOpcode::kTruncs: e = "trunc(xe_sa)"; break;
      case AluScalarOpcode::kFloors: e = "floor(xe_sa)"; break;
      case AluScalarOpcode::kExp: e = "exp2(xe_sa)"; break;
      case AluScalarOpcode::kLogc: e = "xe_logc(xe_sa)"; break;
      case AluScalarOpcode::kLog: e = "log2(xe_sa)"; break;
      case AluScalarOpcode::kRcpc: e = "xe_clamp_max(1.0 / xe_sa)"; break;
      case AluScalarOpcode::kRcpf: e = "xe_clamp_zero(1.0 / xe_sa)"; break;
      case AluScalarOpcode::kRcp: e = "(1.0 / xe_sa)"; break;
      case AluScalarOpcode::kRsqc: e = "xe_clamp_max(rsqrt(xe_sa))"; break;
      case AluScalarOpcode::kRsqf: e = "xe_clamp_zero(rsqrt(xe_sa))"; break;
      case AluScalarOpcode::kRsq: e = "rsqrt(xe_sa)"; break;
      case AluScalarOpcode::kMaxAs:
        Line("int xe_new_a0 = (int)clamp(floor(xe_sa + 0.5), -256.0, 255.0);");
        e = "(xe_sa >= xe_sb ? xe_sa : xe_sb)";
        break;
      case AluScalarOpcode::kMaxAsf:
        Line("int xe_new_a0 = (int)clamp(floor(xe_sa), -256.0, 255.0);");
        e = "(xe_sa >= xe_sb ? xe_sa : xe_sb)";
        break;
      case AluScalarOpcode::kSubs: e = "xe_sa - xe_sb"; break;
      case AluScalarOpcode::kSubsPrev: e = "xe_sa - xe_ps"; break;
      case AluScalarOpcode::kSetpEq: Line("bool xe_sp = xe_sa == 0.0;"); e = "(xe_sp ? 0.0 : 1.0)"; sets_p0 = true; break;
      case AluScalarOpcode::kSetpNe: Line("bool xe_sp = xe_sa != 0.0;"); e = "(xe_sp ? 0.0 : 1.0)"; sets_p0 = true; break;
      case AluScalarOpcode::kSetpGt: Line("bool xe_sp = xe_sa > 0.0;"); e = "(xe_sp ? 0.0 : 1.0)"; sets_p0 = true; break;
      case AluScalarOpcode::kSetpGe: Line("bool xe_sp = xe_sa >= 0.0;"); e = "(xe_sp ? 0.0 : 1.0)"; sets_p0 = true; break;
      case AluScalarOpcode::kSetpInv:
        Line("bool xe_sp = xe_sa == 1.0;");
        e = "(xe_sp ? 0.0 : (xe_sa == 0.0 ? 1.0 : xe_sa))";
        sets_p0 = true;
        break;
      case AluScalarOpcode::kSetpPop:
        Line("bool xe_sp = xe_sa - 1.0 <= 0.0;");
        e = "(xe_sp ? 0.0 : xe_sa - 1.0)";
        sets_p0 = true;
        break;
      case AluScalarOpcode::kSetpClr:
        Line("bool xe_sp = false;");
        e = "XE_FLT_MAX";
        sets_p0 = true;
        break;
      case AluScalarOpcode::kSetpRstr:
        Line("bool xe_sp = xe_sa == 0.0;");
        e = "(xe_sp ? 0.0 : xe_sa)";
        sets_p0 = true;
        break;
      case AluScalarOpcode::kKillsEq: Line("bool xe_skill = xe_sa == 0.0;"); e = "float(xe_skill)"; break;
      case AluScalarOpcode::kKillsGt: Line("bool xe_skill = xe_sa > 0.0;"); e = "float(xe_skill)"; break;
      case AluScalarOpcode::kKillsGe: Line("bool xe_skill = xe_sa >= 0.0;"); e = "float(xe_skill)"; break;
      case AluScalarOpcode::kKillsNe: Line("bool xe_skill = xe_sa != 0.0;"); e = "float(xe_skill)"; break;
      case AluScalarOpcode::kKillsOne: Line("bool xe_skill = xe_sa == 1.0;"); e = "float(xe_skill)"; break;
      case AluScalarOpcode::kSqrt: e = "sqrt(xe_sa)"; break;
      case AluScalarOpcode::kMulsc0:
      case AluScalarOpcode::kMulsc1: e = "xe_mul(xe_sa, xe_sb)"; break;
      case AluScalarOpcode::kAddsc0:
      case AluScalarOpcode::kAddsc1: e = "xe_sa + xe_sb"; break;
      case AluScalarOpcode::kSubsc0:
      case AluScalarOpcode::kSubsc1: e = "xe_sa - xe_sb"; break;
      case AluScalarOpcode::kSin: e = "sin(xe_sa)"; break;
      case AluScalarOpcode::kCos: e = "cos(xe_sa)"; break;
      case AluScalarOpcode::kRetainPrev: e = "xe_ps"; break;
      default: e = "0.0"; break;
    }
    (void)sets_p0;
    if (alu.scalar_clamp()) e = "saturate(" + e + ")";
    Line("float xe_s = " + e + ";");
  }

  // --- writes ---
  if (is_export) {
    uint32_t dest = alu.vector_dest();
    uint32_t zero_mask = alu.GetConstant0WriteMask();
    uint32_t one_mask = alu.GetConstant1WriteMask();
    std::string target;
    bool memexport_target = false;
    if (is_ps_) {
      if (dest <= 3) {
        target = Format("oC%u", dest);
        out_.color_outputs |= 1u << dest;
      } else if (dest == 61) {
        target = "oDepth4";
        out_.writes_depth = true;
      }
    } else {
      if (dest < 16) target = Format("oI[%u]", dest);
      else if (dest == 62) target = "oPos";
      else if (dest == 63) target = "oPts";
    }
    if (dest == kExportAddress) {
      target = "xe_eA";
      memexport_target = true;
    } else if (dest >= kExportData0 && dest < kExportData0 + 5) {
      target = Format("xe_eM[%u]", dest - kExportData0);
      memexport_target = true;
    }
    if (!target.empty()) {
      for (uint32_t c = 0; c < 4; c++) {
        uint32_t bit = 1u << c;
        if (vmask & bit) Line(Format("%s.%c = xe_v.%c;", target.c_str(), kComp[c], kComp[c]));
        else if (smask & bit) Line(Format("%s.%c = xe_s;", target.c_str(), kComp[c]));
        else if (one_mask & bit) Line(Format("%s.%c = 1.0;", target.c_str(), kComp[c]));
        else if (zero_mask & bit) Line(Format("%s.%c = 0.0;", target.c_str(), kComp[c]));
      }
      if (memexport_target && dest != kExportAddress) {
        Line(Format("xe_eM_written |= %uu;", 1u << (dest - kExportData0)));
      }
    }
  } else {
    if (vmask) {
      std::string m;
      for (uint32_t c = 0; c < 4; c++)
        if (vmask & (1u << c)) m += kComp[c];
      Line(Format("%s.%s = xe_v.%s;", Temp(alu.vector_dest(), alu.is_vector_dest_relative()).c_str(), m.c_str(),
                  m.c_str()));
    }
    if (smask) {
      std::string m;
      for (uint32_t c = 0; c < 4; c++)
        if (smask & (1u << c)) m += kComp[c];
      Line(Format("%s.%s = xe_s;", Temp(alu.scalar_dest(), alu.is_scalar_dest_relative()).c_str(), m.c_str()));
    }
  }
  if (do_scalar && sop != AluScalarOpcode::kRetainPrev) Line("xe_ps = xe_s;");
  if (vop == AluVectorOpcode::kMaxA || sop == AluScalarOpcode::kMaxAs || sop == AluScalarOpcode::kMaxAsf) {
    if (do_vector || do_scalar) Line("a0 = xe_new_a0;");
  }
  if (vector_sets_p0 && do_vector) Line("p0 = xe_vp;");
  if (scalar_sets_p0 && do_scalar) Line("p0 = xe_sp;");
  if (vop >= AluVectorOpcode::kKillEq && vop <= AluVectorOpcode::kKillNe && do_vector) {
    uses_kill_ = true;
    if (is_ps_) Line("if (xe_vkill) discard;");
  }
  if (sop >= AluScalarOpcode::kKillsEq && sop <= AluScalarOpcode::kKillsOne && do_scalar) {
    uses_kill_ = true;
    if (is_ps_) Line("if (xe_skill) discard;");
  }
  indent_--;
  Line("}");
}

void Translator::EmitVertexFetch(const VertexFetchInstruction& vf, uint32_t address) {
  if (vf.is_predicated()) {
    Line(Format("if (p0 == %s) {", vf.predicate_condition() ? "true" : "false"));
  } else {
    Line("{");
  }
  indent_++;
  if (!vf.is_mini_fetch()) {
    std::string src = Temp(vf.src(), vf.is_src_relative());
    char comp = kComp[vf.src_swizzle() & 3];
    if (vf.is_index_rounded()) Line(Format("xe_vindex = (int)floor(%s.%c + 0.5);", src.c_str(), comp));
    else Line(Format("xe_vindex = (int)floor(%s.%c);", src.c_str(), comp));
  }
  const VertexInputElement* in = nullptr;
  if (inputs_) {
    for (auto& e : *inputs_) {
      if (e.fetch_address == address) {
        in = &e;
        break;
      }
    }
  }
  std::string value;
  if (!in || !in->present) {
    value = "float4(0.0, 0.0, 0.0, 1.0)";
  } else {
    DeclTypeInfo t = DecodeDeclType(in->decl_type);
    uint32_t fc = 95 - (in->stream & 15);
    Line(Format("uint xe_vaddr = xe_vf_base(%u) + (uint)xe_vindex * xe_stride(%u) + %uu;", fc, in->stream & 15,
                in->offset_bytes));
    const char* mem = uses_memexport_ ? "XeMemRW" : "XeMem";
    uint32_t words = 1;
    switch (xenos::VertexFormat(t.format)) {
      case xenos::VertexFormat::k_16_16_16_16:
      case xenos::VertexFormat::k_16_16_16_16_FLOAT:
      case xenos::VertexFormat::k_32_32:
      case xenos::VertexFormat::k_32_32_FLOAT:
        words = 2;
        break;
      case xenos::VertexFormat::k_32_32_32_FLOAT:
        words = 3;
        break;
      case xenos::VertexFormat::k_32_32_32_32:
      case xenos::VertexFormat::k_32_32_32_32_FLOAT:
        words = 4;
        break;
      default:
        words = 1;
        break;
    }
    const char* loads[] = {"", "uint4(%s.Load(xe_vaddr), 0, 0, 0)", "uint4(%s.Load2(xe_vaddr), 0, 0)",
                           "uint4(%s.Load3(xe_vaddr), 0)", "%s.Load4(xe_vaddr)"};
    Line("uint4 xe_w = xe_swap4(" + Format(loads[words], mem) + Format(", %uu);", t.endian));
    const char* norm_u = t.is_integer ? "" : nullptr;
    (void)norm_u;
    std::string decode;
    bool sgn = t.is_signed, integer = t.is_integer;
    switch (xenos::VertexFormat(t.format)) {
      case xenos::VertexFormat::k_8_8_8_8:
        decode = Format("xe_unpack_8888(xe_w.x, %s, %s)", sgn ? "true" : "false", integer ? "true" : "false");
        break;
      case xenos::VertexFormat::k_2_10_10_10:
        decode = Format("xe_unpack_2101010(xe_w.x, %s, %s)", sgn ? "true" : "false", integer ? "true" : "false");
        break;
      case xenos::VertexFormat::k_10_11_11:
        decode = Format("xe_unpack_101111(xe_w.x, %s, %s, 11, 11, 10)", sgn ? "true" : "false",
                        integer ? "true" : "false");
        break;
      case xenos::VertexFormat::k_11_11_10:
        decode = Format("xe_unpack_101111(xe_w.x, %s, %s, 10, 11, 11)", sgn ? "true" : "false",
                        integer ? "true" : "false");
        break;
      case xenos::VertexFormat::k_16_16:
        decode = Format("float4(xe_unpack_16(xe_w.x, %s, %s), 0.0, 0.0)", sgn ? "true" : "false",
                        integer ? "true" : "false");
        break;
      case xenos::VertexFormat::k_16_16_16_16:
        decode = Format("float4(xe_unpack_16(xe_w.x, %s, %s), xe_unpack_16(xe_w.y, %s, %s))", sgn ? "true" : "false",
                        integer ? "true" : "false", sgn ? "true" : "false", integer ? "true" : "false");
        break;
      case xenos::VertexFormat::k_16_16_FLOAT:
        decode = "float4(f16tof32(xe_w.x & 0xFFFF), f16tof32(xe_w.x >> 16), 0.0, 0.0)";
        break;
      case xenos::VertexFormat::k_16_16_16_16_FLOAT:
        decode =
            "float4(f16tof32(xe_w.x & 0xFFFF), f16tof32(xe_w.x >> 16), f16tof32(xe_w.y & 0xFFFF), f16tof32(xe_w.y >> 16))";
        break;
      case xenos::VertexFormat::k_32:
      case xenos::VertexFormat::k_32_32:
      case xenos::VertexFormat::k_32_32_32_32:
        decode = Format("xe_unpack_32(xe_w, %s, %s)", sgn ? "true" : "false", integer ? "true" : "false");
        break;
      case xenos::VertexFormat::k_32_FLOAT:
      case xenos::VertexFormat::k_32_32_FLOAT:
      case xenos::VertexFormat::k_32_32_32_FLOAT:
      case xenos::VertexFormat::k_32_32_32_32_FLOAT:
        decode = "asfloat(xe_w)";
        break;
      default:
        decode = "float4(0.0, 0.0, 0.0, 0.0)";
        break;
    }
    // Missing components of a format read as 0 except W = 1, then the declaration swizzle.
    uint32_t ncomp = 4;
    switch (xenos::VertexFormat(t.format)) {
      case xenos::VertexFormat::k_32:
      case xenos::VertexFormat::k_32_FLOAT: ncomp = 1; break;
      case xenos::VertexFormat::k_16_16:
      case xenos::VertexFormat::k_16_16_FLOAT:
      case xenos::VertexFormat::k_32_32:
      case xenos::VertexFormat::k_32_32_FLOAT: ncomp = 2; break;
      case xenos::VertexFormat::k_10_11_11:
      case xenos::VertexFormat::k_11_11_10:
      case xenos::VertexFormat::k_32_32_32_FLOAT: ncomp = 3; break;
      default: break;
    }
    Line("float4 xe_raw = " + decode + ";");
    if (ncomp < 4) Line("xe_raw.w = 1.0;");
    if (ncomp < 3) Line("xe_raw.z = 0.0;");
    if (ncomp < 2) Line("xe_raw.y = 0.0;");
    std::string swz;
    for (uint32_t c = 0; c < 4; c++) {
      uint32_t sel = (t.swizzle >> (c * 3)) & 7;
      if (sel < 4) swz += Format("xe_raw.%c", kComp[sel]);
      else if (sel == 4) swz += "0.0";
      else swz += "1.0";
      if (c < 3) swz += ", ";
    }
    value = "float4(" + swz + ")";
  }
  Line("float4 xe_f = " + value + ";");
  std::string dst = Temp(vf.dest(), vf.is_dest_relative());
  uint32_t dswz = vf.dest_swizzle();
  for (uint32_t c = 0; c < 4; c++) {
    uint32_t sel = (dswz >> (c * 3)) & 7;
    if (sel == 7) continue;
    if (sel < 4) Line(Format("%s.%c = xe_f.%c;", dst.c_str(), kComp[c], kComp[sel]));
    else if (sel == 4) Line(Format("%s.%c = 0.0;", dst.c_str(), kComp[c]));
    else if (sel == 5) Line(Format("%s.%c = 1.0;", dst.c_str(), kComp[c]));
  }
  indent_--;
  Line("}");
}

void Translator::EmitTextureFetch(const TextureFetchInstruction& tf) {
  FetchOpcode op = tf.opcode();
  if (tf.is_predicated()) {
    Line(Format("if (p0 == %s) {", tf.predicate_condition() ? "true" : "false"));
  } else {
    Line("{");
  }
  indent_++;
  std::string src = Temp(tf.src(), tf.is_src_relative());
  uint32_t sswz = tf.src_swizzle();
  std::string coord = Format("%s.%c%c%c", src.c_str(), kComp[sswz & 3], kComp[(sswz >> 2) & 3], kComp[(sswz >> 4) & 3]);
  if (op == FetchOpcode::kSetTextureLod) {
    Line(Format("xe_lod = %s.%c;", src.c_str(), kComp[sswz & 3]));
    indent_--;
    Line("}");
    return;
  }
  if (op == FetchOpcode::kSetTextureGradientsHorz || op == FetchOpcode::kSetTextureGradientsVert) {
    Line(Format("xe_grad_%s = %s;", op == FetchOpcode::kSetTextureGradientsHorz ? "h" : "v", coord.c_str()));
    indent_--;
    Line("}");
    return;
  }
  uint32_t slot = tf.fetch_constant_index();
  uint32_t dim = uint32_t(tf.dimension());
  std::string result;
  if (op == FetchOpcode::kGetTextureGradients) {
    if (is_ps_) {
      Line("float2 xe_gc = " + coord + ".xy;");
      result = "float4(ddx_coarse(xe_gc.x), ddy_coarse(xe_gc.x), ddx_coarse(xe_gc.y), ddy_coarse(xe_gc.y))";
    } else {
      result = "float4(0.0, 0.0, 0.0, 0.0)";
    }
  } else if (op == FetchOpcode::kGetTextureBorderColorFrac) {
    result = "float4(0.0, 0.0, 0.0, 0.0)";
  } else {
    uint32_t b = BindTexture(slot, dim);
    Line("float3 xe_tc = " + coord + ";");
    // Unnormalized coordinates and texel offsets are in texels of the base level.
    if (tf.unnormalized_coordinates()) Line(Format("xe_tc /= XTexDim[%u].xyz;", slot));
    float ox = tf.offset_x(), oy = tf.offset_y(), oz = tf.offset_z();
    if (ox != 0.0f || oy != 0.0f || (dim == 2 && oz != 0.0f)) {
      Line(Format("xe_tc += float3(%.6f, %.6f, %.6f) / XTexDim[%u].xyz;", ox, oy, dim == 2 ? oz : 0.0f, slot));
    }
    std::string tex = Format("XeT%u", b);
    std::string smp = Format("XeS%u", b);
    std::string c;
    switch (dim) {
      case 0: c = "float2(xe_tc.x, 0.5)"; break;
      case 1: c = "xe_tc.xy"; break;
      case 2: c = "xe_tc"; break;
      default: c = "xe_cube_dir(xe_tc)"; break;
    }
    if (op == FetchOpcode::kGetTextureComputedLod) {
      if (is_ps_ && dim != 3) result = Format("%s.CalculateLevelOfDetailUnclamped(%s, %s).xxxx", tex.c_str(), smp.c_str(), c.c_str());
      else result = "float4(0.0, 0.0, 0.0, 0.0)";
    } else if (op == FetchOpcode::kGetTextureWeights) {
      if (dim == 1) result = Format("float4(frac(xe_tc.xy * XTexDim[%u].xy - 0.5), 0.0, 0.0)", slot);
      else result = Format("float4(frac(xe_tc * XTexDim[%u].xyz - 0.5), 0.0)", slot);
    } else {
      float bias = tf.lod_bias();
      auto sample_from = [&](const std::string& t, const std::string& coords) {
        if (!is_ps_) {
          return Format("%s.SampleLevel(%s, %s, %s)", t.c_str(), smp.c_str(), coords.c_str(),
                        tf.use_register_lod() ? "xe_lod" : Format("%.6f", bias).c_str());
        }
        if (tf.use_register_lod() && !tf.use_computed_lod()) {
          return Format("%s.SampleLevel(%s, %s, xe_lod + %.6f)", t.c_str(), smp.c_str(), coords.c_str(), bias);
        }
        if (tf.use_register_lod() || bias != 0.0f) {
          return Format("%s.SampleBias(%s, %s, %s%.6f)", t.c_str(), smp.c_str(), coords.c_str(),
                        tf.use_register_lod() ? "xe_lod + " : "", bias);
        }
        return Format("%s.Sample(%s, %s)", t.c_str(), smp.c_str(), coords.c_str());
      };
      std::string sample = sample_from(tex, c);
      if (dim == 2) {
        // The fetch constant decides between a volume and a stack of 2D layers (normalized z
        // selects the layer).
        std::string layer = Format("float3(xe_tc.xy, xe_tc.z * XTexDim[%u].z - 0.5)", slot);
        sample = Format("(XTex[%u].z != 0u ? %s : %s)", slot, sample_from(Format("XeTA%u", b), layer).c_str(),
                        sample.c_str());
      }
      result = Format("xe_texfmt(%s, %u)", sample.c_str(), slot);
    }
  }
  Line("float4 xe_t = " + result + ";");
  std::string dst = Temp(tf.dest(), tf.is_dest_relative());
  uint32_t dswz = tf.dest_swizzle();
  for (uint32_t c = 0; c < 4; c++) {
    uint32_t sel = (dswz >> (c * 3)) & 7;
    if (sel == 7) continue;
    if (sel < 4) Line(Format("%s.%c = xe_t.%c;", dst.c_str(), kComp[c], kComp[sel]));
    else if (sel == 4) Line(Format("%s.%c = 0.0;", dst.c_str(), kComp[c]));
    else if (sel == 5) Line(Format("%s.%c = 1.0;", dst.c_str(), kComp[c]));
  }
  indent_--;
  Line("}");
}

void Translator::EmitExec(uint32_t address, uint32_t count, uint32_t sequence) {
  for (uint32_t i = 0; i < count; i++, sequence >>= 2) {
    uint32_t at = (address + i) * 3;
    if (at + 2 >= s_.ucode.size()) break;
    const uint32_t* w = &s_.ucode[at];
    if (sequence & 1) {
      FetchInstruction f;
      std::memcpy(&f, w, sizeof(f));
      if (f.opcode() == FetchOpcode::kVertexFetch) {
        if (!is_ps_) EmitVertexFetch(f.vertex_fetch(), address + i);
      } else {
        EmitTextureFetch(f.texture_fetch());
      }
    } else {
      AluInstruction a;
      std::memcpy(&a, w, sizeof(a));
      EmitAlu(a);
    }
  }
}

void Translator::EmitCfInstruction(uint32_t index, const ControlFlowInstruction& cf, bool general) {
  auto next = [&]() {
    if (general) Line(Format("xe_npc = %uu;", index + 1));
  };
  auto finish = [&]() {
    if (uses_memexport_) Line("xe_memexport_flush(xe_eA, xe_eM, xe_eM_written); xe_eM_written = 0u;");
    Line("xe_exit = true;");
  };
  auto bool_cond = [&](uint32_t b, bool cond) {
    return Format("(xe_bool(%u) == %s)", b, cond ? "true" : "false");
  };
  switch (cf.opcode()) {
    case ControlFlowOpcode::kExec:
    case ControlFlowOpcode::kExecEnd: {
      bool end = cf.opcode() == ControlFlowOpcode::kExecEnd;
      EmitExec(cf.exec.address(), cf.exec.count(), cf.exec.sequence());
      if (end) finish();
      else next();
      break;
    }
    case ControlFlowOpcode::kCondExec:
    case ControlFlowOpcode::kCondExecEnd:
    case ControlFlowOpcode::kCondExecPredClean:
    case ControlFlowOpcode::kCondExecPredCleanEnd: {
      bool end = DoesControlFlowOpcodeEndShader(cf.opcode());
      Line("if " + bool_cond(cf.cond_exec.bool_address(), cf.cond_exec.condition()) + " {");
      indent_++;
      EmitExec(cf.cond_exec.address(), cf.cond_exec.count(), cf.cond_exec.sequence());
      if (end) finish();
      indent_--;
      Line("}");
      if (!end || general) next();
      break;
    }
    case ControlFlowOpcode::kCondExecPred:
    case ControlFlowOpcode::kCondExecPredEnd: {
      bool end = cf.opcode() == ControlFlowOpcode::kCondExecPredEnd;
      Line(Format("if (p0 == %s) {", cf.cond_exec_pred.condition() ? "true" : "false"));
      indent_++;
      EmitExec(cf.cond_exec_pred.address(), cf.cond_exec_pred.count(), cf.cond_exec_pred.sequence());
      if (end) finish();
      indent_--;
      Line("}");
      next();
      break;
    }
    case ControlFlowOpcode::kLoopStart: {
      uint32_t id = cf.loop_start.loop_id();
      Line(Format("{ uint xe_lc = xe_loop(%u);", id));
      Line("  xe_ls_aL[xe_lsp & 3] = aL; xe_ls_cnt[xe_lsp & 3] = xe_lc & 0xFF; xe_lsp++;");
      if (!cf.loop_start.is_repeat()) Line("  aL = (int)((xe_lc >> 8) & 0xFF);");
      Line(Format("  if ((xe_lc & 0xFF) == 0) { xe_lsp--; aL = xe_ls_aL[xe_lsp & 3]; xe_npc = %uu; } else { xe_npc = %uu; } }",
                  cf.loop_start.address(), index + 1));
      break;
    }
    case ControlFlowOpcode::kLoopEnd: {
      uint32_t id = cf.loop_end.loop_id();
      Line(Format("{ uint xe_lc = xe_loop(%u);", id));
      Line("  uint xe_li = (xe_lsp - 1) & 3;");
      Line("  xe_ls_cnt[xe_li] = xe_ls_cnt[xe_li] - 1;");
      Line("  aL += ((int)(xe_lc << 8)) >> 24;");
      std::string brk = "false";
      if (cf.loop_end.is_predicated_break()) brk = Format("(p0 == %s)", cf.loop_end.condition() ? "true" : "false");
      Line(Format("  if (xe_ls_cnt[xe_li] != 0 && !%s) { xe_npc = %uu; } else { xe_lsp--; aL = xe_ls_aL[xe_lsp & 3]; xe_npc = %uu; } }",
                  brk.c_str(), cf.loop_end.address(), index + 1));
      break;
    }
    case ControlFlowOpcode::kCondCall: {
      std::string cond = "true";
      if (!cf.cond_call.is_unconditional()) {
        cond = cf.cond_call.is_predicated() ? Format("(p0 == %s)", cf.cond_call.condition() ? "true" : "false")
                                            : bool_cond(cf.cond_call.bool_address(), cf.cond_call.condition());
      }
      Line(Format("if (%s) { xe_cs[xe_csp & 3] = %uu; xe_csp++; xe_npc = %uu; } else { xe_npc = %uu; }", cond.c_str(),
                  index + 1, cf.cond_call.address(), index + 1));
      break;
    }
    case ControlFlowOpcode::kReturn:
      Line(Format("if (xe_csp != 0) { xe_csp--; xe_npc = xe_cs[xe_csp & 3]; } else { xe_npc = %uu; }", index + 1));
      break;
    case ControlFlowOpcode::kCondJmp: {
      std::string cond = "true";
      if (!cf.cond_jmp.is_unconditional()) {
        cond = cf.cond_jmp.is_predicated() ? Format("(p0 == %s)", cf.cond_jmp.condition() ? "true" : "false")
                                           : bool_cond(cf.cond_jmp.bool_address(), cf.cond_jmp.condition());
      }
      Line(Format("if (%s) { xe_npc = %uu; } else { xe_npc = %uu; }", cond.c_str(), cf.cond_jmp.address(), index + 1));
      break;
    }
    case ControlFlowOpcode::kAlloc:
      if (uses_memexport_) Line("xe_memexport_flush(xe_eA, xe_eM, xe_eM_written); xe_eM_written = 0u;");
      next();
      break;
    default:
      next();
      break;
  }
}

const char* kPrelude = R"HLSL(
#define XE_FLT_MAX asfloat(0x7F7FFFFFu)
cbuffer XeFloat : register(b0) { float4 XC[256]; };
cbuffer XeState : register(b1) {
  uint4 XB[2];
  uint4 XL[8];
  uint4 XVF[48];
  uint4 XStride[4];
  uint4 XTex[32];
  float4 XTexDim[32];
  float4 XNdc;
  float4 XAlpha;
  float4 XRtBias;
  uint4 XFlags;
  float4 XUcp[6];
  float4 XZ;
};
float4 xe_c(int i) { return (uint)i < 256u ? XC[i] : float4(0.0, 0.0, 0.0, 0.0); }
bool xe_bool(uint b) { return ((XB[b >> 7][(b >> 5) & 3] >> (b & 31)) & 1u) != 0u; }
uint xe_loop(uint i) { return XL[i >> 2][i & 3]; }
float4 xe_mul(float4 a, float4 b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float xe_mul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float4 xe_sel(bool4 c, float4 a, float4 b) { return c ? a : b; }
float4 xe_max(float4 a, float4 b) { return a >= b ? a : b; }
float4 xe_min(float4 a, float4 b) { return a < b ? a : b; }
float xe_dp4(float4 a, float4 b) { float4 m = xe_mul(a, b); return m.x + m.y + m.z + m.w; }
float xe_dp3(float4 a, float4 b) { float4 m = xe_mul(a, b); return m.x + m.y + m.z; }
float xe_dp2(float4 a, float4 b) { float4 m = xe_mul(a, b); return m.x + m.y; }
bool xe_finite(float v) { return (asuint(v) & 0x7F800000u) != 0x7F800000u; }
float xe_logc(float v) { float t = log2(v); return t == asfloat(0xFF800000u) ? -XE_FLT_MAX : t; }
float xe_clamp_max(float v) { return clamp(v, -XE_FLT_MAX, XE_FLT_MAX); }
float xe_clamp_zero(float v) { return xe_finite(v) ? v : (v > 0.0 ? 0.0 : -0.0); }
float4 xe_max4(float4 v) {
  if (v.x >= v.y && v.x >= v.z && v.x >= v.w) return v.xxxx;
  if (v.y >= v.z && v.y >= v.w) return v.yyyy;
  if (v.z >= v.w) return v.zzzz;
  return v.wwww;
}
float4 xe_cube(float4 s0, float4 s1) {
  // Operands arrive as src0.zzxy / src1.yxzz; reconstruct the vector.
  float3 v = float3(s0.z, s1.x, s0.x);
  float3 a = abs(v);
  float tc, sc, ma, id;
  if (a.z >= a.x && a.z >= a.y) { tc = -v.y; sc = v.z < 0.0 ? -v.x : v.x; ma = 2.0 * v.z; id = v.z < 0.0 ? 5.0 : 4.0; }
  else if (a.y >= a.x) { tc = v.y < 0.0 ? -v.z : v.z; sc = v.x; ma = 2.0 * v.y; id = v.y < 0.0 ? 3.0 : 2.0; }
  else { tc = -v.y; sc = v.x < 0.0 ? v.z : -v.z; ma = 2.0 * v.x; id = v.x < 0.0 ? 1.0 : 0.0; }
  return float4(tc, sc, ma, id);
}
float3 xe_cube_dir(float3 c) {
  // c = (s + 1.5-ish, t + 1.5-ish, face) as produced by cube + rcp + mad 1.5.
  float s = (c.x - 1.5) * 2.0, t = (c.y - 1.5) * 2.0;
  uint f = (uint)clamp(c.z + 0.5, 0.0, 5.0);
  if (f == 0) return float3(1.0, -t, -s);
  if (f == 1) return float3(-1.0, -t, s);
  if (f == 2) return float3(s, 1.0, t);
  if (f == 3) return float3(s, -1.0, -t);
  if (f == 4) return float3(s, -t, 1.0);
  return float3(-s, -t, -1.0);
}
float xe_pwl_to_linear(float g) {
  g = saturate(g);
  if (g < 0.25) return g * 0.25;
  if (g < 0.375) return 0.0625 + (g - 0.25) * 0.5;
  if (g < 0.75) return 0.125 + (g - 0.375);
  return 0.5 + (g - 0.75) * 2.0;
}
float4 xe_texfmt(float4 v, uint slot) {
  uint info = XTex[slot].x;
  [unroll] for (uint c = 0; c < 4; c++) {
    uint sg = (info >> (12 + 2 * c)) & 3u;
    if (sg == 2u) v[c] = v[c] * 2.0 - 1.0;
    else if (sg == 3u) v[c] = xe_pwl_to_linear(v[c]);
  }
  v *= asfloat(XTex[slot].y);
  float4 r;
  [unroll] for (uint d = 0; d < 4; d++) {
    uint s = (info >> (3 * d)) & 7u;
    r[d] = s < 4u ? v[s] : (s == 5u ? 1.0 : 0.0);
  }
  return r;
}
uint xe_swap(uint v, uint e) {
  if (e == 1u) return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
  if (e == 2u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
  if (e == 3u) return (v << 16) | (v >> 16);
  return v;
}
uint4 xe_swap4(uint4 v, uint e) { return uint4(xe_swap(v.x, e), xe_swap(v.y, e), xe_swap(v.z, e), xe_swap(v.w, e)); }
uint xe_vf_base(uint fc) { uint4 q = XVF[fc >> 1]; return ((fc & 1u) ? q.z : q.x) & 0x1FFFFFFCu; }
uint xe_stride(uint s) { return XStride[s >> 2][s & 3] * 4u; }
float xe_unorm(uint v, uint bits) { return float(v) / float((1u << bits) - 1u); }
float xe_snorm(uint v, uint bits) {
  int s = (int)(v << (32u - bits)) >> (32u - bits);
  return max(float(s) / float((1u << (bits - 1u)) - 1u), -1.0);
}
float xe_sint(uint v, uint bits) { return float((int)(v << (32u - bits)) >> (32u - bits)); }
float xe_comp(uint v, uint bits, bool sgn, bool integer) {
  if (integer) return sgn ? xe_sint(v, bits) : float(v);
  return sgn ? xe_snorm(v, bits) : xe_unorm(v, bits);
}
float4 xe_unpack_8888(uint w, bool sgn, bool integer) {
  return float4(xe_comp(w & 0xFFu, 8, sgn, integer), xe_comp((w >> 8) & 0xFFu, 8, sgn, integer),
                xe_comp((w >> 16) & 0xFFu, 8, sgn, integer), xe_comp(w >> 24, 8, sgn, integer));
}
float4 xe_unpack_2101010(uint w, bool sgn, bool integer) {
  return float4(xe_comp(w & 0x3FFu, 10, sgn, integer), xe_comp((w >> 10) & 0x3FFu, 10, sgn, integer),
                xe_comp((w >> 20) & 0x3FFu, 10, sgn, integer), xe_comp(w >> 30, 2, sgn, integer));
}
float4 xe_unpack_101111(uint w, bool sgn, bool integer, uint bx, uint by, uint bz) {
  uint x = w & ((1u << bx) - 1u), y = (w >> bx) & ((1u << by) - 1u), z = (w >> (bx + by)) & ((1u << bz) - 1u);
  return float4(xe_comp(x, bx, sgn, integer), xe_comp(y, by, sgn, integer), xe_comp(z, bz, sgn, integer), 1.0);
}
float2 xe_unpack_16(uint w, bool sgn, bool integer) {
  return float2(xe_comp(w & 0xFFFFu, 16, sgn, integer), xe_comp(w >> 16, 16, sgn, integer));
}
float4 xe_unpack_32(uint4 w, bool sgn, bool integer) {
  if (integer) return sgn ? float4(asint(w)) : float4(w);
  return sgn ? max(float4(asint(w)) / 2147483647.0, -1.0) : float4(w) / 4294967295.0;
}
)HLSL";

const char* kMemexportHelpers = R"HLSL(
RWByteAddressBuffer XeMemRW : register(u4);
uint xe_f32to16(float v) { return f32tof16(v); }
uint xe_pack_unorm(float v, uint bits) { return (uint)round(saturate(v) * float((1u << bits) - 1u)); }
uint xe_pack_snorm(float v, uint bits) {
  int i = (int)round(clamp(v, -1.0, 1.0) * float((1u << (bits - 1u)) - 1u));
  return (uint)i & ((1u << bits) - 1u);
}
uint xe_pack_comp(float v, uint bits, uint numfmt) {
  if (numfmt == 1u) return xe_pack_snorm(v, bits);
  if (numfmt == 2u) return (uint)clamp(v, 0.0, float((1u << bits) - 1u));
  if (numfmt == 3u) return (uint)(int)v & ((1u << bits) - 1u);
  return xe_pack_unorm(v, bits);
}
uint xe_swap128_word(uint v, uint e) {
  if (e == 1u) return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
  if (e == 2u || e == 4u || e == 5u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
  if (e == 3u) return (v << 16) | (v >> 16);
  return v;
}
void xe_memexport_flush(float4 eA, float4 eM[5], uint written) {
  if (written == 0u) return;
  uint4 a = asuint(eA);
  if (((a.y >> 23) & 0x1FFu) != 0x96u) return;
  uint base = (a.x & 0x3FFFFFFFu) << 2;
  uint index = a.y & 0x7FFFFFu;
  uint endian = a.z & 7u;
  uint fmt = (a.z >> 8) & 0x3Fu;
  uint numfmt = (a.z >> 16) & 7u;
  bool rbswap = ((a.z >> 19) & 1u) != 0u;
  // a.w (2^23 + n) is not an index limit: games size it smaller than what they export.
  [unroll] for (uint k = 0; k < 5; k++) {
    if ((written & (1u << k)) == 0u) continue;
    float4 v = eM[k];
    if (rbswap) v = v.zyxw;
    uint4 w = uint4(0, 0, 0, 0);
    uint size = 4u;
    if (fmt == 38u) { w = asuint(v); size = 16u; }
    else if (fmt == 37u) { w.xy = asuint(v.xy); size = 8u; }
    else if (fmt == 36u) { w.x = asuint(v.x); }
    else if (fmt == 32u) { w.x = xe_f32to16(v.x) | (xe_f32to16(v.y) << 16); w.y = xe_f32to16(v.z) | (xe_f32to16(v.w) << 16); size = 8u; }
    else if (fmt == 31u) { w.x = xe_f32to16(v.x) | (xe_f32to16(v.y) << 16); }
    else if (fmt == 30u) { w.x = xe_f32to16(v.x); size = 2u; }
    else if (fmt == 26u) { w.x = xe_pack_comp(v.x, 16, numfmt) | (xe_pack_comp(v.y, 16, numfmt) << 16);
                           w.y = xe_pack_comp(v.z, 16, numfmt) | (xe_pack_comp(v.w, 16, numfmt) << 16); size = 8u; }
    else if (fmt == 25u) { w.x = xe_pack_comp(v.x, 16, numfmt) | (xe_pack_comp(v.y, 16, numfmt) << 16); }
    else if (fmt == 16u) { w.x = xe_pack_comp(v.x, 11, numfmt) | (xe_pack_comp(v.y, 11, numfmt) << 11) |
                                 (xe_pack_comp(v.z, 10, numfmt) << 22); }
    else if (fmt == 17u) { w.x = xe_pack_comp(v.x, 10, numfmt) | (xe_pack_comp(v.y, 11, numfmt) << 10) |
                                 (xe_pack_comp(v.z, 11, numfmt) << 21); }
    else if (fmt == 7u) { w.x = xe_pack_comp(v.x, 10, numfmt) | (xe_pack_comp(v.y, 10, numfmt) << 10) |
                                (xe_pack_comp(v.z, 10, numfmt) << 20) | (xe_pack_comp(v.w, 2, numfmt) << 30); }
    else if (fmt == 6u || fmt == 14u) { w.x = xe_pack_comp(v.x, 8, numfmt) | (xe_pack_comp(v.y, 8, numfmt) << 8) |
                                              (xe_pack_comp(v.z, 8, numfmt) << 16) | (xe_pack_comp(v.w, 8, numfmt) << 24); }
    else continue;
    if (endian == 4u) w = w.yxwz;
    if (endian == 5u) w = w.wzyx;
    w = uint4(xe_swap128_word(w.x, endian), xe_swap128_word(w.y, endian), xe_swap128_word(w.z, endian), xe_swap128_word(w.w, endian));
    uint addr = base + (index + k) * size;
    if (size == 16u) XeMemRW.Store4(addr, w);
    else if (size == 8u) XeMemRW.Store2(addr, w.xy);
    else if (size == 4u) XeMemRW.Store(addr, w.x);
  }
}
)HLSL";

std::string Translator::BuildPrelude() const {
  std::string p = kPrelude;
  if (!is_ps_) {
    if (uses_memexport_) p += kMemexportHelpers;
    else p += "ByteAddressBuffer XeMem : register(t0);\n";
  } else if (uses_memexport_) {
    p += kMemexportHelpers;
  }
  static const char* kDimType[] = {"Texture2D<float4>", "Texture2D<float4>", "Texture3D<float4>", "TextureCube<float4>"};
  for (size_t i = 0; i < out_.textures.size(); i++) {
    p += Format("%s XeT%u : register(t%u);\nSamplerState XeS%u : register(s%u);\n",
                kDimType[out_.textures[i].dimension & 3], uint32_t(i), uint32_t(i + 1), uint32_t(i), uint32_t(i));
    if ((out_.textures[i].dimension & 3) == 2)
      p += Format("Texture2DArray<float4> XeTA%u : register(t%u);\n", uint32_t(i), uint32_t(i + kStackedTextureBase));
  }
  p += "struct XeInterp {\n  float4 pos : SV_Position;\n  float4 v[24] : TEXCOORD0;\n";
  if (!is_ps_) p += "  float psize : PSIZE;\n  float4 clip0 : SV_ClipDistance0;\n  float2 clip1 : SV_ClipDistance1;\n";
  p += "};\n";
  return p;
}

std::string Translator::BuildEntry() const {
  std::string e;
  std::string regs;
  if (temps_array_) {
    // FXC's initialization analysis ignores array initializers here; zero every element explicitly.
    regs = "  float4 R[64];\n";
    for (int i = 0; i < 64; i++) regs += Format("  R[%d] = float4(0.0, 0.0, 0.0, 0.0);\n", i);
  } else {
    regs = "  float4 r0 = 0.0";
    for (int i = 1; i < 64; i++) regs += Format(", r%d = 0.0", i);
    regs += ";\n";
  }
  auto reg = [&](uint32_t i) { return temps_array_ ? Format("R[%u]", i) : Format("r%u", i); };
  if (is_ps_) {
    e += "struct XePsOut {\n";
    for (uint32_t i = 0; i < 4; i++)
      if (out_.color_outputs & (1u << i)) e += Format("  float4 c%u : SV_Target%u;\n", i, i);
    if (out_.writes_depth) e += "  float depth : SV_Depth;\n";
    if (!out_.color_outputs && !out_.writes_depth) e += "  float4 c0 : SV_Target0;\n";
    e += "};\n";
    e += "XePsOut main(XeInterp xe_in, bool xe_face : SV_IsFrontFace) {\n";
    e += regs;
    for (auto& it : s_.interpolators) {
      int slot = InterpolatorSlot(it.usage, it.usage_index);
      if (slot >= 0 && it.reg < 64) e += Format("  %s = xe_in.v[%d];\n", reg(it.reg).c_str(), slot);
    }
    if (s_.ps_param_gen_reg < 64) {
      // In guest pixels (XAlpha.w: render targets may be drawn at a higher internal resolution).
      e += Format("  %s = float4((xe_in.pos.xy * XAlpha.w - 0.5) * float2(xe_face ? 1.0 : -1.0, 1.0), 0.0, 0.0);\n",
                  reg(s_.ps_param_gen_reg).c_str());
    }
    e += "  float4 oC0 = 0.0, oC1 = 0.0, oC2 = 0.0, oC3 = 0.0, oDepth4 = 0.0;\n";
  } else {
    e += "XeInterp main(uint xe_vid : SV_VertexID) {\n";
    e += regs;
    // SV_VertexID excludes the draw's start/base vertex; XZ.z carries it (as int bits).
    e += Format("  %s.x = float(int(xe_vid) + asint(XZ.z));\n", reg(0).c_str());
    e += "  float4 oPos = float4(0.0, 0.0, 0.0, 1.0), oPts = 0.0;\n";
    e += "  float4 oI[16];\n  [unroll] for (uint xe_o = 0; xe_o < 16; xe_o++) oI[xe_o] = float4(0.0, 0.0, 0.0, 0.0);\n";
    e += "  int xe_vindex = 0;\n";
  }
  e += "  int a0 = 0, aL = 0;\n  bool p0 = false;\n  float xe_ps = 0.0;\n  float xe_lod = 0.0;\n";
  e += "  float3 xe_grad_h = 0.0, xe_grad_v = 0.0;\n";
  e += "  bool xe_exit = false;\n  uint xe_pc = 0u;\n";
  e += "  uint xe_lsp = 0u, xe_csp = 0u;\n  int xe_ls_aL[4] = {0, 0, 0, 0};\n  uint xe_ls_cnt[4] = {0u, 0u, 0u, 0u};\n";
  e += "  uint xe_cs[4] = {0u, 0u, 0u, 0u};\n";
  if (uses_memexport_) {
    e += "  float4 xe_eA = 0.0;\n  float4 xe_eM[5] = {(float4)0, (float4)0, (float4)0, (float4)0, (float4)0};\n";
    e += "  uint xe_eM_written = 0u;\n";
  }
  return e;
}

bool Translator::Run(std::string* error) {
  Prescan();
  out_.uses_memexport = uses_memexport_;
  bool general = false;
  for (auto& cf : cf_) {
    switch (cf.opcode()) {
      case ControlFlowOpcode::kLoopStart:
      case ControlFlowOpcode::kLoopEnd:
      case ControlFlowOpcode::kCondCall:
      case ControlFlowOpcode::kReturn:
      case ControlFlowOpcode::kCondJmp:
        general = true;
        break;
      default:
        break;
    }
  }
  // FXC fails ("l-value expected") on a switch that writes variables inside a loop, so the
  // control flow program counter dispatches through an if/else chain.
  if (general) {
    indent_ = 2;
    Line("[loop] for (uint xe_guard = 0u; xe_guard < 65536u && !xe_exit; xe_guard++) {");
    indent_++;
    Line("uint xe_npc = xe_pc + 1u;");
    for (uint32_t i = 0; i < cf_.size(); i++) {
      Line(Format("%sif (xe_pc == %uu) {", i ? "else " : "", i));
      indent_++;
      EmitCfInstruction(i, cf_[i], true);
      indent_--;
      Line("}");
    }
    Line("else { xe_exit = true; }");
    Line("xe_pc = xe_npc;");
    indent_--;
    Line("}");
  } else {
    indent_ = 1;
    for (uint32_t i = 0; i < cf_.size(); i++) {
      if (cf_[i].opcode() == ControlFlowOpcode::kNop) continue;
      Line("if (!xe_exit) {");
      indent_++;
      EmitCfInstruction(i, cf_[i], false);
      indent_--;
      Line("}");
    }
  }
  if (uses_memexport_) body_ += "  xe_memexport_flush(xe_eA, xe_eM, xe_eM_written);\n";

  std::string epilogue;
  if (is_ps_) {
    // Alpha test (RB_COLORCONTROL) and render target exponent bias.
    epilogue += "  if (XAlpha.z != 0.0) {\n";
    epilogue += "    uint f = (uint)XAlpha.y; float av = oC0.w, ar = XAlpha.x;\n";
    epilogue += "    bool xe_pass = f == 0u ? false : f == 1u ? av < ar : f == 2u ? av == ar : f == 3u ? av <= ar :\n";
    epilogue += "                f == 4u ? av > ar : f == 5u ? av != ar : f == 6u ? av >= ar : true;\n";
    epilogue += "    if (!xe_pass) discard;\n  }\n";
    epilogue += "  XePsOut xe_out;\n";
    for (uint32_t i = 0; i < 4; i++)
      if (out_.color_outputs & (1u << i)) epilogue += Format("  xe_out.c%u = oC%u * XRtBias[%u];\n", i, i, i);
    if (out_.writes_depth) epilogue += "  xe_out.depth = oDepth4.x;\n";
    if (!out_.color_outputs && !out_.writes_depth) epilogue += "  xe_out.c0 = float4(0.0, 0.0, 0.0, 0.0);\n";
    epilogue += "  return xe_out;\n}\n";
  } else {
    epilogue += "  XeInterp xe_out;\n";
    epilogue += "  float4 p = oPos;\n";
    // XNdc: screen-space -> NDC transform when the viewport transform is disabled, plus the
    // Direct3D 9 half-pixel offset (scaled by w).
    epilogue += "  p.xy = p.xy * XNdc.xy + XNdc.zw * p.w;\n";
    // Xenos viewport z transform (PA_CL_VPORT_ZSCALE/ZOFFSET), possibly inverted.
    epilogue += "  p.z = p.z * XZ.x + XZ.y * p.w;\n";
    epilogue += "  xe_out.pos = p;\n";
    epilogue += "  [unroll] for (uint xe_k = 0; xe_k < 24; xe_k++) xe_out.v[xe_k] = float4(0.0, 0.0, 0.0, 0.0);\n";
    for (auto& it : s_.interpolators) {
      int slot = InterpolatorSlot(it.usage, it.usage_index);
      if (slot >= 0 && it.reg < 16) epilogue += Format("  xe_out.v[%d] = oI[%u];\n", slot, it.reg);
    }
    epilogue += "  xe_out.psize = oPts.x;\n";
    // User clip planes (PA_CL_UCP, Xenos clip space); disabled planes are zero.
    epilogue += "  xe_out.clip0 = float4(dot(oPos, XUcp[0]), dot(oPos, XUcp[1]), dot(oPos, XUcp[2]), dot(oPos, XUcp[3]));\n";
    epilogue += "  xe_out.clip1 = float2(dot(oPos, XUcp[4]), dot(oPos, XUcp[5]));\n";
    epilogue += "  return xe_out;\n}\n";
  }
  out_.uses_kill = uses_kill_;
  out_.hlsl = BuildPrelude() + BuildEntry() + body_ + epilogue;
  (void)error;
  return true;
}

}  // namespace

bool TranslateShader(const ParsedShader& shader, const std::vector<VertexInputElement>* vs_inputs,
                     TranslatedShader& out, std::string* error) {
  out = TranslatedShader{};
  if (shader.stage == ShaderStage::kVertex && !vs_inputs) {
    if (error) *error = "vertex shader needs resolved inputs";
    return false;
  }
  Translator t(shader, vs_inputs, out);
  return t.Run(error);
}

}  // namespace fs3gfx
