// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PAGE_CPDF_DECODEDIMAGESTORE_H_
#define CORE_FPDFAPI_PAGE_CPDF_DECODEDIMAGESTORE_H_

#include <stddef.h>
#include <stdint.h>

#include <list>
#include <map>
#include <optional>
#include <utility>

#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fxcrt/retain_ptr.h"

class CFX_DIBBase;
class CPDF_Dictionary;
class CPDF_Document;
class CPDF_Image;
class CPDF_Stream;

// Decoded images kept across page loads, for every document of this PDFium
// instance (one per thread in thread-confined builds), so an image rendered
// again after its page's image cache was emptied is not decoded again. A page's image cache consults the store only where it would
// decode, and a stored decode leaves the cache as a new decode would.
//
// The store is off until a budget is set. Its owner must empty it (a budget
// of 0) before anything a decode depends on can change: the image streams,
// their colour spaces and the resources that name them.
class CPDF_DecodedImageStore {
 public:
  // What a decode depends on besides its stream's contents and, for JPX, the
  // resolution levels it skips.
  class Key {
   public:
    Key(RetainPtr<const CPDF_Stream> stream,
        const CPDF_Document* document,
        RetainPtr<const CPDF_Dictionary> page_resources,
        bool std_cs,
        CPDF_ColorSpace::Family family,
        bool load_mask);
    Key(const Key& that);
    Key& operator=(const Key& that);
    ~Key();

    bool operator<(const Key& that) const;

    const CPDF_Stream* stream() const { return stream_.Get(); }
    uintptr_t document() const { return document_; }

   private:
    // Held, so no other object can take the addresses compared here.
    RetainPtr<const CPDF_Stream> stream_;
    RetainPtr<const CPDF_Dictionary> page_resources_;
    // The document's address: only compared, never used.
    uintptr_t document_;
    bool std_cs_;
    CPDF_ColorSpace::Family family_;
    bool load_mask_;
  };

  struct Decoded {
    Decoded();
    Decoded(const Decoded& that);
    ~Decoded();

    RetainPtr<CFX_DIBBase> bitmap;
    RetainPtr<CFX_DIBBase> mask;
    uint32_t matte_color = 0;
  };

  static void Create();
  static void Destroy();
  static CPDF_DecodedImageStore* Get();

  // The key for decoding `image` with these arguments to
  // CPDF_DIB::StartLoadDIBBase(), or nullopt when the store is off or does not
  // keep such an image.
  static std::optional<Key> KeyFor(const CPDF_Image& image,
                                   const CPDF_Dictionary* page_resources,
                                   bool std_cs,
                                   CPDF_ColorSpace::Family family,
                                   bool load_mask);

  // Sets the most bytes kept, dropping the least recently used decodes above
  // it. 0 empties the store and turns it off.
  void SetBudget(size_t bytes);
  size_t bytes() const { return bytes_; }

  // Whether a decode of `bytes` bytes may be kept.
  bool Fits(size_t bytes) const { return bytes <= budget_; }

  // The decode kept for `key`. JPX decodes are kept per resolution levels
  // skipped, and only one that skipped `jpx_levels_to_skip` is found.
  std::optional<Decoded> Find(const Key& key, uint8_t jpx_levels_to_skip);

  // Keeps `decoded` for `key`, replacing what was kept for it. `jpx_levels`
  // holds the resolution levels a JPX decode skipped, and is empty for every
  // other decoder, whose output does not depend on them.
  void Insert(const Key& key,
              const Decoded& decoded,
              std::optional<uint8_t> jpx_levels);

  void DropDocument(const CPDF_Document* document);
  void DropStream(const CPDF_Stream* stream);

 private:
  struct Entry;
  using EntryList = std::list<Entry>;
  // A key and, for a JPX decode, the resolution levels it skipped.
  using Slot = std::pair<Key, std::optional<uint8_t>>;

  CPDF_DecodedImageStore();
  ~CPDF_DecodedImageStore();

  void EvictTo(size_t bytes);
  void Erase(EntryList::iterator it);

  // Most recently used first.
  EntryList entries_;
  std::map<Slot, EntryList::iterator> index_;
  size_t budget_ = 0;
  size_t bytes_ = 0;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_DECODEDIMAGESTORE_H_
