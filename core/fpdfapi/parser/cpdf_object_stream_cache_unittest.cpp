// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/parser/cpdf_object_stream_cache.h"

#include <memory>
#include <utility>

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_indirect_object_holder.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object_stream.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/data_vector.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

std::shared_ptr<const CPDF_ObjectStream> MakeObjectStream(size_t padding = 0) {
  auto dict = pdfium::MakeRetain<CPDF_Dictionary>();
  dict->SetNewFor<CPDF_Name>("Type", "ObjStm");
  dict->SetNewFor<CPDF_Number>("N", 1);
  dict->SetNewFor<CPDF_Number>("First", 5);
  ByteStringView bytes("10 0 42");
  DataVector<uint8_t> data(bytes.begin(), bytes.end());
  data.resize(data.size() + padding, ' ');
  auto stream = pdfium::MakeRetain<CPDF_Stream>(std::move(data), dict);
  return CPDF_ObjectStream::Create(std::move(stream));
}

}  // namespace

TEST(CPDFObjectStreamCacheTest, EvictionPreservesPinnedStreams) {
  auto first = MakeObjectStream();
  const size_t bytes = first->GetRetainedSize();
  CPDF_ObjectStreamCache cache(bytes * 2);
  cache.Put(1, first);
  cache.Put(2, MakeObjectStream());
  first.reset();

  auto pinned = cache.Get(1);
  ASSERT_TRUE(pinned);
  cache.Put(3, MakeObjectStream());
  EXPECT_FALSE(cache.Get(2));
  EXPECT_TRUE(cache.Get(1));
  cache.Get(3);
  cache.Put(4, MakeObjectStream());
  EXPECT_FALSE(cache.Get(1));
  EXPECT_LE(cache.retained_bytes(), bytes * 2);

  CPDF_IndirectObjectHolder holder;
  auto object = pinned->ParseObject(&holder, 10, 0);
  ASSERT_TRUE(object);
  EXPECT_EQ(42, object->GetInteger());
}

TEST(CPDFObjectStreamCacheTest, OversizedStreamIsRetainedAlone) {
  auto small = MakeObjectStream();
  auto large = MakeObjectStream(1024);
  CPDF_ObjectStreamCache cache(small->GetRetainedSize() * 2);
  cache.Put(1, small);
  cache.Put(2, MakeObjectStream());
  cache.Put(3, large);
  EXPECT_FALSE(cache.Get(1));
  EXPECT_FALSE(cache.Get(2));
  EXPECT_EQ(large, cache.Get(3));
  EXPECT_EQ(large->GetRetainedSize(), cache.retained_bytes());

  cache.Put(4, small);
  EXPECT_FALSE(cache.Get(3));
  EXPECT_EQ(small->GetRetainedSize(), cache.retained_bytes());

  CPDF_IndirectObjectHolder holder;
  EXPECT_TRUE(large->ParseObject(&holder, 10, 0));
}
