// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: lays a CPDF_RichTextDocument out into lines of shaped glyph
// runs, the way Acrobat does it (plan §4.4, Phase C note §4): HarfBuzz
// shaping per run, UAX #14 break opportunities snapped to grapheme
// boundaries, the Unicode bidi algorithm (UAX #9) ordering each line, and the
// measured line model (ascent = max, descent = the last run's, leading =
// 0.2 em of the largest size). Fonts come from a
// CPDF_AnnotFontMap, which the layout extends with fallbacks as it goes.
// Nothing here touches a document.

#ifndef CORE_FPDFDOC_CPDF_RICHTEXTLAYOUT_H_
#define CORE_FPDFDOC_CPDF_RICHTEXTLAYOUT_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_richtext.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/unowned_ptr.h"
#include "core/fxcrt/widestring.h"

class CPDF_AnnotFontMap;

class CPDF_RichTextLayout {
 public:
  struct ShapedGlyph {
    uint32_t gid = 0;      // glyph index; for standard 14, the charcode
    uint32_t cluster = 0;  // index into the run's text
    float x_advance = 0;   // pt at the run's size, letter-spacing included
    float y_advance = 0;
    float x_offset = 0;  // pt; mark positioning
    float y_offset = 0;
    bool unsafe_to_break = false;
  };

  // One font, one resolved style, one segment of a line.
  struct GlyphRun {
    int font_entry = -1;
    CPDF_RichTextStyle style;  // resolved; style.size is the drawn size
    float rise = 0;            // pt; sub/superscript
    WideString text;           // the logical text this run draws
    size_t text_begin = 0;     // range within the paragraph's logical text
    size_t text_end = 0;
    uint8_t bidi_level = 0;  // resolved embedding level; odd = right to left
    bool rtl = false;        // shaped right to left
    std::vector<ShapedGlyph> glyphs;  // visual order for the run's direction
    float x = 0;        // start, from the text area's left edge, pt
    float width = 0;    // sum of advances
    float ascent = 0;   // pt, at the run's size (rise not included)
    float descent = 0;  // pt, positive
    bool needs_actual_text = false;  // glyph sequence != logical text
    bool degraded = false;           // a substitute face drew it
  };

  struct Line {
    std::vector<GlyphRun> runs;  // visual order
    float start_x = 0;           // from the text area's left edge
    float baseline_y = 0;        // from the text area's top edge, positive down
    float ascent = 0;
    float descent = 0;
    float leading = 0;
    float width = 0;  // ink width (trailing spaces excluded)
    bool ends_with_hard_break = false;
  };

  struct Options {
    // Latin `kern`, `liga`, `clig`, `calt`, `dlig`: off for Acrobat parity.
    bool typographic_features = false;
    // Acrobat's line descent is the last run's; false = max over runs.
    bool parity_descent = true;
    // A paragraph reads in the direction of its first strong character (left
    // to right when it has none), as a form field does, instead of its dir.
    bool direction_from_text = false;
  };

  struct Result {
    std::vector<Line> lines;
    float content_height = 0;  // sum of line boxes and paragraph margins
    float body_size = 0;       // the size laid out with (auto size resolved)
    bool degraded = false;     // some run used a substitute face
    bool auto_size_fell_back = false;  // size 0 with several styles: 12 pt
  };

  CPDF_RichTextLayout(CPDF_AnnotFontMap* fonts, const Options& options);
  ~CPDF_RichTextLayout();

  // |area| is the text area (the rect deflated by border + padding), in page
  // coordinates; line positions come back relative to its top-left corner.
  Result Arrange(const CPDF_RichTextDocument& document,
                 const CFX_FloatRect& area,
                 CPDF_Annot::VerticalAlignment vertical_alignment);

  // Pure pieces, exposed for tests.
  // True at |i| when a line may start with character |i| (never at 0).
  static std::vector<bool> BreakOpportunities(const WideString& text);
  // True at |i| when character |i| starts a grapheme (marks, variation
  // selectors, ZWJ sequences and low surrogates extend the previous one).
  static std::vector<bool> GraphemeStarts(const WideString& text);

  // The font a grapheme draws with when its run's face is |face_entry|: the
  // face, when it maps every character of the grapheme; else the first
  // fallback font, in the order for the style, that maps every character.
  // |covered| is false when none does: the grapheme draws as the last-resort
  // font's box, or as the face's missing glyph when there is no last resort
  // or the grapheme draws nothing. Joiners and variation selectors don't
  // need a glyph.
  struct GraphemeMatch {
    int entry = -1;
    bool covered = false;
  };
  static GraphemeMatch MatchGrapheme(CPDF_AnnotFontMap* fonts,
                                     int face_entry,
                                     WideStringView grapheme,
                                     int weight,
                                     bool italic);
  // Whether a grapheme draws nothing a font must provide: controls and
  // default-ignorable characters (joiners, direction marks, variation
  // selectors, the zero width space, the soft hyphen, ...).
  static bool IsInvisibleGrapheme(WideStringView grapheme);

 private:
  struct Impl;
  UnownedPtr<CPDF_AnnotFontMap> const fonts_;
  const Options options_;
};

#endif  // CORE_FPDFDOC_CPDF_RICHTEXTLAYOUT_H_
