// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_CHECKPOINT_H_
#define PUBLIC_EPDF_CHECKPOINT_H_

// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"

// Experimental EmbedPDF Extension API.
//
// A checkpoint makes a group of writes all or nothing. Taken before the
// first write, it can roll the document back to that moment. It is exact for
// writes that add: new objects, entries appended to a recorded page's
// /Annots, and changes to the dictionaries every page shares that appearance
// writers touch, which it records: the catalog (it gains an /AcroForm) and
// the form dictionary with its /DR and /DR /Font. It does not undo a change
// to any other object that existed when it was taken.
//
// On a layer document, an object a write promoted into the layer stays
// promoted after a rollback, holding its base value: saves and "changed
// since load" are the same as before the writes.

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Experimental EmbedPDF Extension API.
// Take a checkpoint of |document|: its last object number, and a copy of the
// catalog, the form dictionary, its /DR and its /DR /Font, each that is an
// object of its own.
//
// Returns the checkpoint, which the caller ends with EPDFDoc_EndCheckpoint(),
// or NULL on error.
FPDF_EXPORT EPDF_CHECKPOINT FPDF_CALLCONV
EPDFDoc_BeginCheckpoint(FPDF_DOCUMENT document);

// Experimental EmbedPDF Extension API.
// Record page |page_index| before the first write to it under |checkpoint|:
// a copy of its /Annots, or the fact that it has none. Recording a page
// again keeps the first record.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_CheckpointPage(EPDF_CHECKPOINT checkpoint, int page_index);

// Experimental EmbedPDF Extension API.
// Undo every write made since |checkpoint| was taken: the recorded
// dictionaries get their values back in place, each recorded page gets its
// /Annots back, every object numbered above the checkpoint's last object
// number is deleted, and those numbers are given back. The checkpoint stays
// open; end it with EPDFDoc_EndCheckpoint().
//
// Returns true when everything recorded was restored.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFDoc_Rollback(EPDF_CHECKPOINT checkpoint);

// Experimental EmbedPDF Extension API.
// Release |checkpoint|. The writes, or the rollback, stay.
FPDF_EXPORT void FPDF_CALLCONV EPDFDoc_EndCheckpoint(EPDF_CHECKPOINT checkpoint);

#ifdef __cplusplus
}
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_CHECKPOINT_H_
