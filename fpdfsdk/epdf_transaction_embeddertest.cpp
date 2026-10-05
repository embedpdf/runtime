// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Layer transactions through the public API (EPDFLayer_BeginTransaction and
// friends): a mixed batch of annotation writes, on pages and with handles
// loaded before the transaction, aborted after every write or committed.
// An abort brings back the layer as it was at begin - apart from the object
// numbers the transaction handed out, which are never given back, so saves
// may carry a longer xref table and a larger trailer /Size. A commit gives
// exactly what the same writes give without a transaction.

#include <cctype>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_checkpoint.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_attachment.h"
#include "public/fpdf_doc.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

constexpr uint32_t kRed = 0xff0000;
constexpr uint32_t kBlue = 0x0000ff;
constexpr uint32_t kWhite = 0xffffff;

// Where the batch stamps a PNG on the first page, and the middle of it.
constexpr FS_RECTF kStampRect = {20, 200, 120, 150};
constexpr int kStampX = 70;
constexpr int kStampY = 175;

std::string FormStream(const std::string& bbox, const std::string& content) {
  return "<< /Type /XObject /Subtype /Form /BBox [" + bbox + "] /Length " +
         std::to_string(content.size()) + " >>\nstream\n" + content +
         "\nendstream";
}

// Two pages. The first has an indirect /Annots holding one red square with an
// appearance (object 7). The second has a direct /Annots holding two inline
// annotations: a blue square with an appearance, and a note.
std::string MakeDocument() {
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 5 0 R "
      "/Resources << >> /Annots 6 0 R >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 5 0 R "
      "/Resources << >> /Annots ["
      "<< /Type /Annot /Subtype /Square /Rect [20 20 80 80] /F 4 "
      "/Contents (inline one) /AP << /N 8 0 R >> >> "
      "<< /Type /Annot /Subtype /Text /Rect [100 100 120 120] "
      "/Contents (inline two) >> ] >>",
      Stream("0 1 0 rg 10 10 30 30 re f"),
      "[7 0 R]",
      "<< /Type /Annot /Subtype /Square /Rect [240 240 290 290] /F 4 "
      "/C [1 0 0] /P 3 0 R /AP << /N 9 0 R >> >>",
      FormStream("0 0 60 60", "0 0 1 rg 0 0 60 60 re f"),
      FormStream("0 0 50 50", "1 0 0 rg 0 0 50 50 re f"),
  });
}

// A save without what rule 7 lets an aborted transaction leave behind: the
// numbers it handed out make the xref table longer and the trailer /Size
// larger. The bodies, and so every offset, are unaffected.
std::string WithoutNumbering(std::string bytes) {
  bytes = WithoutFileId(std::move(bytes));
  std::string out;
  size_t at = 0;
  for (;;) {
    const size_t xref = bytes.find("\nxref\n", at);
    const size_t trailer =
        xref == std::string::npos ? xref : bytes.find("trailer", xref);
    if (trailer == std::string::npos) {
      break;
    }
    out.append(bytes, at, xref + 6 - at);
    at = trailer;
  }
  out.append(bytes, at, std::string::npos);
  for (size_t size = out.find("/Size "); size != std::string::npos;
       size = out.find("/Size ", size + 1)) {
    size_t end = size + 6;
    while (end < out.size() && isdigit(static_cast<unsigned char>(out[end]))) {
      ++end;
    }
    out.replace(size + 6, end - size - 6, "N");
  }
  return out;
}

CPDF_LayerDocument* LayerOf(FPDF_DOCUMENT doc) {
  return CPDF_LayerDocument::FromDocument(CPDFDocumentFromFPDFDocument(doc));
}

std::string Contents(FPDF_ANNOTATION annot) {
  return AnnotString(annot, "Contents");
}

// What one batch works with: pages loaded and an inline annotation handle
// acquired before the transaction, and what earlier steps made.
struct Batch {
  FPDF_DOCUMENT doc = nullptr;
  FPDF_PAGE first = nullptr;
  FPDF_PAGE second = nullptr;
  FPDF_ANNOTATION inline_square = nullptr;
  ScopedFPDFAnnotation square;
};

struct Step {
  const char* name;
  std::function<void(Batch&)> run;
};

// Edits made earlier in the session, outside any transaction, so the batch
// writes over committed layer versions and not only over the base: the base
// square gets /Contents, the first page gets a note (its /Annots array is
// promoted), the inline square gets an author (its page is promoted).
void MakeEarlierEdits(FPDF_PAGE first, FPDF_ANNOTATION inline_square) {
  ScopedFPDFWideString earlier = GetFPDFWideString(L"earlier");
  {
    ScopedFPDFAnnotation base_square(FPDFPage_GetAnnot(first, 0));
    ASSERT_TRUE(base_square);
    ASSERT_TRUE(
        FPDFAnnot_SetStringValue(base_square.get(), "Contents", earlier.get()));
  }
  NewAnnot(first, FPDF_ANNOT_TEXT, {250, 200, 270, 180});
  ASSERT_TRUE(FPDFAnnot_SetStringValue(inline_square, "T", earlier.get()));
}

// The mixed batch: every kind of annotation write an action makes.
const std::vector<Step>& MixedBatch() {
  static const std::vector<Step>* const steps = new std::vector<Step>{
      {"create a square",
       [](Batch& b) {
         b.square = NewAnnot(b.first, FPDF_ANNOT_SQUARE, {150, 280, 200, 230});
         ASSERT_TRUE(EPDFAnnot_GenerateAppearance(b.square.get()));
       }},
      {"update an inline annotation through a handle from before begin",
       [](Batch& b) {
         ScopedFPDFWideString text = GetFPDFWideString(L"changed");
         ScopedFPDFWideString author = GetFPDFWideString(L"Bob");
         ASSERT_TRUE(
             FPDFAnnot_SetStringValue(b.inline_square, "Contents", text.get()));
         ASSERT_TRUE(
             FPDFAnnot_SetStringValue(b.inline_square, "T", author.get()));
       }},
      {"delete the base annotation",
       [](Batch& b) { ASSERT_TRUE(EPDFPage_RemoveAnnot(b.first, 0)); }},
      {"stamp a PNG",
       [](Batch& b) { AddPngStamp(b.doc, b.first, kRedPng, kStampRect); }},
      {"render",
       [](Batch& b) { EXPECT_EQ(kRed, ColorAt(b.first, kStampX, kStampY)); }},
      {"reply to the new square",
       [](Batch& b) {
         ScopedFPDFAnnotation reply =
             NewAnnot(b.first, FPDF_ANNOT_TEXT, {210, 280, 230, 260});
         ASSERT_TRUE(
             EPDFAnnot_SetLinkedAnnot(reply.get(), "IRT", b.square.get()));
         ASSERT_TRUE(EPDFAnnot_SetReplyType(reply.get(), FPDF_ANNOT_RT_REPLY));
       }},
      {"create and delete a note on the inline page",
       [](Batch& b) {
         NewAnnot(b.second, FPDF_ANNOT_TEXT, {200, 280, 220, 260});
         ASSERT_TRUE(EPDFPage_RemoveAnnot(
             b.second, FPDFPage_GetAnnotCount(b.second) - 1));
       }},
  };
  return *steps;
}

}  // namespace

class EPDFTransactionEmbedderTest : public EmbedderTest {
 protected:
  // A fresh layer over the fixture.
  ScopedFPDFDocument Open() {
    input_ = MakeDocument();
    EPDF_BASE_DOCUMENT base =
        EPDF_LoadMemBaseDocument(input_.data(), input_.size(), nullptr);
    EXPECT_TRUE(base);
    ScopedFPDFDocument opened(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
    EPDF_ReleaseBaseDocument(base);
    return opened;
  }

  // What the layer is, and what its saves write.
  struct State {
    std::map<uint32_t, std::string> graph;
    uint32_t last_number = 0;
    std::string full_save;
    std::string delta_save;
    bool changed_since_load = false;
    unsigned long promoted = 0;
  };

  State Capture(FPDF_DOCUMENT doc) {
    State state;
    ClearString();
    EXPECT_TRUE(FPDF_SaveAsCopy(doc, this, FPDF_NO_INCREMENTAL));
    state.full_save = GetString();
    ClearString();
    FPDF_BOOL changed = false;
    EXPECT_TRUE(EPDFLayer_SaveDeltaEx(doc, this, nullptr, &changed));
    state.delta_save = GetString();
    state.changed_since_load = changed;
    state.promoted = EPDFLayer_GetPromotedObjectCount(doc);
    CPDF_Document* document = CPDFDocumentFromFPDFDocument(doc);
    state.graph = ObjectGraph(document);
    state.last_number = document->GetLastObjNum();
    return state;
  }

  static std::string DifferingObjects(const State& before, const State& after) {
    std::string differing;
    for (const auto& [number, text] : after.graph) {
      auto it = before.graph.find(number);
      if (it == before.graph.end() || it->second != text) {
        differing += " " + std::to_string(number);
      }
    }
    for (const auto& [number, text] : before.graph) {
      if (!after.graph.count(number)) {
        differing += " " + std::to_string(number);
      }
    }
    return differing;
  }

  // The layer as at begin, apart from the numbers handed out meanwhile.
  void ExpectAsAtBegin(const State& before, const State& after) {
    EXPECT_EQ("", DifferingObjects(before, after)) << "objects that differ";
    EXPECT_LE(before.last_number, after.last_number);
    EXPECT_EQ(WithoutNumbering(before.full_save),
              WithoutNumbering(after.full_save));
    EXPECT_EQ(WithoutNumbering(before.delta_save),
              WithoutNumbering(after.delta_save));
    EXPECT_EQ(before.changed_since_load, after.changed_since_load);
    EXPECT_EQ(before.promoted, after.promoted);
  }

  // Exactly the same layer.
  void ExpectSame(const State& expected, const State& actual) {
    EXPECT_EQ("", DifferingObjects(expected, actual)) << "objects that differ";
    EXPECT_EQ(expected.last_number, actual.last_number);
    EXPECT_EQ(WithoutFileId(expected.full_save),
              WithoutFileId(actual.full_save));
    EXPECT_EQ(WithoutFileId(expected.delta_save),
              WithoutFileId(actual.delta_save));
    EXPECT_EQ(expected.changed_since_load, actual.changed_since_load);
    EXPECT_EQ(expected.promoted, actual.promoted);
  }

  std::string input_;
};

TEST_F(EPDFTransactionEmbedderTest, OnlyOnALayerAndOneAtATime) {
  EXPECT_FALSE(EPDFLayer_BeginTransaction(nullptr));
  EXPECT_FALSE(EPDFLayer_IsInTransaction(nullptr));
  const std::string plain_bytes = MakeDocument();
  ScopedFPDFDocument plain(
      FPDF_LoadMemDocument(plain_bytes.data(), plain_bytes.size(), nullptr));
  ASSERT_TRUE(plain);
  EXPECT_FALSE(EPDFLayer_BeginTransaction(plain.get()));

  ScopedFPDFDocument doc = Open();
  ASSERT_TRUE(doc);
  EXPECT_FALSE(EPDFLayer_CommitTransaction(doc.get()));
  EXPECT_FALSE(EPDFLayer_AbortTransaction(doc.get()));
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  EXPECT_TRUE(EPDFLayer_IsInTransaction(doc.get()));
  EXPECT_FALSE(EPDFLayer_BeginTransaction(doc.get()));
  EXPECT_TRUE(EPDFLayer_AbortTransaction(doc.get()));
  EXPECT_FALSE(EPDFLayer_IsInTransaction(doc.get()));
}

// Saving is a post-commit operation: a save inside a transaction would write
// state that may still be dropped.
TEST_F(EPDFTransactionEmbedderTest, SavesAreRefusedInsideATransaction) {
  ScopedFPDFDocument doc = Open();
  ASSERT_TRUE(doc);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  EXPECT_FALSE(EPDFLayer_SaveDeltaEx(doc.get(), this, nullptr, nullptr));
  EXPECT_FALSE(FPDF_SaveAsCopy(doc.get(), this, FPDF_NO_INCREMENTAL));
  EXPECT_TRUE(EPDFLayer_AbortTransaction(doc.get()));
  EXPECT_TRUE(EPDFLayer_SaveDeltaEx(doc.get(), this, nullptr, nullptr));
}

// §4.15: what a transaction rules out while it is open - a checkpoint, a
// second layer opened on this thread - and a checkpoint rules out a
// transaction.
TEST_F(EPDFTransactionEmbedderTest, RefusesWhatOverlapsATransaction) {
  ScopedFPDFDocument doc = Open();
  ASSERT_TRUE(doc);

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  EXPECT_FALSE(EPDFDoc_BeginCheckpoint(doc.get()));
  EPDF_BASE_DOCUMENT base =
      EPDF_LoadMemBaseDocument(input_.data(), input_.size(), nullptr);
  ASSERT_TRUE(base);
  EXPECT_FALSE(EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
  ASSERT_TRUE(EPDFLayer_AbortTransaction(doc.get()));
  ScopedFPDFDocument second(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
  EXPECT_TRUE(second);
  EPDF_ReleaseBaseDocument(base);

  EPDF_CHECKPOINT checkpoint = EPDFDoc_BeginCheckpoint(doc.get());
  ASSERT_TRUE(checkpoint);
  EXPECT_FALSE(EPDFLayer_BeginTransaction(doc.get()));
  EPDFDoc_EndCheckpoint(checkpoint);
  EXPECT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  EXPECT_TRUE(EPDFLayer_AbortTransaction(doc.get()));
}

// G1 + G2: the mixed batch fails after each of its writes, on a fresh layer
// and on one with earlier edits. After the abort, the layer is as at begin,
// and the pages and the inline handle loaded before begin show it.
TEST_F(EPDFTransactionEmbedderTest, FailureAfterEveryWriteAbortsCleanly) {
  const std::vector<Step>& steps = MixedBatch();
  for (bool earlier_edits : {false, true}) {
    for (size_t failing = 1; failing <= steps.size(); ++failing) {
      SCOPED_TRACE(std::string(earlier_edits ? "after earlier edits, "
                                             : "fresh layer, ") +
                   "fails after: " + steps[failing - 1].name);
      ScopedFPDFDocument doc = Open();
      ASSERT_TRUE(doc);
      ScopedFPDFPage first(FPDF_LoadPage(doc.get(), 0));
      ScopedFPDFPage second(FPDF_LoadPage(doc.get(), 1));
      ASSERT_TRUE(first);
      ASSERT_TRUE(second);
      ScopedFPDFAnnotation inline_square(FPDFPage_GetAnnot(second.get(), 0));
      ASSERT_TRUE(inline_square);
      if (earlier_edits) {
        MakeEarlierEdits(first.get(), inline_square.get());
      }
      const int counts[] = {FPDFPage_GetAnnotCount(first.get()),
                            FPDFPage_GetAnnotCount(second.get())};
      // Warm the page caches with the committed state.
      EXPECT_EQ(kRed, ColorAt(first.get(), 265, 265));
      EXPECT_EQ(kBlue, ColorAt(second.get(), 50, 50));
      const State before = Capture(doc.get());

      Batch batch{
          doc.get(), first.get(), second.get(), inline_square.get(), {}};
      ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
      for (size_t i = 0; i < failing; ++i) {
        steps[i].run(batch);
      }
      batch.square.reset();
      EXPECT_TRUE(EPDFLayer_AbortTransaction(doc.get()));

      ExpectAsAtBegin(before, Capture(doc.get()));
      EXPECT_EQ(counts[0], FPDFPage_GetAnnotCount(first.get()));
      EXPECT_EQ(counts[1], FPDFPage_GetAnnotCount(second.get()));
      EXPECT_EQ("inline one", Contents(inline_square.get()));
      EXPECT_EQ(kRed, ColorAt(first.get(), 265, 265));
      EXPECT_EQ(kWhite, ColorAt(first.get(), kStampX, kStampY));
      EXPECT_EQ(kBlue, ColorAt(second.get(), 50, 50));
    }
  }
}

// G5: a committed batch is exactly what the same writes make without a
// transaction, in memory and in both saves; and the pages loaded before begin
// show it.
TEST_F(EPDFTransactionEmbedderTest, CommitEqualsTheSameWritesWithout) {
  for (bool earlier_edits : {false, true}) {
    SCOPED_TRACE(earlier_edits ? "after earlier edits" : "fresh layer");
    State results[2];
    for (bool transaction : {false, true}) {
      SCOPED_TRACE(transaction ? "in a transaction" : "without");
      ScopedFPDFDocument doc = Open();
      ASSERT_TRUE(doc);
      ScopedFPDFPage first(FPDF_LoadPage(doc.get(), 0));
      ScopedFPDFPage second(FPDF_LoadPage(doc.get(), 1));
      ScopedFPDFAnnotation inline_square(FPDFPage_GetAnnot(second.get(), 0));
      ASSERT_TRUE(inline_square);
      if (earlier_edits) {
        MakeEarlierEdits(first.get(), inline_square.get());
      }
      const int count = FPDFPage_GetAnnotCount(first.get());
      EXPECT_EQ(kRed, ColorAt(first.get(), 265, 265));

      Batch batch{
          doc.get(), first.get(), second.get(), inline_square.get(), {}};
      if (transaction) {
        ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
      }
      for (const Step& step : MixedBatch()) {
        step.run(batch);
      }
      batch.square.reset();
      if (transaction) {
        ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
      }

      results[transaction] = Capture(doc.get());
      // The square, the stamp and the reply; the base square is gone.
      EXPECT_EQ(count + 2, FPDFPage_GetAnnotCount(first.get()));
      EXPECT_EQ(2, FPDFPage_GetAnnotCount(second.get()));
      EXPECT_EQ("changed", Contents(inline_square.get()));
      EXPECT_EQ(kWhite, ColorAt(first.get(), 265, 265));
      EXPECT_EQ(kRed, ColorAt(first.get(), kStampX, kStampY));
    }
    ExpectSame(results[false], results[true]);
  }
}

// A stamp made, rendered and deleted inside a transaction that is then
// aborted leaves nothing behind for the page caches to show: stamps made
// afterwards show their own colour.
TEST_F(EPDFTransactionEmbedderTest, CreateRenderDeleteAbort) {
  ScopedFPDFDocument doc = Open();
  ASSERT_TRUE(doc);
  ScopedFPDFPage first(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(first);
  const State before = Capture(doc.get());

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  AddPngStamp(doc.get(), first.get(), kRedPng, kStampRect);
  EXPECT_EQ(kRed, ColorAt(first.get(), kStampX, kStampY));
  ASSERT_TRUE(EPDFPage_RemoveAnnot(first.get(), 1));
  EXPECT_EQ(kWhite, ColorAt(first.get(), kStampX, kStampY));
  EXPECT_TRUE(EPDFLayer_AbortTransaction(doc.get()));
  ExpectAsAtBegin(before, Capture(doc.get()));

  // Rendered, then aborted with the stamp still there.
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  AddPngStamp(doc.get(), first.get(), kRedPng, kStampRect);
  EXPECT_EQ(kRed, ColorAt(first.get(), kStampX, kStampY));
  EXPECT_TRUE(EPDFLayer_AbortTransaction(doc.get()));
  EXPECT_EQ(kWhite, ColorAt(first.get(), kStampX, kStampY));
  ExpectAsAtBegin(before, Capture(doc.get()));

  // A stamp made afterwards, committed and not.
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  AddPngStamp(doc.get(), first.get(), kBluePng, kStampRect);
  EXPECT_EQ(kBlue, ColorAt(first.get(), kStampX, kStampY));
  EXPECT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  EXPECT_EQ(kBlue, ColorAt(first.get(), kStampX, kStampY));
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(first.get()));
}

// G10 through the public API: pages added and deleted inside a transaction
// are back after an abort, and a page loaded before begin still renders.
TEST_F(EPDFTransactionEmbedderTest, PageInsertAndDeleteAbort) {
  ScopedFPDFDocument doc = Open();
  ASSERT_TRUE(doc);
  ScopedFPDFPage second(FPDF_LoadPage(doc.get(), 1));
  ASSERT_TRUE(second);
  const State before = Capture(doc.get());

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  ScopedFPDFPage added(FPDFPage_New(doc.get(), 2, 300, 300));
  ASSERT_TRUE(added);
  EXPECT_EQ(3, FPDF_GetPageCount(doc.get()));
  FPDFPage_Delete(doc.get(), 0);
  EXPECT_EQ(2, FPDF_GetPageCount(doc.get()));
  added.reset();
  EXPECT_TRUE(EPDFLayer_AbortTransaction(doc.get()));

  EXPECT_EQ(2, FPDF_GetPageCount(doc.get()));
  ExpectAsAtBegin(before, Capture(doc.get()));
  EXPECT_EQ(kBlue, ColorAt(second.get(), 50, 50));
  ScopedFPDFPage first(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(first);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(first.get()));
}

// Writes to a committed annotation's appearance stream - its /BBox (a larger
// /Rect) or its content (a new page object) - copy the stream up first: an
// abort leaves the committed stream exactly as it was.
TEST_F(EPDFTransactionEmbedderTest, AppearanceWritesCopyTheStreamUp) {
  for (bool commit : {false, true}) {
    SCOPED_TRACE(commit ? "commit" : "abort");
    ScopedFPDFDocument doc = Open();
    ASSERT_TRUE(doc);
    ScopedFPDFPage first(FPDF_LoadPage(doc.get(), 0));
    ASSERT_TRUE(first);
    AddPngStamp(doc.get(), first.get(), kRedPng, kStampRect);  // committed
    const State before = Capture(doc.get());
    ScopedFPDFAnnotation stamp(FPDFPage_GetAnnot(
        first.get(), FPDFPage_GetAnnotCount(first.get()) - 1));
    ASSERT_EQ(1, FPDFAnnot_GetObjectCount(stamp.get()));

    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    const FS_RECTF bigger = {0, 300, 300, 0};
    ASSERT_TRUE(FPDFAnnot_SetRect(stamp.get(), &bigger));
    FPDF_PAGEOBJECT square = FPDFPageObj_CreateNewRect(30, 160, 20, 20);
    ASSERT_TRUE(FPDFPageObj_SetFillColor(square, 0, 0, 255, 255));
    ASSERT_TRUE(FPDFPath_SetDrawMode(square, FPDF_FILLMODE_ALTERNATE, false));
    ASSERT_TRUE(FPDFAnnot_AppendObject(stamp.get(), square));
    ASSERT_TRUE(commit ? EPDFLayer_CommitTransaction(doc.get())
                       : EPDFLayer_AbortTransaction(doc.get()));

    if (commit) {
      EXPECT_EQ(2, FPDFAnnot_GetObjectCount(stamp.get()));
      FS_RECTF rect;
      ASSERT_TRUE(FPDFAnnot_GetRect(stamp.get(), &rect));
      EXPECT_EQ(300.0f, rect.right);
    } else {
      ExpectAsAtBegin(before, Capture(doc.get()));
      EXPECT_EQ(1, FPDFAnnot_GetObjectCount(stamp.get()));
    }
  }
}

// An attachment read before a transaction belongs to a committed version:
// inside the transaction the attachment writers refuse it, and the handle
// EPDFAnnot_GetFileAttachmentForWrite() gives copies it up first.
TEST_F(EPDFTransactionEmbedderTest, AttachmentWritesNeedAWriteHandle) {
  auto description = [](FPDF_ATTACHMENT attachment) {
    unsigned long length =
        EPDFAttachment_GetDescription(attachment, nullptr, 0);
    std::vector<FPDF_WCHAR> buffer(length / sizeof(FPDF_WCHAR) + 1);
    EPDFAttachment_GetDescription(attachment, buffer.data(), length);
    return GetPlatformString(buffer.data());
  };
  for (bool commit : {false, true}) {
    SCOPED_TRACE(commit ? "commit" : "abort");
    ScopedFPDFDocument doc = Open();
    ASSERT_TRUE(doc);
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
    {
      ScopedFPDFAnnotation annot =
          NewAnnot(page.get(), FPDF_ANNOT_FILEATTACHMENT, {150, 200, 170, 180});
      ScopedFPDFWideString name = GetFPDFWideString(L"notes.txt");
      FPDF_ATTACHMENT file =
          FPDFAnnot_AddFileAttachment(annot.get(), name.get());
      ASSERT_TRUE(file);
      ASSERT_TRUE(FPDFAttachment_SetFile(file, doc.get(), "hello", 5));
      ScopedFPDFWideString desc = GetFPDFWideString(L"before");
      ASSERT_TRUE(EPDFAttachment_SetDescription(file, desc.get()));
    }
    ScopedFPDFAnnotation annot(
        FPDFPage_GetAnnot(page.get(), FPDFPage_GetAnnotCount(page.get()) - 1));
    FPDF_ATTACHMENT read = FPDFAnnot_GetFileAttachment(annot.get());
    ASSERT_EQ("before", description(read));

    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    ScopedFPDFWideString during = GetFPDFWideString(L"during");
    EXPECT_FALSE(EPDFAttachment_SetDescription(read, during.get()));
    FPDF_ATTACHMENT write = EPDFAnnot_GetFileAttachmentForWrite(annot.get());
    ASSERT_TRUE(write);
    EXPECT_TRUE(EPDFAttachment_SetDescription(write, during.get()));
    EXPECT_TRUE(FPDFAttachment_SetFile(write, doc.get(), "world", 5));
    ASSERT_TRUE(commit ? EPDFLayer_CommitTransaction(doc.get())
                       : EPDFLayer_AbortTransaction(doc.get()));

    FPDF_ATTACHMENT after = FPDFAnnot_GetFileAttachment(annot.get());
    EXPECT_EQ(commit ? "during" : "before", description(after));
    unsigned long size = 0;
    ASSERT_TRUE(FPDFAttachment_GetFile(after, nullptr, 0, &size));
    std::string bytes(size, '\0');
    ASSERT_TRUE(FPDFAttachment_GetFile(after, bytes.data(), size, &size));
    EXPECT_EQ(commit ? "world" : "hello", bytes);
  }
}

// Bookmarks: a handle read before a transaction belongs to a committed
// version. Writers that take the document write the document's version of
// it (a copy, in the transaction); those that don't refuse it.
TEST_F(EPDFTransactionEmbedderTest, BookmarkWritesGoThroughTheDoor) {
  auto title = [](FPDF_BOOKMARK bookmark) {
    unsigned long length = FPDFBookmark_GetTitle(bookmark, nullptr, 0);
    std::vector<FPDF_WCHAR> buffer(length / sizeof(FPDF_WCHAR) + 1);
    FPDFBookmark_GetTitle(bookmark, buffer.data(), length);
    return GetPlatformString(buffer.data());
  };
  auto children = [](FPDF_DOCUMENT doc, FPDF_BOOKMARK parent) {
    int count = 0;
    for (FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, parent); child;
         child = FPDFBookmark_GetNextSibling(doc, child)) {
      ++count;
    }
    return count;
  };
  for (bool commit : {false, true}) {
    SCOPED_TRACE(commit ? "commit" : "abort");
    ScopedFPDFDocument doc = Open();
    ASSERT_TRUE(doc);
    ScopedFPDFWideString chapter = GetFPDFWideString(L"Chapter");
    ASSERT_TRUE(EPDFBookmark_AppendChild(doc.get(), nullptr, chapter.get()));
    FPDF_BOOKMARK read = FPDFBookmark_GetFirstChild(doc.get(), nullptr);
    ASSERT_EQ("Chapter", title(read));

    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    ScopedFPDFWideString renamed = GetFPDFWideString(L"Renamed");
    EXPECT_FALSE(EPDFBookmark_SetTitle(read, renamed.get()));  // no document
    ScopedFPDFWideString section = GetFPDFWideString(L"Section");
    EXPECT_TRUE(EPDFBookmark_AppendChild(doc.get(), read, section.get()));
    EXPECT_EQ(
        1, children(doc.get(), FPDFBookmark_GetFirstChild(doc.get(), nullptr)));
    ASSERT_TRUE(commit ? EPDFLayer_CommitTransaction(doc.get())
                       : EPDFLayer_AbortTransaction(doc.get()));

    FPDF_BOOKMARK after = FPDFBookmark_GetFirstChild(doc.get(), nullptr);
    EXPECT_EQ("Chapter", title(after));
    EXPECT_EQ(commit ? 1 : 0, children(doc.get(), after));
  }
}

// The content generator caches the resource names it gave graphics states.
// An abort drops the names a transaction added, so a page that generates its
// content again must not reuse one: a half-transparent rect would render
// opaque under a graphics state nothing defines.
TEST_F(EPDFTransactionEmbedderTest, ResourceNamesDroppedByAnAbortAreNotReused) {
  ScopedFPDFDocument doc = Open();
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  FPDF_PAGEOBJECT rect = FPDFPageObj_CreateNewRect(150, 150, 50, 50);
  ASSERT_TRUE(FPDFPageObj_SetFillColor(rect, 0, 0, 255, 128));
  ASSERT_TRUE(FPDFPath_SetDrawMode(rect, FPDF_FILLMODE_ALTERNATE, false));
  FPDFPage_InsertObject(page.get(), rect);
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  ASSERT_TRUE(EPDFLayer_AbortTransaction(doc.get()));

  // The page still holds the rect it parsed; generating again must define
  // the graphics state it names. A page loaded afresh draws what the
  // content says, not what the old page holds in memory.
  FPDFPageObj_Transform(rect, 1, 0, 0, 1, 0, 0);  // marks it for writing
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  ScopedFPDFPage fresh(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(fresh);
  const uint32_t color = ColorAt(fresh.get(), 175, 175);
  const int red = (color >> 16) & 0xff;
  EXPECT_GT(red, 100) << "opaque: the graphics state is undefined";
  EXPECT_LT(red, 160);
}

// A stamp whose appearance has no /Resources of its own borrows its page's.
// Adding a graphics state gives the appearance resources of its own, a copy
// of what it borrowed: the page's own resources, frozen in the base or
// committed, are never written - in a transaction or outside one.
TEST_F(EPDFTransactionEmbedderTest, FormWithoutResourcesGetsItsOwn) {
  input_ = MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
      "/Resources << /ProcSet [/PDF] >> /Annots [4 0 R] >>",
      "<< /Type /Annot /Subtype /Stamp /Rect [50 50 250 250] "
      "/AP << /N 5 0 R >> >>",
      "<< /Type /XObject /Subtype /Form /BBox [0 0 200 200] /Length 25 >>\n"
      "stream\n0 1 0 rg 0 0 99 99 re f\nendstream",
  });
  for (bool transaction : {false, true}) {
    SCOPED_TRACE(transaction ? "in a transaction" : "outside one");
    EPDF_BASE_DOCUMENT base =
        EPDF_LoadMemBaseDocument(input_.data(), input_.size(), nullptr);
    ASSERT_TRUE(base);
    ScopedFPDFDocument doc(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
    EPDF_ReleaseBaseDocument(base);
    ASSERT_TRUE(doc);
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
    ScopedFPDFAnnotation stamp(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(stamp);

    if (transaction) {
      ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    }
    FPDF_PAGEOBJECT rect = FPDFPageObj_CreateNewRect(120, 120, 40, 40);
    ASSERT_TRUE(FPDFPageObj_SetFillColor(rect, 0, 0, 255, 128));
    ASSERT_TRUE(FPDFPath_SetDrawMode(rect, FPDF_FILLMODE_ALTERNATE, false));
    ASSERT_TRUE(FPDFAnnot_AppendObject(stamp.get(), rect));
    if (transaction) {
      ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
    }

    RetainPtr<CPDF_Stream> ap = GetAnnotAP(
        CPDFAnnotContextFromFPDFAnnotation(stamp.get())->GetAnnotDict(),
        CPDF_Annot::AppearanceMode::kNormal);
    ASSERT_TRUE(ap);
    RetainPtr<const CPDF_Dictionary> own =
        ap->GetDict()->GetDictFor("Resources");
    ASSERT_TRUE(own);
    EXPECT_TRUE(own->GetDictFor("ExtGState"));
    RetainPtr<const CPDF_Dictionary> page_resources =
        CPDFPageFromFPDFPage(page.get())->GetDict()->GetDictFor("Resources");
    EXPECT_FALSE(page_resources->KeyExist("ExtGState"));
  }
}

// G9: an image stream changed inside a transaction renders its new bytes
// inside it, its old bytes after an abort and its new bytes after a commit,
// on a page loaded before begin. The page's parsed image keeps one
// CPDF_Image, which must follow the stream's version (rule 5).
TEST_F(EPDFTransactionEmbedderTest, ImageStreamFollowsTheTransaction) {
  // One page drawing image 5, a 1 x 1 red pixel, over most of the page.
  input_ = MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R "
      "/Resources << /XObject << /Im0 5 0 R >> >> >>",
      Stream("q 200 0 0 200 50 50 cm /Im0 Do Q"),
      "<< /Type /XObject /Subtype /Image /Width 1 /Height 1 "
      "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 3 >>\nstream\n" +
          std::string("\xff\x00\x00", 3) + "\nendstream",
  });
  EPDF_BASE_DOCUMENT base =
      EPDF_LoadMemBaseDocument(input_.data(), input_.size(), nullptr);
  ASSERT_TRUE(base);
  ScopedFPDFDocument doc(EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
  EPDF_ReleaseBaseDocument(base);
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_EQ(kRed, ColorAt(page.get(), 150, 150));

  auto paint_blue = [&] {
    RetainPtr<CPDF_Stream> image = ToStream(
        CPDFDocumentFromFPDFDocument(doc.get())->GetMutableIndirectObject(5));
    ASSERT_TRUE(image);
    const uint8_t blue[] = {0x00, 0x00, 0xff};
    image->SetData(blue);
  };

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  paint_blue();
  EXPECT_EQ(kBlue, ColorAt(page.get(), 150, 150));
  ASSERT_TRUE(EPDFLayer_AbortTransaction(doc.get()));
  EXPECT_EQ(kRed, ColorAt(page.get(), 150, 150));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  paint_blue();
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  EXPECT_EQ(kBlue, ColorAt(page.get(), 150, 150));
  ScopedFPDFPage reloaded(FPDF_LoadPage(doc.get(), 0));
  EXPECT_EQ(kBlue, ColorAt(reloaded.get(), 150, 150));
}

// G6: what a transaction costs beside a 100 MB image in the layer. The copy
// unit is the indirect object, so writes that don't touch the image stream
// never pay for it. Prints a table; asserts the copy counts.
TEST_F(EPDFTransactionEmbedderTest, CostBesideALargeImage) {
  ScopedFPDFDocument doc = Open();
  ASSERT_TRUE(doc);
  CPDF_LayerDocument* layer = LayerOf(doc.get());
  ASSERT_TRUE(layer);
  ScopedFPDFPage first(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(first);

  // A stamp of an uncompressed 5800 x 6000 RGB image: ~104 MB of stream.
  constexpr int kWidth = 5800;
  constexpr int kHeight = 6000;
  constexpr FS_RECTF kBigRect = {20, 120, 140, 20};
  {
    ScopedFPDFAnnotation stamp =
        NewAnnot(first.get(), FPDF_ANNOT_STAMP, kBigRect);
    ScopedFPDFBitmap bitmap(FPDFBitmap_Create(kWidth, kHeight, 0));
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(
        FPDFBitmap_FillRect(bitmap.get(), 0, 0, kWidth, kHeight, 0xff2050a0));
    FPDF_PAGEOBJECT image = FPDFPageObj_NewImageObj(doc.get());
    ASSERT_TRUE(FPDFImageObj_SetBitmap(nullptr, 0, image, bitmap.get()));
    const FS_MATRIX matrix{120, 0, 0, 100, 20, 20};
    ASSERT_TRUE(FPDFPageObj_SetMatrix(image, &matrix));
    ASSERT_TRUE(FPDFAnnot_AppendObject(stamp.get(), image));
    ASSERT_TRUE(
        EPDFAnnot_UpdateAppearanceToRect(stamp.get(), EPDF_STAMP_FIT_STRETCH));
  }
  const int stamp_index = FPDFPage_GetAnnotCount(first.get()) - 1;

  using Clock = std::chrono::steady_clock;
  struct Row {
    std::string name;
    long long micros;
    CPDF_LayerTransactionStats stats;
  };
  std::vector<Row> rows;
  auto measure = [&](const std::string& name, bool transaction, bool commit,
                     const std::function<void()>& write) {
    const Clock::time_point t0 = Clock::now();
    if (transaction) {
      ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    }
    write();
    if (transaction) {
      ASSERT_TRUE(commit ? EPDFLayer_CommitTransaction(doc.get())
                         : EPDFLayer_AbortTransaction(doc.get()));
    }
    const long long micros =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0)
            .count();
    rows.push_back({name, micros,
                    transaction ? layer->GetTransactionStats()
                                : CPDF_LayerTransactionStats()});
  };
  auto set_contents = [&](int index, const wchar_t* text) {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(first.get(), index));
    ScopedFPDFWideString value = GetFPDFWideString(text);
    ASSERT_TRUE(FPDFAnnot_SetStringValue(annot.get(), "Contents", value.get()));
  };

  measure("other annotation, no transaction", false, false,
          [&] { set_contents(0, L"plain"); });
  measure("other annotation, commit", true, true,
          [&] { set_contents(0, L"committed"); });
  measure("other annotation, abort", true, false,
          [&] { set_contents(0, L"aborted"); });
  measure("the stamp's /Contents, commit", true, true,
          [&] { set_contents(stamp_index, L"stamp"); });
  measure("move the stamp (rect + appearance), commit", true, true, [&] {
    ScopedFPDFAnnotation stamp(FPDFPage_GetAnnot(first.get(), stamp_index));
    const FS_RECTF moved = {40, 140, 160, 40};
    ASSERT_TRUE(FPDFAnnot_SetRect(stamp.get(), &moved));
    ASSERT_TRUE(
        EPDFAnnot_UpdateAppearanceToRect(stamp.get(), EPDF_STAMP_FIT_STRETCH));
  });
  measure("render the page, abort", true, false,
          [&] { EXPECT_NE(kWhite, ColorAt(first.get(), 100, 90)); });

  std::cout << "\n[layer transaction cost beside a ~104 MB image stamp]\n";
  for (const Row& row : rows) {
    std::cout << "  " << row.name << ": " << row.micros << " us, copied "
              << row.stats.objects_copied << " objects / "
              << row.stats.stream_bytes_copied << " stream bytes, added "
              << row.stats.objects_added << "\n";
  }

  for (const Row& row : rows) {
    SCOPED_TRACE(row.name);
    EXPECT_LT(row.stats.stream_bytes_copied, 1024u * 1024u);
  }
}

// L4 at commit: an object created, cached and deleted inside a transaction
// is unreachable after the commit as much as after an abort, so the caches
// forget it either way.
TEST_F(EPDFTransactionEmbedderTest, CachesForgetObjectsDroppedInsideIt) {
  for (bool commit : {false, true}) {
    SCOPED_TRACE(commit ? "commit" : "abort");
    ScopedFPDFDocument doc = Open();
    ASSERT_TRUE(doc);
    CPDF_LayerDocument* layer = LayerOf(doc.get());
    ASSERT_TRUE(layer->BeginTransaction());
    RetainPtr<CPDF_Dictionary> dict = layer->NewIndirect<CPDF_Dictionary>();
    dict->SetNewFor<CPDF_Name>("Type", "Font");
    dict->SetNewFor<CPDF_Name>("Subtype", "Type1");
    dict->SetNewFor<CPDF_Name>("BaseFont", "Helvetica");
    RetainPtr<CPDF_Font> font =
        CPDF_DocPageData::FromDocument(layer)->GetFont(dict);
    ASSERT_TRUE(font);
    ASSERT_FALSE(font->HasOneRef());  // the cache holds it
    layer->DeleteIndirectObject(dict->GetObjNum());
    ASSERT_TRUE(commit ? layer->CommitTransaction()
                       : layer->AbortTransaction());
    EXPECT_TRUE(font->HasOneRef());
  }
}
