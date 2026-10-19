// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_richtextlayout.h"

#include <stdint.h>

#include <string>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_richtext.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_font.h"
#include "public/fpdf_edit.h"
#include "testing/embedder_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

namespace {

using Line = CPDF_RichTextLayout::Line;
using GlyphRun = CPDF_RichTextLayout::GlyphRun;
using Align = CPDF_RichTextParagraphProps::Align;

constexpr float kBoxWidth = 400.0f;
constexpr float kSize = 12.0f;

// A line's runs as drawn, left to right: "[text]level" each.
std::wstring Describe(const Line& line) {
  std::wstring description;
  for (const GlyphRun& run : line.runs) {
    description += L"[";
    description += run.text.c_str();
    description += L"]";
    description += std::to_wstring(run.bidi_level);
  }
  return description;
}

}  // namespace

// The rich text layout with real fonts. Arimo covers Latin, digits and Hebrew
// with its points; Helvetica is a standard font, drawn without HarfBuzz.
class EPDFRichTextLayoutEmbedderTest : public EmbedderTest {
 protected:
  void SetUp() override {
    EmbedderTest::SetUp();
    EPDFFont_ClearRegisteredFonts();
    const std::string path = PathService::GetThirdPartyFilePath(
        "test_fonts/test_fonts/Arimo-Regular.ttf");
    ASSERT_FALSE(path.empty());
    arimo_ = GetFileContents(path.c_str());
    ASSERT_FALSE(arimo_.empty());
    ASSERT_NE(0u, EPDFFont_RegisterMemFont64("Arimo", /*weight=*/400,
                                             /*italic=*/0, arimo_.data(),
                                             arimo_.size()));
    doc_.reset(FPDF_CreateNewDocument());
    ASSERT_TRUE(doc_);
  }

  void TearDown() override {
    doc_.reset();
    EPDFFont_ClearRegisteredFonts();
    EmbedderTest::TearDown();
  }

  // One paragraph of |text| in |family| at 12 pt, in a box |width| wide.
  std::vector<Line> Arrange(const WideString& text,
                            bool rtl,
                            float width = kBoxWidth,
                            const char* family = "Arimo",
                            CPDF_RichTextLayout::Options options = {}) {
    CPDF_RichTextParagraphProps props;
    props.rtl = rtl;
    return Arrange(text, props, width, family, options);
  }

  std::vector<Line> Arrange(const WideString& text,
                            const CPDF_RichTextParagraphProps& props,
                            float width = kBoxWidth,
                            const char* family = "Arimo",
                            CPDF_RichTextLayout::Options options = {}) {
    CPDF_RichTextDocument document;
    document.body.family = WideString::FromASCII(family);
    document.body.size = kSize;
    CPDF_RichTextParagraph paragraph;
    paragraph.props = props;
    paragraph.runs.push_back({text, {}});
    document.paragraphs.push_back(std::move(paragraph));
    CPDF_AnnotFontMap fonts(CPDFDocumentFromFPDFDocument(doc_.get()),
                            /*default_font=*/nullptr, ByteString(),
                            /*allow_registered_fallbacks=*/true);
    CPDF_RichTextLayout layout(&fonts, options);
    return layout
        .Arrange(document, CFX_FloatRect(0, 0, width, 300),
                 CPDF_Annot::VerticalAlignment::kTop)
        .lines;
  }

  std::vector<uint8_t> arimo_;
  ScopedFPDFDocument doc_;
};

// Hebrew and digits in a left-to-right paragraph: the digits take the
// Hebrew's side (they read as right to left between strong characters), and
// the Hebrew with its space and digits reads from the right.
TEST_F(EPDFRichTextLayoutEmbedderTest, LeftToRightParagraphOrdersHebrew) {
  const std::vector<Line> lines =
      Arrange(L"Hello \x05E9\x05DC\x05D5\x05DD 123", /*rtl=*/false);
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(L"[Hello ]0[123]2[\x05E9\x05DC\x05D5\x05DD ]1", Describe(lines[0]));
  // Drawn edge to edge from the left.
  float x = 0;
  for (const GlyphRun& run : lines[0].runs) {
    EXPECT_FLOAT_EQ(x, run.x);
    x += run.width;
  }
  EXPECT_TRUE(lines[0].runs[2].rtl);
  EXPECT_TRUE(lines[0].runs[2].needs_actual_text);
  EXPECT_FALSE(lines[0].runs[1].rtl);
}

// A right-to-left paragraph starting with digits: they still read left to
// right, at its right end; the line is right-aligned.
TEST_F(EPDFRichTextLayoutEmbedderTest, RightToLeftParagraphStartingWithDigits) {
  const std::vector<Line> lines =
      Arrange(L"123 \x05E9\x05DC\x05D5\x05DD", /*rtl=*/true);
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(L"[ \x05E9\x05DC\x05D5\x05DD]1[123]2", Describe(lines[0]));
  EXPECT_NEAR(kBoxWidth, lines[0].start_x + lines[0].width, 0.01f);
  EXPECT_NEAR(kBoxWidth, lines[0].runs.back().x + lines[0].runs.back().width,
              0.01f);
}

// A form field's paragraph reads in the direction of its first strong
// character, whatever its dir says.
TEST_F(EPDFRichTextLayoutEmbedderTest, DirectionFromText) {
  CPDF_RichTextLayout::Options options;
  options.direction_from_text = true;
  std::vector<Line> lines = Arrange(L"123 \x05E9\x05DC\x05D5\x05DD",
                                    /*rtl=*/false, kBoxWidth, "Arimo", options);
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(L"[ \x05E9\x05DC\x05D5\x05DD]1[123]2", Describe(lines[0]));

  lines = Arrange(L"abc \x05E9\x05DC\x05D5\x05DD", /*rtl=*/true, kBoxWidth,
                  "Arimo", options);
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(L"[abc ]0[\x05E9\x05DC\x05D5\x05DD]1", Describe(lines[0]));
}

// Breaking a right-to-left paragraph: the space that ends a line goes to the
// paragraph's level, so it trails at the left, past the edge the line is
// aligned to, whether it ran left to right before the break or not.
TEST_F(EPDFRichTextLayoutEmbedderTest, TrailingSpaceHangsAtTheLeft) {
  // Two Hebrew words, one per line.
  std::vector<Line> lines =
      Arrange(L"\x05E9\x05DC\x05D5\x05DD \x05E2\x05D5\x05DC\x05DD",
              /*rtl=*/true, /*width=*/30);
  ASSERT_EQ(2u, lines.size());
  EXPECT_EQ(L"[\x05E9\x05DC\x05D5\x05DD ]1", Describe(lines[0]));
  const GlyphRun& hebrew = lines[0].runs[0];
  EXPECT_LT(hebrew.x, lines[0].start_x);
  EXPECT_NEAR(30.0f, hebrew.x + hebrew.width, 0.01f);

  // Two Latin words: the space ran at the words' level, 2.
  lines = Arrange(L"abc def", /*rtl=*/true, /*width=*/25);
  ASSERT_EQ(2u, lines.size());
  EXPECT_EQ(L"[ ]1[abc]2", Describe(lines[0]));
  EXPECT_NEAR(lines[0].start_x - lines[0].runs[0].width, lines[0].runs[0].x,
              0.01f);
  EXPECT_NEAR(lines[0].start_x, lines[0].runs[1].x, 0.01f);
  EXPECT_NEAR(25.0f, lines[0].start_x + lines[0].width, 0.01f);
  EXPECT_EQ(L"[def]2", Describe(lines[1]));
}

// Hebrew points are marks: they stay with their letters, in one run.
TEST_F(EPDFRichTextLayoutEmbedderTest, HebrewPointsStayWithTheirLetters) {
  const std::vector<Line> lines =
      Arrange(L"\x05E9\x05C1\x05B8\x05DC\x05D5\x05B9\x05DD", /*rtl=*/true);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(1u, lines[0].runs.size());
  EXPECT_EQ(1, lines[0].runs[0].bidi_level);
  EXPECT_FALSE(lines[0].runs[0].degraded);
}

// Brackets read right to left are mirrored: by HarfBuzz for a font program,
// by the layout for a standard font.
TEST_F(EPDFRichTextLayoutEmbedderTest, RightToLeftBracketsAreMirrored) {
  for (const char* family : {"Arimo", "Helvetica"}) {
    SCOPED_TRACE(family);
    std::vector<Line> lines = Arrange(L"()", /*rtl=*/false, kBoxWidth, family);
    ASSERT_EQ(1u, lines.size());
    ASSERT_EQ(1u, lines[0].runs.size());
    ASSERT_EQ(2u, lines[0].runs[0].glyphs.size());
    const uint32_t open = lines[0].runs[0].glyphs[0].gid;
    const uint32_t close = lines[0].runs[0].glyphs[1].gid;
    ASSERT_NE(open, close);

    // "(abc)" in a right-to-left paragraph: the brackets at level 1 around
    // the Latin at level 2. Logical ")" is drawn first, as "(".
    lines = Arrange(L"(abc)", /*rtl=*/true, kBoxWidth, family);
    ASSERT_EQ(1u, lines.size());
    EXPECT_EQ(L"[)]1[abc]2[(]1", Describe(lines[0]));
    ASSERT_EQ(1u, lines[0].runs[0].glyphs.size());
    EXPECT_EQ(open, lines[0].runs[0].glyphs[0].gid);
    ASSERT_EQ(1u, lines[0].runs[2].glyphs.size());
    EXPECT_EQ(close, lines[0].runs[2].glyphs[0].gid);
  }
}

// The first line's indent is on the side the paragraph starts from.
TEST_F(EPDFRichTextLayoutEmbedderTest, IndentOnTheStartSide) {
  CPDF_RichTextParagraphProps props;
  props.text_indent = 10;
  std::vector<Line> lines = Arrange(L"abc", props);
  ASSERT_EQ(1u, lines.size());
  EXPECT_NEAR(10.0f, lines[0].start_x, 0.01f);

  props.rtl = true;
  lines = Arrange(L"\x05E9\x05DC\x05D5\x05DD", props);
  ASSERT_EQ(1u, lines.size());
  EXPECT_NEAR(kBoxWidth - 10.0f, lines[0].start_x + lines[0].width, 0.01f);
}

// A justified line's ink reaches both edges: the spaces between its words
// stretch, the space it ends with doesn't.
TEST_F(EPDFRichTextLayoutEmbedderTest, JustifiedInkReachesBothEdges) {
  CPDF_RichTextParagraphProps props;
  props.align = Align::kJustify;
  const std::vector<Line> lines = Arrange(L"aaa bbb ccc", props, /*width=*/50);
  ASSERT_EQ(2u, lines.size());
  ASSERT_EQ(1u, lines[0].runs.size());
  const GlyphRun& run = lines[0].runs[0];
  EXPECT_EQ(L"aaa bbb ", std::wstring(run.text.c_str()));
  EXPECT_NEAR(0.0f, run.x, 0.01f);
  float x = run.x;
  float ink_end = x;
  for (const CPDF_RichTextLayout::ShapedGlyph& glyph : run.glyphs) {
    x += glyph.x_advance;
    if (run.text[glyph.cluster] != L' ') {
      ink_end = x;
    }
  }
  EXPECT_NEAR(50.0f, ink_end, 0.01f);
}
