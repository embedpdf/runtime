// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/fpdfview.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/fdrm/fx_crypt_sha.h"
#include "core/fpdfapi/edit/cpdf_creator.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_base_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_write_generation.h"
#include "core/fxcrt/cfx_fileaccess_stream.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span_util.h"
#include "fpdfsdk/cpdfsdk_customaccess.h"
#include "fpdfsdk/cpdfsdk_filewriteadapter.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/fpdf_save.h"

namespace {

CPDF_LayerDocument* LayerFromFPDFDocument(FPDF_DOCUMENT layer) {
  return CPDF_LayerDocument::FromDocument(CPDFDocumentFromFPDFDocument(layer));
}

constexpr FX_FILESIZE kReservedDeltaHeadroom = 16 * 1024 * 1024;
constexpr FX_FILESIZE kSafeNotionalStartOffsetMax =
    0xffffffff - kReservedDeltaHeadroom;
// A layer artifact: a header, the layer's raw delta, and (version 2) its
// birth list. Version 1 had no birth list: it reads as a layer that promoted
// nothing, which is what every version-1 layer is.
constexpr char kLayerArtifactMagic[] = "EPDFLYR1";
constexpr uint32_t kLayerArtifactVersion = 2;
constexpr size_t kSha256DigestSize = 32;
constexpr size_t kLayerArtifactHeaderSizeV1 =
    8 + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t) * 3 +
    kSha256DigestSize * 2;
// Version 2 adds the birth list's size.
constexpr size_t kLayerArtifactHeaderSize =
    kLayerArtifactHeaderSizeV1 + sizeof(uint64_t);

// One byte range of a reader the runtime keeps open: the delta inside an
// artifact file, read in place. Its underlying stream is the file, which is
// what a layer recognises as storage it owns.
class FileRangeReadStream final : public IFX_SeekableReadStream {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  FX_FILESIZE GetSize() override { return size_; }

  bool ReadBlockAtOffset(pdfium::span<uint8_t> buffer,
                         FX_FILESIZE offset) override {
    if (offset < 0 || offset > size_ ||
        static_cast<FX_FILESIZE>(buffer.size()) > size_ - offset) {
      return false;
    }
    if (buffer.empty()) {
      return true;
    }
    return inner_->ReadBlockAtOffset(buffer, start_ + offset);
  }

  IFX_SeekableReadStream* GetUnderlyingStream() override {
    return inner_->GetUnderlyingStream();
  }
  bool IsSelfContained() const override { return inner_->IsSelfContained(); }

 private:
  FileRangeReadStream(RetainPtr<IFX_SeekableReadStream> inner,
                      FX_FILESIZE start,
                      FX_FILESIZE size)
      : inner_(std::move(inner)), start_(start), size_(size) {}
  ~FileRangeReadStream() override = default;

  RetainPtr<IFX_SeekableReadStream> inner_;
  const FX_FILESIZE start_;
  const FX_FILESIZE size_;
};

class OwnedReadOnlyMemoryStream final : public IFX_SeekableReadStream {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  FX_FILESIZE GetSize() override {
    return static_cast<FX_FILESIZE>(data_.size());
  }
  bool IsSelfContained() const override { return true; }  // owns |data_|

  bool ReadBlockAtOffset(pdfium::span<uint8_t> buffer,
                         FX_FILESIZE offset) override {
    if (offset < 0 || static_cast<uint64_t>(offset) > data_.size() ||
        buffer.size() > data_.size() - static_cast<size_t>(offset)) {
      return false;
    }
    if (buffer.empty()) {
      return true;
    }
    memcpy(buffer.data(), data_.data() + offset, buffer.size());
    return true;
  }

 private:
  explicit OwnedReadOnlyMemoryStream(DataVector<uint8_t> data)
      : data_(std::move(data)) {}
  ~OwnedReadOnlyMemoryStream() override = default;

  DataVector<uint8_t> data_;
};

struct MemoryFileWriter : public FPDF_FILEWRITE {
  std::string data;

  MemoryFileWriter() {
    version = 1;
    WriteBlock = [](FPDF_FILEWRITE* self, const void* buf,
                    unsigned long size) -> int {
      static_cast<MemoryFileWriter*>(self)->data.append(
          static_cast<const char*>(buf), size);
      return 1;
    };
  }
};

struct HashingTempFileWriter : public FPDF_FILEWRITE {
  FILE* file = nullptr;
  CRYPT_sha2_context sha_context = {};
  uint64_t size = 0;
  bool failed = false;
  bool finalized = false;

  HashingTempFileWriter() {
    version = 1;
    file = std::tmpfile();
    CRYPT_SHA256Start(&sha_context);
    WriteBlock = [](FPDF_FILEWRITE* self, const void* buf,
                    unsigned long block_size) -> int {
      auto* writer = static_cast<HashingTempFileWriter*>(self);
      if (!writer || !writer->file || writer->failed || writer->finalized) {
        return 0;
      }
      if (writer->size + block_size < writer->size) {
        writer->failed = true;
        return 0;
      }
      if (block_size == 0) {
        return 1;
      }
      const size_t written = fwrite(buf, 1, block_size, writer->file);
      if (written != block_size) {
        writer->failed = true;
        return 0;
      }
      CRYPT_SHA256Update(&writer->sha_context,
                         UNSAFE_BUFFERS(pdfium::span(
                             static_cast<const uint8_t*>(buf), block_size)));
      writer->size += block_size;
      return 1;
    };
  }

  ~HashingTempFileWriter() {
    if (file) {
      fclose(file);
    }
  }

  bool IsValid() const { return file && !failed; }

  std::optional<std::array<uint8_t, kSha256DigestSize>> FinishSha256() {
    if (!IsValid() || finalized) {
      return std::nullopt;
    }
    if (fflush(file) != 0) {
      failed = true;
      return std::nullopt;
    }
    finalized = true;
    std::array<uint8_t, kSha256DigestSize> digest = {};
    CRYPT_SHA256Finish(&sha_context, digest);
    return digest;
  }

  bool ReplayTo(FPDF_FILEWRITE* out) {
    if (!out || !IsValid() || !finalized) {
      return false;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
      return false;
    }

    std::array<uint8_t, 8192> buffer = {};
    uint64_t remaining = size;
    while (remaining > 0) {
      const size_t chunk_size =
          static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
      const size_t read = fread(buffer.data(), 1, chunk_size, file);
      if (read != chunk_size) {
        return false;
      }
      if (!out->WriteBlock(out, buffer.data(),
                           static_cast<unsigned long>(chunk_size))) {
        return false;
      }
      remaining -= chunk_size;
    }
    return true;
  }
};

CPDF_BaseDocument* CPDFBaseDocumentFromEPDFBaseDocument(
    EPDF_BASE_DOCUMENT base) {
  return reinterpret_cast<CPDF_BaseDocument*>(base);
}

EPDF_BASE_DOCUMENT EPDFBaseDocumentFromCPDFBaseDocument(
    CPDF_BaseDocument* base) {
  return reinterpret_cast<EPDF_BASE_DOCUMENT>(base);
}

EPDFLayerOpenStatus ToPublicStatus(CPDF_LayerDocument::OpenStatus status) {
  switch (status) {
    case CPDF_LayerDocument::OpenStatus::kSuccess:
      return EPDFLayerOpenStatus_kSuccess;
    case CPDF_LayerDocument::OpenStatus::kMalformedDelta:
      return EPDFLayerOpenStatus_kMalformedDelta;
    case CPDF_LayerDocument::OpenStatus::kBaseLayerMismatch:
      return EPDFLayerOpenStatus_kBaseLayerMismatch;
    case CPDF_LayerDocument::OpenStatus::kOpenFailed:
      return EPDFLayerOpenStatus_kOpenFailed;
  }
}

void SetOpenStatus(EPDFLayerOpenStatus* out_status,
                   EPDFLayerOpenStatus status) {
  if (out_status) {
    *out_status = status;
  }
}

void SetSaveStatus(EPDFLayerSaveStatus* out_status,
                   EPDFLayerSaveStatus status) {
  if (out_status) {
    *out_status = status;
  }
}

FPDF_DOCUMENT OpenLayerWithDeltaStream(
    EPDF_BASE_DOCUMENT base,
    RetainPtr<IFX_SeekableReadStream> delta_stream,
    EPDFLayerOpenStatus* out_status,
    std::map<uint32_t, CPDF_PageBirths> births = {}) {
  SetOpenStatus(out_status, EPDFLayerOpenStatus_kOpenFailed);
  // Not while a layer transaction is open on this thread: the open
  // generation is the thread's, so nothing could write the new layer.
  if (!base || CPDF_WriteGeneration::Current() != 0) {
    return nullptr;
  }

  CPDF_BaseDocument* base_doc = CPDFBaseDocumentFromEPDFBaseDocument(base);
  RetainPtr<CPDF_BaseDocument> retained_base = pdfium::WrapRetain(base_doc);
  auto layer = std::make_unique<CPDF_LayerDocument>(std::move(retained_base),
                                                    std::move(delta_stream));

  const EPDFLayerOpenStatus status = ToPublicStatus(layer->ingest_status());
  SetOpenStatus(out_status, status);
  if (status != EPDFLayerOpenStatus_kSuccess) {
    return nullptr;
  }
  // The births name objects the delta carries, so they load after it.
  if (!births.empty() && !layer->LoadBirths(std::move(births))) {
    SetOpenStatus(out_status, EPDFLayerOpenStatus_kMalformedDelta);
    return nullptr;
  }

  return FPDFDocumentFromCPDFDocument(layer.release());
}

std::optional<std::array<uint8_t, kSha256DigestSize>> ComputeDeltaSha256(
    IFX_SeekableReadStream* stream,
    FX_FILESIZE size) {
  if (!stream || size < 0) {
    return std::nullopt;
  }

  CRYPT_sha2_context context;
  CRYPT_SHA256Start(&context);
  std::array<uint8_t, 8192> buffer = {};
  FX_FILESIZE offset = 0;
  while (offset < size) {
    const size_t read_size = static_cast<size_t>(
        std::min<FX_FILESIZE>(buffer.size(), size - offset));
    if (!stream->ReadBlockAtOffset(pdfium::span(buffer).first(read_size),
                                   offset)) {
      return std::nullopt;
    }
    CRYPT_SHA256Update(&context, pdfium::span(buffer).first(read_size));
    offset += read_size;
  }

  std::array<uint8_t, kSha256DigestSize> digest = {};
  CRYPT_SHA256Finish(&context, digest);
  return digest;
}

void AppendUint32LE(std::vector<uint8_t>* buffer, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) {
    buffer->push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
}

void AppendUint64LE(std::vector<uint8_t>* buffer, uint64_t value) {
  for (size_t i = 0; i < 8; ++i) {
    buffer->push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
}

std::vector<uint8_t> BuildLayerArtifactHeader(
    CPDF_BaseDocument* base_doc,
    uint64_t delta_size,
    const std::array<uint8_t, kSha256DigestSize>& delta_sha,
    uint64_t births_size) {
  std::vector<uint8_t> artifact;
  artifact.reserve(kLayerArtifactHeaderSize);
  artifact.insert(artifact.end(), kLayerArtifactMagic, kLayerArtifactMagic + 8);
  AppendUint32LE(&artifact, kLayerArtifactVersion);
  AppendUint32LE(&artifact, kLayerArtifactHeaderSize);
  AppendUint64LE(&artifact, static_cast<uint64_t>(base_doc->GetRawBaseSize()));
  AppendUint64LE(&artifact,
                 static_cast<uint64_t>(base_doc->GetLayerAppendBaseOffset()));
  AppendUint64LE(&artifact, delta_size);
  const std::array<uint8_t, kSha256DigestSize>& base_sha =
      base_doc->GetRawBaseSha256();
  artifact.insert(artifact.end(), base_sha.begin(), base_sha.end());
  artifact.insert(artifact.end(), delta_sha.begin(), delta_sha.end());
  AppendUint64LE(&artifact, births_size);
  return artifact;
}

// The birth list as bytes, little-endian: the page count; then per page its
// object number and entry count, and per entry the birth index and the
// object it became.
std::vector<uint8_t> SerializeBirths(
    const std::map<uint32_t, CPDF_PageBirths>& births) {
  std::vector<uint8_t> bytes;
  if (births.empty()) {
    return bytes;
  }
  AppendUint32LE(&bytes, static_cast<uint32_t>(births.size()));
  for (const auto& [page_objnum, page_births] : births) {
    AppendUint32LE(&bytes, page_objnum);
    AppendUint32LE(&bytes, static_cast<uint32_t>(page_births.size()));
    for (const auto& [birth_index, objnum] : page_births) {
      AppendUint32LE(&bytes, birth_index);
      AppendUint32LE(&bytes, objnum);
    }
  }
  return bytes;
}

bool WriteBytes(FPDF_FILEWRITE* file_write, pdfium::span<const uint8_t> bytes) {
  if (!file_write) {
    return false;
  }
  if (bytes.empty()) {
    return true;
  }
  if (bytes.size() > std::numeric_limits<unsigned long>::max()) {
    return false;
  }
  return file_write->WriteBlock(file_write, bytes.data(),
                                static_cast<unsigned long>(bytes.size()));
}

uint32_t ReadUint32LE(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t ReadUint64LE(const uint8_t* data) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(data[i]) << (i * 8);
  }
  return value;
}

// SerializeBirths(), read back. Null when the bytes aren't exactly that.
std::optional<std::map<uint32_t, CPDF_PageBirths>> ParseBirths(
    pdfium::span<const uint8_t> bytes) {
  std::map<uint32_t, CPDF_PageBirths> births;
  if (bytes.empty()) {
    return births;
  }
  size_t cursor = 0;
  auto read = [&](uint32_t* value) {
    if (bytes.size() - cursor < sizeof(uint32_t)) {
      return false;
    }
    *value = ReadUint32LE(bytes.subspan(cursor).data());
    cursor += sizeof(uint32_t);
    return true;
  };
  uint32_t page_count = 0;
  if (!read(&page_count)) {
    return std::nullopt;
  }
  for (uint32_t page = 0; page < page_count; ++page) {
    uint32_t page_objnum = 0;
    uint32_t entry_count = 0;
    if (!read(&page_objnum) || !read(&entry_count) ||
        births.contains(page_objnum)) {
      return std::nullopt;
    }
    CPDF_PageBirths& page_births = births[page_objnum];
    for (uint32_t entry = 0; entry < entry_count; ++entry) {
      uint32_t birth_index = 0;
      uint32_t objnum = 0;
      if (!read(&birth_index) || !read(&objnum) ||
          !page_births.emplace(birth_index, objnum).second) {
        return std::nullopt;
      }
    }
  }
  if (cursor != bytes.size()) {
    return std::nullopt;
  }
  return births;
}

// A layer artifact's header, either version.
struct LayerArtifactHeader {
  uint32_t size = 0;  // the bytes before the delta
  uint64_t raw_base_size = 0;
  uint64_t layer_append_base_offset = 0;
  uint64_t delta_size = 0;
  uint64_t births_size = 0;  // 0 in version 1
  std::array<uint8_t, kSha256DigestSize> base_sha = {};
  std::array<uint8_t, kSha256DigestSize> delta_sha = {};
};

// Reads the header at the start of |bytes|, which hold at least the header
// (or the whole artifact, if it is shorter). |total_size| is the artifact's
// size. Checks it is this layer's base's and that the parts add up.
std::optional<LayerArtifactHeader> ReadLayerArtifactHeader(
    pdfium::span<const uint8_t> bytes,
    uint64_t total_size,
    CPDF_BaseDocument* base_doc,
    EPDFLayerOpenStatus* out_status) {
  constexpr size_t kPrefix = 8 + sizeof(uint32_t) * 2;
  if (bytes.size() < kPrefix || memcmp(bytes.data(), kLayerArtifactMagic, 8)) {
    SetOpenStatus(out_status, EPDFLayerOpenStatus_kMalformedDelta);
    return std::nullopt;
  }
  const uint32_t version = ReadUint32LE(bytes.data() + 8);
  LayerArtifactHeader header;
  header.size = ReadUint32LE(bytes.data() + 12);
  const size_t expected_size =
      version == 1 ? kLayerArtifactHeaderSizeV1 : kLayerArtifactHeaderSize;
  if ((version != 1 && version != kLayerArtifactVersion) ||
      header.size != expected_size) {
    SetOpenStatus(out_status, EPDFLayerOpenStatus_kBaseLayerMismatch);
    return std::nullopt;
  }
  if (bytes.size() < header.size) {
    SetOpenStatus(out_status, EPDFLayerOpenStatus_kMalformedDelta);
    return std::nullopt;
  }
  size_t cursor = kPrefix;
  header.raw_base_size = ReadUint64LE(bytes.data() + cursor);
  cursor += sizeof(uint64_t);
  header.layer_append_base_offset = ReadUint64LE(bytes.data() + cursor);
  cursor += sizeof(uint64_t);
  header.delta_size = ReadUint64LE(bytes.data() + cursor);
  cursor += sizeof(uint64_t);
  memcpy(header.base_sha.data(), bytes.data() + cursor, kSha256DigestSize);
  cursor += kSha256DigestSize;
  memcpy(header.delta_sha.data(), bytes.data() + cursor, kSha256DigestSize);
  cursor += kSha256DigestSize;
  if (version >= 2) {
    header.births_size = ReadUint64LE(bytes.data() + cursor);
  }

  if (header.raw_base_size !=
          static_cast<uint64_t>(base_doc->GetRawBaseSize()) ||
      header.layer_append_base_offset !=
          static_cast<uint64_t>(base_doc->GetLayerAppendBaseOffset()) ||
      header.delta_size > total_size - header.size) {
    SetOpenStatus(out_status, EPDFLayerOpenStatus_kBaseLayerMismatch);
    return std::nullopt;
  }
  if (header.births_size > total_size - header.size - header.delta_size ||
      header.size + header.delta_size + header.births_size != total_size) {
    SetOpenStatus(out_status, EPDFLayerOpenStatus_kMalformedDelta);
    return std::nullopt;
  }
  if (memcmp(base_doc->GetRawBaseSha256().data(), header.base_sha.data(),
             kSha256DigestSize) != 0) {
    SetOpenStatus(out_status, EPDFLayerOpenStatus_kBaseLayerMismatch);
    return std::nullopt;
  }
  return header;
}

void* CopyToOwnedBuffer(pdfium::span<const uint8_t> data,
                        unsigned long* out_size) {
  if (!out_size || data.empty() ||
      data.size() > std::numeric_limits<unsigned long>::max()) {
    if (out_size) {
      *out_size = 0;
    }
    return nullptr;
  }

  void* buffer = malloc(data.size());
  if (!buffer) {
    *out_size = 0;
    return nullptr;
  }
  memcpy(buffer, data.data(), data.size());
  *out_size = static_cast<unsigned long>(data.size());
  return buffer;
}

DataVector<uint8_t> ReadStreamToVector(IFX_SeekableReadStream* stream) {
  if (!stream || stream->GetSize() < 0 ||
      !pdfium::IsValueInRangeForNumericType<size_t>(stream->GetSize())) {
    return {};
  }

  const FX_FILESIZE size = stream->GetSize();
  DataVector<uint8_t> data(pdfium::checked_cast<size_t>(size));
  if (!data.empty() &&
      !stream->ReadBlockAtOffset(pdfium::span(data), /*offset=*/0)) {
    return {};
  }
  return data;
}

}  // namespace

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFLayer_OpenLayer(EPDF_BASE_DOCUMENT base,
                    FPDF_FILEACCESS* pFileAccess,
                    FPDF_BYTESTRING password,
                    EPDFLayerOpenStatus* out_status) {
  // Slice 7.2 layers share the base parser/security state; password handling is
  // already complete when the base is loaded.
  (void)password;

  // The layer retains the delta it ingests as part of its loaded bytes, and
  // |pFileAccess| is only promised for the duration of this call: copy it.
  RetainPtr<IFX_SeekableReadStream> delta_stream;
  if (pFileAccess && pFileAccess->m_FileLen > 0) {
    auto caller_stream = pdfium::MakeRetain<CPDFSDK_CustomAccess>(pFileAccess);
    DataVector<uint8_t> delta = ReadStreamToVector(caller_stream.Get());
    if (delta.empty()) {
      SetOpenStatus(out_status, EPDFLayerOpenStatus_kOpenFailed);
      return nullptr;
    }
    delta_stream =
        pdfium::MakeRetain<OwnedReadOnlyMemoryStream>(std::move(delta));
  }
  return OpenLayerWithDeltaStream(base, std::move(delta_stream), out_status);
}

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFLayer_OpenLayerArtifact(EPDF_BASE_DOCUMENT base,
                            FPDF_FILEACCESS* pFileAccess,
                            FPDF_BYTESTRING password,
                            EPDFLayerOpenStatus* out_status) {
  (void)password;
  SetOpenStatus(out_status, EPDFLayerOpenStatus_kOpenFailed);
  if (!base || !pFileAccess) {
    return nullptr;
  }

  CPDF_BaseDocument* base_doc = CPDFBaseDocumentFromEPDFBaseDocument(base);
  if (!base_doc) {
    return nullptr;
  }

  RetainPtr<IFX_SeekableReadStream> artifact_stream =
      pdfium::MakeRetain<CPDFSDK_CustomAccess>(pFileAccess);
  DataVector<uint8_t> artifact = ReadStreamToVector(artifact_stream.Get());
  const std::optional<LayerArtifactHeader> header =
      ReadLayerArtifactHeader(artifact, artifact.size(), base_doc, out_status);
  if (!header) {
    return nullptr;
  }

  const size_t delta_offset = header->size;
  DataVector<uint8_t> delta;
  delta.resize(static_cast<size_t>(header->delta_size));
  if (!delta.empty()) {
    memcpy(delta.data(), artifact.data() + delta_offset, delta.size());
  }
  std::optional<std::array<uint8_t, kSha256DigestSize>> actual_delta_sha =
      ComputeDeltaSha256(
          pdfium::MakeRetain<OwnedReadOnlyMemoryStream>(delta).Get(),
          delta.size());
  if (!actual_delta_sha || *actual_delta_sha != header->delta_sha) {
    SetOpenStatus(out_status, EPDFLayerOpenStatus_kMalformedDelta);
    return nullptr;
  }
  std::optional<std::map<uint32_t, CPDF_PageBirths>> births =
      ParseBirths(pdfium::span<const uint8_t>(artifact).subspan(
          delta_offset + delta.size(),
          static_cast<size_t>(header->births_size)));
  if (!births) {
    SetOpenStatus(out_status, EPDFLayerOpenStatus_kMalformedDelta);
    return nullptr;
  }

  return OpenLayerWithDeltaStream(
      base, pdfium::MakeRetain<OwnedReadOnlyMemoryStream>(std::move(delta)),
      out_status, std::move(*births));
}

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFLayer_OpenLayerArtifactFromPath(EPDF_BASE_DOCUMENT base,
                                    FPDF_STRING path,
                                    FPDF_BYTESTRING password,
                                    EPDFLayerOpenStatus* out_status) {
  (void)password;
  SetOpenStatus(out_status, EPDFLayerOpenStatus_kOpenFailed);
  if (!base || !path || !*path) {
    return nullptr;
  }
  CPDF_BaseDocument* base_doc = CPDFBaseDocumentFromEPDFBaseDocument(base);
  if (!base_doc) {
    return nullptr;
  }
  // The runtime opens the file itself and the layer keeps it open: the
  // delta is read in place, and every stream it carries stays a view.
  RetainPtr<IFX_SeekableReadStream> file =
      CFX_FileAccessStream::CreateFromFilename(path);
  if (!file) {
    return nullptr;
  }
  const FX_FILESIZE file_size = file->GetSize();
  if (file_size < 0) {
    return nullptr;
  }
  // The header is at most kLayerArtifactHeaderSize bytes; a version-1 file
  // with a tiny delta can be shorter than that.
  std::array<uint8_t, kLayerArtifactHeaderSize> header_bytes = {};
  const size_t header_read = static_cast<size_t>(std::min<FX_FILESIZE>(
      file_size, static_cast<FX_FILESIZE>(header_bytes.size())));
  if (!file->ReadBlockAtOffset(pdfium::span(header_bytes).first(header_read),
                               0)) {
    return nullptr;
  }
  const std::optional<LayerArtifactHeader> header = ReadLayerArtifactHeader(
      pdfium::span<const uint8_t>(header_bytes).first(header_read),
      static_cast<uint64_t>(file_size), base_doc, out_status);
  if (!header) {
    return nullptr;
  }

  RetainPtr<IFX_SeekableReadStream> delta_stream;
  if (header->delta_size > 0) {
    delta_stream = pdfium::MakeRetain<FileRangeReadStream>(
        file, static_cast<FX_FILESIZE>(header->size),
        static_cast<FX_FILESIZE>(header->delta_size));
    std::optional<std::array<uint8_t, kSha256DigestSize>> actual_delta_sha =
        ComputeDeltaSha256(delta_stream.Get(),
                           static_cast<FX_FILESIZE>(header->delta_size));
    if (!actual_delta_sha || *actual_delta_sha != header->delta_sha) {
      SetOpenStatus(out_status, EPDFLayerOpenStatus_kMalformedDelta);
      return nullptr;
    }
  }
  // The birth list is small: read it whole.
  DataVector<uint8_t> births_bytes(static_cast<size_t>(header->births_size));
  if (!births_bytes.empty() &&
      !file->ReadBlockAtOffset(
          births_bytes,
          static_cast<FX_FILESIZE>(header->size + header->delta_size))) {
    return nullptr;
  }
  std::optional<std::map<uint32_t, CPDF_PageBirths>> births =
      ParseBirths(births_bytes);
  if (!births) {
    SetOpenStatus(out_status, EPDFLayerOpenStatus_kMalformedDelta);
    return nullptr;
  }
  return OpenLayerWithDeltaStream(base, std::move(delta_stream), out_status,
                                  std::move(*births));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFLayer_IsObjectPromoted(FPDF_DOCUMENT layer, unsigned long obj_num) {
  if (obj_num > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  CPDF_Document* document = CPDFDocumentFromFPDFDocument(layer);
  CPDF_LayerDocument* layer_doc = CPDF_LayerDocument::FromDocument(document);
  return layer_doc &&
         layer_doc->IsObjectPromoted(static_cast<uint32_t>(obj_num));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFPage_IsValid(FPDF_PAGE page) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  CPDF_Document* doc = pdf_page ? pdf_page->GetDocument() : nullptr;
  if (!doc) {
    return false;
  }
  CPDF_DocumentViewScope document_view(doc);
  RetainPtr<const CPDF_Dictionary> dict = pdf_page->GetDict();
  const uint32_t objnum = dict ? dict->GetObjNum() : 0;
  return objnum && doc->GetPageIndex(objnum) >= 0 &&
         doc->GetIndirectObject(objnum);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFLayer_IsPagePromoted(FPDF_DOCUMENT layer, unsigned long page_obj_num) {
  CPDF_LayerDocument* layer_doc = LayerFromFPDFDocument(layer);
  return layer_doc && page_obj_num <= std::numeric_limits<uint32_t>::max() &&
         layer_doc->IsPagePromoted(static_cast<uint32_t>(page_obj_num));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFLayer_GetBirthObjectNumber(FPDF_DOCUMENT layer,
                               unsigned long page_obj_num,
                               unsigned long birth_index) {
  CPDF_LayerDocument* layer_doc = LayerFromFPDFDocument(layer);
  if (!layer_doc || page_obj_num > std::numeric_limits<uint32_t>::max() ||
      birth_index > std::numeric_limits<uint32_t>::max()) {
    return 0;
  }
  return layer_doc->FindBirth(static_cast<uint32_t>(page_obj_num),
                              static_cast<uint32_t>(birth_index));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFLayer_GetBirthName(FPDF_DOCUMENT layer,
                       unsigned long obj_num,
                       unsigned long* page_obj_num,
                       unsigned long* birth_index) {
  CPDF_LayerDocument* layer_doc = LayerFromFPDFDocument(layer);
  if (!layer_doc || !page_obj_num || !birth_index ||
      obj_num > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  std::optional<std::pair<uint32_t, uint32_t>> name =
      layer_doc->FindBirthName(static_cast<uint32_t>(obj_num));
  if (!name) {
    return false;
  }
  *page_obj_num = name->first;
  *birth_index = name->second;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFLayer_BeginTransaction(FPDF_DOCUMENT layer) {
  CPDF_LayerDocument* layer_doc = LayerFromFPDFDocument(layer);
  return layer_doc && layer_doc->BeginTransaction();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFLayer_CommitTransaction(FPDF_DOCUMENT layer) {
  CPDF_LayerDocument* layer_doc = LayerFromFPDFDocument(layer);
  return layer_doc && layer_doc->CommitTransaction();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFLayer_AbortTransaction(FPDF_DOCUMENT layer) {
  CPDF_LayerDocument* layer_doc = LayerFromFPDFDocument(layer);
  return layer_doc && layer_doc->AbortTransaction();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFLayer_IsInTransaction(FPDF_DOCUMENT layer) {
  CPDF_LayerDocument* layer_doc = LayerFromFPDFDocument(layer);
  return layer_doc && layer_doc->InTransaction();
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFLayer_GetPromotedObjectCount(FPDF_DOCUMENT layer) {
  CPDF_Document* document = CPDFDocumentFromFPDFDocument(layer);
  CPDF_LayerDocument* layer_doc = CPDF_LayerDocument::FromDocument(document);
  return layer_doc ? layer_doc->GetPromotedObjectCount() : 0;
}

FPDF_EXPORT EPDF_BASE_DOCUMENT FPDF_CALLCONV
EPDFLayer_GetBaseDocument(FPDF_DOCUMENT layer) {
  CPDF_Document* document = CPDFDocumentFromFPDFDocument(layer);
  CPDF_LayerDocument* layer_doc = CPDF_LayerDocument::FromDocument(document);
  return layer_doc ? EPDFBaseDocumentFromCPDFBaseDocument(
                         layer_doc->GetBaseDocument())
                   : nullptr;
}

FPDF_EXPORT void FPDF_CALLCONV
EPDF_SetBaseDocumentSha256(EPDF_BASE_DOCUMENT base,
                           const unsigned char* sha256) {
  CPDF_BaseDocument* base_doc = CPDFBaseDocumentFromEPDFBaseDocument(base);
  if (!base_doc || !sha256) {
    return;
  }
  std::array<uint8_t, kSha256DigestSize> digest;
  // SAFETY: the caller provides 32 bytes.
  auto in = UNSAFE_BUFFERS(pdfium::span(sha256, kSha256DigestSize));
  std::copy(in.begin(), in.end(), digest.begin());
  base_doc->SetKnownRawBaseSha256(digest);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFLayer_GetBaseSha256(FPDF_DOCUMENT layer, unsigned char* out_sha256) {
  CPDF_Document* document = CPDFDocumentFromFPDFDocument(layer);
  CPDF_LayerDocument* layer_doc = CPDF_LayerDocument::FromDocument(document);
  if (!layer_doc || !out_sha256) {
    return false;
  }
  const std::array<uint8_t, kSha256DigestSize>& sha =
      layer_doc->GetBaseDocument()->GetRawBaseSha256();
  // SAFETY: the caller provides 32 bytes.
  auto out = UNSAFE_BUFFERS(pdfium::span(out_sha256, kSha256DigestSize));
  std::copy(sha.begin(), sha.end(), out.begin());
  return true;
}

namespace {

void SetChanged(FPDF_BOOL* out_changed, bool changed) {
  if (out_changed) {
    *out_changed = changed;
  }
}

// The cumulative delta against the base (overlay objects equal to their base
// twin are never written). With |skip_if_unchanged| nothing is written when
// no reachable object differs from the document the layer was opened with;
// |out_changed_since_load| reports that either way.
bool SaveDeltaImpl(FPDF_DOCUMENT layer,
                   FPDF_FILEWRITE* file_write,
                   EPDFLayerSaveStatus* out_status,
                   bool skip_if_unchanged,
                   FPDF_BOOL* out_changed_since_load) {
  SetSaveStatus(out_status, EPDFLayerSaveStatus_kSaveFailed);
  SetChanged(out_changed_since_load, false);
  CPDF_Document* document = CPDFDocumentFromFPDFDocument(layer);
  CPDF_LayerDocument* layer_doc = CPDF_LayerDocument::FromDocument(document);
  if (!layer_doc || !file_write || layer_doc->InTransaction()) {
    return false;
  }

  if (layer_doc->GetPromotedObjectCount() == 0) {
    SetSaveStatus(out_status, EPDFLayerSaveStatus_kSuccess);
    return true;
  }

  if (!layer_doc->GetParser()) {
    return false;
  }
  if (layer_doc->GetLayerAppendBaseOffset() > kSafeNotionalStartOffsetMax) {
    SetSaveStatus(out_status, EPDFLayerSaveStatus_kAppendOnlyOffsetTooLarge);
    return false;
  }

  CPDF_Creator creator(
      layer_doc, pdfium::MakeRetain<CPDFSDK_FileWriteAdapter>(file_write));
  Mask<CPDF_Creator::CreateFlags> flags(
      CPDF_Creator::CreateFlags::kIncremental,
      CPDF_Creator::CreateFlags::kIncrementalAppendOnly);
  if (skip_if_unchanged) {
    flags |= CPDF_Creator::CreateFlags::kSkipIfUnchangedSinceLoad;
  }
  const bool ok = creator.Create(flags, /*file_version=*/0);
  if (ok) {
    SetChanged(out_changed_since_load, creator.changed_since_load());
    SetSaveStatus(out_status, EPDFLayerSaveStatus_kSuccess);
    return true;
  }

  if (creator.GetFailureReason() ==
      CPDF_Creator::FailureReason::kAppendOnlyOffsetTooLarge) {
    SetSaveStatus(out_status, EPDFLayerSaveStatus_kAppendOnlyOffsetTooLarge);
  }
  return false;
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFLayer_SaveDelta(FPDF_DOCUMENT layer,
                    FPDF_FILEWRITE* file_write,
                    EPDFLayerSaveStatus* out_status) {
  return SaveDeltaImpl(layer, file_write, out_status,
                       /*skip_if_unchanged=*/false,
                       /*out_changed_since_load=*/nullptr);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFLayer_SaveDeltaEx(FPDF_DOCUMENT layer,
                      FPDF_FILEWRITE* file_write,
                      EPDFLayerSaveStatus* out_status,
                      FPDF_BOOL* out_changed_since_load) {
  return SaveDeltaImpl(layer, file_write, out_status,
                       /*skip_if_unchanged=*/true, out_changed_since_load);
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDFLayer_SaveDeltaToOwnedBuffer(FPDF_DOCUMENT layer,
                                 unsigned long* out_size,
                                 EPDFLayerSaveStatus* out_status) {
  if (out_size) {
    *out_size = 0;
  }
  MemoryFileWriter writer;
  if (!EPDFLayer_SaveDelta(layer, &writer, out_status) || writer.data.empty()) {
    return nullptr;
  }
  return CopyToOwnedBuffer(pdfium::as_byte_span(writer.data), out_size);
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDFLayer_SaveDeltaToOwnedBufferEx(FPDF_DOCUMENT layer,
                                   unsigned long* out_size,
                                   EPDFLayerSaveStatus* out_status,
                                   FPDF_BOOL* out_changed_since_load) {
  if (out_size) {
    *out_size = 0;
  }
  MemoryFileWriter writer;
  if (!SaveDeltaImpl(layer, &writer, out_status, /*skip_if_unchanged=*/true,
                     out_changed_since_load) ||
      writer.data.empty()) {
    return nullptr;
  }
  return CopyToOwnedBuffer(pdfium::as_byte_span(writer.data), out_size);
}

namespace {

bool SaveLayerArtifactImpl(FPDF_DOCUMENT layer,
                           FPDF_FILEWRITE* file_write,
                           EPDFLayerSaveStatus* out_status,
                           bool skip_if_unchanged,
                           FPDF_BOOL* out_changed_since_load) {
  SetSaveStatus(out_status, EPDFLayerSaveStatus_kSaveFailed);
  SetChanged(out_changed_since_load, false);
  if (!file_write) {
    return false;
  }

  CPDF_Document* document = CPDFDocumentFromFPDFDocument(layer);
  CPDF_LayerDocument* layer_doc = CPDF_LayerDocument::FromDocument(document);
  CPDF_BaseDocument* base_doc =
      layer_doc ? layer_doc->GetBaseDocument() : nullptr;
  if (!layer_doc || !base_doc) {
    return false;
  }

  HashingTempFileWriter delta_writer;
  if (!delta_writer.IsValid()) {
    return false;
  }

  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  FPDF_BOOL changed = false;
  if (!SaveDeltaImpl(layer, &delta_writer, &save_status, skip_if_unchanged,
                     &changed)) {
    SetSaveStatus(out_status, save_status);
    return false;
  }
  SetChanged(out_changed_since_load, !!changed);
  if (skip_if_unchanged && !changed) {
    // Nothing changed since load: the artifact the layer was opened with
    // stands. Nothing written, by design.
    SetSaveStatus(out_status, EPDFLayerSaveStatus_kSuccess);
    return true;
  }

  std::optional<std::array<uint8_t, kSha256DigestSize>> delta_sha =
      delta_writer.FinishSha256();
  if (!delta_sha) {
    return false;
  }

  const std::vector<uint8_t> births =
      SerializeBirths(layer_doc->GetCommittedBirths());
  const std::vector<uint8_t> header = BuildLayerArtifactHeader(
      base_doc, delta_writer.size, *delta_sha, births.size());
  if (!WriteBytes(file_write, pdfium::span<const uint8_t>(header)) ||
      !delta_writer.ReplayTo(file_write) ||
      !WriteBytes(file_write, pdfium::span<const uint8_t>(births))) {
    return false;
  }

  SetSaveStatus(out_status, EPDFLayerSaveStatus_kSuccess);
  return true;
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFLayer_SaveLayerArtifact(FPDF_DOCUMENT layer,
                            FPDF_FILEWRITE* file_write,
                            EPDFLayerSaveStatus* out_status) {
  return SaveLayerArtifactImpl(layer, file_write, out_status,
                               /*skip_if_unchanged=*/false,
                               /*out_changed_since_load=*/nullptr);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFLayer_SaveLayerArtifactEx(FPDF_DOCUMENT layer,
                              FPDF_FILEWRITE* file_write,
                              EPDFLayerSaveStatus* out_status,
                              FPDF_BOOL* out_changed_since_load) {
  return SaveLayerArtifactImpl(layer, file_write, out_status,
                               /*skip_if_unchanged=*/true,
                               out_changed_since_load);
}

namespace {

void* SaveLayerArtifactToOwnedBufferImpl(FPDF_DOCUMENT layer,
                                         unsigned long* out_size,
                                         EPDFLayerSaveStatus* out_status,
                                         bool skip_if_unchanged,
                                         FPDF_BOOL* out_changed_since_load) {
  if (out_size) {
    *out_size = 0;
  }
  SetSaveStatus(out_status, EPDFLayerSaveStatus_kSaveFailed);
  SetChanged(out_changed_since_load, false);

  CPDF_Document* document = CPDFDocumentFromFPDFDocument(layer);
  CPDF_LayerDocument* layer_doc = CPDF_LayerDocument::FromDocument(document);
  CPDF_BaseDocument* base_doc =
      layer_doc ? layer_doc->GetBaseDocument() : nullptr;
  if (!layer_doc || !base_doc) {
    return nullptr;
  }

  MemoryFileWriter delta_writer;
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  FPDF_BOOL changed = false;
  if (!SaveDeltaImpl(layer, &delta_writer, &save_status, skip_if_unchanged,
                     &changed)) {
    SetSaveStatus(out_status, save_status);
    return nullptr;
  }
  SetChanged(out_changed_since_load, !!changed);
  if (skip_if_unchanged && !changed) {
    // Nothing changed since load: the artifact the layer was opened with
    // stands. Nothing written, by design.
    SetSaveStatus(out_status, EPDFLayerSaveStatus_kSuccess);
    return nullptr;
  }

  const DataVector<uint8_t> delta_bytes(delta_writer.data.begin(),
                                        delta_writer.data.end());
  std::optional<std::array<uint8_t, kSha256DigestSize>> delta_sha =
      ComputeDeltaSha256(
          pdfium::MakeRetain<OwnedReadOnlyMemoryStream>(delta_bytes).Get(),
          delta_bytes.size());
  if (!delta_sha) {
    return nullptr;
  }

  const std::vector<uint8_t> births =
      SerializeBirths(layer_doc->GetCommittedBirths());
  std::vector<uint8_t> artifact = BuildLayerArtifactHeader(
      base_doc, static_cast<uint64_t>(delta_writer.data.size()), *delta_sha,
      births.size());
  artifact.reserve(kLayerArtifactHeaderSize + delta_writer.data.size() +
                   births.size());
  artifact.insert(artifact.end(), delta_writer.data.begin(),
                  delta_writer.data.end());
  artifact.insert(artifact.end(), births.begin(), births.end());

  SetSaveStatus(out_status, EPDFLayerSaveStatus_kSuccess);
  return CopyToOwnedBuffer(pdfium::span(artifact), out_size);
}

}  // namespace

FPDF_EXPORT void* FPDF_CALLCONV
EPDFLayer_SaveLayerArtifactToOwnedBuffer(FPDF_DOCUMENT layer,
                                         unsigned long* out_size,
                                         EPDFLayerSaveStatus* out_status) {
  return SaveLayerArtifactToOwnedBufferImpl(layer, out_size, out_status,
                                            /*skip_if_unchanged=*/false,
                                            /*out_changed_since_load=*/nullptr);
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDFLayer_SaveLayerArtifactToOwnedBufferEx(FPDF_DOCUMENT layer,
                                           unsigned long* out_size,
                                           EPDFLayerSaveStatus* out_status,
                                           FPDF_BOOL* out_changed_since_load) {
  return SaveLayerArtifactToOwnedBufferImpl(layer, out_size, out_status,
                                            /*skip_if_unchanged=*/true,
                                            out_changed_since_load);
}
