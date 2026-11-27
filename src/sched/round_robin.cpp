#include <algorithm>

#include "sched/policies.h"

namespace nrs {

// Equal-share round robin: every backlogged UE gets the same number of PRBs
// this slot, regardless of channel quality. PRBs a UE cannot use (small
// buffer) are re-split among the rest ("water filling"). PRBs that do not
// divide evenly go one each to UEs starting at a rotating position, so over
// several slots every UE gets the same total.
std::vector<Allocation> RoundRobinScheduler::schedule(const SlotInput& in) {
  std::vector<size_t> active;
  for (size_t i = 0; i < in.ues.size(); ++i) {
    if (is_eligible(in.ues[i], in.link)) active.push_back(i);
  }
  if (active.empty()) return {};

  // Rotate so the UE at/after next_start_id_ comes first. in.ues is sorted by id.
  auto first = std::find_if(active.begin(), active.end(),
                            [&](size_t i) { return in.ues[i].id >= next_start_id_; });
  std::rotate(active.begin(), first, active.end());

  std::vector<uint32_t> need(active.size());
  std::vector<uint32_t> got(active.size(), 0);
  for (size_t k = 0; k < active.size(); ++k) {
    const UeState& ue = in.ues[active[k]];
    need[k] = in.link.prbs_for_bytes(ue.cqi, ue.buffer_bytes);
  }

  uint32_t remaining = in.num_prbs;
  while (remaining > 0) {
    std::vector<size_t> hungry;
    for (size_t k = 0; k < active.size(); ++k) {
      if (got[k] < need[k]) hungry.push_back(k);
    }
    if (hungry.empty()) break;
    const uint32_t share = remaining / static_cast<uint32_t>(hungry.size());
    if (share == 0) {
      // Fewer PRBs than hungry UEs: hand out the remainder one by one in rotation order.
      for (size_t k : hungry) {
        if (remaining == 0) break;
        ++got[k];
        --remaining;
        next_start_id_ = in.ues[active[k]].id + 1u;
      }
      break;
    }
    for (size_t k : hungry) {
      const uint32_t g = std::min(share, need[k] - got[k]);
      got[k] += g;
      remaining -= g;
    }
  }

  std::vector<Allocation> out;
  for (size_t k = 0; k < active.size(); ++k) {
    if (got[k] == 0) continue;
    const UeState& ue = in.ues[active[k]];
    const uint64_t bytes = std::min(ue.buffer_bytes, in.link.bytes_for_prbs(ue.cqi, got[k]));
    out.push_back({ue.id, got[k], static_cast<uint32_t>(bytes)});
  }
  // Keep allocations in id order so output does not depend on the rotation.
  std::sort(out.begin(), out.end(),
            [](const Allocation& a, const Allocation& b) { return a.ue_id < b.ue_id; });
  return out;
}

}  // namespace nrs
