#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "core/types.h"

namespace nrs {

struct TraceRow {
  uint64_t slot = 0;
  UeReport report;
};

// CSV with header "slot,ue_id,cqi,new_bytes", rows in non-decreasing slot
// order. Format is in docs/interface.md. Throws std::runtime_error with the
// line number on any syntax error, so a corrupt trace never silently changes a benchmark.
std::vector<TraceRow> parse_trace(std::istream& in);
std::vector<TraceRow> load_trace(const std::string& path);

}  // namespace nrs
