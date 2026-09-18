// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PARSER_CPDF_REFERENCE_INDEX_H_
#define CORE_FPDFAPI_PARSER_CPDF_REFERENCE_INDEX_H_

#include <stdint.h>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "core/fxcrt/span.h"

// A bounded memo of outgoing references in immutable file objects. It contains
// no CPDF_Objects or layer state. Missing rows always fall back to parsing.
// Sparse row pages and packed edge pages avoid an allocation per object.
class CPDF_ReferenceIndex {
 public:
  static constexpr size_t kDefaultByteLimit = 32 * 1024 * 1024;

  explicit CPDF_ReferenceIndex(size_t byte_limit = kDefaultByteLimit);
  ~CPDF_ReferenceIndex();

  // A present, empty span means "parsed, no references". nullopt means unknown.
  // Spans remain valid until Clear(), including across subsequent Insert().
  std::optional<pdfium::span<const uint32_t>> Find(uint32_t number) const;
  bool Insert(uint32_t number, pdfium::span<const uint32_t> references);
  void Clear();

  // Includes page storage and a conservative allowance for container nodes.
  size_t accounted_bytes() const { return accounted_bytes_; }
  size_t row_count() const { return row_count_; }

 private:
  static constexpr size_t kRowsPerPage = 4096;
  static constexpr size_t kWordsPerPage = 16384;
  using RowPage = std::array<uint32_t, kRowsPerPage>;
  struct EdgePage {
    std::array<uint32_t, kWordsPerPage> words;
    size_t used = 0;
  };

  const size_t byte_limit_;
  size_t accounted_bytes_ = 0;
  size_t row_count_ = 0;
  std::map<uint32_t, std::unique_ptr<RowPage>> rows_;
  std::vector<std::unique_ptr<EdgePage>> edges_;
};

#endif  // CORE_FPDFAPI_PARSER_CPDF_REFERENCE_INDEX_H_
