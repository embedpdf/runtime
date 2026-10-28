// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_ppo.h"

#include <map>
#include <string>
#include <vector>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_form.h"
#include "public/epdf_layer.h"
#include "public/epdf_save.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

namespace {

// form_page_import.pdf's objects.
constexpr uint32_t kPerson = 5;
constexpr uint32_t kEmail = 8;
constexpr uint32_t kPlan = 10;
constexpr uint32_t kAgree = 12;

// A field as the form model reads it.
struct Field {
  std::wstring value;
  int widgets = 0;
  bool declared = false;  // in /AcroForm /Fields, not found on a page only

  bool operator==(const Field& other) const {
    return value == other.value && widgets == other.widgets &&
           declared == other.declared;
  }
};

std::map<std::wstring, Field> ReadFields(FPDF_DOCUMENT document) {
  std::map<std::wstring, Field> fields;
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document);
  EXPECT_TRUE(model);
  if (!model) {
    return fields;
  }
  for (int i = 0; i < EPDFForm_CountFields(model); ++i) {
    unsigned long length = EPDFForm_GetFieldName(model, i, nullptr, 0);
    std::vector<FPDF_WCHAR> name = GetFPDFWideStringBuffer(length);
    EPDFForm_GetFieldName(model, i, name.data(), length);
    Field field;
    if (EPDFForm_CountFieldValues(model, i) > 0) {
      length = EPDFForm_GetFieldValueAt(model, i, 0, nullptr, 0);
      std::vector<FPDF_WCHAR> value = GetFPDFWideStringBuffer(length);
      EPDFForm_GetFieldValueAt(model, i, 0, value.data(), length);
      field.value = GetPlatformWString(value.data());
    }
    field.widgets = EPDFForm_CountFieldWidgets(model, i);
    field.declared =
        EPDFForm_GetFieldOrigin(model, i) == EPDF_FORMFIELD_ORIGIN_ACROFORM;
    fields[GetPlatformWString(name.data())] = field;
  }
  EPDFForm_CloseModel(model);
  return fields;
}

// What form_page_import.pdf's pages bring, all of them.
std::map<std::wstring, Field> AllPagesFields() {
  return {
      {L"person.name", {L"Ann", 1, true}},
      {L"person.email", {L"ann@example.com", 1, true}},
      {L"plan", {L"yearly", 2, true}},
      {L"agree", {L"Yes", 1, true}},
  };
}

std::wstring PartialName(FPDF_DOCUMENT document, uint32_t objnum) {
  const unsigned long length =
      EPDFForm_GetFieldPartialName(document, objnum, nullptr, 0);
  if (length == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length, EPDFForm_GetFieldPartialName(document, objnum,
                                                 buffer.data(), length));
  return GetPlatformWString(buffer.data());
}

std::vector<uint32_t> ImportRoots(FPDF_DOCUMENT document,
                                  const std::vector<int>& pages) {
  const int* indices = pages.empty() ? nullptr : pages.data();
  const unsigned long count =
      EPDFDoc_GetImportFieldRoots(document, indices, pages.size(), nullptr, 0);
  std::vector<uint32_t> roots(count);
  EXPECT_EQ(count, EPDFDoc_GetImportFieldRoots(document, indices, pages.size(),
                                               roots.data(), roots.size()));
  return roots;
}

RetainPtr<const CPDF_Dictionary> AcroFormOf(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  return doc->GetRoot()->GetDictFor("AcroForm");
}

class EPDFPpoEmbedderTest : public EmbedderTest {
 protected:
  // |document| saved in full and opened again.
  ScopedFPDFDocument SaveAndReopen(FPDF_DOCUMENT document) {
    unsigned long size = 0;
    void* buffer =
        EPDF_SaveDocumentToOwnedBuffer(document, FPDF_NO_INCREMENTAL, &size);
    EXPECT_TRUE(buffer);
    saved_.emplace_back(static_cast<const char*>(buffer),
                        static_cast<const char*>(buffer) + size);
    EPDF_FreeBuffer(buffer);
    const std::string& bytes = saved_.back();
    return ScopedFPDFDocument(FPDF_LoadMemDocument(
        bytes.data(), static_cast<int>(bytes.size()), nullptr));
  }

  struct LayerDoc {
    std::vector<uint8_t> bytes;
    EPDF_BASE_DOCUMENT base = nullptr;
    FPDF_DOCUMENT layer = nullptr;

    ~LayerDoc() {
      if (layer) {
        FPDF_CloseDocument(layer);
      }
      if (base) {
        EPDF_ReleaseBaseDocument(base);
      }
    }
  };

  bool OpenLayer(const char* file_name, LayerDoc* out) {
    std::string file_path = PathService::GetTestFilePath(file_name);
    out->bytes = GetFileContents(file_path.c_str());
    if (out->bytes.empty()) {
      return false;
    }
    out->base = EPDF_LoadMemBaseDocument(
        out->bytes.data(), static_cast<int>(out->bytes.size()), nullptr);
    if (!out->base) {
      return false;
    }
    EPDFLayerOpenStatus status;
    out->layer = EPDFLayer_OpenLayer(out->base, nullptr, nullptr, &status);
    return out->layer && status == EPDFLayerOpenStatus_kSuccess;
  }

 private:
  std::vector<std::string> saved_;
};

}  // namespace

TEST_F(EPDFPpoEmbedderTest, NamesTheTopLevelFieldsThePagesBring) {
  ASSERT_TRUE(OpenDocument("form_page_import.pdf"));
  EXPECT_EQ((std::vector<uint32_t>{kPerson, kPlan, kAgree}),
            ImportRoots(document(), {}));
  EXPECT_EQ((std::vector<uint32_t>{kPerson, kPlan}),
            ImportRoots(document(), {0}));
  EXPECT_EQ((std::vector<uint32_t>{kPerson, kPlan, kAgree}),
            ImportRoots(document(), {1}));
  EXPECT_TRUE(ImportRoots(document(), {2}).empty());

  EXPECT_EQ(L"person", PartialName(document(), kPerson));
  EXPECT_EQ(L"agree", PartialName(document(), kAgree));
  EXPECT_EQ(L"email", PartialName(document(), kEmail));
  EXPECT_EQ(L"", PartialName(document(), 3));  // a page
  EXPECT_EQ(0ul, EPDFForm_GetFieldPartialName(document(), 99, nullptr, 0));
}

TEST_F(EPDFPpoEmbedderTest, BringsTheFieldsOfEveryPage) {
  ASSERT_TRUE(OpenDocument("form_page_import.pdf"));
  ScopedFPDFDocument dest(FPDF_CreateNewDocument());
  ASSERT_TRUE(EPDFDoc_ImportPages(dest.get(), document(), nullptr, 0, 0));
  EXPECT_EQ(2, FPDF_GetPageCount(dest.get()));
  EXPECT_EQ(AllPagesFields(), ReadFields(dest.get()));

  // The source's /AcroForm settings, on the one made for them.
  RetainPtr<const CPDF_Dictionary> acro_form = AcroFormOf(dest.get());
  ASSERT_TRUE(acro_form);
  EXPECT_EQ(3u, acro_form->GetArrayFor("Fields")->size());
  EXPECT_EQ("/Helv 0 Tf 0 g", acro_form->GetByteStringFor("DA"));
  EXPECT_EQ(1, acro_form->GetIntegerFor("Q"));
  EXPECT_TRUE(acro_form->GetBooleanFor("NeedAppearances", false));
  EXPECT_FALSE(acro_form->KeyExist("SigFlags"));
  RetainPtr<const CPDF_Dictionary> fonts =
      acro_form->GetDictFor("DR")->GetDictFor("Font");
  EXPECT_EQ("Helvetica", fonts->GetDictFor("Helv")->GetNameFor("BaseFont"));
  EXPECT_EQ("Times-Roman", fonts->GetDictFor("F1")->GetNameFor("BaseFont"));
  RetainPtr<const CPDF_Array> order = acro_form->GetArrayFor("CO");
  ASSERT_TRUE(order);
  ASSERT_EQ(1u, order->size());
  EXPECT_EQ(L"email", order->GetDictAt(0)->GetUnicodeTextFor("T"));
  // Their own settings, so none is set on the top-level fields.
  RetainPtr<const CPDF_Dictionary> person =
      acro_form->GetArrayFor("Fields")->GetDictAt(0);
  EXPECT_FALSE(person->KeyExist("DA"));
  EXPECT_FALSE(person->KeyExist("Q"));

  ScopedFPDFDocument reopened = SaveAndReopen(dest.get());
  ASSERT_TRUE(reopened);
  EXPECT_EQ(AllPagesFields(), ReadFields(reopened.get()));
}

TEST_F(EPDFPpoEmbedderTest, BringsOnlyWhatIsAboveThePagesWidgets) {
  ASSERT_TRUE(OpenDocument("form_page_import.pdf"));
  ScopedFPDFDocument dest(FPDF_CreateNewDocument());
  const int second_page = 1;
  ASSERT_TRUE(EPDFDoc_ImportPages(dest.get(), document(), &second_page, 1, 0));
  const std::map<std::wstring, Field> expected = {
      {L"person.email", {L"ann@example.com", 1, true}},
      {L"plan", {L"yearly", 1, true}},
      {L"agree", {L"Yes", 1, true}},
  };
  EXPECT_EQ(expected, ReadFields(dest.get()));

  // "person" lists only what came: email, not name.
  RetainPtr<const CPDF_Dictionary> person =
      AcroFormOf(dest.get())->GetArrayFor("Fields")->GetDictAt(0);
  ASSERT_TRUE(person);
  EXPECT_EQ(L"person", person->GetUnicodeTextFor("T"));
  ASSERT_EQ(1u, person->GetArrayFor("Kids")->size());

  ScopedFPDFDocument reopened = SaveAndReopen(dest.get());
  ASSERT_TRUE(reopened);
  EXPECT_EQ(expected, ReadFields(reopened.get()));
}

TEST_F(EPDFPpoEmbedderTest, PointsCopiedParentsAtTheCopies) {
  ASSERT_TRUE(OpenDocument("form_page_import.pdf"));
  ScopedFPDFDocument dest(FPDF_CreateNewDocument());
  // A page first, so the copies' numbers differ from the source's.
  FPDF_ClosePage(FPDFPage_New(dest.get(), 0, 300, 300));
  const int first_page = 0;
  ASSERT_TRUE(EPDFDoc_ImportPages(dest.get(), document(), &first_page, 1, 1));

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(dest.get());
  RetainPtr<const CPDF_Array> annots =
      doc->GetPageDictionary(1)->GetArrayFor("Annots");
  ASSERT_EQ(4u, annots->size());
  // The name widget, found only through name's /Kids, has name's copy as its
  // /Parent.
  RetainPtr<const CPDF_Dictionary> name =
      annots->GetDictAt(0)->GetDictFor("Parent");
  ASSERT_TRUE(name);
  EXPECT_EQ(L"name", name->GetUnicodeTextFor("T"));
  EXPECT_EQ(annots->GetDictAt(0)->GetObjNum(),
            name->GetArrayFor("Kids")->GetDictAt(0)->GetObjNum());
  // The pop-up's /Parent is the note's copy.
  const CPDF_Reference* popup_parent =
      annots->GetDictAt(3)->GetObjectFor("Parent")->AsReference();
  ASSERT_TRUE(popup_parent);
  EXPECT_EQ(annots->GetDictAt(2)->GetObjNum(), popup_parent->GetRefObjNum());
}

TEST_F(EPDFPpoEmbedderTest, AddsToAFormAndRenamesAFontItUses) {
  ASSERT_TRUE(OpenDocument("form_page_import.pdf"));
  ScopedFPDFDocument dest(FPDF_CreateNewDocument());
  FPDF_ClosePage(FPDFPage_New(dest.get(), 0, 300, 300));
  // A form with "person", /F1 another font, and a /DA of its own.
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(dest.get());
  auto courier = doc->NewIndirect<CPDF_Dictionary>();
  courier->SetNewFor<CPDF_Name>("Type", "Font");
  courier->SetNewFor<CPDF_Name>("Subtype", "Type1");
  courier->SetNewFor<CPDF_Name>("BaseFont", "Courier");
  auto existing = doc->NewIndirect<CPDF_Dictionary>();
  existing->SetNewFor<CPDF_Name>("FT", "Tx");
  existing->SetNewFor<CPDF_String>("T", "person");
  auto acro_form =
      doc->GetMutableRoot()->SetNewFor<CPDF_Dictionary>("AcroForm");
  acro_form->SetNewFor<CPDF_Array>("Fields")->AppendNew<CPDF_Reference>(
      doc, existing->GetObjNum());
  acro_form->SetNewFor<CPDF_Dictionary>("DR")
      ->SetNewFor<CPDF_Dictionary>("Font")
      ->SetNewFor<CPDF_Reference>("F1", doc, courier->GetObjNum());
  acro_form->SetNewFor<CPDF_String>("DA", "/F1 12 Tf 0 g");

  // The caller renames the source's "person" first.
  EXPECT_FALSE(EPDFForm_SetFieldName(document(), kPerson,
                                     GetFPDFWideString(L"plan").get()));
  ASSERT_TRUE(EPDFForm_SetFieldName(document(), kPerson,
                                    GetFPDFWideString(L"person_2").get()));
  ASSERT_TRUE(EPDFDoc_ImportPages(dest.get(), document(), nullptr, 0, 1));

  std::map<std::wstring, Field> expected = {
      {L"person", {L"", 0, true}},
      {L"person_2.name", {L"Ann", 1, true}},
      {L"person_2.email", {L"ann@example.com", 1, true}},
      {L"plan", {L"yearly", 2, true}},
      {L"agree", {L"Yes", 1, true}},
  };
  EXPECT_EQ(expected, ReadFields(dest.get()));

  RetainPtr<const CPDF_Dictionary> fonts_after =
      AcroFormOf(dest.get())->GetDictFor("DR")->GetDictFor("Font");
  EXPECT_EQ("Courier", fonts_after->GetDictFor("F1")->GetNameFor("BaseFont"));
  EXPECT_EQ("Times-Roman",
            fonts_after->GetDictFor("F1_1")->GetNameFor("BaseFont"));
  EXPECT_TRUE(fonts_after->KeyExist("Helv"));
  RetainPtr<const CPDF_Array> fields =
      AcroFormOf(dest.get())->GetArrayFor("Fields");
  ASSERT_EQ(4u, fields->size());
  // The source's /DA and /Q on its top-level fields, which said nothing.
  RetainPtr<const CPDF_Dictionary> person = fields->GetDictAt(1);
  EXPECT_EQ(L"person_2", person->GetUnicodeTextFor("T"));
  EXPECT_EQ("/Helv 0 Tf 0 g", person->GetByteStringFor("DA"));
  EXPECT_EQ(1, person->GetIntegerFor("Q"));
  // email's own /DA names the renamed font.
  RetainPtr<const CPDF_Dictionary> email =
      person->GetArrayFor("Kids")->GetDictAt(1);
  EXPECT_EQ(L"email", email->GetUnicodeTextFor("T"));
  EXPECT_EQ("/F1_1 10 Tf 0 g", email->GetByteStringFor("DA"));

  ScopedFPDFDocument reopened = SaveAndReopen(dest.get());
  ASSERT_TRUE(reopened);
  EXPECT_EQ(expected, ReadFields(reopened.get()));
}

TEST_F(EPDFPpoEmbedderTest, CopiesIntoALayerInATransaction) {
  ASSERT_TRUE(OpenDocument("form_page_import.pdf"));
  LayerDoc dest;
  ASSERT_TRUE(OpenLayer("hello_world.pdf", &dest));
  ASSERT_TRUE(EPDFLayer_BeginTransaction(dest.layer));
  ASSERT_TRUE(EPDFDoc_ImportPages(dest.layer, document(), nullptr, 0, 1));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(dest.layer));
  EXPECT_EQ(3, FPDF_GetPageCount(dest.layer));
  EXPECT_EQ(AllPagesFields(), ReadFields(dest.layer));

  ScopedFPDFDocument reopened = SaveAndReopen(dest.layer);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(AllPagesFields(), ReadFields(reopened.get()));
}

TEST_F(EPDFPpoEmbedderTest, CopiesOutOfALayerAsTheLayerReadsIt) {
  LayerDoc src;
  ASSERT_TRUE(OpenLayer("form_page_import.pdf", &src));
  ASSERT_TRUE(EPDFLayer_BeginTransaction(src.layer));
  ASSERT_TRUE(EPDFForm_SetFieldName(src.layer, kPerson,
                                    GetFPDFWideString(L"applicant").get()));
  ASSERT_TRUE(EPDFLayer_CommitTransaction(src.layer));

  ScopedFPDFDocument dest(FPDF_CreateNewDocument());
  const int first_page = 0;
  ASSERT_TRUE(EPDFDoc_ImportPages(dest.get(), src.layer, &first_page, 1, 0));
  const std::map<std::wstring, Field> expected = {
      {L"applicant.name", {L"Ann", 1, true}},
      {L"plan", {L"yearly", 1, true}},
  };
  EXPECT_EQ(expected, ReadFields(dest.get()));
  ScopedFPDFDocument reopened = SaveAndReopen(dest.get());
  ASSERT_TRUE(reopened);
  EXPECT_EQ(expected, ReadFields(reopened.get()));
}

TEST_F(EPDFPpoEmbedderTest, RefusesBadArguments) {
  ASSERT_TRUE(OpenDocument("form_page_import.pdf"));
  ScopedFPDFDocument dest(FPDF_CreateNewDocument());
  const int out_of_range[] = {0, 2};
  EXPECT_FALSE(EPDFDoc_ImportPages(dest.get(), document(), out_of_range, 2, 0));
  const int negative = -1;
  EXPECT_FALSE(EPDFDoc_ImportPages(dest.get(), document(), &negative, 1, 0));
  EXPECT_FALSE(EPDFDoc_ImportPages(dest.get(), document(), &negative, 0, 0));
  EXPECT_FALSE(EPDFDoc_ImportPages(document(), document(), nullptr, 0, 0));
  EXPECT_FALSE(EPDFDoc_ImportPages(nullptr, document(), nullptr, 0, 0));
  // Nothing was copied.
  EXPECT_EQ(0, FPDF_GetPageCount(dest.get()));
  EXPECT_FALSE(AcroFormOf(dest.get()));
  EXPECT_EQ(0ul, EPDFDoc_GetImportFieldRoots(document(), out_of_range, 2,
                                             nullptr, 0));
}
