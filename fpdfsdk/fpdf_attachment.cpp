// Copyright 2017 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "public/fpdf_attachment.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "constants/stream_dict_common.h"
#include "core/fdrm/fx_crypt.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfdoc/cpdf_filespec.h"
#include "core/fpdfdoc/cpdf_nametree.h"
#include "core/fxcodec/data_and_bytes_consumed.h"
#include "core/fxcodec/flate/flatemodule.h"
#include "core/fxcrt/cfx_datetime.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_extension.h"
#include "core/fxcrt/notreached.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

constexpr char kChecksumKey[] = "CheckSum";

// How EPDFAttachment_ExtractFile* decodes a given embedded file stream.
enum class ExtractFilterPath {
  kUnfiltered,   // No filters — the raw stream bytes ARE the file.
  kSingleFlate,  // Exactly one predictor-less FlateDecode — streamable.
  kGeneric,      // Anything else — decode fully in memory (stock behavior).
};

ExtractFilterPath ClassifyExtractFilters(const CPDF_Stream* stream) {
  std::optional<DecoderArray> decoders = GetDecoderArray(stream->GetDict());
  if (!decoders.has_value()) {
    return ExtractFilterPath::kGeneric;
  }
  if (decoders->empty()) {
    return ExtractFilterPath::kUnfiltered;
  }
  if (decoders->size() != 1) {
    return ExtractFilterPath::kGeneric;
  }
  const ByteString& name = (*decoders)[0].first;
  if (name != "FlateDecode" && name != "Fl") {
    return ExtractFilterPath::kGeneric;
  }
  RetainPtr<const CPDF_Dictionary> param = ToDictionary((*decoders)[0].second);
  if (param && param->GetIntegerFor("Predictor", 1) > 1) {
    return ExtractFilterPath::kGeneric;
  }
  return ExtractFilterPath::kSingleFlate;
}

struct ExtractOutcome {
  EPDFAttachmentExtractStatus status;
  uint64_t size;
};

using ExtractSink = std::function<bool(pdfium::span<const uint8_t>)>;

// Shared core of the EPDFAttachment_ExtractFile* APIs: locates the embedded
// file stream and pushes its decoded bytes into |sink|. Termination and
// malformed-filter behavior deliberately match FPDFAttachment_GetFile(),
// which this replaces on the read path.
ExtractOutcome ExtractAttachmentFileToSink(FPDF_ATTACHMENT attachment,
                                           uint64_t max_decoded_bytes,
                                           const ExtractSink& sink) {
  CPDF_Object* file = CPDFObjectFromFPDFAttachment(attachment);
  if (!file) {
    return {EPDFAttachmentExtractStatus_kNoFileStream, 0};
  }

  CPDF_FileSpec spec(pdfium::WrapRetain(file));
  RetainPtr<const CPDF_Stream> file_stream = spec.GetFileStream();
  if (!file_stream) {
    return {EPDFAttachmentExtractStatus_kNoFileStream, 0};
  }

  const ExtractFilterPath path = ClassifyExtractFilters(file_stream.Get());
  auto stream_acc = pdfium::MakeRetain<CPDF_StreamAcc>(std::move(file_stream));
  if (path == ExtractFilterPath::kSingleFlate) {
    stream_acc->LoadAllDataRaw();
    uint64_t total = 0;
    switch (FlateModule::FlateDecodeToSink(stream_acc->GetSpan(),
                                           max_decoded_bytes, sink, &total)) {
      case FlateModule::SinkDecodeStatus::kSuccess:
        return {EPDFAttachmentExtractStatus_kSuccess, total};
      case FlateModule::SinkDecodeStatus::kLimitExceeded:
        return {EPDFAttachmentExtractStatus_kSizeLimitExceeded, total};
      case FlateModule::SinkDecodeStatus::kSinkError:
        return {EPDFAttachmentExtractStatus_kWriteFailed, total};
    }
    NOTREACHED();
  }

  if (path == ExtractFilterPath::kUnfiltered) {
    stream_acc->LoadAllDataRaw();
  } else {
    stream_acc->LoadAllDataFiltered();
  }
  pdfium::span<const uint8_t> data = stream_acc->GetSpan();
  if (max_decoded_bytes && data.size() > max_decoded_bytes) {
    return {EPDFAttachmentExtractStatus_kSizeLimitExceeded, 0};
  }
  if (!data.empty() && !sink(data)) {
    return {EPDFAttachmentExtractStatus_kWriteFailed, 0};
  }
  return {EPDFAttachmentExtractStatus_kSuccess, data.size()};
}

// Sizes these APIs can report are capped by the uint32_t |out_size|.
ExtractOutcome CapOutcomeToUint32(ExtractOutcome outcome) {
  if (outcome.status == EPDFAttachmentExtractStatus_kSuccess &&
      outcome.size > std::numeric_limits<uint32_t>::max()) {
    outcome.status = EPDFAttachmentExtractStatus_kSizeLimitExceeded;
  }
  return outcome;
}

}  // namespace

FPDF_EXPORT int FPDF_CALLCONV
FPDFDoc_GetAttachmentCount(FPDF_DOCUMENT document) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return 0;
  }

  auto name_tree = CPDF_NameTree::CreateForReading(doc, "EmbeddedFiles");
  return name_tree ? pdfium::checked_cast<int>(name_tree->GetCount()) : 0;
}

FPDF_EXPORT FPDF_ATTACHMENT FPDF_CALLCONV
FPDFDoc_AddAttachment(FPDF_DOCUMENT document, FPDF_WIDESTRING name) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return nullptr;
  }

  // SAFETY: required from caller.
  WideString wsName = UNSAFE_BUFFERS(WideStringFromFPDFWideString(name));
  if (wsName.IsEmpty()) {
    return nullptr;
  }

  auto name_tree = CPDF_NameTree::CreateWithRootNameArray(doc, "EmbeddedFiles");
  if (!name_tree) {
    return nullptr;
  }

  // Set up the basic entries in the filespec dictionary.
  auto pFile = doc->NewIndirect<CPDF_Dictionary>();
  pFile->SetNewFor<CPDF_Name>("Type", "Filespec");
  pFile->SetNewFor<CPDF_String>("UF", wsName.AsStringView());
  pFile->SetNewFor<CPDF_String>(pdfium::stream::kF, wsName.AsStringView());

  // Add the new attachment name and filespec into the document's EmbeddedFiles.
  if (!name_tree->AddValueAndName(pFile->MakeReference(doc), wsName)) {
    return nullptr;
  }

  // Unretained reference in public API. NOLINTNEXTLINE
  return FPDFAttachmentFromCPDFObject(pFile);
}

FPDF_EXPORT FPDF_ATTACHMENT FPDF_CALLCONV
FPDFDoc_GetAttachment(FPDF_DOCUMENT document, int index) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || index < 0) {
    return nullptr;
  }

  auto name_tree = CPDF_NameTree::CreateForReading(doc, "EmbeddedFiles");
  if (!name_tree || static_cast<size_t>(index) >= name_tree->GetCount()) {
    return nullptr;
  }

  WideString csName;

  // Unretained reference in public API. NOLINTNEXTLINE
  return FPDFAttachmentFromCPDFObject(
      name_tree->LookupValueAndName(index, &csName));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetAttachmentKey(FPDF_DOCUMENT document,
                         int index,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || index < 0) {
    return 0;
  }

  auto name_tree = CPDF_NameTree::CreateForReading(doc, "EmbeddedFiles");
  if (!name_tree || static_cast<size_t>(index) >= name_tree->GetCount()) {
    return 0;
  }

  WideString key;
  if (!name_tree->LookupValueAndName(index, &key)) {
    return 0;
  }

  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      key, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFDoc_GetAttachmentIndexByKey(FPDF_DOCUMENT document, FPDF_WIDESTRING key) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || !key) {
    return -1;
  }

  auto name_tree = CPDF_NameTree::CreateForReading(doc, "EmbeddedFiles");
  if (!name_tree) {
    return -1;
  }

  // SAFETY: required from caller.
  WideString target = UNSAFE_BUFFERS(WideStringFromFPDFWideString(key));
  const size_t count = name_tree->GetCount();
  for (size_t i = 0; i < count; ++i) {
    WideString candidate;
    if (name_tree->LookupValueAndName(i, &candidate) && candidate == target) {
      return pdfium::checked_cast<int>(i);
    }
  }
  return -1;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFDoc_DeleteAttachment(FPDF_DOCUMENT document, int index) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || index < 0) {
    return false;
  }

  auto name_tree = CPDF_NameTree::Create(doc, "EmbeddedFiles");
  if (!name_tree || static_cast<size_t>(index) >= name_tree->GetCount()) {
    return false;
  }

  return name_tree->DeleteValueAndName(index);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAttachment_GetName(FPDF_ATTACHMENT attachment,
                       FPDF_WCHAR* buffer,
                       unsigned long buflen) {
  CPDF_Object* pFile = CPDFObjectFromFPDFAttachment(attachment);
  if (!pFile) {
    return 0;
  }

  CPDF_FileSpec spec(pdfium::WrapRetain(pFile));
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      spec.GetFileName(), UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAttachment_HasKey(FPDF_ATTACHMENT attachment, FPDF_BYTESTRING key) {
  CPDF_Object* pFile = CPDFObjectFromFPDFAttachment(attachment);
  if (!pFile) {
    return 0;
  }

  CPDF_FileSpec spec(pdfium::WrapRetain(pFile));
  RetainPtr<const CPDF_Dictionary> pParamsDict = spec.GetParamsDict();
  return pParamsDict ? pParamsDict->KeyExist(key) : 0;
}

FPDF_EXPORT FPDF_OBJECT_TYPE FPDF_CALLCONV
FPDFAttachment_GetValueType(FPDF_ATTACHMENT attachment, FPDF_BYTESTRING key) {
  if (!FPDFAttachment_HasKey(attachment, key)) {
    return FPDF_OBJECT_UNKNOWN;
  }

  CPDF_FileSpec spec(
      pdfium::WrapRetain(CPDFObjectFromFPDFAttachment(attachment)));
  RetainPtr<const CPDF_Object> pObj = spec.GetParamsDict()->GetObjectFor(key);
  return pObj ? pObj->GetType() : FPDF_OBJECT_UNKNOWN;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAttachment_SetStringValue(FPDF_ATTACHMENT attachment,
                              FPDF_BYTESTRING key,
                              FPDF_WIDESTRING value) {
  CPDF_Object* pFile = CPDFObjectFromFPDFAttachment(attachment);
  if (!pFile) {
    return false;
  }

  CPDF_FileSpec spec(pdfium::WrapRetain(pFile));
  RetainPtr<CPDF_Dictionary> pParamsDict = spec.GetMutableParamsDict();
  if (!pParamsDict) {
    return false;
  }

  // SAFETY: required from caller.
  ByteString bsValue = UNSAFE_BUFFERS(ByteStringFromFPDFWideString(value));
  ByteString bsKey = key;
  if (bsKey == kChecksumKey) {
    pParamsDict->SetNewFor<CPDF_String>(bsKey,
                                        HexDecode(bsValue.unsigned_span()).data,
                                        CPDF_String::DataType::kIsHex);
  } else {
    pParamsDict->SetNewFor<CPDF_String>(bsKey, bsValue);
  }
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAttachment_GetStringValue(FPDF_ATTACHMENT attachment,
                              FPDF_BYTESTRING key,
                              FPDF_WCHAR* buffer,
                              unsigned long buflen) {
  CPDF_Object* file = CPDFObjectFromFPDFAttachment(attachment);
  if (!file) {
    return 0;
  }

  CPDF_FileSpec spec(pdfium::WrapRetain(file));
  RetainPtr<const CPDF_Dictionary> params = spec.GetParamsDict();
  if (!params) {
    return 0;
  }

  // SAFETY: required from caller.
  auto buffer_span = UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen));

  ByteStringView key_view(key);
  RetainPtr<const CPDF_Object> object = params->GetObjectFor(key_view);
  if (!object || (!object->IsString() && !object->IsName())) {
    // Per API description, return an empty string in these cases.
    return Utf16EncodeMaybeCopyAndReturnLength(WideString(), buffer_span);
  }

  if (key_view == kChecksumKey) {
    RetainPtr<const CPDF_String> string_object = ToString(object);
    if (string_object && string_object->IsHex()) {
      ByteString encoded =
          PDF_HexEncodeString(string_object->GetString().AsStringView());
      return Utf16EncodeMaybeCopyAndReturnLength(
          PDF_DecodeText(encoded.unsigned_span()), buffer_span);
    }
  }

  return Utf16EncodeMaybeCopyAndReturnLength(object->GetUnicodeText(),
                                             buffer_span);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAttachment_SetFile(FPDF_ATTACHMENT attachment,
                       FPDF_DOCUMENT document,
                       const void* contents,
                       unsigned long len) {
  // An empty content must have a zero length.
  if (!contents && len != 0) {
    return false;
  }

  CPDF_Object* pFile = CPDFObjectFromFPDFAttachment(attachment);
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!pFile || !pFile->IsDictionary() || !doc || len > INT_MAX) {
    return false;
  }
  RetainPtr<CPDF_Object> effective_file;
  if (pFile->GetObjNum() != 0) {
    effective_file = doc->GetOrParseIndirectObject(pFile->GetObjNum());
    if (effective_file) {
      pFile = effective_file.Get();
    }
  }

  // Create a dictionary for the new embedded file stream.
  auto pFileStreamDict = pdfium::MakeRetain<CPDF_Dictionary>();
  auto pParamsDict = pFileStreamDict->SetNewFor<CPDF_Dictionary>("Params");

  // Set the size of the new file in the dictionary.
  pFileStreamDict->SetNewFor<CPDF_Number>(pdfium::stream::kDL,
                                          static_cast<int>(len));
  pParamsDict->SetNewFor<CPDF_Number>("Size", static_cast<int>(len));

  // Set the creation date of the new attachment in the dictionary.
  CFX_DateTime dateTime = CFX_DateTime::Now();
  pParamsDict->SetNewFor<CPDF_String>(
      "CreationDate",
      ByteString::Format("D:%d%02d%02d%02d%02d%02d", dateTime.GetYear(),
                         dateTime.GetMonth(), dateTime.GetDay(),
                         dateTime.GetHour(), dateTime.GetMinute(),
                         dateTime.GetSecond()));

  // SAFETY: required from caller.
  pdfium::span<const uint8_t> contents_span =
      UNSAFE_BUFFERS(pdfium::span(static_cast<const uint8_t*>(contents), len));

  std::array<uint8_t, 16> digest;
  CRYPT_MD5Generate(contents_span, digest);

  // Set the checksum of the new attachment in the dictionary.
  pParamsDict->SetNewFor<CPDF_String>(kChecksumKey, digest,
                                      CPDF_String::DataType::kIsHex);

  // Create the file stream and have the filespec dictionary link to it.
  auto pFileStream = doc->NewIndirect<CPDF_Stream>(
      DataVector<uint8_t>(contents_span.begin(), contents_span.end()),
      std::move(pFileStreamDict));

  auto pEFDict = pFile->AsMutableDictionary()->SetNewFor<CPDF_Dictionary>("EF");
  pEFDict->SetNewFor<CPDF_Reference>("F", doc, pFileStream->GetObjNum());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAttachment_GetFile(FPDF_ATTACHMENT attachment,
                       void* buffer,
                       unsigned long buflen,
                       unsigned long* out_buflen) {
  if (!out_buflen) {
    return false;
  }

  CPDF_Object* pFile = CPDFObjectFromFPDFAttachment(attachment);
  if (!pFile) {
    return false;
  }

  CPDF_FileSpec spec(pdfium::WrapRetain(pFile));
  RetainPtr<const CPDF_Stream> pFileStream = spec.GetFileStream();
  if (!pFileStream) {
    return false;
  }

  // SAFETY: required from caller.
  *out_buflen = DecodeStreamMaybeCopyAndReturnLength(
      std::move(pFileStream),
      UNSAFE_BUFFERS(pdfium::span(static_cast<uint8_t*>(buffer),
                                  static_cast<size_t>(buflen))));
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAttachment_GetSubtype(FPDF_ATTACHMENT attachment,
                          FPDF_WCHAR* buffer,
                          unsigned long buflen) {
  CPDF_Object* file = CPDFObjectFromFPDFAttachment(attachment);
  if (!file) {
    return 0;
  }

  // SAFETY: required from caller.
  auto buffer_span = UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen));
  CPDF_FileSpec spec(pdfium::WrapRetain(file));
  RetainPtr<const CPDF_Stream> file_stream = spec.GetFileStream();
  if (!file_stream) {
    return Utf16EncodeMaybeCopyAndReturnLength(WideString(), buffer_span);
  }

  ByteString subtype = file_stream->GetDict()->GetNameFor("Subtype");
  if (subtype.IsEmpty()) {
    // Per API description, return an empty string in these cases.
    return Utf16EncodeMaybeCopyAndReturnLength(WideString(), buffer_span);
  }

  return Utf16EncodeMaybeCopyAndReturnLength(
      PDF_DecodeText(subtype.unsigned_span()), buffer_span);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAttachment_SetSubtype(FPDF_ATTACHMENT attachment, FPDF_BYTESTRING subtype) {
  CPDF_Object* file = CPDFObjectFromFPDFAttachment(attachment);
  if (!file) {
    return false;
  }

  CPDF_FileSpec spec(pdfium::WrapRetain(file));
  RetainPtr<const CPDF_Stream> file_stream = spec.GetFileStream();
  if (!file_stream) {
    return false;
  }

  CPDF_Stream* s = const_cast<CPDF_Stream*>(file_stream.Get());
  CPDF_Dictionary* dict = s->GetMutableDict();
  if (!dict) {
    return false;
  }

  // Ensure /Type is present (defensive).
  if (dict->GetNameFor("Type").IsEmpty()) {
    dict->SetNewFor<CPDF_Name>("Type", "EmbeddedFile");
  }

  // Convert to ByteString.
  ByteString bs = subtype ? ByteString(subtype) : ByteString();
  if (bs.IsEmpty()) {
    dict->RemoveFor("Subtype");
  } else {
    dict->SetNewFor<CPDF_Name>("Subtype", bs);
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAttachment_SetDescription(FPDF_ATTACHMENT attachment,
                              FPDF_WIDESTRING desc) {
  CPDF_Object* file = CPDFObjectFromFPDFAttachment(attachment);
  if (!file || !file->IsDictionary()) {
    return false;
  }

  // SAFETY: required from caller.
  WideString ws = UNSAFE_BUFFERS(WideStringFromFPDFWideString(desc));
  CPDF_Dictionary* filespec = file->AsMutableDictionary();

  if (ws.IsEmpty()) {
    filespec->RemoveFor("Desc");
  } else {
    filespec->SetNewFor<CPDF_String>("Desc", ws.AsStringView());
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAttachment_SetName(FPDF_ATTACHMENT attachment, FPDF_WIDESTRING name) {
  CPDF_Object* file = CPDFObjectFromFPDFAttachment(attachment);
  if (!file || !file->IsDictionary()) {
    return false;
  }

  // SAFETY: required from caller.
  WideString ws = UNSAFE_BUFFERS(WideStringFromFPDFWideString(name));
  if (ws.IsEmpty()) {
    return false;
  }

  // The same two entries FPDFAnnot_AddFileAttachment and FPDFDoc_AddAttachment
  // write, so a renamed file reads like one created with that name.
  CPDF_Dictionary* filespec = file->AsMutableDictionary();
  filespec->SetNewFor<CPDF_String>("UF", ws.AsStringView());
  filespec->SetNewFor<CPDF_String>("F", ws.AsStringView());
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAttachment_GetDescription(FPDF_ATTACHMENT attachment,
                              FPDF_WCHAR* buffer,
                              unsigned long buflen) {
  CPDF_Object* file = CPDFObjectFromFPDFAttachment(attachment);
  if (!file || !file->IsDictionary()) {
    return 0;
  }

  RetainPtr<const CPDF_Object> obj = file->AsDictionary()->GetObjectFor("Desc");
  if (!obj || !obj->IsString()) {
    return Utf16EncodeMaybeCopyAndReturnLength(
        WideString(), UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
  }

  return Utf16EncodeMaybeCopyAndReturnLength(
      obj->GetUnicodeText(),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAttachment_GetIntegerValue(FPDF_ATTACHMENT attachment,
                               FPDF_BYTESTRING key,
                               int* out_value) {
  if (!out_value) {
    return false;
  }

  CPDF_Object* file = CPDFObjectFromFPDFAttachment(attachment);
  if (!file) {
    return false;
  }

  CPDF_FileSpec spec(pdfium::WrapRetain(file));
  RetainPtr<const CPDF_Dictionary> params = spec.GetParamsDict();
  if (!params) {
    return false;
  }

  ByteStringView k(key);
  RetainPtr<const CPDF_Object> obj = params->GetObjectFor(k);
  if (!obj || !obj->IsNumber()) {
    return false;
  }

  const CPDF_Number* num = obj->AsNumber();
  *out_value =
      num->IsInteger() ? num->GetInteger() : static_cast<int>(num->GetNumber());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAttachment_ExtractFile(FPDF_ATTACHMENT attachment,
                           FPDF_FILEWRITE* file_write,
                           uint64_t max_decoded_bytes,
                           uint32_t* out_size,
                           EPDFAttachmentExtractStatus* out_status) {
  if (out_size) {
    *out_size = 0;
  }
  if (out_status) {
    *out_status = EPDFAttachmentExtractStatus_kWriteFailed;
  }
  if (!file_write || file_write->version != 1 || !file_write->WriteBlock) {
    return false;
  }

  ExtractOutcome outcome = CapOutcomeToUint32(ExtractAttachmentFileToSink(
      attachment, max_decoded_bytes,
      [file_write](pdfium::span<const uint8_t> chunk) {
        return file_write->WriteBlock(
                   file_write, chunk.data(),
                   pdfium::checked_cast<unsigned long>(chunk.size())) != 0;
      }));
  if (out_status) {
    *out_status = outcome.status;
  }
  if (outcome.status != EPDFAttachmentExtractStatus_kSuccess) {
    return false;
  }
  if (out_size) {
    *out_size = static_cast<uint32_t>(outcome.size);
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAttachment_ExtractFileToOwnedBuffer(
    FPDF_ATTACHMENT attachment,
    uint64_t max_decoded_bytes,
    void** out_buffer,
    uint32_t* out_size,
    EPDFAttachmentExtractStatus* out_status) {
  if (out_buffer) {
    *out_buffer = nullptr;
  }
  if (out_size) {
    *out_size = 0;
  }
  if (out_status) {
    *out_status = EPDFAttachmentExtractStatus_kWriteFailed;
  }
  if (!out_buffer || !out_size) {
    return false;
  }

  DataVector<uint8_t> data;
  ExtractOutcome outcome = CapOutcomeToUint32(ExtractAttachmentFileToSink(
      attachment, max_decoded_bytes,
      [&data](pdfium::span<const uint8_t> chunk) {
        data.insert(data.end(), chunk.begin(), chunk.end());
        return true;
      }));
  if (out_status) {
    *out_status = outcome.status;
  }
  if (outcome.status != EPDFAttachmentExtractStatus_kSuccess) {
    return false;
  }
  // A zero-byte embedded file is a valid success: NULL buffer, size 0.
  if (data.empty()) {
    return true;
  }

  // Must be malloc() so EPDF_FreeBuffer() (which calls free()) can release
  // it — same contract as the EPDF_*ToOwnedBuffer() save APIs.
  void* buffer = malloc(data.size());
  if (!buffer) {
    if (out_status) {
      *out_status = EPDFAttachmentExtractStatus_kWriteFailed;
    }
    return false;
  }
  memcpy(buffer, data.data(), data.size());
  *out_buffer = buffer;
  *out_size = static_cast<uint32_t>(data.size());
  return true;
}
