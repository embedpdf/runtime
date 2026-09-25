// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Checkpoints (public/epdf_checkpoint.h) make a group of writes all or
// nothing. After writes that fail part way, a rollback brings the document
// back to its baseline: the same object graph, the same last object number,
// and saves that write the same apart from the trailer /ID, which every save
// writes afresh.

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_checkpoint.h"
#include "public/epdf_font.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_attachment.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

namespace {

// An 8 × 8 red PNG and the same in blue.
constexpr uint8_t kRedPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x4b, 0x6d, 0x29, 0xdc, 0x00, 0x00, 0x00,
    0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0x80,
    0x15, 0x61, 0x17, 0x1d, 0xb4, 0x12, 0x00, 0x28, 0xff, 0x3f, 0xc1, 0x6e,
    0xec, 0xdf, 0x61, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
    0x42, 0x60, 0x82};
constexpr uint8_t kBluePng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x4b, 0x6d, 0x29, 0xdc, 0x00, 0x00, 0x00,
    0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x60, 0xf8, 0x8f,
    0x03, 0x0d, 0x29, 0x09, 0x00, 0xa9, 0x70, 0x3f, 0xc1, 0x14, 0xca, 0xea,
    0x73, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
    0x82};

// A PDF of the given object bodies, numbered from 1, with a classic xref.
std::string MakePdf(const std::vector<std::string>& objects) {
  std::string pdf = "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const size_t xref = pdf.size();
  pdf += "xref\n0 " + std::to_string(objects.size() + 1) +
         "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    std::string number = std::to_string(offset);
    pdf += std::string(10 - number.size(), '0') + number + " 00000 n \n";
  }
  pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
         " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return pdf;
}

std::string Stream(const std::string& data) {
  return "<< /Length " + std::to_string(data.size()) + " >>\nstream\n" + data +
         "\nendstream";
}

// Two pages: the first has an indirect /Annots holding one square, the second
// has no /Annots. With `with_form`, the catalog has a form dictionary whose
// /DR /Font is its own object, holding Helvetica.
std::string MakeTwoPageDocument(bool with_form) {
  std::vector<std::string> objects = {
      with_form ? "<< /Type /Catalog /Pages 2 0 R /AcroForm 8 0 R >>"
                : "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 5 0 R "
      "/Resources << >> /Annots 6 0 R >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 5 0 R "
      "/Resources << >> >>",
      Stream("0 1 0 rg 10 10 30 30 re f"),
      "[7 0 R]",
      "<< /Type /Annot /Subtype /Square /Rect [240 240 290 290] /C [1 0 0] "
      "/P 3 0 R >>",
  };
  if (with_form) {
    objects.push_back(
        "<< /Fields [] /DR << /Font 9 0 R >> /DA (/Helv 0 Tf 0 g) >>");
    objects.push_back("<< /Helv 10 0 R >>");
    objects.push_back(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding /WinAnsiEncoding >>");
  }
  return MakePdf(objects);
}

// A one-page vector drawing: the source of a stamp.
std::string MakeDrawingDocument() {
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 50] /Contents 4 0 R "
      "/Resources << >> >>",
      Stream("1 0 0 rg 0 0 60 50 re f 0 0 1 rg 60 0 40 50 re f"),
  });
}

void Write(std::ostringstream& out, const CPDF_Object* object);

void WriteValue(std::ostringstream& out, const CPDF_Object* value) {
  if (!value->IsInline()) {
    out << " " << value->GetObjNum() << " R";
    return;
  }
  Write(out, value);
}

// An object as text, streams with their raw bytes, so two graphs compare by
// value.
void Write(std::ostringstream& out, const CPDF_Object* object) {
  switch (object->GetType()) {
    case CPDF_Object::kNullobj:
      out << " null";
      break;
    case CPDF_Object::kBoolean:
    case CPDF_Object::kNumber:
      out << " " << object->GetString();
      break;
    case CPDF_Object::kString:
      out << " " << object->AsString()->EncodeString();
      break;
    case CPDF_Object::kName:
      out << " /" << object->GetString();
      break;
    case CPDF_Object::kReference:
      out << " " << object->AsReference()->GetRefObjNum() << " R";
      break;
    case CPDF_Object::kArray: {
      const CPDF_Array* array = object->AsArray();
      out << " [";
      for (size_t i = 0; i < array->size(); ++i) {
        WriteValue(out, array->GetObjectAt(i).Get());
      }
      out << " ]";
      break;
    }
    case CPDF_Object::kDictionary: {
      CPDF_DictionaryLocker locker(object->AsDictionary());
      out << " <<";
      for (const auto& entry : locker) {
        out << " /" << entry.first;
        WriteValue(out, entry.second.Get());
      }
      out << " >>";
      break;
    }
    case CPDF_Object::kStream: {
      const CPDF_Stream* stream = object->AsStream();
      Write(out, stream->GetDict().Get());
      auto access = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
      access->LoadAllDataRaw();
      pdfium::span<const uint8_t> bytes = access->GetSpan();
      out << " stream " << std::string(bytes.begin(), bytes.end());
      break;
    }
  }
}

// Every indirect object, by number, from 1 to the last.
std::map<uint32_t, std::string> ObjectGraph(CPDF_Document* doc) {
  std::map<uint32_t, std::string> graph;
  for (uint32_t number = 1; number <= doc->GetLastObjNum(); ++number) {
    RetainPtr<CPDF_Object> object = doc->GetOrParseIndirectObject(number);
    if (!object) {
      continue;
    }
    std::ostringstream out;
    Write(out, object.Get());
    graph[number] = out.str();
  }
  return graph;
}

// A save with the digits of its /ID blanked: every save writes a new one.
std::string WithoutFileId(std::string bytes) {
  size_t at = 0;
  while ((at = bytes.find("/ID", at)) != std::string::npos) {
    size_t close = bytes.find(']', at);
    if (close == std::string::npos) {
      break;
    }
    bool in_hex = false;
    for (size_t i = at + 3; i < close; ++i) {
      if (bytes[i] == '<') {
        in_hex = true;
      } else if (bytes[i] == '>') {
        in_hex = false;
      } else if (in_hex) {
        bytes[i] = '0';
      }
    }
    at = close;
  }
  return bytes;
}

ScopedFPDFAnnotation NewAnnot(FPDF_PAGE page,
                              FPDF_ANNOTATION_SUBTYPE subtype,
                              const FS_RECTF& rect) {
  ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page, subtype));
  EXPECT_TRUE(annot);
  if (annot) {
    EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  }
  return annot;
}

// A stamp drawn from a PNG, as the stamp writer makes one.
void AddPngStamp(FPDF_DOCUMENT doc,
                 FPDF_PAGE page,
                 pdfium::span<const uint8_t> png,
                 const FS_RECTF& rect) {
  ScopedFPDFAnnotation stamp = NewAnnot(page, FPDF_ANNOT_STAMP, rect);
  FPDF_PAGEOBJECT image = FPDFPageObj_NewImageObj(doc);
  ASSERT_TRUE(image);
  ASSERT_TRUE(
      EPDFImageObj_SetPng(nullptr, 0, image, png.data(), png.size()));
  const FS_MATRIX matrix{rect.right - rect.left, 0, 0,
                         rect.top - rect.bottom, rect.left, rect.bottom};
  ASSERT_TRUE(FPDFPageObj_SetMatrix(image, &matrix));
  ASSERT_TRUE(FPDFAnnot_AppendObject(stamp.get(), image));
  ASSERT_TRUE(
      EPDFAnnot_UpdateAppearanceToRect(stamp.get(), EPDF_STAMP_FIT_STRETCH));
}

// What an import writes: a stamp from a PDF with an opacity layer, a PNG
// stamp, a file attachment, a free text with a registered font and a
// redaction with overlay text on the page with /Annots; a note, its popup and a reply, with attribution, on the
// page without.
void ImportEverything(FPDF_DOCUMENT doc,
                      EPDF_CHECKPOINT checkpoint,
                      FPDF_DOCUMENT drawing,
                      EPDF_FONT_ID font) {
  ASSERT_TRUE(EPDFDoc_CheckpointPage(checkpoint, 0));
  ASSERT_TRUE(EPDFDoc_CheckpointPage(checkpoint, 1));
  ScopedFPDFPage first(FPDF_LoadPage(doc, 0));
  ScopedFPDFPage second(FPDF_LoadPage(doc, 1));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  {
    ScopedFPDFAnnotation stamp =
        NewAnnot(first.get(), FPDF_ANNOT_STAMP, {20, 280, 120, 230});
    ASSERT_TRUE(EPDFAnnot_SetAppearanceFromPage(stamp.get(), drawing, 0));
    ASSERT_TRUE(
        EPDFAnnot_UpdateAppearanceToRect(stamp.get(), EPDF_STAMP_FIT_CONTAIN));
    ASSERT_TRUE(
        EPDFAnnot_SetStampOpacity(stamp.get(), EPDF_STAMP_FIT_CONTAIN, 128));
  }
  AddPngStamp(doc, first.get(), kRedPng, {20, 200, 120, 150});
  {
    ScopedFPDFAnnotation attachment =
        NewAnnot(first.get(), FPDF_ANNOT_FILEATTACHMENT, {150, 200, 170, 180});
    ScopedFPDFWideString name = GetFPDFWideString(L"notes.txt");
    FPDF_ATTACHMENT file =
        FPDFAnnot_AddFileAttachment(attachment.get(), name.get());
    ASSERT_TRUE(file);
    ASSERT_TRUE(FPDFAttachment_SetFile(file, doc, "hello", 5));
  }
  {
    ScopedFPDFAnnotation text =
        NewAnnot(first.get(), FPDF_ANNOT_FREETEXT, {20, 120, 200, 80});
    ScopedFPDFWideString contents = GetFPDFWideString(L"Checked");
    ASSERT_TRUE(
        FPDFAnnot_SetStringValue(text.get(), "Contents", contents.get()));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(text.get(), font,
                                                             14, 0, 0, 0));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(text.get()));
  }
  {
    ScopedFPDFAnnotation redact =
        NewAnnot(first.get(), FPDF_ANNOT_REDACT, {210, 120, 290, 80});
    ScopedFPDFWideString overlay = GetFPDFWideString(L"REDACTED");
    EXPECT_TRUE(EPDFAnnot_SetOverlayText(redact.get(), overlay.get()));
    EXPECT_TRUE(EPDFAnnot_GenerateAppearance(redact.get()));
  }

  ScopedFPDFAnnotation note =
      NewAnnot(second.get(), FPDF_ANNOT_TEXT, {20, 280, 40, 260});
  ScopedFPDFAnnotation popup =
      NewAnnot(second.get(), FPDF_ANNOT_POPUP, {60, 280, 200, 200});
  ScopedFPDFAnnotation reply =
      NewAnnot(second.get(), FPDF_ANNOT_TEXT, {20, 250, 40, 230});
  ASSERT_TRUE(EPDFAnnot_SetLinkedAnnot(popup.get(), "Parent", note.get()));
  ASSERT_TRUE(EPDFAnnot_SetLinkedAnnot(note.get(), "Popup", popup.get()));
  ASSERT_TRUE(EPDFAnnot_SetLinkedAnnot(reply.get(), "IRT", note.get()));
  ASSERT_TRUE(EPDFAnnot_SetReplyType(reply.get(), FPDF_ANNOT_RT_REPLY));
  ScopedFPDFWideString author = GetFPDFWideString(L"Alice");
  ScopedFPDFWideString date = GetFPDFWideString(L"D:20260901111200+02'00'");
  ScopedFPDFWideString user = GetFPDFWideString(L"u_alice");
  ASSERT_TRUE(FPDFAnnot_SetStringValue(note.get(), "T", author.get()));
  ASSERT_TRUE(FPDFAnnot_SetStringValue(note.get(), "M", date.get()));
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(note.get(), "CreationDate", date.get()));
  ASSERT_TRUE(
      EPDFAnnot_SetEmbedMetadataString(note.get(), "UserID", user.get()));

  // Render both pages, as a viewer would while the import runs: the page
  // caches then hold what the rollback removes.
  EXPECT_TRUE(EmbedderTest::RenderPageWithFlags(first.get(), nullptr,
                                                FPDF_ANNOT));
  EXPECT_TRUE(EmbedderTest::RenderPageWithFlags(second.get(), nullptr,
                                                FPDF_ANNOT));
}

// The colour at a point of a page rendered with its annotations, as 0xRRGGBB.
uint32_t ColorAt(FPDF_PAGE page, int x, int y_from_bottom) {
  ScopedFPDFBitmap bitmap =
      EmbedderTest::RenderPageWithFlags(page, nullptr, FPDF_ANNOT);
  const int height = FPDFBitmap_GetHeight(bitmap.get());
  const int stride = FPDFBitmap_GetStride(bitmap.get());
  const auto* pixels =
      static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get()));
  const uint8_t* pixel = pixels + (height - 1 - y_from_bottom) * stride + x * 4;
  return (pixel[2] << 16) | (pixel[1] << 8) | pixel[0];
}

}  // namespace

class EPDFCheckpointEmbedderTest : public EmbedderTest {
 protected:
  void SetUp() override {
    EmbedderTest::SetUp();
    drawing_bytes_ = MakeDrawingDocument();
    drawing_.reset(FPDF_LoadMemDocument(drawing_bytes_.data(),
                                        drawing_bytes_.size(), nullptr));
    ASSERT_TRUE(drawing_);
    const std::string path = PathService::GetTestFilePath("fonts/ahem/Ahem.ttf");
    ASSERT_FALSE(path.empty());
    font_bytes_ = GetFileContents(path.c_str());
    ASSERT_FALSE(font_bytes_.empty());
    font_ = EPDFFont_RegisterMemFont64("Ahem", 400, 0, font_bytes_.data(),
                                       font_bytes_.size());
    ASSERT_TRUE(font_);
  }

  void TearDown() override {
    EPDFFont_ClearRegisteredFonts();
    drawing_.reset();
    EmbedderTest::TearDown();
  }

  // The fixture as an ordinary document, or as a fresh layer over it.
  ScopedFPDFDocument Open(bool layer, bool with_form = false) {
    input_ = MakeTwoPageDocument(with_form);
    if (!layer) {
      return ScopedFPDFDocument(
          FPDF_LoadMemDocument(input_.data(), input_.size(), nullptr));
    }
    EPDF_BASE_DOCUMENT base =
        EPDF_LoadMemBaseDocument(input_.data(), input_.size(), nullptr);
    EXPECT_TRUE(base);
    ScopedFPDFDocument opened(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, nullptr));
    EPDF_ReleaseBaseDocument(base);
    return opened;
  }

  // What the document is, and what its saves write.
  struct State {
    std::map<uint32_t, std::string> graph;
    uint32_t last_number = 0;
    std::string full_save;
    std::string incremental_save;
    bool changed_since_load = false;
    unsigned long promoted = 0;
  };

  State Capture(FPDF_DOCUMENT doc, bool layer) {
    CPDF_Document* document = CPDFDocumentFromFPDFDocument(doc);
    State state;
    ClearString();
    EXPECT_TRUE(FPDF_SaveAsCopy(doc, this, FPDF_NO_INCREMENTAL));
    state.full_save = WithoutFileId(GetString());
    ClearString();
    if (layer) {
      FPDF_BOOL changed = false;
      EXPECT_TRUE(EPDFLayer_SaveDeltaEx(doc, this, nullptr, &changed));
      state.changed_since_load = changed;
      state.promoted = EPDFLayer_GetPromotedObjectCount(doc);
    } else {
      EXPECT_TRUE(FPDF_SaveAsCopy(doc, this, FPDF_INCREMENTAL));
    }
    state.incremental_save = WithoutFileId(GetString());
    state.graph = ObjectGraph(document);
    state.last_number = document->GetLastObjNum();
    return state;
  }

  // The same document: every object, the last object number, and saves.
  void ExpectSame(const State& before, const State& after) {
    std::ostringstream differing;
    for (const auto& [number, text] : after.graph) {
      auto it = before.graph.find(number);
      if (it == before.graph.end() || it->second != text) {
        differing << " " << number;
      }
    }
    for (const auto& [number, text] : before.graph) {
      if (!after.graph.count(number)) {
        differing << " " << number;
      }
    }
    EXPECT_EQ("", differing.str()) << "objects that differ";
    EXPECT_EQ(before.last_number, after.last_number);
    EXPECT_EQ(before.full_save, after.full_save);
    EXPECT_EQ(before.incremental_save, after.incremental_save);
    EXPECT_EQ(before.changed_since_load, after.changed_since_load);
  }

  std::string input_;
  std::string drawing_bytes_;
  ScopedFPDFDocument drawing_;
  std::vector<uint8_t> font_bytes_;
  EPDF_FONT_ID font_ = 0;
};

// After writes that add everything an import can, on an ordinary document
// and on a layer, with and without a form dictionary, a rollback brings back
// the document as it was when the checkpoint was taken.
TEST_F(EPDFCheckpointEmbedderTest, RollbackRestoresTheBaseline) {
  for (bool with_form : {false, true}) {
    for (bool layer : {false, true}) {
      SCOPED_TRACE(std::string(layer ? "layer" : "document") +
                   (with_form ? " with a form dictionary"
                              : " without a form dictionary"));
      ScopedFPDFDocument doc = Open(layer, with_form);
      ASSERT_TRUE(doc);
      CPDF_Document* document = CPDFDocumentFromFPDFDocument(doc.get());

      // An edit earlier in the session: the checkpoint brings back this
      // state, not the loaded one.
      {
        ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
        ScopedFPDFAnnotation square =
            NewAnnot(page.get(), FPDF_ANNOT_SQUARE, {150, 280, 200, 230});
        ASSERT_TRUE(EPDFAnnot_GenerateAppearance(square.get()));
      }
      const State before = Capture(doc.get(), layer);

      EPDF_CHECKPOINT checkpoint = EPDFDoc_BeginCheckpoint(doc.get());
      ASSERT_TRUE(checkpoint);
      ImportEverything(doc.get(), checkpoint, drawing_.get(), font_);
      EXPECT_GT(document->GetLastObjNum(), before.last_number);
      EXPECT_TRUE(EPDFDoc_Rollback(checkpoint));
      EPDFDoc_EndCheckpoint(checkpoint);

      ExpectSame(before, Capture(doc.get(), layer));
    }
  }
}

// Object numbers given back are used again. What the page caches kept under
// those numbers must not show through: a stamp made after the rollback, whose
// image takes the number the rolled-back stamp's image had, shows its own
// colour.
TEST_F(EPDFCheckpointEmbedderTest, NumbersUsedAgainShowTheNewContent) {
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    ScopedFPDFDocument doc = Open(layer);
    ASSERT_TRUE(doc);

    EPDF_CHECKPOINT checkpoint = EPDFDoc_BeginCheckpoint(doc.get());
    ASSERT_TRUE(EPDFDoc_CheckpointPage(checkpoint, 0));
    {
      ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
      AddPngStamp(doc.get(), page.get(), kRedPng, {20, 200, 120, 150});
      EXPECT_EQ(0xff0000u, ColorAt(page.get(), 70, 175));
    }
    EXPECT_TRUE(EPDFDoc_Rollback(checkpoint));
    EPDFDoc_EndCheckpoint(checkpoint);

    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
    AddPngStamp(doc.get(), page.get(), kBluePng, {20, 200, 120, 150});
    EXPECT_EQ(0x0000ffu, ColorAt(page.get(), 70, 175));
  }
}

// An annotation made without loading its page is on the page, and a rollback
// takes it back, on a page with /Annots and on one without.
TEST_F(EPDFCheckpointEmbedderTest, AnAnnotationMadeWithoutItsPageRollsBack) {
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    ScopedFPDFDocument doc = Open(layer);
    ASSERT_TRUE(doc);
    const State before = Capture(doc.get(), layer);
    const int counts[] = {EPDFPage_GetAnnotCountRaw(doc.get(), 0),
                          EPDFPage_GetAnnotCountRaw(doc.get(), 1)};

    EPDF_CHECKPOINT checkpoint = EPDFDoc_BeginCheckpoint(doc.get());
    for (int index : {0, 1}) {
      ASSERT_TRUE(EPDFDoc_CheckpointPage(checkpoint, index));
      ScopedFPDFAnnotation square(
          EPDFPage_CreateAnnotRaw(doc.get(), index, FPDF_ANNOT_SQUARE));
      ASSERT_TRUE(square);
      const FS_RECTF rect{20, 60, 60, 20};
      ASSERT_TRUE(FPDFAnnot_SetRect(square.get(), &rect));
      ASSERT_TRUE(EPDFAnnot_GenerateAppearance(square.get()));
      EXPECT_EQ(counts[index] + 1, EPDFPage_GetAnnotCountRaw(doc.get(), index));
      ScopedFPDFPage page(FPDF_LoadPage(doc.get(), index));
      ASSERT_EQ(counts[index] + 1, FPDFPage_GetAnnotCount(page.get()));
      ScopedFPDFAnnotation loaded(FPDFPage_GetAnnot(page.get(), counts[index]));
      EXPECT_EQ(FPDF_ANNOT_SQUARE, FPDFAnnot_GetSubtype(loaded.get()));
    }
    EXPECT_TRUE(EPDFDoc_Rollback(checkpoint));
    EPDFDoc_EndCheckpoint(checkpoint);
    ExpectSame(before, Capture(doc.get(), layer));
  }
}

TEST_F(EPDFCheckpointEmbedderTest, ARecordedAnnotationGetsItsValueBack) {
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer" : "document");
    ScopedFPDFDocument doc = Open(layer);
    ASSERT_TRUE(doc);
    // A note that is there before the checkpoint.
    ScopedFPDFAnnotation note(
        EPDFPage_CreateAnnotRaw(doc.get(), 0, FPDF_ANNOT_TEXT));
    ASSERT_TRUE(note);
    const unsigned int number =
        static_cast<unsigned int>(EPDFAnnot_GetObjectNumber(note.get()));
    const int index = EPDFPage_GetAnnotCountRaw(doc.get(), 0) - 1;
    note.reset();
    const State before = Capture(doc.get(), layer);

    // A popup linked to it: the note gains a /Popup.
    EPDF_CHECKPOINT checkpoint = EPDFDoc_BeginCheckpoint(doc.get());
    ASSERT_TRUE(EPDFDoc_CheckpointPage(checkpoint, 0));
    ASSERT_TRUE(EPDFDoc_CheckpointObject(checkpoint, number));
    ScopedFPDFAnnotation popup(
        EPDFPage_CreateAnnotRaw(doc.get(), 0, FPDF_ANNOT_POPUP));
    ASSERT_TRUE(popup);
    // A new object needs no record.
    EXPECT_TRUE(EPDFDoc_CheckpointObject(
        checkpoint,
        static_cast<unsigned int>(EPDFAnnot_GetObjectNumber(popup.get()))));
    ScopedFPDFAnnotation parent(EPDFPage_GetAnnotRaw(doc.get(), 0, index));
    ASSERT_TRUE(parent);
    ASSERT_TRUE(EPDFAnnot_SetLinkedAnnot(popup.get(), "Parent", parent.get()));
    ASSERT_TRUE(EPDFAnnot_SetLinkedAnnot(parent.get(), "Popup", popup.get()));
    ScopedFPDFWideString contents = GetFPDFWideString(L"changed");
    ASSERT_TRUE(FPDFAnnot_SetStringValue(parent.get(), "Contents", contents.get()));
    parent.reset();
    popup.reset();

    EXPECT_TRUE(EPDFDoc_Rollback(checkpoint));
    EPDFDoc_EndCheckpoint(checkpoint);
    ExpectSame(before, Capture(doc.get(), layer));
  }
}

TEST_F(EPDFCheckpointEmbedderTest, RefusesWhatIsNotThere) {
  EXPECT_FALSE(EPDFDoc_BeginCheckpoint(nullptr));
  EXPECT_FALSE(EPDFDoc_CheckpointPage(nullptr, 0));
  EXPECT_FALSE(EPDFDoc_Rollback(nullptr));
  EPDFDoc_EndCheckpoint(nullptr);
  ScopedFPDFDocument doc = Open(/*layer=*/false);
  EPDF_CHECKPOINT checkpoint = EPDFDoc_BeginCheckpoint(doc.get());
  EXPECT_FALSE(EPDFDoc_CheckpointPage(checkpoint, 99));
  EXPECT_FALSE(EPDFDoc_CheckpointPage(checkpoint, -1));
  EXPECT_FALSE(EPDFDoc_CheckpointObject(nullptr, 1));
  EXPECT_FALSE(EPDFDoc_CheckpointObject(checkpoint, 0));
  EPDFDoc_EndCheckpoint(checkpoint);
  EXPECT_FALSE(EPDFPage_CreateAnnotRaw(doc.get(), 99, FPDF_ANNOT_SQUARE));
  EXPECT_FALSE(EPDFPage_CreateAnnotRaw(nullptr, 0, FPDF_ANNOT_SQUARE));
}
