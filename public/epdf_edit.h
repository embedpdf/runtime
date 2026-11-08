// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_EDIT_H_
#define PUBLIC_EPDF_EDIT_H_

// NOLINTNEXTLINE(build/include)
#include "fpdf_edit.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// EmbedPDF's additions to public/fpdf_edit.h: blank pages without loading the
// page, PNG and JPEG image objects, text redaction, and page-object state.

// Experimental EmbedPDF Extension API.
// Read the object's non-stroking alpha, including for images and shadings
// that have no color state. This is the object's graphics-state value, not
// a composited opacity after soft masks or enclosing transparency groups.
//
//   page_object - handle to a page object.
//   alpha       - receives the alpha (0 = transparent, 1 = opaque).
//
// Returns TRUE on success. Returns FALSE for a NULL handle or output pointer,
// leaving the output unchanged. Does not modify the object.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPageObj_GetFillAlpha(FPDF_PAGEOBJECT page_object, float* alpha);

// Experimental EmbedPDF Extension API.
// Read the object's stroking alpha. The output, failure, and ownership
// semantics are the same as EPDFPageObj_GetFillAlpha(). The value does not
// imply that the object actually strokes any content.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPageObj_GetStrokeAlpha(FPDF_PAGEOBJECT page_object, float* alpha);

// Experimental EmbedPDF Extension API.
// Read the object's resolved blend-mode name, using the names accepted by
// FPDFPageObj_SetBlendMode(). The default is "Normal".
//
//   page_object - handle to a page object.
//   buffer      - caller-owned buffer, or NULL to query the required size.
//   buflen      - size of |buffer| in bytes.
//
// Returns the required byte count including the terminating NUL, or 0 for a
// NULL object. Copies only when |buffer| is non-NULL and large enough;
// otherwise leaves it unchanged. Does not modify the object.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFPageObj_GetBlendMode(FPDF_PAGEOBJECT page_object,
                         char* buffer,
                         unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Return TRUE if the object's graphics state contains a soft mask, FALSE
// otherwise (including a NULL object). This does not report an image's own
// /SMask or masks applied by enclosing objects. Does not modify the object.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPageObj_HasSoftMask(FPDF_PAGEOBJECT page_object);

// Experimental EmbedPDF Extension API.
// Read a Form XObject's transparency-group state.
//
//   form_object - handle to a form page object.
//   is_group    - receives TRUE for a transparency group, FALSE otherwise.
//   is_isolated - receives TRUE for an isolated transparency group, FALSE
//                 otherwise, including an ordinary form.
//
// Returns TRUE on success. Returns FALSE for a NULL/non-form object or NULL
// output pointer, leaving both outputs unchanged. Does not modify the form.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFormObj_GetTransparencyGroup(FPDF_PAGEOBJECT form_object,
                                 FPDF_BOOL* is_group,
                                 FPDF_BOOL* is_isolated);

// Experimental EmbedPDF Extension API.
// Read the object's miter limit: the maximum ratio of miter length to stroke
// width. The PDF default is 10. The value does not imply a miter join is used.
//
//   page_object - handle to a page object.
//   miter_limit - receives the graphics-state miter limit.
//
// Returns TRUE on success. Returns FALSE for a NULL handle or output pointer,
// leaving the output unchanged. Does not modify the object.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPageObj_GetMiterLimit(FPDF_PAGEOBJECT page_object, float* miter_limit);

// Experimental EmbedPDF Extension API.
// Insert a blank page without loading it: FPDFPage_New() for a caller that
// only writes the page into the document.
//
//   document   - handle to document.
//   page_index - the 0-based index the new page gets, from 0 to the page
//                count (append). Out of range is refused, never clamped.
//   width      - the page width in points.
//   height     - the page height in points.
//   objnum     - the object number the page gets, or 0 for the next free one.
//                A number other than 0 needs a layer transaction and must be
//                free (EPDFLayer_RaiseLastObjectNumber()).
//
// Returns TRUE when the page was inserted; FALSE otherwise, and then nothing
// changed.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_InsertBlankRaw(FPDF_DOCUMENT document,
                        int page_index,
                        double width,
                        double height,
                        unsigned long objnum);

// Set a PNG image on |image_object|. Decodes the PNG buffer, applies
// FlateDecode with PNG prediction filters for optimal compression, and
// handles alpha transparency via an SMask stream.
//
//   pages        - pointer to the start of all loaded pages, may be NULL.
//   count        - number of |pages|, may be 0.
//   image_object - handle to an image object.
//   png_data     - pointer to the raw PNG file bytes.
//   png_size     - size in bytes of the PNG data.
//
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFImageObj_SetPng(FPDF_PAGE* pages,
                    int count,
                    FPDF_PAGEOBJECT image_object,
                    const uint8_t* png_data,
                    size_t png_size);

// Set a JPEG image on |image_object| from a raw buffer. Embeds the JPEG
// data inline (copied into the document) using DCTDecode -- no re-encoding.
//
//   pages        - pointer to the start of all loaded pages, may be NULL.
//   count        - number of |pages|, may be 0.
//   image_object - handle to an image object.
//   jpeg_data    - pointer to the raw JPEG file bytes.
//   jpeg_size    - size in bytes of the JPEG data.
//
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFImageObj_SetJpeg(FPDF_PAGE* pages,
                     int count,
                     FPDF_PAGEOBJECT image_object,
                     const uint8_t* jpeg_data,
                     size_t jpeg_size);

// Experimental API.
//
// Redact text in a given rectangle on a page.
//
//   page          - handle to a page.
//   rect          - handle to a rectangle.
//   recurse_forms - whether to recurse into form objects.
//
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFText_RedactInRect(FPDF_PAGE page,
                      const FS_RECTF* rect,
                      FPDF_BOOL recurse_forms,
                      FPDF_BOOL draw_black_boxes);

// Experimental API.
//
// Redact text in a given quads on a page.
//
//   page          - handle to a page.
//   quads         - handle to a quads.
//   count         - the number of quads.
//   recurse_forms - whether to recurse into form objects.
//
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFText_RedactInQuads(FPDF_PAGE page,
                       const FS_QUADPOINTSF* quads,
                       size_t count,
                       FPDF_BOOL recurse_forms,
                       FPDF_BOOL draw_black_boxes);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_EDIT_H_
