// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

// EmbedPDF: the appearance generator's shared drawing: colours, borders and
// dashes, blend modes, geometry, resources, and where an appearance is written
// (the annotation, or memory only).
//
// See docs/embedpdf/fork-ledger.md, rule own-appearance-generator. EmbedPDF's
// changes are Copyright 2026 CloudPDF LTD.

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "constants/annotation_common.h"
#include "constants/transparency.h"
#include "core/fxcrt/stream_write_utils.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_paintedbounds.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_annot_rotation.h"
#include "core/fpdfdoc/cpdf_appearancegenerator_internal.h"
#include "core/fpdfdoc/cpdf_color_utils.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/notreached.h"
#include "core/fxge/cfx_renderdevice.h"

using appearance_generator::AnnotationDimensionsAndColor;
using appearance_generator::APGenerationTarget;
using appearance_generator::AppearanceCharacteristics;
using appearance_generator::BorderStyleInfo;
using appearance_generator::GenerateAPDict;
using appearance_generator::GenerateColorAP;
using appearance_generator::GetAppearanceCharacteristics;
using appearance_generator::kDefaultDash;
using appearance_generator::kGSDictName;
using appearance_generator::PaintOperation;
using appearance_generator::ShapeRotationInfo;

namespace {

// A dash array's values, at most ten, as the dash operator takes them.
std::vector<float> DashValues(const CPDF_Array* dash_array) {
  std::vector<float> values;
  if (!dash_array) {
    return values;
  }
  const size_t count = std::min<size_t>(dash_array->size(), 10);
  for (size_t i = 0; i < count; ++i) {
    values.push_back(dash_array->GetFloatAt(i));
  }
  return values;
}

// The dash operator for |values|; empty when there are none.
ByteString DashOperator(const std::vector<float>& values) {
  if (values.empty()) {
    return ByteString();
  }
  fxcrt::ostringstream dash_stream;
  dash_stream << "[";
  for (float value : values) {
    WriteFloat(dash_stream, value) << " ";
  }
  dash_stream << "] 0 d\n";
  return ByteString(dash_stream);
}

// /RD as its four numbers, in rectangle order (left, bottom, right, top), as
// Acrobat writes it and PDFium reads it.
CFX_FloatRect GetRectDifferences(const CPDF_Dictionary* annot_dict) {
  RetainPtr<const CPDF_Array> rd = annot_dict->GetArrayFor("RD");
  if (!rd || rd->size() < 4) {
    return CFX_FloatRect();
  }
  return CFX_FloatRect(rd->GetFloatAt(0), rd->GetFloatAt(1), rd->GetFloatAt(2),
                       rd->GetFloatAt(3));
}

RetainPtr<CPDF_Dictionary> BuildAPStreamDict(
    const CPDF_Dictionary* annot_dict,
    RetainPtr<CPDF_Dictionary> resource_dict,
    bool is_text_markup_annotation,
    const CFX_Matrix& matrix,
    const CFX_FloatRect& bbox_override) {
  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  stream_dict->SetNewFor<CPDF_Number>("FormType", 1);
  stream_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  stream_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  stream_dict->SetMatrixFor("Matrix", matrix);

  CFX_FloatRect rect = !bbox_override.IsEmpty() ? bbox_override
                       : is_text_markup_annotation
                           ? CPDF_Annot::BoundingRectFromQuadPoints(annot_dict)
                           : annot_dict->GetRectFor(pdfium::annotation::kRect);
  stream_dict->SetRectFor("BBox", rect);
  stream_dict->SetFor("Resources", std::move(resource_dict));
  return stream_dict;
}

// The box around what `content` paints in its own space, as a form with
// `resources` draws it: parsed without a /Matrix and without the /BBox clip.
CFX_FloatRect MeasurePaintedBox(CPDF_Document* doc,
                                fxcrt::ostringstream* content,
                                RetainPtr<CPDF_Dictionary> resources) {
  auto stream =
      pdfium::MakeRetain<CPDF_Stream>(pdfium::MakeRetain<CPDF_Dictionary>());
  stream->SetDataFromStringstream(content);
  CPDF_Form form(doc, std::move(resources), std::move(stream));
  form.ParseContent();
  return GetPaintedBounds(form);
}

// Points: how far a measured edge may sit past the shape it's drawn about and
// still be that edge, as a border drawn inside the shape is.
constexpr float kMeasureNoise = 1e-3f;

// An appearance drawn about `footprint` in the form's space (a box kind's
// shape, or a line's points) and put on the page by `matrix`. The form's
// /BBox is the box around the footprint and all the drawing paints (a
// border's bumps, a line's width and endings, a caption). A persistent
// annotation's /Rect is where `matrix` puts that box, which places the
// appearance at scale 1. Returns the /BBox, or nothing when the stream isn't
// set.
std::optional<CFX_FloatRect> GenerateAndSetFramedAPDict(
    APGenerationTarget* target,
    const CPDF_Dictionary* annot_dict,
    fxcrt::ostringstream* app_stream,
    RetainPtr<CPDF_Dictionary> resource_dict,
    const CFX_Matrix& matrix,
    const CFX_FloatRect& footprint) {
  CFX_FloatRect shape = footprint;
  shape.Normalize();
  CFX_FloatRect frame = shape;
  const CFX_FloatRect painted =
      MeasurePaintedBox(target->doc, app_stream, resource_dict);
  if (!painted.IsEmpty()) {
    if (painted.left < shape.left - kMeasureNoise) {
      frame.left = painted.left;
    }
    if (painted.bottom < shape.bottom - kMeasureNoise) {
      frame.bottom = painted.bottom;
    }
    if (painted.right > shape.right + kMeasureNoise) {
      frame.right = painted.right;
    }
    if (painted.top > shape.top + kMeasureNoise) {
      frame.top = painted.top;
    }
  }
  if (!GenerateAPDict(target, annot_dict, app_stream, std::move(resource_dict),
                      /*is_text_markup_annotation=*/false, matrix, frame)) {
    return std::nullopt;
  }
  if (target->IsPersistent()) {
    CFX_FloatRect rect = matrix.TransformRect(frame);
    rect.Normalize();
    target->persistent_annot_dict->SetRectFor(pdfium::annotation::kRect, rect);
  }
  return frame;
}

// The box around `points`; empty when there are none.
CFX_FloatRect BoundsOfPoints(pdfium::span<const CFX_PointF> points) {
  if (points.empty()) {
    return CFX_FloatRect();
  }
  CFX_FloatRect bounds(points.front().x, points.front().y, points.front().x,
                       points.front().y);
  for (const CFX_PointF& point : points) {
    bounds.UpdateRect(point);
  }
  return bounds;
}

}  // namespace

namespace appearance_generator {

// Return a unit‑length copy of `v`.  If the vector has zero length, fall back
// to the X axis so that later maths cannot explode.
CFX_PointF UnitVector(const CFX_PointF& v) {
  float len = std::hypot(v.x, v.y);
  if (len <= 0.0f) {
    return CFX_PointF(1.0f, 0.0f);
  }
  return CFX_PointF(v.x / len, v.y / len);
}

float DotProduct(const CFX_PointF& lhs, const CFX_PointF& rhs) {
  return lhs.x * rhs.x + lhs.y * rhs.y;
}

float CrossProduct(const CFX_PointF& lhs, const CFX_PointF& rhs) {
  return lhs.x * rhs.y - lhs.y * rhs.x;
}

float VectorLength(const CFX_PointF& vector) {
  return std::hypot(vector.x, vector.y);
}

bool IsFinitePoint(const CFX_PointF& point) {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

CFX_PointF Midpoint(const CFX_PointF& first, const CFX_PointF& second) {
  return 0.5f * (first + second);
}

ByteString BlendModeToPDFName(BlendMode bm) {
  switch (bm) {
    case BlendMode::kNormal:
      return ByteString(pdfium::transparency::kNormal);
    case BlendMode::kMultiply:
      return ByteString(pdfium::transparency::kMultiply);
    case BlendMode::kScreen:
      return ByteString(pdfium::transparency::kScreen);
    case BlendMode::kOverlay:
      return ByteString(pdfium::transparency::kOverlay);
    case BlendMode::kDarken:
      return ByteString(pdfium::transparency::kDarken);
    case BlendMode::kLighten:
      return ByteString(pdfium::transparency::kLighten);
    case BlendMode::kColorDodge:
      return ByteString(pdfium::transparency::kColorDodge);
    case BlendMode::kColorBurn:
      return ByteString(pdfium::transparency::kColorBurn);
    case BlendMode::kHardLight:
      return ByteString(pdfium::transparency::kHardLight);
    case BlendMode::kSoftLight:
      return ByteString(pdfium::transparency::kSoftLight);
    case BlendMode::kDifference:
      return ByteString(pdfium::transparency::kDifference);
    case BlendMode::kExclusion:
      return ByteString(pdfium::transparency::kExclusion);
    case BlendMode::kHue:
      return ByteString(pdfium::transparency::kHue);
    case BlendMode::kSaturation:
      return ByteString(pdfium::transparency::kSaturation);
    case BlendMode::kColor:
      return ByteString(pdfium::transparency::kColor);
    case BlendMode::kLuminosity:
      return ByteString(pdfium::transparency::kLuminosity);
  }
  return ByteString(pdfium::transparency::kNormal);
}

BlendMode DefaultBlendModeFor(CPDF_Annot::Subtype subtype) {
  switch (subtype) {
    case CPDF_Annot::Subtype::HIGHLIGHT:
      return BlendMode::kMultiply;
    default:
      return BlendMode::kNormal;
  }
}

BorderStyleInfo GetBorderStyleInfo(const CPDF_Dictionary* border_style_dict) {
  BorderStyleInfo border_style_info;
  if (!border_style_dict) {
    return border_style_info;
  }

  if (border_style_dict->KeyExist("W")) {
    border_style_info.width = border_style_dict->GetFloatFor("W");
  }

  const ByteString border_style_string =
      border_style_dict->GetByteStringFor("S");
  if (border_style_string.GetLength()) {
    switch (border_style_string[0]) {
      case 'S':
        border_style_info.style = BorderStyle::kSolid;
        break;
      case 'D':
        border_style_info.style = BorderStyle::kDash;
        break;
      case 'B':
        border_style_info.style = BorderStyle::kBeveled;
        border_style_info.width *= 2;
        break;
      case 'I':
        border_style_info.style = BorderStyle::kInset;
        border_style_info.width *= 2;
        break;
      case 'U':
        border_style_info.style = BorderStyle::kUnderline;
        break;
    }
  }

  std::vector<float> dash =
      DashValues(border_style_dict->GetArrayFor("D").Get());
  if (!dash.empty()) {
    border_style_info.dash_pattern = std::move(dash);
  }

  return border_style_info;
}

AnnotationDimensionsAndColor GetAnnotationDimensionsAndColor(
    const CPDF_Dictionary* annot_dict) {
  const AppearanceCharacteristics appearance_characteristics =
      GetAppearanceCharacteristics(annot_dict->GetDictFor("MK"));
  const CFX_FloatRect annot_rect =
      annot_dict->GetRectFor(pdfium::annotation::kRect);

  CFX_FloatRect bbox_rect;
  CFX_Matrix matrix;
  switch (appearance_characteristics.rotation % 360) {
    case 0:
      bbox_rect = CFX_FloatRect(0, 0, annot_rect.right - annot_rect.left,
                                annot_rect.top - annot_rect.bottom);
      break;
    case 90:
      matrix = CFX_Matrix(0, 1, -1, 0, annot_rect.right - annot_rect.left, 0);
      bbox_rect = CFX_FloatRect(0, 0, annot_rect.top - annot_rect.bottom,
                                annot_rect.right - annot_rect.left);
      break;
    case 180:
      matrix = CFX_Matrix(-1, 0, 0, -1, annot_rect.right - annot_rect.left,
                          annot_rect.top - annot_rect.bottom);
      bbox_rect = CFX_FloatRect(0, 0, annot_rect.right - annot_rect.left,
                                annot_rect.top - annot_rect.bottom);
      break;
    case 270:
      matrix = CFX_Matrix(0, -1, 1, 0, 0, annot_rect.top - annot_rect.bottom);
      bbox_rect = CFX_FloatRect(0, 0, annot_rect.top - annot_rect.bottom,
                                annot_rect.right - annot_rect.left);
      break;
  }

  return {
      .bbox = bbox_rect,
      .matrix = matrix,
      .border_color = appearance_characteristics.border_color,
      .background_color = appearance_characteristics.background_color,
  };
}

ShapeRotationInfo GetShapeRotationInfo(const CPDF_Dictionary* annot_dict) {
  ShapeRotationInfo info;
  info.is_rotated = false;
  info.matrix = CFX_Matrix();
  // Upright, the shape is /Rect less /RD: /Rect also holds what the drawing
  // adds around it (a cloudy border's bumps, a callout's line).
  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  rect.Normalize();
  const CFX_FloatRect rd = GetRectDifferences(annot_dict);
  const CFX_FloatRect shape(rect.left + rd.left, rect.bottom + rd.bottom,
                            rect.right - rd.right, rect.top - rd.top);
  info.bbox = shape.left <= shape.right && shape.bottom <= shape.top ? shape
                                                                     : rect;

  // The engine records the turn the annotation reads before it draws it.
  const std::optional<fpdfdoc::AnnotRotation> rotation =
      fpdfdoc::GetRecordedAnnotRotation(annot_dict);
  if (!rotation) {
    return info;
  }
  info.is_rotated = true;
  info.bbox = rotation->box;
  // Rotate about the centre of the unrotated box.
  info.matrix = fpdfdoc::TurnAbout(fpdfdoc::BoxCenter(rotation->box),
                                   rotation->degrees);
  return info;
}

ByteString GenerateColorAP(const CFX_Color& color, PaintOperation operation) {
  fxcrt::ostringstream color_stream;
  switch (color.nColorType) {
    case CFX_Color::Type::kRGB:
      WriteFloat(color_stream, color.fColor1) << " ";
      WriteFloat(color_stream, color.fColor2) << " ";
      WriteFloat(color_stream, color.fColor3) << " ";
      color_stream << (operation == PaintOperation::kStroke ? "RG" : "rg")
                   << "\n";
      return ByteString(color_stream);
    case CFX_Color::Type::kGray:
      WriteFloat(color_stream, color.fColor1) << " ";
      color_stream << (operation == PaintOperation::kStroke ? "G" : "g")
                   << "\n";
      return ByteString(color_stream);
    case CFX_Color::Type::kCMYK:
      WriteFloat(color_stream, color.fColor1) << " ";
      WriteFloat(color_stream, color.fColor2) << " ";
      WriteFloat(color_stream, color.fColor3) << " ";
      WriteFloat(color_stream, color.fColor4) << " ";
      color_stream << (operation == PaintOperation::kStroke ? "K" : "k")
                   << "\n";
      return ByteString(color_stream);
    case CFX_Color::Type::kTransparent:
      return ByteString();
  }
  NOTREACHED();
}

ByteString GenerateBorderAP(const CFX_FloatRect& rect,
                            const BorderStyleInfo& border_style_info,
                            const CFX_Color& border_color) {
  const float width = border_style_info.width;
  if (width <= 0) {
    return ByteString();
  }

  fxcrt::ostringstream app_stream;
  const float left = rect.left;
  const float bottom = rect.bottom;
  const float right = rect.right;
  const float top = rect.top;
  const float half_width = width / 2.0f;
  switch (border_style_info.style) {
    case BorderStyle::kSolid: {
      ByteString color_string =
          GenerateColorAP(border_color, PaintOperation::kFill);
      if (color_string.GetLength() > 0) {
        app_stream << color_string;
        WriteRect(app_stream, rect) << " re\n";
        CFX_FloatRect inner_rect = rect;
        inner_rect.Deflate(width, width);
        WriteRect(app_stream, inner_rect) << " re f*\n";
      }
      return ByteString(app_stream);
    }
    case BorderStyle::kDash: {
      ByteString color_string =
          GenerateColorAP(border_color, PaintOperation::kStroke);
      if (color_string.GetLength() > 0) {
        app_stream << color_string;
        WriteFloat(app_stream, width) << " w\n"
                                      << DashOperator(
                                             border_style_info.dash_pattern);
        WritePoint(app_stream, {left + half_width, bottom + half_width})
            << " m\n";
        WritePoint(app_stream, {left + half_width, top - half_width}) << " l\n";
        WritePoint(app_stream, {right - half_width, top - half_width})
            << " l\n";
        WritePoint(app_stream, {right - half_width, bottom + half_width})
            << " l\n";
        WritePoint(app_stream, {left + half_width, bottom + half_width})
            << " l S\n";
      }
      return ByteString(app_stream);
    }
    case BorderStyle::kBeveled:
    case BorderStyle::kInset: {
      const float left_top_gray_value =
          border_style_info.style == BorderStyle::kBeveled ? 1.0f : 0.5f;
      app_stream << GenerateColorAP(CFX_Color::MakeGray(left_top_gray_value),
                                    PaintOperation::kFill);
      WritePoint(app_stream, {left + half_width, bottom + half_width})
          << " m\n";
      WritePoint(app_stream, {left + half_width, top - half_width}) << " l\n";
      WritePoint(app_stream, {right - half_width, top - half_width}) << " l\n";
      WritePoint(app_stream, {right - width, top - width}) << " l\n";
      WritePoint(app_stream, {left + width, top - width}) << " l\n";
      WritePoint(app_stream, {left + width, bottom + width}) << " l f\n";

      const float right_bottom_gray_value =
          border_style_info.style == BorderStyle::kBeveled ? 0.5f : 0.75f;
      app_stream << GenerateColorAP(
          CFX_Color::MakeGray(right_bottom_gray_value), PaintOperation::kFill);
      WritePoint(app_stream, {right - half_width, top - half_width}) << " m\n";
      WritePoint(app_stream, {right - half_width, bottom + half_width})
          << " l\n";
      WritePoint(app_stream, {left + half_width, bottom + half_width})
          << " l\n";
      WritePoint(app_stream, {left + width, bottom + width}) << " l\n";
      WritePoint(app_stream, {right - width, bottom + width}) << " l\n";
      WritePoint(app_stream, {right - width, top - width}) << " l f\n";

      ByteString color_string =
          GenerateColorAP(border_color, PaintOperation::kFill);
      if (color_string.GetLength() > 0) {
        app_stream << color_string;
        WriteRect(app_stream, rect) << " re\n";
        CFX_FloatRect inner_rect = rect;
        inner_rect.Deflate(half_width, half_width);
        WriteRect(app_stream, inner_rect) << " re f*\n";
      }
      return ByteString(app_stream);
    }
    case BorderStyle::kUnderline: {
      ByteString color_string =
          GenerateColorAP(border_color, PaintOperation::kStroke);
      if (color_string.GetLength() > 0) {
        app_stream << color_string;
        WriteFloat(app_stream, width) << " w\n";
        WritePoint(app_stream, {left, bottom + half_width}) << " m\n";
        WritePoint(app_stream, {right, bottom + half_width}) << " l S\n";
      }
      return ByteString(app_stream);
    }
  }
  NOTREACHED();
}

ByteString GetColorStringWithDefault(const CPDF_Array* color_array,
                                     const CFX_Color& default_color,
                                     PaintOperation operation) {
  if (color_array) {
    CFX_Color color = fpdfdoc::CFXColorFromArray(*color_array);
    return GenerateColorAP(color, operation);
  }

  return GenerateColorAP(default_color, operation);
}

float GetBorderWidth(const CPDF_Dictionary* dict) {
  RetainPtr<const CPDF_Dictionary> border_style_dict = dict->GetDictFor("BS");
  if (border_style_dict) {
    return border_style_dict->KeyExist("W")
               ? border_style_dict->GetFloatFor("W")
               : 1.0f;
  }

  auto border_array = dict->GetArrayFor(pdfium::annotation::kBorder);
  if (border_array && border_array->size() > 2) {
    return border_array->GetFloatAt(2);
  }

  return 1.0f;
}

// The dash operator a border draws with: a dashed /BS draws its /D, or the
// default dash when it names none; a /Border draws its own dash array.
ByteString GetDashPatternString(const CPDF_Dictionary* dict) {
  RetainPtr<const CPDF_Dictionary> border_style_dict = dict->GetDictFor("BS");
  if (border_style_dict) {
    if (border_style_dict->GetByteStringFor("S") != "D") {
      return ByteString();
    }
    std::vector<float> dash =
        DashValues(border_style_dict->GetArrayFor("D").Get());
    if (dash.empty()) {
      dash.push_back(kDefaultDash);
    }
    return DashOperator(dash);
  }

  RetainPtr<const CPDF_Array> border_array =
      dict->GetArrayFor(pdfium::annotation::kBorder);
  if (border_array && border_array->size() == 4) {
    return DashOperator(DashValues(border_array->GetArrayAt(3).Get()));
  }
  return ByteString();
}

ByteString GetPaintOperatorString(bool is_stroke_rect, bool is_fill_rect) {
  if (is_stroke_rect) {
    return is_fill_rect ? "b" : "s";
  }
  return is_fill_rect ? "f" : "n";
}

RetainPtr<CPDF_Dictionary> GenerateExtGStateDict(
    const CPDF_Dictionary& annot_dict,
    const ByteString& blend_mode) {
  auto gs_dict =
      pdfium::MakeRetain<CPDF_Dictionary>(annot_dict.GetByteStringPool());
  gs_dict->SetNewFor<CPDF_Name>("Type", "ExtGState");

  float opacity = annot_dict.KeyExist("CA") ? annot_dict.GetFloatFor("CA") : 1;
  gs_dict->SetNewFor<CPDF_Number>("CA", opacity);
  gs_dict->SetNewFor<CPDF_Number>("ca", opacity);
  gs_dict->SetNewFor<CPDF_Boolean>("AIS", false);
  gs_dict->SetNewFor<CPDF_Name>("BM", blend_mode);

  auto resources_dict =
      pdfium::MakeRetain<CPDF_Dictionary>(annot_dict.GetByteStringPool());
  resources_dict->SetFor(kGSDictName, std::move(gs_dict));
  return resources_dict;
}

RetainPtr<CPDF_Dictionary> GenerateResourcesDict(
    CPDF_Document* doc,
    RetainPtr<CPDF_Dictionary> gs_dict,
    RetainPtr<CPDF_Dictionary> font_resource_dict) {
  auto resources_dict = doc->New<CPDF_Dictionary>();
  if (gs_dict) {
    resources_dict->SetFor("ExtGState", gs_dict);
  }
  if (font_resource_dict) {
    resources_dict->SetFor("Font", font_resource_dict);
  }
  return resources_dict;
}

bool GenerateAPDict(APGenerationTarget* target,
                    const CPDF_Dictionary* annot_dict,
                    fxcrt::ostringstream* app_stream,
                    RetainPtr<CPDF_Dictionary> resource_dict,
                    bool is_text_markup_annotation,
                    const CFX_Matrix& matrix,
                    const CFX_FloatRect& bbox_override) {
  RetainPtr<CPDF_Dictionary> stream_dict =
      BuildAPStreamDict(annot_dict, std::move(resource_dict),
                        is_text_markup_annotation, matrix, bbox_override);

  target->normal_stream =
      target->IsPersistent()
          ? target->doc->NewIndirect<CPDF_Stream>(std::move(stream_dict))
          : pdfium::MakeRetain<CPDF_Stream>(std::move(stream_dict));
  target->normal_stream->SetDataFromStringstream(app_stream);

  if (!target->IsPersistent()) {
    return true;
  }

  RetainPtr<CPDF_Dictionary> ap_dict =
      target->persistent_annot_dict->GetOrCreateDictFor(
          pdfium::annotation::kAP);
  ap_dict->SetNewFor<CPDF_Reference>("N", target->doc,
                                     target->normal_stream->GetObjNum());
  return true;
}

bool GenerateAndSetAPDict(APGenerationTarget* target,
                          const CPDF_Dictionary* annot_dict,
                          fxcrt::ostringstream* app_stream,
                          RetainPtr<CPDF_Dictionary> resource_dict,
                          bool is_text_markup_annotation) {
  return GenerateAPDict(target, annot_dict, app_stream,
                        std::move(resource_dict), is_text_markup_annotation,
                        CFX_Matrix(), CFX_FloatRect());
}

// A box kind's appearance (`GenerateAndSetFramedAPDict`), with /RD: how far
// /Rect reaches past the upright box around the shape, in rectangle order as
// Acrobat writes it; none when the drawing stays inside the shape.
bool GenerateAndSetBoxAPDict(APGenerationTarget* target,
                             const CPDF_Dictionary* annot_dict,
                             fxcrt::ostringstream* app_stream,
                             RetainPtr<CPDF_Dictionary> resource_dict,
                             const CFX_Matrix& matrix,
                             const CFX_FloatRect& footprint) {
  const std::optional<CFX_FloatRect> frame =
      GenerateAndSetFramedAPDict(target, annot_dict, app_stream,
                                 std::move(resource_dict), matrix, footprint);
  if (!frame.has_value()) {
    return false;
  }
  if (!target->IsPersistent()) {
    return true;
  }
  CFX_FloatRect shape = footprint;
  shape.Normalize();
  CFX_FloatRect rect = matrix.TransformRect(frame.value());
  rect.Normalize();
  CFX_FloatRect upright = matrix.TransformRect(shape);
  upright.Normalize();
  CPDF_Dictionary* dict = target->persistent_annot_dict;
  const float differences[] = {upright.left - rect.left,
                               upright.bottom - rect.bottom,
                               rect.right - upright.right,
                               rect.top - upright.top};
  if (std::all_of(std::begin(differences), std::end(differences),
                  [](float difference) { return difference == 0; })) {
    dict->RemoveFor("RD");
    return true;
  }
  RetainPtr<CPDF_Array> rd = dict->SetNewFor<CPDF_Array>("RD");
  for (float difference : differences) {
    rd->AppendNew<CPDF_Number>(difference);
  }
  return true;
}

// A line, polyline, polygon or ink appearance: framed about its points, with
// no /RD (a point kind has none).
bool GenerateAndSetPointsAPDict(APGenerationTarget* target,
                                const CPDF_Dictionary* annot_dict,
                                fxcrt::ostringstream* app_stream,
                                RetainPtr<CPDF_Dictionary> resource_dict,
                                pdfium::span<const CFX_PointF> points) {
  return GenerateAndSetFramedAPDict(target, annot_dict, app_stream,
                                    std::move(resource_dict), CFX_Matrix(),
                                    BoundsOfPoints(points))
      .has_value();
}

bool GenerateAndSetAPDict(CPDF_Document* doc,
                          CPDF_Dictionary* annot_dict,
                          fxcrt::ostringstream* app_stream,
                          RetainPtr<CPDF_Dictionary> resource_dict,
                          bool is_text_markup_annotation) {
  APGenerationTarget target{doc, annot_dict};
  return GenerateAndSetAPDict(&target, annot_dict, app_stream,
                              std::move(resource_dict),
                              is_text_markup_annotation);
}

}  // namespace appearance_generator
