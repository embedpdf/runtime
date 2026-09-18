// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/edit/cpdf_save_object_reader.h"

#include <utility>

#include "core/fpdfapi/parser/cpdf_base_document.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_parser.h"

namespace {

constexpr size_t kSaveObjectStreamCacheBytes = 8 * 1024 * 1024;

}  // namespace

CPDF_SaveObjectReader::CPDF_SaveObjectReader(CPDF_Document* document)
    : document_(document),
      parser_(document->GetParser()),
      stream_cache_(kSaveObjectStreamCacheBytes) {}

CPDF_SaveObjectReader::~CPDF_SaveObjectReader() = default;

RetainPtr<const CPDF_Object> CPDF_SaveObjectReader::Read(
    uint32_t object_number) {
  dependencies_.clear();
  GetByteStringPool()->Clear();
  return ReadObject(object_number);
}

RetainPtr<const CPDF_Object> CPDF_SaveObjectReader::ReadObject(
    uint32_t object_number) {
  if (const auto* layer = CPDF_LayerDocument::FromDocument(document_)) {
    if (auto promoted = layer->FindPromotedObject(object_number)) {
      return promoted;
    }
    // The base lookup is cache-only. The layer's GetIndirectObject() would
    // lazily populate the shared base cache on a miss.
    if (auto cached =
            layer->GetBaseDocument()->GetIndirectObject(object_number)) {
      return cached;
    }
  } else if (auto cached = document_->GetIndirectObject(object_number)) {
    return cached;
  }

  return parser_ ? parser_->ParseIndirectObjectForSave(object_number, this,
                                                       &stream_cache_)
                 : nullptr;
}

CPDF_Object* CPDF_SaveObjectReader::GetOrParseIndirectObjectInternal(
    uint32_t object_number) {
  auto it = dependencies_.find(object_number);
  if (it != dependencies_.end()) {
    return const_cast<CPDF_Object*>(it->second.Get());
  }

  auto object = ReadObject(object_number);
  if (!object) {
    return nullptr;
  }
  auto* result = const_cast<CPDF_Object*>(object.Get());
  dependencies_.emplace(object_number, std::move(object));
  return result;
}
