# EmbedPDF fork ledger

This fork changes PDFium in ways upstream doesn't know about. This file lists those changes as
rules: what must stay true, why, where it lives, what guards it, and what to do when upstream
changes the same code.

Every change we make inside an upstream file names its rule, in a comment inside or just above the
change:

```cpp
// EmbedPDF(layer-door): <why this line is here>
```

- `scripts/embedpdf-runtime/check-fork-tags.py` lists changes that don't name a rule, or name one
  this file doesn't have.
- Files that exist only in the fork (`epdf_*`, `cpdf_layer_*`, ...) are ours as a whole and carry
  no tags.
- A merge conflict shows code, not commit messages. When a conflict block carries a tag, read its
  rule here before resolving it, and keep the rule while taking upstream's change.
- Rules change only with review. A sync never edits this file on its own.

Status: **draft** (Oct 12 2026), written from the fork's commits, its `EmbedPDF:` comments and the
platform plans. Rules come first; their "Guarded by" lines will grow as the safety net of the
upstream-sync plan lands (layer mode, the globals inventory).

---

## Layer and transactions

### layer-door
**Rule.** On a layer document, a write to an existing indirect object goes through
`GetMutableIndirectObject`, which first copies a base object up into the layer (`PromoteFromBase`,
`CloneForHolder`). A mutable handle copies on its first actual write, not when it is taken.
**Why.** Base objects are frozen and shared by every session on the same file. A write to one
changes every session's document and every later save.
**Where.** `CPDF_LayerDocument::GetMutableIndirectObject`, `PromoteFromBase`, the
`CloneForHolder` family on the object classes, stream bytes shared through `CFX_SharedBytes`.
**Guarded by.** `cpdf_layer_document_unittest.cpp`, `cpdf_layer_transaction_unittest.cpp`;
layer mode (`testing/tools/run_layer_mode.py`), which runs upstream's tests over a frozen base.
**When upstream touches this.** New code that writes an object it didn't create or get from the
door: route it through the door, or open its owner. A new `Clone` variant needs a `CloneForHolder`
twin.

### write-generation
**Rule.** Every stored object carries the generation that may write it
(`CPDF_Object::write_generation_`), and every mutator asserts `DCHECK_PDF_WRITABLE`. A frozen
base object is never writable; inside a transaction only that transaction's objects are.
**Why.** It catches writes that bypass the door, in debug builds and in CI.
**Where.** `cpdf_write_generation.{h,cpp}`, `CPDF_Object::IsWritable`, the `DCHECK_PDF_WRITABLE`
lines in the mutators of every object class.
**Guarded by.** The `EXPECT_DEATH(..., "IsWritable")` cases in
`cpdf_layer_transaction_unittest.cpp`.
**When upstream touches this.** A new mutator on an object class gets `DCHECK_PDF_WRITABLE(this)`.
Never drop one while resolving a conflict: nothing else notices.

### layer-transaction
**Rule.** A layer edit runs inside a transaction. Reads look down (transaction, layer, base),
writes land in the transaction, commit moves versions down and abort drops them. Saving inside a
transaction is refused.
**Why.** An engine action is all or nothing, without copying or undoing anything.
**Where.** `cpdf_layer_transaction.h`, `CPDF_LayerDocument`, `EPDFLayer_BeginTransaction` and
friends, `ForgetDerivedDataOf`.
**Guarded by.** `cpdf_layer_transaction_unittest.cpp`, `epdf_transaction_embeddertest.cpp`.
**When upstream touches this.** A new cache of derived data (fonts, colour spaces, images) must
forget dropped versions at commit and abort.

### layer-seam
**Rule.** Upstream methods we made `virtual` or `protected` so `CPDF_LayerDocument` and
`CPDF_BaseDocument` can override them: the document's root, info and page dictionary getters, its
page list storage, the indirect object holder, the document's page and render data.
**Why.** The layer steps in at these seams instead of changing every caller.
**Where.** `cpdf_document.{h,cpp}`, `cpdf_indirect_object_holder.h`, `cpdf_docpagedata.{h,cpp}`,
`cpdf_docrenderdata.{h,cpp}`.
**Guarded by.** The build: an `override` fails when its seam disappears. The layer tests.
**When upstream touches this.** Keep the seam when upstream renames or splits a method. A new
upstream accessor to the same state needs the same seam.

### view-scope
**Rule.** A base document resolves references through the current `CPDF_DocumentViewScope`: the
effective view (the base as a layer sees it) or the frozen base. Read entry points open a scope.
**Why.** One frozen base serves many layers, and a reference must resolve to the version this
layer sees.
**Where.** `cpdf_document_view_scope.{h,cpp}`, `CPDF_BaseDocument`, read entry points in
`fpdfsdk/`. Every function in `epdf_annot.cpp` opens one before it walks the document's objects.
**Guarded by.** `cpdf_base_document_unittest.cpp`, `cpdf_layer_document_unittest.cpp`,
`EPDFAnnotEmbedderTest.ReadsResolveThroughTheLayer`.
**When upstream touches this.** A new read entry point that walks a document's objects opens a
scope. Without one, a base page with layer `/Annots` crashes the view detector.

### overlay-epoch
**Rule.** Anything that caches an object pointer, or something derived from one, re-reads it when
the document's `GetOverlayEpoch()` moves: page and form dictionaries, resources, images, annotation
contexts.
**Why.** The layer swaps object versions underneath long-lived holders; a stale pointer reads the
old version.
**Where.** `CPDF_PageObjectHolder`, `CPDF_Page`, `CPDF_Form`, `CPDF_AnnotContext`, `CPDF_Image`,
`CPDF_DocPageData`.
**Guarded by.** `epdf_transaction_embeddertest.cpp`.
**When upstream touches this.** A new field that caches an object pointer needs an epoch check. The
epoch moves only inside a transaction, so a test must open one to reproduce a bug.

### holder-readback
**Rule.** Code that sets a cached holder field (page or form `/Resources`) names the new value
first and never reads it back through a getter that re-resolves.
**Why.** On a layer, `CPDF_Page::GetResources()` re-reads `/Resources` whenever the epoch moved;
reading back right after setting returned null (wasm `unreachable`, native SIGSEGV, Oct 7 2026).
**Where.** `CPDF_PageContentGenerator::RealizeResource`.
**Guarded by.** `EPDFRedactEmbedderTest.AppliesOnAPageWithoutContentsOrResources`.
**When upstream touches this.** New code that sets resources and then calls `GetResources()`.

### reads-dont-write
**Rule.** Read paths (rendering, text, forms, name trees, links, annotation lists, page lookup)
use const accessors, never a mutable getter. A missing appearance is drawn from memory, never
saved.
**Why.** On a layer, a mutable getter promotes objects: the layer grows, the session reports unsaved
edits, and the copy happens outside any transaction. On a frozen base it writes shared state.
**Where.** Const walks in the content parser, `cpdf_interactiveform.cpp`, `cpdf_nametree.cpp`
(`CreateForReading`), `cpdf_linklist.cpp` and `cpdf_annotlist.cpp`; in-memory appearances in
`cpdf_annot.cpp`.
**Guarded by.** `cpdf_read_only_graph_guard_unittest.cpp`,
`EPDFAnnotEmbedderTest.DrawingARedactionMarkWithoutAppearanceWritesNone`; layer mode's list of
tests that write (`testing/embedpdf_layer_mode_writers.txt`): a test that only reads must leave
nothing in a layer.
**When upstream touches this.** A new read path that calls `GetMutable*` or the appearance
generator: switch it to const access.

### annot-identity
**Rule.** An annotation keeps one identity: its object number or, for an inline annotation, its
page and birth index. Promoting a page's inline annotations records their births; a handle is
valid while its annotation is in its page's `/Annots`.
**Why.** The engine addresses annotations across edits, saves and reopening.
**Where.** `CPDF_LayerDocument::PromotePageAnnots`, `CPDF_AnnotContext`, `EPDFLayer_GetBirth*`.
**Guarded by.** `epdf_annot_identity_embeddertest.cpp`, `epdf_annot_raw_embeddertest.cpp`.
**When upstream touches this.** Annotation removal and reordering keep going through our raw paths;
promote before any structural write.

### chosen-object-numbers
**Rule.** A layer edit may write an object at a number the caller chose. A number is never reused,
and the last object number only rises.
**Why.** Optimistic edits name objects before the engine applies them.
**Where.** `CPDF_LayerDocument::RaiseLastObjectNumber`, `CanAddIndirectObjectAt`,
`AddIndirectObjectAt`, the explicit-number APIs in `fpdf_editpage.cpp`.
**Guarded by.** `epdf_object_number_embeddertest.cpp`.
**When upstream touches this.** The object number allocator: keep the free-number check.

### layer-open
**Rule.** A layer opens over a base file plus a saved delta: the parser reads the base bytes
followed by the delta.
**Why.** A session's edits persist as a small delta next to an unchanged original.
**Where.** `CPDF_Parser`, `CPDF_SyntaxParser`, `CPDF_ReadValidator`, delta ingest in
`CPDF_LayerDocument`.
**Guarded by.** `cpdf_layer_document_unittest.cpp`.
**When upstream touches this.** Parser offsets and cross-reference reading.

## Saving

### layer-save
**Rule.** Saving a layer writes only objects changed since load, skips empty revisions, shares
file-backed stream bytes and appends. It never null-replaces base pages, and collects objects
through the layer's view.
**Why.** Deltas stay small and the original file stays byte-identical.
**Where.** `CPDF_Creator` (skip-unchanged, append-only), `object_tree_traversal_util.cpp`
(`kEffectiveDocument`), page deletion in `cpdf_document.cpp`.
**Guarded by.** `cpdf_creator_unittest.cpp`, the independent save checker
(`testing/tools/check_saved_pdfs.py`).
**When upstream touches this.** Incremental save logic in the creator.

### save-generations
**Rule.** Writers carry a `CPDF_WriteContext`. References, cross-reference entries and encryption
keep each object's generation number.
**Why.** Renumbering generations breaks references in incremental saves of files that use non-zero
generations.
**Where.** `CPDF_WriteContext`, `CPDF_Object::WriteTo` and its overrides, `CPDF_Creator`,
`CPDF_Encryptor`.
**Guarded by.** `cpdf_creator_unittest.cpp`, the save checker.
**When upstream touches this.** Any change to a `WriteTo` signature.

### save-traversal
**Rule.** A save picks one trailer and root, walks live objects with a bounded reference index,
and reads objects through `CPDF_SaveObjectReader` and a byte-budgeted object stream cache, without
filling the live document's caches.
**Why.** Full rewrites of large files kept every object parsed in memory, and stale cross-reference
and encryption objects leaked into saves.
**Where.** `cpdf_save_trailer`, `cpdf_reference_index`, `cpdf_save_object_reader`,
`cpdf_object_stream_cache`, `CPDF_Parser::ParseIndirectObjectForSave`.
**Guarded by.** `cpdf_save_trailer_unittest.cpp`, `cpdf_reference_index_unittest.cpp`,
`cpdf_object_stream_cache_unittest.cpp`.
**When upstream touches this.** The creator's traversal and the parser's object stream access.

### save-security
**Rule.** Pending encryption settings belong to the document, not to a global. AES-256 encryption
and password probing are ours. The file key, the `/U` and `/O` salts and every AES IV come from
`EPDF_FillSecureRandom`, the operating system's secure random source, never from `FX_Random` or
`rand()`. When it fails, `EPDF_SetEncryption` or the save fails.
**Why.** Saves on different threads must not share pending security state. `FX_Random` is a
Mersenne Twister with a 32-bit seed (time, pid and a stack address outside Windows): the public
salts give the seed away, and with it a file key, without the password.
**Where.** `CPDF_Document` pending security, `fpdf_save.cpp`, `cpdf_security_handler.cpp`,
`cpdf_crypto_handler.cpp`, `CPDF_Encryptor` and its callers in `cpdf_string.cpp` and
`cpdf_stream.cpp`, `core/fxcrt/epdf_secure_random.{h,cpp}`.
**Guarded by.** Encrypted save embedder tests (`EncryptedSavesShareNoKeyOrSalts`),
`epdf_secure_random_unittest.cpp`; the thread soak's encrypted save.
**When upstream touches this.** The security handler and the save paths. New key, salt or IV bytes
use `EPDF_FillSecureRandom` and fail when it fails. Two upstream draws still use `FX_Random`, and
neither is secret: the `/Perms` filler bytes (encrypted under the file key) and the R≥5 branch of
`OnCreate` (the creator calls `OnCreate` only for R2 and R3).

## Content and pages

### faithful-content-rewrite
**Rule.** Regenerating a page's or form's content writes back what the parser read: colour spaces
by their names, the matrix a form's content starts at, soft-mask matrices, Type3 glyphs, `TJ`
arrays, a form XObject's own resource names.
**Why.** Redaction and editing rewrite content, and anything the writer drops shows up far from the
edit (embedpdf/runtime#58, #59).
**Where.** `CPDF_PageContentGenerator`, `CPDF_PageContentManager`, `CPDF_StreamContentParser`,
`CPDF_ColorState`, `CPDF_Type3Font`.
**Guarded by.** `epdf_content_rewrite_embeddertest.cpp`, `cpdf_contentnames_unittest.cpp`.
**When upstream touches this.** The content generator, and what the parser records about state.

### redaction
**Rule.** Redaction removes text, paths, images and marked content under its regions, and draws
its overlays from one builder.
**Why.** A redaction must remove the content, not hide it.
**Where.** `cpdf_text_redactor`, `cpdf_path_redactor`, `cpdf_redaction_mark_sanitizer`, hooks in
the content generator and parser.
**Guarded by.** `epdf_redact_embeddertest.cpp`, `cpdf_text_redactor_unittest.cpp`,
`cpdf_path_redactor_unittest.cpp`.
**When upstream touches this.** Content generation and text objects.

### page-residency
**Rule.** A parsed page or form records the stream versions it was parsed from and its memory
size. Nested forms parse in their own later stage.
**Why.** The engine keeps pages loaded across edits, and must know when one is stale and what it
costs.
**Where.** `cpdf_contentversions`, `cpdf_parsedsize`, `CPDF_ContentParser`, `CPDF_Form`,
`CPDF_Page`.
**Guarded by.** `epdf_page_residency_embeddertest.cpp`.
**When upstream touches this.** Page and form parsing stages.

### rotation-override
**Rule.** A page can carry a rotation override (`CPDF_Page::SetRotationOverride`) that replaces
`/Rotate` for its size, matrix and rendering. `GetOriginalRotation()` still reads the dictionary,
and the page dictionary is never written.
**Why.** The engine loads pages normalized (rotation 0) to work in unrotated page space, and
reports the page's own rotation separately.
**Where.** `CPDF_Page::GetPageRotation`, `GetOriginalRotation`, `SetRotationOverride`; the callers
are `EPDF_LoadPageNormalized` and the normalized sliced load in `fpdf_view.cpp`.
**Guarded by.** None yet: the sliced-load tests only load with `normalized=false`. A normalized-load
embedder test is missing.
**When upstream touches this.** `GetPageRotation` or `UpdateDimensions`: keep the override check,
and keep `GetOriginalRotation` reading the dictionary.

### sliced-load
**Rule.** Page and form content decode and parse in slices that can pause between steps. A read
API finishes an in-progress load before it returns page data.
**Why.** A long stream must not block a worker, and a half-built page must never be visible.
**Where.** `CPDF_ContentParser`, `CPDF_StreamAcc`, incremental Flate in `flatemodule.cpp`,
`epdf_budget_pause.h`.
**Guarded by.** `epdf_sliced_load_embeddertest.cpp`.
**When upstream touches this.** The content parser's stages and stream decoding.

### sliced-render
**Rule.** A page render can stop between slices and be cancelled.
**Why.** Large renders must not block a worker, and a cancelled render must stop.
**Where.** `CPDF_ProgressiveRenderer`, `CPDF_RenderStatus`, `EPDF_RenderPageBitmapWithMatrix_Start`.
**Guarded by.** The slicing tests in `epdf_view_embeddertest.cpp`.
**When upstream touches this.** The progressive renderer.

### decoded-image-store
**Rule.** Decoded images can outlive a page load in a per-instance store, consulted only where a
page's image cache would decode. Renders stay byte-identical.
**Why.** Heavy images decoded again on every page load cost seconds.
**Where.** `cpdf_decodedimagestore`, `CPDF_PageImageCache`, `CPDF_DIB`.
**Guarded by.** The decoded-image tests in `epdf_view_embeddertest.cpp`.
**When upstream touches this.** The page image cache and image decoding.

### render-speedup
**Rule.** Our render and parse speed-ups keep output byte-identical: reused AGG path storage,
shared path geometry, paths built from batched points while parsing, skipping runs of objects
outside the clip, dedicated stretch loops. Each render speed-up has a `...ForTesting` switch so
tests compare off and on.
**Why.** Vector-heavy pages parse and render much faster.
**Where.** `cfx_agg_devicedriver.cpp`, `cstretchengine.cpp`, `cpdf_pageobjectholder.cpp`,
`cpdf_streamcontentparser.cpp`, `cpdf_progressiverenderer.cpp`, `cpdf_renderstatus.cpp`,
`agg_rasterizer_scanline_aa.h`.
**Guarded by.** The off/on tile comparisons in the embedder tests.
**When upstream touches this.** Rendering code. Re-run the off/on comparisons: a pixel difference
is a bug.

### lean-page-objects
**Rule.** The original bounds and matrix live only on text and image objects, and paths share one
matrix.
**Why.** Memory: a CAD drawing of six million strokes needs 192 MiB less heap.
**Where.** `cpdf_pageobject.h`, `cpdf_pathobject`, `cpdf_textobject`, `cpdf_imageobject`.
**Guarded by.** The rotated-bounds and image-matrix tests.
**When upstream touches this.** Page object fields.

### own-appearance-generator
**Rule.** Our appearance generator is `CPDF_AppearanceGenerator`
(`core/fpdfdoc/cpdf_appearancegenerator.{h,cpp}`, with one
`cpdf_appearancegenerator_<family>.cpp` per family and what they share in
`cpdf_appearancegenerator_internal.h`). None of our code calls upstream's `CPDF_GenerateAP`, and
upstream's `core/fpdfdoc/cpdf_generateap.{h,cpp}` stay byte-identical to upstream, compiled and
unused.
**Why.** We rewrote most of the generator: appearances built in memory, rich text, rotation,
measurement, redaction overlays, form fields. Mixed into upstream's file, it was the worst conflict
file. Apart, upstream's file merges cleanly.
**Where.** `cpdf_appearancegenerator*` and its unit test; the calls in `cpdf_annot.cpp`,
upstream's `FPDFAnnot_AppendObject` and `FPDFAnnot_SetFontColor` in `fpdf_annot.cpp`,
`epdf_annot.cpp`, `epdf_form.cpp`, `epdf_redact.cpp` and the rich text writer.
**Guarded by.** `check-fork-tags.py` fails when upstream's files differ from upstream
(`fork-pristine.txt`); `upstream-preview.py` lists new upstream callers of `CPDF_GenerateAP::`, and
upstream's commits to its generator.
**When upstream touches this.** A commit to `cpdf_generateap.*`: port it to ours or skip it, with
the reason in the sync report. A new upstream caller of `CPDF_GenerateAP::`: route it to ours, or
accept it in the report.

## Text

### text-geometry
**Rule.** Text pages expose each character's geometry (loose and tight boxes, quads, matrix), and
text markup reads oriented quads.
**Why.** Selection and markup on rotated and skewed text.
**Where.** `CPDF_TextPage`, `fpdf_text.cpp`, `EPDFText_GetCharGeometry`, quad reading in
`cpdf_annot.cpp`.
**Guarded by.** `epdf_text_bounds_embeddertest.cpp`, the text embedder tests.
**When upstream touches this.** `CPDF_TextPage` character processing.

### substitute-font-bounds
**Rule.** Glyph bounds of substitute fonts are measured as drawn, including multiple-master design
changes.
**Why.** Text bounds of substituted fonts came out wrong, while renders were right.
**Where.** `cfx_face.cpp`.
**Guarded by.** `epdf_text_bounds_embeddertest.cpp`.
**When upstream touches this.** `CFX_Face` metrics.

## Annotations, forms and features

### annot-appearance
**Rule.** Our appearance model: appearance states, box kinds drawn in a frame they measure, dashes,
note icons, canonical stamp drawings, widget rendering, and the hooks into our appearance
generator.
**Why.** Annotations look the same in every viewer and survive edits.
**Where.** `cpdf_annot.cpp`, `cpdf_annotlist.cpp`, `cpdf_annotcontext.cpp`.
**Guarded by.** `epdf_appearance_state_embeddertest.cpp`, the annotation embedder tests.
**When upstream touches this.** Annotation drawing and appearance lookup.

### forms-model
**Rule.** The form model works without a form-fill environment: `ResolveCurrentDict`,
`ReconcileWidget`, raw values, explicit normalize, export flags, compact XML.
**Why.** The engine reads and writes forms headless, on layers.
**Where.** `cpdf_interactiveform.cpp`, `cpdf_formcontrol.cpp`, `cpdf_formfield.cpp`, `cfx_xml*`.
**Guarded by.** `epdf_form_embeddertest.cpp`.
**When upstream touches this.** `CPDF_InteractiveForm` and field export.

### actions
**Rule.** A detached, read-only action model, JavaScript scanning through const name trees, and
selective flattening.
**Why.** The engine runs actions itself and flattens without touching other annotations.
**Where.** `fpdf_javascript.cpp`, `epdf_action*.cpp`, `epdf_flatten.cpp`.
**Guarded by.** `epdf_action_embeddertest.cpp`.
**When upstream touches this.** JavaScript and action reading.

### attachments
**Rule.** Attachment extraction streams large and compressed files without loading them whole.
**Why.** Attachments can be hundreds of megabytes.
**Where.** `fpdf_attachment.cpp`, `FlateModule::FlateDecodeToSink`.
**Guarded by.** The attachment embedder tests.
**When upstream touches this.** Attachment APIs and Flate decoding.

### rich-text
**Rule.** Rich-text FreeText layout: parsing, line breaking, shaping, and font resolution.
**Why.** FreeText annotations with mixed styles and scripts.
**Where.** `cpdf_richtext*` (ours), line breaking in `fx_linebreak.cpp` and `fgas_linebreak.h`,
`fx_unicode`.
**Guarded by.** `cpdf_richtextlayout_unittest.cpp`, `cpdf_richtextparser_unittest.cpp`.
**When upstream touches this.** Line breaking and Unicode tables.

### fonts
**Rule.** Registered fonts, fallbacks, subsetting, CID-keyed CFF and font embedding policies. A
standard font (the 14, on an ANSI code page) draws with PDFium's built-in face on every platform,
never a system font.
**Why.** Text the user types must render and embed with the right font, and a document must draw
the same in every engine, WebAssembly (which has no system fonts) included.
**Where.** `cfx_fontregistry` (ours), `cpdf_font*`, `cfx_fontmapper.cpp`. Upstream's embedder tests
expect standard fonts from the test fonts, so the embedder test environment turns on
`CFX_FontMapper::SetStandardFontsFromSystemForTesting()`; our tests of the rule turn it off.
**Guarded by.** The font embedder tests; `cfx_fontmapper_unittest.cpp` and
`EPDFViewEmbedderTest.StandardFontsDrawWithBuiltInFaces` for standard fonts.
**When upstream touches this.** Font loading and mapping. A new upstream test that expects the test
fonts for a standard font passes as it is; one of ours that expects the built-in faces turns the
switch off.

### measurement
**Rule.** Measurement dictionaries and dimension appearances.
**Why.** Measure annotations (distance, area, perimeter).
**Where.** `cpdf_measure` (ours), hooks in annotation code.
**Guarded by.** `cpdf_measure_unittest.cpp`, `epdf_measure_embeddertest.cpp`.
**When upstream touches this.** Annotation drawing.

### signatures
**Rule.** Revisions, signature reading and signing support.
**Why.** Digital signatures.
**Where.** `epdf_signature.cpp`, `epdf_revision_*` (ours), hooks in the parser and creator.
**Guarded by.** `epdf_signature_embeddertest.cpp`.
**When upstream touches this.** Parser revision handling and the creator.

### embed-metadata
**Rule.** EmbedPDF metadata stored on annotations and widgets.
**Why.** The engine keeps its own data with an annotation across saves.
**Where.** `cpdf_embed_metadata` (ours), hooks in annotation code.
**Guarded by.** The metadata embedder tests.
**When upstream touches this.** Annotation dictionaries.

### codecs
**Rule.** PNG encoding of page and image bitmaps (`EPDF_PNG_EncodeRGBA`, `public/epdf_png.h`).
**Why.** Thumbnails and exports without a second image library.
**Where.** `pngmodule` (ours), `epdf_png_shim.cpp`.
**Guarded by.** The image export embedder tests.
**When upstream touches this.** `fxcodec`.

## Threads

The contract lives in the embedpdf repo, `docs/conventions/runtime-confinement.md`.

### thread-confined-globals
**Rule.** Every global that PDFium or we own is thread-local (`EPDF_TLS`), atomic, or on the
globals allowlist as a setting written once at start-up.
**Why.** The native server runs PDFium on several threads in one process, each with its own copy of
PDFium's globals.
**Where.** `core/fxcrt/epdf_tls.h`, the `EPDF_TLS` globals, the lcms fixes.
**Guarded by.** The globals inventory (`scripts/embedpdf-runtime/check-globals.py` with
`globals-allowlist.txt`); the ThreadSanitizer soak (`testing/tools/epdf_thread_soak.cpp`).
**When upstream touches this.** A conflict block that contains `EPDF_TLS` is never resolved by
taking upstream's side without putting `EPDF_TLS` back, on the variable renamed or not.

### per-thread-init
**Rule.** Library start-up runs once per thread. A process-wide value it writes must be
thread-local, or identical on every thread and on the allowlist.
**Why.** `EPDF_InitThread` calls `FPDF_InitLibrary` on every worker thread.
**Where.** `fpdf_view.cpp` (`FPDF_InitLibrary`), `epdf_threading.cpp`.
**Guarded by.** The thread soak.
**When upstream touches this.** Anything new in `FPDF_InitLibraryWithConfig` or
`FPDF_DestroyLibrary`.

### no-shared-refcounted-statics
**Rule.** No static `ByteString`, `WideString` or `RetainPtr` that two threads can copy.
**Why.** PDFium's reference counts are plain integers; copying a shared one from two threads is a
race.
**Where.** Everywhere.
**Guarded by.** The thread soak; the globals inventory (`check-globals.py`).
**When upstream touches this.** A new function-local static of those types.

## Tests

### layer-mode
**Rule.** With `EPDF_EMBEDDER_LAYER_MODE=1`, the embedder test harness opens every file as a fresh
layer over a frozen base. Like the engine, it sends the form-fill environment no page events and
doesn't draw with it.
**Why.** Upstream's embedder tests then run through the layer's read and write paths, so a new
upstream feature is tested on a layer without anyone writing a layer test.
**Where.** `testing/embedpdf_layer_mode.{h,cpp}`, four hooks in `testing/embedder_test.cpp`,
`testing/tools/run_layer_mode.py` with `testing/embedpdf_layer_mode_expectations.txt` and
`testing/embedpdf_layer_mode_writers.txt`.
**Guarded by.** `run_layer_mode.py` fails when no test leaves an object in a layer, which is what
happens when a hook stops working.
**When upstream touches this.** `EmbedderTest`'s document opening, page loading and page
rendering: keep the hooks.

## Build

### runtime-exports
**Rule.** `FPDF_EXPORT` exports the public API in every build, not only in component builds.
**Why.** The runtime ships PDFium as one library (native shared libraries and wasm) built without
`COMPONENT_BUILD`, and its `FPDF*` and `EPDF*` functions must stay exported.
**Where.** `public/fpdfview.h`, the `FPDF_EXPORT` block.
**Guarded by.** The runtime build and packaging (`scripts/embedpdf-runtime/`, the
`release-libpdfium.yml` workflow).
**When upstream touches this.** Keep `FPDF_EXPORT` defined to the export attribute outside
component builds; don't bring back the empty `#else` branch.

## Elsewhere

### epdf-api
**Rule.** An `EPDF*` function or declaration placed inside an upstream API file
(`fpdfsdk/fpdf_*.cpp`, `public/fpdf_*.h`), or a core helper that exists only to back one.
**Why.** It was put there when the API was added.
**Where.** The implementations still in `fpdf_view.cpp`, `fpdf_doc.cpp`, `fpdf_attachment.cpp`,
`fpdf_save.cpp` and `fpdf_text.cpp`; the two colour types `public/fpdf_annot.h` adds for
`public/epdf_annot.h`; core helpers such as `CPDF_Dest::GetPageObjectNumber`. Every declaration we
added to a public header now lives in our own: `public/epdf_<name>.h` next to upstream's
`public/fpdf_<name>.h` (`epdf_view.h` and `epdf_layer.h` for `fpdfview.h`), and the annotation API
in `fpdfsdk/epdf_annot.cpp`.
**Guarded by.** The embedder tests of each API. `epdf.CApiTest` (`fpdfsdk/epdf_c_api_test.c`) takes
every `EPDF*` function's address from C, so each header compiles as C and each declaration links;
`check-c-api.py` keeps its list complete.
**When upstream touches this.** Keep ours and take upstream's around it. A new `EPDF*` declaration
goes in our `public/epdf_<name>.h`, never in upstream's header. The implementations left in
`fpdf_*.cpp` move to `epdf_*.cpp` when they conflict (upstream-sync plan §6.1).

### bug-fix
**Rule.** Our fix of an upstream bug. The comment says what the bug was.
**Why.** We needed the fix before upstream had one.
**Where.** Anywhere.
**Guarded by.** The test added with the fix.
**When upstream touches this.** If upstream fixed the same bug, take their fix and drop ours.
