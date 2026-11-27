#include <algorithm>

#include "sched/policies.h"

namespace nrs {

std::vector<Allocation> MaxThroughputScheduler::schedule(const SlotInput& in) {
  std::vector<size_t> order;
  for (size_t i = 0; i < in.ues.size(); ++i) {
    if (is_eligible(in.ues[i], in.link)) order.push_back(i);
  }
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (in.ues[a].cqi != in.ues[b].cqi) return in.ues[a].cqi > in.ues[b].cqi;
    return in.ues[a].id < in.ues[b].id;
  });
  return greedy_fill(in, order);
}

}  // namespace nrs
