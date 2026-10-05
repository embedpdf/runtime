// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FXCRT_CFX_SHARED_BYTES_H_
#define CORE_FXCRT_CFX_SHARED_BYTES_H_

#include <stddef.h>
#include <stdint.h>

#include <utility>

#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"

// Bytes that several owners share and nobody changes: an owner that needs
// different bytes replaces its buffer instead of writing into this one. A
// stream copy (CPDF_Stream::CloneForHolder()) shares its source's in-memory
// bytes this way, so copying a stream costs its dictionary, not its data.
//
// The reference count isn't atomic: share only between objects one thread
// owns, as everything a document and its layers hold already is.
class CFX_SharedBytes final : public Retainable {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  pdfium::span<const uint8_t> span() const { return data_; }
  size_t size() const { return data_.size(); }

 private:
  explicit CFX_SharedBytes(DataVector<uint8_t> data) : data_(std::move(data)) {}
  ~CFX_SharedBytes() override = default;

  const DataVector<uint8_t> data_;
};

#endif  // CORE_FXCRT_CFX_SHARED_BYTES_H_
