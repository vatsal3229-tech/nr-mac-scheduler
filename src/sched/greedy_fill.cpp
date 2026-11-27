#include <algorithm>

#include "sched/policies.h"

namespace nrs {

bool is_eligible(const UeState& ue, const LinkModel& link) {
  return ue.buffer_bytes > 0 && link.bits_per_prb(ue.cqi) > 0;
}

std::vector<Allocation> greedy_fill(const SlotInput& in, const std::vector<size_t>& order) {
  std::vector<Allocation> out;
  uint32_t remaining = in.num_prbs;
  for (size_t idx : order) {
    if (remaining == 0) break;
    const UeState& ue = in.ues[idx];
    if (!is_eligible(ue, in.link)) continue;
    const uint32_t need = in.link.prbs_for_bytes(ue.cqi, ue.buffer_bytes);
    const uint32_t give = std::min(need, remaining);
    // The last PRB is usually only partly filled; never claim more than the buffer holds.
    const uint64_t bytes = std::min(ue.buffer_bytes, in.link.bytes_for_prbs(ue.cqi, give));
    if (bytes == 0) continue;
    out.push_back({ue.id, give, static_cast<uint32_t>(bytes)});
    remaining -= give;
  }
  return out;
}

}  // namespace nrs
