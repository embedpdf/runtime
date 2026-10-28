// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_catalog.h"

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

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
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

constexpr char kOldPacket[] =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
    "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
    "<rdf:Description rdf:about=\"\" "
    "xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\">"
    "<pdf:Producer>OLDPRODUCER</pdf:Producer>"
    "</rdf:Description></rdf:RDF></x:xmpmeta>";

constexpr char kNewPacket[] =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
    "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
    "<rdf:Description rdf:about=\"\"/></rdf:RDF></x:xmpmeta>";

std::string Hex(const std::string& bytes) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string hex;
  for (unsigned char byte : bytes) {
    hex += kDigits[byte >> 4];
    hex += kDigits[byte & 0xf];
  }
  return hex + ">";
}

// A one-page PDF whose catalog has a /Metadata stream (object 4) holding
// kOldPacket hex-encoded, so reading it applies a filter.
std::string PdfWithPacket() {
  const std::string encoded = Hex(kOldPacket);
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R /Metadata 4 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>",
      "<< /Type /Metadata /Subtype /XML /Filter /ASCIIHexDecode /Length " +
          std::to_string(encoded.size()) + " >>\nstream\n" + encoded +
          "\nendstream",
  });
}

std::string PdfWithoutPacket() {
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>",
  });
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
    opened.doc.reset(
        FPDF_LoadMemDocument64(input.data(), input.size(), nullptr));
  }
  return opened;
}

std::string MetadataOf(FPDF_DOCUMENT doc) {
  const unsigned long size = EPDFCatalog_GetMetadata(doc, nullptr, 0);
  std::string bytes(size, '\0');
  if (size > 0) {
    EXPECT_EQ(size, EPDFCatalog_GetMetadata(doc, bytes.data(), size));
  }
  return bytes;
}

// Sets the packet; on a layer in a transaction, as the engine runs every
// write.
void SetPacket(FPDF_DOCUMENT doc, bool layer, const char* packet) {
  if (layer) {
    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc));
  }
  ASSERT_TRUE(EPDFCatalog_SetMetadata(
      doc, packet, packet ? std::char_traits<char>::length(packet) : 0));
  if (layer) {
    ASSERT_TRUE(EPDFLayer_CommitTransaction(doc));
  }
}

// The catalog's /Metadata entry: its object number, and its stream.
struct MetadataEntry {
  uint32_t objnum = 0;
  RetainPtr<const CPDF_Stream> stream;
};

MetadataEntry EntryOf(FPDF_DOCUMENT doc) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  CPDF_DocumentViewScope document_view(pdf);
  MetadataEntry entry;
  RetainPtr<const CPDF_Reference> reference =
      ToReference(pdf->GetRoot()->GetObjectFor("Metadata"));
  if (reference) {
    entry.objnum = reference->GetRefObjNum();
  }
  entry.stream = pdf->GetRoot()->GetStreamFor("Metadata");
  return entry;
}

}  // namespace

class EPDFCatalogEmbedderTest : public EmbedderTest {};

TEST_F(EPDFCatalogEmbedderTest, GetMetadataReadsThePacketDecoded) {
  const std::string input = PdfWithPacket();
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);

    const unsigned long size = std::char_traits<char>::length(kOldPacket);
    EXPECT_EQ(size, EPDFCatalog_GetMetadata(opened.doc.get(), nullptr, 0));
    // A buffer that can't hold every byte gets none.
    std::string small(size - 1, '-');
    EXPECT_EQ(size, EPDFCatalog_GetMetadata(opened.doc.get(), small.data(),
                                            small.size()));
    EXPECT_EQ(std::string(size - 1, '-'), small);
    EXPECT_EQ(kOldPacket, MetadataOf(opened.doc.get()));
  }

  const std::string empty = PdfWithoutPacket();
  OpenedPdf without = OpenPdf(empty, false);
  ASSERT_TRUE(without.doc);
  EXPECT_EQ(0u, EPDFCatalog_GetMetadata(without.doc.get(), nullptr, 0));
  EXPECT_EQ(0u, EPDFCatalog_GetMetadata(nullptr, nullptr, 0));
}

TEST_F(EPDFCatalogEmbedderTest, SetMetadataReplacesThePacketInPlace) {
  const std::string input = PdfWithPacket();
  for (bool layer : {false, true}) {
    for (FPDF_DWORD flags : {FPDF_NO_INCREMENTAL, FPDF_INCREMENTAL}) {
      SCOPED_TRACE(layer ? "layer" : "document");
      SCOPED_TRACE(flags == FPDF_INCREMENTAL ? "incremental" : "rewrite");
      OpenedPdf opened = OpenPdf(input, layer);
      ASSERT_TRUE(opened.doc);
      ASSERT_NO_FATAL_FAILURE(SetPacket(opened.doc.get(), layer, kNewPacket));

      EXPECT_EQ(kNewPacket, MetadataOf(opened.doc.get()));
      MetadataEntry entry = EntryOf(opened.doc.get());
      EXPECT_EQ(4u, entry.objnum);
      ASSERT_TRUE(entry.stream);
      EXPECT_FALSE(entry.stream->HasFilter());
      EXPECT_EQ("Metadata", entry.stream->GetDict()->GetNameFor("Type"));
      EXPECT_EQ("XML", entry.stream->GetDict()->GetNameFor("Subtype"));

      ClearString();
      ASSERT_TRUE(FPDF_SaveAsCopy(opened.doc.get(), this, flags));
      const std::string saved = GetString();
      // Written as it is, so XMP readers that scan the file find it.
      EXPECT_NE(std::string::npos, saved.find(kNewPacket));
      if (flags == FPDF_NO_INCREMENTAL) {
        EXPECT_EQ(std::string::npos, saved.find(Hex(kOldPacket)));
      }
      ScopedSavedDoc reopened = OpenScopedSavedDocument();
      ASSERT_TRUE(reopened);
      EXPECT_EQ(kNewPacket, MetadataOf(reopened.get()));
    }
  }
}

TEST_F(EPDFCatalogEmbedderTest, SetMetadataOnALayerLeavesItsBaseAlone) {
  const std::string input = PdfWithPacket();
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
  ASSERT_NO_FATAL_FAILURE(SetPacket(edited.get(), true, kNewPacket));
  EXPECT_EQ(kNewPacket, MetadataOf(edited.get()));
  EXPECT_EQ(kOldPacket, MetadataOf(sibling.get()));

  // The delta carries the new packet back onto the base.
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
  EXPECT_EQ(kNewPacket, MetadataOf(replayed.get()));
}

TEST_F(EPDFCatalogEmbedderTest, SetMetadataMakesAStreamWhenThereIsNone) {
  const std::string input = PdfWithoutPacket();
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    ASSERT_NO_FATAL_FAILURE(SetPacket(opened.doc.get(), layer, kNewPacket));
    EXPECT_EQ(kNewPacket, MetadataOf(opened.doc.get()));
    MetadataEntry entry = EntryOf(opened.doc.get());
    EXPECT_GT(entry.objnum, 3u);

    ClearString();
    ASSERT_TRUE(
        FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_NO_INCREMENTAL));
    EXPECT_NE(std::string::npos, GetString().find(kNewPacket));
    ScopedSavedDoc reopened = OpenScopedSavedDocument();
    ASSERT_TRUE(reopened);
    EXPECT_EQ(kNewPacket, MetadataOf(reopened.get()));
  }
}

TEST_F(EPDFCatalogEmbedderTest, SetMetadataWithoutDataRemovesThePacket) {
  const std::string input = PdfWithPacket();
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    EXPECT_FALSE(EPDFCatalog_SetMetadata(opened.doc.get(), nullptr, 5));
    EXPECT_EQ(kOldPacket, MetadataOf(opened.doc.get()));

    ASSERT_NO_FATAL_FAILURE(SetPacket(opened.doc.get(), layer, nullptr));
    EXPECT_EQ(0u, EPDFCatalog_GetMetadata(opened.doc.get(), nullptr, 0));
    EXPECT_EQ(0u, EntryOf(opened.doc.get()).objnum);
    // Removing what isn't there is no failure.
    ASSERT_NO_FATAL_FAILURE(SetPacket(opened.doc.get(), layer, nullptr));

    ClearString();
    ASSERT_TRUE(
        FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_NO_INCREMENTAL));
    EXPECT_EQ(std::string::npos, GetString().find(Hex(kOldPacket)));
    ScopedSavedDoc reopened = OpenScopedSavedDocument();
    ASSERT_TRUE(reopened);
    EXPECT_EQ(0u, EPDFCatalog_GetMetadata(reopened.get(), nullptr, 0));
  }
}
