// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PARSER_CPDF_WRITE_CONTEXT_H_
#define CORE_FPDFAPI_PARSER_CPDF_WRITE_CONTEXT_H_

#include <stdint.h>

// The identity policy of one save. Incremental output preserves generations;
// a full rewrite normalizes them. References use this without resolving their
// target objects or changing the live document.
class CPDF_WriteContext {
 public:
  virtual ~CPDF_WriteContext() = default;

  virtual uint32_t GetObjectGeneration(uint32_t object_number) const = 0;
};

#endif  // CORE_FPDFAPI_PARSER_CPDF_WRITE_CONTEXT_H_
