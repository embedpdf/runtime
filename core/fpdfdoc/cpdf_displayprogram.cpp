// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_displayprogram.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfdoc/cpdf_fontface.h"
#include "core/fxcrt/fx_string.h"
#include "core/fxge/cfx_face.h"
#include "core/fxge/cfx_font.h"
#include "core/fxge/cfx_glyphoutline.h"
#include "core/fxge/cfx_opentypewriter.h"
#include "core/fxge/fx_font.h"

namespace {

// What the engine writes for a character: the glyph it draws and the advance
// it lays it out with, in thousandths of the font size.
struct Written {
  uint32_t glyph;
  int width;
};

bool operator<(const Written& left, const Written& right) {
  return std::tie(left.glyph, left.width) < std::tie(right.glyph, right.width);
}

// Every character the engine can write with |font|: the layout asks the
// font's encoding for each one's code (CharCodeFromUnicode) and draws that
// code's glyph, so a character is written when that code exists and has a
// glyph. The layout writes Basic Multilingual Plane characters only. The
// candidates are what the font's codes read as.
std::map<uint32_t, Written> WrittenCharacters(CPDF_Font* font) {
  std::map<uint32_t, Written> written;
  const uint32_t last_code = font->IsCIDFont() ? 0xffff : 0xff;
  for (uint32_t code = 0; code <= last_code; ++code) {
    const WideString text = font->UnicodeFromCharCode(code);
    if (text.GetLength() != 1) {
      continue;
    }
    const uint32_t scalar = static_cast<uint32_t>(text[0]);
    if (scalar == 0 || scalar > 0xffff ||
        (scalar >= 0xd800 && scalar <= 0xdfff) || written.contains(scalar)) {
      continue;
    }
    const uint32_t write_code =
        font->CharCodeFromUnicode(static_cast<wchar_t>(scalar));
    if (write_code == CPDF_Font::kInvalidCharCode || write_code == 0) {
      continue;
    }
    bool vertical = false;
    const int glyph = font->GlyphFromCharCode(write_code, &vertical);
    if (glyph <= 0) {
      continue;
    }
    written[scalar] = {static_cast<uint32_t>(glyph),
                       font->GetCharWidth(write_code)};
  }
  return written;
}

// |text| with only what a name table and a PostScript name may hold.
ByteString AsciiName(const WideString& text, bool postscript) {
  ByteString name;
  for (wchar_t c : text) {
    const bool letter_or_digit = (c >= 'a' && c <= 'z') ||
                                 (c >= 'A' && c <= 'Z') ||
                                 (c >= '0' && c <= '9');
    if (letter_or_digit || c == '-' || (!postscript && c == ' ')) {
      name += static_cast<char>(c);
    }
    if (name.GetLength() >= 63) {
      break;
    }
  }
  return name;
}

}  // namespace

CPDF_DisplayProgram::CPDF_DisplayProgram() = default;
CPDF_DisplayProgram::CPDF_DisplayProgram(CPDF_DisplayProgram&&) noexcept =
    default;
CPDF_DisplayProgram& CPDF_DisplayProgram::operator=(
    CPDF_DisplayProgram&&) noexcept = default;
CPDF_DisplayProgram::~CPDF_DisplayProgram() = default;

std::optional<CPDF_DisplayProgram> BuildDisplayProgram(
    CPDF_Document* doc,
    const CPDF_Dictionary* font_dict,
    const ByteString& postscript_name,
    const CPDF_FontFace& face) {
  RetainPtr<CPDF_Font> font = CPDF_DocPageData::FromDocument(doc)->GetFont(
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(font_dict)));
  if (!font || font->IsType3Font() || !font->IsEmbedded()) {
    return std::nullopt;
  }
  CFX_Font* program = font->GetFont();
  RetainPtr<CFX_Face> program_face = program->GetFace();
  // A program that didn't load is drawn with a substitute: nothing of its own.
  if (!program_face || program->GetSubstFont()) {
    return std::nullopt;
  }
  const std::map<uint32_t, Written> written = WrittenCharacters(font.Get());
  if (written.empty()) {
    return std::nullopt;
  }

  CFX_OpenTypeFont display;
  display.units_per_em =
      program_face->GetUnitsPerEm() ? program_face->GetUnitsPerEm() : 1000;
  const float em = display.units_per_em / 1000.0f;
  auto advance = [&](int thousandths) {
    return static_cast<uint16_t>(
        std::clamp<long>(std::lround(thousandths * em), 0, 0xffff));
  };
  auto glyph_of = [&](uint32_t source) {
    CFX_OpenTypeFont::Glyph glyph;
    std::optional<CFX_GlyphOutline> outline =
        program_face->LoadUnscaledGlyphOutline(source);
    if (outline.has_value()) {
      glyph.outline = std::move(*outline);
    }
    return glyph;
  };

  // .notdef first; a browser never draws it, the cmap only maps written
  // characters.
  display.glyphs.push_back(glyph_of(0));
  display.glyphs.back().advance = advance(500);
  // One glyph per source glyph and advance: two codes may reach one glyph
  // with different widths.
  std::map<Written, uint16_t> glyph_index;
  for (const auto& [scalar, character] : written) {
    auto [it, added] = glyph_index.emplace(
        character, static_cast<uint16_t>(display.glyphs.size()));
    if (added) {
      if (display.glyphs.size() >= 0xffff) {
        return std::nullopt;
      }
      display.glyphs.push_back(glyph_of(character.glyph));
      display.glyphs.back().advance = advance(character.width);
    }
    display.cmap.emplace_back(scalar, it->second);
  }

  float ascent = 0;
  float descent = 0;
  ProgramLineMetrics(program, &ascent, &descent);
  display.ascent = static_cast<int>(std::lround(ascent * display.units_per_em));
  display.descent =
      -static_cast<int>(std::lround(descent * display.units_per_em));
  display.postscript_name =
      AsciiName(WideString::FromLatin1(postscript_name.AsStringView()),
                /*postscript=*/true);
  if (display.postscript_name.IsEmpty()) {
    display.postscript_name = "EmbedPDFDisplay";
  }
  display.family = AsciiName(face.family, /*postscript=*/false);
  if (display.family.IsEmpty()) {
    display.family = display.postscript_name;
  }
  display.weight = face.weight;
  display.italic = face.italic;
  display.italic_angle = static_cast<float>(font->GetItalicAngle());
  display.fixed_pitch =
      (font->GetFontFlags() & pdfium::kFontStyleFixedPitch) != 0;
  CPDF_DisplayProgram result;
  result.bytes = WriteOpenTypeCFF(display);
  if (result.bytes.empty()) {
    return std::nullopt;
  }
  for (const auto& [scalar, glyph] : display.cmap) {
    result.characters.push_back(scalar);
  }
  return result;
}
