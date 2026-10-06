// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Objects at numbers the caller chooses. Raising a layer's last object
// number hands out the numbers below it; a create inside a transaction can
// take any of them that is free: above the base's numbers, at or below the
// last one, holding no layer object and untouched by the open transaction.
// New objects without a chosen number go above the last one. Which numbers a
// caller may use is the caller's decision; the layer only refuses a number
// that isn't free.

#include <string>
#include <vector>

#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_form.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using testing::ElementsAre;
using testing::IsEmpty;
using testing::UnorderedElementsAre;

namespace {

constexpr unsigned long kFirstPage = 3;
constexpr unsigned long kSecondPage = 4;
constexpr unsigned long kMergedField = 5;
constexpr unsigned long kBaseLast = 6;
// What the tests raise the last object number to: 7 to 26 are handed out.
constexpr unsigned long kRaised = 26;

// Two pages and a form. The first page holds a merged text field and widget
// (object 5) with no /P; the second has no annotations.
std::string MakeDocument() {
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [5 0 R] "
      "/DA (/Helv 0 Tf 0 g) /DR << /Font << /Helv << /Type /Font "
      "/Subtype /Type1 /BaseFont /Helvetica >> >> >> >> >>",
      "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 6 0 R "
      "/Resources << >> /Annots [5 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 6 0 R "
      "/Resources << >> >>",
      "<< /Type /Annot /Subtype /Widget /FT /Tx /T (merged) "
      "/Rect [10 10 110 30] >>",
      Stream(""),
  });
}

// The object numbers |array| refers to, in order.
std::vector<unsigned long> RefsIn(const CPDF_Array* array) {
  std::vector<unsigned long> refs;
  for (size_t i = 0; array && i < array->size(); ++i) {
    RetainPtr<const CPDF_Object> entry = array->GetObjectAt(i);
    const CPDF_Reference* ref = ToReference(entry.Get());
    refs.push_back(ref ? ref->GetRefObjNum() : 0);
  }
  return refs;
}

// The page tree's /Kids.
std::vector<unsigned long> Kids(FPDF_DOCUMENT doc) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  CPDF_DocumentViewScope view(pdf);
  RetainPtr<const CPDF_Dictionary> pages = pdf->GetRoot()->GetDictFor("Pages");
  return RefsIn(pages->GetArrayFor("Kids").Get());
}

int PageTreeCount(FPDF_DOCUMENT doc) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  CPDF_DocumentViewScope view(pdf);
  return pdf->GetRoot()->GetDictFor("Pages")->GetIntegerFor("Count");
}

// Page |page_index|'s /Annots.
std::vector<unsigned long> Annots(FPDF_DOCUMENT doc, int page_index) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  CPDF_DocumentViewScope view(pdf);
  RetainPtr<const CPDF_Dictionary> page = pdf->GetPageDictionary(page_index);
  return RefsIn(page->GetArrayFor("Annots").Get());
}

// The object number the dictionary |objnum| refers to under |key|: its
// /Parent, or an annotation's page (/P).
unsigned long ReferenceIn(FPDF_DOCUMENT doc,
                          unsigned long objnum,
                          const char* key) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  CPDF_DocumentViewScope view(pdf);
  RetainPtr<const CPDF_Dictionary> dict =
      ToDictionary(pdf->GetOrParseIndirectObject(objnum));
  RetainPtr<const CPDF_Dictionary> target =
      dict ? dict->GetDictFor(key) : nullptr;
  return target ? target->GetObjNum() : 0;
}

// The object number of an annotation's normal appearance stream.
unsigned long NormalAppearanceOf(FPDF_ANNOTATION annot) {
  const CPDF_Dictionary* dict =
      CPDFAnnotContextFromFPDFAnnotation(annot)->GetAnnotDict();
  RetainPtr<const CPDF_Dictionary> ap = dict->GetDictFor("AP");
  RetainPtr<const CPDF_Object> normal = ap ? ap->GetObjectFor("N") : nullptr;
  const CPDF_Reference* ref = ToReference(normal.Get());
  return ref ? ref->GetRefObjNum() : 0;
}

// Creates an annotation at |objnum| (0: the next free number) on page
// |page_index|. Returns its object number, or 0 when the create was refused.
unsigned long CreateAnnot(FPDF_DOCUMENT doc,
                          int page_index,
                          unsigned long objnum,
                          FPDF_ANNOTATION_SUBTYPE subtype = FPDF_ANNOT_SQUARE) {
  ScopedFPDFAnnotation annot(
      EPDFPage_CreateAnnotRaw(doc, page_index, subtype, objnum));
  return annot ? EPDFAnnot_GetObjectNumber(annot.get()) : 0;
}

// Creates a widget annotation with a rect at |objnum| on page |page_index|.
bool CreateWidget(FPDF_DOCUMENT doc, int page_index, unsigned long objnum) {
  ScopedFPDFAnnotation widget(
      EPDFPage_CreateAnnotRaw(doc, page_index, FPDF_ANNOT_WIDGET, objnum));
  const FS_RECTF rect{20, 220, 200, 200};
  return widget && FPDFAnnot_SetRect(widget.get(), &rect);
}

uint32_t CreateTextField(FPDF_DOCUMENT doc,
                         const wchar_t* name,
                         uint32_t objnum) {
  return EPDFForm_CreateField(doc, EPDF_FORMFIELD_FAMILY_TEXT,
                              GetFPDFWideString(name).get(), objnum);
}

// The widgets of the field |field_objnum|, in order; empty when the form
// has no such field.
std::vector<unsigned long> WidgetsOf(FPDF_DOCUMENT doc, uint32_t field_objnum) {
  std::vector<unsigned long> widgets;
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(doc);
  const int index = EPDFForm_GetFieldIndexByObjNum(model, field_objnum);
  for (int i = 0; index >= 0 && i < EPDFForm_CountFieldWidgets(model, index);
       ++i) {
    widgets.push_back(EPDFForm_GetFieldWidgetObjNum(model, index, i));
  }
  EPDFForm_CloseModel(model);
  return widgets;
}

int CountFields(FPDF_DOCUMENT doc) {
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(doc);
  const int count = EPDFForm_CountFields(model);
  EPDFForm_CloseModel(model);
  return count;
}

}  // namespace

class EPDFObjectNumberEmbedderTest : public EmbedderTest {
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

  // A layer over the base, its last object number raised to kRaised.
  ScopedFPDFDocument OpenLayer() {
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base_, nullptr, nullptr, nullptr));
    if (layer && !EPDFLayer_RaiseLastObjectNumber(layer.get(), kRaised)) {
      return nullptr;
    }
    return layer;
  }

  std::string SaveArtifact(FPDF_DOCUMENT layer) {
    ClearString();
    EXPECT_TRUE(EPDFLayer_SaveLayerArtifact(layer, this, nullptr));
    return GetString();
  }

  ScopedFPDFDocument OpenArtifact(std::string* artifact) {
    FPDF_FILEACCESS access = {};
    access.m_FileLen = artifact->size();
    access.m_GetBlock = GetBlockFromString;
    access.m_Param = artifact;
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayerArtifact(base_, &access, nullptr, &status));
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    return layer;
  }

  std::string input_;
  EPDF_BASE_DOCUMENT base_ = nullptr;
};

// Raising only ever raises, and what is created without a chosen number goes
// above it.
TEST_F(EPDFObjectNumberEmbedderTest, RaiseHandsOutNumbersBelowIt) {
  ScopedFPDFDocument doc(EPDFLayer_OpenLayer(base_, nullptr, nullptr, nullptr));
  ASSERT_TRUE(doc);
  EXPECT_EQ(kBaseLast, EPDFLayer_GetLastObjectNumber(doc.get()));

  ASSERT_TRUE(EPDFLayer_RaiseLastObjectNumber(doc.get(), kRaised));
  EXPECT_EQ(kRaised, EPDFLayer_GetLastObjectNumber(doc.get()));
  ASSERT_TRUE(EPDFLayer_RaiseLastObjectNumber(doc.get(), 10));
  EXPECT_EQ(kRaised, EPDFLayer_GetLastObjectNumber(doc.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  EXPECT_EQ(kRaised + 1, CreateAnnot(doc.get(), 1, 0));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  EXPECT_EQ(kRaised + 1, EPDFLayer_GetLastObjectNumber(doc.get()));
}

TEST_F(EPDFObjectNumberEmbedderTest, RaiseStopsAtTheParserLimit) {
  ScopedFPDFDocument doc(EPDFLayer_OpenLayer(base_, nullptr, nullptr, nullptr));
  ASSERT_TRUE(doc);
  EXPECT_FALSE(EPDFLayer_RaiseLastObjectNumber(
      doc.get(), CPDF_Parser::kMaxObjectNumber + 1));
  EXPECT_EQ(kBaseLast, EPDFLayer_GetLastObjectNumber(doc.get()));
  EXPECT_TRUE(EPDFLayer_RaiseLastObjectNumber(doc.get(),
                                              CPDF_Parser::kMaxObjectNumber));
  EXPECT_EQ(CPDF_Parser::kMaxObjectNumber,
            EPDFLayer_GetLastObjectNumber(doc.get()));
}

// A document that is not a layer has no last object number to raise and
// takes no chosen numbers; creates without one work as before.
TEST_F(EPDFObjectNumberEmbedderTest, PlainDocumentTakesNoChosenNumber) {
  ScopedFPDFDocument doc(
      FPDF_LoadMemDocument(input_.data(), input_.size(), nullptr));
  ASSERT_TRUE(doc);
  EXPECT_EQ(0u, EPDFLayer_GetLastObjectNumber(doc.get()));
  EXPECT_FALSE(EPDFLayer_RaiseLastObjectNumber(doc.get(), kRaised));

  EXPECT_EQ(0u, CreateAnnot(doc.get(), 1, kBaseLast + 1));
  EXPECT_FALSE(EPDFPage_InsertBlankRaw(doc.get(), 0, 100, 100, kBaseLast + 1));
  EXPECT_EQ(0u, CreateTextField(doc.get(), L"name", kBaseLast + 1));
  EXPECT_THAT(Annots(doc.get(), 1), IsEmpty());
  EXPECT_EQ(2, FPDF_GetPageCount(doc.get()));
  EXPECT_EQ(1, CountFields(doc.get()));

  EXPECT_EQ(kBaseLast + 1, CreateAnnot(doc.get(), 1, 0));
  ASSERT_TRUE(EPDFPage_InsertBlankRaw(doc.get(), 2, 100, 100, 0));
  EXPECT_EQ(3, FPDF_GetPageCount(doc.get()));
}

// The annotation is the object at the chosen number, in its page's /Annots;
// what it makes for itself gets new numbers; a save and reopen keep it.
TEST_F(EPDFObjectNumberEmbedderTest, AnnotationTakesTheChosenNumber) {
  std::string artifact;
  {
    ScopedFPDFDocument doc = OpenLayer();
    ASSERT_TRUE(doc);
    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnotRaw(doc.get(), 1, FPDF_ANNOT_SQUARE, 20));
    ASSERT_TRUE(annot);
    EXPECT_EQ(20u, EPDFAnnot_GetObjectNumber(annot.get()));
    const FS_RECTF rect{10, 50, 50, 10};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    for (FPDFANNOT_COLORTYPE type :
         {FPDFANNOT_COLORTYPE_Color, FPDFANNOT_COLORTYPE_InteriorColor}) {
      ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(), type, 255, 0, 0, 255));
    }
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
    EXPECT_GT(NormalAppearanceOf(annot.get()), kRaised);
    annot.reset();
    EXPECT_THAT(Annots(doc.get(), 1), ElementsAre(20u));
    EXPECT_EQ(kSecondPage, ReferenceIn(doc.get(), 20, "P"));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
    EXPECT_EQ(0, EPDFPage_GetAnnotIndexByObjectNumberRaw(doc.get(), 1, 20));
    artifact = SaveArtifact(doc.get());
  }

  ScopedFPDFDocument reopened = OpenArtifact(&artifact);
  ASSERT_TRUE(reopened);
  EXPECT_THAT(Annots(reopened.get(), 1), ElementsAre(20u));
  EXPECT_EQ(0, EPDFPage_GetAnnotIndexByObjectNumberRaw(reopened.get(), 1, 20));
  ScopedFPDFPage page(FPDF_LoadPage(reopened.get(), 1));
  ASSERT_TRUE(page);
  EXPECT_EQ(0xFF0000u, ColorAt(page.get(), 30, 30));
}

// Chosen numbers in any order, and the next free number, in one transaction.
TEST_F(EPDFObjectNumberEmbedderTest, ChosenAndNextNumbersInOneTransaction) {
  ScopedFPDFDocument doc = OpenLayer();
  ASSERT_TRUE(doc);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  EXPECT_EQ(20u, CreateAnnot(doc.get(), 1, 20));
  EXPECT_EQ(15u, CreateAnnot(doc.get(), 1, 15));
  EXPECT_EQ(kRaised + 1, CreateAnnot(doc.get(), 1, 0));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  EXPECT_THAT(Annots(doc.get(), 1), ElementsAre(20u, 15u, kRaised + 1u));
}

// A number is refused outside a transaction, when it is the base's or was
// never handed out, and when a layer object holds it or the open transaction
// touched it. A refusal changes nothing.
TEST_F(EPDFObjectNumberEmbedderTest, RefusesNumbersThatAreNotFree) {
  ScopedFPDFDocument doc = OpenLayer();
  ASSERT_TRUE(doc);
  EXPECT_EQ(0u, CreateAnnot(doc.get(), 1, 12));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  EXPECT_EQ(0u, CreateAnnot(doc.get(), 1, kSecondPage));
  EXPECT_EQ(0u, CreateAnnot(doc.get(), 1, kBaseLast));
  EXPECT_EQ(0u, CreateAnnot(doc.get(), 1, kRaised + 1));
  EXPECT_EQ(12u, CreateAnnot(doc.get(), 1, 12));
  EXPECT_EQ(0u, CreateAnnot(doc.get(), 1, 12));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  EXPECT_EQ(0u, CreateAnnot(doc.get(), 1, 12));
  // Deleted by this transaction, it still holds its number until the
  // transaction ends.
  ASSERT_TRUE(EPDFPage_RemoveAnnotRaw(doc.get(), 1, 0));
  EXPECT_EQ(0u, CreateAnnot(doc.get(), 1, 12));
  ASSERT_TRUE(EPDFLayer_AbortTransaction(doc.get()));

  EXPECT_THAT(Annots(doc.get(), 1), ElementsAre(12u));
  EXPECT_EQ(kRaised, EPDFLayer_GetLastObjectNumber(doc.get()));
}

// An abort leaves the number free: a later transaction can create at it.
TEST_F(EPDFObjectNumberEmbedderTest, AbortLeavesTheNumberFree) {
  ScopedFPDFDocument doc = OpenLayer();
  ASSERT_TRUE(doc);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  EXPECT_EQ(12u, CreateAnnot(doc.get(), 1, 12));
  ASSERT_TRUE(EPDFLayer_AbortTransaction(doc.get()));
  EXPECT_THAT(Annots(doc.get(), 1), IsEmpty());
  EXPECT_EQ(kRaised, EPDFLayer_GetLastObjectNumber(doc.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  EXPECT_EQ(12u, CreateAnnot(doc.get(), 1, 12));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  EXPECT_THAT(Annots(doc.get(), 1), ElementsAre(12u));
}

// The layer artifact keeps the raised last object number: after a reopen the
// numbers the layer handed out but didn't use are still free.
TEST_F(EPDFObjectNumberEmbedderTest, ArtifactKeepsTheRaise) {
  std::string artifact;
  {
    ScopedFPDFDocument doc = OpenLayer();
    ASSERT_TRUE(doc);
    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    EXPECT_EQ(20u, CreateAnnot(doc.get(), 1, 20));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
    artifact = SaveArtifact(doc.get());
  }

  ScopedFPDFDocument reopened = OpenArtifact(&artifact);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(kRaised, EPDFLayer_GetLastObjectNumber(reopened.get()));
  ASSERT_TRUE(EPDFLayer_BeginTransaction(reopened.get()));
  EXPECT_EQ(0u, CreateAnnot(reopened.get(), 1, 20));
  EXPECT_EQ(12u, CreateAnnot(reopened.get(), 1, 12));
  EXPECT_EQ(kRaised + 1, CreateAnnot(reopened.get(), 1, 0));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(reopened.get()));
  EXPECT_THAT(Annots(reopened.get(), 1), ElementsAre(20u, 12u, kRaised + 1u));
}

// A blank page at a chosen number is in the page list and the page tree at
// once, takes annotations that name it as their page, renders, and survives
// a save and reopen.
TEST_F(EPDFObjectNumberEmbedderTest, BlankPageTakesTheChosenNumber) {
  std::string artifact;
  {
    ScopedFPDFDocument doc = OpenLayer();
    ASSERT_TRUE(doc);
    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    ASSERT_TRUE(EPDFPage_InsertBlankRaw(doc.get(), 1, 200, 400, 10));
    EXPECT_EQ(3, FPDF_GetPageCount(doc.get()));
    EXPECT_EQ(10u, EPDFDoc_GetPageObjectNumberByIndex(doc.get(), 1));
    EXPECT_THAT(Kids(doc.get()), ElementsAre(kFirstPage, 10u, kSecondPage));
    EXPECT_EQ(3, PageTreeCount(doc.get()));
    EXPECT_EQ(11u, CreateAnnot(doc.get(), 1, 11));
    EXPECT_EQ(10u, ReferenceIn(doc.get(), 11, "P"));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));

    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 1));
    ASSERT_TRUE(page);
    EXPECT_FLOAT_EQ(200, FPDF_GetPageWidthF(page.get()));
    EXPECT_FLOAT_EQ(400, FPDF_GetPageHeightF(page.get()));
    EXPECT_EQ(0xFFFFFFu, ColorAt(page.get(), 100, 200));
    page.reset();
    artifact = SaveArtifact(doc.get());
  }

  ScopedFPDFDocument reopened = OpenArtifact(&artifact);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(3, FPDF_GetPageCount(reopened.get()));
  EXPECT_EQ(10u, EPDFDoc_GetPageObjectNumberByIndex(reopened.get(), 1));
  EXPECT_THAT(Kids(reopened.get()), ElementsAre(kFirstPage, 10u, kSecondPage));
  EXPECT_THAT(Annots(reopened.get(), 1), ElementsAre(11u));
  EXPECT_EQ(10u, ReferenceIn(reopened.get(), 11, "P"));
}

// A page index out of range or a number that isn't free inserts nothing.
TEST_F(EPDFObjectNumberEmbedderTest, BlankPageRefusalsInsertNothing) {
  ScopedFPDFDocument doc = OpenLayer();
  ASSERT_TRUE(doc);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  EXPECT_FALSE(EPDFPage_InsertBlankRaw(doc.get(), -1, 100, 100, 10));
  EXPECT_FALSE(EPDFPage_InsertBlankRaw(doc.get(), 3, 100, 100, 10));
  EXPECT_FALSE(EPDFPage_InsertBlankRaw(doc.get(), 0, 100, 100, kSecondPage));
  EXPECT_FALSE(EPDFPage_InsertBlankRaw(doc.get(), 0, 100, 100, kRaised + 1));
  EXPECT_EQ(2, FPDF_GetPageCount(doc.get()));
  EXPECT_THAT(Kids(doc.get()), ElementsAre(kFirstPage, kSecondPage));

  ASSERT_TRUE(EPDFPage_InsertBlankRaw(doc.get(), 2, 100, 100, 10));
  EXPECT_FALSE(EPDFPage_InsertBlankRaw(doc.get(), 0, 100, 100, 10));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  EXPECT_THAT(Kids(doc.get()), ElementsAre(kFirstPage, kSecondPage, 10u));
}

// A field at a chosen number with a widget at another. Parents created on the
// way get the next free numbers; a refused field creates none of them.
TEST_F(EPDFObjectNumberEmbedderTest, FieldAndWidgetTakeTheChosenNumbers) {
  ScopedFPDFDocument doc = OpenLayer();
  ASSERT_TRUE(doc);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  ASSERT_TRUE(CreateWidget(doc.get(), 1, 11));
  EXPECT_EQ(0u, CreateTextField(doc.get(), L"billing.name", 11));
  EXPECT_EQ(kRaised, EPDFLayer_GetLastObjectNumber(doc.get()));

  EXPECT_EQ(10u, CreateTextField(doc.get(), L"billing.name", 10));
  EXPECT_EQ(kRaised + 1, ReferenceIn(doc.get(), 10, "Parent"));
  // The field isn't merged: the split number goes unused and stays free.
  ASSERT_TRUE(EPDFForm_AttachWidget(doc.get(), 10, 11, nullptr, 12));
  EXPECT_EQ(12u, CreateAnnot(doc.get(), 0, 12));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));

  EXPECT_THAT(WidgetsOf(doc.get(), 10), ElementsAre(11u));
  EXPECT_EQ(2, CountFields(doc.get()));
}

// Attaching a second widget to a merged field splits it: the field keeps its
// number and its widget half moves to the chosen split number, naming its
// page.
TEST_F(EPDFObjectNumberEmbedderTest, MergedFieldSplitsToTheChosenNumber) {
  ScopedFPDFDocument doc = OpenLayer();
  ASSERT_TRUE(doc);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
  ASSERT_TRUE(CreateWidget(doc.get(), 0, 11));
  EXPECT_FALSE(
      EPDFForm_AttachWidget(doc.get(), kMergedField, 11, nullptr, kSecondPage));
  EXPECT_FALSE(EPDFForm_AttachWidget(doc.get(), kMergedField, 11, nullptr, 11));
  EXPECT_THAT(Annots(doc.get(), 0), ElementsAre(kMergedField, 11u));
  EXPECT_THAT(WidgetsOf(doc.get(), kMergedField), ElementsAre(kMergedField));

  ASSERT_TRUE(EPDFForm_AttachWidget(doc.get(), kMergedField, 11, nullptr, 12));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
  EXPECT_THAT(WidgetsOf(doc.get(), kMergedField), ElementsAre(12u, 11u));
  EXPECT_THAT(Annots(doc.get(), 0), UnorderedElementsAre(11u, 12u));
  EXPECT_EQ(kFirstPage, ReferenceIn(doc.get(), 12, "P"));
}
