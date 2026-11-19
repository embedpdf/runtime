// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// EmbedPDF: thread-confined runtime lifecycle.
//
// EPDF_InitThread / EPDF_ShutdownThread make the per-thread PDFium lifecycle
// explicit for callers that run PDFium on worker threads. With
// embedpdf_thread_local_globals enabled (see core/fxcrt/epdf_tls.h) each worker
// thread owns its own PDFium globals, so every such thread must initialize and
// tear down PDFium itself. With the flag disabled they start and tear down the
// process's one PDFium, as FPDF_InitLibrary / FPDF_DestroyLibrary do.
//
// The calls are counted per thread. Callers that share a thread share its
// PDFium (two engines in one Node thread, each over the same native library),
// so a caller that leaves must not tear it down under the ones still there.
//
// These are intentionally exported for ALL targets (including wasm, where they
// simply wrap the normal init/destroy) so shared runtime code can call a single
// lifecycle entry point regardless of platform.

#include "public/epdf_view.h"

#include "core/fxcrt/epdf_tls.h"
#include "public/fpdfview.h"

namespace {

// How many EPDF_InitThread() calls on this thread still wait for their
// EPDF_ShutdownThread().
EPDF_TLS int g_thread_users = 0;

}  // namespace

FPDF_EXPORT void FPDF_CALLCONV EPDF_InitThread() {
  // Routes through FPDF_InitLibrary so the standard config/init path runs for
  // the calling thread. It does nothing on a thread that is already started.
  FPDF_InitLibrary();
  ++g_thread_users;
}

FPDF_EXPORT void FPDF_CALLCONV EPDF_ShutdownThread() {
  // Lifecycle-strict: a caller must have already closed every PDFium handle
  // it created on this thread before invoking this.
  if (g_thread_users > 1) {
    // Others still use this thread's PDFium: the last one out tears it down.
    --g_thread_users;
    return;
  }
  // The last one out, or a call with no EPDF_InitThread() before it, which
  // tears down as FPDF_DestroyLibrary() does.
  g_thread_users = 0;
  FPDF_DestroyLibrary();
}
