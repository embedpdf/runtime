// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/parser/cpdf_write_generation.h"

#include <atomic>

#include "core/fxcrt/check_op.h"
#include "core/fxcrt/epdf_tls.h"

namespace {

std::atomic<uint32_t> g_next_write_generation{CPDF_WriteGeneration::kCommitted +
                                              1};
EPDF_TLS uint32_t g_current_write_generation = 0;

}  // namespace

// static
uint32_t CPDF_WriteGeneration::Next() {
  const uint32_t generation = g_next_write_generation++;
  CHECK_LT(generation, kFrozen);  // 4 billion transactions in one process
  return generation;
}

// static
uint32_t CPDF_WriteGeneration::Current() {
  return g_current_write_generation;
}

// static
void CPDF_WriteGeneration::SetCurrent(uint32_t generation) {
  DCHECK(generation == 0 || g_current_write_generation == 0);
  g_current_write_generation = generation;
}
