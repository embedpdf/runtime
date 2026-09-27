// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/page/cpdf_pathobject.h"

#include <stdint.h>

#include <bit>
#include <utility>

namespace {

bool HaveSameBits(const CFX_Matrix& a, const CFX_Matrix& b) {
  return std::bit_cast<uint32_t>(a.a) == std::bit_cast<uint32_t>(b.a) &&
         std::bit_cast<uint32_t>(a.b) == std::bit_cast<uint32_t>(b.b) &&
         std::bit_cast<uint32_t>(a.c) == std::bit_cast<uint32_t>(b.c) &&
         std::bit_cast<uint32_t>(a.d) == std::bit_cast<uint32_t>(b.d) &&
         std::bit_cast<uint32_t>(a.e) == std::bit_cast<uint32_t>(b.e) &&
         std::bit_cast<uint32_t>(a.f) == std::bit_cast<uint32_t>(b.f);
}

}  // namespace

CPDF_PathObject::SharedMatrix::SharedMatrix(const CFX_Matrix& value)
    : value_(value) {}

CPDF_PathObject::SharedMatrix::~SharedMatrix() = default;

// static
RetainPtr<const CPDF_PathObject::SharedMatrix> CPDF_PathObject::ShareMatrix(
    const CFX_Matrix& matrix,
    RetainPtr<const SharedMatrix> previous) {
  if (HaveSameBits(matrix, CFX_Matrix())) {
    return nullptr;
  }
  if (previous && HaveSameBits(matrix, previous->value())) {
    return previous;
  }
  return pdfium::MakeRetain<SharedMatrix>(matrix);
}

CPDF_PathObject::CPDF_PathObject(int32_t content_stream)
    : CPDF_PageObject(content_stream) {}

CPDF_PathObject::CPDF_PathObject() : CPDF_PathObject(kNoContentStream) {}

CPDF_PathObject::~CPDF_PathObject() = default;

CPDF_PageObject::Type CPDF_PathObject::GetType() const {
  return Type::kPath;
}

void CPDF_PathObject::Transform(const CFX_Matrix& matrix) {
  CFX_Matrix path_matrix = this->matrix();
  path_matrix.Concat(matrix);
  matrix_ = ShareMatrix(path_matrix, std::move(matrix_));
  CalcBoundingBox();
  SetDirty(true);
}

bool CPDF_PathObject::IsPath() const {
  return true;
}

CPDF_PathObject* CPDF_PathObject::AsPath() {
  return this;
}

const CPDF_PathObject* CPDF_PathObject::AsPath() const {
  return this;
}

void CPDF_PathObject::CalcBoundingBox() {
  if (!path_.HasRef()) {
    return;
  }
  CFX_FloatRect rect;
  float width = graph_state().GetLineWidth();
  if (stroke_ && width != 0) {
    rect =
        path_.GetBoundingBoxForStrokePath(width, graph_state().GetMiterLimit());
  } else {
    rect = path_.GetBoundingBox();
  }
  rect = matrix().TransformRect(rect);

  if (width == 0 && stroke_) {
    rect.Inflate(0.5f, 0.5f);
  }
  SetRect(rect);
}

void CPDF_PathObject::SetPathMatrix(const CFX_Matrix& matrix) {
  matrix_ = ShareMatrix(matrix, std::move(matrix_));
  CalcBoundingBox();
}

void CPDF_PathObject::SetSharedPathMatrix(
    RetainPtr<const SharedMatrix> matrix) {
  matrix_ = std::move(matrix);
  CalcBoundingBox();
}
