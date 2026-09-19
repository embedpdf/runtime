// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
#ifndef TESTING_EMBEDPDF_LAYER_FIXTURE_H_
#define TESTING_EMBEDPDF_LAYER_FIXTURE_H_
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"
namespace embedpdf_test {
class MemoryFileAccess final : public FPDF_FILEACCESS {
 public:
  explicit MemoryFileAccess(std::vector<uint8_t> data)
      : data_(std::move(data)) {
    m_FileLen = static_cast<unsigned long>(data_.size());
    m_GetBlock = &MemoryFileAccess::GetBlock;
    m_Param = this;
  }

 private:
  static int GetBlock(void* param,
                      unsigned long pos,
                      unsigned char* buf,
                      unsigned long size) {
    auto* file_access = static_cast<MemoryFileAccess*>(param);
    if (!file_access || !buf || pos > file_access->data_.size() ||
        size > file_access->data_.size() - pos) {
      return 0;
    }

    std::copy_n(file_access->data_.data() + pos, size, buf);
    return 1;
  }

  std::vector<uint8_t> data_;
};
// A base document plus a layer over it, fresh or reopened over a delta.
struct LayerFixture {
  std::vector<uint8_t> bytes;
  EPDF_BASE_DOCUMENT base = nullptr;
  FPDF_DOCUMENT layer = nullptr;
  std::unique_ptr<MemoryFileAccess> delta_access;

  ~LayerFixture() {
    if (layer) {
      FPDF_CloseDocument(layer);
    }
    if (base) {
      EPDF_ReleaseBaseDocument(base);
    }
  }

  bool OpenFresh(const char* file_name) {
    std::string file_path = PathService::GetTestFilePath(file_name);
    if (file_path.empty()) {
      return false;
    }
    bytes = GetFileContents(file_path.c_str());
    if (bytes.empty()) {
      return false;
    }
    return OpenBytes(std::move(bytes));
  }

  bool OpenBytes(std::vector<uint8_t> input) {
    bytes = std::move(input);
    base = EPDF_LoadMemBaseDocument(bytes.data(),
                                    static_cast<int>(bytes.size()), nullptr);
    if (!base) {
      return false;
    }
    EPDFLayerOpenStatus status;
    layer = EPDFLayer_OpenLayer(base, nullptr, nullptr, &status);
    return layer && status == EPDFLayerOpenStatus_kSuccess;
  }

  // Close the layer and open a new one over the same base with |delta| as
  // the loaded delta: what a session reopening its saved layer does.
  bool Reopen(std::vector<uint8_t> delta) {
    if (layer) {
      FPDF_CloseDocument(layer);
      layer = nullptr;
    }
    delta_access = std::make_unique<MemoryFileAccess>(std::move(delta));
    EPDFLayerOpenStatus status;
    layer = EPDFLayer_OpenLayer(base, delta_access.get(), nullptr, &status);
    return layer && status == EPDFLayerOpenStatus_kSuccess;
  }
};

}  // namespace embedpdf_test
#endif  // TESTING_EMBEDPDF_LAYER_FIXTURE_H_
