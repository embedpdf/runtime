// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PAGE_CPDF_IMAGEOBJECT_H_
#define CORE_FPDFAPI_PAGE_CPDF_IMAGEOBJECT_H_

#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"

class CFX_DIBitmap;
class CPDF_Image;

class CPDF_ImageObject final : public CPDF_PageObject {
 public:
  explicit CPDF_ImageObject(int32_t content_stream);
  CPDF_ImageObject();
  ~CPDF_ImageObject() override;

  // CPDF_PageObject
  Type GetType() const override;
  void Transform(const CFX_Matrix& matrix) override;
  bool IsImage() const override;
  CPDF_ImageObject* AsImage() override;
  const CPDF_ImageObject* AsImage() const override;

  void CalcBoundingBox();
  void SetImage(RetainPtr<CPDF_Image> pImage);
  RetainPtr<CPDF_Image> GetImage() const;
  RetainPtr<CFX_DIBitmap> GetIndependentBitmap() const;

  void SetInitialImageMatrix(const CFX_Matrix& matrix);

  void SetImageMatrix(const CFX_Matrix& matrix);
  const CFX_Matrix& matrix() const { return matrix_; }

  // The unit square the image matrix maps, once the bounding box is known.
  const CFX_FloatRect& GetOriginalRect() const { return original_rect_; }
  const CFX_Matrix& original_matrix() const { return original_matrix_; }

 private:
  void MaybePurgeCache();

  CFX_Matrix matrix_;
  CFX_FloatRect original_rect_;
  // TODO(thestig): Use with `CPDF_FormObject` and `CPDF_PageObject` as well.
  CFX_Matrix original_matrix_;
  RetainPtr<CPDF_Image> image_;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_IMAGEOBJECT_H_
