#pragma once

#include <cstdint>
#include <vector>

#include "config/config.h"
#include "kpi/kpi.h"
#include "offline/trace.h"

namespace nrs {

// Runs `num_slots` slots back to back (no sleeping). Results depend only on
// the trace and config, never on wall-clock time, so CI and benchmarks are
// reproducible. Scheduling-time percentiles are measured but are the one
// non-deterministic field; the performance gate does not use them.
KpiReport replay(const Config& cfg, const std::vector<TraceRow>& trace, uint64_t num_slots);

}  // namespace nrs
