// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfdoc/cpdf_annot.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "build/build_config.h"
#include "constants/annotation_common.h"
#include "constants/annotation_flags.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageimagecache.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fpdfapi/render/cpdf_rendercontext.h"
#include "core/fpdfapi/render/cpdf_renderoptions.h"
#include "core/fpdfdoc/cpdf_generateap.h"
#include "core/fxcrt/check.h"
#include "core/fxge/cfx_fillrenderoptions.h"
#include "core/fxge/cfx_graphstatedata.h"
#include "core/fxge/cfx_path.h"
#include "core/fxge/cfx_renderdevice.h"

namespace {

const char kPDFiumKey_HasGeneratedAP[] = "PDFIUM_HasGeneratedAP";

bool IsTextMarkupAnnotation(CPDF_Annot::Subtype type) {
  return type == CPDF_Annot::Subtype::HIGHLIGHT ||
         type == CPDF_Annot::Subtype::SQUIGGLY ||
         type == CPDF_Annot::Subtype::STRIKEOUT ||
         type == CPDF_Annot::Subtype::UNDERLINE;
}

CPDF_Form* AnnotGetMatrix(CPDF_Page* pPage,
                          CPDF_Annot* pAnnot,
                          CPDF_Annot::AppearanceMode mode,
                          const CFX_Matrix& mtUser2Device,
                          CFX_Matrix* matrix) {
  CPDF_Form* pForm = pAnnot->GetAPForm(pPage, mode);
  if (!pForm) {
    return nullptr;
  }

  CFX_Matrix form_matrix = pForm->GetDict()->GetMatrixFor("Matrix");
  CFX_FloatRect form_bbox =
      form_matrix.TransformRect(pForm->GetDict()->GetRectFor("BBox"));
  matrix->MatchRect(pAnnot->GetRect(), form_bbox);

  // Compensate for page rotation.
  if ((pAnnot->GetFlags() & pdfium::annotation_flags::kNoRotate) &&
      pPage->GetPageRotation() != 0) {
    // Rotate annotation rect around top-left angle (according to the
    // specification).
    const float offset_x = pAnnot->GetRect().Left();
    const float offset_y = pAnnot->GetRect().Top();
    matrix->Concat({1, 0, 0, 1, -offset_x, -offset_y});
    // GetPageRotation returns value in fractions of pi/2.
    const float angle = FXSYS_PI / 2 * pPage->GetPageRotation();
    matrix->Rotate(angle);
    matrix->Concat({1, 0, 0, 1, offset_x, offset_y});
  }

  matrix->Concat(mtUser2Device);
  return pForm;
}

RetainPtr<CPDF_Stream> GetAnnotAPInternal(const CPDF_Dictionary* pAnnotDict,
                                          CPDF_Annot::AppearanceMode eMode,
                                          bool bFallbackToNormal) {
  RetainPtr<const CPDF_Dictionary> pAP =
      pAnnotDict->GetDictFor(pdfium::annotation::kAP);
  if (!pAP) {
    return nullptr;
  }

  const char* ap_entry = "N";
  if (eMode == CPDF_Annot::AppearanceMode::kDown) {
    ap_entry = "D";
  } else if (eMode == CPDF_Annot::AppearanceMode::kRollover) {
    ap_entry = "R";
  }
  if (bFallbackToNormal && !pAP->KeyExist(ap_entry)) {
    ap_entry = "N";
  }

  RetainPtr<const CPDF_Object> psub = pAP->GetDirectObjectFor(ap_entry);
  if (!psub) {
    return nullptr;
  }

  RetainPtr<const CPDF_Stream> pStream(psub->AsStream());
  if (pStream) {
    return pdfium::WrapRetain(const_cast<CPDF_Stream*>(pStream.Get()));
  }

  const CPDF_Dictionary* dict = psub->AsDictionary();
  if (!dict) {
    return nullptr;
  }

  ByteString as = pAnnotDict->GetByteStringFor(pdfium::annotation::kAS);
  if (as.IsEmpty()) {
    ByteString value = pAnnotDict->GetByteStringFor("V");
    if (value.IsEmpty()) {
      RetainPtr<const CPDF_Dictionary> pParentDict =
          pAnnotDict->GetDictFor("Parent");
      value = pParentDict ? pParentDict->GetByteStringFor("V") : ByteString();
    }
    as = (!value.IsEmpty() && dict->KeyExist(value.AsStringView())) ? value
                                                                    : "Off";
  }
  RetainPtr<const CPDF_Stream> stream = dict->GetStreamFor(as.AsStringView());
  return pdfium::WrapRetain(const_cast<CPDF_Stream*>(stream.Get()));
}

}  // namespace

CPDF_Annot::CPDF_Annot(RetainPtr<CPDF_Dictionary> dict, CPDF_Document* document)
    : annot_dict_(std::move(dict)),
      document_(document),
      subtype_(StringToAnnotSubtype(
          annot_dict_->GetByteStringFor(pdfium::annotation::kSubtype))),
      is_text_markup_annotation_(IsTextMarkupAnnotation(subtype_)),
      has_generated_ap_(
          annot_dict_->GetBooleanFor(kPDFiumKey_HasGeneratedAP, false) ||
          (CanGenerateEphemeralAP() && ShouldGenerateAP())) {
  if (!CanGenerateEphemeralAP()) {
    GenerateAPIfNeeded();
  }
}

CPDF_Annot::~CPDF_Annot() {
  ClearCachedAP();
}

void CPDF_Annot::GenerateAPIfNeeded() {
  if (!ShouldGenerateAP()) {
    return;
  }
  if (!CPDF_GenerateAP::GenerateAnnotAP(document_, annot_dict_.Get(),
                                        subtype_)) {
    return;
  }

  annot_dict_->SetNewFor<CPDF_Boolean>(kPDFiumKey_HasGeneratedAP, true);
  has_generated_ap_ = true;
}

bool CPDF_Annot::ShouldGenerateAP() const {
  // If AP dictionary exists and defines an appearance for normal mode, we use
  // the appearance defined in the existing AP dictionary.
  RetainPtr<const CPDF_Dictionary> pAP =
      annot_dict_->GetDictFor(pdfium::annotation::kAP);
  if (pAP && pAP->GetDictFor("N")) {
    return false;
  }

  return !IsHidden();
}

bool CPDF_Annot::CanGenerateEphemeralAP() const {
  return CPDF_GenerateAP::CanGenerateEphemeralAnnotAP(subtype_);
}

RetainPtr<CPDF_Stream> CPDF_Annot::GetOrBuildEphemeralAP(AppearanceMode mode) {
  if (mode != AppearanceMode::kNormal || !CanGenerateEphemeralAP()) {
    return nullptr;
  }

  if (ephemeral_built_) {
    return ephemeral_normal_ap_;
  }

  ephemeral_built_ = true;
  if (!ShouldGenerateAP()) {
    return nullptr;
  }

  std::optional<CPDF_GenerateAP::GeneratedAP> generated =
      CPDF_GenerateAP::GenerateEphemeralAnnotAP(document_, annot_dict_.Get(),
                                                subtype_);
  if (!generated.has_value()) {
    return nullptr;
  }

  ephemeral_font_lifetime_ = std::move(generated->font_lifetime);
  ephemeral_normal_ap_ = std::move(generated->normal_stream);
  // Dimension labels/leaders can extend /Rect. Use their generated envelope
  // for drawing without persisting it, or the AP is squeezed into the old box.
  if (subtype_ == CPDF_Annot::Subtype::INK ||
      subtype_ == CPDF_Annot::Subtype::LINE ||
      subtype_ == CPDF_Annot::Subtype::POLYGON ||
      subtype_ == CPDF_Annot::Subtype::POLYLINE) {
    ephemeral_rect_ = ephemeral_normal_ap_->GetDict()->GetRectFor("BBox");
  }
  has_generated_ap_ = true;
  return ephemeral_normal_ap_;
}

bool CPDF_Annot::ShouldDrawAnnotation() const {
  if (IsHidden()) {
    return false;
  }
  return open_state_ || subtype_ != CPDF_Annot::Subtype::POPUP;
}

void CPDF_Annot::ClearCachedAP() {
  ap_map_.clear();
  ephemeral_normal_ap_.Reset();
  ephemeral_font_lifetime_.reset();
  ephemeral_rect_.reset();
  ephemeral_built_ = false;
  has_generated_ap_ =
      annot_dict_->GetBooleanFor(kPDFiumKey_HasGeneratedAP, false) ||
      (CanGenerateEphemeralAP() && ShouldGenerateAP());
}

CPDF_Annot::Subtype CPDF_Annot::GetSubtype() const {
  return subtype_;
}

CFX_FloatRect CPDF_Annot::RectForDrawing() const {
  if (ephemeral_rect_.has_value()) {
    return ephemeral_rect_.value();
  }

  bool bShouldUseQuadPointsCoords =
      is_text_markup_annotation_ && has_generated_ap_;
  if (bShouldUseQuadPointsCoords) {
    return BoundingRectFromQuadPoints(annot_dict_.Get());
  }
  return annot_dict_->GetRectFor(pdfium::annotation::kRect);
}

CFX_FloatRect CPDF_Annot::GetRect() const {
  CFX_FloatRect rect = RectForDrawing();
  rect.Normalize();
  return rect;
}

uint32_t CPDF_Annot::GetFlags() const {
  return annot_dict_->GetIntegerFor(pdfium::annotation::kF);
}

bool CPDF_Annot::IsHidden() const {
  return !!(GetFlags() & pdfium::annotation_flags::kHidden);
}

RetainPtr<CPDF_Stream> GetAnnotAP(const CPDF_Dictionary* pAnnotDict,
                                  CPDF_Annot::AppearanceMode eMode) {
  DCHECK(pAnnotDict);
  return GetAnnotAPInternal(pAnnotDict, eMode, true);
}

RetainPtr<CPDF_Stream> GetAnnotAPNoFallback(const CPDF_Dictionary* pAnnotDict,
                                            CPDF_Annot::AppearanceMode eMode) {
  DCHECK(pAnnotDict);
  return GetAnnotAPInternal(pAnnotDict, eMode, false);
}

CPDF_Form* CPDF_Annot::GetAPForm(CPDF_Page* pPage, AppearanceMode mode) {
  RetainPtr<CPDF_Stream> pStream = GetAnnotAP(annot_dict_.Get(), mode);
  if (!pStream) {
    pStream = GetOrBuildEphemeralAP(mode);
  }
  if (!pStream) {
    return nullptr;
  }

  auto it = ap_map_.find(pStream);
  if (it != ap_map_.end()) {
    return it->second.get();
  }

  auto pNewForm = std::make_unique<CPDF_Form>(
      document_,
      pdfium::WrapRetain(
          const_cast<CPDF_Dictionary*>(pPage->GetResources().Get())),
      pStream);
  pNewForm->ParseContent();

  CPDF_Form* pResult = pNewForm.get();
  ap_map_[pStream] = std::move(pNewForm);
  return pResult;
}

void CPDF_Annot::SetPopupAnnotOpenState(bool bOpenState) {
  if (popup_annot_) {
    popup_annot_->SetOpenState(bOpenState);
  }
}

std::optional<CFX_FloatRect> CPDF_Annot::GetPopupAnnotRect() const {
  if (!popup_annot_) {
    return std::nullopt;
  }
  return popup_annot_->GetRect();
}

// static
CFX_FloatRect CPDF_Annot::RectFromQuadPointsArray(const CPDF_Array* pArray,
                                                  size_t nIndex) {
  DCHECK(pArray);
  DCHECK(nIndex < pArray->size() / 8);

  // QuadPoints text-markup order is top-left, top-right, bottom-left,
  // bottom-right. Bounds remain producer-order agnostic and inspect all four.
  const size_t offset = nIndex * 8;
  std::optional<CFX_FloatRect> rect;
  for (size_t i = 0; i < 4; ++i) {
    const CFX_PointF point(pArray->GetFloatAt(offset + i * 2),
                           pArray->GetFloatAt(offset + i * 2 + 1));
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
      continue;
    }
    if (rect) {
      rect->UpdateRect(point);
    } else {
      rect.emplace(point);
    }
  }
  return rect.value_or(CFX_FloatRect());
}

// static
CFX_FloatRect CPDF_Annot::BoundingRectFromQuadPoints(
    const CPDF_Dictionary* pAnnotDict) {
  CFX_FloatRect ret;
  RetainPtr<const CPDF_Array> pArray = pAnnotDict->GetArrayFor("QuadPoints");
  size_t nQuadPointCount = pArray ? QuadPointCount(pArray.Get()) : 0;
  if (nQuadPointCount == 0) {
    return ret;
  }

  ret = RectFromQuadPointsArray(pArray.Get(), 0);
  for (size_t i = 1; i < nQuadPointCount; ++i) {
    CFX_FloatRect rect = RectFromQuadPointsArray(pArray.Get(), i);
    ret.Union(rect);
  }
  return ret;
}

// static
CFX_FloatRect CPDF_Annot::RectFromQuadPoints(const CPDF_Dictionary* pAnnotDict,
                                             size_t nIndex) {
  RetainPtr<const CPDF_Array> pArray = pAnnotDict->GetArrayFor("QuadPoints");
  size_t nQuadPointCount = pArray ? QuadPointCount(pArray.Get()) : 0;
  if (nIndex >= nQuadPointCount) {
    return CFX_FloatRect();
  }
  return RectFromQuadPointsArray(pArray.Get(), nIndex);
}

// static
CPDF_Annot::Subtype CPDF_Annot::StringToAnnotSubtype(
    const ByteString& sSubtype) {
  if (sSubtype == "Text") {
    return CPDF_Annot::Subtype::TEXT;
  }
  if (sSubtype == "Link") {
    return CPDF_Annot::Subtype::LINK;
  }
  if (sSubtype == "FreeText") {
    return CPDF_Annot::Subtype::FREETEXT;
  }
  if (sSubtype == "Line") {
    return CPDF_Annot::Subtype::LINE;
  }
  if (sSubtype == "Square") {
    return CPDF_Annot::Subtype::SQUARE;
  }
  if (sSubtype == "Circle") {
    return CPDF_Annot::Subtype::CIRCLE;
  }
  if (sSubtype == "Polygon") {
    return CPDF_Annot::Subtype::POLYGON;
  }
  if (sSubtype == "PolyLine") {
    return CPDF_Annot::Subtype::POLYLINE;
  }
  if (sSubtype == "Highlight") {
    return CPDF_Annot::Subtype::HIGHLIGHT;
  }
  if (sSubtype == "Underline") {
    return CPDF_Annot::Subtype::UNDERLINE;
  }
  if (sSubtype == "Squiggly") {
    return CPDF_Annot::Subtype::SQUIGGLY;
  }
  if (sSubtype == "StrikeOut") {
    return CPDF_Annot::Subtype::STRIKEOUT;
  }
  if (sSubtype == "Stamp") {
    return CPDF_Annot::Subtype::STAMP;
  }
  if (sSubtype == "Caret") {
    return CPDF_Annot::Subtype::CARET;
  }
  if (sSubtype == "Ink") {
    return CPDF_Annot::Subtype::INK;
  }
  if (sSubtype == "Popup") {
    return CPDF_Annot::Subtype::POPUP;
  }
  if (sSubtype == "FileAttachment") {
    return CPDF_Annot::Subtype::FILEATTACHMENT;
  }
  if (sSubtype == "Sound") {
    return CPDF_Annot::Subtype::SOUND;
  }
  if (sSubtype == "Movie") {
    return CPDF_Annot::Subtype::MOVIE;
  }
  if (sSubtype == "Widget") {
    return CPDF_Annot::Subtype::WIDGET;
  }
  if (sSubtype == "Screen") {
    return CPDF_Annot::Subtype::SCREEN;
  }
  if (sSubtype == "PrinterMark") {
    return CPDF_Annot::Subtype::PRINTERMARK;
  }
  if (sSubtype == "TrapNet") {
    return CPDF_Annot::Subtype::TRAPNET;
  }
  if (sSubtype == "Watermark") {
    return CPDF_Annot::Subtype::WATERMARK;
  }
  if (sSubtype == "3D") {
    return CPDF_Annot::Subtype::THREED;
  }
  if (sSubtype == "RichMedia") {
    return CPDF_Annot::Subtype::RICHMEDIA;
  }
  if (sSubtype == "XFAWidget") {
    return CPDF_Annot::Subtype::XFAWIDGET;
  }
  if (sSubtype == "Redact") {
    return CPDF_Annot::Subtype::REDACT;
  }
  return CPDF_Annot::Subtype::UNKNOWN;
}

// static
ByteString CPDF_Annot::AnnotSubtypeToString(CPDF_Annot::Subtype nSubtype) {
  switch (nSubtype) {
    case CPDF_Annot::Subtype::UNKNOWN:
      break;
    case CPDF_Annot::Subtype::TEXT:
      return "Text";
    case CPDF_Annot::Subtype::LINK:
      return "Link";
    case CPDF_Annot::Subtype::FREETEXT:
      return "FreeText";
    case CPDF_Annot::Subtype::LINE:
      return "Line";
    case CPDF_Annot::Subtype::SQUARE:
      return "Square";
    case CPDF_Annot::Subtype::CIRCLE:
      return "Circle";
    case CPDF_Annot::Subtype::POLYGON:
      return "Polygon";
    case CPDF_Annot::Subtype::POLYLINE:
      return "PolyLine";
    case CPDF_Annot::Subtype::HIGHLIGHT:
      return "Highlight";
    case CPDF_Annot::Subtype::UNDERLINE:
      return "Underline";
    case CPDF_Annot::Subtype::SQUIGGLY:
      return "Squiggly";
    case CPDF_Annot::Subtype::STRIKEOUT:
      return "StrikeOut";
    case CPDF_Annot::Subtype::STAMP:
      return "Stamp";
    case CPDF_Annot::Subtype::CARET:
      return "Caret";
    case CPDF_Annot::Subtype::INK:
      return "Ink";
    case CPDF_Annot::Subtype::POPUP:
      return "Popup";
    case CPDF_Annot::Subtype::FILEATTACHMENT:
      return "FileAttachment";
    case CPDF_Annot::Subtype::SOUND:
      return "Sound";
    case CPDF_Annot::Subtype::MOVIE:
      return "Movie";
    case CPDF_Annot::Subtype::WIDGET:
      return "Widget";
    case CPDF_Annot::Subtype::SCREEN:
      return "Screen";
    case CPDF_Annot::Subtype::PRINTERMARK:
      return "PrinterMark";
    case CPDF_Annot::Subtype::TRAPNET:
      return "TrapNet";
    case CPDF_Annot::Subtype::WATERMARK:
      return "Watermark";
    case CPDF_Annot::Subtype::THREED:
      return "3D";
    case CPDF_Annot::Subtype::RICHMEDIA:
      return "RichMedia";
    case CPDF_Annot::Subtype::XFAWIDGET:
      return "XFAWidget";
    case CPDF_Annot::Subtype::REDACT:
      return "Redact";
  }
  return ByteString();
}

CPDF_Annot::Icon CPDF_Annot::StringToIcon(const ByteString& name) {
  if (name == "Comment") {
    return Icon::kText_Comment;
  }
  if (name == "Key") {
    return Icon::kText_Key;
  }
  if (name == "Note") {
    return Icon::kText_Note;
  }
  if (name == "Help") {
    return Icon::kText_Help;
  }
  if (name == "NewParagraph") {
    return Icon::kText_NewParagraph;
  }
  if (name == "Paragraph") {
    return Icon::kText_Paragraph;
  }
  if (name == "Insert") {
    return Icon::kText_Insert;
  }
  if (name == "Graph") {
    return Icon::kFile_Graph;
  }
  if (name == "PushPin") {
    return Icon::kFile_PushPin;
  }
  if (name == "Paperclip") {
    return Icon::kFile_Paperclip;
  }
  if (name == "Tag") {
    return Icon::kFile_Tag;
  }
  if (name == "Speaker") {
    return Icon::kSound_Speaker;
  }
  if (name == "Mic") {
    return Icon::kSound_Mic;
  }
  if (name == "Approved") {
    return Icon::kStamp_Approved;
  }
  if (name == "Experimental") {
    return Icon::kStamp_Experimental;
  }
  if (name == "NotApproved") {
    return Icon::kStamp_NotApproved;
  }
  if (name == "AsIs") {
    return Icon::kStamp_AsIs;
  }
  if (name == "Expired") {
    return Icon::kStamp_Expired;
  }
  if (name == "NotForPublicRelease") {
    return Icon::kStamp_NotForPublicRelease;
  }
  if (name == "Confidential") {
    return Icon::kStamp_Confidential;
  }
  if (name == "Final") {
    return Icon::kStamp_Final;
  }
  if (name == "Sold") {
    return Icon::kStamp_Sold;
  }
  if (name == "Departmental") {
    return Icon::kStamp_Departmental;
  }
  if (name == "ForComment") {
    return Icon::kStamp_ForComment;
  }
  if (name == "TopSecret") {
    return Icon::kStamp_TopSecret;
  }
  if (name == "Draft") {
    return Icon::kStamp_Draft;
  }
  if (name == "ForPublicRelease") {
    return Icon::kStamp_ForPublicRelease;
  }
  if (name == "Completed") {
    return Icon::kStamp_Completed;
  }
  if (name == "Void") {
    return Icon::kStamp_Void;
  }
  if (name == "PreliminaryResults") {
    return Icon::kStamp_PreliminaryResults;
  }
  if (name == "InformationOnly") {
    return Icon::kStamp_InformationOnly;
  }
  if (name == "Rejected") {
    return Icon::kStamp_Rejected;
  }
  if (name == "Witness") {
    return Icon::kStamp_Witness;
  }
  if (name == "InitialHere") {
    return Icon::kStamp_InitialHere;
  }
  if (name == "SignHere") {
    return Icon::kStamp_SignHere;
  }
  if (name == "Accepted") {
    return Icon::kStamp_Accepted;
  }
  if (name == "Custom") {
    return Icon::kStamp_Custom;
  }
  if (name == "Image") {
    return Icon::kStamp_Image;
  }
  // Try stripping Adobe SB/SH prefix and re-matching.
  if (name.GetLength() > 2) {
    ByteString prefix = name.First(2);
    if (prefix == "SB" || prefix == "SH") {
      Icon result = StringToIcon(name.Substr(2));
      if (result != Icon::kUnknown && result != Icon::kStamp_Custom) {
        return result;
      }
    }
  }
  return Icon::kStamp_Custom;
}

ByteString CPDF_Annot::IconToString(CPDF_Annot::Icon icon) {
  switch (icon) {
    case Icon::kText_Comment:
      return "Comment";
    case Icon::kText_Key:
      return "Key";
    case Icon::kText_Note:
      return "Note";
    case Icon::kText_Help:
      return "Help";
    case Icon::kText_NewParagraph:
      return "NewParagraph";
    case Icon::kText_Paragraph:
      return "Paragraph";
    case Icon::kText_Insert:
      return "Insert";
    case Icon::kFile_Graph:
      return "Graph";
    case Icon::kFile_PushPin:
      return "PushPin";
    case Icon::kFile_Paperclip:
      return "Paperclip";
    case Icon::kFile_Tag:
      return "Tag";
    case Icon::kSound_Speaker:
      return "Speaker";
    case Icon::kSound_Mic:
      return "Mic";
    case Icon::kStamp_Approved:
      return "Approved";
    case Icon::kStamp_Experimental:
      return "Experimental";
    case Icon::kStamp_NotApproved:
      return "NotApproved";
    case Icon::kStamp_AsIs:
      return "AsIs";
    case Icon::kStamp_Expired:
      return "Expired";
    case Icon::kStamp_NotForPublicRelease:
      return "NotForPublicRelease";
    case Icon::kStamp_Confidential:
      return "Confidential";
    case Icon::kStamp_Final:
      return "Final";
    case Icon::kStamp_Sold:
      return "Sold";
    case Icon::kStamp_Departmental:
      return "Departmental";
    case Icon::kStamp_ForComment:
      return "ForComment";
    case Icon::kStamp_TopSecret:
      return "TopSecret";
    case Icon::kStamp_Draft:
      return "Draft";
    case Icon::kStamp_ForPublicRelease:
      return "ForPublicRelease";
    case Icon::kStamp_Completed:
      return "Completed";
    case Icon::kStamp_Void:
      return "Void";
    case Icon::kStamp_PreliminaryResults:
      return "PreliminaryResults";
    case Icon::kStamp_InformationOnly:
      return "InformationOnly";
    case Icon::kStamp_Rejected:
      return "Rejected";
    case Icon::kStamp_Witness:
      return "Witness";
    case Icon::kStamp_InitialHere:
      return "InitialHere";
    case Icon::kStamp_SignHere:
      return "SignHere";
    case Icon::kStamp_Accepted:
      return "Accepted";
    case Icon::kStamp_Custom:
      return "Custom";
    case Icon::kStamp_Image:
      return "Image";
    case Icon::kUnknown:
      break;
  }
  return ByteString();
}

// static
CPDF_Annot::ReplyType CPDF_Annot::StringToReplyType(const ByteString& name) {
  // Per PDF spec, if RT is missing, the default is /R (Reply).
  if (name.IsEmpty() || name == "R") {
    return ReplyType::kReply;
  }
  if (name == "Group") {
    return ReplyType::kGroup;
  }
  return ReplyType::kUnknown;
}

// static
ByteString CPDF_Annot::ReplyTypeToString(CPDF_Annot::ReplyType rt) {
  switch (rt) {
    case ReplyType::kReply:
      return "R";
    case ReplyType::kGroup:
      return "Group";
    case ReplyType::kUnknown:
      break;
  }
  return ByteString();
}

ByteString CPDF_Annot::LineEndingToString(CPDF_Annot::LineEnding le) {
  switch (le) {
    case LineEnding::kNone:
      return "None";
    case LineEnding::kSquare:
      return "Square";
    case LineEnding::kCircle:
      return "Circle";
    case LineEnding::kDiamond:
      return "Diamond";
    case LineEnding::kOpenArrow:
      return "OpenArrow";
    case LineEnding::kClosedArrow:
      return "ClosedArrow";
    case LineEnding::kButt:
      return "Butt";
    case LineEnding::kROpenArrow:
      return "ROpenArrow";
    case LineEnding::kRClosedArrow:
      return "RClosedArrow";
    case LineEnding::kSlash:
      return "Slash";
    case LineEnding::kUnknown:
      break;
  }
  return ByteString();
}

CPDF_Annot::LineEnding CPDF_Annot::StringToLineEnding(const ByteString& n) {
  if (n == "None") {
    return LineEnding::kNone;
  }
  if (n == "Square") {
    return LineEnding::kSquare;
  }
  if (n == "Circle") {
    return LineEnding::kCircle;
  }
  if (n == "Diamond") {
    return LineEnding::kDiamond;
  }
  if (n == "OpenArrow") {
    return LineEnding::kOpenArrow;
  }
  if (n == "ClosedArrow") {
    return LineEnding::kClosedArrow;
  }
  if (n == "Butt") {
    return LineEnding::kButt;
  }
  if (n == "ROpenArrow") {
    return LineEnding::kROpenArrow;
  }
  if (n == "RClosedArrow") {
    return LineEnding::kRClosedArrow;
  }
  if (n == "Slash") {
    return LineEnding::kSlash;
  }
  return LineEnding::kUnknown;
}

CPDF_Annot::StandardFont CPDF_Annot::StringToStandardFont(
    const ByteString& name) {
  // Full canonical names (PDF Reference, Table 5.17)
  if (name == "Courier" || name == "Cour") {
    return StandardFont::kCourier;
  }
  if (name == "Courier-Bold" || name == "CoBo") {
    return StandardFont::kCourier_Bold;
  }
  if (name == "Courier-BoldOblique") {
    return StandardFont::kCourier_BoldOblique;
  }
  if (name == "Courier-Oblique" || name == "CoOb") {
    return StandardFont::kCourier_Oblique;
  }
  if (name == "Helvetica" || name == "Helv") {
    return StandardFont::kHelvetica;
  }
  if (name == "Helvetica-Bold" || name == "HeBo") {
    return StandardFont::kHelvetica_Bold;
  }
  if (name == "Helvetica-BoldOblique") {
    return StandardFont::kHelvetica_BoldOblique;
  }
  if (name == "Helvetica-Oblique") {
    return StandardFont::kHelvetica_Oblique;
  }
  if (name == "Times-Roman" || name == "TiRo") {
    return StandardFont::kTimes_Roman;
  }
  if (name == "Times-Bold" || name == "TiBo") {
    return StandardFont::kTimes_Bold;
  }
  if (name == "Times-BoldItalic") {
    return StandardFont::kTimes_BoldItalic;
  }
  if (name == "Times-Italic" || name == "TiIt") {
    return StandardFont::kTimes_Italic;
  }
  if (name == "Symbol" || name == "Symb") {
    return StandardFont::kSymbol;
  }
  if (name == "ZapfDingbats" || name == "ZaDb") {
    return StandardFont::kZapfDingbats;
  }
  return StandardFont::kUnknown;
}

ByteString CPDF_Annot::StandardFontToString(CPDF_Annot::StandardFont font) {
  switch (font) {
    case StandardFont::kCourier:
      return "Courier";
    case StandardFont::kCourier_Bold:
      return "Courier-Bold";
    case StandardFont::kCourier_BoldOblique:
      return "Courier-BoldOblique";
    case StandardFont::kCourier_Oblique:
      return "Courier-Oblique";
    case StandardFont::kHelvetica:
      return "Helvetica";
    case StandardFont::kHelvetica_Bold:
      return "Helvetica-Bold";
    case StandardFont::kHelvetica_BoldOblique:
      return "Helvetica-BoldOblique";
    case StandardFont::kHelvetica_Oblique:
      return "Helvetica-Oblique";
    case StandardFont::kTimes_Roman:
      return "Times-Roman";
    case StandardFont::kTimes_Bold:
      return "Times-Bold";
    case StandardFont::kTimes_BoldItalic:
      return "Times-BoldItalic";
    case StandardFont::kTimes_Italic:
      return "Times-Italic";
    case StandardFont::kSymbol:
      return "Symbol";
    case StandardFont::kZapfDingbats:
      return "ZapfDingbats";
    case StandardFont::kUnknown:
      break;
  }
  return ByteString();
}

// static
CPDF_Annot::BorderStyle CPDF_Annot::StringToBorderStyle(
    const ByteString& sStyle) {
  if (sStyle == "S") {
    return CPDF_Annot::BorderStyle::kSolid;
  }
  if (sStyle == "D") {
    return CPDF_Annot::BorderStyle::kDashed;
  }
  if (sStyle == "B") {
    return CPDF_Annot::BorderStyle::kBeveled;
  }
  if (sStyle == "I") {
    return CPDF_Annot::BorderStyle::kInset;
  }
  if (sStyle == "U") {
    return CPDF_Annot::BorderStyle::kUnderline;
  }
  return CPDF_Annot::BorderStyle::kUnknown;
}

ByteString CPDF_Annot::BorderStyleToString(CPDF_Annot::BorderStyle nStyle) {
  switch (nStyle) {
    case CPDF_Annot::BorderStyle::kSolid:
      return "S";
    case CPDF_Annot::BorderStyle::kDashed:
      return "D";
    case CPDF_Annot::BorderStyle::kBeveled:
      return "B";
    case CPDF_Annot::BorderStyle::kInset:
      return "I";
    case CPDF_Annot::BorderStyle::kUnderline:
      return "U";
    case CPDF_Annot::BorderStyle::kUnknown:
      break;
  }
  return ByteString();
}

// static
size_t CPDF_Annot::QuadPointCount(const CPDF_Array* pArray) {
  return pArray->size() / 8;
}

bool CPDF_Annot::DrawAppearance(CPDF_Page* pPage,
                                CFX_RenderDevice* pDevice,
                                const CFX_Matrix& mtUser2Device,
                                AppearanceMode mode) {
  if (!ShouldDrawAnnotation()) {
    return false;
  }

  if (!CanGenerateEphemeralAP()) {
    // It might happen that by the time this annotation instance was created,
    // it was flagged as "hidden" (e.g. /F 2), and hence CPDF_GenerateAP decided
    // to not "generate" its AP.
    // If for a reason the object is no longer hidden, but still does not have
    // its "AP" generated, generate it now.
    GenerateAPIfNeeded();
  }

  CFX_Matrix matrix;
  CPDF_Form* pForm = AnnotGetMatrix(pPage, this, mode, mtUser2Device, &matrix);
  if (!pForm) {
    return false;
  }

  CPDF_RenderContext context(pPage->GetDocument(),
                             pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(
                                 pPage->GetPageResources().Get())),
                             pPage->GetPageImageCache());
  context.AppendLayer(pForm, matrix);
  context.Render(pDevice, nullptr, nullptr, nullptr);
  return true;
}

bool CPDF_Annot::DrawInContext(CPDF_Page* pPage,
                               CPDF_RenderContext* context,
                               const CFX_Matrix& mtUser2Device,
                               AppearanceMode mode) {
  if (!ShouldDrawAnnotation()) {
    return false;
  }

  if (!CanGenerateEphemeralAP()) {
    // It might happen that by the time this annotation instance was created,
    // it was flagged as "hidden" (e.g. /F 2), and hence CPDF_GenerateAP decided
    // to not "generate" its AP.
    // If for a reason the object is no longer hidden, but still does not have
    // its "AP" generated, generate it now.
    GenerateAPIfNeeded();
  }

  CFX_Matrix matrix;
  CPDF_Form* pForm = AnnotGetMatrix(pPage, this, mode, mtUser2Device, &matrix);
  if (!pForm) {
    return false;
  }

  context->AppendLayer(pForm, matrix);
  return true;
}

void CPDF_Annot::DrawBorder(CFX_RenderDevice* pDevice,
                            const CFX_Matrix* pUser2Device) {
  if (GetSubtype() == CPDF_Annot::Subtype::POPUP) {
    return;
  }

  uint32_t annot_flags = GetFlags();
  if (annot_flags & pdfium::annotation_flags::kHidden) {
    return;
  }

#if BUILDFLAG(IS_WIN)
  bool is_printing = pDevice->GetDeviceType() == DeviceType::kPrinter;
  if (is_printing && (annot_flags & pdfium::annotation_flags::kPrint) == 0) {
    return;
  }
#else
  const bool is_printing = false;
#endif

  if (!is_printing && (annot_flags & pdfium::annotation_flags::kNoView)) {
    return;
  }

  RetainPtr<const CPDF_Dictionary> pBS = annot_dict_->GetDictFor("BS");
  char style_char;
  float width;
  RetainPtr<const CPDF_Array> pDashArray;
  if (!pBS) {
    RetainPtr<const CPDF_Array> pBorderArray =
        annot_dict_->GetArrayFor(pdfium::annotation::kBorder);
    style_char = 'S';
    if (pBorderArray) {
      width = pBorderArray->GetFloatAt(2);
      if (pBorderArray->size() == 4) {
        pDashArray = pBorderArray->GetArrayAt(3);
        if (!pDashArray) {
          return;
        }
        size_t nLen = pDashArray->size();
        size_t i = 0;
        for (; i < nLen; ++i) {
          RetainPtr<const CPDF_Object> pObj = pDashArray->GetDirectObjectAt(i);
          if (pObj && pObj->GetInteger()) {
            break;
          }
        }
        if (i == nLen) {
          return;
        }
        style_char = 'D';
      }
    } else {
      width = 1;
    }
  } else {
    ByteString style = pBS->GetByteStringFor("S");
    pDashArray = pBS->GetArrayFor("D");
    style_char = style[0];
    width = pBS->GetFloatFor("W");
  }
  if (width <= 0) {
    return;
  }
  RetainPtr<const CPDF_Array> pColor =
      annot_dict_->GetArrayFor(pdfium::annotation::kC);
  uint32_t argb = 0xff000000;
  if (pColor) {
    int R = static_cast<int32_t>(pColor->GetFloatAt(0) * 255);
    int G = static_cast<int32_t>(pColor->GetFloatAt(1) * 255);
    int B = static_cast<int32_t>(pColor->GetFloatAt(2) * 255);
    argb = ArgbEncode(0xff, R, G, B);
  }
  CFX_GraphStateData graph_state;
  graph_state.set_line_width(width);
  if (style_char == 'U') {
    // TODO(https://crbug.com/237527): Handle the "Underline" border style
    // instead of drawing the rectangle border.
    return;
  }

  if (style_char == 'D') {
    if (pDashArray) {
      std::vector<float> dash_array =
          ReadArrayElementsToVector(pDashArray.Get(), pDashArray->size());
      if (dash_array.size() % 2) {
        dash_array.push_back(dash_array.back());
      }
      graph_state.set_dash_array(std::move(dash_array));
    } else {
      graph_state.set_dash_array({3.0f, 3.0f});
    }
  }

  CFX_FloatRect rect = GetRect();
  rect.Deflate(width / 2, width / 2);

  CFX_Path path;
  path.AppendFloatRect(rect);
  pDevice->DrawPath(path, pUser2Device, &graph_state, argb, argb,
                    CFX_FillRenderOptions());
}
