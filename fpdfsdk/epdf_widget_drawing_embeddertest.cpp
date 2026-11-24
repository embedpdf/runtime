// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Drawings in widgets (public/epdf_annot.h). A drawing is a picture the
// document holds once. A widget shows one in two ways: as a push button's
// icon, which /MK names and the generator draws in every layout that shows
// an icon, alone (/MK /TP 1, what Acrobat writes for an image field) or with
// the button's caption (/MK /TP 2 to 6), and as a mark, which a wrapper
// places as it places a stamp's artwork (a signature field's). Stamps, marks
// and icons share one drawing.

#include <stdio.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_appearancegenerator.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/retain_ptr.h"
#include "fpdfsdk/cpdfsdk_formfillenvironment.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/cpdfsdk_pageview.h"
#include "fpdfsdk/cpdfsdk_widget.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_annot.h"
#include "public/epdf_form.h"
#include "public/epdf_layer.h"
#include "public/epdf_view.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_formfill.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/embedpdf_form_writes.h"
#include "testing/embedpdf_layer_fixture.h"
#include "testing/embedpdf_layer_mode.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

constexpr uint32_t kRed = 0xFF0000;
constexpr uint32_t kBlue = 0x0000FF;
constexpr uint32_t kGreen = 0x00FF00;
constexpr uint32_t kYellow = 0xFFFF00;
constexpr uint32_t kWhite = 0xFFFFFF;

// The picture the tests draw is 200 by 100, red on its left half and blue on
// its right, so its direction shows. A second one is green and yellow.
constexpr CFX_FloatRect kPictureBox(0, 0, 200, 100);

// What only the content of the picture's page holds, when the page is drawn
// with paths, and only the second picture's: how a test finds either in a
// saved file.
constexpr char kPictureMark[] = "1 0 0 rg";
constexpr char kSecondPictureMark[] = "0 1 0 rg";

// How a picture's page is made: drawn with paths, or holding one image at its
// own size, as the engine makes a page from a PNG or a JPEG.
enum class Source { kVector, kImage };
constexpr Source kSources[] = {Source::kVector, Source::kImage};

const char* NameOf(Source source) {
  return source == Source::kVector ? "a vector page" : "an image page";
}

// A one-page document whose page is 200 by 100, its left half `left` and its
// right half `right`.
embedpdf_test::ScopedTestDocument NewPicturePage(Source source,
                                                 uint32_t left = kRed,
                                                 uint32_t right = kBlue) {
  embedpdf_test::ScopedTestDocument document(NewTestDocument());
  EXPECT_TRUE(document);
  ScopedFPDFPage page(FPDFPage_New(document.get(), 0, 200, 100));
  EXPECT_TRUE(page);
  if (source == Source::kImage) {
    ScopedFPDFBitmap bitmap(FPDFBitmap_Create(200, 100, 0));
    EXPECT_TRUE(
        FPDFBitmap_FillRect(bitmap.get(), 0, 0, 100, 100, 0xFF000000 | left));
    EXPECT_TRUE(FPDFBitmap_FillRect(bitmap.get(), 100, 0, 100, 100,
                                    0xFF000000 | right));
    FPDF_PAGEOBJECT image = FPDFPageObj_NewImageObj(document.get());
    EXPECT_TRUE(FPDFImageObj_SetBitmap(nullptr, 0, image, bitmap.get()));
    const FS_MATRIX at_its_size{200, 0, 0, 100, 0, 0};
    EXPECT_TRUE(FPDFPageObj_SetMatrix(image, &at_its_size));
    FPDFPage_InsertObject(page.get(), image);
  } else {
    const struct {
      float left;
      uint32_t colour;
    } kHalves[] = {{0, left}, {100, right}};
    for (const auto& half : kHalves) {
      FPDF_PAGEOBJECT box = FPDFPageObj_CreateNewRect(half.left, 0, 100, 100);
      EXPECT_TRUE(FPDFPageObj_SetFillColor(box, (half.colour >> 16) & 0xFF,
                                           (half.colour >> 8) & 0xFF,
                                           half.colour & 0xFF, 255));
      EXPECT_TRUE(FPDFPath_SetDrawMode(box, FPDF_FILLMODE_ALTERNATE, false));
      FPDFPage_InsertObject(page.get(), box);
    }
  }
  EXPECT_TRUE(FPDFPage_GenerateContent(page.get()));
  return document;
}

// The picture as a drawing of `document`, by object number: the canonical
// drawing of its page, added, as the engine adds one. Each call adds a
// drawing; a test that shares one keeps the number.
unsigned int AddDrawing(FPDF_DOCUMENT document,
                        Source source = Source::kVector,
                        uint32_t left = kRed,
                        uint32_t right = kBlue) {
  embedpdf_test::ScopedTestDocument page = NewPicturePage(source, left, right);
  ScopedFPDFDocument canonical(EPDFDoc_CanonicalDrawing(page.get(), 0));
  EXPECT_TRUE(canonical);
  return canonical ? EPDFDoc_ImportDrawing(document, canonical.get()) : 0;
}

// A page rendered once with its form fields' own appearances, for the colour
// at any point of it as 0xRRGGBB. (ColorAt() draws no widget.) Points are
// the page's as it is shown, from its lower left corner. At a `scale` above
// 1 the page is drawn that many times larger, so text has pixels of its own
// colour.
class Shown {
 public:
  explicit Shown(FPDF_PAGE page, int scale = 1) : scale_(scale) {
    if (scale == 1) {
      bitmap_ = EmbedderTest::RenderPageWithFlags(
          page, nullptr, FPDF_ANNOT | EPDF_RENDER_WIDGETS);
      return;
    }
    const int width = static_cast<int>(FPDF_GetPageWidthF(page)) * scale;
    const int height = static_cast<int>(FPDF_GetPageHeightF(page)) * scale;
    bitmap_.reset(FPDFBitmap_Create(width, height, 0));
    EXPECT_TRUE(
        FPDFBitmap_FillRect(bitmap_.get(), 0, 0, width, height, 0xFFFFFFFF));
    FPDF_RenderPageBitmap(bitmap_.get(), page, 0, 0, width, height, 0,
                          FPDF_ANNOT | EPDF_RENDER_WIDGETS);
  }

  uint32_t At(float x, float y_from_bottom) const {
    return Pixel(static_cast<int>(x * scale_),
                 static_cast<int>(y_from_bottom * scale_));
  }

  // True when text is drawn inside `box`: one of its pixels is black, which
  // nothing but a caption is in these tests.
  bool HasInkIn(const CFX_FloatRect& box) const {
    const int right = static_cast<int>(box.right * scale_);
    const int top = static_cast<int>(box.top * scale_);
    for (int y = static_cast<int>(box.bottom * scale_) + 1; y < top; ++y) {
      for (int x = static_cast<int>(box.left * scale_) + 1; x < right; ++x) {
        const uint32_t colour = Pixel(x, y);
        if (((colour >> 16) & 0xFF) < 0x40 && ((colour >> 8) & 0xFF) < 0x40 &&
            (colour & 0xFF) < 0x40) {
          return true;
        }
      }
    }
    return false;
  }

 private:
  // The pixel `x` from the left and `y` up from the bottom row.
  uint32_t Pixel(int x, int y) const {
    const int height = FPDFBitmap_GetHeight(bitmap_.get());
    const int stride = FPDFBitmap_GetStride(bitmap_.get());
    const auto* pixels =
        static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap_.get()));
    const uint8_t* pixel = pixels + (height - 1 - y) * stride + x * 4;
    return (pixel[2] << 16) | (pixel[1] << 8) | pixel[0];
  }

  ScopedFPDFBitmap bitmap_;
  const int scale_;
};

uint32_t ShownColorAt(FPDF_PAGE page, int x, int y_from_bottom) {
  return Shown(page).At(x, y_from_bottom);
}

const CPDF_Dictionary* DictOf(FPDF_ANNOTATION annot) {
  return CPDFAnnotContextFromFPDFAnnotation(annot)->GetAnnotDict();
}

RetainPtr<CPDF_Dictionary> MutableDictOf(FPDF_ANNOTATION annot) {
  return CPDFAnnotContextFromFPDFAnnotation(annot)->GetMutableAnnotDict();
}

// The annotation's normal appearance.
RetainPtr<const CPDF_Stream> AppearanceOf(FPDF_ANNOTATION annot) {
  return GetAnnotAP(DictOf(annot), CPDF_Annot::AppearanceMode::kNormal);
}

// The bytes of a stream, filters applied.
std::string StreamBytes(const CPDF_Stream* stream) {
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
  acc->LoadAllDataFiltered();
  const auto span = acc->GetSpan();
  return std::string(span.begin(), span.end());
}

// The object number of the form the appearance names /FRM, or 0: the icon a
// generated button draws.
uint32_t DrawnIconNumber(FPDF_ANNOTATION annot) {
  RetainPtr<const CPDF_Stream> appearance = AppearanceOf(annot);
  RetainPtr<const CPDF_Dictionary> resources =
      appearance ? appearance->GetDict()->GetDictFor("Resources") : nullptr;
  RetainPtr<const CPDF_Dictionary> forms =
      resources ? resources->GetDictFor("XObject") : nullptr;
  RetainPtr<const CPDF_Stream> icon =
      forms ? forms->GetStreamFor("FRM") : nullptr;
  return icon ? icon->GetObjNum() : 0;
}

// How an appearance draws its icon: the plate it is clipped to, and where
// the picture's box lands, both in the appearance's own space.
struct DrawnIcon {
  CFX_FloatRect plate;
  CFX_FloatRect picture;
};

std::optional<DrawnIcon> DrawnIconOf(FPDF_ANNOTATION annot) {
  RetainPtr<const CPDF_Stream> appearance = AppearanceOf(annot);
  if (!appearance) {
    return std::nullopt;
  }
  const std::string content = StreamBytes(appearance.Get());
  const size_t drawn = content.find("/FRM Do");
  const size_t start =
      drawn == std::string::npos ? drawn : content.rfind("q\n", drawn);
  if (start == std::string::npos) {
    return std::nullopt;
  }
  float plate[4];
  float matrix[6];
  if (sscanf(content.c_str() + start,
             "q\n%f %f %f %f re W n\n%f %f %f %f %f %f cm\n/FRM Do\nQ\n",
             &plate[0], &plate[1], &plate[2], &plate[3], &matrix[0], &matrix[1],
             &matrix[2], &matrix[3], &matrix[4], &matrix[5]) != 10) {
    return std::nullopt;
  }
  DrawnIcon icon;
  icon.plate = CFX_FloatRect(plate[0], plate[1], plate[0] + plate[2],
                             plate[1] + plate[3]);
  icon.picture = CFX_Matrix(matrix[0], matrix[1], matrix[2], matrix[3],
                            matrix[4], matrix[5])
                     .TransformRect(kPictureBox);
  return icon;
}

// The part an appearance clips its caption to, in the appearance's own
// space, or nothing when it draws no caption.
std::optional<CFX_FloatRect> DrawnCaptionOf(FPDF_ANNOTATION annot) {
  RetainPtr<const CPDF_Stream> appearance = AppearanceOf(annot);
  if (!appearance) {
    return std::nullopt;
  }
  const std::string content = StreamBytes(appearance.Get());
  const size_t text = content.find("BT\n");
  const size_t start =
      text == std::string::npos ? text : content.rfind("q\n", text);
  float part[4];
  if (start == std::string::npos ||
      sscanf(content.c_str() + start, "q\n%f %f %f %f re W n\nBT\n", &part[0],
             &part[1], &part[2], &part[3]) != 4) {
    return std::nullopt;
  }
  return CFX_FloatRect(part[0], part[1], part[0] + part[2], part[1] + part[3]);
}

void ExpectRectNear(const CFX_FloatRect& expected,
                    const CFX_FloatRect& actual,
                    float within = 1e-3f) {
  EXPECT_NEAR(expected.left, actual.left, within);
  EXPECT_NEAR(expected.bottom, actual.bottom, within);
  EXPECT_NEAR(expected.right, actual.right, within);
  EXPECT_NEAR(expected.top, actual.top, within);
}

// How many streams of the file `bytes` hold pass `counts`: every object the
// file lists, reached from its root or not.
template <typename Counts>
int StreamsOf(const std::string& bytes, Counts counts) {
  embedpdf_test::ScopedTestDocument reopened(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  EXPECT_TRUE(reopened);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(reopened.get());
  int count = 0;
  for (uint32_t number = 1; doc && number <= doc->GetLastObjNum(); ++number) {
    RetainPtr<const CPDF_Stream> stream =
        ToStream(doc->GetOrParseIndirectObject(number));
    if (stream && counts(stream.Get())) {
      ++count;
    }
  }
  return count;
}

// How many streams of a saved file have `mark` in their data.
int StreamsHolding(const std::string& bytes, const char* mark) {
  return StreamsOf(bytes, [mark](const CPDF_Stream* stream) {
    return StreamBytes(stream).find(mark) != std::string::npos;
  });
}

// How many times a saved file holds the picture: the streams with its mark
// for a vector page, the image XObjects for an image page.
int PicturesIn(const std::string& bytes, Source source) {
  if (source == Source::kVector) {
    return StreamsHolding(bytes, kPictureMark);
  }
  return StreamsOf(bytes, [](const CPDF_Stream* stream) {
    return stream->GetDict()->GetNameFor("Subtype") == "Image";
  });
}

// The drawings `document` lists.
std::vector<unsigned int> DrawingsOf(FPDF_DOCUMENT document) {
  std::vector<unsigned int> drawings(EPDFDoc_GetDrawings(document, nullptr, 0));
  if (!drawings.empty()) {
    EXPECT_EQ(drawings.size(),
              EPDFDoc_GetDrawings(document, drawings.data(), drawings.size()));
  }
  return drawings;
}

// Writes `bytes` as `<name>.pdf` into the folder EPDF_BUTTON_ICON_SAMPLE_DIR
// names, when it names one: files to open in another viewer.
void KeepSample(const std::string& bytes, const char* name) {
  const char* directory = std::getenv("EPDF_BUTTON_ICON_SAMPLE_DIR");
  if (!directory || !*directory) {
    return;
  }
  std::ofstream pdf(std::string(directory) + "/" + name + ".pdf",
                    std::ios::binary);
  pdf.write(bytes.data(), bytes.size());
  ASSERT_TRUE(pdf.good());
}

// A file as another app writes a button with a picture, built by hand from
// ISO 32000-2 12.5.6.19 and 12.7.8.3.3 (it is not Acrobat's): one push
// button field with seven widgets, one for each layout /MK /TP names, in
// that order. Every widget has the caption "Go", the same icon (object 6)
// with an icon fit dictionary that spells out the defaults, and an
// appearance that draws the icon inside a border of 1. Each stands 102 by
// 42, 50 apart, from y 10 up. With `with_fonts` false, nothing names a /DA
// or a font. With `with_appearances` false, no widget has an /AP. The
// caption's font size is `font_size`: 0 for one fitted to its room.
std::string ForeignButtonsPdf(bool with_fonts = true,
                              bool with_appearances = true,
                              int font_size = 12) {
  std::vector<std::string> objects = {
      std::string("<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields "
                  "[5 0 R]") +
          (with_fonts
               ? " /DA (/Helv 0 Tf 0 g) /DR << /Font << /Helv 4 0 R >> >>"
               : "") +
          " >> >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] /Resources << >> "
      "/Annots [7 0 R 8 0 R 9 0 R 10 0 R 11 0 R 12 0 R 13 0 R] >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
      "/Encoding /WinAnsiEncoding >>",
      "<< /FT /Btn /Ff 65536 /T (picture) "
      "/Kids [7 0 R 8 0 R 9 0 R 10 0 R 11 0 R 12 0 R 13 0 R] >>",
      Stream("/Type /XObject /Subtype /Form /BBox [0 0 200 100] "
             "/Resources << >>",
             "1 0 0 rg 0 0 100 100 re f 0 0 1 rg 100 0 100 100 re f"),
  };
  for (int layout = 0; layout <= 6; ++layout) {
    const int bottom = 10 + 50 * layout;
    objects.push_back(
        "<< /Type /Annot /Subtype /Widget /Parent 5 0 R /P 3 0 R /F 4 "
        "/Rect [20 " +
        std::to_string(bottom) + " 122 " + std::to_string(bottom + 42) +
        "] /MK << /I 6 0 R /TP " + std::to_string(layout) +
        " /CA (Go) /IF << /SW /A /S /P /A [0.5 0.5] >> >>" +
        (with_fonts ? " /DA (/Helv " + std::to_string(font_size) + " Tf 0 g)"
                    : "") +
        (with_appearances
             ? " /AP << /N " + std::to_string(14 + layout) + " 0 R >>"
             : "") +
        " >>");
  }
  for (int layout = 0; layout <= 6; ++layout) {
    objects.push_back(
        Stream("/Type /XObject /Subtype /Form /BBox [0 0 102 42] "
               "/Resources << /XObject << /FRM 6 0 R >> >>",
               "q 1 1 100 40 re W n q .4 0 0 .4 11 1 cm /FRM Do Q Q"));
  }
  return MakePdf(objects);
}

constexpr uint32_t kForeignIcon = 6;

// A canonical drawing whose placement is folded into its form's /Matrix,
// built by hand: the form draws the picture at half size, off the origin,
// in the box [10 20 110 70], and its /Matrix [2 0 0 2 -20 -40] shows that box
// as [0 0 200 100].
std::string CanonicalPageWithAMatrix() {
  return MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Resources "
      "<< /XObject << /EPDFDRAWING 5 0 R >> >> /Contents 4 0 R >>",
      Stream("/EPDFDRAWING Do"),
      Stream("/Type /XObject /Subtype /Form /BBox [10 20 110 70] "
             "/Matrix [2 0 0 2 -20 -40]",
             "1 0 0 rg 10 20 50 50 re f 0 0 1 rg 60 20 50 50 re f"),
  });
}

// A file with one annotation of every subtype ISO 32000-2 Table 171 names,
// a stamp and a widget last. Every one of them has the same appearance: a
// wrapper of ours (object 5) around a drawing (object 4), as only a stamp
// and a widget are given one.
constexpr const char* kOtherSubtypes[] = {
    "Text",           "Link",      "FreeText", "Line",      "Square",
    "Circle",         "Polygon",   "PolyLine", "Highlight", "Underline",
    "Squiggly",       "StrikeOut", "Caret",    "Ink",       "Popup",
    "FileAttachment", "Sound",     "Movie",    "Screen",    "PrinterMark",
    "TrapNet",        "Watermark", "3D",       "Redact",    "Projection",
    "RichMedia"};
constexpr uint32_t kWrappedDrawing = 4;

std::string EverySubtypePdf() {
  const int count = static_cast<int>(std::size(kOtherSubtypes)) + 2;
  std::string annots;
  for (int i = 0; i < count; ++i) {
    annots += std::to_string(6 + i) + " 0 R ";
  }
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] /Annots [" +
          annots + "] >>",
      Stream("/Type /XObject /Subtype /Form /BBox [0 0 200 100]",
             "1 0 0 rg 0 0 100 100 re f 0 0 1 rg 100 0 100 100 re f"),
      Stream("/Type /XObject /Subtype /Form /BBox [0 0 100 50] "
             "/Resources << /XObject << /EPDFWRAP 4 0 R >> >>",
             "q .5 0 0 .5 0 0 cm /EPDFWRAP Do Q"),
  };
  auto annotation = [](const std::string& subtype) {
    return "<< /Type /Annot /Subtype /" + subtype +
           " /Rect [20 20 120 70] /AP << /N 5 0 R >> >>";
  };
  for (const char* subtype : kOtherSubtypes) {
    objects.push_back(annotation(subtype));
  }
  objects.push_back(annotation("Stamp"));
  objects.push_back(annotation("Widget"));
  return MakePdf(objects);
}

// A stream's data that counts how often it is fetched: reading a stream's
// content (to decode or to parse it) fetches its data, and reading its
// dictionary doesn't.
class CountedData final : public IFX_SeekableReadStream {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  FX_FILESIZE GetSize() override {
    return static_cast<FX_FILESIZE>(data_.size());
  }
  bool ReadBlockAtOffset(pdfium::span<uint8_t> buffer,
                         FX_FILESIZE offset) override {
    ++fetches_;
    if (offset < 0 || static_cast<size_t>(offset) > data_.size() ||
        buffer.size() > data_.size() - static_cast<size_t>(offset)) {
      return false;
    }
    std::copy_n(data_.begin() + offset, buffer.size(), buffer.begin());
    return true;
  }
  bool IsSelfContained() const override { return true; }

  int fetches() const { return fetches_; }

 private:
  explicit CountedData(std::string data) : data_(std::move(data)) {}
  ~CountedData() override = default;

  const std::string data_;
  int fetches_ = 0;
};

// The layouts that show a caption and an icon (/MK /TP, ISO 32000-2 Table
// 192).
constexpr int kCaptionBelow = 2;
constexpr int kCaptionAbove = 3;
constexpr int kCaptionRight = 4;
constexpr int kCaptionLeft = 5;
constexpr int kCaptionOver = 6;
const struct {
  int layout;
  const char* name;
} kCaptionAndIconLayouts[] = {{kCaptionBelow, "the caption below the icon"},
                              {kCaptionAbove, "the caption above the icon"},
                              {kCaptionRight, "the caption right of the icon"},
                              {kCaptionLeft, "the caption left of the icon"},
                              {kCaptionOver, "the caption over the icon"}};

// The room the caption "Go" takes at `size` in the font the button's
// appearance writes it in: its width, and the height of its line, which is
// the font's ascent and descent, as PDFium's form filler measures a line.
// The tests' stand-in for Helvetica has its own.
CFX_SizeF CaptionRoomOf(FPDF_ANNOTATION annot, float size = 12) {
  RetainPtr<const CPDF_Stream> appearance = AppearanceOf(annot);
  RetainPtr<const CPDF_Dictionary> resources =
      appearance ? appearance->GetDict()->GetDictFor("Resources") : nullptr;
  RetainPtr<const CPDF_Dictionary> fonts =
      resources ? resources->GetDictFor("Font") : nullptr;
  if (!fonts || fonts->size() == 0) {
    ADD_FAILURE() << "the appearance names no font";
    return CFX_SizeF();
  }
  RetainPtr<const CPDF_Dictionary> dict;
  {
    CPDF_DictionaryLocker locker(fonts);
    dict = ToDictionary(locker.begin()->second->GetDirect());
  }
  CPDF_Document* doc =
      CPDFAnnotContextFromFPDFAnnotation(annot)->GetPage()->GetDocument();
  RetainPtr<CPDF_Font> font = CPDF_DocPageData::FromDocument(doc)->GetFont(
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(dict.Get())));
  if (!font) {
    ADD_FAILURE() << "the appearance's font doesn't load";
    return CFX_SizeF();
  }
  return CFX_SizeF(
      font->GetStringWidth("Go") * size / 1000,
      (font->GetTypeAscent() - font->GetTypeDescent()) * size / 1000);
}

// The size an appearance writes its caption at: the number before Tf.
float DrawnCaptionSizeOf(FPDF_ANNOTATION annot) {
  const std::string content = StreamBytes(AppearanceOf(annot).Get());
  const size_t set = content.find(" Tf");
  const size_t start =
      set == std::string::npos ? set : content.rfind(' ', set - 1);
  return start == std::string::npos ? 0
                                    : strtof(content.c_str() + start, nullptr);
}

// A point of the picture's red half and one of its blue half, when it lands
// in `box`: in the middle of each half, near the bottom, where no caption
// over the picture reaches.
CFX_PointF RedPointOf(const CFX_FloatRect& box) {
  return CFX_PointF(box.left + box.Width() / 4, box.bottom + box.Height() / 8);
}
CFX_PointF BluePointOf(const CFX_FloatRect& box) {
  return CFX_PointF(box.right - box.Width() / 4, box.bottom + box.Height() / 8);
}

// Where "Go" at 12 is when it is in the middle of `part`: 16 by 9 around the
// middle, with room to spare.
CFX_FloatRect CaptionInkOf(const CFX_FloatRect& part) {
  const float x = (part.left + part.right) / 2;
  const float y = (part.bottom + part.top) / 2;
  return CFX_FloatRect(x - 9, y - 6, x + 9, y + 6);
}

// Checks that `caption` and `icon` share `plate` as `layout` says: the
// caption in a strip as high as its line, at the bottom or the top, or as
// wide as its text, at the right or the left, and the icon in the rest; or
// both in all of it. `room` is what the caption takes.
void ExpectPartsShare(int layout,
                      const CFX_FloatRect& plate,
                      const CFX_SizeF& room,
                      const CFX_FloatRect& caption,
                      const CFX_FloatRect& icon) {
  constexpr float kWithin = 1e-2f;
  if (layout == kCaptionOver) {
    ExpectRectNear(plate, caption, kWithin);
    ExpectRectNear(plate, icon, kWithin);
    return;
  }
  CFX_FloatRect both = caption;
  both.Union(icon);
  ExpectRectNear(plate, both, kWithin);
  switch (layout) {
    case kCaptionBelow:
      EXPECT_NEAR(plate.bottom, caption.bottom, kWithin);
      EXPECT_NEAR(room.height, caption.Height(), kWithin);
      EXPECT_NEAR(caption.top, icon.bottom, kWithin);
      break;
    case kCaptionAbove:
      EXPECT_NEAR(plate.top, caption.top, kWithin);
      EXPECT_NEAR(room.height, caption.Height(), kWithin);
      EXPECT_NEAR(caption.bottom, icon.top, kWithin);
      break;
    case kCaptionRight:
      EXPECT_NEAR(plate.right, caption.right, kWithin);
      EXPECT_NEAR(room.width, caption.Width(), kWithin);
      EXPECT_NEAR(caption.left, icon.right, kWithin);
      break;
    case kCaptionLeft:
      EXPECT_NEAR(plate.left, caption.left, kWithin);
      EXPECT_NEAR(room.width, caption.Width(), kWithin);
      EXPECT_NEAR(caption.right, icon.left, kWithin);
      break;
    default:
      ADD_FAILURE() << "not a layout that shows both";
  }
}

// What a button's appearance draws, read from its content whoever wrote it:
// the part its icon is clipped to and where the picture's box lands, and the
// size its caption is written at and the point its text starts. Nothing for
// what it doesn't draw.
struct ButtonContent {
  std::optional<CFX_FloatRect> icon_part;
  std::optional<CFX_FloatRect> picture;
  std::optional<float> caption_size;
  std::optional<CFX_PointF> caption_start;
};

ButtonContent ReadButtonContent(const std::string& content) {
  struct State {
    CFX_Matrix placed;
    std::optional<CFX_FloatRect> clip;
  };
  ButtonContent read;
  std::vector<State> saved;
  State state;
  std::vector<float> numbers;
  std::istringstream tokens(content);
  std::string token;
  auto last = [&numbers](size_t count) {
    return numbers.size() < count ? nullptr
                                  : numbers.data() + numbers.size() - count;
  };
  while (tokens >> token) {
    char* end = nullptr;
    const float number = strtof(token.c_str(), &end);
    if (end != token.c_str() && *end == '\0') {
      numbers.push_back(number);
      continue;
    }
    if (token == "q") {
      saved.push_back(state);
    } else if (token == "Q" && !saved.empty()) {
      state = saved.back();
      saved.pop_back();
    } else if (token == "re") {
      if (const float* box = last(4)) {
        state.clip =
            CFX_FloatRect(box[0], box[1], box[0] + box[2], box[1] + box[3]);
      }
    } else if (token == "cm") {
      if (const float* m = last(6)) {
        CFX_Matrix placed(m[0], m[1], m[2], m[3], m[4], m[5]);
        placed.Concat(state.placed);
        state.placed = placed;
      }
    } else if (token == "Do") {
      read.icon_part = state.clip;
      read.picture = state.placed.TransformRect(kPictureBox);
    } else if (token == "Tf" && !read.caption_size) {
      if (const float* size = last(1)) {
        read.caption_size = *size;
      }
    } else if (token == "Td" && !read.caption_start) {
      if (const float* start = last(2)) {
        read.caption_start = CFX_PointF(start[0], start[1]);
      }
    }
    numbers.clear();
  }
  return read;
}

}  // namespace

class EPDFWidgetDrawingEmbedderTest : public EmbedderTest {
 protected:
  void SetUp() override {
    EmbedderTest::SetUp();
    doc_ = NewTestDocument();
    ASSERT_TRUE(doc_);
    page_.reset(FPDFPage_New(doc_.get(), 0, 400, 400));
    ASSERT_TRUE(page_);
  }

  void TearDown() override {
    page_.reset();
    doc_.reset();
    EmbedderTest::TearDown();
  }

  CPDF_Document* pdf() { return CPDFDocumentFromFPDFDocument(doc_.get()); }

  // A new field of `family` in `document`, without a widget, under a name no
  // other field of the test has.
  uint32_t NewField(FPDF_DOCUMENT document, int family) {
    return EPDFForm_CreateField(
        document, family,
        GetFPDFWideString(L"field" + std::to_wstring(++fields_)).get(), 0);
  }

  // A new widget of `field` on `page`, a page of `document`, standing at
  // `rect`, with a border of `border_width` when one is given: /BS /W, and no
  // colour to paint it. A check box's names its checked state `on_state`.
  ScopedFPDFAnnotation NewWidget(FPDF_DOCUMENT document,
                                 uint32_t field,
                                 FPDF_PAGE page,
                                 const FS_RECTF& rect,
                                 std::optional<float> border_width = {},
                                 const char* on_state = nullptr) {
    uint32_t number = 0;
    {
      ScopedFPDFAnnotation widget(
          EPDFPage_CreateAnnot(page, FPDF_ANNOT_WIDGET));
      EXPECT_TRUE(widget);
      EXPECT_TRUE(EPDFAnnot_SetRect(widget.get(), &rect));
      if (border_width) {
        EXPECT_TRUE(EPDFAnnot_SetBorderStyle(widget.get(), FPDF_ANNOT_BS_SOLID,
                                             *border_width));
      }
      number = EPDFAnnot_GetObjectNumber(widget.get());
    }
    EXPECT_TRUE(EPDFForm_AttachWidget(document, field, number, on_state, 0));
    return ScopedFPDFAnnotation(EPDFPage_GetAnnotByObjectNumber(page, number));
  }

  // A new push button with one widget, on a page of the test's document.
  ScopedFPDFAnnotation NewButton(const FS_RECTF& rect,
                                 std::optional<float> border_width = {},
                                 FPDF_PAGE page = nullptr) {
    const uint32_t field =
        NewField(doc_.get(), EPDF_FORMFIELD_FAMILY_PUSHBUTTON);
    EXPECT_GT(field, 0u);
    return NewWidget(doc_.get(), field, page ? page : page_.get(), rect,
                     border_width);
  }

  // A new signature field with one widget, on a page of the test's document:
  // where a mark goes.
  ScopedFPDFAnnotation NewSignatureWidget(const FS_RECTF& rect,
                                          FPDF_PAGE page = nullptr) {
    const uint32_t field =
        NewField(doc_.get(), EPDF_FORMFIELD_FAMILY_SIGNATURE);
    EXPECT_GT(field, 0u);
    return NewWidget(doc_.get(), field, page ? page : page_.get(), rect);
  }

  // What the engine does to show a drawing as one box's image: the drawing
  // as the normal icon, the layout that shows the icon alone, and a new
  // appearance.
  void SetIcon(FPDF_ANNOTATION widget, unsigned int drawing) {
    ASSERT_TRUE(EPDFAnnot_SetMKIcon(widget, EPDF_MK_ICON_I, drawing));
    ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(widget, 1));
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(widget));
  }

  // What shows a drawing beside a caption in one box: the caption "Go" in
  // Helvetica at `font_size` (0 for a size fitted to the box), the drawing as
  // the normal icon, the layout, and a new appearance.
  void SetCaptionAndIcon(FPDF_ANNOTATION widget,
                         unsigned int drawing,
                         int layout,
                         float font_size = 12) {
    ASSERT_TRUE(EPDFAnnot_SetMKText(widget, EPDF_MK_TEXT_CA,
                                    GetFPDFWideString(L"Go").get()));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(widget, FPDF_FONT_HELVETICA,
                                               font_size, 0, 0, 0));
    ASSERT_TRUE(EPDFAnnot_SetMKIcon(widget, EPDF_MK_ICON_I, drawing));
    ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(widget, layout));
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(widget));
  }

  // A save of `doc`: a full one, or with FPDF_INCREMENTAL one appended to
  // the bytes the document was opened from.
  std::string SavedBytes(FPDF_DOCUMENT doc,
                         FPDF_DWORD flags = FPDF_NO_INCREMENTAL) {
    ClearString();
    EXPECT_TRUE(FPDF_SaveAsCopy(doc, this, flags));
    return GetString();
  }

  // The canonical bytes of the picture: what the engine hashes to know a
  // drawing by its content.
  std::string CanonicalBytes(Source source) {
    embedpdf_test::ScopedTestDocument page = NewPicturePage(source);
    ScopedFPDFDocument canonical(EPDFDoc_CanonicalDrawing(page.get(), 0));
    EXPECT_TRUE(canonical);
    return canonical ? SavedBytes(canonical.get()) : std::string();
  }

  embedpdf_test::ScopedTestDocument doc_;
  ScopedFPDFPage page_;
  int fields_ = 0;
};

namespace {

// The two kinds of save a test looks into.
const struct {
  const char* name;
  FPDF_DWORD flags;
} kSaves[] = {{"a full save", FPDF_NO_INCREMENTAL},
              {"an incremental save", FPDF_INCREMENTAL}};

}  // namespace

// Naming a drawing as a button's icon writes /MK /I and nothing else: no
// object is made, and the layout and the appearance are the caller's next
// two calls. Drawn, the appearance names the same form /FRM, so the file
// holds the picture once. Clearing takes /MK /I away and leaves the layout,
// and the next appearance is the box alone.
TEST_F(EPDFWidgetDrawingEmbedderTest, SetsReadsAndClearsAnIcon) {
  // 202 by 102, with the border of 1 a widget has when nothing says
  // otherwise: the picture fits the inside at its own size.
  ScopedFPDFAnnotation button = NewButton({100, 302, 302, 200});
  ASSERT_TRUE(button);
  const unsigned int drawing = AddDrawing(doc_.get());
  const unsigned int second =
      AddDrawing(doc_.get(), Source::kVector, kGreen, kYellow);
  ASSERT_NE(0u, drawing);
  ASSERT_NE(0u, second);
  EXPECT_FALSE(EPDFAnnot_HasMKIcon(button.get(), EPDF_MK_ICON_I));
  EXPECT_EQ(0u, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
  const uint32_t first_appearance = AppearanceOf(button.get())->GetObjNum();
  const uint32_t objects = pdf()->GetLastObjNum();
  const std::string drawing_before = ObjectGraph(pdf())[drawing];

  ASSERT_TRUE(EPDFAnnot_SetMKIcon(button.get(), EPDF_MK_ICON_I, drawing));
  EXPECT_TRUE(EPDFAnnot_HasMKIcon(button.get(), EPDF_MK_ICON_I));
  EXPECT_EQ(drawing, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
  {
    // /MK names the drawing by reference, and nothing else is written: the
    // layout is the caption alone still, no object was made, and the
    // appearance is the one the button had.
    RetainPtr<const CPDF_Dictionary> mk =
        DictOf(button.get())->GetDictFor("MK");
    EXPECT_TRUE(mk->GetObjectFor("I")->IsReference());
    EXPECT_FALSE(mk->KeyExist("TP"));
    EXPECT_FALSE(mk->KeyExist("IF"));
  }
  EXPECT_EQ(objects, pdf()->GetLastObjNum());
  EXPECT_EQ(0, EPDFAnnot_GetMKTextPosition(button.get()));
  EXPECT_EQ(first_appearance, AppearanceOf(button.get())->GetObjNum());
  EXPECT_EQ(kWhite, ShownColorAt(page_.get(), 150, 250));

  ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(button.get(), 1));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
  RetainPtr<const CPDF_Stream> appearance = AppearanceOf(button.get());
  ASSERT_TRUE(appearance);
  EXPECT_NE(first_appearance, appearance->GetObjNum());
  EXPECT_EQ("Form", appearance->GetDict()->GetNameFor("Subtype"));
  EXPECT_EQ(CFX_FloatRect(0, 0, 202, 102),
            appearance->GetDict()->GetRectFor("BBox"));
  EXPECT_EQ("q\nQ\nq\n1 1 200 100 re W n\n1 0 0 1 1 1 cm\n/FRM Do\nQ\n",
            StreamBytes(appearance.Get()));
  EXPECT_EQ(drawing, DrawnIconNumber(button.get()));
  // A generated button's appearance is no wrapper: its picture is its icon.
  EXPECT_EQ(0u, EPDFAnnot_GetDrawing(button.get()));
  EXPECT_EQ(kRed, ShownColorAt(page_.get(), 150, 250));
  EXPECT_EQ(kBlue, ShownColorAt(page_.get(), 250, 250));
  EXPECT_EQ(kWhite, ShownColorAt(page_.get(), 50, 250));

  // Naming the drawing the entry names already writes nothing.
  const std::string widget_before =
      ObjectGraph(pdf())[EPDFAnnot_GetObjectNumber(button.get())];
  EXPECT_TRUE(EPDFAnnot_SetMKIcon(button.get(), EPDF_MK_ICON_I, drawing));
  EXPECT_EQ(widget_before,
            ObjectGraph(pdf())[EPDFAnnot_GetObjectNumber(button.get())]);

  // Another drawing replaces the first, which is as it was.
  ASSERT_TRUE(EPDFAnnot_SetMKIcon(button.get(), EPDF_MK_ICON_I, second));
  EXPECT_EQ(second, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
  EXPECT_EQ(drawing_before, ObjectGraph(pdf())[drawing]);

  ASSERT_TRUE(EPDFAnnot_ClearMKIcon(button.get(), EPDF_MK_ICON_I));
  EXPECT_FALSE(EPDFAnnot_HasMKIcon(button.get(), EPDF_MK_ICON_I));
  EXPECT_EQ(0u, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
  EXPECT_FALSE(DictOf(button.get())->GetDictFor("MK")->KeyExist("I"));
  // The layout stays: the button still shows an icon alone, and has none.
  EXPECT_EQ(1, EPDFAnnot_GetMKTextPosition(button.get()));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
  appearance = AppearanceOf(button.get());
  EXPECT_EQ("q\nQ\n", StreamBytes(appearance.Get()));
  EXPECT_FALSE(appearance->GetDict()->KeyExist("Resources"));
  EXPECT_EQ(0u, DrawnIconNumber(button.get()));
  EXPECT_EQ(kWhite, ShownColorAt(page_.get(), 150, 250));
  // Clearing what isn't there succeeds and writes nothing.
  EXPECT_TRUE(EPDFAnnot_ClearMKIcon(button.get(), EPDF_MK_ICON_I));
}

// The rollover and down icons are kept as the normal one is, each under its
// own key. Only the normal icon is drawn: the generator makes the normal
// appearance.
TEST_F(EPDFWidgetDrawingEmbedderTest, TheOtherIconsAreKeptAndNotDrawn) {
  ScopedFPDFAnnotation button = NewButton({100, 302, 302, 200});
  ASSERT_TRUE(button);
  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(button.get(), 1));
  const struct {
    int which;
    const char* key;
  } kOthers[] = {{EPDF_MK_ICON_RI, "RI"}, {EPDF_MK_ICON_IX, "IX"}};
  for (const auto& other : kOthers) {
    SCOPED_TRACE(other.key);
    EXPECT_FALSE(EPDFAnnot_HasMKIcon(button.get(), other.which));
    ASSERT_TRUE(EPDFAnnot_SetMKIcon(button.get(), other.which, drawing));
    EXPECT_TRUE(EPDFAnnot_HasMKIcon(button.get(), other.which));
    EXPECT_EQ(drawing, EPDFAnnot_GetMKIcon(button.get(), other.which));
    EXPECT_TRUE(DictOf(button.get())->GetDictFor("MK")->KeyExist(other.key));
    EXPECT_FALSE(EPDFAnnot_HasMKIcon(button.get(), EPDF_MK_ICON_I));
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
    EXPECT_EQ(0u, DrawnIconNumber(button.get()));
  }
  // The document lists the drawing once, though two entries name it.
  EXPECT_EQ(std::vector<unsigned int>({drawing}), DrawingsOf(doc_.get()));
  // Clearing one leaves the other.
  ASSERT_TRUE(EPDFAnnot_ClearMKIcon(button.get(), EPDF_MK_ICON_RI));
  EXPECT_FALSE(EPDFAnnot_HasMKIcon(button.get(), EPDF_MK_ICON_RI));
  EXPECT_TRUE(EPDFAnnot_HasMKIcon(button.get(), EPDF_MK_ICON_IX));
}

// A call that can't show a drawing writes nothing: the widget has no /MK,
// the document no new object, and on a layer nothing is copied up.
TEST_F(EPDFWidgetDrawingEmbedderTest, RefusesWithoutWriting) {
  ScopedFPDFAnnotation button = NewButton({100, 302, 302, 200});
  ASSERT_TRUE(button);
  ScopedFPDFAnnotation flat = NewSignatureWidget({100, 100, 300, 100});
  ASSERT_TRUE(flat);
  ScopedFPDFAnnotation square(
      FPDFPage_CreateAnnot(page_.get(), FPDF_ANNOT_SQUARE));
  ASSERT_TRUE(square);
  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_NE(0u, drawing);
  const unsigned int page_number =
      EPDFDoc_GetPageObjectNumberByIndex(doc_.get(), 0);
  const uint32_t objects = pdf()->GetLastObjNum();
  const uint32_t flat_appearance = AppearanceOf(flat.get())->GetObjNum();

  for (int which : {-1, 3}) {
    EXPECT_FALSE(EPDFAnnot_SetMKIcon(button.get(), which, drawing));
    EXPECT_FALSE(EPDFAnnot_ClearMKIcon(button.get(), which));
    EXPECT_FALSE(EPDFAnnot_HasMKIcon(button.get(), which));
    EXPECT_EQ(0u, EPDFAnnot_GetMKIcon(button.get(), which));
  }
  // Only a widget has icons.
  EXPECT_FALSE(EPDFAnnot_SetMKIcon(square.get(), EPDF_MK_ICON_I, drawing));
  EXPECT_FALSE(EPDFAnnot_ClearMKIcon(square.get(), EPDF_MK_ICON_I));
  EXPECT_FALSE(EPDFAnnot_HasMKIcon(square.get(), EPDF_MK_ICON_I));
  EXPECT_EQ(0u, EPDFAnnot_GetMKIcon(square.get(), EPDF_MK_ICON_I));
  EXPECT_FALSE(EPDFAnnot_SetMKIcon(nullptr, EPDF_MK_ICON_I, drawing));
  EXPECT_FALSE(EPDFAnnot_ClearMKIcon(nullptr, EPDF_MK_ICON_I));
  EXPECT_FALSE(EPDFAnnot_HasMKIcon(nullptr, EPDF_MK_ICON_I));
  EXPECT_EQ(0u, EPDFAnnot_GetMKIcon(nullptr, EPDF_MK_ICON_I));
  // Only a form of the document is a drawing: not no object, not a page,
  // not a number the document doesn't have.
  for (unsigned int no_drawing : {0u, page_number, 999999u}) {
    SCOPED_TRACE(no_drawing);
    EXPECT_FALSE(EPDFAnnot_SetMKIcon(button.get(), EPDF_MK_ICON_I, no_drawing));
    EXPECT_FALSE(
        EPDFAnnot_SetDrawing(button.get(), no_drawing, EPDF_STAMP_FIT_CONTAIN));
  }
  // Only a stamp and a widget place a drawing, and only one with area.
  EXPECT_FALSE(
      EPDFAnnot_SetDrawing(square.get(), drawing, EPDF_STAMP_FIT_CONTAIN));
  EXPECT_FALSE(EPDFAnnot_SetDrawing(nullptr, drawing, EPDF_STAMP_FIT_CONTAIN));
  EXPECT_FALSE(
      EPDFAnnot_SetDrawing(flat.get(), drawing, EPDF_STAMP_FIT_CONTAIN));
  EXPECT_EQ(0u, EPDFAnnot_GetDrawing(square.get()));
  EXPECT_EQ(0u, EPDFAnnot_GetDrawing(nullptr));
  EXPECT_EQ(0u, EPDFAnnot_GetDrawing(flat.get()));

  EXPECT_FALSE(DictOf(button.get())->KeyExist("MK"));
  EXPECT_FALSE(DictOf(square.get())->KeyExist("MK"));
  EXPECT_EQ(flat_appearance, AppearanceOf(flat.get())->GetObjNum());
  EXPECT_EQ(objects, pdf()->GetLastObjNum());

  // On a layer, a refused call and a call that changes nothing copy nothing
  // up: object 3 is the page, and the second widget names object 6 already.
  embedpdf_test::LayerFixture foreign;
  const std::string bytes = ForeignButtonsPdf();
  ASSERT_TRUE(foreign.OpenBytes({bytes.begin(), bytes.end()}));
  {
    ScopedFPDFPage page(FPDF_LoadPage(foreign.layer, 0));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation widget(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(widget);
    EXPECT_FALSE(EPDFAnnot_SetMKIcon(widget.get(), EPDF_MK_ICON_I, 3));
    EXPECT_FALSE(EPDFAnnot_SetDrawing(widget.get(), 3, EPDF_STAMP_FIT_CONTAIN));
    EXPECT_TRUE(
        EPDFAnnot_SetMKIcon(widget.get(), EPDF_MK_ICON_I, kForeignIcon));
    EXPECT_TRUE(EPDFAnnot_ClearMKIcon(widget.get(), EPDF_MK_ICON_RI));
    EXPECT_EQ(kForeignIcon, EPDFAnnot_GetMKIcon(widget.get(), EPDF_MK_ICON_I));
    EXPECT_EQ(0u, EPDFAnnot_GetDrawing(widget.get()));
  }
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(foreign.layer));
}

// The icon is fitted into the button less the border's width at each edge,
// the width /BS gives whether or not a colour paints it, and clipped there.
// /MK /IF /FB takes the whole button.
TEST_F(EPDFWidgetDrawingEmbedderTest, AnIconIsFittedInsideTheBorder) {
  const FS_RECTF kStands{100, 308, 308, 200};  // 208 by 108
  const struct {
    const char* name;
    std::optional<float> border_width;
    bool painted;
    bool fit_to_bounds;
    CFX_FloatRect plate;
    CFX_FloatRect picture;
  } kCases[] = {
      // No /BS: a border of 1. 206 by 106 inside, so the picture is 206 by
      // 103 in its middle.
      {"no /BS",
       {},
       false,
       false,
       CFX_FloatRect(1, 1, 207, 107),
       CFX_FloatRect(1, 2.5f, 207, 105.5f)},
      {"no border", 0.0f, false, false, CFX_FloatRect(0, 0, 208, 108),
       CFX_FloatRect(0, 2, 208, 106)},
      {"a border of 4", 4.0f, true, false, CFX_FloatRect(4, 4, 204, 104),
       CFX_FloatRect(4, 4, 204, 104)},
      {"a border of 4, fitted to the bounds", 4.0f, true, true,
       CFX_FloatRect(0, 0, 208, 108), CFX_FloatRect(0, 2, 208, 106)},
  };
  for (Source source : kSources) {
    SCOPED_TRACE(NameOf(source));
    const unsigned int drawing = AddDrawing(doc_.get(), source);
    ASSERT_NE(0u, drawing);
    for (const auto& expected : kCases) {
      SCOPED_TRACE(expected.name);
      ScopedFPDFAnnotation button = NewButton(kStands, expected.border_width);
      ASSERT_TRUE(button);
      if (expected.painted) {
        ASSERT_TRUE(
            EPDFAnnot_SetMKColor(button.get(), EPDF_MK_COLOR_BC, 0, 255, 0));
      }
      if (expected.fit_to_bounds) {
        MutableDictOf(button.get())
            ->GetOrCreateDictFor("MK")
            ->SetNewFor<CPDF_Dictionary>("IF")
            ->SetNewFor<CPDF_Boolean>("FB", true);
      }
      SetIcon(button.get(), drawing);

      const std::optional<DrawnIcon> drawn = DrawnIconOf(button.get());
      ASSERT_TRUE(drawn);
      ExpectRectNear(expected.plate, drawn->plate);
      ExpectRectNear(expected.picture, drawn->picture);
      // The border is stroked before the picture is drawn, when it has a
      // colour.
      const std::string content = StreamBytes(AppearanceOf(button.get()).Get());
      EXPECT_EQ(expected.painted, content.find(" re S\n") != std::string::npos);
    }
  }
}

// The icon is fitted as the file's icon fit dictionary says (ISO 32000-2
// Table 250), and by the table's defaults without one: always scaled,
// keeping its proportions, in the middle.
TEST_F(EPDFWidgetDrawingEmbedderTest, AnIconIsFittedAsTheIconFitSays) {
  struct Fit {
    const char* scale_when = nullptr;  // /SW
    const char* scale_type = nullptr;  // /S
    std::optional<CFX_PointF> share;   // /A
  };
  const struct {
    const char* name;
    float width;
    float height;
    std::optional<Fit> fit;
    CFX_FloatRect picture;
  } kCases[] = {
      {"no /IF", 300, 100, {}, CFX_FloatRect(50, 0, 250, 100)},
      {"an empty /IF", 300, 100, Fit{}, CFX_FloatRect(50, 0, 250, 100)},
      {"the defaults spelled out", 300, 100,
       Fit{"A", "P", CFX_PointF(0.5f, 0.5f)}, CFX_FloatRect(50, 0, 250, 100)},
      {"without its proportions", 300, 100, Fit{nullptr, "A", {}},
       CFX_FloatRect(0, 0, 300, 100)},
      {"at the left", 300, 100, Fit{nullptr, nullptr, CFX_PointF(0, 0)},
       CFX_FloatRect(0, 0, 200, 100)},
      {"at the right", 300, 100, Fit{nullptr, nullptr, CFX_PointF(1, 1)},
       CFX_FloatRect(100, 0, 300, 100)},
      {"at the top", 100, 100, Fit{nullptr, nullptr, CFX_PointF(0.5f, 1)},
       CFX_FloatRect(0, 50, 100, 100)},
      // A share outside 0 to 1 is brought back to it.
      {"past the right", 300, 100, Fit{nullptr, nullptr, CFX_PointF(5, 0)},
       CFX_FloatRect(100, 0, 300, 100)},
      // Never scaled: its own size, in the middle, clipped where it is
      // larger.
      {"never, in a larger button", 300, 200, Fit{"N", nullptr, {}},
       CFX_FloatRect(50, 50, 250, 150)},
      {"never, in a smaller button", 100, 40, Fit{"N", nullptr, {}},
       CFX_FloatRect(-50, -30, 150, 70)},
      // Only when it is bigger than the button.
      {"when bigger, in a larger button", 300, 200, Fit{"B", nullptr, {}},
       CFX_FloatRect(50, 50, 250, 150)},
      {"when bigger, in a smaller button", 100, 40, Fit{"B", nullptr, {}},
       CFX_FloatRect(10, 0, 90, 40)},
      // Only when it is smaller than the button.
      {"when smaller, in a larger button", 300, 200, Fit{"S", nullptr, {}},
       CFX_FloatRect(0, 25, 300, 175)},
      {"when smaller, in a smaller button", 100, 40, Fit{"S", nullptr, {}},
       CFX_FloatRect(-50, -30, 150, 70)},
  };
  for (Source source : kSources) {
    SCOPED_TRACE(NameOf(source));
    const unsigned int drawing = AddDrawing(doc_.get(), source);
    ASSERT_NE(0u, drawing);
    for (const auto& expected : kCases) {
      SCOPED_TRACE(expected.name);
      ScopedFPDFAnnotation button = NewButton(
          {50, 150 + expected.height, 50 + expected.width, 150}, 0.0f);
      ASSERT_TRUE(button);
      if (expected.fit) {
        RetainPtr<CPDF_Dictionary> fit = MutableDictOf(button.get())
                                             ->GetOrCreateDictFor("MK")
                                             ->SetNewFor<CPDF_Dictionary>("IF");
        if (expected.fit->scale_when) {
          fit->SetNewFor<CPDF_Name>("SW", expected.fit->scale_when);
        }
        if (expected.fit->scale_type) {
          fit->SetNewFor<CPDF_Name>("S", expected.fit->scale_type);
        }
        if (expected.fit->share) {
          RetainPtr<CPDF_Array> share = fit->SetNewFor<CPDF_Array>("A");
          share->AppendNew<CPDF_Number>(expected.fit->share->x);
          share->AppendNew<CPDF_Number>(expected.fit->share->y);
        }
      }
      SetIcon(button.get(), drawing);

      const std::optional<DrawnIcon> drawn = DrawnIconOf(button.get());
      ASSERT_TRUE(drawn);
      ExpectRectNear(CFX_FloatRect(0, 0, expected.width, expected.height),
                     drawn->plate);
      ExpectRectNear(expected.picture, drawn->picture);
    }
  }
}

namespace {

// What a picture in a box turned a quarter turn looks like, for a box that
// stands 40 wide and 160 tall: the frame it is laid out in, the turn, where
// the picture lands in the frame, and a point of its red half and of its
// blue half, from the box's lower left corner. An icon without a border and
// a mark land the same.
struct Turn {
  std::optional<int> written;  // /MK /R
  CFX_FloatRect frame;
  CFX_Matrix turn;
  CFX_FloatRect picture;
  int red_x;
  int red_y;
  int blue_x;
  int blue_y;
};
const Turn kTurns[] = {
    // As it stands, 40 by 160: the picture is 40 by 20 in its middle, red at
    // the left.
    {{},
     CFX_FloatRect(0, 0, 40, 160),
     CFX_Matrix(),
     CFX_FloatRect(0, 70, 40, 90),
     10,
     80,
     30,
     80},
    // Laid out 160 by 40: the picture is 80 by 40 in its middle. Turned
    // counterclockwise, its left is at the bottom.
    {90, CFX_FloatRect(0, 0, 160, 40), CFX_Matrix(0, 1, -1, 0, 40, 0),
     CFX_FloatRect(40, 0, 120, 40), 20, 60, 20, 100},
    // Upside down: its left is at the right.
    {180, CFX_FloatRect(0, 0, 40, 160), CFX_Matrix(-1, 0, 0, -1, 40, 160),
     CFX_FloatRect(0, 70, 40, 90), 30, 80, 10, 80},
    // Turned clockwise, its left is at the top; -90 is the same turn.
    {270, CFX_FloatRect(0, 0, 160, 40), CFX_Matrix(0, -1, 1, 0, 0, 160),
     CFX_FloatRect(40, 0, 120, 40), 20, 100, 20, 60},
    {-90, CFX_FloatRect(0, 0, 160, 40), CFX_Matrix(0, -1, 1, 0, 0, 160),
     CFX_FloatRect(40, 0, 120, 40), 20, 100, 20, 60},
};

// Where the box for the turn at `index` stands, in a row that starts at
// `bottom`: 40 wide and 160 tall, 70 from the one before it.
FS_RECTF TurnedBoxAt(int index, int bottom) {
  const float left = 20 + 70 * index;
  return {left, bottom + 160.0f, left + 40, static_cast<float>(bottom)};
}

void WriteTurn(FPDF_ANNOTATION widget, const Turn& turn) {
  if (turn.written) {
    MutableDictOf(widget)->GetOrCreateDictFor("MK")->SetNewFor<CPDF_Number>(
        "R", *turn.written);
  }
}

}  // namespace

// A turned button's icon is fitted into the frame its quarter turn gives,
// /Rect with its sides swapped at 90 and 270, and turned with it: the
// appearance's /Matrix is the turn a field's own appearance has.
TEST_F(EPDFWidgetDrawingEmbedderTest, AnIconTurnsWithItsButton) {
  int page_index = 0;
  for (Source source : kSources) {
    SCOPED_TRACE(NameOf(source));
    ScopedFPDFPage page(FPDFPage_New(doc_.get(), ++page_index, 400, 400));
    ASSERT_TRUE(page);
    const unsigned int drawing = AddDrawing(doc_.get(), source);
    ASSERT_NE(0u, drawing);
    int index = 0;
    for (const Turn& expected : kTurns) {
      SCOPED_TRACE(expected.written.value_or(0));
      const FS_RECTF stands = TurnedBoxAt(index++, 200);
      ScopedFPDFAnnotation button = NewButton(stands, 0.0f, page.get());
      ASSERT_TRUE(button);
      WriteTurn(button.get(), expected);
      SetIcon(button.get(), drawing);

      RetainPtr<const CPDF_Stream> appearance = AppearanceOf(button.get());
      ASSERT_TRUE(appearance);
      EXPECT_EQ(expected.frame, appearance->GetDict()->GetRectFor("BBox"));
      EXPECT_EQ(expected.turn, appearance->GetDict()->GetMatrixFor("Matrix"));
      const std::optional<DrawnIcon> drawn = DrawnIconOf(button.get());
      ASSERT_TRUE(drawn);
      ExpectRectNear(expected.frame, drawn->plate);
      ExpectRectNear(expected.picture, drawn->picture);
      const Shown shown(page.get());
      const int left = static_cast<int>(stands.left);
      EXPECT_EQ(kRed, shown.At(left + expected.red_x, 200 + expected.red_y));
      EXPECT_EQ(kBlue, shown.At(left + expected.blue_x, 200 + expected.blue_y));
      // /Rect is where the button stands, whatever its turn.
      EXPECT_EQ(CFX_FloatRect(stands.left, 200, stands.right, 360),
                DictOf(button.get())->GetRectFor("Rect"));
    }
  }
}

// A mark is placed in a widget as EPDFAnnot_UpdateAppearanceToRect() fits one
// today: a widget given the picture's page and fitted, and a widget given
// the picture's drawing, get the same wrapper, at every turn and with every
// fit. The first makes a drawing of its own; the second places one the
// document has.
TEST_F(EPDFWidgetDrawingEmbedderTest, AMarkIsPlacedAsAPageIsFitted) {
  int page_index = 0;
  for (Source source : kSources) {
    SCOPED_TRACE(NameOf(source));
    ScopedFPDFPage page(FPDFPage_New(doc_.get(), ++page_index, 400, 400));
    ASSERT_TRUE(page);
    embedpdf_test::ScopedTestDocument picture = NewPicturePage(source);
    const unsigned int drawing = AddDrawing(doc_.get(), source);
    ASSERT_NE(0u, drawing);
    const std::string drawing_before = ObjectGraph(pdf())[drawing];

    int index = 0;
    for (const Turn& expected : kTurns) {
      SCOPED_TRACE(expected.written.value_or(0));
      // The widget fitted from the page stands above the one given the
      // drawing.
      const FS_RECTF from_page_at = TurnedBoxAt(index, 220);
      const FS_RECTF from_drawing_at = TurnedBoxAt(index++, 20);
      ScopedFPDFAnnotation from_page =
          NewSignatureWidget(from_page_at, page.get());
      ScopedFPDFAnnotation from_drawing =
          NewSignatureWidget(from_drawing_at, page.get());
      ASSERT_TRUE(from_page);
      ASSERT_TRUE(from_drawing);
      WriteTurn(from_page.get(), expected);
      WriteTurn(from_drawing.get(), expected);
      ASSERT_TRUE(
          EPDFAnnot_SetAppearanceFromPage(from_page.get(), picture.get(), 0));
      ASSERT_TRUE(EPDFAnnot_UpdateAppearanceToRect(from_page.get(),
                                                   EPDF_STAMP_FIT_CONTAIN));
      ASSERT_TRUE(EPDFAnnot_SetDrawing(from_drawing.get(), drawing,
                                       EPDF_STAMP_FIT_CONTAIN));

      RetainPtr<const CPDF_Stream> fitted = AppearanceOf(from_page.get());
      RetainPtr<const CPDF_Stream> placed = AppearanceOf(from_drawing.get());
      ASSERT_TRUE(fitted);
      ASSERT_TRUE(placed);
      EXPECT_EQ(StreamBytes(fitted.Get()), StreamBytes(placed.Get()));
      EXPECT_EQ(expected.frame, placed->GetDict()->GetRectFor("BBox"));
      EXPECT_EQ(fitted->GetDict()->GetRectFor("BBox"),
                placed->GetDict()->GetRectFor("BBox"));
      EXPECT_EQ(fitted->GetDict()->GetMatrixFor("Matrix"),
                placed->GetDict()->GetMatrixFor("Matrix"));
      EXPECT_EQ(fitted->GetDict()->KeyExist("Matrix"),
                placed->GetDict()->KeyExist("Matrix"));
      if (placed->GetDict()->KeyExist("Matrix")) {
        EXPECT_EQ(expected.turn, placed->GetDict()->GetMatrixFor("Matrix"));
      }
      // Each reads its drawing: the page's own, and the document's.
      EXPECT_EQ(drawing, EPDFAnnot_GetDrawing(from_drawing.get()));
      EXPECT_NE(0u, EPDFAnnot_GetDrawing(from_page.get()));
      EXPECT_NE(drawing, EPDFAnnot_GetDrawing(from_page.get()));
      // A widget's /Rect and /MK are read, never written.
      EXPECT_EQ(
          CFX_FloatRect(from_drawing_at.left, 20, from_drawing_at.right, 180),
          DictOf(from_drawing.get())->GetRectFor("Rect"));
      if (expected.written) {
        EXPECT_EQ(
            *expected.written,
            DictOf(from_drawing.get())->GetDictFor("MK")->GetIntegerFor("R"));
      }
      const Shown shown(page.get());
      for (const FS_RECTF& stands : {from_page_at, from_drawing_at}) {
        const int left = static_cast<int>(stands.left);
        const int bottom = static_cast<int>(stands.bottom);
        EXPECT_EQ(kRed,
                  shown.At(left + expected.red_x, bottom + expected.red_y));
        EXPECT_EQ(kBlue,
                  shown.At(left + expected.blue_x, bottom + expected.blue_y));
      }
    }

    // Every fit, in a box 120 by 30: the same wrapper either way.
    for (EPDF_STAMP_FIT fit : {EPDF_STAMP_FIT_CONTAIN, EPDF_STAMP_FIT_COVER,
                               EPDF_STAMP_FIT_STRETCH}) {
      SCOPED_TRACE(fit);
      const FS_RECTF stands{260, 390, 380, 360};
      ScopedFPDFAnnotation from_page = NewSignatureWidget(stands, page.get());
      ScopedFPDFAnnotation from_drawing =
          NewSignatureWidget(stands, page.get());
      ASSERT_TRUE(
          EPDFAnnot_SetAppearanceFromPage(from_page.get(), picture.get(), 0));
      ASSERT_TRUE(EPDFAnnot_UpdateAppearanceToRect(from_page.get(), fit));
      ASSERT_TRUE(EPDFAnnot_SetDrawing(from_drawing.get(), drawing, fit));
      EXPECT_EQ(StreamBytes(AppearanceOf(from_page.get()).Get()),
                StreamBytes(AppearanceOf(from_drawing.get()).Get()));
      EXPECT_EQ(
          AppearanceOf(from_page.get())->GetDict()->GetRectFor("BBox"),
          AppearanceOf(from_drawing.get())->GetDict()->GetRectFor("BBox"));
    }
    // Placing a drawing never writes it.
    EXPECT_EQ(drawing_before, ObjectGraph(pdf())[drawing]);
  }
}

// A drawing whose placement is folded into its form's /Matrix, as a
// canonical drawing can have it, is placed by the box it shows: /BBox
// through /Matrix. As an icon and as a mark it lands as the plain picture
// does.
TEST_F(EPDFWidgetDrawingEmbedderTest, ADrawingWithAMatrixIsPlacedByItsBox) {
  const std::string bytes = CanonicalPageWithAMatrix();
  embedpdf_test::ScopedTestDocument canonical(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  ASSERT_TRUE(canonical);
  const unsigned int drawing =
      EPDFDoc_ImportDrawing(doc_.get(), canonical.get());
  ASSERT_NE(0u, drawing);
  {
    // Adopted as it is: its box off the origin, and its matrix.
    RetainPtr<const CPDF_Stream> form =
        ToStream(pdf()->GetOrParseIndirectObject(drawing));
    ASSERT_TRUE(form);
    EXPECT_EQ(CFX_FloatRect(10, 20, 110, 70),
              form->GetDict()->GetRectFor("BBox"));
    EXPECT_EQ(CFX_Matrix(2, 0, 0, 2, -20, -40),
              form->GetDict()->GetMatrixFor("Matrix"));
  }

  // An icon: a button 204 by 104 with a border of 2 shows it at its size.
  ScopedFPDFAnnotation button = NewButton({100, 304, 304, 200}, 2.0f);
  ASSERT_TRUE(button);
  SetIcon(button.get(), drawing);
  const std::optional<DrawnIcon> drawn = DrawnIconOf(button.get());
  ASSERT_TRUE(drawn);
  ExpectRectNear(CFX_FloatRect(2, 2, 202, 102), drawn->picture);

  // A mark: a widget 100 by 50 shows it at half its size.
  ScopedFPDFAnnotation widget = NewSignatureWidget({100, 150, 200, 100});
  ASSERT_TRUE(widget);
  ASSERT_TRUE(
      EPDFAnnot_SetDrawing(widget.get(), drawing, EPDF_STAMP_FIT_CONTAIN));
  EXPECT_EQ(drawing, EPDFAnnot_GetDrawing(widget.get()));

  const Shown shown(page_.get());
  EXPECT_EQ(kRed, shown.At(150, 250));
  EXPECT_EQ(kBlue, shown.At(250, 250));
  EXPECT_EQ(kWhite, shown.At(150, 310));
  EXPECT_EQ(kRed, shown.At(125, 125));
  EXPECT_EQ(kBlue, shown.At(175, 125));
  EXPECT_EQ(kWhite, shown.At(125, 160));
}

// A field with several widgets: each names the same drawing as its icon, and
// the file holds the picture once.
TEST_F(EPDFWidgetDrawingEmbedderTest, EachWidgetOfAFieldShowsTheSameIcon) {
  ScopedFPDFPage second(FPDFPage_New(doc_.get(), 1, 400, 400));
  ASSERT_TRUE(second);
  const uint32_t field = NewField(doc_.get(), EPDF_FORMFIELD_FAMILY_PUSHBUTTON);
  ASSERT_GT(field, 0u);
  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_NE(0u, drawing);
  const FS_RECTF kLow{100, 150, 200, 100};   // 100 by 50, at (100, 100)
  const FS_RECTF kHigh{100, 350, 200, 300};  // 100 by 50, at (100, 300)
  ScopedFPDFAnnotation widgets[] = {
      NewWidget(doc_.get(), field, page_.get(), kLow, 0.0f),
      NewWidget(doc_.get(), field, page_.get(), kHigh, 0.0f),
      NewWidget(doc_.get(), field, second.get(), kLow, 0.0f),
  };
  for (const ScopedFPDFAnnotation& widget : widgets) {
    ASSERT_TRUE(widget);
    SetIcon(widget.get(), drawing);
    EXPECT_EQ(drawing, EPDFAnnot_GetMKIcon(widget.get(), EPDF_MK_ICON_I));
    EXPECT_EQ(drawing, DrawnIconNumber(widget.get()));
    const std::optional<DrawnIcon> drawn = DrawnIconOf(widget.get());
    ASSERT_TRUE(drawn);
    ExpectRectNear(CFX_FloatRect(0, 0, 100, 50), drawn->picture);
  }
  EXPECT_EQ(std::vector<unsigned int>({drawing}), DrawingsOf(doc_.get()));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(doc_.get());
  ASSERT_TRUE(model);
  EXPECT_EQ(3, EPDFForm_CountFieldWidgets(
                   model, EPDFForm_GetFieldIndexByObjNum(model, field)));
  EPDFForm_CloseModel(model);

  const Shown first_page(page_.get());
  EXPECT_EQ(kRed, first_page.At(125, 125));
  EXPECT_EQ(kBlue, first_page.At(175, 125));
  EXPECT_EQ(kRed, first_page.At(125, 325));
  EXPECT_EQ(kBlue, first_page.At(175, 325));
  EXPECT_EQ(kRed, ShownColorAt(second.get(), 125, 125));
  EXPECT_EQ(kBlue, ShownColorAt(second.get(), 175, 125));
  for (ScopedFPDFAnnotation& widget : widgets) {
    widget.reset();
  }
  EXPECT_EQ(1, PicturesIn(SavedBytes(doc_.get()), Source::kVector));
}

// A saved file holds the icon in both places, as Acrobat writes an image
// field: /MK /I with /MK /TP 1, and an /AP /N that draws the same form.
TEST_F(EPDFWidgetDrawingEmbedderTest, ASavedFileHoldsTheIconInBothPlaces) {
  {
    ScopedFPDFAnnotation button = NewButton({100, 304, 304, 200}, 2.0f);
    ASSERT_TRUE(button);
    ASSERT_TRUE(
        EPDFAnnot_SetMKColor(button.get(), EPDF_MK_COLOR_BC, 0, 255, 0));
    ASSERT_TRUE(
        EPDFAnnot_SetMKColor(button.get(), EPDF_MK_COLOR_BG, 255, 255, 204));
    SetIcon(button.get(), AddDrawing(doc_.get()));
  }
  const std::string bytes = SavedBytes(doc_.get());
  ASSERT_FALSE(bytes.empty());
  KeepSample(bytes, "button-picture-upright");

  embedpdf_test::ScopedTestDocument reopened(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  ASSERT_TRUE(reopened);
  ScopedFPDFPage page(FPDF_LoadPage(reopened.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
  ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(button);
  EXPECT_TRUE(EPDFAnnot_HasMKIcon(button.get(), EPDF_MK_ICON_I));
  EXPECT_EQ(1, EPDFAnnot_GetMKTextPosition(button.get()));
  const unsigned int icon = EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I);
  EXPECT_GT(icon, 0u);
  EXPECT_EQ(icon, DrawnIconNumber(button.get()));
  EXPECT_EQ(std::vector<unsigned int>({icon}), DrawingsOf(reopened.get()));
  const std::optional<DrawnIcon> drawn = DrawnIconOf(button.get());
  ASSERT_TRUE(drawn);
  ExpectRectNear(CFX_FloatRect(2, 2, 202, 102), drawn->picture);
  const Shown shown(page.get());
  EXPECT_EQ(kRed, shown.At(150, 250));
  EXPECT_EQ(kBlue, shown.At(250, 250));
  // The border, in the middle of its 2 points.
  EXPECT_EQ(kGreen, shown.At(101, 250));
  button.reset();
}

// On a page turned a quarter turn, a button turned the other way reads
// upright: the picture's left is at the reader's left.
TEST_F(EPDFWidgetDrawingEmbedderTest, AnIconReadsUprightOnATurnedPage) {
  // The page is shown turned 90 clockwise. The button stands 52 wide and 102
  // tall on it, and is turned 90 counterclockwise, so the reader sees it 102
  // wide and 52 tall.
  FPDFPage_SetRotation(page_.get(), 1);
  {
    ScopedFPDFAnnotation button = NewButton({100, 202, 152, 100});
    ASSERT_TRUE(button);
    ASSERT_TRUE(EPDFAnnot_SetMKRotation(button.get(), 90));
    ASSERT_TRUE(
        EPDFAnnot_SetMKColor(button.get(), EPDF_MK_COLOR_BC, 0, 255, 0));
    SetIcon(button.get(), AddDrawing(doc_.get()));
    const std::optional<DrawnIcon> drawn = DrawnIconOf(button.get());
    ASSERT_TRUE(drawn);
    ExpectRectNear(CFX_FloatRect(1, 1, 101, 51), drawn->picture);
  }
  const std::string bytes = SavedBytes(doc_.get());
  ASSERT_FALSE(bytes.empty());
  KeepSample(bytes, "button-picture-on-turned-page");

  embedpdf_test::ScopedTestDocument reopened(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  ASSERT_TRUE(reopened);
  ScopedFPDFPage page(FPDF_LoadPage(reopened.get(), 0));
  ASSERT_TRUE(page);
  // Shown, a page point (x, y) is at (y, x) from the top left: the button
  // spans 100 to 202 across and 100 to 152 down, so 248 to 300 up from the
  // bottom.
  const Shown shown(page.get());
  EXPECT_EQ(kRed, shown.At(125, 274));
  EXPECT_EQ(kBlue, shown.At(175, 274));
  EXPECT_EQ(kWhite, shown.At(125, 230));
}

// A button another app made with a picture reads as having one in every
// layout, and drawing its appearance again never takes the picture out of
// the file: /MK /I still names the same form, and the form is as it was.
// Drawn again, every layout that shows an icon shows this one, and every
// layout that shows a caption shows it, in a stream of its own that leaves
// the other app's appearance as it was. The document lists the form as a
// drawing, once, whoever made it.
TEST_F(EPDFWidgetDrawingEmbedderTest, AForeignButtonKeepsItsIconInEveryLayout) {
  const std::string bytes = ForeignButtonsPdf();
  embedpdf_test::ScopedTestDocument foreign(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  ASSERT_TRUE(foreign);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(foreign.get());
  const std::map<uint32_t, std::string> before = ObjectGraph(doc);
  ASSERT_FALSE(before.at(kForeignIcon).empty());
  EXPECT_EQ(std::vector<unsigned int>({kForeignIcon}),
            DrawingsOf(foreign.get()));
  ScopedFPDFPage page(FPDF_LoadPage(foreign.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_EQ(7, FPDFPage_GetAnnotCount(page.get()));
  for (int layout = 0; layout <= 6; ++layout) {
    SCOPED_TRACE(layout);
    ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), layout));
    ASSERT_TRUE(button);
    EXPECT_EQ(layout, EPDFAnnot_GetMKTextPosition(button.get()));
    EXPECT_TRUE(EPDFAnnot_HasMKIcon(button.get(), EPDF_MK_ICON_I));
    EXPECT_EQ(kForeignIcon, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
    // Its appearance draws the icon and is no wrapper of ours.
    EXPECT_EQ(0u, EPDFAnnot_GetDrawing(button.get()));
    const uint32_t foreign_appearance = AppearanceOf(button.get())->GetObjNum();

    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
    EXPECT_TRUE(EPDFAnnot_HasMKIcon(button.get(), EPDF_MK_ICON_I));
    EXPECT_EQ(kForeignIcon, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));

    // The button stands at (20, 10 + 50 * layout), 102 by 42.
    const float bottom = 10 + 50 * layout;
    const Shown shown(page.get(), 4);
    const bool shows_icon = layout != 0;
    const bool shows_caption = layout != 1;
    EXPECT_EQ(shows_icon, DrawnIconOf(button.get()).has_value());
    // A new stream in every layout, and it names the icon only where it
    // draws it.
    EXPECT_NE(foreign_appearance, AppearanceOf(button.get())->GetObjNum());
    EXPECT_EQ(before.at(foreign_appearance),
              ObjectGraph(doc)[foreign_appearance]);
    EXPECT_EQ(shows_icon ? kForeignIcon : 0u, DrawnIconNumber(button.get()));
    if (shows_icon) {
      const std::optional<DrawnIcon> icon = DrawnIconOf(button.get());
      ASSERT_TRUE(icon);
      const CFX_PointF red = RedPointOf(icon->picture);
      const CFX_PointF blue = BluePointOf(icon->picture);
      EXPECT_EQ(kRed, shown.At(20 + red.x, bottom + red.y));
      EXPECT_EQ(kBlue, shown.At(20 + blue.x, bottom + blue.y));
    }
    const std::optional<CFX_FloatRect> caption = DrawnCaptionOf(button.get());
    EXPECT_EQ(shows_caption, caption.has_value());
    EXPECT_EQ(shows_caption,
              shown.HasInkIn(CFX_FloatRect(20, bottom, 122, bottom + 42)));
    if (shows_icon && shows_caption) {
      ExpectPartsShare(layout, CFX_FloatRect(1, 1, 101, 41),
                       CaptionRoomOf(button.get()), *caption,
                       DrawnIconOf(button.get())->plate);
    }
  }
  EXPECT_EQ(before.at(kForeignIcon), ObjectGraph(doc)[kForeignIcon]);
  EXPECT_EQ(std::vector<unsigned int>({kForeignIcon}),
            DrawingsOf(foreign.get()));

  // The icon exports as a drawing: its canonical page shows it.
  ScopedFPDFDocument exported(
      EPDFDoc_ExportDrawing(foreign.get(), kForeignIcon));
  ASSERT_TRUE(exported);
  ScopedFPDFPage exported_page(FPDF_LoadPage(exported.get(), 0));
  ASSERT_TRUE(exported_page);
  EXPECT_EQ(200, FPDF_GetPageWidthF(exported_page.get()));
  EXPECT_EQ(100, FPDF_GetPageHeightF(exported_page.get()));
  EXPECT_EQ(kRed, ShownColorAt(exported_page.get(), 50, 50));
  EXPECT_EQ(kBlue, ShownColorAt(exported_page.get(), 150, 50));
}

// A button another app made that shows its icon alone keeps showing it when
// its appearance is drawn again: as it is, after a restyle, at a new place
// and size, and turned.
TEST_F(EPDFWidgetDrawingEmbedderTest, AForeignIconOnlyButtonSurvivesARedraw) {
  const std::string bytes = ForeignButtonsPdf();
  embedpdf_test::ScopedTestDocument foreign(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  ASSERT_TRUE(foreign);
  ScopedFPDFPage page(FPDF_LoadPage(foreign.get(), 0));
  ASSERT_TRUE(page);
  // The second widget: /MK /TP 1, standing at (20, 60), 102 by 42.
  ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), 1));
  ASSERT_TRUE(button);
  ASSERT_EQ(1, EPDFAnnot_GetMKTextPosition(button.get()));
  // As the other app drew it: 80 by 40 in the middle, inside a border of 1.
  EXPECT_EQ(kRed, ShownColorAt(page.get(), 51, 81));
  EXPECT_EQ(kBlue, ShownColorAt(page.get(), 91, 81));
  const uint32_t foreign_appearance = AppearanceOf(button.get())->GetObjNum();

  // Drawn again, it is the same picture in the same place, in a stream of
  // our own.
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
  EXPECT_NE(foreign_appearance, AppearanceOf(button.get())->GetObjNum());
  EXPECT_EQ(kForeignIcon, DrawnIconNumber(button.get()));
  std::optional<DrawnIcon> drawn = DrawnIconOf(button.get());
  ASSERT_TRUE(drawn);
  ExpectRectNear(CFX_FloatRect(1, 1, 101, 41), drawn->plate);
  ExpectRectNear(CFX_FloatRect(11, 1, 91, 41), drawn->picture);
  EXPECT_EQ(kRed, ShownColorAt(page.get(), 51, 81));
  EXPECT_EQ(kBlue, ShownColorAt(page.get(), 91, 81));
  // An icon-only button shows no caption, though the file has one.
  EXPECT_EQ(std::string::npos,
            StreamBytes(AppearanceOf(button.get()).Get()).find("BT"));

  // A restyle: a background and a wider border.
  ASSERT_TRUE(
      EPDFAnnot_SetMKColor(button.get(), EPDF_MK_COLOR_BG, 255, 255, 204));
  ASSERT_TRUE(EPDFAnnot_SetMKColor(button.get(), EPDF_MK_COLOR_BC, 0, 255, 0));
  ASSERT_TRUE(EPDFAnnot_SetBorderStyle(button.get(), FPDF_ANNOT_BS_SOLID, 3));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
  EXPECT_EQ(kForeignIcon, DrawnIconNumber(button.get()));
  drawn = DrawnIconOf(button.get());
  ASSERT_TRUE(drawn);
  ExpectRectNear(CFX_FloatRect(3, 3, 99, 39), drawn->plate);
  ExpectRectNear(CFX_FloatRect(15, 3, 87, 39), drawn->picture);
  EXPECT_EQ(kRed, ShownColorAt(page.get(), 51, 81));
  EXPECT_EQ(kBlue, ShownColorAt(page.get(), 91, 81));

  // A new place and size: 206 by 106 at (150, 250).
  const FS_RECTF kMoved{150, 356, 356, 250};
  ASSERT_TRUE(EPDFAnnot_SetRect(button.get(), &kMoved));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
  EXPECT_EQ(kForeignIcon, DrawnIconNumber(button.get()));
  drawn = DrawnIconOf(button.get());
  ASSERT_TRUE(drawn);
  ExpectRectNear(CFX_FloatRect(3, 3, 203, 103), drawn->picture);
  EXPECT_EQ(kRed, ShownColorAt(page.get(), 200, 300));
  EXPECT_EQ(kBlue, ShownColorAt(page.get(), 300, 300));

  // Turned a quarter turn: laid out 106 by 206, the picture is 100 by 50 in
  // its middle and its left is at the bottom.
  ASSERT_TRUE(EPDFAnnot_SetMKRotation(button.get(), 90));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
  EXPECT_EQ(kForeignIcon, DrawnIconNumber(button.get()));
  drawn = DrawnIconOf(button.get());
  ASSERT_TRUE(drawn);
  ExpectRectNear(CFX_FloatRect(3, 78, 103, 128), drawn->picture);
  EXPECT_EQ(kRed, ShownColorAt(page.get(), 253, 280));
  EXPECT_EQ(kBlue, ShownColorAt(page.get(), 253, 326));
  button.reset();
}

// A button that shows its icon alone draws no text, so its appearance needs
// no /DA and no font: a file that names neither still gets its picture.
TEST_F(EPDFWidgetDrawingEmbedderTest, AnIconOnlyButtonNeedsNoFont) {
  const std::string bytes = ForeignButtonsPdf(/*with_fonts=*/false);
  embedpdf_test::ScopedTestDocument foreign(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  ASSERT_TRUE(foreign);
  ScopedFPDFPage page(FPDF_LoadPage(foreign.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), 1));
  ASSERT_TRUE(button);
  const uint32_t foreign_appearance = AppearanceOf(button.get())->GetObjNum();
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
  EXPECT_NE(foreign_appearance, AppearanceOf(button.get())->GetObjNum());
  EXPECT_EQ(kForeignIcon, DrawnIconNumber(button.get()));
  EXPECT_EQ(kRed, ShownColorAt(page.get(), 51, 81));
  EXPECT_EQ(kBlue, ShownColorAt(page.get(), 91, 81));
  // Nothing was added for a font it doesn't use.
  EXPECT_FALSE(CPDFDocumentFromFPDFDocument(foreign.get())
                   ->GetRoot()
                   ->GetDictFor("AcroForm")
                   ->KeyExist("DR"));
  button.reset();
}

// An appearance that is the icon itself (/AP /N and /MK /I naming one form)
// is replaced by a stream of its own: drawing the button never writes into
// its picture, in any layout, whether the layout shows the icon or not.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       AnAppearanceThatIsTheIconIsNotWrittenInto) {
  const unsigned int icon = AddDrawing(doc_.get());
  ASSERT_NE(0u, icon);
  const std::string icon_before = ObjectGraph(pdf())[icon];
  for (int layout = 0; layout <= 6; ++layout) {
    SCOPED_TRACE(layout);
    ScopedFPDFPage page(FPDFPage_New(doc_.get(), layout + 1, 400, 400));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation button =
        NewButton({100, 250, 300, 150}, 0.0f, page.get());
    ASSERT_TRUE(button);
    ASSERT_TRUE(EPDFAnnot_SetMKText(button.get(), EPDF_MK_TEXT_CA,
                                    GetFPDFWideString(L"Go").get()));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(
        button.get(), FPDF_FONT_HELVETICA, 12, 0, 0, 0));
    ASSERT_TRUE(EPDFAnnot_SetMKIcon(button.get(), EPDF_MK_ICON_I, icon));
    ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(button.get(), layout));
    MutableDictOf(button.get())
        ->GetOrCreateDictFor("AP")
        ->SetNewFor<CPDF_Reference>("N", pdf(), icon);

    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
    EXPECT_NE(icon, AppearanceOf(button.get())->GetObjNum());
    EXPECT_EQ(icon_before, ObjectGraph(pdf())[icon]);
    EXPECT_EQ(icon, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
    if (layout == 0) {
      EXPECT_EQ(0u, DrawnIconNumber(button.get()));
      continue;
    }
    EXPECT_EQ(icon, DrawnIconNumber(button.get()));
    const std::optional<DrawnIcon> drawn = DrawnIconOf(button.get());
    ASSERT_TRUE(drawn);
    const CFX_PointF red = RedPointOf(drawn->picture);
    const CFX_PointF blue = BluePointOf(drawn->picture);
    const Shown shown(page.get(), 4);
    EXPECT_EQ(kRed, shown.At(100 + red.x, 150 + red.y));
    EXPECT_EQ(kBlue, shown.At(100 + blue.x, 150 + blue.y));
  }
}

// Every job of the engine runs in a layer transaction. Inside one, a drawing
// is added and shown as a button's icon and as a mark in another widget; a
// transaction that is dropped leaves both as they were.
TEST_F(EPDFWidgetDrawingEmbedderTest, WorksInsideALayerTransaction) {
  const std::string bytes = ForeignButtonsPdf();
  embedpdf_test::LayerFixture foreign;
  ASSERT_TRUE(foreign.OpenBytes({bytes.begin(), bytes.end()}));
  // The second widget shows its icon alone and stands at (20, 60); the third
  // stands at (20, 110). Both are 102 by 42.
  constexpr int kIconOnly = 1;
  constexpr int kMarked = 2;

  // The picture's page is made before the transaction: a test's own
  // documents are layers in layer mode, and no layer opens while a
  // transaction is open on its thread. Its canonical drawing is made and
  // added inside, as the engine does both in the job.
  embedpdf_test::ScopedTestDocument picture = NewPicturePage(Source::kVector);
  ASSERT_TRUE(picture);
  unsigned int drawing = 0;
  ASSERT_TRUE(EPDFLayer_BeginTransaction(foreign.layer));
  {
    ScopedFPDFDocument canonical(EPDFDoc_CanonicalDrawing(picture.get(), 0));
    ASSERT_TRUE(canonical);
    drawing = EPDFDoc_ImportDrawing(foreign.layer, canonical.get());
    ASSERT_NE(0u, drawing);
    ScopedFPDFPage page(FPDF_LoadPage(foreign.layer, 0));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), kIconOnly));
    ScopedFPDFAnnotation marked(FPDFPage_GetAnnot(page.get(), kMarked));
    ASSERT_TRUE(button);
    ASSERT_TRUE(marked);
    ASSERT_TRUE(EPDFAnnot_SetMKIcon(button.get(), EPDF_MK_ICON_I, drawing));
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
    ASSERT_TRUE(
        EPDFAnnot_SetDrawing(marked.get(), drawing, EPDF_STAMP_FIT_CONTAIN));
    EXPECT_EQ(drawing, DrawnIconNumber(button.get()));
    EXPECT_EQ(drawing, EPDFAnnot_GetDrawing(marked.get()));
  }
  ASSERT_TRUE(EPDFLayer_CommitTransaction(foreign.layer));
  auto expect_both_show = [&foreign, &drawing]() {
    ScopedFPDFPage page(FPDF_LoadPage(foreign.layer, 0));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), kIconOnly));
    ScopedFPDFAnnotation marked(FPDFPage_GetAnnot(page.get(), kMarked));
    ASSERT_TRUE(button);
    ASSERT_TRUE(marked);
    EXPECT_EQ(drawing, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
    EXPECT_EQ(drawing, DrawnIconNumber(button.get()));
    EXPECT_EQ(drawing, EPDFAnnot_GetDrawing(marked.get()));
    // The file's own icon, which five other widgets still name, and ours.
    EXPECT_EQ(std::vector<unsigned int>({kForeignIcon, drawing}),
              DrawingsOf(foreign.layer));
    const Shown shown(page.get());
    EXPECT_EQ(kRed, shown.At(51, 81));
    EXPECT_EQ(kBlue, shown.At(91, 81));
    // The mark fills the widget's height: 84 by 42, in its middle.
    EXPECT_EQ(kRed, shown.At(45, 131));
    EXPECT_EQ(kBlue, shown.At(95, 131));
  };
  expect_both_show();

  ASSERT_TRUE(EPDFLayer_BeginTransaction(foreign.layer));
  {
    ScopedFPDFPage page(FPDF_LoadPage(foreign.layer, 0));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), kIconOnly));
    ScopedFPDFAnnotation marked(FPDFPage_GetAnnot(page.get(), kMarked));
    ASSERT_TRUE(button);
    ASSERT_TRUE(marked);
    ASSERT_TRUE(EPDFAnnot_ClearMKIcon(button.get(), EPDF_MK_ICON_I));
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
    // Drawing the third widget again takes its mark away.
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(marked.get()));
    EXPECT_FALSE(EPDFAnnot_HasMKIcon(button.get(), EPDF_MK_ICON_I));
    EXPECT_EQ(0u, DrawnIconNumber(button.get()));
    EXPECT_EQ(0u, EPDFAnnot_GetDrawing(marked.get()));
    EXPECT_EQ(kWhite, ShownColorAt(page.get(), 51, 81));
  }
  ASSERT_TRUE(EPDFLayer_AbortTransaction(foreign.layer));
  expect_both_show();
}

// An icon that is replaced or cleared stays an object of the document: the
// appearance still draws it until it is drawn again, and another widget may
// name it. A save writes only what the document still reaches, so once
// nothing shows the old drawing it is in no saved file, full or incremental.
// A drawing nothing shows yet is in none either.
TEST_F(EPDFWidgetDrawingEmbedderTest, AReplacedOrClearedIconIsInNoSavedFile) {
  ScopedFPDFAnnotation button = NewButton({100, 250, 300, 150}, 0.0f);
  ASSERT_TRUE(button);
  const unsigned int first = AddDrawing(doc_.get());
  const unsigned int second =
      AddDrawing(doc_.get(), Source::kVector, kGreen, kYellow);
  SetIcon(button.get(), first);
  for (const auto& save : kSaves) {
    SCOPED_TRACE(save.name);
    const std::string bytes = SavedBytes(doc_.get(), save.flags);
    EXPECT_EQ(1, StreamsHolding(bytes, kPictureMark));
    EXPECT_EQ(0, StreamsHolding(bytes, kSecondPictureMark));
  }

  // Replaced and not drawn again: /MK names the new drawing and the
  // appearance still draws the old one, so a save holds both.
  ASSERT_TRUE(EPDFAnnot_SetMKIcon(button.get(), EPDF_MK_ICON_I, second));
  EXPECT_EQ(first, DrawnIconNumber(button.get()));
  EXPECT_EQ(kRed, ShownColorAt(page_.get(), 150, 200));
  for (const auto& save : kSaves) {
    SCOPED_TRACE(save.name);
    const std::string bytes = SavedBytes(doc_.get(), save.flags);
    EXPECT_EQ(1, StreamsHolding(bytes, kPictureMark));
    EXPECT_EQ(1, StreamsHolding(bytes, kSecondPictureMark));
  }

  // Drawn again, nothing shows the old drawing: neither save holds it.
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
  EXPECT_EQ(second, DrawnIconNumber(button.get()));
  EXPECT_EQ(kGreen, ShownColorAt(page_.get(), 150, 200));
  EXPECT_EQ(kYellow, ShownColorAt(page_.get(), 250, 200));
  for (const auto& save : kSaves) {
    SCOPED_TRACE(save.name);
    const std::string bytes = SavedBytes(doc_.get(), save.flags);
    EXPECT_EQ(0, StreamsHolding(bytes, kPictureMark));
    EXPECT_EQ(1, StreamsHolding(bytes, kSecondPictureMark));
  }

  // Cleared and drawn again: no picture in either save.
  ASSERT_TRUE(EPDFAnnot_ClearMKIcon(button.get(), EPDF_MK_ICON_I));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
  for (const auto& save : kSaves) {
    SCOPED_TRACE(save.name);
    const std::string bytes = SavedBytes(doc_.get(), save.flags);
    EXPECT_EQ(0, StreamsHolding(bytes, kPictureMark));
    EXPECT_EQ(0, StreamsHolding(bytes, kSecondPictureMark));
  }
}

// A picture the file already held when it was opened is not in a full save
// once it is replaced. An incremental save appends to the file's bytes, so
// the earlier revision's picture is still in them; the reader sees the new
// one.
TEST_F(EPDFWidgetDrawingEmbedderTest, AnIncrementalSaveKeepsWhatTheFileHeld) {
  {
    ScopedFPDFAnnotation button = NewButton({100, 250, 300, 150}, 0.0f);
    ASSERT_TRUE(button);
    SetIcon(button.get(), AddDrawing(doc_.get()));
  }
  const std::string earlier = SavedBytes(doc_.get());
  ASSERT_EQ(1, StreamsHolding(earlier, kPictureMark));

  embedpdf_test::ScopedTestDocument reopened(
      embedpdf_test::LoadMemDocumentForTest(earlier.data(), earlier.size(),
                                            nullptr));
  ASSERT_TRUE(reopened);
  {
    ScopedFPDFPage page(FPDF_LoadPage(reopened.get(), 0));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(button);
    SetIcon(button.get(),
            AddDrawing(reopened.get(), Source::kVector, kGreen, kYellow));
  }

  const std::string full = SavedBytes(reopened.get());
  EXPECT_EQ(0, StreamsHolding(full, kPictureMark));
  EXPECT_EQ(1, StreamsHolding(full, kSecondPictureMark));

  const std::string appended = SavedBytes(reopened.get(), FPDF_INCREMENTAL);
  ASSERT_GT(appended.size(), earlier.size());
  EXPECT_EQ(earlier, appended.substr(0, earlier.size()));
  EXPECT_EQ(1, StreamsHolding(appended, kPictureMark));
  EXPECT_EQ(1, StreamsHolding(appended, kSecondPictureMark));
  embedpdf_test::ScopedTestDocument read(embedpdf_test::LoadMemDocumentForTest(
      appended.data(), appended.size(), nullptr));
  ASSERT_TRUE(read);
  ScopedFPDFPage page(FPDF_LoadPage(read.get(), 0));
  ASSERT_TRUE(page);
  EXPECT_EQ(kGreen, ShownColorAt(page.get(), 150, 200));
  EXPECT_EQ(kYellow, ShownColorAt(page.get(), 250, 200));
}

// A push button that has no appearance is not drawn in memory, in any
// layout: a render draws a missing appearance for a text field and a choice
// field only. A button another app left without an /AP shows nothing, its
// picture included, and the render writes nothing.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       APushButtonWithoutAnAppearanceIsNotDrawn) {
  const std::string bytes =
      ForeignButtonsPdf(/*with_fonts=*/true, /*with_appearances=*/false);
  embedpdf_test::ScopedTestDocument foreign(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  ASSERT_TRUE(foreign);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(foreign.get());
  const uint32_t objects = doc->GetLastObjNum();
  ScopedFPDFPage page(FPDF_LoadPage(foreign.get(), 0));
  ASSERT_TRUE(page);
  const Shown shown(page.get());
  for (int layout = 0; layout <= 6; ++layout) {
    SCOPED_TRACE(layout);
    const int bottom = 10 + 50 * layout;
    // Where the icon's two halves and the caption would be.
    EXPECT_EQ(kWhite, shown.At(51, bottom + 21));
    EXPECT_EQ(kWhite, shown.At(91, bottom + 21));
    EXPECT_EQ(kWhite, shown.At(71, bottom + 21));
    ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), layout));
    ASSERT_TRUE(button);
    EXPECT_TRUE(EPDFAnnot_HasMKIcon(button.get(), EPDF_MK_ICON_I));
    EXPECT_FALSE(DictOf(button.get())->KeyExist("AP"));
  }
  EXPECT_EQ(objects, doc->GetLastObjNum());
}

// The generator can draw an icon-only button in memory: a stream of no
// document that draws the file's icon, and nothing is written. No render
// asks for it yet (the test above).
TEST_F(EPDFWidgetDrawingEmbedderTest, AnIconOnlyButtonCanBeDrawnInMemory) {
  const std::string bytes =
      ForeignButtonsPdf(/*with_fonts=*/false, /*with_appearances=*/false);
  embedpdf_test::ScopedTestDocument foreign(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  ASSERT_TRUE(foreign);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(foreign.get());
  const uint32_t objects = doc->GetLastObjNum();
  ScopedFPDFPage page(FPDF_LoadPage(foreign.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), 1));
  ASSERT_TRUE(button);

  std::optional<CPDF_AppearanceGenerator::GeneratedAP> drawn =
      CPDF_AppearanceGenerator::GenerateEphemeralFormAP(
          doc, DictOf(button.get()), CPDF_AppearanceGenerator::kPushButton);
  ASSERT_TRUE(drawn.has_value());
  ASSERT_TRUE(drawn->normal_stream);
  EXPECT_EQ(0u, drawn->normal_stream->GetObjNum());
  EXPECT_EQ(CFX_FloatRect(0, 0, 102, 42),
            drawn->normal_stream->GetDict()->GetRectFor("BBox"));
  EXPECT_EQ("q\nQ\nq\n1 1 100 40 re W n\n.4 0 0 .4 11 1 cm\n/FRM Do\nQ\n",
            StreamBytes(drawn->normal_stream.Get()));
  EXPECT_EQ(kForeignIcon, drawn->normal_stream->GetDict()
                              ->GetDictFor("Resources")
                              ->GetDictFor("XObject")
                              ->GetStreamFor("FRM")
                              ->GetObjNum());
  EXPECT_FALSE(DictOf(button.get())->KeyExist("AP"));
  EXPECT_EQ(objects, doc->GetLastObjNum());
}

// A refit after a move and a turn gives a widget a new wrapper around the
// same drawing: the drawing stays shared, and the other widget that places
// it is not touched.
TEST_F(EPDFWidgetDrawingEmbedderTest, ARefitKeepsTheDrawingShared) {
  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_NE(0u, drawing);
  ScopedFPDFAnnotation moved = NewSignatureWidget({20, 80, 140, 20});
  ScopedFPDFAnnotation kept = NewSignatureWidget({20, 380, 140, 320});
  ASSERT_TRUE(moved);
  ASSERT_TRUE(kept);
  for (FPDF_ANNOTATION widget : {moved.get(), kept.get()}) {
    ASSERT_TRUE(EPDFAnnot_SetDrawing(widget, drawing, EPDF_STAMP_FIT_CONTAIN));
  }
  const uint32_t first_wrapper = AppearanceOf(moved.get())->GetObjNum();
  const uint32_t kept_wrapper = AppearanceOf(kept.get())->GetObjNum();
  const std::map<uint32_t, std::string> before = ObjectGraph(pdf());

  // Moved to (200, 100), 40 wide and 160 tall, and turned a quarter turn.
  const FS_RECTF kStands{200, 260, 240, 100};
  ASSERT_TRUE(EPDFAnnot_SetRect(moved.get(), &kStands));
  ASSERT_TRUE(EPDFAnnot_SetMKRotation(moved.get(), 90));
  ASSERT_TRUE(
      EPDFAnnot_UpdateAppearanceToRect(moved.get(), EPDF_STAMP_FIT_CONTAIN));

  RetainPtr<const CPDF_Stream> wrapper = AppearanceOf(moved.get());
  ASSERT_TRUE(wrapper);
  EXPECT_NE(first_wrapper, wrapper->GetObjNum());
  EXPECT_EQ(CFX_FloatRect(0, 0, 160, 40),
            wrapper->GetDict()->GetRectFor("BBox"));
  EXPECT_EQ(CFX_Matrix(0, 1, -1, 0, 40, 0),
            wrapper->GetDict()->GetMatrixFor("Matrix"));
  EXPECT_EQ("q .4 0 0 .4 40 0 cm /EPDFWRAP Do Q", StreamBytes(wrapper.Get()));
  EXPECT_EQ(drawing, EPDFAnnot_GetDrawing(moved.get()));
  EXPECT_EQ(drawing, EPDFAnnot_GetDrawing(kept.get()));
  EXPECT_EQ(kept_wrapper, AppearanceOf(kept.get())->GetObjNum());
  const std::map<uint32_t, std::string> after = ObjectGraph(pdf());
  EXPECT_EQ(before.at(drawing), after.at(drawing));
  EXPECT_EQ(before.at(kept_wrapper), after.at(kept_wrapper));
  EXPECT_EQ(before.at(EPDFAnnot_GetObjectNumber(kept.get())),
            after.at(EPDFAnnot_GetObjectNumber(kept.get())));
  EXPECT_EQ(std::vector<unsigned int>({drawing}), DrawingsOf(doc_.get()));

  const Shown shown(page_.get());
  EXPECT_EQ(kRed, shown.At(220, 160));
  EXPECT_EQ(kBlue, shown.At(220, 200));
  EXPECT_EQ(kRed, shown.At(50, 350));
  EXPECT_EQ(kBlue, shown.At(110, 350));
  moved.reset();
  kept.reset();
  for (const auto& save : kSaves) {
    SCOPED_TRACE(save.name);
    EXPECT_EQ(1,
              PicturesIn(SavedBytes(doc_.get(), save.flags), Source::kVector));
  }
}

// The same picture as ten stamps, in ten signature widgets and in ten image
// fields is one drawing: the document lists one, and a saved file holds the
// picture once, full and incremental.
TEST_F(EPDFWidgetDrawingEmbedderTest, StampsMarksAndIconsShareOneDrawing) {
  for (Source source : kSources) {
    SCOPED_TRACE(NameOf(source));
    embedpdf_test::ScopedTestDocument document(NewTestDocument());
    ASSERT_TRUE(document);
    ScopedFPDFPage page(FPDFPage_New(document.get(), 0, 400, 400));
    ASSERT_TRUE(page);
    const unsigned int drawing = AddDrawing(document.get(), source);
    ASSERT_NE(0u, drawing);

    // Three columns of ten boxes, 100 by 30: stamps, marks, icons.
    auto box_at = [](int column, int row) {
      const float left = 20 + 130 * column;
      const float bottom = 20 + 36 * row;
      return FS_RECTF{left, bottom + 30, left + 100, bottom};
    };
    for (int row = 0; row < 10; ++row) {
      ScopedFPDFAnnotation stamp =
          NewAnnot(page.get(), FPDF_ANNOT_STAMP, box_at(0, row));
      ASSERT_TRUE(
          EPDFAnnot_SetDrawing(stamp.get(), drawing, EPDF_STAMP_FIT_CONTAIN));
      EXPECT_EQ(drawing, EPDFAnnot_GetDrawing(stamp.get()));

      ScopedFPDFAnnotation marked =
          NewWidget(document.get(),
                    NewField(document.get(), EPDF_FORMFIELD_FAMILY_SIGNATURE),
                    page.get(), box_at(1, row));
      ASSERT_TRUE(
          EPDFAnnot_SetDrawing(marked.get(), drawing, EPDF_STAMP_FIT_CONTAIN));
      EXPECT_EQ(drawing, EPDFAnnot_GetDrawing(marked.get()));

      ScopedFPDFAnnotation button =
          NewWidget(document.get(),
                    NewField(document.get(), EPDF_FORMFIELD_FAMILY_PUSHBUTTON),
                    page.get(), box_at(2, row), 0.0f);
      SetIcon(button.get(), drawing);
      EXPECT_EQ(drawing, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
    }
    EXPECT_EQ(std::vector<unsigned int>({drawing}), DrawingsOf(document.get()));

    // Each of the thirty shows it: 60 by 30, in the middle of its box.
    const Shown shown(page.get());
    for (int column = 0; column < 3; ++column) {
      for (int row = 0; row < 10; ++row) {
        SCOPED_TRACE(testing::Message() << column << ", " << row);
        const FS_RECTF box = box_at(column, row);
        const int left = static_cast<int>(box.left);
        const int middle = static_cast<int>(box.bottom) + 15;
        EXPECT_EQ(kRed, shown.At(left + 35, middle));
        EXPECT_EQ(kBlue, shown.At(left + 65, middle));
        EXPECT_EQ(kWhite, shown.At(left + 10, middle));
      }
    }
    for (const auto& save : kSaves) {
      SCOPED_TRACE(save.name);
      const std::string bytes = SavedBytes(document.get(), save.flags);
      EXPECT_EQ(1, PicturesIn(bytes, source));
      embedpdf_test::ScopedTestDocument reopened(
          embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                                nullptr));
      ASSERT_TRUE(reopened);
      EXPECT_EQ(1u, DrawingsOf(reopened.get()).size());
    }
  }
}

// After a reopen the document lists the drawing its stamp, its mark or its
// icon shows, and that drawing exports as the picture's canonical bytes: the
// engine knows it by its content, and places it again instead of adding a
// copy.
TEST_F(EPDFWidgetDrawingEmbedderTest, AReopenedFilePlacesItsDrawingAgain) {
  enum class Place { kStamp, kMark, kIcon };
  const struct {
    const char* name;
    Place place;
  } kPlaces[] = {{"a stamp", Place::kStamp},
                 {"a mark", Place::kMark},
                 {"an icon", Place::kIcon}};
  // Shows `drawing` in a new place of one kind, on a page of `document`.
  auto show = [this](FPDF_DOCUMENT document, FPDF_PAGE page, Place place,
                     unsigned int drawing, const FS_RECTF& rect) {
    if (place == Place::kStamp) {
      ScopedFPDFAnnotation stamp = NewAnnot(page, FPDF_ANNOT_STAMP, rect);
      ASSERT_TRUE(
          EPDFAnnot_SetDrawing(stamp.get(), drawing, EPDF_STAMP_FIT_CONTAIN));
      return;
    }
    ScopedFPDFAnnotation widget =
        NewWidget(document,
                  NewField(document, place == Place::kMark
                                         ? EPDF_FORMFIELD_FAMILY_SIGNATURE
                                         : EPDF_FORMFIELD_FAMILY_PUSHBUTTON),
                  page, rect, 0.0f);
    ASSERT_TRUE(widget);
    if (place == Place::kMark) {
      ASSERT_TRUE(
          EPDFAnnot_SetDrawing(widget.get(), drawing, EPDF_STAMP_FIT_CONTAIN));
    } else {
      SetIcon(widget.get(), drawing);
    }
  };

  for (Source source : kSources) {
    SCOPED_TRACE(NameOf(source));
    const std::string canonical = CanonicalBytes(source);
    ASSERT_FALSE(canonical.empty());
    for (const auto& first : kPlaces) {
      SCOPED_TRACE(first.name);
      std::string saved;
      {
        embedpdf_test::ScopedTestDocument document(NewTestDocument());
        ScopedFPDFPage page(FPDFPage_New(document.get(), 0, 400, 400));
        ASSERT_TRUE(page);
        show(document.get(), page.get(), first.place,
             AddDrawing(document.get(), source), {20, 80, 140, 20});
        saved = SavedBytes(document.get());
      }
      ASSERT_EQ(1, PicturesIn(saved, source));

      embedpdf_test::ScopedTestDocument reopened(
          embedpdf_test::LoadMemDocumentForTest(saved.data(), saved.size(),
                                                nullptr));
      ASSERT_TRUE(reopened);
      const std::vector<unsigned int> drawings = DrawingsOf(reopened.get());
      ASSERT_EQ(1u, drawings.size());
      {
        ScopedFPDFDocument exported(
            EPDFDoc_ExportDrawing(reopened.get(), drawings[0]));
        ASSERT_TRUE(exported);
        EXPECT_EQ(canonical, SavedBytes(exported.get()));
      }
      // The same picture again, in a place of every kind.
      {
        ScopedFPDFPage page(FPDF_LoadPage(reopened.get(), 0));
        ASSERT_TRUE(page);
        int row = 0;
        for (const auto& again : kPlaces) {
          const float bottom = 120 + 80 * row++;
          show(reopened.get(), page.get(), again.place, drawings[0],
               {20, bottom + 60, 140, bottom});
        }
        const Shown shown(page.get());
        for (int place = 0; place < 4; ++place) {
          // 120 by 60 in each box: red on the left, blue on the right.
          const int middle = (place == 0 ? 20 : 40 + 80 * place) + 30;
          EXPECT_EQ(kRed, shown.At(50, middle)) << place;
          EXPECT_EQ(kBlue, shown.At(110, middle)) << place;
        }
      }
      EXPECT_EQ(drawings, DrawingsOf(reopened.get()));
      for (const auto& save : kSaves) {
        SCOPED_TRACE(save.name);
        EXPECT_EQ(1,
                  PicturesIn(SavedBytes(reopened.get(), save.flags), source));
      }
    }
  }
}

// Replacing one mark and clearing one icon leaves the stamp, the mark and
// the icon that show the same drawing as they were. Once the last place
// that showed a drawing shows another, or none, a saved file no longer
// holds it.
TEST_F(EPDFWidgetDrawingEmbedderTest, ChangingOnePlaceLeavesTheOthers) {
  const unsigned int first = AddDrawing(doc_.get());
  const unsigned int second =
      AddDrawing(doc_.get(), Source::kVector, kGreen, kYellow);
  ASSERT_NE(0u, first);
  ASSERT_NE(0u, second);
  // Five boxes, 120 by 60, one above the other.
  auto box_at = [](int row) {
    const float bottom = 20 + 70 * row;
    return FS_RECTF{20, bottom + 60, 140, bottom};
  };
  ScopedFPDFAnnotation stamp =
      NewAnnot(page_.get(), FPDF_ANNOT_STAMP, box_at(0));
  ScopedFPDFAnnotation replaced_mark = NewSignatureWidget(box_at(1));
  ScopedFPDFAnnotation kept_mark = NewSignatureWidget(box_at(2));
  ScopedFPDFAnnotation cleared_icon = NewButton(box_at(3), 0.0f);
  ScopedFPDFAnnotation kept_icon = NewButton(box_at(4), 0.0f);
  for (FPDF_ANNOTATION placed :
       {stamp.get(), replaced_mark.get(), kept_mark.get()}) {
    ASSERT_TRUE(EPDFAnnot_SetDrawing(placed, first, EPDF_STAMP_FIT_CONTAIN));
  }
  SetIcon(cleared_icon.get(), first);
  SetIcon(kept_icon.get(), first);
  EXPECT_EQ(std::vector<unsigned int>({first}), DrawingsOf(doc_.get()));

  FPDF_ANNOTATION kept[] = {stamp.get(), kept_mark.get(), kept_icon.get()};
  std::vector<uint32_t> kept_appearances;
  for (FPDF_ANNOTATION annot : kept) {
    kept_appearances.push_back(AppearanceOf(annot)->GetObjNum());
  }
  const std::map<uint32_t, std::string> before = ObjectGraph(pdf());

  // One mark shows the second drawing; one icon is cleared.
  ASSERT_TRUE(EPDFAnnot_SetDrawing(replaced_mark.get(), second,
                                   EPDF_STAMP_FIT_CONTAIN));
  ASSERT_TRUE(EPDFAnnot_ClearMKIcon(cleared_icon.get(), EPDF_MK_ICON_I));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(cleared_icon.get()));

  const std::map<uint32_t, std::string> after = ObjectGraph(pdf());
  EXPECT_EQ(before.at(first), after.at(first));
  for (size_t i = 0; i < std::size(kept); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(kept_appearances[i], AppearanceOf(kept[i])->GetObjNum());
    EXPECT_EQ(before.at(kept_appearances[i]), after.at(kept_appearances[i]));
  }
  EXPECT_EQ(first, EPDFAnnot_GetDrawing(stamp.get()));
  EXPECT_EQ(first, EPDFAnnot_GetDrawing(kept_mark.get()));
  EXPECT_EQ(first, EPDFAnnot_GetMKIcon(kept_icon.get(), EPDF_MK_ICON_I));
  EXPECT_EQ(second, EPDFAnnot_GetDrawing(replaced_mark.get()));
  EXPECT_EQ(0u, EPDFAnnot_GetMKIcon(cleared_icon.get(), EPDF_MK_ICON_I));
  EXPECT_EQ(std::vector<unsigned int>({first, second}), DrawingsOf(doc_.get()));
  {
    const Shown shown(page_.get());
    const uint32_t kLeft[] = {kRed, kGreen, kRed, kWhite, kRed};
    const uint32_t kRight[] = {kBlue, kYellow, kBlue, kWhite, kBlue};
    for (int row = 0; row < 5; ++row) {
      SCOPED_TRACE(row);
      EXPECT_EQ(kLeft[row], shown.At(50, 50 + 70 * row));
      EXPECT_EQ(kRight[row], shown.At(110, 50 + 70 * row));
    }
  }
  for (const auto& save : kSaves) {
    SCOPED_TRACE(save.name);
    const std::string bytes = SavedBytes(doc_.get(), save.flags);
    EXPECT_EQ(1, StreamsHolding(bytes, kPictureMark));
    EXPECT_EQ(1, StreamsHolding(bytes, kSecondPictureMark));
  }

  // The last three places of the first drawing: the stamp and the icon show
  // the second, and the mark is cleared (a signature widget drawn again is
  // its box).
  ASSERT_TRUE(
      EPDFAnnot_SetDrawing(stamp.get(), second, EPDF_STAMP_FIT_CONTAIN));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(kept_mark.get()));
  SetIcon(kept_icon.get(), second);
  EXPECT_EQ(0u, EPDFAnnot_GetDrawing(kept_mark.get()));
  EXPECT_EQ(std::vector<unsigned int>({second}), DrawingsOf(doc_.get()));
  for (const auto& save : kSaves) {
    SCOPED_TRACE(save.name);
    const std::string bytes = SavedBytes(doc_.get(), save.flags);
    EXPECT_EQ(0, StreamsHolding(bytes, kPictureMark));
    EXPECT_EQ(1, StreamsHolding(bytes, kSecondPictureMark));
  }
}

// The document's list is the drawings its annotations show: none for a form
// of text fields, whose appearances draw no form at all.
TEST_F(EPDFWidgetDrawingEmbedderTest, AFormWithoutPicturesListsNoDrawing) {
  for (int row = 0; row < 3; ++row) {
    const uint32_t field = NewField(doc_.get(), EPDF_FORMFIELD_FAMILY_TEXT);
    ASSERT_GT(field, 0u);
    const float bottom = 20 + 40 * row;
    ScopedFPDFAnnotation widget = NewWidget(doc_.get(), field, page_.get(),
                                            {20, bottom + 30, 220, bottom});
    ASSERT_TRUE(widget);
    ASSERT_TRUE(embedpdf_test::FillTextField(doc_.get(), field, L"Hello"));
    EXPECT_EQ(0u, EPDFAnnot_GetDrawing(widget.get()));
  }
  // A drawing nothing shows is not listed either.
  ASSERT_NE(0u, AddDrawing(doc_.get()));
  EXPECT_TRUE(DrawingsOf(doc_.get()).empty());
  EXPECT_EQ(0u, EPDFDoc_GetDrawings(nullptr, nullptr, 0));
}

// Only a stamp and a widget place a drawing. Every other kind of annotation
// is refused, and reads as placing none, even when its appearance is a
// wrapper of ours; nothing is written, and on a layer nothing is copied up.
// The document lists what its stamps and widgets show, and nothing else.
TEST_F(EPDFWidgetDrawingEmbedderTest, OnlyAStampAndAWidgetPlaceADrawing) {
  const std::string bytes = EverySubtypePdf();
  embedpdf_test::LayerFixture every;
  ASSERT_TRUE(every.OpenBytes({bytes.begin(), bytes.end()}));
  const int others = static_cast<int>(std::size(kOtherSubtypes));
  {
    ScopedFPDFPage page(FPDF_LoadPage(every.layer, 0));
    ASSERT_TRUE(page);
    ASSERT_EQ(others + 2, FPDFPage_GetAnnotCount(page.get()));
    for (int i = 0; i < others; ++i) {
      SCOPED_TRACE(kOtherSubtypes[i]);
      ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), i));
      ASSERT_TRUE(annot);
      EXPECT_EQ(kOtherSubtypes[i], DictOf(annot.get())->GetNameFor("Subtype"));
      EXPECT_FALSE(EPDFAnnot_SetDrawing(annot.get(), kWrappedDrawing,
                                        EPDF_STAMP_FIT_CONTAIN));
      EXPECT_EQ(0u, EPDFAnnot_GetDrawing(annot.get()));
    }
    // The stamp and the widget read the drawing their wrapper places.
    for (int i = others; i < others + 2; ++i) {
      ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), i));
      ASSERT_TRUE(annot);
      EXPECT_EQ(kWrappedDrawing, EPDFAnnot_GetDrawing(annot.get())) << i;
    }
  }
  EXPECT_EQ(std::vector<unsigned int>({kWrappedDrawing}),
            DrawingsOf(every.layer));
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(every.layer));
  EXPECT_TRUE(every.SaveDelta().empty());

  // And they take one: each gets a wrapper of its own around it.
  ScopedFPDFPage page(FPDF_LoadPage(every.layer, 0));
  ASSERT_TRUE(page);
  for (int i = others; i < others + 2; ++i) {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), i));
    ASSERT_TRUE(annot);
    ASSERT_TRUE(EPDFAnnot_SetDrawing(annot.get(), kWrappedDrawing,
                                     EPDF_STAMP_FIT_CONTAIN))
        << i;
    EXPECT_EQ(kWrappedDrawing, EPDFAnnot_GetDrawing(annot.get())) << i;
    EXPECT_NE(5u, AppearanceOf(annot.get())->GetObjNum()) << i;
  }
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(every.layer, kWrappedDrawing));
}

// A mark placed the way the engine placed one before drawings served
// widgets (a page given to the widget, then fitted) is a wrapper around a
// drawing of its own. The file here was saved by the fork before this
// change, with the same picture marked into two signature widgets. Each
// mark reads as a drawing and the document lists both; both export as the
// picture's canonical bytes, so the engine knows them as one picture and
// places one of them from then on.
TEST_F(EPDFWidgetDrawingEmbedderTest, AMarkSavedBeforeThisChangeIsADrawing) {
  ASSERT_TRUE(OpenDocument("embedpdf_widget_marks_from_page.pdf"));
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(2, FPDFPage_GetAnnotCount(page));
  std::vector<unsigned int> marks;
  for (int i = 0; i < 2; ++i) {
    ScopedFPDFAnnotation widget(FPDFPage_GetAnnot(page, i));
    ASSERT_TRUE(widget);
    ASSERT_EQ(FPDF_ANNOT_WIDGET, FPDFAnnot_GetSubtype(widget.get()));
    marks.push_back(EPDFAnnot_GetDrawing(widget.get()));
    EXPECT_NE(0u, marks.back());
  }
  // A drawing each, as each call made one.
  EXPECT_NE(marks[0], marks[1]);
  EXPECT_EQ(marks, DrawingsOf(document()));
  const std::string canonical = CanonicalBytes(Source::kVector);
  for (unsigned int mark : marks) {
    ScopedFPDFDocument exported(EPDFDoc_ExportDrawing(document(), mark));
    ASSERT_TRUE(exported);
    EXPECT_EQ(canonical, SavedBytes(exported.get())) << mark;
  }
  {
    const Shown shown(page);
    EXPECT_EQ(kRed, shown.At(50, 50));
    EXPECT_EQ(kBlue, shown.At(110, 50));
    EXPECT_EQ(kRed, shown.At(50, 150));
    EXPECT_EQ(kBlue, shown.At(110, 150));
  }
  EXPECT_EQ(2, PicturesIn(SavedBytes(document()), Source::kVector));

  // The picture again: the first drawing is placed, in a new signature
  // widget and as a new image field's icon. No third copy.
  {
    const uint32_t field =
        NewField(document(), EPDF_FORMFIELD_FAMILY_SIGNATURE);
    ScopedFPDFAnnotation marked =
        NewWidget(document(), field, page, {20, 280, 140, 220});
    ASSERT_TRUE(marked);
    ASSERT_TRUE(
        EPDFAnnot_SetDrawing(marked.get(), marks[0], EPDF_STAMP_FIT_CONTAIN));
    const uint32_t button =
        NewField(document(), EPDF_FORMFIELD_FAMILY_PUSHBUTTON);
    ScopedFPDFAnnotation image =
        NewWidget(document(), button, page, {20, 380, 140, 320}, 0.0f);
    ASSERT_TRUE(image);
    SetIcon(image.get(), marks[0]);
  }
  EXPECT_EQ(marks, DrawingsOf(document()));
  EXPECT_EQ(2, PicturesIn(SavedBytes(document()), Source::kVector));
  {
    const Shown shown(page);
    EXPECT_EQ(kRed, shown.At(50, 250));
    EXPECT_EQ(kBlue, shown.At(110, 250));
    EXPECT_EQ(kRed, shown.At(50, 350));
    EXPECT_EQ(kBlue, shown.At(110, 350));
  }

  // The second mark placed again from the first drawing: one picture is
  // left in a full save. An incremental save keeps the file it appends to.
  {
    ScopedFPDFAnnotation second(FPDFPage_GetAnnot(page, 1));
    ASSERT_TRUE(second);
    ASSERT_TRUE(
        EPDFAnnot_SetDrawing(second.get(), marks[0], EPDF_STAMP_FIT_CONTAIN));
  }
  EXPECT_EQ(std::vector<unsigned int>({marks[0]}), DrawingsOf(document()));
  EXPECT_EQ(1, PicturesIn(SavedBytes(document()), Source::kVector));
  EXPECT_EQ(
      2, PicturesIn(SavedBytes(document(), FPDF_INCREMENTAL), Source::kVector));
  UnloadPage(page);
}

// Listing a document's drawings reads the content of the appearances that
// draw a form, and of no other: a text field's appearance names no XObject,
// so nothing is decoded for it, however many such widgets a form has.
TEST_F(EPDFWidgetDrawingEmbedderTest, ListingReadsNoAppearanceThatDrawsNoForm) {
  // Gives `widget` an appearance whose data counts its fetches.
  auto count_appearance = [this](FPDF_ANNOTATION widget,
                                 const std::string& content,
                                 RetainPtr<CPDF_Dictionary> resources) {
    RetainPtr<CountedData> data = pdfium::MakeRetain<CountedData>(content);
    auto dict = pdfium::MakeRetain<CPDF_Dictionary>();
    dict->SetNewFor<CPDF_Name>("Type", "XObject");
    dict->SetNewFor<CPDF_Name>("Subtype", "Form");
    dict->SetRectFor("BBox", DictOf(widget)->GetRectFor("Rect"));
    dict->SetFor("Resources", std::move(resources));
    auto stream = pdf()->NewIndirect<CPDF_Stream>(data, std::move(dict));
    MutableDictOf(widget)->GetOrCreateDictFor("AP")->SetNewFor<CPDF_Reference>(
        "N", pdf(), stream->GetObjNum());
    return data;
  };

  // A text field with three widgets, as a form has them: each appearance
  // draws text in a font and names no XObject.
  const uint32_t text = NewField(doc_.get(), EPDF_FORMFIELD_FAMILY_TEXT);
  ASSERT_GT(text, 0u);
  std::vector<RetainPtr<CountedData>> text_appearances;
  for (int row = 0; row < 3; ++row) {
    const float bottom = 20 + 40 * row;
    ScopedFPDFAnnotation widget = NewWidget(doc_.get(), text, page_.get(),
                                            {20, bottom + 30, 220, bottom});
    ASSERT_TRUE(widget);
    auto resources = pdfium::MakeRetain<CPDF_Dictionary>();
    resources->SetNewFor<CPDF_Dictionary>("Font");
    text_appearances.push_back(count_appearance(
        widget.get(), "/Tx BMC BT /Helv 12 Tf 2 10 Td (Hello) Tj ET EMC",
        std::move(resources)));
  }
  // An image field: its appearance draws its icon, a form.
  const unsigned int drawing = AddDrawing(doc_.get());
  ScopedFPDFAnnotation button = NewButton({20, 300, 122, 258});
  ASSERT_TRUE(button);
  ASSERT_TRUE(EPDFAnnot_SetMKIcon(button.get(), EPDF_MK_ICON_I, drawing));
  auto resources = pdfium::MakeRetain<CPDF_Dictionary>();
  resources->SetNewFor<CPDF_Dictionary>("XObject")->SetNewFor<CPDF_Reference>(
      "FRM", pdf(), drawing);
  RetainPtr<CountedData> icon_appearance = count_appearance(
      button.get(), "q 1 1 100 40 re W n q .4 0 0 .4 11 1 cm /FRM Do Q Q",
      std::move(resources));

  EXPECT_EQ(std::vector<unsigned int>({drawing}), DrawingsOf(doc_.get()));
  for (const RetainPtr<CountedData>& appearance : text_appearances) {
    EXPECT_EQ(0, appearance->fetches());
  }
  // The push button's appearance draws a form, so its content is read to
  // see whether it is a wrapper. It is not: what is listed is its icon.
  EXPECT_GT(icon_appearance->fetches(), 0);
}

// Listing writes nothing: on a layer, outside a transaction and inside one,
// no object is copied up and the layer has no change to save.
TEST_F(EPDFWidgetDrawingEmbedderTest, ListingDrawingsWritesNothing) {
  // A file with a stamp, a mark and an icon that share one drawing.
  std::string saved;
  {
    const unsigned int drawing = AddDrawing(doc_.get());
    ScopedFPDFAnnotation stamp =
        NewAnnot(page_.get(), FPDF_ANNOT_STAMP, {20, 80, 140, 20});
    ASSERT_TRUE(
        EPDFAnnot_SetDrawing(stamp.get(), drawing, EPDF_STAMP_FIT_CONTAIN));
    ScopedFPDFAnnotation marked = NewSignatureWidget({20, 180, 140, 120});
    ASSERT_TRUE(
        EPDFAnnot_SetDrawing(marked.get(), drawing, EPDF_STAMP_FIT_CONTAIN));
    ScopedFPDFAnnotation button = NewButton({20, 280, 140, 220}, 0.0f);
    SetIcon(button.get(), drawing);
    stamp.reset();
    marked.reset();
    button.reset();
    saved = SavedBytes(doc_.get());
  }
  embedpdf_test::LayerFixture file;
  ASSERT_TRUE(file.OpenBytes({saved.begin(), saved.end()}));
  EXPECT_EQ(1u, DrawingsOf(file.layer).size());
  ASSERT_TRUE(EPDFLayer_BeginTransaction(file.layer));
  EXPECT_EQ(1u, DrawingsOf(file.layer).size());
  ASSERT_TRUE(EPDFLayer_CommitTransaction(file.layer));
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(file.layer));
  EXPECT_TRUE(file.SaveDelta().empty());
}

// In the layouts that show a caption and an icon, a button that has both
// draws both, each in its part of the button inside the border: the caption
// in a strip as high as its line below or above the icon, or as wide as its
// text right or left of it, and the icon fitted into the rest. Over the icon
// both take all of it, and the caption is drawn last, on top. /MK /I still
// names the drawing, and the appearance draws that same form.
TEST_F(EPDFWidgetDrawingEmbedderTest, ACaptionAndAnIconEachTakeTheirPart) {
  // 204 by 104 with a border of 2: 200 by 100 inside it, from (2, 2).
  const FS_RECTF kStands{100, 254, 304, 150};
  ScopedFPDFAnnotation button = NewButton(kStands, 2.0f);
  ASSERT_TRUE(button);
  ASSERT_TRUE(EPDFAnnot_SetMKColor(button.get(), EPDF_MK_COLOR_BC, 0, 255, 0));
  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_NE(0u, drawing);
  SetCaptionAndIcon(button.get(), drawing, kCaptionBelow);
  const CFX_SizeF room = CaptionRoomOf(button.get());
  ASSERT_GT(room.width, 0);
  ASSERT_GT(room.height, 0);

  const float low = 2 + room.height;
  const float high = 102 - room.height;
  const float near = 2 + room.width;
  const float far = 202 - room.width;
  // The picture is twice as wide as high, and stands in the middle of its
  // part. Above or below the caption's strip it is as high as the room left,
  // so it is the strip's height in from each side. Beside the caption's
  // column it is as wide as the room left, so it is a quarter of the
  // column's width in from the bottom and from the top.
  const float in_x = room.height;
  const float in_y = room.width / 4;
  const struct {
    CFX_FloatRect caption;
    CFX_FloatRect icon;
    CFX_FloatRect picture;
  } kExpected[] = {
      {CFX_FloatRect(2, 2, 202, low), CFX_FloatRect(2, low, 202, 102),
       CFX_FloatRect(2 + in_x, low, 202 - in_x, 102)},
      {CFX_FloatRect(2, high, 202, 102), CFX_FloatRect(2, 2, 202, high),
       CFX_FloatRect(2 + in_x, 2, 202 - in_x, high)},
      {CFX_FloatRect(far, 2, 202, 102), CFX_FloatRect(2, 2, far, 102),
       CFX_FloatRect(2, 2 + in_y, far, 102 - in_y)},
      {CFX_FloatRect(2, 2, near, 102), CFX_FloatRect(near, 2, 202, 102),
       CFX_FloatRect(near, 2 + in_y, 202, 102 - in_y)},
      {CFX_FloatRect(2, 2, 202, 102), CFX_FloatRect(2, 2, 202, 102),
       CFX_FloatRect(2, 2, 202, 102)},
  };
  int index = 0;
  for (const auto& layout : kCaptionAndIconLayouts) {
    SCOPED_TRACE(layout.name);
    const auto& expected = kExpected[index++];
    SetCaptionAndIcon(button.get(), drawing, layout.layout);
    EXPECT_EQ(drawing, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
    EXPECT_EQ(drawing, DrawnIconNumber(button.get()));

    const std::optional<DrawnIcon> icon = DrawnIconOf(button.get());
    ASSERT_TRUE(icon);
    ExpectRectNear(expected.icon, icon->plate, 1e-2f);
    ExpectRectNear(expected.picture, icon->picture, 1e-2f);
    const std::optional<CFX_FloatRect> caption = DrawnCaptionOf(button.get());
    ASSERT_TRUE(caption);
    ExpectRectNear(expected.caption, *caption, 1e-2f);
    // The icon first, the caption after it.
    const std::string content = StreamBytes(AppearanceOf(button.get()).Get());
    EXPECT_LT(content.find("/FRM Do"), content.find("BT\n"));

    // Shown: the picture where it lands, red at its left, and the caption in
    // the middle of its part and nowhere beside it.
    const Shown shown(page_.get(), 4);
    const CFX_PointF red = RedPointOf(expected.picture);
    const CFX_PointF blue = BluePointOf(expected.picture);
    EXPECT_EQ(kRed, shown.At(100 + red.x, 150 + red.y));
    EXPECT_EQ(kBlue, shown.At(100 + blue.x, 150 + blue.y));
    CFX_FloatRect ink = CaptionInkOf(expected.caption);
    ink.Translate(100, 150);
    EXPECT_TRUE(shown.HasInkIn(ink));
    if (layout.layout != kCaptionOver) {
      CFX_FloatRect beside = expected.icon;
      beside.Translate(100, 150);
      EXPECT_FALSE(shown.HasInkIn(beside));
    }
  }
}

// The caption and the icon share the button less the border's width at each
// edge, the width /BS gives whether or not a colour paints it. /MK /IF /FB
// shares the whole button, as it gives an icon alone the whole button.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       ACaptionAndAnIconShareWhatTheBorderLeaves) {
  const FS_RECTF kStands{100, 308, 308, 200};  // 208 by 108
  const struct {
    const char* name;
    std::optional<float> border_width;
    bool fit_to_bounds;
    CFX_FloatRect plate;
  } kCases[] = {
      // No /BS: a border of 1.
      {"no /BS", {}, false, CFX_FloatRect(1, 1, 207, 107)},
      {"no border", 0.0f, false, CFX_FloatRect(0, 0, 208, 108)},
      {"a border of 4", 4.0f, false, CFX_FloatRect(4, 4, 204, 104)},
      {"a border of 4, fitted to the bounds", 4.0f, true,
       CFX_FloatRect(0, 0, 208, 108)},
  };
  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_NE(0u, drawing);
  for (const auto& expected : kCases) {
    SCOPED_TRACE(expected.name);
    for (const auto& layout : kCaptionAndIconLayouts) {
      SCOPED_TRACE(layout.name);
      ScopedFPDFAnnotation button = NewButton(kStands, expected.border_width);
      ASSERT_TRUE(button);
      if (expected.fit_to_bounds) {
        MutableDictOf(button.get())
            ->GetOrCreateDictFor("MK")
            ->SetNewFor<CPDF_Dictionary>("IF")
            ->SetNewFor<CPDF_Boolean>("FB", true);
      }
      SetCaptionAndIcon(button.get(), drawing, layout.layout);

      const std::optional<DrawnIcon> icon = DrawnIconOf(button.get());
      ASSERT_TRUE(icon);
      const std::optional<CFX_FloatRect> caption = DrawnCaptionOf(button.get());
      ASSERT_TRUE(caption);
      ExpectPartsShare(layout.layout, expected.plate,
                       CaptionRoomOf(button.get()), *caption, icon->plate);
    }
  }
}

// Beside a caption the icon is fitted into its own part as the icon fit
// dictionary says (ISO 32000-2 Table 250), as an icon alone is fitted into
// the whole button.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       AnIconBesideACaptionIsFittedAsTheIconFitSays) {
  enum class Lands {
    kInTheMiddle,      // the table's defaults
    kFillingItsPart,   // /S /A: without its proportions
    kAtTheLowerLeft,   // /A [0 0]
    kAtTheUpperRight,  // /A [1 1]
    kAtItsOwnSize,     // /SW /N: never scaled
  };
  const struct {
    const char* name;
    Lands lands;
  } kFits[] = {{"no /IF", Lands::kInTheMiddle},
               {"without its proportions", Lands::kFillingItsPart},
               {"at the lower left", Lands::kAtTheLowerLeft},
               {"at the upper right", Lands::kAtTheUpperRight},
               {"never scaled", Lands::kAtItsOwnSize}};
  constexpr float kWithin = 1e-2f;
  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_NE(0u, drawing);
  for (const auto& fit : kFits) {
    SCOPED_TRACE(fit.name);
    for (const auto& layout : kCaptionAndIconLayouts) {
      SCOPED_TRACE(layout.name);
      // 300 by 200, without a border.
      ScopedFPDFAnnotation button = NewButton({50, 350, 350, 150}, 0.0f);
      ASSERT_TRUE(button);
      if (fit.lands != Lands::kInTheMiddle) {
        RetainPtr<CPDF_Dictionary> written =
            MutableDictOf(button.get())
                ->GetOrCreateDictFor("MK")
                ->SetNewFor<CPDF_Dictionary>("IF");
        if (fit.lands == Lands::kFillingItsPart) {
          written->SetNewFor<CPDF_Name>("S", "A");
        } else if (fit.lands == Lands::kAtItsOwnSize) {
          written->SetNewFor<CPDF_Name>("SW", "N");
        } else {
          const float share = fit.lands == Lands::kAtTheLowerLeft ? 0 : 1;
          RetainPtr<CPDF_Array> shares = written->SetNewFor<CPDF_Array>("A");
          shares->AppendNew<CPDF_Number>(share);
          shares->AppendNew<CPDF_Number>(share);
        }
      }
      SetCaptionAndIcon(button.get(), drawing, layout.layout);

      const std::optional<DrawnIcon> icon = DrawnIconOf(button.get());
      ASSERT_TRUE(icon);
      const CFX_FloatRect& part = icon->plate;
      const CFX_FloatRect& picture = icon->picture;
      if (fit.lands == Lands::kFillingItsPart) {
        ExpectRectNear(part, picture, kWithin);
        continue;
      }
      if (fit.lands == Lands::kAtItsOwnSize) {
        EXPECT_NEAR(200, picture.Width(), kWithin);
        EXPECT_NEAR(100, picture.Height(), kWithin);
      } else {
        // Its proportions kept, and as large as its part lets it be: it
        // reaches across the part or up it.
        EXPECT_NEAR(picture.Width(), 2 * picture.Height(), kWithin);
        EXPECT_NEAR(1,
                    std::max(picture.Width() / part.Width(),
                             picture.Height() / part.Height()),
                    1e-4f);
      }
      switch (fit.lands) {
        case Lands::kAtTheLowerLeft:
          EXPECT_NEAR(part.left, picture.left, kWithin);
          EXPECT_NEAR(part.bottom, picture.bottom, kWithin);
          break;
        case Lands::kAtTheUpperRight:
          EXPECT_NEAR(part.right, picture.right, kWithin);
          EXPECT_NEAR(part.top, picture.top, kWithin);
          break;
        default:
          EXPECT_NEAR(part.left + part.right, picture.left + picture.right,
                      kWithin);
          EXPECT_NEAR(part.bottom + part.top, picture.bottom + picture.top,
                      kWithin);
          break;
      }
    }
  }
}

// A turned button's caption and icon are laid out in the frame its quarter
// turn gives, and turn with it: the caption below the icon is at the
// reader's right of a button turned counterclockwise.
TEST_F(EPDFWidgetDrawingEmbedderTest, ACaptionAndAnIconTurnWithTheirButton) {
  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_NE(0u, drawing);
  int page_index = 0;
  for (const auto& layout : kCaptionAndIconLayouts) {
    SCOPED_TRACE(layout.name);
    ScopedFPDFPage page(FPDFPage_New(doc_.get(), ++page_index, 400, 400));
    ASSERT_TRUE(page);
    struct Drawn {
      FS_RECTF stands;
      CFX_Matrix turn;
      DrawnIcon icon;
      CFX_FloatRect caption;
    };
    std::vector<Drawn> buttons;
    for (const Turn& expected : kTurns) {
      SCOPED_TRACE(expected.written.value_or(0));
      const FS_RECTF stands =
          TurnedBoxAt(static_cast<int>(buttons.size()), 200);
      ScopedFPDFAnnotation button = NewButton(stands, 0.0f, page.get());
      ASSERT_TRUE(button);
      WriteTurn(button.get(), expected);
      SetCaptionAndIcon(button.get(), drawing, layout.layout);

      RetainPtr<const CPDF_Stream> appearance = AppearanceOf(button.get());
      ASSERT_TRUE(appearance);
      EXPECT_EQ(expected.frame, appearance->GetDict()->GetRectFor("BBox"));
      EXPECT_EQ(expected.turn, appearance->GetDict()->GetMatrixFor("Matrix"));
      const std::optional<DrawnIcon> icon = DrawnIconOf(button.get());
      ASSERT_TRUE(icon);
      const std::optional<CFX_FloatRect> caption = DrawnCaptionOf(button.get());
      ASSERT_TRUE(caption);
      ExpectPartsShare(layout.layout, expected.frame,
                       CaptionRoomOf(button.get()), *caption, icon->plate);
      buttons.push_back({stands, expected.turn, *icon, *caption});
    }

    // Shown, a point of the frame is turned, then moved to where the button
    // stands.
    const Shown shown(page.get(), 4);
    for (const Drawn& button : buttons) {
      auto on_page = [&button](const CFX_PointF& point) {
        const CFX_PointF turned = button.turn.Transform(point);
        return CFX_PointF(button.stands.left + turned.x, 200 + turned.y);
      };
      const CFX_PointF red = on_page(RedPointOf(button.icon.picture));
      const CFX_PointF blue = on_page(BluePointOf(button.icon.picture));
      EXPECT_EQ(kRed, shown.At(red.x, red.y));
      EXPECT_EQ(kBlue, shown.At(blue.x, blue.y));
      CFX_FloatRect ink =
          button.turn.TransformRect(CaptionInkOf(button.caption));
      ink.Translate(button.stands.left, 200);
      EXPECT_TRUE(shown.HasInkIn(ink));
      if (layout.layout != kCaptionOver) {
        CFX_FloatRect beside = button.turn.TransformRect(button.icon.plate);
        beside.Translate(button.stands.left, 200);
        EXPECT_FALSE(shown.HasInkIn(beside));
      }
    }
  }
}

// On a page turned a quarter turn, buttons turned the other way read upright
// in every layout: the picture's left is at the reader's left, and the
// caption is where the layout puts it for the reader.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       ACaptionAndAnIconReadUprightOnATurnedPage) {
  // The page is shown turned 90 clockwise. Each button stands 52 wide and
  // 102 tall on it, 60 from the one before, and is turned 90
  // counterclockwise, so the reader sees it 102 wide and 52 tall: 100 by 50
  // inside its border of 1.
  FPDFPage_SetRotation(page_.get(), 1);
  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_NE(0u, drawing);
  CFX_SizeF room;
  int index = 0;
  for (const auto& layout : kCaptionAndIconLayouts) {
    const float left = 100 + 60 * index++;
    ScopedFPDFAnnotation button = NewButton({left, 202, left + 52, 100});
    ASSERT_TRUE(button);
    ASSERT_TRUE(EPDFAnnot_SetMKRotation(button.get(), 90));
    ASSERT_TRUE(
        EPDFAnnot_SetMKColor(button.get(), EPDF_MK_COLOR_BC, 0, 255, 0));
    SetCaptionAndIcon(button.get(), drawing, layout.layout);
    room = CaptionRoomOf(button.get());
  }
  const std::string bytes = SavedBytes(doc_.get());
  ASSERT_FALSE(bytes.empty());

  embedpdf_test::ScopedTestDocument reopened(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  ASSERT_TRUE(reopened);
  ScopedFPDFPage page(FPDF_LoadPage(reopened.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_EQ(5, FPDFPage_GetAnnotCount(page.get()));
  const Shown shown(page.get(), 4);
  index = 0;
  for (const auto& layout : kCaptionAndIconLayouts) {
    SCOPED_TRACE(layout.name);
    ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), index));
    ASSERT_TRUE(button);
    const std::optional<DrawnIcon> icon = DrawnIconOf(button.get());
    ASSERT_TRUE(icon);
    const std::optional<CFX_FloatRect> caption = DrawnCaptionOf(button.get());
    ASSERT_TRUE(caption);
    ExpectPartsShare(layout.layout, CFX_FloatRect(1, 1, 101, 51), room,
                     *caption, icon->plate);
    // Shown, a page point (x, y) is at (y, x) from the top left. This button
    // spans 100 to 202 across, and 100 + 60 * index to 152 + 60 * index
    // down: its frame's lower left corner is 248 - 60 * index up from the
    // bottom.
    const float bottom = 248 - 60 * index++;
    const CFX_PointF red = RedPointOf(icon->picture);
    const CFX_PointF blue = BluePointOf(icon->picture);
    EXPECT_EQ(kRed, shown.At(100 + red.x, bottom + red.y));
    EXPECT_EQ(kBlue, shown.At(100 + blue.x, bottom + blue.y));
    EXPECT_LT(red.x, blue.x);
    CFX_FloatRect ink = CaptionInkOf(*caption);
    ink.Translate(100, bottom);
    EXPECT_TRUE(shown.HasInkIn(ink));
    if (layout.layout != kCaptionOver) {
      CFX_FloatRect beside = icon->plate;
      beside.Translate(100, bottom);
      EXPECT_FALSE(shown.HasInkIn(beside));
    }
  }
}

// Without an icon a button draws its caption alone in every layout that
// shows a caption, exactly as the layout that shows the caption alone does.
// And that layout shows no icon, though the button has one. Each time the
// appearance is a new stream, and the one before is left as it was.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       AButtonWithoutAnIconDrawsItsCaptionAlone) {
  ScopedFPDFAnnotation button = NewButton({100, 254, 304, 150}, 2.0f);
  ASSERT_TRUE(button);
  ASSERT_TRUE(EPDFAnnot_SetMKText(button.get(), EPDF_MK_TEXT_CA,
                                  GetFPDFWideString(L"Go").get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(button.get(), FPDF_FONT_HELVETICA,
                                             12, 0, 0, 0));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
  uint32_t appearance = AppearanceOf(button.get())->GetObjNum();
  const std::string alone = StreamBytes(AppearanceOf(button.get()).Get());
  ASSERT_NE(std::string::npos, alone.find("BT\n"));
  // Drawn again, the button has a new stream that holds the same, and the
  // one it had is untouched.
  auto expect_drawn_alone_again = [&]() {
    const std::string before = ObjectGraph(pdf())[appearance];
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
    EXPECT_NE(appearance, AppearanceOf(button.get())->GetObjNum());
    EXPECT_EQ(before, ObjectGraph(pdf())[appearance]);
    EXPECT_EQ(alone, StreamBytes(AppearanceOf(button.get()).Get()));
    EXPECT_EQ(0u, DrawnIconNumber(button.get()));
    appearance = AppearanceOf(button.get())->GetObjNum();
  };
  for (const auto& layout : kCaptionAndIconLayouts) {
    SCOPED_TRACE(layout.name);
    ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(button.get(), layout.layout));
    expect_drawn_alone_again();
  }

  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_TRUE(EPDFAnnot_SetMKIcon(button.get(), EPDF_MK_ICON_I, drawing));
  ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(button.get(), 0));
  expect_drawn_alone_again();
  EXPECT_EQ(drawing, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
}

// A button that has an icon and no caption draws the icon in the whole of
// the button inside its border, in every layout that shows one, as the
// layout that shows the icon alone does. Nothing is kept free for a caption
// that isn't there, and no text is drawn, so it needs no /DA and no font.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       AnIconWithoutACaptionTakesTheWholeButton) {
  ScopedFPDFAnnotation button = NewButton({100, 254, 304, 150}, 2.0f);
  ASSERT_TRUE(button);
  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_NE(0u, drawing);
  SetIcon(button.get(), drawing);
  const std::string alone = StreamBytes(AppearanceOf(button.get()).Get());
  ASSERT_NE(std::string::npos, alone.find("/FRM Do"));
  for (bool empty_caption : {false, true}) {
    SCOPED_TRACE(empty_caption ? "an empty caption" : "no caption");
    if (empty_caption) {
      ASSERT_TRUE(EPDFAnnot_SetMKText(button.get(), EPDF_MK_TEXT_CA,
                                      GetFPDFWideString(L"").get()));
    }
    for (const auto& layout : kCaptionAndIconLayouts) {
      SCOPED_TRACE(layout.name);
      ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(button.get(), layout.layout));
      ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
      EXPECT_EQ(alone, StreamBytes(AppearanceOf(button.get()).Get()));
      EXPECT_EQ(drawing, DrawnIconNumber(button.get()));
      EXPECT_FALSE(DictOf(button.get())->KeyExist("DA"));
      const Shown shown(page_.get());
      EXPECT_EQ(kRed, shown.At(152, 202));
      EXPECT_EQ(kBlue, shown.At(252, 202));
    }
  }
}

// A caption needs a /DA: without one no field that shows text is drawn. A
// button with a caption and an icon is then left as it was, the other app's
// appearance included, and not drawn with its caption missing. Nothing is
// written, and the picture is still shown and still named.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       AButtonWhoseCaptionCannotBeDrawnIsLeftAsItWas) {
  const std::string bytes = ForeignButtonsPdf(/*with_fonts=*/false);
  embedpdf_test::ScopedTestDocument foreign(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  ASSERT_TRUE(foreign);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(foreign.get());
  ScopedFPDFPage page(FPDF_LoadPage(foreign.get(), 0));
  ASSERT_TRUE(page);
  const std::map<uint32_t, std::string> before = ObjectGraph(doc);
  for (const auto& layout : kCaptionAndIconLayouts) {
    SCOPED_TRACE(layout.name);
    ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), layout.layout));
    ASSERT_TRUE(button);
    const uint32_t appearance = AppearanceOf(button.get())->GetObjNum();
    EPDFAnnot_GenerateFormFieldAP(button.get());
    EXPECT_EQ(appearance, AppearanceOf(button.get())->GetObjNum());
    EXPECT_EQ(kForeignIcon, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
    // As the other app drew it: 80 by 40 in the middle, inside a border of 1.
    const int bottom = 10 + 50 * layout.layout;
    const Shown shown(page.get());
    EXPECT_EQ(kRed, shown.At(51, bottom + 21));
    EXPECT_EQ(kBlue, shown.At(91, bottom + 21));
  }
  EXPECT_EQ(before, ObjectGraph(doc));
}

// A /DA that names a font the form doesn't have is drawn in a fallback font,
// as every field's text is: the caption and the icon are both drawn.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       ACaptionWhoseFontIsMissingIsStillDrawnBesideItsIcon) {
  const std::string bytes = ForeignButtonsPdf();
  embedpdf_test::ScopedTestDocument foreign(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  ASSERT_TRUE(foreign);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(foreign.get());
  ASSERT_TRUE(
      doc->GetMutableRoot()->GetMutableDictFor("AcroForm")->RemoveFor("DR"));
  ScopedFPDFPage page(FPDF_LoadPage(foreign.get(), 0));
  ASSERT_TRUE(page);
  for (const auto& layout : kCaptionAndIconLayouts) {
    SCOPED_TRACE(layout.name);
    ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), layout.layout));
    ASSERT_TRUE(button);
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
    EXPECT_EQ(kForeignIcon, DrawnIconNumber(button.get()));
    const std::optional<DrawnIcon> icon = DrawnIconOf(button.get());
    ASSERT_TRUE(icon);
    const std::optional<CFX_FloatRect> caption = DrawnCaptionOf(button.get());
    ASSERT_TRUE(caption);
    ExpectPartsShare(layout.layout, CFX_FloatRect(1, 1, 101, 41),
                     CaptionRoomOf(button.get()), *caption, icon->plate);
    const float bottom = 10 + 50 * layout.layout;
    const Shown shown(page.get(), 4);
    const CFX_PointF red = RedPointOf(icon->picture);
    const CFX_PointF blue = BluePointOf(icon->picture);
    EXPECT_EQ(kRed, shown.At(20 + red.x, bottom + red.y));
    EXPECT_EQ(kBlue, shown.At(20 + blue.x, bottom + blue.y));
    CFX_FloatRect ink = CaptionInkOf(*caption);
    ink.Translate(20, bottom);
    EXPECT_TRUE(shown.HasInkIn(ink));
  }
}

// A caption that needs more than the button has takes all of it, and the
// icon is not drawn: the rule of PDFium's form filler. The button is then
// drawn as the layout that shows the caption alone draws it. /MK /I still
// names the picture. Over the icon nothing is shared, so both are drawn.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       ACaptionTooLargeForTheButtonIsDrawnAlone) {
  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_NE(0u, drawing);
  const struct {
    const char* name;
    FS_RECTF stands;
    float font_size;
    int layouts[2];
  } kCases[] = {
      // 100 by 40 inside the border. A line at 48 is higher than 40.
      {"too high", {100, 244, 204, 200}, 48, {kCaptionBelow, kCaptionAbove}},
      // 14 by 100 inside the border. "Go" at 12 is 16 wide.
      {"too wide", {100, 304, 118, 200}, 12, {kCaptionRight, kCaptionLeft}},
  };
  for (const auto& expected : kCases) {
    SCOPED_TRACE(expected.name);
    ScopedFPDFAnnotation button = NewButton(expected.stands, 2.0f);
    ASSERT_TRUE(button);
    SetCaptionAndIcon(button.get(), drawing, 0, expected.font_size);
    const std::string alone = StreamBytes(AppearanceOf(button.get()).Get());
    ASSERT_NE(std::string::npos, alone.find("BT\n"));
    for (int layout : expected.layouts) {
      SCOPED_TRACE(layout);
      SetCaptionAndIcon(button.get(), drawing, layout, expected.font_size);
      EXPECT_EQ(alone, StreamBytes(AppearanceOf(button.get()).Get()));
      EXPECT_EQ(0u, DrawnIconNumber(button.get()));
      EXPECT_EQ(drawing, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
    }
    SetCaptionAndIcon(button.get(), drawing, kCaptionOver, expected.font_size);
    EXPECT_EQ(drawing, DrawnIconNumber(button.get()));
    EXPECT_TRUE(DrawnCaptionOf(button.get()));
  }
}

// A caption whose size is fitted to its room (a font size of 0) takes a
// third of the button, of its height below or above the icon and of its
// width beside it, and is fitted to that third: the rule of PDFium's form
// filler.
TEST_F(EPDFWidgetDrawingEmbedderTest, AFittedCaptionTakesAThirdOfTheButton) {
  // 204 by 104 with a border of 2: 200 by 100 inside it, from (2, 2).
  ScopedFPDFAnnotation button = NewButton({100, 254, 304, 150}, 2.0f);
  ASSERT_TRUE(button);
  const unsigned int drawing = AddDrawing(doc_.get());
  ASSERT_NE(0u, drawing);
  const float third_up = 2 + 100.0f / 3;
  const float third_down = 102 - 100.0f / 3;
  const float third_along = 2 + 200.0f / 3;
  const float third_back = 202 - 200.0f / 3;
  const struct {
    CFX_FloatRect caption;
    CFX_FloatRect icon;
  } kExpected[] = {
      {CFX_FloatRect(2, 2, 202, third_up),
       CFX_FloatRect(2, third_up, 202, 102)},
      {CFX_FloatRect(2, third_down, 202, 102),
       CFX_FloatRect(2, 2, 202, third_down)},
      {CFX_FloatRect(third_back, 2, 202, 102),
       CFX_FloatRect(2, 2, third_back, 102)},
      {CFX_FloatRect(2, 2, third_along, 102),
       CFX_FloatRect(third_along, 2, 202, 102)},
      {CFX_FloatRect(2, 2, 202, 102), CFX_FloatRect(2, 2, 202, 102)},
  };
  int index = 0;
  for (const auto& layout : kCaptionAndIconLayouts) {
    SCOPED_TRACE(layout.name);
    const auto& expected = kExpected[index++];
    SetCaptionAndIcon(button.get(), drawing, layout.layout, 0);
    EXPECT_EQ(drawing, DrawnIconNumber(button.get()));
    const std::optional<DrawnIcon> icon = DrawnIconOf(button.get());
    ASSERT_TRUE(icon);
    ExpectRectNear(expected.icon, icon->plate, 1e-2f);
    const std::optional<CFX_FloatRect> caption = DrawnCaptionOf(button.get());
    ASSERT_TRUE(caption);
    ExpectRectNear(expected.caption, *caption, 1e-2f);
    // The size the caption is written at fits its part.
    const float size = DrawnCaptionSizeOf(button.get());
    EXPECT_GT(size, 0);
    const CFX_SizeF room = CaptionRoomOf(button.get(), size);
    EXPECT_LE(room.width, caption->Width());
    EXPECT_LE(room.height, caption->Height());
    const Shown shown(page_.get(), 4);
    CFX_FloatRect part = *caption;
    part.Translate(100, 150);
    EXPECT_TRUE(shown.HasInkIn(part));
  }
}

// The generator can draw a button that shows a caption and an icon in
// memory: a stream of no document that draws the file's icon and names a
// font for the caption, and nothing is written. No render asks for it yet.
TEST_F(EPDFWidgetDrawingEmbedderTest, ACaptionAndAnIconCanBeDrawnInMemory) {
  const std::string bytes =
      ForeignButtonsPdf(/*with_fonts=*/true, /*with_appearances=*/false);
  embedpdf_test::ScopedTestDocument foreign(
      embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                            nullptr));
  ASSERT_TRUE(foreign);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(foreign.get());
  const std::map<uint32_t, std::string> before = ObjectGraph(doc);
  ScopedFPDFPage page(FPDF_LoadPage(foreign.get(), 0));
  ASSERT_TRUE(page);
  for (const auto& layout : kCaptionAndIconLayouts) {
    SCOPED_TRACE(layout.name);
    ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), layout.layout));
    ASSERT_TRUE(button);
    std::optional<CPDF_AppearanceGenerator::GeneratedAP> drawn =
        CPDF_AppearanceGenerator::GenerateEphemeralFormAP(
            doc, DictOf(button.get()), CPDF_AppearanceGenerator::kPushButton);
    ASSERT_TRUE(drawn.has_value());
    ASSERT_TRUE(drawn->normal_stream);
    EXPECT_EQ(0u, drawn->normal_stream->GetObjNum());
    EXPECT_EQ(CFX_FloatRect(0, 0, 102, 42),
              drawn->normal_stream->GetDict()->GetRectFor("BBox"));
    const std::string content = StreamBytes(drawn->normal_stream.Get());
    EXPECT_NE(std::string::npos, content.find("/FRM Do"));
    EXPECT_LT(content.find("/FRM Do"), content.find("BT\n"));
    RetainPtr<const CPDF_Dictionary> resources =
        drawn->normal_stream->GetDict()->GetDictFor("Resources");
    ASSERT_TRUE(resources);
    EXPECT_EQ(
        kForeignIcon,
        resources->GetDictFor("XObject")->GetStreamFor("FRM")->GetObjNum());
    ASSERT_TRUE(resources->GetDictFor("Font"));
    EXPECT_EQ(1u, resources->GetDictFor("Font")->size());
    EXPECT_FALSE(DictOf(button.get())->KeyExist("AP"));
  }
  EXPECT_EQ(before, ObjectGraph(doc));
}

// How a button is shared between its caption and its icon is the rule of
// PDFium's own form filler (GetPushButtonAppStream() in
// fpdfsdk/cpdfsdk_appstream.cpp), which draws a button for upstream's
// viewers. Both draw the same seven buttons here, one in each layout, at a
// font size of 12 and at a fitted one: the icon is clipped to the same part
// and lands in the same place, and the caption is written at the same size
// from the same point.
TEST_F(EPDFWidgetDrawingEmbedderTest, AButtonIsSharedAsTheFormFillerSharesIt) {
  constexpr float kWithin = 1e-2f;
  // A border and a background that show, for the files kept to look at.
  auto paint = [](FPDF_ANNOTATION button) {
    ASSERT_TRUE(EPDFAnnot_SetMKColor(button, EPDF_MK_COLOR_BC, 0, 153, 0));
    ASSERT_TRUE(EPDFAnnot_SetMKColor(button, EPDF_MK_COLOR_BG, 255, 255, 204));
  };
  for (int font_size : {12, 0}) {
    SCOPED_TRACE(font_size);
    const std::string bytes = ForeignButtonsPdf(
        /*with_fonts=*/true, /*with_appearances=*/false, font_size);

    // The form filler's, in a document of its own.
    std::vector<ButtonContent> theirs;
    std::string theirs_saved;
    {
      ScopedFPDFDocument document(
          FPDF_LoadMemDocument(bytes.data(), bytes.size(), nullptr));
      ASSERT_TRUE(document);
      FPDF_FORMHANDLE form = SetupFormFillEnvironment(
          document.get(), JavaScriptOption::kDisableJavaScript);
      ASSERT_TRUE(form);
      {
        ScopedFPDFPage page(FPDF_LoadPage(document.get(), 0));
        ASSERT_TRUE(page);
        FORM_OnAfterLoadPage(page.get(), form);
        CPDFSDK_PageView* view =
            CPDFSDKFormFillEnvironmentFromFPDFFormHandle(form)
                ->GetOrCreatePageView(IPDFPageFromFPDFPage(page.get()));
        ASSERT_TRUE(view);
        for (int layout = 0; layout <= 6; ++layout) {
          ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), layout));
          ASSERT_TRUE(button);
          paint(button.get());
          CPDFSDK_Widget* widget =
              ToCPDFSDKWidget(view->GetAnnotByDict(DictOf(button.get())));
          ASSERT_TRUE(widget);
          widget->ResetAppearance(std::nullopt,
                                  CPDFSDK_Widget::kValueUnchanged);
          RetainPtr<const CPDF_Stream> appearance = AppearanceOf(button.get());
          ASSERT_TRUE(appearance);
          theirs.push_back(ReadButtonContent(StreamBytes(appearance.Get())));
        }
        FORM_OnBeforeClosePage(page.get(), form);
      }
      FPDFDOC_ExitFormFillEnvironment(form);
      theirs_saved = SavedBytes(document.get());
    }

    // Ours.
    embedpdf_test::ScopedTestDocument document(
        embedpdf_test::LoadMemDocumentForTest(bytes.data(), bytes.size(),
                                              nullptr));
    ASSERT_TRUE(document);
    {
      ScopedFPDFPage page(FPDF_LoadPage(document.get(), 0));
      ASSERT_TRUE(page);
      for (int layout = 0; layout <= 6; ++layout) {
        SCOPED_TRACE(layout);
        ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), layout));
        ASSERT_TRUE(button);
        paint(button.get());
        ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
        const ButtonContent ours =
            ReadButtonContent(StreamBytes(AppearanceOf(button.get()).Get()));
        const ButtonContent& their = theirs[layout];

        const bool shows_icon = layout != 0;
        ASSERT_EQ(shows_icon, their.picture.has_value());
        ASSERT_EQ(shows_icon, ours.picture.has_value());
        if (shows_icon) {
          ASSERT_TRUE(their.icon_part);
          ASSERT_TRUE(ours.icon_part);
          ExpectRectNear(*their.icon_part, *ours.icon_part, kWithin);
          ExpectRectNear(*their.picture, *ours.picture, kWithin);
        }
        const bool shows_caption = layout != 1;
        ASSERT_EQ(shows_caption, their.caption_start.has_value());
        ASSERT_EQ(shows_caption, ours.caption_start.has_value());
        if (shows_caption) {
          ASSERT_TRUE(their.caption_size);
          ASSERT_TRUE(ours.caption_size);
          EXPECT_EQ(*their.caption_size, *ours.caption_size);
          EXPECT_NEAR(their.caption_start->x, ours.caption_start->x, kWithin);
          EXPECT_NEAR(their.caption_start->y, ours.caption_start->y, kWithin);
        }
      }
    }
    if (font_size == 12) {
      KeepSample(theirs_saved, "button-layouts-drawn-by-pdfium-form-filler");
      KeepSample(SavedBytes(document.get()), "button-layouts-drawn-by-us");
      // And the same buttons without an appearance, for a viewer that draws
      // its own.
      ScopedFPDFDocument undrawn(
          FPDF_LoadMemDocument(bytes.data(), bytes.size(), nullptr));
      ASSERT_TRUE(undrawn);
      ScopedFPDFPage page(FPDF_LoadPage(undrawn.get(), 0));
      ASSERT_TRUE(page);
      for (int layout = 0; layout <= 6; ++layout) {
        ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), layout));
        ASSERT_TRUE(button);
        paint(button.get());
      }
      CPDFDocumentFromFPDFDocument(undrawn.get())
          ->GetMutableRoot()
          ->GetMutableDictFor("AcroForm")
          ->SetNewFor<CPDF_Boolean>("NeedAppearances", true);
      KeepSample(SavedBytes(undrawn.get()), "button-layouts-not-drawn");
    }
  }
}

// A button whose appearance was its icon itself, as another app may write
// one, has the icon cleared and is drawn again, in every layout. /MK no
// longer names the picture, but another button still shows it: it is not
// written into.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       AClearedIconThatWasTheAppearanceIsNotWrittenInto) {
  const unsigned int picture = AddDrawing(doc_.get());
  ASSERT_NE(0u, picture);
  const std::string picture_before = ObjectGraph(pdf())[picture];
  for (int layout = 0; layout <= 6; ++layout) {
    SCOPED_TRACE(layout);
    ScopedFPDFPage page(FPDFPage_New(doc_.get(), layout + 1, 400, 400));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation cleared =
        NewButton({10, 60, 130, 10}, 0.0f, page.get());
    ScopedFPDFAnnotation other =
        NewButton({140, 60, 260, 10}, 0.0f, page.get());
    ASSERT_TRUE(cleared);
    ASSERT_TRUE(other);
    SetIcon(other.get(), picture);
    ASSERT_TRUE(EPDFAnnot_SetMKText(cleared.get(), EPDF_MK_TEXT_CA,
                                    GetFPDFWideString(L"Go").get()));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(
        cleared.get(), FPDF_FONT_HELVETICA, 12, 0, 0, 0));
    ASSERT_TRUE(EPDFAnnot_SetMKIcon(cleared.get(), EPDF_MK_ICON_I, picture));
    ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(cleared.get(), layout));
    MutableDictOf(cleared.get())
        ->GetOrCreateDictFor("AP")
        ->SetNewFor<CPDF_Reference>("N", pdf(), picture);

    ASSERT_TRUE(EPDFAnnot_ClearMKIcon(cleared.get(), EPDF_MK_ICON_I));
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(cleared.get()));

    EXPECT_NE(picture, AppearanceOf(cleared.get())->GetObjNum());
    EXPECT_EQ(picture_before, ObjectGraph(pdf())[picture]);
    // The other button's picture is 100 by 50, in the middle of its box.
    const Shown shown(page.get());
    EXPECT_EQ(kRed, shown.At(170, 35));
    EXPECT_EQ(kBlue, shown.At(230, 35));
  }
}

// A picture a button no longer names is in no file saved in full once the
// button is drawn again, whichever layout drew it: the new appearance names
// no icon, and nothing reaches the appearance that did.
TEST_F(EPDFWidgetDrawingEmbedderTest, AClearedPictureIsInNoFullSave) {
  ScopedFPDFAnnotation button = NewButton({100, 254, 304, 150}, 2.0f);
  ASSERT_TRUE(button);
  const unsigned int picture = AddDrawing(doc_.get());
  ASSERT_NE(0u, picture);
  for (int layout = 1; layout <= 6; ++layout) {
    SCOPED_TRACE(layout);
    SetCaptionAndIcon(button.get(), picture, layout);
    ASSERT_EQ(picture, DrawnIconNumber(button.get()));
    ASSERT_EQ(1, StreamsHolding(SavedBytes(doc_.get()), kPictureMark));

    ASSERT_TRUE(EPDFAnnot_ClearMKIcon(button.get(), EPDF_MK_ICON_I));
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
    EXPECT_EQ(0u, DrawnIconNumber(button.get()));
    EXPECT_TRUE(DrawingsOf(doc_.get()).empty());
    EXPECT_EQ(0, StreamsHolding(SavedBytes(doc_.get()), kPictureMark));
  }
}

// Two widgets may name one /MK. Every setter writes the widget's own: the
// widget takes a copy first, with the same entries, and the other widget
// keeps what it had. A call that changes nothing copies nothing.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       AWidgetKeepsAnMKItSharesWhenTheOtherIsWritten) {
  const unsigned int old_picture = AddDrawing(doc_.get());
  const unsigned int new_picture =
      AddDrawing(doc_.get(), Source::kVector, kGreen, kYellow);
  ASSERT_NE(0u, old_picture);
  ASSERT_NE(0u, new_picture);
  struct Write {
    const char* name;
    // The /MK key it writes.
    const char* key;
    std::function<bool(FPDF_ANNOTATION)> write;
  };
  const Write kWrites[] = {
      {"EPDFAnnot_SetMKIcon", "I",
       [&](FPDF_ANNOTATION widget) {
         return !!EPDFAnnot_SetMKIcon(widget, EPDF_MK_ICON_I, new_picture);
       }},
      {"EPDFAnnot_ClearMKIcon", "I",
       [](FPDF_ANNOTATION widget) {
         return !!EPDFAnnot_ClearMKIcon(widget, EPDF_MK_ICON_I);
       }},
      {"EPDFAnnot_SetMKText", "CA",
       [](FPDF_ANNOTATION widget) {
         return !!EPDFAnnot_SetMKText(widget, EPDF_MK_TEXT_CA,
                                      GetFPDFWideString(L"Stop").get());
       }},
      {"EPDFAnnot_SetMKText, none", "CA",
       [](FPDF_ANNOTATION widget) {
         return !!EPDFAnnot_SetMKText(widget, EPDF_MK_TEXT_CA, nullptr);
       }},
      {"EPDFAnnot_SetMKTextPosition", "TP",
       [](FPDF_ANNOTATION widget) {
         return !!EPDFAnnot_SetMKTextPosition(widget, 4);
       }},
      {"EPDFAnnot_SetMKRotation", "R",
       [](FPDF_ANNOTATION widget) {
         return !!EPDFAnnot_SetMKRotation(widget, 90);
       }},
      {"EPDFAnnot_SetMKColor", "BC",
       [](FPDF_ANNOTATION widget) {
         return !!EPDFAnnot_SetMKColor(widget, EPDF_MK_COLOR_BC, 0, 255, 0);
       }},
      {"EPDFAnnot_ClearMKColor", "BC",
       [](FPDF_ANNOTATION widget) {
         return !!EPDFAnnot_ClearMKColor(widget, EPDF_MK_COLOR_BC);
       }},
  };
  int page_index = 0;
  for (const Write& write : kWrites) {
    SCOPED_TRACE(write.name);
    ScopedFPDFPage page(FPDFPage_New(doc_.get(), ++page_index, 400, 400));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation first = NewButton({10, 60, 130, 10}, 0.0f, page.get());
    ScopedFPDFAnnotation second =
        NewButton({140, 60, 260, 10}, 0.0f, page.get());
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    RetainPtr<CPDF_Dictionary> shared = pdf()->NewIndirect<CPDF_Dictionary>();
    shared->SetNewFor<CPDF_Reference>("I", pdf(), old_picture);
    shared->SetNewFor<CPDF_String>("CA", "Go");
    shared->SetNewFor<CPDF_Number>("TP", 2);
    RetainPtr<CPDF_Array> red = shared->SetNewFor<CPDF_Array>("BC");
    red->AppendNew<CPDF_Number>(1);
    red->AppendNew<CPDF_Number>(0);
    red->AppendNew<CPDF_Number>(0);
    for (FPDF_ANNOTATION widget : {first.get(), second.get()}) {
      MutableDictOf(widget)->SetNewFor<CPDF_Reference>("MK", pdf(),
                                                       shared->GetObjNum());
    }
    const std::string shared_before = ObjectGraph(pdf())[shared->GetObjNum()];

    // A call that changes nothing leaves the widget naming the shared one.
    ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(first.get(), 2));
    EXPECT_TRUE(DictOf(first.get())->GetObjectFor("MK")->IsReference());

    ASSERT_TRUE(write.write(first.get()));

    // The shared dictionary is as it was, and the second still names it.
    EXPECT_EQ(shared_before, ObjectGraph(pdf())[shared->GetObjNum()]);
    RetainPtr<const CPDF_Object> seconds_entry =
        DictOf(second.get())->GetObjectFor("MK");
    ASSERT_TRUE(seconds_entry->IsReference());
    EXPECT_EQ(shared->GetObjNum(),
              seconds_entry->AsReference()->GetRefObjNum());
    EXPECT_EQ(old_picture, EPDFAnnot_GetMKIcon(second.get(), EPDF_MK_ICON_I));
    EXPECT_EQ(2, EPDFAnnot_GetMKTextPosition(second.get()));

    // The first has its own, with everything it didn't write as it was.
    RetainPtr<const CPDF_Object> firsts_entry =
        DictOf(first.get())->GetObjectFor("MK");
    ASSERT_TRUE(firsts_entry);
    EXPECT_FALSE(firsts_entry->IsReference());
    RetainPtr<const CPDF_Dictionary> own =
        DictOf(first.get())->GetDictFor("MK");
    ASSERT_TRUE(own);
    for (const char* key : {"I", "CA", "TP", "BC"}) {
      if (ByteStringView(key) == ByteStringView(write.key)) {
        continue;
      }
      SCOPED_TRACE(key);
      ASSERT_TRUE(own->KeyExist(key));
    }
    if (ByteStringView(write.key) != "I") {
      EXPECT_EQ(old_picture, EPDFAnnot_GetMKIcon(first.get(), EPDF_MK_ICON_I));
    }
    if (ByteStringView(write.key) != "CA") {
      EXPECT_EQ(L"Go", own->GetUnicodeTextFor("CA"));
    }
    if (ByteStringView(write.key) != "TP") {
      EXPECT_EQ(2, EPDFAnnot_GetMKTextPosition(first.get()));
    }
  }
}

// A /MK colour may be an array that is an object of its own, which another
// /MK names too: through one /MK that two widgets share, or from an /MK
// each. A colour one widget sets goes into a new array of its own, and the
// other widget keeps its colour. (The setter wrote into the array it found.)
TEST_F(EPDFWidgetDrawingEmbedderTest, AColourArrayOfItsOwnIsNotWrittenInto) {
  int page_index = 0;
  for (bool one_mk : {true, false}) {
    SCOPED_TRACE(one_mk ? "one /MK for both widgets" : "an /MK each");
    ScopedFPDFPage page(FPDFPage_New(doc_.get(), ++page_index, 400, 400));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation first = NewButton({10, 60, 130, 10}, 2.0f, page.get());
    ScopedFPDFAnnotation second =
        NewButton({140, 60, 260, 10}, 2.0f, page.get());
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    RetainPtr<CPDF_Array> red = pdf()->NewIndirect<CPDF_Array>();
    red->AppendNew<CPDF_Number>(1);
    red->AppendNew<CPDF_Number>(0);
    red->AppendNew<CPDF_Number>(0);
    if (one_mk) {
      RetainPtr<CPDF_Dictionary> shared = pdf()->NewIndirect<CPDF_Dictionary>();
      shared->SetNewFor<CPDF_Reference>("BC", pdf(), red->GetObjNum());
      for (FPDF_ANNOTATION widget : {first.get(), second.get()}) {
        MutableDictOf(widget)->SetNewFor<CPDF_Reference>("MK", pdf(),
                                                         shared->GetObjNum());
      }
    } else {
      for (FPDF_ANNOTATION widget : {first.get(), second.get()}) {
        MutableDictOf(widget)
            ->SetNewFor<CPDF_Dictionary>("MK")
            ->SetNewFor<CPDF_Reference>("BC", pdf(), red->GetObjNum());
      }
    }
    const std::string red_before = ObjectGraph(pdf())[red->GetObjNum()];

    ASSERT_TRUE(EPDFAnnot_SetMKColor(first.get(), EPDF_MK_COLOR_BC, 0, 255, 0));

    // The array is as it was, and the second widget is red, drawn again too.
    EXPECT_EQ(red_before, ObjectGraph(pdf())[red->GetObjNum()]);
    unsigned int r = 9;
    unsigned int g = 9;
    unsigned int b = 9;
    ASSERT_TRUE(
        EPDFAnnot_GetMKColor(second.get(), EPDF_MK_COLOR_BC, &r, &g, &b));
    EXPECT_EQ(255u, r);
    EXPECT_EQ(0u, g);
    EXPECT_EQ(0u, b);
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(second.get()));
    EXPECT_NE(
        std::string::npos,
        StreamBytes(AppearanceOf(second.get()).Get()).find("1 0 0 RG"));

    // The first is green, in an array of its own.
    ASSERT_TRUE(
        EPDFAnnot_GetMKColor(first.get(), EPDF_MK_COLOR_BC, &r, &g, &b));
    EXPECT_EQ(0u, r);
    EXPECT_EQ(255u, g);
    EXPECT_EQ(0u, b);
    RetainPtr<const CPDF_Dictionary> own =
        DictOf(first.get())->GetDictFor("MK");
    ASSERT_TRUE(own);
    RetainPtr<const CPDF_Object> entry = own->GetObjectFor("BC");
    ASSERT_TRUE(entry);
    EXPECT_FALSE(entry->IsReference());
  }
}

// A colour call that changes nothing writes nothing, as the other /MK
// setters: the colour a widget already has, set again, and a colour it does
// not have, removed. A widget that names a shared /MK goes on naming it, and
// on a layer nothing is copied into the layer. A colour is the one asked for
// only as three numbers: four that begin with them are set anew.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       AColourCallThatChangesNothingWritesNothing) {
  {
    ScopedFPDFAnnotation first = NewButton({10, 60, 130, 10});
    ScopedFPDFAnnotation second = NewButton({140, 60, 260, 10});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    RetainPtr<CPDF_Dictionary> shared = pdf()->NewIndirect<CPDF_Dictionary>();
    RetainPtr<CPDF_Array> red = shared->SetNewFor<CPDF_Array>("BC");
    red->AppendNew<CPDF_Number>(1);
    red->AppendNew<CPDF_Number>(0);
    red->AppendNew<CPDF_Number>(0);
    for (FPDF_ANNOTATION widget : {first.get(), second.get()}) {
      MutableDictOf(widget)->SetNewFor<CPDF_Reference>("MK", pdf(),
                                                       shared->GetObjNum());
    }
    const std::string shared_before = ObjectGraph(pdf())[shared->GetObjNum()];

    ASSERT_TRUE(EPDFAnnot_SetMKColor(first.get(), EPDF_MK_COLOR_BC, 255, 0, 0));
    EXPECT_TRUE(DictOf(first.get())->GetObjectFor("MK")->IsReference());
    ASSERT_TRUE(EPDFAnnot_ClearMKColor(first.get(), EPDF_MK_COLOR_BG));
    EXPECT_TRUE(DictOf(first.get())->GetObjectFor("MK")->IsReference());
    EXPECT_EQ(shared_before, ObjectGraph(pdf())[shared->GetObjNum()]);

    // Four numbers are another colour space's: red is set as three.
    red->AppendNew<CPDF_Number>(0);
    ASSERT_TRUE(EPDFAnnot_SetMKColor(first.get(), EPDF_MK_COLOR_BC, 255, 0, 0));
    RetainPtr<const CPDF_Dictionary> own =
        DictOf(first.get())->GetDictFor("MK");
    ASSERT_TRUE(own);
    EXPECT_FALSE(DictOf(first.get())->GetObjectFor("MK")->IsReference());
    RetainPtr<const CPDF_Array> set = own->GetArrayFor("BC");
    ASSERT_TRUE(set);
    EXPECT_EQ(3u, set->size());
    EXPECT_EQ(4u, red->size());
  }
  {
    // A file's own widget, on a layer. Its third button is given a red
    // border colour, in a text of the same length, so the offsets hold.
    std::string bytes = ForeignButtonsPdf();
    const std::string was =
        "/TP 2 /CA (Go) /IF << /SW /A /S /P /A [0.5 0.5] >>";
    const std::string now =
        "/TP 2 /CA (Go) /BC [1 0 0] /IF << /SW /A /S /P >> ";
    ASSERT_EQ(was.size(), now.size());
    const size_t at = bytes.find(was);
    ASSERT_NE(std::string::npos, at);
    bytes.replace(at, was.size(), now);
    embedpdf_test::LayerFixture foreign;
    ASSERT_TRUE(foreign.OpenBytes({bytes.begin(), bytes.end()}));
    ScopedFPDFPage page(FPDF_LoadPage(foreign.layer, 0));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), 2));
    ASSERT_TRUE(button);
    unsigned int r = 9;
    unsigned int g = 9;
    unsigned int b = 9;
    ASSERT_TRUE(
        EPDFAnnot_GetMKColor(button.get(), EPDF_MK_COLOR_BC, &r, &g, &b));
    ASSERT_EQ(255u, r);
    ASSERT_EQ(0u, g);
    ASSERT_EQ(0u, b);
    ASSERT_FALSE(
        EPDFAnnot_GetMKColor(button.get(), EPDF_MK_COLOR_BG, &r, &g, &b));
    ASSERT_EQ(0u, EPDFLayer_GetPromotedObjectCount(foreign.layer));

    ASSERT_TRUE(EPDFLayer_BeginTransaction(foreign.layer));
    ASSERT_TRUE(
        EPDFAnnot_SetMKColor(button.get(), EPDF_MK_COLOR_BC, 255, 0, 0));
    ASSERT_TRUE(EPDFAnnot_ClearMKColor(button.get(), EPDF_MK_COLOR_BG));
    ASSERT_TRUE(EPDFLayer_CommitTransaction(foreign.layer));

    EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(foreign.layer));
  }
}

// A setter writes nothing only for an entry that is well formed and reads as
// the value. A malformed one that reads the same is written anew: a colour
// with a name among its numbers or a number out of range, a caption stored
// as a name, a text position or a rotation stored as a real that is cut down
// to it. (The no-change check read each as the value and left it.)
TEST_F(EPDFWidgetDrawingEmbedderTest,
       AMalformedEntryThatReadsAsTheValueIsWrittenAnew) {
  struct Case {
    const char* name;
    const char* key;
    // Stores the malformed entry in the widget's /MK.
    std::function<void(CPDF_Dictionary*)> store;
    // Asks for the value the entry reads as.
    std::function<bool(FPDF_ANNOTATION)> set;
    // Whether the entry is as the setter writes that value.
    std::function<bool(const CPDF_Object*)> well_formed;
  };
  auto set_red = [](FPDF_ANNOTATION widget) {
    return !!EPDFAnnot_SetMKColor(widget, EPDF_MK_COLOR_BC, 255, 0, 0);
  };
  auto is_red = [](const CPDF_Object* entry) {
    const CPDF_Array* array = entry->AsArray();
    if (!array || array->size() != 3) {
      return false;
    }
    for (size_t i = 0; i < 3; ++i) {
      RetainPtr<const CPDF_Object> number = array->GetDirectObjectAt(i);
      if (!number || !number->IsNumber() ||
          number->GetNumber() != (i == 0 ? 1.0f : 0.0f)) {
        return false;
      }
    }
    return true;
  };
  auto is_integer = [](int value) {
    return [value](const CPDF_Object* entry) {
      const CPDF_Number* number = entry->AsNumber();
      return number && number->IsInteger() && number->GetInteger() == value;
    };
  };
  const Case kCases[] = {
      {"a colour with a name among its numbers", "BC",
       [](CPDF_Dictionary* mk) {
         RetainPtr<CPDF_Array> color = mk->SetNewFor<CPDF_Array>("BC");
         color->AppendNew<CPDF_Number>(1);
         color->AppendNew<CPDF_Name>("None");
         color->AppendNew<CPDF_Number>(0);
       },
       set_red, is_red},
      {"a colour with numbers out of range", "BC",
       [](CPDF_Dictionary* mk) {
         RetainPtr<CPDF_Array> color = mk->SetNewFor<CPDF_Array>("BC");
         color->AppendNew<CPDF_Number>(1.001f);
         color->AppendNew<CPDF_Number>(-0.001f);
         color->AppendNew<CPDF_Number>(0);
       },
       set_red, is_red},
      {"a caption stored as a name", "CA",
       [](CPDF_Dictionary* mk) { mk->SetNewFor<CPDF_Name>("CA", "Go"); },
       [](FPDF_ANNOTATION widget) {
         return !!EPDFAnnot_SetMKText(widget, EPDF_MK_TEXT_CA,
                                      GetFPDFWideString(L"Go").get());
       },
       [](const CPDF_Object* entry) {
         return entry->IsString() && entry->GetUnicodeText() == L"Go";
       }},
      {"a text position stored as a real", "TP",
       [](CPDF_Dictionary* mk) { mk->SetNewFor<CPDF_Number>("TP", 2.7f); },
       [](FPDF_ANNOTATION widget) {
         return !!EPDFAnnot_SetMKTextPosition(widget, 2);
       },
       is_integer(2)},
      {"a rotation stored as a real", "R",
       [](CPDF_Dictionary* mk) { mk->SetNewFor<CPDF_Number>("R", 90.5f); },
       [](FPDF_ANNOTATION widget) {
         return !!EPDFAnnot_SetMKRotation(widget, 90);
       },
       is_integer(90)},
  };
  int page_index = 0;
  for (const Case& c : kCases) {
    SCOPED_TRACE(c.name);
    ScopedFPDFPage page(FPDFPage_New(doc_.get(), ++page_index, 400, 400));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation button = NewButton({10, 60, 130, 10}, 2.0f, page.get());
    ASSERT_TRUE(button);
    c.store(MutableDictOf(button.get())->GetOrCreateDictFor("MK").Get());
    auto entry = [&]() {
      RetainPtr<const CPDF_Dictionary> mk =
          DictOf(button.get())->GetDictFor("MK");
      return mk ? mk->GetDirectObjectFor(c.key) : nullptr;
    };
    ASSERT_TRUE(entry());
    ASSERT_FALSE(c.well_formed(entry().Get()));

    ASSERT_TRUE(c.set(button.get()));

    ASSERT_TRUE(entry());
    EXPECT_TRUE(c.well_formed(entry().Get()));
  }
}

// Two widgets may name one /AP. A widget that is drawn again gets an /AP of
// its own first, and the other widget keeps the shared one and every
// appearance it names: it shows what it showed.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       AWidgetKeepsAnAPItSharesWhenTheOtherIsDrawnAgain) {
  const unsigned int old_picture = AddDrawing(doc_.get());
  const unsigned int new_picture =
      AddDrawing(doc_.get(), Source::kVector, kGreen, kYellow);
  ASSERT_NE(0u, old_picture);
  ASSERT_NE(0u, new_picture);
  enum class Kind {
    kIconButton,
    kCaptionAndIconButton,
    kCaptionButton,
    kCheckBox,
    kSignatureBox
  };
  struct Case {
    const char* name;
    Kind kind;
  };
  const Case kCases[] = {
      {"a button that shows its icon alone", Kind::kIconButton},
      {"a button with a caption and an icon", Kind::kCaptionAndIconButton},
      {"a button with a caption", Kind::kCaptionButton},
      {"a check box", Kind::kCheckBox},
      {"a signature box", Kind::kSignatureBox},
  };
  // The objects a dictionary names, and those the dictionaries in it name.
  std::function<void(const CPDF_Dictionary*, std::vector<uint32_t>*)> named =
      [&](const CPDF_Dictionary* dict, std::vector<uint32_t>* numbers) {
        CPDF_DictionaryLocker locker(dict);
        for (const auto& entry : locker) {
          if (const CPDF_Reference* reference = entry.second->AsReference()) {
            numbers->push_back(reference->GetRefObjNum());
          } else if (const CPDF_Dictionary* inner =
                         entry.second->AsDictionary()) {
            named(inner, numbers);
          }
        }
      };
  int page_index = 0;
  for (const Case& c : kCases) {
    SCOPED_TRACE(c.name);
    ScopedFPDFPage page(FPDFPage_New(doc_.get(), ++page_index, 400, 400));
    ASSERT_TRUE(page);
    auto make = [&](const FS_RECTF& rect) {
      switch (c.kind) {
        case Kind::kCheckBox:
          return NewWidget(doc_.get(),
                           NewField(doc_.get(), EPDF_FORMFIELD_FAMILY_CHECKBOX),
                           page.get(), rect, {}, "Yes");
        case Kind::kSignatureBox:
          return NewSignatureWidget(rect, page.get());
        default:
          return NewButton(rect, 0.0f, page.get());
      }
    };
    // As the widget stands first, and then changed so that it draws
    // something else.
    auto draw = [&](FPDF_ANNOTATION widget, bool changed) {
      switch (c.kind) {
        case Kind::kIconButton:
          SetIcon(widget, changed ? new_picture : old_picture);
          return;
        case Kind::kCaptionAndIconButton:
          SetCaptionAndIcon(widget, changed ? new_picture : old_picture,
                            kCaptionBelow);
          return;
        case Kind::kCaptionButton:
          ASSERT_TRUE(EPDFAnnot_SetMKText(
              widget, EPDF_MK_TEXT_CA,
              GetFPDFWideString(changed ? L"Stop here" : L"Go").get()));
          ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(
              widget, FPDF_FONT_HELVETICA, 12, 0, 0, 0));
          ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(widget));
          return;
        case Kind::kCheckBox:
        case Kind::kSignatureBox:
          ASSERT_TRUE(EPDFAnnot_SetMKColor(widget, EPDF_MK_COLOR_BG,
                                           changed ? 0 : 255, 255, 0));
          ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(widget));
          return;
      }
    };
    ScopedFPDFAnnotation first = make({10, 60, 130, 10});
    ScopedFPDFAnnotation second = make({140, 60, 260, 10});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    draw(first.get(), /*changed=*/false);
    draw(second.get(), /*changed=*/false);

    // The second's /AP becomes an object of its own, and both name it.
    RetainPtr<const CPDF_Dictionary> seconds_own =
        DictOf(second.get())->GetDictFor("AP");
    ASSERT_TRUE(seconds_own);
    const uint32_t shared = pdf()->AddIndirectObject(seconds_own->Clone());
    ASSERT_NE(0u, shared);
    for (FPDF_ANNOTATION widget : {first.get(), second.get()}) {
      MutableDictOf(widget)->SetNewFor<CPDF_Reference>("AP", pdf(), shared);
    }
    std::vector<uint32_t> kept = {shared};
    named(ToDictionary(pdf()->GetOrParseIndirectObject(shared)).Get(), &kept);
    ASSERT_GT(kept.size(), 1u);
    std::map<uint32_t, std::string> before = ObjectGraph(pdf());
    const uint32_t shown_before = Shown(page.get()).At(170, 35);

    draw(first.get(), /*changed=*/true);

    std::map<uint32_t, std::string> after = ObjectGraph(pdf());
    for (uint32_t number : kept) {
      SCOPED_TRACE(number);
      EXPECT_EQ(before[number], after[number]);
    }
    RetainPtr<const CPDF_Object> seconds_entry =
        DictOf(second.get())->GetObjectFor("AP");
    ASSERT_TRUE(seconds_entry->IsReference());
    EXPECT_EQ(shared, seconds_entry->AsReference()->GetRefObjNum());
    EXPECT_EQ(shown_before, Shown(page.get()).At(170, 35));
    // The first has its own, which names its new appearance.
    RetainPtr<const CPDF_Object> firsts_entry =
        DictOf(first.get())->GetObjectFor("AP");
    ASSERT_TRUE(firsts_entry);
    EXPECT_FALSE(firsts_entry->IsReference());
    std::vector<uint32_t> firsts;
    named(DictOf(first.get())->GetDictFor("AP").Get(), &firsts);
    ASSERT_FALSE(firsts.empty());
    for (uint32_t number : firsts) {
      EXPECT_FALSE(before.contains(number)) << number;
    }
  }
}

// On a layer, drawing a button again copies nothing into the layer that it
// leaves behind: not the appearance the button had, and not a picture, even
// when the appearance was the picture itself. In every layout.
TEST_F(EPDFWidgetDrawingEmbedderTest,
       ARedrawnButtonCopiesNoOldAppearanceIntoALayer) {
  for (bool appearance_is_the_icon : {false, true}) {
    for (int layout = 0; layout <= 6; ++layout) {
      SCOPED_TRACE(appearance_is_the_icon ? "/AP /N is the icon"
                                          : "/AP /N is a stream of its own");
      SCOPED_TRACE(layout);
      std::string bytes = ForeignButtonsPdf();
      if (appearance_is_the_icon) {
        // Of the same length, so the file's offsets hold.
        const std::string was = "/N " + std::to_string(14 + layout) + " 0 R >>";
        const size_t at = bytes.find(was);
        ASSERT_NE(std::string::npos, at);
        bytes.replace(at, was.size(), "/N 6 0 R  >>");
      }
      embedpdf_test::LayerFixture foreign;
      ASSERT_TRUE(foreign.OpenBytes({bytes.begin(), bytes.end()}));
      auto* layer = static_cast<CPDF_LayerDocument*>(
          CPDFDocumentFromFPDFDocument(foreign.layer));
      ScopedFPDFPage page(FPDF_LoadPage(foreign.layer, 0));
      ASSERT_TRUE(page);
      ScopedFPDFAnnotation button(FPDFPage_GetAnnot(page.get(), layout));
      ASSERT_TRUE(button);
      const uint32_t old_appearance = AppearanceOf(button.get())->GetObjNum();
      ASSERT_EQ(appearance_is_the_icon, old_appearance == kForeignIcon);
      ASSERT_FALSE(layer->FindPromotedObject(old_appearance));
      ASSERT_FALSE(layer->FindPromotedObject(kForeignIcon));

      ASSERT_TRUE(EPDFLayer_BeginTransaction(foreign.layer));
      ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
      ASSERT_TRUE(EPDFLayer_CommitTransaction(foreign.layer));

      EXPECT_NE(old_appearance, AppearanceOf(button.get())->GetObjNum());
      EXPECT_FALSE(layer->FindPromotedObject(old_appearance));
      EXPECT_FALSE(layer->FindPromotedObject(kForeignIcon));
    }
  }
}

// An icon is a form XObject. A /MK entry that names anything else, an image
// say, is no icon: it is not read as one, not listed, not drawn, and no call
// sets one. The entry can still be removed.
TEST_F(EPDFWidgetDrawingEmbedderTest, AnIconEntryThatNamesNoFormIsNoIcon) {
  ScopedFPDFAnnotation button = NewButton({100, 254, 304, 150}, 2.0f);
  ASSERT_TRUE(button);
  RetainPtr<CPDF_Stream> image =
      pdf()->NewIndirect<CPDF_Stream>(pdfium::MakeRetain<CPDF_Dictionary>());
  RetainPtr<CPDF_Dictionary> image_dict = image->GetMutableDict();
  image_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  image_dict->SetNewFor<CPDF_Name>("Subtype", "Image");
  MutableDictOf(button.get())
      ->GetOrCreateDictFor("MK")
      ->SetNewFor<CPDF_Reference>("I", pdf(), image->GetObjNum());

  EXPECT_EQ(0u, EPDFAnnot_GetMKIcon(button.get(), EPDF_MK_ICON_I));
  EXPECT_FALSE(EPDFAnnot_HasMKIcon(button.get(), EPDF_MK_ICON_I));
  EXPECT_TRUE(DrawingsOf(doc_.get()).empty());
  EXPECT_FALSE(
      EPDFAnnot_SetMKIcon(button.get(), EPDF_MK_ICON_RI, image->GetObjNum()));

  // Where the icon shows alone the button is its box, and beside a caption
  // the caption is drawn alone.
  ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(button.get(), 1));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
  EXPECT_FALSE(DrawnIconOf(button.get()));
  EXPECT_EQ(0u, DrawnIconNumber(button.get()));
  ASSERT_TRUE(EPDFAnnot_SetMKText(button.get(), EPDF_MK_TEXT_CA,
                                  GetFPDFWideString(L"Go").get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(button.get(), FPDF_FONT_HELVETICA,
                                             12, 0, 0, 0));
  ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(button.get(), kCaptionBelow));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(button.get()));
  EXPECT_FALSE(DrawnIconOf(button.get()));
  EXPECT_EQ(0u, DrawnIconNumber(button.get()));
  EXPECT_TRUE(DrawnCaptionOf(button.get()));

  ASSERT_TRUE(EPDFAnnot_ClearMKIcon(button.get(), EPDF_MK_ICON_I));
  EXPECT_FALSE(DictOf(button.get())->GetDictFor("MK")->KeyExist("I"));
}
