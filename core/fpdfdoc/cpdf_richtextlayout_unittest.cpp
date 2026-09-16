// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_richtextlayout.h"

#include <vector>

#include "core/fxcrt/widestring.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

std::vector<size_t> TruePositions(const std::vector<bool>& flags) {
  std::vector<size_t> positions;
  for (size_t i = 0; i < flags.size(); ++i) {
    if (flags[i]) {
      positions.push_back(i);
    }
  }
  return positions;
}

}  // namespace

TEST(CPDFRichTextLayoutTest, BreakOpportunitiesAfterSpaces) {
  // A line may start at "world" and at "again", never inside a word, never
  // at a space, never at position 0.
  EXPECT_EQ((std::vector<size_t>{6, 12}),
            TruePositions(
                CPDF_RichTextLayout::BreakOpportunities(L"Hello world again")));
  EXPECT_EQ(
      (std::vector<size_t>{8}),
      TruePositions(CPDF_RichTextLayout::BreakOpportunities(L"Hello   world")));
  EXPECT_TRUE(CPDF_RichTextLayout::BreakOpportunities(L"").empty());
}

TEST(CPDFRichTextLayoutTest, BreakOpportunitiesHyphenAndPunctuation) {
  // After a hyphen (LB21), not before a closing punctuation (LB13), not
  // before a full stop.
  EXPECT_EQ(
      (std::vector<size_t>{5}),
      TruePositions(CPDF_RichTextLayout::BreakOpportunities(L"well-known")));
  EXPECT_EQ(
      (std::vector<size_t>{7}),
      TruePositions(CPDF_RichTextLayout::BreakOpportunities(L"(word) next.")));
}

TEST(CPDFRichTextLayoutTest, BreakOpportunitiesIdeographs) {
  // CJK ideographs break between any two (LB30-ish for ID).
  EXPECT_EQ((std::vector<size_t>{1, 2}),
            TruePositions(CPDF_RichTextLayout::BreakOpportunities(
                L"\x4e2d\x6587\x5b57")));
}

TEST(CPDFRichTextLayoutTest, GraphemeStartsMarksAndJoiners) {
  // e + combining acute: one grapheme. ZWJ glues both neighbours.
  EXPECT_EQ((std::vector<size_t>{0, 2}),
            TruePositions(CPDF_RichTextLayout::GraphemeStarts(L"e\x0301x")));
  EXPECT_EQ((std::vector<size_t>{0}),
            TruePositions(CPDF_RichTextLayout::GraphemeStarts(L"a\x200d"
                                                              L"b")));
  // A variation selector extends its base.
  EXPECT_EQ((std::vector<size_t>{0, 2}),
            TruePositions(CPDF_RichTextLayout::GraphemeStarts(L"\x2764\xfe0f"
                                                              L"x")));
}

TEST(CPDFRichTextLayoutTest, BreakOpportunitiesNeverInsideGrapheme) {
  // A space before a combining mark still cannot split the grapheme.
  const std::vector<bool> opportunities =
      CPDF_RichTextLayout::BreakOpportunities(L"ab e\x0301");
  EXPECT_TRUE(opportunities[3]);
  EXPECT_FALSE(opportunities[4]);
}
