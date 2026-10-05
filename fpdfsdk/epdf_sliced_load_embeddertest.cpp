// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// A page loaded in slices (EPDFDoc_StartLoadPageByObjectNumber,
// EPDFPage_ContinueLoad) is the page FPDF_LoadPage() loads in one go: the same
// objects, with the same bounds, clips and flags, and the same pixels. The
// parse pauses inside nested forms, which it parses after the content that
// places them; a slice keeps to its budget, closing a page abandons its load,
// and a call that reads a page still loading finishes the load first.
//
// The corpus gate runs when EPDF_SLICED_LOAD_CORPUS names PDF files or
// directories (separated by ':'); EPDF_SLICED_LOAD_MAX_PAGES (default 20)
// limits the pages per file, EPDF_SLICED_LOAD_SKIP (':'-separated) skips files
// whose path contains one of its parts.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <ratio>
#include <string>
#include <vector>

#include "core/fpdfapi/page/cpdf_clippath.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcodec/flate/flatemodule.h"
#include "core/fxcrt/bytestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdf_edit.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

std::string Number(float value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.9g", value);
  return buffer;
}

std::string Rect(const CFX_FloatRect& rect) {
  return "[" + Number(rect.left) + " " + Number(rect.bottom) + " " +
         Number(rect.right) + " " + Number(rect.top) + "]";
}

std::string Matrix(const CFX_Matrix& m) {
  return "[" + Number(m.a) + " " + Number(m.b) + " " + Number(m.c) + " " +
         Number(m.d) + " " + Number(m.e) + " " + Number(m.f) + "]";
}

// Everything parsing decides about a holder's objects, forms opened.
void Describe(const CPDF_PageObjectHolder& holder,
              const std::string& indent,
              std::string& out) {
  out += indent + "holder parsed=" +
         std::to_string(static_cast<int>(holder.GetParseState())) +
         " alpha=" + std::to_string(holder.BackgroundAlphaNeeded()) +
         " masks=" + std::to_string(holder.GetMaskBoundingBoxes().size()) +
         " objects=" + std::to_string(holder.GetPageObjectCount()) + "\n";
  for (const auto& object : holder) {
    out += indent + std::to_string(static_cast<int>(object->GetType())) +
           " rect=" + Rect(object->GetRect()) +
           " stream=" + std::to_string(object->GetContentStream()) +
           " active=" + std::to_string(object->IsActive());
    const CPDF_ClipPath& clip = object->clip_path();
    if (clip.HasRef()) {
      out += " clip=" + std::to_string(clip.GetPathCount()) + "/" +
             std::to_string(clip.GetTextCount()) + Rect(clip.GetClipBox());
    }
    if (const CPDF_PathObject* path = object->AsPath()) {
      out += " points=" + std::to_string(path->path().GetPoints().size()) +
             " fill=" + std::to_string(static_cast<int>(path->filltype())) +
             " stroke=" + std::to_string(path->stroke()) +
             " matrix=" + Matrix(path->matrix());
    } else if (const CPDF_TextObject* text = object->AsText()) {
      out += " chars=" + std::to_string(text->GetCharCodes().size()) +
             " at=" + Number(text->GetPos().x) + "," + Number(text->GetPos().y);
    } else if (const CPDF_ImageObject* image = object->AsImage()) {
      out += " image=" + Matrix(image->matrix());
    }
    out += "\n";
    if (const CPDF_FormObject* form = object->AsForm()) {
      out += indent + " form matrix=" + Matrix(form->form_matrix()) +
             " bbox=" + Rect(form->form()->GetBBox()) + "\n";
      Describe(*form->form(), indent + "  ", out);
    }
  }
}

struct Parsed {
  bool loaded = false;
  std::string tree;
  std::string pixels;
  EPDF_PAGE_PARSED_SIZE size = {};
};

// Renders at most `max_side` pixels on a side, with annotations.
std::string Pixels(FPDF_PAGE page, int max_side) {
  const float width = FPDF_GetPageWidthF(page);
  const float height = FPDF_GetPageHeightF(page);
  const float longest = std::max(width, height);
  const float scale = longest > max_side ? max_side / longest : 1.0f;
  const int w = std::max(1, static_cast<int>(width * scale));
  const int h = std::max(1, static_cast<int>(height * scale));
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(w, h, 0));
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, w, h, 0xffffffff);
  FPDF_RenderPageBitmap(bitmap.get(), page, 0, 0, w, h, 0, FPDF_ANNOT);

  // FNV-1a over the rows: equal renders give equal digests.
  const auto* pixels =
      static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get()));
  const int stride = FPDFBitmap_GetStride(bitmap.get());
  uint64_t digest = 0xcbf29ce484222325ull;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w * 4; ++x) {
      digest = (digest ^ pixels[y * stride + x]) * 0x100000001b3ull;
    }
  }
  return std::to_string(w) + "x" + std::to_string(h) + ":" +
         std::to_string(digest);
}

Parsed Describe(FPDF_PAGE page, int max_side) {
  Parsed parsed;
  parsed.loaded = true;
  Describe(*CPDFPageFromFPDFPage(page), "", parsed.tree);
  parsed.pixels = Pixels(page, max_side);
  EXPECT_TRUE(EPDFPage_GetParsedSize(page, &parsed.size));
  return parsed;
}

uint32_t ObjectNumberOf(FPDF_DOCUMENT doc, int page_index) {
  RetainPtr<const CPDF_Dictionary> dict =
      CPDFDocumentFromFPDFDocument(doc)->GetPageDictionary(page_index);
  return dict ? dict->GetObjNum() : 0;
}

// The page as loading left it, without finishing a load (the test sees what
// the API hides).
const CPDF_Page* AsIs(FPDF_PAGE page) {
  return CPDFPageFromFPDFPageAsIs(page);
}

// Loads a step at a time until the page holds `count` objects of its own:
// its content parsed, the forms it places not yet.
void LoadUntilPlaced(FPDF_PAGE page, size_t count) {
  while (AsIs(page)->GetPageObjectCount() < count) {
    ASSERT_EQ(0, EPDFPage_ContinueLoad(page, 0));
  }
}

Parsed LoadInOneGo(FPDF_DOCUMENT doc, int page_index) {
  ScopedFPDFPage page(FPDF_LoadPage(doc, page_index));
  return page ? Describe(page.get(), 1200) : Parsed();
}

// Loads in slices of one step each, and counts them in `slices`.
Parsed LoadInSlices(FPDF_DOCUMENT doc, int page_index, int* slices) {
  ScopedFPDFPage page(EPDFDoc_StartLoadPageByObjectNumber(
      doc, ObjectNumberOf(doc, page_index), /*normalized=*/false));
  if (!page) {
    return Parsed();
  }
  int count = 1;
  while (EPDFPage_ContinueLoad(page.get(), 0) == 0) {
    ++count;
  }
  if (slices) {
    *slices = count;
  }
  return Describe(page.get(), 1200);
}

void ExpectSame(const Parsed& expected, const Parsed& actual) {
  ASSERT_TRUE(expected.loaded);
  EXPECT_TRUE(actual.loaded);
  EXPECT_EQ(expected.tree, actual.tree);
  EXPECT_EQ(expected.pixels, actual.pixels);
  EXPECT_EQ(expected.size.objects, actual.size.objects);
  EXPECT_EQ(expected.size.path_points, actual.size.path_points);
  EXPECT_EQ(expected.size.text_chars, actual.size.text_chars);
  EXPECT_EQ(expected.size.estimated_bytes, actual.size.estimated_bytes);
}

std::string Form(const std::string& extra, const std::string& content) {
  return "<< /Type /XObject /Subtype /Form " + extra + " /Length " +
         std::to_string(content.size()) + " >>\nstream\n" + content +
         "\nendstream";
}

std::string OnePage(const std::string& resources,
                    const std::string& content,
                    const std::vector<std::string>& more_objects) {
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R "
      "/Resources " +
          resources + " >>",
      Stream(content),
  };
  objects.insert(objects.end(), more_objects.begin(), more_objects.end());
  return MakePdf(objects);
}

// One form holding `paths` short paths, placed once: a drawing exported as a
// single form.
std::string OneHugeForm(int paths) {
  std::string content;
  for (int i = 0; i < paths; ++i) {
    const int x = (i * 37) % 290;
    const int y = (i * 91) % 290;
    content += std::to_string(x) + " " + std::to_string(y) + " m " +
               std::to_string(x + 5) + " " + std::to_string(y + 3) + " l S\n";
  }
  return OnePage("<< /XObject << /Drawing 5 0 R >> >>", "/Drawing Do",
                 {Form("/BBox [0 0 300 300]", content)});
}

class EPDFSlicedLoadEmbedderTest : public EmbedderTest {
 protected:
  // The page loaded in one go and in slices of one step, each on a newly
  // loaded document, so no cache one load fills serves the other.
  void ExpectSameEitherWay(const std::string& pdf) {
    Parsed expected;
    {
      ScopedFPDFDocument doc(
          FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
      ASSERT_TRUE(doc);
      expected = LoadInOneGo(doc.get(), 0);
    }
    ScopedFPDFDocument doc(
        FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
    ASSERT_TRUE(doc);
    int slices = 0;
    ExpectSame(expected, LoadInSlices(doc.get(), 0, &slices));
    EXPECT_GT(slices, 1);
  }

  ScopedFPDFDocument OpenLayer(std::string pdf) {
    input_ = std::move(pdf);
    EPDF_BASE_DOCUMENT base =
        EPDF_LoadMemBaseDocument(input_.data(), input_.size(), nullptr);
    EXPECT_TRUE(base);
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
    EPDF_ReleaseBaseDocument(base);
    return layer;
  }

  std::string input_;
};

}  // namespace

// Three forms deep, with matrices, clips to their boxes, a blend mode deep
// inside (which every holder above must learn of) and content after each
// placement.
TEST_F(EPDFSlicedLoadEmbedderTest, NestedFormsWithClipsAndTransparency) {
  const std::string pdf = OnePage(
      "<< /XObject << /Outer 5 0 R >> /Font << /F1 8 0 R >> >>",
      "0.9 g 0 0 300 300 re f /Outer Do "
      "BT /F1 18 Tf 10 280 Td (after outer) Tj ET "
      "0 0 1 RG 4 w 5 5 290 290 re S",
      {
          Form("/BBox [0 0 200 200] /Matrix [1 0 0 1 20 20] "
               "/Resources << /XObject << /Mid 6 0 R >> >>",
               "1 0 0 rg 0 0 200 200 re f q 0.5 0 0 0.5 10 10 cm /Mid Do Q "
               "0 1 0 rg 150 150 40 40 re f /Mid Do"),
          Form("/BBox [0 0 150 150] /Matrix [0.8 0.2 -0.2 0.8 30 0] "
               "/Resources << /XObject << /Inner 7 0 R >> "
               "/ExtGState << /G1 << /BM /Multiply /ca 0.6 >> >> >>",
               "0 0 1 rg 0 0 300 50 re f /G1 gs /Inner Do"),
          Form("/BBox [0 0 100 100] /Resources << /Font << /F1 8 0 R >> >> "
               "/Group << /S /Transparency >>",
               "1 1 0 rg 10 10 80 80 re f "
               "BT /F1 12 Tf 5 50 Td (inner) Tj ET "
               "0 0 0 RG 0 0 m 120 120 l S"),
          "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      });
  ExpectSameEitherWay(pdf);
}

// A form placed many times, more than a parse step holds, and placed inside
// itself's sibling: the order of placements is the order of objects.
TEST_F(EPDFSlicedLoadEmbedderTest, ManyPlacements) {
  std::string content;
  for (int i = 0; i < 260; ++i) {
    content +=
        "q 1 0 0 1 " + std::to_string(i % 20 * 14) + " " +
        std::to_string(i / 20 * 22) + " cm /Dot Do Q " +
        (i % 7 == 0 ? "0.5 g " + std::to_string(i % 290) + " 290 4 4 re f "
                    : "");
  }
  const std::string pdf =
      OnePage("<< /XObject << /Dot 5 0 R >> >>", content,
              {Form("/BBox [0 0 12 12] /Resources << /XObject << /Ring 6 0 R "
                    ">> >>",
                    "1 0 0 rg 0 0 10 10 re f /Ring Do"),
               Form("/BBox [0 0 12 12]", "0 0 1 RG 2 2 6 6 re S")});
  ExpectSameEitherWay(pdf);
}

// A form that draws itself: the recursion guard stops it at the same depth.
TEST_F(EPDFSlicedLoadEmbedderTest, AFormThatDrawsItself) {
  const std::string pdf = OnePage(
      "<< /XObject << /Self 5 0 R >> >>", "/Self Do",
      {Form("/BBox [0 0 300 300] /Resources << /XObject << /Self 5 0 R >> >>",
            "0 0 1 rg 0 0 20 20 re f 0.9 0 0 0.9 15 15 cm /Self Do")});
  ExpectSameEitherWay(pdf);
}

// Content in several streams, with forms in each: the guard keys on where
// each step starts.
TEST_F(EPDFSlicedLoadEmbedderTest, FormsAcrossContentStreams) {
  const std::string pdf = MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
      "/Contents [4 0 R 5 0 R] "
      "/Resources << /XObject << /A 6 0 R /B 7 0 R >> >> >>",
      Stream("q 1 0 0 1 10 10 cm /A Do Q 0 0 1 rg 100 100 20 20 re f"),
      Stream("q 1 0 0 1 150 150 cm /B Do Q /A Do"),
      Form("/BBox [0 0 50 50] /Resources << /XObject << /B 7 0 R >> >>",
           "1 0 0 rg 0 0 50 50 re f q 0.5 0 0 0.5 0 0 cm /B Do Q"),
      Form("/BBox [0 0 40 40]", "0 1 0 rg 5 5 30 30 re f"),
  });
  ExpectSameEitherWay(pdf);
}

// A form waits for the content that placed it: at the first pause it is
// placed and not parsed, and its bounds come once it is.
TEST_F(EPDFSlicedLoadEmbedderTest, FormsWaitForTheContentThatPlacesThem) {
  const std::string pdf =
      OnePage("<< /XObject << /F 5 0 R >> >>", "/F Do 0 0 1 rg 0 0 10 10 re f",
              {Form("/BBox [0 0 100 100]", "1 0 0 rg 20 20 30 30 re f")});
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(EPDFDoc_StartLoadPageByObjectNumber(
      doc.get(), ObjectNumberOf(doc.get(), 0), /*normalized=*/false));
  ASSERT_TRUE(page);
  LoadUntilPlaced(page.get(), 2);
  ASSERT_EQ(2u, AsIs(page.get())->GetPageObjectCount());
  const CPDF_FormObject* form =
      AsIs(page.get())->GetPageObjectByIndex(0)->AsForm();
  ASSERT_TRUE(form);
  EXPECT_EQ(CPDF_PageObjectHolder::ParseState::kNotParsed,
            form->form()->GetParseState());

  while (EPDFPage_ContinueLoad(page.get(), 0) == 0) {
  }
  EXPECT_EQ(CPDF_PageObjectHolder::ParseState::kParsed,
            form->form()->GetParseState());
  EXPECT_EQ(CFX_FloatRect(20, 20, 50, 50), form->GetRect());
}

// A page drawn inside one huge form pauses inside the form, step by step.
TEST_F(EPDFSlicedLoadEmbedderTest, AHugeFormLoadsInSlices) {
  constexpr int kPaths = 20000;
  const std::string pdf = OneHugeForm(kPaths);
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(EPDFDoc_StartLoadPageByObjectNumber(
      doc.get(), ObjectNumberOf(doc.get(), 0), /*normalized=*/false));
  ASSERT_TRUE(page);
  int slices_inside_the_form = 0;
  while (EPDFPage_ContinueLoad(page.get(), 0) == 0) {
    const CPDF_Page* loading = AsIs(page.get());
    if (loading->GetPageObjectCount() == 1) {
      const size_t parsed = loading->GetPageObjectByIndex(0)
                                ->AsForm()
                                ->form()
                                ->GetPageObjectCount();
      if (parsed > 0 && parsed < static_cast<size_t>(kPaths)) {
        ++slices_inside_the_form;
      }
    }
  }
  EXPECT_GT(slices_inside_the_form, kPaths / 100 - 5);
  EXPECT_EQ(static_cast<unsigned long>(kPaths + 1), [&] {
    EPDF_PAGE_PARSED_SIZE size;
    EPDFPage_GetParsedSize(page.get(), &size);
    return size.objects;
  }());
}

// A content stream that inflates to megabytes decodes over several slices,
// a megabyte each, before its parse starts.
TEST_F(EPDFSlicedLoadEmbedderTest, BigContentDecodesInSlices) {
  std::string content = "0 0 1 rg 10 10 50 50 re f\n";
  for (int i = 0; content.size() < 4 * 1024 * 1024; ++i) {
    content += "% line " + std::to_string(i) + " of a long comment\n";
  }
  content += "1 0 0 rg 100 100 50 50 re f\n";
  const DataVector<uint8_t> compressed =
      FlateModule::Encode(pdfium::as_byte_span(content));
  const std::string pdf = MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R "
      "/Resources << >> >>",
      "<< /Length " + std::to_string(compressed.size()) +
          " /Filter /FlateDecode >>\nstream\n" +
          std::string(compressed.begin(), compressed.end()) + "\nendstream",
  });
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(EPDFDoc_StartLoadPageByObjectNumber(
      doc.get(), ObjectNumberOf(doc.get(), 0), /*normalized=*/false));
  ASSERT_TRUE(page);
  int decoding_slices = 0;
  while (EPDFPage_ContinueLoad(page.get(), 0) == 0) {
    if (AsIs(page.get())->GetPageObjectCount() == 0) {
      ++decoding_slices;
    }
  }
  EXPECT_GE(decoding_slices, 4);
  EXPECT_EQ(2, FPDFPage_CountObjects(page.get()));
  ExpectSameEitherWay(pdf);
}

// Slices keep to their budget, give or take one step.
TEST_F(EPDFSlicedLoadEmbedderTest, SlicesKeepToTheirBudget) {
  constexpr int kBudgetMs = 4;
  const std::string pdf = OneHugeForm(100000);
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(EPDFDoc_StartLoadPageByObjectNumber(
      doc.get(), ObjectNumberOf(doc.get(), 0), /*normalized=*/false));
  ASSERT_TRUE(page);
  int slices = 0;
  double longest_ms = 0;
  for (;;) {
    const auto start = std::chrono::steady_clock::now();
    const int result = EPDFPage_ContinueLoad(page.get(), kBudgetMs);
    longest_ms =
        std::max(longest_ms, std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start)
                                 .count());
    ++slices;
    if (result != 0) {
      ASSERT_EQ(1, result);
      break;
    }
  }
  std::cerr << slices << " slices, the longest " << longest_ms << " ms\n";
  EXPECT_GT(slices, 5);
  // A step of a debug build, with room for a busy machine.
  EXPECT_LT(longest_ms, kBudgetMs + 50);
}

// Closing a page mid-load abandons the load; the document loads it again.
TEST_F(EPDFSlicedLoadEmbedderTest, ClosingMidLoadAbandonsIt) {
  const std::string pdf = OneHugeForm(5000);
  Parsed expected;
  {
    ScopedFPDFDocument doc(
        FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
    expected = LoadInOneGo(doc.get(), 0);
  }
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
  ASSERT_TRUE(doc);
  const uint32_t objnum = ObjectNumberOf(doc.get(), 0);
  for (int steps : {0, 1, 3, 20}) {
    ScopedFPDFPage page(
        EPDFDoc_StartLoadPageByObjectNumber(doc.get(), objnum, false));
    ASSERT_TRUE(page);
    for (int i = 0; i < steps; ++i) {
      ASSERT_EQ(0, EPDFPage_ContinueLoad(page.get(), 0));
    }
  }
  ExpectSame(expected, LoadInOneGo(doc.get(), 0));
}

// A call that reads a page still loading finishes the load first.
TEST_F(EPDFSlicedLoadEmbedderTest, CallsThatReadThePageFinishItsLoad) {
  const std::string pdf = OneHugeForm(3000);
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(EPDFDoc_StartLoadPageByObjectNumber(
      doc.get(), ObjectNumberOf(doc.get(), 0), /*normalized=*/false));
  ASSERT_TRUE(page);
  ASSERT_EQ(0, EPDFPage_ContinueLoad(page.get(), 0));

  // These leave it loading.
  EPDF_PAGE_PARSED_SIZE size;
  ASSERT_TRUE(EPDFPage_GetParsedSize(page.get(), &size));
  EXPECT_FALSE(size.complete);
  EXPECT_TRUE(EPDFPage_IsContentCurrent(page.get()));
  EXPECT_TRUE(EPDFPage_IsValid(page.get()));
  EXPECT_EQ(ObjectNumberOf(doc.get(), 0), EPDFPage_GetObjectNumber(page.get()));
  EXPECT_FLOAT_EQ(300, FPDF_GetPageWidthF(page.get()));
  EXPECT_EQ(CPDF_PageObjectHolder::ParseState::kParsing,
            AsIs(page.get())->GetParseState());

  // This reads it.
  EXPECT_EQ(1, FPDFPage_CountObjects(page.get()));
  ASSERT_TRUE(EPDFPage_GetParsedSize(page.get(), &size));
  EXPECT_TRUE(size.complete);
  EXPECT_EQ(3001u, size.objects);
  EXPECT_EQ(1, EPDFPage_ContinueLoad(page.get(), 0));
}

TEST_F(EPDFSlicedLoadEmbedderTest, StartAndContinueRefuseWhatIsNoPage) {
  const std::string pdf = MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Rotate 90 "
      "/Contents 4 0 R /Resources << >> >>",
      Stream("0 0 1 rg 10 10 50 50 re f"),
  });
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
  ASSERT_TRUE(doc);
  EXPECT_FALSE(EPDFDoc_StartLoadPageByObjectNumber(nullptr, 3, false));
  EXPECT_FALSE(EPDFDoc_StartLoadPageByObjectNumber(doc.get(), 0, false));
  EXPECT_FALSE(EPDFDoc_StartLoadPageByObjectNumber(doc.get(), 4, false));
  EXPECT_FALSE(EPDFDoc_StartLoadPageByObjectNumber(doc.get(), 99, false));
  EXPECT_EQ(-1, EPDFPage_ContinueLoad(nullptr, 0));

  ScopedFPDFPage loaded(FPDF_LoadPage(doc.get(), 0));
  EXPECT_EQ(1, EPDFPage_ContinueLoad(loaded.get(), 0));

  // Normalized or not, it's the page the one-go loads give.
  for (bool normalized : {false, true}) {
    ScopedFPDFPage one_go(
        normalized ? EPDFDoc_LoadPageByObjectNumberNormalized(doc.get(), 3)
                   : EPDFDoc_LoadPageByObjectNumber(doc.get(), 3));
    ScopedFPDFPage sliced(
        EPDFDoc_StartLoadPageByObjectNumber(doc.get(), 3, normalized));
    ASSERT_TRUE(sliced);
    while (EPDFPage_ContinueLoad(sliced.get(), 0) == 0) {
    }
    EXPECT_FLOAT_EQ(FPDF_GetPageWidthF(one_go.get()),
                    FPDF_GetPageWidthF(sliced.get()));
    EXPECT_FLOAT_EQ(FPDF_GetPageHeightF(one_go.get()),
                    FPDF_GetPageHeightF(sliced.get()));
    EXPECT_EQ(Describe(one_go.get(), 300).pixels,
              Describe(sliced.get(), 300).pixels);
  }
}

// A write between slices that changes what the page is - its content, or a
// form it has placed and not parsed yet - leaves it not current.
TEST_F(EPDFSlicedLoadEmbedderTest, AWriteBetweenSlicesLeavesThePageStale) {
  const std::string pdf =
      OnePage("<< /XObject << /F 5 0 R >> >>",
              "0 0 1 rg 0 0 10 10 re f /F Do 20 20 10 10 re f",
              {Form("/BBox [0 0 100 100]", "1 0 0 rg 20 20 30 30 re f")});
  for (bool content : {true, false}) {
    SCOPED_TRACE(content ? "content" : "form");
    ScopedFPDFDocument doc = OpenLayer(pdf);
    ASSERT_TRUE(doc);
    ScopedFPDFPage loading(EPDFDoc_StartLoadPageByObjectNumber(
        doc.get(), ObjectNumberOf(doc.get(), 0), /*normalized=*/false));
    LoadUntilPlaced(loading.get(), 3);
    ASSERT_EQ(CPDF_PageObjectHolder::ParseState::kNotParsed,
              AsIs(loading.get())
                  ->GetPageObjectByIndex(1)
                  ->AsForm()
                  ->form()
                  ->GetParseState());

    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    {
      CPDF_Document* document = CPDFDocumentFromFPDFDocument(doc.get());
      CPDF_DocumentViewScope view(document);
      RetainPtr<CPDF_Object> stream =
          document->GetMutableIndirectObject(content ? 4 : 5);
      ASSERT_TRUE(stream && stream->AsMutableStream());
      stream->AsMutableStream()->SetData(
          ByteStringView("0 1 0 rg 0 0 99 99 re f").unsigned_span());
    }
    ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));

    while (EPDFPage_ContinueLoad(loading.get(), 0) == 0) {
    }
    EXPECT_FALSE(EPDFPage_IsContentCurrent(loading.get()));
    ScopedFPDFPage fresh(FPDF_LoadPage(doc.get(), 0));
    EXPECT_TRUE(EPDFPage_IsContentCurrent(fresh.get()));
  }
}

// How long the slices of real pages take: EPDF_SLICE_TIMES names files
// (separated by ':', each optionally "#page"), EPDF_SLICE_BUDGET_MS is the
// budget (default 8). Prints, per page, the slices, the longest, and the
// slices over twice the budget.
TEST_F(EPDFSlicedLoadEmbedderTest, SliceTimes) {
  const char* files = getenv("EPDF_SLICE_TIMES");
  if (!files || !*files) {
    GTEST_SKIP() << "set EPDF_SLICE_TIMES to PDF files";
  }
  const char* budget_env = getenv("EPDF_SLICE_BUDGET_MS");
  const int budget_ms = budget_env ? atoi(budget_env) : 8;
  for (std::string list = files; !list.empty();) {
    const size_t colon = list.find(':');
    std::string item = list.substr(0, colon);
    list = colon == std::string::npos ? "" : list.substr(colon + 1);
    int page_index = 0;
    const size_t hash = item.rfind('#');
    if (hash != std::string::npos) {
      page_index = atoi(item.c_str() + hash + 1);
      item = item.substr(0, hash);
    }
    ScopedFPDFDocument doc(FPDF_LoadDocument(item.c_str(), nullptr));
    if (!doc) {
      continue;
    }
    ScopedFPDFPage page(EPDFDoc_StartLoadPageByObjectNumber(
        doc.get(), ObjectNumberOf(doc.get(), page_index), false));
    if (!page) {
      continue;
    }
    int slices = 0;
    int over = 0;
    double longest_ms = 0;
    double total_ms = 0;
    for (;;) {
      EPDF_PAGE_PARSED_SIZE before;
      EPDFPage_GetParsedSize(page.get(), &before);
      const auto start = std::chrono::steady_clock::now();
      const int result = EPDFPage_ContinueLoad(page.get(), budget_ms);
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
      ++slices;
      total_ms += ms;
      longest_ms = std::max(longest_ms, ms);
      if (ms > 2 * budget_ms) {
        ++over;
        EPDF_PAGE_PARSED_SIZE after;
        EPDFPage_GetParsedSize(page.get(), &after);
        std::cerr << "[slice-times]   slice " << slices << ": " << ms
                  << " ms, objects " << before.objects << " -> "
                  << after.objects << (result ? ", the last" : "") << "\n";
      }
      if (result != 0) {
        break;
      }
    }
    EPDF_PAGE_PARSED_SIZE size;
    EPDFPage_GetParsedSize(page.get(), &size);
    std::cerr << "[slice-times] " << item.substr(item.rfind('/') + 1) << " #"
              << page_index << ": " << size.objects << " objects, " << slices
              << " slices of " << budget_ms << " ms, " << total_ms
              << " ms in all, the longest " << longest_ms << " ms, " << over
              << " over " << 2 * budget_ms << " ms\n";
  }
}

// The pages of real files, loaded both ways.
TEST_F(EPDFSlicedLoadEmbedderTest, Corpus) {
  const char* corpus = getenv("EPDF_SLICED_LOAD_CORPUS");
  if (!corpus || !*corpus) {
    GTEST_SKIP() << "set EPDF_SLICED_LOAD_CORPUS to PDF files or folders";
  }
  const char* max_pages_env = getenv("EPDF_SLICED_LOAD_MAX_PAGES");
  const int max_pages = max_pages_env ? atoi(max_pages_env) : 20;
  const char* skip_env = getenv("EPDF_SLICED_LOAD_SKIP");

  auto split = [](const std::string& list) {
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= list.size()) {
      size_t end = list.find(':', at);
      if (end == std::string::npos) {
        end = list.size();
      }
      if (end > at) {
        parts.push_back(list.substr(at, end - at));
      }
      at = end + 1;
    }
    return parts;
  };
  const std::vector<std::string> skips = split(skip_env ? skip_env : "");

  std::vector<std::string> files;
  std::vector<std::string> pending = split(corpus);
  while (!pending.empty()) {
    const std::string path = pending.back();
    pending.pop_back();
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
      continue;
    }
    if (S_ISDIR(info.st_mode)) {
      DIR* dir = opendir(path.c_str());
      if (!dir) {
        continue;
      }
      while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
          pending.push_back(path + "/" + name);
        }
      }
      closedir(dir);
      continue;
    }
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".pdf") {
      files.push_back(path);
    }
  }
  std::sort(files.begin(), files.end());

  int pages = 0;
  int forms_pages = 0;
  int unloadable_pages = 0;
  std::vector<std::string> differing;
  for (const std::string& file : files) {
    if (std::any_of(skips.begin(), skips.end(), [&](const std::string& skip) {
          return file.find(skip) != std::string::npos;
        })) {
      continue;
    }
    ScopedFPDFDocument one_go(FPDF_LoadDocument(file.c_str(), nullptr));
    ScopedFPDFDocument sliced(FPDF_LoadDocument(file.c_str(), nullptr));
    if (!one_go || !sliced) {
      continue;
    }
    const int count = std::min(FPDF_GetPageCount(one_go.get()), max_pages);
    for (int i = 0; i < count; ++i) {
      std::cerr << "[sliced-load] start " << file << " page " << i << "\n";
      const auto start = std::chrono::steady_clock::now();
      const Parsed expected = LoadInOneGo(one_go.get(), i);
      const auto middle = std::chrono::steady_clock::now();
      const Parsed actual = LoadInSlices(sliced.get(), i, nullptr);
      const auto end = std::chrono::steady_clock::now();
      auto ms = [](auto from, auto to) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(to - from)
            .count();
      };
      ++pages;
      if (expected.tree.find(" form matrix=") != std::string::npos) {
        ++forms_pages;
      }
      if (!expected.loaded && !actual.loaded) {
        ++unloadable_pages;
        continue;
      }
      // A page of a broken page tree may be out of reach of its object
      // number, the one way to start a load in slices.
      if (!actual.loaded &&
          !ScopedFPDFPage(EPDFDoc_LoadPageByObjectNumber(
              sliced.get(), ObjectNumberOf(sliced.get(), i)))) {
        ++unloadable_pages;
        continue;
      }
      const bool same =
          expected.loaded == actual.loaded && expected.tree == actual.tree &&
          expected.pixels == actual.pixels &&
          expected.size.estimated_bytes == actual.size.estimated_bytes;
      if (!same) {
        std::string what;
        if (expected.tree != actual.tree) {
          size_t at = 0;
          while (at < expected.tree.size() &&
                 expected.tree[at] == actual.tree[at]) {
            ++at;
          }
          const size_t line = expected.tree.rfind('\n', at) + 1;
          what +=
              " tree at: " +
              expected.tree.substr(line, expected.tree.find('\n', at) - line) +
              " | " +
              actual.tree.substr(line, actual.tree.find('\n', at) - line);
        }
        if (expected.pixels != actual.pixels) {
          what += " pixels";
        }
        if (expected.size.estimated_bytes != actual.size.estimated_bytes) {
          what += " size";
        }
        differing.push_back(file + " page " + std::to_string(i) + ":" + what);
      }
      std::cerr << "[sliced-load] " << (same ? "same " : "DIFFERS ") << file
                << " page " << i << " objects=" << expected.size.objects
                << " one go " << ms(start, middle) << " ms, sliced "
                << ms(middle, end) << " ms\n";
    }
  }
  std::cerr << "[sliced-load] " << pages << " pages, " << forms_pages
            << " with forms, " << unloadable_pages
            << " that don't load in one of the ways, " << differing.size()
            << " differ\n";
  for (const std::string& page : differing) {
    ADD_FAILURE() << "differs: " << page;
  }
}
