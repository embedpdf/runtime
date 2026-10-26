// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Text geometry and text maps (public/epdf_text.h).

#include "public/epdf_text.h"

#include <cstdint>

#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_read_only_graph_guard.h"
#include "core/fxcrt/notreached.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdf_text.h"
#include "testing/embedder_test.h"
#include "testing/gmock/include/gmock/gmock.h"

using ::testing::ElementsAreArray;

namespace {

// The same as in fpdf_text_embeddertest.cpp.
int GetRotatedTextFirstCharIndexForQuadrant(int quadrant) {
  // Unlike hello_world.pdf, rotated_text.pdf has an extra space before
  // "Goodbye".
  static constexpr size_t kSubstringsSize[] = {
      std::char_traits<char>::length("Hello, "),
      std::char_traits<char>::length("world!\r\n "),
      std::char_traits<char>::length("Goodbye, ")};
  switch (quadrant) {
    case 0:
      return 0;
    case 1:
      return kSubstringsSize[0];
    case 2:
      return kSubstringsSize[0] + kSubstringsSize[1];
    case 3:
      return kSubstringsSize[0] + kSubstringsSize[1] + kSubstringsSize[2];
    default:
      NOTREACHED();
  }
}

CFX_FloatRect BoundsFromQuad(const FS_QUADPOINTSF& quad) {
  CFX_FloatRect bounds(quad.x1, quad.y1, quad.x1, quad.y1);
  bounds.UpdateRect({quad.x2, quad.y2});
  bounds.UpdateRect({quad.x3, quad.y3});
  bounds.UpdateRect({quad.x4, quad.y4});
  return bounds;
}

}  // namespace

class EPDFTextEmbedderTest : public EmbedderTest {};

TEST_F(EPDFTextEmbedderTest, GetCharGeometry) {
  ASSERT_TRUE(OpenDocument("font_matrix.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFTextPage text_page(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(text_page);

  EPDF_CHAR_GEOMETRY geometry = {};
  geometry.flags = 0xffffffffu;
  EXPECT_FALSE(EPDFText_GetCharGeometry(nullptr, 0, &geometry));
  EXPECT_EQ(0xffffffffu, geometry.flags);
  EXPECT_FALSE(EPDFText_GetCharGeometry(text_page.get(), -1, &geometry));
  EXPECT_EQ(0xffffffffu, geometry.flags);
  EXPECT_FALSE(EPDFText_GetCharGeometry(text_page.get(), 10, &geometry));
  EXPECT_EQ(0xffffffffu, geometry.flags);
  EXPECT_FALSE(EPDFText_GetCharGeometry(text_page.get(), 0, nullptr));

  ASSERT_TRUE(EPDFText_GetCharGeometry(text_page.get(), 0, &geometry));
  EXPECT_EQ(EPDF_CHARGEO_HAS_TIGHT_BOX | EPDF_CHARGEO_HAS_LOOSE_QUAD |
                EPDF_CHARGEO_HAS_TIGHT_QUAD | EPDF_CHARGEO_UPRIGHT,
            geometry.flags);

  FS_RECTF legacy_loose;
  ASSERT_TRUE(FPDFText_GetLooseCharBox(text_page.get(), 0, &legacy_loose));
  EXPECT_FLOAT_EQ(legacy_loose.left, geometry.loose_box.left);
  EXPECT_FLOAT_EQ(legacy_loose.right, geometry.loose_box.right);
  EXPECT_FLOAT_EQ(legacy_loose.bottom, geometry.loose_box.bottom);
  EXPECT_FLOAT_EQ(legacy_loose.top, geometry.loose_box.top);

  const CFX_FloatRect loose_quad_bounds = BoundsFromQuad(geometry.loose_quad);
  EXPECT_FLOAT_EQ(geometry.loose_box.left, loose_quad_bounds.left);
  EXPECT_FLOAT_EQ(geometry.loose_box.right, loose_quad_bounds.right);
  EXPECT_FLOAT_EQ(geometry.loose_box.bottom, loose_quad_bounds.bottom);
  EXPECT_FLOAT_EQ(geometry.loose_box.top, loose_quad_bounds.top);

  const CFX_FloatRect tight_quad_bounds = BoundsFromQuad(geometry.tight_quad);
  EXPECT_FLOAT_EQ(geometry.tight_box.left, tight_quad_bounds.left);
  EXPECT_FLOAT_EQ(geometry.tight_box.right, tight_quad_bounds.right);
  EXPECT_FLOAT_EQ(geometry.tight_box.bottom, tight_quad_bounds.bottom);
  EXPECT_FLOAT_EQ(geometry.tight_box.top, tight_quad_bounds.top);
}

// A text object with no width is left out of the text page (upstream
// 49042621c, Feb 2026), so a zero font size gives no characters to measure.
// Only a space between words stays (ChromeType3TextKeepsSpaces). An empty
// character box is covered by FPDFTextEmbedderTest.Bug399689604.
TEST_F(EPDFTextEmbedderTest, ZeroFontSizeTextHasNoChars) {
  ASSERT_TRUE(OpenDocument("font_size_zero.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFTextPage text_page(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(text_page);
  EXPECT_EQ(0, FPDFText_CountChars(text_page.get()));
}

// Chrome and Skia print each glyph as its own text object, so a space is an
// object without ink, and so without width. It still separates the words.
TEST_F(EPDFTextEmbedderTest, ChromeType3TextKeepsSpaces) {
  ASSERT_TRUE(OpenDocument("embedpdf_chrome_type3_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFTextPage text_page(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(text_page);

  static constexpr char kExpected[] = "to pixels";
  static constexpr int kExpectedSize = std::size(kExpected);
  ASSERT_EQ(kExpectedSize - 1, FPDFText_CountChars(text_page.get()));
  unsigned short buffer[16] = {};
  ASSERT_EQ(kExpectedSize,
            FPDFText_GetText(text_page.get(), 0, std::size(buffer), buffer));
  EXPECT_THAT(pdfium::span(buffer).first<kExpectedSize>(),
              ElementsAreArray(kExpected));
}

// A Type3 font has no ascent or descent, so its characters' loose boxes take
// the top and bottom of /FontBBox: one line, one height, whatever each glyph's
// ink. The space between the words has the same box.
TEST_F(EPDFTextEmbedderTest, ChromeType3TextLooseBoxesShareTheLine) {
  ASSERT_TRUE(OpenDocument("embedpdf_chrome_type3_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFTextPage text_page(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(text_page);

  // 20pt text on a baseline at y = 50; /FontBBox runs from 200 below the
  // baseline to 750 above it, in thousandths of the font size.
  static constexpr float kTop = 65.0f;
  static constexpr float kBottom = 46.0f;
  const int char_count = FPDFText_CountChars(text_page.get());
  ASSERT_GT(char_count, 0);
  for (int i = 0; i < char_count; ++i) {
    FS_RECTF loose;
    ASSERT_TRUE(FPDFText_GetLooseCharBox(text_page.get(), i, &loose));
    EXPECT_FLOAT_EQ(kTop, loose.top) << i;
    EXPECT_FLOAT_EQ(kBottom, loose.bottom) << i;

    EPDF_CHAR_GEOMETRY geometry = {};
    ASSERT_TRUE(EPDFText_GetCharGeometry(text_page.get(), i, &geometry));
    EXPECT_FLOAT_EQ(kTop, geometry.loose_box.top) << i;
    EXPECT_FLOAT_EQ(kBottom, geometry.loose_box.bottom) << i;
  }
}

TEST_F(EPDFTextEmbedderTest, GetRotatedCharGeometry) {
  ASSERT_TRUE(OpenDocument("rotated_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFTextPage text_page(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(text_page);

  EPDF_CHAR_GEOMETRY geometry = {};
  ASSERT_TRUE(EPDFText_GetCharGeometry(
      text_page.get(), GetRotatedTextFirstCharIndexForQuadrant(0), &geometry));
  EXPECT_TRUE(geometry.flags & EPDF_CHARGEO_HAS_LOOSE_QUAD);
  EXPECT_TRUE(geometry.flags & EPDF_CHARGEO_HAS_TIGHT_QUAD);
  EXPECT_FALSE(geometry.flags & EPDF_CHARGEO_UPRIGHT);

  const CFX_FloatRect loose_quad_bounds = BoundsFromQuad(geometry.loose_quad);
  EXPECT_FLOAT_EQ(geometry.loose_box.left, loose_quad_bounds.left);
  EXPECT_FLOAT_EQ(geometry.loose_box.right, loose_quad_bounds.right);
  EXPECT_FLOAT_EQ(geometry.loose_box.bottom, loose_quad_bounds.bottom);
  EXPECT_FLOAT_EQ(geometry.loose_box.top, loose_quad_bounds.top);

  FS_RECTF legacy_loose;
  ASSERT_TRUE(FPDFText_GetLooseCharBox(
      text_page.get(), GetRotatedTextFirstCharIndexForQuadrant(0),
      &legacy_loose));
  EXPECT_LE(geometry.loose_box.left, legacy_loose.left);
  EXPECT_GE(geometry.loose_box.right, legacy_loose.right);
  EXPECT_LE(geometry.loose_box.bottom, legacy_loose.bottom);
  EXPECT_GE(geometry.loose_box.top, legacy_loose.top);
}

TEST_F(EPDFTextEmbedderTest, GetShearedCharGeometry) {
  ASSERT_TRUE(OpenDocument("sheared_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFTextPage text_page(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(text_page);
  ASSERT_EQ(2, FPDFText_CountChars(text_page.get()));

  EPDF_CHAR_GEOMETRY geometry = {};
  ASSERT_TRUE(EPDFText_GetCharGeometry(text_page.get(), 0, &geometry));
  EXPECT_TRUE(geometry.flags & EPDF_CHARGEO_HAS_LOOSE_QUAD);
  EXPECT_TRUE(geometry.flags & EPDF_CHARGEO_HAS_TIGHT_QUAD);
  EXPECT_FALSE(geometry.flags & EPDF_CHARGEO_UPRIGHT);
  EXPECT_FLOAT_EQ(1.0f, geometry.matrix.a);
  EXPECT_FLOAT_EQ(0.0f, geometry.matrix.b);
  EXPECT_FLOAT_EQ(0.5f, geometry.matrix.c);
  EXPECT_FLOAT_EQ(1.0f, geometry.matrix.d);

  // The cell is the exact sheared parallelogram, never an axis-aligned
  // approximation: both side edges equal M * (0, h) = (0.5 * h, h).
  const FS_QUADPOINTSF& quad = geometry.loose_quad;
  EXPECT_FLOAT_EQ(quad.y1, quad.y2);  // upper edge stays horizontal (b == 0)
  EXPECT_FLOAT_EQ(quad.y3, quad.y4);  // lower edge stays horizontal
  EXPECT_FLOAT_EQ(quad.x1 - quad.x3, quad.x2 - quad.x4);
  EXPECT_FLOAT_EQ(quad.y1 - quad.y3, quad.y2 - quad.y4);
  EXPECT_FLOAT_EQ(quad.x1 - quad.x3, 0.5f * (quad.y1 - quad.y3));
  EXPECT_GT(quad.y1, quad.y3);  // upper corners sit on the ascent side

  const CFX_FloatRect loose_quad_bounds = BoundsFromQuad(quad);
  EXPECT_FLOAT_EQ(geometry.loose_box.left, loose_quad_bounds.left);
  EXPECT_FLOAT_EQ(geometry.loose_box.right, loose_quad_bounds.right);
  EXPECT_FLOAT_EQ(geometry.loose_box.bottom, loose_quad_bounds.bottom);
  EXPECT_FLOAT_EQ(geometry.loose_box.top, loose_quad_bounds.top);
}

TEST_F(EPDFTextEmbedderTest, GetMirroredCharGeometry) {
  ASSERT_TRUE(OpenDocument("mirrored_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFTextPage text_page(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(text_page);
  ASSERT_EQ(2, FPDFText_CountChars(text_page.get()));

  EPDF_CHAR_GEOMETRY geometry = {};
  ASSERT_TRUE(EPDFText_GetCharGeometry(text_page.get(), 0, &geometry));
  EXPECT_TRUE(geometry.flags & EPDF_CHARGEO_HAS_LOOSE_QUAD);
  EXPECT_TRUE(geometry.flags & EPDF_CHARGEO_HAS_TIGHT_QUAD);
  EXPECT_FALSE(geometry.flags & EPDF_CHARGEO_UPRIGHT);
  EXPECT_FLOAT_EQ(-1.0f, geometry.matrix.a);
  EXPECT_FLOAT_EQ(1.0f, geometry.matrix.d);

  // Corner slots keep frame-geometric semantics: "start" is the local
  // minimum-x corner, which the horizontal mirror maps to the page-space
  // right-hand side; "upper" stays the ascent side (d > 0).
  const FS_QUADPOINTSF& quad = geometry.loose_quad;
  EXPECT_GT(quad.x1, quad.x2);
  EXPECT_GT(quad.y1, quad.y3);

  const CFX_FloatRect loose_quad_bounds = BoundsFromQuad(quad);
  EXPECT_FLOAT_EQ(geometry.loose_box.left, loose_quad_bounds.left);
  EXPECT_FLOAT_EQ(geometry.loose_box.right, loose_quad_bounds.right);
  EXPECT_FLOAT_EQ(geometry.loose_box.bottom, loose_quad_bounds.bottom);
  EXPECT_FLOAT_EQ(geometry.loose_box.top, loose_quad_bounds.top);
}

TEST_F(EPDFTextEmbedderTest, GetVerticalCharGeometry) {
  ASSERT_TRUE(OpenDocument("embedpdf_vertical_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFTextPage text_page(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(text_page);

  int h_index = -1;
  for (int i = 0; i < FPDFText_CountChars(text_page.get()); ++i) {
    if (FPDFText_GetUnicode(text_page.get(), i) ==
        static_cast<uint32_t>('H')) {
      h_index = i;
      break;
    }
  }
  ASSERT_GE(h_index, 0);

  // Vertical CJK writing uses an upright matrix: the vertical-metrics loose
  // cell must stay available through the new API, self-consistent, and in
  // parity with the legacy loose box.
  EPDF_CHAR_GEOMETRY geometry = {};
  ASSERT_TRUE(EPDFText_GetCharGeometry(text_page.get(), h_index, &geometry));
  EXPECT_TRUE(geometry.flags & EPDF_CHARGEO_UPRIGHT);
  EXPECT_TRUE(geometry.flags & EPDF_CHARGEO_HAS_LOOSE_QUAD);

  const CFX_FloatRect loose_quad_bounds = BoundsFromQuad(geometry.loose_quad);
  EXPECT_FLOAT_EQ(geometry.loose_box.left, loose_quad_bounds.left);
  EXPECT_FLOAT_EQ(geometry.loose_box.right, loose_quad_bounds.right);
  EXPECT_FLOAT_EQ(geometry.loose_box.bottom, loose_quad_bounds.bottom);
  EXPECT_FLOAT_EQ(geometry.loose_box.top, loose_quad_bounds.top);

  FS_RECTF legacy_loose;
  ASSERT_TRUE(
      FPDFText_GetLooseCharBox(text_page.get(), h_index, &legacy_loose));
  EXPECT_FLOAT_EQ(legacy_loose.left, geometry.loose_box.left);
  EXPECT_FLOAT_EQ(legacy_loose.right, geometry.loose_box.right);
  EXPECT_FLOAT_EQ(legacy_loose.bottom, geometry.loose_box.bottom);
  EXPECT_FLOAT_EQ(legacy_loose.top, geometry.loose_box.top);
}

TEST_F(EPDFTextEmbedderTest, ReadPurityRenderType3Font) {
  ASSERT_TRUE(OpenDocument("bug_1591.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(doc);
  const uint32_t last_obj_num = doc->GetLastObjNum();

  {
    CPDF_ReadOnlyGraphGuard guard;
    ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
    ASSERT_TRUE(bitmap);
  }

  EXPECT_EQ(last_obj_num, doc->GetLastObjNum());
}

TEST_F(EPDFTextEmbedderTest, EPDFTextGetTextFullMatchesGetTextOnBmpText) {
  // bug_1139.pdf is BMP-only (31 characters, 30 text units — the first
  // character is non-printing), so the full-fidelity extraction must be
  // byte-identical to FPDFText_GetText, two-call contract included.
  ASSERT_TRUE(OpenDocument("bug_1139.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFTextPage textpage(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(textpage);

  const int required = EPDFText_GetTextFull(textpage.get(), nullptr, 0);
  ASSERT_EQ(31, required);  // 30 units + NUL

  unsigned short full[64] = {};
  ASSERT_EQ(required,
            EPDFText_GetTextFull(textpage.get(), full, std::size(full)));

  unsigned short legacy[64] = {};
  ASSERT_EQ(required,
            FPDFText_GetText(textpage.get(), 0,
                             FPDFText_CountChars(textpage.get()), legacy));
  EXPECT_THAT(pdfium::span(full).first(static_cast<size_t>(required)),
              ElementsAreArray(
                  pdfium::span(legacy).first(static_cast<size_t>(required))));

  // A too-small buffer reports the requirement and writes nothing.
  unsigned short tiny[4] = {0xABCD, 0xABCD, 0xABCD, 0xABCD};
  ASSERT_EQ(required, EPDFText_GetTextFull(textpage.get(), tiny, 4));
  EXPECT_EQ(0xABCD, tiny[0]);
}

TEST_F(EPDFTextEmbedderTest, EPDFTextGetCharToTextMapNonPrintingChar) {
  // bug_1139.pdf: character 0 is non-printing, so the map is the single
  // anchor {1, 0} — every later character shifts down by one text unit
  // (the same +1 the searchex conversion tests pin).
  ASSERT_TRUE(OpenDocument("bug_1139.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFTextPage textpage(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(textpage);

  ASSERT_EQ(1, EPDFText_GetCharToTextMap(textpage.get(), nullptr, 0));

  EPDF_CHAR_MAP_ANCHOR anchors[4] = {};
  ASSERT_EQ(1, EPDFText_GetCharToTextMap(textpage.get(), anchors,
                                         std::size(anchors)));
  EXPECT_EQ(1, anchors[0].char_index);
  EXPECT_EQ(0, anchors[0].text_offset);
}

TEST_F(EPDFTextEmbedderTest, EPDFTextGetCharToTextMapIdentity) {
  // Every character contributes exactly one text unit — zero anchors.
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFTextPage textpage(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(textpage);

  EXPECT_EQ(0, EPDFText_GetCharToTextMap(textpage.get(), nullptr, 0));
}

TEST_F(EPDFTextEmbedderTest, EPDFTextAstralToUnicodeSurrogatePair) {
  // A ToUnicode CMap mapping 'A' to <D83DDE00> stores one character-list
  // entry PER SURROGATE HALF in lockstep with the text, so the page is
  // identity-mapped (4 characters, 4 units) and the emoji survives
  // extraction whole: D83D DE00 'B' '!'.
  ASSERT_TRUE(OpenDocument("embedpdf_astral_tounicode.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFTextPage textpage(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(textpage);

  ASSERT_EQ(4, FPDFText_CountChars(textpage.get()));

  const int required = EPDFText_GetTextFull(textpage.get(), nullptr, 0);
  ASSERT_EQ(5, required);  // 4 units + NUL

  unsigned short buffer[8] = {};
  ASSERT_EQ(required,
            EPDFText_GetTextFull(textpage.get(), buffer, std::size(buffer)));
  static constexpr unsigned short kExpected[] = {0xD83D, 0xDE00, 0x0042,
                                                 0x0021, 0};
  EXPECT_THAT(pdfium::span(buffer).first(5u), ElementsAreArray(kExpected));

  EXPECT_EQ(0, EPDFText_GetCharToTextMap(textpage.get(), nullptr, 0));
}

TEST_F(EPDFTextEmbedderTest, EPDFTextFullAndMapInvalidArgs) {
  EXPECT_EQ(0, EPDFText_GetTextFull(nullptr, nullptr, 0));
  EXPECT_EQ(-1, EPDFText_GetCharToTextMap(nullptr, nullptr, 0));
}
