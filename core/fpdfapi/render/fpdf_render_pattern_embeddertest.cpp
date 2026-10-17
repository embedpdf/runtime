// Copyright 2015 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdint>
#include <vector>

#include "public/cpp/fpdf_scopers.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/embedder_test_constants.h"
#include "testing/gtest/include/gtest/gtest.h"

class FPDFRenderPatternEmbedderTest : public EmbedderTest {};

TEST_F(FPDFRenderPatternEmbedderTest, ShadingStrokePaintsAfterSolidFill) {
  ASSERT_TRUE(OpenDocument("gradient_pattern_stroke_order.pdf"));
  ASSERT_EQ(3, FPDF_GetPageCount(document()));

  for (int page_index = 0; page_index < 3; ++page_index) {
    SCOPED_TRACE(page_index);
    ScopedPage page = LoadScopedPage(page_index);
    ASSERT_TRUE(page);
    for (int scale : {1, 2, 4}) {
      SCOPED_TRACE(scale);
      const int size = 100 * scale;
      const int stride = size * 4;
      std::vector<uint8_t> pixels(size * stride);
      ScopedFPDFBitmap bitmap(FPDFBitmap_CreateEx(size, size, FPDFBitmap_BGRA,
                                                  pixels.data(), stride));
      ASSERT_TRUE(bitmap);
      ASSERT_TRUE(FPDFBitmap_FillRect(bitmap.get(), 0, 0, size, size,
                                      /*color=*/0xFFFFFFFF));
      FPDF_RenderPageBitmap(bitmap.get(), page.get(), 0, 0, size, size,
                            /*rotate=*/0, /*flags=*/0);

      // Both halves of the 10-point stroke stay visible for B, B* and S.
      const int row = 50 * scale * stride;
      EXPECT_LT(pixels[row + 17 * scale * 4 + 1], 20);
      EXPECT_LT(pixels[row + 23 * scale * 4 + 1], 20);
      EXPECT_EQ(pixels[row + 50 * scale * 4 + 1], 255);
    }
  }
}

TEST_F(FPDFRenderPatternEmbedderTest, LoadError547706) {
  // Test shading where object is a dictionary instead of a stream.
  ASSERT_TRUE(OpenDocument("bug_547706.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
  CompareBitmap(bitmap.get(), pdfium::kBlankPage612By792Png);
}

TEST_F(FPDFRenderPatternEmbedderTest, ZeroTilingPatternStep509964646) {
  // Rendering a tiling pattern with a zero step should not hang.
  ASSERT_TRUE(OpenDocument("bug_509964646.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
  ASSERT_TRUE(bitmap);
}
