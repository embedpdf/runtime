// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Annotation calls that work on a page's dictionaries without loading or
// parsing the page (the *Raw calls in public/fpdf_annot.h).

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

// Name `annot` `nm`, as an annotation's /NM is written.
void SetName(FPDF_ANNOTATION annot, const wchar_t* nm) {
  ScopedFPDFWideString text = GetFPDFWideString(nm);
  ASSERT_TRUE(FPDFAnnot_SetStringValue(annot, "NM", text.get()));
}

int IndexByName(FPDF_DOCUMENT doc, int page_index, const wchar_t* nm) {
  ScopedFPDFWideString text = GetFPDFWideString(nm);
  return EPDFPage_GetAnnotIndexByNameRaw(doc, page_index, text.get());
}

}  // namespace

class EPDFAnnotRawEmbedderTest : public EmbedderTest {};

TEST_F(EPDFAnnotRawEmbedderTest, FindsAnAnnotationByNameOnItsPage) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  for (int index : {0, 1}) {
    ScopedFPDFPage page(FPDFPage_New(doc.get(), index, 612, 792));
    ASSERT_TRUE(page);
  }
  {
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
    for (const wchar_t* nm : {L"first", L"Café · review", L"first"}) {
      ScopedFPDFAnnotation annot(
          FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_SQUARE));
      ASSERT_TRUE(annot);
      SetName(annot.get(), nm);
    }
    ScopedFPDFAnnotation unnamed(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_SQUARE));
    ASSERT_TRUE(unnamed);
  }

  // No page is loaded now: the names are read from the dictionaries.
  EXPECT_EQ(0, IndexByName(doc.get(), 0, L"first"));
  EXPECT_EQ(1, IndexByName(doc.get(), 0, L"Café · review"));
  EXPECT_EQ(-1, IndexByName(doc.get(), 0, L"First"));
  EXPECT_EQ(-1, IndexByName(doc.get(), 1, L"first"));

  // A name given to an annotation made without loading its page is found.
  ScopedFPDFAnnotation raw(
      EPDFPage_CreateAnnotRaw(doc.get(), 1, FPDF_ANNOT_TEXT));
  ASSERT_TRUE(raw);
  SetName(raw.get(), L"first");
  EXPECT_EQ(0, IndexByName(doc.get(), 1, L"first"));
}

TEST_F(EPDFAnnotRawEmbedderTest, FindsANameOnAnAnnotationWrittenInPlace) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 612, 792));
  ASSERT_TRUE(page);
  // An annotation dictionary written directly in /Annots, not an object of
  // its own, as some writers do.
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());
  RetainPtr<CPDF_Array> annots =
      pdf->GetMutablePageDictionary(0)->SetNewFor<CPDF_Array>("Annots");
  RetainPtr<CPDF_Dictionary> direct = annots->AppendNew<CPDF_Dictionary>();
  direct->SetNewFor<CPDF_Name>("Subtype", "Square");
  direct->SetNewFor<CPDF_String>("NM", "inline");
  EXPECT_EQ(0, IndexByName(doc.get(), 0, L"inline"));
}

TEST_F(EPDFAnnotRawEmbedderTest, FindsAnAnnotationByObjectNumber) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 612, 792));
  ASSERT_TRUE(page);
  page.reset();
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());
  // One written in place in /Annots, then two objects of their own.
  RetainPtr<CPDF_Array> annots =
      pdf->GetMutablePageDictionary(0)->SetNewFor<CPDF_Array>("Annots");
  annots->AppendNew<CPDF_Dictionary>()->SetNewFor<CPDF_Name>("Subtype",
                                                             "Square");
  unsigned int numbers[2] = {};
  for (unsigned int& number : numbers) {
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnotRaw(doc.get(), 0, FPDF_ANNOT_SQUARE));
    ASSERT_TRUE(annot);
    number = static_cast<unsigned int>(EPDFAnnot_GetObjectNumber(annot.get()));
    ASSERT_NE(0u, number);
  }
  EXPECT_EQ(1,
            EPDFPage_GetAnnotIndexByObjectNumberRaw(doc.get(), 0, numbers[0]));
  EXPECT_EQ(2,
            EPDFPage_GetAnnotIndexByObjectNumberRaw(doc.get(), 0, numbers[1]));
  EXPECT_EQ(-1, EPDFPage_GetAnnotIndexByObjectNumberRaw(doc.get(), 0, 0));
  EXPECT_EQ(-1, EPDFPage_GetAnnotIndexByObjectNumberRaw(doc.get(), 0, 99999));
  EXPECT_EQ(-1,
            EPDFPage_GetAnnotIndexByObjectNumberRaw(doc.get(), 1, numbers[0]));
  EXPECT_EQ(-1,
            EPDFPage_GetAnnotIndexByObjectNumberRaw(nullptr, 0, numbers[0]));
}

TEST_F(EPDFAnnotRawEmbedderTest, FindsNothingWhereThereIsNothing) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 612, 792));
  ASSERT_TRUE(page);
  EXPECT_EQ(-1, IndexByName(doc.get(), 0, L"first"));  // no /Annots
  EXPECT_EQ(-1, IndexByName(doc.get(), 0, L""));
  EXPECT_EQ(-1, IndexByName(doc.get(), 1, L"first"));
  EXPECT_EQ(-1, IndexByName(doc.get(), -1, L"first"));
  EXPECT_EQ(-1, IndexByName(nullptr, 0, L"first"));
  EXPECT_EQ(-1, EPDFPage_GetAnnotIndexByNameRaw(doc.get(), 0, nullptr));
}
