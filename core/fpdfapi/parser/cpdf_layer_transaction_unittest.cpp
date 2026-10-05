// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Layer transactions (see CPDF_LayerTransaction): reads look down the stack,
// the first write copies up, commit moves the overlay down, abort drops it,
// deletes hide only the layer's own versions, numbers are never given back.

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_pagemodule.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_base_document.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_write_generation.h"
#include "core/fxcrt/cfx_read_only_container_stream.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/retain_ptr.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

// Catalog 1, page tree 2, page 3 (with /Annots [4 0 R]), annotation 4,
// info 5.
std::string BuildBasePdf() {
  const std::vector<std::string> objects = {
      "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
      "2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n",
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100]\n"
      "   /Annots [4 0 R] >>\n"
      "endobj\n",
      "4 0 obj\n"
      "<< /Type /Annot /Subtype /Square /Rect [10 10 30 30] >>\n"
      "endobj\n",
      "5 0 obj\n<< /Title (Base Title) >>\nendobj\n",
  };

  std::ostringstream pdf;
  pdf << "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (const std::string& object : objects) {
    offsets.push_back(pdf.tellp());
    pdf << object;
  }

  const size_t xref_offset = pdf.tellp();
  pdf << "xref\n0 " << (objects.size() + 1) << "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
  }
  pdf << "trailer\n<< /Size " << (objects.size() + 1)
      << " /Root 1 0 R /Info 5 0 R >>\nstartxref\n"
      << xref_offset << "\n%%EOF\n";
  return pdf.str();
}

constexpr uint32_t kCatalog = 1;
constexpr uint32_t kPages = 2;
constexpr uint32_t kPage = 3;
constexpr uint32_t kAnnot = 4;

int Marker(const CPDF_Object* object) {
  const CPDF_Dictionary* dict = object ? object->GetDict().Get() : nullptr;
  return dict ? dict->GetIntegerFor("Marker") : 0;
}

int MarkerOf(CPDF_LayerDocument* layer, uint32_t objnum) {
  return Marker(layer->GetIndirectObject(objnum).Get());
}

void SetMarker(CPDF_LayerDocument* layer, uint32_t objnum, int value) {
  RetainPtr<CPDF_Object> object = layer->GetMutableIndirectObject(objnum);
  ASSERT_TRUE(object);
  object->GetMutableDict()->SetNewFor<CPDF_Number>("Marker", value);
}

class CPDFLayerTransactionTest : public testing::Test {
 protected:
  static void SetUpTestSuite() { pdfium::InitializePageModule(); }
  static void TearDownTestSuite() { pdfium::DestroyPageModule(); }

  void SetUp() override {
    const std::string pdf = BuildBasePdf();
    base_ = pdfium::MakeRetain<CPDF_BaseDocument>();
    ASSERT_EQ(
        CPDF_Parser::SUCCESS,
        base_->LoadBaseDoc(pdfium::MakeRetain<CFX_ReadOnlyByteStringStream>(
                               ByteString(pdf.data(), pdf.size())),
                           ""));
    layer_ = std::make_unique<CPDF_LayerDocument>(base_, nullptr);
    ASSERT_EQ(CPDF_LayerDocument::OpenStatus::kSuccess,
              layer_->ingest_status());
  }

  CPDF_LayerDocument* layer() { return layer_.get(); }

  // A layer object outside any transaction: it is committed.
  uint32_t CommitNewObject(int marker) {
    auto dict = layer_->NewIndirect<CPDF_Dictionary>();
    dict->SetNewFor<CPDF_Number>("Marker", marker);
    return dict->GetObjNum();
  }

  RetainPtr<CPDF_BaseDocument> base_;
  std::unique_ptr<CPDF_LayerDocument> layer_;
};

TEST_F(CPDFLayerTransactionTest, OneAtATime) {
  EXPECT_FALSE(layer()->CommitTransaction());
  EXPECT_FALSE(layer()->AbortTransaction());
  ASSERT_TRUE(layer()->BeginTransaction());
  EXPECT_TRUE(layer()->InTransaction());
  EXPECT_FALSE(layer()->BeginTransaction());  // never nested
  EXPECT_TRUE(layer()->AbortTransaction());
  EXPECT_FALSE(layer()->InTransaction());
}

TEST_F(CPDFLayerTransactionTest, ReadsSeeTheTransactionAndAbortDropsIt) {
  ASSERT_TRUE(layer()->BeginTransaction());
  SetMarker(layer(), kPage, 7);
  EXPECT_EQ(7, MarkerOf(layer(), kPage));
  EXPECT_EQ(7, Marker(layer()->FindPromotedObject(kPage).Get()));
  EXPECT_EQ(0u, layer()->GetPromotedObjectCount());  // nothing committed
  EXPECT_TRUE(layer()->HasPromotedObjects());        // but a view sees it

  EXPECT_TRUE(layer()->AbortTransaction());
  EXPECT_EQ(0, MarkerOf(layer(), kPage));
  EXPECT_FALSE(layer()->FindPromotedObject(kPage));
  EXPECT_EQ(0u, layer()->GetPromotedObjectCount());
  EXPECT_EQ(0, Marker(base_->GetFrozenObjectForLayer(kPage).Get()));
}

TEST_F(CPDFLayerTransactionTest, CommitMovesTheOverlayDown) {
  ASSERT_TRUE(layer()->BeginTransaction());
  SetMarker(layer(), kPage, 7);
  SetMarker(layer(), kPage, 8);  // the second write reuses the copy
  EXPECT_EQ(1u, layer()->GetTransactionStats().objects_copied);

  EXPECT_TRUE(layer()->CommitTransaction());
  EXPECT_EQ(8, MarkerOf(layer(), kPage));
  EXPECT_EQ(1u, layer()->GetPromotedObjectCount());
  EXPECT_EQ(1u, layer()->GetTransactionStats().objects_copied);
  EXPECT_EQ(0, Marker(base_->GetFrozenObjectForLayer(kPage).Get()));
}

TEST_F(CPDFLayerTransactionTest, CommittedVersionIsCopiedUpNotWrittenInPlace) {
  SetMarker(layer(), kPage, 1);  // committed
  RetainPtr<const CPDF_Object> committed = layer()->FindPromotedObject(kPage);
  ASSERT_TRUE(committed);

  ASSERT_TRUE(layer()->BeginTransaction());
  RetainPtr<CPDF_Object> copy = layer()->GetMutableIndirectObject(kPage);
  EXPECT_NE(committed.Get(), copy.Get());
  copy->GetMutableDict()->SetNewFor<CPDF_Number>("Marker", 2);
  EXPECT_EQ(1, Marker(committed.Get()));
  EXPECT_EQ(2, MarkerOf(layer(), kPage));

  EXPECT_TRUE(layer()->AbortTransaction());
  EXPECT_EQ(committed.Get(), layer()->GetIndirectObject(kPage).Get());
  EXPECT_EQ(1, MarkerOf(layer(), kPage));

  ASSERT_TRUE(layer()->BeginTransaction());
  SetMarker(layer(), kPage, 3);
  EXPECT_TRUE(layer()->CommitTransaction());
  EXPECT_EQ(3, MarkerOf(layer(), kPage));
  EXPECT_EQ(1, Marker(committed.Get()));  // a retained pointer is stale
}

TEST_F(CPDFLayerTransactionTest, CopiesRebindReferencesToTheLayer) {
  ASSERT_TRUE(layer()->BeginTransaction());
  RetainPtr<CPDF_Dictionary> catalog =
      ToDictionary(layer()->GetMutableIndirectObject(kCatalog));
  ASSERT_TRUE(catalog);
  // Reaching the page tree through the copied catalog copies it up too.
  RetainPtr<CPDF_Dictionary> pages = catalog->GetMutableDictFor("Pages");
  ASSERT_TRUE(pages);
  pages->SetNewFor<CPDF_Number>("Marker", 4);
  EXPECT_EQ(4, MarkerOf(layer(), kPages));
  EXPECT_EQ(2u, layer()->GetTransactionStats().objects_copied);
  EXPECT_TRUE(layer()->AbortTransaction());
  EXPECT_EQ(0, MarkerOf(layer(), kPages));
  EXPECT_EQ(0, Marker(base_->GetFrozenObjectForLayer(kPages).Get()));
}

TEST_F(CPDFLayerTransactionTest, NewObjectsAreDroppedAndNumbersNotReused) {
  const uint32_t before = layer()->GetLastObjNum();

  ASSERT_TRUE(layer()->BeginTransaction());
  auto created = layer()->NewIndirect<CPDF_Dictionary>();
  const uint32_t first = created->GetObjNum();
  EXPECT_EQ(before + 1, first);
  EXPECT_TRUE(layer()->GetIndirectObject(first));
  EXPECT_EQ(1u, layer()->GetTransactionStats().objects_added);
  EXPECT_TRUE(layer()->AbortTransaction());

  EXPECT_FALSE(layer()->GetIndirectObject(first));
  EXPECT_EQ(first, layer()->GetLastObjNum());  // rule 7: not rewound

  ASSERT_TRUE(layer()->BeginTransaction());
  const uint32_t second = layer()->NewIndirect<CPDF_Dictionary>()->GetObjNum();
  EXPECT_EQ(first + 1, second);
  EXPECT_TRUE(layer()->CommitTransaction());
  EXPECT_TRUE(layer()->GetIndirectObject(second));
  EXPECT_FALSE(layer()->GetIndirectObject(first));

  // Outside a transaction too: the next number follows the highest handed out.
  EXPECT_EQ(second + 1, CommitNewObject(0));
}

// §4.6 of the fork plan: every case of a delete, inside the transaction and
// after its commit. A delete hides the layer's own version; an object that
// exists in the base keeps resolving to its base version.
TEST_F(CPDFLayerTransactionTest, DeletionContract) {
  const uint32_t committed_new = CommitNewObject(11);
  SetMarker(layer(), kPage, 12);  // a base object with a committed copy

  ASSERT_TRUE(layer()->BeginTransaction());
  // Row 1: created in this transaction.
  const uint32_t created = layer()->NewIndirect<CPDF_Dictionary>()->GetObjNum();
  layer()->DeleteIndirectObject(created);
  EXPECT_FALSE(layer()->GetIndirectObject(created));
  // Row 2: created in the layer earlier.
  layer()->DeleteIndirectObject(committed_new);
  EXPECT_FALSE(layer()->GetIndirectObject(committed_new));
  // Row 3: a base object with a committed layer copy: the base answers.
  layer()->DeleteIndirectObject(kPage);
  EXPECT_EQ(base_->GetFrozenObjectForLayer(kPage).Get(),
            layer()->GetIndirectObject(kPage).Get());
  EXPECT_FALSE(layer()->FindPromotedObject(kPage));
  // Row 4: a base object copied up in this transaction.
  SetMarker(layer(), kAnnot, 13);
  layer()->DeleteIndirectObject(kAnnot);
  EXPECT_EQ(base_->GetFrozenObjectForLayer(kAnnot).Get(),
            layer()->GetIndirectObject(kAnnot).Get());
  // Row 5: a base object never copied.
  layer()->DeleteIndirectObject(kPages);
  EXPECT_EQ(base_->GetFrozenObjectForLayer(kPages).Get(),
            layer()->GetIndirectObject(kPages).Get());

  EXPECT_TRUE(layer()->CommitTransaction());
  EXPECT_FALSE(layer()->GetIndirectObject(created));
  EXPECT_FALSE(layer()->GetIndirectObject(committed_new));
  EXPECT_EQ(base_->GetFrozenObjectForLayer(kPage).Get(),
            layer()->GetIndirectObject(kPage).Get());
  EXPECT_EQ(base_->GetFrozenObjectForLayer(kAnnot).Get(),
            layer()->GetIndirectObject(kAnnot).Get());
  EXPECT_EQ(base_->GetFrozenObjectForLayer(kPages).Get(),
            layer()->GetIndirectObject(kPages).Get());
  EXPECT_EQ(0u, layer()->GetPromotedObjectCount());
}

TEST_F(CPDFLayerTransactionTest, AbortedDeletesComeBack) {
  const uint32_t committed_new = CommitNewObject(11);
  SetMarker(layer(), kPage, 12);

  ASSERT_TRUE(layer()->BeginTransaction());
  layer()->DeleteIndirectObject(committed_new);
  layer()->DeleteIndirectObject(kPage);
  EXPECT_FALSE(layer()->GetIndirectObject(committed_new));
  EXPECT_EQ(0, MarkerOf(layer(), kPage));
  // A write after the delete copies up the version reads now see: the base's.
  SetMarker(layer(), kPage, 14);
  EXPECT_EQ(14, MarkerOf(layer(), kPage));
  EXPECT_TRUE(layer()->AbortTransaction());

  EXPECT_EQ(11, MarkerOf(layer(), committed_new));
  EXPECT_EQ(12, MarkerOf(layer(), kPage));
}

// G10: the catalog, Info and the page tree are visible to every lookup
// before commit and gone after abort.
TEST_F(CPDFLayerTransactionTest, RootAndInfoFollowTheTransaction) {
  ASSERT_EQ(0, layer()->GetRoot()->GetIntegerFor("Marker"));
  ASSERT_TRUE(layer()->GetInfo());

  ASSERT_TRUE(layer()->BeginTransaction());
  layer()->GetMutableRoot()->SetNewFor<CPDF_Number>("Marker", 21);
  layer()->GetMutableInfo()->SetNewFor<CPDF_String>("Title", "Changed");
  EXPECT_EQ(21, layer()->GetRoot()->GetIntegerFor("Marker"));
  EXPECT_EQ("Changed", layer()->GetInfo()->GetByteStringFor("Title"));
  EXPECT_TRUE(layer()->FindPromotedObject(kCatalog));
  EXPECT_TRUE(layer()->AbortTransaction());

  EXPECT_EQ(0, layer()->GetRoot()->GetIntegerFor("Marker"));
  EXPECT_EQ("Base Title", layer()->GetInfo()->GetByteStringFor("Title"));
  EXPECT_FALSE(layer()->FindPromotedObject(kCatalog));

  ASSERT_TRUE(layer()->BeginTransaction());
  layer()->GetMutableRoot()->SetNewFor<CPDF_Number>("Marker", 22);
  EXPECT_TRUE(layer()->CommitTransaction());
  EXPECT_EQ(22, layer()->GetRoot()->GetIntegerFor("Marker"));
}

TEST_F(CPDFLayerTransactionTest, PageInsertAndDeleteAbort) {
  ASSERT_EQ(1, layer()->GetPageCount());
  RetainPtr<const CPDF_Dictionary> page_before = layer()->GetPageDictionary(0);

  ASSERT_TRUE(layer()->BeginTransaction());
  RetainPtr<CPDF_Dictionary> added = layer()->CreateNewPage(1);
  ASSERT_TRUE(added);
  EXPECT_EQ(2, layer()->GetPageCount());
  EXPECT_EQ(added.Get(), layer()->GetPageDictionary(1).Get());
  EXPECT_EQ(kPage, layer()->DeletePage(0));
  EXPECT_EQ(1, layer()->GetPageCount());
  EXPECT_EQ(added->GetObjNum(), layer()->GetPageDictionary(0)->GetObjNum());
  EXPECT_TRUE(layer()->AbortTransaction());

  EXPECT_EQ(1, layer()->GetPageCount());
  EXPECT_EQ(page_before.Get(), layer()->GetPageDictionary(0).Get());
  EXPECT_FALSE(layer()->GetIndirectObject(added->GetObjNum()));
  RetainPtr<const CPDF_Dictionary> pages =
      layer()->GetRoot()->GetDictFor("Pages");
  ASSERT_TRUE(pages);
  EXPECT_EQ(1, pages->GetIntegerFor("Count"));
  EXPECT_EQ(1u, pages->GetArrayFor("Kids")->size());

  ASSERT_TRUE(layer()->BeginTransaction());
  ASSERT_TRUE(layer()->CreateNewPage(1));
  EXPECT_TRUE(layer()->CommitTransaction());
  EXPECT_EQ(2, layer()->GetPageCount());
  EXPECT_EQ(2, layer()->GetRoot()->GetDictFor("Pages")->GetIntegerFor("Count"));
}

TEST_F(CPDFLayerTransactionTest, AnnotsAppendAborts) {
  ASSERT_TRUE(layer()->BeginTransaction());
  SetMarker(layer(), kAnnot, 1);
  RetainPtr<CPDF_Dictionary> page =
      ToDictionary(layer()->GetMutableIndirectObject(kPage));
  page->GetMutableArrayFor("Annots")->AppendNew<CPDF_Reference>(
      layer(), layer()->NewIndirect<CPDF_Dictionary>()->GetObjNum());
  EXPECT_EQ(2u, layer()->GetTransactionStats().objects_copied);
  EXPECT_EQ(1u, layer()->GetTransactionStats().objects_added);
  EXPECT_TRUE(layer()->AbortTransaction());
  EXPECT_EQ(1, layer()->GetPageCount());
  EXPECT_EQ(1u, ToDictionary(layer()->GetIndirectObject(kPage))
                    ->GetArrayFor("Annots")
                    ->size());
}

// T1: every object the layer stores carries the generation it was stored
// under: 1 outside any transaction, the transaction's own inside one, and a
// detached child takes its owner's when attached.
TEST_F(CPDFLayerTransactionTest, GenerationsAreStampedWhereObjectsAreStored) {
  EXPECT_EQ(0u, CPDF_WriteGeneration::Current());

  // Outside a transaction: promoted and created objects, with their children.
  RetainPtr<CPDF_Dictionary> promoted =
      ToDictionary(layer()->GetMutableIndirectObject(kPage));
  ASSERT_TRUE(promoted);
  EXPECT_EQ(CPDF_WriteGeneration::kCommitted, promoted->write_generation());
  EXPECT_EQ(CPDF_WriteGeneration::kCommitted,
            promoted->GetArrayFor("MediaBox")->write_generation());
  EXPECT_EQ(CPDF_WriteGeneration::kCommitted,
            layer()->NewIndirect<CPDF_Dictionary>()->write_generation());

  // Detached until attached; then its owner's.
  auto detached = layer()->New<CPDF_Dictionary>();
  detached->SetNewFor<CPDF_Number>("Inner", 1);
  EXPECT_EQ(0u, detached->write_generation());
  promoted->SetFor("Detached", detached);
  EXPECT_EQ(CPDF_WriteGeneration::kCommitted, detached->write_generation());
  EXPECT_EQ(CPDF_WriteGeneration::kCommitted,
            detached->GetObjectFor("Inner")->write_generation());

  // Inside: copies, their children, new objects and attached ones.
  ASSERT_TRUE(layer()->BeginTransaction());
  const uint32_t first = CPDF_WriteGeneration::Current();
  EXPECT_GT(first, CPDF_WriteGeneration::kCommitted);
  RetainPtr<CPDF_Dictionary> copy =
      ToDictionary(layer()->GetMutableIndirectObject(kPage));
  EXPECT_EQ(first, copy->write_generation());
  EXPECT_EQ(first, copy->GetArrayFor("MediaBox")->write_generation());
  EXPECT_EQ(first, layer()->NewIndirect<CPDF_Dictionary>()->write_generation());
  RetainPtr<CPDF_Array> attached = copy->SetNewFor<CPDF_Array>("Attached");
  EXPECT_EQ(first, attached->write_generation());
  EXPECT_EQ(CPDF_WriteGeneration::kCommitted, promoted->write_generation());
  EXPECT_TRUE(layer()->AbortTransaction());
  EXPECT_EQ(0u, CPDF_WriteGeneration::Current());

  // An aborted transaction's generation is never handed out again.
  ASSERT_TRUE(layer()->BeginTransaction());
  EXPECT_GT(CPDF_WriteGeneration::Current(), first);
  EXPECT_TRUE(layer()->CommitTransaction());
  EXPECT_EQ(0u, CPDF_WriteGeneration::Current());
}

// T1: what may be written while a transaction is open.
TEST_F(CPDFLayerTransactionTest, OnlyTheTransactionsOwnObjectsAreWritable) {
  RetainPtr<CPDF_Dictionary> committed =
      ToDictionary(layer()->GetMutableIndirectObject(kPage));
  auto detached = layer()->New<CPDF_Dictionary>();
  RetainPtr<const CPDF_Object> base = base_->GetFrozenObjectForLayer(kAnnot);
  EXPECT_TRUE(committed->IsWritable());  // no transaction: today's rules
  EXPECT_FALSE(base->IsWritable());      // frozen, always

  ASSERT_TRUE(layer()->BeginTransaction());
  EXPECT_FALSE(committed->IsWritable());
  EXPECT_FALSE(committed->GetArrayFor("MediaBox")->IsWritable());
  EXPECT_TRUE(detached->IsWritable());
  RetainPtr<CPDF_Object> copy = layer()->GetMutableIndirectObject(kPage);
  EXPECT_TRUE(copy->IsWritable());
  EXPECT_TRUE(layer()->CommitTransaction());
  EXPECT_TRUE(copy->IsWritable());

  // Committed by that transaction: a later one must copy it up too.
  ASSERT_TRUE(layer()->BeginTransaction());
  EXPECT_FALSE(copy->IsWritable());
  EXPECT_NE(copy.Get(), layer()->GetMutableIndirectObject(kPage).Get());
  EXPECT_TRUE(layer()->AbortTransaction());
}

// One transaction per thread: the open generation is per thread.
TEST_F(CPDFLayerTransactionTest, OneOpenTransactionPerThread) {
  auto other = std::make_unique<CPDF_LayerDocument>(base_, nullptr);
  ASSERT_TRUE(layer()->BeginTransaction());
  EXPECT_FALSE(other->BeginTransaction());
  EXPECT_TRUE(layer()->AbortTransaction());
  EXPECT_TRUE(other->BeginTransaction());
  other.reset();  // closing inside a transaction drops it
  EXPECT_EQ(0u, CPDF_WriteGeneration::Current());
  EXPECT_TRUE(layer()->BeginTransaction());
  EXPECT_TRUE(layer()->CommitTransaction());
}

#if DCHECK_IS_ON()
// G7: a write that bypasses the door - through a pointer retained from
// before the transaction, to an object committed outside it or by an earlier
// transaction, or to a direct child of one - fails the generation check.
TEST_F(CPDFLayerTransactionTest, BypassingTheDoorFailsTheCheck) {
  RetainPtr<CPDF_Dictionary> committed =
      ToDictionary(layer()->GetMutableIndirectObject(kPage));
  RetainPtr<CPDF_Array> media_box = committed->GetMutableArrayFor("MediaBox");
  ASSERT_TRUE(layer()->BeginTransaction());
  EXPECT_DEATH(committed->SetNewFor<CPDF_Number>("Bypass", 1), "IsWritable");
  EXPECT_DEATH(committed->RemoveFor("MediaBox"), "IsWritable");
  EXPECT_DEATH(media_box->AppendNew<CPDF_Number>(1), "IsWritable");
  EXPECT_DEATH(ToNumber(media_box->GetMutableObjectAt(0))->SetString("7"),
               "IsWritable");
  EXPECT_TRUE(layer()->CommitTransaction());

  // Committed by an earlier transaction.
  ASSERT_TRUE(layer()->BeginTransaction());
  auto created =
      layer()->NewIndirect<CPDF_Stream>(layer()->New<CPDF_Dictionary>());
  EXPECT_TRUE(layer()->CommitTransaction());
  ASSERT_TRUE(layer()->BeginTransaction());
  const uint8_t bytes[] = {'b', 'y', 'p', 'a', 's', 's'};
  EXPECT_DEATH(created->SetData(bytes), "IsWritable");
  EXPECT_DEATH(created->GetMutableDict()->SetNewFor<CPDF_Number>("X", 1),
               "IsWritable");
  EXPECT_TRUE(layer()->AbortTransaction());
}
#endif  // DCHECK_IS_ON()

// L4: derived caches forget the versions nobody can reach - after an abort
// the transaction's copies, after a commit the versions it replaced - so a
// cached colour space doesn't keep one alive.
TEST_F(CPDFLayerTransactionTest, CachesForgetUnreachableVersions) {
  CPDF_DocPageData* page_data = CPDF_DocPageData::FromDocument(layer());
  ASSERT_TRUE(page_data);
  auto cal_gray = layer()->NewIndirect<CPDF_Array>();  // committed
  cal_gray->AppendNew<CPDF_Name>("CalGray");
  auto params = cal_gray->AppendNew<CPDF_Dictionary>();
  params->SetNewFor<CPDF_Array>("WhitePoint")->AppendNew<CPDF_Number>(1);
  params->GetMutableArrayFor("WhitePoint")->AppendNew<CPDF_Number>(1);
  params->GetMutableArrayFor("WhitePoint")->AppendNew<CPDF_Number>(1);
  const uint32_t objnum = cal_gray->GetObjNum();
  params.Reset();

  auto copy_up_and_cache = [&]() -> RetainPtr<const CPDF_Object> {
    RetainPtr<CPDF_Array> copy =
        ToArray(layer()->GetMutableIndirectObject(objnum));
    EXPECT_TRUE(copy);
    copy->GetMutableDictAt(1)->SetNewFor<CPDF_Number>("Gamma", 2);
    EXPECT_TRUE(page_data->GetColorSpace(copy.Get(), nullptr));
    return copy;
  };

  // Abort: the copy was cached, and only this test holds it afterwards.
  ASSERT_TRUE(layer()->BeginTransaction());
  RetainPtr<const CPDF_Object> aborted = copy_up_and_cache();
  ASSERT_TRUE(layer()->AbortTransaction());
  EXPECT_TRUE(aborted->HasOneRef());

  // Commit: the committed version it replaced was cached too.
  RetainPtr<const CPDF_Object> committed = std::move(cal_gray);
  ASSERT_TRUE(page_data->GetColorSpace(committed.Get(), nullptr));
  ASSERT_TRUE(layer()->BeginTransaction());
  RetainPtr<const CPDF_Object> replacement = copy_up_and_cache();
  ASSERT_TRUE(layer()->CommitTransaction());
  EXPECT_TRUE(committed->HasOneRef());
  EXPECT_FALSE(replacement->HasOneRef());  // it is the committed version now
}

// G6: what a transaction costs beside a large image in the layer. The copy
// is per indirect object, so an edit that doesn't touch the image never pays
// for it; one that does (today) copies its bytes.
TEST_F(CPDFLayerTransactionTest, CostBesideALargeImage) {
  constexpr size_t kImageBytes = 100u * 1024 * 1024;
  uint32_t image_objnum = 0;
  {
    DataVector<uint8_t> bytes(kImageBytes, 0x5a);
    auto dict = layer()->New<CPDF_Dictionary>();
    dict->SetNewFor<CPDF_Name>("Type", "XObject");
    dict->SetNewFor<CPDF_Name>("Subtype", "Image");
    auto image =
        layer()->NewIndirect<CPDF_Stream>(std::move(bytes), std::move(dict));
    image_objnum = image->GetObjNum();
    ASSERT_TRUE(image->IsMemoryBased());
  }
  SetMarker(layer(), kAnnot, 1);  // the annotation is already in the layer

  using Clock = std::chrono::steady_clock;
  auto micros = [](Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
  };

  // Baseline: the same edit with no transaction.
  Clock::time_point t0 = Clock::now();
  SetMarker(layer(), kAnnot, 2);
  const auto plain_us = micros(Clock::now() - t0);

  // An unrelated annotation update: begin, copy up, commit.
  t0 = Clock::now();
  ASSERT_TRUE(layer()->BeginTransaction());
  SetMarker(layer(), kAnnot, 3);
  ASSERT_TRUE(layer()->CommitTransaction());
  const auto unrelated_us = micros(Clock::now() - t0);
  const CPDF_LayerTransactionStats unrelated = layer()->GetTransactionStats();
  EXPECT_EQ(1u, unrelated.objects_copied);
  EXPECT_EQ(0u, unrelated.stream_bytes_copied);

  // The same, aborted.
  t0 = Clock::now();
  ASSERT_TRUE(layer()->BeginTransaction());
  SetMarker(layer(), kAnnot, 4);
  ASSERT_TRUE(layer()->AbortTransaction());
  const auto aborted_us = micros(Clock::now() - t0);
  EXPECT_EQ(3, MarkerOf(layer(), kAnnot));

  // An edit to the image's dictionary copies the stream, bytes included,
  // until streams share immutable bytes (T3).
  t0 = Clock::now();
  ASSERT_TRUE(layer()->BeginTransaction());
  SetMarker(layer(), image_objnum, 5);
  ASSERT_TRUE(layer()->AbortTransaction());
  const auto image_us = micros(Clock::now() - t0);
  const CPDF_LayerTransactionStats touched = layer()->GetTransactionStats();
  EXPECT_EQ(1u, touched.objects_copied);
  EXPECT_EQ(kImageBytes, touched.stream_bytes_copied);
  EXPECT_EQ(0, MarkerOf(layer(), image_objnum));

  std::cout << "\n[layer transaction cost beside a 100 MB in-memory image]\n"
            << "  plain edit, no transaction:      " << plain_us << " us\n"
            << "  unrelated edit, begin..commit:   " << unrelated_us
            << " us (objects copied " << unrelated.objects_copied
            << ", stream bytes " << unrelated.stream_bytes_copied << ")\n"
            << "  unrelated edit, begin..abort:    " << aborted_us << " us\n"
            << "  image dict edit, begin..abort:   " << image_us
            << " us (objects copied " << touched.objects_copied
            << ", stream bytes " << touched.stream_bytes_copied << ")\n";
}

}  // namespace
