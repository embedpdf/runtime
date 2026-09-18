// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/parser/cpdf_layer_document.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pagemodule.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_base_document.h"
#include "core/fpdfapi/parser/cpdf_concat_read_stream.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_object_equality.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_write_context.h"
#include "core/fpdfapi/render/cpdf_docrenderdata.h"
#include "core/fxcrt/cfx_read_only_container_stream.h"
#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

enum class TwinView { kCurrent, kBase, kLoaded };

class TwinWriteContext final : public CPDF_WriteContext {
 public:
  TwinWriteContext(CPDF_LayerDocument* layer, TwinView view)
      : layer_(layer), view_(view) {}

  uint32_t GetObjectGeneration(uint32_t number) const override {
    if (view_ == TwinView::kCurrent) {
      if (auto object = layer_->FindPromotedObject(number)) {
        return object->GetGenNum();
      }
    } else if (view_ == TwinView::kLoaded) {
      if (auto object = layer_->FindLoadedDeltaTwin(number)) {
        return object->GetGenNum();
      }
    }
    const auto* info =
        layer_->GetParser()->GetCrossRefTable()->GetObjectInfo(number);
    return info && info->type == CPDF_CrossRefTable::ObjectType::kNormal
               ? info->gennum
               : 0;
  }

 private:
  CPDF_LayerDocument* const layer_;
  const TwinView view_;
};

bool SameAsTwin(CPDF_LayerDocument* layer, uint32_t number, TwinView view) {
  RetainPtr<const CPDF_Object> current = layer->FindPromotedObject(number);
  if (!current) {
    current = layer->GetBaseTwin(number);
  }
  RetainPtr<const CPDF_Object> twin;
  if (view == TwinView::kLoaded) {
    twin = layer->FindLoadedDeltaTwin(number);
  }
  if (!twin) {
    twin = layer->GetBaseTwin(number);
  }
  if (!current || !twin) {
    return current == twin;
  }
  const TwinWriteContext current_context(layer, TwinView::kCurrent);
  const TwinWriteContext twin_context(layer, view);
  return current->GetGenNum() == twin->GetGenNum() &&
         CPDF_SameEffectiveValue(current.Get(), twin.Get(), &current_context,
                                 &twin_context);
}

class CPDFLayerDocumentTest : public testing::Test {
 protected:
  static void SetUpTestSuite() { pdfium::InitializePageModule(); }
  static void TearDownTestSuite() { pdfium::DestroyPageModule(); }
};

class CountingReadStream final : public IFX_SeekableReadStream {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  FX_FILESIZE GetSize() override {
    return static_cast<FX_FILESIZE>(data_.size());
  }

  bool ReadBlockAtOffset(pdfium::span<uint8_t> buffer,
                         FX_FILESIZE offset) override {
    if (offset < 0 || static_cast<size_t>(offset) > data_.size() ||
        buffer.size() > data_.size() - static_cast<size_t>(offset)) {
      return false;
    }
    ++read_count_;
    read_bytes_ += buffer.size();
    if (!buffer.empty()) {
      memcpy(buffer.data(), data_.data() + offset, buffer.size());
    }
    return true;
  }

  size_t read_count() const { return read_count_; }
  size_t read_bytes() const { return read_bytes_; }

 private:
  explicit CountingReadStream(std::string data) : data_(std::move(data)) {}
  ~CountingReadStream() override = default;

  std::string data_;
  size_t read_count_ = 0;
  size_t read_bytes_ = 0;
};

std::string BuildSimplePdf() {
  const std::vector<std::string> objects = {
      "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
      "2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n",
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] >>\n"
      "endobj\n",
  };

  std::ostringstream pdf;
  pdf << "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (const std::string& object : objects) {
    offsets.push_back(pdf.tellp());
    pdf << object;
  }

  const size_t xref_offset = pdf.tellp();
  pdf << "xref\n0 " << (objects.size() + 1) << "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
  }
  pdf << "trailer\n<< /Size " << (objects.size() + 1)
      << " /Root 1 0 R >>\nstartxref\n"
      << xref_offset << "\n%%EOF\n";
  return pdf.str();
}

std::string BuildSimplePdfWithInfo() {
  const std::vector<std::string> objects = {
      "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
      "2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n",
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] >>\n"
      "endobj\n",
      "4 0 obj\n<< /Title (Base Title) >>\nendobj\n",
  };

  std::ostringstream pdf;
  pdf << "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (const std::string& object : objects) {
    offsets.push_back(pdf.tellp());
    pdf << object;
  }

  const size_t xref_offset = pdf.tellp();
  pdf << "xref\n0 " << (objects.size() + 1) << "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
  }
  pdf << "trailer\n<< /Size " << (objects.size() + 1)
      << " /Root 1 0 R /Info 4 0 R >>\nstartxref\n"
      << xref_offset << "\n%%EOF\n";
  return pdf.str();
}

std::string BuildPdfWithIndirectAnnotation() {
  const std::vector<std::string> objects = {
      "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
      "2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n",
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100]\n"
      "   /Annots [4 0 R] >>\n"
      "endobj\n",
      "4 0 obj\n"
      "<< /Type /Annot /Subtype /Polygon /Rect [10 10 30 30]\n"
      "   /Vertices [10 10 30 10 20 30] >>\n"
      "endobj\n",
  };

  std::ostringstream pdf;
  pdf << "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (const std::string& object : objects) {
    offsets.push_back(pdf.tellp());
    pdf << object;
  }

  const size_t xref_offset = pdf.tellp();
  pdf << "xref\n0 " << (objects.size() + 1) << "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
  }
  pdf << "trailer\n<< /Size " << (objects.size() + 1)
      << " /Root 1 0 R >>\nstartxref\n"
      << xref_offset << "\n%%EOF\n";
  return pdf.str();
}

std::string BuildPdfWithFormXObject() {
  const std::string form_content = "0 0 10 10 re f\n";
  std::ostringstream form_object;
  form_object << "4 0 obj\n"
              << "<< /Type /XObject /Subtype /Form /BBox [0 0 100 100]\n"
              << "   /Resources << /BaseMarker 1 >> /Length "
              << form_content.size() << " >>\n"
              << "stream\n"
              << form_content
              << "endstream\nendobj\n";

  const std::vector<std::string> objects = {
      "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
      "2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n",
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100]\n"
      "   /Resources << /XObject << /Fm0 4 0 R >> >> >>\n"
      "endobj\n",
      form_object.str(),
  };

  std::ostringstream pdf;
  pdf << "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (const std::string& object : objects) {
    offsets.push_back(pdf.tellp());
    pdf << object;
  }

  const size_t xref_offset = pdf.tellp();
  pdf << "xref\n0 " << (objects.size() + 1) << "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
  }
  pdf << "trailer\n<< /Size " << (objects.size() + 1)
      << " /Root 1 0 R >>\nstartxref\n"
      << xref_offset << "\n%%EOF\n";
  return pdf.str();
}

std::string BuildPdfWithDirectResources() {
  const std::vector<std::string> objects = {
      "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
      "2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n",
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100]\n"
      "   /Resources << /ProcSet [/PDF] >> >>\n"
      "endobj\n",
  };

  std::ostringstream pdf;
  pdf << "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (const std::string& object : objects) {
    offsets.push_back(pdf.tellp());
    pdf << object;
  }

  const size_t xref_offset = pdf.tellp();
  pdf << "xref\n0 " << (objects.size() + 1) << "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
  }
  pdf << "trailer\n<< /Size " << (objects.size() + 1)
      << " /Root 1 0 R >>\nstartxref\n"
      << xref_offset << "\n%%EOF\n";
  return pdf.str();
}

size_t GetStartXrefOffsetFromPdf(const std::string& pdf) {
  constexpr char kStartXref[] = "startxref\n";
  const size_t start = pdf.rfind(kStartXref);
  CHECK_NE(std::string::npos, start);
  return static_cast<size_t>(
      std::stoull(pdf.substr(start + sizeof(kStartXref) - 1)));
}

std::string BuildFreeEntryDeltaForObject(const std::string& base_pdf,
                                         uint32_t objnum) {
  std::ostringstream delta;
  const size_t xref_offset = base_pdf.size();
  delta << "xref\n"
        << objnum << " 1\n0000000000 00001 f \n"
        << "trailer\n<< /Size 5 /Root 1 0 R /Prev "
        << GetStartXrefOffsetFromPdf(base_pdf) << " >>\nstartxref\n"
        << xref_offset << "\n%%EOF\n";
  return delta.str();
}

std::string BuildCorruptPagesDelta(const std::string& base_pdf) {
  std::ostringstream delta;
  delta << "2 0 obj\n"
        << "<< /Type /Pages /Count 1 /Kids [4 0 R] >>\n"
        << "endobj\n";
  const size_t xref_offset = base_pdf.size() + delta.tellp();
  delta << "xref\n2 1\n"
        << std::setw(10) << std::setfill('0') << base_pdf.size()
        << " 00000 n \n"
        << "trailer\n<< /Size 5 /Root 1 0 R /Prev "
        << GetStartXrefOffsetFromPdf(base_pdf) << " >>\nstartxref\n"
        << xref_offset << "\n%%EOF\n";
  return delta.str();
}

RetainPtr<IFX_SeekableReadStream> MakeStreamForString(const std::string& data) {
  // Own the bytes: a base parses on first touch, so the stream must outlive
  // the call (a span of a temporary would dangle).
  return pdfium::MakeRetain<CFX_ReadOnlyByteStringStream>(
      ByteString(data.data(), data.size()));
}

RetainPtr<CPDF_BaseDocument> LoadBaseDocumentFromString(
    const std::string& data) {
  RetainPtr<CPDF_BaseDocument> document =
      pdfium::MakeRetain<CPDF_BaseDocument>();
  if (document->LoadBaseDoc(MakeStreamForString(data), "") !=
      CPDF_Parser::SUCCESS) {
    return nullptr;
  }
  return document;
}

RetainPtr<CPDF_Page> MakeLayerPage(CPDF_LayerDocument* layer, int page_index) {
  RetainPtr<const CPDF_Dictionary> page_dict =
      layer->GetPageDictionary(page_index);
  if (!page_dict) {
    return nullptr;
  }
  return pdfium::MakeRetain<CPDF_Page>(
      layer, pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(page_dict.Get())));
}

}  // namespace

TEST(CPDFConcatReadStreamTest, DelegatesReadsAcrossStreamBoundary) {
  RetainPtr<CountingReadStream> first =
      pdfium::MakeRetain<CountingReadStream>("abc");
  RetainPtr<CountingReadStream> second =
      pdfium::MakeRetain<CountingReadStream>("DEF");
  RetainPtr<CPDF_ConcatReadStream> concat =
      pdfium::MakeRetain<CPDF_ConcatReadStream>(first, second);

  ASSERT_EQ(6, concat->GetSize());
  std::array<uint8_t, 4> buffer = {};
  ASSERT_TRUE(concat->ReadBlockAtOffset(pdfium::span(buffer), 2));
  EXPECT_EQ("cDEF", std::string(reinterpret_cast<const char*>(buffer.data()),
                                buffer.size()));
  EXPECT_EQ(1u, first->read_count());
  EXPECT_EQ(1u, first->read_bytes());
  EXPECT_EQ(1u, second->read_count());
  EXPECT_EQ(3u, second->read_bytes());
}

TEST(CPDFConcatReadStreamTest, AllowsZeroLengthReadAtEnd) {
  RetainPtr<CPDF_ConcatReadStream> concat =
      pdfium::MakeRetain<CPDF_ConcatReadStream>(
          pdfium::MakeRetain<CountingReadStream>("abc"),
          pdfium::MakeRetain<CountingReadStream>(""));
  std::array<uint8_t, 1> buffer = {};
  EXPECT_TRUE(concat->ReadBlockAtOffset(
      pdfium::span(buffer).first(static_cast<size_t>(0)), 3));
  EXPECT_FALSE(concat->ReadBlockAtOffset(
      pdfium::span(buffer).first(static_cast<size_t>(0)), 4));
}

TEST_F(CPDFLayerDocumentTest, FreshLayerFallsThroughToFrozenBase) {
  const std::string pdf = BuildSimplePdf();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);

  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);

  EXPECT_EQ(CPDF_LayerDocument::OpenStatus::kSuccess, layer->ingest_status());
  EXPECT_TRUE(layer->IsLayerDocument());
  EXPECT_EQ(base->GetParser(), layer->GetParser());
  EXPECT_EQ(base->GetLastObjNum(), layer->GetLastObjNum());
  EXPECT_EQ(0u, layer->GetPromotedObjectCount());
  EXPECT_EQ(1, layer->GetPageCount());

  RetainPtr<const CPDF_Dictionary> page = layer->GetPageDictionary(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(3u, page->GetObjNum());
  EXPECT_EQ(base->GetFrozenObjectForLayer(3).Get(), page.Get());
  EXPECT_EQ(base->GetUserPermissions(false), layer->GetUserPermissions(false));
  EXPECT_EQ(0u, layer->GetPromotedObjectCount());
}

TEST_F(CPDFLayerDocumentTest, DeleteBaseObjectIsNoOp) {
  const std::string pdf = BuildSimplePdf();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);

  ASSERT_TRUE(layer->GetIndirectObject(1));
  layer->DeleteIndirectObject(1);
  EXPECT_TRUE(layer->GetIndirectObject(1));
  EXPECT_EQ(0u, layer->GetPromotedObjectCount());
}

TEST_F(CPDFLayerDocumentTest, MalformedRawDeltaFailsClosed) {
  const std::string pdf = BuildSimplePdf();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  const std::string delta = "\n% malformed delta placeholder\n";

  auto layer =
      std::make_unique<CPDF_LayerDocument>(base, MakeStreamForString(delta));

  EXPECT_EQ(CPDF_LayerDocument::OpenStatus::kMalformedDelta,
            layer->ingest_status());
  EXPECT_EQ(0u, layer->GetPromotedObjectCount());
}

TEST_F(CPDFLayerDocumentTest, DeltaFreeEntryOverBaseObjectFailsClosed) {
  const std::string pdf = BuildSimplePdf();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);

  auto layer = std::make_unique<CPDF_LayerDocument>(
      base, MakeStreamForString(BuildFreeEntryDeltaForObject(pdf, 3)));

  EXPECT_EQ(CPDF_LayerDocument::OpenStatus::kMalformedDelta,
            layer->ingest_status());
  EXPECT_EQ(0u, layer->GetPromotedObjectCount());
}

TEST_F(CPDFLayerDocumentTest, DeltaWithCorruptPageTreeFailsClosed) {
  const std::string pdf = BuildSimplePdf();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);

  auto layer = std::make_unique<CPDF_LayerDocument>(
      base, MakeStreamForString(BuildCorruptPagesDelta(pdf)));

  EXPECT_EQ(CPDF_LayerDocument::OpenStatus::kMalformedDelta,
            layer->ingest_status());
  EXPECT_TRUE(layer->FindPromotedObject(2));
}

TEST_F(CPDFLayerDocumentTest, GetMutableIndirectObjectPromotesFromBase) {
  const std::string pdf = BuildSimplePdf();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);

  RetainPtr<CPDF_Object> promoted = layer->GetMutableIndirectObject(1);
  ASSERT_TRUE(promoted);
  EXPECT_EQ(1u, promoted->GetObjNum());
  EXPECT_NE(base->GetFrozenObjectForLayer(1).Get(), promoted.Get());
  EXPECT_EQ(promoted.Get(), layer->FindPromotedObject(1).Get());
  EXPECT_EQ(1u, layer->GetPromotedObjectCount());
  EXPECT_FALSE(promoted->IsFrozen());
  EXPECT_TRUE(base->GetFrozenObjectForLayer(1)->IsFrozen());
}

TEST_F(CPDFLayerDocumentTest, PageMutableDictPromotesAndLeavesBaseFrozen) {
  const std::string pdf = BuildSimplePdf();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);

  auto page = MakeLayerPage(layer.get(), 0);
  ASSERT_TRUE(page);
  EXPECT_EQ(0u, layer->GetPromotedObjectCount());

  RetainPtr<CPDF_Dictionary> page_dict = page->GetMutableDict();
  ASSERT_TRUE(page_dict);
  EXPECT_EQ(3u, page_dict->GetObjNum());
  EXPECT_EQ(1u, layer->GetPromotedObjectCount());
  EXPECT_TRUE(layer->FindPromotedObject(3));
  EXPECT_NE(base->GetFrozenObjectForLayer(3).Get(), page_dict.Get());

  page_dict->SetNewFor<CPDF_Number>("Tier3Marker", 73);
  EXPECT_EQ(73, page->GetDict()->GetIntegerFor("Tier3Marker"));
  ASSERT_TRUE(base->GetFrozenObjectForLayer(3)->AsDictionary());
  EXPECT_FALSE(base->GetFrozenObjectForLayer(3)->AsDictionary()->KeyExist(
      "Tier3Marker"));
}

TEST_F(CPDFLayerDocumentTest, PromotedReferencesResolveThroughLayerHolder) {
  const std::string pdf = BuildSimplePdf();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);

  auto page = MakeLayerPage(layer.get(), 0);
  ASSERT_TRUE(page);
  RetainPtr<CPDF_Dictionary> page_dict = page->GetMutableDict();
  ASSERT_TRUE(page_dict);

  RetainPtr<const CPDF_Reference> parent_ref =
      ToReference(page_dict->GetObjectFor("Parent"));
  ASSERT_TRUE(parent_ref);
  EXPECT_TRUE(parent_ref->HasIndirectObjectHolder());

  RetainPtr<CPDF_Dictionary> parent = page_dict->GetMutableDictFor("Parent");
  ASSERT_TRUE(parent);
  EXPECT_EQ("Pages", parent->GetNameFor("Type"));
  EXPECT_TRUE(layer->FindPromotedObject(2));
  EXPECT_EQ(2u, layer->GetPromotedObjectCount());
  EXPECT_FALSE(base->GetFrozenObjectForLayer(2)->AsDictionary()->KeyExist(
      "Tier3ParentMarker"));

  parent->SetNewFor<CPDF_Number>("Tier3ParentMarker", 91);
  EXPECT_EQ(
      91,
      layer->GetMutableIndirectObject(2)->AsMutableDictionary()->GetIntegerFor(
          "Tier3ParentMarker"));
  EXPECT_FALSE(base->GetFrozenObjectForLayer(2)->AsDictionary()->KeyExist(
      "Tier3ParentMarker"));
}

TEST_F(CPDFLayerDocumentTest, CrossHandleReadRefreshesAfterPagePromotion) {
  const std::string pdf = BuildSimplePdf();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);

  auto page_a = MakeLayerPage(layer.get(), 0);
  auto page_b = MakeLayerPage(layer.get(), 0);
  ASSERT_TRUE(page_a);
  ASSERT_TRUE(page_b);

  page_a->GetMutableDict()->SetNewFor<CPDF_Number>("Foo", 1);
  EXPECT_EQ(1, page_b->GetDict()->GetIntegerFor("Foo"));
  EXPECT_EQ(1u, layer->GetPromotedObjectCount());

  page_b->GetMutableDict()->SetNewFor<CPDF_Number>("Bar", 2);
  EXPECT_EQ(2, page_a->GetDict()->GetIntegerFor("Bar"));
  EXPECT_EQ(1u, layer->GetPromotedObjectCount());
  EXPECT_FALSE(
      base->GetFrozenObjectForLayer(3)->AsDictionary()->KeyExist("Foo"));
  EXPECT_FALSE(
      base->GetFrozenObjectForLayer(3)->AsDictionary()->KeyExist("Bar"));
}

TEST_F(CPDFLayerDocumentTest,
       FrozenReferencesResolveThroughOwningLayerWithoutCrossLayerLeakage) {
  EXPECT_EQ(CPDF_DocumentViewScope::Mode::kNone,
            CPDF_DocumentViewScope::GetCurrentMode());
  const std::string pdf = BuildPdfWithIndirectAnnotation();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  auto layer_a = std::make_unique<CPDF_LayerDocument>(base, nullptr);
  auto layer_b = std::make_unique<CPDF_LayerDocument>(base, nullptr);

  auto page_a = MakeLayerPage(layer_a.get(), 0);
  auto page_b = MakeLayerPage(layer_b.get(), 0);
  ASSERT_TRUE(page_a);
  ASSERT_TRUE(page_b);

  RetainPtr<const CPDF_Array> annots_a = page_a->GetAnnotsArray();
  RetainPtr<const CPDF_Array> annots_b = page_b->GetAnnotsArray();
  ASSERT_TRUE(annots_a);
  ASSERT_TRUE(annots_b);
  ASSERT_TRUE(annots_a->GetDictAt(0));
  ASSERT_TRUE(annots_b->GetDictAt(0));
  EXPECT_FALSE(annots_a->GetDictAt(0)->KeyExist("LayerMarker"));
  EXPECT_FALSE(annots_b->GetDictAt(0)->KeyExist("LayerMarker"));

  RetainPtr<CPDF_Object> promoted_a = layer_a->GetMutableIndirectObject(4);
  ASSERT_TRUE(promoted_a);
  promoted_a->AsMutableDictionary()->SetNewFor<CPDF_Number>("LayerMarker", 41);

  {
    CPDF_DocumentViewScope scope(layer_a.get());
    EXPECT_EQ(CPDF_DocumentViewScope::Mode::kEffective,
              CPDF_DocumentViewScope::GetCurrentMode());
    EXPECT_EQ(41, page_a->GetAnnotsArray()->GetDictAt(0)->GetIntegerFor(
                      "LayerMarker"));
  }
  EXPECT_EQ(CPDF_DocumentViewScope::Mode::kNone,
            CPDF_DocumentViewScope::GetCurrentMode());
  {
    CPDF_DocumentViewScope scope(layer_b.get());
    EXPECT_FALSE(
        page_b->GetAnnotsArray()->GetDictAt(0)->KeyExist("LayerMarker"));
  }

  RetainPtr<CPDF_Object> promoted_b = layer_b->GetMutableIndirectObject(4);
  ASSERT_TRUE(promoted_b);
  promoted_b->AsMutableDictionary()->SetNewFor<CPDF_Number>("LayerMarker", 82);

  {
    CPDF_DocumentViewScope scope_a(layer_a.get());
    EXPECT_EQ(41, page_a->GetAnnotsArray()->GetDictAt(0)->GetIntegerFor(
                      "LayerMarker"));
    {
      CPDF_DocumentViewScope scope_b(layer_b.get());
      EXPECT_EQ(CPDF_DocumentViewScope::Mode::kEffective,
                CPDF_DocumentViewScope::GetCurrentMode());
      EXPECT_EQ(82, page_b->GetAnnotsArray()->GetDictAt(0)->GetIntegerFor(
                        "LayerMarker"));
    }
    EXPECT_EQ(41, page_a->GetAnnotsArray()->GetDictAt(0)->GetIntegerFor(
                      "LayerMarker"));
  }
  EXPECT_FALSE(base->GetFrozenObjectForLayer(4)->AsDictionary()->KeyExist(
      "LayerMarker"));

  {
    CPDF_DocumentViewScope frozen_scope(base.Get());
    EXPECT_EQ(CPDF_DocumentViewScope::Mode::kFrozen,
              CPDF_DocumentViewScope::GetCurrentMode());
    EXPECT_FALSE(base->GetOrParseIndirectObject(4)
                     ->AsDictionary()
                     ->KeyExist("LayerMarker"));
  }
  EXPECT_EQ(CPDF_DocumentViewScope::Mode::kNone,
            CPDF_DocumentViewScope::GetCurrentMode());
}

#if DCHECK_IS_ON()
TEST_F(CPDFLayerDocumentTest,
       DebugDetectorRejectsUnscopedPromotedBaseResolution) {
  RetainPtr<CPDF_BaseDocument> base =
      LoadBaseDocumentFromString(BuildPdfWithIndirectAnnotation());
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);
  ASSERT_TRUE(layer->GetMutableIndirectObject(4));

  EXPECT_DEATH_IF_SUPPORTED(base->GetOrParseIndirectObject(4), "");
  {
    CPDF_DocumentViewScope frozen_scope(base.Get());
    EXPECT_TRUE(base->GetOrParseIndirectObject(4));
  }
  {
    CPDF_DocumentViewScope effective_scope(layer.get());
    EXPECT_EQ(layer->FindPromotedObject(4).Get(),
              base->GetOrParseIndirectObject(4).Get());
  }
}
#endif  // DCHECK_IS_ON()

TEST_F(CPDFLayerDocumentTest,
       ParsedFormRefreshesAfterSiblingPromotesStream) {
  RetainPtr<CPDF_BaseDocument> base =
      LoadBaseDocumentFromString(BuildPdfWithFormXObject());
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);

  RetainPtr<const CPDF_Object> form_object = layer->GetIndirectObject(4);
  ASSERT_TRUE(form_object);
  const CPDF_Stream* form_stream = form_object->AsStream();
  ASSERT_TRUE(form_stream);

  CPDF_Form writer(
      layer.get(), nullptr,
      pdfium::WrapRetain(const_cast<CPDF_Stream*>(form_stream)));
  CPDF_Form reader(
      layer.get(), nullptr,
      pdfium::WrapRetain(const_cast<CPDF_Stream*>(form_stream)));
  writer.ParseContent();
  reader.ParseContent();
  ASSERT_EQ(1u, reader.GetPageObjectCount());
  EXPECT_FLOAT_EQ(10.0f, reader.CalcBoundingBox().Width());
  EXPECT_EQ(1, reader.GetResources()->GetIntegerFor("BaseMarker"));

  RetainPtr<CPDF_Stream> mutable_stream = writer.GetMutableFormStream();
  ASSERT_TRUE(mutable_stream);
  fxcrt::ostringstream updated_content;
  updated_content << "0 0 20 20 re f\n";
  mutable_stream->SetDataFromStringstreamAndRemoveFilter(&updated_content);
  mutable_stream->GetMutableDict()
      ->GetMutableDictFor("Resources")
      ->SetNewFor<CPDF_Number>("LayerMarker", 9);

  reader.ParseContent();
  ASSERT_EQ(1u, reader.GetPageObjectCount());
  EXPECT_FLOAT_EQ(20.0f, reader.CalcBoundingBox().Width());
  EXPECT_EQ(9, reader.GetResources()->GetIntegerFor("LayerMarker"));
}

TEST_F(CPDFLayerDocumentTest, GetOrCreateInfoPromotesBaseInfo) {
  const std::string pdf = BuildSimplePdfWithInfo();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  ASSERT_TRUE(base->GetInfo());
  EXPECT_EQ("Base Title", base->GetInfo()->GetUnicodeTextFor("Title").ToUTF8());

  auto layer_a = std::make_unique<CPDF_LayerDocument>(base, nullptr);
  auto layer_b = std::make_unique<CPDF_LayerDocument>(base, nullptr);
  EXPECT_EQ(0u, layer_a->GetPromotedObjectCount());
  EXPECT_EQ(0u, layer_b->GetPromotedObjectCount());

  RetainPtr<CPDF_Dictionary> info = layer_a->GetOrCreateInfo();
  ASSERT_TRUE(info);
  EXPECT_EQ(4u, info->GetObjNum());
  EXPECT_EQ(1u, layer_a->GetPromotedObjectCount());
  EXPECT_TRUE(layer_a->FindPromotedObject(4));
  EXPECT_NE(base->GetFrozenObjectForLayer(4).Get(), info.Get());

  info->SetNewFor<CPDF_String>("Title", "Layer A Title");

  EXPECT_EQ("Layer A Title",
            layer_a->GetInfo()->GetUnicodeTextFor("Title").ToUTF8());
  EXPECT_EQ("Base Title",
            layer_b->GetInfo()->GetUnicodeTextFor("Title").ToUTF8());
  EXPECT_EQ("Base Title", base->GetInfo()->GetUnicodeTextFor("Title").ToUTF8());
  EXPECT_EQ(0u, layer_b->GetPromotedObjectCount());
}

TEST_F(CPDFLayerDocumentTest,
       GetOrCreateInfoCreatesLayerLocalInfoWhenBaseHasNone) {
  const std::string pdf = BuildSimplePdf();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  EXPECT_FALSE(base->GetInfo());

  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);
  EXPECT_FALSE(layer->GetInfo());
  EXPECT_EQ(0u, layer->GetPromotedObjectCount());

  RetainPtr<CPDF_Dictionary> info = layer->GetOrCreateInfo();
  ASSERT_TRUE(info);
  EXPECT_NE(0u, info->GetObjNum());
  EXPECT_EQ(info.Get(), layer->FindPromotedObject(info->GetObjNum()).Get());
  EXPECT_EQ(1u, layer->GetPromotedObjectCount());

  info->SetNewFor<CPDF_String>("Title", "Layer Title");

  EXPECT_EQ("Layer Title",
            layer->GetInfo()->GetUnicodeTextFor("Title").ToUTF8());
  EXPECT_FALSE(base->GetInfo());
}

TEST_F(CPDFLayerDocumentTest, DirectResourcesPromoteOwningPage) {
  const std::string pdf = BuildPdfWithDirectResources();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);

  auto page = MakeLayerPage(layer.get(), 0);
  auto second_page_handle = MakeLayerPage(layer.get(), 0);
  ASSERT_TRUE(page);
  ASSERT_TRUE(second_page_handle);
  RetainPtr<CPDF_Dictionary> resources = page->GetMutableResources();
  ASSERT_TRUE(resources);
  EXPECT_EQ(1u, layer->GetPromotedObjectCount());

  resources->SetNewFor<CPDF_Number>("Tier3ResourceMarker", 5);
  EXPECT_EQ(5, page->GetResources()->GetIntegerFor("Tier3ResourceMarker"));
  EXPECT_EQ(5, second_page_handle->GetResources()->GetIntegerFor(
                   "Tier3ResourceMarker"));

  RetainPtr<const CPDF_Dictionary> base_page =
      base->GetFrozenObjectForLayer(3)->GetDict();
  ASSERT_TRUE(base_page);
  RetainPtr<const CPDF_Dictionary> base_resources =
      base_page->GetDictFor("Resources");
  ASSERT_TRUE(base_resources);
  EXPECT_FALSE(base_resources->KeyExist("Tier3ResourceMarker"));
}

#if DCHECK_IS_ON()
TEST_F(CPDFLayerDocumentTest, ParseIndirectObjectStillUnsupportedOnLayer) {
  const std::string pdf = BuildSimplePdf();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);

  EXPECT_DEATH_IF_SUPPORTED(layer->ParseIndirectObject(1), "");
}
#endif  // DCHECK_IS_ON()


TEST_F(CPDFLayerDocumentTest, LazyBaseParseIsSharedAndNeverLeaksPromotions) {
  const std::string pdf = BuildPdfWithFormXObject();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  // Loading touched the catalog and page tree only: the form XObject (4)
  // was not walked.
  EXPECT_FALSE(base->GetIndirectObject(4));

  auto layer_a = std::make_unique<CPDF_LayerDocument>(base, nullptr);
  auto layer_b = std::make_unique<CPDF_LayerDocument>(base, nullptr);

  // Layer A touches the form first: parsed once, frozen, into the base.
  RetainPtr<const CPDF_Object> via_a = layer_a->GetIndirectObject(4);
  ASSERT_TRUE(via_a);
  EXPECT_TRUE(via_a->IsFrozen());
  EXPECT_EQ(via_a.Get(), base->GetIndirectObject(4).Get());
  EXPECT_EQ(0u, layer_a->GetPromotedObjectCount());
  // Layer B sees the same shared object, no second parse.
  EXPECT_EQ(via_a.Get(), layer_b->GetIndirectObject(4).Get());
  EXPECT_EQ(0u, layer_b->GetPromotedObjectCount());

  // Layer A writes: a clone is promoted into A alone; the base keeps its
  // frozen object and B keeps reading it.
  RetainPtr<CPDF_Object> mutable_a = layer_a->GetMutableIndirectObject(4);
  ASSERT_TRUE(mutable_a);
  EXPECT_NE(mutable_a.Get(), via_a.Get());
  EXPECT_FALSE(mutable_a->IsFrozen());
  EXPECT_EQ(1u, layer_a->GetPromotedObjectCount());
  EXPECT_EQ(via_a.Get(), base->GetIndirectObject(4).Get());
  EXPECT_EQ(via_a.Get(), layer_b->GetIndirectObject(4).Get());
  EXPECT_EQ(0u, layer_b->GetPromotedObjectCount());

  // With A's effective view active, a base object nobody has touched yet
  // (the page, 3, references the promoted 4) still parses from the bytes
  // and never picks up A's clone.
  {
    CPDF_DocumentViewScope effective_a(layer_a.get());
    RetainPtr<const CPDF_Object> page = base->GetFrozenObjectForLayer(3);
    ASSERT_TRUE(page);
    EXPECT_TRUE(page->IsFrozen());
    RetainPtr<const CPDF_Dictionary> xobjects =
        page->AsDictionary()->GetDictFor("Resources")->GetDictFor("XObject");
    ASSERT_TRUE(xobjects);
    // Through the base's frozen view the reference resolves to the shared
    // frozen form, not to A's promoted clone.
    CPDF_DocumentViewScope frozen(base.Get());
    EXPECT_EQ(via_a.Get(), xobjects->GetObjectFor("Fm0")->GetDirect().Get());
  }
  for (const auto& item : *base) {
    if (item.second) {
      EXPECT_TRUE(item.second->IsFrozen()) << "base object " << item.first;
      EXPECT_NE(item.second.Get(), mutable_a.Get());
    }
  }
}

// ---------------------------------------------------------------------------
// EmbedPDF: the two twins of an overlay object. "Differs from base" decides
// what a cumulative save writes; "differs from loaded" decides whether a save
// is a no-op. They agree on a fresh layer and disagree on a reopened one.
// ---------------------------------------------------------------------------

namespace {

// Object 4 (the annotation) rewritten with a wider /Rect, as one revision
// appended to |base_pdf|: what a saved layer delta looks like.
std::string BuildAnnotationRectDelta(const std::string& base_pdf) {
  std::ostringstream delta;
  delta << "4 0 obj\n"
        << "<< /Type /Annot /Subtype /Polygon /Rect [10 10 40 40]\n"
        << "   /Vertices [10 10 30 10 20 30] >>\n"
        << "endobj\n";
  const size_t xref_offset = base_pdf.size() + delta.tellp();
  delta << "xref\n4 1\n"
        << std::setw(10) << std::setfill('0') << base_pdf.size()
        << " 00000 n \n"
        << "trailer\n<< /Size 5 /Root 1 0 R /Prev "
        << GetStartXrefOffsetFromPdf(base_pdf) << " >>\nstartxref\n"
        << xref_offset << "\n%%EOF\n";
  return delta.str();
}

RetainPtr<CPDF_Dictionary> MutableAnnot(CPDF_LayerDocument* layer) {
  RetainPtr<CPDF_Object> object = layer->GetMutableIndirectObject(4);
  return object ? pdfium::WrapRetain(object->AsMutableDictionary()) : nullptr;
}

}  // namespace

TEST_F(CPDFLayerDocumentTest, TwinsAgreeOnAFreshLayer) {
  const std::string pdf = BuildPdfWithIndirectAnnotation();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);
  ASSERT_EQ(CPDF_LayerDocument::OpenStatus::kSuccess, layer->ingest_status());

  // Not in the overlay: neither differs, and the loaded twin is the base's.
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kBase));
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kLoaded));
  EXPECT_EQ(base->GetFrozenObjectForLayer(3).Get(),
            layer->GetLoadedTwin(3).Get());

  // Touched, not changed.
  RetainPtr<CPDF_Dictionary> annot = MutableAnnot(layer.get());
  ASSERT_TRUE(annot);
  EXPECT_TRUE(layer->IsObjectPromoted(4));
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kBase));
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kLoaded));

  // Changed.
  annot->SetRectFor("Rect", CFX_FloatRect(10, 10, 40, 40));
  EXPECT_FALSE(SameAsTwin(layer.get(), 4, TwinView::kBase));
  EXPECT_FALSE(SameAsTwin(layer.get(), 4, TwinView::kLoaded));

  // Restored: both sides go through the same writer, so the base's
  // integers and our floats compare as the bytes the creator would write.
  annot->SetRectFor("Rect", CFX_FloatRect(10, 10, 30, 30));
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kBase));
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kLoaded));

  // New here: no twin at all.
  RetainPtr<CPDF_Dictionary> fresh = layer->NewIndirect<CPDF_Dictionary>();
  ASSERT_TRUE(fresh);
  EXPECT_FALSE(SameAsTwin(layer.get(), fresh->GetObjNum(), TwinView::kBase));
  EXPECT_FALSE(SameAsTwin(layer.get(), fresh->GetObjNum(), TwinView::kLoaded));
}

TEST_F(CPDFLayerDocumentTest, TwinsDisagreeOnAReopenedLayer) {
  const std::string pdf = BuildPdfWithIndirectAnnotation();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(
      base, MakeStreamForString(BuildAnnotationRectDelta(pdf)));
  ASSERT_EQ(CPDF_LayerDocument::OpenStatus::kSuccess, layer->ingest_status());
  ASSERT_EQ(1u, layer->GetPromotedObjectCount());

  // Ingested and untouched: differs from the base, not from the loaded file.
  EXPECT_FALSE(SameAsTwin(layer.get(), 4, TwinView::kBase));
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kLoaded));
  RetainPtr<const CPDF_Object> twin = layer->GetLoadedTwin(4);
  ASSERT_TRUE(twin);
  EXPECT_NE(base->GetFrozenObjectForLayer(4).Get(), twin.Get());
  EXPECT_EQ(40, twin->AsDictionary()->GetRectFor("Rect").right);

  // Reverted to the base value: nothing to write, yet changed since load.
  RetainPtr<CPDF_Dictionary> annot = MutableAnnot(layer.get());
  ASSERT_TRUE(annot);
  annot->SetRectFor("Rect", CFX_FloatRect(10, 10, 30, 30));
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kBase));
  EXPECT_FALSE(SameAsTwin(layer.get(), 4, TwinView::kLoaded));

  // Back to the loaded value.
  annot->SetRectFor("Rect", CFX_FloatRect(10, 10, 40, 40));
  EXPECT_FALSE(SameAsTwin(layer.get(), 4, TwinView::kBase));
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kLoaded));

  // The loaded twin itself never moved.
  EXPECT_EQ(40, layer->GetLoadedTwin(4)->AsDictionary()->GetRectFor("Rect").right);

  // Deleted: the delta carried it, it is gone - changed since load; the
  // base's copy stands, so it does not differ from the base.
  annot.Reset();
  layer->DeleteIndirectObject(4);
  EXPECT_FALSE(layer->IsObjectPromoted(4));
  EXPECT_FALSE(SameAsTwin(layer.get(), 4, TwinView::kLoaded));
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kBase));
}

// ---------------------------------------------------------------------------
// EmbedPDF M1: streams share file-backed bytes within a lineage.
// ---------------------------------------------------------------------------

namespace {

// Object 4 (the form XObject stream) rewritten with other content, as one
// revision appended to |base_pdf|.
std::string BuildStreamDelta(const std::string& base_pdf) {
  const std::string content = "0 0 20 20 re f\n";
  std::ostringstream delta;
  delta << "4 0 obj\n"
        << "<< /Type /XObject /Subtype /Form /BBox [0 0 100 100]\n"
        << "   /Resources << /BaseMarker 1 >> /Length " << content.size()
        << " >>\nstream\n"
        << content << "endstream\nendobj\n";
  const size_t xref_offset = base_pdf.size() + delta.tellp();
  delta << "xref\n4 1\n"
        << std::setw(10) << std::setfill('0') << base_pdf.size()
        << " 00000 n \n"
        << "trailer\n<< /Size 6 /Root 1 0 R /Prev "
        << GetStartXrefOffsetFromPdf(base_pdf) << " >>\nstartxref\n"
        << xref_offset << "\n%%EOF\n";
  return delta.str();
}

}  // namespace

TEST_F(CPDFLayerDocumentTest, PromotedStreamSharesTheBaseView) {
  const std::string pdf = BuildPdfWithFormXObject();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(base, nullptr);
  ASSERT_EQ(CPDF_LayerDocument::OpenStatus::kSuccess, layer->ingest_status());

  RetainPtr<const CPDF_Stream> twin = ToStream(base->GetFrozenObjectForLayer(4));
  ASSERT_TRUE(twin);
  ASSERT_TRUE(twin->IsFileBased());

  // Promotion clones for the layer: the bytes stay where they are.
  RetainPtr<CPDF_Stream> promoted =
      ToStream(layer->GetMutableIndirectObject(4));
  ASSERT_TRUE(promoted);
  EXPECT_TRUE(promoted->IsFileBased());
  EXPECT_EQ(twin->BackingView(), promoted->BackingView());
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kBase));  // identity: O(1)
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kLoaded));

  // A write gives the clone its own buffer; the base keeps its view.
  const uint8_t bytes[] = "1 0 0 rg 0 0 5 5 re f\n";
  promoted->SetData(pdfium::span(bytes).first(sizeof(bytes) - 1));
  EXPECT_TRUE(promoted->IsMemoryBased());
  EXPECT_TRUE(twin->IsFileBased());
  EXPECT_FALSE(SameAsTwin(layer.get(), 4, TwinView::kBase));

  // A plain clone (the cross-document path) copies, as it always did.
  RetainPtr<CPDF_Object> copy = twin->Clone();
  ASSERT_TRUE(copy && copy->IsStream());
  EXPECT_TRUE(copy->AsStream()->IsMemoryBased());
}

TEST_F(CPDFLayerDocumentTest, IngestedStreamAndItsTwinShareOneView) {
  const std::string pdf = BuildPdfWithFormXObject();
  RetainPtr<CPDF_BaseDocument> base = LoadBaseDocumentFromString(pdf);
  ASSERT_TRUE(base);
  auto layer = std::make_unique<CPDF_LayerDocument>(
      base, MakeStreamForString(BuildStreamDelta(pdf)));
  ASSERT_EQ(CPDF_LayerDocument::OpenStatus::kSuccess, layer->ingest_status());
  ASSERT_EQ(1u, layer->GetPromotedObjectCount());

  RetainPtr<const CPDF_Stream> overlay = ToStream(layer->GetIndirectObject(4));
  RetainPtr<const CPDF_Stream> loaded = ToStream(layer->GetLoadedTwin(4));
  ASSERT_TRUE(overlay && loaded);
  EXPECT_TRUE(overlay->IsFileBased());
  EXPECT_TRUE(loaded->IsFileBased());
  EXPECT_EQ(overlay->BackingView(), loaded->BackingView());  // no second copy
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kLoaded));  // identity
  EXPECT_FALSE(SameAsTwin(layer.get(), 4,
                          TwinView::kBase));  // exact compare: content differs

  // Replace the data, then restore the loaded bytes: a different view, the
  // same bytes - the exact fallback says unchanged.
  RetainPtr<CPDF_Stream> mutable_overlay =
      ToStream(layer->GetMutableIndirectObject(4));
  const uint8_t other[] = "0 0 1 1 re f\n";
  mutable_overlay->SetData(pdfium::span(other).first(sizeof(other) - 1));
  EXPECT_FALSE(SameAsTwin(layer.get(), 4, TwinView::kLoaded));
  const std::string restored = "0 0 20 20 re f\n";
  mutable_overlay->SetData(pdfium::as_bytes(pdfium::span(restored)));
  EXPECT_TRUE(mutable_overlay->IsMemoryBased());
  EXPECT_NE(mutable_overlay->BackingView(), loaded->BackingView());
  EXPECT_TRUE(SameAsTwin(layer.get(), 4, TwinView::kLoaded));
}
