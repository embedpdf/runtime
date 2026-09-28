// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// Derived from Apache PDFBox's CloudyBorder.java:
// https://github.com/apache/pdfbox/blob/trunk/pdfbox/src/main/java/org/apache/pdfbox/pdmodel/interactive/annotation/handlers/CloudyBorder.java
// Original code licensed under the Apache License, Version 2.0:
// https://www.apache.org/licenses/LICENSE-2.0

#ifndef CORE_FPDFDOC_CPDF_CLOUDY_BORDER_H_
#define CORE_FPDFDOC_CPDF_CLOUDY_BORDER_H_

#include <vector>

#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/fx_string_wrappers.h"

// Generates a cloudy (scalloped) border path for a rectangle annotation.
// Writes PDF path operators (m, c, h) to |out|. The caller is responsible
// for setting graphics state and paint operators.
// |box| is the square itself: the bumps reach out past it.
void GenerateCloudyRectanglePath(fxcrt::ostringstream& out,
                                 const CFX_FloatRect& box,
                                 float intensity,
                                 float line_width);

// Same for an ellipse (Circle annotation), |box| the ellipse's own box.
void GenerateCloudyEllipsePath(fxcrt::ostringstream& out,
                               const CFX_FloatRect& box,
                               float intensity,
                               float line_width);

// Same for a polygon annotation. |vertices| are in page coordinates.
void GenerateCloudyPolygonPath(fxcrt::ostringstream& out,
                               const std::vector<CFX_PointF>& vertices,
                               float intensity,
                               float line_width);

#endif  // CORE_FPDFDOC_CPDF_CLOUDY_BORDER_H_
