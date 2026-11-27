#include <algorithm>

#include "sched/policies.h"

namespace nrs {
namespace {
// Floor on the average so a UE that has never been served gets a large but
// finite priority instead of a division by zero.
constexpr double kMinAvgBps = 1.0;
}  // namespace

std::vector<Allocation> ProportionalFairScheduler::schedule(const SlotInput& in) {
  std::vector<size_t> order;
  std::vector<double> priority(in.ues.size(), 0.0);
  for (size_t i = 0; i < in.ues.size(); ++i) {
    const UeState& ue = in.ues[i];
    if (!is_eligible(ue, in.link)) continue;
    // Achievable rate if this UE got the whole carrier this slot. With one
    // wideband CQI the per-PRB rate is the same on every PRB, so this is
    // proportional to the per-PRB rate and the ranking is identical.
    const double achievable =
        static_cast<double>(in.link.bits_per_prb(ue.cqi)) * in.num_prbs / in.slot_duration_s;
    priority[i] = achievable / std::max(ue.avg_tput_bps, kMinAvgBps);
    order.push_back(i);
  }
  // Ties broken by UE id so runs are bit-for-bit reproducible.
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (priority[a] != priority[b]) return priority[a] > priority[b];
    return in.ues[a].id < in.ues[b].id;
  });
  return greedy_fill(in, order);
}

}  // namespace nrs
