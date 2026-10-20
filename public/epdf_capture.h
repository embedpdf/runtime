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
// line "%EPDF-CAPTURE 1", a manifest dictionary naming what was captured,
// where it was and, when the layer knows its base's SHA-256, the file it was
// taken on (/Base: supplied with EPDF_SetBaseDocumentSha256() or worked out
// once, so taking a capture never hashes the file), then each object as
// "N G obj ... endobj", a stream with its bytes as stored. Following references
// stops at the catalog, the page tree and its pages, and at the keys /P,
// /Parent, /IRT and /OC.
//
// An import runs inside a layer transaction (EPDFLayer_BeginTransaction()).
// It puts an object back only where the layer holds no version of its number
// now: a version the layer holds may be newer (a shared font grows its
// subset). What it puts back in its place (an annotation into /Annots, a page
// into the page tree) is what the number holds once the objects are in, and
// an import that would place something of another kind is refused: a save can
// free a deleted object's number for another object. A capture that names
// another file, one that names a number above the layer's last one, and one
// that holds the catalog, the page tree or a page it isn't of are refused
// before anything is written. Which objects to capture, and when to put them
// back, is the caller's decision.
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

// Experimental EmbedPDF Extension API.
// The outline item |objnum| as EPDFOutline_DeleteItem() removes it: the item
// and the items under it, with every object the layer holds a version of
// that they reach (their actions and destinations, not their structure
// elements), and the item's place: its parent, and the sibling it followed.
// NULL when |objnum| is not an item in the outline (public/epdf_outline.h),
// or a list it is in holds an item that isn't an indirect object.
FPDF_EXPORT void* FPDF_CALLCONV
EPDFOutline_ExportItemRawToOwnedBuffer(FPDF_DOCUMENT document,
                                       uint32_t objnum,
                                       unsigned long* out_size);

// Experimental EmbedPDF Extension API.
// Puts the outline item of |capture| back with the items under it: their
// objects where the layer holds no version, then the item among its parent's
// children, right after the sibling it followed while that is still one of
// them, else first. Counts are kept, as every outline write keeps them.
// False outside a transaction, for a malformed capture, when its parent is no
// longer an item in the outline, when the item is in the outline already, or
// when what its number holds isn't an outline item.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFOutline_ImportItemRaw(FPDF_DOCUMENT document,
                          const void* capture,
                          unsigned long size);

// Experimental EmbedPDF Extension API.
// The page |page_objnum| as EPDFDoc_DeletePageByObjectNumber() removes it:
// the page and every object the layer holds a version of that it reaches
// (its contents, resources and annotations, with their appearances; never
// another page or the form's fields), the /Names /Pages keys that name it,
// its place (the page it followed), and what it takes from the page tree
// above it: the /Resources, /MediaBox, /CropBox and /Rotate it doesn't hold
// itself. NULL when |page_objnum| is not a page in the page tree.
FPDF_EXPORT void* FPDF_CALLCONV
EPDFPage_ExportRawToOwnedBuffer(FPDF_DOCUMENT document,
                                uint32_t page_objnum,
                                unsigned long* out_size);

// Experimental EmbedPDF Extension API.
// Puts the page of |capture| back: its objects where the layer holds no
// version, the page itself also over the null a delete leaves for a page the
// layer made; then the page into the page tree right after the page it
// followed while that is still in it, else first, holding what it took from
// the page tree as its own keys where its place now gives otherwise; then each
// of its keys that no registration holds now. False outside a transaction, for
// a malformed capture, when the page is in the page tree already, or when what
// its number holds is no page.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_ImportRaw(FPDF_DOCUMENT document,
                   const void* capture,
                   unsigned long size);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // PUBLIC_EPDF_CAPTURE_H_
