// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_pieceinfo.h"

#include <set>
#include <string>
#include <vector>

#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_layer.h"
#include "public/epdf_save.h"
#include "public/epdf_view.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_ppo.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/embedpdf_layer_mode.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

std::wstring GetDocumentLastModified(FPDF_DOCUMENT document) {
  const unsigned long length = EPDFDoc_GetLastModified(document, nullptr, 0);
  if (length == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length, EPDFDoc_GetLastModified(document, buffer.data(), length));
  return GetPlatformWString(buffer.data());
}

std::wstring GetDocumentPieceInfoLastModified(FPDF_DOCUMENT document,
                                              const char* application) {
  const unsigned long length =
      EPDFDoc_GetPieceInfoLastModified(document, application, nullptr, 0);
  if (length == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length, EPDFDoc_GetPieceInfoLastModified(document, application,
                                                     buffer.data(), length));
  return GetPlatformWString(buffer.data());
}

std::wstring GetDocumentPieceInfoString(FPDF_DOCUMENT document,
                                        const char* application,
                                        const char* key) {
  const unsigned long length =
      EPDFDoc_GetPieceInfoString(document, application, key, nullptr, 0);
  if (length == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length, EPDFDoc_GetPieceInfoString(document, application, key,
                                               buffer.data(), length));
  return GetPlatformWString(buffer.data());
}

std::wstring GetDocumentPieceInfoStringArrayAt(FPDF_DOCUMENT document,
                                               const char* application,
                                               const char* key,
                                               int index) {
  const unsigned long length = EPDFDoc_GetPieceInfoStringArrayAt(
      document, application, key, index, nullptr, 0);
  if (length == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length,
            EPDFDoc_GetPieceInfoStringArrayAt(document, application, key, index,
                                              buffer.data(), length));
  return GetPlatformWString(buffer.data());
}

std::string GetDocumentPieceInfoName(FPDF_DOCUMENT document,
                                     const char* application,
                                     const char* key) {
  const unsigned long length =
      EPDFDoc_GetPieceInfoName(document, application, key, nullptr, 0);
  if (length == 0) {
    return std::string();
  }
  std::vector<char> buffer(length);
  EXPECT_EQ(length, EPDFDoc_GetPieceInfoName(document, application, key,
                                             buffer.data(), length));
  return std::string(buffer.data());
}

std::string GetDocumentPieceInfoEntryAt(FPDF_DOCUMENT document, int index) {
  const unsigned long length =
      EPDFDoc_GetPieceInfoEntryAt(document, index, nullptr, 0);
  if (length == 0) {
    return std::string();
  }
  std::vector<char> buffer(length);
  EXPECT_EQ(length, EPDFDoc_GetPieceInfoEntryAt(document, index, buffer.data(),
                                                length));
  return std::string(buffer.data());
}

std::wstring GetPageLastModified(FPDF_DOCUMENT document,
                                 unsigned int page_object_number) {
  const unsigned long length =
      EPDFDoc_GetPageLastModified(document, page_object_number, nullptr, 0);
  if (length == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length, EPDFDoc_GetPageLastModified(document, page_object_number,
                                                buffer.data(), length));
  return GetPlatformWString(buffer.data());
}

std::wstring GetPieceInfoLastModified(FPDF_DOCUMENT document,
                                      unsigned int page_object_number,
                                      const char* application) {
  const unsigned long length = EPDFDoc_GetPagePieceInfoLastModified(
      document, page_object_number, application, nullptr, 0);
  if (length == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length, EPDFDoc_GetPagePieceInfoLastModified(
                        document, page_object_number, application,
                        buffer.data(), length));
  return GetPlatformWString(buffer.data());
}

std::wstring GetPieceInfoString(FPDF_DOCUMENT document,
                                unsigned int page_object_number,
                                const char* application,
                                const char* key) {
  const unsigned long length = EPDFDoc_GetPagePieceInfoString(
      document, page_object_number, application, key, nullptr, 0);
  if (length == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length, EPDFDoc_GetPagePieceInfoString(document, page_object_number,
                                                   application, key,
                                                   buffer.data(), length));
  return GetPlatformWString(buffer.data());
}

std::wstring GetPieceInfoStringArrayAt(FPDF_DOCUMENT document,
                                       unsigned int page_object_number,
                                       const char* application,
                                       const char* key,
                                       int index) {
  const unsigned long length = EPDFDoc_GetPagePieceInfoStringArrayAt(
      document, page_object_number, application, key, index, nullptr, 0);
  if (length == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length, EPDFDoc_GetPagePieceInfoStringArrayAt(
                        document, page_object_number, application, key, index,
                        buffer.data(), length));
  return GetPlatformWString(buffer.data());
}

std::string GetPieceInfoName(FPDF_DOCUMENT document,
                             unsigned int page_object_number,
                             const char* application,
                             const char* key) {
  const unsigned long length = EPDFDoc_GetPagePieceInfoName(
      document, page_object_number, application, key, nullptr, 0);
  if (length == 0) {
    return std::string();
  }
  std::vector<char> buffer(length);
  EXPECT_EQ(length, EPDFDoc_GetPagePieceInfoName(document, page_object_number,
                                                 application, key,
                                                 buffer.data(), length));
  return std::string(buffer.data());
}

std::string GetPieceInfoEntryAt(FPDF_DOCUMENT document,
                                unsigned int page_object_number,
                                int index) {
  const unsigned long length = EPDFDoc_GetPagePieceInfoEntryAt(
      document, page_object_number, index, nullptr, 0);
  if (length == 0) {
    return std::string();
  }
  std::vector<char> buffer(length);
  EXPECT_EQ(length,
            EPDFDoc_GetPagePieceInfoEntryAt(document, page_object_number, index,
                                            buffer.data(), length));
  return std::string(buffer.data());
}

}  // namespace

class EPDFPieceInfoEmbedderTest : public EmbedderTest {};

TEST_F(EPDFPieceInfoEmbedderTest, CatalogTypedValuesSaveReloadAndLayerRead) {
  ASSERT_TRUE(OpenDocumentFromMemory(EmptyPdf()));
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 240, 100));
  ASSERT_TRUE(page);

  ScopedFPDFWideString timestamp =
      GetFPDFWideString(L"D:20260713153000+03'00'");
  ScopedFPDFWideString name = GetFPDFWideString(L"Company Stamps \u2713");
  ScopedFPDFWideString review = GetFPDFWideString(L"Review");
  ScopedFPDFWideString internal = GetFPDFWideString(L"Internal");
  const FPDF_WIDESTRING categories[] = {review.get(), internal.get()};

  EXPECT_TRUE(EPDFDoc_SetPieceInfoString(document(), "EMBD_StampLibrary",
                                         "Name", name.get(), timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPieceInfoNumber(document(), "EMBD_StampLibrary",
                                         "Version", 1.0f, timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPieceInfoBoolean(document(), "EMBD_StampLibrary",
                                          "Archived", true, timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPieceInfoName(document(), "EMBD_StampLibrary", "Kind",
                                       "StampLibrary", timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPieceInfoStringArray(
      document(), "EMBD_StampLibrary", "Categories", categories,
      std::size(categories), timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPieceInfoBoolean(document(), "EMBD_Other", "Pinned",
                                          true, timestamp.get()));

  EXPECT_TRUE(EPDFDoc_HasPieceInfoEntry(document(), "EMBD_StampLibrary"));
  EXPECT_EQ(2, EPDFDoc_GetPieceInfoEntryCount(document()));
  std::set<std::string> applications;
  applications.insert(GetDocumentPieceInfoEntryAt(document(), 0));
  applications.insert(GetDocumentPieceInfoEntryAt(document(), 1));
  EXPECT_EQ((std::set<std::string>{"EMBD_Other", "EMBD_StampLibrary"}),
            applications);
  EXPECT_EQ(5, EPDFDoc_GetPieceInfoKeyCount(document(), "EMBD_StampLibrary"));

  EXPECT_EQ(FPDF_OBJECT_STRING, EPDFDoc_GetPieceInfoValueType(
                                    document(), "EMBD_StampLibrary", "Name"));
  EXPECT_EQ(FPDF_OBJECT_NUMBER,
            EPDFDoc_GetPieceInfoValueType(document(), "EMBD_StampLibrary",
                                          "Version"));
  EXPECT_EQ(FPDF_OBJECT_BOOLEAN,
            EPDFDoc_GetPieceInfoValueType(document(), "EMBD_StampLibrary",
                                          "Archived"));
  EXPECT_EQ(FPDF_OBJECT_NAME, EPDFDoc_GetPieceInfoValueType(
                                  document(), "EMBD_StampLibrary", "Kind"));
  EXPECT_EQ(FPDF_OBJECT_ARRAY,
            EPDFDoc_GetPieceInfoValueType(document(), "EMBD_StampLibrary",
                                          "Categories"));
  EXPECT_EQ(
      L"Company Stamps \u2713",
      GetDocumentPieceInfoString(document(), "EMBD_StampLibrary", "Name"));
  float number = 0.0f;
  EXPECT_TRUE(EPDFDoc_GetPieceInfoNumber(document(), "EMBD_StampLibrary",
                                         "Version", &number));
  EXPECT_FLOAT_EQ(1.0f, number);
  FPDF_BOOL boolean = false;
  EXPECT_TRUE(EPDFDoc_GetPieceInfoBoolean(document(), "EMBD_StampLibrary",
                                          "Archived", &boolean));
  EXPECT_TRUE(boolean);
  EXPECT_EQ("StampLibrary",
            GetDocumentPieceInfoName(document(), "EMBD_StampLibrary", "Kind"));
  EXPECT_EQ(2, EPDFDoc_GetPieceInfoStringArrayCount(
                   document(), "EMBD_StampLibrary", "Categories"));
  EXPECT_EQ(L"Review", GetDocumentPieceInfoStringArrayAt(
                           document(), "EMBD_StampLibrary", "Categories", 0));
  EXPECT_EQ(L"Internal", GetDocumentPieceInfoStringArrayAt(
                             document(), "EMBD_StampLibrary", "Categories", 1));
  EXPECT_EQ(L"D:20260713153000+03'00'", GetDocumentLastModified(document()));
  EXPECT_EQ(L"D:20260713153000+03'00'",
            GetDocumentPieceInfoLastModified(document(), "EMBD_StampLibrary"));

  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  const std::string saved_pdf = GetString();
  ASSERT_FALSE(saved_pdf.empty());

  ScopedSavedDoc saved_document = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_document);
  EXPECT_EQ(L"Company Stamps \u2713",
            GetDocumentPieceInfoString(saved_document.get(),
                                       "EMBD_StampLibrary", "Name"));
  EXPECT_EQ(L"D:20260713153000+03'00'",
            GetDocumentLastModified(saved_document.get()));

  EPDF_BASE_DOCUMENT base =
      EPDF_LoadMemBaseDocument64(saved_pdf.data(), saved_pdf.size(), nullptr);
  ASSERT_TRUE(base);
  {
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &status));
    ASSERT_TRUE(layer);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));
    EXPECT_EQ(2, EPDFDoc_GetPieceInfoEntryCount(layer.get()));
    EXPECT_EQ(
        L"Company Stamps \u2713",
        GetDocumentPieceInfoString(layer.get(), "EMBD_StampLibrary", "Name"));
    EXPECT_EQ(L"D:20260713153000+03'00'", GetDocumentLastModified(layer.get()));
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));
  }

  std::string delta;
  {
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &status));
    ASSERT_TRUE(layer);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    ScopedFPDFWideString updated_timestamp =
        GetFPDFWideString(L"D:20260713160000+03'00'");
    ScopedFPDFWideString updated_name =
        GetFPDFWideString(L"Updated Company Stamps");
    ASSERT_TRUE(EPDFDoc_SetPieceInfoString(layer.get(), "EMBD_StampLibrary",
                                           "Name", updated_name.get(),
                                           updated_timestamp.get()));
    EXPECT_GT(EPDFLayer_GetPromotedObjectCount(layer.get()), 0u);

    ClearString();
    EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
    ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, &save_status));
    EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
    delta = GetString();
    ASSERT_FALSE(delta.empty());
  }
  {
    FPDF_FILEACCESS delta_access = {};
    delta_access.m_FileLen = delta.size();
    delta_access.m_GetBlock = GetBlockFromString;
    delta_access.m_Param = &delta;
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument replayed(
        EPDFLayer_OpenLayer(base, &delta_access, nullptr, &status));
    ASSERT_TRUE(replayed);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    EXPECT_EQ(L"Updated Company Stamps",
              GetDocumentPieceInfoString(replayed.get(), "EMBD_StampLibrary",
                                         "Name"));
    EXPECT_EQ(L"D:20260713160000+03'00'",
              GetDocumentLastModified(replayed.get()));
  }
  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFPieceInfoEmbedderTest, CatalogGranularClearPreservesOtherData) {
  ASSERT_TRUE(OpenDocumentFromMemory(EmptyPdf()));
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 240, 100));
  ASSERT_TRUE(page);

  ScopedFPDFWideString timestamp =
      GetFPDFWideString(L"D:20260713154500+03'00'");
  ScopedFPDFWideString value = GetFPDFWideString(L"value");

  EXPECT_TRUE(EPDFDoc_ClearPieceInfoKey(document(), "EMBD_Missing", "Missing",
                                        nullptr));
  EXPECT_EQ(0, EPDFDoc_GetPieceInfoEntryCount(document()));
  EXPECT_TRUE(EPDFDoc_SetPieceInfoString(document(), "EMBD_First", "Remove",
                                         value.get(), timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPieceInfoString(document(), "EMBD_First", "Keep",
                                         value.get(), timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPieceInfoString(document(), "EMBD_Second", "Keep",
                                         value.get(), timestamp.get()));

  EXPECT_TRUE(EPDFDoc_ClearPieceInfoKey(document(), "EMBD_First", "Remove",
                                        timestamp.get()));
  EXPECT_EQ(FPDF_OBJECT_UNKNOWN,
            EPDFDoc_GetPieceInfoValueType(document(), "EMBD_First", "Remove"));
  EXPECT_EQ(L"value",
            GetDocumentPieceInfoString(document(), "EMBD_First", "Keep"));
  EXPECT_TRUE(EPDFDoc_ClearPieceInfoEntry(document(), "EMBD_First"));
  EXPECT_FALSE(EPDFDoc_HasPieceInfoEntry(document(), "EMBD_First"));
  EXPECT_TRUE(EPDFDoc_HasPieceInfoEntry(document(), "EMBD_Second"));
  EXPECT_EQ(1, EPDFDoc_GetPieceInfoEntryCount(document()));
  EXPECT_TRUE(EPDFDoc_ClearPieceInfoEntry(document(), "EMBD_Second"));
  EXPECT_EQ(0, EPDFDoc_GetPieceInfoEntryCount(document()));
  EXPECT_EQ(L"D:20260713154500+03'00'", GetDocumentLastModified(document()));
}

TEST_F(EPDFPieceInfoEmbedderTest, TypedValuesSaveReloadAndImport) {
  ASSERT_TRUE(OpenDocumentFromMemory(EmptyPdf()));
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 240, 100));
  ASSERT_TRUE(page);
  const unsigned int page_object_number = EPDFPage_GetObjectNumber(page.get());
  ASSERT_NE(0u, page_object_number);

  ScopedFPDFWideString timestamp =
      GetFPDFWideString(L"D:20260713093703+03'00'");
  ScopedFPDFWideString name = GetFPDFWideString(L"Approved \u2713");
  ScopedFPDFWideString review = GetFPDFWideString(L"Review");
  ScopedFPDFWideString internal = GetFPDFWideString(L"Internal");
  const FPDF_WIDESTRING categories[] = {review.get(), internal.get()};

  EXPECT_TRUE(EPDFDoc_SetPagePieceInfoString(document(), page_object_number,
                                             "EMBD_Stamp", "Name", name.get(),
                                             timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPagePieceInfoNumber(document(), page_object_number,
                                             "EMBD_Stamp", "Version", 1.0f,
                                             timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPagePieceInfoBoolean(document(), page_object_number,
                                              "EMBD_Stamp", "Archived", true,
                                              timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPagePieceInfoName(document(), page_object_number,
                                           "EMBD_Stamp", "Kind", "Stamp",
                                           timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPagePieceInfoStringArray(
      document(), page_object_number, "EMBD_Stamp", "Categories", categories,
      std::size(categories), timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPagePieceInfoBoolean(document(), page_object_number,
                                              "EMBD_PageState", "Pinned", true,
                                              timestamp.get()));

  EXPECT_TRUE(EPDFDoc_HasPagePieceInfoEntry(document(), page_object_number,
                                            "EMBD_Stamp"));
  EXPECT_EQ(2,
            EPDFDoc_GetPagePieceInfoEntryCount(document(), page_object_number));
  std::set<std::string> applications;
  applications.insert(GetPieceInfoEntryAt(document(), page_object_number, 0));
  applications.insert(GetPieceInfoEntryAt(document(), page_object_number, 1));
  EXPECT_EQ((std::set<std::string>{"EMBD_PageState", "EMBD_Stamp"}),
            applications);

  EXPECT_EQ(FPDF_OBJECT_STRING,
            EPDFDoc_GetPagePieceInfoValueType(document(), page_object_number,
                                              "EMBD_Stamp", "Name"));
  EXPECT_EQ(FPDF_OBJECT_NUMBER,
            EPDFDoc_GetPagePieceInfoValueType(document(), page_object_number,
                                              "EMBD_Stamp", "Version"));
  EXPECT_EQ(FPDF_OBJECT_BOOLEAN,
            EPDFDoc_GetPagePieceInfoValueType(document(), page_object_number,
                                              "EMBD_Stamp", "Archived"));
  EXPECT_EQ(FPDF_OBJECT_NAME,
            EPDFDoc_GetPagePieceInfoValueType(document(), page_object_number,
                                              "EMBD_Stamp", "Kind"));
  EXPECT_EQ(FPDF_OBJECT_ARRAY,
            EPDFDoc_GetPagePieceInfoValueType(document(), page_object_number,
                                              "EMBD_Stamp", "Categories"));
  EXPECT_EQ(
      L"Approved \u2713",
      GetPieceInfoString(document(), page_object_number, "EMBD_Stamp", "Name"));
  float number = 0.0f;
  EXPECT_TRUE(EPDFDoc_GetPagePieceInfoNumber(document(), page_object_number,
                                             "EMBD_Stamp", "Version", &number));
  EXPECT_FLOAT_EQ(1.0f, number);
  FPDF_BOOL boolean = false;
  EXPECT_TRUE(EPDFDoc_GetPagePieceInfoBoolean(
      document(), page_object_number, "EMBD_Stamp", "Archived", &boolean));
  EXPECT_TRUE(boolean);
  EXPECT_EQ("Stamp", GetPieceInfoName(document(), page_object_number,
                                      "EMBD_Stamp", "Kind"));
  EXPECT_EQ(2, EPDFDoc_GetPagePieceInfoStringArrayCount(
                   document(), page_object_number, "EMBD_Stamp", "Categories"));
  EXPECT_EQ(L"Review",
            GetPieceInfoStringArrayAt(document(), page_object_number,
                                      "EMBD_Stamp", "Categories", 0));
  EXPECT_EQ(L"Internal",
            GetPieceInfoStringArrayAt(document(), page_object_number,
                                      "EMBD_Stamp", "Categories", 1));
  EXPECT_EQ(L"D:20260713093703+03'00'",
            GetPageLastModified(document(), page_object_number));
  EXPECT_EQ(
      L"D:20260713093703+03'00'",
      GetPieceInfoLastModified(document(), page_object_number, "EMBD_Stamp"));

  embedpdf_test::ScopedTestDocument imported(NewTestDocument());
  ASSERT_TRUE(imported);
  static constexpr int kPageIndices[] = {0};
  ASSERT_TRUE(FPDF_ImportPagesByIndex(imported.get(), document(), kPageIndices,
                                      std::size(kPageIndices), 0));
  const unsigned int imported_page_object_number =
      EPDFDoc_GetPageObjectNumberByIndex(imported.get(), 0);
  ASSERT_NE(0u, imported_page_object_number);
  EXPECT_EQ(L"Approved \u2713",
            GetPieceInfoString(imported.get(), imported_page_object_number,
                               "EMBD_Stamp", "Name"));

  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  const std::string saved_pdf = GetString();
  ASSERT_FALSE(saved_pdf.empty());

  ScopedSavedDoc saved_document = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_document);
  const unsigned int saved_page_object_number =
      EPDFDoc_GetPageObjectNumberByIndex(saved_document.get(), 0);
  ASSERT_NE(0u, saved_page_object_number);
  EXPECT_EQ(L"Approved \u2713",
            GetPieceInfoString(saved_document.get(), saved_page_object_number,
                               "EMBD_Stamp", "Name"));
  EXPECT_EQ(L"D:20260713093703+03'00'",
            GetPieceInfoLastModified(saved_document.get(),
                                     saved_page_object_number, "EMBD_Stamp"));

  EPDF_BASE_DOCUMENT base =
      EPDF_LoadMemBaseDocument64(saved_pdf.data(), saved_pdf.size(), nullptr);
  ASSERT_TRUE(base);
  {
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &status));
    ASSERT_TRUE(layer);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));
    const unsigned int layer_page_object_number =
        EPDFDoc_GetPageObjectNumberByIndex(layer.get(), 0);
    EXPECT_EQ(2, EPDFDoc_GetPagePieceInfoEntryCount(layer.get(),
                                                    layer_page_object_number));
    EXPECT_EQ(L"Approved \u2713",
              GetPieceInfoString(layer.get(), layer_page_object_number,
                                 "EMBD_Stamp", "Name"));
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));
  }
  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFPieceInfoEmbedderTest, GranularClearPreservesOtherData) {
  ASSERT_TRUE(OpenDocumentFromMemory(EmptyPdf()));
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 240, 100));
  ASSERT_TRUE(page);
  const unsigned int page_object_number = EPDFPage_GetObjectNumber(page.get());
  ASSERT_NE(0u, page_object_number);

  ScopedFPDFWideString timestamp =
      GetFPDFWideString(L"D:20260713103000+03'00'");
  ScopedFPDFWideString value = GetFPDFWideString(L"value");

  EXPECT_TRUE(EPDFDoc_ClearPagePieceInfoKey(
      document(), page_object_number, "EMBD_Missing", "Missing", nullptr));
  EXPECT_EQ(0,
            EPDFDoc_GetPagePieceInfoEntryCount(document(), page_object_number));
  EXPECT_TRUE(EPDFDoc_SetPagePieceInfoString(document(), page_object_number,
                                             "EMBD_First", "Remove",
                                             value.get(), timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPagePieceInfoString(document(), page_object_number,
                                             "EMBD_First", "Keep", value.get(),
                                             timestamp.get()));
  EXPECT_TRUE(EPDFDoc_SetPagePieceInfoString(document(), page_object_number,
                                             "EMBD_Second", "Keep", value.get(),
                                             timestamp.get()));

  EXPECT_TRUE(EPDFDoc_ClearPagePieceInfoKey(
      document(), page_object_number, "EMBD_First", "Remove", timestamp.get()));
  EXPECT_EQ(FPDF_OBJECT_UNKNOWN,
            EPDFDoc_GetPagePieceInfoValueType(document(), page_object_number,
                                              "EMBD_First", "Remove"));
  EXPECT_EQ(L"value", GetPieceInfoString(document(), page_object_number,
                                         "EMBD_First", "Keep"));
  EXPECT_TRUE(EPDFDoc_ClearPagePieceInfoEntry(document(), page_object_number,
                                              "EMBD_First"));
  EXPECT_FALSE(EPDFDoc_HasPagePieceInfoEntry(document(), page_object_number,
                                             "EMBD_First"));
  EXPECT_TRUE(EPDFDoc_HasPagePieceInfoEntry(document(), page_object_number,
                                            "EMBD_Second"));
  EXPECT_EQ(1,
            EPDFDoc_GetPagePieceInfoEntryCount(document(), page_object_number));
  EXPECT_TRUE(EPDFDoc_ClearPagePieceInfoEntry(document(), page_object_number,
                                              "EMBD_Second"));
  EXPECT_EQ(0,
            EPDFDoc_GetPagePieceInfoEntryCount(document(), page_object_number));
}
