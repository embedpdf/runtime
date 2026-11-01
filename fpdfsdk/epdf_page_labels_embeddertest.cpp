// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include <ostream>
#include <string>
#include <vector>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_doc.h"
#include "public/epdf_layer.h"
#include "public/epdf_save.h"
#include "public/fpdf_doc.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "testing/embedder_test.h"
#include "testing/embedpdf_layer_mode.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

namespace {

struct Section {
  int first_page = 0;
  int style = EPDF_PAGE_LABEL_STYLE_DECIMAL;
  int first_number = 1;
  std::wstring prefix;

  bool operator==(const Section& other) const {
    return first_page == other.first_page && style == other.style &&
           first_number == other.first_number && prefix == other.prefix;
  }
};

void PrintTo(const Section& section, std::ostream* os) {
  *os << "{" << section.first_page << ", style " << section.style << ", from "
      << section.first_number << ", " << testing::PrintToString(section.prefix)
      << "}";
}

std::vector<Section> SectionsOf(FPDF_DOCUMENT document) {
  std::vector<Section> sections;
  const int count = EPDFDoc_GetPageLabelSectionCount(document);
  for (int i = 0; i < count; ++i) {
    Section section;
    const unsigned long length = EPDFDoc_GetPageLabelSectionAt(
        document, i, nullptr, 0, &section.first_page, &section.style,
        &section.first_number);
    EXPECT_GE(length, 2u);
    std::vector<FPDF_WCHAR> prefix = GetFPDFWideStringBuffer(length);
    EXPECT_EQ(length,
              EPDFDoc_GetPageLabelSectionAt(document, i, prefix.data(), length,
                                            nullptr, nullptr, nullptr));
    section.prefix = GetPlatformWString(prefix.data());
    sections.push_back(section);
  }
  return sections;
}

bool WriteSections(FPDF_DOCUMENT document,
                   const std::vector<Section>& sections) {
  std::vector<int> first_pages;
  std::vector<int> styles;
  std::vector<int> first_numbers;
  std::vector<ScopedFPDFWideString> owned;
  std::vector<FPDF_WIDESTRING> prefixes;
  for (const Section& section : sections) {
    first_pages.push_back(section.first_page);
    styles.push_back(section.style);
    first_numbers.push_back(section.first_number);
    owned.push_back(GetFPDFWideString(section.prefix));
    prefixes.push_back(owned.back().get());
  }
  return EPDFDoc_SetPageLabelSections(document, first_pages.data(),
                                      styles.data(), first_numbers.data(),
                                      prefixes.data(), sections.size());
}

// Each page's label as FPDF_GetPageLabel() gives it, "(none)" for none.
std::vector<std::wstring> LabelsOf(FPDF_DOCUMENT document) {
  std::vector<std::wstring> labels;
  for (int i = 0; i < FPDF_GetPageCount(document); ++i) {
    const unsigned long length = FPDF_GetPageLabel(document, i, nullptr, 0);
    if (length == 0) {
      labels.push_back(L"(none)");
      continue;
    }
    std::vector<FPDF_WCHAR> label = GetFPDFWideStringBuffer(length);
    FPDF_GetPageLabel(document, i, label.data(), length);
    labels.push_back(GetPlatformWString(label.data()));
  }
  return labels;
}

// The object /PageLabels names in the catalog, 0 when it is direct or
// missing.
uint32_t PageLabelsObject(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  CPDF_DocumentViewScope document_view(doc);
  RetainPtr<const CPDF_Reference> reference =
      ToReference(doc->GetRoot()->GetObjectFor("PageLabels"));
  return reference ? reference->GetRefObjNum() : 0;
}

embedpdf_test::ScopedTestDocument NewDocument(int page_count) {
  embedpdf_test::ScopedTestDocument document(NewTestDocument());
  for (int i = 0; i < page_count; ++i) {
    FPDF_ClosePage(FPDFPage_New(document.get(), i, 612, 792));
  }
  return document;
}

// page_labels.pdf's sections: front matter in roman numerals, a body from 1,
// an appendix as zzA, zzB, and a last page labelled "".
const std::vector<Section> kFileSections = {
    {0, EPDF_PAGE_LABEL_STYLE_LOWER_ROMAN, 1, L""},
    {2, EPDF_PAGE_LABEL_STYLE_DECIMAL, 1, L""},
    {4, EPDF_PAGE_LABEL_STYLE_UPPER_LETTERS, 1, L"zz"},
    {6, EPDF_PAGE_LABEL_STYLE_NONE, 1, L""},
};
const std::vector<std::wstring> kFileLabels = {L"i",   L"ii",  L"1", L"2",
                                               L"zzA", L"zzB", L""};

}  // namespace

class EPDFPageLabelsEmbedderTest : public EmbedderTest {
 protected:
  embedpdf_test::ScopedTestDocument SaveAndReopen(FPDF_DOCUMENT document) {
    ClearString();
    EXPECT_TRUE(FPDF_SaveAsCopy(document, this, 0));
    saved_.push_back(GetString());
    const std::string& bytes = saved_.back();
    return embedpdf_test::ScopedTestDocument(
        embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                              nullptr));
  }

  // page_labels.pdf as a layer over a frozen base.
  ScopedFPDFDocument OpenLayer() {
    if (!base_) {
      const std::string path = PathService::GetTestFilePath("page_labels.pdf");
      bytes_ = GetFileContents(path.c_str());
      base_ = EPDF_LoadMemBaseDocument(
          bytes_.data(), static_cast<int>(bytes_.size()), nullptr);
    }
    return ScopedFPDFDocument(
        EPDFLayer_OpenLayer(base_, nullptr, nullptr, nullptr));
  }

  ScopedFPDFDocument SaveAndReopenArtifact(FPDF_DOCUMENT layer) {
    ClearString();
    EXPECT_TRUE(EPDFLayer_SaveLayerArtifact(layer, this, nullptr));
    saved_.push_back(GetString());
    FPDF_FILEACCESS access = {};
    access.m_FileLen = saved_.back().size();
    access.m_GetBlock = GetBlockFromString;
    access.m_Param = &saved_.back();
    return ScopedFPDFDocument(
        EPDFLayer_OpenLayerArtifact(base_, &access, nullptr, nullptr));
  }

  void TearDown() override {
    if (base_) {
      EPDF_ReleaseBaseDocument(base_);
    }
    EmbedderTest::TearDown();
  }

 private:
  std::vector<std::string> saved_;
  std::vector<uint8_t> bytes_;
  EPDF_BASE_DOCUMENT base_ = nullptr;
};

TEST_F(EPDFPageLabelsEmbedderTest, ReadsTheSectionsOfAFile) {
  ASSERT_TRUE(OpenDocument("page_labels.pdf"));
  SetWholeFileAvailable();
  EXPECT_EQ(kFileSections, SectionsOf(document()));
  EXPECT_EQ(kFileLabels, LabelsOf(document()));

  int first_page = -1;
  EXPECT_EQ(0u, EPDFDoc_GetPageLabelSectionAt(document(), 4, nullptr, 0,
                                              &first_page, nullptr, nullptr));
  EXPECT_EQ(0u, EPDFDoc_GetPageLabelSectionAt(document(), -1, nullptr, 0,
                                              &first_page, nullptr, nullptr));
  EXPECT_EQ(-1, first_page);
  EXPECT_EQ(-1, EPDFDoc_GetPageLabelSectionCount(nullptr));
}

TEST_F(EPDFPageLabelsEmbedderTest, AFileWithoutPageLabelsHasNoSections) {
  ASSERT_TRUE(OpenDocument("about_blank.pdf"));
  EXPECT_EQ(0, EPDFDoc_GetPageLabelSectionCount(document()));
  EXPECT_EQ(std::vector<std::wstring>{L"(none)"}, LabelsOf(document()));
}

// A tree of two leaves, the way FPDF_GetPageLabel() reads it: the pages before
// the first entry and the entry that isn't a dictionary number by their
// place, and an entry past the last page is left out.
TEST_F(EPDFPageLabelsEmbedderTest, ReadsATreeAsPageLabelsDo) {
  embedpdf_test::ScopedTestDocument document = NewDocument(7);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document.get());
  auto labels = doc->GetMutableRoot()->SetNewFor<CPDF_Dictionary>("PageLabels");
  auto kids = labels->SetNewFor<CPDF_Array>("Kids");

  auto front = doc->NewIndirect<CPDF_Dictionary>();
  auto front_limits = front->SetNewFor<CPDF_Array>("Limits");
  front_limits->AppendNew<CPDF_Number>(2);
  front_limits->AppendNew<CPDF_Number>(3);
  auto front_nums = front->SetNewFor<CPDF_Array>("Nums");
  front_nums->AppendNew<CPDF_Number>(2);
  front_nums->AppendNew<CPDF_Dictionary>()->SetNewFor<CPDF_Name>("S", "r");
  front_nums->AppendNew<CPDF_Number>(3);
  front_nums->AppendNew<CPDF_Number>(5);
  kids->AppendNew<CPDF_Reference>(doc, front->GetObjNum());

  auto back = doc->NewIndirect<CPDF_Dictionary>();
  auto back_limits = back->SetNewFor<CPDF_Array>("Limits");
  back_limits->AppendNew<CPDF_Number>(5);
  back_limits->AppendNew<CPDF_Number>(9);
  auto back_nums = back->SetNewFor<CPDF_Array>("Nums");
  back_nums->AppendNew<CPDF_Number>(5);
  auto appendix = back_nums->AppendNew<CPDF_Dictionary>();
  appendix->SetNewFor<CPDF_Name>("S", "A");
  appendix->SetNewFor<CPDF_String>("P", "x-");
  appendix->SetNewFor<CPDF_Number>("St", 3);
  back_nums->AppendNew<CPDF_Number>(9);
  back_nums->AppendNew<CPDF_Dictionary>()->SetNewFor<CPDF_Name>("S", "D");
  kids->AppendNew<CPDF_Reference>(doc, back->GetObjNum());

  const std::vector<Section> expected = {
      {0, EPDF_PAGE_LABEL_STYLE_DECIMAL, 1, L""},
      {2, EPDF_PAGE_LABEL_STYLE_LOWER_ROMAN, 1, L""},
      {3, EPDF_PAGE_LABEL_STYLE_DECIMAL, 4, L""},
      {5, EPDF_PAGE_LABEL_STYLE_UPPER_LETTERS, 3, L"x-"},
  };
  EXPECT_EQ(expected, SectionsOf(document.get()));
  const std::vector<std::wstring> labels_read = {L"1", L"2",   L"i",  L"4",
                                                 L"5", L"x-C", L"x-D"};
  EXPECT_EQ(labels_read, LabelsOf(document.get()));

  // Written back as they read, the pages keep every label.
  ASSERT_TRUE(WriteSections(document.get(), expected));
  EXPECT_EQ(expected, SectionsOf(document.get()));
  EXPECT_EQ(labels_read, LabelsOf(document.get()));
}

TEST_F(EPDFPageLabelsEmbedderTest, WritesSectionsThatLabelThePages) {
  embedpdf_test::ScopedTestDocument document = NewDocument(6);
  const std::vector<Section> sections = {
      {0, EPDF_PAGE_LABEL_STYLE_LOWER_ROMAN, 1, L""},
      {2, EPDF_PAGE_LABEL_STYLE_DECIMAL, 1, L""},
      {4, EPDF_PAGE_LABEL_STYLE_DECIMAL, 7, L"B-\u00e9"},
  };
  ASSERT_TRUE(WriteSections(document.get(), sections));
  EXPECT_EQ(sections, SectionsOf(document.get()));
  const std::vector<std::wstring> labels = {L"i", L"ii",        L"1",
                                            L"2", L"B-\u00e97", L"B-\u00e98"};
  EXPECT_EQ(labels, LabelsOf(document.get()));

  // /S, /P and /St only when they say something.
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document.get());
  RetainPtr<const CPDF_Array> nums =
      doc->GetRoot()->GetDictFor("PageLabels")->GetArrayFor("Nums");
  ASSERT_EQ(6u, nums->size());
  EXPECT_FALSE(nums->GetDictAt(1)->KeyExist("St"));
  EXPECT_FALSE(nums->GetDictAt(1)->KeyExist("P"));
  EXPECT_EQ(7, nums->GetDictAt(5)->GetIntegerFor("St"));

  embedpdf_test::ScopedTestDocument reopened = SaveAndReopen(document.get());
  ASSERT_TRUE(reopened);
  EXPECT_EQ(sections, SectionsOf(reopened.get()));
  EXPECT_EQ(labels, LabelsOf(reopened.get()));
}

TEST_F(EPDFPageLabelsEmbedderTest, NoSectionsRemovesTheLabels) {
  ASSERT_TRUE(OpenDocument("page_labels.pdf"));
  SetWholeFileAvailable();
  ASSERT_TRUE(EPDFDoc_SetPageLabelSections(document(), nullptr, nullptr,
                                           nullptr, nullptr, 0));
  EXPECT_EQ(0, EPDFDoc_GetPageLabelSectionCount(document()));
  EXPECT_EQ(std::vector<std::wstring>(7, L"(none)"), LabelsOf(document()));
  // Removing none is fine too.
  EXPECT_TRUE(EPDFDoc_SetPageLabelSections(document(), nullptr, nullptr,
                                           nullptr, nullptr, 0));
}

TEST_F(EPDFPageLabelsEmbedderTest, RefusesSectionsThatBreakTheRules) {
  ASSERT_TRUE(OpenDocument("page_labels.pdf"));
  SetWholeFileAvailable();
  const int d = EPDF_PAGE_LABEL_STYLE_DECIMAL;
  // Not starting at page 0, not in order, past the last page, a style that
  // isn't one.
  EXPECT_FALSE(WriteSections(document(), {{1, d, 1, L""}}));
  EXPECT_FALSE(WriteSections(document(),
                             {{0, d, 1, L""}, {3, d, 1, L""}, {3, d, 1, L""}}));
  EXPECT_FALSE(WriteSections(document(),
                             {{0, d, 1, L""}, {2, d, 1, L""}, {1, d, 1, L""}}));
  EXPECT_FALSE(WriteSections(document(), {{0, d, 1, L""}, {7, d, 1, L""}}));
  EXPECT_FALSE(WriteSections(document(), {{0, 6, 1, L""}}));
  EXPECT_FALSE(WriteSections(document(), {{0, -1, 1, L""}}));
  const int zero = 0;
  EXPECT_FALSE(EPDFDoc_SetPageLabelSections(document(), &zero, nullptr, &zero,
                                            nullptr, 1));
  EXPECT_FALSE(WriteSections(nullptr, {{0, d, 1, L""}}));
  // Nothing was written.
  EXPECT_EQ(kFileSections, SectionsOf(document()));
  EXPECT_EQ(kFileLabels, LabelsOf(document()));

  // Without prefixes at all, and with a first number below 1, as written.
  const int first_pages[] = {0, 5};
  const int styles[] = {d, EPDF_PAGE_LABEL_STYLE_NONE};
  const int first_numbers[] = {0, 1};
  ASSERT_TRUE(EPDFDoc_SetPageLabelSections(document(), first_pages, styles,
                                           first_numbers, nullptr, 2));
  const std::vector<Section> written = {
      {0, d, 0, L""}, {5, EPDF_PAGE_LABEL_STYLE_NONE, 1, L""}};
  EXPECT_EQ(written, SectionsOf(document()));
}

// On a layer, inside a transaction: /PageLabels keeps its object, an abort
// leaves the sections as they were, and the layer's artifact keeps a commit.
TEST_F(EPDFPageLabelsEmbedderTest, WritesInALayerTransaction) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  const uint32_t labels_object = PageLabelsObject(layer.get());
  ASSERT_NE(0u, labels_object);
  EXPECT_EQ(kFileSections, SectionsOf(layer.get()));

  const std::vector<Section> moved = {
      {0, EPDF_PAGE_LABEL_STYLE_LOWER_ROMAN, 1, L""},
      {1, EPDF_PAGE_LABEL_STYLE_DECIMAL, 1, L""},
      {5, EPDF_PAGE_LABEL_STYLE_UPPER_LETTERS, 1, L"zz"},
  };
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(WriteSections(layer.get(), moved));
  EXPECT_EQ(moved, SectionsOf(layer.get()));
  ASSERT_TRUE(EPDFLayer_AbortTransaction(layer.get()));
  EXPECT_EQ(kFileSections, SectionsOf(layer.get()));
  EXPECT_EQ(kFileLabels, LabelsOf(layer.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(WriteSections(layer.get(), moved));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_EQ(labels_object, PageLabelsObject(layer.get()));
  const std::vector<std::wstring> labels = {L"i", L"1",   L"2",  L"3",
                                            L"4", L"zzA", L"zzB"};
  EXPECT_EQ(labels, LabelsOf(layer.get()));

  ScopedFPDFDocument reopened = SaveAndReopenArtifact(layer.get());
  ASSERT_TRUE(reopened);
  EXPECT_EQ(moved, SectionsOf(reopened.get()));
  EXPECT_EQ(labels, LabelsOf(reopened.get()));
}
