// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: reads the JSON shape EPDFAnnot_GetRichTextJSON() writes (plan
// §4.7) back into a CPDF_RichTextDocument for EPDFAnnot_SetRichTextJSON().
// `source` and `diagnostics` are ignored; `body` is optional (a document
// without one keeps the annotation's current body style).

#ifndef CORE_FPDFDOC_CPDF_RICHTEXTJSON_H_
#define CORE_FPDFDOC_CPDF_RICHTEXTJSON_H_

#include "core/fpdfdoc/cpdf_richtext.h"
#include "core/fxcrt/bytestring.h"

class CPDF_RichTextJson {
 public:
  // False for invalid JSON or a shape that is not a rich text document
  // (|out| is then unspecified). |*has_body| says whether "body" was given;
  // when it was, |out->body| holds it (engine defaults under the given
  // style keys). Paragraph properties the JSON leaves unsaid — the body's
  // alignment and direction, and every paragraph's — come from
  // |base_paragraph| when one is given (the annotation's current /Q, /DS
  // and /RC body), so "no body" and "no align" both mean "keep".
  static bool Parse(const ByteString& json_utf8,
                    CPDF_RichTextDocument* out,
                    bool* has_body,
                    const CPDF_RichTextParagraphProps* base_paragraph = nullptr);
};

#endif  // CORE_FPDFDOC_CPDF_RICHTEXTJSON_H_
