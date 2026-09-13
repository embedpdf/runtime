// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PARSER_CPDF_OBJECT_EQUALITY_H_
#define CORE_FPDFAPI_PARSER_CPDF_OBJECT_EQUALITY_H_

class CPDF_Object;

// Whether two objects would be written as the same bytes by the creator:
// the same canonical text (dictionaries, arrays, scalars, a stream's
// dictionary) and, for streams, the same raw data. Both sides go through the
// same writer, so a file's original whitespace and number formatting never
// matter; references compare by number and are never resolved. Streams are
// compared by raw bytes, never re-encoded. This is how a save tells a
// touched object from a changed one.
bool CPDF_SameEffectiveValue(const CPDF_Object* a, const CPDF_Object* b);

#endif  // CORE_FPDFAPI_PARSER_CPDF_OBJECT_EQUALITY_H_
