// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_edit.h"

#include <array>
#include <cstring>

#include "public/cpp/fpdf_scopers.h"
#include "testing/embedder_test.h"

class EPDFPageObjectEmbedderTest : public EmbedderTest {};

TEST_F(EPDFPageObjectEmbedderTest, DefaultsAndInvalidArguments) {
  ScopedFPDFPageObject path(FPDFPageObj_CreateNewRect(0, 0, 10, 10));
  ASSERT_TRUE(path);
  float value = -1;
  EXPECT_TRUE(EPDFPageObj_GetFillAlpha(path.get(), &value));
  EXPECT_FLOAT_EQ(1, value);
  EXPECT_TRUE(EPDFPageObj_GetStrokeAlpha(path.get(), &value));
  EXPECT_FLOAT_EQ(1, value);
  EXPECT_TRUE(EPDFPageObj_GetMiterLimit(path.get(), &value));
  EXPECT_FLOAT_EQ(10, value);
  EXPECT_FALSE(EPDFPageObj_HasSoftMask(path.get()));
  char mode[7] = {};
  EXPECT_EQ(7u, EPDFPageObj_GetBlendMode(path.get(), mode, sizeof(mode)));
  EXPECT_STREQ("Normal", mode);

  value = -1;
  EXPECT_FALSE(EPDFPageObj_GetFillAlpha(nullptr, &value));
  EXPECT_FALSE(EPDFPageObj_GetStrokeAlpha(nullptr, &value));
  EXPECT_FALSE(EPDFPageObj_GetMiterLimit(nullptr, &value));
  EXPECT_FLOAT_EQ(-1, value);
  EXPECT_FALSE(EPDFPageObj_GetFillAlpha(path.get(), nullptr));
  EXPECT_FALSE(EPDFPageObj_GetStrokeAlpha(path.get(), nullptr));
  EXPECT_FALSE(EPDFPageObj_GetMiterLimit(path.get(), nullptr));
  EXPECT_FALSE(EPDFPageObj_HasSoftMask(nullptr));
  EXPECT_EQ(0u, EPDFPageObj_GetBlendMode(nullptr, mode, sizeof(mode)));
  EXPECT_STREQ("Normal", mode);
  FPDF_BOOL group = -1;
  FPDF_BOOL isolated = -1;
  EXPECT_FALSE(EPDFFormObj_GetTransparencyGroup(nullptr, &group, &isolated));
  EXPECT_FALSE(EPDFFormObj_GetTransparencyGroup(path.get(), &group, &isolated));
  EXPECT_EQ(-1, group);
  EXPECT_EQ(-1, isolated);
}

TEST_F(EPDFPageObjectEmbedderTest, ReadsGraphicsStateWithoutChangingRendering) {
  ASSERT_TRUE(OpenDocument("epdf_pageobject_state.pdf"));
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(7, FPDFPage_CountObjects(page));
  const auto before = HashBitmap(RenderPage(page).get());

  // Images and shadings carry alpha without a color state.
  const std::array<int, 3> types = {FPDF_PAGEOBJ_PATH, FPDF_PAGEOBJ_IMAGE,
                                    FPDF_PAGEOBJ_SHADING};
  for (int i = 0; i < 3; ++i) {
    FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, i);
    EXPECT_EQ(types[i], FPDFPageObj_GetType(object));
    float alpha = -1;
    EXPECT_TRUE(EPDFPageObj_GetFillAlpha(object, &alpha));
    EXPECT_FLOAT_EQ(0.25f, alpha);
    EXPECT_TRUE(EPDFPageObj_GetStrokeAlpha(object, &alpha));
    EXPECT_FLOAT_EQ(0.75f, alpha);
    char mode[9] = {};
    EXPECT_EQ(9u, EPDFPageObj_GetBlendMode(object, nullptr, 0));
    EXPECT_EQ(9u, EPDFPageObj_GetBlendMode(object, nullptr, sizeof(mode)));
    EXPECT_EQ(9u, EPDFPageObj_GetBlendMode(object, mode, sizeof(mode)));
    EXPECT_STREQ("Multiply", mode);
    char small[4] = {'k', 'e', 'e', 'p'};
    EXPECT_EQ(9u, EPDFPageObj_GetBlendMode(object, small, sizeof(small)));
    EXPECT_EQ(0, std::memcmp(small, "keep", sizeof(small)));
    EXPECT_FALSE(EPDFPageObj_HasSoftMask(object));
    if (i != 0) {
      unsigned int r = 0, g = 0, b = 0, a = 0;
      EXPECT_FALSE(FPDFPageObj_GetFillColor(object, &r, &g, &b, &a));
    }
  }
  float miter = 0;
  EXPECT_TRUE(EPDFPageObj_GetMiterLimit(FPDFPage_GetObject(page, 0), &miter));
  EXPECT_FLOAT_EQ(7, miter);
  EXPECT_EQ(FPDF_LINEJOIN_ROUND,
            FPDFPageObj_GetLineJoin(FPDFPage_GetObject(page, 0)));

  for (int i = 3; i < 6; ++i) {
    FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, i);
    FPDF_BOOL group = -1;
    FPDF_BOOL isolated = -1;
    EXPECT_TRUE(EPDFFormObj_GetTransparencyGroup(object, &group, &isolated));
    EXPECT_EQ(i != 3, group);
    EXPECT_EQ(i == 5, isolated);
    group = isolated = -1;
    EXPECT_FALSE(EPDFFormObj_GetTransparencyGroup(object, nullptr, &isolated));
    EXPECT_FALSE(EPDFFormObj_GetTransparencyGroup(object, &group, nullptr));
    EXPECT_EQ(-1, group);
    EXPECT_EQ(-1, isolated);
  }
  EXPECT_TRUE(EPDFPageObj_HasSoftMask(FPDFPage_GetObject(page, 6)));
  EXPECT_EQ(before, HashBitmap(RenderPage(page).get()));
  UnloadPage(page);
}
