// Diagnostics only (FS3_DT_LOG=1): the game's frame timer sub_8247F238 returns the seconds elapsed
// since its previous call (f1). Every 5 s of wall-clock time this logs, per call site, how many
// calls there were and the sum of the deltas: a simulation that keeps real time sums ~5 s per
// call site whatever the frame rate.
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>

REX_EXTERN(__imp__sub_8247F238);

REX_HOOK_RAW(sub_8247F238) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_8247F238(ctx, base);
  static const bool on = [] { const char* e = std::getenv("FS3_DT_LOG"); return e && *e == '1'; }();
  if (!on) return;
  using Clock = std::chrono::steady_clock;
  static std::mutex mu;
  static std::map<uint32_t, std::pair<int, double>> sums;
  static Clock::time_point window = Clock::now();
  std::lock_guard<std::mutex> lock(mu);
  auto& e = sums[lr];
  ++e.first;
  e.second += ctx.f1.f64;
  double wall = std::chrono::duration<double>(Clock::now() - window).count();
  if (wall >= 5.0) {
    for (auto& [site, v] : sums)
      if (v.first >= 20) REXLOG_INFO("[dt] {:08X}: {} calls, {:.2f} s game / {:.2f} s wall", site, v.first, v.second, wall);
    sums.clear();
    window = Clock::now();
  }
}
