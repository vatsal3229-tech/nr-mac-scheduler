#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "core/types.h"

namespace nrs {

// Jain's fairness index: (sum x)^2 / (n * sum x^2), in [1/n, 1].
// Conventions: empty input -> 0; all-zero input -> 1 (everyone equally served).
double jain_index(const std::vector<double>& x);

// Nearest-rank percentile, p in (0, 100]. Empty input -> 0.
double percentile(std::vector<double> samples, double p);

// Fixed-bucket latency histogram in microseconds, exported as a Prometheus histogram.
class LatencyHistogram {
 public:
  static constexpr std::array<double, 10> kBucketsUs = {1, 2, 5, 10, 25, 50, 100, 250, 500, 1000};
  void observe(double us);
  const std::array<uint64_t, kBucketsUs.size()>& cumulative_counts() const { return cum_; }
  uint64_t count() const { return count_; }
  double sum() const { return sum_; }

 private:
  std::array<uint64_t, kBucketsUs.size()> cum_{};
  uint64_t count_ = 0;
  double sum_ = 0.0;
};

struct KpiReport {
  std::string policy;
  uint64_t slots = 0;
  double duration_s = 0.0;
  double cell_throughput_bps = 0.0;
  std::map<uint16_t, double> ue_throughput_bps;
  double jain_fairness = 0.0;
  double prb_utilization = 0.0;
  double sched_time_p50_us = 0.0;
  double sched_time_p99_us = 0.0;
  uint64_t slot_overruns = 0;
  uint64_t malformed_packets = 0;
  uint64_t rejected_reports = 0;
  uint64_t buffer_overflow_bytes = 0;
  uint64_t invariant_violations = 0;
};

// Accumulates per-slot results into cell-level KPIs. Used by both offline
// replay and the live slot loop, so both report identical definitions.
class KpiCollector {
 public:
  // max_latency_samples bounds memory in live mode (rolling window); 0 = keep all.
  KpiCollector(uint32_t num_prbs, double slot_duration_s, size_t max_latency_samples = 0);

  void record_slot(const SlotResult& r, const std::vector<UeState>& ues, double sched_time_us);
  // Fills throughput, fairness, utilization and latency fields; caller adds counters.
  KpiReport report() const;

  const LatencyHistogram& histogram() const { return hist_; }
  uint64_t total_bytes() const { return total_bytes_; }
  uint64_t total_prbs() const { return total_prbs_; }
  const std::map<uint16_t, uint64_t>& ue_bytes() const { return ue_bytes_; }

 private:
  uint32_t num_prbs_;
  double slot_s_;
  size_t max_samples_;
  uint64_t slots_ = 0;
  uint64_t total_bytes_ = 0;
  uint64_t total_prbs_ = 0;
  std::map<uint16_t, uint64_t> ue_bytes_;  // every UE ever seen, served or not
  std::deque<double> latency_us_;
  LatencyHistogram hist_;
};

}  // namespace nrs
