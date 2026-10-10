// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef TESTING_EMBEDPDF_FORM_WRITES_H_
#define TESTING_EMBEDPDF_FORM_WRITES_H_

#include <stdint.h>

#include <string>

#include "public/epdf_form.h"
#include "public/fpdfview.h"
#include "testing/fx_string_testhelpers.h"

namespace embedpdf_test {

// Fill in a text field as the engine does: its /V, then its widgets' new
// pictures.
inline bool FillTextField(FPDF_DOCUMENT document,
                          uint32_t field_objnum,
                          const std::wstring& text) {
  ScopedFPDFWideString value = GetFPDFWideString(text);
  FPDF_WIDESTRING values[] = {value.get()};
  return EPDFForm_SetFieldValue(document, field_objnum, EPDF_FORM_VALUE_SCALAR,
                                values, 1) &&
         EPDFForm_RedrawFieldWidgets(document, field_objnum, nullptr, 0,
                                     nullptr);
}

// Choose one option of a combo box or list box as the engine does: its
// export value as /V, its position in /Opt as /I, then the widgets' new
// pictures.
inline bool ChooseOption(FPDF_DOCUMENT document,
                         uint32_t field_objnum,
                         const std::wstring& export_value,
                         int option_index) {
  ScopedFPDFWideString value = GetFPDFWideString(export_value);
  FPDF_WIDESTRING values[] = {value.get()};
  const int indices[] = {option_index};
  return EPDFForm_SetFieldValue(document, field_objnum, EPDF_FORM_VALUE_SCALAR,
                                values, 1) &&
         EPDFForm_SetFieldSelectedIndices(document, field_objnum, indices, 1) &&
         EPDFForm_RedrawFieldWidgets(document, field_objnum, nullptr, 0,
                                     nullptr);
}

}  // namespace embedpdf_test

#endif  // TESTING_EMBEDPDF_FORM_WRITES_H_
