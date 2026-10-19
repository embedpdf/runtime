// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_ANNOT_H_
#define PUBLIC_EPDF_ANNOT_H_

// NOLINTNEXTLINE(build/include)
#include "fpdf_annot.h"

// NOLINTNEXTLINE(build/include)
#include "epdf_font.h"

// NOLINTNEXTLINE(build/include)
#include "epdf_redact.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// EmbedPDF's annotation API: the EPDFAnnot_*, EPDFPage_* and EPDFDoc_*
// functions that extend public/fpdf_annot.h, and their types.

typedef enum FPDF_ANNOT_BORDER_STYLE {
  FPDF_ANNOT_BS_UNKNOWN = 0,
  FPDF_ANNOT_BS_SOLID,
  FPDF_ANNOT_BS_DASHED,
  FPDF_ANNOT_BS_BEVELED,
  FPDF_ANNOT_BS_INSET,
  FPDF_ANNOT_BS_UNDERLINE,
  FPDF_ANNOT_BS_CLOUDY
} FPDF_ANNOT_BORDER_STYLE;

typedef enum FPDF_BLENDMODE {
  FPDF_BLENDMODE_Normal = 0,
  FPDF_BLENDMODE_Multiply,
  FPDF_BLENDMODE_Screen,
  FPDF_BLENDMODE_Overlay,
  FPDF_BLENDMODE_Darken,
  FPDF_BLENDMODE_Lighten,
  FPDF_BLENDMODE_ColorDodge,
  FPDF_BLENDMODE_ColorBurn,
  FPDF_BLENDMODE_HardLight,
  FPDF_BLENDMODE_SoftLight,
  FPDF_BLENDMODE_Difference,
  FPDF_BLENDMODE_Exclusion,
  FPDF_BLENDMODE_Hue,
  FPDF_BLENDMODE_Saturation,
  FPDF_BLENDMODE_Color,
  FPDF_BLENDMODE_Luminosity,
  FPDF_BLENDMODE_LAST = FPDF_BLENDMODE_Luminosity
} FPDF_BLENDMODE;

typedef enum FPDF_ANNOT_LINE_END {
  FPDF_ANNOT_LE_None = 0,
  FPDF_ANNOT_LE_Square,
  FPDF_ANNOT_LE_Circle,
  FPDF_ANNOT_LE_Diamond,
  FPDF_ANNOT_LE_OpenArrow,
  FPDF_ANNOT_LE_ClosedArrow,
  FPDF_ANNOT_LE_Butt,
  FPDF_ANNOT_LE_ROpenArrow,
  FPDF_ANNOT_LE_RClosedArrow,
  FPDF_ANNOT_LE_Slash,
  FPDF_ANNOT_LE_Unknown
} FPDF_ANNOT_LINE_END;

typedef enum FPDF_STANDARD_FONT {
  FPDF_FONT_UNKNOWN = -1,
  FPDF_FONT_COURIER = 0,
  FPDF_FONT_COURIER_BOLD,
  FPDF_FONT_COURIER_BOLDITALIC,
  FPDF_FONT_COURIER_ITALIC,
  FPDF_FONT_HELVETICA,
  FPDF_FONT_HELVETICA_BOLD,
  FPDF_FONT_HELVETICA_BOLDITALIC,
  FPDF_FONT_HELVETICA_ITALIC,
  FPDF_FONT_TIMES_ROMAN,
  FPDF_FONT_TIMES_BOLD,
  FPDF_FONT_TIMES_BOLDITALIC,
  FPDF_FONT_TIMES_ITALIC,
  FPDF_FONT_SYMBOL,
  FPDF_FONT_ZAPFDINGBATS
} FPDF_STANDARD_FONT;

typedef enum FPDF_TEXT_ALIGNMENT {
  FPDF_TEXT_ALIGNMENT_LEFT = 0,
  FPDF_TEXT_ALIGNMENT_CENTER = 1,
  FPDF_TEXT_ALIGNMENT_RIGHT = 2
} FPDF_TEXT_ALIGNMENT;

typedef enum FPDF_VERTICAL_ALIGNMENT {
  FPDF_VERTICAL_ALIGNMENT_TOP = 0,
  FPDF_VERTICAL_ALIGNMENT_MIDDLE = 1,
  FPDF_VERTICAL_ALIGNMENT_BOTTOM = 2
} FPDF_VERTICAL_ALIGNMENT;

typedef enum EPDF_STAMP_FIT {
  EPDF_STAMP_FIT_CONTAIN = 0,  // preserve aspect, fully visible
  EPDF_STAMP_FIT_COVER = 1,    // preserve aspect, fill box, might crop
  EPDF_STAMP_FIT_STRETCH = 2   // ignore aspect, fill box
} EPDF_STAMP_FIT;

// Reply Type (RT) for annotations that reply to other annotations via IRT.
// See ISO 32000-2, section 12.5.6.
typedef enum FPDF_ANNOT_REPLY_TYPE {
  FPDF_ANNOT_RT_UNKNOWN = 0,  // Unknown or invalid
  FPDF_ANNOT_RT_REPLY,        // /R - comment reply (default if RT missing)
  FPDF_ANNOT_RT_GROUP         // /Group - logical grouping
} FPDF_ANNOT_REPLY_TYPE;

// Experimental EmbedPDF Extension API.
// Set |key| in |annot|'s dictionary to the number |value|, the counterpart of
// FPDFAnnot_GetNumberValue(). A whole number is written as a PDF integer, so
// integer entries such as /MaxLen stay integers; any other value is written as
// a real.
//
//   annot - handle to an annotation.
//   key   - the key to set, encoded in UTF-8. Must not be NULL or empty.
//   value - the number. Must be finite.
//
// Returns true on success. A refused call changes nothing.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetNumberValue(FPDF_ANNOTATION annot,
                         FPDF_BYTESTRING key,
                         float value);

// Experimental EmbedPDF Extension API.
// Returns true if |annot| has an /EMBD_Metadata dictionary.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_HasEmbedMetadata(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Removes the /EMBD_Metadata dictionary from |annot|.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearEmbedMetadata(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Removes |key| from |annot|'s /EMBD_Metadata dictionary. If the metadata
// dictionary becomes empty, /EMBD_Metadata is removed too.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearEmbedMetadataKey(FPDF_ANNOTATION annot, FPDF_BYTESTRING key);

// Experimental EmbedPDF Extension API.
// Set a UTF-16LE string in |annot|'s /EMBD_Metadata dictionary.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetEmbedMetadataString(FPDF_ANNOTATION annot,
                                 FPDF_BYTESTRING key,
                                 FPDF_WIDESTRING value);

// Experimental EmbedPDF Extension API.
// Get a UTF-16LE string from |annot|'s /EMBD_Metadata dictionary.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetEmbedMetadataString(FPDF_ANNOTATION annot,
                                 FPDF_BYTESTRING key,
                                 FPDF_WCHAR* buffer,
                                 unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Set a number in |annot|'s /EMBD_Metadata dictionary.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetEmbedMetadataNumber(FPDF_ANNOTATION annot,
                                 FPDF_BYTESTRING key,
                                 float value);

// Experimental EmbedPDF Extension API.
// Get a number from |annot|'s /EMBD_Metadata dictionary.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetEmbedMetadataNumber(FPDF_ANNOTATION annot,
                                 FPDF_BYTESTRING key,
                                 float* value);

// Experimental EmbedPDF Extension API.
// Set a boolean in |annot|'s /EMBD_Metadata dictionary.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetEmbedMetadataBoolean(FPDF_ANNOTATION annot,
                                  FPDF_BYTESTRING key,
                                  FPDF_BOOL value);

// Experimental EmbedPDF Extension API.
// Get a boolean from |annot|'s /EMBD_Metadata dictionary.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetEmbedMetadataBoolean(FPDF_ANNOTATION annot,
                                  FPDF_BYTESTRING key,
                                  FPDF_BOOL* value);

// Experimental EmbedPDF Extension API.
// Set a rect in |annot|'s /EMBD_Metadata dictionary.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetEmbedMetadataRect(FPDF_ANNOTATION annot,
                               FPDF_BYTESTRING key,
                               const FS_RECTF* rect);

// Experimental EmbedPDF Extension API.
// Get a rect from |annot|'s /EMBD_Metadata dictionary.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetEmbedMetadataRect(FPDF_ANNOTATION annot,
                               FPDF_BYTESTRING key,
                               FS_RECTF* rect);

// Experimental EmbedPDF Extension API.
// Set the arbitrary app-specific JSON bucket in /EMBD_Metadata /CustomJSON.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetEmbedMetadataJSON(FPDF_ANNOTATION annot, FPDF_WIDESTRING json);

// Experimental EmbedPDF Extension API.
// Get the arbitrary app-specific JSON bucket from /EMBD_Metadata /CustomJSON.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetEmbedMetadataJSON(FPDF_ANNOTATION annot,
                               FPDF_WCHAR* buffer,
                               unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Removes /EMBD_Metadata from every annotation and every form field in
// |document|: the fields in the field tree, and those a widget's /Parent chain
// reaches. This does not remove standard annotation fields or appearance
// streams.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDocument_ClearEmbedMetadata(FPDF_DOCUMENT document);

// Experimental EmbedPDF Extension API.
// FPDFAnnot_GetFileAttachment() for writing: the attachment as the document
// opens it for writing, so a layer transaction copies it up first. Use this
// to change an attachment the document already has; the handle
// FPDFAnnot_GetFileAttachment() returns is for reading, and inside a layer
// transaction the FPDFAttachment_Set*() and EPDFAttachment_Set*() calls
// refuse it.
//
//   annot - handle to a file annotation.
//
// Returns the handle to the attachment object, or NULL on failure.
FPDF_EXPORT FPDF_ATTACHMENT FPDF_CALLCONV
EPDFAnnot_GetFileAttachmentForWrite(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Get the color of an annotation. If no color is specified, default to yellow
// for highlight annotation, black for all else.
//
//   annot    - handle to an annotation.
//   type     - type of the color requested.
//   R, G, B  - buffer to hold the RGB value of the color. Ranges from 0 to 255.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_GetColor(FPDF_ANNOTATION annot,
                                                       FPDFANNOT_COLORTYPE type,
                                                       unsigned int* R,
                                                       unsigned int* G,
                                                       unsigned int* B);

// Experimental EmbedPDF Extension API.
// Set the color of an annotation.
//
//   annot    - handle to an annotation.
//   type     - type of the color to be set.
//   R, G, B  - buffer to hold the RGB value of the color. Ranges from 0 to 255.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetColor(FPDF_ANNOTATION annot,
                                                       FPDFANNOT_COLORTYPE type,
                                                       unsigned int R,
                                                       unsigned int G,
                                                       unsigned int B);

// Experimental EmbedPDF Extension API.
// Set the opacity of an annotation: /CA, stored as given. 1 removes /CA.
//
// annot   - handle to an annotation.
// opacity - the opacity, 0 (transparent) to 1 (opaque).
//
// Returns true if successful; false for a value outside 0..1.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetOpacity(FPDF_ANNOTATION annot, float opacity);

// Experimental EmbedPDF Extension API.
// Get the opacity of an annotation: /CA as stored, clamped to 0..1; 1 when
// absent.
//
// annot   - handle to an annotation.
// opacity - receives the opacity.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetOpacity(FPDF_ANNOTATION annot, float* opacity);

// Experimental EmbedPDF Extension API.
// Clear the color of an annotation.
//
//   annot    - handle to an annotation.
//   type     - type of the color to be cleared.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearColor(FPDF_ANNOTATION annot, FPDFANNOT_COLORTYPE type);

// Experimental EmbedPDF Extension API.
// Get the border style and width of an annotation. This function handles both
// the modern /BS dictionary and the legacy /Border array.
//
//   annot  - handle to an annotation.
//   width  - receives the border width; can be NULL.
//
// Returns the border style enum.
FPDF_EXPORT FPDF_ANNOT_BORDER_STYLE FPDF_CALLCONV
EPDFAnnot_GetBorderStyle(FPDF_ANNOTATION annot, float* width);

// Experimental EmbedPDF Extension API.
// Set the border style and width of an annotation.
//
//   annot  - handle to an annotation.
//   width  - the border width to be set.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetBorderStyle(FPDF_ANNOTATION annot,
                         FPDF_ANNOT_BORDER_STYLE style,
                         float width);

// Experimental EmbedPDF Extension API.
// Get the intensity of a cloudy border effect.
//
//   annot     - handle to an annotation.
//   intensity - receives the effect intensity, typically 1 or 2.
//
// Returns true if the annotation has a cloudy border effect, false otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetBorderEffect(FPDF_ANNOTATION annot, float* intensity);

// Experimental EmbedPDF Extension API.
// Set a cloudy border effect (/BE dictionary with S=/C) on a supported
// annotation.
//
//   annot     - handle to a square, circle, polygon, or free-text annotation.
//   intensity - the cloudy effect intensity, typically 1.0 or 2.0.
//
// Returns true on success, false on failure or unsupported subtype.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetBorderEffect(FPDF_ANNOTATION annot, float intensity);

// Experimental EmbedPDF Extension API.
// Remove the border effect (/BE dictionary) from a supported annotation.
//
//   annot  - handle to a square, circle, polygon, or free-text annotation.
//
// Returns true on success, false on failure or unsupported subtype.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearBorderEffect(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Get the rectangle differences (/RD) — the inset between an annotation's
// /Rect and its drawn appearance — for a supported annotation.
//
//   annot            - handle to a square, circle, caret, free-text, or polygon
//                      annotation.
//   left, bottom,    - receive the native PDF /RD values for each side.
//   right, top         PDFium core treats /RD as [left, bottom, right, top].
//
// Returns true if the annotation has an /RD entry, false otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetRectangleDifferences(FPDF_ANNOTATION annot,
                                  float* left,
                                  float* bottom,
                                  float* right,
                                  float* top);

// Experimental EmbedPDF Extension API.
// Remove the rectangle differences (/RD) entry from a supported annotation.
// Generating the appearance of a square, circle, caret or free text writes
// /RD again when what it draws reaches past the shape (a cloudy border's
// bumps, a callout's line): /Rect less /RD is the shape.
//
//   annot  - handle to a square, circle, caret, free-text, or polygon
//            annotation.
//
// Returns true on success, false on failure or unsupported subtype.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearRectangleDifferences(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Get the number of entries in the dash pattern a border stores: /BS /D
// whatever the border's style (only a dashed border draws it), or the legacy
// /Border array's dash array when there is no /BS.
//
//   annot  - handle to an annotation.
//
// Returns the number of entries in the dash pattern array, or 0 if the border
// stores none.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetBorderDashPatternCount(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Get the dash pattern a border stores, as EPDFAnnot_GetBorderDashPatternCount()
// counts it.
//
//   annot      - handle to an annotation.
//   dash_array - a buffer to receive the dash pattern values.
//   count      - the number of entries in the buffer, obtained from
//                EPDFAnnot_GetBorderDashPatternCount().
//
// Returns true if the dash pattern was successfully retrieved, false otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetBorderDashPattern(FPDF_ANNOTATION annot,
                               float* dash_array,
                               unsigned long count);

// Experimental EmbedPDF Extension API.
// Sets (or replaces) the dash pattern on an annotation's border (/BS /D).
// The border style (/BS /S) is left as it is: only a dashed border draws the
// pattern. A NULL |dash_array| or a |count| of 0 removes the pattern, and a
// dashed border then draws the default dash [3].
//
//   annot      - handle to an annotation.
//   dash_array - the dash pattern to be set.
//   count      - the number of entries in the buffer, obtained from
//                EPDFAnnot_GetBorderDashPatternCount().
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetBorderDashPattern(FPDF_ANNOTATION annot,
                               const float* dash_array,
                               unsigned long count);

// Experimental EmbedPDF Extension API.
// Generates or regenerates the appearance stream for a given annotation
// using PDFium's internal AP generation engine. This is the most reliable way
// to create a standard-compliant appearance after modifying an annotation's
// properties like color, quads, etc.
//
//   annot  - handle to an annotation.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GenerateAppearance(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Generates or regenerates the appearance stream for a given annotation
// using PDFium's internal AP generation engine. This is the most reliable way
// to create a standard-compliant appearance after modifying an annotation's
// properties like color, quads, etc.
//
//   annot  - handle to an annotation.
//   blend  - the blend mode to use.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GenerateAppearanceWithBlend(FPDF_ANNOTATION annot,
                                      FPDF_BLENDMODE blend);

// Returns the effective blend mode used by the annotation's *normal*
// appearance stream. If the annotation has no appearance stream and is a
// Highlight annotation, returns Multiply (typical synthesized behavior).
// Returns FPDF_BLENDMODE_Normal on error.
FPDF_EXPORT FPDF_BLENDMODE FPDF_CALLCONV
EPDFAnnot_GetBlendMode(FPDF_ANNOTATION annot);

// Sets the Intent (/IT) *name* of an annotation. The value must be a non-empty
// ASCII byte string *without* the leading slash. If the caller includes a
// leading '/', it is stripped. Returns false on invalid input or failure.
// Succeeds regardless of subtype (permissive; /IT is just metadata).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetIntent(FPDF_ANNOTATION annot,
                                                        FPDF_BYTESTRING intent);

// Retrieves the Intent (/IT) name of an annotation as UTF-16 (without a
// leading slash). Returns the number of 16-bit code units required (excluding
// terminating NUL). If `buffer` is non-null and `buflen` large enough, copies
// the UTF-16 data and NUL-terminates it (same pattern as
// FPDFAnnot_GetStringValue). Returns 0 if annotation invalid, no /IT entry, or
// empty.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetIntent(FPDF_ANNOTATION annot,
                    FPDF_WCHAR* buffer,
                    unsigned long buflen);

// Get the rich (formatted) text stored in the annotation’s /RC entry.
// Returns the number of 16‑bit characters required (including the
// terminating NUL).  Call once with `buffer == nullptr` to get the size.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetRichContent(FPDF_ANNOTATION annot,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen);

// Experimental EmbedPDF Extension API.
// The rich text of a FreeText annotation (/RC over /DS and /DA) or of a rich
// text field (/RV), as UTF-8 JSON: {"source":"rc"|"contents","body":{...},
// "paragraphs":[{"align","dir","runs":[{"text","style":{deltas}}]}],
// "diagnostics":[{"code","detail"}]}. When there is no rich text the document
// is synthesised from /Contents (one paragraph per line break) and "source" is
// "contents", so callers always get one model. Read-only.
//
//   annot  - handle to an annotation.
//   buffer - buffer for the UTF-8 JSON, NUL-terminated. May be NULL.
//   buflen - length of the buffer in bytes.
//
// Returns the number of bytes needed including the NUL, or 0 on error. If
// |buflen| is smaller than that, nothing is copied.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetRichTextJSON(FPDF_ANNOTATION annot,
                          char* buffer,
                          unsigned long buflen);

// Experimental EmbedPDF Extension API (rich text, Phase D).
// Replace the annotation's rich text with |json_utf8|, the shape
// EPDFAnnot_GetRichTextJSON() writes ("source" and "diagnostics" ignored;
// "body" optional, in which case the annotation's current body style is
// kept). Writes /RC, /DS, /DA and /Contents and regenerates the appearance
// through the rich layout engine, all of it or nothing: invalid JSON, an
// empty rect, or a font resource that cannot be built return false and
// change nothing. An unresolvable family is not a failure: it substitutes
// and the run reads back "degraded".
//
//   annot     - handle to a FreeText annotation.
//   json_utf8 - the document, UTF-8 JSON.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetRichTextJSON(FPDF_ANNOTATION annot, FPDF_BYTESTRING json_utf8);

// Experimental EmbedPDF Extension API (rich text, Phase D).
// Import raw XHTML (the XFA rich text subset, e.g. another producer's /RC)
// over the annotation's DA and DS defaults; same effects and failures as
// EPDFAnnot_SetRichTextJSON(). Malformed XML returns false.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetRichTextXHTML(FPDF_ANNOTATION annot, FPDF_WIDESTRING xhtml);

// Experimental EmbedPDF Extension API (rich text, Phase D).
// Latin typographic features (kern, liga, clig, calt, dlig) when shaping
// rich text. Off by default: Acrobat's appearances show plain advance
// widths and no ligatures, and parity with what Acrobat draws wins. Session
// state of the document handle; applies to appearances generated after
// the call.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_SetTypographicFeatures(FPDF_DOCUMENT document, FPDF_BOOL enabled);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_GetTypographicFeatures(FPDF_DOCUMENT document);

// Experimental EmbedPDF Extension API.
// Set the line endings of a Line, Polyline, or FreeText annotation.
// For Line/Polyline: writes /LE as a 2-element array [start_style, end_style].
// For FreeText: writes /LE as a single name using end_style (Acrobat
// convention); start_style is ignored.
//
//   annot       - handle to an annotation.
//   start_style - the start line ending style (ignored for FreeText).
//   end_style   - the end line ending style.
//
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetLineEndings(FPDF_ANNOTATION annot,
                         FPDF_ANNOT_LINE_END start_style,
                         FPDF_ANNOT_LINE_END end_style);

// Experimental EmbedPDF Extension API.
// Get the line endings of a Line, Polyline, or FreeText annotation.
// Handles both 2-element arrays and single name values (Acrobat FreeText).
// When /LE is a single name, start_style is set to FPDF_ANNOT_LE_None.
//
//   annot       - handle to an annotation.
//   start_style - receives the start line ending style.
//   end_style   - receives the end line ending style.
//
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetLineEndings(FPDF_ANNOTATION annot,
                         FPDF_ANNOT_LINE_END* start_style,
                         FPDF_ANNOT_LINE_END* end_style);

// Experimental EmbedPDF Extension API.
// Replace every vertex in the /Vertices array with the |points| supplied.
// If |points| is nullptr or |count| is 0, the array is removed.
//
//   annot    - handle to an annotation.
//   points   - the vertices to be set.
//   count    - the number of vertices to be set.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetVertices(FPDF_ANNOTATION annot,
                      const FS_POINTF* points,
                      unsigned long count);

// Experimental EmbedPDF Extension API.
// Set (or create) the two end‑points of a **Line** annotation
// by writing a new /L array `[ x1 y1 x2 y2 ]`.
//
//   annot    - handle to an annotation.
//   start    - pointer to an `FS_POINTF` holding the new start‑point.
//   end      - pointer to an `FS_POINTF` holding the new end‑point.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetLine(FPDF_ANNOTATION annot,
                                                      const FS_POINTF* start,
                                                      const FS_POINTF* end);

// Experimental EmbedPDF Extension API.
// Set the default appearance of a FreeText annotation.
//
//   annot    - handle to an annotation.
//   font     - the font to be set.
//   font_size - the font size to be set.
//   R, G, B  - the color to be set.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetDefaultAppearance(FPDF_ANNOTATION annot,
                               FPDF_STANDARD_FONT font,
                               float font_size,
                               unsigned int R,
                               unsigned int G,
                               unsigned int B);

// Experimental EmbedPDF Extension API.
// Set the default appearance of a FreeText annotation using a registered font.
//
//   annot     - handle to an annotation.
//   font_id   - font id returned by EPDFFont_RegisterFont() or
//               EPDFFont_RegisterMemFont64().
//   font_size - the font size to be set.
//   R, G, B   - the color to be set.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetDefaultAppearanceRegisteredFont(FPDF_ANNOTATION annot,
                                             EPDF_FONT_ID font_id,
                                             float font_size,
                                             unsigned int R,
                                             unsigned int G,
                                             unsigned int B);

// Experimental EmbedPDF Extension API.
// Set the default appearance of a FreeText, Redact or Widget annotation to a
// font program the document already holds, by the object number of its
// stream (EPDFFontFaces_GetProgramObjNum()). /DA names an alias for it this
// session; the appearance generated next installs its /DR entry. Fails for
// a program the layout can't write new text with: not TrueType or OpenType,
// no Unicode cmap, or a licence (OS/2 fsType) that doesn't allow editing.
//
//   annot          - handle to an annotation.
//   program_objnum - the font program stream's object number.
//   font_size      - the font size to be set.
//   R, G, B        - the color to be set.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetDefaultAppearanceDocumentFont(FPDF_ANNOTATION annot,
                                           unsigned long program_objnum,
                                           float font_size,
                                           unsigned int R,
                                           unsigned int G,
                                           unsigned int B);

// Experimental EmbedPDF Extension API.
// Get the default appearance of a FreeText annotation.
//
//   annot    - handle to an annotation.
//   font     - receives the font.
//   font_size - receives the font size.
//   R, G, B  - receives the color.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetDefaultAppearance(FPDF_ANNOTATION annot,
                               FPDF_STANDARD_FONT* font,
                               float* font_size,
                               unsigned int* R,
                               unsigned int* G,
                               unsigned int* B);

// Experimental EmbedPDF Extension API.
// Get the face of the font a FreeText, Redact or Widget annotation's default
// appearance (/DA) names: its family, weight and italic, as the font's
// descriptor and /BaseFont say (the subset tag and the style dropped from the
// family). The font is looked up in the default resources (/DR, the field's
// and then the form's); a name no /DR holds is a registered font's alias, a
// standard alias (Helv is Helvetica), or the family itself.
//
//   annot  - handle to an annotation.
//   family - receives the family as UTF-16LE with a trailing NUL; may be
//            NULL to query the length.
//   buflen - the size of |family| in bytes.
//   weight - receives the weight, 100 to 900.
//   italic - receives whether the face is italic.
//
// Returns the family's length in bytes, including the trailing NUL, or 0
// when /DA names no font.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetDefaultAppearanceFontFace(FPDF_ANNOTATION annot,
                                       FPDF_WCHAR* family,
                                       unsigned long buflen,
                                       int* weight,
                                       FPDF_BOOL* italic);

// Experimental EmbedPDF Extension API.
// Get the PostScript name of the font a FreeText, Redact or Widget
// annotation's default appearance (/DA) names: the /BaseFont of the font
// dictionary /DR holds for it (a Type0 font's descendant's), without its
// subset tag, or the program's for an alias reserved by
// EPDFAnnot_SetDefaultAppearanceDocumentFont().
//
//   annot  - handle to an annotation.
//   buffer - receives the name's bytes and a trailing NUL; may be NULL to
//            query the length.
//   buflen - the size of |buffer| in bytes.
//
// Returns the name's length in bytes, including the trailing NUL, or 0 when
// /DA names no font dictionary (a standard alias, a registered font's alias)
// or the dictionary has no /BaseFont.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetDefaultAppearanceFontPostScriptName(FPDF_ANNOTATION annot,
                                                 char* buffer,
                                                 unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Set the text alignment (/Q) of a FreeText, Redact or Widget annotation. A
// widget's own /Q wins over its field's.
//
//   annot    - handle to an annotation.
//   alignment - the text alignment to be set.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetTextAlignment(FPDF_ANNOTATION annot,
                           FPDF_TEXT_ALIGNMENT alignment);

// Experimental EmbedPDF Extension API.
// Get the text alignment (/Q) of a FreeText, Redact or Widget annotation. A
// widget without its own /Q takes its field's (inherited), then the
// interactive form's.
//
//   annot    - handle to an annotation.
//
// Returns the text alignment.
FPDF_EXPORT FPDF_TEXT_ALIGNMENT FPDF_CALLCONV
EPDFAnnot_GetTextAlignment(FPDF_ANNOTATION annot);

// Get the annotation by name.
//
//   page    - handle to a page.
//   nm      - the name of the annotation.
//
// Returns the annotation.
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
EPDFPage_GetAnnotByName(FPDF_PAGE page, FPDF_WIDESTRING nm);

// Remove the annotation by name.
//
//   page    - handle to a page.
//   nm      - the name of the annotation.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_RemoveAnnotByName(FPDF_PAGE page, FPDF_WIDESTRING nm);

// Set the linked annotation.

//   annot    - handle to an annotation.
//   key      - the key of the linked annotation.
//   linked_annot - the linked annotation.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetLinkedAnnot(FPDF_ANNOTATION annot,
                         FPDF_BYTESTRING key,
                         FPDF_ANNOTATION linked_annot);

// Experimental EmbedPDF Extension API.
// Set the action of a Link annotation.
//
//   annot  - handle to a link annotation.
//   action - handle to an action dictionary (e.g. from
//   EPDFAction_CreateGoTo()).
//
// Returns true on success.
//
// Notes:
//  * Only valid for FPDF_ANNOT_LINK annotations.
//  * The action must be an indirect object.
//  * Any existing /A entry will be replaced, and any direct /Dest entry is
//    removed — ISO 32000-1 Table 173 forbids a link dictionary carrying
//    both, so setting an action leaves the dictionary spec-clean.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetAction(FPDF_ANNOTATION annot,
                                                        FPDF_ACTION action);

// Experimental EmbedPDF Extension API.
// Remove the /A action entry of a Link annotation.
//
//   annot - handle to a link annotation.
//
// Returns true when the entry is absent afterwards, including when there
// was none to begin with (idempotent). False for non-link annotations.
//
// Notes:
//  * Only valid for FPDF_ANNOT_LINK annotations.
//  * Removes ONLY /A. A link may also carry a direct /Dest — remove it
//    with EPDFAnnot_RemoveDest() when the intent is a target-less link,
//    otherwise the /Dest becomes the link's effective target again.
//  * The removed action dictionary itself is not garbage-collected; like
//    every unreferenced indirect object it is dropped by a full save.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_RemoveAction(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Remove the direct /Dest destination entry of a Link annotation.
//
//   annot - handle to a link annotation.
//
// Returns true when the entry is absent afterwards, including when there
// was none to begin with (idempotent). False for non-link annotations.
//
// Notes:
//  * Only valid for FPDF_ANNOT_LINK annotations.
//  * Removes ONLY /Dest; an /A action entry is left untouched.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_RemoveDest(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Remove |key| from |annot|'s dictionary.
//
//   annot - handle to an annotation.
//   key   - the key to remove, encoded in UTF-8.
//
// Returns true when the entry is absent afterwards, including when there
// was none to begin with (idempotent). False on a null |annot| or |key|.
//
// Notes:
//  * Generic top-level entry removal, completing the FPDFAnnot_HasKey /
//    FPDFAnnot_GetValueType / FPDFAnnot_SetStringValue family. Passing a
//    NULL value to FPDFAnnot_SetStringValue writes an EMPTY string; this
//    function is the only way to truly remove an entry (e.g. /State,
//    /StateModel, /Subj, /Contents).
//  * Like FPDFAnnot_SetStringValue, no key is off-limits — removing a
//    structural entry (/Subtype, /Rect) produces a malformed annotation.
//    Callers are expected to know what they are removing.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_RemoveKey(FPDF_ANNOTATION annot,
                                                        FPDF_BYTESTRING key);

// Experimental EmbedPDF Extension API.
// Get the annotation count.
//
//   doc    - handle to a document.
//   page_index - the index of the page.
//
// Returns the annotation count.
FPDF_EXPORT int FPDF_CALLCONV EPDFPage_GetAnnotCountRaw(FPDF_DOCUMENT doc,
                                                        int page_index);

// Experimental EmbedPDF Extension API.
// Get the annotation by index.
//
//   doc    - handle to a document.
//   page_index - the index of the page.
//   index    - the index of the annotation.
//
// Returns the annotation.
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
EPDFPage_GetAnnotRaw(FPDF_DOCUMENT doc, int page_index, int index);

// Experimental EmbedPDF Extension API.
// Find the annotation named |nm| (its /NM) on a page, without loading or
// parsing the page or opening an annotation: only the page's /Annots and the
// annotation dictionaries are read.
//
//   doc        - handle to a document.
//   page_index - the index of the page.
//   nm         - the name, UTF-16LE, NUL-terminated.
//
// Returns the annotation's index in the page's /Annots (the first, should
// two share the name), or -1 when no annotation on the page has that name,
// |nm| is empty, or the arguments are invalid.
FPDF_EXPORT int FPDF_CALLCONV
EPDFPage_GetAnnotIndexByNameRaw(FPDF_DOCUMENT doc,
                                int page_index,
                                FPDF_WIDESTRING nm);

// Experimental EmbedPDF Extension API.
// Find the annotation that is object |object_number| on a page, without
// loading or parsing the page or opening an annotation: only the page's
// /Annots is read.
//
//   doc           - handle to a document.
//   page_index    - the index of the page.
//   object_number - the annotation dictionary's object number.
//
// Returns the annotation's index in the page's /Annots, or -1 when the page
// has no such annotation (one written in place in /Annots has no object
// number) or the arguments are invalid.
FPDF_EXPORT int FPDF_CALLCONV
EPDFPage_GetAnnotIndexByObjectNumberRaw(FPDF_DOCUMENT doc,
                                        int page_index,
                                        unsigned int object_number);

// Experimental EmbedPDF Extension API.
// Create an annotation of |subtype| on a page without loading or parsing the
// page: a new annotation dictionary, an object of its own whose /P names the
// page, appended to the page's /Annots. EPDFPage_CreateAnnot() on an unparsed
// page, for writing to many pages at once.
//
//   doc        - handle to a document.
//   page_index - the index of the page.
//   subtype    - the subtype of the new annotation.
//   objnum     - the object number the annotation gets, or 0 for the next
//                free one. A number other than 0 needs a layer transaction
//                and must be free (EPDFLayer_RaiseLastObjectNumber()).
//
// Returns a handle to the new annotation, which the caller closes with
// FPDFPage_CloseAnnot(), or NULL on error, including a number that isn't
// free.
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
EPDFPage_CreateAnnotRaw(FPDF_DOCUMENT doc,
                        int page_index,
                        FPDF_ANNOTATION_SUBTYPE subtype,
                        unsigned long objnum);

// Experimental EmbedPDF Extension API.
// Remove the annotation by index.
//
//   doc    - handle to a document.
//   page_index - the index of the page.
//   index    - the index of the annotation.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFPage_RemoveAnnotRaw(FPDF_DOCUMENT doc,
                                                            int page_index,
                                                            int index);

// Experimental EmbedPDF Extension API.
// Promote a page: move every inline annotation dictionary in its /Annots
// into its own indirect object, in place - same content, same order. On a
// layer, the first promotion of a page the base has also records the page's
// births: where each inline annotation the base has went (see
// EPDFLayer_GetBirthObjectNumber()), in the open transaction if there is one.
//
// An inline annotation's permanent name is its position in the base page's
// /Annots (its birth index), and it resolves by position only while every
// one sits at its birth position. So promote a page before any write that
// would move an existing entry of its /Annots: a delete, a reorder, a
// flatten, a redaction, an insertion at a position.
//
//   doc        - handle to a document.
//   page_index - the index of the page.
//
// Returns how many annotations moved (0 when none was inline), or -1.
FPDF_EXPORT int FPDF_CALLCONV EPDFPage_PromoteInlineAnnotsRaw(FPDF_DOCUMENT doc,
                                                              int page_index);

// Experimental EmbedPDF Extension API.
// Whether |annot| still names an annotation: its object resolves, it is in
// its page's /Annots (if it ever was), and its page is in the document. A
// handle to an annotation that was deleted, or created in an aborted layer
// transaction, is invalid; an abort that brings an annotation back makes its
// handle valid again. Every other call on an invalid handle fails;
// FPDFPage_CloseAnnot() still closes it.
//
//   annot - handle to an annotation.
//
// Returns true if the handle is valid.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_IsValid(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Set the /Name entry of an annotation: the icon name of a text, file
// attachment, or sound annotation, or a stamp's identifier (ISO 32000-2
// tables 175, 184, 187, 188). Any name is accepted — the predefined sets
// are a READER's rendering obligation, not a writer's validation — so
// custom stamp identifiers such as Acrobat's "#LBGiYhk8V_oAfmqAPENiwD"
// work as well as "Approved". Written as a name object (the serializer
// applies #xx escaping; callers pass raw text). Never touches /AP: a
// stamp keeps the appearance it was given, and icon subtypes are rebaked
// by the caller's appearance regeneration.
//
// To remove /Name (fall back to the subtype's default icon), use
// EPDFAnnot_RemoveKey(annot, "Name").
//
//   annot    - handle to an annotation of a subtype that carries /Name
//              (text, file attachment, sound, stamp).
//   name     - UTF-8 zero-terminated name text without the leading "/";
//              must be non-empty.
//
// Returns true on success; false for other subtypes, an empty name, or a
// null handle.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetName(FPDF_ANNOTATION annot,
                                                      FPDF_BYTESTRING name);

// Experimental EmbedPDF Extension API.
// Get the /Name entry of an annotation as text, whatever its value.
//
//   annot    - handle to an annotation.
//   buffer   - receives the name as UTF-8 with a NUL terminator; may be
//              NULL to query the required size.
//   buflen   - size of |buffer| in bytes.
//
// Returns the number of bytes needed including the terminator (the name is
// copied only when |buflen| is large enough), or 0 when the annotation has
// no /Name or on error.
FPDF_EXPORT unsigned long FPDF_CALLCONV EPDFAnnot_GetName(FPDF_ANNOTATION annot,
                                                          char* buffer,
                                                          unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Resize the normal appearance (/AP/N) of a Stamp to match the annotation's
// /Rect using the specified fit policy. Updates the AP /BBox and the image's
// CTM. When our /EMBD_Metadata records a turn and the appearance isn't our
// wrapper yet, the drawing wrapped is the form another app drew it in
// (EPDFAnnot_GetAppearanceTransform), without the matrices that turn and place
// it: our wrapper does the turn, so a stamp Acrobat turned is turned once. Below full opacity (/CA < 1) the appearance is `/R0 gs /MWFOForm Do`
// over the resized form, the layer Acrobat writes: Acrobat replaces a layer
// named so when its opacity changes, rather than painting over it. Our
// wrapper is found again under a layer that paints /CA and under forms that
// only draw it, so another editor's changes don't nest the drawing. A layer
// that paints another opacity is part of the drawing; to change /CA, use
// EPDFAnnot_SetStampOpacity.
//
//   annot - handle to a Stamp annotation.
//   fit   - one of EPDF_STAMP_FIT_*.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_UpdateAppearanceToRect(FPDF_ANNOTATION annot, EPDF_STAMP_FIT fit);

// Experimental EmbedPDF Extension API.
// Set a Stamp's opacity (/CA, as EPDFAnnot_SetOpacity) and re-fit its
// appearance as EPDFAnnot_UpdateAppearanceToRect does, reading the current
// appearance with the opacity it was painted with: its opacity layer, ours or
// Acrobat's, is replaced by one for the new value, not kept as drawing.
//
//   annot   - handle to a Stamp annotation.
//   fit     - one of EPDF_STAMP_FIT_*.
//   opacity - the new opacity, 0 to 1; 1 removes /CA.
//
// Returns true on success; on failure /CA and the appearance are unchanged.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetStampOpacity(FPDF_ANNOTATION annot,
                          EPDF_STAMP_FIT fit,
                          float opacity);

// Experimental EmbedPDF Extension API.
// Create an annotation. (the difference from FPDFPage_CreateAnnot is that it
// creates an indirect object, whose /P names the page)
//
//   page    - handle to a page.
//   subtype - the subtype of the annotation.
//
// Returns the annotation.
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
EPDFPage_CreateAnnot(FPDF_PAGE page, FPDF_ANNOTATION_SUBTYPE subtype);

// Experimental EmbedPDF Extension API.
// Set the rotation of an annotation in degrees.
//
//   annot    - handle to an annotation.
//   rotation - the rotation in degrees (any value, e.g. 0, 45.5, 90, 180,
//   etc.).
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetRotate(FPDF_ANNOTATION annot,
                                                        float rotation);

// Experimental EmbedPDF Extension API.
// Get the rotation of an annotation in degrees.
//
//   annot    - handle to an annotation.
//   rotation - receives the rotation value in degrees.
//
// Returns true on success, false if annot is invalid or rotation is NULL.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_GetRotate(FPDF_ANNOTATION annot,
                                                        float* rotation);

// Experimental EmbedPDF Extension API.
// Where an annotation's normal appearance is drawn: the box of the form its
// drawing is written in, and the matrix from that form to the page, as a
// viewer places an appearance (ISO 32000-2 12.5.5: the /BBox through the
// /Matrix, stretched onto /Rect). The form is found as a viewer reaches it:
// through a layer that only paints an opacity and through forms that only draw
// another form (moving it at most), so it is the one whose /Matrix turns the
// drawing, when one does: a stamp Acrobat turned, our stamp wrapper, a turned
// shape we generated. Nothing is decided here: a caller reads a turn, a scale
// or a shift from the matrix.
//
//   annot  - handle to an annotation.
//   box    - receives the form's /BBox.
//   matrix - receives the matrix from the form to the page.
//
// Returns true on success; false when there is no normal appearance with a
// box, or on invalid arguments.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetAppearanceTransform(FPDF_ANNOTATION annot,
                                 FS_RECTF* box,
                                 FS_MATRIX* matrix);

// Experimental EmbedPDF Extension API.
// Get the reply type (RT) of an annotation. This specifies how an annotation
// relates to another annotation when used together with IRT (In Reply To).
// See ISO 32000-2, section 12.5.6.
//
//   annot - handle to an annotation.
//
// Returns the reply type. Returns FPDF_ANNOT_RT_REPLY if RT is missing
// (the default per PDF specification).
FPDF_EXPORT FPDF_ANNOT_REPLY_TYPE FPDF_CALLCONV
EPDFAnnot_GetReplyType(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Set the reply type (RT) of an annotation. This specifies how an annotation
// relates to another annotation when used together with IRT (In Reply To).
// See ISO 32000-2, section 12.5.6.
//
//   annot - handle to an annotation.
//   rt    - the reply type to set. Pass FPDF_ANNOT_RT_UNKNOWN to remove the
//           RT entry from the annotation dictionary.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetReplyType(FPDF_ANNOTATION annot, FPDF_ANNOT_REPLY_TYPE rt);

// Experimental EmbedPDF Extension API.
// Get a boolean entry of an annotation's dictionary, such as a popup's /Open.
//
//   annot - handle to an annotation.
//   key   - the key of the entry.
//   value - receives the value.
//
// Returns true when the entry exists and is a boolean; false otherwise, and
// |value| is left unchanged.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetBooleanValue(FPDF_ANNOTATION annot,
                          FPDF_BYTESTRING key,
                          FPDF_BOOL* value);

// Experimental EmbedPDF Extension API.
// Set a boolean entry of an annotation's dictionary, such as a popup's /Open.
//
//   annot - handle to an annotation.
//   key   - the key of the entry.
//   value - the value to set.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetBooleanValue(FPDF_ANNOTATION annot,
                          FPDF_BYTESTRING key,
                          FPDF_BOOL value);

// Experimental EmbedPDF Extension API.
// Set the overlay text for a Redact annotation. The overlay text is displayed
// on the redacted area after the redaction is applied.
//
//   annot - handle to a Redact annotation.
//   text  - the overlay text to set. Pass NULL or empty string to remove.
//
// Returns true on success. Returns false if the annotation is not a Redact.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetOverlayText(FPDF_ANNOTATION annot, FPDF_WIDESTRING text);

// Experimental EmbedPDF Extension API.
// Get the overlay text for a Redact annotation.
//
//   annot  - handle to a Redact annotation.
//   buffer - a buffer for the overlay text (UTF-16LE).
//   buflen - the length of the buffer in bytes.
//
// Returns the number of bytes in the overlay text (including the terminating
// NUL character), or 0 if not a Redact annotation or no overlay text is set.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetOverlayText(FPDF_ANNOTATION annot,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Set whether the overlay text repeats to fill the redaction area.
//
//   annot  - handle to a Redact annotation.
//   repeat - true to repeat the overlay text, false otherwise.
//
// Returns true on success. Returns false if the annotation is not a Redact.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetOverlayTextRepeat(FPDF_ANNOTATION annot, FPDF_BOOL repeat);

// Experimental EmbedPDF Extension API.
// Get whether the overlay text repeats to fill the redaction area.
//
//   annot - handle to a Redact annotation.
//
// Returns true if the overlay text repeats, false otherwise or if not a Redact.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetOverlayTextRepeat(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Set an annotation's normal appearance (AP/N) from a page of another document.
// The page's content stream and resources are deep-cloned into the annotation's
// document as a Form XObject and set as the AP/N entry.
//
//   annot         - handle to the target annotation.
//   src_doc       - handle to the source document containing the page.
//   page_index    - zero-based index of the source page.
//
// Returns TRUE on success, FALSE on error.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetAppearanceFromPage(FPDF_ANNOTATION annot,
                                FPDF_DOCUMENT src_doc,
                                int page_index);

// Experimental EmbedPDF Extension API.
// Export what an annotation's normal appearance draws, apart from what its
// dictionary describes, as a new single-page document: the drawing a copy of
// the annotation is created from.
//
//   - EmbedPDF's own wrapper (the appearance a stamp gets from
//     EPDFAnnot_SetStampDrawing or EPDFAnnot_UpdateAppearanceToRect), also
//     when another editor has put forms that only draw it around it: the
//     drawing it places, as EPDFDoc_ExportDrawing() exports it, without the
//     wrapper's placement, rotation and opacity.
//   - Any other appearance: the drawing in its own box, as its /BBox and
//     /Matrix show it. The annotation's /Rect places it again: a copy fills
//     /Rect with it, as ISO 32000-2 12.5.5 places any appearance. A layer
//     that only paints the annotation's /CA (`/GS gs /Form Do`, as Acrobat
//     writes it) is left out, one that paints another opacity is part of the
//     drawing. With a `degrees` turn, the drawing is placed in /Rect, that
//     turn is taken out, and the page is `unrotated_box`.
//
// The page is the canonical form of a drawing: its content is exactly
// `/EPDFDRAWING Do`, the placement is folded into the form's /Matrix, and
// the document has no /Info and an /ID made from its content. The same
// drawing is the same bytes on every runtime, and EPDFAnnot_SetAppearanceFromPage
// and EPDFDoc_ImportDrawing adopt the form as it is, so exporting what they
// made gives the same bytes again. The source document is untouched.
//
//   annot         - handle to an annotation with a normal appearance.
//   degrees       - the turn the annotation's data describes, counterclockwise
//                   (the PDF convention); 0 for none.
//   unrotated_box - the box it turns about, in page space; NULL for none.
//
// Returns a new document (the caller closes it with FPDF_CloseDocument()),
// or NULL when the annotation has no normal appearance or on error.
FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFAnnot_ExportAppearance(FPDF_ANNOTATION annot,
                           float degrees,
                           const FS_RECTF* unrotated_box);

// Stamp drawings.
//
// A stamp's drawing is its artwork, one form. Each stamp places it with a
// wrapper of its own, which holds the stamp's box, fit, rotation and
// opacity, so stamps with the same artwork can share one drawing. A drawing
// is never edited in place: a stamp that changes gets a new wrapper, and new
// artwork is a new drawing. A drawing is known by its content, the
// EPDF_DIGEST_SHA256 (EPDF_DigestBuffer()) of its canonical bytes.

// Experimental EmbedPDF Extension API.
// The canonical drawing of a page, in a new single-page document: the bytes
// EPDFAnnot_ExportAppearance() gives for a stamp made from that page. A page
// that is already canonical gives the same bytes back. It depends on the page
// alone. |src_doc| is untouched.
//
//   src_doc    - the document holding the page: a PDF, or a page holding an
//                image at its own size.
//   page_index - zero-based index of the page.
//
// Returns a new document (the caller closes it with FPDF_CloseDocument()),
// or NULL on error.
FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFDoc_CanonicalDrawing(FPDF_DOCUMENT src_doc, int page_index);

// Experimental EmbedPDF Extension API.
// Add the drawing of a canonical page to |document| as a new object. Its
// export (EPDFDoc_ExportDrawing()) is the canonical page's bytes.
//
//   document      - the document to add the drawing to.
//   canonical_doc - a single-page document from EPDFDoc_CanonicalDrawing(),
//                   EPDFDoc_ExportDrawing() or EPDFAnnot_ExportAppearance().
//
// Returns the drawing's object number, or 0 when |canonical_doc| is not one
// canonical page, or on error.
FPDF_EXPORT unsigned int FPDF_CALLCONV
EPDFDoc_ImportDrawing(FPDF_DOCUMENT document, FPDF_DOCUMENT canonical_doc);

// Experimental EmbedPDF Extension API.
// Export one drawing of |document| as its canonical page, in a new
// single-page document. |document| is untouched.
//
//   document - the document holding the drawing.
//   drawing  - the drawing's object number (a form XObject).
//
// Returns a new document (the caller closes it with FPDF_CloseDocument()),
// or NULL when |drawing| is not a form, or on error.
FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFDoc_ExportDrawing(FPDF_DOCUMENT document, unsigned int drawing);

// Experimental EmbedPDF Extension API.
// The drawings EmbedPDF wrappers place on the stamps of |document|'s pages,
// each once, in page and /Annots order. Reads the page dictionaries; no page
// is loaded. A stamp without our wrapper has none.
//
//   document - handle to the document.
//   buffer   - receives up to |buflen| object numbers; may be NULL.
//   buflen   - capacity of |buffer|, in entries.
//
// Returns the number of drawings, which may exceed |buflen|.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetStampDrawings(FPDF_DOCUMENT document,
                         unsigned int* buffer,
                         unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Make a stamp place |drawing|: a new wrapper that fits it into the stamp's
// box with |fit|, honouring /EMBD_Metadata rotation, under a layer that
// paints /CA below full opacity, as EPDFAnnot_UpdateAppearanceToRect() does.
// The drawing is not changed, so other stamps can place it too.
//
//   annot   - handle to a Stamp annotation.
//   drawing - object number of a drawing of the annotation's document.
//   fit     - one of EPDF_STAMP_FIT_*.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetStampDrawing(FPDF_ANNOTATION annot,
                          unsigned int drawing,
                          EPDF_STAMP_FIT fit);

// Experimental EmbedPDF Extension API.
// The drawing a stamp's wrapper places.
//
//   annot - handle to a Stamp annotation.
//
// Returns the drawing's object number, or 0 for a stamp without our wrapper,
// or an annotation that is not a stamp.
FPDF_EXPORT unsigned int FPDF_CALLCONV
EPDFAnnot_GetStampDrawing(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Get the annotation rectangle with normalization applied.
// Wraps FPDFAnnot_GetRect and ensures the returned rect is normalized
// (left <= right, bottom <= top in page coordinates).
//
//   annot - handle to an annotation.
//   rect  - receives the normalized rectangle; must not be NULL.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_GetRect(FPDF_ANNOTATION annot,
                                                      FS_RECTF* rect);

// Experimental EmbedPDF Extension API.
// Set the annotation rectangle without modifying any appearance stream.
// Unlike FPDFAnnot_SetRect(), this function only writes the annotation's
// /Rect entry; it never changes /AP, an appearance /BBox, or its /Matrix.
// Appearance preservation or regeneration remains the caller's decision.
//
//   annot - handle to an annotation.
//   rect  - rectangle to write; must not be NULL.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetRect(FPDF_ANNOTATION annot,
                                                      const FS_RECTF* rect);

// Experimental EmbedPDF Extension API.
// Set the Matrix on an annotation's appearance stream for the given mode.
// This overwrites any existing Matrix entry on the AP stream.
//
//   annot          - handle to an annotation.
//   appearanceMode - one of FPDF_ANNOT_APPEARANCEMODE_NORMAL (0),
//                    FPDF_ANNOT_APPEARANCEMODE_ROLLOVER (1),
//                    FPDF_ANNOT_APPEARANCEMODE_DOWN (2).
//   matrix         - pointer to FS_MATRIX to set. Must not be NULL.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetAPMatrix(FPDF_ANNOTATION annot,
                      FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                      const FS_MATRIX* matrix);

// Experimental EmbedPDF Extension API.
// Get the Matrix from an annotation's appearance stream for the given mode.
// Returns identity matrix {1,0,0,1,0,0} if no Matrix entry exists.
//
//   annot          - handle to an annotation.
//   appearanceMode - one of FPDF_ANNOT_APPEARANCEMODE_NORMAL (0),
//                    FPDF_ANNOT_APPEARANCEMODE_ROLLOVER (1),
//                    FPDF_ANNOT_APPEARANCEMODE_DOWN (2).
//   matrix         - receives the matrix. Must not be NULL.
//
// Returns true on success, false if annot is invalid or matrix is NULL.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetAPMatrix(FPDF_ANNOTATION annot,
                      FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                      FS_MATRIX* matrix);

// Experimental EmbedPDF Extension API.
// Get a bitmask of which appearance stream modes exist for an annotation.
//
//   annot - handle to an annotation.
//
// Returns an integer bitmask:
//   bit 0 (1) = Normal  (/AP/N exists)
//   bit 1 (2) = Rollover (/AP/R exists)
//   bit 2 (4) = Down     (/AP/D exists)
// Returns 0 if the annotation has no /AP dictionary or annot is invalid.
FPDF_EXPORT int FPDF_CALLCONV
EPDFAnnot_GetAvailableAppearanceModes(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Get how many states an appearance mode stores: the entries of its
// appearance subdictionary that are streams (/AP /N << /Off … /Yes … >>
// stores two). A mode whose entry is a single stream, or that the annotation
// doesn't have, stores none: there is no falling back to /N.
//
//   annot          - handle to an annotation.
//   appearanceMode - one of FPDF_ANNOT_APPEARANCEMODE_*.
//
// Returns the number of states, or 0.
FPDF_EXPORT int FPDF_CALLCONV
EPDFAnnot_GetAppearanceStateCount(FPDF_ANNOTATION annot,
                                  FPDF_ANNOT_APPEARANCEMODE appearanceMode);

// Experimental EmbedPDF Extension API.
// Copy the name of one state of an appearance mode into |buffer| as raw PDF
// name bytes, including the trailing NUL: the name EPDF_RenderAnnotBitmap
// takes as its `state`. States are in the subdictionary's key order, which
// is stable.
//
//   annot          - handle to an annotation.
//   appearanceMode - one of FPDF_ANNOT_APPEARANCEMODE_*.
//   index          - the state, in [0, EPDFAnnot_GetAppearanceStateCount).
//   buffer         - receives the name; may be NULL to query the length.
//   buflen         - the length of |buffer| in bytes.
//
// Returns the byte length of the name including the NUL, or 0 when there is
// no such state.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetAppearanceStateName(FPDF_ANNOTATION annot,
                                 FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                                 int index,
                                 void* buffer,
                                 unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Copy the annotation's /AS, the state it shows, into |buffer| as raw PDF
// name bytes, including the trailing NUL: the same bytes
// EPDFAnnot_GetAppearanceStateName gives for that state.
//
//   annot  - handle to an annotation.
//   buffer - receives the name; may be NULL to query the length.
//   buflen - the length of |buffer| in bytes.
//
// Returns the byte length of the name including the NUL, or 0 when the
// annotation has no /AS name.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetAppearanceState(FPDF_ANNOTATION annot,
                             void* buffer,
                             unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Check whether an annotation has a renderable appearance stream for the
// given mode, taking sub-appearance dictionaries and /AS into account.
//
// For simple annotations where /AP/<mode> is a stream, returns true if that
// stream exists. For button widgets where /AP/<mode> is a dictionary of
// sub-appearances (e.g. "Off", "Yes"), returns true only if a sub-appearance
// matching the current /AS value exists.
//
//   annot          - handle to an annotation.
//   appearanceMode - the appearance mode to check. One of:
//                    FPDF_ANNOT_APPEARANCEMODE_NORMAL (0),
//                    FPDF_ANNOT_APPEARANCEMODE_ROLLOVER (1),
//                    FPDF_ANNOT_APPEARANCEMODE_DOWN (2).
//
// Returns true if a renderable appearance stream exists, false otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_HasAppearanceStream(FPDF_ANNOTATION annot,
                              FPDF_ANNOT_APPEARANCEMODE appearanceMode);

// Experimental EmbedPDF Extension API.
// Get where an annotation's appearance for the given mode is drawn, in page
// space, without writing anything. A stored appearance is fitted into /Rect.
// With none, the normal appearance PDFium draws in memory takes its own box,
// which can reach past /Rect; it is never written to the file.
//
//   annot          - handle to an annotation.
//   appearanceMode - one of FPDF_ANNOT_APPEARANCEMODE_*.
//   rect           - receives the box.
//
// Returns false, and leaves `rect` alone, when there is nothing to draw
// without writing an appearance: a hidden annotation, or a kind PDFium draws
// only by generating one into the file.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetDrawingRect(FPDF_ANNOTATION annot,
                         FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                         FS_RECTF* rect);

// Experimental EmbedPDF Extension API.
// Color types for the MK (appearance characteristics) dictionary on widget
// annotations.  BC = border color, BG = background color.
typedef enum {
  EPDF_MK_COLOR_BC = 0,
  EPDF_MK_COLOR_BG = 1,
} EPDF_MK_COLORTYPE;

// Experimental EmbedPDF Extension API.
// Set a color in the widget annotation's /MK dictionary.
//
//   annot - handle to an annotation.
//   type  - EPDF_MK_COLOR_BC (border) or EPDF_MK_COLOR_BG (background).
//   R,G,B - color components in 0..255.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetMKColor(FPDF_ANNOTATION annot,
                                                         EPDF_MK_COLORTYPE type,
                                                         unsigned int R,
                                                         unsigned int G,
                                                         unsigned int B);

// Experimental EmbedPDF Extension API.
// Get a color from the widget annotation's /MK dictionary.
//
//   annot - handle to an annotation.
//   type  - EPDF_MK_COLOR_BC (border) or EPDF_MK_COLOR_BG (background).
//   R,G,B - pointers to receive color components in 0..255.
//
// Returns true on success, false if no MK color is set or annot is invalid.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_GetMKColor(FPDF_ANNOTATION annot,
                                                         EPDF_MK_COLORTYPE type,
                                                         unsigned int* R,
                                                         unsigned int* G,
                                                         unsigned int* B);

// Experimental EmbedPDF Extension API.
// Remove a color from the widget annotation's /MK dictionary.
//
//   annot - handle to an annotation.
//   type  - EPDF_MK_COLOR_BC (border) or EPDF_MK_COLOR_BG (background).
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearMKColor(FPDF_ANNOTATION annot, EPDF_MK_COLORTYPE type);

// The captions of a widget's /MK dictionary.
#define EPDF_MK_TEXT_CA 0  // /CA, the normal caption
#define EPDF_MK_TEXT_RC 1  // /RC, the rollover caption (push buttons)
#define EPDF_MK_TEXT_AC 2  // /AC, the down caption (push buttons)

// Experimental EmbedPDF Extension API.
// Set a widget's /MK caption |which| (EPDF_MK_TEXT_*) to |text|, or remove
// it when |text| is NULL. The appearance is not regenerated
// (EPDFAnnot_GenerateFormFieldAP()). Returns false for another subtype.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetMKText(FPDF_ANNOTATION annot,
                                                        int which,
                                                        FPDF_WIDESTRING text);

// Experimental EmbedPDF Extension API.
// Copy a widget's /MK caption |which| (EPDF_MK_TEXT_*) as UTF-16LE,
// including the trailing NUL. Returns the required byte length, or 0 when
// the caption is absent (an empty caption returns 2). |buffer| may be NULL
// to query the length.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetMKText(FPDF_ANNOTATION annot,
                    int which,
                    FPDF_WCHAR* buffer,
                    unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Set a widget's /MK /TP, where a push button's caption sits beside its icon
// (ISO 32000-2 Table 192): 0 caption only (the default), 1 icon only, 2-6
// caption below, above, right of, left of, or over the icon.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetMKTextPosition(FPDF_ANNOTATION annot, int position);

// Experimental EmbedPDF Extension API.
// A widget's /MK /TP (0 when absent), or -1 for another subtype.
FPDF_EXPORT int FPDF_CALLCONV
EPDFAnnot_GetMKTextPosition(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Set a widget's /MK /R: how far its appearance turns, in degrees
// counterclockwise, a multiple of 90 from 0 to 270 (0 removes /R). The
// appearance is not regenerated (EPDFAnnot_GenerateFormFieldAP()).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetMKRotation(FPDF_ANNOTATION annot, int degrees);

// Experimental EmbedPDF Extension API.
// A widget's /MK /R as stored, in degrees counterclockwise (0 when absent),
// or -1 for another subtype.
FPDF_EXPORT int FPDF_CALLCONV EPDFAnnot_GetMKRotation(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Generate the appearance stream for a form field widget annotation.
// The standard EPDFAnnot_GenerateAppearance does NOT handle Widget subtypes.
// This function reads /FT (from the annotation dict or its /Parent) and
// dispatches to the appropriate form AP generator:
//   - /Tx  -> text field appearance
//   - /Ch  -> combo box or list box appearance (based on /Ff flags)
//   - /Btn -> a check box or radio button appearance: both states, the
//             symbol /MK /CA names in the /DA colour. A push button: its
//             background and border, and its caption (/MK /CA) centred in
//             the /DA font and colour; an icon (/MK /I) is not drawn.
//
//   annot - handle to a widget annotation.
//
// Returns true on success, false if the annotation is not a form field.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GenerateFormFieldAP(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Get the number of callout line points (/CL array) on a FreeText annotation.
//
//   annot - handle to a FreeText annotation.
//
// Returns 2 (simple line), 3 (knee-jointed line), or 0 if absent/invalid.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetCalloutLineCount(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Get the callout line points (/CL array) from a FreeText annotation.
// Same two-call pattern as FPDFAnnot_GetVertices: call with buffer=NULL
// to get the count, then allocate and call again.
//
//   annot  - handle to a FreeText annotation.
//   buffer - buffer to receive FS_POINTF structs (may be NULL).
//   length - number of FS_POINTF entries the buffer can hold.
//
// Returns the number of points (2 or 3), or 0 on failure.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetCalloutLine(FPDF_ANNOTATION annot,
                         FS_POINTF* buffer,
                         unsigned long length);

// Experimental EmbedPDF Extension API.
// Set or remove the callout line (/CL array) on a FreeText annotation.
// |count| must be 2 (simple line) or 3 (knee-jointed line).
// Pass NULL/0 to remove the /CL entry.
//
//   annot  - handle to a FreeText annotation.
//   points - array of 2 or 3 FS_POINTF structs (may be NULL to remove).
//   count  - number of points.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetCalloutLine(FPDF_ANNOTATION annot,
                         const FS_POINTF* points,
                         unsigned long count);

// Experimental EmbedPDF Extension API.
// Get the PDF indirect object number of an annotation's dictionary.
//
//   annot - handle to an annotation.
//
// Returns the object number (> 0) on success, or 0 if the annotation
// dictionary is a direct object or the handle is invalid.
FPDF_EXPORT unsigned int FPDF_CALLCONV
EPDFAnnot_GetObjectNumber(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Get an annotation on a page by its PDF indirect object number.
// Similar to EPDFPage_GetAnnotByName() but keyed by object number.
//
//   page    - handle to the page.
//   obj_num - the indirect object number to look for.
//
// Returns a handle to the annotation, or NULL if not found.
// The caller must close the returned handle with FPDFPage_CloseAnnot().
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
EPDFPage_GetAnnotByObjectNumber(FPDF_PAGE page, unsigned int obj_num);

// Experimental EmbedPDF Extension API.
// Remove an annotation on a page by index. Same as FPDFPage_RemoveAnnot but
// also deletes the underlying indirect object so no orphan remains in the
// document.
//
//   page  - handle to the page.
//   index - the index of the annotation in the /Annots array.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFPage_RemoveAnnot(FPDF_PAGE page,
                                                         int index);

// Experimental EmbedPDF Extension API.
// Remove an annotation on a page by its PDF indirect object number.
// Similar to EPDFPage_RemoveAnnotByName() but keyed by object number.
//
//   page    - handle to the page.
//   obj_num - the indirect object number to remove.
//
// Returns true on success. Also deletes the indirect object so no orphan
// remains in the document.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_RemoveAnnotByObjectNumber(FPDF_PAGE page, unsigned int obj_num);

// Experimental EmbedPDF Extension API.
// Move a contiguous block of annotations within a page's /Annots array.
// Mirrors FPDF_MovePages semantics for the per-page annotation list:
// the entries at |from_indices[0..from_indices_len)| are detached, then
// re-inserted as a contiguous block starting at |to_index| in the
// post-removal index space, preserving caller-supplied order.
//
//   page              - handle to the page.
//   from_indices      - array of source indices in /Annots to move.
//   from_indices_len  - length of |from_indices|; must be > 0.
//   to_index          - destination index in the post-removal index
//                       space; must be in [0, current_count -
//                       from_indices_len].
//
// All validation runs BEFORE any mutation: a failure (out-of-range
// source, out-of-range destination, or any duplicate source index)
// returns false WITHOUT mutating the array. The indirect annotation
// objects are never destroyed by this call, so durable identity
// (objectNumber, /NM) is preserved across the move.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFPage_MoveAnnots(FPDF_PAGE page,
                                                        const int* from_indices,
                                                        int from_indices_len,
                                                        int to_index);

// Experimental EmbedPDF Extension API.
// EPDFPage_MoveAnnots() on a page that isn't loaded: the page's /Annots is
// read and written from its dictionary, so its content is never parsed. A
// reorder moves entries, so promote the page first (see
// EPDFPage_PromoteInlineAnnotsRaw()).
//
//   doc              - handle to a document.
//   page_index       - the index of the page.
//   from_indices     - as EPDFPage_MoveAnnots().
//   from_indices_len - as EPDFPage_MoveAnnots().
//   to_index         - as EPDFPage_MoveAnnots().
//
// Returns true on success. A request EPDFPage_MoveAnnots() refuses returns
// false and changes nothing.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_MoveAnnotsRaw(FPDF_DOCUMENT doc,
                       int page_index,
                       const int* from_indices,
                       int from_indices_len,
                       int to_index);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_ANNOT_H_
