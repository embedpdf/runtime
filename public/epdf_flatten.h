// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_FLATTEN_H_
#define PUBLIC_EPDF_FLATTEN_H_

// NOLINTNEXTLINE(build/include)
#include "fpdf_flatten.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// EmbedPDF's additions to public/fpdf_flatten.h: flattening a page or its
// annotations, exporting annotations as a document, and copying pages as they
// draw (a print copy).

// Per-annotation outcome of EPDFPage_FlattenAnnotations().
// Painted into the page content and removed from /Annots.
#define EPDF_FLATTEN_STATUS_APPLIED 0
// Belongs to the page but was left in place: hidden for the usage, a Popup,
// or without a usable normal appearance.
#define EPDF_FLATTEN_STATUS_SKIPPED 1
// Not an annotation of |page| (or a null handle) — the call fails whole.
#define EPDF_FLATTEN_STATUS_NOT_ON_PAGE 2

// Experimental EmbedPDF Extension API.
// Flatten every eligible annotation appearance on a page. This is the
// layer-safe counterpart to FPDFPage_Flatten(): the page dictionary is
// promoted before any mutation.
//
// Only annotations whose normal appearance was successfully added to page
// content are removed. Hidden, usage-ineligible, popup and malformed
// annotations remain in /Annots, and so does one without a stored appearance
// that the renderer doesn't draw from memory either. One the renderer draws
// from memory is painted with that appearance, written into the document
// with its fonts. Flattened widgets are also
// detached from the AcroForm field tree; a separate logical field remains as
// an unplaced field.
//
//   page  - handle to the page.
//   usage - exactly one of the |FLAT_*| values.
//
// Returns FLATTEN_SUCCESS if at least one annotation was flattened,
// FLATTEN_NOTHINGTODO if the page has no eligible usable appearances, or
// FLATTEN_FAIL for invalid arguments.
FPDF_EXPORT int FPDF_CALLCONV EPDFPage_Flatten(FPDF_PAGE page, int usage);

// Experimental EmbedPDF Extension API.
// Flatten a chosen SET of annotations of a page — EPDFPage_Flatten() with a
// scope. Eligibility, appearance resolution, widget detachment, and layer
// promotion are identical: annotations that are hidden for |usage|, Popups,
// or have no usable normal appearance are left in /Annots and reported
// EPDF_FLATTEN_STATUS_SKIPPED, so a caller can say "2 of 3 flattened".
//
// All-or-nothing on validity: when any handle is null or not an annotation
// of |page|, nothing changes, that entry reports
// EPDF_FLATTEN_STATUS_NOT_ON_PAGE, and the call returns FLATTEN_FAIL.
//
// The caller must close every handle with FPDFPage_CloseAnnot() after this
// call; a flattened annotation is gone from the page, so its handle must not
// be used again before it is closed.
//
//   page        - handle to the page owning the annotations.
//   annots      - handles of the annotations to flatten.
//   annot_count - number of entries; > 0.
//   usage       - exactly one of the |FLAT_*| values.
//   statuses    - receives one EPDF_FLATTEN_STATUS_* per entry; may be NULL.
//
// Returns FLATTEN_SUCCESS if at least one annotation was flattened,
// FLATTEN_NOTHINGTODO if every entry was skipped, or FLATTEN_FAIL for invalid
// arguments (including an entry that is not on |page|).
FPDF_EXPORT int FPDF_CALLCONV
EPDFPage_FlattenAnnotations(FPDF_PAGE page,
                            FPDF_ANNOTATION* annots,
                            int annot_count,
                            int usage,
                            int* statuses);

// Experimental EmbedPDF Extension API.
// Flatten the normal appearances of a chosen set of annotations of |page|
// into a NEW single-page document — the same placement as
// EPDFPage_FlattenAnnotations() (ISO 32000-2 12.5.5 algorithm 8.1, /Matrix
// honored), aimed at a fresh page instead of the source page. The page is
// the union of the annotations' /Rect with its lower-left at the origin;
// every appearance lands exactly where the source page shows it. Forms and
// their resources are deep-cloned once; resources shared between
// annotations are deduplicated. The source document is untouched.
//
// Eligibility follows FLAT_NORMALDISPLAY. All-or-nothing: if any entry is
// null, not on |page|, hidden, a Popup, or without a usable normal
// appearance, no document is produced — a stamp silently missing one of its
// parts would be worse than an error.
//
//   page        - handle to the page owning the annotations.
//   annots      - handles of the annotations to export.
//   annot_count - number of entries; > 0.
//
// Returns a new document (the caller closes it with FPDF_CloseDocument()),
// or NULL.
FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFPage_ExportAnnotationsAsDocument(FPDF_PAGE page,
                                     FPDF_ANNOTATION* annots,
                                     int annot_count);

// Which annotations EPDFDoc_ExportPagesFlattened() paints into the pages.
// Every annotation that is not a form field: markup, links, stamps, ...
#define EPDF_EXPORT_FLATTENED_ANNOTATIONS 0x1
// Form field widgets, as filled.
#define EPDF_EXPORT_FLATTENED_FORM_FIELDS 0x2

// Experimental EmbedPDF Extension API.
// Copy pages of |document| into a NEW document, each page as it draws for
// |usage|: with FLAT_PRINT, a print copy.
//
// The annotations |flags| chooses that draw for |usage| are painted into the
// page content, with the plan and placement of EPDFPage_Flatten(). Flattening
// paints what the renderer draws, so an annotation without a stored
// appearance is painted with the one the renderer builds in memory, from the
// source document's form defaults and fonts.
//
// Nothing interactive is copied: no /Annots, no page actions (/AA), no
// article beads, and none of the catalog (outline, form, document scripts).
// Optional content keeps the state it has for |usage|: the copy's default
// configuration turns off the groups that are off. The source is untouched.
//
//   document     - the source document.
//   page_indices - the pages to copy, by index, in the copy's order.
//   length       - number of entries; > 0.
//   usage        - exactly one of the |FLAT_*| values.
//   flags        - EPDF_EXPORT_FLATTENED_* bits; 0 copies the page content
//                  alone.
//
// Returns a new document (the caller closes it with FPDF_CloseDocument()),
// or NULL for invalid arguments or a copy that failed.
FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFDoc_ExportPagesFlattened(FPDF_DOCUMENT document,
                             const int* page_indices,
                             unsigned long length,
                             int usage,
                             int flags);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_FLATTEN_H_
