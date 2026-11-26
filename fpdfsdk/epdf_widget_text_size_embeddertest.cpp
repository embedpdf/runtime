// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// A field's automatic text size (a /DA size of 0) and a check box's mark, as
// the appearance generator draws them for EPDFAnnot_GenerateFormFieldAP().
// Acrobat 26 fits a field's text exactly, where PDFium stepped down a fixed
// ladder of sizes, and the numbers here are Acrobat's own wherever the
// field's width decides the size: they were read from fields Acrobat drew
// itself, each with a border of 1 (Nov 26 2026).

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fxcrt/fx_coordinates.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_annot.h"
#include "public/fpdf_annot.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

// The sizes of CPVT's ladder, which a fitted size no longer snaps to.
constexpr std::array<float, 25> kLadder = {
    4,  6,  8,  9,  10, 12, 14, 18,  20,  25,  30,  35, 40,
    45, 50, 55, 60, 70, 80, 90, 100, 110, 120, 130, 144};

bool OnTheLadder(float size) {
  return std::any_of(kLadder.begin(), kLadder.end(), [size](float step) {
    return std::fabs(step - size) < 1e-3f;
  });
}

// A file of one page whose widgets are `widgets` (each a widget and its
// field in one dictionary, without /Type, /Subtype and /P), in that order,
// every one with a black border 1 wide and the form's /DA: Helvetica at an
// automatic size. Object 4 is Helvetica and 5 ZapfDingbats.
std::string FieldsPdf(const std::vector<std::string>& widgets) {
  std::string refs;
  for (size_t i = 0; i < widgets.size(); ++i) {
    refs += std::to_string(6 + i) + " 0 R ";
  }
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [" + refs +
          "] /DA (/Helv 0 Tf 0 g) /DR << /Font << /Helv 4 0 R /ZaDb 5 0 R "
          ">> >> >> >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Annots [" + refs +
          "] >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
      "/WinAnsiEncoding >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /ZapfDingbats >>",
  };
  for (const std::string& widget : widgets) {
    objects.push_back(
        "<< /Type /Annot /Subtype /Widget /P 3 0 R /F 4 /BS << /W 1 /S /S >> "
        "/MK << /BC [0 0 0] >> /DA (/Helv 0 Tf 0 g) " +
        widget + " >>");
  }
  return MakePdf(objects);
}

// A field's /Rect, `width` by `height` at the lower left of the page.
std::string RectOf(float width, float height) {
  return "/Rect [10 10 " + std::to_string(10 + width) + " " +
         std::to_string(10 + height) + "]";
}

const CPDF_Dictionary* DictOf(FPDF_ANNOTATION annot) {
  return CPDFAnnotContextFromFPDFAnnotation(annot)->GetAnnotDict();
}

// The bytes of a stream, filters applied.
std::string StreamBytes(const CPDF_Stream* stream) {
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
  acc->LoadAllDataFiltered();
  const auto span = acc->GetSpan();
  return std::string(span.begin(), span.end());
}

// The normal appearance an annotation shows: its stream, or the state /AS
// names.
RetainPtr<const CPDF_Stream> AppearanceOf(FPDF_ANNOTATION annot) {
  return GetAnnotAP(DictOf(annot), CPDF_Annot::AppearanceMode::kNormal);
}

// The size an appearance writes its text at: the number before its first
// Tf, or nothing.
std::optional<float> DrawnSizeOf(const std::string& content) {
  const size_t set = content.find(" Tf");
  if (set == std::string::npos) {
    return std::nullopt;
  }
  const size_t start = content.rfind(' ', set - 1);
  return strtof(content.c_str() + (start == std::string::npos ? 0 : start),
                nullptr);
}

// The font the appearance writes its text in: the first one its resources
// name.
RetainPtr<CPDF_Font> FontOf(FPDF_ANNOTATION annot) {
  RetainPtr<const CPDF_Stream> appearance = AppearanceOf(annot);
  RetainPtr<const CPDF_Dictionary> resources =
      appearance ? appearance->GetDict()->GetDictFor("Resources") : nullptr;
  RetainPtr<const CPDF_Dictionary> fonts =
      resources ? resources->GetDictFor("Font") : nullptr;
  if (!fonts || fonts->size() == 0) {
    return nullptr;
  }
  RetainPtr<const CPDF_Dictionary> dict;
  {
    CPDF_DictionaryLocker locker(fonts);
    dict = ToDictionary(locker.begin()->second->GetDirect());
  }
  CPDF_Document* doc =
      CPDFAnnotContextFromFPDFAnnotation(annot)->GetPage()->GetDocument();
  return CPDF_DocPageData::FromDocument(doc)->GetFont(
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(dict.Get())));
}

// How wide `text` is at `size` in `font`, in points.
float WidthOf(CPDF_Font* font, const char* text, float size) {
  return font->GetStringWidth(text) * size / 1000;
}

// The box every point of the paths in `content` from `from` on reaches:
// the numbers before each m, l and c.
CFX_FloatRect PathBoxOf(const std::string& content, size_t from) {
  CFX_FloatRect box(1e6f, 1e6f, -1e6f, -1e6f);
  std::vector<float> numbers;
  size_t at = from;
  while (at < content.size()) {
    while (at < content.size() && (content[at] == ' ' || content[at] == '\n')) {
      ++at;
    }
    size_t end = at;
    while (end < content.size() && content[end] != ' ' &&
           content[end] != '\n') {
      ++end;
    }
    const std::string token = content.substr(at, end - at);
    at = end;
    if (token.empty()) {
      continue;
    }
    if (token == "m" || token == "l" || token == "c") {
      for (size_t i = 0; i + 1 < numbers.size(); i += 2) {
        box.left = std::min(box.left, numbers[i]);
        box.right = std::max(box.right, numbers[i]);
        box.bottom = std::min(box.bottom, numbers[i + 1]);
        box.top = std::max(box.top, numbers[i + 1]);
      }
    }
    char* number_end = nullptr;
    const float number = strtof(token.c_str(), &number_end);
    if (number_end != token.c_str() && *number_end == '\0') {
      numbers.push_back(number);
    } else {
      numbers.clear();
    }
  }
  return box;
}

class EPDFWidgetTextSizeEmbedderTest : public EmbedderTest {
 protected:
  // Opens `widgets` (FieldsPdf()) and has the generator draw each.
  void DrawFields(const std::vector<std::string>& widgets) {
    bytes_ = FieldsPdf(widgets);
    doc_.reset(FPDF_LoadMemDocument(bytes_.data(), bytes_.size(), nullptr));
    ASSERT_TRUE(doc_);
    page_.reset(FPDF_LoadPage(doc_.get(), 0));
    ASSERT_TRUE(page_);
    ASSERT_EQ(static_cast<int>(widgets.size()),
              FPDFPage_GetAnnotCount(page_.get()));
    for (size_t i = 0; i < widgets.size(); ++i) {
      ScopedFPDFAnnotation widget(
          FPDFPage_GetAnnot(page_.get(), static_cast<int>(i)));
      ASSERT_TRUE(widget);
      ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(widget.get()));
    }
  }

  ScopedFPDFAnnotation Widget(int index) {
    return ScopedFPDFAnnotation(FPDFPage_GetAnnot(page_.get(), index));
  }

  void TearDown() override {
    page_.reset();
    doc_.reset();
    EmbedderTest::TearDown();
  }

  std::string bytes_;
  ScopedFPDFDocument doc_;
  ScopedFPDFPage page_;
};

}  // namespace

// A value too long for its field shrinks until it fills the field's width
// inside twice the border at each side, whatever the field's height: 220
// wide leaves 216, and Acrobat's Helvetica size there is 8.679.
TEST_F(EPDFWidgetTextSizeEmbedderTest, ALongValueFillsTheWidth) {
  constexpr char kLong[] =
      "The quick brown fox jumps over the lazy dog, twice over";
  const float kHeights[] = {14, 20, 30, 48};
  std::vector<std::string> widgets;
  for (float height : kHeights) {
    widgets.push_back("/FT /Tx /T (long" + std::to_string(widgets.size()) +
                      ") /V (" + kLong + ") " + RectOf(220, height));
  }
  DrawFields(widgets);
  for (size_t i = 0; i < std::size(kHeights); ++i) {
    SCOPED_TRACE(kHeights[i]);
    ScopedFPDFAnnotation widget = Widget(static_cast<int>(i));
    const std::optional<float> size =
        DrawnSizeOf(StreamBytes(AppearanceOf(widget.get()).Get()));
    ASSERT_TRUE(size);
    EXPECT_FALSE(OnTheLadder(*size));
    RetainPtr<CPDF_Font> font = FontOf(widget.get());
    ASSERT_TRUE(font);
    // To a thousandth of a point: the line is up to 0.03 short of the room.
    EXPECT_NEAR(216, WidthOf(font.Get(), kLong, *size), 0.03f);
    // The tests' stand-in for Helvetica is 0.2% narrower than Acrobat's.
    EXPECT_NEAR(8.679f, *size, 0.02f);
  }
}

// A short value is as large as the field's height lets its line be: the
// font's ascent and descent fill the field inside its border, to a
// thousandth of a point, where PDFium stepped down to the size below
// (14 pt high: 10, where it fits 10.7).
TEST_F(EPDFWidgetTextSizeEmbedderTest, AShortValueFillsTheHeight) {
  const float kHeights[] = {14, 20, 30, 48};
  std::vector<std::string> widgets;
  for (float height : kHeights) {
    widgets.push_back("/FT /Tx /T (short" + std::to_string(widgets.size()) +
                      ") /V (Hello world) " + RectOf(220, height));
  }
  DrawFields(widgets);
  for (size_t i = 0; i < std::size(kHeights); ++i) {
    SCOPED_TRACE(kHeights[i]);
    ScopedFPDFAnnotation widget = Widget(static_cast<int>(i));
    const std::optional<float> size =
        DrawnSizeOf(StreamBytes(AppearanceOf(widget.get()).Get()));
    ASSERT_TRUE(size);
    RetainPtr<CPDF_Font> font = FontOf(widget.get());
    ASSERT_TRUE(font);
    const float line =
        (font->GetTypeAscent() - font->GetTypeDescent()) * *size / 1000;
    EXPECT_NEAR(kHeights[i] - 2, line, 0.01f);
    EXPECT_LT(WidthOf(font.Get(), "Hello world", *size), 216);
  }
}

// A field of several lines is at most 12, as in Acrobat, and smaller until
// the wrapped text fits: 220 by 80 holds the paragraph at 12, 220 by 40 at
// a size between two of PDFium's steps (Acrobat: 12 and 8.25).
TEST_F(EPDFWidgetTextSizeEmbedderTest, AFieldOfSeveralLinesIsAtMostTwelve) {
  constexpr char kParagraph[] =
      "Automatic text size fits this paragraph into the box. A longer "
      "paragraph wraps onto more lines, and the size goes down until every "
      "line fits inside the border of the field.";
  const float kBoxes[][2] = {{220, 80}, {220, 40}};
  std::vector<std::string> widgets;
  for (const auto& box : kBoxes) {
    widgets.push_back("/FT /Tx /Ff 4096 /T (lines" +
                      std::to_string(widgets.size()) + ") /V (" + kParagraph +
                      ") " + RectOf(box[0], box[1]));
  }
  DrawFields(widgets);
  const std::optional<float> roomy =
      DrawnSizeOf(StreamBytes(AppearanceOf(Widget(0).get()).Get()));
  ASSERT_TRUE(roomy);
  EXPECT_EQ(12, *roomy);
  {
    const std::optional<float> size =
        DrawnSizeOf(StreamBytes(AppearanceOf(Widget(1).get()).Get()));
    ASSERT_TRUE(size);
    EXPECT_LT(*size, 12);
    EXPECT_GT(*size, 4);
    EXPECT_FALSE(OnTheLadder(*size));
  }
}

// A list box's automatic size fits its widest option to its width inside
// twice the border at each side, at most 12: Acrobat draws the options of
// a list 120 wide at 8.199, and of one 200 wide at 12. Every option is
// drawn at that size.
TEST_F(EPDFWidgetTextSizeEmbedderTest, AListBoxFitsItsWidestOption) {
  constexpr char kOptions[] =
      "/Opt [(Option one) (A much longer option label here) (Three) (Four) "
      "(Five) (Six)] /I [2] /V (Three)";
  DrawFields({
      std::string("/FT /Ch /T (narrow) ") + kOptions + " " + RectOf(120, 60),
      std::string("/FT /Ch /T (wide) ") + kOptions + " " + RectOf(200, 40),
  });
  const std::string narrow = StreamBytes(AppearanceOf(Widget(0).get()).Get());
  const std::optional<float> size = DrawnSizeOf(narrow);
  ASSERT_TRUE(size);
  // The tests' stand-in for Helvetica is 0.2% narrower than Acrobat's.
  EXPECT_NEAR(8.199f, *size, 0.02f);
  RetainPtr<CPDF_Font> font = FontOf(Widget(0).get());
  ASSERT_TRUE(font);
  EXPECT_NEAR(116,
              WidthOf(font.Get(), "A much longer option label here", *size),
              0.03f);
  EXPECT_NE(std::string::npos, narrow.find("Six"));
  const std::optional<float> wide =
      DrawnSizeOf(StreamBytes(AppearanceOf(Widget(1).get()).Get()));
  ASSERT_TRUE(wide);
  EXPECT_EQ(12, *wide);
}

// A check box's check is as large as Acrobat's: ZapfDingbats' check mark at
// the size whose font box (0.963 of the size high) fills the box inside its
// border less the border's width at each side, so the mark is 0.719 / 0.963
// of the box less 4 high with a border of 1 (Acrobat: 11.92 in a box of 20,
// 34.32 in one of 50), in the middle.
TEST_F(EPDFWidgetTextSizeEmbedderTest, ACheckIsAsLargeAsAcrobats) {
  const float kSides[] = {10, 14, 20, 30, 50};
  std::vector<std::string> widgets;
  for (float side : kSides) {
    widgets.push_back("/FT /Btn /T (check" + std::to_string(widgets.size()) +
                      ") /V /Yes /AS /Yes /MK << /BC [0 0 0] /CA (4) >> " +
                      RectOf(side, side));
  }
  DrawFields(widgets);
  for (size_t i = 0; i < std::size(kSides); ++i) {
    const float side = kSides[i];
    SCOPED_TRACE(side);
    ScopedFPDFAnnotation widget = Widget(static_cast<int>(i));
    const std::string checked = StreamBytes(AppearanceOf(widget.get()).Get());
    // The check comes after the box's own block.
    const size_t box_end = checked.find("Q\n");
    ASSERT_NE(std::string::npos, box_end);
    const CFX_FloatRect mark = PathBoxOf(checked, box_end + 2);
    EXPECT_NEAR(0.719f / 0.963f * (side - 4), mark.Height(), 1e-2f);
    EXPECT_NEAR(side, mark.left + mark.right, 1e-2f);
    EXPECT_NEAR(side, mark.bottom + mark.top, 1e-2f);
  }
}
