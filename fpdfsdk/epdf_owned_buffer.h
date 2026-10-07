// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef FPDFSDK_EPDF_OWNED_BUFFER_H_
#define FPDFSDK_EPDF_OWNED_BUFFER_H_

#include <stdint.h>

#include "core/fxcrt/span.h"

namespace epdf {

// A copy of |data| the caller owns and releases with EPDF_FreeBuffer(), its
// size in |out_size|. Null (and a size of 0) for empty data, data too large
// to report, or a failed allocation.
void* CopyToOwnedBuffer(pdfium::span<const uint8_t> data,
                        unsigned long* out_size);

}  // namespace epdf

#endif  // FPDFSDK_EPDF_OWNED_BUFFER_H_
