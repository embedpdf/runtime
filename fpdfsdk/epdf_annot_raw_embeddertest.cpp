// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Annotation calls that work on a page's dictionaries without loading or
// parsing the page (the *Raw calls in public/epdf_annot.h).

#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fxcrt/retain_ptr.h"
#include "fpdfsdk/cpdfsdk_formfillenvironment.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_annot.h"
#include "public/epdf_view.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_formfill.h"
#include "public/fpdf_ppo.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/embedpdf_layer_mode.h"
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

// The object number of the page that the first annotation on page
// |page_index| names as its own (/P), or 0 when it names none.
uint32_t PageNamedByFirstAnnot(FPDF_DOCUMENT doc, int page_index) {
  RetainPtr<const CPDF_Dictionary> page =
      CPDFDocumentFromFPDFDocument(doc)->GetPageDictionary(page_index);
  RetainPtr<const CPDF_Array> annots = page->GetArrayFor("Annots");
  RetainPtr<const CPDF_Dictionary> annot =
      annots ? annots->GetDictAt(0) : nullptr;
  RetainPtr<const CPDF_Dictionary> named =
      annot ? annot->GetDictFor("P") : nullptr;
  return named ? named->GetObjNum() : 0;
}

// The page object the handle |annot| works on, kept alive by the caller.
RetainPtr<IPDF_Page> PageOf(FPDF_ANNOTATION annot) {
  return pdfium::WrapRetain(
      CPDFAnnotContextFromFPDFAnnotation(annot)->GetPage());
}

}  // namespace

class EPDFAnnotRawEmbedderTest : public EmbedderTest {};

TEST_F(EPDFAnnotRawEmbedderTest, FindsAnAnnotationByNameOnItsPage) {
  embedpdf_test::ScopedTestDocument doc(NewTestDocument());
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
      EPDFPage_CreateAnnotRaw(doc.get(), 1, FPDF_ANNOT_TEXT, 0));
  ASSERT_TRUE(raw);
  SetName(raw.get(), L"first");
  EXPECT_EQ(0, IndexByName(doc.get(), 1, L"first"));
}

TEST_F(EPDFAnnotRawEmbedderTest, FindsANameOnAnAnnotationWrittenInPlace) {
  embedpdf_test::ScopedTestDocument doc(NewTestDocument());
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
  embedpdf_test::ScopedTestDocument doc(NewTestDocument());
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
        EPDFPage_CreateAnnotRaw(doc.get(), 0, FPDF_ANNOT_SQUARE, 0));
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
  embedpdf_test::ScopedTestDocument doc(NewTestDocument());
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

// A new annotation names its page (/P), whichever call made it; a copy of the
// page names the copy.
TEST_F(EPDFAnnotRawEmbedderTest, NewAnnotationsNameTheirPage) {
  embedpdf_test::ScopedTestDocument doc(NewTestDocument());
  ASSERT_TRUE(doc);
  for (int index : {0, 1}) {
    ScopedFPDFPage page(FPDFPage_New(doc.get(), index, 612, 792));
    ASSERT_TRUE(page);
  }
  {
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_SQUARE));
    ASSERT_TRUE(annot);
  }
  {
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnotRaw(doc.get(), 1, FPDF_ANNOT_TEXT, 0));
    ASSERT_TRUE(annot);
  }
  EXPECT_EQ(EPDFDoc_GetPageObjectNumberByIndex(doc.get(), 0),
            PageNamedByFirstAnnot(doc.get(), 0));
  EXPECT_EQ(EPDFDoc_GetPageObjectNumberByIndex(doc.get(), 1),
            PageNamedByFirstAnnot(doc.get(), 1));

  embedpdf_test::ScopedTestDocument copy(NewTestDocument());
  ASSERT_TRUE(copy);
  const int second_page[] = {1};
  ASSERT_TRUE(
      FPDF_ImportPagesByIndex(copy.get(), doc.get(), second_page, 1, 0));
  const unsigned int copied_page =
      EPDFDoc_GetPageObjectNumberByIndex(copy.get(), 0);
  ASSERT_NE(0u, copied_page);
  EXPECT_EQ(copied_page, PageNamedByFirstAnnot(copy.get(), 0));
}

// A handle opened without a page is built on a page object of its own, and
// closing the handle frees it. Each handle used to leak that page.
TEST_F(EPDFAnnotRawEmbedderTest, ClosingAHandleFreesItsPage) {
  embedpdf_test::ScopedTestDocument doc(NewTestDocument());
  ASSERT_TRUE(doc);
  {
    ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 612, 792));
    ASSERT_TRUE(page);
  }

  FPDF_ANNOTATION created =
      EPDFPage_CreateAnnotRaw(doc.get(), 0, FPDF_ANNOT_SQUARE, 0);
  ASSERT_TRUE(created);
  RetainPtr<IPDF_Page> created_page = PageOf(created);
  EXPECT_FALSE(created_page->HasOneRef());  // the handle holds it too
  FPDFPage_CloseAnnot(created);
  EXPECT_TRUE(created_page->HasOneRef());

  FPDF_ANNOTATION opened = EPDFPage_GetAnnotRaw(doc.get(), 0, 0);
  ASSERT_TRUE(opened);
  RetainPtr<IPDF_Page> opened_page = PageOf(opened);
  EXPECT_FALSE(opened_page->HasOneRef());
  FPDFPage_CloseAnnot(opened);
  EXPECT_TRUE(opened_page->HasOneRef());
}

// A linked handle works on the page of the handle it came from. When that
// handle owns the page the linked one shares it, so either can close first.
TEST_F(EPDFAnnotRawEmbedderTest, ALinkedHandleSharesThePage) {
  embedpdf_test::ScopedTestDocument doc(NewTestDocument());
  ASSERT_TRUE(doc);
  {
    ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 612, 792));
    ASSERT_TRUE(page);
  }
  {
    ScopedFPDFAnnotation note(
        EPDFPage_CreateAnnotRaw(doc.get(), 0, FPDF_ANNOT_TEXT, 0));
    ASSERT_TRUE(note);
    ScopedFPDFAnnotation popup(
        EPDFPage_CreateAnnotRaw(doc.get(), 0, FPDF_ANNOT_POPUP, 0));
    ASSERT_TRUE(popup);
    ASSERT_TRUE(EPDFAnnot_SetLinkedAnnot(note.get(), "Popup", popup.get()));
  }

  FPDF_ANNOTATION note = EPDFPage_GetAnnotRaw(doc.get(), 0, 0);
  ASSERT_TRUE(note);
  FPDF_ANNOTATION popup = FPDFAnnot_GetLinkedAnnot(note, "Popup");
  ASSERT_TRUE(popup);
  RetainPtr<IPDF_Page> page = PageOf(note);
  EXPECT_EQ(page.Get(), PageOf(popup).Get());

  // The handle it came from closes first: the popup's still has its page.
  FPDFPage_CloseAnnot(note);
  EXPECT_FALSE(page->HasOneRef());
  EXPECT_EQ(FPDF_ANNOT_POPUP, FPDFAnnot_GetSubtype(popup));
  FPDFPage_CloseAnnot(popup);
  EXPECT_TRUE(page->HasOneRef());

  // On a loaded page both handles borrow the page their caller keeps open.
  ScopedFPDFPage loaded(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(loaded);
  ScopedFPDFAnnotation loaded_note(FPDFPage_GetAnnot(loaded.get(), 0));
  ASSERT_TRUE(loaded_note);
  ScopedFPDFAnnotation loaded_popup(
      FPDFAnnot_GetLinkedAnnot(loaded_note.get(), "Popup"));
  ASSERT_TRUE(loaded_popup);
  EXPECT_TRUE(CPDFPageFromFPDFPage(loaded.get())->HasOneRef());
}

// The form-fill calls work on a page the form environment shows. A handle
// opened without a page is on none: they fail for it, and make no view for
// the page it owns, which that page would not outlive.
TEST_F(EPDFAnnotRawEmbedderTest,
       FormFillCallsMakeNoViewForAHandleWithoutAPage) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  CPDFSDK_FormFillEnvironment* environment =
      CPDFSDKFormFillEnvironmentFromFPDFFormHandle(form_handle());
  ASSERT_TRUE(environment);

  ScopedFPDFAnnotation field(EPDFPage_GetAnnotRaw(document(), 0, 0));
  ASSERT_TRUE(field);
  ASSERT_EQ(FPDF_ANNOT_WIDGET, FPDFAnnot_GetSubtype(field.get()));
  RetainPtr<IPDF_Page> page = PageOf(field.get());

  EXPECT_FALSE(FORM_SetFocusedAnnot(form_handle(), field.get()));
  EXPECT_FALSE(environment->GetPageView(page.Get()));
  EXPECT_EQ(FPDF_TEXTDIR_UNKNOWN,
            FORM_GetTextDirection(form_handle(), field.get()));
  EXPECT_FALSE(
      FORM_SetTextDirection(form_handle(), field.get(), FPDF_TEXTDIR_RTL));
  EXPECT_FALSE(environment->GetPageView(page.Get()));
}
