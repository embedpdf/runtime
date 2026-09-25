// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef PUBLIC_FPDF_DOC_H_
#define PUBLIC_FPDF_DOC_H_

// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Unsupported action type.
#define PDFACTION_UNSUPPORTED 0
// Go to a destination within current document.
#define PDFACTION_GOTO 1
// Go to a destination within another document.
#define PDFACTION_REMOTEGOTO 2
// URI, including web pages and other Internet resources.
#define PDFACTION_URI 3
// Launch an application or open a file.
#define PDFACTION_LAUNCH 4
// Go to a destination in an embedded file.
#define PDFACTION_EMBEDDEDGOTO 5

// View destination fit types. See pdfmark reference v9, page 48.
#define PDFDEST_VIEW_UNKNOWN_MODE 0
#define PDFDEST_VIEW_XYZ 1
#define PDFDEST_VIEW_FIT 2
#define PDFDEST_VIEW_FITH 3
#define PDFDEST_VIEW_FITV 4
#define PDFDEST_VIEW_FITR 5
#define PDFDEST_VIEW_FITB 6
#define PDFDEST_VIEW_FITBH 7
#define PDFDEST_VIEW_FITBV 8

// The file identifier entry type. See section 14.4 "File Identifiers" of the
// ISO 32000-1:2008 spec.
typedef enum {
  FILEIDTYPE_PERMANENT = 0,
  FILEIDTYPE_CHANGING = 1
} FPDF_FILEIDTYPE;

// The trapped status of the document. See section 14.10.2.4 "Trapped" of the
// ISO 32000-1:2008 spec.
typedef enum FPDF_TRAPPED_STATUS {
  PDFTRAPPED_NOTSET = 0,  // No /Trapped key
  PDFTRAPPED_TRUE = 1,    // Explicitly /Trapped /True
  PDFTRAPPED_FALSE = 2,   // Explicitly /Trapped /False
  PDFTRAPPED_UNKNOWN = 3  // Explicitly /Trapped /Unknown or invalid
} FPDF_TRAPPED_STATUS;

// Get the first child of |bookmark|, or the first top-level bookmark item.
//
//   document - handle to the document.
//   bookmark - handle to the current bookmark. Pass NULL for the first top
//              level item.
//
// Returns a handle to the first child of |bookmark| or the first top-level
// bookmark item. NULL if no child or top-level bookmark found.
// Note that another name for the bookmarks is the document outline, as
// described in ISO 32000-1:2008, section 12.3.3.
FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
FPDFBookmark_GetFirstChild(FPDF_DOCUMENT document, FPDF_BOOKMARK bookmark);

// Get the next sibling of |bookmark|.
//
//   document - handle to the document.
//   bookmark - handle to the current bookmark.
//
// Returns a handle to the next sibling of |bookmark|, or NULL if this is the
// last bookmark at this level.
//
// Note that the caller is responsible for handling circular bookmark
// references, as may arise from malformed documents.
FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
FPDFBookmark_GetNextSibling(FPDF_DOCUMENT document, FPDF_BOOKMARK bookmark);

// Get the title of |bookmark|.
//
//   bookmark - handle to the bookmark.
//   buffer   - buffer for the title. May be NULL.
//   buflen   - the length of the buffer in bytes. May be 0.
//
// Returns the number of bytes in the title, including the terminating NUL
// character. The number of bytes is returned regardless of the |buffer| and
// |buflen| parameters.
//
// Regardless of the platform, the |buffer| is always in UTF-16LE encoding. The
// string is terminated by a UTF16 NUL character. If |buflen| is less than the
// required length, or |buffer| is NULL, |buffer| will not be modified.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFBookmark_GetTitle(FPDF_BOOKMARK bookmark,
                      void* buffer,
                      unsigned long buflen);

// Experimental API.
// Get the number of chlidren of |bookmark|.
//
//   bookmark - handle to the bookmark.
//
// Returns a signed integer that represents the number of sub-items the given
// bookmark has. If the value is positive, child items shall be shown by default
// (open state). If the value is negative, child items shall be hidden by
// default (closed state). Please refer to PDF 32000-1:2008, Table 153.
// Returns 0 if the bookmark has no children or is invalid.
FPDF_EXPORT int FPDF_CALLCONV FPDFBookmark_GetCount(FPDF_BOOKMARK bookmark);

// Find the bookmark with |title| in |document|.
//
//   document - handle to the document.
//   title    - the UTF-16LE encoded Unicode title for which to search.
//
// Returns the handle to the bookmark, or NULL if |title| can't be found.
//
// FPDFBookmark_Find() will always return the first bookmark found even if
// multiple bookmarks have the same |title|.
FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
FPDFBookmark_Find(FPDF_DOCUMENT document, FPDF_WIDESTRING title);

// Get the destination associated with |bookmark|.
//
//   document - handle to the document.
//   bookmark - handle to the bookmark.
//
// Returns the handle to the destination data, or NULL if no destination is
// associated with |bookmark|.
FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
FPDFBookmark_GetDest(FPDF_DOCUMENT document, FPDF_BOOKMARK bookmark);

// Get the action associated with |bookmark|.
//
//   bookmark - handle to the bookmark.
//
// Returns the handle to the action data, or NULL if no action is associated
// with |bookmark|.
// If this function returns a valid handle, it is valid as long as |bookmark| is
// valid.
// If this function returns NULL, FPDFBookmark_GetDest() should be called to get
// the |bookmark| destination data.
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
FPDFBookmark_GetAction(FPDF_BOOKMARK bookmark);

// Get the type of |action|.
//
//   action - handle to the action.
//
// Returns one of:
//   PDFACTION_UNSUPPORTED
//   PDFACTION_GOTO
//   PDFACTION_REMOTEGOTO
//   PDFACTION_URI
//   PDFACTION_LAUNCH
FPDF_EXPORT unsigned long FPDF_CALLCONV FPDFAction_GetType(FPDF_ACTION action);

// Get the destination of |action|.
//
//   document - handle to the document.
//   action   - handle to the action. |action| must be a |PDFACTION_GOTO| or
//              |PDFACTION_REMOTEGOTO|.
//
// Returns a handle to the destination data, or NULL on error, typically
// because the arguments were bad or the action was of the wrong type.
//
// In the case of |PDFACTION_REMOTEGOTO|, you must first call
// FPDFAction_GetFilePath(), then load the document at that path, then pass
// the document handle from that document as |document| to FPDFAction_GetDest().
FPDF_EXPORT FPDF_DEST FPDF_CALLCONV FPDFAction_GetDest(FPDF_DOCUMENT document,
                                                       FPDF_ACTION action);

// Get the file path of |action|.
//
//   action - handle to the action. |action| must be a |PDFACTION_LAUNCH| or
//            |PDFACTION_REMOTEGOTO|.
//   buffer - a buffer for output the path string. May be NULL.
//   buflen - the length of the buffer, in bytes. May be 0.
//
// Returns the number of bytes in the file path, including the trailing NUL
// character, or 0 on error, typically because the arguments were bad or the
// action was of the wrong type.
//
// Regardless of the platform, the |buffer| is always in UTF-8 encoding.
// If |buflen| is less than the returned length, or |buffer| is NULL, |buffer|
// will not be modified.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAction_GetFilePath(FPDF_ACTION action, void* buffer, unsigned long buflen);

// Get the URI path of |action|.
//
//   document - handle to the document.
//   action   - handle to the action. Must be a |PDFACTION_URI|.
//   buffer   - a buffer for the path string. May be NULL.
//   buflen   - the length of the buffer, in bytes. May be 0.
//
// Returns the number of bytes in the URI path, including the trailing NUL
// character, or 0 on error, typically because the arguments were bad or the
// action was of the wrong type.
//
// The |buffer| may contain badly encoded data. The caller should validate the
// output. e.g. Check to see if it is UTF-8.
//
// If |buflen| is less than the returned length, or |buffer| is NULL, |buffer|
// will not be modified.
//
// Historically, the documentation for this API claimed |buffer| is always
// encoded in 7-bit ASCII, but did not actually enforce it.
// https://pdfium.googlesource.com/pdfium.git/+/d609e84cee2e14a18333247485af91df48a40592
// added that enforcement, but that did not work well for real world PDFs that
// used UTF-8. As of this writing, this API reverted back to its original
// behavior prior to commit d609e84cee.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAction_GetURIPath(FPDF_DOCUMENT document,
                      FPDF_ACTION action,
                      void* buffer,
                      unsigned long buflen);

// Get the page index of |dest|.
//
//   document - handle to the document.
//   dest     - handle to the destination.
//
// Returns the 0-based page index containing |dest|. Returns -1 on error.
FPDF_EXPORT int FPDF_CALLCONV FPDFDest_GetDestPageIndex(FPDF_DOCUMENT document,
                                                        FPDF_DEST dest);

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

// Experimental API.
// Get the view (fit type) specified by |dest|.
//
//   dest         - handle to the destination.
//   pNumParams   - receives the number of view parameters, which is at most 4.
//   pParams      - buffer to write the view parameters. Must be at least 4
//                  FS_FLOATs long.
// Returns one of the PDFDEST_VIEW_* constants, PDFDEST_VIEW_UNKNOWN_MODE if
// |dest| does not specify a view.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFDest_GetView(FPDF_DEST dest, unsigned long* pNumParams, FS_FLOAT* pParams);

// Get the (x, y, zoom) location of |dest| in the destination page, if the
// destination is in [page /XYZ x y zoom] syntax.
//
//   dest       - handle to the destination.
//   hasXVal    - out parameter; true if the x value is not null
//   hasYVal    - out parameter; true if the y value is not null
//   hasZoomVal - out parameter; true if the zoom value is not null
//   x          - out parameter; the x coordinate, in page coordinates.
//   y          - out parameter; the y coordinate, in page coordinates.
//   zoom       - out parameter; the zoom value.
// Returns TRUE on successfully reading the /XYZ value.
//
// Note the [x, y, zoom] values are only set if the corresponding hasXVal,
// hasYVal or hasZoomVal flags are true.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFDest_GetLocationInPage(FPDF_DEST dest,
                           FPDF_BOOL* hasXVal,
                           FPDF_BOOL* hasYVal,
                           FPDF_BOOL* hasZoomVal,
                           FS_FLOAT* x,
                           FS_FLOAT* y,
                           FS_FLOAT* zoom);

// Find a link at point (|x|,|y|) on |page|.
//
//   page - handle to the document page.
//   x    - the x coordinate, in the page coordinate system.
//   y    - the y coordinate, in the page coordinate system.
//
// Returns a handle to the link, or NULL if no link found at the given point.
//
// You can convert coordinates from screen coordinates to page coordinates using
// FPDF_DeviceToPage().
FPDF_EXPORT FPDF_LINK FPDF_CALLCONV FPDFLink_GetLinkAtPoint(FPDF_PAGE page,
                                                            double x,
                                                            double y);

// Find the Z-order of link at point (|x|,|y|) on |page|.
//
//   page - handle to the document page.
//   x    - the x coordinate, in the page coordinate system.
//   y    - the y coordinate, in the page coordinate system.
//
// Returns the Z-order of the link, or -1 if no link found at the given point.
// Larger Z-order numbers are closer to the front.
//
// You can convert coordinates from screen coordinates to page coordinates using
// FPDF_DeviceToPage().
FPDF_EXPORT int FPDF_CALLCONV FPDFLink_GetLinkZOrderAtPoint(FPDF_PAGE page,
                                                            double x,
                                                            double y);

// Get destination info for |link|.
//
//   document - handle to the document.
//   link     - handle to the link.
//
// Returns a handle to the destination, or NULL if there is no destination
// associated with the link. In this case, you should call FPDFLink_GetAction()
// to retrieve the action associated with |link|.
FPDF_EXPORT FPDF_DEST FPDF_CALLCONV FPDFLink_GetDest(FPDF_DOCUMENT document,
                                                     FPDF_LINK link);

// Get action info for |link|.
//
//   link - handle to the link.
//
// Returns a handle to the action associated to |link|, or NULL if no action.
// If this function returns a valid handle, it is valid as long as |link| is
// valid.
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV FPDFLink_GetAction(FPDF_LINK link);

// Enumerates all the link annotations in |page|.
//
//   page       - handle to the page.
//   start_pos  - the start position, should initially be 0 and is updated with
//                the next start position on return.
//   link_annot - the link handle for |startPos|.
//
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFLink_Enumerate(FPDF_PAGE page,
                                                       int* start_pos,
                                                       FPDF_LINK* link_annot);

// Experimental API.
// Gets FPDF_ANNOTATION object for |link_annot|.
//
//   page       - handle to the page in which FPDF_LINK object is present.
//   link_annot - handle to link annotation.
//
// Returns FPDF_ANNOTATION from the FPDF_LINK and NULL on failure,
// if the input link annot or page is NULL.
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDFLink_GetAnnot(FPDF_PAGE page, FPDF_LINK link_annot);

// Get the rectangle for |link_annot|.
//
//   link_annot - handle to the link annotation.
//   rect       - the annotation rectangle.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFLink_GetAnnotRect(FPDF_LINK link_annot,
                                                          FS_RECTF* rect);

// Get the count of quadrilateral points to the |link_annot|.
//
//   link_annot - handle to the link annotation.
//
// Returns the count of quadrilateral points.
FPDF_EXPORT int FPDF_CALLCONV FPDFLink_CountQuadPoints(FPDF_LINK link_annot);

// Get the quadrilateral points for the specified |quad_index| in |link_annot|.
//
//   link_annot  - handle to the link annotation.
//   quad_index  - the specified quad point index.
//   quad_points - receives the quadrilateral points.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFLink_GetQuadPoints(FPDF_LINK link_annot,
                       int quad_index,
                       FS_QUADPOINTSF* quad_points);

// Experimental API
// Gets an additional-action from |page|.
//
//   page      - handle to the page, as returned by FPDF_LoadPage().
//   aa_type   - the type of the page object's addtional-action, defined
//               in public/fpdf_formfill.h
//
//   Returns the handle to the action data, or NULL if there is no
//   additional-action of type |aa_type|.
//   If this function returns a valid handle, it is valid as long as |page| is
//   valid.
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV FPDF_GetPageAAction(FPDF_PAGE page,
                                                          int aa_type);

// Experimental API.
// Get the file identifer defined in the trailer of |document|.
//
//   document - handle to the document.
//   id_type  - the file identifier type to retrieve.
//   buffer   - a buffer for the file identifier. May be NULL.
//   buflen   - the length of the buffer, in bytes. May be 0.
//
// Returns the number of bytes in the file identifier, including the NUL
// terminator.
//
// The |buffer| is always a byte string. The |buffer| is followed by a NUL
// terminator.  If |buflen| is less than the returned length, or |buffer| is
// NULL, |buffer| will not be modified.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDF_GetFileIdentifier(FPDF_DOCUMENT document,
                       FPDF_FILEIDTYPE id_type,
                       void* buffer,
                       unsigned long buflen);

// Get meta-data |tag| content from |document|.
//
//   document - handle to the document.
//   tag      - the tag to retrieve. The tag can be one of:
//                Title, Author, Subject, Keywords, Creator, Producer,
//                CreationDate, or ModDate.
//              For detailed explanations of these tags and their respective
//              values, please refer to PDF Reference 1.6, section 10.2.1,
//              'Document Information Dictionary'.
//   buffer   - a buffer for the tag. May be NULL.
//   buflen   - the length of the buffer, in bytes. May be 0.
//
// Returns the number of bytes in the tag, including trailing zeros.
//
// The |buffer| is always encoded in UTF-16LE. The |buffer| is followed by two
// bytes of zeros indicating the end of the string.  If |buflen| is less than
// the returned length, or |buffer| is NULL, |buffer| will not be modified.
//
// For linearized files, FPDFAvail_IsFormAvail must be called before this, and
// it must have returned PDF_FORM_AVAIL or PDF_FORM_NOTEXIST. Before that, there
// is no guarantee the metadata has been loaded.
FPDF_EXPORT unsigned long FPDF_CALLCONV FPDF_GetMetaText(FPDF_DOCUMENT document,
                                                         FPDF_BYTESTRING tag,
                                                         void* buffer,
                                                         unsigned long buflen);

// Get the page label for |page_index| from |document|.
//
//   document    - handle to the document.
//   page_index  - the 0-based index of the page.
//   buffer      - a buffer for the page label. May be NULL.
//   buflen      - the length of the buffer, in bytes. May be 0.
//
// Returns the number of bytes in the page label, including trailing zeros.
//
// The |buffer| is always encoded in UTF-16LE. The |buffer| is followed by two
// bytes of zeros indicating the end of the string.  If |buflen| is less than
// the returned length, or |buffer| is NULL, |buffer| will not be modified.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDF_GetPageLabel(FPDF_DOCUMENT document,
                  int page_index,
                  void* buffer,
                  unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Set meta-data |tag| content in |document|.
//
//   document - handle to the document.
//   tag      - the tag to set.
//   value    - the value to set.
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
//    EPDFBookmark_SetDest() or EPDFAction_CreateGoTo().
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
//    Excess parameters are ignored; missing parameters default to 0.
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

// -----------------------------------------------------------------------------
// Outlines / bookmarks
// -----------------------------------------------------------------------------

// Experimental EmbedPDF Outline API.
// Create a new top-level bookmark appended to the end of the outline.
//
//   document - handle to the document.
//   title    - UTF-16LE encoded title string; may be empty.
//
// Returns a handle to the created bookmark, or NULL on error.
//
// Notes:
//  * Creates the document's /Outlines dictionary if it does not exist.
//  * The new node is inserted as the last top-level item.
//  * The bookmark is created without a destination or action; use
//    EPDFBookmark_SetDest() or EPDFBookmark_SetAction() to set a target.
FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
EPDFBookmark_Create(FPDF_DOCUMENT document, FPDF_WIDESTRING title);

// Experimental EmbedPDF Outline API.
// Delete a bookmark subtree rooted at |bookmark|.
//
//   document - handle to the document.
//   bookmark - handle to the root of the subtree to delete.
//
// Returns true on success.
//
// Notes:
//  * Removes the node from its parent's list and deletes the entire subtree,
//    fixing /First, /Last, /Prev, /Next as needed.
//  * Fails if |bookmark| does not belong to |document|.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFBookmark_Delete(FPDF_DOCUMENT document,
                                                        FPDF_BOOKMARK bookmark);

// Experimental EmbedPDF Outline API.
// Create and append a new child bookmark under |parent|.
//
//   document - handle to the document.
//   parent   - handle to the parent bookmark; NULL inserts at top level.
//   title    - UTF-16LE encoded title string; may be empty.
//
// Returns a handle to the created bookmark, or NULL on error.
//
// Notes:
//  * If |parent| is NULL, the node is appended as a top-level item.
//  * The bookmark is created without a destination or action; set one
//    with EPDFBookmark_SetDest() or EPDFBookmark_SetAction().
FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
EPDFBookmark_AppendChild(FPDF_DOCUMENT document,
                         FPDF_BOOKMARK parent,
                         FPDF_WIDESTRING title);

// Experimental EmbedPDF Outline API.
// Create and insert a new child under |parent| right after |after_sibling|.
//
//   document      - handle to the document.
//   parent        - handle to the parent bookmark; NULL means top level.
//   after_sibling - handle to the sibling to insert after; if NULL, inserts
//                   as the first child under |parent|.
//   title         - UTF-16LE encoded title string; may be empty.
//
// Returns a handle to the created bookmark, or NULL on error.
//
// Notes:
//  * |after_sibling| (if non-NULL) must be a direct child of |parent|.
//  * The bookmark is created without a destination or action; set one
//    with EPDFBookmark_SetDest() or EPDFBookmark_SetAction().
FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
EPDFBookmark_InsertAfter(FPDF_DOCUMENT document,
                         FPDF_BOOKMARK parent,
                         FPDF_BOOKMARK after_sibling,
                         FPDF_WIDESTRING title);

// Experimental EmbedPDF Extension API.
// Set a bookmark's UTF-16LE title.
//
//   bookmark - handle to the bookmark.
//   title    - UTF-16LE encoded title string; may be empty.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFBookmark_SetTitle(FPDF_BOOKMARK bookmark, FPDF_WIDESTRING title);

// Experimental EmbedPDF Extension API.
// Set the target of |bookmark| to |dest| (clears any existing action).
//
//   document - handle to the document that owns |bookmark| and |dest|.
//   bookmark - handle to the bookmark.
//   dest     - handle to an in-document destination (created via EPDFDest_*()).
//
// Returns true on success.
//
// Notes:
//  * On success, /Dest is set to an indirect reference to |dest| and any /A is
//    removed.
//  * |dest| must belong to |document| and be indirect; otherwise the call
//  fails.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFBookmark_SetDest(FPDF_DOCUMENT document,
                                                         FPDF_BOOKMARK bookmark,
                                                         FPDF_DEST dest);

// Experimental EmbedPDF Extension API.
// Set the target of |bookmark| to |action| (clears any existing destination).
//
//   document - handle to the document that owns |bookmark| and |action|.
//   bookmark - handle to the bookmark.
//   action   - handle to an action dictionary (e.g. from EPDFAction_Create*()).
//
// Returns true on success.
//
// Notes:
//  * On success, /A is set to an indirect reference to |action| and any /Dest
//    is removed.
//  * |action| must belong to |document| and be indirect; otherwise the call
//  fails.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFBookmark_SetAction(FPDF_DOCUMENT document,
                       FPDF_BOOKMARK bookmark,
                       FPDF_ACTION action);

// Experimental EmbedPDF Extension API.
// Clear any target from |bookmark| (removes both /Dest and /A).
//
//   bookmark - handle to the bookmark.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFBookmark_ClearTarget(FPDF_BOOKMARK bookmark);

// Experimental EmbedPDF Outline API.
// Clear all bookmarks from the document.
//
//   document - handle to the document.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFBookmark_Clear(FPDF_DOCUMENT document);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_FPDF_DOC_H_
