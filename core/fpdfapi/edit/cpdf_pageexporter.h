// Copyright 2024 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_EDIT_CPDF_PAGEEXPORTER_H_
#define CORE_FPDFAPI_EDIT_CPDF_PAGEEXPORTER_H_

#include <stdint.h>

#include "core/fpdfapi/edit/cpdf_pageorganizer.h"
#include "core/fxcrt/span.h"

class CPDF_Document;

// Copies pages from a source document into a destination document.
// This class is intended to be used once via ExportPages() and then destroyed.
class CPDF_PageExporter final : public CPDF_PageOrganizer {
 public:
  CPDF_PageExporter(CPDF_Document* dest_doc, CPDF_Document* src_doc);
  ~CPDF_PageExporter();

  // For the pages from the source document with `page_indices` as their page
  // indices, insert them into the destination document at page `index`.
  // `page_indices` and `index` are 0-based.
  //
  // EmbedPDF(chosen-object-numbers): `page_objnums`, when not empty, has one
  // number per page: the copy of `page_indices[i]` is written at
  // `page_objnums[i]` (see CPDF_Document::CreateNewPageAt()), or at the next
  // free number when it is 0.
  bool ExportPages(pdfium::span<const uint32_t> page_indices,
                   int index,
                   pdfium::span<const uint32_t> page_objnums = {});
};

#endif  // CORE_FPDFAPI_EDIT_CPDF_PAGEEXPORTER_H_
