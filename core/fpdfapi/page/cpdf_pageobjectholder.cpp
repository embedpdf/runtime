// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/page/cpdf_pageobjectholder.h"

#include <algorithm>
#include <utility>

#include "constants/transparency.h"
#include "core/fpdfapi/page/cpdf_allstates.h"
#include "core/fpdfapi/page/cpdf_contentparser.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pageobjectgroups.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/containers/unique_ptr_adapters.h"
#include "core/fxcrt/fx_extension.h"
#include "core/fxcrt/notreached.h"
#include "core/fxcrt/stl_util.h"

namespace {

// Holders with fewer objects walk them faster than they build runs.
constexpr size_t kMinObjectsForGroups = 8 * CPDF_PageObjectGroups::kSize;

bool g_object_groups_enabled = true;

}  // namespace

bool GraphicsData::operator<(const GraphicsData& other) const {
  if (!FXSYS_SafeEQ(fillAlpha, other.fillAlpha)) {
    return FXSYS_SafeLT(fillAlpha, other.fillAlpha);
  }
  if (!FXSYS_SafeEQ(strokeAlpha, other.strokeAlpha)) {
    return FXSYS_SafeLT(strokeAlpha, other.strokeAlpha);
  }
  return blendType < other.blendType;
}

bool FontData::operator<(const FontData& other) const {
  if (baseFont != other.baseFont) {
    return baseFont < other.baseFont;
  }
  return type < other.type;
}

CPDF_PageObjectHolder::CPDF_PageObjectHolder(
    CPDF_Document* doc,
    RetainPtr<CPDF_Dictionary> dict,
    RetainPtr<CPDF_Dictionary> pPageResources,
    RetainPtr<CPDF_Dictionary> pResources)
    : page_resources_(std::move(pPageResources)),
      resources_(std::move(pResources)),
      dict_(std::move(dict)),
      document_(doc) {
  DCHECK(dict_);
  const uint64_t overlay_epoch = document_ ? document_->GetOverlayEpoch() : 0;
  page_resources_epoch_ = overlay_epoch;
  resources_epoch_ = overlay_epoch;
  dict_epoch_ = overlay_epoch;
}

CPDF_PageObjectHolder::~CPDF_PageObjectHolder() = default;

void CPDF_PageObjectHolder::SetResources(RetainPtr<CPDF_Dictionary> dict) {
  resources_ = std::move(dict);
  resources_epoch_ = document_ ? document_->GetOverlayEpoch() : 0;
}

bool CPDF_PageObjectHolder::IsPage() const {
  return false;
}

RetainPtr<CPDF_Stream> CPDF_PageObjectHolder::GetMutableFormStream() {
  return nullptr;
}

RetainPtr<const CPDF_Dictionary> CPDF_PageObjectHolder::GetDict() const {
  if (!document_) {
    return dict_;
  }

  const uint64_t current_epoch = document_->GetOverlayEpoch();
  if (dict_epoch_ == current_epoch) {
    return dict_;
  }
  dict_epoch_ = current_epoch;

  const uint32_t objnum = dict_->GetObjNum();
  if (objnum == 0) {
    return dict_;
  }

  RetainPtr<const CPDF_Object> live = document_->GetIndirectObject(objnum);
  if (live && live.Get() != dict_.Get()) {
    const_cast<CPDF_PageObjectHolder*>(this)->dict_ =
        pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(live->AsDictionary()));
  }
  return dict_;
}

RetainPtr<CPDF_Dictionary> CPDF_PageObjectHolder::GetMutableDict() {
  if (!document_) {
    return dict_;
  }

  const uint32_t objnum = dict_->GetObjNum();
  if (objnum != 0) {
    RetainPtr<CPDF_Object> live = document_->GetMutableIndirectObject(objnum);
    if (live && live.Get() != dict_.Get()) {
      dict_ = pdfium::WrapRetain(live->AsMutableDictionary());
    }
    dict_epoch_ = document_->GetOverlayEpoch();
    return dict_;
  }

  // A direct dictionary is re-derived from its owner when it can't be
  // written in place, or when the overlay moved since it was derived: after
  // an abort it may still point into the dropped copy, which nothing would
  // ever read again.
  if (!dict_->IsWritable() || dict_epoch_ != document_->GetOverlayEpoch()) {
    EnsureMutableBackingObjectForDict();
  }
  dict_epoch_ = document_->GetOverlayEpoch();
  return dict_;
}

RetainPtr<const CPDF_Dictionary> CPDF_PageObjectHolder::GetResources() const {
  if (!document_ || !resources_) {
    return resources_;
  }

  const uint64_t current_epoch = document_->GetOverlayEpoch();
  if (resources_epoch_ == current_epoch) {
    return resources_;
  }
  resources_epoch_ = current_epoch;

  const uint32_t objnum = resources_->GetObjNum();
  if (objnum == 0) {
    return resources_;
  }

  RetainPtr<const CPDF_Object> live = document_->GetIndirectObject(objnum);
  if (live && live.Get() != resources_.Get()) {
    const_cast<CPDF_PageObjectHolder*>(this)->resources_ =
        pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(live->AsDictionary()));
  }
  return resources_;
}

RetainPtr<CPDF_Dictionary> CPDF_PageObjectHolder::GetMutableResources() {
  if (!document_ || !resources_) {
    return resources_;
  }

  const uint32_t objnum = resources_->GetObjNum();
  if (objnum != 0) {
    RetainPtr<CPDF_Object> live = document_->GetMutableIndirectObject(objnum);
    if (live && live.Get() != resources_.Get()) {
      resources_ = pdfium::WrapRetain(live->AsMutableDictionary());
    }
    resources_epoch_ = document_->GetOverlayEpoch();
    return resources_;
  }

  if (!resources_->IsWritable() ||
      resources_epoch_ != document_->GetOverlayEpoch()) {
    EnsureMutableBackingObjectForResources();
  }
  resources_epoch_ = document_->GetOverlayEpoch();
  return resources_;
}

RetainPtr<const CPDF_Dictionary> CPDF_PageObjectHolder::GetPageResources()
    const {
  if (!document_ || !page_resources_) {
    return page_resources_;
  }

  const uint64_t current_epoch = document_->GetOverlayEpoch();
  if (page_resources_epoch_ == current_epoch) {
    return page_resources_;
  }
  page_resources_epoch_ = current_epoch;

  const uint32_t objnum = page_resources_->GetObjNum();
  if (objnum == 0) {
    return page_resources_;
  }

  RetainPtr<const CPDF_Object> live = document_->GetIndirectObject(objnum);
  if (live && live.Get() != page_resources_.Get()) {
    const_cast<CPDF_PageObjectHolder*>(this)->page_resources_ =
        pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(live->AsDictionary()));
  }
  return page_resources_;
}

RetainPtr<CPDF_Dictionary> CPDF_PageObjectHolder::GetMutablePageResources() {
  if (!document_ || !page_resources_) {
    return page_resources_;
  }

  const uint32_t objnum = page_resources_->GetObjNum();
  if (objnum != 0) {
    RetainPtr<CPDF_Object> live = document_->GetMutableIndirectObject(objnum);
    if (live && live.Get() != page_resources_.Get()) {
      page_resources_ = pdfium::WrapRetain(live->AsMutableDictionary());
    }
    page_resources_epoch_ = document_->GetOverlayEpoch();
    return page_resources_;
  }

  if (!page_resources_->IsWritable() ||
      page_resources_epoch_ != document_->GetOverlayEpoch()) {
    EnsureMutableBackingObjectForPageResources();
  }
  page_resources_epoch_ = document_->GetOverlayEpoch();
  return page_resources_;
}

void CPDF_PageObjectHolder::EnsureMutableBackingObjectForDict() {
  NOTREACHED();
  CHECK(false);
}

void CPDF_PageObjectHolder::EnsureMutableBackingObjectForResources() {
  RetainPtr<CPDF_Dictionary> dict = GetMutableDict();
  resources_ = dict ? dict->GetMutableDictFor("Resources") : nullptr;
}

void CPDF_PageObjectHolder::EnsureMutableBackingObjectForPageResources() {
  RetainPtr<CPDF_Dictionary> dict = GetMutableDict();
  page_resources_ = dict ? dict->GetMutableDictFor("Resources") : nullptr;
}

void CPDF_PageObjectHolder::StartParse(
    std::unique_ptr<CPDF_ContentParser> pParser) {
  DCHECK_EQ(parse_state_, ParseState::kNotParsed);
  parser_ = std::move(pParser);
  parse_state_ = ParseState::kParsing;
}

void CPDF_PageObjectHolder::ContinueParse(PauseIndicatorIface* pPause) {
  if (parse_state_ == ParseState::kParsed) {
    return;
  }

  DCHECK_EQ(parse_state_, ParseState::kParsing);
  if (parser_->Continue(pPause)) {
    return;
  }

  parse_state_ = ParseState::kParsed;
  document_->IncrementParsedPageCount();
  all_ctms_ = parser_->TakeAllCTMs();

  parser_.reset();
}

void CPDF_PageObjectHolder::ResetParsedContent() {
  parser_.reset();
  parse_state_ = ParseState::kNotParsed;
  page_object_list_.clear();
  object_groups_.reset();
  all_ctms_.clear();
  mask_bounding_boxes_.clear();
  dirty_streams_.clear();
  graphics_map_.clear();
  fonts_map_.clear();
  fonts_by_objnum_.clear();
  colorspace_map_.clear();
  all_removed_resources_map_.clear();
  background_alpha_needed_ = false;
}

void CPDF_PageObjectHolder::AddImageMaskBoundingBox(const CFX_FloatRect& box) {
  mask_bounding_boxes_.push_back(box);
}

std::set<int32_t> CPDF_PageObjectHolder::TakeDirtyStreams() {
  auto dirty_streams = std::move(dirty_streams_);
  dirty_streams_.clear();
  return dirty_streams;
}

bool CPDF_PageObjectHolder::ResourcesDefine(ByteStringView category,
                                            const ByteString& name) const {
  RetainPtr<const CPDF_Dictionary> resources = GetResources();
  RetainPtr<const CPDF_Dictionary> names =
      resources ? resources->GetDictFor(category) : nullptr;
  return names && names->KeyExist(name.AsStringView());
}

std::optional<ByteString> CPDF_PageObjectHolder::GraphicsMapSearch(
    const GraphicsData& gd) {
  auto it = graphics_map_.find(gd);
  if (it == graphics_map_.end()) {
    return std::nullopt;
  }
  if (!ResourcesDefine("ExtGState", it->second)) {
    graphics_map_.erase(it);
    return std::nullopt;
  }
  return it->second;
}

void CPDF_PageObjectHolder::GraphicsMapInsert(const GraphicsData& gd,
                                              const ByteString& str) {
  graphics_map_[gd] = str;
}

std::optional<ByteString> CPDF_PageObjectHolder::FontsMapSearch(
    const FontData& fd) {
  auto it = fonts_map_.find(fd);
  if (it == fonts_map_.end()) {
    return std::nullopt;
  }
  if (!ResourcesDefine("Font", it->second)) {
    fonts_map_.erase(it);
    return std::nullopt;
  }
  return it->second;
}

void CPDF_PageObjectHolder::FontsMapInsert(const FontData& fd,
                                           const ByteString& str) {
  fonts_map_[fd] = str;
}

std::optional<ByteString> CPDF_PageObjectHolder::FontsByObjnumSearch(uint32_t objnum) {
  if (!objnum)
    return std::nullopt;
  auto it = fonts_by_objnum_.find(objnum);
  if (it == fonts_by_objnum_.end()) {
    return std::nullopt;
  }
  if (!ResourcesDefine("Font", it->second)) {
    fonts_by_objnum_.erase(it);
    return std::nullopt;
  }
  return it->second;
}

void CPDF_PageObjectHolder::FontsByObjnumInsert(uint32_t objnum, const ByteString& name) {
  if (!objnum)
    return;
  fonts_by_objnum_[objnum] = name;
}

std::optional<ByteString> CPDF_PageObjectHolder::ColorSpaceMapSearch(
    const ByteString& key) {
  auto it = colorspace_map_.find(key);
  if (it == colorspace_map_.end()) {
    return std::nullopt;
  }
  if (!ResourcesDefine("ColorSpace", it->second)) {
    colorspace_map_.erase(it);
    return std::nullopt;
  }
  return it->second;
}

void CPDF_PageObjectHolder::ColorSpaceMapInsert(const ByteString& key,
                                                const ByteString& name) {
  colorspace_map_[key] = name;
}

CFX_Matrix CPDF_PageObjectHolder::GetCTMAtBeginningOfStream(int32_t stream) {
  CHECK(stream >= 0 || stream == CPDF_PageObject::kNoContentStream);

  if (stream == 0 || all_ctms_.empty()) {
    return CFX_Matrix();
  }

  if (stream == CPDF_PageObject::kNoContentStream) {
    return all_ctms_.rbegin()->second;
  }

  // For all other cases, CTM at beginning of `stream` is the same value as CTM
  // at the end of the previous stream.
  return GetCTMAtEndOfStream(stream - 1);
}

CFX_Matrix CPDF_PageObjectHolder::GetCTMAtEndOfStream(int32_t stream) {
  // This code should never need to calculate the CTM for the end of
  // `CPDF_PageObject::kNoContentStream`, which uses a negative sentinel value.
  // All other streams have a non-negative index.
  CHECK_GE(stream, 0);

  if (all_ctms_.empty()) {
    return CFX_Matrix();
  }

  const auto it = all_ctms_.lower_bound(stream);
  return it != all_ctms_.end() ? it->second : all_ctms_.rbegin()->second;
}

void CPDF_PageObjectHolder::LoadTransparencyInfo() {
  RetainPtr<const CPDF_Dictionary> pGroup = dict_->GetDictFor("Group");
  if (!pGroup) {
    return;
  }

  if (pGroup->GetByteStringFor(pdfium::transparency::kGroupSubType) !=
      pdfium::transparency::kTransparency) {
    return;
  }
  transparency_.SetGroup();
  if (pGroup->GetIntegerFor(pdfium::transparency::kI)) {
    transparency_.SetIsolated();
  }
}

size_t CPDF_PageObjectHolder::GetActivePageObjectCount() const {
  size_t count = 0;
  for (const auto& page_object : page_object_list_) {
    if (page_object->IsActive()) {
      ++count;
    }
  }
  return count;
}

CPDF_PageObject* CPDF_PageObjectHolder::GetPageObjectByIndex(
    size_t index) const {
  return fxcrt::IndexInBounds(page_object_list_, index)
             ? page_object_list_[index].get()
             : nullptr;
}

const CPDF_PageObjectGroups* CPDF_PageObjectHolder::GetObjectGroups(
    const CFX_FloatRect& clip) const {
  if (!g_object_groups_enabled || parse_state_ != ParseState::kParsed ||
      page_object_list_.size() < kMinObjectsForGroups) {
    return nullptr;
  }
  if (!object_groups_ || !object_groups_->IsCurrent()) {
    // Where the objects can show: a page's box, or a form's /BBox, which is in
    // the same space as a form's clip.
    CFX_FloatRect area = bbox_;
    if (area.IsEmpty()) {
      area = GetDict()->GetRectFor("BBox");
      area.Normalize();
    }
    CFX_FloatRect shown = clip;
    shown.Intersect(area);
    if (2 * shown.Width() * shown.Height() > area.Width() * area.Height()) {
      return nullptr;
    }
    object_groups_ = std::make_unique<CPDF_PageObjectGroups>(*this);
  }
  return object_groups_.get();
}

// static
void CPDF_PageObjectHolder::SetObjectGroupsEnabledForTesting(bool enabled) {
  g_object_groups_enabled = enabled;
}

void CPDF_PageObjectHolder::AppendPageObject(
    std::unique_ptr<CPDF_PageObject> pPageObj) {
  CHECK(pPageObj);
  pPageObj->held_ = true;
  page_object_list_.push_back(std::move(pPageObj));
  object_groups_.reset();
  if (parse_state_ == ParseState::kParsed) {
    NoteObjectsEdited();
  }
}

bool CPDF_PageObjectHolder::InsertPageObjectAtIndex(
    size_t index,
    std::unique_ptr<CPDF_PageObject> page_obj) {
  CHECK(page_obj);
  if (index > page_object_list_.size()) {
    return false;
  }

  page_obj->held_ = true;
  // Unsafe, but the compiler will not complain, because
  // std::deque::iterator::operator++() has not been marked as unsafe yet.
  page_object_list_.insert(UNSAFE_TODO(page_object_list_.begin() + index),
                           std::move(page_obj));
  object_groups_.reset();
  NoteObjectsEdited();
  return true;
}

std::unique_ptr<CPDF_PageObject> CPDF_PageObjectHolder::RemovePageObject(
    CPDF_PageObject* pPageObj) {
  auto it = std::ranges::find_if(page_object_list_,
                                 pdfium::MatchesUniquePtr(pPageObj));
  if (it == std::end(page_object_list_)) {
    return nullptr;
  }

  std::unique_ptr<CPDF_PageObject> result = std::move(*it);
  page_object_list_.erase(it);
  result->held_ = false;
  object_groups_.reset();
  NoteObjectsEdited();

  int32_t content_stream = pPageObj->GetContentStream();
  if (content_stream >= 0) {
    dirty_streams_.insert(content_stream);
  }

  return result;
}

bool CPDF_PageObjectHolder::ErasePageObjectAtIndex(size_t index) {
  if (index >= page_object_list_.size()) {
    return false;
  }

  // Unsafe, but the compiler will not complain, because
  // std::deque::iterator::operator++() has not been marked as unsafe yet.
  page_object_list_.erase(UNSAFE_TODO(page_object_list_.begin() + index));
  object_groups_.reset();
  NoteObjectsEdited();
  return true;
}
