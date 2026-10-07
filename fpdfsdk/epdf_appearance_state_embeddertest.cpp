// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Appearance states and modes. A caller can list the states a mode stores
// (EPDFAnnot_GetAppearanceState*) and draw any one of them whatever /AS says
// (EPDF_RenderAnnotBitmap's `state`), so a viewer has both looks of a check
// box before anyone clicks it. A page render draws annotations (FPDF_ANNOT)
// and widgets (EPDF_RENDER_WIDGETS) independently. And the check box and
// radio button generators draw the symbol /MK /CA names, in the /DA colour.

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fxcrt/bytestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdf_annot.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/utils/path_service.h"

namespace {

// Indexes in the fixture page's /Annots
// (testing/resources/appearance_states.in).
constexpr int kStates = 0;  // off; N Yes red, N Off green, D Yes blue,
                            // D Off yellow, R Yes black
constexpr int kSquare = 1;  // one normal stream, cyan
constexpr int kHidden = 2;  // a hidden check box, magenta
constexpr int kCross = 3;   // check box, /CA 8, /DA blue, no appearance yet
constexpr int kSquareRadio = 4;  // radio, /CA n, /DA red, black border
constexpr int kPlain = 5;        // check box, no /CA, no /DA
constexpr int kDot = 6;          // radio, no /CA, /DA blue
constexpr int kSi = 7;           // check box, on, its on state a UTF-8 name

constexpr int kPageSize = 300;

constexpr uint32_t kWhite = 0xFFFFFFFF;
constexpr uint32_t kRed = 0xFFFF0000;
constexpr uint32_t kGreen = 0xFF00FF00;
constexpr uint32_t kBlue = 0xFF0000FF;
constexpr uint32_t kYellow = 0xFFFFFF00;
constexpr uint32_t kCyan = 0xFF00FFFF;
constexpr uint32_t kBlack = 0xFF000000;

uint32_t PixelAt(FPDF_BITMAP bitmap, int x, int y) {
  const uint8_t* buffer =
      static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
  const int stride = FPDFBitmap_GetStride(bitmap);
  const uint8_t* pixel = buffer + y * stride + x * 4;
  return (uint32_t{pixel[3]} << 24) | (uint32_t{pixel[2]} << 16) |
         (uint32_t{pixel[1]} << 8) | uint32_t{pixel[0]};
}

// The middle of a 40-point annotation whose /Rect starts at (left, bottom),
// in a page-sized bitmap (y down).
int MiddleX(int left) {
  return left + 20;
}
int MiddleY(int bottom) {
  return kPageSize - bottom - 20;
}

ScopedFPDFBitmap WhiteBitmap() {
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(kPageSize, kPageSize, /*alpha=*/1));
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, kPageSize, kPageSize, kWhite);
  return bitmap;
}

// Draws one annotation's `mode` and `state` where the page shows it. Returns
// the colour in its middle, or nothing when nothing was drawn.
std::optional<uint32_t> DrawAnnot(FPDF_PAGE page,
                                  FPDF_ANNOTATION annot,
                                  FPDF_ANNOT_APPEARANCEMODE mode,
                                  const char* state,
                                  int left,
                                  int bottom) {
  ScopedFPDFBitmap bitmap = WhiteBitmap();
  const FS_MATRIX identity = {1, 0, 0, 1, 0, 0};
  if (!EPDF_RenderAnnotBitmap(bitmap.get(), page, annot, mode, state, &identity,
                              0)) {
    return std::nullopt;
  }
  return PixelAt(bitmap.get(), MiddleX(left), MiddleY(bottom));
}

std::vector<std::string> StateNames(FPDF_ANNOTATION annot,
                                    FPDF_ANNOT_APPEARANCEMODE mode) {
  std::vector<std::string> names;
  const int count = EPDFAnnot_GetAppearanceStateCount(annot, mode);
  for (int index = 0; index < count; ++index) {
    const unsigned long length =
        EPDFAnnot_GetAppearanceStateName(annot, mode, index, nullptr, 0);
    std::string name(length, '\0');
    EXPECT_EQ(length, EPDFAnnot_GetAppearanceStateName(annot, mode, index,
                                                       name.data(), length));
    name.pop_back();  // the NUL
    names.push_back(name);
  }
  return names;
}

// The content of the state of the normal appearance that isn't Off.
ByteString OnStateContent(FPDF_ANNOTATION annot) {
  const CPDF_Dictionary* dict =
      CPDFAnnotContextFromFPDFAnnotation(annot)->GetAnnotDict();
  RetainPtr<const CPDF_Dictionary> normal =
      dict->GetDictFor("AP")->GetDictFor("N");
  RetainPtr<const CPDF_Stream> on;
  CPDF_DictionaryLocker locker(normal);
  for (const auto& [name, value] : locker) {
    if (name != "Off") {
      on = ToStream(value->GetDirect());
    }
  }
  if (!on) {
    return ByteString();
  }
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(on);
  acc->LoadAllDataFiltered();
  return ByteString(ByteStringView(acc->GetSpan()));
}

// The fixture, opened as the engine opens documents: with no form fill
// environment, which would write an appearance for every field without one
// as soon as a page loads.
ScopedFPDFDocument OpenFixture() {
  const std::string path =
      PathService::GetTestFilePath("appearance_states.pdf");
  return ScopedFPDFDocument(FPDF_LoadDocument(path.c_str(), nullptr));
}

}  // namespace

class EPDFAppearanceStateEmbedderTest : public EmbedderTest {};

TEST_F(EPDFAppearanceStateEmbedderTest, DrawsTheChosenStateWhateverAsSays) {
  ScopedFPDFDocument doc = OpenFixture();
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation box(FPDFPage_GetAnnot(page.get(), kStates));
  ASSERT_TRUE(box);

  // /AS is Off: without a state, the off look; with one, that state.
  EXPECT_EQ(kGreen,
            DrawAnnot(page.get(), box.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                      nullptr, 20, 20));
  // An empty state is no state, for bindings that can't pass NULL.
  EXPECT_EQ(kGreen, DrawAnnot(page.get(), box.get(),
                              FPDF_ANNOT_APPEARANCEMODE_NORMAL, "", 20, 20));
  EXPECT_EQ(kRed, DrawAnnot(page.get(), box.get(),
                            FPDF_ANNOT_APPEARANCEMODE_NORMAL, "Yes", 20, 20));
  EXPECT_EQ(kGreen, DrawAnnot(page.get(), box.get(),
                              FPDF_ANNOT_APPEARANCEMODE_NORMAL, "Off", 20, 20));
  EXPECT_EQ(kBlue, DrawAnnot(page.get(), box.get(),
                             FPDF_ANNOT_APPEARANCEMODE_DOWN, "Yes", 20, 20));
  EXPECT_EQ(kYellow, DrawAnnot(page.get(), box.get(),
                               FPDF_ANNOT_APPEARANCEMODE_DOWN, "Off", 20, 20));
  EXPECT_EQ(kBlack,
            DrawAnnot(page.get(), box.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER,
                      "Yes", 20, 20));

  // A state the mode doesn't store draws nothing, even one normal stores;
  // so does an entry that is no stream.
  EXPECT_FALSE(DrawAnnot(page.get(), box.get(),
                         FPDF_ANNOT_APPEARANCEMODE_NORMAL, "Maybe", 20, 20));
  EXPECT_FALSE(DrawAnnot(page.get(), box.get(),
                         FPDF_ANNOT_APPEARANCEMODE_ROLLOVER, "Off", 20, 20));
  EXPECT_FALSE(DrawAnnot(page.get(), box.get(),
                         FPDF_ANNOT_APPEARANCEMODE_ROLLOVER, "Bad", 20, 20));

  // Choosing a state never changes which one /AS selects.
  EXPECT_EQ(kGreen,
            DrawAnnot(page.get(), box.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                      nullptr, 20, 20));
}

TEST_F(EPDFAppearanceStateEmbedderTest, ASingleStreamHasNoStates) {
  ScopedFPDFDocument doc = OpenFixture();
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation square(FPDFPage_GetAnnot(page.get(), kSquare));
  ASSERT_TRUE(square);

  EXPECT_EQ(kCyan,
            DrawAnnot(page.get(), square.get(),
                      FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 100, 20));
  EXPECT_FALSE(DrawAnnot(page.get(), square.get(),
                         FPDF_ANNOT_APPEARANCEMODE_NORMAL, "Yes", 100, 20));
  // A mode it doesn't have falls back to the normal stream, as before.
  EXPECT_EQ(kCyan, DrawAnnot(page.get(), square.get(),
                             FPDF_ANNOT_APPEARANCEMODE_DOWN, nullptr, 100, 20));
  EXPECT_EQ(0, EPDFAnnot_GetAppearanceStateCount(
                   square.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL));
}

TEST_F(EPDFAppearanceStateEmbedderTest, ListsTheStatesEachModeStores) {
  ScopedFPDFDocument doc = OpenFixture();
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation box(FPDFPage_GetAnnot(page.get(), kStates));
  ASSERT_TRUE(box);

  EXPECT_EQ((std::vector<std::string>{"Off", "Yes"}),
            StateNames(box.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL));
  EXPECT_EQ((std::vector<std::string>{"Off", "Yes"}),
            StateNames(box.get(), FPDF_ANNOT_APPEARANCEMODE_DOWN));
  // The entry that is no stream is no state.
  EXPECT_EQ((std::vector<std::string>{"Yes"}),
            StateNames(box.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER));
  // Hidden or not, an annotation lists what it stores.
  ScopedFPDFAnnotation hidden(FPDFPage_GetAnnot(page.get(), kHidden));
  ASSERT_TRUE(hidden);
  EXPECT_EQ((std::vector<std::string>{"Off"}),
            StateNames(hidden.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL));

  // Out of range, a mode that doesn't exist, no handle: nothing.
  char buffer[8] = {};
  EXPECT_EQ(0u, EPDFAnnot_GetAppearanceStateName(
                    box.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, 2, buffer, 8));
  EXPECT_EQ(0u,
            EPDFAnnot_GetAppearanceStateName(
                box.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, -1, buffer, 8));
  EXPECT_EQ(0, EPDFAnnot_GetAppearanceStateCount(
                   box.get(), FPDF_ANNOT_APPEARANCEMODE_COUNT));
  EXPECT_EQ(0, EPDFAnnot_GetAppearanceStateCount(
                   nullptr, FPDF_ANNOT_APPEARANCEMODE_NORMAL));

  // A buffer too small is left alone, and the length still comes back.
  char small[2] = {'x', 'x'};
  EXPECT_EQ(4u, EPDFAnnot_GetAppearanceStateName(
                    box.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, 1, small, 2));
  EXPECT_EQ('x', small[0]);

  // Every listed name draws.
  for (const std::string& name :
       StateNames(box.get(), FPDF_ANNOT_APPEARANCEMODE_DOWN)) {
    EXPECT_TRUE(DrawAnnot(page.get(), box.get(), FPDF_ANNOT_APPEARANCEMODE_DOWN,
                          name.c_str(), 20, 20));
  }
}

TEST_F(EPDFAppearanceStateEmbedderTest, UnrotatedRenderTakesAState) {
  ScopedFPDFDocument doc = OpenFixture();
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation box(FPDFPage_GetAnnot(page.get(), kStates));
  ASSERT_TRUE(box);

  const FS_MATRIX identity = {1, 0, 0, 1, 0, 0};
  ScopedFPDFBitmap on = WhiteBitmap();
  ASSERT_TRUE(EPDF_RenderAnnotBitmapUnrotated(
      on.get(), page.get(), box.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, "Yes",
      /*degrees=*/0, /*box=*/nullptr, &identity, 0));
  EXPECT_EQ(kRed, PixelAt(on.get(), MiddleX(20), MiddleY(20)));

  ScopedFPDFBitmap missing = WhiteBitmap();
  EXPECT_FALSE(EPDF_RenderAnnotBitmapUnrotated(
      missing.get(), page.get(), box.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
      "Maybe", /*degrees=*/0, /*box=*/nullptr, &identity, 0));
}

TEST_F(EPDFAppearanceStateEmbedderTest,
       PageRenderDrawsAnnotationsAndWidgetsIndependently) {
  ScopedFPDFDocument doc = OpenFixture();
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);

  auto toggles_without_appearance = [&] {
    int count = 0;
    for (int index : {kCross, kSquareRadio, kPlain, kDot}) {
      ScopedFPDFAnnotation toggle(FPDFPage_GetAnnot(page.get(), index));
      count += toggle && !FPDFAnnot_HasKey(toggle.get(), "AP");
    }
    return count;
  };
  ASSERT_EQ(4, toggles_without_appearance());

  auto render = [&](int flags) {
    ScopedFPDFBitmap bitmap = WhiteBitmap();
    FPDF_RenderPageBitmap(bitmap.get(), page.get(), 0, 0, kPageSize, kPageSize,
                          0, flags);
    return bitmap;
  };

  // Neither flag: the page content only.
  ScopedFPDFBitmap content = render(0);
  EXPECT_EQ(kWhite, PixelAt(content.get(), MiddleX(20), MiddleY(20)));
  EXPECT_EQ(kWhite, PixelAt(content.get(), MiddleX(100), MiddleY(20)));

  // Annotations only: the square, not the widgets.
  ScopedFPDFBitmap annotations = render(FPDF_ANNOT);
  EXPECT_EQ(kCyan, PixelAt(annotations.get(), MiddleX(100), MiddleY(20)));
  EXPECT_EQ(kWhite, PixelAt(annotations.get(), MiddleX(20), MiddleY(20)));

  // Widgets only: the visible widget in its current state, not the square.
  // The hidden widget stays hidden.
  ScopedFPDFBitmap widgets = render(EPDF_RENDER_WIDGETS);
  EXPECT_EQ(kGreen, PixelAt(widgets.get(), MiddleX(20), MiddleY(20)));
  EXPECT_EQ(kWhite, PixelAt(widgets.get(), MiddleX(100), MiddleY(20)));
  EXPECT_EQ(kWhite, PixelAt(widgets.get(), MiddleX(160), MiddleY(20)));

  // Both: everything the page shows.
  ScopedFPDFBitmap both = render(FPDF_ANNOT | EPDF_RENDER_WIDGETS);
  EXPECT_EQ(kCyan, PixelAt(both.get(), MiddleX(100), MiddleY(20)));
  EXPECT_EQ(kGreen, PixelAt(both.get(), MiddleX(20), MiddleY(20)));
  EXPECT_EQ(kWhite, PixelAt(both.get(), MiddleX(160), MiddleY(20)));

  // Drawing widgets writes nothing: the toggles without an appearance still
  // have none.
  EXPECT_EQ(4, toggles_without_appearance());
}

TEST_F(EPDFAppearanceStateEmbedderTest, CheckBoxDrawsTheSymbolItsCaptionNames) {
  ScopedFPDFDocument doc = OpenFixture();
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);

  // /CA 8 and a blue /DA: a blue cross, stroked.
  ScopedFPDFAnnotation cross(FPDFPage_GetAnnot(page.get(), kCross));
  ASSERT_TRUE(cross);
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(cross.get()));
  const ByteString cross_content = OnStateContent(cross.get());
  EXPECT_NE(cross_content.Find("q\n0 0 1 RG\n"), std::nullopt) << cross_content;
  EXPECT_NE(cross_content.Find(" l\nS\nQ\n"), std::nullopt) << cross_content;
  EXPECT_EQ(cross_content.Find(" c\n"), std::nullopt) << cross_content;

  // No caption and no colour: the black check this generator always drew,
  // byte for byte.
  ScopedFPDFAnnotation plain(FPDFPage_GetAnnot(page.get(), kPlain));
  ASSERT_TRUE(plain);
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(plain.get()));
  const ByteString plain_content = OnStateContent(plain.get());
  EXPECT_NE(plain_content.Find("q\n0 0 0 rg\n"), std::nullopt) << plain_content;
  EXPECT_NE(plain_content.Find(" c\nf\nQ\n"), std::nullopt) << plain_content;

  // Both states draw, and the on state shows the cross where the page has it.
  EXPECT_EQ((std::vector<std::string>{"Off", "Yes"}),
            StateNames(cross.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL));
  EXPECT_EQ(kWhite,
            DrawAnnot(page.get(), cross.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                      "Off", 20, 100));
}

TEST_F(EPDFAppearanceStateEmbedderTest,
       RadioButtonDrawsTheSymbolItsCaptionNames) {
  ScopedFPDFDocument doc = OpenFixture();
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);

  // /CA n and a red /DA: a red square in a box, not a circle.
  ScopedFPDFAnnotation square(FPDFPage_GetAnnot(page.get(), kSquareRadio));
  ASSERT_TRUE(square);
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(square.get()));
  const ByteString square_content = OnStateContent(square.get());
  EXPECT_NE(square_content.Find(" re S\n"), std::nullopt) << square_content;
  EXPECT_NE(square_content.Find("q\n1 0 0 rg\n"), std::nullopt)
      << square_content;
  EXPECT_NE(square_content.Find(" l\nh\nf\nQ\n"), std::nullopt)
      << square_content;
  EXPECT_EQ(square_content.Find(" c\n"), std::nullopt) << square_content;
  EXPECT_EQ(kRed, DrawAnnot(page.get(), square.get(),
                            FPDF_ANNOT_APPEARANCEMODE_NORMAL, "Yes", 100, 100));

  // No caption: the dot this generator always drew, now in the /DA colour.
  ScopedFPDFAnnotation dot(FPDFPage_GetAnnot(page.get(), kDot));
  ASSERT_TRUE(dot);
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(dot.get()));
  const ByteString dot_content = OnStateContent(dot.get());
  EXPECT_NE(dot_content.Find("q\n0 0 1 rg\n"), std::nullopt) << dot_content;
  EXPECT_NE(dot_content.Find(" c\nh\nf*\nQ\n"), std::nullopt) << dot_content;
  EXPECT_EQ(kBlue,
            DrawAnnot(page.get(), dot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                      "Yes", 220, 100));
}

TEST_F(EPDFAppearanceStateEmbedderTest, TheShownStateReadsAsTheStateNames) {
  ScopedFPDFDocument doc = OpenFixture();
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);

  // The same raw bytes as the state list gives, so a caller can match the
  // shown state to its image, also when the name isn't ASCII.
  ScopedFPDFAnnotation si(FPDFPage_GetAnnot(page.get(), kSi));
  ASSERT_TRUE(si);
  const unsigned long length =
      EPDFAnnot_GetAppearanceState(si.get(), nullptr, 0);
  ASSERT_EQ(4u, length);  // "S", the two bytes of the i, the NUL
  std::string shown(length, '\0');
  ASSERT_EQ(length,
            EPDFAnnot_GetAppearanceState(si.get(), shown.data(), length));
  shown.pop_back();
  EXPECT_EQ("S\xC3\xAD", shown);
  const std::vector<std::string> names =
      StateNames(si.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL);
  EXPECT_EQ((std::vector<std::string>{"Off", "S\xC3\xAD"}), names);
  EXPECT_EQ(kRed,
            DrawAnnot(page.get(), si.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                      shown.c_str(), 220, 20));

  // Off reads as Off; an annotation without /AS has none.
  ScopedFPDFAnnotation box(FPDFPage_GetAnnot(page.get(), kStates));
  ASSERT_TRUE(box);
  char off[8] = {};
  EXPECT_EQ(4u, EPDFAnnot_GetAppearanceState(box.get(), off, sizeof(off)));
  EXPECT_STREQ("Off", off);
  ScopedFPDFAnnotation square(FPDFPage_GetAnnot(page.get(), kSquare));
  ASSERT_TRUE(square);
  EXPECT_EQ(0u, EPDFAnnot_GetAppearanceState(square.get(), nullptr, 0));
  EXPECT_EQ(0u, EPDFAnnot_GetAppearanceState(nullptr, nullptr, 0));
}
