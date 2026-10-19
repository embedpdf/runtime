// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_richtextlayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpvt_section.h"
#include "core/fxcrt/cfx_bidiparagraph.h"
#include "core/fxcrt/code_point_view.h"
#include "core/fxcrt/fx_linebreak.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxcrt/fx_unicode.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/utf16.h"
#include "core/fxge/cfx_face.h"
#include "core/fxge/cfx_font.h"
#include "hb-ot.h"  // nogncheck
#include "hb.h"     // nogncheck

namespace {

using Line = CPDF_RichTextLayout::Line;
using GlyphRun = CPDF_RichTextLayout::GlyphRun;
using ShapedGlyph = CPDF_RichTextLayout::ShapedGlyph;
using Align = CPDF_RichTextParagraphProps::Align;
using Script = CPDF_RichTextStyle::Script;

// The measured line model (plan §6, C note §4.4).
constexpr float kLeadingFactor = 0.2f;
constexpr float kSubSuperScale = 0.66f;
constexpr float kSuperRise = 0.31f;
constexpr float kSubRise = -0.15f;
constexpr float kFallbackSize = 12.0f;
constexpr float kEpsilon = 0.01f;

// CPVT's auto font size ladder: plain FreeText and form fields with a DA
// size of 0. A field of several lines tries its first quarter.
constexpr std::array<float, 25> kAutoSizeSteps = {
    4,  6,  8,  9,  10, 12, 14, 18,  20,  25,  30,  35, 40,
    45, 50, 55, 60, 70, 80, 90, 100, 110, 120, 130, 144};

bool IsSpace(wchar_t ch) {
  return ch == L' ' || ch == 0x00A0 || ch == 0x3000;
}

// ---- HarfBuzz ---------------------------------------------------------------

struct HbFontDeleter {
  void operator()(hb_font_t* font) const { hb_font_destroy(font); }
};
struct HbFaceDeleter {
  void operator()(hb_face_t* face) const { hb_face_destroy(face); }
};
struct HbBufferDeleter {
  void operator()(hb_buffer_t* buffer) const { hb_buffer_destroy(buffer); }
};

struct HbFont {
  std::unique_ptr<hb_face_t, HbFaceDeleter> face;
  std::unique_ptr<hb_font_t, HbFontDeleter> font;
  unsigned int upem = 1000;
};

std::unique_ptr<HbFont> CreateHbFont(const CFX_Font* program) {
  if (!program) {
    return nullptr;
  }
  pdfium::span<const uint8_t> data = program->GetFontSpan();
  if (data.empty()) {
    return nullptr;
  }
  hb_blob_t* blob =
      hb_blob_create(reinterpret_cast<const char*>(data.data()),
                     pdfium::checked_cast<unsigned int>(data.size()),
                     HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  if (!blob) {
    return nullptr;
  }
  auto result = std::make_unique<HbFont>();
  result->face.reset(hb_face_create(blob, 0));
  hb_blob_destroy(blob);
  if (!result->face || hb_face_get_glyph_count(result->face.get()) == 0) {
    return nullptr;
  }
  result->upem = hb_face_get_upem(result->face.get());
  if (result->upem == 0) {
    result->upem = 1000;
  }
  result->font.reset(hb_font_create(result->face.get()));
  if (!result->font) {
    return nullptr;
  }
  hb_font_set_scale(result->font.get(), static_cast<int>(result->upem),
                    static_cast<int>(result->upem));
  hb_ot_font_set_funcs(result->font.get());
  return result;
}

hb_script_t ScriptOf(uint32_t codepoint) {
  return hb_unicode_script(hb_unicode_funcs_get_default(), codepoint);
}

bool IsCommonScript(hb_script_t script) {
  return script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED ||
         script == HB_SCRIPT_UNKNOWN;
}

// Scripts whose shaping is a matter of default features only: the Latin
// features Acrobat leaves off are turned off for them (C note §4.2).
bool IsSimpleScript(hb_script_t script) {
  switch (script) {
    case HB_SCRIPT_LATIN:
    case HB_SCRIPT_GREEK:
    case HB_SCRIPT_CYRILLIC:
    case HB_SCRIPT_ARMENIAN:
    case HB_SCRIPT_GEORGIAN:
    case HB_SCRIPT_HAN:
    case HB_SCRIPT_HIRAGANA:
    case HB_SCRIPT_KATAKANA:
    case HB_SCRIPT_HANGUL:
    case HB_SCRIPT_BOPOMOFO:
    case HB_SCRIPT_COMMON:
    case HB_SCRIPT_INHERITED:
    case HB_SCRIPT_UNKNOWN:
      return true;
    default:
      return false;
  }
}

// ---- Break opportunities
// ------------------------------------------------------

BreakProperty BreakClassOf(wchar_t ch) {
  if (static_cast<uint32_t>(ch) > 0xFFFF) {
    const uint32_t cp = static_cast<uint32_t>(ch);
    return (cp >= 0x1F000 && cp <= 0x1FAFF) ? BreakProperty::kID
                                            : BreakProperty::kAL;
  }
  return pdfium::unicode::GetBreakProperty(ch);
}

bool IsZeroWidthJoiner(wchar_t ch) {
  return ch == 0x200D;
}

bool IsVariationSelector(uint32_t cp) {
  return (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF);
}

bool IsMark(uint32_t cp) {
  switch (hb_unicode_general_category(hb_unicode_funcs_get_default(), cp)) {
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
      return true;
    default:
      return false;
  }
}

// ---- Layout state
// -------------------------------------------------------------

// The resolved character style of one run of the source document.
struct ResolvedStyle {
  CPDF_RichTextStyle style;  // cascade applied; size is the parent size
  float drawn_size = 0;      // after sub/superscript scaling
  float rise = 0;            // pt
  bool has_size_override = false;
  int face_entry = -1;  // the face, when it isn't resolved from the style
};

// One shaping unit: one font entry, one style, one script and one bidi
// level, covering [begin, end) of the segment's text.
struct Item {
  int entry = -1;
  int style_index = 0;
  size_t begin = 0;
  size_t end = 0;
  hb_script_t script = HB_SCRIPT_COMMON;
  uint8_t level = 0;  // odd: shaped right to left
  bool rtl = false;
  std::vector<ShapedGlyph> glyphs;  // clusters are absolute segment indices
  float ascent = 0;                 // pt
  float descent = 0;
  bool degraded = false;
  bool needs_actual_text = false;
};

struct Segment {
  WideString text;                 // without the hard break
  std::vector<int> style_of;       // per character
  std::vector<size_t> source_pos;  // per character: index in the paragraph
  std::optional<CFX_BidiParagraph> bidi;
  std::vector<Item> items;
  int break_style = 0;  // the style at the hard break, for an empty line
  bool ends_with_hard_break = false;
};

}  // namespace

// static
std::vector<bool> CPDF_RichTextLayout::GraphemeStarts(const WideString& text) {
  std::vector<bool> starts(text.GetLength(), true);
  bool after_zwj = false;
  for (size_t i = 0; i < text.GetLength(); ++i) {
    const uint32_t cp = static_cast<uint32_t>(text[i]);
    bool extends = false;
    if (i > 0) {
      if (IsMark(cp) || IsVariationSelector(cp) || IsZeroWidthJoiner(text[i]) ||
          after_zwj) {
        extends = true;
      }
      if (sizeof(wchar_t) == 2 && cp >= 0xDC00 && cp <= 0xDFFF) {
        extends = true;  // low surrogate
      }
    }
    starts[i] = !extends;
    after_zwj = IsZeroWidthJoiner(text[i]);
  }
  return starts;
}

// static
CPDF_RichTextLayout::GraphemeMatch CPDF_RichTextLayout::MatchGrapheme(
    CPDF_AnnotFontMap* fonts,
    int face_entry,
    WideStringView grapheme,
    int weight,
    bool italic) {
  if (face_entry < 0) {
    return {};
  }
  if (fonts->CoversText(face_entry, grapheme)) {
    return {face_entry, true};
  }
  const int fallback = fonts->FindRichFallback(grapheme, weight, italic);
  if (fallback >= 0) {
    return {fallback, true};
  }
  // No font has it: the last resort draws it as its box. A grapheme that
  // draws nothing stays with the face, which hides it.
  const int last_resort =
      IsInvisibleGrapheme(grapheme) ? -1 : fonts->FindLastResort();
  return {last_resort >= 0 ? last_resort : face_entry, false};
}

// static
bool CPDF_RichTextLayout::IsInvisibleGrapheme(WideStringView grapheme) {
  for (char32_t cp : pdfium::CodePointView(grapheme)) {
    const bool invisible =
        cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x00AD ||
        cp == 0x034F || cp == 0x061C || (cp >= 0x115F && cp <= 0x1160) ||
        (cp >= 0x17B4 && cp <= 0x17B5) || (cp >= 0x180B && cp <= 0x180F) ||
        (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
        (cp >= 0x2060 && cp <= 0x206F) || cp == 0x3164 ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || cp == 0xFFA0 ||
        (cp >= 0xFFF0 && cp <= 0xFFF8) || (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
        (cp >= 0x1D173 && cp <= 0x1D17A) || (cp >= 0xE0000 && cp <= 0xE0FFF);
    if (!invisible) {
      return false;
    }
  }
  return true;
}

// static
std::vector<bool> CPDF_RichTextLayout::BreakOpportunities(
    const WideString& text) {
  const size_t length = text.GetLength();
  std::vector<bool> opportunities(length, false);
  if (length == 0) {
    return opportunities;
  }
  const std::vector<bool> grapheme_starts = GraphemeStarts(text);

  // Classes, with marks and joiners attached to their base (UAX #14 LB9).
  std::vector<BreakProperty> classes(length);
  for (size_t i = 0; i < length; ++i) {
    BreakProperty cls = BreakClassOf(text[i]);
    if (i > 0 && !grapheme_starts[i]) {
      cls = classes[i - 1];
    } else if (cls == BreakProperty::kCM || cls == BreakProperty::kZW) {
      cls = i > 0 ? classes[i - 1] : BreakProperty::kAL;
    }
    classes[i] = cls;
  }

  for (size_t i = 1; i < length; ++i) {
    if (!grapheme_starts[i] || IsSpace(text[i])) {
      continue;  // never before a mark, never before a space (LB7)
    }
    // The class before the spaces, if any (LB18: break after spaces).
    size_t j = i;
    bool spaces = false;
    while (j > 0 && IsSpace(text[j - 1])) {
      --j;
      spaces = true;
    }
    if (j == 0) {
      opportunities[i] = spaces;  // leading spaces: after them, yes
      continue;
    }
    const BreakProperty before = classes[j - 1];
    if (before == BreakProperty::kZW) {
      opportunities[i] = true;  // LB8
      continue;
    }
    switch (pdfium::GetLineBreakTypeFromPair(before, classes[i])) {
      case pdfium::LineBreakType::kDirectBreak:
        opportunities[i] = true;
        break;
      case pdfium::LineBreakType::kIndirectBreak:
      case pdfium::LineBreakType::kCommonIndirectBreak:
      case pdfium::LineBreakType::kHangulSpaceBreak:
      case pdfium::LineBreakType::kUnknown:
        opportunities[i] = spaces;
        break;
      case pdfium::LineBreakType::kProhibitedBreak:
      case pdfium::LineBreakType::kCommonProhibitedBreak:
        opportunities[i] = false;
        break;
    }
  }
  return opportunities;
}

struct CPDF_RichTextLayout::Impl {
  Impl(CPDF_AnnotFontMap* fonts, const Options& options)
      : fonts(fonts), options(options) {}

  CPDF_AnnotFontMap* fonts;
  const Options& options;
  std::map<int, std::unique_ptr<HbFont>> hb_fonts;
  std::map<int, CPDF_AnnotFontMap::RichFace> faces;
  bool degraded = false;
  // A form field: widths in CPVT's arithmetic, so its lines break where
  // CPVT's did.
  bool field = false;

  // |width| thousandths of an em at |size| pt.
  float Scaled(int width, float size) const {
    return field ? width * size * 0.001f : width * size / 1000.0f;
  }

  const CPDF_AnnotFontMap::RichFace& FaceOf(int entry) {
    auto it = faces.find(entry);
    if (it == faces.end()) {
      it = faces.emplace(entry, fonts->GetRichFace(entry)).first;
    }
    return it->second;
  }

  HbFont* HbFontOf(int entry) {
    auto it = hb_fonts.find(entry);
    if (it == hb_fonts.end()) {
      it = hb_fonts.emplace(entry, CreateHbFont(FaceOf(entry).program)).first;
    }
    return it->second.get();
  }

  uint32_t GlyphFor(int entry, wchar_t ch) {
    return fonts->RichGlyphFor(entry, static_cast<uint32_t>(ch));
  }

  // Shapes one item into glyphs with absolute clusters.
  void Shape(Item* item,
             const WideString& text,
             const ResolvedStyle& resolved) {
    item->glyphs.clear();
    const CPDF_AnnotFontMap::RichFace& face = FaceOf(item->entry);
    const float size = resolved.drawn_size;
    const float scale_x = resolved.style.horz_scale;
    const float letter_spacing = resolved.style.letter_spacing;
    item->ascent = face.ascent * size;
    item->descent = face.descent * size;
    item->degraded = face.degraded;
    item->rtl = (item->level & 1) != 0;

    HbFont* hb = face.program ? HbFontOf(item->entry) : nullptr;
    if (!hb) {
      // Standard 14 (or a program HarfBuzz refused): one glyph per
      // character through the font's encoding; right to left, in reverse
      // and mirrored, as HarfBuzz would.
      for (size_t i = item->begin; i < item->end; ++i) {
        uint32_t ch = static_cast<uint32_t>(text[i]);
        if (item->rtl) {
          ch = hb_unicode_mirroring(hb_unicode_funcs_get_default(), ch);
        }
        const uint32_t code = GlyphFor(item->entry, static_cast<wchar_t>(ch));
        ShapedGlyph glyph;
        glyph.gid = code;
        glyph.cluster = pdfium::checked_cast<uint32_t>(i);
        const int width =
            face.pdf_font && code ? face.pdf_font->GetCharWidth(code) : 0;
        glyph.x_advance = (Scaled(width, size) + letter_spacing) * scale_x;
        item->glyphs.push_back(glyph);
      }
      if (item->rtl) {
        std::reverse(item->glyphs.begin(), item->glyphs.end());
      }
      item->needs_actual_text = item->rtl;
      return;
    }

    std::unique_ptr<hb_buffer_t, HbBufferDeleter> buffer(hb_buffer_create());
    if (!buffer || !hb_buffer_allocation_successful(buffer.get())) {
      return;
    }
    const unsigned int length =
        pdfium::checked_cast<unsigned int>(text.GetLength());
    const unsigned int offset = pdfium::checked_cast<unsigned int>(item->begin);
    const unsigned int count =
        pdfium::checked_cast<unsigned int>(item->end - item->begin);
    if constexpr (sizeof(wchar_t) == 4) {
      hb_buffer_add_utf32(buffer.get(),
                          reinterpret_cast<const uint32_t*>(text.c_str()),
                          length, offset, count);
    } else {
      hb_buffer_add_utf16(buffer.get(),
                          reinterpret_cast<const uint16_t*>(text.c_str()),
                          length, offset, count);
    }
    hb_buffer_set_direction(buffer.get(),
                            item->rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_set_script(buffer.get(), IsCommonScript(item->script)
                                           ? HB_SCRIPT_LATIN
                                           : item->script);
    hb_buffer_set_language(buffer.get(), hb_language_get_default());
    hb_buffer_set_cluster_level(buffer.get(),
                                HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);

    std::vector<hb_feature_t> features;
    if (!options.typographic_features && IsSimpleScript(item->script)) {
      for (const char* tag : {"kern", "liga", "clig", "calt", "dlig"}) {
        hb_feature_t feature;
        if (hb_feature_from_string(tag, -1, &feature)) {
          feature.value = 0;
          feature.start = HB_FEATURE_GLOBAL_START;
          feature.end = HB_FEATURE_GLOBAL_END;
          features.push_back(feature);
        }
      }
    }
    hb_shape(hb->font.get(), buffer.get(), features.data(),
             pdfium::checked_cast<unsigned int>(features.size()));

    const unsigned int glyph_count = hb_buffer_get_length(buffer.get());
    hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer.get(), nullptr);
    hb_glyph_position_t* positions =
        hb_buffer_get_glyph_positions(buffer.get(), nullptr);
    const float unit = size / static_cast<float>(hb->upem);
    item->glyphs.reserve(glyph_count);
    for (unsigned int i = 0; i < glyph_count; ++i) {
      ShapedGlyph glyph;
      glyph.gid = infos[i].codepoint;
      glyph.cluster = infos[i].cluster;
      // The advance a viewer computes from /W is the font's width in
      // thousandths, rounded; when shaping only reproduces that width, use
      // it exactly so the appearance needs no TJ adjustment.
      float advance = positions[i].x_advance * unit;
      const float pdf_width =
          Scaled(face.program->GetGlyphWidth(glyph.gid), size);
      if (std::fabs(advance - pdf_width) * 1000.0f / size < 0.75f) {
        advance = pdf_width;
      }
      glyph.x_advance = (advance + letter_spacing) * scale_x;
      glyph.y_advance = positions[i].y_advance * unit;
      glyph.x_offset = positions[i].x_offset * unit * scale_x;
      glyph.y_offset = positions[i].y_offset * unit;
      glyph.unsafe_to_break = (hb_glyph_info_get_glyph_flags(&infos[i]) &
                               HB_GLYPH_FLAG_UNSAFE_TO_BREAK) != 0;
      item->glyphs.push_back(glyph);
    }

    // C note §1.4: the glyphs reproduce the text only when every cluster is
    // one glyph for one scalar, in logical order, within the BMP.
    item->needs_actual_text = item->rtl;
    if (!item->needs_actual_text) {
      std::map<uint32_t, int> glyphs_per_cluster;
      for (const ShapedGlyph& glyph : item->glyphs) {
        ++glyphs_per_cluster[glyph.cluster];
      }
      for (size_t i = item->begin; i < item->end && !item->needs_actual_text;
           ++i) {
        auto it = glyphs_per_cluster.find(pdfium::checked_cast<uint32_t>(i));
        if (it == glyphs_per_cluster.end() || it->second != 1 ||
            static_cast<uint32_t>(text[i]) > 0xFFFF) {
          item->needs_actual_text = true;
        }
      }
    }
  }

  // Width of the glyphs of |item| whose clusters lie in [begin, end).
  static float WidthOf(const Item& item, size_t begin, size_t end) {
    float width = 0;
    for (const ShapedGlyph& glyph : item.glyphs) {
      if (glyph.cluster >= begin && glyph.cluster < end) {
        width += glyph.x_advance;
      }
    }
    return width;
  }

  // Width of the segment's text in [begin, end).
  static float RangeWidth(const Segment& segment, size_t begin, size_t end) {
    float width = 0;
    for (const Item& item : segment.items) {
      if (item.end <= begin || item.begin >= end) {
        continue;
      }
      width +=
          WidthOf(item, std::max(begin, item.begin), std::min(end, item.end));
    }
    return width;
  }

  // Where the ink of [begin, end) ends: trailing spaces do not count
  // (Acrobat centres and wraps without them).
  static size_t InkEnd(const Segment& segment, size_t begin, size_t end) {
    while (end > begin && IsSpace(segment.text[end - 1])) {
      --end;
    }
    return end;
  }

  static float InkWidth(const Segment& segment, size_t begin, size_t end) {
    return RangeWidth(segment, begin, InkEnd(segment, begin, end));
  }

  static bool IsClusterBoundary(const Segment& segment, size_t pos) {
    for (const Item& item : segment.items) {
      if (pos == item.begin || pos == item.end) {
        return true;
      }
      if (pos > item.begin && pos < item.end) {
        for (const ShapedGlyph& glyph : item.glyphs) {
          if (glyph.cluster == pos) {
            return true;
          }
        }
        return false;
      }
    }
    return true;
  }

  // Splits the item containing |pos| there and re-shapes both halves, so a
  // line break inside a shaped word never keeps a shape that assumed its
  // other half (C note §4.2).
  void SplitAt(Segment* segment,
               size_t pos,
               const std::vector<ResolvedStyle>& styles) {
    for (size_t i = 0; i < segment->items.size(); ++i) {
      Item& item = segment->items[i];
      if (pos <= item.begin || pos >= item.end) {
        continue;
      }
      Item second = item;
      second.begin = pos;
      item.end = pos;
      Shape(&item, segment->text, styles[item.style_index]);
      Shape(&second, segment->text, styles[second.style_index]);
      segment->items.insert(segment->items.begin() + i + 1, std::move(second));
      return;
    }
  }

  // Builds the items of a segment of a paragraph that reads right to left
  // when |rtl|: runs of one style, split by script, by bidi level and by the
  // face that covers each grapheme, then shaped.
  void Itemize(Segment* segment,
               bool rtl,
               const std::vector<ResolvedStyle>& styles) {
    const WideString& text = segment->text;
    const size_t length = text.GetLength();
    segment->items.clear();
    segment->bidi.emplace(text, rtl);
    if (length == 0) {
      return;
    }
    const std::vector<bool> grapheme_starts = GraphemeStarts(text);
    const std::vector<uint8_t>& levels = segment->bidi->levels();

    // Per grapheme: style, script, level, entry.
    struct Piece {
      size_t begin, end;
      int style;
      hb_script_t script;
      uint8_t level;
      int entry;
    };
    std::vector<Piece> pieces;
    hb_script_t last_script = HB_SCRIPT_COMMON;
    for (size_t begin = 0; begin < length;) {
      size_t end = begin + 1;
      while (end < length && !grapheme_starts[end]) {
        ++end;
      }
      hb_script_t script = ScriptOf(static_cast<uint32_t>(text[begin]));
      if (IsCommonScript(script)) {
        script = last_script;  // common characters follow their neighbours
      } else {
        last_script = script;
      }
      const int style = segment->style_of[begin];
      const CPDF_RichTextStyle& resolved = styles[style].style;
      const WideStringView grapheme =
          text.AsStringView().Substr(begin, end - begin);
      const int face =
          styles[style].face_entry >= 0
              ? styles[style].face_entry
              : fonts->ResolveRichFace(resolved.family, resolved.weight,
                                       resolved.italic);
      const GraphemeMatch match = MatchGrapheme(
          fonts, face, grapheme, resolved.weight, resolved.italic);
      if (!match.covered && !IsInvisibleGrapheme(grapheme)) {
        fonts->NoteMissingCharacter(WideString(grapheme));
      }
      pieces.push_back({begin, end, style, script, levels[begin], match.entry});
      begin = end;
    }
    // A leading common-script piece takes the script that follows it.
    for (size_t i = pieces.size(); i > 1; --i) {
      if (IsCommonScript(pieces[i - 2].script)) {
        pieces[i - 2].script = pieces[i - 1].script;
      }
    }

    for (const Piece& piece : pieces) {
      Item* last = segment->items.empty() ? nullptr : &segment->items.back();
      if (last && last->style_index == piece.style &&
          last->entry == piece.entry && last->level == piece.level &&
          (last->script == piece.script || IsCommonScript(piece.script))) {
        last->end = piece.end;
        continue;
      }
      Item item;
      item.entry = piece.entry;
      item.style_index = piece.style;
      item.begin = piece.begin;
      item.end = piece.end;
      item.script = piece.script;
      item.level = piece.level;
      segment->items.push_back(std::move(item));
    }
    for (Item& item : segment->items) {
      Shape(&item, text, styles[item.style_index]);
      if (item.degraded) {
        degraded = true;
      }
    }
  }

  // The run drawing [begin, end) of |item| at bidi level |level|.
  static GlyphRun RunOf(const Segment& segment,
                        const Item& item,
                        size_t begin,
                        size_t end,
                        uint8_t level,
                        const ResolvedStyle& resolved) {
    GlyphRun run;
    run.font_entry = item.entry;
    run.style = resolved.style;
    run.style.size = resolved.drawn_size;
    run.rise = resolved.rise;
    run.text = segment.text.Substr(begin, end - begin);
    run.text_begin = segment.source_pos[begin];
    run.text_end = end < segment.source_pos.size()
                       ? segment.source_pos[end]
                       : segment.source_pos[end - 1] + 1;
    run.rtl = item.rtl;
    run.bidi_level = level;
    run.ascent = item.ascent;
    run.descent = item.descent;
    run.needs_actual_text = item.needs_actual_text;
    run.degraded = item.degraded;
    for (const ShapedGlyph& glyph : item.glyphs) {
      if (glyph.cluster >= begin && glyph.cluster < end) {
        ShapedGlyph copy = glyph;
        copy.cluster -= pdfium::checked_cast<uint32_t>(begin);
        run.glyphs.push_back(copy);
        run.width += glyph.x_advance;
      }
    }
    return run;
  }

  // The runs of a line covering [begin, end) of the segment, in the order
  // they are drawn, left to right.
  static std::vector<GlyphRun> RunsFor(
      const Segment& segment,
      size_t begin,
      size_t end,
      const std::vector<ResolvedStyle>& styles) {
    // Whitespace ending the line takes the paragraph's level, so an item's
    // last characters may run at another level than the rest of it.
    const std::vector<uint8_t> levels = segment.bidi->LineLevels(begin, end);
    std::vector<GlyphRun> runs;
    std::vector<uint8_t> run_levels;
    for (const Item& item : segment.items) {
      const size_t item_end = std::min(end, item.end);
      for (size_t run_begin = std::max(begin, item.begin);
           run_begin < item_end;) {
        const uint8_t level = levels[run_begin - begin];
        size_t run_end = run_begin + 1;
        while (run_end < item_end && levels[run_end - begin] == level) {
          ++run_end;
        }
        runs.push_back(RunOf(segment, item, run_begin, run_end, level,
                             styles[item.style_index]));
        run_levels.push_back(level);
        run_begin = run_end;
      }
    }
    std::vector<GlyphRun> visual;
    visual.reserve(runs.size());
    for (size_t index : CFX_BidiParagraph::VisualOrder(run_levels)) {
      visual.push_back(std::move(runs[index]));
    }
    return visual;
  }
};

CPDF_RichTextLayout::CPDF_RichTextLayout(CPDF_AnnotFontMap* fonts,
                                         const Options& options)
    : fonts_(fonts), options_(options) {}

CPDF_RichTextLayout::~CPDF_RichTextLayout() = default;

namespace {

struct ParagraphLayout {
  std::vector<Line> lines;
  float margin_top = 0;
  float margin_bottom = 0;
};

float LineBox(const Line& line, const CPDF_RichTextParagraphProps& props) {
  if (props.line_height.has_value() && *props.line_height > 0) {
    return *props.line_height;
  }
  return line.ascent + line.descent + line.leading;
}

}  // namespace

CPDF_RichTextLayout::Result CPDF_RichTextLayout::Arrange(
    const CPDF_RichTextDocument& document,
    const CFX_FloatRect& area,
    CPDF_Annot::VerticalAlignment vertical_alignment) {
  Result result;
  const float area_width = std::max(0.0f, area.Width());
  const float area_height = std::max(0.0f, area.Height());

  // Auto size (DA size 0): plain, single-style documents search CPVT's
  // ladder for the largest size that fits; anything else is 12 pt.
  std::vector<float> candidate_sizes;
  bool auto_size = document.body.size <= 0;
  if (auto_size) {
    bool single_style = true;
    for (const CPDF_RichTextParagraph& paragraph : document.paragraphs) {
      for (const CPDF_RichTextRun& run : paragraph.runs) {
        if (run.style.size.has_value() || run.style.script.has_value()) {
          single_style = false;
        }
      }
    }
    if (single_style) {
      candidate_sizes.assign(kAutoSizeSteps.rbegin(), kAutoSizeSteps.rend());
    } else {
      candidate_sizes.push_back(kFallbackSize);
      result.auto_size_fell_back = true;
    }
  } else {
    candidate_sizes.push_back(document.body.size);
  }

  // Faces resolve once per request (family, weight, italic) over every
  // character styled with it, whatever the size attempt (C note §1.3): a
  // document face that cannot draw all of that text is not the request's
  // face, the /DA answers the same, and the itemiser below only ever hits
  // the map's cache.
  {
    struct RequestText {
      ByteString key;
      CPDF_RichTextStyle style;
      WideString text;
    };
    std::vector<RequestText> requests;
    auto note = [&](const CPDF_RichTextStyle& style, const WideString& text) {
      const ByteString key = CPDF_AnnotFontMap::FaceRequestKey(
          style.family, style.weight, style.italic);
      auto it = std::ranges::find_if(
          requests, [&](const RequestText& r) { return r.key == key; });
      if (it == requests.end()) {
        requests.push_back({key, style, WideString()});
        it = requests.end() - 1;
      }
      for (wchar_t ch : text) {
        if (ch != L'\r') {
          it->text += ch;
        }
      }
    };
    note(document.body, WideString());  // the /DA face, and empty lines
    for (const CPDF_RichTextParagraph& paragraph : document.paragraphs) {
      for (const CPDF_RichTextRun& run : paragraph.runs) {
        note(ApplyRichTextStyleDelta(document.body, run.style), run.text);
      }
    }
    for (const RequestText& request : requests) {
      fonts_->ResolveRichFace(request.style.family, request.style.weight,
                              request.style.italic,
                              request.text.AsStringView());
    }
  }

  for (size_t attempt = 0; attempt < candidate_sizes.size(); ++attempt) {
    const float body_size = candidate_sizes[attempt];
    Impl impl(fonts_, options_);
    std::vector<ParagraphLayout> paragraphs;
    float widest = 0;

    for (const CPDF_RichTextParagraph& paragraph : document.paragraphs) {
      const CPDF_RichTextParagraphProps& props = paragraph.props;
      ParagraphLayout laid_out;
      laid_out.margin_top = props.margin_top;
      laid_out.margin_bottom = props.margin_bottom;

      // 1. Resolve the cascade per run.
      std::vector<ResolvedStyle> styles;
      styles.reserve(paragraph.runs.size() + 1);
      CPDF_RichTextStyle body = document.body;
      body.size = body_size;
      auto resolve = [&](const CPDF_RichTextStyleDelta& delta) {
        ResolvedStyle resolved;
        resolved.style = ApplyRichTextStyleDelta(body, delta);
        if (resolved.style.size <= 0) {
          resolved.style.size = body_size;
        }
        resolved.has_size_override = delta.size.has_value();
        const float parent = resolved.style.size;
        // Sizes and rises to two decimals, the precision Acrobat writes
        // (11.88 for 18 pt), so the stream carries no float noise.
        auto round2 = [](float value) {
          return std::round(value * 100.0f) / 100.0f;
        };
        switch (resolved.style.script) {
          case Script::kNormal:
            resolved.drawn_size = parent;
            break;
          case Script::kSuper:
            resolved.drawn_size = round2(parent * kSubSuperScale);
            resolved.rise = round2(parent * kSuperRise);
            break;
          case Script::kSub:
            resolved.drawn_size = round2(parent * kSubSuperScale);
            resolved.rise = round2(parent * kSubRise);
            break;
        }
        return resolved;
      };
      for (const CPDF_RichTextRun& run : paragraph.runs) {
        styles.push_back(resolve(run.style));
      }
      if (styles.empty()) {
        styles.push_back(resolve(CPDF_RichTextStyleDelta()));
      }

      // 2. The logical text, split into segments at hard breaks.
      std::vector<Segment> segments;
      segments.emplace_back();
      size_t source_pos = 0;
      int last_style = 0;
      for (size_t r = 0; r < paragraph.runs.size(); ++r) {
        const WideString& text = paragraph.runs[r].text;
        for (size_t i = 0; i < text.GetLength(); ++i, ++source_pos) {
          last_style = static_cast<int>(r);
          if (text[i] == L'\r') {
            segments.back().ends_with_hard_break = true;
            segments.back().break_style = last_style;
            segments.emplace_back();
            continue;
          }
          segments.back().text += text[i];
          segments.back().style_of.push_back(last_style);
          segments.back().source_pos.push_back(source_pos);
        }
      }
      segments.back().break_style = last_style;

      // The direction the paragraph reads in: its dir, or its text's.
      bool rtl = props.rtl;
      if (options_.direction_from_text) {
        WideString text;
        for (const Segment& segment : segments) {
          text += segment.text;
        }
        rtl = CFX_BidiParagraph::FirstStrongIsRtl(text.AsStringView())
                  .value_or(false);
      }

      // 3. Each segment: itemise, shape, break into lines.
      const float margin_left = std::max(0.0f, props.margin_left);
      const float margin_right = std::max(0.0f, props.margin_right);
      bool first_line_of_paragraph = true;
      for (Segment& segment : segments) {
        impl.Itemize(&segment, rtl, styles);
        const size_t length = segment.text.GetLength();
        const std::vector<bool> opportunities =
            BreakOpportunities(segment.text);

        size_t pos = 0;
        bool emitted = false;
        while (pos < length || !emitted) {
          const float indent = first_line_of_paragraph ? props.text_indent : 0;
          const float avail =
              std::max(0.0f, area_width - margin_left - margin_right - indent);

          // The furthest opportunity whose ink fits.
          size_t best = length;
          bool fits_whole =
              Impl::InkWidth(segment, pos, length) <= avail + kEpsilon;
          if (!fits_whole) {
            best = 0;
            for (size_t i = pos + 1; i < length; ++i) {
              if (!opportunities[i] || !Impl::IsClusterBoundary(segment, i)) {
                continue;
              }
              if (Impl::InkWidth(segment, pos, i) <= avail + kEpsilon) {
                best = i;
              } else {
                break;
              }
            }
            if (best == 0) {
              // A word wider than the line breaks inside itself, at a
              // grapheme that is also a cluster boundary (Acrobat does).
              const std::vector<bool> graphemes = GraphemeStarts(segment.text);
              size_t fallback = 0;
              for (size_t i = pos + 1; i < length; ++i) {
                if (!graphemes[i] || !Impl::IsClusterBoundary(segment, i)) {
                  continue;
                }
                if (Impl::InkWidth(segment, pos, i) <= avail + kEpsilon) {
                  fallback = i;
                } else {
                  break;
                }
              }
              if (fallback == 0) {
                for (size_t i = pos + 1; i < length; ++i) {
                  if (graphemes[i] && Impl::IsClusterBoundary(segment, i)) {
                    fallback = i;
                    break;
                  }
                }
              }
              best = fallback == 0 ? length : fallback;
              if (best < length) {
                impl.SplitAt(&segment, best, styles);
              }
            }
          }

          Line line;
          line.runs = impl.RunsFor(segment, pos, best, styles);
          line.ends_with_hard_break =
              best == length && segment.ends_with_hard_break;
          // 4. The line box (C note §4.4).
          if (line.runs.empty()) {
            const ResolvedStyle& at_break = styles[segment.break_style];
            int entry = fonts_->ResolveRichFace(at_break.style.family,
                                                at_break.style.weight,
                                                at_break.style.italic);
            const CPDF_AnnotFontMap::RichFace face = impl.FaceOf(entry);
            const float size = at_break.style.size;  // parent size
            line.ascent = face.ascent * size;
            line.descent = face.descent * size;
            line.leading = kLeadingFactor * size;
          } else {
            float max_size = 0;
            for (const GlyphRun& run : line.runs) {
              line.ascent = std::max(line.ascent, run.ascent + run.rise);
              max_size = std::max(max_size, run.style.size);
              // A run's descent below the LINE's baseline: a subscript's
              // rise adds to it (measured: the empty line after a subscript
              // sits 0.17 × 11.88 + 2.7 below, not 0.17 × 11.88).
              const float descent = std::max(0.0f, run.descent - run.rise);
              if (options_.parity_descent) {
                line.descent = descent;  // the last run's
              } else {
                line.descent = std::max(line.descent, descent);
              }
              if (run.rise != 0) {
                // A sub/superscript's parent size sets the leading.
                max_size = std::max(max_size, run.style.size / kSubSuperScale);
              }
            }
            line.leading = kLeadingFactor * max_size;
          }
          // 5. Horizontal placement.
          const size_t ink_end = Impl::InkEnd(segment, pos, best);
          line.width = Impl::RangeWidth(segment, pos, ink_end);
          const float slack = std::max(0.0f, avail - line.width);
          // A paragraph whose dir is rtl names its sides from where it
          // starts: left (the default) is its right side.
          Align align = props.align;
          if (props.rtl) {
            if (align == Align::kLeft) {
              align = Align::kRight;
            } else if (align == Align::kRight) {
              align = Align::kLeft;
            }
          }
          const bool justify_this_line = align == Align::kJustify &&
                                         best < length &&
                                         !line.ends_with_hard_break;
          // The indent is on the side the paragraph starts from.
          float start = margin_left + (rtl ? 0 : indent);
          switch (align) {
            case Align::kLeft:
            case Align::kJustify:
              break;
            case Align::kCenter:
              start += slack / 2.0f;
              break;
            case Align::kRight:
              start += slack;
              break;
          }
          if (justify_this_line) {
            // The spaces between words stretch; the line's trailing ones
            // hang.
            const size_t ink_source_end =
                segment.source_pos[pos] + (ink_end - pos);
            auto stretches = [&](const GlyphRun& run,
                                 const ShapedGlyph& glyph) {
              return glyph.cluster < run.text.GetLength() &&
                     IsSpace(run.text[glyph.cluster]) &&
                     run.text_begin + glyph.cluster < ink_source_end;
            };
            int spaces = 0;
            for (const GlyphRun& run : line.runs) {
              for (const ShapedGlyph& glyph : run.glyphs) {
                if (stretches(run, glyph)) {
                  ++spaces;
                }
              }
            }
            if (spaces > 0) {
              const float extra = slack / spaces;
              for (GlyphRun& run : line.runs) {
                for (ShapedGlyph& glyph : run.glyphs) {
                  if (stretches(run, glyph)) {
                    glyph.x_advance += extra;
                    run.width += extra;
                  }
                }
              }
            }
          }
          // The line's trailing spaces hang past its end: past the left
          // edge when the paragraph reads right to left.
          line.start_x = start;
          float x = start;
          if (rtl) {
            x -= Impl::RangeWidth(segment, ink_end, best);
          }
          for (GlyphRun& run : line.runs) {
            run.x = x;
            x += run.width;
            if (run.degraded) {
              result.degraded = true;
            }
          }
          widest = std::max(widest, line.start_x + line.width);
          laid_out.lines.push_back(std::move(line));
          emitted = true;
          first_line_of_paragraph = false;
          pos = best;
          if (best == length) {
            break;
          }
        }
      }
      paragraphs.push_back(std::move(laid_out));
    }

    // 6. Stack the boxes; paragraph margins collapse to the larger one.
    float content_height = 0;
    for (size_t p = 0; p < paragraphs.size(); ++p) {
      if (p == 0) {
        content_height += paragraphs[p].margin_top;
      } else {
        content_height +=
            std::max(paragraphs[p - 1].margin_bottom, paragraphs[p].margin_top);
      }
      for (const Line& line : paragraphs[p].lines) {
        content_height += LineBox(line, document.paragraphs[p].props);
      }
    }
    if (!paragraphs.empty()) {
      content_height += paragraphs.back().margin_bottom;
    }

    const bool last_attempt = attempt + 1 == candidate_sizes.size();
    if (auto_size && !last_attempt &&
        (content_height > area_height + kEpsilon ||
         widest > area_width + kEpsilon)) {
      continue;  // try the next smaller size
    }

    // 7. Vertical alignment of the block, then absolute baselines.
    float top = 0;
    switch (vertical_alignment) {
      case CPDF_Annot::VerticalAlignment::kTop:
        break;
      case CPDF_Annot::VerticalAlignment::kMiddle:
        top = std::max(0.0f, (area_height - content_height) / 2.0f);
        break;
      case CPDF_Annot::VerticalAlignment::kBottom:
        top = std::max(0.0f, area_height - content_height);
        break;
    }
    for (size_t p = 0; p < paragraphs.size(); ++p) {
      top += p == 0 ? paragraphs[p].margin_top
                    : std::max(paragraphs[p - 1].margin_bottom,
                               paragraphs[p].margin_top);
      for (Line& line : paragraphs[p].lines) {
        line.baseline_y = top + line.ascent;
        top += LineBox(line, document.paragraphs[p].props);
        result.lines.push_back(std::move(line));
      }
    }
    result.content_height = content_height;
    result.body_size = body_size;
    result.degraded = result.degraded || impl.degraded;
    return result;
  }
  return result;
}

// ---- Form fields
// --------------------------------------------------------------

namespace {

// A cluster of a field's text: what CPVT called a word.
struct FieldUnit {
  size_t begin = 0;  // in its segment's text
  size_t end = 0;
  float width = 0;
  uint16_t code = 0;  // its first character, as CPVT's word breaking sees it
  float ascent = 0;   // its font's at the size, as CPVT measures it
  float descent = 0;  // negative
};

struct FieldLine {
  int begin = -1;  // units; -1 for an empty section's one line
  int end = -1;
  float width = 0;
  float ascent = 0;
  float descent = 0;  // negative
};

// What a field's text breaks into at line breaks: CPVT's sections.
struct FieldSection {
  Segment segment;
  std::vector<FieldUnit> units;
  std::vector<FieldLine> lines;
  std::vector<float> cell_x;  // a comb's: where each unit starts
  float left = 0;  // the box its lines take, y down from the text's top
  float top = 0;
  float right = 0;
  float bottom = 0;
};

struct FieldExtent {
  float width = 0;   // the widest line
  float height = 0;  // the lines' heights, summed
};

// CPVT's character for its word breaking: a code unit, so a supplementary
// character is its high surrogate.
uint16_t BreakCodeOf(wchar_t ch) {
  const uint32_t code_point = static_cast<uint32_t>(ch);
  if (pdfium::IsSupplementary(code_point)) {
    return pdfium::SurrogatePair(static_cast<char32_t>(code_point)).high();
  }
  return static_cast<uint16_t>(code_point);
}

size_t Utf16LengthOf(WideStringView text) {
  size_t length = 0;
  for (char32_t code_point : pdfium::CodePointView(text)) {
    length += pdfium::IsSupplementary(code_point) ? 2 : 1;
  }
  return length;
}

// The text's sections and characters as CPVT's SetText() takes them: line
// breaks (CR, LF or both) start a section in a multi-line field and are
// dropped in any other, a tab is a space, and the limit or the comb's cells
// stop it. Characters (graphemes) are kept whole; a password's are '*'.
std::vector<WideString> FieldSectionsOf(
    const CPDF_RichTextLayout::Field& field) {
  const WideString& text = field.text;
  const std::vector<bool> starts = CPDF_RichTextLayout::GraphemeStarts(text);
  std::vector<WideString> sections(1);
  // What CPVT counts: the text taken, line breaks included, and what it
  // kept, a break between sections included; the limit in code units, the
  // cells in characters.
  size_t taken_units = 0;
  size_t taken_characters = 0;
  size_t kept_units = 0;
  size_t kept_characters = 0;
  for (size_t begin = 0; begin < text.GetLength();) {
    size_t end = begin + 1;
    while (end < text.GetLength() && !starts[end]) {
      ++end;
    }
    const WideStringView grapheme =
        text.AsStringView().Substr(begin, end - begin);
    const size_t units = Utf16LengthOf(grapheme);
    if (field.limit > 0 && taken_units + units > field.limit) {
      break;
    }
    if (field.cells > 0 && taken_characters >= field.cells) {
      break;
    }
    const wchar_t first = text[begin];
    if (first == L'\r' || first == L'\n') {
      if (field.multiline) {
        // CR LF is one break.
        const wchar_t pair = first == L'\r' ? L'\n' : L'\r';
        if (end < text.GetLength() && text[end] == pair) {
          ++end;
        }
        sections.emplace_back();
        ++kept_units;
        ++kept_characters;
      }
    } else {
      const bool full = (field.limit > 0 && kept_units + units > field.limit) ||
                        (field.cells > 0 && kept_characters >= field.cells);
      if (!full) {
        if (field.password) {
          sections.back() += L'*';
        } else if (first == L'\t') {
          sections.back() += L' ';
        } else {
          sections.back() += WideString(grapheme);
        }
        kept_units += units;
        ++kept_characters;
      }
    }
    taken_units += units;
    ++taken_characters;
    begin = end;
  }
  return sections;
}

// CPVT's SplitLines() over a section's units: the lines they break into
// when |wrap| (before the word that would pass |typeset_width|, or before
// the unit when one word is wider), and the box they take.
FieldExtent SplitFieldLines(const std::vector<FieldUnit>& units,
                            bool wrap,
                            float typeset_width,
                            float empty_ascent,
                            float empty_descent,
                            std::vector<FieldLine>* lines) {
  if (units.empty()) {
    if (lines) {
      lines->push_back({-1, -1, 0, empty_ascent, empty_descent});
    }
    return {0, empty_ascent - empty_descent};
  }
  const int total = pdfium::checked_cast<int>(units.size());
  int head = 0;
  float max_x = 0;
  float max_y = 0;
  float line_width = 0;
  float backup_width = 0;
  float line_ascent = 0;
  float backup_ascent = 0;
  float line_descent = 0;
  float backup_descent = 0;
  int word_start = 0;
  bool full_word = false;
  int full_words = 0;
  int char_index = 0;
  bool opened = false;
  auto add_line = [&](int tail) {
    if (lines) {
      lines->push_back({head, tail, line_width, line_ascent, line_descent});
    }
    max_y += line_ascent;
    max_y -= line_descent;
    max_x = std::max(line_width, max_x);
  };
  int i = 0;
  while (i < total) {
    const FieldUnit& unit = units[i];
    const FieldUnit& previous = units[i > 0 ? i - 1 : i];
    line_ascent = std::max(line_ascent, unit.ascent);
    line_descent = std::min(line_descent, unit.descent);
    if (!opened) {
      if (CPVT_IsOpenStylePunctuation(unit.code)) {
        opened = true;
        full_word = true;
      } else if (CPVT_NeedDivision(previous.code, unit.code)) {
        full_word = true;
      }
    } else if (!CPVT_IsSpace(unit.code) &&
               !CPVT_IsOpenStylePunctuation(unit.code)) {
      opened = false;
    }
    if (full_word) {
      full_word = false;
      if (char_index > 0) {
        ++full_words;
      }
      word_start = i;
      backup_width = line_width;
      backup_ascent = line_ascent;
      backup_descent = line_descent;
    }
    ++char_index;
    if (wrap && typeset_width > 0 && line_width + unit.width > typeset_width) {
      if (full_words > 0) {
        i = word_start;
        line_width = backup_width;
        line_ascent = backup_ascent;
        line_descent = backup_descent;
      }
      if (char_index == 1) {
        line_width = unit.width;
        ++i;
      }
      add_line(i - 1);
      head = i;
      line_width = 0;
      line_ascent = 0;
      line_descent = 0;
      char_index = 0;
      full_words = 0;
      full_word = false;
    } else {
      line_width += unit.width;
      ++i;
    }
  }
  if (head <= total - 1) {
    add_line(total - 1);
  }
  return {max_x, max_y};
}

// Where a line |width| wide starts in |typeset_width|, by /Q.
float FieldAlignmentOffset(int alignment, float typeset_width, float width) {
  switch (alignment) {
    case 1:
      return (typeset_width - width) * 0.5f;
    case 2:
      return typeset_width - width;
    default:
      return 0.0f;
  }
}

}  // namespace

CPDF_RichTextLayout::Result CPDF_RichTextLayout::ArrangeField(
    const Field& field,
    const CFX_FloatRect& plate) {
  Result result;
  const float plate_width = plate.right - plate.left;
  const float plate_height = plate.top - plate.bottom;
  const std::vector<WideString> texts = FieldSectionsOf(field);
  // A field reads in the direction of its first strong character.
  WideString all_text;
  for (const WideString& text : texts) {
    all_text += text;
  }
  const bool rtl = CFX_BidiParagraph::FirstStrongIsRtl(all_text.AsStringView())
                       .value_or(false);
  const bool comb = field.cells > 0;

  // The fonts' ascent and descent, as CPVT measures a line with them.
  std::map<int, std::pair<int, int>> metrics;
  auto metrics_of = [&](int entry) {
    auto it = metrics.find(entry);
    if (it == metrics.end()) {
      RetainPtr<CPDF_Font> font = fonts_->GetPDFFont(entry);
      it = metrics
               .emplace(entry, font ? std::make_pair(font->GetTypeAscent(),
                                                     font->GetTypeDescent())
                                    : std::make_pair(0, 0))
               .first;
    }
    return it->second;
  };

  // The sections, itemised and shaped at |size|, cut into units.
  auto sections_at = [&](Impl* impl, float size) {
    ResolvedStyle style;
    style.style.size = size;
    style.style.weight = field.weight;
    style.style.italic = field.italic;
    style.drawn_size = size;
    style.face_entry = field.face_entry;
    const std::vector<ResolvedStyle> styles = {style};
    std::vector<FieldSection> sections(texts.size());
    size_t source_pos = 0;
    for (size_t s = 0; s < texts.size(); ++s) {
      Segment& segment = sections[s].segment;
      segment.text = texts[s];
      segment.style_of.assign(segment.text.GetLength(), 0);
      for (size_t i = 0; i < segment.text.GetLength(); ++i) {
        segment.source_pos.push_back(source_pos++);
      }
      ++source_pos;  // the line break
      impl->Itemize(&segment, rtl, styles);
      const std::vector<bool> starts = GraphemeStarts(segment.text);
      const size_t length = segment.text.GetLength();
      for (size_t begin = 0; begin < length;) {
        size_t end = begin + 1;
        while (end < length &&
               !(starts[end] && Impl::IsClusterBoundary(segment, end))) {
          ++end;
        }
        FieldUnit unit;
        unit.begin = begin;
        unit.end = end;
        unit.width = Impl::RangeWidth(segment, begin, end);
        unit.code = BreakCodeOf(segment.text[begin]);
        for (const Item& item : segment.items) {
          if (begin >= item.begin && begin < item.end) {
            const auto [ascent, descent] = metrics_of(item.entry);
            unit.ascent = ascent * size * 0.001f;
            unit.descent = descent * size * 0.001f;
            break;
          }
        }
        sections[s].units.push_back(unit);
        begin = end;
      }
    }
    return sections;
  };
  auto face_ascent = [&](float size) {
    return metrics_of(field.face_entry).first * size * 0.001f;
  };
  auto face_descent = [&](float size) {
    return metrics_of(field.face_entry).second * size * 0.001f;
  };
  const float typeset_width = std::max(plate_width, 0.0f);

  // The size: the biggest step of the ladder the text fits in, searched as
  // CPVT's std::lower_bound searches it.
  float size = field.size;
  if (FXSYS_IsFloatZero(size)) {
    size = 0;
    if (plate_width > 0) {
      const size_t steps =
          field.multiline ? kAutoSizeSteps.size() / 4 : kAutoSizeSteps.size();
      auto too_big = [&](float candidate) {
        Impl impl(fonts_, options_);
        impl.field = true;
        float width = 0;
        float height = 0;
        for (const FieldSection& section : sections_at(&impl, candidate)) {
          const FieldExtent extent = SplitFieldLines(
              section.units, field.multiline, typeset_width,
              face_ascent(candidate), face_descent(candidate), nullptr);
          width = std::max(extent.width, width);
          height += extent.height;
          if (FXSYS_IsFloatBigger(width, plate_width) ||
              FXSYS_IsFloatBigger(height, plate_height)) {
            return true;
          }
        }
        return false;
      };
      size_t first = 0;
      size_t count = steps;
      while (count > 0) {
        const size_t step = count / 2;
        if (!too_big(kAutoSizeSteps[first + step])) {
          first += step + 1;
          count -= step + 1;
        } else {
          count = step;
        }
      }
      size = first == steps ? kAutoSizeSteps[steps - 1]
                            : kAutoSizeSteps[first == 0 ? 0 : first - 1];
    }
  }

  Impl impl(fonts_, options_);
  impl.field = true;
  std::vector<FieldSection> sections = sections_at(&impl, size);
  const std::vector<ResolvedStyle> styles = [&] {
    ResolvedStyle style;
    style.style.size = size;
    style.drawn_size = size;
    style.face_entry = field.face_entry;
    return std::vector<ResolvedStyle>{style};
  }();

  // Each section's lines, placed as CPVT's RearrangeTypeset() and
  // RearrangeCharArray() place them, the sections one under the other.
  float position_y = 0;
  float content_height = 0;
  for (FieldSection& section : sections) {
    if (comb) {
      // Each unit in the middle of its cell, the cells filled from the
      // left, the middle or the right; every baseline at the face's ascent.
      const float node_width = plate_width / static_cast<float>(field.cells);
      const int units = pdfium::checked_cast<int>(section.units.size());
      const int cells = pdfium::checked_cast<int>(field.cells);
      int start = 0;
      if (field.alignment == 1) {
        start = (cells - units) / 2;
      } else if (field.alignment == 2) {
        start = cells - units;
      }
      const float ascent = face_ascent(size);
      float line_descent = face_descent(size);
      float x = 0;
      for (int i = 0; i < units && i < cells; ++i) {
        const FieldUnit& unit = section.units[i];
        // In double, as CPVT computes it.
        x = static_cast<float>(node_width * (i + start + 0.5) -
                               unit.width * 0.5f);
        section.cell_x.push_back(x);
        x += unit.width;
        line_descent = std::min(line_descent, unit.descent);
      }
      section.lines = {
          {units > 0 ? 0 : -1, units - 1, 0, ascent, line_descent}};
      section.top = position_y;
      section.right = x;
      section.bottom = section.top + (ascent - line_descent);
    } else {
      const FieldExtent extent = SplitFieldLines(
          section.units, field.multiline, typeset_width, face_ascent(size),
          face_descent(size), &section.lines);
      section.left =
          FieldAlignmentOffset(field.alignment, typeset_width, extent.width);
      section.right = section.left + extent.width;
      section.top = position_y;
      section.bottom = position_y + extent.height;
    }
    position_y += section.bottom - section.top;
    content_height += section.bottom - section.top;
  }

  // A one-line field's text has its middle at the plate's middle.
  const float offset_y =
      field.centered ? (content_height - plate_height) / 2.0f : 0.0f;

  for (FieldSection& section : sections) {
    const Segment& segment = section.segment;
    float line_top = 0;  // the baseline steps down from the section's top
    for (const FieldLine& field_line : section.lines) {
      Line line;
      line.ascent = field_line.ascent;
      line.descent = -field_line.descent;
      line.width = field_line.width;
      line_top += field_line.ascent;
      line.baseline_y = (line_top + section.top) - offset_y;
      line_top -= field_line.descent;
      if (field_line.begin >= 0) {
        if (comb) {
          // A run per cell.
          for (size_t i = 0; i < section.cell_x.size(); ++i) {
            const FieldUnit& unit = section.units[i];
            for (GlyphRun& run :
                 Impl::RunsFor(segment, unit.begin, unit.end, styles)) {
              run.x = section.cell_x[i];
              line.runs.push_back(std::move(run));
            }
          }
          line.start_x = line.runs.empty() ? 0 : line.runs.front().x;
        } else {
          const size_t begin = section.units[field_line.begin].begin;
          const size_t end = section.units[field_line.end].end;
          float x = FieldAlignmentOffset(field.alignment, typeset_width,
                                         field_line.width);
          line.start_x = (x - section.left) + section.left;
          for (GlyphRun& run : Impl::RunsFor(segment, begin, end, styles)) {
            run.x = (x - section.left) + section.left;
            x += run.width;
            line.runs.push_back(std::move(run));
          }
        }
        for (const GlyphRun& run : line.runs) {
          result.degraded = result.degraded || run.degraded;
        }
      }
      result.lines.push_back(std::move(line));
    }
  }
  result.content_height = content_height;
  result.body_size = size;
  result.degraded = result.degraded || impl.degraded;
  return result;
}
