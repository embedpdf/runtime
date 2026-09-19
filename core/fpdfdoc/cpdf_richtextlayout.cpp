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
#include "core/fxcrt/code_point_view.h"
#include "core/fxcrt/fx_linebreak.h"
#include "core/fxcrt/fx_unicode.h"
#include "core/fxcrt/numerics/safe_conversions.h"
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

// CPVT's auto font size ladder, kept for plain FreeText with a DA size of 0.
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

bool IsRtlScript(hb_script_t script) {
  return hb_script_get_horizontal_direction(script) == HB_DIRECTION_RTL;
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

FX_BREAKPROPERTY BreakClassOf(wchar_t ch) {
  if (static_cast<uint32_t>(ch) > 0xFFFF) {
    const uint32_t cp = static_cast<uint32_t>(ch);
    return (cp >= 0x1F000 && cp <= 0x1FAFF) ? FX_BREAKPROPERTY::kID
                                            : FX_BREAKPROPERTY::kAL;
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
};

// One shaping unit: one font entry, one style, one script and direction,
// covering [begin, end) of the segment's text.
struct Item {
  int entry = -1;
  int style_index = 0;
  size_t begin = 0;
  size_t end = 0;
  hb_script_t script = HB_SCRIPT_COMMON;
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
std::vector<bool> CPDF_RichTextLayout::BreakOpportunities(
    const WideString& text) {
  const size_t length = text.GetLength();
  std::vector<bool> opportunities(length, false);
  if (length == 0) {
    return opportunities;
  }
  const std::vector<bool> grapheme_starts = GraphemeStarts(text);

  // Classes, with marks and joiners attached to their base (UAX #14 LB9).
  std::vector<FX_BREAKPROPERTY> classes(length);
  for (size_t i = 0; i < length; ++i) {
    FX_BREAKPROPERTY cls = BreakClassOf(text[i]);
    if (i > 0 && !grapheme_starts[i]) {
      cls = classes[i - 1];
    } else if (cls == FX_BREAKPROPERTY::kCM || cls == FX_BREAKPROPERTY::kZW) {
      cls = i > 0 ? classes[i - 1] : FX_BREAKPROPERTY::kAL;
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
    const FX_BREAKPROPERTY before = classes[j - 1];
    if (before == FX_BREAKPROPERTY::kZW) {
      opportunities[i] = true;  // LB8
      continue;
    }
    switch (GetLineBreakTypeFromPair(before, classes[i])) {
      case FX_LINEBREAKTYPE::kDIRECT_BRK:
        opportunities[i] = true;
        break;
      case FX_LINEBREAKTYPE::kINDIRECT_BRK:
      case FX_LINEBREAKTYPE::kCOM_INDIRECT_BRK:
      case FX_LINEBREAKTYPE::kHANGUL_SPACE_BRK:
      case FX_LINEBREAKTYPE::kUNKNOWN:
        opportunities[i] = spaces;
        break;
      case FX_LINEBREAKTYPE::kPROHIBITED_BRK:
      case FX_LINEBREAKTYPE::kCOM_PROHIBITED_BRK:
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

  // Does |entry| have a glyph for every character of the grapheme at
  // [begin, end)? Joiners and variation selectors may be missing.
  bool Covers(int entry, const WideString& text, size_t begin, size_t end) {
    // Scalars, not code units: a supplementary character is one grapheme
    // of two units on a 16-bit wchar_t platform.
    for (char32_t cp : pdfium::CodePointView(
             text.AsStringView().Substr(begin, end - begin))) {
      if (cp == 0x200D || IsVariationSelector(cp)) {
        continue;
      }
      if (fonts->RichGlyphFor(entry, static_cast<uint32_t>(cp)) == 0) {
        return false;
      }
    }
    return true;
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
    item->rtl = IsRtlScript(item->script);

    HbFont* hb = face.program ? HbFontOf(item->entry) : nullptr;
    if (!hb) {
      // Standard 14 (or a program HarfBuzz refused): one glyph per
      // character through the font's encoding.
      for (size_t i = item->begin; i < item->end; ++i) {
        const uint32_t code = GlyphFor(item->entry, text[i]);
        ShapedGlyph glyph;
        glyph.gid = code;
        glyph.cluster = pdfium::checked_cast<uint32_t>(i);
        const int width =
            face.pdf_font && code ? face.pdf_font->GetCharWidthF(code) : 0;
        glyph.x_advance = (width * size / 1000.0f + letter_spacing) * scale_x;
        item->glyphs.push_back(glyph);
      }
      item->needs_actual_text = false;
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
          face.program->GetGlyphWidth(glyph.gid) * size / 1000.0f;
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

  // Ink width of the segment's text in [begin, end): trailing spaces do
  // not count (Acrobat centres and wraps without them).
  static float InkWidth(const Segment& segment, size_t begin, size_t end) {
    while (end > begin && IsSpace(segment.text[end - 1])) {
      --end;
    }
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

  // Builds the items of a segment: runs of one style, split by script and
  // by the face that covers each grapheme, then shaped.
  void Itemize(Segment* segment, const std::vector<ResolvedStyle>& styles) {
    const WideString& text = segment->text;
    const size_t length = text.GetLength();
    segment->items.clear();
    if (length == 0) {
      return;
    }
    const std::vector<bool> grapheme_starts = GraphemeStarts(text);

    // Per grapheme: style, script, entry.
    struct Piece {
      size_t begin, end;
      int style;
      hb_script_t script;
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
      int entry = fonts->ResolveRichFace(resolved.family, resolved.weight,
                                         resolved.italic);
      if (entry >= 0 && !Covers(entry, text, begin, end)) {
        // The grapheme's base scalar picks the fallback font.
        uint32_t base = 0;
        for (char32_t cp : pdfium::CodePointView(
                 text.AsStringView().Substr(begin, end - begin))) {
          if (base == 0 || !IsMark(static_cast<uint32_t>(cp))) {
            base = static_cast<uint32_t>(cp);
            if (!IsMark(base)) {
              break;
            }
          }
        }
        const int fallback =
            fonts->FindRichFallback(base, resolved.weight, resolved.italic);
        if (fallback >= 0 && Covers(fallback, text, begin, end)) {
          entry = fallback;
        }
      }
      pieces.push_back({begin, end, style, script, entry});
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
          last->entry == piece.entry &&
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
      segment->items.push_back(std::move(item));
    }
    for (Item& item : segment->items) {
      Shape(&item, text, styles[item.style_index]);
      if (item.degraded) {
        degraded = true;
      }
    }
  }

  // The runs of a line covering [begin, end) of the segment.
  std::vector<GlyphRun> RunsFor(const Segment& segment,
                                size_t begin,
                                size_t end,
                                const std::vector<ResolvedStyle>& styles) {
    std::vector<GlyphRun> runs;
    for (const Item& item : segment.items) {
      if (item.end <= begin || item.begin >= end) {
        continue;
      }
      const size_t run_begin = std::max(begin, item.begin);
      const size_t run_end = std::min(end, item.end);
      const ResolvedStyle& resolved = styles[item.style_index];
      GlyphRun run;
      run.font_entry = item.entry;
      run.style = resolved.style;
      run.style.size = resolved.drawn_size;
      run.rise = resolved.rise;
      run.text = segment.text.Substr(run_begin, run_end - run_begin);
      run.text_begin = segment.source_pos[run_begin];
      run.text_end = run_end < segment.source_pos.size()
                         ? segment.source_pos[run_end]
                         : segment.source_pos[run_end - 1] + 1;
      run.rtl = item.rtl;
      run.bidi_level = item.rtl ? 1 : 0;
      run.ascent = item.ascent;
      run.descent = item.descent;
      run.needs_actual_text = item.needs_actual_text;
      run.degraded = item.degraded;
      for (const ShapedGlyph& glyph : item.glyphs) {
        if (glyph.cluster >= run_begin && glyph.cluster < run_end) {
          ShapedGlyph copy = glyph;
          copy.cluster -= pdfium::checked_cast<uint32_t>(run_begin);
          run.glyphs.push_back(copy);
          run.width += glyph.x_advance;
        }
      }
      runs.push_back(std::move(run));
    }
    return runs;
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

      // 3. Each segment: itemise, shape, break into lines.
      const float margin_left = std::max(0.0f, props.margin_left);
      const float margin_right = std::max(0.0f, props.margin_right);
      bool first_line_of_paragraph = true;
      for (Segment& segment : segments) {
        impl.Itemize(&segment, styles);
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
          line.width = Impl::InkWidth(segment, pos, best);
          const float slack = std::max(0.0f, avail - line.width);
          Align align = props.align;
          if (props.rtl) {
            std::reverse(line.runs.begin(), line.runs.end());
            if (align == Align::kLeft) {
              align = Align::kRight;
            } else if (align == Align::kRight) {
              align = Align::kLeft;
            }
          }
          const bool justify_this_line = align == Align::kJustify &&
                                         best < length &&
                                         !line.ends_with_hard_break;
          float start = margin_left + indent;
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
            int spaces = 0;
            for (const GlyphRun& run : line.runs) {
              for (const ShapedGlyph& glyph : run.glyphs) {
                if (glyph.cluster < run.text.GetLength() &&
                    IsSpace(run.text[glyph.cluster])) {
                  ++spaces;
                }
              }
            }
            if (spaces > 0) {
              const float extra = slack / spaces;
              for (GlyphRun& run : line.runs) {
                for (ShapedGlyph& glyph : run.glyphs) {
                  if (glyph.cluster < run.text.GetLength() &&
                      IsSpace(run.text[glyph.cluster])) {
                    glyph.x_advance += extra;
                    run.width += extra;
                  }
                }
              }
            }
          }
          line.start_x = start;
          float x = start;
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
