// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_VIEW_H_
#define PUBLIC_EPDF_VIEW_H_

// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// EmbedPDF's additions to public/fpdfview.h: the thread lifecycle, encryption
// and permissions, page boxes, rendering, and pages by object number.

// Experimental EmbedPDF Extension API.
// Function: EPDF_InitThread / EPDF_ShutdownThread
//          Thread-confined runtime lifecycle. Initialize / tear down PDFium for
//          the CALLING thread.
// Comments:
//          When the runtime is built with per-thread globals
//          (embedpdf_thread_local_globals), each worker thread owns its own
//          PDFium state, so every thread that uses PDFium MUST call
//          EPDF_InitThread before creating any handle and EPDF_ShutdownThread
//          after closing every handle it created. Handles must never cross
//          threads. When per-thread globals are disabled, they start and tear
//          down the process's one PDFium, as FPDF_InitLibrary and
//          FPDF_DestroyLibrary do.
//          The calls are counted per thread. Callers that share a thread
//          (two runtimes loaded over the same native library) each call
//          EPDF_InitThread once and EPDF_ShutdownThread once, and only the
//          last EPDF_ShutdownThread tears PDFium down: a caller that leaves
//          does not take PDFium from the ones still using it.
//          EPDF_ShutdownThread is lifecycle-strict: a caller makes it only
//          after closing every handle it created on the calling thread.
FPDF_EXPORT void FPDF_CALLCONV EPDF_InitThread();
FPDF_EXPORT void FPDF_CALLCONV EPDF_ShutdownThread();

// Permission flags for EPDF_SetEncryption allowed_flags parameter
#define EPDF_PERM_PRINT (1 << 2)          // Bit 3: Print document
#define EPDF_PERM_MODIFY (1 << 3)         // Bit 4: Modify contents
#define EPDF_PERM_COPY (1 << 4)           // Bit 5: Copy/extract text
#define EPDF_PERM_ANNOT (1 << 5)          // Bit 6: Add/modify annotations
#define EPDF_PERM_FILL_FORMS (1 << 8)     // Bit 9: Fill form fields
#define EPDF_PERM_ACCESSIBILITY (1 << 9)  // Bit 10: Extract for accessibility
#define EPDF_PERM_ASSEMBLE (1 << 10)      // Bit 11: Assemble document
#define EPDF_PERM_PRINT_HIGH (1 << 11)    // Bit 12: High quality print

// Experimental EmbedPDF Extension API.
// Sets AES-256 encryption on a document with user/owner passwords and
// permissions.
//
//   document        - handle to a document. Must not already be encrypted.
//   user_password   - password required to open the document.
//                     NULL or empty = no open password (document opens freely
//                     but has owner restrictions enforced by compliant
//                     readers).
//   owner_password  - password required to change permissions. Required, cannot
//                     be NULL or empty.
//   allowed_flags   - OR'd combination of EPDF_PERM_* flags indicating which
//                     actions ARE ALLOWED. Internally converted to proper P
//                     value.
//
// Returns TRUE on success, FALSE on failure (e.g., already encrypted, NULL
// owner password).
// Note: Must be called before FPDF_SaveAsCopy for encryption to take effect.
//
// Example allowed_flags: EPDF_PERM_PRINT | EPDF_PERM_COPY
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_SetEncryption(FPDF_DOCUMENT document,
                   FPDF_BYTESTRING user_password,
                   FPDF_BYTESTRING owner_password,
                   unsigned long allowed_flags);

// Experimental EmbedPDF Extension API.
// Clears any pending encryption set by EPDF_SetEncryption.
// To remove encryption from an already-encrypted document, use
// FPDF_REMOVE_SECURITY flag with FPDF_SaveAsCopy instead.
//
//   document - handle to a document.
//
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_RemoveEncryption(FPDF_DOCUMENT document);

#define EPDF_PASSWORD_PERMISSION_INVALID 0
#define EPDF_PASSWORD_PERMISSION_NONE 1
#define EPDF_PASSWORD_PERMISSION_USER 2
#define EPDF_PASSWORD_PERMISSION_OWNER 3

// Experimental EmbedPDF Extension API.
// Checks what permissions the given password proves without changing the
// document's current owner-unlocked state.
//
//   document                      - handle to a document
//   password                      - raw password to check
//   out_kind                      - receives EPDF_PASSWORD_PERMISSION_*
//   out_user_permissions          - receives the document's user permission
//   bits out_effective_permissions     - receives permissions implied by
//   |password| out_security_handler_revision - receives the security handler
//   revision
//
// Returns TRUE if:
//   - document is not encrypted (kind NONE, full permissions, revision -1)
//   - password is valid as user or owner
//
// Returns FALSE if:
//   - document is NULL
//   - any out pointer is NULL
//   - document has no parser
//   - password is invalid for an encrypted document
//
// This is a password probe, not a runtime permission override. It must not be
// used as evidence that the current document handle is owner-unlocked.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_CheckPasswordPermissions(FPDF_DOCUMENT document,
                              FPDF_BYTESTRING password,
                              int* out_kind,
                              unsigned int* out_user_permissions,
                              unsigned int* out_effective_permissions,
                              int* out_security_handler_revision);

// Experimental EmbedPDF Extension API.
// Attempts to unlock owner permissions on an already-opened encrypted document.
//
//   document        - handle to a document
//   owner_password  - the owner password to verify (raw, encoding handled
//   internally)
//
// Returns TRUE if the password is valid and owner permissions are now unlocked.
// Returns FALSE if:
//   - document is NULL
//   - document has no parser (not loaded from file)
//   - document is not encrypted
//   - password is empty or invalid
//
// After successful unlock, FPDF_GetDocPermissions() will return full
// permissions.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_UnlockOwnerPermissions(FPDF_DOCUMENT document,
                            FPDF_BYTESTRING owner_password);

// Experimental EmbedPDF Extension API.
// Runtime policy override for EmbedPDF-managed sessions.
//
//   document - handle to a document
//   enabled  - TRUE to make PDFium internals behave as owner-unlocked, FALSE to
//              restore the normal user-permission state for this handle
//
// This does not prove an owner password and does not authorize the caller.
// Callers must enforce JWT/PDF permission policy outside PDFium. This exists so
// high-level PDFium subsystems that consult permissions (notably forms) do not
// silently block operations after app-level authorization has already happened.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_SetRuntimeOwnerPermissions(FPDF_DOCUMENT document, FPDF_BOOL enabled);

// Experimental EmbedPDF Extension API.
// Checks if a document is encrypted.
//
//   document - handle to a document
//
// Returns TRUE if the document has encryption (Standard security handler).
// Returns FALSE if:
//   - document is NULL
//   - document has no parser
//   - document is not encrypted
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDF_IsEncrypted(FPDF_DOCUMENT document);

// Experimental EmbedPDF Extension API.
// Checks if owner permissions are currently unlocked on an encrypted document.
//
//   document - handle to a document
//
// Returns TRUE if:
//   - Document is encrypted AND owner password was verified (at load or via
//   UnlockOwner)
// Returns FALSE if:
//   - document is NULL
//   - document has no parser
//   - document is not encrypted
//   - owner permissions are not unlocked
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_IsOwnerUnlocked(FPDF_DOCUMENT document);

// Page box type used by EPDF_GetPageBoxByIndex().
typedef enum EPDF_PAGE_BOX_TYPE {
  EPDF_PAGE_BOX_MEDIA = 0,
  EPDF_PAGE_BOX_CROP = 1,
  EPDF_PAGE_BOX_BLEED = 2,
  EPDF_PAGE_BOX_TRIM = 3,
  EPDF_PAGE_BOX_ART = 4,
} EPDF_PAGE_BOX_TYPE;

// Experimental EmbedPDF API.
// Function: EPDF_GetPageRotateByIndex
//          Get the page's /Rotate at the given index as the file writes it,
//          resolved through page-tree inheritance, without parsing the page
//          contents. No normalizing: a value that isn't a multiple of 90, or
//          is negative or past 360, comes back as written; the caller applies
//          the rules.
// Parameters:
//          document    -   Handle to document. Returned by FPDF_LoadDocument().
//          page_index  -   Page index, zero for the first page.
//          rotate      -   Receives the /Rotate value, 0 when absent.
// Return value:
//          Non-zero for success. 0 on error (document or page not found).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_GetPageRotateByIndex(FPDF_DOCUMENT document, int page_index, int* rotate);

// Experimental EmbedPDF API.
// Function: EPDF_GetPageBoxByIndex
//          Get a page box at the given index without loading or parsing the
//          page.
// Parameters:
//          document    -   Handle to document. Returned by FPDF_LoadDocument().
//          page_index  -   Page index, zero for the first page.
//          box_type    -   The page box to query.
//          box         -   Pointer to a FS_RECTF to receive the box (in
//          points).
// Return value:
//          Non-zero for success. 0 for error, or when the box is absent, empty
//          or not four numbers.
// Comments:
//          The box as the file writes it, with its corners put in order.
//          MediaBox and CropBox are resolved through page-tree inheritance;
//          BleedBox, TrimBox and ArtBox are the page's own (ISO 32000-1 Table
//          30). No defaults and no clipping to the media box: the caller
//          applies those.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_GetPageBoxByIndex(FPDF_DOCUMENT document,
                       int page_index,
                       EPDF_PAGE_BOX_TYPE box_type,
                       FS_RECTF* box);

// Experimental EmbedPDF API.
// Function: EPDF_GetPageUserUnitByIndex
//          Get /UserUnit at the given page index without loading or parsing the
//          page.
// Parameters:
//          document    -   Handle to document. Returned by FPDF_LoadDocument().
//          page_index  -   Page index, zero for the first page.
//          user_unit   -   Pointer to a float to receive the user unit.
// Return value:
//          Non-zero for success. 0 for error.
// Comments:
//          Missing or invalid /UserUnit values return the PDF default 1.0.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_GetPageUserUnitByIndex(FPDF_DOCUMENT document,
                            int page_index,
                            float* user_unit);

// Experimental EmbedPDF API.
// Function: EPDF_GetPageTabOrderByIndex
//          Get the page's /Tabs at the given index, the order its annotations
//          take keyboard focus in, as the file writes it, without loading or
//          parsing the page: R (rows), C (columns), S (structure), or a name
//          PDF 2.0 adds (A, W). /Tabs isn't inherited.
// Parameters:
//          document    -   Handle to document. Returned by FPDF_LoadDocument().
//          page_index  -   Page index, zero for the first page.
//          buffer      -   Receives the name's bytes and a trailing NUL. May
//                          be NULL to query the length.
//          buflen      -   The size of |buffer| in bytes.
// Return value:
//          The name's length in bytes, including the trailing NUL. 0 when the
//          page sets no /Tabs, or on error (document or page not found).
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDF_GetPageTabOrderByIndex(FPDF_DOCUMENT document,
                            int page_index,
                            void* buffer,
                            unsigned long buflen);

// Experimental EmbedPDF API.
// Function: EPDF_LoadPageNormalized
//          Load a page with rotation normalized to 0 degrees.
//          All subsequent operations (GetPageWidth, annotations, text,
//          rendering) will use normalized coordinates as if the page had no
//          rotation.
// Parameters:
//          document            -   Handle to document.
//          page_index          -   Page index, zero for the first page.
//          out_original_rotation - Optional. Receives the original rotation
//          (0-3).
//                                  Can be NULL if not needed.
// Return value:
//          Handle to the loaded page, or NULL on failure.
// Comments:
//          The returned page must be closed with FPDF_ClosePage().
//          Use out_original_rotation to know the actual page rotation for
//          display.
FPDF_EXPORT FPDF_PAGE FPDF_CALLCONV
EPDF_LoadPageNormalized(FPDF_DOCUMENT document,
                        int page_index,
                        int* out_original_rotation);

// Experimental EmbedPDF Extension API.
// Draw form field widgets, from their appearances. It works with or without
// FPDF_ANNOT, which draws the other annotations: with both, the widgets come
// after them; alone, the page shows its widgets and no other annotation.
// Hidden widgets stay hidden, and so do no-view ones unless printing. A widget
// with no appearance is drawn in memory; the render writes nothing. Without
// this flag widgets are left out, as upstream does.
#define EPDF_RENDER_WIDGETS 0x8000

// Experimental EmbedPDF Extension API.
// Renders the appearance stream of a single annotation to a bitmap.
//
//   bitmap         - Destination bitmap handle.
//   page           - Page that owns the annotation.
//   annot          - Annotation handle obtained from any FPDFAnnot_* API.
//   appearanceMode - One of FPDF_ANNOT_APPEARANCEMODE_*.
//   state          - NULL or empty to draw the state /AS selects (empty for
//                    bindings that can't pass NULL). Otherwise the name
//                    of a state of the mode's appearance subdictionary (as
//                    EPDFAnnot_GetAppearanceStateName gives it), drawn
//                    whatever /AS says. A mode whose entry is a single
//                    stream, or a state the subdictionary lacks, draws
//                    nothing.
//   matrix         - Matrix to transform the annotation
//   flags          - Flags to control the rendering
//
// Returns true if the rendering was successful, false otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_RenderAnnotBitmap(FPDF_BITMAP bitmap,
                       FPDF_PAGE page,
                       FPDF_ANNOTATION annot,
                       FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                       FPDF_BYTESTRING state,
                       const FS_MATRIX* matrix,
                       int flags);

// Experimental EmbedPDF Extension API.
// Renders the annotation's appearance turned back upright: as the page shows
// it, then turned back by `degrees` about the middle of `box`, whether the
// turn is in the appearance's /Matrix or drawn in its stream. `matrix` maps
// page space to the bitmap, so mapping `box` fills the bitmap with the
// upright drawing, for UI layers that turn it themselves. With `degrees` 0
// or no `box`, it renders as the page shows it.
//
//   bitmap, page, annot, appearanceMode, state, matrix, flags - as
//            EPDF_RenderAnnotBitmap.
//   degrees - the turn to take out, counterclockwise (the PDF convention).
//   box     - the box it turns about, in page space, before turning.
//
// Returns true if the rendering was successful, false otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_RenderAnnotBitmapUnrotated(FPDF_BITMAP bitmap,
                                FPDF_PAGE page,
                                FPDF_ANNOTATION annot,
                                FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                                FPDF_BYTESTRING state,
                                float degrees,
                                const FS_RECTF* box,
                                const FS_MATRIX* matrix,
                                int flags);

// Experimental EmbedPDF Extension API.
// Empties the page's image cache and frees the decoded images it holds, so
// the page renders its images as a newly loaded page does. Renders of one
// page share its image cache, and a cached decode depends on the size of the
// render that made it: call this where each render must be independent of
// earlier ones, such as before reusing a loaded page for a later job.
//
//   page - Handle to the page.
//
// Returns false, and changes nothing, when `page` is invalid or a
// progressive render of it (FPDF_RenderPageBitmap_Start) is in progress.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFPage_ResetRenderCache(FPDF_PAGE page);

// Experimental EmbedPDF Extension API.
// Sets how many bytes of decoded images are kept across page loads, for every
// document of this PDFium instance (the calling thread's, in thread-confined
// builds), so an image rendered again after its page's image cache was
// emptied is not decoded again. A kept decode renders the same bytes as a new
// one. Above the budget the least recently used decodes are dropped. 0, the
// default, keeps none and empties the store.
//
// The caller must set the budget to 0 before changing anything a decode
// depends on: an image stream, its colour space or the resources naming it.
// Closing a document drops its decodes.
//
//   bytes - The most bytes kept, counting images and their masks.
FPDF_EXPORT void FPDF_CALLCONV EPDF_SetDecodedImageBudget(unsigned long bytes);

// Experimental EmbedPDF Extension API.
// Returns how many bytes of decoded images are kept (see
// EPDF_SetDecodedImageBudget).
FPDF_EXPORT unsigned long FPDF_CALLCONV EPDF_GetDecodedImageBytes();

// Experimental EmbedPDF Extension API.
// Load a page by its PDF indirect object number.
// Similar to FPDF_LoadPage() but keyed by the page dictionary's object
// number instead of page index.
//
//   document - handle to the document.
//   obj_num  - the indirect object number of the page dictionary.
//
// Returns a handle to the loaded page, or NULL if the object number
// does not correspond to a page. The caller must close the returned
// handle with FPDF_ClosePage().
FPDF_EXPORT FPDF_PAGE FPDF_CALLCONV
EPDFDoc_LoadPageByObjectNumber(FPDF_DOCUMENT document, unsigned int obj_num);

// Experimental EmbedPDF Extension API.
// Load a page by its PDF indirect object number with rotation normalized to
// 0 degrees. Like EPDFDoc_LoadPageByObjectNumber(), but forces the page's
// rotation override to 0 so all subsequent operations (GetPageWidth,
// annotations, text, rendering) use normalized coordinates as if the page had
// no rotation. The intrinsic rotation is surfaced separately via
// EPDF_GetPageRotateByIndex().
//
//   document - handle to the document.
//   obj_num  - the indirect object number of the page dictionary.
//
// Returns a handle to the loaded page, or NULL if the object number
// does not correspond to a page. The caller must close the returned
// handle with FPDF_ClosePage().
FPDF_EXPORT FPDF_PAGE FPDF_CALLCONV
EPDFDoc_LoadPageByObjectNumberNormalized(FPDF_DOCUMENT document,
                                         unsigned int obj_num);

// Experimental EmbedPDF Extension API.
// Get the PDF indirect object number of a page's dictionary by page index.
// Unlike FPDF_LoadPage(), this does not construct a page object or parse page
// content.
//
//   document   - handle to the document.
//   page_index - zero-based page index.
//
// Returns the page dictionary object number (> 0) on success, or 0 if the
// document/index is invalid, the page dictionary is a direct object, or the
// page is an XFA page.
FPDF_EXPORT unsigned int FPDF_CALLCONV
EPDFDoc_GetPageObjectNumberByIndex(FPDF_DOCUMENT document, int page_index);

// Experimental EmbedPDF Extension API.
// Delete the first visible page whose page dictionary has |obj_num| as its PDF
// indirect object number. Unlike FPDFPage_Delete(), this remains stable across
// page moves because it is keyed by page identity rather than page position.
//
//   document - handle to the document.
//   obj_num  - the indirect object number of the page dictionary.
//
// Returns TRUE if a matching page was found and deleted, or FALSE if |document|
// is invalid, |obj_num| is 0, or |obj_num| does not correspond to a visible
// page. If a malformed PDF references the same /Page object more than once,
// the first visible occurrence resolved by PDFium is deleted.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_DeletePageByObjectNumber(FPDF_DOCUMENT document, unsigned int obj_num);

// Experimental EmbedPDF Extension API.
// Set the rotation of the page whose page dictionary has |obj_num| as its PDF
// indirect object number. Unlike FPDFPage_SetRotation(), this does not require
// loading or parsing the page.
//
//   document - handle to the document.
//   obj_num  - the indirect object number of the page dictionary.
//   rotate   - the rotation value, one of:
//                0 - No rotation.
//                1 - Rotated 90 degrees clockwise.
//                2 - Rotated 180 degrees clockwise.
//                3 - Rotated 270 degrees clockwise.
//
// Returns TRUE if a matching page was found and updated, or FALSE if |document|
// is invalid, |obj_num| is 0, |rotate| is outside 0..3, or |obj_num| does not
// correspond to a visible page.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_SetPageRotationByObjectNumber(FPDF_DOCUMENT document,
                                      unsigned int obj_num,
                                      int rotate);

// Experimental EmbedPDF Extension API.
// Get the PDF indirect object number of a page's dictionary.
//
//   page - handle to the page.
//
// Returns the object number (> 0) on success, or 0 if the page handle is
// invalid, the page dictionary is a direct object, or the page is an XFA page.
FPDF_EXPORT unsigned int FPDF_CALLCONV EPDFPage_GetObjectNumber(FPDF_PAGE page);

// Experimental EmbedPDF Extension API.
// Starts loading a page by its PDF indirect object number, in slices: makes
// the page and parses nothing yet. Continue the load with
// EPDFPage_ContinueLoad() until it returns 1; FPDF_ClosePage() between slices
// abandons it.
//
// Until the load ends, a call that reads the page - render, text, objects,
// annotations, content generation - first finishes it in one go, so no call
// sees half a page. EPDFPage_ContinueLoad(), EPDFPage_GetParsedSize() (the
// size parsed so far), EPDFPage_IsContentCurrent(), EPDFPage_IsValid(),
// EPDFPage_GetObjectNumber(), the page's size and FPDF_ClosePage() leave it
// loading.
//
//   document   - handle to the document.
//   obj_num    - the indirect object number of the page dictionary.
//   normalized - true to load it as
//                EPDFDoc_LoadPageByObjectNumberNormalized() does, with its
//                rotation normalized to 0 degrees.
//
// Returns a handle to the page, or NULL if the object number does not
// correspond to a page. Close it with FPDF_ClosePage().
FPDF_EXPORT FPDF_PAGE FPDF_CALLCONV
EPDFDoc_StartLoadPageByObjectNumber(FPDF_DOCUMENT document,
                                    unsigned int obj_num,
                                    FPDF_BOOL normalized);

// Experimental EmbedPDF Extension API.
// Parses more of a page that EPDFDoc_StartLoadPageByObjectNumber() started,
// pausing once `budget_ms` milliseconds have passed; 0 parses one step. A step
// is about 100 objects, a form's start or a content stream's decoding, so a
// slice can run over its budget by one step. The parse pauses inside forms
// too: a page drawn inside one huge form loads in slices like any other.
// Loading in slices or in one go (FPDF_LoadPage()) gives the same page.
//
//   page      - handle to the page.
//   budget_ms - how long to parse before pausing.
//
// Returns 1 once the page is loaded (at once for a page already loaded), 0
// when the budget ran out first, and -1 when `page` is NULL or wasn't loaded
// as a page.
FPDF_EXPORT int FPDF_CALLCONV EPDFPage_ContinueLoad(FPDF_PAGE page,
                                                    int budget_ms);

// Experimental EmbedPDF Extension API.
// What a page's parsed content costs in memory (EPDFPage_GetParsedSize()).
// Every count is 64 bits, so the struct is laid out the same on every
// platform: 40 bytes, the counts at 0, 8, 16 and 24, `complete` at 32.
typedef struct EPDF_PAGE_PARSED_SIZE_ {
  // Every page object, the objects inside forms included.
  unsigned long long objects;
  // The points of the paths, a geometry several paths share counted once.
  unsigned long long path_points;
  // The character codes of the text objects.
  unsigned long long text_chars;
  // An estimate of the bytes the objects hold, on the safe side: their own
  // sizes, points, codes, positions and inline images. Fonts, cached images
  // and the states objects share belong to the document and are not counted.
  unsigned long long estimated_bytes;
  // False while the page is still parsing: the size of what it has parsed.
  FPDF_BOOL complete;
} EPDF_PAGE_PARSED_SIZE;

// Experimental EmbedPDF Extension API.
// Gets what |page|'s parsed content costs in memory, counted while it parsed,
// and counted again after its objects changed.
//
//   page - handle to the page.
//   size - receives the size.
//
// Returns false, and leaves |size| alone, when |page| or |size| is null.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_GetParsedSize(FPDF_PAGE page, EPDF_PAGE_PARSED_SIZE* size);

// Experimental EmbedPDF Extension API.
// Returns whether |page|'s parsed objects still match its document: the page's
// boxes, its rotation, its content streams and the stream of every form placed
// on it are the versions the objects were parsed from, or last generated into
// (FPDFPage_GenerateContent()). A page that no longer matches renders what its
// content was: load it again.
//
// A layer makes a new version of an object for every write inside a
// transaction (EPDFLayer_BeginTransaction()), and an abort brings back the one
// before, so a page loaded before a transaction is current after an abort and
// after a commit that left its content alone. A write outside a transaction,
// and every write to a document without a layer, changes objects in place,
// which this can't see: close the pages such a write changes.
//
// Cheap: it compares only after the layer changed since the last call.
//
//   page - handle to the page.
//
// Returns false when |page| is null.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFPage_IsContentCurrent(FPDF_PAGE page);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_VIEW_H_
