// Copyright 2017 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/parser/cpdf_encryptor.h"

#include <stdint.h>

#include "core/fpdfapi/parser/cpdf_crypto_handler.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/data_vector.h"

// EmbedPDF(save-generations): the encryptor carries the object's generation.
CPDF_Encryptor::CPDF_Encryptor(const CPDF_CryptoHandler* pHandler,
                               int objnum,
                               uint32_t generation)
    : handler_(pHandler), obj_num_(objnum), generation_(generation) {
  DCHECK(handler_);
}

// EmbedPDF(save-security): nullopt when the content can't be encrypted.
std::optional<DataVector<uint8_t>> CPDF_Encryptor::Encrypt(
    pdfium::span<const uint8_t> src_data) const {
  if (src_data.empty()) {
    return DataVector<uint8_t>();
  }
  // EmbedPDF(save-generations): encrypt with the object's own generation,
  // not 0.
  return handler_->EncryptContent(obj_num_, generation_, src_data);
}

CPDF_Encryptor::~CPDF_Encryptor() = default;
