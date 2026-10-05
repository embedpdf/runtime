// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// What an embedder needs to decide which parsed pages to keep: what a page's
// parsed content costs, and whether it still matches its document.

#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_parsedsize.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/fpdfview.h"

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_GetParsedSize(FPDF_PAGE page, EPDF_PAGE_PARSED_SIZE* size) {
  const CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!pdf_page || !size) {
    return false;
  }
  const CPDF_ParsedSize& parsed = pdf_page->GetParsedSize();
  size->objects = static_cast<unsigned long>(parsed.objects);
  size->path_points = static_cast<unsigned long>(parsed.path_points);
  size->text_chars = static_cast<unsigned long>(parsed.text_chars);
  size->estimated_bytes = parsed.estimated_bytes;
  size->complete =
      pdf_page->GetParseState() == CPDF_PageObjectHolder::ParseState::kParsed;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFPage_IsContentCurrent(FPDF_PAGE page) {
  const CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  return pdf_page && pdf_page->IsContentCurrent();
}
