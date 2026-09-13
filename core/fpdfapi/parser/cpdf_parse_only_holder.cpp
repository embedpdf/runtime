// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/parser/cpdf_parse_only_holder.h"

#include "core/fpdfapi/parser/cpdf_object.h"

CPDF_ParseOnlyHolder::CPDF_ParseOnlyHolder() = default;

CPDF_ParseOnlyHolder::~CPDF_ParseOnlyHolder() = default;

bool CPDF_ParseOnlyHolder::TryInit() {
  return true;
}

RetainPtr<CPDF_Object> CPDF_ParseOnlyHolder::ParseIndirectObject(
    uint32_t objnum) {
  return parser_ ? parser_->ParseIndirectObject(objnum) : nullptr;
}
