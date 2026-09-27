// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/page/cpdf_decodedimagestore.h"

#include <iterator>
#include <tuple>
#include <utility>

#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/epdf_tls.h"
#include "core/fxge/dib/cfx_dibbase.h"

namespace {

// One store per PDFium instance: per thread in thread-confined builds, whose
// handles never cross threads.
EPDF_TLS CPDF_DecodedImageStore* g_decoded_image_store = nullptr;

size_t SizeOf(const CPDF_DecodedImageStore::Decoded& decoded) {
  size_t bytes = decoded.bitmap->GetEstimatedImageMemoryBurden();
  if (decoded.mask) {
    bytes += decoded.mask->GetEstimatedImageMemoryBurden();
  }
  return bytes;
}

}  // namespace

struct CPDF_DecodedImageStore::Entry {
  Entry(const Key& key,
        const Decoded& decoded,
        std::optional<uint8_t> jpx_levels,
        size_t bytes)
      : key(key), decoded(decoded), jpx_levels(jpx_levels), bytes(bytes) {}

  const Key key;
  const Decoded decoded;
  const std::optional<uint8_t> jpx_levels;
  const size_t bytes;
};

CPDF_DecodedImageStore::Key::Key(RetainPtr<const CPDF_Stream> stream,
                                 const CPDF_Document* document,
                                 RetainPtr<const CPDF_Dictionary> page_resources,
                                 bool std_cs,
                                 CPDF_ColorSpace::Family family,
                                 bool load_mask)
    : stream_(std::move(stream)),
      page_resources_(std::move(page_resources)),
      document_(reinterpret_cast<uintptr_t>(document)),
      std_cs_(std_cs),
      family_(family),
      load_mask_(load_mask) {}

CPDF_DecodedImageStore::Key::Key(const Key& that) = default;

CPDF_DecodedImageStore::Key& CPDF_DecodedImageStore::Key::operator=(
    const Key& that) = default;

CPDF_DecodedImageStore::Key::~Key() = default;

bool CPDF_DecodedImageStore::Key::operator<(const Key& that) const {
  return std::tie(stream_, page_resources_, document_, std_cs_, family_,
                  load_mask_) < std::tie(that.stream_, that.page_resources_,
                                         that.document_, that.std_cs_,
                                         that.family_, that.load_mask_);
}

CPDF_DecodedImageStore::Decoded::Decoded() = default;

CPDF_DecodedImageStore::Decoded::Decoded(const Decoded& that) = default;

CPDF_DecodedImageStore::Decoded::~Decoded() = default;

// static
void CPDF_DecodedImageStore::Create() {
  CHECK(!g_decoded_image_store);
  g_decoded_image_store = new CPDF_DecodedImageStore();
}

// static
void CPDF_DecodedImageStore::Destroy() {
  delete g_decoded_image_store;
  g_decoded_image_store = nullptr;
}

// static
CPDF_DecodedImageStore* CPDF_DecodedImageStore::Get() {
  return g_decoded_image_store;
}

// static
std::optional<CPDF_DecodedImageStore::Key> CPDF_DecodedImageStore::KeyFor(
    const CPDF_Image& image,
    const CPDF_Dictionary* page_resources,
    bool std_cs,
    CPDF_ColorSpace::Family family,
    bool load_mask) {
  CPDF_DecodedImageStore* store = Get();
  // An inline image is a new object each time its page is parsed, so it would
  // never be found again.
  if (!store || !store->budget_ || image.IsInline()) {
    return std::nullopt;
  }
  RetainPtr<const CPDF_Stream> stream = image.GetStream();
  if (!stream) {
    return std::nullopt;
  }
  return Key(std::move(stream), image.GetDocument(),
             pdfium::WrapRetain(page_resources), std_cs, family, load_mask);
}

CPDF_DecodedImageStore::CPDF_DecodedImageStore() = default;

CPDF_DecodedImageStore::~CPDF_DecodedImageStore() = default;

void CPDF_DecodedImageStore::SetBudget(size_t bytes) {
  budget_ = bytes;
  EvictTo(budget_);
}

std::optional<CPDF_DecodedImageStore::Decoded> CPDF_DecodedImageStore::Find(
    const Key& key,
    uint8_t jpx_levels_to_skip) {
  auto it = index_.find(Slot(key, std::nullopt));
  if (it == index_.end()) {
    it = index_.find(Slot(key, jpx_levels_to_skip));
  }
  if (it == index_.end()) {
    return std::nullopt;
  }
  entries_.splice(entries_.begin(), entries_, it->second);
  return it->second->decoded;
}

void CPDF_DecodedImageStore::Insert(const Key& key,
                                    const Decoded& decoded,
                                    std::optional<uint8_t> jpx_levels) {
  CHECK(decoded.bitmap);
  const size_t bytes = SizeOf(decoded);
  if (!budget_ || bytes > budget_) {
    return;
  }
  const auto it = index_.find(Slot(key, jpx_levels));
  if (it != index_.end()) {
    Erase(it->second);
  }
  EvictTo(budget_ - bytes);
  entries_.emplace_front(key, decoded, jpx_levels, bytes);
  index_.emplace(Slot(key, jpx_levels), entries_.begin());
  bytes_ += bytes;
}

void CPDF_DecodedImageStore::DropDocument(const CPDF_Document* document) {
  const uintptr_t id = reinterpret_cast<uintptr_t>(document);
  for (auto it = entries_.begin(); it != entries_.end();) {
    auto next = std::next(it);
    if (it->key.document() == id) {
      Erase(it);
    }
    it = next;
  }
}

void CPDF_DecodedImageStore::DropStream(const CPDF_Stream* stream) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    auto next = std::next(it);
    if (it->key.stream() == stream) {
      Erase(it);
    }
    it = next;
  }
}

void CPDF_DecodedImageStore::EvictTo(size_t bytes) {
  while (bytes_ > bytes) {
    Erase(std::prev(entries_.end()));
  }
}

void CPDF_DecodedImageStore::Erase(EntryList::iterator it) {
  bytes_ -= it->bytes;
  index_.erase(Slot(it->key, it->jpx_levels));
  entries_.erase(it);
}
