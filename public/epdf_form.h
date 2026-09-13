// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_FORM_H_
#define PUBLIC_EPDF_FORM_H_

#include <stdint.h>

// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"

#include "epdf_action.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Experimental EmbedPDF Extension API.
//
// Session-free AcroForm model API.
//
// EPDFForm_LoadModel() builds an immutable, detached snapshot of the
// document's interactive form: the /AcroForm field tree, reconciled with a
// sweep over every page's /Annots array so that widget annotations that were
// never linked into /AcroForm /Fields (a common producer bug) still appear
// as fields. The sweep walks page-tree dictionaries only; it never loads
// pages and never parses content streams.
//
// The snapshot is a pure read: it NEVER mutates the document, so it is safe
// to build over a frozen shared base document or a layer document without
// promoting a single object.
//
// All strings are copied into the snapshot at build time. The model stays
// valid after the document is closed, and is invalidated (in the sense of
// becoming stale, not dangling) by any document mutation - callers should
// rebuild after a mutation. Free with EPDFForm_CloseModel().
typedef struct epdf_form_model_t__* EPDF_FORM_MODEL;

// Document form kind, as declared by the document catalog.
// Note that a document with no /AcroForm dictionary can still yield
// recovered fields from the page sweep; kind reports what the catalog
// declares, not whether fields exist.
#define EPDF_FORMKIND_NONE 0
#define EPDF_FORMKIND_ACROFORM 1
// /AcroForm has an /XFA entry. Fields describe the AcroForm shell only.
#define EPDF_FORMKIND_XFA 2

// Field families. Text-family subtleties (password, file-select, rich text,
// multiline, comb) are expressed through the /Ff flags, not extra families.
#define EPDF_FORMFIELD_FAMILY_UNKNOWN 0
#define EPDF_FORMFIELD_FAMILY_PUSHBUTTON 1
#define EPDF_FORMFIELD_FAMILY_CHECKBOX 2
#define EPDF_FORMFIELD_FAMILY_RADIO 3
#define EPDF_FORMFIELD_FAMILY_TEXT 4
#define EPDF_FORMFIELD_FAMILY_COMBOBOX 5
#define EPDF_FORMFIELD_FAMILY_LISTBOX 6
#define EPDF_FORMFIELD_FAMILY_SIGNATURE 7

// Field provenance.
// kAcroForm: reachable from the /AcroForm /Fields tree.
// kRecovered: only reachable through a page's /Annots array; the document
// needs repair for other processors to see this field.
#define EPDF_FORMFIELD_ORIGIN_ACROFORM 0
#define EPDF_FORMFIELD_ORIGIN_RECOVERED 1

// Experimental EmbedPDF Extension API.
// Build a form model snapshot for |document|.
//
// Returns a model handle, or NULL if |document| is NULL or the build failed.
// Documents without any form yield a valid empty model with kind
// EPDF_FORMKIND_NONE.
FPDF_EXPORT EPDF_FORM_MODEL FPDF_CALLCONV
EPDFForm_LoadModel(FPDF_DOCUMENT document);

// Experimental EmbedPDF Extension API.
// Release a model returned by EPDFForm_LoadModel().
FPDF_EXPORT void FPDF_CALLCONV EPDFForm_CloseModel(EPDF_FORM_MODEL model);

// Experimental EmbedPDF Extension API.
// Return the document's declared form kind (EPDF_FORMKIND_*).
FPDF_EXPORT int FPDF_CALLCONV EPDFForm_GetFormKind(EPDF_FORM_MODEL model);

// Experimental EmbedPDF Extension API.
// Return whether the /AcroForm dictionary sets /NeedAppearances.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_GetNeedAppearances(EPDF_FORM_MODEL model);

// Experimental EmbedPDF Extension API.
// Return the number of terminal fields in the model.
FPDF_EXPORT int FPDF_CALLCONV EPDFForm_CountFields(EPDF_FORM_MODEL model);

// Field additional-action events. EPDFForm_GetFieldActionModel() reads the
// effective field /AA through the field hierarchy. These are distinct from
// annotation/widget /AA events even when a field and widget share one merged
// PDF dictionary.
#define EPDF_FORM_ACTION_KEYSTROKE 0
#define EPDF_FORM_ACTION_FORMAT 1
#define EPDF_FORM_ACTION_VALIDATE 2
#define EPDF_FORM_ACTION_CALCULATE 3

// Return a caller-owned detached action model for one effective field action,
// or NULL when absent/malformed. Close with EPDFAction_CloseModel().
FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFForm_GetFieldActionModel(EPDF_FORM_MODEL model, int field_index, int event);

// Return the raw number of entries in /AcroForm /CO. Each entry resolves to a
// field index in this same snapshot, or -1 when malformed/unresolved. Keeping
// malformed slots preserves the declared calculation order.
FPDF_EXPORT int FPDF_CALLCONV
EPDFForm_CountCalculationOrder(EPDF_FORM_MODEL model);
FPDF_EXPORT int FPDF_CALLCONV
EPDFForm_GetCalculationOrderFieldIndex(EPDF_FORM_MODEL model, int order_index);

// Experimental EmbedPDF Extension API.
// Return the indirect object number of the field dictionary, or 0 when the
// field dictionary is a direct object (spec-violating; identity is weak).
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFForm_GetFieldObjNum(EPDF_FORM_MODEL model, int field_index);

// Experimental EmbedPDF Extension API.
// Return the field family (EPDF_FORMFIELD_FAMILY_*), or UNKNOWN when
// |field_index| is out of range.
FPDF_EXPORT int FPDF_CALLCONV EPDFForm_GetFieldFamily(EPDF_FORM_MODEL model,
                                                      int field_index);

// Experimental EmbedPDF Extension API.
// Return the effective /Ff flags (inheritance resolved), or 0 on error.
FPDF_EXPORT uint32_t FPDF_CALLCONV EPDFForm_GetFieldFlags(EPDF_FORM_MODEL model,
                                                          int field_index);

// Experimental EmbedPDF Extension API.
// Return the field provenance (EPDF_FORMFIELD_ORIGIN_*), or -1 on error.
FPDF_EXPORT int FPDF_CALLCONV EPDFForm_GetFieldOrigin(EPDF_FORM_MODEL model,
                                                      int field_index);

// Experimental EmbedPDF Extension API.
// Copy the fully qualified field name ("parent.child") into |buffer| as
// UTF-16LE, including the trailing NUL. Returns the byte length of the
// string, or 0 on error. |buffer| may be NULL to query the length.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldName(EPDF_FORM_MODEL model,
                      int field_index,
                      FPDF_WCHAR* buffer,
                      unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Copy the field's alternate name (/TU, the tooltip) into |buffer| as
// UTF-16LE. Same conventions as EPDFForm_GetFieldName().
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldAlternateName(EPDF_FORM_MODEL model,
                               int field_index,
                               FPDF_WCHAR* buffer,
                               unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Copy the field's mapping name (/TM, the export name) into |buffer| as
// UTF-16LE. Same conventions as EPDFForm_GetFieldName().
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldMappingName(EPDF_FORM_MODEL model,
                             int field_index,
                             FPDF_WCHAR* buffer,
                             unsigned long buflen);

// String-oriented PDF field value shapes. Text strings and button name
// objects are exposed as SCALAR. A multi-select choice array is ARRAY,
// including an empty array. NONE means the inherited entry is absent or
// explicitly null. UNSUPPORTED means an entry exists with another shape
// (for example a signature dictionary or a malformed choice array).
#define EPDF_FORM_VALUE_NONE 0
#define EPDF_FORM_VALUE_SCALAR 1
#define EPDF_FORM_VALUE_ARRAY 2
#define EPDF_FORM_VALUE_UNSUPPORTED 3

// Experimental EmbedPDF Extension API.
// Return the shape of the field's raw, inheritance-resolved /V entry.
// Unlike the former scalar getter, this does not fall back to /DV and does
// not derive a button value from widget /AS. Widget state remains available
// through EPDFForm_IsFieldWidgetChecked() and the widget state/value getters.
FPDF_EXPORT int FPDF_CALLCONV EPDFForm_GetFieldValueKind(EPDF_FORM_MODEL model,
                                                         int field_index);

// Return the number of string/name values in /V. SCALAR has one value;
// ARRAY has its exact element count; NONE and UNSUPPORTED have zero.
FPDF_EXPORT int FPDF_CALLCONV EPDFForm_CountFieldValues(EPDF_FORM_MODEL model,
                                                        int field_index);

// Copy /V value |value_index| into |buffer| as UTF-16LE. Same buffer
// conventions as EPDFForm_GetFieldName(). Returns 0 when out of range or
// when the value shape is NONE/UNSUPPORTED.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldValueAt(EPDF_FORM_MODEL model,
                         int field_index,
                         int value_index,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen);

// Equivalent typed accessors for the raw, inheritance-resolved /DV entry.
FPDF_EXPORT int FPDF_CALLCONV
EPDFForm_GetFieldDefaultValueKind(EPDF_FORM_MODEL model, int field_index);
FPDF_EXPORT int FPDF_CALLCONV
EPDFForm_CountFieldDefaultValues(EPDF_FORM_MODEL model, int field_index);
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldDefaultValueAt(EPDF_FORM_MODEL model,
                                int field_index,
                                int value_index,
                                FPDF_WCHAR* buffer,
                                unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Return /MaxLen for text fields, or 0 when absent or not applicable.
FPDF_EXPORT int FPDF_CALLCONV EPDFForm_GetFieldMaxLen(EPDF_FORM_MODEL model,
                                                      int field_index);

// Experimental EmbedPDF Extension API.
// Return the number of /Opt options for choice fields, 0 otherwise.
FPDF_EXPORT int FPDF_CALLCONV EPDFForm_CountFieldOptions(EPDF_FORM_MODEL model,
                                                         int field_index);

// Experimental EmbedPDF Extension API.
// Copy a choice option's display label into |buffer| as UTF-16LE.
// Same conventions as EPDFForm_GetFieldName().
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldOptionLabel(EPDF_FORM_MODEL model,
                             int field_index,
                             int option_index,
                             FPDF_WCHAR* buffer,
                             unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Copy a choice option's export value into |buffer| as UTF-16LE.
// Same conventions as EPDFForm_GetFieldName().
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldOptionValue(EPDF_FORM_MODEL model,
                             int field_index,
                             int option_index,
                             FPDF_WCHAR* buffer,
                             unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Return whether a choice option is currently selected.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_IsFieldOptionSelected(EPDF_FORM_MODEL model,
                               int field_index,
                               int option_index);

// Experimental EmbedPDF Extension API.
// Return the number of widget annotations bound to the field. A merged
// field/widget dictionary counts as one widget whose object number equals
// the field's. Zero widgets means the field is unplaced.
FPDF_EXPORT int FPDF_CALLCONV EPDFForm_CountFieldWidgets(EPDF_FORM_MODEL model,
                                                         int field_index);

// Experimental EmbedPDF Extension API.
// Return the widget annotation's indirect object number, or 0 for direct
// (spec-violating) widget dictionaries.
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFForm_GetFieldWidgetObjNum(EPDF_FORM_MODEL model,
                              int field_index,
                              int widget_index);

// Experimental EmbedPDF Extension API.
// Return the object number of the page whose /Annots array references the
// widget (resolved during the sweep, falling back to the widget's /P
// entry), or 0 when the widget is not reachable from any page.
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFForm_GetFieldWidgetPageObjNum(EPDF_FORM_MODEL model,
                                  int field_index,
                                  int widget_index);

// Experimental EmbedPDF Extension API.
// Copy the widget's on-state name (the non-"Off" key of its /AP /N
// dictionary) into |buffer| as raw PDF name bytes, including the trailing
// NUL. Only meaningful for checkbox and radio widgets; empty otherwise.
// Returns the byte length of the string, or 0 on error. |buffer| may be
// NULL to query the length. The returned bytes are an opaque token for use
// with future toggle APIs.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldWidgetOnState(EPDF_FORM_MODEL model,
                               int field_index,
                               int widget_index,
                               void* buffer,
                               unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Copy the widget's export value (/Opt entry for its control index when
// present, else the on-state name) into |buffer| as UTF-16LE. Only
// meaningful for checkbox and radio widgets. Same conventions as
// EPDFForm_GetFieldName().
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_GetFieldWidgetExportValue(EPDF_FORM_MODEL model,
                                   int field_index,
                                   int widget_index,
                                   FPDF_WCHAR* buffer,
                                   unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Return whether a checkbox/radio widget is currently checked
// (its /AS equals its on-state).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_IsFieldWidgetChecked(EPDF_FORM_MODEL model,
                              int field_index,
                              int widget_index);

// Experimental EmbedPDF Extension API.
// Return the index of the field whose dictionary has the given indirect
// object number, or -1 when unknown.
FPDF_EXPORT int FPDF_CALLCONV
EPDFForm_GetFieldIndexByObjNum(EPDF_FORM_MODEL model, uint32_t field_objnum);

// ---------------------------------------------------------------------------
// Write transactions.
//
// All write APIs below are stateless, field-local transactions keyed by the
// field dictionary's indirect object number (from EPDFForm_GetFieldObjNum).
// They take the DOCUMENT, not a model: models are immutable snapshots and
// become stale after any successful write - rebuild with EPDFForm_LoadModel.
//
// Transactions validate first and mutate second: on FALSE the document is
// untouched (on a layer document: zero objects promoted). On success, only
// the objects that actually change are written (on a layer document: only
// those promote), which keeps layer deltas minimal.
//
// /Ff ReadOnly (bit 1) is deliberately NOT enforced here: the PDF spec
// forbids USER modification of read-only fields, not programmatic writes
// (calculated fields are read-only yet script-written). Enforcing fill
// policy is the caller's responsibility.
//
// Changed-widget reporting (uniform across all write APIs):
//   changed_widget_objnums - optional caller buffer receiving the object
//                            numbers of widget annotations whose appearance
//                            changed (may span multiple pages). May be NULL.
//   buffer_size            - capacity of |changed_widget_objnums| in
//                            elements.
//   out_changed_count      - optional; receives the TOTAL number of changed
//                            widgets, which may exceed |buffer_size|.
//                            Widgets stored as direct objects (no object
//                            number) are counted but not reported.
// ---------------------------------------------------------------------------

// Experimental EmbedPDF Extension API.
// Set the value of a checkbox or radio field.
//
// |on_state| is the target widget appearance state, exactly as returned by
// EPDFForm_GetFieldWidgetOnState(), and selects WHICH widget of the group
// is checked. NULL clears the group (rejected for radio fields with
// NoToggleToOff). Every sibling widget's /AS is updated (checkboxes and
// in-unison radios check all widgets sharing the target's export value and
// on-state) and the field's /V is set to the export value name, or to the
// control index for fields carrying /Opt, matching Acrobat conventions.
//
// No appearance streams are regenerated: toggle widgets carry one appearance
// per state, so flipping /AS IS the visual change.
//
// Fails when |field_objnum| is not a checkbox/radio terminal field or
// |on_state| matches no widget. Returns TRUE with zero changes when the
// field is already in the requested state.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetToggle(FPDF_DOCUMENT document,
                   uint32_t field_objnum,
                   FPDF_BYTESTRING on_state,
                   uint32_t* changed_widget_objnums,
                   unsigned long buffer_size,
                   unsigned long* out_changed_count);

// Experimental EmbedPDF Extension API.
// Set the value of a text field.
//
// Writes /V, drops any stale rich-text /RV, and regenerates the /AP stream
// of every widget of the field. Fails when |field_objnum| is not a text
// terminal field. When the value exceeds the field's effective /MaxLen, only
// the first /MaxLen characters are written, matching Acrobat assignment
// semantics.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetTextValue(FPDF_DOCUMENT document,
                      uint32_t field_objnum,
                      FPDF_WIDESTRING value,
                      uint32_t* changed_widget_objnums,
                      unsigned long buffer_size,
                      unsigned long* out_changed_count);

// Experimental EmbedPDF Extension API.
// Set the selection of a combo box or list box field.
//
// |values| holds |value_count| option export values. Zero values clears the
// effective selection, using a local empty value when needed to shadow an
// inherited /V. Multiple values require a multi-select list box. For combo
// boxes with the Edit flag a single non-option value is accepted as free
// text; otherwise every value must match an option's export value.
//
// Writes /V (string, or array for multiple values ordered by option index),
// keeps /I in sync (sorted ascending; removed when free text is set), and
// regenerates every widget's /AP stream.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetChoiceValues(FPDF_DOCUMENT document,
                         uint32_t field_objnum,
                         const FPDF_WIDESTRING* values,
                         unsigned long value_count,
                         uint32_t* changed_widget_objnums,
                         unsigned long buffer_size,
                         unsigned long* out_changed_count);

// Experimental EmbedPDF Extension API.
// Reset a field to its default value.
//
// Restores /V from the effective /DV (clearing the effective value when no
// default exists), clears stale /RV and /I, updates toggle widget /AS states,
// and regenerates /AP streams for text and choice widgets. Fails for push
// buttons and signature fields.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_ResetField(FPDF_DOCUMENT document,
                    uint32_t field_objnum,
                    uint32_t* changed_widget_objnums,
                    unsigned long buffer_size,
                    unsigned long* out_changed_count);

// Acrobat-compatible Field.display values. Only the Invisible, Hidden, Print,
// and NoView annotation flag bits are changed; all unrelated flags survive.
#define EPDF_FORM_DISPLAY_VISIBLE 0
#define EPDF_FORM_DISPLAY_HIDDEN 1
#define EPDF_FORM_DISPLAY_NO_PRINT 2
#define EPDF_FORM_DISPLAY_NO_VIEW 3

// Set Field.display for every widget of a terminal field. The write follows
// the same validate-then-promote transaction path as value writes.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldDisplay(FPDF_DOCUMENT document,
                         uint32_t field_objnum,
                         int display,
                         uint32_t* changed_widget_objnums,
                         unsigned long buffer_size,
                         unsigned long* out_changed_count);

// Regenerate every text/combo widget /AP using |appearance_text| without
// changing the field's semantic /V. This is the native sink for /AA /F
// formatting results. List boxes, buttons, and signatures are rejected.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldAppearanceText(FPDF_DOCUMENT document,
                                uint32_t field_objnum,
                                FPDF_WIDESTRING appearance_text,
                                uint32_t* changed_widget_objnums,
                                unsigned long buffer_size,
                                unsigned long* out_changed_count);

// ---------------------------------------------------------------------------
// Form data interchange (FDF / XFDF).
//
// Exports read through the same reconciled view as EPDFForm_LoadModel, so
// recovered fields (origin kRecovered) are included, and on layer documents
// promoted values win. Imports replay each entry through the typed write
// transactions above, so validation, appearance regeneration, and minimal
// layer promotion apply per field; one bad entry never poisons the rest.
// ---------------------------------------------------------------------------

// Omit required fields whose value is empty (Acrobat's form-submission
// behavior). Off by default: interchange exports are faithful.
#define EPDF_FORM_EXPORT_SKIP_EMPTY_REQUIRED 0x1

// Experimental EmbedPDF Extension API.
// Serialize the document's form data as FDF.
//
//   pdf_path     - optional /F filespec recorded in the FDF; NULL to omit.
//   export_flags - EPDF_FORM_EXPORT_* bits.
//
// Returns the byte length of the FDF payload, or 0 on error. When |buffer|
// is non-NULL and |buflen| is large enough, the payload is copied into it.
// Call with a NULL buffer first to query the size.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_ExportFDF(FPDF_DOCUMENT document,
                   FPDF_WIDESTRING pdf_path,
                   uint32_t export_flags,
                   void* buffer,
                   unsigned long buflen);

// Experimental EmbedPDF Extension API.
// Serialize the document's form data as XFDF (UTF-8 XML, form data only -
// no annotations). Field names nest per fully-qualified-name component.
// Same conventions as EPDFForm_ExportFDF.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFForm_ExportXFDF(FPDF_DOCUMENT document,
                    FPDF_WIDESTRING pdf_path,
                    uint32_t export_flags,
                    void* buffer,
                    unsigned long buflen);

// Per-import accounting. A field entry is "applied" when its value was
// written (including no-op writes of an unchanged value) and "skipped" when
// the name is unknown, the field family cannot take the value, or the value
// failed validation (unknown toggle state, MaxLen, non-option choice, ...).
typedef struct {
  uint32_t fields_total;
  uint32_t fields_applied;
  uint32_t fields_skipped;
  uint32_t widgets_changed;
} EPDF_FORM_IMPORT_RESULT;

// Experimental EmbedPDF Extension API.
// Apply form data from an FDF payload to the document.
//
// Accepts both flat entries with dotted /T names and hierarchical /Kids
// trees. Returns TRUE when the FDF parsed, regardless of per-field skips
// (see |out_result|); FALSE when the payload is not FDF. On a layer
// document only the fields that actually change promote.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_ImportFDF(FPDF_DOCUMENT document,
                   const void* data,
                   unsigned long size,
                   EPDF_FORM_IMPORT_RESULT* out_result);

// Experimental EmbedPDF Extension API.
// Apply form data from an XFDF payload to the document. Accepts nested
// <field> elements and dotted name attributes; multiple <value> elements
// select multiple options of a multi-select list box. Same conventions as
// EPDFForm_ImportFDF.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_ImportXFDF(FPDF_DOCUMENT document,
                    const void* data,
                    unsigned long size,
                    EPDF_FORM_IMPORT_RESULT* out_result);

// ---------------------------------------------------------------------------
// Repair ("form doctor").
//
// EPDFForm_LoadModel() reconciles broken documents in memory on every load;
// EPDFForm_Repair() makes those fixes durable in the document so any other
// PDF processor sees the same form. Validate-then-apply: the document is
// only touched when there is something to fix, so on a layer document a
// no-op repair promotes nothing and a real repair promotes only the
// structural containers it edits. Idempotent: a second call reports zero
// fixes.
// ---------------------------------------------------------------------------

// Also regenerate widget appearance streams: widgets with no /AP get one,
// and when the /AcroForm sets /NeedAppearances every widget is re-baked and
// the flag is cleared, making rendering deterministic across viewers.
#define EPDF_FORM_REPAIR_BAKE_APPEARANCES 0x1

typedef struct {
  // 1 when a missing /AcroForm dictionary was created (with /DR + /DA).
  uint32_t acroform_created;
  // Recovered field roots appended to /AcroForm /Fields.
  uint32_t fields_linked;
  // Stray widgets appended to their parent field's /Kids (only when the
  // widget's /Parent already references that field).
  uint32_t widgets_linked;
  // Recovered fields stored as direct objects: they cannot be referenced
  // from /Fields and stay reconciled-in-memory only.
  uint32_t fields_unrepairable;
  // Widgets whose appearance stream was (re)generated.
  uint32_t appearances_baked;
  // 1 when /NeedAppearances was cleared after re-baking.
  uint32_t need_appearances_cleared;
} EPDF_FORM_REPAIR_REPORT;

// Experimental EmbedPDF Extension API.
// Repair the document's form structure. |repair_flags| is a bitset of
// EPDF_FORM_REPAIR_* values. Returns TRUE when the repair pass ran (even
// with zero fixes); FALSE on error. |out_report| may be NULL.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_Repair(FPDF_DOCUMENT document,
                uint32_t repair_flags,
                EPDF_FORM_REPAIR_REPORT* out_report);

// ---------------------------------------------------------------------------
// Authoring: field lifecycle and adoption.
//
// Widgets are born, styled, moved, and deleted through the ANNOTATION APIs
// (FPDFPage_CreateAnnot with the Widget subtype, EPDFAnnot_SetMKColor,
// EPDFAnnot_SetBorderStyle, EPDFAnnot_SetDefaultAppearance, ...). The forms
// API below only does the field-tree side: create a logical (unplaced)
// field, ADOPT an existing widget annotation as one of its views, detach
// it again, and configure field-plane properties. An unattached widget is
// an ordinary, inert annotation - adoption is what turns it into a form
// control (and what bakes its family-correct appearance stream).
//
// All operations are validate-then-apply: a FALSE/0 return leaves the
// document untouched (on layer documents: zero objects promoted).
// ---------------------------------------------------------------------------

// Experimental EmbedPDF Extension API.
// Create a logical form field with no widgets ("unplaced").
//
//   family    - EPDF_FORMFIELD_FAMILY_TEXT / CHECKBOX / RADIO / COMBOBOX /
//               LISTBOX / SIGNATURE (created unsigned; /AcroForm /SigFlags
//               gains SignaturesExist). Push buttons and unknown are not
//               authorable.
//   full_name - dotted fully qualified name ("billing.name"). Missing
//               non-terminal ancestors are created; a sibling name
//               collision at any level fails.
//
// Bootstraps /AcroForm (with /DR and /DA) when the document has none.
// Returns the new field dictionary's object number, or 0 on failure.
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFForm_CreateField(FPDF_DOCUMENT document,
                     int family,
                     FPDF_WIDESTRING full_name);

// Experimental EmbedPDF Extension API.
// Adopt an existing widget annotation as a view of |field_objnum|.
//
// The widget must be an unattached widget annotation (no /Parent, not a
// merged field). For checkbox/radio fields |on_state| names the widget's
// checked appearance state and must be non-empty; for other families pass
// NULL. Adoption wires /Parent + /Kids, seeds toggle /AP states and /AS,
// and bakes the family-correct appearance stream. Adopting into a legacy
// MERGED field first splits it (the field keeps its object number; the
// previously merged widget becomes a new kid annotation - widget identity
// changes, field identity never does).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_AttachWidget(FPDF_DOCUMENT document,
                      uint32_t field_objnum,
                      uint32_t widget_objnum,
                      FPDF_BYTESTRING on_state);

// Experimental EmbedPDF Extension API.
// Detach a widget from its field. The widget keeps its page placement and
// last appearance but becomes an ordinary inert annotation (deletable via
// the annotation APIs). The field survives, "unplaced" when this was its
// last widget.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_DetachWidget(FPDF_DOCUMENT document,
                      uint32_t field_objnum,
                      uint32_t widget_objnum);

// Experimental EmbedPDF Extension API.
// Delete a terminal field: detaches every widget (reported through the
// caller buffer so they can be deleted as annotations), removes the field
// from the tree, and prunes non-terminal ancestors left empty. Fails for
// non-terminal fields (nodes with child FIELDS).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_DeleteField(FPDF_DOCUMENT document,
                     uint32_t field_objnum,
                     uint32_t* out_detached_widgets,
                     unsigned long buffer_size,
                     unsigned long* out_detached_count);

// ---------------------------------------------------------------------------
// Authoring: field-plane property setters. Each is an independent
// validate-then-apply transaction; the TypeScript layer composes them into
// one updateField() call.
// ---------------------------------------------------------------------------

// Experimental EmbedPDF Extension API.
// Rename the field's own /T segment (NOT the dotted path - reparenting is
// not supported). Fails when a sibling under the same parent already
// carries that name, or when |partial_name| is empty or contains '.'.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldName(FPDF_DOCUMENT document,
                      uint32_t field_objnum,
                      FPDF_WIDESTRING partial_name);

// Experimental EmbedPDF Extension API.
// Masked /Ff update: bits in |set_bits| are set, bits in |clear_bits| are
// cleared. Family-DEFINING bits (Radio, Pushbutton, Combo) are immutable -
// touching them fails.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldFlags(FPDF_DOCUMENT document,
                       uint32_t field_objnum,
                       uint32_t set_bits,
                       uint32_t clear_bits);

// Experimental EmbedPDF Extension API.
// Set /MaxLen on a text field. 0 clears the limit. Fails when the current
// value is already longer than the new limit.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldMaxLen(FPDF_DOCUMENT document,
                        uint32_t field_objnum,
                        int max_len);

// Experimental EmbedPDF Extension API.
// Set /DV on a text or choice field. Text fields require exactly one value.
// Choice fields accept one value, or multiple values for a multi-select list
// box; option validation and ordering match EPDFForm_SetChoiceValues(). A
// single empty string is a real scalar default, not a request to remove /DV.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldDefaultValues(FPDF_DOCUMENT document,
                               uint32_t field_objnum,
                               const FPDF_WIDESTRING* values,
                               unsigned long value_count);

// Set a checkbox/radio /DV from the opaque widget appearance-state token
// returned by EPDFForm_GetFieldWidgetOnState(). "Off" is an explicit default;
// NULL and the empty string are rejected.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldDefaultToggle(FPDF_DOCUMENT document,
                               uint32_t field_objnum,
                               FPDF_BYTESTRING on_state);

// Remove /DV from the addressed field dictionary. If an ancestor provides an
// inherited /DV, that inherited default becomes effective; this API removes a
// local override rather than mutating a shared ancestor.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_RemoveFieldDefaultValue(FPDF_DOCUMENT document, uint32_t field_objnum);

// Experimental EmbedPDF Extension API.
// Set /TU (the accessible tooltip). An empty string clears it.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldAlternateName(FPDF_DOCUMENT document,
                               uint32_t field_objnum,
                               FPDF_WIDESTRING value);

// Experimental EmbedPDF Extension API.
// Set /TM (the export mapping name). An empty string clears it.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldMappingName(FPDF_DOCUMENT document,
                             uint32_t field_objnum,
                             FPDF_WIDESTRING value);

// Experimental EmbedPDF Extension API.
// Replace a choice field's effective /Opt with |count| options. Entries where
// label equals export are written as plain strings, otherwise as [export label]
// pairs. The current selection is re-synced: selected exports that vanish
// are dropped, /V and /I are rewritten consistently, and widget appearance
// streams are regenerated. /DV is filtered through the same new option set.
// |count| of 0 writes an empty local /Opt array (shadowing inherited options);
// an edit combo's current/default free text survives and other selections
// clear.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_SetFieldOptions(FPDF_DOCUMENT document,
                         uint32_t field_objnum,
                         const FPDF_WIDESTRING* labels,
                         const FPDF_WIDESTRING* exports,
                         unsigned long count);

// Experimental EmbedPDF Extension API.
// Return the index of the field owning the widget annotation with the given
// indirect object number, or -1 when the object is not a known widget. This
// is the join key for decorating widget annotations in page annotation
// listings with their logical field.
FPDF_EXPORT int FPDF_CALLCONV
EPDFForm_GetFieldIndexForWidget(EPDF_FORM_MODEL model, uint32_t widget_objnum);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_FORM_H_
