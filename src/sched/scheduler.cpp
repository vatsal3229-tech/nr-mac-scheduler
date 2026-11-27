#include "sched/scheduler.h"

#include "sched/policies.h"

namespace nrs {

std::optional<Policy> parse_policy(std::string_view s) {
  if (s == "round_robin" || s == "rr") return Policy::kRoundRobin;
  if (s == "proportional_fair" || s == "pf") return Policy::kProportionalFair;
  if (s == "max_throughput" || s == "maxt") return Policy::kMaxThroughput;
  return std::nullopt;
}

std::string_view to_string(Policy p) {
  switch (p) {
    case Policy::kRoundRobin:
      return "round_robin";
    case Policy::kProportionalFair:
      return "proportional_fair";
    case Policy::kMaxThroughput:
      return "max_throughput";
  }
  return "unknown";
}

std::unique_ptr<Scheduler> make_scheduler(Policy p) {
  switch (p) {
    case Policy::kRoundRobin:
      return std::make_unique<RoundRobinScheduler>();
    case Policy::kProportionalFair:
      return std::make_unique<ProportionalFairScheduler>();
    case Policy::kMaxThroughput:
      return std::make_unique<MaxThroughputScheduler>();
  }
  return nullptr;
}

}  // namespace nrs
