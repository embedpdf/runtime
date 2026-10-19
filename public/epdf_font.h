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
// Clear all registered fonts, the fallback fonts and the last resort.
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
// weight when the axis covers it). A variable font that cannot be instanced
// is refused by registration, as are programs that are not TrueType or
// OpenType/CFF sfnts (Type1, bare CFF, collections, WOFF).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFFont_IsInstanced(EPDF_FONT_ID font_id);

// EmbedPDF: the persistent identity of a registered font, as resolved at
// registration: the family (the name given, else the font's own), the
// weight (100..900) and the italic flag. A saved document names a face by
// these (the font descriptor's /FontFamily, /FontWeight, /ItalicAngle), and
// a rich text body reads back with them, so a host maps them to its own
// font keys. |buffer| receives the family as UTF-8, NUL-terminated; the
// return value is the byte length including the terminator, 0 for an
// unknown id. Pass NULL/0 to query the length.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFont_GetFamilyName(EPDF_FONT_ID font_id, char* buffer, unsigned long buflen);
FPDF_EXPORT int FPDF_CALLCONV EPDFFont_GetWeight(EPDF_FONT_ID font_id);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFFont_IsItalic(EPDF_FONT_ID font_id);

// EmbedPDF: how much of a registered font's program the appearances authored
// in a document carry. DEFAULT subsets annotation text (FreeText, redaction
// labels) and embeds form field text whole; SUBSET and FULL apply to both.
// Session state on the document handle, never written to the file; applies
// to appearances generated after the call. A font whose fsType forbids
// subsetting is embedded whole under every policy. Programs already in the
// document are never re-embedded or subset by this setting.
#define EPDF_FONT_EMBEDDING_POLICY_DEFAULT 0
#define EPDF_FONT_EMBEDDING_POLICY_SUBSET 1
#define EPDF_FONT_EMBEDDING_POLICY_FULL 2

// Returns false for an invalid document or policy value.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_SetFontEmbeddingPolicy(FPDF_DOCUMENT document, int policy);

// Returns one of EPDF_FONT_EMBEDDING_POLICY_*, or -1 for an invalid document.
FPDF_EXPORT int FPDF_CALLCONV
EPDFDoc_GetFontEmbeddingPolicy(FPDF_DOCUMENT document);

// Experimental EmbedPDF Extension API.
// Add a registered font to the fallback fonts: the fonts that draw a
// character the selected font doesn't have, in new text and where a PDF page
// needs a substitute font. They are tried tier by tier, lowest |tier| first.
// Adding a font that is already one changes its tier only.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFont_AddFallbackFont(EPDF_FONT_ID font_id, int tier);

// Experimental EmbedPDF Extension API.
// Clear the fallback fonts without unregistering them.
FPDF_EXPORT void FPDF_CALLCONV EPDFFont_ClearFallbackFonts(void);

// Experimental EmbedPDF Extension API.
// Make a registered font the last resort: new text draws a character that
// neither its font nor any fallback has with it, so the character shows as
// that font's box and the text keeps it. Such a character is still reported
// as missing (EPDFDoc_CountMissingCharacters()). A page's own text never uses
// it. Pass 0 to clear it. Returns false for an unknown id.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFont_SetLastResortFont(EPDF_FONT_ID font_id);

// Experimental EmbedPDF Extension API.
// The last-resort font; 0 when there is none.
FPDF_EXPORT EPDF_FONT_ID FPDF_CALLCONV EPDFFont_GetLastResortFont(void);

// Experimental EmbedPDF Extension API.
// The registered fallback fonts in the order new text tries them for a
// style: tier by tier, lowest first; within a tier the closest weight and
// italic first, then the order they were added in. Fonts not authorized for
// editing are left out. The first of them that has a whole character
// (a base character with its marks) is the one that draws it.
//
//   weight, italic - the style.
//   ids            - receives up to |capacity| font ids; may be NULL.
//   capacity       - the number of ids |ids| holds.
//
// Returns the number of fonts in the order, whatever |capacity| is.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFont_GetFallbackOrder(int weight,
                          FPDF_BOOL italic,
                          EPDF_FONT_ID* ids,
                          unsigned long capacity);

// Experimental EmbedPDF Extension API.
// A registered font's program: the bytes the writer embeds (the static
// instance for a variable font). Returns the byte length, 0 for an unknown
// id; pass NULL/0 to query it. Nothing is copied when |buflen| is too small.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFont_GetProgramData(EPDF_FONT_ID font_id,
                        void* buffer,
                        unsigned long buflen);

// Experimental EmbedPDF Extension API.
// What a registered font's program is: EPDF_FONT_FORMAT_TRUETYPE or
// EPDF_FONT_FORMAT_OPENTYPE_CFF; -1 for an unknown id.
FPDF_EXPORT int FPDF_CALLCONV EPDFFont_GetProgramFormat(EPDF_FONT_ID font_id);

// Experimental EmbedPDF Extension API.
// The SHA-256 of EPDFFont_GetProgramData()'s bytes into |sha256| (32
// bytes). Returns false for an unknown id.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFont_GetProgramHash(EPDF_FONT_ID font_id, unsigned char* sha256);

// Experimental EmbedPDF Extension API.
// The characters a registered font maps, as ranges: |ranges| receives up to
// |range_capacity| pairs (first code point, count). Returns the number of
// ranges, 0 for an unknown id; pass NULL/0 to query it.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFont_GetCoverage(EPDF_FONT_ID font_id,
                     uint32_t* ranges,
                     unsigned long range_capacity);

// Experimental EmbedPDF Extension API.
// A registered font's line metrics as the layout reads them: its hhea
// ascender and descender over its units per em (descent positive). Returns
// false for an unknown id.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFFont_GetMetrics(EPDF_FONT_ID font_id,
                                                        float* ascent,
                                                        float* descent);

// Experimental EmbedPDF Extension API.
// A registered font's PostScript name, as its embedded /BaseFont spells it
// (without a subset tag). Bytes and a trailing NUL; returns the length
// including the NUL, 0 for an unknown id; pass NULL/0 to query it.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFont_GetPostScriptName(EPDF_FONT_ID font_id,
                           char* buffer,
                           unsigned long buflen);

// ---- A document's fonts -----------------------------------------------------

// Experimental EmbedPDF Extension API.
// The fonts a document uses, as faces (family, weight, italic) and the
// programs behind each: a detached snapshot, read with the EPDFFontFaces_*
// getters and freed with EPDFFontFaces_Close(). Found in the document's
// dictionaries without loading a page: /AcroForm /DR, fields' own /DR, every
// page's resources (inherited ones too), form XObjects, tiling patterns,
// soft masks, Type 3 glyph resources and every state of every annotation
// appearance. Facts only: what may be written with a font is the caller's
// decision. A snapshot goes stale with the next write.
typedef struct epdf_font_faces_t__* EPDF_FONT_FACES;

// What a program is (EPDFFontFaces_GetProgramFormat).
#define EPDF_FONT_FORMAT_NONE 0          // the PDF only names the font
#define EPDF_FONT_FORMAT_TRUETYPE 1      // TrueType outlines (sfnt)
#define EPDF_FONT_FORMAT_OPENTYPE_CFF 2  // OpenType with CFF outlines (sfnt)
#define EPDF_FONT_FORMAT_CFF 3           // bare CFF (/FontFile3 /Type1C ...)
#define EPDF_FONT_FORMAT_TYPE1 4         // Type 1 (/FontFile)
#define EPDF_FONT_FORMAT_TYPE3 5         // glyphs drawn by content streams

// How much of the font the PDF holds (EPDFFontFaces_GetProgramEmbedded).
#define EPDF_FONT_EMBEDDED_NONE 0
#define EPDF_FONT_EMBEDDED_WHOLE 1
#define EPDF_FONT_EMBEDDED_SUBSET 2  // the /BaseFont carries a subset tag

// Returns NULL for a NULL document.
FPDF_EXPORT EPDF_FONT_FACES FPDF_CALLCONV
EPDFFontFaces_Load(FPDF_DOCUMENT document);
FPDF_EXPORT void FPDF_CALLCONV EPDFFontFaces_Close(EPDF_FONT_FACES faces);

// The number of faces; -1 for a NULL snapshot.
FPDF_EXPORT int FPDF_CALLCONV EPDFFontFaces_Count(EPDF_FONT_FACES faces);

// A face's family as UTF-16LE with a trailing NUL. Returns the length in
// bytes including the NUL, 0 for an index out of range; pass NULL/0 to
// query it. Empty families (some Type 3 fonts) return 2.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontFaces_GetFamily(EPDF_FONT_FACES faces,
                        int face,
                        FPDF_WCHAR* buffer,
                        unsigned long buflen);
// 100 to 900; 0 for an index out of range.
FPDF_EXPORT int FPDF_CALLCONV EPDFFontFaces_GetWeight(EPDF_FONT_FACES faces,
                                                      int face);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontFaces_IsItalic(EPDF_FONT_FACES faces, int face);

// The programs behind a face: one per program stream, in the order found,
// plus one per kind for fonts without a program. -1 out of range.
FPDF_EXPORT int FPDF_CALLCONV EPDFFontFaces_CountPrograms(EPDF_FONT_FACES faces,
                                                          int face);

// The program stream's object number: what EPDFDoc_GetFontProgramData() and
// EPDFAnnot_SetDefaultAppearanceDocumentFont() take. 0 for a font without a
// program, an inline program stream, or an index out of range.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontFaces_GetProgramObjNum(EPDF_FONT_FACES faces, int face, int program);
// One of EPDF_FONT_FORMAT_*; -1 out of range.
FPDF_EXPORT int FPDF_CALLCONV
EPDFFontFaces_GetProgramFormat(EPDF_FONT_FACES faces, int face, int program);
// One of EPDF_FONT_EMBEDDED_*; -1 out of range.
FPDF_EXPORT int FPDF_CALLCONV
EPDFFontFaces_GetProgramEmbedded(EPDF_FONT_FACES faces, int face, int program);
// The /BaseFont without its subset tag (a Type0 font's descendant's): bytes
// and a trailing NUL. Returns the length including the NUL, 0 out of range;
// pass NULL/0 to query it.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontFaces_GetProgramPostScriptName(EPDF_FONT_FACES faces,
                                       int face,
                                       int program,
                                       char* buffer,
                                       unsigned long buflen);
// What the program's OS/2 fsType grants: one of EPDF_FONT_EMBEDDING_*; -1
// when it has no OS/2 table to say (Type 1, bare CFF, no program) or out of
// range.
FPDF_EXPORT int FPDF_CALLCONV
EPDFFontFaces_GetProgramEmbeddingPermission(EPDF_FONT_FACES faces,
                                            int face,
                                            int program);
// Whether the program loads, has glyphs and maps characters through a
// Unicode cmap: whether the layout can write any character it maps.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontFaces_ProgramMapsUnicode(EPDF_FONT_FACES faces, int face, int program);
// The SHA-256 of the decoded program into |sha256| (32 bytes). Returns
// false for a font without a program or an index out of range.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontFaces_GetProgramHash(EPDF_FONT_FACES faces,
                             int face,
                             int program,
                             unsigned char* sha256);
// The SHA-256 of what a browser loads for the program into |sha256| (32
// bytes): the program itself when it is TrueType or OpenType with a Unicode
// cmap, else the display program built for it
// (EPDFDoc_GetFontDisplayProgramData()). Returns false when a browser can't
// draw it (Type 3, nothing embedded, a program that doesn't load) or for an
// index out of range.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontFaces_GetProgramDisplayHash(EPDF_FONT_FACES faces,
                                    int face,
                                    int program,
                                    unsigned char* sha256);
// The program's line metrics as the layout reads them (em, descent
// positive). Returns false when it doesn't load or out of range.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontFaces_GetProgramMetrics(EPDF_FONT_FACES faces,
                                int face,
                                int program,
                                float* ascent,
                                float* descent);

// Experimental EmbedPDF Extension API.
// A program the document's fonts use, by its stream's object number: its
// decoded bytes. Returns the length, 0 when |program_objnum| isn't such a
// program; pass NULL/0 to query it. Nothing is copied when |buflen| is too
// small.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetFontProgramData(FPDF_DOCUMENT document,
                           unsigned long program_objnum,
                           void* buffer,
                           unsigned long buflen);

// Experimental EmbedPDF Extension API.
// What a browser loads for such a program: the program itself when it is
// TrueType or OpenType with a Unicode cmap. Otherwise (Type 1, bare CFF, a
// program without a Unicode cmap) an OpenType font built from it: its
// glyphs, a Unicode cmap holding exactly the characters the engine writes
// with the font (each through the PDF's encoding to the glyph it draws), and
// the advances the engine lays them out with (/Widths). The same bytes every
// time. Returns the length, 0 when a browser can't draw it or
// |program_objnum| isn't such a program; pass NULL/0 to query it. Nothing is
// copied when |buflen| is too small.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetFontDisplayProgramData(FPDF_DOCUMENT document,
                                  unsigned long program_objnum,
                                  void* buffer,
                                  unsigned long buflen);

// Experimental EmbedPDF Extension API.
// The characters a browser draws with such a program, which are the ones the
// engine writes with it, as ranges: |ranges| receives up to
// |range_capacity| pairs (first code point, count). Returns the number of
// ranges, 0 when it maps none or isn't such a program; pass NULL/0 to query
// it.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetFontProgramCoverage(FPDF_DOCUMENT document,
                               unsigned long program_objnum,
                               uint32_t* ranges,
                               unsigned long range_capacity);

// ---- The fonts a write would use --------------------------------------------

// Experimental EmbedPDF Extension API.
// Which font each run of text would be drawn with, found the way a write
// finds it, without writing: the same /DA resolution, each face resolved
// once over all of its text, the same per-character fallback. A detached
// snapshot, read with the EPDFFontResolution_* getters and freed with
// EPDFFontResolution_Close().
typedef struct epdf_font_resolution_t__* EPDF_FONT_RESOLUTION;

// Where a run's font comes from (EPDFFontResolution_GetSource).
#define EPDF_FONT_SOURCE_STANDARD 0          // one of the standard 14
#define EPDF_FONT_SOURCE_REGISTERED 1        // a registered font
#define EPDF_FONT_SOURCE_DOCUMENT_PROGRAM 2  // a program the document holds
#define EPDF_FONT_SOURCE_DOCUMENT_FONT 3     // the box's /DA font as it is

//   document  - the document.
//   annot     - the box being edited (its own font counts first), or NULL
//               for a new one.
//   run_count - the number of runs.
//   texts     - each run's text, UTF-16LE, NUL-terminated.
//   families  - each run's family (a family name or a registered font's).
//   weights   - each run's weight, 100 to 900.
//   italics   - whether each run is italic.
//
// Returns NULL for a NULL document or arrays.
FPDF_EXPORT EPDF_FONT_RESOLUTION FPDF_CALLCONV
EPDFDoc_ResolveFonts(FPDF_DOCUMENT document,
                     FPDF_ANNOTATION annot,
                     int run_count,
                     const FPDF_WIDESTRING* texts,
                     const FPDF_WIDESTRING* families,
                     const int* weights,
                     const FPDF_BOOL* italics);
FPDF_EXPORT void FPDF_CALLCONV
EPDFFontResolution_Close(EPDF_FONT_RESOLUTION resolution);

// One of EPDF_FONT_SOURCE_*; -1 out of range.
FPDF_EXPORT int FPDF_CALLCONV
EPDFFontResolution_GetSource(EPDF_FONT_RESOLUTION resolution, int run);
// The registered font for EPDF_FONT_SOURCE_REGISTERED; 0 otherwise.
FPDF_EXPORT EPDF_FONT_ID FPDF_CALLCONV
EPDFFontResolution_GetRegisteredFont(EPDF_FONT_RESOLUTION resolution, int run);
// The program stream's object number for EPDF_FONT_SOURCE_DOCUMENT_PROGRAM,
// and for EPDF_FONT_SOURCE_DOCUMENT_FONT when it embeds one; 0 otherwise.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontResolution_GetProgramObjNum(EPDF_FONT_RESOLUTION resolution, int run);
// The font's /BaseFont: the standard name for EPDF_FONT_SOURCE_STANDARD
// ("Helvetica-Bold"). Bytes and a trailing NUL; returns the length
// including the NUL, 0 when there is none; pass NULL/0 to query it.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontResolution_GetBaseFontName(EPDF_FONT_RESOLUTION resolution,
                                   int run,
                                   char* buffer,
                                   unsigned long buflen);
// The face the run is drawn in: its family (UTF-16LE with a trailing NUL;
// the length in bytes, 0 out of range), weight and italic.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontResolution_GetFamily(EPDF_FONT_RESOLUTION resolution,
                             int run,
                             FPDF_WCHAR* buffer,
                             unsigned long buflen);
FPDF_EXPORT int FPDF_CALLCONV
EPDFFontResolution_GetWeight(EPDF_FONT_RESOLUTION resolution, int run);
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontResolution_IsItalic(EPDF_FONT_RESOLUTION resolution, int run);
// Whether the face isn't the one the run asked for (a substitute).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFFontResolution_IsDegraded(EPDF_FONT_RESOLUTION resolution, int run);
// The fallback fonts for the run's style, in the order they are tried
// (EPDFFont_GetFallbackOrder()). Returns their number; pass NULL/0 to
// query it.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontResolution_GetFallbacks(EPDF_FONT_RESOLUTION resolution,
                                int run,
                                EPDF_FONT_ID* ids,
                                unsigned long capacity);
// The characters (graphemes) no font has: they would be drawn as missing
// glyphs. Count, then each as UTF-16LE with a trailing NUL (the length in
// bytes, 0 out of range).
FPDF_EXPORT int FPDF_CALLCONV
EPDFFontResolution_CountMissingCharacters(EPDF_FONT_RESOLUTION resolution);
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFFontResolution_GetMissingCharacter(EPDF_FONT_RESOLUTION resolution,
                                       int index,
                                       FPDF_WCHAR* buffer,
                                       unsigned long buflen);

// ---- Characters a write drew with no font -----------------------------------

// Experimental EmbedPDF Extension API.
// The characters (graphemes) appearances generated for |document| drew with
// no font that has them, first seen first, since the list was last cleared:
// clear it before a write and read it after. At most 256 are kept.
FPDF_EXPORT void FPDF_CALLCONV
EPDFDoc_ClearMissingCharacters(FPDF_DOCUMENT document);
FPDF_EXPORT int FPDF_CALLCONV
EPDFDoc_CountMissingCharacters(FPDF_DOCUMENT document);
// Each as UTF-16LE with a trailing NUL: the length in bytes, 0 out of
// range; pass NULL/0 to query it.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetMissingCharacter(FPDF_DOCUMENT document,
                            int index,
                            FPDF_WCHAR* buffer,
                            unsigned long buflen);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_FONT_H_
