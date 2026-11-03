// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_ink_curves.h"

#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace {

// Acrobat's case H: dense like a real pen, a zigzag of 40 points 3 pt apart.
std::vector<CFX_PointF> DenseZigzag() {
  std::vector<CFX_PointF> points;
  for (int i = 0; i < 40; ++i) {
    points.push_back({260.0f + i * 3, 300.0f + (i % 2) * 2 + i * 0.5f});
  }
  return points;
}

void ExpectPoint(const InkCurvePoint& point, double x, double y) {
  EXPECT_NEAR(x, point.x, 1e-4);
  EXPECT_NEAR(y, point.y, 1e-4);
}

}  // namespace

// Acrobat draws the dense zigzag with 37 cubics. That pins its tolerance
// between 1.2747 (below it the curve of the first three points strays too far
// from their lines: 39 cubics) and 1.5062 (above it a piece whose worst point
// is that far stays whole: 33).
TEST(CPDFInkCurvesTest, TheDenseZigzagPinsTheTolerance) {
  const std::vector<CFX_PointF> points = DenseZigzag();
  EXPECT_EQ(37u, FitInkStroke(points).size());
  EXPECT_EQ(37u, FitInkStroke(points, 1.28).size());
  EXPECT_EQ(37u, FitInkStroke(points, 1.5).size());
  EXPECT_EQ(39u, FitInkStroke(points, 1.27).size());
  EXPECT_EQ(33u, FitInkStroke(points, 1.51).size());
}

TEST(CPDFInkCurvesTest, TwoPointsGetHandlesAThirdOfTheirLength) {
  static constexpr CFX_PointF kStroke[] = {{60, 520}, {200, 580}};
  const std::vector<InkCubic> curves = FitInkStroke(kStroke);
  ASSERT_EQ(1u, curves.size());
  // Acrobat: 60 520 m 106.6667 540 153.3333 560 200 580 c
  ExpectPoint(curves[0].control1, 106.6667, 540);
  ExpectPoint(curves[0].control2, 153.3333, 560);
  ExpectPoint(curves[0].end, 200, 580);
}

TEST(CPDFInkCurvesTest, OnePointHasNoCurves) {
  static constexpr CFX_PointF kDot[] = {{260, 550}};
  EXPECT_TRUE(FitInkStroke(kDot).empty());
  static constexpr CFX_PointF kRepeated[] = {{260, 550}, {260, 550}};
  EXPECT_TRUE(FitInkStroke(kRepeated).empty());
  EXPECT_TRUE(FitInkStroke({}).empty());
}

// A repeated point counts once: Acrobat's case D draws as if it were there
// once.
TEST(CPDFInkCurvesTest, ARepeatedPointCountsOnce) {
  static constexpr CFX_PointF kRepeated[] = {
      {330, 520}, {330, 520}, {370, 580}, {410, 520}};
  static constexpr CFX_PointF kOnce[] = {{330, 520}, {370, 580}, {410, 520}};
  const std::vector<InkCubic> repeated = FitInkStroke(kRepeated);
  const std::vector<InkCubic> once = FitInkStroke(kOnce);
  ASSERT_EQ(2u, repeated.size());
  ASSERT_EQ(once.size(), repeated.size());
  for (size_t i = 0; i < once.size(); ++i) {
    ExpectPoint(repeated[i].control1, once[i].control1.x, once[i].control1.y);
    ExpectPoint(repeated[i].control2, once[i].control2.x, once[i].control2.y);
    ExpectPoint(repeated[i].end, once[i].end.x, once[i].end.y);
  }
  // Acrobat: 330 520 m 343.3333 540 345.963 580 370 580 c
  ExpectPoint(repeated[0].control1, 343.3333, 540);
  ExpectPoint(repeated[0].control2, 345.963, 580);
}

// A stroke that turns straight back splits where it turns, with no direction
// there: the handles at the turn have no length (Acrobat's case E).
TEST(CPDFInkCurvesTest, TurningStraightBackHasNoHandlesAtTheTurn) {
  static constexpr CFX_PointF kStroke[] = {{60, 420}, {140, 470}, {60, 420}};
  const std::vector<InkCubic> curves = FitInkStroke(kStroke);
  ASSERT_EQ(2u, curves.size());
  // Acrobat: 60 420 m 86.6667 436.6667 140 470 140 470 c
  //          140 470 86.6667 436.6667 60 420 c
  ExpectPoint(curves[0].control1, 86.6667, 436.6667);
  ExpectPoint(curves[0].control2, 140, 470);
  ExpectPoint(curves[0].end, 140, 470);
  ExpectPoint(curves[1].control1, 140, 470);
  ExpectPoint(curves[1].control2, 86.6667, 436.6667);
  ExpectPoint(curves[1].end, 60, 420);
}
