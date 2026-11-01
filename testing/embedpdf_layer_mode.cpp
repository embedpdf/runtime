// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "testing/embedpdf_layer_mode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "public/epdf_layer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace embedpdf_test {

namespace {

// A fresh layer over `base`, which keeps the base alive.
FPDF_DOCUMENT OpenLayerOver(EPDF_BASE_DOCUMENT base) {
  if (!base) {
    return nullptr;
  }
  FPDF_DOCUMENT layer = EPDFLayer_OpenLayer(base, /*pFileAccess=*/nullptr,
                                            /*password=*/nullptr,
                                            /*out_status=*/nullptr);
  EPDF_ReleaseBaseDocument(base);
  return layer;
}

}  // namespace

bool IsLayerModeEnabled() {
  static const bool enabled = [] {
    const char* value = getenv("EPDF_EMBEDDER_LAYER_MODE");
    return value && strcmp(value, "1") == 0;
  }();
  return enabled;
}

FPDF_DOCUMENT LoadCustomDocumentForTest(FPDF_FILEACCESS* file_access,
                                        FPDF_BYTESTRING password) {
  if (!IsLayerModeEnabled()) {
    return FPDF_LoadCustomDocument(file_access, password);
  }
  return OpenLayerOver(EPDF_LoadBaseDocument(file_access, password));
}

FPDF_DOCUMENT LoadMemDocumentForTest(const void* data,
                                     size_t size,
                                     FPDF_BYTESTRING password) {
  if (!IsLayerModeEnabled()) {
    return FPDF_LoadMemDocument64(data, size, password);
  }
  return OpenLayerOver(EPDF_LoadMemBaseDocument64(data, size, password));
}

void NoteClosingDocument(FPDF_DOCUMENT document) {
  const char* report = getenv("EPDF_EMBEDDER_LAYER_MODE_REPORT");
  if (!document || !IsLayerModeEnabled() || !report || !*report) {
    return;
  }
  const ::testing::TestInfo* test =
      ::testing::UnitTest::GetInstance()->current_test_info();
  if (!test) {
    return;
  }
  const std::string name =
      std::string(test->test_suite_name()) + "." + test->name();
  FILE* file = fopen(report, "a");
  if (!file) {
    return;
  }
  fprintf(file, "%s %lu\n", name.c_str(),
          EPDFLayer_GetPromotedObjectCount(document));
  fclose(file);
}

void TestDocumentCloser::operator()(FPDF_DOCUMENT document) const {
  NoteClosingDocument(document);
  FPDF_CloseDocument(document);
}

}  // namespace embedpdf_test
