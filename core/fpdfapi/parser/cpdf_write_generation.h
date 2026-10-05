// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PARSER_CPDF_WRITE_GENERATION_H_
#define CORE_FPDFAPI_PARSER_CPDF_WRITE_GENERATION_H_

#include <stdint.h>

#include "core/fxcrt/check.h"

// Write generations: which transaction may write an object.
//
// Every object a layer document stores carries the generation it was stored
// under (CPDF_Object::write_generation()):
//   0       detached: not stored anywhere yet, so anyone building it may
//           write it;
//   1       stored outside any transaction (open, ingest, a write with no
//           transaction open);
//   N       stored by the transaction with generation N: copied up, created,
//           or attached to something that was;
//   kFrozen frozen: a shared base object nobody may write, ever.
// Generations come from one counter, so a generation names one transaction
// in one layer for the life of the process; an aborted transaction's number
// is never used again.
//
// While a layer transaction is open on this thread, an object is writable
// only when it is detached or belongs to that transaction. A write to
// anything else - a committed object reached through a retained pointer
// instead of the copy-up door - is the bug DCHECK_PDF_WRITABLE catches, and
// the test owners use to decide they must open their owner first.
class CPDF_WriteGeneration {
 public:
  // The generation of objects stored outside any transaction.
  static constexpr uint32_t kCommitted = 1;
  // A frozen object's generation (CPDF_Object::Freeze()). Never handed out.
  static constexpr uint32_t kFrozen = UINT32_MAX;

  // A generation never handed out before in this process.
  static uint32_t Next();

  // The open transaction's generation on this thread, or 0 when none is.
  static uint32_t Current();

  // Opens (a nonzero generation) or closes (0) the transaction on this
  // thread. Only CPDF_LayerDocument's begin, commit and abort call this.
  static void SetCurrent(uint32_t generation);
};

#if DCHECK_IS_ON()
#define DCHECK_PDF_WRITABLE(obj) DCHECK((obj)->IsWritable())
#else
#define DCHECK_PDF_WRITABLE(obj) ((void)0)
#endif

#endif  // CORE_FPDFAPI_PARSER_CPDF_WRITE_GENERATION_H_
