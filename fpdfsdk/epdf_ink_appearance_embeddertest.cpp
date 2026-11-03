// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Ink appearances against the ones Acrobat writes (cpdf_ink_curves.h). Each
// case is an ink annotation Acrobat drew from its /InkList alone, on save,
// with the appearance it wrote; ours must draw the same cubics, every number
// within 0.001 pt.

#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "constants/annotation_common.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_annot.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "testing/embedder_test.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using testing::HasSubstr;
using testing::Not;

namespace {

using Stroke = std::vector<FS_POINTF>;

// A path as its numbers, one list per subpath: the point its `m` moves to,
// then each `c`'s six.
using PathNumbers = std::vector<std::vector<float>>;

// An ink annotation as Acrobat drew it: its strokes, and the path of the
// appearance Acrobat wrote for them (the `m` and `c` lines).
struct AcrobatInk {
  const char* name;
  std::vector<Stroke> strokes;
  const char* path;
};

// The cases of ink-curves.pdf, all at /W 2 in red. One point (case C) is a
// dot, below.
AcrobatInk UnevenSegments() {
  return {"A: uneven segments",
          {{{60, 620}, {75, 680}, {160, 700}, {175, 630}, {300, 640}}},
          "60 620 m\n"
          "65 640 58.902 667.1216 75 680 c\n"
          "97.7288 698.183 133.9659 713.0171 160 700 c\n"
          "181.3438 689.3281 153.0664 639.4001 175 630 c\n"
          "213.4201 613.5342 258.3333 636.6667 300 640 c\n"};
}

AcrobatInk Dense() {
  return {
      "H: dense, like a real pen",
      {{{260, 300},   {263, 302.5}, {266, 301},   {269, 303.5}, {272, 302},
        {275, 304.5}, {278, 303},   {281, 305.5}, {284, 304},   {287, 306.5},
        {290, 305},   {293, 307.5}, {296, 306},   {299, 308.5}, {302, 307},
        {305, 309.5}, {308, 308},   {311, 310.5}, {314, 309},   {317, 311.5},
        {320, 310},   {323, 312.5}, {326, 311},   {329, 313.5}, {332, 312},
        {335, 314.5}, {338, 313},   {341, 315.5}, {344, 314},   {347, 316.5},
        {350, 315},   {353, 317.5}, {356, 316},   {359, 318.5}, {362, 317},
        {365, 319.5}, {368, 318},   {371, 320.5}, {374, 319},   {377, 321.5}}},
      "260 300 m\n"
      "268.7073 307.2561 257.6714 299.6119 266 301 c\n"
      "267.284 301.214 267.716 303.286 269 303.5 c\n"
      "270.1028 303.6838 270.8972 301.8162 272 302 c\n"
      "273.284 302.214 273.716 304.286 275 304.5 c\n"
      "276.1028 304.6838 276.8972 302.8162 278 303 c\n"
      "279.284 303.214 279.716 305.286 281 305.5 c\n"
      "282.1028 305.6838 282.8972 303.8162 284 304 c\n"
      "285.284 304.214 285.716 306.286 287 306.5 c\n"
      "288.1028 306.6838 288.8972 304.8162 290 305 c\n"
      "291.284 305.214 291.716 307.286 293 307.5 c\n"
      "294.1028 307.6838 294.8972 305.8162 296 306 c\n"
      "297.284 306.214 297.716 308.286 299 308.5 c\n"
      "300.1028 308.6838 300.8972 306.8162 302 307 c\n"
      "303.284 307.214 303.716 309.286 305 309.5 c\n"
      "306.1028 309.6838 306.8972 307.8162 308 308 c\n"
      "309.284 308.214 309.716 310.286 311 310.5 c\n"
      "312.1028 310.6838 312.8972 308.8162 314 309 c\n"
      "315.284 309.214 315.716 311.286 317 311.5 c\n"
      "318.1028 311.6838 318.8972 309.8162 320 310 c\n"
      "321.284 310.214 321.716 312.286 323 312.5 c\n"
      "324.1028 312.6838 324.8972 310.8162 326 311 c\n"
      "327.284 311.214 327.716 313.286 329 313.5 c\n"
      "330.1028 313.6838 330.8972 311.8162 332 312 c\n"
      "333.284 312.214 333.716 314.286 335 314.5 c\n"
      "336.1028 314.6838 336.8972 312.8162 338 313 c\n"
      "339.284 313.214 339.716 315.286 341 315.5 c\n"
      "342.1028 315.6838 342.8972 313.8162 344 314 c\n"
      "345.284 314.214 345.716 316.286 347 316.5 c\n"
      "348.1028 316.6838 348.8972 314.8162 350 315 c\n"
      "351.284 315.214 351.716 317.286 353 317.5 c\n"
      "354.1028 317.6838 354.8972 315.8162 356 316 c\n"
      "357.284 316.214 357.716 318.286 359 318.5 c\n"
      "360.1028 318.6838 360.8972 316.8162 362 317 c\n"
      "363.284 317.214 363.716 319.286 365 319.5 c\n"
      "366.1028 319.6838 366.8972 317.8162 368 318 c\n"
      "369.284 318.214 369.716 320.286 371 320.5 c\n"
      "379.3286 321.8881 368.2927 314.2439 377 321.5 c\n"};
}

std::vector<AcrobatInk> AcrobatInks() {
  return {
      UnevenSegments(),
      {"B: two points",
       {{{60, 520}, {200, 580}}},
       "60 520 m\n"
       "106.6667 540 153.3333 560 200 580 c\n"},
      {"D: a point repeated",
       {{{330, 520}, {330, 520}, {370, 580}, {410, 520}}},
       "330 520 m\n"
       "343.3333 540 345.963 580 370 580 c\n"
       "394.037 580 396.6667 540 410 520 c\n"},
      {"E: straight back on itself",
       {{{60, 420}, {140, 470}, {60, 420}}},
       "60 420 m\n"
       "86.6667 436.6667 140 470 140 470 c\n"
       "140 470 86.6667 436.6667 60 420 c\n"},
      {"F: two strokes",
       {{{220, 420}, {260, 470}, {300, 420}},
        {{330, 420}, {370, 470}, {410, 440}}},
       "220 420 m\n"
       "233.3333 436.6667 238.6563 470 260 470 c\n"
       "281.3438 470 286.6667 436.6667 300 420 c\n"
       "330 420 m\n"
       "343.3333 436.6667 349.2935 464.8234 370 470 c\n"
       "386.169 474.0423 396.6667 450 410 440 c\n"},
      {"G: a tight turn between long runs",
       {{{60, 320}, {200, 330}, {205, 300}, {60, 290}}},
       "60 320 m\n"
       "106.6667 323.3333 153.6532 336.3927 200 330 c\n"
       "210.0429 328.6148 214.7479 302.7851 205 300 c\n"
       "158.416 286.6903 108.3333 293.3333 60 290 c\n"},
      Dense(),
  };
}

PathNumbers PathOf(const std::string& content) {
  PathNumbers path;
  std::vector<float> operands;
  std::istringstream tokens(content);
  std::string token;
  while (tokens >> token) {
    char* end = nullptr;
    const float value = std::strtof(token.c_str(), &end);
    if (end != token.c_str() && *end == '\0') {
      operands.push_back(value);
      continue;
    }
    if (token == "m" && operands.size() >= 2) {
      path.push_back({operands.end()[-2], operands.end()[-1]});
    } else if (token == "c" && operands.size() >= 6 && !path.empty()) {
      path.back().insert(path.back().end(), operands.end() - 6, operands.end());
    }
    operands.clear();
  }
  return path;
}

// `path` moved by (dx, dy).
PathNumbers Moved(PathNumbers path, float dx, float dy) {
  for (std::vector<float>& subpath : path) {
    for (size_t i = 0; i < subpath.size(); ++i) {
      subpath[i] += i % 2 == 0 ? dx : dy;
    }
  }
  return path;
}

std::vector<Stroke> Moved(std::vector<Stroke> strokes, float dx, float dy) {
  for (Stroke& stroke : strokes) {
    for (FS_POINTF& point : stroke) {
      point.x += dx;
      point.y += dy;
    }
  }
  return strokes;
}

void ExpectSamePath(const PathNumbers& expected, const PathNumbers& actual) {
  ASSERT_EQ(expected.size(), actual.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(expected[i].size(), actual[i].size()) << "subpath " << i;
    for (size_t j = 0; j < expected[i].size(); ++j) {
      EXPECT_NEAR(expected[i][j], actual[i][j], 0.001f)
          << "subpath " << i << ", number " << j;
    }
  }
}

// The content of `annot`'s normal appearance.
std::string AppearanceOf(FPDF_ANNOTATION annot) {
  const CPDF_Dictionary* annot_dict =
      CPDFAnnotContextFromFPDFAnnotation(annot)->GetAnnotDict();
  RetainPtr<const CPDF_Dictionary> ap_dict =
      annot_dict->GetDictFor(pdfium::annotation::kAP);
  RetainPtr<const CPDF_Stream> normal =
      ap_dict ? ap_dict->GetStreamFor("N") : nullptr;
  if (!normal) {
    ADD_FAILURE() << "no normal appearance";
    return std::string();
  }
  auto stream_acc = pdfium::MakeRetain<CPDF_StreamAcc>(std::move(normal));
  stream_acc->LoadAllDataFiltered();
  const pdfium::span<const uint8_t> data = stream_acc->GetSpan();
  return std::string(data.begin(), data.end());
}

// A red ink annotation of `strokes` at `width`, with the appearance we draw
// for it.
ScopedFPDFAnnotation NewRedInk(FPDF_PAGE page,
                               const std::vector<Stroke>& strokes,
                               float width) {
  ScopedFPDFAnnotation ink(FPDFPage_CreateAnnot(page, FPDF_ANNOT_INK));
  EXPECT_TRUE(ink);
  for (const Stroke& stroke : strokes) {
    EXPECT_GE(FPDFAnnot_AddInkStroke(ink.get(), stroke.data(), stroke.size()),
              0);
  }
  EXPECT_TRUE(
      FPDFAnnot_SetColor(ink.get(), FPDFANNOT_COLORTYPE_Color, 255, 0, 0, 255));
  EXPECT_TRUE(FPDFAnnot_SetBorder(ink.get(), 0, 0, width));
  EXPECT_TRUE(EPDFAnnot_GenerateAppearance(ink.get()));
  return ink;
}

}  // namespace

class EPDFInkAppearanceEmbedderTest : public EmbedderTest {};

TEST_F(EPDFInkAppearanceEmbedderTest, DrawsAcrobatsCurves) {
  embedpdf_test::ScopedTestDocument doc(NewTestDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 612, 792));
  ASSERT_TRUE(page);
  for (const AcrobatInk& acrobat : AcrobatInks()) {
    SCOPED_TRACE(acrobat.name);
    ScopedFPDFAnnotation ink = NewRedInk(page.get(), acrobat.strokes, 2);
    const std::string content = AppearanceOf(ink.get());
    // Set as Acrobat sets it: colour, width, round caps, and no join.
    EXPECT_THAT(content, HasSubstr("1 0 0 RG\n2 w\n1 J\n"));
    EXPECT_THAT(content, Not(HasSubstr(" j\n")));
    ExpectSamePath(PathOf(acrobat.path), PathOf(content));
  }
}

// One point is Acrobat's dot: a circle of radius 0.5, filled and stroked.
// Acrobat leaves the fill colour at its black default, which a stroke of
// width 1 or more covers; ours fills in the stroke's colour.
TEST_F(EPDFInkAppearanceEmbedderTest, OnePointIsAcrobatsDot) {
  embedpdf_test::ScopedTestDocument doc(NewTestDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 612, 792));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation ink = NewRedInk(page.get(), {{{260, 550}}}, 2);
  EXPECT_THAT(AppearanceOf(ink.get()),
              HasSubstr("1 0 0 RG\n2 w\n1 J\n1 0 0 rg\n"
                        "260 550.5 m\n"
                        "260.2778 550.5 260.5 550.2778 260.5 550 c\n"
                        "260.5 549.7222 260.2778 549.5 260 549.5 c\n"
                        "259.7222 549.5 259.5 549.7222 259.5 550 c\n"
                        "259.5 550.2778 259.7222 550.5 260 550.5 c\n"
                        "h\n"
                        "B\n"));
}

// ink-widths.pdf: Acrobat drew the dense stroke and the uneven one at /W 0.5,
// 1, 4 and 8, each 170 pt below the one before, with the same curves (cases H
// and A moved). The width doesn't change the fit; 1, the default, isn't
// written.
TEST_F(EPDFInkAppearanceEmbedderTest, TheCurvesDoNotDependOnTheWidth) {
  embedpdf_test::ScopedTestDocument doc(NewTestDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 612, 792));
  ASSERT_TRUE(page);
  struct Width {
    float width;
    const char* written;
  };
  static constexpr Width kWidths[] = {{0.5f, "1 0 0 RG\n0.5 w\n1 J\n"},
                                      {1, "1 0 0 RG\n1 J\n"},
                                      {4, "1 0 0 RG\n4 w\n1 J\n"},
                                      {8, "1 0 0 RG\n8 w\n1 J\n"}};
  const AcrobatInk dense_ink = Dense();
  const AcrobatInk uneven_ink = UnevenSegments();
  for (size_t i = 0; i < std::size(kWidths); ++i) {
    SCOPED_TRACE(kWidths[i].width);
    const float down = 170.0f * i;
    ScopedFPDFAnnotation dense =
        NewRedInk(page.get(), Moved(dense_ink.strokes, -220, 420 - down),
                  kWidths[i].width);
    ScopedFPDFAnnotation uneven =
        NewRedInk(page.get(), Moved(uneven_ink.strokes, 240, 60 - down),
                  kWidths[i].width);
    for (FPDF_ANNOTATION ink : {dense.get(), uneven.get()}) {
      EXPECT_THAT(AppearanceOf(ink), HasSubstr(kWidths[i].written));
    }
    ExpectSamePath(Moved(PathOf(dense_ink.path), -220, 420 - down),
                   PathOf(AppearanceOf(dense.get())));
    ExpectSamePath(Moved(PathOf(uneven_ink.path), 240, 60 - down),
                   PathOf(AppearanceOf(uneven.get())));
  }
}

// Cell 1E of no-stroke-check.pdf, a zigzag of nine points: every point is
// a turn, so every pair is its own cubic, its handles a third of its length
// and level at each tip. The first cubic is Acrobat's own.
TEST_F(EPDFInkAppearanceEmbedderTest, AZigzagIsOneCubicPerPair) {
  embedpdf_test::ScopedTestDocument doc(NewTestDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 612, 792));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation ink = NewRedInk(page.get(),
                                       {{{526, 654},
                                         {531, 682},
                                         {536, 654},
                                         {541, 682},
                                         {546, 654},
                                         {551, 682},
                                         {556, 654},
                                         {561, 682},
                                         {566, 654}}},
                                       2);
  const std::string content = AppearanceOf(ink.get());
  EXPECT_THAT(
      content,
      HasSubstr("526 654 m\n527.6667 663.3333 521.519 682 531 682 c\n"));
  ExpectSamePath(PathOf("526 654 m\n"
                        "527.6667 663.3333 521.519 682 531 682 c\n"
                        "540.481 682 526.519 654 536 654 c\n"
                        "545.481 654 531.519 682 541 682 c\n"
                        "550.481 682 536.519 654 546 654 c\n"
                        "555.481 654 541.519 682 551 682 c\n"
                        "560.481 682 546.519 654 556 654 c\n"
                        "565.481 654 551.519 682 561 682 c\n"
                        "570.481 682 564.3333 663.3333 566 654 c\n"),
                 PathOf(content));
}
