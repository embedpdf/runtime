// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef FPDFSDK_EPDF_FORM_HELPERS_H_
#define FPDFSDK_EPDF_FORM_HELPERS_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <set>

#include "core/fxcrt/retain_ptr.h"

class CPDF_Dictionary;
class CPDF_Document;
class CPDF_InteractiveForm;

namespace epdf {

// The reconciled form lens shared by the form model (epdf_form.cpp) and the
// signature model (epdf_signature.cpp): the /AcroForm tree merged by fully
// qualified name and reconciled with a page sweep, so recovered widgets
// participate and layer promotions win.

size_t CountFormFields(const CPDF_InteractiveForm& form);

// The field dictionaries currently known to |form|; used to tell recovered
// fields (found only by the page sweep) apart from declared ones.
std::set<const CPDF_Dictionary*> CollectFieldDicts(
    const CPDF_InteractiveForm& form);

// Walk every page dictionary (page-tree traversal only - no CPDF_Page, no
// content parsing), reconcile widgets the /AcroForm /Fields walk missed, and
// return which page references each widget.
std::map<const CPDF_Dictionary*, uint32_t> SweepPageWidgets(
    CPDF_Document* doc,
    CPDF_InteractiveForm* form);

// A form with the page sweep applied.
std::unique_ptr<CPDF_InteractiveForm> BuildReconciledForm(CPDF_Document* doc);

uint32_t PageObjNumForWidget(
    const std::map<const CPDF_Dictionary*, uint32_t>& widget_pages,
    const CPDF_Dictionary* widget_dict);

// GetOrParseIndirectObject parses on demand on plain documents and is the
// promoted-first lookup on layer documents, where it never promotes - safe
// for planning reads.
RetainPtr<const CPDF_Dictionary> ResolveFieldDict(CPDF_Document* doc,
                                                  uint32_t field_objnum);

}  // namespace epdf

#endif  // FPDFSDK_EPDF_FORM_HELPERS_H_
