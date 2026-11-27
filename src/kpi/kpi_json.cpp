#include "kpi/kpi_json.h"

#include <string>

namespace nrs {

nlohmann::json to_json(const KpiReport& k) {
  nlohmann::json ue = nlohmann::json::object();
  for (const auto& [id, bps] : k.ue_throughput_bps) ue[std::to_string(id)] = bps;
  return {
      {"policy", k.policy},
      {"slots", k.slots},
      {"duration_s", k.duration_s},
      {"cell_throughput_bps", k.cell_throughput_bps},
      {"jain_fairness", k.jain_fairness},
      {"prb_utilization", k.prb_utilization},
      {"sched_time_p50_us", k.sched_time_p50_us},
      {"sched_time_p99_us", k.sched_time_p99_us},
      {"slot_overruns", k.slot_overruns},
      {"malformed_packets", k.malformed_packets},
      {"rejected_reports", k.rejected_reports},
      {"buffer_overflow_bytes", k.buffer_overflow_bytes},
      {"invariant_violations", k.invariant_violations},
      {"ue_throughput_bps", ue},
  };
}

}  // namespace nrs
