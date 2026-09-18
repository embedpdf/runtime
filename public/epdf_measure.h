// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
#ifndef PUBLIC_EPDF_MEASURE_H_
#define PUBLIC_EPDF_MEASURE_H_
// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"
// NOLINTNEXTLINE(build/include)
#include "fpdf_annot.h"

#ifdef __cplusplus
extern "C" {
#endif

// All handles are borrowed, thread-confined and owned by their originating
// annotation/page. Close children before the document; never use a handle after
// its originating annotation/page closes. Structural mutations through this API
// invalidate affected handles across ALL open handles to the same PDF owner.
// Reacquire them after invalidation. Getters and rejected setters never promote
// PDF objects in a layer. Generic raw dictionary edits must not be mixed with
// retained measurement handles: reacquire after such edits.
//
// A /Measure dictionary (ISO 32000-2 Table 266), owned by an annotation or a
// viewport.
typedef struct epdf_measure_t__* EPDF_MEASURE;
// One number format dictionary (Table 268) inside a measure's /X /Y /D /A /T /S
// array.
typedef struct epdf_numberformat_t__* EPDF_NUMBERFORMAT;
// One viewport dictionary (Table 265) inside a page's /VP array.
typedef struct epdf_viewport_t__* EPDF_VIEWPORT;

typedef enum EPDF_MEASURE_SUBTYPE {
  EPDF_MEASURE_SUBTYPE_UNKNOWN = 0,
  EPDF_MEASURE_SUBTYPE_RL = 1,   // rectilinear — the only writable subtype
  EPDF_MEASURE_SUBTYPE_GEO = 2,  // geospatial — readable as "present, not ours"
} EPDF_MEASURE_SUBTYPE;

// The number-format arrays of a rectilinear measure (Table 267).
typedef enum EPDF_MEASURE_AXIS {
  EPDF_MEASURE_AXIS_X = 0,         // /X  user space → largest x unit
  EPDF_MEASURE_AXIS_Y = 1,         // /Y  only when y differs from x
  EPDF_MEASURE_AXIS_DISTANCE = 2,  // /D  from x units
  EPDF_MEASURE_AXIS_AREA = 3,      // /A  from x units squared
  EPDF_MEASURE_AXIS_ANGLE = 4,     // /T  from degrees
  EPDF_MEASURE_AXIS_SLOPE = 5,     // /S
} EPDF_MEASURE_AXIS;

typedef enum EPDF_MEASURE_FRACTION {  // /F
  EPDF_MEASURE_FRACTION_DECIMAL = 0,
  EPDF_MEASURE_FRACTION_FRACTION = 1,
  EPDF_MEASURE_FRACTION_ROUND = 2,
  EPDF_MEASURE_FRACTION_TRUNCATE = 3,
} EPDF_MEASURE_FRACTION;

typedef enum EPDF_MEASURE_LABEL_POSITION {  // /O
  EPDF_MEASURE_LABEL_SUFFIX = 0,
  EPDF_MEASURE_LABEL_PREFIX = 1,
} EPDF_MEASURE_LABEL_POSITION;

typedef enum EPDF_MEASURE_TEXT {  // the text entries of a number format
  EPDF_MEASURE_TEXT_THOUSANDS_SEPARATOR = 0,  // /RT
  EPDF_MEASURE_TEXT_DECIMAL_SEPARATOR = 1,    // /RD
  EPDF_MEASURE_TEXT_PREFIX_SPACING = 2,       // /PS
  EPDF_MEASURE_TEXT_SUFFIX_SPACING = 3,       // /SS
} EPDF_MEASURE_TEXT;

typedef enum EPDF_CAPTION_POSITION {  // /CP
  EPDF_CAPTION_INLINE = 0,
  EPDF_CAPTION_TOP = 1,
} EPDF_CAPTION_POSITION;

// A displacement in the directed line's axes, in default user-space units.
// Positive perpendicular is rotate(end-start, +90 degrees) in y-up space.
// This is deliberately not an FS_POINTF: it is not a PDF page position.
typedef struct EPDF_CAPTION_OFFSET {
  float along;
  float perpendicular;
} EPDF_CAPTION_OFFSET;

// ── 1. Measure on an annotation
// ─────────────────────────────────────────────────────────────

// Experimental EmbedPDF Extension API.
// The annotation's /Measure dictionary, or NULL when there is none. Permissive
// about the annotation subtype, so vendor measurements on Circle or Ink are
// readable. The handle is valid while |annot| is open; it is never closed.
// Reading through it never modifies the document (in a layer: never promotes).
FPDF_EXPORT EPDF_MEASURE FPDF_CALLCONV
EPDFAnnot_GetMeasure(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Create the annotation's /Measure as an EMPTY rectilinear measure (/Type
// /Measure /Subtype /RL), replacing any existing rectilinear one, and return
// it. A shared indirect /Measure is detached first (a direct copy on this
// annotation), so no other annotation changes. A reset is a structural
// mutation: the previous EPDF_MEASURE and every EPDF_NUMBERFORMAT obtained
// from it become stale. Returns NULL when an existing measure has any non-RL
// subtype (never overwritten) or |annot| is invalid.
FPDF_EXPORT EPDF_MEASURE FPDF_CALLCONV
EPDFAnnot_AddMeasure(FPDF_ANNOTATION annot);

// Experimental EmbedPDF Extension API.
// Remove /Measure. Returns true when the annotation is valid, whether or not it
// had one. Every handle obtained from the removed measure becomes stale (fails
// closed).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_RemoveMeasure(FPDF_ANNOTATION annot);

// ── 2. Viewports on a page (the /VP array)
// ──────────────────────────────────────────────────

// Experimental EmbedPDF Extension API.
// Number of entries in the page's /VP array, in drawing order. Malformed
// entries retain their index; GetViewport returns NULL for them.
FPDF_EXPORT unsigned long FPDF_CALLCONV EPDFPage_CountViewports(FPDF_PAGE page);

// Experimental EmbedPDF Extension API.
// The viewport at |index|, or NULL when out of range. Valid while |page| is
// open.
FPDF_EXPORT EPDF_VIEWPORT FPDF_CALLCONV
EPDFPage_GetViewport(FPDF_PAGE page, unsigned long index);

// Experimental EmbedPDF Extension API.
// Append a viewport with /Type /Viewport and the given /BBox (normalised) and
// return it. Appending is deliberate: the last viewport containing a point wins
// (§12.9.1).
FPDF_EXPORT EPDF_VIEWPORT FPDF_CALLCONV
EPDFPage_AddViewport(FPDF_PAGE page, const FS_RECTF* bbox);

// Experimental EmbedPDF Extension API.
// Remove the viewport at |index|; removes /VP itself when the array becomes
// empty. Every EPDF_VIEWPORT (and measure/format handle under it) obtained from
// this page before the call becomes stale — a handle for index 0 does NOT start
// resolving to the former index 1.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_RemoveViewport(FPDF_PAGE page, unsigned long index);

// Experimental EmbedPDF Extension API.
// Index of the viewport whose /BBox contains |point| (default user space), last
// wins; -1 if none.
FPDF_EXPORT int FPDF_CALLCONV EPDFPage_FindViewport(FPDF_PAGE page,
                                                    const FS_POINTF* point);

// Experimental EmbedPDF Extension API.
// /BBox, /Name and /Measure of a viewport. SetName(NULL) removes /Name. GetName
// returns the byte length of the UTF-16LE text including the NUL, 0 when absent
// (two-call convention).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFViewport_GetBBox(EPDF_VIEWPORT viewport,
                                                         FS_RECTF* bbox);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFViewport_SetBBox(EPDF_VIEWPORT viewport,
                                                         const FS_RECTF* bbox);
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFViewport_GetName(EPDF_VIEWPORT viewport,
                     FPDF_WCHAR* buffer,
                     unsigned long buflen);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFViewport_SetName(EPDF_VIEWPORT viewport,
                                                         FPDF_WIDESTRING name);
FPDF_EXPORT EPDF_MEASURE FPDF_CALLCONV
EPDFViewport_GetMeasure(EPDF_VIEWPORT viewport);  // NULL if none
FPDF_EXPORT EPDF_MEASURE FPDF_CALLCONV
EPDFViewport_AddMeasure(EPDF_VIEWPORT viewport);  // as EPDFAnnot_AddMeasure
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFViewport_RemoveMeasure(EPDF_VIEWPORT viewport);

// ── 3. The measure dictionary (host-agnostic)
// ───────────────────────────────────────────────

// Experimental EmbedPDF Extension API.
// /Subtype; an absent /Subtype reads as RL (ISO default). Every setter below
// returns false on a non-RL measure.
FPDF_EXPORT EPDF_MEASURE_SUBTYPE FPDF_CALLCONV
EPDFMeasure_GetSubtype(EPDF_MEASURE measure);

// Experimental EmbedPDF Extension API.
// /R, the scale-ratio text ("1 in = 1 ft"). Two-call convention; 0 when absent.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFMeasure_GetRatio(EPDF_MEASURE measure,
                     FPDF_WCHAR* buffer,
                     unsigned long buflen);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFMeasure_SetRatio(EPDF_MEASURE measure,
                                                         FPDF_WIDESTRING ratio);

// Experimental EmbedPDF Extension API.
// /O origin and /CYX. Getters return false when the entry is absent (the caller
// applies the ISO default); setters with NULL remove the entry.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFMeasure_GetOrigin(EPDF_MEASURE measure,
                                                          FS_POINTF* origin);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFMeasure_SetOrigin(EPDF_MEASURE measure, const FS_POINTF* origin);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFMeasure_GetCYX(EPDF_MEASURE measure,
                                                       float* cyx);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFMeasure_SetCYX(EPDF_MEASURE measure,
                                                       const float* cyx);

// Experimental EmbedPDF Extension API.
// The number-format array for |axis|: count, element access, append, remove.
// AddFormat writes the two required entries (/Type /NumberFormat /U /C) so an
// element is never half-built; the returned handle addresses the new last
// element. Appending keeps existing handles valid; RemoveFormats makes every
// handle into that array stale.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFMeasure_CountFormats(EPDF_MEASURE measure, EPDF_MEASURE_AXIS axis);
FPDF_EXPORT EPDF_NUMBERFORMAT FPDF_CALLCONV
EPDFMeasure_GetFormat(EPDF_MEASURE measure,
                      EPDF_MEASURE_AXIS axis,
                      unsigned long index);
FPDF_EXPORT EPDF_NUMBERFORMAT FPDF_CALLCONV
EPDFMeasure_AddFormat(EPDF_MEASURE measure,
                      EPDF_MEASURE_AXIS axis,
                      FPDF_WIDESTRING unit,
                      float conversion);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFMeasure_RemoveFormats(EPDF_MEASURE measure, EPDF_MEASURE_AXIS axis);

// ── 4. One number format
// ────────────────────────────────────────────────────────────────────

// Experimental EmbedPDF Extension API.
// /U and /C — required entries. GetUnit uses the two-call convention.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFNumberFormat_GetUnit(EPDF_NUMBERFORMAT format,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetUnit(EPDF_NUMBERFORMAT format, FPDF_WIDESTRING unit);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_GetConversion(EPDF_NUMBERFORMAT format, float* conversion);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetConversion(EPDF_NUMBERFORMAT format, float conversion);

// Experimental EmbedPDF Extension API.
// /F, /D, /FD, /O — optional. Getters return false when absent (ISO defaults
// are the caller's: decimal, 100 or 16, false, suffix); SetPrecision rejects
// values < 1.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_GetFraction(EPDF_NUMBERFORMAT format,
                             EPDF_MEASURE_FRACTION* fraction);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetFraction(EPDF_NUMBERFORMAT format,
                             EPDF_MEASURE_FRACTION fraction);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_GetPrecision(EPDF_NUMBERFORMAT format, int* precision);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetPrecision(EPDF_NUMBERFORMAT format, int precision);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_GetFixedDenominator(EPDF_NUMBERFORMAT format,
                                     FPDF_BOOL* fixed);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetFixedDenominator(EPDF_NUMBERFORMAT format, FPDF_BOOL fixed);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_GetLabelPosition(EPDF_NUMBERFORMAT format,
                                  EPDF_MEASURE_LABEL_POSITION* position);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetLabelPosition(EPDF_NUMBERFORMAT format,
                                  EPDF_MEASURE_LABEL_POSITION position);

// Experimental EmbedPDF Extension API.
// /RT /RD /PS /SS. GetText returns 0 when the key is absent and 2 (the NUL) for
// an explicitly empty string — the two mean different things in ISO 32000-2
// Table 268 ("no text" vs "default"). SetText(NULL) removes the key;
// SetText("") writes an empty string.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFNumberFormat_GetText(EPDF_NUMBERFORMAT format,
                         EPDF_MEASURE_TEXT kind,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetText(EPDF_NUMBERFORMAT format,
                         EPDF_MEASURE_TEXT kind,
                         FPDF_WIDESTRING text);

// ── 5. Dimension line entries (ISO 32000-2 Table 178)
// ───────────────────────────────────────

// Experimental EmbedPDF Extension API.
// /LL, /LLE, /LLO on a Line annotation. |length| may be negative (leaders on
// the other side); |extension| and |offset| must be >= 0. All three zero
// removes the entries. Returns false for any other subtype. The getter reads 0
// for absent entries.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetLineLeader(FPDF_ANNOTATION annot,
                        float length,
                        float extension,
                        float offset);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetLineLeader(FPDF_ANNOTATION annot,
                        float* length,
                        float* extension,
                        float* offset);

// Experimental EmbedPDF Extension API.
// /Cap, /CP and /CO on a Line annotation only. Writes the complete caption
// state, including placement when disabled. Pass the saved position/offset
// when toggling visibility; |offset| NULL means [0 0] (the entry is removed).
// The getter applies the ISO defaults (false, Inline, [0 0]).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetLineCaption(FPDF_ANNOTATION annot,
                         FPDF_BOOL enabled,
                         EPDF_CAPTION_POSITION position,
                         const EPDF_CAPTION_OFFSET* offset);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetLineCaption(FPDF_ANNOTATION annot,
                         FPDF_BOOL* enabled,
                         EPDF_CAPTION_POSITION* position,
                         EPDF_CAPTION_OFFSET* offset);

// Experimental EmbedPDF Extension API.
// EmbedPDF captions on Polygon/PolyLine only, persisted as typed keys under
// /EMBD_Metadata: /MeasurementCaption (boolean), /MeasurementCaptionCenter
// ([x y], optional). Center is the label center in default PDF user space,
// y-up, with the page origin preserved. NULL clears the center (auto layout).
// No standard /Cap, /CP or /CO entries are written on these subtypes.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetShapeCaption(FPDF_ANNOTATION annot,
                          FPDF_BOOL enabled,
                          const FS_POINTF* center);
// |has_center| distinguishes automatic placement from an explicit (0, 0).
// Output arguments are required. The setter writes the complete caption state;
// pass the saved center when disabling to retain it, NULL to reset placement.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetShapeCaption(FPDF_ANNOTATION annot,
                          FPDF_BOOL* enabled,
                          FPDF_BOOL* has_center,
                          FS_POINTF* center);

#ifdef __cplusplus
}
#endif
#endif  // PUBLIC_EPDF_MEASURE_H_
