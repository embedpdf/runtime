// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_redact.h"

#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fxcodec/flate/flatemodule.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_annot.h"
#include "public/epdf_edit.h"
#include "public/epdf_flatten.h"
#include "public/epdf_layer.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_flatten.h"
#include "public/fpdf_save.h"
#include "public/fpdf_text.h"
#include "testing/document_sentinels.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

std::wstring ReadText(FPDF_PAGE page) {
  ScopedFPDFTextPage text(FPDFText_LoadPage(page));
  EXPECT_TRUE(text);
  if (!text) {
    return {};
  }
  const int count = FPDFText_CountChars(text.get());
  if (count <= 0) {
    return {};
  }
  std::vector<FPDF_WCHAR> buffer(count + 1);
  EXPECT_GT(FPDFText_GetText(text.get(), 0, count, buffer.data()), 0);
  return GetPlatformWString(buffer.data());
}

ScopedFPDFBitmap Render(FPDF_PAGE page) {
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(600, 600, 0));
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, 600, 600, 0xffffffff);
  FPDF_RenderPageBitmap(bitmap.get(), page, 0, 0, 600, 600, 0, 0);
  return bitmap;
}

uint32_t Pixel(FPDF_BITMAP bitmap, int x, int y) {
  const auto* data = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
  const uint8_t* pixel = data + y * FPDFBitmap_GetStride(bitmap) + x * 4;
  return (uint32_t{pixel[2]} << 16) | (uint32_t{pixel[1]} << 8) | pixel[0];
}

void ExpectSamePixels(FPDF_BITMAP before,
                      FPDF_BITMAP after,
                      int left,
                      int top,
                      int right,
                      int bottom) {
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      ASSERT_EQ(Pixel(before, x, y), Pixel(after, x, y)) << x << ", " << y;
    }
  }
}

// A PDF file of `objects`, numbered from 1, with its cross-reference table.
std::string PdfOf(const std::vector<std::string>& objects) {
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
    const std::string number = std::to_string(offset);
    pdf += std::string(10 - number.size(), '0') + number + " 00000 n \n";
  }
  pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
         " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return pdf;
}

// A one-page drawing 300 wide that writes `lines`, one every 40 points from
// the bottom up, its font dictionary an object of its own (/Font 5 0 R), as
// some PDF generators write it.
std::string MakeTextDrawing(const std::vector<std::string>& lines) {
  std::string content;
  for (size_t i = 0; i < lines.size(); ++i) {
    content += "BT /F1 24 Tf 10 " + std::to_string(10 + 40 * i) + " Td (" +
               lines[i] + ") Tj ET\n";
  }
  return PdfOf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 " +
          std::to_string(10 + 40 * lines.size()) +
          "] /Contents 4 0 R /Resources << /Font 5 0 R >> >>",
      "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
          content + "\nendstream",
      "<< /F1 6 0 R >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  });
}

RetainPtr<CPDF_Dictionary> AnnotDict(FPDF_ANNOTATION annot) {
  return CPDFAnnotContextFromFPDFAnnotation(annot)->GetMutableAnnotDict();
}

// A stamp at `rect` drawing the first page of `source`.
ScopedFPDFAnnotation AddStamp(FPDF_PAGE page,
                              FPDF_DOCUMENT source,
                              const FS_RECTF& rect) {
  ScopedFPDFAnnotation stamp(FPDFPage_CreateAnnot(page, FPDF_ANNOT_STAMP));
  EXPECT_TRUE(EPDFAnnot_SetRect(stamp.get(), &rect));
  EXPECT_TRUE(EPDFAnnot_SetAppearanceFromPage(stamp.get(), source, 0));
  EXPECT_TRUE(
      EPDFAnnot_UpdateAppearanceToRect(stamp.get(), EPDF_STAMP_FIT_CONTAIN));
  return stamp;
}

// A second stamp at `rect` placing the same drawing object as `stamp`: its
// own frame around a shared drawing.
ScopedFPDFAnnotation AddStampSharingDrawing(FPDF_DOCUMENT doc,
                                            FPDF_PAGE page,
                                            FPDF_ANNOTATION stamp,
                                            const FS_RECTF& rect) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  ScopedFPDFAnnotation other(FPDFPage_CreateAnnot(page, FPDF_ANNOT_STAMP));
  EXPECT_TRUE(EPDFAnnot_SetRect(other.get(), &rect));
  RetainPtr<CPDF_Stream> frame =
      ToStream(AnnotDict(stamp)->GetDictFor("AP")->GetStreamFor("N")->Clone());
  pdf->AddIndirectObject(frame);
  AnnotDict(other.get())
      ->SetNewFor<CPDF_Dictionary>("AP")
      ->SetNewFor<CPDF_Reference>("N", pdf, frame->GetObjNum());
  EXPECT_TRUE(
      EPDFAnnot_UpdateAppearanceToRect(other.get(), EPDF_STAMP_FIT_CONTAIN));
  return other;
}

// The drawing a stamp's frame places.
RetainPtr<const CPDF_Stream> DrawingOf(FPDF_ANNOTATION stamp) {
  return AnnotDict(stamp)
      ->GetDictFor("AP")
      ->GetStreamFor("N")
      ->GetDict()
      ->GetDictFor("Resources")
      ->GetDictFor("XObject")
      ->GetStreamFor("EPDFWRAP");
}

std::string StreamText(const CPDF_Stream* stream) {
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
  acc->LoadAllDataFiltered();
  const auto bytes = acc->GetSpan();
  return std::string(bytes.begin(), bytes.end());
}

// A stream object holding `data`: its dictionary is `entries` and /Length.
std::string StreamObject(const std::string& entries, const std::string& data) {
  return "<< " + entries + " /Length " + std::to_string(data.size()) +
         " >>\nstream\n" + data + "\nendstream";
}

// A PDF file opened as a document, or as a layer over it.
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

// Marks `rect` on `page` and applies the page's marks; on a layer in a
// transaction, as the engine runs every write.
void MarkAndApply(FPDF_DOCUMENT doc,
                  FPDF_PAGE page,
                  bool layer,
                  const FS_RECTF& rect) {
  {
    ScopedFPDFAnnotation mark(FPDFPage_CreateAnnot(page, FPDF_ANNOT_REDACT));
    ASSERT_TRUE(mark);
    ASSERT_TRUE(FPDFAnnot_SetRect(mark.get(), &rect));
  }
  if (layer) {
    ASSERT_TRUE(EPDFLayer_BeginTransaction(doc));
  }
  ASSERT_TRUE(EPDFPage_ApplyRedactions(page, nullptr, 0, nullptr));
  ASSERT_TRUE(FPDFPage_GenerateContent(page));
  if (layer) {
    ASSERT_TRUE(EPDFLayer_CommitTransaction(doc));
  }
}

// The image XObjects page `index` names in its resources.
std::vector<RetainPtr<const CPDF_Dictionary>> PageImages(CPDF_Document* doc,
                                                         int index) {
  std::vector<RetainPtr<const CPDF_Dictionary>> images;
  RetainPtr<const CPDF_Dictionary> xobjects = doc->GetPageDictionary(index)
                                                  ->GetDictFor("Resources")
                                                  ->GetDictFor("XObject");
  if (!xobjects) {
    return images;
  }
  CPDF_DictionaryLocker locker(xobjects.Get());
  for (const auto& [name, object] : locker) {
    RetainPtr<const CPDF_Stream> stream = ToStream(object->GetDirect());
    if (stream && stream->GetDict()->GetNameFor("Subtype") == "Image") {
      images.push_back(stream->GetDict());
    }
  }
  return images;
}

// A 20 x 10 checkerboard of 2 x 2 blocks, CCITT G4 coded, in hex.
constexpr char kCheckerG4[] =
    "2f97cbe5f0bff8410410410410417ff86104104104105ffc20820820820820bf"
    "fc3082082082082ffe002002";

// The same checkerboard as a JBIG2 generic region coded with MMR, in hex.
constexpr char kCheckerJbig2[] =
    "0000000030000100000013000000140000000a00000000000000000000000000"
    "00012700010000003e000000140000000a000000000000000000012f97cbe5f0"
    "bff8410410410410417ff86104104104105ffc20820820820820bffc30820820"
    "82082ffe0020020000000231000100000000";

// A 40 x 20 gray baseline JPEG (JFIF), in hex.
constexpr char kGrayJpeg[] =
    "FFD8FFE000104A46494600010100000100010000FFDB00430008060607060508"
    "0707070909080A0C140D0C0B0B0C1912130F141D1A1F1E1D1A1C1C20242E2720"
    "222C231C1C2837292C30313434341F27393D38323C2E333432FFC0000B080014"
    "002801011100FFC4001F00000105010101010101000000000000000001020304"
    "05060708090A0BFFC400B5100002010303020403050504040000017D01020300"
    "041105122131410613516107227114328191A1082342B1C11552D1F024336272"
    "82090A161718191A25262728292A3435363738393A434445464748494A535455"
    "565758595A636465666768696A737475767778797A838485868788898A929394"
    "95969798999AA2A3A4A5A6A7A8A9AAB2B3B4B5B6B7B8B9BAC2C3C4C5C6C7C8C9"
    "CAD2D3D4D5D6D7D8D9DAE1E2E3E4E5E6E7E8E9EAF1F2F3F4F5F6F7F8F9FAFFDA"
    "0008010100003F00E652DBEC7DB7EEFC318FFF005D4E96DF63EDBF77E18C5584"
    "B6FB1F6DFBBF0C63FF00D75612DBEC7DB7EEFC318FFF005D584B6FB1F6DFBFF0"
    "C62ABA5B7D8FB6FDDF8631FF00EBAB096DF63EDBF77E18C7FF00AEAC25B7D8FB"
    "6FDDF863153A5B7D8FB6FDDF86315612DBEC7DB7EEFC318FFF005D54D3D17E7E"
    "3D2AC69F1AFCFC7A558D3E35F9F8F4A9F4F45F9F8F4FEB5634F8D7E7E3D2BFFF"
    "D9";

// A 40 x 20 RGB JPEG, YCbCr 4:2:0 (JFIF), in hex.
constexpr char kColorJpeg[] =
    "FFD8FFE000104A46494600010100000100010000FFDB00430008060607060508"
    "0707070909080A0C140D0C0B0B0C1912130F141D1A1F1E1D1A1C1C20242E2720"
    "222C231C1C2837292C30313434341F27393D38323C2E333432FFDB0043010809"
    "090C0B0C180D0D1832211C213232323232323232323232323232323232323232"
    "323232323232323232323232323232323232323232323232323232323232FFC0"
    "0011080014002803012200021101031101FFC4001F0000010501010101010100"
    "000000000000000102030405060708090A0BFFC400B510000201030302040305"
    "0504040000017D01020300041105122131410613516107227114328191A10823"
    "42B1C11552D1F02433627282090A161718191A25262728292A3435363738393A"
    "434445464748494A535455565758595A636465666768696A737475767778797A"
    "838485868788898A92939495969798999AA2A3A4A5A6A7A8A9AAB2B3B4B5B6B7"
    "B8B9BAC2C3C4C5C6C7C8C9CAD2D3D4D5D6D7D8D9DAE1E2E3E4E5E6E7E8E9EAF1"
    "F2F3F4F5F6F7F8F9FAFFC4001F01000301010101010101010100000000000001"
    "02030405060708090A0BFFC400B5110002010204040304070504040001027700"
    "0102031104052131061241510761711322328108144291A1B1C109233352F015"
    "6272D10A162434E125F11718191A262728292A35363738393A43444546474849"
    "4A535455565758595A636465666768696A737475767778797A82838485868788"
    "898A92939495969798999AA2A3A4A5A6A7A8A9AAB2B3B4B5B6B7B8B9BAC2C3C4"
    "C5C6C7C8C9CAD2D3D4D5D6D7D8D9DAE2E3E4E5E6E7E8E9EAF2F3F4F5F6F7F8F9"
    "FAFFDA000C03010002110311003F00E652DBEC7DB7EEFC318FFF005D4E96DF63"
    "EDBF77E18C56825B7D8FB6FDDF8631FF00EBAB096DF63EDBF77E18C7FF00AEBD"
    "FAF5CCF0D88B795BF0FF003B99E96DF63EDBF77E18C7FF00AEAC25B7D8FB6FDD"
    "F8631FFEBAD04B6FB1F6DFBBF0C62A74B6FB1F6DFBBF0C62BC4AF5CFA3C3626D"
    "E56FC3FCEE504B6FB1F6DFBFF0C628AD74B6FB1F6DFBBF0C63FF00D7457913AF"
    "AEE7BB4F17CB1B7372F96E54D3D17E7E3D2AC69F1AFCFC7A51457BF59B3F28C1"
    "37FBBF99634F8D7E7E3D2A7D3D17E7E3D3FAD14578B59B3E8B04DFEEFE658D3E"
    "35F9F8F4A28A2BC96F53E9708DFB147FFFD9";

// A 40 x 20 CMYK JPEG with an Adobe marker, stored inverted (read with
// /Decode [1 0 1 0 1 0 1 0], as Photoshop writes them), in hex.
constexpr char kAdobeCmykJpeg[] =
    "FFD8FFEE000E41646F626500640000000000FFDB004300080606070605080707"
    "070909080A0C140D0C0B0B0C1912130F141D1A1F1E1D1A1C1C20242E2720222C"
    "231C1C2837292C30313434341F27393D38323C2E333432FFC000140800140028"
    "044311004D11005911004B1100FFC4001F000001050101010101010000000000"
    "0000000102030405060708090A0BFFC400B51000020103030204030505040400"
    "00017D01020300041105122131410613516107227114328191A1082342B1C115"
    "52D1F02433627282090A161718191A25262728292A3435363738393A43444546"
    "4748494A535455565758595A636465666768696A737475767778797A83848586"
    "8788898A92939495969798999AA2A3A4A5A6A7A8A9AAB2B3B4B5B6B7B8B9BAC2"
    "C3C4C5C6C7C8C9CAD2D3D4D5D6D7D8D9DAE1E2E3E4E5E6E7E8E9EAF1F2F3F4F5"
    "F6F7F8F9FAFFDA000E0443004D0059004B00003F00E4ECFF00E29EC7FCBC79FF"
    "00F00DBB7F3CF5AE33EC1ED5D44DFF0012FF00FA69BFF0C63FFD75EFF5D059FF"
    "00C53D8FF978F3FF00E01B76FE79EB47D83DAB3E6FF897FF00D34DFF008631FF"
    "00EBA2BA0B3FF8A7B1FF002F1E7FFC036EDFCF3D68FB07B567CDFF0012FF00FA"
    "69BFF0C63FFD74574167FF0014F63FE5E3CFFF00806DDBF9E7AD1F60F6ACF9BF"
    "E25FFF004D37FE18C7FF00AE8AE82CFF00E29EC7FCBC79FF00F00DBB7F3CF5A3"
    "EC1ED59F37FC4BFF00E9A6FF00C318FF00F5D15F3FD9FF00C53D8FF978F3FF00"
    "E01B76FE79EB5D47D83DABD026FF00897FFD34DFF8631FFEBA2BA0B3FF008A7B"
    "1FF2F1E7FF00C036EDFCF3D68FB07B567CDFF12FFF00A69BFF000C63FF00D745"
    "74167FF14F63FE5E3CFF00F806DDBF9E7AD1F60F6ACF9BFE25FF00F4D37FE18C"
    "7FFAE8AE82CFFE29EC7FCBC79FFF0000DBB7F3CF5A3EC1ED59F37FC4BFFE9A6F"
    "FC318FFF005D15D059FF00C53D8FF978F3FF00E01B76FE79EB47D83DAB3E6FF8"
    "97FF00D34DFF008631FF00EBA2BE7FF07FFCBCFF00C03FAD745E4A7A57A059FF"
    "00CB5FC3FAD15D0783FF00E5E7FE01FD68F253D2B3ECFF00E5AFE1FD68AE83C1"
    "FF00F2F3FF0000FEB47929E959F67FF2D7F0FEB45741E0FF00F979FF00807F5A"
    "3C94F4ACFB3FF96BF87F5A2BA0F07FFCBCFF00C03FAD1E4A7A567D9FFCB5FC3F"
    "AD15FFD9";

// A 40 x 20 gray progressive JPEG carrying an EXIF segment whose description
// reads EXIFSECRET, in hex.
constexpr char kProgressiveExifJpeg[] =
    "FFD8FFE000104A46494600010100000100010000FFE1003A4578696600004D4D"
    "002A000000080001010E0002000000170000001A000000004558494653454352"
    "4554206465736372697074696F6E0000FFDB0043000806060706050807070709"
    "09080A0C140D0C0B0B0C1912130F141D1A1F1E1D1A1C1C20242E2720222C231C"
    "1C2837292C30313434341F27393D38323C2E333432FFC2000B08001400280101"
    "1100FFC400160001010100000000000000000000000000030205FFDA00080101"
    "00000001CCB4443444414444FFC4001710010101010000000000000000000000"
    "0012000301FFDA000801010001050222222222222222222222CECF967CB3B3E5"
    "FFC40014100100000000000000000000000000000030FFDA0008010100063F02"
    "7FFFC4001810000301010000000000000000000000000100117121FFDA000801"
    "0100013F211A546951A546951A546951A546951A5469501D41D41D4075075FFF"
    "DA00080101000000107F39FFC4001A1000030101010100000000000000000000"
    "110051A1F141C1FFDA0008010100013F109B203ACD900CD901D66C80EB366033"
    "640759B203ACD900CD900CD901D7C58F831F063E2CFAF831FFD9";

// A 40 x 20 gray JPEG 2000, in hex.
constexpr char kGrayJpx[] =
    "0000000C6A5020200D0A870A00000014667479706A703220000000006A703220"
    "0000002D6A703268000000166968647200000014000000280001070700000000"
    "000F636F6C7201000000000011000000B46A703263FF4FFF5100290000000000"
    "2800000014000000000000000000000028000000140000000000000000000107"
    "0101FF52000C00000001000404040000FF5C001D426F206F006F006EE0675067"
    "50676850055005504757D357D35762FF64002500014372656174656420627920"
    "4F70656E4A5045472076657273696F6E20322E352E32FF90000A000000000029"
    "0001FF93C3E90E07CC180D6C9A2F80901880075A0519C1C280182C37D40F80FF"
    "D9";

// A 40 x 20 RGB JPEG 2000, in hex.
constexpr char kRgbJpx[] =
    "0000000C6A5020200D0A870A00000014667479706A703220000000006A703220"
    "0000002D6A703268000000166968647200000014000000280003070700000000"
    "000F636F6C7201000000000010000000EC6A703263FF4FFF51002F0000000000"
    "2800000014000000000000000000000028000000140000000000000000000307"
    "0101070101070101FF52000C00000001000404040000FF5C001D426F206F006F"
    "006EE067506750676850055005504757D357D35762FF64002500014372656174"
    "6564206279204F70656E4A5045472076657273696F6E20322E352E32FF90000A"
    "00000000005B0001FF93C3E30A08F3103ECBC3E6080694B0B0C3E90C0250D63C"
    "B0B0C04200028080C0E1507C028003EC7E3D6F075A05701E8080C32211C15012"
    "B91FAB89E72CC910BBA910961650D538144B8080808080FFD9";

// A 40 x 20 RGB JPEG 2000 with its own alpha: opaque on the left half,
// transparent on the right (read with /SMaskInData 1), in hex.
constexpr char kRgbaJpx[] =
    "0000000C6A5020200D0A870A00000014667479706A703220000000006A703220"
    "0000004F6A703268000000166968647200000014000000280004070700000000"
    "000F636F6C720100000000001000000022636465660004000000000001000100"
    "0000020002000000030003000100000000012D6A703263FF4FFF510032000000"
    "0000280000001400000000000000000000002800000014000000000000000000"
    "04070101070101070101070101FF52000C00000001000404040000FF5C001D42"
    "6F206F006F006EE067506750676850055005504757D357D35762FF6400250001"
    "43726561746564206279204F70656E4A5045472076657273696F6E20322E352E"
    "32FF90000A0000000000990001FF93C3E30A08F3103ECBC3E6080694B0B0C3E9"
    "0C0250D63CB0B0CFA430023A4193EC01C04200028080C3E60A000E04FD678DC0"
    "E1604507C02803EC7E3D6F050B075A05701E8080C1C280168FB5E102C3C5C3C5"
    "415012B91FAB89E72CC933607910BBA92281E512A8CBE410961650D538144B80"
    "C12012B91FD5CF9838273AEC103F6483808080C7080040E0C62CFFD9";

// A 300 x 300 page drawing `image` (an image's stream object) at x 50..250,
// y 100..200 after `before`, with `more` objects from 6 on. Rendered at
// 600 x 600 the image is x 100..500, y 200..400: a sample of a 20 x 10 image
// is 20 pixels, and a mark over x 50..90 covers its first four columns.
std::string ImagePage(const std::string& image,
                      const std::string& before = "",
                      const std::vector<std::string>& more = {}) {
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R "
      "/Resources << /XObject << /Im 5 0 R >> >> >>",
      StreamObject("", before + " q 200 0 0 100 50 100 cm /Im Do Q"),
      image,
  };
  objects.insert(objects.end(), more.begin(), more.end());
  return PdfOf(objects);
}

// 20 x 10 samples of `components` components, `bits` bits each, packed into
// rows from the top: component c of sample (x, y) is `sample(x, y, c)`.
std::string Samples(int components,
                    int bits,
                    const std::function<uint32_t(int, int, int)>& sample) {
  const size_t pitch = (20 * components * bits + 7) / 8;
  std::string data(pitch * 10, '\0');
  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 20; ++x) {
      for (int c = 0; c < components; ++c) {
        const uint32_t value = sample(x, y, c);
        const size_t index = static_cast<size_t>(x) * components + c;
        if (bits == 16) {
          data[y * pitch + index * 2] = static_cast<char>(value >> 8);
          data[y * pitch + index * 2 + 1] = static_cast<char>(value);
          continue;
        }
        const size_t bit = index * bits;
        const int shift = 8 - bits - static_cast<int>(bit % 8);
        data[y * pitch + bit / 8] = static_cast<char>(
            static_cast<uint8_t>(data[y * pitch + bit / 8]) | (value << shift));
      }
    }
  }
  return data;
}

std::string Flate(const std::string& data) {
  DataVector<uint8_t> compressed =
      fxcodec::FlateModule::Encode(pdfium::as_byte_span(data));
  return std::string(compressed.begin(), compressed.end());
}

// Whether every channel of `pixel` is at least `floor`.
bool AtLeast(uint32_t pixel, uint32_t floor) {
  return (pixel >> 16 & 0xff) >= floor && (pixel >> 8 & 0xff) >= floor &&
         (pixel & 0xff) >= floor;
}

// Whether no channel of `a` and `b` differs by more than `tolerance`.
bool Near(uint32_t a, uint32_t b, int tolerance) {
  for (int shift : {0, 8, 16}) {
    const int x = static_cast<int>(a >> shift & 0xff);
    const int y = static_cast<int>(b >> shift & 0xff);
    if (std::abs(x - y) > tolerance) {
      return false;
    }
  }
  return true;
}

// The only image page 0 names, whatever its name (content regeneration can
// rename it).
RetainPtr<const CPDF_Stream> OnlyImage(FPDF_DOCUMENT doc) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  CPDF_DocumentViewScope document_view(pdf);
  RetainPtr<const CPDF_Dictionary> xobjects =
      pdf->GetPageDictionary(0)->GetDictFor("Resources")->GetDictFor("XObject");
  std::vector<RetainPtr<const CPDF_Stream>> images;
  CPDF_DictionaryLocker locker(xobjects.Get());
  for (const auto& [name, object] : locker) {
    RetainPtr<const CPDF_Stream> stream = ToStream(object->GetDirect());
    if (stream && stream->GetDict()->GetNameFor("Subtype") == "Image") {
      images.push_back(std::move(stream));
    }
  }
  EXPECT_EQ(1u, images.size());
  return images.empty() ? nullptr : images[0];
}

}  // namespace

class EPDFRedactEmbedderTest : public EmbedderTest {
 protected:
  void ApplyAndSave(FPDF_PAGE page) {
    ASSERT_TRUE(EPDFPage_ApplyRedactions(page, nullptr, 0, nullptr));
    ASSERT_TRUE(FPDFPage_GenerateContent(page));
    // Safe export is caller-selected. Never let an incremental save obscure
    // the distinction between removal defects and deliberately retained
    // history.
    ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_NO_INCREMENTAL));
    OpenSaved();
  }

  // Opens the saved copy, which stays open until the test ends or the next
  // save.
  void OpenSaved() {
    saved_.reset();
    saved_.emplace(OpenScopedSavedDocument());
    ASSERT_TRUE(*saved_);
  }

  // How many streams reachable in a full rewrite of `doc` carry `sentinel`.
  int SentinelsInRewrite(FPDF_DOCUMENT doc, const std::string& sentinel) {
    ClearString();
    if (!FPDF_SaveAsCopy(doc, this, FPDF_NO_INCREMENTAL)) {
      ADD_FAILURE() << "the rewrite failed";
      return -1;
    }
    const std::string bytes = GetString();
    ScopedFPDFDocument saved(
        FPDF_LoadMemDocument(bytes.data(), bytes.size(), nullptr));
    if (!saved) {
      ADD_FAILURE() << "the rewrite does not open";
      return -1;
    }
    return CountSentinel(CPDFDocumentFromFPDFDocument(saved.get()), sentinel);
  }

 private:
  std::optional<ScopedSavedDoc> saved_;
};

TEST_F(EPDFRedactEmbedderTest, NestedFormsUseTheRenderersTransformOrder) {
  ASSERT_TRUE(OpenDocument("redact_nested_forms.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(L"SECRET\r\nPUBLIC", ReadText(page.get()));
  auto before = Render(page.get());

  ApplyAndSave(page.get());
  ScopedSavedPage saved = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"PUBLIC", ReadText(saved.get()));
  auto after = Render(saved.get());
  // Independent, fixture-defined coordinates. Overlays are disabled, so
  // neither extraction nor rendering can hide the surviving nested word.
  for (int y = 440; y < 465; ++y) {
    for (int x = 299; x < 390; ++x) {
      EXPECT_EQ(0xffffffu, Pixel(after.get(), x, y));
    }
  }
  ExpectSamePixels(before.get(), after.get(), 45, 275, 160, 305);
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
}

TEST_F(EPDFRedactEmbedderTest, SharedImageAndMaskPreserveOtherPlacements) {
  ASSERT_TRUE(OpenDocument("redact_shared_image.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  auto before = Render(page.get());
  const uint32_t original = Pixel(before.get(), 275, 500);
  EXPECT_NE(0xffffffu, original);

  ApplyAndSave(page.get());
  ScopedSavedPage saved = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved);
  auto after = Render(saved.get());
  EXPECT_EQ(0xffffffu, Pixel(after.get(), 75, 500));
  ExpectSamePixels(before.get(), after.get(), 100, 450, 150, 550);
  ExpectSamePixels(before.get(), after.get(), 250, 450, 350, 550);

  // This placement was never parsed or rendered before the mutation; caches
  // cannot disguise an overwrite of its shared image or soft-mask stream.
  ScopedSavedPage other = LoadScopedSavedPage(1);
  ASSERT_TRUE(other);
  auto other_bitmap = Render(other.get());
  EXPECT_EQ(original, Pixel(other_bitmap.get(), 75, 500));
  EXPECT_EQ(original, Pixel(other_bitmap.get(), 125, 500));
}

TEST_F(EPDFRedactEmbedderTest, RemovesReplacementTextFromWholeAffectedSpan) {
  ASSERT_TRUE(OpenDocument("redact_actual_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  auto before = Render(page.get());

  ApplyAndSave(page.get());
  ScopedSavedPage saved = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"keep\r\nsibling\r\nPUBLIC", ReadText(saved.get()));
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
  auto after = Render(saved.get());
  ExpectSamePixels(before.get(), after.get(), 135, 475, 200, 505);
  ExpectSamePixels(before.get(), after.get(), 45, 375, 130, 405);
  ExpectSamePixels(before.get(), after.get(), 45, 275, 160, 305);

  // Unaffected sibling structure retains its accessibility text, while the
  // affected element and its aggregate parent lose stale replacement text.
  auto root = CPDFDocumentFromFPDFDocument(saved_document())->GetRoot();
  auto tree = root->GetDictFor("StructTreeRoot");
  auto paragraph = tree->GetArrayFor("K")->GetDictAt(0);
  EXPECT_FALSE(paragraph->KeyExist("ActualText"));
  auto kids = paragraph->GetArrayFor("K");
  EXPECT_FALSE(kids->GetDictAt(0)->KeyExist("ActualText"));
  EXPECT_EQ(L"PUBLIC", kids->GetDictAt(1)->GetUnicodeTextFor("ActualText"));
}

TEST_F(EPDFRedactEmbedderTest,
       DirectReplacementTextIsRemovedFromSiblingObjects) {
  ASSERT_TRUE(OpenDocument("redact_direct_actual_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ApplyAndSave(page.get());
  ScopedSavedPage saved = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"keep\r\nsibling", ReadText(saved.get()));
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
}

TEST_F(EPDFRedactEmbedderTest,
       FullyRemovedSpanLeavesNoPropertyOrStructureResidue) {
  ASSERT_TRUE(OpenDocument("redact_actual_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    const FS_RECTF whole_span = {40, 230, 300, 90};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &whole_span));
  }
  ApplyAndSave(page.get());
  ScopedSavedPage saved = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"PUBLIC", ReadText(saved.get()));
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
}

TEST_F(EPDFRedactEmbedderTest, SharedFormPreservesUnmarkedReplacementText) {
  ASSERT_TRUE(OpenDocument("redact_shared_marked_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  auto before = Render(page.get());
  ApplyAndSave(page.get());
  ScopedSavedPage saved = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved);
  // The unmarked copy is legitimate content. A document-wide forbidden-word
  // assertion would be wrong here, and global property mutation loses it.
  EXPECT_EQ(L"keep SECRET keep", ReadText(saved.get()));
  auto after = Render(saved.get());
  ExpectSamePixels(before.get(), after.get(), 45, 275, 220, 305);
}

TEST_F(EPDFRedactEmbedderTest, SharedImagePlacementsReceiveIndependentRegions) {
  ASSERT_TRUE(OpenDocument("redact_shared_image.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  auto before = Render(page.get());
  const uint32_t original = Pixel(before.get(), 275, 500);
  {
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
    const FS_RECTF other_half = {300, 150, 350, 50};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &other_half));
  }
  ApplyAndSave(page.get());
  ScopedSavedPage saved = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved);
  auto after = Render(saved.get());
  EXPECT_EQ(0xffffffu, Pixel(after.get(), 75, 500));
  EXPECT_EQ(original, Pixel(after.get(), 125, 500));
  EXPECT_EQ(original, Pixel(after.get(), 275, 500));
  EXPECT_EQ(0xffffffu, Pixel(after.get(), 325, 500));
}

TEST_F(EPDFRedactEmbedderTest, InheritedPropertiesLeaveNoUnusedOriginalOnPage) {
  ASSERT_TRUE(OpenDocument("redact_inherited_actual_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ApplyAndSave(page.get());
  ScopedSavedPage saved = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"keep", ReadText(saved.get()));
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
}

// Exercise page-tree inheritance separately from Form resource inheritance.
// Parameters: layer session, direct (rather than indirect) property dictionary.
class EPDFRedactPageTreeTest
    : public EPDFRedactEmbedderTest,
      public testing::WithParamInterface<std::tuple<bool, bool>> {
 protected:
  void OpenInput(bool sibling_uses_property) {
    ASSERT_TRUE(OpenDocument("redact_actual_text.pdf"));
    auto* doc = CPDFDocumentFromFPDFDocument(document());
    auto first = doc->GetMutablePageDictionary(0);
    auto resources = first->GetMutableDictFor("Resources");
    if (std::get<1>(GetParam())) {
      auto properties = resources->GetMutableDictFor("Properties");
      properties->SetFor(
          "Sensitive",
          properties->GetDictFor("Sensitive")->CloneForHolder(doc));
    }

    auto sibling = doc->CreateNewPage(1);
    ASSERT_TRUE(sibling);
    sibling->SetFor("MediaBox", first->GetObjectFor("MediaBox")->Clone());
    if (sibling_uses_property) {
      sibling->SetFor("Contents", first->GetObjectFor("Contents")->Clone());
    } else {
      const ByteString contents("BT /F1 20 Tf 50 300 Td (PUBLIC) Tj ET");
      auto stream = doc->NewIndirect<CPDF_Stream>(contents.unsigned_span());
      sibling->SetNewFor<CPDF_Reference>("Contents", doc, stream->GetObjNum());
    }

    auto root_pages = doc->GetMutableRoot()->GetMutableDictFor("Pages");
    auto branch = doc->NewIndirect<CPDF_Dictionary>();
    branch->SetNewFor<CPDF_Name>("Type", "Pages");
    branch->SetNewFor<CPDF_Number>("Count", 2);
    branch->SetFor("Kids", root_pages->GetArrayFor("Kids")->Clone());
    branch->SetNewFor<CPDF_Reference>("Parent", doc, root_pages->GetObjNum());
    auto kids = doc->New<CPDF_Array>();
    kids->AppendNew<CPDF_Reference>(doc, branch->GetObjNum());
    root_pages->SetFor("Kids", std::move(kids));
    first->SetNewFor<CPDF_Reference>("Parent", doc, branch->GetObjNum());
    sibling->SetNewFor<CPDF_Reference>("Parent", doc, branch->GetObjNum());

    // Both ancestors reference the same resource dictionary. The higher,
    // shadowed copy must not keep an unused secret reachable after rewrite.
    const uint32_t resources_number = doc->AddIndirectObject(resources);
    root_pages->SetNewFor<CPDF_Reference>("Resources", doc, resources_number);
    branch->SetNewFor<CPDF_Reference>("Resources", doc, resources_number);
    first->RemoveFor("Resources");
    sibling->RemoveFor("Resources");

    ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, FPDF_NO_INCREMENTAL));
    input_ = GetString();
    ClearString();
    if (std::get<0>(GetParam())) {
      base_.reset(
          EPDF_LoadMemBaseDocument64(input_.data(), input_.size(), nullptr));
      ASSERT_TRUE(base_);
      EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
      working_.reset(
          EPDFLayer_OpenLayer(base_.get(), nullptr, nullptr, &status));
      EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
    } else {
      working_.reset(
          FPDF_LoadMemDocument64(input_.data(), input_.size(), nullptr));
    }
    ASSERT_TRUE(working_);
  }

  void SaveWorking() {
    ASSERT_TRUE(FPDF_SaveAsCopy(working_.get(), this, FPDF_NO_INCREMENTAL));
    OpenSaved();
  }

  void ExpectOtherLayerUnchanged() {
    if (!base_) {
      return;
    }
    ScopedFPDFDocument other(
        EPDFLayer_OpenLayer(base_.get(), nullptr, nullptr, nullptr));
    ASSERT_TRUE(other);
    ScopedFPDFPage page(FPDF_LoadPage(other.get(), 0));
    ASSERT_TRUE(page);
    EXPECT_NE(std::wstring::npos, ReadText(page.get()).find(L"SECRET"));
    auto* doc = CPDFDocumentFromFPDFDocument(other.get());
    CPDF_DocumentViewScope document_view(doc);
    auto resources =
        doc->GetRoot()->GetDictFor("Pages")->GetDictFor("Resources");
    EXPECT_TRUE(resources->GetDictFor("Properties")->KeyExist("Sensitive"));
  }

  std::string input_;
  std::unique_ptr<std::remove_pointer_t<EPDF_BASE_DOCUMENT>,
                  decltype(&EPDF_ReleaseBaseDocument)>
      base_{nullptr, EPDF_ReleaseBaseDocument};
  ScopedFPDFDocument working_;
};

TEST_P(EPDFRedactPageTreeTest, RemovesUnusedPropertiesFromEveryAncestor) {
  OpenInput(/*sibling_uses_property=*/false);
  ASSERT_TRUE(working_);
  ScopedFPDFPage page(FPDF_LoadPage(working_.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_TRUE(EPDFPage_ApplyRedactions(page.get(), nullptr, 0, nullptr));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  SaveWorking();
  ASSERT_TRUE(saved_document());
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
  ScopedSavedPage saved = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"keep\r\nsibling\r\nPUBLIC", ReadText(saved.get()));
  ScopedSavedPage sibling = LoadScopedSavedPage(1);
  ASSERT_TRUE(sibling);
  EXPECT_EQ(L"PUBLIC", ReadText(sibling.get()));
  ExpectOtherLayerUnchanged();
}

TEST_P(EPDFRedactPageTreeTest, PreservesAnUnmarkedSiblingUsingTheProperty) {
  OpenInput(/*sibling_uses_property=*/true);
  ASSERT_TRUE(working_);
  ScopedFPDFPage page(FPDF_LoadPage(working_.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_TRUE(EPDFPage_ApplyRedactions(page.get(), nullptr, 0, nullptr));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  SaveWorking();
  ASSERT_TRUE(saved_document());
  ScopedSavedPage sibling = LoadScopedSavedPage(1);
  ASSERT_TRUE(sibling);
  EXPECT_EQ(L"SECRET keep sibling\r\nPUBLIC", ReadText(sibling.get()));
  ExpectOtherLayerUnchanged();
}

TEST_P(EPDFRedactPageTreeTest,
       RemovesAncestorPropertyAfterBothPagesAreRedacted) {
  OpenInput(/*sibling_uses_property=*/true);
  ASSERT_TRUE(working_);
  ScopedFPDFPage first(FPDF_LoadPage(working_.get(), 0));
  ScopedFPDFPage second(FPDF_LoadPage(working_.get(), 1));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  {
    ScopedFPDFAnnotation mark(
        FPDFPage_CreateAnnot(second.get(), FPDF_ANNOT_REDACT));
    const FS_RECTF rect = {49, 121, 133, 95};
    ASSERT_TRUE(FPDFAnnot_SetRect(mark.get(), &rect));
  }
  ASSERT_TRUE(EPDFPage_ApplyRedactions(first.get(), nullptr, 0, nullptr));
  ASSERT_TRUE(EPDFPage_ApplyRedactions(second.get(), nullptr, 0, nullptr));
  ASSERT_TRUE(FPDFPage_GenerateContent(first.get()));
  ASSERT_TRUE(FPDFPage_GenerateContent(second.get()));
  SaveWorking();
  ASSERT_TRUE(saved_document());
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
  ExpectOtherLayerUnchanged();
}

INSTANTIATE_TEST_SUITE_P(All,
                         EPDFRedactPageTreeTest,
                         testing::Combine(testing::Bool(), testing::Bool()));

// Stamps can share one drawing. Redacting part of one of them, flattened
// into the page, redacts a copy of the drawing for that placement: the other
// stamp's drawing, and the font dictionary it names, are left as they were,
// though the copy still names the font for the line it keeps.
TEST_F(EPDFRedactEmbedderTest,
       RedactingAFlattenedStampLeavesAStampSharingItsDrawingAlone) {
  const std::string drawing_pdf = MakeTextDrawing({"SENTINELSHARED", "KEEP"});
  ScopedFPDFDocument source(
      FPDF_LoadMemDocument(drawing_pdf.data(), drawing_pdf.size(), nullptr));
  ASSERT_TRUE(source);
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 600, 600));
  ScopedFPDFAnnotation redacted =
      AddStamp(page.get(), source.get(), FS_RECTF{50, 590, 350, 500});
  ScopedFPDFAnnotation kept = AddStampSharingDrawing(
      doc.get(), page.get(), redacted.get(), FS_RECTF{50, 390, 350, 300});
  RetainPtr<const CPDF_Stream> drawing = DrawingOf(kept.get());
  ASSERT_TRUE(drawing);
  ASSERT_EQ(drawing.Get(), DrawingOf(redacted.get()).Get());
  const std::string content = StreamText(drawing.Get());
  RetainPtr<const CPDF_Dictionary> fonts =
      drawing->GetDict()->GetDictFor("Resources")->GetDictFor("Font");
  ASSERT_TRUE(fonts);
  ASSERT_NE(0u, fonts->GetObjNum());

  FPDF_ANNOTATION flattened[] = {redacted.get()};
  ASSERT_EQ(FLATTEN_SUCCESS,
            EPDFPage_FlattenAnnotations(page.get(), flattened, 1,
                                        FLAT_NORMALDISPLAY, nullptr));
  // Flattening writes the page dictionary, not the loaded page: load it
  // again to see the stamp in its content.
  redacted.reset();
  kept.reset();
  page.reset(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
  kept.reset(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_EQ(drawing.Get(), DrawingOf(kept.get()).Get());
  ASSERT_NE(std::wstring::npos, ReadText(page.get()).find(L"SENTINELSHARED"));
  {
    // The first line only.
    ScopedFPDFAnnotation redact(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
    const FS_RECTF area = {40, 535, 360, 490};
    ASSERT_TRUE(FPDFAnnot_SetRect(redact.get(), &area));
  }
  ASSERT_TRUE(EPDFPage_ApplyRedactions(page.get(), nullptr, 0, nullptr));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  const std::wstring text = ReadText(page.get());
  EXPECT_EQ(std::wstring::npos, text.find(L"SENTINELSHARED"));
  EXPECT_NE(std::wstring::npos, text.find(L"KEEP"));
  EXPECT_EQ(drawing.Get(), DrawingOf(kept.get()).Get());
  EXPECT_EQ(content, StreamText(drawing.Get()));
  // No names added, none pruned.
  EXPECT_EQ(1u, fonts->size());
  EXPECT_TRUE(fonts->KeyExist("F1"));

  // A full rewrite keeps the drawing once: for the stamp that still shows it.
  EXPECT_EQ(1, SentinelsInRewrite(doc.get(), "SENTINELSHARED"));
}

// The only stamp with a drawing, redacted as a live annotation or after it
// was flattened into the page: a full rewrite keeps nothing of the drawing.
TEST_F(EPDFRedactEmbedderTest, RedactingTheOnlyStampLeavesNothingOfItsDrawing) {
  for (bool flatten : {false, true}) {
    SCOPED_TRACE(flatten ? "flattened" : "live");
    const std::string drawing_pdf = MakeTextDrawing({"SENTINELONLY"});
    ScopedFPDFDocument source(
        FPDF_LoadMemDocument(drawing_pdf.data(), drawing_pdf.size(), nullptr));
    ASSERT_TRUE(source);
    ScopedFPDFDocument doc(FPDF_CreateNewDocument());
    ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 600, 600));
    ScopedFPDFAnnotation stamp =
        AddStamp(page.get(), source.get(), FS_RECTF{50, 550, 350, 500});
    if (flatten) {
      FPDF_ANNOTATION flattened[] = {stamp.get()};
      ASSERT_EQ(FLATTEN_SUCCESS,
                EPDFPage_FlattenAnnotations(page.get(), flattened, 1,
                                            FLAT_NORMALDISPLAY, nullptr));
      stamp.reset();
      page.reset(FPDF_LoadPage(doc.get(), 0));
      ASSERT_TRUE(page);
      ASSERT_NE(std::wstring::npos, ReadText(page.get()).find(L"SENTINELONLY"));
    }
    stamp.reset();
    ASSERT_EQ(1, SentinelsInRewrite(doc.get(), "SENTINELONLY"));
    {
      ScopedFPDFAnnotation redact(
          FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
      const FS_RECTF area = {40, 560, 360, 490};
      ASSERT_TRUE(FPDFAnnot_SetRect(redact.get(), &area));
    }
    ASSERT_TRUE(EPDFPage_ApplyRedactions(page.get(), nullptr, 0, nullptr));
    ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
    EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
    EXPECT_EQ(0, SentinelsInRewrite(doc.get(), "SENTINELONLY"));
  }
}

// A page with neither /Contents nor /Resources, as some generators write a
// blank page. The overlay an apply draws is a form with its own graphics
// state, which the content generator names in /Resources the page doesn't
// have yet. In a layer transaction (every engine job runs in one) creating
// that dictionary is a write, after which the page reads /Resources from its
// dictionary again.
TEST_F(EPDFRedactEmbedderTest, AppliesOnAPageWithoutContentsOrResources) {
  const std::string input = PdfOf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
  });
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer transaction" : "document");
    std::unique_ptr<std::remove_pointer_t<EPDF_BASE_DOCUMENT>,
                    decltype(&EPDF_ReleaseBaseDocument)>
        base(nullptr, EPDF_ReleaseBaseDocument);
    ScopedFPDFDocument doc;
    if (layer) {
      base.reset(
          EPDF_LoadMemBaseDocument64(input.data(), input.size(), nullptr));
      ASSERT_TRUE(base);
      doc.reset(EPDFLayer_OpenLayer(base.get(), nullptr, nullptr, nullptr));
    } else {
      doc.reset(FPDF_LoadMemDocument64(input.data(), input.size(), nullptr));
    }
    ASSERT_TRUE(doc);
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
    ASSERT_TRUE(page);
    {
      ScopedFPDFAnnotation mark(
          FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
      ASSERT_TRUE(mark);
      const FS_RECTF rect = {20, 150, 60, 120};
      ASSERT_TRUE(FPDFAnnot_SetRect(mark.get(), &rect));
      ASSERT_TRUE(FPDFAnnot_SetColor(
          mark.get(), FPDFANNOT_COLORTYPE_InteriorColor, 0, 0, 0, 255));
    }
    if (layer) {
      ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.get()));
    }
    ASSERT_TRUE(EPDFPage_ApplyRedactions(page.get(), nullptr, 0, nullptr));
    ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
    if (layer) {
      ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.get()));
    }
    EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
    {
      CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc.get());
      CPDF_DocumentViewScope document_view(pdf);
      RetainPtr<const CPDF_Dictionary> resources =
          pdf->GetPageDictionary(0)->GetDictFor("Resources");
      ASSERT_TRUE(resources);
      RetainPtr<const CPDF_Dictionary> forms = resources->GetDictFor("XObject");
      ASSERT_TRUE(forms);
      EXPECT_EQ(1u, forms->size());
    }

    // Black where the mark was, before and after a full rewrite: 600 pixels
    // for 300 points puts the mark at x 40..120, y 300..360.
    auto after = Render(page.get());
    EXPECT_EQ(0x000000u, Pixel(after.get(), 80, 330));
    EXPECT_EQ(0xffffffu, Pixel(after.get(), 200, 100));
    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(doc.get(), this, FPDF_NO_INCREMENTAL));
    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    ScopedSavedPage saved = LoadScopedSavedPage(0);
    ASSERT_TRUE(saved);
    auto reloaded = Render(saved.get());
    EXPECT_EQ(0x000000u, Pixel(reloaded.get(), 80, 330));
    EXPECT_EQ(0xffffffu, Pixel(reloaded.get(), 200, 100));
  }
}

TEST_F(EPDFRedactEmbedderTest,
       RemovesTheThumbnailAndOtherCopiesOfARedactedPage) {
  const std::string xmp =
      "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
      "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description rdf:about=\"\" "
      "xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" "
      "xmlns:xmpGImg=\"http://ns.adobe.com/xap/1.0/g/img/\" "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
      "<dc:format>KEEPFORMAT</dc:format>"
      "<xmp:Thumbnails><rdf:Alt><rdf:li rdf:parseType=\"Resource\">"
      "<xmpGImg:image>XMPTHUMBSECRET</xmpGImg:image>"
      "</rdf:li></rdf:Alt></xmp:Thumbnails>"
      "</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>";
  const std::string input = PdfOf({
      "<< /Type /Catalog /Pages 2 0 R /Metadata 9 0 R >>",
      "<< /Type /Pages /Kids [3 0 R 8 0 R] /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R "
      "/Resources << /Font << /F1 5 0 R >> >> /Thumb 6 0 R /PieceInfo << "
      "/App << /LastModified (D:20260101) /Private 7 0 R >> >> "
      "/Metadata 10 0 R >>",
      StreamObject("", "BT /F1 24 Tf 20 150 Td (SECRET) Tj ET"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      StreamObject(
          "/Width 11 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8",
          "THUMBSECRET"),
      StreamObject("", "PIECESECRET"),
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Thumb 11 0 R >>",
      StreamObject("/Type /Metadata /Subtype /XML", xmp),
      StreamObject("/Type /Metadata /Subtype /XML", "PAGEMETASECRET"),
      StreamObject(
          "/Width 9 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8",
          "KEEPTHUMB"),
  });
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer transaction" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    {
      ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
      ASSERT_TRUE(page);
      ASSERT_NO_FATAL_FAILURE(MarkAndApply(opened.doc.get(), page.get(), layer,
                                           {15, 180, 150, 140}));
    }
    {
      CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(opened.doc.get());
      CPDF_DocumentViewScope document_view(pdf);
      RetainPtr<const CPDF_Dictionary> redacted = pdf->GetPageDictionary(0);
      EXPECT_FALSE(redacted->KeyExist("Thumb"));
      EXPECT_FALSE(redacted->KeyExist("PieceInfo"));
      EXPECT_FALSE(redacted->KeyExist("Metadata"));
      // A page no mark was applied on keeps its thumbnail.
      EXPECT_TRUE(pdf->GetPageDictionary(1)->KeyExist("Thumb"));
      const std::string metadata =
          StreamText(pdf->GetRoot()->GetStreamFor("Metadata").Get());
      EXPECT_NE(std::string::npos, metadata.find("KEEPFORMAT"));
      EXPECT_EQ(std::string::npos, metadata.find("Thumbnails"));
    }

    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_NO_INCREMENTAL));
    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    CPDF_Document* saved = CPDFDocumentFromFPDFDocument(saved_document());
    for (const char* sentinel :
         {"THUMBSECRET", "PIECESECRET", "PAGEMETASECRET", "XMPTHUMBSECRET"}) {
      ExpectNoSentinel(saved, sentinel);
    }
    EXPECT_EQ(1, CountSentinel(saved, "KEEPTHUMB"));
  }
}

TEST_F(EPDFRedactEmbedderTest, TheCopyOfARedactedFormHoldsNoOtherCopiesOfIt) {
  const std::string input = PdfOf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 5 0 R "
      "/Resources << /XObject << /Fm 4 0 R >> >> >>",
      StreamObject("/Type /XObject /Subtype /Form /BBox [0 0 300 300] "
                   "/Resources << /Font << /F1 6 0 R >> >> /PieceInfo << /App "
                   "<< /LastModified (D:20260101) /Private 7 0 R >> >> "
                   "/Metadata 8 0 R /OPI << /2.0 << /Type /OPI /Version 2.0 "
                   "/F (OPISECRET.tif) /Inks /full_color >> >>",
                   "BT /F1 24 Tf 20 150 Td (SECRET) Tj ET"),
      StreamObject("", "/Fm Do"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      StreamObject("", "FORMPIECESECRET"),
      StreamObject("/Type /Metadata /Subtype /XML", "FORMMETASECRET"),
  });
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer transaction" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    {
      ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
      ASSERT_TRUE(page);
      ASSERT_NO_FATAL_FAILURE(MarkAndApply(opened.doc.get(), page.get(), layer,
                                           {15, 180, 150, 140}));
    }

    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_NO_INCREMENTAL));
    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    CPDF_Document* saved = CPDFDocumentFromFPDFDocument(saved_document());
    // The page still draws a form: the redacted copy.
    RetainPtr<const CPDF_Dictionary> xobjects = saved->GetPageDictionary(0)
                                                    ->GetDictFor("Resources")
                                                    ->GetDictFor("XObject");
    ASSERT_TRUE(xobjects);
    EXPECT_GE(xobjects->size(), 1u);
    for (const char* sentinel :
         {"SECRET", "FORMPIECESECRET", "FORMMETASECRET", "OPISECRET"}) {
      ExpectNoSentinel(saved, sentinel);
    }
  }
}

TEST_F(EPDFRedactEmbedderTest, ARedactedImageKeepsItsLayerAndHowItIsDrawnOnly) {
  const std::string dark(4, '\x28');
  const std::string input = PdfOf({
      "<< /Type /Catalog /Pages 2 0 R /OCProperties << /OCGs [6 0 R] "
      "/D << /OFF [6 0 R] /Order [6 0 R] >> >> >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 5 0 R "
      "/Resources << /XObject << /Hidden 4 0 R /Shown 9 0 R >> >> >>",
      StreamObject(
          "/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace "
          "/DeviceGray /BitsPerComponent 8 /OC 6 0 R /Intent /Perceptual "
          "/Interpolate true /StructParent 3 /Name /Hidden /Metadata 7 0 R "
          "/Alternates [<< /Image 8 0 R >>] /OPI << /1.3 << /Type /OPI "
          "/Version 1.3 /F (OPISECRET.tif) /Size [2 2] /CropRect [0 0 2 2] "
          "/Position [0 0 0 2 2 2 2 0] >> >>",
          dark),
      StreamObject("",
                   "q 200 0 0 100 50 150 cm /Hidden Do Q "
                   "q 200 0 0 50 50 30 cm /Shown Do Q"),
      "<< /Type /OCG /Name (Hidden) >>",
      StreamObject("/Type /Metadata /Subtype /XML", "IMAGEMETASECRET"),
      StreamObject("/Type /XObject /Subtype /Image /Width 3 /Height 3 "
                   "/ColorSpace /DeviceGray /BitsPerComponent 8",
                   "ALTSECRET"),
      StreamObject("/Type /XObject /Subtype /Image /Width 2 /Height 2 "
                   "/ColorSpace /DeviceGray /BitsPerComponent 8",
                   dark),
  });
  // 600 pixels for 300 points: the hidden image's right part is around
  // (400, 200), the shown image around (300, 490).
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer transaction" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
    ASSERT_TRUE(page);
    auto before = Render(page.get());
    EXPECT_EQ(0xffffffu, Pixel(before.get(), 400, 200));
    EXPECT_NE(0xffffffu, Pixel(before.get(), 300, 490));

    ASSERT_NO_FATAL_FAILURE(
        MarkAndApply(opened.doc.get(), page.get(), layer, {50, 250, 90, 150}));
    auto after = Render(page.get());
    EXPECT_EQ(0xffffffu, Pixel(after.get(), 400, 200));
    EXPECT_NE(0xffffffu, Pixel(after.get(), 300, 490));
    {
      CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(opened.doc.get());
      CPDF_DocumentViewScope document_view(pdf);
      int replacements = 0;
      for (const auto& image : PageImages(pdf, 0)) {
        if (!image->KeyExist("OC")) {
          continue;
        }
        ++replacements;
        EXPECT_EQ("Perceptual", image->GetNameFor("Intent"));
        EXPECT_TRUE(image->GetBooleanFor("Interpolate", false));
        EXPECT_EQ(3, image->GetIntegerFor("StructParent"));
        for (const char* gone : {"Metadata", "Alternates", "OPI", "Name"}) {
          EXPECT_FALSE(image->KeyExist(gone)) << gone;
        }
      }
      EXPECT_EQ(1, replacements);
    }

    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_NO_INCREMENTAL));
    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    CPDF_Document* saved = CPDFDocumentFromFPDFDocument(saved_document());
    for (const char* sentinel : {"IMAGEMETASECRET", "ALTSECRET", "OPISECRET"}) {
      ExpectNoSentinel(saved, sentinel);
    }
    ScopedSavedPage reloaded = LoadScopedSavedPage(0);
    ASSERT_TRUE(reloaded);
    auto saved_render = Render(reloaded.get());
    EXPECT_EQ(0xffffffu, Pixel(saved_render.get(), 400, 200));
  }
}

TEST_F(EPDFRedactEmbedderTest, AnExplicitMaskStillMasksTheReplacementImage) {
  // A 4 x 2 dark image whose mask paints the left half and masks out the
  // right. A mask sample of 0 is painted, unless /Decode is [1 0].
  struct MaskCase {
    const char* name;
    int width;
    char row;
    const char* decode;
  };
  const MaskCase cases[] = {
      {"the image's grid", 4, '\x30', ""},
      {"an inverted /Decode", 4, '\xC0', "/Decode [1 0]"},
      {"a finer grid", 8, '\x0F', ""},
  };
  for (const MaskCase& mask : cases) {
    const int height = mask.width / 2;
    const std::string input = PdfOf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 5 0 R "
        "/Resources << /XObject << /Im 4 0 R >> >> >>",
        StreamObject("/Type /XObject /Subtype /Image /Width 4 /Height 2 "
                     "/ColorSpace /DeviceGray /BitsPerComponent 8 /Mask 6 0 R",
                     std::string(8, '\x28')),
        StreamObject("", "q 200 0 0 100 50 100 cm /Im Do Q"),
        StreamObject("/Type /XObject /Subtype /Image /Width " +
                         std::to_string(mask.width) + " /Height " +
                         std::to_string(height) +
                         " /ImageMask true /BitsPerComponent 1 " + mask.decode,
                     std::string(height, mask.row)),
    });
    for (bool layer : {false, true}) {
      SCOPED_TRACE(std::string(mask.name) +
                   (layer ? ", layer transaction" : ", document"));
      OpenedPdf opened = OpenPdf(input, layer);
      ASSERT_TRUE(opened.doc);
      ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
      ASSERT_TRUE(page);
      // 600 pixels for 300 points, the image at x 100..500, y 200..400: its
      // marked column x 100..200, painted x 200..300, masked out x 300..500.
      auto before = Render(page.get());
      EXPECT_NE(0xffffffu, Pixel(before.get(), 240, 300));
      EXPECT_EQ(0xffffffu, Pixel(before.get(), 400, 300));

      ASSERT_NO_FATAL_FAILURE(MarkAndApply(opened.doc.get(), page.get(), layer,
                                           {50, 200, 90, 100}));
      auto after = Render(page.get());
      EXPECT_EQ(0xffffffu, Pixel(after.get(), 140, 300));
      EXPECT_NE(0xffffffu, Pixel(after.get(), 240, 300));
      EXPECT_EQ(0xffffffu, Pixel(after.get(), 400, 300));

      ClearString();
      ASSERT_TRUE(FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_NO_INCREMENTAL));
      ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
      ASSERT_TRUE(saved_doc);
      ScopedSavedPage reloaded = LoadScopedSavedPage(0);
      ASSERT_TRUE(reloaded);
      auto saved_render = Render(reloaded.get());
      EXPECT_EQ(0xffffffu, Pixel(saved_render.get(), 140, 300));
      EXPECT_NE(0xffffffu, Pixel(saved_render.get(), 240, 300));
      EXPECT_EQ(0xffffffu, Pixel(saved_render.get(), 400, 300));
    }
  }
}

TEST_F(EPDFRedactEmbedderTest, RedactingByQuadsRemovesThePageThumbnailToo) {
  const std::string input = PdfOf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R "
      "/Resources << /Font << /F1 5 0 R >> >> /Thumb 6 0 R >>",
      StreamObject("", "BT /F1 24 Tf 20 150 Td (SECRET) Tj ET"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      StreamObject(
          "/Width 11 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8",
          "THUMBSECRET"),
  });
  OpenedPdf opened = OpenPdf(input, /*layer=*/false);
  ASSERT_TRUE(opened.doc);
  {
    ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
    ASSERT_TRUE(page);
    const FS_QUADPOINTSF quad = {15, 180, 150, 180, 15, 140, 150, 140};
    ASSERT_TRUE(EPDFText_RedactInQuads(page.get(), &quad, 1, true, false));
    ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  }
  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_NO_INCREMENTAL));
  ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_doc);
  ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved_document()), "SECRET");
}

// The page paints through a soft mask whose group shows B of the page's Type3
// font; the text in A is redacted, so the group is copied to draw a copy of
// the font without A. The group carries /Metadata and /PieceInfo, and B's
// glyph /Metadata: none of them may come along, whether or not B's resources
// change (an unused resource is dropped from its copy; none, or an empty
// dictionary, changes nothing).
TEST_F(EPDFRedactEmbedderTest, CopiesOfAType3FontsContentHoldNoOtherCopies) {
  for (const char* glyph_b : {"/Resources << /XObject << /Unused 8 0 R >> >> ",
                              "", "/Resources << >> "}) {
    SCOPED_TRACE(std::string("glyph B: ") + glyph_b);
    const std::string input = PdfOf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 600] /Contents 4 0 R "
        "/Resources << /Font << /I 5 0 R >> /ExtGState << /GP << /Type "
        "/ExtGState /SMask << /Type /Mask /S /Luminosity /G 9 0 R >> >> >> >> "
        ">>",
        StreamObject("",
                     "BT /I 72 Tf 100 500 Td (A) Tj ET\n"
                     "q /GP gs 0 0 1 rg 300 300 100 100 re f Q"),
        "<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] "
        "/FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << /a 6 0 R /b 7 0 R >> "
        "/Encoding << /Type /Encoding /Differences [65 /a /b] >> /FirstChar 65 "
        "/LastChar 66 /Widths [1100 1100] /Resources << /XObject << /ImA 8 0 R "
        ">> >> >>",
        StreamObject("", "1100 0 d0 q 1000 0 0 1000 0 0 cm /ImA Do Q"),
        StreamObject(std::string(glyph_b) + "/Metadata 12 0 R",
                     "1100 0 d0 0 1 0 rg 0 0 1000 1000 re f"),
        StreamObject("/Type /XObject /Subtype /Form /BBox [0 0 1 1]",
                     "0 0 1 rg 0 0 1 1 re f"),
        StreamObject(
            "/Type /XObject /Subtype /Form /Group << /S /Transparency "
            "/CS /DeviceRGB >> /BBox [0 0 600 600] /Resources << /Font "
            "<< /In 5 0 R >> >> /Metadata 10 0 R /PieceInfo << /App << "
            "/LastModified (D:20260101) /Private 11 0 R >> >>",
            "BT /In 100 Tf 300 300 Td (B) Tj ET"),
        StreamObject("/Type /Metadata /Subtype /XML", "NESTEDPRIVATE metadata"),
        StreamObject("", "NESTEDPRIVATE piece"),
        StreamObject("/Type /Metadata /Subtype /XML", "GLYPHPRIVATE"),
    });
    for (bool layer : {false, true}) {
      SCOPED_TRACE(layer ? "layer transaction" : "document");
      OpenedPdf opened = OpenPdf(input, layer);
      ASSERT_TRUE(opened.doc);
      ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
      ASSERT_TRUE(page);
      // 600 pixels for 600 points: the masked square around (350, 250).
      auto before = Render(page.get());
      EXPECT_NE(0xffffffu, Pixel(before.get(), 350, 250));

      ASSERT_NO_FATAL_FAILURE(MarkAndApply(opened.doc.get(), page.get(), layer,
                                           {98, 574, 181, 498}));
      auto after = Render(page.get());
      EXPECT_EQ(Pixel(before.get(), 350, 250), Pixel(after.get(), 350, 250));

      ClearString();
      ASSERT_TRUE(FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_NO_INCREMENTAL));
      ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
      ASSERT_TRUE(saved_doc);
      CPDF_Document* saved = CPDFDocumentFromFPDFDocument(saved_document());
      ExpectNoSentinel(saved, "NESTEDPRIVATE");
      ExpectNoSentinel(saved, "GLYPHPRIVATE");
      ScopedSavedPage reloaded = LoadScopedSavedPage(0);
      ASSERT_TRUE(reloaded);
      auto saved_render = Render(reloaded.get());
      EXPECT_EQ(Pixel(before.get(), 350, 250),
                Pixel(saved_render.get(), 350, 250));
    }
  }
}

// A redacted image whose filters leave plain samples keeps them: its colour
// space, bits per component and /Decode stay, it is written with Flate, the
// samples under the mark become paper (white, or no ink) and every other
// sample is the one it was.
TEST_F(EPDFRedactEmbedderTest, ARedactedImageKeepsItsSamplesAndFormat) {
  const std::string gray = "/ColorSpace /DeviceGray /BitsPerComponent ";
  const std::string flate = " /Filter /FlateDecode";
  struct ImageCase {
    const char* name;
    std::string entries;
    std::string data;
  };
  const std::string checker = Samples(1, 1, [](int x, int y, int) {
    return static_cast<uint32_t>((x / 2 + y / 2) % 2);
  });
  const std::string gradient = Samples(1, 8, [](int x, int y, int) {
    return static_cast<uint32_t>(20 + x * 9 + y);
  });
  const ImageCase cases[] = {
      {"gray, 8 bits", gray + "8" + flate, Flate(gradient)},
      {"gray, 1 bit", gray + "1" + flate, Flate(checker)},
      {"gray, 4 bits", gray + "4" + flate,
       Flate(Samples(1, 4, [](int x, int, int) { return x % 12u; }))},
      {"gray, 16 bits", gray + "16" + flate,
       Flate(Samples(1, 16, [](int x, int, int) { return x * 3000u; }))},
      {"RGB", "/ColorSpace /DeviceRGB /BitsPerComponent 8" + flate,
       Flate(Samples(3, 8,
                     [](int x, int, int c) {
                       return c == 0 ? x * 12u : c == 1 ? 50u : 200u;
                     }))},
      {"CMYK", "/ColorSpace /DeviceCMYK /BitsPerComponent 8" + flate,
       Flate(Samples(4, 8,
                     [](int x, int, int c) {
                       return c == 0 ? x * 10u : c == 2 ? 100u : 20u;
                     }))},
      {"Indexed, white at index 2",
       "/ColorSpace [/Indexed /DeviceRGB 3 <000000FF0000FFFFFF00FF00>] "
       "/BitsPerComponent 8" +
           flate,
       Flate(Samples(1, 8, [](int x, int, int) { return x % 2 ? 1u : 3u; }))},
      {"Separation",
       "/ColorSpace [/Separation /Spot /DeviceCMYK << /FunctionType 2 "
       "/Domain [0 1] /C0 [0 0 0 0] /C1 [0 1 1 0] /N 1 >>] "
       "/BitsPerComponent 8" +
           flate,
       Flate(Samples(1, 8, [](int x, int, int) { return 128u + x * 6; }))},
      {"gray, inverted /Decode", gray + "8 /Decode [1 0]" + flate,
       Flate(Samples(1, 8, [](int x, int, int) { return 150u + x * 5; }))},
      {"gray through ASCIIHex", gray + "8 /Filter /ASCIIHexDecode",
       [&] {
         std::string hex;
         for (unsigned char c : gradient) {
           static constexpr char kDigits[] = "0123456789ABCDEF";
           hex += kDigits[c >> 4];
           hex += kDigits[c & 15];
         }
         return hex + ">";
       }()},
      {"CCITT G4",
       gray + "1 /Filter [/ASCIIHexDecode /CCITTFaxDecode] /DecodeParms "
              "[null << /K -1 /Columns 20 /Rows 10 >>]",
       std::string(kCheckerG4) + ">"},
      {"JBIG2", gray + "1 /Filter [/ASCIIHexDecode /JBIG2Decode]",
       std::string(kCheckerJbig2) + ">"},
      {"stencil", "/ImageMask true /BitsPerComponent 1" + flate,
       Flate(checker)},
  };
  for (const ImageCase& image : cases) {
    const std::string input = ImagePage(
        StreamObject("/Type /XObject /Subtype /Image /Width 20 /Height 10 " +
                         image.entries,
                     image.data),
        "1 0 0 rg");
    ScopedFPDFDocument source_doc(
        FPDF_LoadMemDocument64(input.data(), input.size(), nullptr));
    ASSERT_TRUE(source_doc);
    RetainPtr<const CPDF_Stream> source_image = OnlyImage(source_doc.get());
    ASSERT_TRUE(source_image);
    RetainPtr<const CPDF_Dictionary> source = source_image->GetDict();
    for (bool layer : {false, true}) {
      SCOPED_TRACE(std::string(image.name) +
                   (layer ? ", layer transaction" : ", document"));
      OpenedPdf opened = OpenPdf(input, layer);
      ASSERT_TRUE(opened.doc);
      ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
      ASSERT_TRUE(page);
      auto before = Render(page.get());
      ASSERT_NO_FATAL_FAILURE(MarkAndApply(opened.doc.get(), page.get(), layer,
                                           {50, 200, 90, 100}));
      auto after = Render(page.get());
      const std::pair<int, int> hole[] = {{110, 210}, {150, 300}, {130, 390}};
      for (const auto& [x, y] : hole) {
        EXPECT_EQ(0xffffffu, Pixel(after.get(), x, y)) << x << ", " << y;
      }
      ExpectSamePixels(before.get(), after.get(), 200, 200, 500, 400);

      RetainPtr<const CPDF_Stream> kept_image = OnlyImage(opened.doc.get());
      ASSERT_TRUE(kept_image);
      RetainPtr<const CPDF_Dictionary> kept = kept_image->GetDict();
      EXPECT_EQ("FlateDecode", kept->GetNameFor("Filter"));
      EXPECT_FALSE(kept->KeyExist("DecodeParms"));
      for (const char* key : {"BitsPerComponent", "Width", "Height"}) {
        EXPECT_EQ(source->GetIntegerFor(key), kept->GetIntegerFor(key)) << key;
      }
      EXPECT_EQ(source->GetBooleanFor("ImageMask", false),
                kept->GetBooleanFor("ImageMask", false));
      EXPECT_EQ(source->KeyExist("Decode"), kept->KeyExist("Decode"));
      RetainPtr<const CPDF_Object> space =
          source->GetDirectObjectFor("ColorSpace");
      RetainPtr<const CPDF_Object> kept_space =
          kept->GetDirectObjectFor("ColorSpace");
      ASSERT_EQ(!!space, !!kept_space);
      if (space && space->IsName()) {
        EXPECT_EQ(space->GetString(), kept_space->GetString());
      } else if (space) {
        ASSERT_TRUE(kept_space->IsArray());
        EXPECT_EQ(space->AsArray()->GetByteStringAt(0),
                  kept_space->AsArray()->GetByteStringAt(0));
        EXPECT_EQ(space->AsArray()->size(), kept_space->AsArray()->size());
      }

      ClearString();
      ASSERT_TRUE(FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_NO_INCREMENTAL));
      ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
      ASSERT_TRUE(saved_doc);
      ScopedSavedPage reloaded = LoadScopedSavedPage(0);
      ASSERT_TRUE(reloaded);
      auto saved = Render(reloaded.get());
      ExpectSamePixels(after.get(), saved.get(), 100, 200, 500, 400);
    }
  }
}

// Kept samples are the image's own: only those under the mark change.
TEST_F(EPDFRedactEmbedderTest, KeptSamplesChangeOnlyUnderTheMark) {
  const std::string samples = Samples(1, 8, [](int x, int y, int) {
    return static_cast<uint32_t>(20 + x * 9 + y);
  });
  const std::string input = ImagePage(StreamObject(
      "/Type /XObject /Subtype /Image /Width 20 /Height 10 /ColorSpace "
      "/DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
      Flate(samples)));
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer transaction" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
    ASSERT_TRUE(page);
    ASSERT_NO_FATAL_FAILURE(
        MarkAndApply(opened.doc.get(), page.get(), layer, {50, 200, 90, 100}));
    RetainPtr<const CPDF_Stream> kept = OnlyImage(opened.doc.get());
    ASSERT_TRUE(kept);
    std::string expected = samples;
    for (int y = 0; y < 10; ++y) {
      for (int x = 0; x < 4; ++x) {
        expected[y * 20 + x] = static_cast<char>(0xFF);
      }
    }
    EXPECT_EQ(expected, StreamText(kept.Get()));
  }
}

// A soft mask keeps its samples too, opaque under the mark: the hole shows
// as paper, not as what lies under the image.
TEST_F(EPDFRedactEmbedderTest,
       ARedactedImagesSoftMaskIsKeptAndOpaqueInTheHole) {
  const std::string input = ImagePage(
      StreamObject("/Type /XObject /Subtype /Image /Width 20 /Height 10 "
                   "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter "
                   "/FlateDecode /SMask 6 0 R",
                   Flate(std::string(200, '\x28'))),
      "0 0 1 rg 50 100 200 100 re f",
      {StreamObject("/Type /XObject /Subtype /Image /Width 20 /Height 10 "
                    "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter "
                    "/FlateDecode",
                    Flate(Samples(1, 8, [](int x, int, int) {
                      return x < 10 ? 255u : 96u;
                    })))});
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer transaction" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
    ASSERT_TRUE(page);
    auto before = Render(page.get());
    ASSERT_NO_FATAL_FAILURE(
        MarkAndApply(opened.doc.get(), page.get(), layer, {50, 200, 90, 100}));
    auto after = Render(page.get());
    EXPECT_EQ(0xffffffu, Pixel(after.get(), 130, 300));
    ExpectSamePixels(before.get(), after.get(), 200, 200, 500, 400);
    RetainPtr<const CPDF_Stream> kept = OnlyImage(opened.doc.get());
    ASSERT_TRUE(kept);
    CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(opened.doc.get());
    CPDF_DocumentViewScope document_view(pdf);
    RetainPtr<const CPDF_Stream> mask = kept->GetDict()->GetStreamFor("SMask");
    ASSERT_TRUE(mask);
    EXPECT_EQ("FlateDecode", mask->GetDict()->GetNameFor("Filter"));
    EXPECT_EQ("DeviceGray", mask->GetDict()->GetNameFor("ColorSpace"));
  }
}

// A redacted JPEG stays a JPEG. Its blocks away from the mark are its own, so
// it keeps its size and quality; blocks the mark covers become paper, and a
// block its edge crosses only where it covers. No marker of the original but
// the colour transform's comes along.
TEST_F(EPDFRedactEmbedderTest, ARedactedJpegStaysAJpeg) {
  struct JpegCase {
    const char* name;
    std::string entries;
    const char* hex;
    bool progressive;
  };
  const JpegCase cases[] = {
      {"gray", "/ColorSpace /DeviceGray", kGrayJpeg, false},
      {"RGB, 4:2:0",
       "/ColorSpace /DeviceRGB /DecodeParms [null << /ColorTransform 1 >>]",
       kColorJpeg, false},
      {"Adobe CMYK", "/ColorSpace /DeviceCMYK /Decode [1 0 1 0 1 0 1 0]",
       kAdobeCmykJpeg, false},
      {"progressive, EXIF", "/ColorSpace /DeviceGray", kProgressiveExifJpeg,
       true},
  };
  for (const JpegCase& jpeg : cases) {
    const std::string input = ImagePage(StreamObject(
        "/Type /XObject /Subtype /Image /Width 40 /Height 20 "
        "/BitsPerComponent 8 /Filter [/ASCIIHexDecode /DCTDecode] " +
            jpeg.entries,
        std::string(jpeg.hex) + ">"));
    const size_t source_size = strlen(jpeg.hex) / 2;
    for (bool layer : {false, true}) {
      SCOPED_TRACE(std::string(jpeg.name) +
                   (layer ? ", layer transaction" : ", document"));
      OpenedPdf opened = OpenPdf(input, layer);
      ASSERT_TRUE(opened.doc);
      ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
      ASSERT_TRUE(page);
      auto before = Render(page.get());
      // A sample is 10 pixels: the mark covers columns 0..12, so block 0 is
      // under it and block 1 (columns 8..15) partly.
      ASSERT_NO_FATAL_FAILURE(MarkAndApply(opened.doc.get(), page.get(), layer,
                                           {50, 200, 115, 100}));
      auto after = Render(page.get());
      for (int y : {215, 305, 385}) {
        EXPECT_TRUE(AtLeast(Pixel(after.get(), 135, y), 0xf0)) << y;
      }
      EXPECT_TRUE(AtLeast(Pixel(after.get(), 205, 305), 0xe0));
      EXPECT_TRUE(Near(Pixel(before.get(), 245, 305),
                       Pixel(after.get(), 245, 305), 48));
      ExpectSamePixels(before.get(), after.get(), 350, 200, 500, 400);

      RetainPtr<const CPDF_Stream> kept = OnlyImage(opened.doc.get());
      ASSERT_TRUE(kept);
      RetainPtr<const CPDF_Dictionary> dict = kept->GetDict();
      EXPECT_EQ("DCTDecode", dict->GetNameFor("Filter"));
      EXPECT_EQ(8, dict->GetIntegerFor("BitsPerComponent"));
      EXPECT_EQ(jpeg.entries.find("/Decode ") != std::string::npos,
                dict->KeyExist("Decode"));
      if (jpeg.entries.find("ColorTransform") != std::string::npos) {
        RetainPtr<const CPDF_Dictionary> params =
            dict->GetDictFor("DecodeParms");
        ASSERT_TRUE(params);
        EXPECT_EQ(1, params->GetIntegerFor("ColorTransform"));
      }
      const std::string bytes = StreamText(kept.Get());
      EXPECT_LE(bytes.size(), source_size * 5 / 4 + 64);
      EXPECT_EQ(std::string::npos, bytes.find("EXIFSECRET"));
      EXPECT_EQ(std::string::npos, bytes.find("Exif"));
      EXPECT_EQ(jpeg.progressive, bytes.find("\xFF\xC2") != std::string::npos);
      EXPECT_EQ(std::string(jpeg.name) == "Adobe CMYK",
                bytes.find("Adobe") != std::string::npos);

      ClearString();
      ASSERT_TRUE(FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_NO_INCREMENTAL));
      ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
      ASSERT_TRUE(saved_doc);
      ScopedSavedPage reloaded = LoadScopedSavedPage(0);
      ASSERT_TRUE(reloaded);
      auto saved = Render(reloaded.get());
      ExpectSamePixels(after.get(), saved.get(), 100, 200, 500, 400);
    }
  }
}

// A JPEG soft mask stays a JPEG too, on its own grid, opaque in the hole.
TEST_F(EPDFRedactEmbedderTest, AJpegSoftMaskStaysAJpeg) {
  const std::string input = ImagePage(
      StreamObject("/Type /XObject /Subtype /Image /Width 20 /Height 10 "
                   "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter "
                   "/FlateDecode /SMask 6 0 R",
                   Flate(std::string(200, '\x28'))),
      "0 0 1 rg 50 100 200 100 re f",
      {StreamObject("/Type /XObject /Subtype /Image /Width 40 /Height 20 "
                    "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter "
                    "[/ASCIIHexDecode /DCTDecode]",
                    std::string(kGrayJpeg) + ">")});
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer transaction" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
    ASSERT_TRUE(page);
    auto before = Render(page.get());
    ASSERT_NO_FATAL_FAILURE(
        MarkAndApply(opened.doc.get(), page.get(), layer, {50, 200, 90, 100}));
    auto after = Render(page.get());
    EXPECT_TRUE(AtLeast(Pixel(after.get(), 130, 300), 0xf0));
    ExpectSamePixels(before.get(), after.get(), 350, 200, 500, 400);
    RetainPtr<const CPDF_Stream> kept = OnlyImage(opened.doc.get());
    ASSERT_TRUE(kept);
    CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(opened.doc.get());
    CPDF_DocumentViewScope document_view(pdf);
    RetainPtr<const CPDF_Stream> mask = kept->GetDict()->GetStreamFor("SMask");
    ASSERT_TRUE(mask);
    EXPECT_EQ("DCTDecode", mask->GetDict()->GetNameFor("Filter"));
  }
}

// A JPEG libjpeg reads only with a warning is decoded and written from its
// pixels instead, as a JPEG again (it was lossy already). Here three bytes in
// the middle of its coded data become a run of one bits, a Huffman code no
// table has.
TEST_F(EPDFRedactEmbedderTest, AJpegReadWithAWarningIsRewrittenFromItsPixels) {
  std::string hex(kGrayJpeg);
  size_t scan = 0;
  while (hex.compare(scan, 4, "FFDA") != 0) {
    scan += 2;
  }
  const size_t start =
      scan + 4 + std::stoul(hex.substr(scan + 4, 4), nullptr, 16) * 2;
  size_t at = start + (hex.size() - 4 - start) / 4 * 2;
  while (hex.compare(at - 2, 2, "FF") == 0) {
    at += 2;
  }
  hex.replace(at, 6, "FF00FF00FF00");
  const std::string input = ImagePage(StreamObject(
      "/Type /XObject /Subtype /Image /Width 40 /Height 20 /ColorSpace "
      "/DeviceGray /BitsPerComponent 8 /Filter [/ASCIIHexDecode /DCTDecode]",
      hex + ">"));
  OpenedPdf opened = OpenPdf(input, /*layer=*/false);
  ASSERT_TRUE(opened.doc);
  ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_NO_FATAL_FAILURE(
      MarkAndApply(opened.doc.get(), page.get(), false, {50, 200, 115, 100}));
  auto after = Render(page.get());
  EXPECT_TRUE(AtLeast(Pixel(after.get(), 135, 305), 0xf0));
  RetainPtr<const CPDF_Stream> kept = OnlyImage(opened.doc.get());
  ASSERT_TRUE(kept);
  // A new JPEG of what it showed, gray as it was.
  EXPECT_EQ("DCTDecode", kept->GetDict()->GetNameFor("Filter"));
  EXPECT_EQ("DeviceGray", kept->GetDict()->GetNameFor("ColorSpace"));
}

// An image that can't be decoded at all (here: a JPEG cut short) fails the
// apply: nothing is drawn over pixels that could not be removed.
TEST_F(EPDFRedactEmbedderTest, AnImageThatCannotBeDecodedFailsTheApply) {
  std::string hex(kGrayJpeg);
  hex.resize(hex.size() * 3 / 5 / 2 * 2);
  const std::string input = ImagePage(StreamObject(
      "/Type /XObject /Subtype /Image /Width 40 /Height 20 /ColorSpace "
      "/DeviceGray /BitsPerComponent 8 /Filter [/ASCIIHexDecode /DCTDecode]",
      hex + ">"));
  OpenedPdf opened = OpenPdf(input, /*layer=*/false);
  ASSERT_TRUE(opened.doc);
  ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
  ASSERT_TRUE(page);
  {
    ScopedFPDFAnnotation mark(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
    ASSERT_TRUE(mark);
    const FS_RECTF rect = {50, 200, 115, 100};
    ASSERT_TRUE(FPDFAnnot_SetRect(mark.get(), &rect));
  }
  EXPECT_FALSE(EPDFPage_ApplyRedactions(page.get(), nullptr, 0, nullptr));
}

// Marks passed to EPDFPage_ApplyRedactions are applied, in any order; the
// page's other marks stay.
TEST_F(EPDFRedactEmbedderTest, ApplyingSomeMarksLeavesTheOthers) {
  const std::string input = MakeTextDrawing({"AAAA", "BBBB", "CCCC"});
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer transaction" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
    ASSERT_TRUE(page);
    // Line i is drawn from y = 10 + 40 * i.
    std::vector<ScopedFPDFAnnotation> marks;
    for (int line = 0; line < 3; ++line) {
      marks.emplace_back(FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
      ASSERT_TRUE(marks.back());
      const float bottom = 10.0f + 40 * line - 4;
      const FS_RECTF rect = {5, bottom + 30, 150, bottom};
      ASSERT_TRUE(FPDFAnnot_SetRect(marks.back().get(), &rect));
    }
    const FPDF_ANNOTATION applied[] = {marks[2].get(), marks[0].get()};
    if (layer) {
      ASSERT_TRUE(EPDFLayer_BeginTransaction(opened.doc.get()));
    }
    ASSERT_TRUE(EPDFPage_ApplyRedactions(page.get(), applied, 2, nullptr));
    ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
    if (layer) {
      ASSERT_TRUE(EPDFLayer_CommitTransaction(opened.doc.get()));
    }
    marks.clear();
    EXPECT_EQ(L"BBBB", ReadText(page.get()));
    ASSERT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
    ScopedFPDFAnnotation left(FPDFPage_GetAnnot(page.get(), 0));
    EXPECT_EQ(FPDF_ANNOT_REDACT, FPDFAnnot_GetSubtype(left.get()));
  }
}

// A list holding anything but REDACT annotations of the page applies nothing.
TEST_F(EPDFRedactEmbedderTest,
       ApplyingAnythingButAMarkOfThePageAppliesNothing) {
  const std::string input = PdfOf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 50] /Contents 4 0 R "
      "/Resources << /Font << /F1 5 0 R >> >> >>",
      StreamObject("", "BT /F1 24 Tf 10 10 Td (AAAA) Tj ET"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 50] >>",
  });
  OpenedPdf opened = OpenPdf(input, /*layer=*/false);
  ASSERT_TRUE(opened.doc);
  ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
  ScopedFPDFPage other(FPDF_LoadPage(opened.doc.get(), 1));
  ASSERT_TRUE(page);
  ASSERT_TRUE(other);
  const FS_RECTF rect = {5, 40, 150, 5};
  ScopedFPDFAnnotation mark(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
  ScopedFPDFAnnotation square(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_SQUARE));
  ScopedFPDFAnnotation elsewhere(
      FPDFPage_CreateAnnot(other.get(), FPDF_ANNOT_REDACT));
  for (FPDF_ANNOTATION annot : {mark.get(), square.get(), elsewhere.get()}) {
    ASSERT_TRUE(FPDFAnnot_SetRect(annot, &rect));
  }

  const FPDF_ANNOTATION with_square[] = {mark.get(), square.get()};
  EXPECT_FALSE(EPDFPage_ApplyRedactions(page.get(), with_square, 2, nullptr));
  const FPDF_ANNOTATION with_other_page[] = {mark.get(), elsewhere.get()};
  EXPECT_FALSE(
      EPDFPage_ApplyRedactions(page.get(), with_other_page, 2, nullptr));
  EXPECT_FALSE(EPDFPage_ApplyRedactions(page.get(), nullptr, 1, nullptr));
  EXPECT_FALSE(EPDFPage_ApplyRedactions(page.get(), with_square, 0, nullptr));
  EXPECT_FALSE(EPDFPage_ApplyRedactions(page.get(), with_square, -1, nullptr));
  EXPECT_EQ(L"AAAA", ReadText(page.get()));
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));
}

// A mark of another document applies nothing, though the page has a mark
// under the same object number.
TEST_F(EPDFRedactEmbedderTest, AMarkOfAnotherDocumentAppliesNothing) {
  const std::string input = PdfOf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 50] /Contents 4 0 R "
      "/Resources << /Font << /F1 5 0 R >> >> /Annots [6 0 R] >>",
      StreamObject("", "BT /F1 24 Tf 10 10 Td (AAAA) Tj ET"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Type /Annot /Subtype /Redact /Rect [5 5 150 40] >>",
  });
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer transaction" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    OpenedPdf other = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    ASSERT_TRUE(other.doc);
    ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
    ScopedFPDFPage other_page(FPDF_LoadPage(other.doc.get(), 0));
    ASSERT_TRUE(page);
    ASSERT_TRUE(other_page);
    ScopedFPDFAnnotation foreign(FPDFPage_GetAnnot(other_page.get(), 0));
    ASSERT_TRUE(foreign);
    if (layer) {
      ASSERT_TRUE(EPDFLayer_BeginTransaction(opened.doc.get()));
    }
    const FPDF_ANNOTATION marks[] = {foreign.get()};
    EXPECT_FALSE(EPDFPage_ApplyRedactions(page.get(), marks, 1, nullptr));
    if (layer) {
      ASSERT_TRUE(EPDFLayer_CommitTransaction(opened.doc.get()));
    }
    EXPECT_EQ(L"AAAA", ReadText(page.get()));
    EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
    EXPECT_EQ(1, FPDFPage_GetAnnotCount(other_page.get()));
  }
}

// Two marks on one image applied in one call both leave their hole.
TEST_F(EPDFRedactEmbedderTest, MarksOnOneImageApplyInOneCall) {
  const std::string input = ImagePage(StreamObject(
      "/Type /XObject /Subtype /Image /Width 20 /Height 10 /ColorSpace "
      "/DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
      Flate(std::string(200, '\x28'))));
  OpenedPdf opened = OpenPdf(input, /*layer=*/false);
  ASSERT_TRUE(opened.doc);
  ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
  ASSERT_TRUE(page);
  // A sample is 10 points: the marks cover columns 0..3 and 16..19.
  ScopedFPDFAnnotation left(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
  ScopedFPDFAnnotation right(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
  const FS_RECTF left_rect = {50, 200, 90, 100};
  const FS_RECTF right_rect = {210, 200, 250, 100};
  ASSERT_TRUE(FPDFAnnot_SetRect(left.get(), &left_rect));
  ASSERT_TRUE(FPDFAnnot_SetRect(right.get(), &right_rect));
  const FPDF_ANNOTATION marks[] = {left.get(), right.get()};
  ASSERT_TRUE(EPDFPage_ApplyRedactions(page.get(), marks, 2, nullptr));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  auto after = Render(page.get());
  EXPECT_EQ(0xffffffu, Pixel(after.get(), 130, 300));
  EXPECT_EQ(0xffffffu, Pixel(after.get(), 470, 300));
  EXPECT_NE(0xffffffu, Pixel(after.get(), 300, 300));
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
}

// A redacted JPEG 2000 is written as a JPEG of what it showed: gray stays
// gray, RGB stays RGB, compressed when the redaction is applied.
TEST_F(EPDFRedactEmbedderTest, ARedactedJpeg2000IsWrittenAsAJpeg) {
  struct JpxCase {
    const char* name;
    const char* hex;
    const char* color_space;
  };
  const JpxCase cases[] = {
      {"gray", kGrayJpx, "DeviceGray"},
      {"RGB", kRgbJpx, "DeviceRGB"},
  };
  for (const JpxCase& jpx : cases) {
    const std::string input = ImagePage(StreamObject(
        std::string("/Type /XObject /Subtype /Image /Width 40 /Height 20 "
                    "/BitsPerComponent 8 /ColorSpace /") +
            jpx.color_space + " /Filter [/ASCIIHexDecode /JPXDecode]",
        std::string(jpx.hex) + ">"));
    for (bool layer : {false, true}) {
      SCOPED_TRACE(std::string(jpx.name) +
                   (layer ? ", layer transaction" : ", document"));
      OpenedPdf opened = OpenPdf(input, layer);
      ASSERT_TRUE(opened.doc);
      ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
      ASSERT_TRUE(page);
      auto before = Render(page.get());
      ASSERT_NO_FATAL_FAILURE(MarkAndApply(opened.doc.get(), page.get(), layer,
                                           {50, 200, 115, 100}));
      auto after = Render(page.get());
      for (int y : {215, 305, 385}) {
        EXPECT_TRUE(AtLeast(Pixel(after.get(), 135, y), 0xf0)) << y;
      }
      for (int x : {360, 420, 480}) {
        EXPECT_TRUE(
            Near(Pixel(before.get(), x, 300), Pixel(after.get(), x, 300), 40))
            << x;
      }
      RetainPtr<const CPDF_Stream> kept = OnlyImage(opened.doc.get());
      ASSERT_TRUE(kept);
      RetainPtr<const CPDF_Dictionary> dict = kept->GetDict();
      EXPECT_EQ("DCTDecode", dict->GetNameFor("Filter"));
      EXPECT_EQ(jpx.color_space, dict->GetNameFor("ColorSpace"));
      EXPECT_EQ(8, dict->GetIntegerFor("BitsPerComponent"));
      EXPECT_FALSE(dict->KeyExist("SMask"));

      ClearString();
      ASSERT_TRUE(FPDF_SaveAsCopy(opened.doc.get(), this, FPDF_NO_INCREMENTAL));
      ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
      ASSERT_TRUE(saved_doc);
      ScopedSavedPage reloaded = LoadScopedSavedPage(0);
      ASSERT_TRUE(reloaded);
      auto saved = Render(reloaded.get());
      ExpectSamePixels(after.get(), saved.get(), 100, 200, 500, 400);
    }
  }
}

// A JPEG 2000's own transparency becomes a soft mask beside the JPEG: what it
// left transparent stays so, and the hole is opaque paper.
TEST_F(EPDFRedactEmbedderTest, AJpeg2000sOwnTransparencyBecomesASoftMask) {
  const std::string input = ImagePage(
      StreamObject("/Type /XObject /Subtype /Image /Width 40 /Height 20 "
                   "/ColorSpace /DeviceRGB /SMaskInData 1 /Filter "
                   "[/ASCIIHexDecode /JPXDecode]",
                   std::string(kRgbaJpx) + ">"),
      "1 0 0 rg 50 100 200 100 re f");
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer transaction" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
    ASSERT_TRUE(page);
    // The right half (x 300..500) is transparent: the red square shows.
    auto before = Render(page.get());
    EXPECT_EQ(0xff0000u, Pixel(before.get(), 420, 300));
    ASSERT_NO_FATAL_FAILURE(
        MarkAndApply(opened.doc.get(), page.get(), layer, {50, 200, 90, 100}));
    auto after = Render(page.get());
    EXPECT_TRUE(AtLeast(Pixel(after.get(), 130, 300), 0xf0));
    EXPECT_EQ(0xff0000u, Pixel(after.get(), 420, 300));
    RetainPtr<const CPDF_Stream> kept = OnlyImage(opened.doc.get());
    ASSERT_TRUE(kept);
    EXPECT_EQ("DCTDecode", kept->GetDict()->GetNameFor("Filter"));
    EXPECT_FALSE(kept->GetDict()->KeyExist("SMaskInData"));
    CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(opened.doc.get());
    CPDF_DocumentViewScope document_view(pdf);
    RetainPtr<const CPDF_Stream> mask = kept->GetDict()->GetStreamFor("SMask");
    ASSERT_TRUE(mask);
    EXPECT_EQ("FlateDecode", mask->GetDict()->GetNameFor("Filter"));
    EXPECT_EQ("DeviceGray", mask->GetDict()->GetNameFor("ColorSpace"));
    // The colours were blended with white where the image is see-through.
    RetainPtr<const CPDF_Array> matte = mask->GetDict()->GetArrayFor("Matte");
    ASSERT_TRUE(matte);
    ASSERT_EQ(3u, matte->size());
    for (size_t i = 0; i < matte->size(); ++i) {
      EXPECT_EQ(1, matte->GetFloatAt(i));
    }
  }
}

// An image stored losslessly whose soft mask can't keep its format (a JPEG
// 2000) is decoded, and written losslessly again: gray as it was. Its soft
// mask is decoded on its own grid.
TEST_F(EPDFRedactEmbedderTest,
       ALosslessImageWhoseSoftMaskCannotBeKeptStaysLossless) {
  const std::string input = ImagePage(
      StreamObject(
          "/Type /XObject /Subtype /Image /Width 20 /Height 10 "
          "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter "
          "/FlateDecode /SMask 6 0 R",
          Flate(Samples(1, 8, [](int x, int, int) { return 20u + x * 5; }))),
      "0 0 1 rg 50 100 200 100 re f",
      {StreamObject("/Type /XObject /Subtype /Image /Width 40 /Height 20 "
                    "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter "
                    "[/ASCIIHexDecode /JPXDecode]",
                    std::string(kGrayJpx) + ">")});
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer transaction" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
    ASSERT_TRUE(page);
    auto before = Render(page.get());
    ASSERT_NO_FATAL_FAILURE(
        MarkAndApply(opened.doc.get(), page.get(), layer, {50, 200, 90, 100}));
    auto after = Render(page.get());
    EXPECT_TRUE(AtLeast(Pixel(after.get(), 130, 300), 0xf0));
    ExpectSamePixels(before.get(), after.get(), 350, 200, 500, 400);
    RetainPtr<const CPDF_Stream> kept = OnlyImage(opened.doc.get());
    ASSERT_TRUE(kept);
    EXPECT_EQ("FlateDecode", kept->GetDict()->GetNameFor("Filter"));
    EXPECT_EQ("DeviceGray", kept->GetDict()->GetNameFor("ColorSpace"));
    CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(opened.doc.get());
    CPDF_DocumentViewScope document_view(pdf);
    RetainPtr<const CPDF_Stream> mask = kept->GetDict()->GetStreamFor("SMask");
    ASSERT_TRUE(mask);
    EXPECT_EQ("FlateDecode", mask->GetDict()->GetNameFor("Filter"));
    EXPECT_EQ(40, mask->GetDict()->GetIntegerFor("Width"));
  }
}

// A soft mask's /Matte names a colour of its image's colour space, which
// PDFium takes back out of the image's colours. A CMYK image decoded and
// written as RGB gets its matte as RGB: one left in CMYK doesn't fit, so it
// would be ignored and the colours drawn darker.
TEST_F(EPDFRedactEmbedderTest, ADecodedImagesMatteIsWrittenInItsNewColorSpace) {
  const std::string input = ImagePage(
      StreamObject("/Type /XObject /Subtype /Image /Width 20 /Height 10 "
                   "/ColorSpace /DeviceCMYK /BitsPerComponent 8 /Filter "
                   "/FlateDecode /SMask 6 0 R",
                   Flate(Samples(4, 8,
                                 [](int x, int, int component) {
                                   return component == 1 || component == 2
                                              ? 60u + x * 5
                                              : 120u;
                                 }))),
      "0 0 1 rg 50 100 200 100 re f",
      {StreamObject("/Type /XObject /Subtype /Image /Width 40 /Height 20 "
                    "/ColorSpace /DeviceGray /BitsPerComponent 8 "
                    "/Matte [0 0 0 1] /Filter [/ASCIIHexDecode /JPXDecode]",
                    std::string(kGrayJpx) + ">")});
  for (bool layer : {false, true}) {
    SCOPED_TRACE(layer ? "layer transaction" : "document");
    OpenedPdf opened = OpenPdf(input, layer);
    ASSERT_TRUE(opened.doc);
    ScopedFPDFPage page(FPDF_LoadPage(opened.doc.get(), 0));
    ASSERT_TRUE(page);
    auto before = Render(page.get());
    ASSERT_NO_FATAL_FAILURE(
        MarkAndApply(opened.doc.get(), page.get(), layer, {50, 200, 90, 100}));
    auto after = Render(page.get());
    EXPECT_TRUE(AtLeast(Pixel(after.get(), 130, 300), 0xf0));
    ExpectSamePixels(before.get(), after.get(), 350, 200, 500, 400);
    RetainPtr<const CPDF_Stream> kept = OnlyImage(opened.doc.get());
    ASSERT_TRUE(kept);
    EXPECT_EQ("DeviceRGB", kept->GetDict()->GetNameFor("ColorSpace"));
    CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(opened.doc.get());
    CPDF_DocumentViewScope document_view(pdf);
    RetainPtr<const CPDF_Stream> mask = kept->GetDict()->GetStreamFor("SMask");
    ASSERT_TRUE(mask);
    RetainPtr<const CPDF_Array> matte = mask->GetDict()->GetArrayFor("Matte");
    ASSERT_TRUE(matte);
    EXPECT_EQ(3u, matte->size());
  }
}
