// Copyright 2015 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <math.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "build/build_config.h"
#include "constants/page_object.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fxge/cfx_defaultrenderdevice.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/fpdf_view_c_api_test.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_attachment.h"
#include "public/fpdf_doc.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_javascript.h"
#include "public/fpdf_save.h"
#include "public/fpdf_text.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/embedder_test_constants.h"
#include "testing/embedder_test_environment.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/utils/file_util.h"
#include "testing/utils/hash.h"
#include "testing/utils/path_service.h"

#if defined(PDF_USE_SKIA)
#include "third_party/skia/include/core/SkCanvas.h"           // nogncheck
#include "third_party/skia/include/core/SkColor.h"            // nogncheck
#include "third_party/skia/include/core/SkColorType.h"        // nogncheck
#include "third_party/skia/include/core/SkImage.h"            // nogncheck
#include "third_party/skia/include/core/SkImageInfo.h"        // nogncheck
#include "third_party/skia/include/core/SkPicture.h"          // nogncheck
#include "third_party/skia/include/core/SkPictureRecorder.h"  // nogncheck
#include "third_party/skia/include/core/SkRefCnt.h"           // nogncheck
#include "third_party/skia/include/core/SkSize.h"             // nogncheck
#include "third_party/skia/include/core/SkSurface.h"          // nogncheck
#endif  // defined(PDF_USE_SKIA)

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

#if BUILDFLAG(IS_WIN)
const char kExpectedRectanglePostScript[] = R"(
save
/im/initmatrix load def
/n/newpath load def/m/moveto load def/l/lineto load def/c/curveto load def/h/closepath load def
/f/fill load def/F/eofill load def/s/stroke load def/W/clip load def/W*/eoclip load def
/rg/setrgbcolor load def/k/setcmykcolor load def
/J/setlinecap load def/j/setlinejoin load def/w/setlinewidth load def/M/setmiterlimit load def/d/setdash load def
/q/gsave load def/Q/grestore load def/iM/imagemask load def
/Tj/show load def/Ff/findfont load def/Fs/scalefont load def/Sf/setfont load def
/cm/concat load def/Cm/currentmatrix load def/mx/matrix load def/sm/setmatrix load def
0 300 m 0 0 l 200 0 l 200 300 l 0 300 l h W n
q
0 300 m 0 0 l 200 0 l 200 300 l 0 300 l h W n
q
0 J
[]0 d
0 j
1 w
10 M
mx Cm [1 0 0 -1 0 300]cm 0 290 m 10 290 l 10 300 l 0 300 l 0 290 l h 0 0 0 rg
q F Q s sm
mx Cm [1 0 0 -1 0 300]cm 10 150 m 60 150 l 60 180 l 10 180 l 10 150 l h q F Q s sm
mx Cm [1 0 0 -1 0 300]cm 190 290 m 200 290 l 200 300 l 190 300 l 190 290 l h 0 0 1 rg
q F Q 0 0 0 rg
s sm
mx Cm [1 0 0 -1 0 300]cm 70 232 m 120 232 l 120 262 l 70 262 l 70 232 l h 0 0 1 rg
q F Q 0 0 0 rg
s sm
mx Cm [1 0 0 -1 0 300]cm 190 0 m 200 0 l 200 10 l 190 10 l 190 0 l h 0 1 0 rg
q F Q 0 0 0 rg
s sm
mx Cm [1 0 0 -1 0 300]cm 130 150 m 180 150 l 180 180 l 130 180 l 130 150 l h 0 1 0 rg
q F Q 0 0 0 rg
s sm
mx Cm [1 0 0 -1 0 300]cm 0 0 m 10 0 l 10 10 l 0 10 l 0 0 l h 1 0 0 rg
q F Q 0 0 0 rg
s sm
mx Cm [1 0 0 -1 0 300]cm 70 67 m 120 67 l 120 97 l 70 97 l 70 67 l h 1 0 0 rg
q F Q 0 0 0 rg
s sm
Q
Q
Q

restore
)";
#endif  // BUILDFLAG(IS_WIN)

class MockDownloadHints final : public FX_DOWNLOADHINTS {
 public:
  static void SAddSegment(FX_DOWNLOADHINTS* pThis, size_t offset, size_t size) {
  }

  MockDownloadHints() {
    FX_DOWNLOADHINTS::version = 1;
    FX_DOWNLOADHINTS::AddSegment = SAddSegment;
  }

  ~MockDownloadHints() = default;
};

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

#if defined(PDF_USE_SKIA)
ScopedFPDFBitmap SkImageToPdfiumBitmap(const SkImage& image) {
  ScopedFPDFBitmap bitmap(
      FPDFBitmap_Create(image.width(), image.height(), /*alpha=*/1));
  if (!bitmap) {
    ADD_FAILURE() << "Could not create FPDF_BITMAP";
    return nullptr;
  }

  if (!image.readPixels(/*context=*/nullptr,
                        image.imageInfo().makeColorType(kBGRA_8888_SkColorType),
                        FPDFBitmap_GetBuffer(bitmap.get()),
                        FPDFBitmap_GetStride(bitmap.get()),
                        /*srcX=*/0, /*srcY=*/0)) {
    ADD_FAILURE() << "Could not read pixels from SkImage";
    return nullptr;
  }

  return bitmap;
}

ScopedFPDFBitmap SkPictureToPdfiumBitmap(sk_sp<SkPicture> picture,
                                         const SkISize& size) {
  sk_sp<SkSurface> surface =
      SkSurfaces::Raster(SkImageInfo::MakeN32Premul(size));
  if (!surface) {
    ADD_FAILURE() << "Could not create SkSurface";
    return nullptr;
  }

  surface->getCanvas()->clear(SK_ColorWHITE);
  surface->getCanvas()->drawPicture(picture);
  sk_sp<SkImage> image = surface->makeImageSnapshot();
  if (!image) {
    ADD_FAILURE() << "Could not snapshot SkSurface";
    return nullptr;
  }

  return SkImageToPdfiumBitmap(*image);
}
#endif  // defined(PDF_USE_SKIA)

}  // namespace

TEST(fpdf, CApiTest) {
  EXPECT_TRUE(CheckPDFiumCApi());
}

class FPDFViewEmbedderTest : public EmbedderTest {
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

  void TestRenderPageBitmapWithMatrix(FPDF_PAGE page,
                                      int bitmap_width,
                                      int bitmap_height,
                                      const FS_MATRIX& matrix,
                                      const FS_RECTF& rect,
                                      std::string_view expectation_png_name) {
    ScopedFPDFBitmap bitmap(FPDFBitmap_Create(bitmap_width, bitmap_height, 0));
    EXPECT_TRUE(FPDFBitmap_FillRect(bitmap.get(), 0, 0, bitmap_width,
                                    bitmap_height, 0xFFFFFFFF));
    FPDF_RenderPageBitmapWithMatrix(bitmap.get(), page, &matrix, &rect, 0);
    CompareBitmapWithExpectationSuffix(bitmap.get(), expectation_png_name);
  }

  void TestRenderPageBitmapWithFlags(FPDF_PAGE page,
                                     int flags,
                                     std::string_view expectation_png_name) {
    ScopedFPDFBitmap bitmap = TestRenderPageBitmapWithFlagsImpl(page, flags);
    ASSERT_TRUE(bitmap);
    CompareBitmapWithExpectationSuffix(bitmap.get(), expectation_png_name);
  }

  void TestRenderPageBitmapWithInternalMemory(
      FPDF_PAGE page,
      int format,
      std::string_view expectation_png_name,
      bool fuzzy = false) {
    TestRenderPageBitmapWithInternalMemoryAndStride(
        page, format, /*bitmap_stride=*/0, expectation_png_name, fuzzy);
  }

  void TestRenderPageBitmapWithInternalMemoryAndStride(
      FPDF_PAGE page,
      int format,
      int bitmap_stride,
      std::string_view expectation_png_name,
      bool fuzzy = false) {
    int bitmap_width = static_cast<int>(FPDF_GetPageWidth(page));
    int bitmap_height = static_cast<int>(FPDF_GetPageHeight(page));
    int bytes_per_pixel = BytesPerPixelForFormat(format);
    EXPECT_NE(0, bytes_per_pixel);

    ScopedFPDFBitmap bitmap(FPDFBitmap_CreateEx(
        bitmap_width, bitmap_height, format, nullptr, bitmap_stride));
    ASSERT_TRUE(bitmap);
    RenderPageToBitmapAndCheck(page, bitmap.get(), expectation_png_name, fuzzy);
  }

  int GetBitmapStride(FPDF_PAGE page, int format) {
    int bitmap_width = static_cast<int>(FPDF_GetPageWidth(page));
    int bytes_per_pixel = BytesPerPixelForFormat(format);
    EXPECT_NE(0, bytes_per_pixel);

    int bitmap_stride = bytes_per_pixel * bitmap_width;
    return bitmap_stride;
  }

  void TestRenderPageBitmapWithExternalMemory(
      FPDF_PAGE page,
      int format,
      std::string_view expectation_png_name,
      bool fuzzy = false) {
    int bitmap_stride = GetBitmapStride(page, format);
    return TestRenderPageBitmapWithExternalMemoryImpl(
        page, format, bitmap_stride, expectation_png_name, fuzzy);
  }

  void TestRenderPageBitmapWithExternalMemoryAndNoStride(
      FPDF_PAGE page,
      int format,
      std::string_view expectation_png_name,
      bool fuzzy = false) {
    return TestRenderPageBitmapWithExternalMemoryImpl(
        page, format, /*bitmap_stride=*/0, expectation_png_name, fuzzy);
  }

#if defined(PDF_USE_SKIA)
  void TestRenderPageSkp(FPDF_PAGE page, std::string_view png_name) {
    int width = static_cast<int>(FPDF_GetPageWidth(page));
    int height = static_cast<int>(FPDF_GetPageHeight(page));

    sk_sp<SkPicture> picture;
    {
      auto recorder = std::make_unique<SkPictureRecorder>();
      recorder->beginRecording(width, height);

      FPDF_RenderPageSkia(
          FPDFSkiaCanvasFromSkCanvas(recorder->getRecordingCanvas()), page,
          width, height);
      picture = recorder->finishRecordingAsPicture();
      EXPECT_TRUE(picture);
    }

    ScopedFPDFBitmap bitmap = SkPictureToPdfiumBitmap(
        std::move(picture), SkISize::Make(width, height));
    CompareBitmapWithExpectationSuffix(bitmap.get(), png_name);
  }
#endif  // defined(PDF_USE_SKIA)

 private:
  ScopedFPDFBitmap TestRenderPageBitmapWithFlagsImpl(FPDF_PAGE page,
                                                     int flags) {
    int bitmap_width = static_cast<int>(FPDF_GetPageWidth(page));
    int bitmap_height = static_cast<int>(FPDF_GetPageHeight(page));
    ScopedFPDFBitmap bitmap(FPDFBitmap_Create(bitmap_width, bitmap_height, 0));
    if (!FPDFBitmap_FillRect(bitmap.get(), 0, 0, bitmap_width, bitmap_height,
                             0xFFFFFFFF)) {
      return nullptr;
    }
    FPDF_RenderPageBitmap(bitmap.get(), page, 0, 0, bitmap_width, bitmap_height,
                          0, flags);
    return bitmap;
  }

  void TestRenderPageBitmapWithExternalMemoryImpl(
      FPDF_PAGE page,
      int format,
      int bitmap_stride,
      std::string_view expectation_png_name,
      bool fuzzy = false) {
    int bitmap_width = static_cast<int>(FPDF_GetPageWidth(page));
    int bitmap_height = static_cast<int>(FPDF_GetPageHeight(page));

    std::vector<uint8_t> external_memory(bitmap_stride * bitmap_height);
    ScopedFPDFBitmap bitmap(FPDFBitmap_CreateEx(bitmap_width, bitmap_height,
                                                format, external_memory.data(),
                                                bitmap_stride));
    RenderPageToBitmapAndCheck(page, bitmap.get(), expectation_png_name, fuzzy);
  }

  void RenderPageToBitmap(FPDF_PAGE page, FPDF_BITMAP bitmap) {
    int bitmap_width = FPDFBitmap_GetWidth(bitmap);
    int bitmap_height = FPDFBitmap_GetHeight(bitmap);
    EXPECT_EQ(bitmap_width, static_cast<int>(FPDF_GetPageWidth(page)));
    EXPECT_EQ(bitmap_height, static_cast<int>(FPDF_GetPageHeight(page)));
    ASSERT_TRUE(FPDFBitmap_FillRect(bitmap, 0, 0, bitmap_width, bitmap_height,
                                    0xFFFFFFFF));
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, bitmap_width, bitmap_height, 0,
                          FPDF_ANNOT);
  }
  void RenderPageToBitmapAndCheck(FPDF_PAGE page,
                                  FPDF_BITMAP bitmap,
                                  std::string_view expectation_png_name,
                                  bool fuzzy = false) {
    RenderPageToBitmap(page, bitmap);
    if (fuzzy) {
      CompareBitmapWithFuzzyExpectationSuffix(bitmap, expectation_png_name);
    } else {
      CompareBitmapWithExpectationSuffix(bitmap, expectation_png_name);
    }
  }
};

TEST_F(FPDFViewEmbedderTest, LoadMemBaseDocument) {
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

TEST_F(FPDFViewEmbedderTest, LoadMemBaseDocument64) {
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

// Test for conversion of a point in device coordinates to page coordinates
TEST_F(FPDFViewEmbedderTest, DeviceCoordinatesToPageCoordinates) {
  ASSERT_TRUE(OpenDocument("about_blank.pdf"));
  ScopedPage page = LoadScopedPage(0);
  EXPECT_TRUE(page);

  // Error tolerance for floating point comparison
  const double kTolerance = 0.0001;

  // Display bounds in device coordinates
  int start_x = 0;
  int start_y = 0;
  int size_x = 640;
  int size_y = 480;

  // Page Orientation normal
  int rotate = 0;

  // Device coordinate to be converted
  int device_x = 10;
  int device_y = 10;

  double page_x = 0.0;
  double page_y = 0.0;
  EXPECT_TRUE(FPDF_DeviceToPage(page.get(), start_x, start_y, size_x, size_y,
                                rotate, device_x, device_y, &page_x, &page_y));
  EXPECT_NEAR(9.5625, page_x, kTolerance);
  EXPECT_NEAR(775.5, page_y, kTolerance);

  // Rotate 90 degrees clockwise
  rotate = 1;
  EXPECT_TRUE(FPDF_DeviceToPage(page.get(), start_x, start_y, size_x, size_y,
                                rotate, device_x, device_y, &page_x, &page_y));
  EXPECT_NEAR(12.75, page_x, kTolerance);
  EXPECT_NEAR(12.375, page_y, kTolerance);

  // Rotate 180 degrees
  rotate = 2;
  EXPECT_TRUE(FPDF_DeviceToPage(page.get(), start_x, start_y, size_x, size_y,
                                rotate, device_x, device_y, &page_x, &page_y));
  EXPECT_NEAR(602.4374, page_x, kTolerance);
  EXPECT_NEAR(16.5, page_y, kTolerance);

  // Rotate 90 degrees counter-clockwise
  rotate = 3;
  EXPECT_TRUE(FPDF_DeviceToPage(page.get(), start_x, start_y, size_x, size_y,
                                rotate, device_x, device_y, &page_x, &page_y));
  EXPECT_NEAR(599.25, page_x, kTolerance);
  EXPECT_NEAR(779.625, page_y, kTolerance);

  // FPDF_DeviceToPage() converts |rotate| into legal rotation by taking
  // modulo by 4. A value of 4 is expected to be converted into 0 (normal
  // rotation)
  rotate = 4;
  EXPECT_TRUE(FPDF_DeviceToPage(page.get(), start_x, start_y, size_x, size_y,
                                rotate, device_x, device_y, &page_x, &page_y));
  EXPECT_NEAR(9.5625, page_x, kTolerance);
  EXPECT_NEAR(775.5, page_y, kTolerance);

  // FPDF_DeviceToPage returns untransformed coordinates if |rotate| % 4 is
  // negative.
  rotate = -1;
  EXPECT_TRUE(FPDF_DeviceToPage(page.get(), start_x, start_y, size_x, size_y,
                                rotate, device_x, device_y, &page_x, &page_y));
  EXPECT_NEAR(device_x, page_x, kTolerance);
  EXPECT_NEAR(device_y, page_y, kTolerance);

  // Negative case - invalid page
  page_x = 1234.0;
  page_y = 5678.0;
  EXPECT_FALSE(FPDF_DeviceToPage(nullptr, start_x, start_y, size_x, size_y,
                                 rotate, device_x, device_y, &page_x, &page_y));
  // Out parameters are expected to remain unchanged
  EXPECT_NEAR(1234.0, page_x, kTolerance);
  EXPECT_NEAR(5678.0, page_y, kTolerance);

  // Negative case - invalid output parameters
  EXPECT_FALSE(FPDF_DeviceToPage(page.get(), start_x, start_y, size_x, size_y,
                                 rotate, device_x, device_y, nullptr, nullptr));
}

// Test for conversion of a point in page coordinates to device coordinates.
TEST_F(FPDFViewEmbedderTest, PageCoordinatesToDeviceCoordinates) {
  ASSERT_TRUE(OpenDocument("about_blank.pdf"));
  ScopedPage page = LoadScopedPage(0);
  EXPECT_TRUE(page);

  // Display bounds in device coordinates
  int start_x = 0;
  int start_y = 0;
  int size_x = 640;
  int size_y = 480;

  // Page Orientation normal
  int rotate = 0;

  // Page coordinate to be converted
  double page_x = 9.0;
  double page_y = 775.0;

  int device_x = 0;
  int device_y = 0;
  EXPECT_TRUE(FPDF_PageToDevice(page.get(), start_x, start_y, size_x, size_y,
                                rotate, page_x, page_y, &device_x, &device_y));

  EXPECT_EQ(9, device_x);
  EXPECT_EQ(10, device_y);

  // Rotate 90 degrees clockwise
  rotate = 1;
  EXPECT_TRUE(FPDF_PageToDevice(page.get(), start_x, start_y, size_x, size_y,
                                rotate, page_x, page_y, &device_x, &device_y));
  EXPECT_EQ(626, device_x);
  EXPECT_EQ(7, device_y);

  // Rotate 180 degrees
  rotate = 2;
  EXPECT_TRUE(FPDF_PageToDevice(page.get(), start_x, start_y, size_x, size_y,
                                rotate, page_x, page_y, &device_x, &device_y));
  EXPECT_EQ(631, device_x);
  EXPECT_EQ(470, device_y);

  // Rotate 90 degrees counter-clockwise
  rotate = 3;
  EXPECT_TRUE(FPDF_PageToDevice(page.get(), start_x, start_y, size_x, size_y,
                                rotate, page_x, page_y, &device_x, &device_y));
  EXPECT_EQ(14, device_x);
  EXPECT_EQ(473, device_y);

  // FPDF_PageToDevice() converts |rotate| into legal rotation by taking
  // modulo by 4. A value of 4 is expected to be converted into 0 (normal
  // rotation)
  rotate = 4;
  EXPECT_TRUE(FPDF_PageToDevice(page.get(), start_x, start_y, size_x, size_y,
                                rotate, page_x, page_y, &device_x, &device_y));
  EXPECT_EQ(9, device_x);
  EXPECT_EQ(10, device_y);

  // FPDF_PageToDevice() returns untransformed coordinates if |rotate| % 4 is
  // negative.
  rotate = -1;
  EXPECT_TRUE(FPDF_PageToDevice(page.get(), start_x, start_y, size_x, size_y,
                                rotate, page_x, page_y, &device_x, &device_y));
  EXPECT_EQ(start_x, device_x);
  EXPECT_EQ(start_y, device_y);

  // Negative case - invalid page
  device_x = 1234;
  device_y = 5678;
  EXPECT_FALSE(FPDF_PageToDevice(nullptr, start_x, start_y, size_x, size_y,
                                 rotate, page_x, page_y, &device_x, &device_y));
  // Out parameters are expected to remain unchanged
  EXPECT_EQ(1234, device_x);
  EXPECT_EQ(5678, device_y);

  // Negative case - invalid output parameters
  EXPECT_FALSE(FPDF_PageToDevice(page.get(), start_x, start_y, size_x, size_y,
                                 rotate, page_x, page_y, nullptr, nullptr));
}

TEST_F(FPDFViewEmbedderTest, MultipleInitDestroy) {
  FPDF_InitLibrary();  // Redundant given SetUp() in environment, but safe.
  FPDF_InitLibrary();  // Doubly-redundant even, but safe.

  ASSERT_TRUE(OpenDocument("about_blank.pdf"));
  CloseDocument();
  CloseDocument();  // Redundant given above, but safe.
  CloseDocument();  // Doubly-redundant even, but safe.

  FPDF_DestroyLibrary();  // Doubly-Redundant even, but safe.
  FPDF_DestroyLibrary();  // Redundant given call to TearDown(), but safe.

  EmbedderTestEnvironment::GetInstance()->TearDown();
  EmbedderTestEnvironment::GetInstance()->SetUp();
}

TEST_F(FPDFViewEmbedderTest, RepeatedInitDestroy) {
  for (int i = 0; i < 3; ++i) {
    if (!OpenDocument("about_blank.pdf")) {
      ADD_FAILURE();
    }
    CloseDocument();

    FPDF_DestroyLibrary();
    FPDF_InitLibrary();
  }

  // Puts the test environment back the way it was.
  EmbedderTestEnvironment::GetInstance()->TearDown();
  EmbedderTestEnvironment::GetInstance()->SetUp();
}

TEST_F(FPDFViewEmbedderTest, Document) {
  ASSERT_TRUE(OpenDocument("about_blank.pdf"));
  EXPECT_EQ(1, GetPageCount());
  EXPECT_EQ(0, GetFirstPageNum());

  int version;
  EXPECT_TRUE(FPDF_GetFileVersion(document(), &version));
  EXPECT_EQ(14, version);

  // 0xFFFFFFFF because no security handler present
  EXPECT_EQ(0xFFFFFFFF, FPDF_GetDocPermissions(document()));
  EXPECT_EQ(0xFFFFFFFF, FPDF_GetDocUserPermissions(document()));
  EXPECT_EQ(-1, FPDF_GetSecurityHandlerRevision(document()));
  CloseDocument();

  // Safe to open again and do the same things all over again.
  ASSERT_TRUE(OpenDocument("about_blank.pdf"));
  EXPECT_EQ(1, GetPageCount());
  EXPECT_EQ(0, GetFirstPageNum());

  version = 42;
  EXPECT_TRUE(FPDF_GetFileVersion(document(), &version));
  EXPECT_EQ(14, version);

  // 0xFFFFFFFF because no security handler present
  EXPECT_EQ(0xFFFFFFFF, FPDF_GetDocPermissions(document()));
  EXPECT_EQ(0xFFFFFFFF, FPDF_GetDocUserPermissions(document()));
  EXPECT_EQ(-1, FPDF_GetSecurityHandlerRevision(document()));
  // CloseDocument() called by TearDown().
}

TEST_F(FPDFViewEmbedderTest, LoadDocument64) {
  std::string file_path = PathService::GetTestFilePath("about_blank.pdf");
  ASSERT_FALSE(file_path.empty());

  std::vector<uint8_t> file_contents = GetFileContents(file_path.c_str());
  ASSERT_FALSE(file_contents.empty());
  ScopedFPDFDocument doc(FPDF_LoadMemDocument64(file_contents.data(),
                                                file_contents.size(), nullptr));
  ASSERT_TRUE(doc);

  int version;
  EXPECT_TRUE(FPDF_GetFileVersion(doc.get(), &version));
  EXPECT_EQ(14, version);
}

TEST_F(FPDFViewEmbedderTest, LoadNonexistentDocument) {
  FPDF_DOCUMENT doc = FPDF_LoadDocument("nonexistent_document.pdf", "");
  ASSERT_FALSE(doc);
  EXPECT_EQ(static_cast<int>(FPDF_GetLastError()), FPDF_ERR_FILE);
}

TEST_F(FPDFViewEmbedderTest, DocumentWithNoPageCount) {
  ASSERT_TRUE(OpenDocument("no_page_count.pdf"));
  ASSERT_EQ(6, FPDF_GetPageCount(document()));
}

TEST_F(FPDFViewEmbedderTest, DocumentWithEmptyPageTreeNode) {
  ASSERT_TRUE(OpenDocument("page_tree_empty_node.pdf"));
  ASSERT_EQ(2, FPDF_GetPageCount(document()));
}

// See https://crbug.com/42271445
TEST_F(FPDFViewEmbedderTest, EmptyDocument) {
  CreateEmptyDocument();
  {
    int version = 42;
    EXPECT_FALSE(FPDF_GetFileVersion(document(), &version));
    EXPECT_EQ(0, version);
  }
  EXPECT_EQ(0U, FPDF_GetDocPermissions(document()));
  EXPECT_EQ(-1, FPDF_GetSecurityHandlerRevision(document()));
  EXPECT_EQ(0, FPDF_GetPageCount(document()));
  EXPECT_TRUE(FPDF_VIEWERREF_GetPrintScaling(document()));
  EXPECT_EQ(1, FPDF_VIEWERREF_GetNumCopies(document()));
  EXPECT_EQ(DuplexUndefined, FPDF_VIEWERREF_GetDuplex(document()));

  char buf[100];
  EXPECT_EQ(0U, FPDF_VIEWERREF_GetName(document(), "foo", nullptr, 0));
  EXPECT_EQ(0U, FPDF_VIEWERREF_GetName(document(), "foo", buf, sizeof(buf)));
  EXPECT_EQ(0u, FPDF_CountNamedDests(document()));
}

TEST_F(FPDFViewEmbedderTest, SandboxDocument) {
  uint16_t buf[200];
  unsigned long len;

  CreateEmptyDocument();
  len = FPDF_GetMetaText(document(), "CreationDate", buf, sizeof(buf));
  EXPECT_GT(len, 2u);  // Not just "double NUL" end-of-string indicator.
  EXPECT_NE(0u, buf[0]);
  CloseDocument();

  FPDF_SetSandBoxPolicy(FPDF_POLICY_MACHINETIME_ACCESS, false);
  CreateEmptyDocument();
  len = FPDF_GetMetaText(document(), "CreationDate", buf, sizeof(buf));
  EXPECT_EQ(2u, len);  // Only a "double NUL" end-of-string indicator.
  EXPECT_EQ(0u, buf[0]);
  CloseDocument();

  static constexpr unsigned long kNoSuchPolicy = 102;
  FPDF_SetSandBoxPolicy(kNoSuchPolicy, true);
  CreateEmptyDocument();
  len = FPDF_GetMetaText(document(), "CreationDate", buf, sizeof(buf));
  EXPECT_EQ(2u, len);  // Only a "double NUL" end-of-string indicator.
  EXPECT_EQ(0u, buf[0]);
  CloseDocument();

  FPDF_SetSandBoxPolicy(FPDF_POLICY_MACHINETIME_ACCESS, true);
  CreateEmptyDocument();
  len = FPDF_GetMetaText(document(), "CreationDate", buf, sizeof(buf));
  EXPECT_GT(len, 2u);  // Not just "double NUL" end-of-string indicator.
  EXPECT_NE(0u, buf[0]);
  CloseDocument();
}

TEST_F(FPDFViewEmbedderTest, LinearizedDocument) {
  ASSERT_TRUE(OpenDocumentLinearized("feature_linearized_loading.pdf"));
  int version;
  EXPECT_TRUE(FPDF_GetFileVersion(document(), &version));
  EXPECT_EQ(16, version);
}

TEST_F(FPDFViewEmbedderTest, LoadCustomDocumentWithoutFileAccess) {
  EXPECT_FALSE(FPDF_LoadCustomDocument(nullptr, ""));
}

// See https://crbug.com/42270260
TEST_F(FPDFViewEmbedderTest, LoadCustomDocumentWithShortLivedFileAccess) {
  std::string file_contents_string;  // Must outlive |doc|.
  ScopedFPDFDocument doc;
  {
    // Read a PDF, and copy it into |file_contents_string|.
    std::string pdf_path = PathService::GetTestFilePath("rectangles.pdf");
    ASSERT_FALSE(pdf_path.empty());
    std::vector<uint8_t> file_contents = GetFileContents(pdf_path.c_str());
    ASSERT_FALSE(file_contents.empty());
    std::copy(file_contents.begin(), file_contents.end(),
              std::back_inserter(file_contents_string));

    // Define a FPDF_FILEACCESS object that will go out of scope, while the
    // loaded document in |doc| remains valid.
    FPDF_FILEACCESS file_access = {};
    file_access.m_FileLen = file_contents_string.size();
    file_access.m_GetBlock = GetBlockFromString;
    file_access.m_Param = &file_contents_string;
    doc.reset(FPDF_LoadCustomDocument(&file_access, nullptr));
    ASSERT_TRUE(doc);
  }

  // Now try to access |doc| and make sure it still works.
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  EXPECT_FLOAT_EQ(200.0f, FPDF_GetPageWidthF(page.get()));
  EXPECT_FLOAT_EQ(300.0f, FPDF_GetPageHeightF(page.get()));
}

TEST_F(FPDFViewEmbedderTest, OpenFreshLayerRendersWithEmptyOverlay) {
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

TEST_F(FPDFViewEmbedderTest, OpenFreshLayerAnnotHandleDoesNotPromote) {
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
                                       &identity, 0));
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));
  }

  EPDF_ReleaseBaseDocument(base);
}

TEST_F(FPDFViewEmbedderTest,
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
  const FS_RECTF moved_rect = {
      160.0f,
      440.0f,
      500.0f,
      250.0f,
  };
  const FS_POINTF moved_vertices[] = {
      {176.0f, 319.0f},
      {367.0f, 434.0f},
      {489.0f, 266.42f},
  };

  ASSERT_TRUE(FPDFAnnot_SetRect(writer.get(), &moved_rect));
  ASSERT_TRUE(EPDFAnnot_SetVertices(writer.get(), moved_vertices,
                                    std::size(moved_vertices)));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(writer.get()));
  ASSERT_TRUE(FPDFPage_GenerateContent(writer_page.get()));

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

TEST_F(FPDFViewEmbedderTest,
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
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(writer.get()));
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

TEST_F(FPDFViewEmbedderTest, LinkCacheRebuildsAfterOverlayEpochChanges) {
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

TEST_F(FPDFViewEmbedderTest, ReadOnlyLayerWorkflowsProduceEmptyDelta) {
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

TEST_F(FPDFViewEmbedderTest, ReadOnlyLayerRenderCacheMatrixProducesEmptyDelta) {
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

TEST_F(FPDFViewEmbedderTest, ReadOnlyLayerAnnotMatrixProducesEmptyDelta) {
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

TEST_F(FPDFViewEmbedderTest, ReadOnlyLayerCatalogMatrixProducesEmptyDelta) {
  static constexpr const char* kFiles[] = {
      "embedded_attachments.pdf", "named_dests.pdf",    "page_labels.pdf",
      "tagged_mcr_multipage.pdf", "two_signatures.pdf",
  };

  for (const char* file_name : kFiles) {
    CheckReadOnlyLayerWorkflowProducesEmptyDelta(file_name);
  }
}

TEST_F(FPDFViewEmbedderTest, ReadOnlyLayerFixtureParityProducesEmptyDelta) {
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

TEST_F(FPDFViewEmbedderTest,
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

TEST_F(FPDFViewEmbedderTest, ReadOnlyLayerNameTreeApisProduceEmptyDelta) {
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

TEST_F(FPDFViewEmbedderTest, ReadOnlySiblingLayersStayEmptyAfterPeerMutates) {
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

TEST_F(FPDFViewEmbedderTest, LayerMetadataUpdatePromotesInfoAndSavesDelta) {
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

TEST_F(FPDFViewEmbedderTest, LayerMetadataUpdateCreatesInfoWhenBaseHasNone) {
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

TEST_F(FPDFViewEmbedderTest,
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

TEST_F(FPDFViewEmbedderTest,
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

TEST_F(FPDFViewEmbedderTest, LayerMovePagesPromotesMovedPageAndReplays) {
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

TEST_F(FPDFViewEmbedderTest, CreateAnnotOnFreshLayerPromotesOverlayOnly) {
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

TEST_F(FPDFViewEmbedderTest, SaveLayerDeltaMaterializesWithBaseBytes) {
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

TEST_F(FPDFViewEmbedderTest, LayerDeltaReplayWithLeadingBytesInBase) {
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

TEST_F(FPDFViewEmbedderTest, LayerDeltaReplayWithTrailingBytesInBase) {
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
TEST_F(FPDFViewEmbedderTest, LayerArtifactSaveHashesBaseOnceUnlessSupplied) {
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

TEST_F(FPDFViewEmbedderTest, LayerDiagnosticsRejectPlainDocuments) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  EXPECT_FALSE(EPDFLayer_GetBaseDocument(document()));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(document(), 1));
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(document()));

  ClearString();
  EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSuccess;
  EXPECT_FALSE(EPDFLayer_SaveDelta(document(), this, &save_status));
  EXPECT_EQ(EPDFLayerSaveStatus_kSaveFailed, save_status);
}

TEST_F(FPDFViewEmbedderTest, LayerOwnedBufferAndArtifactReplay) {
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

TEST_F(FPDFViewEmbedderTest, LayerArtifactIncludesNewAnnotObjectBodies) {
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

TEST_F(FPDFViewEmbedderTest, LayerReplaySoakSmoke) {
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

TEST_F(FPDFViewEmbedderTest, LayerDeltaReplaysMultipleAnnotRemoval) {
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

TEST_F(FPDFViewEmbedderTest, LayerDeltaReplaysRemoveAllAnnots) {
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

TEST_F(FPDFViewEmbedderTest, Page) {
  ASSERT_TRUE(OpenDocument("about_blank.pdf"));
  ScopedPage page = LoadScopedPage(0);
  EXPECT_TRUE(page);

  EXPECT_FLOAT_EQ(612.0f, FPDF_GetPageWidthF(page.get()));
  EXPECT_FLOAT_EQ(792.0f, FPDF_GetPageHeightF(page.get()));

  FS_RECTF rect;
  EXPECT_TRUE(FPDF_GetPageBoundingBox(page.get(), &rect));
  EXPECT_EQ(0.0, rect.left);
  EXPECT_EQ(0.0, rect.bottom);
  EXPECT_EQ(612.0, rect.right);
  EXPECT_EQ(792.0, rect.top);

  // Null arguments return errors rather than crashing,
  EXPECT_EQ(0.0, FPDF_GetPageWidth(nullptr));
  EXPECT_EQ(0.0, FPDF_GetPageHeight(nullptr));
  EXPECT_FALSE(FPDF_GetPageBoundingBox(nullptr, &rect));
  EXPECT_FALSE(FPDF_GetPageBoundingBox(page.get(), nullptr));

  EXPECT_FALSE(LoadScopedPage(1));
}

TEST_F(FPDFViewEmbedderTest, ViewerRefDummy) {
  ASSERT_TRUE(OpenDocument("about_blank.pdf"));
  EXPECT_TRUE(FPDF_VIEWERREF_GetPrintScaling(document()));
  EXPECT_EQ(1, FPDF_VIEWERREF_GetNumCopies(document()));
  EXPECT_EQ(DuplexUndefined, FPDF_VIEWERREF_GetDuplex(document()));

  char buf[100];
  EXPECT_EQ(0U, FPDF_VIEWERREF_GetName(document(), "foo", nullptr, 0));
  EXPECT_EQ(0U, FPDF_VIEWERREF_GetName(document(), "foo", buf, sizeof(buf)));

  FPDF_PAGERANGE page_range = FPDF_VIEWERREF_GetPrintPageRange(document());
  EXPECT_FALSE(page_range);
  EXPECT_EQ(0U, FPDF_VIEWERREF_GetPrintPageRangeCount(page_range));
  EXPECT_EQ(-1, FPDF_VIEWERREF_GetPrintPageRangeElement(page_range, 0));
  EXPECT_EQ(-1, FPDF_VIEWERREF_GetPrintPageRangeElement(page_range, 1));
}

TEST_F(FPDFViewEmbedderTest, ViewerRef) {
  ASSERT_TRUE(OpenDocument("viewer_ref.pdf"));
  EXPECT_TRUE(FPDF_VIEWERREF_GetPrintScaling(document()));
  EXPECT_EQ(5, FPDF_VIEWERREF_GetNumCopies(document()));
  EXPECT_EQ(DuplexUndefined, FPDF_VIEWERREF_GetDuplex(document()));

  // Test some corner cases.
  char buf[100];
  EXPECT_EQ(0U, FPDF_VIEWERREF_GetName(document(), "", buf, sizeof(buf)));
  EXPECT_EQ(0U, FPDF_VIEWERREF_GetName(document(), "foo", nullptr, 0));
  EXPECT_EQ(0U, FPDF_VIEWERREF_GetName(document(), "foo", buf, sizeof(buf)));

  // Make sure |buf| does not get written into when it appears to be too small.
  // NOLINTNEXTLINE(runtime/printf)
  UNSAFE_TODO(strcpy(buf, "ABCD"));
  EXPECT_EQ(4U, FPDF_VIEWERREF_GetName(document(), "Foo", buf, 1));
  EXPECT_STREQ("ABCD", buf);

  // Note "Foo" is a different key from "foo".
  EXPECT_EQ(4U, FPDF_VIEWERREF_GetName(document(), "Foo", nullptr, 0));
  ASSERT_EQ(4U, FPDF_VIEWERREF_GetName(document(), "Foo", buf, sizeof(buf)));
  EXPECT_STREQ("foo", buf);

  // Try to retrieve a boolean and an integer.
  EXPECT_EQ(
      0U, FPDF_VIEWERREF_GetName(document(), "HideToolbar", buf, sizeof(buf)));
  EXPECT_EQ(0U,
            FPDF_VIEWERREF_GetName(document(), "NumCopies", buf, sizeof(buf)));

  // Try more valid cases.
  ASSERT_EQ(4U,
            FPDF_VIEWERREF_GetName(document(), "Direction", buf, sizeof(buf)));
  EXPECT_STREQ("R2L", buf);
  ASSERT_EQ(8U,
            FPDF_VIEWERREF_GetName(document(), "ViewArea", buf, sizeof(buf)));
  EXPECT_STREQ("CropBox", buf);

  FPDF_PAGERANGE page_range = FPDF_VIEWERREF_GetPrintPageRange(document());
  EXPECT_TRUE(page_range);
  EXPECT_EQ(4U, FPDF_VIEWERREF_GetPrintPageRangeCount(page_range));
  EXPECT_EQ(0, FPDF_VIEWERREF_GetPrintPageRangeElement(page_range, 0));
  EXPECT_EQ(2, FPDF_VIEWERREF_GetPrintPageRangeElement(page_range, 1));
  EXPECT_EQ(4, FPDF_VIEWERREF_GetPrintPageRangeElement(page_range, 2));
  EXPECT_EQ(4, FPDF_VIEWERREF_GetPrintPageRangeElement(page_range, 3));
  EXPECT_EQ(-1, FPDF_VIEWERREF_GetPrintPageRangeElement(page_range, 4));
}

TEST_F(FPDFViewEmbedderTest, NamedDests) {
  ASSERT_TRUE(OpenDocument("named_dests.pdf"));
  EXPECT_EQ(6u, FPDF_CountNamedDests(document()));

  long buffer_size;
  char fixed_buffer[512];
  FPDF_DEST dest;

  // Query the size of the first item.
  buffer_size = 2000000;  // Absurdly large, check not used for this case.
  dest = FPDF_GetNamedDest(document(), 0, nullptr, &buffer_size);
  EXPECT_TRUE(dest);
  EXPECT_EQ(12, buffer_size);

  // Try to retrieve the first item with too small a buffer.
  buffer_size = 10;
  dest = FPDF_GetNamedDest(document(), 0, fixed_buffer, &buffer_size);
  EXPECT_TRUE(dest);
  EXPECT_EQ(-1, buffer_size);

  // Try to retrieve the first item with correctly sized buffer. Item is
  // taken from Dests NameTree in named_dests.pdf.
  buffer_size = 12;
  dest = FPDF_GetNamedDest(document(), 0, fixed_buffer, &buffer_size);
  EXPECT_TRUE(dest);
  EXPECT_EQ(12, buffer_size);
  EXPECT_EQ("First",
            GetPlatformString(reinterpret_cast<FPDF_WIDESTRING>(fixed_buffer)));

  // Try to retrieve the second item with ample buffer. Item is taken
  // from Dests NameTree but has a sub-dictionary in named_dests.pdf.
  buffer_size = sizeof(fixed_buffer);
  dest = FPDF_GetNamedDest(document(), 1, fixed_buffer, &buffer_size);
  EXPECT_TRUE(dest);
  EXPECT_EQ(10, buffer_size);
  EXPECT_EQ("Next",
            GetPlatformString(reinterpret_cast<FPDF_WIDESTRING>(fixed_buffer)));

  // Try to retrieve third item with ample buffer. Item is taken
  // from Dests NameTree but has a bad sub-dictionary in named_dests.pdf.
  // in named_dests.pdf).
  buffer_size = sizeof(fixed_buffer);
  dest = FPDF_GetNamedDest(document(), 2, fixed_buffer, &buffer_size);
  EXPECT_FALSE(dest);
  EXPECT_EQ(sizeof(fixed_buffer),
            static_cast<size_t>(buffer_size));  // unmodified.

  // Try to retrieve the forth item with ample buffer. Item is taken
  // from Dests NameTree but has a vale of the wrong type in named_dests.pdf.
  buffer_size = sizeof(fixed_buffer);
  dest = FPDF_GetNamedDest(document(), 3, fixed_buffer, &buffer_size);
  EXPECT_FALSE(dest);
  EXPECT_EQ(sizeof(fixed_buffer),
            static_cast<size_t>(buffer_size));  // unmodified.

  // Try to retrieve fifth item with ample buffer. Item taken from the
  // old-style Dests dictionary object in named_dests.pdf.
  buffer_size = sizeof(fixed_buffer);
  dest = FPDF_GetNamedDest(document(), 4, fixed_buffer, &buffer_size);
  EXPECT_TRUE(dest);
  EXPECT_EQ(30, buffer_size);
  EXPECT_EQ(kFirstAlternate,
            GetPlatformString(reinterpret_cast<FPDF_WIDESTRING>(fixed_buffer)));

  // Try to retrieve sixth item with ample buffer. Item istaken from the
  // old-style Dests dictionary object but has a sub-dictionary in
  // named_dests.pdf.
  buffer_size = sizeof(fixed_buffer);
  dest = FPDF_GetNamedDest(document(), 5, fixed_buffer, &buffer_size);
  EXPECT_TRUE(dest);
  EXPECT_EQ(28, buffer_size);
  EXPECT_EQ(kLastAlternate,
            GetPlatformString(reinterpret_cast<FPDF_WIDESTRING>(fixed_buffer)));

  // Try to retrieve non-existent item with ample buffer.
  buffer_size = sizeof(fixed_buffer);
  dest = FPDF_GetNamedDest(document(), 6, fixed_buffer, &buffer_size);
  EXPECT_FALSE(dest);
  EXPECT_EQ(sizeof(fixed_buffer),
            static_cast<size_t>(buffer_size));  // unmodified.

  // Try to underflow/overflow the integer index.
  buffer_size = sizeof(fixed_buffer);
  dest = FPDF_GetNamedDest(document(), std::numeric_limits<int>::max(),
                           fixed_buffer, &buffer_size);
  EXPECT_FALSE(dest);
  EXPECT_EQ(sizeof(fixed_buffer),
            static_cast<size_t>(buffer_size));  // unmodified.

  buffer_size = sizeof(fixed_buffer);
  dest = FPDF_GetNamedDest(document(), std::numeric_limits<int>::min(),
                           fixed_buffer, &buffer_size);
  EXPECT_FALSE(dest);
  EXPECT_EQ(sizeof(fixed_buffer),
            static_cast<size_t>(buffer_size));  // unmodified.

  buffer_size = sizeof(fixed_buffer);
  dest = FPDF_GetNamedDest(document(), -1, fixed_buffer, &buffer_size);
  EXPECT_FALSE(dest);
  EXPECT_EQ(sizeof(fixed_buffer),
            static_cast<size_t>(buffer_size));  // unmodified.
}

TEST_F(FPDFViewEmbedderTest, NamedDestsByName) {
  ASSERT_TRUE(OpenDocument("named_dests.pdf"));

  // Null pointer returns nullptr.
  FPDF_DEST dest = FPDF_GetNamedDestByName(document(), nullptr);
  EXPECT_FALSE(dest);

  // Empty string returns nullptr.
  dest = FPDF_GetNamedDestByName(document(), "");
  EXPECT_FALSE(dest);

  // Item from Dests NameTree.
  dest = FPDF_GetNamedDestByName(document(), "First");
  EXPECT_TRUE(dest);

  long ignore_len = 0;
  FPDF_DEST dest_by_index =
      FPDF_GetNamedDest(document(), 0, nullptr, &ignore_len);
  EXPECT_EQ(dest_by_index, dest);

  // Item from Dests dictionary.
  dest = FPDF_GetNamedDestByName(document(), kFirstAlternate);
  EXPECT_TRUE(dest);

  ignore_len = 0;
  dest_by_index = FPDF_GetNamedDest(document(), 4, nullptr, &ignore_len);
  EXPECT_EQ(dest_by_index, dest);

  // Bad value type for item from Dests NameTree array.
  dest = FPDF_GetNamedDestByName(document(), "WrongType");
  EXPECT_FALSE(dest);

  // No such destination in either Dest NameTree or dictionary.
  dest = FPDF_GetNamedDestByName(document(), "Bogus");
  EXPECT_FALSE(dest);
}

TEST_F(FPDFViewEmbedderTest, NamedDestsOldStyle) {
  ASSERT_TRUE(OpenDocument("named_dests_old_style.pdf"));
  EXPECT_EQ(2u, FPDF_CountNamedDests(document()));

  // Test bad parameters.
  EXPECT_FALSE(FPDF_GetNamedDestByName(document(), nullptr));
  EXPECT_FALSE(FPDF_GetNamedDestByName(document(), ""));
  EXPECT_FALSE(FPDF_GetNamedDestByName(document(), "NoSuchName"));

  // These should return a valid destination.
  EXPECT_TRUE(FPDF_GetNamedDestByName(document(), kFirstAlternate));
  EXPECT_TRUE(FPDF_GetNamedDestByName(document(), kLastAlternate));

  char buffer[512];
  static constexpr long kBufferSize = sizeof(buffer);
  long size = kBufferSize;

  // Test bad indices.
  EXPECT_FALSE(FPDF_GetNamedDest(document(), -1, buffer, &size));
  EXPECT_EQ(kBufferSize, size);
  size = kBufferSize;
  EXPECT_FALSE(FPDF_GetNamedDest(document(), 2, buffer, &size));
  EXPECT_EQ(kBufferSize, size);

  // These should return a valid destination.
  size = kBufferSize;
  ASSERT_TRUE(FPDF_GetNamedDest(document(), 0, buffer, &size));
  ASSERT_EQ(static_cast<int>(sizeof(kFirstAlternate) * 2), size);
  EXPECT_EQ(kFirstAlternate,
            GetPlatformString(reinterpret_cast<FPDF_WIDESTRING>(buffer)));
  size = kBufferSize;
  ASSERT_TRUE(FPDF_GetNamedDest(document(), 1, buffer, &size));
  ASSERT_EQ(static_cast<int>(sizeof(kLastAlternate) * 2), size);
  EXPECT_EQ(kLastAlternate,
            GetPlatformString(reinterpret_cast<FPDF_WIDESTRING>(buffer)));
}

// The following tests pass if the document opens without crashing.
TEST_F(FPDFViewEmbedderTest, Crasher113) {
  ASSERT_TRUE(OpenDocument("bug_113.pdf"));
}

TEST_F(FPDFViewEmbedderTest, Crasher451830) {
  // Document is damaged and can't be opened.
  EXPECT_FALSE(OpenDocument("bug_451830.pdf"));
}

TEST_F(FPDFViewEmbedderTest, Crasher452455) {
  ASSERT_TRUE(OpenDocument("bug_452455.pdf"));
  ScopedPage page = LoadScopedPage(0);
  EXPECT_TRUE(page);
}

TEST_F(FPDFViewEmbedderTest, Crasher454695) {
  // Document is damaged and can't be opened.
  EXPECT_FALSE(OpenDocument("bug_454695.pdf"));
}

TEST_F(FPDFViewEmbedderTest, Crasher572871) {
  ASSERT_TRUE(OpenDocument("bug_572871.pdf"));
}

// It tests that document can still be loaded even the trailer has no 'Size'
// field if other information is right.
TEST_F(FPDFViewEmbedderTest, Failed213) {
  ASSERT_TRUE(OpenDocument("bug_213.pdf"));
}

// The following tests pass if the document opens without infinite looping.
TEST_F(FPDFViewEmbedderTest, Hang298) {
  EXPECT_FALSE(OpenDocument("bug_298.pdf"));
}

TEST_F(FPDFViewEmbedderTest, Crasher773229) {
  ASSERT_TRUE(OpenDocument("bug_773229.pdf"));
}

// Test if the document opens without infinite looping.
// Previously this test will hang in a loop inside LoadAllCrossRefV4. After
// the fix, LoadAllCrossRefV4 will return false after detecting a cross
// reference loop. Cross references will be rebuilt successfully.
TEST_F(FPDFViewEmbedderTest, CrossRefV4Loop) {
  ASSERT_TRUE(OpenDocument("bug_xrefv4_loop.pdf"));
  MockDownloadHints hints;

  // Make sure calling FPDFAvail_IsDocAvail() on this file does not infinite
  // loop either. See bug 875.
  int ret = PDF_DATA_NOTAVAIL;
  while (ret == PDF_DATA_NOTAVAIL) {
    ret = FPDFAvail_IsDocAvail(avail(), &hints);
  }
  EXPECT_EQ(PDF_DATA_AVAIL, ret);
}

// The test should pass when circular references to ParseIndirectObject will not
// cause infinite loop.
TEST_F(FPDFViewEmbedderTest, Hang343) {
  EXPECT_FALSE(OpenDocument("bug_343.pdf"));
}

// The test should pass when the absence of 'Contents' field in a signature
// dictionary will not cause an infinite loop in CPDF_SyntaxParser::GetObject().
TEST_F(FPDFViewEmbedderTest, Hang344) {
  EXPECT_FALSE(OpenDocument("bug_344.pdf"));
}

// The test should pass when there is no infinite recursion in
// CPDF_SyntaxParser::GetString().
TEST_F(FPDFViewEmbedderTest, Hang355) {
  EXPECT_FALSE(OpenDocument("bug_355.pdf"));
}
// The test should pass even when the file has circular references to pages.
TEST_F(FPDFViewEmbedderTest, Hang360) {
  EXPECT_FALSE(OpenDocument("bug_360.pdf"));
}

// Deliberately damaged version of linearized.pdf with bad data in the shared
// object hint table.
TEST_F(FPDFViewEmbedderTest, Hang1055) {
  ASSERT_TRUE(OpenDocumentLinearized("linearized_bug_1055.pdf"));
  int version;
  EXPECT_TRUE(FPDF_GetFileVersion(document(), &version));
  EXPECT_EQ(16, version);
}

TEST_F(FPDFViewEmbedderTest, FPDFRenderPageBitmapWithMatrix) {
  constexpr char kClippedRectanglesBasename[] = "rectangles_clipped";
  constexpr char kRectanglesTopLeftBasename[] = "rectangles_top_left";
  constexpr char kRotated90ClockwiseBasename[] = "rectangles_clockwise_90";
  constexpr char kRotated180ClockwiseBasename[] = "rectangles_clockwise_180";
  constexpr char kRotated270ClockwiseBasename[] = "rectangles_clockwise_270";
  constexpr char kMirrorHoriBasename[] = "rectangles_mirrored_horizontal";
  constexpr char kMirrorVertBasename[] = "rectangles_mirrored_vertical";
  constexpr char kLargerTopLeftQuarterBasename[] = "rectangles_large_top_left";
  constexpr char kLargerRotatedDiagonalBasename[] =
      "rectangles_large_rotated_diagonal";
  constexpr char kTileBasename[] = "rectangles_tile";
  constexpr char kHoriStretchedBasename[] = "rectangles_stretched_horizontal";
  constexpr char kLargerBasename[] = "rectangles_larger";
  constexpr char kLargerClippedBasename[] = "rectangles_clipped_larger";
  constexpr char kLargerRotatedBasename[] = "rectangles_rotated_larger";
  constexpr char kLargerRotatedLandscapeBasename[] =
      "rectangles_landscape_rotated_larger";

  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  const float page_width = FPDF_GetPageWidthF(page.get());
  const float page_height = FPDF_GetPageHeightF(page.get());
  EXPECT_FLOAT_EQ(200, page_width);
  EXPECT_FLOAT_EQ(300, page_height);

  ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
  CompareBitmapWithExpectationSuffix(bitmap.get(), pdfium::kRectanglesPng);

  FS_RECTF page_rect{0, 0, page_width, page_height};

  // Try rendering with an identity matrix. The output should be the same as
  // the RenderLoadedPage() output.
  FS_MATRIX identity_matrix{1, 0, 0, 1, 0, 0};
  TestRenderPageBitmapWithMatrix(page.get(), page_width, page_height,
                                 identity_matrix, page_rect,
                                 pdfium::kRectanglesPng);

  // Again render with an identity matrix but with a smaller clipping rect.
  FS_RECTF middle_of_page_rect{page_width / 4, page_height / 4,
                               page_width * 3 / 4, page_height * 3 / 4};
  TestRenderPageBitmapWithMatrix(page.get(), page_width, page_height,
                                 identity_matrix, middle_of_page_rect,
                                 kClippedRectanglesBasename);

  // Now render again with the image scaled smaller.
  FS_MATRIX half_scale_matrix{0.5, 0, 0, 0.5, 0, 0};
  TestRenderPageBitmapWithMatrix(page.get(), page_width, page_height,
                                 half_scale_matrix, page_rect,
                                 kRectanglesTopLeftBasename);

  // Now render again with the image scaled larger horizontally (the right half
  // will be clipped).
  FS_MATRIX stretch_x_matrix{2, 0, 0, 1, 0, 0};
  TestRenderPageBitmapWithMatrix(page.get(), page_width, page_height,
                                 stretch_x_matrix, page_rect,
                                 kHoriStretchedBasename);

  // Try a 90 degree rotation clockwise but with the same bitmap size, so part
  // will be clipped.
  FS_MATRIX rotate_90_matrix{0, 1, -1, 0, page_width, 0};
  TestRenderPageBitmapWithMatrix(page.get(), page_width, page_height,
                                 rotate_90_matrix, page_rect,
                                 kRotated90ClockwiseBasename);

  // 180 degree rotation clockwise.
  FS_MATRIX rotate_180_matrix{-1, 0, 0, -1, page_width, page_height};
  TestRenderPageBitmapWithMatrix(page.get(), page_width, page_height,
                                 rotate_180_matrix, page_rect,
                                 kRotated180ClockwiseBasename);

  // 270 degree rotation clockwise.
  FS_MATRIX rotate_270_matrix{0, -1, 1, 0, 0, page_width};
  TestRenderPageBitmapWithMatrix(page.get(), page_width, page_height,
                                 rotate_270_matrix, page_rect,
                                 kRotated270ClockwiseBasename);

  // Mirror horizontally.
  FS_MATRIX mirror_hori_matrix{-1, 0, 0, 1, page_width, 0};
  TestRenderPageBitmapWithMatrix(page.get(), page_width, page_height,
                                 mirror_hori_matrix, page_rect,
                                 kMirrorHoriBasename);

  // Mirror vertically.
  FS_MATRIX mirror_vert_matrix{1, 0, 0, -1, 0, page_height};
  TestRenderPageBitmapWithMatrix(page.get(), page_width, page_height,
                                 mirror_vert_matrix, page_rect,
                                 kMirrorVertBasename);

  // Tests rendering to a larger bitmap
  const float bitmap_width = page_width * 2;
  const float bitmap_height = page_height * 2;

  // Render using an identity matrix and the whole bitmap area as clipping rect.
  FS_RECTF bitmap_rect{0, 0, bitmap_width, bitmap_height};
  TestRenderPageBitmapWithMatrix(page.get(), bitmap_width, bitmap_height,
                                 identity_matrix, bitmap_rect,
                                 kLargerTopLeftQuarterBasename);

  // Render using a scaling matrix to fill the larger bitmap.
  FS_MATRIX double_scale_matrix{2, 0, 0, 2, 0, 0};
  TestRenderPageBitmapWithMatrix(page.get(), bitmap_width, bitmap_height,
                                 double_scale_matrix, bitmap_rect,
                                 kLargerBasename);

  // Render the larger image again but with clipping.
  FS_RECTF middle_of_bitmap_rect{bitmap_width / 4, bitmap_height / 4,
                                 bitmap_width * 3 / 4, bitmap_height * 3 / 4};
  TestRenderPageBitmapWithMatrix(page.get(), bitmap_width, bitmap_height,
                                 double_scale_matrix, middle_of_bitmap_rect,
                                 kLargerClippedBasename);

  // On the larger bitmap, try a 90 degree rotation but with the same bitmap
  // size, so part will be clipped.
  FS_MATRIX rotate_90_scale_2_matrix{0, 2, -2, 0, bitmap_width, 0};
  TestRenderPageBitmapWithMatrix(page.get(), bitmap_width, bitmap_height,
                                 rotate_90_scale_2_matrix, bitmap_rect,
                                 kLargerRotatedBasename);

  // On the larger bitmap, apply 90 degree rotation to a bitmap with the
  // appropriate dimensions.
  const float landscape_bitmap_width = bitmap_height;
  const float landscape_bitmap_height = bitmap_width;
  FS_RECTF landscape_bitmap_rect{0, 0, landscape_bitmap_width,
                                 landscape_bitmap_height};
  FS_MATRIX landscape_rotate_90_scale_2_matrix{
      0, 2, -2, 0, landscape_bitmap_width, 0};
  TestRenderPageBitmapWithMatrix(
      page.get(), landscape_bitmap_width, landscape_bitmap_height,
      landscape_rotate_90_scale_2_matrix, landscape_bitmap_rect,
      kLargerRotatedLandscapeBasename);

  // On the larger bitmap, apply 45 degree rotation to a bitmap with the
  // appropriate dimensions.
  const float sqrt2 = 1.41421356f;
  const float diagonal_bitmap_size =
      ceil((bitmap_width + bitmap_height) / sqrt2);
  FS_RECTF diagonal_bitmap_rect{0, 0, diagonal_bitmap_size,
                                diagonal_bitmap_size};
  FS_MATRIX rotate_45_scale_2_matrix{
      sqrt2, sqrt2, -sqrt2, sqrt2, bitmap_height / sqrt2, 0};
  TestRenderPageBitmapWithMatrix(page.get(), diagonal_bitmap_size,
                                 diagonal_bitmap_size, rotate_45_scale_2_matrix,
                                 diagonal_bitmap_rect,
                                 kLargerRotatedDiagonalBasename);

  // Render the (2, 1) tile of the page (third column, second row) when the page
  // is divided in 50x50 pixel tiles. The tile is scaled by a factor of 7.
  const float scale = 7.0;
  const int tile_size = 50;
  const int tile_x = 2;
  const int tile_y = 1;
  float tile_bitmap_size = scale * tile_size;
  FS_RECTF tile_bitmap_rect{0, 0, tile_bitmap_size, tile_bitmap_size};
  FS_MATRIX tile_2_1_matrix{scale,
                            0,
                            0,
                            scale,
                            -tile_x * tile_bitmap_size,
                            -tile_y * tile_bitmap_size};
  TestRenderPageBitmapWithMatrix(page.get(), tile_bitmap_size, tile_bitmap_size,
                                 tile_2_1_matrix, tile_bitmap_rect,
                                 kTileBasename);
}

TEST_F(FPDFViewEmbedderTest, FPDFGetPageSizeByIndexF) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  FS_SIZEF size;
  EXPECT_FALSE(FPDF_GetPageSizeByIndexF(nullptr, 0, &size));
  EXPECT_FALSE(FPDF_GetPageSizeByIndexF(document(), 0, nullptr));

  // Page -1 doesn't exist.
  EXPECT_FALSE(FPDF_GetPageSizeByIndexF(document(), -1, &size));

  // Page 1 doesn't exist.
  EXPECT_FALSE(FPDF_GetPageSizeByIndexF(document(), 1, &size));

  // Page 0 exists.
  EXPECT_TRUE(FPDF_GetPageSizeByIndexF(document(), 0, &size));
  EXPECT_FLOAT_EQ(200.0f, size.width);
  EXPECT_FLOAT_EQ(300.0f, size.height);

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
#ifdef PDF_ENABLE_XFA
  // TODO(tsepez): XFA must obtain this size without parsing.
  EXPECT_EQ(1u, doc->GetParsedPageCountForTesting());
#else   // PDF_ENABLE_XFA
  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());
#endif  // PDF_ENABLE_XFA

  // Double-check against values from when page is actually parsed.
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_FLOAT_EQ(size.width, FPDF_GetPageWidthF(page.get()));
  EXPECT_FLOAT_EQ(size.height, FPDF_GetPageHeightF(page.get()));
  EXPECT_EQ(1u, doc->GetParsedPageCountForTesting());
}

TEST_F(FPDFViewEmbedderTest, EPDFDocGetPageObjectNumberByIndex) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  EXPECT_EQ(0u, EPDFDoc_GetPageObjectNumberByIndex(nullptr, 0));

  // Page -1 doesn't exist.
  EXPECT_EQ(0u, EPDFDoc_GetPageObjectNumberByIndex(document(), -1));

  // Page 1 doesn't exist.
  EXPECT_EQ(0u, EPDFDoc_GetPageObjectNumberByIndex(document(), 1));

  const unsigned int objnum = EPDFDoc_GetPageObjectNumberByIndex(document(), 0);
  EXPECT_NE(0u, objnum);

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());

  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(objnum, EPDFPage_GetObjectNumber(page.get()));
  EXPECT_EQ(1u, doc->GetParsedPageCountForTesting());
}

TEST_F(FPDFViewEmbedderTest, EPDFDocSetPageRotationByObjectNumber) {
  constexpr char kRotatedPng[] = "rectangles_rotated";

  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  const unsigned int objnum = EPDFDoc_GetPageObjectNumberByIndex(document(), 0);
  ASSERT_NE(0u, objnum);

  EXPECT_FALSE(EPDFDoc_SetPageRotationByObjectNumber(nullptr, objnum, 1));
  EXPECT_FALSE(EPDFDoc_SetPageRotationByObjectNumber(document(), 0, 1));
  EXPECT_FALSE(EPDFDoc_SetPageRotationByObjectNumber(document(), objnum, -1));
  EXPECT_FALSE(EPDFDoc_SetPageRotationByObjectNumber(document(), objnum, 4));

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(doc);
  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());

  EXPECT_TRUE(EPDFDoc_SetPageRotationByObjectNumber(document(), objnum, 1));
  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());

  {
    ScopedPage page = LoadScopedPage(0);
    ASSERT_TRUE(page);
    EXPECT_EQ(1, FPDFPage_GetRotation(page.get()));
    EXPECT_EQ(300, static_cast<int>(FPDF_GetPageWidth(page.get())));
    EXPECT_EQ(200, static_cast<int>(FPDF_GetPageHeight(page.get())));
    ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
    CompareBitmapWithExpectationSuffix(bitmap.get(), kRotatedPng);
  }

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ScopedSavedDoc saved_document = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_document);
  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);
  EXPECT_EQ(1, FPDFPage_GetRotation(saved_page.get()));
}

TEST_F(FPDFViewEmbedderTest, EPDFDocDeletePageByObjectNumber) {
  ASSERT_TRUE(OpenDocument("rectangles_multi_pages.pdf"));
  ASSERT_EQ(5, FPDF_GetPageCount(document()));

  const unsigned int original_page_1 =
      EPDFDoc_GetPageObjectNumberByIndex(document(), 1);
  ASSERT_NE(0u, original_page_1);

  int page_to_move = 1;
  ASSERT_TRUE(FPDF_MovePages(document(), &page_to_move, 1, 4));
  ASSERT_EQ(5, FPDF_GetPageCount(document()));
  EXPECT_EQ(original_page_1, EPDFDoc_GetPageObjectNumberByIndex(document(), 4));

  EXPECT_FALSE(EPDFDoc_DeletePageByObjectNumber(nullptr, original_page_1));
  EXPECT_FALSE(EPDFDoc_DeletePageByObjectNumber(document(), 0));
  EXPECT_TRUE(EPDFDoc_DeletePageByObjectNumber(document(), original_page_1));
  EXPECT_EQ(4, FPDF_GetPageCount(document()));

  for (int i = 0; i < FPDF_GetPageCount(document()); ++i) {
    EXPECT_NE(original_page_1,
              EPDFDoc_GetPageObjectNumberByIndex(document(), i));
  }

  EXPECT_FALSE(EPDFDoc_DeletePageByObjectNumber(document(), original_page_1));
}

TEST_F(FPDFViewEmbedderTest,
       EPDFDocDeletePageByObjectNumberDeletesDuplicatePageObjectOccurrence) {
  // This malformed compatibility fixture references the same /Page object from
  // multiple visible page positions. Delete-by-object-number removes the first
  // visible occurrence resolved by PDFium.
  ASSERT_TRUE(OpenDocument("bug_1229106.pdf"));
  ASSERT_EQ(4, FPDF_GetPageCount(document()));

  const unsigned int duplicate_objnum =
      EPDFDoc_GetPageObjectNumberByIndex(document(), 0);
  ASSERT_NE(0u, duplicate_objnum);
  ASSERT_EQ(duplicate_objnum,
            EPDFDoc_GetPageObjectNumberByIndex(document(), 1));

  EXPECT_TRUE(EPDFDoc_DeletePageByObjectNumber(document(), duplicate_objnum));
  EXPECT_EQ(3, FPDF_GetPageCount(document()));
  EXPECT_EQ(duplicate_objnum,
            EPDFDoc_GetPageObjectNumberByIndex(document(), 0));

  EXPECT_TRUE(EPDFDoc_DeletePageByObjectNumber(document(), duplicate_objnum));
  EXPECT_EQ(2, FPDF_GetPageCount(document()));

  for (int i = 0; i < FPDF_GetPageCount(document()); ++i) {
    EXPECT_NE(duplicate_objnum,
              EPDFDoc_GetPageObjectNumberByIndex(document(), i));
  }
}

TEST_F(FPDFViewEmbedderTest, EPDFGetPageBoxByIndex) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  auto expect_rect = [](const FS_RECTF& rect, float left, float top,
                        float right, float bottom) {
    EXPECT_FLOAT_EQ(left, rect.left);
    EXPECT_FLOAT_EQ(top, rect.top);
    EXPECT_FLOAT_EQ(right, rect.right);
    EXPECT_FLOAT_EQ(bottom, rect.bottom);
  };

  FS_RECTF box = {-1.0f, -1.0f, -1.0f, -1.0f};
  EXPECT_FALSE(EPDF_GetPageBoxByIndex(nullptr, 0, EPDF_PAGE_BOX_MEDIA, &box));
  EXPECT_FALSE(
      EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_MEDIA, nullptr));
  EXPECT_FALSE(
      EPDF_GetPageBoxByIndex(document(), -1, EPDF_PAGE_BOX_MEDIA, &box));
  EXPECT_FALSE(
      EPDF_GetPageBoxByIndex(document(), 1, EPDF_PAGE_BOX_MEDIA, &box));
  EXPECT_FALSE(EPDF_GetPageBoxByIndex(
      document(), 0, static_cast<EPDF_PAGE_BOX_TYPE>(999), &box));

  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_MEDIA, &box));
  expect_rect(box, 0.0f, 300.0f, 200.0f, 0.0f);

  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_CROP, &box));
  expect_rect(box, 0.0f, 300.0f, 200.0f, 0.0f);

  box = {-1.0f, -1.0f, -1.0f, -1.0f};
  EXPECT_FALSE(
      EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_BLEED, &box));
  expect_rect(box, -1.0f, -1.0f, -1.0f, -1.0f);

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  RetainPtr<CPDF_Dictionary> page_dict = doc->GetMutablePageDictionary(0);
  ASSERT_TRUE(page_dict);
  page_dict->SetRectFor(pdfium::page_object::kCropBox,
                        CFX_FloatRect(10.0f, 20.0f, 110.0f, 120.0f));
  page_dict->SetRectFor(pdfium::page_object::kBleedBox,
                        CFX_FloatRect(20.0f, 30.0f, 100.0f, 110.0f));
  page_dict->SetRectFor(pdfium::page_object::kTrimBox,
                        CFX_FloatRect(30.0f, 40.0f, 90.0f, 100.0f));
  page_dict->SetRectFor(pdfium::page_object::kArtBox,
                        CFX_FloatRect(40.0f, 50.0f, 80.0f, 90.0f));

  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_CROP, &box));
  expect_rect(box, 10.0f, 120.0f, 110.0f, 20.0f);
  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_BLEED, &box));
  expect_rect(box, 20.0f, 110.0f, 100.0f, 30.0f);
  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_TRIM, &box));
  expect_rect(box, 30.0f, 100.0f, 90.0f, 40.0f);
  ASSERT_TRUE(EPDF_GetPageBoxByIndex(document(), 0, EPDF_PAGE_BOX_ART, &box));
  expect_rect(box, 40.0f, 90.0f, 80.0f, 50.0f);

  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());
}

TEST_F(FPDFViewEmbedderTest, EPDFGetPageUserUnitByIndex) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  float user_unit = 0.0f;
  EXPECT_FALSE(EPDF_GetPageUserUnitByIndex(nullptr, 0, &user_unit));
  EXPECT_FALSE(EPDF_GetPageUserUnitByIndex(document(), 0, nullptr));
  EXPECT_FALSE(EPDF_GetPageUserUnitByIndex(document(), -1, &user_unit));
  EXPECT_FALSE(EPDF_GetPageUserUnitByIndex(document(), 1, &user_unit));

  ASSERT_TRUE(EPDF_GetPageUserUnitByIndex(document(), 0, &user_unit));
  EXPECT_FLOAT_EQ(1.0f, user_unit);

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  RetainPtr<CPDF_Dictionary> page_dict = doc->GetMutablePageDictionary(0);
  ASSERT_TRUE(page_dict);
  page_dict->SetNewFor<CPDF_Number>("UserUnit", 2.5f);

  ASSERT_TRUE(EPDF_GetPageUserUnitByIndex(document(), 0, &user_unit));
  EXPECT_FLOAT_EQ(2.5f, user_unit);

  page_dict->SetNewFor<CPDF_Number>("UserUnit", -10.0f);
  ASSERT_TRUE(EPDF_GetPageUserUnitByIndex(document(), 0, &user_unit));
  EXPECT_FLOAT_EQ(1.0f, user_unit);

  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());
}

TEST_F(FPDFViewEmbedderTest, FPDFGetPageSizeByIndex) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  double width = 0;
  double height = 0;

  EXPECT_FALSE(FPDF_GetPageSizeByIndex(nullptr, 0, &width, &height));
  EXPECT_FALSE(FPDF_GetPageSizeByIndex(document(), 0, nullptr, &height));
  EXPECT_FALSE(FPDF_GetPageSizeByIndex(document(), 0, &width, nullptr));

  // Page -1 doesn't exist.
  EXPECT_FALSE(FPDF_GetPageSizeByIndex(document(), -1, &width, &height));

  // Page 1 doesn't exist.
  EXPECT_FALSE(FPDF_GetPageSizeByIndex(document(), 1, &width, &height));

  // Page 0 exists.
  EXPECT_TRUE(FPDF_GetPageSizeByIndex(document(), 0, &width, &height));
  EXPECT_EQ(200.0, width);
  EXPECT_EQ(300.0, height);

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
#ifdef PDF_ENABLE_XFA
  // TODO(tsepez): XFA must obtain this size without parsing.
  EXPECT_EQ(1u, doc->GetParsedPageCountForTesting());
#else   // PDF_ENABLE_XFA
  EXPECT_EQ(0u, doc->GetParsedPageCountForTesting());
#endif  // PDF_ENABLE_XFA

  // Double-check against values from when page is actually parsed.
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(width, FPDF_GetPageWidth(page.get()));
  EXPECT_EQ(height, FPDF_GetPageHeight(page.get()));
  EXPECT_EQ(1u, doc->GetParsedPageCountForTesting());
}

TEST_F(FPDFViewEmbedderTest, CroppedTextPageSize) {
  ASSERT_TRUE(OpenDocument("cropped_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_FLOAT_EQ(100.0f, FPDF_GetPageWidthF(page.get()));
  EXPECT_FLOAT_EQ(100.0f, FPDF_GetPageHeightF(page.get()));

  double width = 1.0;
  double height = 1.0;
  ASSERT_TRUE(FPDF_GetPageSizeByIndex(document(), 0, &width, &height));
  EXPECT_DOUBLE_EQ(100.0, width);
  EXPECT_DOUBLE_EQ(100.0, height);
}

TEST_F(FPDFViewEmbedderTest, CroppedNoOverlapPageSize) {
  ASSERT_TRUE(OpenDocument("cropped_no_overlap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_FLOAT_EQ(0.0f, FPDF_GetPageWidthF(page.get()));
  EXPECT_FLOAT_EQ(0.0f, FPDF_GetPageHeightF(page.get()));

  double width = 1.0;
  double height = 1.0;
  ASSERT_TRUE(FPDF_GetPageSizeByIndex(document(), 0, &width, &height));
  EXPECT_DOUBLE_EQ(0.0, width);
  EXPECT_DOUBLE_EQ(0.0, height);
}

TEST_F(FPDFViewEmbedderTest, GetXFAArrayData) {
  static constexpr struct {
    int index;
    const char* name;
    size_t content_length;
    const char* content_checksum;
  } kTestCases[]{
      {0, "preamble", 124u, "71be364e53292596412242bfcdb46eab"},
      {1, "config", 642u, "bcd1ca1d420ee31a561273a54a06435f"},
      {2, "template", 541u, "0f48cb2fa1bb9cbf9eee802d66e81bf4"},
      {3, "localeSet", 3455u, "bb1f253d3e5c719ac0da87d055bc164e"},
      {4, "postamble", 11u, "6b79e25da35d86634ea27c38f64cf243"},
  };

  ASSERT_TRUE(OpenDocument("simple_xfa.pdf"));
  ASSERT_EQ(static_cast<int>(std::size(kTestCases)),
            FPDF_GetXFAPacketCount(document()));

  for (const auto& testcase : kTestCases) {
    char name_buffer[20] = {};
    ASSERT_EQ(strlen(testcase.name) + 1,
              FPDF_GetXFAPacketName(document(), testcase.index, nullptr, 0));
    EXPECT_EQ(strlen(testcase.name) + 1,
              FPDF_GetXFAPacketName(document(), testcase.index, name_buffer,
                                    sizeof(name_buffer)));
    EXPECT_STREQ(testcase.name, name_buffer);

    unsigned long buflen;
    ASSERT_TRUE(FPDF_GetXFAPacketContent(document(), testcase.index, nullptr, 0,
                                         &buflen));
    ASSERT_EQ(testcase.content_length, buflen);
    std::vector<uint8_t> data_buffer(buflen);
    EXPECT_TRUE(FPDF_GetXFAPacketContent(document(), testcase.index,
                                         data_buffer.data(), data_buffer.size(),
                                         &buflen));
    EXPECT_EQ(testcase.content_length, buflen);
    EXPECT_EQ(testcase.content_checksum, GenerateMD5Base16(data_buffer));
  }

  // Test bad parameters.
  EXPECT_EQ(-1, FPDF_GetXFAPacketCount(nullptr));

  EXPECT_EQ(0u, FPDF_GetXFAPacketName(nullptr, 0, nullptr, 0));
  EXPECT_EQ(0u, FPDF_GetXFAPacketName(document(), -1, nullptr, 0));
  EXPECT_EQ(
      0u, FPDF_GetXFAPacketName(document(), std::size(kTestCases), nullptr, 0));

  unsigned long buflen = 123;
  EXPECT_FALSE(FPDF_GetXFAPacketContent(nullptr, 0, nullptr, 0, &buflen));
  EXPECT_EQ(123u, buflen);
  EXPECT_FALSE(FPDF_GetXFAPacketContent(document(), -1, nullptr, 0, &buflen));
  EXPECT_EQ(123u, buflen);
  EXPECT_FALSE(FPDF_GetXFAPacketContent(document(), std::size(kTestCases),
                                        nullptr, 0, &buflen));
  EXPECT_EQ(123u, buflen);
  EXPECT_FALSE(FPDF_GetXFAPacketContent(document(), 0, nullptr, 0, nullptr));
}

TEST_F(FPDFViewEmbedderTest, GetXFAStreamData) {
  ASSERT_TRUE(OpenDocument("bug_1265.pdf"));

  ASSERT_EQ(1, FPDF_GetXFAPacketCount(document()));

  char name_buffer[20] = {};
  ASSERT_EQ(1u, FPDF_GetXFAPacketName(document(), 0, nullptr, 0));
  EXPECT_EQ(1u, FPDF_GetXFAPacketName(document(), 0, name_buffer,
                                      sizeof(name_buffer)));
  EXPECT_STREQ("", name_buffer);

  unsigned long buflen;
  ASSERT_TRUE(FPDF_GetXFAPacketContent(document(), 0, nullptr, 0, &buflen));
  ASSERT_EQ(121u, buflen);
  std::vector<uint8_t> data_buffer(buflen);
  EXPECT_TRUE(FPDF_GetXFAPacketContent(document(), 0, data_buffer.data(),
                                       data_buffer.size(), &buflen));
  EXPECT_EQ(121u, buflen);
  EXPECT_EQ("8f912eaa1e66c9341cb3032ede71e147", GenerateMD5Base16(data_buffer));
}

TEST_F(FPDFViewEmbedderTest, GetXFADataForNoForm) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));

  EXPECT_EQ(0, FPDF_GetXFAPacketCount(document()));
}

TEST_F(FPDFViewEmbedderTest, GetXFADataForAcroForm) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));

  EXPECT_EQ(0, FPDF_GetXFAPacketCount(document()));
}

class RecordUnsupportedErrorDelegate final : public EmbedderTest::Delegate {
 public:
  RecordUnsupportedErrorDelegate() = default;
  ~RecordUnsupportedErrorDelegate() override = default;

  void UnsupportedHandler(int type) override { type_ = type; }

  int type_ = -1;
};

TEST_F(FPDFViewEmbedderTest, UnSupportedOperationsNotFound) {
  RecordUnsupportedErrorDelegate delegate;
  SetDelegate(&delegate);
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_EQ(delegate.type_, -1);
  SetDelegate(nullptr);
}

TEST_F(FPDFViewEmbedderTest, UnSupportedOperationsLoadCustomDocument) {
  RecordUnsupportedErrorDelegate delegate;
  SetDelegate(&delegate);
  ASSERT_TRUE(OpenDocument("unsupported_feature.pdf"));
  EXPECT_EQ(FPDF_UNSP_DOC_PORTABLECOLLECTION, delegate.type_);
  SetDelegate(nullptr);
}

TEST_F(FPDFViewEmbedderTest, UnSupportedOperationsLoadDocument) {
  std::string file_path =
      PathService::GetTestFilePath("unsupported_feature.pdf");
  ASSERT_FALSE(file_path.empty());

  RecordUnsupportedErrorDelegate delegate;
  SetDelegate(&delegate);
  {
    ScopedFPDFDocument doc(FPDF_LoadDocument(file_path.c_str(), ""));
    EXPECT_TRUE(doc);
    EXPECT_EQ(FPDF_UNSP_DOC_PORTABLECOLLECTION, delegate.type_);
  }
  SetDelegate(nullptr);
}

TEST_F(FPDFViewEmbedderTest, DocumentHasValidCrossReferenceTable) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_TRUE(FPDF_DocumentHasValidCrossReferenceTable(document()));
}

TEST_F(FPDFViewEmbedderTest, DocumentHasInvalidCrossReferenceTable) {
  EXPECT_FALSE(FPDF_DocumentHasValidCrossReferenceTable(nullptr));

  ASSERT_TRUE(OpenDocument("bug_664284.pdf"));
  EXPECT_FALSE(FPDF_DocumentHasValidCrossReferenceTable(document()));
}

// Related to https://crbug.com/42270189
TEST_F(FPDFViewEmbedderTest, LoadDocumentWithEmptyXRefConsistently) {
  ASSERT_TRUE(OpenDocument("empty_xref.pdf"));
  EXPECT_TRUE(FPDF_DocumentHasValidCrossReferenceTable(document()));

  std::string file_path = PathService::GetTestFilePath("empty_xref.pdf");
  ASSERT_FALSE(file_path.empty());
  {
    ScopedFPDFDocument doc(FPDF_LoadDocument(file_path.c_str(), ""));
    ASSERT_TRUE(doc);
    EXPECT_TRUE(FPDF_DocumentHasValidCrossReferenceTable(doc.get()));
  }
  {
    std::vector<uint8_t> file_contents = GetFileContents(file_path.c_str());
    ASSERT_FALSE(file_contents.empty());
    ScopedFPDFDocument doc(
        FPDF_LoadMemDocument(file_contents.data(), file_contents.size(), ""));
    ASSERT_TRUE(doc);
    EXPECT_TRUE(FPDF_DocumentHasValidCrossReferenceTable(doc.get()));
  }
}

TEST_F(FPDFViewEmbedderTest, RenderBug664284WithNoNativeText) {
  // For Skia, since the font used in bug_664284.pdf is not a CID font,
  // ShouldDrawDeviceText() will always return true. Therefore
  // FPDF_NO_NATIVETEXT and the font widths defined in the PDF determines
  // whether to go through the rendering path in
  // CFX_SkiaDeviceDriver::DrawDeviceText(). In this case, it returns false and
  // affects the rendering results across all platforms.

  // For AGG, since CFX_AggDeviceDriver::DrawDeviceText() is only implemented
  // for macOS, FPDF_NO_NATIVETEXT will not affect the device-specific rendering
  // path on other platforms and it will only disable native text support on
  // macOS. Therefore Windows and Linux rendering results remain the same as
  // rendering with no flags, while the macOS rendering result does not.
  static constexpr char kOriginalBasename[] = "bug_664284";
  static constexpr char kNoNativeTextFilename[] = "bug_664284_no_native_text";
  ASSERT_TRUE(OpenDocument("bug_664284.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  TestRenderPageBitmapWithFlags(page.get(), 0, kOriginalBasename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_NO_NATIVETEXT,
                                kNoNativeTextFilename);
}

TEST_F(FPDFViewEmbedderTest, RenderAnnotationWithPrintingFlag) {
  static constexpr char kAnnotationBasename[] = "bug_1658_annot";
  static constexpr char kPrintingFilename[] = "bug_1658_print";
  ASSERT_TRUE(OpenDocument("bug_1658.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // A yellow highlight is rendered with `FPDF_ANNOT` flag.
  TestRenderPageBitmapWithFlags(page.get(), FPDF_ANNOT, kAnnotationBasename);

  // After adding `FPDF_PRINTING` flag, the yellow highlight is not rendered.
  TestRenderPageBitmapWithFlags(page.get(), FPDF_PRINTING | FPDF_ANNOT,
                                kPrintingFilename);
}

// TODO(crbug.com/41480203): Remove this test once pixel tests can pass with
// `reverse-byte-order` option.
TEST_F(FPDFViewEmbedderTest, RenderBlueAndRedImagesWithReverByteOrderFlag) {
  // When rendering with `FPDF_REVERSE_BYTE_ORDER` flag, the blue and red
  // channels should be reversed.
  ASSERT_TRUE(OpenDocument("bug_1396264.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(document(), 0));
  ASSERT_TRUE(page);

  TestRenderPageBitmapWithFlags(page.get(), 0, "bug_1396264");
  TestRenderPageBitmapWithFlags(page.get(), FPDF_REVERSE_BYTE_ORDER,
                                "bug_1396264_reverse_byte");
}

TEST_F(FPDFViewEmbedderTest, RenderJpxLzwImageWithFlags) {
  static const char kNormalFilename[] = "jpx_lzw";
  static const char kGrayscaleFilename[] = "jpx_lzw_grayscale";

  ASSERT_TRUE(OpenDocument("jpx_lzw.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  TestRenderPageBitmapWithFlags(page.get(), 0, kNormalFilename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_ANNOT, kNormalFilename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_LCD_TEXT, kNormalFilename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_NO_NATIVETEXT,
                                kNormalFilename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_GRAYSCALE, kGrayscaleFilename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_LIMITEDIMAGECACHE,
                                kNormalFilename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_FORCEHALFTONE,
                                kNormalFilename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_PRINTING, kNormalFilename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_NO_SMOOTHTEXT,
                                kNormalFilename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_NO_SMOOTHIMAGE,
                                kNormalFilename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_NO_SMOOTHPATH,
                                kNormalFilename);
}

TEST_F(FPDFViewEmbedderTest, RenderManyRectanglesWithFlags) {
  constexpr char kGrayscaleFilename[] = "many_rectangles_grayscale";
  constexpr char kNoSmoothpathFilename[] = "many_rectangles_no_smoothpath";

  ASSERT_TRUE(OpenDocument("many_rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  TestRenderPageBitmapWithFlags(page.get(), 0, pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_ANNOT,
                                pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_LCD_TEXT,
                                pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_NO_NATIVETEXT,
                                pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_GRAYSCALE, kGrayscaleFilename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_LIMITEDIMAGECACHE,
                                pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_FORCEHALFTONE,
                                pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_PRINTING,
                                pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_NO_SMOOTHTEXT,
                                pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_NO_SMOOTHIMAGE,
                                pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_NO_SMOOTHPATH,
                                kNoSmoothpathFilename);
}

TEST_F(FPDFViewEmbedderTest, RenderManyRectanglesWithAndWithoutExternalMemory) {
  ASSERT_TRUE(OpenDocument("many_rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  constexpr char kBGRBasename[] = "many_rectangles_bgr";
  static constexpr int kBgrStride = 600;  // Width of 200 * 24 bits per pixel.
  TestRenderPageBitmapWithInternalMemory(page.get(), FPDFBitmap_BGR,
                                         kBGRBasename);
  TestRenderPageBitmapWithInternalMemoryAndStride(page.get(), FPDFBitmap_BGR,
                                                  kBgrStride, kBGRBasename);
  TestRenderPageBitmapWithExternalMemory(page.get(), FPDFBitmap_BGR,
                                         kBGRBasename);
  TestRenderPageBitmapWithExternalMemoryAndNoStride(page.get(), FPDFBitmap_BGR,
                                                    kBGRBasename);

  const char* kGrayBasename = "many_rectangles_grayscale_memory";

  TestRenderPageBitmapWithInternalMemory(page.get(), FPDFBitmap_Gray,
                                         kGrayBasename, /*fuzzy=*/true);
  static constexpr int kGrayStride = 200;  // Width of 200 * 8 bits per pixel.
  TestRenderPageBitmapWithInternalMemoryAndStride(
      page.get(), FPDFBitmap_Gray, kGrayStride, kGrayBasename, /*fuzzy=*/true);
  TestRenderPageBitmapWithExternalMemory(page.get(), FPDFBitmap_Gray,
                                         kGrayBasename, /*fuzzy=*/true);
  TestRenderPageBitmapWithExternalMemoryAndNoStride(
      page.get(), FPDFBitmap_Gray, kGrayBasename, /*fuzzy=*/true);

  static constexpr int kBgrxStride = 800;  // Width of 200 * 32 bits per pixel.
  TestRenderPageBitmapWithInternalMemory(page.get(), FPDFBitmap_BGRx,
                                         pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithInternalMemoryAndStride(
      page.get(), FPDFBitmap_BGRx, kBgrxStride, pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithExternalMemory(page.get(), FPDFBitmap_BGRx,
                                         pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithExternalMemoryAndNoStride(page.get(), FPDFBitmap_BGRx,
                                                    pdfium::kManyRectanglesPng);

  TestRenderPageBitmapWithInternalMemory(page.get(), FPDFBitmap_BGRA,
                                         pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithInternalMemoryAndStride(
      page.get(), FPDFBitmap_BGRA, kBgrxStride, pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithExternalMemory(page.get(), FPDFBitmap_BGRA,
                                         pdfium::kManyRectanglesPng);
  TestRenderPageBitmapWithExternalMemoryAndNoStride(page.get(), FPDFBitmap_BGRA,
                                                    pdfium::kManyRectanglesPng);

#if defined(PDF_USE_SKIA)
  if (CFX_DefaultRenderDevice::UseSkiaRenderer()) {
    TestRenderPageBitmapWithInternalMemory(page.get(), FPDFBitmap_BGRA_Premul,
                                           pdfium::kManyRectanglesPng);
    TestRenderPageBitmapWithInternalMemoryAndStride(
        page.get(), FPDFBitmap_BGRA_Premul, kBgrxStride,
        pdfium::kManyRectanglesPng);
    TestRenderPageBitmapWithExternalMemory(page.get(), FPDFBitmap_BGRA_Premul,
                                           pdfium::kManyRectanglesPng);
    TestRenderPageBitmapWithExternalMemoryAndNoStride(
        page.get(), FPDFBitmap_BGRA_Premul, pdfium::kManyRectanglesPng);
  }
#endif
}

TEST_F(FPDFViewEmbedderTest, RenderHelloWorldWithFlags) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  using pdfium::kHelloWorldPng;
  TestRenderPageBitmapWithFlags(page.get(), 0, kHelloWorldPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_ANNOT, kHelloWorldPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_GRAYSCALE, kHelloWorldPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_LIMITEDIMAGECACHE,
                                kHelloWorldPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_FORCEHALFTONE,
                                kHelloWorldPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_PRINTING, kHelloWorldPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_NO_SMOOTHIMAGE,
                                kHelloWorldPng);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_NO_SMOOTHPATH,
                                kHelloWorldPng);

  constexpr char kLCDTextBasename[] = "hello_world_lcd";
  constexpr char kNoSmoothTextBasename[] = "hello_world_no_smoothtext";

  TestRenderPageBitmapWithFlags(page.get(), FPDF_LCD_TEXT, kLCDTextBasename);
  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_NO_SMOOTHTEXT,
                                kNoSmoothTextBasename);

  // For text rendering, When anti-aliasing is disabled, LCD Optimization flag
  // will be ignored.
  TestRenderPageBitmapWithFlags(page.get(),
                                FPDF_LCD_TEXT | FPDF_RENDER_NO_SMOOTHTEXT,
                                kNoSmoothTextBasename);
}

// Deliberately disabled because this test case renders a large bitmap, which is
// very slow for debug builds.
#if defined(NDEBUG)
#define MAYBE_LargeImageDoesNotRenderBlank LargeImageDoesNotRenderBlank
#else
#define MAYBE_LargeImageDoesNotRenderBlank DISABLED_LargeImageDoesNotRenderBlank
#endif
TEST_F(FPDFViewEmbedderTest, LargeImageDoesNotRenderBlank) {
  static constexpr char kFilename[] = "bug_1646";

  ASSERT_TRUE(OpenDocument("bug_1646.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  static constexpr int kWidth = 40000;
  static constexpr int kHeight = 100;
  TestRenderPageBitmapWithMatrix(page.get(), kWidth, kHeight,
                                 {1000, 0, 0, 1, 0, 0}, {0, 0, kWidth, kHeight},
                                 kFilename);
}

#if BUILDFLAG(IS_WIN)
TEST_F(FPDFViewEmbedderTest, FPDFRenderPageEmf) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  std::vector<uint8_t> emf_normal = RenderPageWithFlagsToEmf(page.get(), 0);
  EXPECT_EQ(3772u, emf_normal.size());

  // FPDF_REVERSE_BYTE_ORDER is ignored since EMFs are always BGR.
  std::vector<uint8_t> emf_reverse_byte_order =
      RenderPageWithFlagsToEmf(page.get(), FPDF_REVERSE_BYTE_ORDER);
  EXPECT_EQ(emf_normal, emf_reverse_byte_order);
}

class PostScriptRenderEmbedderTestBase : public FPDFViewEmbedderTest {
 protected:
  ~PostScriptRenderEmbedderTestBase() override = default;

  // FPDFViewEmbedderTest:
  void TearDown() override {
    FPDF_SetPrintMode(FPDF_PRINTMODE_EMF);
    FPDFViewEmbedderTest::TearDown();
  }
};

class PostScriptLevel2EmbedderTest : public PostScriptRenderEmbedderTestBase {
 public:
  PostScriptLevel2EmbedderTest() = default;
  ~PostScriptLevel2EmbedderTest() override = default;

 protected:
  // FPDFViewEmbedderTest:
  void SetUp() override {
    FPDFViewEmbedderTest::SetUp();
    FPDF_SetPrintMode(FPDF_PRINTMODE_POSTSCRIPT2);
  }
};

class PostScriptLevel3EmbedderTest : public PostScriptRenderEmbedderTestBase {
 public:
  PostScriptLevel3EmbedderTest() = default;
  ~PostScriptLevel3EmbedderTest() override = default;

 protected:
  // FPDFViewEmbedderTest:
  void SetUp() override {
    FPDFViewEmbedderTest::SetUp();
    FPDF_SetPrintMode(FPDF_PRINTMODE_POSTSCRIPT3);
  }
};

TEST_F(PostScriptLevel2EmbedderTest, Rectangles) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  std::vector<uint8_t> emf_normal = RenderPageWithFlagsToEmf(page.get(), 0);
  std::string ps_data = GetPostScriptFromEmf(emf_normal);
  EXPECT_EQ(kExpectedRectanglePostScript, ps_data);

  // FPDF_REVERSE_BYTE_ORDER is ignored since PostScript is not bitmap-based.
  std::vector<uint8_t> emf_reverse_byte_order =
      RenderPageWithFlagsToEmf(page.get(), FPDF_REVERSE_BYTE_ORDER);
  EXPECT_EQ(emf_normal, emf_reverse_byte_order);
}

TEST_F(PostScriptLevel3EmbedderTest, Rectangles) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  std::vector<uint8_t> emf_normal = RenderPageWithFlagsToEmf(page.get(), 0);
  std::string ps_data = GetPostScriptFromEmf(emf_normal);
  EXPECT_EQ(kExpectedRectanglePostScript, ps_data);

  // FPDF_REVERSE_BYTE_ORDER is ignored since PostScript is not bitmap-based.
  std::vector<uint8_t> emf_reverse_byte_order =
      RenderPageWithFlagsToEmf(page.get(), FPDF_REVERSE_BYTE_ORDER);
  EXPECT_EQ(emf_normal, emf_reverse_byte_order);
}

TEST_F(PostScriptLevel2EmbedderTest, Image) {
  const char kExpected[] =
      "\n"
      "save\n"
      "/im/initmatrix load def\n"
      "/n/newpath load def/m/moveto load def/l/lineto load def/c/curveto load "
      "def/h/closepath load def\n"
      "/f/fill load def/F/eofill load def/s/stroke load def/W/clip load "
      "def/W*/eoclip load def\n"
      "/rg/setrgbcolor load def/k/setcmykcolor load def\n"
      "/J/setlinecap load def/j/setlinejoin load def/w/setlinewidth load "
      "def/M/setmiterlimit load def/d/setdash load def\n"
      "/q/gsave load def/Q/grestore load def/iM/imagemask load def\n"
      "/Tj/show load def/Ff/findfont load def/Fs/scalefont load def/Sf/setfont "
      "load def\n"
      "/cm/concat load def/Cm/currentmatrix load def/mx/matrix load "
      "def/sm/setmatrix load def\n"
      "0 792 m 0 0 l 612 0 l 612 792 l 0 792 l h W n\n"
      "q\n"
      "0 792 m 0 0 l 612 0 l 612 792 l 0 792 l h W n\n"
      "q\n"
      "Q\n"
      "q\n"
      "281 106.7 m 331 106.7 l 331 56.7 l 281 56.7 l 281 106.7 l h W* n\n"
      "q\n"
      "[49.9 0 0 -50 281.1 106.6]cm 50 50 8[50 0 0 -50 0 "
      "50]currentfile/ASCII85Decode filter /DCTDecode filter false 3 "
      "colorimage\n"
      "s4IA0!\"_al8O`[\\!<<*#!!*'\"s4[N@!!ic5#6k>;#6tJ?#m^kH'FbHY$Odmc'+Yct)"
      "BU\"@)B9_>\r\n"
      ",VCGe+tOrY*%3`p/2/e81c-:%3B]>W4>&EH1B6)/"
      "6NIK\"#n.1M(_$ok1*IV\\1,:U?1,:U?1,:U?\r\n"
      "1,:U?1,:U?1,:U?1,:U?1,:U?1,:U?1,:U?1,:U?1,:U?1,AmF!\"fJ:1&s'3!?qLF&HMtG!"
      "WU(<\r\n"
      "*rl9A\"T\\W)!<E3$z!!!!\"!WrQ/\"pYD?$4HmP!4<@<!W`B*!X&T/"
      "\"U\"r.!!.KK!WrE*&Hrdj0gQ!W\r\n"
      ";.0\\RE>10ZOeE%*6F\"?A;UOtZ1LbBV#mqFa(`=5<-7:2j.Ps\"@2`NfY6UX@47n?3D;"
      "cHat='/U/\r\n"
      "@q9._B4u!oF*)PJGBeCZK7nr5LPUeEP*;,qQC!u,R\\HRQV5C/"
      "hWN*81['d?O\\@K2f_o0O6a2lBF\r\n"
      "daQ^rf%8R-g>V&OjQ5OekiqC&o(2MHp@n@XqZ\"J6*ru?D!<E3%!<E3%!<<*\"!!!!\"!"
      "WrQ/\"pYD?\r\n"
      "$4HmP!4<C=!W`?*\"9Sc3\"U\"r.!<RHF!<N?8\"9fr'\"qj4!#@VTc+u4]T'LIqUZ,$_"
      "k1K*]W@WKj'\r\n"
      "(*k`q-1Mcg)&ahL-n-W'2E*TU3^Z;(7Rp!@8lJ\\h<``C+>%;)SAnPdkC3+K>G'A1VH@gd&"
      "KnbA=\r\n"
      "M2II[Pa.Q$R$jD;USO``Vl6SpZEppG[^WcW]#)A'`Q#s>ai`&\\eCE.%f\\,!<j5f="
      "akNM0qo(2MH\r\n"
      "p@n@XqZ#7L$j-M1!YGMH!'^JZre`+s!fAD!!fAD!!fAD!!fAD!!fAD!!fAD!!fAD!!fAD!!"
      "fAD!\r\n"
      "!fAD!!fAD!!fAD!!fAD!!fAD!!fAD!&-(;~>\n"
      "Q\n"
      "Q\n"
      "q\n"
      "q\n"
      "Q\n"
      "Q\n"
      "Q\n"
      "Q\n"
      "\n"
      "restore\n";

  ASSERT_TRUE(OpenDocument("tagged_alt_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  std::vector<uint8_t> emf = RenderPageWithFlagsToEmf(page.get(), 0);
  std::string ps_data = GetPostScriptFromEmf(emf);
  EXPECT_EQ(kExpected, ps_data);
}

TEST_F(PostScriptLevel3EmbedderTest, Image) {
  const char kExpected[] = R"(
save
/im/initmatrix load def
/n/newpath load def/m/moveto load def/l/lineto load def/c/curveto load def/h/closepath load def
/f/fill load def/F/eofill load def/s/stroke load def/W/clip load def/W*/eoclip load def
/rg/setrgbcolor load def/k/setcmykcolor load def
/J/setlinecap load def/j/setlinejoin load def/w/setlinewidth load def/M/setmiterlimit load def/d/setdash load def
/q/gsave load def/Q/grestore load def/iM/imagemask load def
/Tj/show load def/Ff/findfont load def/Fs/scalefont load def/Sf/setfont load def
/cm/concat load def/Cm/currentmatrix load def/mx/matrix load def/sm/setmatrix load def
0 792 m 0 0 l 612 0 l 612 792 l 0 792 l h W n
q
0 792 m 0 0 l 612 0 l 612 792 l 0 792 l h W n
q
Q
q
281 106.7 m 331 106.7 l 331 56.7 l 281 56.7 l 281 106.7 l h W* n
q
[49.9 0 0 -50 281.1 106.6]cm 50 50 8[50 0 0 -50 0 50]currentfile/ASCII85Decode filter /FlateDecode filter false 3 colorimage
Gb"0;0`_7S!5bE%:[N')TE"rlzGQSs[!!*~>
Q
Q
q
q
Q
Q
Q
Q

restore
)";

  ASSERT_TRUE(OpenDocument("tagged_alt_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  std::vector<uint8_t> emf = RenderPageWithFlagsToEmf(page.get(), 0);
  std::string ps_data = GetPostScriptFromEmf(emf);
  EXPECT_EQ(kExpected, ps_data);
}

TEST_F(FPDFViewEmbedderTest, ImageMask) {
  ASSERT_TRUE(OpenDocument("bug_674771.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // Render the page with more efficient processing of image masks.
  FPDF_SetPrintMode(FPDF_PRINTMODE_EMF_IMAGE_MASKS);
  std::vector<uint8_t> emf_image_masks =
      RenderPageWithFlagsToEmf(page.get(), 0);

  // Render the page normally.
  FPDF_SetPrintMode(FPDF_PRINTMODE_EMF);
  std::vector<uint8_t> emf_normal = RenderPageWithFlagsToEmf(page.get(), 0);

  EXPECT_LT(emf_image_masks.size(), emf_normal.size());
}
#endif  // BUILDFLAG(IS_WIN)

TEST_F(FPDFViewEmbedderTest, GetTrailerEnds) {
  ASSERT_TRUE(OpenDocument("two_signatures.pdf"));

  // FPDF_GetTrailerEnds() positive testing.
  unsigned long size = FPDF_GetTrailerEnds(document(), nullptr, 0);
  const std::vector<unsigned int> kExpectedEnds{633, 1703, 2781};
  ASSERT_EQ(kExpectedEnds.size(), size);
  std::vector<unsigned int> ends(size);
  ASSERT_EQ(size, FPDF_GetTrailerEnds(document(), ends.data(), size));
  ASSERT_EQ(kExpectedEnds, ends);

  // FPDF_GetTrailerEnds() negative testing.
  ASSERT_EQ(0U, FPDF_GetTrailerEnds(nullptr, nullptr, 0));

  ends.resize(2);
  ends[0] = 0;
  ends[1] = 1;
  size = FPDF_GetTrailerEnds(document(), ends.data(), ends.size());
  ASSERT_EQ(kExpectedEnds.size(), size);
  EXPECT_EQ(0U, ends[0]);
  EXPECT_EQ(1U, ends[1]);
}

TEST_F(FPDFViewEmbedderTest, GetTrailerEndsHelloWorld) {
  // Single trailer, \n line ending at the trailer end.
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));

  // FPDF_GetTrailerEnds() positive testing.
  unsigned long size = FPDF_GetTrailerEnds(document(), nullptr, 0);
  const std::vector<unsigned int> kExpectedEnds{840};
  ASSERT_EQ(kExpectedEnds.size(), size);
  std::vector<unsigned int> ends(size);
  ASSERT_EQ(size, FPDF_GetTrailerEnds(document(), ends.data(), size));
  ASSERT_EQ(kExpectedEnds, ends);
}

TEST_F(FPDFViewEmbedderTest, GetTrailerEndsAnnotationStamp) {
  // Multiple trailers, \r\n line ending at the trailer ends.
  ASSERT_TRUE(OpenDocument("annotation_stamp_with_ap.pdf"));

  // FPDF_GetTrailerEnds() positive testing.
  unsigned long size = FPDF_GetTrailerEnds(document(), nullptr, 0);
  const std::vector<unsigned int> kExpectedEnds{441, 7945, 101719};
  ASSERT_EQ(kExpectedEnds.size(), size);
  std::vector<unsigned int> ends(size);
  ASSERT_EQ(size, FPDF_GetTrailerEnds(document(), ends.data(), size));
  ASSERT_EQ(kExpectedEnds, ends);
}

TEST_F(FPDFViewEmbedderTest, GetTrailerEndsLinearized) {
  // Set up linearized PDF.
  FileAccessForTesting file_acc("linearized.pdf");
  FakeFileAccess fake_acc(&file_acc);
  CreateAvail(fake_acc.GetFileAvail(), fake_acc.GetFileAccess());
  fake_acc.SetWholeFileAvailable();

  // Multiple trailers, \r line ending at the trailer ends (no \n).
  SetDocumentFromAvail();
  ASSERT_TRUE(document());

  // FPDF_GetTrailerEnds() positive testing.
  unsigned long size = FPDF_GetTrailerEnds(document(), nullptr, 0);
  const std::vector<unsigned int> kExpectedEnds{474, 11384};
  ASSERT_EQ(kExpectedEnds.size(), size);
  std::vector<unsigned int> ends(size);
  ASSERT_EQ(size, FPDF_GetTrailerEnds(document(), ends.data(), size));
  ASSERT_EQ(kExpectedEnds, ends);
}

TEST_F(FPDFViewEmbedderTest, GetTrailerEndsWhitespace) {
  // Whitespace between 'endstream'/'endobj' and the newline.
  ASSERT_TRUE(OpenDocument("trailer_end_trailing_space.pdf"));

  unsigned long size = FPDF_GetTrailerEnds(document(), nullptr, 0);
  const std::vector<unsigned int> kExpectedEnds{1193};
  // Without the accompanying fix in place, this test would have failed, as the
  // size was 0, not 1, i.e. no trailer ends were found.
  ASSERT_EQ(kExpectedEnds.size(), size);
  std::vector<unsigned int> ends(size);
  ASSERT_EQ(size, FPDF_GetTrailerEnds(document(), ends.data(), size));
  EXPECT_EQ(kExpectedEnds, ends);
}

TEST_F(FPDFViewEmbedderTest, RenderXfaPage) {
  ASSERT_TRUE(OpenDocument("simple_xfa.pdf"));

  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // Should always be blank, as we're not testing `FPDF_FFLDraw()` here.
  TestRenderPageBitmapWithFlags(page.get(), 0, pdfium::kBlankPage612By792Png);
}

#if defined(PDF_USE_SKIA)
TEST_F(FPDFViewEmbedderTest, RenderPageToSkp) {
  if (!CFX_DefaultRenderDevice::UseSkiaRenderer()) {
    GTEST_SKIP() << "FPDF_RenderPageSkp() only makes sense with Skia";
  }

  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  TestRenderPageSkp(page.get(), pdfium::kRectanglesPng);
}

TEST_F(FPDFViewEmbedderTest, RenderXfaPageToSkp) {
  if (!CFX_DefaultRenderDevice::UseSkiaRenderer()) {
    GTEST_SKIP() << "FPDF_RenderPageSkp() only makes sense with Skia";
  }

  ASSERT_TRUE(OpenDocument("simple_xfa.pdf"));

  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // Should always be blank, as we're not testing `FPDF_FFLRecord()` here.
  TestRenderPageSkp(page.get(), pdfium::kBlankPage612By792Png);
}

TEST_F(FPDFViewEmbedderTest, Bug2087) {
  FPDF_DestroyLibrary();

  std::string agg_checksum;
  const FPDF_LIBRARY_CONFIG kAggConfig = {
      .version = 4,
      .m_pUserFontPaths = nullptr,
      .m_pIsolate = nullptr,
      .m_v8EmbedderSlot = 0,
      .m_pPlatform = nullptr,
      .m_RendererType = FPDF_RENDERERTYPE_AGG,
  };
  FPDF_InitLibraryWithConfig(&kAggConfig);
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  {
    ScopedPage page = LoadScopedPage(0);
    ScopedFPDFBitmap bitmap = RenderPage(page.get());
    agg_checksum = HashBitmap(bitmap.get());
  }
  CloseDocument();
  FPDF_DestroyLibrary();

  std::string skia_checksum;
  const FPDF_LIBRARY_CONFIG kSkiaConfig = {
      .version = 2,
      .m_pUserFontPaths = nullptr,
      .m_pIsolate = nullptr,
      .m_v8EmbedderSlot = 0,
  };
  FPDF_InitLibraryWithConfig(&kSkiaConfig);
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  {
    ScopedPage page = LoadScopedPage(0);
    ScopedFPDFBitmap bitmap = RenderPage(page.get());
    skia_checksum = HashBitmap(bitmap.get());
  }
  CloseDocument();

  EXPECT_NE(agg_checksum, skia_checksum);

  EmbedderTestEnvironment::GetInstance()->TearDown();
  EmbedderTestEnvironment::GetInstance()->SetUp();
}
#endif  // defined(PDF_USE_SKIA)

TEST_F(FPDFViewEmbedderTest, NoSmoothTextItalicOverlappingGlyphs) {
  ASSERT_TRUE(OpenDocument("bug_1919.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  TestRenderPageBitmapWithFlags(page.get(), FPDF_RENDER_NO_SMOOTHTEXT,
                                "bug_1919");
}

TEST_F(FPDFViewEmbedderTest, RenderTransparencyOnWhiteBackground) {
  ASSERT_TRUE(OpenDocument("bug_1302355.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  static constexpr int kWidth = 200;
  static constexpr int kHeight = 200;
  EXPECT_EQ(kWidth, static_cast<int>(FPDF_GetPageWidthF(page.get())));
  EXPECT_EQ(kHeight, static_cast<int>(FPDF_GetPageHeightF(page.get())));
  EXPECT_TRUE(FPDFPage_HasTransparency(page.get()));

  // PDFium does not render pages with transparencies correctly if it is
  // painting into a bitmap that is all white. Thus the output is expected to
  // be blank. Whereas painting into a transparent bitmap works correctly. If
  // the background needs to be white, the embedder can separately composite
  // the transparent bitmap onto an all white bitmap.
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(kWidth, kHeight, /*alpha=*/true));
  ASSERT_TRUE(
      FPDFBitmap_FillRect(bitmap.get(), 0, 0, kWidth, kHeight, 0xFFFFFFFF));
  FPDF_RenderPageBitmap(bitmap.get(), page.get(), /*start_x=*/0,
                        /*start_y=*/0, kWidth, kHeight, /*rotate=*/0,
                        /*flags=*/0);
  CompareBitmap(bitmap.get(), pdfium::kBlankPage200x200Png);
}

TEST_F(FPDFViewEmbedderTest, Bug2112) {
  static constexpr int kWidth = 595;
  static constexpr int kHeight = 842;
  static constexpr int kStride = kWidth * 3;
  std::vector<uint8_t> vec(kStride * kHeight);
  ScopedFPDFBitmap bitmap(FPDFBitmap_CreateEx(kWidth, kHeight, FPDFBitmap_BGR,
                                              vec.data(), kStride));
  EXPECT_EQ(FPDFBitmap_BGR, FPDFBitmap_GetFormat(bitmap.get()));
}

TEST_F(FPDFViewEmbedderTest, RenderAnnotsGrayScale) {
  ASSERT_TRUE(OpenDocument("annotation_highlight_square_with_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  constexpr char kGrayBasename[] = "annotation_with_grayscale";
  TestRenderPageBitmapWithInternalMemory(page.get(), FPDFBitmap_Gray,
                                         kGrayBasename, /*fuzzy=*/true);
}

TEST_F(FPDFViewEmbedderTest, BadFillRectInput) {
  static constexpr int kWidth = 200;
  static constexpr int kHeight = 200;
  static constexpr char kExpectedFilename[] = "bad_fill_rect";
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(200, 200, /*alpha=*/true));
  ASSERT_TRUE(FPDFBitmap_FillRect(bitmap.get(), /*left=*/0, /*top=*/0,
                                  /*width=*/kWidth,
                                  /*height=*/kHeight, 0xFFFF0000));
  CompareBitmap(bitmap.get(), kExpectedFilename);

  // Empty rect dimensions is a no-op.
  ASSERT_TRUE(FPDFBitmap_FillRect(bitmap.get(), /*left=*/0, /*top=*/0,
                                  /*width=*/0,
                                  /*height=*/0, 0xFF0000FF));
  CompareBitmap(bitmap.get(), kExpectedFilename);

  // Rect dimension overflows are also no-ops.
  ASSERT_FALSE(FPDFBitmap_FillRect(
      bitmap.get(), /*left=*/std::numeric_limits<int>::max(),
      /*top=*/0, /*width=*/std::numeric_limits<int>::max(),
      /*height=*/kHeight, 0xFF0000FF));
  CompareBitmap(bitmap.get(), kExpectedFilename);

  ASSERT_FALSE(FPDFBitmap_FillRect(
      bitmap.get(), /*left=*/0,
      /*top=*/std::numeric_limits<int>::max(), /*width=*/kWidth,
      /*height=*/std::numeric_limits<int>::max(), 0xFF0000FF));
  CompareBitmap(bitmap.get(), kExpectedFilename);

  // Make sure null bitmap handle does not trigger a crash.
  ASSERT_FALSE(FPDFBitmap_FillRect(nullptr, 0, 0, kWidth, kHeight, 0xFF0000FF));
}

TEST_F(FPDFViewEmbedderTest, BitmapBGRAPremulFormat) {
  static constexpr int kWidth = 100;
  static constexpr int kHeight = 200;
  ScopedFPDFBitmap bitmap(
      FPDFBitmap_CreateEx(kWidth, kHeight, FPDFBitmap_BGRA_Premul, nullptr, 0));
  if (CFX_DefaultRenderDevice::UseSkiaRenderer()) {
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(FPDFBitmap_BGRA_Premul, FPDFBitmap_GetFormat(bitmap.get()));
  } else {
    ASSERT_FALSE(bitmap);
  }
}

TEST_F(FPDFViewEmbedderTest, DocumentVersionInCatalog) {
  ASSERT_TRUE(OpenDocument("version_in_catalog.pdf"));
  int version;
  EXPECT_TRUE(FPDF_GetFileVersion(document(), &version));
  EXPECT_EQ(16, version);
}
