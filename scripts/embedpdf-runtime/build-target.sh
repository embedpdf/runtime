#!/usr/bin/env bash
# Copyright 2026 CloudPDF LTD
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SOURCE_DIR="${PDF_RUNTIME_SOURCE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
TARGET="${1:-}"
PDF_IS_COMPLETE_LIB=true
# EmbedPDF: thread-confined runtime. Off by default; enabled per-target below
# for the native server builds. wasm isolates globals per-instance, so it stays
# off there.
EMBEDPDF_TLS_GLOBALS=false

if [[ -z "$TARGET" ]]; then
  echo "usage: $0 <target>" >&2
  exit 1
fi

case "$TARGET" in
  wasm32)
    GN_TARGET_OS="emscripten"
    GN_TARGET_CPU="wasm"
    EXTRA_ARGS=$'\nis_clang=false\nuse_custom_libcxx=false'
    # build/ reads the macOS SDK whenever the host is a Mac (gn_logs.gni), but
    # PDFium's build_overrides/build.gni decides use_system_xcode only for mac
    # and iOS targets.
    if [[ "$(uname -s)" == "Darwin" ]]; then
      EXTRA_ARGS+=$'\nuse_system_xcode=true'
    fi
    ;;
  darwin-arm64)
    GN_TARGET_OS="mac"
    GN_TARGET_CPU="arm64"
    PDF_IS_COMPLETE_LIB=false
    ;;
  darwin-x64)
    GN_TARGET_OS="mac"
    GN_TARGET_CPU="x64"
    PDF_IS_COMPLETE_LIB=false
    ;;
  linux-x64)
    GN_TARGET_OS="linux"
    GN_TARGET_CPU="x64"
    PDF_IS_COMPLETE_LIB=false
    ;;
  linux-arm64)
    GN_TARGET_OS="linux"
    GN_TARGET_CPU="arm64"
    PDF_IS_COMPLETE_LIB=false
    EXTRA_ARGS=$'\narm_control_flow_integrity="none"'
    ;;
  linuxmusl-x64)
    GN_TARGET_OS="linux"
    GN_TARGET_CPU="x64"
    EXTRA_ARGS=$'\nis_musl=true\nis_clang=false\nuse_sysroot=false\nuse_custom_libcxx=false\nuse_custom_libcxx_for_host=false\nuse_glib=false'
    ;;
  linuxmusl-arm64)
    GN_TARGET_OS="linux"
    GN_TARGET_CPU="arm64"
    # No libyuv: its arm64 sources use dot-product, SVE2 and SME instructions,
    # and the musl GCC toolchain's assembler rejects them. PDFium keeps its own
    # loops for pdf_use_libyuv=false.
    EXTRA_ARGS=$'\nis_musl=true\nis_clang=false\nuse_sysroot=false\nuse_custom_libcxx=false\nuse_custom_libcxx_for_host=false\nuse_glib=false\npdf_use_libyuv=false'
    ;;
  win32-x64)
    GN_TARGET_OS="win"
    GN_TARGET_CPU="x64"
    PDF_IS_COMPLETE_LIB=false
    ;;
  win32-arm64)
    GN_TARGET_OS="win"
    GN_TARGET_CPU="arm64"
    PDF_IS_COMPLETE_LIB=false
    ;;
  *)
    echo "unknown target: $TARGET" >&2
    exit 1
    ;;
esac

# EmbedPDF: enable per-thread PDFium globals for the native server targets so
# the server worker pool can render in parallel in-process. wasm stays off
# (each instance already isolates globals via its own linear memory).
if [[ "$TARGET" != "wasm32" ]]; then
  EMBEDPDF_TLS_GLOBALS=true
fi

PDF_RUNTIME_TARGET_OS_LIST="${PDF_RUNTIME_TARGET_OS_LIST:-$GN_TARGET_OS}" \
  "$SOURCE_DIR/scripts/embedpdf-runtime/ensure-deps.sh"

"$SOURCE_DIR/scripts/embedpdf-runtime/apply-patches.sh" "$TARGET"

if [[ "$TARGET" == linux-* ]]; then
  (
    cd "$SOURCE_DIR"
    build/install-build-deps.sh --no-prompt
    build/linux/sysroot_scripts/install-sysroot.py "--arch=$GN_TARGET_CPU"
  )
fi

OUT="$SOURCE_DIR/out/embedpdf-runtime/$TARGET"
mkdir -p "$OUT"

cat > "$OUT/args.gn" <<EOF
is_debug=false
treat_warnings_as_errors=false
pdf_use_skia=false
pdf_enable_xfa=false
pdf_enable_v8=false
is_component_build=false
clang_use_chrome_plugins=false
pdf_is_standalone=true
use_debug_fission=false
pdf_is_complete_lib=$PDF_IS_COMPLETE_LIB
pdf_use_partition_alloc=false
embedpdf_thread_local_globals=$EMBEDPDF_TLS_GLOBALS
symbol_level=0
target_os="$GN_TARGET_OS"
target_cpu="$GN_TARGET_CPU"${EXTRA_ARGS:-}
EOF

(
  cd "$SOURCE_DIR"
  gn gen "$OUT"
  ninja -C "$OUT" pdfium
)

echo "$OUT"
