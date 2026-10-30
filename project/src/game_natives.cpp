// Native replacements for hot translated game functions (same results as the translated code).
//
// sub_82474C50 is the game's memcpy (r3 = dst, r4 = src, r5 = bytes; returns r3 unchanged):
// a forward copy, bytewise below 64 bytes and when the pointers are not word aligned, else
// in 16-byte word groups. On big-endian memory a word copy moves the same bytes, so for
// ranges that do not overlap (or dst <= src) the result is a plain memcpy/memmove. A dst
// inside (src, src+n) depends on the copy granularity: that case runs the translated code.
// It is ~17% of the render thread while movies play.
//
// sub_821F2370 is the qsort comparator of the render list (0x821F0074 sorts 4-byte entries
// that point to 120-byte keys): a bytewise compare of the two keys, -1 if the first differing
// byte of *r3 is smaller, 1 if larger, 0 if equal (unsigned bytes, i.e. memcmp order). ~22% of
// the render thread in a match.
//
// FS3_NO_NATIVE_MEMCPY=1  translated memcpy only
// FS3_NO_NATIVE_SORTKEY=1 translated comparator only
// FS3_NATIVE_CHECK=1      run the translated copy and compare it with the native result
// FS3_NATIVE_AB=1         alternate native/translated every 600 presented frames and log
//                         "[native-ab]" with the frame time of each block

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/fifastreet3_init.h"

namespace fs3_natives {
namespace {
bool EnvSet(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && *v != '0';
}
const bool g_memcpy_enabled = !EnvSet("FS3_NO_NATIVE_MEMCPY");
const bool g_sortkey_enabled = !EnvSet("FS3_NO_NATIVE_SORTKEY");
const bool g_check = EnvSet("FS3_NATIVE_CHECK");
const bool g_ab = EnvSet("FS3_NATIVE_AB");
std::atomic<bool> g_ab_on{true};
std::atomic<uint64_t> g_check_calls{0}, g_check_mismatches{0};
std::atomic<uint64_t> g_key_calls{0}, g_key_mismatches{0};

bool NativeOn() { return !g_ab || g_ab_on.load(std::memory_order_relaxed); }

uint8_t* Host(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
uint32_t Load32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Host(base, a), 4);
  return __builtin_bswap32(v);
}

// A range that stays on one side of the 0xE0000000 host offset and does not wrap.
bool Contiguous(uint32_t a, uint32_t n) {
  uint64_t end = uint64_t(a) + n;
  return end <= 0x100000000ull && (a >= 0xE0000000u || end <= 0xE0000000ull);
}
}  // namespace

// Called once per presented frame (perf_hooks.cpp).
void FrameTick(uint64_t frame) {
  if (!g_ab) return;
  static auto start = std::chrono::steady_clock::now();
  if (frame % 600 != 0) return;
  auto now = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(now - start).count();
  start = now;
  bool was = g_ab_on.load(std::memory_order_relaxed);
  REXLOG_INFO("[native-ab] native={} frames=600 ms={:.1f} fps={:.2f}", was ? 1 : 0, ms, 600000.0 / ms);
  g_ab_on.store(!was, std::memory_order_relaxed);
}
}  // namespace fs3_natives

using namespace fs3_natives;

REX_EXTERN(__imp__sub_82474C50);
REX_HOOK_RAW(sub_82474C50) {
  uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  bool simple = n && Contiguous(dst, n) && Contiguous(src, n) && !(dst > src && dst - src < n);
  if (!g_memcpy_enabled || !simple || !NativeOn()) {
    __imp__sub_82474C50(ctx, base);
    return;
  }
  if (g_check) {
    std::vector<uint8_t> want(Host(base, src), Host(base, src) + n);
    __imp__sub_82474C50(ctx, base);
    uint64_t calls = g_check_calls.fetch_add(1) + 1;
    if (std::memcmp(Host(base, dst), want.data(), n) != 0) {
      uint64_t m = g_check_mismatches.fetch_add(1) + 1;
      if (m <= 20) REXLOG_WARN("[native-check] memcpy MISMATCH dst={:08X} src={:08X} n={}", dst, src, n);
    }
    if ((calls & 0xFFFFF) == 0)
      REXLOG_INFO("[native-check] memcpy calls={} mismatches={}", calls, g_check_mismatches.load());
    return;
  }
  std::memmove(Host(base, dst), Host(base, src), n);
}

REX_EXTERN(__imp__sub_821F2370);
REX_HOOK_RAW(sub_821F2370) {
  uint32_t a = Load32(base, ctx.r3.u32), b = Load32(base, ctx.r4.u32);
  if (!g_sortkey_enabled || !NativeOn() || !Contiguous(a, 120) || !Contiguous(b, 120)) {
    __imp__sub_821F2370(ctx, base);
    return;
  }
  int c = std::memcmp(Host(base, a), Host(base, b), 120);
  int64_t r = c < 0 ? -1 : c > 0 ? 1 : 0;
  if (g_check) {
    __imp__sub_821F2370(ctx, base);
    uint64_t calls = g_key_calls.fetch_add(1) + 1;
    if (ctx.r3.s64 != r) {
      uint64_t m = g_key_mismatches.fetch_add(1) + 1;
      if (m <= 20) REXLOG_WARN("[native-check] sortkey MISMATCH {:08X} {:08X} native={} translated={}", a, b, r, ctx.r3.s64);
    }
    if ((calls & 0xFFFFF) == 0)
      REXLOG_INFO("[native-check] sortkey calls={} mismatches={}", calls, g_key_mismatches.load());
    return;
  }
  ctx.r3.s64 = r;
}
