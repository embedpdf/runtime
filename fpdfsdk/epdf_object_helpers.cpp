// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "fpdfsdk/epdf_object_helpers.h"

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"

namespace epdf {

RetainPtr<CPDF_Array> GetMutableArrayMember(CPDF_Document* doc,
                                            CPDF_Dictionary* dict,
                                            ByteStringView key,
                                            bool create_if_missing) {
  if (!doc || !dict) {
    return nullptr;
  }
  RetainPtr<const CPDF_Object> entry = dict->GetObjectFor(key);
  if (!entry) {
    return create_if_missing ? dict->SetNewFor<CPDF_Array>(ByteString(key))
                             : nullptr;
  }
  if (const CPDF_Reference* ref = entry->AsReference()) {
    return ToArray(doc->GetMutableIndirectObject(ref->GetRefObjNum()));
  }
  return dict->GetMutableArrayFor(key);
}

}  // namespace epdf
