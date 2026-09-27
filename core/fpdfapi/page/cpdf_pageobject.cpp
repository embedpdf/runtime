// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/page/cpdf_pageobject.h"

#include <atomic>
#include <utility>

#include "core/fxcrt/fx_coordinates.h"

namespace {

std::atomic<uint64_t> g_held_bounds_generation{0};

}  // namespace

// static
uint64_t CPDF_PageObject::HeldBoundsGeneration() {
  return g_held_bounds_generation.load(std::memory_order_relaxed);
}

// static
void CPDF_PageObject::NoteHeldBoundsChanged() {
  g_held_bounds_generation.fetch_add(1, std::memory_order_relaxed);
}

CPDF_PageObject::CPDF_PageObject(int32_t content_stream)
    : content_stream_(content_stream) {}

CPDF_PageObject::~CPDF_PageObject() = default;

bool CPDF_PageObject::IsText() const {
  return false;
}

bool CPDF_PageObject::IsPath() const {
  return false;
}

bool CPDF_PageObject::IsImage() const {
  return false;
}

bool CPDF_PageObject::IsShading() const {
  return false;
}

bool CPDF_PageObject::IsForm() const {
  return false;
}

CPDF_TextObject* CPDF_PageObject::AsText() {
  return nullptr;
}

const CPDF_TextObject* CPDF_PageObject::AsText() const {
  return nullptr;
}

CPDF_PathObject* CPDF_PageObject::AsPath() {
  return nullptr;
}

const CPDF_PathObject* CPDF_PageObject::AsPath() const {
  return nullptr;
}

CPDF_ImageObject* CPDF_PageObject::AsImage() {
  return nullptr;
}

const CPDF_ImageObject* CPDF_PageObject::AsImage() const {
  return nullptr;
}

CPDF_ShadingObject* CPDF_PageObject::AsShading() {
  return nullptr;
}

const CPDF_ShadingObject* CPDF_PageObject::AsShading() const {
  return nullptr;
}

CPDF_FormObject* CPDF_PageObject::AsForm() {
  return nullptr;
}

const CPDF_FormObject* CPDF_PageObject::AsForm() const {
  return nullptr;
}

pdfium::span<const ByteString> CPDF_PageObject::GetGraphicsResourceNames()
    const {
  return general_state().GetGraphicsResourceNames();
}

void CPDF_PageObject::SetDefaultStates() {
  graphic_states_.SetDefaultStates();
}

void CPDF_PageObject::CopyData(const CPDF_PageObject* pSrc) {
  graphic_states_ = pSrc->graphic_states_;
  SetRect(pSrc->rect_);
  dirty_ = true;
}

void CPDF_PageObject::SetIsActive(bool value) {
  if (is_active_ != value) {
    is_active_ = value;
    dirty_ = true;
  }
}

void CPDF_PageObject::TransformClipPath(const CFX_Matrix& matrix) {
  CPDF_ClipPath& clip_path = mutable_clip_path();
  if (!clip_path.HasRef()) {
    return;
  }
  clip_path.Transform(matrix);
  SetDirty(true);
}

FX_RECT CPDF_PageObject::GetBBox() const {
  return GetRect().GetOuterRect();
}

FX_RECT CPDF_PageObject::GetTransformedBBox(const CFX_Matrix& matrix) const {
  return matrix.TransformRect(GetRect()).GetOuterRect();
}
