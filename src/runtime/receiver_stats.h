#pragma once

#include <array>
#include <cstdint>

#include "proto/wire.h"

namespace nrs {

// Plain copy of the receiver counters, safe to pass between threads.
struct ReceiverStats {
  uint64_t datagrams = 0;
  uint64_t reports = 0;
  uint64_t queue_drops = 0;
  std::array<uint64_t, wire::kParseErrorCount> malformed{};

  uint64_t malformed_total() const {
    uint64_t t = 0;
    for (uint64_t v : malformed) t += v;
    return t;
  }
};

}  // namespace nrs
