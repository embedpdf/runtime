// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Flattening and annotation export (public/epdf_flatten.h).

#include "public/epdf_flatten.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_indirect_object_holder.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_object_copier.h"
#include "public/epdf_annot.h"
#include "public/epdf_form.h"
#include "public/epdf_layer.h"
#include "public/epdf_save.h"
#include "public/epdf_view.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_flatten.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/test_loader.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

namespace {

// The removed single-annotation entry point, expressed through the set API:
// every existing single-target test keeps its exact expectations.
int FlattenOne(FPDF_PAGE page, FPDF_ANNOTATION annot, int usage) {
  FPDF_ANNOTATION set[] = {annot};
  return EPDFPage_FlattenAnnotations(page, set, 1, usage, nullptr);
}

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

// A yellow highlight over |rect| with no stored appearance: the renderer
// draws one in memory.
RetainPtr<CPDF_Dictionary> AddHighlightWithoutAppearance(
    CPDF_Document* document,
    CPDF_Dictionary* page,
    const CFX_FloatRect& rect) {
  auto annotation = document->NewIndirect<CPDF_Dictionary>();
  annotation->SetNewFor<CPDF_Name>("Type", "Annot");
  annotation->SetNewFor<CPDF_Name>("Subtype", "Highlight");
  annotation->SetRectFor("Rect", rect);
  annotation->SetNewFor<CPDF_Number>("F", FPDF_ANNOT_FLAG_PRINT);
  RetainPtr<CPDF_Array> quad = annotation->SetNewFor<CPDF_Array>("QuadPoints");
  for (float value : {rect.left, rect.top, rect.right, rect.top, rect.left,
                      rect.bottom, rect.right, rect.bottom}) {
    quad->AppendNew<CPDF_Number>(value);
  }
  RetainPtr<CPDF_Array> color = annotation->SetNewFor<CPDF_Array>("C");
  for (float value : {1.0f, 1.0f, 0.0f}) {
    color->AppendNew<CPDF_Number>(value);
  }
  RetainPtr<CPDF_Array> annots = page->GetMutableArrayFor("Annots");
  if (!annots) {
    annots = page->SetNewFor<CPDF_Array>("Annots");
  }
  annots->AppendNew<CPDF_Reference>(document, annotation->GetObjNum());
  return annotation;
}

constexpr int kAllKinds =
    EPDF_EXPORT_FLATTENED_ANNOTATIONS | EPDF_EXPORT_FLATTENED_FORM_FIELDS;

// The appearance forms the copy's page paints: the forms inside the wrapper
// flatten adds (0 when nothing was painted).
size_t CountPaintedAppearances(FPDF_DOCUMENT copy, int page_index) {
  RetainPtr<const CPDF_Dictionary> page =
      CPDFDocumentFromFPDFDocument(copy)->GetPageDictionary(page_index);
  RetainPtr<const CPDF_Dictionary> resources =
      page ? page->GetDictFor("Resources") : nullptr;
  RetainPtr<const CPDF_Dictionary> xobjects =
      resources ? resources->GetDictFor("XObject") : nullptr;
  RetainPtr<const CPDF_Stream> wrapper =
      xobjects ? xobjects->GetStreamFor("FFT0") : nullptr;
  if (!wrapper) {
    return 0;
  }
  RetainPtr<const CPDF_Dictionary> forms =
      wrapper->GetDict()->GetDictFor("Resources")->GetDictFor("XObject");
  return forms ? forms->size() : 0;
}

}  // namespace

class EPDFFlattenEmbedderTest : public EmbedderTest {
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

TEST_F(EPDFFlattenEmbedderTest, FlattenSpecificAnnotationByHandle) {
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
  ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_doc);
  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);
  EXPECT_EQ(5, FPDFPage_GetAnnotCount(saved_page.get()));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(saved_page.get(), 4u));
}

TEST_F(EPDFFlattenEmbedderTest,
       FlattenPagePreservesUnpaintedAnnotationsAndDetachesWidget) {
  ASSERT_TRUE(OpenDocument("flatten_selective.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ASSERT_EQ(FLATTEN_SUCCESS, EPDFPage_Flatten(page.get(), FLAT_NORMALDISPLAY));
  // The Text annotation (object 6) has no appearance; the renderer draws its
  // icon in memory, and flatten paints what the renderer draws. The hidden
  // square (5) and the invisible one (9) stay.
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(page.get(), 4u));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(page.get(), 6u));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(page.get(), 13u));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(page.get(), 16u));
  for (unsigned int object_number : {5u, 9u}) {
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

TEST_F(EPDFFlattenEmbedderTest, FlattenPageHonorsPrintUsage) {
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

TEST_F(EPDFFlattenEmbedderTest, FlattenMergedWidgetRemovesFieldTreeEntry) {
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

TEST_F(EPDFFlattenEmbedderTest, FlattenPageIsLayerSafeAndDeltaDurable) {
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

  // The Text annotation without an appearance is painted from memory too.
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

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
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(replay_page.get()));
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(replay.get());
  ASSERT_TRUE(model);
  ASSERT_EQ(1, EPDFForm_CountFields(model));
  EXPECT_EQ(0, EPDFForm_CountFieldWidgets(model, 0));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFlattenEmbedderTest, FlattenReadsAlreadyPromotedAnnotationState) {
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

TEST_F(EPDFFlattenEmbedderTest, FlattenDirectAnnotationByHandle) {
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

TEST_F(EPDFFlattenEmbedderTest, FlattenAnnotationSetReportsPerEntry) {
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

TEST_F(EPDFFlattenEmbedderTest, FlattenAnnotationSetRejectsForeignAnnotation) {
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

TEST_F(EPDFFlattenEmbedderTest, ExportAnnotationsMatchesSourceCrop) {
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

TEST_F(EPDFFlattenEmbedderTest, ExportAnnotationsIsAllOrNothing) {
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

TEST_F(EPDFFlattenEmbedderTest, ExportAnnotationsDeduplicatesSharedResources) {
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

TEST_F(EPDFFlattenEmbedderTest, FlattenPaintsTheAppearanceDrawnInMemory) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());
  {
    ScopedFPDFPage blank(FPDFPage_New(doc.get(), 0, 200, 200));
    ASSERT_TRUE(blank);
  }
  RetainPtr<CPDF_Dictionary> highlight =
      AddHighlightWithoutAppearance(pdf, pdf->GetMutablePageDictionary(0).Get(),
                                    CFX_FloatRect(20, 20, 120, 40));

  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFBitmap drawn = RenderPageWithFlags(page.get(), nullptr, FPDF_ANNOT);
  ScopedFPDFBitmap blank = WhiteBitmap(200, 200);
  ASSERT_NE(HashBitmap(blank.get()), HashBitmap(drawn.get()));

  ASSERT_EQ(FLATTEN_SUCCESS, EPDFPage_Flatten(page.get(), FLAT_NORMALDISPLAY));
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
  // Painted into the page, not written onto the annotation.
  EXPECT_FALSE(highlight->KeyExist("AP"));

  // Saved and opened again, the page shows what the renderer drew.
  ASSERT_TRUE(FPDF_SaveAsCopy(doc.get(), this, 0));
  ScopedSavedDoc saved = OpenScopedSavedDocument();
  ASSERT_TRUE(saved);
  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(saved_page.get()));
  ScopedFPDFBitmap painted = RenderSavedPage(saved_page.get());
  EXPECT_EQ(HashBitmap(drawn.get()), HashBitmap(painted.get()));
}

TEST_F(EPDFFlattenEmbedderTest, ExportPagesFlattenedPrintsAFieldDrawnInMemory) {
  // Opened as the engine opens a document: no form-fill environment, which
  // would write appearances onto the fields when a page loads.
  const std::string path = PathService::GetTestFilePath("text_form.pdf");
  ASSERT_FALSE(path.empty());
  std::vector<uint8_t> bytes = GetFileContents(path.c_str());
  ScopedFPDFDocument doc(
      FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr));
  ASSERT_TRUE(doc);
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());
  // The text field has no appearance: the renderer draws its value in
  // memory, in the form's default font.
  RetainPtr<CPDF_Dictionary> field =
      ToDictionary(pdf->GetMutableIndirectObject(4u));
  ASSERT_TRUE(field);
  ASSERT_FALSE(field->KeyExist("AP"));
  field->SetNewFor<CPDF_String>("V", "Printed");
  field->SetNewFor<CPDF_Number>("F", FPDF_ANNOT_FLAG_PRINT);

  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  // Fields draw with EPDF_RENDER_WIDGETS, as the engine draws them.
  ScopedFPDFBitmap source = RenderPageWithFlags(
      page.get(), nullptr, EPDF_RENDER_WIDGETS | FPDF_PRINTING);
  ScopedFPDFBitmap content_only =
      RenderPageWithFlags(page.get(), nullptr, FPDF_PRINTING);
  ASSERT_NE(HashBitmap(content_only.get()), HashBitmap(source.get()));

  const int index = 0;
  ScopedFPDFDocument copy(EPDFDoc_ExportPagesFlattened(doc.get(), &index, 1,
                                                       FLAT_PRINT, kAllKinds));
  ASSERT_TRUE(copy);
  ASSERT_EQ(1, FPDF_GetPageCount(copy.get()));
  ScopedFPDFPage copy_page(FPDF_LoadPage(copy.get(), 0));
  ASSERT_TRUE(copy_page);
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(copy_page.get()));
  EXPECT_EQ(1u, CountPaintedAppearances(copy.get(), 0));
  ScopedFPDFBitmap printed =
      RenderPageWithFlags(copy_page.get(), nullptr, FPDF_PRINTING);
  EXPECT_EQ(HashBitmap(source.get()), HashBitmap(printed.get()));

  // The copy is page content only, and the source is untouched.
  EXPECT_FALSE(CPDFDocumentFromFPDFDocument(copy.get())
                   ->GetRoot()
                   ->KeyExist("AcroForm"));
  EXPECT_FALSE(field->KeyExist("AP"));
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
}

TEST_F(EPDFFlattenEmbedderTest, ExportPagesFlattenedLeavesThePageActionsOut) {
  ASSERT_TRUE(OpenDocument("get_page_aaction.pdf"));
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(pdf->GetPageDictionary(0)->KeyExist("AA"));

  const int index = 0;
  ScopedFPDFDocument copy(EPDFDoc_ExportPagesFlattened(document(), &index, 1,
                                                       FLAT_PRINT, kAllKinds));
  ASSERT_TRUE(copy);
  RetainPtr<const CPDF_Dictionary> page =
      CPDFDocumentFromFPDFDocument(copy.get())->GetPageDictionary(0);
  ASSERT_TRUE(page);
  EXPECT_FALSE(page->KeyExist("AA"));
  EXPECT_FALSE(page->KeyExist("Annots"));
  EXPECT_TRUE(page->KeyExist("MediaBox"));
  EXPECT_TRUE(page->KeyExist("Resources"));
  ScopedFPDFPage loaded(FPDF_LoadPage(copy.get(), 0));
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(FPDF_GetPageAAction(loaded.get(), FPDFPAGE_AACTION_OPEN));
  EXPECT_FALSE(FPDF_GetPageAAction(loaded.get(), FPDFPAGE_AACTION_CLOSE));
}

TEST_F(EPDFFlattenEmbedderTest, ExportPagesFlattenedKeepsTheLayersPrintState) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());

  // One layer on screen but never on paper, one on paper but never on
  // screen.
  auto make_layer = [&](const char* name, const char* view, const char* print) {
    auto layer = pdf->NewIndirect<CPDF_Dictionary>();
    layer->SetNewFor<CPDF_Name>("Type", "OCG");
    layer->SetNewFor<CPDF_String>("Name", ByteString(name));
    RetainPtr<CPDF_Dictionary> usage =
        layer->SetNewFor<CPDF_Dictionary>("Usage");
    usage->SetNewFor<CPDF_Dictionary>("View")->SetNewFor<CPDF_Name>("ViewState",
                                                                    view);
    usage->SetNewFor<CPDF_Dictionary>("Print")->SetNewFor<CPDF_Name>(
        "PrintState", print);
    return layer;
  };
  RetainPtr<CPDF_Dictionary> screen_only = make_layer("Screen", "ON", "OFF");
  RetainPtr<CPDF_Dictionary> paper_only = make_layer("Paper", "OFF", "ON");
  RetainPtr<CPDF_Dictionary> properties =
      pdf->GetMutableRoot()->SetNewFor<CPDF_Dictionary>("OCProperties");
  RetainPtr<CPDF_Array> groups = properties->SetNewFor<CPDF_Array>("OCGs");
  groups->AppendNew<CPDF_Reference>(pdf, screen_only->GetObjNum());
  groups->AppendNew<CPDF_Reference>(pdf, paper_only->GetObjNum());
  properties->SetNewFor<CPDF_Dictionary>("D");

  RetainPtr<CPDF_Dictionary> page_dict = pdf->CreateNewPage(0);
  ASSERT_TRUE(page_dict);
  page_dict->SetRectFor("MediaBox", CFX_FloatRect(0, 0, 100, 100));
  RetainPtr<CPDF_Dictionary> layers =
      page_dict->SetNewFor<CPDF_Dictionary>("Resources")
          ->SetNewFor<CPDF_Dictionary>("Properties");
  layers->SetNewFor<CPDF_Reference>("S", pdf, screen_only->GetObjNum());
  layers->SetNewFor<CPDF_Reference>("P", pdf, paper_only->GetObjNum());
  auto content = pdf->NewIndirect<CPDF_Stream>(pdf->New<CPDF_Dictionary>());
  const ByteString ops =
      "/OC /S BDC 1 0 0 rg 10 10 30 30 re f EMC "
      "/OC /P BDC 0 0 1 rg 60 60 30 30 re f EMC";
  content->SetData(ops.unsigned_span());
  page_dict->SetNewFor<CPDF_Reference>("Contents", pdf, content->GetObjNum());

  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFBitmap on_screen = RenderPageWithFlags(page.get(), nullptr, 0);
  ScopedFPDFBitmap on_paper =
      RenderPageWithFlags(page.get(), nullptr, FPDF_PRINTING);
  ASSERT_NE(HashBitmap(on_screen.get()), HashBitmap(on_paper.get()));

  const int index = 0;
  ScopedFPDFDocument copy(
      EPDFDoc_ExportPagesFlattened(doc.get(), &index, 1, FLAT_PRINT, 0));
  ASSERT_TRUE(copy);
  CPDF_Document* copy_pdf = CPDFDocumentFromFPDFDocument(copy.get());
  RetainPtr<const CPDF_Dictionary> copy_properties =
      copy_pdf->GetRoot()->GetDictFor("OCProperties");
  ASSERT_TRUE(copy_properties);
  EXPECT_EQ(2u, copy_properties->GetArrayFor("OCGs")->size());
  RetainPtr<const CPDF_Array> off =
      copy_properties->GetDictFor("D")->GetArrayFor("OFF");
  ASSERT_TRUE(off);
  ASSERT_EQ(1u, off->size());
  EXPECT_EQ("Screen", off->GetDictAt(0)->GetUnicodeTextFor("Name").ToUTF8());

  // The copy prints as the source prints, and a viewer that reads only the
  // configuration shows the same.
  ScopedFPDFPage copy_page(FPDF_LoadPage(copy.get(), 0));
  ASSERT_TRUE(copy_page);
  ScopedFPDFBitmap printed =
      RenderPageWithFlags(copy_page.get(), nullptr, FPDF_PRINTING);
  EXPECT_EQ(HashBitmap(on_paper.get()), HashBitmap(printed.get()));
}

TEST_F(EPDFFlattenEmbedderTest, ExportPagesFlattenedPaintsTheChosenKinds) {
  ASSERT_TRUE(OpenDocument("flatten_selective.pdf"));
  // For print: the square (4) and the widget (13); the hidden (5), the
  // invisible (9) and the unflagged (6, 16) annotations don't print.
  const int index = 0;
  struct {
    int flags;
    size_t painted;
  } cases[] = {
      {0, 0u},
      {EPDF_EXPORT_FLATTENED_ANNOTATIONS, 1u},
      {EPDF_EXPORT_FLATTENED_FORM_FIELDS, 1u},
      {kAllKinds, 2u},
  };
  for (const auto& test_case : cases) {
    ScopedFPDFDocument copy(EPDFDoc_ExportPagesFlattened(
        document(), &index, 1, FLAT_PRINT, test_case.flags));
    ASSERT_TRUE(copy) << test_case.flags;
    EXPECT_EQ(test_case.painted, CountPaintedAppearances(copy.get(), 0))
        << test_case.flags;
  }
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(6, FPDFPage_GetAnnotCount(page.get()));
}

TEST_F(EPDFFlattenEmbedderTest,
       ExportPagesFlattenedCopiesPagesInTheGivenOrder) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  for (int i = 0; i < 3; ++i) {
    ScopedFPDFPage page(FPDFPage_New(doc.get(), i, 100.0 + 10 * i, 200));
    ASSERT_TRUE(page);
  }
  const int indices[] = {2, 0};
  ScopedFPDFDocument copy(EPDFDoc_ExportPagesFlattened(doc.get(), indices, 2,
                                                       FLAT_PRINT, kAllKinds));
  ASSERT_TRUE(copy);
  ASSERT_EQ(2, FPDF_GetPageCount(copy.get()));
  ScopedFPDFPage first(FPDF_LoadPage(copy.get(), 0));
  ScopedFPDFPage second(FPDF_LoadPage(copy.get(), 1));
  EXPECT_FLOAT_EQ(120.0f, FPDF_GetPageWidthF(first.get()));
  EXPECT_FLOAT_EQ(100.0f, FPDF_GetPageWidthF(second.get()));
}

TEST_F(EPDFFlattenEmbedderTest, ExportPagesFlattenedRefusesWhatItCantCopy) {
  ASSERT_TRUE(OpenDocument("flatten_selective.pdf"));
  const int index = 0;
  const int beyond = 1;
  EXPECT_FALSE(
      EPDFDoc_ExportPagesFlattened(nullptr, &index, 1, FLAT_PRINT, kAllKinds));
  EXPECT_FALSE(EPDFDoc_ExportPagesFlattened(document(), nullptr, 1, FLAT_PRINT,
                                            kAllKinds));
  EXPECT_FALSE(EPDFDoc_ExportPagesFlattened(document(), &index, 0, FLAT_PRINT,
                                            kAllKinds));
  EXPECT_FALSE(EPDFDoc_ExportPagesFlattened(document(), &beyond, 1, FLAT_PRINT,
                                            kAllKinds));
  EXPECT_FALSE(
      EPDFDoc_ExportPagesFlattened(document(), &index, 1, 99, kAllKinds));
  EXPECT_FALSE(EPDFDoc_ExportPagesFlattened(document(), &index, 1, FLAT_PRINT,
                                            kAllKinds << 1));
}

TEST_F(EPDFFlattenEmbedderTest, ExportPagesFlattenedFromALayerWritesNothing) {
  LayerDocument document;
  ASSERT_TRUE(OpenLayer("flatten_selective.pdf", &document));
  // For display, the Text annotation without an appearance is painted from
  // memory: drawing it there reads the layer and writes nothing to it.
  const int index = 0;
  ScopedFPDFDocument copy(EPDFDoc_ExportPagesFlattened(
      document.layer, &index, 1, FLAT_NORMALDISPLAY, kAllKinds));
  ASSERT_TRUE(copy);
  // The squares (4, 16), the widget (13) and the Text annotation (6).
  EXPECT_EQ(4u, CountPaintedAppearances(copy.get(), 0));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(document.layer));
}

TEST_F(EPDFFlattenEmbedderTest, CopierFollowsReferencesByWhatTheyResolveTo) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());
  // An appearance drawn in memory numbers its fonts from 1 in a store of its
  // own, while the document has an object 1 of its own (its catalog).
  CPDF_IndirectObjectHolder store;
  auto font = store.NewIndirect<CPDF_Dictionary>();
  font->SetNewFor<CPDF_Name>("Type", "Font");
  font->SetNewFor<CPDF_Name>("BaseFont", "InMemory");
  ASSERT_EQ(1u, font->GetObjNum());
  ASSERT_TRUE(pdf->GetIndirectObject(1u));
  auto document_font = pdf->NewIndirect<CPDF_Dictionary>();
  document_font->SetNewFor<CPDF_Name>("BaseFont", "InTheDocument");

  auto appearance =
      pdfium::MakeRetain<CPDF_Stream>(pdfium::MakeRetain<CPDF_Dictionary>());
  RetainPtr<CPDF_Dictionary> fonts =
      appearance->GetMutableDict()
          ->SetNewFor<CPDF_Dictionary>("Resources")
          ->SetNewFor<CPDF_Dictionary>("Font");
  fonts->SetNewFor<CPDF_Reference>("F1", &store, 1u);
  fonts->SetNewFor<CPDF_Reference>("F2", pdf, document_font->GetObjNum());
  fonts->SetNewFor<CPDF_Reference>("F3", &store, 1u);

  epdf::ObjectCopier copier(pdf);
  RetainPtr<CPDF_Stream> copy = copier.CopyStream(appearance.Get());
  ASSERT_TRUE(copy);
  ASSERT_NE(0u, copy->GetObjNum());
  RetainPtr<const CPDF_Dictionary> copied_fonts =
      copy->GetDict()->GetDictFor("Resources")->GetDictFor("Font");
  RetainPtr<const CPDF_Dictionary> in_memory = copied_fonts->GetDictFor("F1");
  ASSERT_TRUE(in_memory);
  EXPECT_EQ("InMemory", in_memory->GetNameFor("BaseFont"));
  EXPECT_NE(1u, in_memory->GetObjNum());
  // Copied once, however many references reach it.
  EXPECT_EQ(in_memory->GetObjNum(),
            copied_fonts->GetDictFor("F3")->GetObjNum());
  // The document's own object is referenced, not copied.
  EXPECT_EQ(document_font->GetObjNum(),
            copied_fonts->GetDictFor("F2")->GetObjNum());
}
