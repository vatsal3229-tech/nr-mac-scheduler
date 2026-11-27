#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>

#include "offline/replay.h"
#include "offline/trace.h"

namespace nrs {
namespace {

std::vector<TraceRow> parse(const std::string& s) {
  std::istringstream in(s);
  return parse_trace(in);
}

TEST(Trace, ParsesRows) {
  auto rows = parse("slot,ue_id,cqi,new_bytes\n0,1,15,100\n0,2,7,0\r\n\n3,1,9,4000000000\n");
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[1].report.ue_id, 2);
  EXPECT_EQ(rows[1].report.cqi, 7);
  EXPECT_EQ(rows[2].slot, 3u);
  EXPECT_EQ(rows[2].report.new_bytes, 4'000'000'000u);
}

TEST(Trace, RejectsBadInput) {
  const char* bad[] = {
      "",
      "slot,ue,cqi,bytes\n",
      "slot,ue_id,cqi,new_bytes\n1,2,3\n",
      "slot,ue_id,cqi,new_bytes\n1,2,3,4,5\n",
      "slot,ue_id,cqi,new_bytes\n1,x,3,4\n",
      "slot,ue_id,cqi,new_bytes\n1,70000,3,4\n",
      "slot,ue_id,cqi,new_bytes\n1,2,256,4\n",
      "slot,ue_id,cqi,new_bytes\n5,1,3,4\n4,1,3,4\n",
  };
  for (const char* s : bad) EXPECT_THROW(parse(s), std::runtime_error) << s;
}

TEST(Replay, ReportsAppliedAtTheirSlot) {
  // One UE, 250 bytes arriving at slot 2: nothing is sent in slots 0-1.
  auto rows = parse("slot,ue_id,cqi,new_bytes\n2,1,15,250\n");
  Config cfg;
  KpiReport k = replay(cfg, rows, 4);
  EXPECT_EQ(k.slots, 4u);
  EXPECT_DOUBLE_EQ(k.cell_throughput_bps, 250 * 8 / 0.004);
}

TEST(Replay, InvalidCqiRowsCountedAsRejected) {
  auto rows = parse("slot,ue_id,cqi,new_bytes\n0,1,0,10\n0,2,99,10\n0,3,5,10\n");
  KpiReport k = replay(Config{}, rows, 1);
  EXPECT_EQ(k.rejected_reports, 2u);
}

}  // namespace
}  // namespace nrs
