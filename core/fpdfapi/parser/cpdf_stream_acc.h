// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PARSER_CPDF_STREAM_ACC_H_
#define CORE_FPDFAPI_PARSER_CPDF_STREAM_ACC_H_

#include <stdint.h>

#include <memory>
#include <variant>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/raw_span.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"

class CPDF_Dictionary;
class CPDF_Stream;
class PauseIndicatorIface;

namespace fxcodec {
class FlateUncompressor;
}  // namespace fxcodec

class CPDF_StreamAcc final : public Retainable {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  CPDF_StreamAcc(const CPDF_StreamAcc&) = delete;
  CPDF_StreamAcc& operator=(const CPDF_StreamAcc&) = delete;

  void LoadAllDataFiltered();
  void LoadAllDataFilteredWithEstimatedSize(uint32_t estimated_size);
  void LoadAllDataImageAcc(uint32_t estimated_size);
  void LoadAllDataRaw();

  // EmbedPDF: loads the data LoadAllDataFiltered() loads, in steps. A stream
  // whose one filter is FlateDecode, without parameters - a content stream,
  // typically, which can inflate to tens of megabytes - inflates a megabyte
  // at a time, pausing when `pause` asks; any other stream loads in one go.
  // Returns true once loaded; call again until it does.
  bool LoadAllDataFilteredInSteps(PauseIndicatorIface* pause);

  RetainPtr<const CPDF_Stream> GetStream() const;
  RetainPtr<const CPDF_Dictionary> GetImageParam() const;

  uint32_t GetSize() const;
  pdfium::span<const uint8_t> GetSpan() const;
  uint64_t KeyForCache() const;
  DataVector<uint8_t> ComputeDigest() const;
  ByteString GetImageDecoder() const { return image_decoder_; }
  DataVector<uint8_t> DetachData();

  int GetLength1ForTest() const;

 private:
  explicit CPDF_StreamAcc(RetainPtr<const CPDF_Stream> pStream);
  ~CPDF_StreamAcc() override;

  void LoadAllData(bool bRawAccess, uint32_t estimated_size, bool bImageAcc);
  void ProcessRawData();
  void ProcessFilteredData(uint32_t estimated_size, bool bImageAcc);

  // Returns the raw data from `stream_`, or no data on failure.
  DataVector<uint8_t> ReadRawStream() const;

  // EmbedPDF: for LoadAllDataFilteredInSteps(). Starts inflating, or returns
  // false for a stream that doesn't inflate in steps.
  bool StartInflating();
  void FinishInflating();

  bool is_owned() const {
    return std::holds_alternative<DataVector<uint8_t>>(data_);
  }

  ByteString image_decoder_;
  RetainPtr<const CPDF_Dictionary> image_param_;
  // Needs to outlive `data_` when the data is not owned.
  RetainPtr<const CPDF_Stream> const stream_;
  std::variant<pdfium::raw_span<const uint8_t>, DataVector<uint8_t>> data_;

  // EmbedPDF: a load in steps, and while it inflates, the raw bytes it
  // inflates (the stream's own, or a copy when it reads them from a file).
  enum class StepLoad : uint8_t { kNotStarted, kInflating, kLoaded };
  StepLoad step_load_ = StepLoad::kNotStarted;
  std::unique_ptr<fxcodec::FlateUncompressor> uncompressor_;
  std::variant<pdfium::raw_span<const uint8_t>, DataVector<uint8_t>>
      inflate_src_;
};

#endif  // CORE_FPDFAPI_PARSER_CPDF_STREAM_ACC_H_
