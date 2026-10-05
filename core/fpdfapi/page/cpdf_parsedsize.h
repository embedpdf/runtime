// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PAGE_CPDF_PARSEDSIZE_H_
#define CORE_FPDFAPI_PAGE_CPDF_PARSEDSIZE_H_

#include <stddef.h>
#include <stdint.h>

#include "core/fxcrt/unowned_ptr.h"

class CFX_Path;
class CPDF_PageObject;
class CPDF_PageObjectHolder;

// What a page's parsed objects cost in memory, nested forms included. A page
// counts its objects as its parse adds them, so the count is there at any
// point of the parse at no cost; after its objects change, the page counts
// them again (CPDF_Page::GetParsedSize()).
//
// The bytes are an estimate, meant to be consistent and on the safe side:
// each object's own size, the points of its path, the codes and positions of
// its text and the bytes of an inline image, with an allocator's overhead per
// allocation. What objects share is not counted: graphics states, clip paths,
// fonts and images the document caches belong to the document.
struct CPDF_ParsedSize {
  // Counts the objects of one holder in order. The parser shares one path's
  // points with the next path when they are equal, so a path holding the same
  // points as the path before it in its holder adds no points of its own.
  class Counter {
   public:
    // Counts into `size`; counts nothing when `size` is null.
    explicit Counter(CPDF_ParsedSize* size);
    ~Counter();

    // Adds `object` itself. A form object adds its form, not the form's
    // objects: those are counted as they are parsed into the form.
    void Add(const CPDF_PageObject& object);

   private:
    UnownedPtr<CPDF_ParsedSize> const size_;
    // The points of the last path counted, compared by identity only.
    UnownedPtr<const CFX_Path> last_path_;
  };

  // Every object of `holder` and of the forms in it, at any depth.
  static CPDF_ParsedSize Of(const CPDF_PageObjectHolder& holder);

  bool operator==(const CPDF_ParsedSize& that) const = default;

  size_t objects = 0;
  size_t path_points = 0;
  size_t text_chars = 0;
  uint64_t estimated_bytes = 0;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_PARSEDSIZE_H_
