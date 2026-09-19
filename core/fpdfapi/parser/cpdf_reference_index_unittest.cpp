// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/parser/cpdf_reference_index.h"

#include <array>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

TEST(CPDFReferenceIndexTest, DistinguishesEmptyRowsFromMissingRows) {
  CPDF_ReferenceIndex index;
  EXPECT_FALSE(index.Find(7));
  EXPECT_TRUE(index.Insert(7, {}));
  ASSERT_TRUE(index.Find(7));
  EXPECT_TRUE(index.Find(7)->empty());
  EXPECT_FALSE(index.Find(8));
  EXPECT_EQ(1u, index.row_count());
}

TEST(CPDFReferenceIndexTest, PacksRowsWithoutInvalidatingEarlierSpans) {
  CPDF_ReferenceIndex index;
  const std::array<uint32_t, 3> references = {1, 19, 9000000};
  ASSERT_TRUE(index.Insert(5, references));
  const auto first = *index.Find(5);
  for (uint32_t i = 6; i < 20000; ++i) {
    ASSERT_TRUE(index.Insert(i, references));
    ASSERT_TRUE(index.Find(i));
    EXPECT_EQ(first, *index.Find(i));
  }
  EXPECT_EQ(pdfium::span(references), first);
  EXPECT_LT(index.accounted_bytes(), 1024u * 1024);
}

TEST(CPDFReferenceIndexTest, BoundedCacheFallsBackWithoutLosingExistingRows) {
  CPDF_ReferenceIndex index(90000);
  const std::array<uint32_t, 2> references = {2, 3};
  ASSERT_TRUE(index.Insert(1, references));
  const size_t initial_bytes = index.accounted_bytes();
  EXPECT_FALSE(index.Insert(9000000, references));
  EXPECT_FALSE(index.Find(9000000));
  EXPECT_TRUE(index.Find(1));
  EXPECT_EQ(initial_bytes, index.accounted_bytes());
  EXPECT_LE(index.accounted_bytes(), 90000u);

  std::vector<uint32_t> wide(20000, 1);
  EXPECT_FALSE(index.Insert(2, wide));
  EXPECT_FALSE(index.Find(2));
  index.Clear();
  EXPECT_EQ(0u, index.accounted_bytes());
  EXPECT_EQ(0u, index.row_count());
  EXPECT_FALSE(index.Find(1));
}
