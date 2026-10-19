// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF(fonts): a glyph's outline in font units, as the font program
// draws it: unscaled and unhinted, so another program can carry it exactly.

#ifndef CORE_FXGE_CFX_GLYPHOUTLINE_H_
#define CORE_FXGE_CFX_GLYPHOUTLINE_H_

#include <stdint.h>

#include <array>
#include <vector>

#include "core/fxcrt/fx_coordinates.h"

struct CFX_GlyphOutline {
  enum class Op : uint8_t {
    kMove,   // starts a contour at points[0]
    kLine,   // to points[0]
    kCubic,  // through points[0] and points[1] to points[2]
  };
  struct Segment {
    Op op;
    std::array<CFX_PointF, 3> points;
  };

  // Contours close where they started. Empty for a glyph that draws nothing
  // (a space). A quadratic curve is the cubic it is.
  std::vector<Segment> segments;
};

#endif  // CORE_FXGE_CFX_GLYPHOUTLINE_H_
