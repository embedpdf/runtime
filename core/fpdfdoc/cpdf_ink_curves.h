// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFDOC_CPDF_INK_CURVES_H_
#define CORE_FPDFDOC_CPDF_INK_CURVES_H_

#include <vector>

#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/span.h"

// How Acrobat draws an ink stroke (/InkList) when it writes the appearance:
// cubic Béziers fitted to the points as Schneider fits digitized curves ("An
// Algorithm for Automatically Fitting Digitized Curves", Graphics Gems,
// 1990), without his Newton step. Checked against the appearances Acrobat
// wrote, to 0.001 pt, segment for segment:
//
// - Repeated points in a row count once.
// - A piece of the stroke is one cubic. At the stroke's ends it leaves and
//   arrives along the first and last segment; where two pieces meet, along
//   the line from the point before to the point after (no direction when
//   those are the same point: the stroke turns straight back).
// - A piece of two points gets handles a third of its length.
// - A longer piece gets the least-squares cubic over its points, placed by
//   chord length. It stays one piece when every point is within the tolerance
//   of the curve, at its place, and the curve is within the tolerance of the
//   lines between the points. Otherwise it splits at its worst point and both
//   halves are fitted again.

// How far the curves may stray, in points. Acrobat's lies above 1.2747 and at
// most 1.5062 (the dense zigzag in the unit test tells), at any width.
inline constexpr double kInkCurveTolerance = 1.4142135623730951;  // sqrt(2)

// A point of a fitted curve. Doubles, so the fit decides on distances as
// Acrobat's does, not on floats rounded on the way.
struct InkCurvePoint {
  double x = 0;
  double y = 0;
};

// One cubic of a stroke, from the end of the one before (or the stroke's
// first point).
struct InkCubic {
  InkCurvePoint control1;
  InkCurvePoint control2;
  InkCurvePoint end;
};

// The cubics that draw `stroke`, from its first point. None when it has fewer
// than two different points: Acrobat draws one point as a dot.
std::vector<InkCubic> FitInkStroke(pdfium::span<const CFX_PointF> stroke,
                                   double tolerance = kInkCurveTolerance);

#endif  // CORE_FPDFDOC_CPDF_INK_CURVES_H_
