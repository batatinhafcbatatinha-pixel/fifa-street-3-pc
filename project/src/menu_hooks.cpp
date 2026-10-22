// The game's own menus on PC: the main menu's "XBOX LIVE" line (the online modes) reads "ONLINE".
// Every text the menus (EA APT movies) show comes through the localization interface's
// GetString (sub_821D5640: r3 interface, r4 output string object, r5 key; the text is at
// BE32(output + 4) after the call). fifastreet3-pc.ini [opcoes] menu_no_jogo=0 keeps the original.
#include <cstring>

#include <rex/hook.h>
#include <rex/memory.h>

#include "pc_settings.h"

namespace {

struct Rename {
  const char* from;  // the localized text, as the game builds it
  const char* to;    // at most as long
};

// Brand names, the same in every language of the game.
const Rename kRenames[] = {
    {"XBOX LIVE", "ONLINE"},
};

}  // namespace

REX_EXTERN(__imp__sub_821D5640);

// The "PC SETTINGS" line added by apt_menu.cpp triggers the menu through ltxt("PcSetngGo");
// that key is not in the game's database, so the lookup only serves as the native call.
constexpr char kOpenId[] = "PcSetngGo";
// Its row text: the key is not in the database either, and the lookup marks missing keys with
// a leading '*'. The key is longer than "PC SETTINGS", so the text is replaced in place.
constexpr char kRowId[] = "PcSettingsRow";

REX_HOOK_RAW(sub_821D5640) {
  const uint32_t output = ctx.r4.u32;
  const uint32_t key = ctx.r5.u32;
  __imp__sub_821D5640(ctx, base);
  if (!pcs::GameMenuEntry()) return;
  if (key && std::strncmp(reinterpret_cast<const char*>(rex::memory::GuestPtr(base, key)),
                          kOpenId, sizeof(kOpenId)) == 0) {
    pcs::RequestMenu();
    return;
  }
  if (!output) return;
  const uint32_t text = rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, output + 4));
  if (!text) return;
  char* s = reinterpret_cast<char*>(rex::memory::GuestPtr(base, text));
  {
    const char* id = s[0] == '*' ? s + 1 : s;
    if (std::strncmp(id, kRowId, sizeof(kRowId)) == 0) {
      static constexpr char kText[] = "PC SETTINGS";
      std::memcpy(s, kText, sizeof(kText));
      return;
    }
  }
  for (const Rename& r : kRenames) {
    size_t n = std::strlen(r.from);
    if (std::strncmp(s, r.from, n + 1) == 0) {
      size_t m = std::strlen(r.to);
      std::memcpy(s, r.to, m);
      std::memset(s + m, 0, n - m);
      return;
    }
  }
}

// ------------------------------------------------------- "PC settings" button on the menus
// The game is on its menus when the app's current module (app+4; app+8 is the next one) is the
// FRONTEND one (app+228),
// or, inside a match session (GAME module, app+236), when the game mode is not playing:
// state 1..3 setup/loading, 5 pause, 6 post-match; 4 = playing. The game mode object is the r3 of
// its state setter sub_821E2600, called on every change. FS3_MENU_LOG=1 logs the transitions.
#include <atomic>
#include <cstdlib>
#include <string>

#include <rex/logging.h>

namespace {
std::atomic<uint32_t> g_game_mode{0};

uint32_t Load32(uint8_t* base, uint32_t a) { return rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, a)); }
}  // namespace

REX_EXTERN(__imp__sub_821E2600);
REX_HOOK_RAW(sub_821E2600) {
  g_game_mode = ctx.r3.u32;
  __imp__sub_821E2600(ctx, base);
}

// Called once a frame from the flip hook.
void UpdateMenuButton(uint8_t* base) {
  uint32_t app = Load32(base, 0x8283E99C);
  bool menus = false;
  uint32_t module = 0, state = 0;
  if (app) {
    module = Load32(base, app + 4);
    if (module && module == Load32(base, app + 228)) {
      menus = true;
    } else if (module && module == Load32(base, app + 236)) {
      uint32_t mode = g_game_mode.load();
      state = mode ? Load32(base, mode + 0x10) : 4;
      menus = (state >= 1 && state <= 3) || state == 5 || state == 6;
    }
  }
  pcs::SetGameMenus(menus);
  static const bool log = [] { const char* e = std::getenv("FS3_MENU_LOG"); return e && *e == '1'; }();
  static int last = -1;
  int now = int(menus) | int(state << 1) | int((app && module == Load32(base, app + 228)) << 8);
  static uint32_t last_module = ~0u;
  if (log && (now != last || module != last_module)) {
    last = now;
    last_module = module;
    REXLOG_INFO("[menu] menus={} state={} module={:08X}", menus, state, module);
  }
}
