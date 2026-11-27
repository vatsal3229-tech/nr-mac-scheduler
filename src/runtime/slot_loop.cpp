#include "runtime/slot_loop.h"

#include <string>

namespace nrs {
namespace {
// Live mode keeps only the most recent samples for p50/p99 so memory stays flat.
constexpr size_t kLatencyWindow = 4096;
// Publishing copies per-UE state under a lock; every 10 slots (10 ms) is far
// finer than any Prometheus scrape interval and keeps the hot path cheap.
constexpr uint64_t kPublishEverySlots = 10;
}  // namespace

SlotLoop::SlotLoop(Cell& cell, Scheduler& scheduler, ConcurrentQueue<UeReport>& queue,
                   MetricsHub& hub)
    : cell_(cell),
      scheduler_(scheduler),
      queue_(queue),
      hub_(hub),
      kpi_(cell.config().num_prbs, cell.config().slot_duration_s(), kLatencyWindow) {}

SlotLoop::~SlotLoop() { stop(); }

void SlotLoop::start() {
  stop_.store(false);
  thread_ = std::thread([this] { run(); });
}

void SlotLoop::stop() {
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
}

void SlotLoop::run() {
  using clock = std::chrono::steady_clock;
  const auto slot = std::chrono::microseconds(cell_.config().slot_duration_us);
  auto slot_start = clock::now();

  while (!stop_.load(std::memory_order_relaxed)) {
    const auto deadline = slot_start + slot;

    inbox_.clear();
    queue_.drain(inbox_);
    for (const UeReport& r : inbox_) cell_.apply_report(r);

    const auto t0 = clock::now();
    SlotResult result = cell_.run_slot(scheduler_);
    const auto t1 = clock::now();
    kpi_.record_slot(result, cell_.ues(), std::chrono::duration<double, std::micro>(t1 - t0).count());
    ++slots_;

    hub_.last_slot_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                clock::now().time_since_epoch())
                                .count(),
                            std::memory_order_relaxed);
    if (slots_ % kPublishEverySlots == 1) publish();

    const auto now = clock::now();
    if (now > deadline) {
      // Missed the slot boundary: in a real DU the transport block would miss
      // its air-interface slot. Count it, and if we are more than a full slot
      // behind, resynchronise instead of bursting to "catch up" on stale slots.
      ++overruns_;
      slot_start = (now - deadline > slot) ? now : deadline;
      continue;
    }
    std::this_thread::sleep_until(deadline);
    slot_start = deadline;
  }
  publish();
}

void SlotLoop::publish() {
  SlotLoopSnapshot s;
  s.policy = std::string(scheduler_.name());
  s.num_prbs = cell_.config().num_prbs;
  s.slots = slots_;
  s.overruns = overruns_;
  s.prbs_allocated = kpi_.total_prbs();
  s.bytes_scheduled = kpi_.total_bytes();
  s.rejected_reports = cell_.rejected_reports();
  s.buffer_overflow_bytes = cell_.buffer_overflow_bytes();
  s.invariant_violations = cell_.invariant_violations();
  s.kpi = kpi_.report();
  s.sched_hist = kpi_.histogram();
  const auto& ue_bytes = kpi_.ue_bytes();
  for (const UeState& ue : cell_.ues()) {
    auto it = ue_bytes.find(ue.id);
    s.ues.push_back({ue.id, ue.cqi, ue.buffer_bytes, ue.avg_tput_bps,
                     it == ue_bytes.end() ? 0 : it->second});
  }
  hub_.publish(std::move(s));
  hub_.ready.store(true);
}

}  // namespace nrs
