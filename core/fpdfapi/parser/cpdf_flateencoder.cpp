// Copyright 2017 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/parser/cpdf_flateencoder.h"

#include <variant>

#include "constants/stream_dict_common.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fxcodec/flate/flatemodule.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/numerics/safe_conversions.h"

CPDF_FlateEncoder::CPDF_FlateEncoder(RetainPtr<const CPDF_Stream> pStream,
                                     bool bFlateEncode)
    : acc_(pdfium::MakeRetain<CPDF_StreamAcc>(pStream)) {
  acc_->LoadAllDataRaw();

  bool bHasFilter = pStream->HasFilter();
  if (bHasFilter && !bFlateEncode) {
    // EmbedPDF(bug-fix): a stream written unencoded (the metadata stream,
    // which XMP readers look for in the file's bytes) is written decoded.
    // Upstream decoded it, then wrote its encoded bytes without the filter
    // that decodes them. Bytes that don't decode stay as they are, filter and
    // all.
    std::optional<DecoderArray> decoders = GetDecoderArray(pStream->GetDict());
    std::optional<PDFDataDecodeResult> decoded;
    if (decoders.has_value() && !decoders.value().empty()) {
      decoded = PDF_DataDecode(acc_->GetSpan(), 0, false, decoders.value());
    }
    if (decoded.has_value() && decoded.value().image_encoding.IsEmpty() &&
        !decoded.value().data.empty()) {
      data_ = std::move(decoded.value().data);
      cloned_dict_ = ToDictionary(pStream->GetDict()->Clone());
      cloned_dict_->RemoveFor("Filter");
      cloned_dict_->RemoveFor(pdfium::stream::kDecodeParms);
      DCHECK(!dict_);
      return;
    }
  }
  if (bHasFilter || !bFlateEncode) {
    data_ = acc_->GetSpan();
    dict_ = pStream->GetDict();
    DCHECK(!cloned_dict_);
    return;
  }

  data_ = FlateModule::Encode(acc_->GetSpan());
  CHECK(!GetSpan().empty());
  cloned_dict_ = ToDictionary(pStream->GetDict()->Clone());
  cloned_dict_->SetNewFor<CPDF_Number>(
      "Length", pdfium::checked_cast<int>(GetSpan().size()));
  cloned_dict_->SetNewFor<CPDF_Name>("Filter", "FlateDecode");
  cloned_dict_->RemoveFor(pdfium::stream::kDecodeParms);
  DCHECK(!dict_);
}

CPDF_FlateEncoder::~CPDF_FlateEncoder() = default;

void CPDF_FlateEncoder::UpdateLength(size_t size) {
  if (static_cast<size_t>(GetDict()->GetIntegerFor("Length")) == size) {
    return;
  }

  if (!cloned_dict_) {
    cloned_dict_ = ToDictionary(dict_->Clone());
    dict_.Reset();
  }
  DCHECK(cloned_dict_);
  DCHECK(!dict_);
  cloned_dict_->SetNewFor<CPDF_Number>("Length", static_cast<int>(size));
}

// EmbedPDF(save-generations): writers carry the write context.
bool CPDF_FlateEncoder::WriteDictTo(IFX_ArchiveStream* archive,
                                    const CPDF_Encryptor* encryptor,
                                    const CPDF_WriteContext* context) const {
  return GetDict()->WriteTo(archive, encryptor, context);
}

const CPDF_Dictionary* CPDF_FlateEncoder::GetDict() const {
  if (cloned_dict_) {
    DCHECK(!dict_);
    return cloned_dict_.Get();
  }
  return dict_.Get();
}

pdfium::span<const uint8_t> CPDF_FlateEncoder::GetSpan() const {
  if (is_owned()) {
    return std::get<DataVector<uint8_t>>(data_);
  }
  return std::get<pdfium::raw_span<const uint8_t>>(data_);
}
