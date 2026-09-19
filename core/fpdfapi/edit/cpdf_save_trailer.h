// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_EDIT_CPDF_SAVE_TRAILER_H_
#define CORE_FPDFAPI_EDIT_CPDF_SAVE_TRAILER_H_

#include <stdint.h>

#include <utility>
#include <vector>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"

class CPDF_Array;
class CPDF_Dictionary;
class CPDF_Object;

// The semantic trailer for one save. Traversal and serialization consume the
// same retained entries and explicit references. Physical xref fields (/Size,
// /Prev, /W, etc.) are written later, once their offsets are known.
class CPDF_SaveTrailer {
 public:
  CPDF_SaveTrailer(RetainPtr<const CPDF_Dictionary> input,
                   uint32_t root_number,
                   uint32_t current_info_number,
                   uint32_t original_info_number,
                   RetainPtr<const CPDF_Dictionary> encryption,
                   uint32_t encryption_number,
                   RetainPtr<const CPDF_Array> id);
  ~CPDF_SaveTrailer();

  const auto& copied_entries() const { return copied_entries_; }
  const std::vector<uint32_t>& roots() const { return roots_; }
  uint32_t root_number() const { return root_number_; }
  uint32_t info_number() const { return info_number_; }
  uint32_t encryption_number() const { return encryption_number_; }

 private:
  std::vector<std::pair<ByteString, RetainPtr<const CPDF_Object>>>
      copied_entries_;
  std::vector<uint32_t> roots_;
  uint32_t root_number_ = 0;
  uint32_t info_number_ = 0;
  uint32_t encryption_number_ = 0;
};

#endif  // CORE_FPDFAPI_EDIT_CPDF_SAVE_TRAILER_H_
