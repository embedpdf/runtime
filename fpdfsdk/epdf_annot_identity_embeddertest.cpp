// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Annotation identity on a layer (fork plan §4.14, T2): what an annotation
// handle names, and for how long.
// - L1: a handle is valid while its annotation is there - its object
//   resolves, it is in its page's /Annots, its page is in the document - and
//   only then; an abort that brings it back makes it valid again.
// - L2: object numbers are never reused, so a stale handle never names a
//   later annotation.
// - L3: an inline annotation is named by its birth index; promoting its page
//   records where it went (the birth list), and the handle follows.
// The birth list survives the layer artifact.

#include <string>
#include <vector>

#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

constexpr unsigned long kFirstPage = 3;
constexpr unsigned long kSecondPage = 4;
constexpr unsigned long kAnnotsArray = 5;
constexpr unsigned long kObjectAnnot = 6;

std::string Note(const std::string& contents, int x) {
  const std::string left = std::to_string(x);
  const std::string right = std::to_string(x + 20);
  return "<< /Type /Annot /Subtype /Text /Rect [" + left + " 10 " + right +
         " 30] /Contents (" + contents + ") >>";
}

// Two pages. The first has an indirect /Annots array (object 5) holding
// inline A, object 6 ("Obj"), inline B and inline C. The second has a direct
// /Annots holding inline D and E.
std::string MakeDocument() {
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 7 0 R "
      "/Resources << >> /Annots 5 0 R >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 7 0 R "
      "/Resources << >> /Annots [" +
          Note("D", 10) + " " + Note("E", 40) + "] >>",
      "[" + Note("A", 10) + " 6 0 R " + Note("B", 70) + " " + Note("C", 100) +
          "]",
      Note("Obj", 40),
      Stream(""),
  });
}

std::vector<std::string> ContentsOf(FPDF_PAGE page) {
  std::vector<std::string> contents;
  for (int i = 0; i < FPDFPage_GetAnnotCount(page); ++i) {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, i));
    contents.push_back(AnnotString(annot.get(), "Contents"));
  }
  return contents;
}

int ObjectNumber(FPDF_ANNOTATION annot) {
  return EPDFAnnot_GetObjectNumber(annot);
}

bool SetContents(FPDF_ANNOTATION annot, const wchar_t* text) {
  ScopedFPDFWideString value = GetFPDFWideString(text);
  return FPDFAnnot_SetStringValue(annot, "Contents", value.get());
}

}  // namespace

class EPDFAnnotIdentityEmbedderTest : public EmbedderTest {
 protected:
  void SetUp() override {
    EmbedderTest::SetUp();
    input_ = MakeDocument();
    base_ = EPDF_LoadMemBaseDocument(input_.data(), input_.size(), nullptr);
    ASSERT_TRUE(base_);
  }

  void TearDown() override {
    EPDF_ReleaseBaseDocument(base_);
    EmbedderTest::TearDown();
  }

  ScopedFPDFDocument OpenLayer() {
    return ScopedFPDFDocument(
        EPDFLayer_OpenLayer(base_, nullptr, nullptr, nullptr));
  }

  ScopedFPDFDocument OpenArtifact(std::string* artifact,
                                  EPDFLayerOpenStatus* status) {
    FPDF_FILEACCESS access = {};
    access.m_FileLen = artifact->size();
    access.m_GetBlock = GetBlockFromString;
    access.m_Param = artifact;
    return ScopedFPDFDocument(
        EPDFLayer_OpenLayerArtifact(base_, &access, nullptr, status));
  }

  std::string input_;
  EPDF_BASE_DOCUMENT base_ = nullptr;
};

// Promotion moves every inline annotation into its own object, in place,
// and records the births of the base's inline annotations, once.
TEST_F(EPDFAnnotIdentityEmbedderTest, PromotionKeepsOrderAndRecordsBirths) {
  ScopedFPDFDocument doc = OpenLayer();
  ASSERT_TRUE(doc);
  EXPECT_FALSE(EPDFLayer_IsPagePromoted(doc.get(), kFirstPage));

  EXPECT_EQ(3, EPDFPage_PromoteInlineAnnotsRaw(doc.get(), 0));
  EXPECT_TRUE(EPDFLayer_IsPagePromoted(doc.get(), kFirstPage));
  EXPECT_FALSE(EPDFLayer_IsPagePromoted(doc.get(), kSecondPage));

  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  EXPECT_EQ((std::vector<std::string>{"A", "Obj", "B", "C"}),
            ContentsOf(page.get()));
  const std::string names[] = {"A", "Obj", "B", "C"};
  for (int i = 0; i < 4; ++i) {
    SCOPED_TRACE(names[i]);
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), i));
    const unsigned long objnum = ObjectNumber(annot.get());
    ASSERT_NE(0u, objnum);
    unsigned long birth_page = 0;
    unsigned long birth_index = 0;
    if (i == 1) {
      EXPECT_EQ(kObjectAnnot, objnum);
      EXPECT_EQ(0u, EPDFLayer_GetBirthObjectNumber(doc.get(), kFirstPage, 1));
      EXPECT_FALSE(
          EPDFLayer_GetBirthName(doc.get(), objnum, &birth_page, &birth_index));
      continue;
    }
    EXPECT_EQ(objnum, EPDFLayer_GetBirthObjectNumber(doc.get(), kFirstPage,
                                                     static_cast<unsigned>(i)));
    ASSERT_TRUE(
        EPDFLayer_GetBirthName(doc.get(), objnum, &birth_page, &birth_index));
    EXPECT_EQ(kFirstPage, birth_page);
    EXPECT_EQ(static_cast<unsigned long>(i), birth_index);
  }

  // Once: nothing is inline any more, and the births stay as they are.
  const unsigned long c =
      EPDFLayer_GetBirthObjectNumber(doc.get(), kFirstPage, 3);
  EXPECT_EQ(0, EPDFPage_PromoteInlineAnnotsRaw(doc.get(), 0));
  EXPECT_EQ(c, EPDFLayer_GetBirthObjectNumber(doc.get(), kFirstPage, 3));
  EXPECT_EQ(-1, EPDFPage_PromoteInlineAnnotsRaw(doc.get(), 2));
}

// G11 + L3: inline handles made before a delete that promotes their page,
// and shifts the entries before them, still name their own annotation after
// the commit; the deleted one's handle is invalid. After an abort, all of
// them name their inline annotation again.
TEST_F(EPDFAnnotIdentityEmbedderTest, InlineHandlesFollowAPromotion) {
  for (bool commit : {true, false}) {
    SCOPED_TRACE(commit ? "commit" : "abort");
    ScopedFPDFDocument doc = OpenLayer();
    ASSERT_TRUE(doc);
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
    ScopedFPDFAnnotation a(FPDFPage_GetAnnot(page.get(), 0));
    ScopedFPDFAnnotation b(FPDFPage_GetAnnot(page.get(), 2));
    ScopedFPDFAnnotation c(FPDFPage_GetAnnot(page.get(), 3));
    ASSERT_EQ("C", AnnotString(c.get(), "Contents"));
    ASSERT_EQ(0, ObjectNumber(c.get()));  // inline

    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    ASSERT_EQ(3, EPDFPage_PromoteInlineAnnotsRaw(doc.get(), 0));
    ASSERT_TRUE(EPDFPage_RemoveAnnot(page.get(), 2));  // B; C shifts to 2
    // Inside the transaction the handles already see it.
    EXPECT_FALSE(EPDFAnnot_IsValid(b.get()));
    EXPECT_EQ("C", AnnotString(c.get(), "Contents"));
    ASSERT_TRUE(commit ? EPDFLayer_CommitTransaction(doc.get())
                       : EPDFLayer_AbortTransaction(doc.get()));

    if (commit) {
      EXPECT_EQ((std::vector<std::string>{"A", "Obj", "C"}),
                ContentsOf(page.get()));
      EXPECT_TRUE(EPDFAnnot_IsValid(a.get()));
      EXPECT_TRUE(EPDFAnnot_IsValid(c.get()));
      EXPECT_EQ("A", AnnotString(a.get(), "Contents"));
      EXPECT_EQ("C", AnnotString(c.get(), "Contents"));
      EXPECT_EQ(static_cast<int>(
                    EPDFLayer_GetBirthObjectNumber(doc.get(), kFirstPage, 3)),
                ObjectNumber(c.get()));
      EXPECT_FALSE(EPDFAnnot_IsValid(b.get()));
      EXPECT_EQ("", AnnotString(b.get(), "Contents"));
      EXPECT_FALSE(SetContents(b.get(), L"lost"));
      // A write through the followed handle lands on the promoted object.
      EXPECT_TRUE(SetContents(c.get(), L"C2"));
      ScopedFPDFAnnotation fresh(FPDFPage_GetAnnot(page.get(), 2));
      EXPECT_EQ("C2", AnnotString(fresh.get(), "Contents"));
    } else {
      EXPECT_EQ((std::vector<std::string>{"A", "Obj", "B", "C"}),
                ContentsOf(page.get()));
      EXPECT_FALSE(EPDFLayer_IsPagePromoted(doc.get(), kFirstPage));
      EXPECT_EQ(0u, EPDFLayer_GetBirthObjectNumber(doc.get(), kFirstPage, 3));
      for (FPDF_ANNOTATION handle : {a.get(), b.get(), c.get()}) {
        EXPECT_TRUE(EPDFAnnot_IsValid(handle));
        EXPECT_EQ(0, ObjectNumber(handle));  // inline again
      }
      EXPECT_EQ("B", AnnotString(b.get(), "Contents"));
      EXPECT_EQ("C", AnnotString(c.get(), "Contents"));
    }
  }
}

// G8 + L1/L2: handles to annotations that are gone - created in an aborted
// transaction, deleted, on a deleted page - are invalid, and a later
// annotation never makes them valid. A deleted base annotation keeps
// resolving as an object (rule 6), so membership decides.
TEST_F(EPDFAnnotIdentityEmbedderTest, HandlesToDroppedAnnotationsAreInvalid) {
  ScopedFPDFDocument doc = OpenLayer();
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));

  // Created in an aborted transaction.
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  ScopedFPDFAnnotation aborted =
      NewAnnot(page.get(), FPDF_ANNOT_SQUARE, {150, 280, 200, 230});
  const int aborted_number = ObjectNumber(aborted.get());
  EXPECT_TRUE(EPDFAnnot_IsValid(aborted.get()));
  ASSERT_TRUE(EPDFLayer_AbortTransaction(doc.get()));
  EXPECT_FALSE(EPDFAnnot_IsValid(aborted.get()));
  EXPECT_FALSE(SetContents(aborted.get(), L"lost"));
  ScopedFPDFAnnotation later =
      NewAnnot(page.get(), FPDF_ANNOT_SQUARE, {150, 280, 200, 230});
  EXPECT_NE(aborted_number, ObjectNumber(later.get()));
  EXPECT_FALSE(EPDFAnnot_IsValid(aborted.get()));

  // Deleted and committed: a layer-created object.
  const int index = FPDFPage_GetAnnotCount(page.get()) - 1;
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  ASSERT_TRUE(EPDFPage_RemoveAnnot(page.get(), index));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  EXPECT_FALSE(EPDFAnnot_IsValid(later.get()));

  // A base object annotation: deleting it only unreferences it.
  ScopedFPDFAnnotation object(FPDFPage_GetAnnot(page.get(), 1));
  ASSERT_EQ(static_cast<int>(kObjectAnnot), ObjectNumber(object.get()));
  for (bool commit : {false, true}) {
    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    ASSERT_EQ(3, EPDFPage_PromoteInlineAnnotsRaw(doc.get(), 0));
    ASSERT_TRUE(EPDFPage_RemoveAnnot(page.get(), 1));
    EXPECT_FALSE(EPDFAnnot_IsValid(object.get()));
    ASSERT_TRUE(commit ? EPDFLayer_CommitTransaction(doc.get())
                       : EPDFLayer_AbortTransaction(doc.get()));
    EXPECT_EQ(!commit, !!EPDFAnnot_IsValid(object.get()));
  }

  // On a deleted page: the page's inline annotations go with it.
  ScopedFPDFPage second(FPDF_LoadPage(doc.get(), 1));
  ScopedFPDFAnnotation d(FPDFPage_GetAnnot(second.get(), 0));
  ASSERT_EQ("D", AnnotString(d.get(), "Contents"));
  for (bool commit : {false, true}) {
    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    FPDFPage_Delete(doc.get(), 1);
    ASSERT_TRUE(commit ? EPDFLayer_CommitTransaction(doc.get())
                       : EPDFLayer_AbortTransaction(doc.get()));
    EXPECT_EQ(!commit, !!EPDFAnnot_IsValid(d.get()));
  }
}

// L1 for pages: a page handle is valid while its page is in the document.
TEST_F(EPDFAnnotIdentityEmbedderTest, PageHandlesFollowTheirPage) {
  ScopedFPDFDocument doc = OpenLayer();
  ASSERT_TRUE(doc);
  ScopedFPDFPage second(FPDF_LoadPage(doc.get(), 1));
  ASSERT_TRUE(EPDFPage_IsValid(second.get()));
  for (bool commit : {false, true}) {
    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    FPDFPage_Delete(doc.get(), 1);
    EXPECT_FALSE(EPDFPage_IsValid(second.get()));
    ASSERT_TRUE(commit ? EPDFLayer_CommitTransaction(doc.get())
                       : EPDFLayer_AbortTransaction(doc.get()));
    EXPECT_EQ(!commit, !!EPDFPage_IsValid(second.get()));
  }

  // A page created in an aborted transaction.
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  ScopedFPDFPage added(FPDFPage_New(doc.get(), 1, 300, 300));
  EXPECT_TRUE(EPDFPage_IsValid(added.get()));
  ASSERT_TRUE(EPDFLayer_AbortTransaction(doc.get()));
  EXPECT_FALSE(EPDFPage_IsValid(added.get()));
}

// G4: a handle acquired earlier - before the transaction, or by an earlier
// op of the same batch - writes through the door.
TEST_F(EPDFAnnotIdentityEmbedderTest, EarlierHandlesWriteThroughTheDoor) {
  for (bool commit : {false, true}) {
    SCOPED_TRACE(commit ? "commit" : "abort");
    ScopedFPDFDocument doc = OpenLayer();
    ASSERT_TRUE(doc);
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
    ScopedFPDFAnnotation object(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(SetContents(object.get(), L"committed"));  // a layer copy

    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    ScopedFPDFAnnotation first =
        NewAnnot(page.get(), FPDF_ANNOT_SQUARE, {150, 280, 200, 230});  // op 1
    NewAnnot(page.get(), FPDF_ANNOT_SQUARE, {150, 200, 200, 150});      // op 2
    ASSERT_TRUE(SetContents(first.get(), L"op 3"));                     // op 3
    ASSERT_TRUE(SetContents(object.get(), L"in the transaction"));
    ASSERT_TRUE(commit ? EPDFLayer_CommitTransaction(doc.get())
                       : EPDFLayer_AbortTransaction(doc.get()));

    EXPECT_EQ(commit ? "in the transaction" : "committed",
              AnnotString(object.get(), "Contents"));
    EXPECT_EQ(commit, !!EPDFAnnot_IsValid(first.get()));
    if (commit) {
      EXPECT_EQ("op 3", AnnotString(first.get(), "Contents"));
      EXPECT_EQ(6, FPDFPage_GetAnnotCount(page.get()));
    } else {
      EXPECT_EQ(4, FPDFPage_GetAnnotCount(page.get()));
    }
  }
}

// G3: an inline annotation inside an indirect /Annots array, updated in a
// transaction: an abort leaves the committed array as it was; a commit puts
// the change in the array's copy, and the delta carries it.
TEST_F(EPDFAnnotIdentityEmbedderTest, InlineAnnotationInAnIndirectArray) {
  for (bool commit : {false, true}) {
    SCOPED_TRACE(commit ? "commit" : "abort");
    ScopedFPDFDocument doc = OpenLayer();
    ASSERT_TRUE(doc);
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
    ScopedFPDFAnnotation a(FPDFPage_GetAnnot(page.get(), 0));

    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    ASSERT_TRUE(SetContents(a.get(), L"A2"));
    EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.get(), kAnnotsArray));
    EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.get(), kFirstPage));
    ASSERT_TRUE(commit ? EPDFLayer_CommitTransaction(doc.get())
                       : EPDFLayer_AbortTransaction(doc.get()));

    EXPECT_EQ(commit, !!EPDFLayer_IsObjectPromoted(doc.get(), kAnnotsArray));
    EXPECT_EQ(commit ? "A2" : "A", AnnotString(a.get(), "Contents"));
    ScopedFPDFAnnotation fresh(FPDFPage_GetAnnot(page.get(), 0));
    EXPECT_EQ(commit ? "A2" : "A", AnnotString(fresh.get(), "Contents"));
    ClearString();
    ASSERT_TRUE(EPDFLayer_SaveDelta(doc.get(), this, nullptr));
    EXPECT_EQ(commit, GetString().find("(A2)") != std::string::npos);
  }
}

// Linking to an inline annotation promotes its page: the target gets an
// object number in place, and no second copy appears.
TEST_F(EPDFAnnotIdentityEmbedderTest, LinkingToAnInlineAnnotationPromotes) {
  for (bool layer : {true, false}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    ScopedFPDFDocument doc = layer
                                 ? OpenLayer()
                                 : ScopedFPDFDocument(FPDF_LoadMemDocument(
                                       input_.data(), input_.size(), nullptr));
    ASSERT_TRUE(doc);
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
    ScopedFPDFAnnotation a(FPDFPage_GetAnnot(page.get(), 0));
    ScopedFPDFAnnotation reply =
        NewAnnot(page.get(), FPDF_ANNOT_TEXT, {200, 280, 220, 260});

    ASSERT_TRUE(EPDFAnnot_SetLinkedAnnot(reply.get(), "IRT", a.get()));
    EXPECT_EQ((std::vector<std::string>{"A", "Obj", "B", "C", ""}),
              ContentsOf(page.get()));
    const int target = ObjectNumber(a.get());
    ASSERT_NE(0, target);
    ScopedFPDFAnnotation linked(FPDFAnnot_GetLinkedAnnot(reply.get(), "IRT"));
    EXPECT_EQ(target, ObjectNumber(linked.get()));
    if (layer) {
      EXPECT_EQ(static_cast<unsigned long>(target),
                EPDFLayer_GetBirthObjectNumber(doc.get(), kFirstPage, 0));
    }
  }
}

// The birth list travels in the layer artifact (version 2); a version-1
// artifact opens as a layer that promoted nothing; a birth naming an object
// the delta doesn't carry is refused.
TEST_F(EPDFAnnotIdentityEmbedderTest, BirthsSurviveTheArtifact) {
  std::string artifact;
  unsigned long births[3] = {};
  {
    ScopedFPDFDocument doc = OpenLayer();
    ASSERT_TRUE(doc);
    ASSERT_EQ(3, EPDFPage_PromoteInlineAnnotsRaw(doc.get(), 0));
    for (unsigned long i : {0ul, 2ul, 3ul}) {
      births[i == 0 ? 0 : i - 1] =
          EPDFLayer_GetBirthObjectNumber(doc.get(), kFirstPage, i);
    }
    ClearString();
    ASSERT_TRUE(EPDFLayer_SaveLayerArtifact(doc.get(), this, nullptr));
    artifact = GetString();
  }

  EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
  {
    ScopedFPDFDocument reopened = OpenArtifact(&artifact, &status);
    ASSERT_TRUE(reopened);
    EXPECT_TRUE(EPDFLayer_IsPagePromoted(reopened.get(), kFirstPage));
    EXPECT_EQ(births[0],
              EPDFLayer_GetBirthObjectNumber(reopened.get(), kFirstPage, 0));
    EXPECT_EQ(births[1],
              EPDFLayer_GetBirthObjectNumber(reopened.get(), kFirstPage, 2));
    EXPECT_EQ(births[2],
              EPDFLayer_GetBirthObjectNumber(reopened.get(), kFirstPage, 3));
    // A handle made on the reopened layer names the promoted object.
    ScopedFPDFPage page(FPDF_LoadPage(reopened.get(), 0));
    ScopedFPDFAnnotation c(FPDFPage_GetAnnot(page.get(), 3));
    EXPECT_EQ(static_cast<int>(births[2]), ObjectNumber(c.get()));
  }

  // The header's last 8 bytes are the births' size, little-endian; the
  // births are the artifact's tail.
  const size_t header_size = 112;
  uint64_t births_size = 0;
  for (int i = 7; i >= 0; --i) {
    births_size = (births_size << 8) |
                  static_cast<uint8_t>(artifact[header_size - 8 + i]);
  }
  ASSERT_GT(births_size, 0u);

  // A birth that names an object the delta doesn't carry: refused.
  std::string corrupt = artifact;
  corrupt[corrupt.size() - 4] = '\x7f';  // the last birth's object number
  status = EPDFLayerOpenStatus_kSuccess;
  EXPECT_FALSE(OpenArtifact(&corrupt, &status));
  EXPECT_EQ(EPDFLayerOpenStatus_kMalformedDelta, status);

  // Version 1: the same header without the births' size, and no births.
  std::string v1 =
      artifact.substr(0, header_size - 8) +
      artifact.substr(header_size, artifact.size() - header_size -
                                       static_cast<size_t>(births_size));
  v1[8] = 1;                                    // version
  v1[12] = static_cast<char>(header_size - 8);  // header size (104)
  status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument old = OpenArtifact(&v1, &status);
  ASSERT_TRUE(old);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
  EXPECT_FALSE(EPDFLayer_IsPagePromoted(old.get(), kFirstPage));
}
