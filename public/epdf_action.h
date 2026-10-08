// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_ACTION_H_
#define PUBLIC_EPDF_ACTION_H_

#include <stdint.h>

// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Experimental EmbedPDF Extension API.
//
// Detached PDF action model. A model contains one root action and its
// normalized /Next descendants. Type, subtype, script, chain, destination,
// URI (with /IsMap), file path, named-action, Hide /T + /H, and ResetForm
// /Fields + /Flags payloads are copied at build time and stay valid after
// the document is closed or mutated. The getters that take an
// FPDF_DOCUMENT still require the originating document to be open: it
// validates destination page identity and is used to resolve a named
// destination when EPDFAction_LoadModel() had no document owner available.
//
// The API extracts action data only. It never executes JavaScript.
typedef struct epdf_action_model_t__* EPDF_ACTION_MODEL;
typedef uint32_t EPDF_ACTION_NODE_ID;

#define EPDF_ACTION_NODE_INVALID UINT32_MAX

// Normalized values of an action dictionary's /S name. The raw /S name is
// also available so unknown future action types are preserved.
#define EPDF_ACTION_TYPE_UNKNOWN 0
#define EPDF_ACTION_TYPE_GOTO 1
#define EPDF_ACTION_TYPE_GOTO_REMOTE 2
#define EPDF_ACTION_TYPE_GOTO_EMBEDDED 3
#define EPDF_ACTION_TYPE_LAUNCH 4
#define EPDF_ACTION_TYPE_THREAD 5
#define EPDF_ACTION_TYPE_URI 6
#define EPDF_ACTION_TYPE_SOUND 7
#define EPDF_ACTION_TYPE_MOVIE 8
#define EPDF_ACTION_TYPE_HIDE 9
#define EPDF_ACTION_TYPE_NAMED 10
#define EPDF_ACTION_TYPE_SUBMIT_FORM 11
#define EPDF_ACTION_TYPE_RESET_FORM 12
#define EPDF_ACTION_TYPE_IMPORT_DATA 13
#define EPDF_ACTION_TYPE_JAVASCRIPT 14
#define EPDF_ACTION_TYPE_SET_OCG_STATE 15
#define EPDF_ACTION_TYPE_RENDITION 16
#define EPDF_ACTION_TYPE_TRANSITION 17
#define EPDF_ACTION_TYPE_GOTO_3D_VIEW 18

// Non-fatal normalization warnings. Models marked INCOMPLETE must not be
// executed: a safety bound prevented the complete action sequence from being
// represented. Cyclic back-edges and malformed /Next entries are dropped;
// their other well-formed siblings remain available.
#define EPDF_ACTION_WARNING_CYCLE_DROPPED 0x1
#define EPDF_ACTION_WARNING_MALFORMED_NEXT 0x2
#define EPDF_ACTION_WARNING_INCOMPLETE 0x4

// Release a model returned by any EPDF*ActionModel() function below.
FPDF_EXPORT void FPDF_CALLCONV EPDFAction_CloseModel(EPDF_ACTION_MODEL model);

// Build a detached model from an existing borrowed FPDF_ACTION. This lets
// callers normalize actions returned by APIs such as FPDFBookmark_GetAction().
FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFAction_LoadModel(FPDF_ACTION action);

// Return the root node id, or EPDF_ACTION_NODE_INVALID for an invalid model.
FPDF_EXPORT EPDF_ACTION_NODE_ID FPDF_CALLCONV
EPDFAction_GetRootNode(EPDF_ACTION_MODEL model);

FPDF_EXPORT int FPDF_CALLCONV EPDFAction_GetNodeCount(EPDF_ACTION_MODEL model);

// Return an EPDF_ACTION_TYPE_* value for |node|.
FPDF_EXPORT int FPDF_CALLCONV EPDFAction_GetNodeType(EPDF_ACTION_MODEL model,
                                                     EPDF_ACTION_NODE_ID node);

// Copy the raw PDF /S name as UTF-8, including the trailing NUL. Returns the
// required byte length, or 0 on error. |buffer| may be NULL to query length.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeSubtype(EPDF_ACTION_MODEL model,
                          EPDF_ACTION_NODE_ID node,
                          char* buffer,
                          unsigned long buflen);

// Return whether |node| contains a string or stream /JS entry that belongs to
// either a /JavaScript or /Rendition action. This distinguishes an empty
// script from a missing or malformed /JS entry.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_NodeHasJavaScript(EPDF_ACTION_MODEL model, EPDF_ACTION_NODE_ID node);

// Copy decoded /JS source as UTF-16LE, including the trailing NUL. Returns the
// required byte length, or 0 when absent/malformed. Rendition /JS is exposed
// through this same getter.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeJavaScript(EPDF_ACTION_MODEL model,
                             EPDF_ACTION_NODE_ID node,
                             FPDF_WCHAR* buffer,
                             unsigned long buflen);

// Get the destination of a goto / goto-remote / goto-embedded |node| as an
// explicit FPDF_DEST. Named destinations resolve through |document|'s
// catalog — same normalization as FPDFLink_GetDest. Returns NULL when the
// node carries no destination, has a different type, or |document| is
// invalid. |document| must be the document the model was built from.
FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFAction_GetNodeDest(FPDF_DOCUMENT document,
                       EPDF_ACTION_MODEL model,
                       EPDF_ACTION_NODE_ID node);

// Copy the /URI of a uri-type |node| as a NUL-terminated byte string.
// Returns the required byte length including the NUL, or 0 when the node
// is not a uri action. |buffer| may be NULL to query the length.
// |document| must be the document the model was built from.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeURI(FPDF_DOCUMENT document,
                      EPDF_ACTION_MODEL model,
                      EPDF_ACTION_NODE_ID node,
                      void* buffer,
                      unsigned long buflen);

// Copy the file spec of a goto-remote / goto-embedded / launch |node| as
// UTF-8, including the trailing NUL. Returns the required byte length, or
// 0 for other node types. |buffer| may be NULL to query the length.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeFilePath(EPDF_ACTION_MODEL model,
                           EPDF_ACTION_NODE_ID node,
                           void* buffer,
                           unsigned long buflen);

// Copy the /N name of a named-type |node| (NextPage, PrevPage, ...) as
// UTF-8, including the trailing NUL. Returns the required byte length, or
// 0 for other node types. |buffer| may be NULL to query the length.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeName(EPDF_ACTION_MODEL model,
                       EPDF_ACTION_NODE_ID node,
                       void* buffer,
                       unsigned long buflen);

// Number of Hide /T, ResetForm /Fields, or SubmitForm /Fields entries
// captured for |node|. Returns 0 for other node types, and -1 when an entry
// could not be represented: a partial target list must never execute, so
// consumers treat -1 as an unreadable payload and degrade the node.
FPDF_EXPORT int FPDF_CALLCONV
EPDFAction_GetNodeTargetCount(EPDF_ACTION_MODEL model,
                              EPDF_ACTION_NODE_ID node);

// Copy target |index|'s field name as UTF-8, including the trailing NUL.
// Returns the required byte length, or 0 when the entry is an object
// reference rather than a name, the index is out of range, or the node
// carries no target list. |buffer| may be NULL to query the length.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeTargetName(EPDF_ACTION_MODEL model,
                             EPDF_ACTION_NODE_ID node,
                             int index,
                             void* buffer,
                             unsigned long buflen);

// Get target |index|'s indirect object number (an annotation or field
// dictionary reference). Returns false when the entry is a name rather than
// an object reference, the index is out of range, or the node carries no
// target list. |object_number| is only written on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_GetNodeTargetObjectNumber(EPDF_ACTION_MODEL model,
                                     EPDF_ACTION_NODE_ID node,
                                     int index,
                                     unsigned int* object_number);

// Hide /H for a hide-type |node|: true hides (the spec default), false
// shows. Returns false for other node types; |hide| is only written on
// success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_GetNodeHideFlag(EPDF_ACTION_MODEL model,
                           EPDF_ACTION_NODE_ID node,
                           FPDF_BOOL* hide);

// ResetForm state for a reset-form-type |node|: |has_fields| reports whether
// /Fields was PRESENT (absent means "reset every field" and |exclude| is
// meaningless); |exclude| is /Flags bit 0 (set = /Fields lists EXCLUDED
// fields). Returns false for other node types; out-params are only written
// on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_GetNodeResetForm(EPDF_ACTION_MODEL model,
                            EPDF_ACTION_NODE_ID node,
                            FPDF_BOOL* has_fields,
                            FPDF_BOOL* exclude);

// SubmitForm state for a submit-form-type |node|. Returns true only when
// the REQUIRED /F resolved to a URL — a << /FS /URL >> file specification
// (ISO 32000-2 7.11.5, /UF preferred over /F per 7.11.2), or a bare
// string/name /F accepted as a producer-compat extension. Returns false
// otherwise, including for a submit-form node whose payload was withheld —
// consumers degrade such a node instead of executing a half payload.
// |has_fields| reports whether /Fields was PRESENT (same presence-vs-empty
// distinction as ResetForm); |flags| is the raw /Flags word (ISO 32000-2
// Table 240, bits numbered from 1). Out-params are only written on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_GetNodeSubmitForm(EPDF_ACTION_MODEL model,
                             EPDF_ACTION_NODE_ID node,
                             FPDF_BOOL* has_fields,
                             unsigned int* flags);

// Copy the resolved submission URL of a submit-form-type |node| as UTF-8,
// including the trailing NUL. Returns the required byte length, or 0 when
// the node is not a submit-form action or its payload was withheld.
// |buffer| may be NULL to query the length.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeSubmitFormURL(EPDF_ACTION_MODEL model,
                                EPDF_ACTION_NODE_ID node,
                                void* buffer,
                                unsigned long buflen);

// Copy the /CharSet of a submit-form-type |node| (PDF 2.0) as UTF-8,
// including the trailing NUL. Returns 0 when absent or for other node
// types. |buffer| may be NULL to query the length.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeSubmitFormCharSet(EPDF_ACTION_MODEL model,
                                    EPDF_ACTION_NODE_ID node,
                                    void* buffer,
                                    unsigned long buflen);

// URI /IsMap for a uri-type |node| (default false). Returns false for other
// node types; |is_map| is only written on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_GetNodeURIIsMap(EPDF_ACTION_MODEL model,
                           EPDF_ACTION_NODE_ID node,
                           FPDF_BOOL* is_map);

FPDF_EXPORT int FPDF_CALLCONV EPDFAction_GetNextCount(EPDF_ACTION_MODEL model,
                                                      EPDF_ACTION_NODE_ID node);

// Return the normalized child node at |index| in PDF /Next order.
FPDF_EXPORT EPDF_ACTION_NODE_ID FPDF_CALLCONV
EPDFAction_GetNextAt(EPDF_ACTION_MODEL model,
                     EPDF_ACTION_NODE_ID node,
                     int index);

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFAction_GetWarningFlags(EPDF_ACTION_MODEL model);

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_IsComplete(EPDF_ACTION_MODEL model);

// Document-owned actions ----------------------------------------------------

#define EPDF_DOCUMENT_ACTION_WILL_CLOSE 0
#define EPDF_DOCUMENT_ACTION_WILL_SAVE 1
#define EPDF_DOCUMENT_ACTION_DID_SAVE 2
#define EPDF_DOCUMENT_ACTION_WILL_PRINT 3
#define EPDF_DOCUMENT_ACTION_DID_PRINT 4

// Return the action model for /Names /JavaScript entry |index|. Index pairing
// is guaranteed with FPDFDoc_GetJavaScriptAction(document, index): both calls
// resolve the same name-tree entry and therefore preserve boot order.
FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFDoc_GetNamedJavaScriptActionModel(FPDF_DOCUMENT document, int index);

// Return the action form of catalog /OpenAction. Returns NULL when absent,
// malformed, or when /OpenAction is a destination rather than an action
// (read the destination form with EPDFDoc_GetOpenActionDest()).
FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFDoc_GetOpenActionModel(FPDF_DOCUMENT document);

// Return the destination form of catalog /OpenAction as an explicit
// FPDF_DEST — a named value resolves through the catalog, the same
// normalization as FPDFLink_GetDest. Returns NULL when /OpenAction is
// absent, malformed, or an action dictionary. The handle points into the
// live document, like FPDFLink_GetDest, and is only valid while the
// document stays open.
FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDoc_GetOpenActionDest(FPDF_DOCUMENT document);

// Return one catalog /AA action selected by EPDF_DOCUMENT_ACTION_*.
FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFDoc_GetAdditionalActionModel(FPDF_DOCUMENT document, int event);

// Page-owned actions --------------------------------------------------------

#define EPDF_PAGE_ACTION_OPEN 0
#define EPDF_PAGE_ACTION_CLOSE 1

// Read page /AA without loading or rendering the page.
FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFDoc_GetPageActionModel(FPDF_DOCUMENT document,
                           uint32_t page_object_number,
                           int event);

// Annotation-owned actions --------------------------------------------------

#define EPDF_ANNOT_ACTION_ACTIVATE 0
#define EPDF_ANNOT_ACTION_CURSOR_ENTER 1
#define EPDF_ANNOT_ACTION_CURSOR_EXIT 2
#define EPDF_ANNOT_ACTION_MOUSE_DOWN 3
#define EPDF_ANNOT_ACTION_MOUSE_UP 4
#define EPDF_ANNOT_ACTION_FOCUS 5
#define EPDF_ANNOT_ACTION_BLUR 6
#define EPDF_ANNOT_ACTION_PAGE_OPEN 7
#define EPDF_ANNOT_ACTION_PAGE_CLOSE 8
#define EPDF_ANNOT_ACTION_PAGE_VISIBLE 9
#define EPDF_ANNOT_ACTION_PAGE_INVISIBLE 10

// Return annotation /A (ACTIVATE) or an annotation /AA action. Field events
// K/F/V/C are deliberately not accepted here, including for merged
// field/widget dictionaries.
FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFAnnot_GetActionModel(FPDF_ANNOTATION annotation, int event);

// Writing actions -----------------------------------------------------------
//
// Each creator makes a new action dictionary, an indirect object of
// |document|, and returns its handle, or NULL on bad arguments. Attach it
// with EPDFAnnot_SetEventAction(), EPDFForm_SetFieldEventAction() or
// EPDFAction_SetNext(). The fork writes what it is given; which actions a
// caller may write is the caller's decision.

#define EPDF_ACTION_TARGET_NAME 0
#define EPDF_ACTION_TARGET_OBJECT 1

// A field or widget an action names, as the reader's targets: by fully
// qualified field name (EPDF_ACTION_TARGET_NAME, |name| as UTF-16LE), or by
// the object number of a field or annotation dictionary
// (EPDF_ACTION_TARGET_OBJECT, |object_number|).
typedef struct {
  int kind;
  uint32_t object_number;
  FPDF_WIDESTRING name;
} EPDF_ACTION_TARGET;

// /S /JavaScript with |script| as the /JS text string.
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateJavaScript(FPDF_DOCUMENT document, FPDF_WIDESTRING script);

// /S /Hide naming |count| (at least 1) |targets| in /T: the entry itself for
// one, an array for several. /H false is written only when |hide| is false
// (true, hide, is the default).
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateHide(FPDF_DOCUMENT document,
                      const EPDF_ACTION_TARGET* targets,
                      int count,
                      FPDF_BOOL hide);

// /S /ResetForm. |count| -1 writes no /Fields (every field is reset); 0 or
// more writes /Fields with |count| |targets|. |exclude| writes /Flags 1: the
// listed fields are the ones NOT reset.
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateResetForm(FPDF_DOCUMENT document,
                           const EPDF_ACTION_TARGET* targets,
                           int count,
                           FPDF_BOOL exclude);

// /S /SubmitForm to |url|, written as /F << /FS /URL /F (url) >>. /Fields as
// for EPDFAction_CreateResetForm() (|count| -1: none). |flags| is written raw
// as /Flags (ISO 32000-2 Table 240), and left out when 0.
FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateSubmitForm(FPDF_DOCUMENT document,
                            FPDF_WIDESTRING url,
                            const EPDF_ACTION_TARGET* targets,
                            int count,
                            unsigned int flags);

// Sets |action|'s /Next to the |count| actions in |next|: one reference, or
// an array for several; |count| 0 removes /Next. |action| and every next
// must be indirect actions of |document|, and none may be |action| itself.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAction_SetNext(FPDF_DOCUMENT document,
                                                       FPDF_ACTION action,
                                                       const FPDF_ACTION* next,
                                                       int count);

// Sets a widget's action for |event| (EPDF_ANNOT_ACTION_*): ACTIVATE writes
// /A, the others their /AA entry, each as a reference to |action|, an
// indirect action of the widget's document. NULL removes the entry, and an
// /AA left empty goes. On a field merged with its widget only the widget's
// keys change, and the field events it inherits from a parent are copied in
// first, so they keep applying. Widgets only: a link's action is
// EPDFAnnot_SetAction().
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetEventAction(FPDF_ANNOTATION annot, int event, FPDF_ACTION action);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_ACTION_H_
