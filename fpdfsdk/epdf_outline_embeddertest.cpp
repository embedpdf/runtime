// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// The document outline's writers (public/epdf_outline.h) and its captures
// (EPDFOutline_ExportItemRawToOwnedBuffer and EPDFOutline_ImportItemRaw):
// items named by number, lists and counts kept right, refusals that change
// nothing, and layers.

#include <stdint.h>

#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fxcrt/compiler_specific.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_capture.h"
#include "public/epdf_doc.h"
#include "public/epdf_edit.h"
#include "public/epdf_layer.h"
#include "public/epdf_outline.h"
#include "public/epdf_save.h"
#include "public/fpdf_doc.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

// The objects of the document below.
constexpr uint32_t kFirstPage = 3;
constexpr uint32_t kOutlines = 5;
constexpr uint32_t kA = 6;
constexpr uint32_t kB = 7;
constexpr uint32_t kC = 8;
constexpr uint32_t kA1 = 9;
constexpr uint32_t kC1 = 10;

// Two pages and an outline: A (open, its child A1, a destination), B (red,
// bold) and C (closed, its child C1). Four items show when it opens.
std::string MakeDocument() {
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R /Outlines 5 0 R >>",
      "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
      "<< /Type /Outlines /First 6 0 R /Last 8 0 R /Count 4 >>",
      "<< /Title (A) /Parent 5 0 R /Next 7 0 R /First 9 0 R /Last 9 0 R "
      "/Count 1 /Dest [3 0 R /Fit] >>",
      "<< /Title (B) /Parent 5 0 R /Prev 6 0 R /Next 8 0 R /C [1 0 0] /F 2 >>",
      "<< /Title (C) /Parent 5 0 R /Prev 7 0 R /First 10 0 R /Last 10 0 R "
      "/Count -1 >>",
      "<< /Title (A1) /Parent 6 0 R >>",
      "<< /Title (C1) /Parent 8 0 R >>",
  });
}

// A document with |outline| as object 3 and the items after it, its page 2.
std::string MakeDocumentWith(const std::vector<std::string>& outline) {
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R /Outlines 3 0 R >>",
      "<< /Type /Pages /Kids [4 0 R] /Count 1 >>",
  };
  objects.insert(objects.end(), outline.begin(), outline.end());
  return MakePdf(objects);
}

RetainPtr<const CPDF_Dictionary> DictOf(FPDF_DOCUMENT doc, uint32_t objnum) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  CPDF_DocumentViewScope view(pdf);
  return ToDictionary(pdf->GetOrParseIndirectObject(objnum));
}

uint32_t RefOf(const CPDF_Dictionary* dict, const char* key) {
  RetainPtr<const CPDF_Object> value = dict ? dict->GetObjectFor(key) : nullptr;
  const CPDF_Reference* ref = ToReference(value.Get());
  return ref ? ref->GetRefObjNum() : 0;
}

uint32_t OutlinesOf(FPDF_DOCUMENT doc) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  CPDF_DocumentViewScope view(pdf);
  return RefOf(pdf->GetRoot(), "Outlines");
}

std::string TitleOf(FPDF_BOOKMARK bookmark) {
  unsigned long length = FPDFBookmark_GetTitle(bookmark, nullptr, 0);
  std::vector<FPDF_WCHAR> buffer(length / sizeof(FPDF_WCHAR) + 1);
  FPDFBookmark_GetTitle(bookmark, buffer.data(), length);
  return GetPlatformString(buffer.data());
}

std::string TitleOf(FPDF_DOCUMENT doc, uint32_t objnum) {
  FPDF_BOOKMARK item = EPDFOutline_GetItem(doc, objnum);
  return item ? TitleOf(item) : "(none)";
}

// The outline as the readers of fpdf_doc.h see it: each title, its /Count
// when it has one, and its children in brackets; the outline's own /Count
// first. Only for outlines without loops: the readers don't stop at them.
std::string Tree(FPDF_DOCUMENT doc, FPDF_BOOKMARK parent = nullptr) {
  std::string out;
  if (!parent) {
    RetainPtr<const CPDF_Dictionary> outlines = DictOf(doc, OutlinesOf(doc));
    out = outlines && outlines->KeyExist("Count")
              ? std::to_string(outlines->GetIntegerFor("Count")) + ":"
              : "-:";
  }
  bool first = true;
  for (FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, parent); child;
       child = FPDFBookmark_GetNextSibling(doc, child)) {
    out += first ? "" : " ";
    first = false;
    out += TitleOf(child);
    if (DictOf(doc, EPDFOutline_GetObjectNumber(child))->KeyExist("Count")) {
      const int count = FPDFBookmark_GetCount(child);
      out += (count > 0 ? "+" : "") + std::to_string(count);
    }
    const std::string under = Tree(doc, child);
    if (!under.empty()) {
      out += "[" + under + "]";
    }
  }
  return out;
}

// The numbers of every item, depth first, through the readers.
std::vector<uint32_t> Items(FPDF_DOCUMENT doc, FPDF_BOOKMARK parent = nullptr) {
  std::vector<uint32_t> items;
  for (FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, parent); child;
       child = FPDFBookmark_GetNextSibling(doc, child)) {
    items.push_back(EPDFOutline_GetObjectNumber(child));
    for (uint32_t under : Items(doc, child)) {
      items.push_back(under);
    }
  }
  return items;
}

// What is wrong with the outline's links and counts, worked out here from
// the dictionaries and not by the code under test; empty when nothing is.
class OutlineChecker {
 public:
  explicit OutlineChecker(FPDF_DOCUMENT doc)
      : pdf_(CPDFDocumentFromFPDFDocument(doc)) {}

  std::string Problems() {
    CPDF_DocumentViewScope view(pdf_);
    const uint32_t root = RefOf(pdf_->GetRoot(), "Outlines");
    RetainPtr<const CPDF_Dictionary> outlines = Dict(root);
    if (!outlines) {
      return problems_;
    }
    const int visible = VisibleUnder(outlines.Get(), root);
    if (visible ? outlines->GetIntegerFor("Count") != visible
                : outlines->KeyExist("Count")) {
      Report(root, "/Count " +
                       std::to_string(outlines->GetIntegerFor("Count")) + ", " +
                       std::to_string(visible) + " visible");
    }
    return problems_;
  }

 private:
  RetainPtr<const CPDF_Dictionary> Dict(uint32_t objnum) {
    return ToDictionary(pdf_->GetOrParseIndirectObject(objnum));
  }

  void Report(uint32_t objnum, const std::string& what) {
    problems_ += std::to_string(objnum) + ": " + what + "; ";
  }

  void ExpectRef(uint32_t objnum,
                 const CPDF_Dictionary* dict,
                 const char* key,
                 uint32_t expected) {
    const uint32_t actual = RefOf(dict, key);
    if (actual != expected || (!expected && dict->KeyExist(key))) {
      Report(objnum, std::string("/") + key + " " + std::to_string(actual) +
                         ", expected " + std::to_string(expected));
    }
  }

  // Checks |node|'s list and everything under it; returns how many items
  // show under |node| when it is open.
  int VisibleUnder(const CPDF_Dictionary* node, uint32_t objnum) {
    std::vector<uint32_t> numbers;
    std::vector<RetainPtr<const CPDF_Dictionary>> kids;
    RetainPtr<const CPDF_Object> link = node->GetObjectFor("First");
    while (link) {
      const CPDF_Reference* ref = ToReference(link.Get());
      if (!ref) {
        Report(objnum, "a child that isn't indirect");
        break;
      }
      if (!seen_.insert(ref->GetRefObjNum()).second) {
        Report(ref->GetRefObjNum(), "reached twice");
        break;
      }
      numbers.push_back(ref->GetRefObjNum());
      kids.push_back(Dict(ref->GetRefObjNum()));
      link = kids.back()->GetObjectFor("Next");
    }
    ExpectRef(objnum, node, "First", numbers.empty() ? 0 : numbers.front());
    ExpectRef(objnum, node, "Last", numbers.empty() ? 0 : numbers.back());
    int visible = 0;
    for (size_t i = 0; i < kids.size(); ++i) {
      const CPDF_Dictionary* kid = kids[i].Get();
      ExpectRef(numbers[i], kid, "Parent", objnum);
      ExpectRef(numbers[i], kid, "Prev", i ? numbers[i - 1] : 0);
      ExpectRef(numbers[i], kid, "Next",
                i + 1 < kids.size() ? numbers[i + 1] : 0);
      if (!kid->KeyExist("Title")) {
        Report(numbers[i], "no /Title");
      }
      const int under = VisibleUnder(kid, numbers[i]);
      const int count = kid->GetIntegerFor("Count");
      if (under == 0 && kid->KeyExist("Count")) {
        Report(numbers[i], "/Count without children");
      } else if (under > 0 && count != under && count != -under) {
        Report(numbers[i], "/Count " + std::to_string(count) + ", " +
                               std::to_string(under) + " under it");
      }
      visible += 1 + (count > 0 ? under : 0);
    }
    return visible;
  }

  CPDF_Document* const pdf_;
  std::set<uint32_t> seen_;
  std::string problems_;
};

std::string Problems(FPDF_DOCUMENT doc) {
  return OutlineChecker(doc).Problems();
}

// A GoTo action of |doc| to the page |page| (an object number).
FPDF_ACTION GoTo(FPDF_DOCUMENT doc, uint32_t page) {
  FPDF_DEST dest =
      EPDFDest_CreateXYZByObjectNumber(doc, page, true, 10, true, 20, false, 0);
  return dest ? EPDFAction_CreateGoTo(doc, dest) : nullptr;
}

uint32_t Create(FPDF_DOCUMENT doc,
                uint32_t parent,
                uint32_t after,
                const wchar_t* title,
                uint32_t objnum = 0) {
  ScopedFPDFWideString text = GetFPDFWideString(title);
  return EPDFOutline_CreateItem(doc, objnum, parent, after, text.get());
}

std::vector<uint8_t> Take(void* buffer, unsigned long size) {
  std::vector<uint8_t> bytes;
  if (buffer) {
    const uint8_t* data = static_cast<const uint8_t*>(buffer);
    // SAFETY: the export reported |size| bytes.
    bytes.assign(data, UNSAFE_BUFFERS(data + size));
    EPDF_FreeBuffer(buffer);
  }
  return bytes;
}

std::vector<uint8_t> ExportItem(FPDF_DOCUMENT doc, uint32_t objnum) {
  unsigned long size = 0;
  void* buffer = EPDFOutline_ExportItemRawToOwnedBuffer(doc, objnum, &size);
  return Take(buffer, size);
}

bool ImportItem(FPDF_DOCUMENT doc, const std::vector<uint8_t>& capture) {
  return EPDFOutline_ImportItemRaw(doc, capture.data(),
                                   static_cast<unsigned long>(capture.size()));
}

}  // namespace

class EPDFOutlineEmbedderTest : public EmbedderTest {
 protected:
  void TearDown() override {
    if (base_) {
      EPDF_ReleaseBaseDocument(base_);
    }
    EmbedderTest::TearDown();
  }

  // A plain document of |bytes|.
  ScopedFPDFDocument OpenPlain(std::string bytes = MakeDocument()) {
    plain_ = std::move(bytes);
    return ScopedFPDFDocument(
        FPDF_LoadMemDocument(plain_.data(), plain_.size(), nullptr));
  }

  // A layer over a base of |bytes|, with numbers up to 40 handed out.
  ScopedFPDFDocument OpenLayer(std::string bytes = MakeDocument()) {
    input_ = std::move(bytes);
    base_ = EPDF_LoadMemBaseDocument(input_.data(), input_.size(), nullptr);
    if (!base_) {
      return nullptr;
    }
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base_, nullptr, nullptr, nullptr));
    if (layer && !EPDFLayer_RaiseLastObjectNumber(layer.get(), 40)) {
      return nullptr;
    }
    return layer;
  }

  // The layer saved and opened again: what nothing reaches is gone.
  ScopedFPDFDocument Reopen(FPDF_DOCUMENT layer) {
    ClearString();
    EXPECT_TRUE(EPDFLayer_SaveLayerArtifact(layer, this, nullptr));
    artifact_ = GetString();
    FPDF_FILEACCESS access = {};
    access.m_FileLen = artifact_.size();
    access.m_GetBlock = GetBlockFromString;
    access.m_Param = &artifact_;
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument reopened(
        EPDFLayer_OpenLayerArtifact(base_, &access, nullptr, &status));
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    return reopened;
  }

  void RandomWrites(FPDF_DOCUMENT doc, bool layer);

  std::string plain_;
  std::string input_;
  std::string artifact_;
  EPDF_BASE_DOCUMENT base_ = nullptr;
  // The next number a layer write creates an item at (OpenLayer() hands out
  // numbers up to 40).
  uint32_t next_number = 41;
};

TEST_F(EPDFOutlineEmbedderTest, ItemsAreNamedByNumber) {
  ScopedFPDFDocument doc = OpenPlain();
  ASSERT_TRUE(doc);
  EXPECT_EQ("4:A+1[A1] B C-1[C1]", Tree(doc.get()));
  EXPECT_EQ("", Problems(doc.get()));
  FPDF_BOOKMARK first = FPDFBookmark_GetFirstChild(doc.get(), nullptr);
  EXPECT_EQ(kA, EPDFOutline_GetObjectNumber(first));
  EXPECT_EQ(0u, EPDFOutline_GetObjectNumber(nullptr));
  EXPECT_EQ("C1", TitleOf(doc.get(), kC1));
  EXPECT_EQ(first, EPDFOutline_GetItem(doc.get(), kA));
  // The outline itself, a page and a number nothing has are no items.
  EXPECT_FALSE(EPDFOutline_GetItem(doc.get(), kOutlines));
  EXPECT_FALSE(EPDFOutline_GetItem(doc.get(), kFirstPage));
  EXPECT_FALSE(EPDFOutline_GetItem(doc.get(), 99));
  EXPECT_FALSE(EPDFOutline_GetItem(doc.get(), 0));
  EXPECT_FALSE(EPDFOutline_GetItem(nullptr, kA));
}

TEST_F(EPDFOutlineEmbedderTest, CreatePlacesTheItemAndCounts) {
  ScopedFPDFDocument doc = OpenPlain();
  ASSERT_TRUE(doc);
  const uint32_t top = Create(doc.get(), 0, 0, L"Top");
  ASSERT_TRUE(top);
  EXPECT_EQ("5:Top A+1[A1] B C-1[C1]", Tree(doc.get()));
  ASSERT_TRUE(Create(doc.get(), 0, kB, L"AfterB"));
  ASSERT_TRUE(Create(doc.get(), 0, kC, L"End"));
  EXPECT_EQ("7:Top A+1[A1] B AfterB C-1[C1] End", Tree(doc.get()));

  // Under an open item the outline shows one more; under a closed one, none.
  ASSERT_TRUE(Create(doc.get(), kA, kA1, L"A2"));
  ASSERT_TRUE(Create(doc.get(), kC, 0, L"C0"));
  EXPECT_EQ("8:Top A+2[A1 A2] B AfterB C-2[C0 C1] End", Tree(doc.get()));

  // A leaf that gets a child starts closed.
  ASSERT_TRUE(Create(doc.get(), kB, 0, L"B1"));
  EXPECT_EQ("8:Top A+2[A1 A2] B-1[B1] AfterB C-2[C0 C1] End", Tree(doc.get()));
  EXPECT_EQ("", Problems(doc.get()));

  // No title is an empty one: /Title is always written.
  const uint32_t untitled = EPDFOutline_CreateItem(doc.get(), 0, 0, 0, nullptr);
  ASSERT_TRUE(untitled);
  EXPECT_TRUE(DictOf(doc.get(), untitled)->KeyExist("Title"));
  EXPECT_EQ("", TitleOf(doc.get(), untitled));
  EXPECT_EQ("", Problems(doc.get()));
}

TEST_F(EPDFOutlineEmbedderTest, CreateRefusesAndChangesNothing) {
  ScopedFPDFDocument doc = OpenPlain();
  ASSERT_TRUE(doc);
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());
  const std::map<uint32_t, std::string> before = ObjectGraph(pdf);
  EXPECT_FALSE(Create(doc.get(), kFirstPage, 0, L"x"));  // a page
  EXPECT_FALSE(Create(doc.get(), 99, 0, L"x"));          // nothing
  EXPECT_FALSE(Create(doc.get(), kOutlines, 0, L"x"));   // the outline itself
  EXPECT_FALSE(Create(doc.get(), 0, kA1, L"x"));         // not a top child
  EXPECT_FALSE(Create(doc.get(), kA, kB, L"x"));         // not a child of A
  EXPECT_FALSE(Create(doc.get(), 0, 0, L"x", 30));       // a number, no layer
  EXPECT_EQ(before, ObjectGraph(pdf));
}

TEST_F(EPDFOutlineEmbedderTest, CreateAtANumberOnALayer) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  EXPECT_FALSE(Create(layer.get(), 0, 0, L"x", 30));  // outside a transaction
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  EXPECT_EQ(30u, Create(layer.get(), 0, kC, L"Thirty", 30));
  EXPECT_FALSE(Create(layer.get(), 0, 0, L"again", 30));  // taken
  EXPECT_FALSE(Create(layer.get(), 0, 0, L"base", kB));   // the file's
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_EQ("5:A+1[A1] B C-1[C1] Thirty", Tree(layer.get()));
  EXPECT_EQ("", Problems(layer.get()));
}

TEST_F(EPDFOutlineEmbedderTest, CreateMakesTheOutline) {
  ScopedFPDFDocument doc = OpenPlain(MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
  }));
  ASSERT_TRUE(doc);
  EXPECT_FALSE(Create(doc.get(), 4, 0, L"x"));  // no outline, no items
  EXPECT_FALSE(OutlinesOf(doc.get()));
  const uint32_t first = Create(doc.get(), 0, 0, L"First");
  ASSERT_TRUE(first);
  const uint32_t outlines = OutlinesOf(doc.get());
  ASSERT_TRUE(outlines);
  EXPECT_EQ("Outlines", DictOf(doc.get(), outlines)->GetNameFor("Type"));
  EXPECT_EQ("1:First", Tree(doc.get()));
  EXPECT_EQ("", Problems(doc.get()));
}

TEST_F(EPDFOutlineEmbedderTest, MoveKeepsNumbersKeysAndState) {
  ScopedFPDFDocument doc = OpenPlain();
  ASSERT_TRUE(doc);
  // C goes under A, closed, with its child and its keys.
  ASSERT_TRUE(EPDFOutline_MoveItem(doc.get(), kC, kA, kA1));
  EXPECT_EQ("4:A+2[A1 C-1[C1]] B", Tree(doc.get()));
  EXPECT_EQ(kC, RefOf(DictOf(doc.get(), kC1).Get(), "Parent"));
  EXPECT_EQ("", Problems(doc.get()));
  // To the top, first; then along its own list.
  ASSERT_TRUE(EPDFOutline_MoveItem(doc.get(), kC, 0, 0));
  EXPECT_EQ("4:C-1[C1] A+1[A1] B", Tree(doc.get()));
  ASSERT_TRUE(EPDFOutline_MoveItem(doc.get(), kC, 0, kB));
  EXPECT_EQ("4:A+1[A1] B C-1[C1]", Tree(doc.get()));
  ASSERT_TRUE(EPDFOutline_MoveItem(doc.get(), kA, 0, kB));
  EXPECT_EQ("4:B A+1[A1] C-1[C1]", Tree(doc.get()));
  // A's subtree under B, which was a leaf: B starts closed.
  ASSERT_TRUE(EPDFOutline_MoveItem(doc.get(), kA, kB, 0));
  EXPECT_EQ("2:B-2[A+1[A1]] C-1[C1]", Tree(doc.get()));
  // A last child leaving takes its parent's /Count with it.
  ASSERT_TRUE(EPDFOutline_MoveItem(doc.get(), kC1, 0, 0));
  EXPECT_EQ("3:C1 B-2[A+1[A1]] C", Tree(doc.get()));
  EXPECT_EQ("", Problems(doc.get()));
}

TEST_F(EPDFOutlineEmbedderTest, MoveRefusesAndChangesNothing) {
  ScopedFPDFDocument doc = OpenPlain();
  ASSERT_TRUE(doc);
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());
  const std::map<uint32_t, std::string> before = ObjectGraph(pdf);
  EXPECT_FALSE(EPDFOutline_MoveItem(doc.get(), kA, kA, 0));   // itself
  EXPECT_FALSE(EPDFOutline_MoveItem(doc.get(), kA, kA1, 0));  // under it
  EXPECT_FALSE(EPDFOutline_MoveItem(doc.get(), kA, 0, kA));   // after itself
  EXPECT_FALSE(EPDFOutline_MoveItem(doc.get(), kA, 0, kA1));  // not a top child
  EXPECT_FALSE(EPDFOutline_MoveItem(doc.get(), kA, kFirstPage, 0));
  EXPECT_FALSE(EPDFOutline_MoveItem(doc.get(), kOutlines, 0, 0));
  EXPECT_FALSE(EPDFOutline_MoveItem(doc.get(), 99, 0, 0));
  EXPECT_EQ(before, ObjectGraph(pdf));
}

TEST_F(EPDFOutlineEmbedderTest, DeleteTakesTheItemsUnderIt) {
  ScopedFPDFDocument doc = OpenPlain();
  ASSERT_TRUE(doc);
  ASSERT_TRUE(EPDFOutline_DeleteItem(doc.get(), kA));
  EXPECT_EQ("2:B C-1[C1]", Tree(doc.get()));
  // The file's dictionaries can still be read; they are no items.
  EXPECT_FALSE(EPDFOutline_GetItem(doc.get(), kA));
  EXPECT_FALSE(EPDFOutline_GetItem(doc.get(), kA1));
  EXPECT_FALSE(EPDFOutline_DeleteItem(doc.get(), kA));
  ASSERT_TRUE(EPDFOutline_DeleteItem(doc.get(), kC1));
  EXPECT_EQ("2:B C", Tree(doc.get()));
  EXPECT_EQ("", Problems(doc.get()));
  // The last ones: the outline stays, empty.
  ASSERT_TRUE(EPDFOutline_DeleteItem(doc.get(), kB));
  ASSERT_TRUE(EPDFOutline_DeleteItem(doc.get(), kC));
  RetainPtr<const CPDF_Dictionary> outlines = DictOf(doc.get(), kOutlines);
  ASSERT_TRUE(outlines);
  EXPECT_FALSE(outlines->KeyExist("First"));
  EXPECT_FALSE(outlines->KeyExist("Last"));
  EXPECT_FALSE(outlines->KeyExist("Count"));
  EXPECT_EQ(kOutlines, OutlinesOf(doc.get()));
  ASSERT_TRUE(Create(doc.get(), 0, 0, L"Again"));
  EXPECT_EQ("1:Again", Tree(doc.get()));
}

TEST_F(EPDFOutlineEmbedderTest, SetOpenFlipsTheSignAndCountsAgain) {
  ScopedFPDFDocument doc = OpenPlain();
  ASSERT_TRUE(doc);
  ASSERT_TRUE(EPDFOutline_SetOpen(doc.get(), kC, true));
  EXPECT_EQ("5:A+1[A1] B C+1[C1]", Tree(doc.get()));
  ASSERT_TRUE(EPDFOutline_SetOpen(doc.get(), kA, false));
  EXPECT_EQ("4:A-1[A1] B C+1[C1]", Tree(doc.get()));
  // A leaf has no open state: nothing is written.
  ASSERT_TRUE(EPDFOutline_SetOpen(doc.get(), kB, true));
  EXPECT_FALSE(DictOf(doc.get(), kB)->KeyExist("Count"));
  EXPECT_FALSE(EPDFOutline_SetOpen(doc.get(), 99, true));
  EXPECT_EQ("", Problems(doc.get()));
}

TEST_F(EPDFOutlineEmbedderTest, TitleColorAndStyle) {
  ScopedFPDFDocument doc = OpenPlain();
  ASSERT_TRUE(doc);
  ScopedFPDFWideString renamed = GetFPDFWideString(L"Ölçü");
  ASSERT_TRUE(EPDFOutline_SetTitle(doc.get(), kA1, renamed.get()));
  EXPECT_EQ("Ölçü", TitleOf(doc.get(), kA1));
  EXPECT_FALSE(EPDFOutline_SetTitle(doc.get(), kFirstPage, renamed.get()));

  FPDF_BOOKMARK a = EPDFOutline_GetItem(doc.get(), kA);
  float r = 0;
  float g = 0;
  float b = 0;
  EXPECT_FALSE(FPDFBookmark_GetColor(a, &r, &g, &b));
  ASSERT_TRUE(EPDFOutline_SetColor(doc.get(), kA, true, 0, 0.5f, 1));
  a = EPDFOutline_GetItem(doc.get(), kA);
  ASSERT_TRUE(FPDFBookmark_GetColor(a, &r, &g, &b));
  EXPECT_FLOAT_EQ(0, r);
  EXPECT_FLOAT_EQ(0.5f, g);
  EXPECT_FLOAT_EQ(1, b);
  EXPECT_FALSE(EPDFOutline_SetColor(doc.get(), kA, true, 0, 2, 0));
  EXPECT_FALSE(EPDFOutline_SetColor(doc.get(), kA, true, -1, 0, 0));
  ASSERT_TRUE(EPDFOutline_SetColor(doc.get(), kB, false, 0, 0, 0));
  EXPECT_FALSE(DictOf(doc.get(), kB)->KeyExist("C"));

  ASSERT_TRUE(EPDFOutline_SetStyle(doc.get(), kA, 3));
  EXPECT_EQ(3, FPDFBookmark_GetStyle(EPDFOutline_GetItem(doc.get(), kA)));
  ASSERT_TRUE(EPDFOutline_SetStyle(doc.get(), kB, 0));
  EXPECT_FALSE(DictOf(doc.get(), kB)->KeyExist("F"));
  EXPECT_FALSE(EPDFOutline_SetStyle(doc.get(), kA, -1));
}

TEST_F(EPDFOutlineEmbedderTest, SetActionReplacesTheDestination) {
  ScopedFPDFDocument doc = OpenPlain();
  ASSERT_TRUE(doc);
  ASSERT_TRUE(DictOf(doc.get(), kA)->KeyExist("Dest"));
  FPDF_ACTION action = GoTo(doc.get(), kFirstPage);
  ASSERT_TRUE(action);
  ASSERT_TRUE(EPDFOutline_SetAction(doc.get(), kA, action));
  RetainPtr<const CPDF_Dictionary> a = DictOf(doc.get(), kA);
  EXPECT_FALSE(a->KeyExist("Dest"));
  EXPECT_TRUE(RefOf(a.Get(), "A"));
  EXPECT_TRUE(FPDFBookmark_GetAction(EPDFOutline_GetItem(doc.get(), kA)));

  // Another document's action is refused, also when its number names an
  // object here (the action set above); none removes the target.
  ScopedFPDFDocument other(
      FPDF_LoadMemDocument(plain_.data(), plain_.size(), nullptr));
  ASSERT_TRUE(other);
  FPDF_ACTION foreign = GoTo(other.get(), kFirstPage);
  ASSERT_TRUE(foreign);
  ASSERT_TRUE(
      DictOf(doc.get(), CPDFDictionaryFromFPDFAction(foreign)->GetObjNum()));
  EXPECT_FALSE(EPDFOutline_SetAction(doc.get(), kB, foreign));
  ASSERT_TRUE(EPDFOutline_SetAction(doc.get(), kA, nullptr));
  a = DictOf(doc.get(), kA);
  EXPECT_FALSE(a->KeyExist("A"));
  EXPECT_FALSE(a->KeyExist("Dest"));
}

// A new target, or none, replaces the item's own /A: an action object it
// shares with another item is never written, on a plain document or a layer.
TEST_F(EPDFOutlineEmbedderTest, SetActionLeavesASharedActionAlone) {
  const std::string bytes = MakeDocumentWith({
      "<< /Type /Outlines /First 5 0 R /Last 6 0 R /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
      "<< /Title (One) /Parent 3 0 R /Next 6 0 R /A 7 0 R >>",
      "<< /Title (Two) /Parent 3 0 R /Prev 5 0 R /A 7 0 R >>",
      "<< /S /GoTo /D [4 0 R /Fit] >>",
  });
  {
    ScopedFPDFDocument doc = OpenPlain(bytes);
    ASSERT_TRUE(doc);
    CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());
    const std::string shared = ObjectGraph(pdf).at(7);
    ASSERT_TRUE(EPDFOutline_SetAction(doc.get(), 5, GoTo(doc.get(), 4)));
    const uint32_t replaced = RefOf(DictOf(doc.get(), 5).Get(), "A");
    EXPECT_TRUE(replaced);
    EXPECT_NE(7u, replaced);
    EXPECT_EQ(7u, RefOf(DictOf(doc.get(), 6).Get(), "A"));
    ASSERT_TRUE(EPDFOutline_SetAction(doc.get(), 6, nullptr));
    EXPECT_FALSE(DictOf(doc.get(), 6)->KeyExist("A"));
    EXPECT_EQ(shared, ObjectGraph(pdf).at(7));
  }
  ScopedFPDFDocument layer = OpenLayer(bytes);
  ASSERT_TRUE(layer);
  CPDF_LayerDocument* pdf = CPDF_LayerDocument::FromDocument(
      CPDFDocumentFromFPDFDocument(layer.get()));
  ASSERT_TRUE(EPDFOutline_SetAction(layer.get(), 5, GoTo(layer.get(), 4)));
  EXPECT_TRUE(pdf->FindLayerVersion(5));
  EXPECT_FALSE(pdf->FindLayerVersion(6));
  EXPECT_FALSE(pdf->FindLayerVersion(7));
  EXPECT_EQ(7u, RefOf(DictOf(layer.get(), 6).Get(), "A"));
}

// A /Next that comes back ends the list; the next write of that list
// straightens it.
TEST_F(EPDFOutlineEmbedderTest, ALoopEndsTheList) {
  ScopedFPDFDocument doc = OpenPlain(MakeDocumentWith({
      "<< /Type /Outlines /First 5 0 R /Last 6 0 R /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
      "<< /Title (One) /Parent 3 0 R /Next 6 0 R >>",
      "<< /Title (Two) /Parent 3 0 R /Prev 5 0 R /Next 5 0 R >>",
  }));
  ASSERT_TRUE(doc);
  EXPECT_TRUE(EPDFOutline_GetItem(doc.get(), 6));
  ASSERT_TRUE(Create(doc.get(), 0, 6, L"Three"));
  EXPECT_EQ("", Problems(doc.get()));
  EXPECT_EQ("3:One Two Three", Tree(doc.get()));
}

// A wrong /Last, /Prev or /Count never misleads a write. A write puts right
// the lists it changes and the counts above them, and leaves the rest.
TEST_F(EPDFOutlineEmbedderTest, WrongLinksAndCountsAreWrittenRight) {
  ScopedFPDFDocument doc = OpenPlain(MakeDocumentWith({
      "<< /Type /Outlines /First 5 0 R /Last 5 0 R /Count 9 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
      "<< /Title (One) /Parent 3 0 R /Next 6 0 R /Prev 6 0 R >>",
      "<< /Title (Two) /Parent 3 0 R /First 7 0 R /Count 5 >>",
      "<< /Title (Two.1) /Parent 6 0 R >>",
  }));
  ASSERT_TRUE(doc);
  ASSERT_TRUE(Create(doc.get(), 0, 6, L"Three"));
  // The top list and the outline's count are right, counted from the tree;
  // Two's own list wasn't written.
  EXPECT_EQ("6: /Last 0, expected 7; 6: /Count 5, 1 under it; ",
            Problems(doc.get()));
  EXPECT_EQ("4:One Two+5[Two.1] Three", Tree(doc.get()));
  ASSERT_TRUE(Create(doc.get(), 6, 7, L"Two.2"));
  EXPECT_EQ("", Problems(doc.get()));
  EXPECT_EQ("5:One Two+2[Two.1 Two.2] Three", Tree(doc.get()));
}

// A list holding an item that isn't an indirect object is never written;
// what doesn't change the list still works.
TEST_F(EPDFOutlineEmbedderTest, ADirectItemKeepsItsListAsItIs) {
  ScopedFPDFDocument doc = OpenPlain(MakeDocumentWith({
      "<< /Type /Outlines /First 5 0 R /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
      "<< /Title (One) /Parent 3 0 R /Next << /Title (Direct) >> >>",
  }));
  ASSERT_TRUE(doc);
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());
  const std::map<uint32_t, std::string> before = ObjectGraph(pdf);
  EXPECT_FALSE(Create(doc.get(), 0, 0, L"x"));
  EXPECT_FALSE(EPDFOutline_DeleteItem(doc.get(), 5));
  EXPECT_FALSE(EPDFOutline_MoveItem(doc.get(), 5, 0, 0));
  EXPECT_EQ(before, ObjectGraph(pdf));
  ScopedFPDFWideString renamed = GetFPDFWideString(L"Renamed");
  EXPECT_TRUE(EPDFOutline_SetTitle(doc.get(), 5, renamed.get()));
  EXPECT_EQ("Renamed", TitleOf(doc.get(), 5));
}

TEST_F(EPDFOutlineEmbedderTest, ADirectOutlineDictionaryIsNeverWritten) {
  ScopedFPDFDocument doc = OpenPlain(MakePdf({
      "<< /Type /Catalog /Pages 2 0 R /Outlines << /Type /Outlines >> >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
  }));
  ASSERT_TRUE(doc);
  EXPECT_FALSE(Create(doc.get(), 0, 0, L"x"));
}

// A dictionary that isn't an item (a page, an action, a field, an
// annotation, a page tree) ends the list a broken file links it into: the
// readers don't show it, and no write takes it for an item or changes it.
TEST_F(EPDFOutlineEmbedderTest, ADictionaryThatIsntAnItemEndsTheList) {
  for (const char* other : {
           "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
           "<< /MediaBox [0 0 300 300] /Resources << >> >>",
           "<< /S /URI /URI (https://example.com) /Next 7 0 R >>",
           "<< /FT /Tx /T (name) /Parent 3 0 R >>",
           "<< /Subtype /Text /Rect [0 0 10 10] >>",
           "<< /Type /Pages /Kids [4 0 R] /Count 1 >>",
       }) {
    SCOPED_TRACE(other);
    ScopedFPDFDocument doc = OpenPlain(MakeDocumentWith({
        "<< /Type /Outlines /First 5 0 R /Last 5 0 R /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
        "<< /Title (One) /Parent 3 0 R /Next 6 0 R >>",
        other,
    }));
    ASSERT_TRUE(doc);
    CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());
    const std::string before = ObjectGraph(pdf)[6];
    EXPECT_EQ("1:One", Tree(doc.get()));
    EXPECT_FALSE(EPDFOutline_GetItem(doc.get(), 6));
    EXPECT_FALSE(EPDFOutline_DeleteItem(doc.get(), 6));
    EXPECT_FALSE(EPDFOutline_MoveItem(doc.get(), 6, 0, 0));
    EXPECT_FALSE(Create(doc.get(), 6, 0, L"under it"));
    ASSERT_TRUE(Create(doc.get(), 0, 5, L"Two"));
    EXPECT_EQ(before, ObjectGraph(pdf)[6]);
    EXPECT_EQ("2:One Two", Tree(doc.get()));
    EXPECT_EQ("", Problems(doc.get()));
    EXPECT_EQ(1, FPDF_GetPageCount(doc.get()));
  }
}

// An /Outlines that isn't an outline's dictionary (here the page tree) is
// no outline: the readers show none, and a create makes a new outline in
// its place without touching what was linked there.
TEST_F(EPDFOutlineEmbedderTest, AnOutlinesThatIsntAnOutlineIsNone) {
  ScopedFPDFDocument doc = OpenPlain(MakePdf({
      "<< /Type /Catalog /Pages 2 0 R /Outlines 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
  }));
  ASSERT_TRUE(doc);
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());
  const std::string pages = ObjectGraph(pdf)[2];
  EXPECT_FALSE(FPDFBookmark_GetFirstChild(doc.get(), nullptr));
  ASSERT_TRUE(Create(doc.get(), 0, 0, L"One"));
  EXPECT_EQ(pages, ObjectGraph(pdf)[2]);
  EXPECT_NE(2u, OutlinesOf(doc.get()));
  EXPECT_EQ("1:One", Tree(doc.get()));
  EXPECT_EQ(1, FPDF_GetPageCount(doc.get()));
}

// Lists a broken file shares are counted once: in an outline whose every
// item lists the items before it as its children, each item counts once,
// and counting costs a look at each (not one per list it is in).
TEST_F(EPDFOutlineEmbedderTest, SharedListsAreCountedOnce) {
  constexpr uint32_t kItems = 3000;
  const uint32_t last = 4 + kItems;
  std::vector<std::string> outline = {
      "<< /Type /Outlines /First 5 0 R /Last " + std::to_string(last) +
          " 0 R >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
  };
  for (uint32_t objnum = 5; objnum <= last; ++objnum) {
    std::string item = "<< /Title (x) /Parent 3 0 R /First 5 0 R /Count 1";
    if (objnum < last) {
      item += " /Next " + std::to_string(objnum + 1) + " 0 R";
    }
    outline.push_back(item + " >>");
  }
  ScopedFPDFDocument doc = OpenPlain(MakeDocumentWith(outline));
  ASSERT_TRUE(doc);
  ASSERT_TRUE(Create(doc.get(), 0, last, L"After"));
  EXPECT_EQ(static_cast<int>(kItems) + 1,
            DictOf(doc.get(), 3)->GetIntegerFor("Count"));
}

// Reads of the outline go 128 levels deep; an item deeper than that is
// neither found nor written by its number, and the one at the last level
// still is.
TEST_F(EPDFOutlineEmbedderTest, ItemsDeeperThanReadsGoAreLeftAlone) {
  constexpr uint32_t kLevels = 130;
  std::vector<std::string> outline = {
      "<< /Type /Outlines /First 5 0 R /Last 5 0 R >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
  };
  for (uint32_t level = 0; level < kLevels; ++level) {
    const uint32_t objnum = 5 + level;
    std::string item = "<< /Title (x) /Parent " +
                       std::to_string(level ? objnum - 1 : 3) + " 0 R";
    if (level + 1 < kLevels) {
      const std::string child = std::to_string(objnum + 1);
      item += " /First " + child + " 0 R /Last " + child + " 0 R";
    }
    outline.push_back(item + " >>");
  }
  ScopedFPDFDocument doc = OpenPlain(MakeDocumentWith(outline));
  ASSERT_TRUE(doc);
  ScopedFPDFWideString title = GetFPDFWideString(L"y");
  EXPECT_TRUE(EPDFOutline_GetItem(doc.get(), 5 + 127));
  EXPECT_TRUE(EPDFOutline_SetTitle(doc.get(), 5 + 127, title.get()));
  EXPECT_FALSE(EPDFOutline_GetItem(doc.get(), 5 + 128));
  EXPECT_FALSE(EPDFOutline_SetTitle(doc.get(), 5 + 128, title.get()));
  EXPECT_FALSE(EPDFOutline_DeleteItem(doc.get(), 5 + 128));
}

// Random writes, checked after each one: the links and the counts are what
// the tree says. On a layer each write is a transaction, and one in five is
// aborted, which must leave the outline as it was.
void EPDFOutlineEmbedderTest::RandomWrites(FPDF_DOCUMENT doc, bool layer) {
  std::mt19937 random(20261018);
  auto pick = [&](const std::vector<uint32_t>& from) -> uint32_t {
    if (from.empty()) {
      return 0;
    }
    return from[std::uniform_int_distribution<size_t>(0,
                                                      from.size() - 1)(random)];
  };
  for (int step = 0; step < 300; ++step) {
    SCOPED_TRACE(step);
    const std::vector<uint32_t> items = Items(doc);
    const uint32_t item = pick(items);
    // A parent and a neighbour that may or may not fit: refusals must also
    // leave the outline right.
    const uint32_t parent = random() % 4 ? pick(items) : 0;
    std::vector<uint32_t> siblings;
    for (FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(
             doc, parent ? EPDFOutline_GetItem(doc, parent) : nullptr);
         child; child = FPDFBookmark_GetNextSibling(doc, child)) {
      siblings.push_back(EPDFOutline_GetObjectNumber(child));
    }
    const uint32_t after = random() % 3 ? pick(siblings) : 0;
    const std::string before = Tree(doc);
    if (layer) {
      ASSERT_TRUE(EPDFLayer_BeginTransaction(doc));
    }
    switch (random() % 5) {
      case 0:
      case 1:
        // A layer creates at numbers it holds; a plain document picks one.
        Create(doc, parent, after, L"new", layer ? next_number++ : 0);
        break;
      case 2:
        if (item) {
          EPDFOutline_MoveItem(doc, item, parent, after);
        }
        break;
      case 3:
        if (item && items.size() > 3) {
          EPDFOutline_DeleteItem(doc, item);
        }
        break;
      case 4:
        if (item) {
          EPDFOutline_SetOpen(doc, item, random() % 2);
        }
        break;
    }
    if (layer && random() % 5 == 0) {
      ASSERT_TRUE(EPDFLayer_AbortTransaction(doc));
      ASSERT_EQ(before, Tree(doc));
    } else if (layer) {
      ASSERT_TRUE(EPDFLayer_CommitTransaction(doc));
    }
    ASSERT_EQ("", Problems(doc));
  }
}

TEST_F(EPDFOutlineEmbedderTest, RandomWritesKeepTheOutlineRight) {
  ScopedFPDFDocument doc = OpenPlain();
  ASSERT_TRUE(doc);
  RandomWrites(doc.get(), /*layer=*/false);
}

TEST_F(EPDFOutlineEmbedderTest, RandomWritesKeepALayersOutlineRight) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_RaiseLastObjectNumber(layer.get(), 400));
  RandomWrites(layer.get(), /*layer=*/true);
}

// A write changes only what it must: appending an item touches the item
// before it and the outline, never the items further up the list.
TEST_F(EPDFOutlineEmbedderTest, WritesCopyOnlyWhatChanges) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  CPDF_LayerDocument* pdf = CPDF_LayerDocument::FromDocument(
      CPDFDocumentFromFPDFDocument(layer.get()));
  ASSERT_TRUE(Create(layer.get(), 0, kC, L"End"));
  EXPECT_TRUE(pdf->FindLayerVersion(kC));
  EXPECT_TRUE(pdf->FindLayerVersion(kOutlines));
  EXPECT_FALSE(pdf->FindLayerVersion(kA));
  EXPECT_FALSE(pdf->FindLayerVersion(kB));
  EXPECT_FALSE(pdf->FindLayerVersion(kA1));
  EXPECT_FALSE(pdf->FindLayerVersion(kC1));
}

// The readers see the layer's own versions of the file's items.
TEST_F(EPDFOutlineEmbedderTest, ReadsSeeWhatTheLayerWrote) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ScopedFPDFWideString renamed = GetFPDFWideString(L"B renamed");
  ASSERT_TRUE(EPDFOutline_SetTitle(layer.get(), kB, renamed.get()));
  ASSERT_TRUE(EPDFOutline_SetOpen(layer.get(), kC, true));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_EQ("5:A+1[A1] B renamed C+1[C1]", Tree(layer.get()));
  EXPECT_EQ("", Problems(layer.get()));
  ScopedFPDFDocument reopened = Reopen(layer.get());
  ASSERT_TRUE(reopened);
  EXPECT_EQ("5:A+1[A1] B renamed C+1[C1]", Tree(reopened.get()));
}

// An item the layer made comes back at its number and place, with the items
// under it and its action, also when a save and reopen dropped what the
// delete left behind.
TEST_F(EPDFOutlineEmbedderTest, DeletedLayerItemComesBack) {
  for (bool reopen : {false, true}) {
    SCOPED_TRACE(reopen ? "reopened between delete and import"
                        : "same session");
    ScopedFPDFDocument layer = OpenLayer();
    ASSERT_TRUE(layer);
    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    ASSERT_EQ(20u, Create(layer.get(), 0, kA, L"New", 20));
    ASSERT_EQ(21u, Create(layer.get(), 20, 0, L"New.1", 21));
    ASSERT_TRUE(EPDFOutline_SetOpen(layer.get(), 20, true));
    FPDF_ACTION action = GoTo(layer.get(), kFirstPage);
    ASSERT_TRUE(EPDFOutline_SetAction(layer.get(), 20, action));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    const uint32_t action_objnum = RefOf(DictOf(layer.get(), 20).Get(), "A");
    ASSERT_TRUE(action_objnum);
    EXPECT_EQ("6:A+1[A1] New+1[New.1] B C-1[C1]", Tree(layer.get()));

    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    const std::vector<uint8_t> capture = ExportItem(layer.get(), 20);
    ASSERT_FALSE(capture.empty());
    ASSERT_TRUE(EPDFOutline_DeleteItem(layer.get(), 20));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    EXPECT_EQ("4:A+1[A1] B C-1[C1]", Tree(layer.get()));
    EXPECT_FALSE(EPDFOutline_GetItem(layer.get(), 20));

    ScopedFPDFDocument target = reopen ? Reopen(layer.get()) : std::move(layer);
    ASSERT_TRUE(target);
    if (reopen) {
      EXPECT_FALSE(DictOf(target.get(), action_objnum));
    }
    ASSERT_TRUE(EPDFLayer_BeginTransaction(target.get()));
    ASSERT_TRUE(ImportItem(target.get(), capture));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(target.get()));
    EXPECT_EQ("6:A+1[A1] New+1[New.1] B C-1[C1]", Tree(target.get()));
    EXPECT_EQ(action_objnum, RefOf(DictOf(target.get(), 20).Get(), "A"));
    EXPECT_TRUE(DictOf(target.get(), action_objnum));
    EXPECT_EQ("", Problems(target.get()));
  }
}

// An item of the file comes back as the file's original, the items under it
// with it; the layer edited it, so the capture holds that edit.
TEST_F(EPDFOutlineEmbedderTest, DeletedFileItemComesBack) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ScopedFPDFWideString renamed = GetFPDFWideString(L"A1 renamed");
  ASSERT_TRUE(EPDFOutline_SetTitle(layer.get(), kA1, renamed.get()));
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportItem(layer.get(), kA);
  ASSERT_FALSE(capture.empty());
  ASSERT_TRUE(EPDFOutline_DeleteItem(layer.get(), kA));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_EQ("2:B C-1[C1]", Tree(layer.get()));

  ScopedFPDFDocument reopened = Reopen(layer.get());
  ASSERT_TRUE(reopened);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(reopened.get()));
  ASSERT_TRUE(ImportItem(reopened.get(), capture));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(reopened.get()));
  EXPECT_EQ("4:A+1[A1 renamed] B C-1[C1]", Tree(reopened.get()));
  EXPECT_TRUE(DictOf(reopened.get(), kA)->KeyExist("Dest"));
  EXPECT_EQ("", Problems(reopened.get()));
}

// Its place: after the sibling it followed while that is still there, else
// first; never twice, and never under a parent that is gone.
TEST_F(EPDFOutlineEmbedderTest, ImportFindsItsPlace) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> b = ExportItem(layer.get(), kB);
  const std::vector<uint8_t> c1 = ExportItem(layer.get(), kC1);
  ASSERT_FALSE(b.empty());
  ASSERT_FALSE(c1.empty());
  EXPECT_FALSE(ImportItem(layer.get(), b));  // it's there
  ASSERT_TRUE(EPDFOutline_DeleteItem(layer.get(), kB));
  ASSERT_TRUE(EPDFOutline_DeleteItem(layer.get(), kA));
  ASSERT_TRUE(EPDFOutline_DeleteItem(layer.get(), kC));
  EXPECT_FALSE(ImportItem(layer.get(), c1));  // its parent is gone
  ASSERT_TRUE(Create(layer.get(), 0, 0, L"Other"));
  ASSERT_TRUE(ImportItem(layer.get(), b));  // A is gone: first
  EXPECT_FALSE(ImportItem(layer.get(), b));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_EQ("2:B Other", Tree(layer.get()));
  EXPECT_EQ("", Problems(layer.get()));
}

// What goes back into the outline is what the item's number holds once the
// capture is in: a page that took the number after a save dropped the
// deleted item never becomes an item.
TEST_F(EPDFOutlineEmbedderTest, ImportRefusesWhenItsNumberHoldsSomethingElse) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_EQ(20u, Create(layer.get(), 0, kC, L"New", 20));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportItem(layer.get(), 20);
  ASSERT_FALSE(capture.empty());
  ASSERT_TRUE(EPDFOutline_DeleteItem(layer.get(), 20));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));

  ScopedFPDFDocument reopened = Reopen(layer.get());
  ASSERT_TRUE(reopened);
  ASSERT_TRUE(EPDFLayer_RaiseLastObjectNumber(reopened.get(), 40));
  ASSERT_TRUE(EPDFLayer_BeginTransaction(reopened.get()));
  ASSERT_TRUE(EPDFPage_InsertBlankRaw(reopened.get(), 0, 100, 100, 20));
  EXPECT_FALSE(ImportItem(reopened.get(), capture));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(reopened.get()));
  EXPECT_EQ("4:A+1[A1] B C-1[C1]", Tree(reopened.get()));
}

TEST_F(EPDFOutlineEmbedderTest, CapturesNeedAnItemAndALayer) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  EXPECT_TRUE(ExportItem(layer.get(), kOutlines).empty());
  EXPECT_TRUE(ExportItem(layer.get(), kFirstPage).empty());
  EXPECT_TRUE(ExportItem(layer.get(), 99).empty());
  const std::vector<uint8_t> capture = ExportItem(layer.get(), kB);
  ASSERT_FALSE(capture.empty());
  EXPECT_FALSE(ImportItem(layer.get(), capture));  // outside a transaction
  ScopedFPDFDocument plain = OpenPlain();
  ASSERT_TRUE(plain);
  EXPECT_TRUE(ExportItem(plain.get(), kB).empty());
}
