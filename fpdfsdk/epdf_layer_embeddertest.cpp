// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Base documents and layers (public/epdf_layer.h).
#include "public/epdf_layer.h"

#include <string.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_annot.h"
#include "public/epdf_doc.h"
#include "public/epdf_save.h"
#include "public/epdf_view.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_attachment.h"
#include "public/fpdf_doc.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdf_text.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

namespace {

constexpr char kFirstAlternate[] = "FirstAlternate";
constexpr char kLastAlternate[] = "LastAlternate";

uint32_t GetReferencedObjectNumber(const CPDF_Dictionary* dict,
                                   ByteStringView key) {
  if (!dict) {
    return 0;
  }

  RetainPtr<const CPDF_Reference> ref = ToReference(dict->GetObjectFor(key));
  return ref ? ref->GetRefObjNum() : 0;
}

bool PdfBytesContainObject(const std::string& pdf, uint32_t objnum) {
  return pdf.find(std::to_string(objnum) + " 0 obj") != std::string::npos;
}

struct CountingStringFileAccess {
  explicit CountingStringFileAccess(std::string data) : data(std::move(data)) {
    file_access.m_FileLen = this->data.size();
    file_access.m_GetBlock = &CountingStringFileAccess::GetBlock;
    file_access.m_Param = this;
  }

  FPDF_FILEACCESS* get() { return &file_access; }
  void ResetCounts() {
    read_count = 0;
    read_bytes = 0;
  }

  static int GetBlock(void* param,
                      unsigned long pos,
                      unsigned char* buf,
                      unsigned long size) {
    CountingStringFileAccess* file =
        static_cast<CountingStringFileAccess*>(param);
    if (!file || pos > file->data.size() || size > file->data.size() - pos) {
      return 0;
    }
    memcpy(buf, file->data.data() + pos, size);
    ++file->read_count;
    file->read_bytes += size;
    return 1;
  }

  std::string data;
  FPDF_FILEACCESS file_access = {};
  size_t read_count = 0;
  size_t read_bytes = 0;
};

uint64_t ReadUint64LEForTest(const char* data) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(static_cast<uint8_t>(data[i])) << (i * 8);
  }
  return value;
}

}  // namespace

class EPDFLayerEmbedderTest : public EmbedderTest {
 protected:
  void CheckReadOnlyLayerWorkflowProducesEmptyDelta(const char* file_name) {
    FileAccessForTesting base_access(file_name);
    EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
    ASSERT_TRUE(base) << file_name;

    EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
    ASSERT_TRUE(layer) << file_name;
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status) << file_name;
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get())) << file_name;

    const int page_count = FPDF_GetPageCount(layer.get());
    ASSERT_GT(page_count, 0) << file_name;
    for (int page_index = 0; page_index < page_count; ++page_index) {
      ScopedFPDFPage page(FPDF_LoadPage(layer.get(), page_index));
      ASSERT_TRUE(page) << file_name << " page " << page_index;
      ScopedFPDFBitmap bitmap = RenderPage(page.get());
      ASSERT_TRUE(bitmap) << file_name << " page " << page_index;

      const int annot_count = FPDFPage_GetAnnotCount(page.get());
      for (int annot_index = 0; annot_index < annot_count; ++annot_index) {
        ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), annot_index));
        ASSERT_TRUE(annot) << file_name << " annot " << annot_index;

        (void)FPDFAnnot_GetFlags(annot.get());
        (void)EPDFAnnot_GetBlendMode(annot.get());
        FPDF_STANDARD_FONT font = FPDF_FONT_UNKNOWN;
        float font_size = 0;
        unsigned int red = 0;
        unsigned int green = 0;
        unsigned int blue = 0;
        (void)EPDFAnnot_GetDefaultAppearance(
            annot.get(), &font, &font_size, &red, &green, &blue);
        const int object_count = FPDFAnnot_GetObjectCount(annot.get());
        if (object_count > 0) {
          EXPECT_TRUE(FPDFAnnot_GetObject(annot.get(), 0))
              << file_name << " annot " << annot_index;
        }

        if (FPDFAnnot_GetSubtype(annot.get()) == FPDF_ANNOT_LINK) {
          EXPECT_TRUE(FPDFAnnot_GetLink(annot.get()))
              << file_name << " annot " << annot_index;
        }
        if (FPDFAnnot_GetSubtype(annot.get()) == FPDF_ANNOT_FILEATTACHMENT) {
          EXPECT_TRUE(FPDFAnnot_GetFileAttachment(annot.get()))
              << file_name << " annot " << annot_index;
        }

        ScopedFPDFAnnotation linked(
            FPDFAnnot_GetLinkedAnnot(annot.get(), "IRT"));
      }

      int link_pos = 0;
      FPDF_LINK link = nullptr;
      while (FPDFLink_Enumerate(page.get(), &link_pos, &link)) {
        ASSERT_TRUE(link) << file_name << " page " << page_index;
      }
    }

    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get())) << file_name;

    ClearString();
    EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
    ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status))
        << file_name;
    EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status) << file_name;
    EXPECT_TRUE(GetString().empty()) << file_name;

    EPDF_ReleaseBaseDocument(base);
  }

  void CheckReadOnlyLayerParityProducesEmptyDelta(const char* file_name) {
    FileAccessForTesting plain_access(file_name);
    ScopedFPDFDocument plain(FPDF_LoadCustomDocument(&plain_access, nullptr));
    ASSERT_TRUE(plain) << file_name;

    FileAccessForTesting base_access(file_name);
    EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
    ASSERT_TRUE(base) << file_name;

    EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
    ASSERT_TRUE(layer) << file_name;
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status) << file_name;
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get())) << file_name;

    CompareDocumentReadApis(plain.get(), layer.get(), file_name);

    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get())) << file_name;

    ClearString();
    EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
    ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status))
        << file_name;
    EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status) << file_name;
    EXPECT_TRUE(GetString().empty()) << file_name;

    EPDF_ReleaseBaseDocument(base);
  }

  void CompareDocumentReadApis(FPDF_DOCUMENT plain,
                               FPDF_DOCUMENT layer,
                               const char* file_name) {
    const int plain_page_count = FPDF_GetPageCount(plain);
    EXPECT_EQ(plain_page_count, FPDF_GetPageCount(layer)) << file_name;

    CompareNamedDestReadApis(plain, layer, file_name);
    CompareAttachmentReadApis(plain, layer, file_name);
    CompareJavaScriptReadApis(plain, layer, file_name);
    CompareBookmarkReadApis(plain, layer, file_name);

    for (int page_index = -1; page_index <= plain_page_count; ++page_index) {
      ComparePageLabelReadApi(plain, layer, page_index, file_name);
    }

    for (int page_index = 0; page_index < plain_page_count; ++page_index) {
      FS_SIZEF plain_size;
      FS_SIZEF layer_size;
      const bool plain_has_size =
          FPDF_GetPageSizeByIndexF(plain, page_index, &plain_size);
      const bool layer_has_size =
          FPDF_GetPageSizeByIndexF(layer, page_index, &layer_size);
      EXPECT_EQ(plain_has_size, layer_has_size)
          << file_name << " page " << page_index;
      if (plain_has_size && layer_has_size) {
        EXPECT_FLOAT_EQ(plain_size.width, layer_size.width)
            << file_name << " page " << page_index;
        EXPECT_FLOAT_EQ(plain_size.height, layer_size.height)
            << file_name << " page " << page_index;
      }

      ScopedFPDFPage plain_page(FPDF_LoadPage(plain, page_index));
      ScopedFPDFPage layer_page(FPDF_LoadPage(layer, page_index));
      EXPECT_EQ(!!plain_page, !!layer_page)
          << file_name << " page " << page_index;
      if (!plain_page || !layer_page) {
        continue;
      }

      CompareLoadedPageReadApis(plain, plain_page.get(), layer,
                                layer_page.get(), file_name, page_index);
    }
  }

  void CompareNamedDestReadApis(FPDF_DOCUMENT plain,
                                FPDF_DOCUMENT layer,
                                const char* file_name) {
    const unsigned long plain_count = FPDF_CountNamedDests(plain);
    ASSERT_EQ(plain_count, FPDF_CountNamedDests(layer)) << file_name;

    static constexpr const char* kNamesToProbe[] = {
        "", "First", "Next", kFirstAlternate, kLastAlternate, "NoSuchName"};
    for (const char* name : kNamesToProbe) {
      EXPECT_EQ(!!FPDF_GetNamedDestByName(plain, name),
                !!FPDF_GetNamedDestByName(layer, name))
          << file_name << " named dest " << name;
    }

    for (unsigned long index = 0; index < plain_count; ++index) {
      char plain_buffer[512];
      char layer_buffer[512];
      long plain_size = sizeof(plain_buffer);
      long layer_size = sizeof(layer_buffer);
      const bool plain_has_dest =
          FPDF_GetNamedDest(plain, index, plain_buffer, &plain_size);
      const bool layer_has_dest =
          FPDF_GetNamedDest(layer, index, layer_buffer, &layer_size);
      EXPECT_EQ(plain_has_dest, layer_has_dest)
          << file_name << " named dest index " << index;
      EXPECT_EQ(plain_size, layer_size)
          << file_name << " named dest index " << index;
      if (plain_has_dest && layer_has_dest) {
        EXPECT_EQ(
            GetPlatformString(reinterpret_cast<FPDF_WIDESTRING>(plain_buffer)),
            GetPlatformString(reinterpret_cast<FPDF_WIDESTRING>(layer_buffer)))
            << file_name << " named dest index " << index;
      }
    }
  }

  void CompareAttachmentReadApis(FPDF_DOCUMENT plain,
                                 FPDF_DOCUMENT layer,
                                 const char* file_name) {
    const int plain_count = FPDFDoc_GetAttachmentCount(plain);
    ASSERT_EQ(plain_count, FPDFDoc_GetAttachmentCount(layer)) << file_name;
    for (int index = -1; index <= plain_count; ++index) {
      EXPECT_EQ(!!FPDFDoc_GetAttachment(plain, index),
                !!FPDFDoc_GetAttachment(layer, index))
          << file_name << " attachment " << index;
    }
  }

  void CompareJavaScriptReadApis(FPDF_DOCUMENT plain,
                                 FPDF_DOCUMENT layer,
                                 const char* file_name) {
    const int plain_count = FPDFDoc_GetJavaScriptActionCount(plain);
    ASSERT_EQ(plain_count, FPDFDoc_GetJavaScriptActionCount(layer))
        << file_name;
    for (int index = -1; index <= plain_count; ++index) {
      ScopedFPDFJavaScriptAction plain_js(
          FPDFDoc_GetJavaScriptAction(plain, index));
      ScopedFPDFJavaScriptAction layer_js(
          FPDFDoc_GetJavaScriptAction(layer, index));
      EXPECT_EQ(!!plain_js, !!layer_js)
          << file_name << " JavaScript action " << index;
      if (!plain_js || !layer_js) {
        continue;
      }
      EXPECT_EQ(FPDFJavaScriptAction_GetName(plain_js.get(), nullptr, 0),
                FPDFJavaScriptAction_GetName(layer_js.get(), nullptr, 0))
          << file_name << " JavaScript action " << index;
      EXPECT_EQ(FPDFJavaScriptAction_GetScript(plain_js.get(), nullptr, 0),
                FPDFJavaScriptAction_GetScript(layer_js.get(), nullptr, 0))
          << file_name << " JavaScript action " << index;
    }
  }

  void CompareBookmarkReadApis(FPDF_DOCUMENT plain,
                               FPDF_DOCUMENT layer,
                               const char* file_name) {
    CompareBookmarkSubtreeReadApis(
        plain, layer, FPDFBookmark_GetFirstChild(plain, nullptr),
        FPDFBookmark_GetFirstChild(layer, nullptr), file_name, 0);
  }

  void CompareBookmarkSubtreeReadApis(FPDF_DOCUMENT plain,
                                      FPDF_DOCUMENT layer,
                                      FPDF_BOOKMARK plain_bookmark,
                                      FPDF_BOOKMARK layer_bookmark,
                                      const char* file_name,
                                      int depth) {
    EXPECT_EQ(!!plain_bookmark, !!layer_bookmark)
        << file_name << " bookmark depth " << depth;
    if (!plain_bookmark || !layer_bookmark || depth >= 4) {
      return;
    }

    EXPECT_EQ(FPDFBookmark_GetTitle(plain_bookmark, nullptr, 0),
              FPDFBookmark_GetTitle(layer_bookmark, nullptr, 0))
        << file_name << " bookmark depth " << depth;
    EXPECT_EQ(FPDFBookmark_GetCount(plain_bookmark),
              FPDFBookmark_GetCount(layer_bookmark))
        << file_name << " bookmark depth " << depth;
    EXPECT_EQ(!!FPDFBookmark_GetDest(plain, plain_bookmark),
              !!FPDFBookmark_GetDest(layer, layer_bookmark))
        << file_name << " bookmark depth " << depth;
    EXPECT_EQ(!!FPDFBookmark_GetAction(plain_bookmark),
              !!FPDFBookmark_GetAction(layer_bookmark))
        << file_name << " bookmark depth " << depth;

    CompareBookmarkSubtreeReadApis(
        plain, layer, FPDFBookmark_GetFirstChild(plain, plain_bookmark),
        FPDFBookmark_GetFirstChild(layer, layer_bookmark), file_name,
        depth + 1);
    CompareBookmarkSubtreeReadApis(
        plain, layer, FPDFBookmark_GetNextSibling(plain, plain_bookmark),
        FPDFBookmark_GetNextSibling(layer, layer_bookmark), file_name, depth);
  }

  void ComparePageLabelReadApi(FPDF_DOCUMENT plain,
                               FPDF_DOCUMENT layer,
                               int page_index,
                               const char* file_name) {
    char plain_buffer[128];
    char layer_buffer[128];
    const unsigned long plain_size = FPDF_GetPageLabel(
        plain, page_index, plain_buffer, sizeof(plain_buffer));
    const unsigned long layer_size = FPDF_GetPageLabel(
        layer, page_index, layer_buffer, sizeof(layer_buffer));
    EXPECT_EQ(plain_size, layer_size)
        << file_name << " page label " << page_index;
    if (plain_size > 0 && plain_size <= sizeof(plain_buffer)) {
      EXPECT_EQ(
          GetPlatformString(reinterpret_cast<FPDF_WIDESTRING>(plain_buffer)),
          GetPlatformString(reinterpret_cast<FPDF_WIDESTRING>(layer_buffer)))
          << file_name << " page label " << page_index;
    }
  }

  void CompareLoadedPageReadApis(FPDF_DOCUMENT plain_doc,
                                 FPDF_PAGE plain_page,
                                 FPDF_DOCUMENT layer_doc,
                                 FPDF_PAGE layer_page,
                                 const char* file_name,
                                 int page_index) {
    EXPECT_FLOAT_EQ(FPDF_GetPageWidthF(plain_page),
                    FPDF_GetPageWidthF(layer_page))
        << file_name << " page " << page_index;
    EXPECT_FLOAT_EQ(FPDF_GetPageHeightF(plain_page),
                    FPDF_GetPageHeightF(layer_page))
        << file_name << " page " << page_index;

    CompareAnnotationReadApis(plain_page, layer_page, file_name, page_index);
    CompareLinkReadApis(plain_doc, plain_page, layer_doc, layer_page, file_name,
                        page_index);
    CompareTextReadApis(plain_page, layer_page, file_name, page_index);

    ScopedFPDFBitmap bitmap = RenderPage(layer_page);
    ASSERT_TRUE(bitmap) << file_name << " page " << page_index;
  }

  void CompareAnnotationReadApis(FPDF_PAGE plain_page,
                                 FPDF_PAGE layer_page,
                                 const char* file_name,
                                 int page_index) {
    const int plain_annot_count = FPDFPage_GetAnnotCount(plain_page);
    ASSERT_EQ(plain_annot_count, FPDFPage_GetAnnotCount(layer_page))
        << file_name << " page " << page_index;
    for (int annot_index = -1; annot_index <= plain_annot_count;
         ++annot_index) {
      ScopedFPDFAnnotation plain_annot(
          FPDFPage_GetAnnot(plain_page, annot_index));
      ScopedFPDFAnnotation layer_annot(
          FPDFPage_GetAnnot(layer_page, annot_index));
      EXPECT_EQ(!!plain_annot, !!layer_annot)
          << file_name << " page " << page_index << " annot " << annot_index;
    }
  }

  void CompareLinkReadApis(FPDF_DOCUMENT plain_doc,
                           FPDF_PAGE plain_page,
                           FPDF_DOCUMENT layer_doc,
                           FPDF_PAGE layer_page,
                           const char* file_name,
                           int page_index) {
    std::vector<FPDF_LINK> plain_links;
    std::vector<FPDF_LINK> layer_links;
    int pos = 0;
    FPDF_LINK link = nullptr;
    while (FPDFLink_Enumerate(plain_page, &pos, &link)) {
      plain_links.push_back(link);
    }
    pos = 0;
    link = nullptr;
    while (FPDFLink_Enumerate(layer_page, &pos, &link)) {
      layer_links.push_back(link);
    }
    ASSERT_EQ(plain_links.size(), layer_links.size())
        << file_name << " page " << page_index;
    for (size_t i = 0; i < plain_links.size(); ++i) {
      EXPECT_EQ(!!FPDFLink_GetDest(plain_doc, plain_links[i]),
                !!FPDFLink_GetDest(layer_doc, layer_links[i]))
          << file_name << " page " << page_index << " link " << i;
      EXPECT_EQ(!!FPDFLink_GetAction(plain_links[i]),
                !!FPDFLink_GetAction(layer_links[i]))
          << file_name << " page " << page_index << " link " << i;
      EXPECT_EQ(FPDFLink_CountQuadPoints(plain_links[i]),
                FPDFLink_CountQuadPoints(layer_links[i]))
          << file_name << " page " << page_index << " link " << i;
      EXPECT_EQ(!!FPDFLink_GetAnnot(plain_page, plain_links[i]),
                !!FPDFLink_GetAnnot(layer_page, layer_links[i]))
          << file_name << " page " << page_index << " link " << i;
    }
  }

  void CompareTextReadApis(FPDF_PAGE plain_page,
                           FPDF_PAGE layer_page,
                           const char* file_name,
                           int page_index) {
    ScopedFPDFTextPage plain_text(FPDFText_LoadPage(plain_page));
    ScopedFPDFTextPage layer_text(FPDFText_LoadPage(layer_page));
    EXPECT_EQ(!!plain_text, !!layer_text)
        << file_name << " page " << page_index;
    if (!plain_text || !layer_text) {
      return;
    }

    const int plain_char_count = FPDFText_CountChars(plain_text.get());
    ASSERT_EQ(plain_char_count, FPDFText_CountChars(layer_text.get()))
        << file_name << " page " << page_index;
    if (plain_char_count <= 0) {
      return;
    }

    EXPECT_EQ(FPDFText_GetUnicode(plain_text.get(), 0),
              FPDFText_GetUnicode(layer_text.get(), 0))
        << file_name << " page " << page_index;
    EXPECT_EQ(FPDFText_GetUnicode(plain_text.get(), plain_char_count - 1),
              FPDFText_GetUnicode(layer_text.get(), plain_char_count - 1))
        << file_name << " page " << page_index;
    EXPECT_EQ(FPDFText_CountRects(plain_text.get(), 0, plain_char_count),
              FPDFText_CountRects(layer_text.get(), 0, plain_char_count))
        << file_name << " page " << page_index;
  }
};

TEST_F(EPDFLayerEmbedderTest, LoadMemBaseDocument) {
  const std::string pdf_path = PathService::GetTestFilePath("rectangles.pdf");
  std::vector<uint8_t> file_bytes = GetFileContents(pdf_path.c_str());
  ASSERT_FALSE(file_bytes.empty());

  EPDF_BASE_DOCUMENT base = EPDF_LoadMemBaseDocument(
      file_bytes.data(), static_cast<int>(file_bytes.size()), nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);
  ASSERT_TRUE(layer);

  EXPECT_EQ(1, FPDF_GetPageCount(layer.get()));
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, LoadMemBaseDocument64) {
  const std::string pdf_path = PathService::GetTestFilePath("rectangles.pdf");
  std::vector<uint8_t> file_bytes = GetFileContents(pdf_path.c_str());
  ASSERT_FALSE(file_bytes.empty());

  EPDF_BASE_DOCUMENT base =
      EPDF_LoadMemBaseDocument64(file_bytes.data(), file_bytes.size(), nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
  ASSERT_TRUE(layer);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);
  EXPECT_EQ(1, FPDF_GetPageCount(layer.get()));
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, OpenFreshLayerRendersWithEmptyOverlay) {
  FileAccessForTesting base_access("rectangles.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  {
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &status));
    ASSERT_TRUE(layer);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    EXPECT_EQ(base, EPDFLayer_GetBaseDocument(layer.get()));
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));
    EXPECT_FALSE(EPDFLayer_IsObjectPromoted(layer.get(), 1));
    EXPECT_EQ(1, FPDF_GetPageCount(layer.get()));

    ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
    ASSERT_TRUE(page);
    EXPECT_FLOAT_EQ(200.0f, FPDF_GetPageWidthF(page.get()));
    EXPECT_FLOAT_EQ(300.0f, FPDF_GetPageHeightF(page.get()));
    EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
    ScopedFPDFBitmap bitmap = RenderPage(page.get());
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));
  }

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, OpenFreshLayerAnnotHandleDoesNotPromote) {
  FileAccessForTesting base_access("annotation_stamp_with_ap.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  {
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &status));
    ASSERT_TRUE(layer);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));

    ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
    ASSERT_TRUE(page);
    ASSERT_GT(FPDFPage_GetAnnotCount(page.get()), 0);

    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));

    const int object_count = FPDFAnnot_GetObjectCount(annot.get());
    EXPECT_GT(object_count, 0);
    EXPECT_TRUE(FPDFAnnot_GetObject(annot.get(), 0));
    (void)EPDFAnnot_GetBlendMode(annot.get());
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));

    ScopedFPDFBitmap bitmap(
        FPDFBitmap_Create(/*width=*/612, /*height=*/792, /*alpha=*/1));
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(FPDFBitmap_FillRect(bitmap.get(), 0, 0, 612, 792, 0xFFFFFFFF));
    const FS_MATRIX identity = {1, 0, 0, 1, 0, 0};
    EXPECT_TRUE(EPDF_RenderAnnotBitmap(bitmap.get(), page.get(), annot.get(),
                                       FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                                       /*state=*/nullptr, &identity, 0));
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));
  }

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest,
       ExistingAnnotHandlesAndDeltaReplaySeeEffectiveLayerGeometry) {
  FileAccessForTesting base_access("polygon_annot.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus status_a = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer_a(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status_a));
  ASSERT_TRUE(layer_a);
  ASSERT_EQ(EPDFLayerOpenStatus_kSuccess, status_a);

  EPDFLayerOpenStatus status_b = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer_b(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status_b));
  ASSERT_TRUE(layer_b);
  ASSERT_EQ(EPDFLayerOpenStatus_kSuccess, status_b);

  ScopedFPDFPage writer_page(FPDF_LoadPage(layer_a.get(), 0));
  ScopedFPDFPage reader_page(FPDF_LoadPage(layer_a.get(), 0));
  ScopedFPDFPage sibling_page(FPDF_LoadPage(layer_b.get(), 0));
  ASSERT_TRUE(writer_page);
  ASSERT_TRUE(reader_page);
  ASSERT_TRUE(sibling_page);

  ScopedFPDFAnnotation writer(FPDFPage_GetAnnot(writer_page.get(), 0));
  ScopedFPDFAnnotation reader(FPDFPage_GetAnnot(reader_page.get(), 0));
  ScopedFPDFAnnotation sibling(FPDFPage_GetAnnot(sibling_page.get(), 0));
  ASSERT_TRUE(writer);
  ASSERT_TRUE(reader);
  ASSERT_TRUE(sibling);

  ScopedFPDFBitmap original_bitmap =
      RenderPageWithFlags(sibling_page.get(), nullptr, FPDF_ANNOT);
  ASSERT_TRUE(original_bitmap);
  const std::string original_hash = HashBitmap(original_bitmap.get());

  FS_RECTF original_rect;
  ASSERT_TRUE(FPDFAnnot_GetRect(writer.get(), &original_rect));
  const FS_POINTF moved_vertices[] = {
      {176.0f, 319.0f},
      {367.0f, 434.0f},
      {489.0f, 266.42f},
  };

  ASSERT_TRUE(EPDFAnnot_SetVertices(writer.get(), moved_vertices,
                                    std::size(moved_vertices)));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(writer.get()));
  ASSERT_TRUE(FPDFPage_GenerateContent(writer_page.get()));

  // Drawing the appearance sets /Rect to what it paints around the moved
  // vertices.
  FS_RECTF moved_rect;
  ASSERT_TRUE(FPDFAnnot_GetRect(writer.get(), &moved_rect));
  EXPECT_NE(original_rect.left, moved_rect.left);
  for (const FS_POINTF& vertex : moved_vertices) {
    EXPECT_LE(moved_rect.left, vertex.x);
    EXPECT_GE(moved_rect.right, vertex.x);
    EXPECT_LE(moved_rect.bottom, vertex.y);
    EXPECT_GE(moved_rect.top, vertex.y);
  }

  FS_RECTF observed_rect;
  ASSERT_TRUE(FPDFAnnot_GetRect(reader.get(), &observed_rect));
  EXPECT_FLOAT_EQ(moved_rect.left, observed_rect.left);
  EXPECT_FLOAT_EQ(moved_rect.top, observed_rect.top);
  EXPECT_FLOAT_EQ(moved_rect.right, observed_rect.right);
  EXPECT_FLOAT_EQ(moved_rect.bottom, observed_rect.bottom);

  std::vector<FS_POINTF> observed_vertices(std::size(moved_vertices));
  ASSERT_EQ(observed_vertices.size(),
            FPDFAnnot_GetVertices(reader.get(), observed_vertices.data(),
                                  observed_vertices.size()));
  for (size_t i = 0; i < observed_vertices.size(); ++i) {
    EXPECT_FLOAT_EQ(moved_vertices[i].x, observed_vertices[i].x);
    EXPECT_FLOAT_EQ(moved_vertices[i].y, observed_vertices[i].y);
  }

  FS_RECTF sibling_rect;
  ASSERT_TRUE(FPDFAnnot_GetRect(sibling.get(), &sibling_rect));
  EXPECT_FLOAT_EQ(original_rect.left, sibling_rect.left);
  EXPECT_FLOAT_EQ(original_rect.top, sibling_rect.top);
  EXPECT_FLOAT_EQ(original_rect.right, sibling_rect.right);
  EXPECT_FLOAT_EQ(original_rect.bottom, sibling_rect.bottom);

  ScopedFPDFBitmap writer_bitmap =
      RenderPageWithFlags(writer_page.get(), nullptr, FPDF_ANNOT);
  ScopedFPDFBitmap reader_bitmap =
      RenderPageWithFlags(reader_page.get(), nullptr, FPDF_ANNOT);
  ScopedFPDFBitmap sibling_bitmap =
      RenderPageWithFlags(sibling_page.get(), nullptr, FPDF_ANNOT);
  ASSERT_TRUE(writer_bitmap);
  ASSERT_TRUE(reader_bitmap);
  ASSERT_TRUE(sibling_bitmap);
  const std::string moved_hash = HashBitmap(writer_bitmap.get());
  EXPECT_EQ(moved_hash, HashBitmap(reader_bitmap.get()));
  EXPECT_EQ(original_hash, HashBitmap(sibling_bitmap.get()));
  EXPECT_NE(original_hash, moved_hash);

  ClearString();
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(layer_a.get(), this, &save_status));
  ASSERT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  std::string delta = GetString();
  ASSERT_FALSE(delta.empty());

  FPDF_FILEACCESS delta_access = {};
  delta_access.m_FileLen = delta.size();
  delta_access.m_GetBlock = GetBlockFromString;
  delta_access.m_Param = &delta;
  EPDFLayerOpenStatus replay_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument replayed(
      EPDFLayer_OpenLayer(base, &delta_access, nullptr, &replay_status));
  ASSERT_TRUE(replayed);
  ASSERT_EQ(EPDFLayerOpenStatus_kSuccess, replay_status);

  ScopedFPDFPage replayed_page(FPDF_LoadPage(replayed.get(), 0));
  ASSERT_TRUE(replayed_page);
  ScopedFPDFAnnotation replayed_annot(
      FPDFPage_GetAnnot(replayed_page.get(), 0));
  ASSERT_TRUE(replayed_annot);

  FS_RECTF replayed_rect;
  ASSERT_TRUE(FPDFAnnot_GetRect(replayed_annot.get(), &replayed_rect));
  EXPECT_FLOAT_EQ(moved_rect.left, replayed_rect.left);
  EXPECT_FLOAT_EQ(moved_rect.top, replayed_rect.top);
  EXPECT_FLOAT_EQ(moved_rect.right, replayed_rect.right);
  EXPECT_FLOAT_EQ(moved_rect.bottom, replayed_rect.bottom);

  std::vector<FS_POINTF> replayed_vertices(std::size(moved_vertices));
  ASSERT_EQ(
      replayed_vertices.size(),
      FPDFAnnot_GetVertices(replayed_annot.get(), replayed_vertices.data(),
                            replayed_vertices.size()));
  for (size_t i = 0; i < replayed_vertices.size(); ++i) {
    EXPECT_FLOAT_EQ(moved_vertices[i].x, replayed_vertices[i].x);
    EXPECT_FLOAT_EQ(moved_vertices[i].y, replayed_vertices[i].y);
  }

  ScopedFPDFBitmap replayed_bitmap =
      RenderPageWithFlags(replayed_page.get(), nullptr, FPDF_ANNOT);
  ASSERT_TRUE(replayed_bitmap);
  EXPECT_EQ(moved_hash, HashBitmap(replayed_bitmap.get()));

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest,
       PromotedBaseAnnotReopensByObjectNumberFromEffectiveLayer) {
  FileAccessForTesting base_access("polygon_annot.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status));
  ASSERT_TRUE(layer);
  ASSERT_EQ(EPDFLayerOpenStatus_kSuccess, status);

  ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
  ASSERT_TRUE(page);

  ScopedFPDFAnnotation writer(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(writer);
  const uint32_t object_number = EPDFAnnot_GetObjectNumber(writer.get());
  ASSERT_GT(object_number, 0u);

  const FS_RECTF moved_rect = {
      160.0f,
      440.0f,
      500.0f,
      250.0f,
  };
  ASSERT_TRUE(FPDFAnnot_SetRect(writer.get(), &moved_rect));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  writer.reset();

  ScopedFPDFAnnotation reopened(
      EPDFPage_GetAnnotByObjectNumber(page.get(), object_number));
  ASSERT_TRUE(reopened);

  FS_RECTF observed_rect;
  ASSERT_TRUE(FPDFAnnot_GetRect(reopened.get(), &observed_rect));
  EXPECT_FLOAT_EQ(moved_rect.left, observed_rect.left);
  EXPECT_FLOAT_EQ(moved_rect.top, observed_rect.top);
  EXPECT_FLOAT_EQ(moved_rect.right, observed_rect.right);
  EXPECT_FLOAT_EQ(moved_rect.bottom, observed_rect.bottom);
  EXPECT_EQ(0, FPDFPage_GetAnnotIndex(page.get(), reopened.get()));

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, LinkCacheRebuildsAfterOverlayEpochChanges) {
  FileAccessForTesting base_access("annots.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status));
  ASSERT_TRUE(layer);
  ASSERT_EQ(EPDFLayerOpenStatus_kSuccess, status);

  ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
  ASSERT_TRUE(page);

  FPDF_LINK initial = FPDFLink_GetLinkAtPoint(page.get(), 69.0, 653.0);
  ASSERT_TRUE(initial);
  CPDF_Dictionary* initial_dict = CPDFDictionaryFromFPDFLink(initial);
  ASSERT_TRUE(initial_dict);
  ASSERT_GT(initial_dict->GetObjNum(), 0u);

  CPDF_Document* layer_doc = CPDFDocumentFromFPDFDocument(layer.get());
  ASSERT_TRUE(layer_doc);
  RetainPtr<CPDF_Dictionary> promoted = ToDictionary(
      layer_doc->GetMutableIndirectObject(initial_dict->GetObjNum()));
  ASSERT_TRUE(promoted);
  promoted->SetRectFor("Rect", CFX_FloatRect(300.0f, 300.0f, 350.0f, 350.0f));

  EXPECT_FALSE(FPDFLink_GetLinkAtPoint(page.get(), 69.0, 653.0));
  EXPECT_TRUE(FPDFLink_GetLinkAtPoint(page.get(), 325.0, 325.0));

  layer.reset();
  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, ReadOnlyLayerWorkflowsProduceEmptyDelta) {
  static constexpr const char* kFiles[] = {
      "links_highlights_annots.pdf",
      "multiple_form_types.pdf",
      "embedded_images.pdf",
      "annotation_stamp_with_ap.pdf",
  };

  for (const char* file_name : kFiles) {
    CheckReadOnlyLayerWorkflowProducesEmptyDelta(file_name);
  }
}

TEST_F(EPDFLayerEmbedderTest,
       ReadOnlyLayerRenderCacheMatrixProducesEmptyDelta) {
  static constexpr const char* kFiles[] = {
      "form_object.pdf",
      "form_object_with_text.pdf",
      "form_object_with_image.pdf",
      "form_object_with_path.pdf",
      "shared_form_xobject_matrix.pdf",
      "hello_world_2_pages_shared_resources_dict.pdf",
      "jpx_lzw.pdf",
      "rotated_image.pdf",
      "simple_thumbnail.pdf",
      "thumbnail_with_no_filters.pdf",
  };

  for (const char* file_name : kFiles) {
    CheckReadOnlyLayerWorkflowProducesEmptyDelta(file_name);
  }
}

TEST_F(EPDFLayerEmbedderTest, ReadOnlyLayerAnnotMatrixProducesEmptyDelta) {
  static constexpr const char* kFiles[] = {
      "annots.pdf",
      "annotation_markup_multiline_no_ap.pdf",
      "annotation_highlight_rollover_ap.pdf",
      "annotation_ink_multiple.pdf",
      "polygon_annot.pdf",
      "line_annot.pdf",
      "redact_annot.pdf",
      "annotation_fileattachment.pdf",
  };

  for (const char* file_name : kFiles) {
    CheckReadOnlyLayerWorkflowProducesEmptyDelta(file_name);
  }
}

TEST_F(EPDFLayerEmbedderTest, ReadOnlyLayerCatalogMatrixProducesEmptyDelta) {
  static constexpr const char* kFiles[] = {
      "embedded_attachments.pdf", "named_dests.pdf",    "page_labels.pdf",
      "tagged_mcr_multipage.pdf", "two_signatures.pdf",
  };

  for (const char* file_name : kFiles) {
    CheckReadOnlyLayerWorkflowProducesEmptyDelta(file_name);
  }
}

TEST_F(EPDFLayerEmbedderTest, ReadOnlyLayerFixtureParityProducesEmptyDelta) {
  static constexpr const char* kFiles[] = {
      "annots_action_handling.pdf",
      "bug_679649.pdf",
      "calculate.pdf",
      "document_aactions.pdf",
      "embedded_attachments.pdf",
      "embedded_images.pdf",
      "find_text_consecutive.pdf",
      "font_weight.pdf",
      "hello_world_2_pages_split_streams.pdf",
      "links_highlights_annots.pdf",
      "multiple_form_types.pdf",
      "named_dests_old_style.pdf",
      "page_labels.pdf",
      "tagged_actual_text.pdf",
      "tagged_mcr_multipage.pdf",
      "text_font.pdf",
      "use_outlines.pdf",
      "zero_length_stream.pdf",
  };

  for (const char* file_name : kFiles) {
    CheckReadOnlyLayerParityProducesEmptyDelta(file_name);
  }
}

TEST_F(EPDFLayerEmbedderTest,
       ReadOnlyLayerMalformedOldStyleNamedDestsProducesEmptyDelta) {
  FileAccessForTesting plain_access("named_dests_old_style.pdf");
  ScopedFPDFDocument plain(FPDF_LoadCustomDocument(&plain_access, nullptr));
  ASSERT_TRUE(plain);
  EXPECT_EQ(2, FPDF_GetPageCount(plain.get()));
  ScopedFPDFPage plain_page_0(FPDF_LoadPage(plain.get(), 0));
  EXPECT_TRUE(plain_page_0);
  ScopedFPDFPage plain_page_1(FPDF_LoadPage(plain.get(), 1));
  EXPECT_FALSE(plain_page_1);

  FileAccessForTesting base_access("named_dests_old_style.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
  ASSERT_TRUE(layer);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));
  EXPECT_EQ(2, FPDF_GetPageCount(layer.get()));

  EXPECT_EQ(2u, FPDF_CountNamedDests(layer.get()));
  EXPECT_FALSE(FPDF_GetNamedDestByName(layer.get(), nullptr));
  EXPECT_FALSE(FPDF_GetNamedDestByName(layer.get(), ""));
  EXPECT_FALSE(FPDF_GetNamedDestByName(layer.get(), "NoSuchName"));
  EXPECT_TRUE(FPDF_GetNamedDestByName(layer.get(), kFirstAlternate));
  EXPECT_TRUE(FPDF_GetNamedDestByName(layer.get(), kLastAlternate));

  char buffer[512];
  long size = sizeof(buffer);
  ASSERT_TRUE(FPDF_GetNamedDest(layer.get(), 0, buffer, &size));
  ASSERT_EQ(static_cast<int>(sizeof(kFirstAlternate) * 2), size);
  EXPECT_EQ(kFirstAlternate,
            GetPlatformString(reinterpret_cast<FPDF_WIDESTRING>(buffer)));

  ScopedFPDFPage layer_page_0(FPDF_LoadPage(layer.get(), 0));
  EXPECT_TRUE(layer_page_0);
  ScopedFPDFPage layer_page_1(FPDF_LoadPage(layer.get(), 1));
  EXPECT_FALSE(layer_page_1);
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));

  ClearString();
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  EXPECT_TRUE(GetString().empty());

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, ReadOnlyLayerNameTreeApisProduceEmptyDelta) {
  {
    FileAccessForTesting base_access("embedded_attachments.pdf");
    EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
    ASSERT_TRUE(base);

    EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
    ASSERT_TRUE(layer);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);

    ASSERT_EQ(2, FPDFDoc_GetAttachmentCount(layer.get()));
    EXPECT_TRUE(FPDFDoc_GetAttachment(layer.get(), 0));
    EXPECT_TRUE(FPDFDoc_GetAttachment(layer.get(), 1));
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));

    ClearString();
    EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
    ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status));
    EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
    EXPECT_TRUE(GetString().empty());

    EPDF_ReleaseBaseDocument(base);
  }

  {
    FileAccessForTesting base_access("bug_679649.pdf");
    EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
    ASSERT_TRUE(base);

    EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
    ASSERT_TRUE(layer);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);

    ASSERT_EQ(1, FPDFDoc_GetJavaScriptActionCount(layer.get()));
    ScopedFPDFJavaScriptAction js(FPDFDoc_GetJavaScriptAction(layer.get(), 0));
    EXPECT_TRUE(js);
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));

    ClearString();
    EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
    ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status));
    EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
    EXPECT_TRUE(GetString().empty());

    EPDF_ReleaseBaseDocument(base);
  }
}

TEST_F(EPDFLayerEmbedderTest, ReadOnlySiblingLayersStayEmptyAfterPeerMutates) {
  FileAccessForTesting base_access("embedded_images.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus status_a = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer_a(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status_a));
  ASSERT_TRUE(layer_a);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status_a);

  EPDFLayerOpenStatus status_b = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer_b(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status_b));
  ASSERT_TRUE(layer_b);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status_b);

  EPDFLayerOpenStatus status_c = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer_c(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status_c));
  ASSERT_TRUE(layer_c);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status_c);

  for (FPDF_DOCUMENT layer : {layer_a.get(), layer_b.get(), layer_c.get()}) {
    ScopedFPDFPage page(FPDF_LoadPage(layer, 0));
    ASSERT_TRUE(page);
    ScopedFPDFBitmap bitmap = RenderPage(page.get());
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer));
  }

  {
    ScopedFPDFPage page_b(FPDF_LoadPage(layer_b.get(), 0));
    ASSERT_TRUE(page_b);
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page_b.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(annot);
    EXPECT_GT(EPDFLayer_GetPromotedObjectCount(layer_b.get()), 0u);
  }

  for (FPDF_DOCUMENT read_only_layer : {layer_a.get(), layer_c.get()}) {
    ScopedFPDFPage page(FPDF_LoadPage(read_only_layer, 0));
    ASSERT_TRUE(page);
    ScopedFPDFBitmap bitmap = RenderPage(page.get());
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(read_only_layer));
  }

  ClearString();
  EPDFLayerSaveStatus save_status_a = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(layer_a.get(), this, &save_status_a));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status_a);
  EXPECT_TRUE(GetString().empty());

  ClearString();
  EPDFLayerSaveStatus save_status_b = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(layer_b.get(), this, &save_status_b));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status_b);
  EXPECT_FALSE(GetString().empty());

  ClearString();
  EPDFLayerSaveStatus save_status_c = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(layer_c.get(), this, &save_status_c));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status_c);
  EXPECT_TRUE(GetString().empty());

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, LayerMetadataUpdatePromotesInfoAndSavesDelta) {
  FileAccessForTesting base_access("bug_601362.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus status_a = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer_a(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status_a));
  ASSERT_TRUE(layer_a);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status_a);

  EPDFLayerOpenStatus status_b = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer_b(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status_b));
  ASSERT_TRUE(layer_b);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status_b);

  unsigned short buf[128];
  ASSERT_EQ(30u, FPDF_GetMetaText(layer_a.get(), "Creator", buf, sizeof(buf)));
  EXPECT_EQ(L"Microsoft Word", GetPlatformWString(buf));
  ASSERT_EQ(30u, FPDF_GetMetaText(layer_b.get(), "Creator", buf, sizeof(buf)));
  EXPECT_EQ(L"Microsoft Word", GetPlatformWString(buf));

  ScopedFPDFWideString layer_creator = GetFPDFWideString(L"Layer A Creator");
  ASSERT_TRUE(EPDF_SetMetaText(layer_a.get(), "Creator", layer_creator.get()));
  EXPECT_EQ(1u, EPDFLayer_GetPromotedObjectCount(layer_a.get()));

  ASSERT_EQ(32u, FPDF_GetMetaText(layer_a.get(), "Creator", buf, sizeof(buf)));
  EXPECT_EQ(L"Layer A Creator", GetPlatformWString(buf));
  ASSERT_EQ(30u, FPDF_GetMetaText(layer_b.get(), "Creator", buf, sizeof(buf)));
  EXPECT_EQ(L"Microsoft Word", GetPlatformWString(buf));
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer_b.get()));

  ClearString();
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(layer_a.get(), this, &save_status));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  std::string delta = GetString();
  EXPECT_FALSE(delta.empty());

  FPDF_FILEACCESS delta_access = {};
  delta_access.m_FileLen = delta.size();
  delta_access.m_GetBlock = GetBlockFromString;
  delta_access.m_Param = &delta;
  EPDFLayerOpenStatus replay_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument replayed(
      EPDFLayer_OpenLayer(base, &delta_access, nullptr, &replay_status));
  ASSERT_TRUE(replayed);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, replay_status);
  ASSERT_EQ(32u, FPDF_GetMetaText(replayed.get(), "Creator", buf, sizeof(buf)));
  EXPECT_EQ(L"Layer A Creator", GetPlatformWString(buf));

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, LayerMetadataUpdateCreatesInfoWhenBaseHasNone) {
  FileAccessForTesting base_access("rectangles.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
  ASSERT_TRUE(layer);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);

  unsigned short buf[128];
  EXPECT_EQ(0u, FPDF_GetMetaText(layer.get(), "Title", buf, sizeof(buf)));

  ScopedFPDFWideString title = GetFPDFWideString(L"Layer Title");
  ASSERT_TRUE(EPDF_SetMetaText(layer.get(), "Title", title.get()));
  EXPECT_EQ(1u, EPDFLayer_GetPromotedObjectCount(layer.get()));
  ASSERT_EQ(24u, FPDF_GetMetaText(layer.get(), "Title", buf, sizeof(buf)));
  EXPECT_EQ(L"Layer Title", GetPlatformWString(buf));

  ClearString();
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  std::string delta = GetString();
  EXPECT_FALSE(delta.empty());

  FPDF_FILEACCESS delta_access = {};
  delta_access.m_FileLen = delta.size();
  delta_access.m_GetBlock = GetBlockFromString;
  delta_access.m_Param = &delta;
  EPDFLayerOpenStatus replay_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument replayed(
      EPDFLayer_OpenLayer(base, &delta_access, nullptr, &replay_status));
  ASSERT_TRUE(replayed);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, replay_status);
  ASSERT_EQ(24u, FPDF_GetMetaText(replayed.get(), "Title", buf, sizeof(buf)));
  EXPECT_EQ(L"Layer Title", GetPlatformWString(buf));

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest,
       LayerDeleteBasePageSavesPromotedPageTreeWithoutNullReplacement) {
  FileAccessForTesting base_access("rectangles_multi_pages.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus status_a = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer_a(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status_a));
  ASSERT_TRUE(layer_a);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status_a);
  ASSERT_EQ(5, FPDF_GetPageCount(layer_a.get()));

  EPDFLayerOpenStatus status_b = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer_b(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status_b));
  ASSERT_TRUE(layer_b);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status_b);
  ASSERT_EQ(5, FPDF_GetPageCount(layer_b.get()));

  const unsigned int deleted_page_objnum =
      EPDFDoc_GetPageObjectNumberByIndex(layer_a.get(), 1);
  ASSERT_NE(0u, deleted_page_objnum);

  FPDFPage_Delete(layer_a.get(), 1);
  EXPECT_EQ(4, FPDF_GetPageCount(layer_a.get()));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(layer_a.get(), deleted_page_objnum));

  EXPECT_EQ(5, FPDF_GetPageCount(layer_b.get()));
  EXPECT_EQ(deleted_page_objnum,
            EPDFDoc_GetPageObjectNumberByIndex(layer_b.get(), 1));
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer_b.get()));

  ClearString();
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(layer_a.get(), this, &save_status));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  std::string delta = GetString();
  EXPECT_FALSE(delta.empty());

  FPDF_FILEACCESS delta_access = {};
  delta_access.m_FileLen = delta.size();
  delta_access.m_GetBlock = GetBlockFromString;
  delta_access.m_Param = &delta;
  EPDFLayerOpenStatus replay_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument replayed(
      EPDFLayer_OpenLayer(base, &delta_access, nullptr, &replay_status));
  ASSERT_TRUE(replayed);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, replay_status);
  ASSERT_EQ(4, FPDF_GetPageCount(replayed.get()));
  for (int i = 0; i < FPDF_GetPageCount(replayed.get()); ++i) {
    EXPECT_NE(deleted_page_objnum,
              EPDFDoc_GetPageObjectNumberByIndex(replayed.get(), i));
  }

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest,
       LayerFullSavePrunesDeletedBasePageButKeepsSharedObjects) {
  FileAccessForTesting base_access(
      "hello_world_2_pages_shared_resources_dict.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
  ASSERT_TRUE(layer);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);
  ASSERT_EQ(2, FPDF_GetPageCount(layer.get()));

  CPDF_Document* cpdf_layer = CPDFDocumentFromFPDFDocument(layer.get());
  RetainPtr<const CPDF_Dictionary> surviving_page =
      cpdf_layer->GetPageDictionary(0);
  RetainPtr<const CPDF_Dictionary> deleted_page =
      cpdf_layer->GetPageDictionary(1);
  ASSERT_TRUE(surviving_page);
  ASSERT_TRUE(deleted_page);

  const uint32_t surviving_page_objnum = surviving_page->GetObjNum();
  const uint32_t deleted_page_objnum = deleted_page->GetObjNum();
  const uint32_t shared_resources_objnum =
      GetReferencedObjectNumber(surviving_page.Get(), "Resources");
  const uint32_t deleted_page_resources_objnum =
      GetReferencedObjectNumber(deleted_page.Get(), "Resources");
  const uint32_t shared_contents_objnum =
      GetReferencedObjectNumber(surviving_page.Get(), "Contents");
  const uint32_t deleted_page_contents_objnum =
      GetReferencedObjectNumber(deleted_page.Get(), "Contents");

  ASSERT_NE(0u, surviving_page_objnum);
  ASSERT_NE(0u, deleted_page_objnum);
  ASSERT_NE(surviving_page_objnum, deleted_page_objnum);
  ASSERT_NE(0u, shared_resources_objnum);
  ASSERT_EQ(shared_resources_objnum, deleted_page_resources_objnum);
  ASSERT_NE(0u, shared_contents_objnum);
  ASSERT_EQ(shared_contents_objnum, deleted_page_contents_objnum);

  FPDFPage_Delete(layer.get(), 1);
  ASSERT_EQ(1, FPDF_GetPageCount(layer.get()));

  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(layer.get(), this, 0));
  std::string saved_pdf = GetString();
  EXPECT_FALSE(saved_pdf.empty());

  EXPECT_TRUE(PdfBytesContainObject(saved_pdf, surviving_page_objnum));
  EXPECT_FALSE(PdfBytesContainObject(saved_pdf, deleted_page_objnum));
  EXPECT_TRUE(PdfBytesContainObject(saved_pdf, shared_resources_objnum));
  EXPECT_TRUE(PdfBytesContainObject(saved_pdf, shared_contents_objnum));

  ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_doc);
  EXPECT_EQ(1, FPDF_GetPageCount(saved_doc.get()));
  EXPECT_EQ(surviving_page_objnum,
            EPDFDoc_GetPageObjectNumberByIndex(saved_doc.get(), 0));

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, LayerMovePagesPromotesMovedPageAndReplays) {
  FileAccessForTesting base_access("rectangles_multi_pages.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus status_a = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer_a(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status_a));
  ASSERT_TRUE(layer_a);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status_a);
  ASSERT_EQ(5, FPDF_GetPageCount(layer_a.get()));

  std::vector<unsigned int> original_order;
  for (int i = 0; i < FPDF_GetPageCount(layer_a.get()); ++i) {
    original_order.push_back(
        EPDFDoc_GetPageObjectNumberByIndex(layer_a.get(), i));
  }
  ASSERT_EQ(5u, original_order.size());

  EPDFLayerOpenStatus status_b = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer_b(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status_b));
  ASSERT_TRUE(layer_b);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status_b);

  int page_to_move = 2;
  ASSERT_TRUE(FPDF_MovePages(layer_a.get(), &page_to_move, 1, 1));
  EXPECT_EQ(5, FPDF_GetPageCount(layer_a.get()));
  EXPECT_EQ(original_order[0],
            EPDFDoc_GetPageObjectNumberByIndex(layer_a.get(), 0));
  EXPECT_EQ(original_order[2],
            EPDFDoc_GetPageObjectNumberByIndex(layer_a.get(), 1));
  EXPECT_EQ(original_order[1],
            EPDFDoc_GetPageObjectNumberByIndex(layer_a.get(), 2));
  EXPECT_EQ(original_order[3],
            EPDFDoc_GetPageObjectNumberByIndex(layer_a.get(), 3));
  EXPECT_EQ(original_order[4],
            EPDFDoc_GetPageObjectNumberByIndex(layer_a.get(), 4));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(layer_a.get(), original_order[2]));

  for (int i = 0; i < FPDF_GetPageCount(layer_b.get()); ++i) {
    EXPECT_EQ(original_order[i],
              EPDFDoc_GetPageObjectNumberByIndex(layer_b.get(), i));
  }
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer_b.get()));

  ClearString();
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(layer_a.get(), this, &save_status));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  std::string delta = GetString();
  EXPECT_FALSE(delta.empty());

  FPDF_FILEACCESS delta_access = {};
  delta_access.m_FileLen = delta.size();
  delta_access.m_GetBlock = GetBlockFromString;
  delta_access.m_Param = &delta;
  EPDFLayerOpenStatus replay_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument replayed(
      EPDFLayer_OpenLayer(base, &delta_access, nullptr, &replay_status));
  ASSERT_TRUE(replayed);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, replay_status);
  ASSERT_EQ(5, FPDF_GetPageCount(replayed.get()));
  EXPECT_EQ(original_order[0],
            EPDFDoc_GetPageObjectNumberByIndex(replayed.get(), 0));
  EXPECT_EQ(original_order[2],
            EPDFDoc_GetPageObjectNumberByIndex(replayed.get(), 1));
  EXPECT_EQ(original_order[1],
            EPDFDoc_GetPageObjectNumberByIndex(replayed.get(), 2));
  EXPECT_EQ(original_order[3],
            EPDFDoc_GetPageObjectNumberByIndex(replayed.get(), 3));
  EXPECT_EQ(original_order[4],
            EPDFDoc_GetPageObjectNumberByIndex(replayed.get(), 4));

  page_to_move = 2;
  ASSERT_TRUE(FPDF_MovePages(replayed.get(), &page_to_move, 1, 1));
  EXPECT_EQ(original_order[0],
            EPDFDoc_GetPageObjectNumberByIndex(replayed.get(), 0));
  EXPECT_EQ(original_order[1],
            EPDFDoc_GetPageObjectNumberByIndex(replayed.get(), 1));
  EXPECT_EQ(original_order[2],
            EPDFDoc_GetPageObjectNumberByIndex(replayed.get(), 2));
  EXPECT_EQ(original_order[3],
            EPDFDoc_GetPageObjectNumberByIndex(replayed.get(), 3));
  EXPECT_EQ(original_order[4],
            EPDFDoc_GetPageObjectNumberByIndex(replayed.get(), 4));

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, CreateAnnotOnFreshLayerPromotesOverlayOnly) {
  FileAccessForTesting base_access("rectangles.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  {
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &status));
    ASSERT_TRUE(layer);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));

    ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
    ASSERT_TRUE(page);
    EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));

    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(annot);
    EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
    EXPECT_EQ(2u, EPDFLayer_GetPromotedObjectCount(layer.get()));
  }

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, SaveLayerDeltaMaterializesWithBaseBytes) {
  FileAccessForTesting base_access("rectangles.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  std::string materialized;
  {
    EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
    ASSERT_TRUE(layer);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);

    ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(annot);
    EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));

    ClearString();
    EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
    ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status));
    EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
    const std::string delta = GetString();
    EXPECT_FALSE(delta.empty());
    EXPECT_FALSE(delta.starts_with("%PDF-"));

    FPDF_FILEACCESS delta_access = {};
    delta_access.m_FileLen = delta.size();
    delta_access.m_GetBlock = GetBlockFromString;
    delta_access.m_Param = const_cast<std::string*>(&delta);
    EPDFLayerOpenStatus replay_status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument replayed(
        EPDFLayer_OpenLayer(base, &delta_access, nullptr, &replay_status));
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, replay_status);
    ASSERT_TRUE(replayed);
    ScopedFPDFPage replayed_page(FPDF_LoadPage(replayed.get(), 0));
    ASSERT_TRUE(replayed_page);
    EXPECT_EQ(1, FPDFPage_GetAnnotCount(replayed_page.get()));

    const std::string pdf_path = PathService::GetTestFilePath("rectangles.pdf");
    ASSERT_FALSE(pdf_path.empty());
    std::vector<uint8_t> base_bytes = GetFileContents(pdf_path.c_str());
    ASSERT_FALSE(base_bytes.empty());
    EXPECT_LT(delta.size(), base_bytes.size());
    materialized.assign(reinterpret_cast<const char*>(base_bytes.data()),
                        base_bytes.size());
    materialized += delta;
  }

  ScopedFPDFDocument reopened(FPDF_LoadMemDocument64(
      materialized.data(), materialized.size(), nullptr));
  ASSERT_TRUE(reopened);
  ScopedFPDFPage reopened_page(FPDF_LoadPage(reopened.get(), 0));
  ASSERT_TRUE(reopened_page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(reopened_page.get()));

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, LayerDeltaReplayWithLeadingBytesInBase) {
  const std::string pdf_path = PathService::GetTestFilePath("rectangles.pdf");
  ASSERT_FALSE(pdf_path.empty());
  std::vector<uint8_t> file_bytes = GetFileContents(pdf_path.c_str());
  ASSERT_FALSE(file_bytes.empty());

  std::string base_bytes = "leading junk\n";
  base_bytes.append(reinterpret_cast<const char*>(file_bytes.data()),
                    file_bytes.size());
  CountingStringFileAccess base_access(base_bytes);
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(base_access.get(), nullptr);
  ASSERT_TRUE(base);

  std::string delta;
  {
    EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
    ASSERT_TRUE(layer);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);

    ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(annot);

    ClearString();
    EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
    ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status));
    EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
    delta = GetString();

    unsigned long artifact_size = 0;
    void* artifact_buffer = EPDFLayer_SaveLayerArtifactToOwnedBuffer(
        layer.get(), &artifact_size, &save_status);
    ASSERT_TRUE(artifact_buffer);
    std::string artifact(static_cast<const char*>(artifact_buffer),
                         artifact_size);
    EPDF_FreeBuffer(artifact_buffer);
    ASSERT_GE(artifact.size(), 40u);
    EXPECT_EQ(base_bytes.size(), ReadUint64LEForTest(artifact.data() + 16));
    EXPECT_LT(ReadUint64LEForTest(artifact.data() + 24),
              ReadUint64LEForTest(artifact.data() + 16));
  }

  FPDF_FILEACCESS delta_access = {};
  delta_access.m_FileLen = delta.size();
  delta_access.m_GetBlock = GetBlockFromString;
  delta_access.m_Param = &delta;
  EPDFLayerOpenStatus replay_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument replayed(
      EPDFLayer_OpenLayer(base, &delta_access, nullptr, &replay_status));
  ASSERT_TRUE(replayed);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, replay_status);
  ScopedFPDFPage replayed_page(FPDF_LoadPage(replayed.get(), 0));
  ASSERT_TRUE(replayed_page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(replayed_page.get()));

  const std::string materialized = base_bytes + delta;
  ScopedFPDFDocument stock_reopened(FPDF_LoadMemDocument64(
      materialized.data(), materialized.size(), nullptr));
  ASSERT_TRUE(stock_reopened);
  ScopedFPDFPage stock_page(FPDF_LoadPage(stock_reopened.get(), 0));
  ASSERT_TRUE(stock_page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(stock_page.get()));

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, LayerDeltaReplayWithTrailingBytesInBase) {
  const std::string pdf_path = PathService::GetTestFilePath("rectangles.pdf");
  ASSERT_FALSE(pdf_path.empty());
  std::vector<uint8_t> file_bytes = GetFileContents(pdf_path.c_str());
  ASSERT_FALSE(file_bytes.empty());

  std::string base_bytes(reinterpret_cast<const char*>(file_bytes.data()),
                         file_bytes.size());
  base_bytes += "\n% harmless trailing bytes\n";
  CountingStringFileAccess base_access(base_bytes);
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(base_access.get(), nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
  ASSERT_TRUE(layer);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);

  ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
  ASSERT_TRUE(annot);

  ClearString();
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  const std::string delta = GetString();

  FPDF_FILEACCESS delta_access = {};
  delta_access.m_FileLen = delta.size();
  delta_access.m_GetBlock = GetBlockFromString;
  delta_access.m_Param = const_cast<std::string*>(&delta);
  EPDFLayerOpenStatus replay_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument replayed(
      EPDFLayer_OpenLayer(base, &delta_access, nullptr, &replay_status));
  ASSERT_TRUE(replayed);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, replay_status);
  ScopedFPDFPage replayed_page(FPDF_LoadPage(replayed.get(), 0));
  ASSERT_TRUE(replayed_page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(replayed_page.get()));

  const std::string materialized = base_bytes + delta;
  ScopedFPDFDocument stock_reopened(FPDF_LoadMemDocument64(
      materialized.data(), materialized.size(), nullptr));
  ASSERT_TRUE(stock_reopened);
  ScopedFPDFPage stock_page(FPDF_LoadPage(stock_reopened.get(), 0));
  ASSERT_TRUE(stock_page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(stock_page.get()));

  EPDF_ReleaseBaseDocument(base);
}

// The base's identity (its SHA-256) is not computed at load: the first
// artifact save hashes the base once, later saves reuse it, and a host that
// supplies the hash never pays for the pass at all.
TEST_F(EPDFLayerEmbedderTest, LayerArtifactSaveHashesBaseOnceUnlessSupplied) {
  const std::string pdf_path = PathService::GetTestFilePath("rectangles.pdf");
  ASSERT_FALSE(pdf_path.empty());
  std::vector<uint8_t> file_bytes = GetFileContents(pdf_path.c_str());
  ASSERT_FALSE(file_bytes.empty());
  std::string base_bytes(reinterpret_cast<const char*>(file_bytes.data()),
                         file_bytes.size());

  auto save_artifact = [](FPDF_DOCUMENT layer) {
    unsigned long artifact_size = 0;
    EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
    void* artifact_buffer = EPDFLayer_SaveLayerArtifactToOwnedBuffer(
        layer, &artifact_size, &save_status);
    EXPECT_TRUE(artifact_buffer);
    EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
    EPDF_FreeBuffer(artifact_buffer);
  };

  {
    CountingStringFileAccess base_access(base_bytes);
    EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(base_access.get(), nullptr);
    ASSERT_TRUE(base);
    EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
    ASSERT_TRUE(layer);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);
    ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(annot);

    // First save: one full pass over the base to hash it.
    base_access.ResetCounts();
    save_artifact(layer.get());
    EXPECT_EQ(base_bytes.size(), base_access.read_bytes);
    // Second save: the identity is cached.
    base_access.ResetCounts();
    save_artifact(layer.get());
    EXPECT_EQ(0u, base_access.read_bytes);
    layer.reset();
    EPDF_ReleaseBaseDocument(base);
  }

  {
    CountingStringFileAccess base_access(base_bytes);
    EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(base_access.get(), nullptr);
    ASSERT_TRUE(base);
    unsigned char known[32];
    for (int i = 0; i < 32; ++i) known[i] = static_cast<unsigned char>(0xa0 + i);
    EPDF_SetBaseDocumentSha256(base, known);
    EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
    ASSERT_TRUE(layer);
    ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(annot);

    // Supplied: no pass over the base, and the artifact carries the claim.
    base_access.ResetCounts();
    save_artifact(layer.get());
    EXPECT_EQ(0u, base_access.read_bytes);
    unsigned char reported[32];
    ASSERT_TRUE(EPDFLayer_GetBaseSha256(layer.get(), reported));
    EXPECT_EQ(0, memcmp(reported, known, 32));
    layer.reset();
    EPDF_ReleaseBaseDocument(base);
  }
}

TEST_F(EPDFLayerEmbedderTest, LayerDiagnosticsRejectPlainDocuments) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  EXPECT_FALSE(EPDFLayer_GetBaseDocument(document()));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(document(), 1));
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(document()));

  ClearString();
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSuccess;
  EXPECT_FALSE(EPDFLayer_SaveDelta(document(), this, &save_status));
  EXPECT_EQ(EPDFLayerSaveStatus_kSaveFailed, save_status);
}

TEST_F(EPDFLayerEmbedderTest, LayerOwnedBufferAndArtifactReplay) {
  EPDF_FreeBuffer(nullptr);

  FileAccessForTesting base_access("rectangles.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
  ASSERT_TRUE(layer);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);

  ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
  ASSERT_TRUE(annot);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));

  unsigned long delta_size = 0;
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  void* delta_buffer =
      EPDFLayer_SaveDeltaToOwnedBuffer(layer.get(), &delta_size, &save_status);
  ASSERT_TRUE(delta_buffer);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  std::string delta(static_cast<const char*>(delta_buffer), delta_size);
  EPDF_FreeBuffer(delta_buffer);

  FPDF_FILEACCESS delta_access = {};
  delta_access.m_FileLen = delta.size();
  delta_access.m_GetBlock = GetBlockFromString;
  delta_access.m_Param = &delta;
  EPDFLayerOpenStatus delta_open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument replayed(
      EPDFLayer_OpenLayer(base, &delta_access, nullptr, &delta_open_status));
  ASSERT_TRUE(replayed);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, delta_open_status);
  ScopedFPDFPage replayed_page(FPDF_LoadPage(replayed.get(), 0));
  ASSERT_TRUE(replayed_page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(replayed_page.get()));

  unsigned long artifact_size = 0;
  save_status = EPDFLayerSaveStatus_kSaveFailed;
  void* artifact_buffer = EPDFLayer_SaveLayerArtifactToOwnedBuffer(
      layer.get(), &artifact_size, &save_status);
  ASSERT_TRUE(artifact_buffer);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  std::string artifact(static_cast<const char*>(artifact_buffer),
                       artifact_size);
  EPDF_FreeBuffer(artifact_buffer);

  ClearString();
  save_status = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveLayerArtifact(layer.get(), this, &save_status));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  std::string streamed_artifact = GetString();
  ASSERT_FALSE(streamed_artifact.empty());

  // Independent saves may produce different trailer /ID values. Layer
  // artifact correctness is replay validity, not byte-for-byte equality
  // between separately generated artifacts.
  FPDF_FILEACCESS artifact_access = {};
  artifact_access.m_FileLen = artifact.size();
  artifact_access.m_GetBlock = GetBlockFromString;
  artifact_access.m_Param = &artifact;
  EPDFLayerOpenStatus artifact_open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument artifact_replayed(EPDFLayer_OpenLayerArtifact(
      base, &artifact_access, nullptr, &artifact_open_status));
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, artifact_open_status);
  ASSERT_TRUE(artifact_replayed);
  ScopedFPDFPage artifact_page(FPDF_LoadPage(artifact_replayed.get(), 0));
  ASSERT_TRUE(artifact_page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(artifact_page.get()));

  FPDF_FILEACCESS streamed_artifact_access = {};
  streamed_artifact_access.m_FileLen = streamed_artifact.size();
  streamed_artifact_access.m_GetBlock = GetBlockFromString;
  streamed_artifact_access.m_Param = &streamed_artifact;
  EPDFLayerOpenStatus streamed_artifact_open_status =
      EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument streamed_artifact_replayed(
      EPDFLayer_OpenLayerArtifact(base, &streamed_artifact_access, nullptr,
                                  &streamed_artifact_open_status));
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, streamed_artifact_open_status);
  ASSERT_TRUE(streamed_artifact_replayed);
  ScopedFPDFPage streamed_artifact_page(
      FPDF_LoadPage(streamed_artifact_replayed.get(), 0));
  ASSERT_TRUE(streamed_artifact_page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(streamed_artifact_page.get()));

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, LayerArtifactIncludesNewAnnotObjectBodies) {
  FileAccessForTesting base_access("rectangles.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
  ASSERT_TRUE(layer);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);

  ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
  ASSERT_TRUE(page);
  std::vector<uint32_t> annot_objnums;
  for (int i = 0; i < 5; ++i) {
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(annot);
    const uint32_t objnum = EPDFAnnot_GetObjectNumber(annot.get());
    ASSERT_GT(objnum, 0u);
    annot_objnums.push_back(objnum);
  }
  EXPECT_EQ(5, FPDFPage_GetAnnotCount(page.get()));

  unsigned long artifact_size = 0;
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  void* artifact_buffer = EPDFLayer_SaveLayerArtifactToOwnedBuffer(
      layer.get(), &artifact_size, &save_status);
  ASSERT_TRUE(artifact_buffer);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  std::string artifact(static_cast<const char*>(artifact_buffer),
                       artifact_size);
  EPDF_FreeBuffer(artifact_buffer);
  ASSERT_FALSE(artifact.empty());

  for (uint32_t objnum : annot_objnums) {
    const std::string object_reference = std::to_string(objnum) + " 0 R";
    const std::string object_header = std::to_string(objnum) + " 0 obj";
    EXPECT_NE(std::string::npos, artifact.find(object_reference))
        << "Layer artifact should reference newly created annotation object "
        << objnum << ".";
    EXPECT_NE(std::string::npos, artifact.find(object_header))
        << "Layer artifact references annotation object " << objnum
        << " but does not contain its object body.";
  }

  EPDF_ReleaseBaseDocument(base);

  FileAccessForTesting replay_base_access("rectangles.pdf");
  EPDF_BASE_DOCUMENT replay_base =
      EPDF_LoadBaseDocument(&replay_base_access, nullptr);
  ASSERT_TRUE(replay_base);

  FPDF_FILEACCESS artifact_access = {};
  artifact_access.m_FileLen = artifact.size();
  artifact_access.m_GetBlock = GetBlockFromString;
  artifact_access.m_Param = &artifact;
  EPDFLayerOpenStatus artifact_open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument artifact_replayed(EPDFLayer_OpenLayerArtifact(
      replay_base, &artifact_access, nullptr, &artifact_open_status));
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, artifact_open_status);
  ASSERT_TRUE(artifact_replayed);
  ScopedFPDFPage artifact_page(FPDF_LoadPage(artifact_replayed.get(), 0));
  ASSERT_TRUE(artifact_page);
  ASSERT_EQ(5, FPDFPage_GetAnnotCount(artifact_page.get()));
  for (int i = 0; i < 5; ++i) {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(artifact_page.get(), i));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_TEXT, FPDFAnnot_GetSubtype(annot.get()));
  }

  EPDF_ReleaseBaseDocument(replay_base);
}

TEST_F(EPDFLayerEmbedderTest, LayerReplaySoakSmoke) {
  FileAccessForTesting base_access("rectangles.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  const std::string pdf_path = PathService::GetTestFilePath("rectangles.pdf");
  ASSERT_FALSE(pdf_path.empty());
  std::vector<uint8_t> base_bytes = GetFileContents(pdf_path.c_str());
  ASSERT_FALSE(base_bytes.empty());

  for (int expected_annots = 1; expected_annots <= 3; ++expected_annots) {
    std::string materialized;
    {
      EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
      ScopedFPDFDocument layer(
          EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
      ASSERT_TRUE(layer);
      EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);

      ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
      ASSERT_TRUE(page);
      for (int i = 0; i < expected_annots; ++i) {
        ScopedFPDFAnnotation annot(
            EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
        ASSERT_TRUE(annot);
      }
      EXPECT_EQ(expected_annots, FPDFPage_GetAnnotCount(page.get()));

      ClearString();
      EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
      ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status));
      EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
      materialized.assign(reinterpret_cast<const char*>(base_bytes.data()),
                          base_bytes.size());
      materialized += GetString();
    }

    ScopedFPDFDocument reopened(FPDF_LoadMemDocument64(
        materialized.data(), materialized.size(), nullptr));
    ASSERT_TRUE(reopened);
    ScopedFPDFPage reopened_page(FPDF_LoadPage(reopened.get(), 0));
    ASSERT_TRUE(reopened_page);
    EXPECT_EQ(expected_annots, FPDFPage_GetAnnotCount(reopened_page.get()));
  }

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, LayerDeltaReplaysMultipleAnnotRemoval) {
  FileAccessForTesting base_access("rectangles.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  const std::string pdf_path = PathService::GetTestFilePath("rectangles.pdf");
  ASSERT_FALSE(pdf_path.empty());
  std::vector<uint8_t> base_bytes = GetFileContents(pdf_path.c_str());
  ASSERT_FALSE(base_bytes.empty());

  EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
  ASSERT_TRUE(layer);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);

  ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
  ASSERT_TRUE(page);
  for (int i = 0; i < 3; ++i) {
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(annot);
  }
  ASSERT_EQ(3, FPDFPage_GetAnnotCount(page.get()));

  ASSERT_TRUE(FPDFPage_RemoveAnnot(page.get(), 1));
  ASSERT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

  ClearString();
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  const std::string delta = GetString();

  FPDF_FILEACCESS delta_access = {};
  delta_access.m_FileLen = delta.size();
  delta_access.m_GetBlock = GetBlockFromString;
  delta_access.m_Param = const_cast<std::string*>(&delta);
  EPDFLayerOpenStatus replay_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument replayed(
      EPDFLayer_OpenLayer(base, &delta_access, nullptr, &replay_status));
  ASSERT_TRUE(replayed);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, replay_status);
  ScopedFPDFPage replayed_page(FPDF_LoadPage(replayed.get(), 0));
  ASSERT_TRUE(replayed_page);
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(replayed_page.get()));

  std::string materialized(reinterpret_cast<const char*>(base_bytes.data()),
                           base_bytes.size());
  materialized += delta;
  ScopedFPDFDocument stock_reopened(FPDF_LoadMemDocument64(
      materialized.data(), materialized.size(), nullptr));
  ASSERT_TRUE(stock_reopened);
  ScopedFPDFPage stock_page(FPDF_LoadPage(stock_reopened.get(), 0));
  ASSERT_TRUE(stock_page);
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(stock_page.get()));

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFLayerEmbedderTest, LayerDeltaReplaysRemoveAllAnnots) {
  FileAccessForTesting base_access("rectangles.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
  ASSERT_TRUE(layer);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);

  ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
  ASSERT_TRUE(page);
  for (int i = 0; i < 3; ++i) {
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(annot);
  }
  ASSERT_EQ(3, FPDFPage_GetAnnotCount(page.get()));
  ASSERT_TRUE(FPDFPage_RemoveAnnot(page.get(), 2));
  ASSERT_TRUE(FPDFPage_RemoveAnnot(page.get(), 1));
  ASSERT_TRUE(FPDFPage_RemoveAnnot(page.get(), 0));
  ASSERT_EQ(0, FPDFPage_GetAnnotCount(page.get()));

  ClearString();
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
  const std::string delta = GetString();

  FPDF_FILEACCESS delta_access = {};
  delta_access.m_FileLen = delta.size();
  delta_access.m_GetBlock = GetBlockFromString;
  delta_access.m_Param = const_cast<std::string*>(&delta);
  EPDFLayerOpenStatus replay_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument replayed(
      EPDFLayer_OpenLayer(base, &delta_access, nullptr, &replay_status));
  ASSERT_TRUE(replayed);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, replay_status);
  ScopedFPDFPage replayed_page(FPDF_LoadPage(replayed.get(), 0));
  ASSERT_TRUE(replayed_page);
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(replayed_page.get()));

  EPDF_ReleaseBaseDocument(base);
}

// A layer draws the images inside content: a tiling pattern's one large cell,
// a pattern of small cells, a Type3 glyph. Each page renders as the file does
// on its own. The layer took that content from its base, parsed for the base,
// so its images were the base's, which the layer page's image cache refuses:
// the render crashed.
TEST_F(EPDFLayerEmbedderTest, LayerDrawsImagesInPatternsAndType3Glyphs) {
  static constexpr char kFile[] = "layer_content_images.pdf";
  static constexpr int kPageCount = 3;
  std::vector<std::string> expected;
  ASSERT_TRUE(OpenDocument(kFile));
  for (int page_index = 0; page_index < kPageCount; ++page_index) {
    ScopedPage page = LoadScopedPage(page_index);
    ASSERT_TRUE(page);
    ScopedFPDFBitmap bitmap = RenderPage(page.get());
    expected.push_back(HashBitmap(bitmap.get()));
  }

  FileAccessForTesting base_access(kFile);
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);
  {
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &status));
    ASSERT_TRUE(layer);
    for (int page_index = 0; page_index < kPageCount; ++page_index) {
      ScopedFPDFPage page(FPDF_LoadPage(layer.get(), page_index));
      ASSERT_TRUE(page);
      ScopedFPDFBitmap bitmap = RenderPage(page.get());
      EXPECT_EQ(expected[page_index], HashBitmap(bitmap.get()))
          << "page " << page_index;
    }
  }

  EPDF_ReleaseBaseDocument(base);
}
