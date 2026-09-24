#!/usr/bin/env bash
# Copyright 2026 CloudPDF LTD
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SOURCE_DIR="${PDF_RUNTIME_SOURCE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
TARGET="${1:-}"
TEST_SUITE="${PDFIUM_TEST_SUITE:-all}"
# EmbedPDF: thread-confined runtime. test-target.sh only builds native host
# targets, which is exactly where we ship the flag on, so default it on here to
# exercise the same variant we ship. Override with EMBEDPDF_TLS_GLOBALS=false to
# build the baseline (process-global) variant for comparison/regression.
EMBEDPDF_TLS_GLOBALS="${EMBEDPDF_TLS_GLOBALS:-true}"

if [[ -z "$TARGET" ]]; then
  echo "usage: $0 <target>" >&2
  exit 1
fi

case "$TEST_SUITE" in
  all|unit|embedder) ;;
  *)
    echo "PDFIUM_TEST_SUITE must be one of: all, unit, embedder" >&2
    exit 1
    ;;
esac

case "$TARGET" in
  darwin-arm64)
    GN_TARGET_OS="mac"
    GN_TARGET_CPU="arm64"
    ;;
  darwin-x64)
    GN_TARGET_OS="mac"
    GN_TARGET_CPU="x64"
    ;;
  linux-x64)
    GN_TARGET_OS="linux"
    GN_TARGET_CPU="x64"
    ;;
  linux-arm64)
    GN_TARGET_OS="linux"
    GN_TARGET_CPU="arm64"
    EXTRA_ARGS=$'\narm_control_flow_integrity="none"'
    ;;
  *)
    echo "test-target.sh only supports host-native targets: darwin-arm64, darwin-x64, linux-x64, linux-arm64" >&2
    exit 1
    ;;
esac

PDF_RUNTIME_TARGET_OS_LIST="${PDF_RUNTIME_TARGET_OS_LIST:-$GN_TARGET_OS}" \
  "$SOURCE_DIR/scripts/embedpdf-runtime/ensure-deps.sh"

"$SOURCE_DIR/scripts/embedpdf-runtime/apply-patches.sh" "$TARGET"

if [[ "$TARGET" == linux-* ]]; then
  (
    cd "$SOURCE_DIR"
    # EmbedPDF: the full install runs apt-get update + install, which costs
    # minutes even when nothing is missing. Skip it when an image installed
    # (and quick-checked) the deps from this exact script, as the monorepo's
    # Docker test image does, or when Chromium's own quick check finds every
    # package installed. The sysroot installer is already stamp-checked.
    if [[ -n "${EMBEDPDF_BUILD_DEPS_INSTALLED_FROM:-}" ]] &&
      cmp -s "$EMBEDPDF_BUILD_DEPS_INSTALLED_FROM" build/install-build-deps.py; then
      echo "Linux build dependencies preinstalled from this install-build-deps.py"
    elif build/install-build-deps.sh --quick-check >/dev/null 2>&1; then
      echo "Linux build dependencies already installed"
    else
      build/install-build-deps.sh --no-prompt
    fi
    build/linux/sysroot_scripts/install-sysroot.py "--arch=$GN_TARGET_CPU"
  )
fi

OUT="$SOURCE_DIR/out/embedpdf-runtime-tests-$TARGET"
RESULTS="$SOURCE_DIR/out/embedpdf-runtime-test-results/$TARGET"
mkdir -p "$OUT" "$RESULTS"

SAVE_CHECK_PYTHON="${EPDF_SAVE_CHECK_PYTHON:-python3}"
SAVE_CHECK_ENABLED=false
if [[ "$TEST_SUITE" != unit ]]; then
  if "$SAVE_CHECK_PYTHON" -c 'import pikepdf' >/dev/null 2>&1; then
    SAVE_CHECK_ENABLED=true
    export EPDF_SAVE_DUMP_DIR="${EPDF_SAVE_DUMP_DIR:-$(mktemp -d "$RESULTS/save-pdfs.XXXXXX")}"
    mkdir -p "$EPDF_SAVE_DUMP_DIR"
  elif [[ "${CI:-}" == true || "${EPDF_REQUIRE_SAVE_CHECK:-0}" == 1 ]]; then
    echo "Install testing/tools/requirements-save-check.txt for the required save checker" >&2
    exit 1
  else
    echo "Independent save checker skipped: pikepdf is not installed"
  fi
fi

cat > "$OUT/args.gn" <<EOF
is_debug=true
treat_warnings_as_errors=false
pdf_use_skia=false
pdf_enable_xfa=false
pdf_enable_v8=false
is_component_build=false
clang_use_chrome_plugins=false
pdf_is_standalone=true
use_debug_fission=false
pdf_is_complete_lib=false
pdf_use_partition_alloc=false
embedpdf_thread_local_globals=$EMBEDPDF_TLS_GLOBALS
symbol_level=1
target_os="$GN_TARGET_OS"
target_cpu="$GN_TARGET_CPU"${EXTRA_ARGS:-}
EOF

targets=()
case "$TEST_SUITE" in
  all)
    targets+=(pdfium_unittests pdfium_embeddertests)
    ;;
  unit)
    targets+=(pdfium_unittests)
    ;;
  embedder)
    targets+=(pdfium_embeddertests)
    ;;
esac

(
  cd "$SOURCE_DIR"
  gn gen "$OUT"
  ninja -C "$OUT" "${targets[@]}"
)

run_gtest() {
  local binary="$1"
  local filter="$2"
  local xml="$RESULTS/$binary.xml"
  local log="$RESULTS/$binary.log"
  local cmd=("$OUT/$binary" "--gtest_output=xml:$xml")
  local extra_gtest_args=()

  if [[ -n "$filter" ]]; then
    cmd+=("--gtest_filter=$filter")
  fi

  if [[ -n "${PDFIUM_GTEST_ARGS:-}" ]]; then
    # Intentionally shell-split simple gtest flags such as "--write-pngs".
    # Do not use this for arguments containing spaces.
    extra_gtest_args=($PDFIUM_GTEST_ARGS)
    cmd+=("${extra_gtest_args[@]}")
  fi

  echo "=== running $binary ==="
  "${cmd[@]}" 2>&1 | tee "$log"
}

case "$TEST_SUITE" in
  all)
    run_gtest pdfium_unittests "${PDFIUM_UNIT_FILTER:-}"
    run_gtest pdfium_embeddertests "${PDFIUM_EMBEDDER_FILTER:-}"
    ;;
  unit)
    run_gtest pdfium_unittests "${PDFIUM_UNIT_FILTER:-}"
    ;;
  embedder)
    run_gtest pdfium_embeddertests "${PDFIUM_EMBEDDER_FILTER:-}"
    ;;
esac

if [[ "$SAVE_CHECK_ENABLED" == true ]]; then
  "$SAVE_CHECK_PYTHON" "$SOURCE_DIR/testing/tools/check_saved_pdfs_test.py"
  # A user-supplied gtest filter may intentionally select no save-model tests.
  if compgen -G "$EPDF_SAVE_DUMP_DIR/*.pdf" >/dev/null; then
    "$SAVE_CHECK_PYTHON" "$SOURCE_DIR/testing/tools/check_saved_pdfs.py" "$EPDF_SAVE_DUMP_DIR"
  elif [[ -z "${PDFIUM_EMBEDDER_FILTER:-}" ]]; then
    echo "Save tests emitted no PDFs for independent validation" >&2
    exit 1
  fi
fi

echo "$RESULTS"
