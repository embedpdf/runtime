// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFDOC_CPDF_ANNOT_BORDER_H_
#define CORE_FPDFDOC_CPDF_ANNOT_BORDER_H_

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fxcrt/retain_ptr.h"

class CPDF_Dictionary;

namespace fpdfdoc {

// An annotation's border, read by the one rule of ISO 32000-1 12.5.2: its
// /BS when it has one, and /Border is then not read at all; else
// /Border [h v w [dash]]; else a solid border 1 wide.
//
// Our code reads a border here and nowhere else: the appearance generator
// draws every kind from this reading, and EPDFAnnot_GetBorderStyle() and the
// dash pattern readers report it. So a border is drawn as it reads,
// whichever key holds it.
struct AnnotBorder {
  // /BS /W, or /Border's third number; 1 when the key that is read has none.
  float width = 1.0f;

  // /BS /S, solid when it is left out or is no name ISO 32000 defines. A
  // /Border is dashed when its dash array holds a pattern. A dashed border
  // whose pattern is an empty array draws a solid line (8.4.3.6) and is
  // solid here. Never kUnknown.
  CPDF_Annot::BorderStyle style = CPDF_Annot::BorderStyle::kSolid;

  // The pattern as stored, whatever the style: /BS /D, or /Border's dash
  // array. Null when none is stored; a dashed border then draws ISO's
  // default dash.
  RetainPtr<const CPDF_Array> dash_array;
};

AnnotBorder ReadAnnotBorder(const CPDF_Dictionary* annot_dict);

}  // namespace fpdfdoc

#endif  // CORE_FPDFDOC_CPDF_ANNOT_BORDER_H_
