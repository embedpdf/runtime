// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_edit.h"

#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPageObj_GetFillAlpha(FPDF_PAGEOBJECT page_object, float* alpha) {
  const auto* object = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!object || !alpha) {
    return false;
  }
  *alpha = object->general_state().GetFillAlpha();
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPageObj_GetStrokeAlpha(FPDF_PAGEOBJECT page_object, float* alpha) {
  const auto* object = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!object || !alpha) {
    return false;
  }
  *alpha = object->general_state().GetStrokeAlpha();
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFPageObj_GetBlendMode(FPDF_PAGEOBJECT page_object,
                         char* buffer,
                         unsigned long buflen) {
  const auto* object = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!object) {
    return 0;
  }
  if (!buffer) {
    buflen = 0;
  }
  // SAFETY: the caller provides a writable buffer of buflen bytes.
  return NulTerminateMaybeCopyAndReturnLength(
      object->general_state().GetBlendMode(),
      UNSAFE_BUFFERS(pdfium::span(buffer, static_cast<size_t>(buflen))));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPageObj_HasSoftMask(FPDF_PAGEOBJECT page_object) {
  const auto* object = CPDFPageObjectFromFPDFPageObject(page_object);
  return object && object->general_state().GetSoftMask();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFormObj_GetTransparencyGroup(FPDF_PAGEOBJECT form_object,
                                 FPDF_BOOL* is_group,
                                 FPDF_BOOL* is_isolated) {
  const auto* object = CPDFPageObjectFromFPDFPageObject(form_object);
  if (!object || !object->IsForm() || !is_group || !is_isolated) {
    return false;
  }
  const auto& transparency = object->AsForm()->form()->GetTransparency();
  *is_group = transparency.IsGroup();
  *is_isolated = transparency.IsGroup() && transparency.IsIsolated();
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPageObj_GetMiterLimit(FPDF_PAGEOBJECT page_object, float* miter_limit) {
  const auto* object = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!object || !miter_limit) {
    return false;
  }
  *miter_limit = object->graph_state().GetMiterLimit();
  return true;
}
