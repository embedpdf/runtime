// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_font.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_documentfonts.h"
#include "core/fpdfdoc/cpdf_fontface.h"
#include "core/fpdfdoc/cpdf_fontresolver.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/span_util.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxge/cfx_font.h"
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
EPDFFont_AddFallbackFont(EPDF_FONT_ID font_id, int tier) {
  return CFX_FontRegistry::AddFallbackFont(font_id, tier);
}

FPDF_EXPORT void FPDF_CALLCONV EPDFFont_ClearFallbackFonts(void) {
  CFX_FontRegistry::ClearFallbackFonts();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFont_SetLastResortFont(EPDF_FONT_ID font_id) {
  return CFX_FontRegistry::SetLastResortFont(font_id);
}

FPDF_EXPORT EPDF_FONT_ID FPDF_CALLCONV EPDFFont_GetLastResortFont(void) {
  return CFX_FontRegistry::GetLastResortFont();
}

// ---- Registered fonts: programs, coverage, fallback order -------------------

namespace {

// Copies |bytes| into the caller's buffer when it is big enough; returns how
// many bytes there are either way.
unsigned long CopyBytesAndReturnLength(pdfium::span<const uint8_t> bytes,
                                       void* buffer,
                                       unsigned long buflen) {
  // SAFETY: required from caller.
  pdfium::span<char> destination =
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen));
  fxcrt::try_spancpy(pdfium::as_writable_bytes(destination), bytes);
  return pdfium::checked_cast<unsigned long>(bytes.size());
}

// Writes (first, count) pairs into the caller's array, up to |capacity|
// pairs; returns how many ranges there are either way.
unsigned long CopyRangesAndReturnCount(
    pdfium::span<const CFX_CodePointRange> ranges,
    uint32_t* out,
    unsigned long capacity) {
  if (out) {
    const size_t count = std::min<size_t>(ranges.size(), capacity);
    // SAFETY: required from caller: |out| holds 2 * |capacity| values.
    pdfium::span<uint32_t> destination =
        UNSAFE_BUFFERS(pdfium::span(out, count * 2));
    for (size_t i = 0; i < count; ++i) {
      destination[2 * i] = ranges[i].first;
      destination[2 * i + 1] = ranges[i].count;
    }
  }
  return pdfium::checked_cast<unsigned long>(ranges.size());
}

unsigned long CopyFontIdsAndReturnCount(
    const std::vector<CFX_FontRegistry::FontId>& ids,
    EPDF_FONT_ID* out,
    unsigned long capacity) {
  if (out) {
    const size_t count = std::min<size_t>(ids.size(), capacity);
    // SAFETY: required from caller.
    pdfium::span<EPDF_FONT_ID> destination =
        UNSAFE_BUFFERS(pdfium::span(out, count));
    for (size_t i = 0; i < count; ++i) {
      destination[i] = ids[i];
    }
  }
  return pdfium::checked_cast<unsigned long>(ids.size());
}

bool CopySha256(pdfium::span<const uint8_t> hash, unsigned char* out) {
  if (!out || hash.size() != 32) {
    return false;
  }
  // SAFETY: required from caller: |out| holds 32 bytes.
  fxcrt::spancpy(UNSAFE_BUFFERS(pdfium::span(out, 32u)), hash);
  return true;
}

}  // namespace

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFont_GetFallbackOrder(int weight,
                          FPDF_BOOL italic,
                          EPDF_FONT_ID* ids,
                          unsigned long capacity) {
  return CopyFontIdsAndReturnCount(
      CFX_FontRegistry::GetFallbackOrder(weight, !!italic,
                                         /*for_authoring=*/true),
      ids, capacity);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFont_GetProgramData(EPDF_FONT_ID font_id,
                        void* buffer,
                        unsigned long buflen) {
  if (!CFX_FontRegistry::IsValidFont(font_id)) {
    return 0;
  }
  const DataVector<uint8_t> data = CFX_FontRegistry::GetProgramData(font_id);
  return CopyBytesAndReturnLength(data, buffer, buflen);
}

FPDF_EXPORT int FPDF_CALLCONV EPDFFont_GetProgramFormat(EPDF_FONT_ID font_id) {
  switch (CFX_FontRegistry::GetProgramFormat(font_id)) {
    case CFX_FontRegistry::ProgramFormat::kTrueType:
      return EPDF_FONT_FORMAT_TRUETYPE;
    case CFX_FontRegistry::ProgramFormat::kOpenTypeCFF:
      return EPDF_FONT_FORMAT_OPENTYPE_CFF;
    case CFX_FontRegistry::ProgramFormat::kUnsupported:
      return -1;
  }
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFont_GetProgramHash(EPDF_FONT_ID font_id, unsigned char* sha256) {
  return CopySha256(CFX_FontRegistry::GetSourceHash(font_id), sha256);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFont_GetCoverage(EPDF_FONT_ID font_id,
                     uint32_t* ranges,
                     unsigned long range_capacity) {
  return CopyRangesAndReturnCount(CFX_FontRegistry::GetCoverage(font_id),
                                  ranges, range_capacity);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFFont_GetMetrics(EPDF_FONT_ID font_id,
                                                        float* ascent,
                                                        float* descent) {
  if (!ascent || !descent) {
    return false;
  }
  std::unique_ptr<CFX_Font> program = CFX_FontRegistry::CreateFont(font_id);
  if (!program) {
    return false;
  }
  ProgramLineMetrics(program.get(), ascent, descent);
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFont_GetPostScriptName(EPDF_FONT_ID font_id,
                           char* buffer,
                           unsigned long buflen) {
  if (!CFX_FontRegistry::IsValidFont(font_id)) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      BaseFontNameWithoutSubsetTag(CFX_FontRegistry::GetBaseFontName(font_id)),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

// ---- A document's fonts
// ------------------------------------------------------

struct epdf_font_faces_t__ {
  std::vector<CPDF_DocumentFonts::Face> faces;
};

namespace {

const CPDF_DocumentFonts::Face* FaceAt(EPDF_FONT_FACES faces, int face) {
  if (!faces || !fxcrt::IndexInBounds(faces->faces, face)) {
    return nullptr;
  }
  return &faces->faces[face];
}

const CPDF_DocumentFonts::Program* ProgramAt(EPDF_FONT_FACES faces,
                                             int face,
                                             int program) {
  const CPDF_DocumentFonts::Face* entry = FaceAt(faces, face);
  if (!entry || !fxcrt::IndexInBounds(entry->programs, program)) {
    return nullptr;
  }
  return &entry->programs[program];
}

int FormatCode(CPDF_DocumentFonts::Format format) {
  switch (format) {
    case CPDF_DocumentFonts::Format::kNone:
      return EPDF_FONT_FORMAT_NONE;
    case CPDF_DocumentFonts::Format::kTrueType:
      return EPDF_FONT_FORMAT_TRUETYPE;
    case CPDF_DocumentFonts::Format::kOpenTypeCFF:
      return EPDF_FONT_FORMAT_OPENTYPE_CFF;
    case CPDF_DocumentFonts::Format::kCFF:
      return EPDF_FONT_FORMAT_CFF;
    case CPDF_DocumentFonts::Format::kType1:
      return EPDF_FONT_FORMAT_TYPE1;
    case CPDF_DocumentFonts::Format::kType3:
      return EPDF_FONT_FORMAT_TYPE3;
  }
}

int EmbeddedCode(CPDF_DocumentFonts::Embedded embedded) {
  switch (embedded) {
    case CPDF_DocumentFonts::Embedded::kNone:
      return EPDF_FONT_EMBEDDED_NONE;
    case CPDF_DocumentFonts::Embedded::kWhole:
      return EPDF_FONT_EMBEDDED_WHOLE;
    case CPDF_DocumentFonts::Embedded::kSubset:
      return EPDF_FONT_EMBEDDED_SUBSET;
  }
}

}  // namespace

FPDF_EXPORT EPDF_FONT_FACES FPDF_CALLCONV
EPDFFontFaces_Load(FPDF_DOCUMENT document) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return nullptr;
  }
  auto faces = std::make_unique<epdf_font_faces_t__>();
  faces->faces = CPDF_DocumentFonts::For(doc)->CollectFaces();
  return faces.release();
}

FPDF_EXPORT void FPDF_CALLCONV EPDFFontFaces_Close(EPDF_FONT_FACES faces) {
  delete faces;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFFontFaces_Count(EPDF_FONT_FACES faces) {
  return faces ? fxcrt::CollectionSize<int>(faces->faces) : -1;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontFaces_GetFamily(EPDF_FONT_FACES faces,
                        int face,
                        FPDF_WCHAR* buffer,
                        unsigned long buflen) {
  const CPDF_DocumentFonts::Face* entry = FaceAt(faces, face);
  if (!entry) {
    return 0;
  }
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      entry->family, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT int FPDF_CALLCONV EPDFFontFaces_GetWeight(EPDF_FONT_FACES faces,
                                                      int face) {
  const CPDF_DocumentFonts::Face* entry = FaceAt(faces, face);
  return entry ? entry->weight : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontFaces_IsItalic(EPDF_FONT_FACES faces, int face) {
  const CPDF_DocumentFonts::Face* entry = FaceAt(faces, face);
  return entry && entry->italic;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFFontFaces_CountPrograms(EPDF_FONT_FACES faces,
                                                          int face) {
  const CPDF_DocumentFonts::Face* entry = FaceAt(faces, face);
  return entry ? fxcrt::CollectionSize<int>(entry->programs) : -1;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontFaces_GetProgramObjNum(EPDF_FONT_FACES faces, int face, int program) {
  const CPDF_DocumentFonts::Program* entry = ProgramAt(faces, face, program);
  return entry ? entry->objnum : 0;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFFontFaces_GetProgramFormat(EPDF_FONT_FACES faces, int face, int program) {
  const CPDF_DocumentFonts::Program* entry = ProgramAt(faces, face, program);
  return entry ? FormatCode(entry->format) : -1;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFFontFaces_GetProgramEmbedded(EPDF_FONT_FACES faces, int face, int program) {
  const CPDF_DocumentFonts::Program* entry = ProgramAt(faces, face, program);
  return entry ? EmbeddedCode(entry->embedded) : -1;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontFaces_GetProgramPostScriptName(EPDF_FONT_FACES faces,
                                       int face,
                                       int program,
                                       char* buffer,
                                       unsigned long buflen) {
  const CPDF_DocumentFonts::Program* entry = ProgramAt(faces, face, program);
  if (!entry) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      entry->postscript_name,
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFFontFaces_GetProgramEmbeddingPermission(EPDF_FONT_FACES faces,
                                            int face,
                                            int program) {
  const CPDF_DocumentFonts::Program* entry = ProgramAt(faces, face, program);
  if (!entry || !entry->permission.has_value()) {
    return -1;
  }
  return static_cast<int>(*entry->permission);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontFaces_ProgramMapsUnicode(EPDF_FONT_FACES faces, int face, int program) {
  const CPDF_DocumentFonts::Program* entry = ProgramAt(faces, face, program);
  return entry && entry->maps_unicode;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontFaces_GetProgramHash(EPDF_FONT_FACES faces,
                             int face,
                             int program,
                             unsigned char* sha256) {
  const CPDF_DocumentFonts::Program* entry = ProgramAt(faces, face, program);
  return entry && CopySha256(entry->sha256, sha256);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontFaces_GetProgramDisplayHash(EPDF_FONT_FACES faces,
                                    int face,
                                    int program,
                                    unsigned char* sha256) {
  const CPDF_DocumentFonts::Program* entry = ProgramAt(faces, face, program);
  return entry && CopySha256(entry->display_sha256, sha256);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontFaces_GetProgramMetrics(EPDF_FONT_FACES faces,
                                int face,
                                int program,
                                float* ascent,
                                float* descent) {
  const CPDF_DocumentFonts::Program* entry = ProgramAt(faces, face, program);
  if (!entry || !ascent || !descent || entry->ascent <= 0) {
    return false;
  }
  *ascent = entry->ascent;
  *descent = entry->descent;
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetFontProgramData(FPDF_DOCUMENT document,
                           unsigned long program_objnum,
                           void* buffer,
                           unsigned long buflen) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || program_objnum == 0) {
    return 0;
  }
  const DataVector<uint8_t> data = CPDF_DocumentFonts::For(doc)->ProgramData(
      static_cast<uint32_t>(program_objnum));
  return CopyBytesAndReturnLength(data, buffer, buflen);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetFontDisplayProgramData(FPDF_DOCUMENT document,
                                  unsigned long program_objnum,
                                  void* buffer,
                                  unsigned long buflen) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || program_objnum == 0) {
    return 0;
  }
  const DataVector<uint8_t> data =
      CPDF_DocumentFonts::For(doc)->DisplayProgramData(
          static_cast<uint32_t>(program_objnum));
  return CopyBytesAndReturnLength(data, buffer, buflen);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetFontProgramCoverage(FPDF_DOCUMENT document,
                               unsigned long program_objnum,
                               uint32_t* ranges,
                               unsigned long range_capacity) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || program_objnum == 0) {
    return 0;
  }
  return CopyRangesAndReturnCount(CPDF_DocumentFonts::For(doc)->ProgramCoverage(
                                      static_cast<uint32_t>(program_objnum)),
                                  ranges, range_capacity);
}

// ---- The fonts a write would use
// ---------------------------------------------

struct epdf_font_resolution_t__ {
  CPDF_FontResolver::Result result;
};

namespace {

const CPDF_FontResolver::ResolvedRun* RunAt(EPDF_FONT_RESOLUTION resolution,
                                            int run) {
  if (!resolution || !fxcrt::IndexInBounds(resolution->result.runs, run)) {
    return nullptr;
  }
  return &resolution->result.runs[run];
}

}  // namespace

FPDF_EXPORT EPDF_FONT_RESOLUTION FPDF_CALLCONV
EPDFDoc_ResolveFonts(FPDF_DOCUMENT document,
                     FPDF_ANNOTATION annot,
                     int run_count,
                     const FPDF_WIDESTRING* texts,
                     const FPDF_WIDESTRING* families,
                     const int* weights,
                     const FPDF_BOOL* italics) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || run_count < 0 ||
      (run_count > 0 && (!texts || !families || !weights || !italics))) {
    return nullptr;
  }
  const CPDF_Dictionary* annot_dict = nullptr;
  if (annot) {
    CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
    annot_dict = context ? context->GetAnnotDict() : nullptr;
  }
  const size_t count = static_cast<size_t>(run_count);
  std::vector<CPDF_FontResolver::Run> runs;
  runs.reserve(count);
  // SAFETY: required from caller: each array holds |run_count| entries.
  auto text_span = UNSAFE_BUFFERS(pdfium::span(texts, count));
  auto family_span = UNSAFE_BUFFERS(pdfium::span(families, count));
  auto weight_span = UNSAFE_BUFFERS(pdfium::span(weights, count));
  auto italic_span = UNSAFE_BUFFERS(pdfium::span(italics, count));
  for (size_t i = 0; i < count; ++i) {
    CPDF_FontResolver::Run run;
    run.text = text_span[i] ? WideStringFromFPDFWideString(text_span[i])
                            : WideString();
    run.family = family_span[i] ? WideStringFromFPDFWideString(family_span[i])
                                : WideString();
    run.weight = weight_span[i];
    run.italic = !!italic_span[i];
    runs.push_back(std::move(run));
  }
  auto resolution = std::make_unique<epdf_font_resolution_t__>();
  resolution->result = CPDF_FontResolver::Resolve(doc, annot_dict, runs);
  return resolution.release();
}

FPDF_EXPORT void FPDF_CALLCONV
EPDFFontResolution_Close(EPDF_FONT_RESOLUTION resolution) {
  delete resolution;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFFontResolution_GetSource(EPDF_FONT_RESOLUTION resolution, int run) {
  const CPDF_FontResolver::ResolvedRun* entry = RunAt(resolution, run);
  if (!entry) {
    return -1;
  }
  switch (entry->face.source) {
    case CPDF_AnnotFontMap::Source::kStandard14:
      return EPDF_FONT_SOURCE_STANDARD;
    case CPDF_AnnotFontMap::Source::kRegistered:
      return EPDF_FONT_SOURCE_REGISTERED;
    case CPDF_AnnotFontMap::Source::kDocumentProgram:
      return EPDF_FONT_SOURCE_DOCUMENT_PROGRAM;
    case CPDF_AnnotFontMap::Source::kDocumentFontDict:
      return EPDF_FONT_SOURCE_DOCUMENT_FONT;
  }
}

FPDF_EXPORT EPDF_FONT_ID FPDF_CALLCONV
EPDFFontResolution_GetRegisteredFont(EPDF_FONT_RESOLUTION resolution, int run) {
  const CPDF_FontResolver::ResolvedRun* entry = RunAt(resolution, run);
  return entry ? entry->face.registered_font_id
               : CFX_FontRegistry::kInvalidFontId;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontResolution_GetProgramObjNum(EPDF_FONT_RESOLUTION resolution, int run) {
  const CPDF_FontResolver::ResolvedRun* entry = RunAt(resolution, run);
  return entry ? entry->face.program_objnum : 0;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontResolution_GetBaseFontName(EPDF_FONT_RESOLUTION resolution,
                                   int run,
                                   char* buffer,
                                   unsigned long buflen) {
  const CPDF_FontResolver::ResolvedRun* entry = RunAt(resolution, run);
  if (!entry || entry->face.base_font_name.IsEmpty()) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      entry->face.base_font_name,
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontResolution_GetFamily(EPDF_FONT_RESOLUTION resolution,
                             int run,
                             FPDF_WCHAR* buffer,
                             unsigned long buflen) {
  const CPDF_FontResolver::ResolvedRun* entry = RunAt(resolution, run);
  if (!entry) {
    return 0;
  }
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      entry->face.family, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFFontResolution_GetWeight(EPDF_FONT_RESOLUTION resolution, int run) {
  const CPDF_FontResolver::ResolvedRun* entry = RunAt(resolution, run);
  return entry ? entry->face.weight : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontResolution_IsItalic(EPDF_FONT_RESOLUTION resolution, int run) {
  const CPDF_FontResolver::ResolvedRun* entry = RunAt(resolution, run);
  return entry && entry->face.italic;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontResolution_IsDegraded(EPDF_FONT_RESOLUTION resolution, int run) {
  const CPDF_FontResolver::ResolvedRun* entry = RunAt(resolution, run);
  return entry && entry->face.degraded;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontResolution_GetFallbacks(EPDF_FONT_RESOLUTION resolution,
                                int run,
                                EPDF_FONT_ID* ids,
                                unsigned long capacity) {
  const CPDF_FontResolver::ResolvedRun* entry = RunAt(resolution, run);
  if (!entry) {
    return 0;
  }
  return CopyFontIdsAndReturnCount(entry->fallbacks, ids, capacity);
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFFontResolution_CountMissingCharacters(EPDF_FONT_RESOLUTION resolution) {
  return resolution
             ? fxcrt::CollectionSize<int>(resolution->result.missing_characters)
             : -1;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontResolution_GetMissingCharacter(EPDF_FONT_RESOLUTION resolution,
                                       int index,
                                       FPDF_WCHAR* buffer,
                                       unsigned long buflen) {
  if (!resolution ||
      !fxcrt::IndexInBounds(resolution->result.missing_characters, index)) {
    return 0;
  }
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      resolution->result.missing_characters[index],
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

// ---- Characters a write drew with no font
// -------------------------------------

FPDF_EXPORT void FPDF_CALLCONV
EPDFDoc_ClearMissingCharacters(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (doc) {
    CPDF_DocumentFonts::For(doc)->ClearMissingCharacters();
  }
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFDoc_CountMissingCharacters(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return -1;
  }
  return fxcrt::CollectionSize<int>(
      CPDF_DocumentFonts::For(doc)->missing_characters());
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetMissingCharacter(FPDF_DOCUMENT document,
                            int index,
                            FPDF_WCHAR* buffer,
                            unsigned long buflen) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return 0;
  }
  const std::vector<WideString>& missing =
      CPDF_DocumentFonts::For(doc)->missing_characters();
  if (!fxcrt::IndexInBounds(missing, index)) {
    return 0;
  }
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      missing[index], UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}
