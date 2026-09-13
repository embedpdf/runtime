// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PARSER_CPDF_PARSE_ONLY_HOLDER_H_
#define CORE_FPDFAPI_PARSER_CPDF_PARSE_ONLY_HOLDER_H_

#include <stdint.h>

#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"

class CPDF_Object;

// The smallest object holder a CPDF_Parser can be pointed at: objects come
// from the parser's bytes and nowhere else. Used wherever bytes must be
// read as they are, without a document's cache, page tree, or render state
// standing in between - delta ingest for layers, and the immutable revision
// view behind signature analysis. The parser must be set before parsing
// starts and must be destroyed before the holder.
class CPDF_ParseOnlyHolder final : public CPDF_Parser::ParsedObjectsHolder {
 public:
  CPDF_ParseOnlyHolder();
  ~CPDF_ParseOnlyHolder() override;

  void SetParser(CPDF_Parser* parser) { parser_ = parser; }

  // CPDF_Parser::ParsedObjectsHolder:
  bool TryInit() override;

 protected:
  // CPDF_IndirectObjectHolder:
  RetainPtr<CPDF_Object> ParseIndirectObject(uint32_t objnum) override;

 private:
  UnownedPtr<CPDF_Parser> parser_;
};

#endif  // CORE_FPDFAPI_PARSER_CPDF_PARSE_ONLY_HOLDER_H_
