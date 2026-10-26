// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "fpdfsdk/epdf_object_helpers.h"

#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
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

bool IsAnnotationOfPage(CPDF_Page* page, CPDF_AnnotContext* annotation) {
  CPDF_Page* annotation_page = annotation && annotation->GetPage()
                                   ? annotation->GetPage()->AsPDFPage()
                                   : nullptr;
  if (!page || !annotation_page ||
      page->GetDocument() != annotation_page->GetDocument()) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> page_dictionary = page->GetDict();
  RetainPtr<const CPDF_Dictionary> annotation_page_dictionary =
      annotation_page->GetDict();
  if (!page_dictionary || !annotation_page_dictionary) {
    return false;
  }

  const uint32_t page_object_number = page_dictionary->GetObjNum();
  const uint32_t annotation_page_object_number =
      annotation_page_dictionary->GetObjNum();
  return page_object_number != 0 && annotation_page_object_number != 0
             ? page_object_number == annotation_page_object_number
             : page_dictionary.Get() == annotation_page_dictionary.Get();
}

bool IsIndirectObjectOf(const CPDF_Document* doc, const CPDF_Object* object) {
  if (!doc || !object) {
    return false;
  }
  const uint32_t objnum = object->GetObjNum();
  if (objnum == 0) {
    return false;
  }
  if (const CPDF_LayerDocument* layer = CPDF_LayerDocument::FromDocument(doc)) {
    return layer->IsVersionOf(objnum, object);
  }
  return doc->GetIndirectObject(objnum) == object;
}

}  // namespace epdf
