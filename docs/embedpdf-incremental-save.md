# Incremental save invariants

An incremental save preserves the original bytes and appends a revision. A
layer delta contains that revision alone, with offsets relative to the same
original base. Saving does not advance the live document to the new revision;
repeated saves describe its current state against the input it was opened on.

## Output contract and tests

| Invariant | Enforcement | Regression coverage |
| --- | --- | --- |
| A full rewrite writes only objects reachable from its output trailer, plus any current structural xref object. | `CPDF_SaveTrailer`, `WriteFullDocument` | `FullRewriteWritesOnlyReachableObjects`, `FullRewriteDropsReplacedInfoAndKeepsCustomRoots` |
| Written values are the effective live values. | `CPDF_SaveObjectReader` | `FullRewritePreservesLiveAnnotationAcrossSaves`, `CachedFileReferencesDoNotOverrideLayerEdits` |
| Headers, xrefs, raw reference tokens and encryption use one generation policy. | `CPDF_WriteContext` | `SavePreservesConsistentObjectGenerations`, `EncryptedSaveUsesObjectGenerationForStringsAndStreams` |
| An incremental revision writes exactly the reachable differences from the base; saving does not advance that baseline. | `PrepareIncrementalObjects` | `IncrementalRevisionWritesExactlyChangedObjects`, `LayerRevisionKeepsBaseAndLoadedBaselinesSeparate` |
| Removing or replacing security drops the obsolete encryption dictionary. | `CPDF_SaveTrailer` | `RemoveSecurityLeavesNoEncryptionDictionary`, `ReplacingEncryptionDropsOldDictionary` |
| Saving does not modify live PDF values, object counts, decoded-stream caches, promoted counts or layer epochs. | Save-local reader and explicit write context | Existing cache/sibling tests, `DetachedObjectDoesNotInflateWarmSaveReads` |
| Cached file-reference rows avoid repeated reachability parsing within a fixed memory budget. | `CPDF_ReferenceIndex` | `ReferenceIndexMatchesFileEdges`, `CPDFReferenceIndexTest.*`, detached/sibling tests |
| Independent readers accept the saved output without recovery or warnings. | qpdf through pikepdf plus raw token checks | `check_saved_pdfs.py`, eight checker tests including deliberately invalid outputs |

The trailer plan is built once after the security and ID decisions. It retains
all copied semantic entries, including custom trailer references, and the
explicit root, info and encryption identities. Both walks use its roots; the
writer uses its entries. An inline encryption dictionary gets one allocated
number shared by the object writer and trailer writer. The original xref stream
is not a semantic root.

Incremental output retains the original bytes. Old orphan objects and old xref
structures in that prefix are permitted; only newly written objects are checked
for reachability. Full rewrites have no such inherited prefix.

## Object identity

An indirect object's identity is its object number and generation together.
Incremental output preserves existing generations. New objects and objects
originally stored in object streams use generation zero. Full rewrites retain
the existing policy of normalizing all generations to zero.

`CPDF_Creator` supplies an explicit `CPDF_WriteContext` to serialization. The
same generation policy drives object headers, nested references, trailer
references, cross-reference entries, and per-object encryption. Serializing a
reference reads identity metadata without parsing its target. Live objects are
never renumbered or assigned different generations to prepare output.

Cross-reference streams use two generation bytes, include their own entry, and
derive `/Length` from the entries actually written. Incremental `/Size` retains
at least the previous trailer's value.

## Choosing objects to write

1. Compare only objects already in the plain document or layer overlay. Read
   original values with the save's temporary object reader and bounded
   object-stream cache. Do not load comparison objects into the document.
2. If no candidate differs, skip reachability traversal and the empty revision.
3. Starting at references retained in the output trailer, establish which
   candidates remain reachable. Prefer already-cached objects at each step;
   editing normally loads the relevant path through the page tree.
4. Stop once every changed candidate has been found. Write reachable changes
   in object-number order.

Cached objects are only a traversal priority, never evidence of reachability.
Detached annotations and other orphans remain excluded. Proving that a candidate
is unreachable can still require a complete graph traversal. That traversal
uses temporary parsing and a number-only worklist rather than retaining a
second live object graph.

## Bounded reference index

An unreachable changed candidate can require a complete traversal on every save.
The parser owns a memo of outgoing object numbers from immutable file objects.
Live or promoted objects always take precedence, and their references are never
stored in the shared index. Sibling layers therefore share only original-file
facts. A failed parse is not cached as an empty row.

Rows use sparse pages of 4,096 four-byte entries. Nonempty reference lists are
packed into pages of 16,384 words; there is no allocation per object. The default
32 MiB accounting limit includes page storage and a conservative 128-byte
allowance per page for container/allocation overhead. Missing rows, rows with
16,384 or more references, and insertions exceeding the budget fall back to
ordinary parsing. Existing entries remain valid; there is no eviction churn.
The index is cleared when the source parser is initialized or its xref is rebuilt.
Progressively loaded documents bypass it because their xref can still evolve.

This memo is the one intentional persistent cache populated by incremental
reachability. It contains no live PDF objects and does not change document or
layer semantics. It is not the parser's decoded object-stream cache.

The first complete traversal still has to parse the graph and build the index.
Later traversals still visit edges, and the comparison phase may still read base
twins. Consequently this is not a guarantee of zero file reads or constant-time
saves for every document. Measure cold and warm times, original-file reads and
peak memory separately; budget exhaustion must affect speed only, never output.

## Layer isolation and undo

A layer has two comparison baselines:

- Its frozen base determines the contents of a cumulative replacement delta.
- Its loaded delta, falling back to the base, determines whether anything has
  changed since opening the layer.

For example, base `10`, loaded delta `20`, current value `10` is a real edit
with an empty replacement delta. Conversely, keeping `20` is unchanged since
load even though a cumulative delta still needs to carry that override.

Comparison contexts account for reference generations as well as values. A
referring array must remain in a delta when its referenced object changes
generation, even when the array still contains the same object number.

Save-time parsing and `/Info` identity lookup do not promote objects or populate
the shared base cache. Existing live handles remain valid. Regression tests
save one layer repeatedly while checking its frozen base and untouched sibling.

## Verification

Run the native suites using `scripts/embedpdf-runtime/test-target.sh`. The
`FPDFSaveEmbedderTest` cases cover generations 0, 1, 256, and 65534; tables and
streams; compressed-to-normal object entries; RC4, AES-128, and AES-256; cache
isolation; cross-reference stream self entries; and retained `/Size`.

Existing layer, annotation, security, and signature suites cover reopening,
undo, detached objects, metadata, encryption changes, and signed revisions.
Independent readers should validate output directly, without first repairing
or rewriting it. Preserve the original byte prefix when checking incremental
output.

For a private large fixture, compare native counting sinks with the same edit:

```sh
python3 testing/tools/benchmark_incremental_save.py \
  out/embedpdf-runtime/darwin-arm64/libembedpdf.dylib \
  /path/to/input.pdf --repeat 3
```

The script also works with stock PDFium's native library. It loads from a file,
adds one square annotation, and reports the save-call duration, output size,
and process memory high-water mark observed after each save. Loading, editing,
rendering, and document cleanup are outside the timed call. It uses Python's
POSIX `resource` module.

Use `--output /path/to/new-output.pdf` for a separate correctness run. The first
save then writes to a file; its timing includes callback writes but excludes
the final file close. Existing files are never overwritten. Owned-buffer,
WASM, and viewer timings include different allocation and transfer costs and
must be reported separately from the native counting benchmark.

### Independent checker

Install `testing/tools/requirements-save-check.txt` into a virtual environment
and set `EPDF_SAVE_CHECK_PYTHON` to that environment's Python. `test-target.sh`
then creates a fresh dump directory, runs the checker self-tests, and validates
the save-model outputs after the embedder suite. CI requires the dependency;
local runs without it explicitly report a skip. A deliberate gtest filter that
selects no save-model tests does not require dumped outputs.

The checker uses `attempt_recovery=False` and `check_pdf_syntax()`, disables
page-attribute propagation, and reads trailer metadata without creating it. It
checks raw headers and reference tokens against qpdf's xref table. Literal/hex
strings and stream payloads are not interpreted as references. Its generation
checks are intended for the well-formed regression fixtures; they are not a
universal validator for PDF's permitted null/dangling-reference semantics.

`EPDF_SAVE_DUMP_DIR` can also be set for direct binary runs. Each dumped PDF has
an original-prefix-length `.size` sidecar and, for encrypted test fixtures, a
`.pw` sidecar with the test password. These are test artifacts only.

Use `benchmark_incremental_save.py --detached` to leave a generated annotation
appearance unreachable before saving. Compare it with the default edit-only
case in separate processes, with builds and other CPU-heavy checks stopped.

## Validation results (2026-09-19)

The native macOS arm64 build passed 1,070 unit tests and 1,173 embedder tests.
The optional Adobe fixture generator was the only skipped embedder test. The
independent checker passed its eight self-tests and validated 100 generated
PDFs: 49 native regression outputs, 24 native cross-runtime fixtures, 24 WASM
fixtures and three WASM layer cases. These include classic, stream and hybrid
xrefs, nonzero generations, encrypted files and compressed-object edits.

The private 478-page, 55,416,668-byte fixture was benchmarked with the native
counting sink. Times below measure the save call, not loading or editing. Warm
values are ranges from successive saves in the same process; these are local
measurements, not latency guarantees.

| Scenario | Before reference index | With reference index |
| --- | --- | --- |
| Ordinary edit, warm | About 25 ms | 23.72–24.88 ms |
| Detached appearance, first save | 8,293.56 ms | 7,608.83 ms |
| Detached appearance, warm | 7,847.72–7,881.42 ms | 87.07–88.06 ms |
| Detached appearance, peak process memory | 192.03 MiB | 209.20 MiB |

Ordinary saves peaked at 181.39 MiB. Both final scenarios produced 55,485,443
bytes, so the cached metadata did not increase the PDF size. The first complete
traversal remains expensive; subsequent saves trade retained, bounded reference
metadata for avoiding repeated file parsing.

A separate two-layer run measured 136–138 ms for warm saves with a detached
appearance and zero reads from the original file. Its untouched sibling retained
zero annotations. Both ordinary and detached layer outputs preserved every byte
of the original prefix and reopened with all 478 pages and the expected single
annotation. Whole-process memory for this layer harness includes loading and
editing two layers and is not comparable to the counting-sink figures above.
