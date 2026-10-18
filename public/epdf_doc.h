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
// status, destinations, named destinations and actions. The outline's
// writers are in public/epdf_outline.h.

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

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_DOC_H_
