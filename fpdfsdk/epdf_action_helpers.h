// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef FPDFSDK_EPDF_ACTION_HELPERS_H_
#define FPDFSDK_EPDF_ACTION_HELPERS_H_

#include <array>
#include <memory>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "public/epdf_action.h"

class CPDF_Action;
class CPDF_Dictionary;
class CPDF_Document;

namespace epdf {

// The /AA key of each annotation event after activation (/A), in
// EPDF_ANNOT_ACTION_* order from EPDF_ANNOT_ACTION_CURSOR_ENTER.
inline constexpr std::array<const char*, 10> kAnnotEventKeys = {
    "E", "X", "D", "U", "Fo", "Bl", "PO", "PC", "PV", "PI"};

// The /AA key of each form field event, in EPDF_FORM_ACTION_* order.
inline constexpr std::array<const char*, 4> kFieldEventKeys = {"K", "F", "V",
                                                               "C"};

// The action dictionary |action| is, when it is an indirect object of |doc|
// (the one |doc| holds at its number); null for a direct action or one of
// another document.
RetainPtr<const CPDF_Dictionary> IndirectActionOf(CPDF_Document* doc,
                                                  FPDF_ACTION action);

// Whether |dict|'s |key| is a reference to object |objnum|, or, for |objnum|
// 0, absent. A null |dict| holds nothing.
bool RefersTo(const CPDF_Dictionary* dict, ByteStringView key, uint32_t objnum);

// Sets |owner|'s /AA |event_key| to a reference to object |action_objnum|, or
// removes it for 0. An indirect /AA may be shared with another dictionary, so
// it is copied in as |owner|'s own before it changes. An /AA left empty goes,
// unless |keep_empty|. |owner| must be writable.
void SetAdditionalAction(CPDF_Document* doc,
                         CPDF_Dictionary* owner,
                         ByteStringView event_key,
                         uint32_t action_objnum,
                         bool keep_empty);

// The field events (/AA) a field gets from its parents, when it has none of
// its own: null when no parent gives any.
RetainPtr<const CPDF_Dictionary> InheritedFieldEvents(
    const CPDF_Dictionary* field);

// Before a write to the /AA of |field| (writable): when it has none of its
// own but inherits one, the inherited field events (K F V C) are copied in
// as its own, so the ones the write doesn't set keep their meaning. Returns
// whether |field| inherits events: an /AA of its own left empty must then
// stay, to keep hiding them.
bool TakeInheritedFieldEvents(CPDF_Dictionary* field);

struct ActionModelData;
using ActionModelDataPtr = std::shared_ptr<const ActionModelData>;

ActionModelDataPtr BuildActionModel(const CPDF_Action& action,
                                    CPDF_Document* document = nullptr);
EPDF_ACTION_MODEL MakeActionModelHandle(ActionModelDataPtr data);

}  // namespace epdf

#endif  // FPDFSDK_EPDF_ACTION_HELPERS_H_
