// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fxcrt/epdf_secure_random.h"

#include <stddef.h>

#include <algorithm>

#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <bcrypt.h>
#elif BUILDFLAG(IS_APPLE)
#include <stdlib.h>
#elif BUILDFLAG(IS_ASMJS)
#include <unistd.h>
#elif BUILDFLAG(IS_LINUX)
#include <errno.h>
#include <sys/random.h>
#else
#error "EPDF_FillSecureRandom has no secure random source for this platform."
#endif

namespace {

#if BUILDFLAG(IS_ASMJS)
// getentropy() takes at most 256 bytes per call.
constexpr size_t kMaxChunkSize = 256;
#else
constexpr size_t kMaxChunkSize = 64 * 1024;
#endif

bool FillChunk(pdfium::span<uint8_t> chunk) {
#if BUILDFLAG(IS_WIN)
  return BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, chunk.data(),
                                          static_cast<ULONG>(chunk.size()),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#elif BUILDFLAG(IS_APPLE)
  arc4random_buf(chunk.data(), chunk.size());
  return true;
#elif BUILDFLAG(IS_ASMJS)
  // Emscripten implements getentropy() with crypto.getRandomValues (or Node's
  // crypto.randomFillSync), and aborts when neither exists.
  return getentropy(chunk.data(), chunk.size()) == 0;
#elif BUILDFLAG(IS_LINUX)
  while (!chunk.empty()) {
    const ssize_t read = getrandom(chunk.data(), chunk.size(), 0);
    if (read < 0 && errno == EINTR) {
      continue;
    }
    if (read <= 0) {
      return false;
    }
    chunk = chunk.subspan(static_cast<size_t>(read));
  }
  return true;
#endif
}

}  // namespace

bool EPDF_FillSecureRandom(pdfium::span<uint8_t> buffer) {
  pdfium::span<uint8_t> rest = buffer;
  while (!rest.empty()) {
    const size_t size =
        rest.size() < kMaxChunkSize ? rest.size() : kMaxChunkSize;
    if (!FillChunk(rest.first(size))) {
      std::ranges::fill(buffer, 0);
      return false;
    }
    rest = rest.subspan(size);
  }
  return true;
}
