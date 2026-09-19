// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/parser/cpdf_object_stream_cache.h"

#include <utility>

#include "core/fpdfapi/parser/cpdf_object_stream.h"
#include "core/fxcrt/check.h"

CPDF_ObjectStreamCache::CPDF_ObjectStreamCache(size_t byte_limit)
    : byte_limit_(byte_limit) {}

CPDF_ObjectStreamCache::~CPDF_ObjectStreamCache() = default;

std::shared_ptr<const CPDF_ObjectStream> CPDF_ObjectStreamCache::Get(
    uint32_t object_number) {
  auto it = entries_.find(object_number);
  if (it == entries_.end()) {
    return nullptr;
  }

  recent_objects_.splice(recent_objects_.end(), recent_objects_,
                         it->second.recency);
  return it->second.stream;
}

void CPDF_ObjectStreamCache::Put(
    uint32_t object_number,
    std::shared_ptr<const CPDF_ObjectStream> stream) {
  CHECK(stream);
  CHECK(!entries_.contains(object_number));

  const size_t bytes = stream->GetRetainedSize();
  while (!entries_.empty() &&
         (bytes > byte_limit_ || retained_bytes_ > byte_limit_ - bytes)) {
    auto oldest = entries_.find(recent_objects_.front());
    retained_bytes_ -= oldest->second.bytes;
    recent_objects_.pop_front();
    entries_.erase(oldest);
  }

  recent_objects_.push_back(object_number);
  entries_.emplace(
      object_number,
      Entry{std::move(stream), std::prev(recent_objects_.end()), bytes});
  retained_bytes_ += bytes;
}
