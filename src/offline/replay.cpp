#include "offline/replay.h"

#include <chrono>
#include <string>

#include "core/cell.h"

namespace nrs {

KpiReport replay(const Config& cfg, const std::vector<TraceRow>& trace, uint64_t num_slots) {
  Cell cell(cfg.cell);
  auto scheduler = make_scheduler(cfg.policy);
  KpiCollector kpi(cfg.cell.num_prbs, cfg.cell.slot_duration_s());

  size_t next = 0;
  for (uint64_t slot = 0; slot < num_slots; ++slot) {
    // Reports stamped with slot s are visible to the scheduler in slot s.
    while (next < trace.size() && trace[next].slot <= slot) cell.apply_report(trace[next++].report);
    const auto t0 = std::chrono::steady_clock::now();
    SlotResult r = cell.run_slot(*scheduler);
    const auto t1 = std::chrono::steady_clock::now();
    kpi.record_slot(r, cell.ues(), std::chrono::duration<double, std::micro>(t1 - t0).count());
  }

  KpiReport k = kpi.report();
  k.policy = std::string(to_string(cfg.policy));
  k.rejected_reports = cell.rejected_reports();
  k.buffer_overflow_bytes = cell.buffer_overflow_bytes();
  k.invariant_violations = cell.invariant_violations();
  return k;
}

}  // namespace nrs
