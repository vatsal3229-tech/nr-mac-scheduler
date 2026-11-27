#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "core/types.h"
#include "phy/cqi_table.h"

namespace nrs {

// Everything a policy may look at for one slot. Read-only by construction:
// policies cannot mutate UE state, only propose allocations.
struct SlotInput {
  const std::vector<UeState>& ues;
  uint32_t num_prbs;
  const LinkModel& link;
  double slot_duration_s;
};

// Strategy interface. schedule() is non-const because some policies (round
// robin) carry state between slots.
class Scheduler {
 public:
  virtual ~Scheduler() = default;
  virtual std::vector<Allocation> schedule(const SlotInput& in) = 0;
  virtual std::string_view name() const = 0;
};

enum class Policy { kRoundRobin, kProportionalFair, kMaxThroughput };

std::optional<Policy> parse_policy(std::string_view s);
std::string_view to_string(Policy p);
std::unique_ptr<Scheduler> make_scheduler(Policy p);

}  // namespace nrs
