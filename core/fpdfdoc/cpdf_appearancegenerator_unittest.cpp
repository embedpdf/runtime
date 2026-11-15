// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_appearancegenerator.h"

#include <optional>
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
  std::optional<CPDF_AppearanceGenerator::GeneratedAP> generated =
      CPDF_AppearanceGenerator::GenerateEphemeralAnnotAP(&doc, annot_dict.Get(),
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
    std::optional<CPDF_AppearanceGenerator::GeneratedAP> generated =
        CPDF_AppearanceGenerator::GenerateEphemeralAnnotAP(
            &doc, annot_dict.Get(), subtype);

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
  std::optional<CPDF_AppearanceGenerator::GeneratedAP> generated =
      CPDF_AppearanceGenerator::GenerateEphemeralAnnotAP(
          &doc, annot_dict.Get(), CPDF_Annot::Subtype::FREETEXT);

  ASSERT_TRUE(generated.has_value());
  ASSERT_TRUE(generated->normal_stream);
  EXPECT_EQ(0u, generated->normal_stream->GetObjNum());
  EXPECT_EQ(last_obj_num, doc.GetLastObjNum());
  EXPECT_FALSE(doc.GetRoot()->KeyExist("AcroForm"));
  EXPECT_FALSE(annot_dict->KeyExist(pdfium::annotation::kAP));
}

namespace {

// A free text box at |rect| reading "hello world" in 12 pt Helvetica.
RetainPtr<CPDF_Dictionary> MakeFreeTextDict(const CFX_FloatRect& rect) {
  auto annot_dict = MakeAnnotDict("FreeText", rect);
  annot_dict->SetNewFor<CPDF_String>("DA", "/Helv 12 Tf 0 g");
  annot_dict->SetNewFor<CPDF_String>(pdfium::annotation::kContents,
                                     "hello world");
  return annot_dict;
}

// A callout's free text box, its leader running from (10, 10) through a knee
// at (40, 40) to the box, ending in an open arrow.
RetainPtr<CPDF_Dictionary> MakeCalloutDict() {
  auto annot_dict = MakeFreeTextDict(CFX_FloatRect(0, 0, 200, 100));
  annot_dict->SetNewFor<CPDF_Name>("IT", "FreeTextCallout");
  annot_dict->SetFor("CL", MakeNumberArray({10, 10, 40, 40, 60, 40}));
  annot_dict->SetNewFor<CPDF_Name>("LE", "OpenArrow");
  return annot_dict;
}

// /Border [0 0 |width|], with |dash| as its dash array when given.
void SetBorderArray(CPDF_Dictionary* annot_dict,
                    float width,
                    std::optional<std::vector<float>> dash = std::nullopt) {
  RetainPtr<CPDF_Array> border = MakeNumberArray({0, 0, width});
  if (dash.has_value()) {
    border->Append(MakeNumberArray(dash.value()));
  }
  annot_dict->SetFor(pdfium::annotation::kBorder, std::move(border));
}

// /BS << /W |width| >>, dashed by |dash| when given.
void SetBorderStyle(CPDF_Dictionary* annot_dict,
                    float width,
                    std::optional<std::vector<float>> dash = std::nullopt) {
  auto border_style = annot_dict->SetNewFor<CPDF_Dictionary>("BS");
  border_style->SetNewFor<CPDF_Number>("W", width);
  if (dash.has_value()) {
    border_style->SetNewFor<CPDF_Name>("S", "D");
    border_style->SetFor("D", MakeNumberArray(dash.value()));
  }
}

// The content an in-memory free text appearance draws, each in a document
// of its own; empty when none is drawn.
ByteString GenerateFreeTextAPContent(CPDF_Dictionary* annot_dict) {
  CPDF_TestDocument doc;
  doc.CreateNewDoc();
  std::optional<CPDF_AppearanceGenerator::GeneratedAP> generated =
      CPDF_AppearanceGenerator::GenerateEphemeralAnnotAP(
          &doc, annot_dict, CPDF_Annot::Subtype::FREETEXT);
  if (!generated || !generated->normal_stream ||
      !generated->normal_stream->IsMemoryBased()) {
    return ByteString();
  }
  const pdfium::span<const uint8_t> data =
      generated->normal_stream->GetInMemoryRawData();
  return ByteString(data.data(), data.size());
}

}  // namespace

// Without /BS, a free text box takes its border width from /Border (ISO
// 32000-1 12.5.2), as a callout does: it draws exactly as the same width in
// /BS does, frame and text plate alike. With neither, the border is the
// default 1 pt.
TEST_F(CPDFGenerateAPTest, FreeTextBorderWidthFromBorderArray) {
  auto generate = [](std::optional<float> border_width,
                     std::optional<float> bs_width) {
    auto annot_dict = MakeFreeTextDict(CFX_FloatRect(0, 0, 100, 30));
    if (border_width.has_value()) {
      SetBorderArray(annot_dict.Get(), border_width.value());
    }
    if (bs_width.has_value()) {
      SetBorderStyle(annot_dict.Get(), bs_width.value());
    }
    return GenerateFreeTextAPContent(annot_dict.Get());
  };

  // No border: no frame, and the text plate is the whole box.
  const ByteString no_border = generate(0.0f, std::nullopt);
  ASSERT_FALSE(no_border.IsEmpty());
  EXPECT_FALSE(no_border.Contains("re f*"));
  EXPECT_TRUE(no_border.Contains("0 0 100 30 re W n\n"));
  EXPECT_EQ(generate(std::nullopt, 0.0f), no_border);

  EXPECT_EQ(generate(3.0f, std::nullopt), generate(std::nullopt, 3.0f));

  // Neither key: the 1 pt default, framed, the plate inset 2 pt.
  const ByteString default_border = generate(std::nullopt, std::nullopt);
  EXPECT_TRUE(default_border.Contains("re f*"));
  EXPECT_TRUE(default_border.Contains("2 2 96 26 re W n\n"));
  EXPECT_EQ(generate(std::nullopt, 1.0f), default_border);

  // /BS wins over /Border.
  EXPECT_EQ(generate(0.0f, 3.0f), generate(std::nullopt, 3.0f));
}

// Without /BS, a free text box's /Border dash array dashes its frame, as the
// same pattern in /BS does (ISO 32000-1 12.5.2); an empty one draws solid.
TEST_F(CPDFGenerateAPTest, FreeTextBorderDashFromBorderArray) {
  auto with_border = [](std::optional<std::vector<float>> dash) {
    auto annot_dict = MakeFreeTextDict(CFX_FloatRect(0, 0, 100, 30));
    SetBorderArray(annot_dict.Get(), 2, dash);
    return GenerateFreeTextAPContent(annot_dict.Get());
  };
  auto with_border_style = [](std::optional<std::vector<float>> dash) {
    auto annot_dict = MakeFreeTextDict(CFX_FloatRect(0, 0, 100, 30));
    SetBorderStyle(annot_dict.Get(), 2, dash);
    return GenerateFreeTextAPContent(annot_dict.Get());
  };

  const ByteString dashed = with_border(std::vector<float>{4, 2});
  ASSERT_FALSE(dashed.IsEmpty());
  EXPECT_TRUE(dashed.Contains("[4 2 ] 0 d\n"));
  EXPECT_EQ(with_border_style(std::vector<float>{4, 2}), dashed);

  const ByteString empty_dash = with_border(std::vector<float>{});
  EXPECT_FALSE(empty_dash.Contains(" 0 d\n"));
  EXPECT_EQ(with_border_style(std::nullopt), empty_dash);
}

// A callout's border dash draws its leader, the leader's ending and its box
// alike, from /BS or else /Border, as a line annotation's dash draws its line
// and endings. The text's underlines then draw solid.
TEST_F(CPDFGenerateAPTest, FreeTextCalloutBorderDash) {
  auto callout = MakeCalloutDict();
  SetBorderStyle(callout.Get(), 1, std::vector<float>{3, 1});
  const ByteString dashed = GenerateFreeTextAPContent(callout.Get());
  ASSERT_FALSE(dashed.IsEmpty());

  const std::optional<size_t> dash = dashed.Find("[3 1 ] 0 d\n");
  const std::optional<size_t> leader = dashed.Find(" l S\n");
  const std::optional<size_t> ending = dashed.Find(" cm\n");
  const std::optional<size_t> solid = dashed.Find("[] 0 d\n");
  const std::optional<size_t> text = dashed.Find("BT\n");
  ASSERT_TRUE(dash.has_value());
  ASSERT_TRUE(leader.has_value());
  ASSERT_TRUE(ending.has_value());
  ASSERT_TRUE(solid.has_value());
  ASSERT_TRUE(text.has_value());
  EXPECT_LT(dash.value(), leader.value());
  EXPECT_LT(leader.value(), ending.value());
  EXPECT_LT(ending.value(), solid.value());
  EXPECT_LT(solid.value(), text.value());

  auto from_border = MakeCalloutDict();
  SetBorderArray(from_border.Get(), 1, std::vector<float>{3, 1});
  EXPECT_EQ(GenerateFreeTextAPContent(from_border.Get()), dashed);

  auto solid_callout = MakeCalloutDict();
  SetBorderStyle(solid_callout.Get(), 1);
  EXPECT_FALSE(
      GenerateFreeTextAPContent(solid_callout.Get()).Contains(" 0 d\n"));
}

TEST_F(CPDFGenerateAPTest,
       GeneratePersistentFreeTextAPAfterDefaultAppearanceColorUpdate) {
  CPDF_TestDocument doc;
  doc.CreateNewDoc();
  auto annot_dict = MakeAnnotDict("FreeText", CFX_FloatRect(100, 50, 150, 75));
  annot_dict->SetNewFor<CPDF_String>(pdfium::annotation::kContents, "Hello!");

  ASSERT_TRUE(CPDF_AppearanceGenerator::GenerateDefaultAppearanceWithColor(
      &doc, annot_dict.Get(), CFX_Color::MakeRGBBytes(60, 120, 180)));
  ASSERT_TRUE(CPDF_AppearanceGenerator::GenerateAnnotAP(
      &doc, annot_dict.Get(), CPDF_Annot::Subtype::FREETEXT));

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

  ASSERT_TRUE(CPDF_AppearanceGenerator::GenerateDefaultAppearanceWithColor(
      &doc, annot_dict.Get(), CFX_Color::MakeRGBBytes(60, 120, 180)));

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
  std::optional<CPDF_AppearanceGenerator::GeneratedAP> generated =
      CPDF_AppearanceGenerator::GenerateEphemeralAnnotAP(
          &doc, annot_dict.Get(), CPDF_Annot::Subtype::HIGHLIGHT);

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
  // Black: ink with no /C has no stroke and paints nothing.
  annot_dict->SetFor(pdfium::annotation::kC, MakeNumberArray({0, 0, 0}));

  const CFX_FloatRect original_rect =
      annot_dict->GetRectFor(pdfium::annotation::kRect);
  const uint32_t last_obj_num = doc.GetLastObjNum();
  std::optional<CPDF_AppearanceGenerator::GeneratedAP> generated =
      CPDF_AppearanceGenerator::GenerateEphemeralAnnotAP(
          &doc, annot_dict.Get(), CPDF_Annot::Subtype::INK);

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
