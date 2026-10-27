// Post-process effects the player may turn off in PC SETTINGS (pc_game_info.h, kEffects).
//
// Bloom: the post-FX tweaks (Render_PostFX_Tweaks{Gameplay,Replay,GBActive}_Bloom, table at
// 0x82659D64) are copied by sub_8224FC38 into three 136-byte blocks of the renderer object, and
// every frame sub_8224D738 copies the current block's bloom values to obj+3612..3632. The bloom
// pass sub_82252AA8(obj, ...) uploads (Brightness obj+3616, Threshold +3624, Normalize +3628,
// Pow +3632) as one shader constant: Brightness 0 leaves the bloom target black, so the
// composite adds nothing. The value is zeroed for that call only and put back after it.
// Not verified in game (static analysis only).
#include "generated/default/fifastreet3_init.h"

#include <cstdint>

#include <rex/hook.h>
#include <rex/memory.h>

#include "pc_game_info.h"

REX_HOOK_RAW(sub_82252AA8) {
  const uint32_t brightness = ctx.r3.u32 ? ctx.r3.u32 + 3616 : 0;
  if (!brightness || pcs::EffectEnabled(kEffectBloom)) {
    __imp__sub_82252AA8(ctx, base);
    return;
  }
  uint8_t* p = rex::memory::GuestPtr(base, brightness);
  const uint32_t saved = rex::memory::load_and_swap<uint32_t>(p);
  rex::memory::store_and_swap<uint32_t>(p, 0);
  __imp__sub_82252AA8(ctx, base);
  rex::memory::store_and_swap<uint32_t>(p, saved);
}
