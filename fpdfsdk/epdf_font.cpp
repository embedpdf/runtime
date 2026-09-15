// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_font.h"

#include <stddef.h>
#include <stdint.h>

#include <optional>

#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxge/cfx_fontregistry.h"
#include "fpdfsdk/cpdfsdk_customaccess.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

FPDF_EXPORT EPDF_FONT_ID FPDF_CALLCONV
EPDFFont_RegisterFont(FPDF_BYTESTRING family_name,
                      int weight,
                      int italic,
                      FPDF_FILEACCESS* file_access) {
  if (!file_access) {
    return CFX_FontRegistry::kInvalidFontId;
  }

  ByteString font_family_name(family_name ? family_name : "");
  return CFX_FontRegistry::RegisterFont(
      font_family_name, weight, italic,
      pdfium::MakeRetain<CPDFSDK_CustomAccess>(file_access));
}

FPDF_EXPORT EPDF_FONT_ID FPDF_CALLCONV
EPDFFont_RegisterMemFont(FPDF_BYTESTRING family_name,
                         int weight,
                         int italic,
                         const void* data_buf,
                         int size) {
  if (size < 0) {
    return CFX_FontRegistry::kInvalidFontId;
  }
  return EPDFFont_RegisterMemFont64(family_name, weight, italic, data_buf,
                                    static_cast<size_t>(size));
}

FPDF_EXPORT EPDF_FONT_ID FPDF_CALLCONV
EPDFFont_RegisterMemFont64(FPDF_BYTESTRING family_name,
                           int weight,
                           int italic,
                           const void* data_buf,
                           size_t size) {
  if (!data_buf || size == 0) {
    return CFX_FontRegistry::kInvalidFontId;
  }

  ByteString font_family_name(family_name ? family_name : "");
  // SAFETY: required from caller.
  auto font_data =
      UNSAFE_BUFFERS(pdfium::span(static_cast<const uint8_t*>(data_buf), size));
  return CFX_FontRegistry::RegisterMemoryFont(font_family_name, weight, italic,
                                              font_data);
}

FPDF_EXPORT void FPDF_CALLCONV EPDFFont_ClearRegisteredFonts(void) {
  CFX_FontRegistry::ClearRegisteredFonts();
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFFont_GetEmbeddingPermission(EPDF_FONT_ID font_id) {
  std::optional<CFX_FontRegistry::EmbeddingPermission> permission =
      CFX_FontRegistry::GetEmbeddingPermission(font_id);
  if (!permission.has_value()) {
    return -1;
  }
  switch (*permission) {
    case CFX_FontRegistry::EmbeddingPermission::kInstallable:
      return EPDF_FONT_EMBEDDING_INSTALLABLE;
    case CFX_FontRegistry::EmbeddingPermission::kEditable:
      return EPDF_FONT_EMBEDDING_EDITABLE;
    case CFX_FontRegistry::EmbeddingPermission::kPreviewAndPrint:
      return EPDF_FONT_EMBEDDING_PREVIEW_AND_PRINT;
    case CFX_FontRegistry::EmbeddingPermission::kRestricted:
      return EPDF_FONT_EMBEDDING_RESTRICTED;
    case CFX_FontRegistry::EmbeddingPermission::kBitmapOnly:
      return EPDF_FONT_EMBEDDING_BITMAP_ONLY;
  }
  return -1;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFont_IsEditingAuthorized(EPDF_FONT_ID font_id) {
  return CFX_FontRegistry::IsEditingAuthorized(font_id);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFont_AuthorizeEditing(EPDF_FONT_ID font_id) {
  return CFX_FontRegistry::AuthorizeEditing(font_id);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFFont_IsInstanced(EPDF_FONT_ID font_id) {
  return CFX_FontRegistry::IsInstanced(font_id);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFont_GetFamilyName(EPDF_FONT_ID font_id,
                       char* buffer,
                       unsigned long buflen) {
  if (!CFX_FontRegistry::IsValidFont(font_id)) {
    return 0;
  }
  const ByteString family = CFX_FontRegistry::GetFamilyName(font_id);
  // SAFETY: same pattern as the other UTF-8 getters.
  return NulTerminateMaybeCopyAndReturnLength(
      family, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT int FPDF_CALLCONV EPDFFont_GetWeight(EPDF_FONT_ID font_id) {
  if (!CFX_FontRegistry::IsValidFont(font_id)) {
    return 0;
  }
  return CFX_FontRegistry::GetStyleWeight(font_id);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFFont_IsItalic(EPDF_FONT_ID font_id) {
  return CFX_FontRegistry::IsValidFont(font_id) &&
         CFX_FontRegistry::IsStyleItalic(font_id);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_SetFontEmbeddingPolicy(FPDF_DOCUMENT document, int policy) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return false;
  }
  switch (policy) {
    case EPDF_FONT_EMBEDDING_POLICY_DEFAULT:
      doc->SetFontEmbeddingPolicy(CPDF_Document::FontEmbeddingPolicy::kDefault);
      return true;
    case EPDF_FONT_EMBEDDING_POLICY_SUBSET:
      doc->SetFontEmbeddingPolicy(CPDF_Document::FontEmbeddingPolicy::kSubset);
      return true;
    case EPDF_FONT_EMBEDDING_POLICY_FULL:
      doc->SetFontEmbeddingPolicy(CPDF_Document::FontEmbeddingPolicy::kFull);
      return true;
    default:
      return false;
  }
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFDoc_GetFontEmbeddingPolicy(FPDF_DOCUMENT document) {
  const CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return -1;
  }
  switch (doc->GetFontEmbeddingPolicy()) {
    case CPDF_Document::FontEmbeddingPolicy::kDefault:
      return EPDF_FONT_EMBEDDING_POLICY_DEFAULT;
    case CPDF_Document::FontEmbeddingPolicy::kSubset:
      return EPDF_FONT_EMBEDDING_POLICY_SUBSET;
    case CPDF_Document::FontEmbeddingPolicy::kFull:
      return EPDF_FONT_EMBEDDING_POLICY_FULL;
  }
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFont_AddFallbackFont(EPDF_FONT_ID font_id) {
  return CFX_FontRegistry::AddFallbackFont(font_id);
}

FPDF_EXPORT void FPDF_CALLCONV EPDFFont_ClearFallbackFonts(void) {
  CFX_FontRegistry::ClearFallbackFonts();
}
