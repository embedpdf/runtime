// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/page/cpdf_parsedsize.h"

#include <memory>
#include <vector>

#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_shadingobject.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxge/cfx_path.h"

namespace {

// What an allocator adds to every allocation: its header and rounding.
constexpr uint64_t kAllocationOverhead = 16;

// A heap allocation of `bytes`.
constexpr uint64_t Allocation(uint64_t bytes) {
  return bytes + kAllocationOverhead;
}

// The holder's list keeps a pointer to every object.
constexpr uint64_t kListEntry = sizeof(std::unique_ptr<CPDF_PageObject>);

}  // namespace

CPDF_ParsedSize::Counter::Counter(CPDF_ParsedSize* size) : size_(size) {}

CPDF_ParsedSize::Counter::~Counter() = default;

void CPDF_ParsedSize::Counter::Add(const CPDF_PageObject& object) {
  if (!size_) {
    return;
  }
  ++size_->objects;
  uint64_t bytes = kListEntry;
  switch (object.GetType()) {
    case CPDF_PageObject::Type::kText: {
      const auto* text = object.AsText();
      const size_t codes = text->GetCharCodes().size();
      size_->text_chars += codes;
      bytes += Allocation(sizeof(CPDF_TextObject));
      if (codes) {
        bytes += Allocation(codes * sizeof(uint32_t));
      }
      if (!text->GetCharPositions().empty()) {
        bytes += Allocation(text->GetCharPositions().size() * sizeof(float));
      }
      break;
    }
    case CPDF_PageObject::Type::kPath: {
      bytes += Allocation(sizeof(CPDF_PathObject));
      const CFX_Path* path = object.AsPath()->path().GetObject();
      if (path && path != last_path_) {
        const size_t points = path->GetPoints().size();
        size_->path_points += points;
        bytes += Allocation(sizeof(CFX_RetainablePath));
        if (points) {
          bytes += Allocation(points * sizeof(CFX_Path::Point));
        }
      }
      last_path_ = path;
      break;
    }
    case CPDF_PageObject::Type::kImage: {
      bytes += Allocation(sizeof(CPDF_ImageObject));
      // An image the document caches is shared; an inline image is this
      // object's own, bytes and all.
      RetainPtr<CPDF_Image> image = object.AsImage()->GetImage();
      if (image && image->IsInline()) {
        bytes += Allocation(sizeof(CPDF_Image));
        RetainPtr<const CPDF_Stream> stream = image->GetStream();
        if (stream) {
          bytes += Allocation(stream->GetRawSize());
        }
      }
      break;
    }
    case CPDF_PageObject::Type::kShading:
      bytes += Allocation(sizeof(CPDF_ShadingObject));
      break;
    case CPDF_PageObject::Type::kForm:
      bytes +=
          Allocation(sizeof(CPDF_FormObject)) + Allocation(sizeof(CPDF_Form));
      break;
  }
  size_->estimated_bytes += bytes;
}

// static
CPDF_ParsedSize CPDF_ParsedSize::Of(const CPDF_PageObjectHolder& holder) {
  CPDF_ParsedSize size;
  std::vector<const CPDF_PageObjectHolder*> holders = {&holder};
  while (!holders.empty()) {
    const CPDF_PageObjectHolder* current = holders.back();
    holders.pop_back();
    Counter counter(&size);
    for (const auto& object : *current) {
      counter.Add(*object);
      if (const CPDF_FormObject* form = object->AsForm()) {
        holders.push_back(form->form());
      }
    }
  }
  return size;
}
