// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_SIGNATURE_H_
#define PUBLIC_EPDF_SIGNATURE_H_

#include <stdint.h>

// NOLINTNEXTLINE(build/include)
#include "epdf_digest.h"
#include "fpdf_save.h"
#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// ---------------------------------------------------------------------------
// Revisions.
//
// A revision is a byte prefix of the file. The revisions of a document are
// the cross-reference sections reached from the final startxref through
// /Prev, each closed by the "%%EOF" line ending that follows the startxref
// pointing at it. A boundary only counts when every section reachable from
// it (through /Prev and /XRefStm) lies below that boundary, so the prefix is
// a self-contained document; a pristine linearized file therefore has one
// revision, not two. Offsets are FILE offsets (a signature's /ByteRange is
// expressed in the same space), never header-relative parser offsets.
//
// Everything here reads the bytes the document was loaded from. Unsaved
// in-memory mutations are invisible to it by design.
//
// Layer documents (EPDFLayer_OpenLayer) were loaded from their base file
// followed by the delta they were opened with, and that is what every
// function here reads for them - never the base alone, never the layer's
// in-memory objects. A layer opened over a signed base plus the update that
// signed it reports exactly what the same bytes report as a plain document.
// ---------------------------------------------------------------------------

// Experimental EmbedPDF Extension API.
// Number of revisions, oldest first (index 0 is the original document).
// Returns -1 when the document has no parser (a new document), when the
// cross-reference table was rebuilt by scanning, when the chain is broken,
// or when the final startxref does not close the last revision. Any
// signature analysis on such a document is indeterminate.
FPDF_EXPORT int FPDF_CALLCONV EPDFDoc_GetRevisionCount(FPDF_DOCUMENT document);

// Experimental EmbedPDF Extension API.
// Byte offsets of revision |index|: |out_end| is the offset just past the
// closing "%%EOF" line ending (the revision is bytes [0, end)), and
// |out_xref_offset| is the offset of the cross-reference section the
// revision's startxref points at. Either out-param may be NULL.
// Returns FALSE when |index| is out of range or the chain is invalid.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_GetRevision(FPDF_DOCUMENT document,
                    int index,
                    unsigned long long* out_end,
                    unsigned long long* out_xref_offset);

// Experimental EmbedPDF Extension API.
// Open bytes [0, end) of the SAME underlying stream as an independent
// document. Zero-copy: a length-clamped view over the parent's file access.
// The parent's password unlocks it. |end| must be a revision end returned
// by EPDFDoc_GetRevision(); any other value returns NULL. The caller closes
// the result with FPDF_CloseDocument(), independently of the parent, but
// must not close the parent first.
FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFDoc_OpenRevision(FPDF_DOCUMENT document, unsigned long long end);

// Experimental EmbedPDF Extension API.
// Open a LAYER document's immutable base bytes followed by |delta| as an
// independent, read-only document: the bytes a save of the layer would
// produce, given the cumulative delta EPDFLayer_SaveDelta() writes for it
// (offsets notional from the base's append offset, /Prev into the base's
// last cross-reference section). Zero-copy for the base bytes; |delta| is
// copied. The result's revisions are the base's plus exactly one, which
// REPLACES the layer's loaded delta rather than following it. Returns NULL
// for a plain document (its bytes are not a frozen base), for an empty
// delta, or when the composition does not parse. Close with
// FPDF_CloseDocument(), independently of the layer, but not after it.
// Function: EPDFDoc_OpenBaseOverlayFromPath
//          Open, read-only, the document a layer's cumulative delta describes:
//          the immutable base followed by the delta read from |delta_path| (a
//          UTF-8 file system path the runtime opens itself and keeps open for
//          the life of the returned document). No copy of the base, no copy
//          of the delta. Close with FPDF_CloseDocument.
FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFDoc_OpenBaseOverlayFromPath(FPDF_DOCUMENT layer, FPDF_STRING delta_path);

// Function: EPDFDoc_OpenBaseOverlay
//          See EPDFDoc_OpenBaseOverlayFromPath; this variant takes the delta
//          bytes from memory (copied). The path variant reads the delta in
//          place from a file the runtime opens itself.
FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFDoc_OpenBaseOverlay(FPDF_DOCUMENT layer,
                        const void* delta,
                        unsigned long delta_len);

// ---------------------------------------------------------------------------
// Signature model.
//
// A detached, immutable snapshot of every signature field (/FT /Sig) in the
// document - signed or not - built the way EPDFForm_LoadModel builds the form
// model: through the reconciled form lens (declared fields merged with a page
// sweep), then copied into plain values that outlive the document. Loading
// never mutates the document. The model is stale after any mutation.
//
// Byte-level facts (coverage, revision index, the DER length of /Contents)
// are computed against the loaded bytes, like the revision APIs above.
// ---------------------------------------------------------------------------

typedef struct epdf_signature_model_t__* EPDF_SIGNATURE_MODEL;

#define EPDF_SIG_KIND_SIGNATURE 0      // /Type /Sig, or no /Type
#define EPDF_SIG_KIND_DOC_TIMESTAMP 1  // /Type /DocTimeStamp

// /ByteRange coverage of a signed field.
//   WHOLE_REVISION: [0, a) U [b, c) is a prefix whose end is a revision end
//                   and whose hole [a, b) is exactly the /Contents string,
//                   angle brackets included.
//                   The hole is verified against the PHYSICAL location of
//                   this signature's /Contents value in the file, never by
//                   byte equality alone.
//   PARTIAL:        well-formed ranges that do not describe such a prefix.
//   MALFORMED:      /ByteRange missing, not four integers, negative,
//                   overlapping, beyond the file, or /Contents not a valid
//                   DER object padded with zero bytes. A /V dictionary that
//                   repeats a key is never WHOLE_REVISION.
#define EPDF_SIG_COVERAGE_WHOLE_REVISION 0
#define EPDF_SIG_COVERAGE_PARTIAL 1
#define EPDF_SIG_COVERAGE_MALFORMED 2

// Keys for EPDFSig_GetString(). Names are returned as their text.
#define EPDF_SIG_STRING_FILTER 0
#define EPDF_SIG_STRING_SUBFILTER 1
#define EPDF_SIG_STRING_NAME 2
#define EPDF_SIG_STRING_REASON 3
#define EPDF_SIG_STRING_LOCATION 4
#define EPDF_SIG_STRING_CONTACT_INFO 5
#define EPDF_SIG_STRING_M 6

// /Action of a FieldMDP transform or a /Lock dictionary.
#define EPDF_SIG_FIELD_ACTION_NONE 0
#define EPDF_SIG_FIELD_ACTION_ALL 1
#define EPDF_SIG_FIELD_ACTION_INCLUDE 2
#define EPDF_SIG_FIELD_ACTION_EXCLUDE 3

// Which field-name list EPDFSig_GetFieldNameCount/At read.
#define EPDF_SIG_FIELDS_FIELDMDP 0  // the signature's /Reference FieldMDP
#define EPDF_SIG_FIELDS_LOCK 1      // the field's /Lock

// Seed value (/SV) constraint bits, in the layout of /SV /Ff
// (ISO 32000-2 table 238). EPDFSig_GetSeedValueRequiredFlags() returns /Ff
// (which constraints are REQUIRED); EPDFSig_GetSeedValuePresentFlags()
// returns which constraints are PRESENT in the dictionary at all, so a
// caller can tell "required and unsupported" from "optional".
#define EPDF_SIG_SV_FILTER (1u << 0)
#define EPDF_SIG_SV_SUBFILTER (1u << 1)
#define EPDF_SIG_SV_V (1u << 2)
#define EPDF_SIG_SV_REASONS (1u << 3)
#define EPDF_SIG_SV_LEGAL_ATTESTATION (1u << 4)
#define EPDF_SIG_SV_ADD_REV_INFO (1u << 5)
#define EPDF_SIG_SV_DIGEST_METHOD (1u << 6)
#define EPDF_SIG_SV_LOCK_DOCUMENT (1u << 7)
#define EPDF_SIG_SV_APPEARANCE_FILTER (1u << 8)
// Not an /Ff bit: /SV carries /Cert, /MDP, or /TimeStamp constraints.
#define EPDF_SIG_SV_CERT (1u << 16)
#define EPDF_SIG_SV_MDP (1u << 17)
#define EPDF_SIG_SV_TIMESTAMP (1u << 18)

// Lists for EPDFSig_GetSeedValueListCount/At.
#define EPDF_SIG_SV_LIST_SUBFILTER 0
#define EPDF_SIG_SV_LIST_DIGEST_METHOD 1
#define EPDF_SIG_SV_LIST_REASONS 2

// Experimental EmbedPDF Extension API.
// Build the model. Returns NULL for a NULL document. Close with
// EPDFSig_CloseModel().
FPDF_EXPORT EPDF_SIGNATURE_MODEL FPDF_CALLCONV
EPDFSig_LoadModel(FPDF_DOCUMENT document);

FPDF_EXPORT void FPDF_CALLCONV EPDFSig_CloseModel(EPDF_SIGNATURE_MODEL model);

// Experimental EmbedPDF Extension API.
// Number of signature fields (signed or unsigned), in reconciled form order.
// -1 for a NULL model.
FPDF_EXPORT int FPDF_CALLCONV EPDFSig_Count(EPDF_SIGNATURE_MODEL model);

// Experimental EmbedPDF Extension API.
// Whether the document's revision chain was valid when the model was built
// (see EPDFDoc_GetRevisionCount). When FALSE, every coverage is PARTIAL or
// MALFORMED and every revision index is -1.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_IsRevisionChainValid(EPDF_SIGNATURE_MODEL model);

// Experimental EmbedPDF Extension API.
// Index of the signature field whose dictionary is |field_objnum|, or -1.
FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetIndexByFieldObjNum(EPDF_SIGNATURE_MODEL model,
                              uint32_t field_objnum);

// Identity. Object numbers are 0 when the dictionary is direct or absent.
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_GetFieldObjNum(EPDF_SIGNATURE_MODEL model, int index);
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_GetWidgetObjNum(EPDF_SIGNATURE_MODEL model, int index);
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_GetWidgetPageObjNum(EPDF_SIGNATURE_MODEL model, int index);
// The /V dictionary's object number, 0 when unsigned or direct.
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_GetValueObjNum(EPDF_SIGNATURE_MODEL model, int index);

// Experimental EmbedPDF Extension API.
// Fully qualified field name (UTF-16LE, two-call protocol: pass NULL to get
// the required byte length including the terminator).
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFSig_GetFieldName(EPDF_SIGNATURE_MODEL model,
                     int index,
                     FPDF_WCHAR* buffer,
                     unsigned long buflen);

// Experimental EmbedPDF Extension API.
// TRUE when the field carries a /V dictionary with a /Contents entry.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFSig_IsSigned(EPDF_SIGNATURE_MODEL model,
                                                   int index);

FPDF_EXPORT int FPDF_CALLCONV EPDFSig_GetKind(EPDF_SIGNATURE_MODEL model,
                                            int index);

// Experimental EmbedPDF Extension API.
// The four /ByteRange integers. FALSE when unsigned or the array is not
// four non-negative integers.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_GetByteRange(EPDF_SIGNATURE_MODEL model,
                     int index,
                     unsigned long long out_range[4]);

// EPDF_SIG_COVERAGE_*; MALFORMED for an unsigned field.
FPDF_EXPORT int FPDF_CALLCONV EPDFSig_GetCoverage(EPDF_SIGNATURE_MODEL model,
                                                int index);

// Index of the revision the signature seals, or -1 unless coverage is
// WHOLE_REVISION.
FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetRevisionIndex(EPDF_SIGNATURE_MODEL model, int index);

// Experimental EmbedPDF Extension API.
// The /Contents object, exactly as long as its encoding declares: a DER
// definite length, or a BER indefinite-length SEQUENCE walked to its own
// end-of-contents octets (nesting included). Trailing bytes in the hex
// string must be zero (padding) or the entry is MALFORMED and this returns
// 0. Two-call protocol on |buffer| / |buflen|.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFSig_GetContents(EPDF_SIGNATURE_MODEL model,
                    int index,
                    void* buffer,
                    unsigned long buflen);

// Experimental EmbedPDF Extension API.
// A /V string or name by EPDF_SIG_STRING_* key, UTF-16LE. Returns 0 when
// the entry is absent.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFSig_GetString(EPDF_SIGNATURE_MODEL model,
                  int index,
                  int key,
                  FPDF_WCHAR* buffer,
                  unsigned long buflen);

// Experimental EmbedPDF Extension API.
// /P of the DocMDP transform on THIS signature's /Reference (1..3; 2 when
// the transform exists without /P), or 0 when there is none.
FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetDocMDPPermission(EPDF_SIGNATURE_MODEL model, int index);

// TRUE when the catalog's /Perms /DocMDP points at this signature's /V.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_IsCatalogCertification(EPDF_SIGNATURE_MODEL model, int index);

// /Action of the FieldMDP transform on this signature (EPDF_SIG_FIELD_ACTION_*).
FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetFieldMDPAction(EPDF_SIGNATURE_MODEL model, int index);

// The field's /Lock: /Action, and /P (PDF 2.0 tightening; 0 when absent).
FPDF_EXPORT int FPDF_CALLCONV EPDFSig_GetLockAction(EPDF_SIGNATURE_MODEL model,
                                                  int index);
FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetLockPermission(EPDF_SIGNATURE_MODEL model, int index);

// Experimental EmbedPDF Extension API.
// Field names listed by the FieldMDP transform or the /Lock dictionary
// (|which| is EPDF_SIG_FIELDS_*). Count is -1 for bad arguments; names are
// UTF-16LE with the two-call protocol.
FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetFieldNameCount(EPDF_SIGNATURE_MODEL model, int index, int which);
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFSig_GetFieldNameAt(EPDF_SIGNATURE_MODEL model,
                       int index,
                       int which,
                       int name_index,
                       FPDF_WCHAR* buffer,
                       unsigned long buflen);

// Seed values (/SV on the field).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_HasSeedValue(EPDF_SIGNATURE_MODEL model, int index);
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_GetSeedValueRequiredFlags(EPDF_SIGNATURE_MODEL model, int index);
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_GetSeedValuePresentFlags(EPDF_SIGNATURE_MODEL model, int index);
// /SV /MDP /P: 1..3 = a certification with that permission is required;
// 0 = an approval signature is required and a certification is refused.
// EPDF_SIG_SV_MDP is present only when /MDP carries a /P in 0..3; an /MDP
// without /P imposes nothing and reads as absent (0, flag clear).
FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetSeedValueMDP(EPDF_SIGNATURE_MODEL model, int index);
// /SV /V, the seed-value parser capability the signer needs (1 = PDF 1.5
// entries, 2 = PDF 1.7 entries); 0 when absent.
FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetSeedValueVersion(EPDF_SIGNATURE_MODEL model, int index);
// /SV /Filter as text; 0 when absent.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFSig_GetSeedValueFilter(EPDF_SIGNATURE_MODEL model,
                           int index,
                           FPDF_WCHAR* buffer,
                           unsigned long buflen);
// /SV /SubFilter, /DigestMethod, or /Reasons entries (EPDF_SIG_SV_LIST_*).
FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetSeedValueListCount(EPDF_SIGNATURE_MODEL model, int index, int list);
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFSig_GetSeedValueListAt(EPDF_SIGNATURE_MODEL model,
                           int index,
                           int list,
                           int item,
                           FPDF_WCHAR* buffer,
                           unsigned long buflen);

// ---------------------------------------------------------------------------
// Object diff between two revisions.
//
// |older| must be a revision of |newer| opened with EPDFDoc_OpenRevision()
// (the call verifies that newer's cross-reference chain passes through the
// section older was closed by, i.e. that they share their byte history).
//
// The touched set is exact and mechanical: every object number whose
// EFFECTIVE cross-reference mapping differs between the two parsers (type,
// file position, generation, or object-stream number + index), every member
// of an object stream whose own mapping differs, every number present in
// older but free or absent in newer, and the trailer. A position past the
// older revision's end is NOT the criterion: an update may redirect a number
// at bytes that already existed, and that is a change.
//
// For each entry both values are available, shallow-serialised in PDF syntax
// (references as "n g R", never followed; streams as "stream(<raw length>,
// <sha256 of raw data>)" followed by their dictionary; dictionary keys
// escaped like the writer does). Usage is reported as REFERRERS: every
// inbound reference to an object in a revision, as the referring object's
// number (0 for the trailer) and the key/index path inside that object:
// keys escaped as PDF names without the slash, array indexes as "[n]",
// joined by "/" ("AcroForm/Fields/[3]", "Annots/[0]", "V", "A#2FB"). The
// referrer index covers every object reachable from the trailer, not only
// touched ones, so a caller can walk any object up to the root:
// "#5 <- #7 via V <- #1 via AcroForm/Fields/[0] <- trailer via Root". Root
// paths are not enumerated by the fork on purpose: PDF back-links (/Parent,
// a widget's /P) make simple-path enumeration combinatorial, while the set
// of inbound references is small and complete. Value evidence is complete
// or flagged truncated; a rule engine must never permit an entry whose
// evidence is truncated.
//
// The diff speaks PDF only. What a change MEANS (form fill, annotation,
// DSS update) is decided by the caller.
// ---------------------------------------------------------------------------

typedef const struct epdf_object_diff_t__* EPDF_OBJECT_DIFF;

#define EPDF_DIFF_ADDED 0
#define EPDF_DIFF_MODIFIED 1
#define EPDF_DIFF_FREED 2

#define EPDF_DIFF_OBJ_DICTIONARY 0
#define EPDF_DIFF_OBJ_STREAM 1
#define EPDF_DIFF_OBJ_ARRAY 2
#define EPDF_DIFF_OBJ_SCALAR 3
#define EPDF_DIFF_OBJ_XREF 4     // a cross-reference stream (container)
#define EPDF_DIFF_OBJ_OBJSTM 5   // an object stream (container)
#define EPDF_DIFF_OBJ_TRAILER 6  // the trailer dictionary (object number 0)

// Which revision EPDFObjectDiff_GetValue / GetPath* describe.
#define EPDF_DIFF_OLD 0
#define EPDF_DIFF_NEW 1

// What reading an entry's object from one revision's bytes came to.
#define EPDF_DIFF_READ_OK 0      // parsed; the value is evidence
#define EPDF_DIFF_READ_FAILED 1  // the mapping is live but the bytes do not parse (or a stream's data cannot be read): no evidence, never "null"
#define EPDF_DIFF_READ_ABSENT 2  // the object does not exist in that revision (added / freed)

// Experimental EmbedPDF Extension API.
// Compare two revisions. Returns NULL when either document is NULL, has no
// parser, or when |older| is not a revision of |newer| (its chain does not
// pass through older's final section, or older's bytes are not the prefix
// of newer's file - both are checked). Close with
// EPDFObjectDiff_Close(). Both documents must stay open while the diff is
// being built; the result is detached afterwards. Values and referrers are
// parsed from each side's loaded bytes, not taken from its object cache: an
// unsaved edit is not part of a revision, and a layer's base-only cache is
// not the layer's bytes.
FPDF_EXPORT EPDF_OBJECT_DIFF FPDF_CALLCONV
EPDFDoc_CompareRevisions(FPDF_DOCUMENT older, FPDF_DOCUMENT newer);

FPDF_EXPORT void FPDF_CALLCONV EPDFObjectDiff_Close(EPDF_OBJECT_DIFF diff);

// Number of entries, ascending by object number, the trailer last. -1 for a
// NULL diff.
FPDF_EXPORT int FPDF_CALLCONV EPDFObjectDiff_GetCount(EPDF_OBJECT_DIFF diff);

// Experimental EmbedPDF Extension API.
// One entry. Generations are -1 when the object is absent in that revision.
// |out_stream_data_changed| is TRUE when both revisions hold a stream and the
// raw data differs. Every out-param may be NULL.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFObjectDiff_GetEntry(EPDF_OBJECT_DIFF diff,
                        int index,
                        unsigned int* out_obj_num,
                        int* out_change,
                        int* out_kind,
                        int* out_old_gen,
                        int* out_new_gen,
                        FPDF_BOOL* out_stream_data_changed);

// Experimental EmbedPDF Extension API.
// The shallow serialisation of the object in revision |which|
// (EPDF_DIFF_OLD / EPDF_DIFF_NEW), NUL-terminated, two-call protocol (the
// returned length includes the terminator). 0 when the object is absent in
// that revision. |out_truncated| is set when the serialisation exceeded the
// cap (1 MiB) and carries no value evidence.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFObjectDiff_GetValue(EPDF_OBJECT_DIFF diff,
                        int index,
                        int which,
                        char* buffer,
                        unsigned long buflen,
                        FPDF_BOOL* out_truncated);

// Experimental EmbedPDF Extension API.
// Inbound references to object |obj_num| in revision |which|, for ANY object
// number (touched or not). -1 for bad arguments; 0 for an object nobody
// reachable references (orphaned) or one absent from that revision.
FPDF_EXPORT int FPDF_CALLCONV
EPDFObjectDiff_GetReferrerCount(EPDF_OBJECT_DIFF diff,
                                int which,
                                unsigned int obj_num);

// Experimental EmbedPDF Extension API.
// One inbound reference: the referring object (|out_parent_obj_num|, 0 = the
// trailer) and the label leading from that object's top level to the
// reference (escaped keys and "[n]" indexes joined by "/"), NUL-terminated,
// two-call protocol.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFObjectDiff_GetReferrer(EPDF_OBJECT_DIFF diff,
                           int which,
                           unsigned int obj_num,
                           int referrer_index,
                           unsigned int* out_parent_obj_num,
                           char* buffer,
                           unsigned long buflen);

// Experimental EmbedPDF Extension API.
// How the entry's object read from revision |which| (EPDF_DIFF_READ_*). A
// present side that did not parse is READ_FAILED, and its value is not
// evidence of anything - in particular not of being equal to the other
// side's. -1 for bad arguments.
FPDF_EXPORT int FPDF_CALLCONV EPDFObjectDiff_GetReadStatus(EPDF_OBJECT_DIFF diff,
                                                           int index,
                                                           int which);

// Experimental EmbedPDF Extension API.
// Facts about revision |which| as a whole that decide whether a validator
// can judge later changes to it at all. |out_sparse_xref|: the document's
// original cross-reference section leaves some object number below the
// highest it lists without an entry of any kind (not even free); holes in
// later update sections and an overstated /Size are tolerated.
// |out_bare_reference_count|: reachable indirect objects whose
// entire body is an indirect reference. Acrobat treats a document with either
// as corrupted as soon as any revision follows the signed one. |out_referrers_
// complete|: FALSE when the reachability walk hit its budget, in which case
// referrer counts for that side are lower bounds. Every out-param may be
// NULL. FALSE for bad arguments.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFObjectDiff_GetRevisionHealth(EPDF_OBJECT_DIFF diff,
                                 int which,
                                 FPDF_BOOL* out_sparse_xref,
                                 unsigned int* out_bare_reference_count,
                                 FPDF_BOOL* out_referrers_complete);

// ---------------------------------------------------------------------------
// Digests.
// ---------------------------------------------------------------------------

// Experimental EmbedPDF Extension API.
// Hash bytes [r0, r0+r1) followed by [r2, r2+r3) of the document's own
// file, read straight through the parser's stream (no copy). |inout_len|
// carries the capacity of |out_digest| in and the digest length out; when
// the capacity is too small the required length is written and FALSE is
// returned. Ranges must be non-negative, non-overlapping, ascending, and
// within the file.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_DigestByteRange(FPDF_DOCUMENT document,
                        const unsigned long long range[4],
                        int algorithm,
                        unsigned char* out_digest,
                        unsigned long* inout_len);

// ---------------------------------------------------------------------------
// Loaded bytes.
//
// The bytes the document was loaded from: a plain document's file, or for a
// layer document (EPDFLayer_OpenLayer) the base file followed by the delta
// it was opened with - exactly what the revision, coverage and digest
// functions above read. Unsaved edits are not part of it.
// ---------------------------------------------------------------------------

// Experimental EmbedPDF Extension API.
// Size of the loaded bytes, or 0 when the document has no parser.
FPDF_EXPORT unsigned long long FPDF_CALLCONV
EPDFDoc_GetLoadedBytesSize(FPDF_DOCUMENT document);

// Experimental EmbedPDF Extension API.
// Size of the base the loaded bytes start with: the whole file for a plain
// document, the base file for a layer (its delta, if any, follows it).
FPDF_EXPORT unsigned long long FPDF_CALLCONV
EPDFDoc_GetBaseBytesSize(FPDF_DOCUMENT document);

// Experimental EmbedPDF Extension API.
// The object numbers of the document's structural anchors, as the
// revision analysis needs them to tell a catalog edit from a form edit:
// the catalog (/Root), the /AcroForm dictionary and the /Pages root.
// Each is 0 when absent or when the dictionary is a direct object. Every
// out-param may be NULL. Returns FALSE when the document has no parser.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_GetStructureObjectNumbers(FPDF_DOCUMENT document,
                                  unsigned int* out_root,
                                  unsigned int* out_acroform,
                                  unsigned int* out_pages);

// Experimental EmbedPDF Extension API.
// Copy |length| loaded bytes starting at |offset| into |buffer|. Returns
// |length| on success, 0 when |buffer| is NULL or the range is not within
// the loaded bytes.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_ReadLoadedBytes(FPDF_DOCUMENT document,
                        unsigned long long offset,
                        void* buffer,
                        unsigned long length);

// ---------------------------------------------------------------------------
// Signing.
//
// Signing never touches the live document. The caller snapshots the
// document it wants to sign into a CANDIDATE (its own bytes, or an
// incremental save of its unsaved state, reopened with FPDF_LoadMemDocument),
// prepares the signature on the candidate, saves the candidate
// incrementally with EPDFSig_SaveCandidateToOwnedBuffer(), seals the buffer
// (EPDFSig_Seal() patches /ByteRange and returns the digest), obtains the
// CMS elsewhere, and writes it in with EPDFSig_WriteContents(). The sealed
// buffer is then a complete signed file; the live document is replaced by
// reopening it. A candidate must be a plain document loaded from bytes,
// never a layer document.
// ---------------------------------------------------------------------------

#define EPDF_SIG_SUBFILTER_ADBE_PKCS7_DETACHED 0
#define EPDF_SIG_SUBFILTER_ETSI_CADES_DETACHED 1
#define EPDF_SIG_SUBFILTER_ETSI_RFC3161 2  // a document timestamp (/DocTimeStamp)

typedef struct EPDF_SIG_PREPARE {
  int subfilter;                // EPDF_SIG_SUBFILTER_*
  int digest;                   // EPDF_DIGEST_SHA256/384/512 (SHA-1 refused)
  unsigned long contents_size;  // bytes reserved for the CMS; hex is 2x
  FPDF_WIDESTRING name;         // /Name, nullable
  FPDF_WIDESTRING reason;       // /Reason, nullable
  FPDF_WIDESTRING location;     // /Location, nullable
  FPDF_WIDESTRING contact_info; // /ContactInfo, nullable
  FPDF_BYTESTRING signing_time; // /M as "D:YYYYMMDDHHmmSSZ"; NULL = now (UTC)
  int docmdp_permission;        // 0 none; 1..3 = certification signature
  int fieldmdp_action;          // EPDF_SIG_FIELD_ACTION_* (NONE = no FieldMDP)
  const FPDF_WIDESTRING* fieldmdp_fields;
  int fieldmdp_field_count;
  int lock_permission;          // /Lock /P to tighten after this signature; 0 none
} EPDF_SIG_PREPARE;

// Experimental EmbedPDF Extension API.
// Prepare the signature value on |field_objnum| of the candidate. Validate-
// then-apply: returns 0 and leaves the document untouched when
//   - the field is not /FT /Sig, is already signed, or is ReadOnly;
//   - an earlier signature's FieldMDP or /Lock locks this field;
//   - |candidate| is a layer whose loaded delta already holds signed bytes
//     (a signature whose /ByteRange reaches past the base): saving a layer
//     appends to its BASE and rewrites the layer's objects, which would
//     drop those bytes. Seal them into a new base first (the completion
//     flow), then sign again on a layer over that base;
//   - the field carries a /Lock and the request's fieldmdp_action, fields,
//     or lock_permission disagree with it (leave them at NONE / 0 and the
//     lock is applied as is, as ISO 32000 requires);
//   - the permission in force is 1 (only a document timestamp may follow);
//   - a certification is requested (docmdp_permission, or forced by
//     /SV /MDP) but a signed revision or /Perms /DocMDP already exists, or
//     the request is a document timestamp;
//   - /SV requires a Filter, SubFilter, DigestMethod, or Reason the request
//     does not satisfy, requires a parser capability (/V) newer than the
//     PDF 1.7 entry set this engine implements, asks for an approval
//     signature (/MDP /P 0) while a certification is requested, or
//     requires a constraint this engine does not implement
//     (LegalAttestation, AddRevInfo, LockDocument, AppearanceFilter, a
//     required /Cert or /TimeStamp): required and unsupported is a refusal,
//     never a silent skip;
//   - |opts| is inconsistent (SHA-1, a timestamp with MDP options, a
//     contents_size outside [256, 4 MiB]).
// On success the /V dictionary exists as an indirect object holding /Type,
// /Filter /Adobe.PPKLite, /SubFilter, a sentinel /ByteRange, a zero-filled
// hex /Contents, /M and the strings, and /Reference (DocMDP and/or
// FieldMDP) - all direct objects, as a byte-range signature requires; the
// field holds /V and, when the request introduces a FieldMDP or
// lock_permission, a mirroring /Lock; every field the FieldMDP locks (a
// listed name covers its descendants) gets Ff ReadOnly on top of its
// inherited flags (what Acrobat writes, a permitted change in the signing
// revision); a
// certification points /Root /Perms /DocMDP at /V; /AcroForm /SigFlags gains
// SignaturesExist | AppendOnly. Returns the /V object number.
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_Prepare(FPDF_DOCUMENT candidate,
                uint32_t field_objnum,
                const EPDF_SIG_PREPARE* opts);

// Experimental EmbedPDF Extension API.
// Authoring time, before any signature: write /Lock on an unsigned
// signature field (/Action, /Fields for Include/Exclude, /P when
// |permission| is 1..3). Action NONE removes the lock. Refused once any
// signature field in the document is signed: a lock written after signing
// changes a field dictionary in a way no earlier signature permits, and
// strict validators (pyHanko among them) reject the revision for it.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_SetFieldLock(FPDF_DOCUMENT document,
                     uint32_t field_objnum,
                     int action,
                     const FPDF_WIDESTRING* fields,
                     int field_count,
                     int permission);

// Experimental EmbedPDF Extension API.
// Incremental save of the candidate into an owned buffer (release with
// EPDF_FreeBuffer()), reporting where the serializer wrote object
// |sig_objnum| (the /V dictionary from EPDFSig_Prepare) so the placeholders
// can be located inside that span, never by searching the file. Returns
// NULL when the save fails or the object was not written.
FPDF_EXPORT void* FPDF_CALLCONV
EPDFSig_SaveCandidateToOwnedBuffer(FPDF_DOCUMENT candidate,
                                   uint32_t sig_objnum,
                                   unsigned long long* out_size,
                                   unsigned long long* out_obj_offset,
                                   unsigned long long* out_obj_len);

// Experimental EmbedPDF Extension API.
// EPDFSig_SaveCandidateToOwnedBuffer() through |file_write|: the base bytes
// stream through the writer (never held in memory), the candidate's
// revision follows. |out_size| is the total written; the object span is
// reported as for the buffer variant. For file-backed signing: the caller
// then seals the span in place with EPDFSig_SealSpan() and hashes the
// file with EPDFSig_DigestFileRange().
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_SaveCandidate(FPDF_DOCUMENT candidate,
                      uint32_t sig_objnum,
                      FPDF_FILEWRITE* file_write,
                      unsigned long long* out_size,
                      unsigned long long* out_obj_offset,
                      unsigned long long* out_obj_len);

// Experimental EmbedPDF Extension API.
// The patching half of EPDFSig_Seal() on the object span alone: |span| holds
// the |span_len| bytes of the signature value object that starts at file
// offset |obj_offset| in a file of |file_length| bytes. Patches /ByteRange
// in place and reports the range and the /Contents position (absolute file
// offsets); hashes nothing. The caller writes the span back where it came
// from — its length never changes — and digests with
// EPDFSig_DigestFileRange().
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_SealSpan(unsigned char* span,
                 unsigned long long span_len,
                 unsigned long long obj_offset,
                 unsigned long long file_length,
                 unsigned long long out_range[4],
                 unsigned long long* out_contents_offset,
                 unsigned long long* out_contents_hex_len);

// Experimental EmbedPDF Extension API.
// EPDFSig_DigestByteRange() over a file access instead of a document: the
// two ranges are streamed through the digest. Same |inout_len| protocol.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_DigestFileRange(FPDF_FILEACCESS* file,
                        const unsigned long long range[4],
                        int algorithm,
                        unsigned char* out_digest,
                        unsigned long* inout_len);

// Experimental EmbedPDF Extension API.
// Locate the sentinel /ByteRange and the zero-filled /Contents inside
// [obj_offset, obj_offset + obj_len) of |buffer|, patch the real ByteRange
// in place (left-aligned, space-padded: the file length never changes),
// and hash the two ranges. Fails unless the span holds exactly the
// placeholders EPDFSig_Prepare wrote. |inout_len| follows the
// EPDFSig_DigestByteRange protocol.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_Seal(unsigned char* buffer,
             unsigned long long length,
             unsigned long long obj_offset,
             unsigned long long obj_len,
             int algorithm,
             unsigned long long out_range[4],
             unsigned long long* out_contents_offset,
             unsigned long long* out_contents_hex_len,
             unsigned char* out_digest,
             unsigned long* inout_len);

// Experimental EmbedPDF Extension API.
// Hex-encode |der| into the /Contents placeholder; the remainder stays '0'
// (zero padding, ISO 32000-2 12.8.1). |der| must be one complete DER object
// whose declared length equals |der_len|, and 2 * der_len must fit.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_WriteContents(unsigned char* buffer,
                      unsigned long long length,
                      unsigned long long contents_offset,
                      unsigned long long contents_hex_len,
                      const unsigned char* der,
                      unsigned long der_len);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_SIGNATURE_H_
