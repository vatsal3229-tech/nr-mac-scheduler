#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>

#include "offline/replay.h"

namespace nrs {
namespace {

// Seeded trace with the conditions under which PF is known to beat RR:
// full buffers and CQIs that fade independently per UE, around different
// means (near and far UEs). Built in C++ so the test does not depend on the
// Python simulator. mt19937 output is fully specified by the standard, and we
// avoid std::*_distribution (implementation-defined), so the trace is identical everywhere.
std::vector<TraceRow> fading_trace(uint64_t slots, int num_ues, uint32_t seed) {
  std::mt19937 rng(seed);
  constexpr double kPi = 3.14159265358979323846;
  struct Ue {
    double mean, amp, period, phase;
  };
  std::vector<Ue> ues;
  for (int i = 0; i < num_ues; ++i) {
    const double mean = 3.0 + 10.0 * i / std::max(1, num_ues - 1);  // CQI 3 .. 13
    const double period = 40.0 + static_cast<double>(rng() % 160);  // 40-199 slots
    const double phase = 2.0 * kPi * static_cast<double>(rng() % 1000) / 1000.0;
    ues.push_back({mean, 3.0, period, phase});
  }
  std::vector<TraceRow> rows;
  for (uint64_t s = 0; s < slots; ++s) {
    for (int i = 0; i < num_ues; ++i) {
      const Ue& u = ues[static_cast<size_t>(i)];
      const double noise = static_cast<double>(static_cast<int>(rng() % 3) - 1);  // -1, 0, +1
      const double v = u.mean +
                       u.amp * std::sin(2.0 * kPi * static_cast<double>(s) / u.period + u.phase) +
                       noise;
      const auto cqi = static_cast<uint8_t>(std::clamp(std::lround(v), 1L, 15L));
      // 10 kB/slot per UE exceeds what the whole cell can serve: full buffer.
      rows.push_back({s, {static_cast<uint16_t>(i), cqi, 10'000}});
    }
  }
  return rows;
}

KpiReport run_policy(Policy p, const std::vector<TraceRow>& trace, uint64_t slots) {
  Config cfg;
  cfg.policy = p;
  return replay(cfg, trace, slots);
}

TEST(PolicyComparison, PfBeatsRrOnThroughputAndMaxTOnFairness) {
  constexpr uint64_t kSlots = 20'000;
  const auto trace = fading_trace(kSlots, 10, 42);
  const KpiReport rr = run_policy(Policy::kRoundRobin, trace, kSlots);
  const KpiReport pf = run_policy(Policy::kProportionalFair, trace, kSlots);
  const KpiReport mt = run_policy(Policy::kMaxThroughput, trace, kSlots);

  RecordProperty("rr_mbps", std::to_string(rr.cell_throughput_bps / 1e6));
  RecordProperty("pf_mbps", std::to_string(pf.cell_throughput_bps / 1e6));
  RecordProperty("mt_mbps", std::to_string(mt.cell_throughput_bps / 1e6));
  std::printf("RR  %.2f Mbps jain %.3f\nPF  %.2f Mbps jain %.3f\nMT  %.2f Mbps jain %.3f\n",
              rr.cell_throughput_bps / 1e6, rr.jain_fairness, pf.cell_throughput_bps / 1e6,
              pf.jain_fairness, mt.cell_throughput_bps / 1e6, mt.jain_fairness);

  EXPECT_GT(pf.cell_throughput_bps, rr.cell_throughput_bps);
  EXPECT_GT(pf.jain_fairness, mt.jain_fairness);
  // Sanity on the extremes: with full buffers max-throughput is the per-slot optimum.
  EXPECT_GE(mt.cell_throughput_bps, pf.cell_throughput_bps);
  for (const KpiReport* k : {&rr, &pf, &mt}) {
    EXPECT_EQ(k->invariant_violations, 0u);
    EXPECT_NEAR(k->prb_utilization, 1.0, 1e-9);  // full buffer: every PRB used
  }
}

TEST(PolicyComparison, ReplayIsDeterministic) {
  const auto trace = fading_trace(2000, 6, 7);
  const KpiReport a = run_policy(Policy::kProportionalFair, trace, 2000);
  const KpiReport b = run_policy(Policy::kProportionalFair, trace, 2000);
  EXPECT_EQ(a.cell_throughput_bps, b.cell_throughput_bps);
  EXPECT_EQ(a.ue_throughput_bps, b.ue_throughput_bps);
}

}  // namespace
}  // namespace nrs
