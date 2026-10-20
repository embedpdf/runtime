// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF(fonts): the container TrueType and OpenType fonts share, an sfnt:
// a table directory, then the tables it names. The font format starts each
// table on a four-byte boundary. FreeType reads a font whose tables don't; a
// browser's font sanitizer refuses it whole.

#ifndef CORE_FXGE_CFX_SFNT_H_
#define CORE_FXGE_CFX_SFNT_H_

#include <stdint.h>

#include <map>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/span.h"

// An sfnt of |tables|, by tag: the table directory in tag order with each
// table's checksum, each table padded to four bytes, and head's
// checkSumAdjustment set for the whole font (whatever head held there).
DataVector<uint8_t> WriteSfnt(
    uint32_t version,
    const std::map<ByteString, DataVector<uint8_t>>& tables);

// Whether |bytes| is an sfnt whose tables each start on a four-byte
// boundary; false for anything else.
bool SfntTablesAligned(pdfium::span<const uint8_t> bytes);

// |bytes|' tables written again (WriteSfnt): the same tables, each on a
// four-byte boundary. Empty when |bytes| isn't an sfnt whose tables lie
// inside it.
DataVector<uint8_t> AlignSfntTables(pdfium::span<const uint8_t> bytes);

#endif  // CORE_FXGE_CFX_SFNT_H_
