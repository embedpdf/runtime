// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fxcrt/cfx_bidiparagraph.h"

#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

using Levels = std::vector<uint8_t>;

TEST(CFXBidiParagraphTest, HebrewAndDigitsInALeftToRightParagraph) {
  // "Hello שלום 123": the space before the digits sits between two
  // right-to-left neighbours (digits count as such, rule N1), and digits
  // take the next even level (rule I1).
  CFX_BidiParagraph bidi(L"Hello \x05E9\x05DC\x05D5\x05DD 123", false);
  EXPECT_EQ(0, bidi.base_level());
  EXPECT_EQ((Levels{0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2}), bidi.levels());
}

TEST(CFXBidiParagraphTest, DigitsBeforeHebrewInARightToLeftParagraph) {
  // "123 שלום" at level 1: digits at an odd level go up one (rule I2).
  CFX_BidiParagraph bidi(L"123 \x05E9\x05DC\x05D5\x05DD", true);
  EXPECT_EQ(1, bidi.base_level());
  EXPECT_EQ((Levels{2, 2, 2, 1, 1, 1, 1, 1}), bidi.levels());
}

TEST(CFXBidiParagraphTest, OneLevelPerCharacter) {
  // A supplementary character is one character, whatever wchar_t holds.
  const WideString text(L"a\U0001D400\x05E9");
  CFX_BidiParagraph bidi(text, false);
  ASSERT_EQ(text.GetLength(), bidi.levels().size());
  EXPECT_EQ(0, bidi.levels().front());
  EXPECT_EQ(1, bidi.levels().back());
}

TEST(CFXBidiParagraphTest, LineLevelsResetTrailingWhitespace) {
  // Latin in a right-to-left paragraph is at level 2, its inner space too;
  // ending a line, the space goes to the paragraph's level (rule L1).
  CFX_BidiParagraph bidi(L"abc def", true);
  EXPECT_EQ((Levels{2, 2, 2, 2, 2, 2, 2}), bidi.levels());
  EXPECT_EQ((Levels{2, 2, 2, 1}), bidi.LineLevels(0, 4));
  EXPECT_EQ((Levels{2, 2, 2}), bidi.LineLevels(4, 7));
  EXPECT_TRUE(bidi.LineLevels(3, 3).empty());
}

TEST(CFXBidiParagraphTest, FirstStrongIsRtl) {
  EXPECT_EQ(true, CFX_BidiParagraph::FirstStrongIsRtl(L"123 \x05E9\x05DC"));
  EXPECT_EQ(false, CFX_BidiParagraph::FirstStrongIsRtl(L"abc \x05E9\x05DC"));
  EXPECT_EQ(true, CFX_BidiParagraph::FirstStrongIsRtl(L"\x0634\x0643"));
  EXPECT_EQ(std::nullopt, CFX_BidiParagraph::FirstStrongIsRtl(L"123 !"));
  EXPECT_EQ(std::nullopt, CFX_BidiParagraph::FirstStrongIsRtl(L""));
  // An isolate's content doesn't count (rule P2).
  EXPECT_EQ(true, CFX_BidiParagraph::FirstStrongIsRtl(L"\x2066"
                                                      L"abc\x2069 \x05E9"));
}

TEST(CFXBidiParagraphTest, VisualOrder) {
  EXPECT_EQ((std::vector<size_t>{0, 2, 1, 3}),
            CFX_BidiParagraph::VisualOrder(Levels{0, 1, 1, 0}));
  // Higher levels reverse first, then the whole odd stretch.
  EXPECT_EQ((std::vector<size_t>{3, 1, 2, 0}),
            CFX_BidiParagraph::VisualOrder(Levels{1, 2, 2, 1}));
  EXPECT_EQ((std::vector<size_t>{0, 1, 2}),
            CFX_BidiParagraph::VisualOrder(Levels{0, 0, 0}));
  EXPECT_TRUE(CFX_BidiParagraph::VisualOrder(Levels{}).empty());
}
