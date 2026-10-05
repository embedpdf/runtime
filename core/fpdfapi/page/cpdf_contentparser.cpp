// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/page/cpdf_contentparser.h"

#include <algorithm>
#include <utility>
#include <variant>

#include "constants/page_object.h"
#include "core/fpdfapi/font/cpdf_type3char.h"
#include "core/fpdfapi/page/cpdf_allstates.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_path.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/fixed_size_data_vector.h"
#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/pauseindicator_iface.h"
#include "core/fxcrt/span_util.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxge/cfx_fillrenderoptions.h"

CPDF_ContentParser::CPDF_ContentParser(CPDF_Page* pPage)
    : current_stage_(Stage::kGetContent), page_object_holder_(pPage) {
  DCHECK(pPage);
  if (!pPage->GetDocument()) {
    current_stage_ = Stage::kComplete;
    return;
  }

  // EmbedPDF: the page's parse, nested forms included, counts what it adds
  // and records the versions of the streams it reads.
  pPage->StartContentRecords();
  recursion_state_.parsed_size = pPage->mutable_parsed_size();
  recursion_state_.content_versions = pPage->mutable_content_versions();

  RetainPtr<const CPDF_Object> pContent =
      pPage->GetDict()->GetDirectObjectFor(pdfium::page_object::kContents);
  if (!pContent) {
    HandlePageContentFailure();
    return;
  }

  const CPDF_Stream* pStream = pContent->AsStream();
  if (pStream) {
    HandlePageContentStream(pStream);
    return;
  }

  const CPDF_Array* pArray = pContent->AsArray();
  if (pArray && HandlePageContentArray(pArray)) {
    return;
  }

  HandlePageContentFailure();
}

CPDF_ContentParser::CPDF_ContentParser(
    RetainPtr<const CPDF_Stream> pStream,
    CPDF_PageObjectHolder* pPageObjectHolder,
    const CPDF_AllStates* pGraphicStates,
    const CFX_Matrix* pParentMatrix,
    CPDF_Type3Char* pType3Char,
    CPDF_Form::RecursionState* recursion_state)
    : current_stage_(Stage::kGetContent),
      page_object_holder_(pPageObjectHolder),
      type3_char_(pType3Char) {
  DCHECK(page_object_holder_);
  CFX_Matrix form_matrix =
      page_object_holder_->GetDict()->GetMatrixFor("Matrix");
  if (pGraphicStates) {
    form_matrix.Concat(pGraphicStates->current_transformation_matrix());
  }

  RetainPtr<const CPDF_Array> pBBox =
      page_object_holder_->GetDict()->GetArrayFor("BBox");
  CFX_FloatRect form_bbox;
  CPDF_Path ClipPath;
  if (pBBox) {
    form_bbox = pBBox->GetRect();
    ClipPath.Emplace();
    ClipPath.AppendFloatRect(form_bbox);
    ClipPath.Transform(form_matrix);
    if (pParentMatrix) {
      ClipPath.Transform(*pParentMatrix);
    }

    form_bbox = form_matrix.TransformRect(form_bbox);
    if (pParentMatrix) {
      form_bbox = pParentMatrix->TransformRect(form_bbox);
    }
  }

  RetainPtr<const CPDF_Dictionary> pResources =
      page_object_holder_->GetDict()->GetDictFor("Resources");
  parser_ = std::make_unique<CPDF_StreamContentParser>(
      page_object_holder_->GetDocument(),
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(
          page_object_holder_->GetPageResources().Get())),
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(
          page_object_holder_->GetResources().Get())),
      pParentMatrix, page_object_holder_,
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(pResources.Get())),
      form_bbox, pGraphicStates, recursion_state);
  parser_->GetCurStates()->set_current_transformation_matrix(form_matrix);
  parser_->GetCurStates()->set_parent_matrix(form_matrix);
  if (ClipPath.HasRef()) {
    parser_->GetCurStates()->mutable_clip_path().AppendPathWithAutoMerge(
        ClipPath, CFX_FillRenderOptions::FillType::kWinding);
  }
  if (page_object_holder_->GetTransparency().IsGroup()) {
    CPDF_GeneralState& state = parser_->GetCurStates()->mutable_general_state();
    state.SetBlendType(BlendMode::kNormal);
    state.SetStrokeAlpha(1.0f);
    state.SetFillAlpha(1.0f);
    state.SetSoftMask(nullptr);
  }
  // EmbedPDF: decoded in the content stage, which can pause.
  single_stream_ = pdfium::MakeRetain<CPDF_StreamAcc>(std::move(pStream));
}

CPDF_ContentParser::~CPDF_ContentParser() = default;

CPDF_PageObjectHolder::CTMMap CPDF_ContentParser::TakeAllCTMs() {
  return parser_ ? parser_->TakeAllCTMs() : CPDF_PageObjectHolder::CTMMap();
}

// Returning |true| means that there is more content to be processed and
// Continue() should be called again. Returning |false| means that we've
// completed the parse and Continue() is complete.
bool CPDF_ContentParser::Continue(PauseIndicatorIface* pPause) {
  while (current_stage_ == Stage::kGetContent) {
    current_stage_ = GetContent(pPause);
    if (pPause && pPause->NeedToPauseNow()) {
      return true;
    }
  }

  if (current_stage_ == Stage::kPrepareContent) {
    current_stage_ = PrepareContent(pPause);
    if (current_stage_ == Stage::kPrepareContent) {
      return true;
    }
  }

  while (current_stage_ == Stage::kParse) {
    current_stage_ = Parse();
    if (pPause && pPause->NeedToPauseNow()) {
      return true;
    }
  }

  if (current_stage_ == Stage::kParseForms) {
    current_stage_ = ParseForms(pPause);
    if (current_stage_ == Stage::kParseForms) {
      return true;
    }
  }

  if (current_stage_ == Stage::kCheckClip) {
    current_stage_ = CheckClip(pPause);
    if (current_stage_ == Stage::kCheckClip) {
      return true;
    }
  }

  DCHECK_EQ(current_stage_, Stage::kComplete);
  return false;
}

// EmbedPDF: decodes the content a step at a time: a drawing's content can
// inflate to tens of megabytes. One stream - a form's, or a page's - or the
// next of a page's array.
CPDF_ContentParser::Stage CPDF_ContentParser::GetContent(
    PauseIndicatorIface* pPause) {
  DCHECK_EQ(current_stage_, Stage::kGetContent);
  if (single_stream_) {
    return single_stream_->LoadAllDataFilteredInSteps(pPause)
               ? Stage::kPrepareContent
               : Stage::kGetContent;
  }

  DCHECK(page_object_holder_->IsPage());
  if (!stream_array_[current_offset_]) {
    RetainPtr<const CPDF_Array> pContent =
        page_object_holder_->GetDict()->GetArrayFor(
            pdfium::page_object::kContents);
    RetainPtr<const CPDF_Stream> pStreamObj = ToStream(
        pContent ? pContent->GetDirectObjectAt(current_offset_) : nullptr);
    recursion_state_.content_versions->AddContentStream(pStreamObj);
    stream_array_[current_offset_] =
        pdfium::MakeRetain<CPDF_StreamAcc>(std::move(pStreamObj));
  }
  if (!stream_array_[current_offset_]->LoadAllDataFilteredInSteps(pPause)) {
    return Stage::kGetContent;
  }
  current_offset_++;

  return current_offset_ == streams_ ? Stage::kPrepareContent
                                     : Stage::kGetContent;
}

CPDF_ContentParser::Stage CPDF_ContentParser::PrepareContent(
    PauseIndicatorIface* pPause) {
  if (stream_array_.empty()) {
    current_offset_ = 0;
    data_ = single_stream_->GetSpan();
    return Stage::kParse;
  }

  if (!is_owned()) {
    current_offset_ = 0;
    FX_SAFE_UINT32 safe_size = 0;
    for (const auto& stream : stream_array_) {
      stream_segment_offsets_.push_back(safe_size.ValueOrDie());
      safe_size += stream->GetSize();
      safe_size += 1;
      if (!safe_size.IsValid()) {
        return Stage::kComplete;
      }
    }

    const size_t buffer_size = safe_size.ValueOrDie();
    auto buffer = FixedSizeDataVector<uint8_t>::TryZeroed(buffer_size);
    if (buffer.empty()) {
      data_.emplace<pdfium::raw_span<const uint8_t>>();
      return Stage::kComplete;
    }
    data_ = std::move(buffer);
  }

  // EmbedPDF: the streams are joined, a space after each, a few megabytes
  // at a time, so a parse can pause between them.
  static constexpr size_t kJoinStepBytes = 4 * 1024 * 1024;
  pdfium::span<uint8_t> buffer =
      std::get<FixedSizeDataVector<uint8_t>>(data_).span();
  while (joined_streams_ < stream_array_.size()) {
    pdfium::span<const uint8_t> stream =
        stream_array_[joined_streams_]->GetSpan();
    const size_t piece =
        std::min(stream.size() - joined_bytes_, kJoinStepBytes);
    const size_t at = stream_segment_offsets_[joined_streams_];
    fxcrt::spancpy(buffer.subspan(at + joined_bytes_, piece),
                   stream.subspan(joined_bytes_, piece));
    joined_bytes_ += piece;
    if (joined_bytes_ == stream.size()) {
      buffer[at + stream.size()] = ' ';
      ++joined_streams_;
      joined_bytes_ = 0;
    }
    if (joined_streams_ < stream_array_.size() && pPause &&
        pPause->NeedToPauseNow()) {
      return Stage::kPrepareContent;
    }
  }
  stream_array_.clear();
  return Stage::kParse;
}

CPDF_ContentParser::Stage CPDF_ContentParser::Parse() {
  if (!parser_) {
    recursion_state_.parsed_set.clear();
    parser_ = std::make_unique<CPDF_StreamContentParser>(
        page_object_holder_->GetDocument(),
        pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(
            page_object_holder_->GetPageResources().Get())),
        nullptr, nullptr, page_object_holder_,
        pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(
            page_object_holder_->GetResources().Get())),
        page_object_holder_->GetBBox(), nullptr, &recursion_state_);
    parser_->GetCurStates()->mutable_color_state().SetDefault();
  }
  if (current_offset_ >= GetData().size()) {
    return Stage::kParseForms;
  }

  if (stream_segment_offsets_.empty()) {
    stream_segment_offsets_.push_back(0);
  }

  static constexpr uint32_t kParseStepLimit = 100;
  current_offset_ += parser_->Parse(GetData(), current_offset_, kParseStepLimit,
                                    stream_segment_offsets_);
  return Stage::kParse;
}

// The forms the content placed and left for now: clip checking needs their
// bounds, and so does the holder of this content when it is itself a form.
CPDF_ContentParser::Stage CPDF_ContentParser::ParseForms(
    PauseIndicatorIface* pPause) {
  if (parser_ && !parser_->ParseDeferredForms(pPause)) {
    return Stage::kParseForms;
  }
  return Stage::kCheckClip;
}

CPDF_ContentParser::Stage CPDF_ContentParser::CheckClip(
    PauseIndicatorIface* pPause) {
  if (type3_char_ && check_clip_index_ == 0) {
    type3_char_->InitializeFromStreamData(parser_->IsColored(),
                                          parser_->GetType3Data());
  }

  // EmbedPDF: in steps, so a parse can pause in a page of a million objects.
  // Each call checks one step at least.
  static constexpr size_t kCheckClipStepLimit = 10000;
  const size_t count = page_object_holder_->GetPageObjectCount();
  const size_t first = check_clip_index_;
  for (; check_clip_index_ < count; ++check_clip_index_) {
    const size_t checked = check_clip_index_ - first;
    if (checked > 0 && checked % kCheckClipStepLimit == 0 && pPause &&
        pPause->NeedToPauseNow()) {
      return Stage::kCheckClip;
    }
    CPDF_PageObject* pObj =
        page_object_holder_->GetPageObjectByIndex(check_clip_index_);
    if (!pObj->IsActive()) {
      continue;
    }
    CPDF_ClipPath& clip_path = pObj->mutable_clip_path();
    if (!clip_path.HasRef()) {
      continue;
    }
    if (clip_path.GetPathCount() != 1) {
      continue;
    }
    if (clip_path.GetTextCount() > 0) {
      continue;
    }

    CPDF_Path path = clip_path.GetPath(0);
    if (!path.IsRect() || pObj->IsShading()) {
      continue;
    }

    CFX_PointF point0 = path.GetPoint(0);
    CFX_PointF point2 = path.GetPoint(2);
    CFX_FloatRect old_rect(point0.x, point0.y, point2.x, point2.y);
    if (old_rect.Contains(pObj->GetRect())) {
      clip_path.SetNull();
    }
  }
  return Stage::kComplete;
}

void CPDF_ContentParser::HandlePageContentStream(const CPDF_Stream* pStream) {
  recursion_state_.content_versions->AddContentStream(
      pdfium::WrapRetain(pStream));
  // EmbedPDF: decoded in the content stage, which can pause.
  single_stream_ =
      pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(pStream));
}

bool CPDF_ContentParser::HandlePageContentArray(const CPDF_Array* pArray) {
  streams_ = fxcrt::CollectionSize<uint32_t>(*pArray);
  if (streams_ == 0) {
    return false;
  }

  stream_array_.resize(streams_);
  return true;
}

void CPDF_ContentParser::HandlePageContentFailure() {
  current_stage_ = Stage::kComplete;
}

pdfium::span<const uint8_t> CPDF_ContentParser::GetData() const {
  if (is_owned()) {
    return std::get<FixedSizeDataVector<uint8_t>>(data_).span();
  }
  return std::get<pdfium::raw_span<const uint8_t>>(data_);
}
