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
#include "public/epdf_action.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/embedpdf_form_writes.h"
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

// Write one value (EPDF_FORM_VALUE_SCALAR) as a field's /V or /DV.
bool SetOneValue(FPDF_DOCUMENT document,
                 uint32_t field_objnum,
                 const std::wstring& text) {
  ScopedFPDFWideString value = GetFPDFWideString(text);
  FPDF_WIDESTRING values[] = {value.get()};
  return EPDFForm_SetFieldValue(document, field_objnum, EPDF_FORM_VALUE_SCALAR,
                                values, 1);
}

bool SetOneDefault(FPDF_DOCUMENT document,
                   uint32_t field_objnum,
                   const std::wstring& text) {
  ScopedFPDFWideString value = GetFPDFWideString(text);
  FPDF_WIDESTRING values[] = {value.get()};
  return EPDFForm_SetFieldDefaultValue(document, field_objnum,
                                       EPDF_FORM_VALUE_SCALAR, values, 1);
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
  FS_RECTF rect;
  ASSERT_TRUE(EPDFForm_GetFieldWidgetRect(model, 0, 0, &rect));
  EXPECT_FLOAT_EQ(100.0f, rect.left);
  EXPECT_FLOAT_EQ(100.0f, rect.bottom);
  EXPECT_FLOAT_EQ(200.0f, rect.right);
  EXPECT_FLOAT_EQ(130.0f, rect.top);
  EXPECT_FALSE(EPDFForm_GetFieldWidgetRect(model, 0, 1, &rect));
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

TEST_F(EPDFFormEmbedderTest, WidgetOnDeletedPageIsOnNoPage) {
  ASSERT_TRUE(OpenDocument("widget_on_deleted_page.pdf"));
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  ASSERT_EQ(1, EPDFForm_CountFields(model));
  EXPECT_EQ(L"gone", GetWideString(EPDFForm_GetFieldName, model, 0));
  ASSERT_EQ(1, EPDFForm_CountFieldWidgets(model, 0));
  EXPECT_EQ(5u, EPDFForm_GetFieldWidgetObjNum(model, 0, 0));
  // Its /P names a page the page tree no longer holds: it is on no page.
  EXPECT_EQ(0u, EPDFForm_GetFieldWidgetPageObjNum(model, 0, 0));
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

  // Turning the widgets on flips /AS on BOTH twins — above all the page
  // twin, the only one the user can see.
  uint32_t changed[4] = {};
  unsigned long changed_count = 0;
  const FPDF_BOOL both_on[] = {true, true};
  ASSERT_TRUE(EPDFForm_SetFieldWidgetsChecked(document(), 4u, both_on, 2,
                                              changed, 4, &changed_count));
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

  // A text value lands on the page twin too, and its new picture shows it —
  // even though this document has no /AcroForm /DR: generation must seed a
  // fallback font instead of vetoing the appearance.
  ASSERT_TRUE(SetOneValue(document(), 5u, L"TWIN"));
  RetainPtr<const CPDF_Dictionary> page_twin =
      GetEffectiveIndirectDictionary(document(), 10u);
  ASSERT_TRUE(page_twin);
  EXPECT_EQ(L"TWIN", page_twin->GetUnicodeTextFor(pdfium::form_fields::kV));
  changed_count = 0;
  ASSERT_TRUE(
      EPDFForm_RedrawFieldWidgets(document(), 5u, changed, 4, &changed_count));
  ASSERT_EQ(2ul, changed_count);
  EXPECT_EQ(5u, changed[0]);
  EXPECT_EQ(10u, changed[1]);
  const std::wstring appearance =
      GetEffectiveWidgetAppearance(document(), 10u);
  EXPECT_NE(std::wstring::npos, appearance.find(L"TWIN")) << appearance;

  // The redraw seeded /DR/Font with the /DA-named font.
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

// The radio walkthrough: switching the group promotes exactly the field
// plus the two widgets whose /AS changed - the minimal FDF-shaped delta.
TEST_F(EPDFFormEmbedderTest, WidgetsCheckedOnLayerPromoteOnlyWhatChanged) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("orphan_widgets.pdf", &doc));

  uint32_t changed[4] = {};
  unsigned long changed_count = 0;
  const FPDF_BOOL second_on[] = {false, true};
  ASSERT_TRUE(EPDFForm_SetFieldWidgetsChecked(doc.layer, 6u, second_on, 2,
                                              changed, 4, &changed_count));
  EXPECT_EQ(2ul, changed_count);
  EXPECT_EQ(8u, changed[0]);  // /AS a -> Off
  EXPECT_EQ(9u, changed[1]);  // /AS Off -> b
  ASSERT_TRUE(SetOneValue(doc.layer, 6u, L"b"));
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

  // Writing what the file already holds writes nothing and promotes nothing
  // further.
  ASSERT_TRUE(EPDFForm_SetFieldWidgetsChecked(doc.layer, 6u, second_on, 2,
                                              nullptr, 0, &changed_count));
  EXPECT_EQ(0ul, changed_count);
  ASSERT_TRUE(SetOneValue(doc.layer, 6u, L"b"));
  EXPECT_EQ(3ul, EPDFLayer_GetPromotedObjectCount(doc.layer));
}

// A refused write must be side-effect free: zero objects promoted.
TEST_F(EPDFFormEmbedderTest, RefusedValueWritesChangeNothing) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("orphan_widgets.pdf", &doc));

  // One entry per widget, or nothing.
  const FPDF_BOOL one_on[] = {true};
  EXPECT_FALSE(EPDFForm_SetFieldWidgetsChecked(doc.layer, 6u, one_on, 1,
                                               nullptr, 0, nullptr));
  // Not a checkbox or radio group (the text field), or no field at all.
  EXPECT_FALSE(EPDFForm_SetFieldWidgetsChecked(doc.layer, 4u, one_on, 1,
                                               nullptr, 0, nullptr));
  EXPECT_FALSE(EPDFForm_SetFieldWidgetsChecked(doc.layer, 9999u, one_on, 1,
                                               nullptr, 0, nullptr));
  // A value's count must fit its kind, and only a list box takes an array.
  EXPECT_FALSE(EPDFForm_SetFieldValue(doc.layer, 4u, EPDF_FORM_VALUE_SCALAR,
                                      nullptr, 0));
  ScopedFPDFWideString b = GetFPDFWideString(L"b");
  FPDF_WIDESTRING values[] = {b.get()};
  EXPECT_FALSE(
      EPDFForm_SetFieldValue(doc.layer, 4u, EPDF_FORM_VALUE_NONE, values, 1));
  EXPECT_FALSE(
      EPDFForm_SetFieldValue(doc.layer, 6u, EPDF_FORM_VALUE_ARRAY, values, 1));
  EXPECT_FALSE(EPDFForm_SetFieldValue(doc.layer, 6u, 99, values, 1));
  // /I is a choice field's, and a position is never negative.
  const int indices[] = {0};
  EXPECT_FALSE(EPDFForm_SetFieldSelectedIndices(doc.layer, 4u, indices, 1));
  // Only text and choice fields are redrawn from their value.
  EXPECT_FALSE(EPDFForm_RedrawFieldWidgets(doc.layer, 6u, nullptr, 0, nullptr));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(doc.layer));
}

TEST_F(EPDFFormEmbedderTest, ToggleWidgetsTurnOnTheirOwnState) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));

  // Radios in unison: the caller turns on both /u1 widgets.
  unsigned long changed_count = 0;
  const FPDF_BOOL u1_on[] = {true, true, false};
  ASSERT_TRUE(EPDFForm_SetFieldWidgetsChecked(document(), 8u, u1_on, 3, nullptr,
                                              0, &changed_count));
  EXPECT_EQ(2ul, changed_count);
  // Switching to /u2 flips all three.
  const FPDF_BOOL u2_on[] = {false, false, true};
  ASSERT_TRUE(EPDFForm_SetFieldWidgetsChecked(document(), 8u, u2_on, 3, nullptr,
                                              0, &changed_count));
  EXPECT_EQ(3ul, changed_count);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int field = EPDFForm_GetFieldIndexByObjNum(model, 8u);
  ASSERT_GE(field, 0);
  EXPECT_FALSE(EPDFForm_IsFieldWidgetChecked(model, field, 0));
  EXPECT_FALSE(EPDFForm_IsFieldWidgetChecked(model, field, 1));
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, field, 2));
  EPDFForm_CloseModel(model);
}

// A checkbox's or radio group's value is a name, written as the caller
// gives it: the fork turns no on-state into a position, even for a field
// with /Opt. The export value stays on the widget.
TEST_F(EPDFFormEmbedderTest, ToggleValueIsTheNameItIsGiven) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));

  const FPDF_BOOL on[] = {true};
  ASSERT_TRUE(EPDFForm_SetFieldWidgetsChecked(document(), 12u, on, 1, nullptr,
                                              0, nullptr));
  ASSERT_TRUE(SetOneValue(document(), 12u, L"On"));
  RetainPtr<const CPDF_Dictionary> dict =
      GetEffectiveIndirectDictionary(document(), 12u);
  ASSERT_TRUE(dict);
  RetainPtr<const CPDF_Object> value =
      dict->GetObjectFor(pdfium::form_fields::kV);
  ASSERT_TRUE(value);
  EXPECT_TRUE(value->IsName());
  EXPECT_EQ("On", value->GetString());

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  ASSERT_GE(field, 0);
  EXPECT_TRUE(EPDFForm_IsFieldWidgetChecked(model, field, 0));
  EXPECT_EQ(L"On", GetCurrentFieldValue(model, field));
  EXPECT_EQ(L"Alpha", GetWidgetExportValue(model, field, 0));
  EPDFForm_CloseModel(model);

  // Clearing: every widget off, and the caller's /Off.
  const FPDF_BOOL off[] = {false};
  ASSERT_TRUE(EPDFForm_SetFieldWidgetsChecked(document(), 12u, off, 1, nullptr,
                                              0, nullptr));
  ASSERT_TRUE(SetOneValue(document(), 12u, L"Off"));
  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_FALSE(EPDFForm_IsFieldWidgetChecked(
      model, EPDFForm_GetFieldIndexByObjNum(model, 12u), 0));
  EXPECT_EQ(L"Off", GetCurrentFieldValue(
                        model, EPDFForm_GetFieldIndexByObjNum(model, 12u)));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, TextValueAndItsPicture) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));

  ASSERT_TRUE(SetOneValue(document(), 4u, L"Hello EmbedPDF"));
  uint32_t changed[2] = {};
  unsigned long changed_count = 0;
  ASSERT_TRUE(
      EPDFForm_RedrawFieldWidgets(document(), 4u, changed, 2, &changed_count));
  EXPECT_EQ(1ul, changed_count);
  EXPECT_EQ(4u, changed[0]);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(L"Hello EmbedPDF", GetCurrentFieldValue(model, 0));
  EPDFForm_CloseModel(model);

  // The widget's normal appearance stream was drawn again.
  EXPECT_NE(std::wstring::npos,
            GetEffectiveWidgetAppearance(document(), 4u).find(L"Hello"));
}

// The fork cuts nothing: a value longer than /MaxLen is written whole (the
// engine cuts it first). Only the field and what its picture needs promote.
TEST_F(EPDFFormEmbedderTest, TextValueOnLayerIsWrittenWhole) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("toggle_fields.pdf", &doc));

  ASSERT_TRUE(embedpdf_test::FillTextField(doc.layer, 4u, L"abcdef"));
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(doc.layer, 4u));
  EXPECT_LE(EPDFLayer_GetPromotedObjectCount(doc.layer), 4ul);
  const unsigned long promoted = EPDFLayer_GetPromotedObjectCount(doc.layer);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(doc.layer);
  ASSERT_TRUE(model);
  const int field = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(L"abcdef", GetCurrentFieldValue(model, field));
  EPDFForm_CloseModel(model);

  // The same value again writes nothing.
  ASSERT_TRUE(SetOneValue(doc.layer, 4u, L"abcdef"));
  EXPECT_EQ(promoted, EPDFLayer_GetPromotedObjectCount(doc.layer));
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

// A choice field's value and its /I are written as given: the fork checks
// no option and orders nothing.
TEST_F(EPDFFormEmbedderTest, ChoiceValueAndIndicesAreWrittenAsGiven) {
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

  ASSERT_TRUE(
      embedpdf_test::ChooseOption(document(), combo1_objnum, L"Cherry", 2));
  // Free text: the value, and no /I.
  ASSERT_TRUE(SetOneValue(document(), editable_objnum, L"NotAnOption"));
  ASSERT_TRUE(EPDFForm_SetFieldSelectedIndices(document(), editable_objnum,
                                               nullptr, 0));

  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(L"Cherry", GetCurrentFieldValue(model, combo1));
  EXPECT_TRUE(EPDFForm_IsFieldOptionSelected(model, combo1, 2));
  EXPECT_EQ(L"NotAnOption", GetCurrentFieldValue(model, editable));
  EPDFForm_CloseModel(model);
  RetainPtr<const CPDF_Dictionary> editable_dict =
      GetEffectiveIndirectDictionary(document(), editable_objnum);
  ASSERT_TRUE(editable_dict);
  EXPECT_FALSE(editable_dict->KeyExist("I"));
}

TEST_F(EPDFFormEmbedderTest, ListBoxValuesAreAnArray) {
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int multi = FieldIndexByName(model, L"Listbox_MultiSelect");
  ASSERT_GE(multi, 0);
  const uint32_t multi_objnum = EPDFForm_GetFieldObjNum(model, multi);
  EPDFForm_CloseModel(model);

  ScopedFPDFWideString apple = GetFPDFWideString(L"Apple");
  ScopedFPDFWideString cherry = GetFPDFWideString(L"Cherry");
  FPDF_WIDESTRING two_values[] = {apple.get(), cherry.get()};
  ASSERT_TRUE(EPDFForm_SetFieldValue(document(), multi_objnum,
                                     EPDF_FORM_VALUE_ARRAY, two_values, 2));
  const int two_indices[] = {0, 2};
  ASSERT_TRUE(EPDFForm_SetFieldSelectedIndices(document(), multi_objnum,
                                               two_indices, 2));

  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_EQ(EPDF_FORM_VALUE_ARRAY, EPDFForm_GetFieldValueKind(model, multi));
  EXPECT_TRUE(EPDFForm_IsFieldOptionSelected(model, multi, 0));   // Apple
  EXPECT_FALSE(EPDFForm_IsFieldOptionSelected(model, multi, 1));  // Banana
  EXPECT_TRUE(EPDFForm_IsFieldOptionSelected(model, multi, 2));   // Cherry
  EPDFForm_CloseModel(model);

  // None: no value, no /I.
  ASSERT_TRUE(EPDFForm_SetFieldValue(document(), multi_objnum,
                                     EPDF_FORM_VALUE_NONE, nullptr, 0));
  ASSERT_TRUE(
      EPDFForm_SetFieldSelectedIndices(document(), multi_objnum, nullptr, 0));
  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_FALSE(EPDFForm_IsFieldOptionSelected(model, multi, 0));
  EXPECT_FALSE(EPDFForm_IsFieldOptionSelected(model, multi, 2));
  EPDFForm_CloseModel(model);
}

// None removes a field's own value, unless a parent field holds one: then
// an empty value of the field's own keeps the parent's from showing.
TEST_F(EPDFFormEmbedderTest, NoneShadowsAParentFieldsValue) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  RetainPtr<CPDF_Dictionary> parent =
      GetMutableIndirectDictionary(document(), 16u);
  ASSERT_TRUE(parent);
  parent->SetNewFor<CPDF_String>(pdfium::form_fields::kV, L"Parent value");

  // Object 17 inherits /FT and /V from object 16.
  ASSERT_TRUE(EPDFForm_SetFieldValue(document(), 17u, EPDF_FORM_VALUE_NONE,
                                     nullptr, 0));
  RetainPtr<const CPDF_Dictionary> child =
      GetEffectiveIndirectDictionary(document(), 17u);
  ASSERT_TRUE(child);
  ASSERT_TRUE(child->KeyExist(pdfium::form_fields::kV));
  EXPECT_EQ(L"", child->GetUnicodeTextFor(pdfium::form_fields::kV));
  EXPECT_EQ(L"Parent value",
            parent->GetUnicodeTextFor(pdfium::form_fields::kV));

  // Without the parent's value, none is no key at all.
  parent->RemoveFor(pdfium::form_fields::kV);
  ASSERT_TRUE(EPDFForm_SetFieldValue(document(), 17u, EPDF_FORM_VALUE_NONE,
                                     nullptr, 0));
  child = GetEffectiveIndirectDictionary(document(), 17u);
  ASSERT_TRUE(child);
  EXPECT_FALSE(child->KeyExist(pdfium::form_fields::kV));
}

// A default is written as given, in the caller's order.
TEST_F(EPDFFormEmbedderTest, DefaultValueIsWrittenAsGiven) {
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));

  ScopedFPDFWideString epsilon = GetFPDFWideString(L"Epsilon");
  ScopedFPDFWideString gamma = GetFPDFWideString(L"Gamma");
  FPDF_WIDESTRING defaults[] = {epsilon.get(), gamma.get()};
  ASSERT_TRUE(EPDFForm_SetFieldDefaultValue(
      document(), 12u, EPDF_FORM_VALUE_ARRAY, defaults, 2));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(EPDF_FORM_VALUE_ARRAY,
            EPDFForm_GetFieldDefaultValueKind(model, field));
  ASSERT_EQ(2, EPDFForm_CountFieldDefaultValues(model, field));
  EXPECT_EQ(L"Epsilon", GetDefaultFieldValue(model, field, 0));
  EXPECT_EQ(L"Gamma", GetDefaultFieldValue(model, field, 1));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, MultiSelectValuesAreLayerDurable) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("listbox_form.pdf", &doc));

  ScopedFPDFWideString gamma = GetFPDFWideString(L"Gamma");
  ScopedFPDFWideString epsilon = GetFPDFWideString(L"Epsilon");
  FPDF_WIDESTRING values[] = {gamma.get(), epsilon.get()};
  ASSERT_TRUE(EPDFForm_SetFieldDefaultValue(doc.layer, 12u,
                                            EPDF_FORM_VALUE_ARRAY, values, 2));
  EXPECT_EQ(1ul, EPDFLayer_GetPromotedObjectCount(doc.layer));
  // The value, its indices and its picture, as a reset to that default
  // writes them.
  ASSERT_TRUE(
      EPDFForm_SetFieldValue(doc.layer, 12u, EPDF_FORM_VALUE_ARRAY, values, 2));
  const int indices[] = {2, 4};
  ASSERT_TRUE(EPDFForm_SetFieldSelectedIndices(doc.layer, 12u, indices, 2));
  ASSERT_TRUE(EPDFForm_RedrawFieldWidgets(doc.layer, 12u, nullptr, 0, nullptr));
  // The new picture also promotes its shared resource object.
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
  ASSERT_EQ(2, EPDFForm_CountFieldValues(model, field));
  EXPECT_EQ(L"Gamma", GetCurrentFieldValue(model, field, 0));
  EXPECT_EQ(L"Epsilon", GetCurrentFieldValue(model, field, 1));
  EXPECT_TRUE(EPDFForm_IsFieldOptionSelected(model, field, 2));
  EXPECT_TRUE(EPDFForm_IsFieldOptionSelected(model, field, 4));
  EPDFForm_CloseModel(model);
  FPDF_CloseDocument(reopened);
}

TEST_F(EPDFFormEmbedderTest, EmptyTextDefaultIsScalarAndCanBeRemoved) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));

  ASSERT_TRUE(SetOneDefault(document(), 4u, L""));
  EXPECT_FALSE(EPDFForm_SetFieldDefaultValue(
      document(), 4u, EPDF_FORM_VALUE_SCALAR, nullptr, 0));
  ScopedFPDFWideString empty = GetFPDFWideString(L"");
  FPDF_WIDESTRING too_many[] = {empty.get(), empty.get()};
  EXPECT_FALSE(EPDFForm_SetFieldDefaultValue(
      document(), 4u, EPDF_FORM_VALUE_ARRAY, too_many, 2));
  ASSERT_TRUE(SetOneValue(document(), 4u, L""));

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

  ASSERT_TRUE(EPDFForm_SetFieldDefaultValue(document(), 4u,
                                            EPDF_FORM_VALUE_NONE, nullptr, 0));
  model = EPDFForm_LoadModel(document());
  field = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  EXPECT_EQ(EPDF_FORM_VALUE_NONE,
            EPDFForm_GetFieldDefaultValueKind(model, field));
  EPDFForm_CloseModel(model);
}

// A checkbox's default is a name too, written as given.
TEST_F(EPDFFormEmbedderTest, ToggleDefaultIsTheNameItIsGiven) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));

  ASSERT_TRUE(SetOneDefault(document(), 12u, L"On"));
  RetainPtr<const CPDF_Dictionary> dict =
      GetEffectiveIndirectDictionary(document(), 12u);
  ASSERT_TRUE(dict);
  RetainPtr<const CPDF_Object> stored =
      dict->GetObjectFor(pdfium::form_fields::kDV);
  ASSERT_TRUE(stored);
  EXPECT_TRUE(stored->IsName());
  EXPECT_EQ("On", stored->GetString());

  ASSERT_TRUE(SetOneDefault(document(), 12u, L"Off"));
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  EXPECT_EQ(L"Off", GetDefaultFieldValue(model, field));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, MalformedDefaultReadsAsUnsupported) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  RetainPtr<CPDF_Dictionary> field =
      GetMutableIndirectDictionary(document(), 4u);
  ASSERT_TRUE(field);
  field->SetNewFor<CPDF_Number>(pdfium::form_fields::kDV, 42);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int index = EPDFForm_GetFieldIndexByObjNum(model, 4u);
  EXPECT_EQ(EPDF_FORM_VALUE_UNSUPPORTED,
            EPDFForm_GetFieldDefaultValueKind(model, index));
  EPDFForm_CloseModel(model);
}

TEST_F(EPDFFormEmbedderTest, ListBoxTopIndexIsRead) {
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));
  RetainPtr<CPDF_Dictionary> list =
      GetMutableIndirectDictionary(document(), 12u);
  ASSERT_TRUE(list);
  list->SetNewFor<CPDF_Number>("TI", 3);

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int field = EPDFForm_GetFieldIndexByObjNum(model, 12u);
  ASSERT_GE(field, 0);
  EXPECT_EQ(3, EPDFForm_GetFieldTopIndex(model, field));
  const int other = FieldIndexByName(model, L"Listbox_SingleSelect");
  ASSERT_GE(other, 0);
  EXPECT_EQ(0, EPDFForm_GetFieldTopIndex(model, other));
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

// FDF export reads a filled layer's values.
TEST_F(EPDFFormEmbedderTest, FdfExportReadsALayersValues) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("orphan_widgets.pdf", &doc));
  ASSERT_TRUE(embedpdf_test::FillTextField(doc.layer, 4u, L"Bob"));
  const std::string fdf = ExportFdf(doc.layer);
  ASSERT_FALSE(fdf.empty());
  EXPECT_NE(std::string::npos, fdf.find("Bob"));
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

// XFDF export escapes what a value holds.
TEST_F(EPDFFormEmbedderTest, XfdfExportEscapesValues) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("toggle_fields.pdf", &doc));
  ASSERT_TRUE(SetOneValue(doc.layer, 17u, L"a<b>&\"c\" 'd'"));
  const std::string xfdf = ExportXfdf(doc.layer);
  EXPECT_NE(std::string::npos,
            xfdf.find("<value>a&lt;b&gt;&amp;&quot;c&quot; &apos;d&apos;"
                      "</value>"))
      << xfdf;
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
      document(), 4 /* text */, GetFPDFWideString(L"billing.name").get(), 0);
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
  EXPECT_EQ(0u,
            EPDFForm_CreateField(document(), 4,
                                 GetFPDFWideString(L"billing.name").get(), 0));
  EXPECT_EQ(0u, EPDFForm_CreateField(document(), 4,
                                     GetFPDFWideString(L"billing").get(), 0));

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
      document(), 3 /* radio */, GetFPDFWideString(L"gender").get(), 0);
  ASSERT_GT(field, 0u);
  const uint32_t w1 = CreateWidgetAnnot(page, 20, 200, 40, 220);
  const uint32_t w2 = CreateWidgetAnnot(page, 60, 200, 80, 220);
  ASSERT_GT(w1, 0u);
  ASSERT_GT(w2, 0u);

  ASSERT_TRUE(EPDFForm_AttachWidget(document(), field, w1, "male", 0));
  ASSERT_TRUE(EPDFForm_AttachWidget(document(), field, w2, "female", 0));
  // Re-attaching an already attached widget fails.
  EXPECT_FALSE(EPDFForm_AttachWidget(document(), field, w1, "male", 0));
  // Toggles demand a usable on-state name.
  EXPECT_FALSE(EPDFForm_AttachWidget(document(), field, w1, nullptr, 0));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int index = EPDFForm_GetFieldIndexByObjNum(model, field);
  ASSERT_GE(index, 0);
  ASSERT_EQ(2, EPDFForm_CountFieldWidgets(model, index));
  EXPECT_EQ("male", GetWidgetOnState(model, index, 0));
  EXPECT_EQ("female", GetWidgetOnState(model, index, 1));
  EPDFForm_CloseModel(model);

  // The newborn group is fillable at once.
  unsigned long changed = 0;
  const FPDF_BOOL first_on[] = {true, false};
  ASSERT_TRUE(EPDFForm_SetFieldWidgetsChecked(document(), field, first_on, 2,
                                              nullptr, 0, &changed));
  EXPECT_EQ(1ul, changed);
  ASSERT_TRUE(SetOneValue(document(), field, L"male"));
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
  ASSERT_TRUE(EPDFForm_AttachWidget(document(), 4u, widget, nullptr, 0));

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
  ASSERT_TRUE(SetOneValue(document(), 4u, L"ab"));
  unsigned long changed = 0;
  ASSERT_TRUE(
      EPDFForm_RedrawFieldWidgets(document(), 4u, nullptr, 0, &changed));
  EXPECT_EQ(2ul, changed);
  UnloadPage(page);
}

TEST_F(EPDFFormEmbedderTest, DetachWidgetKeepsFieldVisible) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);

  const uint32_t field =
      EPDFForm_CreateField(document(), 4, GetFPDFWideString(L"note").get(), 0);
  const uint32_t widget = CreateWidgetAnnot(page, 20, 200, 200, 220);
  ASSERT_TRUE(EPDFForm_AttachWidget(document(), field, widget, nullptr, 0));
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
      document(), 4, GetFPDFWideString(L"billing.name").get(), 0);
  const uint32_t widget = CreateWidgetAnnot(page, 20, 200, 200, 220);
  ASSERT_TRUE(EPDFForm_AttachWidget(document(), field, widget, nullptr, 0));

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

  ASSERT_TRUE(SetOneDefault(document(), 4u, L"dflt"));
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
  ASSERT_TRUE(EPDFForm_SetFieldValue(document(), 17u, EPDF_FORM_VALUE_NONE,
                                     nullptr, 0));

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

// New options leave the selection and the default as they were: the caller
// writes what goes with them.
TEST_F(EPDFFormEmbedderTest, SetFieldOptionsWritesOnlyTheOptions) {
  ASSERT_TRUE(OpenDocument("listbox_form.pdf"));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  int index = FieldIndexByName(model, L"Listbox_MultiSelectMultipleValues");
  ASSERT_GE(index, 0);
  const uint32_t field = EPDFForm_GetFieldObjNum(model, index);
  ASSERT_EQ(2, EPDFForm_CountFieldValues(model, index));
  EPDFForm_CloseModel(model);

  ScopedFPDFWideString alpha = GetFPDFWideString(L"Alpha");
  ScopedFPDFWideString gamma = GetFPDFWideString(L"Gamma");
  ScopedFPDFWideString zeta = GetFPDFWideString(L"Zeta");
  FPDF_WIDESTRING labels[] = {alpha.get(), gamma.get(), zeta.get()};
  ASSERT_TRUE(EPDFForm_SetFieldOptions(document(), field, labels, labels, 3));

  model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  index = EPDFForm_GetFieldIndexByObjNum(model, field);
  ASSERT_EQ(3, EPDFForm_CountFieldOptions(model, index));
  EXPECT_EQ(L"Zeta",
            GetFieldValue(EPDFForm_GetFieldOptionValue, model, index, 2));
  // /V still holds [Epsilon, Gamma].
  EXPECT_EQ(EPDF_FORM_VALUE_ARRAY, EPDFForm_GetFieldValueKind(model, index));
  ASSERT_EQ(2, EPDFForm_CountFieldValues(model, index));
  EPDFForm_CloseModel(model);
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
      doc.layer, 4, GetFPDFWideString(L"layer_field").get(), 0);
  ASSERT_GT(field, 0u);
  FPDF_PAGE page = FPDF_LoadPage(doc.layer, 0);
  ASSERT_TRUE(page);
  const uint32_t widget = CreateWidgetAnnot(page, 20, 200, 200, 220);
  ASSERT_GT(widget, 0u);
  ASSERT_TRUE(EPDFForm_AttachWidget(doc.layer, field, widget, nullptr, 0));
  FPDF_ClosePage(page);

  // Duplicate create fails without growing the delta.
  const unsigned long promoted = EPDFLayer_GetPromotedObjectCount(doc.layer);
  EXPECT_EQ(0u, EPDFForm_CreateField(
                    doc.layer, 4, GetFPDFWideString(L"layer_field").get(), 0));
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

// A field's own /EMBD_Metadata, read and written the way an annotation's is.
// In toggle_fields.pdf, object 4 is a text field merged with its widget, and
// object 5 is a radio group whose field dictionary sits above its widgets
// (objects 6 and 7), so no annotation handle reaches it.
TEST_F(EPDFFormEmbedderTest, FieldEmbedMetadataReadsAndWrites) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));

  auto read_string = [](EPDF_FORM_MODEL model, int index, const char* key) {
    unsigned long length_bytes =
        EPDFForm_GetFieldEmbedMetadataString(model, index, key, nullptr, 0);
    std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length_bytes);
    EXPECT_EQ(length_bytes,
              EPDFForm_GetFieldEmbedMetadataString(
                  model, index, key, buffer.data(), length_bytes));
    return GetPlatformWString(buffer.data());
  };

  for (uint32_t field : {4u, 5u}) {
    SCOPED_TRACE(field);
    EXPECT_TRUE(EPDFForm_SetFieldEmbedMetadataString(
        document(), field, "GroupID", GetFPDFWideString(L"buyer").get()));
    EXPECT_TRUE(EPDFForm_SetFieldEmbedMetadataNumber(document(), field,
                                                     "Weight", 2.5f));
    EXPECT_TRUE(EPDFForm_SetFieldEmbedMetadataBoolean(document(), field,
                                                      "Locked", true));
    EXPECT_TRUE(EPDFForm_SetFieldEmbedMetadataJSON(
        document(), field, GetFPDFWideString(L"{\"a\":1}").get()));

    EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
    ASSERT_TRUE(model);
    const int index = EPDFForm_GetFieldIndexByObjNum(model, field);
    ASSERT_GE(index, 0);
    EXPECT_TRUE(EPDFForm_HasFieldEmbedMetadata(model, index));
    EXPECT_EQ(L"buyer", read_string(model, index, "GroupID"));
    float number = 0;
    EXPECT_TRUE(
        EPDFForm_GetFieldEmbedMetadataNumber(model, index, "Weight", &number));
    EXPECT_FLOAT_EQ(2.5f, number);
    FPDF_BOOL boolean = false;
    EXPECT_TRUE(EPDFForm_GetFieldEmbedMetadataBoolean(model, index, "Locked",
                                                      &boolean));
    EXPECT_TRUE(boolean);
    EXPECT_EQ(L"{\"a\":1}",
              GetWideString(EPDFForm_GetFieldEmbedMetadataJSON, model, index));

    // A typed read succeeds only for its type, and a missing key reads as
    // nothing.
    EXPECT_FALSE(
        EPDFForm_GetFieldEmbedMetadataNumber(model, index, "GroupID", &number));
    EXPECT_FALSE(EPDFForm_GetFieldEmbedMetadataBoolean(model, index, "Weight",
                                                       &boolean));
    EXPECT_FALSE(
        EPDFForm_GetFieldEmbedMetadataNumber(model, index, "Missing", &number));
    EXPECT_EQ(L"", read_string(model, index, "Missing"));
    EPDFForm_CloseModel(model);
  }

  // The radio group's widgets carry nothing: the metadata is the field's.
  for (uint32_t widget : {6u, 7u}) {
    EXPECT_FALSE(GetEffectiveIndirectDictionary(document(), widget)
                     ->KeyExist("EMBD_Metadata"));
  }

  // The merged field shares its dictionary with its widget, so the
  // annotation API reads the same metadata.
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation widget(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(widget);
  unsigned long length_bytes =
      EPDFAnnot_GetEmbedMetadataString(widget.get(), "GroupID", nullptr, 0);
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length_bytes);
  EPDFAnnot_GetEmbedMetadataString(widget.get(), "GroupID", buffer.data(),
                                   length_bytes);
  EXPECT_EQ(L"buyer", GetPlatformWString(buffer.data()));
}

// The model is a snapshot, like every other field fact it reports.
TEST_F(EPDFFormEmbedderTest, FieldEmbedMetadataIsSnapshotted) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  ASSERT_TRUE(EPDFForm_SetFieldEmbedMetadataString(
      document(), 4u, "GroupID", GetFPDFWideString(L"buyer").get()));
  EPDF_FORM_MODEL before = EPDFForm_LoadModel(document());
  ASSERT_TRUE(before);
  ASSERT_TRUE(EPDFForm_SetFieldEmbedMetadataString(
      document(), 4u, "GroupID", GetFPDFWideString(L"seller").get()));

  const int index = EPDFForm_GetFieldIndexByObjNum(before, 4u);
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(64);
  EPDFForm_GetFieldEmbedMetadataString(before, index, "GroupID", buffer.data(),
                                       64);
  EXPECT_EQ(L"buyer", GetPlatformWString(buffer.data()));
  EPDFForm_CloseModel(before);
}

// A write goes on the field dictionary it names, never on a parent field the
// field inherits from (object 17 inherits /FT from object 16).
TEST_F(EPDFFormEmbedderTest, FieldEmbedMetadataWritesTheFieldNotItsParent) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  ASSERT_TRUE(EPDFForm_SetFieldEmbedMetadataString(
      document(), 17u, "GroupID", GetFPDFWideString(L"buyer").get()));
  EXPECT_TRUE(GetEffectiveIndirectDictionary(document(), 17u)
                  ->KeyExist("EMBD_Metadata"));
  EXPECT_FALSE(GetEffectiveIndirectDictionary(document(), 16u)
                   ->KeyExist("EMBD_Metadata"));

  // Something that isn't a field, a missing object, or no key: refused.
  const ScopedFPDFWideString value = GetFPDFWideString(L"buyer");
  EXPECT_FALSE(EPDFForm_SetFieldEmbedMetadataString(document(), 3u, "GroupID",
                                                    value.get()));
  EXPECT_FALSE(EPDFForm_SetFieldEmbedMetadataString(document(), 0u, "GroupID",
                                                    value.get()));
  EXPECT_FALSE(EPDFForm_SetFieldEmbedMetadataString(document(), 4000u,
                                                    "GroupID", value.get()));
  EXPECT_FALSE(EPDFForm_SetFieldEmbedMetadataString(document(), 4u, nullptr,
                                                    value.get()));
  EXPECT_FALSE(EPDFForm_ClearFieldEmbedMetadata(document(), 3u));
}

// Removing the last key removes /EMBD_Metadata, and clearing what isn't there
// creates nothing.
TEST_F(EPDFFormEmbedderTest, ClearingFieldEmbedMetadata) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  auto has_metadata = [&](uint32_t field) {
    return GetEffectiveIndirectDictionary(document(), field)
        ->KeyExist("EMBD_Metadata");
  };

  EXPECT_TRUE(EPDFForm_ClearFieldEmbedMetadataKey(document(), 5u, "GroupID"));
  EXPECT_TRUE(EPDFForm_ClearFieldEmbedMetadata(document(), 5u));
  EXPECT_FALSE(has_metadata(5u));

  ASSERT_TRUE(EPDFForm_SetFieldEmbedMetadataString(
      document(), 5u, "GroupID", GetFPDFWideString(L"buyer").get()));
  ASSERT_TRUE(
      EPDFForm_SetFieldEmbedMetadataBoolean(document(), 5u, "Locked", false));
  EXPECT_TRUE(EPDFForm_ClearFieldEmbedMetadataKey(document(), 5u, "GroupID"));
  EXPECT_TRUE(has_metadata(5u));
  EXPECT_TRUE(EPDFForm_ClearFieldEmbedMetadataKey(document(), 5u, "Locked"));
  EXPECT_FALSE(has_metadata(5u));

  ASSERT_TRUE(EPDFForm_SetFieldEmbedMetadataString(
      document(), 5u, "GroupID", GetFPDFWideString(L"buyer").get()));
  EXPECT_TRUE(EPDFForm_ClearFieldEmbedMetadata(document(), 5u));
  EXPECT_FALSE(has_metadata(5u));
}

// Clearing a document's metadata reaches its fields too, including one no
// annotation handle reaches (the radio group's field) and a parent field.
TEST_F(EPDFFormEmbedderTest, DocumentClearReachesFields) {
  ASSERT_TRUE(OpenDocument("toggle_fields.pdf"));
  for (uint32_t field : {4u, 5u, 16u, 17u}) {
    ASSERT_TRUE(EPDFForm_SetFieldEmbedMetadataString(
        document(), field, "GroupID", GetFPDFWideString(L"buyer").get()));
  }

  ASSERT_TRUE(EPDFDocument_ClearEmbedMetadata(document()));
  for (uint32_t field : {4u, 5u, 16u, 17u}) {
    SCOPED_TRACE(field);
    EXPECT_FALSE(GetEffectiveIndirectDictionary(document(), field)
                     ->KeyExist("EMBD_Metadata"));
  }
}

namespace {

// The script of |model|'s first action, or "-" for no model (no action for
// that event). Closes |model|.
std::wstring TakeScript(EPDF_ACTION_MODEL model) {
  if (!model) {
    return L"-";
  }
  const EPDF_ACTION_NODE_ID root = EPDFAction_GetRootNode(model);
  const unsigned long length =
      EPDFAction_GetNodeJavaScript(model, root, nullptr, 0);
  std::wstring script;
  if (length > 0) {
    std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
    EPDFAction_GetNodeJavaScript(model, root, buffer.data(), length);
    script = GetPlatformWString(buffer.data());
  }
  EPDFAction_CloseModel(model);
  return script;
}

// The script of field |field_objnum|'s |event| (EPDF_FORM_ACTION_*), as the
// form model reads it.
std::wstring FieldScript(FPDF_DOCUMENT document,
                         uint32_t field_objnum,
                         int event) {
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document);
  const int index = EPDFForm_GetFieldIndexByObjNum(model, field_objnum);
  std::wstring script =
      index >= 0 ? TakeScript(EPDFForm_GetFieldActionModel(model, index, event))
                 : L"no field";
  EPDFForm_CloseModel(model);
  return script;
}

// The script of annotation |index| on |page| for |event| (EPDF_ANNOT_ACTION_*).
std::wstring WidgetScript(FPDF_PAGE page, int index, int event) {
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, index));
  return TakeScript(EPDFAnnot_GetActionModel(annot.get(), event));
}

FPDF_ACTION Script(FPDF_DOCUMENT document, const wchar_t* source) {
  return EPDFAction_CreateJavaScript(document, GetFPDFWideString(source).get());
}

// The fields of /AcroForm /CO, by object number, as the form model reads it.
std::vector<uint32_t> CalculationOrder(FPDF_DOCUMENT document) {
  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document);
  std::vector<uint32_t> order;
  for (int i = 0; i < EPDFForm_CountCalculationOrder(model); ++i) {
    order.push_back(EPDFForm_GetFieldObjNum(
        model, EPDFForm_GetCalculationOrderFieldIndex(model, i)));
  }
  EPDFForm_CloseModel(model);
  return order;
}

}  // namespace

// In merged_field_actions.pdf, "amount" (4) is a text field merged with its
// widget, with a format script (the field's), a focus script and a click
// action (the widget's). "group.child" (6) is merged too, with no /AA of its
// own: it inherits a keystroke and a calculate script from "group" (5).

TEST_F(EPDFFormEmbedderTest, EventActionsSetReplaceAndRemove) {
  ASSERT_TRUE(OpenDocument("merged_field_actions.pdf"));
  FPDF_DOCUMENT doc = document();
  FPDF_ACTION blurred = Script(doc, L"blurred = 1;");
  FPDF_ACTION typed = Script(doc, L"typed = 1;");
  ASSERT_TRUE(blurred && typed);
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);

  // A widget event and a field event on the one merged dictionary: each
  // leaves the other plane's entries alone.
  {
    ScopedFPDFAnnotation amount(FPDFPage_GetAnnot(page, 0));
    ASSERT_TRUE(EPDFAnnot_SetEventAction(amount.get(), EPDF_ANNOT_ACTION_BLUR,
                                         blurred));
  }
  ASSERT_TRUE(
      EPDFForm_SetFieldEventAction(doc, 4u, EPDF_FORM_ACTION_KEYSTROKE, typed));
  EXPECT_EQ(L"blurred = 1;", WidgetScript(page, 0, EPDF_ANNOT_ACTION_BLUR));
  EXPECT_EQ(L"focused = 1;", WidgetScript(page, 0, EPDF_ANNOT_ACTION_FOCUS));
  EXPECT_EQ(L"clicked = 1;", WidgetScript(page, 0, EPDF_ANNOT_ACTION_ACTIVATE));
  EXPECT_EQ(L"typed = 1;", FieldScript(doc, 4u, EPDF_FORM_ACTION_KEYSTROKE));
  EXPECT_EQ(L"formatted = 1;", FieldScript(doc, 4u, EPDF_FORM_ACTION_FORMAT));

  // Replaced, then removed; removing what isn't there changes nothing.
  ASSERT_TRUE(
      EPDFForm_SetFieldEventAction(doc, 4u, EPDF_FORM_ACTION_FORMAT, typed));
  EXPECT_EQ(L"typed = 1;", FieldScript(doc, 4u, EPDF_FORM_ACTION_FORMAT));
  ASSERT_TRUE(
      EPDFForm_SetFieldEventAction(doc, 4u, EPDF_FORM_ACTION_FORMAT, nullptr));
  EXPECT_EQ(L"-", FieldScript(doc, 4u, EPDF_FORM_ACTION_FORMAT));
  {
    ScopedFPDFAnnotation amount(FPDFPage_GetAnnot(page, 0));
    ASSERT_TRUE(EPDFAnnot_SetEventAction(amount.get(),
                                         EPDF_ANNOT_ACTION_ACTIVATE, nullptr));
    ASSERT_TRUE(EPDFAnnot_SetEventAction(amount.get(),
                                         EPDF_ANNOT_ACTION_ACTIVATE, nullptr));
  }
  EXPECT_EQ(L"-", WidgetScript(page, 0, EPDF_ANNOT_ACTION_ACTIVATE));
  EXPECT_FALSE(GetEffectiveIndirectDictionary(doc, 4u)->KeyExist("A"));

  // The inherited events keep applying through the field's first entries
  // of its own, and an /AA left empty keeps hiding the parent's.
  {
    ScopedFPDFAnnotation child(FPDFPage_GetAnnot(page, 1));
    ASSERT_TRUE(EPDFAnnot_SetEventAction(child.get(), EPDF_ANNOT_ACTION_FOCUS,
                                         blurred));
  }
  EXPECT_EQ(L"keyed = 1;", FieldScript(doc, 6u, EPDF_FORM_ACTION_KEYSTROKE));
  EXPECT_EQ(L"calculated = 1;",
            FieldScript(doc, 6u, EPDF_FORM_ACTION_CALCULATE));
  ASSERT_TRUE(EPDFForm_SetFieldEventAction(doc, 6u, EPDF_FORM_ACTION_CALCULATE,
                                           nullptr));
  EXPECT_EQ(L"keyed = 1;", FieldScript(doc, 6u, EPDF_FORM_ACTION_KEYSTROKE));
  EXPECT_EQ(L"-", FieldScript(doc, 6u, EPDF_FORM_ACTION_CALCULATE));
  ASSERT_TRUE(EPDFForm_SetFieldEventAction(doc, 6u, EPDF_FORM_ACTION_KEYSTROKE,
                                           nullptr));
  {
    ScopedFPDFAnnotation child(FPDFPage_GetAnnot(page, 1));
    ASSERT_TRUE(EPDFAnnot_SetEventAction(child.get(), EPDF_ANNOT_ACTION_FOCUS,
                                         nullptr));
  }
  EXPECT_EQ(L"-", FieldScript(doc, 6u, EPDF_FORM_ACTION_KEYSTROKE));
  EXPECT_EQ(0u,
            GetEffectiveIndirectDictionary(doc, 6u)->GetDictFor("AA")->size());
  // The parent's own are untouched.
  EXPECT_TRUE(
      GetEffectiveIndirectDictionary(doc, 5u)->GetDictFor("AA")->KeyExist("C"));

  // Refused: an event out of range, a direct action, a number that is no
  // field, another annotation subtype.
  {
    ScopedFPDFAnnotation amount(FPDFPage_GetAnnot(page, 0));
    EXPECT_FALSE(EPDFAnnot_SetEventAction(amount.get(), 11, typed));
    auto direct = pdfium::MakeRetain<CPDF_Dictionary>();
    direct->SetNewFor<CPDF_Name>("S", "JavaScript");
    EXPECT_FALSE(
        EPDFAnnot_SetEventAction(amount.get(), EPDF_ANNOT_ACTION_FOCUS,
                                 FPDFActionFromCPDFDictionary(direct.Get())));
  }
  EXPECT_FALSE(
      EPDFForm_SetFieldEventAction(doc, 3u, EPDF_FORM_ACTION_FORMAT, typed));
  EXPECT_FALSE(EPDFForm_SetFieldEventAction(doc, 4u, 4, typed));
  {
    ScopedFPDFAnnotation square(EPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE));
    ASSERT_TRUE(square);
    EXPECT_FALSE(EPDFAnnot_SetEventAction(square.get(),
                                          EPDF_ANNOT_ACTION_ACTIVATE, typed));
  }
  UnloadPage(page);
}

// Inside a layer transaction: the writes read back through the snapshot,
// and setting what is already there copies nothing up for writing.
TEST_F(EPDFFormEmbedderTest, EventActionsOnALayer) {
  LayerDoc doc;
  ASSERT_TRUE(OpenLayer("merged_field_actions.pdf", &doc));
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.layer));
  FPDF_ACTION typed = Script(doc.layer, L"typed = 1;");
  ASSERT_TRUE(typed);
  ASSERT_TRUE(EPDFForm_SetFieldEventAction(doc.layer, 4u,
                                           EPDF_FORM_ACTION_KEYSTROKE, typed));
  FPDF_PAGE page = FPDF_LoadPage(doc.layer, 0);
  ASSERT_TRUE(page);
  {
    ScopedFPDFAnnotation amount(FPDFPage_GetAnnot(page, 0));
    ASSERT_TRUE(
        EPDFAnnot_SetEventAction(amount.get(), EPDF_ANNOT_ACTION_BLUR, typed));
  }
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.layer));
  EXPECT_EQ(L"typed = 1;",
            FieldScript(doc.layer, 4u, EPDF_FORM_ACTION_KEYSTROKE));
  EXPECT_EQ(L"typed = 1;", WidgetScript(page, 0, EPDF_ANNOT_ACTION_BLUR));
  EXPECT_EQ(L"formatted = 1;",
            FieldScript(doc.layer, 4u, EPDF_FORM_ACTION_FORMAT));

  // Removing what isn't there copies nothing up for writing.
  const unsigned long promoted = EPDFLayer_GetPromotedObjectCount(doc.layer);
  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc.layer));
  ASSERT_TRUE(EPDFForm_SetFieldEventAction(doc.layer, 6u,
                                           EPDF_FORM_ACTION_FORMAT, nullptr));
  {
    ScopedFPDFAnnotation child(FPDFPage_GetAnnot(page, 1));
    ASSERT_TRUE(
        EPDFAnnot_SetEventAction(child.get(), EPDF_ANNOT_ACTION_BLUR, nullptr));
  }
  ASSERT_TRUE(EPDFLayer_CommitTransaction(doc.layer));
  EXPECT_EQ(promoted, EPDFLayer_GetPromotedObjectCount(doc.layer));
  FPDF_ClosePage(page);
}

// Splitting a merged field moves the widget's actions (/A, and its /AA
// events) to the new widget; the field keeps its own.
TEST_F(EPDFFormEmbedderTest, SplitKeepsEachPlanesActions) {
  ASSERT_TRUE(OpenDocument("merged_field_actions.pdf"));
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);
  const uint32_t widget = CreateWidgetAnnot(page, 20, 100, 280, 130);
  ASSERT_GT(widget, 0u);
  ASSERT_TRUE(EPDFForm_AttachWidget(document(), 4u, widget, nullptr, 0));
  UnloadPage(page);

  EXPECT_EQ(L"formatted = 1;",
            FieldScript(document(), 4u, EPDF_FORM_ACTION_FORMAT));
  RetainPtr<const CPDF_Dictionary> field =
      GetEffectiveIndirectDictionary(document(), 4u);
  EXPECT_FALSE(field->KeyExist("A"));
  EXPECT_FALSE(field->GetDictFor("AA")->KeyExist("Fo"));

  // The split widget took the merged dictionary's place in /Annots, so it
  // keeps its place in the stacking order.
  page = LoadPage(0);
  ASSERT_TRUE(page);
  {
    ScopedFPDFAnnotation split(FPDFPage_GetAnnot(page, 0));
    EXPECT_NE(4u, EPDFAnnot_GetObjectNumber(split.get()));
  }
  EXPECT_EQ(L"focused = 1;", WidgetScript(page, 0, EPDF_ANNOT_ACTION_FOCUS));
  EXPECT_EQ(L"clicked = 1;", WidgetScript(page, 0, EPDF_ANNOT_ACTION_ACTIVATE));
  // The new widget has none.
  EXPECT_EQ(L"-", WidgetScript(page, 2, EPDF_ANNOT_ACTION_FOCUS));
  UnloadPage(page);
}

TEST_F(EPDFFormEmbedderTest, CalculationOrderSetReplaceAndRemove) {
  ASSERT_TRUE(OpenDocument("merged_field_actions.pdf"));
  EXPECT_TRUE(CalculationOrder(document()).empty());

  const uint32_t order[] = {6u, 4u};
  ASSERT_TRUE(EPDFForm_SetCalculationOrder(document(), order, 2));
  EXPECT_EQ(std::vector<uint32_t>({6u, 4u}), CalculationOrder(document()));
  const uint32_t replaced[] = {4u};
  ASSERT_TRUE(EPDFForm_SetCalculationOrder(document(), replaced, 1));
  EXPECT_EQ(std::vector<uint32_t>({4u}), CalculationOrder(document()));

  // Refused, writing nothing: a number twice, a number that is no field.
  const uint32_t twice[] = {6u, 6u};
  EXPECT_FALSE(EPDFForm_SetCalculationOrder(document(), twice, 2));
  const uint32_t page[] = {3u};
  EXPECT_FALSE(EPDFForm_SetCalculationOrder(document(), page, 1));
  EXPECT_EQ(std::vector<uint32_t>({4u}), CalculationOrder(document()));

  ASSERT_TRUE(EPDFForm_SetCalculationOrder(document(), nullptr, 0));
  EXPECT_TRUE(CalculationOrder(document()).empty());
  EXPECT_FALSE(CPDFDocumentFromFPDFDocument(document())
                   ->GetRoot()
                   ->GetDictFor("AcroForm")
                   ->KeyExist("CO"));
}

// A push button: created, given a widget and a caption, and drawn with it.
// Its value can't be written.
TEST_F(EPDFFormEmbedderTest, PushButtonIsAuthorableAndShowsItsCaption) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);
  const uint32_t field =
      EPDFForm_CreateField(document(), EPDF_FORMFIELD_FAMILY_PUSHBUTTON,
                           GetFPDFWideString(L"clear").get(), 0);
  ASSERT_GT(field, 0u);
  const uint32_t widget = CreateWidgetAnnot(page, 20, 200, 120, 230);
  ASSERT_GT(widget, 0u);
  {
    ScopedFPDFAnnotation annot(
        FPDFPage_GetAnnot(page, FPDFPage_GetAnnotCount(page) - 1));
    ASSERT_TRUE(EPDFAnnot_SetMKText(annot.get(), EPDF_MK_TEXT_CA,
                                    GetFPDFWideString(L"Clear").get()));
    const unsigned long length =
        EPDFAnnot_GetMKText(annot.get(), EPDF_MK_TEXT_CA, nullptr, 0);
    ASSERT_GT(length, 0u);
    std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
    EPDFAnnot_GetMKText(annot.get(), EPDF_MK_TEXT_CA, buffer.data(), length);
    EXPECT_EQ(L"Clear", GetPlatformWString(buffer.data()));
    EXPECT_EQ(0u,
              EPDFAnnot_GetMKText(annot.get(), EPDF_MK_TEXT_RC, nullptr, 0));
    EXPECT_FALSE(EPDFAnnot_SetMKText(annot.get(), 3, nullptr));

    EXPECT_EQ(0, EPDFAnnot_GetMKTextPosition(annot.get()));
    ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(annot.get(), 2));
    EXPECT_EQ(2, EPDFAnnot_GetMKTextPosition(annot.get()));
    EXPECT_FALSE(EPDFAnnot_SetMKTextPosition(annot.get(), 7));
    ASSERT_TRUE(EPDFAnnot_SetMKTextPosition(annot.get(), 0));
  }
  ASSERT_TRUE(EPDFForm_AttachWidget(document(), field, widget, nullptr, 0));

  EPDF_FORM_MODEL model = EPDFForm_LoadModel(document());
  ASSERT_TRUE(model);
  const int index = EPDFForm_GetFieldIndexByObjNum(model, field);
  ASSERT_GE(index, 0);
  EXPECT_EQ(EPDF_FORMFIELD_FAMILY_PUSHBUTTON,
            EPDFForm_GetFieldFamily(model, index));
  EXPECT_EQ(1, EPDFForm_CountFieldWidgets(model, index));
  EPDFForm_CloseModel(model);

  // Attaching drew the button with its caption: text, in the /DA font.
  const std::wstring look = GetEffectiveWidgetAppearance(document(), widget);
  EXPECT_NE(std::wstring::npos, look.find(L"BT"));
  EXPECT_NE(std::wstring::npos, look.find(L"Tf"));

  // Without a caption it is drawn as the box alone.
  {
    ScopedFPDFAnnotation annot(
        FPDFPage_GetAnnot(page, FPDFPage_GetAnnotCount(page) - 1));
    ASSERT_TRUE(EPDFAnnot_SetMKText(annot.get(), EPDF_MK_TEXT_CA, nullptr));
    ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(annot.get()));
  }
  EXPECT_EQ(std::wstring::npos,
            GetEffectiveWidgetAppearance(document(), widget).find(L"BT"));

  // A push button has no value to write and no value to draw.
  EXPECT_FALSE(SetOneValue(document(), field, L"x"));
  const FPDF_BOOL on[] = {true};
  EXPECT_FALSE(EPDFForm_SetFieldWidgetsChecked(document(), field, on, 1,
                                               nullptr, 0, nullptr));
  EXPECT_FALSE(
      EPDFForm_RedrawFieldWidgets(document(), field, nullptr, 0, nullptr));
  UnloadPage(page);
}

TEST_F(EPDFFormEmbedderTest, WidgetRotationTurnsItsAppearance) {
  ASSERT_TRUE(OpenDocument("merged_field_actions.pdf"));
  FPDF_PAGE page = LoadPage(0);
  ASSERT_TRUE(page);
  ScopedFPDFAnnotation amount(FPDFPage_GetAnnot(page, 0));
  EXPECT_EQ(0, EPDFAnnot_GetMKRotation(amount.get()));
  EXPECT_FALSE(EPDFAnnot_SetMKRotation(amount.get(), 45));
  EXPECT_FALSE(EPDFAnnot_SetMKRotation(amount.get(), 360));
  ASSERT_TRUE(EPDFAnnot_SetMKRotation(amount.get(), 90));
  EXPECT_EQ(90, EPDFAnnot_GetMKRotation(amount.get()));
  ASSERT_TRUE(EPDFAnnot_GenerateFormFieldAP(amount.get()));

  // The normal appearance turns a quarter, counterclockwise.
  RetainPtr<const CPDF_Stream> normal =
      GetEffectiveIndirectDictionary(document(), 4u)
          ->GetDictFor("AP")
          ->GetStreamFor("N");
  ASSERT_TRUE(normal);
  const CFX_Matrix matrix = normal->GetDict()->GetMatrixFor("Matrix");
  EXPECT_FLOAT_EQ(0.0f, matrix.a);
  EXPECT_FLOAT_EQ(1.0f, matrix.b);
  EXPECT_FLOAT_EQ(-1.0f, matrix.c);
  EXPECT_FLOAT_EQ(0.0f, matrix.d);

  // 0 removes /R.
  ASSERT_TRUE(EPDFAnnot_SetMKRotation(amount.get(), 0));
  EXPECT_EQ(0, EPDFAnnot_GetMKRotation(amount.get()));
  EXPECT_FALSE(GetEffectiveIndirectDictionary(document(), 4u)
                   ->GetDictFor("MK")
                   ->KeyExist("R"));
  amount.reset();
  UnloadPage(page);
}
