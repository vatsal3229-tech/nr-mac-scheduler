#pragma once

#include <string>

#include "runtime/metrics.h"
#include "runtime/receiver_stats.h"

namespace nrs {

// Prometheus text exposition format 0.0.4. Pure function so it is unit-testable
// without sockets. Metric names are listed in docs/interface.md.
std::string render_prometheus(const SlotLoopSnapshot& s, const ReceiverStats& rx);

}  // namespace nrs
