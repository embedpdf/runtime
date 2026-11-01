// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

#include <utility>

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/epdf_catalog.h"

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFCatalog_GetMetadata(FPDF_DOCUMENT document,
                        void* buffer,
                        unsigned long length) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return 0;
  }
  // References a base document's objects hold resolve through the layer.
  CPDF_DocumentViewScope document_view(doc);
  const CPDF_Dictionary* catalog = doc->GetRoot();
  RetainPtr<const CPDF_Stream> stream =
      catalog ? catalog->GetStreamFor("Metadata") : nullptr;
  if (!stream) {
    return 0;
  }
  if (!buffer) {
    length = 0;
  }
  // SAFETY: required from caller.
  return DecodeStreamMaybeCopyAndReturnLength(
      std::move(stream),
      UNSAFE_BUFFERS(pdfium::span(static_cast<uint8_t*>(buffer),
                                  static_cast<size_t>(length))));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFCatalog_SetMetadata(FPDF_DOCUMENT document,
                        const void* data,
                        unsigned long size) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || (!data && size > 0)) {
    return false;
  }
  CPDF_DocumentViewScope document_view(doc);
  const CPDF_Dictionary* catalog = doc->GetRoot();
  if (!catalog) {
    return false;
  }
  if (!data) {
    if (!catalog->KeyExist("Metadata")) {
      return true;
    }
    RetainPtr<CPDF_Dictionary> writable = doc->GetMutableRoot();
    if (!writable) {
      return false;
    }
    writable->RemoveFor("Metadata");
    return true;
  }

  // The stream is written through its object number, so on a layer the
  // write lands in the layer's copy and never in the base's.
  RetainPtr<const CPDF_Reference> reference =
      ToReference(catalog->GetObjectFor("Metadata"));
  RetainPtr<CPDF_Stream> stream =
      reference
          ? ToStream(doc->GetMutableIndirectObject(reference->GetRefObjNum()))
          : nullptr;
  if (!stream) {
    RetainPtr<CPDF_Dictionary> writable = doc->GetMutableRoot();
    if (!writable) {
      return false;
    }
    stream = doc->NewIndirect<CPDF_Stream>(
        pdfium::MakeRetain<CPDF_Dictionary>());
    writable->SetNewFor<CPDF_Reference>("Metadata", doc, stream->GetObjNum());
  }
  // SAFETY: required from caller.
  stream->SetDataAndRemoveFilter(UNSAFE_BUFFERS(
      pdfium::span(static_cast<const uint8_t*>(data),
                   static_cast<size_t>(size))));
  RetainPtr<CPDF_Dictionary> dict = stream->GetMutableDict();
  dict->SetNewFor<CPDF_Name>("Type", "Metadata");
  dict->SetNewFor<CPDF_Name>("Subtype", "XML");
  return true;
}
