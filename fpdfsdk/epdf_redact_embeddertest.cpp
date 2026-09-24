// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_redact.h"

#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_flatten.h"
#include "public/fpdf_save.h"
#include "public/fpdf_text.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

std::wstring ReadText(FPDF_PAGE page) {
  ScopedFPDFTextPage text(FPDFText_LoadPage(page));
  EXPECT_TRUE(text);
  if (!text) {
    return {};
  }
  const int count = FPDFText_CountChars(text.get());
  if (count <= 0) {
    return {};
  }
  std::vector<FPDF_WCHAR> buffer(count + 1);
  EXPECT_GT(FPDFText_GetText(text.get(), 0, count, buffer.data()), 0);
  return GetPlatformWString(buffer.data());
}

ScopedFPDFBitmap Render(FPDF_PAGE page) {
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(600, 600, 0));
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, 600, 600, 0xffffffff);
  FPDF_RenderPageBitmap(bitmap.get(), page, 0, 0, 600, 600, 0, 0);
  return bitmap;
}

uint32_t Pixel(FPDF_BITMAP bitmap, int x, int y) {
  const auto* data = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
  const uint8_t* pixel = data + y * FPDFBitmap_GetStride(bitmap) + x * 4;
  return (uint32_t{pixel[2]} << 16) | (uint32_t{pixel[1]} << 8) | pixel[0];
}

void ExpectSamePixels(FPDF_BITMAP before,
                      FPDF_BITMAP after,
                      int left,
                      int top,
                      int right,
                      int bottom) {
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      ASSERT_EQ(Pixel(before, x, y), Pixel(after, x, y)) << x << ", " << y;
    }
  }
}

// These controlled fixtures use ASCII sentinels. Inspect the saved graph as
// well as text extraction: an unused resource or replacement-text property
// can retain the sentinel even when the rendered page/text extractor is clean.
// This is deliberately NOT a general-purpose PDF sanitization oracle.
void ExpectNoSentinel(CPDF_Document* doc, const std::string& sentinel) {
  std::vector<RetainPtr<const CPDF_Object>> pending{
      pdfium::WrapRetain(doc->GetRoot())};
  std::set<const CPDF_Object*> visited;
  while (!pending.empty()) {
    auto object = pending.back()->GetDirect();
    pending.pop_back();
    if (!object || !visited.insert(object.Get()).second) {
      continue;
    }
    if (const CPDF_Stream* stream = object->AsStream()) {
      auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
      acc->LoadAllDataFiltered();
      const auto bytes = acc->GetSpan();
      const std::string decoded(bytes.begin(), bytes.end());
      EXPECT_EQ(std::string::npos, decoded.find(sentinel)) << decoded;
      pending.push_back(stream->GetDict());
    } else if (const CPDF_Dictionary* dict = object->AsDictionary()) {
      for (const auto& key : dict->GetKeys()) {
        pending.push_back(dict->GetObjectFor(key.AsStringView()));
      }
    } else if (const CPDF_Array* array = object->AsArray()) {
      for (size_t i = 0; i < array->size(); ++i) {
        pending.push_back(array->GetObjectAt(i));
      }
    } else if (object->IsString()) {
      const ByteString value = object->GetString();
      EXPECT_EQ(std::string::npos,
                std::string(value.c_str(), value.GetLength()).find(sentinel));
    }
  }
}

// How many streams reachable from the document's root carry `sentinel`.
int CountSentinel(CPDF_Document* doc, const std::string& sentinel) {
  int count = 0;
  std::vector<RetainPtr<const CPDF_Object>> pending{
      pdfium::WrapRetain(doc->GetRoot())};
  std::set<const CPDF_Object*> visited;
  while (!pending.empty()) {
    auto object = pending.back()->GetDirect();
    pending.pop_back();
    if (!object || !visited.insert(object.Get()).second) {
      continue;
    }
    if (const CPDF_Stream* stream = object->AsStream()) {
      auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
      acc->LoadAllDataFiltered();
      const auto bytes = acc->GetSpan();
      if (std::string(bytes.begin(), bytes.end()).find(sentinel) !=
          std::string::npos) {
        ++count;
      }
      pending.push_back(stream->GetDict());
    } else if (const CPDF_Dictionary* dict = object->AsDictionary()) {
      for (const auto& key : dict->GetKeys()) {
        pending.push_back(dict->GetObjectFor(key.AsStringView()));
      }
    } else if (const CPDF_Array* array = object->AsArray()) {
      for (size_t i = 0; i < array->size(); ++i) {
        pending.push_back(array->GetObjectAt(i));
      }
    }
  }
  return count;
}

// A one-page drawing 300 wide that writes `lines`, one every 40 points from
// the bottom up, its font dictionary an object of its own (/Font 5 0 R), as
// some PDF generators write it.
std::string MakeTextDrawing(const std::vector<std::string>& lines) {
  std::string content;
  for (size_t i = 0; i < lines.size(); ++i) {
    content += "BT /F1 24 Tf 10 " + std::to_string(10 + 40 * i) + " Td (" +
               lines[i] + ") Tj ET\n";
  }
  const std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 " +
          std::to_string(10 + 40 * lines.size()) +
          "] /Contents 4 0 R /Resources << /Font 5 0 R >> >>",
      "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
          content + "\nendstream",
      "<< /F1 6 0 R >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  };
  std::string pdf = "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const size_t xref = pdf.size();
  pdf += "xref\n0 " + std::to_string(objects.size() + 1) +
         "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    const std::string number = std::to_string(offset);
    pdf += std::string(10 - number.size(), '0') + number + " 00000 n \n";
  }
  pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
         " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return pdf;
}

RetainPtr<CPDF_Dictionary> AnnotDict(FPDF_ANNOTATION annot) {
  return CPDFAnnotContextFromFPDFAnnotation(annot)->GetMutableAnnotDict();
}

// A stamp at `rect` drawing the first page of `source`.
ScopedFPDFAnnotation AddStamp(FPDF_PAGE page,
                              FPDF_DOCUMENT source,
                              const FS_RECTF& rect) {
  ScopedFPDFAnnotation stamp(FPDFPage_CreateAnnot(page, FPDF_ANNOT_STAMP));
  EXPECT_TRUE(EPDFAnnot_SetRect(stamp.get(), &rect));
  EXPECT_TRUE(EPDFAnnot_SetAppearanceFromPage(stamp.get(), source, 0));
  EXPECT_TRUE(
      EPDFAnnot_UpdateAppearanceToRect(stamp.get(), EPDF_STAMP_FIT_CONTAIN));
  return stamp;
}

// A second stamp at `rect` placing the same drawing object as `stamp`: its
// own frame around a shared drawing.
ScopedFPDFAnnotation AddStampSharingDrawing(FPDF_DOCUMENT doc,
                                            FPDF_PAGE page,
                                            FPDF_ANNOTATION stamp,
                                            const FS_RECTF& rect) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  ScopedFPDFAnnotation other(FPDFPage_CreateAnnot(page, FPDF_ANNOT_STAMP));
  EXPECT_TRUE(EPDFAnnot_SetRect(other.get(), &rect));
  RetainPtr<CPDF_Stream> frame =
      ToStream(AnnotDict(stamp)->GetDictFor("AP")->GetStreamFor("N")->Clone());
  pdf->AddIndirectObject(frame);
  AnnotDict(other.get())
      ->SetNewFor<CPDF_Dictionary>("AP")
      ->SetNewFor<CPDF_Reference>("N", pdf, frame->GetObjNum());
  EXPECT_TRUE(
      EPDFAnnot_UpdateAppearanceToRect(other.get(), EPDF_STAMP_FIT_CONTAIN));
  return other;
}

// The drawing a stamp's frame places.
RetainPtr<const CPDF_Stream> DrawingOf(FPDF_ANNOTATION stamp) {
  return AnnotDict(stamp)
      ->GetDictFor("AP")
      ->GetStreamFor("N")
      ->GetDict()
      ->GetDictFor("Resources")
      ->GetDictFor("XObject")
      ->GetStreamFor("EPDFWRAP");
}

std::string StreamText(const CPDF_Stream* stream) {
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
  acc->LoadAllDataFiltered();
  const auto bytes = acc->GetSpan();
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace

class EPDFRedactEmbedderTest : public EmbedderTest {
 protected:
  void ApplyAndSave(FPDF_PAGE page) {
    ASSERT_TRUE(EPDFPage_ApplyRedactions(page, nullptr));
    ASSERT_TRUE(FPDFPage_GenerateContent(page));
    // Safe export is caller-selected. Never let an incremental save obscure
    // the distinction between removal defects and deliberately retained
    // history.
    ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_NO_INCREMENTAL));
    ASSERT_TRUE(OpenSavedDocument());
  }

  // How many streams reachable in a full rewrite of `doc` carry `sentinel`.
  int SentinelsInRewrite(FPDF_DOCUMENT doc, const std::string& sentinel) {
    ClearString();
    if (!FPDF_SaveAsCopy(doc, this, FPDF_NO_INCREMENTAL)) {
      ADD_FAILURE() << "the rewrite failed";
      return -1;
    }
    const std::string bytes = GetString();
    ScopedFPDFDocument saved(
        FPDF_LoadMemDocument(bytes.data(), bytes.size(), nullptr));
    if (!saved) {
      ADD_FAILURE() << "the rewrite does not open";
      return -1;
    }
    return CountSentinel(CPDFDocumentFromFPDFDocument(saved.get()), sentinel);
  }
};

TEST_F(EPDFRedactEmbedderTest, NestedFormsUseTheRenderersTransformOrder) {
  ASSERT_TRUE(OpenDocument("redact_nested_forms.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(L"SECRET\r\nPUBLIC", ReadText(page.get()));
  auto before = Render(page.get());

  ApplyAndSave(page.get());
  FPDF_PAGE saved = LoadSavedPage(0);
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"PUBLIC", ReadText(saved));
  auto after = Render(saved);
  // Independent, fixture-defined coordinates. Overlays are disabled, so
  // neither extraction nor rendering can hide the surviving nested word.
  for (int y = 440; y < 465; ++y) {
    for (int x = 299; x < 390; ++x) {
      EXPECT_EQ(0xffffffu, Pixel(after.get(), x, y));
    }
  }
  ExpectSamePixels(before.get(), after.get(), 45, 275, 160, 305);
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
  CloseSavedPage(saved);
}

TEST_F(EPDFRedactEmbedderTest, SharedImageAndMaskPreserveOtherPlacements) {
  ASSERT_TRUE(OpenDocument("redact_shared_image.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  auto before = Render(page.get());
  const uint32_t original = Pixel(before.get(), 275, 500);
  EXPECT_NE(0xffffffu, original);

  ApplyAndSave(page.get());
  FPDF_PAGE saved = LoadSavedPage(0);
  ASSERT_TRUE(saved);
  auto after = Render(saved);
  EXPECT_EQ(0xffffffu, Pixel(after.get(), 75, 500));
  ExpectSamePixels(before.get(), after.get(), 100, 450, 150, 550);
  ExpectSamePixels(before.get(), after.get(), 250, 450, 350, 550);
  CloseSavedPage(saved);

  // This placement was never parsed or rendered before the mutation; caches
  // cannot disguise an overwrite of its shared image or soft-mask stream.
  FPDF_PAGE other = LoadSavedPage(1);
  ASSERT_TRUE(other);
  auto other_bitmap = Render(other);
  EXPECT_EQ(original, Pixel(other_bitmap.get(), 75, 500));
  EXPECT_EQ(original, Pixel(other_bitmap.get(), 125, 500));
  CloseSavedPage(other);
}

TEST_F(EPDFRedactEmbedderTest, RemovesReplacementTextFromWholeAffectedSpan) {
  ASSERT_TRUE(OpenDocument("redact_actual_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  auto before = Render(page.get());

  ApplyAndSave(page.get());
  FPDF_PAGE saved = LoadSavedPage(0);
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"keep\r\nsibling\r\nPUBLIC", ReadText(saved));
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
  auto after = Render(saved);
  ExpectSamePixels(before.get(), after.get(), 135, 475, 200, 505);
  ExpectSamePixels(before.get(), after.get(), 45, 375, 130, 405);
  ExpectSamePixels(before.get(), after.get(), 45, 275, 160, 305);

  // Unaffected sibling structure retains its accessibility text, while the
  // affected element and its aggregate parent lose stale replacement text.
  auto root = CPDFDocumentFromFPDFDocument(saved_document())->GetRoot();
  auto tree = root->GetDictFor("StructTreeRoot");
  auto paragraph = tree->GetArrayFor("K")->GetDictAt(0);
  EXPECT_FALSE(paragraph->KeyExist("ActualText"));
  auto kids = paragraph->GetArrayFor("K");
  EXPECT_FALSE(kids->GetDictAt(0)->KeyExist("ActualText"));
  EXPECT_EQ(L"PUBLIC", kids->GetDictAt(1)->GetUnicodeTextFor("ActualText"));
  CloseSavedPage(saved);
}

TEST_F(EPDFRedactEmbedderTest,
       DirectReplacementTextIsRemovedFromSiblingObjects) {
  ASSERT_TRUE(OpenDocument("redact_direct_actual_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ApplyAndSave(page.get());
  FPDF_PAGE saved = LoadSavedPage(0);
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"keep\r\nsibling", ReadText(saved));
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
  CloseSavedPage(saved);
}

TEST_F(EPDFRedactEmbedderTest,
       FullyRemovedSpanLeavesNoPropertyOrStructureResidue) {
  ASSERT_TRUE(OpenDocument("redact_actual_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    const FS_RECTF whole_span = {40, 230, 300, 90};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &whole_span));
  }
  ApplyAndSave(page.get());
  FPDF_PAGE saved = LoadSavedPage(0);
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"PUBLIC", ReadText(saved));
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
  CloseSavedPage(saved);
}

TEST_F(EPDFRedactEmbedderTest, SharedFormPreservesUnmarkedReplacementText) {
  ASSERT_TRUE(OpenDocument("redact_shared_marked_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  auto before = Render(page.get());
  ApplyAndSave(page.get());
  FPDF_PAGE saved = LoadSavedPage(0);
  ASSERT_TRUE(saved);
  // The unmarked copy is legitimate content. A document-wide forbidden-word
  // assertion would be wrong here, and global property mutation loses it.
  EXPECT_EQ(L"keep SECRET keep", ReadText(saved));
  auto after = Render(saved);
  ExpectSamePixels(before.get(), after.get(), 45, 275, 220, 305);
  CloseSavedPage(saved);
}

TEST_F(EPDFRedactEmbedderTest, SharedImagePlacementsReceiveIndependentRegions) {
  ASSERT_TRUE(OpenDocument("redact_shared_image.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  auto before = Render(page.get());
  const uint32_t original = Pixel(before.get(), 275, 500);
  {
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
    const FS_RECTF other_half = {300, 150, 350, 50};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &other_half));
  }
  ApplyAndSave(page.get());
  FPDF_PAGE saved = LoadSavedPage(0);
  ASSERT_TRUE(saved);
  auto after = Render(saved);
  EXPECT_EQ(0xffffffu, Pixel(after.get(), 75, 500));
  EXPECT_EQ(original, Pixel(after.get(), 125, 500));
  EXPECT_EQ(original, Pixel(after.get(), 275, 500));
  EXPECT_EQ(0xffffffu, Pixel(after.get(), 325, 500));
  CloseSavedPage(saved);
}

TEST_F(EPDFRedactEmbedderTest, InheritedPropertiesLeaveNoUnusedOriginalOnPage) {
  ASSERT_TRUE(OpenDocument("redact_inherited_actual_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ApplyAndSave(page.get());
  FPDF_PAGE saved = LoadSavedPage(0);
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"keep", ReadText(saved));
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
  CloseSavedPage(saved);
}

// Exercise page-tree inheritance separately from Form resource inheritance.
// Parameters: layer session, direct (rather than indirect) property dictionary.
class EPDFRedactPageTreeTest
    : public EPDFRedactEmbedderTest,
      public testing::WithParamInterface<std::tuple<bool, bool>> {
 protected:
  void OpenInput(bool sibling_uses_property) {
    ASSERT_TRUE(OpenDocument("redact_actual_text.pdf"));
    auto* doc = CPDFDocumentFromFPDFDocument(document());
    auto first = doc->GetMutablePageDictionary(0);
    auto resources = first->GetMutableDictFor("Resources");
    if (std::get<1>(GetParam())) {
      auto properties = resources->GetMutableDictFor("Properties");
      properties->SetFor(
          "Sensitive",
          properties->GetDictFor("Sensitive")->CloneForHolder(doc));
    }

    auto sibling = doc->CreateNewPage(1);
    ASSERT_TRUE(sibling);
    sibling->SetFor("MediaBox", first->GetObjectFor("MediaBox")->Clone());
    if (sibling_uses_property) {
      sibling->SetFor("Contents", first->GetObjectFor("Contents")->Clone());
    } else {
      const ByteString contents("BT /F1 20 Tf 50 300 Td (PUBLIC) Tj ET");
      auto stream = doc->NewIndirect<CPDF_Stream>(contents.unsigned_span());
      sibling->SetNewFor<CPDF_Reference>("Contents", doc, stream->GetObjNum());
    }

    auto root_pages = doc->GetMutableRoot()->GetMutableDictFor("Pages");
    auto branch = doc->NewIndirect<CPDF_Dictionary>();
    branch->SetNewFor<CPDF_Name>("Type", "Pages");
    branch->SetNewFor<CPDF_Number>("Count", 2);
    branch->SetFor("Kids", root_pages->GetArrayFor("Kids")->Clone());
    branch->SetNewFor<CPDF_Reference>("Parent", doc, root_pages->GetObjNum());
    auto kids = doc->New<CPDF_Array>();
    kids->AppendNew<CPDF_Reference>(doc, branch->GetObjNum());
    root_pages->SetFor("Kids", std::move(kids));
    first->SetNewFor<CPDF_Reference>("Parent", doc, branch->GetObjNum());
    sibling->SetNewFor<CPDF_Reference>("Parent", doc, branch->GetObjNum());

    // Both ancestors reference the same resource dictionary. The higher,
    // shadowed copy must not keep an unused secret reachable after rewrite.
    const uint32_t resources_number = doc->AddIndirectObject(resources);
    root_pages->SetNewFor<CPDF_Reference>("Resources", doc, resources_number);
    branch->SetNewFor<CPDF_Reference>("Resources", doc, resources_number);
    first->RemoveFor("Resources");
    sibling->RemoveFor("Resources");

    ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_NO_INCREMENTAL));
    input_ = GetString();
    ClearString();
    if (std::get<0>(GetParam())) {
      base_.reset(
          EPDF_LoadMemBaseDocument64(input_.data(), input_.size(), nullptr));
      ASSERT_TRUE(base_);
      EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
      working_.reset(
          EPDFLayer_OpenLayer(base_.get(), nullptr, nullptr, &status));
      EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    } else {
      working_.reset(
          FPDF_LoadMemDocument64(input_.data(), input_.size(), nullptr));
    }
    ASSERT_TRUE(working_);
  }

  void SaveWorking() {
    ASSERT_TRUE(FPDF_SaveAsCopy(working_.get(), this, FPDF_NO_INCREMENTAL));
    ASSERT_TRUE(OpenSavedDocument());
  }

  void ExpectOtherLayerUnchanged() {
    if (!base_) {
      return;
    }
    ScopedFPDFDocument other(
        EPDFLayer_OpenLayer(base_.get(), nullptr, nullptr, nullptr));
    ASSERT_TRUE(other);
    ScopedFPDFPage page(FPDF_LoadPage(other.get(), 0));
    ASSERT_TRUE(page);
    EXPECT_NE(std::wstring::npos, ReadText(page.get()).find(L"SECRET"));
    auto* doc = CPDFDocumentFromFPDFDocument(other.get());
    CPDF_DocumentViewScope document_view(doc);
    auto resources =
        doc->GetRoot()->GetDictFor("Pages")->GetDictFor("Resources");
    EXPECT_TRUE(resources->GetDictFor("Properties")->KeyExist("Sensitive"));
  }

  std::string input_;
  std::unique_ptr<std::remove_pointer_t<EPDF_BASE_DOCUMENT>,
                  decltype(&EPDF_ReleaseBaseDocument)>
      base_{nullptr, EPDF_ReleaseBaseDocument};
  ScopedFPDFDocument working_;
};

TEST_P(EPDFRedactPageTreeTest, RemovesUnusedPropertiesFromEveryAncestor) {
  OpenInput(/*sibling_uses_property=*/false);
  ASSERT_TRUE(working_);
  ScopedFPDFPage page(FPDF_LoadPage(working_.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_TRUE(EPDFPage_ApplyRedactions(page.get(), nullptr));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  SaveWorking();
  ASSERT_TRUE(saved_document());
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
  FPDF_PAGE saved = LoadSavedPage(0);
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"keep\r\nsibling\r\nPUBLIC", ReadText(saved));
  CloseSavedPage(saved);
  FPDF_PAGE sibling = LoadSavedPage(1);
  ASSERT_TRUE(sibling);
  EXPECT_EQ(L"PUBLIC", ReadText(sibling));
  CloseSavedPage(sibling);
  ExpectOtherLayerUnchanged();
}

TEST_P(EPDFRedactPageTreeTest, PreservesAnUnmarkedSiblingUsingTheProperty) {
  OpenInput(/*sibling_uses_property=*/true);
  ASSERT_TRUE(working_);
  ScopedFPDFPage page(FPDF_LoadPage(working_.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_TRUE(EPDFPage_ApplyRedactions(page.get(), nullptr));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  SaveWorking();
  ASSERT_TRUE(saved_document());
  FPDF_PAGE sibling = LoadSavedPage(1);
  ASSERT_TRUE(sibling);
  EXPECT_EQ(L"SECRET keep sibling\r\nPUBLIC", ReadText(sibling));
  CloseSavedPage(sibling);
  ExpectOtherLayerUnchanged();
}

TEST_P(EPDFRedactPageTreeTest,
       RemovesAncestorPropertyAfterBothPagesAreRedacted) {
  OpenInput(/*sibling_uses_property=*/true);
  ASSERT_TRUE(working_);
  ScopedFPDFPage first(FPDF_LoadPage(working_.get(), 0));
  ScopedFPDFPage second(FPDF_LoadPage(working_.get(), 1));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  {
    ScopedFPDFAnnotation mark(
        FPDFPage_CreateAnnot(second.get(), FPDF_ANNOT_REDACT));
    const FS_RECTF rect = {49, 121, 133, 95};
    ASSERT_TRUE(FPDFAnnot_SetRect(mark.get(), &rect));
  }
  ASSERT_TRUE(EPDFPage_ApplyRedactions(first.get(), nullptr));
  ASSERT_TRUE(EPDFPage_ApplyRedactions(second.get(), nullptr));
  ASSERT_TRUE(FPDFPage_GenerateContent(first.get()));
  ASSERT_TRUE(FPDFPage_GenerateContent(second.get()));
  SaveWorking();
  ASSERT_TRUE(saved_document());
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
  ExpectOtherLayerUnchanged();
}

INSTANTIATE_TEST_SUITE_P(All,
                         EPDFRedactPageTreeTest,
                         testing::Combine(testing::Bool(), testing::Bool()));

// Stamps can share one drawing. Redacting part of one of them, flattened
// into the page, redacts a copy of the drawing for that placement: the other
// stamp's drawing, and the font dictionary it names, are left as they were,
// though the copy still names the font for the line it keeps.
TEST_F(EPDFRedactEmbedderTest,
       RedactingAFlattenedStampLeavesAStampSharingItsDrawingAlone) {
  const std::string drawing_pdf = MakeTextDrawing({"SENTINELSHARED", "KEEP"});
  ScopedFPDFDocument source(
      FPDF_LoadMemDocument(drawing_pdf.data(), drawing_pdf.size(), nullptr));
  ASSERT_TRUE(source);
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 600, 600));
  ScopedFPDFAnnotation redacted =
      AddStamp(page.get(), source.get(), FS_RECTF{50, 590, 350, 500});
  ScopedFPDFAnnotation kept = AddStampSharingDrawing(
      doc.get(), page.get(), redacted.get(), FS_RECTF{50, 390, 350, 300});
  RetainPtr<const CPDF_Stream> drawing = DrawingOf(kept.get());
  ASSERT_TRUE(drawing);
  ASSERT_EQ(drawing.Get(), DrawingOf(redacted.get()).Get());
  const std::string content = StreamText(drawing.Get());
  RetainPtr<const CPDF_Dictionary> fonts =
      drawing->GetDict()->GetDictFor("Resources")->GetDictFor("Font");
  ASSERT_TRUE(fonts);
  ASSERT_NE(0u, fonts->GetObjNum());

  FPDF_ANNOTATION flattened[] = {redacted.get()};
  ASSERT_EQ(FLATTEN_SUCCESS,
            EPDFPage_FlattenAnnotations(page.get(), flattened, 1,
                                        FLAT_NORMALDISPLAY, nullptr));
  // Flattening writes the page dictionary, not the loaded page: load it
  // again to see the stamp in its content.
  redacted.reset();
  kept.reset();
  page.reset(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
  kept.reset(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_EQ(drawing.Get(), DrawingOf(kept.get()).Get());
  ASSERT_NE(std::wstring::npos, ReadText(page.get()).find(L"SENTINELSHARED"));
  {
    // The first line only.
    ScopedFPDFAnnotation redact(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
    const FS_RECTF area = {40, 535, 360, 490};
    ASSERT_TRUE(FPDFAnnot_SetRect(redact.get(), &area));
  }
  ASSERT_TRUE(EPDFPage_ApplyRedactions(page.get(), nullptr));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  const std::wstring text = ReadText(page.get());
  EXPECT_EQ(std::wstring::npos, text.find(L"SENTINELSHARED"));
  EXPECT_NE(std::wstring::npos, text.find(L"KEEP"));
  EXPECT_EQ(drawing.Get(), DrawingOf(kept.get()).Get());
  EXPECT_EQ(content, StreamText(drawing.Get()));
  // No names added, none pruned.
  EXPECT_EQ(1u, fonts->size());
  EXPECT_TRUE(fonts->KeyExist("F1"));

  // A full rewrite keeps the drawing once: for the stamp that still shows it.
  EXPECT_EQ(1, SentinelsInRewrite(doc.get(), "SENTINELSHARED"));
}

// The only stamp with a drawing, redacted as a live annotation or after it
// was flattened into the page: a full rewrite keeps nothing of the drawing.
TEST_F(EPDFRedactEmbedderTest, RedactingTheOnlyStampLeavesNothingOfItsDrawing) {
  for (bool flatten : {false, true}) {
    SCOPED_TRACE(flatten ? "flattened" : "live");
    const std::string drawing_pdf = MakeTextDrawing({"SENTINELONLY"});
    ScopedFPDFDocument source(
        FPDF_LoadMemDocument(drawing_pdf.data(), drawing_pdf.size(), nullptr));
    ASSERT_TRUE(source);
    ScopedFPDFDocument doc(FPDF_CreateNewDocument());
    ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 600, 600));
    ScopedFPDFAnnotation stamp =
        AddStamp(page.get(), source.get(), FS_RECTF{50, 550, 350, 500});
    if (flatten) {
      FPDF_ANNOTATION flattened[] = {stamp.get()};
      ASSERT_EQ(FLATTEN_SUCCESS,
                EPDFPage_FlattenAnnotations(page.get(), flattened, 1,
                                            FLAT_NORMALDISPLAY, nullptr));
      stamp.reset();
      page.reset(FPDF_LoadPage(doc.get(), 0));
      ASSERT_TRUE(page);
      ASSERT_NE(std::wstring::npos, ReadText(page.get()).find(L"SENTINELONLY"));
    }
    stamp.reset();
    ASSERT_EQ(1, SentinelsInRewrite(doc.get(), "SENTINELONLY"));
    {
      ScopedFPDFAnnotation redact(
          FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
      const FS_RECTF area = {40, 560, 360, 490};
      ASSERT_TRUE(FPDFAnnot_SetRect(redact.get(), &area));
    }
    ASSERT_TRUE(EPDFPage_ApplyRedactions(page.get(), nullptr));
    ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
    EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
    EXPECT_EQ(0, SentinelsInRewrite(doc.get(), "SENTINELONLY"));
  }
}
