// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF(fonts): a document font as a browser can draw it. A browser loads
// TrueType and OpenType, not Type 1 or bare CFF, and it reaches glyphs through
// a font's own cmap and spaces them by its own metrics, where the engine goes
// through the PDF's encoding and its /Widths. So the engine's view of the font
// is written as an OpenType font of its own: the same glyphs, reached by the
// same characters, at the same advances.

#ifndef CORE_FPDFDOC_CPDF_DISPLAYPROGRAM_H_
#define CORE_FPDFDOC_CPDF_DISPLAYPROGRAM_H_

#include <stdint.h>

#include <optional>
#include <vector>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/data_vector.h"

class CPDF_Dictionary;
class CPDF_Document;
struct CPDF_FontFace;

struct CPDF_DisplayProgram {
  CPDF_DisplayProgram();
  CPDF_DisplayProgram(CPDF_DisplayProgram&&) noexcept;
  CPDF_DisplayProgram& operator=(CPDF_DisplayProgram&&) noexcept;
  ~CPDF_DisplayProgram();

  // An OpenType font with CFF outlines.
  DataVector<uint8_t> bytes;
  // The characters its cmap maps, in order.
  std::vector<uint32_t> characters;
};

// The display program of |font_dict|: the glyphs its embedded program draws;
// a Unicode cmap holding exactly the characters the engine writes with the
// font, each mapped through the PDF's encoding to the glyph it draws; and the
// advances the engine lays them out with. |postscript_name| and |face| name
// it. nullopt when the font has no outlines of its own (Type 3, nothing
// embedded, a program that doesn't load) or writes no character.
std::optional<CPDF_DisplayProgram> BuildDisplayProgram(
    CPDF_Document* doc,
    const CPDF_Dictionary* font_dict,
    const ByteString& postscript_name,
    const CPDF_FontFace& face);

#endif  // CORE_FPDFDOC_CPDF_DISPLAYPROGRAM_H_
