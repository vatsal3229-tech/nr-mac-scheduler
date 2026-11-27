#pragma once

#include <cstdint>
#include <vector>

namespace nrs {

struct CellConfig {
  uint16_t num_prbs = 52;  // 10 MHz at 15 kHz SCS
  double overhead = 0.14;  // TS 38.306 FR1 downlink overhead factor
  double ewma_time_constant_slots = 100.0;
  uint32_t slot_duration_us = 1000;  // numerology 0
  uint32_t max_ues = 1024;
  uint64_t max_buffer_bytes = 4'000'000;

  double slot_duration_s() const { return slot_duration_us * 1e-6; }
};

// One UE's report: latest CQI plus bytes that arrived in its downlink buffer.
struct UeReport {
  uint16_t ue_id = 0;
  uint8_t cqi = 0;
  uint32_t new_bytes = 0;
};

struct UeState {
  uint16_t id = 0;
  uint8_t cqi = 0;
  uint64_t buffer_bytes = 0;
  double avg_tput_bps = 0.0;  // EWMA of served rate, updated every slot
};

struct Allocation {
  uint16_t ue_id = 0;
  uint32_t num_prbs = 0;
  uint32_t bytes = 0;
};

struct SlotResult {
  std::vector<Allocation> allocations;  // only those that passed validation
  uint32_t prbs_used = 0;
  uint64_t bytes_sent = 0;
};

}  // namespace nrs
