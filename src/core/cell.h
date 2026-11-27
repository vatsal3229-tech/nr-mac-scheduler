#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "phy/cqi_table.h"

namespace nrs {

class Scheduler;

// Owns all per-UE state for one cell. A scheduler only *proposes* allocations;
// the Cell validates them, drains buffers and updates averages. Keeping the
// side effects here means every policy obeys the same invariants and each
// policy can be tested as a pure function.
class Cell {
 public:
  explicit Cell(const CellConfig& cfg);

  // Returns false if the report was dropped (bad CQI or UE table full).
  bool apply_report(const UeReport& report);

  SlotResult run_slot(Scheduler& scheduler);

  const std::vector<UeState>& ues() const { return ues_; }
  const CellConfig& config() const { return cfg_; }
  const LinkModel& link() const { return link_; }

  uint64_t buffer_overflow_bytes() const { return buffer_overflow_bytes_; }
  uint64_t invariant_violations() const { return invariant_violations_; }
  uint64_t rejected_reports() const { return rejected_reports_; }

 private:
  UeState* find_or_create(uint16_t ue_id);
  void reindex();

  CellConfig cfg_;
  LinkModel link_;
  std::vector<UeState> ues_;  // sorted by id: deterministic iteration order
  std::unordered_map<uint16_t, size_t> index_;
  std::vector<uint64_t> served_bits_scratch_;
  uint64_t buffer_overflow_bytes_ = 0;
  uint64_t invariant_violations_ = 0;
  uint64_t rejected_reports_ = 0;
};

}  // namespace nrs
