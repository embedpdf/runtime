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
  // when it was, |out->body| and |out->body_paragraph| hold it.
  static bool Parse(const ByteString& json_utf8,
                    CPDF_RichTextDocument* out,
                    bool* has_body);
};

#endif  // CORE_FPDFDOC_CPDF_RICHTEXTJSON_H_
