// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF(fonts): an OpenType font with CFF outlines, written from glyph
// outlines, a Unicode cmap and metrics: a font a browser loads.

#ifndef CORE_FXGE_CFX_OPENTYPEWRITER_H_
#define CORE_FXGE_CFX_OPENTYPEWRITER_H_

#include <stdint.h>

#include <utility>
#include <vector>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxge/cfx_glyphoutline.h"

struct CFX_OpenTypeFont {
  CFX_OpenTypeFont();
  CFX_OpenTypeFont(CFX_OpenTypeFont&&) noexcept;
  CFX_OpenTypeFont& operator=(CFX_OpenTypeFont&&) noexcept;
  ~CFX_OpenTypeFont();

  struct Glyph {
    CFX_GlyphOutline outline;
    // In font units.
    uint16_t advance = 0;
  };

  // The names its 'name' table carries; ASCII.
  ByteString postscript_name;
  ByteString family;
  int weight = 400;
  bool italic = false;
  bool fixed_pitch = false;
  // Degrees, counter-clockwise from vertical.
  float italic_angle = 0;
  uint16_t units_per_em = 1000;
  // Line metrics in font units; descent is negative.
  int ascent = 0;
  int descent = 0;
  // Glyph 0 is .notdef.
  std::vector<Glyph> glyphs;
  // Unicode scalars and the glyph each maps to, in scalar order, each scalar
  // once.
  std::vector<std::pair<uint32_t, uint16_t>> cmap;
};

// The font's bytes: the same bytes for the same font. Empty when it has no
// glyphs or more than an OpenType font can hold.
DataVector<uint8_t> WriteOpenTypeCFF(const CFX_OpenTypeFont& font);

#endif  // CORE_FXGE_CFX_OPENTYPEWRITER_H_
