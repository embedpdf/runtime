// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// The document security store (EPDFDss_* in public/epdf_signature.h): what
// it reads from the store's lists and its index, and what adding an item
// writes, in a plain document and in a layer.

#include "public/epdf_signature.h"

#include <stdint.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_layer.h"
#include "public/epdf_save.h"
#include "public/epdf_view.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

constexpr int kKinds[] = {EPDF_DSS_CERTIFICATES, EPDF_DSS_OCSP_RESPONSES,
                          EPDF_DSS_CRLS};

// Bytes as a store holds them: binary, with zero bytes and every value a
// PDF string would have to escape.
std::string Der(const std::string& label) {
  return std::string("\x30\x82\x00\x0d\x00)(\\\r\n>", 11) + label;
}

std::string HexStream(const std::string& data) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string hex;
  for (unsigned char byte : data) {
    hex += kDigits[byte >> 4];
    hex += kDigits[byte & 0xf];
  }
  return Stream("/Filter /ASCIIHexDecode", hex + ">");
}

std::vector<std::string> PageObjects(const std::string& catalog_extra) {
  return {
      "<< /Type /Catalog /Pages 2 0 R " + catalog_extra + " >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>",
  };
}

std::string PdfWithoutStore() {
  return MakePdf(PageObjects(""));
}

// A store as Acrobat lays one out: an object of its own (4) whose lists are
// objects of their own (5, 6, 7), with an index.
//   certificates: 8, 9          OCSP responses: 10          CRLs: 11
//   index: one entry naming 8 and 10
std::string PdfWithStoreOfObjects() {
  std::vector<std::string> objects = PageObjects("/DSS 4 0 R");
  objects.push_back(
      "<< /Certs 5 0 R /OCSPs 6 0 R /CRLs 7 0 R /VRI << /AB12 << /Cert [8 0 R] "
      "/OCSP [10 0 R] >> >> >>");
  objects.push_back("[8 0 R 9 0 R]");
  objects.push_back("[10 0 R]");
  objects.push_back("[11 0 R]");
  objects.push_back(Stream(Der("certificate one")));
  objects.push_back(HexStream(Der("certificate two")));
  objects.push_back(Stream(Der("answer one")));
  objects.push_back(Stream(Der("list one")));
  return MakePdf(objects);
}

// A store written inside the catalog, with its lists inside it.
std::string PdfWithStoreInTheCatalog() {
  std::vector<std::string> objects =
      PageObjects("/DSS << /Certs [4 0 R] /CRLs [5 0 R] >>");
  objects.push_back(Stream(Der("certificate one")));
  objects.push_back(Stream(Der("list one")));
  return MakePdf(objects);
}

// A PDF file opened as a document, or as a layer over it. Both read
// `input`, which must outlive them.
struct OpenedPdf {
  std::unique_ptr<std::remove_pointer_t<EPDF_BASE_DOCUMENT>,
                  decltype(&EPDF_ReleaseBaseDocument)>
      base{nullptr, EPDF_ReleaseBaseDocument};
  ScopedFPDFDocument doc;
};

OpenedPdf OpenPdf(const std::string& input, bool layer) {
  OpenedPdf opened;
  if (layer) {
    opened.base.reset(
        EPDF_LoadMemBaseDocument64(input.data(), input.size(), nullptr));
    if (opened.base) {
      opened.doc.reset(
          EPDFLayer_OpenLayer(opened.base.get(), nullptr, nullptr, nullptr));
    }
  } else {
    // Plain in layer mode too: the layer is the other case.
    opened.doc.reset(
        FPDF_LoadMemDocument64(input.data(), input.size(), nullptr));
  }
  return opened;
}

// Every item of one kind, read with the two-call protocol.
std::vector<std::string> ItemsOf(FPDF_DOCUMENT doc, int kind) {
  std::vector<std::string> items;
  const int count = EPDFDss_GetCount(doc, kind);
  for (int i = 0; i < count; ++i) {
    const unsigned long size = EPDFDss_GetData(doc, kind, i, nullptr, 0);
    std::string bytes(size, '\0');
    if (size > 0) {
      EXPECT_EQ(size, EPDFDss_GetData(doc, kind, i, bytes.data(), size));
    }
    items.push_back(bytes);
  }
  return items;
}

// Adds an item; on a layer in a transaction, as the engine runs every write.
uint32_t Add(FPDF_DOCUMENT doc, bool layer, int kind, const std::string& data) {
  if (layer) {
    EXPECT_TRUE(EPDFLayer_BeginTransaction(doc));
  }
  const uint32_t number = EPDFDss_Add(doc, kind, data.data(), data.size());
  if (layer) {
    EXPECT_TRUE(EPDFLayer_CommitTransaction(doc));
  }
  return number;
}

// The object numbers a revision of |saved| wrote after its first
// |base_size| bytes, the trailer left out.
std::set<unsigned int> WrittenAfter(const std::string& saved,
                                    size_t base_size) {
  std::set<unsigned int> written;
  // Plain in layer mode too: revisions are read from a file's own bytes.
  ScopedFPDFDocument newer(
      FPDF_LoadMemDocument64(saved.data(), saved.size(), nullptr));
  EXPECT_TRUE(newer);
  ScopedFPDFDocument older(EPDFDoc_OpenRevision(newer.get(), base_size));
  EXPECT_TRUE(older);
  EPDF_OBJECT_DIFF diff = EPDFDoc_CompareRevisions(older.get(), newer.get());
  EXPECT_TRUE(diff);
  const int count = EPDFObjectDiff_GetCount(diff);
  for (int i = 0; i < count; ++i) {
    unsigned int number = 0;
    EXPECT_TRUE(EPDFObjectDiff_GetEntry(diff, i, &number, nullptr, nullptr,
                                        nullptr, nullptr, nullptr));
    if (number != 0) {
      written.insert(number);
    }
  }
  EPDFObjectDiff_Close(diff);
  return written;
}

}  // namespace

class EPDFDssEmbedderTest : public EmbedderTest {};

TEST_F(EPDFDssEmbedderTest, BadArguments) {
  const std::string input = PdfWithStoreOfObjects();
  OpenedPdf opened = OpenPdf(input, false);
  ASSERT_TRUE(opened.doc);
  const std::string data = Der("x");

  EXPECT_EQ(-1, EPDFDss_GetCount(nullptr, EPDF_DSS_CERTIFICATES));
  EXPECT_EQ(-1, EPDFDss_GetCount(opened.doc.get(), 3));
  EXPECT_EQ(-1, EPDFDss_GetCount(opened.doc.get(), -1));

  EXPECT_EQ(0u, EPDFDss_GetData(nullptr, EPDF_DSS_CERTIFICATES, 0, nullptr, 0));
  EXPECT_EQ(0u, EPDFDss_GetData(opened.doc.get(), 3, 0, nullptr, 0));
  EXPECT_EQ(0u, EPDFDss_GetData(opened.doc.get(), EPDF_DSS_CERTIFICATES, -1,
                                nullptr, 0));
  EXPECT_EQ(0u, EPDFDss_GetData(opened.doc.get(), EPDF_DSS_CERTIFICATES, 2,
                                nullptr, 0));

  EXPECT_EQ(0u, EPDFDss_Add(nullptr, EPDF_DSS_CRLS, data.data(), data.size()));
  EXPECT_EQ(0u, EPDFDss_Add(opened.doc.get(), 3, data.data(), data.size()));
  EXPECT_EQ(0u, EPDFDss_Add(opened.doc.get(), EPDF_DSS_CRLS, nullptr, 4));
  EXPECT_EQ(0u, EPDFDss_Add(opened.doc.get(), EPDF_DSS_CRLS, data.data(), 0));
  EXPECT_EQ(1, EPDFDss_GetCount(opened.doc.get(), EPDF_DSS_CRLS));
}

TEST_F(EPDFDssEmbedderTest, ADocumentWithoutAStoreReadsEmpty) {
  const std::string input = PdfWithoutStore();
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    for (int kind : kKinds) {
      EXPECT_EQ(0, EPDFDss_GetCount(opened.doc.get(), kind));
      EXPECT_EQ(0u, EPDFDss_GetData(opened.doc.get(), kind, 0, nullptr, 0));
    }
  }
}

TEST_F(EPDFDssEmbedderTest, ReadsEachListDecoded) {
  const std::string input = PdfWithStoreOfObjects();
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    FPDF_DOCUMENT doc = opened.doc.get();

    // The second certificate is stored through a filter.
    EXPECT_EQ(std::vector<std::string>(
                  {Der("certificate one"), Der("certificate two")}),
              ItemsOf(doc, EPDF_DSS_CERTIFICATES));
    EXPECT_EQ(std::vector<std::string>({Der("answer one")}),
              ItemsOf(doc, EPDF_DSS_OCSP_RESPONSES));
    EXPECT_EQ(std::vector<std::string>({Der("list one")}),
              ItemsOf(doc, EPDF_DSS_CRLS));

    // A buffer that can't hold every byte gets none.
    const std::string expected = Der("certificate one");
    std::string small(expected.size() - 1, '-');
    EXPECT_EQ(expected.size(), EPDFDss_GetData(doc, EPDF_DSS_CERTIFICATES, 0,
                                               small.data(), small.size()));
    EXPECT_EQ(std::string(expected.size() - 1, '-'), small);
  }
}

// The index names streams of the same three kinds. What only it names
// follows the list, in the order of the entries' keys; what a list or an
// earlier entry already named is not read twice; what is not a reference to a
// stream is skipped.
TEST_F(EPDFDssEmbedderTest, ReadsWhatOnlyTheIndexNamesAndEachStreamOnce) {
  std::vector<std::string> objects = PageObjects("/DSS 4 0 R");
  objects.push_back(
      "<< /Certs [5 0 R 6 0 R 5 0 R] /CRLs [8 0 R (text) 2 0 R 99 0 R] "
      "/VRI << /B << /Cert [6 0 R 9 0 R] /OCSP [7 0 R] >> /A 10 0 R "
      "/C (not an entry) >> >>");
  objects.push_back(Stream(Der("certificate one")));    // 5
  objects.push_back(Stream(Der("certificate two")));    // 6
  objects.push_back(Stream(Der("answer one")));         // 7
  objects.push_back(Stream(Der("list one")));           // 8
  objects.push_back(Stream(Der("certificate three")));  // 9
  objects.push_back("<< /Cert [11 0 R] /CRL [8 0 R 12 0 R] /OCSP 7 0 R >>");
  objects.push_back(Stream(Der("certificate four")));  // 11
  objects.push_back(Stream(Der("list two")));          // 12
  const std::string input = MakePdf(objects);

  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    FPDF_DOCUMENT doc = opened.doc.get();

    // The list, then entry A (an object of its own), then entry B.
    EXPECT_EQ(std::vector<std::string>(
                  {Der("certificate one"), Der("certificate two"),
                   Der("certificate four"), Der("certificate three")}),
              ItemsOf(doc, EPDF_DSS_CERTIFICATES));
    // No list of answers at all: the index alone names one. Entry A's
    // /OCSP is a stream where an array belongs, and is skipped.
    EXPECT_EQ(std::vector<std::string>({Der("answer one")}),
              ItemsOf(doc, EPDF_DSS_OCSP_RESPONSES));
    // A string, a dictionary and a missing object in the list are skipped.
    EXPECT_EQ(std::vector<std::string>({Der("list one"), Der("list two")}),
              ItemsOf(doc, EPDF_DSS_CRLS));
  }
}

TEST_F(EPDFDssEmbedderTest, AddMakesAStoreAsAnObjectOfItsOwn) {
  const std::string input = PdfWithoutStore();
  for (bool layer : {false, true}) {
    for (FPDF_DWORD flags : {FPDF_NO_INCREMENTAL, FPDF_INCREMENTAL}) {
      SCOPED_TRACE(layer ? "layer" : "document");
      SCOPED_TRACE(flags == FPDF_INCREMENTAL ? "incremental" : "rewrite");
      OpenedPdf opened = OpenPdf(input, layer);
      ASSERT_TRUE(opened.doc);
      FPDF_DOCUMENT doc = opened.doc.get();

      const uint32_t first =
          Add(doc, layer, EPDF_DSS_CERTIFICATES, Der("certificate one"));
      const uint32_t second =
          Add(doc, layer, EPDF_DSS_OCSP_RESPONSES, Der("answer one"));
      const uint32_t third = Add(doc, layer, EPDF_DSS_CRLS, Der("list one"));
      const uint32_t fourth =
          Add(doc, layer, EPDF_DSS_CERTIFICATES, Der("certificate two"));
      // Objects 1 to 3 are the file's; the store is the first new one.
      EXPECT_EQ(5u, first);
      EXPECT_EQ(6u, second);
      EXPECT_EQ(7u, third);
      EXPECT_EQ(8u, fourth);

      // Read back before any save.
      const std::vector<std::string> certificates = {Der("certificate one"),
                                                     Der("certificate two")};
      EXPECT_EQ(certificates, ItemsOf(doc, EPDF_DSS_CERTIFICATES));
      EXPECT_EQ(std::vector<std::string>({Der("answer one")}),
                ItemsOf(doc, EPDF_DSS_OCSP_RESPONSES));
      EXPECT_EQ(std::vector<std::string>({Der("list one")}),
                ItemsOf(doc, EPDF_DSS_CRLS));

      {
        CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
        CPDF_DocumentViewScope document_view(pdf);
        const CPDF_Dictionary* catalog = pdf->GetRoot();
        // The store is an object of its own, its lists are inside it.
        RetainPtr<const CPDF_Reference> reference =
            ToReference(catalog->GetObjectFor("DSS"));
        ASSERT_TRUE(reference);
        EXPECT_EQ(4u, reference->GetRefObjNum());
        RetainPtr<const CPDF_Dictionary> store = catalog->GetDictFor("DSS");
        ASSERT_TRUE(store);
        EXPECT_EQ(std::vector<ByteString>({"CRLs", "Certs", "OCSPs"}),
                  store->GetKeys());
        for (const char* key : {"Certs", "OCSPs", "CRLs"}) {
          EXPECT_TRUE(ToArray(store->GetObjectFor(key))) << key;
        }
        // No index, and nothing announced in the catalog.
        EXPECT_FALSE(catalog->KeyExist("Extensions"));
        // A stream holds the bytes as they were given, and says nothing more.
        RetainPtr<const CPDF_Stream> stream =
            store->GetArrayFor("Certs")->GetStreamAt(1);
        ASSERT_TRUE(stream);
        EXPECT_EQ(fourth, stream->GetObjNum());
        EXPECT_FALSE(stream->HasFilter());
        EXPECT_FALSE(stream->GetDict()->KeyExist("Type"));
      }

      ClearString();
      ASSERT_TRUE(FPDF_SaveAsCopy(doc, this, flags));
      const std::string saved = GetString();
      if (flags == FPDF_INCREMENTAL) {
        // The catalog, the store and the four streams, and nothing else.
        EXPECT_EQ(std::set<unsigned int>({1, 4, 5, 6, 7, 8}),
                  WrittenAfter(saved, input.size()));
      }
      ScopedSavedDoc reopened = OpenScopedSavedDocument();
      ASSERT_TRUE(reopened);
      EXPECT_EQ(certificates, ItemsOf(reopened.get(), EPDF_DSS_CERTIFICATES));
      EXPECT_EQ(std::vector<std::string>({Der("answer one")}),
                ItemsOf(reopened.get(), EPDF_DSS_OCSP_RESPONSES));
      EXPECT_EQ(std::vector<std::string>({Der("list one")}),
                ItemsOf(reopened.get(), EPDF_DSS_CRLS));
    }
  }
}

// Acrobat's layout: the store and each list are objects of their own. An
// item is added to the list's own object; the store and the catalog are not
// written again.
TEST_F(EPDFDssEmbedderTest, AddExtendsAListThatIsAnObjectOfItsOwn) {
  const std::string input = PdfWithStoreOfObjects();
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    FPDF_DOCUMENT doc = opened.doc.get();

    const uint32_t added = Add(doc, layer, EPDF_DSS_CRLS, Der("list two"));
    EXPECT_EQ(12u, added);
    EXPECT_EQ(std::vector<std::string>({Der("list one"), Der("list two")}),
              ItemsOf(doc, EPDF_DSS_CRLS));
    // The other kinds are as they were.
    EXPECT_EQ(2, EPDFDss_GetCount(doc, EPDF_DSS_CERTIFICATES));
    EXPECT_EQ(1, EPDFDss_GetCount(doc, EPDF_DSS_OCSP_RESPONSES));

    {
      CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
      CPDF_DocumentViewScope document_view(pdf);
      RetainPtr<const CPDF_Dictionary> store =
          pdf->GetRoot()->GetDictFor("DSS");
      ASSERT_TRUE(store);
      RetainPtr<const CPDF_Reference> list =
          ToReference(store->GetObjectFor("CRLs"));
      ASSERT_TRUE(list);
      EXPECT_EQ(7u, list->GetRefObjNum());
      // The index is untouched: adding writes no entry.
      EXPECT_EQ(std::vector<ByteString>({"AB12"}),
                store->GetDictFor("VRI")->GetKeys());
    }

    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(doc, this, FPDF_INCREMENTAL));
    EXPECT_EQ(std::set<unsigned int>({7, 12}),
              WrittenAfter(GetString(), input.size()));
    ScopedSavedDoc reopened = OpenScopedSavedDocument();
    ASSERT_TRUE(reopened);
    EXPECT_EQ(std::vector<std::string>({Der("list one"), Der("list two")}),
              ItemsOf(reopened.get(), EPDF_DSS_CRLS));
  }
}

// A store written inside the catalog stays there: the catalog is the one
// object that changes, and a kind it had no list for gets one inside it.
TEST_F(EPDFDssEmbedderTest, AddExtendsAStoreInsideTheCatalog) {
  const std::string input = PdfWithStoreInTheCatalog();
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    FPDF_DOCUMENT doc = opened.doc.get();

    EXPECT_EQ(6u,
              Add(doc, layer, EPDF_DSS_CERTIFICATES, Der("certificate two")));
    EXPECT_EQ(7u, Add(doc, layer, EPDF_DSS_OCSP_RESPONSES, Der("answer one")));
    EXPECT_EQ(std::vector<std::string>(
                  {Der("certificate one"), Der("certificate two")}),
              ItemsOf(doc, EPDF_DSS_CERTIFICATES));
    EXPECT_EQ(std::vector<std::string>({Der("answer one")}),
              ItemsOf(doc, EPDF_DSS_OCSP_RESPONSES));
    EXPECT_EQ(std::vector<std::string>({Der("list one")}),
              ItemsOf(doc, EPDF_DSS_CRLS));
    {
      CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
      CPDF_DocumentViewScope document_view(pdf);
      EXPECT_TRUE(ToDictionary(pdf->GetRoot()->GetObjectFor("DSS")));
    }

    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(doc, this, FPDF_INCREMENTAL));
    EXPECT_EQ(std::set<unsigned int>({1, 6, 7}),
              WrittenAfter(GetString(), input.size()));
    ScopedSavedDoc reopened = OpenScopedSavedDocument();
    ASSERT_TRUE(reopened);
    EXPECT_EQ(std::vector<std::string>(
                  {Der("certificate one"), Der("certificate two")}),
              ItemsOf(reopened.get(), EPDF_DSS_CERTIFICATES));
  }
}

// A store or a list that is there and is something else is left alone, and
// so is the rest of the document.
TEST_F(EPDFDssEmbedderTest, AddRefusesAStoreItCannotExtend) {
  const std::string not_a_store = MakePdf(PageObjects("/DSS [ /Certs 2 0 R ]"));
  std::vector<std::string> objects = PageObjects("/DSS 4 0 R");
  objects.push_back("<< /Certs << /Not /AnArray >> /OCSPs 5 0 R >>");
  objects.push_back("(a string where a list belongs)");
  const std::string bad_lists = MakePdf(objects);
  const std::string data = Der("x");

  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    {
      OpenedPdf opened = OpenPdf(not_a_store, layer);
      ASSERT_TRUE(opened.doc);
      CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(opened.doc.get());
      const std::map<uint32_t, std::string> before = ObjectGraph(pdf);
      for (int kind : kKinds) {
        EXPECT_EQ(0u, Add(opened.doc.get(), layer, kind, data));
        EXPECT_EQ(0, EPDFDss_GetCount(opened.doc.get(), kind));
      }
      EXPECT_EQ(before, ObjectGraph(pdf));
    }
    {
      OpenedPdf opened = OpenPdf(bad_lists, layer);
      ASSERT_TRUE(opened.doc);
      CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(opened.doc.get());
      const std::map<uint32_t, std::string> before = ObjectGraph(pdf);
      EXPECT_EQ(0u, Add(opened.doc.get(), layer, EPDF_DSS_CERTIFICATES, data));
      EXPECT_EQ(0u,
                Add(opened.doc.get(), layer, EPDF_DSS_OCSP_RESPONSES, data));
      EXPECT_EQ(before, ObjectGraph(pdf));
      // The kind whose list is simply missing is still served.
      EXPECT_EQ(6u, Add(opened.doc.get(), layer, EPDF_DSS_CRLS, data));
      EXPECT_EQ(std::vector<std::string>({data}),
                ItemsOf(opened.doc.get(), EPDF_DSS_CRLS));
    }
  }
}

TEST_F(EPDFDssEmbedderTest, AddOnALayerLeavesItsBaseAlone) {
  const std::string input = PdfWithStoreOfObjects();
  std::unique_ptr<std::remove_pointer_t<EPDF_BASE_DOCUMENT>,
                  decltype(&EPDF_ReleaseBaseDocument)>
      base(EPDF_LoadMemBaseDocument64(input.data(), input.size(), nullptr),
           EPDF_ReleaseBaseDocument);
  ASSERT_TRUE(base);
  ScopedFPDFDocument edited(
      EPDFLayer_OpenLayer(base.get(), nullptr, nullptr, nullptr));
  ScopedFPDFDocument sibling(
      EPDFLayer_OpenLayer(base.get(), nullptr, nullptr, nullptr));
  ASSERT_TRUE(edited);
  ASSERT_TRUE(sibling);
  EXPECT_NE(0u, Add(edited.get(), true, EPDF_DSS_CERTIFICATES,
                    Der("certificate three")));
  EXPECT_EQ(3, EPDFDss_GetCount(edited.get(), EPDF_DSS_CERTIFICATES));
  EXPECT_EQ(2, EPDFDss_GetCount(sibling.get(), EPDF_DSS_CERTIFICATES));

  // The delta carries the new item back onto the base.
  ClearString();
  EPDFLayerSaveStatus status = EPDFLayerSaveStatus_kSaveFailed;
  ASSERT_TRUE(EPDFLayer_SaveDelta(edited.get(), this, &status));
  std::string delta = GetString();
  FPDF_FILEACCESS delta_access = {};
  delta_access.m_FileLen = delta.size();
  delta_access.m_GetBlock = GetBlockFromString;
  delta_access.m_Param = &delta;
  ScopedFPDFDocument replayed(
      EPDFLayer_OpenLayer(base.get(), &delta_access, nullptr, nullptr));
  ASSERT_TRUE(replayed);
  EXPECT_EQ(
      std::vector<std::string>({Der("certificate one"), Der("certificate two"),
                                Der("certificate three")}),
      ItemsOf(replayed.get(), EPDF_DSS_CERTIFICATES));
}

// A save of an encrypted file encrypts the store's streams with everything
// else, and a reader decrypts them back.
TEST_F(EPDFDssEmbedderTest, AnEncryptedSaveKeepsTheStoreReadable) {
  const std::string input = PdfWithoutStore();
  std::string encrypted;
  {
    OpenedPdf opened = OpenPdf(input, false);
    ASSERT_TRUE(opened.doc);
    ASSERT_TRUE(
        EPDF_SetEncryption(opened.doc.get(), "", "owner", EPDF_PERM_PRINT));
    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_NO_INCREMENTAL));
    encrypted = GetString();
  }
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    OpenedPdf opened = OpenPdf(encrypted, layer);
    ASSERT_TRUE(opened.doc);
    ASSERT_TRUE(EPDF_IsEncrypted(opened.doc.get()));
    EXPECT_NE(0u, Add(opened.doc.get(), layer, EPDF_DSS_OCSP_RESPONSES,
                      Der("answer one")));
    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_INCREMENTAL));
    EXPECT_EQ(std::string::npos, GetString().find("answer one"));
    ScopedSavedDoc reopened = OpenScopedSavedDocument();
    ASSERT_TRUE(reopened);
    EXPECT_EQ(std::vector<std::string>({Der("answer one")}),
              ItemsOf(reopened.get(), EPDF_DSS_OCSP_RESPONSES));
  }
}
