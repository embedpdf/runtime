// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Captures and their imports (public/epdf_capture.h). A delete captures the
// layer's versions of what it removes; an import puts them back at the same
// numbers, positions and links, also after the layer was saved and reopened
// in between, which drops everything the delete left behind. An import never
// replaces a version the layer holds, and never copies the uploaded file's
// originals.

#include <stdint.h>

#include <list>
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
#include "public/epdf_annot.h"
#include "public/epdf_capture.h"
#include "public/epdf_edit.h"
#include "public/epdf_form.h"
#include "public/epdf_layer.h"
#include "public/epdf_named_pages.h"
#include "public/epdf_save.h"
#include "public/epdf_view.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
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

// The objects of the document of pages below.
constexpr uint32_t kPageTree = 2;
constexpr uint32_t kCover = 3;
constexpr uint32_t kIntro = 4;
constexpr uint32_t kBody = 5;
constexpr uint32_t kCoverLink = 8;
constexpr uint32_t kIntroSquare = 9;
constexpr uint32_t kSignField = 10;
constexpr uint32_t kSignWidget = 11;

// Three pages, two of them named. The cover (3) links to the body; the
// intro (4) holds a blue square (9, its appearance 12), and nothing points at
// it but the page tree and its name; the body (5) holds the widget (11) of the
// field `sign` (10).
std::string MakePagesDocument() {
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R /Names << /Pages << /Names [(Cover) 3 0 "
      "R (Intro) 4 0 R] >> >> /AcroForm << /Fields [10 0 R] >> >>",
      "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 6 0 R "
      "/Resources << >> /Annots [8 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 7 0 R "
      "/Resources << >> /Annots [9 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 6 0 R "
      "/Resources << >> /Annots [11 0 R] >>",
      Stream("0 0 1 rg 0 0 10 10 re f"),
      Stream("1 0 0 rg 0 0 20 20 re f"),
      "<< /Type /Annot /Subtype /Link /Rect [10 10 50 50] /P 3 0 R "
      "/Dest [5 0 R /Fit] >>",
      "<< /Type /Annot /Subtype /Square /Rect [20 200 80 260] /C [0 0 1] "
      "/P 4 0 R /AP << /N 12 0 R >> >>",
      "<< /T (sign) /FT /Tx /Kids [11 0 R] >>",
      "<< /Type /Annot /Subtype /Widget /Parent 10 0 R /Rect [20 20 180 44] "
      "/P 5 0 R >>",
      "<< /Type /XObject /Subtype /Form /BBox [0 0 60 60] /Length 23 >>\n"
      "stream\n0 0 1 rg 0 0 60 60 re f\nendstream",
  });
}

// The pages of |doc| by object number, in page order.
std::vector<uint32_t> PageOrder(FPDF_DOCUMENT doc) {
  std::vector<uint32_t> pages;
  for (int i = 0; i < FPDF_GetPageCount(doc); ++i) {
    pages.push_back(EPDFDoc_GetPageObjectNumberByIndex(doc, i));
  }
  return pages;
}

// The /Names /Pages registrations of |doc|, as "key=page", in tree order.
std::vector<std::wstring> NamedPages(FPDF_DOCUMENT doc) {
  std::vector<std::wstring> entries;
  const int count = EPDFDoc_GetNamedPageCount(doc, EPDF_NAMED_PAGE_TREE_PAGES);
  for (int i = 0; i < count; ++i) {
    const unsigned long length = EPDFDoc_GetNamedPageAt(
        doc, EPDF_NAMED_PAGE_TREE_PAGES, i, nullptr, 0, nullptr, nullptr);
    std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
    unsigned int page = 0;
    EPDFDoc_GetNamedPageAt(doc, EPDF_NAMED_PAGE_TREE_PAGES, i, buffer.data(),
                           length, &page, nullptr);
    entries.push_back(GetPlatformWString(buffer.data()) + L"=" +
                      std::to_wstring(page));
  }
  return entries;
}

// Whether |capture| holds the object |objnum|.
bool HoldsObject(const std::vector<uint8_t>& capture, uint32_t objnum) {
  const std::string text(capture.begin(), capture.end());
  return text.find("\n" + std::to_string(objnum) + " 0 obj\n") !=
         std::string::npos;
}

std::vector<uint8_t> ExportPage(FPDF_DOCUMENT doc, uint32_t page_objnum) {
  unsigned long size = 0;
  void* buffer = EPDFPage_ExportRawToOwnedBuffer(doc, page_objnum, &size);
  return Take(buffer, size);
}

bool ImportPage(FPDF_DOCUMENT doc, const std::vector<uint8_t>& capture) {
  return EPDFPage_ImportRaw(doc, capture.data(),
                            static_cast<unsigned long>(capture.size()));
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

// A field's own /EMBD_Metadata is part of its capture: deleting the field and
// putting it back keeps it. `group.child` is a field whose widget is a
// separate object, so only the field's dictionary holds it.
TEST_F(EPDFCaptureEmbedderTest, FieldMetadataComesBackWithTheField) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(EPDFForm_SetFieldEmbedMetadataString(
      layer.get(), kChildField, "GroupID", GetFPDFWideString(L"buyer").get()));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportField(layer.get(), kChildField);
  ASSERT_FALSE(capture.empty());
  uint32_t detached[4] = {};
  unsigned long count = 0;
  ASSERT_TRUE(
      EPDFForm_DeleteField(layer.get(), kChildField, detached, 4, &count));
  ASSERT_TRUE(EPDFPage_RemoveAnnotRaw(layer.get(), 0, 5));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(ImportField(layer.get(), capture));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  RetainPtr<const CPDF_Dictionary> metadata =
      DictOf(layer.get(), kChildField)->GetDictFor("EMBD_Metadata");
  ASSERT_TRUE(metadata);
  EXPECT_EQ(L"buyer", metadata->GetUnicodeTextFor("GroupID"));
  EXPECT_FALSE(DictOf(layer.get(), kChildWidget)->KeyExist("EMBD_Metadata"));
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

// Pages (EPDFPage_ExportRawToOwnedBuffer and EPDFPage_ImportRaw), over the
// document of pages above.
class EPDFPageCaptureEmbedderTest : public EmbedderTest {
 protected:
  void SetUp() override {
    EmbedderTest::SetUp();
    input_ = MakePagesDocument();
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

  // The layer saved and opened again: what nothing reaches is gone. Each
  // artifact stays alive as long as the test, for the documents reading it.
  ScopedFPDFDocument Reopen(FPDF_DOCUMENT layer) {
    ClearString();
    EXPECT_TRUE(EPDFLayer_SaveLayerArtifact(layer, this, nullptr));
    std::string& artifact = artifacts_.emplace_back(GetString());
    FPDF_FILEACCESS access = {};
    access.m_FileLen = artifact.size();
    access.m_GetBlock = GetBlockFromString;
    access.m_Param = &artifact;
    EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument reopened(
        EPDFLayer_OpenLayerArtifact(base_, &access, nullptr, &status));
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    return reopened;
  }

  // The page tree's /Count.
  int CountOf(FPDF_DOCUMENT doc) {
    RetainPtr<const CPDF_Dictionary> pages = DictOf(doc, kPageTree);
    return pages ? pages->GetIntegerFor("Count") : -1;
  }

  std::string input_;
  std::list<std::string> artifacts_;
  EPDF_BASE_DOCUMENT base_ = nullptr;
};

// A page of the file comes back at its place, with its square and its name.
// The layer turned the page and recoloured the square, so the capture holds
// those edits: they come back also when a save and reopen dropped the
// layer's versions, which nothing reached once the page was gone.
TEST_F(EPDFPageCaptureEmbedderTest, DeletedFilePageComesBackWithItsEdits) {
  for (bool reopen : {false, true}) {
    SCOPED_TRACE(reopen ? "reopened between delete and import"
                        : "same session");
    ScopedFPDFDocument layer = OpenLayer();
    ASSERT_TRUE(layer);
    const std::string blue = ColorOf(layer.get(), kIntroSquare);
    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    ASSERT_TRUE(EPDFDoc_SetPageRotationByObjectNumber(layer.get(), kIntro, 1));
    Recolor(layer.get(), kIntroSquare, 1, 0, 0);
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    const std::string red = ColorOf(layer.get(), kIntroSquare);
    ASSERT_NE(blue, red);

    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    const std::vector<uint8_t> capture = ExportPage(layer.get(), kIntro);
    ASSERT_FALSE(capture.empty());
    ASSERT_TRUE(EPDFDoc_DeletePageByObjectNumber(layer.get(), kIntro));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    EXPECT_THAT(PageOrder(layer.get()), ElementsAre(kCover, kBody));
    EXPECT_THAT(NamedPages(layer.get()), ElementsAre(L"Cover=3"));

    ScopedFPDFDocument target = reopen ? Reopen(layer.get()) : std::move(layer);
    ASSERT_TRUE(target);
    if (reopen) {
      EXPECT_EQ(0, DictOf(target.get(), kIntro)->GetIntegerFor("Rotate"));
      EXPECT_EQ(blue, ColorOf(target.get(), kIntroSquare));
    }
    ASSERT_TRUE(EPDFLayer_BeginTransaction(target.get()));
    ASSERT_TRUE(ImportPage(target.get(), capture));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(target.get()));
    EXPECT_THAT(PageOrder(target.get()), ElementsAre(kCover, kIntro, kBody));
    EXPECT_EQ(3, CountOf(target.get()));
    EXPECT_EQ(kPageTree, RefOf(target.get(), kIntro, "Parent"));
    EXPECT_EQ(90, DictOf(target.get(), kIntro)->GetIntegerFor("Rotate"));
    EXPECT_THAT(Annots(target.get(), 1), ElementsAre(kIntroSquare));
    EXPECT_EQ(red, ColorOf(target.get(), kIntroSquare));
    EXPECT_THAT(NamedPages(target.get()), ElementsAre(L"Cover=3", L"Intro=4"));
    ScopedFPDFPage page(FPDF_LoadPage(target.get(), 1));
    ASSERT_TRUE(page);
    EXPECT_EQ(1, FPDFPage_GetRotation(page.get()));
  }
}

// A page the layer made comes back at its number, over the null its delete
// left, with its square, the square's appearance and its name, also when a
// save and reopen dropped all of them; and it saves as itself afterwards.
TEST_F(EPDFPageCaptureEmbedderTest, DeletedLayerPageComesBack) {
  for (bool reopen : {false, true}) {
    SCOPED_TRACE(reopen ? "reopened between delete and import"
                        : "same session");
    ScopedFPDFDocument layer = OpenLayer();
    ASSERT_TRUE(layer);
    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    ASSERT_TRUE(EPDFPage_InsertBlankRaw(layer.get(), 3, 300, 300, 20));
    const uint32_t appearance = CreateSquare(layer.get(), 3, 21);
    ASSERT_TRUE(appearance);
    ASSERT_TRUE(EPDFDoc_SetNamedPage(layer.get(),
                                     GetFPDFWideString(L"Appendix").get(), 20));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    const std::string appearance_bytes = RawBytesOf(layer.get(), appearance);

    ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
    const std::vector<uint8_t> capture = ExportPage(layer.get(), 20);
    ASSERT_FALSE(capture.empty());
    EXPECT_TRUE(HoldsObject(capture, 20));
    EXPECT_TRUE(HoldsObject(capture, 21));
    EXPECT_TRUE(HoldsObject(capture, appearance));
    ASSERT_TRUE(EPDFDoc_DeletePageByObjectNumber(layer.get(), 20));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
    EXPECT_THAT(PageOrder(layer.get()), ElementsAre(kCover, kIntro, kBody));
    EXPECT_FALSE(DictOf(layer.get(), 20));  // the null the delete left

    ScopedFPDFDocument target = reopen ? Reopen(layer.get()) : std::move(layer);
    ASSERT_TRUE(target);
    if (reopen) {
      EXPECT_FALSE(DictOf(target.get(), 21));
      EXPECT_TRUE(RawBytesOf(target.get(), appearance).empty());
    }
    ASSERT_TRUE(EPDFLayer_BeginTransaction(target.get()));
    ASSERT_TRUE(ImportPage(target.get(), capture));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(target.get()));
    EXPECT_THAT(PageOrder(target.get()),
                ElementsAre(kCover, kIntro, kBody, 20u));
    EXPECT_EQ(4, CountOf(target.get()));
    EXPECT_THAT(Annots(target.get(), 3), ElementsAre(21u));
    EXPECT_EQ(appearance, NormalAppearanceOf(target.get(), 21));
    EXPECT_EQ(appearance_bytes, RawBytesOf(target.get(), appearance));
    EXPECT_THAT(NamedPages(target.get()),
                ElementsAre(L"Appendix=20", L"Cover=3", L"Intro=4"));

    ScopedFPDFDocument saved = Reopen(target.get());
    ASSERT_TRUE(saved);
    EXPECT_THAT(PageOrder(saved.get()),
                ElementsAre(kCover, kIntro, kBody, 20u));
    EXPECT_THAT(Annots(saved.get(), 3), ElementsAre(21u));
    EXPECT_EQ(appearance, NormalAppearanceOf(saved.get(), 21));
  }
}

// A delete leaves the form alone, so the page brings its widget back into
// its /Annots, still the kid of its field; the cover's link still names it.
TEST_F(EPDFPageCaptureEmbedderTest, PageComesBackWithItsFieldsWidget) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportPage(layer.get(), kBody);
  ASSERT_FALSE(capture.empty());
  ASSERT_TRUE(EPDFDoc_DeletePageByObjectNumber(layer.get(), kBody));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  RetainPtr<const CPDF_Dictionary> field = DictOf(layer.get(), kSignField);
  ASSERT_TRUE(field);
  EXPECT_THAT(RefsIn(field->GetArrayFor("Kids").Get()),
              ElementsAre(kSignWidget));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(ImportPage(layer.get(), capture));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_THAT(PageOrder(layer.get()), ElementsAre(kCover, kIntro, kBody));
  EXPECT_THAT(Annots(layer.get(), 2), ElementsAre(kSignWidget));
  EXPECT_EQ(kSignField, RefOf(layer.get(), kSignWidget, "Parent"));
  EXPECT_EQ(kBody, RefOf(layer.get(), kSignWidget, "P"));
  RetainPtr<const CPDF_Dictionary> link = DictOf(layer.get(), kCoverLink);
  ASSERT_TRUE(link);
  EXPECT_THAT(RefsIn(link->GetArrayFor("Dest").Get()), ElementsAre(kBody, 0u));
}

// Its place: after the page it followed while that is still in the page
// tree, wherever that page went since, else first; never twice.
TEST_F(EPDFPageCaptureEmbedderTest, ImportFindsItsPlace) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> cover = ExportPage(layer.get(), kCover);
  const std::vector<uint8_t> intro = ExportPage(layer.get(), kIntro);
  const std::vector<uint8_t> body = ExportPage(layer.get(), kBody);
  ASSERT_FALSE(cover.empty());
  ASSERT_FALSE(intro.empty());
  ASSERT_FALSE(body.empty());
  EXPECT_FALSE(ImportPage(layer.get(), intro));  // it's there

  ASSERT_TRUE(EPDFDoc_DeletePageByObjectNumber(layer.get(), kCover));
  ASSERT_TRUE(EPDFDoc_DeletePageByObjectNumber(layer.get(), kIntro));
  ASSERT_TRUE(ImportPage(layer.get(), intro));  // the cover is gone: first
  EXPECT_FALSE(ImportPage(layer.get(), intro));
  EXPECT_THAT(PageOrder(layer.get()), ElementsAre(kIntro, kBody));
  ASSERT_TRUE(ImportPage(layer.get(), cover));  // it was first
  EXPECT_THAT(PageOrder(layer.get()), ElementsAre(kCover, kIntro, kBody));

  // The body follows the intro, wherever the intro is now.
  const int intro_index = 1;
  ASSERT_TRUE(FPDF_MovePages(layer.get(), &intro_index, 1, 0));
  ASSERT_TRUE(EPDFDoc_DeletePageByObjectNumber(layer.get(), kBody));
  ASSERT_TRUE(ImportPage(layer.get(), body));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_THAT(PageOrder(layer.get()), ElementsAre(kIntro, kBody, kCover));
  EXPECT_EQ(3, CountOf(layer.get()));
  EXPECT_THAT(NamedPages(layer.get()), ElementsAre(L"Cover=3", L"Intro=4"));
}

// A key another page took in the meantime stays with that page.
TEST_F(EPDFPageCaptureEmbedderTest, ImportLeavesAKeyAnotherPageTook) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportPage(layer.get(), kIntro);
  ASSERT_FALSE(capture.empty());
  ASSERT_TRUE(EPDFDoc_DeletePageByObjectNumber(layer.get(), kIntro));
  ASSERT_TRUE(EPDFDoc_SetNamedPage(layer.get(),
                                   GetFPDFWideString(L"Intro").get(), kBody));
  ASSERT_TRUE(ImportPage(layer.get(), capture));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_THAT(PageOrder(layer.get()), ElementsAre(kCover, kIntro, kBody));
  EXPECT_THAT(NamedPages(layer.get()), ElementsAre(L"Cover=3", L"Intro=5"));
}

// A capture holds its page, never another: the cover links to the body,
// which the layer turned, and the page tree is its parent.
TEST_F(EPDFPageCaptureEmbedderTest, CaptureHoldsNoOtherPage) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(EPDFDoc_SetPageRotationByObjectNumber(layer.get(), kCover, 1));
  ASSERT_TRUE(EPDFDoc_SetPageRotationByObjectNumber(layer.get(), kBody, 2));
  ASSERT_TRUE(EPDFDoc_DeletePageByObjectNumber(layer.get(), kIntro));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportPage(layer.get(), kCover);
  ASSERT_FALSE(capture.empty());
  EXPECT_TRUE(HoldsObject(capture, kCover));
  EXPECT_FALSE(HoldsObject(capture, kBody));
  EXPECT_FALSE(HoldsObject(capture, kPageTree));
  EXPECT_FALSE(HoldsObject(capture, 1));  // the catalog
}

// An import that the transaction then drops leaves the page out.
TEST_F(EPDFPageCaptureEmbedderTest, AbortedImportLeavesThePageOut) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  const std::vector<uint8_t> capture = ExportPage(layer.get(), kIntro);
  ASSERT_TRUE(EPDFDoc_DeletePageByObjectNumber(layer.get(), kIntro));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));

  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(ImportPage(layer.get(), capture));
  ASSERT_TRUE(EPDFLayer_AbortTransaction(layer.get()));
  EXPECT_THAT(PageOrder(layer.get()), ElementsAre(kCover, kBody));
  EXPECT_EQ(2, CountOf(layer.get()));
  EXPECT_THAT(NamedPages(layer.get()), ElementsAre(L"Cover=3"));
}

TEST_F(EPDFPageCaptureEmbedderTest, CapturesNeedAPageAndALayer) {
  ScopedFPDFDocument layer = OpenLayer();
  ASSERT_TRUE(layer);
  EXPECT_TRUE(ExportPage(layer.get(), 0).empty());
  EXPECT_TRUE(ExportPage(layer.get(), 1).empty());  // the catalog
  EXPECT_TRUE(ExportPage(layer.get(), kPageTree).empty());
  EXPECT_TRUE(ExportPage(layer.get(), kIntroSquare).empty());
  EXPECT_TRUE(ExportPage(layer.get(), 99).empty());
  // Turned, so the capture holds the page: cutting its end leaves an object
  // without `endobj`.
  ASSERT_TRUE(EPDFDoc_SetPageRotationByObjectNumber(layer.get(), kIntro, 1));
  const std::vector<uint8_t> capture = ExportPage(layer.get(), kIntro);
  ASSERT_TRUE(HoldsObject(capture, kIntro));
  const std::vector<uint8_t> annots = ExportAnnots(layer.get(), 1, {0});
  ASSERT_FALSE(annots.empty());

  ASSERT_TRUE(EPDFLayer_BeginTransaction(layer.get()));
  ASSERT_TRUE(EPDFDoc_DeletePageByObjectNumber(layer.get(), kIntro));
  EXPECT_TRUE(ExportPage(layer.get(), kIntro).empty());  // out of the tree
  EXPECT_FALSE(ImportPage(layer.get(), annots));  // another kind
  const std::vector<uint8_t> cut(capture.begin(), capture.end() - 4);
  EXPECT_FALSE(ImportPage(layer.get(), cut));
  EXPECT_FALSE(ImportPage(layer.get(), {}));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(layer.get()));
  EXPECT_FALSE(ImportPage(layer.get(), capture));  // outside a transaction
  EXPECT_THAT(PageOrder(layer.get()), ElementsAre(kCover, kBody));

  ScopedFPDFDocument plain(
      FPDF_LoadMemDocument(input_.data(), input_.size(), nullptr));
  ASSERT_TRUE(plain);
  EXPECT_TRUE(ExportPage(plain.get(), kIntro).empty());
}
