// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_named_pages.h"

#include <string>
#include <vector>

#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_layer.h"
#include "public/epdf_save.h"
#include "public/epdf_view.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

struct NamedPage {
  std::wstring key;
  unsigned int obj_num = 0;
  int kind = -1;
};

NamedPage ReadNamedPage(FPDF_DOCUMENT document, int tree, int index) {
  NamedPage entry;
  const unsigned long length = EPDFDoc_GetNamedPageAt(
      document, tree, index, nullptr, 0, &entry.obj_num, &entry.kind);
  EXPECT_GT(length, 0u);
  if (length == 0) {
    return entry;
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  unsigned int obj_num = 0;
  int kind = -1;
  EXPECT_EQ(length, EPDFDoc_GetNamedPageAt(document, tree, index, buffer.data(),
                                           length, &obj_num, &kind));
  EXPECT_EQ(entry.obj_num, obj_num);
  EXPECT_EQ(entry.kind, kind);
  entry.key = GetPlatformWString(buffer.data());
  return entry;
}

std::vector<NamedPage> ReadTree(FPDF_DOCUMENT document, int tree) {
  std::vector<NamedPage> entries;
  const int count = EPDFDoc_GetNamedPageCount(document, tree);
  for (int i = 0; i < count; ++i) {
    entries.push_back(ReadNamedPage(document, tree, i));
  }
  return entries;
}

unsigned int PageObjNum(FPDF_DOCUMENT document, int index) {
  return EPDFDoc_GetPageObjectNumberByIndex(document, index);
}

class EPDFNamedPagesEmbedderTest : public EmbedderTest {};

}  // namespace

TEST_F(EPDFNamedPagesEmbedderTest, ReadsNestedTreeInTreeOrder) {
  ASSERT_TRUE(OpenDocument("named_pages.pdf"));
  ASSERT_EQ(3, FPDF_GetPageCount(document()));
  const unsigned int page0 = PageObjNum(document(), 0);
  const unsigned int page1 = PageObjNum(document(), 1);
  const unsigned int page2 = PageObjNum(document(), 2);

  // Bad arguments.
  EXPECT_EQ(-1, EPDFDoc_GetNamedPageCount(nullptr, EPDF_NAMED_PAGE_TREE_PAGES));
  EXPECT_EQ(-1, EPDFDoc_GetNamedPageCount(document(), 7));
  EXPECT_EQ(0u, EPDFDoc_GetNamedPageAt(document(), EPDF_NAMED_PAGE_TREE_PAGES,
                                       99, nullptr, 0, nullptr, nullptr));

  // /Pages: both leaves flattened in tree order; a dangling value and a
  // BOM-encoded key both survive.
  std::vector<NamedPage> pages =
      ReadTree(document(), EPDF_NAMED_PAGE_TREE_PAGES);
  ASSERT_EQ(5u, pages.size());
  EXPECT_EQ(L"#alpha=Alpha", pages[0].key);
  EXPECT_EQ(page0, pages[0].obj_num);
  EXPECT_EQ(EPDF_NAMED_PAGE_KIND_PAGE, pages[0].kind);
  EXPECT_EQ(L"Approved=Goedgekeurd", pages[1].key);
  EXPECT_EQ(page1, pages[1].obj_num);
  EXPECT_EQ(EPDF_NAMED_PAGE_KIND_PAGE, pages[1].kind);
  EXPECT_EQ(L"Draft=Concept", pages[2].key);
  EXPECT_EQ(page2, pages[2].obj_num);
  EXPECT_EQ(L"Ghost", pages[3].key);
  EXPECT_EQ(0u, pages[3].obj_num);
  EXPECT_EQ(EPDF_NAMED_PAGE_KIND_DANGLING, pages[3].kind);
  EXPECT_EQ(L"Stämpel=Stämpel", pages[4].key);
  EXPECT_EQ(page1, pages[4].obj_num);
  EXPECT_EQ(EPDF_NAMED_PAGE_KIND_PAGE, pages[4].kind);

  // /Templates: the hidden page is reported, classified, and never counted
  // as a page.
  std::vector<NamedPage> templates =
      ReadTree(document(), EPDF_NAMED_PAGE_TREE_TEMPLATES);
  ASSERT_EQ(1u, templates.size());
  EXPECT_EQ(L"Tpl=Hidden", templates[0].key);
  EXPECT_EQ(6u, templates[0].obj_num);
  EXPECT_EQ(EPDF_NAMED_PAGE_KIND_TEMPLATE, templates[0].kind);
  EXPECT_EQ(3, FPDF_GetPageCount(document()));
}

TEST_F(EPDFNamedPagesEmbedderTest, SetReplacesRejectsAndRoundTrips) {
  ASSERT_TRUE(OpenDocument("named_pages.pdf"));
  const unsigned int page0 = PageObjNum(document(), 0);
  const unsigned int page2 = PageObjNum(document(), 2);

  // Refusals: empty key, unknown object, a template, a null document.
  ScopedFPDFWideString empty = GetFPDFWideString(L"");
  ScopedFPDFWideString final_key = GetFPDFWideString(L"Final=Definitief");
  EXPECT_FALSE(EPDFDoc_SetNamedPage(document(), empty.get(), page0));
  EXPECT_FALSE(EPDFDoc_SetNamedPage(document(), final_key.get(), 999));
  EXPECT_FALSE(EPDFDoc_SetNamedPage(document(), final_key.get(), 6));
  EXPECT_FALSE(EPDFDoc_SetNamedPage(nullptr, final_key.get(), page0));
  EXPECT_EQ(5,
            EPDFDoc_GetNamedPageCount(document(), EPDF_NAMED_PAGE_TREE_PAGES));

  // Create.
  ASSERT_TRUE(EPDFDoc_SetNamedPage(document(), final_key.get(), page2));
  EXPECT_EQ(6,
            EPDFDoc_GetNamedPageCount(document(), EPDF_NAMED_PAGE_TREE_PAGES));

  // Replace: same key, new page — still one entry.
  ScopedFPDFWideString approved = GetFPDFWideString(L"Approved=Goedgekeurd");
  ASSERT_TRUE(EPDFDoc_SetNamedPage(document(), approved.get(), page0));
  EXPECT_EQ(6,
            EPDFDoc_GetNamedPageCount(document(), EPDF_NAMED_PAGE_TREE_PAGES));

  // A non-ASCII key authored by us must read back identically to Acrobat's.
  ScopedFPDFWideString umlaut = GetFPDFWideString(L"Über=Über");
  ASSERT_TRUE(EPDFDoc_SetNamedPage(document(), umlaut.get(), page2));

  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ScopedSavedDoc saved = OpenScopedSavedDocument();
  ASSERT_TRUE(saved);
  const unsigned int saved_page0 = PageObjNum(saved.get(), 0);
  const unsigned int saved_page1 = PageObjNum(saved.get(), 1);
  const unsigned int saved_page2 = PageObjNum(saved.get(), 2);

  std::vector<NamedPage> pages =
      ReadTree(saved.get(), EPDF_NAMED_PAGE_TREE_PAGES);
  ASSERT_EQ(7u, pages.size());
  EXPECT_EQ(L"#alpha=Alpha", pages[0].key);
  EXPECT_EQ(saved_page0, pages[0].obj_num);
  EXPECT_EQ(L"Approved=Goedgekeurd", pages[1].key);
  EXPECT_EQ(saved_page0, pages[1].obj_num);  // replaced: page 1 -> page 0
  EXPECT_EQ(L"Draft=Concept", pages[2].key);
  EXPECT_EQ(saved_page2, pages[2].obj_num);
  EXPECT_EQ(L"Final=Definitief", pages[3].key);
  EXPECT_EQ(saved_page2, pages[3].obj_num);
  EXPECT_EQ(L"Ghost", pages[4].key);
  EXPECT_EQ(EPDF_NAMED_PAGE_KIND_DANGLING, pages[4].kind);
  EXPECT_EQ(L"Stämpel=Stämpel", pages[5].key);
  EXPECT_EQ(saved_page1, pages[5].obj_num);
  EXPECT_EQ(L"Über=Über", pages[6].key);
  EXPECT_EQ(saved_page2, pages[6].obj_num);
  EXPECT_EQ(EPDF_NAMED_PAGE_KIND_PAGE, pages[6].kind);
}

TEST_F(EPDFNamedPagesEmbedderTest, RemoveByKeyKeepsThePage) {
  ASSERT_TRUE(OpenDocument("named_pages.pdf"));
  ScopedFPDFWideString ghost = GetFPDFWideString(L"Ghost");
  ScopedFPDFWideString absent = GetFPDFWideString(L"Nope");
  ScopedFPDFWideString empty = GetFPDFWideString(L"");
  ScopedFPDFWideString draft = GetFPDFWideString(L"Draft=Concept");

  EXPECT_FALSE(EPDFDoc_RemoveNamedPage(nullptr, ghost.get()));
  EXPECT_FALSE(EPDFDoc_RemoveNamedPage(document(), empty.get()));
  EXPECT_FALSE(EPDFDoc_RemoveNamedPage(document(), absent.get()));
  EXPECT_TRUE(EPDFDoc_RemoveNamedPage(document(), ghost.get()));
  EXPECT_FALSE(EPDFDoc_RemoveNamedPage(document(), ghost.get()));
  EXPECT_TRUE(EPDFDoc_RemoveNamedPage(document(), draft.get()));
  EXPECT_EQ(3,
            EPDFDoc_GetNamedPageCount(document(), EPDF_NAMED_PAGE_TREE_PAGES));
  // The registration went, the page stayed.
  EXPECT_EQ(3, FPDF_GetPageCount(document()));

  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ScopedSavedDoc saved = OpenScopedSavedDocument();
  ASSERT_TRUE(saved);
  std::vector<NamedPage> pages =
      ReadTree(saved.get(), EPDF_NAMED_PAGE_TREE_PAGES);
  ASSERT_EQ(3u, pages.size());
  EXPECT_EQ(L"#alpha=Alpha", pages[0].key);
  EXPECT_EQ(L"Approved=Goedgekeurd", pages[1].key);
  EXPECT_EQ(L"Stämpel=Stämpel", pages[2].key);
  EXPECT_EQ(3, FPDF_GetPageCount(saved.get()));
}

TEST_F(EPDFNamedPagesEmbedderTest, DeletingAPageDropsItsRegistrations) {
  ASSERT_TRUE(OpenDocument("named_pages.pdf"));
  const unsigned int page0 = PageObjNum(document(), 0);
  const unsigned int page1 = PageObjNum(document(), 1);
  const unsigned int page2 = PageObjNum(document(), 2);

  // Two keys point at page 1 ("Approved=…" and the BOM key). Deleting the
  // page removes both, in the same operation, without touching the rest.
  ASSERT_TRUE(EPDFDoc_DeletePageByObjectNumber(document(), page1));
  EXPECT_EQ(2, FPDF_GetPageCount(document()));
  std::vector<NamedPage> pages =
      ReadTree(document(), EPDF_NAMED_PAGE_TREE_PAGES);
  ASSERT_EQ(3u, pages.size());
  EXPECT_EQ(L"#alpha=Alpha", pages[0].key);
  EXPECT_EQ(page0, pages[0].obj_num);
  EXPECT_EQ(L"Draft=Concept", pages[1].key);
  EXPECT_EQ(page2, pages[1].obj_num);
  EXPECT_EQ(L"Ghost", pages[2].key);
  EXPECT_EQ(EPDF_NAMED_PAGE_KIND_DANGLING, pages[2].kind);

  // The explicit helper is idempotent and reports its count.
  EXPECT_EQ(0, EPDFDoc_RemoveNamedPagesForPage(document(), page1));
  EXPECT_EQ(1, EPDFDoc_RemoveNamedPagesForPage(document(), page0));
  EXPECT_EQ(-1, EPDFDoc_RemoveNamedPagesForPage(document(), 0));
  EXPECT_EQ(-1, EPDFDoc_RemoveNamedPagesForPage(nullptr, page0));

  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ScopedSavedDoc saved = OpenScopedSavedDocument();
  ASSERT_TRUE(saved);
  EXPECT_EQ(2, FPDF_GetPageCount(saved.get()));
  pages = ReadTree(saved.get(), EPDF_NAMED_PAGE_TREE_PAGES);
  ASSERT_EQ(2u, pages.size());
  EXPECT_EQ(L"Draft=Concept", pages[0].key);
  EXPECT_EQ(PageObjNum(saved.get(), 1), pages[0].obj_num);
  EXPECT_EQ(L"Ghost", pages[1].key);
}

TEST_F(EPDFNamedPagesEmbedderTest, NewDocumentGrowsATreeOnFirstSet) {
  ASSERT_TRUE(OpenDocumentFromMemory(EmptyPdf()));
  ScopedFPDFPage first(FPDFPage_New(document(), 0, 200, 100));
  ScopedFPDFPage second(FPDFPage_New(document(), 1, 200, 100));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  const unsigned int page0 = PageObjNum(document(), 0);
  const unsigned int page1 = PageObjNum(document(), 1);

  EXPECT_EQ(0,
            EPDFDoc_GetNamedPageCount(document(), EPDF_NAMED_PAGE_TREE_PAGES));
  EXPECT_EQ(
      0, EPDFDoc_GetNamedPageCount(document(), EPDF_NAMED_PAGE_TREE_TEMPLATES));
  // Removing from a document without a tree is a no-op, never a creation.
  ScopedFPDFWideString approved = GetFPDFWideString(L"Approved=Approved");
  EXPECT_FALSE(EPDFDoc_RemoveNamedPage(document(), approved.get()));
  EXPECT_EQ(0, EPDFDoc_RemoveNamedPagesForPage(document(), page0));

  ScopedFPDFWideString draft = GetFPDFWideString(L"Draft=Draft");
  ASSERT_TRUE(EPDFDoc_SetNamedPage(document(), draft.get(), page1));
  ASSERT_TRUE(EPDFDoc_SetNamedPage(document(), approved.get(), page0));
  std::vector<NamedPage> pages =
      ReadTree(document(), EPDF_NAMED_PAGE_TREE_PAGES);
  ASSERT_EQ(2u, pages.size());
  // Key-sorted regardless of insertion order.
  EXPECT_EQ(L"Approved=Approved", pages[0].key);
  EXPECT_EQ(page0, pages[0].obj_num);
  EXPECT_EQ(L"Draft=Draft", pages[1].key);
  EXPECT_EQ(page1, pages[1].obj_num);

  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ScopedSavedDoc saved = OpenScopedSavedDocument();
  ASSERT_TRUE(saved);
  pages = ReadTree(saved.get(), EPDF_NAMED_PAGE_TREE_PAGES);
  ASSERT_EQ(2u, pages.size());
  EXPECT_EQ(L"Approved=Approved", pages[0].key);
  EXPECT_EQ(PageObjNum(saved.get(), 0), pages[0].obj_num);
  EXPECT_EQ(EPDF_NAMED_PAGE_KIND_PAGE, pages[0].kind);
}

TEST_F(EPDFNamedPagesEmbedderTest, LayerDocumentCapturesRegistrations) {
  ASSERT_TRUE(OpenDocument("named_pages.pdf"));
  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  const std::string base_pdf = GetString();
  ASSERT_FALSE(base_pdf.empty());

  EPDF_BASE_DOCUMENT base =
      EPDF_LoadMemBaseDocument64(base_pdf.data(), base_pdf.size(), nullptr);
  ASSERT_TRUE(base);

  std::string delta;
  {
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &status));
    ASSERT_TRUE(layer);
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    EXPECT_EQ(
        5, EPDFDoc_GetNamedPageCount(layer.get(), EPDF_NAMED_PAGE_TREE_PAGES));
    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.get()));

    ScopedFPDFWideString final_key = GetFPDFWideString(L"Final=Definitief");
    ScopedFPDFWideString ghost = GetFPDFWideString(L"Ghost");
    ASSERT_TRUE(EPDFDoc_SetNamedPage(layer.get(), final_key.get(),
                                     PageObjNum(layer.get(), 0)));
    ASSERT_TRUE(EPDFDoc_RemoveNamedPage(layer.get(), ghost.get()));
    // The catalog path was promoted into the overlay — the write is savable.
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
    std::vector<NamedPage> pages =
        ReadTree(replayed.get(), EPDF_NAMED_PAGE_TREE_PAGES);
    ASSERT_EQ(5u, pages.size());
    EXPECT_EQ(L"#alpha=Alpha", pages[0].key);
    EXPECT_EQ(L"Approved=Goedgekeurd", pages[1].key);
    EXPECT_EQ(L"Draft=Concept", pages[2].key);
    EXPECT_EQ(L"Final=Definitief", pages[3].key);
    EXPECT_EQ(PageObjNum(replayed.get(), 0), pages[3].obj_num);
    EXPECT_EQ(EPDF_NAMED_PAGE_KIND_PAGE, pages[3].kind);
    EXPECT_EQ(L"Stämpel=Stämpel", pages[4].key);
  }
  EPDF_ReleaseBaseDocument(base);
}
