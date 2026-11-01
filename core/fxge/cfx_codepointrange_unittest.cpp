// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fxge/cfx_codepointrange.h"

#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

TEST(CFXCodePointRangeTest, AppendJoinsNeighbours) {
  std::vector<CFX_CodePointRange> ranges;
  for (uint32_t code_point : {0x20u, 0x21u, 0x22u, 0x30u, 0x10FFFFu}) {
    CFX_AppendCodePoint(&ranges, code_point);
  }
  ASSERT_EQ(3u, ranges.size());
  EXPECT_EQ(0x20u, ranges[0].first);
  EXPECT_EQ(3u, ranges[0].count);
  EXPECT_EQ(0x30u, ranges[1].first);
  EXPECT_EQ(1u, ranges[1].count);
  EXPECT_EQ(0x10FFFFu, ranges[2].first);
  EXPECT_EQ(1u, ranges[2].count);
}

TEST(CFXCodePointRangeTest, ContainEdges) {
  const std::vector<CFX_CodePointRange> ranges = {{0x20, 3}, {0x30, 1}};
  EXPECT_FALSE(CFX_CodePointRangesContain(ranges, 0x1F));
  EXPECT_TRUE(CFX_CodePointRangesContain(ranges, 0x20));
  EXPECT_TRUE(CFX_CodePointRangesContain(ranges, 0x22));
  EXPECT_FALSE(CFX_CodePointRangesContain(ranges, 0x23));
  EXPECT_FALSE(CFX_CodePointRangesContain(ranges, 0x2F));
  EXPECT_TRUE(CFX_CodePointRangesContain(ranges, 0x30));
  EXPECT_FALSE(CFX_CodePointRangesContain(ranges, 0x31));
  EXPECT_FALSE(CFX_CodePointRangesContain({}, 0x20));
}
