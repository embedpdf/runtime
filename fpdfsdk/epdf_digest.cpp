// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_digest.h"

#include "core/fxcrt/compiler_specific.h"
#include "fpdfsdk/epdf_hasher.h"

EpdfHasher::EpdfHasher(int algorithm) : algorithm_(algorithm) {
  switch (algorithm_) {
    case EPDF_DIGEST_SHA1:
      CRYPT_SHA1Start(&sha1_);
      break;
    case EPDF_DIGEST_SHA256:
      CRYPT_SHA256Start(&sha2_);
      break;
    case EPDF_DIGEST_SHA384:
      CRYPT_SHA384Start(&sha2_);
      break;
    case EPDF_DIGEST_SHA512:
      CRYPT_SHA512Start(&sha2_);
      break;
  }
}

// static
std::optional<size_t> EpdfHasher::DigestSize(int algorithm) {
  switch (algorithm) {
    case EPDF_DIGEST_SHA1:
      return 20;
    case EPDF_DIGEST_SHA256:
      return 32;
    case EPDF_DIGEST_SHA384:
      return 48;
    case EPDF_DIGEST_SHA512:
      return 64;
    default:
      return std::nullopt;
  }
}

void EpdfHasher::Update(pdfium::span<const uint8_t> data) {
  switch (algorithm_) {
    case EPDF_DIGEST_SHA1:
      CRYPT_SHA1Update(&sha1_, data);
      break;
    case EPDF_DIGEST_SHA256:
      CRYPT_SHA256Update(&sha2_, data);
      break;
    case EPDF_DIGEST_SHA384:
      CRYPT_SHA384Update(&sha2_, data);
      break;
    case EPDF_DIGEST_SHA512:
      CRYPT_SHA512Update(&sha2_, data);
      break;
  }
}

void EpdfHasher::Finish(pdfium::span<uint8_t> out) {
  switch (algorithm_) {
    case EPDF_DIGEST_SHA1:
      CRYPT_SHA1Finish(&sha1_, out.first<20>());
      break;
    case EPDF_DIGEST_SHA256:
      CRYPT_SHA256Finish(&sha2_, out.first<32>());
      break;
    case EPDF_DIGEST_SHA384:
      CRYPT_SHA384Finish(&sha2_, out.first<48>());
      break;
    case EPDF_DIGEST_SHA512:
      CRYPT_SHA512Finish(&sha2_, out.first<64>());
      break;
  }
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_DigestBuffer(const void* data,
                  unsigned long size,
                  int algorithm,
                  unsigned char* out_digest,
                  unsigned long* inout_len) {
  if ((!data && size) || !inout_len) {
    return false;
  }
  std::optional<size_t> digest_size = EpdfHasher::DigestSize(algorithm);
  if (!digest_size.has_value()) {
    return false;
  }
  if (!out_digest || *inout_len < digest_size.value()) {
    *inout_len = static_cast<unsigned long>(digest_size.value());
    return false;
  }
  EpdfHasher hasher(algorithm);
  if (size) {
    // SAFETY: required from caller.
    hasher.Update(UNSAFE_BUFFERS(
        pdfium::span(static_cast<const uint8_t*>(data), size)));
  }
  // SAFETY: capacity checked above.
  hasher.Finish(UNSAFE_BUFFERS(pdfium::span(out_digest, digest_size.value())));
  *inout_len = static_cast<unsigned long>(digest_size.value());
  return true;
}
