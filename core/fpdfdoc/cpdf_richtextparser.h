// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: reads a FreeText annotation's rich text (/RC XHTML with the XFA
// rich text vocabulary, /DS default style, /DA) into a CPDF_RichTextDocument,
// and writes that document as JSON for the public API. Read-only: nothing here
// mutates a document.

#ifndef CORE_FPDFDOC_CPDF_RICHTEXTPARSER_H_
#define CORE_FPDFDOC_CPDF_RICHTEXTPARSER_H_

#include <vector>

#include "core/fpdfdoc/cpdf_richtext.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/widestring.h"

class CPDF_Dictionary;

class CPDF_RichTextParser {
 public:
  // Parse a `style="k:v; k:v"` declaration list. Character properties land
  // in |style|; paragraph properties (text-align, margins, line-height…) land
  // in |paragraph| when given, so a text-align on a <span> still reaches the
  // paragraph that contains it (Acrobat writes it there). Unknown properties
  // are kept verbatim and reported.
  static void ParseInlineStyle(
      const WideString& css,
      CPDF_RichTextStyleDelta* style,
      CPDF_RichTextParagraphProps* paragraph,
      std::vector<CPDF_RichTextDiagnostic>* diagnostics);

  // /DS: the same declarations plus the `font:` shorthand Acrobat writes
  // ("font: bold Helvetica,sans-serif 24.0pt").
  static CPDF_RichTextStyleDelta ParseDefaultStyle(
      const WideString& ds,
      CPDF_RichTextParagraphProps* paragraph,
      std::vector<CPDF_RichTextDiagnostic>* diagnostics);

  // /RC. |defaults| is DA ∪ DS; the <body> style is applied on top. When the
  // XML cannot be parsed the result is FromPlainText(|contents_fallback|) with
  // a kMalformedRC diagnostic: never an empty box.
  static CPDF_RichTextDocument ParseRichContent(
      const WideString& rc_xml,
      const CPDF_RichTextStyle& defaults,
      const CPDF_RichTextParagraphProps& paragraph_defaults,
      const WideString& contents_fallback);

  // /Contents only: one paragraph per line break, one run each, no overrides.
  static CPDF_RichTextDocument FromPlainText(
      const WideString& contents,
      const CPDF_RichTextStyle& defaults,
      const CPDF_RichTextParagraphProps& paragraph_defaults);

  // The document an annotation holds: /RC (or /RV for widgets) over
  // DA ∪ DS defaults, else its /Contents (or /V) as plain text.
  static CPDF_RichTextDocument FromAnnotation(
      const CPDF_Dictionary* annot_dict,
      const CPDF_Dictionary* acroform_dict);

  // DA ∪ DS alone (no /RC): the defaults an imported XHTML body style is
  // applied on top of.
  static void DefaultsFromAnnotation(
      const CPDF_Dictionary* annot_dict,
      const CPDF_Dictionary* acroform_dict,
      CPDF_RichTextStyle* style,
      CPDF_RichTextParagraphProps* paragraph,
      std::vector<CPDF_RichTextDiagnostic>* diagnostics);

  // UTF-8 JSON, the wire shape of EPDFAnnot_GetRichTextJSON().
  static ByteString ToJSON(const CPDF_RichTextDocument& document);
};

#endif  // CORE_FPDFDOC_CPDF_RICHTEXTPARSER_H_
