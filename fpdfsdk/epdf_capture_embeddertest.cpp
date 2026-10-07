// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Captures and their imports (public/epdf_capture.h). A delete captures the
// layer's versions of what it removes; an import puts them back at the same
// numbers, positions and links, also after the layer was saved and reopened
// in between, which drops everything the delete left behind. An import never
// replaces a version the layer holds, and never copies the uploaded file's
// originals.

#include <stdint.h>

#include <string>
#include <vector>

#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_capture.h"
#include "public/epdf_form.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using testing::ElementsAre;

namespace {

constexpr unsigned long kRaised = 40;

// The objects of the document below.
constexpr uint32_t kFileSquare = 5;
constexpr uint32_t kFileNote = 6;
constexpr uint32_t kFilePopup = 7;
constexpr uint32_t kFileReply = 8;
constexpr uint32_t kMergedField = 9;
constexpr uint32_t kFileSquareAp = 10;
constexpr uint32_t kGroupField = 12;
constexpr uint32_t kChildField = 13;
constexpr uint32_t kChildWidget = 14;

// Two pages and a form. The first page holds, from the file: a blue square
// with an appearance (5, its stream 10), a note (6) with its popup (7) and a
// reply to it (8), a merged text field and widget `name` (9), and the widget
// (14) of `group.child` (13), the only kid of `group` (12). The second page
// is empty.
std::string MakeDocument() {
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [9 0 R 12 0 R] "
      "/DA (/Helv 0 Tf 0 g) /DR << /Font << /Helv << /Type /Font "
      "/Subtype /Type1 /BaseFont /Helvetica >> >> >> >> >>",
      "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 11 0 R "
      "/Resources << >> /Annots [5 0 R 6 0 R 7 0 R 8 0 R 9 0 R 14 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 11 0 R "
      "/Resources << >> >>",
      "<< /Type /Annot /Subtype /Square /Rect [20 200 80 260] /C [0 0 1] "
      "/P 3 0 R /AP << /N 10 0 R >> >>",
      "<< /Type /Annot /Subtype /Text /Rect [120 220 140 240] /Contents (Note) "
      "/T (Ann) /Popup 7 0 R /P 3 0 R >>",
      "<< /Type /Annot /Subtype /Popup /Rect [150 180 250 240] /Parent 6 0 R "
      "/P 3 0 R >>",
      "<< /Type /Annot /Subtype /Text /Rect [120 220 140 240] "
      "/Contents (Reply) /T (Bea) /IRT 6 0 R /P 3 0 R >>",
      "<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /V (Ann) "
      "/Rect [20 20 180 44] /P 3 0 R >>",
      "<< /Type /XObject /Subtype /Form /BBox [0 0 60 60] /Length 23 >>\n"
      "stream\n0 0 1 rg 0 0 60 60 re f\nendstream",
      Stream(""),
      "<< /T (group) /Kids [13 0 R] >>",
      "<< /T (child) /FT /Tx /Parent 12 0 R /Kids [14 0 R] >>",
      "<< /Type /Annot /Subtype /Widget /Parent 13 0 R /Rect [20 60 180 84] "
      "/P 3 0 R >>",
  });
}

CPDF_LayerDocument* LayerOf(FPDF_DOCUMENT doc) {
  return CPDF_LayerDocument::FromDocument(CPDFDocumentFromFPDFDocument(doc));
}

// The object numbers |array| refers to, in order.
std::vector<uint32_t> RefsIn(const CPDF_Array* array) {
  std::vector<uint32_t> refs;
  for (size_t i = 0; array && i < array->size(); ++i) {
    RetainPtr<const CPDF_Object> entry = array->GetObjectAt(i);
    const CPDF_Reference* ref = ToReference(entry.Get());
    refs.push_back(ref ? ref->GetRefObjNum() : 0);
  }
  return refs;
}

// Page |page_index|'s /Annots.
std::vector<uint32_t> Annots(FPDF_DOCUMENT doc, int page_index) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  CPDF_DocumentViewScope view(pdf);
  RetainPtr<const CPDF_Dictionary> page = pdf->GetPageDictionary(page_index);
  return RefsIn(page->GetArrayFor("Annots").Get());
}

// The dictionary |objnum| as the document reads it, or null.
RetainPtr<const CPDF_Dictionary> DictOf(FPDF_DOCUMENT doc, uint32_t objnum) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  CPDF_DocumentViewScope view(pdf);
  return ToDictionary(pdf->GetOrParseIndirectObject(objnum));
}

// The object number |objnum| refers to under |key|, or 0.
uint32_t RefOf(FPDF_DOCUMENT doc, uint32_t objnum, const char* key) {
  RetainPtr<const CPDF_Dictionary> dict = DictOf(doc, objnum);
  RetainPtr<const CPDF_Object> value = dict ? dict->GetObjectFor(key) : nullptr;
  const CPDF_Reference* ref = ToReference(value.Get());
  return ref ? ref->GetRefObjNum() : 0;
}

// The normal appearance stream of the annotation |objnum|, or 0.
uint32_t NormalAppearanceOf(FPDF_DOCUMENT doc, uint32_t objnum) {
  RetainPtr<const CPDF_Dictionary> dict = DictOf(doc, objnum);
  RetainPtr<const CPDF_Dictionary> ap = dict ? dict->GetDictFor("AP") : nullptr;
  RetainPtr<const CPDF_Object> normal = ap ? ap->GetObjectFor("N") : nullptr;
  const CPDF_Reference* ref = ToReference(normal.Get());
  return ref ? ref->GetRefObjNum() : 0;
}

// A stream's bytes as stored, still encoded.
std::string RawBytesOf(FPDF_DOCUMENT doc, uint32_t objnum) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  CPDF_DocumentViewScope view(pdf);
  RetainPtr<const CPDF_Stream> stream =
      ToStream(pdf->GetOrParseIndirectObject(objnum));
  if (!stream) {
    return std::string();
  }
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(stream);
  acc->LoadAllDataRaw();
  const ByteStringView bytes(acc->GetSpan());
  return std::string(bytes.unterminated_c_str(), bytes.GetLength());
}

// The colour /C of the annotation |objnum|, as "r g b".
std::string ColorOf(FPDF_DOCUMENT doc, uint32_t objnum) {
  RetainPtr<const CPDF_Dictionary> dict = DictOf(doc, objnum);
  RetainPtr<const CPDF_Array> color = dict ? dict->GetArrayFor("C") : nullptr;
  std::string out;
  for (size_t i = 0; color && i < color->size(); ++i) {
    out += (i ? " " : "") + std::to_string(color->GetFloatAt(i));
  }
  return out;
}

// The bytes of an owned buffer, released.
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

std::vector<uint8_t> ExportAnnots(FPDF_DOCUMENT doc,
                                  int page_index,
                                  std::vector<int> indexes) {
  unsigned long size = 0;
  void* buffer = EPDFPage_ExportAnnotsRawToOwnedBuffer(
      doc, page_index, indexes.data(), static_cast<int>(indexes.size()), &size);
  return Take(buffer, size);
}

bool ImportAnnots(FPDF_DOCUMENT doc,
                  int page_index,
                  const std::vector<uint8_t>& capture) {
  return EPDFPage_ImportAnnotsRaw(doc, page_index, capture.data(),
                                  static_cast<unsigned long>(capture.size()));
}

std::vector<uint8_t> ExportDict(FPDF_DOCUMENT doc,
                                uint32_t objnum,
                                const char* deep_keys) {
  unsigned long size = 0;
  void* buffer =
      EPDFDoc_ExportDictRawToOwnedBuffer(doc, objnum, deep_keys, &size);
  return Take(buffer, size);
}

bool ImportDict(FPDF_DOCUMENT doc, const std::vector<uint8_t>& capture) {
  return EPDFDoc_ImportDictRaw(doc, capture.data(),
                               static_cast<unsigned long>(capture.size()));
}

std::vector<uint8_t> ExportField(FPDF_DOCUMENT doc, uint32_t field_objnum) {
  unsigned long size = 0;
  void* buffer = EPDFForm_ExportFieldRawToOwnedBuffer(doc, field_objnum, &size);
  return Take(buffer, size);
}

bool ImportField(FPDF_DOCUMENT doc, const std::vector<uint8_t>& capture) {
  return EPDFForm_ImportFieldRaw(doc, capture.data(),
                                 static_cast<unsigned long>(capture.size()));
}

// A new form XObject drawing |content|, in the open transaction.
uint32_t NewAppearance(FPDF_DOCUMENT doc, const std::string& content) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  auto dict = pdfium::MakeRetain<CPDF_Dictionary>();
  dict->SetNewFor<CPDF_Name>("Type", "XObject");
  dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  RetainPtr<CPDF_Stream> stream = pdf->NewIndirect<CPDF_Stream>(dict);
  stream->SetData(ByteStringView(content.c_str()).unsigned_span());
  return stream->GetObjNum();
}

// The annotation |objnum|'s dictionary, ready to write.
RetainPtr<CPDF_Dictionary> WritableDict(FPDF_DOCUMENT doc, uint32_t objnum) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  CPDF_DocumentViewScope view(pdf);
  return ToDictionary(pdf->GetMutableIndirectObject(objnum));
}

// Gives the annotation |objnum| the colour |r g b| and a new appearance.
uint32_t Recolor(FPDF_DOCUMENT doc,
                 uint32_t objnum,
                 float r,
                 float g,
                 float b) {
  const uint32_t appearance = NewAppearance(doc, "1 0 0 rg 0 0 60 60 re f");
  RetainPtr<CPDF_Dictionary> dict = WritableDict(doc, objnum);
  auto color = dict->SetNewFor<CPDF_Array>("C");
  color->AppendNew<CPDF_Number>(r);
  color->AppendNew<CPDF_Number>(g);
  color->AppendNew<CPDF_Number>(b);
  auto ap = dict->SetNewFor<CPDF_Dictionary>("AP");
  ap->SetNewFor<CPDF_Reference>("N", CPDFDocumentFromFPDFDocument(doc),
                                appearance);
  return appearance;
}

// Creates a square at |objnum| on page |page_index| with an appearance.
// Returns the appearance's object number.
uint32_t CreateSquare(FPDF_DOCUMENT doc, int page_index, uint32_t objnum) {
  ScopedFPDFAnnotation annot(
      EPDFPage_CreateAnnotRaw(doc, page_index, FPDF_ANNOT_SQUARE, objnum));
  if (!annot) {
    return 0;
  }
  return Recolor(doc, objnum, 0, 1, 0);
}

}  // namespace

class EPDFCaptureEmbedderTest : public EmbedderTest {
 protected:
  void SetUp() override {
    EmbedderTest::SetUp();
    input_ = MakeDocument();
    base_ = EPDF_LoadMemBaseDocument(input_.data(), input_.size(), nullptr);
    ASSERT_TRUE(base_);
  }

  void TearDown() override {
    EPDF_ReleaseBaseDocument(base_);
    EmbedderTest::TearDown();
  }

  ScopedFPDFDocument OpenLayer() {
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base_, nullptr, nullptr, nullptr));
    if (layer && !EPDFLayer_RaiseLastObjectNumber(layer.get(), kRaised)) {
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

  std::string input_;
  std::string artifact_;
  EPDF_BASE_DOCUMENT base_ = nullptr;
};

// Layer-made annotations come back at their numbers and positions, their
// appearance with them, also when a save and reopen dropped everything the
// delete left behind.
TEST_F(EPDFCaptureEmbedderTest, DeletedLayerAnnotationsComeBack) {
  for (bool reopen : {false, true}) {
    SCOPED_TRACE(reopen ? "reopened between delete and import"
                        : "same session");
    ScopedFPDFDocument layer = OpenLayer();
    ASSERT_TRUE(layer);
    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    const uint32_t first_ap = CreateSquare(layer.get(), 1, 20);
    const uint32_t second_ap = CreateSquare(layer.get(), 1, 21);
    ASSERT_TRUE(first_ap && second_ap);
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    const std::string first_bytes = RawBytesOf(layer.get(), first_ap);

    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    const std::vector<uint8_t> capture = ExportAnnots(layer.get(), 1, {0});
    ASSERT_FALSE(capture.empty());
    ASSERT_TRUE(EPDFPage_RemoveAnnotRaw(layer.get(), 1, 0));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    EXPECT_THAT(Annots(layer.get(), 1), ElementsAre(21));
    EXPECT_FALSE(DictOf(layer.get(), 20));

    ScopedFPDFDocument target = reopen ? Reopen(layer.get()) : std::move(layer);
    ASSERT_TRUE(target);
    if (reopen) {
      EXPECT_TRUE(RawBytesOf(target.get(), first_ap).empty());
    }
    ASSERT_TRUE(EPDFLayer_BeginTransaction(target.get()));
    ASSERT_TRUE(ImportAnnots(target.get(), 1, capture));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(target.get()));
    EXPECT_THAT(Annots(target.get(), 1), ElementsAre(20, 21));
    EXPECT_EQ(first_ap, NormalAppearanceOf(target.get(), 20));
    EXPECT_EQ(first_bytes, RawBytesOf(target.get(), first_ap));
    EXPECT_EQ("0.000000 1.000000 0.000000", ColorOf(target.get(), 20));
  }
}

// A note from the file goes with its popup and its reply, and comes back
// with them, in place and linked. None was edited, so each comes back as the
// file's original: the capture holds no copy, and the layer no version.
TEST_F(EPDFCaptureEmbedderTest, FileAnnotationsComeBackAsTheOriginals) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportAnnots(layer.get(), 0, {1, 2, 3});
  ASSERT_FALSE(capture.empty());
  const std::string text(capture.begin(), capture.end());
  EXPECT_EQ(std::string::npos, text.find(" obj"));
  for (int index : {3, 2, 1}) {
    ASSERT_TRUE(EPDFPage_RemoveAnnotRaw(layer.get(), 0, index));
  }
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_THAT(Annots(layer.get(), 0),
              ElementsAre(kFileSquare, kMergedField, kChildWidget));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(ImportAnnots(layer.get(), 0, capture));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_THAT(Annots(layer.get(), 0),
              ElementsAre(kFileSquare, kFileNote, kFilePopup, kFileReply,
                          kMergedField, kChildWidget));
  EXPECT_EQ(kFilePopup, RefOf(layer.get(), kFileNote, "Popup"));
  EXPECT_EQ(kFileNote, RefOf(layer.get(), kFilePopup, "Parent"));
  EXPECT_EQ(kFileNote, RefOf(layer.get(), kFileReply, "IRT"));
  for (uint32_t objnum : {kFileNote, kFilePopup, kFileReply}) {
    EXPECT_FALSE(LayerOf(layer.get())->FindLayerVersion(objnum));
  }
}

// The review's case: a square from the file, recoloured by the layer with a
// new appearance, then deleted. The import brings back the edit and its
// appearance over the original, after a save and reopen too.
TEST_F(EPDFCaptureEmbedderTest, EditedFileAnnotationComesBackWithItsEdits) {
  for (bool reopen : {false, true}) {
    SCOPED_TRACE(reopen ? "reopened between delete and import"
                        : "same session");
    ScopedFPDFDocument layer = OpenLayer();
    ASSERT_TRUE(layer);
    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    const uint32_t appearance = Recolor(layer.get(), kFileSquare, 1, 0, 0);
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    const std::string bytes = RawBytesOf(layer.get(), appearance);

    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    const std::vector<uint8_t> capture = ExportAnnots(layer.get(), 0, {0});
    ASSERT_TRUE(EPDFPage_RemoveAnnotRaw(layer.get(), 0, 0));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    // The layer stopped referencing the square; the original shows through.
    EXPECT_EQ("0.000000 0.000000 1.000000", ColorOf(layer.get(), kFileSquare));

    ScopedFPDFDocument target = reopen ? Reopen(layer.get()) : std::move(layer);
    ASSERT_TRUE(target);
    ASSERT_TRUE(EPDFLayer_BeginTransaction(target.get()));
    ASSERT_TRUE(ImportAnnots(target.get(), 0, capture));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(target.get()));
    EXPECT_EQ(kFileSquare, Annots(target.get(), 0)[0]);
    EXPECT_EQ("1.000000 0.000000 0.000000", ColorOf(target.get(), kFileSquare));
    EXPECT_EQ(appearance, NormalAppearanceOf(target.get(), kFileSquare));
    EXPECT_EQ(bytes, RawBytesOf(target.get(), appearance));
    // The file's original appearance is untouched and was never copied.
    EXPECT_FALSE(LayerOf(target.get())->FindLayerVersion(kFileSquareAp));
  }
}

// An object two annotations share, held by the layer and changed after the
// delete, keeps its newer version: the import never replaces what the layer
// holds, and doesn't copy it again.
TEST_F(EPDFCaptureEmbedderTest, ImportNeverReplacesAVersionTheLayerHolds) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const uint32_t shared = NewAppearance(layer.get(), "0 0 1 rg 0 0 9 9 re f");
  for (uint32_t objnum : {20u, 21u}) {
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnotRaw(layer.get(), 1, FPDF_ANNOT_SQUARE, objnum));
    ASSERT_TRUE(annot);
    auto ap =
        WritableDict(layer.get(), objnum)->SetNewFor<CPDF_Dictionary>("AP");
    ap->SetNewFor<CPDF_Reference>(
        "N", CPDFDocumentFromFPDFDocument(layer.get()), shared);
  }
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportAnnots(layer.get(), 1, {0});
  ASSERT_TRUE(EPDFPage_RemoveAnnotRaw(layer.get(), 1, 0));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));

  // The other annotation's use grows the shared object.
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  {
    CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(layer.get());
    CPDF_DocumentViewScope view(pdf);
    RetainPtr<CPDF_Stream> stream =
        ToStream(pdf->GetMutableIndirectObject(shared));
    stream->SetData(
        ByteStringView("0 0 1 rg 0 0 9 9 re f 1 g").unsigned_span());
  }
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(ImportAnnots(layer.get(), 1, capture));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_THAT(Annots(layer.get(), 1), ElementsAre(20, 21));
  EXPECT_EQ(shared, NormalAppearanceOf(layer.get(), 20));
  EXPECT_EQ("0 0 1 rg 0 0 9 9 re f 1 g", RawBytesOf(layer.get(), shared));
}

// An import in a transaction that aborts leaves the layer as it was.
TEST_F(EPDFCaptureEmbedderTest, AbortedImportLeavesNothing) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(CreateSquare(layer.get(), 1, 20));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportAnnots(layer.get(), 1, {0});
  ASSERT_TRUE(EPDFPage_RemoveAnnotRaw(layer.get(), 1, 0));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(ImportAnnots(layer.get(), 1, capture));
  EXPECT_THAT(Annots(layer.get(), 1), ElementsAre(20));
  ASSERT_TRUE(EPDFLayer_AbortTransaction(layer.get()));
  EXPECT_TRUE(Annots(layer.get(), 1).empty());
  EXPECT_FALSE(DictOf(layer.get(), 20));

  // And the capture still applies afterwards.
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(ImportAnnots(layer.get(), 1, capture));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_THAT(Annots(layer.get(), 1), ElementsAre(20));
}

// What an import refuses, before writing anything: no transaction, the wrong
// page, bytes that aren't a capture, a number the layer never handed out.
TEST_F(EPDFCaptureEmbedderTest, ImportRefusesWhatDoesNotFit) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(CreateSquare(layer.get(), 1, 30));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportAnnots(layer.get(), 1, {0});
  ASSERT_TRUE(EPDFPage_RemoveAnnotRaw(layer.get(), 1, 0));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));

  EXPECT_FALSE(ImportAnnots(layer.get(), 1, capture));  // no transaction
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  EXPECT_FALSE(ImportAnnots(layer.get(), 0, capture));  // another page
  const std::string garbage = "%EPDF-CAPTURE 1\n<< /Kind /Annots";
  EXPECT_FALSE(
      EPDFPage_ImportAnnotsRaw(layer.get(), 1, garbage.data(), garbage.size()));
  EXPECT_FALSE(ImportDict(layer.get(), capture));  // another kind
  ASSERT_TRUE(EPDFLayer_AbortTransaction(layer.get()));

  // A layer that never handed out 30 can't take it.
  ScopedFPDFDocument fresh(
      EPDFLayer_OpenLayer(base_, nullptr, nullptr, nullptr));
  ASSERT_TRUE(EPDFLayer_RaiseLastObjectNumber(fresh.get(), 20));
  ASSERT_TRUE(EPDFLayer_BeginTransaction(fresh.get()));
  EXPECT_FALSE(ImportAnnots(fresh.get(), 1, capture));
  EXPECT_TRUE(Annots(fresh.get(), 1).empty());
  ASSERT_TRUE(EPDFLayer_AbortTransaction(fresh.get()));
}

// A dictionary captured before an update comes back exactly: every key as it
// was, none added, and the appearance under a deep key with its bytes, also
// after a save and reopen dropped the old appearance.
TEST_F(EPDFCaptureEmbedderTest, DictionaryComesBackAsItWas) {
  for (bool reopen : {false, true}) {
    SCOPED_TRACE(reopen ? "reopened between update and import"
                        : "same session");
    ScopedFPDFDocument layer = OpenLayer();
    ASSERT_TRUE(layer);
    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    const uint32_t before_ap = CreateSquare(layer.get(), 1, 20);
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    const std::string before_bytes = RawBytesOf(layer.get(), before_ap);

    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    const std::vector<uint8_t> capture = ExportDict(layer.get(), 20, "AP");
    ASSERT_FALSE(capture.empty());
    const uint32_t after_ap = Recolor(layer.get(), 20, 1, 0, 0);
    WritableDict(layer.get(), 20)->SetNewFor<CPDF_String>("Contents", "later");
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    ASSERT_NE(before_ap, after_ap);

    ScopedFPDFDocument target = reopen ? Reopen(layer.get()) : std::move(layer);
    ASSERT_TRUE(target);
    if (reopen) {
      EXPECT_TRUE(RawBytesOf(target.get(), before_ap).empty());
    }
    ASSERT_TRUE(EPDFLayer_BeginTransaction(target.get()));
    ASSERT_TRUE(ImportDict(target.get(), capture));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(target.get()));
    EXPECT_EQ("0.000000 1.000000 0.000000", ColorOf(target.get(), 20));
    EXPECT_FALSE(DictOf(target.get(), 20)->KeyExist("Contents"));
    EXPECT_EQ(before_ap, NormalAppearanceOf(target.get(), 20));
    EXPECT_EQ(before_bytes, RawBytesOf(target.get(), before_ap));
  }
}

// A dictionary a save dropped, once nothing reached it, comes back whole,
// with the objects under its deep keys.
TEST_F(EPDFCaptureEmbedderTest, DroppedDictionaryComesBackWhole) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const uint32_t appearance = CreateSquare(layer.get(), 1, 20);
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  const std::string bytes = RawBytesOf(layer.get(), appearance);
  const std::vector<uint8_t> capture = ExportDict(layer.get(), 20, "AP");

  // Taken off the page without deleting the object: nothing reaches it.
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  {
    CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(layer.get());
    CPDF_DocumentViewScope view(pdf);
    RetainPtr<CPDF_Dictionary> page = pdf->GetMutablePageDictionary(1);
    page->GetMutableArrayFor("Annots")->RemoveAt(0);
  }
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  ScopedFPDFDocument reopened = Reopen(layer.get());
  ASSERT_TRUE(reopened);
  ASSERT_FALSE(DictOf(reopened.get(), 20));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(reopened.get()));
  ASSERT_TRUE(ImportDict(reopened.get(), capture));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(reopened.get()));
  EXPECT_EQ("Square", DictOf(reopened.get(), 20)->GetNameFor("Subtype"));
  EXPECT_EQ(appearance, NormalAppearanceOf(reopened.get(), 20));
  EXPECT_EQ(bytes, RawBytesOf(reopened.get(), appearance));
}

// A dictionary captured without deep keys holds no objects: what it refers
// to is kept as it is.
TEST_F(EPDFCaptureEmbedderTest, ShallowDictionaryHoldsNoObjects) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(CreateSquare(layer.get(), 1, 20));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportDict(layer.get(), 20, nullptr);
  const std::string text(capture.begin(), capture.end());
  EXPECT_EQ(std::string::npos, text.find(" obj"));
  EXPECT_FALSE(ExportDict(layer.get(), 4000, nullptr).size());
}

// A field whose delete prunes its parent comes back into the tree: the
// parent into /AcroForm /Fields, the field into the parent's /Kids, the
// widget onto its page, linked to the field.
TEST_F(EPDFCaptureEmbedderTest, FieldWithPrunedParentComesBack) {
  for (bool reopen : {false, true}) {
    SCOPED_TRACE(reopen ? "reopened between delete and import"
                        : "same session");
    ScopedFPDFDocument layer = OpenLayer();
    ASSERT_TRUE(layer);
    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    const std::vector<uint8_t> capture = ExportField(layer.get(), kChildField);
    ASSERT_FALSE(capture.empty());
    uint32_t detached[4] = {};
    unsigned long count = 0;
    ASSERT_TRUE(
        EPDFForm_DeleteField(layer.get(), kChildField, detached, 4, &count));
    ASSERT_EQ(1u, count);
    ASSERT_TRUE(EPDFPage_RemoveAnnotRaw(layer.get(), 0, 5));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    {
      CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(layer.get());
      CPDF_DocumentViewScope view(pdf);
      EXPECT_THAT(RefsIn(pdf->GetRoot()
                             ->GetDictFor("AcroForm")
                             ->GetArrayFor("Fields")
                             .Get()),
                  ElementsAre(kMergedField));
    }

    ScopedFPDFDocument target = reopen ? Reopen(layer.get()) : std::move(layer);
    ASSERT_TRUE(target);
    ASSERT_TRUE(EPDFLayer_BeginTransaction(target.get()));
    ASSERT_TRUE(ImportField(target.get(), capture));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(target.get()));
    CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(target.get());
    CPDF_DocumentViewScope view(pdf);
    EXPECT_THAT(RefsIn(pdf->GetRoot()
                           ->GetDictFor("AcroForm")
                           ->GetArrayFor("Fields")
                           .Get()),
                ElementsAre(kMergedField, kGroupField));
    EXPECT_THAT(
        RefsIn(DictOf(target.get(), kGroupField)->GetArrayFor("Kids").Get()),
        ElementsAre(kChildField));
    EXPECT_THAT(
        RefsIn(DictOf(target.get(), kChildField)->GetArrayFor("Kids").Get()),
        ElementsAre(kChildWidget));
    EXPECT_EQ(kChildField, RefOf(target.get(), kChildWidget, "Parent"));
    EXPECT_EQ(kChildWidget, Annots(target.get(), 0)[5]);
  }
}

// A merged field and widget is one object: it goes back into /Fields and
// onto its page.
TEST_F(EPDFCaptureEmbedderTest, MergedFieldComesBack) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportField(layer.get(), kMergedField);
  ASSERT_FALSE(capture.empty());
  uint32_t detached[4] = {};
  unsigned long count = 0;
  ASSERT_TRUE(
      EPDFForm_DeleteField(layer.get(), kMergedField, detached, 4, &count));
  ASSERT_TRUE(EPDFPage_RemoveAnnotRaw(layer.get(), 0, 4));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(ImportField(layer.get(), capture));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(layer.get());
  CPDF_DocumentViewScope view(pdf);
  EXPECT_THAT(
      RefsIn(
          pdf->GetRoot()->GetDictFor("AcroForm")->GetArrayFor("Fields").Get()),
      ElementsAre(kMergedField, kGroupField));
  EXPECT_THAT(Annots(layer.get(), 0),
              ElementsAre(kFileSquare, kFileNote, kFilePopup, kFileReply,
                          kMergedField, kChildWidget));
  EXPECT_FALSE(DictOf(layer.get(), kMergedField)->KeyExist("Parent"));
}

// A field that isn't terminal has no capture.
TEST_F(EPDFCaptureEmbedderTest, NonTerminalFieldHasNoCapture) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  EXPECT_TRUE(ExportField(layer.get(), kGroupField).empty());
}

// Captures are for layers: a plain document has none.
TEST_F(EPDFCaptureEmbedderTest, PlainDocumentHasNoCaptures) {
  ScopedFPDFDocument doc(
      FPDF_LoadMemDocument(input_.data(), input_.size(), nullptr));
  ASSERT_TRUE(doc);
  EXPECT_TRUE(ExportAnnots(doc.get(), 0, {0}).empty());
  EXPECT_TRUE(ExportDict(doc.get(), kFileSquare, "AP").empty());
  EXPECT_TRUE(ExportField(doc.get(), kMergedField).empty());
}
