// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Text set in a font the document doesn't embed, drawn with the built-in
// generic font that stands in for it. That font is a multiple master: each
// glyph is drawn with the design that gives it the width the PDF asks for. A
// text object's bounds are those of the glyphs drawn, and don't depend on what
// was drawn before.

#include <algorithm>
#include <string>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fxge/cfx_font.h"
#include "core/fxge/cfx_substfont.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdf_edit.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

constexpr int kPageWidth = 400;
constexpr int kPageHeight = 200;

// One line of text in /Cambria, which the test fonts don't have, with every
// character `width` thousandths of an em wide; with no /Widths at all for 0,
// so the widths come from the font that stands in.
std::string OneLine(int width, const std::string& text) {
  std::string widths;
  if (width) {
    widths = "/FirstChar 32 /LastChar 126 /Widths [";
    for (int i = 32; i <= 126; ++i) {
      widths += std::to_string(width) + " ";
    }
    widths += "] ";
  }
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] /Contents 4 0 R "
      "/Resources << /Font << /F1 5 0 R >> >> >>",
      Stream("BT /F1 40 Tf 20 100 Td (" + text + ") Tj ET"),
      "<< /Type /Font /Subtype /TrueType /BaseFont /Cambria " + widths +
          "/Encoding /WinAnsiEncoding /FontDescriptor 6 0 R >>",
      "<< /Type /FontDescriptor /FontName /Cambria /Flags 32 "
      "/FontBBox [-500 -300 1500 1000] /ItalicAngle 0 /Ascent 900 "
      "/Descent -250 /CapHeight 700 /StemV 80 >>",
  });
}

struct Line {
  CFX_FloatRect bounds;
  bool built_in_generic = false;
};

Line BoundsOf(const std::string& pdf) {
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  const CPDF_TextObject* text =
      CPDFPageFromFPDFPage(page.get())->GetPageObjectByIndex(0)->AsText();
  Line line;
  line.bounds = text->GetRect();
  const CFX_SubstFont* subst = text->GetFont()->GetFont()->GetSubstFont();
  line.built_in_generic = subst && subst->IsBuiltInGenericFont();
  return line;
}

// Draws `pdf`'s page, which sets the generic font's design for its glyphs.
void Draw(const std::string& pdf) {
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ScopedFPDFBitmap bitmap = EmbedderTest::RenderPage(page.get());
}

// The extent of the dark pixels of `pdf`'s page drawn at `scale` pixels per
// point, in page space.
CFX_FloatRect Ink(const std::string& pdf, int scale) {
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  const int width = kPageWidth * scale;
  const int height = kPageHeight * scale;
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(width, height, 0));
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xffffffff);
  FPDF_RenderPageBitmap(bitmap.get(), page.get(), 0, 0, width, height, 0, 0);
  const auto* pixels =
      static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get()));
  const int stride = FPDFBitmap_GetStride(bitmap.get());
  int left = width;
  int right = 0;
  int top = height;
  int bottom = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (pixels[y * stride + x * 4] < 128) {
        left = std::min(left, x);
        right = std::max(right, x + 1);
        top = std::min(top, y);
        bottom = std::max(bottom, y + 1);
      }
    }
  }
  const float s = static_cast<float>(scale);
  return CFX_FloatRect(left / s, kPageHeight - bottom / s, right / s,
                       kPageHeight - top / s);
}

class EPDFTextBoundsEmbedderTest : public EmbedderTest {};

}  // namespace

// The face is shared by every document that uses it. Drawing a glyph sets its
// design; the bounds measured afterwards are the same as before. Without
// /Widths, the widths too come from the face, and so the text's length.
TEST_F(EPDFTextBoundsEmbedderTest, BoundsDontDependOnWhatWasDrawn) {
  for (int width : {500, 0}) {
    SCOPED_TRACE(width);
    const std::string pdf = OneLine(width, "Hamburg");
    const Line before = BoundsOf(pdf);
    ASSERT_TRUE(before.built_in_generic);

    Draw(OneLine(350, "Wolfsburg"));
    Draw(OneLine(700, "Augsburg"));
    EXPECT_EQ(before.bounds, BoundsOf(pdf).bounds);
  }
}

// Narrow, normal and wide widths: the bounds hold the glyphs drawn, to within
// the pixels of a render at 8 pixels per point.
TEST_F(EPDFTextBoundsEmbedderTest, BoundsAreThoseOfTheGlyphsDrawn) {
  constexpr int kScale = 8;
  constexpr float kPixel = 1.0f / kScale;
  for (int width : {350, 500, 700}) {
    SCOPED_TRACE(width);
    const std::string pdf = OneLine(width, "Hamburg");
    const CFX_FloatRect bounds = BoundsOf(pdf).bounds;
    const CFX_FloatRect ink = Ink(pdf, kScale);
    EXPECT_NEAR(ink.left, bounds.left, kPixel);
    EXPECT_NEAR(ink.right, bounds.right, kPixel);
    EXPECT_NEAR(ink.bottom, bounds.bottom, kPixel);
    EXPECT_NEAR(ink.top, bounds.top, kPixel);
  }
}
