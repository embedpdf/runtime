// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
#include "core/fpdfdoc/cpdf_measure.h"

#include "core/fpdfapi/page/test_with_page_module.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_test_document.h"
#include "core/fpdfdoc/cpdf_generateap_dimension.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {
class CPDFMeasureTest : public TestWithPageModule {};
}  // namespace
TEST_F(CPDFMeasureTest, DictionaryGrammarAndResetPreservesUnknownKeys) {
  EXPECT_EQ(CPDF_Measure::Subtype::kUnknown, CPDF_Measure::SubtypeOf(nullptr));
  auto measure = pdfium::MakeRetain<CPDF_Dictionary>();
  EXPECT_EQ(CPDF_Measure::Subtype::kRectilinear,
            CPDF_Measure::SubtypeOf(measure.Get()));
  measure->SetNewFor<CPDF_Name>("Subtype", "GEO");
  EXPECT_EQ(CPDF_Measure::Subtype::kGeospatial,
            CPDF_Measure::SubtypeOf(measure.Get()));
  measure->SetNewFor<CPDF_String>("Subtype", "RL");
  EXPECT_EQ(CPDF_Measure::Subtype::kUnknown,
            CPDF_Measure::SubtypeOf(measure.Get()));
  measure->SetNewFor<CPDF_String>("Vendor", "keep");
  measure->SetNewFor<CPDF_Array>("X");
  measure->SetNewFor<CPDF_Number>("CYX", 3);
  CPDF_Measure::ResetRectilinear(measure.Get());
  EXPECT_EQ(CPDF_Measure::Subtype::kRectilinear,
            CPDF_Measure::SubtypeOf(measure.Get()));
  EXPECT_EQ("keep", measure->GetByteStringFor("Vendor"));
  EXPECT_FALSE(measure->KeyExist("X"));
  EXPECT_FALSE(measure->KeyExist("CYX"));
  for (auto fraction :
       {CPDF_Measure::Fraction::kDecimal, CPDF_Measure::Fraction::kFraction,
        CPDF_Measure::Fraction::kRound, CPDF_Measure::Fraction::kTruncate}) {
    EXPECT_EQ(fraction, CPDF_Measure::FractionFromName(
                            CPDF_Measure::NameForFraction(fraction)));
  }
  EXPECT_FALSE(CPDF_Measure::FractionFromName("garbage"));
  for (auto position : {CPDF_Measure::LabelPosition::kSuffix,
                        CPDF_Measure::LabelPosition::kPrefix}) {
    EXPECT_EQ(position, CPDF_Measure::LabelPositionFromName(
                            CPDF_Measure::NameForLabelPosition(position)));
  }
}
TEST_F(CPDFMeasureTest,
       DetachCopiesNestedIndirectObjectsWithoutChangingSource) {
  CPDF_TestDocument doc;
  auto owner = pdfium::MakeRetain<CPDF_Dictionary>();
  auto measure = doc.NewIndirect<CPDF_Dictionary>();
  auto array = doc.NewIndirect<CPDF_Array>();
  auto format = doc.NewIndirect<CPDF_Dictionary>();
  format->SetNewFor<CPDF_Number>("C", 1);
  array->AppendNew<CPDF_Reference>(&doc, format->GetObjNum());
  measure->SetNewFor<CPDF_Reference>("X", &doc, array->GetObjNum());
  owner->SetNewFor<CPDF_Reference>("Measure", &doc, measure->GetObjNum());
  CPDF_Measure::DetachIndirect(owner.Get(), "Measure");
  auto copy = owner->GetMutableDictFor("Measure");
  ASSERT_TRUE(copy);
  EXPECT_EQ(0u, copy->GetObjNum());
  auto copied_array = copy->GetMutableArrayFor("X");
  EXPECT_EQ(0u, copied_array->GetObjNum());
  copied_array->GetMutableDictAt(0)->SetNewFor<CPDF_Number>("C", 7);
  EXPECT_FLOAT_EQ(1, format->GetFloatFor("C"));
  EXPECT_TRUE(measure->GetObjectFor("X")->IsReference());
}
TEST_F(CPDFMeasureTest, LeadersUsePdfNormalAndSignedLength) {
  using pdfium::dimension::LayoutLine;
  auto positive = LayoutLine({100, 200}, {300, 200}, 30, 5, 2, 1);
  EXPECT_FLOAT_EQ(230, positive.start.y);
  ASSERT_EQ(2u, positive.leaders.size());
  EXPECT_FLOAT_EQ(202, positive.leaders[0].from.y);
  EXPECT_FLOAT_EQ(235, positive.leaders[0].to.y);
  auto negative = LayoutLine({100, 200}, {300, 200}, -15, 5, 2, 1);
  EXPECT_FLOAT_EQ(185, negative.start.y);
  EXPECT_FLOAT_EQ(198, negative.leaders[0].from.y);
  EXPECT_FLOAT_EQ(180, negative.leaders[0].to.y);
  auto diagonal = LayoutLine({0, 0}, {3, 4}, 10, 0, 0, 1);
  EXPECT_FLOAT_EQ(-8, diagonal.start.x);
  EXPECT_FLOAT_EQ(6, diagonal.start.y);
  auto zero = LayoutLine({-50, 20}, {-50, 20}, 0, 0, 0, 1);
  EXPECT_TRUE(zero.leaders.empty());
  EXPECT_FLOAT_EQ(-50, zero.start.x);
}
TEST_F(CPDFMeasureTest, CaptionGapTracksOffsetAndShortLineOverflow) {
  auto annot = pdfium::MakeRetain<CPDF_Dictionary>();
  auto line =
      pdfium::dimension::LayoutLine({100, 100}, {300, 100}, 30, 5, 0, 1);
  auto caption =
      pdfium::dimension::LayoutLineCaption(annot.Get(), line, 40, 9, 1);
  EXPECT_FALSE(caption.outside_arrows);
  EXPECT_FLOAT_EQ(78, caption.gap_start);
  EXPECT_FLOAT_EQ(122, caption.gap_end);
  EXPECT_FLOAT_EQ(200, caption.matrix.e);
  EXPECT_FLOAT_EQ(130, caption.matrix.f);
  auto co = annot->SetNewFor<CPDF_Array>("CO");
  co->AppendNew<CPDF_Number>(20);
  co->AppendNew<CPDF_Number>(30);
  caption = pdfium::dimension::LayoutLineCaption(annot.Get(), line, 40, 9, 1);
  EXPECT_FLOAT_EQ(220, caption.matrix.e);
  EXPECT_FLOAT_EQ(160, caption.matrix.f);
  EXPECT_FLOAT_EQ(caption.gap_start,
                  caption.gap_end);  // no gap at old position
  annot->RemoveFor("CO");
  annot->SetNewFor<CPDF_Number>("LL", -15);
  line = pdfium::dimension::LayoutLine({100, 100}, {110, 100}, -15, 5, 0, 1);
  caption = pdfium::dimension::LayoutLineCaption(annot.Get(), line, 40, 9, 1);
  EXPECT_TRUE(caption.outside_arrows);
  EXPECT_LT(caption.matrix.f, 85);
  line = pdfium::dimension::LayoutLine({300, 100}, {100, 100}, 0, 0, 0, 1);
  caption = pdfium::dimension::LayoutLineCaption(annot.Get(), line, 40, 9, 1);
  EXPECT_FLOAT_EQ(1, caption.matrix.a);
  EXPECT_FLOAT_EQ(0, caption.matrix.b);  // upright reversed line
}
TEST_F(CPDFMeasureTest, ShapeCentersUseGeometryAndExplicitPdfPoint) {
  auto annot = pdfium::MakeRetain<CPDF_Dictionary>();
  annot->SetRectFor(
      "Rect",
      {0, 0, 10000, 10000});  // visual bounds must not affect the anchor
  std::vector<CFX_PointF> square{{-100, 200}, {0, 200}, {0, 300}, {-100, 300}};
  auto p = pdfium::dimension::ShapeCaptionCenter(annot.Get(), square, true, 9);
  EXPECT_FLOAT_EQ(-50, p.x);
  EXPECT_FLOAT_EQ(250, p.y);
  std::vector<CFX_PointF> path{{-100, 200}, {0, 200}, {0, 300}};
  p = pdfium::dimension::ShapeCaptionCenter(annot.Get(), path, false, 9);
  EXPECT_FLOAT_EQ(0, p.x);
  EXPECT_FLOAT_EQ(206.5f, p.y);
  auto metadata = annot->SetNewFor<CPDF_Dictionary>("EMBD_Metadata");
  auto center = metadata->SetNewFor<CPDF_Array>("MeasurementCaptionCenter");
  center->AppendNew<CPDF_Number>(0);
  center->AppendNew<CPDF_Number>(0);
  p = pdfium::dimension::ShapeCaptionCenter(annot.Get(), square, true, 9);
  EXPECT_FLOAT_EQ(0, p.x);
  EXPECT_FLOAT_EQ(0, p.y);
  metadata->RemoveFor("MeasurementCaptionCenter");
  std::vector<CFX_PointF> concave{{0, 0},   {100, 0}, {100, 100}, {80, 100},
                                  {80, 20}, {20, 20}, {20, 100},  {0, 100}};
  p = pdfium::dimension::ShapeCaptionCenter(annot.Get(), concave, true, 9);
  EXPECT_TRUE(p.x < 20 || p.x > 80 || p.y < 20);
}
