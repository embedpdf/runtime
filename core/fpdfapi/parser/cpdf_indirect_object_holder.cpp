// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/parser/cpdf_indirect_object_holder.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_read_only_graph_guard.h"
#include "core/fxcrt/check.h"

namespace {

const CPDF_Object* FilterInvalidObjNum(const CPDF_Object* obj) {
  return obj && obj->GetObjNum() != CPDF_Object::kInvalidObjNum ? obj : nullptr;
}

}  // namespace

CPDF_IndirectObjectHolder::CPDF_IndirectObjectHolder()
    : byte_string_pool_(std::make_unique<ByteStringPool>()) {}

CPDF_IndirectObjectHolder::~CPDF_IndirectObjectHolder() {
  byte_string_pool_.DeleteObject();  // Make weak.
}

RetainPtr<const CPDF_Object> CPDF_IndirectObjectHolder::GetIndirectObject(
    uint32_t objnum) const {
  return pdfium::WrapRetain(GetIndirectObjectInternal(objnum));
}

RetainPtr<CPDF_Object> CPDF_IndirectObjectHolder::GetMutableIndirectObject(
    uint32_t objnum) {
  return pdfium::WrapRetain(
      const_cast<CPDF_Object*>(GetOrParseIndirectObjectInternal(objnum)));
}

const CPDF_Object* CPDF_IndirectObjectHolder::GetIndirectObjectInternal(
    uint32_t objnum) const {
  auto it = indirect_objs_.find(objnum);
  if (it == indirect_objs_.end()) {
    return nullptr;
  }

  return FilterInvalidObjNum(it->second.Get());
}

bool CPDF_IndirectObjectHolder::SharesBackingStorageWith(
    const CPDF_Stream* stream) const {
  return false;
}

RetainPtr<CPDF_Object> CPDF_IndirectObjectHolder::FindLocalIndirectObject(
    uint32_t objnum) const {
  auto it = indirect_objs_.find(objnum);
  if (it == indirect_objs_.end()) {
    return nullptr;
  }

  return pdfium::WrapRetain(
      const_cast<CPDF_Object*>(FilterInvalidObjNum(it->second.Get())));
}

void CPDF_IndirectObjectHolder::AddPromotedObject(
    uint32_t objnum,
    RetainPtr<CPDF_Object> object) {
  DCHECK(objnum);
  DCHECK(objnum != CPDF_Object::kInvalidObjNum);
  CHECK(object);

  DCHECK_PDF_HOLDER_MUTABLE();
  DCHECK(!frozen_);
  object->SetObjNum(objnum);
  indirect_objs_[objnum] = std::move(object);
  last_obj_num_ = std::max(last_obj_num_, objnum);
}

RetainPtr<CPDF_Object> CPDF_IndirectObjectHolder::GetOrParseIndirectObject(
    uint32_t objnum) {
  return pdfium::WrapRetain(GetOrParseIndirectObjectInternal(objnum));
}

void CPDF_IndirectObjectHolder::Freeze() {
  if (frozen_) {
    return;
  }

  frozen_ = true;
  for (const auto& item : indirect_objs_) {
    if (item.second) {
      item.second->Freeze();
    }
  }
}

CPDF_Object* CPDF_IndirectObjectHolder::GetOrParseIndirectObjectInternal(
    uint32_t objnum) {
  if (objnum == 0 || objnum == CPDF_Object::kInvalidObjNum) {
    return nullptr;
  }

  // Add item anyway to prevent recursively parsing of same object.
  auto insert_result = indirect_objs_.insert(std::make_pair(objnum, nullptr));
  if (!insert_result.second) {
    return const_cast<CPDF_Object*>(
        FilterInvalidObjNum(insert_result.first->second.Get()));
  }
  RetainPtr<CPDF_Object> pNewObj = ParseIndirectObject(objnum);
  if (!pNewObj) {
    indirect_objs_.erase(insert_result.first);
    return nullptr;
  }

  pNewObj->SetObjNum(objnum);
  last_obj_num_ = std::max(last_obj_num_, objnum);
  // "Frozen" means frozen against mutation, not closed to the parser: an
  // object parsed from the holder's own immutable bytes is a pure function
  // of those bytes, so it may still enter the cache - frozen on arrival.
  if (frozen_) {
    pNewObj->Freeze();
  }

  CPDF_Object* result = pNewObj.Get();
  insert_result.first->second = std::move(pNewObj);
  return result;
}

RetainPtr<CPDF_Object> CPDF_IndirectObjectHolder::ParseIndirectObject(
    uint32_t objnum) {
  return nullptr;
}

uint32_t CPDF_IndirectObjectHolder::AddIndirectObject(
    RetainPtr<CPDF_Object> pObj) {
  DCHECK_PDF_HOLDER_MUTABLE();
  DCHECK(!frozen_);
  CHECK(!pObj->GetObjNum());
  pObj->SetObjNum(++last_obj_num_);
  indirect_objs_[last_obj_num_] = std::move(pObj);
  return last_obj_num_;
}

bool CPDF_IndirectObjectHolder::ReplaceIndirectObjectIfHigherGeneration(
    uint32_t objnum,
    RetainPtr<CPDF_Object> pObj) {
  DCHECK(objnum);
  if (!pObj || objnum == CPDF_Object::kInvalidObjNum) {
    return false;
  }

  auto& obj_holder = indirect_objs_[objnum];
  const CPDF_Object* old_object = FilterInvalidObjNum(obj_holder.Get());
  if (old_object && pObj->GetGenNum() <= old_object->GetGenNum()) {
    return false;
  }

  DCHECK_PDF_HOLDER_MUTABLE();
  DCHECK(!frozen_);
  pObj->SetObjNum(objnum);
  obj_holder = std::move(pObj);
  last_obj_num_ = std::max(last_obj_num_, objnum);
  return true;
}

void CPDF_IndirectObjectHolder::DeleteIndirectObject(uint32_t objnum) {
  auto it = indirect_objs_.find(objnum);
  if (it == indirect_objs_.end() || !FilterInvalidObjNum(it->second.Get())) {
    return;
  }

  DCHECK_PDF_HOLDER_MUTABLE();
  DCHECK(!frozen_);
  indirect_objs_.erase(it);
}
