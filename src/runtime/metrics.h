#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "kpi/kpi.h"

namespace nrs {

struct UeMetrics {
  uint16_t id = 0;
  uint8_t cqi = 0;
  uint64_t buffer_bytes = 0;
  double avg_tput_bps = 0.0;
  uint64_t bytes_scheduled = 0;
};

// Everything /metrics shows about the slot loop, copied out as one consistent
// snapshot so a scrape never sees half-updated values.
struct SlotLoopSnapshot {
  std::string policy;
  uint32_t num_prbs = 0;
  uint64_t slots = 0;
  uint64_t overruns = 0;
  uint64_t prbs_allocated = 0;
  uint64_t bytes_scheduled = 0;
  uint64_t rejected_reports = 0;
  uint64_t buffer_overflow_bytes = 0;
  uint64_t invariant_violations = 0;
  KpiReport kpi;  // cumulative since start
  LatencyHistogram sched_hist;
  std::vector<UeMetrics> ues;
};

// Hand-off point between the slot loop (writer) and the HTTP thread (reader).
class MetricsHub {
 public:
  void publish(SlotLoopSnapshot s) {
    std::lock_guard<std::mutex> lock(mu_);
    snap_ = std::move(s);
  }
  SlotLoopSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return snap_;
  }

  // Liveness heartbeat: steady_clock time (ns) of the last completed slot.
  std::atomic<int64_t> last_slot_ns{0};
  std::atomic<bool> ready{false};

 private:
  mutable std::mutex mu_;
  SlotLoopSnapshot snap_;
};

}  // namespace nrs
