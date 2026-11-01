// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fxcrt/epdf_secure_random.h"

#include <stddef.h>

#include <algorithm>
#include <array>
#include <vector>

#include "core/fxcrt/span.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

bool IsAllZero(pdfium::span<const uint8_t> bytes) {
  return std::ranges::all_of(bytes, [](uint8_t byte) { return byte == 0; });
}

}  // namespace

TEST(EPDFSecureRandomTest, FillsTheBuffer) {
  std::array<uint8_t, 32> key = {};
  ASSERT_TRUE(EPDF_FillSecureRandom(key));
  EXPECT_FALSE(IsAllZero(key));
}

TEST(EPDFSecureRandomTest, TwoCallsDiffer) {
  std::array<uint8_t, 32> first = {};
  std::array<uint8_t, 32> second = {};
  ASSERT_TRUE(EPDF_FillSecureRandom(first));
  ASSERT_TRUE(EPDF_FillSecureRandom(second));
  EXPECT_NE(first, second);
}

// The sources take a limited number of bytes per call (getentropy: 256), so a
// large buffer is filled in chunks. No 32-byte window may stay zero: one that
// did would be a chunk the loop skipped.
TEST(EPDFSecureRandomTest, FillsALargeBufferToTheEnd) {
  std::vector<uint8_t> buffer(4096 + 32, 0);
  ASSERT_TRUE(EPDF_FillSecureRandom(buffer));
  const pdfium::span<const uint8_t> bytes(buffer);
  for (size_t offset = 0; offset < bytes.size(); offset += 32) {
    SCOPED_TRACE(offset);
    EXPECT_FALSE(IsAllZero(bytes.subspan(offset, 32u)));
  }
}

TEST(EPDFSecureRandomTest, AnEmptyBufferSucceeds) {
  EXPECT_TRUE(EPDF_FillSecureRandom(pdfium::span<uint8_t>()));
}
