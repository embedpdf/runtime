// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FXCODEC_FLATE_FLATEMODULE_H_
#define CORE_FXCODEC_FLATE_FLATEMODULE_H_

#include <stdint.h>

#include <functional>
#include <memory>
#include <vector>

#include "core/fxcodec/data_and_bytes_consumed.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/span.h"

struct z_stream_s;

namespace fxcodec {

class ScanlineDecoder;

// EmbedPDF: inflates a FlateDecode stream the way FlateOrLZWDecode() does
// without a predictor, a piece at a time, so a caller can pause between
// pieces: a drawing's content can inflate to tens of megabytes. It is the
// decoder FlateUncompress() runs to the end, so both give the same bytes.
class FlateUncompressor {
 public:
  // Inflates `src_buf`, which must outlive this, with the first output
  // buffer sized from `orig_size` (0 when unknown) and `src_buf`'s size.
  FlateUncompressor(pdfium::span<const uint8_t> src_buf, uint32_t orig_size);
  ~FlateUncompressor();

  FlateUncompressor(const FlateUncompressor&) = delete;
  FlateUncompressor& operator=(const FlateUncompressor&) = delete;

  // Inflates up to `max_bytes` (at least 1) more output, or once the stream
  // has ended (or stopped on bad data), joins up to `max_bytes` of it into
  // one buffer. Returns false once done; then TakeResult().
  bool Continue(size_t max_bytes);

  // The output and the input bytes consumed, once Continue() returned false.
  DataAndBytesConsumed TakeResult();

 private:
  struct ContextDeleter {
    void operator()(z_stream_s* context) const;
  };

  void Inflate(size_t max_bytes);
  void Join(size_t max_bytes);

  std::unique_ptr<z_stream_s, ContextDeleter> context_;
  const uint32_t buf_size_;
  uint32_t last_buf_size_;
  DataVector<uint8_t> cur_buf_;
  // How much of `cur_buf_` holds output.
  size_t cur_filled_ = 0;
  std::vector<DataVector<uint8_t>> result_tmp_bufs_;
  bool inflated_ = false;
  // Joining the buffers: the output's size, the next buffer, and the output.
  uint32_t dest_size_ = 0;
  size_t next_buf_ = 0;
  // How much of the output came from the buffers before `next_buf_`.
  size_t joined_before_next_ = 0;
  DataVector<uint8_t> result_;
  bool done_ = false;
};

class FlateModule {
 public:
  static std::unique_ptr<ScanlineDecoder> CreateDecoder(
      pdfium::span<const uint8_t> src_span,
      int width,
      int height,
      int nComps,
      int bpc,
      int predictor,
      int Colors,
      int BitsPerComponent,
      int Columns);

  static DataAndBytesConsumed FlateOrLZWDecode(
      bool bLZW,
      pdfium::span<const uint8_t> src_span,
      bool bEarlyChange,
      int predictor,
      int Colors,
      int BitsPerComponent,
      int Columns,
      uint32_t estimated_size);

  // EmbedPDF: outcome of FlateDecodeToSink().
  enum class SinkDecodeStatus : uint8_t {
    kSuccess,
    kLimitExceeded,
    kSinkError,
  };

  // EmbedPDF: inflates |src_span| into |sink| one bounded chunk at a time,
  // without materializing the full decoded output. Peak memory is one chunk
  // regardless of the decoded size. |sink| returns false to abort.
  // |max_decoded_bytes| of 0 means unlimited; when the decoded output would
  // exceed it, decoding stops with kLimitExceeded (|sink| may already have
  // received earlier chunks). On return, |*total_out| holds the number of
  // bytes handed to |sink|. Termination semantics match FlateUncompress():
  // corrupt trailing data yields the successfully inflated prefix rather
  // than an error, and an empty decoded stream is a valid kSuccess result.
  static SinkDecodeStatus FlateDecodeToSink(
      pdfium::span<const uint8_t> src_span,
      uint64_t max_decoded_bytes,
      const std::function<bool(pdfium::span<const uint8_t>)>& sink,
      uint64_t* total_out);

  static DataVector<uint8_t> Encode(pdfium::span<const uint8_t> src_span);

  FlateModule() = delete;
  FlateModule(const FlateModule&) = delete;
  FlateModule& operator=(const FlateModule&) = delete;
};

}  // namespace fxcodec

using FlateModule = fxcodec::FlateModule;

#endif  // CORE_FXCODEC_FLATE_FLATEMODULE_H_
