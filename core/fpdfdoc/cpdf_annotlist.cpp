// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfdoc/cpdf_annotlist.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "constants/annotation_common.h"
#include "constants/annotation_flags.h"
#include "constants/form_fields.h"
#include "core/fpdfapi/page/cpdf_occontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfapi/render/cpdf_renderoptions.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/containers/unique_ptr_adapters.h"

namespace {

bool PopupAppearsForAnnotType(CPDF_Annot::Subtype subtype) {
  switch (subtype) {
    case CPDF_Annot::Subtype::TEXT:
    case CPDF_Annot::Subtype::LINE:
    case CPDF_Annot::Subtype::SQUARE:
    case CPDF_Annot::Subtype::CIRCLE:
    case CPDF_Annot::Subtype::POLYGON:
    case CPDF_Annot::Subtype::POLYLINE:
    case CPDF_Annot::Subtype::HIGHLIGHT:
    case CPDF_Annot::Subtype::UNDERLINE:
    case CPDF_Annot::Subtype::SQUIGGLY:
    case CPDF_Annot::Subtype::STRIKEOUT:
    case CPDF_Annot::Subtype::STAMP:
    case CPDF_Annot::Subtype::CARET:
    case CPDF_Annot::Subtype::INK:
    case CPDF_Annot::Subtype::FILEATTACHMENT:
    case CPDF_Annot::Subtype::REDACT:
      return true;
    case CPDF_Annot::Subtype::UNKNOWN:
    case CPDF_Annot::Subtype::LINK:
    case CPDF_Annot::Subtype::FREETEXT:
    case CPDF_Annot::Subtype::POPUP:
    case CPDF_Annot::Subtype::SOUND:
    case CPDF_Annot::Subtype::MOVIE:
    case CPDF_Annot::Subtype::WIDGET:
    case CPDF_Annot::Subtype::SCREEN:
    case CPDF_Annot::Subtype::PRINTERMARK:
    case CPDF_Annot::Subtype::TRAPNET:
    case CPDF_Annot::Subtype::WATERMARK:
    case CPDF_Annot::Subtype::THREED:
    case CPDF_Annot::Subtype::RICHMEDIA:
    case CPDF_Annot::Subtype::XFAWIDGET:
      return false;
  }
}

std::unique_ptr<CPDF_Annot> CreatePopupAnnot(CPDF_Document* document,
                                             CPDF_Page* pPage,
                                             CPDF_Annot* pAnnot) {
  if (!PopupAppearsForAnnotType(pAnnot->GetSubtype())) {
    return nullptr;
  }

  const CPDF_Dictionary* pParentDict = pAnnot->GetAnnotDict();
  if (!pParentDict) {
    return nullptr;
  }

  // TODO(crbug.com/40643646): Determine if we really need to check if
  // /Contents is empty or not. If so, optimize decoding for empty check.
  ByteString contents =
      pParentDict->GetByteStringFor(pdfium::annotation::kContents);
  if (PDF_DecodeText(contents.unsigned_span()).IsEmpty()) {
    return nullptr;
  }

  auto pAnnotDict = document->New<CPDF_Dictionary>();
  pAnnotDict->SetNewFor<CPDF_Name>(pdfium::annotation::kType, "Annot");
  pAnnotDict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype, "Popup");
  pAnnotDict->SetNewFor<CPDF_String>(
      pdfium::form_fields::kT,
      pParentDict->GetByteStringFor(pdfium::form_fields::kT));
  pAnnotDict->SetNewFor<CPDF_String>(pdfium::annotation::kContents, contents);

  CFX_FloatRect rect = pParentDict->GetRectFor(pdfium::annotation::kRect);
  rect.Normalize();
  CFX_FloatRect popupRect(0, 0, 200, 200);
  // Note that if the popup can set its own dimensions, then we will need to
  // make sure that it isn't larger than the page size.
  if (rect.left + popupRect.Width() > pPage->GetPageWidth() &&
      rect.bottom - popupRect.Height() < 0) {
    // If the annotation is on the bottom-right corner of the page, then place
    // the popup above and to the left of the annotation.
    popupRect.Translate(rect.right - popupRect.Width(), rect.top);
  } else {
    // Place the popup below and to the right of the annotation without getting
    // clipped by page edges.
    popupRect.Translate(
        std::min(rect.left, pPage->GetPageWidth() - popupRect.Width()),
        std::max(rect.bottom - popupRect.Height(), 0.f));
  }

  pAnnotDict->SetRectFor(pdfium::annotation::kRect, popupRect);
  pAnnotDict->SetNewFor<CPDF_Number>(pdfium::annotation::kF, 0);

  // The popup is closed, so it is drawn only once opened, and drawing
  // generates its appearance. Generating it here would build one, with a font
  // and two new objects in the document, for every popup of every list, and
  // rendering a page builds a list each time.
  auto pPopupAnnot = std::make_unique<CPDF_Annot>(
      std::move(pAnnotDict), document,
      CPDF_Annot::MissingAppearance::kGenerateWhenDrawn);
  pAnnot->SetPopupAnnot(pPopupAnnot.get());
  return pPopupAnnot;
}

}  // namespace

CPDF_AnnotList::CPDF_AnnotList(CPDF_Page* pPage)
    : page_(pPage), document_(page_->GetDocument()) {
  CPDF_DocumentViewScope document_view(document_);

  RetainPtr<const CPDF_Array> pAnnots = page_->GetAnnotsArray();
  if (!pAnnots) {
    return;
  }

  for (size_t i = 0; i < pAnnots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> const_dict = pAnnots->GetDictAt(i);
    if (!const_dict) {
      continue;
    }
    const ByteString subtype =
        const_dict->GetByteStringFor(pdfium::annotation::kSubtype);
    if (subtype == "Popup") {
      // Skip creating Popup annotations in the PDF document since PDFium
      // provides its own Popup annotations.
      continue;
    }
    // CPDF_Annot still owns a mutable dictionary handle because explicit edit
    // APIs mutate annotation dictionaries. Listing/rendering must not promote
    // direct annotations to indirect objects.
    RetainPtr<CPDF_Dictionary> dict =
        pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(const_dict.Get()));
    annot_list_.push_back(std::make_unique<CPDF_Annot>(dict, document_));
  }

  annot_count_ = annot_list_.size();
  for (size_t i = 0; i < annot_count_; ++i) {
    std::unique_ptr<CPDF_Annot> pPopupAnnot =
        CreatePopupAnnot(document_, page_, annot_list_[i].get());
    if (pPopupAnnot) {
      annot_list_.push_back(std::move(pPopupAnnot));
    }
  }
}

CPDF_AnnotList::~CPDF_AnnotList() {
  // Move the pop-up annotations out of |annot_list_| into |popups|. Then
  // destroy |annot_list_| first. This prevents dangling pointers to the pop-up
  // annotations.
  size_t nPopupCount = annot_list_.size() - annot_count_;
  std::vector<std::unique_ptr<CPDF_Annot>> popups(nPopupCount);
  for (size_t i = 0; i < nPopupCount; ++i) {
    popups[i] = std::move(annot_list_[annot_count_ + i]);
  }
  annot_list_.clear();
}

bool CPDF_AnnotList::Contains(const CPDF_Annot* pAnnot) const {
  auto it = std::ranges::find_if(
      annot_list_, pdfium::MatchesUniquePtr(const_cast<CPDF_Annot*>(pAnnot)));
  return it != annot_list_.end();
}

void CPDF_AnnotList::DisplayPass(CPDF_RenderContext* context,
                                 bool bPrinting,
                                 const CFX_Matrix& mtMatrix,
                                 bool bWidgetPass) {
  CPDF_DocumentViewScope document_view(document_);

  CHECK(context);
  for (const auto& pAnnot : annot_list_) {
    bool bWidget = pAnnot->GetSubtype() == CPDF_Annot::Subtype::WIDGET;
    if ((bWidgetPass && !bWidget) || (!bWidgetPass && bWidget)) {
      continue;
    }

    uint32_t annot_flags = pAnnot->GetFlags();
    if (annot_flags & pdfium::annotation_flags::kHidden) {
      continue;
    }

    if (bPrinting && (annot_flags & pdfium::annotation_flags::kPrint) == 0) {
      continue;
    }

    if (!bPrinting && (annot_flags & pdfium::annotation_flags::kNoView)) {
      continue;
    }

    pAnnot->DrawInContext(page_, context, mtMatrix,
                          CPDF_Annot::AppearanceMode::kNormal);
  }
}

void CPDF_AnnotList::DisplayAnnots(CPDF_RenderContext* context,
                                   bool bPrinting,
                                   const CFX_Matrix& mtUser2Device,
                                   bool bShowWidget) {
  CHECK(context);
  DisplayPass(context, bPrinting, mtUser2Device, false);
  if (bShowWidget) {
    DisplayPass(context, bPrinting, mtUser2Device, true);
  }
}
