// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef TESTING_EMBEDPDF_LAYER_MODE_H_
#define TESTING_EMBEDPDF_LAYER_MODE_H_

#include <stddef.h>

#include <memory>
#include <type_traits>

#include "public/fpdfview.h"

namespace embedpdf_test {

// Layer mode runs the embedder tests through EmbedPDF's layer documents
// (docs/embedpdf/fork-ledger.md, rule layer-mode). With
// EPDF_EMBEDDER_LAYER_MODE=1 set, every document a test opens from a file is a
// fresh layer over a frozen base. The base is never written, so a write that
// bypasses the layer's copy-up door trips DCHECK_PDF_WRITABLE. Like the
// engine, layer mode never drives the form-fill environment: the harness
// still creates one, but sends it no page events and doesn't draw with it.
bool IsLayerModeEnabled();

// FPDF_LoadCustomDocument(), or in layer mode a fresh layer over a base loaded
// from `file_access`. The layer keeps its base alive; `file_access` must
// outlive both, as it must outlive a plain document.
FPDF_DOCUMENT LoadCustomDocumentForTest(FPDF_FILEACCESS* file_access,
                                        FPDF_BYTESTRING password);

// FPDF_LoadMemDocument64(), or in layer mode a fresh layer over a base loaded
// from `data`: a document a test builds in memory goes through the layer as
// one it opens from a file does. `data` must outlive the document either way.
FPDF_DOCUMENT LoadMemDocumentForTest(const void* data,
                                     size_t size,
                                     FPDF_BYTESTRING password);

// Call just before closing a document a test opened. In layer mode, with
// EPDF_EMBEDDER_LAYER_MODE_REPORT=<file> set, appends `<test> <objects>` to
// that file: how many objects the layer holds. A test that only reads must
// leave 0; run_layer_mode.py compares the counts with the list of tests
// known to write.
void NoteClosingDocument(FPDF_DOCUMENT document);

// Closes a document a test opened itself, NoteClosingDocument() first.
struct TestDocumentCloser {
  void operator()(FPDF_DOCUMENT document) const;
};
using ScopedTestDocument =
    std::unique_ptr<std::remove_pointer_t<FPDF_DOCUMENT>, TestDocumentCloser>;

}  // namespace embedpdf_test

#endif  // TESTING_EMBEDPDF_LAYER_MODE_H_
