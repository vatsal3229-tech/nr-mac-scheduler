#pragma once

#include <cstddef>
#include <vector>

#include "sched/scheduler.h"

namespace nrs {

// A UE can be scheduled only if it has data and a CQI that carries bits.
bool is_eligible(const UeState& ue, const LinkModel& link);

// Serve UEs in `order` (indices into in.ues), each up to its buffer, until
// PRBs run out. Shared by PF and max-throughput, which differ only in ordering.
std::vector<Allocation> greedy_fill(const SlotInput& in, const std::vector<size_t>& order);

class RoundRobinScheduler final : public Scheduler {
 public:
  std::vector<Allocation> schedule(const SlotInput& in) override;
  std::string_view name() const override { return "round_robin"; }

 private:
  // UE id where the next leftover PRB goes; rotating it makes leftovers fair over time.
  uint32_t next_start_id_ = 0;
};

class ProportionalFairScheduler final : public Scheduler {
 public:
  std::vector<Allocation> schedule(const SlotInput& in) override;
  std::string_view name() const override { return "proportional_fair"; }
};

class MaxThroughputScheduler final : public Scheduler {
 public:
  std::vector<Allocation> schedule(const SlotInput& in) override;
  std::string_view name() const override { return "max_throughput"; }
};

}  // namespace nrs
