// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: the rich text model behind a FreeText annotation's /RC (and a
// rich text field's /RV). The source document is paragraphs of runs; a run
// stores only the style properties it overrides (deltas), exactly as the XHTML
// spans do, so changing the body style moves every run that did not override
// that property. The cascade is resolved when layout starts, never earlier.

#ifndef CORE_FPDFDOC_CPDF_RICHTEXT_H_
#define CORE_FPDFDOC_CPDF_RICHTEXT_H_

#include <stdint.h>

#include <optional>
#include <vector>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/widestring.h"
#include "core/fxge/dib/fx_dib.h"

// A fully resolved character style.
struct CPDF_RichTextStyle {
  enum class Script : uint8_t { kNormal, kSub, kSuper };
  static constexpr uint8_t kUnderline = 1;
  static constexpr uint8_t kLineThrough = 2;
  static constexpr uint8_t kWordUnderline =
      4;  // Acrobat's text-decoration:word

  WideString family;  // as written in RC: "Noto Sans", "Helvetica"
  int weight = 400;   // 100..900
  bool italic = false;
  float size = 12.0f;  // pt; 0 means auto (DA font size 0)
  FX_ARGB color = 0xFF000000;
  uint8_t decoration = 0;  // bit set of kUnderline/kLineThrough/kWordUnderline
  Script script = Script::kNormal;
  float letter_spacing = 0.0f;  // pt
  float horz_scale = 1.0f;      // xfa-font-horizontal-scale / 100
};

// What a run overrides. Unknown declarations are kept verbatim so a later
// serialisation does not drop what we do not model.
struct CPDF_RichTextStyleDelta {
  std::optional<WideString> family;
  std::optional<int> weight;
  std::optional<bool> italic;
  std::optional<float> size;
  std::optional<FX_ARGB> color;
  std::optional<uint8_t> decoration;
  std::optional<CPDF_RichTextStyle::Script> script;
  std::optional<float> letter_spacing;
  std::optional<float> horz_scale;
  WideString unknown_declarations;  // "kerning-mode:pair;..." as written

  bool IsEmpty() const;
};

CPDF_RichTextStyle ApplyRichTextStyleDelta(
    const CPDF_RichTextStyle& base,
    const CPDF_RichTextStyleDelta& delta);

struct CPDF_RichTextParagraphProps {
  enum class Align : uint8_t { kLeft, kCenter, kRight, kJustify };

  Align align = Align::kLeft;
  bool rtl = false;  // <p dir="rtl">
  float margin_top = 0.0f;
  float margin_bottom = 0.0f;
  float margin_left = 0.0f;
  float margin_right = 0.0f;
  float text_indent = 0.0f;
  std::optional<float> line_height;  // pt; nullopt = normal
  WideString unknown_declarations;
};

struct CPDF_RichTextRun {
  WideString text;                // may contain '\r' (a hard line break)
  CPDF_RichTextStyleDelta style;  // overrides only
};

struct CPDF_RichTextParagraph {
  CPDF_RichTextParagraphProps props;
  std::vector<CPDF_RichTextRun> runs;
};

struct CPDF_RichTextDiagnostic {
  enum class Code : uint8_t {
    kMalformedRC,         // /RC did not parse; document built from /Contents
    kUnsupportedTag,      // an element outside the XFA rich text set
    kListDegraded,        // <ol>/<ul>/<li> rendered as plain paragraphs
    kUnknownDeclaration,  // a CSS property we do not model (kept verbatim)
    kInvalidValue,        // a value we could not parse
  };
  Code code;
  ByteString detail;
};

struct CPDF_RichTextDocument {
  enum class Source : uint8_t { kRC, kContents };

  Source source = Source::kContents;
  CPDF_RichTextStyle body;  // DA ∪ DS ∪ <body style>, resolved
  CPDF_RichTextParagraphProps
      body_paragraph;  // paragraph defaults (text-align…)
  std::vector<CPDF_RichTextParagraph> paragraphs;
  std::vector<CPDF_RichTextDiagnostic> diagnostics;
};

#endif  // CORE_FPDFDOC_CPDF_RICHTEXT_H_
