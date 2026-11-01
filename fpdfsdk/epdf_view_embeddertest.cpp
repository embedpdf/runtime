// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// What we add to public/fpdfview.h (public/epdf_view.h).
#include "public/epdf_view.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "constants/page_object.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fxge/agg/cfx_agg_devicedriver.h"
#include "core/fxge/cfx_fontmapper.h"
#include "core/fxge/dib/cstretchengine.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_c_api_test.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_progressive.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_progressive.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"

// Our public headers compile and link as C (fpdfsdk/epdf_c_api_test.c).
TEST(epdf, CApiTest) {
  EXPECT_TRUE(CheckEmbedPDFCApi());
}

class EPDFViewEmbedderTest : public EmbedderTest {
 protected:
  // Sets the decoded image budget for a test and empties the store after it.
  class ScopedDecodedImageBudget {
   public:
    explicit ScopedDecodedImageBudget(unsigned long bytes) {
      EPDF_SetDecodedImageBudget(bytes);
    }
    ~ScopedDecodedImageBudget() { EPDF_SetDecodedImageBudget(0); }
  };

  // Renders `page` `width` px wide.
  static std::string RenderWidth(FPDF_PAGE page, int width) {
    const int height = static_cast<int>(width * FPDF_GetPageHeightF(page) /
                                        FPDF_GetPageWidthF(page));
    ScopedFPDFBitmap bitmap(FPDFBitmap_Create(width, height, 0));
    FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap.get(), page, 0, 0, width, height, 0,
                          FPDF_ANNOT);
    return HashBitmap(bitmap.get());
  }

  // Renders `page` scaled by `scale` into a `width` × `height` bitmap whose
  // top left corner is device pixel `left`, `top`. With `slices`, the render
  // is sliced as finely as EPDF_RenderPageBitmapWithMatrix_Start() allows, and
  // `slices` counts the slices; without, FPDF_RenderPageBitmapWithMatrix()
  // renders it at once.
  static std::string RenderMatrixTile(FPDF_PAGE page,
                                      float scale,
                                      int left,
                                      int top,
                                      int width,
                                      int height,
                                      int flags,
                                      int* slices) {
    ScopedFPDFBitmap bitmap(FPDFBitmap_Create(width, height, 1));
    FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);
    const FS_MATRIX matrix = {scale,
                              0,
                              0,
                              scale,
                              static_cast<float>(-left),
                              static_cast<float>(-top)};
    const FS_RECTF clip = {0, 0, static_cast<float>(width),
                           static_cast<float>(height)};
    if (!slices) {
      FPDF_RenderPageBitmapWithMatrix(bitmap.get(), page, &matrix, &clip,
                                      flags);
      return HashBitmap(bitmap.get());
    }
    int status = EPDF_RenderPageBitmapWithMatrix_Start(bitmap.get(), page,
                                                       &matrix, &clip, flags,
                                                       /*budget_ms=*/0);
    *slices = 1;
    while (status == FPDF_RENDER_TOBECONTINUED) {
      status = EPDF_RenderPage_Continue(page, /*budget_ms=*/0);
      ++*slices;
    }
    EXPECT_EQ(FPDF_RENDER_DONE, status);
    FPDF_RenderPage_Close(page);
    return HashBitmap(bitmap.get());
  }

  // Renders `page` at 800 × 800 px into the 200 px tile at `left`, `top`,
  // with the object groups on or off.
  static std::string RenderObjectGroupsTile(FPDF_PAGE page,
                                            int left,
                                            int top,
                                            bool groups) {
    CPDF_PageObjectHolder::SetObjectGroupsEnabledForTesting(groups);
    ScopedFPDFBitmap bitmap(FPDFBitmap_Create(200, 200, 1));
    FPDFBitmap_FillRect(bitmap.get(), 0, 0, 200, 200, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap.get(), page, -left, -top, 800, 800, 0,
                          FPDF_ANNOT);
    CPDF_PageObjectHolder::SetObjectGroupsEnabledForTesting(true);
    return HashBitmap(bitmap.get());
  }
};

TEST_F(EPDFViewEmbedderTest, EPDFDocGetPageObjectNumberByIndex) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  EXPECT_EQ(0u, EPDFDoc_GetPageObjectNumberByIndex(nullptr, 0));

  // Page -1 doesn't exist.
  EXPECT_EQ(0u, EPDFDoc_GetPageObjectNumberByIndex(document(), -1));

  // Page 1 doesn't exist.
  EXPECT_EQ(0u, EPDFDoc_GetPageObjectNumberByIndex(document(), 1));

  const unsigned int objnum = EPDFDoc_GetPageObjectNumberByIndex(document(), 0);
  EXPECT_NE(0u, objnum);

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());

  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(objnum, EPDFPage_GetObjectNumber(page.get()));
  EXPECT_EQ(1u, doc->GetParsedPageCountForTesting());
}

TEST_F(EPDFViewEmbedderTest, EPDFPageResetRenderCache) {
  EXPECT_FALSE(EPDFPage_ResetRenderCache(nullptr));

  ASSERT_TRUE(OpenDocument("embedded_images.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  const std::string first = HashBitmap(RenderLoadedPage(page.get()).get());
  EXPECT_TRUE(EPDFPage_ResetRenderCache(page.get()));
  EXPECT_EQ(first, HashBitmap(RenderLoadedPage(page.get()).get()));

  // Refused while a progressive render holds the page's render context, which
  // it does from FPDF_RenderPageBitmap_Start() until FPDF_RenderPage_Close().
  const int width = static_cast<int>(FPDF_GetPageWidthF(page.get()));
  const int height = static_cast<int>(FPDF_GetPageHeightF(page.get()));
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(width, height, 0));
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);
  IFSDK_PAUSE pause = {};
  pause.version = 1;
  pause.NeedToPauseNow = [](IFSDK_PAUSE*) -> FPDF_BOOL { return false; };
  EXPECT_EQ(FPDF_RENDER_DONE,
            FPDF_RenderPageBitmap_Start(bitmap.get(), page.get(), 0, 0, width,
                                        height, 0, 0, &pause));
  EXPECT_FALSE(EPDFPage_ResetRenderCache(page.get()));
  FPDF_RenderPage_Close(page.get());
  EXPECT_TRUE(EPDFPage_ResetRenderCache(page.get()));
  EXPECT_EQ(first, HashBitmap(RenderLoadedPage(page.get()).get()));
}

// A render sliced at every chance writes the bytes of a render at once:
// images (JPEG, JPX, JBIG2, masks, soft masks, the huge image), forms,
// transparency, shadings, annotations, repeated paths and grouped objects,
// whole pages and tiles.
TEST_F(EPDFViewEmbedderTest, SlicedRenderMatchesOneShot) {
  const char* const kFiles[] = {
      "embedded_images.pdf",
      "embedpdf_jpx_resolution_levels.pdf",
      "bug_631912.pdf",
      "embedpdf_image_masks.pdf",
      "matte.pdf",
      "nested_form_alpha.pdf",
      "form_object_with_image.pdf",
      "bug_547706.pdf",
      "annotation_stamp_with_ap.pdf",
      "many_rectangles.pdf",
      "embedpdf_repeated_paths.pdf",
      "embedpdf_object_groups.pdf",
  };
  int most_slices = 0;
  for (const char* file : kFiles) {
    ASSERT_TRUE(OpenDocument(file)) << file;
    const int page_count = std::min(FPDF_GetPageCount(document()), 4);
    for (int i = 0; i < page_count; ++i) {
      ScopedPage page = LoadScopedPage(i);
      ASSERT_TRUE(page) << file << " page " << i;
      const int width = static_cast<int>(FPDF_GetPageWidthF(page.get()));
      const int height = static_cast<int>(FPDF_GetPageHeightF(page.get()));
      struct Case {
        float scale;
        int left;
        int top;
        int width;
        int height;
        int flags;
      };
      const Case kCases[] = {
          {1, 0, 0, width, height, FPDF_ANNOT | FPDF_REVERSE_BYTE_ORDER},
          {0.5f, 0, 0, width / 2 + 1, height / 2 + 1, 0},
          // A tile in the middle of the page at 2.5 times.
          {2.5f, width, height, 256, 256, FPDF_ANNOT},
      };
      for (const Case& c : kCases) {
        int slices = 0;
        const std::string sliced =
            RenderMatrixTile(page.get(), c.scale, c.left, c.top, c.width,
                             c.height, c.flags, &slices);
        EXPECT_EQ(RenderMatrixTile(page.get(), c.scale, c.left, c.top, c.width,
                                   c.height, c.flags, nullptr),
                  sliced)
            << file << " page " << i << " scale " << c.scale;
        most_slices = std::max(most_slices, slices);
      }
    }
    CloseDocument();
  }
  // The renders really paused.
  EXPECT_GT(most_slices, 10);

  // With decoded images kept, the first sliced render of a page decodes and
  // keeps its images, and the second draws them from the store: the huge
  // image (page 0), and a masked and an inverted image (page 2).
  ASSERT_TRUE(OpenDocument("embedpdf_decoded_images.pdf"));
  for (int page_index : {0, 2}) {
    std::string expected;
    int width = 0;
    int height = 0;
    {
      ScopedPage page = LoadScopedPage(page_index);
      ASSERT_TRUE(page);
      width = static_cast<int>(FPDF_GetPageWidthF(page.get()) * 0.5f);
      height = static_cast<int>(FPDF_GetPageHeightF(page.get()) * 0.5f);
      expected =
          RenderMatrixTile(page.get(), 0.5f, 0, 0, width, height, 0, nullptr);
    }
    ScopedDecodedImageBudget budget(256 * 1024 * 1024);
    for (int round = 0; round < 2; ++round) {
      ScopedPage page = LoadScopedPage(page_index);
      ASSERT_TRUE(page);
      int slices = 0;
      EXPECT_EQ(expected, RenderMatrixTile(page.get(), 0.5f, 0, 0, width,
                                           height, 0, &slices))
          << "page " << page_index << " round " << round;
      EXPECT_GT(EPDF_GetDecodedImageBytes(), 0u);
    }
  }
}

// Every cell of a tiling pattern sits where the pattern puts it, to the pixel,
// at every zoom. Stepping from the pattern's origin by the cell's width rounded
// to whole pixels drifts over the hundreds of cells between an origin far off
// the page and the page, by an amount that changes with the zoom.
TEST_F(EPDFViewEmbedderTest, TilingPatternCellsStayInPlaceAtEveryZoom) {
  ASSERT_TRUE(OpenDocument("embedpdf_tiling_patterns.pdf"));
  // Renders the whole of `page` scaled by `scale`, and returns where each dark
  // run starts along a line one pixel into the bitmap: across it, or down it.
  // (A bitmap smaller than a cell would have its cells drawn one by one.)
  auto run_starts = [](FPDF_PAGE page, float scale, bool down) {
    const int length = static_cast<int>(200 * scale);
    ScopedFPDFBitmap bitmap(FPDFBitmap_Create(length, length, 0));
    FPDFBitmap_FillRect(bitmap.get(), 0, 0, length, length, 0xFFFFFFFF);
    const FS_MATRIX matrix = {scale, 0, 0, scale, 0, 0};
    const FS_RECTF clip = {0, 0, static_cast<float>(length),
                           static_cast<float>(length)};
    FPDF_RenderPageBitmapWithMatrix(bitmap.get(), page, &matrix, &clip, 0);
    const uint8_t* buffer =
        static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get()));
    const int stride = FPDFBitmap_GetStride(bitmap.get());
    std::vector<int> starts;
    bool dark = false;
    for (int i = 0; i < length; ++i) {
      const uint8_t blue = down ? UNSAFE_TODO(buffer[i * stride + 4])
                                : UNSAFE_TODO(buffer[stride + i * 4]);
      const bool now = blue < 128;
      if (now && !dark) {
        starts.push_back(i);
      }
      dark = now;
    }
    return starts;
  };
  struct Case {
    int page;
    bool down;
    // Where on the device, in points, the bars start: `first + k * period`.
    float first;
    float period;
    // In pixels. A cell is drawn into an image of whole pixels, stretched to
    // fit, so a bar far from the cell's corner may move by part of a pixel.
    float tolerance;
  };
  const Case kCases[] = {
      // Bars at x = 6.7 + 10 k along the cells' left edges: the origin is at
      // x = -5003.3.
      {0, false, 6.7f, 10.0f, 1.0f},
      // Turned a quarter and scaled by 1.3: bars 2.6 high every 13, the
      // topmost from y = 200 - 13.8 on the page, at the cells' far side.
      {2, true, 186.2f - 13.0f * 14, 13.0f, 1.5f},
  };
  for (const Case& c : kCases) {
    ScopedPage page = LoadScopedPage(c.page);
    ASSERT_TRUE(page);
    for (float scale : {0.73f, 1.37f, 2.9f, 4.45f}) {
      const std::vector<int> starts = run_starts(page.get(), scale, c.down);
      ASSERT_GE(starts.size(), 14u) << "page " << c.page << " scale " << scale;
      for (int start : starts) {
        const float k = std::round((start / scale - c.first) / c.period);
        EXPECT_NEAR((c.first + c.period * k) * scale, start, c.tolerance)
            << "page " << c.page << " scale " << scale;
      }
    }
  }
}

TEST_F(EPDFViewEmbedderTest, SlicedRenderCancels) {
  EXPECT_EQ(FPDF_RENDER_FAILED, EPDF_RenderPageBitmapWithMatrix_Start(
                                    nullptr, nullptr, nullptr, nullptr, 0, 0));
  EXPECT_EQ(FPDF_RENDER_FAILED, EPDF_RenderPage_Continue(nullptr, 0));

  ASSERT_TRUE(OpenDocument("many_rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  const int width = static_cast<int>(FPDF_GetPageWidthF(page.get()));
  const int height = static_cast<int>(FPDF_GetPageHeightF(page.get()));
  const std::string whole =
      RenderMatrixTile(page.get(), 1, 0, 0, width, height, FPDF_ANNOT, nullptr);

  // Nothing is left on the page by a start that fails.
  EXPECT_EQ(FPDF_RENDER_FAILED,
            EPDF_RenderPageBitmapWithMatrix_Start(nullptr, page.get(), nullptr,
                                                  nullptr, 0, 0));
  EXPECT_EQ(FPDF_RENDER_FAILED, EPDF_RenderPage_Continue(page.get(), 0));
  EXPECT_TRUE(EPDFPage_ResetRenderCache(page.get()));

  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(width, height, 1));
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);
  const FS_MATRIX matrix = {1, 0, 0, 1, 0, 0};
  const FS_RECTF clip = {0, 0, static_cast<float>(width),
                         static_cast<float>(height)};
  ASSERT_EQ(FPDF_RENDER_TOBECONTINUED,
            EPDF_RenderPageBitmapWithMatrix_Start(
                bitmap.get(), page.get(), &matrix, &clip, FPDF_ANNOT, 0));
  ASSERT_EQ(FPDF_RENDER_TOBECONTINUED, EPDF_RenderPage_Continue(page.get(), 0));

  // While it is paused the page holds it: no second render starts, and the
  // render cache stays.
  ScopedFPDFBitmap other(FPDFBitmap_Create(width, height, 1));
  EXPECT_EQ(FPDF_RENDER_FAILED,
            EPDF_RenderPageBitmapWithMatrix_Start(
                other.get(), page.get(), &matrix, &clip, FPDF_ANNOT, 0));
  EXPECT_FALSE(EPDFPage_ResetRenderCache(page.get()));

  // Closing cancels it.
  FPDF_RenderPage_Close(page.get());
  EXPECT_EQ(FPDF_RENDER_FAILED, EPDF_RenderPage_Continue(page.get(), 0));
  EXPECT_TRUE(EPDFPage_ResetRenderCache(page.get()));
  EXPECT_NE(whole, HashBitmap(bitmap.get()));

  // The page renders as before, at once and sliced.
  EXPECT_EQ(whole, RenderMatrixTile(page.get(), 1, 0, 0, width, height,
                                    FPDF_ANNOT, nullptr));
  int slices = 0;
  EXPECT_EQ(whole, RenderMatrixTile(page.get(), 1, 0, 0, width, height,
                                    FPDF_ANNOT, &slices));
  EXPECT_GT(slices, 2);

  // A generous budget renders it in one slice, and continuing a render that
  // is done changes nothing.
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);
  EXPECT_EQ(FPDF_RENDER_DONE,
            EPDF_RenderPageBitmapWithMatrix_Start(
                bitmap.get(), page.get(), &matrix, &clip, FPDF_ANNOT, 60000));
  EXPECT_EQ(FPDF_RENDER_DONE, EPDF_RenderPage_Continue(page.get(), 0));
  FPDF_RenderPage_Close(page.get());
  EXPECT_EQ(whole, HashBitmap(bitmap.get()));
}

// A path drawn again with identical inputs reuses the previous rasterization;
// every page here repeats paths under a condition that changes how the repeat
// composites (alpha, clip, knockout, blend, dash, caps, fill and stroke).
TEST_F(EPDFViewEmbedderTest, RepeatedPathsRenderAsWithoutPathMemo) {
  ASSERT_TRUE(OpenDocument("embedpdf_repeated_paths.pdf"));
  const int page_count = FPDF_GetPageCount(document());
  ASSERT_EQ(12, page_count);

  auto render = [](FPDF_PAGE page, float scale, int alpha, int flags) {
    const int width =
        static_cast<int>(FPDF_GetPageWidthF(page) * scale + 0.5f);
    const int height =
        static_cast<int>(FPDF_GetPageHeightF(page) * scale + 0.5f);
    ScopedFPDFBitmap bitmap(FPDFBitmap_Create(width, height, alpha));
    FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap.get(), page, 0, 0, width, height, 0, flags);
    return HashBitmap(bitmap.get());
  };

  for (int i = 0; i < page_count; ++i) {
    ScopedPage page = LoadScopedPage(i);
    ASSERT_TRUE(page);
    for (float scale : {0.37f, 1.0f, 3.37f}) {
      for (int alpha : {0, 1}) {
        const int flags = alpha ? FPDF_ANNOT | FPDF_REVERSE_BYTE_ORDER : 0;
        pdfium::CFX_AggDeviceDriver::SetPathMemoEnabledForTesting(false);
        const std::string without = render(page.get(), scale, alpha, flags);
        pdfium::CFX_AggDeviceDriver::SetPathMemoEnabledForTesting(true);
        const std::string with = render(page.get(), scale, alpha, flags);
        EXPECT_EQ(without, with)
            << "page " << i << " scale " << scale << " alpha " << alpha;
      }
    }
  }
}

TEST_F(EPDFViewEmbedderTest, ObjectGroupsRenderAsWithoutThem) {
  ASSERT_TRUE(OpenDocument("embedpdf_object_groups.pdf"));
  const int page_count = FPDF_GetPageCount(document());
  ASSERT_EQ(4, page_count);

  // Tiles over the grid, over the empty upper-right quarter, across both, and
  // at the corners.
  constexpr int kTiles[][2] = {{0, 600},   {100, 500}, {500, 100},
                               {300, 300}, {600, 0},   {0, 0}};
  for (int i = 0; i < page_count; ++i) {
    ScopedPage page = LoadScopedPage(i);
    ASSERT_TRUE(page);
    for (const auto& tile : kTiles) {
      EXPECT_EQ(
          RenderObjectGroupsTile(page.get(), tile[0], tile[1], false),
          RenderObjectGroupsTile(page.get(), tile[0], tile[1], true))
          << "page " << i << " tile " << tile[0] << "," << tile[1];
    }
  }
}

TEST_F(EPDFViewEmbedderTest, ObjectGroupsFollowEdits) {
  ASSERT_TRUE(OpenDocument("embedpdf_object_groups.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  const CPDF_Page* cpdf_page = CPDFPageFromFPDFPage(page.get());
  // A clip holding the whole page gets groups only while they are current.
  const CFX_FloatRect whole = cpdf_page->GetBBox();

  // The tile at device 500..700, 100..300 lies in the empty upper-right
  // quarter, which every run of the grid misses.
  const std::string empty = RenderObjectGroupsTile(page.get(), 500, 100, true);
  EXPECT_TRUE(cpdf_page->GetObjectGroups(whole));

  // Move the first square of the grid from 4,4 into the tile.
  FPDF_PAGEOBJECT square = FPDFPage_GetObject(page.get(), 0);
  ASSERT_TRUE(square);
  FPDFPageObj_Transform(square, 1, 0, 0, 1, 146, 146);
  EXPECT_FALSE(cpdf_page->GetObjectGroups(whole));
  const std::string moved = RenderObjectGroupsTile(page.get(), 500, 100, true);
  EXPECT_NE(empty, moved);
  EXPECT_EQ(RenderObjectGroupsTile(page.get(), 500, 100, false), moved);

  // Add a square inside the tile, then take it out again.
  FPDF_PAGEOBJECT added = FPDFPageObj_CreateNewRect(160, 130, 5, 5);
  ASSERT_TRUE(added);
  ASSERT_TRUE(FPDFPageObj_SetFillColor(added, 0, 0, 255, 255));
  ASSERT_TRUE(FPDFPath_SetDrawMode(added, FPDF_FILLMODE_WINDING, 0));
  FPDFPage_InsertObject(page.get(), added);
  EXPECT_FALSE(cpdf_page->GetObjectGroups(whole));
  const std::string inserted =
      RenderObjectGroupsTile(page.get(), 500, 100, true);
  EXPECT_NE(moved, inserted);
  EXPECT_EQ(RenderObjectGroupsTile(page.get(), 500, 100, false), inserted);
  ASSERT_TRUE(FPDFPage_RemoveObject(page.get(), added));
  FPDFPageObj_Destroy(added);
  EXPECT_EQ(moved, RenderObjectGroupsTile(page.get(), 500, 100, true));

  // An object whose bounds are not finite.
  FPDF_PAGEOBJECT odd = FPDFPageObj_CreateNewRect(10, 10, 5, 5);
  ASSERT_TRUE(odd);
  ASSERT_TRUE(FPDFPath_SetDrawMode(odd, FPDF_FILLMODE_WINDING, 0));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  FPDFPageObj_Transform(odd, nan, 0, 0, nan, 0, 0);
  FPDFPage_InsertObject(page.get(), odd);
  EXPECT_EQ(RenderObjectGroupsTile(page.get(), 500, 100, false),
            RenderObjectGroupsTile(page.get(), 500, 100, true));
  EXPECT_EQ(RenderObjectGroupsTile(page.get(), 0, 600, false),
            RenderObjectGroupsTile(page.get(), 0, 600, true));

  // Inside a form: its frame spans the page, so the form object reaches the
  // tile and the form's own groups decide what is drawn.
  ScopedPage form_page = LoadScopedPage(1);
  ASSERT_TRUE(form_page);
  FPDF_PAGEOBJECT form = FPDFPage_GetObject(form_page.get(), 0);
  ASSERT_EQ(FPDF_PAGEOBJ_FORM, FPDFPageObj_GetType(form));
  const CPDF_Form* cpdf_form =
      CPDFPageObjectFromFPDFPageObject(form)->AsForm()->form();
  // The form's /BBox.
  const CFX_FloatRect whole_form(0, 0, 200, 200);
  const std::string before =
      RenderObjectGroupsTile(form_page.get(), 500, 100, true);
  EXPECT_TRUE(cpdf_form->GetObjectGroups(whole_form));

  // Object 0 is the frame; move the first square of the grid into the tile.
  FPDF_PAGEOBJECT inner = FPDFFormObj_GetObject(form, 1);
  ASSERT_TRUE(inner);
  FPDFPageObj_Transform(inner, 1, 0, 0, 1, 146, 146);
  EXPECT_FALSE(cpdf_form->GetObjectGroups(whole_form));
  const std::string after =
      RenderObjectGroupsTile(form_page.get(), 500, 100, true);
  EXPECT_NE(before, after);
  EXPECT_EQ(RenderObjectGroupsTile(form_page.get(), 500, 100, false), after);
}

TEST_F(EPDFViewEmbedderTest, DecodedImageStoreRendersAsNewDecodes) {
  // Every render loads the page again, as the engine does per job, so each
  // render after the first draws from the store.
  struct Case {
    const char* file;
    int page;
    std::vector<int> widths;
  };
  // The huge image, which a store keeps decoded in full; a masked and an
  // inverted image; a JPX image and a JPEG one, whose widths make them decode
  // at several resolution levels and come back to them; and images of
  // PDFium's own tests. The render baselines cover the huge image at more
  // sizes, and drawn three times on one page.
  const Case kCases[] = {
      {"embedpdf_decoded_images.pdf", 0, {300, 150}},
      {"embedpdf_decoded_images.pdf", 2, {1400, 150, 600}},
      {"embedpdf_jpx_resolution_levels.pdf", 0, {1400, 150, 600, 1400, 150}},
      {"jpeg_reduced_size.pdf", 0, {1400, 150, 600, 1400, 150}},
      {"jpeg_reduced_size.pdf", 0, {150, 1400, 600, 150, 1400}},
      {"embedded_images.pdf", 0, {1400, 150, 600}},
      {"form_object_with_image.pdf", 0, {1400, 150, 600}},
      {"jpx_lzw.pdf", 0, {1400, 150, 600}},
  };
  for (const Case& test_case : kCases) {
    ASSERT_TRUE(OpenDocument(test_case.file));
    std::vector<std::string> expected;
    for (int width : test_case.widths) {
      ScopedPage page = LoadScopedPage(test_case.page);
      ASSERT_TRUE(page);
      expected.push_back(RenderWidth(page.get(), width));
    }
    {
      ScopedDecodedImageBudget budget(256 * 1024 * 1024);
      for (size_t i = 0; i < test_case.widths.size(); ++i) {
        ScopedPage page = LoadScopedPage(test_case.page);
        ASSERT_TRUE(page);
        EXPECT_EQ(expected[i], RenderWidth(page.get(), test_case.widths[i]))
            << test_case.file << " page " << test_case.page << " width "
            << test_case.widths[i];
      }
      EXPECT_GT(EPDF_GetDecodedImageBytes(), 0u) << test_case.file;
    }
    EXPECT_EQ(0u, EPDF_GetDecodedImageBytes());
    CloseDocument();
  }
}

TEST_F(EPDFViewEmbedderTest, DecodedImageStoreKeepsItsBudget) {
  ASSERT_TRUE(OpenDocument("embedpdf_decoded_images.pdf"));
  std::string huge_expected;
  {
    ScopedPage page = LoadScopedPage(0);
    huge_expected = RenderWidth(page.get(), 400);
  }

  // The huge image decodes to 67.5 MB, which a 16 MB budget cannot keep.
  ScopedDecodedImageBudget budget(16 * 1024 * 1024);
  {
    ScopedPage page = LoadScopedPage(0);
    EXPECT_EQ(huge_expected, RenderWidth(page.get(), 400));
  }
  EXPECT_EQ(0u, EPDF_GetDecodedImageBytes());

  // The small images fit.
  {
    ScopedPage page = LoadScopedPage(2);
    RenderWidth(page.get(), 800);
  }
  EXPECT_GT(EPDF_GetDecodedImageBytes(), 0u);
  EXPECT_LE(EPDF_GetDecodedImageBytes(), 16u * 1024 * 1024);

  // Closing the document drops its decodes.
  CloseDocument();
  EXPECT_EQ(0u, EPDF_GetDecodedImageBytes());
}

TEST_F(EPDFViewEmbedderTest, SpecializedResampleLoopsMatchGeneralOnes) {
  struct Case {
    const char* file;
    int page;
  };
  // 1-bit masks that are mostly clear, mostly set and mixed, with rows that
  // end mid-byte; RGB images with and without masks; and JPEG photos.
  constexpr Case kCases[] = {
      {"embedpdf_image_masks.pdf", 0},    {"embedpdf_image_masks.pdf", 1},
      {"embedpdf_image_masks.pdf", 2},    {"embedpdf_decoded_images.pdf", 2},
      {"embedded_images.pdf", 0},
  };
  for (const Case& test_case : kCases) {
    ASSERT_TRUE(OpenDocument(test_case.file));
    {
      ScopedPage page = LoadScopedPage(test_case.page);
      ASSERT_TRUE(page);
      // The 1600 px masks shrink strongly, moderately and slightly; the
      // 120 px RGB images grow. The render baselines' zoomed tiles grow the
      // masks too.
      for (int width : {97, 640, 1400}) {
        CStretchEngine::SetSpecializedLoopsEnabledForTesting(false);
        const std::string expected = RenderWidth(page.get(), width);
        CStretchEngine::SetSpecializedLoopsEnabledForTesting(true);
        EXPECT_EQ(expected, RenderWidth(page.get(), width))
            << test_case.file << " page " << test_case.page << " width "
            << width;
      }
    }
    CloseDocument();
  }
}

TEST_F(EPDFViewEmbedderTest, EPDFDocSetPageRotationByObjectNumber) {
  constexpr char kRotatedPng[] = "rectangles_rotated";

  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  const unsigned int objnum = EPDFDoc_GetPageObjectNumberByIndex(document(), 0);
  ASSERT_NE(0u, objnum);

  EXPECT_FALSE(EPDFDoc_SetPageRotationByObjectNumber(nullptr, objnum, 1));
  EXPECT_FALSE(EPDFDoc_SetPageRotationByObjectNumber(document(), 0, 1));
  EXPECT_FALSE(EPDFDoc_SetPageRotationByObjectNumber(document(), objnum, -1));
  EXPECT_FALSE(EPDFDoc_SetPageRotationByObjectNumber(document(), objnum, 4));

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(doc);
  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());

  EXPECT_TRUE(EPDFDoc_SetPageRotationByObjectNumber(document(), objnum, 1));
  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());

  {
    ScopedPage page = LoadScopedPage(0);
    ASSERT_TRUE(page);
    EXPECT_EQ(1, FPDFPage_GetRotation(page.get()));
    EXPECT_EQ(300, static_cast<int>(FPDF_GetPageWidth(page.get())));
    EXPECT_EQ(200, static_cast<int>(FPDF_GetPageHeight(page.get())));
    ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
    CompareBitmapWithExpectationSuffix(bitmap.get(), kRotatedPng);
  }

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ScopedSavedDoc saved_document = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_document);
  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);
  EXPECT_EQ(1, FPDFPage_GetRotation(saved_page.get()));
}

TEST_F(EPDFViewEmbedderTest, EPDFDocDeletePageByObjectNumber) {
  ASSERT_TRUE(OpenDocument("rectangles_multi_pages.pdf"));
  ASSERT_EQ(5, FPDF_GetPageCount(document()));

  const unsigned int original_page_1 =
      EPDFDoc_GetPageObjectNumberByIndex(document(), 1);
  ASSERT_NE(0u, original_page_1);

  int page_to_move = 1;
  ASSERT_TRUE(FPDF_MovePages(document(), &page_to_move, 1, 4));
  ASSERT_EQ(5, FPDF_GetPageCount(document()));
  EXPECT_EQ(original_page_1, EPDFDoc_GetPageObjectNumberByIndex(document(), 4));

  EXPECT_FALSE(EPDFDoc_DeletePageByObjectNumber(nullptr, original_page_1));
  EXPECT_FALSE(EPDFDoc_DeletePageByObjectNumber(document(), 0));
  EXPECT_TRUE(EPDFDoc_DeletePageByObjectNumber(document(), original_page_1));
  EXPECT_EQ(4, FPDF_GetPageCount(document()));

  for (int i = 0; i < FPDF_GetPageCount(document()); ++i) {
    EXPECT_NE(original_page_1,
              EPDFDoc_GetPageObjectNumberByIndex(document(), i));
  }

  EXPECT_FALSE(EPDFDoc_DeletePageByObjectNumber(document(), original_page_1));
}

TEST_F(EPDFViewEmbedderTest,
       EPDFDocDeletePageByObjectNumberDeletesDuplicatePageObjectOccurrence) {
  // This malformed compatibility fixture references the same /Page object from
  // multiple visible page positions. Delete-by-object-number removes the first
  // visible occurrence resolved by PDFium.
  ASSERT_TRUE(OpenDocument("bug_1229106.pdf"));
  ASSERT_EQ(4, FPDF_GetPageCount(document()));

  const unsigned int duplicate_objnum =
      EPDFDoc_GetPageObjectNumberByIndex(document(), 0);
  ASSERT_NE(0u, duplicate_objnum);
  ASSERT_EQ(duplicate_objnum,
            EPDFDoc_GetPageObjectNumberByIndex(document(), 1));

  EXPECT_TRUE(EPDFDoc_DeletePageByObjectNumber(document(), duplicate_objnum));
  EXPECT_EQ(3, FPDF_GetPageCount(document()));
  EXPECT_EQ(duplicate_objnum,
            EPDFDoc_GetPageObjectNumberByIndex(document(), 0));

  EXPECT_TRUE(EPDFDoc_DeletePageByObjectNumber(document(), duplicate_objnum));
  EXPECT_EQ(2, FPDF_GetPageCount(document()));

  for (int i = 0; i < FPDF_GetPageCount(document()); ++i) {
    EXPECT_NE(duplicate_objnum,
              EPDFDoc_GetPageObjectNumberByIndex(document(), i));
  }
}

TEST_F(EPDFViewEmbedderTest, EPDFGetPageBoxByIndex) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  auto expect_rect = [](const FS_RECTF& rect, float left, float top,
                        float right, float bottom) {
    EXPECT_FLOAT_EQ(left, rect.left);
    EXPECT_FLOAT_EQ(top, rect.top);
    EXPECT_FLOAT_EQ(right, rect.right);
    EXPECT_FLOAT_EQ(bottom, rect.bottom);
  };

  FS_RECTF box = {-1.0f, -1.0f, -1.0f, -1.0f};
  EXPECT_FALSE(EPDF_GetPageBoxByIndex(nullptr, 0, EPDF_PAGE_BOX_MEDIA, &box));
  EXPECT_FALSE(
      EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_MEDIA, nullptr));
  EXPECT_FALSE(
      EPDF_GetPageBoxByIndex(document(), -1, EPDF_PAGE_BOX_MEDIA, &box));
  EXPECT_FALSE(
      EPDF_GetPageBoxByIndex(document(), 1, EPDF_PAGE_BOX_MEDIA, &box));
  EXPECT_FALSE(EPDF_GetPageBoxByIndex(
      document(), 0, static_cast<EPDF_PAGE_BOX_TYPE>(999), &box));

  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_MEDIA, &box));
  expect_rect(box, 0.0f, 300.0f, 200.0f, 0.0f);

  // No crop box and no bleed box: absent, with no default in their place.
  box = {-1.0f, -1.0f, -1.0f, -1.0f};
  EXPECT_FALSE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_CROP, &box));
  EXPECT_FALSE(
      EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_BLEED, &box));
  expect_rect(box, -1.0f, -1.0f, -1.0f, -1.0f);

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  RetainPtr<CPDF_Dictionary> page_dict = doc->GetMutablePageDictionary(0);
  ASSERT_TRUE(page_dict);
  page_dict->SetRectFor(pdfium::page_object::kCropBox,
                        CFX_FloatRect(10.0f, 20.0f, 110.0f, 120.0f));
  page_dict->SetRectFor(pdfium::page_object::kBleedBox,
                        CFX_FloatRect(20.0f, 30.0f, 100.0f, 110.0f));
  page_dict->SetRectFor(pdfium::page_object::kTrimBox,
                        CFX_FloatRect(30.0f, 40.0f, 90.0f, 100.0f));
  page_dict->SetRectFor(pdfium::page_object::kArtBox,
                        CFX_FloatRect(40.0f, 50.0f, 80.0f, 90.0f));

  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_CROP, &box));
  expect_rect(box, 10.0f, 120.0f, 110.0f, 20.0f);
  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_BLEED, &box));
  expect_rect(box, 20.0f, 110.0f, 100.0f, 30.0f);
  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_TRIM, &box));
  expect_rect(box, 30.0f, 100.0f, 90.0f, 40.0f);
  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_ART, &box));
  expect_rect(box, 40.0f, 90.0f, 80.0f, 50.0f);

  // Corners given the other way round come back in order.
  page_dict->SetRectFor(pdfium::page_object::kCropBox,
                        CFX_FloatRect(110.0f, 120.0f, 10.0f, 20.0f));
  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_CROP, &box));
  expect_rect(box, 10.0f, 120.0f, 110.0f, 20.0f);

  // An empty box, or one that isn't four numbers, reads as absent.
  page_dict->SetRectFor(pdfium::page_object::kCropBox, CFX_FloatRect());
  EXPECT_FALSE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_CROP, &box));
  auto three = page_dict->SetNewFor<CPDF_Array>(pdfium::page_object::kCropBox);
  three->AppendNew<CPDF_Number>(10);
  three->AppendNew<CPDF_Number>(20);
  three->AppendNew<CPDF_Number>(110);
  EXPECT_FALSE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_CROP, &box));

  // Media and crop boxes are inherited from the page tree; bleed, trim and
  // art boxes are not (ISO 32000-1 Table 30).
  RetainPtr<CPDF_Dictionary> tree =
      page_dict->GetMutableDictFor(pdfium::page_object::kParent);
  ASSERT_TRUE(tree);
  tree->SetRectFor(pdfium::page_object::kMediaBox,
                   CFX_FloatRect(0.0f, 0.0f, 500.0f, 700.0f));
  tree->SetRectFor(pdfium::page_object::kCropBox,
                   CFX_FloatRect(20.0f, 30.0f, 480.0f, 680.0f));
  tree->SetRectFor(pdfium::page_object::kBleedBox,
                   CFX_FloatRect(0.0f, 0.0f, 500.0f, 700.0f));
  page_dict->RemoveFor(pdfium::page_object::kMediaBox);
  page_dict->RemoveFor(pdfium::page_object::kCropBox);
  page_dict->RemoveFor(pdfium::page_object::kBleedBox);
  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_MEDIA, &box));
  expect_rect(box, 0.0f, 700.0f, 500.0f, 0.0f);
  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_CROP, &box));
  expect_rect(box, 20.0f, 680.0f, 480.0f, 30.0f);
  EXPECT_FALSE(
      EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_BLEED, &box));

  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());
}

TEST_F(EPDFViewEmbedderTest, EPDFGetPageRotateByIndex) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  int rotate = -1;
  EXPECT_FALSE(EPDF_GetPageRotateByIndex(nullptr, 0, &rotate));
  EXPECT_FALSE(EPDF_GetPageRotateByIndex(document(), 0, nullptr));
  EXPECT_FALSE(EPDF_GetPageRotateByIndex(document(), 1, &rotate));

  // No /Rotate anywhere: 0.
  ASSERT_TRUE(EPDF_GetPageRotateByIndex(document(), 0, &rotate));
  EXPECT_EQ(0, rotate);

  // As written, even when it isn't a multiple of 90 or is negative.
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  RetainPtr<CPDF_Dictionary> page_dict = doc->GetMutablePageDictionary(0);
  ASSERT_TRUE(page_dict);
  for (int written : {135, -90, 450}) {
    page_dict->SetNewFor<CPDF_Number>(pdfium::page_object::kRotate, written);
    ASSERT_TRUE(EPDF_GetPageRotateByIndex(document(), 0, &rotate));
    EXPECT_EQ(written, rotate);
  }

  // Inherited from the page tree when the page doesn't set it.
  page_dict->RemoveFor(pdfium::page_object::kRotate);
  RetainPtr<CPDF_Dictionary> tree =
      page_dict->GetMutableDictFor(pdfium::page_object::kParent);
  ASSERT_TRUE(tree);
  tree->SetNewFor<CPDF_Number>(pdfium::page_object::kRotate, 270);
  ASSERT_TRUE(EPDF_GetPageRotateByIndex(document(), 0, &rotate));
  EXPECT_EQ(270, rotate);

  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());
}

TEST_F(EPDFViewEmbedderTest, EPDFGetPageTabOrderByIndex) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  EXPECT_EQ(0u, EPDF_GetPageTabOrderByIndex(nullptr, 0, nullptr, 0));
  EXPECT_EQ(0u, EPDF_GetPageTabOrderByIndex(document(), 1, nullptr, 0));
  // No /Tabs: 0.
  EXPECT_EQ(0u, EPDF_GetPageTabOrderByIndex(document(), 0, nullptr, 0));

  // The name as written, with its trailing NUL.
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  RetainPtr<CPDF_Dictionary> page_dict = doc->GetMutablePageDictionary(0);
  ASSERT_TRUE(page_dict);
  page_dict->SetNewFor<CPDF_Name>("Tabs", "S");
  ASSERT_EQ(2u, EPDF_GetPageTabOrderByIndex(document(), 0, nullptr, 0));
  char buffer[2] = {'x', 'x'};
  ASSERT_EQ(2u,
            EPDF_GetPageTabOrderByIndex(document(), 0, buffer, sizeof(buffer)));
  EXPECT_STREQ("S", buffer);

  // /Tabs isn't inherited: the page tree's is not the page's.
  page_dict->RemoveFor("Tabs");
  RetainPtr<CPDF_Dictionary> tree =
      page_dict->GetMutableDictFor(pdfium::page_object::kParent);
  ASSERT_TRUE(tree);
  tree->SetNewFor<CPDF_Name>("Tabs", "R");
  EXPECT_EQ(0u, EPDF_GetPageTabOrderByIndex(document(), 0, nullptr, 0));

  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());
}

TEST_F(EPDFViewEmbedderTest, EPDFGetPageUserUnitByIndex) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  float user_unit = 0.0f;
  EXPECT_FALSE(EPDF_GetPageUserUnitByIndex(nullptr, 0, &user_unit));
  EXPECT_FALSE(EPDF_GetPageUserUnitByIndex(document(), 0, nullptr));
  EXPECT_FALSE(EPDF_GetPageUserUnitByIndex(document(), -1, &user_unit));
  EXPECT_FALSE(EPDF_GetPageUserUnitByIndex(document(), 1, &user_unit));

  ASSERT_TRUE(EPDF_GetPageUserUnitByIndex(document(), 0, &user_unit));
  EXPECT_FLOAT_EQ(1.0f, user_unit);

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  RetainPtr<CPDF_Dictionary> page_dict = doc->GetMutablePageDictionary(0);
  ASSERT_TRUE(page_dict);
  page_dict->SetNewFor<CPDF_Number>("UserUnit", 2.5f);

  ASSERT_TRUE(EPDF_GetPageUserUnitByIndex(document(), 0, &user_unit));
  EXPECT_FLOAT_EQ(2.5f, user_unit);

  page_dict->SetNewFor<CPDF_Number>("UserUnit", -10.0f);
  ASSERT_TRUE(EPDF_GetPageUserUnitByIndex(document(), 0, &user_unit));
  EXPECT_FLOAT_EQ(1.0f, user_unit);

  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());
}

namespace {

// The family names of the fonts hello_world.pdf's text objects draw with: a
// Times-Roman and a Helvetica the file doesn't embed.
std::vector<std::string> HelloWorldFontFamilies(FPDF_PAGE page) {
  std::vector<std::string> families;
  for (int i = 0; i < FPDFPage_CountObjects(page); ++i) {
    FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, i);
    if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT) {
      continue;
    }
    FPDF_FONT font = FPDFTextObj_GetFont(object);
    const size_t size = FPDFFont_GetFamilyName(font, nullptr, 0);
    std::string family(size, '\0');
    FPDFFont_GetFamilyName(font, family.data(), size);
    family.resize(size ? size - 1 : 0);
    families.push_back(family);
  }
  return families;
}

// Standard fonts draw as the product draws them, with PDFium's built-in faces,
// while it lives; then as the embedder test environment set them up for
// upstream's tests, from the test fonts.
class ScopedBuiltInStandardFonts {
 public:
  ScopedBuiltInStandardFonts() {
    CFX_FontMapper::SetStandardFontsFromSystemForTesting(false);
  }
  ~ScopedBuiltInStandardFonts() {
    CFX_FontMapper::SetStandardFontsFromSystemForTesting(true);
  }
};

}  // namespace

// Standard fonts draw with PDFium's built-in faces on every platform (rule
// fonts), even where the system has fonts for them: here, the test fonts the
// embedder test environment installs, which upstream's tests get instead.
TEST_F(EPDFViewEmbedderTest, StandardFontsDrawWithBuiltInFaces) {
  {
    ScopedBuiltInStandardFonts built_in;
    ASSERT_TRUE(OpenDocument("hello_world.pdf"));
    {
      ScopedPage page = LoadScopedPage(0);
      ASSERT_TRUE(page);
      EXPECT_EQ((std::vector<std::string>{"Chrom Serif OTF", "Chrom Sans OTF"}),
                HelloWorldFontFamilies(page.get()));
    }
    CloseDocument();
  }

  // Upstream's tests: the test fonts.
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ((std::vector<std::string>{"Tinos", "Arimo"}),
            HelloWorldFontFamilies(page.get()));
}

// An image object of another document draws as one of the page's own (rule
// bug-fix). The page image cache refuses it, and the image loader then read
// the cache's current entry: with none yet, a crash.
TEST_F(EPDFViewEmbedderTest, ImageOfAnotherDocumentDraws) {
  ScopedFPDFBitmap red(FPDFBitmap_Create(2, 2, 0));
  ASSERT_TRUE(red);
  ASSERT_TRUE(FPDFBitmap_FillRect(red.get(), 0, 0, 2, 2, 0xFFFF0000));
  // Draws an image of |image_document| on a new page of |document|.
  auto render = [&red](FPDF_DOCUMENT document, FPDF_DOCUMENT image_document) {
    ScopedFPDFPage page(FPDFPage_New(document, 0, 200, 200));
    FPDF_PAGEOBJECT image = FPDFPageObj_NewImageObj(image_document);
    EXPECT_TRUE(FPDFImageObj_SetBitmap(nullptr, 0, image, red.get()));
    EXPECT_TRUE(FPDFImageObj_SetMatrix(image, 100, 0, 0, 100, 50, 50));
    FPDFPage_InsertObject(page.get(), image);
    ScopedFPDFBitmap bitmap = RenderPage(page.get());
    return HashBitmap(bitmap.get());
  };

  ScopedFPDFDocument document(FPDF_CreateNewDocument());
  ScopedFPDFDocument other(FPDF_CreateNewDocument());
  ASSERT_TRUE(document);
  ASSERT_TRUE(other);
  EXPECT_EQ(render(document.get(), document.get()),
            render(document.get(), other.get()));
}
