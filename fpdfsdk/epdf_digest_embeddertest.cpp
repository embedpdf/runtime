// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "public/epdf_digest.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

std::string Hex(const unsigned char* bytes, unsigned long size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string hex;
  for (unsigned long i = 0; i < size; ++i) {
    hex += kDigits[bytes[i] >> 4];
    hex += kDigits[bytes[i] & 15];
  }
  return hex;
}

}  // namespace

TEST(EPDFDigestTest, HashesBytesInMemory) {
  unsigned char digest[64];
  unsigned long length = sizeof(digest);
  ASSERT_TRUE(EPDF_DigestBuffer("abc", 3, EPDF_DIGEST_SHA256, digest, &length));
  EXPECT_EQ(32u, length);
  EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            Hex(digest, length));

  length = sizeof(digest);
  ASSERT_TRUE(
      EPDF_DigestBuffer(nullptr, 0, EPDF_DIGEST_SHA256, digest, &length));
  EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            Hex(digest, length));
}

TEST(EPDFDigestTest, ReportsTheLengthItNeeds) {
  unsigned char digest[64];
  unsigned long length = 16;
  EXPECT_FALSE(EPDF_DigestBuffer("abc", 3, EPDF_DIGEST_SHA512, digest, &length));
  EXPECT_EQ(64u, length);
  length = 0;
  EXPECT_FALSE(EPDF_DigestBuffer("abc", 3, EPDF_DIGEST_SHA256, nullptr, &length));
  EXPECT_EQ(32u, length);

  length = sizeof(digest);
  EXPECT_FALSE(EPDF_DigestBuffer("abc", 3, 42, digest, &length));
  EXPECT_FALSE(EPDF_DigestBuffer(nullptr, 3, EPDF_DIGEST_SHA256, digest, &length));
  EXPECT_FALSE(EPDF_DigestBuffer("abc", 3, EPDF_DIGEST_SHA256, digest, nullptr));
}
