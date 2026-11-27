#include <gtest/gtest.h>

#include "core/cell.h"
#include "sched/scheduler.h"

namespace nrs {
namespace {

// Returns a fixed (possibly invalid) allocation list, to prove the Cell
// rejects whatever a buggy policy proposes.
class FakeScheduler : public Scheduler {
 public:
  explicit FakeScheduler(std::vector<Allocation> a) : allocs_(std::move(a)) {}
  std::vector<Allocation> schedule(const SlotInput&) override { return allocs_; }
  std::string_view name() const override { return "fake"; }

 private:
  std::vector<Allocation> allocs_;
};

TEST(Cell, ReportCreatesUeAndAddsBytes) {
  Cell cell(CellConfig{});
  EXPECT_TRUE(cell.apply_report({5, 12, 1000}));
  EXPECT_TRUE(cell.apply_report({5, 9, 500}));
  ASSERT_EQ(cell.ues().size(), 1u);
  EXPECT_EQ(cell.ues()[0].cqi, 9);
  EXPECT_EQ(cell.ues()[0].buffer_bytes, 1500u);
}

TEST(Cell, UesKeptSortedById) {
  Cell cell(CellConfig{});
  for (int id : {9, 2, 5}) cell.apply_report({static_cast<uint16_t>(id), 10, 1});
  ASSERT_EQ(cell.ues().size(), 3u);
  EXPECT_EQ(cell.ues()[0].id, 2);
  EXPECT_EQ(cell.ues()[1].id, 5);
  EXPECT_EQ(cell.ues()[2].id, 9);
}

TEST(Cell, RejectsBadCqiAndEnforcesMaxUes) {
  CellConfig cfg;
  cfg.max_ues = 2;
  Cell cell(cfg);
  EXPECT_FALSE(cell.apply_report({1, 0, 10}));
  EXPECT_FALSE(cell.apply_report({1, 16, 10}));
  EXPECT_TRUE(cell.apply_report({1, 10, 10}));
  EXPECT_TRUE(cell.apply_report({2, 10, 10}));
  EXPECT_FALSE(cell.apply_report({3, 10, 10}));
  EXPECT_EQ(cell.rejected_reports(), 3u);
  EXPECT_EQ(cell.ues().size(), 2u);
}

TEST(Cell, BufferCapDropsExcessBytes) {
  CellConfig cfg;
  cfg.max_buffer_bytes = 1000;
  Cell cell(cfg);
  cell.apply_report({1, 10, 800});
  cell.apply_report({1, 10, 800});
  EXPECT_EQ(cell.ues()[0].buffer_bytes, 1000u);
  EXPECT_EQ(cell.buffer_overflow_bytes(), 600u);
}

TEST(Cell, RunSlotDrainsBufferAndUpdatesEwma) {
  CellConfig cfg;
  cfg.ewma_time_constant_slots = 100;
  Cell cell(cfg);
  cell.apply_report({1, 15, 250});
  cell.apply_report({2, 15, 0});
  auto sched = make_scheduler(Policy::kRoundRobin);
  SlotResult r = cell.run_slot(*sched);
  EXPECT_EQ(r.bytes_sent, 250u);
  EXPECT_EQ(cell.ues()[0].buffer_bytes, 0u);
  // avg = (1 - 1/100) * 0 + (1/100) * (250*8 bits / 1 ms) = 20 kbit/s
  EXPECT_NEAR(cell.ues()[0].avg_tput_bps, 2000.0 / 0.001 / 100.0, 1e-6);
  EXPECT_EQ(cell.ues()[1].avg_tput_bps, 0.0);

  // Unserved slot: average decays by (1 - 1/100).
  cell.run_slot(*sched);
  EXPECT_NEAR(cell.ues()[0].avg_tput_bps, 20000.0 * 0.99, 1e-6);
}

TEST(Cell, InvalidAllocationsAreDroppedAndCounted) {
  CellConfig cfg;
  cfg.num_prbs = 10;
  Cell cell(cfg);
  cell.apply_report({1, 15, 10'000});
  cell.apply_report({2, 15, 50});
  FakeScheduler bad({
      {1, 8, 800},  // ok: 8 PRBs carry 802 bytes
      {1, 1, 10},   // duplicate UE
      {2, 4, 60},   // more bytes than buffer (and would exceed carrier)
      {9, 1, 1},    // unknown UE
      {2, 1, 50},   // ok
      {2, 0, 0},    // duplicate + zero PRBs
  });
  SlotResult r = cell.run_slot(bad);
  EXPECT_EQ(r.allocations.size(), 2u);
  EXPECT_EQ(r.prbs_used, 9u);
  EXPECT_EQ(r.bytes_sent, 850u);
  EXPECT_EQ(cell.invariant_violations(), 4u);
}

TEST(Cell, RejectsAllocationExceedingCarrier) {
  CellConfig cfg;
  cfg.num_prbs = 10;
  Cell cell(cfg);
  cell.apply_report({1, 15, 10'000});
  FakeScheduler bad({{1, 11, 100}});
  SlotResult r = cell.run_slot(bad);
  EXPECT_TRUE(r.allocations.empty());
  EXPECT_EQ(cell.invariant_violations(), 1u);
}

}  // namespace
}  // namespace nrs
