// Copyright 2018 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/fpdfdoc/cpdf_annot.h"

#include <vector>

#include "constants/annotation_common.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/test_with_page_module.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_test_document.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

RetainPtr<CPDF_Array> CreateQuadPointArrayFromVector(
    const std::vector<int>& points) {
  auto array = pdfium::MakeRetain<CPDF_Array>();
  for (float point : points) {
    array->AppendNew<CPDF_Number>(point);
  }
  return array;
}

}  // namespace

class CPDFAnnotWithPageModuleTest : public TestWithPageModule {};

TEST(CPDFAnnotTest, RectFromQuadPointsArray) {
  RetainPtr<CPDF_Array> array = CreateQuadPointArrayFromVector(
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1});
  CFX_FloatRect rect = CPDF_Annot::RectFromQuadPointsArray(array.Get(), 0);
  EXPECT_EQ(0.0f, rect.left);
  EXPECT_EQ(1.0f, rect.bottom);
  EXPECT_EQ(6.0f, rect.right);
  EXPECT_EQ(7.0f, rect.top);

  rect = CPDF_Annot::RectFromQuadPointsArray(array.Get(), 1);
  EXPECT_EQ(2.0f, rect.left);
  EXPECT_EQ(1.0f, rect.bottom);
  EXPECT_EQ(8.0f, rect.right);
  EXPECT_EQ(7.0f, rect.top);
}

TEST(CPDFAnnotTest, BoundingRectFromQuadPoints) {
  auto dict = pdfium::MakeRetain<CPDF_Dictionary>();
  CFX_FloatRect rect = CPDF_Annot::BoundingRectFromQuadPoints(dict.Get());
  EXPECT_EQ(0.0f, rect.left);
  EXPECT_EQ(0.0f, rect.bottom);
  EXPECT_EQ(0.0f, rect.right);
  EXPECT_EQ(0.0f, rect.top);

  dict->SetFor("QuadPoints", CreateQuadPointArrayFromVector({0, 1, 2}));
  rect = CPDF_Annot::BoundingRectFromQuadPoints(dict.Get());
  EXPECT_EQ(0.0f, rect.left);
  EXPECT_EQ(0.0f, rect.bottom);
  EXPECT_EQ(0.0f, rect.right);
  EXPECT_EQ(0.0f, rect.top);

  dict->SetFor("QuadPoints",
               CreateQuadPointArrayFromVector({0, 1, 2, 3, 4, 5, 6, 7}));
  rect = CPDF_Annot::BoundingRectFromQuadPoints(dict.Get());
  EXPECT_EQ(0.0f, rect.left);
  EXPECT_EQ(1.0f, rect.bottom);
  EXPECT_EQ(6.0f, rect.right);
  EXPECT_EQ(7.0f, rect.top);

  dict->SetFor("QuadPoints", CreateQuadPointArrayFromVector(
                                 {0, 1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5,
                                  4, 3, 2, 1, 9, 2, 5, 7, 3, 6, 4, 1}));
  rect = CPDF_Annot::BoundingRectFromQuadPoints(dict.Get());
  EXPECT_EQ(0.0f, rect.left);
  EXPECT_EQ(1.0f, rect.bottom);
  EXPECT_EQ(9.0f, rect.right);
  EXPECT_EQ(7.0f, rect.top);
}

TEST(CPDFAnnotTest, RectFromQuadPoints) {
  auto dict = pdfium::MakeRetain<CPDF_Dictionary>();
  CFX_FloatRect rect = CPDF_Annot::RectFromQuadPoints(dict.Get(), 0);
  EXPECT_EQ(0.0f, rect.left);
  EXPECT_EQ(0.0f, rect.bottom);
  EXPECT_EQ(0.0f, rect.right);
  EXPECT_EQ(0.0f, rect.top);
  rect = CPDF_Annot::RectFromQuadPoints(dict.Get(), 5);
  EXPECT_EQ(0.0f, rect.left);
  EXPECT_EQ(0.0f, rect.bottom);
  EXPECT_EQ(0.0f, rect.right);
  EXPECT_EQ(0.0f, rect.top);

  dict->SetFor("QuadPoints",
               CreateQuadPointArrayFromVector({0, 1, 2, 3, 4, 5, 6, 7}));
  rect = CPDF_Annot::RectFromQuadPoints(dict.Get(), 0);
  EXPECT_EQ(0.0f, rect.left);
  EXPECT_EQ(1.0f, rect.bottom);
  EXPECT_EQ(6.0f, rect.right);
  EXPECT_EQ(7.0f, rect.top);
  rect = CPDF_Annot::RectFromQuadPoints(dict.Get(), 5);
  EXPECT_EQ(0.0f, rect.left);
  EXPECT_EQ(0.0f, rect.bottom);
  EXPECT_EQ(0.0f, rect.right);
  EXPECT_EQ(0.0f, rect.top);

  dict->SetFor("QuadPoints", CreateQuadPointArrayFromVector(
                                 {0, 1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5,
                                  4, 3, 2, 1, 9, 2, 5, 7, 3, 6, 4, 1}));
  rect = CPDF_Annot::RectFromQuadPoints(dict.Get(), 0);
  EXPECT_EQ(0.0f, rect.left);
  EXPECT_EQ(1.0f, rect.bottom);
  EXPECT_EQ(6.0f, rect.right);
  EXPECT_EQ(7.0f, rect.top);
  rect = CPDF_Annot::RectFromQuadPoints(dict.Get(), 1);
  EXPECT_EQ(2.0f, rect.left);
  EXPECT_EQ(1.0f, rect.bottom);
  EXPECT_EQ(8.0f, rect.right);
  EXPECT_EQ(7.0f, rect.top);
  rect = CPDF_Annot::RectFromQuadPoints(dict.Get(), 2);
  EXPECT_EQ(3.0f, rect.left);
  EXPECT_EQ(1.0f, rect.bottom);
  EXPECT_EQ(9.0f, rect.right);
  EXPECT_EQ(7.0f, rect.top);
}

TEST(CPDFAnnotTest, QuadPointCount) {
  RetainPtr<CPDF_Array> array = CreateQuadPointArrayFromVector({});
  EXPECT_EQ(0u, CPDF_Annot::QuadPointCount(array.Get()));

  for (int i = 0; i < 7; ++i) {
    array->AppendNew<CPDF_Number>(0);
    EXPECT_EQ(0u, CPDF_Annot::QuadPointCount(array.Get()));
  }
  for (int i = 0; i < 8; ++i) {
    array->AppendNew<CPDF_Number>(0);
    EXPECT_EQ(1u, CPDF_Annot::QuadPointCount(array.Get()));
  }
  for (int i = 0; i < 50; ++i) {
    array->AppendNew<CPDF_Number>(0);
  }
  EXPECT_EQ(8u, CPDF_Annot::QuadPointCount(array.Get()));
}

TEST_F(CPDFAnnotWithPageModuleTest,
       ConstructorDoesNotPersistEphemeralHighlightAP) {
  CPDF_TestDocument doc;
  auto annot_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  annot_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype, "Highlight");
  annot_dict->SetRectFor(pdfium::annotation::kRect,
                         CFX_FloatRect(0, 0, 100, 100));
  annot_dict->SetFor("QuadPoints", CreateQuadPointArrayFromVector(
                                       {10, 20, 50, 20, 10, 10, 50, 10}));

  const uint32_t last_obj_num = doc.GetLastObjNum();
  CPDF_Annot annot(annot_dict, &doc);

  EXPECT_EQ(last_obj_num, doc.GetLastObjNum());
  EXPECT_FALSE(annot_dict->KeyExist(pdfium::annotation::kAP));
  EXPECT_FALSE(annot_dict->KeyExist("PDFIUM_HasGeneratedAP"));
  EXPECT_EQ(CFX_FloatRect(10, 10, 50, 20), annot.GetRect());
}

TEST_F(CPDFAnnotWithPageModuleTest,
       EphemeralInkAPUsesInflatedDrawingRectWithoutPersistingRect) {
  CPDF_TestDocument doc;
  doc.SetRoot(pdfium::MakeRetain<CPDF_Dictionary>());
  auto page = pdfium::MakeRetain<CPDF_Page>(
      &doc, pdfium::MakeRetain<CPDF_Dictionary>());

  auto annot_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  annot_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype, "Ink");
  annot_dict->SetRectFor(pdfium::annotation::kRect,
                         CFX_FloatRect(0, 0, 10, 10));
  auto border_style = annot_dict->SetNewFor<CPDF_Dictionary>("BS");
  border_style->SetNewFor<CPDF_Number>("W", 4);

  auto ink_list = pdfium::MakeRetain<CPDF_Array>();
  ink_list->Append(CreateQuadPointArrayFromVector({1, 1, 9, 9}));
  annot_dict->SetFor("InkList", std::move(ink_list));
  // Black: ink with no /C has no stroke and paints nothing.
  annot_dict->SetFor(pdfium::annotation::kC,
                     CreateQuadPointArrayFromVector({0, 0, 0}));

  CPDF_Annot annot(annot_dict, &doc);
  EXPECT_EQ(CFX_FloatRect(0, 0, 10, 10),
            annot_dict->GetRectFor(pdfium::annotation::kRect));

  ASSERT_TRUE(annot.GetAPForm(page.Get(), CPDF_Annot::AppearanceMode::kNormal));
  EXPECT_FALSE(annot_dict->KeyExist(pdfium::annotation::kAP));
  EXPECT_EQ(CFX_FloatRect(0, 0, 10, 10),
            annot_dict->GetRectFor(pdfium::annotation::kRect));
  // The drawing rect is the minimal union of the authored /Rect and the
  // stroked ink bounds: points 1..9 inflated by half the width (2).
  EXPECT_EQ(CFX_FloatRect(-1, -1, 11, 11), annot.GetRect());
}
