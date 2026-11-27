#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "core/cell.h"
#include "kpi/kpi.h"
#include "runtime/concurrent_queue.h"
#include "runtime/metrics.h"
#include "sched/scheduler.h"

namespace nrs {

// The real-time part: one iteration per slot, paced with sleep_until on an
// absolute steady_clock deadline (sleeping a relative 1 ms would accumulate drift).
class SlotLoop {
 public:
  SlotLoop(Cell& cell, Scheduler& scheduler, ConcurrentQueue<UeReport>& queue, MetricsHub& hub);
  ~SlotLoop();
  SlotLoop(const SlotLoop&) = delete;
  SlotLoop& operator=(const SlotLoop&) = delete;

  void start();
  void stop();

 private:
  void run();
  void publish();

  Cell& cell_;
  Scheduler& scheduler_;
  ConcurrentQueue<UeReport>& queue_;
  MetricsHub& hub_;
  KpiCollector kpi_;
  std::vector<UeReport> inbox_;
  uint64_t slots_ = 0;
  uint64_t overruns_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace nrs
