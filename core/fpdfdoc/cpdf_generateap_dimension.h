// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
#ifndef CORE_FPDFDOC_CPDF_GENERATEAP_DIMENSION_H_
#define CORE_FPDFDOC_CPDF_GENERATEAP_DIMENSION_H_

#include <vector>
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/span.h"
class CPDF_Dictionary;

// All coordinates are default PDF user space. Page rotation/zoom are absent.
namespace pdfium::dimension {
struct Segment {
  CFX_PointF from;
  CFX_PointF to;
};
struct LineLayout {
  CFX_PointF start;
  CFX_PointF end;
  CFX_PointF along;
  CFX_PointF normal;
  float length = 0;
  std::vector<Segment> leaders;
  CFX_FloatRect bounds;
};
struct CaptionLayout {
  CFX_Matrix matrix;  // centered local text box -> PDF page
  CFX_FloatRect bounds;
  float gap_start = 0;
  float gap_end = 0;
  bool outside_arrows = false;
  std::vector<Segment> connector;
};
LineLayout LayoutLine(CFX_PointF start,
                      CFX_PointF end,
                      float length,
                      float extension,
                      float offset,
                      float stroke_width);
CaptionLayout LayoutLineCaption(const CPDF_Dictionary* annot,
                                const LineLayout& line,
                                float width,
                                float height,
                                float stroke_width);
CFX_PointF ShapeCaptionCenter(const CPDF_Dictionary* annot,
                              pdfium::span<const CFX_PointF> vertices,
                              bool closed,
                              float height);
CFX_Matrix ShapeCaptionRotation(const CPDF_Dictionary* annot);
CaptionLayout LayoutShapeCaption(const CPDF_Dictionary* annot,
                                 CFX_PointF center,
                                 float width,
                                 float height);
}  // namespace pdfium::dimension
#endif  // CORE_FPDFDOC_CPDF_GENERATEAP_DIMENSION_H_
