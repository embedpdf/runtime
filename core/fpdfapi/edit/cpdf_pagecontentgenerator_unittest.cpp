// Copyright 2017 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/fpdfapi/edit/cpdf_pagecontentgenerator.h"

#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/page/cpdf_textstate.h"
#include "core/fpdfapi/page/test_with_page_module.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_test_document.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxge/cfx_fillrenderoptions.h"
#include "testing/gtest/include/gtest/gtest.h"

class CPDFPageContentGeneratorTest : public TestWithPageModule {
 protected:
  void TestProcessPath(CPDF_PageContentGenerator* pGen,
                       fxcrt::ostringstream* buf,
                       CPDF_PathObject* pPathObj) {
    pGen->ProcessPath(buf, pPathObj);
  }

  RetainPtr<const CPDF_Dictionary> TestGetResource(
      CPDF_PageContentGenerator* pGen,
      ByteStringView type,
      ByteStringView name) {
    RetainPtr<const CPDF_Dictionary> pResources =
        pGen->obj_holder_->GetResources();
    return pResources->GetDictFor(type)->GetDictFor(name);
  }

  void TestProcessText(CPDF_PageContentGenerator* pGen,
                       fxcrt::ostringstream* buf,
                       CPDF_TextObject* pTextObj) {
    pGen->ProcessText(buf, pTextObj);
  }

  ByteString GetResourceNameBeforeOperator(const ByteString& stream,
                                           const std::string& op) {
    std::string text(stream.c_str(), stream.GetLength());
    size_t op_pos = text.find(op);
    EXPECT_NE(std::string::npos, op_pos);
    if (op_pos == std::string::npos) {
      return ByteString();
    }

    size_t slash_pos = text.rfind('/', op_pos);
    EXPECT_NE(std::string::npos, slash_pos);
    if (slash_pos == std::string::npos) {
      return ByteString();
    }

    size_t name_start = slash_pos + 1;
    size_t name_end = text.find(' ', name_start);
    EXPECT_NE(std::string::npos, name_end);
    EXPECT_LE(name_end, op_pos);
    if (name_end == std::string::npos || name_end > op_pos) {
      return ByteString();
    }

    return ByteString(text.substr(name_start, name_end - name_start).c_str());
  }
};

TEST_F(CPDFPageContentGeneratorTest, ProcessRect) {
  auto pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->set_stroke(true);
  pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kEvenOdd);
  pPathObj->path().AppendRect(10, 5, 13, 30);

  auto dummy_page_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(nullptr, dummy_page_dict);
  CPDF_PageContentGenerator generator(pTestPage.Get());
  fxcrt::ostringstream buf;
  TestProcessPath(&generator, &buf, pPathObj.get());
  EXPECT_EQ("q 10 5 3 25 re B* Q\n", ByteString(buf));

  pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->path().AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kMove);
  pPathObj->path().AppendPoint(CFX_PointF(5.2f, 0),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(5.2f, 3.78f),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPointAndClose(CFX_PointF(0, 3.78f),
                                       CFX_Path::Point::Type::kLine);
  buf.str("");
  TestProcessPath(&generator, &buf, pPathObj.get());
  EXPECT_EQ("q 0 0 5.2 3.78 re n Q\n", ByteString(buf));

  // A rectangle with a duplicate second point, so its third point is not the
  // opposite corner.
  pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->path().AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kMove);
  pPathObj->path().AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(2, 0), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(2, 1), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(0, 1), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kLine);
  buf.str("");
  TestProcessPath(&generator, &buf, pPathObj.get());
  EXPECT_EQ("q 0 0 2 1 re n Q\n", ByteString(buf));

  // A rectangle with an extra point in the middle of its first edge, so its
  // third point is not the opposite corner either.
  pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->path().AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kMove);
  pPathObj->path().AppendPoint(CFX_PointF(50, 0), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(100, 0),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(100, 100),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(0, 100),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPointAndClose(CFX_PointF(0, 0),
                                       CFX_Path::Point::Type::kLine);
  buf.str("");
  TestProcessPath(&generator, &buf, pPathObj.get());
  EXPECT_EQ("q 0 0 100 100 re n Q\n", ByteString(buf));

  // Same as above, but closed without repeating the first point.
  pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->path().AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kMove);
  pPathObj->path().AppendPoint(CFX_PointF(50, 0), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(100, 0),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(100, 100),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPointAndClose(CFX_PointF(0, 100),
                                       CFX_Path::Point::Type::kLine);
  buf.str("");
  TestProcessPath(&generator, &buf, pPathObj.get());
  EXPECT_EQ("q 0 0 100 100 re n Q\n", ByteString(buf));
}

TEST_F(CPDFPageContentGeneratorTest, Bug937) {
  static const std::vector<float> rgb = {0.000000000000000000001f, 0.7f, 0.35f};
  RetainPtr<CPDF_ColorSpace> pCS =
      CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceRGB);
  {
    auto pPathObj = std::make_unique<CPDF_PathObject>();
    pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);

    // Test code in ProcessPath that generates re operator
    pPathObj->path().AppendRect(0.000000000000000000001,
                                0.000000000000000000001, 100, 100);

    pPathObj->mutable_color_state().SetFillColor(pCS, rgb);
    pPathObj->mutable_color_state().SetStrokeColor(pCS, rgb);
    pPathObj->mutable_graph_state().SetLineWidth(200000000000000000001.0);
    pPathObj->Transform(CFX_Matrix(1, 0, 0, 1, 0.000000000000000000001,
                                   200000000000000.000002));

    auto dummy_page_dict = pdfium::MakeRetain<CPDF_Dictionary>();
    auto pTestPage = pdfium::MakeRetain<CPDF_Page>(nullptr, dummy_page_dict);
    CPDF_PageContentGenerator generator(pTestPage.Get());
    fxcrt::ostringstream buf;
    TestProcessPath(&generator, &buf, pPathObj.get());
    EXPECT_EQ(
        "q .000000000000000000001 .7 .35 rg "
        ".000000000000000000001 .7 .35 RG "
        "200000000000000000000 w 1 0 0 1 .000000000000000000001 "
        "200000000000000 cm .000000000000000000001 "
        ".000000000000000000001 100 100 re f Q\n",
        ByteString(buf));
  }

  {
    // Test code in ProcessPath that handles bezier operator
    auto pPathObj = std::make_unique<CPDF_PathObject>();
    pPathObj->mutable_color_state().SetFillColor(pCS, rgb);
    pPathObj->mutable_color_state().SetStrokeColor(pCS, rgb);
    pPathObj->mutable_graph_state().SetLineWidth(2.000000000000000000001);
    pPathObj->Transform(CFX_Matrix(1, 0, 0, 1, 432, 500000000000000.000002));

    pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
    pPathObj->path().AppendPoint(CFX_PointF(0.000000000000000000001f, 4.67f),
                                 CFX_Path::Point::Type::kMove);
    pPathObj->path().AppendPoint(
        CFX_PointF(0.000000000000000000001, 100000000000000.000002),
        CFX_Path::Point::Type::kLine);
    pPathObj->path().AppendPoint(CFX_PointF(0.0000000000001f, 3.15f),
                                 CFX_Path::Point::Type::kBezier);
    pPathObj->path().AppendPoint(CFX_PointF(3.57f, 2.98f),
                                 CFX_Path::Point::Type::kBezier);
    pPathObj->path().AppendPointAndClose(
        CFX_PointF(53.4f, 5000000000000000000.00000000000000004),
        CFX_Path::Point::Type::kBezier);
    auto dummy_page_dict = pdfium::MakeRetain<CPDF_Dictionary>();
    auto pTestPage = pdfium::MakeRetain<CPDF_Page>(nullptr, dummy_page_dict);
    CPDF_PageContentGenerator generator(pTestPage.Get());
    fxcrt::ostringstream buf;

    TestProcessPath(&generator, &buf, pPathObj.get());
    EXPECT_EQ(
        "q .000000000000000000001 .7 .35 rg "
        ".000000000000000000001 .7 .35 RG 2 w 1 0 0 1 432 "
        "500000000000000 cm .000000000000000000001 4.67 m "
        ".000000000000000000001 100000000000000 l "
        ".0000000000001 3.15 3.57 2.98 53.4 "
        "5000000000000000000 c h f Q\n",
        ByteString(buf));
  }
}

TEST_F(CPDFPageContentGeneratorTest, ProcessPath) {
  auto pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
  pPathObj->path().AppendPoint(CFX_PointF(3.102f, 4.67f),
                               CFX_Path::Point::Type::kMove);
  pPathObj->path().AppendPoint(CFX_PointF(5.45f, 0.29f),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(4.24f, 3.15f),
                               CFX_Path::Point::Type::kBezier);
  pPathObj->path().AppendPoint(CFX_PointF(4.65f, 2.98f),
                               CFX_Path::Point::Type::kBezier);
  pPathObj->path().AppendPoint(CFX_PointF(3.456f, 0.24f),
                               CFX_Path::Point::Type::kBezier);
  pPathObj->path().AppendPoint(CFX_PointF(10.6f, 11.15f),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(11, 12.5f),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(11.46f, 12.67f),
                               CFX_Path::Point::Type::kBezier);
  pPathObj->path().AppendPoint(CFX_PointF(11.84f, 12.96f),
                               CFX_Path::Point::Type::kBezier);
  pPathObj->path().AppendPointAndClose(CFX_PointF(12, 13.64f),
                                       CFX_Path::Point::Type::kBezier);

  auto dummy_page_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(nullptr, dummy_page_dict);
  CPDF_PageContentGenerator generator(pTestPage.Get());
  fxcrt::ostringstream buf;
  TestProcessPath(&generator, &buf, pPathObj.get());
  EXPECT_EQ(
      "q 3.102 4.67 m 5.45 .29 l 4.24 "
      "3.15 4.65 2.98 3.456 .24 c 10.6 "
      "11.15 l 11 12.5 l 11.46 12.67 11.84 12.96 12 13.64 c h f"
      " Q\n",
      ByteString(buf));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessGraphics) {
  auto pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->set_stroke(true);
  pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
  pPathObj->path().AppendPoint(CFX_PointF(1, 2), CFX_Path::Point::Type::kMove);
  pPathObj->path().AppendPoint(CFX_PointF(3, 4), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPointAndClose(CFX_PointF(5, 6),
                                       CFX_Path::Point::Type::kLine);

  static const std::vector<float> rgb = {0.5f, 0.7f, 0.35f};
  RetainPtr<CPDF_ColorSpace> pCS =
      CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceRGB);
  pPathObj->mutable_color_state().SetFillColor(pCS, rgb);

  static const std::vector<float> rgb2 = {1, 0.9f, 0};
  pPathObj->mutable_color_state().SetStrokeColor(pCS, rgb2);
  pPathObj->mutable_general_state().SetFillAlpha(0.5f);
  pPathObj->mutable_general_state().SetStrokeAlpha(0.8f);

  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  RetainPtr<CPDF_Dictionary> pPageDict(doc->CreateNewPage(0));
  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(doc.get(), pPageDict);
  CPDF_PageContentGenerator generator(pTestPage.Get());
  fxcrt::ostringstream buf;
  TestProcessPath(&generator, &buf, pPathObj.get());
  ByteString path_string(buf);

  // Color RGB values used are integers divided by 255.
  const ByteStringView kExpectedStringStart = "q .5 .7 .35 rg 1 .9 0 RG /";
  const ByteStringView kExpectedStringEnd = " gs 1 2 m 3 4 l 5 6 l h B Q\n";
  const size_t kExpectedStringMinLength =
      kExpectedStringStart.GetLength() + kExpectedStringEnd.GetLength();
  EXPECT_EQ(kExpectedStringStart,
            path_string.First(kExpectedStringStart.GetLength()));
  EXPECT_EQ(kExpectedStringEnd,
            path_string.Last(kExpectedStringEnd.GetLength()));
  ASSERT_GT(path_string.GetLength(), kExpectedStringMinLength);
  RetainPtr<const CPDF_Dictionary> external_gs =
      TestGetResource(&generator, "ExtGState",
                      path_string.AsStringView().Substr(
                          kExpectedStringStart.GetLength(),
                          path_string.GetLength() - kExpectedStringMinLength));
  ASSERT_TRUE(external_gs);
  EXPECT_EQ(0.5f, external_gs->GetFloatFor("ca"));
  EXPECT_EQ(0.8f, external_gs->GetFloatFor("CA"));

  // Same path, now with a stroke.
  pPathObj->mutable_graph_state().SetLineWidth(10.5f);
  buf.str("");
  TestProcessPath(&generator, &buf, pPathObj.get());
  const ByteStringView kExpectedStringStart2 =
      "q .5 .7 .35 rg 1 .9 0 RG 10.5 w /";
  ByteString path_string2(buf);
  EXPECT_EQ(kExpectedStringStart2,
            path_string2.First(kExpectedStringStart2.GetLength()));
  EXPECT_EQ(kExpectedStringEnd,
            path_string2.Last(kExpectedStringEnd.GetLength()));

  // Compare with the previous (should use same dictionary for gs)
  const size_t kExpectedStringsLengthDifference =
      kExpectedStringStart2.GetLength() - kExpectedStringStart.GetLength();
  EXPECT_EQ(path_string.GetLength() + kExpectedStringsLengthDifference,
            path_string2.GetLength());
  EXPECT_EQ(
      path_string.Substr(kExpectedStringStart.GetLength(),
                         path_string.GetLength() - kExpectedStringMinLength),
      path_string2.Substr(kExpectedStringStart2.GetLength(),
                          path_string2.GetLength() - kExpectedStringMinLength -
                              kExpectedStringsLengthDifference));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessStandardText) {
  // Checking font whose font dictionary is not yet indirect object.
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  RetainPtr<CPDF_Dictionary> pPageDict(doc->CreateNewPage(0));
  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(doc.get(), pPageDict);
  CPDF_PageContentGenerator generator(pTestPage.Get());
  auto pTextObj = std::make_unique<CPDF_TextObject>();
  pTextObj->mutable_text_state().SetFont(
      CPDF_Font::GetStockFont(doc.get(), "Times-Roman"));
  pTextObj->mutable_text_state().SetFontSize(10.0f);

  static const std::vector<float> rgb = {0.5f, 0.7f, 0.35f};
  RetainPtr<CPDF_ColorSpace> pCS =
      CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceRGB);
  pTextObj->mutable_color_state().SetFillColor(pCS, rgb);

  static const std::vector<float> rgb2 = {1, 0.9f, 0};
  pTextObj->mutable_color_state().SetStrokeColor(pCS, rgb2);
  pTextObj->mutable_general_state().SetFillAlpha(0.5f);
  pTextObj->mutable_general_state().SetStrokeAlpha(0.8f);
  pTextObj->Transform(CFX_Matrix(1, 0, 0, 1, 100, 100));
  pTextObj->SetText("Hello World");
  fxcrt::ostringstream buf;
  TestProcessText(&generator, &buf, pTextObj.get());
  ByteString text_string(buf);
  ByteString gs_name = GetResourceNameBeforeOperator(text_string, " gs ");
  ByteString font_name = GetResourceNameBeforeOperator(text_string, " 10 Tf ");

  std::string expected =
      "q .5 .7 .35 rg 1 .9 0 RG /";
  expected += gs_name.c_str();
  expected +=
      " gs 1 0 0 1 100 100 cm BT 1 0 0 1 0 0 Tm /";
  expected += font_name.c_str();
  expected += " 10 Tf 0 Tr <48656C6C6F20576F726C64> Tj ET Q\n";
  EXPECT_EQ(ByteString(expected.c_str()), text_string);

  std::string text_output(text_string.c_str(), text_string.GetLength());
  EXPECT_LT(text_output.find("100 100 cm "), text_output.find("BT "));
  EXPECT_EQ(std::string::npos,
            text_output.find(" cm ", text_output.find("BT ")));

  RetainPtr<const CPDF_Dictionary> external_gs = TestGetResource(
      &generator, "ExtGState", gs_name.AsStringView());
  ASSERT_TRUE(external_gs);
  EXPECT_EQ(0.5f, external_gs->GetFloatFor("ca"));
  EXPECT_EQ(0.8f, external_gs->GetFloatFor("CA"));
  RetainPtr<const CPDF_Dictionary> font_dict =
      TestGetResource(&generator, "Font", font_name.AsStringView());
  ASSERT_TRUE(font_dict);
  EXPECT_EQ("Font", font_dict->GetNameFor("Type"));
  EXPECT_EQ("Type1", font_dict->GetNameFor("Subtype"));
  EXPECT_EQ("Times-Roman", font_dict->GetNameFor("BaseFont"));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessText) {
  // Checking font whose font dictionary is already an indirect object.
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  RetainPtr<CPDF_Dictionary> pPageDict(doc->CreateNewPage(0));
  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(doc.get(), pPageDict);
  CPDF_PageContentGenerator generator(pTestPage.Get());

  fxcrt::ostringstream buf;
  {
    // Set the text object font and text
    auto pTextObj = std::make_unique<CPDF_TextObject>();
    auto dict = doc->NewIndirect<CPDF_Dictionary>();
    dict->SetNewFor<CPDF_Name>("Type", "Font");
    dict->SetNewFor<CPDF_Name>("Subtype", "TrueType");

    RetainPtr<CPDF_Font> font = CPDF_Font::GetStockFont(doc.get(), "Arial");
    dict->SetNewFor<CPDF_Name>("BaseFont", font->GetBaseFontName());

    auto pDesc = doc->NewIndirect<CPDF_Dictionary>();
    pDesc->SetNewFor<CPDF_Name>("Type", "FontDescriptor");
    pDesc->SetNewFor<CPDF_Name>("FontName", font->GetBaseFontName());
    dict->SetNewFor<CPDF_Reference>("FontDescriptor", doc.get(),
                                    pDesc->GetObjNum());

    pTextObj->mutable_text_state().SetFont(
        CPDF_DocPageData::FromDocument(doc.get())->GetFont(dict));
    pTextObj->mutable_text_state().SetFontSize(15.5f);
    pTextObj->SetText("I am indirect");
    pTextObj->SetTextRenderMode(TextRenderingMode::MODE_FILL_CLIP);

    // Add a clipping path.
    auto pPath = std::make_unique<CPDF_Path>();
    pPath->AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kMove);
    pPath->AppendPoint(CFX_PointF(5, 0), CFX_Path::Point::Type::kLine);
    pPath->AppendPoint(CFX_PointF(5, 4), CFX_Path::Point::Type::kLine);
    pPath->AppendPointAndClose(CFX_PointF(0, 4), CFX_Path::Point::Type::kLine);
    CPDF_ClipPath& clip_path = pTextObj->mutable_clip_path();
    clip_path.Emplace();
    clip_path.AppendPath(*pPath, CFX_FillRenderOptions::FillType::kEvenOdd);

    TestProcessText(&generator, &buf, pTextObj.get());
  }

  ByteString text_string(buf);
  ByteString font_name =
      GetResourceNameBeforeOperator(text_string, " 15.5 Tf ");

  std::string expected = "q 0 0 5 4 re W* n BT 1 0 0 1 0 0 Tm /";
  expected += font_name.c_str();
  expected += " 15.5 Tf 4 Tr <4920616D20696E646972656374> Tj ET Q\n";
  EXPECT_EQ(ByteString(expected.c_str()), text_string);

  RetainPtr<const CPDF_Dictionary> font_dict =
      TestGetResource(&generator, "Font", font_name.AsStringView());
  ASSERT_TRUE(font_dict);
  EXPECT_TRUE(font_dict->GetObjNum());
  EXPECT_EQ("Font", font_dict->GetNameFor("Type"));
  EXPECT_EQ("TrueType", font_dict->GetNameFor("Subtype"));
  EXPECT_EQ("Helvetica", font_dict->GetNameFor("BaseFont"));
  RetainPtr<const CPDF_Dictionary> font_desc =
      font_dict->GetDictFor("FontDescriptor");
  ASSERT_TRUE(font_desc);
  EXPECT_TRUE(font_desc->GetObjNum());
  EXPECT_EQ("FontDescriptor", font_desc->GetNameFor("Type"));
  EXPECT_EQ("Helvetica", font_desc->GetNameFor("FontName"));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessEmptyForm) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();
  auto pStream =
      pdfium::MakeRetain<CPDF_Stream>(pdfium::MakeRetain<CPDF_Dictionary>());

  // Create an empty form.
  auto pTestForm = std::make_unique<CPDF_Form>(doc.get(), nullptr, pStream);
  pTestForm->ParseContent();
  ASSERT_EQ(CPDF_PageObjectHolder::ParseState::kParsed,
            pTestForm->GetParseState());

  // The generated stream for the empty form should be an empty string.
  CPDF_PageContentGenerator generator(pTestForm.get());
  fxcrt::ostringstream buf;
  EXPECT_FALSE(generator.ProcessPageObjects(&buf));
  EXPECT_EQ("", ByteString(buf));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessFormWithPath) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();
  static constexpr uint8_t kContents[] =
      "q 3.102 4.6700001 m 5.4500012 .28999999 "
      "l 4.2399998 3.1499999 4.65 2.98 3.456 0.24 c 3.102 4.6700001 l h f Q\n";
  auto pStream = pdfium::MakeRetain<CPDF_Stream>(
      DataVector<uint8_t>(std::begin(kContents), std::end(kContents)),
      pdfium::MakeRetain<CPDF_Dictionary>());

  // Create a form with a non-empty stream.
  auto pTestForm = std::make_unique<CPDF_Form>(doc.get(), nullptr, pStream);
  pTestForm->ParseContent();
  ASSERT_EQ(CPDF_PageObjectHolder::ParseState::kParsed,
            pTestForm->GetParseState());

  CPDF_PageContentGenerator generator(pTestForm.get());
  fxcrt::ostringstream process_buf;
  ASSERT_TRUE(generator.ProcessPageObjects(&process_buf));
  EXPECT_EQ(
      "q 3.102 4.67 m 5.4500012 .29 l 4.24 3.1499999 "
      "4.65 2.98 3.456 .24 c 3.102 4.67 l h f Q\n",
      ByteString(process_buf));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessContentMarksWithProperties) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  RetainPtr<CPDF_Dictionary> page_dict(doc->CreateNewPage(0));
  auto test_page = pdfium::MakeRetain<CPDF_Page>(doc.get(), page_dict);

  auto path_obj = std::make_unique<CPDF_PathObject>();
  path_obj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
  path_obj->path().AppendRect(0, 0, 10, 10);
  path_obj->SetDirty(true);

  auto marks_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  path_obj->GetContentMarks()->AddMarkWithPropertiesHolder(
      "M1", marks_dict, "Property Name With Space");

  test_page->AppendPageObject(std::move(path_obj));

  CPDF_PageContentGenerator generator(test_page.Get());
  fxcrt::ostringstream buf;
  EXPECT_TRUE(generator.ProcessPageObjects(&buf));
  ByteString content(buf);

  EXPECT_TRUE(content.Contains("/M1 /Property#20Name#20With#20Space BDC"));
}
