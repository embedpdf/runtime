// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PARSER_CPDF_OBJECT_STREAM_CACHE_H_
#define CORE_FPDFAPI_PARSER_CPDF_OBJECT_STREAM_CACHE_H_

#include <stddef.h>
#include <stdint.h>

#include <list>
#include <map>
#include <memory>

class CPDF_ObjectStream;

// A byte-budgeted cache for temporary parsing. A stream larger than the budget
// is retained alone, avoiding repeated decompression for each contained object.
// Returned shared pointers pin active streams during recursive parsing.
class CPDF_ObjectStreamCache {
 public:
  explicit CPDF_ObjectStreamCache(size_t byte_limit);
  ~CPDF_ObjectStreamCache();

  std::shared_ptr<const CPDF_ObjectStream> Get(uint32_t object_number);
  void Put(uint32_t object_number,
           std::shared_ptr<const CPDF_ObjectStream> stream);

  size_t retained_bytes() const { return retained_bytes_; }

 private:
  struct Entry {
    std::shared_ptr<const CPDF_ObjectStream> stream;
    std::list<uint32_t>::iterator recency;
    size_t bytes;
  };

  const size_t byte_limit_;
  size_t retained_bytes_ = 0;
  std::list<uint32_t> recent_objects_;
  std::map<uint32_t, Entry> entries_;
};

#endif  // CORE_FPDFAPI_PARSER_CPDF_OBJECT_STREAM_CACHE_H_
