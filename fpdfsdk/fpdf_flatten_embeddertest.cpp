// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "build/build_config.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fxge/cfx_defaultrenderdevice.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/epdf_form.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_flatten.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/embedder_test_constants.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/test_loader.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

#include <set>
#include <string>
#include <vector>

using testing::HasSubstr;
using testing::Not;

namespace {

class FPDFFlattenEmbedderTest : public EmbedderTest {
 protected:
  struct LayerDocument {
    std::vector<uint8_t> bytes;
    EPDF_BASE_DOCUMENT base = nullptr;
    FPDF_DOCUMENT layer = nullptr;

    ~LayerDocument() {
      if (layer) {
        FPDF_CloseDocument(layer);
      }
      if (base) {
        EPDF_ReleaseBaseDocument(base);
      }
    }
  };

  bool OpenLayer(const char* file_name, LayerDocument* out) {
    const std::string path = PathService::GetTestFilePath(file_name);
    if (path.empty()) {
      return false;
    }
    out->bytes = GetFileContents(path.c_str());
    if (out->bytes.empty()) {
      return false;
    }
    out->base = EPDF_LoadMemBaseDocument(
        out->bytes.data(), static_cast<int>(out->bytes.size()), nullptr);
    if (!out->base) {
      return false;
    }
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    out->layer = EPDFLayer_OpenLayer(out->base, nullptr, nullptr, &status);
    return out->layer && status == EPDFLayerOpenStatus_kSuccess;
  }
};

// The removed single-annotation entry point, expressed through the set API:
// every existing single-target test keeps its exact expectations.
int FlattenOne(FPDF_PAGE page, FPDF_ANNOTATION annot, int usage) {
  FPDF_ANNOTATION set[] = {annot};
  return EPDFPage_FlattenAnnotations(page, set, 1, usage, nullptr);
}

}  // namespace

TEST_F(FPDFFlattenEmbedderTest, FlatNothing) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  EXPECT_TRUE(page);
  EXPECT_EQ(FLATTEN_NOTHINGTODO,
            FPDFPage_Flatten(page.get(), FLAT_NORMALDISPLAY));
}

TEST_F(FPDFFlattenEmbedderTest, FlatNormal) {
  ASSERT_TRUE(OpenDocument("annotiter.pdf"));
  ScopedPage page = LoadScopedPage(0);
  EXPECT_TRUE(page);
  EXPECT_EQ(FLATTEN_SUCCESS, FPDFPage_Flatten(page.get(), FLAT_NORMALDISPLAY));
}

TEST_F(FPDFFlattenEmbedderTest, FlatPrint) {
  ASSERT_TRUE(OpenDocument("annotiter.pdf"));
  ScopedPage page = LoadScopedPage(0);
  EXPECT_TRUE(page);
  EXPECT_EQ(FLATTEN_SUCCESS, FPDFPage_Flatten(page.get(), FLAT_PRINT));
}

TEST_F(FPDFFlattenEmbedderTest, FlattenSpecificAnnotationByHandle) {
  ASSERT_TRUE(OpenDocument("flatten_selective.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(6, FPDFPage_GetAnnotCount(page.get()));

  ScopedFPDFAnnotation target(EPDFPage_GetAnnotByObjectNumber(page.get(), 4u));
  ASSERT_TRUE(target);
  EXPECT_EQ(FLATTEN_FAIL, FlattenOne(page.get(), target.get(), 99));
  EXPECT_EQ(FLATTEN_FAIL, FlattenOne(page.get(), nullptr, FLAT_NORMALDISPLAY));

  ScopedFPDFAnnotation hidden(EPDFPage_GetAnnotByObjectNumber(page.get(), 5u));
  ASSERT_TRUE(hidden);
  EXPECT_EQ(FLATTEN_NOTHINGTODO,
            FlattenOne(page.get(), hidden.get(), FLAT_NORMALDISPLAY));
  EXPECT_EQ(6, FPDFPage_GetAnnotCount(page.get()));

  ASSERT_EQ(FLATTEN_SUCCESS,
            FlattenOne(page.get(), target.get(), FLAT_NORMALDISPLAY));
  EXPECT_EQ(5, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(page.get(), 4u));
  ScopedFPDFAnnotation preserved(
      EPDFPage_GetAnnotByObjectNumber(page.get(), 5u));
  EXPECT_TRUE(preserved);

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ASSERT_TRUE(OpenSavedDocument());
  FPDF_PAGE saved_page = LoadSavedPage(0);
  ASSERT_TRUE(saved_page);
  EXPECT_EQ(5, FPDFPage_GetAnnotCount(saved_page));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(saved_page, 4u));
  CloseSavedPage(saved_page);
}

TEST_F(FPDFFlattenEmbedderTest,
       FlattenPagePreservesUnpaintedAnnotationsAndDetachesWidget) {
  ASSERT_TRUE(OpenDocument("flatten_selective.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ASSERT_EQ(FLATTEN_SUCCESS, EPDFPage_Flatten(page.get(), FLAT_NORMALDISPLAY));
  // The Text annotation (object 6) has no appearance, and loading the page
  // draws its icon in memory without writing one: like every annotation with
  // nothing to paint, it stays.
  EXPECT_EQ(3, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(page.get(), 4u));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(page.get(), 13u));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(page.get(), 16u));
  for (unsigned int object_number : {5u, 6u, 9u}) {
    ScopedFPDFAnnotation annotation(
        EPDFPage_GetAnnotByObjectNumber(page.get(), object_number));
    EXPECT_TRUE(annotation) << object_number;
  }

  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(pdf);
  RetainPtr<const CPDF_Dictionary> page_dictionary = pdf->GetPageDictionary(0);
  ASSERT_TRUE(page_dictionary);
  EXPECT_TRUE(page_dictionary->KeyExist("MediaBox"));
  EXPECT_TRUE(page_dictionary->KeyExist("CropBox"));
  RetainPtr<const CPDF_Dictionary> resources =
      page_dictionary->GetDictFor("Resources");
  ASSERT_TRUE(resources);
  EXPECT_TRUE(resources->KeyExist("ExtGState"));
  EXPECT_TRUE(resources->KeyExist("XObject"));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  ASSERT_EQ(1, EPDFForm_CountFields(model));
  EXPECT_EQ(0, EPDFForm_CountFieldWidgets(model, 0));
  EPDFForm_CloseModel(model);
}

TEST_F(FPDFFlattenEmbedderTest, FlattenPageHonorsPrintUsage) {
  ASSERT_TRUE(OpenDocument("flatten_selective.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(FLATTEN_SUCCESS, EPDFPage_Flatten(page.get(), FLAT_PRINT));
  EXPECT_EQ(4, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(page.get(), 4u));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(page.get(), 13u));
  ScopedFPDFAnnotation normal_only(
      EPDFPage_GetAnnotByObjectNumber(page.get(), 16u));
  EXPECT_TRUE(normal_only);
}

TEST_F(FPDFFlattenEmbedderTest, FlattenMergedWidgetRemovesFieldTreeEntry) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation widget(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(widget);
  ASSERT_EQ(4u, EPDFAnnot_GetObjectNumber(widget.get()));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(widget.get()));

  ASSERT_EQ(FLATTEN_SUCCESS,
            FlattenOne(page.get(), widget.get(), FLAT_NORMALDISPLAY));
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(0, EPDFForm_CountFields(model));
  EPDFForm_CloseModel(model);
}

TEST_F(FPDFFlattenEmbedderTest, FlattenPageIsLayerSafeAndDeltaDurable) {
  LayerDocument document;
  ASSERT_TRUE(OpenLayer("flatten_selective.pdf", &document));
  ASSERT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(document.layer));

  ScopedFPDFPage page(FPDF_LoadPage(document.layer, 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation hidden(EPDFPage_GetAnnotByObjectNumber(page.get(), 5u));
  ASSERT_TRUE(hidden);
  EXPECT_EQ(FLATTEN_NOTHINGTODO,
            FlattenOne(page.get(), hidden.get(), FLAT_NORMALDISPLAY));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(document.layer));

  ASSERT_EQ(FLATTEN_SUCCESS, EPDFPage_Flatten(page.get(), FLAT_NORMALDISPLAY));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(document.layer, 3u));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(document.layer, 12u));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(document.layer, 13u));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(document.layer, 4u));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(document.layer, 7u));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(document.layer, 14u));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(document.layer, 15u));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(document.layer, 17u));

  EXPECT_EQ(3, FPDFPage_GetAnnotCount(page.get()));

  ClearString();
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(document.layer, this, &save_status));
  ASSERT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  const std::string delta = GetString();
  ASSERT_FALSE(delta.empty());

  TestLoader loader(pdfium::as_bytes(pdfium::span(delta.data(), delta.size())));
  FPDF_FILEACCESS access = {};
  access.m_FileLen = static_cast<unsigned long>(delta.size());
  access.m_GetBlock = TestLoader::GetBlock;
  access.m_Param = &loader;
  EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument replay(
      EPDFLayer_OpenLayer(document.base, &access, nullptr, &open_status));
  ASSERT_TRUE(replay);
  ASSERT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);

  ScopedFPDFPage replay_page(FPDF_LoadPage(replay.get(), 0));
  ASSERT_TRUE(replay_page);
  EXPECT_EQ(3, FPDFPage_GetAnnotCount(replay_page.get()));
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(replay.get());
  ASSERT_TRUE(model);
  ASSERT_EQ(1, EPDFForm_CountFields(model));
  EXPECT_EQ(0, EPDFForm_CountFieldWidgets(model, 0));
  EPDFForm_CloseModel(model);
}

TEST_F(FPDFFlattenEmbedderTest, FlattenReadsAlreadyPromotedAnnotationState) {
  LayerDocument document;
  ASSERT_TRUE(OpenLayer("flatten_selective.pdf", &document));
  ScopedFPDFPage page(FPDF_LoadPage(document.layer, 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation target(EPDFPage_GetAnnotByObjectNumber(page.get(), 4u));
  ASSERT_TRUE(target);
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document.layer);
  ASSERT_TRUE(pdf);
  RetainPtr<CPDF_Dictionary> annotation =
      ToDictionary(pdf->GetMutableIndirectObject(4u));
  ASSERT_TRUE(annotation);
  annotation->SetNewFor<CPDF_Number>("F", FPDF_ANNOT_FLAG_HIDDEN);
  ASSERT_TRUE(EPDFLayer_IsObjectPromoted(document.layer, 4u));
  ASSERT_FALSE(EPDFLayer_IsObjectPromoted(document.layer, 3u));

  EXPECT_EQ(FLATTEN_NOTHINGTODO,
            FlattenOne(page.get(), target.get(), FLAT_NORMALDISPLAY));
  EXPECT_EQ(1ul, EPDFLayer_GetPromotedObjectCount(document.layer));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(document.layer, 3u));
}

TEST_F(FPDFFlattenEmbedderTest, FlattenDirectAnnotationByHandle) {
  ScopedFPDFDocument document(FPDF_CreateNewDocument());
  ASSERT_TRUE(document);
  ScopedFPDFPage page(FPDFPage_New(document.get(), 0, 100, 100));
  ASSERT_TRUE(page);
  ScopedFPDFPage other_page(FPDFPage_New(document.get(), 1, 100, 100));
  ASSERT_TRUE(other_page);

  ScopedFPDFAnnotation annotation(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_INK));
  ASSERT_TRUE(annotation);
  ASSERT_EQ(0u, EPDFAnnot_GetObjectNumber(annotation.get()));
  const FS_RECTF rectangle = {10.0f, 40.0f, 40.0f, 10.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annotation.get(), &rectangle));
  ScopedFPDFWideString appearance = GetFPDFWideString(L"0 0 10 10 re f");
  ASSERT_TRUE(FPDFAnnot_SetAP(
      annotation.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, appearance.get()));

  EXPECT_EQ(FLATTEN_FAIL,
            FlattenOne(other_page.get(), annotation.get(), FLAT_NORMALDISPLAY));
  ASSERT_EQ(FLATTEN_SUCCESS,
            FlattenOne(page.get(), annotation.get(), FLAT_NORMALDISPLAY));
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
}

TEST_F(FPDFFlattenEmbedderTest, FlatWithBadFont) {
  ASSERT_TRUE(OpenDocument("344775293.pdf"));
  ScopedPage page = LoadScopedPage(0);
  EXPECT_TRUE(page);

  FORM_OnLButtonDown(form_handle(), page.get(), 0, 20, 30);
  FORM_OnLButtonUp(form_handle(), page.get(), 0, 20, 30);

  EXPECT_EQ(FLATTEN_SUCCESS, FPDFPage_Flatten(page.get(), FLAT_PRINT));
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  EXPECT_THAT(GetString(), Not(HasSubstr("/PDFDocEncoding")));
}

TEST_F(FPDFFlattenEmbedderTest, FlatWithFontNoBaseEncoding) {
  ASSERT_TRUE(OpenDocument("363015187.pdf"));
  ScopedPage page = LoadScopedPage(0);
  EXPECT_TRUE(page);

  EXPECT_EQ(FLATTEN_SUCCESS, FPDFPage_Flatten(page.get(), FLAT_PRINT));
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  EXPECT_THAT(GetString(), HasSubstr("/Differences"));
}

TEST_F(FPDFFlattenEmbedderTest, Bug861842) {
  ASSERT_TRUE(OpenDocument("bug_861842.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
  CompareBitmapWithExpectationSuffix(bitmap.get(), "bug_861842");

  EXPECT_EQ(FLATTEN_SUCCESS, FPDFPage_Flatten(page.get(), FLAT_PRINT));
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  // TODO(crbug.com/861842): This should not render blank.
  VerifySavedDocumentWithExpectationSuffix("blank_100x120");
}

TEST_F(FPDFFlattenEmbedderTest, Bug889099) {
  ASSERT_TRUE(OpenDocument("bug_889099.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // The original document has a malformed media box; the height is -400.
  ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
  CompareBitmapWithExpectationSuffix(bitmap.get(), "bug_889099");

  EXPECT_EQ(FLATTEN_SUCCESS, FPDFPage_Flatten(page.get(), FLAT_PRINT));
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  VerifySavedDocumentWithExpectationSuffix("bug_889099_flattened");
}

TEST_F(FPDFFlattenEmbedderTest, Bug890322) {
  ASSERT_TRUE(OpenDocument("bug_890322.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
  CompareBitmapWithExpectationSuffix(bitmap.get(), pdfium::kBug890322Png);

  EXPECT_EQ(FLATTEN_SUCCESS, FPDFPage_Flatten(page.get(), FLAT_PRINT));
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  VerifySavedDocumentWithExpectationSuffix(pdfium::kBug890322Png);
}

TEST_F(FPDFFlattenEmbedderTest, Bug896366) {
  ASSERT_TRUE(OpenDocument("bug_896366.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
  CompareBitmapWithExpectationSuffix(bitmap.get(), "bug_896366");

  EXPECT_EQ(FLATTEN_SUCCESS, FPDFPage_Flatten(page.get(), FLAT_PRINT));
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  VerifySavedDocumentWithExpectationSuffix("bug_896366");
}

TEST_F(FPDFFlattenEmbedderTest, FlattenAnnotationSetReportsPerEntry) {
  ASSERT_TRUE(OpenDocument("flatten_selective.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(6, FPDFPage_GetAnnotCount(page.get()));

  // 4: visible square; 5: hidden square; 9: print-only square (invisible
  // for display, so skipped).
  ScopedFPDFAnnotation visible(EPDFPage_GetAnnotByObjectNumber(page.get(), 4u));
  ScopedFPDFAnnotation hidden(EPDFPage_GetAnnotByObjectNumber(page.get(), 5u));
  ScopedFPDFAnnotation print_only(
      EPDFPage_GetAnnotByObjectNumber(page.get(), 9u));
  ASSERT_TRUE(visible);
  ASSERT_TRUE(hidden);
  ASSERT_TRUE(print_only);

  int status = -1;
  EXPECT_EQ(FLATTEN_FAIL,
            EPDFPage_FlattenAnnotations(page.get(), nullptr, 1,
                                        FLAT_NORMALDISPLAY, &status));
  FPDF_ANNOTATION just_visible[] = {visible.get()};
  EXPECT_EQ(FLATTEN_FAIL,
            EPDFPage_FlattenAnnotations(page.get(), just_visible, 0,
                                        FLAT_NORMALDISPLAY, &status));
  EXPECT_EQ(FLATTEN_FAIL, EPDFPage_FlattenAnnotations(page.get(), just_visible,
                                                      1, 99, &status));
  FPDF_ANNOTATION null_entry[] = {nullptr};
  EXPECT_EQ(FLATTEN_FAIL,
            EPDFPage_FlattenAnnotations(page.get(), null_entry, 1,
                                        FLAT_NORMALDISPLAY, &status));
  EXPECT_EQ(EPDF_FLATTEN_STATUS_NOT_ON_PAGE, status);
  EXPECT_EQ(6, FPDFPage_GetAnnotCount(page.get()));

  FPDF_ANNOTATION set[] = {hidden.get(), visible.get(), print_only.get()};
  int statuses[3] = {-1, -1, -1};
  ASSERT_EQ(FLATTEN_SUCCESS,
            EPDFPage_FlattenAnnotations(page.get(), set, 3, FLAT_NORMALDISPLAY,
                                        statuses));
  EXPECT_EQ(EPDF_FLATTEN_STATUS_SKIPPED, statuses[0]);
  EXPECT_EQ(EPDF_FLATTEN_STATUS_APPLIED, statuses[1]);
  EXPECT_EQ(EPDF_FLATTEN_STATUS_SKIPPED, statuses[2]);

  // Exactly the applied one left the page; the others stay, untouched.
  EXPECT_EQ(5, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(page.get(), 4u));
  EXPECT_TRUE(
      ScopedFPDFAnnotation(EPDFPage_GetAnnotByObjectNumber(page.get(), 5u)));
  EXPECT_TRUE(
      ScopedFPDFAnnotation(EPDFPage_GetAnnotByObjectNumber(page.get(), 9u)));
}

TEST_F(FPDFFlattenEmbedderTest, FlattenAnnotationSetRejectsForeignAnnotation) {
  ASSERT_TRUE(OpenDocument("flatten_selective.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // An annotation from ANOTHER document: the whole call fails, nothing moves.
  ScopedFPDFDocument other(FPDF_CreateNewDocument());
  ScopedFPDFPage other_page(FPDFPage_New(other.get(), 0, 100, 100));
  ScopedFPDFAnnotation foreign(
      FPDFPage_CreateAnnot(other_page.get(), FPDF_ANNOT_SQUARE));
  ASSERT_TRUE(foreign);
  ScopedFPDFAnnotation visible(EPDFPage_GetAnnotByObjectNumber(page.get(), 4u));
  ASSERT_TRUE(visible);

  FPDF_ANNOTATION set[] = {visible.get(), foreign.get()};
  int statuses[2] = {-1, -1};
  EXPECT_EQ(FLATTEN_FAIL,
            EPDFPage_FlattenAnnotations(page.get(), set, 2, FLAT_NORMALDISPLAY,
                                        statuses));
  EXPECT_EQ(EPDF_FLATTEN_STATUS_SKIPPED, statuses[0]);
  EXPECT_EQ(EPDF_FLATTEN_STATUS_NOT_ON_PAGE, statuses[1]);
  EXPECT_EQ(6, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_TRUE(
      ScopedFPDFAnnotation(EPDFPage_GetAnnotByObjectNumber(page.get(), 4u)));
}

namespace {

// A square annotation with a hand-written normal appearance at |rect|: |ap|
// draws in a 0..10 BBox; |matrix| (optional) becomes the form's /Matrix.
ScopedFPDFAnnotation MakeSquareWithAppearance(FPDF_PAGE page,
                                              const FS_RECTF& rect,
                                              const wchar_t* ap,
                                              const CFX_Matrix* matrix) {
  ScopedFPDFAnnotation annot(FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE));
  if (!annot || !FPDFAnnot_SetRect(annot.get(), &rect)) {
    return nullptr;
  }
  ScopedFPDFWideString stream = GetFPDFWideString(ap);
  if (!FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                       stream.get())) {
    return nullptr;
  }
  CPDF_AnnotContext* ctx = CPDFAnnotContextFromFPDFAnnotation(annot.get());
  RetainPtr<CPDF_Dictionary> ap_dict =
      ctx->GetMutableAnnotDict()->GetMutableDictFor("AP");
  RetainPtr<CPDF_Stream> normal = ap_dict->GetMutableStreamFor("N");
  if (!normal) {
    return nullptr;
  }
  normal->GetMutableDict()->SetRectFor("BBox", CFX_FloatRect(0, 0, 10, 10));
  if (matrix) {
    normal->GetMutableDict()->SetMatrixFor("Matrix", *matrix);
  }
  return annot;
}

ScopedFPDFBitmap WhiteBitmap(int width, int height) {
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(width, height, 0));
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);
  return bitmap;
}

}  // namespace

TEST_F(FPDFFlattenEmbedderTest, ExportAnnotationsMatchesSourceCrop) {
  // Blank source page: a render of the exported page must equal a crop of
  // the source page rendered WITH annotations, pixel for pixel.
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 300, 300));
  ASSERT_TRUE(page);

  // Rect ≠ BBox (scaled 4×), a rotated /Matrix, and a plain one: the three
  // placement cases the ISO algorithm must get right.
  const CFX_Matrix rotate(0.0f, 1.0f, -1.0f, 0.0f, 10.0f, 0.0f);  // 90° CCW
  ScopedFPDFAnnotation a = MakeSquareWithAppearance(
      page.get(), {20.0f, 80.0f, 60.0f, 40.0f},
      L"1 0 0 rg 0 0 10 5 re f 0 0 1 rg 0 5 5 5 re f", nullptr);
  ScopedFPDFAnnotation b = MakeSquareWithAppearance(
      page.get(), {100.0f, 100.0f, 140.0f, 60.0f},
      L"0 1 0 rg 0 0 10 5 re f 0 0 0 rg 5 5 5 5 re f", &rotate);
  ScopedFPDFAnnotation c =
      MakeSquareWithAppearance(page.get(), {70.0f, 200.0f, 90.0f, 180.0f},
                               L"0 0 1 RG 2 w 1 1 8 8 re S", nullptr);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  ASSERT_TRUE(c);
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  FPDF_ANNOTATION set[] = {a.get(), b.get(), c.get()};
  ScopedFPDFDocument exported(
      EPDFPage_ExportAnnotationsAsDocument(page.get(), set, 3));
  ASSERT_TRUE(exported);
  ASSERT_EQ(1, FPDF_GetPageCount(exported.get()));
  ScopedFPDFPage exported_page(FPDF_LoadPage(exported.get(), 0));
  ASSERT_TRUE(exported_page);

  // The union of the three rects: x 20..140, y 40..200.
  EXPECT_FLOAT_EQ(120.0f,
                  static_cast<float>(FPDF_GetPageWidthF(exported_page.get())));
  EXPECT_FLOAT_EQ(160.0f,
                  static_cast<float>(FPDF_GetPageHeightF(exported_page.get())));
  // The source is untouched.
  EXPECT_EQ(3, FPDFPage_GetAnnotCount(page.get()));

  // Source crop (x 20..140, y 40..200 → device 120×160, top-left origin) with
  // annotations, against the exported page rendered plainly.
  ScopedFPDFBitmap crop = WhiteBitmap(120, 160);
  const FS_MATRIX to_crop = {1, 0, 0, 1, -20.0f, -(300.0f - 200.0f)};
  const FS_RECTF clip = {0, 0, 120, 160};
  FPDF_RenderPageBitmapWithMatrix(crop.get(), page.get(), &to_crop, &clip,
                                  FPDF_ANNOT);
  ScopedFPDFBitmap out = RenderPage(exported_page.get());
  ASSERT_TRUE(out);
  EXPECT_EQ(120, FPDFBitmap_GetWidth(out.get()));
  EXPECT_EQ(160, FPDFBitmap_GetHeight(out.get()));
  EXPECT_EQ(HashBitmap(crop.get()), HashBitmap(out.get()));
  ScopedFPDFBitmap blank = WhiteBitmap(120, 160);
  EXPECT_NE(HashBitmap(blank.get()), HashBitmap(out.get()));
}

TEST_F(FPDFFlattenEmbedderTest, ExportAnnotationsIsAllOrNothing) {
  ASSERT_TRUE(OpenDocument("flatten_selective.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation visible(EPDFPage_GetAnnotByObjectNumber(page.get(), 4u));
  ScopedFPDFAnnotation hidden(EPDFPage_GetAnnotByObjectNumber(page.get(), 5u));
  ScopedFPDFAnnotation print_only(
      EPDFPage_GetAnnotByObjectNumber(page.get(), 9u));
  ASSERT_TRUE(visible);
  ASSERT_TRUE(hidden);
  ASSERT_TRUE(print_only);

  FPDF_ANNOTATION with_hidden[] = {visible.get(), hidden.get()};
  EXPECT_FALSE(
      EPDFPage_ExportAnnotationsAsDocument(page.get(), with_hidden, 2));
  FPDF_ANNOTATION with_invisible[] = {visible.get(), print_only.get()};
  EXPECT_FALSE(
      EPDFPage_ExportAnnotationsAsDocument(page.get(), with_invisible, 2));
  FPDF_ANNOTATION null_entry[] = {nullptr};
  EXPECT_FALSE(EPDFPage_ExportAnnotationsAsDocument(page.get(), null_entry, 1));
  EXPECT_FALSE(
      EPDFPage_ExportAnnotationsAsDocument(page.get(), with_hidden, 0));

  FPDF_ANNOTATION ok[] = {visible.get()};
  ScopedFPDFDocument exported(
      EPDFPage_ExportAnnotationsAsDocument(page.get(), ok, 1));
  ASSERT_TRUE(exported);
  ScopedFPDFPage exported_page(FPDF_LoadPage(exported.get(), 0));
  ASSERT_TRUE(exported_page);
  EXPECT_FLOAT_EQ(30.0f,
                  static_cast<float>(FPDF_GetPageWidthF(exported_page.get())));
  EXPECT_FLOAT_EQ(30.0f,
                  static_cast<float>(FPDF_GetPageHeightF(exported_page.get())));
  EXPECT_EQ(6, FPDFPage_GetAnnotCount(page.get()));
}

TEST_F(FPDFFlattenEmbedderTest, ExportAnnotationsDeduplicatesSharedResources) {
  // Two annotations whose appearance streams reference the SAME indirect
  // font object: the exported document must carry that font once.
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 200, 200));
  ASSERT_TRUE(page);
  CPDF_Document* src = CPDFDocumentFromFPDFDocument(doc.get());
  auto font_dict = src->NewIndirect<CPDF_Dictionary>();
  font_dict->SetNewFor<CPDF_Name>("Type", "Font");
  font_dict->SetNewFor<CPDF_Name>("Subtype", "Type1");
  font_dict->SetNewFor<CPDF_Name>("BaseFont", "Helvetica");
  const uint32_t font_object = font_dict->GetObjNum();
  ASSERT_NE(0u, font_object);

  auto make_text_annot = [&](float x, float y) {
    ScopedFPDFAnnotation annot =
        MakeSquareWithAppearance(page.get(), {x, y + 30.0f, x + 80.0f, y},
                                 L"BT /F1 12 Tf 1 1 Td (Hi) Tj ET", nullptr);
    if (!annot) {
      return annot;
    }
    CPDF_AnnotContext* ctx = CPDFAnnotContextFromFPDFAnnotation(annot.get());
    RetainPtr<CPDF_Stream> normal = ctx->GetMutableAnnotDict()
                                        ->GetMutableDictFor("AP")
                                        ->GetMutableStreamFor("N");
    RetainPtr<CPDF_Dictionary> fonts = normal->GetMutableDict()
                                           ->GetOrCreateDictFor("Resources")
                                           ->GetOrCreateDictFor("Font");
    fonts->SetNewFor<CPDF_Reference>("F1", src, font_object);
    return annot;
  };
  ScopedFPDFAnnotation s1 = make_text_annot(10, 10);
  ScopedFPDFAnnotation s2 = make_text_annot(100, 100);
  ASSERT_TRUE(s1);
  ASSERT_TRUE(s2);

  FPDF_ANNOTATION set[] = {s1.get(), s2.get()};
  ScopedFPDFDocument exported(
      EPDFPage_ExportAnnotationsAsDocument(page.get(), set, 2));
  ASSERT_TRUE(exported);

  // Walk page → wrapper form → the two cloned appearance forms → their
  // /Font resources: both must point at ONE font object.
  CPDF_Document* dest = CPDFDocumentFromFPDFDocument(exported.get());
  RetainPtr<const CPDF_Dictionary> page_dict = dest->GetPageDictionary(0);
  ASSERT_TRUE(page_dict);
  RetainPtr<const CPDF_Dictionary> xobjects =
      page_dict->GetDictFor("Resources")->GetDictFor("XObject");
  ASSERT_TRUE(xobjects);
  RetainPtr<const CPDF_Stream> wrapper = xobjects->GetStreamFor("FFT0");
  ASSERT_TRUE(wrapper);
  RetainPtr<const CPDF_Dictionary> forms =
      wrapper->GetDict()->GetDictFor("Resources")->GetDictFor("XObject");
  ASSERT_TRUE(forms);
  std::set<uint32_t> font_objects;
  for (const char* name : {"F0", "F1"}) {
    RetainPtr<const CPDF_Stream> form = forms->GetStreamFor(name);
    ASSERT_TRUE(form) << name;
    RetainPtr<const CPDF_Dictionary> fonts =
        form->GetDict()->GetDictFor("Resources")->GetDictFor("Font");
    ASSERT_TRUE(fonts) << name;
    CPDF_DictionaryLocker locker(fonts);
    for (const auto& item : locker) {
      RetainPtr<const CPDF_Object> font = item.second->GetDirect();
      ASSERT_TRUE(font);
      font_objects.insert(font->GetObjNum());
    }
  }
  EXPECT_EQ(1u, font_objects.size());
  EXPECT_FALSE(font_objects.contains(0u));
}
