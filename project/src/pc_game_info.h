// FIFA Street 3 for the in-game PC settings (pc_settings.h).
#pragma once

#include "pc_settings.h"

inline pcs::GameInfo PcGameInfo() {
  // The game picks its language on its own screen (fe/01_languageSelect): no choice here.
  pcs::GameInfo info;
  info.title = L"FIFA STREET 3";
  info.window_title = "FIFA Street 3";
  info.ini_name = L"fifastreet3-pc.ini";
  info.internal_resolution = true;
  info.football_keyboard = true;
  return info;
}
