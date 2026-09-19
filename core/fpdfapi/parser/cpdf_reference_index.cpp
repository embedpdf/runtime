// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/parser/cpdf_reference_index.h"

#include <limits>

CPDF_ReferenceIndex::CPDF_ReferenceIndex(size_t byte_limit)
    : byte_limit_(byte_limit) {}

CPDF_ReferenceIndex::~CPDF_ReferenceIndex() = default;

std::optional<pdfium::span<const uint32_t>> CPDF_ReferenceIndex::Find(
    uint32_t number) const {
  auto page = rows_.find(number / kRowsPerPage);
  if (page == rows_.end()) {
    return std::nullopt;
  }
  const uint32_t entry = (*page->second)[number % kRowsPerPage];
  if (entry == 0) {
    return std::nullopt;
  }
  if (entry == 1) {
    return pdfium::span<const uint32_t>();
  }
  const uint32_t offset = entry - 2;
  const auto& words = edges_[offset / kWordsPerPage]->words;
  const size_t start = offset % kWordsPerPage;
  return pdfium::span(words).subspan(start + 1, words[start]);
}

bool CPDF_ReferenceIndex::Insert(uint32_t number,
                                 pdfium::span<const uint32_t> references) {
  if (Find(number)) {
    return true;
  }
  // A very wide object remains uncached rather than forcing a large allocation.
  if (references.size() >= kWordsPerPage) {
    return false;
  }
  auto row = rows_.find(number / kRowsPerPage);
  const bool new_row_page = row == rows_.end();
  const size_t words_needed = references.size() + 1;
  const bool new_edge_page =
      !references.empty() &&
      (edges_.empty() || edges_.back()->used + words_needed > kWordsPerPage);
  // Allow for map links, allocation metadata and vector spare capacity. These
  // charges are per page, not per object or reference.
  const size_t bytes_needed = (new_row_page ? sizeof(RowPage) + 128 : 0) +
                              (new_edge_page ? sizeof(EdgePage) + 128 : 0);
  if (bytes_needed > byte_limit_ - accounted_bytes_ ||
      edges_.size() >=
          (std::numeric_limits<uint32_t>::max() - 2) / kWordsPerPage) {
    return false;
  }
  if (new_row_page) {
    row =
        rows_.emplace(number / kRowsPerPage, std::make_unique<RowPage>()).first;
  }
  uint32_t entry = 1;
  if (!references.empty()) {
    if (new_edge_page) {
      edges_.push_back(std::make_unique<EdgePage>());
    }
    auto& page = *edges_.back();
    entry = static_cast<uint32_t>((edges_.size() - 1) * kWordsPerPage +
                                  page.used + 2);
    page.words[page.used++] = static_cast<uint32_t>(references.size());
    pdfium::span(page.words)
        .subspan(page.used, references.size())
        .copy_from(references);
    page.used += references.size();
  }
  (*row->second)[number % kRowsPerPage] = entry;
  accounted_bytes_ += bytes_needed;
  ++row_count_;
  return true;
}

void CPDF_ReferenceIndex::Clear() {
  rows_.clear();
  edges_.clear();
  edges_.shrink_to_fit();
  accounted_bytes_ = 0;
  row_count_ = 0;
}
