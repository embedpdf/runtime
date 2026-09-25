// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_DIGEST_H_
#define PUBLIC_EPDF_DIGEST_H_

// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"

// Experimental EmbedPDF Extension API.
//
// Digests, one implementation for every caller: signatures hash byte ranges
// of a file (EPDFSig_DigestByteRange(), EPDFSig_DigestFileRange()), and a
// stamp drawing is known by the SHA-256 of its canonical bytes
// (EPDFDoc_CanonicalDrawing()).

#define EPDF_DIGEST_SHA1 0
#define EPDF_DIGEST_SHA256 1
#define EPDF_DIGEST_SHA384 2
#define EPDF_DIGEST_SHA512 3

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Experimental EmbedPDF Extension API.
// Hash |size| bytes at |data|: bytes already in memory, such as a saved
// document or a caller's upload. |inout_len| carries the capacity of
// |out_digest| in and the digest length out; when the capacity is too small
// the required length is written and FALSE is returned.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_DigestBuffer(const void* data,
                  unsigned long size,
                  int algorithm,
                  unsigned char* out_digest,
                  unsigned long* inout_len);

#ifdef __cplusplus
}
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_DIGEST_H_
