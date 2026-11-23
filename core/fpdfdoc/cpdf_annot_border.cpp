// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_annot_border.h"

#include "constants/annotation_common.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"

namespace fpdfdoc {

AnnotBorder ReadAnnotBorder(const CPDF_Dictionary* annot_dict) {
  AnnotBorder border;

  RetainPtr<const CPDF_Dictionary> border_style = annot_dict->GetDictFor("BS");
  if (border_style) {
    if (border_style->KeyExist("W")) {
      border.width = border_style->GetFloatFor("W");
    }
    const CPDF_Annot::BorderStyle style =
        CPDF_Annot::StringToBorderStyle(border_style->GetByteStringFor("S"));
    if (style != CPDF_Annot::BorderStyle::kUnknown) {
      border.style = style;
    }
    border.dash_array = border_style->GetArrayFor("D");
    if (border.style == CPDF_Annot::BorderStyle::kDashed && border.dash_array &&
        border.dash_array->IsEmpty()) {
      border.style = CPDF_Annot::BorderStyle::kSolid;
    }
    return border;
  }

  RetainPtr<const CPDF_Array> border_array =
      annot_dict->GetArrayFor(pdfium::annotation::kBorder);
  if (!border_array) {
    return border;
  }
  if (border_array->size() > 2) {
    border.width = border_array->GetFloatAt(2);
  }
  // The dash array is the fourth and last entry.
  if (border_array->size() == 4) {
    border.dash_array = border_array->GetArrayAt(3);
    if (border.dash_array && !border.dash_array->IsEmpty()) {
      border.style = CPDF_Annot::BorderStyle::kDashed;
    }
  }
  return border;
}

}  // namespace fpdfdoc
