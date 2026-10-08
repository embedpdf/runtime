// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include <utility>
#include <vector>

#include "constants/form_fields.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/ipdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfdoc/cpdf_formfield.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_action_helpers.h"
#include "public/epdf_action.h"

namespace epdf {

RetainPtr<const CPDF_Dictionary> IndirectActionOf(CPDF_Document* doc,
                                                  FPDF_ACTION action) {
  const CPDF_Dictionary* dict = CPDFDictionaryFromFPDFAction(action);
  if (!doc || !dict || dict->GetObjNum() == 0) {
    return nullptr;
  }
  RetainPtr<const CPDF_Object> held =
      doc->GetOrParseIndirectObject(dict->GetObjNum());
  if (held.Get() != dict) {
    return nullptr;
  }
  return pdfium::WrapRetain(dict);
}

bool RefersTo(const CPDF_Dictionary* dict,
              ByteStringView key,
              uint32_t objnum) {
  RetainPtr<const CPDF_Object> entry = dict ? dict->GetObjectFor(key) : nullptr;
  if (objnum == 0) {
    return !entry;
  }
  const CPDF_Reference* ref = entry ? entry->AsReference() : nullptr;
  return ref && ref->GetRefObjNum() == objnum;
}

RetainPtr<const CPDF_Dictionary> InheritedFieldEvents(
    const CPDF_Dictionary* field) {
  RetainPtr<const CPDF_Dictionary> parent =
      field->GetDictFor(pdfium::form_fields::kParent);
  return parent ? ToDictionary(CPDF_FormField::GetFieldAttrForDict(
                      parent.Get(), pdfium::form_fields::kAA))
                : nullptr;
}

bool TakeInheritedFieldEvents(CPDF_Dictionary* field) {
  RetainPtr<const CPDF_Dictionary> inherited = InheritedFieldEvents(field);
  if (!inherited) {
    return false;
  }
  if (!field->KeyExist(pdfium::form_fields::kAA)) {
    RetainPtr<CPDF_Dictionary> own =
        field->SetNewFor<CPDF_Dictionary>(pdfium::form_fields::kAA);
    for (const char* key : kFieldEventKeys) {
      if (RetainPtr<const CPDF_Object> entry = inherited->GetObjectFor(key)) {
        own->SetFor(key, entry->Clone());
      }
    }
  }
  return true;
}

void SetAdditionalAction(CPDF_Document* doc,
                         CPDF_Dictionary* owner,
                         ByteStringView event_key,
                         uint32_t action_objnum,
                         bool keep_empty) {
  RetainPtr<CPDF_Dictionary> events;
  RetainPtr<const CPDF_Object> entry = owner->GetObjectFor("AA");
  if (entry && entry->IsReference()) {
    // Another dictionary may share it: change a copy of our own.
    RetainPtr<const CPDF_Dictionary> shared = owner->GetDictFor("AA");
    events = shared ? ToDictionary(shared->Clone()) : nullptr;
    if (events) {
      owner->SetFor("AA", events);
    }
  } else {
    events = owner->GetMutableDictFor("AA");
  }
  if (!events) {
    if (action_objnum == 0 && !keep_empty) {
      owner->RemoveFor("AA");
      return;
    }
    events = owner->SetNewFor<CPDF_Dictionary>("AA");
  }
  if (action_objnum != 0) {
    events->SetNewFor<CPDF_Reference>(ByteString(event_key), doc,
                                      action_objnum);
  } else {
    events->RemoveFor(event_key);
  }
  if (events->size() == 0 && !keep_empty) {
    owner->RemoveFor("AA");
  }
}

}  // namespace epdf

namespace {

// The target list for |count| |targets|: names as text strings, objects as
// references. Null when a target is malformed or names no dictionary of
// |doc|.
RetainPtr<CPDF_Array> TargetList(CPDF_Document* doc,
                                 const EPDF_ACTION_TARGET* targets,
                                 int count) {
  if (count < 0 || (count > 0 && !targets)) {
    return nullptr;
  }
  auto list = pdfium::MakeRetain<CPDF_Array>();
  for (int i = 0; i < count; ++i) {
    const EPDF_ACTION_TARGET& target = UNSAFE_BUFFERS(targets[i]);
    if (target.kind == EPDF_ACTION_TARGET_NAME && target.name) {
      WideString name =
          UNSAFE_BUFFERS(WideStringFromFPDFWideString(target.name));
      if (name.IsEmpty()) {
        return nullptr;
      }
      list->AppendNew<CPDF_String>(name.AsStringView());
    } else if (target.kind == EPDF_ACTION_TARGET_OBJECT &&
               target.object_number != 0 &&
               ToDictionary(
                   doc->GetOrParseIndirectObject(target.object_number))) {
      list->AppendNew<CPDF_Reference>(doc, target.object_number);
    } else {
      return nullptr;
    }
  }
  return list;
}

// A new indirect action dictionary of |doc| with /S |type|.
RetainPtr<CPDF_Dictionary> NewAction(CPDF_Document* doc, const char* type) {
  RetainPtr<CPDF_Dictionary> action = doc->NewIndirect<CPDF_Dictionary>();
  action->SetNewFor<CPDF_Name>("S", type);
  return action;
}

}  // namespace

FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateJavaScript(FPDF_DOCUMENT document, FPDF_WIDESTRING script) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || !script) {
    return nullptr;
  }
  RetainPtr<CPDF_Dictionary> action = NewAction(doc, "JavaScript");
  action->SetNewFor<CPDF_String>(
      "JS",
      UNSAFE_BUFFERS(WideStringFromFPDFWideString(script)).AsStringView());
  return FPDFActionFromCPDFDictionary(action.Get());
}

FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateHide(FPDF_DOCUMENT document,
                      const EPDF_ACTION_TARGET* targets,
                      int count,
                      FPDF_BOOL hide) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  RetainPtr<CPDF_Array> list =
      doc && count >= 1 ? TargetList(doc, targets, count) : nullptr;
  if (!list) {
    return nullptr;
  }
  RetainPtr<CPDF_Dictionary> action = NewAction(doc, "Hide");
  // /T is the target itself for one, an array for several.
  if (list->size() == 1) {
    action->SetFor("T", list->GetObjectAt(0)->Clone());
  } else {
    action->SetFor("T", std::move(list));
  }
  if (!hide) {
    action->SetNewFor<CPDF_Boolean>("H", false);
  }
  return FPDFActionFromCPDFDictionary(action.Get());
}

FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateResetForm(FPDF_DOCUMENT document,
                           const EPDF_ACTION_TARGET* targets,
                           int count,
                           FPDF_BOOL exclude) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || count < -1) {
    return nullptr;
  }
  RetainPtr<CPDF_Array> fields;
  if (count >= 0) {
    fields = TargetList(doc, targets, count);
    if (!fields) {
      return nullptr;
    }
  }
  RetainPtr<CPDF_Dictionary> action = NewAction(doc, "ResetForm");
  if (fields) {
    action->SetFor("Fields", std::move(fields));
  }
  if (exclude) {
    action->SetNewFor<CPDF_Number>("Flags", 1);
  }
  return FPDFActionFromCPDFDictionary(action.Get());
}

FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateSubmitForm(FPDF_DOCUMENT document,
                            FPDF_WIDESTRING url,
                            const EPDF_ACTION_TARGET* targets,
                            int count,
                            unsigned int flags) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || !url || count < -1) {
    return nullptr;
  }
  const ByteString address =
      UNSAFE_BUFFERS(WideStringFromFPDFWideString(url)).ToUTF8();
  if (address.IsEmpty()) {
    return nullptr;
  }
  RetainPtr<CPDF_Array> fields;
  if (count >= 0) {
    fields = TargetList(doc, targets, count);
    if (!fields) {
      return nullptr;
    }
  }
  RetainPtr<CPDF_Dictionary> action = NewAction(doc, "SubmitForm");
  RetainPtr<CPDF_Dictionary> file = action->SetNewFor<CPDF_Dictionary>("F");
  file->SetNewFor<CPDF_Name>("FS", "URL");
  file->SetNewFor<CPDF_String>("F", address);
  if (fields) {
    action->SetFor("Fields", std::move(fields));
  }
  if (flags != 0) {
    action->SetNewFor<CPDF_Number>("Flags", static_cast<int>(flags));
  }
  return FPDFActionFromCPDFDictionary(action.Get());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAction_SetNext(FPDF_DOCUMENT document,
                                                       FPDF_ACTION action,
                                                       const FPDF_ACTION* next,
                                                       int count) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  RetainPtr<const CPDF_Dictionary> current =
      epdf::IndirectActionOf(doc, action);
  if (!current || count < 0 || (count > 0 && !next)) {
    return false;
  }
  std::vector<uint32_t> objnums;
  for (int i = 0; i < count; ++i) {
    RetainPtr<const CPDF_Dictionary> following =
        epdf::IndirectActionOf(doc, UNSAFE_BUFFERS(next[i]));
    if (!following || following == current) {
      return false;
    }
    objnums.push_back(following->GetObjNum());
  }

  // Nothing to change: no write.
  if (count == 0 && !current->KeyExist("Next")) {
    return true;
  }
  if (count == 1 && epdf::RefersTo(current.Get(), "Next", objnums[0])) {
    return true;
  }

  RetainPtr<CPDF_Dictionary> mutable_action =
      ToDictionary(doc->GetMutableIndirectObject(current->GetObjNum()));
  if (!mutable_action) {
    return false;
  }
  if (count == 0) {
    mutable_action->RemoveFor("Next");
  } else if (count == 1) {
    mutable_action->SetNewFor<CPDF_Reference>("Next", doc, objnums[0]);
  } else {
    auto list = mutable_action->SetNewFor<CPDF_Array>("Next");
    for (uint32_t objnum : objnums) {
      list->AppendNew<CPDF_Reference>(doc, objnum);
    }
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetEventAction(FPDF_ANNOTATION annot, int event, FPDF_ACTION action) {
  if (event < EPDF_ANNOT_ACTION_ACTIVATE ||
      event > EPDF_ANNOT_ACTION_PAGE_INVISIBLE) {
    return false;
  }
  ScopedFPDFAnnotationView annotation_view(annot);
  CPDF_AnnotContext* context = annotation_view.Get();
  const CPDF_Dictionary* current = context ? context->GetAnnotDict() : nullptr;
  if (!current || current->GetNameFor("Subtype") != "Widget") {
    return false;
  }
  CPDF_Document* doc = context->GetPage()->GetDocument();
  uint32_t action_objnum = 0;
  if (action) {
    RetainPtr<const CPDF_Dictionary> held = epdf::IndirectActionOf(doc, action);
    if (!held) {
      return false;
    }
    action_objnum = held->GetObjNum();
  }

  // Nothing to change: no write, so nothing is copied up for writing.
  const bool activate = event == EPDF_ANNOT_ACTION_ACTIVATE;
  const ByteStringView key =
      activate
          ? ByteStringView("A")
          : ByteStringView(
                epdf::kAnnotEventKeys[event - EPDF_ANNOT_ACTION_CURSOR_ENTER]);
  if (activate ? epdf::RefersTo(current, key, action_objnum)
               : epdf::RefersTo(current->GetDictFor("AA").Get(), key,
                                action_objnum)) {
    return true;
  }

  RetainPtr<CPDF_Dictionary> dict = context->GetMutableAnnotDict();
  if (!dict) {
    return false;
  }
  if (!activate) {
    // A widget merged with its field shares the field's /AA: the field's
    // inherited events must survive the widget's first entry.
    const bool merged = dict->KeyExist(pdfium::form_fields::kT);
    const bool inherits = merged && epdf::TakeInheritedFieldEvents(dict.Get());
    epdf::SetAdditionalAction(doc, dict.Get(), key, action_objnum,
                              /*keep_empty=*/inherits);
  } else if (action_objnum != 0) {
    dict->SetNewFor<CPDF_Reference>("A", doc, action_objnum);
  } else {
    dict->RemoveFor("A");
  }
  return true;
}
