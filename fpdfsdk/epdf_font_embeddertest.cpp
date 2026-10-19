// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_font.h"

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "core/fdrm/fx_crypt_sha.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_annot.h"
#include "public/epdf_form.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "testing/embedder_test.h"
#include "testing/embedpdf_form_writes.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

namespace {

class ScopedRegisteredFonts {
 public:
  ScopedRegisteredFonts() { EPDFFont_ClearRegisteredFonts(); }
  ~ScopedRegisteredFonts() { EPDFFont_ClearRegisteredFonts(); }
};

std::vector<uint8_t> LoadThirdPartyFont(const char* path) {
  std::string font_path = PathService::GetThirdPartyFilePath(path);
  if (font_path.empty()) {
    ADD_FAILURE() << "Failed to find " << path;
    return {};
  }
  return GetFileContents(font_path.c_str());
}

std::vector<uint8_t> LoadRoboto() {
  return LoadThirdPartyFont("harfbuzz/src/perf/fonts/Roboto-Regular.ttf");
}

std::vector<uint8_t> LoadDroidSansFallback() {
  std::string font_path =
      PathService::GetTestFilePath("fonts/DroidSansFallbackFull.ttf");
  if (font_path.empty()) {
    ADD_FAILURE() << "Failed to find DroidSansFallbackFull";
    return {};
  }
  return GetFileContents(font_path.c_str());
}

// Roboto with its OS/2 fsType set to preview-and-print.
std::vector<uint8_t> RobotoPreviewAndPrint() {
  std::vector<uint8_t> font = LoadRoboto();
  if (font.size() < 12) {
    return {};
  }
  const size_t tables = (font[4] << 8) | font[5];
  for (size_t i = 0; i < tables; ++i) {
    const size_t record = 12 + 16 * i;
    if (record + 16 > font.size()) {
      break;
    }
    if (font[record] == 'O' && font[record + 1] == 'S' &&
        font[record + 2] == '/' && font[record + 3] == '2') {
      const size_t offset = (static_cast<size_t>(font[record + 8]) << 24) |
                            (font[record + 9] << 16) |
                            (font[record + 10] << 8) | font[record + 11];
      font[offset + 8] = 0x00;
      font[offset + 9] = 0x04;  // fsType: preview-and-print
      return font;
    }
  }
  return {};
}

std::wstring Utf16Of(std::vector<FPDF_WCHAR> buffer) {
  return GetPlatformWString(buffer.data());
}

std::wstring FaceFamily(EPDF_FONT_FACES faces, int face) {
  const unsigned long length = EPDFFontFaces_GetFamily(faces, face, nullptr, 0);
  if (length == 0) {
    return L"";
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  EPDFFontFaces_GetFamily(faces, face, buffer.data(), length);
  return Utf16Of(std::move(buffer));
}

std::string ProgramPostScriptName(EPDF_FONT_FACES faces,
                                  int face,
                                  int program) {
  const unsigned long length =
      EPDFFontFaces_GetProgramPostScriptName(faces, face, program, nullptr, 0);
  if (length == 0) {
    return "";
  }
  std::vector<char> buffer(length);
  EPDFFontFaces_GetProgramPostScriptName(faces, face, program, buffer.data(),
                                         length);
  return std::string(buffer.data());
}

struct FoundProgram {
  int face = -1;
  int program = -1;
};

// The first program of the first face named |family|.
std::optional<FoundProgram> FindProgram(EPDF_FONT_FACES faces,
                                        const std::wstring& family) {
  for (int face = 0; face < EPDFFontFaces_Count(faces); ++face) {
    if (FaceFamily(faces, face) != family) {
      continue;
    }
    if (EPDFFontFaces_CountPrograms(faces, face) > 0) {
      return FoundProgram{face, 0};
    }
  }
  return std::nullopt;
}

bool HasFace(EPDF_FONT_FACES faces, const std::wstring& family) {
  for (int face = 0; face < EPDFFontFaces_Count(faces); ++face) {
    if (FaceFamily(faces, face) == family) {
      return true;
    }
  }
  return false;
}

std::vector<std::wstring> MissingCharacters(FPDF_DOCUMENT document) {
  std::vector<std::wstring> missing;
  for (int i = 0; i < EPDFDoc_CountMissingCharacters(document); ++i) {
    const unsigned long length =
        EPDFDoc_GetMissingCharacter(document, i, nullptr, 0);
    std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
    EPDFDoc_GetMissingCharacter(document, i, buffer.data(), length);
    missing.push_back(Utf16Of(std::move(buffer)));
  }
  return missing;
}

ByteString DefaultAppearanceFontName(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  const ByteString da = context->GetAnnotDict()->GetByteStringFor("DA");
  // "0 g /EDocF12 12 Tf": the name after the slash.
  std::optional<size_t> slash = da.Find('/');
  if (!slash.has_value()) {
    return ByteString();
  }
  ByteString rest = da.Substr(*slash + 1);
  std::optional<size_t> space = rest.Find(' ');
  return space.has_value() ? rest.First(*space) : rest;
}

// The object number of the program the appearance's |alias| font embeds.
uint32_t AppearanceFontProgram(FPDF_ANNOTATION annot, const ByteString& alias) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  RetainPtr<const CPDF_Dictionary> ap =
      context->GetAnnotDict()->GetDictFor("AP");
  RetainPtr<const CPDF_Stream> normal = ap ? ap->GetStreamFor("N") : nullptr;
  RetainPtr<const CPDF_Dictionary> resources =
      normal ? normal->GetDict()->GetDictFor("Resources") : nullptr;
  RetainPtr<const CPDF_Dictionary> fonts =
      resources ? resources->GetDictFor("Font") : nullptr;
  RetainPtr<const CPDF_Dictionary> font =
      fonts ? fonts->GetDictFor(alias.AsStringView()) : nullptr;
  if (!font || font->GetNameFor("Subtype") != "Type0") {
    return 0;
  }
  RetainPtr<const CPDF_Array> descendants =
      font->GetArrayFor("DescendantFonts");
  RetainPtr<const CPDF_Dictionary> descendant =
      descendants ? descendants->GetDictAt(0) : nullptr;
  RetainPtr<const CPDF_Dictionary> descriptor =
      descendant ? descendant->GetDictFor("FontDescriptor") : nullptr;
  RetainPtr<const CPDF_Stream> program =
      descriptor ? descriptor->GetStreamFor("FontFile2") : nullptr;
  return program ? program->GetObjNum() : 0;
}

class EPDFFontEmbedderTest : public EmbedderTest {
 protected:
  // A page with a text field whose widget writes with |font_id|, filled
  // with |value|: under the default embedding policy, the font's whole
  // program goes into the document.
  uint32_t AddRegisteredFontField(FPDF_PAGE page,
                                  EPDF_FONT_ID font_id,
                                  const wchar_t* name,
                                  float top,
                                  const std::wstring& value) {
    ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page, FPDF_ANNOT_WIDGET));
    EXPECT_TRUE(annot);
    const FS_RECTF rect{50.0f, top, 350.0f, top - 30.0f};
    EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
    EXPECT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(
        annot.get(), font_id, 12.0f, 0, 0, 0));
    return AttachAndFill(annot.get(), name, value);
  }

  uint32_t AttachAndFill(FPDF_ANNOTATION annot,
                         const wchar_t* name,
                         const std::wstring& value) {
    ScopedFPDFWideString field_name = GetFPDFWideString(name);
    const uint32_t field = EPDFForm_CreateField(
        document(), 4 /* EPDF_FORMFIELD_FAMILY_TEXT */, field_name.get(), 0);
    EXPECT_GT(field, 0u);
    EXPECT_TRUE(EPDFForm_AttachWidget(
        document(), field, EPDFAnnot_GetObjectNumber(annot), nullptr, 0));
    EXPECT_TRUE(embedpdf_test::FillTextField(document(), field, value));
    EXPECT_TRUE(EPDFAnnot_GenerateFormFieldAP(annot));
    return field;
  }
};

}  // namespace

// The registry hands out a font's program as the writer embeds it, its
// hash, the characters it maps and the order fallbacks are tried in.
TEST_F(EPDFFontEmbedderTest, RegisteredFontProgramCoverageAndFallbackOrder) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRoboto();
  ASSERT_FALSE(roboto.empty());
  const EPDF_FONT_ID regular = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  const EPDF_FONT_ID bold =
      EPDFFont_RegisterMemFont64("Roboto Heavy", /*weight=*/700, /*italic=*/0,
                                 roboto.data(), roboto.size());
  ASSERT_NE(0u, regular);
  ASSERT_NE(0u, bold);

  ASSERT_EQ(roboto.size(), EPDFFont_GetProgramData(regular, nullptr, 0));
  std::vector<uint8_t> bytes(roboto.size());
  ASSERT_EQ(roboto.size(),
            EPDFFont_GetProgramData(regular, bytes.data(), bytes.size()));
  EXPECT_EQ(roboto, bytes);

  unsigned char hash[32] = {};
  ASSERT_TRUE(EPDFFont_GetProgramHash(regular, hash));
  const DataVector<uint8_t> expected = CryptSha256Generate(roboto);
  EXPECT_EQ(std::vector<uint8_t>(expected.begin(), expected.end()),
            std::vector<uint8_t>(hash, hash + 32));
  EXPECT_FALSE(EPDFFont_GetProgramHash(9999, hash));

  const unsigned long range_count = EPDFFont_GetCoverage(regular, nullptr, 0);
  ASSERT_GT(range_count, 0u);
  std::vector<uint32_t> ranges(range_count * 2);
  ASSERT_EQ(range_count,
            EPDFFont_GetCoverage(regular, ranges.data(), range_count));
  bool covers_a = false;
  for (unsigned long i = 0; i < range_count; ++i) {
    covers_a |= ranges[2 * i] <= 'A' && 'A' < ranges[2 * i] + ranges[2 * i + 1];
  }
  EXPECT_TRUE(covers_a);

  float ascent = 0;
  float descent = 0;
  ASSERT_TRUE(EPDFFont_GetMetrics(regular, &ascent, &descent));
  EXPECT_GT(ascent, 0.5f);
  EXPECT_GT(descent, 0.0f);
  EXPECT_GT(EPDFFont_GetPostScriptName(regular, nullptr, 0), 1u);
  EXPECT_EQ(EPDF_FONT_FORMAT_TRUETYPE, EPDFFont_GetProgramFormat(regular));
  EXPECT_EQ(-1, EPDFFont_GetProgramFormat(9999));

  ASSERT_TRUE(EPDFFont_AddFallbackFont(regular));
  ASSERT_TRUE(EPDFFont_AddFallbackFont(bold));
  EPDF_FONT_ID order[2] = {};
  ASSERT_EQ(2u, EPDFFont_GetFallbackOrder(700, false, order, 2));
  EXPECT_EQ(bold, order[0]);
  EXPECT_EQ(regular, order[1]);
  ASSERT_EQ(2u, EPDFFont_GetFallbackOrder(400, false, order, 2));
  EXPECT_EQ(regular, order[0]);
  EXPECT_EQ(bold, order[1]);
}

// A whole program the document holds is listed with its facts, and its bytes
// and coverage are handed out by object number; nothing else is.
TEST_F(EPDFFontEmbedderTest, ListsADocumentProgramAndHandsOutItsBytes) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRoboto();
  ASSERT_FALSE(roboto.empty());
  const EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);

  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 400, 400));
  ASSERT_TRUE(page);
  AddRegisteredFontField(page.get(), roboto_id, L"name", 320.0f, L"hello");

  EPDF_FONT_FACES faces = EPDFFontFaces_Load(document());
  ASSERT_TRUE(faces);
  std::optional<FoundProgram> found = FindProgram(faces, L"Roboto");
  ASSERT_TRUE(found.has_value());
  const unsigned long objnum =
      EPDFFontFaces_GetProgramObjNum(faces, found->face, found->program);
  EXPECT_NE(0u, objnum);
  EXPECT_EQ(400, EPDFFontFaces_GetWeight(faces, found->face));
  EXPECT_FALSE(EPDFFontFaces_IsItalic(faces, found->face));
  EXPECT_EQ(EPDF_FONT_FORMAT_TRUETYPE,
            EPDFFontFaces_GetProgramFormat(faces, found->face, found->program));
  EXPECT_EQ(EPDF_FONT_EMBEDDED_WHOLE, EPDFFontFaces_GetProgramEmbedded(
                                          faces, found->face, found->program));
  EXPECT_EQ(EPDF_FONT_EMBEDDING_INSTALLABLE,
            EPDFFontFaces_GetProgramEmbeddingPermission(faces, found->face,
                                                        found->program));
  EXPECT_TRUE(
      EPDFFontFaces_ProgramMapsUnicode(faces, found->face, found->program));
  EXPECT_FALSE(
      ProgramPostScriptName(faces, found->face, found->program).empty());
  unsigned char hash[32] = {};
  unsigned char registered_hash[32] = {};
  ASSERT_TRUE(
      EPDFFontFaces_GetProgramHash(faces, found->face, found->program, hash));
  ASSERT_TRUE(EPDFFont_GetProgramHash(roboto_id, registered_hash));
  EXPECT_EQ(std::vector<uint8_t>(registered_hash, registered_hash + 32),
            std::vector<uint8_t>(hash, hash + 32));
  float ascent = 0;
  float descent = 0;
  EXPECT_TRUE(EPDFFontFaces_GetProgramMetrics(
      faces, found->face, found->program, &ascent, &descent));
  EPDFFontFaces_Close(faces);

  ASSERT_EQ(roboto.size(),
            EPDFDoc_GetFontProgramData(document(), objnum, nullptr, 0));
  std::vector<uint8_t> bytes(roboto.size());
  ASSERT_EQ(roboto.size(), EPDFDoc_GetFontProgramData(
                               document(), objnum, bytes.data(), bytes.size()));
  EXPECT_EQ(roboto, bytes);
  EXPECT_GT(EPDFDoc_GetFontProgramCoverage(document(), objnum, nullptr, 0), 0u);
  // The catalog is an object, not a font program.
  EXPECT_EQ(0u, EPDFDoc_GetFontProgramData(document(), 1, nullptr, 0));
}

// A widget writes with a document program by its object number: /DA names a
// session alias, the appearance embeds that very program, characters it
// lacks are reported, and the box's font reads back as that program.
TEST_F(EPDFFontEmbedderTest, WidgetWritesWithADocumentProgram) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRoboto();
  ASSERT_FALSE(roboto.empty());
  const EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);

  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 400, 400));
  ASSERT_TRUE(page);
  AddRegisteredFontField(page.get(), roboto_id, L"first", 320.0f, L"hello");
  EPDF_FONT_FACES faces = EPDFFontFaces_Load(document());
  std::optional<FoundProgram> found = FindProgram(faces, L"Roboto");
  ASSERT_TRUE(found.has_value());
  const unsigned long objnum =
      EPDFFontFaces_GetProgramObjNum(faces, found->face, found->program);
  const std::string postscript_name =
      ProgramPostScriptName(faces, found->face, found->program);
  EPDFFontFaces_Close(faces);

  // The font is the document's alone now.
  EPDFFont_ClearRegisteredFonts();

  ScopedFPDFAnnotation annot(
      EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_WIDGET));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 250.0f, 350.0f, 220.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  EXPECT_FALSE(
      EPDFAnnot_SetDefaultAppearanceDocumentFont(annot.get(), 1, 12, 0, 0, 0));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceDocumentFont(annot.get(), objnum,
                                                         12, 0, 0, 0));
  const ByteString alias = DefaultAppearanceFontName(annot.get());
  EXPECT_EQ(ByteString::Format("EDocF%lu", objnum), alias);

  EPDFDoc_ClearMissingCharacters(document());
  const uint32_t second =
      AttachAndFill(annot.get(), L"second", L"\x03A9 and \x4E2D");
  EXPECT_EQ(objnum, AppearanceFontProgram(annot.get(), alias));
  EXPECT_EQ(std::vector<std::wstring>{L"\x4E2D"},
            MissingCharacters(document()));

  const unsigned long name_length =
      EPDFAnnot_GetDefaultAppearanceFontPostScriptName(annot.get(), nullptr, 0);
  ASSERT_GT(name_length, 1u);
  std::vector<char> name(name_length);
  EPDFAnnot_GetDefaultAppearanceFontPostScriptName(annot.get(), name.data(),
                                                   name_length);
  EXPECT_EQ(postscript_name, std::string(name.data()));

  int weight = 0;
  FPDF_BOOL italic = true;
  const unsigned long family_length = EPDFAnnot_GetDefaultAppearanceFontFace(
      annot.get(), nullptr, 0, &weight, &italic);
  std::vector<FPDF_WCHAR> family = GetFPDFWideStringBuffer(family_length);
  EPDFAnnot_GetDefaultAppearanceFontFace(annot.get(), family.data(),
                                         family_length, &weight, &italic);
  EXPECT_EQ(L"Roboto", Utf16Of(std::move(family)));
  EXPECT_EQ(400, weight);
  EXPECT_FALSE(italic);

  // Once the appearance exists, /DR holds the composite font over the
  // program, and a new value with a character no earlier one used lays out
  // with the program's own cmap.
  RetainPtr<const CPDF_Dictionary> acroform =
      CPDFDocumentFromFPDFDocument(document())
          ->GetRoot()
          ->GetDictFor("AcroForm");
  RetainPtr<const CPDF_Dictionary> dr_font =
      acroform->GetDictFor("DR")->GetDictFor("Font")->GetDictFor(
          alias.AsStringView());
  ASSERT_TRUE(dr_font);
  EXPECT_EQ("Type0", dr_font->GetNameFor("Subtype"));
  EPDFDoc_ClearMissingCharacters(document());
  ASSERT_TRUE(embedpdf_test::FillTextField(document(), second, L"\x03A8"));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(annot.get()));
  EXPECT_EQ(objnum, AppearanceFontProgram(annot.get(), alias));
  EXPECT_TRUE(MissingCharacters(document()).empty());
}

// What a write would draw with, without writing: the standard face, a
// registered font, a substitute, the fallbacks in order, and the characters
// no font has.
TEST_F(EPDFFontEmbedderTest, ResolvesTheFontsAWriteWouldUse) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRoboto();
  std::vector<uint8_t> droid = LoadDroidSansFallback();
  ASSERT_FALSE(roboto.empty());
  ASSERT_FALSE(droid.empty());
  const EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  const EPDF_FONT_ID droid_id =
      EPDFFont_RegisterMemFont64("DroidSansFallbackFull", /*weight=*/400,
                                 /*italic=*/0, droid.data(), droid.size());
  ASSERT_NE(0u, roboto_id);
  ASSERT_NE(0u, droid_id);
  ASSERT_TRUE(EPDFFont_AddFallbackFont(droid_id));

  CreateEmptyDocument();
  ScopedFPDFWideString text0 = GetFPDFWideString(L"Hello \x4E2D");
  ScopedFPDFWideString text1 = GetFPDFWideString(L"Roboto here");
  ScopedFPDFWideString text2 = GetFPDFWideString(L"X");
  ScopedFPDFWideString family0 = GetFPDFWideString(L"Helvetica");
  ScopedFPDFWideString family1 = GetFPDFWideString(L"Roboto");
  ScopedFPDFWideString family2 = GetFPDFWideString(L"NoSuchFamily");
  FPDF_WIDESTRING texts[] = {text0.get(), text1.get(), text2.get()};
  FPDF_WIDESTRING families[] = {family0.get(), family1.get(), family2.get()};
  const int weights[] = {400, 400, 700};
  const FPDF_BOOL italics[] = {false, false, false};

  EPDF_FONT_RESOLUTION resolution = EPDFDoc_ResolveFonts(
      document(), nullptr, 3, texts, families, weights, italics);
  ASSERT_TRUE(resolution);
  EXPECT_EQ(EPDF_FONT_SOURCE_STANDARD,
            EPDFFontResolution_GetSource(resolution, 0));
  char base_font[64] = {};
  EPDFFontResolution_GetBaseFontName(resolution, 0, base_font,
                                     sizeof(base_font));
  EXPECT_STREQ("Helvetica", base_font);
  EXPECT_FALSE(EPDFFontResolution_IsDegraded(resolution, 0));
  EPDF_FONT_ID fallbacks[2] = {};
  ASSERT_EQ(1u, EPDFFontResolution_GetFallbacks(resolution, 0, fallbacks, 2));
  EXPECT_EQ(droid_id, fallbacks[0]);

  EXPECT_EQ(EPDF_FONT_SOURCE_REGISTERED,
            EPDFFontResolution_GetSource(resolution, 1));
  EXPECT_EQ(roboto_id, EPDFFontResolution_GetRegisteredFont(resolution, 1));

  EXPECT_EQ(EPDF_FONT_SOURCE_STANDARD,
            EPDFFontResolution_GetSource(resolution, 2));
  EXPECT_TRUE(EPDFFontResolution_IsDegraded(resolution, 2));
  EXPECT_EQ(700, EPDFFontResolution_GetWeight(resolution, 2));
  EXPECT_EQ(0, EPDFFontResolution_CountMissingCharacters(resolution));
  EPDFFontResolution_Close(resolution);

  // Without the fallback, the CJK character has no font.
  EPDFFont_ClearFallbackFonts();
  resolution = EPDFDoc_ResolveFonts(document(), nullptr, 1, texts, families,
                                    weights, italics);
  ASSERT_TRUE(resolution);
  EXPECT_EQ(0u, EPDFFontResolution_GetFallbacks(resolution, 0, nullptr, 0));
  ASSERT_EQ(1, EPDFFontResolution_CountMissingCharacters(resolution));
  const unsigned long length =
      EPDFFontResolution_GetMissingCharacter(resolution, 0, nullptr, 0);
  std::vector<FPDF_WCHAR> missing = GetFPDFWideStringBuffer(length);
  EPDFFontResolution_GetMissingCharacter(resolution, 0, missing.data(), length);
  EXPECT_EQ(L"\x4E2D", Utf16Of(std::move(missing)));
  EPDFFontResolution_Close(resolution);
}

// A family the document holds as a whole program, and nobody registered,
// resolves to that program.
TEST_F(EPDFFontEmbedderTest, ResolvesADocumentProgram) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> roboto = LoadRoboto();
  ASSERT_FALSE(roboto.empty());
  const EPDF_FONT_ID roboto_id = EPDFFont_RegisterMemFont64(
      "Roboto", /*weight=*/400, /*italic=*/0, roboto.data(), roboto.size());
  ASSERT_NE(0u, roboto_id);
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 400, 400));
  ASSERT_TRUE(page);
  AddRegisteredFontField(page.get(), roboto_id, L"name", 320.0f, L"hello");
  EPDF_FONT_FACES faces = EPDFFontFaces_Load(document());
  std::optional<FoundProgram> found = FindProgram(faces, L"Roboto");
  ASSERT_TRUE(found.has_value());
  const unsigned long objnum =
      EPDFFontFaces_GetProgramObjNum(faces, found->face, found->program);
  EPDFFontFaces_Close(faces);
  EPDFFont_ClearRegisteredFonts();

  ScopedFPDFWideString text = GetFPDFWideString(L"Hi");
  ScopedFPDFWideString family = GetFPDFWideString(L"Roboto");
  FPDF_WIDESTRING texts[] = {text.get()};
  FPDF_WIDESTRING families[] = {family.get()};
  const int weights[] = {400};
  const FPDF_BOOL italics[] = {false};
  EPDF_FONT_RESOLUTION resolution = EPDFDoc_ResolveFonts(
      document(), nullptr, 1, texts, families, weights, italics);
  ASSERT_TRUE(resolution);
  EXPECT_EQ(EPDF_FONT_SOURCE_DOCUMENT_PROGRAM,
            EPDFFontResolution_GetSource(resolution, 0));
  EXPECT_EQ(objnum, EPDFFontResolution_GetProgramObjNum(resolution, 0));
  EPDFFontResolution_Close(resolution);
}

// The walk finds fonts wherever a drawing can use them: inherited page
// resources, a form XObject (that names itself), every appearance state, a
// field's own /DR, the form's /DR, and a Type 3 font's resources.
TEST_F(EPDFFontEmbedderTest, FindsFontsWhereverADrawingUsesThem) {
  const std::string pdf = MakePdf({
      "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [8 0 R] "
      "/DR << /Font << /FDR 9 0 R >> >> >> >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 /Resources << /Font << /FInh 10 "
      "0 R /FT3 14 0 R >> /XObject << /X1 4 0 R >> >> >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [5 0 R] "
      ">>",
      "<< /Type /XObject /Subtype /Form /BBox [0 0 10 10] /Resources << /Font "
      "<< /FX 11 0 R >> /XObject << /Self 4 0 R >> >> /Length 0 >>\n"
      "stream\n\nendstream",
      "<< /Type /Annot /Subtype /Square /Rect [10 10 50 50] /AP << /N << /On "
      "6 0 R /Off 7 0 R >> >> >>",
      "<< /BBox [0 0 10 10] /Resources << /Font << /FOn 12 0 R >> >> /Length "
      "0 >>\nstream\n\nendstream",
      Stream(""),
      "<< /T (field) /FT /Tx /DR << /Font << /FField 13 0 R >> >> >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Symbol >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /ZapfDingbats >>",
      "<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1 1] /FontMatrix [1 0 0 1 "
      "0 0] /CharProcs << >> /Encoding << /Differences [] >> /FirstChar 0 "
      "/LastChar 0 /Widths [0] /Resources << /Font << /FG 15 0 R >> >> >>",
      "<< /Type /Font /Subtype /TrueType /BaseFont /Georgia >>",
  });
  ScopedFPDFDocument doc(FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
  ASSERT_TRUE(doc);
  EPDF_FONT_FACES faces = EPDFFontFaces_Load(doc.get());
  ASSERT_TRUE(faces);
  for (const wchar_t* family : {L"Courier", L"Helvetica", L"Times", L"Symbol",
                                L"ZapfDingbats", L"Georgia"}) {
    EXPECT_TRUE(HasFace(faces, family)) << family;
  }
  std::optional<FoundProgram> georgia = FindProgram(faces, L"Georgia");
  ASSERT_TRUE(georgia.has_value());
  EXPECT_EQ(EPDF_FONT_FORMAT_NONE, EPDFFontFaces_GetProgramFormat(
                                       faces, georgia->face, georgia->program));
  EXPECT_EQ(
      EPDF_FONT_EMBEDDED_NONE,
      EPDFFontFaces_GetProgramEmbedded(faces, georgia->face, georgia->program));
  EXPECT_EQ(0u, EPDFFontFaces_GetProgramObjNum(faces, georgia->face,
                                               georgia->program));
  EXPECT_EQ(-1, EPDFFontFaces_GetProgramEmbeddingPermission(
                    faces, georgia->face, georgia->program));
  unsigned char hash[32] = {};
  EXPECT_FALSE(EPDFFontFaces_GetProgramHash(faces, georgia->face,
                                            georgia->program, hash));
  bool type3 = false;
  for (int face = 0; face < EPDFFontFaces_Count(faces); ++face) {
    for (int program = 0; program < EPDFFontFaces_CountPrograms(faces, face);
         ++program) {
      type3 |= EPDFFontFaces_GetProgramFormat(faces, face, program) ==
               EPDF_FONT_FORMAT_TYPE3;
    }
  }
  EXPECT_TRUE(type3);
  EPDFFontFaces_Close(faces);
}

// A free text drawn with no font for a character reports it once; marks,
// line breaks and zero width characters are never reported.
TEST_F(EPDFFontEmbedderTest, FreeTextReportsCharactersNoFontHas) {
  ScopedRegisteredFonts scoped_fonts;
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_FREETEXT));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 250.0f, 350.0f, 320.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ScopedFPDFWideString contents = GetFPDFWideString(L"a\x4E2D\x200B b\r\x4E2D");
  ASSERT_TRUE(
      FPDFAnnot_SetStringValue(annot.get(), "Contents", contents.get()));
  EPDFDoc_ClearMissingCharacters(document());
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  EXPECT_EQ(std::vector<std::wstring>{L"\x4E2D"},
            MissingCharacters(document()));
  EPDFDoc_ClearMissingCharacters(document());
  EXPECT_EQ(0, EPDFDoc_CountMissingCharacters(document()));
}

// A registered font not authorized for editing never lays out a box's new
// text, even when the box's /DR font was written with it: the box keeps the
// font dictionary it has.
TEST_F(EPDFFontEmbedderTest, UnauthorizedRegisteredFontDoesNotTakeOverABox) {
  ScopedRegisteredFonts scoped_fonts;
  std::vector<uint8_t> font = RobotoPreviewAndPrint();
  ASSERT_FALSE(font.empty());
  EPDF_FONT_ID font_id = EPDFFont_RegisterMemFont64(
      "RobotoPP", /*weight=*/400, /*italic=*/0, font.data(), font.size());
  ASSERT_NE(0u, font_id);
  ASSERT_EQ(EPDF_FONT_EMBEDDING_PREVIEW_AND_PRINT,
            EPDFFont_GetEmbeddingPermission(font_id));
  ASSERT_TRUE(EPDFFont_AuthorizeEditing(font_id));

  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 400, 400));
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation annot(
      EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_WIDGET));
  ASSERT_TRUE(annot);
  const FS_RECTF rect{50.0f, 320.0f, 350.0f, 290.0f};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ASSERT_TRUE(EPDFAnnot_SetDefaultAppearanceRegisteredFont(annot.get(), font_id,
                                                           12, 0, 0, 0));
  AttachAndFill(annot.get(), L"name", L"hello");

  // The same font again, this time not authorized.
  EPDFFont_ClearRegisteredFonts();
  font_id = EPDFFont_RegisterMemFont64("RobotoPP", /*weight=*/400,
                                       /*italic=*/0, font.data(), font.size());
  ASSERT_NE(0u, font_id);
  ASSERT_FALSE(EPDFFont_IsEditingAuthorized(font_id));

  ScopedFPDFWideString text = GetFPDFWideString(L"hello");
  ScopedFPDFWideString family = GetFPDFWideString(L"RobotoPP");
  FPDF_WIDESTRING texts[] = {text.get()};
  FPDF_WIDESTRING families[] = {family.get()};
  const int weights[] = {400};
  const FPDF_BOOL italics[] = {false};
  EPDF_FONT_RESOLUTION resolution = EPDFDoc_ResolveFonts(
      document(), annot.get(), 1, texts, families, weights, italics);
  ASSERT_TRUE(resolution);
  EXPECT_EQ(EPDF_FONT_SOURCE_DOCUMENT_FONT,
            EPDFFontResolution_GetSource(resolution, 0));
  EPDFFontResolution_Close(resolution);
}
