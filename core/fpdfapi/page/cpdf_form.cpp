// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/page/cpdf_form.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "core/fpdfapi/page/cpdf_contentparser.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/check_op.h"
#include "core/fxge/dib/cfx_dibitmap.h"

CPDF_Form::RecursionState::RecursionState() = default;

CPDF_Form::RecursionState::~RecursionState() = default;

// static
const CPDF_Dictionary* CPDF_Form::ChooseResourcesDict(
    const CPDF_Dictionary* pResources,
    const CPDF_Dictionary* pParentResources,
    const CPDF_Dictionary* pPageResources) {
  if (pResources) {
    return pResources;
  }
  return pParentResources ? pParentResources : pPageResources;
}

CPDF_Form::CPDF_Form(CPDF_Document* doc,
                     RetainPtr<CPDF_Dictionary> pPageResources,
                     RetainPtr<CPDF_Stream> pFormStream)
    : CPDF_Form(doc,
                std::move(pPageResources),
                std::move(pFormStream),
                nullptr) {}

CPDF_Form::CPDF_Form(CPDF_Document* doc,
                     RetainPtr<CPDF_Dictionary> pPageResources,
                     RetainPtr<CPDF_Stream> pFormStream,
                     CPDF_Dictionary* pParentResources)
    : CPDF_PageObjectHolder(
          doc,
          pdfium::WrapRetain(
              const_cast<CPDF_Dictionary*>(pFormStream->GetDict().Get())),
          pPageResources,
          nullptr),
      fallback_resources_(
          pParentResources ? pdfium::WrapRetain(pParentResources)
                           : std::move(pPageResources)),
      form_stream_(std::move(pFormStream)) {
  CPDF_DocumentViewScope document_view(doc);
  RebindFormStream(form_stream_, /*reset_parsed_content=*/false);
  form_stream_epoch_ = doc ? doc->GetOverlayEpoch() : 0;
}

CPDF_Form::~CPDF_Form() = default;

void CPDF_Form::ParseContent() {
  ParseContentInternal(nullptr, nullptr, nullptr, nullptr);
}

void CPDF_Form::ParseContent(const CPDF_AllStates* pGraphicStates,
                             const CFX_Matrix* pParentMatrix,
                             RecursionState* recursion_state) {
  ParseContentInternal(pGraphicStates, pParentMatrix, nullptr, recursion_state);
}

void CPDF_Form::ParseContentForType3Char(CPDF_Type3Char* pType3Char) {
  ParseContentInternal(nullptr, nullptr, pType3Char, nullptr);
}

void CPDF_Form::ParseContentInternal(const CPDF_AllStates* pGraphicStates,
                                     const CFX_Matrix* pParentMatrix,
                                     CPDF_Type3Char* pType3Char,
                                     RecursionState* recursion_state) {
  CPDF_DocumentViewScope document_view(GetDocument());
  RetainPtr<const CPDF_Stream> stream = GetStream();

  if (GetParseState() == ParseState::kParsed) {
    return;
  }

  if (GetParseState() == ParseState::kNotParsed) {
    StartParse(std::make_unique<CPDF_ContentParser>(
        std::move(stream), this, pGraphicStates, pParentMatrix, pType3Char,
        recursion_state ? recursion_state : &recursion_state_));
  }
  DCHECK_EQ(GetParseState(), ParseState::kParsing);
  ContinueParse(nullptr);
}

bool CPDF_Form::HasPageObjects() const {
  return GetActivePageObjectCount() != 0;
}

CFX_FloatRect CPDF_Form::CalcBoundingBox() const {
  if (GetActivePageObjectCount() == 0) {
    return CFX_FloatRect();
  }

  float left = 1000000.0f;
  float right = -1000000.0f;
  float bottom = 1000000.0f;
  float top = -1000000.0f;
  for (const auto& pObj : *this) {
    if (!pObj->IsActive()) {
      continue;
    }
    const auto& rect = pObj->GetRect();
    left = std::min(left, rect.left);
    right = std::max(right, rect.right);
    bottom = std::min(bottom, rect.bottom);
    top = std::max(top, rect.top);
  }
  return CFX_FloatRect(left, bottom, right, top);
}

RetainPtr<CPDF_Stream> CPDF_Form::GetMutableFormStream() {
  CPDF_Document* doc = GetDocument();
  if (!doc || !form_stream_) {
    return form_stream_;
  }

  // Rebind a cached base stream to the effective layer object before asking
  // the document for a mutable version.
  (void)GetStream();
  const uint32_t objnum = form_stream_->GetObjNum();
  DCHECK(objnum);
  RetainPtr<CPDF_Object> live = doc->GetMutableIndirectObject(objnum);
  if (live && live.Get() != form_stream_.Get()) {
    RebindFormStream(pdfium::WrapRetain(live->AsMutableStream()),
                     /*reset_parsed_content=*/false);
  }
  form_stream_epoch_ = doc->GetOverlayEpoch();
  return form_stream_;
}

void CPDF_Form::EnsureMutableBackingObjectForDict() {
  RetainPtr<CPDF_Stream> live_stream = GetMutableFormStream();
  if (live_stream) {
    dict_ = live_stream->GetMutableDict();
  }
}

RetainPtr<const CPDF_Stream> CPDF_Form::GetStream() const {
  CPDF_Document* doc = GetDocument();
  if (!doc || !form_stream_) {
    return form_stream_;
  }

  const uint64_t current_epoch = doc->GetOverlayEpoch();
  if (form_stream_epoch_ == current_epoch) {
    return form_stream_;
  }
  form_stream_epoch_ = current_epoch;

  const uint32_t objnum = form_stream_->GetObjNum();
  if (objnum != 0) {
    RetainPtr<const CPDF_Object> effective = doc->GetIndirectObject(objnum);
    const CPDF_Stream* effective_stream =
        effective ? effective->AsStream() : nullptr;
    if (effective_stream && effective_stream != form_stream_.Get()) {
      const_cast<CPDF_Form*>(this)->RebindFormStream(
          pdfium::WrapRetain(const_cast<CPDF_Stream*>(effective_stream)),
          /*reset_parsed_content=*/true);
    }
  }
  return form_stream_;
}

bool CPDF_Form::CloneBackingStreamForWrite() {
  CPDF_Document* doc = GetDocument();
  RetainPtr<const CPDF_Stream> source = GetStream();
  if (!doc || !source) {
    return false;
  }

  RetainPtr<CPDF_Stream> clone = ToStream(source->CloneForHolder(doc));
  if (!clone) {
    return false;
  }

  // The form dictionary clone keeps indirect resource references by design.
  // Make the effective resource dictionary private too, since content
  // generation may add or prune resource names for the sanitized stream.
  if (resources_) {
    RetainPtr<CPDF_Dictionary> cloned_resources =
        ToDictionary(resources_->CloneForHolder(doc));
    if (!cloned_resources) {
      return false;
    }
    // EmbedPDF: a category (/Font, /XObject, /ExtGState...) that is an object
    // of its own is shared with every other placement of this form, and
    // content generation adds and prunes names in it. Copy it too. The fonts,
    // images and forms it names stay shared: nothing here edits them.
    std::vector<ByteString> shared_categories;
    {
      CPDF_DictionaryLocker locker(cloned_resources.Get());
      for (const auto& entry : locker) {
        if (entry.second->IsReference()) {
          shared_categories.push_back(entry.first);
        }
      }
    }
    for (const ByteString& key : shared_categories) {
      RetainPtr<const CPDF_Dictionary> category =
          cloned_resources->GetDictFor(key.AsStringView());
      if (category) {
        cloned_resources->SetFor(key, category->CloneForHolder(doc));
      }
    }
    clone->GetMutableDict()->SetFor("Resources", std::move(cloned_resources));
  }

  if (doc->AddIndirectObject(clone) == 0) {
    return false;
  }
  RebindFormStream(std::move(clone), /*reset_parsed_content=*/false);
  return true;
}

void CPDF_Form::RebindFormStream(RetainPtr<CPDF_Stream> stream,
                                 bool reset_parsed_content) {
  CHECK(stream);
  form_stream_ = std::move(stream);
  dict_ = form_stream_->GetMutableDict();

  RetainPtr<const CPDF_Dictionary> stream_resources =
      dict_->GetDictFor("Resources");
  resources_ = stream_resources
                   ? pdfium::WrapRetain(
                         const_cast<CPDF_Dictionary*>(stream_resources.Get()))
                   : fallback_resources_;

  const uint64_t current_epoch =
      GetDocument() ? GetDocument()->GetOverlayEpoch() : 0;
  dict_epoch_ = current_epoch;
  resources_epoch_ = current_epoch;

  if (reset_parsed_content) {
    ResetParsedContent();
    recursion_state_.parsed_set.clear();
  }

  transparency_ = CPDF_Transparency();
  LoadTransparencyInfo();
}

std::optional<std::pair<RetainPtr<CFX_DIBitmap>, CFX_Matrix>>
CPDF_Form::GetBitmapAndMatrixFromSoleImageOfForm() const {
  // TODO(crbug.com/377660088): Determine if there is a case where only a single
  // active object but other inactive objects is problematic for this method.
  if (GetActivePageObjectCount() != 1) {
    return std::nullopt;
  }

  CPDF_ImageObject* pImageObject = (*begin())->AsImage();
  if (!pImageObject) {
    return std::nullopt;
  }

  return {{pImageObject->GetIndependentBitmap(), pImageObject->matrix()}};
}
