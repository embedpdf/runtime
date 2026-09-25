// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cstdint>

#include "public/cpp/fpdf_scopers.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/gtest/include/gtest/gtest.h"

class FPDFRenderTransparencyEmbedderTest : public EmbedderTest {};

TEST_F(FPDFRenderTransparencyEmbedderTest, NestedFormsApplyFillAlphaOnce) {
  ASSERT_TRUE(OpenDocument("nested_form_alpha.pdf"));
  ASSERT_EQ(4, FPDF_GetPageCount(document()));

  constexpr int kPageSize = 40;
  constexpr int kExpectedAlpha[] = {128, 128, 128, 255};
  for (int page_index = 0; page_index < 4; ++page_index) {
    SCOPED_TRACE(page_index);
    ScopedPage page = LoadScopedPage(page_index);
    ASSERT_TRUE(page);
    ScopedFPDFBitmap bitmap(
        FPDFBitmap_Create(kPageSize, kPageSize, /*alpha=*/1));
    ASSERT_TRUE(bitmap);
    ASSERT_EQ(FPDFBitmap_BGRA, FPDFBitmap_GetFormat(bitmap.get()));
    ASSERT_TRUE(FPDFBitmap_FillRect(bitmap.get(), 0, 0, kPageSize, kPageSize,
                                    /*color=*/0));
    FPDF_RenderPageBitmap(bitmap.get(), page.get(), 0, 0, kPageSize, kPageSize,
                          /*rotate=*/0, /*flags=*/0);

    const auto* pixels =
        static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get()));
    ASSERT_TRUE(pixels);
    const int stride = FPDFBitmap_GetStride(bitmap.get());
    EXPECT_EQ(0, pixels[3]);
    int max_alpha = 0;
    for (int y = 0; y < kPageSize; ++y) {
      for (int x = 0; x < kPageSize; ++x) {
        max_alpha = std::max(max_alpha,
                             static_cast<int>(pixels[y * stride + x * 4 + 3]));
      }
    }
    EXPECT_NEAR(kExpectedAlpha[page_index], max_alpha, 3);
  }
}
