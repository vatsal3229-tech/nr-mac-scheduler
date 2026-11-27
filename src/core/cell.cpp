#include "core/cell.h"

#include <algorithm>

#include "sched/scheduler.h"

namespace nrs {

Cell::Cell(const CellConfig& cfg) : cfg_(cfg), link_(cfg.overhead) {}

void Cell::reindex() {
  index_.clear();
  for (size_t i = 0; i < ues_.size(); ++i) index_[ues_[i].id] = i;
}

UeState* Cell::find_or_create(uint16_t ue_id) {
  if (auto it = index_.find(ue_id); it != index_.end()) return &ues_[it->second];
  // Bounded so a flood of spoofed UE ids cannot grow memory without limit.
  if (ues_.size() >= cfg_.max_ues) return nullptr;
  auto pos = std::lower_bound(ues_.begin(), ues_.end(), ue_id,
                              [](const UeState& u, uint16_t id) { return u.id < id; });
  UeState fresh;
  fresh.id = ue_id;
  pos = ues_.insert(pos, fresh);
  const size_t at = static_cast<size_t>(pos - ues_.begin());
  reindex();  // attach is rare compared to slots, so O(n) here is fine
  return &ues_[at];
}

bool Cell::apply_report(const UeReport& report) {
  if (report.cqi < kMinCqi || report.cqi > kMaxCqi) {
    ++rejected_reports_;
    return false;
  }
  UeState* ue = find_or_create(report.ue_id);
  if (ue == nullptr) {
    ++rejected_reports_;
    return false;
  }
  ue->cqi = report.cqi;
  // Finite RLC buffer: bytes beyond the cap are dropped, like a real tail-drop queue.
  const uint64_t room = cfg_.max_buffer_bytes - std::min(cfg_.max_buffer_bytes, ue->buffer_bytes);
  const uint64_t accepted = std::min<uint64_t>(room, report.new_bytes);
  ue->buffer_bytes += accepted;
  buffer_overflow_bytes_ += report.new_bytes - accepted;
  return true;
}

SlotResult Cell::run_slot(Scheduler& scheduler) {
  const SlotInput input{ues_, cfg_.num_prbs, link_, cfg_.slot_duration_s()};
  std::vector<Allocation> proposed = scheduler.schedule(input);

  SlotResult result;
  served_bits_scratch_.assign(ues_.size(), 0);
  std::vector<bool> seen(ues_.size(), false);

  // Defence in depth: a buggy policy must not be able to over-allocate the
  // carrier or send bytes that do not exist. Bad allocations are dropped and counted.
  for (const Allocation& a : proposed) {
    auto it = index_.find(a.ue_id);
    if (it == index_.end() || seen[it->second] || a.num_prbs == 0) {
      ++invariant_violations_;
      continue;
    }
    UeState& ue = ues_[it->second];
    const bool over_carrier = result.prbs_used + a.num_prbs > cfg_.num_prbs;
    const bool over_buffer = a.bytes > ue.buffer_bytes;
    const bool over_capacity = a.bytes > link_.bytes_for_prbs(ue.cqi, a.num_prbs);
    if (over_carrier || over_buffer || over_capacity) {
      ++invariant_violations_;
      continue;
    }
    seen[it->second] = true;
    ue.buffer_bytes -= a.bytes;
    served_bits_scratch_[it->second] = static_cast<uint64_t>(a.bytes) * 8;
    result.prbs_used += a.num_prbs;
    result.bytes_sent += a.bytes;
    result.allocations.push_back(a);
  }

  // EWMA over every UE, including unscheduled ones, so a starved UE's average
  // decays and PF raises its priority over time.
  const double alpha = 1.0 / cfg_.ewma_time_constant_slots;
  const double slot_s = cfg_.slot_duration_s();
  for (size_t i = 0; i < ues_.size(); ++i) {
    const double rate = static_cast<double>(served_bits_scratch_[i]) / slot_s;
    ues_[i].avg_tput_bps = (1.0 - alpha) * ues_[i].avg_tput_bps + alpha * rate;
  }
  return result;
}

}  // namespace nrs
