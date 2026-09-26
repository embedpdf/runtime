// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_form.h"

#include <string>
#include <vector>

#include "constants/form_fields.h"
#include "constants/form_flags.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/test_loader.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

namespace {

using WideStringGetter = unsigned long (*)(EPDF_FORM_MODEL,
                                           int,
                                           FPDF_WCHAR*,
                                           unsigned long);

std::wstring GetWideString(WideStringGetter getter,
                           EPDF_FORM_MODEL model,
                           int field_index) {
  unsigned long length_bytes = getter(model, field_index, nullptr, 0);
  if (length_bytes == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(length_bytes,
            getter(model, field_index, buffer.data(), length_bytes));
  return GetPlatformWString(buffer.data());
}

using FieldValueGetter =
    unsigned long (*)(EPDF_FORM_MODEL, int, int, FPDF_WCHAR*, unsigned long);

std::wstring GetFieldValue(FieldValueGetter getter,
                           EPDF_FORM_MODEL model,
                           int field_index,
                           int value_index = 0) {
  unsigned long length_bytes =
      getter(model, field_index, value_index, nullptr, 0);
  if (length_bytes == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(length_bytes, getter(model, field_index, value_index, buffer.data(),
                                 length_bytes));
  return GetPlatformWString(buffer.data());
}

std::wstring GetCurrentFieldValue(EPDF_FORM_MODEL model,
                                  int field_index,
                                  int value_index = 0) {
  return GetFieldValue(EPDFForm_GetFieldValueAt, model, field_index,
                       value_index);
}

std::wstring GetDefaultFieldValue(EPDF_FORM_MODEL model,
                                  int field_index,
                                  int value_index = 0) {
  return GetFieldValue(EPDFForm_GetFieldDefaultValueAt, model, field_index,
                       value_index);
}

std::wstring GetWidgetExportValue(EPDF_FORM_MODEL model,
                                  int field_index,
                                  int widget_index) {
  unsigned long length_bytes = EPDFForm_GetFieldWidgetExportValue(
      model, field_index, widget_index, nullptr, 0);
  if (length_bytes == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(length_bytes,
            EPDFForm_GetFieldWidgetExportValue(model, field_index, widget_index,
                                               buffer.data(), length_bytes));
  return GetPlatformWString(buffer.data());
}

std::string GetWidgetOnState(EPDF_FORM_MODEL model,
                             int field_index,
                             int widget_index) {
  unsigned long length_bytes = EPDFForm_GetFieldWidgetOnState(
      model, field_index, widget_index, nullptr, 0);
  if (length_bytes == 0) {
    return std::string();
  }
  std::vector<char> buffer(length_bytes);
  EXPECT_EQ(length_bytes,
            EPDFForm_GetFieldWidgetOnState(model, field_index, widget_index,
                                           buffer.data(), length_bytes));
  // |length_bytes| includes the trailing NUL.
  return std::string(buffer.data());
}

RetainPtr<const CPDF_Dictionary> GetEffectiveIndirectDictionary(
    FPDF_DOCUMENT document,
    uint32_t object_number) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  return doc ? ToDictionary(doc->GetOrParseIndirectObject(object_number))
             : nullptr;
}

RetainPtr<CPDF_Dictionary> GetMutableIndirectDictionary(
    FPDF_DOCUMENT document,
    uint32_t object_number) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  return doc ? ToDictionary(doc->GetMutableIndirectObject(object_number))
             : nullptr;
}

std::wstring GetEffectiveWidgetAppearance(FPDF_DOCUMENT document,
                                          uint32_t widget_object_number) {
  RetainPtr<const CPDF_Dictionary> widget =
      GetEffectiveIndirectDictionary(document, widget_object_number);
  RetainPtr<const CPDF_Dictionary> appearance =
      widget ? widget->GetDictFor("AP") : nullptr;
  RetainPtr<const CPDF_Stream> normal =
      appearance ? appearance->GetStreamFor("N") : nullptr;
  if (!normal) {
    return std::wstring();
  }
  const WideString text = normal->GetUnicodeText();
  return std::wstring(text.c_str(), text.GetLength());
}

int FieldIndexByName(EPDF_FORM_MODEL model, const wchar_t* name) {
  for (int i = 0; i < EPDFForm_CountFields(model); ++i) {
    if (GetWideString(EPDFForm_GetFieldName, model, i) == name) {
      return i;
    }
  }
  return -1;
}

class EPDFFormEmbedderTest : public EmbedderTest {
 protected:
  // A base document plus a fresh empty layer over it, for delta assertions.
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
    if (file_path.empty()) {
      return false;
    }
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
};

}  // namespace

TEST_F(EPDFFormEmbedderTest, NoFormYieldsEmptyModel) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(EPDF_FORMKIND_NONE, EPDFForm_GetFormKind(model));
  EXPECT_FALSE(EPDFForm_GetNeedAppearances(model));
  EXPECT_EQ(0, EPDFForm_CountFields(model));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, TextFormModel) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(EPDF_FORMKIND_ACROFORM, EPDFForm_GetFormKind(model));
  ASSERT_EQ(1, EPDFForm_CountFields(model));

  EXPECT_EQ(EPDF_FORMFIELD_FAMILY_TEXT, EPDFForm_GetFieldFamily(model, 0));
  EXPECT_EQ(EPDF_FORMFIELD_ORIGIN_ACROFORM, EPDFForm_GetFieldOrigin(model, 0));
  EXPECT_EQ(L"Text Box", GetWideString(EPDFForm_GetFieldName, model, 0));
  EXPECT_EQ(4u, EPDFForm_GetFieldObjNum(model, 0));

  // Merged field/widget dictionary: one widget sharing the field's object
  // number, placed on the page (object 3).
  ASSERT_EQ(1, EPDFForm_CountFieldWidgets(model, 0));
  EXPECT_EQ(4u, EPDFForm_GetFieldWidgetObjNum(model, 0, 0));
  EXPECT_EQ(3u, EPDFForm_GetFieldWidgetPageObjNum(model, 0, 0));
  EXPECT_EQ(0, EPDFForm_GetFieldIndexForWidget(model, 4u));
  EXPECT_EQ(0, EPDFForm_GetFieldIndexByObjNum(model, 4u));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, TypedValueSnapshotPreservesPdfShapes) {
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));

  RetainPtr<CPDF_Dictionary> multi =
      GetMutableIndirectDictionary(document(), 12u);
  ASSERT_TRUE(multi);
  RetainPtr<CPDF_Array> defaults =
      multi->SetNewFor<CPDF_Array>(pdfium::form_fields::kDV);
  defaults->AppendNew<CPDF_String>(L"Alpha");
  defaults->AppendNew<CPDF_String>(L"Gamma");

  RetainPtr<CPDF_Dictionary> empty_array =
      GetMutableIndirectDictionary(document(), 9u);
  ASSERT_TRUE(empty_array);
  empty_array->SetNewFor<CPDF_Array>(pdfium::form_fields::kDV);

  RetainPtr<CPDF_Dictionary> malformed =
      GetMutableIndirectDictionary(document(), 10u);
  ASSERT_TRUE(malformed);
  malformed->SetNewFor<CPDF_Number>(pdfium::form_fields::kV, 7);
  malformed->SetNewFor<CPDF_Dictionary>(pdfium::form_fields::kDV);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);

  int field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(EPDF_FORM_VALUE_ARRAY, EPDFForm_GetFieldValueKind(model, field));
  ASSERT_EQ(2, EPDFForm_CountFieldValues(model, field));
  EXPECT_EQ(L"Epsilon", GetCurrentFieldValue(model, field, 0));
  EXPECT_EQ(L"Gamma", GetCurrentFieldValue(model, field, 1));
  EXPECT_EQ(EPDF_FORM_VALUE_ARRAY,
            EPDFForm_GetFieldDefaultValueKind(model, field));
  ASSERT_EQ(2, EPDFForm_CountFieldDefaultValues(model, field));
  EXPECT_EQ(L"Alpha", GetDefaultFieldValue(model, field, 0));
  EXPECT_EQ(L"Gamma", GetDefaultFieldValue(model, field, 1));
  EXPECT_EQ(0u, EPDFForm_GetFieldValueAt(model, field, 2, nullptr, 0));

  field = EPDFForm_GetFieldIndexByObjNum(model, 9u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(EPDF_FORM_VALUE_SCALAR, EPDFForm_GetFieldValueKind(model, field));
  EXPECT_EQ(L"Banana", GetCurrentFieldValue(model, field));
  EXPECT_EQ(EPDF_FORM_VALUE_ARRAY,
            EPDFForm_GetFieldDefaultValueKind(model, field));
  EXPECT_EQ(0, EPDFForm_CountFieldDefaultValues(model, field));

  field = EPDFForm_GetFieldIndexByObjNum(model, 10u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(EPDF_FORM_VALUE_UNSUPPORTED,
            EPDFForm_GetFieldValueKind(model, field));
  EXPECT_EQ(0, EPDFForm_CountFieldValues(model, field));
  EXPECT_EQ(EPDF_FORM_VALUE_UNSUPPORTED,
            EPDFForm_GetFieldDefaultValueKind(model, field));
  EXPECT_EQ(0, EPDFForm_CountFieldDefaultValues(model, field));

  EXPECT_EQ(EPDF_FORM_VALUE_NONE, EPDFForm_GetFieldValueKind(nullptr, 0));
  EXPECT_EQ(0, EPDFForm_CountFieldValues(nullptr, 0));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, ClickFormModel) {
  ASSERT_TRUE(OpenDocument("click_form.pdf"));
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(EPDF_FORMKIND_ACROFORM, EPDFForm_GetFormKind(model));
  ASSERT_EQ(4, EPDFForm_CountFields(model));

  // Field 0: merged read-only checkbox, checked via /AS /Yes.
  EXPECT_EQ(L"readOnlyCheckbox",
            GetWideString(EPDFForm_GetFieldName, model, 0));
  EXPECT_EQ(EPDF_FORMFIELD_FAMILY_CHECKBOX, EPDFForm_GetFieldFamily(model, 0));
  EXPECT_TRUE(EPDFForm_GetFieldFlags(model, 0) & 1);  // ReadOnly.
  EXPECT_EQ(L"Yes", GetCurrentFieldValue(model, 0));
  ASSERT_EQ(1, EPDFForm_CountFieldWidgets(model, 0));
  EXPECT_EQ("Yes", GetWidgetOnState(model, 0, 0));
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, 0, 0));

  // Field 1: merged checkbox, unchecked.
  EXPECT_EQ(L"checkbox", GetWideString(EPDFForm_GetFieldName, model, 1));
  EXPECT_EQ(EPDF_FORMFIELD_FAMILY_CHECKBOX, EPDFForm_GetFieldFamily(model, 1));
  EXPECT_EQ(L"Off", GetCurrentFieldValue(model, 1));
  EXPECT_FALSE(EPDFForm_IsFieldWidgetChecked(model, 1, 0));

  // Field 2: read-only radio group with three separate widget kids.
  EXPECT_EQ(L"readOnlyRadioButton",
            GetWideString(EPDFForm_GetFieldName, model, 2));
  EXPECT_EQ(EPDF_FORMFIELD_FAMILY_RADIO, EPDFForm_GetFieldFamily(model, 2));
  ASSERT_EQ(3, EPDFForm_CountFieldWidgets(model, 2));
  EXPECT_EQ("value1", GetWidgetOnState(model, 2, 0));
  EXPECT_EQ("value2", GetWidgetOnState(model, 2, 1));
  EXPECT_EQ("value3", GetWidgetOnState(model, 2, 2));
  EXPECT_FALSE(EPDFForm_IsFieldWidgetChecked(model, 2, 0));
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, 2, 2));
  EXPECT_EQ(L"value3", GetCurrentFieldValue(model, 2));
  EXPECT_EQ(L"value3", GetWidgetExportValue(model, 2, 2));

  // Field 3: radio group; widgets 13/14/15 all map back to it.
  EXPECT_EQ(L"radioButton", GetWideString(EPDFForm_GetFieldName, model, 3));
  ASSERT_EQ(3, EPDFForm_CountFieldWidgets(model, 3));
  EXPECT_EQ(3, EPDFForm_GetFieldIndexForWidget(model, 13u));
  EXPECT_EQ(3, EPDFForm_GetFieldIndexForWidget(model, 14u));
  EXPECT_EQ(3, EPDFForm_GetFieldIndexForWidget(model, 15u));
  EXPECT_EQ(-1, EPDFForm_GetFieldIndexForWidget(model, 9999u));

  // Everything in this document is properly linked into /AcroForm /Fields.
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(EPDF_FORMFIELD_ORIGIN_ACROFORM,
              EPDFForm_GetFieldOrigin(model, i));
  }
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, OrphanWidgetsRecovered) {
  ASSERT_TRUE(OpenDocument("orphan_widgets.pdf"));
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(EPDF_FORMKIND_ACROFORM, EPDFForm_GetFormKind(model));

  // /AcroForm /Fields only lists the text field; the checkbox and the whole
  // radio group are reachable through page /Annots alone. Without the sweep
  // this model would contain one field instead of three.
  ASSERT_EQ(3, EPDFForm_CountFields(model));

  EXPECT_EQ(L"linked_text", GetWideString(EPDFForm_GetFieldName, model, 0));
  EXPECT_EQ(EPDF_FORMFIELD_FAMILY_TEXT, EPDFForm_GetFieldFamily(model, 0));
  EXPECT_EQ(EPDF_FORMFIELD_ORIGIN_ACROFORM, EPDFForm_GetFieldOrigin(model, 0));
  EXPECT_EQ(L"hello", GetCurrentFieldValue(model, 0));

  EXPECT_EQ(L"orphan_check", GetWideString(EPDFForm_GetFieldName, model, 1));
  EXPECT_EQ(EPDF_FORMFIELD_FAMILY_CHECKBOX, EPDFForm_GetFieldFamily(model, 1));
  EXPECT_EQ(EPDF_FORMFIELD_ORIGIN_RECOVERED, EPDFForm_GetFieldOrigin(model, 1));
  EXPECT_EQ(5u, EPDFForm_GetFieldObjNum(model, 1));
  ASSERT_EQ(1, EPDFForm_CountFieldWidgets(model, 1));
  EXPECT_EQ("Yes", GetWidgetOnState(model, 1, 0));
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, 1, 0));
  EXPECT_EQ(L"Yes", GetCurrentFieldValue(model, 1));

  // The radio group's parent field dictionary is not referenced anywhere in
  // /AcroForm /Fields. The sweep climbs /Parent from the first widget it
  // sees, so BOTH widgets must land on ONE logical field.
  EXPECT_EQ(L"orphan_radio", GetWideString(EPDFForm_GetFieldName, model, 2));
  EXPECT_EQ(EPDF_FORMFIELD_FAMILY_RADIO, EPDFForm_GetFieldFamily(model, 2));
  EXPECT_EQ(EPDF_FORMFIELD_ORIGIN_RECOVERED, EPDFForm_GetFieldOrigin(model, 2));
  EXPECT_EQ(6u, EPDFForm_GetFieldObjNum(model, 2));
  EXPECT_TRUE(EPDFForm_GetFieldFlags(model, 2) & 0x8000);  // Radio.
  ASSERT_EQ(2, EPDFForm_CountFieldWidgets(model, 2));
  EXPECT_EQ(8u, EPDFForm_GetFieldWidgetObjNum(model, 2, 0));
  EXPECT_EQ(9u, EPDFForm_GetFieldWidgetObjNum(model, 2, 1));
  EXPECT_EQ(3u, EPDFForm_GetFieldWidgetPageObjNum(model, 2, 0));
  EXPECT_EQ("a", GetWidgetOnState(model, 2, 0));
  EXPECT_EQ("b", GetWidgetOnState(model, 2, 1));
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, 2, 0));
  EXPECT_FALSE(EPDFForm_IsFieldWidgetChecked(model, 2, 1));
  EXPECT_EQ(L"a", GetCurrentFieldValue(model, 2));

  EXPECT_EQ(2, EPDFForm_GetFieldIndexForWidget(model, 8u));
  EXPECT_EQ(2, EPDFForm_GetFieldIndexForWidget(model, 9u));
  EXPECT_EQ(2, EPDFForm_GetFieldIndexByObjNum(model, 6u));
  EPDFForm_CloseModel(model);
}

// A "two-plane" document (the IRS f1040 class): every field exists TWICE
// under one fully qualified name — an orphaned twin inside /AcroForm
// /Fields that no page references, and a standalone merged twin in page
// /Annots that /AcroForm cannot reach. Reads reconcile the planes into ONE
// field, so writes must cover BOTH twins; a write planned from the raw
// field dictionary alone would edit the invisible orphan while the
// on-screen widget never changes.
TEST_F(EPDFFormEmbedderTest, TwoPlaneTwinWidgetsFillTogether) {
  ASSERT_TRUE(OpenDocument("two_plane_form.pdf"));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  // Two logical fields, not four: the same-FQN twins merge, and each field
  // carries both twin widgets (the orphan first — /Fields loads before the
  // page sweep — then the page twin).
  ASSERT_EQ(2, EPDFForm_CountFields(model));
  const int checkbox = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  ASSERT_GE(checkbox, 0);
  ASSERT_EQ(2, EPDFForm_CountFieldWidgets(model, checkbox));
  EXPECT_EQ(4u, EPDFForm_GetFieldWidgetObjNum(model, checkbox, 0));
  EXPECT_EQ(9u, EPDFForm_GetFieldWidgetObjNum(model, checkbox, 1));
  const int text = EPDFForm_GetFieldIndexByObjNum(model, 5u);
  ASSERT_GE(text, 0);
  ASSERT_EQ(2, EPDFForm_CountFieldWidgets(model, text));
  EXPECT_EQ(10u, EPDFForm_GetFieldWidgetObjNum(model, text, 1));
  EPDFForm_CloseModel(model);

  // Toggling flips /AS on BOTH twins — above all the page twin (obj 11),
  // the only one the user can see.
  uint32_t changed[4] = {};
  unsigned long changed_count = 0;
  ASSERT_TRUE(
      EPDFForm_SetToggle(document(), 4u, "1", changed, 4, &changed_count));
  ASSERT_EQ(2ul, changed_count);
  EXPECT_EQ(4u, changed[0]);
  EXPECT_EQ(9u, changed[1]);
  for (const uint32_t objnum : {4u, 9u}) {
    RetainPtr<const CPDF_Dictionary> widget =
        GetEffectiveIndirectDictionary(document(), objnum);
    ASSERT_TRUE(widget);
    EXPECT_EQ("1", widget->GetNameFor("AS")) << "widget " << objnum;
  }

  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int checked = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  ASSERT_GE(checked, 0);
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, checked, 0));
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, checked, 1));
  EPDFForm_CloseModel(model);

  // A text commit regenerates the page twin's appearance with the value —
  // even though this document has no /AcroForm /DR: generation must seed a
  // fallback font instead of vetoing the appearance.
  ScopedFPDFWideString value = GetFPDFWideString(L"TWIN");
  changed_count = 0;
  ASSERT_TRUE(EPDFForm_SetTextValue(document(), 5u, value.get(), changed, 4,
                                    &changed_count));
  ASSERT_EQ(2ul, changed_count);
  EXPECT_EQ(5u, changed[0]);
  EXPECT_EQ(10u, changed[1]);
  const std::wstring appearance =
      GetEffectiveWidgetAppearance(document(), 10u);
  EXPECT_NE(std::wstring::npos, appearance.find(L"TWIN")) << appearance;

  // The write seeded /DR/Font with the /DA-named font.
  RetainPtr<const CPDF_Dictionary> acroform =
      GetEffectiveIndirectDictionary(document(), 2u);
  ASSERT_TRUE(acroform);
  RetainPtr<const CPDF_Dictionary> dr_dict = acroform->GetDictFor("DR");
  ASSERT_TRUE(dr_dict);
  RetainPtr<const CPDF_Dictionary> dr_font_dict = dr_dict->GetDictFor("Font");
  ASSERT_TRUE(dr_font_dict);
  EXPECT_TRUE(dr_font_dict->KeyExist("Helv"));
}

// Building a form model must be a pure read: over a layer document it must
// not promote a single object into the layer, even while it reconciles
// orphan widgets in memory.
TEST_F(EPDFFormEmbedderTest, LayerModelLoadIsPure) {
  std::string file_path = PathService::GetTestFilePath("orphan_widgets.pdf");
  ASSERT_FALSE(file_path.empty());
  std::vector<uint8_t> contents = GetFileContents(file_path.c_str());
  ASSERT_FALSE(contents.empty());

  EPDF_BASE_DOCUMENT base = EPDF_LoadMemBaseDocument(
      contents.data(), static_cast<int>(contents.size()), nullptr);
  ASSERT_TRUE(base);

  EPDFLayerOpenStatus status;
  FPDF_DOCUMENT layer = EPDFLayer_OpenLayer(base, nullptr, nullptr, &status);
  ASSERT_TRUE(layer);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(layer);
  ASSERT_TRUE(model);
  EXPECT_EQ(3, EPDFForm_CountFields(model));
  EXPECT_EQ(EPDF_FORMFIELD_ORIGIN_RECOVERED, EPDFForm_GetFieldOrigin(model, 2));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(layer));
  EPDFForm_CloseModel(model);

  FPDF_CloseDocument(layer);
  EPDF_ReleaseBaseDocument(base);
}

// A promoted non-terminal field is only reachable through frozen /Fields and
// /Kids references. Rebuilding the model must resolve those references through
// the effective layer view so the child's fully qualified name observes the
// promoted ancestor.
TEST_F(EPDFFormEmbedderTest, LayerModelReadsPromotedFieldAncestor) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("toggle_fields.pdf", &doc));

  ASSERT_TRUE(EPDFForm_SetFieldName(
      doc.layer, 16u, GetFPDFWideString(L"account").get()));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, 16u));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(doc.layer);
  ASSERT_TRUE(model);
  const int field = EPDFForm_GetFieldIndexByObjNum(model, 17u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(L"account.name",
            GetWideString(EPDFForm_GetFieldName, model, field));
  EPDFForm_CloseModel(model);
}

// The radio walkthrough: flipping the group promotes exactly the field plus
// the two widgets whose /AS changed - the minimal FDF-shaped delta.
TEST_F(EPDFFormEmbedderTest, SetToggleRadioOnLayer) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("orphan_widgets.pdf", &doc));

  uint32_t changed[4] = {};
  unsigned long changed_count = 0;
  ASSERT_TRUE(
      EPDFForm_SetToggle(doc.layer, 6u, "b", changed, 4, &changed_count));
  EXPECT_EQ(2ul, changed_count);
  EXPECT_EQ(8u, changed[0]);  // /AS a -> Off
  EXPECT_EQ(9u, changed[1]);  // /AS Off -> b
  EXPECT_EQ(3ul, EPDFLayer_GetPromotedObjectCount(doc.layer));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, 6u));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, 8u));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, 9u));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(doc.layer);
  ASSERT_TRUE(model);
  const int field = EPDFForm_GetFieldIndexByObjNum(model, 6u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(L"b", GetCurrentFieldValue(model, field));
  EXPECT_FALSE(EPDFForm_IsFieldWidgetChecked(model, field, 0));
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, field, 1));
  EPDFForm_CloseModel(model);

  // Idempotence: re-setting the same state changes nothing and promotes
  // nothing further.
  ASSERT_TRUE(
      EPDFForm_SetToggle(doc.layer, 6u, "b", nullptr, 0, &changed_count));
  EXPECT_EQ(0ul, changed_count);
  EXPECT_EQ(3ul, EPDFLayer_GetPromotedObjectCount(doc.layer));
}

// A failed transaction must be side-effect free: zero objects promoted.
TEST_F(EPDFFormEmbedderTest, SetToggleFailuresAreSideEffectFree) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("orphan_widgets.pdf", &doc));

  // Unknown on-state.
  EXPECT_FALSE(EPDFForm_SetToggle(doc.layer, 6u, "zz", nullptr, 0, nullptr));
  // Not a toggle field (the text field).
  EXPECT_FALSE(EPDFForm_SetToggle(doc.layer, 4u, "Yes", nullptr, 0, nullptr));
  // Unknown field object number.
  EXPECT_FALSE(EPDFForm_SetToggle(doc.layer, 9999u, "a", nullptr, 0, nullptr));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(doc.layer));
}

TEST_F(EPDFFormEmbedderTest, SetToggleClearRadioGroup) {
  ASSERT_TRUE(OpenDocument("orphan_widgets.pdf"));
  // orphan_radio has no NoToggleToOff flag, so clearing is legal.
  unsigned long changed_count = 0;
  ASSERT_TRUE(
      EPDFForm_SetToggle(document(), 6u, nullptr, nullptr, 0, &changed_count));
  EXPECT_EQ(1ul, changed_count);  // Only widget 8 was checked.

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int field = EPDFForm_GetFieldIndexByObjNum(model, 6u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(L"Off", GetCurrentFieldValue(model, field));
  EXPECT_FALSE(EPDFForm_IsFieldWidgetChecked(model, field, 0));
  EXPECT_FALSE(EPDFForm_IsFieldWidgetChecked(model, field, 1));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, ToggleSemantics) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));

  // NoToggleToOff: clearing the group is rejected; switching is fine.
  EXPECT_FALSE(
      EPDFForm_SetToggle(document(), 5u, nullptr, nullptr, 0, nullptr));
  ASSERT_TRUE(EPDFForm_SetToggle(document(), 5u, "y", nullptr, 0, nullptr));

  // Radios in unison: both /u1 widgets check together.
  unsigned long changed_count = 0;
  ASSERT_TRUE(
      EPDFForm_SetToggle(document(), 8u, "u1", nullptr, 0, &changed_count));
  EXPECT_EQ(2ul, changed_count);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  int field = EPDFForm_GetFieldIndexByObjNum(model, 5u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(L"y", GetCurrentFieldValue(model, field));

  field = EPDFForm_GetFieldIndexByObjNum(model, 8u);
  ASSERT_GE(field, 0);
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, field, 0));
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, field, 1));
  EXPECT_FALSE(EPDFForm_IsFieldWidgetChecked(model, field, 2));
  EXPECT_EQ(L"u1", GetCurrentFieldValue(model, field));
  EPDFForm_CloseModel(model);

  // Switching to /u2 unchecks both unison widgets: three /AS flips.
  ASSERT_TRUE(
      EPDFForm_SetToggle(document(), 8u, "u2", nullptr, 0, &changed_count));
  EXPECT_EQ(3ul, changed_count);

  // Checkbox with /Opt: raw /V is the control index name. The semantic
  // export value remains available on the widget.
  ASSERT_TRUE(EPDFForm_SetToggle(document(), 12u, "On", nullptr, 0, nullptr));
  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  ASSERT_GE(field, 0);
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, field, 0));
  EXPECT_EQ(L"0", GetCurrentFieldValue(model, field));
  EXPECT_EQ(L"Alpha", GetWidgetExportValue(model, field, 0));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, SetTextValue) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));

  ScopedFPDFWideString text = GetFPDFWideString(L"Hello EmbedPDF");
  uint32_t changed[2] = {};
  unsigned long changed_count = 0;
  ASSERT_TRUE(EPDFForm_SetTextValue(document(), 4u, text.get(), changed, 2,
                                    &changed_count));
  EXPECT_EQ(1ul, changed_count);
  EXPECT_EQ(4u, changed[0]);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(L"Hello EmbedPDF", GetCurrentFieldValue(model, 0));
  EPDFForm_CloseModel(model);

  // The widget's normal appearance stream was regenerated.
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, 0));
    ASSERT_TRUE(annot);
    EXPECT_GT(FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                              nullptr, 0),
              2u);
  }
  UnloadPage(page);

  // Idempotence: same value again reports zero changes.
  ASSERT_TRUE(EPDFForm_SetTextValue(document(), 4u, text.get(), nullptr, 0,
                                    &changed_count));
  EXPECT_EQ(0ul, changed_count);
}

TEST_F(EPDFFormEmbedderTest, SetTextValueMaxLenAndLayerDelta) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("toggle_fields.pdf", &doc));

  // Six characters against /MaxLen 5: Acrobat-compatible writes truncate.
  ScopedFPDFWideString too_long = GetFPDFWideString(L"abcdef");
  unsigned long changed_count = 0;
  ASSERT_TRUE(EPDFForm_SetTextValue(doc.layer, 4u, too_long.get(), nullptr, 0,
                                    &changed_count));
  EXPECT_EQ(1ul, changed_count);
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, 4u));
  // Merged field/widget plus the regenerated appearance machinery; the
  // delta must stay small.
  EXPECT_LE(EPDFLayer_GetPromotedObjectCount(doc.layer), 4ul);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(doc.layer);
  ASSERT_TRUE(model);
  const int field = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(L"abcde", GetCurrentFieldValue(model, field));
  EPDFForm_CloseModel(model);

  // Reassigning a value that normalizes to the stored value is a no-op.
  ScopedFPDFWideString fits = GetFPDFWideString(L"abcde");
  ASSERT_TRUE(EPDFForm_SetTextValue(doc.layer, 4u, fits.get(), nullptr, 0,
                                    &changed_count));
  EXPECT_EQ(0ul, changed_count);
}

TEST_F(EPDFFormEmbedderTest, SetFieldDisplayOnLayerIsDurable) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("text_form.pdf", &doc));

  EXPECT_FALSE(
      EPDFForm_SetFieldDisplay(doc.layer, 4u, 99, nullptr, 0, nullptr));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(doc.layer));

  uint32_t changed[1] = {};
  unsigned long changed_count = 0;
  ASSERT_TRUE(EPDFForm_SetFieldDisplay(doc.layer, 4u, EPDF_FORM_DISPLAY_HIDDEN,
                                       changed, 1, &changed_count));
  ASSERT_EQ(1ul, changed_count);
  EXPECT_EQ(4u, changed[0]);
  EXPECT_EQ(1ul, EPDFLayer_GetPromotedObjectCount(doc.layer));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, 4u));

  RetainPtr<const CPDF_Dictionary> widget =
      GetEffectiveIndirectDictionary(doc.layer, 4u);
  ASSERT_TRUE(widget);
  int flags = widget->GetIntegerFor("F");
  EXPECT_TRUE(flags & FPDF_ANNOT_FLAG_HIDDEN);
  EXPECT_TRUE(flags & FPDF_ANNOT_FLAG_PRINT);
  EXPECT_FALSE(flags & FPDF_ANNOT_FLAG_NOVIEW);

  ClearString();
  EPDFLayerSaveStatus save_status;
  ASSERT_TRUE(EPDFLayer_SaveDelta(doc.layer, this, &save_status));
  const std::string delta = GetString();
  ASSERT_FALSE(delta.empty());

  TestLoader loader(pdfium::as_bytes(pdfium::span(delta.data(), delta.size())));
  FPDF_FILEACCESS file_access = {};
  file_access.m_FileLen = static_cast<unsigned long>(delta.size());
  file_access.m_GetBlock = TestLoader::GetBlock;
  file_access.m_Param = &loader;
  EPDFLayerOpenStatus status;
  FPDF_DOCUMENT second =
      EPDFLayer_OpenLayer(doc.base, &file_access, nullptr, &status);
  ASSERT_TRUE(second);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);

  widget = GetEffectiveIndirectDictionary(second, 4u);
  ASSERT_TRUE(widget);
  flags = widget->GetIntegerFor("F");
  EXPECT_TRUE(flags & FPDF_ANNOT_FLAG_HIDDEN);
  EXPECT_TRUE(flags & FPDF_ANNOT_FLAG_PRINT);
  FPDF_CloseDocument(second);
}

TEST_F(EPDFFormEmbedderTest, SetFieldAppearanceTextOnLayerIsDurable) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("text_form.pdf", &doc));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(doc.layer);
  ASSERT_TRUE(model);
  int field = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  ASSERT_GE(field, 0);
  ASSERT_EQ(L"", GetCurrentFieldValue(model, field));
  EPDFForm_CloseModel(model);

  ScopedFPDFWideString formatted = GetFPDFWideString(L"FormattedValue");
  uint32_t changed[1] = {};
  unsigned long changed_count = 0;
  ASSERT_TRUE(EPDFForm_SetFieldAppearanceText(doc.layer, 4u, formatted.get(),
                                              changed, 1, &changed_count));
  ASSERT_EQ(1ul, changed_count);
  EXPECT_EQ(4u, changed[0]);
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, 4u));

  model = EPDFForm_LoadModel(doc.layer);
  ASSERT_TRUE(model);
  field = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(L"", GetCurrentFieldValue(model, field));
  EPDFForm_CloseModel(model);

  const std::wstring first_appearance =
      GetEffectiveWidgetAppearance(doc.layer, 4u);
  EXPECT_NE(std::wstring::npos, first_appearance.find(L"FormattedValue"))
      << first_appearance;

  ClearString();
  EPDFLayerSaveStatus save_status;
  ASSERT_TRUE(EPDFLayer_SaveDelta(doc.layer, this, &save_status));
  const std::string delta = GetString();
  ASSERT_FALSE(delta.empty());

  TestLoader loader(pdfium::as_bytes(pdfium::span(delta.data(), delta.size())));
  FPDF_FILEACCESS file_access = {};
  file_access.m_FileLen = static_cast<unsigned long>(delta.size());
  file_access.m_GetBlock = TestLoader::GetBlock;
  file_access.m_Param = &loader;
  EPDFLayerOpenStatus status;
  FPDF_DOCUMENT second =
      EPDFLayer_OpenLayer(doc.base, &file_access, nullptr, &status);
  ASSERT_TRUE(second);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);

  model = EPDFForm_LoadModel(second);
  ASSERT_TRUE(model);
  field = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(L"", GetCurrentFieldValue(model, field));
  EPDFForm_CloseModel(model);
  const std::wstring second_appearance =
      GetEffectiveWidgetAppearance(second, 4u);
  EXPECT_NE(std::wstring::npos, second_appearance.find(L"FormattedValue"))
      << second_appearance;
  FPDF_CloseDocument(second);
}

TEST_F(EPDFFormEmbedderTest, SetChoiceValuesCombo) {
  ASSERT_TRUE(OpenDocument("combobox_form.pdf"));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int combo1 = FieldIndexByName(model, L"Combo1");
  const int editable = FieldIndexByName(model, L"Combo_Editable");
  ASSERT_GE(combo1, 0);
  ASSERT_GE(editable, 0);
  const uint32_t combo1_objnum = EPDFForm_GetFieldObjNum(model, combo1);
  const uint32_t editable_objnum = EPDFForm_GetFieldObjNum(model, editable);
  EPDFForm_CloseModel(model);

  // Non-edit combo: option values only.
  ScopedFPDFWideString cherry = GetFPDFWideString(L"Cherry");
  FPDF_WIDESTRING one_value[] = {cherry.get()};
  ASSERT_TRUE(EPDFForm_SetChoiceValues(document(), combo1_objnum, one_value, 1,
                                       nullptr, 0, nullptr));
  ScopedFPDFWideString bogus = GetFPDFWideString(L"NotAnOption");
  FPDF_WIDESTRING bogus_value[] = {bogus.get()};
  EXPECT_FALSE(EPDFForm_SetChoiceValues(document(), combo1_objnum, bogus_value,
                                        1, nullptr, 0, nullptr));

  // Edit combo: free text is accepted and clears /I; an option export value
  // selects that option.
  ASSERT_TRUE(EPDFForm_SetChoiceValues(document(), editable_objnum, bogus_value,
                                       1, nullptr, 0, nullptr));
  ScopedFPDFWideString bar = GetFPDFWideString(L"bar");
  FPDF_WIDESTRING bar_value[] = {bar.get()};
  ASSERT_TRUE(EPDFForm_SetChoiceValues(document(), editable_objnum, bar_value,
                                       1, nullptr, 0, nullptr));

  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(L"Cherry", GetCurrentFieldValue(model, combo1));
  EXPECT_TRUE(EPDFForm_IsFieldOptionSelected(model, combo1, 2));
  EXPECT_EQ(L"bar", GetCurrentFieldValue(model, editable));
  EXPECT_TRUE(EPDFForm_IsFieldOptionSelected(model, editable, 1));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, SetChoiceValuesListbox) {
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int multi = FieldIndexByName(model, L"Listbox_MultiSelect");
  const int single = FieldIndexByName(model, L"Listbox_SingleSelect");
  ASSERT_GE(multi, 0);
  ASSERT_GE(single, 0);
  const uint32_t multi_objnum = EPDFForm_GetFieldObjNum(model, multi);
  const uint32_t single_objnum = EPDFForm_GetFieldObjNum(model, single);
  EPDFForm_CloseModel(model);

  // Multi-select accepts several values regardless of input order.
  ScopedFPDFWideString cherry = GetFPDFWideString(L"Cherry");
  ScopedFPDFWideString apple = GetFPDFWideString(L"Apple");
  FPDF_WIDESTRING two_values[] = {cherry.get(), apple.get()};
  ASSERT_TRUE(EPDFForm_SetChoiceValues(document(), multi_objnum, two_values, 2,
                                       nullptr, 0, nullptr));
  // Single-select rejects multiple values.
  EXPECT_FALSE(EPDFForm_SetChoiceValues(document(), single_objnum, two_values,
                                        2, nullptr, 0, nullptr));

  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_TRUE(EPDFForm_IsFieldOptionSelected(model, multi, 0));   // Apple
  EXPECT_FALSE(EPDFForm_IsFieldOptionSelected(model, multi, 1));  // Banana
  EXPECT_TRUE(EPDFForm_IsFieldOptionSelected(model, multi, 2));   // Cherry
  EPDFForm_CloseModel(model);

  // Clearing the selection.
  ASSERT_TRUE(EPDFForm_SetChoiceValues(document(), multi_objnum, nullptr, 0,
                                       nullptr, 0, nullptr));
  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_FALSE(EPDFForm_IsFieldOptionSelected(model, multi, 0));
  EXPECT_FALSE(EPDFForm_IsFieldOptionSelected(model, multi, 2));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, ResetField) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));

  // Toggle reset restores the /DV state.
  ASSERT_TRUE(EPDFForm_SetToggle(document(), 5u, "y", nullptr, 0, nullptr));
  ASSERT_TRUE(EPDFForm_ResetField(document(), 5u, nullptr, 0, nullptr));

  // Text reset with no /DV removes the value.
  ScopedFPDFWideString text = GetFPDFWideString(L"xyz");
  ASSERT_TRUE(
      EPDFForm_SetTextValue(document(), 4u, text.get(), nullptr, 0, nullptr));
  ASSERT_TRUE(EPDFForm_ResetField(document(), 4u, nullptr, 0, nullptr));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  int field = EPDFForm_GetFieldIndexByObjNum(model, 5u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(L"x", GetCurrentFieldValue(model, field));
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, field, 0));
  EXPECT_FALSE(EPDFForm_IsFieldWidgetChecked(model, field, 1));

  field = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(L"", GetCurrentFieldValue(model, field));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, MultiSelectDefaultsResetValueAndIndices) {
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));

  ScopedFPDFWideString epsilon = GetFPDFWideString(L"Epsilon");
  ScopedFPDFWideString gamma = GetFPDFWideString(L"Gamma");
  FPDF_WIDESTRING defaults[] = {epsilon.get(), gamma.get()};
  ASSERT_TRUE(EPDFForm_SetFieldDefaultValues(document(), 12u, defaults, 2));

  // Defaults are stored in option order, matching current-value writes.
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  int field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(EPDF_FORM_VALUE_ARRAY,
            EPDFForm_GetFieldDefaultValueKind(model, field));
  ASSERT_EQ(2, EPDFForm_CountFieldDefaultValues(model, field));
  EXPECT_EQ(L"Gamma", GetDefaultFieldValue(model, field, 0));
  EXPECT_EQ(L"Epsilon", GetDefaultFieldValue(model, field, 1));
  EPDFForm_CloseModel(model);

  ScopedFPDFWideString alpha = GetFPDFWideString(L"Alpha");
  FPDF_WIDESTRING current[] = {alpha.get()};
  ASSERT_TRUE(EPDFForm_SetChoiceValues(document(), 12u, current, 1, nullptr, 0,
                                       nullptr));
  ASSERT_TRUE(EPDFForm_ResetField(document(), 12u, nullptr, 0, nullptr));

  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  EXPECT_EQ(EPDF_FORM_VALUE_ARRAY, EPDFForm_GetFieldValueKind(model, field));
  ASSERT_EQ(2, EPDFForm_CountFieldValues(model, field));
  EXPECT_EQ(L"Gamma", GetCurrentFieldValue(model, field, 0));
  EXPECT_EQ(L"Epsilon", GetCurrentFieldValue(model, field, 1));
  EPDFForm_CloseModel(model);

  RetainPtr<const CPDF_Dictionary> dictionary =
      GetEffectiveIndirectDictionary(document(), 12u);
  ASSERT_TRUE(dictionary);
  RetainPtr<const CPDF_Array> indices = dictionary->GetArrayFor("I");
  ASSERT_TRUE(indices);
  ASSERT_EQ(2u, indices->size());
  EXPECT_EQ(2, indices->GetIntegerAt(0));  // Gamma.
  EXPECT_EQ(4, indices->GetIntegerAt(1));  // Epsilon.
}

TEST_F(EPDFFormEmbedderTest, MultiSelectDefaultsAreLayerDurable) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("listbox_form.pdf", &doc));

  ScopedFPDFWideString gamma = GetFPDFWideString(L"Gamma");
  ScopedFPDFWideString epsilon = GetFPDFWideString(L"Epsilon");
  FPDF_WIDESTRING defaults[] = {gamma.get(), epsilon.get()};
  ASSERT_TRUE(EPDFForm_SetFieldDefaultValues(doc.layer, 12u, defaults, 2));
  EXPECT_EQ(1ul, EPDFLayer_GetPromotedObjectCount(doc.layer));
  ASSERT_TRUE(EPDFForm_ResetField(doc.layer, 12u, nullptr, 0, nullptr));
  // Reset also regenerates the appearance and therefore promotes its shared
  // resource object in addition to the field/widget dictionary.
  EXPECT_EQ(2ul, EPDFLayer_GetPromotedObjectCount(doc.layer));

  ClearString();
  EPDFLayerSaveStatus save_status;
  ASSERT_TRUE(EPDFLayer_SaveDelta(doc.layer, this, &save_status));
  const std::string delta = GetString();
  ASSERT_FALSE(delta.empty());

  TestLoader loader(pdfium::as_bytes(pdfium::span(delta.data(), delta.size())));
  FPDF_FILEACCESS file_access = {};
  file_access.m_FileLen = static_cast<unsigned long>(delta.size());
  file_access.m_GetBlock = TestLoader::GetBlock;
  file_access.m_Param = &loader;
  EPDFLayerOpenStatus status;
  FPDF_DOCUMENT reopened =
      EPDFLayer_OpenLayer(doc.base, &file_access, nullptr, &status);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(reopened);
  ASSERT_TRUE(model);
  const int field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(EPDF_FORM_VALUE_ARRAY,
            EPDFForm_GetFieldDefaultValueKind(model, field));
  EXPECT_EQ(EPDF_FORM_VALUE_ARRAY, EPDFForm_GetFieldValueKind(model, field));
  ASSERT_EQ(2, EPDFForm_CountFieldDefaultValues(model, field));
  EXPECT_EQ(L"Gamma", GetDefaultFieldValue(model, field, 0));
  EXPECT_EQ(L"Epsilon", GetDefaultFieldValue(model, field, 1));
  EPDFForm_CloseModel(model);
  FPDF_CloseDocument(reopened);
}

TEST_F(EPDFFormEmbedderTest, EmptyTextDefaultIsScalarAndCanBeRemoved) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));

  ScopedFPDFWideString empty = GetFPDFWideString(L"");
  FPDF_WIDESTRING defaults[] = {empty.get()};
  ASSERT_TRUE(EPDFForm_SetFieldDefaultValues(document(), 4u, defaults, 1));
  EXPECT_FALSE(EPDFForm_SetFieldDefaultValues(document(), 4u, nullptr, 0));
  FPDF_WIDESTRING too_many[] = {empty.get(), empty.get()};
  EXPECT_FALSE(EPDFForm_SetFieldDefaultValues(document(), 4u, too_many, 2));

  ScopedFPDFWideString current = GetFPDFWideString(L"not empty");
  ASSERT_TRUE(EPDFForm_SetTextValue(document(), 4u, current.get(), nullptr, 0,
                                    nullptr));
  ASSERT_TRUE(EPDFForm_ResetField(document(), 4u, nullptr, 0, nullptr));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  int field = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(EPDF_FORM_VALUE_SCALAR,
            EPDFForm_GetFieldDefaultValueKind(model, field));
  EXPECT_EQ(1, EPDFForm_CountFieldDefaultValues(model, field));
  EXPECT_EQ(L"", GetDefaultFieldValue(model, field));
  EXPECT_EQ(EPDF_FORM_VALUE_SCALAR, EPDFForm_GetFieldValueKind(model, field));
  EXPECT_EQ(1, EPDFForm_CountFieldValues(model, field));
  EXPECT_EQ(L"", GetCurrentFieldValue(model, field));
  EPDFForm_CloseModel(model);

  ASSERT_TRUE(EPDFForm_RemoveFieldDefaultValue(document(), 4u));
  model = EPDFForm_LoadModel(document());
  field = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  EXPECT_EQ(EPDF_FORM_VALUE_NONE,
            EPDFForm_GetFieldDefaultValueKind(model, field));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, ToggleDefaultWithOptUsesControlIndex) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));

  ASSERT_TRUE(EPDFForm_SetFieldDefaultToggle(document(), 12u, "On"));
  EXPECT_FALSE(EPDFForm_SetFieldDefaultToggle(document(), 12u, "Missing"));
  EXPECT_FALSE(EPDFForm_SetFieldDefaultToggle(document(), 12u, nullptr));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  int field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(EPDF_FORM_VALUE_SCALAR,
            EPDFForm_GetFieldDefaultValueKind(model, field));
  EXPECT_EQ(L"0", GetDefaultFieldValue(model, field));
  EPDFForm_CloseModel(model);

  ASSERT_TRUE(EPDFForm_SetToggle(document(), 12u, "On", nullptr, 0, nullptr));
  ASSERT_TRUE(
      EPDFForm_SetToggle(document(), 12u, nullptr, nullptr, 0, nullptr));
  ASSERT_TRUE(EPDFForm_ResetField(document(), 12u, nullptr, 0, nullptr));

  model = EPDFForm_LoadModel(document());
  field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  EXPECT_EQ(L"0", GetCurrentFieldValue(model, field));
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, field, 0));
  EPDFForm_CloseModel(model);

  // /Off is distinct from a missing default and resets the widget off.
  ASSERT_TRUE(EPDFForm_SetFieldDefaultToggle(document(), 12u, "Off"));
  ASSERT_TRUE(EPDFForm_ResetField(document(), 12u, nullptr, 0, nullptr));
  model = EPDFForm_LoadModel(document());
  field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  EXPECT_EQ(L"Off", GetDefaultFieldValue(model, field));
  EXPECT_FALSE(EPDFForm_IsFieldWidgetChecked(model, field, 0));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, ResetRejectsMalformedDefaultWithoutMutation) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  RetainPtr<CPDF_Dictionary> field =
      GetMutableIndirectDictionary(document(), 4u);
  ASSERT_TRUE(field);
  field->SetNewFor<CPDF_Number>(pdfium::form_fields::kDV, 42);
  const WideString original = field->GetUnicodeTextFor(pdfium::form_fields::kV);

  EXPECT_FALSE(EPDFForm_ResetField(document(), 4u, nullptr, 0, nullptr));
  EXPECT_EQ(original, field->GetUnicodeTextFor(pdfium::form_fields::kV));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int index = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  EXPECT_EQ(EPDF_FORM_VALUE_UNSUPPORTED,
            EPDFForm_GetFieldDefaultValueKind(model, index));
  EPDFForm_CloseModel(model);
}

namespace {

std::string ExportFdf(FPDF_DOCUMENT doc, uint32_t flags = 0) {
  unsigned long length = EPDFForm_ExportFDF(doc, nullptr, flags, nullptr, 0);
  if (length == 0) {
    return std::string();
  }
  std::vector<char> buffer(length);
  EXPECT_EQ(length,
            EPDFForm_ExportFDF(doc, nullptr, flags, buffer.data(), length));
  return std::string(buffer.data(), length);
}

std::string ExportXfdf(FPDF_DOCUMENT doc, uint32_t flags = 0) {
  unsigned long length = EPDFForm_ExportXFDF(doc, nullptr, flags, nullptr, 0);
  if (length == 0) {
    return std::string();
  }
  std::vector<char> buffer(length);
  EXPECT_EQ(length,
            EPDFForm_ExportXFDF(doc, nullptr, flags, buffer.data(), length));
  return std::string(buffer.data(), length);
}

}  // namespace

TEST_F(EPDFFormEmbedderTest, ExportFDF) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  const std::string fdf = ExportFdf(document());
  ASSERT_FALSE(fdf.empty());
  EXPECT_NE(std::string::npos, fdf.find("%FDF-1.2"));
  EXPECT_NE(std::string::npos, fdf.find("(maxlen_text)"));
  EXPECT_NE(std::string::npos, fdf.find("(abc)"));
  EXPECT_NE(std::string::npos, fdf.find("(ntto_radio)"));
  // Hierarchical fields export with their fully qualified name.
  EXPECT_NE(std::string::npos, fdf.find("(billing.name)"));
}

TEST_F(EPDFFormEmbedderTest, ExportFDFIncludesRecoveredFields) {
  ASSERT_TRUE(OpenDocument("orphan_widgets.pdf"));
  const std::string fdf = ExportFdf(document());
  ASSERT_FALSE(fdf.empty());
  // Only linked_text is reachable through /AcroForm /Fields; the exporter
  // must see the reconciled view.
  EXPECT_NE(std::string::npos, fdf.find("(linked_text)"));
  EXPECT_NE(std::string::npos, fdf.find("(orphan_check)"));
  EXPECT_NE(std::string::npos, fdf.find("(orphan_radio)"));
}

TEST_F(EPDFFormEmbedderTest, RequiredMultiSelectArrayIsNotSkippedOnExport) {
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));
  ASSERT_TRUE(EPDFForm_SetFieldFlags(document(), 12u,
                                     pdfium::form_flags::kRequired, 0));

  const std::string fdf =
      ExportFdf(document(), EPDF_FORM_EXPORT_SKIP_EMPTY_REQUIRED);
  ASSERT_FALSE(fdf.empty());
  EXPECT_NE(std::string::npos, fdf.find("(Listbox_MultiSelectMultipleValues)"));
  EXPECT_NE(std::string::npos, fdf.find("(Epsilon)"));
  EXPECT_NE(std::string::npos, fdf.find("(Gamma)"));

  const std::string xfdf =
      ExportXfdf(document(), EPDF_FORM_EXPORT_SKIP_EMPTY_REQUIRED);
  ASSERT_FALSE(xfdf.empty());
  EXPECT_NE(std::string::npos,
            xfdf.find("<field name=\"Listbox_MultiSelectMultipleValues\">"));
  EXPECT_NE(std::string::npos, xfdf.find("<value>Epsilon</value>"));
  EXPECT_NE(std::string::npos, xfdf.find("<value>Gamma</value>"));
}

TEST_F(EPDFFormEmbedderTest, ImportFDF) {
  ASSERT_TRUE(OpenDocument("orphan_widgets.pdf"));
  static const char kFdf[] =
      "%FDF-1.2\r\n"
      "1 0 obj\r\n"
      "<< /FDF << /Fields [\r\n"
      "<< /T (linked_text) /V (imported) >>\r\n"
      "<< /T (orphan_radio) /V (b) >>\r\n"
      "<< /T (no_such_field) /V (x) >>\r\n"
      "] >> >>\r\n"
      "endobj\r\n"
      "trailer\r\n"
      "<< /Root 1 0 R >>\r\n"
      "%%EOF\r\n";

  EPDF_FORM_IMPORT_RESULT result;
  ASSERT_TRUE(EPDFForm_ImportFDF(document(), kFdf, sizeof(kFdf) - 1, nullptr, 0,
                                 &result));
  EXPECT_EQ(3u, result.fields_total);
  EXPECT_EQ(2u, result.fields_applied);
  EXPECT_EQ(1u, result.fields_skipped);
  EXPECT_EQ(3u, result.widgets_changed);  // text widget + both radio kids

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  int field = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  EXPECT_EQ(L"imported", GetCurrentFieldValue(model, field));
  field = EPDFForm_GetFieldIndexByObjNum(model, 6u);
  EXPECT_EQ(L"b", GetCurrentFieldValue(model, field));
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, field, 1));
  EPDFForm_CloseModel(model);

  // Garbage payloads are rejected.
  EXPECT_FALSE(
      EPDFForm_ImportFDF(document(), "not fdf", 7, nullptr, 0, &result));
}

// Fields the caller lists are never written and count as skipped: the
// engine passes the fields a signature locked.
TEST_F(EPDFFormEmbedderTest, ImportSkipsListedFields) {
  ASSERT_TRUE(OpenDocument("orphan_widgets.pdf"));
  static const char kFdf[] =
      "%FDF-1.2\r\n"
      "1 0 obj\r\n"
      "<< /FDF << /Fields [\r\n"
      "<< /T (linked_text) /V (imported) >>\r\n"
      "<< /T (orphan_radio) /V (b) >>\r\n"
      "] >> >>\r\n"
      "endobj\r\n"
      "trailer\r\n"
      "<< /Root 1 0 R >>\r\n"
      "%%EOF\r\n";

  const uint32_t skip[] = {4u};  // linked_text
  EPDF_FORM_IMPORT_RESULT result;
  ASSERT_TRUE(
      EPDFForm_ImportFDF(document(), kFdf, sizeof(kFdf) - 1, skip, 1, &result));
  EXPECT_EQ(2u, result.fields_total);
  EXPECT_EQ(1u, result.fields_applied);
  EXPECT_EQ(1u, result.fields_skipped);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  int field = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  EXPECT_NE(L"imported", GetCurrentFieldValue(model, field));
  field = EPDFForm_GetFieldIndexByObjNum(model, 6u);
  EXPECT_EQ(L"b", GetCurrentFieldValue(model, field));
  EPDFForm_CloseModel(model);
}

// Fill a layer, export its FDF, and replay it onto a second fresh layer of
// the same base: values must survive and only touched objects promote.
TEST_F(EPDFFormEmbedderTest, FdfRoundTripAcrossLayers) {
  LayerDoc first;
  ASSERT_TRUE(OpenLayer("orphan_widgets.pdf", &first));
  ScopedFPDFWideString bob = GetFPDFWideString(L"Bob");
  ASSERT_TRUE(
      EPDFForm_SetTextValue(first.layer, 4u, bob.get(), nullptr, 0, nullptr));
  ASSERT_TRUE(EPDFForm_SetToggle(first.layer, 6u, "b", nullptr, 0, nullptr));

  unsigned long length =
      EPDFForm_ExportFDF(first.layer, nullptr, 0, nullptr, 0);
  ASSERT_GT(length, 0u);
  std::vector<char> fdf(length);
  ASSERT_EQ(length,
            EPDFForm_ExportFDF(first.layer, nullptr, 0, fdf.data(), length));

  LayerDoc second;
  ASSERT_TRUE(OpenLayer("orphan_widgets.pdf", &second));
  EPDF_FORM_IMPORT_RESULT result;
  ASSERT_TRUE(EPDFForm_ImportFDF(second.layer, fdf.data(), length, nullptr, 0,
                                 &result));
  EXPECT_EQ(3u,
            result.fields_total);  // linked_text, orphan_check, orphan_radio
  EXPECT_EQ(3u, result.fields_applied);
  EXPECT_EQ(0u, result.fields_skipped);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(second.layer);
  ASSERT_TRUE(model);
  int field = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  EXPECT_EQ(L"Bob", GetCurrentFieldValue(model, field));
  field = EPDFForm_GetFieldIndexByObjNum(model, 6u);
  EXPECT_EQ(L"b", GetCurrentFieldValue(model, field));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, ExportXFDF) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  const std::string xfdf = ExportXfdf(document());
  ASSERT_FALSE(xfdf.empty());
  EXPECT_NE(std::string::npos, xfdf.find("<?xml version=\"1.0\""));
  // Attributes serialize in map order (xml:space before xmlns); assert them
  // individually rather than positionally.
  EXPECT_NE(std::string::npos, xfdf.find("<xfdf "));
  EXPECT_NE(std::string::npos,
            xfdf.find("xmlns=\"http://ns.adobe.com/xfdf/\""));
  EXPECT_NE(std::string::npos, xfdf.find("xml:space=\"preserve\""));
  // Values are whitespace-exact: no injected newlines inside <value>.
  EXPECT_NE(std::string::npos, xfdf.find("<value>abc</value>"));
  EXPECT_NE(std::string::npos, xfdf.find("<value>x</value>"));
  // Hierarchical names nest per component.
  EXPECT_NE(std::string::npos, xfdf.find("<field name=\"billing\">"));
  EXPECT_NE(std::string::npos, xfdf.find("<field name=\"name\""));
}

TEST_F(EPDFFormEmbedderTest, ImportXFDF) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  static const char kXfdf[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">"
      "<fields>"
      "<field name=\"billing\"><field name=\"name\">"
      "<value>Bob &amp; Co</value></field></field>"
      "<field name=\"ntto_radio\"><value>y</value></field>"
      "</fields></xfdf>";

  EPDF_FORM_IMPORT_RESULT result;
  ASSERT_TRUE(EPDFForm_ImportXFDF(document(), kXfdf, sizeof(kXfdf) - 1, nullptr,
                                  0, &result));
  EXPECT_EQ(2u, result.fields_total);
  EXPECT_EQ(2u, result.fields_applied);
  EXPECT_EQ(0u, result.fields_skipped);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int billing_name = FieldIndexByName(model, L"billing.name");
  ASSERT_GE(billing_name, 0);
  // Entity decoding round-trips.
  EXPECT_EQ(L"Bob & Co", GetCurrentFieldValue(model, billing_name));
  const int radio = EPDFForm_GetFieldIndexByObjNum(model, 5u);
  EXPECT_EQ(L"y", GetCurrentFieldValue(model, radio));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, ImportXFDFMultiSelect) {
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));
  static const char kXfdf[] =
      "<?xml version=\"1.0\"?>"
      "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\"><fields>"
      "<field name=\"Listbox_MultiSelect\">"
      "<value>Cherry</value><value>Apple</value>"
      "</field>"
      "</fields></xfdf>";

  EPDF_FORM_IMPORT_RESULT result;
  ASSERT_TRUE(EPDFForm_ImportXFDF(document(), kXfdf, sizeof(kXfdf) - 1, nullptr,
                                  0, &result));
  EXPECT_EQ(1u, result.fields_total);
  EXPECT_EQ(1u, result.fields_applied);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int multi = FieldIndexByName(model, L"Listbox_MultiSelect");
  ASSERT_GE(multi, 0);
  EXPECT_TRUE(EPDFForm_IsFieldOptionSelected(model, multi, 0));   // Apple
  EXPECT_FALSE(EPDFForm_IsFieldOptionSelected(model, multi, 1));  // Banana
  EXPECT_TRUE(EPDFForm_IsFieldOptionSelected(model, multi, 2));   // Cherry
  EPDFForm_CloseModel(model);
}

// The full circle: export XFDF from a filled document and re-import it into
// a pristine copy via a fresh layer - values must match exactly.
TEST_F(EPDFFormEmbedderTest, XfdfRoundTripPreservesValues) {
  LayerDoc first;
  ASSERT_TRUE(OpenLayer("toggle_fields.pdf", &first));
  ScopedFPDFWideString tricky = GetFPDFWideString(L"a<b>&\"c\" 'd'");
  ASSERT_TRUE(EPDFForm_SetTextValue(first.layer, 17u, tricky.get(), nullptr, 0,
                                    nullptr));

  unsigned long length =
      EPDFForm_ExportXFDF(first.layer, nullptr, 0, nullptr, 0);
  ASSERT_GT(length, 0u);
  std::vector<char> xfdf(length);
  ASSERT_EQ(length,
            EPDFForm_ExportXFDF(first.layer, nullptr, 0, xfdf.data(), length));

  LayerDoc second;
  ASSERT_TRUE(OpenLayer("toggle_fields.pdf", &second));
  EPDF_FORM_IMPORT_RESULT result;
  ASSERT_TRUE(EPDFForm_ImportXFDF(second.layer, xfdf.data(), length, nullptr, 0,
                                  &result));
  EXPECT_EQ(0u, result.fields_skipped);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(second.layer);
  ASSERT_TRUE(model);
  const int billing_name = FieldIndexByName(model, L"billing.name");
  ASSERT_GE(billing_name, 0);
  EXPECT_EQ(L"a<b>&\"c\" 'd'", GetCurrentFieldValue(model, billing_name));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, RepairLinksRecoveredFields) {
  ASSERT_TRUE(OpenDocument("orphan_widgets.pdf"));

  EPDF_FORM_REPAIR_REPORT report;
  ASSERT_TRUE(EPDFForm_Repair(document(), 0, &report));
  EXPECT_EQ(0u, report.acroform_created);
  EXPECT_EQ(2u, report.fields_linked);  // orphan_check + orphan_radio root
  EXPECT_EQ(0u, report.widgets_linked);
  EXPECT_EQ(0u, report.fields_unrepairable);

  // The reconciliation is now durable structure, not an in-memory patch.
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  ASSERT_EQ(3, EPDFForm_CountFields(model));
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(EPDF_FORMFIELD_ORIGIN_ACROFORM,
              EPDFForm_GetFieldOrigin(model, i));
  }
  EPDFForm_CloseModel(model);

  // Idempotent: a second pass fixes nothing.
  ASSERT_TRUE(EPDFForm_Repair(document(), 0, &report));
  EXPECT_EQ(0u, report.fields_linked);
  EXPECT_EQ(0u, report.widgets_linked);
}

TEST_F(EPDFFormEmbedderTest, RepairCreatesAcroFormAndLinksKids) {
  ASSERT_TRUE(OpenDocument("widgets_no_acroform.pdf"));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(EPDF_FORMKIND_NONE, EPDFForm_GetFormKind(model));
  ASSERT_EQ(2, EPDFForm_CountFields(model));
  EPDFForm_CloseModel(model);

  EPDF_FORM_REPAIR_REPORT report;
  ASSERT_TRUE(EPDFForm_Repair(document(), 0, &report));
  EXPECT_EQ(1u, report.acroform_created);
  EXPECT_EQ(2u, report.fields_linked);   // orphan_text + gap_radio
  EXPECT_EQ(1u, report.widgets_linked);  // widget 7 into gap_radio's /Kids
  EXPECT_EQ(0u, report.fields_unrepairable);

  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(EPDF_FORMKIND_ACROFORM, EPDFForm_GetFormKind(model));
  ASSERT_EQ(2, EPDFForm_CountFields(model));
  const int radio = EPDFForm_GetFieldIndexByObjNum(model, 5u);
  ASSERT_GE(radio, 0);
  EXPECT_EQ(EPDF_FORMFIELD_ORIGIN_ACROFORM,
            EPDFForm_GetFieldOrigin(model, radio));
  EXPECT_EQ(2, EPDFForm_CountFieldWidgets(model, radio));
  EPDFForm_CloseModel(model);
}

// Repair on a layer is a tiny structural delta, and it survives a delta
// save/reload: the repaired document stays repaired.
TEST_F(EPDFFormEmbedderTest, RepairOnLayerIsDurable) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("orphan_widgets.pdf", &doc));

  EPDF_FORM_REPAIR_REPORT report;
  ASSERT_TRUE(EPDFForm_Repair(doc.layer, 0, &report));
  EXPECT_EQ(2u, report.fields_linked);
  // /AcroForm lives inline in the catalog, so linking promotes exactly the
  // root object and nothing else.
  EXPECT_EQ(1ul, EPDFLayer_GetPromotedObjectCount(doc.layer));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, 1u));

  // Round-trip the delta into a second layer over the same base.
  ClearString();
  EPDFLayerSaveStatus save_status;
  ASSERT_TRUE(EPDFLayer_SaveDelta(doc.layer, this, &save_status));
  const std::string delta = GetString();
  ASSERT_FALSE(delta.empty());

  TestLoader loader(pdfium::as_bytes(pdfium::span(delta.data(), delta.size())));
  FPDF_FILEACCESS file_access = {};
  file_access.m_FileLen = static_cast<unsigned long>(delta.size());
  file_access.m_GetBlock = TestLoader::GetBlock;
  file_access.m_Param = &loader;

  EPDFLayerOpenStatus status;
  FPDF_DOCUMENT second =
      EPDFLayer_OpenLayer(doc.base, &file_access, nullptr, &status);
  ASSERT_TRUE(second);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(second);
  ASSERT_TRUE(model);
  ASSERT_EQ(3, EPDFForm_CountFields(model));
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(EPDF_FORMFIELD_ORIGIN_ACROFORM,
              EPDFForm_GetFieldOrigin(model, i));
  }
  EPDFForm_CloseModel(model);
  FPDF_CloseDocument(second);
}

TEST_F(EPDFFormEmbedderTest, RepairBakesMissingAppearances) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));

  EPDF_FORM_REPAIR_REPORT report;
  ASSERT_TRUE(
      EPDFForm_Repair(document(), EPDF_FORM_REPAIR_BAKE_APPEARANCES, &report));
  // maxlen_text (4) and billing.name (17) ship without /AP.
  EXPECT_GE(report.appearances_baked, 2u);
  EXPECT_EQ(0u, report.need_appearances_cleared);  // flag was never set

  // billing.name is /Annots index 7 on the page; it has an /AP now.
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);
  {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, 7));
    ASSERT_TRUE(annot);
    EXPECT_GT(FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                              nullptr, 0),
              2u);
  }
  UnloadPage(page);

  // Idempotent: everything has an appearance now.
  ASSERT_TRUE(
      EPDFForm_Repair(document(), EPDF_FORM_REPAIR_BAKE_APPEARANCES, &report));
  EXPECT_EQ(0u, report.appearances_baked);
}

TEST_F(EPDFFormEmbedderTest, RepairBakeClearsNeedAppearances) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(doc);
  RetainPtr<CPDF_Dictionary> acro_form =
      doc->GetMutableRoot()->GetMutableDictFor("AcroForm");
  ASSERT_TRUE(acro_form);
  acro_form->SetNewFor<CPDF_Boolean>("NeedAppearances", true);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_TRUE(EPDFForm_GetNeedAppearances(model));
  EPDFForm_CloseModel(model);

  EPDF_FORM_REPAIR_REPORT report;
  ASSERT_TRUE(
      EPDFForm_Repair(document(), EPDF_FORM_REPAIR_BAKE_APPEARANCES, &report));
  EXPECT_GT(report.appearances_baked, 0u);
  EXPECT_EQ(1u, report.need_appearances_cleared);
  EXPECT_FALSE(acro_form->KeyExist("NeedAppearances"));

  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_FALSE(EPDFForm_GetNeedAppearances(model));
  EPDFForm_CloseModel(model);
}

namespace {

// Create an unattached widget annotation through the ANNOTATION API - the
// authoring model's first step (widgets are born as annotations).
uint32_t CreateWidgetAnnot(FPDF_PAGE page,
                           float left,
                           float bottom,
                           float right,
                           float top) {
  // EPDFPage_CreateAnnot creates an INDIRECT annotation (durable object
  // number), unlike upstream FPDFPage_CreateAnnot.
  ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page, FPDF_ANNOT_WIDGET));
  if (!annot) {
    return 0;
  }
  FS_RECTF rect{left, top, right, bottom};
  if (!FPDFAnnot_SetRect(annot.get(), &rect)) {
    return 0;
  }
  return EPDFAnnot_GetObjectNumber(annot.get());
}

}  // namespace

TEST_F(EPDFFormEmbedderTest, CreateUnplacedFieldBootstrapsAcroForm) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));

  const uint32_t field = EPDFForm_CreateField(
      document(), 4 /* text */, GetFPDFWideString(L"billing.name").get());
  ASSERT_GT(field, 0u);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(EPDF_FORMKIND_ACROFORM, EPDFForm_GetFormKind(model));
  ASSERT_EQ(1, EPDFForm_CountFields(model));
  EXPECT_EQ(L"billing.name", GetWideString(EPDFForm_GetFieldName, model, 0));
  EXPECT_EQ(EPDF_FORMFIELD_FAMILY_TEXT, EPDFForm_GetFieldFamily(model, 0));
  EXPECT_EQ(EPDF_FORMFIELD_ORIGIN_ACROFORM, EPDFForm_GetFieldOrigin(model, 0));
  EXPECT_EQ(0, EPDFForm_CountFieldWidgets(model, 0));  // unplaced
  EPDFForm_CloseModel(model);

  // Sibling collisions fail without touching the tree.
  EXPECT_EQ(0u, EPDFForm_CreateField(document(), 4,
                                     GetFPDFWideString(L"billing.name").get()));
  EXPECT_EQ(0u, EPDFForm_CreateField(document(), 4,
                                     GetFPDFWideString(L"billing").get()));

  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(1, EPDFForm_CountFields(model));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, AttachWidgetsFormsARadioGroup) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);

  const uint32_t field = EPDFForm_CreateField(
      document(), 3 /* radio */, GetFPDFWideString(L"gender").get());
  ASSERT_GT(field, 0u);
  const uint32_t w1 = CreateWidgetAnnot(page, 20, 200, 40, 220);
  const uint32_t w2 = CreateWidgetAnnot(page, 60, 200, 80, 220);
  ASSERT_GT(w1, 0u);
  ASSERT_GT(w2, 0u);

  ASSERT_TRUE(EPDFForm_AttachWidget(document(), field, w1, "male"));
  ASSERT_TRUE(EPDFForm_AttachWidget(document(), field, w2, "female"));
  // Re-attaching an already attached widget fails.
  EXPECT_FALSE(EPDFForm_AttachWidget(document(), field, w1, "male"));
  // Toggles demand a usable on-state name.
  EXPECT_FALSE(EPDFForm_AttachWidget(document(), field, w1, nullptr));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int index = EPDFForm_GetFieldIndexByObjNum(model, field);
  ASSERT_GE(index, 0);
  ASSERT_EQ(2, EPDFForm_CountFieldWidgets(model, index));
  EXPECT_EQ("male", GetWidgetOnState(model, index, 0));
  EXPECT_EQ("female", GetWidgetOnState(model, index, 1));
  EPDFForm_CloseModel(model);

  // The newborn group is immediately fillable through the P1 transaction.
  unsigned long changed = 0;
  ASSERT_TRUE(
      EPDFForm_SetToggle(document(), field, "male", nullptr, 0, &changed));
  EXPECT_EQ(1ul, changed);
  model = EPDFForm_LoadModel(document());
  EXPECT_EQ(L"male", GetCurrentFieldValue(
                         model, EPDFForm_GetFieldIndexByObjNum(model, field)));
  EPDFForm_CloseModel(model);
  UnloadPage(page);
}

TEST_F(EPDFFormEmbedderTest, AttachToLegacyMergedFieldKeepsFieldId) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);
  const int annots_before = FPDFPage_GetAnnotCount(page);

  // maxlen_text (object 4) is a merged field/widget.
  const uint32_t widget = CreateWidgetAnnot(page, 20, 20, 280, 36);
  ASSERT_GT(widget, 0u);
  ASSERT_TRUE(EPDFForm_AttachWidget(document(), 4u, widget, nullptr));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int index = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  ASSERT_GE(index, 0);  // the FIELD object number never changes
  ASSERT_EQ(2, EPDFForm_CountFieldWidgets(model, index));
  // The split widget is a NEW object; neither widget is the field dict.
  EXPECT_NE(4u, EPDFForm_GetFieldWidgetObjNum(model, index, 0));
  EXPECT_EQ(widget, EPDFForm_GetFieldWidgetObjNum(model, index, 1));
  EPDFForm_CloseModel(model);

  // /Annots: merged entry swapped for the split widget, new widget appended.
  EXPECT_EQ(annots_before + 1, FPDFPage_GetAnnotCount(page));

  // Both widgets still fill together.
  ScopedFPDFWideString value = GetFPDFWideString(L"ab");
  ASSERT_TRUE(
      EPDFForm_SetTextValue(document(), 4u, value.get(), nullptr, 0, nullptr));
  UnloadPage(page);
}

TEST_F(EPDFFormEmbedderTest, DetachWidgetKeepsFieldVisible) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);

  const uint32_t field =
      EPDFForm_CreateField(document(), 4, GetFPDFWideString(L"note").get());
  const uint32_t widget = CreateWidgetAnnot(page, 20, 200, 200, 220);
  ASSERT_TRUE(EPDFForm_AttachWidget(document(), field, widget, nullptr));
  ASSERT_TRUE(EPDFForm_DetachWidget(document(), field, widget));
  // Detaching twice fails (no longer attached).
  EXPECT_FALSE(EPDFForm_DetachWidget(document(), field, widget));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int index = EPDFForm_GetFieldIndexByObjNum(model, field);
  ASSERT_GE(index, 0);  // the field survives, unplaced
  EXPECT_EQ(0, EPDFForm_CountFieldWidgets(model, index));
  // The widget is inert again: no field claims it.
  EXPECT_EQ(-1, EPDFForm_GetFieldIndexForWidget(model, widget));
  EPDFForm_CloseModel(model);
  UnloadPage(page);
}

TEST_F(EPDFFormEmbedderTest, DeleteFieldDetachesAndPrunesAncestors) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);

  const uint32_t field = EPDFForm_CreateField(
      document(), 4, GetFPDFWideString(L"billing.name").get());
  const uint32_t widget = CreateWidgetAnnot(page, 20, 200, 200, 220);
  ASSERT_TRUE(EPDFForm_AttachWidget(document(), field, widget, nullptr));

  uint32_t detached[4] = {};
  unsigned long detached_count = 0;
  ASSERT_TRUE(
      EPDFForm_DeleteField(document(), field, detached, 4, &detached_count));
  EXPECT_EQ(1ul, detached_count);
  EXPECT_EQ(widget, detached[0]);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  // The empty "billing" ancestor was pruned along with the field.
  EXPECT_EQ(0, EPDFForm_CountFields(model));
  EPDFForm_CloseModel(model);
  UnloadPage(page);
}

TEST_F(EPDFFormEmbedderTest, FieldSettersValidateAndApply) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));

  // Rename: the /T segment only; sibling collisions fail.
  ASSERT_TRUE(EPDFForm_SetFieldName(document(), 17u,
                                    GetFPDFWideString(L"fullName").get()));
  EXPECT_FALSE(EPDFForm_SetFieldName(document(), 4u,
                                     GetFPDFWideString(L"unison_radio").get()));
  EXPECT_FALSE(
      EPDFForm_SetFieldName(document(), 4u, GetFPDFWideString(L"a.b").get()));

  // Flags: masked update works; family-defining bits are immutable.
  ASSERT_TRUE(EPDFForm_SetFieldFlags(document(), 4u, 1u << 1, 0));  // +Required
  EXPECT_FALSE(EPDFForm_SetFieldFlags(document(), 4u, 1u << 15, 0));

  // MaxLen: cannot cut below the current value ("abc").
  EXPECT_FALSE(EPDFForm_SetFieldMaxLen(document(), 4u, 2));
  ASSERT_TRUE(EPDFForm_SetFieldMaxLen(document(), 4u, 10));

  ScopedFPDFWideString default_value = GetFPDFWideString(L"dflt");
  FPDF_WIDESTRING default_values[] = {default_value.get()};
  ASSERT_TRUE(
      EPDFForm_SetFieldDefaultValues(document(), 4u, default_values, 1));
  ASSERT_TRUE(EPDFForm_SetFieldAlternateName(
      document(), 4u, GetFPDFWideString(L"Your name").get()));
  ASSERT_TRUE(EPDFForm_SetFieldMappingName(document(), 4u,
                                           GetFPDFWideString(L"name_x").get()));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  int index = EPDFForm_GetFieldIndexByObjNum(model, 17u);
  EXPECT_EQ(L"billing.fullName",
            GetWideString(EPDFForm_GetFieldName, model, index));
  index = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  EXPECT_TRUE(EPDFForm_GetFieldFlags(model, index) & (1u << 1));
  EXPECT_EQ(10, EPDFForm_GetFieldMaxLen(model, index));
  EXPECT_EQ(L"dflt", GetDefaultFieldValue(model, index));
  EXPECT_EQ(L"Your name",
            GetWideString(EPDFForm_GetFieldAlternateName, model, index));
  EXPECT_EQ(L"name_x",
            GetWideString(EPDFForm_GetFieldMappingName, model, index));
  EPDFForm_CloseModel(model);

  // Reset now restores the fresh /DV through the P1 transaction.
  ASSERT_TRUE(EPDFForm_ResetField(document(), 4u, nullptr, 0, nullptr));
  model = EPDFForm_LoadModel(document());
  index = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  EXPECT_EQ(L"dflt", GetCurrentFieldValue(model, index));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, EmptySettersShadowInheritedProperties) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  RetainPtr<CPDF_Dictionary> parent =
      GetMutableIndirectDictionary(document(), 16u);
  ASSERT_TRUE(parent);
  parent->SetNewFor<CPDF_Number>("MaxLen", 8);
  parent->SetNewFor<CPDF_String>(pdfium::form_fields::kTU, L"Parent tooltip");
  parent->SetNewFor<CPDF_String>(pdfium::form_fields::kTM, L"parent_mapping");
  parent->SetNewFor<CPDF_String>(pdfium::form_fields::kV, L"Parent value");

  // Object 17 inherits /FT and these properties from object 16. Clearing the
  // effective child properties must not mutate the shared parent.
  ASSERT_TRUE(EPDFForm_SetFieldMaxLen(document(), 17u, 0));
  ASSERT_TRUE(EPDFForm_SetFieldAlternateName(document(), 17u,
                                             GetFPDFWideString(L"").get()));
  ASSERT_TRUE(EPDFForm_SetFieldMappingName(document(), 17u,
                                           GetFPDFWideString(L"").get()));
  ASSERT_TRUE(EPDFForm_ResetField(document(), 17u, nullptr, 0, nullptr));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int field = EPDFForm_GetFieldIndexByObjNum(model, 17u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(0, EPDFForm_GetFieldMaxLen(model, field));
  EXPECT_EQ(L"", GetWideString(EPDFForm_GetFieldAlternateName, model, field));
  EXPECT_EQ(L"", GetWideString(EPDFForm_GetFieldMappingName, model, field));
  EXPECT_EQ(EPDF_FORM_VALUE_SCALAR, EPDFForm_GetFieldValueKind(model, field));
  EXPECT_EQ(L"", GetCurrentFieldValue(model, field));
  EPDFForm_CloseModel(model);

  EXPECT_EQ(8, parent->GetIntegerFor("MaxLen"));
  EXPECT_EQ(L"Parent tooltip",
            parent->GetUnicodeTextFor(pdfium::form_fields::kTU));
  EXPECT_EQ(L"parent_mapping",
            parent->GetUnicodeTextFor(pdfium::form_fields::kTM));
  EXPECT_EQ(L"Parent value",
            parent->GetUnicodeTextFor(pdfium::form_fields::kV));
  RetainPtr<const CPDF_Dictionary> child =
      GetEffectiveIndirectDictionary(document(), 17u);
  ASSERT_TRUE(child);
  EXPECT_EQ(0, child->GetIntegerFor("MaxLen"));
  EXPECT_TRUE(child->KeyExist(pdfium::form_fields::kTU));
  EXPECT_TRUE(child->KeyExist(pdfium::form_fields::kTM));
  EXPECT_TRUE(child->KeyExist(pdfium::form_fields::kV));
}

TEST_F(EPDFFormEmbedderTest, SetFieldOptionsResyncsSelection) {
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  int index = FieldIndexByName(model, L"Listbox_MultiSelectMultipleValues");
  ASSERT_GE(index, 0);
  const uint32_t field = EPDFForm_GetFieldObjNum(model, index);
  EPDFForm_CloseModel(model);

  // Current /V is [Epsilon, Gamma]; the new option list keeps only Gamma.
  ScopedFPDFWideString alpha = GetFPDFWideString(L"Alpha");
  ScopedFPDFWideString gamma = GetFPDFWideString(L"Gamma");
  ScopedFPDFWideString zeta = GetFPDFWideString(L"Zeta");
  ScopedFPDFWideString epsilon = GetFPDFWideString(L"Epsilon");
  FPDF_WIDESTRING defaults[] = {epsilon.get(), gamma.get()};
  ASSERT_TRUE(EPDFForm_SetFieldDefaultValues(document(), field, defaults, 2));
  FPDF_WIDESTRING labels[] = {alpha.get(), gamma.get(), zeta.get()};
  ASSERT_TRUE(EPDFForm_SetFieldOptions(document(), field, labels, labels, 3));

  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  index = EPDFForm_GetFieldIndexByObjNum(model, field);
  ASSERT_EQ(3, EPDFForm_CountFieldOptions(model, index));
  EXPECT_FALSE(EPDFForm_IsFieldOptionSelected(model, index, 0));  // Alpha
  EXPECT_TRUE(EPDFForm_IsFieldOptionSelected(model, index, 1));   // Gamma kept
  EXPECT_FALSE(EPDFForm_IsFieldOptionSelected(model, index, 2));  // Zeta
  EXPECT_EQ(EPDF_FORM_VALUE_SCALAR,
            EPDFForm_GetFieldDefaultValueKind(model, index));
  EXPECT_EQ(L"Gamma", GetDefaultFieldValue(model, index));
  EPDFForm_CloseModel(model);

  ASSERT_TRUE(EPDFForm_ResetField(document(), field, nullptr, 0, nullptr));
  model = EPDFForm_LoadModel(document());
  index = EPDFForm_GetFieldIndexByObjNum(model, field);
  EXPECT_EQ(L"Gamma", GetCurrentFieldValue(model, index));
  EPDFForm_CloseModel(model);

  RetainPtr<const CPDF_Dictionary> field_dictionary =
      GetEffectiveIndirectDictionary(document(), field);
  ASSERT_TRUE(field_dictionary);
  RetainPtr<const CPDF_Array> indices = field_dictionary->GetArrayFor("I");
  ASSERT_TRUE(indices);
  ASSERT_EQ(1u, indices->size());
  EXPECT_EQ(1, indices->GetIntegerAt(0));
}

TEST_F(EPDFFormEmbedderTest, FieldFlagsRejectInvalidChoiceShapeTransitions) {
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));

  // Object 12 has array /V and MultiSelect. Clearing MultiSelect would make
  // the existing /V invalid, so the transaction is rejected unchanged.
  EXPECT_FALSE(EPDFForm_SetFieldFlags(document(), 12u, 0,
                                      pdfium::form_flags::kChoiceMultiSelect));
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  int field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  ASSERT_GE(field, 0);
  EXPECT_TRUE(EPDFForm_GetFieldFlags(model, field) &
              pdfium::form_flags::kChoiceMultiSelect);
  EXPECT_EQ(EPDF_FORM_VALUE_ARRAY, EPDFForm_GetFieldValueKind(model, field));
  EPDFForm_CloseModel(model);

  // Edit is a combo-only flag; setting it on a list box is invalid.
  EXPECT_FALSE(EPDFForm_SetFieldFlags(document(), 8u,
                                      pdfium::form_flags::kChoiceEdit, 0));
}

// Authoring on a layer produces a minimal, durable delta.
TEST_F(EPDFFormEmbedderTest, AuthoringOnLayerIsDurable) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("hello_world.pdf", &doc));

  const uint32_t field = EPDFForm_CreateField(
      doc.layer, 4, GetFPDFWideString(L"layer_field").get());
  ASSERT_GT(field, 0u);
  FPDF_PAGE page = FPDF_LoadPage(doc.layer, 0);
  ASSERT_TRUE(page);
  const uint32_t widget = CreateWidgetAnnot(page, 20, 200, 200, 220);
  ASSERT_GT(widget, 0u);
  ASSERT_TRUE(EPDFForm_AttachWidget(doc.layer, field, widget, nullptr));
  FPDF_ClosePage(page);

  // Duplicate create fails without growing the delta.
  const unsigned long promoted = EPDFLayer_GetPromotedObjectCount(doc.layer);
  EXPECT_EQ(0u, EPDFForm_CreateField(doc.layer, 4,
                                     GetFPDFWideString(L"layer_field").get()));
  EXPECT_EQ(promoted, EPDFLayer_GetPromotedObjectCount(doc.layer));

  ClearString();
  EPDFLayerSaveStatus save_status;
  ASSERT_TRUE(EPDFLayer_SaveDelta(doc.layer, this, &save_status));
  const std::string delta = GetString();
  ASSERT_FALSE(delta.empty());

  TestLoader loader(pdfium::as_bytes(pdfium::span(delta.data(), delta.size())));
  FPDF_FILEACCESS file_access = {};
  file_access.m_FileLen = static_cast<unsigned long>(delta.size());
  file_access.m_GetBlock = TestLoader::GetBlock;
  file_access.m_Param = &loader;
  EPDFLayerOpenStatus status;
  FPDF_DOCUMENT second =
      EPDFLayer_OpenLayer(doc.base, &file_access, nullptr, &status);
  ASSERT_TRUE(second);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(second);
  ASSERT_TRUE(model);
  const int index = EPDFForm_GetFieldIndexByObjNum(model, field);
  ASSERT_GE(index, 0);
  EXPECT_EQ(L"layer_field", GetWideString(EPDFForm_GetFieldName, model, index));
  EXPECT_EQ(1, EPDFForm_CountFieldWidgets(model, index));
  EPDFForm_CloseModel(model);
  FPDF_CloseDocument(second);
}
