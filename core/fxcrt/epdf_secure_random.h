// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FXCRT_EPDF_SECURE_RANDOM_H_
#define CORE_FXCRT_EPDF_SECURE_RANDOM_H_

#include <stdint.h>

#include "core/fxcrt/span.h"

// Fills `buffer` with bytes from the operating system's cryptographically
// secure random source: BCryptGenRandom on Windows, arc4random_buf on Apple,
// getrandom on Linux, and getentropy (crypto.getRandomValues) in WebAssembly.
//
// Use it for anything an attacker must not guess: encryption keys, salts and
// IVs. FX_Random is a Mersenne Twister with a 32-bit seed and is not.
//
// Returns false, with `buffer` zeroed, when the source fails. There is no
// weaker fallback, so a caller that needs the bytes must fail too.
bool EPDF_FillSecureRandom(pdfium::span<uint8_t> buffer);

#endif  // CORE_FXCRT_EPDF_SECURE_RANDOM_H_
