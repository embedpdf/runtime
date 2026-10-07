// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "fpdfsdk/epdf_owned_buffer.h"

#include <cstdlib>
#include <cstring>
#include <limits>

namespace epdf {

void* CopyToOwnedBuffer(pdfium::span<const uint8_t> data,
                        unsigned long* out_size) {
  if (!out_size || data.empty() ||
      data.size() > std::numeric_limits<unsigned long>::max()) {
    if (out_size) {
      *out_size = 0;
    }
    return nullptr;
  }

  void* buffer = malloc(data.size());
  if (!buffer) {
    *out_size = 0;
    return nullptr;
  }
  memcpy(buffer, data.data(), data.size());
  *out_size = static_cast<unsigned long>(data.size());
  return buffer;
}

}  // namespace epdf
