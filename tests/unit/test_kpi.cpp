#include <gtest/gtest.h>

#include "kpi/kpi.h"
#include "kpi/kpi_json.h"

namespace nrs {

TEST(Jain, KnownValues) {
  EXPECT_DOUBLE_EQ(jain_index({5, 5, 5, 5}), 1.0);
  EXPECT_DOUBLE_EQ(jain_index({1, 0, 0, 0}), 0.25);  // one UE gets all: 1/n
  // (1+2+3+4)^2 / (4 * (1+4+9+16)) = 100 / 120
  EXPECT_DOUBLE_EQ(jain_index({1, 2, 3, 4}), 100.0 / 120.0);
  EXPECT_DOUBLE_EQ(jain_index({3}), 1.0);
}

TEST(Jain, ScaleInvariant) {
  EXPECT_DOUBLE_EQ(jain_index({1, 2, 3}), jain_index({1000, 2000, 3000}));
}

TEST(Jain, Conventions) {
  EXPECT_EQ(jain_index({}), 0.0);
  EXPECT_EQ(jain_index({0, 0}), 1.0);
}

TEST(Percentile, NearestRank) {
  std::vector<double> v;
  for (int i = 1; i <= 100; ++i) v.push_back(i);
  EXPECT_EQ(percentile(v, 50), 50.0);
  EXPECT_EQ(percentile(v, 99), 99.0);
  EXPECT_EQ(percentile(v, 100), 100.0);
  EXPECT_EQ(percentile({7.0}, 99), 7.0);
  EXPECT_EQ(percentile({}, 50), 0.0);
}

TEST(KpiCollector, ThroughputUtilizationAndUnservedUes) {
  KpiCollector k(10, 0.001);
  std::vector<UeState> ues = {{1, 10, 0, 0}, {2, 10, 0, 0}};
  SlotResult r;
  r.allocations = {{1, 5, 1000}};
  r.prbs_used = 5;
  r.bytes_sent = 1000;
  k.record_slot(r, ues, 3.0);
  k.record_slot(SlotResult{}, ues, 5.0);
  KpiReport rep = k.report();
  EXPECT_EQ(rep.slots, 2u);
  EXPECT_DOUBLE_EQ(rep.cell_throughput_bps, 1000 * 8 / 0.002);
  EXPECT_DOUBLE_EQ(rep.prb_utilization, 5.0 / 20.0);
  // UE 2 was never served but still counts in fairness.
  ASSERT_EQ(rep.ue_throughput_bps.size(), 2u);
  EXPECT_DOUBLE_EQ(rep.ue_throughput_bps[2], 0.0);
  EXPECT_DOUBLE_EQ(rep.jain_fairness, 0.5);
  EXPECT_EQ(rep.sched_time_p50_us, 3.0);
  EXPECT_EQ(rep.sched_time_p99_us, 5.0);
}

TEST(KpiCollector, LatencyWindowIsBounded) {
  KpiCollector k(10, 0.001, 3);
  for (double us : {100.0, 1.0, 2.0, 3.0}) k.record_slot(SlotResult{}, {}, us);
  EXPECT_EQ(k.report().sched_time_p99_us, 3.0);  // the 100 us sample fell out of the window
  EXPECT_EQ(k.histogram().count(), 4u);           // the histogram keeps everything
}

TEST(LatencyHistogram, CumulativeBuckets) {
  LatencyHistogram h;
  h.observe(0.5);
  h.observe(3);
  h.observe(5000);
  EXPECT_EQ(h.cumulative_counts()[0], 1u);  // le=1
  EXPECT_EQ(h.cumulative_counts()[2], 2u);  // le=5
  EXPECT_EQ(h.cumulative_counts().back(), 2u);
  EXPECT_EQ(h.count(), 3u);
}

TEST(KpiJson, HasExpectedFields) {
  KpiReport k;
  k.policy = "round_robin";
  k.ue_throughput_bps[3] = 1.5;
  auto j = to_json(k);
  EXPECT_EQ(j["policy"], "round_robin");
  EXPECT_EQ(j["ue_throughput_bps"]["3"], 1.5);
  for (const char* key : {"cell_throughput_bps", "jain_fairness", "prb_utilization",
                          "sched_time_p50_us", "sched_time_p99_us", "slot_overruns",
                          "malformed_packets"}) {
    EXPECT_TRUE(j.contains(key)) << key;
  }
}

}  // namespace nrs
