// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/edit/cpdf_save_trailer.h"

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_object_walker.h"

namespace {

bool ShouldCopyEntry(const ByteString& key, bool replace_info) {
  return key != "Encrypt" && key != "Size" && key != "Filter" &&
         key != "Index" && key != "Length" && key != "Prev" && key != "W" &&
         key != "XRefStm" && key != "ID" && key != "DecodeParms" &&
         key != "Type" && !(key == "Info" && replace_info);
}

}  // namespace

CPDF_SaveTrailer::CPDF_SaveTrailer(RetainPtr<const CPDF_Dictionary> input,
                                   uint32_t root_number,
                                   uint32_t current_info_number,
                                   uint32_t original_info_number,
                                   RetainPtr<const CPDF_Dictionary> encryption,
                                   uint32_t encryption_number,
                                   RetainPtr<const CPDF_Array> id)
    : root_number_(input ? 0 : root_number),
      info_number_(current_info_number != original_info_number
                       ? current_info_number
                       : 0),
      encryption_number_(encryption ? encryption_number : 0) {
  auto add_references = [&](RetainPtr<const CPDF_Object> object) {
    auto references = CPDF_CollectReferences(std::move(object));
    roots_.insert(roots_.end(), references.begin(), references.end());
  };
  if (input) {
    CPDF_DictionaryLocker locker(input);
    for (const auto& [key, value] : locker) {
      if (ShouldCopyEntry(key, current_info_number != original_info_number)) {
        copied_entries_.emplace_back(key, value);
        add_references(value);
      }
    }
  }
  for (uint32_t number : {root_number_, info_number_, encryption_number_}) {
    if (number) {
      roots_.push_back(number);
    }
  }
  // An inline encryption dictionary is emitted separately, so its references
  // must also be visited even though it is absent from the document's holder.
  if (encryption && encryption->IsInline()) {
    add_references(std::move(encryption));
  }
  add_references(std::move(id));
}

CPDF_SaveTrailer::~CPDF_SaveTrailer() = default;
