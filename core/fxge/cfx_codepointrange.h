// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF(fonts): the characters a font maps, as runs of consecutive code
// points. A font that maps every code point is a handful of runs.

#ifndef CORE_FXGE_CFX_CODEPOINTRANGE_H_
#define CORE_FXGE_CFX_CODEPOINTRANGE_H_

#include <stdint.h>

#include <vector>

#include "core/fxcrt/span.h"

struct CFX_CodePointRange {
  uint32_t first = 0;
  uint32_t count = 0;
};

// Whether |ranges|, in order and apart, hold |code_point|.
bool CFX_CodePointRangesContain(pdfium::span<const CFX_CodePointRange> ranges,
                                uint32_t code_point);

// Adds |code_point|, which is above every code point |ranges| holds.
void CFX_AppendCodePoint(std::vector<CFX_CodePointRange>* ranges,
                         uint32_t code_point);

#endif  // CORE_FXGE_CFX_CODEPOINTRANGE_H_
