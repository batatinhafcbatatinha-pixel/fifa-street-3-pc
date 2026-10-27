// FIFA Street 3 for the in-game PC settings (pc_settings.h).
#pragma once

#include "pc_settings.h"

const pcs::Theme* PcStreet3Theme();

// Indices into kEffects (switched in post_effects.cpp).
enum { kEffectBloom };

inline pcs::GameInfo PcGameInfo() {
  // The game picks its language on its own screen (fe/01_languageSelect): no choice here.
  static const pcs::Effect kEffects[] = {
      {"bloom", L"Bloom", L"Bloom",
       L"Brilho em volta das partes claras da imagem.", L"Glow around the bright parts of the image."},
  };
  pcs::GameInfo info;
  info.title = L"FIFA STREET 3";
  info.window_title = "FIFA Street 3";
  info.ini_name = L"fifastreet3-pc.ini";
  info.internal_resolution = true;
  info.msaa = true;  // the renderer already draws the game's own 2x/4x targets multisampled
  info.football_keyboard = true;
  // Measured 2026-10-20 (FS3_DT_LOG): the frame timer sub_8247F238 sums 5.0 s of game time per 5 s
  // of wall time at 60 fps and at ~86 fps unlimited, so frame rates above 60 keep the speed.
  info.real_time = true;
  info.effects = kEffects;
  info.effect_count = int(sizeof(kEffects) / sizeof(kEffects[0]));
  info.theme = PcStreet3Theme();
  return info;
}
