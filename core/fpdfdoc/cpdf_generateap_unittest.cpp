// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_generateap.h"

#include <vector>

#include "constants/annotation_common.h"
#include "constants/font_encodings.h"
#include "constants/form_fields.h"
#include "core/fpdfapi/page/test_with_page_module.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_test_document.h"
#include "core/fxge/cfx_color.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

class CPDFGenerateAPTest : public TestWithPageModule {};

RetainPtr<CPDF_Array> MakeNumberArray(const std::vector<float>& values) {
  auto array = pdfium::MakeRetain<CPDF_Array>();
  for (float value : values) {
    array->AppendNew<CPDF_Number>(value);
  }
  return array;
}

RetainPtr<CPDF_Dictionary> MakeAnnotDict(const ByteString& subtype,
                                         const CFX_FloatRect& rect) {
  auto annot_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  annot_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype, subtype);
  annot_dict->SetRectFor(pdfium::annotation::kRect, rect);
  return annot_dict;
}

ByteString GenerateTextMarkupAPContent(const ByteString& subtype_name,
                                       CPDF_Annot::Subtype subtype,
                                       const std::vector<float>& quad_points) {
  CPDF_TestDocument doc;
  auto annot_dict = MakeAnnotDict(subtype_name, CFX_FloatRect(0, 0, 100, 100));
  annot_dict->SetFor("QuadPoints", MakeNumberArray(quad_points));
  std::optional<CPDF_GenerateAP::GeneratedAP> generated =
      CPDF_GenerateAP::GenerateEphemeralAnnotAP(&doc, annot_dict.Get(),
                                                subtype);
  if (!generated || !generated->normal_stream ||
      !generated->normal_stream->IsMemoryBased()) {
    return ByteString();
  }
  const pdfium::span<const uint8_t> data =
      generated->normal_stream->GetInMemoryRawData();
  return ByteString(data.data(), data.size());
}

RetainPtr<CPDF_Dictionary> AddAcroFormWithHelvetica(CPDF_TestDocument* doc) {
  doc->CreateNewDoc();
  RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
  auto acroform = root->SetNewFor<CPDF_Dictionary>("AcroForm");
  acroform->SetNewFor<CPDF_String>("DA", "/Helv 12 Tf 0 g");

  auto dr = acroform->SetNewFor<CPDF_Dictionary>("DR");
  auto fonts = dr->SetNewFor<CPDF_Dictionary>("Font");
  auto helv = doc->NewIndirect<CPDF_Dictionary>();
  helv->SetNewFor<CPDF_Name>("Type", "Font");
  helv->SetNewFor<CPDF_Name>("Subtype", "Type1");
  helv->SetNewFor<CPDF_Name>("BaseFont", "Helvetica");
  helv->SetNewFor<CPDF_Name>("Encoding",
                             pdfium::font_encodings::kWinAnsiEncoding);
  fonts->SetNewFor<CPDF_Reference>("Helv", doc, helv->GetObjNum());
  return acroform;
}

RetainPtr<CPDF_Dictionary> MakeSupportedAnnotDict(CPDF_Annot::Subtype subtype) {
  switch (subtype) {
    case CPDF_Annot::Subtype::CIRCLE:
      return MakeAnnotDict("Circle", CFX_FloatRect(0, 0, 100, 100));
    case CPDF_Annot::Subtype::HIGHLIGHT: {
      auto annot_dict =
          MakeAnnotDict("Highlight", CFX_FloatRect(0, 0, 100, 100));
      annot_dict->SetFor("QuadPoints",
                         MakeNumberArray({10, 20, 50, 20, 10, 10, 50, 10}));
      return annot_dict;
    }
    case CPDF_Annot::Subtype::FREETEXT: {
      auto annot_dict = MakeAnnotDict("FreeText", CFX_FloatRect(0, 0, 100, 40));
      annot_dict->SetNewFor<CPDF_String>("DA", "/Helv 12 Tf 0 g");
      annot_dict->SetNewFor<CPDF_String>(pdfium::annotation::kContents,
                                         "hello");
      return annot_dict;
    }
    case CPDF_Annot::Subtype::INK: {
      auto annot_dict = MakeAnnotDict("Ink", CFX_FloatRect(0, 0, 10, 10));
      auto ink_list = pdfium::MakeRetain<CPDF_Array>();
      ink_list->Append(MakeNumberArray({1, 1, 9, 9}));
      annot_dict->SetFor("InkList", std::move(ink_list));
      return annot_dict;
    }
    case CPDF_Annot::Subtype::LINE: {
      auto annot_dict = MakeAnnotDict("Line", CFX_FloatRect(0, 0, 100, 100));
      annot_dict->SetFor("L", MakeNumberArray({10, 10, 90, 90}));
      return annot_dict;
    }
    case CPDF_Annot::Subtype::POLYGON: {
      auto annot_dict = MakeAnnotDict("Polygon", CFX_FloatRect(0, 0, 100, 100));
      annot_dict->SetFor(pdfium::annotation::kVertices,
                         MakeNumberArray({10, 10, 90, 10, 90, 90}));
      return annot_dict;
    }
    case CPDF_Annot::Subtype::POLYLINE: {
      auto annot_dict =
          MakeAnnotDict("PolyLine", CFX_FloatRect(0, 0, 100, 100));
      annot_dict->SetFor(pdfium::annotation::kVertices,
                         MakeNumberArray({10, 10, 50, 90, 90, 10}));
      return annot_dict;
    }
    case CPDF_Annot::Subtype::SQUARE:
      return MakeAnnotDict("Square", CFX_FloatRect(0, 0, 100, 100));
    case CPDF_Annot::Subtype::SQUIGGLY: {
      auto annot_dict =
          MakeAnnotDict("Squiggly", CFX_FloatRect(0, 0, 100, 100));
      annot_dict->SetFor("QuadPoints",
                         MakeNumberArray({10, 20, 50, 20, 10, 10, 50, 10}));
      return annot_dict;
    }
    case CPDF_Annot::Subtype::STRIKEOUT: {
      auto annot_dict =
          MakeAnnotDict("StrikeOut", CFX_FloatRect(0, 0, 100, 100));
      annot_dict->SetFor("QuadPoints",
                         MakeNumberArray({10, 20, 50, 20, 10, 10, 50, 10}));
      return annot_dict;
    }
    case CPDF_Annot::Subtype::UNDERLINE: {
      auto annot_dict =
          MakeAnnotDict("Underline", CFX_FloatRect(0, 0, 100, 100));
      annot_dict->SetFor("QuadPoints",
                         MakeNumberArray({10, 20, 50, 20, 10, 10, 50, 10}));
      return annot_dict;
    }
    case CPDF_Annot::Subtype::WIDGET: {
      auto annot_dict = MakeAnnotDict("Widget", CFX_FloatRect(0, 0, 100, 40));
      annot_dict->SetNewFor<CPDF_Name>(pdfium::form_fields::kFT,
                                       pdfium::form_fields::kTx);
      annot_dict->SetNewFor<CPDF_String>("DA", "/Helv 12 Tf 0 g");
      annot_dict->SetNewFor<CPDF_String>("V", "hello");
      return annot_dict;
    }
    default:
      return nullptr;
  }
}

}  // namespace

TEST_F(CPDFGenerateAPTest,
       GenerateEphemeralSupportedAnnotAPDoesNotPersistGraphState) {
  static constexpr CPDF_Annot::Subtype kSupportedSubtypes[] = {
      CPDF_Annot::Subtype::CIRCLE,    CPDF_Annot::Subtype::FREETEXT,
      CPDF_Annot::Subtype::HIGHLIGHT, CPDF_Annot::Subtype::INK,
      CPDF_Annot::Subtype::LINE,      CPDF_Annot::Subtype::POLYGON,
      CPDF_Annot::Subtype::POLYLINE,  CPDF_Annot::Subtype::SQUARE,
      CPDF_Annot::Subtype::SQUIGGLY,  CPDF_Annot::Subtype::STRIKEOUT,
      CPDF_Annot::Subtype::UNDERLINE, CPDF_Annot::Subtype::WIDGET};

  for (CPDF_Annot::Subtype subtype : kSupportedSubtypes) {
    CPDF_TestDocument doc;
    if (subtype == CPDF_Annot::Subtype::FREETEXT ||
        subtype == CPDF_Annot::Subtype::WIDGET) {
      AddAcroFormWithHelvetica(&doc);
    }
    RetainPtr<CPDF_Dictionary> annot_dict = MakeSupportedAnnotDict(subtype);
    ASSERT_TRUE(annot_dict);

    const uint32_t last_obj_num = doc.GetLastObjNum();
    std::optional<CPDF_GenerateAP::GeneratedAP> generated =
        CPDF_GenerateAP::GenerateEphemeralAnnotAP(&doc, annot_dict.Get(),
                                                  subtype);

    ASSERT_TRUE(generated.has_value());
    ASSERT_TRUE(generated->normal_stream);
    EXPECT_EQ(0u, generated->normal_stream->GetObjNum());
    EXPECT_EQ(last_obj_num, doc.GetLastObjNum());
    EXPECT_FALSE(annot_dict->KeyExist(pdfium::annotation::kAP));
  }
}

TEST_F(CPDFGenerateAPTest, GenerateEphemeralFreeTextAPDoesNotCreateAcroForm) {
  CPDF_TestDocument doc;
  doc.CreateNewDoc();
  auto annot_dict = MakeSupportedAnnotDict(CPDF_Annot::Subtype::FREETEXT);

  const uint32_t last_obj_num = doc.GetLastObjNum();
  std::optional<CPDF_GenerateAP::GeneratedAP> generated =
      CPDF_GenerateAP::GenerateEphemeralAnnotAP(&doc, annot_dict.Get(),
                                                CPDF_Annot::Subtype::FREETEXT);

  ASSERT_TRUE(generated.has_value());
  ASSERT_TRUE(generated->normal_stream);
  EXPECT_EQ(0u, generated->normal_stream->GetObjNum());
  EXPECT_EQ(last_obj_num, doc.GetLastObjNum());
  EXPECT_FALSE(doc.GetRoot()->KeyExist("AcroForm"));
  EXPECT_FALSE(annot_dict->KeyExist(pdfium::annotation::kAP));
}

TEST_F(CPDFGenerateAPTest,
       GeneratePersistentFreeTextAPAfterDefaultAppearanceColorUpdate) {
  CPDF_TestDocument doc;
  doc.CreateNewDoc();
  auto annot_dict = MakeAnnotDict("FreeText", CFX_FloatRect(100, 50, 150, 75));
  annot_dict->SetNewFor<CPDF_String>(pdfium::annotation::kContents, "Hello!");

  ASSERT_TRUE(CPDF_GenerateAP::GenerateDefaultAppearanceWithColor(
      &doc, annot_dict.Get(), CFX_Color(60, 120, 180)));
  ASSERT_TRUE(CPDF_GenerateAP::GenerateAnnotAP(&doc, annot_dict.Get(),
                                               CPDF_Annot::Subtype::FREETEXT));

  RetainPtr<const CPDF_Dictionary> ap_dict =
      annot_dict->GetDictFor(pdfium::annotation::kAP);
  ASSERT_TRUE(ap_dict);
  RetainPtr<const CPDF_Stream> normal_stream = ap_dict->GetStreamFor("N");
  ASSERT_TRUE(normal_stream);
  EXPECT_NE(0u, normal_stream->GetObjNum());
}

TEST_F(CPDFGenerateAPTest,
       DefaultAppearanceColorUpdateEnsuresPersistentFontResource) {
  CPDF_TestDocument doc;
  doc.CreateNewDoc();
  RetainPtr<CPDF_Dictionary> acroform =
      doc.GetMutableRoot()->SetNewFor<CPDF_Dictionary>("AcroForm");
  acroform->SetNewFor<CPDF_String>("DA", "/Helv 12 Tf 0 g");

  auto annot_dict = MakeAnnotDict("FreeText", CFX_FloatRect(100, 50, 150, 75));

  ASSERT_TRUE(CPDF_GenerateAP::GenerateDefaultAppearanceWithColor(
      &doc, annot_dict.Get(), CFX_Color(60, 120, 180)));

  RetainPtr<const CPDF_Dictionary> font_dict =
      acroform->GetDictFor("DR")->GetDictFor("Font");
  ASSERT_TRUE(font_dict);
  EXPECT_TRUE(font_dict->KeyExist("Helv"));
}

TEST_F(CPDFGenerateAPTest, GenerateEphemeralAnnotAPDoesNotPersistHighlightAP) {
  CPDF_TestDocument doc;
  auto annot_dict = MakeAnnotDict("Highlight", CFX_FloatRect(0, 0, 100, 100));
  annot_dict->SetFor("QuadPoints",
                     MakeNumberArray({10, 20, 50, 20, 10, 10, 50, 10}));

  const uint32_t last_obj_num = doc.GetLastObjNum();
  std::optional<CPDF_GenerateAP::GeneratedAP> generated =
      CPDF_GenerateAP::GenerateEphemeralAnnotAP(&doc, annot_dict.Get(),
                                                CPDF_Annot::Subtype::HIGHLIGHT);

  ASSERT_TRUE(generated.has_value());
  ASSERT_TRUE(generated->normal_stream);
  EXPECT_EQ(0u, generated->normal_stream->GetObjNum());
  EXPECT_EQ(last_obj_num, doc.GetLastObjNum());
  EXPECT_FALSE(annot_dict->KeyExist(pdfium::annotation::kAP));
}

TEST_F(CPDFGenerateAPTest, TextMarkupAxisAlignedFastPathIsStable) {
  const ByteString content =
      GenerateTextMarkupAPContent("Highlight", CPDF_Annot::Subtype::HIGHLIGHT,
                                  {10, 20, 50, 20, 10, 10, 50, 10});
  EXPECT_TRUE(content.Contains("10 20 m 50 20 l 50 10 l 10 10 l h f\n"));
}

TEST_F(CPDFGenerateAPTest, HighlightUsesOrientedQuad) {
  const ByteString content =
      GenerateTextMarkupAPContent("Highlight", CPDF_Annot::Subtype::HIGHLIGHT,
                                  {0, 10, 0, 20, 10, 10, 10, 20});
  EXPECT_TRUE(content.Contains("0 10 m 0 20 l 10 20 l 10 10 l h f\n"));
}

TEST_F(CPDFGenerateAPTest, LineMarkupUsesOrientedQuad) {
  const std::vector<float> quad = {0, 10, 0, 20, 10, 10, 10, 20};
  const ByteString underline = GenerateTextMarkupAPContent(
      "Underline", CPDF_Annot::Subtype::UNDERLINE, quad);
  EXPECT_TRUE(underline.Contains("9 10 m 9 20 l S\n"));

  const ByteString strikeout = GenerateTextMarkupAPContent(
      "StrikeOut", CPDF_Annot::Subtype::STRIKEOUT, quad);
  EXPECT_TRUE(strikeout.Contains("5 10 m 5 20 l S\n"));

  const ByteString squiggly = GenerateTextMarkupAPContent(
      "Squiggly", CPDF_Annot::Subtype::SQUIGGLY, quad);
  EXPECT_TRUE(
      squiggly.Contains("8 10 m 10 12 l 8 14 l 10 16 l 8 18 l 10 20 l S\n"));
}

TEST_F(CPDFGenerateAPTest, MalformedTextMarkupFallsBackToAllCornerBounds) {
  const ByteString content =
      GenerateTextMarkupAPContent("Highlight", CPDF_Annot::Subtype::HIGHLIGHT,
                                  {0, 10, 10, 0, 10, 10, 0, 0});
  EXPECT_TRUE(content.Contains("0 10 m 10 10 l 10 0 l 0 0 l h f\n"));
}

TEST_F(CPDFGenerateAPTest, GenerateEphemeralInkAPDoesNotInflateAnnotRect) {
  CPDF_TestDocument doc;
  auto annot_dict = MakeAnnotDict("Ink", CFX_FloatRect(0, 0, 10, 10));

  auto border_style = annot_dict->SetNewFor<CPDF_Dictionary>("BS");
  border_style->SetNewFor<CPDF_Number>("W", 4);

  auto ink_list = pdfium::MakeRetain<CPDF_Array>();
  ink_list->Append(MakeNumberArray({1, 1, 9, 9}));
  annot_dict->SetFor("InkList", std::move(ink_list));

  const CFX_FloatRect original_rect =
      annot_dict->GetRectFor(pdfium::annotation::kRect);
  const uint32_t last_obj_num = doc.GetLastObjNum();
  std::optional<CPDF_GenerateAP::GeneratedAP> generated =
      CPDF_GenerateAP::GenerateEphemeralAnnotAP(&doc, annot_dict.Get(),
                                                CPDF_Annot::Subtype::INK);

  ASSERT_TRUE(generated.has_value());
  ASSERT_TRUE(generated->normal_stream);
  EXPECT_EQ(0u, generated->normal_stream->GetObjNum());
  EXPECT_EQ(last_obj_num, doc.GetLastObjNum());
  EXPECT_EQ(original_rect, annot_dict->GetRectFor(pdfium::annotation::kRect));
  // The ephemeral BBox is what the ink paints: points 1..9 inflated by half
  // the width (2).
  EXPECT_EQ(CFX_FloatRect(-1, -1, 11, 11),
            generated->normal_stream->GetDict()->GetRectFor("BBox"));
  EXPECT_FALSE(annot_dict->KeyExist(pdfium::annotation::kAP));
}
