// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "fpdfsdk/epdf_object_copier.h"

#include <utility>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_null.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/bytestring.h"

namespace epdf {

namespace {

bool IsPageTreeNode(const CPDF_Object* object) {
  const CPDF_Dictionary* dictionary = object->AsDictionary();
  if (!dictionary) {
    return false;
  }
  const ByteString type = dictionary->GetNameFor("Type");
  return type.EqualNoCase("Page") || type.EqualNoCase("Pages");
}

}  // namespace

ObjectCopier::ObjectCopier(CPDF_Document* dest) : dest_(dest) {}

ObjectCopier::~ObjectCopier() = default;

RetainPtr<CPDF_Object> ObjectCopier::Copy(const CPDF_Object* object) {
  if (!object) {
    return nullptr;
  }
  if (const CPDF_Reference* reference = object->AsReference()) {
    const uint32_t number = CopyReferenced(reference);
    return number ? pdfium::MakeRetain<CPDF_Reference>(dest_.get(), number)
                  : nullptr;
  }
  // An indirect object handed over directly, or a stream (which the file
  // only holds as an object of its own): referenced by its copy.
  if (object->GetObjNum() != 0 || object->IsStream()) {
    const uint32_t number = CopyIndirect(object);
    return number ? pdfium::MakeRetain<CPDF_Reference>(dest_.get(), number)
                  : nullptr;
  }
  RetainPtr<CPDF_Object> copy = object->Clone();
  if (copy) {
    Retarget(copy.Get());
  }
  return copy;
}

RetainPtr<CPDF_Stream> ObjectCopier::CopyStream(const CPDF_Stream* stream) {
  const uint32_t number = CopyIndirect(stream);
  return number ? ToStream(dest_->GetMutableIndirectObject(number)) : nullptr;
}

uint32_t ObjectCopier::CopyIndirect(const CPDF_Object* source) {
  if (!source) {
    return 0;
  }
  const uint32_t number = source->GetObjNum();
  if (number != 0 && dest_->GetIndirectObject(number).Get() == source) {
    return number;  // the destination's own object
  }
  const auto it = copied_.find(source);
  if (it != copied_.end()) {
    return it->second;
  }
  if (IsPageTreeNode(source)) {
    return 0;
  }

  RetainPtr<CPDF_Object> copy = source->Clone();
  if (!copy) {
    return 0;
  }
  const uint32_t copy_number = dest_->AddIndirectObject(copy);
  // Recorded before its references are followed, so a cycle back to it ends
  // here.
  copied_.emplace(source, copy_number);
  sources_.push_back(pdfium::WrapRetain(source));
  Retarget(copy.Get());
  return copy_number;
}

uint32_t ObjectCopier::CopyReferenced(const CPDF_Reference* reference) {
  // Resolved through the reference's own store, not by its number here.
  RetainPtr<const CPDF_Object> target = reference->GetDirect();
  return target ? CopyIndirect(target.Get()) : 0;
}

void ObjectCopier::Retarget(CPDF_Object* copy) {
  if (CPDF_Stream* stream = copy->AsMutableStream()) {
    Retarget(stream->GetMutableDict().Get());
    return;
  }

  if (CPDF_Dictionary* dictionary = copy->AsMutableDictionary()) {
    std::vector<ByteString> dropped;
    std::vector<std::pair<ByteString, uint32_t>> made_indirect;
    {
      CPDF_DictionaryLocker locker(dictionary);
      for (const auto& item : locker) {
        CPDF_Object* value = item.second.Get();
        if (CPDF_Reference* reference = value->AsMutableReference()) {
          const uint32_t number = CopyReferenced(reference);
          if (number) {
            reference->SetRef(dest_.get(), number);
          } else {
            dropped.push_back(item.first);
          }
        } else if (value->IsStream()) {
          made_indirect.emplace_back(item.first,
                                     dest_->AddIndirectObject(item.second));
          Retarget(value);
        } else {
          Retarget(value);
        }
      }
    }
    for (const ByteString& key : dropped) {
      dictionary->RemoveFor(key.AsStringView());
    }
    for (const auto& [key, number] : made_indirect) {
      dictionary->SetNewFor<CPDF_Reference>(key, dest_.get(), number);
    }
    return;
  }

  if (CPDF_Array* array = copy->AsMutableArray()) {
    for (size_t i = 0; i < array->size(); ++i) {
      RetainPtr<CPDF_Object> value = array->GetMutableObjectAt(i);
      if (CPDF_Reference* reference = value->AsMutableReference()) {
        const uint32_t number = CopyReferenced(reference);
        if (number) {
          reference->SetRef(dest_.get(), number);
        } else {
          // Kept as null, so the entries after it keep their places.
          array->SetNewAt<CPDF_Null>(i);
        }
      } else if (value->IsStream()) {
        const uint32_t number = dest_->AddIndirectObject(value);
        Retarget(value.Get());
        array->SetNewAt<CPDF_Reference>(i, dest_.get(), number);
      } else {
        Retarget(value.Get());
      }
    }
  }
}

}  // namespace epdf
