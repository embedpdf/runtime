// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fxge/cfx_codepointrange.h"

#include <algorithm>

#include "core/fxge/cfx_face.h"

#if defined(PDF_ENABLE_FREETYPE)
#include "core/fxge/freetype/fx_freetype.h"
#endif

#if defined(PDF_ENABLE_FONTATIONS)
#include "core/fxge/cfx_gemodule.h"
#endif

bool CFX_CodePointRangesContain(pdfium::span<const CFX_CodePointRange> ranges,
                                uint32_t code_point) {
  // The last range starting at or below |code_point|.
  auto after = std::ranges::upper_bound(ranges, code_point, {},
                                        &CFX_CodePointRange::first);
  if (after == ranges.begin()) {
    return false;
  }
  const CFX_CodePointRange& range = *(after - 1);
  return code_point - range.first < range.count;
}

void CFX_AppendCodePoint(std::vector<CFX_CodePointRange>* ranges,
                         uint32_t code_point) {
  if (!ranges->empty() &&
      ranges->back().first + ranges->back().count == code_point) {
    ++ranges->back().count;
    return;
  }
  ranges->push_back({code_point, 1});
}

std::vector<CFX_CodePointRange> CFX_Face::GetMappedCodePointRanges(
    char32_t max_char) {
  std::vector<CFX_CodePointRange> ranges;
#if defined(PDF_ENABLE_FONTATIONS)
  if (CFX_GEModule::IsFontations()) {
    for (const CharCodeAndIndex& item : GetCharCodesAndIndices(max_char)) {
      if (item.glyph_index != 0) {
        CFX_AppendCodePoint(&ranges, item.char_code);
      }
    }
    return ranges;
  }
#endif  // defined(PDF_ENABLE_FONTATIONS)
#if defined(PDF_ENABLE_FREETYPE)
  // Walked one code point at a time, never collected: a font that maps all
  // of Unicode is a million of them.
  FT_UInt glyph_index = 0;
  FT_ULong code_point = FT_Get_First_Char(GetRec(), &glyph_index);
  while (glyph_index != 0 && code_point <= max_char) {
    CFX_AppendCodePoint(&ranges, static_cast<uint32_t>(code_point));
    code_point = FT_Get_Next_Char(GetRec(), code_point, &glyph_index);
  }
#endif  // defined(PDF_ENABLE_FREETYPE)
  return ranges;
}
