// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Saves (public/epdf_save.h): what a save writes, and what it leaves out.

#include "public/epdf_save.h"

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/fpdfapi/edit/cpdf_save_object_reader.h"
#include "core/fpdfapi/edit/cpdf_stringarchivestream.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_base_document.h"
#include "core/fpdfapi/parser/cpdf_cross_ref_table.h"
#include "core/fpdfapi/parser/cpdf_crypto_handler.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object_walker.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_security_handler.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_syntax_parser.h"
#include "core/fpdfapi/render/cpdf_docrenderdata.h"
#include "core/fxcrt/cfx_read_only_span_stream.h"
#include "core/fxcrt/data_vector.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_annot.h"
#include "public/epdf_layer.h"
#include "public/epdf_view.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gmock/include/gmock/gmock-matchers.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

using testing::HasSubstr;
using testing::StartsWith;

namespace {

// Whether `bytes` name a subset of `font`: a slash, six capital letters, a
// plus sign and the font's name (ISO 32000-2 9.9.2).
bool NamesASubsetOf(const std::string& bytes, const std::string& font) {
  const std::string suffix = "+" + font;
  for (size_t at = bytes.find(suffix); at != std::string::npos;
       at = bytes.find(suffix, at + 1)) {
    if (at < 7 || bytes[at - 7] != '/') {
      continue;
    }
    const std::string_view tag(bytes.data() + at - 6, 6);
    if (std::ranges::all_of(tag, [](char c) { return c >= 'A' && c <= 'Z'; })) {
      return true;
    }
  }
  return false;
}

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
          " R /MediaBox [0 0 200 200] /Resources <<>> /Contents 4 " + gen +
          " R>>",
      "<</Length 0>>stream\n\nendstream", "<</Title (original)>>"};
  std::string trailer = "/Root 1 " + gen + " R /Info 5 " + gen + " R";
  if (encryption_source) {
    auto* crypto = encryption_source->GetSecurityHandler()->GetCryptoHandler();
    const std::string title = "original";
    auto encrypted_title =
        crypto->EncryptContent(5, generation, pdfium::as_byte_span(title))
            .value();
    auto title_object = pdfium::MakeRetain<CPDF_String>(
        encrypted_title, CPDF_String::DataType::kIsHex);
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

// Preserve the on-disk reference generations: CPDF_Reference stores only the
// object number, so walking already parsed objects cannot verify these tokens.
void ExpectRawGenerationsConsistent(CPDF_Parser* parser) {
  CPDF_SyntaxParser syntax(parser->GetFileAccess());
  auto check_references = [&]() {
    std::vector<uint32_t> numbers;
    while (true) {
      const auto token = syntax.GetNextWord();
      if (token.word.IsEmpty() || token.word == "endobj" ||
          token.word == "stream" || token.word == "startxref") {
        break;
      }
      if (token.word == "(") {
        syntax.ReadString();
      } else if (token.word == "<") {
        syntax.ReadHexString();
      } else if (token.word == "R" && numbers.size() >= 2) {
        const uint32_t target = numbers[numbers.size() - 2];
        const uint32_t generation = numbers.back();
        const auto* info = parser->GetCrossRefTable()->GetObjectInfo(target);
        ASSERT_TRUE(info) << target;
        ASSERT_NE(CPDF_CrossRefTable::ObjectType::kFree, info->type);
        EXPECT_EQ(info->type == CPDF_CrossRefTable::ObjectType::kCompressed
                      ? 0u
                      : info->gennum,
                  generation)
            << target;
      }
      if (token.is_number) {
        uint32_t value = 0;
        const char* begin = token.word.c_str();
        const char* end = begin + token.word.GetLength();
        const auto result = std::from_chars(begin, end, value);
        if (result.ec == std::errc() && result.ptr == end) {
          numbers.push_back(value);
        } else {
          numbers.clear();
        }
      } else {
        numbers.clear();
      }
    }
  };
  for (const auto& [number, info] :
       parser->GetCrossRefTable()->objects_info()) {
    if (info.type != CPDF_CrossRefTable::ObjectType::kNormal) {
      continue;
    }
    syntax.SetPos(info.pos + parser->GetFileHeaderOffset());
    EXPECT_EQ(ByteString::Format("%u", number), syntax.GetNextWord().word);
    EXPECT_EQ(ByteString::Format("%u", info.gennum), syntax.GetNextWord().word);
    EXPECT_EQ("obj", syntax.GetNextWord().word);
    check_references();
  }
  if (!parser->IsXRefStream()) {
    syntax.SetPos(parser->GetLastXRefOffset() + parser->GetFileHeaderOffset());
    while (true) {
      const auto word = syntax.GetNextWord().word;
      ASSERT_FALSE(word.IsEmpty());
      if (word == "trailer") {
        break;
      }
    }
    check_references();
  }
}

void DumpSavedPdf(const std::string& bytes,
                  const char* password,
                  size_t original_size) {
  const char* directory = std::getenv("EPDF_SAVE_DUMP_DIR");
  if (!directory || !*directory) {
    return;
  }
  static size_t sequence = 0;
  const auto* test = testing::UnitTest::GetInstance()->current_test_info();
  const std::string path = std::string(directory) + "/" + test->name() + "-" +
                           std::to_string(++sequence);
  std::ofstream pdf(path + ".pdf", std::ios::binary);
  pdf.write(bytes.data(), bytes.size());
  ASSERT_TRUE(pdf.good());
  std::ofstream metadata(path + ".size");
  metadata << original_size;
  ASSERT_TRUE(metadata.good());
  if (password) {
    std::ofstream credentials(path + ".pw", std::ios::binary);
    credentials << password;
    ASSERT_TRUE(credentials.good());
  }
}

// Inspect the saved file independently of the creator's traversal. In-use
// entries and reachable objects are separate sets: reopening alone cannot
// detect an extra orphan written into the output.
struct SavedPdfModel {
  std::set<uint32_t> in_use;
  std::set<uint32_t> reachable;
  std::set<uint32_t> written;
  size_t encryption_dictionaries = 0;
  size_t stale_xref_streams = 0;
};

SavedPdfModel AnalyzeSavedPdf(const std::string& bytes,
                              const char* password = nullptr,
                              size_t original_size = 0) {
  SavedPdfModel model;
  ScopedFPDFDocument saved(
      FPDF_LoadMemDocument(bytes.data(), bytes.size(), password));
  EXPECT_TRUE(saved);
  if (!saved) {
    return model;
  }
  auto* doc = CPDFDocumentFromFPDFDocument(saved.get());
  auto* parser = doc->GetParser();
  EXPECT_FALSE(parser->xref_table_rebuilt());
  ExpectRawGenerationsConsistent(parser);
  DumpSavedPdf(bytes, password, original_size);
  for (const auto& [number, info] :
       parser->GetCrossRefTable()->objects_info()) {
    if (info.type == CPDF_CrossRefTable::ObjectType::kFree) {
      continue;
    }
    model.in_use.insert(number);
    if (info.type == CPDF_CrossRefTable::ObjectType::kNormal &&
        info.pos >= static_cast<FX_FILESIZE>(original_size)) {
      model.written.insert(number);
    }
    auto object = doc->GetOrParseIndirectObject(number);
    EXPECT_TRUE(object);
    if (!object) {
      continue;
    }
    const auto dict = object->GetDict();
    if (dict && dict->GetByteStringFor("Filter") == "Standard" &&
        (dict->KeyExist("O") || dict->KeyExist("U"))) {
      ++model.encryption_dictionaries;
    }
    if (dict && dict->GetByteStringFor("Type") == "XRef" &&
        number != parser->GetTrailerObjectNumber()) {
      ++model.stale_xref_streams;
    }
  }

  std::deque<uint32_t> pending;
  auto add_references = [&](RetainPtr<const CPDF_Object> object) {
    CPDF_ObjectWalker walker(std::move(object));
    while (auto current = walker.GetNext()) {
      if (current->IsReference()) {
        const uint32_t number = current->AsReference()->GetRefObjNum();
        if (model.reachable.insert(number).second) {
          pending.push_back(number);
        }
      }
    }
  };
  add_references(parser->GetCombinedTrailer());
  if (const uint32_t xref = parser->GetTrailerObjectNumber()) {
    model.reachable.insert(xref);
  }
  while (!pending.empty()) {
    const uint32_t number = pending.front();
    pending.pop_front();
    if (auto object = doc->GetOrParseIndirectObject(number)) {
      add_references(std::move(object));
    }
  }
  return model;
}

}  // namespace

class EPDFSaveEmbedderTest : public EmbedderTest {};

TEST_F(EPDFSaveEmbedderTest, FullRewriteWritesOnlyReachableObjects) {
  for (bool xref_stream : {false, true}) {
    for (bool use_layer : {false, true}) {
      SCOPED_TRACE(testing::Message() << xref_stream << ":" << use_layer);
      const std::string input = MakeGenerationDocument(256, xref_stream);
      ScopedFPDFDocument source;
      if (use_layer) {
        EPDF_BASE_DOCUMENT base =
            EPDF_LoadMemBaseDocument(input.data(), input.size(), nullptr);
        ASSERT_TRUE(base);
        source.reset(EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
        EPDF_ReleaseBaseDocument(base);
      } else {
        source.reset(FPDF_LoadMemDocument(input.data(), input.size(), nullptr));
      }
      ASSERT_TRUE(source);
      ClearString();
      ASSERT_TRUE(FPDF_SaveAsCopy(source.get(), this, FPDF_NO_INCREMENTAL));
      const auto model = AnalyzeSavedPdf(GetString());
      EXPECT_EQ(model.reachable, model.in_use);
      EXPECT_EQ(0u, model.stale_xref_streams);
      EXPECT_EQ((std::set<uint32_t>{1, 2, 3, 4, 5}), model.in_use);
    }
  }
}

TEST_F(EPDFSaveEmbedderTest, RemoveSecurityLeavesNoEncryptionDictionary) {
  const std::pair<const char*, const char*> fixtures[] = {
      {"encrypted_hello_world_r3.pdf", "\xc3\xa2ge"},
      {"encrypted.pdf", "1234"},
      {"encrypted_hello_world_r6.pdf", "\xc3\xa2ge"}};
  for (const auto& [filename, password] : fixtures) {
    for (bool use_layer : {false, true}) {
      SCOPED_TRACE(testing::Message() << filename << ":" << use_layer);
      const std::string path = PathService::GetTestFilePath(filename);
      const auto input = GetFileContents(path.c_str());
      ScopedFPDFDocument source;
      if (use_layer) {
        EPDF_BASE_DOCUMENT base =
            EPDF_LoadMemBaseDocument(input.data(), input.size(), password);
        ASSERT_TRUE(base);
        source.reset(EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
        EPDF_ReleaseBaseDocument(base);
      } else {
        source.reset(FPDF_LoadDocument(path.c_str(), password));
      }
      ASSERT_TRUE(source);
      ClearString();
      ASSERT_TRUE(FPDF_SaveAsCopy(source.get(), this, FPDF_REMOVE_SECURITY));
      const auto model = AnalyzeSavedPdf(GetString());
      EXPECT_EQ(model.reachable, model.in_use);
      EXPECT_EQ(0u, model.encryption_dictionaries);
      EXPECT_EQ(0u, model.stale_xref_streams);
    }
  }
}

TEST_F(EPDFSaveEmbedderTest, ReplacingEncryptionDropsOldDictionary) {
  ASSERT_TRUE(
      OpenDocumentWithPassword("encrypted_hello_world_r3.pdf", "\xc3\xa2ge"));
  ASSERT_TRUE(EPDF_SetEncryption(document(), "new-user", "new-owner",
                                 EPDF_PERM_PRINT | EPDF_PERM_COPY));
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_NO_INCREMENTAL));
  const auto model = AnalyzeSavedPdf(GetString(), "new-user");
  EXPECT_EQ(model.reachable, model.in_use);
  EXPECT_EQ(1u, model.encryption_dictionaries);
}

TEST_F(EPDFSaveEmbedderTest, FullRewriteDropsReplacedInfoAndKeepsCustomRoots) {
  class DocumentWithReplaceableInfo : public CPDF_Document {
   public:
    DocumentWithReplaceableInfo()
        : CPDF_Document(std::make_unique<CPDF_DocRenderData>(),
                        std::make_unique<CPDF_DocPageData>()) {}
    using CPDF_Document::SetCachedInfoDict;
  };
  const std::string input = MakeGenerationDocument(0, false);
  DocumentWithReplaceableInfo doc;
  ASSERT_EQ(CPDF_Parser::SUCCESS,
            doc.LoadDoc(pdfium::MakeRetain<CFX_ReadOnlySpanStream>(
                            pdfium::as_byte_span(input)),
                        ""));
  auto replacement = doc.NewIndirect<CPDF_Dictionary>();
  replacement->SetNewFor<CPDF_String>("Title", "replacement");
  doc.SetCachedInfoDict(replacement);
  auto custom = doc.NewIndirect<CPDF_Dictionary>();
  custom->SetNewFor<CPDF_String>("Marker", "retained trailer root");
  doc.GetParser()->GetMutableTrailerForTesting()->SetNewFor<CPDF_Reference>(
      "Custom", &doc, custom->GetObjNum());
  ASSERT_TRUE(FPDF_SaveAsCopy(FPDFDocumentFromCPDFDocument(&doc), this,
                              FPDF_NO_INCREMENTAL));
  const auto model = AnalyzeSavedPdf(GetString());
  EXPECT_EQ(model.reachable, model.in_use);
  EXPECT_FALSE(model.in_use.contains(5));
  EXPECT_TRUE(model.in_use.contains(replacement->GetObjNum()));
  EXPECT_TRUE(model.in_use.contains(custom->GetObjNum()));
}

TEST_F(EPDFSaveEmbedderTest, ReferenceIndexMatchesFileEdges) {
  const auto bytes = GetFileContents(
      PathService::GetTestFilePath("annotation_stamp_with_ap.pdf").c_str());
  const std::string input(bytes.begin(), bytes.end());
  CountingFileAccess access(input);
  ScopedFPDFDocument source(FPDF_LoadCustomDocument(&access, nullptr));
  ASSERT_TRUE(source);
  auto* doc = CPDFDocumentFromFPDFDocument(source.get());
  CPDF_SaveObjectReader reader(doc);
  CPDF_SaveObjectReader original(doc,
                                 CPDF_SaveObjectReader::Version::kOriginal);
  for (const auto& [number, info] :
       doc->GetParser()->GetCrossRefTable()->objects_info()) {
    if (info.type == CPDF_CrossRefTable::ObjectType::kFree) {
      continue;
    }
    auto object = original.Read(number);
    ASSERT_TRUE(object);
    const auto expected = CPDF_CollectReferences(object);
    EXPECT_EQ(pdfium::span(expected), reader.ReferencesFor(number));
    const size_t reads = access.read_count();
    EXPECT_EQ(pdfium::span(expected), reader.ReferencesFor(number));
    EXPECT_EQ(reads, access.read_count());
  }
  const uint32_t missing = doc->GetParser()->GetLastObjNum() + 1;
  EXPECT_TRUE(reader.ReferencesFor(missing).empty());
  EXPECT_FALSE(doc->GetParser()->GetSaveReferenceIndex()->Find(missing));
}

TEST_F(EPDFSaveEmbedderTest, CachedFileReferencesDoNotOverrideLayerEdits) {
  const std::string input = MakeGenerationDocument(256, false);
  EPDF_BASE_DOCUMENT base =
      EPDF_LoadMemBaseDocument(input.data(), input.size(), nullptr);
  ASSERT_TRUE(base);
  ScopedFPDFDocument first(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
  ScopedFPDFDocument sibling(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
  EPDF_ReleaseBaseDocument(base);
  ASSERT_TRUE(first);
  ASSERT_TRUE(sibling);
  auto* doc = CPDFDocumentFromFPDFDocument(first.get());
  CPDF_SaveObjectReader original(doc,
                                 CPDF_SaveObjectReader::Version::kOriginal);
  const auto references = CPDF_CollectReferences(original.Read(3));
  ASSERT_TRUE(doc->GetParser()->GetSaveReferenceIndex()->Insert(3, references));

  auto page = doc->GetMutablePageDictionary(0);
  page->RemoveFor("Contents");
  auto stream = ToStream(doc->GetMutableIndirectObject(4));
  stream->GetMutableDict()->SetNewFor<CPDF_Number>("Probe", 1);
  ASSERT_TRUE(EPDFLayer_SaveDelta(first.get(), this, nullptr));
  auto model = AnalyzeSavedPdf(input + GetString(), nullptr, input.size());
  EXPECT_EQ((std::set<uint32_t>{3}), model.written);

  page->SetNewFor<CPDF_Reference>("Contents", doc, 4);
  ClearString();
  ASSERT_TRUE(EPDFLayer_SaveDelta(first.get(), this, nullptr));
  model = AnalyzeSavedPdf(input + GetString(), nullptr, input.size());
  EXPECT_EQ((std::set<uint32_t>{4}), model.written);

  ClearString();
  ASSERT_TRUE(EPDFLayer_SaveDelta(sibling.get(), this, nullptr));
  EXPECT_TRUE(GetString().empty());
  auto* other = CPDFDocumentFromFPDFDocument(sibling.get());
  EXPECT_TRUE(other->GetPageDictionary(0)->KeyExist("Contents"));
  EXPECT_FALSE(
      other->GetOrParseIndirectObject(4)->GetDict()->KeyExist("Probe"));
}

TEST_F(EPDFSaveEmbedderTest, DetachedObjectDoesNotInflateWarmSaveReads) {
  const auto bytes = GetFileContents(
      PathService::GetTestFilePath("annotation_stamp_with_ap.pdf").c_str());
  const std::string input(bytes.begin(), bytes.end());
  for (bool use_layer : {false, true}) {
    SCOPED_TRACE(use_layer);
    CountingFileAccess access(input);
    ScopedFPDFDocument source;
    if (use_layer) {
      EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&access, nullptr);
      ASSERT_TRUE(base);
      source.reset(EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
      EPDF_ReleaseBaseDocument(base);
    } else {
      source.reset(FPDF_LoadCustomDocument(&access, nullptr));
    }
    ASSERT_TRUE(source);
    auto* doc = CPDFDocumentFromFPDFDocument(source.get());
    auto page = doc->GetMutablePageDictionary(0);
    page->SetNewFor<CPDF_Number>("Rotate", 90);
    const size_t before = access.read_count();
    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(source.get(), this, FPDF_INCREMENTAL));
    const size_t baseline_reads = access.read_count() - before;
    doc->NewIndirect<CPDF_Dictionary>();
    const auto count = std::distance(doc->begin(), doc->end());
    const size_t streams =
        doc->GetParser()->GetCachedObjectStreamCountForTesting();
    const uint64_t epoch = doc->GetOverlayEpoch();
    // The first detached save learns file edges. Without the index the original
    // probe read 177 blocks on every save, versus 31 for the ordinary edit.
    for (int pass = 0; pass < 2; ++pass) {
      const size_t reads = access.read_count();
      ClearString();
      ASSERT_TRUE(FPDF_SaveAsCopy(source.get(), this, FPDF_INCREMENTAL));
      if (pass == 1) {
        EXPECT_LE(access.read_count() - reads, baseline_reads);
      }
      EXPECT_EQ(count, std::distance(doc->begin(), doc->end()));
      EXPECT_EQ(streams,
                doc->GetParser()->GetCachedObjectStreamCountForTesting());
      EXPECT_EQ(epoch, doc->GetOverlayEpoch());
    }
    auto* index = doc->GetParser()->GetSaveReferenceIndex();
    ASSERT_TRUE(index);
    EXPECT_GT(index->row_count(), 0u);
    EXPECT_LE(index->accounted_bytes(), CPDF_ReferenceIndex::kDefaultByteLimit);
  }
}

TEST_F(EPDFSaveEmbedderTest, IncrementalRevisionWritesExactlyChangedObjects) {
  for (bool xref_stream : {false, true}) {
    const std::string input = MakeGenerationDocument(256, xref_stream);
    ScopedFPDFDocument source(
        FPDF_LoadMemDocument(input.data(), input.size(), nullptr));
    ASSERT_TRUE(source);
    auto* doc = CPDFDocumentFromFPDFDocument(source.get());
    auto page = doc->GetMutablePageDictionary(0);
    for (int pass = 0; pass < 2; ++pass) {
      page->SetNewFor<CPDF_Number>("Rotate", 90);
      ClearString();
      ASSERT_TRUE(FPDF_SaveAsCopy(source.get(), this, FPDF_INCREMENTAL));
      EXPECT_EQ(input, GetString().substr(0, input.size()));
      const auto model = AnalyzeSavedPdf(GetString(), nullptr, input.size());
      // Saving again retains the same cumulative edit against the loaded input.
      EXPECT_EQ(
          xref_stream ? (std::set<uint32_t>{3, 7}) : (std::set<uint32_t>{3}),
          model.written);
    }
    page->RemoveFor("Rotate");
    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(source.get(), this, FPDF_INCREMENTAL));
    EXPECT_EQ(input, GetString());
    doc->NewIndirect<CPDF_Dictionary>();
    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(source.get(), this, FPDF_INCREMENTAL));
    EXPECT_EQ(input, GetString());

    ScopedFPDFPage loaded_page(FPDF_LoadPage(source.get(), 0));
    ASSERT_TRUE(loaded_page);
    ScopedFPDFAnnotation annotation(
        EPDFPage_CreateAnnot(loaded_page.get(), FPDF_ANNOT_SQUARE));
    ASSERT_TRUE(annotation);
    const FS_RECTF rect{20, 80, 80, 20};
    ASSERT_TRUE(FPDFAnnot_SetRect(annotation.get(), &rect));
    std::set<uint32_t> expected{3, EPDFAnnot_GetObjectNumber(annotation.get())};
    if (xref_stream) {
      expected.insert(doc->GetLastObjNum() + 1);
    }
    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(source.get(), this, FPDF_INCREMENTAL));
    EXPECT_EQ(input, GetString().substr(0, input.size()));
    const auto attached = AnalyzeSavedPdf(GetString(), nullptr, input.size());
    EXPECT_EQ(expected, attached.written);
  }
}

TEST_F(EPDFSaveEmbedderTest, LayerRevisionKeepsBaseAndLoadedBaselinesSeparate) {
  const std::string input = MakeGenerationDocument(256, true);
  EPDF_BASE_DOCUMENT base =
      EPDF_LoadMemBaseDocument(input.data(), input.size(), nullptr);
  ASSERT_TRUE(base);
  ScopedFPDFDocument first(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
  ASSERT_TRUE(first);
  auto* first_doc = CPDFDocumentFromFPDFDocument(first.get());
  first_doc->GetMutablePageDictionary(0)->SetNewFor<CPDF_Number>("Rotate", 90);
  ASSERT_TRUE(EPDFLayer_SaveDelta(first.get(), this, nullptr));
  const std::string loaded_delta = GetString();
  CountingFileAccess delta_access(loaded_delta);
  ScopedFPDFDocument reopened(
      EPDFLayer_OpenLayer(base, &delta_access, nullptr, nullptr));
  EPDF_ReleaseBaseDocument(base);
  ASSERT_TRUE(reopened);

  FPDF_BOOL changed = true;
  ClearString();
  ASSERT_TRUE(EPDFLayer_SaveDeltaEx(reopened.get(), this, nullptr, &changed));
  EXPECT_FALSE(changed);
  EXPECT_TRUE(GetString().empty());
  ASSERT_TRUE(EPDFLayer_SaveDelta(reopened.get(), this, nullptr));
  const auto model =
      AnalyzeSavedPdf(input + GetString(), nullptr, input.size());
  EXPECT_EQ((std::set<uint32_t>{3}), model.written);

  auto* doc = CPDFDocumentFromFPDFDocument(reopened.get());
  doc->GetMutablePageDictionary(0)->RemoveFor("Rotate");
  ClearString();
  ASSERT_TRUE(EPDFLayer_SaveDeltaEx(reopened.get(), this, nullptr, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(GetString().empty());
}

TEST_F(EPDFSaveEmbedderTest, SavePreservesConsistentObjectGenerations) {
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
        AnalyzeSavedPdf(GetString(), nullptr, incremental ? input.size() : 0);
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

TEST_F(EPDFSaveEmbedderTest, IncrementalSaveDoesNotPopulateDocumentCaches) {
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

TEST_F(EPDFSaveEmbedderTest, IncrementalSavePreservesDeclaredXrefSize) {
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

TEST_F(EPDFSaveEmbedderTest,
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
        const auto model =
            AnalyzeSavedPdf(GetString(), password,
                            flags == FPDF_INCREMENTAL ? input.size() : 0);
        if (flags != FPDF_INCREMENTAL) {
          EXPECT_EQ(model.reachable, model.in_use);
          EXPECT_EQ(1u, model.encryption_dictionaries);
        }
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

TEST_F(EPDFSaveEmbedderTest, IncrementalSavePreservesSharedBaseAndSibling) {
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

TEST_F(EPDFSaveEmbedderTest, LayerSavePreservesReferenceGenerationChanges) {
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

TEST_F(EPDFSaveEmbedderTest, FullRewriteDoesNotPopulateDocumentCaches) {
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

TEST_F(EPDFSaveEmbedderTest, FullRewriteDoesNotPopulateSharedBaseCaches) {
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

// A full save walks the document from its roots instead of upstream's
// WriteNewObjs(), and still writes the subset fonts: the file shrinks and the
// page draws the same. Native text is off so that only the font data counts.
TEST_F(EPDFSaveEmbedderTest, FullSaveSubsetsNewFonts) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  struct NewText {
    const char* font_path;
    const wchar_t* text;
    float y;
  };
  // An OpenType font with CFF outlines, then a TrueType one.
  for (const NewText& new_text :
       {NewText{"NotoSansCJK/NotoSansSC-Regular.subset.otf", L"这是第一句。",
                10.0f},
        NewText{"test_fonts/test_fonts/Arimo-Regular.ttf", L"Hello again.",
                150.0f}}) {
    std::vector<uint8_t> font_data = GetFileContents(
        PathService::GetThirdPartyFilePath(new_text.font_path).c_str());
    ASSERT_FALSE(font_data.empty());
    ScopedFPDFFont font(FPDFText_LoadFont(document(), font_data.data(),
                                          font_data.size(), FPDF_FONT_TRUETYPE,
                                          /*cid=*/true));
    ASSERT_TRUE(font);
    FPDF_PAGEOBJECT text =
        FPDFPageObj_CreateTextObj(document(), font.get(), 24.0f);
    ScopedFPDFWideString words = GetFPDFWideString(new_text.text);
    ASSERT_TRUE(FPDFText_SetText(text, words.get()));
    FPDFPageObj_Transform(text, 1, 0, 0, 1, 10, new_text.y);
    FPDFPage_InsertObject(page.get(), text);
  }
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  const std::string drawn = HashBitmap(
      RenderLoadedPageWithFlags(page.get(), FPDF_NO_NATIVETEXT).get());

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  const size_t whole_size = GetString().size();
  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_SUBSET_NEW_FONTS));
  EXPECT_LT(GetString().size() * 10, whole_size);
  EXPECT_TRUE(NamesASubsetOf(GetString(), "Arimo-Regular"));

  ScopedSavedDoc saved = OpenScopedSavedDocument();
  ASSERT_TRUE(saved);
  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);
  EXPECT_EQ(drawn, HashBitmap(RenderSavedPageWithFlags(saved_page.get(),
                                                       FPDF_NO_NATIVETEXT)
                                  .get()));
}

// Subsetting new fonts parses every page to find the glyphs in use. On a layer
// it still subsets, and copies nothing up: the save leaves the layer as it
// was.
TEST_F(EPDFSaveEmbedderTest, SubsettingNewFontsCopiesNoPageIntoTheLayer) {
  FileAccessForTesting access("hello_world_2_pages.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&access, nullptr);
  ASSERT_TRUE(base);
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
  // The layer retains the base, including on an assertion failure below.
  EPDF_ReleaseBaseDocument(base);
  ASSERT_TRUE(layer);

  std::vector<uint8_t> font_data =
      GetFileContents(PathService::GetThirdPartyFilePath(
                          "test_fonts/test_fonts/Arimo-Regular.ttf")
                          .c_str());
  ASSERT_FALSE(font_data.empty());
  ScopedFPDFFont font(FPDFText_LoadFont(layer.get(), font_data.data(),
                                        font_data.size(), FPDF_FONT_TRUETYPE,
                                        /*cid=*/true));
  ASSERT_TRUE(font);
  {
    ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
    ASSERT_TRUE(page);
    FPDF_PAGEOBJECT text = FPDFPageObj_CreateTextObj(layer.get(), font.get(),
                                                     24.0f);
    ScopedFPDFWideString words = GetFPDFWideString(L"New words");
    ASSERT_TRUE(FPDFText_SetText(text, words.get()));
    FPDFPage_InsertObject(page.get(), text);
    ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  }

  auto* doc = CPDF_LayerDocument::FromDocument(
      CPDFDocumentFromFPDFDocument(layer.get()));
  ASSERT_TRUE(doc);
  const size_t promoted_count = doc->GetPromotedObjectCount();
  ASSERT_TRUE(FPDF_SaveAsCopy(layer.get(), this, FPDF_SUBSET_NEW_FONTS));
  EXPECT_EQ(promoted_count, doc->GetPromotedObjectCount());
  EXPECT_TRUE(NamesASubsetOf(GetString(), "Arimo-Regular"));
}

TEST_F(EPDFSaveEmbedderTest, FullRewritePreservesLiveAnnotationAcrossSaves) {
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

TEST_F(EPDFSaveEmbedderTest, SaveSimpleDocToOwnedBuffer) {
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

TEST_F(EPDFSaveEmbedderTest, SaveAsCopyPrunesUnlinkedNewAnnotation) {
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

TEST_F(EPDFSaveEmbedderTest, IncrementalSavePrunesUnlinkedNewAnnotation) {
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

TEST_F(EPDFSaveEmbedderTest, SaveAsCopyKeepsLinkedNewAnnotation) {
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

TEST_F(EPDFSaveEmbedderTest, SetEncryptionRoundTripsWithInlineEncryptDict) {
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

// The file key and the salts in /U and /O come from the operating system's
// secure random source, so two saves with the same passwords share neither.
// (They came from FX_Random, a Mersenne Twister with a 32-bit seed, and the
// public salts let an attacker find that seed and recompute the key.)
TEST_F(EPDFSaveEmbedderTest, EncryptedSavesShareNoKeyOrSalts) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  std::string saves[2];
  for (std::string& save : saves) {
    ASSERT_TRUE(EPDF_SetEncryption(document(), "user", "owner",
                                   EPDF_PERM_PRINT | EPDF_PERM_COPY));
    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_NO_INCREMENTAL));
    save = GetString();
  }

  ScopedFPDFDocument saved[2];
  for (size_t i = 0; i < 2; ++i) {
    SCOPED_TRACE(i);
    const std::string& save = saves[i];
    // The owner password opens the file too.
    EXPECT_TRUE(ScopedFPDFDocument(
        FPDF_LoadMemDocument(save.data(), save.size(), "owner")));
    saved[i].reset(FPDF_LoadMemDocument(save.data(), save.size(), "user"));
    ASSERT_TRUE(saved[i]);
    // Streams decrypt with the file's key and its IVs: the page's content
    // reads back.
    ScopedFPDFPage page(FPDF_LoadPage(saved[i].get(), 0));
    ASSERT_TRUE(page);
    EXPECT_GT(FPDFPage_CountObjects(page.get()), 0);
  }

  const CPDF_Parser* parsers[2] = {
      CPDFDocumentFromFPDFDocument(saved[0].get())->GetParser(),
      CPDFDocumentFromFPDFDocument(saved[1].get())->GetParser()};
  for (const char* key : {"U", "UE", "O", "OE"}) {
    SCOPED_TRACE(key);
    const ByteString first =
        parsers[0]->GetEncryptDict()->GetByteStringFor(key);
    const ByteString second =
        parsers[1]->GetEncryptDict()->GetByteStringFor(key);
    EXPECT_FALSE(first.IsEmpty());
    EXPECT_NE(first, second);
  }

  // The file keys differ: what the first file's key encrypts, its own key
  // decrypts and the second file's doesn't.
  CPDF_CryptoHandler* crypto[2] = {
      parsers[0]->GetSecurityHandler()->GetCryptoHandler(),
      parsers[1]->GetSecurityHandler()->GetCryptoHandler()};
  const ByteString probe = "the same plaintext";
  const DataVector<uint8_t> encrypted =
      crypto[0]->EncryptContent(1, 0, probe.unsigned_span()).value();
  auto own =
      pdfium::MakeRetain<CPDF_String>(encrypted, CPDF_String::DataType::kIsHex);
  auto other =
      pdfium::MakeRetain<CPDF_String>(encrypted, CPDF_String::DataType::kIsHex);
  ASSERT_TRUE(crypto[0]->DecryptObjectTree(own));
  EXPECT_EQ(probe, own->GetString());
  crypto[1]->DecryptObjectTree(other);
  EXPECT_NE(probe, other->GetString());
}

TEST_F(EPDFSaveEmbedderTest, RemoveEncryptionUsesDocumentOwnedPendingState) {
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
