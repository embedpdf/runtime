// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_richtextlayout.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_documentfonts.h"
#include "core/fpdfdoc/cpdf_richtext.h"
#include "core/fpdfdoc/cpvt_fontmap.h"
#include "core/fpdfdoc/cpvt_variabletext.h"
#include "core/fpdfdoc/cpvt_word.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_font.h"
#include "public/fpdf_edit.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
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
    arimo_ = Register("Arimo", TestFont("Arimo-Regular.ttf"));
    ASSERT_NE(0u, arimo_);
    doc_.reset(FPDF_CreateNewDocument());
    ASSERT_TRUE(doc_);
  }

  void TearDown() override {
    fonts_.reset();
    doc_.reset();
    EPDFFont_ClearRegisteredFonts();
    EmbedderTest::TearDown();
  }

  static std::string TestFont(const char* name) {
    return PathService::GetThirdPartyFilePath(
        std::string("test_fonts/test_fonts/") + name);
  }

  static EPDF_FONT_ID Register(const char* family, const std::string& path) {
    const std::vector<uint8_t> data = GetFileContents(path.c_str());
    EXPECT_FALSE(data.empty()) << path;
    return EPDFFont_RegisterMemFont64(family, /*weight=*/400, /*italic=*/0,
                                      data.data(), data.size());
  }

  // The registered font |run| draws with; 0 for any other.
  EPDF_FONT_ID FontOf(const GlyphRun& run) const {
    return fonts_->DescribeEntry(run.font_entry).registered_font_id;
  }

  CPDF_DocumentFonts* DocumentFonts() {
    return CPDF_DocumentFonts::For(CPDFDocumentFromFPDFDocument(doc_.get()));
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
    fonts_ = std::make_unique<CPDF_AnnotFontMap>(
        CPDFDocumentFromFPDFDocument(doc_.get()),
        /*default_font=*/nullptr, ByteString(),
        /*allow_registered_fallbacks=*/true);
    CPDF_RichTextLayout layout(fonts_.get(), options);
    return layout
        .Arrange(document, CFX_FloatRect(0, 0, width, 300),
                 CPDF_Annot::VerticalAlignment::kTop)
        .lines;
  }

  EPDF_FONT_ID arimo_ = 0;
  ScopedFPDFDocument doc_;
  // The fonts of the last Arrange().
  std::unique_ptr<CPDF_AnnotFontMap> fonts_;
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

// A cluster goes to the first fallback, in order, that has all of it. Gelasio
// has "e" but not the combining acute, so "e" + U+0301 goes to Arimo, added
// after it; Helvetica, the face, has neither whole.
TEST_F(EPDFRichTextLayoutEmbedderTest, FallbackHasTheWholeCluster) {
  const EPDF_FONT_ID gelasio =
      Register("Gelasio", TestFont("Gelasio-Regular.ttf"));
  ASSERT_NE(0u, gelasio);
  ASSERT_TRUE(EPDFFont_AddFallbackFont(gelasio, /*tier=*/0));
  ASSERT_TRUE(EPDFFont_AddFallbackFont(arimo_, /*tier=*/0));
  const std::vector<Line> lines =
      Arrange(L"e\x0301", /*rtl=*/false, kBoxWidth, "Helvetica");
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(1u, lines[0].runs.size());
  EXPECT_EQ(arimo_, FontOf(lines[0].runs[0]));
  EXPECT_TRUE(DocumentFonts()->missing_characters().empty());
}

// Fallbacks are tried tier by tier: Tinos, added after Arimo in a lower tier,
// draws the Hebrew both have.
TEST_F(EPDFRichTextLayoutEmbedderTest, FallbackTiers) {
  const EPDF_FONT_ID tinos = Register("Tinos", TestFont("Tinos-Regular.ttf"));
  ASSERT_NE(0u, tinos);
  ASSERT_TRUE(EPDFFont_AddFallbackFont(arimo_, /*tier=*/1));
  ASSERT_TRUE(EPDFFont_AddFallbackFont(tinos, /*tier=*/0));
  const std::vector<Line> lines =
      Arrange(L"\x05E9\x05DC", /*rtl=*/false, kBoxWidth, "Helvetica");
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(1u, lines[0].runs.size());
  EXPECT_EQ(tinos, FontOf(lines[0].runs[0]));
}

// A character no font has is drawn with the last resort, as its box, and is
// still reported; one that draws nothing stays with the face.
TEST_F(EPDFRichTextLayoutEmbedderTest, LastResortDrawsWhatNoFontHas) {
  // Without a last resort, the face draws its missing glyph (a run of its
  // own: another script).
  std::vector<Line> lines = Arrange(
      L"A\x4E2D"
      L"B",
      /*rtl=*/false);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0].runs.size());
  for (const GlyphRun& run : lines[0].runs) {
    EXPECT_EQ(arimo_, FontOf(run));
  }

  const EPDF_FONT_ID box = Register(
      "Last Resort", PathService::GetTestFilePath("fonts/last_resort.ttf"));
  ASSERT_NE(0u, box);
  ASSERT_TRUE(EPDFFont_SetLastResortFont(box));
  DocumentFonts()->ClearMissingCharacters();
  lines = Arrange(
      L"A\x4E2D"
      L"B",
      /*rtl=*/false);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0].runs.size());
  EXPECT_EQ(arimo_, FontOf(lines[0].runs[0]));
  EXPECT_EQ(box, FontOf(lines[0].runs[1]));
  EXPECT_EQ(arimo_, FontOf(lines[0].runs[2]));
  ASSERT_EQ(1u, lines[0].runs[1].glyphs.size());
  EXPECT_NE(0u, lines[0].runs[1].glyphs[0].gid);
  EXPECT_EQ(std::vector<WideString>{L"\x4E2D"},
            DocumentFonts()->missing_characters());

  // An invisible separator Arimo lacks is hidden by it, not boxed.
  lines = Arrange(
      L"A\x2063"
      L"B",
      /*rtl=*/false);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(1u, lines[0].runs.size());
  EXPECT_EQ(arimo_, FontOf(lines[0].runs[0]));
}

namespace {

// A character a field draws, where its baseline starts.
struct PlacedCharacter {
  wchar_t ch;
  float x;
  float y;
};

}  // namespace

// The field layout keeps the geometry of PDFium's form filler (CPVT) where
// CPVT drew correctly: the same characters at the same places, for the
// standard fonts, the sizes, fitted sizes, alignments, several lines, long
// words, passwords, limits and combs the forms parity gate draws.
TEST_F(EPDFRichTextLayoutEmbedderTest, FieldMatchesCpvt) {
  struct Case {
    const char* font;
    const wchar_t* text;
    float size;
    int alignment;
    bool multiline;
    bool password;
    size_t limit;
    size_t cells;
    CFX_FloatRect plate;
  };
  const CFX_FloatRect line(20, 20, 180, 44);
  const CFX_FloatRect block(20, 20, 170, 110);
  const wchar_t* const kParagraphs =
      L"The quick brown fox jumps over the lazy dog, again and again.\n"
      L"Second paragraph here\n\nAfter a blank line";
  std::vector<Case> cases;
  for (const char* font : {"Helvetica", "Helvetica-Bold", "Times-Roman",
                           "Times-BoldItalic", "Courier-BoldOblique"}) {
    for (float size : {0.0f, 8.0f, 11.0f, 14.0f}) {
      cases.push_back({font,
                       L"H\x00E9g, quick \x20AC"
                       L"12.50 \x201C"
                       L"fox\x201D",
                       size, 0, false, false, 0, 0, line});
    }
  }
  for (int alignment : {0, 1, 2}) {
    for (const wchar_t* text : {L"Wide WWW text", L"i", L""}) {
      cases.push_back(
          {"Helvetica", text, 10, alignment, false, false, 0, 0, line});
    }
    for (float size : {0.0f, 9.0f}) {
      cases.push_back({"Helvetica", kParagraphs, size, alignment, true, false,
                       0, 0, block});
    }
    for (size_t cells : {6u, 10u}) {
      cases.push_back({"Courier", L"AiW1\x00E9", 12, alignment, false, false, 0,
                       cells, line});
    }
  }
  cases.push_back({"Helvetica",
                   L"A long value that will not fit in this box at all", 0, 0,
                   false, false, 0, 0, line});
  cases.push_back({"Times-Roman", L"Tall", 0, 0, false, false, 0, 0,
                   CFX_FloatRect(20, 20, 140, 90)});
  cases.push_back({"Times-Roman", L"Supercalifragilisticexpialidocious word",
                   12, 0, true, false, 0, 0, CFX_FloatRect(20, 20, 100, 80)});
  cases.push_back({"Helvetica",
                   L"Words (in brackets) and [more] text {here} to (wrap)", 11,
                   0, true, false, 0, 0, CFX_FloatRect(20, 20, 95, 110)});
  cases.push_back({"Helvetica", L"secret", 10, 0, false, true, 0, 0, line});
  cases.push_back(
      {"Helvetica", L"truncate me please", 10, 0, false, false, 8, 0, line});
  cases.push_back({"Helvetica", L"AiWm.", 12, 0, false, false, 0, 7, line});
  cases.push_back({"Helvetica", L"1234", 0, 0, false, false, 0, 4, line});

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(doc_.get());
  for (const Case& c : cases) {
    SCOPED_TRACE(testing::Message()
                 << c.font << " " << c.size << " q" << c.alignment
                 << (c.multiline ? " multiline" : "")
                 << (c.password ? " password" : "") << " limit " << c.limit
                 << " cells " << c.cells << " \"" << WideString(c.text).ToUTF8()
                 << "\"");
    RetainPtr<CPDF_Font> font = CPDF_Font::GetStockFont(doc, c.font);
    ASSERT_TRUE(font);
    const bool centered = !c.multiline;

    // CPVT, as the form generator drove it.
    std::vector<PlacedCharacter> expected;
    {
      CPVT_FontMap cpvt_fonts(doc, /*resources=*/nullptr, font, "F");
      CPVT_VariableText::Provider provider(&cpvt_fonts);
      CPVT_VariableText vt(&provider);
      vt.SetPlateRect(c.plate);
      vt.SetAlignment(CPVT_VariableText::ToAlignment(c.alignment));
      if (c.size == 0) {
        vt.SetAutoFontSize(true);
      } else {
        vt.SetFontSize(c.size);
      }
      if (c.multiline) {
        vt.SetMultiLine(true);
        vt.SetAutoReturn(true);
      }
      if (c.password) {
        vt.SetPasswordChar('*');
      }
      if (c.cells > 0) {
        vt.SetCharArray(static_cast<int32_t>(c.cells));
      } else {
        vt.SetLimitChar(static_cast<int32_t>(c.limit));
      }
      vt.Initialize();
      vt.SetText(c.text);
      vt.RearrangeAll();
      const CFX_PointF offset(
          0, centered ? (vt.GetContentRect().Height() - c.plate.Height()) / 2.0f
                      : 0);
      CPVT_VariableText::Iterator* it = vt.GetIterator();
      it->SetAt(0);
      while (it->NextWord()) {
        CPVT_Word word;
        if (it->GetWord(word)) {
          const CFX_PointF at = word.location() + offset;
          expected.push_back(
              {c.password ? L'*' : static_cast<wchar_t>(word.word()), at.x,
               at.y});
        }
      }
    }

    // The field layout.
    std::vector<PlacedCharacter> actual;
    {
      CPDF_AnnotFontMap map(doc, font, "F",
                            /*allow_registered_fallbacks=*/false);
      ASSERT_TRUE(map.HasDefaultFont());
      CPDF_RichTextLayout::Field field;
      field.text = c.text;
      field.face_entry = 0;
      field.size = c.size;
      field.alignment = c.alignment;
      field.multiline = c.multiline;
      field.password = c.password;
      field.limit = c.limit;
      field.cells = c.cells;
      field.centered = centered;
      CPDF_RichTextLayout layout(&map, {});
      const CPDF_RichTextLayout::Result laid =
          layout.ArrangeField(field, c.plate);
      for (const Line& laid_line : laid.lines) {
        const float y = c.plate.top - laid_line.baseline_y;
        for (const GlyphRun& run : laid_line.runs) {
          float x = c.plate.left + run.x;
          for (const CPDF_RichTextLayout::ShapedGlyph& glyph : run.glyphs) {
            actual.push_back({run.text[glyph.cluster], x, y});
            x += glyph.x_advance;
          }
        }
      }
    }

    ASSERT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      SCOPED_TRACE(i);
      EXPECT_EQ(expected[i].ch, actual[i].ch);
      EXPECT_NEAR(expected[i].x, actual[i].x, 0.001f);
      EXPECT_NEAR(expected[i].y, actual[i].y, 0.001f);
    }
  }
}
