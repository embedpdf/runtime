// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/parser/cpdf_string.h"

#include <stdint.h>

#include <optional>
#include <utility>

#include "core/fpdfapi/parser/cpdf_encryptor.h"
#include "core/fpdfapi/parser/cpdf_read_only_graph_guard.h"
#include "core/fpdfapi/parser/cpdf_write_generation.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_stream.h"

CPDF_String::CPDF_String() = default;

CPDF_String::CPDF_String(pdfium::span<const uint8_t> data, DataType is_hex)
    : data_(ByteStringView(data)), output_is_hex_(true) {}

CPDF_String::CPDF_String(const ByteString& str) : data_(str) {}

CPDF_String::CPDF_String(WideStringView str)
    : CPDF_String(PDF_EncodeText(str)) {
  // Delegates to ctor above.
}

CPDF_String::~CPDF_String() = default;

CPDF_Object::Type CPDF_String::GetType() const {
  return kString;
}

RetainPtr<CPDF_Object> CPDF_String::Clone() const {
  auto clone = pdfium::MakeRetain<CPDF_String>();
  clone->data_ = data_;
  clone->output_is_hex_ = output_is_hex_;
  return clone;
}

ByteString CPDF_String::GetString() const {
  return data_;
}

void CPDF_String::SetString(const ByteString& str) {
  // EmbedPDF(write-generation, reads-dont-write): a write past the layer's
  // door, or one during a read-only walk, is a bug.
  DCHECK_PDF_GRAPH_MUTABLE_FOR(this);
  DCHECK_PDF_WRITABLE(this);
  data_ = str;
}

CPDF_String* CPDF_String::AsMutableString() {
  return this;
}

WideString CPDF_String::GetUnicodeText() const {
  return PDF_DecodeText(data_.unsigned_span());
}

// EmbedPDF(save-generations): writers carry the write context.
bool CPDF_String::WriteTo(IFX_ArchiveStream* archive,
                          const CPDF_Encryptor* encryptor,
                          const CPDF_WriteContext* context) const {
  std::optional<DataVector<uint8_t>> encrypted_data;
  pdfium::span<const uint8_t> data = data_.unsigned_span();
  if (encryptor) {
    // EmbedPDF(save-security): a string that can't be encrypted fails the save.
    encrypted_data = encryptor->Encrypt(data);
    if (!encrypted_data) {
      return false;
    }
    data = *encrypted_data;
  }
  ByteStringView raw(data);
  ByteString content =
      output_is_hex_ ? PDF_HexEncodeString(raw) : PDF_EncodeString(raw);
  return archive->WriteString(content.AsStringView());
}

ByteString CPDF_String::EncodeString() const {
  return output_is_hex_ ? PDF_HexEncodeString(data_.AsStringView())
                        : PDF_EncodeString(data_.AsStringView());
}
