// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// What a parsed page costs (EPDFPage_GetParsedSize) and whether it still
// matches its document (EPDFPage_IsContentCurrent): the two things an
// embedder needs to keep parsed pages instead of loading them again.

#include <deque>
#include <string>

#include "core/fpdfapi/page/cpdf_contentparser.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/pauseindicator_iface.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_transformpage.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

std::string FormStream(const std::string& resources,
                       const std::string& content) {
  return "<< /Type /XObject /Subtype /Form /BBox [0 0 200 200] /Resources " +
         resources + " /Length " + std::to_string(content.size()) +
         " >>\nstream\n" + content + "\nendstream";
}

// One page: a filled square and its outline (the outline shares the square's
// points), a word, and a form holding a square and two placements of an inner
// form, which holds two equal squares.
//
// Objects: 4 on the page, 3 in the outer form, 2 in each inner placement.
// Points: 5 per square, shared ones once: page 5, outer 5, inner 5 + 5.
constexpr size_t kObjects = 11;
constexpr size_t kPathPoints = 20;
constexpr size_t kTextChars = 5;

std::string MakeDocument() {
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R "
      "/Resources << /XObject << /Outer 5 0 R >> /Font << /F1 7 0 R >> >> >>",
      Stream("0 0 1 rg 10 10 50 50 re f 10 10 50 50 re S "
             "BT /F1 12 Tf 20 150 Td (Hello) Tj ET /Outer Do"),
      FormStream("<< /XObject << /Inner 6 0 R >> >>",
                 "1 0 0 rg 100 100 20 20 re f /Inner Do 1 0 0 1 50 0 cm "
                 "/Inner Do"),
      FormStream("<< >>", "0 1 0 rg 0 0 5 5 re f 0 0 5 5 re f"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  });
}

// A page of `count` squares, more than one parse step holds.
std::string MakeManySquares(int count) {
  std::string content;
  for (int i = 0; i < count; ++i) {
    content += std::to_string(i % 190) + " " + std::to_string(i / 190 * 2) +
               " 1 1 re f\n";
  }
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R "
      "/Resources << >> >>",
      Stream(content),
  });
}

EPDF_PAGE_PARSED_SIZE SizeOf(FPDF_PAGE page) {
  EPDF_PAGE_PARSED_SIZE size = {};
  EXPECT_TRUE(EPDFPage_GetParsedSize(page, &size));
  return size;
}

class PauseEveryStep final : public PauseIndicatorIface {
 public:
  bool NeedToPauseNow() override { return true; }
};

class EPDFPageResidencyEmbedderTest : public EmbedderTest {
 protected:
  ScopedFPDFDocument OpenLayer(std::string pdf) {
    inputs_.push_back(std::move(pdf));
    EPDF_BASE_DOCUMENT base = EPDF_LoadMemBaseDocument(
        inputs_.back().data(), inputs_.back().size(), nullptr);
    EXPECT_TRUE(base);
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
    EPDF_ReleaseBaseDocument(base);
    return layer;
  }

  ScopedFPDFDocument OpenPlain(std::string pdf) {
    inputs_.push_back(std::move(pdf));
    return ScopedFPDFDocument(FPDF_LoadMemDocument(
        inputs_.back().data(), inputs_.back().size(), nullptr));
  }

  // Removes the page's first object and writes the page's content.
  static void RemoveFirstObjectAndGenerate(FPDF_PAGE page) {
    FPDF_PAGEOBJECT first = FPDFPage_GetObject(page, 0);
    ASSERT_TRUE(first);
    ASSERT_TRUE(FPDFPage_RemoveObject(page, first));
    FPDFPageObj_Destroy(first);
    ASSERT_TRUE(FPDFPage_GenerateContent(page));
  }

  std::deque<std::string> inputs_;
};

}  // namespace

TEST_F(EPDFPageResidencyEmbedderTest, CountsEveryObjectNestedFormsIncluded) {
  ScopedFPDFDocument doc = OpenPlain(MakeDocument());
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);

  const EPDF_PAGE_PARSED_SIZE size = SizeOf(page.get());
  EXPECT_TRUE(size.complete);
  EXPECT_EQ(kObjects, size.objects);
  EXPECT_EQ(kPathPoints, size.path_points);
  EXPECT_EQ(kTextChars, size.text_chars);
  EXPECT_GT(size.estimated_bytes, 0u);

  EPDF_PAGE_PARSED_SIZE unused;
  EXPECT_FALSE(EPDFPage_GetParsedSize(nullptr, &unused));
  EXPECT_FALSE(EPDFPage_GetParsedSize(page.get(), nullptr));
}

// The count made while parsing and the count made by walking the objects
// agree: after an edit the page counts again, and putting back what was taken
// gives the parse's count exactly.
TEST_F(EPDFPageResidencyEmbedderTest, CountsAgainAfterTheObjectsChange) {
  ScopedFPDFDocument doc = OpenPlain(MakeDocument());
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  const EPDF_PAGE_PARSED_SIZE parsed = SizeOf(page.get());

  // The word is the third object.
  FPDF_PAGEOBJECT text = FPDFPage_GetObject(page.get(), 2);
  ASSERT_EQ(FPDF_PAGEOBJ_TEXT, FPDFPageObj_GetType(text));
  ASSERT_TRUE(FPDFPage_RemoveObject(page.get(), text));
  const EPDF_PAGE_PARSED_SIZE removed = SizeOf(page.get());
  EXPECT_EQ(kObjects - 1, removed.objects);
  EXPECT_EQ(0u, removed.text_chars);
  EXPECT_EQ(kPathPoints, removed.path_points);
  EXPECT_LT(removed.estimated_bytes, parsed.estimated_bytes);

  FPDFPage_InsertObject(page.get(), text);
  const EPDF_PAGE_PARSED_SIZE restored = SizeOf(page.get());
  EXPECT_EQ(parsed.objects, restored.objects);
  EXPECT_EQ(parsed.path_points, restored.path_points);
  EXPECT_EQ(parsed.text_chars, restored.text_chars);
  EXPECT_EQ(parsed.estimated_bytes, restored.estimated_bytes);
}

// While a page parses, its size is what the parse has added so far.
TEST_F(EPDFPageResidencyEmbedderTest, CountsWhileParsing) {
  constexpr int kSquares = 250;
  ScopedFPDFDocument doc = OpenPlain(MakeManySquares(kSquares));
  CPDF_Document* document = CPDFDocumentFromFPDFDocument(doc.get());
  auto page = pdfium::MakeRetain<CPDF_Page>(
      document, document->GetMutablePageDictionary(0));
  FPDF_PAGE handle = FPDFPageFromIPDFPage(page.Get());

  EXPECT_FALSE(SizeOf(handle).complete);
  EXPECT_EQ(0u, SizeOf(handle).objects);

  page->StartParse(std::make_unique<CPDF_ContentParser>(page.Get()));
  PauseEveryStep pause;
  size_t last = 0;
  int pauses = 0;
  while (page->GetParseState() != CPDF_PageObjectHolder::ParseState::kParsed) {
    page->ContinueParse(&pause);
    const EPDF_PAGE_PARSED_SIZE size = SizeOf(handle);
    EXPECT_GE(size.objects, last);
    last = size.objects;
    if (!size.complete) {
      ++pauses;
      EXPECT_LT(size.objects, static_cast<size_t>(kSquares) + 1);
    }
  }
  EXPECT_GT(pauses, 2);
  EXPECT_EQ(static_cast<size_t>(kSquares), SizeOf(handle).objects);
  EXPECT_TRUE(SizeOf(handle).complete);
}

TEST_F(EPDFPageResidencyEmbedderTest, AnnotationWritesLeaveThePageCurrent) {
  ScopedFPDFDocument doc = OpenLayer(MakeDocument());
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  EXPECT_TRUE(EPDFPage_IsContentCurrent(page.get()));
  EXPECT_FALSE(EPDFPage_IsContentCurrent(nullptr));

  // The page gets an /Annots of its own: a new version of the page's
  // dictionary, with the same content.
  for (bool commit : {false, true}) {
    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    NewAnnot(page.get(), FPDF_ANNOT_SQUARE, {10, 190, 40, 160});
    EXPECT_TRUE(EPDFPage_IsContentCurrent(page.get()));
    ASSERT_TRUE(commit ? EPDFLayer_CommitTransaction(doc.get())
                       : EPDFLayer_AbortTransaction(doc.get()));
    EXPECT_TRUE(EPDFPage_IsContentCurrent(page.get())) << commit;
  }
}

// One handle writes the page's content; the other still holds the objects
// the content was. An abort brings that content back.
TEST_F(EPDFPageResidencyEmbedderTest,
       ContentWrittenElsewhereMakesThePageStale) {
  for (bool commit : {false, true}) {
    ScopedFPDFDocument doc = OpenLayer(MakeDocument());
    ScopedFPDFPage kept(FPDF_LoadPage(doc.get(), 0));
    ScopedFPDFPage writer(FPDF_LoadPage(doc.get(), 0));
    ASSERT_TRUE(kept);
    ASSERT_TRUE(writer);

    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    RemoveFirstObjectAndGenerate(writer.get());
    EXPECT_FALSE(EPDFPage_IsContentCurrent(kept.get()));
    EXPECT_TRUE(EPDFPage_IsContentCurrent(writer.get()));
    ASSERT_TRUE(commit ? EPDFLayer_CommitTransaction(doc.get())
                       : EPDFLayer_AbortTransaction(doc.get()));

    EXPECT_EQ(commit, !EPDFPage_IsContentCurrent(kept.get())) << commit;
    EXPECT_EQ(commit, !!EPDFPage_IsContentCurrent(writer.get())) << commit;
    ScopedFPDFPage fresh(FPDF_LoadPage(doc.get(), 0));
    EXPECT_TRUE(EPDFPage_IsContentCurrent(fresh.get()));
  }
}

// A page that writes its own content outside a transaction matches what it
// wrote; a page loaded before is not told.
TEST_F(EPDFPageResidencyEmbedderTest, WritesInPlaceAreNotSeen) {
  ScopedFPDFDocument doc = OpenPlain(MakeDocument());
  ScopedFPDFPage kept(FPDF_LoadPage(doc.get(), 0));
  ScopedFPDFPage writer(FPDF_LoadPage(doc.get(), 0));
  RemoveFirstObjectAndGenerate(writer.get());
  EXPECT_TRUE(EPDFPage_IsContentCurrent(writer.get()));
  EXPECT_TRUE(EPDFPage_IsContentCurrent(kept.get()));
}

TEST_F(EPDFPageResidencyEmbedderTest, ANestedFormWrittenMakesThePageStale) {
  ScopedFPDFDocument doc = OpenLayer(MakeDocument());
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);

  CPDF_Document* document = CPDFDocumentFromFPDFDocument(doc.get());
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  {
    CPDF_DocumentViewScope view(document);
    RetainPtr<CPDF_Object> inner = document->GetMutableIndirectObject(6);
    ASSERT_TRUE(inner && inner->AsMutableStream());
    inner->AsMutableStream()->SetData(
        ByteStringView("0 0 1 rg 0 0 9 9 re f").unsigned_span());
  }
  EXPECT_FALSE(EPDFPage_IsContentCurrent(page.get()));
  ASSERT_TRUE(EPDFLayer_AbortTransaction(doc.get()));
  EXPECT_TRUE(EPDFPage_IsContentCurrent(page.get()));
}

TEST_F(EPDFPageResidencyEmbedderTest, BoxesAndRotationAreContentToo) {
  ScopedFPDFDocument doc = OpenLayer(MakeDocument());
  ScopedFPDFPage kept(FPDF_LoadPage(doc.get(), 0));
  ScopedFPDFPage writer(FPDF_LoadPage(doc.get(), 0));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  FPDFPage_SetRotation(writer.get(), 1);
  EXPECT_FALSE(EPDFPage_IsContentCurrent(kept.get()));
  EXPECT_TRUE(EPDFPage_IsContentCurrent(writer.get()));
  ASSERT_TRUE(EPDFLayer_AbortTransaction(doc.get()));
  EXPECT_TRUE(EPDFPage_IsContentCurrent(kept.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  FPDFPage_SetCropBox(writer.get(), 0, 0, 100, 100);
  EXPECT_FALSE(EPDFPage_IsContentCurrent(kept.get()));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  EXPECT_FALSE(EPDFPage_IsContentCurrent(kept.get()));
}

// An object added to a page of one content stream goes into a stream of its
// own: /Contents becomes an array, which the kept page never read.
TEST_F(EPDFPageResidencyEmbedderTest, AContentStreamAddedMakesThePageStale) {
  ScopedFPDFDocument doc = OpenLayer(MakeDocument());
  ScopedFPDFPage kept(FPDF_LoadPage(doc.get(), 0));
  ScopedFPDFPage writer(FPDF_LoadPage(doc.get(), 0));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  FPDF_PAGEOBJECT square = FPDFPageObj_CreateNewRect(150, 10, 20, 20);
  ASSERT_TRUE(FPDFPath_SetDrawMode(square, FPDF_FILLMODE_WINDING, false));
  FPDFPage_InsertObject(writer.get(), square);
  ASSERT_TRUE(FPDFPage_GenerateContent(writer.get()));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));

  EXPECT_FALSE(EPDFPage_IsContentCurrent(kept.get()));
  EXPECT_TRUE(EPDFPage_IsContentCurrent(writer.get()));
  EXPECT_EQ(kObjects + 1, SizeOf(writer.get()).objects);
}

// A page that no longer matches writes content from its old objects: the
// streams it wrote match them, the ones it didn't still don't.
TEST_F(EPDFPageResidencyEmbedderTest, AStalePageThatWritesStaysStale) {
  ScopedFPDFDocument doc = OpenLayer(MakeDocument());
  ScopedFPDFPage stale(FPDF_LoadPage(doc.get(), 0));
  ScopedFPDFPage writer(FPDF_LoadPage(doc.get(), 0));
  CPDF_Document* document = CPDFDocumentFromFPDFDocument(doc.get());

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  {
    CPDF_DocumentViewScope view(document);
    RetainPtr<CPDF_Object> inner = document->GetMutableIndirectObject(6);
    ASSERT_TRUE(inner && inner->AsMutableStream());
    inner->AsMutableStream()->SetData(
        ByteStringView("0 0 1 rg 0 0 9 9 re f").unsigned_span());
  }
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  ASSERT_FALSE(EPDFPage_IsContentCurrent(stale.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  RemoveFirstObjectAndGenerate(stale.get());
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  EXPECT_FALSE(EPDFPage_IsContentCurrent(stale.get()));
  EXPECT_FALSE(EPDFPage_IsContentCurrent(writer.get()));
}

// Content in two streams. Another handle rewrote the second; the stale page
// rewrites only the first, and its objects of the second are still old.
TEST_F(EPDFPageResidencyEmbedderTest, AStalePageWritingOneStreamStaysStale) {
  ScopedFPDFDocument doc = OpenLayer(MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
      "/Contents [4 0 R 5 0 R] /Resources << >> >>",
      Stream("0 0 1 rg 10 10 50 50 re f 70 10 50 50 re f"),
      Stream("1 0 0 rg 100 100 20 20 re f 150 150 20 20 re f"),
  }));
  ScopedFPDFPage stale(FPDF_LoadPage(doc.get(), 0));
  ScopedFPDFPage writer(FPDF_LoadPage(doc.get(), 0));
  ASSERT_EQ(4, FPDFPage_CountObjects(writer.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  FPDF_PAGEOBJECT last = FPDFPage_GetObject(writer.get(), 3);
  ASSERT_TRUE(FPDFPage_RemoveObject(writer.get(), last));
  FPDFPageObj_Destroy(last);
  ASSERT_TRUE(FPDFPage_GenerateContent(writer.get()));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  ASSERT_FALSE(EPDFPage_IsContentCurrent(stale.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  RemoveFirstObjectAndGenerate(stale.get());
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  EXPECT_FALSE(EPDFPage_IsContentCurrent(stale.get()));
}
