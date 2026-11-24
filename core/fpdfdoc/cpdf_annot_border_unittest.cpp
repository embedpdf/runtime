// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_annot_border.h"

#include <initializer_list>
#include <optional>
#include <vector>

#include "constants/annotation_common.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

using BorderStyle = CPDF_Annot::BorderStyle;

RetainPtr<CPDF_Array> Numbers(std::initializer_list<float> values) {
  auto array = pdfium::MakeRetain<CPDF_Array>();
  for (float value : values) {
    array->AppendNew<CPDF_Number>(value);
  }
  return array;
}

// /BS on a new annotation: /W |width|, /S |style| and /D |dash|, each when
// given.
RetainPtr<CPDF_Dictionary> WithBorderStyle(
    std::optional<float> width,
    const char* style,
    std::optional<std::vector<float>> dash) {
  auto annot_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  auto border_style = annot_dict->SetNewFor<CPDF_Dictionary>("BS");
  if (width.has_value()) {
    border_style->SetNewFor<CPDF_Number>("W", width.value());
  }
  if (style) {
    border_style->SetNewFor<CPDF_Name>("S", style);
  }
  if (dash.has_value()) {
    auto dash_array = border_style->SetNewFor<CPDF_Array>("D");
    for (float value : dash.value()) {
      dash_array->AppendNew<CPDF_Number>(value);
    }
  }
  return annot_dict;
}

// /Border |border| on a new annotation.
RetainPtr<CPDF_Dictionary> WithBorderArray(RetainPtr<CPDF_Array> border) {
  auto annot_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  annot_dict->SetFor(pdfium::annotation::kBorder, std::move(border));
  return annot_dict;
}

// The pattern |border| stores; empty when it stores none or an empty one.
std::vector<float> StoredPattern(const fpdfdoc::AnnotBorder& border) {
  std::vector<float> values;
  if (border.dash_array) {
    for (size_t i = 0; i < border.dash_array->size(); ++i) {
      values.push_back(border.dash_array->GetFloatAt(i));
    }
  }
  return values;
}

}  // namespace

TEST(CPDFAnnotBorderTest, NeitherKeyIsSolidAndOneWide) {
  auto annot_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  const fpdfdoc::AnnotBorder border =
      fpdfdoc::ReadAnnotBorder(annot_dict.Get());
  EXPECT_EQ(1.0f, border.width);
  EXPECT_EQ(BorderStyle::kSolid, border.style);
  EXPECT_FALSE(border.dash_array);
}

TEST(CPDFAnnotBorderTest, BorderStyleWidthAndNames) {
  // What /BS leaves out takes its default: 1 wide, solid.
  fpdfdoc::AnnotBorder border = fpdfdoc::ReadAnnotBorder(
      WithBorderStyle(std::nullopt, nullptr, std::nullopt).Get());
  EXPECT_EQ(1.0f, border.width);
  EXPECT_EQ(BorderStyle::kSolid, border.style);

  border = fpdfdoc::ReadAnnotBorder(
      WithBorderStyle(0.0f, nullptr, std::nullopt).Get());
  EXPECT_EQ(0.0f, border.width);

  struct NameCase {
    const char* name;
    BorderStyle style;
  };
  static constexpr NameCase kNames[] = {
      {"S", BorderStyle::kSolid},
      {"D", BorderStyle::kDashed},
      {"B", BorderStyle::kBeveled},
      {"I", BorderStyle::kInset},
      {"U", BorderStyle::kUnderline},
      // A name ISO 32000 doesn't define is the default, solid, whatever its
      // first letter.
      {"Dashed", BorderStyle::kSolid},
      {"Underline", BorderStyle::kSolid},
      {"X", BorderStyle::kSolid},
  };
  for (const NameCase& name_case : kNames) {
    SCOPED_TRACE(name_case.name);
    border = fpdfdoc::ReadAnnotBorder(
        WithBorderStyle(2.5f, name_case.name, std::nullopt).Get());
    EXPECT_EQ(name_case.style, border.style);
    // The width as written, for a beveled or inset border too.
    EXPECT_EQ(2.5f, border.width);
  }
}

TEST(CPDFAnnotBorderTest, BorderStylePattern) {
  // Dashed without /D: no pattern stored, and it draws the default dash.
  fpdfdoc::AnnotBorder border =
      fpdfdoc::ReadAnnotBorder(WithBorderStyle(2, "D", std::nullopt).Get());
  EXPECT_EQ(BorderStyle::kDashed, border.style);
  EXPECT_FALSE(border.dash_array);

  border = fpdfdoc::ReadAnnotBorder(
      WithBorderStyle(2, "D", std::vector<float>{4, 2}).Get());
  EXPECT_EQ(BorderStyle::kDashed, border.style);
  EXPECT_EQ((std::vector<float>{4, 2}), StoredPattern(border));

  // An empty pattern is a solid line (ISO 32000 8.4.3.6).
  border = fpdfdoc::ReadAnnotBorder(
      WithBorderStyle(2, "D", std::vector<float>{}).Get());
  EXPECT_EQ(BorderStyle::kSolid, border.style);
  ASSERT_TRUE(border.dash_array);
  EXPECT_TRUE(border.dash_array->IsEmpty());

  // A pattern is stored whatever the style; only a dashed border draws it.
  for (const char* style : {"S", "B", "I", "U"}) {
    SCOPED_TRACE(style);
    border = fpdfdoc::ReadAnnotBorder(
        WithBorderStyle(2, style, std::vector<float>{4, 2}).Get());
    EXPECT_NE(BorderStyle::kDashed, border.style);
    EXPECT_EQ((std::vector<float>{4, 2}), StoredPattern(border));
  }
}

TEST(CPDFAnnotBorderTest, BorderArray) {
  fpdfdoc::AnnotBorder border =
      fpdfdoc::ReadAnnotBorder(WithBorderArray(Numbers({0, 0, 5})).Get());
  EXPECT_EQ(5.0f, border.width);
  EXPECT_EQ(BorderStyle::kSolid, border.style);
  EXPECT_FALSE(border.dash_array);

  border = fpdfdoc::ReadAnnotBorder(WithBorderArray(Numbers({0, 0, 0})).Get());
  EXPECT_EQ(0.0f, border.width);

  // The corner radii are not part of what is read.
  border = fpdfdoc::ReadAnnotBorder(WithBorderArray(Numbers({3, 3, 5})).Get());
  EXPECT_EQ(5.0f, border.width);
  EXPECT_EQ(BorderStyle::kSolid, border.style);

  // Too short to hold a width: the default.
  border = fpdfdoc::ReadAnnotBorder(WithBorderArray(Numbers({0, 0})).Get());
  EXPECT_EQ(1.0f, border.width);

  // A dash array with a pattern makes the border dashed.
  RetainPtr<CPDF_Array> dashed = Numbers({0, 0, 5});
  dashed->Append(Numbers({4, 2}));
  border = fpdfdoc::ReadAnnotBorder(WithBorderArray(dashed).Get());
  EXPECT_EQ(5.0f, border.width);
  EXPECT_EQ(BorderStyle::kDashed, border.style);
  EXPECT_EQ((std::vector<float>{4, 2}), StoredPattern(border));

  // An empty one is a solid line.
  RetainPtr<CPDF_Array> empty_dash = Numbers({0, 0, 5});
  empty_dash->Append(Numbers({}));
  border = fpdfdoc::ReadAnnotBorder(WithBorderArray(empty_dash).Get());
  EXPECT_EQ(BorderStyle::kSolid, border.style);
  ASSERT_TRUE(border.dash_array);
  EXPECT_TRUE(border.dash_array->IsEmpty());

  // A fourth entry that is no array is no dash.
  border =
      fpdfdoc::ReadAnnotBorder(WithBorderArray(Numbers({0, 0, 5, 7})).Get());
  EXPECT_EQ(5.0f, border.width);
  EXPECT_EQ(BorderStyle::kSolid, border.style);
  EXPECT_FALSE(border.dash_array);

  // The dash array is the last of four entries: with more, the width is
  // still read and no dash is.
  RetainPtr<CPDF_Array> five = Numbers({0, 0, 5});
  five->Append(Numbers({4, 2}));
  five->AppendNew<CPDF_Number>(9);
  border = fpdfdoc::ReadAnnotBorder(WithBorderArray(five).Get());
  EXPECT_EQ(5.0f, border.width);
  EXPECT_EQ(BorderStyle::kSolid, border.style);
  EXPECT_FALSE(border.dash_array);
}

TEST(CPDFAnnotBorderTest, BorderStyleWinsOverBorderArray) {
  // An empty /BS is the default border; /Border beside it is not read.
  RetainPtr<CPDF_Array> dashed = Numbers({0, 0, 5});
  dashed->Append(Numbers({4, 2}));
  RetainPtr<CPDF_Dictionary> annot_dict =
      WithBorderStyle(std::nullopt, nullptr, std::nullopt);
  annot_dict->SetFor(pdfium::annotation::kBorder, dashed);
  fpdfdoc::AnnotBorder border = fpdfdoc::ReadAnnotBorder(annot_dict.Get());
  EXPECT_EQ(1.0f, border.width);
  EXPECT_EQ(BorderStyle::kSolid, border.style);
  EXPECT_FALSE(border.dash_array);

  // A /BS that is no dictionary is no /BS.
  annot_dict->SetNewFor<CPDF_Number>("BS", 4);
  border = fpdfdoc::ReadAnnotBorder(annot_dict.Get());
  EXPECT_EQ(5.0f, border.width);
  EXPECT_EQ(BorderStyle::kDashed, border.style);
}
