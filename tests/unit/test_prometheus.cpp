#include <gtest/gtest.h>

#include "runtime/prometheus.h"

namespace nrs {

TEST(Prometheus, RendersCoreMetricsAndLabels) {
  SlotLoopSnapshot s;
  s.policy = "proportional_fair";
  s.num_prbs = 52;
  s.slots = 1000;
  s.overruns = 3;
  s.kpi.prb_utilization = 0.5;
  s.sched_hist.observe(4);
  s.ues = {{7, 12, 100, 2.5e6, 4000}};
  ReceiverStats rx;
  rx.datagrams = 10;
  rx.malformed[static_cast<size_t>(wire::ParseError::kBadMagic)] = 2;

  const std::string out = render_prometheus(s, rx);
  const auto has = [&](const std::string& line) {
    EXPECT_NE(out.find(line + "\n"), std::string::npos) << "missing: " << line;
  };
  has("# TYPE nrs_slots_total counter");
  has("nrs_slots_total 1000");
  has("nrs_slot_overruns_total 3");
  has("nrs_prb_utilization 0.5");
  has("nrs_info{policy=\"proportional_fair\",num_prbs=\"52\"} 1");
  has("nrs_malformed_packets_total 2");
  has("nrs_malformed_packets_by_reason_total{reason=\"bad_magic\"} 2");
  has("nrs_ue_cqi{ue=\"7\"} 12");
  has("nrs_ue_bytes_scheduled_total{ue=\"7\"} 4000");
  has("nrs_sched_time_us_bucket{le=\"5\"} 1");
  has("nrs_sched_time_us_bucket{le=\"+Inf\"} 1");
  EXPECT_EQ(out.find("reason=\"ok\""), std::string::npos);
}

}  // namespace nrs
