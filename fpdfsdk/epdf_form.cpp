// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_form.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "constants/annotation_flags.h"
#include "constants/form_fields.h"
#include "constants/form_flags.h"
#include "core/fpdfapi/parser/cfdf_document.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfdoc/cpdf_aaction.h"
#include "core/fpdfdoc/cpdf_action.h"
#include "core/fpdfdoc/cpdf_formcontrol.h"
#include "core/fpdfdoc/cpdf_formfield.h"
#include "core/fpdfdoc/cpdf_generateap.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/cfx_memorystream.h"
#include "core/fxcrt/cfx_read_only_span_stream.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/containers/contains.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/span_util.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxcrt/widestring.h"
#include "core/fxcrt/xml/cfx_xmldocument.h"
#include "core/fxcrt/xml/cfx_xmlelement.h"
#include "core/fxcrt/xml/cfx_xmlnode.h"
#include "core/fxcrt/xml/cfx_xmlparser.h"
#include "core/fxcrt/xml/cfx_xmltext.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_action_helpers.h"
#include "fpdfsdk/epdf_form_helpers.h"

namespace {

using epdf::BuildReconciledForm;
using epdf::CollectFieldDicts;
using epdf::CountFormFields;
using epdf::PageObjNumForWidget;
using epdf::ResolveFieldDict;
using epdf::SweepPageWidgets;

struct WidgetRecord {
  uint32_t objnum = 0;
  uint32_t page_objnum = 0;
  ByteString on_state;
  WideString export_value;
  bool checked = false;
};

struct OptionRecord {
  WideString label;
  WideString value;
  bool selected = false;
};

struct FieldValueRecord {
  int kind = EPDF_FORM_VALUE_NONE;
  std::vector<WideString> values;
};

struct FieldRecord {
  uint32_t objnum = 0;
  int family = EPDF_FORMFIELD_FAMILY_UNKNOWN;
  uint32_t flags = 0;
  int origin = EPDF_FORMFIELD_ORIGIN_ACROFORM;
  int max_len = 0;
  WideString fqn;
  WideString alternate_name;
  WideString mapping_name;
  FieldValueRecord value;
  FieldValueRecord default_value;
  std::vector<OptionRecord> options;
  std::vector<WidgetRecord> widgets;
  std::array<epdf::ActionModelDataPtr, 4> actions;
};

// A detached, immutable snapshot. Holds no pointers into the document, so
// it stays valid after the document is closed and can never dangle or
// observe stale pre-promotion objects.
struct FormModel {
  int kind = EPDF_FORMKIND_NONE;
  bool need_appearances = false;
  std::vector<FieldRecord> fields;
  std::map<uint32_t, int> field_index_by_objnum;
  std::map<uint32_t, int> field_index_by_widget_objnum;
  std::vector<int> calculation_order;
};

FormModel* FormModelFromHandle(EPDF_FORM_MODEL model) {
  return reinterpret_cast<FormModel*>(model);
}

EPDF_FORM_MODEL HandleFromFormModel(FormModel* model) {
  return reinterpret_cast<EPDF_FORM_MODEL>(model);
}

const FieldRecord* GetFieldRecord(EPDF_FORM_MODEL model, int field_index) {
  FormModel* form = FormModelFromHandle(model);
  if (!form || field_index < 0 ||
      field_index >= fxcrt::CollectionSize<int>(form->fields)) {
    return nullptr;
  }
  return &form->fields[field_index];
}

const WidgetRecord* GetWidgetRecord(EPDF_FORM_MODEL model,
                                    int field_index,
                                    int widget_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  if (!field || widget_index < 0 ||
      widget_index >= fxcrt::CollectionSize<int>(field->widgets)) {
    return nullptr;
  }
  return &field->widgets[widget_index];
}

const OptionRecord* GetOptionRecord(EPDF_FORM_MODEL model,
                                    int field_index,
                                    int option_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  if (!field || option_index < 0 ||
      option_index >= fxcrt::CollectionSize<int>(field->options)) {
    return nullptr;
  }
  return &field->options[option_index];
}

int FamilyFromFieldType(CPDF_FormField::Type type) {
  switch (type) {
    case CPDF_FormField::kPushButton:
      return EPDF_FORMFIELD_FAMILY_PUSHBUTTON;
    case CPDF_FormField::kRadioButton:
      return EPDF_FORMFIELD_FAMILY_RADIO;
    case CPDF_FormField::kCheckBox:
      return EPDF_FORMFIELD_FAMILY_CHECKBOX;
    case CPDF_FormField::kText:
    case CPDF_FormField::kRichText:
    case CPDF_FormField::kFile:
      return EPDF_FORMFIELD_FAMILY_TEXT;
    case CPDF_FormField::kListBox:
      return EPDF_FORMFIELD_FAMILY_LISTBOX;
    case CPDF_FormField::kComboBox:
      return EPDF_FORMFIELD_FAMILY_COMBOBOX;
    case CPDF_FormField::kSign:
      return EPDF_FORMFIELD_FAMILY_SIGNATURE;
    case CPDF_FormField::kUnknown:
      return EPDF_FORMFIELD_FAMILY_UNKNOWN;
  }
  return EPDF_FORMFIELD_FAMILY_UNKNOWN;
}

bool IsToggleFamily(int family) {
  return family == EPDF_FORMFIELD_FAMILY_CHECKBOX ||
         family == EPDF_FORMFIELD_FAMILY_RADIO;
}

bool IsChoiceFamily(int family) {
  return family == EPDF_FORMFIELD_FAMILY_COMBOBOX ||
         family == EPDF_FORMFIELD_FAMILY_LISTBOX;
}

FieldValueRecord SnapshotFieldValue(RetainPtr<const CPDF_Object> object) {
  FieldValueRecord record;
  if (!object || object->IsNull()) {
    return record;
  }
  if (object->IsString() || object->IsName()) {
    record.kind = EPDF_FORM_VALUE_SCALAR;
    record.values.push_back(object->GetUnicodeText());
    return record;
  }
  const CPDF_Array* array = object->AsArray();
  if (!array) {
    record.kind = EPDF_FORM_VALUE_UNSUPPORTED;
    return record;
  }

  record.kind = EPDF_FORM_VALUE_ARRAY;
  record.values.reserve(array->size());
  for (size_t i = 0; i < array->size(); ++i) {
    RetainPtr<const CPDF_Object> element = array->GetDirectObjectAt(i);
    if (!element || !element->IsString()) {
      record.kind = EPDF_FORM_VALUE_UNSUPPORTED;
      record.values.clear();
      return record;
    }
    record.values.push_back(element->GetUnicodeText());
  }
  return record;
}


FieldRecord SnapshotField(
    CPDF_Document* document,
    CPDF_FormField* field,
    const std::set<const CPDF_Dictionary*>& initial_fields,
    const std::map<const CPDF_Dictionary*, uint32_t>& widget_pages) {
  FieldRecord record;
  const CPDF_Dictionary* field_dict = field->GetFieldDict().Get();
  record.objnum = field_dict->GetObjNum();
  record.family = FamilyFromFieldType(field->GetType());
  record.flags = field->GetFieldFlags();
  record.origin = pdfium::Contains(initial_fields, field_dict)
                      ? EPDF_FORMFIELD_ORIGIN_ACROFORM
                      : EPDF_FORMFIELD_ORIGIN_RECOVERED;
  record.fqn = field->GetFullName();
  record.alternate_name = field->GetAlternateName();
  record.mapping_name = field->GetMappingName();
  record.value = SnapshotFieldValue(
      CPDF_FormField::GetFieldAttrForDict(field_dict, pdfium::form_fields::kV));
  record.default_value = SnapshotFieldValue(CPDF_FormField::GetFieldAttrForDict(
      field_dict, pdfium::form_fields::kDV));

  static constexpr std::array<CPDF_AAction::AActionType, 4> kActionTypes = {
      CPDF_AAction::kKeyStroke, CPDF_AAction::kFormat, CPDF_AAction::kValidate,
      CPDF_AAction::kCalculate};
  CPDF_AAction additional_actions = field->GetAdditionalAction();
  for (size_t i = 0; i < kActionTypes.size(); ++i) {
    if (additional_actions.ActionExist(kActionTypes[i])) {
      record.actions[i] =
          epdf::BuildActionModel(additional_actions.GetAction(kActionTypes[i]),
                                 document);
    }
  }
  if (record.family == EPDF_FORMFIELD_FAMILY_TEXT) {
    record.max_len = field->GetMaxLen();
  }

  if (IsChoiceFamily(record.family)) {
    const int option_count = field->CountOptions();
    record.options.reserve(option_count);
    for (int i = 0; i < option_count; ++i) {
      OptionRecord option;
      option.label = field->GetOptionLabel(i);
      option.value = field->GetOptionValue(i);
      option.selected = field->IsItemSelected(i);
      record.options.push_back(std::move(option));
    }
  }

  const int control_count = field->CountControls();
  record.widgets.reserve(control_count);
  for (int i = 0; i < control_count; ++i) {
    const CPDF_FormControl* control = field->GetControl(i);
    if (!control) {
      continue;
    }
    const CPDF_Dictionary* widget_dict = control->GetWidgetDict().Get();
    WidgetRecord widget;
    widget.objnum = widget_dict->GetObjNum();
    widget.page_objnum = PageObjNumForWidget(widget_pages, widget_dict);
    if (IsToggleFamily(record.family)) {
      widget.on_state = control->GetOnStateName();
      widget.export_value = control->GetExportValue();
      widget.checked = control->IsChecked();
    }
    record.widgets.push_back(std::move(widget));
  }
  return record;
}

}  // namespace

// ---------------------------------------------------------------------------
// Write transactions.
//
// Layer-correctness rules, load-bearing on CPDF_LayerDocument:
//   1. Plan with const reads resolved per object number through
//      doc->GetIndirectObject() (layer-first lookup), never through cached
//      references captured from frozen base objects.
//   2. Validate fully BEFORE the first mutable access: a failed transaction
//      must promote nothing.
//   3. Mutate ONLY objects obtained from doc->GetMutableIndirectObject()
//      (which promotes) or reached through such a promoted clone. Never
//      mutate an object reached by resolving a reference held by a frozen
//      base object - that would corrupt the shared base.
// ---------------------------------------------------------------------------

namespace {

constexpr char kOffState[] = "Off";

// One widget of a terminal field, resolved for a transaction.
struct TxnControl {
  uint32_t objnum = 0;  // 0 for direct (spec-violating) kid dictionaries.
  size_t kids_index = 0;
  bool merged = false;  // The control IS the field dictionary.
  RetainPtr<const CPDF_Dictionary> dict;  // Planning-phase resolution.
  ByteString on_state;
  WideString export_value;
  ByteString current_as;
};


RetainPtr<const CPDF_Dictionary> ResolveParentFieldDict(
    CPDF_Document* doc,
    const CPDF_Dictionary* field) {
  RetainPtr<const CPDF_Object> parent_object =
      field ? field->GetObjectFor(pdfium::form_fields::kParent) : nullptr;
  if (!parent_object) {
    return nullptr;
  }
  if (const CPDF_Reference* reference = parent_object->AsReference()) {
    return ToDictionary(
        doc->GetOrParseIndirectObject(reference->GetRefObjNum()));
  }
  return ToDictionary(parent_object->GetDirect());
}

bool HasInheritedFieldAttribute(CPDF_Document* doc,
                                const CPDF_Dictionary* field,
                                ByteStringView key) {
  RetainPtr<const CPDF_Dictionary> current = ResolveParentFieldDict(doc, field);
  for (int depth = 0; current && depth < 32; ++depth) {
    if (current->KeyExist(key)) {
      return true;
    }
    current = ResolveParentFieldDict(doc, current.Get());
  }
  return false;
}

ByteString InheritedFieldType(const CPDF_Dictionary* field_dict) {
  RetainPtr<const CPDF_Object> ft =
      CPDF_FormField::GetFieldAttrForDict(field_dict, pdfium::form_fields::kFT);
  return ft ? ft->GetString() : ByteString();
}

uint32_t InheritedFieldFlags(const CPDF_Dictionary* field_dict) {
  RetainPtr<const CPDF_Object> ff =
      CPDF_FormField::GetFieldAttrForDict(field_dict, pdfium::form_fields::kFf);
  return ff ? static_cast<uint32_t>(ff->GetInteger()) : 0;
}

ByteString ReadWidgetOnState(const CPDF_Dictionary* widget_dict) {
  RetainPtr<const CPDF_Dictionary> ap = widget_dict->GetDictFor("AP");
  if (!ap) {
    return ByteString();
  }
  RetainPtr<const CPDF_Dictionary> normal = ap->GetDictFor("N");
  if (!normal) {
    return ByteString();
  }
  CPDF_DictionaryLocker locker(normal);
  for (const auto& it : locker) {
    if (it.first != kOffState) {
      return it.first;
    }
  }
  return ByteString();
}

WideString OptExportAt(const CPDF_Array* opt, size_t index) {
  RetainPtr<const CPDF_Object> element = opt->GetDirectObjectAt(index);
  if (!element) {
    return WideString();
  }
  const CPDF_Array* pair = element->AsArray();
  return pair ? pair->GetUnicodeTextAt(0) : element->GetUnicodeText();
}

// Populate the planning info of one resolved control and append it.
// Mirrors CPDF_FormControl::GetExportValue(): toggle /Opt entries are
// plain strings indexed by control ordinal, with a "Yes" fallback.
void FinishTxnControl(const CPDF_Array* opt_array,
                      bool want_toggle_info,
                      std::vector<TxnControl>* out,
                      TxnControl control) {
  if (want_toggle_info && control.dict) {
    control.on_state = ReadWidgetOnState(control.dict.Get());
    control.current_as = control.dict->GetNameFor("AS");
    const size_t ordinal = out->size();
    ByteString export_bytes = control.on_state;
    if (opt_array && ordinal < opt_array->size()) {
      export_bytes = opt_array->GetByteStringAt(ordinal);
    }
    if (export_bytes.IsEmpty()) {
      export_bytes = "Yes";
    }
    control.export_value = PDF_DecodeText(export_bytes.unsigned_span());
  }
  out->push_back(std::move(control));
}

// Resolve the widgets of a terminal field from its own dictionary, each
// through the document so layer promotions win. Fails when a kid carries
// /T: the target is a non-terminal field and value transactions must
// address terminal fields.
bool CollectRawTxnControls(CPDF_Document* doc,
                           const CPDF_Dictionary* field_dict,
                           uint32_t field_objnum,
                           bool want_toggle_info,
                           std::vector<TxnControl>* out) {
  RetainPtr<const CPDF_Array> opt_array;
  if (want_toggle_info) {
    opt_array = ToArray(CPDF_FormField::GetFieldAttrForDict(field_dict, "Opt"));
  }

  RetainPtr<const CPDF_Array> kids =
      field_dict->GetArrayFor(pdfium::form_fields::kKids);
  if (!kids) {
    TxnControl control;
    control.merged = true;
    control.objnum = field_objnum;
    control.dict = pdfium::WrapRetain(field_dict);
    FinishTxnControl(opt_array.Get(), want_toggle_info, out,
                     std::move(control));
    return true;
  }

  for (size_t i = 0; i < kids->size(); ++i) {
    RetainPtr<const CPDF_Object> element = kids->GetObjectAt(i);
    if (!element) {
      continue;
    }
    TxnControl control;
    control.kids_index = i;
    if (const CPDF_Reference* ref = element->AsReference()) {
      control.objnum = ref->GetRefObjNum();
      control.dict =
          ToDictionary(doc->GetOrParseIndirectObject(control.objnum));
    } else {
      control.dict = ToDictionary(std::move(element));
    }
    if (!control.dict) {
      continue;
    }
    if (control.dict->KeyExist(pdfium::form_fields::kT)) {
      return false;  // Child field: |field_dict| is not terminal.
    }
    FinishTxnControl(opt_array.Get(), want_toggle_info, out,
                     std::move(control));
  }
  return !out->empty();
}

// Locate the reconciled field owning |field_objnum|: the merged
// CPDF_FormField whose storage dictionary carries that object number.
CPDF_FormField* ReconciledFieldByObjNum(const CPDF_InteractiveForm* form,
                                        uint32_t field_objnum) {
  const size_t count = form->CountFields(WideString());
  for (size_t i = 0; i < count; ++i) {
    CPDF_FormField* field = form->GetField(i, WideString());
    if (field && field->GetFieldDict() &&
        field->GetFieldDict()->GetObjNum() == field_objnum) {
      return field;
    }
  }
  return nullptr;
}

// Resolve the widgets of a terminal field from the reconciled form view.
// Two-plane documents (the IRS f1040 class: an orphaned /AcroForm twin plus
// a standalone page-annot twin sharing one fully qualified name) fill
// correctly only when a write covers every twin — the raw /Kids walk cannot
// see across planes, but the reconciled control list is exactly the widget
// set the model snapshot reported to the caller. This mirrors what stock
// CPDF_FormField::CheckControl gets for free from its in-memory state.
bool CollectReconciledTxnControls(CPDF_Document* doc,
                                  const CPDF_InteractiveForm* form,
                                  const CPDF_Dictionary* field_dict,
                                  uint32_t field_objnum,
                                  bool want_toggle_info,
                                  std::vector<TxnControl>* out) {
  const CPDF_FormField* field = ReconciledFieldByObjNum(form, field_objnum);
  if (!field) {
    return false;
  }

  RetainPtr<const CPDF_Array> opt_array;
  if (want_toggle_info) {
    opt_array = ToArray(CPDF_FormField::GetFieldAttrForDict(field_dict, "Opt"));
  }

  const CPDF_Dictionary* storage_dict = field->GetFieldDict().Get();
  const int count = field->CountControls();
  for (int i = 0; i < count; ++i) {
    const CPDF_FormControl* form_control = field->GetControl(i);
    if (!form_control) {
      continue;
    }
    RetainPtr<const CPDF_Dictionary> control_dict =
        form_control->GetWidgetDict();
    if (!control_dict) {
      continue;
    }

    TxnControl control;
    const uint32_t objnum = control_dict->GetObjNum();
    if (objnum == field_objnum || control_dict.Get() == storage_dict) {
      // The merged control: the field dictionary itself is the widget.
      control.merged = true;
      control.objnum = field_objnum;
      control.dict = pdfium::WrapRetain(field_dict);
    } else if (objnum != 0) {
      control.objnum = objnum;
      // Re-resolve through the document so layer promotions win over the
      // instance the form captured at build time.
      control.dict = ToDictionary(doc->GetOrParseIndirectObject(objnum));
    } else {
      // Direct (spec-violating) kid: recover its /Kids index from the
      // form-held storage dictionary, then plan against the current view.
      RetainPtr<const CPDF_Array> storage_kids =
          storage_dict->GetArrayFor(pdfium::form_fields::kKids);
      RetainPtr<const CPDF_Array> current_kids =
          field_dict->GetArrayFor(pdfium::form_fields::kKids);
      if (!storage_kids || !current_kids) {
        continue;
      }
      for (size_t k = 0; k < storage_kids->size(); ++k) {
        if (storage_kids->GetDictAt(k).Get() == control_dict.Get()) {
          control.kids_index = k;
          control.dict = current_kids->GetDictAt(k);
          break;
        }
      }
      if (!control.dict) {
        continue;
      }
    }
    if (!control.dict) {
      continue;
    }
    FinishTxnControl(opt_array.Get(), want_toggle_info, out,
                     std::move(control));
  }
  return !out->empty();
}

// Resolve the widgets of a terminal field for a transaction. The reconciled
// view is authoritative — reads and writes must see the SAME widget set.
// Falls back to the raw /Kids walk for fields the interactive form cannot
// represent (unnamed, type-less, or unplaced authoring drafts). |reconciled|
// may be null; batch callers (interchange import) pass their own so the
// form is built once per batch instead of once per field.
bool CollectTxnControls(CPDF_Document* doc,
                        const CPDF_Dictionary* field_dict,
                        uint32_t field_objnum,
                        bool want_toggle_info,
                        const CPDF_InteractiveForm* reconciled,
                        std::vector<TxnControl>* out) {
  std::unique_ptr<CPDF_InteractiveForm> owned_form;
  if (!reconciled) {
    owned_form = BuildReconciledForm(doc);
    reconciled = owned_form.get();
  }
  if (CollectReconciledTxnControls(doc, reconciled, field_dict, field_objnum,
                                   want_toggle_info, out)) {
    return true;
  }
  out->clear();
  return CollectRawTxnControls(doc, field_dict, field_objnum, want_toggle_info,
                               out);
}

// Resolve a control for mutation. Everything routes through promotion:
// indirect widgets promote themselves; direct kids are reached through the
// already-promoted field clone.
RetainPtr<CPDF_Dictionary> MutableControlDict(
    CPDF_Document* doc,
    const TxnControl& control,
    const RetainPtr<CPDF_Dictionary>& promoted_field) {
  if (control.merged) {
    return promoted_field;
  }
  if (control.objnum != 0) {
    return ToDictionary(doc->GetMutableIndirectObject(control.objnum));
  }
  RetainPtr<CPDF_Array> kids =
      promoted_field->GetMutableArrayFor(pdfium::form_fields::kKids);
  return kids ? kids->GetMutableDictAt(control.kids_index) : nullptr;
}

void ReportChangedWidgets(const std::vector<uint32_t>& changed_objnums,
                          unsigned long total_changed,
                          uint32_t* buffer,
                          unsigned long buffer_size,
                          unsigned long* out_changed_count) {
  if (buffer && buffer_size > 0) {
    pdfium::span<uint32_t> out_span =
        UNSAFE_BUFFERS(pdfium::span(buffer, static_cast<size_t>(buffer_size)));
    const size_t n = std::min(out_span.size(), changed_objnums.size());
    fxcrt::Copy(pdfium::span(changed_objnums).first(n), out_span);
  }
  if (out_changed_count) {
    *out_changed_count = total_changed;
  }
}

CPDF_GenerateAP::FormType ChoiceFormType(uint32_t flags) {
  return (flags & pdfium::form_flags::kChoiceCombo) ? CPDF_GenerateAP::kComboBox
                                                    : CPDF_GenerateAP::kListBox;
}

struct NormalizedChoiceValues {
  bool free_text = false;
  std::vector<std::pair<size_t, WideString>> matched;
};

std::optional<std::vector<WideString>> ReadChoiceValues(
    const CPDF_Object* object) {
  std::vector<WideString> values;
  if (!object || object->IsNull()) {
    return values;
  }
  if (object->IsString()) {
    values.push_back(object->GetUnicodeText());
    return values;
  }
  const CPDF_Array* array = object->AsArray();
  if (!array) {
    return std::nullopt;
  }
  values.reserve(array->size());
  for (size_t i = 0; i < array->size(); ++i) {
    RetainPtr<const CPDF_Object> element = array->GetDirectObjectAt(i);
    if (!element || !element->IsString()) {
      return std::nullopt;
    }
    values.push_back(element->GetUnicodeText());
  }
  return values;
}

std::vector<WideString> FilterChoiceValues(
    const std::vector<WideString>& values,
    const std::vector<WideString>& available_exports,
    bool preserve_free_text) {
  std::vector<WideString> kept;
  for (const WideString& value : values) {
    if (pdfium::Contains(available_exports, value)) {
      kept.push_back(value);
    }
  }
  if (kept.empty() && preserve_free_text && !values.empty()) {
    kept = values;
  }
  return kept;
}

std::optional<NormalizedChoiceValues> NormalizeChoiceValues(
    const CPDF_Dictionary* field,
    uint32_t flags,
    const std::vector<WideString>& values) {
  const bool is_combo = flags & pdfium::form_flags::kChoiceCombo;
  const bool is_edit = flags & pdfium::form_flags::kChoiceEdit;
  const bool is_multi = flags & pdfium::form_flags::kChoiceMultiSelect;
  if (values.size() > 1 && (is_combo || !is_multi)) {
    return std::nullopt;
  }

  RetainPtr<const CPDF_Array> opt_array =
      ToArray(CPDF_FormField::GetFieldAttrForDict(field, "Opt"));
  NormalizedChoiceValues normalized;
  bool all_matched = true;
  for (const WideString& value : values) {
    bool found = false;
    if (opt_array) {
      for (size_t i = 0; i < opt_array->size(); ++i) {
        if (OptExportAt(opt_array.Get(), i) == value) {
          normalized.matched.emplace_back(i, value);
          found = true;
          break;
        }
      }
    }
    all_matched = all_matched && found;
  }
  normalized.free_text = !all_matched;
  if (normalized.free_text && !(is_combo && is_edit && values.size() == 1)) {
    return std::nullopt;
  }

  std::sort(normalized.matched.begin(), normalized.matched.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  normalized.matched.erase(
      std::unique(
          normalized.matched.begin(), normalized.matched.end(),
          [](const auto& a, const auto& b) { return a.first == b.first; }),
      normalized.matched.end());
  return normalized;
}

void WriteChoiceDefaultValues(CPDF_Dictionary* field,
                              const std::vector<WideString>& requested_values,
                              const NormalizedChoiceValues& normalized) {
  if (normalized.free_text) {
    field->SetNewFor<CPDF_String>(pdfium::form_fields::kDV,
                                  requested_values[0].AsStringView());
    return;
  }
  if (normalized.matched.size() == 1) {
    field->SetNewFor<CPDF_String>(pdfium::form_fields::kDV,
                                  normalized.matched[0].second.AsStringView());
    return;
  }
  auto defaults = field->SetNewFor<CPDF_Array>(pdfium::form_fields::kDV);
  for (const auto& entry : normalized.matched) {
    defaults->AppendNew<CPDF_String>(entry.second.AsStringView());
  }
}

// Toggle transactions mirror CPDF_FormField::CheckControl semantics:
// checkboxes are always in unison; radios only with the RadiosInUnison
// flag; /V holds the export value name, or the control index when the
// field carries /Opt.
struct ToggleContext {
  RetainPtr<const CPDF_Dictionary> field;
  uint32_t flags = 0;
  bool is_radio = false;
  std::vector<TxnControl> controls;
};

bool PrepareToggle(CPDF_Document* doc,
                   const CPDF_InteractiveForm* reconciled,
                   uint32_t field_objnum,
                   ToggleContext* ctx) {
  ctx->field = ResolveFieldDict(doc, field_objnum);
  if (!ctx->field ||
      InheritedFieldType(ctx->field.Get()) != pdfium::form_fields::kBtn) {
    return false;
  }
  ctx->flags = InheritedFieldFlags(ctx->field.Get());
  if (ctx->flags & pdfium::form_flags::kButtonPushbutton) {
    return false;
  }
  ctx->is_radio = ctx->flags & pdfium::form_flags::kButtonRadio;
  return CollectTxnControls(doc, ctx->field.Get(), field_objnum,
                            /*want_toggle_info=*/true, reconciled,
                            &ctx->controls);
}

bool RejectClearForNoToggleToOff(const ToggleContext& ctx) {
  return ctx.is_radio && (ctx.flags & pdfium::form_flags::kButtonNoToggleToOff);
}

bool ExecuteToggle(CPDF_Document* doc,
                   uint32_t field_objnum,
                   const ToggleContext& ctx,
                   const TxnControl* target,
                   size_t target_ordinal,
                   uint32_t* changed_widget_objnums,
                   unsigned long buffer_size,
                   unsigned long* out_changed_count) {
  const bool unison =
      !ctx.is_radio || (ctx.flags & pdfium::form_flags::kButtonRadiosInUnison);

  // Plan: new /AS per control, new /V for the field.
  struct Step {
    size_t control_index;
    ByteString new_as;
  };
  std::vector<Step> steps;
  for (size_t i = 0; i < ctx.controls.size(); ++i) {
    const TxnControl& control = ctx.controls[i];
    bool checked = false;
    if (target) {
      checked = unison ? control.export_value == target->export_value &&
                             control.on_state == target->on_state
                       : i == target_ordinal;
    }
    ByteString new_as = checked ? control.on_state : ByteString(kOffState);
    if (new_as != control.current_as) {
      steps.push_back({i, std::move(new_as)});
    }
  }

  RetainPtr<const CPDF_Array> opt_array =
      ToArray(CPDF_FormField::GetFieldAttrForDict(ctx.field.Get(), "Opt"));
  ByteString new_v = kOffState;
  if (target) {
    new_v = opt_array ? ByteString::FormatInteger(
                            pdfium::checked_cast<int>(target_ordinal))
                      : PDF_EncodeText(target->export_value.AsStringView());
  }
  RetainPtr<const CPDF_Object> current_v = CPDF_FormField::GetFieldAttrForDict(
      ctx.field.Get(), pdfium::form_fields::kV);
  const bool v_changes = !current_v || current_v->GetString() != new_v;

  if (steps.empty() && !v_changes) {
    ReportChangedWidgets({}, 0, changed_widget_objnums, buffer_size,
                         out_changed_count);
    return true;
  }

  // Apply. First mutable access happens here; promotion is now safe.
  const bool need_promoted_field =
      v_changes || std::any_of(steps.begin(), steps.end(), [&](const Step& s) {
        const TxnControl& c = ctx.controls[s.control_index];
        return c.merged || c.objnum == 0;
      });
  RetainPtr<CPDF_Dictionary> promoted_field;
  if (need_promoted_field) {
    promoted_field = ToDictionary(doc->GetMutableIndirectObject(field_objnum));
    if (!promoted_field) {
      return false;
    }
  }
  if (v_changes) {
    promoted_field->SetNewFor<CPDF_Name>(pdfium::form_fields::kV, new_v);
  }

  std::vector<uint32_t> changed;
  unsigned long total_changed = 0;
  for (const Step& step : steps) {
    const TxnControl& control = ctx.controls[step.control_index];
    RetainPtr<CPDF_Dictionary> widget =
        MutableControlDict(doc, control, promoted_field);
    if (!widget) {
      continue;
    }
    widget->SetNewFor<CPDF_Name>("AS", step.new_as);
    ++total_changed;
    if (control.objnum != 0) {
      changed.push_back(control.objnum);
    }
  }
  ReportChangedWidgets(changed, total_changed, changed_widget_objnums,
                       buffer_size, out_changed_count);
  return true;
}

// Select the target widget by appearance state name ("Off"/empty clears).
bool ApplyToggle(CPDF_Document* doc,
                 const CPDF_InteractiveForm* reconciled,
                 uint32_t field_objnum,
                 const ByteString& requested_state,
                 bool lenient_unknown_state,
                 uint32_t* changed_widget_objnums,
                 unsigned long buffer_size,
                 unsigned long* out_changed_count) {
  ToggleContext ctx;
  if (!PrepareToggle(doc, reconciled, field_objnum, &ctx)) {
    return false;
  }
  const bool clearing =
      requested_state.IsEmpty() || requested_state == kOffState;
  if (clearing && RejectClearForNoToggleToOff(ctx)) {
    return false;
  }
  const TxnControl* target = nullptr;
  size_t target_ordinal = 0;
  if (!clearing) {
    for (size_t i = 0; i < ctx.controls.size(); ++i) {
      if (ctx.controls[i].on_state == requested_state) {
        target = &ctx.controls[i];
        target_ordinal = i;
        break;
      }
    }
    if (!target && !lenient_unknown_state) {
      return false;
    }
  }
  return ExecuteToggle(doc, field_objnum, ctx, target, target_ordinal,
                       changed_widget_objnums, buffer_size, out_changed_count);
}

// Select the target widget by export value - the identity FDF/XFDF carry.
bool ApplyToggleByExport(CPDF_Document* doc,
                         const CPDF_InteractiveForm* reconciled,
                         uint32_t field_objnum,
                         const WideString& export_value,
                         uint32_t* changed_widget_objnums,
                         unsigned long buffer_size,
                         unsigned long* out_changed_count) {
  ToggleContext ctx;
  if (!PrepareToggle(doc, reconciled, field_objnum, &ctx)) {
    return false;
  }
  const bool clearing = export_value.IsEmpty() || export_value == L"Off";
  if (clearing && RejectClearForNoToggleToOff(ctx)) {
    return false;
  }
  const TxnControl* target = nullptr;
  size_t target_ordinal = 0;
  if (!clearing) {
    for (size_t i = 0; i < ctx.controls.size(); ++i) {
      if (ctx.controls[i].export_value == export_value) {
        target = &ctx.controls[i];
        target_ordinal = i;
        break;
      }
    }
    if (!target) {
      return false;
    }
  }
  return ExecuteToggle(doc, field_objnum, ctx, target, target_ordinal,
                       changed_widget_objnums, buffer_size, out_changed_count);
}

bool ResolveToggleDefault(const ToggleContext& ctx,
                          const CPDF_Object* default_value,
                          const TxnControl** out_target,
                          size_t* out_target_ordinal) {
  *out_target = nullptr;
  *out_target_ordinal = 0;
  if (!default_value || default_value->IsNull()) {
    return true;
  }
  if (!default_value->IsName()) {
    return false;
  }

  const ByteString raw_default = default_value->GetString();
  if (raw_default == kOffState) {
    return true;
  }
  RetainPtr<const CPDF_Array> opt_array =
      ToArray(CPDF_FormField::GetFieldAttrForDict(ctx.field.Get(), "Opt"));
  for (size_t i = 0; i < ctx.controls.size(); ++i) {
    const ByteString checked_value =
        opt_array ? ByteString::FormatInteger(pdfium::checked_cast<int>(i))
                  : ctx.controls[i].on_state;
    if (checked_value == raw_default) {
      *out_target = &ctx.controls[i];
      *out_target_ordinal = i;
      return true;
    }
  }
  return false;
}

bool ApplyToggleDefault(CPDF_Document* doc,
                        uint32_t field_objnum,
                        const CPDF_Object* default_value,
                        uint32_t* changed_widget_objnums,
                        unsigned long buffer_size,
                        unsigned long* out_changed_count) {
  ToggleContext ctx;
  if (!PrepareToggle(doc, /*reconciled=*/nullptr, field_objnum, &ctx)) {
    return false;
  }
  const TxnControl* target = nullptr;
  size_t target_ordinal = 0;
  if (!ResolveToggleDefault(ctx, default_value, &target, &target_ordinal)) {
    return false;
  }
  // NoToggleToOff governs interactive changes, not restoring the declared
  // default. A missing or explicit /Off default must still reset to Off.
  return ExecuteToggle(doc, field_objnum, ctx, target, target_ordinal,
                       changed_widget_objnums, buffer_size, out_changed_count);
}

// Same-FQN twin controls are field roots in their own plane (they carry
// /T). Value state must land on them too: appearance generation re-reads
// the value by climbing the control's own dictionary chain — which never
// crosses planes — and per-widget readers in other viewers do the same.
// Runs after the field-level value write and before appearance regen.
void MirrorFieldValueToTwinControls(
    CPDF_Document* doc,
    const RetainPtr<CPDF_Dictionary>& promoted_field,
    const std::vector<TxnControl>& controls) {
  for (const TxnControl& control : controls) {
    if (control.merged || !control.dict ||
        !control.dict->KeyExist(pdfium::form_fields::kT)) {
      continue;
    }
    RetainPtr<CPDF_Dictionary> twin =
        MutableControlDict(doc, control, promoted_field);
    if (!twin || twin == promoted_field) {
      continue;
    }
    for (const char* key : {pdfium::form_fields::kV, "I", "RV"}) {
      RetainPtr<const CPDF_Object> value = promoted_field->GetObjectFor(key);
      if (value) {
        twin->SetFor(key, value->Clone());
      } else {
        twin->RemoveFor(key);
      }
    }
  }
}

// Regenerate the /AP of every control and report them all as changed.
bool RegenerateControlAppearances(
    CPDF_Document* doc,
    const std::vector<TxnControl>& controls,
    const RetainPtr<CPDF_Dictionary>& promoted_field,
    CPDF_GenerateAP::FormType type,
    uint32_t* changed_widget_objnums,
    unsigned long buffer_size,
    unsigned long* out_changed_count) {
  std::vector<uint32_t> changed;
  unsigned long total_changed = 0;
  for (const TxnControl& control : controls) {
    RetainPtr<CPDF_Dictionary> widget =
        MutableControlDict(doc, control, promoted_field);
    if (!widget) {
      continue;
    }
    CPDF_GenerateAP::GenerateFormAP(doc, widget.Get(), type);
    ++total_changed;
    if (control.objnum != 0) {
      changed.push_back(control.objnum);
    }
  }
  ReportChangedWidgets(changed, total_changed, changed_widget_objnums,
                       buffer_size, out_changed_count);
  return true;
}

// Internal text transaction; the public wrapper converts the wire string.
bool ApplyTextValue(CPDF_Document* doc,
                    const CPDF_InteractiveForm* reconciled,
                    uint32_t field_objnum,
                    const WideString& new_value,
                    uint32_t* changed_widget_objnums,
                    unsigned long buffer_size,
                    unsigned long* out_changed_count) {
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field || InheritedFieldType(field.Get()) != pdfium::form_fields::kTx) {
    return false;
  }

  RetainPtr<const CPDF_Object> max_len_obj =
      CPDF_FormField::GetFieldAttrForDict(field.Get(), "MaxLen");
  const int max_len = max_len_obj ? max_len_obj->GetInteger() : 0;
  const WideString normalized_value =
      max_len > 0 && new_value.GetLength() > static_cast<size_t>(max_len)
          ? new_value.First(static_cast<size_t>(max_len))
          : new_value;

  std::vector<TxnControl> controls;
  if (!CollectTxnControls(doc, field.Get(), field_objnum,
                          /*want_toggle_info=*/false, reconciled, &controls)) {
    return false;
  }

  RetainPtr<const CPDF_Object> current_v =
      CPDF_FormField::GetFieldAttrForDict(field.Get(), pdfium::form_fields::kV);
  const WideString current_value =
      current_v ? current_v->GetUnicodeText() : WideString();
  if (current_value == normalized_value && !field->KeyExist("RV")) {
    ReportChangedWidgets({}, 0, changed_widget_objnums, buffer_size,
                         out_changed_count);
    return true;
  }

  RetainPtr<CPDF_Dictionary> promoted_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!promoted_field) {
    return false;
  }
  promoted_field->SetNewFor<CPDF_String>(pdfium::form_fields::kV,
                                         normalized_value.AsStringView());
  // A rich text value would now contradict /V; drop it rather than lie.
  promoted_field->RemoveFor("RV");
  MirrorFieldValueToTwinControls(doc, promoted_field, controls);

  return RegenerateControlAppearances(
      doc, controls, promoted_field, CPDF_GenerateAP::kTextField,
      changed_widget_objnums, buffer_size, out_changed_count);
}

// Internal choice transaction; the public wrapper converts the wire strings.
bool ApplyChoiceValues(CPDF_Document* doc,
                       const CPDF_InteractiveForm* reconciled,
                       uint32_t field_objnum,
                       const std::vector<WideString>& new_values,
                       uint32_t* changed_widget_objnums,
                       unsigned long buffer_size,
                       unsigned long* out_changed_count) {
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field || InheritedFieldType(field.Get()) != pdfium::form_fields::kCh) {
    return false;
  }
  const uint32_t flags = InheritedFieldFlags(field.Get());
  std::optional<NormalizedChoiceValues> normalized =
      NormalizeChoiceValues(field.Get(), flags, new_values);
  if (!normalized.has_value()) {
    return false;
  }

  std::vector<TxnControl> controls;
  if (!CollectTxnControls(doc, field.Get(), field_objnum,
                          /*want_toggle_info=*/false, reconciled, &controls)) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> promoted_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!promoted_field) {
    return false;
  }

  if (new_values.empty()) {
    if (HasInheritedFieldAttribute(doc, field.Get(), pdfium::form_fields::kV)) {
      if (!(flags & pdfium::form_flags::kChoiceCombo) &&
          (flags & pdfium::form_flags::kChoiceMultiSelect)) {
        promoted_field->SetNewFor<CPDF_Array>(pdfium::form_fields::kV);
      } else {
        promoted_field->SetNewFor<CPDF_String>(pdfium::form_fields::kV,
                                               WideStringView());
      }
    } else {
      promoted_field->RemoveFor(pdfium::form_fields::kV);
    }
    if (HasInheritedFieldAttribute(doc, field.Get(), "I")) {
      promoted_field->SetNewFor<CPDF_Array>("I");
    } else {
      promoted_field->RemoveFor("I");
    }
  } else if (normalized->free_text) {
    promoted_field->SetNewFor<CPDF_String>(pdfium::form_fields::kV,
                                           new_values[0].AsStringView());
    if (HasInheritedFieldAttribute(doc, field.Get(), "I")) {
      promoted_field->SetNewFor<CPDF_Array>("I");
    } else {
      promoted_field->RemoveFor("I");
    }
  } else {
    if (normalized->matched.size() == 1) {
      promoted_field->SetNewFor<CPDF_String>(
          pdfium::form_fields::kV,
          normalized->matched[0].second.AsStringView());
    } else {
      auto value_array =
          promoted_field->SetNewFor<CPDF_Array>(pdfium::form_fields::kV);
      for (const auto& entry : normalized->matched) {
        value_array->AppendNew<CPDF_String>(entry.second.AsStringView());
      }
    }
    auto index_array = promoted_field->SetNewFor<CPDF_Array>("I");
    for (const auto& entry : normalized->matched) {
      index_array->AppendNew<CPDF_Number>(
          pdfium::checked_cast<int>(entry.first));
    }
  }
  promoted_field->RemoveFor("RV");
  MirrorFieldValueToTwinControls(doc, promoted_field, controls);

  return RegenerateControlAppearances(
      doc, controls, promoted_field, ChoiceFormType(flags),
      changed_widget_objnums, buffer_size, out_changed_count);
}

uint32_t DisplayFlags(uint32_t current_flags, int display) {
  switch (display) {
    case EPDF_FORM_DISPLAY_VISIBLE:
      return (current_flags & ~(pdfium::annotation_flags::kInvisible |
                                pdfium::annotation_flags::kHidden |
                                pdfium::annotation_flags::kNoView)) |
             pdfium::annotation_flags::kPrint;
    case EPDF_FORM_DISPLAY_HIDDEN:
      return (current_flags & ~(pdfium::annotation_flags::kInvisible |
                                pdfium::annotation_flags::kNoView)) |
             pdfium::annotation_flags::kHidden |
             pdfium::annotation_flags::kPrint;
    case EPDF_FORM_DISPLAY_NO_PRINT:
      return current_flags & ~(pdfium::annotation_flags::kInvisible |
                               pdfium::annotation_flags::kHidden |
                               pdfium::annotation_flags::kPrint |
                               pdfium::annotation_flags::kNoView);
    case EPDF_FORM_DISPLAY_NO_VIEW:
      return (current_flags & ~pdfium::annotation_flags::kHidden) |
             pdfium::annotation_flags::kNoView |
             pdfium::annotation_flags::kPrint;
    default:
      return current_flags;
  }
}

bool ApplyFieldDisplay(CPDF_Document* doc,
                       uint32_t field_objnum,
                       int display,
                       uint32_t* changed_widget_objnums,
                       unsigned long buffer_size,
                       unsigned long* out_changed_count) {
  if (display < EPDF_FORM_DISPLAY_VISIBLE ||
      display > EPDF_FORM_DISPLAY_NO_VIEW) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field) {
    return false;
  }
  std::vector<TxnControl> controls;
  if (!CollectTxnControls(doc, field.Get(), field_objnum,
                          /*want_toggle_info=*/false, /*reconciled=*/nullptr,
                          &controls)) {
    return false;
  }

  struct Step {
    size_t control_index;
    uint32_t flags;
  };
  std::vector<Step> steps;
  for (size_t i = 0; i < controls.size(); ++i) {
    const uint32_t current_flags =
        static_cast<uint32_t>(controls[i].dict->GetIntegerFor("F"));
    const uint32_t new_flags = DisplayFlags(current_flags, display);
    if (new_flags != current_flags) {
      steps.push_back({i, new_flags});
    }
  }
  if (steps.empty()) {
    ReportChangedWidgets({}, 0, changed_widget_objnums, buffer_size,
                         out_changed_count);
    return true;
  }

  const bool needs_promoted_field =
      std::any_of(steps.begin(), steps.end(), [&](const Step& step) {
        const TxnControl& control = controls[step.control_index];
        return control.merged || control.objnum == 0;
      });
  RetainPtr<CPDF_Dictionary> promoted_field;
  if (needs_promoted_field) {
    promoted_field = ToDictionary(doc->GetMutableIndirectObject(field_objnum));
    if (!promoted_field) {
      return false;
    }
  }

  std::vector<uint32_t> changed;
  unsigned long total_changed = 0;
  for (const Step& step : steps) {
    const TxnControl& control = controls[step.control_index];
    RetainPtr<CPDF_Dictionary> widget =
        MutableControlDict(doc, control, promoted_field);
    if (!widget) {
      return false;
    }
    widget->SetNewFor<CPDF_Number>("F", static_cast<int>(step.flags));
    ++total_changed;
    if (control.objnum != 0) {
      changed.push_back(control.objnum);
    }
  }
  ReportChangedWidgets(changed, total_changed, changed_widget_objnums,
                       buffer_size, out_changed_count);
  return true;
}

bool ApplyFieldAppearanceText(CPDF_Document* doc,
                              uint32_t field_objnum,
                              const WideString& appearance_text,
                              uint32_t* changed_widget_objnums,
                              unsigned long buffer_size,
                              unsigned long* out_changed_count) {
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field) {
    return false;
  }
  const ByteString field_type = InheritedFieldType(field.Get());
  CPDF_GenerateAP::FormType appearance_type;
  if (field_type == pdfium::form_fields::kTx) {
    appearance_type = CPDF_GenerateAP::kTextField;
  } else if (field_type == pdfium::form_fields::kCh &&
             (InheritedFieldFlags(field.Get()) &
              pdfium::form_flags::kChoiceCombo)) {
    appearance_type = CPDF_GenerateAP::kComboBox;
  } else {
    return false;
  }

  std::vector<TxnControl> controls;
  if (!CollectTxnControls(doc, field.Get(), field_objnum,
                          /*want_toggle_info=*/false, /*reconciled=*/nullptr,
                          &controls)) {
    return false;
  }
  const bool needs_promoted_field = std::any_of(
      controls.begin(), controls.end(), [](const TxnControl& control) {
        return control.merged || control.objnum == 0;
      });
  RetainPtr<CPDF_Dictionary> promoted_field;
  if (needs_promoted_field) {
    promoted_field = ToDictionary(doc->GetMutableIndirectObject(field_objnum));
    if (!promoted_field) {
      return false;
    }
  }

  std::vector<uint32_t> changed;
  unsigned long total_changed = 0;
  for (const TxnControl& control : controls) {
    RetainPtr<CPDF_Dictionary> widget =
        MutableControlDict(doc, control, promoted_field);
    if (!widget || !CPDF_GenerateAP::GenerateFormAPWithValueOverride(
                       doc, widget.Get(), appearance_type, appearance_text)) {
      return false;
    }
    ++total_changed;
    if (control.objnum != 0) {
      changed.push_back(control.objnum);
    }
  }
  ReportChangedWidgets(changed, total_changed, changed_widget_objnums,
                       buffer_size, out_changed_count);
  return true;
}

// ---------------------------------------------------------------------------
// FDF / XFDF interchange helpers.
// ---------------------------------------------------------------------------

unsigned long CopyPayloadToBuffer(const ByteString& payload,
                                  void* buffer,
                                  unsigned long buflen) {
  const auto length = static_cast<unsigned long>(payload.GetLength());
  if (buffer && length > 0 && buflen >= length) {
    fxcrt::Copy(payload.unsigned_span(),
                UNSAFE_BUFFERS(pdfium::span(static_cast<uint8_t*>(buffer),
                                            static_cast<size_t>(buflen))));
  }
  return length;
}

struct ImportStats {
  uint32_t total = 0;
  uint32_t applied = 0;
  uint32_t skipped = 0;
  uint32_t widgets_changed = 0;
};

void WriteImportResult(const ImportStats& stats,
                       EPDF_FORM_IMPORT_RESULT* out_result) {
  if (!out_result) {
    return;
  }
  out_result->fields_total = stats.total;
  out_result->fields_applied = stats.applied;
  out_result->fields_skipped = stats.skipped;
  out_result->widgets_changed = stats.widgets_changed;
}

// Route one imported (fqn, values) entry through the typed transactions.
void ApplyImportedValues(CPDF_Document* doc,
                         CPDF_InteractiveForm* form,
                         const WideString& fqn,
                         const std::vector<WideString>& values,
                         ImportStats* stats) {
  ++stats->total;
  CPDF_FormField* field =
      form->CountFields(fqn) > 0 ? form->GetField(0, fqn) : nullptr;
  if (!field || values.empty()) {
    ++stats->skipped;
    return;
  }
  const uint32_t field_objnum = field->GetFieldDict()->GetObjNum();
  if (field_objnum == 0) {
    ++stats->skipped;
    return;
  }

  unsigned long changed = 0;
  bool applied = false;
  switch (field->GetType()) {
    case CPDF_FormField::kCheckBox:
    case CPDF_FormField::kRadioButton:
      applied = values.size() == 1 &&
                ApplyToggleByExport(doc, form, field_objnum, values[0], nullptr,
                                    0, &changed);
      break;
    case CPDF_FormField::kText:
    case CPDF_FormField::kRichText:
    case CPDF_FormField::kFile:
      applied = values.size() == 1 &&
                ApplyTextValue(doc, form, field_objnum, values[0], nullptr, 0,
                               &changed);
      break;
    case CPDF_FormField::kComboBox:
    case CPDF_FormField::kListBox:
      applied = ApplyChoiceValues(doc, form, field_objnum, values, nullptr, 0,
                                  &changed);
      break;
    default:
      break;  // Push buttons, signatures, unknown: never written.
  }
  if (applied) {
    ++stats->applied;
    stats->widgets_changed += static_cast<uint32_t>(changed);
  } else {
    ++stats->skipped;
  }
}

// Walk an FDF /Fields array: flat entries with dotted /T names and
// hierarchical /Kids trees both resolve to fully qualified names.
void WalkFdfFields(CPDF_Document* doc,
                   CPDF_InteractiveForm* form,
                   const CPDF_Array* entries,
                   const WideString& prefix,
                   ImportStats* stats,
                   int depth) {
  if (!entries || depth > 32) {
    return;
  }
  for (size_t i = 0; i < entries->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> entry = entries->GetDictAt(i);
    if (!entry) {
      continue;
    }
    const WideString name = entry->GetUnicodeTextFor("T");
    WideString fqn = prefix;
    if (!name.IsEmpty()) {
      fqn = prefix.IsEmpty() ? name : prefix + L"." + name;
    }
    RetainPtr<const CPDF_Object> value = entry->GetDirectObjectFor("V");
    if (value && !fqn.IsEmpty()) {
      std::vector<WideString> values;
      if (const CPDF_Array* value_array = value->AsArray()) {
        for (size_t j = 0; j < value_array->size(); ++j) {
          values.push_back(value_array->GetUnicodeTextAt(j));
        }
      } else {
        values.push_back(value->GetUnicodeText());
      }
      ApplyImportedValues(doc, form, fqn, values, stats);
    }
    RetainPtr<const CPDF_Array> kids = entry->GetArrayFor("Kids");
    if (kids) {
      WalkFdfFields(doc, form, kids.Get(), fqn, stats, depth + 1);
    }
  }
}

// XFDF field tree, keyed by fully-qualified-name component.
struct XfdfNode {
  std::map<WideString, XfdfNode> children;
  std::vector<WideString> values;
};

bool IsFieldValueEmpty(const CPDF_Object* value) {
  if (!value || value->IsNull()) {
    return true;
  }
  const CPDF_Array* array = value->AsArray();
  return array ? array->IsEmpty() : value->GetString().IsEmpty();
}

// Assemble <field>/<value> elements into the document-owned DOM.
void EmitXfdfFieldNodes(const std::map<WideString, XfdfNode>& nodes,
                        CFX_XMLDocument* xml,
                        CFX_XMLElement* parent) {
  for (const auto& it : nodes) {
    CFX_XMLElement* field = xml->CreateNode<CFX_XMLElement>(L"field");
    field->SetAttribute(L"name", it.first);
    parent->AppendLastChild(field);
    for (const WideString& value : it.second.values) {
      CFX_XMLElement* value_element = xml->CreateNode<CFX_XMLElement>(L"value");
      value_element->AppendLastChild(xml->CreateNode<CFX_XMLText>(value));
      field->AppendLastChild(value_element);
    }
    EmitXfdfFieldNodes(it.second.children, xml, field);
  }
}

ByteString BuildXfdf(CPDF_InteractiveForm* form,
                     const WideString& pdf_path,
                     bool skip_empty_required) {
  std::map<WideString, XfdfNode> root;
  const size_t field_count = form->CountFields(WideString());
  for (size_t i = 0; i < field_count; ++i) {
    CPDF_FormField* field = form->GetField(i, WideString());
    if (!field) {
      continue;
    }
    const CPDF_FormField::Type type = field->GetType();
    if (type == CPDF_FormField::kPushButton || type == CPDF_FormField::kSign) {
      continue;
    }
    const uint32_t flags = field->GetFieldFlags();
    if (flags & pdfium::form_flags::kNoExport) {
      continue;
    }
    RetainPtr<const CPDF_Object> value_object =
        field->GetFieldAttr(pdfium::form_fields::kV);
    if (skip_empty_required && (flags & pdfium::form_flags::kRequired) &&
        IsFieldValueEmpty(value_object.Get())) {
      continue;
    }
    const WideString fqn = field->GetFullName();
    if (fqn.IsEmpty()) {
      continue;
    }

    // Nest by fully-qualified-name component.
    std::map<WideString, XfdfNode>* level = &root;
    XfdfNode* node = nullptr;
    size_t start = 0;
    while (true) {
      std::optional<size_t> dot = fqn.Find(L'.', start);
      const size_t end = dot.value_or(fqn.GetLength());
      node = &(*level)[fqn.Substr(start, end - start)];
      level = &node->children;
      if (!dot.has_value()) {
        break;
      }
      start = dot.value() + 1;
    }

    if (value_object) {
      if (const CPDF_Array* value_array = value_object->AsArray()) {
        for (size_t j = 0; j < value_array->size(); ++j) {
          node->values.push_back(value_array->GetUnicodeTextAt(j));
        }
      } else {
        // Toggles surface the checked export value ("Off" when cleared),
        // matching the FDF exporter.
        node->values.push_back(field->GetValue());
      }
    }
  }

  // Serialize through the CFX_XML DOM: EncodeEntities() is the single
  // escaping authority (the exact inverse of the parser used on import),
  // and SaveCompact() keeps text content whitespace-exact as XFDF's
  // xml:space="preserve" requires.
  CFX_XMLDocument xml;
  CFX_XMLElement* xfdf = xml.CreateNode<CFX_XMLElement>(L"xfdf");
  xfdf->SetAttribute(L"xmlns", L"http://ns.adobe.com/xfdf/");
  xfdf->SetAttribute(L"xml:space", L"preserve");
  CFX_XMLElement* fields = xml.CreateNode<CFX_XMLElement>(L"fields");
  xfdf->AppendLastChild(fields);
  EmitXfdfFieldNodes(root, &xml, fields);
  if (!pdf_path.IsEmpty()) {
    CFX_XMLElement* filespec = xml.CreateNode<CFX_XMLElement>(L"f");
    filespec->SetAttribute(L"href", pdf_path);
    xfdf->AppendLastChild(filespec);
  }

  auto stream = pdfium::MakeRetain<CFX_MemoryStream>();
  stream->WriteString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  xfdf->SaveCompact(stream);
  return ByteString(ByteStringView(stream->GetSpan()));
}

CFX_XMLElement* FindXmlChildByTag(CFX_XMLNode* parent, WideStringView tag) {
  for (CFX_XMLNode* child = parent->GetFirstChild(); child;
       child = child->GetNextSibling()) {
    CFX_XMLElement* element = ToXMLElement(child);
    if (element && element->GetLocalTagName() == tag) {
      return element;
    }
  }
  return nullptr;
}

// Accepts nested <field> elements and dotted name attributes; multiple
// <value> children form a multi-select selection.
void WalkXfdfField(CPDF_Document* doc,
                   CPDF_InteractiveForm* form,
                   CFX_XMLElement* element,
                   const WideString& prefix,
                   ImportStats* stats,
                   int depth) {
  if (depth > 32) {
    return;
  }
  const WideString name = element->GetAttribute(L"name");
  WideString fqn = prefix;
  if (!name.IsEmpty()) {
    fqn = prefix.IsEmpty() ? name : prefix + L"." + name;
  }
  std::vector<WideString> values;
  for (CFX_XMLNode* child = element->GetFirstChild(); child;
       child = child->GetNextSibling()) {
    CFX_XMLElement* child_element = ToXMLElement(child);
    if (!child_element) {
      continue;
    }
    const WideString tag = child_element->GetLocalTagName();
    if (tag == L"value") {
      values.push_back(child_element->GetTextData());
    } else if (tag == L"field") {
      WalkXfdfField(doc, form, child_element, fqn, stats, depth + 1);
    }
  }
  if (!values.empty() && !fqn.IsEmpty()) {
    ApplyImportedValues(doc, form, fqn, values, stats);
  }
}

// ---------------------------------------------------------------------------
// Repair helpers.
// ---------------------------------------------------------------------------

// Climb /Parent to the field root, re-resolving every hop by object number
// so layer promotions win. Cycle-guarded.
RetainPtr<const CPDF_Dictionary> ClimbToFieldRoot(
    CPDF_Document* doc,
    RetainPtr<const CPDF_Dictionary> dict) {
  std::vector<const CPDF_Dictionary*> visited = {dict.Get()};
  for (int i = 0; i < 32; ++i) {
    RetainPtr<const CPDF_Dictionary> parent =
        dict->GetDictFor(pdfium::form_fields::kParent);
    if (parent && parent->GetObjNum() != 0) {
      parent = ToDictionary(doc->GetOrParseIndirectObject(parent->GetObjNum()));
    }
    if (!parent || pdfium::Contains(visited, parent.Get())) {
      break;
    }
    visited.push_back(parent.Get());
    dict = std::move(parent);
  }
  return dict;
}

// Resolve /AcroForm for mutation, handling all three storage shapes:
// missing (optionally bootstrap one), an indirect reference (promote the
// target), or a direct dictionary inside the catalog (promote the root and
// mutate the embedded clone). Never mutates through a reference held by a
// frozen base object.
RetainPtr<CPDF_Dictionary> GetMutableAcroForm(CPDF_Document* doc,
                                              bool create_if_missing,
                                              bool* out_created) {
  const CPDF_Dictionary* root = doc->GetRoot();
  if (!root) {
    return nullptr;
  }
  RetainPtr<const CPDF_Object> entry = root->GetObjectFor("AcroForm");
  if (!entry) {
    if (!create_if_missing) {
      return nullptr;
    }
    RetainPtr<CPDF_Dictionary> acro_form =
        CPDF_InteractiveForm::InitAcroFormDict(doc);
    if (acro_form && out_created) {
      *out_created = true;
    }
    return acro_form;
  }
  if (const CPDF_Reference* ref = entry->AsReference()) {
    return ToDictionary(doc->GetMutableIndirectObject(ref->GetRefObjNum()));
  }
  RetainPtr<CPDF_Dictionary> mutable_root = doc->GetMutableRoot();
  return mutable_root ? mutable_root->GetMutableDictFor("AcroForm") : nullptr;
}

// Resolve an array member of an already-mutable dictionary, following (and
// promoting) an indirect reference when present, creating the array when
// absent.
RetainPtr<CPDF_Array> GetMutableArrayMember(CPDF_Document* doc,
                                            CPDF_Dictionary* dict,
                                            const ByteString& key) {
  RetainPtr<const CPDF_Object> entry = dict->GetObjectFor(key.AsStringView());
  if (!entry) {
    return dict->SetNewFor<CPDF_Array>(key);
  }
  if (const CPDF_Reference* ref = entry->AsReference()) {
    return ToArray(doc->GetMutableIndirectObject(ref->GetRefObjNum()));
  }
  return dict->GetMutableArrayFor(key.AsStringView());
}

// Membership of a raw array: indirect references by object number, direct
// dictionaries by pointer identity.
bool ArrayReferencesDict(const CPDF_Array* array,
                         uint32_t objnum,
                         const CPDF_Dictionary* dict) {
  if (!array) {
    return false;
  }
  for (size_t i = 0; i < array->size(); ++i) {
    RetainPtr<const CPDF_Object> element = array->GetObjectAt(i);
    if (!element) {
      continue;
    }
    if (const CPDF_Reference* ref = element->AsReference()) {
      if (objnum != 0 && ref->GetRefObjNum() == objnum) {
        return true;
      }
    } else if (element.Get() == static_cast<const CPDF_Object*>(dict)) {
      return true;
    }
  }
  return false;
}

}  // namespace

FPDF_EXPORT EPDF_FORM_MODEL FPDF_CALLCONV
EPDFForm_LoadModel(FPDF_DOCUMENT document) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return nullptr;
  }

  auto model = std::make_unique<FormModel>();

  const CPDF_Dictionary* root = doc->GetRoot();
  RetainPtr<const CPDF_Dictionary> acro_form =
      root ? root->GetDictFor("AcroForm") : nullptr;
  if (acro_form) {
    model->kind =
        acro_form->KeyExist("XFA") ? EPDF_FORMKIND_XFA : EPDF_FORMKIND_ACROFORM;
    model->need_appearances =
        acro_form->GetBooleanFor("NeedAppearances", false);
  }

  // Phase 1: the declared field tree.
  auto form = std::make_unique<CPDF_InteractiveForm>(doc);
  const std::set<const CPDF_Dictionary*> initial_fields =
      CollectFieldDicts(*form);

  // Phase 2: reconcile widgets only reachable through page /Annots.
  const std::map<const CPDF_Dictionary*, uint32_t> widget_pages =
      SweepPageWidgets(doc, form.get());

  // Phase 3: detach into a plain snapshot.
  const size_t field_count = CountFormFields(*form);
  model->fields.reserve(field_count);
  std::map<const CPDF_Dictionary*, int> field_index_by_dict;
  for (size_t i = 0; i < field_count; ++i) {
    CPDF_FormField* field = form->GetField(i, WideString());
    if (!field) {
      continue;
    }
    FieldRecord record =
        SnapshotField(doc, field, initial_fields, widget_pages);
    const int index = fxcrt::CollectionSize<int>(model->fields);
    field_index_by_dict.try_emplace(field->GetFieldDict().Get(), index);
    if (record.objnum != 0) {
      model->field_index_by_objnum.try_emplace(record.objnum, index);
    }
    for (const WidgetRecord& widget : record.widgets) {
      if (widget.objnum != 0) {
        model->field_index_by_widget_objnum.try_emplace(widget.objnum, index);
      }
    }
    model->fields.push_back(std::move(record));
  }

  const int calculation_count = form->CountFieldsInCalculationOrder();
  model->calculation_order.reserve(calculation_count);
  for (int i = 0; i < calculation_count; ++i) {
    CPDF_FormField* field = form->GetFieldInCalculationOrder(i);
    const auto it = field
                        ? field_index_by_dict.find(field->GetFieldDict().Get())
                        : field_index_by_dict.end();
    model->calculation_order.push_back(
        it != field_index_by_dict.end() ? it->second : -1);
  }

  return HandleFromFormModel(model.release());
}

FPDF_EXPORT void FPDF_CALLCONV EPDFForm_CloseModel(EPDF_FORM_MODEL model) {
  delete FormModelFromHandle(model);
}

FPDF_EXPORT int FPDF_CALLCONV EPDFForm_GetFormKind(EPDF_FORM_MODEL model) {
  FormModel* form = FormModelFromHandle(model);
  return form ? form->kind : EPDF_FORMKIND_NONE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_GetNeedAppearances(EPDF_FORM_MODEL model) {
  FormModel* form = FormModelFromHandle(model);
  return form && form->need_appearances;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFForm_CountFields(EPDF_FORM_MODEL model) {
  FormModel* form = FormModelFromHandle(model);
  return form ? fxcrt::CollectionSize<int>(form->fields) : 0;
}

FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFForm_GetFieldActionModel(EPDF_FORM_MODEL model,
                             int field_index,
                             int event) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  if (!field || event < EPDF_FORM_ACTION_KEYSTROKE ||
      event > EPDF_FORM_ACTION_CALCULATE) {
    return nullptr;
  }
  return epdf::MakeActionModelHandle(
      field->actions[static_cast<size_t>(event)]);
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFForm_CountCalculationOrder(EPDF_FORM_MODEL model) {
  FormModel* form = FormModelFromHandle(model);
  return form ? fxcrt::CollectionSize<int>(form->calculation_order) : 0;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFForm_GetCalculationOrderFieldIndex(EPDF_FORM_MODEL model, int order_index) {
  FormModel* form = FormModelFromHandle(model);
  if (!form || order_index < 0 ||
      order_index >= fxcrt::CollectionSize<int>(form->calculation_order)) {
    return -1;
  }
  return form->calculation_order[order_index];
}

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFForm_GetFieldObjNum(EPDF_FORM_MODEL model, int field_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  return field ? field->objnum : 0;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFForm_GetFieldFamily(EPDF_FORM_MODEL model,
                                                      int field_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  return field ? field->family : EPDF_FORMFIELD_FAMILY_UNKNOWN;
}

FPDF_EXPORT uint32_t FPDF_CALLCONV EPDFForm_GetFieldFlags(EPDF_FORM_MODEL model,
                                                          int field_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  return field ? field->flags : 0;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFForm_GetFieldOrigin(EPDF_FORM_MODEL model,
                                                      int field_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  return field ? field->origin : -1;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldName(EPDF_FORM_MODEL model,
                      int field_index,
                      FPDF_WCHAR* buffer,
                      unsigned long buflen) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  if (!field) {
    return 0;
  }
  return Utf16EncodeMaybeCopyAndReturnLength(
      field->fqn, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldAlternateName(EPDF_FORM_MODEL model,
                               int field_index,
                               FPDF_WCHAR* buffer,
                               unsigned long buflen) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  if (!field) {
    return 0;
  }
  return Utf16EncodeMaybeCopyAndReturnLength(
      field->alternate_name,
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldMappingName(EPDF_FORM_MODEL model,
                             int field_index,
                             FPDF_WCHAR* buffer,
                             unsigned long buflen) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  if (!field) {
    return 0;
  }
  return Utf16EncodeMaybeCopyAndReturnLength(
      field->mapping_name, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT int FPDF_CALLCONV EPDFForm_GetFieldValueKind(EPDF_FORM_MODEL model,
                                                         int field_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  return field ? field->value.kind : EPDF_FORM_VALUE_NONE;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFForm_CountFieldValues(EPDF_FORM_MODEL model,
                                                        int field_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  return field ? fxcrt::CollectionSize<int>(field->value.values) : 0;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldValueAt(EPDF_FORM_MODEL model,
                         int field_index,
                         int value_index,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  if (!field || value_index < 0 ||
      value_index >= fxcrt::CollectionSize<int>(field->value.values)) {
    return 0;
  }
  return Utf16EncodeMaybeCopyAndReturnLength(
      field->value.values[value_index],
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFForm_GetFieldDefaultValueKind(EPDF_FORM_MODEL model, int field_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  return field ? field->default_value.kind : EPDF_FORM_VALUE_NONE;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFForm_CountFieldDefaultValues(EPDF_FORM_MODEL model, int field_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  return field ? fxcrt::CollectionSize<int>(field->default_value.values) : 0;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldDefaultValueAt(EPDF_FORM_MODEL model,
                                int field_index,
                                int value_index,
                                FPDF_WCHAR* buffer,
                                unsigned long buflen) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  if (!field || value_index < 0 ||
      value_index >= fxcrt::CollectionSize<int>(field->default_value.values)) {
    return 0;
  }
  return Utf16EncodeMaybeCopyAndReturnLength(
      field->default_value.values[value_index],
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT int FPDF_CALLCONV EPDFForm_GetFieldMaxLen(EPDF_FORM_MODEL model,
                                                      int field_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  return field ? field->max_len : 0;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFForm_CountFieldOptions(EPDF_FORM_MODEL model,
                                                         int field_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  return field ? fxcrt::CollectionSize<int>(field->options) : 0;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldOptionLabel(EPDF_FORM_MODEL model,
                             int field_index,
                             int option_index,
                             FPDF_WCHAR* buffer,
                             unsigned long buflen) {
  const OptionRecord* option =
      GetOptionRecord(model, field_index, option_index);
  if (!option) {
    return 0;
  }
  return Utf16EncodeMaybeCopyAndReturnLength(
      option->label, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldOptionValue(EPDF_FORM_MODEL model,
                             int field_index,
                             int option_index,
                             FPDF_WCHAR* buffer,
                             unsigned long buflen) {
  const OptionRecord* option =
      GetOptionRecord(model, field_index, option_index);
  if (!option) {
    return 0;
  }
  return Utf16EncodeMaybeCopyAndReturnLength(
      option->value, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_IsFieldOptionSelected(EPDF_FORM_MODEL model,
                               int field_index,
                               int option_index) {
  const OptionRecord* option =
      GetOptionRecord(model, field_index, option_index);
  return option && option->selected;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFForm_CountFieldWidgets(EPDF_FORM_MODEL model,
                                                         int field_index) {
  const FieldRecord* field = GetFieldRecord(model, field_index);
  return field ? fxcrt::CollectionSize<int>(field->widgets) : 0;
}

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFForm_GetFieldWidgetObjNum(EPDF_FORM_MODEL model,
                              int field_index,
                              int widget_index) {
  const WidgetRecord* widget =
      GetWidgetRecord(model, field_index, widget_index);
  return widget ? widget->objnum : 0;
}

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFForm_GetFieldWidgetPageObjNum(EPDF_FORM_MODEL model,
                                  int field_index,
                                  int widget_index) {
  const WidgetRecord* widget =
      GetWidgetRecord(model, field_index, widget_index);
  return widget ? widget->page_objnum : 0;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldWidgetOnState(EPDF_FORM_MODEL model,
                               int field_index,
                               int widget_index,
                               void* buffer,
                               unsigned long buflen) {
  const WidgetRecord* widget =
      GetWidgetRecord(model, field_index, widget_index);
  if (!widget) {
    return 0;
  }
  return NulTerminateMaybeCopyAndReturnLength(
      widget->on_state, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldWidgetExportValue(EPDF_FORM_MODEL model,
                                   int field_index,
                                   int widget_index,
                                   FPDF_WCHAR* buffer,
                                   unsigned long buflen) {
  const WidgetRecord* widget =
      GetWidgetRecord(model, field_index, widget_index);
  if (!widget) {
    return 0;
  }
  return Utf16EncodeMaybeCopyAndReturnLength(
      widget->export_value,
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_IsFieldWidgetChecked(EPDF_FORM_MODEL model,
                              int field_index,
                              int widget_index) {
  const WidgetRecord* widget =
      GetWidgetRecord(model, field_index, widget_index);
  return widget && widget->checked;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFForm_GetFieldIndexByObjNum(EPDF_FORM_MODEL model, uint32_t field_objnum) {
  FormModel* form = FormModelFromHandle(model);
  if (!form || field_objnum == 0) {
    return -1;
  }
  const auto it = form->field_index_by_objnum.find(field_objnum);
  return it != form->field_index_by_objnum.end() ? it->second : -1;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFForm_GetFieldIndexForWidget(EPDF_FORM_MODEL model, uint32_t widget_objnum) {
  FormModel* form = FormModelFromHandle(model);
  if (!form || widget_objnum == 0) {
    return -1;
  }
  const auto it = form->field_index_by_widget_objnum.find(widget_objnum);
  return it != form->field_index_by_widget_objnum.end() ? it->second : -1;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetToggle(FPDF_DOCUMENT document,
                   uint32_t field_objnum,
                   FPDF_BYTESTRING on_state,
                   uint32_t* changed_widget_objnums,
                   unsigned long buffer_size,
                   unsigned long* out_changed_count) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return false;
  }
  return ApplyToggle(doc, /*reconciled=*/nullptr, field_objnum,
                     ByteString(on_state ? on_state : ""),
                     /*lenient_unknown_state=*/false, changed_widget_objnums,
                     buffer_size, out_changed_count);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetTextValue(FPDF_DOCUMENT document,
                      uint32_t field_objnum,
                      FPDF_WIDESTRING value,
                      uint32_t* changed_widget_objnums,
                      unsigned long buffer_size,
                      unsigned long* out_changed_count) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return false;
  }
  return ApplyTextValue(
      doc, /*reconciled=*/nullptr, field_objnum,
      value ? WideStringFromFPDFWideString(value) : WideString(),
      changed_widget_objnums, buffer_size, out_changed_count);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetChoiceValues(FPDF_DOCUMENT document,
                         uint32_t field_objnum,
                         const FPDF_WIDESTRING* values,
                         unsigned long value_count,
                         uint32_t* changed_widget_objnums,
                         unsigned long buffer_size,
                         unsigned long* out_changed_count) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || (value_count > 0 && !values)) {
    return false;
  }
  std::vector<WideString> new_values;
  if (value_count > 0) {
    pdfium::span<const FPDF_WIDESTRING> values_span =
        UNSAFE_BUFFERS(pdfium::span(values, static_cast<size_t>(value_count)));
    for (FPDF_WIDESTRING wide_value : values_span) {
      new_values.push_back(wide_value ? WideStringFromFPDFWideString(wide_value)
                                      : WideString());
    }
  }
  return ApplyChoiceValues(doc, /*reconciled=*/nullptr, field_objnum,
                           new_values, changed_widget_objnums, buffer_size,
                           out_changed_count);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_ResetField(FPDF_DOCUMENT document,
                    uint32_t field_objnum,
                    uint32_t* changed_widget_objnums,
                    unsigned long buffer_size,
                    unsigned long* out_changed_count) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field) {
    return false;
  }
  const ByteString field_type = InheritedFieldType(field.Get());
  const uint32_t flags = InheritedFieldFlags(field.Get());
  RetainPtr<const CPDF_Object> default_value =
      CPDF_FormField::GetFieldAttrForDict(field.Get(),
                                          pdfium::form_fields::kDV);

  if (field_type == pdfium::form_fields::kBtn) {
    if (flags & pdfium::form_flags::kButtonPushbutton) {
      return false;
    }
    return ApplyToggleDefault(doc, field_objnum, default_value.Get(),
                              changed_widget_objnums, buffer_size,
                              out_changed_count);
  }

  if (field_type != pdfium::form_fields::kTx &&
      field_type != pdfium::form_fields::kCh) {
    return false;  // Push buttons handled above; signatures are never reset.
  }

  if (field_type == pdfium::form_fields::kCh) {
    std::vector<WideString> defaults;
    if (default_value && !default_value->IsNull()) {
      if (default_value->IsString()) {
        WideString value = default_value->GetUnicodeText();
        // An empty choice default represents no selected option unless an
        // option actually uses the empty export value (normalization below
        // will retain it in that case).
        defaults.push_back(std::move(value));
      } else if (const CPDF_Array* array = default_value->AsArray()) {
        defaults.reserve(array->size());
        for (size_t i = 0; i < array->size(); ++i) {
          RetainPtr<const CPDF_Object> element = array->GetDirectObjectAt(i);
          if (!element || !element->IsString()) {
            return false;
          }
          defaults.push_back(element->GetUnicodeText());
        }
      } else {
        return false;
      }
    }
    if (defaults.size() == 1 && defaults[0].IsEmpty()) {
      std::optional<NormalizedChoiceValues> normalized =
          NormalizeChoiceValues(field.Get(), flags, defaults);
      if (!normalized.has_value()) {
        defaults.clear();
      }
    }
    return ApplyChoiceValues(doc, /*reconciled=*/nullptr, field_objnum,
                             defaults, changed_widget_objnums, buffer_size,
                             out_changed_count);
  }

  if (default_value && !default_value->IsNull() && !default_value->IsString()) {
    return false;
  }
  std::vector<TxnControl> controls;
  if (!CollectTxnControls(doc, field.Get(), field_objnum,
                          /*want_toggle_info=*/false, /*reconciled=*/nullptr,
                          &controls)) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> promoted_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!promoted_field) {
    return false;
  }
  if (default_value && !default_value->IsNull()) {
    promoted_field->SetNewFor<CPDF_String>(
        pdfium::form_fields::kV,
        default_value->GetUnicodeText().AsStringView());
  } else {
    if (HasInheritedFieldAttribute(doc, field.Get(), pdfium::form_fields::kV)) {
      promoted_field->SetNewFor<CPDF_String>(pdfium::form_fields::kV,
                                             WideStringView());
    } else {
      promoted_field->RemoveFor(pdfium::form_fields::kV);
    }
  }
  promoted_field->RemoveFor("RV");
  MirrorFieldValueToTwinControls(doc, promoted_field, controls);
  return RegenerateControlAppearances(
      doc, controls, promoted_field, CPDF_GenerateAP::kTextField,
      changed_widget_objnums, buffer_size, out_changed_count);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldDisplay(FPDF_DOCUMENT document,
                         uint32_t field_objnum,
                         int display,
                         uint32_t* changed_widget_objnums,
                         unsigned long buffer_size,
                         unsigned long* out_changed_count) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  return doc &&
         ApplyFieldDisplay(doc, field_objnum, display, changed_widget_objnums,
                           buffer_size, out_changed_count);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldAppearanceText(FPDF_DOCUMENT document,
                                uint32_t field_objnum,
                                FPDF_WIDESTRING appearance_text,
                                uint32_t* changed_widget_objnums,
                                unsigned long buffer_size,
                                unsigned long* out_changed_count) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || !appearance_text) {
    return false;
  }
  return ApplyFieldAppearanceText(
      doc, field_objnum, WideStringFromFPDFWideString(appearance_text),
      changed_widget_objnums, buffer_size, out_changed_count);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_ExportFDF(FPDF_DOCUMENT document,
                   FPDF_WIDESTRING pdf_path,
                   uint32_t export_flags,
                   void* buffer,
                   unsigned long buflen) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return 0;
  }
  std::unique_ptr<CPDF_InteractiveForm> form = BuildReconciledForm(doc);
  const WideString path =
      pdf_path ? WideStringFromFPDFWideString(pdf_path) : WideString();
  std::unique_ptr<CFDF_Document> fdf = form->ExportToFDF(
      path, !!(export_flags & EPDF_FORM_EXPORT_SKIP_EMPTY_REQUIRED));
  if (!fdf) {
    return 0;
  }
  return CopyPayloadToBuffer(fdf->WriteToString(), buffer, buflen);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_ExportXFDF(FPDF_DOCUMENT document,
                    FPDF_WIDESTRING pdf_path,
                    uint32_t export_flags,
                    void* buffer,
                    unsigned long buflen) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return 0;
  }
  std::unique_ptr<CPDF_InteractiveForm> form = BuildReconciledForm(doc);
  const WideString path =
      pdf_path ? WideStringFromFPDFWideString(pdf_path) : WideString();
  const ByteString payload =
      BuildXfdf(form.get(), path,
                !!(export_flags & EPDF_FORM_EXPORT_SKIP_EMPTY_REQUIRED));
  return CopyPayloadToBuffer(payload, buffer, buflen);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_ImportFDF(FPDF_DOCUMENT document,
                   const void* data,
                   unsigned long size,
                   EPDF_FORM_IMPORT_RESULT* out_result) {
  if (out_result) {
    *out_result = {};
  }
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || !data || size == 0) {
    return false;
  }
  pdfium::span<const uint8_t> payload = UNSAFE_BUFFERS(pdfium::span(
      static_cast<const uint8_t*>(data), static_cast<size_t>(size)));
  std::unique_ptr<CFDF_Document> fdf = CFDF_Document::ParseMemory(payload);
  if (!fdf || !fdf->GetRoot()) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> main_dict =
      fdf->GetRoot()->GetDictFor("FDF");
  if (!main_dict) {
    return false;
  }

  std::unique_ptr<CPDF_InteractiveForm> form = BuildReconciledForm(doc);
  ImportStats stats;
  RetainPtr<const CPDF_Array> fields = main_dict->GetArrayFor("Fields");
  if (fields) {
    WalkFdfFields(doc, form.get(), fields.Get(), WideString(), &stats,
                  /*depth=*/0);
  }
  WriteImportResult(stats, out_result);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_ImportXFDF(FPDF_DOCUMENT document,
                    const void* data,
                    unsigned long size,
                    EPDF_FORM_IMPORT_RESULT* out_result) {
  if (out_result) {
    *out_result = {};
  }
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || !data || size == 0) {
    return false;
  }
  pdfium::span<const uint8_t> payload = UNSAFE_BUFFERS(pdfium::span(
      static_cast<const uint8_t*>(data), static_cast<size_t>(size)));
  auto stream = pdfium::MakeRetain<CFX_ReadOnlySpanStream>(payload);
  CFX_XMLParser parser(stream);
  std::unique_ptr<CFX_XMLDocument> xml = parser.Parse();
  if (!xml || !xml->GetRoot()) {
    return false;
  }
  CFX_XMLElement* xfdf = xml->GetRoot()->GetLocalTagName() == L"xfdf"
                             ? xml->GetRoot()
                             : FindXmlChildByTag(xml->GetRoot(), L"xfdf");
  if (!xfdf) {
    return false;
  }

  std::unique_ptr<CPDF_InteractiveForm> form = BuildReconciledForm(doc);
  ImportStats stats;
  CFX_XMLElement* fields = FindXmlChildByTag(xfdf, L"fields");
  if (fields) {
    for (CFX_XMLNode* child = fields->GetFirstChild(); child;
         child = child->GetNextSibling()) {
      CFX_XMLElement* field_element = ToXMLElement(child);
      if (field_element && field_element->GetLocalTagName() == L"field") {
        WalkXfdfField(doc, form.get(), field_element, WideString(), &stats,
                      /*depth=*/0);
      }
    }
  }
  WriteImportResult(stats, out_result);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_Repair(FPDF_DOCUMENT document,
                uint32_t repair_flags,
                EPDF_FORM_REPAIR_REPORT* out_report) {
  if (out_report) {
    *out_report = {};
  }
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || !doc->GetRoot()) {
    return false;
  }
  EPDF_FORM_REPAIR_REPORT report = {};

  std::unique_ptr<CPDF_InteractiveForm> form = BuildReconciledForm(doc);
  const size_t field_count = form->CountFields(WideString());
  const bool bake = repair_flags & EPDF_FORM_REPAIR_BAKE_APPEARANCES;
  const bool bake_all = bake && form->NeedConstructAP();

  // ---- Plan (const reads only; a no-op repair must promote nothing). ----
  RetainPtr<const CPDF_Dictionary> const_acro_form =
      doc->GetRoot()->GetDictFor("AcroForm");
  RetainPtr<const CPDF_Array> const_fields =
      const_acro_form ? const_acro_form->GetArrayFor("Fields") : nullptr;

  std::vector<uint32_t> roots_to_link;
  std::set<uint32_t> seen_roots;
  struct KidFix {
    uint32_t field_objnum;
    uint32_t widget_objnum;
  };
  std::vector<KidFix> kid_fixes;
  struct BakeStep {
    uint32_t field_objnum;
    int family;
  };
  std::vector<BakeStep> bake_fields;

  for (size_t i = 0; i < field_count; ++i) {
    CPDF_FormField* field = form->GetField(i, WideString());
    if (!field) {
      continue;
    }
    RetainPtr<const CPDF_Dictionary> field_dict = field->GetFieldDict();

    // Recovered roots -> /AcroForm /Fields.
    RetainPtr<const CPDF_Dictionary> root = ClimbToFieldRoot(doc, field_dict);
    const uint32_t root_objnum = root->GetObjNum();
    if (!ArrayReferencesDict(const_fields.Get(), root_objnum, root.Get())) {
      if (root_objnum == 0) {
        ++report.fields_unrepairable;
      } else if (seen_roots.insert(root_objnum).second) {
        roots_to_link.push_back(root_objnum);
      }
    }

    // Stray widgets -> parent /Kids (only when /Parent already points at
    // this field, so the fix is purely additive).
    const uint32_t field_objnum = field_dict->GetObjNum();
    RetainPtr<const CPDF_Array> kids =
        field_dict->GetArrayFor(pdfium::form_fields::kKids);
    if (kids && field_objnum != 0) {
      for (const auto& control : form->GetControlsForField(field)) {
        RetainPtr<const CPDF_Dictionary> widget = control->GetWidgetDict();
        if (widget.Get() == field_dict.Get() || widget->GetObjNum() == 0) {
          continue;
        }
        if (ArrayReferencesDict(kids.Get(), widget->GetObjNum(),
                                widget.Get())) {
          continue;
        }
        RetainPtr<const CPDF_Dictionary> parent =
            widget->GetDictFor(pdfium::form_fields::kParent);
        if (parent && parent->GetObjNum() == field_objnum) {
          kid_fixes.push_back({field_objnum, widget->GetObjNum()});
        }
      }
    }

    if (bake && field_objnum != 0) {
      const int family = FamilyFromFieldType(field->GetType());
      if (family != EPDF_FORMFIELD_FAMILY_PUSHBUTTON &&
          family != EPDF_FORMFIELD_FAMILY_SIGNATURE &&
          family != EPDF_FORMFIELD_FAMILY_UNKNOWN) {
        bake_fields.push_back({field_objnum, family});
      }
    }
  }

  // ---- Apply. ----
  if (!roots_to_link.empty()) {
    bool created = false;
    RetainPtr<CPDF_Dictionary> acro_form =
        GetMutableAcroForm(doc, /*create_if_missing=*/true, &created);
    if (!acro_form) {
      return false;
    }
    report.acroform_created = created ? 1 : 0;
    RetainPtr<CPDF_Array> fields_array =
        GetMutableArrayMember(doc, acro_form.Get(), "Fields");
    if (!fields_array) {
      return false;
    }
    for (uint32_t objnum : roots_to_link) {
      fields_array->AppendNew<CPDF_Reference>(doc, objnum);
      ++report.fields_linked;
    }
  }

  for (const KidFix& fix : kid_fixes) {
    RetainPtr<CPDF_Dictionary> field_dict =
        ToDictionary(doc->GetMutableIndirectObject(fix.field_objnum));
    if (!field_dict) {
      continue;
    }
    RetainPtr<CPDF_Array> kids = GetMutableArrayMember(
        doc, field_dict.Get(), pdfium::form_fields::kKids);
    if (!kids) {
      continue;
    }
    kids->AppendNew<CPDF_Reference>(doc, fix.widget_objnum);
    ++report.widgets_linked;
  }

  // The structural phase above may have linked fields and widgets; bake
  // against a FRESH reconciled view so just-linked widgets participate.
  std::unique_ptr<CPDF_InteractiveForm> bake_form;
  if (!bake_fields.empty()) {
    bake_form = BuildReconciledForm(doc);
  }
  for (const BakeStep& step : bake_fields) {
    RetainPtr<const CPDF_Dictionary> field_dict =
        ResolveFieldDict(doc, step.field_objnum);
    if (!field_dict) {
      continue;
    }
    std::vector<TxnControl> controls;
    if (!CollectTxnControls(doc, field_dict.Get(), step.field_objnum,
                            /*want_toggle_info=*/false, bake_form.get(),
                            &controls)) {
      continue;
    }
    RetainPtr<CPDF_Dictionary> promoted_field;
    for (const TxnControl& control : controls) {
      RetainPtr<const CPDF_Dictionary> ap = control.dict->GetDictFor("AP");
      const bool has_normal_ap = ap && ap->GetObjectFor("N");
      if (has_normal_ap && !bake_all) {
        continue;
      }
      if (!promoted_field && (control.merged || control.objnum == 0)) {
        promoted_field =
            ToDictionary(doc->GetMutableIndirectObject(step.field_objnum));
        if (!promoted_field) {
          break;
        }
      }
      RetainPtr<CPDF_Dictionary> widget =
          MutableControlDict(doc, control, promoted_field);
      if (!widget) {
        continue;
      }
      switch (step.family) {
        case EPDF_FORMFIELD_FAMILY_TEXT:
          CPDF_GenerateAP::GenerateFormAP(doc, widget.Get(),
                                          CPDF_GenerateAP::kTextField);
          break;
        case EPDF_FORMFIELD_FAMILY_COMBOBOX:
          CPDF_GenerateAP::GenerateFormAP(doc, widget.Get(),
                                          CPDF_GenerateAP::kComboBox);
          break;
        case EPDF_FORMFIELD_FAMILY_LISTBOX:
          CPDF_GenerateAP::GenerateFormAP(doc, widget.Get(),
                                          CPDF_GenerateAP::kListBox);
          break;
        case EPDF_FORMFIELD_FAMILY_CHECKBOX:
          CPDF_GenerateAP::GenerateCheckboxFormAP(doc, widget.Get());
          break;
        case EPDF_FORMFIELD_FAMILY_RADIO:
          CPDF_GenerateAP::GenerateRadioButtonFormAP(doc, widget.Get());
          break;
        default:
          continue;
      }
      ++report.appearances_baked;
    }
  }

  if (bake_all) {
    RetainPtr<CPDF_Dictionary> acro_form =
        GetMutableAcroForm(doc, /*create_if_missing=*/false, nullptr);
    if (acro_form) {
      acro_form->RemoveFor("NeedAppearances");
      report.need_appearances_cleared = 1;
    }
  }

  if (out_report) {
    *out_report = report;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Authoring: field lifecycle and adoption.
// ---------------------------------------------------------------------------

namespace {

// Family-defining /Ff bits are immutable through EPDFForm_SetFieldFlags.
constexpr uint32_t kFamilyDefiningFlags =
    pdfium::form_flags::kButtonRadio | pdfium::form_flags::kButtonPushbutton |
    pdfium::form_flags::kChoiceCombo;

// Widget-plane keys that move to the new kid when a legacy merged field is
// split by EPDFForm_AttachWidget. Field-plane keys (/FT /T /Ff /V /DV /Opt
// /MaxLen /TU /TM /DA /Q /AA) stay on the field dictionary.
constexpr const char* kWidgetPlaneKeys[] = {
    "Type", "Subtype", "Rect", "AP", "AS", "MK", "BS", "Border",
    "F",    "P",       "H",    "OC", "CA", "NM", "M",  "StructParent",
};

struct AuthorFamily {
  ByteString field_type;
  uint32_t flags;
  bool toggle;
};

bool AuthorFamilyFromCode(int family, AuthorFamily* out) {
  switch (family) {
    case 4 /* EPDF_FORMFIELD_FAMILY_TEXT */:
      *out = {pdfium::form_fields::kTx, 0, false};
      return true;
    case 2 /* CHECKBOX */:
      *out = {pdfium::form_fields::kBtn, 0, true};
      return true;
    case 3 /* RADIO */:
      *out = {pdfium::form_fields::kBtn, pdfium::form_flags::kButtonRadio,
              true};
      return true;
    case 5 /* COMBOBOX */:
      *out = {pdfium::form_fields::kCh, pdfium::form_flags::kChoiceCombo,
              false};
      return true;
    case 6 /* LISTBOX */:
      *out = {pdfium::form_fields::kCh, 0, false};
      return true;
    case 7 /* SIGNATURE */:
      *out = {pdfium::form_fields::kSig, 0, false};
      return true;
    default:
      return false;
  }
}

int FamilyOfFieldDict(const CPDF_Dictionary* field_dict) {
  const ByteString field_type = InheritedFieldType(field_dict);
  const uint32_t flags = InheritedFieldFlags(field_dict);
  if (field_type == pdfium::form_fields::kBtn) {
    if (flags & pdfium::form_flags::kButtonPushbutton) {
      return 1;
    }
    if (flags & pdfium::form_flags::kButtonRadio) {
      return 3;
    }
    return 2;
  }
  if (field_type == pdfium::form_fields::kTx) {
    return 4;
  }
  if (field_type == pdfium::form_fields::kCh) {
    return (flags & pdfium::form_flags::kChoiceCombo) ? 5 : 6;
  }
  if (field_type == pdfium::form_fields::kSig) {
    return 7;
  }
  return 0;
}

std::vector<WideString> SplitFqnSegments(const WideString& full_name) {
  std::vector<WideString> segments;
  size_t start = 0;
  while (start <= full_name.GetLength()) {
    std::optional<size_t> dot = full_name.Find(L'.', start);
    const size_t end = dot.value_or(full_name.GetLength());
    if (end == start) {
      return {};  // empty segment -> invalid
    }
    segments.push_back(full_name.Substr(start, end - start));
    if (!dot.has_value()) {
      break;
    }
    start = dot.value() + 1;
  }
  return segments;
}

// Find a direct child (of /Fields or a /Kids array) whose own /T equals
// |segment|, resolving every entry through the document.
RetainPtr<const CPDF_Dictionary> FindChildFieldByName(
    CPDF_Document* doc,
    const CPDF_Array* entries,
    const WideString& segment) {
  if (!entries) {
    return nullptr;
  }
  for (size_t i = 0; i < entries->size(); ++i) {
    RetainPtr<const CPDF_Object> element = entries->GetObjectAt(i);
    if (!element) {
      continue;
    }
    RetainPtr<const CPDF_Dictionary> child;
    if (const CPDF_Reference* ref = element->AsReference()) {
      child = ToDictionary(doc->GetOrParseIndirectObject(ref->GetRefObjNum()));
    } else {
      child = ToDictionary(std::move(element));
    }
    if (child && child->GetUnicodeTextFor(pdfium::form_fields::kT) == segment) {
      return child;
    }
  }
  return nullptr;
}

bool RemoveObjNumFromMutableArray(CPDF_Array* array, uint32_t objnum) {
  if (!array) {
    return false;
  }
  for (size_t i = 0; i < array->size(); ++i) {
    RetainPtr<const CPDF_Object> element = array->GetObjectAt(i);
    const CPDF_Reference* ref = element ? element->AsReference() : nullptr;
    if (ref && ref->GetRefObjNum() == objnum) {
      array->RemoveAt(i);
      return true;
    }
  }
  return false;
}

// Locate the page whose /Annots references |annot_objnum|. Page-tree walk
// only; returns the page's object number or 0.
uint32_t FindPageContainingAnnot(CPDF_Document* doc, uint32_t annot_objnum) {
  const int page_count = doc->GetPageCount();
  for (int i = 0; i < page_count; ++i) {
    RetainPtr<const CPDF_Dictionary> page = doc->GetPageDictionary(i);
    if (!page) {
      continue;
    }
    RetainPtr<const CPDF_Array> annots = page->GetArrayFor("Annots");
    if (!annots) {
      continue;
    }
    for (size_t j = 0; j < annots->size(); ++j) {
      RetainPtr<const CPDF_Object> element = annots->GetObjectAt(j);
      const CPDF_Reference* ref = element ? element->AsReference() : nullptr;
      if (ref && ref->GetRefObjNum() == annot_objnum) {
        return page->GetObjNum();
      }
    }
  }
  return 0;
}

// After toggle AP generation, make sure the /AP /N "on" state carries the
// requested name so EPDFForm_SetToggle can address it.
void NormalizeToggleOnState(CPDF_Dictionary* widget,
                            const ByteString& on_state) {
  RetainPtr<CPDF_Dictionary> ap = widget->GetMutableDictFor("AP");
  if (!ap) {
    return;
  }
  RetainPtr<CPDF_Dictionary> normal = ap->GetMutableDictFor("N");
  if (!normal) {
    return;
  }
  ByteString current_on;
  {
    CPDF_DictionaryLocker locker(normal);
    for (const auto& it : locker) {
      if (it.first != kOffState) {
        current_on = it.first;
        break;
      }
    }
  }
  if (current_on.IsEmpty() || current_on == on_state) {
    return;
  }
  RetainPtr<CPDF_Object> stream =
      normal->GetMutableObjectFor(current_on.AsStringView());
  if (!stream) {
    return;
  }
  normal->SetFor(on_state, stream->Clone());
  normal->RemoveFor(current_on.AsStringView());
}

// Bake the family-correct appearance for an attached widget.
void BakeWidgetAppearance(CPDF_Document* doc,
                          CPDF_Dictionary* widget,
                          int family,
                          const ByteString& on_state) {
  switch (family) {
    case 2:  // checkbox
      CPDF_GenerateAP::GenerateCheckboxFormAP(doc, widget);
      NormalizeToggleOnState(widget, on_state);
      break;
    case 3:  // radio
      CPDF_GenerateAP::GenerateRadioButtonFormAP(doc, widget);
      NormalizeToggleOnState(widget, on_state);
      break;
    case 4:
      CPDF_GenerateAP::GenerateFormAP(doc, widget, CPDF_GenerateAP::kTextField);
      break;
    case 5:
      CPDF_GenerateAP::GenerateFormAP(doc, widget, CPDF_GenerateAP::kComboBox);
      break;
    case 6:
      CPDF_GenerateAP::GenerateFormAP(doc, widget, CPDF_GenerateAP::kListBox);
      break;
    default:
      break;
  }
}

}  // namespace

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFForm_CreateField(FPDF_DOCUMENT document,
                     int family,
                     FPDF_WIDESTRING full_name) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  AuthorFamily author;
  if (!doc || !doc->GetRoot() || !AuthorFamilyFromCode(family, &author)) {
    return 0;
  }
  const WideString name =
      full_name ? WideStringFromFPDFWideString(full_name) : WideString();
  const std::vector<WideString> segments = SplitFqnSegments(name);
  if (segments.empty()) {
    return 0;
  }

  // ---- Plan (const reads only): walk existing nodes, find conflicts. ----
  // existing_path[i] holds the object number of the node matching
  // segments[i], for the leading run of segments that already exist.
  std::vector<uint32_t> existing_path;
  {
    RetainPtr<const CPDF_Dictionary> acro_form =
        doc->GetRoot()->GetDictFor("AcroForm");
    RetainPtr<const CPDF_Array> entries =
        acro_form ? acro_form->GetArrayFor("Fields") : nullptr;
    const CPDF_Array* level = entries.Get();
    RetainPtr<const CPDF_Array> keep_alive = entries;
    for (size_t i = 0; i < segments.size(); ++i) {
      RetainPtr<const CPDF_Dictionary> found =
          FindChildFieldByName(doc, level, segments[i]);
      if (!found) {
        break;
      }
      if (i + 1 == segments.size()) {
        return 0;  // sibling name collision at the terminal level
      }
      if (found->KeyExist(pdfium::form_fields::kFT)) {
        return 0;  // cannot nest under a terminal field
      }
      if (found->GetObjNum() == 0) {
        return 0;  // direct-object intermediate: not authorable
      }
      existing_path.push_back(found->GetObjNum());
      keep_alive = found->GetArrayFor(pdfium::form_fields::kKids);
      level = keep_alive.Get();
    }
  }

  // ---- Apply. ----
  bool created = false;
  RetainPtr<CPDF_Dictionary> acro_form =
      GetMutableAcroForm(doc, /*create_if_missing=*/true, &created);
  if (!acro_form) {
    return 0;
  }

  RetainPtr<CPDF_Array> parent_array =
      GetMutableArrayMember(doc, acro_form.Get(), "Fields");
  RetainPtr<CPDF_Dictionary> parent_field;  // null at the root level
  for (uint32_t objnum : existing_path) {
    parent_field = ToDictionary(doc->GetMutableIndirectObject(objnum));
    if (!parent_field) {
      return 0;
    }
    parent_array = GetMutableArrayMember(doc, parent_field.Get(),
                                         pdfium::form_fields::kKids);
  }
  if (!parent_array) {
    return 0;
  }

  for (size_t i = existing_path.size(); i < segments.size(); ++i) {
    auto node = doc->NewIndirect<CPDF_Dictionary>();
    node->SetNewFor<CPDF_String>(pdfium::form_fields::kT,
                                 segments[i].AsStringView());
    if (parent_field) {
      node->SetNewFor<CPDF_Reference>(pdfium::form_fields::kParent, doc,
                                      parent_field->GetObjNum());
    }
    const bool terminal = i + 1 == segments.size();
    if (terminal) {
      node->SetNewFor<CPDF_Name>(pdfium::form_fields::kFT, author.field_type);
      if (author.flags != 0) {
        node->SetNewFor<CPDF_Number>(pdfium::form_fields::kFf,
                                     static_cast<int>(author.flags));
      }
    }
    parent_array->AppendNew<CPDF_Reference>(doc, node->GetObjNum());
    if (terminal) {
      if (author.field_type == pdfium::form_fields::kSig) {
        // ISO 32000-2 table 224: SignaturesExist once a signature field exists.
        acro_form->SetNewFor<CPDF_Number>(
            "SigFlags", acro_form->GetIntegerFor("SigFlags") | 1);
      }
      return node->GetObjNum();
    }
    parent_field = node;
    parent_array = GetMutableArrayMember(doc, parent_field.Get(),
                                         pdfium::form_fields::kKids);
    if (!parent_array) {
      return 0;
    }
  }
  return 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_AttachWidget(FPDF_DOCUMENT document,
                      uint32_t field_objnum,
                      uint32_t widget_objnum,
                      FPDF_BYTESTRING on_state) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || field_objnum == 0 || widget_objnum == 0 ||
      field_objnum == widget_objnum) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field || InheritedFieldType(field.Get()).IsEmpty()) {
    return false;  // must address the terminal field dictionary itself
  }
  const int family = FamilyOfFieldDict(field.Get());
  if (family == 0 || family == 1) {
    return false;  // unknown / pushbutton are not authorable
  }
  const bool toggle = family == 2 || family == 3;
  const ByteString state(on_state ? on_state : "");
  if (toggle && (state.IsEmpty() || state == kOffState)) {
    return false;  // toggles need a real on-state name
  }

  RetainPtr<const CPDF_Dictionary> widget =
      ToDictionary(doc->GetOrParseIndirectObject(widget_objnum));
  if (!widget || widget->GetNameFor("Subtype") != "Widget" ||
      widget->KeyExist(pdfium::form_fields::kParent) ||
      widget->KeyExist(pdfium::form_fields::kFT)) {
    return false;  // must be an unattached, non-merged widget annotation
  }

  // ---- Apply. ----
  RetainPtr<CPDF_Dictionary> mutable_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!mutable_field) {
    return false;
  }

  // Legacy merged field: split it first. The field keeps its object number;
  // the previously merged widget half moves into a new kid annotation.
  if (mutable_field->GetNameFor("Subtype") == "Widget") {
    const uint32_t page_objnum = FindPageContainingAnnot(doc, field_objnum);
    auto split_widget = doc->NewIndirect<CPDF_Dictionary>();
    for (const char* key : kWidgetPlaneKeys) {
      RetainPtr<CPDF_Object> value = mutable_field->GetMutableObjectFor(key);
      if (!value) {
        continue;
      }
      split_widget->SetFor(key, value->Clone());
      mutable_field->RemoveFor(key);
    }
    split_widget->SetNewFor<CPDF_Name>("Type", "Annot");
    split_widget->SetNewFor<CPDF_Name>("Subtype", "Widget");
    split_widget->SetNewFor<CPDF_Reference>(pdfium::form_fields::kParent, doc,
                                            field_objnum);
    RetainPtr<CPDF_Array> kids = GetMutableArrayMember(
        doc, mutable_field.Get(), pdfium::form_fields::kKids);
    if (!kids) {
      return false;
    }
    kids->AppendNew<CPDF_Reference>(doc, split_widget->GetObjNum());
    if (page_objnum != 0) {
      RetainPtr<CPDF_Dictionary> page =
          ToDictionary(doc->GetMutableIndirectObject(page_objnum));
      RetainPtr<CPDF_Array> annots =
          page ? page->GetMutableArrayFor("Annots") : nullptr;
      if (annots && RemoveObjNumFromMutableArray(annots.Get(), field_objnum)) {
        annots->AppendNew<CPDF_Reference>(doc, split_widget->GetObjNum());
      }
    }
  }

  RetainPtr<CPDF_Dictionary> mutable_widget =
      ToDictionary(doc->GetMutableIndirectObject(widget_objnum));
  if (!mutable_widget) {
    return false;
  }
  mutable_widget->SetNewFor<CPDF_Reference>(pdfium::form_fields::kParent, doc,
                                            field_objnum);
  // A widget without /F has no Print flag: viewers show it and printers
  // drop it. A freshly authored widget prints, as Acrobat's do (/F 4).
  if (!mutable_widget->KeyExist("F")) {
    mutable_widget->SetNewFor<CPDF_Number>(
        "F", static_cast<int>(pdfium::annotation_flags::kPrint));
  }
  RetainPtr<CPDF_Array> kids = GetMutableArrayMember(
      doc, mutable_field.Get(), pdfium::form_fields::kKids);
  if (!kids) {
    return false;
  }
  kids->AppendNew<CPDF_Reference>(doc, widget_objnum);

  if (toggle) {
    mutable_widget->SetNewFor<CPDF_Name>("AS", kOffState);
    BakeWidgetAppearance(doc, mutable_widget.Get(), family, state);
    // The generator may key the "on" stream off a default; make sure the
    // requested state name is addressable even when generation bailed.
    NormalizeToggleOnState(mutable_widget.Get(), state);
  } else {
    BakeWidgetAppearance(doc, mutable_widget.Get(), family, ByteString());
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_DetachWidget(FPDF_DOCUMENT document,
                      uint32_t field_objnum,
                      uint32_t widget_objnum) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || field_objnum == 0 || widget_objnum == 0) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  RetainPtr<const CPDF_Dictionary> widget =
      ToDictionary(doc->GetOrParseIndirectObject(widget_objnum));
  if (!field || !widget) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> parent =
      widget->GetDictFor(pdfium::form_fields::kParent);
  if (!parent || parent->GetObjNum() != field_objnum) {
    return false;
  }
  RetainPtr<const CPDF_Array> kids =
      field->GetArrayFor(pdfium::form_fields::kKids);
  if (!ArrayReferencesDict(kids.Get(), widget_objnum, widget.Get())) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> mutable_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  RetainPtr<CPDF_Dictionary> mutable_widget =
      ToDictionary(doc->GetMutableIndirectObject(widget_objnum));
  if (!mutable_field || !mutable_widget) {
    return false;
  }
  RetainPtr<CPDF_Array> mutable_kids = GetMutableArrayMember(
      doc, mutable_field.Get(), pdfium::form_fields::kKids);
  if (!mutable_kids ||
      !RemoveObjNumFromMutableArray(mutable_kids.Get(), widget_objnum)) {
    return false;
  }
  // An empty /Kids array would make CPDF_InteractiveForm skip the field
  // entirely; drop the key so the field stays visible as "unplaced".
  if (mutable_kids->IsEmpty()) {
    mutable_field->RemoveFor(pdfium::form_fields::kKids);
  }
  mutable_widget->RemoveFor(pdfium::form_fields::kParent);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_DeleteField(FPDF_DOCUMENT document,
                     uint32_t field_objnum,
                     uint32_t* out_detached_widgets,
                     unsigned long buffer_size,
                     unsigned long* out_detached_count) {
  if (out_detached_count) {
    *out_detached_count = 0;
  }
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || field_objnum == 0) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field || InheritedFieldType(field.Get()).IsEmpty()) {
    return false;
  }

  // Collect widget kids (a terminal field's kids are widgets; a kid with
  // /T is a child FIELD, which makes this node non-terminal -> fail).
  std::vector<uint32_t> widget_objnums;
  RetainPtr<const CPDF_Array> kids =
      field->GetArrayFor(pdfium::form_fields::kKids);
  if (kids) {
    for (size_t i = 0; i < kids->size(); ++i) {
      RetainPtr<const CPDF_Object> element = kids->GetObjectAt(i);
      const CPDF_Reference* ref = element ? element->AsReference() : nullptr;
      if (!ref) {
        return false;  // direct kid: not authorable
      }
      RetainPtr<const CPDF_Dictionary> kid =
          ToDictionary(doc->GetOrParseIndirectObject(ref->GetRefObjNum()));
      if (!kid) {
        continue;
      }
      if (kid->KeyExist(pdfium::form_fields::kT)) {
        return false;  // non-terminal field
      }
      widget_objnums.push_back(ref->GetRefObjNum());
    }
  }

  // ---- Apply: detach widgets, unlink the field, prune empty ancestors. ----
  for (uint32_t objnum : widget_objnums) {
    RetainPtr<CPDF_Dictionary> widget =
        ToDictionary(doc->GetMutableIndirectObject(objnum));
    if (widget) {
      widget->RemoveFor(pdfium::form_fields::kParent);
    }
  }

  // Walk up: remove |current| from its container; prune empty non-terminal
  // ancestors (never the /AcroForm itself).
  uint32_t current = field_objnum;
  for (int depth = 0; depth < 32; ++depth) {
    RetainPtr<const CPDF_Dictionary> node =
        ToDictionary(doc->GetOrParseIndirectObject(current));
    if (!node) {
      break;
    }
    RetainPtr<const CPDF_Dictionary> parent =
        node->GetDictFor(pdfium::form_fields::kParent);
    if (parent && parent->GetObjNum() != 0) {
      RetainPtr<CPDF_Dictionary> mutable_parent =
          ToDictionary(doc->GetMutableIndirectObject(parent->GetObjNum()));
      RetainPtr<CPDF_Array> parent_kids = GetMutableArrayMember(
          doc, mutable_parent.Get(), pdfium::form_fields::kKids);
      if (!parent_kids ||
          !RemoveObjNumFromMutableArray(parent_kids.Get(), current)) {
        break;
      }
      if (!parent_kids->IsEmpty() ||
          mutable_parent->KeyExist(pdfium::form_fields::kFT)) {
        break;  // parent still has children, or is itself a real field
      }
      mutable_parent->RemoveFor(pdfium::form_fields::kKids);
      current = parent->GetObjNum();  // parent is now empty: prune it too
      continue;
    }
    // Root level: remove from /AcroForm /Fields.
    RetainPtr<CPDF_Dictionary> acro_form =
        GetMutableAcroForm(doc, /*create_if_missing=*/false, nullptr);
    if (acro_form) {
      RetainPtr<CPDF_Array> fields =
          GetMutableArrayMember(doc, acro_form.Get(), "Fields");
      if (fields) {
        RemoveObjNumFromMutableArray(fields.Get(), current);
      }
    }
    break;
  }

  ReportChangedWidgets(widget_objnums,
                       static_cast<unsigned long>(widget_objnums.size()),
                       out_detached_widgets, buffer_size, out_detached_count);
  return true;
}

// ---------------------------------------------------------------------------
// Authoring: field-plane property setters.
// ---------------------------------------------------------------------------

namespace {

// The array holding this field: the parent field's /Kids, or /AcroForm
// /Fields at the root. Const view for sibling checks.
RetainPtr<const CPDF_Array> SiblingArrayOf(CPDF_Document* doc,
                                           const CPDF_Dictionary* field) {
  RetainPtr<const CPDF_Dictionary> parent =
      field->GetDictFor(pdfium::form_fields::kParent);
  if (parent) {
    if (parent->GetObjNum() != 0) {
      parent = ToDictionary(doc->GetOrParseIndirectObject(parent->GetObjNum()));
    }
    return parent ? parent->GetArrayFor(pdfium::form_fields::kKids) : nullptr;
  }
  const CPDF_Dictionary* root = doc->GetRoot();
  RetainPtr<const CPDF_Dictionary> acro_form =
      root ? root->GetDictFor("AcroForm") : nullptr;
  return acro_form ? acro_form->GetArrayFor("Fields") : nullptr;
}

// Regenerate appearances after a field-plane change that affects rendering
// (options, flags, MaxLen). No-op for families without generated text APs.
void RegenerateFieldAppearances(CPDF_Document* doc, uint32_t field_objnum) {
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field) {
    return;
  }
  const int family = FamilyOfFieldDict(field.Get());
  if (family != 4 && family != 5 && family != 6) {
    return;
  }
  std::vector<TxnControl> controls;
  if (!CollectTxnControls(doc, field.Get(), field_objnum,
                          /*want_toggle_info=*/false, /*reconciled=*/nullptr,
                          &controls)) {
    return;
  }
  RetainPtr<CPDF_Dictionary> promoted_field;
  for (const TxnControl& control : controls) {
    if (!promoted_field && (control.merged || control.objnum == 0)) {
      promoted_field =
          ToDictionary(doc->GetMutableIndirectObject(field_objnum));
      if (!promoted_field) {
        return;
      }
    }
    RetainPtr<CPDF_Dictionary> widget =
        MutableControlDict(doc, control, promoted_field);
    if (widget) {
      BakeWidgetAppearance(doc, widget.Get(), family, ByteString());
    }
  }
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldName(FPDF_DOCUMENT document,
                      uint32_t field_objnum,
                      FPDF_WIDESTRING partial_name) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || field_objnum == 0) {
    return false;
  }
  const WideString name =
      partial_name ? WideStringFromFPDFWideString(partial_name) : WideString();
  if (name.IsEmpty() || name.Find(L'.', 0).has_value()) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field || !field->KeyExist(pdfium::form_fields::kT)) {
    return false;
  }

  RetainPtr<const CPDF_Array> siblings = SiblingArrayOf(doc, field.Get());
  if (siblings) {
    for (size_t i = 0; i < siblings->size(); ++i) {
      RetainPtr<const CPDF_Object> element = siblings->GetObjectAt(i);
      if (!element) {
        continue;
      }
      RetainPtr<const CPDF_Dictionary> sibling;
      if (const CPDF_Reference* ref = element->AsReference()) {
        if (ref->GetRefObjNum() == field_objnum) {
          continue;
        }
        sibling =
            ToDictionary(doc->GetOrParseIndirectObject(ref->GetRefObjNum()));
      } else {
        sibling = ToDictionary(std::move(element));
        if (sibling.Get() == field.Get()) {
          continue;
        }
      }
      if (sibling &&
          sibling->GetUnicodeTextFor(pdfium::form_fields::kT) == name) {
        return false;  // sibling name collision
      }
    }
  }

  RetainPtr<CPDF_Dictionary> mutable_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!mutable_field) {
    return false;
  }
  mutable_field->SetNewFor<CPDF_String>(pdfium::form_fields::kT,
                                        name.AsStringView());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldFlags(FPDF_DOCUMENT document,
                       uint32_t field_objnum,
                       uint32_t set_bits,
                       uint32_t clear_bits) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || field_objnum == 0) {
    return false;
  }
  if ((set_bits | clear_bits) & kFamilyDefiningFlags) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field || InheritedFieldType(field.Get()).IsEmpty()) {
    return false;
  }

  const uint32_t current = InheritedFieldFlags(field.Get());
  const uint32_t next = (current & ~clear_bits) | set_bits;
  if (next == current) {
    return true;
  }
  if (InheritedFieldType(field.Get()) == pdfium::form_fields::kCh &&
      ((current ^ next) & (pdfium::form_flags::kChoiceEdit |
                           pdfium::form_flags::kChoiceMultiSelect))) {
    const bool is_combo = next & pdfium::form_flags::kChoiceCombo;
    const bool is_edit = next & pdfium::form_flags::kChoiceEdit;
    const bool is_multi = next & pdfium::form_flags::kChoiceMultiSelect;
    if ((is_combo && is_multi) || (!is_combo && is_edit)) {
      return false;
    }
    RetainPtr<const CPDF_Array> options =
        ToArray(CPDF_FormField::GetFieldAttrForDict(field.Get(), "Opt"));
    auto value_is_compatible = [&](ByteStringView key) {
      RetainPtr<const CPDF_Object> value =
          CPDF_FormField::GetFieldAttrForDict(field.Get(), key);
      if (!value || value->IsNull()) {
        return true;
      }
      if (value->IsArray()) {
        return is_multi && !is_combo;
      }
      if (!value->IsString()) {
        return false;
      }
      const WideString text = value->GetUnicodeText();
      if (text.IsEmpty() || !is_combo || is_edit) {
        return true;
      }
      if (!options) {
        return false;
      }
      for (size_t i = 0; i < options->size(); ++i) {
        if (OptExportAt(options.Get(), i) == text) {
          return true;
        }
      }
      return false;
    };
    if (!value_is_compatible(pdfium::form_fields::kV) ||
        !value_is_compatible(pdfium::form_fields::kDV)) {
      return false;
    }
  }

  RetainPtr<CPDF_Dictionary> mutable_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!mutable_field) {
    return false;
  }
  mutable_field->SetNewFor<CPDF_Number>(pdfium::form_fields::kFf,
                                        static_cast<int>(next));
  // Rendering-relevant text/choice bits (multiline, comb, ...) changed.
  RegenerateFieldAppearances(doc, field_objnum);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldMaxLen(FPDF_DOCUMENT document,
                        uint32_t field_objnum,
                        int max_len) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || field_objnum == 0 || max_len < 0) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field || InheritedFieldType(field.Get()) != pdfium::form_fields::kTx) {
    return false;
  }
  if (max_len > 0) {
    RetainPtr<const CPDF_Object> value = CPDF_FormField::GetFieldAttrForDict(
        field.Get(), pdfium::form_fields::kV);
    if (value &&
        value->GetUnicodeText().GetLength() > static_cast<size_t>(max_len)) {
      return false;  // never truncate an existing value implicitly
    }
  }
  RetainPtr<const CPDF_Object> current_max_len =
      CPDF_FormField::GetFieldAttrForDict(field.Get(), "MaxLen");
  if ((!current_max_len && max_len == 0) ||
      (current_max_len && current_max_len->IsNumber() &&
       current_max_len->GetInteger() == max_len)) {
    return true;
  }
  RetainPtr<CPDF_Dictionary> mutable_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!mutable_field) {
    return false;
  }
  // Keep a local zero so clearing an inherited limit has an effective result
  // without mutating the ancestor (and therefore its sibling fields).
  mutable_field->SetNewFor<CPDF_Number>("MaxLen", max_len);
  RegenerateFieldAppearances(doc, field_objnum);  // comb cells follow MaxLen
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldDefaultValues(FPDF_DOCUMENT document,
                               uint32_t field_objnum,
                               const FPDF_WIDESTRING* values,
                               unsigned long value_count) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || field_objnum == 0 || value_count == 0 || !values) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field) {
    return false;
  }
  const ByteString field_type = InheritedFieldType(field.Get());
  if (field_type != pdfium::form_fields::kTx &&
      field_type != pdfium::form_fields::kCh) {
    return false;
  }
  std::vector<WideString> defaults;
  pdfium::span<const FPDF_WIDESTRING> values_span =
      UNSAFE_BUFFERS(pdfium::span(values, static_cast<size_t>(value_count)));
  defaults.reserve(values_span.size());
  for (FPDF_WIDESTRING value : values_span) {
    defaults.push_back(value ? WideStringFromFPDFWideString(value)
                             : WideString());
  }

  std::optional<NormalizedChoiceValues> normalized;
  if (field_type == pdfium::form_fields::kTx) {
    if (defaults.size() != 1) {
      return false;
    }
    RetainPtr<const CPDF_Object> current = CPDF_FormField::GetFieldAttrForDict(
        field.Get(), pdfium::form_fields::kDV);
    if (current && current->IsString() &&
        current->GetUnicodeText() == defaults[0]) {
      return true;
    }
  } else {
    normalized = NormalizeChoiceValues(
        field.Get(), InheritedFieldFlags(field.Get()), defaults);
    if (!normalized.has_value()) {
      return false;
    }
  }

  RetainPtr<CPDF_Dictionary> mutable_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!mutable_field) {
    return false;
  }
  if (field_type == pdfium::form_fields::kTx) {
    mutable_field->SetNewFor<CPDF_String>(pdfium::form_fields::kDV,
                                          defaults[0].AsStringView());
  } else {
    WriteChoiceDefaultValues(mutable_field.Get(), defaults, normalized.value());
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldDefaultToggle(FPDF_DOCUMENT document,
                               uint32_t field_objnum,
                               FPDF_BYTESTRING on_state) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || field_objnum == 0 || !on_state || on_state[0] == '\0') {
    return false;
  }
  ToggleContext ctx;
  if (!PrepareToggle(doc, /*reconciled=*/nullptr, field_objnum, &ctx)) {
    return false;
  }

  const ByteString requested(on_state);
  ByteString stored_default;
  if (requested == kOffState) {
    stored_default = kOffState;
  } else {
    RetainPtr<const CPDF_Array> opt_array =
        ToArray(CPDF_FormField::GetFieldAttrForDict(ctx.field.Get(), "Opt"));
    for (size_t i = 0; i < ctx.controls.size(); ++i) {
      if (ctx.controls[i].on_state == requested) {
        stored_default =
            opt_array ? ByteString::FormatInteger(pdfium::checked_cast<int>(i))
                      : requested;
        break;
      }
    }
    if (stored_default.IsEmpty()) {
      return false;
    }
  }

  RetainPtr<const CPDF_Object> current = CPDF_FormField::GetFieldAttrForDict(
      ctx.field.Get(), pdfium::form_fields::kDV);
  if (current && current->IsName() && current->GetString() == stored_default) {
    return true;
  }
  RetainPtr<CPDF_Dictionary> mutable_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!mutable_field) {
    return false;
  }
  mutable_field->SetNewFor<CPDF_Name>(pdfium::form_fields::kDV, stored_default);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_RemoveFieldDefaultValue(FPDF_DOCUMENT document,
                                 uint32_t field_objnum) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || field_objnum == 0) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field || InheritedFieldType(field.Get()).IsEmpty()) {
    return false;
  }
  if (!field->KeyExist(pdfium::form_fields::kDV)) {
    return true;
  }
  RetainPtr<CPDF_Dictionary> mutable_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!mutable_field) {
    return false;
  }
  mutable_field->RemoveFor(pdfium::form_fields::kDV);
  return true;
}

namespace {

FPDF_BOOL SetOptionalFieldText(FPDF_DOCUMENT document,
                               uint32_t field_objnum,
                               FPDF_WIDESTRING value,
                               const char* key) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || field_objnum == 0) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field || InheritedFieldType(field.Get()).IsEmpty()) {
    return false;
  }
  const WideString text =
      value ? WideStringFromFPDFWideString(value) : WideString();
  RetainPtr<const CPDF_Object> current =
      CPDF_FormField::GetFieldAttrForDict(field.Get(), key);
  if ((!current && text.IsEmpty()) ||
      (current && current->IsString() && current->GetUnicodeText() == text)) {
    return true;
  }
  RetainPtr<CPDF_Dictionary> mutable_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!mutable_field) {
    return false;
  }
  // An empty local string shadows an inherited value. Removing the key would
  // make the ancestor's value effective again and would not actually clear
  // what EPDFForm_LoadModel reports.
  mutable_field->SetNewFor<CPDF_String>(key, text.AsStringView());
  return true;
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldAlternateName(FPDF_DOCUMENT document,
                               uint32_t field_objnum,
                               FPDF_WIDESTRING value) {
  return SetOptionalFieldText(document, field_objnum, value, "TU");
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldMappingName(FPDF_DOCUMENT document,
                             uint32_t field_objnum,
                             FPDF_WIDESTRING value) {
  return SetOptionalFieldText(document, field_objnum, value, "TM");
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldOptions(FPDF_DOCUMENT document,
                         uint32_t field_objnum,
                         const FPDF_WIDESTRING* labels,
                         const FPDF_WIDESTRING* exports,
                         unsigned long count) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || field_objnum == 0 || (count > 0 && (!labels || !exports))) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> field = ResolveFieldDict(doc, field_objnum);
  if (!field || InheritedFieldType(field.Get()) != pdfium::form_fields::kCh) {
    return false;
  }
  const uint32_t flags = InheritedFieldFlags(field.Get());
  const bool free_text_combo = (flags & pdfium::form_flags::kChoiceCombo) &&
                               (flags & pdfium::form_flags::kChoiceEdit);

  std::vector<WideString> new_labels;
  std::vector<WideString> new_exports;
  if (count > 0) {
    pdfium::span<const FPDF_WIDESTRING> labels_span =
        UNSAFE_BUFFERS(pdfium::span(labels, static_cast<size_t>(count)));
    pdfium::span<const FPDF_WIDESTRING> exports_span =
        UNSAFE_BUFFERS(pdfium::span(exports, static_cast<size_t>(count)));
    for (unsigned long i = 0; i < count; ++i) {
      new_labels.push_back(labels_span[i]
                               ? WideStringFromFPDFWideString(labels_span[i])
                               : WideString());
      new_exports.push_back(exports_span[i]
                                ? WideStringFromFPDFWideString(exports_span[i])
                                : WideString());
    }
  }

  RetainPtr<const CPDF_Object> current_value =
      CPDF_FormField::GetFieldAttrForDict(field.Get(), pdfium::form_fields::kV);
  std::optional<std::vector<WideString>> selected =
      ReadChoiceValues(current_value.Get());
  RetainPtr<const CPDF_Object> current_default =
      CPDF_FormField::GetFieldAttrForDict(field.Get(),
                                          pdfium::form_fields::kDV);
  std::optional<std::vector<WideString>> defaults =
      ReadChoiceValues(current_default.Get());
  if (!selected.has_value() || !defaults.has_value()) {
    return false;
  }
  std::vector<WideString> kept =
      FilterChoiceValues(selected.value(), new_exports, free_text_combo);
  std::vector<WideString> kept_defaults =
      FilterChoiceValues(defaults.value(), new_exports, free_text_combo);
  if (kept_defaults.size() > 1 &&
      ((flags & pdfium::form_flags::kChoiceCombo) ||
       !(flags & pdfium::form_flags::kChoiceMultiSelect))) {
    return false;
  }

  // ---- Apply: rewrite /Opt, then re-sync selection + appearances. ----
  RetainPtr<CPDF_Dictionary> mutable_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!mutable_field) {
    return false;
  }
  if (count == 0) {
    // An empty local array also shadows an inherited /Opt, so count=0 has the
    // same effective meaning for hierarchical and non-hierarchical fields.
    mutable_field->SetNewFor<CPDF_Array>("Opt");
  } else {
    auto opt = mutable_field->SetNewFor<CPDF_Array>("Opt");
    for (unsigned long i = 0; i < count; ++i) {
      if (new_labels[i] == new_exports[i]) {
        opt->AppendNew<CPDF_String>(new_exports[i].AsStringView());
      } else {
        auto pair = opt->AppendNew<CPDF_Array>();
        pair->AppendNew<CPDF_String>(new_exports[i].AsStringView());
        pair->AppendNew<CPDF_String>(new_labels[i].AsStringView());
      }
    }
  }

  if (free_text_combo && !kept.empty() &&
      !pdfium::Contains(new_exports, kept.front())) {
    // Free text survives; only the index hint is stale now.
    if (HasInheritedFieldAttribute(doc, field.Get(), "I")) {
      mutable_field->SetNewFor<CPDF_Array>("I");
    } else {
      mutable_field->RemoveFor("I");
    }
    RegenerateFieldAppearances(doc, field_objnum);
  } else if (!ApplyChoiceValues(doc, /*reconciled=*/nullptr, field_objnum, kept,
                                nullptr, 0, nullptr)) {
    return false;
  }

  if (current_default && !current_default->IsNull()) {
    if (kept_defaults.empty()) {
      if (!(flags & pdfium::form_flags::kChoiceCombo) &&
          (flags & pdfium::form_flags::kChoiceMultiSelect)) {
        mutable_field->SetNewFor<CPDF_Array>(pdfium::form_fields::kDV);
      } else {
        mutable_field->SetNewFor<CPDF_String>(pdfium::form_fields::kDV,
                                              WideStringView());
      }
    } else {
      std::optional<NormalizedChoiceValues> normalized_defaults =
          NormalizeChoiceValues(mutable_field.Get(), flags, kept_defaults);
      if (!normalized_defaults.has_value()) {
        return false;
      }
      WriteChoiceDefaultValues(mutable_field.Get(), kept_defaults,
                               normalized_defaults.value());
    }
  }
  return true;
}
