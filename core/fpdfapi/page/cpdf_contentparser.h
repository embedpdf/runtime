// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PAGE_CPDF_CONTENTPARSER_H_
#define CORE_FPDFAPI_PAGE_CPDF_CONTENTPARSER_H_

#include <stdint.h>

#include <memory>
#include <variant>
#include <vector>

#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"
#include "core/fpdfapi/page/cpdf_streamcontentparser.h"
#include "core/fxcrt/fixed_size_data_vector.h"
#include "core/fxcrt/raw_span.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"

class CPDF_AllStates;
class CPDF_Array;
class CPDF_Page;
class CPDF_PageObjectHolder;
class CPDF_Stream;
class CPDF_StreamAcc;
class CPDF_Type3Char;
class PauseIndicatorIface;

class CPDF_ContentParser {
 public:
  explicit CPDF_ContentParser(CPDF_Page* pPage);
  CPDF_ContentParser(RetainPtr<const CPDF_Stream> pStream,
                     CPDF_PageObjectHolder* pPageObjectHolder,
                     const CPDF_AllStates* pGraphicStates,
                     const CFX_Matrix* pParentMatrix,
                     CPDF_Type3Char* pType3Char,
                     CPDF_Form::RecursionState* recursion_state);
  ~CPDF_ContentParser();

  CPDF_PageObjectHolder::CTMMap TakeAllCTMs();

  // Returns whether to continue or not.
  bool Continue(PauseIndicatorIface* pPause);

 private:
  enum class Stage : uint8_t {
    kGetContent = 1,
    kPrepareContent,
    kParse,
    kParseForms,
    kCheckClip,
    kComplete,
  };

  Stage GetContent(PauseIndicatorIface* pPause);
  Stage PrepareContent(PauseIndicatorIface* pPause);
  Stage Parse();
  Stage ParseForms(PauseIndicatorIface* pPause);
  Stage CheckClip(PauseIndicatorIface* pPause);

  void HandlePageContentStream(const CPDF_Stream* pStream);
  bool HandlePageContentArray(const CPDF_Array* pArray);
  void HandlePageContentFailure();

  bool is_owned() const {
    return std::holds_alternative<FixedSizeDataVector<uint8_t>>(data_);
  }
  pdfium::span<const uint8_t> GetData() const;

  Stage current_stage_;
  UnownedPtr<CPDF_PageObjectHolder> const page_object_holder_;
  UnownedPtr<CPDF_Type3Char> type3_char_;  // Only used when parsing forms.
  RetainPtr<CPDF_StreamAcc> single_stream_;
  std::vector<RetainPtr<CPDF_StreamAcc>> stream_array_;
  std::vector<uint32_t> stream_segment_offsets_;
  std::variant<pdfium::raw_span<const uint8_t>, FixedSizeDataVector<uint8_t>>
      data_;
  uint32_t streams_ = 0;
  uint32_t current_offset_ = 0;
  // EmbedPDF: how far joining the page's content streams has got, and the
  // next object clip checking looks at.
  size_t joined_streams_ = 0;
  size_t joined_bytes_ = 0;
  size_t check_clip_index_ = 0;
  // Only used when parsing pages.
  CPDF_Form::RecursionState recursion_state_;

  // Must not outlive |recursion_state_|.
  std::unique_ptr<CPDF_StreamContentParser> parser_;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_CONTENTPARSER_H_
