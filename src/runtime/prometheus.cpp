#include "runtime/prometheus.h"

#include <cstdio>
#include <sstream>

namespace nrs {
namespace {

class Writer {
 public:
  void header(const char* name, const char* type, const char* help) {
    out_ << "# HELP " << name << ' ' << help << "\n# TYPE " << name << ' ' << type << '\n';
  }
  void sample(const std::string& name_and_labels, double v) {
    char buf[64];
    // %.17g round-trips doubles exactly; integers print without a decimal point.
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    out_ << name_and_labels << ' ' << buf << '\n';
  }
  void sample(const std::string& name_and_labels, uint64_t v) {
    out_ << name_and_labels << ' ' << v << '\n';
  }
  void metric(const char* name, const char* type, const char* help, uint64_t v) {
    header(name, type, help);
    sample(name, v);
  }
  void metric(const char* name, const char* type, const char* help, double v) {
    header(name, type, help);
    sample(name, v);
  }
  std::string str() const { return out_.str(); }

 private:
  std::ostringstream out_;
};

}  // namespace

std::string render_prometheus(const SlotLoopSnapshot& s, const ReceiverStats& rx) {
  Writer w;
  w.header("nrs_info", "gauge", "Static scheduler information.");
  w.sample("nrs_info{policy=\"" + s.policy + "\",num_prbs=\"" + std::to_string(s.num_prbs) + "\"}",
           uint64_t{1});

  w.metric("nrs_slots_total", "counter", "Slots executed.", s.slots);
  w.metric("nrs_slot_overruns_total", "counter", "Slots that finished after their deadline.",
           s.overruns);
  w.metric("nrs_prbs_allocated_total", "counter", "PRBs allocated across all slots.",
           s.prbs_allocated);
  w.metric("nrs_bytes_scheduled_total", "counter", "Downlink bytes scheduled.", s.bytes_scheduled);
  w.metric("nrs_cell_throughput_bps", "gauge", "Mean cell throughput since start.",
           s.kpi.cell_throughput_bps);
  w.metric("nrs_prb_utilization", "gauge", "Fraction of PRBs used since start.",
           s.kpi.prb_utilization);
  w.metric("nrs_jain_fairness", "gauge", "Jain fairness index of per-UE throughput since start.",
           s.kpi.jain_fairness);
  w.metric("nrs_sched_time_p50_us", "gauge", "Median scheduling time, recent window.",
           s.kpi.sched_time_p50_us);
  w.metric("nrs_sched_time_p99_us", "gauge", "p99 scheduling time, recent window.",
           s.kpi.sched_time_p99_us);

  w.header("nrs_sched_time_us", "histogram", "Per-slot scheduling time in microseconds.");
  const auto& cum = s.sched_hist.cumulative_counts();
  for (size_t i = 0; i < cum.size(); ++i) {
    char le[32];
    std::snprintf(le, sizeof(le), "%g", LatencyHistogram::kBucketsUs[i]);
    w.sample(std::string("nrs_sched_time_us_bucket{le=\"") + le + "\"}", cum[i]);
  }
  w.sample("nrs_sched_time_us_bucket{le=\"+Inf\"}", s.sched_hist.count());
  w.sample("nrs_sched_time_us_sum", s.sched_hist.sum());
  w.sample("nrs_sched_time_us_count", s.sched_hist.count());

  w.metric("nrs_rejected_reports_total", "counter", "Reports dropped by the cell (bad CQI, UE table full).",
           s.rejected_reports);
  w.metric("nrs_buffer_overflow_bytes_total", "counter", "Bytes dropped because a UE buffer was full.",
           s.buffer_overflow_bytes);
  w.metric("nrs_invariant_violations_total", "counter",
           "Allocations rejected by the cell's safety checks (indicates a scheduler bug).",
           s.invariant_violations);

  w.metric("nrs_datagrams_received_total", "counter", "UDP datagrams received.", rx.datagrams);
  w.metric("nrs_reports_received_total", "counter", "UE reports parsed and queued.", rx.reports);
  w.metric("nrs_queue_drops_total", "counter", "Reports dropped because the queue was full.",
           rx.queue_drops);
  w.metric("nrs_malformed_packets_total", "counter", "Datagrams rejected by the parser.",
           rx.malformed_total());
  w.header("nrs_malformed_packets_by_reason_total", "counter", "Rejected datagrams by reason.");
  for (size_t i = 1; i < rx.malformed.size(); ++i) {
    const auto reason = wire::to_string(static_cast<wire::ParseError>(i));
    w.sample("nrs_malformed_packets_by_reason_total{reason=\"" + std::string(reason) + "\"}",
             rx.malformed[i]);
  }

  w.metric("nrs_ues", "gauge", "UEs known to the cell.", static_cast<uint64_t>(s.ues.size()));
  w.header("nrs_ue_cqi", "gauge", "Latest CQI per UE.");
  for (const auto& u : s.ues) {
    w.sample("nrs_ue_cqi{ue=\"" + std::to_string(u.id) + "\"}", uint64_t{u.cqi});
  }
  w.header("nrs_ue_buffer_bytes", "gauge", "Downlink buffer occupancy per UE.");
  for (const auto& u : s.ues) {
    w.sample("nrs_ue_buffer_bytes{ue=\"" + std::to_string(u.id) + "\"}", u.buffer_bytes);
  }
  w.header("nrs_ue_avg_throughput_bps", "gauge", "EWMA throughput per UE (the PF denominator).");
  for (const auto& u : s.ues) {
    w.sample("nrs_ue_avg_throughput_bps{ue=\"" + std::to_string(u.id) + "\"}", u.avg_tput_bps);
  }
  w.header("nrs_ue_bytes_scheduled_total", "counter", "Bytes scheduled per UE.");
  for (const auto& u : s.ues) {
    w.sample("nrs_ue_bytes_scheduled_total{ue=\"" + std::to_string(u.id) + "\"}",
             u.bytes_scheduled);
  }
  return w.str();
}

}  // namespace nrs
