// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
#ifndef CORE_FPDFAPI_PARSER_CPDF_MEASURE_STORAGE_H_
#define CORE_FPDFAPI_PARSER_CPDF_MEASURE_STORAGE_H_

// EmbedPDF: the SDK supplies the measurement registry/handle arenas. Core only
// owns their lifetime: registry per document, borrowed handles per page/annot.
class CPDF_MeasureStorage {
 public:
  virtual ~CPDF_MeasureStorage() = default;
};
#endif  // CORE_FPDFAPI_PARSER_CPDF_MEASURE_STORAGE_H_
