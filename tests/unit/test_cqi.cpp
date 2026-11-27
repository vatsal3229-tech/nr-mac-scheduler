#include <gtest/gtest.h>

#include "phy/cqi_table.h"

namespace nrs {

TEST(Cqi, TableMatchesTs38214Table1) {
  EXPECT_DOUBLE_EQ(cqi_to_efficiency(1), 0.1523);
  EXPECT_DOUBLE_EQ(cqi_to_efficiency(6), 1.1758);
  EXPECT_DOUBLE_EQ(cqi_to_efficiency(7), 1.4766);   // first 16QAM entry
  EXPECT_DOUBLE_EQ(cqi_to_efficiency(10), 2.7305);  // first 64QAM entry
  EXPECT_DOUBLE_EQ(cqi_to_efficiency(15), 5.5547);
}

TEST(Cqi, OutOfRangeIsZero) {
  EXPECT_EQ(cqi_to_efficiency(0), 0.0);
  EXPECT_EQ(cqi_to_efficiency(16), 0.0);
  EXPECT_EQ(cqi_to_efficiency(255), 0.0);
}

TEST(Cqi, EfficiencyStrictlyIncreasing) {
  for (uint8_t c = 2; c <= kMaxCqi; ++c) {
    EXPECT_GT(cqi_to_efficiency(c), cqi_to_efficiency(static_cast<uint8_t>(c - 1))) << int{c};
  }
}

TEST(Cqi, BitsPerPrbFormula) {
  // 5.5547 * 12 * 14 * 0.86 = 802.54 -> floor 802
  EXPECT_EQ(bits_per_prb(15, 0.14), 802u);
  // 0.1523 * 168 = 25.59 -> 25
  EXPECT_EQ(bits_per_prb(1, 0.0), 25u);
  EXPECT_EQ(bits_per_prb(0, 0.14), 0u);
}

TEST(LinkModel, PrbsForBytesRoundsUp) {
  LinkModel link(0.14);
  ASSERT_EQ(link.bits_per_prb(15), 802u);
  EXPECT_EQ(link.prbs_for_bytes(15, 0), 0u);
  EXPECT_EQ(link.prbs_for_bytes(15, 1), 1u);
  EXPECT_EQ(link.prbs_for_bytes(15, 100), 1u);  // 800 bits fit in 802
  EXPECT_EQ(link.prbs_for_bytes(15, 101), 2u);  // 808 bits do not
  EXPECT_EQ(link.bytes_for_prbs(15, 2), 200u);  // 1604 bits -> 200 bytes
  EXPECT_EQ(link.prbs_for_bytes(0, 1000), 0u);  // CQI 0 cannot carry data
}

}  // namespace nrs
