// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef FPDFSDK_EPDF_APPEARANCE_EXPORTER_H_
#define FPDFSDK_EPDF_APPEARANCE_EXPORTER_H_

#include "core/fpdfapi/edit/cpdf_pageorganizer.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/retain_ptr.h"

// Deep-clones appearance Form XObjects from one document into another,
// carrying every resource they reference (fonts, images, nested forms) and
// deduplicating objects shared between streams cloned through the same
// instance. Shared by the page → appearance path (a stamp taking its
// artwork from another document's page) and the appearance → page path
// (flattening a selection into a new document).
class AnnotAppearanceExporter final : public CPDF_PageOrganizer {
 public:
  AnnotAppearanceExporter(CPDF_Document* dest_doc, CPDF_Document* src_doc)
      : CPDF_PageOrganizer(dest_doc, src_doc) {}

  // Changes nothing else in the destination: not its catalog, and not its
  // /Info, which CPDF_PageOrganizer::Init() would stamp with a /Producer.
  RetainPtr<CPDF_Stream> ExportFormXObject(
      RetainPtr<const CPDF_Stream> src_stream) {
    if (!src_stream) {
      return nullptr;
    }

    RetainPtr<CPDF_Object> cloned_object = src_stream->Clone();
    RetainPtr<CPDF_Stream> cloned_stream = ToStream(cloned_object);
    if (!cloned_stream) {
      return nullptr;
    }

    const uint32_t src_obj_num = src_stream->GetObjNum();
    const uint32_t dest_obj_num = dest()->AddIndirectObject(cloned_object);
    if (src_obj_num) {
      AddObjectMapping(src_obj_num, dest_obj_num);
    }

    if (!UpdateReference(cloned_object)) {
      return nullptr;
    }

    return cloned_stream;
  }
};

#endif  // FPDFSDK_EPDF_APPEARANCE_EXPORTER_H_
