// "PC SETTINGS" line at the end of the game's own MY FIFA STREET options menu, drawn by the
// game like its other lines.
//
// The menus are EA APT movies (Flash-like ActionScript bytecode; fe\*.big in the disc data).
// MainMenu.big builds extrasMenuData.arrData with six entries (GAME SETTINGS, CONTROLS,
// VIDEO CALIBRATION, EA TRAX, PROFILE MANAGEMENT, CREDITS) and doNext() sends the chosen
// entry's screenID to gSM.changeMainScreen.
//
// sub_8232E198 (r3 action block, r4 the movie's .apt base, r5 its .const base or 0 on the
// unload pass, r6 a counter) prepares a block: operands become absolute pointers and the
// constants of push lists become value objects (strings interned by sub_82338370). The block
// that prepared a site is not reported directly, so the site itself is the signature: after
// the call it must hold a prepared push whose text is the expected one. The block is patched
// in place by replacing the push with a jump to code written here (guest memory), which runs
// the original push and the additions, then jumps back.
//   - build site 0x5FB2: push ["videosData"]; the added code appends a 7th entry to
//     extrasMenuData.arrData: {name:"PC SETTINGS", screenID:"PcSetngGo", modeID:undefined,
//     helpText:"PC SETTINGS"}.
//   - doNext select site 0x4598: push [entry, "screenID"]; the added code compares it with
//     "PcSetngGo" and, when it matches, calls ltxt("PcSetngGo") and returns instead of
//     changeMainScreen. The localization lookup is answered in menu_hooks.cpp, which opens
//     the PC settings menu.
// The strings are interned with the game's own function, so the values are ordinary APT
// values (no hand-built layouts). fifastreet3-pc.ini [opcoes] menu_no_jogo=0 keeps the
// original menus; FS3_APT_LOG=1 logs the blocks and the patches.
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "pc_settings.h"

namespace {

// Offsets from the "Apt Data" chunk of MainMenu.big (scratchpad s3apt.py). The block that
// prepared a site is not reported, but its start and the unprepared instruction are unique
// enough: the build site lives in the movie's main script (block 568) and the select site in
// doNext (block 4148).
constexpr uint32_t kBuildBlock = 0x568;
constexpr uint32_t kBuildSite = 0x5FB2;   // push ["videosData"]
constexpr uint32_t kBuildLen = 0x5FBC - kBuildSite;
constexpr uint8_t kBuildSig[16] = {0x96, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
                                   0xA0, 0x84, 0x1C, 0x96, 0x00, 0x00, 0x00, 0x00};
constexpr uint32_t kSelectBlock = 0x4148;
constexpr uint32_t kSelectSite = 0x4598;  // push [entry, "screenID"]
constexpr uint32_t kSelectLen = 0x45A4 - kSelectSite;
constexpr uint32_t kSelectDone = 0x45EA;  // end of doNext
constexpr uint8_t kSelectSig[16] = {0x96, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
                                    0x00, 0x00, 0x8E, 0x38, 0x4E, 0x96, 0x00, 0x00};

// Push lists of the same block that already hold the values the added code needs.
constexpr uint32_t kExtrasPush = 0x5D96;   // ["extrasMenuData"]
constexpr uint32_t kArrPush = 0x5DA1;      // ["arrData"]
constexpr uint32_t kEntryPush = 0x5DAD;    // [0, "name", "GameSettings", "screenID", "_global"]
constexpr uint32_t kFieldPush = 0x5DDD;    // ["modeID", undefined, "helpText", "GameSettingsHelp", 1, "ltxt"]
constexpr uint32_t kCountPush = 0x5DE9;    // [4]
constexpr uint32_t kTwoPush = 0x5E6D;      // [2, "name", "VideoCalibration", ...]

// The row text is looked up in the game's database; a missing key comes back with a leading
// '*'. This key is a little longer than "PC SETTINGS" so the lookup's own buffer can hold the
// replacement (menu_hooks.cpp) and the marker disappears.
constexpr char kRowId[] = "PcSettingsRow";

uint32_t Load32(uint8_t* base, uint32_t a) { return rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, a)); }
void Store32(uint8_t* base, uint32_t a, uint32_t v) { rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, a), v); }
uint32_t Align4(uint32_t a) { return (a + 3) & ~3u; }

bool LogOn() {
  static const bool on = [] { const char* e = std::getenv("FS3_APT_LOG"); return e && *e == '1'; }();
  return on;
}

uint32_t GuestAlloc(uint32_t size) {
  uint32_t a = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  if (a) std::memset(rex::memory::GuestPtr(REX_KERNEL_MEMORY()->virtual_membase(), a), 0, size);
  return a;
}

// A prepared push instruction: opcode, then (aligned) count and list pointer.
struct Push {
  uint32_t count = 0, list = 0;
  bool ok = false;
};
Push ReadPush(uint8_t* base, uint32_t at) {
  Push p;
  uint32_t operand = Align4(at + 1);
  p.count = Load32(base, operand);
  p.list = Load32(base, operand + 4);
  p.ok = *rex::memory::GuestPtr(base, at) == 0x96 && p.count && p.count <= 64 &&
         p.list > 0x10000000u;
  return p;
}
uint32_t PushValue(uint8_t* base, const Push& p, uint32_t pos) {
  return p.ok && pos < p.count ? Load32(base, p.list + 4 * pos) : 0;
}

// APT bytecode writer: one opcode byte; operands are 4-byte aligned big-endian words.
struct Writer {
  uint8_t* base;
  uint32_t data, data_end;  // push lists
  uint32_t code, code_end;

  uint32_t List(std::initializer_list<uint32_t> values) {
    uint32_t at = data;
    for (uint32_t v : values) Store32(base, data, v), data += 4;
    return at;
  }
  void Op(uint8_t op) { *rex::memory::GuestPtr(base, code++) = op; }
  void Word(uint32_t v) {
    while (code & 3) Op(0);
    Store32(base, code, v);
    code += 4;
  }
  void PushList(uint32_t list, uint32_t count) { Op(0x96); Word(count); Word(list); }
  // Branch (0x99 jump, 0x9D jump if true) to `target`; returns the operand (to fix later).
  uint32_t Branch(uint8_t op, uint32_t target = 0) {
    Op(op);
    Word(0);
    if (target) Fix(code - 4, target);
    return code - 4;
  }
  void Fix(uint32_t operand, uint32_t target) { Store32(base, operand, target - (operand + 4)); }
  bool Ok() const { return data <= data_end && code <= code_end; }
};

bool NewWriter(uint8_t* base, Writer& w) {
  uint32_t mem = GuestAlloc(384);
  if (!mem) return false;
  w = {base, mem, mem + 128, mem + 128, mem + 384};
  return true;
}

// Replaces the instruction at `site` (`len` bytes) with a jump to `target`, the rest with pops
// (never reached; they keep the block walkable).
void JumpTo(uint8_t* base, uint32_t site, uint32_t len, uint32_t target) {
  uint8_t* p = rex::memory::GuestPtr(base, site);
  uint32_t operand = Align4(site + 1);
  p[0] = 0x99;
  for (uint32_t a = site + 1; a < operand; ++a) p[a - site] = 0;
  Store32(base, operand, target - (operand + 4));
  for (uint32_t a = operand + 4; a < site + len; ++a) p[a - site] = 0x17;
}

// Side call into the game (CallFrame keeps the hook's registers intact).
REX_IMPORT(sub_82338370, g_intern_string, uint32_t(uint32_t));

// Interns a NUL-terminated C string with the game's own string table and returns the APT value.
uint32_t InternString(PPCContext& parent, uint8_t* base, const char* text) {
  size_t n = std::strlen(text);
  uint32_t mem = GuestAlloc(uint32_t(n + 1));
  if (!mem) return 0;
  std::memcpy(rex::memory::GuestPtr(base, mem), text, n + 1);
  rex::CallFrame frame(parent);
  return g_intern_string(frame, base, mem);
}

// PcSetngGo is the marker the select patch compares with and the key its ltxt call carries.
constexpr char kOpenId[] = "PcSetngGo";

void PatchBuild(PPCContext& ctx, uint8_t* base, uint32_t apt) {
  const uint32_t site = apt + kBuildSite;
  const Push orig = ReadPush(base, site);
  if (!orig.ok || orig.count != 1) {
    REXLOG_WARN("[apt] MainMenu build site not prepared, PC SETTINGS not added");
    return;
  }
  const uint32_t name = InternString(ctx, base, kRowId);
  const uint32_t open = InternString(ctx, base, kOpenId);
  Writer w;
  if (!name || !open || !NewWriter(base, w)) {
    REXLOG_WARN("[apt] MainMenu build site: could not write the PC SETTINGS line");
    return;
  }
  const Push extras_p = ReadPush(base, apt + kExtrasPush);
  const Push arr_p = ReadPush(base, apt + kArrPush);
  const Push entry_p = ReadPush(base, apt + kEntryPush);
  const Push field_p = ReadPush(base, apt + kFieldPush);
  const Push count_p = ReadPush(base, apt + kCountPush);
  const Push two_p = ReadPush(base, apt + kTwoPush);
  const uint32_t extras = PushValue(base, extras_p, 0), arr = PushValue(base, arr_p, 0),
                 name_key = PushValue(base, entry_p, 1), screen_key = PushValue(base, entry_p, 3),
                 mode_key = PushValue(base, field_p, 0), undef = PushValue(base, field_p, 1),
                 help_key = PushValue(base, field_p, 2), four = PushValue(base, count_p, 0),
                 two = PushValue(base, two_p, 0);
  if (!extras || !arr || !name_key || !screen_key || !mode_key || !undef || !help_key || !four || !two) {
    REXLOG_WARN("[apt] MainMenu build values not found, PC SETTINGS not added");
    return;
  }
  // helpText is what the game draws under the list: localize it now instead of storing the raw
  // key, so it is the same text as the row (menu_hooks.cpp answers the lookup).
  const uint32_t one = PushValue(base, field_p, 4), ltxt = PushValue(base, field_p, 5);
  if (!one || !ltxt) {
    REXLOG_WARN("[apt] MainMenu build: ltxt not found, PC SETTINGS not added");
    return;
  }
  const uint32_t l_extras = w.List({extras}), l_arr = w.List({arr}),
                 l_name = w.List({name_key, name}), l_screen = w.List({screen_key, open}),
                 l_mode = w.List({mode_key, undef}), l_help = w.List({help_key}),
                 l_trigger = w.List({name, one, ltxt}),
                 l_four = w.List({four}), l_two = w.List({two}),
                 l_orig = w.List({PushValue(base, orig, 0)});
  const uint32_t entry = w.code;
  w.PushList(l_extras, 1);
  w.Op(0x1C);  // getVar: extrasMenuData
  w.PushList(l_arr, 1);
  w.Op(0x4E);  // getMember: .arrData
  w.PushList(l_four, 1);
  w.PushList(l_two, 1);
  w.Op(0x47);  // add2: index 4 + 2 = 6 (the six original lines end at 5)
  w.PushList(l_name, 2);    // name
  w.PushList(l_screen, 2);  // screenID
  w.PushList(l_mode, 2);    // modeID
  w.PushList(l_help, 1);    // helpText key
  w.PushList(l_trigger, 3);  // ltxt("PcSettingsRow")
  w.Op(0x3D);  // callFunction -> helpText value
  w.PushList(l_four, 1);     // initObject takes 4 fields
  w.Op(0x43);  // initObject -> the entry
  w.Op(0x4F);  // setMember: arrData[6] = entry
  w.PushList(l_orig, 1);  // the replaced push ["videosData"]
  w.Branch(0x99, site + kBuildLen);
  if (!w.Ok()) return;
  JumpTo(base, site, kBuildLen, entry);
  REXLOG_INFO("[apt] MainMenu: PC SETTINGS added as line 7 (code {:08X})", entry);
}

void PatchSelect(PPCContext& ctx, uint8_t* base, uint32_t apt) {
  const uint32_t site = apt + kSelectSite;
  const Push orig = ReadPush(base, site);
  if (!orig.ok || orig.count != 2) {
    REXLOG_WARN("[apt] doNext select site not prepared, PC SETTINGS left alone");
    return;
  }
  Writer w;
  if (!NewWriter(base, w)) return;
  const uint32_t entry = PushValue(base, orig, 0);
  const Push field_p = ReadPush(base, apt + kFieldPush);
  const uint32_t one = PushValue(base, field_p, 4), ltxt = PushValue(base, field_p, 5);
  const uint32_t open = InternString(ctx, base, kOpenId);
  if (!entry || !one || !ltxt || !open) {
    REXLOG_WARN("[apt] doNext select values not found, PC SETTINGS left alone");
    return;
  }
  const uint32_t l_entry = w.List({entry, PushValue(base, orig, 1)}), l_open = w.List({open}),
                 l_trigger = w.List({open, one, ltxt}), l_orig = w.List({entry, PushValue(base, orig, 1)});
  const uint32_t code = w.code;
  w.PushList(l_entry, 2);
  w.Op(0x4E);  // getMember: entry.screenID
  w.PushList(l_open, 1);
  w.Op(0x49);  // equals2
  w.Op(0x12);  // not
  const uint32_t to_original = w.Branch(0x9D);  // jump if the line is another one
  w.PushList(l_trigger, 3);  // ltxt("PcSetngGo")
  w.Op(0x3D);  // callFunction
  w.Op(0x17);  // pop
  w.Branch(0x99, apt + kSelectDone);  // skip changeMainScreen
  w.Fix(to_original, w.code);
  w.PushList(l_orig, 2);  // the replaced push
  w.Branch(0x99, site + kSelectLen);
  if (!w.Ok()) return;
  JumpTo(base, site, kSelectLen, code);
  REXLOG_INFO("[apt] doNext: PC SETTINGS line opens the PC menu (code {:08X})", code);
}

// Every distinct block offset seen in a session, only for FS3_APT_LOG=1.
void LogBlock(uint32_t off) {
  static uint32_t seen[512] = {};
  static size_t count = 0;
  for (size_t i = 0; i < count; ++i) if (seen[i] == off) return;
  if (count < std::size(seen)) seen[count++] = off;
  REXLOG_INFO("[apt] block {:X}", off);
}

}  // namespace

REX_EXTERN(__imp__sub_8232E198);

REX_HOOK_RAW(sub_8232E198) {
  const uint32_t block = ctx.r3.u32, apt = ctx.r4.u32, constants = ctx.r5.u32;
  bool build = false, select = false;
  // The unprepared bytes are checked before the call: afterwards they hold pointers. A site
  // already patched starts with the jump opcode and no longer matches (reloads re-parse it).
  // The block that prepares a site is not reported, so the site itself is the signature.
  if (apt && constants && pcs::GameMenuEntry()) {
    if (std::memcmp(rex::memory::GuestPtr(base, apt + kBuildSite), kBuildSig, 16) == 0)
      build = true;
    if (std::memcmp(rex::memory::GuestPtr(base, apt + kSelectSite), kSelectSig, 16) == 0)
      select = true;
    if (LogOn() && block >= apt && (build || select))
      REXLOG_INFO("[apt] site of block {:X} (build {} select {})", block - apt, build, select);
    if (LogOn()) LogBlock(block >= apt ? block - apt : 0);
  }
  __imp__sub_8232E198(ctx, base);
  // The sites are prepared in the pass that creates the value objects (constants != 0).
  if (build && ReadPush(base, apt + kBuildSite).ok) PatchBuild(ctx, base, apt);
  if (select && ReadPush(base, apt + kSelectSite).ok) PatchSelect(ctx, base, apt);
}
