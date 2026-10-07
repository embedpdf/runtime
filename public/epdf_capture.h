// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_CAPTURE_H_
#define PUBLIC_EPDF_CAPTURE_H_

#include <stdint.h>

// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Captures: what a write is about to lose or replace in a layer, kept so an
// undo can put it back.
//
// A capture holds the layer's own versions of the objects involved and of
// what they reach: what the layer made, and what it edited. It never holds
// the uploaded file's originals, which always stay. It is PDF syntax: the
// line "%EPDF-CAPTURE 1", a manifest dictionary naming what was captured and
// where it was, then each object as "N G obj ... endobj", a stream with its
// bytes as stored. Following references stops at the catalog, the page tree
// and its pages, and at the keys /P, /Parent, /IRT and /OC.
//
// An import runs inside a layer transaction (EPDFLayer_BeginTransaction()).
// It puts an object back only where the layer holds no version of its number
// now: a version the layer holds may be newer (a shared font grows its
// subset), and numbers are never reused, so it is the same object. A capture
// that names a number above the layer's last one is refused before anything
// is written. Which objects to capture, and when to put them back, is the
// caller's decision.
//
// Exports return a buffer the caller releases with EPDF_FreeBuffer(), its
// size in |out_size|, or NULL. All of these work on layer documents only, and
// none loads a page.

// Experimental EmbedPDF Extension API.
// The annotations at |indexes| (|count| of them) of page |page_index|'s
// /Annots, with their positions, and every object the layer holds a version
// of that they reach. Every member must be an indirect object: promote the
// page's inline annotations first.
FPDF_EXPORT void* FPDF_CALLCONV
EPDFPage_ExportAnnotsRawToOwnedBuffer(FPDF_DOCUMENT document,
                                      int page_index,
                                      const int* indexes,
                                      int count,
                                      unsigned long* out_size);

// Experimental EmbedPDF Extension API.
// Puts the annotations of |capture| back on page |page_index|: their objects
// where the layer holds no version, then each into /Annots at the position it
// had, in ascending order (at the end when the page has fewer now). A member
// already in /Annots stays where it is. False outside a transaction, for a
// malformed capture, or when |page_index| is not the page it was taken on.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_ImportAnnotsRaw(FPDF_DOCUMENT document,
                         int page_index,
                         const void* capture,
                         unsigned long size);

// Experimental EmbedPDF Extension API.
// The dictionary |objnum| as it reads now: every key with its value, and,
// under the keys named in |deep_keys| (separated by spaces, such as "AP FS";
// NULL for none), every object the layer holds a version of that they reach.
// NULL when |objnum| is not a dictionary.
FPDF_EXPORT void* FPDF_CALLCONV
EPDFDoc_ExportDictRawToOwnedBuffer(FPDF_DOCUMENT document,
                                   uint32_t objnum,
                                   FPDF_BYTESTRING deep_keys,
                                   unsigned long* out_size);

// Experimental EmbedPDF Extension API.
// Puts the dictionary of |capture| back as it was: the objects under its deep
// keys where the layer holds no version, then exactly the keys it held, with
// their values. A dictionary that is gone (a save dropped it once nothing
// reached it) comes back whole. False outside a transaction, for a malformed
// capture, or when the object is something other than a dictionary now.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_ImportDictRaw(FPDF_DOCUMENT document,
                      const void* capture,
                      unsigned long size);

// Experimental EmbedPDF Extension API.
// The terminal field |field_objnum| as EPDFForm_DeleteField() removes it:
// the field (with its value), its widgets with their pages and positions in
// /Annots, the ancestors the delete prunes, and the position of the field
// and of each of those ancestors in its container (its parent's /Kids, or
// /AcroForm /Fields), with every object the layer holds a version of that
// they reach. NULL for a field that isn't terminal.
FPDF_EXPORT void* FPDF_CALLCONV
EPDFForm_ExportFieldRawToOwnedBuffer(FPDF_DOCUMENT document,
                                     uint32_t field_objnum,
                                     unsigned long* out_size);

// Experimental EmbedPDF Extension API.
// Puts the field of |capture| back: its objects where the layer holds no
// version, then the pruned ancestors and the field into their containers at
// the positions they had, and each widget into its page's /Annots at its
// position, its /Parent the field. Anything already in place stays. False
// outside a transaction, for a malformed capture, or when a container or a
// widget's page is gone. Whether the field's name is free is the caller's to
// check.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_ImportFieldRaw(FPDF_DOCUMENT document,
                        const void* capture,
                        unsigned long size);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // PUBLIC_EPDF_CAPTURE_H_
