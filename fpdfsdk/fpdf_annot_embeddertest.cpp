// Copyright 2017 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "public/fpdf_annot.h"
#include "public/epdf_signature.h"
#include "public/epdf_form.h"
#include "public/epdf_text.h"
#include "public/fpdf_edit.h"

#include <limits.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "build/build_config.h"
#include "constants/annotation_common.h"
#include "core/fpdfapi/edit/cpdf_stringarchivestream.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/font/cpdf_tounicodemap.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_read_only_graph_guard.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_annotfontsubset.h"
#include "core/fpdfdoc/cpdf_richtextwriter.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/containers/contains.h"
#include "core/fxcrt/fx_memcpy_wrappers.h"
#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxcrt/span.h"
#include "core/fxge/cfx_defaultrenderdevice.h"
#include "core/fxge/cfx_font.h"
#include "core/fxge/cfx_fontregistry.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_font.h"
#include "public/fpdf_attachment.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_flatten.h"
#include "public/fpdf_formfill.h"
#include "public/fpdf_ppo.h"
#include "public/fpdf_save.h"
#include "public/fpdf_text.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/embedder_test_constants.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gmock/include/gmock/gmock-matchers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/utils/file_util.h"
#include "testing/utils/hash.h"
#include "testing/utils/path_service.h"

using pdfium::kAnnotationStampWithApPng;
using testing::HasSubstr;

namespace {

const wchar_t kStreamData[] =
    L"/GS gs 0.0 0.0 0.0 RG 4 w 211.8 747.6 m 211.8 744.8 "
    L"212.6 743.0 214.2 740.8 "
    L"c 215.4 739.0 216.8 737.1 218.9 736.1 c 220.8 735.1 221.4 733.0 "
    L"223.7 732.4 c 232.6 729.9 242.0 730.8 251.2 730.8 c 257.5 730.8 "
    L"263.0 732.9 269.0 734.4 c S";

std::wstring ExtractPageText(FPDF_PAGE page) {
  ScopedFPDFTextPage text_page(FPDFText_LoadPage(page));
  if (!text_page) {
    ADD_FAILURE() << "Failed to load text page";
    return L"";
  }

  const int char_count = FPDFText_CountChars(text_page.get());
  std::vector<FPDF_WCHAR> buffer(char_count + 1);
  EXPECT_GT(FPDFText_GetText(text_page.get(), 0, char_count, buffer.data()), 0);
  return GetPlatformWString(buffer.data());
}

std::vector<uint8_t> LoadNotoSansSCFontData() {
  std::string font_path = PathService::GetThirdPartyFilePath(
      "NotoSansCJK/NotoSansSC-Regular.subset.otf");
  if (font_path.empty()) {
    ADD_FAILURE() << "Failed to find NotoSansSC subset font";
    return {};
  }
  return GetFileContents(font_path.c_str());
}

std::vector<uint8_t> LoadAmiriFontData() {
  std::string font_path = PathService::GetThirdPartyFilePath(
      "harfbuzz-ng/src/perf/fonts/Amiri-Regular.ttf");
  if (font_path.empty()) {
    ADD_FAILURE() << "Failed to find Amiri test font";
    return {};
  }
  return GetFileContents(font_path.c_str());
}

std::vector<uint8_t> LoadRobotoVariableAbcFontData() {
  std::string font_path = PathService::GetThirdPartyFilePath(
      "harfbuzz-ng/src/test/subset/data/fonts/Roboto-Variable.ABC.ttf");
  if (font_path.empty()) {
    ADD_FAILURE() << "Failed to find Roboto-Variable.ABC test font";
    return {};
  }
  return GetFileContents(font_path.c_str());
}

std::vector<uint8_t> LoadCffLatinFontData() {
  // A small OpenType/CFF font (no CIDFont operators) with A, B, C.
  std::string font_path = PathService::GetThirdPartyFilePath(
      "harfbuzz-ng/src/test/subset/data/fonts/gpos1_2_font.otf");
  if (font_path.empty()) {
    ADD_FAILURE() << "Failed to find gpos1_2_font.otf";
    return {};
  }
  return GetFileContents(font_path.c_str());
}

std::vector<uint8_t> LoadRobotoFontData() {
  std::string font_path = PathService::GetThirdPartyFilePath(
      "harfbuzz-ng/src/perf/fonts/Roboto-Regular.ttf");
  if (font_path.empty()) {
    ADD_FAILURE() << "Failed to find Roboto test font";
    return {};
  }
  return GetFileContents(font_path.c_str());
}

std::vector<uint8_t> LoadDroidSansFallbackFullFontData() {
  std::string font_path =
      PathService::GetTestFilePath("fonts/DroidSansFallbackFull.ttf");
  if (font_path.empty()) {
    ADD_FAILURE() << "Failed to find DroidSansFallbackFull test font";
    return {};
  }
  return GetFileContents(font_path.c_str());
}

EPDF_FONT_ID RegisterDroidSansFallbackFullFont() {
  std::vector<uint8_t> font_data = LoadDroidSansFallbackFullFontData();
  if (font_data.empty()) {
    return 0;
  }

  EPDF_FONT_ID font_id = EPDFFont_RegisterMemFont64(
      "DroidSansFallbackFull", /*weight=*/400, /*italic=*/0, font_data.data(),
      font_data.size());
  if (font_id == 0 || !EPDFFont_AddFallbackFont(font_id)) {
    ADD_FAILURE() << "Failed to register DroidSansFallbackFull as fallback";
    return 0;
  }
  return font_id;
}

std::wstring GetNormalAppearance(FPDF_ANNOTATION annot) {
  unsigned long length_bytes =
      FPDFAnnot_GetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0);
  if (length_bytes == 0) {
    ADD_FAILURE() << "Missing normal appearance stream";
    return L"";
  }

  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(length_bytes,
            FPDFAnnot_GetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                            buffer.data(), length_bytes));
  return GetPlatformWString(buffer.data());
}

class ScopedRegisteredFonts {
 public:
  ScopedRegisteredFonts() { EPDFFont_ClearRegisteredFonts(); }
  ~ScopedRegisteredFonts() { EPDFFont_ClearRegisteredFonts(); }
};

class MemoryFileAccess final : public FPDF_FILEACCESS {
 public:
  explicit MemoryFileAccess(std::vector<uint8_t> data) : data_(std::move(data)) {
    m_FileLen = static_cast<unsigned long>(data_.size());
    m_GetBlock = &MemoryFileAccess::GetBlock;
    m_Param = this;
  }

 private:
  static int GetBlock(void* param,
                      unsigned long pos,
                      unsigned char* buf,
                      unsigned long size) {
    auto* file_access = static_cast<MemoryFileAccess*>(param);
    if (!file_access || !buf || pos > file_access->data_.size() ||
        size > file_access->data_.size() - pos) {
      return 0;
    }

    std::copy_n(file_access->data_.data() + pos, size, buf);
    return 1;
  }

  std::vector<uint8_t> data_;
};

ByteString RegisteredFontAlias(EPDF_FONT_ID font_id) {
  return ByteString::Format("ERegF%u", font_id);
}

ByteString GetDefaultAppearanceFontAlias(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context) {
    return ByteString();
  }

  const CPDF_Dictionary* annot_dict = context->GetAnnotDict();
  if (!annot_dict) {
    return ByteString();
  }

  ByteString da = annot_dict->GetByteStringFor("DA");
  std::optional<size_t> slash_pos = da.Find('/');
  if (!slash_pos.has_value()) {
    return ByteString();
  }

  ByteStringView remainder = da.AsStringView().Substr(slash_pos.value() + 1);
  std::optional<size_t> end_pos = remainder.Find(' ');
  if (!end_pos.has_value()) {
    return ByteString(remainder);
  }
  return ByteString(remainder.First(end_pos.value()));
}

ByteString GetNormalAppearanceStreamBytes(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context) {
    return ByteString();
  }

  const CPDF_Dictionary* annot_dict = context->GetAnnotDict();
  if (!annot_dict) {
    return ByteString();
  }

  RetainPtr<const CPDF_Dictionary> ap_dict =
      annot_dict->GetDictFor(pdfium::annotation::kAP);
  RetainPtr<const CPDF_Stream> normal_stream =
      ap_dict ? ap_dict->GetStreamFor("N") : nullptr;
  if (!normal_stream) {
    return ByteString();
  }

  RetainPtr<CPDF_StreamAcc> stream_acc =
      pdfium::MakeRetain<CPDF_StreamAcc>(std::move(normal_stream));
  stream_acc->LoadAllDataFiltered();
  return ByteString(ByteStringView(stream_acc->GetSpan()));
}

RetainPtr<const CPDF_Dictionary> GetAppearanceFontDict(
    FPDF_ANNOTATION annot,
    const ByteString& font_alias) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context) {
    return nullptr;
  }

  const CPDF_Dictionary* annot_dict = context->GetAnnotDict();
  if (!annot_dict) {
    return nullptr;
  }

  RetainPtr<const CPDF_Dictionary> ap_dict =
      annot_dict->GetDictFor(pdfium::annotation::kAP);
  RetainPtr<const CPDF_Dictionary> stream_dict =
      ap_dict ? ap_dict->GetDictFor("N") : nullptr;
  RetainPtr<const CPDF_Dictionary> resources_dict =
      stream_dict ? stream_dict->GetDictFor("Resources") : nullptr;
  RetainPtr<const CPDF_Dictionary> font_dict =
      resources_dict ? resources_dict->GetDictFor("Font") : nullptr;
  return font_dict ? font_dict->GetDictFor(font_alias.AsStringView()) : nullptr;
}

// A Type0 font written by EmbedPDF with an embedded TrueType program and a
// ToUnicode map, subset or whole.
bool AppearanceFontEmbedsProgram(const CPDF_Dictionary* font_dict) {
  if (!font_dict || font_dict->GetNameFor("Subtype") != "Type0" ||
      !font_dict->GetStreamFor("ToUnicode")) {
    return false;
  }

  RetainPtr<const CPDF_Array> descendant_fonts =
      font_dict->GetArrayFor("DescendantFonts");
  if (!descendant_fonts || descendant_fonts->size() == 0) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> cid_font_dict =
      descendant_fonts->GetDictAt(0);
  RetainPtr<const CPDF_Dictionary> font_descriptor =
      cid_font_dict ? cid_font_dict->GetDictFor("FontDescriptor") : nullptr;
  return font_descriptor && font_descriptor->GetStreamFor("FontFile2");
}

bool AppearanceFontIsSubsetTagged(const CPDF_Dictionary* font_dict) {
  ByteString base_font = font_dict ? font_dict->GetNameFor("BaseFont") : "";
  return base_font.GetLength() > 7 && base_font[6] == '+';
}

bool AppearanceFontHasEmbeddedSubset(const CPDF_Dictionary* font_dict) {
  return AppearanceFontEmbedsProgram(font_dict) &&
         AppearanceFontIsSubsetTagged(font_dict);
}

// The whole program of |size| bytes, untagged (Embedding::kFull).
bool AppearanceFontEmbedsWholeProgram(const CPDF_Dictionary* font_dict,
                                      size_t size) {
  if (!AppearanceFontEmbedsProgram(font_dict) ||
      AppearanceFontIsSubsetTagged(font_dict)) {
    return false;
  }
  RetainPtr<const CPDF_Array> descendant_fonts =
      font_dict->GetArrayFor("DescendantFonts");
  RetainPtr<const CPDF_Dictionary> font_descriptor =
      descendant_fonts->GetDictAt(0)->GetDictFor("FontDescriptor");
  RetainPtr<const CPDF_Stream> program =
      font_descriptor->GetStreamFor("FontFile2");
  return program->GetDict()->GetIntegerFor("Length1") == static_cast<int>(size);
}

bool AppearanceFontMapsUnicode(const CPDF_Dictionary* font_dict,
                               wchar_t value) {
  RetainPtr<const CPDF_Stream> to_unicode =
      font_dict ? font_dict->GetStreamFor("ToUnicode") : nullptr;
  if (!to_unicode) {
    return false;
  }

  CPDF_ToUnicodeMap to_unicode_map(std::move(to_unicode));
  return to_unicode_map.ReverseLookup(value) != 0;
}

// |expect_subset|: the DEFAULT embedding policy subsets annotation text and
// embeds form field text whole (Phase C note §2).
void ExpectRegisteredAppearanceMapsUnicode(
    FPDF_ANNOTATION annot,
    EPDF_FONT_ID font_id,
    std::initializer_list<wchar_t> unicodes,
    bool expect_subset = true) {
  RetainPtr<const CPDF_Dictionary> font_dict =
      GetAppearanceFontDict(annot, RegisteredFontAlias(font_id));
  ASSERT_TRUE(font_dict);
  EXPECT_TRUE(AppearanceFontEmbedsProgram(font_dict.Get()));
  EXPECT_EQ(expect_subset, AppearanceFontIsSubsetTagged(font_dict.Get()));
  for (wchar_t unicode : unicodes) {
    EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), unicode));
  }
}

std::string BitmapChecksum(FPDF_BITMAP bitmap) {
  if (!bitmap) {
    return std::string();
  }

  const int stride = FPDFBitmap_GetStride(bitmap);
  const int height = FPDFBitmap_GetHeight(bitmap);
  FX_SAFE_SIZE_T size = stride;
  size *= height;
  if (!size.IsValid()) {
    return std::string();
  }

  return GenerateMD5Base16(
      pdfium::span(static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap)),
                   size.ValueOrDie()));
}

bool BitmapHasNonWhitePixels(FPDF_BITMAP bitmap) {
  if (!bitmap) {
    return false;
  }

  const int stride = FPDFBitmap_GetStride(bitmap);
  const int height = FPDFBitmap_GetHeight(bitmap);
  FX_SAFE_SIZE_T size = stride;
  size *= height;
  if (!size.IsValid()) {
    return false;
  }

  pdfium::span<const uint8_t> bytes(
      static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap)),
      size.ValueOrDie());
  return std::ranges::any_of(bytes,
                             [](uint8_t value) { return value != 0xff; });
}

void AddBrokenTrueTypeCjkTextPageContent(FPDF_DOCUMENT doc, FPDF_PAGE page) {
  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc);
  CPDF_Page* cpdf_page = CPDFPageFromFPDFPage(page);
  ASSERT_TRUE(cpdf_doc);
  ASSERT_TRUE(cpdf_page);

  auto font_dict = cpdf_doc->NewIndirect<CPDF_Dictionary>();
  font_dict->SetNewFor<CPDF_Name>("Type", "Font");
  font_dict->SetNewFor<CPDF_Name>("Subtype", "TrueType");
  font_dict->SetNewFor<CPDF_Name>("BaseFont", "Helvetica");

  auto encoding_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  encoding_dict->SetNewFor<CPDF_Name>("Type", "Encoding");
  auto differences = pdfium::MakeRetain<CPDF_Array>();
  differences->AppendNew<CPDF_Number>(65);
  differences->AppendNew<CPDF_Name>("uni8FD9");
  encoding_dict->SetFor("Differences", std::move(differences));
  font_dict->SetFor("Encoding", std::move(encoding_dict));

  RetainPtr<CPDF_Dictionary> page_dict = cpdf_page->GetMutableDict();
  RetainPtr<CPDF_Dictionary> resources =
      page_dict->GetOrCreateDictFor("Resources");
  RetainPtr<CPDF_Dictionary> font_resources =
      resources->GetOrCreateDictFor("Font");
  font_resources->SetNewFor<CPDF_Reference>("F1", cpdf_doc,
                                            font_dict->GetObjNum());

  const ByteString kContent =
      "BT\n"
      "/F1 72 Tf\n"
      "40 100 Td\n"
      "<41> Tj\n"
      "ET\n";
  RetainPtr<CPDF_Stream> contents =
      cpdf_doc->NewIndirect<CPDF_Stream>(kContent.unsigned_span());
  page_dict->SetNewFor<CPDF_Reference>("Contents", cpdf_doc,
                                       contents->GetObjNum());
}

// Applies one redaction and returns how many annotations other than REDACT
// ones were removed alongside it.
uint32_t ApplyRedactionCountingRemoved(FPDF_PAGE page, FPDF_ANNOTATION annot) {
  uint32_t removed_count = 0;
  EXPECT_TRUE(EPDFAnnot_ApplyRedaction(page, annot, &removed_count));
  return removed_count;
}

uint32_t ApplyPageRedactionsCountingRemoved(FPDF_PAGE page) {
  uint32_t removed_count = 0;
  EXPECT_TRUE(EPDFPage_ApplyRedactions(page, &removed_count));
  return removed_count;
}

ScopedFPDFAnnotation CreateRedactAnnot(FPDF_PAGE page, const FS_RECTF& rect) {
  ScopedFPDFAnnotation annot(FPDFPage_CreateAnnot(page, FPDF_ANNOT_REDACT));
  EXPECT_TRUE(annot);
  if (annot) {
    EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  }
  return annot;
}

ScopedFPDFBitmap RenderPageOnWhite(FPDF_PAGE page, int width, int height) {
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(width, height, /*alpha=*/0));
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);
  FPDF_RenderPageBitmap(bitmap.get(), page, 0, 0, width, height, /*rotate=*/0,
                        /*flags=*/0);
  return bitmap;
}

uint32_t GetPixelColor(FPDF_BITMAP bitmap, int x, int y) {
  const uint8_t* buffer =
      static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
  const int stride = FPDFBitmap_GetStride(bitmap);
  const uint8_t* pixel = buffer + y * stride + x * 4;
  return (uint32_t{pixel[3]} << 24) | (uint32_t{pixel[2]} << 16) |
         (uint32_t{pixel[1]} << 8) | uint32_t{pixel[0]};
}

// Number of pixels in [left, right) x [top, bottom), bitmap coordinates, that
// differ from `background`.
int CountInkPixels(FPDF_BITMAP bitmap,
                   int left,
                   int top,
                   int right,
                   int bottom,
                   uint32_t background) {
  int count = 0;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      if (GetPixelColor(bitmap, x, y) != background) {
        ++count;
      }
    }
  }
  return count;
}

void VerifyFocusableAnnotSubtypes(
    FPDF_FORMHANDLE form_handle,
    pdfium::span<const FPDF_ANNOTATION_SUBTYPE> expected_subtypes) {
  ASSERT_EQ(static_cast<int>(expected_subtypes.size()),
            FPDFAnnot_GetFocusableSubtypesCount(form_handle));

  std::vector<FPDF_ANNOTATION_SUBTYPE> actual_subtypes(
      expected_subtypes.size());
  ASSERT_TRUE(FPDFAnnot_GetFocusableSubtypes(
      form_handle, actual_subtypes.data(), actual_subtypes.size()));
  for (size_t i = 0; i < expected_subtypes.size(); ++i) {
    ASSERT_EQ(expected_subtypes[i], actual_subtypes[i]);
  }
}

void SetAndVerifyFocusableAnnotSubtypes(
    FPDF_FORMHANDLE form_handle,
    pdfium::span<const FPDF_ANNOTATION_SUBTYPE> subtypes) {
  ASSERT_TRUE(FPDFAnnot_SetFocusableSubtypes(form_handle, subtypes.data(),
                                             subtypes.size()));
  VerifyFocusableAnnotSubtypes(form_handle, subtypes);
}

void VerifyAnnotationSubtypesAndFocusability(
    FPDF_FORMHANDLE form_handle,
    FPDF_PAGE page,
    pdfium::span<const FPDF_ANNOTATION_SUBTYPE> expected_subtypes,
    pdfium::span<const FPDF_ANNOTATION_SUBTYPE> expected_focusable_subtypes) {
  ASSERT_EQ(static_cast<int>(expected_subtypes.size()),
            FPDFPage_GetAnnotCount(page));
  for (size_t i = 0; i < expected_subtypes.size(); ++i) {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, i));
    ASSERT_TRUE(annot);
    EXPECT_EQ(expected_subtypes[i], FPDFAnnot_GetSubtype(annot.get()));

    bool expected_focusable =
        pdfium::Contains(expected_focusable_subtypes, expected_subtypes[i]);
    EXPECT_EQ(expected_focusable,
              FORM_SetFocusedAnnot(form_handle, annot.get()));

    // Kill the focus so the next test starts in an unfocused state.
    FORM_ForceToKillFocus(form_handle);
  }
}

void VerifyUriActionInLink(FPDF_DOCUMENT doc,
                           FPDF_LINK link,
                           const std::string& expected_uri) {
  ASSERT_TRUE(link);

  FPDF_ACTION action = FPDFLink_GetAction(link);
  ASSERT_TRUE(action);
  EXPECT_EQ(static_cast<unsigned long>(PDFACTION_URI),
            FPDFAction_GetType(action));

  unsigned long bufsize = FPDFAction_GetURIPath(doc, action, nullptr, 0);
  ASSERT_EQ(expected_uri.size() + 1, bufsize);

  std::vector<char> buffer(bufsize);
  EXPECT_EQ(bufsize,
            FPDFAction_GetURIPath(doc, action, buffer.data(), bufsize));
  EXPECT_EQ(expected_uri, buffer.data());
}

}  // namespace

class FPDFAnnotEmbedderTest : public EmbedderTest {};

TEST_F(FPDFAnnotEmbedderTest, SetAP) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 100, 100));
  ASSERT_TRUE(page);
  ScopedFPDFWideString ap_stream = GetFPDFWideString(kStreamData);
  ASSERT_TRUE(ap_stream);

  ScopedFPDFAnnotation annot(FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_INK));
  ASSERT_TRUE(annot);

  // Negative case: FPDFAnnot_SetAP() should fail if bounding rect is not yet
  // set on the annotation.
  EXPECT_FALSE(FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                               ap_stream.get()));

  const FS_RECTF bounding_rect{206.0f, 753.0f, 339.0f, 709.0f};
  EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &bounding_rect));

  ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color,
                                 /*R=*/255, /*G=*/0, /*B=*/0, /*A=*/255));

  EXPECT_TRUE(FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                              ap_stream.get()));

  // Verify that appearance stream is created as form XObject
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot.get());
  ASSERT_TRUE(context);
  const CPDF_Dictionary* annot_dict = context->GetAnnotDict();
  ASSERT_TRUE(annot_dict);
  RetainPtr<const CPDF_Dictionary> ap_dict =
      annot_dict->GetDictFor(pdfium::annotation::kAP);
  ASSERT_TRUE(ap_dict);
  RetainPtr<const CPDF_Dictionary> stream_dict = ap_dict->GetDictFor("N");
  ASSERT_TRUE(stream_dict);
  // Check for non-existence of resources dictionary in case of opaque color
  RetainPtr<const CPDF_Dictionary> resources_dict =
      stream_dict->GetDictFor("Resources");
  ASSERT_FALSE(resources_dict);
  ByteString type = stream_dict->GetByteStringFor(pdfium::annotation::kType);
  EXPECT_EQ("XObject", type);
  ByteString sub_type =
      stream_dict->GetByteStringFor(pdfium::annotation::kSubtype);
  EXPECT_EQ("Form", sub_type);

  // Check that the appearance stream is same as we just set.
  const uint32_t kStreamDataSize = std::size(kStreamData) * sizeof(FPDF_WCHAR);
  unsigned long normal_length_bytes = FPDFAnnot_GetAP(
      annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0);
  ASSERT_EQ(kStreamDataSize, normal_length_bytes);
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(normal_length_bytes);
  EXPECT_EQ(kStreamDataSize,
            FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                            buf.data(), normal_length_bytes));
  EXPECT_EQ(kStreamData, GetPlatformWString(buf.data()));
}

TEST_F(FPDFAnnotEmbedderTest, SetAPWithOpacity) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 100, 100));
  ASSERT_TRUE(page);
  ScopedFPDFWideString ap_stream = GetFPDFWideString(kStreamData);
  ASSERT_TRUE(ap_stream);

  ScopedFPDFAnnotation annot(FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_INK));
  ASSERT_TRUE(annot);

  ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color,
                                 /*R=*/255, /*G=*/0, /*B=*/0, /*A=*/102));

  const FS_RECTF bounding_rect{206.0f, 753.0f, 339.0f, 709.0f};
  EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &bounding_rect));

  EXPECT_TRUE(FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                              ap_stream.get()));

  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot.get());
  ASSERT_TRUE(context);
  const CPDF_Dictionary* annot_dict = context->GetAnnotDict();
  ASSERT_TRUE(annot_dict);
  RetainPtr<const CPDF_Dictionary> ap_dict =
      annot_dict->GetDictFor(pdfium::annotation::kAP);
  ASSERT_TRUE(ap_dict);
  RetainPtr<const CPDF_Dictionary> stream_dict = ap_dict->GetDictFor("N");
  ASSERT_TRUE(stream_dict);
  RetainPtr<const CPDF_Dictionary> resources_dict =
      stream_dict->GetDictFor("Resources");
  ASSERT_TRUE(stream_dict);
  RetainPtr<const CPDF_Dictionary> extGState_dict =
      resources_dict->GetDictFor("ExtGState");
  ASSERT_TRUE(extGState_dict);
  RetainPtr<const CPDF_Dictionary> gs_dict = extGState_dict->GetDictFor("GS");
  ASSERT_TRUE(gs_dict);
  ByteString type = gs_dict->GetByteStringFor(pdfium::annotation::kType);
  EXPECT_EQ("ExtGState", type);
  float opacity = gs_dict->GetFloatFor("CA");
  // Opacity value of 102 is represented as 0.4f (=104/255) in /CA entry.
  EXPECT_FLOAT_EQ(0.4f, opacity);
  ByteString blend_mode = gs_dict->GetByteStringFor("BM");
  EXPECT_EQ("Normal", blend_mode);
  bool alpha_source_flag = gs_dict->GetBooleanFor("AIS", true);
  EXPECT_FALSE(alpha_source_flag);
}

TEST_F(FPDFAnnotEmbedderTest, InkListAPIValidations) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 100, 100));
  ASSERT_TRUE(page);

  // Create a new ink annotation.
  ScopedFPDFAnnotation ink_annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_INK));
  ASSERT_TRUE(ink_annot);
  CPDF_AnnotContext* context =
      CPDFAnnotContextFromFPDFAnnotation(ink_annot.get());
  ASSERT_TRUE(context);
  const CPDF_Dictionary* annot_dict = context->GetAnnotDict();
  ASSERT_TRUE(annot_dict);

  static constexpr FS_POINTF kFirstInkStroke[] = {
      {80.0f, 90.0f}, {81.0f, 91.0f}, {82.0f, 92.0f},
      {83.0f, 93.0f}, {84.0f, 94.0f}, {85.0f, 95.0f}};
  static constexpr size_t kFirstStrokePointCount = std::size(kFirstInkStroke);

  static constexpr FS_POINTF kSecondInkStroke[] = {
      {70.0f, 90.0f}, {71.0f, 91.0f}, {72.0f, 92.0f}};
  static constexpr size_t kSecondStrokePointCount = std::size(kSecondInkStroke);

  static constexpr FS_POINTF kThirdInkStroke[] = {{60.0f, 90.0f},
                                                  {61.0f, 91.0f},
                                                  {62.0f, 92.0f},
                                                  {63.0f, 93.0f},
                                                  {64.0f, 94.0f}};
  static constexpr size_t kThirdStrokePointCount = std::size(kThirdInkStroke);

  // Negative test: |annot| is passed as nullptr.
  EXPECT_EQ(-1, FPDFAnnot_AddInkStroke(nullptr, kFirstInkStroke,
                                       kFirstStrokePointCount));

  // Negative test: |annot| is not ink annotation.
  // Create a new highlight annotation.
  ScopedFPDFAnnotation highlight_annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_HIGHLIGHT));
  ASSERT_TRUE(highlight_annot);
  EXPECT_EQ(-1, FPDFAnnot_AddInkStroke(highlight_annot.get(), kFirstInkStroke,
                                       kFirstStrokePointCount));

  // Negative test: passing |point_count| as  0.
  EXPECT_EQ(-1, FPDFAnnot_AddInkStroke(ink_annot.get(), kFirstInkStroke, 0));

  // Negative test: passing |points| array as nullptr.
  EXPECT_EQ(-1, FPDFAnnot_AddInkStroke(ink_annot.get(), nullptr,
                                       kFirstStrokePointCount));

  // Negative test: passing |point_count| more than ULONG_MAX/2.
  EXPECT_EQ(-1, FPDFAnnot_AddInkStroke(ink_annot.get(), kSecondInkStroke,
                                       ULONG_MAX / 2 + 1));

  // InkStroke should get added to ink annotation. Also inklist should get
  // created.
  EXPECT_EQ(0, FPDFAnnot_AddInkStroke(ink_annot.get(), kFirstInkStroke,
                                      kFirstStrokePointCount));

  RetainPtr<const CPDF_Array> inklist = annot_dict->GetArrayFor("InkList");
  ASSERT_TRUE(inklist);
  EXPECT_EQ(1u, inklist->size());
  EXPECT_EQ(kFirstStrokePointCount * 2, inklist->GetArrayAt(0)->size());

  // Adding another inkStroke to ink annotation with all valid paremeters.
  // InkList already exists in ink_annot.
  EXPECT_EQ(1, FPDFAnnot_AddInkStroke(ink_annot.get(), kSecondInkStroke,
                                      kSecondStrokePointCount));
  EXPECT_EQ(2u, inklist->size());
  EXPECT_EQ(kSecondStrokePointCount * 2, inklist->GetArrayAt(1)->size());

  // Adding one more InkStroke to the ink annotation. |point_count| passed is
  // less than the data available in |buffer|.
  EXPECT_EQ(2, FPDFAnnot_AddInkStroke(ink_annot.get(), kThirdInkStroke,
                                      kThirdStrokePointCount - 1));
  EXPECT_EQ(3u, inklist->size());
  EXPECT_EQ((kThirdStrokePointCount - 1) * 2, inklist->GetArrayAt(2)->size());
}

TEST_F(FPDFAnnotEmbedderTest, RemoveInkList) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 100, 100));
  ASSERT_TRUE(page);

  // Negative test: |annot| is passed as nullptr.
  EXPECT_FALSE(FPDFAnnot_RemoveInkList(nullptr));

  // Negative test: |annot| is not ink annotation.
  // Create a new highlight annotation.
  ScopedFPDFAnnotation highlight_annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_HIGHLIGHT));
  ASSERT_TRUE(highlight_annot);
  EXPECT_FALSE(FPDFAnnot_RemoveInkList(highlight_annot.get()));

  // Create a new ink annotation.
  ScopedFPDFAnnotation ink_annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_INK));
  ASSERT_TRUE(ink_annot);
  CPDF_AnnotContext* context =
      CPDFAnnotContextFromFPDFAnnotation(ink_annot.get());
  ASSERT_TRUE(context);
  const CPDF_Dictionary* annot_dict = context->GetAnnotDict();
  ASSERT_TRUE(annot_dict);

  static constexpr FS_POINTF kInkStroke[] = {{80.0f, 90.0f}, {81.0f, 91.0f},
                                             {82.0f, 92.0f}, {83.0f, 93.0f},
                                             {84.0f, 94.0f}, {85.0f, 95.0f}};
  static constexpr size_t kPointCount = std::size(kInkStroke);

  // InkStroke should get added to ink annotation. Also inklist should get
  // created.
  EXPECT_EQ(0,
            FPDFAnnot_AddInkStroke(ink_annot.get(), kInkStroke, kPointCount));

  RetainPtr<const CPDF_Array> inklist = annot_dict->GetArrayFor("InkList");
  ASSERT_TRUE(inklist);
  ASSERT_EQ(1u, inklist->size());
  EXPECT_EQ(kPointCount * 2, inklist->GetArrayAt(0)->size());

  // Remove inklist.
  EXPECT_TRUE(FPDFAnnot_RemoveInkList(ink_annot.get()));
  EXPECT_FALSE(annot_dict->KeyExist("InkList"));
}

TEST_F(FPDFAnnotEmbedderTest, GenerateInkAppearanceIsIdempotentOnRect) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 200, 200));
  ASSERT_TRUE(page);

  ScopedFPDFAnnotation annot(FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_INK));
  ASSERT_TRUE(annot);

  static constexpr FS_POINTF kStroke[] = {
      {50.0f, 50.0f}, {80.0f, 90.0f}, {120.0f, 60.0f}};
  ASSERT_EQ(0,
            FPDFAnnot_AddInkStroke(annot.get(), kStroke, std::size(kStroke)));
  ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, 255,
                                 0, 0, 255));
  ASSERT_TRUE(FPDFAnnot_SetBorder(annot.get(), /*horizontal_radius=*/0.0f,
                                  /*vertical_radius=*/0.0f,
                                  /*border_width=*/6.0f));

  // A caller-authored /Rect that already encloses the STROKED ink: the point
  // bounds (50..120, 50..90) inflated by border_width / 2 = 3 on every side —
  // exactly the rect EmbedPDF's writers supply.
  const FS_RECTF authored{/*left=*/47.0f, /*top=*/93.0f, /*right=*/123.0f,
                          /*bottom=*/47.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &authored));

  // Generating the appearance must NOT disturb a rect the ink already fits in
  // — no matter how many times it runs (the engine re-bakes after every
  // edit). The old behavior inflated /Rect by border_width / 2 per call.
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
    FS_RECTF rect;
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
    EXPECT_FLOAT_EQ(authored.left, rect.left) << "iteration " << i;
    EXPECT_FLOAT_EQ(authored.top, rect.top) << "iteration " << i;
    EXPECT_FLOAT_EQ(authored.right, rect.right) << "iteration " << i;
    EXPECT_FLOAT_EQ(authored.bottom, rect.bottom) << "iteration " << i;
  }

  // A TIGHT rect (bare point bounds, no stroke padding — common in foreign
  // documents, the case the upstream inflate was hacked in for) is corrected
  // ONCE to the minimal rect that contains the stroked ink, then stays
  // stable on further regenerations.
  const FS_RECTF tight{/*left=*/50.0f, /*top=*/90.0f, /*right=*/120.0f,
                       /*bottom=*/50.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &tight));
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
    FS_RECTF rect;
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
    EXPECT_FLOAT_EQ(47.0f, rect.left) << "iteration " << i;
    EXPECT_FLOAT_EQ(93.0f, rect.top) << "iteration " << i;
    EXPECT_FLOAT_EQ(123.0f, rect.right) << "iteration " << i;
    EXPECT_FLOAT_EQ(47.0f, rect.bottom) << "iteration " << i;
  }
}

TEST_F(FPDFAnnotEmbedderTest, BadParams) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  EXPECT_EQ(0, FPDFPage_GetAnnotCount(nullptr));

  EXPECT_FALSE(FPDFPage_GetAnnot(nullptr, 0));
  EXPECT_FALSE(FPDFPage_GetAnnot(nullptr, -1));
  EXPECT_FALSE(FPDFPage_GetAnnot(nullptr, 1));
  EXPECT_FALSE(FPDFPage_GetAnnot(page.get(), -1));
  EXPECT_FALSE(FPDFPage_GetAnnot(page.get(), 1));

  EXPECT_EQ(FPDF_ANNOT_UNKNOWN, FPDFAnnot_GetSubtype(nullptr));

  EXPECT_EQ(0, FPDFAnnot_GetObjectCount(nullptr));

  EXPECT_FALSE(FPDFAnnot_GetObject(nullptr, 0));
  EXPECT_FALSE(FPDFAnnot_GetObject(nullptr, -1));
  EXPECT_FALSE(FPDFAnnot_GetObject(nullptr, 1));

  EXPECT_FALSE(FPDFAnnot_HasKey(nullptr, "foo"));

  static const wchar_t kContents[] = L"Bar";
  ScopedFPDFWideString text = GetFPDFWideString(kContents);
  EXPECT_FALSE(FPDFAnnot_SetStringValue(nullptr, "foo", text.get()));

  FPDF_WCHAR buffer[64];
  EXPECT_EQ(0u, FPDFAnnot_GetStringValue(nullptr, "foo", nullptr, 0));
  EXPECT_EQ(0u, FPDFAnnot_GetStringValue(nullptr, "foo", buffer, 0));
  EXPECT_EQ(0u,
            FPDFAnnot_GetStringValue(nullptr, "foo", buffer, sizeof(buffer)));
}

TEST_F(FPDFAnnotEmbedderTest, BadAnnotsEntry) {
  ASSERT_TRUE(OpenDocument("bad_annots_entry.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_FALSE(FPDFPage_GetAnnot(page.get(), 0));
}

TEST_F(FPDFAnnotEmbedderTest, RenderAnnotWithOnlyRolloverAP) {
  // Open a file with one annotation and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_highlight_rollover_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // This annotation has a malformed appearance stream, which does not have its
  // normal appearance defined, only its rollover appearance. In this case, its
  // normal appearance should be generated, allowing the highlight annotation to
  // still display.
  ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
  CompareBitmap(bitmap.get(), "fpdf_annot_render_annot_with_only_rollover_ap");
}

TEST_F(FPDFAnnotEmbedderTest, RenderMultilineMarkupAnnotWithoutAP) {
  // Open a file with multiline markup annotations.
  ASSERT_TRUE(OpenDocument("annotation_markup_multiline_no_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
  CompareBitmapWithExpectationSuffix(bitmap.get(),
                                     "annotation_markup_multiline_no_ap");
}

TEST_F(FPDFAnnotEmbedderTest, ReadPurityRenderMarkupAnnotWithoutAP) {
  ASSERT_TRUE(OpenDocument("annotation_markup_multiline_no_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(doc);
  const uint32_t last_obj_num = doc->GetLastObjNum();

  {
    CPDF_ReadOnlyGraphGuard guard;
    EXPECT_GT(FPDFPage_GetAnnotCount(page.get()), 0);
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    ASSERT_TRUE(bitmap);
  }

  EXPECT_EQ(last_obj_num, doc->GetLastObjNum());
}

TEST_F(FPDFAnnotEmbedderTest, ExplicitGenerateAppearanceAllowed) {
  ASSERT_TRUE(OpenDocument("annotation_markup_multiline_no_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(doc);
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(annot);
  const uint32_t before = doc->GetLastObjNum();

  EXPECT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

  EXPECT_GT(doc->GetLastObjNum(), before);
}

TEST_F(FPDFAnnotEmbedderTest, TextFieldGenerateAppearanceStreamIsStable) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  static const FS_POINTF kTextFieldPoint = {120.0f, 120.0f};
  ScopedFPDFAnnotation annot(FPDFAnnot_GetFormFieldAtPoint(
      form_handle(), page.get(), &kTextFieldPoint));
  ASSERT_TRUE(annot);

  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(annot.get()));
  ByteString appearance = GetNormalAppearanceStreamBytes(annot.get());
  ASSERT_FALSE(appearance.IsEmpty());
  EXPECT_EQ("68a94799890022965d780f65db1e7430",
            GenerateMD5Base16(appearance.unsigned_span()));
}

TEST_F(FPDFAnnotEmbedderTest,
       StandardFontDefaultAppearanceRoundTripsThroughDr) {
  static constexpr std::array<FPDF_STANDARD_FONT, 14> kStandardFonts = {
      FPDF_FONT_COURIER,
      FPDF_FONT_COURIER_BOLD,
      FPDF_FONT_COURIER_BOLDITALIC,
      FPDF_FONT_COURIER_ITALIC,
      FPDF_FONT_HELVETICA,
      FPDF_FONT_HELVETICA_BOLD,
      FPDF_FONT_HELVETICA_BOLDITALIC,
      FPDF_FONT_HELVETICA_ITALIC,
      FPDF_FONT_TIMES_ROMAN,
      FPDF_FONT_TIMES_BOLD,
      FPDF_FONT_TIMES_BOLDITALIC,
      FPDF_FONT_TIMES_ITALIC,
      FPDF_FONT_SYMBOL,
      FPDF_FONT_ZAPFDINGBATS,
  };

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);

  for (FPDF_STANDARD_FONT expected_font : kStandardFonts) {
    SCOPED_TRACE(static_cast<int>(expected_font));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), expected_font,
                                               17.0f, 12, 34, 56));

    FPDF_STANDARD_FONT actual_font = FPDF_FONT_UNKNOWN;
    float font_size = 0.0f;
    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;
    ASSERT_TRUE(EPDFAnnot_GetDefaultAppearance(annot.get(), &actual_font,
                                               &font_size, &r, &g, &b));
    EXPECT_EQ(expected_font, actual_font);
    EXPECT_FLOAT_EQ(17.0f, font_size);
    EXPECT_EQ(12u, r);
    EXPECT_EQ(34u, g);
    EXPECT_EQ(56u, b);
  }
}

TEST_F(FPDFAnnotEmbedderTest,
       DefaultAppearancePrefersAnnotationDrThenFallsBackToAcroFormDr) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);

  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot.get());
  ASSERT_TRUE(context);
  RetainPtr<CPDF_Dictionary> annot_dict = context->GetMutableAnnotDict();
  ASSERT_TRUE(annot_dict);
  annot_dict->SetNewFor<CPDF_String>("DA", "/SharedAlias 17 Tf 0 g");

  RetainPtr<CPDF_Dictionary> annot_font =
      annot_dict->SetNewFor<CPDF_Dictionary>("DR")
          ->SetNewFor<CPDF_Dictionary>("Font")
          ->SetNewFor<CPDF_Dictionary>("SharedAlias");
  annot_font->SetNewFor<CPDF_Name>("Type", "Font");
  annot_font->SetNewFor<CPDF_Name>("Subtype", "Type1");
  annot_font->SetNewFor<CPDF_Name>("BaseFont", "Courier");

  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc.get());
  ASSERT_TRUE(cpdf_doc);
  RetainPtr<CPDF_Dictionary> acroform_font =
      cpdf_doc->GetMutableRoot()
          ->SetNewFor<CPDF_Dictionary>("AcroForm")
          ->SetNewFor<CPDF_Dictionary>("DR")
          ->SetNewFor<CPDF_Dictionary>("Font")
          ->SetNewFor<CPDF_Dictionary>("SharedAlias");
  acroform_font->SetNewFor<CPDF_Name>("Type", "Font");
  acroform_font->SetNewFor<CPDF_Name>("Subtype", "Type1");
  acroform_font->SetNewFor<CPDF_Name>("BaseFont", "Times-Roman");

  FPDF_STANDARD_FONT font = FPDF_FONT_UNKNOWN;
  float font_size = 0.0f;
  unsigned int r = 0;
  unsigned int g = 0;
  unsigned int b = 0;
  ASSERT_TRUE(EPDFAnnot_GetDefaultAppearance(annot.get(), &font, &font_size, &r,
                                             &g, &b));
  EXPECT_EQ(FPDF_FONT_COURIER, font);

  annot_dict->RemoveFor("DR");
  ASSERT_TRUE(EPDFAnnot_GetDefaultAppearance(annot.get(), &font, &font_size, &r,
                                             &g, &b));
  EXPECT_EQ(FPDF_FONT_TIMES_ROMAN, font);
}

TEST_F(FPDFAnnotEmbedderTest, FreeTextAppearanceUsesRegisteredMemoryFont) {
  ScopedRegisteredFonts scoped_fonts;

  std::vector<uint8_t> font_data = LoadNotoSansSCFontData();
  ASSERT_FALSE(font_data.empty());

  EPDF_FONT_ID font_id =
      EPDFFont_RegisterMemFont64("NotoSansSC", /*weight=*/400, /*italic=*/0,
                                 font_data.data(), font_data.size());
  ASSERT_NE(0u, font_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);

  const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"这是第一句。");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(annot.get(), font_id,
                                                           18.0f, 0, 0, 0));

  FPDF_STANDARD_FONT font = FPDF_FONT_COURIER;
  float font_size = 0.0f;
  unsigned int r = 0;
  unsigned int g = 0;
  unsigned int b = 0;
  ASSERT_TRUE(EPDFAnnot_GetDefaultAppearance(annot.get(), &font, &font_size, &r,
                                             &g, &b));
  EXPECT_EQ(FPDF_FONT_UNKNOWN, font);
  EXPECT_FLOAT_EQ(18.0f, font_size);

  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  EXPECT_THAT(GetNormalAppearance(annot.get()), HasSubstr(L"/ERegF"));
}

TEST_F(FPDFAnnotEmbedderTest, FreeTextAppearanceFallsBackToRegisteredFont) {
  ScopedRegisteredFonts scoped_fonts;

  std::vector<uint8_t> font_data = LoadNotoSansSCFontData();
  ASSERT_FALSE(font_data.empty());

  EPDF_FONT_ID font_id =
      EPDFFont_RegisterMemFont64("NotoSansSC", /*weight=*/400, /*italic=*/0,
                                 font_data.data(), font_data.size());
  ASSERT_NE(0u, font_id);
  ASSERT_TRUE(EPDFFont_AddFallbackFont(font_id));

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);

  const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"Hello 这是");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), FPDF_FONT_HELVETICA,
                                             18.0f, 0, 0, 0));

  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  std::wstring appearance = GetNormalAppearance(annot.get());
  EXPECT_THAT(appearance, HasSubstr(L"/Helv"));
  EXPECT_THAT(appearance, HasSubstr(L"/ERegF"));
}

TEST_F(FPDFAnnotEmbedderTest, FreeTextKoreanUsesRegisteredDroidFallbackFont) {
  ScopedRegisteredFonts scoped_fonts;

  EPDF_FONT_ID font_id = RegisterDroidSansFallbackFullFont();
  ASSERT_NE(0u, font_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);

  const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"Hello \xD55C\xAE00");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), FPDF_FONT_HELVETICA,
                                             18.0f, 0, 0, 0));

  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  ExpectRegisteredAppearanceMapsUnicode(annot.get(), font_id,
                                        {L'\xD55C', L'\xAE00'});
}

TEST_F(FPDFAnnotEmbedderTest, FreeTextRegisteredFontEmbedsSubsetInSavedPdf) {
  ScopedRegisteredFonts scoped_fonts;

  std::vector<uint8_t> font_data = LoadRobotoFontData();
  ASSERT_FALSE(font_data.empty());

  EPDF_FONT_ID font_id =
      EPDFFont_RegisterMemFont64("Roboto", /*weight=*/400, /*italic=*/0,
                                 font_data.data(), font_data.size());
  ASSERT_NE(0u, font_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);

  const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"ABC");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(annot.get(), font_id,
                                                           18.0f, 0, 0, 0));

  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  RetainPtr<const CPDF_Dictionary> font_dict =
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(font_id));
  ASSERT_TRUE(font_dict);
  EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(font_dict.Get()));
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), 'A'));
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), 'B'));
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), 'C'));

  unsigned long saved_size = 0;
  void* saved_buffer =
      EPDF_SaveDocumentToOwnedBuffer(doc.get(), /*flags=*/0, &saved_size);
  ASSERT_TRUE(saved_buffer);
  std::string saved_pdf(static_cast<const char*>(saved_buffer), saved_size);
  EPDF_FreeBuffer(saved_buffer);

  EXPECT_LT(saved_pdf.size(), font_data.size() / 2);
}

TEST_F(FPDFAnnotEmbedderTest, FreeTextRegistersFontFromFileAccess) {
  ScopedRegisteredFonts scoped_fonts;

  std::vector<uint8_t> font_data = LoadRobotoFontData();
  ASSERT_FALSE(font_data.empty());
  const size_t original_font_size = font_data.size();
  MemoryFileAccess font_access(std::move(font_data));

  EPDF_FONT_ID font_id = EPDFFont_RegisterFont("Roboto", /*weight=*/400,
                                               /*italic=*/0, &font_access);
  ASSERT_NE(0u, font_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);

  const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"ABC");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(annot.get(), font_id,
                                                           18.0f, 0, 0, 0));

  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  RetainPtr<const CPDF_Dictionary> font_dict =
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(font_id));
  ASSERT_TRUE(font_dict);
  EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(font_dict.Get()));
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), 'A'));
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), 'B'));
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), 'C'));

  unsigned long saved_size = 0;
  void* saved_buffer =
      EPDF_SaveDocumentToOwnedBuffer(doc.get(), /*flags=*/0, &saved_size);
  ASSERT_TRUE(saved_buffer);
  std::string saved_pdf(static_cast<const char*>(saved_buffer), saved_size);
  EPDF_FreeBuffer(saved_buffer);

  EXPECT_LT(saved_pdf.size(), original_font_size / 2);
}

namespace {

// A1 helpers: the /DR entry a /DA alias names, and its Type0 descriptor.
RetainPtr<const CPDF_Dictionary> GetDrFontEntry(FPDF_DOCUMENT doc,
                                                const ByteString& alias) {
  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc);
  const CPDF_Dictionary* root = cpdf_doc ? cpdf_doc->GetRoot() : nullptr;
  RetainPtr<const CPDF_Dictionary> acroform =
      root ? root->GetDictFor("AcroForm") : nullptr;
  RetainPtr<const CPDF_Dictionary> dr =
      acroform ? acroform->GetDictFor("DR") : nullptr;
  RetainPtr<const CPDF_Dictionary> fonts =
      dr ? dr->GetDictFor("Font") : nullptr;
  return fonts ? fonts->GetDictFor(alias.AsStringView()) : nullptr;
}

RetainPtr<const CPDF_Dictionary> GetType0FontDescriptor(
    const CPDF_Dictionary* font_dict) {
  RetainPtr<const CPDF_Array> descendants =
      font_dict ? font_dict->GetArrayFor("DescendantFonts") : nullptr;
  RetainPtr<const CPDF_Dictionary> cid_font =
      descendants ? descendants->GetDictAt(0) : nullptr;
  return cid_font ? cid_font->GetDictFor("FontDescriptor") : nullptr;
}

constexpr char kRegisteredFontHintKey[] = "EmbedPDFRegisteredFontId";

// A saved-file shape of an EmbedPDF registered font: Type0 with the family
// identity in the descriptor and a session hint on the dictionary. No program
// is needed to exercise resolution.
RetainPtr<CPDF_Dictionary> MakeRegisteredFontDictForTest(CPDF_Document* doc,
                                                         const char* family,
                                                         int weight,
                                                         bool italic,
                                                         EPDF_FONT_ID hint) {
  auto descriptor = doc->NewIndirect<CPDF_Dictionary>();
  descriptor->SetNewFor<CPDF_Name>("Type", "FontDescriptor");
  descriptor->SetNewFor<CPDF_String>("FontFamily", ByteString(family));
  descriptor->SetNewFor<CPDF_Number>("FontWeight", weight);
  descriptor->SetNewFor<CPDF_Number>("ItalicAngle", italic ? -12 : 0);
  auto cid_font = doc->NewIndirect<CPDF_Dictionary>();
  cid_font->SetNewFor<CPDF_Name>("Type", "Font");
  cid_font->SetNewFor<CPDF_Name>("Subtype", "CIDFontType2");
  cid_font->SetNewFor<CPDF_Reference>("FontDescriptor", doc,
                                      descriptor->GetObjNum());
  auto font = doc->NewIndirect<CPDF_Dictionary>();
  font->SetNewFor<CPDF_Name>("Type", "Font");
  font->SetNewFor<CPDF_Name>("Subtype", "Type0");
  font->SetNewFor<CPDF_Array>("DescendantFonts")
      ->AppendNew<CPDF_Reference>(doc, cid_font->GetObjNum());
  font->SetNewFor<CPDF_String>(kRegisteredFontHintKey,
                               ByteString::Format("%u", hint));
  return font;
}

// The pre-A1 /DR marker: a descriptor-less /Type1 stub with the hint.
RetainPtr<CPDF_Dictionary> MakeLegacyMarkerForTest(CPDF_Document* doc,
                                                   const char* base_font,
                                                   EPDF_FONT_ID hint) {
  auto marker = doc->NewIndirect<CPDF_Dictionary>();
  marker->SetNewFor<CPDF_Name>("Type", "Font");
  marker->SetNewFor<CPDF_Name>("Subtype", "Type1");
  marker->SetNewFor<CPDF_Name>("BaseFont", base_font);
  marker->SetNewFor<CPDF_Name>("Encoding", "WinAnsiEncoding");
  marker->SetNewFor<CPDF_String>(kRegisteredFontHintKey,
                                 ByteString::Format("%u", hint));
  return marker;
}

// sfnt table directory helpers for the A2 tests: locate a table, and rewrite
// the OS/2 fsType field of an in-memory TrueType font.
struct SfntTable {
  uint32_t offset = 0;
  uint32_t length = 0;
};

std::optional<SfntTable> FindSfntTable(pdfium::span<const uint8_t> font,
                                       const char* tag) {
  if (font.size() < 12) {
    return std::nullopt;
  }
  const size_t num_tables = (font[4] << 8) | font[5];
  for (size_t i = 0; i < num_tables; ++i) {
    const size_t record = 12 + i * 16;
    if (record + 16 > font.size()) {
      return std::nullopt;
    }
    if (memcmp(font.data() + record, tag, 4) != 0) {
      continue;
    }
    SfntTable table;
    table.offset = (font[record + 8] << 24) | (font[record + 9] << 16) |
                   (font[record + 10] << 8) | font[record + 11];
    table.length = (font[record + 12] << 24) | (font[record + 13] << 16) |
                   (font[record + 14] << 8) | font[record + 15];
    return table;
  }
  return std::nullopt;
}

std::vector<uint8_t> WithFsType(std::vector<uint8_t> font, uint16_t fs_type) {
  std::optional<SfntTable> os2 = FindSfntTable(font, "OS/2");
  if (!os2.has_value() || os2->offset + 10 > font.size()) {
    ADD_FAILURE() << "no OS/2 table";
    return {};
  }
  font[os2->offset + 8] = static_cast<uint8_t>(fs_type >> 8);
  font[os2->offset + 9] = static_cast<uint8_t>(fs_type & 0xff);
  return font;
}

std::vector<uint8_t> EmbeddedFontProgram(const CPDF_Dictionary* font_dict) {
  RetainPtr<const CPDF_Dictionary> descriptor =
      GetType0FontDescriptor(font_dict);
  RetainPtr<const CPDF_Stream> program =
      descriptor ? descriptor->GetStreamFor("FontFile2") : nullptr;
  if (!program) {
    return {};
  }
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(std::move(program));
  acc->LoadAllDataFiltered();
  pdfium::span<const uint8_t> span = acc->GetSpan();
  return std::vector<uint8_t>(span.begin(), span.end());
}

RetainPtr<CPDF_Dictionary> GetMutableDrFontResources(FPDF_DOCUMENT doc) {
  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc);
  return cpdf_doc->GetMutableRoot()
      ->GetOrCreateDictFor("AcroForm")
      ->GetOrCreateDictFor("DR")
      ->GetOrCreateDictFor("Font");
}

}  // namespace

// A1: /DR names the real embedded font the appearance uses. Before the
// appearance exists nothing is written to /DR (no descriptor-less marker,
// which made Acrobat report "Bad parameter" on edit); afterwards the /DR
// entry and the appearance's font resource are the same Type0 subset, whose
// descriptor carries the family/weight identity and whose dictionary carries
// the private session hint.
TEST_F(FPDFAnnotEmbedderTest, FreeTextRegisteredFontInstallsRealDrEntry) {
  ScopedRegisteredFonts scoped_fonts;

  std::vector<uint8_t> font_data = LoadRobotoFontData();
  ASSERT_FALSE(font_data.empty());
  EPDF_FONT_ID font_id =
      EPDFFont_RegisterMemFont64("Roboto", /*weight=*/400, /*italic=*/0,
                                 font_data.data(), font_data.size());
  ASSERT_NE(0u, font_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"ABC");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(annot.get(), font_id,
                                                           18.0f, 0, 0, 0));

  const ByteString alias = GetDefaultAppearanceFontAlias(annot.get());
  EXPECT_EQ(RegisteredFontAlias(font_id), alias);
  // No placeholder before the appearance is generated.
  EXPECT_FALSE(GetDrFontEntry(doc.get(), alias));

  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

  RetainPtr<const CPDF_Dictionary> dr_entry = GetDrFontEntry(doc.get(), alias);
  ASSERT_TRUE(dr_entry);
  RetainPtr<const CPDF_Dictionary> ap_font =
      GetAppearanceFontDict(annot.get(), alias);
  ASSERT_TRUE(ap_font);
  EXPECT_EQ(dr_entry.Get(), ap_font.Get());
  EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(dr_entry.Get()));
  EXPECT_EQ(ByteString::Format("%u", font_id),
            dr_entry->GetByteStringFor(kRegisteredFontHintKey));

  RetainPtr<const CPDF_Dictionary> descriptor =
      GetType0FontDescriptor(dr_entry.Get());
  ASSERT_TRUE(descriptor);
  EXPECT_EQ(L"Roboto", descriptor->GetUnicodeTextFor("FontFamily"));
  EXPECT_EQ(400, descriptor->GetIntegerFor("FontWeight"));
  EXPECT_EQ(0, descriptor->GetIntegerFor("ItalicAngle"));
  EXPECT_TRUE(descriptor->GetStreamFor("FontFile2"));
}

// A1-1: a document saved in one session resolves the right face in a session
// where the same fonts were registered in a different order (different ids).
// Identity is the family in the descriptor, not the numeric hint or the alias.
TEST_F(FPDFAnnotEmbedderTest,
       FreeTextRegisteredFontResolvesByFamilyAfterReregistration) {
  ScopedRegisteredFonts scoped_fonts;

  std::vector<uint8_t> roboto = LoadRobotoFontData();
  std::vector<uint8_t> amiri = LoadAmiriFontData();
  ASSERT_FALSE(roboto.empty());
  ASSERT_FALSE(amiri.empty());

  // Session 1: Roboto first, Amiri second; author with Amiri (both cover
  // Latin, so the glyphs alone cannot tell them apart).
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  EPDF_FONT_ID amiri_id = EPDFFont_RegisterMemFont64(
      "Amiri", /*weight=*/400, /*italic=*/0, amiri.data(), amiri.size());
  ASSERT_NE(0u, roboto_id);
  ASSERT_NE(0u, amiri_id);

  std::string saved_pdf;
  {
    ScopedFPDFDocument doc(FPDF_CreateNewDocument());
    ASSERT_TRUE(doc);
    ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
    ASSERT_TRUE(annot);
    const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ScopedFPDFWideString contents = GetFPDFWideString(L"ABC");
    ASSERT_TRUE(
        FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
        annot.get(), amiri_id, 18.0f, 0, 0, 0));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

    unsigned long saved_size = 0;
    void* saved_buffer =
        EPDF_SaveDocumentToOwnedBuffer(doc.get(), /*flags=*/0, &saved_size);
    ASSERT_TRUE(saved_buffer);
    saved_pdf.assign(static_cast<const char*>(saved_buffer), saved_size);
    EPDF_FreeBuffer(saved_buffer);
  }

  // Session 2: the registry is cleared and the fonts come back in the other
  // order, so every numeric id differs from the hint in the saved file.
  EPDFFont_ClearRegisteredFonts();
  EPDF_FONT_ID amiri_id_2 = EPDFFont_RegisterMemFont64(
      "Amiri", /*weight=*/400, /*italic=*/0, amiri.data(), amiri.size());
  EPDF_FONT_ID roboto_id_2 = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, amiri_id_2);
  ASSERT_NE(0u, roboto_id_2);
  ASSERT_NE(amiri_id, amiri_id_2);
  ASSERT_NE(roboto_id, roboto_id_2);
  ASSERT_EQ(roboto_id_2, amiri_id + 2u);  // the stale hint must not win by luck

  ScopedFPDFDocument doc(FPDF_LoadMemDocument(
      saved_pdf.data(), static_cast<int>(saved_pdf.size()), nullptr));
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(annot);

  const ByteString alias = GetDefaultAppearanceFontAlias(annot.get());
  EXPECT_EQ(RegisteredFontAlias(amiri_id), alias);  // stale alias, still fine
  ScopedFPDFWideString contents = GetFPDFWideString(L"ABD");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

  RetainPtr<const CPDF_Dictionary> ap_font =
      GetAppearanceFontDict(annot.get(), alias);
  ASSERT_TRUE(ap_font);
  EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(ap_font.Get()));
  EXPECT_TRUE(ap_font->GetNameFor("BaseFont").Contains("Amiri"));
  EXPECT_FALSE(ap_font->GetNameFor("BaseFont").Contains("Roboto"));
  EXPECT_TRUE(AppearanceFontMapsUnicode(ap_font.Get(), 'D'));

  // /DR follows the regenerated appearance.
  RetainPtr<const CPDF_Dictionary> dr_entry = GetDrFontEntry(doc.get(), alias);
  ASSERT_TRUE(dr_entry);
  EXPECT_EQ(dr_entry.Get(), ap_font.Get());
  EXPECT_EQ(ByteString::Format("%u", amiri_id_2),
            dr_entry->GetByteStringFor(kRegisteredFontHintKey));
}

// Files saved before A1 carry a descriptor-less /Type1 marker in /DR. They
// still resolve, and regenerating the appearance replaces the marker with the
// real subset and drops the marker object.
TEST_F(FPDFAnnotEmbedderTest, FreeTextLegacyMarkerDrEntryUpgradesToRealFont) {
  ScopedRegisteredFonts scoped_fonts;

  std::vector<uint8_t> font_data = LoadRobotoFontData();
  ASSERT_FALSE(font_data.empty());
  EPDF_FONT_ID font_id =
      EPDFFont_RegisterMemFont64("Roboto", /*weight=*/400, /*italic=*/0,
                                 font_data.data(), font_data.size());
  ASSERT_NE(0u, font_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);

  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc.get());
  ASSERT_TRUE(cpdf_doc);
  RetainPtr<CPDF_Dictionary> font_resources =
      cpdf_doc->GetMutableRoot()
          ->GetOrCreateDictFor("AcroForm")
          ->GetOrCreateDictFor("DR")
          ->GetOrCreateDictFor("Font");
  const ByteString alias = RegisteredFontAlias(font_id);
  auto marker = cpdf_doc->NewIndirect<CPDF_Dictionary>();
  marker->SetNewFor<CPDF_Name>("Type", "Font");
  marker->SetNewFor<CPDF_Name>("Subtype", "Type1");
  marker->SetNewFor<CPDF_Name>("BaseFont", "Roboto");
  marker->SetNewFor<CPDF_Name>("Encoding", "WinAnsiEncoding");
  marker->SetNewFor<CPDF_String>(kRegisteredFontHintKey,
                                 ByteString::Format("%u", font_id));
  const uint32_t marker_obj_num = marker->GetObjNum();
  font_resources->SetNewFor<CPDF_Reference>(alias, cpdf_doc, marker_obj_num);
  // The same marker object under a second name: replacing one reference must
  // not touch the other (unreachable-object cleanup is a save-time concern).
  font_resources->SetNewFor<CPDF_Reference>("SharedMarker", cpdf_doc,
                                            marker_obj_num);

  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"ABC");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  const std::wstring da_text =
      L"0 g /" + std::wstring(alias.begin(), alias.end()) + L" 18 Tf";
  ScopedFPDFWideString da = GetFPDFWideString(da_text);
  ASSERT_TRUE(FPDFAnnot_SetStringValue(annot.get(), "DA", da.get()));

  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

  RetainPtr<const CPDF_Dictionary> dr_entry = GetDrFontEntry(doc.get(), alias);
  ASSERT_TRUE(dr_entry);
  EXPECT_EQ("Type0", dr_entry->GetNameFor("Subtype"));
  EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(dr_entry.Get()));
  EXPECT_EQ(dr_entry.Get(), GetAppearanceFontDict(annot.get(), alias).Get());
  EXPECT_TRUE(AppearanceFontMapsUnicode(dr_entry.Get(), 'A'));
  RetainPtr<const CPDF_Dictionary> shared =
      GetDrFontEntry(doc.get(), "SharedMarker");
  ASSERT_TRUE(shared);
  EXPECT_EQ(marker_obj_num, shared->GetObjNum());
  EXPECT_EQ("Type1", shared->GetNameFor("Subtype"));
  EXPECT_TRUE(cpdf_doc->GetIndirectObject(marker_obj_num));
}

// Resolution is by the stored family; a numeric hint that points at some
// other registered font never wins (ids are session-local and get reused).
TEST_F(FPDFAnnotEmbedderTest, RegisteredFontResolutionPrefersStoredFamily) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  std::vector<uint8_t> amiri = LoadAmiriFontData();
  ASSERT_FALSE(roboto.empty());
  ASSERT_FALSE(amiri.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  EPDF_FONT_ID amiri_id = EPDFFont_RegisterMemFont64(
      "Amiri", /*weight=*/400, /*italic=*/0, amiri.data(), amiri.size());
  ASSERT_NE(0u, roboto_id);
  ASSERT_NE(0u, amiri_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc.get());

  // Requested family present, hint elsewhere: the family wins.
  RetainPtr<CPDF_Dictionary> amiri_with_roboto_hint =
      MakeRegisteredFontDictForTest(cpdf_doc, "Amiri", 400, false, roboto_id);
  EXPECT_EQ(amiri_id, CPDF_AnnotFontSubset::ResolveRegisteredFont(
                          amiri_with_roboto_hint.Get()));

  // Exact family spelling outranks a normalised match.
  EPDF_FONT_ID noto_spaced = EPDFFont_RegisterMemFont64(
      "Noto Sans", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  EPDF_FONT_ID noto_joined = EPDFFont_RegisterMemFont64(
      "NotoSans", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, noto_spaced);
  ASSERT_NE(0u, noto_joined);
  RetainPtr<CPDF_Dictionary> joined =
      MakeRegisteredFontDictForTest(cpdf_doc, "NotoSans", 400, false, 0);
  EXPECT_EQ(noto_joined,
            CPDF_AnnotFontSubset::ResolveRegisteredFont(joined.Get()));
  RetainPtr<CPDF_Dictionary> spaced =
      MakeRegisteredFontDictForTest(cpdf_doc, "Noto Sans", 400, false, 0);
  EXPECT_EQ(noto_spaced,
            CPDF_AnnotFontSubset::ResolveRegisteredFont(spaced.Get()));
}

TEST_F(FPDFAnnotEmbedderTest, RegisteredFontResolutionRefusesConflictingHint) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc.get());

  // Requested family absent, hint points at a live but different font.
  RetainPtr<CPDF_Dictionary> saved_amiri =
      MakeRegisteredFontDictForTest(cpdf_doc, "Amiri", 400, false, roboto_id);
  EXPECT_FALSE(CPDF_AnnotFontSubset::ResolveRegisteredFont(saved_amiri.Get()));

  // Legacy marker: /BaseFont is the identity, a conflicting hint loses.
  RetainPtr<CPDF_Dictionary> marker_amiri =
      MakeLegacyMarkerForTest(cpdf_doc, "Amiri", roboto_id);
  EXPECT_FALSE(CPDF_AnnotFontSubset::ResolveRegisteredFont(marker_amiri.Get()));
  RetainPtr<CPDF_Dictionary> marker_roboto =
      MakeLegacyMarkerForTest(cpdf_doc, "Roboto", roboto_id);
  EXPECT_EQ(roboto_id,
            CPDF_AnnotFontSubset::ResolveRegisteredFont(marker_roboto.Get()));
  // A marker whose base font is registered under another id resolves by name.
  RetainPtr<CPDF_Dictionary> marker_stale_hint =
      MakeLegacyMarkerForTest(cpdf_doc, "Roboto", roboto_id + 1000);
  EXPECT_EQ(roboto_id, CPDF_AnnotFontSubset::ResolveRegisteredFont(
                           marker_stale_hint.Get()));
}

// A stale /DA alias from another session can spell the same name a fallback
// font would take in this session; the appearance must still give every
// entry its own resource name.
TEST_F(FPDFAnnotEmbedderTest,
       FreeTextFallbackAliasNeverCollidesWithStaleDaAlias) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);
  EPDF_FONT_ID droid_id = RegisterDroidSansFallbackFullFont();
  ASSERT_NE(0u, droid_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString latin = GetFPDFWideString(L"ABC");
  ASSERT_TRUE(FPDFAnnot_SetStringValue(annot.get(), "Contents", latin.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
      annot.get(), roboto_id, 18.0f, 0, 0, 0));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

  // Simulate the other session: the Roboto entry lives under the name the
  // Droid fallback would take here.
  const ByteString stale_alias = RegisteredFontAlias(droid_id);
  RetainPtr<CPDF_Dictionary> font_resources =
      GetMutableDrFontResources(doc.get());
  RetainPtr<const CPDF_Dictionary> roboto_entry =
      GetDrFontEntry(doc.get(), RegisteredFontAlias(roboto_id));
  ASSERT_TRUE(roboto_entry);
  font_resources->SetNewFor<CPDF_Reference>(
      stale_alias, CPDFDocumentFromFPDFDocument(doc.get()),
      roboto_entry->GetObjNum());
  font_resources->RemoveFor(RegisteredFontAlias(roboto_id).AsStringView());
  const std::wstring da_text =
      L"0 g /" + std::wstring(stale_alias.begin(), stale_alias.end()) +
      L" 18 Tf";
  ScopedFPDFWideString da = GetFPDFWideString(da_text);
  ASSERT_TRUE(FPDFAnnot_SetStringValue(annot.get(), "DA", da.get()));

  ScopedFPDFWideString mixed = GetFPDFWideString(L"AB\xD55C\xAE00");
  ASSERT_TRUE(FPDFAnnot_SetStringValue(annot.get(), "Contents", mixed.get()));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

  RetainPtr<const CPDF_Dictionary> default_font =
      GetAppearanceFontDict(annot.get(), stale_alias);
  ASSERT_TRUE(default_font);
  EXPECT_TRUE(default_font->GetNameFor("BaseFont").Contains("Roboto"));
  EXPECT_TRUE(AppearanceFontMapsUnicode(default_font.Get(), 'A'));
  RetainPtr<const CPDF_Dictionary> fallback_font =
      GetAppearanceFontDict(annot.get(), stale_alias + "_1");
  ASSERT_TRUE(fallback_font);
  EXPECT_TRUE(fallback_font->GetNameFor("BaseFont").Contains("DroidSans"));
  EXPECT_TRUE(AppearanceFontMapsUnicode(fallback_font.Get(), L'\xD55C'));
  EXPECT_NE(default_font.Get(), fallback_font.Get());
}

// An empty annotation still needs its /DA font in /DR, or the font identity
// is lost on save; the minimal resource carries it across a reopen.
TEST_F(FPDFAnnotEmbedderTest,
       FreeTextEmptyContentsKeepsRegisteredDaFontAcrossSave) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  std::vector<uint8_t> amiri = LoadAmiriFontData();
  ASSERT_FALSE(roboto.empty());
  ASSERT_FALSE(amiri.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);

  std::string saved_pdf;
  {
    ScopedFPDFDocument doc(FPDF_CreateNewDocument());
    ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
    ASSERT_TRUE(annot);
    const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ScopedFPDFWideString empty = GetFPDFWideString(L"");
    ASSERT_TRUE(FPDFAnnot_SetStringValue(annot.get(), "Contents", empty.get()));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
        annot.get(), roboto_id, 18.0f, 0, 0, 0));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

    RetainPtr<const CPDF_Dictionary> dr_entry =
        GetDrFontEntry(doc.get(), RegisteredFontAlias(roboto_id));
    ASSERT_TRUE(dr_entry);
    EXPECT_EQ("Type0", dr_entry->GetNameFor("Subtype"));
    RetainPtr<const CPDF_Dictionary> descriptor =
        GetType0FontDescriptor(dr_entry.Get());
    ASSERT_TRUE(descriptor);
    EXPECT_EQ(L"Roboto", descriptor->GetUnicodeTextFor("FontFamily"));
    RetainPtr<const CPDF_Stream> program =
        descriptor->GetStreamFor("FontFile2");
    ASSERT_TRUE(program);
    // Minimal means minimal: glyph 0 only, not the whole program.
    EXPECT_LT(program->GetRawSize(), roboto.size() / 10);

    unsigned long saved_size = 0;
    void* saved_buffer =
        EPDF_SaveDocumentToOwnedBuffer(doc.get(), /*flags=*/0, &saved_size);
    ASSERT_TRUE(saved_buffer);
    saved_pdf.assign(static_cast<const char*>(saved_buffer), saved_size);
    EPDF_FreeBuffer(saved_buffer);
  }

  EPDFFont_ClearRegisteredFonts();
  ASSERT_NE(0u, EPDFFont_RegisterMemFont64("Amiri", 400, 0, amiri.data(),
                                           amiri.size()));
  EPDF_FONT_ID roboto_id_2 = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(roboto_id, roboto_id_2);

  ScopedFPDFDocument doc(FPDF_LoadMemDocument(
      saved_pdf.data(), static_cast<int>(saved_pdf.size()), nullptr));
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(annot);
  ScopedFPDFWideString contents = GetFPDFWideString(L"ABC");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  RetainPtr<const CPDF_Dictionary> ap_font =
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(roboto_id));
  ASSERT_TRUE(ap_font);
  EXPECT_TRUE(ap_font->GetNameFor("BaseFont").Contains("Roboto"));
  EXPECT_TRUE(AppearanceFontMapsUnicode(ap_font.Get(), 'A'));
}

// Text drawn entirely by a fallback font still keeps the /DA font's identity
// in /DR, and the fallback resource renders the text.
TEST_F(FPDFAnnotEmbedderTest, FreeTextAllFallbackTextKeepsRegisteredDaFont) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);
  EPDF_FONT_ID droid_id = RegisterDroidSansFallbackFullFont();
  ASSERT_NE(0u, droid_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString korean = GetFPDFWideString(L"\xD55C\xAE00");
  ASSERT_TRUE(FPDFAnnot_SetStringValue(annot.get(), "Contents", korean.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
      annot.get(), roboto_id, 18.0f, 0, 0, 0));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

  RetainPtr<const CPDF_Dictionary> dr_entry =
      GetDrFontEntry(doc.get(), RegisteredFontAlias(roboto_id));
  ASSERT_TRUE(dr_entry);
  EXPECT_EQ("Type0", dr_entry->GetNameFor("Subtype"));
  EXPECT_EQ(
      L"Roboto",
      GetType0FontDescriptor(dr_entry.Get())->GetUnicodeTextFor("FontFamily"));
  EXPECT_EQ(
      dr_entry.Get(),
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(roboto_id)).Get());
  ExpectRegisteredAppearanceMapsUnicode(annot.get(), droid_id,
                                        {L'\xD55C', L'\xAE00'});
}

// Regenerating one annotation never rewrites another's appearance resources.
TEST_F(FPDFAnnotEmbedderTest, FreeTextRegeneratingOneAnnotationLeavesTheOther) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);
  const ByteString alias = RegisteredFontAlias(roboto_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  auto make = [&](const wchar_t* text, float top) {
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
    const FS_RECTF rect{50.0f, top, 350.0f, top - 60.0f};
    EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ScopedFPDFWideString contents = GetFPDFWideString(text);
    EXPECT_TRUE(
        FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
    EXPECT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
        annot.get(), roboto_id, 18.0f, 0, 0, 0));
    EXPECT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
    return annot;
  };
  ScopedFPDFAnnotation first = make(L"ABC", 380.0f);
  ScopedFPDFAnnotation second = make(L"DEF", 300.0f);
  RetainPtr<const CPDF_Dictionary> first_font =
      GetAppearanceFontDict(first.get(), alias);
  ASSERT_TRUE(first_font);
  const uint32_t first_font_obj = first_font->GetObjNum();

  ScopedFPDFWideString longer = GetFPDFWideString(L"DEFG");
  ASSERT_TRUE(FPDFAnnot_SetStringValue(second.get(), "Contents", longer.get()));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(second.get()));

  RetainPtr<const CPDF_Dictionary> first_font_after =
      GetAppearanceFontDict(first.get(), alias);
  ASSERT_TRUE(first_font_after);
  EXPECT_EQ(first_font_obj, first_font_after->GetObjNum());
  EXPECT_TRUE(AppearanceFontMapsUnicode(first_font_after.Get(), 'A'));
  EXPECT_FALSE(AppearanceFontMapsUnicode(first_font_after.Get(), 'G'));
  RetainPtr<const CPDF_Dictionary> second_font =
      GetAppearanceFontDict(second.get(), alias);
  ASSERT_TRUE(second_font);
  EXPECT_TRUE(AppearanceFontMapsUnicode(second_font.Get(), 'G'));
  // /DR follows the last generated appearance.
  EXPECT_EQ(second_font.Get(), GetDrFontEntry(doc.get(), alias).Get());
}

// Rendering an annotation that has no appearance yet must not write /DR.
TEST_F(FPDFAnnotEmbedderTest, FreeTextEphemeralAppearanceLeavesDrUntouched) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"ABC");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
      annot.get(), roboto_id, 18.0f, 0, 0, 0));
  EXPECT_FALSE(GetDrFontEntry(doc.get(), RegisteredFontAlias(roboto_id)));

  ScopedFPDFBitmap bitmap =
      EmbedderTest::RenderPageWithFlags(page.get(), nullptr, FPDF_ANNOT);
  ASSERT_TRUE(bitmap);
  EXPECT_FALSE(GetDrFontEntry(doc.get(), RegisteredFontAlias(roboto_id)));
  EXPECT_TRUE(GetNormalAppearanceStreamBytes(annot.get()).IsEmpty());
}

// Widgets share the resolution path: their /DR entry is the real font too.
TEST_F(FPDFAnnotEmbedderTest, WidgetRegisteredFontInstallsRealDrEntry) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);
  const ByteString alias = RegisteredFontAlias(roboto_id);

  CreateEmptyDocument();
  {
    ScopedFPDFPage page(FPDFPage_New(document(), 0, 400, 400));
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_WIDGET));
    ASSERT_TRUE(annot);
    const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
        annot.get(), roboto_id, 12.0f, 0, 0, 0));

    ScopedFPDFWideString field_name = GetFPDFWideString(L"roboto_text");
    const uint32_t field = EPDFForm_CreateField(
        document(), 4 /* EPDF_FORMFIELD_FAMILY_TEXT */, field_name.get());
    ASSERT_GT(field, 0u);
    ASSERT_TRUE(EPDFForm_AttachWidget(
        document(), field, EPDFAnnot_GetObjectNumber(annot.get()), nullptr));
    ScopedFPDFWideString value = GetFPDFWideString(L"hello");
    ASSERT_TRUE(EPDFForm_SetTextValue(document(), field, value.get(), nullptr,
                                      0, nullptr));
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(annot.get()));

    RetainPtr<const CPDF_Dictionary> dr_entry =
        GetDrFontEntry(document(), alias);
    ASSERT_TRUE(dr_entry);
    // Form field text is embedded whole under the DEFAULT policy (Phase C
    // note §2): the program is shared by every widget that fills in later.
    EXPECT_TRUE(
        AppearanceFontEmbedsWholeProgram(dr_entry.Get(), roboto.size()));
    EXPECT_EQ(dr_entry.Get(), GetAppearanceFontDict(annot.get(), alias).Get());
    EXPECT_TRUE(AppearanceFontMapsUnicode(dr_entry.Get(), 'h'));
    EXPECT_EQ(L"Roboto", GetType0FontDescriptor(dr_entry.Get())
                             ->GetUnicodeTextFor("FontFamily"));
  }
  CloseDocument();
}

// Redaction labels share it as well.
TEST_F(FPDFAnnotEmbedderTest, RedactLabelRegisteredFontInstallsRealDrEntry) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 200, 200));
  {
    ScopedFPDFAnnotation annot =
        CreateRedactAnnot(page.get(), {20, 150, 180, 50});
    ASSERT_TRUE(annot);
    ScopedFPDFWideString text = GetFPDFWideString(L"SECRET");
    ASSERT_TRUE(EPDFAnnot_SetOverlayText(annot.get(), text.get()));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
        annot.get(), roboto_id, 12.0f, 255, 255, 255));
    ASSERT_TRUE(EPDFAnnot_ApplyRedaction(page.get(), annot.get(), nullptr));
  }
  RetainPtr<const CPDF_Dictionary> dr_entry =
      GetDrFontEntry(doc.get(), RegisteredFontAlias(roboto_id));
  ASSERT_TRUE(dr_entry);
  EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(dr_entry.Get()));
  EXPECT_TRUE(AppearanceFontMapsUnicode(dr_entry.Get(), 'S'));
  EXPECT_EQ(
      L"Roboto",
      GetType0FontDescriptor(dr_entry.Get())->GetUnicodeTextFor("FontFamily"));
}

// Opt-in fixture for the manual Acrobat check of A1 (a FreeText in a
// registered font whose /DR entry is the real subset). Skipped unless
// EPDF_A1_FIXTURE_FONT (a TrueType file, e.g. Noto Sans) and
// EPDF_A1_FIXTURE_PATH (output PDF) are set; EPDF_A1_FIXTURE_FAMILY names the
// family Acrobat should re-resolve (default "Noto Sans").
TEST_F(FPDFAnnotEmbedderTest, WriteA1AcrobatFixtureFromEnv) {
  const char* font_path = getenv("EPDF_A1_FIXTURE_FONT");
  const char* out_path = getenv("EPDF_A1_FIXTURE_PATH");
  if (!font_path || !out_path) {
    GTEST_SKIP() << "set EPDF_A1_FIXTURE_FONT and EPDF_A1_FIXTURE_PATH";
  }
  const char* family_env = getenv("EPDF_A1_FIXTURE_FAMILY");
  const std::string family = family_env ? family_env : "Noto Sans";
  // EPDF_A1_FIXTURE_TEXT="" writes an empty box: the minimal glyph-0 resource
  // that keeps the /DA font's identity, for Acrobat to validate.
  const char* text_env = getenv("EPDF_A1_FIXTURE_TEXT");
  const std::string text = text_env ? text_env : "Hello";

  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> font_data = GetFileContents(font_path);
  ASSERT_FALSE(font_data.empty());
  EPDF_FONT_ID font_id =
      EPDFFont_RegisterMemFont64(family.c_str(), /*weight=*/400, /*italic=*/0,
                                 font_data.data(), font_data.size());
  ASSERT_NE(0u, font_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 612, 792));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  // FS_RECTF is {left, top, right, bottom}; keep the rect normalised so the
  // fixture is exactly what Acrobat expects to see.
  const FS_RECTF rect{72.0f, 650.0f, 420.0f, 570.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents =
      GetFPDFWideString(std::wstring(text.begin(), text.end()));
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(annot.get(), font_id,
                                                           24.0f, 0, 0, 0));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  unsigned long saved_size = 0;
  void* saved_buffer =
      EPDF_SaveDocumentToOwnedBuffer(doc.get(), /*flags=*/0, &saved_size);
  ASSERT_TRUE(saved_buffer);
  std::ofstream out(out_path, std::ios::binary);
  out.write(static_cast<const char*>(saved_buffer), saved_size);
  EPDF_FreeBuffer(saved_buffer);
  ASSERT_TRUE(out.good());
}

// A2: fsType is read at registration. Restricted and bitmap-only licences
// are refused; preview-and-print registers but may not author until the app
// asserts a licence; editable and installable author freely.
TEST_F(FPDFAnnotEmbedderTest, RegistryHonoursFsTypeEmbeddingPermissions) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  ASSERT_TRUE(FindSfntTable(roboto, "OS/2").has_value());

  auto register_with = [&](uint16_t fs_type) {
    std::vector<uint8_t> font = WithFsType(roboto, fs_type);
    return EPDFFont_RegisterMemFont64("Roboto", /*weight=*/400, /*italic=*/0,
                                      font.data(), font.size());
  };

  EXPECT_EQ(0u, register_with(0x0002));  // restricted
  EXPECT_EQ(0u, register_with(0x0200));  // bitmap only
  EXPECT_EQ(0u, register_with(0x0202));  // bitmap only wins over any level

  EPDF_FONT_ID installable = register_with(0x0000);
  ASSERT_NE(0u, installable);
  EXPECT_EQ(EPDF_FONT_EMBEDDING_INSTALLABLE,
            EPDFFont_GetEmbeddingPermission(installable));
  EXPECT_TRUE(EPDFFont_IsEditingAuthorized(installable));

  EPDF_FONT_ID editable = register_with(0x0008);
  ASSERT_NE(0u, editable);
  EXPECT_EQ(EPDF_FONT_EMBEDDING_EDITABLE,
            EPDFFont_GetEmbeddingPermission(editable));
  EXPECT_TRUE(EPDFFont_IsEditingAuthorized(editable));

  // Several level bits set: the least restrictive applies.
  EPDF_FONT_ID mixed = register_with(0x000E);
  ASSERT_NE(0u, mixed);
  EXPECT_EQ(EPDF_FONT_EMBEDDING_EDITABLE,
            EPDFFont_GetEmbeddingPermission(mixed));

  EPDF_FONT_ID preview = register_with(0x0004);
  ASSERT_NE(0u, preview);
  EXPECT_EQ(EPDF_FONT_EMBEDDING_PREVIEW_AND_PRINT,
            EPDFFont_GetEmbeddingPermission(preview));
  EXPECT_FALSE(EPDFFont_IsEditingAuthorized(preview));
  EXPECT_EQ(-1, EPDFFont_GetEmbeddingPermission(preview + 1000));
  EXPECT_FALSE(EPDFFont_AuthorizeEditing(preview + 1000));

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"ABC");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));

  // Preview-and-print may not author…
  EXPECT_FALSE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
      annot.get(), preview, 18.0f, 0, 0, 0));
  // …until the app asserts a licence.
  EXPECT_TRUE(EPDFFont_AuthorizeEditing(preview));
  EXPECT_TRUE(EPDFFont_IsEditingAuthorized(preview));
  EXPECT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(annot.get(), preview,
                                                           18.0f, 0, 0, 0));
  EXPECT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(preview)).Get()));
}

// A preview-and-print fallback font renders existing page text but is not
// used to write new text until authorized.
TEST_F(FPDFAnnotEmbedderTest,
       PreviewAndPrintFallbackDoesNotAuthorUntilAuthorized) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  std::vector<uint8_t> droid = LoadDroidSansFallbackFullFontData();
  ASSERT_FALSE(roboto.empty());
  ASSERT_FALSE(droid.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);
  std::vector<uint8_t> droid_preview = WithFsType(droid, 0x0004);
  EPDF_FONT_ID droid_id = EPDFFont_RegisterMemFont64(
      "DroidSansFallbackFull", /*weight=*/400, /*italic=*/0,
      droid_preview.data(), droid_preview.size());
  ASSERT_NE(0u, droid_id);
  ASSERT_TRUE(EPDFFont_AddFallbackFont(droid_id));

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString korean = GetFPDFWideString(L"AB\xD55C\xAE00");
  ASSERT_TRUE(FPDFAnnot_SetStringValue(annot.get(), "Contents", korean.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
      annot.get(), roboto_id, 18.0f, 0, 0, 0));

  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  EXPECT_TRUE(
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(roboto_id)));
  EXPECT_FALSE(
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(droid_id)));

  ASSERT_TRUE(EPDFFont_AuthorizeEditing(droid_id));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  ExpectRegisteredAppearanceMapsUnicode(annot.get(), droid_id,
                                        {L'\xD55C', L'\xAE00'});
}

// fsType "no subsetting": the whole program is embedded, untagged.
TEST_F(FPDFAnnotEmbedderTest, NoSubsettingFontEmbedsWholeProgram) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  std::vector<uint8_t> no_subsetting = WithFsType(roboto, 0x0100);
  EPDF_FONT_ID font_id =
      EPDFFont_RegisterMemFont64("Roboto", /*weight=*/400, /*italic=*/0,
                                 no_subsetting.data(), no_subsetting.size());
  ASSERT_NE(0u, font_id);
  EXPECT_EQ(EPDF_FONT_EMBEDDING_INSTALLABLE,
            EPDFFont_GetEmbeddingPermission(font_id));

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"ABC");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(annot.get(), font_id,
                                                           18.0f, 0, 0, 0));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

  RetainPtr<const CPDF_Dictionary> font_dict =
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(font_id));
  ASSERT_TRUE(font_dict);
  EXPECT_EQ("Roboto", font_dict->GetNameFor("BaseFont"));  // no subset tag
  std::vector<uint8_t> program = EmbeddedFontProgram(font_dict.Get());
  EXPECT_EQ(no_subsetting.size(), program.size());
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), 'A'));
}

// A2: a variable font is instanced once at registration. The registered
// weight pins `wght`; everything downstream (layout, subset, embedding) sees a
// static program.
TEST_F(FPDFAnnotEmbedderTest, VariableFontIsInstancedAtRegistration) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> vf = LoadRobotoVariableAbcFontData();
  ASSERT_FALSE(vf.empty());
  ASSERT_TRUE(FindSfntTable(vf, "fvar").has_value());
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());

  EPDF_FONT_ID regular = EPDFFont_RegisterMemFont64(
      "Roboto VF", /*weight=*/400, /*italic=*/0, vf.data(), vf.size());
  EPDF_FONT_ID bold = EPDFFont_RegisterMemFont64(
      "Roboto VF", /*weight=*/700, /*italic=*/0, vf.data(), vf.size());
  EPDF_FONT_ID static_font = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, regular);
  ASSERT_NE(0u, bold);
  ASSERT_NE(0u, static_font);
  EXPECT_TRUE(EPDFFont_IsInstanced(regular));
  EXPECT_TRUE(EPDFFont_IsInstanced(bold));
  EXPECT_FALSE(EPDFFont_IsInstanced(static_font));
  EXPECT_EQ(32u, CFX_FontRegistry::GetSourceHash(regular).size());
  EXPECT_NE(
      std::vector<uint8_t>(CFX_FontRegistry::GetSourceHash(regular).begin(),
                           CFX_FontRegistry::GetSourceHash(regular).end()),
      std::vector<uint8_t>(CFX_FontRegistry::GetSourceHash(bold).begin(),
                           CFX_FontRegistry::GetSourceHash(bold).end()));

  // The two instances are different static programs: 'A' is wider at 700.
  auto glyph_width_of_a = [](EPDF_FONT_ID id) -> int {
    std::unique_ptr<CFX_Font> font = CFX_FontRegistry::CreateFont(id);
    if (!font) {
      return -1;
    }
    for (const auto& item : font->GetCharCodesAndIndices(0x7f)) {
      if (item.char_code == 'A') {
        return font->GetGlyphWidth(item.glyph_index);
      }
    }
    return -1;
  };
  const int regular_a = glyph_width_of_a(regular);
  const int bold_a = glyph_width_of_a(bold);
  ASSERT_GT(regular_a, 0);
  ASSERT_GT(bold_a, 0);
  EXPECT_GT(bold_a, regular_a);

  // What gets embedded is static too.
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"ABC");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(annot.get(), bold,
                                                           18.0f, 0, 0, 0));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  RetainPtr<const CPDF_Dictionary> font_dict =
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(bold));
  ASSERT_TRUE(font_dict);
  std::vector<uint8_t> program = EmbeddedFontProgram(font_dict.Get());
  ASSERT_FALSE(program.empty());
  EXPECT_FALSE(FindSfntTable(program, "fvar").has_value());
  EXPECT_FALSE(FindSfntTable(program, "gvar").has_value());
  EXPECT_TRUE(FindSfntTable(program, "glyf").has_value());
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), 'A'));
  RetainPtr<const CPDF_Dictionary> descriptor =
      GetType0FontDescriptor(font_dict.Get());
  ASSERT_TRUE(descriptor);
  EXPECT_EQ(700, descriptor->GetIntegerFor("FontWeight"));
}

namespace {

std::string GetRichTextJson(FPDF_ANNOTATION annot) {
  const unsigned long length = EPDFAnnot_GetRichTextJSON(annot, nullptr, 0);
  if (length == 0) {
    return std::string();
  }
  std::vector<char> buffer(length);
  EXPECT_EQ(length, EPDFAnnot_GetRichTextJSON(annot, buffer.data(), length));
  return std::string(buffer.data());
}

}  // namespace

// B: without /RC the rich text model is synthesised from /Contents and /DA,
// so callers always get one shape.
TEST_F(FPDFAnnotEmbedderTest, RichTextJsonFromContentsOnly) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"Hello\rWorld");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), FPDF_FONT_HELVETICA,
                                             12.0f, 255, 0, 0));

  const std::string json = GetRichTextJson(annot.get());
  EXPECT_NE(std::string::npos, json.find("\"source\":\"contents\""));
  EXPECT_NE(std::string::npos, json.find("\"family\":\"Helvetica\""));
  EXPECT_NE(std::string::npos, json.find("\"size\":12"));
  EXPECT_NE(std::string::npos, json.find("\"color\":\"#FF0000\""));
  EXPECT_NE(std::string::npos, json.find("{\"text\":\"Hello\"}"));
  EXPECT_NE(std::string::npos, json.find("{\"text\":\"World\"}"));
  EXPECT_NE(std::string::npos, json.find("\"diagnostics\":[]"));
}

TEST_F(FPDFAnnotEmbedderTest, RichTextJsonFromRCAndDS) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"bold plain");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), FPDF_FONT_HELVETICA,
                                             12.0f, 0, 0, 0));
  ScopedFPDFWideString ds = GetFPDFWideString(
      L"font: 'Noto Sans',sans-serif 20.0pt; text-align:right");
  ASSERT_TRUE(FPDFAnnot_SetStringValue(annot.get(), "DS", ds.get()));
  ScopedFPDFWideString rc = GetFPDFWideString(
      L"<?xml version=\"1.0\"?><body xmlns=\"http://www.w3.org/1999/xhtml\" "
      L"style=\"color:#0000FF\"><p dir=\"ltr\"><span "
      L"style=\"font-weight:bold\">"
      L"bold</span> plain</p></body>");
  ASSERT_TRUE(FPDFAnnot_SetStringValue(annot.get(), "RC", rc.get()));

  const std::string json = GetRichTextJson(annot.get());
  EXPECT_NE(std::string::npos, json.find("\"source\":\"rc\""));
  EXPECT_NE(std::string::npos, json.find("\"family\":\"Noto Sans\""));  // DS
  EXPECT_NE(std::string::npos, json.find("\"size\":20"));               // DS
  EXPECT_NE(std::string::npos, json.find("\"color\":\"#0000FF\""));     // body
  EXPECT_NE(std::string::npos, json.find("\"align\":\"right\""));       // DS
  EXPECT_NE(std::string::npos,
            json.find("{\"text\":\"bold\",\"style\":{\"weight\":700}}"));
  EXPECT_NE(std::string::npos, json.find("{\"text\":\" plain\"}"));
}

// Acrobat-authored fixtures (experiments 05 / 07 / 04 of the plan).
TEST_F(FPDFAnnotEmbedderTest, RichTextJsonFromAcrobatDecorations) {
  ASSERT_TRUE(OpenDocument("freetext_rich_text_acrobat_decorations.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  std::string json;
  for (int i = 0; i < FPDFPage_GetAnnotCount(page.get()); ++i) {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), i));
    if (FPDFAnnot_GetSubtype(annot.get()) == FPDF_ANNOT_FREETEXT) {
      json = GetRichTextJson(annot.get());
    }
  }
  ASSERT_FALSE(json.empty());
  EXPECT_NE(std::string::npos, json.find("\"source\":\"rc\""));
  EXPECT_NE(std::string::npos, json.find("\"family\":\"Helvetica\""));
  EXPECT_NE(std::string::npos,
            json.find("\"weight\":400,\"italic\":false,\"size\":24"));
  EXPECT_NE(std::string::npos,
            json.find("{\"text\":\"hello\",\"style\":{\"weight\":700}}"));
  EXPECT_NE(
      std::string::npos,
      json.find("{\"text\":\"how\",\"style\":{\"decoration\":[\"word\"]}}"));
  EXPECT_NE(std::string::npos,
            json.find("{\"text\":\"are\",\"style\":{\"italic\":true}}"));
  EXPECT_NE(std::string::npos, json.find("{\"text\":\" you \"}"));
  EXPECT_NE(std::string::npos,
            json.find("{\"text\":\"doing?\",\"style\":{\"decoration\":[\"line-"
                      "through\"]}}"));
  EXPECT_NE(std::string::npos, json.find("\"diagnostics\":[]"));
}

TEST_F(FPDFAnnotEmbedderTest, RichTextJsonFromAcrobatProperties) {
  ASSERT_TRUE(OpenDocument("freetext_rich_text_acrobat_properties.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  bool saw_center = false;
  bool saw_scripts = false;
  bool saw_empty = false;
  for (int i = 0; i < FPDFPage_GetAnnotCount(page.get()); ++i) {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), i));
    if (FPDFAnnot_GetSubtype(annot.get()) != FPDF_ANNOT_FREETEXT) {
      continue;
    }
    const std::string json = GetRichTextJson(annot.get());
    ASSERT_FALSE(json.empty());
    if (json.find("Welcome") != std::string::npos) {
      saw_center = true;
      EXPECT_NE(std::string::npos,
                json.find("\"weight\":700,\"italic\":false,\"size\":26"));
      EXPECT_NE(std::string::npos,
                json.find("\"align\":\"center\",\"dir\":\"ltr\",\"runs\""));
      EXPECT_NE(std::string::npos, json.find("{\"text\":\"Welcome\\r\\r\"}"));
      EXPECT_NE(std::string::npos,
                json.find("\"style\":{\"weight\":400,\"size\":18}"));
    } else if (json.find("\"script\":\"sub\"") != std::string::npos) {
      saw_scripts = true;
      EXPECT_NE(
          std::string::npos,
          json.find("{\"text\":\"hello\\r\",\"style\":{\"script\":\"sub\"}}"));
      EXPECT_NE(
          std::string::npos,
          json.find("{\"text\":\"hello\",\"style\":{\"script\":\"super\"}}"));
    } else {
      // The never-typed box: no RC, no Contents, DS names the family.
      saw_empty = true;
      EXPECT_NE(std::string::npos, json.find("\"source\":\"contents\""));
      EXPECT_NE(std::string::npos, json.find("\"family\":\"Helvetica\""));
      EXPECT_NE(std::string::npos, json.find("\"size\":18"));
    }
  }
  EXPECT_TRUE(saw_center);
  EXPECT_TRUE(saw_scripts);
  EXPECT_TRUE(saw_empty);
}

TEST_F(FPDFAnnotEmbedderTest, RichTextJsonFromAcrobatBareLineBreak) {
  ASSERT_TRUE(OpenDocument("freetext_rich_text_acrobat_lines.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
  const std::string json = GetRichTextJson(annot.get());
  EXPECT_NE(std::string::npos, json.find("\"size\":22"));
  EXPECT_NE(
      std::string::npos,
      json.find(
          "{\"text\":\"How are you doing this is great! \\rnow a new line\"}"));
}

namespace {

RetainPtr<const CPDF_Dictionary> GetType0DescendantFont(
    const CPDF_Dictionary* font_dict) {
  RetainPtr<const CPDF_Array> descendants =
      font_dict ? font_dict->GetArrayFor("DescendantFonts") : nullptr;
  return descendants ? descendants->GetDictAt(0) : nullptr;
}

ScopedFPDFAnnotation AuthorFreeText(FPDF_PAGE page,
                                    EPDF_FONT_ID font_id,
                                    const wchar_t* text) {
  ScopedFPDFAnnotation annot(FPDFPage_CreateAnnot(page, FPDF_ANNOT_FREETEXT));
  if (!annot) {
    return annot;
  }
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(text);
  EXPECT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  EXPECT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(annot.get(), font_id,
                                                           24.0f, 0, 0, 0));
  EXPECT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  return annot;
}

}  // namespace

// D landing 1: an OpenType/CFF program is written as a Type 0 CIDFont with a
// FontFile3 /OpenType stream (ISO 32000-2 Table 124, 9.7.4.2), subset by
// HarfBuzz, and renders.
TEST_F(FPDFAnnotEmbedderTest, RegisteredCffFontEmitsCidFontType0) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> cff = LoadCffLatinFontData();
  ASSERT_FALSE(cff.empty());
  ASSERT_EQ(CPDF_AnnotFontSubset::ProgramFormat::kOpenTypeCFF,
            CPDF_AnnotFontSubset::DetectProgramFormat(cff));
  EPDF_FONT_ID font_id = EPDFFont_RegisterMemFont64(
      "CffLatin", /*weight=*/400, /*italic=*/0, cff.data(), cff.size());
  ASSERT_NE(0u, font_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot = AuthorFreeText(page.get(), font_id, L"ABC");
  ASSERT_TRUE(annot);

  RetainPtr<const CPDF_Dictionary> font_dict =
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(font_id));
  ASSERT_TRUE(font_dict);
  EXPECT_EQ("Type0", font_dict->GetNameFor("Subtype"));
  EXPECT_EQ("Identity-H", font_dict->GetNameFor("Encoding"));
  RetainPtr<const CPDF_Dictionary> cid_font =
      GetType0DescendantFont(font_dict.Get());
  ASSERT_TRUE(cid_font);
  EXPECT_EQ("CIDFontType0", cid_font->GetNameFor("Subtype"));
  EXPECT_FALSE(cid_font->KeyExist("CIDToGIDMap"));
  RetainPtr<const CPDF_Dictionary> descriptor =
      cid_font->GetDictFor("FontDescriptor");
  ASSERT_TRUE(descriptor);
  EXPECT_FALSE(descriptor->KeyExist("FontFile2"));
  RetainPtr<const CPDF_Stream> program = descriptor->GetStreamFor("FontFile3");
  ASSERT_TRUE(program);
  EXPECT_EQ("OpenType", program->GetDict()->GetNameFor("Subtype"));
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(program);
  acc->LoadAllDataFiltered();
  pdfium::span<const uint8_t> bytes = acc->GetSpan();
  ASSERT_GE(bytes.size(), 4u);
  EXPECT_EQ('O', bytes[0]);
  EXPECT_EQ('T', bytes[1]);
  EXPECT_EQ('T', bytes[2]);
  EXPECT_EQ('O', bytes[3]);
  EXPECT_LT(bytes.size(), cff.size());  // CFF subsetting is compiled in
  EXPECT_TRUE(font_dict->GetNameFor("BaseFont").Contains("+CffLatin"));
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), 'A'));

  ScopedFPDFBitmap bitmap =
      EmbedderTest::RenderPageWithFlags(page.get(), nullptr, FPDF_ANNOT);
  ASSERT_TRUE(bitmap);
  EXPECT_GT(CountInkPixels(bitmap.get(), 50, 80, 350, 150, 0xFFFFFFFFu), 0);
}

// D landing 1: a CID-keyed CFF program keeps its CIDs. The appearance bytes
// carry the CID (not the GID), /W is keyed by CID, ToUnicode by charcode,
// the subset keeps the GID, and the reader maps CID → GID through the charset.
TEST_F(FPDFAnnotEmbedderTest, RegisteredCidKeyedCffKeepsCidsInAppearance) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> cjk = LoadNotoSansSCFontData();
  ASSERT_FALSE(cjk.empty());
  EPDF_FONT_ID font_id = EPDFFont_RegisterMemFont64(
      "Noto Sans SC", /*weight=*/400, /*italic=*/0, cjk.data(), cjk.size());
  ASSERT_NE(0u, font_id);

  // The fixture maps U+4E00 to CID 9831 at GID 3: the two differ.
  {
    std::unique_ptr<CFX_Font> font = CFX_FontRegistry::CreateFont(font_id);
    ASSERT_TRUE(font);
    CPDF_AnnotFontSubset::GlyphIdentity identity(font.get());
    EXPECT_FALSE(identity.IsIdentity());
    EXPECT_EQ(3u, identity.GidOf(9831).value());
    EXPECT_EQ(9831, identity.CidOf(3).value());
  }

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot = AuthorFreeText(page.get(), font_id, L"\x4e00");
  ASSERT_TRUE(annot);

  const ByteString stream = GetNormalAppearanceStreamBytes(annot.get());
  ASSERT_FALSE(stream.IsEmpty());
  EXPECT_TRUE(stream.Contains(ByteStringView("\x26\x67")));      // CID 9831
  EXPECT_FALSE(stream.Contains(ByteStringView("\x00\x03", 2)));  // not GID 3

  RetainPtr<const CPDF_Dictionary> font_dict =
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(font_id));
  ASSERT_TRUE(font_dict);
  RetainPtr<const CPDF_Dictionary> cid_font =
      GetType0DescendantFont(font_dict.Get());
  ASSERT_TRUE(cid_font);
  EXPECT_EQ("CIDFontType0", cid_font->GetNameFor("Subtype"));
  RetainPtr<const CPDF_Array> widths = cid_font->GetArrayFor("W");
  ASSERT_TRUE(widths);
  bool keyed_by_cid = false;
  for (size_t i = 0; i < widths->size(); ++i) {
    if (widths->GetIntegerAt(i) == 9831) {
      keyed_by_cid = true;
    }
  }
  EXPECT_TRUE(keyed_by_cid);
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), L'\x4e00'));
  CPDF_ToUnicodeMap to_unicode(font_dict->GetStreamFor("ToUnicode"));
  EXPECT_EQ(9831u, to_unicode.ReverseLookup(L'\x4e00'));

  // The embedded subset still holds the glyph at GID 3.
  RetainPtr<const CPDF_Stream> program =
      cid_font->GetDictFor("FontDescriptor")->GetStreamFor("FontFile3");
  ASSERT_TRUE(program);
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(program);
  acc->LoadAllDataFiltered();
  EXPECT_EQ(CPDF_AnnotFontSubset::ProgramFormat::kOpenTypeCFF,
            CPDF_AnnotFontSubset::DetectProgramFormat(acc->GetSpan()));

  // And our own reader draws it (CID → GID through the charset).
  ScopedFPDFBitmap bitmap =
      EmbedderTest::RenderPageWithFlags(page.get(), nullptr, FPDF_ANNOT);
  ASSERT_TRUE(bitmap);
  EXPECT_GT(CountInkPixels(bitmap.get(), 50, 80, 350, 150, 0xFFFFFFFFu), 0);
}

// D landing 1: writing a FontFile3 /OpenType program raises an older file to
// PDF 1.6; a 1.7 file is left alone.
TEST_F(FPDFAnnotEmbedderTest, CffProgramRaisesPdfVersionTo16) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> cff = LoadCffLatinFontData();
  ASSERT_FALSE(cff.empty());
  EPDF_FONT_ID font_id = EPDFFont_RegisterMemFont64(
      "CffLatin", /*weight=*/400, /*italic=*/0, cff.data(), cff.size());
  ASSERT_NE(0u, font_id);

  // A 1.4 file: a fresh document saved with that version and reloaded.
  std::string old_pdf;
  {
    ScopedFPDFDocument doc(FPDF_CreateNewDocument());
    ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
    ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
    ASSERT_TRUE(FPDF_SaveWithVersion(doc.get(), this, 0, 14));
    old_pdf = GetString();
    ClearString();
  }
  ASSERT_EQ(0u, old_pdf.find("%PDF-1.4"));

  ScopedFPDFDocument doc(FPDF_LoadMemDocument(
      old_pdf.data(), static_cast<int>(old_pdf.size()), nullptr));
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot = AuthorFreeText(page.get(), font_id, L"ABC");
  ASSERT_TRUE(annot);

  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc.get());
  EXPECT_EQ("1.6", cpdf_doc->GetRoot()->GetNameFor("Version"));

  // A TrueType program does not touch the version.
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ScopedFPDFDocument doc2(FPDF_LoadMemDocument(
      old_pdf.data(), static_cast<int>(old_pdf.size()), nullptr));
  ScopedFPDFPage page2(FPDF_LoadPage(doc2.get(), 0));
  ScopedFPDFAnnotation annot2 = AuthorFreeText(page2.get(), roboto_id, L"ABC");
  ASSERT_TRUE(annot2);
  EXPECT_FALSE(
      CPDFDocumentFromFPDFDocument(doc2.get())->GetRoot()->KeyExist("Version"));
}

namespace {

std::string SerializeObject(const CPDF_Object* object) {
  fxcrt::ostringstream buffer;
  CPDF_StringArchiveStream archive(&buffer);
  EXPECT_TRUE(object && object->WriteTo(&archive, nullptr));
  return std::string(buffer.str().data(), buffer.str().size());
}

std::string SerializeDrDict(FPDF_DOCUMENT doc) {
  const CPDF_Dictionary* root = CPDFDocumentFromFPDFDocument(doc)->GetRoot();
  RetainPtr<const CPDF_Dictionary> acroform = root->GetDictFor("AcroForm");
  RetainPtr<const CPDF_Dictionary> dr =
      acroform ? acroform->GetDictFor("DR") : nullptr;
  return dr ? SerializeObject(dr.Get()) : std::string("<no DR>");
}

// |sfnt| wrapped as a one-font TrueType collection: a 16-byte 'ttcf' header
// in front, every table record offset moved by it. FreeType opens face 0 of
// such a file; the registry refuses the format (Phase C note §3).
std::vector<uint8_t> WrapAsCollection(pdfium::span<const uint8_t> sfnt) {
  static constexpr size_t kHeaderSize = 16;
  std::vector<uint8_t> ttc = {'t', 't', 'c', 'f', 0, 1, 0, 0,
                              0,   0,   0,   1,   0, 0, 0, kHeaderSize};
  ttc.insert(ttc.end(), sfnt.begin(), sfnt.end());
  const size_t num_tables = (sfnt[4] << 8) | sfnt[5];
  for (size_t i = 0; i < num_tables; ++i) {
    const size_t record = kHeaderSize + 12 + i * 16;
    uint32_t offset = (ttc[record + 8] << 24) | (ttc[record + 9] << 16) |
                      (ttc[record + 10] << 8) | ttc[record + 11];
    offset += kHeaderSize;
    ttc[record + 8] = offset >> 24;
    ttc[record + 9] = offset >> 16;
    ttc[record + 10] = offset >> 8;
    ttc[record + 11] = offset;
  }
  return ttc;
}

}  // namespace

// D8 (Phase C note §6): a font resource that fails to stage after another
// was staged leaves the document exactly as it was. The FreeText below needs
// two registered fonts (the /DA font and a CJK fallback); the injection makes
// the second one fail. Nothing of the first may remain: no object number,
// no /DR entry, no alias, no change to the annotation.
TEST_F(FPDFAnnotEmbedderTest,
       FreeTextStagedFontFailureLeavesDocumentUntouched) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  std::vector<uint8_t> noto = LoadNotoSansSCFontData();
  ASSERT_FALSE(roboto.empty());
  ASSERT_FALSE(noto.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  EPDF_FONT_ID noto_id = EPDFFont_RegisterMemFont64(
      "NotoSansSC", /*weight=*/400, /*italic=*/0, noto.data(), noto.size());
  ASSERT_NE(0u, roboto_id);
  ASSERT_NE(0u, noto_id);
  ASSERT_TRUE(EPDFFont_AddFallbackFont(noto_id));

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"AB\x4e00");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
      annot.get(), roboto_id, 24.0f, 0, 0, 0));

  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc.get());
  const CPDF_Dictionary* annot_dict =
      CPDFAnnotContextFromFPDFAnnotation(annot.get())->GetAnnotDict();
  const std::string annot_before = SerializeObject(annot_dict);
  const std::string dr_before = SerializeDrDict(doc.get());
  const uint32_t last_obj_before = cpdf_doc->GetLastObjNum();
  const ByteString da_alias = RegisteredFontAlias(roboto_id);
  ASSERT_EQ(roboto_id, cpdf_doc->LookupSessionFontAlias(da_alias));

  CPDF_AnnotFontMap::FailAfterStagedFontsForTesting(1);
  EXPECT_FALSE(EPDFAnnot_GenerateAppearance(annot.get()));
  CPDF_AnnotFontMap::FailAfterStagedFontsForTesting(0);

  EXPECT_EQ(annot_before, SerializeObject(annot_dict));
  EXPECT_EQ(dr_before, SerializeDrDict(doc.get()));
  EXPECT_EQ(last_obj_before, cpdf_doc->GetLastObjNum());
  EXPECT_FALSE(GetDrFontEntry(doc.get(), da_alias));
  EXPECT_EQ(roboto_id, cpdf_doc->LookupSessionFontAlias(da_alias));
  EXPECT_FALSE(cpdf_doc->LookupSessionFontAlias(RegisteredFontAlias(noto_id))
                   .has_value());

  // Without the injection the same annotation publishes both fonts.
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  EXPECT_GT(cpdf_doc->GetLastObjNum(), last_obj_before);
  RetainPtr<const CPDF_Dictionary> da_font =
      GetAppearanceFontDict(annot.get(), da_alias);
  RetainPtr<const CPDF_Dictionary> fallback_font =
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(noto_id));
  ASSERT_TRUE(da_font);
  ASSERT_TRUE(fallback_font);
  EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(da_font.Get()));
  EXPECT_TRUE(AppearanceFontMapsUnicode(da_font.Get(), 'A'));
  EXPECT_TRUE(AppearanceFontMapsUnicode(fallback_font.Get(), L'\x4e00'));
  EXPECT_EQ(da_font.Get(), GetDrFontEntry(doc.get(), da_alias).Get());
}

// Laying out with a registered font allocates no document object: the
// layout font's streams live in a scratch holder of their own.
TEST_F(FPDFAnnotEmbedderTest, RegisteredLayoutFontAllocatesNoDocumentObject) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc.get());
  const uint32_t last_obj_before = cpdf_doc->GetLastObjNum();
  {
    CPDF_AnnotFontSubset::LayoutFont layout =
        CPDF_AnnotFontSubset::CreateLayoutFont(cpdf_doc, roboto_id);
    ASSERT_TRUE(layout.font);
    ASSERT_TRUE(layout.scratch);
    EXPECT_NE(0u, layout.font->CharCodeFromUnicode('A'));
    EXPECT_GT(layout.font->GetCharWidthF(layout.font->CharCodeFromUnicode('A')),
              0);
    EXPECT_EQ(0u, layout.font->GetFontDict()->GetObjNum());
  }
  EXPECT_EQ(last_obj_before, cpdf_doc->GetLastObjNum());
}

// Phase C note §2: the document's embedding policy. DEFAULT subsets
// annotation text; FULL embeds the whole program, untagged; SUBSET subsets
// form field text too, which DEFAULT embeds whole.
TEST_F(FPDFAnnotEmbedderTest, FontEmbeddingPolicyGovernsRegisteredResources) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);
  const ByteString alias = RegisteredFontAlias(roboto_id);

  EXPECT_EQ(-1, EPDFDoc_GetFontEmbeddingPolicy(nullptr));
  EXPECT_FALSE(EPDFDoc_SetFontEmbeddingPolicy(nullptr, 0));

  CreateEmptyDocument();
  {
    EXPECT_EQ(EPDF_FONT_EMBEDDING_POLICY_DEFAULT,
              EPDFDoc_GetFontEmbeddingPolicy(document()));
    EXPECT_FALSE(EPDFDoc_SetFontEmbeddingPolicy(document(), 3));
    EXPECT_FALSE(EPDFDoc_SetFontEmbeddingPolicy(document(), -1));
    EXPECT_EQ(EPDF_FONT_EMBEDDING_POLICY_DEFAULT,
              EPDFDoc_GetFontEmbeddingPolicy(document()));

    ScopedFPDFPage page(FPDFPage_New(document(), 0, 400, 400));
    ASSERT_TRUE(page);

    // DEFAULT: FreeText subset.
    ScopedFPDFAnnotation subset_annot =
        AuthorFreeText(page.get(), roboto_id, L"ABC");
    ASSERT_TRUE(subset_annot);
    EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(
        GetAppearanceFontDict(subset_annot.get(), alias).Get()));

    // FULL: the same annotation regenerated carries the whole program.
    ASSERT_TRUE(EPDFDoc_SetFontEmbeddingPolicy(
        document(), EPDF_FONT_EMBEDDING_POLICY_FULL));
    EXPECT_EQ(EPDF_FONT_EMBEDDING_POLICY_FULL,
              EPDFDoc_GetFontEmbeddingPolicy(document()));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(subset_annot.get()));
    RetainPtr<const CPDF_Dictionary> full_font =
        GetAppearanceFontDict(subset_annot.get(), alias);
    EXPECT_TRUE(
        AppearanceFontEmbedsWholeProgram(full_font.Get(), roboto.size()));
    EXPECT_TRUE(AppearanceFontMapsUnicode(full_font.Get(), 'A'));
    EXPECT_EQ(full_font.Get(), GetDrFontEntry(document(), alias).Get());

    // An empty annotation (no glyph drawn, so no ToUnicode) under FULL also
    // carries the whole program, untagged: its /DA font must survive a save.
    // Under SUBSET it is the minimal glyph-0 subset, tagged like any other.
    auto program_length = [](const CPDF_Dictionary* font_dict) {
      RetainPtr<const CPDF_Dictionary> descriptor =
          GetType0FontDescriptor(font_dict);
      RetainPtr<const CPDF_Stream> program =
          descriptor ? descriptor->GetStreamFor("FontFile2") : nullptr;
      return program ? program->GetDict()->GetIntegerFor("Length1") : -1;
    };
    ScopedFPDFAnnotation empty_annot =
        AuthorFreeText(page.get(), roboto_id, L"");
    ASSERT_TRUE(empty_annot);
    RetainPtr<const CPDF_Dictionary> whole_font =
        GetAppearanceFontDict(empty_annot.get(), alias);
    ASSERT_TRUE(whole_font);
    EXPECT_FALSE(AppearanceFontIsSubsetTagged(whole_font.Get()));
    EXPECT_EQ(static_cast<int>(roboto.size()),
              program_length(whole_font.Get()));
    ASSERT_TRUE(EPDFDoc_SetFontEmbeddingPolicy(
        document(), EPDF_FONT_EMBEDDING_POLICY_SUBSET));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(empty_annot.get()));
    RetainPtr<const CPDF_Dictionary> minimal_font =
        GetAppearanceFontDict(empty_annot.get(), alias);
    ASSERT_TRUE(minimal_font);
    EXPECT_TRUE(AppearanceFontIsSubsetTagged(minimal_font.Get()));
    EXPECT_GT(program_length(minimal_font.Get()), 0);
    EXPECT_LT(program_length(minimal_font.Get()),
              static_cast<int>(roboto.size() / 10));

    // SUBSET: a form field, which DEFAULT embeds whole, is subset too.
    ScopedFPDFAnnotation widget(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_WIDGET));
    ASSERT_TRUE(widget);
    const FS_RECTF rect{50.0f, 200.0f, 350.0f, 150.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(widget.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
        widget.get(), roboto_id, 12.0f, 0, 0, 0));
    ScopedFPDFWideString field_name = GetFPDFWideString(L"policy_text");
    const uint32_t field = EPDFForm_CreateField(
        document(), 4 /* EPDF_FORMFIELD_FAMILY_TEXT */, field_name.get());
    ASSERT_GT(field, 0u);
    ASSERT_TRUE(EPDFForm_AttachWidget(
        document(), field, EPDFAnnot_GetObjectNumber(widget.get()), nullptr));
    ScopedFPDFWideString value = GetFPDFWideString(L"hello");
    ASSERT_TRUE(EPDFForm_SetTextValue(document(), field, value.get(), nullptr,
                                      0, nullptr));
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(widget.get()));
    EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(
        GetAppearanceFontDict(widget.get(), alias).Get()));
  }
  CloseDocument();
}

// Phase C note §3 and §9: only TrueType and OpenType/CFF sfnts are
// registered. A collection FreeType would open is refused at the door, and
// so are bytes that are not an sfnt at all.
TEST_F(FPDFAnnotEmbedderTest, RegistrationRefusesUnsupportedProgramFormats) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EXPECT_EQ(CFX_FontRegistry::ProgramFormat::kTrueType,
            CFX_FontRegistry::DetectProgramFormat(roboto));

  const std::vector<uint8_t> ttc = WrapAsCollection(roboto);
  EXPECT_EQ(CFX_FontRegistry::ProgramFormat::kUnsupported,
            CFX_FontRegistry::DetectProgramFormat(ttc));
  {
    CFX_Font font;  // the refusal is policy, not an unloadable file
    EXPECT_TRUE(font.LoadEmbedded(ttc, /*force_vertical=*/false,
                                  /*object_tag=*/0));
  }
  EXPECT_EQ(0u, EPDFFont_RegisterMemFont64("Roboto TTC", 400, 0, ttc.data(),
                                           ttc.size()));

  const std::string type1 = "%!PS-AdobeFont-1.0: NotAFont";
  EXPECT_EQ(CFX_FontRegistry::ProgramFormat::kUnsupported,
            CFX_FontRegistry::DetectProgramFormat(pdfium::as_byte_span(type1)));
  EXPECT_EQ(0u, EPDFFont_RegisterMemFont64("Type1", 400, 0, type1.data(),
                                           type1.size()));

  // The real thing still registers.
  EXPECT_NE(0u, EPDFFont_RegisterMemFont64("Roboto", 400, 0, roboto.data(),
                                           roboto.size()));
}

namespace {

// ---- Rich text (Phase D landing 2)
// ---------------------------------------------

// A text-showing segment of an appearance stream: where it starts and what
// it draws, so Acrobat's appearance and ours can be compared glyph by glyph.
struct TextSegment {
  std::string text;  // 1-byte codes as characters (standard-14 fixtures)
  float x = 0;
  float y = 0;
  float size = 0;
  std::string font;
};

std::string DecodePdfString(const std::string& token) {
  std::string out;
  if (token.size() >= 2 && token.front() == '<') {
    int nibble = -1;
    for (size_t i = 1; i + 1 < token.size(); ++i) {
      const char ch = token[i];
      int value;
      if (ch >= '0' && ch <= '9') {
        value = ch - '0';
      } else if (ch >= 'a' && ch <= 'f') {
        value = ch - 'a' + 10;
      } else if (ch >= 'A' && ch <= 'F') {
        value = ch - 'A' + 10;
      } else {
        continue;
      }
      if (nibble < 0) {
        nibble = value;
      } else {
        out += static_cast<char>((nibble << 4) | value);
        nibble = -1;
      }
    }
    return out;
  }
  for (size_t i = 1; i + 1 < token.size(); ++i) {
    char ch = token[i];
    if (ch != '\\') {
      out += ch;
      continue;
    }
    ++i;
    if (i + 1 >= token.size()) {
      break;
    }
    ch = token[i];
    switch (ch) {
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      default:
        if (ch >= '0' && ch <= '7') {
          int value = 0;
          int digits = 0;
          while (digits < 3 && i + 1 < token.size() && token[i] >= '0' &&
                 token[i] <= '7') {
            value = value * 8 + (token[i] - '0');
            ++i;
            ++digits;
          }
          --i;
          out += static_cast<char>(value);
        } else {
          out += ch;
        }
    }
  }
  return out;
}

// Tokenises just enough of a content stream: numbers, names, strings,
// arrays, and the text operators BT, Tf, Td, TD, Tm, T*, Tj, TJ, '.
std::vector<TextSegment> ParseTextSegments(const std::string& content) {
  std::vector<TextSegment> segments;
  std::vector<std::string> operands;
  float origin_x = 0;
  float origin_y = 0;
  float leading = 0;
  float rise = 0;  // Ts; Acrobat moves sub/superscripts with Td instead
  float size = 0;
  std::string font;
  size_t i = 0;
  auto read_token = [&]() -> std::string {
    while (i < content.size() &&
           isspace(static_cast<unsigned char>(content[i]))) {
      ++i;
    }
    if (i >= content.size()) {
      return std::string();
    }
    const size_t start = i;
    const char ch = content[i];
    if (ch == '(') {
      int depth = 0;
      for (; i < content.size(); ++i) {
        if (content[i] == '\\') {
          ++i;
          continue;
        }
        if (content[i] == '(') {
          ++depth;
        } else if (content[i] == ')') {
          if (--depth == 0) {
            ++i;
            break;
          }
        }
      }
      return content.substr(start, i - start);
    }
    if (ch == '<' && i + 1 < content.size() && content[i + 1] != '<') {
      while (i < content.size() && content[i] != '>') {
        ++i;
      }
      ++i;
      return content.substr(start, i - start);
    }
    if (ch == '[' || ch == ']') {
      ++i;
      return std::string(1, ch);
    }
    if (ch == '<' || ch == '>') {
      i += 2;
      return content.substr(start, 2);
    }
    while (i < content.size() &&
           !isspace(static_cast<unsigned char>(content[i])) &&
           content[i] != '(' && content[i] != '<' && content[i] != '[' &&
           content[i] != ']' && (i == start || content[i] != '/')) {
      ++i;
    }
    return content.substr(start, i - start);
  };
  auto number = [](const std::string& token) {
    return static_cast<float>(atof(token.c_str()));
  };
  auto push_segment = [&](const std::string& text) {
    TextSegment segment;
    segment.text = text;
    segment.x = origin_x;
    segment.y = origin_y + rise;
    segment.size = size;
    segment.font = font;
    segments.push_back(segment);
  };
  while (true) {
    const std::string token = read_token();
    if (token.empty()) {
      break;
    }
    const char first = token[0];
    if (first == '(' || first == '<' || first == '[' || first == ']' ||
        first == '/' || first == '-' || first == '.' || isdigit(first)) {
      operands.push_back(token);
      continue;
    }
    if (token == "BT") {
      origin_x = origin_y = 0;
      rise = 0;
    } else if (token == "Ts" && !operands.empty()) {
      rise = number(operands.back());
    } else if (token == "Tf" && operands.size() >= 2) {
      font = operands[operands.size() - 2].substr(1);
      size = number(operands.back());
    } else if ((token == "Td" || token == "TD") && operands.size() >= 2) {
      origin_x += number(operands[operands.size() - 2]);
      origin_y += number(operands.back());
      if (token == "TD") {
        leading = -number(operands.back());
      }
    } else if (token == "TL" && !operands.empty()) {
      leading = number(operands.back());
    } else if (token == "T*") {
      origin_y -= leading;
    } else if (token == "Tm" && operands.size() >= 6) {
      origin_x = number(operands[operands.size() - 2]);
      origin_y = number(operands.back());
    } else if (token == "Tj" && !operands.empty()) {
      push_segment(DecodePdfString(operands.back()));
    } else if (token == "'" && !operands.empty()) {
      origin_y -= leading;
      push_segment(DecodePdfString(operands.back()));
    } else if (token == "TJ") {
      std::string text;
      for (const std::string& operand : operands) {
        if (!operand.empty() && (operand[0] == '(' || operand[0] == '<')) {
          text += DecodePdfString(operand);
        }
      }
      push_segment(text);
    }
    operands.clear();
  }
  return segments;
}

// One drawn character with its position, expanded from the segments with
// the font's own widths (both appearances name standard-14 fonts).
struct PlacedChar {
  char ch;
  float x;
  float y;
  float size;
};

std::vector<PlacedChar> PlaceCharacters(FPDF_DOCUMENT doc,
                                        FPDF_ANNOTATION annot,
                                        const std::string& content) {
  std::vector<PlacedChar> placed;
  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc);
  for (const TextSegment& segment : ParseTextSegments(content)) {
    RetainPtr<const CPDF_Dictionary> font_dict =
        GetAppearanceFontDict(annot, ByteString(segment.font.c_str()));
    RetainPtr<CPDF_Font> font =
        font_dict
            ? CPDF_Font::GetStockFont(
                  cpdf_doc, font_dict->GetNameFor("BaseFont").AsStringView())
            : nullptr;
    float x = segment.x;
    for (char ch : segment.text) {
      if (ch != ' ') {
        placed.push_back({ch, x, segment.y, segment.size});
      }
      const int width =
          font ? font->GetCharWidthF(static_cast<uint8_t>(ch)) : 0;
      x += width * segment.size / 1000.0f;
    }
  }
  return placed;
}

// A stroked horizontal segment ("w", "m", "l S") after ET: a decoration.
struct StrokedSegment {
  float width = 0;
  float x0 = 0;
  float x1 = 0;
  float y = 0;
};

std::vector<StrokedSegment> ParseStrokedSegments(const std::string& content) {
  std::vector<StrokedSegment> segments;
  const size_t et = content.find("\nET\n");
  if (et == std::string::npos) {
    return segments;
  }
  std::istringstream in(content.substr(et + 4));
  std::vector<std::string> operands;
  StrokedSegment current;
  std::string token;
  while (in >> token) {
    if (token == "w" && !operands.empty()) {
      current.width = static_cast<float>(atof(operands.back().c_str()));
    } else if (token == "m" && operands.size() >= 2) {
      current.x0 =
          static_cast<float>(atof(operands[operands.size() - 2].c_str()));
      current.y = static_cast<float>(atof(operands.back().c_str()));
    } else if (token == "l" && operands.size() >= 2) {
      current.x1 =
          static_cast<float>(atof(operands[operands.size() - 2].c_str()));
    } else if (token == "S") {
      segments.push_back(current);
    } else if (isdigit(static_cast<unsigned char>(token[0])) ||
               token[0] == '-' || token[0] == '.') {
      operands.push_back(token);
      continue;
    }
    operands.clear();
  }
  return segments;
}

std::string RichTextJsonRuns(const std::string& json) {
  const size_t start = json.find("\"paragraphs\"");
  return start == std::string::npos ? std::string() : json.substr(start);
}

}  // namespace

// D3 (Phase C note §7): the three Acrobat fixtures regenerated by our
// engine place every glyph within 0.05 pt of where Acrobat placed it, at
// the same size, in the same order. Acrobat's own appearance is read back
// from the file before the regeneration replaces it.
class RichTextParityTest : public FPDFAnnotEmbedderTest {
 protected:
  struct Deviation {
    // Characters of one line Acrobat centred in a width 2 pt wider than
    // the text area (the line "I want to tell" of the properties fixture,
    // measured 110.05 against the 109.05 every other line's rule gives).
    // Recorded, not reproduced.
    std::string chars;
    float acrobat_extra_x;
  };

  void CompareFixture(const char* fixture,
                      int annot_index,
                      const std::vector<Deviation>& deviations) {
    ASSERT_TRUE(OpenDocument(fixture));
    ScopedPage page = LoadScopedPage(0);
    ASSERT_TRUE(page);
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), annot_index));
    ASSERT_TRUE(annot);
    const ByteString acrobat_stream =
        GetNormalAppearanceStreamBytes(annot.get());
    ASSERT_FALSE(acrobat_stream.IsEmpty());
    const std::vector<PlacedChar> acrobat = PlaceCharacters(
        document(), annot.get(), std::string(acrobat_stream.c_str()));
    ASSERT_FALSE(acrobat.empty());

    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
    const ByteString our_stream = GetNormalAppearanceStreamBytes(annot.get());
    EXPECT_NE(acrobat_stream, our_stream);
    const std::vector<PlacedChar> ours = PlaceCharacters(
        document(), annot.get(), std::string(our_stream.c_str()));

    std::string acrobat_text;
    std::string our_text;
    for (const PlacedChar& c : acrobat) {
      acrobat_text += c.ch;
    }
    for (const PlacedChar& c : ours) {
      our_text += c.ch;
    }
    ASSERT_EQ(acrobat_text, our_text);
    size_t deviation_index = 0;
    size_t deviation_pos = 0;
    for (size_t i = 0; i < acrobat.size(); ++i) {
      float tolerance = 0.06f;
      float expected_x = acrobat[i].x;
      if (deviation_index < deviations.size()) {
        const Deviation& deviation = deviations[deviation_index];
        if (deviation_pos == 0) {
          deviation_pos = acrobat_text.find(deviation.chars, i);
        }
        if (i >= deviation_pos && i < deviation_pos + deviation.chars.size()) {
          expected_x = acrobat[i].x - deviation.acrobat_extra_x;
          tolerance = 0.06f;
          if (i + 1 == deviation_pos + deviation.chars.size()) {
            ++deviation_index;
            deviation_pos = 0;
          }
        }
      }
      EXPECT_NEAR(expected_x, ours[i].x, tolerance)
          << "char '" << acrobat[i].ch << "' at index " << i;
      EXPECT_NEAR(acrobat[i].y, ours[i].y, 0.05f)
          << "char '" << acrobat[i].ch << "' at index " << i;
      EXPECT_NEAR(acrobat[i].size, ours[i].size, 0.01f)
          << "char '" << acrobat[i].ch << "' at index " << i;
    }
    EXPECT_EQ(acrobat.size(), ours.size());
  }
};

TEST_F(RichTextParityTest, LinesFixtureMatchesAcrobat) {
  // Helvetica 22, five wrapped lines and a hard break: the 0.830 ascent,
  // the 1.2 em pitch, Arial-vs-AFM widths within 0.02 pt per word.
  CompareFixture("freetext_rich_text_acrobat_lines.pdf", 0, {});
}

TEST_F(RichTextParityTest, DecorationsFixtureMatchesAcrobat) {
  // Bold, italic, word-underline and line-through runs in one line: run
  // splits and the decoration geometry.
  CompareFixture("freetext_rich_text_acrobat_decorations.pdf", 0, {});
  ScopedPage page = LoadScopedPage(0);
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
  // Acrobat: "0.652 w 214.564 588.283 m 258.591 588.283 l S" under "how",
  // "0.797 w 351.978 597.847 m 424.048 597.847 l S" through "doing?".
  // Ours: the same segments from the 0.44/0.16 descent and 0.39/0.04 ascent
  // rules, Arial-vs-AFM widths aside.
  const std::vector<StrokedSegment> strokes = ParseStrokedSegments(
      std::string(GetNormalAppearanceStreamBytes(annot.get()).c_str()));
  ASSERT_EQ(2u, strokes.size());
  EXPECT_NEAR(0.652f, strokes[0].width, 0.002f);
  EXPECT_NEAR(214.564f, strokes[0].x0, 0.05f);
  EXPECT_NEAR(258.591f, strokes[0].x1, 0.05f);
  EXPECT_NEAR(588.283f, strokes[0].y, 0.01f);
  EXPECT_NEAR(0.797f, strokes[1].width, 0.002f);
  EXPECT_NEAR(351.978f, strokes[1].x0, 0.05f);
  EXPECT_NEAR(424.048f, strokes[1].x1, 0.05f);
  EXPECT_NEAR(597.847f, strokes[1].y, 0.01f);
}

TEST_F(RichTextParityTest, PropertiesFixtureMatchesAcrobat) {
  // Centred paragraph, an empty line, a size change mid-paragraph.
  CompareFixture("freetext_rich_text_acrobat_properties.pdf", 0, {});
}

TEST_F(RichTextParityTest, SubSuperscriptFixtureMatchesAcrobat) {
  // 0.66 × size, rise −0.15 / +0.31 em, the last run's descent, the
  // superscript raising its line.
  CompareFixture("freetext_rich_text_acrobat_properties.pdf", 2, {});
}

// D2 / §6 of the Phase C note: the set-API writes RC, DS, DA, Contents and
// the appearance in one go, in Acrobat's shapes, and reads back the same
// document.
TEST_F(FPDFAnnotEmbedderTest, SetRichTextJSONWritesAllFourForms) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));

  const char kJson[] =
      "{\"body\":{\"family\":\"Helvetica\",\"weight\":400,\"italic\":false,"
      "\"size\":18,\"color\":\"#102030\",\"align\":\"left\"},"
      "\"paragraphs\":[{\"runs\":[{\"text\":\"Hello \"},"
      "{\"text\":\"bold\",\"style\":{\"weight\":700}},"
      "{\"text\":\" red\",\"style\":{\"color\":\"#FF0000\"}}]},"
      "{\"align\":\"center\",\"runs\":[{\"text\":\"H\"},"
      "{\"text\":\"2\",\"style\":{\"script\":\"sub\"}},{\"text\":\"O\"}]}]}";
  ASSERT_TRUE(EPDFAnnot_SetRichTextJSON(annot.get(), kJson));

  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot.get());
  const CPDF_Dictionary* dict = context->GetAnnotDict();
  EXPECT_EQ("0 0 0 rg /Helv 18 Tf", dict->GetByteStringFor("DA"));
  EXPECT_EQ(
      L"font: Helvetica,sans-serif 18.0pt; text-align:left; "
      L"color:#000000",
      dict->GetUnicodeTextFor("DS"));
  EXPECT_EQ(L"Hello bold red\rH2O", dict->GetUnicodeTextFor("Contents"));
  const WideString rc = dict->GetUnicodeTextFor("RC");
  EXPECT_TRUE(rc.Contains(
      L"<?xml version=\"1.0\"?><body xmlns=\"http://www.w3.org/1999/xhtml\" "
      L"xmlns:xfa=\"http://www.xfa.org/schema/xfa-data/1.0/\" "
      L"xfa:APIVersion=\"EmbedPDF:1.0\" xfa:spec=\"2.0.2\" "
      L"style=\"font-size:18.0pt;text-align:left;color:#102030;"
      L"font-weight:normal;font-style:normal;font-family:Helvetica;"
      L"font-stretch:normal\">"));
  EXPECT_TRUE(
      rc.Contains(L"<p dir=\"ltr\">Hello <span style=\"font-weight:"
                  L"bold\">bold</span><span style=\"color:#FF0000\">"
                  L" red</span></p>"));
  EXPECT_TRUE(
      rc.Contains(L"<p dir=\"ltr\" style=\"text-align:center\">H<span "
                  L"style=\"vertical-align:-0.0pt\">2</span>O</p>"));

  // Fonts: Helvetica for the body and Helvetica-Bold for the bold run,
  // under Acrobat's aliases; /DR carries the DA font.
  EXPECT_TRUE(GetAppearanceFontDict(annot.get(), "Helv"));
  EXPECT_TRUE(GetAppearanceFontDict(annot.get(), "HeBo"));
  EXPECT_EQ("Helvetica-Bold",
            GetAppearanceFontDict(annot.get(), "HeBo")->GetNameFor("BaseFont"));
  EXPECT_TRUE(GetDrFontEntry(doc.get(), "Helv"));
  const std::wstring ap = GetNormalAppearance(annot.get());
  EXPECT_THAT(ap, HasSubstr(L"/Helv 18 Tf"));
  EXPECT_THAT(ap, HasSubstr(L"/HeBo 18 Tf"));
  EXPECT_THAT(ap, HasSubstr(L"1 0 0 rg"));
  EXPECT_THAT(ap, HasSubstr(L"/Helv 11.88 Tf"));
  EXPECT_THAT(ap, HasSubstr(L"-2.7 Ts"));

  // Round trip through the getter.
  const std::string json = GetRichTextJson(annot.get());
  EXPECT_NE(std::string::npos, json.find("\"source\":\"rc\""));
  EXPECT_NE(std::string::npos, json.find("\"family\":\"Helvetica\""));
  EXPECT_NE(std::string::npos, json.find("\"color\":\"#102030\""));
  EXPECT_NE(std::string::npos,
            json.find("{\"text\":\"bold\",\"style\":{\"weight\":700}}"));
  EXPECT_NE(std::string::npos,
            json.find("{\"text\":\"2\",\"style\":{\"script\":\"sub\"}}"));
}

TEST_F(FPDFAnnotEmbedderTest, SetRichTextJSONWithoutBodyKeepsCurrentBody) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), FPDF_FONT_TIMES_BOLD,
                                             14.0f, 255, 0, 0));
  // A plain-text replacement (plan §4.7, "contents only"): one run, no body.
  ASSERT_TRUE(EPDFAnnot_SetRichTextJSON(
      annot.get(), "{\"paragraphs\":[{\"runs\":[{\"text\":\"Plain\"}]}]}"));
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot.get());
  const CPDF_Dictionary* dict = context->GetAnnotDict();
  const ByteString da = dict->GetByteStringFor("DA");
  EXPECT_TRUE(da.Contains("1 0 0 rg")) << da;
  EXPECT_TRUE(da.Contains(" 14 Tf")) << da;
  EXPECT_EQ(L"Plain", dict->GetUnicodeTextFor("Contents"));
  EXPECT_TRUE(dict->GetUnicodeTextFor("RC").Contains(
      L"font-size:14.0pt;text-align:left;color:#FF0000;font-weight:bold;"
      L"font-style:normal;font-family:Times"));
  EXPECT_TRUE(dict->GetUnicodeTextFor("DS").Contains(L"font: bold Times"));
}

TEST_F(FPDFAnnotEmbedderTest, SetRichTextJSONRejectsInvalidInputUnchanged) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), FPDF_FONT_HELVETICA,
                                             12.0f, 0, 0, 0));
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot.get());
  const std::string before = SerializeObject(context->GetAnnotDict());
  const uint32_t last_obj =
      CPDFDocumentFromFPDFDocument(doc.get())->GetLastObjNum();

  EXPECT_FALSE(EPDFAnnot_SetRichTextJSON(annot.get(), "not json"));
  EXPECT_FALSE(EPDFAnnot_SetRichTextJSON(annot.get(), "{}"));
  EXPECT_FALSE(EPDFAnnot_SetRichTextJSON(
      annot.get(), "{\"paragraphs\":[{\"runs\":[{\"text\":5}]}]}"));
  EXPECT_FALSE(EPDFAnnot_SetRichTextJSON(
      annot.get(),
      "{\"paragraphs\":[{\"runs\":[{\"text\":\"a\",\"style\":{\"color\":"
      "\"red\"}}]}]}"));
  EXPECT_FALSE(EPDFAnnot_SetRichTextJSON(annot.get(), nullptr));
  EXPECT_FALSE(EPDFAnnot_SetRichTextXHTML(annot.get(), nullptr));
  // A prolog with no element at all: nothing to lay out (an unclosed tag
  // is tolerated by the XML parser, as it is by Acrobat).
  ScopedFPDFWideString malformed =
      GetFPDFWideString(L"<?xml version=\"1.0\"?>");
  EXPECT_FALSE(EPDFAnnot_SetRichTextXHTML(annot.get(), malformed.get()));

  EXPECT_EQ(before, SerializeObject(context->GetAnnotDict()));
  EXPECT_EQ(last_obj, CPDFDocumentFromFPDFDocument(doc.get())->GetLastObjNum());

  // An empty rect is a failure too.
  const FS_RECTF empty{50.0f, 320.0f, 50.0f, 320.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &empty));
  EXPECT_FALSE(EPDFAnnot_SetRichTextJSON(
      annot.get(), "{\"paragraphs\":[{\"runs\":[{\"text\":\"a\"}]}]}"));
}

TEST_F(FPDFAnnotEmbedderTest, SetRichTextXHTMLImportsOverDefaults) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), FPDF_FONT_HELVETICA,
                                             16.0f, 0, 0, 0));
  ScopedFPDFWideString xhtml = GetFPDFWideString(
      L"<body xmlns=\"http://www.w3.org/1999/xhtml\"><p>Imported <span "
      L"style=\"font-style:italic;color:#00FF00\">text</span></p></body>");
  ASSERT_TRUE(EPDFAnnot_SetRichTextXHTML(annot.get(), xhtml.get()));
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot.get());
  const CPDF_Dictionary* dict = context->GetAnnotDict();
  EXPECT_EQ(L"Imported text", dict->GetUnicodeTextFor("Contents"));
  EXPECT_TRUE(dict->GetUnicodeTextFor("RC").Contains(
      L"<span style=\"color:#00FF00;font-style:italic\">text</span>"));
  EXPECT_TRUE(GetAppearanceFontDict(annot.get(), "HeOb"));
  const std::string json = GetRichTextJson(annot.get());
  EXPECT_NE(std::string::npos, json.find("\"size\":16"));
}

TEST_F(FPDFAnnotEmbedderTest, SetRichTextJSONWithRegisteredBodyFont) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);
  const ByteString alias = RegisteredFontAlias(roboto_id);

  std::string saved;
  {
    ScopedFPDFDocument doc(FPDF_CreateNewDocument());
    ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
    ASSERT_TRUE(annot);
    const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_SetRichTextJSON(
        annot.get(),
        "{\"body\":{\"family\":\"Roboto\",\"size\":20},"
        "\"paragraphs\":[{\"runs\":[{\"text\":\"Registered \"},"
        "{\"text\":\"Helvetica\",\"style\":{\"family\":\"Helvetica\"}}]}]}"));
    CPDF_AnnotContext* context =
        CPDFAnnotContextFromFPDFAnnotation(annot.get());
    const CPDF_Dictionary* dict = context->GetAnnotDict();
    EXPECT_EQ("0 0 0 rg /" + alias + " 20 Tf", dict->GetByteStringFor("DA"));
    // The alias was reserved at Publish and /DR names the real subset.
    EXPECT_EQ(
        roboto_id,
        CPDFDocumentFromFPDFDocument(doc.get())->LookupSessionFontAlias(alias));
    RetainPtr<const CPDF_Dictionary> dr_entry =
        GetDrFontEntry(doc.get(), alias);
    ASSERT_TRUE(dr_entry);
    EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(dr_entry.Get()));
    EXPECT_EQ(dr_entry.Get(), GetAppearanceFontDict(annot.get(), alias).Get());
    EXPECT_TRUE(AppearanceFontMapsUnicode(dr_entry.Get(), 'R'));
    EXPECT_TRUE(GetAppearanceFontDict(annot.get(), "Helv"));
    const std::wstring ap = GetNormalAppearance(annot.get());
    EXPECT_THAT(ap, HasSubstr(L"/" + std::wstring(alias.begin(), alias.end()) +
                              L" 20 Tf"));
    EXPECT_THAT(ap, HasSubstr(L"/Helv 20 Tf"));

    unsigned long saved_size = 0;
    void* saved_buffer =
        EPDF_SaveDocumentToOwnedBuffer(doc.get(), /*flags=*/0, &saved_size);
    ASSERT_TRUE(saved_buffer);
    saved.assign(static_cast<const char*>(saved_buffer), saved_size);
    EPDF_FreeBuffer(saved_buffer);
  }
  // Reopened: the body family reads back from the descriptor.
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(
      saved.data(), static_cast<int>(saved.size()), nullptr));
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
  const std::string json = GetRichTextJson(annot.get());
  EXPECT_NE(std::string::npos, json.find("\"family\":\"Roboto\""));
  EXPECT_NE(std::string::npos, json.find("\"source\":\"rc\""));
}

// D8 at the writer level (Phase C note §6): the second of two font
// resources fails after the first was staged; nothing is written, not even
// the /DA alias reservation, and the same call succeeds afterwards.
TEST_F(FPDFAnnotEmbedderTest, RichTextWriterFailureLeavesDocumentUntouched) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  std::vector<uint8_t> amiri = LoadAmiriFontData();
  ASSERT_FALSE(roboto.empty());
  ASSERT_FALSE(amiri.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  EPDF_FONT_ID amiri_id = EPDFFont_RegisterMemFont64(
      "Amiri", /*weight=*/400, /*italic=*/0, amiri.data(), amiri.size());
  ASSERT_NE(0u, roboto_id);
  ASSERT_NE(0u, amiri_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc.get());
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot.get());
  const std::string annot_before = SerializeObject(context->GetAnnotDict());
  const std::string dr_before = SerializeDrDict(doc.get());
  const uint32_t last_obj_before = cpdf_doc->GetLastObjNum();

  const char kJson[] =
      "{\"body\":{\"family\":\"Roboto\",\"size\":18},"
      "\"paragraphs\":[{\"runs\":[{\"text\":\"Two \"},"
      "{\"text\":\"fonts\",\"style\":{\"family\":\"Amiri\"}}]}]}";
  CPDF_RichTextWriter::FailAfterStagedFontsForTesting(1);
  EXPECT_FALSE(EPDFAnnot_SetRichTextJSON(annot.get(), kJson));
  CPDF_RichTextWriter::FailAfterStagedFontsForTesting(0);

  EXPECT_EQ(annot_before, SerializeObject(context->GetAnnotDict()));
  EXPECT_EQ(dr_before, SerializeDrDict(doc.get()));
  EXPECT_EQ(last_obj_before, cpdf_doc->GetLastObjNum());
  EXPECT_FALSE(cpdf_doc->LookupSessionFontAlias(RegisteredFontAlias(roboto_id))
                   .has_value());
  EXPECT_FALSE(cpdf_doc->LookupSessionFontAlias(RegisteredFontAlias(amiri_id))
                   .has_value());

  ASSERT_TRUE(EPDFAnnot_SetRichTextJSON(annot.get(), kJson));
  EXPECT_EQ(roboto_id,
            cpdf_doc->LookupSessionFontAlias(RegisteredFontAlias(roboto_id)));
  EXPECT_TRUE(
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(roboto_id)));
  EXPECT_TRUE(
      GetAppearanceFontDict(annot.get(), RegisteredFontAlias(amiri_id)));
  EXPECT_TRUE(GetDrFontEntry(doc.get(), RegisteredFontAlias(roboto_id)));
}

// The document switches: plain FreeText stays on CPVT unless the rich
// engine is selected; an /RC always takes the rich engine.
TEST_F(FPDFAnnotEmbedderTest, FreeTextLayoutSwitchSelectsRichEngine) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  EXPECT_EQ(EPDF_FREETEXT_LAYOUT_CPVT, EPDFDoc_GetFreeTextLayout(doc.get()));
  EXPECT_EQ(-1, EPDFDoc_GetFreeTextLayout(nullptr));
  EXPECT_FALSE(EPDFDoc_SetFreeTextLayout(doc.get(), 7));
  EXPECT_FALSE(EPDFDoc_GetTypographicFeatures(doc.get()));
  EXPECT_TRUE(EPDFDoc_SetTypographicFeatures(doc.get(), true));
  EXPECT_TRUE(EPDFDoc_GetTypographicFeatures(doc.get()));
  EXPECT_TRUE(EPDFDoc_SetTypographicFeatures(doc.get(), false));

  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{100.0f, 300.0f, 300.0f, 200.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"Plain text");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), FPDF_FONT_HELVETICA,
                                             20.0f, 0, 0, 0));

  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  const std::string cpvt =
      std::string(GetNormalAppearanceStreamBytes(annot.get()).c_str());
  ASSERT_TRUE(EPDFDoc_SetFreeTextLayout(doc.get(), EPDF_FREETEXT_LAYOUT_RICH));
  EXPECT_EQ(EPDF_FREETEXT_LAYOUT_RICH, EPDFDoc_GetFreeTextLayout(doc.get()));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  const std::string rich =
      std::string(GetNormalAppearanceStreamBytes(annot.get()).c_str());
  EXPECT_NE(cpvt, rich);
  // Acrobat's inset: the text starts border + 1 in from the left edge and
  // the baseline sits 0.830 em below the top inset.
  const std::vector<TextSegment> segments = ParseTextSegments(rich);
  ASSERT_FALSE(segments.empty());
  EXPECT_NEAR(102.0f, segments[0].x, 0.01f);
  EXPECT_NEAR(300.0f - 2.0f - 0.830f * 20.0f, segments[0].y, 0.01f);
  EXPECT_EQ("Plain ", segments[0].text);
}

// D5, the right-to-left case: an Arabic run is shaped, written in visual
// order under an ActualText span, and extracts as its logical text after
// flattening.
TEST_F(FPDFAnnotEmbedderTest, RichTextRtlRunCarriesActualTextAndExtracts) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> amiri = LoadAmiriFontData();
  ASSERT_FALSE(amiri.empty());
  ASSERT_NE(
      0u, EPDFFont_RegisterMemFont64("Amiri", /*weight=*/400,
                                     /*italic=*/0, amiri.data(), amiri.size()));
  std::string saved;
  {
    ScopedFPDFDocument doc(FPDF_CreateNewDocument());
    ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
    ASSERT_TRUE(annot);
    const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_SetRichTextJSON(
        annot.get(),
        "{\"body\":{\"family\":\"Amiri\",\"size\":24},"
        "\"paragraphs\":[{\"dir\":\"rtl\",\"runs\":[{\"text\":"
        "\"\\u0634\\u0643\\u0631\\u0627\"}]}]}"));
    const std::wstring ap = GetNormalAppearance(annot.get());
    EXPECT_THAT(ap,
                HasSubstr(L"/Span <</ActualText <FEFF0634064306310627>>> BDC"));
    EXPECT_THAT(ap, HasSubstr(L"EMC"));
    ASSERT_TRUE(FPDFPage_Flatten(page.get(), FLAT_NORMALDISPLAY));
    unsigned long saved_size = 0;
    void* saved_buffer =
        EPDF_SaveDocumentToOwnedBuffer(doc.get(), /*flags=*/0, &saved_size);
    ASSERT_TRUE(saved_buffer);
    saved.assign(static_cast<const char*>(saved_buffer), saved_size);
    EPDF_FreeBuffer(saved_buffer);
  }
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(
      saved.data(), static_cast<int>(saved.size()), nullptr));
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFTextPage text_page(FPDFText_LoadPage(page.get()));
  ASSERT_TRUE(text_page);
  const int count = FPDFText_CountChars(text_page.get());
  std::wstring extracted;
  for (int i = 0; i < count; ++i) {
    extracted += static_cast<wchar_t>(FPDFText_GetUnicode(text_page.get(), i));
  }
  std::string codepoints;
  for (wchar_t ch : extracted) {
    codepoints += ByteString::Format("U+%04X ", static_cast<int>(ch)).c_str();
  }
  EXPECT_NE(std::wstring::npos, extracted.find(L"\x0634\x0643\x0631\x0627"))
      << "extracted: " << codepoints;
}

// Rung 2 of plan §3.3 (C note §1.3): a family that is no longer registered
// but whose program is already in the document resolves to that program,
// through a new Type0 dictionary that references the existing stream.
TEST_F(FPDFAnnotEmbedderTest, RichTextResolvesDocumentProgramWhenUnregistered) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(EPDFDoc_SetFontEmbeddingPolicy(doc.get(),
                                             EPDF_FONT_EMBEDDING_POLICY_FULL));
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ScopedFPDFAnnotation first(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(first);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 250.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(first.get(), &rect));
  ASSERT_TRUE(EPDFAnnot_SetRichTextJSON(
      first.get(),
      "{\"body\":{\"family\":\"Roboto\",\"size\":16},"
      "\"paragraphs\":[{\"runs\":[{\"text\":\"ABC\"}]}]}"));
  RetainPtr<const CPDF_Dictionary> first_font =
      GetAppearanceFontDict(first.get(), RegisteredFontAlias(roboto_id));
  ASSERT_TRUE(first_font);
  RetainPtr<const CPDF_Stream> program =
      GetType0FontDescriptor(first_font.Get())->GetStreamFor("FontFile2");
  ASSERT_TRUE(program);
  EXPECT_EQ(static_cast<int>(roboto.size()),
            program->GetDict()->GetIntegerFor("Length1"));

  // A later session without the registration: the family still resolves,
  // to the program the file carries, with no second copy of it.
  EPDFFont_ClearRegisteredFonts();
  const uint32_t last_obj_before =
      CPDFDocumentFromFPDFDocument(doc.get())->GetLastObjNum();
  ScopedFPDFAnnotation second(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(second);
  const FS_RECTF rect2{50.0f, 200.0f, 350.0f, 130.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(second.get(), &rect2));
  ASSERT_TRUE(EPDFAnnot_SetRichTextJSON(
      second.get(),
      "{\"body\":{\"family\":\"Roboto\",\"size\":16},"
      "\"paragraphs\":[{\"runs\":[{\"text\":\"XYZ\"}]}]}"));
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(second.get());
  const ByteString alias = ByteString::Format("EDocF%u", program->GetObjNum());
  EXPECT_EQ("0 0 0 rg /" + alias + " 16 Tf",
            context->GetAnnotDict()->GetByteStringFor("DA"));
  RetainPtr<const CPDF_Dictionary> second_font =
      GetAppearanceFontDict(second.get(), alias);
  ASSERT_TRUE(second_font);
  EXPECT_NE(first_font.Get(), second_font.Get());
  EXPECT_EQ("Type0", second_font->GetNameFor("Subtype"));
  EXPECT_EQ(program.Get(), GetType0FontDescriptor(second_font.Get())
                               ->GetStreamFor("FontFile2")
                               .Get());
  EXPECT_TRUE(AppearanceFontMapsUnicode(second_font.Get(), 'X'));
  EXPECT_EQ(second_font.Get(), GetDrFontEntry(doc.get(), alias).Get());
  EXPECT_EQ(L"Roboto", GetType0FontDescriptor(second_font.Get())
                           ->GetUnicodeTextFor("FontFamily"));
  // New dictionaries only: no font program was added.
  int streams_added = 0;
  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc.get());
  for (uint32_t n = last_obj_before + 1; n <= cpdf_doc->GetLastObjNum(); ++n) {
    RetainPtr<const CPDF_Object> object = cpdf_doc->GetIndirectObject(n);
    if (object && object->IsStream() &&
        object->AsStream()->GetDict()->KeyExist("Length1")) {
      ++streams_added;
    }
  }
  EXPECT_EQ(0, streams_added);
  const std::string json = GetRichTextJson(second.get());
  EXPECT_NE(std::string::npos, json.find("\"family\":\"Roboto\""));
}

// The identity a host maps back to its own keys: family (given, else the
// font's own), weight and italic, as the document will name the face.
TEST_F(FPDFAnnotEmbedderTest, RegisteredFontIdentityGetters) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRobotoFontData();
  ASSERT_FALSE(roboto.empty());
  EPDF_FONT_ID named = EPDFFont_RegisterMemFont64(
      "My Roboto", /*weight=*/700, /*italic=*/1, roboto.data(), roboto.size());
  EPDF_FONT_ID inferred = EPDFFont_RegisterMemFont64(
      "", /*weight=*/0, /*italic=*/-1, roboto.data(), roboto.size());
  ASSERT_NE(0u, named);
  ASSERT_NE(0u, inferred);

  char buffer[64] = {};
  const unsigned long length = EPDFFont_GetFamilyName(named, nullptr, 0);
  ASSERT_EQ(strlen("My Roboto") + 1, length);
  ASSERT_EQ(length, EPDFFont_GetFamilyName(named, buffer, sizeof(buffer)));
  EXPECT_STREQ("My Roboto", buffer);
  EXPECT_EQ(700, EPDFFont_GetWeight(named));
  EXPECT_TRUE(EPDFFont_IsItalic(named));

  ASSERT_LT(0u, EPDFFont_GetFamilyName(inferred, buffer, sizeof(buffer)));
  EXPECT_STREQ("Roboto", buffer);
  EXPECT_EQ(400, EPDFFont_GetWeight(inferred));
  EXPECT_FALSE(EPDFFont_IsItalic(inferred));

  EXPECT_EQ(0u, EPDFFont_GetFamilyName(999, buffer, sizeof(buffer)));
  EXPECT_EQ(0, EPDFFont_GetWeight(999));
  EXPECT_FALSE(EPDFFont_IsItalic(999));
}

TEST_F(FPDFAnnotEmbedderTest, FreeTextRegisteredFontMarkerSurvivesAliasSuffix) {
  ScopedRegisteredFonts scoped_fonts;

  std::vector<uint8_t> font_data = LoadRobotoFontData();
  ASSERT_FALSE(font_data.empty());

  EPDF_FONT_ID font_id =
      EPDFFont_RegisterMemFont64("Roboto", /*weight=*/400, /*italic=*/0,
                                 font_data.data(), font_data.size());
  ASSERT_NE(0u, font_id);

  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);

  CPDF_Document* cpdf_doc = CPDFDocumentFromFPDFDocument(doc.get());
  ASSERT_TRUE(cpdf_doc);
  RetainPtr<CPDF_Dictionary> root_dict = cpdf_doc->GetMutableRoot();
  ASSERT_TRUE(root_dict);
  RetainPtr<CPDF_Dictionary> font_resources =
      root_dict->GetOrCreateDictFor("AcroForm")
          ->GetOrCreateDictFor("DR")
          ->GetOrCreateDictFor("Font");

  auto colliding_font_dict = cpdf_doc->NewIndirect<CPDF_Dictionary>();
  colliding_font_dict->SetNewFor<CPDF_Name>("Type", "Font");
  colliding_font_dict->SetNewFor<CPDF_Name>("Subtype", "Type1");
  colliding_font_dict->SetNewFor<CPDF_Name>("BaseFont", "Helvetica");
  const ByteString base_alias = RegisteredFontAlias(font_id);
  font_resources->SetNewFor<CPDF_Reference>(base_alias, cpdf_doc,
                                            colliding_font_dict->GetObjNum());

  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);

  const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"ABC");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(annot.get(), font_id,
                                                           18.0f, 0, 0, 0));

  ByteString actual_alias = GetDefaultAppearanceFontAlias(annot.get());
  ASSERT_FALSE(actual_alias.IsEmpty());
  EXPECT_NE(base_alias, actual_alias);
  EXPECT_EQ(base_alias, actual_alias.First(base_alias.GetLength()));

  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  RetainPtr<const CPDF_Dictionary> font_dict =
      GetAppearanceFontDict(annot.get(), actual_alias);
  ASSERT_TRUE(font_dict);
  EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(font_dict.Get()));
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), 'A'));
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), 'B'));
  EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), 'C'));
}

TEST_F(FPDFAnnotEmbedderTest, TextFieldKoreanUsesRegisteredDroidFallbackFont) {
  ScopedRegisteredFonts scoped_fonts;
  EPDF_FONT_ID font_id = RegisterDroidSansFallbackFullFont();
  ASSERT_NE(0u, font_id);

  CreateEmptyDocument();
  {
    ScopedFPDFPage page(FPDFPage_New(document(), 0, 400, 400));
    ASSERT_TRUE(page);

    // Widgets are born through the annotation API and adopted by a field
    // (EPDFForm_AttachWidget); values flow through the typed EPDFForm_*
    // transactions, which regenerate the appearance stream.
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_WIDGET));
    ASSERT_TRUE(annot);
    const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), FPDF_FONT_HELVETICA,
                                               18.0f, 0, 0, 0));

    ScopedFPDFWideString field_name = GetFPDFWideString(L"korean_text");
    const uint32_t field = EPDFForm_CreateField(
        document(), 4 /* EPDF_FORMFIELD_FAMILY_TEXT */, field_name.get());
    ASSERT_GT(field, 0u);
    ASSERT_TRUE(EPDFForm_AttachWidget(
        document(), field, EPDFAnnot_GetObjectNumber(annot.get()), nullptr));

    ScopedFPDFWideString value = GetFPDFWideString(L"\xD55C\xAE00");
    ASSERT_TRUE(EPDFForm_SetTextValue(document(), field, value.get(), nullptr,
                                      0, nullptr));

    // The annotation-plane companion regenerates the same appearance.
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(annot.get()));
    ExpectRegisteredAppearanceMapsUnicode(annot.get(), font_id,
                                          {L'\xD55C', L'\xAE00'},
                                          /*expect_subset=*/false);
  }
  CloseDocument();
}

TEST_F(FPDFAnnotEmbedderTest, ComboBoxKoreanUsesRegisteredDroidFallbackFont) {
  ScopedRegisteredFonts scoped_fonts;
  EPDF_FONT_ID font_id = RegisterDroidSansFallbackFullFont();
  ASSERT_NE(0u, font_id);

  CreateEmptyDocument();
  {
    ScopedFPDFPage page(FPDFPage_New(document(), 0, 400, 400));
    ASSERT_TRUE(page);

    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_WIDGET));
    ASSERT_TRUE(annot);
    const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), FPDF_FONT_HELVETICA,
                                               18.0f, 0, 0, 0));

    ScopedFPDFWideString field_name = GetFPDFWideString(L"korean_combo");
    const uint32_t field = EPDFForm_CreateField(
        document(), 5 /* EPDF_FORMFIELD_FAMILY_COMBOBOX */, field_name.get());
    ASSERT_GT(field, 0u);
    ASSERT_TRUE(EPDFForm_AttachWidget(
        document(), field, EPDFAnnot_GetObjectNumber(annot.get()), nullptr));

    ScopedFPDFWideString latin_option = GetFPDFWideString(L"Latin");
    ScopedFPDFWideString korean_option = GetFPDFWideString(L"\xD55C\xAE00");
    FPDF_WIDESTRING labels[] = {latin_option.get(), korean_option.get()};
    ASSERT_TRUE(EPDFForm_SetFieldOptions(document(), field, labels, labels, 2));
    FPDF_WIDESTRING selection[] = {korean_option.get()};
    ASSERT_TRUE(EPDFForm_SetChoiceValues(document(), field, selection, 1,
                                         nullptr, 0, nullptr));

    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(annot.get()));
    ExpectRegisteredAppearanceMapsUnicode(annot.get(), font_id,
                                          {L'\xD55C', L'\xAE00'},
                                          /*expect_subset=*/false);
  }
  CloseDocument();
}

TEST_F(FPDFAnnotEmbedderTest, ListBoxKoreanUsesRegisteredDroidFallbackFont) {
  ScopedRegisteredFonts scoped_fonts;
  EPDF_FONT_ID font_id = RegisterDroidSansFallbackFullFont();
  ASSERT_NE(0u, font_id);

  CreateEmptyDocument();
  {
    ScopedFPDFPage page(FPDFPage_New(document(), 0, 400, 400));
    ASSERT_TRUE(page);

    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_WIDGET));
    ASSERT_TRUE(annot);
    const FS_RECTF rect{50.0f, 220.0f, 350.0f, 330.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), FPDF_FONT_HELVETICA,
                                               18.0f, 0, 0, 0));

    ScopedFPDFWideString field_name = GetFPDFWideString(L"korean_list");
    const uint32_t field = EPDFForm_CreateField(
        document(), 6 /* EPDF_FORMFIELD_FAMILY_LISTBOX */, field_name.get());
    ASSERT_GT(field, 0u);
    ASSERT_TRUE(EPDFForm_AttachWidget(
        document(), field, EPDFAnnot_GetObjectNumber(annot.get()), nullptr));

    ScopedFPDFWideString latin_option = GetFPDFWideString(L"Latin");
    ScopedFPDFWideString korean_option = GetFPDFWideString(L"\xD55C\xAE00");
    FPDF_WIDESTRING labels[] = {latin_option.get(), korean_option.get()};
    ASSERT_TRUE(EPDFForm_SetFieldOptions(document(), field, labels, labels, 2));
    FPDF_WIDESTRING selection[] = {korean_option.get()};
    ASSERT_TRUE(EPDFForm_SetChoiceValues(document(), field, selection, 1,
                                         nullptr, 0, nullptr));

    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(annot.get()));
    ExpectRegisteredAppearanceMapsUnicode(annot.get(), font_id,
                                          {L'\xD55C', L'\xAE00'},
                                          /*expect_subset=*/false);
  }
  CloseDocument();
}

TEST_F(FPDFAnnotEmbedderTest, FreeTextRegisteredFontSubsetsAreLayerLocal) {
  ScopedRegisteredFonts scoped_fonts;

  std::vector<uint8_t> font_data = LoadRobotoFontData();
  ASSERT_FALSE(font_data.empty());

  EPDF_FONT_ID font_id =
      EPDFFont_RegisterMemFont64("Roboto", /*weight=*/400, /*italic=*/0,
                                 font_data.data(), font_data.size());
  ASSERT_NE(0u, font_id);

  FileAccessForTesting base_access("rectangles.pdf");
  EPDF_BASE_DOCUMENT base = EPDF_LoadBaseDocument(&base_access, nullptr);
  ASSERT_TRUE(base);

  struct LayerSubsetResult {
    std::string delta;
    ByteString subset_base_font;
  };

  auto save_layer_with_text = [&](const wchar_t* text,
                                  const std::vector<char>& expected_chars,
                                  const std::vector<char>& unexpected_chars) {
    EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
    ScopedFPDFDocument layer(
        EPDFLayer_OpenLayer(base, nullptr, nullptr, &open_status));
    EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);
    EXPECT_TRUE(layer);

    ScopedFPDFPage page(FPDF_LoadPage(layer.get(), 0));
    EXPECT_TRUE(page);
    ScopedFPDFAnnotation annot(
        EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
    EXPECT_TRUE(annot);

    const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
    EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ScopedFPDFWideString contents = GetFPDFWideString(text);
    EXPECT_TRUE(
        FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
    EXPECT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
        annot.get(), font_id, 18.0f, 0, 0, 0));
    EXPECT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

    RetainPtr<const CPDF_Dictionary> font_dict =
        GetAppearanceFontDict(annot.get(), RegisteredFontAlias(font_id));
    EXPECT_TRUE(font_dict);
    EXPECT_TRUE(AppearanceFontHasEmbeddedSubset(font_dict.Get()));
    for (char value : expected_chars) {
      EXPECT_TRUE(AppearanceFontMapsUnicode(font_dict.Get(), value));
    }
    for (char value : unexpected_chars) {
      EXPECT_FALSE(AppearanceFontMapsUnicode(font_dict.Get(), value));
    }

    ByteString subset_base_font =
        font_dict ? font_dict->GetNameFor("BaseFont") : ByteString();

    unsigned long delta_size = 0;
    EPDFLayerSaveStatus save_status = EPDFLayerSaveStatus_kSaveFailed;
    void* delta_buffer = EPDFLayer_SaveDeltaToOwnedBuffer(
        layer.get(), &delta_size, &save_status);
    EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, save_status);
    EXPECT_TRUE(delta_buffer);
    std::string delta(static_cast<const char*>(delta_buffer), delta_size);
    EPDF_FreeBuffer(delta_buffer);
    return LayerSubsetResult{std::move(delta), std::move(subset_base_font)};
  };

  LayerSubsetResult layer_a =
      save_layer_with_text(L"ABC", {'A', 'B', 'C'}, {'D', 'E', 'F'});
  LayerSubsetResult layer_b =
      save_layer_with_text(L"DEF", {'D', 'E', 'F'}, {'A', 'B', 'C'});
  EPDF_ReleaseBaseDocument(base);

  EXPECT_FALSE(layer_a.delta.empty());
  EXPECT_FALSE(layer_b.delta.empty());
  EXPECT_LT(layer_a.delta.size(), font_data.size() / 2);
  EXPECT_LT(layer_b.delta.size(), font_data.size() / 2);
  EXPECT_NE(layer_a.subset_base_font, layer_b.subset_base_font);
  EXPECT_NE(std::string::npos,
            layer_a.delta.find(layer_a.subset_base_font.c_str()));
  EXPECT_NE(std::string::npos,
            layer_b.delta.find(layer_b.subset_base_font.c_str()));
  EXPECT_EQ(std::string::npos,
            layer_a.delta.find(layer_b.subset_base_font.c_str()));
  EXPECT_EQ(std::string::npos,
            layer_b.delta.find(layer_a.subset_base_font.c_str()));
}

TEST_F(FPDFAnnotEmbedderTest, RegisteredFallbackFontRendersPageMissingGlyph) {
  auto create_broken_pdf = []() {
    ScopedFPDFDocument doc(FPDF_CreateNewDocument());
    EXPECT_TRUE(doc);
    {
      ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 200, 200));
      EXPECT_TRUE(page);
      AddBrokenTrueTypeCjkTextPageContent(doc.get(), page.get());
    }

    unsigned long saved_size = 0;
    void* saved_buffer =
        EPDF_SaveDocumentToOwnedBuffer(doc.get(), /*flags=*/0, &saved_size);
    EXPECT_TRUE(saved_buffer);
    if (!saved_buffer) {
      return std::vector<uint8_t>();
    }
    std::vector<uint8_t> pdf_bytes(
        static_cast<const uint8_t*>(saved_buffer),
        static_cast<const uint8_t*>(saved_buffer) + saved_size);
    EPDF_FreeBuffer(saved_buffer);
    return pdf_bytes;
  };

  auto render_broken_pdf = [](const std::vector<uint8_t>& pdf_bytes) {
    MemoryFileAccess file_access(pdf_bytes);
    ScopedFPDFDocument doc(FPDF_LoadCustomDocument(&file_access, nullptr));
    EXPECT_TRUE(doc);
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
    EXPECT_TRUE(page);
    ScopedFPDFBitmap bitmap = EmbedderTest::RenderPage(page.get());
    EXPECT_TRUE(bitmap);
    return bitmap;
  };

  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> broken_pdf = create_broken_pdf();
  ASSERT_FALSE(broken_pdf.empty());
  ScopedFPDFBitmap bitmap_without_fallback = render_broken_pdf(broken_pdf);
  ASSERT_TRUE(bitmap_without_fallback);
  std::string without_registered_fallback =
      BitmapChecksum(bitmap_without_fallback.get());
  ASSERT_FALSE(without_registered_fallback.empty());

  std::vector<uint8_t> font_data = LoadNotoSansSCFontData();
  ASSERT_FALSE(font_data.empty());

  EPDF_FONT_ID font_id =
      EPDFFont_RegisterMemFont64("NotoSansSC", /*weight=*/400, /*italic=*/0,
                                 font_data.data(), font_data.size());
  ASSERT_NE(0u, font_id);
  ASSERT_TRUE(EPDFFont_AddFallbackFont(font_id));

  ScopedFPDFBitmap bitmap = render_broken_pdf(broken_pdf);
  ASSERT_TRUE(bitmap);

  EXPECT_TRUE(BitmapHasNonWhitePixels(bitmap.get()));
  EXPECT_NE(without_registered_fallback, BitmapChecksum(bitmap.get()));
}

TEST_F(FPDFAnnotEmbedderTest, ExtractHighlightLongContent) {
  // Open a file with one annotation and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_highlight_long_content.pdf"));
  FPDF_PAGE page = LoadPageNoEvents(0);
  ASSERT_TRUE(page);

  // Check that there is a total of 1 annotation on its first page.
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(page));

  // Check that the annotation is of type "highlight".
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_HIGHLIGHT, FPDFAnnot_GetSubtype(annot.get()));

    // Check that the annotation color is yellow.
    unsigned int R;
    unsigned int G;
    unsigned int B;
    unsigned int A;
    ASSERT_TRUE(FPDFAnnot_GetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, &R,
                                   &G, &B, &A));
    EXPECT_EQ(255u, R);
    EXPECT_EQ(255u, G);
    EXPECT_EQ(0u, B);
    EXPECT_EQ(255u, A);

    // Check that the author is correct.
    static const char kAuthorKey[] = "T";
    EXPECT_EQ(FPDF_OBJECT_STRING,
              FPDFAnnot_GetValueType(annot.get(), kAuthorKey));
    unsigned long length_bytes =
        FPDFAnnot_GetStringValue(annot.get(), kAuthorKey, nullptr, 0);
    ASSERT_EQ(28u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(28u, FPDFAnnot_GetStringValue(annot.get(), kAuthorKey, buf.data(),
                                            length_bytes));
    EXPECT_EQ(L"Jae Hyun Park", GetPlatformWString(buf.data()));

    // Check that the content is correct.
    EXPECT_EQ(
        FPDF_OBJECT_STRING,
        FPDFAnnot_GetValueType(annot.get(), pdfium::annotation::kContents));
    length_bytes = FPDFAnnot_GetStringValue(
        annot.get(), pdfium::annotation::kContents, nullptr, 0);
    ASSERT_EQ(2690u, length_bytes);
    buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(2690u, FPDFAnnot_GetStringValue(annot.get(),
                                              pdfium::annotation::kContents,
                                              buf.data(), length_bytes));
    static const wchar_t kContents[] =
        L"This is a note for that highlight annotation. Very long highlight "
        "annotation. Long long long Long long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long longLong long longLong long longLong long longLong long "
        "longLong long long. END";
    EXPECT_EQ(kContents, GetPlatformWString(buf.data()));

    // Check that the quadpoints are correct.
    FS_QUADPOINTSF quadpoints;
    ASSERT_TRUE(FPDFAnnot_GetAttachmentPoints(annot.get(), 0, &quadpoints));
    EXPECT_EQ(115.802643f, quadpoints.x1);
    EXPECT_EQ(718.913940f, quadpoints.y1);
    EXPECT_EQ(157.211166f, quadpoints.x4);
    EXPECT_EQ(706.264404f, quadpoints.y4);
  }
  UnloadPageNoEvents(page);
}

TEST_F(FPDFAnnotEmbedderTest, ExtractInkMultiple) {
  // Open a file with three annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_ink_multiple.pdf"));
  FPDF_PAGE page = LoadPageNoEvents(0);
  ASSERT_TRUE(page);

  // Check that there is a total of 3 annotation on its first page.
  EXPECT_EQ(3, FPDFPage_GetAnnotCount(page));

  {
    // Check that the third annotation is of type "ink".
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, 2));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_INK, FPDFAnnot_GetSubtype(annot.get()));

    // Check that the annotation color is blue with opacity.
    unsigned int R;
    unsigned int G;
    unsigned int B;
    unsigned int A;
    ASSERT_TRUE(FPDFAnnot_GetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, &R,
                                   &G, &B, &A));
    EXPECT_EQ(0u, R);
    EXPECT_EQ(0u, G);
    EXPECT_EQ(255u, B);
    EXPECT_EQ(76u, A);

    // Check that there is no content.
    EXPECT_EQ(2u, FPDFAnnot_GetStringValue(
                      annot.get(), pdfium::annotation::kContents, nullptr, 0));

    // Check that the rectangle coordinates are correct.
    // Note that upon rendering, the rectangle coordinates will be adjusted.
    FS_RECTF rect;
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
    EXPECT_EQ(351.820435f, rect.left);
    EXPECT_EQ(583.830750f, rect.bottom);
    EXPECT_EQ(475.336121f, rect.right);
    EXPECT_EQ(681.535034f, rect.top);
  }
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page, FPDF_ANNOT);
    CompareBitmapWithFuzzyExpectationSuffix(bitmap.get(),
                                            "annotation_ink_multiple");
  }
  UnloadPageNoEvents(page);
}

TEST_F(FPDFAnnotEmbedderTest, AddIllegalSubtypeAnnotation) {
  // Open a file with one annotation and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_highlight_long_content.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // Add an annotation with an illegal subtype.
  ASSERT_FALSE(FPDFPage_CreateAnnot(page.get(), -1));
}

TEST_F(FPDFAnnotEmbedderTest, AddFirstTextAnnotation) {
  // Open a file with no annotation and load its first page.
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));

  {
    // Add a text annotation to the page.
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(annot);

    // Check that there is now 1 annotations on this page.
    EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));

    // Check that the subtype of the annotation is correct.
    EXPECT_EQ(FPDF_ANNOT_TEXT, FPDFAnnot_GetSubtype(annot.get()));
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_TEXT, FPDFAnnot_GetSubtype(annot.get()));

    // Set the color of the annotation.
    ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, 51,
                                   102, 153, 204));
    // Check that the color has been set correctly.
    unsigned int R;
    unsigned int G;
    unsigned int B;
    unsigned int A;
    ASSERT_TRUE(FPDFAnnot_GetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, &R,
                                   &G, &B, &A));
    EXPECT_EQ(51u, R);
    EXPECT_EQ(102u, G);
    EXPECT_EQ(153u, B);
    EXPECT_EQ(204u, A);

    // Change the color of the annotation.
    ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, 204,
                                   153, 102, 51));
    // Check that the color has been set correctly.
    ASSERT_TRUE(FPDFAnnot_GetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, &R,
                                   &G, &B, &A));
    EXPECT_EQ(204u, R);
    EXPECT_EQ(153u, G);
    EXPECT_EQ(102u, B);
    EXPECT_EQ(51u, A);

    // Set the annotation rectangle.
    FS_RECTF rect;
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
    EXPECT_EQ(0.f, rect.left);
    EXPECT_EQ(0.f, rect.right);
    rect.left = 35;
    rect.bottom = 150;
    rect.right = 53;
    rect.top = 165;
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    // Check that the annotation rectangle has been set correctly.
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
    EXPECT_EQ(35.f, rect.left);
    EXPECT_EQ(150.f, rect.bottom);
    EXPECT_EQ(53.f, rect.right);
    EXPECT_EQ(165.f, rect.top);

    // Set the content of the annotation.
    static const wchar_t kContents[] = L"Hello! This is a customized content.";
    ScopedFPDFWideString text = GetFPDFWideString(kContents);
    ASSERT_TRUE(FPDFAnnot_SetStringValue(
        annot.get(), pdfium::annotation::kContents, text.get()));
    // Check that the content has been set correctly.
    unsigned long length_bytes = FPDFAnnot_GetStringValue(
        annot.get(), pdfium::annotation::kContents, nullptr, 0);
    ASSERT_EQ(74u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(74u, FPDFAnnot_GetStringValue(annot.get(),
                                            pdfium::annotation::kContents,
                                            buf.data(), length_bytes));
    EXPECT_EQ(kContents, GetPlatformWString(buf.data()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, AddAndSaveLinkAnnotation) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(bitmap.get(), pdfium::kHelloWorldPng);
  }
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));

  static constexpr char kUri[] = "https://pdfium.org/";

  {
    // Add a link annotation to the page and set its URI.
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINK));
    ASSERT_TRUE(annot);
    EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
    EXPECT_EQ(FPDF_ANNOT_LINK, FPDFAnnot_GetSubtype(annot.get()));
    EXPECT_TRUE(FPDFAnnot_SetURI(annot.get(), kUri));
    VerifyUriActionInLink(document(), FPDFAnnot_GetLink(annot.get()), kUri);

    // Negative tests:
    EXPECT_FALSE(FPDFAnnot_SetURI(nullptr, nullptr));
    VerifyUriActionInLink(document(), FPDFAnnot_GetLink(annot.get()), kUri);
    EXPECT_FALSE(FPDFAnnot_SetURI(annot.get(), nullptr));
    VerifyUriActionInLink(document(), FPDFAnnot_GetLink(annot.get()), kUri);
    EXPECT_FALSE(FPDFAnnot_SetURI(nullptr, kUri));
    VerifyUriActionInLink(document(), FPDFAnnot_GetLink(annot.get()), kUri);

    // Position the link on top of "Hello, world!" without a border.
    const FS_RECTF kRect = {19.0f, 48.0f, 85.0f, 60.0f};
    EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &kRect));
    EXPECT_TRUE(FPDFAnnot_SetBorder(annot.get(), /*horizontal_radius=*/0.0f,
                                    /*vertical_radius=*/0.0f,
                                    /*border_width=*/0.0f));

    VerifyUriActionInLink(
        document(), FPDFLink_GetLinkAtPoint(page.get(), 40.0, 50.0), kUri);
  }

  {
    // Add an ink annotation to the page. Trying to add a link to it fails.
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_INK));
    ASSERT_TRUE(annot);
    EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));
    EXPECT_EQ(FPDF_ANNOT_INK, FPDFAnnot_GetSubtype(annot.get()));
    EXPECT_FALSE(FPDFAnnot_SetURI(annot.get(), kUri));
  }

  // Remove the ink annotation added above for negative testing.
  EXPECT_TRUE(FPDFPage_RemoveAnnot(page.get(), 1));
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));

  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  // Reopen the document and make sure it still renders the same. Since the link
  // does not have a border, it does not affect the rendering.
  {
    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    ScopedSavedPage saved_page = LoadScopedSavedPage(0);
    ASSERT_TRUE(saved_page);
    VerifySavedRenderingWithExpectationSuffix(saved_page.get(),
                                              pdfium::kHelloWorldPng);
    EXPECT_EQ(1, FPDFPage_GetAnnotCount(saved_page.get()));

    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(saved_page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_LINK, FPDFAnnot_GetSubtype(annot.get()));
    VerifyUriActionInLink(document(), FPDFAnnot_GetLink(annot.get()), kUri);
    VerifyUriActionInLink(document(),
                          FPDFLink_GetLinkAtPoint(saved_page.get(), 40.0, 50.0),
                          kUri);
  }
}

TEST_F(FPDFAnnotEmbedderTest, AddAndSaveUnderlineAnnotation) {
  // Open a file with one annotation and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_highlight_long_content.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // Check that there is a total of one annotation on its first page, and verify
  // its quadpoints.
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
  FS_QUADPOINTSF quadpoints;
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    ASSERT_TRUE(FPDFAnnot_GetAttachmentPoints(annot.get(), 0, &quadpoints));
    EXPECT_EQ(115.802643f, quadpoints.x1);
    EXPECT_EQ(718.913940f, quadpoints.y1);
    EXPECT_EQ(157.211166f, quadpoints.x4);
    EXPECT_EQ(706.264404f, quadpoints.y4);
  }

  // Add an underline annotation to the page and set its quadpoints.
  {
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_UNDERLINE));
    ASSERT_TRUE(annot);
    quadpoints.x1 = 140.802643f;
    quadpoints.x3 = 140.802643f;
    ASSERT_TRUE(FPDFAnnot_AppendAttachmentPoints(annot.get(), &quadpoints));
  }

  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  {
    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    ScopedSavedPage saved_page = LoadScopedSavedPage(0);
    ASSERT_TRUE(saved_page);
    VerifySavedRenderingWithFuzzyExpectationSuffix(
        saved_page.get(), "annotation_highlight_long_content_added_underline");

    // Check that the saved document has 2 annotations on the first page
    EXPECT_EQ(2, FPDFPage_GetAnnotCount(saved_page.get()));

    // Check that the second annotation is an underline annotation and verify
    // its quadpoints.
    ScopedFPDFAnnotation new_annot(FPDFPage_GetAnnot(saved_page.get(), 1));
    ASSERT_TRUE(new_annot);
    EXPECT_EQ(FPDF_ANNOT_UNDERLINE, FPDFAnnot_GetSubtype(new_annot.get()));
    FS_QUADPOINTSF new_quadpoints;
    ASSERT_TRUE(
        FPDFAnnot_GetAttachmentPoints(new_annot.get(), 0, &new_quadpoints));
    EXPECT_NEAR(quadpoints.x1, new_quadpoints.x1, 0.001f);
    EXPECT_NEAR(quadpoints.y1, new_quadpoints.y1, 0.001f);
    EXPECT_NEAR(quadpoints.x4, new_quadpoints.x4, 0.001f);
    EXPECT_NEAR(quadpoints.y4, new_quadpoints.y4, 0.001f);
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetAndSetQuadPoints) {
  // Open a file with four annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_highlight_square_with_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(4, FPDFPage_GetAnnotCount(page.get()));

  // Retrieve the highlight annotation.
  FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), 0);
  ASSERT_TRUE(annot);
  ASSERT_EQ(FPDF_ANNOT_HIGHLIGHT, FPDFAnnot_GetSubtype(annot));

  FS_QUADPOINTSF quadpoints;
  ASSERT_TRUE(FPDFAnnot_GetAttachmentPoints(annot, 0, &quadpoints));

  {
    // Verify the current one set of quadpoints.
    ASSERT_EQ(1u, FPDFAnnot_CountAttachmentPoints(annot));

    EXPECT_NEAR(72.0000f, quadpoints.x1, 0.001f);
    EXPECT_NEAR(720.792f, quadpoints.y1, 0.001f);
    EXPECT_NEAR(132.055f, quadpoints.x4, 0.001f);
    EXPECT_NEAR(704.796f, quadpoints.y4, 0.001f);
  }

  {
    // Update the quadpoints.
    FS_QUADPOINTSF new_quadpoints = quadpoints;
    new_quadpoints.y1 -= 20.f;
    new_quadpoints.y2 -= 20.f;
    new_quadpoints.y3 -= 20.f;
    new_quadpoints.y4 -= 20.f;
    ASSERT_TRUE(FPDFAnnot_SetAttachmentPoints(annot, 0, &new_quadpoints));

    // Verify added quadpoint set
    ASSERT_EQ(1u, FPDFAnnot_CountAttachmentPoints(annot));
    ASSERT_TRUE(FPDFAnnot_GetAttachmentPoints(annot, 0, &quadpoints));
    EXPECT_NEAR(new_quadpoints.x1, quadpoints.x1, 0.001f);
    EXPECT_NEAR(new_quadpoints.y1, quadpoints.y1, 0.001f);
    EXPECT_NEAR(new_quadpoints.x4, quadpoints.x4, 0.001f);
    EXPECT_NEAR(new_quadpoints.y4, quadpoints.y4, 0.001f);
  }

  {
    // Append a new set of quadpoints.
    FS_QUADPOINTSF new_quadpoints = quadpoints;
    new_quadpoints.y1 += 20.f;
    new_quadpoints.y2 += 20.f;
    new_quadpoints.y3 += 20.f;
    new_quadpoints.y4 += 20.f;
    ASSERT_TRUE(FPDFAnnot_AppendAttachmentPoints(annot, &new_quadpoints));

    // Verify added quadpoint set
    ASSERT_EQ(2u, FPDFAnnot_CountAttachmentPoints(annot));
    ASSERT_TRUE(FPDFAnnot_GetAttachmentPoints(annot, 1, &quadpoints));
    EXPECT_NEAR(new_quadpoints.x1, quadpoints.x1, 0.001f);
    EXPECT_NEAR(new_quadpoints.y1, quadpoints.y1, 0.001f);
    EXPECT_NEAR(new_quadpoints.x4, quadpoints.x4, 0.001f);
    EXPECT_NEAR(new_quadpoints.y4, quadpoints.y4, 0.001f);
  }

  {
    // Setting and getting quadpoints at out-of-bound index should fail
    EXPECT_FALSE(FPDFAnnot_SetAttachmentPoints(annot, 300000, &quadpoints));
    EXPECT_FALSE(FPDFAnnot_GetAttachmentPoints(annot, 300000, &quadpoints));
  }

  FPDFPage_CloseAnnot(annot);

  // Retrieve the square annotation
  FPDF_ANNOTATION squareAnnot = FPDFPage_GetAnnot(page.get(), 2);

  {
    // Check that attempting to set its quadpoints would fail
    ASSERT_TRUE(squareAnnot);
    EXPECT_EQ(FPDF_ANNOT_SQUARE, FPDFAnnot_GetSubtype(squareAnnot));
    EXPECT_EQ(0u, FPDFAnnot_CountAttachmentPoints(squareAnnot));
    EXPECT_FALSE(FPDFAnnot_SetAttachmentPoints(squareAnnot, 0, &quadpoints));
  }

  FPDFPage_CloseAnnot(squareAnnot);
}

TEST_F(FPDFAnnotEmbedderTest, ModifyRectQuadpointsWithAP) {
  // Open a file with four annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_highlight_square_with_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(4, FPDFPage_GetAnnotCount(page.get()));

  // Check that the original file renders correctly.
  static constexpr char kAnnotationHighlightSquareWithApPng[] =
      "annotation_highlight_square_with_ap";
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithFuzzyExpectationSuffix(
        bitmap.get(), kAnnotationHighlightSquareWithApPng);
  }

  FS_RECTF rect;
  FS_RECTF new_rect;

  // Retrieve the highlight annotation which has its AP stream already defined.
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_HIGHLIGHT, FPDFAnnot_GetSubtype(annot.get()));

    // Check that color cannot be set when an AP stream is defined already.
    EXPECT_FALSE(FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, 51,
                                    102, 153, 204));

    // Verify its attachment points.
    FS_QUADPOINTSF quadpoints;
    ASSERT_TRUE(FPDFAnnot_GetAttachmentPoints(annot.get(), 0, &quadpoints));
    EXPECT_NEAR(72.0000f, quadpoints.x1, 0.001f);
    EXPECT_NEAR(720.792f, quadpoints.y1, 0.001f);
    EXPECT_NEAR(132.055f, quadpoints.x4, 0.001f);
    EXPECT_NEAR(704.796f, quadpoints.y4, 0.001f);

    // Check that updating the attachment points would succeed.
    quadpoints.x1 -= 50.f;
    quadpoints.x2 -= 50.f;
    quadpoints.x3 -= 50.f;
    quadpoints.x4 -= 50.f;
    ASSERT_TRUE(FPDFAnnot_SetAttachmentPoints(annot.get(), 0, &quadpoints));
    FS_QUADPOINTSF new_quadpoints;
    ASSERT_TRUE(FPDFAnnot_GetAttachmentPoints(annot.get(), 0, &new_quadpoints));
    EXPECT_EQ(quadpoints.x1, new_quadpoints.x1);
    EXPECT_EQ(quadpoints.y1, new_quadpoints.y1);
    EXPECT_EQ(quadpoints.x4, new_quadpoints.x4);
    EXPECT_EQ(quadpoints.y4, new_quadpoints.y4);

    // Check that updating quadpoints does not change the annotation's position.
    {
      ScopedFPDFBitmap bitmap =
          RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
      CompareBitmapWithFuzzyExpectationSuffix(
          bitmap.get(), kAnnotationHighlightSquareWithApPng);
    }

    // Verify its annotation rectangle.
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
    EXPECT_NEAR(67.7299f, rect.left, 0.001f);
    EXPECT_NEAR(704.296f, rect.bottom, 0.001f);
    EXPECT_NEAR(136.325f, rect.right, 0.001f);
    EXPECT_NEAR(721.292f, rect.top, 0.001f);

    // Check that updating the rectangle would succeed.
    rect.left -= 60.f;
    rect.right -= 60.f;
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &new_rect));
    EXPECT_EQ(rect.right, new_rect.right);
  }

  // Check that updating the rectangle changes the annotation's position.
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithFuzzyExpectationSuffix(
        bitmap.get(), "annotation_highlight_square_with_ap_modified_highlight");
  }

  {
    // Retrieve the square annotation which has its AP stream already defined.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 2));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_SQUARE, FPDFAnnot_GetSubtype(annot.get()));

    // Check that updating the rectangle would succeed.
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
    rect.left += 70.f;
    rect.right += 70.f;
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &new_rect));
    EXPECT_EQ(rect.right, new_rect.right);

    // Check that updating the rectangle changes the square annotation's
    // position.
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithFuzzyExpectationSuffix(
        bitmap.get(), "annotation_highlight_square_with_ap_modified_square");
  }
}

TEST_F(FPDFAnnotEmbedderTest, CountAttachmentPoints) {
  // Open a file with multiline markup annotations.
  ASSERT_TRUE(OpenDocument("annotation_markup_multiline_no_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // This is a three line annotation.
    EXPECT_EQ(3u, FPDFAnnot_CountAttachmentPoints(annot.get()));
  }

  // null annotation should return 0
  EXPECT_EQ(0u, FPDFAnnot_CountAttachmentPoints(nullptr));
}

TEST_F(FPDFAnnotEmbedderTest, RemoveAnnotation) {
  // Open a file with 3 annotations on its first page.
  ASSERT_TRUE(OpenDocument("annotation_ink_multiple.pdf"));
  FPDF_PAGE page = LoadPageNoEvents(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(3, FPDFPage_GetAnnotCount(page));

  FS_RECTF rect;

  // Check that the annotations have the expected rectangle coordinates.
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, 0));
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
    EXPECT_NEAR(86.1971f, rect.left, 0.001f);
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, 1));
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
    EXPECT_NEAR(149.8127f, rect.left, 0.001f);
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, 2));
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
    EXPECT_NEAR(351.8204f, rect.left, 0.001f);
  }

  // Check that nothing happens when attempting to remove an annotation with an
  // out-of-bound index.
  EXPECT_FALSE(FPDFPage_RemoveAnnot(page, 4));
  EXPECT_FALSE(FPDFPage_RemoveAnnot(page, -1));
  EXPECT_EQ(3, FPDFPage_GetAnnotCount(page));

  // Remove the second annotation.
  EXPECT_TRUE(FPDFPage_RemoveAnnot(page, 1));
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(page));
  EXPECT_FALSE(FPDFPage_GetAnnot(page, 2));

  // Save the document and close the page.
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  UnloadPageNoEvents(page);

  // TODO(npm): VerifySavedRendering changes annot rect dimensions by 1??
  // Open the saved document.
  std::string new_file = GetString();
  FPDF_FILEACCESS file_access = {};  // Aggregate initialization
  static_assert(std::is_aggregate_v<decltype(file_access)>);
  file_access.m_FileLen = new_file.size();
  file_access.m_GetBlock = GetBlockFromString;
  file_access.m_Param = &new_file;
  ScopedFPDFDocument new_doc(FPDF_LoadCustomDocument(&file_access, nullptr));
  ASSERT_TRUE(new_doc);
  ScopedFPDFPage new_page(FPDF_LoadPage(new_doc.get(), 0));
  ASSERT_TRUE(new_page);

  // Check that the saved document has 2 annotations on the first page.
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(new_page.get()));

  // Check that the remaining 2 annotations are the original 1st and 3rd ones
  // by verifying their rectangle coordinates.
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(new_page.get(), 0));
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
    EXPECT_NEAR(86.1971f, rect.left, 0.001f);
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(new_page.get(), 1));
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
    EXPECT_NEAR(351.8204f, rect.left, 0.001f);
  }
}

TEST_F(FPDFAnnotEmbedderTest, AddAndModifyPath) {
  // Open a file with two annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_stamp_with_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

  // Check that the page renders correctly.
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(bitmap.get(), kAnnotationStampWithApPng);
  }

  {
    // Retrieve the stamp annotation which has its AP stream already defined.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Check that this annotation has one path object and retrieve it.
    EXPECT_EQ(1, FPDFAnnot_GetObjectCount(annot.get()));
    ASSERT_EQ(32, FPDFPage_CountObjects(page.get()));
    FPDF_PAGEOBJECT path = FPDFAnnot_GetObject(annot.get(), 1);
    EXPECT_FALSE(path);
    path = FPDFAnnot_GetObject(annot.get(), 0);
    EXPECT_EQ(FPDF_PAGEOBJ_PATH, FPDFPageObj_GetType(path));
    EXPECT_TRUE(path);

    // Modify the color of the path object.
    EXPECT_TRUE(FPDFPageObj_SetStrokeColor(path, 0, 0, 0, 255));
    EXPECT_TRUE(FPDFAnnot_UpdateObject(annot.get(), path));

    // Check that the page with the modified annotation renders correctly.
    {
      ScopedFPDFBitmap bitmap =
          RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
      CompareBitmapWithExpectationSuffix(
          bitmap.get(), "annotation_stamp_with_ap_modified_path");
    }

    // Add a second path object to the same annotation.
    FPDF_PAGEOBJECT dot = FPDFPageObj_CreateNewPath(7, 84);
    EXPECT_TRUE(FPDFPath_BezierTo(dot, 9, 86, 10, 87, 11, 88));
    EXPECT_TRUE(FPDFPageObj_SetStrokeColor(dot, 255, 0, 0, 100));
    EXPECT_TRUE(FPDFPageObj_SetStrokeWidth(dot, 14));
    EXPECT_TRUE(FPDFPath_SetDrawMode(dot, 0, 1));
    EXPECT_TRUE(FPDFAnnot_AppendObject(annot.get(), dot));
    EXPECT_EQ(2, FPDFAnnot_GetObjectCount(annot.get()));

    // The object is in the annontation, not in the page, so the page object
    // array should not change.
    ASSERT_EQ(32, FPDFPage_CountObjects(page.get()));

    // Check that the page with an annotation with two paths renders correctly.
    {
      ScopedFPDFBitmap bitmap =
          RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
      CompareBitmapWithExpectationSuffix(bitmap.get(),
                                         "annotation_stamp_with_ap_two_paths");
    }

    // Delete the newly added path object.
    EXPECT_TRUE(FPDFAnnot_RemoveObject(annot.get(), 1));
    EXPECT_EQ(1, FPDFAnnot_GetObjectCount(annot.get()));
    ASSERT_EQ(32, FPDFPage_CountObjects(page.get()));
  }

  // Check that the page renders the same as before.
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(
        bitmap.get(), "annotation_stamp_with_ap_modified_path");
  }

  FS_RECTF rect;

  {
    // Create another stamp annotation and set its annotation rectangle.
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_STAMP));
    ASSERT_TRUE(annot);
    rect.left = 200.f;
    rect.bottom = 400.f;
    rect.right = 500.f;
    rect.top = 600.f;
    EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));

    // Add a new path to the annotation.
    FPDF_PAGEOBJECT check = FPDFPageObj_CreateNewPath(200, 500);
    EXPECT_TRUE(FPDFPath_LineTo(check, 300, 400));
    EXPECT_TRUE(FPDFPath_LineTo(check, 500, 600));
    EXPECT_TRUE(FPDFPath_MoveTo(check, 350, 550));
    EXPECT_TRUE(FPDFPath_LineTo(check, 450, 450));
    EXPECT_TRUE(FPDFPageObj_SetStrokeColor(check, 0, 255, 255, 180));
    EXPECT_TRUE(FPDFPageObj_SetStrokeWidth(check, 8.35f));
    EXPECT_TRUE(FPDFPath_SetDrawMode(check, 0, 1));
    EXPECT_TRUE(FPDFAnnot_AppendObject(annot.get(), check));
    EXPECT_EQ(1, FPDFAnnot_GetObjectCount(annot.get()));

    // Check that the annotation's bounding box came from its rectangle.
    FS_RECTF new_rect;
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &new_rect));
    EXPECT_EQ(rect.left, new_rect.left);
    EXPECT_EQ(rect.bottom, new_rect.bottom);
    EXPECT_EQ(rect.right, new_rect.right);
    EXPECT_EQ(rect.top, new_rect.top);
  }

  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  {
    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    ScopedSavedPage saved_page = LoadScopedSavedPage(0);
    ASSERT_TRUE(saved_page);
    VerifySavedRenderingWithExpectationSuffix(
        saved_page.get(), "annotation_stamp_with_ap_new_annot");

    // Check that the document has a correct count of annotations and objects.
    EXPECT_EQ(3, FPDFPage_GetAnnotCount(saved_page.get()));

    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(saved_page.get(), 2));
    ASSERT_TRUE(annot);
    EXPECT_EQ(1, FPDFAnnot_GetObjectCount(annot.get()));

    // Check that the new annotation's rectangle is as defined.
    FS_RECTF new_rect;
    ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &new_rect));
    EXPECT_EQ(rect.left, new_rect.left);
    EXPECT_EQ(rect.bottom, new_rect.bottom);
    EXPECT_EQ(rect.right, new_rect.right);
    EXPECT_EQ(rect.top, new_rect.top);
  }
}

TEST_F(FPDFAnnotEmbedderTest, ModifyAnnotationFlags) {
  // Open a file with an annotation and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_highlight_rollover_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // Check that the page renders correctly.
  static constexpr char kAnnotationHighlightRolloverApPng[] =
      "annotation_highlight_rollover_ap";
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmap(bitmap.get(), kAnnotationHighlightRolloverApPng);
  }

  {
    // Retrieve the annotation.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Check that the original flag values are as expected.
    int flags = FPDFAnnot_GetFlags(annot.get());
    EXPECT_FALSE(flags & FPDF_ANNOT_FLAG_INVISIBLE);
    EXPECT_FALSE(flags & FPDF_ANNOT_FLAG_HIDDEN);
    EXPECT_TRUE(flags & FPDF_ANNOT_FLAG_PRINT);
    EXPECT_FALSE(flags & FPDF_ANNOT_FLAG_NOZOOM);
    EXPECT_FALSE(flags & FPDF_ANNOT_FLAG_NOROTATE);
    EXPECT_FALSE(flags & FPDF_ANNOT_FLAG_NOVIEW);
    EXPECT_FALSE(flags & FPDF_ANNOT_FLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_ANNOT_FLAG_LOCKED);
    EXPECT_FALSE(flags & FPDF_ANNOT_FLAG_TOGGLENOVIEW);

    // Set the HIDDEN flag.
    flags |= FPDF_ANNOT_FLAG_HIDDEN;
    EXPECT_TRUE(FPDFAnnot_SetFlags(annot.get(), flags));
    flags = FPDFAnnot_GetFlags(annot.get());
    EXPECT_TRUE(flags & FPDF_ANNOT_FLAG_HIDDEN);
    EXPECT_TRUE(flags & FPDF_ANNOT_FLAG_PRINT);

    // Check that the page renders correctly without rendering the annotation.
    {
      ScopedFPDFBitmap bitmap =
          RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
      CompareBitmap(bitmap.get(), pdfium::kBlankPage612By792Png);
    }

    // Unset the HIDDEN flag.
    EXPECT_TRUE(FPDFAnnot_SetFlags(annot.get(), FPDF_ANNOT_FLAG_NONE));
    EXPECT_FALSE(FPDFAnnot_GetFlags(annot.get()));
    flags &= ~FPDF_ANNOT_FLAG_HIDDEN;
    EXPECT_TRUE(FPDFAnnot_SetFlags(annot.get(), flags));
    flags = FPDFAnnot_GetFlags(annot.get());
    EXPECT_FALSE(flags & FPDF_ANNOT_FLAG_HIDDEN);
    EXPECT_TRUE(flags & FPDF_ANNOT_FLAG_PRINT);

    // Check that the page renders correctly as before.
    {
      ScopedFPDFBitmap bitmap =
          RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
      CompareBitmap(bitmap.get(), kAnnotationHighlightRolloverApPng);
    }
  }
}

TEST_F(FPDFAnnotEmbedderTest, AddAndModifyImage) {
  // Open a file with two annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_stamp_with_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

  // Check that the page renders correctly.
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(bitmap.get(), kAnnotationStampWithApPng);
  }

  static constexpr int kBitmapSize = 200;
  FPDF_BITMAP image_bitmap;

  {
    // Create a stamp annotation and set its annotation rectangle.
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_STAMP));
    FPDF_PAGE page0 = page.get();
    ASSERT_TRUE(annot);
    FS_RECTF rect;
    rect.left = 200.f;
    rect.bottom = 600.f;
    rect.right = 400.f;
    rect.top = 800.f;
    EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));

    // Add a solid-color translucent image object to the new annotation.
    image_bitmap = FPDFBitmap_Create(kBitmapSize, kBitmapSize, 1);
    ASSERT_TRUE(FPDFBitmap_FillRect(image_bitmap, 0, 0, kBitmapSize,
                                    kBitmapSize, 0xeeeecccc));
    EXPECT_EQ(kBitmapSize, FPDFBitmap_GetWidth(image_bitmap));
    EXPECT_EQ(kBitmapSize, FPDFBitmap_GetHeight(image_bitmap));
    FPDF_PAGEOBJECT image_object = FPDFPageObj_NewImageObj(document());
    ASSERT_TRUE(FPDFImageObj_SetBitmap(&page0, 0, image_object, image_bitmap));
    static constexpr FS_MATRIX kBitmapScaleMatrix{kBitmapSize, 0, 0,
                                                  kBitmapSize, 0, 0};
    ASSERT_TRUE(FPDFPageObj_SetMatrix(image_object, &kBitmapScaleMatrix));
    FPDFPageObj_Transform(image_object, 1, 0, 0, 1, 200, 600);
    EXPECT_TRUE(FPDFAnnot_AppendObject(annot.get(), image_object));
  }

  // Check that the page renders correctly with the new image object.
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithFuzzyExpectationSuffix(
        bitmap.get(), "annotation_stamp_with_ap_new_image");
  }

  {
    // Retrieve the newly added stamp annotation and its image object.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 2));
    ASSERT_TRUE(annot);
    EXPECT_EQ(1, FPDFAnnot_GetObjectCount(annot.get()));
    FPDF_PAGEOBJECT image_object = FPDFAnnot_GetObject(annot.get(), 0);
    EXPECT_EQ(FPDF_PAGEOBJ_IMAGE, FPDFPageObj_GetType(image_object));

    // Modify the image in the new annotation.
    ASSERT_TRUE(FPDFBitmap_FillRect(image_bitmap, 0, 0, kBitmapSize,
                                    kBitmapSize, 0xff000000));
    FPDF_PAGE page_ptr = page.get();
    ASSERT_TRUE(
        FPDFImageObj_SetBitmap(&page_ptr, 0, image_object, image_bitmap));
    EXPECT_TRUE(FPDFAnnot_UpdateObject(annot.get(), image_object));
  }

  // Save the document and close the page.
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  FPDFBitmap_Destroy(image_bitmap);

  // Test that the saved document renders the modified image object correctly.
  VerifySavedDocumentWithExpectationSuffix(
      "annotation_stamp_with_ap_modified_image");
}

TEST_F(FPDFAnnotEmbedderTest, AddAndModifyText) {
  // Open a file with two annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_stamp_with_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

  // Check that the page renders correctly.
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(bitmap.get(), kAnnotationStampWithApPng);
  }

  {
    // Create a stamp annotation and set its annotation rectangle.
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_STAMP));
    ASSERT_TRUE(annot);
    FS_RECTF rect;
    rect.left = 200.f;
    rect.bottom = 550.f;
    rect.right = 450.f;
    rect.top = 650.f;
    EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));

    // Add a translucent text object to the new annotation.
    FPDF_PAGEOBJECT text_object =
        FPDFPageObj_NewTextObj(document(), "Arial", 12.0f);
    EXPECT_TRUE(text_object);
    ScopedFPDFWideString text =
        GetFPDFWideString(L"I'm a translucent text laying on other text.");
    EXPECT_TRUE(FPDFText_SetText(text_object, text.get()));
    EXPECT_TRUE(FPDFPageObj_SetFillColor(text_object, 0, 0, 255, 150));
    FPDFPageObj_Transform(text_object, 1, 0, 0, 1, 200, 600);
    EXPECT_TRUE(FPDFAnnot_AppendObject(annot.get(), text_object));
  }

  // Check that the page renders correctly with the new text object.
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithFuzzyExpectationSuffix(
        bitmap.get(), "annotation_stamp_with_ap_new_text");
  }

  {
    // Retrieve the newly added stamp annotation and its text object.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 2));
    ASSERT_TRUE(annot);
    EXPECT_EQ(1, FPDFAnnot_GetObjectCount(annot.get()));
    FPDF_PAGEOBJECT text_object = FPDFAnnot_GetObject(annot.get(), 0);
    EXPECT_EQ(FPDF_PAGEOBJ_TEXT, FPDFPageObj_GetType(text_object));

    // Modify the text in the new annotation.
    ScopedFPDFWideString new_text = GetFPDFWideString(L"New text!");
    EXPECT_TRUE(FPDFText_SetText(text_object, new_text.get()));
    EXPECT_TRUE(FPDFAnnot_UpdateObject(annot.get(), text_object));
  }

  // Check that the page renders correctly with the modified text object.
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithFuzzyExpectationSuffix(
        bitmap.get(), "annotation_stamp_with_ap_modified_text");
  }

  // Remove the new annotation, and check that the page renders as before.
  EXPECT_TRUE(FPDFPage_RemoveAnnot(page.get(), 2));
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(bitmap.get(), kAnnotationStampWithApPng);
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetSetStringValue) {
  // Open a file with four annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_stamp_with_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  static const wchar_t kNewDate[] = L"D:201706282359Z00'00'";

  {
    // Retrieve the first annotation.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Check that a non-existent key does not exist.
    EXPECT_FALSE(FPDFAnnot_HasKey(annot.get(), "none"));

    // Check that the string value of a non-string dictionary entry is empty.
    EXPECT_TRUE(FPDFAnnot_HasKey(annot.get(), pdfium::annotation::kAP));
    EXPECT_EQ(FPDF_OBJECT_REFERENCE,
              FPDFAnnot_GetValueType(annot.get(), pdfium::annotation::kAP));
    EXPECT_EQ(2u, FPDFAnnot_GetStringValue(annot.get(), pdfium::annotation::kAP,
                                           nullptr, 0));

    // Check that the string value of the hash is correct.
    static const char kHashKey[] = "AAPL:Hash";
    EXPECT_EQ(FPDF_OBJECT_NAME, FPDFAnnot_GetValueType(annot.get(), kHashKey));
    unsigned long length_bytes =
        FPDFAnnot_GetStringValue(annot.get(), kHashKey, nullptr, 0);
    ASSERT_EQ(66u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(66u, FPDFAnnot_GetStringValue(annot.get(), kHashKey, buf.data(),
                                            length_bytes));
    EXPECT_EQ(L"395fbcb98d558681742f30683a62a2ad",
              GetPlatformWString(buf.data()));

    // Check that the string value of the modified date is correct.
    EXPECT_EQ(FPDF_OBJECT_NAME, FPDFAnnot_GetValueType(annot.get(), kHashKey));
    length_bytes = FPDFAnnot_GetStringValue(annot.get(), pdfium::annotation::kM,
                                            nullptr, 0);
    ASSERT_EQ(44u, length_bytes);
    buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(44u, FPDFAnnot_GetStringValue(annot.get(), pdfium::annotation::kM,
                                            buf.data(), length_bytes));
    EXPECT_EQ(L"D:201706071721Z00'00'", GetPlatformWString(buf.data()));

    // Update the date entry for the annotation.
    ScopedFPDFWideString text = GetFPDFWideString(kNewDate);
    EXPECT_TRUE(FPDFAnnot_SetStringValue(annot.get(), pdfium::annotation::kM,
                                         text.get()));
  }

  // Save the document and close the page.
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  {
    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    ScopedSavedPage saved_page = LoadScopedSavedPage(0);
    ASSERT_TRUE(saved_page);
    VerifySavedRenderingWithExpectationSuffix(saved_page.get(),
                                              kAnnotationStampWithApPng);

    ScopedFPDFAnnotation new_annot(FPDFPage_GetAnnot(saved_page.get(), 0));

    // Check that the string value of the modified date is the newly-set
    // value.
    EXPECT_EQ(FPDF_OBJECT_STRING,
              FPDFAnnot_GetValueType(new_annot.get(), pdfium::annotation::kM));
    unsigned long length_bytes = FPDFAnnot_GetStringValue(
        new_annot.get(), pdfium::annotation::kM, nullptr, 0);
    ASSERT_EQ(44u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(44u,
              FPDFAnnot_GetStringValue(new_annot.get(), pdfium::annotation::kM,
                                       buf.data(), length_bytes));
    EXPECT_EQ(kNewDate, GetPlatformWString(buf.data()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetNumberValue) {
  // Open a file with four text annotations and load its first page.
  ASSERT_TRUE(OpenDocument("text_form_multiple.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  {
    // First two annotations do not have "MaxLen" attribute.
    for (int i = 0; i < 2; i++) {
      ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), i));
      ASSERT_TRUE(annot);

      // Verify that no "MaxLen" key present.
      EXPECT_FALSE(FPDFAnnot_HasKey(annot.get(), "MaxLen"));

      float value;
      EXPECT_FALSE(FPDFAnnot_GetNumberValue(annot.get(), "MaxLen", &value));
    }

    // Annotation in index 2 has "MaxLen" of 10.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 2));
    ASSERT_TRUE(annot);

    // Verify that "MaxLen" key present.
    EXPECT_TRUE(FPDFAnnot_HasKey(annot.get(), "MaxLen"));

    float value;
    EXPECT_TRUE(FPDFAnnot_GetNumberValue(annot.get(), "MaxLen", &value));
    EXPECT_FLOAT_EQ(10.0f, value);

    // Check bad inputs.
    EXPECT_FALSE(FPDFAnnot_GetNumberValue(nullptr, "MaxLen", &value));
    EXPECT_FALSE(FPDFAnnot_GetNumberValue(annot.get(), nullptr, &value));
    EXPECT_FALSE(FPDFAnnot_GetNumberValue(annot.get(), "MaxLen", nullptr));
    // Ask for key that exists but is not a number.
    EXPECT_FALSE(FPDFAnnot_GetNumberValue(annot.get(), "V", &value));
  }
}

TEST_F(FPDFAnnotEmbedderTest, EmbedMetadata) {
  ASSERT_TRUE(OpenDocument("text_form_multiple.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(annot);

  EXPECT_FALSE(EPDFAnnot_HasEmbedMetadata(annot.get()));

  ScopedFPDFWideString user_id = GetFPDFWideString(L"44");
  EXPECT_TRUE(
      EPDFAnnot_SetEmbedMetadataString(annot.get(), "UserID", user_id.get()));

  unsigned long length_bytes =
      EPDFAnnot_GetEmbedMetadataString(annot.get(), "UserID", nullptr, 0);
  ASSERT_EQ(6u, length_bytes);
  std::vector<FPDF_WCHAR> string_buffer = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(length_bytes,
            EPDFAnnot_GetEmbedMetadataString(
                annot.get(), "UserID", string_buffer.data(), length_bytes));
  EXPECT_EQ(L"44", GetPlatformWString(string_buffer.data()));

  EXPECT_TRUE(EPDFAnnot_SetEmbedMetadataNumber(annot.get(), "Rotation", 12.5f));
  float number_value = 0.0f;
  EXPECT_TRUE(
      EPDFAnnot_GetEmbedMetadataNumber(annot.get(), "Rotation", &number_value));
  EXPECT_FLOAT_EQ(12.5f, number_value);

  EXPECT_TRUE(EPDFAnnot_SetEmbedMetadataBoolean(annot.get(), "Archived", true));
  FPDF_BOOL boolean_value = false;
  EXPECT_TRUE(EPDFAnnot_GetEmbedMetadataBoolean(annot.get(), "Archived",
                                                &boolean_value));
  EXPECT_TRUE(boolean_value);

  const FS_RECTF rect{1.0f, 2.0f, 3.0f, 4.0f};
  EXPECT_TRUE(
      EPDFAnnot_SetEmbedMetadataRect(annot.get(), "UnrotatedRect", &rect));
  FS_RECTF rect_value;
  EXPECT_TRUE(EPDFAnnot_GetEmbedMetadataRect(annot.get(), "UnrotatedRect",
                                             &rect_value));
  EXPECT_FLOAT_EQ(rect.left, rect_value.left);
  EXPECT_FLOAT_EQ(rect.bottom, rect_value.bottom);
  EXPECT_FLOAT_EQ(rect.right, rect_value.right);
  EXPECT_FLOAT_EQ(rect.top, rect_value.top);

  ScopedFPDFWideString json = GetFPDFWideString(L"{\"source\":\"test\"}");
  EXPECT_TRUE(EPDFAnnot_SetEmbedMetadataJSON(annot.get(), json.get()));
  length_bytes = EPDFAnnot_GetEmbedMetadataJSON(annot.get(), nullptr, 0);
  ASSERT_EQ(36u, length_bytes);
  string_buffer = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(length_bytes, EPDFAnnot_GetEmbedMetadataJSON(
                              annot.get(), string_buffer.data(), length_bytes));
  EXPECT_EQ(L"{\"source\":\"test\"}", GetPlatformWString(string_buffer.data()));

  EXPECT_TRUE(EPDFAnnot_HasEmbedMetadata(annot.get()));
  EXPECT_TRUE(EPDFAnnot_ClearEmbedMetadataKey(annot.get(), "UserID"));
  EXPECT_TRUE(EPDFAnnot_HasEmbedMetadata(annot.get()));
  EXPECT_EQ(
      2u, EPDFAnnot_GetEmbedMetadataString(annot.get(), "UserID", nullptr, 0));

  EXPECT_TRUE(EPDFAnnot_ClearEmbedMetadata(annot.get()));
  EXPECT_FALSE(EPDFAnnot_HasEmbedMetadata(annot.get()));
}

TEST_F(FPDFAnnotEmbedderTest, GetSetAP) {
  // Open a file with four annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_stamp_with_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    static const char kMd5NormalAP[] = "be903df0343fd774fadab9c8900cdf4a";
    static constexpr size_t kExpectNormalAPLength = 73970;

    // Retrieve the first annotation.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Check that the string value of an AP returns the expected length.
    unsigned long normal_length_bytes = FPDFAnnot_GetAP(
        annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0);
    ASSERT_EQ(kExpectNormalAPLength, normal_length_bytes);

    // Check that the string value of an AP is not returned if the buffer is too
    // small. The result buffer should be overwritten with an empty string.
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(normal_length_bytes);
    // Write in the buffer to verify it's not overwritten.
    UNSAFE_TODO(FXSYS_memcpy(buf.data(), "abcdefgh", 8));
    EXPECT_EQ(kExpectNormalAPLength,
              FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                              buf.data(), normal_length_bytes - 1));
    UNSAFE_TODO(EXPECT_EQ(0, memcmp(buf.data(), "abcdefgh", 8)));

    // Check that the string value of an AP is returned through a buffer that is
    // the right size.
    EXPECT_EQ(kExpectNormalAPLength,
              FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                              buf.data(), normal_length_bytes));
    EXPECT_EQ(kMd5NormalAP, GenerateMD5Base16(pdfium::as_byte_span(buf).first(
                                normal_length_bytes)));

    // Check that the string value of an AP is returned through a buffer that is
    // larger than necessary.
    buf = GetFPDFWideStringBuffer(normal_length_bytes + 2);
    EXPECT_EQ(kExpectNormalAPLength,
              FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                              buf.data(), normal_length_bytes + 2));
    EXPECT_EQ(kMd5NormalAP, GenerateMD5Base16(pdfium::as_byte_span(buf).first(
                                normal_length_bytes)));

    // Check that getting an AP for a mode that does not have an AP returns an
    // empty string.
    unsigned long rollover_length_bytes = FPDFAnnot_GetAP(
        annot.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER, nullptr, 0);
    ASSERT_EQ(2u, rollover_length_bytes);

    buf = GetFPDFWideStringBuffer(1000);
    EXPECT_EQ(2u,
              FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER,
                              buf.data(), 1000));
    EXPECT_EQ(L"", GetPlatformWString(buf.data()));

    // Check that setting the AP for an invalid appearance mode fails.
    ScopedFPDFWideString ap_text = GetFPDFWideString(L"new test ap");
    EXPECT_FALSE(FPDFAnnot_SetAP(annot.get(), -1, ap_text.get()));
    EXPECT_FALSE(FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_COUNT,
                                 ap_text.get()));
    EXPECT_FALSE(FPDFAnnot_SetAP(
        annot.get(), FPDF_ANNOT_APPEARANCEMODE_COUNT + 1, ap_text.get()));

    // Set the AP correctly now.
    EXPECT_TRUE(FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER,
                                ap_text.get()));

    // Check that the new annotation value is equal to the value we just set.
    rollover_length_bytes = FPDFAnnot_GetAP(
        annot.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER, nullptr, 0);
    ASSERT_EQ(24u, rollover_length_bytes);
    buf = GetFPDFWideStringBuffer(rollover_length_bytes);
    EXPECT_EQ(24u,
              FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER,
                              buf.data(), rollover_length_bytes));
    EXPECT_EQ(L"new test ap", GetPlatformWString(buf.data()));

    // Check that the Normal AP was not touched when the Rollover AP was set.
    buf = GetFPDFWideStringBuffer(normal_length_bytes);
    EXPECT_EQ(kExpectNormalAPLength,
              FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                              buf.data(), normal_length_bytes));
    EXPECT_EQ(kMd5NormalAP, GenerateMD5Base16(pdfium::as_byte_span(buf).first(
                                normal_length_bytes)));
  }

  // Save the modified document, then reopen it.
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  {
    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    ScopedSavedPage saved_page = LoadScopedSavedPage(0);
    ASSERT_TRUE(saved_page);

    ScopedFPDFAnnotation new_annot(FPDFPage_GetAnnot(saved_page.get(), 0));

    // Check that the new annotation value is equal to the value we set before
    // saving.
    unsigned long rollover_length_bytes = FPDFAnnot_GetAP(
        new_annot.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER, nullptr, 0);
    ASSERT_EQ(24u, rollover_length_bytes);
    std::vector<FPDF_WCHAR> buf =
        GetFPDFWideStringBuffer(rollover_length_bytes);
    EXPECT_EQ(24u, FPDFAnnot_GetAP(new_annot.get(),
                                   FPDF_ANNOT_APPEARANCEMODE_ROLLOVER,
                                   buf.data(), rollover_length_bytes));
    EXPECT_EQ(L"new test ap", GetPlatformWString(buf.data()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, RemoveOptionalAP) {
  // Open a file with four annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_stamp_with_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Retrieve the first annotation.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Set Down AP. Normal AP is already set.
    ScopedFPDFWideString ap_text = GetFPDFWideString(L"new test ap");
    EXPECT_TRUE(FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_DOWN,
                                ap_text.get()));
    EXPECT_EQ(73970u,
              FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                              nullptr, 0));
    EXPECT_EQ(24u, FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_DOWN,
                                   nullptr, 0));

    // Check that setting the Down AP to null removes the Down entry but keeps
    // Normal intact.
    EXPECT_TRUE(
        FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_DOWN, nullptr));
    EXPECT_EQ(73970u,
              FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                              nullptr, 0));
    EXPECT_EQ(2u, FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_DOWN,
                                  nullptr, 0));
  }
}

TEST_F(FPDFAnnotEmbedderTest, RemoveRequiredAP) {
  // Open a file with four annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_stamp_with_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Retrieve the first annotation.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Set Down AP. Normal AP is already set.
    ScopedFPDFWideString ap_text = GetFPDFWideString(L"new test ap");
    EXPECT_TRUE(FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_DOWN,
                                ap_text.get()));
    EXPECT_EQ(73970u,
              FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                              nullptr, 0));
    EXPECT_EQ(24u, FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_DOWN,
                                   nullptr, 0));

    // Check that setting the Normal AP to null removes the whole AP dictionary.
    EXPECT_TRUE(FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                                nullptr));
    EXPECT_EQ(2u, FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                                  nullptr, 0));
    EXPECT_EQ(2u, FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_DOWN,
                                  nullptr, 0));
  }
}

TEST_F(FPDFAnnotEmbedderTest, ExtractLinkedAnnotations) {
  // Open a file with annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_highlight_square_with_ap.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(-1, FPDFPage_GetAnnotIndex(page.get(), nullptr));

  {
    // Retrieve the highlight annotation which has its popup defined.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_HIGHLIGHT, FPDFAnnot_GetSubtype(annot.get()));
    EXPECT_EQ(0, FPDFPage_GetAnnotIndex(page.get(), annot.get()));
    static const char kPopupKey[] = "Popup";
    ASSERT_TRUE(FPDFAnnot_HasKey(annot.get(), kPopupKey));
    ASSERT_EQ(FPDF_OBJECT_REFERENCE,
              FPDFAnnot_GetValueType(annot.get(), kPopupKey));

    // Retrieve and verify the popup of the highlight annotation.
    ScopedFPDFAnnotation popup(
        FPDFAnnot_GetLinkedAnnot(annot.get(), kPopupKey));
    ASSERT_TRUE(popup);
    EXPECT_EQ(FPDF_ANNOT_POPUP, FPDFAnnot_GetSubtype(popup.get()));
    EXPECT_EQ(1, FPDFPage_GetAnnotIndex(page.get(), popup.get()));
    FS_RECTF rect;
    ASSERT_TRUE(FPDFAnnot_GetRect(popup.get(), &rect));
    EXPECT_NEAR(612.0f, rect.left, 0.001f);
    EXPECT_NEAR(578.792, rect.bottom, 0.001f);

    // Attempting to retrieve |annot|'s "IRT"-linked annotation would fail,
    // since "IRT" is not a key in |annot|'s dictionary.
    static const char kIRTKey[] = "IRT";
    ASSERT_FALSE(FPDFAnnot_HasKey(annot.get(), kIRTKey));
    EXPECT_FALSE(FPDFAnnot_GetLinkedAnnot(annot.get(), kIRTKey));

    // Attempting to retrieve |annot|'s parent dictionary as an annotation
    // would fail, since its parent is not an annotation.
    ASSERT_TRUE(FPDFAnnot_HasKey(annot.get(), pdfium::annotation::kP));
    EXPECT_EQ(FPDF_OBJECT_REFERENCE,
              FPDFAnnot_GetValueType(annot.get(), pdfium::annotation::kP));
    EXPECT_FALSE(FPDFAnnot_GetLinkedAnnot(annot.get(), pdfium::annotation::kP));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormFieldFlagsTextField) {
  // Open file with form text fields.
  ASSERT_TRUE(OpenDocument("text_form_multiple.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Retrieve the first annotation: user-editable text field.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Check that the flag values are as expected.
    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_FALSE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_TEXT_MULTILINE);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_TEXT_PASSWORD);
  }

  {
    // Retrieve the second annotation: read-only text field.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    // Check that the flag values are as expected.
    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_TRUE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_TEXT_MULTILINE);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_TEXT_PASSWORD);
  }

  {
    // Retrieve the fourth annotation: user-editable password text field.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 3));
    ASSERT_TRUE(annot);

    // Check that the flag values are as expected.
    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_FALSE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_TEXT_MULTILINE);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_TEXT_PASSWORD);
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormFieldFlagsComboBox) {
  // Open file with form text fields.
  ASSERT_TRUE(OpenDocument("combobox_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Retrieve the first annotation: user-editable combobox.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Check that the flag values are as expected.
    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_FALSE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_CHOICE_COMBO);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_CHOICE_EDIT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
  }

  {
    // Retrieve the second annotation: regular combobox.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    // Check that the flag values are as expected.
    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_FALSE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_CHOICE_COMBO);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_CHOICE_EDIT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
  }

  {
    // Retrieve the third annotation: read-only combobox.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 2));
    ASSERT_TRUE(annot);

    // Check that the flag values are as expected.
    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_TRUE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_CHOICE_COMBO);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_CHOICE_EDIT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormAnnotNull) {
  // Open file with form text fields.
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // Attempt to get an annotation where no annotation exists on page.
  static const FS_POINTF kOriginPoint = {0.0f, 0.0f};
  EXPECT_FALSE(
      FPDFAnnot_GetFormFieldAtPoint(form_handle(), page.get(), &kOriginPoint));

  static const FS_POINTF kValidPoint = {120.0f, 120.0f};
  {
    // Verify there is an annotation.
    ScopedFPDFAnnotation annot(
        FPDFAnnot_GetFormFieldAtPoint(form_handle(), page.get(), &kValidPoint));
    EXPECT_TRUE(annot);
  }

  // Try other bad inputs at a valid location.
  EXPECT_FALSE(FPDFAnnot_GetFormFieldAtPoint(nullptr, nullptr, &kValidPoint));
  EXPECT_FALSE(
      FPDFAnnot_GetFormFieldAtPoint(nullptr, page.get(), &kValidPoint));
  EXPECT_FALSE(
      FPDFAnnot_GetFormFieldAtPoint(form_handle(), nullptr, &kValidPoint));
}

TEST_F(FPDFAnnotEmbedderTest, GetFormAnnotAndCheckFlagsTextField) {
  // Open file with form text fields.
  ASSERT_TRUE(OpenDocument("text_form_multiple.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Retrieve user-editable text field annotation.
    static const FS_POINTF kPoint = {105.0f, 118.0f};
    ScopedFPDFAnnotation annot(
        FPDFAnnot_GetFormFieldAtPoint(form_handle(), page.get(), &kPoint));
    ASSERT_TRUE(annot);

    // Check that interactive form annotation flag values are as expected.
    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_READONLY);
  }

  {
    // Retrieve read-only text field annotation.
    static const FS_POINTF kPoint = {105.0f, 202.0f};
    ScopedFPDFAnnotation annot(
        FPDFAnnot_GetFormFieldAtPoint(form_handle(), page.get(), &kPoint));
    ASSERT_TRUE(annot);

    // Check that interactive form annotation flag values are as expected.
    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_TRUE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormAnnotAndCheckFlagsComboBox) {
  // Open file with form comboboxes.
  ASSERT_TRUE(OpenDocument("combobox_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Retrieve user-editable combobox annotation.
    static const FS_POINTF kPoint = {102.0f, 363.0f};
    ScopedFPDFAnnotation annot(
        FPDFAnnot_GetFormFieldAtPoint(form_handle(), page.get(), &kPoint));
    ASSERT_TRUE(annot);

    // Check that interactive form annotation flag values are as expected.
    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_FALSE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_CHOICE_COMBO);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_CHOICE_EDIT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
  }

  {
    // Retrieve regular combobox annotation.
    static const FS_POINTF kPoint = {102.0f, 413.0f};
    ScopedFPDFAnnotation annot(
        FPDFAnnot_GetFormFieldAtPoint(form_handle(), page.get(), &kPoint));
    ASSERT_TRUE(annot);

    // Check that interactive form annotation flag values are as expected.
    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_FALSE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_CHOICE_COMBO);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_CHOICE_EDIT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
  }

  {
    // Retrieve read-only combobox annotation.
    static const FS_POINTF kPoint = {102.0f, 513.0f};
    ScopedFPDFAnnotation annot(
        FPDFAnnot_GetFormFieldAtPoint(form_handle(), page.get(), &kPoint));
    ASSERT_TRUE(annot);

    // Check that interactive form annotation flag values are as expected.
    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_TRUE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_CHOICE_COMBO);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_CHOICE_EDIT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
  }
}

TEST_F(FPDFAnnotEmbedderTest, Bug1206) {
  ASSERT_TRUE(OpenDocument("bug_1206.pdf"));

  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  const size_t original_size = GetString().size();
  ASSERT_GT(original_size, 0u);
  ClearString();

  for (size_t i = 0; i < 10; ++i) {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(bitmap.get(), "bug_1206");

    ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
    EXPECT_EQ(original_size, GetString().size());

    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    ScopedSavedPage saved_page = LoadScopedSavedPage(0);
    ASSERT_TRUE(saved_page);
    ScopedFPDFBitmap saved_bitmap =
        RenderSavedPageWithFlags(saved_page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(saved_bitmap.get(), "bug_1206");

    ClearString();
  }
}

TEST_F(FPDFAnnotEmbedderTest, Bug1212) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));

  static const char kTestKey[] = "test";
  static const wchar_t kData[] = L"\xf6\xe4";
  static const size_t kBufSize = 12;
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(kBufSize);

  {
    // Add a text annotation to the page.
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(annot);
    EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
    EXPECT_EQ(FPDF_ANNOT_TEXT, FPDFAnnot_GetSubtype(annot.get()));

    // Make sure there is no test key, add set a value there, and read it back.
    std::ranges::fill(buf, 'x');
    ASSERT_EQ(2u, FPDFAnnot_GetStringValue(annot.get(), kTestKey, buf.data(),
                                           kBufSize));
    EXPECT_EQ(L"", GetPlatformWString(buf.data()));

    ScopedFPDFWideString text = GetFPDFWideString(kData);
    EXPECT_TRUE(FPDFAnnot_SetStringValue(annot.get(), kTestKey, text.get()));

    std::ranges::fill(buf, 'x');
    ASSERT_EQ(6u, FPDFAnnot_GetStringValue(annot.get(), kTestKey, buf.data(),
                                           kBufSize));
    EXPECT_EQ(kData, GetPlatformWString(buf.data()));
  }

  {
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_STAMP));
    ASSERT_TRUE(annot);
    const FS_RECTF bounding_rect{206.0f, 753.0f, 339.0f, 709.0f};
    EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &bounding_rect));
    EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));
    EXPECT_EQ(FPDF_ANNOT_STAMP, FPDFAnnot_GetSubtype(annot.get()));
    // Also do the same test for its appearance string.
    std::ranges::fill(buf, 'x');
    ASSERT_EQ(2u,
              FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER,
                              buf.data(), kBufSize));
    EXPECT_EQ(L"", GetPlatformWString(buf.data()));

    ScopedFPDFWideString text = GetFPDFWideString(kData);
    EXPECT_TRUE(FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER,
                                text.get()));

    std::ranges::fill(buf, 'x');
    ASSERT_EQ(6u,
              FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER,
                              buf.data(), kBufSize));
    EXPECT_EQ(kData, GetPlatformWString(buf.data()));
  }

  // Save a copy, open the copy, and check the annotation again.
  // Note that it renders the rotation.
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  {
    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    ScopedSavedPage saved_page = LoadScopedSavedPage(0);
    ASSERT_TRUE(saved_page);

    EXPECT_EQ(2, FPDFPage_GetAnnotCount(saved_page.get()));
    {
      ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(saved_page.get(), 0));
      ASSERT_TRUE(annot);
      EXPECT_EQ(FPDF_ANNOT_TEXT, FPDFAnnot_GetSubtype(annot.get()));

      std::ranges::fill(buf, 'x');
      ASSERT_EQ(6u, FPDFAnnot_GetStringValue(annot.get(), kTestKey, buf.data(),
                                             kBufSize));
      EXPECT_EQ(kData, GetPlatformWString(buf.data()));
    }

    {
      ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(saved_page.get(), 0));
      ASSERT_TRUE(annot);
      // TODO(thestig): This return FPDF_ANNOT_UNKNOWN for some reason.
      // EXPECT_EQ(FPDF_ANNOT_TEXT, FPDFAnnot_GetSubtype(annot.get()));

      std::ranges::fill(buf, 'x');
      ASSERT_EQ(6u, FPDFAnnot_GetStringValue(annot.get(), kTestKey, buf.data(),
                                             kBufSize));
      EXPECT_EQ(kData, GetPlatformWString(buf.data()));
    }
  }
}

TEST_F(FPDFAnnotEmbedderTest, RemoveKey) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));

  static const char kStateKey[] = "State";
  static const char kStateModelKey[] = "StateModel";
  static const wchar_t kState[] = L"Accepted";
  static const wchar_t kStateModel[] = L"Review";
  static const size_t kBufSize = 32;
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(kBufSize);

  {
    // A review-status reply per ISO 32000-2 12.5.6.3: a text annotation
    // carrying /State + /StateModel.
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(annot);

    ScopedFPDFWideString state = GetFPDFWideString(kState);
    ScopedFPDFWideString model = GetFPDFWideString(kStateModel);
    EXPECT_TRUE(FPDFAnnot_SetStringValue(annot.get(), kStateKey, state.get()));
    EXPECT_TRUE(
        FPDFAnnot_SetStringValue(annot.get(), kStateModelKey, model.get()));

    std::ranges::fill(buf, 'x');
    ASSERT_EQ(18u, FPDFAnnot_GetStringValue(annot.get(), kStateKey, buf.data(),
                                            kBufSize));
    EXPECT_EQ(kState, GetPlatformWString(buf.data()));

    // Overwriting with an empty string is NOT removal: the entry stays.
    ScopedFPDFWideString empty = GetFPDFWideString(L"");
    EXPECT_TRUE(FPDFAnnot_SetStringValue(annot.get(), kStateKey, empty.get()));
    EXPECT_TRUE(FPDFAnnot_HasKey(annot.get(), kStateKey));

    // RemoveKey truly removes the entry.
    EXPECT_TRUE(EPDFAnnot_RemoveKey(annot.get(), kStateKey));
    EXPECT_FALSE(FPDFAnnot_HasKey(annot.get(), kStateKey));
    std::ranges::fill(buf, 'x');
    ASSERT_EQ(2u, FPDFAnnot_GetStringValue(annot.get(), kStateKey, buf.data(),
                                           kBufSize));
    EXPECT_EQ(L"", GetPlatformWString(buf.data()));

    // Idempotent on an absent key; false only on null inputs.
    EXPECT_TRUE(EPDFAnnot_RemoveKey(annot.get(), kStateKey));
    EXPECT_FALSE(EPDFAnnot_RemoveKey(nullptr, kStateKey));
    EXPECT_FALSE(EPDFAnnot_RemoveKey(annot.get(), nullptr));

    // /StateModel is untouched by the /State removal.
    EXPECT_TRUE(FPDFAnnot_HasKey(annot.get(), kStateModelKey));
  }

  // The removal survives a save/reopen round trip.
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  {
    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    ScopedSavedPage saved_page = LoadScopedSavedPage(0);
    ASSERT_TRUE(saved_page);

    EXPECT_EQ(1, FPDFPage_GetAnnotCount(saved_page.get()));
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(saved_page.get(), 0));
    ASSERT_TRUE(annot);

    EXPECT_FALSE(FPDFAnnot_HasKey(annot.get(), kStateKey));
    std::ranges::fill(buf, 'x');
    ASSERT_EQ(14u, FPDFAnnot_GetStringValue(annot.get(), kStateModelKey,
                                            buf.data(), kBufSize));
    EXPECT_EQ(kStateModel, GetPlatformWString(buf.data()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetOptionCountCombobox) {
  // Open a file with combobox widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("combobox_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    EXPECT_EQ(3, FPDFAnnot_GetOptionCount(form_handle(), annot.get()));

    annot.reset(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    EXPECT_EQ(26, FPDFAnnot_GetOptionCount(form_handle(), annot.get()));

    // Check bad form handle / annot.
    EXPECT_EQ(-1, FPDFAnnot_GetOptionCount(nullptr, nullptr));
    EXPECT_EQ(-1, FPDFAnnot_GetOptionCount(form_handle(), nullptr));
    EXPECT_EQ(-1, FPDFAnnot_GetOptionCount(nullptr, annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetOptionCountListbox) {
  // Open a file with listbox widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    EXPECT_EQ(3, FPDFAnnot_GetOptionCount(form_handle(), annot.get()));

    annot.reset(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    EXPECT_EQ(26, FPDFAnnot_GetOptionCount(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetOptionCountInvalidAnnotations) {
  // Open a file with ink annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_ink_multiple.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // annotations do not have "Opt" array and will return -1
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    EXPECT_EQ(-1, FPDFAnnot_GetOptionCount(form_handle(), annot.get()));

    annot.reset(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    EXPECT_EQ(-1, FPDFAnnot_GetOptionCount(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetOptionCountWrongAnnotationType) {
  // Open a file with annotations and load its first page.
  ASSERT_TRUE(OpenDocument("multiple_form_types.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // This annotation is a form field, but does not support Options.
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 3));
  ASSERT_TRUE(annot);

  // Check types.
  EXPECT_EQ(FPDF_ANNOT_WIDGET, FPDFAnnot_GetSubtype(annot.get()));
  EXPECT_EQ(FPDF_FORMFIELD_TEXTFIELD,
            FPDFAnnot_GetFormFieldType(form_handle(), annot.get()));

  // Option APIs do not support this annot type. They gracefully return an error
  // and do not crash.
  EXPECT_EQ(-1, FPDFAnnot_GetOptionCount(form_handle(), annot.get()));
  EXPECT_EQ(
      0u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), 0, nullptr, 0));
}

TEST_F(FPDFAnnotEmbedderTest, GetOptionLabelCombobox) {
  // Open a file with combobox widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("combobox_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    int index = 0;
    unsigned long length_bytes =
        FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), index, nullptr, 0);
    ASSERT_EQ(8u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(8u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), index,
                                           buf.data(), length_bytes));
    EXPECT_EQ(L"Foo", GetPlatformWString(buf.data()));

    annot.reset(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    index = 0;
    length_bytes =
        FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), index, nullptr, 0);
    ASSERT_EQ(12u, length_bytes);
    buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(12u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), index,
                                            buf.data(), length_bytes));
    EXPECT_EQ(L"Apple", GetPlatformWString(buf.data()));

    index = 25;
    length_bytes =
        FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), index, nullptr, 0);
    buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(18u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), index,
                                            buf.data(), length_bytes));
    EXPECT_EQ(L"Zucchini", GetPlatformWString(buf.data()));

    // Indices out of range
    EXPECT_EQ(0u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), -1,
                                           nullptr, 0));
    EXPECT_EQ(0u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), 26,
                                           nullptr, 0));

    // Check bad form handle / annot.
    EXPECT_EQ(0u, FPDFAnnot_GetOptionLabel(nullptr, nullptr, 0, nullptr, 0));
    EXPECT_EQ(0u,
              FPDFAnnot_GetOptionLabel(nullptr, annot.get(), 0, nullptr, 0));
    EXPECT_EQ(0u,
              FPDFAnnot_GetOptionLabel(form_handle(), nullptr, 0, nullptr, 0));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetOptionLabelListbox) {
  // Open a file with listbox widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    int index = 0;
    unsigned long length_bytes =
        FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), index, nullptr, 0);
    ASSERT_EQ(8u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(8u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), index,
                                           buf.data(), length_bytes));
    EXPECT_EQ(L"Foo", GetPlatformWString(buf.data()));

    annot.reset(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    index = 0;
    length_bytes =
        FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), index, nullptr, 0);
    ASSERT_EQ(12u, length_bytes);
    buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(12u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), index,
                                            buf.data(), length_bytes));
    EXPECT_EQ(L"Apple", GetPlatformWString(buf.data()));

    index = 25;
    length_bytes =
        FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), index, nullptr, 0);
    ASSERT_EQ(18u, length_bytes);
    buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(18u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), index,
                                            buf.data(), length_bytes));
    EXPECT_EQ(L"Zucchini", GetPlatformWString(buf.data()));

    // indices out of range
    EXPECT_EQ(0u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), -1,
                                           nullptr, 0));
    EXPECT_EQ(0u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), 26,
                                           nullptr, 0));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetOptionLabelInvalidAnnotations) {
  // Open a file with ink annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_ink_multiple.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // annotations do not have "Opt" array and will return 0
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    EXPECT_EQ(0u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), 0,
                                           nullptr, 0));

    annot.reset(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    EXPECT_EQ(0u, FPDFAnnot_GetOptionLabel(form_handle(), annot.get(), 0,
                                           nullptr, 0));
  }
}

TEST_F(FPDFAnnotEmbedderTest, IsOptionSelectedCombobox) {
  // Open a file with combobox widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("combobox_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Checks for Combobox with no Values (/V) or Selected Indices (/I) objects.
    int count = FPDFAnnot_GetOptionCount(form_handle(), annot.get());
    ASSERT_EQ(3, count);
    for (int i = 0; i < count; i++) {
      EXPECT_FALSE(FPDFAnnot_IsOptionSelected(form_handle(), annot.get(), i));
    }

    annot.reset(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    // Checks for Combobox with Values (/V) object which is just a string.
    count = FPDFAnnot_GetOptionCount(form_handle(), annot.get());
    ASSERT_EQ(26, count);
    for (int i = 0; i < count; i++) {
      EXPECT_EQ(i == 1,
                FPDFAnnot_IsOptionSelected(form_handle(), annot.get(), i));
    }

    // Checks for index outside bound i.e. (index >= CountOption()).
    EXPECT_FALSE(FPDFAnnot_IsOptionSelected(form_handle(), annot.get(),
                                            /*index=*/26));
    // Checks for negetive index.
    EXPECT_FALSE(FPDFAnnot_IsOptionSelected(form_handle(), annot.get(),
                                            /*index=*/-1));

    // Checks for bad form handle/annot.
    EXPECT_FALSE(FPDFAnnot_IsOptionSelected(nullptr, nullptr, /*index=*/0));
    EXPECT_FALSE(
        FPDFAnnot_IsOptionSelected(form_handle(), nullptr, /*index=*/0));
    EXPECT_FALSE(FPDFAnnot_IsOptionSelected(nullptr, annot.get(), /*index=*/0));
  }
}

TEST_F(FPDFAnnotEmbedderTest, IsOptionSelectedListbox) {
  // Open a file with listbox widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Checks for Listbox with no Values (/V) or Selected Indices (/I) objects.
    int count = FPDFAnnot_GetOptionCount(form_handle(), annot.get());
    ASSERT_EQ(3, count);
    for (int i = 0; i < count; i++) {
      EXPECT_FALSE(FPDFAnnot_IsOptionSelected(form_handle(), annot.get(), i));
    }

    annot.reset(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    // Checks for Listbox with Values (/V) object which is just a string.
    count = FPDFAnnot_GetOptionCount(form_handle(), annot.get());
    ASSERT_EQ(26, count);
    for (int i = 0; i < count; i++) {
      EXPECT_EQ(i == 1,
                FPDFAnnot_IsOptionSelected(form_handle(), annot.get(), i));
    }

    annot.reset(FPDFPage_GetAnnot(page.get(), 3));
    ASSERT_TRUE(annot);

    // Checks for Listbox with only Selected indices (/I) object which is an
    // array with multiple objects.
    count = FPDFAnnot_GetOptionCount(form_handle(), annot.get());
    ASSERT_EQ(5, count);
    for (int i = 0; i < count; i++) {
      bool expected = (i == 1 || i == 3);
      EXPECT_EQ(expected,
                FPDFAnnot_IsOptionSelected(form_handle(), annot.get(), i));
    }

    annot.reset(FPDFPage_GetAnnot(page.get(), 4));
    ASSERT_TRUE(annot);

    // Checks for Listbox with Values (/V) object which is an array with
    // multiple objects.
    count = FPDFAnnot_GetOptionCount(form_handle(), annot.get());
    ASSERT_EQ(5, count);
    for (int i = 0; i < count; i++) {
      bool expected = (i == 2 || i == 4);
      EXPECT_EQ(expected,
                FPDFAnnot_IsOptionSelected(form_handle(), annot.get(), i));
    }

    annot.reset(FPDFPage_GetAnnot(page.get(), 5));
    ASSERT_TRUE(annot);

    // Checks for Listbox with both Values (/V) and Selected Indices (/I)
    // objects conflict with different lengths.
    count = FPDFAnnot_GetOptionCount(form_handle(), annot.get());
    ASSERT_EQ(5, count);
    for (int i = 0; i < count; i++) {
      bool expected = (i == 0 || i == 2);
      EXPECT_EQ(expected,
                FPDFAnnot_IsOptionSelected(form_handle(), annot.get(), i));
    }
  }
}

TEST_F(FPDFAnnotEmbedderTest, IsOptionSelectedInvalidAnnotations) {
  // Open a file with multiple form field annotations and load its first page.
  ASSERT_TRUE(OpenDocument("multiple_form_types.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Checks for link annotation.
    EXPECT_FALSE(FPDFAnnot_IsOptionSelected(form_handle(), annot.get(),
                                            /*index=*/0));

    annot.reset(FPDFPage_GetAnnot(page.get(), 3));
    ASSERT_TRUE(annot);

    // Checks for text field annotation.
    EXPECT_FALSE(FPDFAnnot_IsOptionSelected(form_handle(), annot.get(),
                                            /*index=*/0));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFontSizeCombobox) {
  // Open a file with combobox annotations and load its first page.
  ASSERT_TRUE(OpenDocument("combobox_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // All 3 widgets have Tf font size 12.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    float value;
    ASSERT_TRUE(FPDFAnnot_GetFontSize(form_handle(), annot.get(), &value));
    EXPECT_EQ(12.0, value);

    annot.reset(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    float value_two;
    ASSERT_TRUE(FPDFAnnot_GetFontSize(form_handle(), annot.get(), &value_two));
    EXPECT_EQ(12.0, value_two);

    annot.reset(FPDFPage_GetAnnot(page.get(), 2));
    ASSERT_TRUE(annot);

    float value_three;
    ASSERT_TRUE(
        FPDFAnnot_GetFontSize(form_handle(), annot.get(), &value_three));
    EXPECT_EQ(12.0, value_three);
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFontSizeTextField) {
  // Open a file with textfield annotations and load its first page.
  ASSERT_TRUE(OpenDocument("text_form_multiple.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // All 4 widgets have Tf font size 12.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    float value;
    ASSERT_TRUE(FPDFAnnot_GetFontSize(form_handle(), annot.get(), &value));
    EXPECT_EQ(12.0, value);

    annot.reset(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    float value_two;
    ASSERT_TRUE(FPDFAnnot_GetFontSize(form_handle(), annot.get(), &value_two));
    EXPECT_EQ(12.0, value_two);

    annot.reset(FPDFPage_GetAnnot(page.get(), 2));
    ASSERT_TRUE(annot);

    float value_three;
    ASSERT_TRUE(
        FPDFAnnot_GetFontSize(form_handle(), annot.get(), &value_three));
    EXPECT_EQ(12.0, value_three);

    float value_four;
    ASSERT_TRUE(FPDFAnnot_GetFontSize(form_handle(), annot.get(), &value_four));
    EXPECT_EQ(12.0, value_four);
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFontSizeInvalidAnnotationTypes) {
  // Open a file with ink annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_ink_multiple.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Annotations that do not have variable text and will return -1.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    float value;
    ASSERT_FALSE(FPDFAnnot_GetFontSize(form_handle(), annot.get(), &value));

    annot.reset(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    ASSERT_FALSE(FPDFAnnot_GetFontSize(form_handle(), annot.get(), &value));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFontSizeInvalidArguments) {
  // Open a file with combobox annotations and load its first page.
  ASSERT_TRUE(OpenDocument("combobox_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Check bad form handle / annot.
    float value;
    ASSERT_FALSE(FPDFAnnot_GetFontSize(nullptr, annot.get(), &value));
    ASSERT_FALSE(FPDFAnnot_GetFontSize(form_handle(), nullptr, &value));
    ASSERT_FALSE(FPDFAnnot_GetFontSize(nullptr, nullptr, &value));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFontSizeNegative) {
  // Open a file with textfield annotations and load its first page.
  ASSERT_TRUE(OpenDocument("text_form_negative_fontsize.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Obtain the first annotation, a text field with negative font size, -12.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    float value;
    ASSERT_TRUE(FPDFAnnot_GetFontSize(form_handle(), annot.get(), &value));
    EXPECT_EQ(-12.0, value);
  }
}

TEST_F(FPDFAnnotEmbedderTest,
       DirectAnnotationObjectNumberStaysZeroAfterRender) {
  ASSERT_TRUE(OpenDocument("freetext_annotation_without_da.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(1, FPDFPage_GetAnnotCount(page.get()));

  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(annot);
  EXPECT_EQ(0u, EPDFAnnot_GetObjectNumber(annot.get()));
  EXPECT_FALSE(EPDFPage_GetAnnotByObjectNumber(page.get(), 0));

  ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
  ASSERT_TRUE(bitmap);
  EXPECT_EQ(0u, EPDFAnnot_GetObjectNumber(annot.get()));

  ASSERT_TRUE(
      EPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, 10, 20, 30));
  EXPECT_EQ(0u, EPDFAnnot_GetObjectNumber(annot.get()));
}

TEST_F(FPDFAnnotEmbedderTest, SetFontColor) {
  ASSERT_TRUE(OpenDocument("freetext_annotation_without_da.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  static constexpr char kFreetextAnnotationWithoutDaModifiedPng[] =
      "freetext_annotation_without_da_modified";
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(bitmap.get(),
                                       "freetext_annotation_without_da");

    // Obtain the only annotation and set its text color.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    unsigned int r = 1;
    unsigned int g = 2;
    unsigned int b = 3;
    ASSERT_TRUE(FPDFAnnot_GetFontColor(form_handle(), annot.get(), &r, &g, &b));
    EXPECT_EQ(0u, r);
    EXPECT_EQ(0u, g);
    EXPECT_EQ(0u, b);

    ASSERT_TRUE(
        FPDFAnnot_SetFontColor(form_handle(), annot.get(), 60, 120, 180));

    ASSERT_TRUE(FPDFAnnot_GetFontColor(form_handle(), annot.get(), &r, &g, &b));
    EXPECT_EQ(60u, r);
    EXPECT_EQ(120u, g);
    EXPECT_EQ(180u, b);

    bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(bitmap.get(),
                                       kFreetextAnnotationWithoutDaModifiedPng);
  }

  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_doc);
  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);
  VerifySavedRenderingWithExpectationSuffix(
      saved_page.get(), kFreetextAnnotationWithoutDaModifiedPng);
}

TEST_F(FPDFAnnotEmbedderTest, SetFontColorNegative) {
  ASSERT_TRUE(OpenDocument("text_form_color.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Obtain the first annotation, a text field with orange color.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Negative testing with invalid parameters.
    ASSERT_FALSE(FPDFAnnot_SetFontColor(nullptr, nullptr, 256, 256, 256));
    ASSERT_FALSE(FPDFAnnot_SetFontColor(form_handle(), nullptr, 0, 0, 0));
    ASSERT_FALSE(FPDFAnnot_SetFontColor(nullptr, annot.get(), 0, 0, 0));
    ASSERT_FALSE(FPDFAnnot_SetFontColor(nullptr, nullptr, 256, 0, 0));
    ASSERT_FALSE(FPDFAnnot_SetFontColor(nullptr, nullptr, 0, 256, 0));
    ASSERT_FALSE(FPDFAnnot_SetFontColor(nullptr, nullptr, 0, 0, 256));

    // The text field widget in the PDF is not supported yet.
    // TODO(thestig): Move out of this test case and make sure this succeeds
    // after adding support.
    ASSERT_FALSE(
        FPDFAnnot_SetFontColor(form_handle(), annot.get(), 60, 120, 180));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFontColor) {
  // Open a file with textfield annotations and load its first page.
  ASSERT_TRUE(OpenDocument("text_form_color.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Obtain the first annotation, a text field with orange color.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // Negative testing.
    unsigned int R;
    unsigned int G;
    unsigned int B;
    ASSERT_FALSE(
        FPDFAnnot_GetFontColor(nullptr, nullptr, nullptr, nullptr, nullptr));
    ASSERT_FALSE(FPDFAnnot_GetFontColor(form_handle(), nullptr, nullptr,
                                        nullptr, nullptr));
    ASSERT_FALSE(FPDFAnnot_GetFontColor(form_handle(), annot.get(), nullptr,
                                        nullptr, nullptr));
    ASSERT_FALSE(FPDFAnnot_GetFontColor(form_handle(), annot.get(), &R, nullptr,
                                        nullptr));
    ASSERT_FALSE(
        FPDFAnnot_GetFontColor(form_handle(), annot.get(), &R, &G, nullptr));

    // Positive testing.
    ASSERT_TRUE(FPDFAnnot_GetFontColor(form_handle(), annot.get(), &R, &G, &B));
    // Make sure it's #ff8000, i.e. orange.
    EXPECT_EQ(0xffU, R);
    EXPECT_EQ(0x80U, G);
    EXPECT_EQ(0x00U, B);
  }
}

TEST_F(FPDFAnnotEmbedderTest, IsCheckedCheckbox) {
  // Open a file with checkbox and radiobuttons widget annotations and load its
  // first page.
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);
    ASSERT_FALSE(FPDFAnnot_IsChecked(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, IsCheckedCheckboxReadOnly) {
  // Open a file with checkbox and radiobutton widget annotations and load its
  // first page.
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    ASSERT_TRUE(FPDFAnnot_IsChecked(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, IsCheckedRadioButton) {
  // Open a file with checkbox and radiobutton widget annotations and load its
  // first page.
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 5));
    ASSERT_TRUE(annot);
    ASSERT_FALSE(FPDFAnnot_IsChecked(form_handle(), annot.get()));

    annot.reset(FPDFPage_GetAnnot(page.get(), 6));
    ASSERT_FALSE(FPDFAnnot_IsChecked(form_handle(), annot.get()));

    annot.reset(FPDFPage_GetAnnot(page.get(), 7));
    ASSERT_TRUE(FPDFAnnot_IsChecked(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, IsCheckedRadioButtonReadOnly) {
  // Open a file with checkbox and radiobutton widget annotations and load its
  // first page.
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 2));
    ASSERT_TRUE(annot);
    ASSERT_FALSE(FPDFAnnot_IsChecked(form_handle(), annot.get()));

    annot.reset(FPDFPage_GetAnnot(page.get(), 3));
    ASSERT_FALSE(FPDFAnnot_IsChecked(form_handle(), annot.get()));

    annot.reset(FPDFPage_GetAnnot(page.get(), 4));
    ASSERT_TRUE(FPDFAnnot_IsChecked(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, IsCheckedInvalidArguments) {
  // Open a file with checkbox and radiobuttons widget annotations and load its
  // first page.
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    ASSERT_FALSE(FPDFAnnot_IsChecked(nullptr, annot.get()));
    ASSERT_FALSE(FPDFAnnot_IsChecked(form_handle(), nullptr));
    ASSERT_FALSE(FPDFAnnot_IsChecked(nullptr, nullptr));
  }
}

TEST_F(FPDFAnnotEmbedderTest, IsCheckedInvalidWidgetType) {
  // Open a file with text widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    ASSERT_FALSE(FPDFAnnot_IsChecked(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormFieldType) {
  ASSERT_TRUE(OpenDocument("multiple_form_types.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  EXPECT_EQ(-1, FPDFAnnot_GetFormFieldType(form_handle(), nullptr));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);
    EXPECT_EQ(-1, FPDFAnnot_GetFormFieldType(nullptr, annot.get()));
  }

  static const struct {
    int input;
    int output;
  } kTests[] = {{0, -1},
                {1, FPDF_FORMFIELD_COMBOBOX},
                {2, FPDF_FORMFIELD_LISTBOX},
                {3, FPDF_FORMFIELD_TEXTFIELD},
                {4, FPDF_FORMFIELD_CHECKBOX},
                {5, FPDF_FORMFIELD_RADIOBUTTON}};
  for (const auto& test : kTests) {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), test.input));
    ASSERT_TRUE(annot);
    EXPECT_EQ(test.output,
              FPDFAnnot_GetFormFieldType(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormFieldValueTextField) {
  ASSERT_TRUE(OpenDocument("text_form_multiple.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    EXPECT_EQ(0u,
              FPDFAnnot_GetFormFieldValue(form_handle(), nullptr, nullptr, 0));

    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    EXPECT_EQ(0u,
              FPDFAnnot_GetFormFieldValue(nullptr, annot.get(), nullptr, 0));

    unsigned long length_bytes =
        FPDFAnnot_GetFormFieldValue(form_handle(), annot.get(), nullptr, 0);
    ASSERT_EQ(2u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(2u, FPDFAnnot_GetFormFieldValue(form_handle(), annot.get(),
                                              buf.data(), length_bytes));
    EXPECT_EQ(L"", GetPlatformWString(buf.data()));
  }
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 2));
    ASSERT_TRUE(annot);

    unsigned long length_bytes =
        FPDFAnnot_GetFormFieldValue(form_handle(), annot.get(), nullptr, 0);
    ASSERT_EQ(18u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(18u, FPDFAnnot_GetFormFieldValue(form_handle(), annot.get(),
                                               buf.data(), length_bytes));
    EXPECT_EQ(L"Elephant", GetPlatformWString(buf.data()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormFieldValueComboBox) {
  ASSERT_TRUE(OpenDocument("combobox_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    unsigned long length_bytes =
        FPDFAnnot_GetFormFieldValue(form_handle(), annot.get(), nullptr, 0);
    ASSERT_EQ(2u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(2u, FPDFAnnot_GetFormFieldValue(form_handle(), annot.get(),
                                              buf.data(), length_bytes));
    EXPECT_EQ(L"", GetPlatformWString(buf.data()));
  }
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    unsigned long length_bytes =
        FPDFAnnot_GetFormFieldValue(form_handle(), annot.get(), nullptr, 0);
    ASSERT_EQ(14u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(14u, FPDFAnnot_GetFormFieldValue(form_handle(), annot.get(),
                                               buf.data(), length_bytes));
    EXPECT_EQ(L"Banana", GetPlatformWString(buf.data()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormFieldNameTextField) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    EXPECT_EQ(0u,
              FPDFAnnot_GetFormFieldName(form_handle(), nullptr, nullptr, 0));

    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    EXPECT_EQ(0u, FPDFAnnot_GetFormFieldName(nullptr, annot.get(), nullptr, 0));

    unsigned long length_bytes =
        FPDFAnnot_GetFormFieldName(form_handle(), annot.get(), nullptr, 0);
    ASSERT_EQ(18u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(18u, FPDFAnnot_GetFormFieldName(form_handle(), annot.get(),
                                              buf.data(), length_bytes));
    EXPECT_EQ(L"Text Box", GetPlatformWString(buf.data()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormFieldNameComboBox) {
  ASSERT_TRUE(OpenDocument("combobox_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    unsigned long length_bytes =
        FPDFAnnot_GetFormFieldName(form_handle(), annot.get(), nullptr, 0);
    ASSERT_EQ(30u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(30u, FPDFAnnot_GetFormFieldName(form_handle(), annot.get(),
                                              buf.data(), length_bytes));
    EXPECT_EQ(L"Combo_Editable", GetPlatformWString(buf.data()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, FocusableAnnotSubtypes) {
  ASSERT_TRUE(OpenDocument("annots.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // Verify widgets are by default focusable.
  const FPDF_ANNOTATION_SUBTYPE kDefaultSubtypes[] = {FPDF_ANNOT_WIDGET};
  VerifyFocusableAnnotSubtypes(form_handle(), kDefaultSubtypes);

  // Expected annot subtypes for page 0 of annots.pdf.
  const FPDF_ANNOTATION_SUBTYPE kExpectedAnnotSubtypes[] = {
      FPDF_ANNOT_LINK,  FPDF_ANNOT_LINK,      FPDF_ANNOT_LINK,
      FPDF_ANNOT_LINK,  FPDF_ANNOT_HIGHLIGHT, FPDF_ANNOT_HIGHLIGHT,
      FPDF_ANNOT_POPUP, FPDF_ANNOT_HIGHLIGHT, FPDF_ANNOT_WIDGET,
  };

  const FPDF_ANNOTATION_SUBTYPE kExpectedDefaultFocusableSubtypes[] = {
      FPDF_ANNOT_WIDGET};
  VerifyAnnotationSubtypesAndFocusability(form_handle(), page.get(),
                                          kExpectedAnnotSubtypes,
                                          kExpectedDefaultFocusableSubtypes);

  // Make no annotation type focusable using the preferred method.
  ASSERT_TRUE(FPDFAnnot_SetFocusableSubtypes(form_handle(), nullptr, 0));
  ASSERT_EQ(0, FPDFAnnot_GetFocusableSubtypesCount(form_handle()));

  // Restore the focusable type count back to 1, then set it back to 0 using a
  // different method.
  SetAndVerifyFocusableAnnotSubtypes(form_handle(), kDefaultSubtypes);
  ASSERT_TRUE(
      FPDFAnnot_SetFocusableSubtypes(form_handle(), kDefaultSubtypes, 0));
  ASSERT_EQ(0, FPDFAnnot_GetFocusableSubtypesCount(form_handle()));

  VerifyAnnotationSubtypesAndFocusability(form_handle(), page.get(),
                                          kExpectedAnnotSubtypes, {});

  // Now make links focusable.
  const FPDF_ANNOTATION_SUBTYPE kLinkSubtypes[] = {FPDF_ANNOT_LINK};
  SetAndVerifyFocusableAnnotSubtypes(form_handle(), kLinkSubtypes);

  const FPDF_ANNOTATION_SUBTYPE kExpectedLinkocusableSubtypes[] = {
      FPDF_ANNOT_LINK};
  VerifyAnnotationSubtypesAndFocusability(form_handle(), page.get(),
                                          kExpectedAnnotSubtypes,
                                          kExpectedLinkocusableSubtypes);

  // Test invalid parameters.
  EXPECT_FALSE(FPDFAnnot_SetFocusableSubtypes(nullptr, kDefaultSubtypes,
                                              std::size(kDefaultSubtypes)));
  EXPECT_FALSE(FPDFAnnot_SetFocusableSubtypes(form_handle(), nullptr,
                                              std::size(kDefaultSubtypes)));
  EXPECT_EQ(-1, FPDFAnnot_GetFocusableSubtypesCount(nullptr));

  std::vector<FPDF_ANNOTATION_SUBTYPE> subtypes(1);
  EXPECT_FALSE(FPDFAnnot_GetFocusableSubtypes(nullptr, subtypes.data(),
                                              subtypes.size()));
  EXPECT_FALSE(
      FPDFAnnot_GetFocusableSubtypes(form_handle(), nullptr, subtypes.size()));
  EXPECT_FALSE(
      FPDFAnnot_GetFocusableSubtypes(form_handle(), subtypes.data(), 0));
}

TEST_F(FPDFAnnotEmbedderTest, FocusableAnnotRendering) {
  ASSERT_TRUE(OpenDocument("annots.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Check the initial rendering.
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(bitmap.get(), "annots_page1");
  }

  // Make links and highlights focusable.
  static constexpr FPDF_ANNOTATION_SUBTYPE kSubTypes[] = {FPDF_ANNOT_LINK,
                                                          FPDF_ANNOT_HIGHLIGHT};
  static constexpr int kSubTypesCount = std::size(kSubTypes);
  ASSERT_TRUE(
      FPDFAnnot_SetFocusableSubtypes(form_handle(), kSubTypes, kSubTypesCount));
  ASSERT_EQ(kSubTypesCount, FPDFAnnot_GetFocusableSubtypesCount(form_handle()));
  std::vector<FPDF_ANNOTATION_SUBTYPE> subtypes(kSubTypesCount);
  ASSERT_TRUE(FPDFAnnot_GetFocusableSubtypes(form_handle(), subtypes.data(),
                                             subtypes.size()));
  ASSERT_EQ(FPDF_ANNOT_LINK, subtypes[0]);
  ASSERT_EQ(FPDF_ANNOT_HIGHLIGHT, subtypes[1]);

  {
    // Focus the first link and check the rendering.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_LINK, FPDFAnnot_GetSubtype(annot.get()));
    EXPECT_TRUE(FORM_SetFocusedAnnot(form_handle(), annot.get()));
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(bitmap.get(), "annots_page1_focus_link");
  }

  {
    // Focus the first highlight and check the rendering.
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 4));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_HIGHLIGHT, FPDFAnnot_GetSubtype(annot.get()));
    EXPECT_TRUE(FORM_SetFocusedAnnot(form_handle(), annot.get()));
    ScopedFPDFBitmap bitmap = RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
    CompareBitmapWithExpectationSuffix(bitmap.get(),
                                       "annots_page1_focus_highlight");
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetLinkFromAnnotation) {
  ASSERT_TRUE(OpenDocument("annots.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  {
    static constexpr char kExpectedResult[] =
        "https://cs.chromium.org/chromium/src/third_party/pdfium/public/"
        "fpdf_text.h";

    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 3));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_LINK, FPDFAnnot_GetSubtype(annot.get()));
    VerifyUriActionInLink(document(), FPDFAnnot_GetLink(annot.get()),
                          kExpectedResult);
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 4));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_HIGHLIGHT, FPDFAnnot_GetSubtype(annot.get()));
    EXPECT_FALSE(FPDFAnnot_GetLink(annot.get()));
  }

  EXPECT_FALSE(FPDFAnnot_GetLink(nullptr));
}

TEST_F(FPDFAnnotEmbedderTest, GetFormControlCountRadioButton) {
  // Open a file with radio button widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Checks for bad annot.
    EXPECT_EQ(-1,
              FPDFAnnot_GetFormControlCount(form_handle(), /*annot=*/nullptr));

    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 3));
    ASSERT_TRUE(annot);

    // Checks for bad form handle.
    EXPECT_EQ(-1,
              FPDFAnnot_GetFormControlCount(/*hHandle=*/nullptr, annot.get()));

    EXPECT_EQ(3, FPDFAnnot_GetFormControlCount(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormControlCountCheckBox) {
  // Open a file with checkbox widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(1, FPDFAnnot_GetFormControlCount(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormControlCountInvalidAnnotation) {
  // Open a file with ink annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_ink_multiple.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(-1, FPDFAnnot_GetFormControlCount(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormControlIndexRadioButton) {
  // Open a file with radio button widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Checks for bad annot.
    EXPECT_EQ(-1,
              FPDFAnnot_GetFormControlIndex(form_handle(), /*annot=*/nullptr));

    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 3));
    ASSERT_TRUE(annot);

    // Checks for bad form handle.
    EXPECT_EQ(-1,
              FPDFAnnot_GetFormControlIndex(/*hHandle=*/nullptr, annot.get()));

    EXPECT_EQ(1, FPDFAnnot_GetFormControlIndex(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormControlIndexCheckBox) {
  // Open a file with checkbox widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(0, FPDFAnnot_GetFormControlIndex(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormControlIndexInvalidAnnotation) {
  // Open a file with ink annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_ink_multiple.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(-1, FPDFAnnot_GetFormControlIndex(form_handle(), annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormFieldExportValueRadioButton) {
  // Open a file with radio button widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    // Checks for bad annot.
    EXPECT_EQ(0u, FPDFAnnot_GetFormFieldExportValue(
                      form_handle(), /*annot=*/nullptr,
                      /*buffer=*/nullptr, /*buflen=*/0));

    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 6));
    ASSERT_TRUE(annot);

    // Checks for bad form handle.
    EXPECT_EQ(0u, FPDFAnnot_GetFormFieldExportValue(
                      /*hHandle=*/nullptr, annot.get(),
                      /*buffer=*/nullptr, /*buflen=*/0));

    unsigned long length_bytes =
        FPDFAnnot_GetFormFieldExportValue(form_handle(), annot.get(),
                                          /*buffer=*/nullptr, /*buflen=*/0);
    ASSERT_EQ(14u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(14u, FPDFAnnot_GetFormFieldExportValue(form_handle(), annot.get(),
                                                     buf.data(), length_bytes));
    EXPECT_EQ(L"value2", GetPlatformWString(buf.data()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormFieldExportValueCheckBox) {
  // Open a file with checkbox widget annotations and load its first page.
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    unsigned long length_bytes =
        FPDFAnnot_GetFormFieldExportValue(form_handle(), annot.get(),
                                          /*buffer=*/nullptr, /*buflen=*/0);
    ASSERT_EQ(8u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(8u, FPDFAnnot_GetFormFieldExportValue(form_handle(), annot.get(),
                                                    buf.data(), length_bytes));
    EXPECT_EQ(L"Yes", GetPlatformWString(buf.data()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetFormFieldExportValueInvalidAnnotation) {
  // Open a file with ink annotations and load its first page.
  ASSERT_TRUE(OpenDocument("annotation_ink_multiple.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(0u, FPDFAnnot_GetFormFieldExportValue(form_handle(), annot.get(),
                                                    /*buffer=*/nullptr,
                                                    /*buflen=*/0));
  }
}

TEST_F(FPDFAnnotEmbedderTest, Redactannotation) {
  ASSERT_TRUE(OpenDocument("redact_annot.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_REDACT, FPDFAnnot_GetSubtype(annot.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionRemovesTextInMiddleOfSentence) {
  ASSERT_TRUE(OpenDocument("redact_text_middle.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  std::wstring before = ExtractPageText(page.get());
  EXPECT_NE(std::wstring::npos, before.find(L"hello"));
  EXPECT_NE(std::wstring::npos, before.find(L"secret"));
  EXPECT_NE(std::wstring::npos, before.find(L"world"));
  ScopedFPDFBitmap before_bitmap = RenderLoadedPage(page.get());
  ASSERT_TRUE(before_bitmap);
  const std::string before_hash = HashBitmap(before_bitmap.get());

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    ASSERT_TRUE(EPDFAnnot_ApplyRedaction(page.get(), annot.get(), nullptr));
  }

  ASSERT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  ASSERT_TRUE(OpenSavedDocument());
  FPDF_PAGE saved_page = LoadSavedPage(0);
  ASSERT_TRUE(saved_page);

  std::wstring after = ExtractPageText(saved_page);
  ScopedFPDFBitmap after_bitmap = RenderSavedPage(saved_page);
  ASSERT_TRUE(after_bitmap);
  const std::string after_hash = HashBitmap(after_bitmap.get());
  CloseSavedPage(saved_page);
  EXPECT_NE(std::wstring::npos, after.find(L"hello"));
  EXPECT_EQ(std::wstring::npos, after.find(L"secret"));
  EXPECT_NE(std::wstring::npos, after.find(L"world"));
  EXPECT_NE(before_hash, after_hash);
}

TEST_F(FPDFAnnotEmbedderTest,
       ApplyRedactionSplitsFillAndStrokePathAcrossSave) {
  ASSERT_TRUE(OpenDocument("about_blank.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(612.0f, FPDF_GetPageWidthF(page.get()));
  ASSERT_EQ(792.0f, FPDF_GetPageHeightF(page.get()));
  const int original_object_count = FPDFPage_CountObjects(page.get());

  ScopedFPDFPageObject path(
      FPDFPageObj_CreateNewRect(100.0f, 300.0f, 400.0f, 200.0f));
  ASSERT_TRUE(path);
  ASSERT_TRUE(FPDFPageObj_SetFillColor(path.get(), 0, 0, 255, 255));
  ASSERT_TRUE(FPDFPageObj_SetStrokeColor(path.get(), 255, 0, 0, 255));
  ASSERT_TRUE(FPDFPageObj_SetStrokeWidth(path.get(), 20.0f));
  ASSERT_TRUE(FPDFPath_SetDrawMode(path.get(), FPDF_FILLMODE_WINDING,
                                   /*stroke=*/true));
  FPDFPage_InsertObject(page.get(), path.release());
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  ASSERT_EQ(original_object_count + 1, FPDFPage_CountObjects(page.get()));

  {
    ScopedFPDFBitmap bitmap = RenderPageOnWhite(page.get(), 612, 792);
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 150, 392));
    EXPECT_EQ(0xFFFF0000u, GetPixelColor(bitmap.get(), 150, 287));
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 300, 392));
  }

  {
    ScopedFPDFAnnotation annot =
        CreateRedactAnnot(page.get(), {280.0f, 550.0f, 332.0f, 250.0f});
    ASSERT_TRUE(annot);
    ASSERT_TRUE(EPDFAnnot_ApplyRedaction(page.get(), annot.get(), nullptr));
  }
  ASSERT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
  ASSERT_EQ(original_object_count + 2, FPDFPage_CountObjects(page.get()));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  {
    ScopedFPDFBitmap bitmap = RenderPageOnWhite(page.get(), 612, 792);
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 150, 392));
    EXPECT_EQ(0xFFFF0000u, GetPixelColor(bitmap.get(), 150, 287));
    EXPECT_EQ(0xFFFFFFFFu, GetPixelColor(bitmap.get(), 300, 392));
    EXPECT_EQ(0xFFFFFFFFu, GetPixelColor(bitmap.get(), 300, 287));
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 450, 392));
    EXPECT_EQ(0xFFFF0000u, GetPixelColor(bitmap.get(), 450, 287));
  }

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ASSERT_TRUE(OpenSavedDocument());
  FPDF_PAGE saved_page = LoadSavedPage(0);
  ASSERT_TRUE(saved_page);
  EXPECT_EQ(original_object_count + 2, FPDFPage_CountObjects(saved_page));
  {
    ScopedFPDFBitmap bitmap = RenderPageOnWhite(saved_page, 612, 792);
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 150, 392));
    EXPECT_EQ(0xFFFF0000u, GetPixelColor(bitmap.get(), 150, 287));
    EXPECT_EQ(0xFFFFFFFFu, GetPixelColor(bitmap.get(), 300, 392));
    EXPECT_EQ(0xFFFFFFFFu, GetPixelColor(bitmap.get(), 300, 287));
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 450, 392));
    EXPECT_EQ(0xFFFF0000u, GetPixelColor(bitmap.get(), 450, 287));
  }
  CloseSavedPage(saved_page);
}

TEST_F(FPDFAnnotEmbedderTest,
       ApplyRedactionPreservesEnclosedVectorHoleAcrossSave) {
  ASSERT_TRUE(OpenDocument("about_blank.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  const int original_object_count = FPDFPage_CountObjects(page.get());

  ScopedFPDFPageObject path(
      FPDFPageObj_CreateNewRect(100.0f, 300.0f, 400.0f, 200.0f));
  ASSERT_TRUE(path);
  ASSERT_TRUE(FPDFPageObj_SetFillColor(path.get(), 0, 0, 255, 255));
  ASSERT_TRUE(FPDFPath_SetDrawMode(path.get(), FPDF_FILLMODE_WINDING,
                                   /*stroke=*/false));
  FPDFPage_InsertObject(page.get(), path.release());
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  {
    ScopedFPDFAnnotation annot =
        CreateRedactAnnot(page.get(), {280.0f, 425.0f, 332.0f, 375.0f});
    ASSERT_TRUE(annot);
    ASSERT_TRUE(EPDFAnnot_ApplyRedaction(page.get(), annot.get(), nullptr));
  }
  ASSERT_EQ(original_object_count + 1, FPDFPage_CountObjects(page.get()));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  {
    ScopedFPDFBitmap bitmap = RenderPageOnWhite(page.get(), 612, 792);
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 150, 392));
    EXPECT_EQ(0xFFFFFFFFu, GetPixelColor(bitmap.get(), 300, 392));
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 450, 392));
  }

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ASSERT_TRUE(OpenSavedDocument());
  FPDF_PAGE saved_page = LoadSavedPage(0);
  ASSERT_TRUE(saved_page);
  EXPECT_EQ(original_object_count + 1, FPDFPage_CountObjects(saved_page));
  {
    ScopedFPDFBitmap bitmap = RenderPageOnWhite(saved_page, 612, 792);
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 150, 392));
    EXPECT_EQ(0xFFFFFFFFu, GetPixelColor(bitmap.get(), 300, 392));
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 450, 392));
  }
  CloseSavedPage(saved_page);
}

TEST_F(FPDFAnnotEmbedderTest,
       ApplyRedactionClonesSharedFormBeforeVectorEdit) {
  ASSERT_TRUE(OpenDocument("about_blank.pdf"));
  ScopedPage source_page = LoadScopedPage(0);
  ASSERT_TRUE(source_page);

  ScopedFPDFPageObject source_path(
      FPDFPageObj_CreateNewRect(0.0f, 0.0f, 100.0f, 100.0f));
  ASSERT_TRUE(source_path);
  ASSERT_TRUE(FPDFPageObj_SetFillColor(source_path.get(), 0, 0, 255, 255));
  ASSERT_TRUE(FPDFPath_SetDrawMode(source_path.get(), FPDF_FILLMODE_WINDING,
                                   /*stroke=*/false));
  FPDFPage_InsertObject(source_page.get(), source_path.release());
  ASSERT_TRUE(FPDFPage_GenerateContent(source_page.get()));

  FPDF_XOBJECT xobject =
      FPDF_NewXObjectFromPage(document(), document(), /*src_page_index=*/0);
  ASSERT_TRUE(xobject);
  ScopedFPDFPage target_page(FPDFPage_New(document(), 1, 612.0f, 792.0f));
  ASSERT_TRUE(target_page);

  FPDF_PAGEOBJECT first_form = FPDF_NewFormObjectFromXObject(xobject);
  ASSERT_TRUE(first_form);
  FPDFPageObj_Transform(first_form, 1.0, 0.0, 0.0, 1.0, 100.0, 300.0);
  FPDFPage_InsertObject(target_page.get(), first_form);

  FPDF_PAGEOBJECT second_form = FPDF_NewFormObjectFromXObject(xobject);
  ASSERT_TRUE(second_form);
  FPDFPageObj_Transform(second_form, 1.0, 0.0, 0.0, 1.0, 300.0, 300.0);
  FPDFPage_InsertObject(target_page.get(), second_form);
  ASSERT_TRUE(FPDFPage_GenerateContent(target_page.get()));
  FPDF_CloseXObject(xobject);

  {
    ScopedFPDFBitmap bitmap = RenderPageOnWhite(target_page.get(), 612, 792);
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 180, 442));
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 150, 442));
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 350, 442));
  }

  {
    ScopedFPDFAnnotation annot =
        CreateRedactAnnot(target_page.get(), {140.0f, 420.0f, 160.0f, 280.0f});
    ASSERT_TRUE(annot);
    ASSERT_TRUE(
        EPDFAnnot_ApplyRedaction(target_page.get(), annot.get(), nullptr));
  }
  ASSERT_TRUE(FPDFPage_GenerateContent(target_page.get()));

  {
    ScopedFPDFBitmap bitmap = RenderPageOnWhite(target_page.get(), 612, 792);
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 180, 442));
    EXPECT_EQ(0xFFFFFFFFu, GetPixelColor(bitmap.get(), 150, 442));
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 350, 442));
  }

  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ASSERT_TRUE(OpenSavedDocument());
  FPDF_PAGE saved_page = LoadSavedPage(1);
  ASSERT_TRUE(saved_page);
  {
    ScopedFPDFBitmap bitmap = RenderPageOnWhite(saved_page, 612, 792);
    ASSERT_TRUE(bitmap);
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 180, 442));
    EXPECT_EQ(0xFFFFFFFFu, GetPixelColor(bitmap.get(), 150, 442));
    EXPECT_EQ(0xFF0000FFu, GetPixelColor(bitmap.get(), 350, 442));
  }
  CloseSavedPage(saved_page);
}

TEST_F(FPDFAnnotEmbedderTest, RotatedCaretAppearanceCarriesTransform) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ScopedFPDFAnnotation annot(FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_CARET));
  ASSERT_TRUE(annot);
  // Rotated-visual AABB as /Rect + the box-family /EMBD_Metadata pair: the
  // generator must draw in the UNROTATED box and emit an AP form matrix.
  const FS_RECTF rect = {90.0f, 110.0f, 110.0f, 90.0f};  // L, T, R, B
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ASSERT_TRUE(
      EPDFAnnot_SetEmbedMetadataNumber(annot.get(), "Rotation", 90.0f));
  const FS_RECTF unrotated = {92.0f, 108.0f, 108.0f, 92.0f};
  ASSERT_TRUE(EPDFAnnot_SetEmbedMetadataRect(annot.get(), "UnrotatedRect",
                                             &unrotated));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

  FS_MATRIX matrix = {};
  ASSERT_TRUE(EPDFAnnot_GetAPMatrix(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                                    &matrix));
  // 90° CCW (PDF convention): a≈0, b≈1, c≈-1, d≈0 — the symbol rides its
  // text's baseline instead of staying screen-upright.
  EXPECT_NEAR(0.0f, matrix.a, 1e-4f);
  EXPECT_NEAR(1.0f, matrix.b, 1e-4f);
  EXPECT_NEAR(-1.0f, matrix.c, 1e-4f);
  EXPECT_NEAR(0.0f, matrix.d, 1e-4f);

  // An upright caret keeps the identity form matrix.
  ScopedFPDFAnnotation upright(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_CARET));
  ASSERT_TRUE(upright);
  ASSERT_TRUE(FPDFAnnot_SetRect(upright.get(), &rect));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(upright.get()));
  FS_MATRIX upright_matrix = {};
  ASSERT_TRUE(EPDFAnnot_GetAPMatrix(
      upright.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, &upright_matrix));
  EXPECT_FLOAT_EQ(1.0f, upright_matrix.a);
  EXPECT_FLOAT_EQ(0.0f, upright_matrix.b);
  EXPECT_FLOAT_EQ(0.0f, upright_matrix.c);
  EXPECT_FLOAT_EQ(1.0f, upright_matrix.d);
}

TEST_F(FPDFAnnotEmbedderTest, EmbedSetRectPreservesRotatedAppearance) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));

  const FS_RECTF original_rect = {/*left=*/58.57864f, /*top=*/341.42136f,
                                  /*right=*/341.42136f, /*bottom=*/58.57864f};
  const FS_RECTF original_unrotated = {/*left=*/100.0f, /*top=*/300.0f,
                                       /*right=*/300.0f, /*bottom=*/100.0f};
  const FS_RECTF moved_rect = {/*left=*/38.57864f, /*top=*/341.42136f,
                               /*right=*/321.42136f, /*bottom=*/58.57864f};
  const FS_RECTF moved_unrotated = {/*left=*/80.0f, /*top=*/300.0f,
                                    /*right=*/280.0f, /*bottom=*/100.0f};

  auto expect_rect = [](const CFX_FloatRect& actual, const FS_RECTF& expected) {
    EXPECT_FLOAT_EQ(expected.left, actual.left);
    EXPECT_FLOAT_EQ(expected.bottom, actual.bottom);
    EXPECT_FLOAT_EQ(expected.right, actual.right);
    EXPECT_FLOAT_EQ(expected.top, actual.top);
  };
  auto expect_matrix = [](const CFX_Matrix& actual,
                          const CFX_Matrix& expected) {
    EXPECT_FLOAT_EQ(expected.a, actual.a);
    EXPECT_FLOAT_EQ(expected.b, actual.b);
    EXPECT_FLOAT_EQ(expected.c, actual.c);
    EXPECT_FLOAT_EQ(expected.d, actual.d);
    EXPECT_FLOAT_EQ(expected.e, actual.e);
    EXPECT_FLOAT_EQ(expected.f, actual.f);
  };

  int annot_index = -1;
  CFX_FloatRect original_bbox;
  CFX_Matrix original_matrix;
  {
    ScopedPage page = LoadScopedPage(0);
    ASSERT_TRUE(page);
    annot_index = FPDFPage_GetAnnotCount(page.get());

    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_CIRCLE));
    ASSERT_TRUE(annot);
    ASSERT_TRUE(EPDFAnnot_SetRect(annot.get(), &original_rect));
    ASSERT_TRUE(
        EPDFAnnot_SetEmbedMetadataNumber(annot.get(), "Rotation", 45.0f));
    ASSERT_TRUE(EPDFAnnot_SetEmbedMetadataRect(annot.get(), "UnrotatedRect",
                                               &original_unrotated));
    ASSERT_TRUE(EPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, 229,
                                   72, 77));
    ASSERT_TRUE(EPDFAnnot_SetColor(
        annot.get(), FPDFANNOT_COLORTYPE_InteriorColor, 255, 213, 0));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

    CPDF_AnnotContext* context =
        CPDFAnnotContextFromFPDFAnnotation(annot.get());
    ASSERT_TRUE(context);
    const CPDF_Dictionary* annot_dict = context->GetAnnotDict();
    ASSERT_TRUE(annot_dict);
    RetainPtr<const CPDF_Dictionary> ap_dict =
        annot_dict->GetDictFor(pdfium::annotation::kAP);
    ASSERT_TRUE(ap_dict);
    RetainPtr<const CPDF_Dictionary> stream_dict = ap_dict->GetDictFor("N");
    ASSERT_TRUE(stream_dict);

    original_bbox = stream_dict->GetRectFor("BBox");
    original_matrix = stream_dict->GetMatrixFor("Matrix");
    expect_rect(original_bbox, original_unrotated);

    // This is the exact condition under which upstream FPDFAnnot_SetRect()
    // replaces a transform-aware AP /BBox with the larger annotation /Rect.
    EXPECT_TRUE(CFXFloatRectFromFSRectF(moved_rect).Contains(original_bbox));

    ASSERT_TRUE(EPDFAnnot_SetRect(annot.get(), &moved_rect));
    ASSERT_TRUE(EPDFAnnot_SetEmbedMetadataRect(annot.get(), "UnrotatedRect",
                                               &moved_unrotated));

    expect_rect(stream_dict->GetRectFor("BBox"), original_unrotated);
    expect_matrix(stream_dict->GetMatrixFor("Matrix"), original_matrix);

    ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  }

  ASSERT_TRUE(OpenSavedDocument());
  FPDF_PAGE saved_page = LoadSavedPage(0);
  ASSERT_TRUE(saved_page);
  {
    ScopedFPDFAnnotation saved_annot(
        FPDFPage_GetAnnot(saved_page, annot_index));
    ASSERT_TRUE(saved_annot);

    FS_RECTF saved_rect = {};
    ASSERT_TRUE(EPDFAnnot_GetRect(saved_annot.get(), &saved_rect));
    EXPECT_FLOAT_EQ(moved_rect.left, saved_rect.left);
    EXPECT_FLOAT_EQ(moved_rect.bottom, saved_rect.bottom);
    EXPECT_FLOAT_EQ(moved_rect.right, saved_rect.right);
    EXPECT_FLOAT_EQ(moved_rect.top, saved_rect.top);

    CPDF_AnnotContext* context =
        CPDFAnnotContextFromFPDFAnnotation(saved_annot.get());
    ASSERT_TRUE(context);
    const CPDF_Dictionary* annot_dict = context->GetAnnotDict();
    ASSERT_TRUE(annot_dict);
    RetainPtr<const CPDF_Dictionary> ap_dict =
        annot_dict->GetDictFor(pdfium::annotation::kAP);
    ASSERT_TRUE(ap_dict);
    RetainPtr<const CPDF_Dictionary> stream_dict = ap_dict->GetDictFor("N");
    ASSERT_TRUE(stream_dict);

    expect_rect(stream_dict->GetRectFor("BBox"), original_unrotated);
    expect_matrix(stream_dict->GetMatrixFor("Matrix"), original_matrix);
  }
  CloseSavedPage(saved_page);
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionRotatedQuadSparesNeighbours) {
  ASSERT_TRUE(OpenDocument("rotated_redact.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  std::wstring before = ExtractPageText(page.get());
  ASSERT_NE(std::wstring::npos, before.find(L"SECRET"));
  ASSERT_NE(std::wstring::npos, before.find(L"KEEPME"));

  // Measure SECRET's oriented cells and derive the mark's QuadPoints from
  // them: the union of the first and last glyph cells in slot space
  // (upper-start of the first, upper-end of the last, and so on).
  FS_QUADPOINTSF region = {};
  float bbox_left = 0;
  float bbox_bottom = 0;
  float bbox_right = 0;
  float bbox_top = 0;
  bool keepme_overlaps_bbox = false;
  {
    ScopedFPDFTextPage text_page(FPDFText_LoadPage(page.get()));
    ASSERT_TRUE(text_page);
    const int secret = static_cast<int>(before.find(L"SECRET"));
    EPDF_CHAR_GEOMETRY first = {};
    EPDF_CHAR_GEOMETRY last = {};
    ASSERT_TRUE(EPDFText_GetCharGeometry(text_page.get(), secret, &first));
    ASSERT_TRUE(EPDFText_GetCharGeometry(text_page.get(), secret + 5, &last));
    ASSERT_TRUE(first.flags & EPDF_CHARGEO_HAS_LOOSE_QUAD);
    ASSERT_FALSE(first.flags & EPDF_CHARGEO_UPRIGHT);
    region.x1 = first.loose_quad.x1;
    region.y1 = first.loose_quad.y1;
    region.x2 = last.loose_quad.x2;
    region.y2 = last.loose_quad.y2;
    region.x3 = first.loose_quad.x3;
    region.y3 = first.loose_quad.y3;
    region.x4 = last.loose_quad.x4;
    region.y4 = last.loose_quad.y4;
    bbox_left = std::min({region.x1, region.x2, region.x3, region.x4});
    bbox_right = std::max({region.x1, region.x2, region.x3, region.x4});
    bbox_bottom = std::min({region.y1, region.y2, region.y3, region.y4});
    bbox_top = std::max({region.y1, region.y2, region.y3, region.y4});

    // Discrimination guard: the pre-quad AABB semantics WOULD have clipped
    // the parallel neighbour — at least one KEEPME glyph box intersects the
    // mark's bounding box even though its cells sit outside the quad.
    const int keep = static_cast<int>(before.find(L"KEEPME"));
    for (int i = keep; i < keep + 6; ++i) {
      EPDF_CHAR_GEOMETRY geo = {};
      ASSERT_TRUE(EPDFText_GetCharGeometry(text_page.get(), i, &geo));
      if (geo.loose_box.right > bbox_left && geo.loose_box.left < bbox_right &&
          geo.loose_box.top > bbox_bottom && geo.loose_box.bottom < bbox_top) {
        keepme_overlaps_bbox = true;
        break;
      }
    }
  }
  EXPECT_TRUE(keepme_overlaps_bbox);

  {
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_REDACT));
    ASSERT_TRUE(annot);
    ASSERT_TRUE(FPDFAnnot_AppendAttachmentPoints(annot.get(), &region));
    const FS_RECTF rect = {bbox_left, bbox_top, bbox_right, bbox_bottom};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_ApplyRedaction(page.get(), annot.get(), nullptr));
  }

  ASSERT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  ASSERT_TRUE(OpenSavedDocument());
  FPDF_PAGE saved_page = LoadSavedPage(0);
  ASSERT_TRUE(saved_page);
  std::wstring after = ExtractPageText(saved_page);
  CloseSavedPage(saved_page);
  EXPECT_EQ(std::wstring::npos, after.find(L"SECRET"));
  EXPECT_NE(std::wstring::npos, after.find(L"KEEPME"));
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionFallsBackToRectForNonFiniteQuad) {
  ASSERT_TRUE(OpenDocument("redact_text_middle.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    FS_QUADPOINTSF quad = {};
    ASSERT_TRUE(FPDFAnnot_GetAttachmentPoints(annot.get(), 0, &quad));
    quad.x4 = std::numeric_limits<float>::quiet_NaN();
    ASSERT_TRUE(FPDFAnnot_SetAttachmentPoints(annot.get(), 0, &quad));
    ASSERT_TRUE(EPDFAnnot_ApplyRedaction(page.get(), annot.get(), nullptr));
  }

  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ASSERT_TRUE(OpenSavedDocument());
  FPDF_PAGE saved_page = LoadSavedPage(0);
  ASSERT_TRUE(saved_page);
  const std::wstring after = ExtractPageText(saved_page);
  CloseSavedPage(saved_page);
  EXPECT_NE(std::wstring::npos, after.find(L"hello"));
  EXPECT_EQ(std::wstring::npos, after.find(L"secret"));
  EXPECT_NE(std::wstring::npos, after.find(L"world"));
}

TEST_F(FPDFAnnotEmbedderTest, RedactInQuadsRejectsNonFiniteInput) {
  ASSERT_TRUE(OpenDocument("rotated_redact.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  const std::wstring before = ExtractPageText(page.get());
  FS_QUADPOINTSF region = {0.0f, 10.0f, 10.0f, 10.0f,
                           0.0f, 0.0f,
                           std::numeric_limits<float>::quiet_NaN(), 0.0f};
  EXPECT_FALSE(EPDFText_RedactInQuads(page.get(), &region, 1,
                                      /*recurse_forms=*/true,
                                      /*draw_black_boxes=*/false));
  EXPECT_EQ(before, ExtractPageText(page.get()));
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionCountsIntersectingAnnotation) {
  ASSERT_TRUE(OpenDocument("redact_remove_annots.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    // The intersecting Square counts; the applied REDACT itself does not.
    EXPECT_EQ(1u, ApplyRedactionCountingRemoved(page.get(), annot.get()));
  }

  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionPreservesSiblingRedactions) {
  ASSERT_TRUE(OpenDocument("redact_preserve_sibling.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(3, FPDFPage_GetAnnotCount(page.get()));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    // Only the intersecting Square counts: the applied REDACT is the
    // instruction and the sibling REDACT is preserved.
    EXPECT_EQ(1u, ApplyRedactionCountingRemoved(page.get(), annot.get()));
  }

  ASSERT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
  ScopedFPDFAnnotation remaining(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(remaining);
  EXPECT_EQ(FPDF_ANNOT_REDACT, FPDFAnnot_GetSubtype(remaining.get()));
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionDoesNotRemoveTouchOnlyAnnotation) {
  ASSERT_TRUE(OpenDocument("redact_touch_only.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    // The edge-touching Square (no positive-area intersection) survives, so
    // nothing but the REDACT itself was removed.
    EXPECT_EQ(0u, ApplyRedactionCountingRemoved(page.get(), annot.get()));
  }

  ASSERT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
  ScopedFPDFAnnotation remaining(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(remaining);
  EXPECT_EQ(FPDF_ANNOT_SQUARE, FPDFAnnot_GetSubtype(remaining.get()));
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionCascadesPopupRemoval) {
  ASSERT_TRUE(OpenDocument("redact_popup_cascade.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(3, FPDFPage_GetAnnotCount(page.get()));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    // The Text annotation and its cascaded Popup both count.
    EXPECT_EQ(2u, ApplyRedactionCountingRemoved(page.get(), annot.get()));
  }

  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
}

TEST_F(FPDFAnnotEmbedderTest, ApplyPageRedactionsCountsRemovedAnnotations) {
  ASSERT_TRUE(OpenDocument("redact_apply_all_visible.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(4, FPDFPage_GetAnnotCount(page.get()));

  // Two Squares count; the two REDACT annotations consumed by the apply do
  // not.
  EXPECT_EQ(2u, ApplyPageRedactionsCountingRemoved(page.get()));
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
}

// The overlay tests below author a REDACT annotation via the public API on a
// blank page. Since FPDFPage_CreateAnnot() bakes no /RO, applying exercises
// the synthesis path from the declarative entries (/IC, /OverlayText, /DA,
// /Q, /Repeat) — the same situation as a file marked by another processor.
// Page: 200x200; region /Rect [20 50 180 150] => bitmap x [20,180), y
// [50,150) with the top half of the region at y [50,100).

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionSynthesizesInteriorColorFill) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 200, 200));
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot =
        CreateRedactAnnot(page.get(), {20, 150, 180, 50});
    ASSERT_TRUE(annot);
    ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(),
                                   FPDFANNOT_COLORTYPE_InteriorColor,
                                   /*R=*/0, /*G=*/0, /*B=*/0, /*A=*/255));
    uint32_t removed_count = 7;  // Sentinel: must be zeroed on entry.
    ASSERT_TRUE(
        EPDFAnnot_ApplyRedaction(page.get(), annot.get(), &removed_count));
    EXPECT_EQ(0u, removed_count);
  }
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  ScopedFPDFBitmap bitmap = RenderPageOnWhite(page.get(), 200, 200);
  EXPECT_EQ(0xFF000000u, GetPixelColor(bitmap.get(), 100, 100));  // inside
  EXPECT_EQ(0xFFFFFFFFu, GetPixelColor(bitmap.get(), 10, 100));   // outside
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionSynthesizesOverlayTextLabel) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 200, 200));
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot =
        CreateRedactAnnot(page.get(), {20, 150, 180, 50});
    ASSERT_TRUE(annot);
    ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(),
                                   FPDFANNOT_COLORTYPE_InteriorColor,
                                   /*R=*/0, /*G=*/0, /*B=*/0, /*A=*/255));
    ScopedFPDFWideString text = GetFPDFWideString(L"SECRET");
    ASSERT_TRUE(EPDFAnnot_SetOverlayText(annot.get(), text.get()));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(),
                                               FPDF_FONT_HELVETICA, 12.0f,
                                               /*R=*/255, /*G=*/255,
                                               /*B=*/255));
    ASSERT_TRUE(EPDFAnnot_ApplyRedaction(page.get(), annot.get(), nullptr));
  }
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  ScopedFPDFBitmap bitmap = RenderPageOnWhite(page.get(), 200, 200);
  // The label is drawn top-aligned into the black fill, so the top half of
  // the region has non-black ink and a single 12pt line never reaches the
  // bottom half.
  EXPECT_GT(CountInkPixels(bitmap.get(), 20, 50, 180, 100, 0xFF000000u), 0);
  EXPECT_EQ(0, CountInkPixels(bitmap.get(), 20, 100, 180, 150, 0xFF000000u));
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionRepeatsOverlayTextToFillRegion) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 200, 200));
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot =
        CreateRedactAnnot(page.get(), {20, 150, 180, 50});
    ASSERT_TRUE(annot);
    ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(),
                                   FPDFANNOT_COLORTYPE_InteriorColor,
                                   /*R=*/0, /*G=*/0, /*B=*/0, /*A=*/255));
    ScopedFPDFWideString text = GetFPDFWideString(L"SECRET");
    ASSERT_TRUE(EPDFAnnot_SetOverlayText(annot.get(), text.get()));
    ASSERT_TRUE(EPDFAnnot_SetOverlayTextRepeat(annot.get(), true));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(),
                                               FPDF_FONT_HELVETICA, 12.0f,
                                               /*R=*/255, /*G=*/255,
                                               /*B=*/255));
    ASSERT_TRUE(EPDFAnnot_ApplyRedaction(page.get(), annot.get(), nullptr));
  }
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  ScopedFPDFBitmap bitmap = RenderPageOnWhite(page.get(), 200, 200);
  // /Repeat tiles the label down the region, so unlike the single-label case
  // the bottom half of the region carries ink too.
  EXPECT_GT(CountInkPixels(bitmap.get(), 20, 50, 180, 100, 0xFF000000u), 0);
  EXPECT_GT(CountInkPixels(bitmap.get(), 20, 100, 180, 150, 0xFF000000u), 0);
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionHonorsOverlayTextAlignment) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 200, 200));
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot =
        CreateRedactAnnot(page.get(), {20, 150, 180, 50});
    ASSERT_TRUE(annot);
    ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(),
                                   FPDFANNOT_COLORTYPE_InteriorColor,
                                   /*R=*/0, /*G=*/0, /*B=*/0, /*A=*/255));
    ScopedFPDFWideString text = GetFPDFWideString(L"X");
    ASSERT_TRUE(EPDFAnnot_SetOverlayText(annot.get(), text.get()));
    ASSERT_TRUE(EPDFAnnot_SetTextAlignment(annot.get(),
                                           FPDF_TEXT_ALIGNMENT_RIGHT));
    ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(),
                                               FPDF_FONT_HELVETICA, 12.0f,
                                               /*R=*/255, /*G=*/255,
                                               /*B=*/255));
    ASSERT_TRUE(EPDFAnnot_ApplyRedaction(page.get(), annot.get(), nullptr));
  }
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  ScopedFPDFBitmap bitmap = RenderPageOnWhite(page.get(), 200, 200);
  // /Q 2 pushes the single short label into the right half of the region.
  EXPECT_EQ(0, CountInkPixels(bitmap.get(), 20, 50, 100, 150, 0xFF000000u));
  EXPECT_GT(CountInkPixels(bitmap.get(), 100, 50, 180, 150, 0xFF000000u), 0);
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionPrefersBakedOverlayStream) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 200, 200));
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot =
        CreateRedactAnnot(page.get(), {20, 150, 180, 50});
    ASSERT_TRUE(annot);
    ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(),
                                   FPDFANNOT_COLORTYPE_InteriorColor,
                                   /*R=*/0, /*G=*/0, /*B=*/0, /*A=*/255));
    // Bake the appearance (and with it /RO, black fill), then flip /IC to
    // white WITHOUT regenerating. ISO 32000-2: an existing /RO takes
    // precedence over the declarative entries, so apply must paint black.
    // FPDFAnnot_SetColor() refuses to touch an annotation that already has an
    // /AP, which is precisely the stale-/RO situation this test needs — write
    // the dict entry directly.
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
    CPDF_AnnotContext* context =
        CPDFAnnotContextFromFPDFAnnotation(annot.get());
    ASSERT_TRUE(context);
    RetainPtr<CPDF_Array> ic =
        context->GetMutableAnnotDict()->SetNewFor<CPDF_Array>("IC");
    ic->AppendNew<CPDF_Number>(1.0f);
    ic->AppendNew<CPDF_Number>(1.0f);
    ic->AppendNew<CPDF_Number>(1.0f);
    ASSERT_TRUE(EPDFAnnot_ApplyRedaction(page.get(), annot.get(), nullptr));
  }
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  ScopedFPDFBitmap bitmap = RenderPageOnWhite(page.get(), 200, 200);
  EXPECT_EQ(0xFF000000u, GetPixelColor(bitmap.get(), 100, 100));
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionWithoutOverlayLeavesRegionClear) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 200, 200));
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot =
        CreateRedactAnnot(page.get(), {20, 150, 180, 50});
    ASSERT_TRUE(annot);
    // No /RO, no /IC, no /OverlayText: ISO leaves the region transparent.
    uint32_t removed_count = 7;
    ASSERT_TRUE(
        EPDFAnnot_ApplyRedaction(page.get(), annot.get(), &removed_count));
    EXPECT_EQ(0u, removed_count);
  }
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  ScopedFPDFBitmap bitmap = RenderPageOnWhite(page.get(), 200, 200);
  EXPECT_EQ(0, CountInkPixels(bitmap.get(), 20, 50, 180, 150, 0xFFFFFFFFu));
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionPreservesInheritedColorspaceResources) {
  // The fixture models a common exporter pattern: the page content stream is
  // a bare prolog (`/C1 CS /C1 cs q /X1 Do Q`) and the artwork form paints
  // with `scn` alone, INHERITING the page-level colorspace. No page object
  // "owns" the /C1 reference, so an append-only regeneration (the redaction
  // overlay) must NOT let resource pruning delete it — that turned whole
  // pages grayscale.
  ASSERT_TRUE(OpenDocument("redact_inherited_colorspace.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  auto is_reddish = [](uint32_t argb) {
    const int r = (argb >> 16) & 0xff;
    const int g = (argb >> 8) & 0xff;
    const int b = argb & 0xff;
    return r > 180 && g < 100 && b < 100;
  };

  {
    ScopedFPDFBitmap bmp = RenderPageOnWhite(page.get(), 200, 200);
    ASSERT_TRUE(is_reddish(GetPixelColor(bmp.get(), 100, 100)));
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    uint32_t removed_count = 7;
    ASSERT_TRUE(
        EPDFAnnot_ApplyRedaction(page.get(), annot.get(), &removed_count));
    EXPECT_EQ(0u, removed_count);
  }
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));

  // Live: the artwork keeps its colour, the corner box is painted.
  {
    ScopedFPDFBitmap bmp = RenderPageOnWhite(page.get(), 200, 200);
    EXPECT_TRUE(is_reddish(GetPixelColor(bmp.get(), 100, 100)));
    EXPECT_EQ(0xFF000000u, GetPixelColor(bmp.get(), 15, 185));
  }

  // Round-trip: the saved file must still carry the /C1 resource.
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ASSERT_TRUE(OpenSavedDocument());
  FPDF_PAGE saved_page = LoadSavedPage(0);
  ASSERT_TRUE(saved_page);
  {
    ScopedFPDFBitmap bmp = RenderPageOnWhite(saved_page, 200, 200);
    EXPECT_TRUE(is_reddish(GetPixelColor(bmp.get(), 100, 100)));
    EXPECT_EQ(0xFF000000u, GetPixelColor(bmp.get(), 15, 185));
  }
  CloseSavedPage(saved_page);
}

TEST_F(FPDFAnnotEmbedderTest, ApplyRedactionOnNeverParsedPageRemovesText) {
  ASSERT_TRUE(OpenDocument("redact_text_middle.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  // Deliberately touch NOTHING that would parse the page first — a headless
  // worker page looks exactly like this. The apply itself must parse; an
  // unparsed apply would silently remove nothing.
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    ASSERT_TRUE(EPDFAnnot_ApplyRedaction(page.get(), annot.get(), nullptr));
  }
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  ASSERT_TRUE(OpenSavedDocument());
  FPDF_PAGE saved_page = LoadSavedPage(0);
  ASSERT_TRUE(saved_page);
  std::wstring after = ExtractPageText(saved_page);
  EXPECT_EQ(std::wstring::npos, after.find(L"secret"));
  EXPECT_NE(std::wstring::npos, after.find(L"hello"));
  EXPECT_NE(std::wstring::npos, after.find(L"world"));
  CloseSavedPage(saved_page);
}

TEST_F(FPDFAnnotEmbedderTest, GenerateRedactAppearanceBakesLabelIntoOverlay) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 200, 200));
  ASSERT_TRUE(page);

  ScopedFPDFAnnotation annot =
      CreateRedactAnnot(page.get(), {20, 150, 180, 50});
  ASSERT_TRUE(annot);
  ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(),
                                 FPDFANNOT_COLORTYPE_InteriorColor,
                                 /*R=*/0, /*G=*/0, /*B=*/0, /*A=*/255));
  ScopedFPDFWideString text = GetFPDFWideString(L"SECRET");
  ASSERT_TRUE(EPDFAnnot_SetOverlayText(annot.get(), text.get()));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearance(annot.get(), FPDF_FONT_HELVETICA,
                                             12.0f, /*R=*/255, /*G=*/255,
                                             /*B=*/255));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

  // The rollover appearance shares the final overlay stream with /RO, so the
  // baked marking-stage hover preview must contain both the fill and the
  // label text ops.
  unsigned long length_bytes = FPDFAnnot_GetAP(
      annot.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER, nullptr, 0);
  ASSERT_GT(length_bytes, 0u);
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(length_bytes,
            FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_ROLLOVER,
                            buffer.data(), length_bytes));
  std::wstring rollover = GetPlatformWString(buffer.data());
  EXPECT_NE(std::wstring::npos, rollover.find(L" re f"));  // /IC fill
  EXPECT_NE(std::wstring::npos, rollover.find(L"Tj"));     // label text
}

TEST_F(FPDFAnnotEmbedderTest, PolygonAnnotation) {
  ASSERT_TRUE(OpenDocument("polygon_annot.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // FPDFAnnot_GetVertices() positive testing.
    unsigned long size = FPDFAnnot_GetVertices(annot.get(), nullptr, 0);
    const size_t kExpectedSize = 3;
    ASSERT_EQ(kExpectedSize, size);
    std::vector<FS_POINTF> vertices_buffer(size);
    EXPECT_EQ(size,
              FPDFAnnot_GetVertices(annot.get(), vertices_buffer.data(), size));
    EXPECT_FLOAT_EQ(159.0f, vertices_buffer[0].x);
    EXPECT_FLOAT_EQ(296.0f, vertices_buffer[0].y);
    EXPECT_FLOAT_EQ(350.0f, vertices_buffer[1].x);
    EXPECT_FLOAT_EQ(411.0f, vertices_buffer[1].y);
    EXPECT_FLOAT_EQ(472.0f, vertices_buffer[2].x);
    EXPECT_FLOAT_EQ(243.42f, vertices_buffer[2].y);

    // FPDFAnnot_GetVertices() negative testing.
    EXPECT_EQ(0U, FPDFAnnot_GetVertices(nullptr, nullptr, 0));

    // vertices_buffer is not overwritten if it is too small.
    vertices_buffer.resize(1);
    vertices_buffer[0].x = 42;
    vertices_buffer[0].y = 43;
    size = FPDFAnnot_GetVertices(annot.get(), vertices_buffer.data(),
                                 vertices_buffer.size());
    EXPECT_EQ(kExpectedSize, size);
    EXPECT_FLOAT_EQ(42, vertices_buffer[0].x);
    EXPECT_FLOAT_EQ(43, vertices_buffer[0].y);
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    // This has an odd number of elements in the vertices array, ignore the last
    // element.
    unsigned long size = FPDFAnnot_GetVertices(annot.get(), nullptr, 0);
    const size_t kExpectedSize = 3;
    ASSERT_EQ(kExpectedSize, size);
    std::vector<FS_POINTF> vertices_buffer(size);
    EXPECT_EQ(size,
              FPDFAnnot_GetVertices(annot.get(), vertices_buffer.data(), size));
    EXPECT_FLOAT_EQ(259.0f, vertices_buffer[0].x);
    EXPECT_FLOAT_EQ(396.0f, vertices_buffer[0].y);
    EXPECT_FLOAT_EQ(450.0f, vertices_buffer[1].x);
    EXPECT_FLOAT_EQ(511.0f, vertices_buffer[1].y);
    EXPECT_FLOAT_EQ(572.0f, vertices_buffer[2].x);
    EXPECT_FLOAT_EQ(343.0f, vertices_buffer[2].y);
  }

  {
    // Wrong annotation type.
    ScopedFPDFAnnotation ink_annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_INK));
    EXPECT_EQ(0U, FPDFAnnot_GetVertices(ink_annot.get(), nullptr, 0));
  }
}

TEST_F(FPDFAnnotEmbedderTest, InkAnnotation) {
  ASSERT_TRUE(OpenDocument("ink_annot.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // FPDFAnnot_GetInkListCount() and FPDFAnnot_GetInkListPath() positive
    // testing.
    unsigned long size = FPDFAnnot_GetInkListCount(annot.get());
    const size_t kExpectedSize = 1;
    ASSERT_EQ(kExpectedSize, size);
    const unsigned long kPathIndex = 0;
    unsigned long path_size =
        FPDFAnnot_GetInkListPath(annot.get(), kPathIndex, nullptr, 0);
    const size_t kExpectedPathSize = 3;
    ASSERT_EQ(kExpectedPathSize, path_size);
    std::vector<FS_POINTF> path_buffer(path_size);
    EXPECT_EQ(path_size,
              FPDFAnnot_GetInkListPath(annot.get(), kPathIndex,
                                       path_buffer.data(), path_size));
    EXPECT_FLOAT_EQ(159.0f, path_buffer[0].x);
    EXPECT_FLOAT_EQ(296.0f, path_buffer[0].y);
    EXPECT_FLOAT_EQ(350.0f, path_buffer[1].x);
    EXPECT_FLOAT_EQ(411.0f, path_buffer[1].y);
    EXPECT_FLOAT_EQ(472.0f, path_buffer[2].x);
    EXPECT_FLOAT_EQ(243.42f, path_buffer[2].y);

    // FPDFAnnot_GetInkListCount() and FPDFAnnot_GetInkListPath() negative
    // testing.
    EXPECT_EQ(0U, FPDFAnnot_GetInkListCount(nullptr));
    EXPECT_EQ(0U, FPDFAnnot_GetInkListPath(nullptr, 0, nullptr, 0));

    // out of bounds path_index.
    EXPECT_EQ(0U, FPDFAnnot_GetInkListPath(nullptr, 42, nullptr, 0));

    // path_buffer is not overwritten if it is too small.
    path_buffer.resize(1);
    path_buffer[0].x = 42;
    path_buffer[0].y = 43;
    path_size = FPDFAnnot_GetInkListPath(
        annot.get(), kPathIndex, path_buffer.data(), path_buffer.size());
    EXPECT_EQ(kExpectedPathSize, path_size);
    EXPECT_FLOAT_EQ(42, path_buffer[0].x);
    EXPECT_FLOAT_EQ(43, path_buffer[0].y);
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    // This has an odd number of elements in the path array, ignore the last
    // element.
    unsigned long size = FPDFAnnot_GetInkListCount(annot.get());
    const size_t kExpectedSize = 1;
    ASSERT_EQ(kExpectedSize, size);
    const unsigned long kPathIndex = 0;
    unsigned long path_size =
        FPDFAnnot_GetInkListPath(annot.get(), kPathIndex, nullptr, 0);
    const size_t kExpectedPathSize = 3;
    ASSERT_EQ(kExpectedPathSize, path_size);
    std::vector<FS_POINTF> path_buffer(path_size);
    EXPECT_EQ(path_size,
              FPDFAnnot_GetInkListPath(annot.get(), kPathIndex,
                                       path_buffer.data(), path_size));
    EXPECT_FLOAT_EQ(259.0f, path_buffer[0].x);
    EXPECT_FLOAT_EQ(396.0f, path_buffer[0].y);
    EXPECT_FLOAT_EQ(450.0f, path_buffer[1].x);
    EXPECT_FLOAT_EQ(511.0f, path_buffer[1].y);
    EXPECT_FLOAT_EQ(572.0f, path_buffer[2].x);
    EXPECT_FLOAT_EQ(343.0f, path_buffer[2].y);
  }

  {
    // Wrong annotation type.
    ScopedFPDFAnnotation polygon_annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_POLYGON));
    EXPECT_EQ(0U, FPDFAnnot_GetInkListCount(polygon_annot.get()));
    const unsigned long kPathIndex = 0;
    EXPECT_EQ(0U, FPDFAnnot_GetInkListPath(polygon_annot.get(), kPathIndex,
                                           nullptr, 0));
  }
}

TEST_F(FPDFAnnotEmbedderTest, LineAnnotation) {
  ASSERT_TRUE(OpenDocument("line_annot.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // FPDFAnnot_GetVertices() positive testing.
    FS_POINTF start;
    FS_POINTF end;
    ASSERT_TRUE(FPDFAnnot_GetLine(annot.get(), &start, &end));
    EXPECT_FLOAT_EQ(159.0f, start.x);
    EXPECT_FLOAT_EQ(296.0f, start.y);
    EXPECT_FLOAT_EQ(472.0f, end.x);
    EXPECT_FLOAT_EQ(243.42f, end.y);

    // FPDFAnnot_GetVertices() negative testing.
    EXPECT_FALSE(FPDFAnnot_GetLine(nullptr, nullptr, nullptr));
    EXPECT_FALSE(FPDFAnnot_GetLine(annot.get(), nullptr, nullptr));
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    // Too few elements in the line array.
    FS_POINTF start;
    FS_POINTF end;
    EXPECT_FALSE(FPDFAnnot_GetLine(annot.get(), &start, &end));
  }

  {
    // Wrong annotation type.
    ScopedFPDFAnnotation ink_annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_INK));
    FS_POINTF start;
    FS_POINTF end;
    EXPECT_FALSE(FPDFAnnot_GetLine(ink_annot.get(), &start, &end));
  }
}

TEST_F(FPDFAnnotEmbedderTest, AnnotationBorder) {
  ASSERT_TRUE(OpenDocument("line_annot.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // FPDFAnnot_GetBorder() positive testing.
    float horizontal_radius;
    float vertical_radius;
    float border_width;
    ASSERT_TRUE(FPDFAnnot_GetBorder(annot.get(), &horizontal_radius,
                                    &vertical_radius, &border_width));
    EXPECT_FLOAT_EQ(0.25f, horizontal_radius);
    EXPECT_FLOAT_EQ(0.5f, vertical_radius);
    EXPECT_FLOAT_EQ(2.0f, border_width);

    // FPDFAnnot_GetBorder() negative testing.
    EXPECT_FALSE(FPDFAnnot_GetBorder(nullptr, nullptr, nullptr, nullptr));
    EXPECT_FALSE(FPDFAnnot_GetBorder(annot.get(), nullptr, nullptr, nullptr));
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);

    // Too few elements in the border array.
    float horizontal_radius;
    float vertical_radius;
    float border_width;
    EXPECT_FALSE(FPDFAnnot_GetBorder(annot.get(), &horizontal_radius,
                                     &vertical_radius, &border_width));

    // FPDFAnnot_SetBorder() positive testing.
    EXPECT_TRUE(FPDFAnnot_SetBorder(annot.get(), /*horizontal_radius=*/2.0f,
                                    /*vertical_radius=*/3.5f,
                                    /*border_width=*/4.0f));

    EXPECT_TRUE(FPDFAnnot_GetBorder(annot.get(), &horizontal_radius,
                                    &vertical_radius, &border_width));
    EXPECT_FLOAT_EQ(2.0f, horizontal_radius);
    EXPECT_FLOAT_EQ(3.5f, vertical_radius);
    EXPECT_FLOAT_EQ(4.0f, border_width);

    // FPDFAnnot_SetBorder() negative testing.
    EXPECT_FALSE(FPDFAnnot_SetBorder(nullptr, /*horizontal_radius=*/1.0f,
                                     /*vertical_radius=*/2.5f,
                                     /*border_width=*/3.0f));
  }
}

TEST_F(FPDFAnnotEmbedderTest, BorderStyleResolutionUsesISOPrecedence) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 100, 100));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_CIRCLE));
  ASSERT_TRUE(annot);

  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot.get());
  ASSERT_TRUE(context);
  RetainPtr<CPDF_Dictionary> annot_dict = context->GetMutableAnnotDict();
  ASSERT_TRUE(annot_dict);
  annot_dict->RemoveFor("BS");
  annot_dict->RemoveFor(pdfium::annotation::kBorder);

  auto expect_style = [&](FPDF_ANNOT_BORDER_STYLE expected_style,
                          float expected_width) {
    float width = -1.0f;
    EXPECT_EQ(expected_style, EPDFAnnot_GetBorderStyle(annot.get(), &width));
    EXPECT_FLOAT_EQ(expected_width, width);
  };

  // ISO 32000-2, 12.5.4: if neither /BS nor /Border is present, the
  // effective border is a solid 1-point line.
  expect_style(FPDF_ANNOT_BS_SOLID, 1.0f);
  EXPECT_EQ(0u, EPDFAnnot_GetBorderDashPatternCount(annot.get()));

  // The legacy /Border array supplies the width and optional dash pattern
  // when /BS is absent.
  RetainPtr<CPDF_Array> border =
      annot_dict->SetNewFor<CPDF_Array>(pdfium::annotation::kBorder);
  border->AppendNew<CPDF_Number>(0);
  border->AppendNew<CPDF_Number>(0);
  border->AppendNew<CPDF_Number>(0);
  expect_style(FPDF_ANNOT_BS_SOLID, 0.0f);

  RetainPtr<CPDF_Array> border_dash = border->AppendNew<CPDF_Array>();
  border_dash->AppendNew<CPDF_Number>(3);
  border_dash->AppendNew<CPDF_Number>(2);
  expect_style(FPDF_ANNOT_BS_DASHED, 0.0f);
  ASSERT_EQ(2u, EPDFAnnot_GetBorderDashPatternCount(annot.get()));
  std::array<float, 2> dash_values;
  ASSERT_TRUE(EPDFAnnot_GetBorderDashPattern(annot.get(), dash_values.data(),
                                             dash_values.size()));
  EXPECT_FLOAT_EQ(3.0f, dash_values[0]);
  EXPECT_FLOAT_EQ(2.0f, dash_values[1]);

  // /BS suppresses /Border completely. Missing /W and /S in /BS use their
  // defaults (1 point and solid), rather than falling back to /Border.
  RetainPtr<CPDF_Dictionary> border_style =
      annot_dict->SetNewFor<CPDF_Dictionary>("BS");
  expect_style(FPDF_ANNOT_BS_SOLID, 1.0f);
  EXPECT_EQ(0u, EPDFAnnot_GetBorderDashPatternCount(annot.get()));

  const FS_RECTF rect{/*left=*/10.0f, /*top=*/90.0f, /*right=*/90.0f,
                      /*bottom=*/10.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  ByteString appearance = GetNormalAppearanceStreamBytes(annot.get());
  EXPECT_TRUE(appearance.Find("1 w ").has_value());
  EXPECT_FALSE(appearance.Find("] 0 d").has_value());

  // Explicit /BS values, including zero width, remain authoritative.
  border_style->SetNewFor<CPDF_Number>("W", 0);
  border_style->SetNewFor<CPDF_Name>("S", "D");
  RetainPtr<CPDF_Array> bs_dash = border_style->SetNewFor<CPDF_Array>("D");
  bs_dash->AppendNew<CPDF_Number>(4);
  bs_dash->AppendNew<CPDF_Number>(1);
  expect_style(FPDF_ANNOT_BS_DASHED, 0.0f);
  ASSERT_EQ(2u, EPDFAnnot_GetBorderDashPatternCount(annot.get()));
  ASSERT_TRUE(EPDFAnnot_GetBorderDashPattern(annot.get(), dash_values.data(),
                                             dash_values.size()));
  EXPECT_FLOAT_EQ(4.0f, dash_values[0]);
  EXPECT_FLOAT_EQ(1.0f, dash_values[1]);

  // Unknown /BS styles are required to use the solid default.
  border_style->SetNewFor<CPDF_Name>("S", "Unknown");
  expect_style(FPDF_ANNOT_BS_SOLID, 0.0f);
  EXPECT_EQ(0u, EPDFAnnot_GetBorderDashPatternCount(annot.get()));

  float invalid_width = 42.0f;
  EXPECT_EQ(FPDF_ANNOT_BS_UNKNOWN,
            EPDFAnnot_GetBorderStyle(nullptr, &invalid_width));
  EXPECT_FLOAT_EQ(0.0f, invalid_width);
}

TEST_F(FPDFAnnotEmbedderTest, AnnotationJavaScript) {
  ASSERT_TRUE(OpenDocument("annot_javascript.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // FPDFAnnot_GetFormAdditionalActionJavaScript() positive testing.
    unsigned long length_bytes = FPDFAnnot_GetFormAdditionalActionJavaScript(
        form_handle(), annot.get(), FPDF_ANNOT_AACTION_FORMAT, nullptr, 0);
    ASSERT_EQ(62u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(62u, FPDFAnnot_GetFormAdditionalActionJavaScript(
                       form_handle(), annot.get(), FPDF_ANNOT_AACTION_FORMAT,
                       buf.data(), length_bytes));
    EXPECT_EQ(L"AFDate_FormatEx(\"yyyy-mm-dd\");",
              GetPlatformWString(buf.data()));

    // FPDFAnnot_GetFormAdditionalActionJavaScript() negative testing.
    EXPECT_EQ(0u, FPDFAnnot_GetFormAdditionalActionJavaScript(
                      form_handle(), nullptr, 0, nullptr, 0));
    EXPECT_EQ(0u, FPDFAnnot_GetFormAdditionalActionJavaScript(
                      nullptr, annot.get(), 0, nullptr, 0));
    EXPECT_EQ(0u, FPDFAnnot_GetFormAdditionalActionJavaScript(
                      form_handle(), annot.get(), 0, nullptr, 0));
    EXPECT_EQ(2u, FPDFAnnot_GetFormAdditionalActionJavaScript(
                      form_handle(), annot.get(), FPDF_ANNOT_AACTION_KEY_STROKE,
                      nullptr, 0));
  }
}

TEST_F(FPDFAnnotEmbedderTest, FormFieldAlternateName) {
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(8, FPDFPage_GetAnnotCount(page.get()));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    // FPDFAnnot_GetFormFieldAlternateName() positive testing.
    unsigned long length_bytes = FPDFAnnot_GetFormFieldAlternateName(
        form_handle(), annot.get(), nullptr, 0);
    ASSERT_EQ(34u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(34u, FPDFAnnot_GetFormFieldAlternateName(
                       form_handle(), annot.get(), buf.data(), length_bytes));
    EXPECT_EQ(L"readOnlyCheckbox", GetPlatformWString(buf.data()));

    // FPDFAnnot_GetFormFieldAlternateName() negative testing.
    EXPECT_EQ(0u, FPDFAnnot_GetFormFieldAlternateName(form_handle(), nullptr,
                                                      nullptr, 0));
    EXPECT_EQ(0u, FPDFAnnot_GetFormFieldAlternateName(nullptr, annot.get(),
                                                      nullptr, 0));
  }
}

// Due to https://crbug.com/41480220, the AnnotationBorder test above cannot
// actually render the line annotations inside line_annot.pdf. For now, use a
// square annotation in annots.pdf for testing.
TEST_F(FPDFAnnotEmbedderTest, AnnotationBorderRendering) {
  ASSERT_TRUE(OpenDocument("annots.pdf"));
  ScopedPage page = LoadScopedPage(1);
  ASSERT_TRUE(page);
  EXPECT_EQ(3, FPDFPage_GetAnnotCount(page.get()));

  static constexpr char kAnnotsPage2ModifiedPng[] = "annots_page2_modified";
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 2));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_SQUARE, FPDFAnnot_GetSubtype(annot.get()));

    {
      ScopedFPDFBitmap bitmap =
          RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
      CompareBitmapWithExpectationSuffix(bitmap.get(), "annots_page2");
    }

    EXPECT_TRUE(FPDFAnnot_SetBorder(annot.get(), /*horizontal_radius=*/2.0f,
                                    /*vertical_radius=*/3.5f,
                                    /*border_width=*/4.0f));

    {
      ScopedFPDFBitmap bitmap =
          RenderLoadedPageWithFlags(page.get(), FPDF_ANNOT);
      CompareBitmapWithExpectationSuffix(bitmap.get(), kAnnotsPage2ModifiedPng);
    }
  }

  // Save the document and close the page.
  EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));

  {
    ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
    ASSERT_TRUE(saved_doc);
    ScopedSavedPage saved_page = LoadScopedSavedPage(1);
    ASSERT_TRUE(saved_page);
    VerifySavedRenderingWithExpectationSuffix(saved_page.get(),
                                              kAnnotsPage2ModifiedPng);
  }
}

TEST_F(FPDFAnnotEmbedderTest, GetAndAddFileAttachmentAnnotation) {
  ASSERT_TRUE(OpenDocument("annotation_fileattachment.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_FILEATTACHMENT, FPDFAnnot_GetSubtype(annot.get()));

    FPDF_ATTACHMENT attachment = FPDFAnnot_GetFileAttachment(annot.get());
    ASSERT_TRUE(attachment);

    // Check that the name of the attachment is correct.
    unsigned long length_bytes = FPDFAttachment_GetName(attachment, nullptr, 0);
    ASSERT_EQ(18u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(18u,
              FPDFAttachment_GetName(attachment, buf.data(), length_bytes));
    EXPECT_EQ(L"test.txt", GetPlatformWString(buf.data()));

    // Check that the content of the attachment is correct.
    ASSERT_TRUE(FPDFAttachment_GetFile(attachment, nullptr, 0, &length_bytes));
    std::vector<uint8_t> content_buf(length_bytes);
    unsigned long actual_length_bytes;
    ASSERT_TRUE(FPDFAttachment_GetFile(attachment, content_buf.data(),
                                       length_bytes, &actual_length_bytes));
    ASSERT_THAT(content_buf, testing::ElementsAre('t', 'e', 's', 't', ' ', 't',
                                                  'e', 'x', 't'));
  }

  {
    // Add a file attachment annotation to the page.
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FILEATTACHMENT));
    ASSERT_TRUE(annot);

    // Check that there is now 2 annotations on this page.
    EXPECT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

    ScopedFPDFWideString file_name = GetFPDFWideString(L"0.txt");
    FPDF_ATTACHMENT attachment =
        FPDFAnnot_AddFileAttachment(annot.get(), file_name.get());
    ASSERT_TRUE(attachment);

    // The filling of the FPDF_ATTACHMENT has been tested in
    // fpdf_attachment_embeddertest.cpp
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 1));
    ASSERT_TRUE(annot);
    EXPECT_EQ(FPDF_ANNOT_FILEATTACHMENT, FPDFAnnot_GetSubtype(annot.get()));

    // Check that we can read newly created file spec
    FPDF_ATTACHMENT attachment = FPDFAnnot_GetFileAttachment(annot.get());
    ASSERT_TRUE(attachment);

    // Verify the name of the new attachment.
    unsigned long length_bytes = FPDFAttachment_GetName(attachment, nullptr, 0);
    ASSERT_EQ(12u, length_bytes);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(12u,
              FPDFAttachment_GetName(attachment, buf.data(), length_bytes));
    EXPECT_EQ(L"0.txt", GetPlatformWString(buf.data()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, BadCasesFileAttachmentAnnotation) {
  ASSERT_TRUE(OpenDocument("annotation_fileattachment.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));

  {
    ASSERT_FALSE(FPDFAnnot_GetFileAttachment(nullptr));

    ScopedFPDFAnnotation text_annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_TEXT));
    ASSERT_TRUE(text_annot);
    ASSERT_FALSE(FPDFAnnot_GetFileAttachment(text_annot.get()));

    ScopedFPDFAnnotation newly_file_annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FILEATTACHMENT));
    ASSERT_TRUE(newly_file_annot);
    ASSERT_FALSE(FPDFAnnot_GetFileAttachment(newly_file_annot.get()));
  }

  {
    ScopedFPDFWideString empty_name = GetFPDFWideString(L"");
    ScopedFPDFWideString not_empty_name = GetFPDFWideString(L"0.txt");

    ASSERT_FALSE(FPDFAnnot_AddFileAttachment(nullptr, nullptr));
    ASSERT_FALSE(FPDFAnnot_AddFileAttachment(nullptr, empty_name.get()));
    ASSERT_FALSE(FPDFAnnot_AddFileAttachment(nullptr, not_empty_name.get()));

    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);

    ASSERT_FALSE(FPDFAnnot_AddFileAttachment(annot.get(), nullptr));
    ASSERT_FALSE(FPDFAnnot_AddFileAttachment(annot.get(), empty_name.get()));

    FPDF_ATTACHMENT old_attachment = FPDFAnnot_GetFileAttachment(annot.get());
    EXPECT_NE(old_attachment,
              FPDFAnnot_AddFileAttachment(annot.get(), not_empty_name.get()));
  }
}

TEST_F(FPDFAnnotEmbedderTest, SetFormFieldFlags) {
  ASSERT_TRUE(OpenDocument("text_form_multiple.pdf"));
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, 0));
    ASSERT_TRUE(annot);

    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_FALSE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_TEXT_MULTILINE);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_TEXT_PASSWORD);

    int new_flags = FPDF_FORMFLAG_READONLY | FPDF_FORMFLAG_REQUIRED;
    EXPECT_TRUE(
        FPDFAnnot_SetFormFieldFlags(form_handle(), annot.get(), new_flags));

    flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_TRUE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_TEXT_MULTILINE);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_TEXT_PASSWORD);
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, 3));
    ASSERT_TRUE(annot);

    int flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_FALSE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_TEXT_MULTILINE);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_TEXT_PASSWORD);

    int new_flags = FPDF_FORMFLAG_TEXT_MULTILINE | FPDF_FORMFLAG_NOEXPORT;
    EXPECT_TRUE(
        FPDFAnnot_SetFormFieldFlags(form_handle(), annot.get(), new_flags));

    flags = FPDFAnnot_GetFormFieldFlags(form_handle(), annot.get());
    EXPECT_FALSE(flags & FPDF_FORMFLAG_READONLY);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_REQUIRED);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_NOEXPORT);
    EXPECT_TRUE(flags & FPDF_FORMFLAG_TEXT_MULTILINE);
    EXPECT_FALSE(flags & FPDF_FORMFLAG_TEXT_PASSWORD);
  }

  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, 0));
    ASSERT_TRUE(annot);
    EXPECT_FALSE(FPDFAnnot_SetFormFieldFlags(nullptr, annot.get(), 0));

    EXPECT_FALSE(FPDFAnnot_SetFormFieldFlags(form_handle(), nullptr, 0));
    EXPECT_FALSE(FPDFAnnot_SetFormFieldFlags(nullptr, nullptr, 0));
  }

  UnloadPage(page);
}

TEST_F(FPDFAnnotEmbedderTest, SharedFormXObjectMatrix) {
  ASSERT_TRUE(OpenDocument("shared_form_xobject_matrix.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(2, FPDFPage_GetAnnotCount(page.get()));

  // The first annotation directly accesses a shared form XObject. Retrieve the
  // page object to trigger crbug.com/475719025.
  ScopedFPDFAnnotation annotation1(FPDFPage_GetAnnot(page.get(), 0));
  FPDF_PAGEOBJECT page_object1 = FPDFAnnot_GetObject(annotation1.get(), 0);
  ASSERT_TRUE(page_object1);

  FS_MATRIX matrix1;
  ASSERT_TRUE(FPDFPageObj_GetMatrix(page_object1, &matrix1));
  EXPECT_FLOAT_EQ(1.0f, matrix1.a);
  EXPECT_FLOAT_EQ(0.0f, matrix1.b);
  EXPECT_FLOAT_EQ(0.0f, matrix1.c);
  EXPECT_FLOAT_EQ(1.0f, matrix1.d);
  EXPECT_FLOAT_EQ(156.1774f, matrix1.e);
  EXPECT_FLOAT_EQ(681.1501f, matrix1.f);

  // The second annotation indirectly accesses the shared form XObject through a
  // wrapper XObject.
  ScopedFPDFAnnotation annotation2(FPDFPage_GetAnnot(page.get(), 1));
  ASSERT_TRUE(annotation2);
  FPDF_PAGEOBJECT page_object2 = FPDFAnnot_GetObject(annotation2.get(), 0);
  ASSERT_TRUE(page_object2);
  FPDF_PAGEOBJECT form_object2 = FPDFFormObj_GetObject(page_object2, 0);
  ASSERT_TRUE(form_object2);

  FS_MATRIX matrix2;
  ASSERT_TRUE(FPDFPageObj_GetMatrix(form_object2, &matrix2));
  EXPECT_FLOAT_EQ(1.0f, matrix2.a);
  EXPECT_FLOAT_EQ(0.0f, matrix2.b);
  EXPECT_FLOAT_EQ(0.0f, matrix2.c);
  EXPECT_FLOAT_EQ(1.0f, matrix2.d);
  EXPECT_FLOAT_EQ(-10.395f, matrix2.e);
  EXPECT_FLOAT_EQ(-5.42212f, matrix2.f);
}

TEST_F(FPDFAnnotEmbedderTest, SetNameAcceptsAnyNameAndKeepsAppearance) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_STAMP));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 130.0f, 90.0f, 50.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString ap = GetFPDFWideString(L"0 0 1 RG 1 w 0 0 m 10 10 l S");
  ASSERT_TRUE(FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, ap.get()));
  ASSERT_TRUE(FPDFAnnot_HasKey(annot.get(), "AP"));

  // Refusals.
  EXPECT_FALSE(EPDFAnnot_SetName(nullptr, "#x"));
  EXPECT_FALSE(EPDFAnnot_SetName(annot.get(), nullptr));
  EXPECT_FALSE(EPDFAnnot_SetName(annot.get(), ""));
  EXPECT_EQ(0u, EPDFAnnot_GetName(annot.get(), nullptr, 0));

  // An Acrobat-style custom identifier: '#' is raw text to the caller; the
  // serializer escapes it. /AP survives — unlike the UNKNOWN enum path.
  static constexpr char kAcrobatId[] = "#LBGiYhk8V_oAfmqAPENiwD";
  ASSERT_TRUE(EPDFAnnot_SetName(annot.get(), kAcrobatId));
  EXPECT_TRUE(FPDFAnnot_HasKey(annot.get(), "AP"));
  {
    const unsigned long len = EPDFAnnot_GetName(annot.get(), nullptr, 0);
    ASSERT_EQ(sizeof(kAcrobatId), len);
    std::vector<char> buf(len);
    EXPECT_EQ(len, EPDFAnnot_GetName(annot.get(), buf.data(), len));
    EXPECT_STREQ(kAcrobatId, buf.data());
  }
  // The generic string reader (what the engine's raw-name fallback uses)
  // decodes the name object too.
  {
    const unsigned long len =
        FPDFAnnot_GetStringValue(annot.get(), "Name", nullptr, 0);
    ASSERT_GT(len, 0u);
    std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(len);
    EXPECT_EQ(len, FPDFAnnot_GetStringValue(annot.get(), "Name", buf.data(), len));
    EXPECT_EQ(L"#LBGiYhk8V_oAfmqAPENiwD", GetPlatformWString(buf.data()));
  }

  // A standard name is just another name.
  ASSERT_TRUE(EPDFAnnot_SetName(annot.get(), "Approved"));
  EXPECT_TRUE(FPDFAnnot_HasKey(annot.get(), "AP"));
  {
    const unsigned long len = EPDFAnnot_GetName(annot.get(), nullptr, 0);
    std::vector<char> buf(len);
    EXPECT_EQ(len, EPDFAnnot_GetName(annot.get(), buf.data(), len));
    EXPECT_STREQ("Approved", buf.data());
  }

  // Removal is the generic key removal — /AP stays.
  ASSERT_TRUE(EPDFAnnot_RemoveKey(annot.get(), "Name"));
  EXPECT_EQ(0u, EPDFAnnot_GetName(annot.get(), nullptr, 0));
  EXPECT_TRUE(FPDFAnnot_HasKey(annot.get(), "AP"));

  // Only /Name-bearing subtypes accept it.
  ScopedFPDFAnnotation square(FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_SQUARE));
  ASSERT_TRUE(square);
  EXPECT_FALSE(EPDFAnnot_SetName(square.get(), "Approved"));

  // Round trip through a save: the '#' comes back as text.
  ASSERT_TRUE(EPDFAnnot_SetName(annot.get(), kAcrobatId));
  ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(doc.get(), this, 0));
  const std::string saved = GetString();
  // Serialized as a NAME with '#' escaped, never as a string.
  EXPECT_NE(std::string::npos, saved.find("/#23LBGiYhk8V_oAfmqAPENiwD"));
  EXPECT_EQ(std::string::npos, saved.find("(#LBGiYhk8V_oAfmqAPENiwD)"));
  ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_doc);
  ScopedFPDFPage saved_page(FPDF_LoadPage(saved_doc.get(), 0));
  ASSERT_TRUE(saved_page);
  ASSERT_EQ(2, FPDFPage_GetAnnotCount(saved_page.get()));
  ScopedFPDFAnnotation reloaded(FPDFPage_GetAnnot(saved_page.get(), 0));
  ASSERT_TRUE(reloaded);
  const unsigned long len = EPDFAnnot_GetName(reloaded.get(), nullptr, 0);
  ASSERT_EQ(sizeof(kAcrobatId), len);
  std::vector<char> buf(len);
  EXPECT_EQ(len, EPDFAnnot_GetName(reloaded.get(), buf.data(), len));
  EXPECT_STREQ(kAcrobatId, buf.data());
}

TEST_F(FPDFAnnotEmbedderTest, GenerateFileAttachmentAppearance) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FILEATTACHMENT));
  ASSERT_TRUE(annot);

  const FS_RECTF rect{50.0f, 130.0f, 90.0f, 50.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, 255, 0,
                                 0, 255));
  ASSERT_TRUE(EPDFAnnot_SetName(annot.get(), "Paperclip"));

  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

  // The paperclip is a stroked wire glyph.
  std::wstring appearance = GetNormalAppearance(annot.get());
  EXPECT_THAT(appearance, HasSubstr(L"S\n"));

  // Like the note icon, the /Rect is forced to the fixed 20x20 icon box
  // anchored at the original bottom-left corner.
  FS_RECTF actual_rect;
  ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &actual_rect));
  EXPECT_FLOAT_EQ(50.0f, actual_rect.left);
  EXPECT_FLOAT_EQ(50.0f, actual_rect.bottom);
  EXPECT_FLOAT_EQ(70.0f, actual_rect.right);
  EXPECT_FLOAT_EQ(70.0f, actual_rect.top);
}

TEST_F(FPDFAnnotEmbedderTest, GenerateFileAttachmentAppearancePerIcon) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ASSERT_TRUE(doc);
  ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 400, 400));
  ASSERT_TRUE(page);

  auto make_appearance = [&](const char* icon) {
    ScopedFPDFAnnotation annot(
        FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FILEATTACHMENT));
    EXPECT_TRUE(annot);
    const FS_RECTF rect{10.0f, 30.0f, 30.0f, 10.0f};
    EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    if (icon) {
      EXPECT_TRUE(EPDFAnnot_SetName(annot.get(), icon));
    }
    EXPECT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
    return GetNormalAppearance(annot.get());
  };

  const std::wstring pushpin = make_appearance("PushPin");
  const std::wstring paperclip = make_appearance("Paperclip");
  const std::wstring graph = make_appearance("Graph");
  const std::wstring tag = make_appearance("Tag");

  // Each icon draws a distinct glyph.
  EXPECT_NE(pushpin, paperclip);
  EXPECT_NE(pushpin, graph);
  EXPECT_NE(pushpin, tag);
  EXPECT_NE(paperclip, graph);
  EXPECT_NE(paperclip, tag);
  EXPECT_NE(graph, tag);

  // Filled glyphs paint fill+stroke; the paperclip wire only strokes.
  EXPECT_THAT(pushpin, HasSubstr(L"B\n"));
  EXPECT_THAT(graph, HasSubstr(L"B*\n"));
  EXPECT_THAT(tag, HasSubstr(L"B*\n"));
  EXPECT_THAT(paperclip, HasSubstr(L"S\n"));

  // An absent /Name renders the PushPin glyph (the ISO 32000 default).
  const std::wstring default_icon = make_appearance(nullptr);
  EXPECT_EQ(pushpin, default_icon);
}

// ---------------------------------------------------------------------------
// EmbedPDF: the delta is what changed, not what was touched.
//
// L1 - reads never promote: removing one annotation promotes exactly the page.
// L2 - a save writes what changed from the base and reports whether anything
//      reachable changed since the layer was loaded; an annotation added and
//      removed again (its generated appearance stream left as an orphan)
//      writes nothing.
// ---------------------------------------------------------------------------

namespace {

// A base document plus a layer over it, fresh or reopened over a delta.
struct LayerFixture {
  std::vector<uint8_t> bytes;
  EPDF_BASE_DOCUMENT base = nullptr;
  FPDF_DOCUMENT layer = nullptr;
  std::unique_ptr<MemoryFileAccess> delta_access;

  ~LayerFixture() {
    if (layer) {
      FPDF_CloseDocument(layer);
    }
    if (base) {
      EPDF_ReleaseBaseDocument(base);
    }
  }

  bool OpenFresh(const char* file_name) {
    std::string file_path = PathService::GetTestFilePath(file_name);
    if (file_path.empty()) {
      return false;
    }
    bytes = GetFileContents(file_path.c_str());
    if (bytes.empty()) {
      return false;
    }
    base = EPDF_LoadMemBaseDocument(bytes.data(), static_cast<int>(bytes.size()),
                                    nullptr);
    if (!base) {
      return false;
    }
    EPDFLayerOpenStatus status;
    layer = EPDFLayer_OpenLayer(base, nullptr, nullptr, &status);
    return layer && status == EPDFLayerOpenStatus_kSuccess;
  }

  // Close the layer and open a new one over the same base with |delta| as
  // the loaded delta: what a session reopening its saved layer does.
  bool Reopen(std::vector<uint8_t> delta) {
    if (layer) {
      FPDF_CloseDocument(layer);
      layer = nullptr;
    }
    delta_access = std::make_unique<MemoryFileAccess>(std::move(delta));
    EPDFLayerOpenStatus status;
    layer = EPDFLayer_OpenLayer(base, delta_access.get(), nullptr, &status);
    return layer && status == EPDFLayerOpenStatus_kSuccess;
  }
};

uint32_t AnnotObjNumAt(FPDF_PAGE page, int index) {
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, index));
  return annot ? EPDFAnnot_GetObjectNumber(annot.get()) : 0;
}

struct DeltaProbe {
  std::vector<uint8_t> bytes;
  EPDFLayerSaveStatus status = EPDFLayerSaveStatus_kSaveFailed;
  bool changed_since_load = true;
};

// EPDFLayer_SaveDeltaToOwnedBufferEx: the cumulative delta, or nothing when
// nothing changed since load.
DeltaProbe SaveDeltaEx(FPDF_DOCUMENT layer) {
  DeltaProbe out;
  unsigned long size = 0;
  FPDF_BOOL changed = true;
  void* buffer =
      EPDFLayer_SaveDeltaToOwnedBufferEx(layer, &size, &out.status, &changed);
  out.changed_since_load = !!changed;
  if (buffer) {
    const uint8_t* data = static_cast<const uint8_t*>(buffer);
    out.bytes.assign(data, data + size);
    EPDF_FreeBuffer(buffer);
  }
  return out;
}

// EPDFLayer_SaveDeltaToOwnedBuffer: the cumulative delta a persisted artifact
// carries, always written.
std::vector<uint8_t> SaveDeltaCumulative(FPDF_DOCUMENT layer) {
  unsigned long size = 0;
  EPDFLayerSaveStatus status = EPDFLayerSaveStatus_kSaveFailed;
  void* buffer = EPDFLayer_SaveDeltaToOwnedBuffer(layer, &size, &status);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, status);
  std::vector<uint8_t> out;
  if (buffer) {
    const uint8_t* data = static_cast<const uint8_t*>(buffer);
    out.assign(data, data + size);
    EPDF_FreeBuffer(buffer);
  }
  return out;
}

struct StandaloneProbe {
  std::vector<uint8_t> bytes;
  EPDFSaveStatus status = EPDFSaveStatus_kFailed;
};

// EPDF_SaveDocumentToOwnedBufferEx with FPDF_INCREMENTAL: the base bytes
// plus one revision, or nothing when nothing changed since load.
StandaloneProbe SaveStandaloneEx(FPDF_DOCUMENT doc) {
  StandaloneProbe out;
  unsigned long size = 0;
  void* buffer = EPDF_SaveDocumentToOwnedBufferEx(doc, FPDF_INCREMENTAL, 0,
                                                  &size, &out.status);
  if (buffer) {
    const uint8_t* data = static_cast<const uint8_t*>(buffer);
    out.bytes.assign(data, data + size);
    EPDF_FreeBuffer(buffer);
  }
  return out;
}

// How many times |objnum| is written as an indirect object in |bytes|.
int CountObjectWrites(const std::vector<uint8_t>& bytes, uint32_t objnum) {
  const std::string text(bytes.begin(), bytes.end());
  const std::string needle = std::to_string(objnum) + " 0 obj";
  int count = 0;
  size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    const bool digit_before = pos > 0 && isdigit(static_cast<unsigned char>(text[pos - 1]));
    if (!digit_before) {
      ++count;
    }
    pos += needle.size();
  }
  return count;
}

void SetAnnotString(FPDF_ANNOTATION annot, const char* key, const wchar_t* value) {
  ScopedFPDFWideString text = GetFPDFWideString(value);
  ASSERT_TRUE(FPDFAnnot_SetStringValue(annot, key, text.get()));
}

// What one mutation of a page's /Annots promotes: the page, plus the array
// itself when the fixture stores it as an indirect object (this one does).
unsigned long PromotedByAnnotsMutation(FPDF_PAGE page) {
  RetainPtr<const CPDF_Object> annots =
      CPDFPageFromFPDFPage(page)->GetDict()->GetObjectFor("Annots");
  return annots && annots->IsReference() ? 2ul : 1ul;
}

constexpr char kTwoAnnots[] = "annotation_highlight_square_with_ap.pdf";
constexpr char kProbeKey[] = "EPDFTwinProbe";

}  // namespace

TEST_F(FPDFAnnotEmbedderTest, LayerRemoveAnnotByObjectNumberPromotesOnlyThePage) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh(kTwoAnnots));
  ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
  ASSERT_TRUE(page);
  const int initial = FPDFPage_GetAnnotCount(page.get());
  ASSERT_GE(initial, 2);
  const uint32_t page_num = EPDFPage_GetObjectNumber(page.get());
  const uint32_t first = AnnotObjNumAt(page.get(), 0);
  const uint32_t second = AnnotObjNumAt(page.get(), 1);
  ASSERT_NE(0u, first);
  ASSERT_NE(0u, second);
  // Loading the page and listing its annotations are reads.
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(doc.layer));
  const unsigned long expected = PromotedByAnnotsMutation(page.get());

  ASSERT_TRUE(EPDFPage_RemoveAnnotByObjectNumber(page.get(), second));

  EXPECT_EQ(initial - 1, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_EQ(first, AnnotObjNumAt(page.get(), 0));
  // Exactly the page (and its /Annots array): it shrank. The siblings were
  // read, not touched; the removed object was deleted, not edited.
  EXPECT_EQ(expected, EPDFLayer_GetPromotedObjectCount(doc.layer));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, page_num));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.layer, first));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.layer, second));

  // What really changed is written, and only that. When the fixture keeps
  // /Annots as an indirect array, the array is what shrank: the page was
  // touched (promoted) but still equals its base twin and is elided.
  RetainPtr<const CPDF_Object> annots_entry =
      CPDFPageFromFPDFPage(page.get())->GetDict()->GetObjectFor("Annots");
  ASSERT_TRUE(annots_entry);
  DeltaProbe delta = SaveDeltaEx(doc.layer);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, delta.status);
  EXPECT_TRUE(delta.changed_since_load);
  if (annots_entry->IsReference()) {
    EXPECT_EQ(1, CountObjectWrites(delta.bytes,
                                   annots_entry->AsReference()->GetRefObjNum()));
    EXPECT_EQ(0, CountObjectWrites(delta.bytes, page_num));
  } else {
    EXPECT_EQ(1, CountObjectWrites(delta.bytes, page_num));
  }
  EXPECT_EQ(0, CountObjectWrites(delta.bytes, first));
  EXPECT_EQ(0, CountObjectWrites(delta.bytes, second));
}

TEST_F(FPDFAnnotEmbedderTest, LayerRemoveAnnotByNamePromotesOnlyThePage) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh(kTwoAnnots));
  ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
  ASSERT_TRUE(page);
  const uint32_t page_num = EPDFPage_GetObjectNumber(page.get());

  // The fixture carries one /NM; find it with reads.
  int named_index = -1;
  std::wstring name;
  for (int i = 0; i < FPDFPage_GetAnnotCount(page.get()); ++i) {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), i));
    ASSERT_TRUE(annot);
    if (!FPDFAnnot_HasKey(annot.get(), "NM")) {
      continue;
    }
    const unsigned long length =
        FPDFAnnot_GetStringValue(annot.get(), "NM", nullptr, 0);
    std::vector<FPDF_WCHAR> buffer(length / sizeof(FPDF_WCHAR));
    FPDFAnnot_GetStringValue(annot.get(), "NM", buffer.data(), length);
    name = GetPlatformWString(buffer.data());
    named_index = i;
    break;
  }
  ASSERT_GE(named_index, 0);
  ASSERT_FALSE(name.empty());
  const uint32_t named = AnnotObjNumAt(page.get(), named_index);
  const uint32_t other = AnnotObjNumAt(page.get(), named_index == 0 ? 1 : 0);
  const int initial = FPDFPage_GetAnnotCount(page.get());
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(doc.layer));
  const unsigned long expected = PromotedByAnnotsMutation(page.get());

  ScopedFPDFWideString nm = GetFPDFWideString(name.c_str());
  ASSERT_TRUE(EPDFPage_RemoveAnnotByName(page.get(), nm.get()));

  EXPECT_EQ(initial - 1, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_EQ(expected, EPDFLayer_GetPromotedObjectCount(doc.layer));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, page_num));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.layer, named));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.layer, other));
}

TEST_F(FPDFAnnotEmbedderTest, LayerRemoveAnnotByIndexPromotesOnlyThePage) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh(kTwoAnnots));
  ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
  ASSERT_TRUE(page);
  const uint32_t page_num = EPDFPage_GetObjectNumber(page.get());
  const uint32_t first = AnnotObjNumAt(page.get(), 0);
  const uint32_t second = AnnotObjNumAt(page.get(), 1);
  const int initial = FPDFPage_GetAnnotCount(page.get());
  const unsigned long expected = PromotedByAnnotsMutation(page.get());

  ASSERT_TRUE(EPDFPage_RemoveAnnot(page.get(), 1));

  EXPECT_EQ(initial - 1, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_EQ(expected, EPDFLayer_GetPromotedObjectCount(doc.layer));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, page_num));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.layer, first));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.layer, second));
}

TEST_F(FPDFAnnotEmbedderTest, LayerRemoveAnnotRawPromotesOnlyThePage) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh(kTwoAnnots));
  uint32_t page_num = 0;
  uint32_t first = 0;
  uint32_t second = 0;
  int initial = 0;
  unsigned long expected = 0;
  {
    ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
    ASSERT_TRUE(page);
    page_num = EPDFPage_GetObjectNumber(page.get());
    first = AnnotObjNumAt(page.get(), 0);
    second = AnnotObjNumAt(page.get(), 1);
    initial = FPDFPage_GetAnnotCount(page.get());
    expected = PromotedByAnnotsMutation(page.get());
  }
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(doc.layer));

  ASSERT_TRUE(EPDFPage_RemoveAnnotRaw(doc.layer, 0, 1));

  EXPECT_EQ(initial - 1, EPDFPage_GetAnnotCountRaw(doc.layer, 0));
  EXPECT_EQ(expected, EPDFLayer_GetPromotedObjectCount(doc.layer));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, page_num));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.layer, first));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.layer, second));
}

TEST_F(FPDFAnnotEmbedderTest, LayerMoveAnnotsPromotesOnlyThePageAndMoveBackIsUnchanged) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh(kTwoAnnots));
  ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
  ASSERT_TRUE(page);
  const uint32_t page_num = EPDFPage_GetObjectNumber(page.get());
  const uint32_t first = AnnotObjNumAt(page.get(), 0);
  const uint32_t second = AnnotObjNumAt(page.get(), 1);

  const unsigned long expected = PromotedByAnnotsMutation(page.get());

  const int from[1] = {1};
  ASSERT_TRUE(EPDFPage_MoveAnnots(page.get(), from, 1, 0));
  EXPECT_EQ(second, AnnotObjNumAt(page.get(), 0));
  EXPECT_EQ(expected, EPDFLayer_GetPromotedObjectCount(doc.layer));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, page_num));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.layer, first));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.layer, second));
  EXPECT_TRUE(SaveDeltaEx(doc.layer).changed_since_load);

  // Move it back: the page is still promoted (touched) but equals its twin.
  ASSERT_TRUE(EPDFPage_MoveAnnots(page.get(), from, 1, 0));
  EXPECT_EQ(first, AnnotObjNumAt(page.get(), 0));
  EXPECT_EQ(expected, EPDFLayer_GetPromotedObjectCount(doc.layer));
  DeltaProbe delta = SaveDeltaEx(doc.layer);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, delta.status);
  EXPECT_FALSE(delta.changed_since_load);
  EXPECT_TRUE(delta.bytes.empty());
}

// The report's flow. The annotation is created the way the engine creates
// it, with a generated appearance: an indirect /AP stream that removal leaves
// behind as an orphan. Promoted counts it; the save pass reaches neither
// difference and writes nothing.
TEST_F(FPDFAnnotEmbedderTest, LayerAddThenRemoveIsUnchangedSinceLoad) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh(kTwoAnnots));
  ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
  ASSERT_TRUE(page);
  const uint32_t page_num = EPDFPage_GetObjectNumber(page.get());
  const int initial = FPDFPage_GetAnnotCount(page.get());

  uint32_t added = 0;
  {
    ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_SQUARE));
    ASSERT_TRUE(annot);
    const FS_RECTF rect{50.0f, 50.0f, 150.0f, 150.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, 255, 0, 0, 255));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
    ASSERT_TRUE(FPDFAnnot_HasKey(annot.get(), "AP"));
    added = EPDFAnnot_GetObjectNumber(annot.get());
  }
  ASSERT_NE(0u, added);
  EXPECT_EQ(initial + 1, FPDFPage_GetAnnotCount(page.get()));
  // The page (and its /Annots array), the annotation, its appearance stream.
  const unsigned long touched = PromotedByAnnotsMutation(page.get()) + 2;
  EXPECT_EQ(touched, EPDFLayer_GetPromotedObjectCount(doc.layer));
  EXPECT_TRUE(SaveDeltaEx(doc.layer).changed_since_load);

  ASSERT_TRUE(EPDFPage_RemoveAnnotByObjectNumber(page.get(), added));

  EXPECT_EQ(initial, FPDFPage_GetAnnotCount(page.get()));
  // The page (touched) and the orphaned appearance stream remain promoted;
  // the annotation dictionary is gone.
  EXPECT_EQ(touched - 1, EPDFLayer_GetPromotedObjectCount(doc.layer));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, page_num));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.layer, added));

  DeltaProbe delta = SaveDeltaEx(doc.layer);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, delta.status);
  EXPECT_FALSE(delta.changed_since_load);
  EXPECT_TRUE(delta.bytes.empty());

  StandaloneProbe standalone = SaveStandaloneEx(doc.layer);
  EXPECT_EQ(EPDFSaveStatus_kUnchangedSinceLoad, standalone.status);
  EXPECT_TRUE(standalone.bytes.empty());

  // The cumulative delta an artifact persists is the same nothing: the
  // orphan is unreachable and the page equals its base twin.
  EXPECT_TRUE(SaveDeltaCumulative(doc.layer).empty());
}

// A page that had no /Annots gets none back: the reverse of the add that
// created the array is not an empty array.
TEST_F(FPDFAnnotEmbedderTest, LayerAddThenRemoveOnPageWithoutAnnotsRestoresShape) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh("hello_world.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
  ASSERT_TRUE(page);
  ASSERT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
  ASSERT_FALSE(CPDFPageFromFPDFPage(page.get())->GetDict()->KeyExist("Annots"));

  uint32_t added = 0;
  {
    ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_SQUARE));
    ASSERT_TRUE(annot);
    const FS_RECTF rect{50.0f, 50.0f, 150.0f, 150.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
    added = EPDFAnnot_GetObjectNumber(annot.get());
  }
  ASSERT_TRUE(EPDFPage_RemoveAnnotByObjectNumber(page.get(), added));

  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_FALSE(CPDFPageFromFPDFPage(page.get())->GetDict()->KeyExist("Annots"));
  DeltaProbe delta = SaveDeltaEx(doc.layer);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, delta.status);
  EXPECT_FALSE(delta.changed_since_load);
  EXPECT_TRUE(delta.bytes.empty());
}

// A base annotation edited and restored: touched, not changed. A real edit
// writes the object, and only it.
TEST_F(FPDFAnnotEmbedderTest, LayerEditAndRestoreIsUnchangedSinceLoad) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh(kTwoAnnots));
  ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
  ASSERT_TRUE(page);
  const uint32_t page_num = EPDFPage_GetObjectNumber(page.get());
  const uint32_t first = AnnotObjNumAt(page.get(), 0);
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(annot);
  ASSERT_FALSE(FPDFAnnot_HasKey(annot.get(), kProbeKey));

  SetAnnotString(annot.get(), kProbeKey, L"changed");
  EXPECT_EQ(1ul, EPDFLayer_GetPromotedObjectCount(doc.layer));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, first));
  EXPECT_FALSE(EPDFLayer_IsObjectPromoted(doc.layer, page_num));
  DeltaProbe edited = SaveDeltaEx(doc.layer);
  EXPECT_TRUE(edited.changed_since_load);
  EXPECT_EQ(1, CountObjectWrites(edited.bytes, first));
  EXPECT_EQ(0, CountObjectWrites(edited.bytes, page_num));

  ASSERT_TRUE(EPDFAnnot_RemoveKey(annot.get(), kProbeKey));
  EXPECT_EQ(1ul, EPDFLayer_GetPromotedObjectCount(doc.layer));  // still touched
  DeltaProbe restored = SaveDeltaEx(doc.layer);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, restored.status);
  EXPECT_FALSE(restored.changed_since_load);
  EXPECT_TRUE(restored.bytes.empty());
}

// A reopened layer: its saved edits live in the overlay and differ from the
// base, but not from the loaded document. The two twins disagree exactly
// where a session must not confuse them.
TEST_F(FPDFAnnotEmbedderTest, LayerReopenedTwinsDisagree) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh(kTwoAnnots));
  uint32_t first = 0;
  uint32_t second = 0;
  std::vector<uint8_t> saved;
  {
    ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
    ASSERT_TRUE(page);
    first = AnnotObjNumAt(page.get(), 0);
    second = AnnotObjNumAt(page.get(), 1);
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(annot);
    SetAnnotString(annot.get(), kProbeKey, L"twenty");
    saved = SaveDeltaCumulative(doc.layer);
    ASSERT_EQ(1, CountObjectWrites(saved, first));
  }

  ASSERT_TRUE(doc.Reopen(saved));
  EXPECT_EQ(1ul, EPDFLayer_GetPromotedObjectCount(doc.layer));  // ingested
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, first));
  {
    // Untouched: nothing changed since load, even though the overlay object
    // differs from the base.
    DeltaProbe untouched = SaveDeltaEx(doc.layer);
    EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, untouched.status);
    EXPECT_FALSE(untouched.changed_since_load);
    EXPECT_TRUE(untouched.bytes.empty());
    // The persisted, cumulative delta still carries the saved edit.
    EXPECT_EQ(1, CountObjectWrites(SaveDeltaCumulative(doc.layer), first));
    EXPECT_EQ(EPDFSaveStatus_kUnchangedSinceLoad, SaveStandaloneEx(doc.layer).status);
  }

  ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(annot);

  // Set to the loaded value, and set away then back: unchanged since load.
  SetAnnotString(annot.get(), kProbeKey, L"twenty");
  EXPECT_FALSE(SaveDeltaEx(doc.layer).changed_since_load);
  SetAnnotString(annot.get(), kProbeKey, L"thirty");
  EXPECT_TRUE(SaveDeltaEx(doc.layer).changed_since_load);
  SetAnnotString(annot.get(), kProbeKey, L"twenty");
  EXPECT_FALSE(SaveDeltaEx(doc.layer).changed_since_load);

  // Revert to the BASE value: changed since load (the loaded bytes say
  // twenty), yet nothing to write (the layer equals its base).
  ASSERT_TRUE(EPDFAnnot_RemoveKey(annot.get(), kProbeKey));
  DeltaProbe reverted = SaveDeltaEx(doc.layer);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, reverted.status);
  EXPECT_TRUE(reverted.changed_since_load);
  EXPECT_TRUE(reverted.bytes.empty());
  EXPECT_TRUE(SaveDeltaCumulative(doc.layer).empty());
  StandaloneProbe standalone = SaveStandaloneEx(doc.layer);
  EXPECT_EQ(EPDFSaveStatus_kWritten, standalone.status);
  EXPECT_EQ(doc.bytes, standalone.bytes);  // the base, and no revision after it

  // An edit elsewhere still writes the ingested object: the cumulative delta
  // is judged against the base, never against the loaded document.
  SetAnnotString(annot.get(), kProbeKey, L"twenty");
  ScopedFPDFAnnotation other(FPDFPage_GetAnnot(page.get(), 1));
  ASSERT_TRUE(other);
  SetAnnotString(other.get(), kProbeKey, L"also");
  DeltaProbe both = SaveDeltaEx(doc.layer);
  EXPECT_TRUE(both.changed_since_load);
  EXPECT_EQ(1, CountObjectWrites(both.bytes, first));
  EXPECT_EQ(1, CountObjectWrites(both.bytes, second));
}

// ---------------------------------------------------------------------------
// EmbedPDF: a reopened layer, and the file-backed server paths.
// ---------------------------------------------------------------------------

namespace {

std::string WriteTempFile(const std::string& stem, const std::vector<uint8_t>& bytes) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (stem + "-" + std::to_string(::getpid()) + "-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  return path.string();
}

}  // namespace

// The reviewer's sequence: sign, add, persist, reopen (eviction, restart,
// another replica), remove. The base page had no /Annots; the persisted page
// had one. Removing it restores the BASE shape, so the page is elided and the
// layer equals its base again - changed since load, nothing to write.
TEST_F(FPDFAnnotEmbedderTest, LayerReopenedRemoveOfPersistedAnnotationRestoresBaseShape) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh("hello_world.pdf"));
  std::vector<uint8_t> saved;
  uint32_t added = 0;
  {
    ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
    ASSERT_TRUE(page);
    ASSERT_FALSE(CPDFPageFromFPDFPage(page.get())->GetDict()->KeyExist("Annots"));
    ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_SQUARE));
    ASSERT_TRUE(annot);
    const FS_RECTF rect{50.0f, 50.0f, 150.0f, 150.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
    added = EPDFAnnot_GetObjectNumber(annot.get());
    saved = SaveDeltaCumulative(doc.layer);
    ASSERT_FALSE(saved.empty());
  }
  ASSERT_TRUE(doc.Reopen(saved));
  ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
  ASSERT_TRUE(page);
  ASSERT_EQ(1, FPDFPage_GetAnnotCount(page.get()));

  ASSERT_TRUE(EPDFPage_RemoveAnnotByObjectNumber(page.get(), added));

  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_FALSE(CPDFPageFromFPDFPage(page.get())->GetDict()->KeyExist("Annots"));
  DeltaProbe delta = SaveDeltaEx(doc.layer);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, delta.status);
  EXPECT_TRUE(delta.changed_since_load);
  EXPECT_TRUE(delta.bytes.empty());
  StandaloneProbe standalone = SaveStandaloneEx(doc.layer);
  EXPECT_EQ(EPDFSaveStatus_kWritten, standalone.status);
  EXPECT_EQ(doc.bytes, standalone.bytes);
}

// The FPDF_FILEWRITE variants report the same decision as the buffer ones,
// and write nothing when nothing changed since load.
TEST_F(FPDFAnnotEmbedderTest, LayerFileWriteVariantsReportChange) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh(kTwoAnnots));
  ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(annot);

  SetAnnotString(annot.get(), kProbeKey, L"edited");
  ASSERT_TRUE(EPDFAnnot_RemoveKey(annot.get(), kProbeKey));  // touched, not changed

  EPDFLayerSaveStatus status = EPDFLayerSaveStatus_kSaveFailed;
  FPDF_BOOL changed = true;
  ClearString();
  ASSERT_TRUE(EPDFLayer_SaveDeltaEx(doc.layer, this, &status, &changed));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, status);
  EXPECT_FALSE(changed);
  EXPECT_TRUE(GetString().empty());
  ClearString();
  ASSERT_TRUE(EPDFLayer_SaveLayerArtifactEx(doc.layer, this, &status, &changed));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, status);
  EXPECT_FALSE(changed);
  EXPECT_TRUE(GetString().empty());

  SetAnnotString(annot.get(), kProbeKey, L"edited");  // a real change
  ClearString();
  ASSERT_TRUE(EPDFLayer_SaveLayerArtifactEx(doc.layer, this, &status, &changed));
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, status);
  EXPECT_TRUE(changed);
  EXPECT_FALSE(GetString().empty());
  // The artifact written by the file variant has the shape the buffer
  // variant writes: same size, same header up to the base identity (each
  // save mints a fresh trailer /ID, so the delta hash and bytes differ).
  const std::string from_file = GetString();
  unsigned long size = 0;
  EPDFLayerSaveStatus buffer_status = EPDFLayerSaveStatus_kSaveFailed;
  void* buffer = EPDFLayer_SaveLayerArtifactToOwnedBuffer(doc.layer, &size, &buffer_status);
  ASSERT_TRUE(buffer);
  const std::string from_buffer(static_cast<const char*>(buffer), size);
  EPDF_FreeBuffer(buffer);
  EXPECT_EQ(from_buffer.size(), from_file.size());
  constexpr size_t kUpToBaseSha = 8 + 4 + 4 + 8 + 8 + 8 + 32;
  EXPECT_EQ(from_buffer.substr(0, kUpToBaseSha), from_file.substr(0, kUpToBaseSha));
  EXPECT_NE(std::string::npos, from_file.find("EPDFTwinProbe(edited)"));
}

// An artifact opened from a path is read in place: the streams the delta
// carries are views into the file, not copies, in the overlay and in the twin.
TEST_F(FPDFAnnotEmbedderTest, LayerArtifactOpensFromPathWithoutCopyingStreams) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh("hello_world.pdf"));
  uint32_t added = 0;
  std::string artifact;
  {
    ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
    ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_SQUARE));
    const FS_RECTF rect{50.0f, 50.0f, 150.0f, 150.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
    added = EPDFAnnot_GetObjectNumber(annot.get());
    EPDFLayerSaveStatus status = EPDFLayerSaveStatus_kSaveFailed;
    ClearString();
    ASSERT_TRUE(EPDFLayer_SaveLayerArtifact(doc.layer, this, &status));
    artifact = GetString();
    ASSERT_FALSE(artifact.empty());
  }
  const std::string path =
      WriteTempFile("epdf-artifact", std::vector<uint8_t>(artifact.begin(), artifact.end()));
  FPDF_CloseDocument(doc.layer);
  EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
  doc.layer = EPDFLayer_OpenLayerArtifactFromPath(doc.base, path.c_str(), nullptr, &status);
  ASSERT_TRUE(doc.layer);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);

  ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
  ASSERT_TRUE(page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
  EXPECT_EQ(added, AnnotObjNumAt(page.get(), 0));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, added));

  // The appearance stream the delta carried: a view into the artifact file.
  CPDF_Document* layer_doc = CPDFDocumentFromFPDFDocument(doc.layer);
  RetainPtr<const CPDF_Dictionary> annot_dict =
      ToDictionary(layer_doc->GetIndirectObject(added));
  ASSERT_TRUE(annot_dict);
  RetainPtr<const CPDF_Stream> ap =
      annot_dict->GetDictFor("AP")->GetStreamFor("N");
  ASSERT_TRUE(ap);
  EXPECT_TRUE(ap->IsFileBased());
  EXPECT_TRUE(ap->BackingView());

  // Nothing changed since load: the reporting saves write nothing.
  DeltaProbe delta = SaveDeltaEx(doc.layer);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, delta.status);
  EXPECT_FALSE(delta.changed_since_load);
  EXPECT_TRUE(delta.bytes.empty());
  std::filesystem::remove(path);
}

// base ⊕ delta composed from a file on disk: what a working-copy analysis
// opens on the server without buffering the delta.
TEST_F(FPDFAnnotEmbedderTest, BaseOverlayOpensFromPath) {
  LayerFixture doc;
  ASSERT_TRUE(doc.OpenFresh("hello_world.pdf"));
  {
    ScopedFPDFPage page(FPDF_LoadPage(doc.layer, 0));
    ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_SQUARE));
    const FS_RECTF rect{50.0f, 50.0f, 150.0f, 150.0f};
    ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  }
  const std::vector<uint8_t> delta = SaveDeltaCumulative(doc.layer);
  ASSERT_FALSE(delta.empty());
  const std::string path = WriteTempFile("epdf-delta", delta);
  FPDF_DOCUMENT overlay = EPDFDoc_OpenBaseOverlayFromPath(doc.layer, path.c_str());
  ASSERT_TRUE(overlay);
  EXPECT_EQ(1, FPDF_GetPageCount(overlay));
  ScopedFPDFPage page(FPDF_LoadPage(overlay, 0));
  ASSERT_TRUE(page);
  EXPECT_EQ(1, FPDFPage_GetAnnotCount(page.get()));
  FPDF_CloseDocument(overlay);
  EXPECT_FALSE(EPDFDoc_OpenBaseOverlayFromPath(doc.layer, "/nonexistent/epdf-delta"));
  std::filesystem::remove(path);
}
