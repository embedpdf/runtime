// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/page/cpdf_page.h"

#include <limits>
#include <set>
#include <utility>
#include <vector>

#include "constants/page_object.h"
#include "core/fpdfapi/page/cpdf_contentparser.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_pageimagecache.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/containers/contains.h"

namespace {

// A content check made at no epoch: the next one compares again.
constexpr uint64_t kContentUnchecked = std::numeric_limits<uint64_t>::max();

}  // namespace

CPDF_Page::CPDF_Page(CPDF_Document* document,
                     RetainPtr<CPDF_Dictionary> pPageDict)
    : CPDF_PageObjectHolder(document, std::move(pPageDict), nullptr, nullptr),
      page_size_(100, 100),
      pdf_document_(document) {
  CPDF_DocumentViewScope document_view(document);

  // Cannot initialize |resources_| and |page_resources_| via the
  // CPDF_PageObjectHolder ctor because GetPageAttr() requires
  // CPDF_PageObjectHolder to finish initializing first.
  RetainPtr<const CPDF_Object> pPageAttr =
      GetPageAttr(pdfium::page_object::kResources);
  resources_ = pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(
      pPageAttr ? pPageAttr->GetDict().Get() : nullptr));
  page_resources_ = resources_;

  UpdateDimensions();
  transparency_.SetIsolated();
  LoadTransparencyInfo();
  content_checked_epoch_ = document ? document->GetOverlayEpoch() : 0;
}

CPDF_Page::~CPDF_Page() = default;

CPDF_Page* CPDF_Page::AsPDFPage() {
  return this;
}

CPDFXFA_Page* CPDF_Page::AsXFAPage() {
  return nullptr;
}

CPDF_Document* CPDF_Page::GetDocument() const {
  return pdf_document_;
}

float CPDF_Page::GetPageWidth() const {
  return page_size_.width;
}

float CPDF_Page::GetPageHeight() const {
  return page_size_.height;
}

bool CPDF_Page::IsPage() const {
  return true;
}

RetainPtr<const CPDF_Dictionary> CPDF_Page::GetResources() const {
  RefreshResourcesIfNeeded();
  return CPDF_PageObjectHolder::GetResources();
}

RetainPtr<const CPDF_Dictionary> CPDF_Page::GetPageResources() const {
  RefreshResourcesIfNeeded();
  return CPDF_PageObjectHolder::GetPageResources();
}

void CPDF_Page::ParseContent() {
  CPDF_DocumentViewScope document_view(GetDocument());

  if (GetParseState() == ParseState::kParsed) {
    return;
  }

  if (GetParseState() == ParseState::kNotParsed) {
    StartParse(std::make_unique<CPDF_ContentParser>(this));
  }

  DCHECK_EQ(GetParseState(), ParseState::kParsing);
  ContinueParse(nullptr);
}

const CPDF_ParsedSize& CPDF_Page::GetParsedSize() const {
  if (GetParseState() == ParseState::kParsed &&
      parsed_size_edits_ != GetObjectEdits()) {
    parsed_size_ = CPDF_ParsedSize::Of(*this);
    parsed_size_edits_ = GetObjectEdits();
  }
  return parsed_size_;
}

bool CPDF_Page::IsContentCurrent() const {
  CPDF_Document* document = GetDocument();
  if (!document) {
    return true;
  }
  // Nothing makes a version without moving the epoch.
  const uint64_t epoch = document->GetOverlayEpoch();
  if (content_checked_epoch_ == epoch) {
    return content_current_;
  }

  CPDF_DocumentViewScope document_view(document);
  content_current_ = GetBox(pdfium::page_object::kMediaBox) == read_mediabox_ &&
                     GetBox(pdfium::page_object::kCropBox) == read_cropbox_ &&
                     GetOriginalRotation() == read_rotation_ &&
                     content_versions_.Match(document, GetDict().Get());
  content_checked_epoch_ = epoch;
  return content_current_;
}

void CPDF_Page::StartContentRecords() {
  parsed_size_ = CPDF_ParsedSize();
  parsed_size_edits_ = GetObjectEdits();
  content_versions_.Clear();
  content_checked_epoch_ = GetDocument()->GetOverlayEpoch();
  content_current_ = true;
}

void CPDF_Page::ContentGenerated(bool was_current) {
  NoteObjectsEdited();
  if (!was_current) {
    return;
  }

  // The generator wrote this page's /Contents and the stream of every form it
  // regenerated; every other form still holds the version it was parsed from,
  // which matched.
  CPDF_DocumentViewScope document_view(GetDocument());
  content_versions_.Clear();
  for (auto& stream : CPDF_ContentVersions::ContentStreamsOf(GetDict().Get())) {
    content_versions_.AddContentStream(std::move(stream));
  }
  std::vector<const CPDF_PageObjectHolder*> holders = {this};
  while (!holders.empty()) {
    const CPDF_PageObjectHolder* holder = holders.back();
    holders.pop_back();
    for (const auto& object : *holder) {
      if (const CPDF_FormObject* form_object = object->AsForm()) {
        content_versions_.AddFormStream(form_object->form()->GetParsedStream());
        holders.push_back(form_object->form());
      }
    }
  }
  content_checked_epoch_ = GetDocument()->GetOverlayEpoch();
  content_current_ = true;
}

RetainPtr<CPDF_Object> CPDF_Page::GetMutablePageAttr(ByteStringView name) {
  return pdfium::WrapRetain(const_cast<CPDF_Object*>(GetPageAttr(name).Get()));
}

void CPDF_Page::EnsureMutableBackingObjectForResources() {
  RetainPtr<CPDF_Dictionary> page_dict = GetMutableDict();
  if (GetDocument() && GetDocument()->IsLayerDocument() &&
      !page_dict->KeyExist(pdfium::page_object::kResources) && resources_) {
    page_dict->SetFor(pdfium::page_object::kResources,
                      resources_->CloneDirectObject());
  }
  resources_ = page_dict->GetMutableDictFor(pdfium::page_object::kResources);
}

void CPDF_Page::EnsureMutableBackingObjectForPageResources() {
  EnsureMutableBackingObjectForResources();
  page_resources_ = resources_;
}

void CPDF_Page::RefreshResourcesIfNeeded() const {
  CPDF_Document* document = GetDocument();
  if (!document) {
    return;
  }

  const uint64_t current_epoch = document->GetOverlayEpoch();
  if (resources_epoch_ == current_epoch &&
      page_resources_epoch_ == current_epoch) {
    return;
  }

  RetainPtr<const CPDF_Object> page_attr =
      GetPageAttr(pdfium::page_object::kResources);
  RetainPtr<const CPDF_Dictionary> effective_resources =
      page_attr ? page_attr->GetDict() : nullptr;
  auto* mutable_this = const_cast<CPDF_Page*>(this);
  mutable_this->resources_ = pdfium::WrapRetain(
      const_cast<CPDF_Dictionary*>(effective_resources.Get()));
  mutable_this->page_resources_ = mutable_this->resources_;
  resources_epoch_ = current_epoch;
  page_resources_epoch_ = current_epoch;
}

RetainPtr<const CPDF_Object> CPDF_Page::GetPageAttr(ByteStringView name) const {
  CPDF_DocumentViewScope document_view(GetDocument());

  std::set<RetainPtr<const CPDF_Dictionary>> visited;
  RetainPtr<const CPDF_Dictionary> pPageDict = GetDict();
  while (pPageDict && !pdfium::Contains(visited, pPageDict)) {
    RetainPtr<const CPDF_Object> pObj = pPageDict->GetDirectObjectFor(name);
    if (pObj) {
      return pObj;
    }

    visited.insert(pPageDict);
    pPageDict = pPageDict->GetDictFor(pdfium::page_object::kParent);
  }
  return nullptr;
}

CFX_FloatRect CPDF_Page::GetBox(ByteStringView name) const {
  CFX_FloatRect box;
  RetainPtr<const CPDF_Array> pBox = ToArray(GetPageAttr(name));
  if (pBox) {
    box = pBox->GetRect();
    box.Normalize();
  }
  return box;
}

std::optional<CFX_PointF> CPDF_Page::DeviceToPage(
    const FX_RECT& rect,
    int rotation,
    const CFX_PointF& device_point) const {
  CFX_Matrix page2device = GetDisplayMatrixForRect(rect, rotation);
  return page2device.GetInverse().Transform(device_point);
}

std::optional<CFX_PointF> CPDF_Page::PageToDevice(
    const FX_RECT& rect,
    int rotation,
    const CFX_PointF& page_point) const {
  CFX_Matrix page2device = GetDisplayMatrixForRect(rect, rotation);
  return page2device.Transform(page_point);
}

CFX_Matrix CPDF_Page::GetDisplayMatrixForRect(const FX_RECT& rect,
                                              int rotation) const {
  return GetDisplayMatrixForFloatRect(CFX_FloatRect(rect), rotation);
}

CFX_Matrix CPDF_Page::GetDisplayMatrixForFloatRect(const CFX_FloatRect& rect,
                                                   int rotation) const {
  if (page_size_.width == 0 || page_size_.height == 0) {
    return CFX_Matrix();
  }

  float x0;
  float y0;
  float x1;
  float y1;
  float x2;
  float y2;
  // This code implicitly inverts the y-axis to account for page coordinates
  // pointing up and bitmap coordinates pointing down. (x0, y0) is the base
  // point, (x1, y1) is that point translated on y and (x2, y2) is the point
  // translated on x. On rotation = 0, y0 is rect.top and the translation to get
  // y1 is performed as negative. This results in the desired transformation.
  switch (rotation % 4) {
    case 0:
      x0 = rect.left;
      y0 = rect.top;
      x1 = rect.left;
      y1 = rect.bottom;
      x2 = rect.right;
      y2 = rect.top;
      break;
    case 1:
      x0 = rect.left;
      y0 = rect.bottom;
      x1 = rect.right;
      y1 = rect.bottom;
      x2 = rect.left;
      y2 = rect.top;
      break;
    case 2:
      x0 = rect.right;
      y0 = rect.bottom;
      x1 = rect.right;
      y1 = rect.top;
      x2 = rect.left;
      y2 = rect.bottom;
      break;
    case 3:
      x0 = rect.right;
      y0 = rect.top;
      x1 = rect.left;
      y1 = rect.top;
      x2 = rect.right;
      y2 = rect.bottom;
      break;
    default:
      CHECK_LT(rotation, 0);
      // Handing this with `rotation += 4` breaks public API compatibility. So
      // just return early here without doing all the matrix calculations below.
      return CFX_Matrix(0, 0, 0, 0, 0, 0);
  }
  CFX_Matrix matrix((x2 - x0) / page_size_.width, (y2 - y0) / page_size_.width,
                    (x1 - x0) / page_size_.height,
                    (y1 - y0) / page_size_.height, x0, y0);
  return page_matrix_ * matrix;
}

CFX_Matrix CPDF_Page::GetDisplayMatrix() const {
  const CFX_FloatRect rect(0, 0, GetPageWidth(), GetPageHeight());
  return GetDisplayMatrixForFloatRect(rect, 0);
}

int CPDF_Page::GetPageRotation() const {
  // EmbedPDF: If rotation override is set, use it instead of dictionary value
  if (rotation_override_.has_value()) {
    return rotation_override_.value();
  }
  return GetOriginalRotation();
}

int CPDF_Page::GetOriginalRotation() const {
  // Always read from dictionary, ignoring any override
  RetainPtr<const CPDF_Object> pRotate =
      GetPageAttr(pdfium::page_object::kRotate);
  int rotation = pRotate ? (pRotate->GetInteger() / 90) % 4 : 0;
  return (rotation < 0) ? (rotation + 4) : rotation;
}

void CPDF_Page::SetRotationOverride(int rotation) {
  // Set override (-1 clears it)
  if (rotation < 0) {
    rotation_override_.reset();
  } else {
    rotation_override_ = rotation % 4;
  }
  // Recalculate dimensions with the new rotation
  UpdateDimensions();
}

RetainPtr<CPDF_Array> CPDF_Page::GetOrCreateAnnotsArray() {
  return GetMutableDict()->GetOrCreateArrayFor("Annots");
}

RetainPtr<CPDF_Array> CPDF_Page::GetMutableAnnotsArray() {
  return GetMutableDict()->GetMutableArrayFor("Annots");
}

RetainPtr<const CPDF_Array> CPDF_Page::GetAnnotsArray() const {
  return GetDict()->GetArrayFor("Annots");
}

void CPDF_Page::AddPageImageCache() {
  page_image_cache_ = std::make_unique<CPDF_PageImageCache>(this);
}

void CPDF_Page::SetRenderContext(std::unique_ptr<RenderContextIface> context) {
  DCHECK(!render_context_);
  DCHECK(context);
  render_context_ = std::move(context);
}

void CPDF_Page::ClearRenderContext() {
  render_context_.reset();
}

void CPDF_Page::ClearView() {
  if (view_) {
    view_->ClearPage(this);
  }
}

void CPDF_Page::UpdateDimensions() {
  CFX_FloatRect mediabox = GetBox(pdfium::page_object::kMediaBox);
  if (mediabox.IsEmpty()) {
    mediabox = CFX_FloatRect(0, 0, 612, 792);
  }

  bbox_ = GetBox(pdfium::page_object::kCropBox);
  if (bbox_.IsEmpty()) {
    bbox_ = mediabox;
  } else {
    bbox_.Intersect(mediabox);
  }

  page_size_.width = bbox_.Width();
  page_size_.height = bbox_.Height();

  // EmbedPDF: what this page now shows, for IsContentCurrent().
  read_mediabox_ = GetBox(pdfium::page_object::kMediaBox);
  read_cropbox_ = GetBox(pdfium::page_object::kCropBox);
  read_rotation_ = GetOriginalRotation();
  content_checked_epoch_ = kContentUnchecked;

  switch (GetPageRotation()) {
    case 0:
      page_matrix_ = CFX_Matrix(1.0f, 0, 0, 1.0f, -bbox_.left, -bbox_.bottom);
      break;
    case 1:
      std::swap(page_size_.width, page_size_.height);
      page_matrix_ = CFX_Matrix(0, -1.0f, 1.0f, 0, -bbox_.bottom, bbox_.right);
      break;
    case 2:
      page_matrix_ = CFX_Matrix(-1.0f, 0, 0, -1.0f, bbox_.right, bbox_.top);
      break;
    case 3:
      std::swap(page_size_.width, page_size_.height);
      page_matrix_ = CFX_Matrix(0, 1.0f, -1.0f, 0, bbox_.top, -bbox_.left);
      break;
  }
}

CPDF_Page::RenderContextClearer::RenderContextClearer(CPDF_Page* pPage)
    : page_(pPage) {}

CPDF_Page::RenderContextClearer::~RenderContextClearer() {
  if (page_) {
    page_->ClearRenderContext();
  }
}
