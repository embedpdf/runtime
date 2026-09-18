// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <iterator>
#include <string>
#include <vector>

#include "core/fpdfapi/edit/cpdf_stringarchivestream.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_base_document.h"
#include "core/fpdfapi/parser/cpdf_cross_ref_table.h"
#include "core/fpdfapi/parser/cpdf_crypto_handler.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_security_handler.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fxcrt/fx_string.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_ppo.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/embedder_test_constants.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gmock/include/gmock/gmock-matchers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

using testing::HasSubstr;
using testing::Not;
using testing::StartsWith;

namespace {

class CountingFileAccess final : public FPDF_FILEACCESS {
 public:
  explicit CountingFileAccess(const std::string& bytes) : bytes_(bytes) {
    m_FileLen = bytes_.size();
    m_Param = this;
    m_GetBlock = [](void* context, unsigned long position,
                    unsigned char* buffer, unsigned long size) {
      auto* self = static_cast<CountingFileAccess*>(context);
      if (position > self->bytes_.size() ||
          size > self->bytes_.size() - position) {
        return 0;
      }
      ++self->read_count_;
      // SAFETY: FPDF_FILEACCESS supplies a writable buffer of `size` bytes.
      auto destination = UNSAFE_BUFFERS(pdfium::span(buffer, size));
      destination.copy_from(
          pdfium::as_byte_span(self->bytes_).subspan(position, size));
      return 1;
    };
  }

  size_t read_count() const { return read_count_; }

 private:
  const std::string& bytes_;
  size_t read_count_ = 0;
};

std::string SerializeObject(const CPDF_Object* object) {
  fxcrt::ostringstream output;
  CPDF_StringArchiveStream archive(&output);
  EXPECT_TRUE(object->WriteTo(&archive, nullptr));
  const auto bytes = output.str();
  return std::string(bytes.begin(), bytes.end());
}

// Keep the generations in the bytes: CPDF_Reference intentionally stores only
// an object number, so constructing an in-memory document would miss this bug.
std::string MakeGenerationDocument(uint16_t generation,
                                   bool xref_stream,
                                   CPDF_Parser* encryption_source = nullptr) {
  const std::string gen = std::to_string(generation);
  std::vector<std::string> objects = {
      "<</Type /Catalog /Pages 2 " + gen + " R>>",
      "<</Type /Pages /Count 1 /Kids [3 " + gen + " R]>>",
      "<</Type /Page /Parent 2 " + gen +
          " R /MediaBox [0 0 200 200] /Contents 4 " + gen + " R>>",
      "<</Length 0>>stream\n\nendstream", "<</Title (original)>>"};
  std::string trailer = "/Root 1 " + gen + " R /Info 5 " + gen + " R";
  if (encryption_source) {
    auto* crypto = encryption_source->GetSecurityHandler()->GetCryptoHandler();
    const std::string title = "original";
    auto encrypted_title =
        crypto->EncryptContent(5, generation, pdfium::as_byte_span(title));
    auto title_object = pdfium::MakeRetain<CPDF_String>(
        nullptr, encrypted_title, CPDF_String::DataType::kIsHex);
    objects[4] = "<</Title " + SerializeObject(title_object.Get()) + ">>";
    objects.push_back(
        SerializeObject(encryption_source->GetEncryptDict().Get()));
    trailer += " /Encrypt 6 " + gen + " R /ID " +
               SerializeObject(encryption_source->GetIDArray().Get());
  }
  std::string pdf = "%PDF-1.7\n";
  std::vector<size_t> offsets(objects.size() + (xref_stream ? 2 : 1));
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets[i + 1] = pdf.size();
    pdf += std::to_string(i + 1) + " " + gen + " obj\n" + objects[i] +
           "\nendobj\n";
  }
  const size_t xref_offset = pdf.size();
  if (xref_stream) {
    const size_t xref_number = objects.size() + 1;
    offsets[xref_number] = xref_offset;
    std::string entries;
    for (size_t i = 0; i < offsets.size(); ++i) {
      entries.push_back(i == 0 ? 0 : 1);
      for (int shift = 24; shift >= 0; shift -= 8) {
        entries.push_back(static_cast<char>(offsets[i] >> shift));
      }
      const uint16_t entry_gen = i == 0             ? 65535
                                 : i == xref_number ? 0
                                                    : generation;
      entries.push_back(static_cast<char>(entry_gen >> 8));
      entries.push_back(static_cast<char>(entry_gen));
    }
    pdf += std::to_string(xref_number) + " 0 obj\n<</Type /XRef /Size " +
           std::to_string(offsets.size()) + " /W [1 4 2] /Length " +
           std::to_string(entries.size()) + " " + trailer + ">>stream\n" +
           entries + "\nendstream\nendobj\n";
  } else {
    pdf += "xref\n0 " + std::to_string(offsets.size()) +
           "\n0000000000 65535 f\r\n";
    for (size_t i = 1; i <= objects.size(); ++i) {
      pdf += ByteString::Format("%010zu %05u n\r\n", offsets[i], generation)
                 .c_str();
    }
    pdf += "trailer\n<</Size " + std::to_string(offsets.size()) + " " +
           trailer + ">>\n";
  }
  return pdf + "startxref\n" + std::to_string(xref_offset) + "\n%%EOF\n";
}

bool HasSavedXRefEntryForObject(FPDF_DOCUMENT document, uint32_t objnum) {
  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(document);
  if (!cpdf_doc || !cpdf_doc->GetParser() ||
      !cpdf_doc->GetParser()->GetCrossRefTable()) {
    return false;
  }

  const CPDF_CrossRefTable::ObjectInfo* info =
      cpdf_doc->GetParser()->GetCrossRefTable()->GetObjectInfo(objnum);
  return info && (info->type == CPDF_CrossRefTable::ObjectType::kNormal ||
                  info->type == CPDF_CrossRefTable::ObjectType::kCompressed);
}

}  // namespace

class FPDFSaveEmbedderTest : public EmbedderTest {};

TEST_F(FPDFSaveEmbedderTest, SavePreservesConsistentObjectGenerations) {
  for (uint16_t generation : {0, 1, 256, 65534}) {
    for (bool xref_stream : {false, true}) {
      for (unsigned long flags : {FPDF_INCREMENTAL, FPDF_NO_INCREMENTAL}) {
        SCOPED_TRACE(testing::Message()
                     << generation << ":" << xref_stream << ":" << flags);
        const std::string input =
            MakeGenerationDocument(generation, xref_stream);
        ScopedFPDFDocument source(
            FPDF_LoadMemDocument(input.data(), input.size(), nullptr));
        ASSERT_TRUE(source);
        auto* doc = CPDFDocumentFromFPDFDocument(source.get());
        ASSERT_FALSE(doc->GetParser()->xref_table_rebuilt());
        auto page = doc->GetMutablePageDictionary(0);
        ASSERT_TRUE(page);
        page->SetNewFor<CPDF_Number>("Rotate", 90);
        const uint32_t live_generation = page->GetGenNum();

        ClearString();
        ASSERT_TRUE(FPDF_SaveAsCopy(source.get(), this, flags));
        EXPECT_EQ(live_generation, page->GetGenNum());
        const bool incremental = flags == FPDF_INCREMENTAL;
        const uint16_t saved_generation = incremental ? generation : 0;
        const std::string revision =
            incremental ? GetString().substr(input.size()) : GetString();
        if (incremental) {
          EXPECT_EQ(input, GetString().substr(0, input.size()));
        }
        EXPECT_THAT(
            revision,
            HasSubstr("/Root 1 " + std::to_string(saved_generation) + " R"));
        EXPECT_THAT(
            revision,
            HasSubstr("/Parent 2 " + std::to_string(saved_generation) + " R"));
        EXPECT_THAT(
            revision,
            HasSubstr("3 " + std::to_string(saved_generation) + " obj"));

        ScopedFPDFDocument saved(FPDF_LoadMemDocument(
            GetString().data(), GetString().size(), nullptr));
        ASSERT_TRUE(saved);
        auto* saved_doc = CPDFDocumentFromFPDFDocument(saved.get());
        auto* parser = saved_doc->GetParser();
        EXPECT_FALSE(parser->xref_table_rebuilt());
        EXPECT_EQ(saved_generation,
                  parser->GetCrossRefTable()->GetObjectInfo(3)->gennum);
        EXPECT_EQ(90, saved_doc->GetPageDictionary(0)->GetIntegerFor("Rotate"));
        if (incremental && xref_stream) {
          const auto* self = parser->GetCrossRefTable()->GetObjectInfo(
              parser->GetTrailerObjectNumber());
          ASSERT_TRUE(self);
          EXPECT_EQ(CPDF_CrossRefTable::ObjectType::kNormal, self->type);
          EXPECT_EQ(0, self->gennum);
          EXPECT_EQ(parser->GetLastXRefOffset(), self->pos);
        }
      }
    }
  }
}

TEST_F(FPDFSaveEmbedderTest, IncrementalSaveDoesNotPopulateDocumentCaches) {
  ASSERT_TRUE(OpenDocument("annotation_stamp_with_ap.pdf"));
  auto* doc = CPDFDocumentFromFPDFDocument(document());
  auto page = doc->GetMutablePageDictionary(0);
  page->SetNewFor<CPDF_Number>("Rotate", 90);
  auto parent = page->GetMutableDictFor("Parent");
  ASSERT_TRUE(parent);
  parent->SetNewFor<CPDF_Number>("SaveProbe", 1);
  ASSERT_EQ(CPDF_CrossRefTable::ObjectType::kCompressed,
            doc->GetParser()
                ->GetCrossRefTable()
                ->GetObjectInfo(parent->GetObjNum())
                ->type);
  const auto object_count = std::distance(doc->begin(), doc->end());
  const size_t stream_count =
      doc->GetParser()->GetCachedObjectStreamCountForTesting();

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_INCREMENTAL));
  EXPECT_EQ(object_count, std::distance(doc->begin(), doc->end()));
  EXPECT_EQ(stream_count,
            doc->GetParser()->GetCachedObjectStreamCountForTesting());

  ScopedSavedDoc saved = OpenScopedSavedDocument();
  ASSERT_TRUE(saved);
  auto* saved_doc = CPDFDocumentFromFPDFDocument(saved.get());
  auto saved_page = saved_doc->GetPageDictionary(0);
  EXPECT_EQ(0u, saved_page->GetGenNum());
  EXPECT_EQ(90, saved_page->GetIntegerFor("Rotate"));
  auto saved_parent = saved_page->GetDictFor("Parent");
  EXPECT_EQ(0u, saved_parent->GetGenNum());
  EXPECT_EQ(1, saved_parent->GetIntegerFor("SaveProbe"));
  EXPECT_EQ(CPDF_CrossRefTable::ObjectType::kNormal,
            saved_doc->GetParser()
                ->GetCrossRefTable()
                ->GetObjectInfo(parent->GetObjNum())
                ->type);
}

TEST_F(FPDFSaveEmbedderTest, IncrementalSavePreservesDeclaredXrefSize) {
  std::string input = MakeGenerationDocument(1, false);
  const auto size_position = input.find("/Size 6");
  ASSERT_NE(std::string::npos, size_position);
  input.replace(size_position, 7, "/Size 20");
  ScopedFPDFDocument source(
      FPDF_LoadMemDocument(input.data(), input.size(), nullptr));
  ASSERT_TRUE(source);
  auto* doc = CPDFDocumentFromFPDFDocument(source.get());
  ASSERT_FALSE(doc->GetParser()->xref_table_rebuilt());
  doc->GetMutablePageDictionary(0)->SetNewFor<CPDF_Number>("Rotate", 90);

  ASSERT_TRUE(FPDF_SaveAsCopy(source.get(), this, FPDF_INCREMENTAL));
  EXPECT_THAT(GetString().substr(input.size()), HasSubstr("/Size 20"));
}

TEST_F(FPDFSaveEmbedderTest,
       EncryptedSaveUsesObjectGenerationForStringsAndStreams) {
  const std::pair<const char*, const char*> fixtures[] = {
      {"encrypted_hello_world_r3.pdf", "\xc3\xa2ge"},
      {"encrypted.pdf", "1234"},
      {"encrypted_hello_world_r6.pdf", "\xc3\xa2ge"}};
  for (const auto& [filename, password] : fixtures) {
    const std::string path = PathService::GetTestFilePath(filename);
    ScopedFPDFDocument seed(FPDF_LoadDocument(path.c_str(), password));
    ASSERT_TRUE(seed);
    for (bool xref_stream : {false, true}) {
      const std::string input = MakeGenerationDocument(
          256, xref_stream,
          CPDFDocumentFromFPDFDocument(seed.get())->GetParser());
      for (unsigned long flags : {FPDF_INCREMENTAL, FPDF_NO_INCREMENTAL}) {
        SCOPED_TRACE(testing::Message()
                     << filename << ":" << xref_stream << ":" << flags);
        ScopedFPDFDocument source(
            FPDF_LoadMemDocument(input.data(), input.size(), password));
        ASSERT_TRUE(source);
        auto* doc = CPDFDocumentFromFPDFDocument(source.get());
        ASSERT_EQ("original", doc->GetInfo()->GetByteStringFor("Title"));
        doc->GetMutableInfo()->SetNewFor<CPDF_String>("Title", "edited");
        auto stream = ToStream(doc->GetMutableIndirectObject(4));
        ASSERT_TRUE(stream);
        const std::string content = "0 0 20 20 re f\n";
        stream->SetData(pdfium::as_byte_span(content));

        ClearString();
        ASSERT_TRUE(FPDF_SaveAsCopy(source.get(), this, flags));
        ScopedFPDFDocument saved(FPDF_LoadMemDocument(
            GetString().data(), GetString().size(), password));
        ASSERT_TRUE(saved);
        auto* saved_doc = CPDFDocumentFromFPDFDocument(saved.get());
        EXPECT_FALSE(saved_doc->GetParser()->xref_table_rebuilt());
        EXPECT_EQ("edited", saved_doc->GetInfo()->GetByteStringFor("Title"));
        auto saved_stream = ToStream(saved_doc->GetOrParseIndirectObject(4));
        ASSERT_TRUE(saved_stream);
        EXPECT_EQ(L"0 0 20 20 re f\n", saved_stream->GetUnicodeText());
        EXPECT_EQ(256u, stream->GetGenNum());
      }
    }
  }
}

TEST_F(FPDFSaveEmbedderTest, IncrementalSavePreservesSharedBaseAndSibling) {
  const std::string input = MakeGenerationDocument(256, true);
  CountingFileAccess access(input);
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&access, nullptr);
  ASSERT_TRUE(base);
  ScopedFPDFDocument first(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
  ScopedFPDFDocument sibling(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
  EPDF_ReleaseBaseDocument(base);
  ASSERT_TRUE(first);
  ASSERT_TRUE(sibling);
  auto* doc = CPDF_LayerDocument::FromDocument(
      CPDFDocumentFromFPDFDocument(first.get()));
  auto* other = CPDFDocumentFromFPDFDocument(sibling.get());
  auto base_page = other->GetPageDictionary(0);
  auto page = doc->GetMutablePageDictionary(0);
  ASSERT_TRUE(base_page->IsFrozen());
  page->SetNewFor<CPDF_Number>("Rotate", 90);

  auto* base_doc = doc->GetBaseDocument();
  const auto object_count = std::distance(base_doc->begin(), base_doc->end());
  const auto stream_count =
      base_doc->GetParser()->GetCachedObjectStreamCountForTesting();
  const auto promoted_count = doc->GetPromotedObjectCount();
  const auto epoch = doc->GetOverlayEpoch();

  for (int pass = 0; pass < 2; ++pass) {
    ClearString();
    const size_t reads_before_save = access.read_count();
    ASSERT_TRUE(EPDFLayer_SaveDelta(first.get(), this, nullptr));
    // The edited page and its ancestors are cached. Proving reachability must
    // not open unrelated objects (including the still-unloaded /Info).
    EXPECT_EQ(reads_before_save, access.read_count());
    EXPECT_THAT(GetString(), HasSubstr("3 256 obj"));
    EXPECT_THAT(GetString(), HasSubstr("/Root 1 256 R"));
    const std::string combined = input + GetString();
    ScopedFPDFDocument saved(
        FPDF_LoadMemDocument(combined.data(), combined.size(), nullptr));
    ASSERT_TRUE(saved);
    auto* saved_doc = CPDFDocumentFromFPDFDocument(saved.get());
    EXPECT_FALSE(saved_doc->GetParser()->xref_table_rebuilt());
    EXPECT_EQ(90, saved_doc->GetPageDictionary(0)->GetIntegerFor("Rotate"));

    EXPECT_EQ(object_count, std::distance(base_doc->begin(), base_doc->end()));
    EXPECT_EQ(stream_count,
              base_doc->GetParser()->GetCachedObjectStreamCountForTesting());
    EXPECT_EQ(promoted_count, doc->GetPromotedObjectCount());
    EXPECT_EQ(epoch, doc->GetOverlayEpoch());
    EXPECT_EQ(256u, base_page->GetGenNum());
    EXPECT_FALSE(base_page->KeyExist("Rotate"));
    EXPECT_EQ(base_page, other->GetPageDictionary(0));

    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(sibling.get(), this, FPDF_INCREMENTAL));
    EXPECT_EQ(input, GetString());
  }
}

TEST_F(FPDFSaveEmbedderTest, LayerSavePreservesReferenceGenerationChanges) {
  const std::string input = MakeGenerationDocument(256, false);
  EPDF_BASE_DOCUMENT base =
      EPDF_LoadMemBaseDocument(input.data(), input.size(), nullptr);
  ASSERT_TRUE(base);
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
  EPDF_ReleaseBaseDocument(base);
  ASSERT_TRUE(layer);
  auto* doc = CPDFDocumentFromFPDFDocument(layer.get());
  // Model a delta that reuses an object number at a newer generation and
  // updates its referring page-tree array. References themselves keep only
  // the object number, so a value-only comparison would elide that array.
  auto page = doc->GetMutablePageDictionary(0);
  page->SetGenNum(257);
  ASSERT_TRUE(doc->GetMutableIndirectObject(2));

  ASSERT_TRUE(EPDFLayer_SaveDelta(layer.get(), this, nullptr));
  EXPECT_THAT(GetString(), HasSubstr("2 256 obj"));
  EXPECT_THAT(GetString(), HasSubstr("3 257 obj"));
  EXPECT_THAT(GetString(), HasSubstr(" 3 257 R "));
  const std::string combined = input + GetString();
  ScopedFPDFDocument saved(
      FPDF_LoadMemDocument(combined.data(), combined.size(), nullptr));
  ASSERT_TRUE(saved);
  auto* saved_doc = CPDFDocumentFromFPDFDocument(saved.get());
  EXPECT_FALSE(saved_doc->GetParser()->xref_table_rebuilt());
  EXPECT_EQ(257u, saved_doc->GetPageDictionary(0)->GetGenNum());
}

TEST_F(FPDFSaveEmbedderTest, FullRewriteDoesNotPopulateDocumentCaches) {
  ASSERT_TRUE(OpenDocument("annotation_stamp_with_ap.pdf"));
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  doc->GetInfo();
  const auto object_count = std::distance(doc->begin(), doc->end());
  const size_t stream_count =
      doc->GetParser()->GetCachedObjectStreamCountForTesting();

  for (int pass = 0; pass < 2; ++pass) {
    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_NO_INCREMENTAL));
    EXPECT_EQ(object_count, std::distance(doc->begin(), doc->end()));
    EXPECT_EQ(stream_count,
              doc->GetParser()->GetCachedObjectStreamCountForTesting());

    ScopedSavedDoc saved = OpenScopedSavedDocument();
    ASSERT_TRUE(saved);
    EXPECT_EQ(FPDF_GetPageCount(document()), FPDF_GetPageCount(saved.get()));
    ScopedSavedPage page = LoadScopedSavedPage(0);
    ASSERT_TRUE(page);
    EXPECT_TRUE(RenderSavedPageWithFlags(page.get(), FPDF_ANNOT));
  }
}

TEST_F(FPDFSaveEmbedderTest, FullRewriteDoesNotPopulateSharedBaseCaches) {
  FileAccessForTesting access("annotation_stamp_with_ap.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&access, nullptr);
  ASSERT_TRUE(base);
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
  // The layer retains the base, including on an assertion failure below.
  EPDF_ReleaseBaseDocument(base);
  ASSERT_TRUE(layer);

  auto* doc = CPDF_LayerDocument::FromDocument(
      CPDFDocumentFromFPDFDocument(layer.get()));
  ASSERT_TRUE(doc);
  doc->GetInfo();
  auto* base_doc = doc->GetBaseDocument();
  const auto object_count = std::distance(base_doc->begin(), base_doc->end());
  const size_t stream_count =
      base_doc->GetParser()->GetCachedObjectStreamCountForTesting();
  const size_t promoted_count = doc->GetPromotedObjectCount();

  ASSERT_TRUE(FPDF_SaveAsCopy(layer.get(), this, FPDF_NO_INCREMENTAL));
  EXPECT_EQ(object_count, std::distance(base_doc->begin(), base_doc->end()));
  EXPECT_EQ(stream_count,
            base_doc->GetParser()->GetCachedObjectStreamCountForTesting());
  EXPECT_EQ(promoted_count, doc->GetPromotedObjectCount());

  ScopedSavedDoc saved = OpenScopedSavedDocument();
  ASSERT_TRUE(saved);
  EXPECT_EQ(FPDF_GetPageCount(layer.get()), FPDF_GetPageCount(saved.get()));
}

TEST_F(FPDFSaveEmbedderTest, FullRewritePreservesLiveAnnotationAcrossSaves) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annotation(
      EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_SQUARE));
  ASSERT_TRUE(annotation);

  for (int pass = 0; pass < 2; ++pass) {
    const FS_RECTF rect = {20.0f + pass * 10, 120.0f, 120.0f, 20.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annotation.get(), &rect));
    const std::string expected_render =
        HashBitmap(RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT).get());
    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_NO_INCREMENTAL));

    FS_RECTF live_rect;
    ASSERT_TRUE(FPDFAnnot_GetRect(annotation.get(), &live_rect));
    EXPECT_EQ(rect.left, live_rect.left);
    EXPECT_EQ(
        expected_render,
        HashBitmap(RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT).get()));

    ScopedSavedDoc saved = OpenScopedSavedDocument();
    ASSERT_TRUE(saved);
    ScopedSavedPage saved_page = LoadScopedSavedPage(0);
    ASSERT_TRUE(saved_page);
    EXPECT_EQ(
        expected_render,
        HashBitmap(
            RenderSavedPageWithFlags(saved_page.get(), FPDF_ANNOT).get()));
    ASSERT_EQ(1, FPDFPage_GetAnnotCount(saved_page.get()));
    ScopedFPDFAnnotation saved_annotation(
        FPDFPage_GetAnnot(saved_page.get(), 0));
    FS_RECTF saved_rect;
    ASSERT_TRUE(FPDFAnnot_GetRect(saved_annotation.get(), &saved_rect));
    EXPECT_EQ(rect.left, saved_rect.left);
    EXPECT_EQ(rect.top, saved_rect.top);
    EXPECT_EQ(rect.right, saved_rect.right);
    EXPECT_EQ(rect.bottom, saved_rect.bottom);
  }
}

TEST_F(FPDFSaveEmbedderTest, SaveSimpleDoc) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.7\r\n"));
  EXPECT_EQ(805u, GetString().size());
}

TEST_F(FPDFSaveEmbedderTest, SaveSimpleDocWithVersion) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_TRUE(FPDF_SaveWithVersion(document(), this, 0, 14));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.4\r\n"));
  EXPECT_EQ(805u, GetString().size());
}

TEST_F(FPDFSaveEmbedderTest, SaveSimpleDocToOwnedBuffer) {
  EPDF_FreeBuffer(nullptr);

  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  unsigned long size = 0;
  void* buffer = EPDF_SaveDocumentToOwnedBuffer(document(), 0, &size);
  ASSERT_TRUE(buffer);
  EXPECT_EQ(805u, size);
  std::string saved(static_cast<const char*>(buffer), size);
  EPDF_FreeBuffer(buffer);
  EXPECT_THAT(saved, StartsWith("%PDF-1.7\r\n"));

  size = 0;
  buffer = EPDF_SaveDocumentToOwnedBufferWithVersion(document(), 0, &size, 14);
  ASSERT_TRUE(buffer);
  EXPECT_EQ(805u, size);
  saved.assign(static_cast<const char*>(buffer), size);
  EPDF_FreeBuffer(buffer);
  EXPECT_THAT(saved, StartsWith("%PDF-1.4\r\n"));
}

TEST_F(FPDFSaveEmbedderTest, SaveSimpleDocWithBadVersion) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_TRUE(FPDF_SaveWithVersion(document(), this, 0, -1));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.7\r\n"));

  ClearString();
  EXPECT_TRUE(FPDF_SaveWithVersion(document(), this, 0, 0));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.7\r\n"));

  ClearString();
  EXPECT_TRUE(FPDF_SaveWithVersion(document(), this, 0, 18));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.7\r\n"));
}

TEST_F(FPDFSaveEmbedderTest, SaveSimpleDocIncremental) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_TRUE(FPDF_SaveWithVersion(document(), this, FPDF_INCREMENTAL, 14));
  // Version gets taken as-is from input document.
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.7\n%\xa0\xf2\xa4\xf4"));
  // EmbedPDF: an incremental save writes what changed. Nothing changed, so
  // no revision is appended - not the objects the parser happened to load
  // (upstream rewrote every one of them), not an empty cross-reference
  // section. The output is the loaded file, byte for byte.
  std::string file_path = PathService::GetTestFilePath("hello_world.pdf");
  ASSERT_FALSE(file_path.empty());
  std::vector<uint8_t> original = GetFileContents(file_path.c_str());
  ASSERT_FALSE(original.empty());
  EXPECT_EQ(std::string(original.begin(), original.end()), GetString());
}

TEST_F(FPDFSaveEmbedderTest, SaveAsCopyPrunesUnlinkedNewAnnotation) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(0, FPDFPage_GetAnnotCount(page.get()));

  ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
  ASSERT_TRUE(annot);
  const uint32_t annot_objnum = EPDFAnnot_GetObjectNumber(annot.get());
  ASSERT_GT(annot_objnum, 0u);
  ASSERT_EQ(1, FPDFPage_GetAnnotCount(page.get()));

  annot.reset();
  ASSERT_TRUE(FPDFPage_RemoveAnnot(page.get(), 0));
  ASSERT_EQ(0, FPDFPage_GetAnnotCount(page.get()));

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_doc);
  EXPECT_FALSE(HasSavedXRefEntryForObject(saved_doc.get(), annot_objnum));

  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(saved_page.get()));
}

TEST_F(FPDFSaveEmbedderTest, IncrementalSavePrunesUnlinkedNewAnnotation) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(0, FPDFPage_GetAnnotCount(page.get()));

  ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
  ASSERT_TRUE(annot);
  const uint32_t annot_objnum = EPDFAnnot_GetObjectNumber(annot.get());
  ASSERT_GT(annot_objnum, 0u);

  annot.reset();
  ASSERT_TRUE(FPDFPage_RemoveAnnot(page.get(), 0));
  ASSERT_EQ(0, FPDFPage_GetAnnotCount(page.get()));

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_INCREMENTAL));
  ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_doc);
  EXPECT_FALSE(HasSavedXRefEntryForObject(saved_doc.get(), annot_objnum));

  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(saved_page.get()));
}

TEST_F(FPDFSaveEmbedderTest, SaveAsCopyKeepsLinkedNewAnnotation) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(0, FPDFPage_GetAnnotCount(page.get()));

  ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
  ASSERT_TRUE(annot);
  const uint32_t annot_objnum = EPDFAnnot_GetObjectNumber(annot.get());
  ASSERT_GT(annot_objnum, 0u);
  ASSERT_EQ(1, FPDFPage_GetAnnotCount(page.get()));

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_doc);
  EXPECT_TRUE(HasSavedXRefEntryForObject(saved_doc.get(), annot_objnum));

  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);
  ASSERT_EQ(1, FPDFPage_GetAnnotCount(saved_page.get()));
  ScopedFPDFAnnotation saved_annot(FPDFPage_GetAnnot(saved_page.get(), 0));
  ASSERT_TRUE(saved_annot);
  EXPECT_EQ(FPDF_ANNOT_TEXT, FPDFAnnot_GetSubtype(saved_annot.get()));
}

TEST_F(FPDFSaveEmbedderTest, SetEncryptionRoundTripsWithInlineEncryptDict) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ASSERT_TRUE(EPDF_SetEncryption(document(), "user", "owner",
                                 EPDF_PERM_PRINT | EPDF_PERM_COPY));

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ScopedSavedDoc saved_doc = OpenScopedSavedDocumentWithPassword("user");
  ASSERT_TRUE(saved_doc);
  EXPECT_TRUE(EPDF_IsEncrypted(saved_doc.get()));

  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);
  ScopedFPDFBitmap bitmap = RenderSavedPage(saved_page.get());
  ASSERT_TRUE(bitmap);
}

TEST_F(FPDFSaveEmbedderTest, RemoveEncryptionUsesDocumentOwnedPendingState) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ASSERT_TRUE(EPDF_SetEncryption(document(), "user", "owner",
                                 EPDF_PERM_PRINT | EPDF_PERM_COPY));
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  {
    ScopedSavedDoc encrypted_doc = OpenScopedSavedDocumentWithPassword("user");
    ASSERT_TRUE(encrypted_doc);
    ASSERT_TRUE(EPDF_IsEncrypted(encrypted_doc.get()));

    ClearString();
    ASSERT_TRUE(EPDF_RemoveEncryption(encrypted_doc.get()));
    ASSERT_TRUE(FPDF_SaveAsCopy(encrypted_doc.get(), this, 0));
  }

  ScopedSavedDoc decrypted_doc = OpenScopedSavedDocument();
  ASSERT_TRUE(decrypted_doc);
  EXPECT_FALSE(EPDF_IsEncrypted(decrypted_doc.get()));
}

TEST_F(FPDFSaveEmbedderTest, SaveSimpleDocNoIncremental) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_TRUE(FPDF_SaveWithVersion(document(), this, FPDF_NO_INCREMENTAL, 14));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.4\r\n"));
  EXPECT_EQ(805u, GetString().size());
}

TEST_F(FPDFSaveEmbedderTest, SaveSimpleDocRemoveSecurityDeprecated) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_TRUE(FPDF_SaveWithVersion(document(), this,
                                   FPDF_REMOVE_SECURITY_DEPRECATED, 14));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.4\r\n"));
  EXPECT_EQ(805u, GetString().size());
}

TEST_F(FPDFSaveEmbedderTest, SaveSimpleDocRemoveSecurity) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_TRUE(FPDF_SaveWithVersion(document(), this, FPDF_REMOVE_SECURITY, 14));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.4\r\n"));
  EXPECT_EQ(805u, GetString().size());
}

TEST_F(FPDFSaveEmbedderTest, SaveSimpleDocBadFlags) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_TRUE(FPDF_SaveWithVersion(document(), this, 999999, 14));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.4\r\n"));
  EXPECT_EQ(805u, GetString().size());
}

TEST_F(FPDFSaveEmbedderTest, SaveCopiedDoc) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));

  ScopedPage page = LoadScopedPage(0);
  EXPECT_TRUE(page);

  ScopedFPDFDocument output_doc(FPDF_CreateNewDocument());
  EXPECT_TRUE(output_doc);
  EXPECT_TRUE(FPDF_ImportPages(output_doc.get(), document(), "1", 0));
  EXPECT_TRUE(FPDF_SaveAsCopy(output_doc.get(), this, 0));
}

TEST_F(FPDFSaveEmbedderTest, Bug42271133) {
  ASSERT_TRUE(OpenDocument("bug_42271133.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // Arbitrarily remove the first page object.
  auto text_object = FPDFPage_GetObject(page.get(), 0);
  ASSERT_TRUE(text_object);
  ASSERT_TRUE(FPDFPage_RemoveObject(page.get(), text_object));
  FPDFPageObj_Destroy(text_object);

  // Regenerate dirty stream and save the document.
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  // Reload saved document.
  ScopedSavedDoc saved_document = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_document);
  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);

  // Assert path fill color is not changed to black.
  auto path_obj = FPDFPage_GetObject(saved_page.get(), 0);
  ASSERT_TRUE(path_obj);
  unsigned int r;
  unsigned int g;
  unsigned int b;
  unsigned int a;
  ASSERT_TRUE(FPDFPageObj_GetFillColor(path_obj, &r, &g, &b, &a));
  EXPECT_EQ(180u, r);
  EXPECT_EQ(180u, g);
  EXPECT_EQ(180u, b);
}

TEST_F(FPDFSaveEmbedderTest, SaveLinearizedDoc) {
  const int kPageCount = 3;
  std::array<std::string, kPageCount> original_md5;

  ASSERT_TRUE(OpenDocument("linearized.pdf"));
  for (int i = 0; i < kPageCount; ++i) {
    ScopedPage page = LoadScopedPage(i);
    ASSERT_TRUE(page);
    ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
    EXPECT_EQ(612, FPDFBitmap_GetWidth(bitmap.get()));
    EXPECT_EQ(792, FPDFBitmap_GetHeight(bitmap.get()));
    original_md5[i] = HashBitmap(bitmap.get());
  }

  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.6\r\n"));
  EXPECT_THAT(GetString(), HasSubstr("/Root "));
  EXPECT_THAT(GetString(), HasSubstr("/Info "));
  EXPECT_THAT(GetString(), HasSubstr("/Size 37"));
  EXPECT_THAT(GetString(), HasSubstr("35 0 obj"));
  EXPECT_THAT(GetString(), HasSubstr("36 0 obj"));
  EXPECT_THAT(GetString(), Not(HasSubstr("37 0 obj")));
  EXPECT_THAT(GetString(), Not(HasSubstr("38 0 obj")));
  EXPECT_EQ(7986u, GetString().size());

  // Make sure new document renders the same as the old one.
  ScopedSavedDoc saved_document = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_document);
  for (int i = 0; i < kPageCount; ++i) {
    ScopedSavedPage page = LoadScopedSavedPage(i);
    ASSERT_TRUE(page);
    ScopedFPDFBitmap bitmap = RenderSavedPage(page.get());
    EXPECT_EQ(original_md5[i], HashBitmap(bitmap.get()));
  }
}

TEST_F(FPDFSaveEmbedderTest, Bug1409) {
  ASSERT_TRUE(OpenDocument("jpx_lzw.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  while (FPDFPage_CountObjects(page.get()) > 0) {
    ScopedFPDFPageObject object(FPDFPage_GetObject(page.get(), 0));
    ASSERT_TRUE(object);
    ASSERT_TRUE(FPDFPage_RemoveObject(page.get(), object.get()));
  }
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  // The new document should render as empty.
  ScopedSavedDoc saved_document = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_document);
  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);
  ScopedFPDFBitmap bitmap = RenderSavedPage(saved_page.get());
  CompareBitmap(bitmap.get(), pdfium::kBlankPage612By792Png);

  EXPECT_THAT(GetString(), StartsWith("%PDF-1.7\r\n"));
  EXPECT_THAT(GetString(), HasSubstr("/Root "));
  EXPECT_THAT(GetString(), Not(HasSubstr("/Image")));
  EXPECT_LT(GetString().size(), 600u);
}

#ifdef PDF_ENABLE_XFA
TEST_F(FPDFSaveEmbedderTest, SaveXFADoc) {
  ASSERT_TRUE(OpenDocument("simple_xfa.pdf"));
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.7\r\n"));
  ASSERT_TRUE(OpenSavedDocument());
  // TODO(tsepez): check for XFA forms in document
  CloseSavedDocument();
}
#endif  // PDF_ENABLE_XFA

TEST_F(FPDFSaveEmbedderTest, Bug342) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  EXPECT_THAT(GetString(), HasSubstr("0000000000 65535 f\r\n"));
  EXPECT_THAT(GetString(), Not(HasSubstr("0000000000 65536 f\r\n")));
}

TEST_F(FPDFSaveEmbedderTest, Bug905142) {
  ASSERT_TRUE(OpenDocument("bug_905142.pdf"));
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  EXPECT_THAT(GetString(), HasSubstr("/Length 0"));
}

// Should not trigger a DCHECK() failure in CFX_FileBufferArchive.
// Fails because the PDF is malformed.
TEST_F(FPDFSaveEmbedderTest, Bug1328389) {
  ASSERT_TRUE(OpenDocument("bug_1328389.pdf"));
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  EXPECT_THAT(GetString(), HasSubstr("/Foo/"));
}

TEST_F(FPDFSaveEmbedderTest, IncrementalSaveWithModifications) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));

  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // Get the original bitmap for comparison
  ScopedFPDFBitmap original_bitmap = RenderLoadedPage(page.get());
  std::string original_md5 = HashBitmap(original_bitmap.get());

  // Count text objects on a page
  auto count_text_objects = [](FPDF_PAGE page) {
    int object_count = FPDFPage_CountObjects(page);
    int text_count = 0;
    for (int i = 0; i < object_count; ++i) {
      FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
      if (FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_TEXT) {
        ++text_count;
      }
    }
    return text_count;
  };

  // Verify the original PDF does not have any text objects
  EXPECT_EQ(0, count_text_objects(page.get()));

  // Add a new text object to modify the page.
  ScopedFPDFPageObject text_object(
      FPDFPageObj_NewTextObj(document(), "Arial", 12.0f));
  ScopedFPDFWideString text = GetFPDFWideString(L"Test Incremental Save");
  FPDFText_SetText(text_object.get(), text.get());
  FPDFPageObj_Transform(text_object.get(), 1, 0, 0, 1, 100, 100);
  FPDFPage_InsertObject(page.get(), text_object.release());
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_INCREMENTAL));

  // Verify the saved document
  // Count occurrences of key markers
  auto count_occurrences = [](const std::string& str,
                              const std::string& substr) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = str.find(substr, pos)) != std::string::npos) {
      ++count;
      pos += substr.size();
    }
    return count;
  };

  // Should contain incremental save markers (original + incremental)
  std::string saved_content = GetString();
  EXPECT_EQ(2u, count_occurrences(saved_content, "trailer"));
  // In incremental PDF saving, /Prev points to the previous xref table's
  // offset. Since we're doing only one incremental save operation, there's only
  // one /Prev entry pointing to the original PDF's xref table.
  EXPECT_EQ(1u, count_occurrences(saved_content, "/Prev"));
  EXPECT_EQ(2u, count_occurrences(saved_content, "startxref"));
  EXPECT_EQ(2u, count_occurrences(saved_content, "%%EOF"));

  // Load the saved document and verify the modification is visible
  ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_doc);
  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);

  // The rendered output should be different from the original
  ScopedFPDFBitmap saved_bitmap = RenderSavedPage(saved_page.get());
  std::string saved_md5 = HashBitmap(saved_bitmap.get());
  EXPECT_NE(original_md5, saved_md5);

  // Verify the text object exists after the save
  EXPECT_EQ(1, count_text_objects(saved_page.get()));
}

class FPDFSaveWithFontSubsetEmbedderTest : public FPDFSaveEmbedderTest {
 public:
  static constexpr char kSaveNewTextFilename[] = "save_new_text";

  ScopedFPDFFont LoadTestFont() {
    std::string font_path = PathService::GetThirdPartyFilePath(
        "NotoSansCJK/NotoSansSC-Regular.subset.otf");
    if (font_path.empty()) {
      return nullptr;
    }

    std::vector<uint8_t> font_data = GetFileContents(font_path.c_str());
    if (font_data.empty()) {
      return nullptr;
    }

    return ScopedFPDFFont(FPDFText_LoadFont(
        document(), font_data.data(), font_data.size(), FPDF_FONT_TRUETYPE,
        /*cid=*/true));
  }

  void InsertNewTextObject(const FPDF_PAGE& page) {
    ScopedFPDFFont font = LoadTestFont();
    ASSERT_TRUE(font);

    FPDF_PAGEOBJECT text_obj =
        FPDFPageObj_CreateTextObj(document(), font.get(), 24.0f);

    // `text` only contains a subset of the characters in the test font.
    ScopedFPDFWideString text = GetFPDFWideString(L"这是第一句。");
    ASSERT_TRUE(FPDFText_SetText(text_obj, text.get()));

    const FS_MATRIX matrix{1, 0, 0, 1, 10, 10};
    ASSERT_TRUE(FPDFPageObj_TransformF(text_obj, &matrix));
    FPDFPage_InsertObject(page, text_obj);
    ASSERT_TRUE(FPDFPage_GenerateContent(page));
  }
};

TEST_F(FPDFSaveWithFontSubsetEmbedderTest, SaveWithSubsetWithoutNewText) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_SUBSET_NEW_FONTS));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.7\r\n"));
  EXPECT_EQ(805u, GetString().size());
}

TEST_F(FPDFSaveWithFontSubsetEmbedderTest, SaveWithoutSubsetWithNewText) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ASSERT_NO_FATAL_FAILURE(InsertNewTextObject(page.get()));

  ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
  CompareBitmapWithExpectationSuffix(bitmap.get(), kSaveNewTextFilename);

  // Verify the file grew enough to include the new text's font data without
  // depending on exact PDF serialization byte counts.
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.7\r\n"));
  EXPECT_GT(GetString().size(), 805u);
  EXPECT_LT(GetString().size(), 10000u);

  // Verify the text is visible.
  VerifySavedDocumentWithExpectationSuffix(kSaveNewTextFilename);
}

TEST_F(FPDFSaveWithFontSubsetEmbedderTest, SaveWithSubsetWithNewText) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ASSERT_NO_FATAL_FAILURE(InsertNewTextObject(page.get()));

  ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
  CompareBitmapWithExpectationSuffix(bitmap.get(), kSaveNewTextFilename);

  // Verify the file grew enough to include the new text's font data without
  // depending on exact PDF serialization byte counts.
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_SUBSET_NEW_FONTS));
  EXPECT_THAT(GetString(), StartsWith("%PDF-1.7\r\n"));
  // TODO(crbug.com/476127152): File size increase should be smaller.
  EXPECT_GT(GetString().size(), 805u);
  EXPECT_LT(GetString().size(), 10000u);

  // Verify the text is visible.
  VerifySavedDocumentWithExpectationSuffix(kSaveNewTextFilename);
}
