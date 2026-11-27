#include <gtest/gtest.h>

#include <map>
#include <random>

#include "sched/policies.h"
#include "sched/scheduler.h"

namespace nrs {
namespace {

constexpr double kSlotS = 0.001;

UeState ue(uint16_t id, uint8_t cqi, uint64_t buffer, double avg = 0.0) {
  return UeState{id, cqi, buffer, avg};
}

std::vector<Allocation> run(Scheduler& s, const std::vector<UeState>& ues, uint32_t prbs,
                            const LinkModel& link) {
  return s.schedule(SlotInput{ues, prbs, link, kSlotS});
}

// Invariants every policy must satisfy, checked over many random slots.
class AllPolicies : public ::testing::TestWithParam<Policy> {};

TEST_P(AllPolicies, RespectsPrbBufferAndCapacityLimits) {
  LinkModel link(0.14);
  auto sched = make_scheduler(GetParam());
  std::mt19937 rng(1234);
  for (int trial = 0; trial < 2000; ++trial) {
    const auto prbs = static_cast<uint32_t>(1 + rng() % 106);
    std::vector<UeState> ues;
    const int n = 1 + static_cast<int>(rng() % 12);
    for (int i = 0; i < n; ++i) {
      ues.push_back(ue(static_cast<uint16_t>(i), static_cast<uint8_t>(rng() % 16),
                       rng() % 4 == 0 ? 0 : rng() % 20000, static_cast<double>(rng() % 1000000)));
    }
    const auto allocs = run(*sched, ues, prbs, link);
    uint32_t total = 0;
    std::map<uint16_t, int> count;
    for (const auto& a : allocs) {
      const UeState& u = ues[a.ue_id];
      total += a.num_prbs;
      EXPECT_GT(a.num_prbs, 0u);
      EXPECT_LE(a.bytes, u.buffer_bytes) << "more bytes than buffer";
      EXPECT_LE(a.bytes, link.bytes_for_prbs(u.cqi, a.num_prbs)) << "more bytes than PRBs carry";
      EXPECT_GT(u.buffer_bytes, 0u) << "empty-buffer UE scheduled";
      EXPECT_GT(u.cqi, 0) << "CQI-0 UE scheduled";
      EXPECT_EQ(++count[a.ue_id], 1) << "UE allocated twice";
    }
    EXPECT_LE(total, prbs) << "over-allocated carrier";
  }
}

TEST_P(AllPolicies, EmptyBuffersGetNothing) {
  LinkModel link(0.14);
  auto sched = make_scheduler(GetParam());
  std::vector<UeState> ues = {ue(1, 15, 0), ue(2, 9, 0), ue(3, 3, 0)};
  EXPECT_TRUE(run(*sched, ues, 52, link).empty());
}

TEST_P(AllPolicies, LoneUeDoesNotTakeMorePrbsThanItsBufferNeeds) {
  LinkModel link(0.14);
  auto sched = make_scheduler(GetParam());
  std::vector<UeState> ues = {ue(7, 15, 250)};  // 2000 bits -> 3 PRBs at 802 bits each
  auto a = run(*sched, ues, 52, link);
  ASSERT_EQ(a.size(), 1u);
  EXPECT_EQ(a[0].num_prbs, 3u);
  EXPECT_EQ(a[0].bytes, 250u);
}

INSTANTIATE_TEST_SUITE_P(Policies, AllPolicies,
                         ::testing::Values(Policy::kRoundRobin, Policy::kProportionalFair,
                                           Policy::kMaxThroughput),
                         [](const auto& p) { return std::string(to_string(p.param)); });

TEST(RoundRobin, EqualSharesForEqualCqi) {
  LinkModel link(0.14);
  RoundRobinScheduler rr;
  std::vector<UeState> ues;
  for (uint16_t i = 0; i < 4; ++i) ues.push_back(ue(i, 10, 1'000'000));
  auto a = run(rr, ues, 52, link);
  ASSERT_EQ(a.size(), 4u);
  for (const auto& x : a) EXPECT_EQ(x.num_prbs, 13u);
}

TEST(RoundRobin, RemainderRotatesSoTotalsEqualOverTime) {
  LinkModel link(0.14);
  RoundRobinScheduler rr;
  std::vector<UeState> ues;
  for (uint16_t i = 0; i < 5; ++i) ues.push_back(ue(i, 10, 1'000'000));
  std::map<uint16_t, uint32_t> total;
  // 52 = 5*10 + 2: two leftover PRBs per slot; after 5 slots each UE had 2 extra.
  for (int slot = 0; slot < 5; ++slot) {
    for (const auto& x : run(rr, ues, 52, link)) total[x.ue_id] += x.num_prbs;
  }
  for (uint16_t i = 0; i < 5; ++i) EXPECT_EQ(total[i], 52u) << "ue " << i;
}

TEST(RoundRobin, UnusedShareIsRedistributed) {
  LinkModel link(0.14);
  RoundRobinScheduler rr;
  // UE 0 needs only 1 PRB; the other 51 split between UEs 1 and 2.
  std::vector<UeState> ues = {ue(0, 15, 10), ue(1, 10, 1'000'000), ue(2, 10, 1'000'000)};
  auto a = run(rr, ues, 52, link);
  ASSERT_EQ(a.size(), 3u);
  EXPECT_EQ(a[0].num_prbs, 1u);
  EXPECT_EQ(a[1].num_prbs + a[2].num_prbs, 51u);
}

TEST(ProportionalFair, PrefersUeWithHigherRateToAverageRatio) {
  LinkModel link(0.14);
  ProportionalFairScheduler pf;
  // UE 1 has the better channel but has already been served a lot; UE 2 has a
  // worse channel and a tiny average, so its ratio is higher.
  std::vector<UeState> ues = {ue(1, 15, 1'000'000, 40e6), ue(2, 7, 1'000'000, 1e5)};
  auto a = run(pf, ues, 52, link);
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a[0].ue_id, 2);
  EXPECT_EQ(a[0].num_prbs, 52u);
}

TEST(ProportionalFair, EqualAveragesFallBackToBestChannel) {
  LinkModel link(0.14);
  ProportionalFairScheduler pf;
  std::vector<UeState> ues = {ue(1, 5, 1'000'000, 1e6), ue(2, 12, 1'000'000, 1e6)};
  auto a = run(pf, ues, 52, link);
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a[0].ue_id, 2);
}

TEST(MaxThroughput, HighestCqiFirstAndLeftoversToNext) {
  LinkModel link(0.14);
  MaxThroughputScheduler mt;
  // UE 3 (CQI 15) needs only 3 PRBs; the rest go to UE 1 (CQI 9), UE 2 (CQI 4) gets none.
  std::vector<UeState> ues = {ue(1, 9, 1'000'000), ue(2, 4, 1'000'000), ue(3, 15, 250)};
  auto a = run(mt, ues, 52, link);
  ASSERT_EQ(a.size(), 2u);
  EXPECT_EQ(a[0].ue_id, 3);
  EXPECT_EQ(a[0].num_prbs, 3u);
  EXPECT_EQ(a[1].ue_id, 1);
  EXPECT_EQ(a[1].num_prbs, 49u);
}

TEST(PolicyNames, ParseAndPrintRoundTrip) {
  for (Policy p : {Policy::kRoundRobin, Policy::kProportionalFair, Policy::kMaxThroughput}) {
    EXPECT_EQ(parse_policy(to_string(p)), p);
  }
  EXPECT_EQ(parse_policy("pf"), Policy::kProportionalFair);
  EXPECT_FALSE(parse_policy("fastest").has_value());
}

}  // namespace
}  // namespace nrs
