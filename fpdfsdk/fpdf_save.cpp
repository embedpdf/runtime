// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "public/fpdf_save.h"

#include <stdint.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "build/build_config.h"
#include "core/fpdfapi/edit/cpdf_creator.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_security_handler.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fxcrt/fx_extension.h"
#include "core/fxcrt/mask.h"
#include "core/fxcrt/stl_util.h"
#include "fpdfsdk/cpdfsdk_filewriteadapter.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/fpdf_edit.h"

#ifdef PDF_ENABLE_XFA
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/cfx_memorystream.h"
#include "fpdfsdk/fpdfxfa/cpdfxfa_context.h"
#include "public/fpdf_formfill.h"
#endif

static_assert(FPDF_INCREMENTAL == CPDF_Creator::CreateFlags::kIncremental);
static_assert(FPDF_NO_INCREMENTAL == CPDF_Creator::CreateFlags::kNoOriginal);
static_assert(FPDF_REMOVE_SECURITY_DEPRECATED ==
              CPDF_Creator::CreateFlags::kRemoveSecurityDeprecated);
static_assert(FPDF_REMOVE_SECURITY ==
              CPDF_Creator::CreateFlags::kRemoveSecurity);
static_assert(FPDF_SUBSET_NEW_FONTS ==
              CPDF_Creator::CreateFlags::kSubsetNewFonts);

namespace {

#ifdef PDF_ENABLE_XFA
bool SaveXFADocumentData(
    CPDFXFA_Context* context,
    std::vector<RetainPtr<IFX_SeekableStream>>* file_list) {
  if (!context) {
    return false;
  }

  if (!context->ContainsExtensionForm()) {
    return true;
  }

  CPDF_Document* doc = context->GetPDFDoc();
  if (!doc) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
  if (!root) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> acro_form = root->GetMutableDictFor("AcroForm");
  if (!acro_form) {
    return false;
  }

  RetainPtr<CPDF_Object> xfa = acro_form->GetMutableObjectFor("XFA");
  if (!xfa) {
    return true;
  }

  CPDF_Array* xfa_array = xfa->AsMutableArray();
  if (!xfa_array) {
    return false;
  }

  int size = fxcrt::CollectionSize<int>(*xfa_array);
  int form_index = -1;
  int datasets_index = -1;
  for (int i = 0; i < size - 1; i++) {
    RetainPtr<const CPDF_Object> xfa_obj = xfa_array->GetObjectAt(i);
    if (!xfa_obj->IsString()) {
      continue;
    }
    if (xfa_obj->GetString() == "form") {
      form_index = i + 1;
    } else if (xfa_obj->GetString() == "datasets") {
      datasets_index = i + 1;
    }
  }

  RetainPtr<CPDF_Stream> form_stream;
  if (form_index != -1) {
    // Get form CPDF_Stream
    RetainPtr<CPDF_Object> form_obj = xfa_array->GetMutableObjectAt(form_index);
    if (form_obj->IsReference()) {
      RetainPtr<CPDF_Object> form_direct_obj = form_obj->GetMutableDirect();
      if (form_direct_obj && form_direct_obj->IsStream()) {
        form_stream.Reset(form_direct_obj->AsMutableStream());
      }
    } else if (form_obj->IsStream()) {
      form_stream.Reset(form_obj->AsMutableStream());
    }
  }

  RetainPtr<CPDF_Stream> datasets_stream;
  if (datasets_index != -1) {
    // Get datasets CPDF_Stream
    RetainPtr<CPDF_Object> datasets_obj =
        xfa_array->GetMutableObjectAt(datasets_index);
    if (datasets_obj->IsReference()) {
      CPDF_Reference* datasets_ref_obj = datasets_obj->AsMutableReference();
      RetainPtr<CPDF_Object> datasets_direct_obj =
          datasets_ref_obj->GetMutableDirect();
      if (datasets_direct_obj && datasets_direct_obj->IsStream()) {
        datasets_stream.Reset(datasets_direct_obj->AsMutableStream());
      }
    } else if (datasets_obj->IsStream()) {
      datasets_stream.Reset(datasets_obj->AsMutableStream());
    }
  }
  // L"datasets"
  {
    RetainPtr<IFX_SeekableStream> file_write =
        pdfium::MakeRetain<CFX_MemoryStream>();
    if (context->SaveDatasetsPackage(file_write) && file_write->GetSize() > 0) {
      if (datasets_index != -1) {
        if (datasets_stream) {
          datasets_stream->InitStreamFromFile(file_write);
        }
      } else {
        auto data_stream = doc->NewIndirect<CPDF_Stream>(
            file_write, doc->New<CPDF_Dictionary>());
        int last_index = fxcrt::CollectionSize<int>(*xfa_array) - 2;
        xfa_array->InsertNewAt<CPDF_String>(last_index, "datasets");
        xfa_array->InsertNewAt<CPDF_Reference>(last_index + 1, doc,
                                               data_stream->GetObjNum());
      }
      file_list->push_back(std::move(file_write));
    }
  }
  // L"form"
  {
    RetainPtr<IFX_SeekableStream> file_write =
        pdfium::MakeRetain<CFX_MemoryStream>();
    if (context->SaveFormPackage(file_write) && file_write->GetSize() > 0) {
      if (form_index != -1) {
        if (form_stream) {
          form_stream->InitStreamFromFile(file_write);
        }
      } else {
        auto data_stream = doc->NewIndirect<CPDF_Stream>(
            file_write, doc->New<CPDF_Dictionary>());
        int last_index = fxcrt::CollectionSize<int>(*xfa_array) - 2;
        xfa_array->InsertNewAt<CPDF_String>(last_index, "form");
        xfa_array->InsertNewAt<CPDF_Reference>(last_index + 1, doc,
                                               data_stream->GetObjNum());
      }
      file_list->push_back(std::move(file_write));
    }
  }
  return true;
}
#endif  // PDF_ENABLE_XFA

void SetSaveStatus(EPDFSaveStatus* out_status, EPDFSaveStatus status) {
  if (out_status) {
    *out_status = status;
  }
}

// |skip_if_unchanged|: an incremental save of a layer writes nothing when no
// reachable object differs from the document it was opened with, and
// |out_status| says so. Without it the save always writes (the cumulative
// delta a persisted artifact needs).
bool DoDocSaveImpl(FPDF_DOCUMENT document,
                   FPDF_FILEWRITE* file_write,
                   FPDF_DWORD flags,
                   std::optional<int> version,
                   bool skip_if_unchanged,
                   EPDFSaveStatus* out_status) {
  SetSaveStatus(out_status, EPDFSaveStatus_kFailed);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return false;
  }

#ifdef PDF_ENABLE_XFA
  auto* context = static_cast<CPDFXFA_Context*>(doc->GetExtension());
  if (context) {
    std::vector<RetainPtr<IFX_SeekableStream>> file_list;
    context->SendPreSaveToXFADoc(&file_list);
    SaveXFADocumentData(context, &file_list);
  }
#endif  // PDF_ENABLE_XFA

  CPDF_Creator file_maker(
      doc, pdfium::MakeRetain<CPDFSDK_FileWriteAdapter>(file_write));

  // Apply document-owned pending security state. It stays on the document so
  // repeated saves use the same requested encryption/removal policy until the
  // caller changes it or closes the document.
  if (const CPDF_Document::PendingSecurity* pending =
          doc->GetPendingSecurity()) {
    if (pending->mode == CPDF_Document::PendingSecurityMode::kRemove) {
      flags |= FPDF_REMOVE_SECURITY;
    } else if (pending->mode == CPDF_Document::PendingSecurityMode::kEncrypt) {
      // SetEncryption sets both:
      // 1. encrypt_dict_ - so /Encrypt reference is written to trailer
      // 2. security_handler_ - so GetCryptoHandler() encrypts streams/strings
      file_maker.SetEncryption(pending->encrypt_dict,
                               pending->security_handler);
    }
  }

  Mask<CPDF_Creator::CreateFlags> create_flags =
      Mask<CPDF_Creator::CreateFlags>::FromUnderlyingUnchecked(
          static_cast<uint32_t>(flags));
  if (skip_if_unchanged) {
    create_flags |= CPDF_Creator::CreateFlags::kSkipIfUnchangedSinceLoad;
  }
  bool create_result = file_maker.Create(create_flags, version.value_or(0));

#ifdef PDF_ENABLE_XFA
  if (context) {
    context->SendPostSaveToXFADoc();
  }
#endif  // PDF_ENABLE_XFA

  if (create_result) {
    SetSaveStatus(out_status, file_maker.IsUnchangedSinceLoad()
                                  ? EPDFSaveStatus_kUnchangedSinceLoad
                                  : EPDFSaveStatus_kWritten);
  }
  return create_result;
}

bool DoDocSave(FPDF_DOCUMENT document,
               FPDF_FILEWRITE* file_write,
               FPDF_DWORD flags,
               std::optional<int> version) {
  return DoDocSaveImpl(document, file_write, flags, version,
                       /*skip_if_unchanged=*/false, /*out_status=*/nullptr);
}

struct MemoryFileWriter : public FPDF_FILEWRITE {
  MemoryFileWriter() {
    version = 1;
    WriteBlock = [](FPDF_FILEWRITE* self, const void* buf,
                    unsigned long size) -> int {
      auto* writer = static_cast<MemoryFileWriter*>(self);
      if (writer->failed_ || !writer->Append(buf, size)) {
        writer->failed_ = true;
        return 0;
      }
      return 1;
    };
  }

  ~MemoryFileWriter() { free(data_); }

  MemoryFileWriter(const MemoryFileWriter&) = delete;
  MemoryFileWriter& operator=(const MemoryFileWriter&) = delete;

  size_t size() const { return size_; }
  bool failed() const { return failed_; }

  // The returned allocation is released by EPDF_FreeBuffer(). No final
  // output-sized copy is needed to transfer ownership to the caller.
  void* Release() { return std::exchange(data_, nullptr); }

 private:
  bool Append(const void* data, size_t size) {
    if (size > std::numeric_limits<size_t>::max() - size_) {
      return false;
    }
    const size_t required = size_ + size;
    if (required > capacity_) {
      const size_t growth = std::min(
          capacity_ / 2, std::numeric_limits<size_t>::max() - capacity_);
      const size_t capacity =
          std::max(required, std::max(size_t{32768}, capacity_ + growth));
      void* allocation = realloc(data_, capacity);
      if (!allocation) {
        return false;
      }
      data_ = static_cast<uint8_t*>(allocation);
      capacity_ = capacity;
    }
    if (size != 0) {
      memcpy(data_ + size_, data, size);
    }
    size_ = required;
    return true;
  }

  uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
  bool failed_ = false;
};

void* SaveToOwnedBuffer(FPDF_DOCUMENT document,
                        FPDF_DWORD flags,
                        unsigned long* out_size,
                        std::optional<int> version,
                        bool skip_if_unchanged = false,
                        EPDFSaveStatus* out_status = nullptr) {
  SetSaveStatus(out_status, EPDFSaveStatus_kFailed);
  if (!out_size) {
    return nullptr;
  }
  *out_size = 0;

  MemoryFileWriter writer;
  EPDFSaveStatus status = EPDFSaveStatus_kFailed;
  const bool ok = DoDocSaveImpl(document, &writer, flags, version,
                                skip_if_unchanged, &status);
  // The archive flushes its final block during destruction in DoDocSaveImpl().
  // Include that callback's result before handing a complete PDF to the caller.
  if (!ok || writer.failed()) {
    return nullptr;
  }
  SetSaveStatus(out_status, status);
  if (status == EPDFSaveStatus_kUnchangedSinceLoad) {
    return nullptr;  // nothing written, by design: the loaded bytes stand
  }
  if (writer.size() == 0 ||
      writer.size() > std::numeric_limits<unsigned long>::max()) {
    SetSaveStatus(out_status, EPDFSaveStatus_kFailed);
    return nullptr;
  }

  *out_size = static_cast<unsigned long>(writer.size());
  return writer.Release();
}

}  // namespace

FPDF_EXPORT void FPDF_CALLCONV EPDF_FreeBuffer(void* buffer) {
  free(buffer);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_SaveAsCopy(FPDF_DOCUMENT document,
                                                    FPDF_FILEWRITE* file_write,
                                                    FPDF_DWORD flags) {
  return DoDocSave(document, file_write, flags, {});
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_SaveWithVersion(FPDF_DOCUMENT document,
                     FPDF_FILEWRITE* file_write,
                     FPDF_DWORD flags,
                     int fileVersion) {
  return DoDocSave(document, file_write, flags, fileVersion);
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDF_SaveDocumentToOwnedBuffer(FPDF_DOCUMENT document,
                               FPDF_DWORD flags,
                               unsigned long* out_size) {
  return SaveToOwnedBuffer(document, flags, out_size, {});
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDF_SaveDocumentToOwnedBufferWithVersion(FPDF_DOCUMENT document,
                                          FPDF_DWORD flags,
                                          unsigned long* out_size,
                                          int file_version) {
  return SaveToOwnedBuffer(document, flags, out_size, file_version);
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDF_SaveDocumentToOwnedBufferEx(FPDF_DOCUMENT document,
                                 FPDF_DWORD flags,
                                 int file_version,
                                 unsigned long* out_size,
                                 EPDFSaveStatus* out_status) {
  std::optional<int> version;
  if (file_version > 0) {
    version = file_version;
  }
  return SaveToOwnedBuffer(document, flags, out_size, version,
                           /*skip_if_unchanged=*/true, out_status);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_SaveAsCopyEx(FPDF_DOCUMENT document,
                  FPDF_FILEWRITE* file_write,
                  FPDF_DWORD flags,
                  EPDFSaveStatus* out_status) {
  return DoDocSaveImpl(document, file_write, flags, {},
                       /*skip_if_unchanged=*/true, out_status);
}
