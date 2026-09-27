// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PAGE_CPDF_PATHOBJECT_H_
#define CORE_FPDFAPI_PAGE_CPDF_PATHOBJECT_H_

#include <stdint.h>

#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_path.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxge/cfx_fillrenderoptions.h"

class CPDF_PathObject final : public CPDF_PageObject {
 public:
  // A path's matrix, stored once for every path that has the same one, as
  // the paths of a content stream usually do. It never changes: a path that
  // gets another matrix gets another SharedMatrix.
  class SharedMatrix final : public Retainable {
   public:
    CONSTRUCT_VIA_MAKE_RETAIN;

    const CFX_Matrix& value() const { return value_; }

   private:
    explicit SharedMatrix(const CFX_Matrix& value);
    ~SharedMatrix() override;

    const CFX_Matrix value_;
  };

  // The storage for `matrix`: `previous` when it holds the same bits (0 and
  // -0 differ), otherwise new storage, and null for the identity, which needs
  // none.
  static RetainPtr<const SharedMatrix> ShareMatrix(
      const CFX_Matrix& matrix,
      RetainPtr<const SharedMatrix> previous);

  explicit CPDF_PathObject(int32_t content_stream);
  CPDF_PathObject();
  ~CPDF_PathObject() override;

  // CPDF_PageObject
  Type GetType() const override;
  void Transform(const CFX_Matrix& matrix) override;
  bool IsPath() const override;
  CPDF_PathObject* AsPath() override;
  const CPDF_PathObject* AsPath() const override;

  void CalcBoundingBox();

  bool stroke() const { return stroke_; }
  void set_stroke(bool stroke) { stroke_ = stroke; }

  // Layering, avoid caller knowledge of CFX_FillRenderOptions::FillType values.
  bool has_no_filltype() const {
    return fill_type_ == CFX_FillRenderOptions::FillType::kNoFill;
  }
  bool has_winding_filltype() const {
    return fill_type_ == CFX_FillRenderOptions::FillType::kWinding;
  }
  bool has_alternate_filltype() const {
    return fill_type_ == CFX_FillRenderOptions::FillType::kEvenOdd;
  }
  void set_no_filltype() {
    fill_type_ = CFX_FillRenderOptions::FillType::kNoFill;
  }
  void set_winding_filltype() {
    fill_type_ = CFX_FillRenderOptions::FillType::kWinding;
  }
  void set_alternate_filltype() {
    fill_type_ = CFX_FillRenderOptions::FillType::kEvenOdd;
  }

  CFX_FillRenderOptions::FillType filltype() const { return fill_type_; }
  void set_filltype(CFX_FillRenderOptions::FillType fill_type) {
    fill_type_ = fill_type;
  }

  CPDF_Path& path() { return path_; }
  const CPDF_Path& path() const { return path_; }

  CFX_Matrix matrix() const {
    return matrix_ ? matrix_->value() : CFX_Matrix();
  }
  void SetPathMatrix(const CFX_Matrix& matrix);
  // Sets a matrix from ShareMatrix(), which other paths may hold too.
  void SetSharedPathMatrix(RetainPtr<const SharedMatrix> matrix);

 private:
  bool stroke_ = false;
  CFX_FillRenderOptions::FillType fill_type_ =
      CFX_FillRenderOptions::FillType::kNoFill;
  CPDF_Path path_;
  // Null for the identity.
  RetainPtr<const SharedMatrix> matrix_;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_PATHOBJECT_H_
