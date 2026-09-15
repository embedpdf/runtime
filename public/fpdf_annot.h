// Copyright 2017 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PUBLIC_FPDF_ANNOT_H_
#define PUBLIC_FPDF_ANNOT_H_

#include <stddef.h>
// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"

// NOLINTNEXTLINE(build/include)
#include "fpdf_formfill.h"

// NOLINTNEXTLINE(build/include)
#include "epdf_font.h"

// NOLINTNEXTLINE(build/include)
#include "epdf_redact.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

#define FPDF_ANNOT_UNKNOWN 0
#define FPDF_ANNOT_TEXT 1
#define FPDF_ANNOT_LINK 2
#define FPDF_ANNOT_FREETEXT 3
#define FPDF_ANNOT_LINE 4
#define FPDF_ANNOT_SQUARE 5
#define FPDF_ANNOT_CIRCLE 6
#define FPDF_ANNOT_POLYGON 7
#define FPDF_ANNOT_POLYLINE 8
#define FPDF_ANNOT_HIGHLIGHT 9
#define FPDF_ANNOT_UNDERLINE 10
#define FPDF_ANNOT_SQUIGGLY 11
#define FPDF_ANNOT_STRIKEOUT 12
#define FPDF_ANNOT_STAMP 13
#define FPDF_ANNOT_CARET 14
#define FPDF_ANNOT_INK 15
#define FPDF_ANNOT_POPUP 16
#define FPDF_ANNOT_FILEATTACHMENT 17
#define FPDF_ANNOT_SOUND 18
#define FPDF_ANNOT_MOVIE 19
#define FPDF_ANNOT_WIDGET 20
#define FPDF_ANNOT_SCREEN 21
#define FPDF_ANNOT_PRINTERMARK 22
#define FPDF_ANNOT_TRAPNET 23
#define FPDF_ANNOT_WATERMARK 24
#define FPDF_ANNOT_THREED 25
#define FPDF_ANNOT_RICHMEDIA 26
#define FPDF_ANNOT_XFAWIDGET 27
#define FPDF_ANNOT_REDACT 28

// Refer to PDF Reference (6th edition) table 8.16 for all annotation flags.
#define FPDF_ANNOT_FLAG_NONE 0
#define FPDF_ANNOT_FLAG_INVISIBLE (1 << 0)
#define FPDF_ANNOT_FLAG_HIDDEN (1 << 1)
#define FPDF_ANNOT_FLAG_PRINT (1 << 2)
#define FPDF_ANNOT_FLAG_NOZOOM (1 << 3)
#define FPDF_ANNOT_FLAG_NOROTATE (1 << 4)
#define FPDF_ANNOT_FLAG_NOVIEW (1 << 5)
#define FPDF_ANNOT_FLAG_READONLY (1 << 6)
#define FPDF_ANNOT_FLAG_LOCKED (1 << 7)
#define FPDF_ANNOT_FLAG_TOGGLENOVIEW (1 << 8)

#define FPDF_ANNOT_APPEARANCEMODE_NORMAL 0
#define FPDF_ANNOT_APPEARANCEMODE_ROLLOVER 1
#define FPDF_ANNOT_APPEARANCEMODE_DOWN 2
#define FPDF_ANNOT_APPEARANCEMODE_COUNT 3

// Refer to PDF Reference version 1.7 table 8.70 for field flags common to all
// interactive form field types.
#define FPDF_FORMFLAG_NONE 0
#define FPDF_FORMFLAG_READONLY (1 << 0)
#define FPDF_FORMFLAG_REQUIRED (1 << 1)
#define FPDF_FORMFLAG_NOEXPORT (1 << 2)

// Refer to PDF Reference version 1.7 table 8.77 for field flags specific to
// interactive form text fields.
#define FPDF_FORMFLAG_TEXT_MULTILINE (1 << 12)
#define FPDF_FORMFLAG_TEXT_PASSWORD (1 << 13)

// Refer to PDF Reference version 1.7 table 8.79 for field flags specific to
// interactive form choice fields.
#define FPDF_FORMFLAG_CHOICE_COMBO (1 << 17)
#define FPDF_FORMFLAG_CHOICE_EDIT (1 << 18)
#define FPDF_FORMFLAG_CHOICE_MULTI_SELECT (1 << 21)

// Additional actions type of form field:
//   K, on key stroke, JavaScript action.
//   F, on format, JavaScript action.
//   V, on validate, JavaScript action.
//   C, on calculate, JavaScript action.
#define FPDF_ANNOT_AACTION_KEY_STROKE 12
#define FPDF_ANNOT_AACTION_FORMAT 13
#define FPDF_ANNOT_AACTION_VALIDATE 14
#define FPDF_ANNOT_AACTION_CALCULATE 15

typedef enum FPDFANNOT_COLORTYPE {
  FPDFANNOT_COLORTYPE_Color = 0,
  FPDFANNOT_COLORTYPE_InteriorColor,
  FPDFANNOT_COLORTYPE_OverlayColor,  // For Redact annotations only (OC key)
  FPDFANNOT_COLORTYPE_TextColor      // For FreeText annotations (TextColor key)
} FPDFANNOT_COLORTYPE;

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

// Experimental API.
// Check if an annotation subtype is currently supported for creation.
// Currently supported subtypes:
//    - circle
//    - fileattachment
//    - freetext
//    - highlight
//    - ink
//    - line
//    - link
//    - polygon
//    - polyline
//    - popup
//    - redact
//    - square
//    - squiggly
//    - stamp
//    - strikeout
//    - text
//    - underline
//
//   subtype   - the subtype to be checked.
//
// Returns true if this subtype supported.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_IsSupportedSubtype(FPDF_ANNOTATION_SUBTYPE subtype);

// Experimental API.
// Create an annotation in |page| of the subtype |subtype|. If the specified
// subtype is illegal or unsupported, then a new annotation will not be created.
// Must call FPDFPage_CloseAnnot() when the annotation returned by this
// function is no longer needed.
//
//   page      - handle to a page.
//   subtype   - the subtype of the new annotation.
//
// Returns a handle to the new annotation object, or NULL on failure.
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDFPage_CreateAnnot(FPDF_PAGE page, FPDF_ANNOTATION_SUBTYPE subtype);

// Experimental API.
// Get the number of annotations in |page|.
//
//   page   - handle to a page.
//
// Returns the number of annotations in |page|.
FPDF_EXPORT int FPDF_CALLCONV FPDFPage_GetAnnotCount(FPDF_PAGE page);

// Experimental API.
// Get annotation in |page| at |index|. Must call FPDFPage_CloseAnnot() when the
// annotation returned by this function is no longer needed.
//
//   page  - handle to a page.
//   index - the index of the annotation.
//
// Returns a handle to the annotation object, or NULL on failure.
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV FPDFPage_GetAnnot(FPDF_PAGE page,
                                                            int index);

// Experimental API.
// Get the index of |annot| in |page|. This is the opposite of
// FPDFPage_GetAnnot().
//
//   page  - handle to the page that the annotation is on.
//   annot - handle to an annotation.
//
// Returns the index of |annot|, or -1 on failure.
FPDF_EXPORT int FPDF_CALLCONV FPDFPage_GetAnnotIndex(FPDF_PAGE page,
                                                     FPDF_ANNOTATION annot);

// Experimental API.
// Close an annotation. Must be called when the annotation returned by
// FPDFPage_CreateAnnot() or FPDFPage_GetAnnot() is no longer needed. This
// function does not remove the annotation from the document.
//
//   annot  - handle to an annotation.
FPDF_EXPORT void FPDF_CALLCONV FPDFPage_CloseAnnot(FPDF_ANNOTATION annot);

// Experimental API.
// Remove the annotation in |page| at |index|.
//
//   page  - handle to a page.
//   index - the index of the annotation.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFPage_RemoveAnnot(FPDF_PAGE page,
                                                         int index);

// Experimental API.
// Get the subtype of an annotation.
//
//   annot  - handle to an annotation.
//
// Returns the annotation subtype.
FPDF_EXPORT FPDF_ANNOTATION_SUBTYPE FPDF_CALLCONV
FPDFAnnot_GetSubtype(FPDF_ANNOTATION annot);

// Experimental API.
// Check if an annotation subtype is currently supported for object extraction,
// update, and removal.
// Currently supported subtypes: ink and stamp.
//
//   subtype   - the subtype to be checked.
//
// Returns true if this subtype supported.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_IsObjectSupportedSubtype(FPDF_ANNOTATION_SUBTYPE subtype);

// Experimental API.
// Update |obj| in |annot|. |obj| must be in |annot| already and must have
// been retrieved by FPDFAnnot_GetObject(). Currently, only ink and stamp
// annotations are supported by this API. Also note that only path, image, and
// text objects have APIs for modification; see FPDFPath_*(), FPDFText_*(), and
// FPDFImageObj_*().
//
//   annot  - handle to an annotation.
//   obj    - handle to the object that |annot| needs to update.
//
// Return true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_UpdateObject(FPDF_ANNOTATION annot, FPDF_PAGEOBJECT obj);

// Experimental API.
// Add a new InkStroke, represented by an array of points, to the InkList of
// |annot|. The API creates an InkList if one doesn't already exist in |annot|.
// This API works only for ink annotations. Please refer to ISO 32000-1:2008
// spec, section 12.5.6.13.
//
//   annot       - handle to an annotation.
//   points      - pointer to a FS_POINTF array representing input points.
//   point_count - number of elements in |points| array. This should not exceed
//                 the maximum value that can be represented by an int32_t).
//
// Returns the 0-based index at which the new InkStroke is added in the InkList
// of the |annot|. Returns -1 on failure.
FPDF_EXPORT int FPDF_CALLCONV FPDFAnnot_AddInkStroke(FPDF_ANNOTATION annot,
                                                     const FS_POINTF* points,
                                                     size_t point_count);

// Experimental API.
// Removes an InkList in |annot|.
// This API works only for ink annotations.
//
//   annot  - handle to an annotation.
//
// Return true on successful removal of /InkList entry from context of the
// non-null ink |annot|. Returns false on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_RemoveInkList(FPDF_ANNOTATION annot);

// Experimental API.
// Add |obj| to |annot|. |obj| must have been created by
// FPDFPageObj_CreateNew{Path|Rect}() or FPDFPageObj_New{Text|Image}Obj(), and
// will be owned by |annot|. Note that an |obj| cannot belong to more than one
// |annot|. Currently, only ink and stamp annotations are supported by this API.
// Also note that only path, image, and text objects have APIs for creation.
//
//   annot  - handle to an annotation.
//   obj    - handle to the object that is to be added to |annot|.
//
// Return true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_AppendObject(FPDF_ANNOTATION annot, FPDF_PAGEOBJECT obj);

// Experimental API.
// Get the total number of objects in |annot|, including path objects, text
// objects, external objects, image objects, and shading objects.
//
//   annot  - handle to an annotation.
//
// Returns the number of objects in |annot|.
FPDF_EXPORT int FPDF_CALLCONV FPDFAnnot_GetObjectCount(FPDF_ANNOTATION annot);

// Experimental API.
// Get the object in |annot| at |index|.
//
//   annot  - handle to an annotation.
//   index  - the index of the object.
//
// Return a handle to the object, or NULL on failure.
FPDF_EXPORT FPDF_PAGEOBJECT FPDF_CALLCONV
FPDFAnnot_GetObject(FPDF_ANNOTATION annot, int index);

// Experimental API.
// Remove the object in |annot| at |index|.
//
//   annot  - handle to an annotation.
//   index  - the index of the object to be removed.
//
// Return true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_RemoveObject(FPDF_ANNOTATION annot, int index);

// Experimental API.
// Set the color of an annotation. Fails when called on annotations with
// appearance streams already defined; instead use
// FPDFPageObj_Set{Stroke|Fill}Color().
//
//   annot    - handle to an annotation.
//   type     - type of the color to be set.
//   R, G, B  - buffer to hold the RGB value of the color. Ranges from 0 to 255.
//   A        - buffer to hold the opacity. Ranges from 0 to 255.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_SetColor(FPDF_ANNOTATION annot,
                                                       FPDFANNOT_COLORTYPE type,
                                                       unsigned int R,
                                                       unsigned int G,
                                                       unsigned int B,
                                                       unsigned int A);

// Experimental API.
// Get the color of an annotation. If no color is specified, default to yellow
// for highlight annotation, black for all else. Fails when called on
// annotations with appearance streams already defined; instead use
// FPDFPageObj_Get{Stroke|Fill}Color().
//
//   annot    - handle to an annotation.
//   type     - type of the color requested.
//   R, G, B  - buffer to hold the RGB value of the color. Ranges from 0 to 255.
//   A        - buffer to hold the opacity. Ranges from 0 to 255.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_GetColor(FPDF_ANNOTATION annot,
                                                       FPDFANNOT_COLORTYPE type,
                                                       unsigned int* R,
                                                       unsigned int* G,
                                                       unsigned int* B,
                                                       unsigned int* A);

// Experimental API.
// Check if the annotation is of a type that has attachment points
// (i.e. quadpoints). Quadpoints are the vertices of the rectangle that
// encompasses the texts affected by the annotation. They provide the
// coordinates in the page where the annotation is attached. Only text markup
// annotations (i.e. highlight, strikeout, squiggly, and underline) and link
// annotations have quadpoints.
//
//   annot  - handle to an annotation.
//
// Returns true if the annotation is of a type that has quadpoints, false
// otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_HasAttachmentPoints(FPDF_ANNOTATION annot);

// Experimental API.
// Replace the attachment points (i.e. quadpoints) set of an annotation at
// |quad_index|. This index needs to be within the result of
// FPDFAnnot_CountAttachmentPoints().
// If the annotation's appearance stream is defined and this annotation is of a
// type with quadpoints, then update the bounding box too if the new quadpoints
// define a bigger one.
//
//   annot       - handle to an annotation.
//   quad_index  - index of the set of quadpoints.
//   quad_points - the quadpoints to be set.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetAttachmentPoints(FPDF_ANNOTATION annot,
                              size_t quad_index,
                              const FS_QUADPOINTSF* quad_points);

// Experimental API.
// Append to the list of attachment points (i.e. quadpoints) of an annotation.
// If the annotation's appearance stream is defined and this annotation is of a
// type with quadpoints, then update the bounding box too if the new quadpoints
// define a bigger one.
//
//   annot       - handle to an annotation.
//   quad_points - the quadpoints to be set.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_AppendAttachmentPoints(FPDF_ANNOTATION annot,
                                 const FS_QUADPOINTSF* quad_points);

// Experimental API.
// Get the number of sets of quadpoints of an annotation.
//
//   annot  - handle to an annotation.
//
// Returns the number of sets of quadpoints, or 0 on failure.
FPDF_EXPORT size_t FPDF_CALLCONV
FPDFAnnot_CountAttachmentPoints(FPDF_ANNOTATION annot);

// Experimental API.
// Get the attachment points (i.e. quadpoints) of an annotation.
//
//   annot       - handle to an annotation.
//   quad_index  - index of the set of quadpoints.
//   quad_points - receives the quadpoints; must not be NULL.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetAttachmentPoints(FPDF_ANNOTATION annot,
                              size_t quad_index,
                              FS_QUADPOINTSF* quad_points);

// Experimental API.
// Set the annotation rectangle defining the location of the annotation. If the
// annotation's appearance stream is defined and this annotation is of a type
// without quadpoints, then update the bounding box too if the new rectangle
// defines a bigger one.
//
//   annot  - handle to an annotation.
//   rect   - the annotation rectangle to be set.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_SetRect(FPDF_ANNOTATION annot,
                                                      const FS_RECTF* rect);

// Experimental API.
// Get the annotation rectangle defining the location of the annotation.
//
//   annot  - handle to an annotation.
//   rect   - receives the rectangle; must not be NULL.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_GetRect(FPDF_ANNOTATION annot,
                                                      FS_RECTF* rect);

// Experimental API.
// Get the vertices of a polygon or polyline annotation. |buffer| is an array of
// points of the annotation. If |length| is less than the returned length, or
// |annot| or |buffer| is NULL, |buffer| will not be modified.
//
//   annot  - handle to an annotation, as returned by e.g. FPDFPage_GetAnnot()
//   buffer - buffer for holding the points.
//   length - length of the buffer in points.
//
// Returns the number of points if the annotation is of type polygon or
// polyline, 0 otherwise.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetVertices(FPDF_ANNOTATION annot,
                      FS_POINTF* buffer,
                      unsigned long length);

// Experimental API.
// Get the number of paths in the ink list of an ink annotation.
//
//   annot  - handle to an annotation, as returned by e.g. FPDFPage_GetAnnot()
//
// Returns the number of paths in the ink list if the annotation is of type ink,
// 0 otherwise.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetInkListCount(FPDF_ANNOTATION annot);

// Experimental API.
// Get a path in the ink list of an ink annotation. |buffer| is an array of
// points of the path. If |length| is less than the returned length, or |annot|
// or |buffer| is NULL, |buffer| will not be modified.
//
//   annot  - handle to an annotation, as returned by e.g. FPDFPage_GetAnnot()
//   path_index - index of the path
//   buffer - buffer for holding the points.
//   length - length of the buffer in points.
//
// Returns the number of points of the path if the annotation is of type ink, 0
// otherwise.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetInkListPath(FPDF_ANNOTATION annot,
                         unsigned long path_index,
                         FS_POINTF* buffer,
                         unsigned long length);

// Experimental API.
// Get the starting and ending coordinates of a line annotation.
//
//   annot  - handle to an annotation, as returned by e.g. FPDFPage_GetAnnot()
//   start - starting point
//   end - ending point
//
// Returns true if the annotation is of type line, |start| and |end| are not
// NULL, false otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_GetLine(FPDF_ANNOTATION annot,
                                                      FS_POINTF* start,
                                                      FS_POINTF* end);

// Experimental API.
// Set the characteristics of the annotation's border (rounded rectangle).
//
//   annot              - handle to an annotation
//   horizontal_radius  - horizontal corner radius, in default user space units
//   vertical_radius    - vertical corner radius, in default user space units
//   border_width       - border width, in default user space units
//
// Returns true if setting the border for |annot| succeeds, false otherwise.
//
// If |annot| contains an appearance stream that overrides the border values,
// then the appearance stream will be removed on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_SetBorder(FPDF_ANNOTATION annot,
                                                        float horizontal_radius,
                                                        float vertical_radius,
                                                        float border_width);

// Experimental API.
// Get the characteristics of the annotation's border (rounded rectangle).
//
//   annot              - handle to an annotation
//   horizontal_radius  - horizontal corner radius, in default user space units
//   vertical_radius    - vertical corner radius, in default user space units
//   border_width       - border width, in default user space units
//
// Returns true if |horizontal_radius|, |vertical_radius| and |border_width| are
// not NULL, false otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetBorder(FPDF_ANNOTATION annot,
                    float* horizontal_radius,
                    float* vertical_radius,
                    float* border_width);

// Experimental API.
// Get the JavaScript of an event of the annotation's additional actions.
// |buffer| is only modified if |buflen| is large enough to hold the whole
// JavaScript string. If |buflen| is smaller, the total size of the JavaScript
// is still returned, but nothing is copied.  If there is no JavaScript for
// |event| in |annot|, an empty string is written to |buf| and 2 is returned,
// denoting the size of the null terminator in the buffer.  On other errors,
// nothing is written to |buffer| and 0 is returned.
//
//    hHandle     -   handle to the form fill module, returned by
//                    FPDFDOC_InitFormFillEnvironment().
//    annot       -   handle to an interactive form annotation.
//    event       -   event type, one of the FPDF_ANNOT_AACTION_* values.
//    buffer      -   buffer for holding the value string, encoded in UTF-16LE.
//    buflen      -   length of the buffer in bytes.
//
// Returns the length of the string value in bytes, including the 2-byte
// null terminator.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetFormAdditionalActionJavaScript(FPDF_FORMHANDLE hHandle,
                                            FPDF_ANNOTATION annot,
                                            int event,
                                            FPDF_WCHAR* buffer,
                                            unsigned long buflen);

// Experimental API.
// Check if |annot|'s dictionary has |key| as a key.
//
//   annot  - handle to an annotation.
//   key    - the key to look for, encoded in UTF-8.
//
// Returns true if |key| exists.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_HasKey(FPDF_ANNOTATION annot,
                                                     FPDF_BYTESTRING key);

// Experimental API.
// Get the type of the value corresponding to |key| in |annot|'s dictionary.
//
//   annot  - handle to an annotation.
//   key    - the key to look for, encoded in UTF-8.
//
// Returns the type of the dictionary value.
FPDF_EXPORT FPDF_OBJECT_TYPE FPDF_CALLCONV
FPDFAnnot_GetValueType(FPDF_ANNOTATION annot, FPDF_BYTESTRING key);

// Experimental API.
// Set the string value corresponding to |key| in |annot|'s dictionary,
// overwriting the existing value if any. The value type would be
// FPDF_OBJECT_STRING after this function call succeeds.
//
//   annot  - handle to an annotation.
//   key    - the key to the dictionary entry to be set, encoded in UTF-8.
//   value  - the string value to be set, encoded in UTF-16LE.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetStringValue(FPDF_ANNOTATION annot,
                         FPDF_BYTESTRING key,
                         FPDF_WIDESTRING value);

// Experimental API.
// Get the string value corresponding to |key| in |annot|'s dictionary. |buffer|
// is only modified if |buflen| is longer than the length of contents. Note that
// if |key| does not exist in the dictionary or if |key|'s corresponding value
// in the dictionary is not a string (i.e. the value is not of type
// FPDF_OBJECT_STRING or FPDF_OBJECT_NAME), then an empty string would be copied
// to |buffer| and the return value would be 2. On other errors, nothing would
// be added to |buffer| and the return value would be 0.
//
//   annot  - handle to an annotation.
//   key    - the key to the requested dictionary entry, encoded in UTF-8.
//   buffer - buffer for holding the value string, encoded in UTF-16LE.
//   buflen - length of the buffer in bytes.
//
// Returns the length of the string value in bytes.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetStringValue(FPDF_ANNOTATION annot,
                         FPDF_BYTESTRING key,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen);

// Experimental API.
// Get the float value corresponding to |key| in |annot|'s dictionary. Writes
// value to |value| and returns True if |key| exists in the dictionary and
// |key|'s corresponding value is a number (FPDF_OBJECT_NUMBER), False
// otherwise.
//
//   annot  - handle to an annotation.
//   key    - the key to the requested dictionary entry, encoded in UTF-8.
//   value  - receives the value, must not be NULL.
//
// Returns True if value found, False otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetNumberValue(FPDF_ANNOTATION annot,
                         FPDF_BYTESTRING key,
                         float* value);

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
// Removes /EMBD_Metadata from every annotation in |document|. This does not
// remove standard annotation fields or appearance streams.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDocument_ClearEmbedMetadata(FPDF_DOCUMENT document);

// Experimental API.
// Set the AP (appearance string) in |annot|'s dictionary for a given
// |appearanceMode|.
//
//   annot          - handle to an annotation.
//   appearanceMode - the appearance mode (normal, rollover or down) for which
//                    to get the AP.
//   value          - the string value to be set, encoded in UTF-16LE. If
//                    nullptr is passed, the AP is cleared for that mode. If the
//                    mode is Normal, APs for all modes are cleared.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetAP(FPDF_ANNOTATION annot,
                FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                FPDF_WIDESTRING value);

// Experimental API.
// Get the AP (appearance string) from |annot|'s dictionary for a given
// |appearanceMode|.
// |buffer| is only modified if |buflen| is large enough to hold the whole AP
// string. If |buflen| is smaller, the total size of the AP is still returned,
// but nothing is copied.
// If there is no appearance stream for |annot| in |appearanceMode|, an empty
// string is written to |buf| and 2 is returned.
// On other errors, nothing is written to |buffer| and 0 is returned.
//
//   annot          - handle to an annotation.
//   appearanceMode - the appearance mode (normal, rollover or down) for which
//                    to get the AP.
//   buffer         - buffer for holding the value string, encoded in UTF-16LE.
//   buflen         - length of the buffer in bytes.
//
// Returns the length of the string value in bytes.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetAP(FPDF_ANNOTATION annot,
                FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                FPDF_WCHAR* buffer,
                unsigned long buflen);

// Experimental API.
// Get the annotation corresponding to |key| in |annot|'s dictionary. Common
// keys for linking annotations include "IRT" and "Popup". Must call
// FPDFPage_CloseAnnot() when the annotation returned by this function is no
// longer needed.
//
//   annot  - handle to an annotation.
//   key    - the key to the requested dictionary entry, encoded in UTF-8.
//
// Returns a handle to the linked annotation object, or NULL on failure.
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDFAnnot_GetLinkedAnnot(FPDF_ANNOTATION annot, FPDF_BYTESTRING key);

// Experimental API.
// Get the annotation flags of |annot|.
//
//   annot    - handle to an annotation.
//
// Returns the annotation flags.
FPDF_EXPORT int FPDF_CALLCONV FPDFAnnot_GetFlags(FPDF_ANNOTATION annot);

// Experimental API.
// Set the |annot|'s flags to be of the value |flags|.
//
//   annot      - handle to an annotation.
//   flags      - the flag values to be set.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_SetFlags(FPDF_ANNOTATION annot,
                                                       int flags);

// Experimental API.
// Get the annotation flags of |annot|.
//
//    hHandle     -   handle to the form fill module, returned by
//                    FPDFDOC_InitFormFillEnvironment().
//    annot       -   handle to an interactive form annotation.
//
// Returns the annotation flags specific to interactive forms.
FPDF_EXPORT int FPDF_CALLCONV
FPDFAnnot_GetFormFieldFlags(FPDF_FORMHANDLE handle, FPDF_ANNOTATION annot);

// Experimental API.
// Sets the form field flags for an interactive form annotation.
//
//   handle       -   the handle to the form fill module, returned by
//                    FPDFDOC_InitFormFillEnvironment().
//   annot        -   handle to an interactive form annotation.
//   flags        -   the form field flags to be set.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetFormFieldFlags(FPDF_FORMHANDLE handle,
                            FPDF_ANNOTATION annot,
                            int flags);

// Experimental API.
// Retrieves an interactive form annotation whose rectangle contains a given
// point on a page. Must call FPDFPage_CloseAnnot() when the annotation returned
// is no longer needed.
//
//
//    hHandle     -   handle to the form fill module, returned by
//                    FPDFDOC_InitFormFillEnvironment().
//    page        -   handle to the page, returned by FPDF_LoadPage function.
//    point       -   position in PDF "user space".
//
// Returns the interactive form annotation whose rectangle contains the given
// coordinates on the page. If there is no such annotation, return NULL.
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDFAnnot_GetFormFieldAtPoint(FPDF_FORMHANDLE hHandle,
                              FPDF_PAGE page,
                              const FS_POINTF* point);

// Experimental API.
// Gets the name of |annot|, which is an interactive form annotation.
// |buffer| is only modified if |buflen| is longer than the length of contents.
// In case of error, nothing will be added to |buffer| and the return value will
// be 0. Note that return value of empty string is 2 for "\0\0".
//
//    hHandle     -   handle to the form fill module, returned by
//                    FPDFDOC_InitFormFillEnvironment().
//    annot       -   handle to an interactive form annotation.
//    buffer      -   buffer for holding the name string, encoded in UTF-16LE.
//    buflen      -   length of the buffer in bytes.
//
// Returns the length of the string value in bytes.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetFormFieldName(FPDF_FORMHANDLE hHandle,
                           FPDF_ANNOTATION annot,
                           FPDF_WCHAR* buffer,
                           unsigned long buflen);

// Experimental API.
// Gets the alternate name of |annot|, which is an interactive form annotation.
// |buffer| is only modified if |buflen| is longer than the length of contents.
// In case of error, nothing will be added to |buffer| and the return value will
// be 0. Note that return value of empty string is 2 for "\0\0".
//
//    hHandle     -   handle to the form fill module, returned by
//                    FPDFDOC_InitFormFillEnvironment().
//    annot       -   handle to an interactive form annotation.
//    buffer      -   buffer for holding the alternate name string, encoded in
//                    UTF-16LE.
//    buflen      -   length of the buffer in bytes.
//
// Returns the length of the string value in bytes.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetFormFieldAlternateName(FPDF_FORMHANDLE hHandle,
                                    FPDF_ANNOTATION annot,
                                    FPDF_WCHAR* buffer,
                                    unsigned long buflen);

// Experimental API.
// Gets the form field type of |annot|, which is an interactive form annotation.
//
//    hHandle     -   handle to the form fill module, returned by
//                    FPDFDOC_InitFormFillEnvironment().
//    annot       -   handle to an interactive form annotation.
//
// Returns the type of the form field (one of the FPDF_FORMFIELD_* values) on
// success. Returns -1 on error.
// See field types in fpdf_formfill.h.
FPDF_EXPORT int FPDF_CALLCONV
FPDFAnnot_GetFormFieldType(FPDF_FORMHANDLE hHandle, FPDF_ANNOTATION annot);

// Experimental API.
// Gets the value of |annot|, which is an interactive form annotation.
// |buffer| is only modified if |buflen| is longer than the length of contents.
// In case of error, nothing will be added to |buffer| and the return value will
// be 0. Note that return value of empty string is 2 for "\0\0".
//
//    hHandle     -   handle to the form fill module, returned by
//                    FPDFDOC_InitFormFillEnvironment().
//    annot       -   handle to an interactive form annotation.
//    buffer      -   buffer for holding the value string, encoded in UTF-16LE.
//    buflen      -   length of the buffer in bytes.
//
// Returns the length of the string value in bytes.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetFormFieldValue(FPDF_FORMHANDLE hHandle,
                            FPDF_ANNOTATION annot,
                            FPDF_WCHAR* buffer,
                            unsigned long buflen);

// Experimental API.
// Get the number of options in the |annot|'s "Opt" dictionary. Intended for
// use with listbox and combobox widget annotations.
//
//   hHandle - handle to the form fill module, returned by
//             FPDFDOC_InitFormFillEnvironment.
//   annot   - handle to an annotation.
//
// Returns the number of options in "Opt" dictionary on success. Return value
// will be -1 if annotation does not have an "Opt" dictionary or other error.
FPDF_EXPORT int FPDF_CALLCONV FPDFAnnot_GetOptionCount(FPDF_FORMHANDLE hHandle,
                                                       FPDF_ANNOTATION annot);

// Experimental API.
// Get the string value for the label of the option at |index| in |annot|'s
// "Opt" dictionary. Intended for use with listbox and combobox widget
// annotations. |buffer| is only modified if |buflen| is longer than the length
// of contents. If index is out of range or in case of other error, nothing
// will be added to |buffer| and the return value will be 0. Note that
// return value of empty string is 2 for "\0\0".
//
//   hHandle - handle to the form fill module, returned by
//             FPDFDOC_InitFormFillEnvironment.
//   annot   - handle to an annotation.
//   index   - numeric index of the option in the "Opt" array
//   buffer  - buffer for holding the value string, encoded in UTF-16LE.
//   buflen  - length of the buffer in bytes.
//
// Returns the length of the string value in bytes.
// If |annot| does not have an "Opt" array, |index| is out of range or if any
// other error occurs, returns 0.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetOptionLabel(FPDF_FORMHANDLE hHandle,
                         FPDF_ANNOTATION annot,
                         int index,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen);

// Experimental API.
// Determine whether or not the option at |index| in |annot|'s "Opt" dictionary
// is selected. Intended for use with listbox and combobox widget annotations.
//
//   handle  - handle to the form fill module, returned by
//             FPDFDOC_InitFormFillEnvironment.
//   annot   - handle to an annotation.
//   index   - numeric index of the option in the "Opt" array.
//
// Returns true if the option at |index| in |annot|'s "Opt" dictionary is
// selected, false otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_IsOptionSelected(FPDF_FORMHANDLE handle,
                           FPDF_ANNOTATION annot,
                           int index);

// Experimental API.
// Get the float value of the font size for an |annot| with variable text.
// If 0, the font is to be auto-sized: its size is computed as a function of
// the height of the annotation rectangle.
//
//   hHandle - handle to the form fill module, returned by
//             FPDFDOC_InitFormFillEnvironment.
//   annot   - handle to an annotation.
//   value   - Required. Float which will be set to font size on success.
//
// Returns true if the font size was set in |value|, false on error or if
// |value| not provided.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetFontSize(FPDF_FORMHANDLE hHandle,
                      FPDF_ANNOTATION annot,
                      float* value);

// Experimental API.
// Set the text color of an annotation.
//
//   handle   - handle to the form fill module, returned by
//              FPDFDOC_InitFormFillEnvironment.
//   annot    - handle to an annotation.
//   R        - the red component for the text color.
//   G        - the green component for the text color.
//   B        - the blue component for the text color.
//
// Returns true if successful.
//
// Currently supported subtypes: freetext.
// The range for the color components is 0 to 255.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetFontColor(FPDF_FORMHANDLE handle,
                       FPDF_ANNOTATION annot,
                       unsigned int R,
                       unsigned int G,
                       unsigned int B);

// Experimental API.
// Get the RGB value of the font color for an |annot| with variable text.
//
//   hHandle  - handle to the form fill module, returned by
//              FPDFDOC_InitFormFillEnvironment.
//   annot    - handle to an annotation.
//   R, G, B  - buffer to hold the RGB value of the color. Ranges from 0 to 255.
//
// Returns true if the font color was set, false on error or if the font
// color was not provided.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetFontColor(FPDF_FORMHANDLE hHandle,
                       FPDF_ANNOTATION annot,
                       unsigned int* R,
                       unsigned int* G,
                       unsigned int* B);

// Experimental API.
// Determine if |annot| is a form widget that is checked. Intended for use with
// checkbox and radio button widgets.
//
//   hHandle - handle to the form fill module, returned by
//             FPDFDOC_InitFormFillEnvironment.
//   annot   - handle to an annotation.
//
// Returns true if |annot| is a form widget and is checked, false otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_IsChecked(FPDF_FORMHANDLE hHandle,
                                                        FPDF_ANNOTATION annot);

// Experimental API.
// Set the list of focusable annotation subtypes. Annotations of subtype
// FPDF_ANNOT_WIDGET are by default focusable. New subtypes set using this API
// will override the existing subtypes.
//
//   hHandle  - handle to the form fill module, returned by
//              FPDFDOC_InitFormFillEnvironment.
//   subtypes - list of annotation subtype which can be tabbed over.
//   count    - total number of annotation subtype in list.
// Returns true if list of annotation subtype is set successfully, false
// otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetFocusableSubtypes(FPDF_FORMHANDLE hHandle,
                               const FPDF_ANNOTATION_SUBTYPE* subtypes,
                               size_t count);

// Experimental API.
// Get the count of focusable annotation subtypes as set by host
// for a |hHandle|.
//
//   hHandle  - handle to the form fill module, returned by
//              FPDFDOC_InitFormFillEnvironment.
// Returns the count of focusable annotation subtypes or -1 on error.
// Note : Annotations of type FPDF_ANNOT_WIDGET are by default focusable.
FPDF_EXPORT int FPDF_CALLCONV
FPDFAnnot_GetFocusableSubtypesCount(FPDF_FORMHANDLE hHandle);

// Experimental API.
// Get the list of focusable annotation subtype as set by host.
//
//   hHandle  - handle to the form fill module, returned by
//              FPDFDOC_InitFormFillEnvironment.
//   subtypes - receives the list of annotation subtype which can be tabbed
//              over. Caller must have allocated |subtypes| more than or
//              equal to the count obtained from
//              FPDFAnnot_GetFocusableSubtypesCount() API.
//   count    - size of |subtypes|.
// Returns true on success and set list of annotation subtype to |subtypes|,
// false otherwise.
// Note : Annotations of type FPDF_ANNOT_WIDGET are by default focusable.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetFocusableSubtypes(FPDF_FORMHANDLE hHandle,
                               FPDF_ANNOTATION_SUBTYPE* subtypes,
                               size_t count);

// Experimental API.
// Gets FPDF_LINK object for |annot|. Intended to use for link annotations.
//
//   annot   - handle to an annotation.
//
// Returns FPDF_LINK from the FPDF_ANNOTATION and NULL on failure,
// if the input annot is NULL or input annot's subtype is not link.
FPDF_EXPORT FPDF_LINK FPDF_CALLCONV FPDFAnnot_GetLink(FPDF_ANNOTATION annot);

// Experimental API.
// Gets the count of annotations in the |annot|'s control group.
// A group of interactive form annotations is collectively called a form
// control group. Here, |annot|, an interactive form annotation, should be
// either a radio button or a checkbox.
//
//   hHandle - handle to the form fill module, returned by
//             FPDFDOC_InitFormFillEnvironment.
//   annot   - handle to an annotation.
//
// Returns number of controls in its control group or -1 on error.
FPDF_EXPORT int FPDF_CALLCONV
FPDFAnnot_GetFormControlCount(FPDF_FORMHANDLE hHandle, FPDF_ANNOTATION annot);

// Experimental API.
// Gets the index of |annot| in |annot|'s control group.
// A group of interactive form annotations is collectively called a form
// control group. Here, |annot|, an interactive form annotation, should be
// either a radio button or a checkbox.
//
//   hHandle - handle to the form fill module, returned by
//             FPDFDOC_InitFormFillEnvironment.
//   annot   - handle to an annotation.
//
// Returns index of a given |annot| in its control group or -1 on error.
FPDF_EXPORT int FPDF_CALLCONV
FPDFAnnot_GetFormControlIndex(FPDF_FORMHANDLE hHandle, FPDF_ANNOTATION annot);

// Experimental API.
// Gets the export value of |annot| which is an interactive form annotation.
// Intended for use with radio button and checkbox widget annotations.
// |buffer| is only modified if |buflen| is longer than the length of contents.
// In case of error, nothing will be added to |buffer| and the return value
// will be 0. Note that return value of empty string is 2 for "\0\0".
//
//    hHandle     -   handle to the form fill module, returned by
//                    FPDFDOC_InitFormFillEnvironment().
//    annot       -   handle to an interactive form annotation.
//    buffer      -   buffer for holding the value string, encoded in UTF-16LE.
//    buflen      -   length of the buffer in bytes.
//
// Returns the length of the string value in bytes.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetFormFieldExportValue(FPDF_FORMHANDLE hHandle,
                                  FPDF_ANNOTATION annot,
                                  FPDF_WCHAR* buffer,
                                  unsigned long buflen);

// Experimental API.
// Add a URI action to |annot|, overwriting the existing action, if any.
//
//   annot  - handle to a link annotation.
//   uri    - the URI to be set, encoded in 7-bit ASCII.
//
// Returns true if successful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_SetURI(FPDF_ANNOTATION annot,
                                                     const char* uri);

// Experimental API.
// Get the attachment from |annot|.
//
//   annot - handle to a file annotation.
//
// Returns the handle to the attachment object, or NULL on failure.
FPDF_EXPORT FPDF_ATTACHMENT FPDF_CALLCONV
FPDFAnnot_GetFileAttachment(FPDF_ANNOTATION annot);

// Experimental API.
// Add an embedded file with |name| to |annot|.
//
//   annot    - handle to a file annotation.
//   name     - name of the new attachment.
//
// Returns a handle to the new attachment object, or NULL on failure.
FPDF_EXPORT FPDF_ATTACHMENT FPDF_CALLCONV
FPDFAnnot_AddFileAttachment(FPDF_ANNOTATION annot, FPDF_WIDESTRING name);

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
// Set the opacity of an annotaion.
//
// annot - handle to an annotation.
// alpha - the opacity. Ranges from 0 to 255.
//
// Returns true if succesful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetOpacity(FPDF_ANNOTATION annot,
                     unsigned int alpha /* 0 = transparent … 255 = opaque */);

// Experimental EmbedPDF Extension API.
// Get the opacity of an annotation.
//
// annot - handle to an annotation.
// alpha - buffer to hold the opacity. Ranges from 0 to 255.
//
// Returns true if succesful.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetOpacity(FPDF_ANNOTATION annot, unsigned int* alpha /* 0-255 */);

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
// Set the rectangle differences (/RD) for a supported annotation.
//
//   annot            - handle to a square, circle, caret, free-text, or polygon
//                      annotation.
//   left, bottom,    - the native PDF /RD values for each side.
//   right, top         PDFium core treats /RD as [left, bottom, right, top].
//
// Returns true on success, false on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetRectangleDifferences(FPDF_ANNOTATION annot,
                                  float left,
                                  float bottom,
                                  float right,
                                  float top);

// Experimental EmbedPDF Extension API.
// Remove the rectangle differences (/RD) entry from a supported annotation.
//
//   annot  - handle to a square, circle, caret, free-text, or polygon
//            annotation.
//
// Returns true on success, false on failure or unsupported subtype.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearRectangleDifferences(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Get the number of entries in the dash pattern for a dashed border. This
// function handles both the modern /BS dictionary and the legacy /Border
// array.
//
//   annot  - handle to an annotation.
//
// Returns the number of entries in the dash pattern array, or 0 if the border
// is not dashed or has no pattern.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetBorderDashPatternCount(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Get the dash pattern for a dashed border. This function handles both the
// modern /BS dictionary and the legacy /Border array.
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
// Sets (or replaces) the dash pattern on an annotation's border.
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

// Experimental EmbedPDF Extension API (rich text, Phase D).
// Which engine lays out a plain FreeText (one without /RC): the CPVT engine
// (today's default) or the rich text engine that also draws /RC. An
// annotation with /RC always uses the rich engine. Session state of the
// document handle.
#define EPDF_FREETEXT_LAYOUT_CPVT 0
#define EPDF_FREETEXT_LAYOUT_RICH 1
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_SetFreeTextLayout(FPDF_DOCUMENT document, int layout);
// Returns one of EPDF_FREETEXT_LAYOUT_*, or -1 for an invalid document.
FPDF_EXPORT int FPDF_CALLCONV EPDFDoc_GetFreeTextLayout(FPDF_DOCUMENT document);

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
// Set the text alignment of a FreeText annotation.
//
//   annot    - handle to an annotation.
//   alignment - the text alignment to be set.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetTextAlignment(FPDF_ANNOTATION annot,
                           FPDF_TEXT_ALIGNMENT alignment);

// Experimental EmbedPDF Extension API.
// Get the text alignment of a FreeText annotation.
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
// CTM.
//
//   annot - handle to a Stamp annotation.
//   fit   - one of EPDF_STAMP_FIT_*.
//
// Returns true on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_UpdateAppearanceToRect(FPDF_ANNOTATION annot, EPDF_STAMP_FIT fit);

// Experimental EmbedPDF Extension API.
// Create an annotation. (the difference from FPDFPage_CreateAnnot is that it
// creates an indirect object)
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

// Experimental EmbedPDF Extension API.
// Generate the appearance stream for a form field widget annotation.
// The standard EPDFAnnot_GenerateAppearance does NOT handle Widget subtypes.
// This function reads /FT (from the annotation dict or its /Parent) and
// dispatches to the appropriate form AP generator:
//   - /Tx  -> text field appearance
//   - /Ch  -> combo box or list box appearance (based on /Ff flags)
//   - /Btn -> returns true (buttons use custom AP/N state-based appearances)
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

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_FPDF_ANNOT_H_
