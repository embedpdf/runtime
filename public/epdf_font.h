// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_FONT_H_
#define PUBLIC_EPDF_FONT_H_

#include <stddef.h>
#include <stdint.h>

// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Experimental EmbedPDF Extension API.
typedef uint32_t EPDF_FONT_ID;

// Experimental EmbedPDF Extension API.
// Register a font for runtime fallback use and PDF authoring from file access.
// Font registration follows PDFium's normal handle/thread ownership model. In
// TLS builds, register and use fonts on the initialized worker thread that owns
// the document/page handles, and call EPDF_ShutdownThread() on that worker to
// release registered font state. In non-TLS builds, do not mutate the registry
// concurrently with rendering, saving, or editing.
//
//   family_name  - optional family/resource base name. Pass NULL or "" to
//                  infer from the font.
//   weight       - style weight for matching. Pass 0 to infer from the font.
//   italic       - style italic flag for matching. Pass -1 to infer from the
//                  font, 0 for non-italic, or 1 for italic.
//   file_access  - font bytes as FPDF_FILEACCESS. The underlying file resources
//                  must remain valid until EPDFFont_ClearRegisteredFonts() or
//                  PDFium shutdown. The FPDF_FILEACCESS struct itself may be
//                  stack-owned.
//
// Returns a non-zero font id on success, or 0 on failure.
FPDF_EXPORT EPDF_FONT_ID FPDF_CALLCONV
EPDFFont_RegisterFont(FPDF_BYTESTRING family_name,
                      int weight,
                      int italic,
                      FPDF_FILEACCESS* file_access);

// Experimental EmbedPDF Extension API.
// Register an in-memory font for runtime fallback use and PDF authoring.
//
//   family_name - optional family/resource base name. Pass NULL or "" to infer
//                 from the font.
//   weight      - style weight for matching. Pass 0 to infer from the font.
//   italic      - style italic flag for matching. Pass -1 to infer from the
//                 font, 0 for non-italic, or 1 for italic.
//   data_buf    - pointer to font bytes.
//   size        - size of |data_buf| in bytes.
//
// Returns a non-zero font id on success, or 0 on failure.
FPDF_EXPORT EPDF_FONT_ID FPDF_CALLCONV
EPDFFont_RegisterMemFont(FPDF_BYTESTRING family_name,
                         int weight,
                         int italic,
                         const void* data_buf,
                         int size);

// Experimental EmbedPDF Extension API.
// Same as EPDFFont_RegisterMemFont(), but supports size_t byte counts.
FPDF_EXPORT EPDF_FONT_ID FPDF_CALLCONV
EPDFFont_RegisterMemFont64(FPDF_BYTESTRING family_name,
                           int weight,
                           int italic,
                           const void* data_buf,
                           size_t size);

// Experimental EmbedPDF Extension API.
// Clear all registered fonts and the fallback font order.
//
// Existing documents may still contain registered-font DA marker resources
// after this call; those markers are invalid until their fonts are registered
// again.
FPDF_EXPORT void FPDF_CALLCONV EPDFFont_ClearRegisteredFonts(void);

// EmbedPDF: the OS/2 fsType embedding permission of a registered font.
// Registration refuses RESTRICTED and BITMAP_ONLY fonts (EPDFFont_Register*
// returns 0 for them). PREVIEW_AND_PRINT fonts render existing text and may
// be embedded, but may not author new text (EPDFAnnot_SetDefaultAppearance-
// RegisteredFont fails, fallback lookups for new text skip them) until the
// app asserts a licence with EPDFFont_AuthorizeEditing().
#define EPDF_FONT_EMBEDDING_INSTALLABLE 0
#define EPDF_FONT_EMBEDDING_EDITABLE 1
#define EPDF_FONT_EMBEDDING_PREVIEW_AND_PRINT 2
#define EPDF_FONT_EMBEDDING_RESTRICTED 3
#define EPDF_FONT_EMBEDDING_BITMAP_ONLY 4

// Returns one of EPDF_FONT_EMBEDDING_*, or -1 for an unknown id.
FPDF_EXPORT int FPDF_CALLCONV
EPDFFont_GetEmbeddingPermission(EPDF_FONT_ID font_id);

// Whether new text may be authored with the font (see above).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFont_IsEditingAuthorized(EPDF_FONT_ID font_id);

// The application asserts it holds a licence that permits editing with this
// font. Returns false for an unknown id.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFont_AuthorizeEditing(EPDF_FONT_ID font_id);

// True when the registered program is a static instance made from a variable
// font at registration (axes pinned to defaults, `wght` to the registered
// weight when the axis covers it).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFFont_IsInstanced(EPDF_FONT_ID font_id);

// Experimental EmbedPDF Extension API.
// Add a registered font to the ordered fallback list used when the selected
// font does not contain a glyph or a PDF page needs a substitute font.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFont_AddFallbackFont(EPDF_FONT_ID font_id);

// Experimental EmbedPDF Extension API.
// Clear the ordered fallback font list without unregistering fonts.
FPDF_EXPORT void FPDF_CALLCONV EPDFFont_ClearFallbackFonts(void);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_FONT_H_
