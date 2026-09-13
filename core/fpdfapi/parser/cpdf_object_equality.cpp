// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/parser/cpdf_object_equality.h"

#include <algorithm>

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"

namespace {

// A byte sink for CPDF_Object::WriteTo: the creator's own serialisation,
// captured instead of written.
class ByteSinkArchive final : public IFX_ArchiveStream {
 public:
  explicit ByteSinkArchive(DataVector<uint8_t>* out) : out_(out) {}
  ~ByteSinkArchive() override = default;

  bool WriteBlock(pdfium::span<const uint8_t> buffer) override {
    out_->insert(out_->end(), buffer.begin(), buffer.end());
    return true;
  }
  FX_FILESIZE CurrentOffset() const override {
    return static_cast<FX_FILESIZE>(out_->size());
  }

 private:
  DataVector<uint8_t>* const out_;
};

// No encryptor: both sides are in-memory plaintext (the parser decrypted
// them on load).
DataVector<uint8_t> CanonicalBytes(const CPDF_Object* object) {
  DataVector<uint8_t> out;
  ByteSinkArchive archive(&out);
  if (const CPDF_Stream* stream = object->AsStream()) {
    stream->GetDict()->WriteTo(&archive, /*encryptor=*/nullptr);
  } else {
    object->WriteTo(&archive, /*encryptor=*/nullptr);
  }
  return out;
}

bool SameRawData(const CPDF_Stream* a, const CPDF_Stream* b) {
  // The same view of the same immutable bytes: equal, in O(1). A view is
  // one byte range (never the whole underlying file), so identity of the
  // view object is identity of the bytes.
  RetainPtr<IFX_SeekableReadStream> view_a = a->BackingView();
  RetainPtr<IFX_SeekableReadStream> view_b = b->BackingView();
  if (view_a && view_a == view_b) {
    return true;
  }
  const size_t size = a->GetRawSize();
  if (size != b->GetRawSize()) {
    return false;
  }
  // Exact, in chunks: never two whole copies of a large stream in memory.
  static constexpr size_t kChunk = 64 * 1024;
  DataVector<uint8_t> x(kChunk);
  DataVector<uint8_t> y(kChunk);
  for (size_t offset = 0; offset < size;) {
    const size_t step = std::min(kChunk, size - offset);
    pdfium::span<uint8_t> sx = pdfium::span(x).first(step);
    pdfium::span<uint8_t> sy = pdfium::span(y).first(step);
    if (!a->ReadRawBlock(sx, static_cast<FX_FILESIZE>(offset)) ||
        !b->ReadRawBlock(sy, static_cast<FX_FILESIZE>(offset)) ||
        !std::equal(sx.begin(), sx.end(), sy.begin())) {
      return false;
    }
    offset += step;
  }
  return true;
}

}  // namespace

bool CPDF_SameEffectiveValue(const CPDF_Object* a, const CPDF_Object* b) {
  if (!a || !b || a->GetType() != b->GetType()) {
    return false;
  }
  if (CanonicalBytes(a) != CanonicalBytes(b)) {
    return false;
  }
  const CPDF_Stream* stream = a->AsStream();
  return !stream || SameRawData(stream, b->AsStream());
}
