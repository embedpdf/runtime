// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "fpdfsdk/epdf_form_helpers.h"

#include <utility>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfdoc/cpdf_formcontrol.h"
#include "core/fpdfdoc/cpdf_formfield.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fxcrt/widestring.h"

namespace epdf {

size_t CountFormFields(const CPDF_InteractiveForm& form) {
  return form.CountFields(WideString());
}

// Collect the set of field dictionaries currently known to |form|. Used to
// tell recovered fields (found only by the page sweep) apart from fields
// reachable through /AcroForm /Fields.
std::set<const CPDF_Dictionary*> CollectFieldDicts(
    const CPDF_InteractiveForm& form) {
  std::set<const CPDF_Dictionary*> dicts;
  const size_t count = CountFormFields(form);
  for (size_t i = 0; i < count; ++i) {
    CPDF_FormField* field = form.GetField(i, WideString());
    if (field) {
      dicts.insert(field->GetFieldDict().Get());
    }
  }
  return dicts;
}

// Walk every page dictionary (page-tree traversal only - no CPDF_Page, no
// content parsing) and reconcile widget annotations that the /AcroForm
// /Fields walk did not reach. Also records which page references each
// widget, which the snapshot uses as the widget's placement.
std::map<const CPDF_Dictionary*, uint32_t> SweepPageWidgets(
    CPDF_Document* doc,
    CPDF_InteractiveForm* form) {
  std::map<const CPDF_Dictionary*, uint32_t> widget_pages;
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
      // Resolve each annotation by object number through the document so
      // layer promotions win over the frozen instances that references
      // held by frozen base objects would yield.
      RetainPtr<const CPDF_Object> element = annots->GetObjectAt(j);
      if (!element) {
        continue;
      }
      RetainPtr<const CPDF_Dictionary> annot;
      if (const CPDF_Reference* ref = element->AsReference()) {
        annot =
            ToDictionary(doc->GetOrParseIndirectObject(ref->GetRefObjNum()));
      } else {
        annot = ToDictionary(std::move(element));
      }
      if (!annot || annot->GetNameFor("Subtype") != "Widget") {
        continue;
      }
      widget_pages.try_emplace(annot.Get(), page->GetObjNum());
      if (!form->GetControlByDict(annot.Get())) {
        form->ReconcileWidget(annot);
      }
    }
  }
  return widget_pages;
}

// The reconciled view of the form: the /AcroForm tree merged by fully
// qualified name and reconciled with the page sweep, so recovered fields
// participate and promoted values win. This is the ONE lens both reads
// (model snapshot, interchange export) and write transactions look through;
// a write planned against the raw field dictionary alone would miss
// same-FQN twin widgets that only the reconciliation knows about.
std::unique_ptr<CPDF_InteractiveForm> BuildReconciledForm(CPDF_Document* doc) {
  auto form = std::make_unique<CPDF_InteractiveForm>(doc);
  SweepPageWidgets(doc, form.get());
  return form;
}

uint32_t PageObjNumForWidget(
    const std::map<const CPDF_Dictionary*, uint32_t>& widget_pages,
    const CPDF_Dictionary* widget_dict) {
  const auto it = widget_pages.find(widget_dict);
  if (it != widget_pages.end()) {
    return it->second;
  }
  // Fall back to the widget's /P entry for widgets that no swept page
  // references (e.g. pages outside a layer's page list).
  RetainPtr<const CPDF_Dictionary> page = widget_dict->GetDictFor("P");
  return page ? page->GetObjNum() : 0;
}

// GetOrParseIndirectObject parses on demand on plain documents (the const
// GetIndirectObject is a map-only lookup) and is the promoted-first lookup
// on layer documents, where it never promotes - safe for planning reads.
RetainPtr<const CPDF_Dictionary> ResolveFieldDict(CPDF_Document* doc,
                                                  uint32_t field_objnum) {
  if (!doc || field_objnum == 0) {
    return nullptr;
  }
  return ToDictionary(doc->GetOrParseIndirectObject(field_objnum));
}

}  // namespace epdf
