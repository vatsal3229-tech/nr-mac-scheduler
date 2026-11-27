#include "kpi/kpi.h"

#include <algorithm>
#include <cmath>

namespace nrs {

double jain_index(const std::vector<double>& x) {
  if (x.empty()) return 0.0;
  double sum = 0.0;
  double sum_sq = 0.0;
  for (double v : x) {
    sum += v;
    sum_sq += v * v;
  }
  if (sum_sq == 0.0) return 1.0;
  return (sum * sum) / (static_cast<double>(x.size()) * sum_sq);
}

double percentile(std::vector<double> samples, double p) {
  if (samples.empty()) return 0.0;
  const auto n = static_cast<double>(samples.size());
  auto rank = static_cast<size_t>(std::ceil(p / 100.0 * n));
  rank = std::clamp<size_t>(rank, 1, samples.size());
  std::nth_element(samples.begin(), samples.begin() + static_cast<long>(rank - 1), samples.end());
  return samples[rank - 1];
}

void LatencyHistogram::observe(double us) {
  for (size_t i = 0; i < kBucketsUs.size(); ++i) {
    if (us <= kBucketsUs[i]) ++cum_[i];
  }
  ++count_;
  sum_ += us;
}

KpiCollector::KpiCollector(uint32_t num_prbs, double slot_duration_s, size_t max_latency_samples)
    : num_prbs_(num_prbs), slot_s_(slot_duration_s), max_samples_(max_latency_samples) {}

void KpiCollector::record_slot(const SlotResult& r, const std::vector<UeState>& ues,
                               double sched_time_us) {
  ++slots_;
  total_bytes_ += r.bytes_sent;
  total_prbs_ += r.prbs_used;
  for (const UeState& ue : ues) ue_bytes_.try_emplace(ue.id, 0);
  for (const Allocation& a : r.allocations) ue_bytes_[a.ue_id] += a.bytes;
  latency_us_.push_back(sched_time_us);
  if (max_samples_ != 0 && latency_us_.size() > max_samples_) latency_us_.pop_front();
  hist_.observe(sched_time_us);
}

KpiReport KpiCollector::report() const {
  KpiReport k;
  k.slots = slots_;
  k.duration_s = static_cast<double>(slots_) * slot_s_;
  if (k.duration_s > 0) {
    k.cell_throughput_bps = static_cast<double>(total_bytes_) * 8.0 / k.duration_s;
    std::vector<double> per_ue;
    for (const auto& [id, bytes] : ue_bytes_) {
      const double bps = static_cast<double>(bytes) * 8.0 / k.duration_s;
      k.ue_throughput_bps[id] = bps;
      per_ue.push_back(bps);
    }
    k.jain_fairness = jain_index(per_ue);
    k.prb_utilization =
        static_cast<double>(total_prbs_) / (static_cast<double>(slots_) * num_prbs_);
  }
  const std::vector<double> lat(latency_us_.begin(), latency_us_.end());
  k.sched_time_p50_us = percentile(lat, 50.0);
  k.sched_time_p99_us = percentile(lat, 99.0);
  return k;
}

}  // namespace nrs
