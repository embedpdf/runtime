// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_DOC_H_
#define PUBLIC_EPDF_DOC_H_

// NOLINTNEXTLINE(build/include)
#include "fpdf_doc.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// EmbedPDF's additions to public/fpdf_doc.h: document metadata and its trapped
// status, destinations, named destinations, actions and page label sections.
// The outline's writers are in public/epdf_outline.h.

// The trapped status of the document. See section 14.10.2.4 "Trapped" of the
// ISO 32000-1:2008 spec.
typedef enum FPDF_TRAPPED_STATUS {
  PDFTRAPPED_NOTSET = 0,  // No /Trapped key
  PDFTRAPPED_TRUE = 1,    // Explicitly /Trapped /True
  PDFTRAPPED_FALSE = 2,   // Explicitly /Trapped /False
  PDFTRAPPED_UNKNOWN = 3  // Explicitly /Trapped /Unknown or invalid
} FPDF_TRAPPED_STATUS;

// Experimental EmbedPDF Extension API.
// Get the PDF indirect object number of the page targeted by |dest|.
//
//   document - handle to the document containing the destination page.
//   dest     - handle to the destination.
//
// Returns the page dictionary object number (> 0) on success, or 0 if the
// document/destination is invalid, the target is not a visible page in the
// document, the page dictionary is a direct object, or the page is an XFA
// page. If the destination contains a numeric page index, it is resolved
// against |document|, matching FPDFDest_GetDestPageIndex() compatibility
// behavior.
FPDF_EXPORT unsigned int FPDF_CALLCONV
EPDFDest_GetPageObjectNumber(FPDF_DOCUMENT document, FPDF_DEST dest);

// Experimental EmbedPDF Extension API.
// Set meta-data |tag| content in |document|.
//
//   document - handle to the document.
//   tag      - the tag to set.
//   value    - the value to set; NULL removes |tag|. An empty string is a
//              value: |tag| stays, present and empty.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDF_SetMetaText(FPDF_DOCUMENT document,
                                                     FPDF_BYTESTRING tag,
                                                     FPDF_WIDESTRING value);

// Experimental EmbedPDF Extension API.
// Check if meta-data |tag| exists in |document|.
//
//   document - handle to the document.
//   tag      - the tag to check.
//
// Returns true if |tag| exists in |document|.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDF_HasMetaText(FPDF_DOCUMENT document,
                                                     FPDF_BYTESTRING tag);

// Experimental EmbedPDF Extension API.
// Get the trapped status of |document|.
//
//   document - handle to the document.
//
// Returns the trapped status of |document|.
FPDF_EXPORT FPDF_TRAPPED_STATUS FPDF_CALLCONV
EPDF_GetMetaTrapped(FPDF_DOCUMENT document);

// Experimental EmbedPDF Extension API.
// Set the trapped status of |document|.
//
//   document - handle to the document.
//   status   - the trapped status to set.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_SetMetaTrapped(FPDF_DOCUMENT document, FPDF_TRAPPED_STATUS status);

// Experimental EmbedPDF Extension API.
// Get the number of keys in the document's Info dictionary.
//
//   document    - handle to the document.
//   custom_only - if true, only count non-reserved (custom) keys; if false,
//                 count all keys.
//
// Returns the number of keys (possibly 0). On error, returns 0.
FPDF_EXPORT int FPDF_CALLCONV EPDF_GetMetaKeyCount(FPDF_DOCUMENT document,
                                                   FPDF_BOOL custom_only);

// Experimental EmbedPDF Extension API.
// Get the name of the Info dictionary key at |index|.
//
//   document    - handle to the document.
//   index       - 0-based key index in the order returned by PDFium.
//   custom_only - if true, indexes only over non-reserved (custom) keys; if
//                 false, indexes over all keys.
//   buffer      - a buffer for the key name in UTF-8 with trailing NUL. May be
//   NULL. buflen      - the length of the buffer, in bytes. May be 0.
//
// Returns the number of bytes in the key name including the trailing NUL, or 0
// on error (bad |document|, |index| out of range, etc.). If |buflen| is less
// than the returned length, or |buffer| is NULL, |buffer| will not be modified.
//
// Reserved keys (excluded when |custom_only| is true) are:
//   Title, Author, Subject, Keywords, Producer, Creator,
//   CreationDate, ModDate, Trapped.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDF_GetMetaKeyName(FPDF_DOCUMENT document,
                    int index,
                    FPDF_BOOL custom_only,
                    void* buffer,
                    unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Create a new destination array of the form [page /XYZ left top zoom].
//
//   page     - handle to the destination page.
//   has_left - whether |left| is specified; if false, |left| is encoded as
//   null. left     - the left coordinate, in page coordinates. has_top  -
//   whether |top| is specified; if false, |top| is encoded as null. top      -
//   the top coordinate, in page coordinates. has_zoom - whether |zoom| is
//   specified; if false or |zoom|==0, encoded as null. zoom     - the zoom
//   factor (must be non-zero to be considered specified).
//
// Returns a handle to the created (INDIRECT) destination array, or NULL on
// error.
//
// Notes:
//  * The returned object is an INDIRECT array suitable for use by
//    EPDFAction_CreateGoTo().
//  * Unspecified fields are encoded as PDF nulls.
FPDF_EXPORT FPDF_DEST FPDF_CALLCONV EPDFDest_CreateXYZ(FPDF_PAGE page,
                                                       FPDF_BOOL has_left,
                                                       FS_FLOAT left,
                                                       FPDF_BOOL has_top,
                                                       FS_FLOAT top,
                                                       FPDF_BOOL has_zoom,
                                                       FS_FLOAT zoom);

// Experimental EmbedPDF Extension API.
// Create a new destination array of the form [page /<View> params…].
//
//   page        - handle to the destination page.
//   view        - one of the PDFDEST_VIEW_* constants EXCEPT PDFDEST_VIEW_XYZ.
//                 Valid: PDFDEST_VIEW_FIT, FITH, FITV, FITR, FITB, FITBH,
//                 FITBV.
//   params      - pointer to an array of float parameters (may be NULL).
//   num_params  - number of entries in |params|.
//
// Returns a handle to the created (INDIRECT) destination array, or NULL on
// error.
//
// Notes:
//  * The required parameter count depends on |view| and matches
//  FPDFDest_GetView().
//    Excess parameters are ignored; a missing parameter is written as null
//    (the viewer keeps its current value, ISO 32000 Table 151).
//  * Use EPDFDest_CreateXYZ() for /XYZ destinations.
FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDest_CreateView(FPDF_PAGE page,
                    unsigned long view,
                    const FS_FLOAT* params,
                    unsigned long num_params);

// Experimental EmbedPDF Extension API.
// As EPDFDest_CreateXYZ(), for the page whose dictionary is object
// |page_obj_num| of |document|. The page is not loaded: the destination only
// refers to its dictionary.
//
// Returns NULL when |page_obj_num| is not a page of |document|.
FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDest_CreateXYZByObjectNumber(FPDF_DOCUMENT document,
                                 unsigned int page_obj_num,
                                 FPDF_BOOL has_left,
                                 FS_FLOAT left,
                                 FPDF_BOOL has_top,
                                 FS_FLOAT top,
                                 FPDF_BOOL has_zoom,
                                 FS_FLOAT zoom);

// Experimental EmbedPDF Extension API.
// As EPDFDest_CreateView(), for the page whose dictionary is object
// |page_obj_num| of |document|. The page is not loaded: the destination only
// refers to its dictionary.
//
// Returns NULL when |page_obj_num| is not a page of |document|, or on the
// errors EPDFDest_CreateView() returns NULL for.
FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDest_CreateViewByObjectNumber(FPDF_DOCUMENT document,
                                  unsigned int page_obj_num,
                                  unsigned long view,
                                  const FS_FLOAT* params,
                                  unsigned long num_params);

// Experimental EmbedPDF Extension API.
// Which view parameters of |dest| are null, where the viewer keeps its
// current value: bit i is set when parameter i of FPDFDest_GetView() is
// null (FPDFDest_GetView() reports it as 0).
//
// Returns 0 for a NULL |dest|, or when no parameter is null.
FPDF_EXPORT unsigned int FPDF_CALLCONV EPDFDest_GetViewNullParams(FPDF_DEST dest);

// Experimental EmbedPDF Extension API.
// Create a new *remote* destination array of the form [pageIndex /<View>
// params…].
//
//   document    - handle to the owning document.
//   page_index  - 0-based page index in the *remote* file (must be >= 0).
//   view        - one of the PDFDEST_VIEW_* constants EXCEPT PDFDEST_VIEW_XYZ.
//   params      - pointer to float parameters (may be NULL).
//   num_params  - number of parameters.
//
// Returns a handle to the created (INDIRECT) destination array, or NULL on
// error.
//
// Notes:
//  * This is an explicit *remote* dest (first element is a number).
//  * Use EPDFDest_CreateRemoteXYZ() for /XYZ.
FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDest_CreateRemoteView(FPDF_DOCUMENT document,
                          int page_index,
                          unsigned long view,
                          const FS_FLOAT* params,
                          unsigned long num_params);

// Experimental EmbedPDF Extension API.
// Create a new *remote* XYZ destination: [pageIndex /XYZ left? top? zoom?].
//
//   document    - handle to the owning document.
//   page_index  - 0-based page index in the *remote* file (must be >= 0).
//   has_left,left,has_top,top,has_zoom,zoom - as in EPDFDest_CreateXYZ().
//
// Returns a handle to the created (INDIRECT) destination array, or NULL on
// error.
//
// Notes:
//  * The left/top/zoom fields are encoded as number-or-null as in local /XYZ.
FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDest_CreateRemoteXYZ(FPDF_DOCUMENT document,
                         int page_index,
                         FPDF_BOOL has_left,
                         FS_FLOAT left,
                         FPDF_BOOL has_top,
                         FS_FLOAT top,
                         FPDF_BOOL has_zoom,
                         FS_FLOAT zoom);

// -----------------------------------------------------------------------------
// Named destinations
// -----------------------------------------------------------------------------

// Experimental EmbedPDF Extension API.
// Add or replace a named destination mapping to an existing INDIRECT |dest|.
//
//   document - handle to the document owning both the name tree and |dest|.
//   name     - UTF-8 zero-terminated name key.
//   dest     - handle to an INDIRECT destination array that belongs to
//   |document|.
//
// Returns true on success.
//
// Notes:
//  * Stores the value as an *indirect reference* to |dest| in /Names/Dests.
//  * Fails if |dest| is not an indirect object of |document|.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNamedDest_SetDest(FPDF_DOCUMENT document,
                      FPDF_BYTESTRING name,
                      FPDF_DEST dest);

// Experimental EmbedPDF Extension API.
// Remove a named destination mapping. No-op if absent.
//
//   document - handle to the document.
//   name     - UTF-8 zero-terminated name key.
//
// Returns true on success (or if the name did not exist).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFNamedDest_Remove(FPDF_DOCUMENT document,
                                                         FPDF_BYTESTRING name);

// -----------------------------------------------------------------------------
// Actions
// -----------------------------------------------------------------------------

// Experimental EmbedPDF Extension API.
// Create an in-document "GoTo" action targeting |dest|.
//
//   document - handle to the document that will own the action.
//   dest     - handle to an in-document *local* destination (INDIRECT).
//
// Returns a handle to the created action dictionary, or NULL on error.
//
// Notes:
//  * The returned action has /S /GoTo and /D set to an indirect ref of |dest|.
//  * |dest| must be an INDIRECT array whose first element resolves to a page
//    dictionary in |document| (i.e. a local explicit destination).
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateGoTo(FPDF_DOCUMENT document, FPDF_DEST dest);

// Experimental EmbedPDF Extension API.
// Create a "GoToR" (remote go-to) action targeting a *named* destination
// in the remote file.
//
//   document   - handle to the document that will own the action.
//   file_path  - UTF-16LE path to the remote file (FileSpec as string).
//   named_dest - UTF-16LE named destination in the *remote* file.
//
// Returns a handle to the created action dictionary, or NULL on error.
//
// Notes:
//  * The returned action has /S /GoToR, /F set to |file_path|, and /D to the
//    remote *name* (text string).
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateRemoteGoToByName(FPDF_DOCUMENT document,
                                  FPDF_WIDESTRING file_path,
                                  FPDF_WIDESTRING named_dest);

// Experimental EmbedPDF Extension API.
// Create a "GoToR" (remote go-to) action targeting an explicit *remote* dest.
//
//   document  - handle to the document that will own the action.
//   file_path - UTF-16LE path to the remote file (FileSpec as string).
//   dest      - handle to an INDIRECT destination array of the form
//               [pageIndex /XYZ|Fit* ...] belonging to |document|.
//
// Returns a handle to the created action dictionary, or NULL on error.
//
// Notes:
//  * The returned action has /S /GoToR, /F set to |file_path|, and /D is an
//    *indirect reference* to |dest|.
//  * Fails if |dest| is not an INDIRECT object in |document| or if its first
//    element is not a number (remote page index).
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateRemoteGoToDest(FPDF_DOCUMENT document,
                                FPDF_WIDESTRING file_path,
                                FPDF_DEST dest);

// Experimental EmbedPDF Extension API.
// Create a "GoTo" action targeting a *named* destination in the same document.
//
//   document - handle to the document that will own the action.
//   name     - UTF-8 zero-terminated name.
//
// Returns a handle to the created action dictionary, or NULL on error.
//
// Notes:
//  * The returned action has /S /GoTo and /D set to the name (text string).
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateGoToNamed(FPDF_DOCUMENT document, FPDF_BYTESTRING name);

// Experimental EmbedPDF Extension API.
// Create a "Launch" action with a simple FileSpec-as-string.
//
//   document - handle to the document that will own the action.
//   file_path - UTF-16LE path.
//
// Returns a handle to the created action dictionary, or NULL on error.
//
// Notes:
//  * The returned action has /S /Launch and /F set to |file_path|.
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateLaunch(FPDF_DOCUMENT document, FPDF_WIDESTRING file_path);

// Experimental EmbedPDF Extension API.
// Create a "URI" action with the given UTF-8 |uri|.
//
//   document - handle to the document that will own the action.
//   uri      - UTF-8 zero-terminated string.
//
// Returns a handle to the created action dictionary, or NULL on error.
//
// Notes:
//  * The returned action has /S /URI and /URI (byte string) set to |uri|.
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateURI(FPDF_DOCUMENT document, FPDF_BYTESTRING uri);

// Experimental EmbedPDF Extension API.
// Create a "Named" action that runs the viewer action |name|, such as
// NextPage, PrevPage, FirstPage or LastPage (ISO 32000 Table 211).
//
//   document - handle to the document that will own the action.
//   name     - the action's name, without the leading slash.
//
// Returns a handle to the created (indirect) action dictionary, or NULL for
// an empty |name| or on error. The name is written as given; which names
// make sense is the caller's choice.
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateNamed(FPDF_DOCUMENT document, FPDF_BYTESTRING name);

// How a page label section numbers its pages (/S, ISO 32000-1 12.4.2).
// No number: the label is the prefix alone. Also for a style PDFium
// doesn't know, which it labels the same way.
#define EPDF_PAGE_LABEL_STYLE_NONE 0
// /D: 1, 2, 3.
#define EPDF_PAGE_LABEL_STYLE_DECIMAL 1
// /R: I, II, III.
#define EPDF_PAGE_LABEL_STYLE_UPPER_ROMAN 2
// /r: i, ii, iii.
#define EPDF_PAGE_LABEL_STYLE_LOWER_ROMAN 3
// /A: A to Z, then AA to ZZ.
#define EPDF_PAGE_LABEL_STYLE_UPPER_LETTERS 4
// /a: a to z, then aa to zz.
#define EPDF_PAGE_LABEL_STYLE_LOWER_LETTERS 5

// Experimental EmbedPDF Extension API.
// The number of page label sections of |document| (/PageLabels). A section
// labels the pages from its first page up to the next section's: each page
// gets the section's prefix followed by its number in the section's style,
// counting from the section's first number. The sections are read as
// FPDF_GetPageLabel() labels the pages:
//
//   - an entry of the number tree starts a section at its key, the page
//     index; keys out of order are taken in order, and of two entries with
//     one key the later one counts;
//   - an entry whose key isn't a page of the document is left out;
//   - pages before the first entry, and an entry that isn't a dictionary,
//     are numbered 1, 2, 3 by their place in the document, without a
//     prefix.
//
// So the first section always starts at page 0. (For a tree whose keys are
// out of order, or whose /Limits are wrong, FPDF_GetPageLabel() reads
// differently; for a tree as ISO 32000 describes it, the readings are the
// same.)
//
//   document - handle to the document.
//
// Returns the number of sections: 0 when the document has no /PageLabels,
// and its pages no labels, or -1 for an invalid |document|.
FPDF_EXPORT int FPDF_CALLCONV
EPDFDoc_GetPageLabelSectionCount(FPDF_DOCUMENT document);

// Experimental EmbedPDF Extension API.
// Read page label section |index| of |document|.
//
//   document     - handle to the document.
//   index        - the section, 0 <= index <
//                  EPDFDoc_GetPageLabelSectionCount(), in page order.
//   buffer       - receives the section's prefix (/P) as UTF-16LE with a NUL
//                  terminator; may be NULL to query the size.
//   buflen       - size of |buffer| in bytes.
//   first_page   - receives the index of the section's first page; may be
//                  NULL.
//   style        - receives one of EPDF_PAGE_LABEL_STYLE_*; may be NULL.
//   first_number - receives the number of the section's first page (/St,
//                  1 when the entry gives none, as written even below 1);
//                  may be NULL.
//
// Returns the number of bytes the prefix needs including the terminator (2
// for no prefix; it is copied only when |buflen| holds it), or 0 for an
// invalid |document| or |index|.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetPageLabelSectionAt(FPDF_DOCUMENT document,
                              int index,
                              FPDF_WCHAR* buffer,
                              unsigned long buflen,
                              int* first_page,
                              int* style,
                              int* first_number);

// Experimental EmbedPDF Extension API.
// Replace the page label sections of |document| with |count| sections: the
// i-th starts at page |first_pages[i]|, numbers in style |styles[i]| (one of
// EPDF_PAGE_LABEL_STYLE_*) from |first_numbers[i]|, and has the prefix
// |prefixes[i]| (UTF-16LE, NUL-terminated; NULL or empty for none). NULL
// |prefixes| gives no section a prefix.
//
// The first section starts at page 0, each one after the one before, and
// every one at a page of the document. 0 |count| removes /PageLabels, and
// with it the pages' labels.
//
// /PageLabels is written as one number tree of the sections, in that order:
// into its own object when it is one, which keeps its number, else into the
// catalog. A section's /S, /P and /St are written when they say something:
// /St only when the first number isn't 1. Where the sections go is the
// caller's: pages that move, come or go take no section along by themselves.
//
// Returns true when the sections were written; false, writing nothing, for an
// invalid |document| or sections that break the rules above.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_SetPageLabelSections(FPDF_DOCUMENT document,
                             const int* first_pages,
                             const int* styles,
                             const int* first_numbers,
                             const FPDF_WIDESTRING* prefixes,
                             unsigned long count);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_DOC_H_
