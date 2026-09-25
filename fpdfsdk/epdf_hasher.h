// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef FPDFSDK_EPDF_HASHER_H_
#define FPDFSDK_EPDF_HASHER_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>

#include "core/fdrm/fx_crypt_sha.h"
#include "core/fxcrt/span.h"

// One digest, fed in parts: EPDF_DIGEST_* from public/epdf_digest.h. The one
// implementation behind EPDF_DigestBuffer() and the signature digests.
class EpdfHasher {
 public:
  explicit EpdfHasher(int algorithm);

  // The digest's length in bytes, or nullopt for an unknown algorithm.
  static std::optional<size_t> DigestSize(int algorithm);

  void Update(pdfium::span<const uint8_t> data);
  // `out` holds at least DigestSize() bytes.
  void Finish(pdfium::span<uint8_t> out);

 private:
  const int algorithm_;
  CRYPT_sha1_context sha1_;
  CRYPT_sha2_context sha2_;
};

#endif  // FPDFSDK_EPDF_HASHER_H_
