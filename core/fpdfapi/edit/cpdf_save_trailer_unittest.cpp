// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/edit/cpdf_save_trailer.h"

#include <set>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_indirect_object_holder.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "testing/gtest/include/gtest/gtest.h"

TEST(CPDFSaveTrailerTest, RootsMatchRetainedAndReplacementEntries) {
  CPDF_IndirectObjectHolder holder;
  auto input = pdfium::MakeRetain<CPDF_Dictionary>();
  uint32_t number = 1;
  for (const char* key :
       {"Root", "Info", "Encrypt", "Custom", "XRefStm", "Prev", "Size", "ID"}) {
    input->SetNewFor<CPDF_Reference>(key, &holder, number++);
  }
  for (bool encrypted : {false, true}) {
    auto encryption = pdfium::MakeRetain<CPDF_Dictionary>();
    encryption->SetNewFor<CPDF_Reference>("Dependency", &holder, 11);
    CPDF_SaveTrailer plan(input, 0, 9, 2, encrypted ? encryption : nullptr, 10,
                          nullptr);
    const std::set<uint32_t> roots(plan.roots().begin(), plan.roots().end());
    EXPECT_EQ(encrypted ? (std::set<uint32_t>{1, 4, 9, 10, 11})
                        : (std::set<uint32_t>{1, 4, 9}),
              roots);
    std::set<ByteString> keys;
    for (const auto& [key, value] : plan.copied_entries()) {
      keys.insert(key);
    }
    EXPECT_EQ((std::set<ByteString>{"Custom", "Root"}), keys);
  }
  CPDF_SaveTrailer keep_info(input, 0, 2, 2, nullptr, 0, nullptr);
  const std::set<uint32_t> roots(keep_info.roots().begin(),
                                 keep_info.roots().end());
  EXPECT_EQ((std::set<uint32_t>{1, 2, 4}), roots);
}
