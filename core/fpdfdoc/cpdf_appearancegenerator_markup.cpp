// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

// EmbedPDF: the appearance generator's text markup: highlight, underline,
// squiggly and strike-out.
//
// See docs/embedpdf/fork-ledger.md, rule own-appearance-generator. EmbedPDF's
// changes are Copyright 2026 CloudPDF LTD.

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <utility>

#include "constants/annotation_common.h"
#include "core/fxcrt/stream_write_utils.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_appearancegenerator_internal.h"
#include "core/fxcrt/fx_string_wrappers.h"

using appearance_generator::APGenerationTarget;
using appearance_generator::CrossProduct;
using appearance_generator::DotProduct;
using appearance_generator::GenerateAndSetAPDict;
using appearance_generator::GenerateExtGStateDict;
using appearance_generator::GenerateResourcesDict;
using appearance_generator::GetColorStringWithDefault;
using appearance_generator::IsAxisAlignedZigZag;
using appearance_generator::IsFinitePoint;
using appearance_generator::kGSDictName;
using appearance_generator::MarkupQuad;
using appearance_generator::Midpoint;
using appearance_generator::PaintOperation;
using appearance_generator::ReadMarkupQuad;
using appearance_generator::UnitVector;
using appearance_generator::VectorLength;

namespace {

constexpr float kMarkupQuadTolerance = 1e-4f;

CFX_PointF LowerToUpperUnit(const MarkupQuad& quad) {
  return UnitVector((quad.upper_start - quad.lower_start) +
                    (quad.upper_end - quad.lower_end));
}

void EmitOrientedSquiggle(fxcrt::ostringstream& stream,
                          const MarkupQuad& quad,
                          float delta) {
  const CFX_PointF lower_edge = quad.lower_end - quad.lower_start;
  const float length = VectorLength(lower_edge);
  const CFX_PointF along = UnitVector(lower_edge);
  const CFX_PointF toward_upper = LowerToUpperUnit(quad);
  WritePoint(stream, quad.lower_start + delta * toward_upper) << " m ";

  float distance = delta;
  bool is_upwards = false;
  while (distance < length) {
    CFX_PointF point = quad.lower_start + distance * along;
    if (is_upwards) {
      point += delta * toward_upper;
    }
    WritePoint(stream, point) << " l ";
    distance += delta;
    is_upwards = !is_upwards;
  }

  const float remainder = length - (distance - delta);
  const float height = is_upwards ? remainder : delta - remainder;
  WritePoint(stream, quad.lower_end + height * toward_upper) << " l S\n";
}

}  // namespace

namespace appearance_generator {

std::optional<MarkupQuad> ReadMarkupQuad(const CPDF_Array* array,
                                         size_t index) {
  const size_t offset = index * 8;
  MarkupQuad quad = {
      {array->GetFloatAt(offset), array->GetFloatAt(offset + 1)},
      {array->GetFloatAt(offset + 2), array->GetFloatAt(offset + 3)},
      {array->GetFloatAt(offset + 4), array->GetFloatAt(offset + 5)},
      {array->GetFloatAt(offset + 6), array->GetFloatAt(offset + 7)}};
  if (!IsFinitePoint(quad.upper_start) || !IsFinitePoint(quad.upper_end) ||
      !IsFinitePoint(quad.lower_start) || !IsFinitePoint(quad.lower_end)) {
    return std::nullopt;
  }

  const CFX_PointF upper_edge = quad.upper_end - quad.upper_start;
  const CFX_PointF lower_edge = quad.lower_end - quad.lower_start;
  const CFX_PointF start_edge = quad.lower_start - quad.upper_start;
  const CFX_PointF end_edge = quad.lower_end - quad.upper_end;
  const float upper_length = VectorLength(upper_edge);
  const float lower_length = VectorLength(lower_edge);
  const float start_length = VectorLength(start_edge);
  const float end_length = VectorLength(end_edge);
  if (!std::isfinite(upper_length) || !std::isfinite(lower_length) ||
      !std::isfinite(start_length) || !std::isfinite(end_length) ||
      upper_length <= kMarkupQuadTolerance ||
      lower_length <= kMarkupQuadTolerance ||
      start_length <= kMarkupQuadTolerance ||
      end_length <= kMarkupQuadTolerance) {
    return std::nullopt;
  }

  // The PDF text-markup order is upper-start, upper-end, lower-start,
  // lower-end. Reject crossed and reversed producer data before interpreting
  // those pairs as a local frame.
  const float horizontal_alignment = DotProduct(upper_edge, lower_edge);
  const float vertical_alignment = DotProduct(start_edge, end_edge);
  if (!std::isfinite(horizontal_alignment) ||
      !std::isfinite(vertical_alignment) || horizontal_alignment <= 0.0f ||
      vertical_alignment <= 0.0f) {
    return std::nullopt;
  }
  const float start_area = CrossProduct(upper_edge, start_edge);
  const float end_area = CrossProduct(lower_edge, end_edge);
  if (!std::isfinite(start_area) || !std::isfinite(end_area) ||
      std::signbit(start_area) != std::signbit(end_area) ||
      std::abs(start_area) <=
          kMarkupQuadTolerance * upper_length * start_length ||
      std::abs(end_area) <= kMarkupQuadTolerance * lower_length * end_length) {
    return std::nullopt;
  }
  return quad;
}

bool IsAxisAlignedZigZag(const MarkupQuad& quad) {
  const float scale =
      std::max({1.0f, VectorLength(quad.upper_end - quad.upper_start),
                VectorLength(quad.lower_end - quad.lower_start),
                VectorLength(quad.lower_start - quad.upper_start),
                VectorLength(quad.lower_end - quad.upper_end)});
  const float tolerance = kMarkupQuadTolerance * scale;
  return std::abs(quad.upper_start.y - quad.upper_end.y) <= tolerance &&
         std::abs(quad.lower_start.y - quad.lower_end.y) <= tolerance &&
         std::abs(quad.upper_start.x - quad.lower_start.x) <= tolerance &&
         std::abs(quad.upper_end.x - quad.lower_end.x) <= tolerance;
}

bool GenerateHighlightAP(APGenerationTarget* target,
                         CPDF_Dictionary* annot_dict,
                         const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color::MakeRGB(1, 1, 0), PaintOperation::kFill);

  RetainPtr<const CPDF_Array> quad_points_array =
      annot_dict->GetArrayFor("QuadPoints");
  if (quad_points_array) {
    const size_t quad_point_count =
        CPDF_Annot::QuadPointCount(quad_points_array.Get());
    for (size_t i = 0; i < quad_point_count; ++i) {
      const std::optional<MarkupQuad> quad =
          ReadMarkupQuad(quad_points_array.Get(), i);
      if (quad && !IsAxisAlignedZigZag(*quad)) {
        WritePoint(app_stream, quad->upper_start) << " m ";
        WritePoint(app_stream, quad->upper_end) << " l ";
        WritePoint(app_stream, quad->lower_end) << " l ";
        WritePoint(app_stream, quad->lower_start) << " l h f\n";
        continue;
      }

      CFX_FloatRect rect = CPDF_Annot::RectFromQuadPoints(annot_dict, i);
      rect.Normalize();

      app_stream << rect.left << " " << rect.top << " m " << rect.right << " "
                 << rect.top << " l " << rect.right << " " << rect.bottom
                 << " l " << rect.left << " " << rect.bottom << " l h f\n";
    }
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &app_stream,
                       std::move(resources_dict),
                       true /*IsTextMarkupAnnotation*/);

  return true;
}

bool GenerateUnderlineAP(APGenerationTarget* target,
                         CPDF_Dictionary* annot_dict,
                         const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color::MakeRGB(0, 0, 0), PaintOperation::kStroke);

  RetainPtr<const CPDF_Array> quad_points_array =
      annot_dict->GetArrayFor("QuadPoints");
  if (quad_points_array) {
    static constexpr int kLineWidth = 1;
    app_stream << kLineWidth << " w ";
    const size_t quad_point_count =
        CPDF_Annot::QuadPointCount(quad_points_array.Get());
    for (size_t i = 0; i < quad_point_count; ++i) {
      const std::optional<MarkupQuad> quad =
          ReadMarkupQuad(quad_points_array.Get(), i);
      if (quad && !IsAxisAlignedZigZag(*quad)) {
        const CFX_PointF inset = LowerToUpperUnit(*quad);
        WritePoint(app_stream, quad->lower_start + kLineWidth * inset) << " m ";
        WritePoint(app_stream, quad->lower_end + kLineWidth * inset)
            << " l S\n";
        continue;
      }

      CFX_FloatRect rect = CPDF_Annot::RectFromQuadPoints(annot_dict, i);
      rect.Normalize();
      app_stream << rect.left << " " << rect.bottom + kLineWidth << " m "
                 << rect.right << " " << rect.bottom + kLineWidth << " l S\n";
    }
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &app_stream,
                       std::move(resources_dict),
                       true /*IsTextMarkupAnnotation*/);
  return true;
}

bool GenerateSquigglyAP(APGenerationTarget* target,
                        CPDF_Dictionary* annot_dict,
                        const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color::MakeRGB(0, 0, 0), PaintOperation::kStroke);

  RetainPtr<const CPDF_Array> quad_points_array =
      annot_dict->GetArrayFor("QuadPoints");
  if (quad_points_array) {
    static constexpr int kLineWidth = 1;
    static constexpr int kDelta = 2;
    app_stream << kLineWidth << " w ";
    const size_t quad_point_count =
        CPDF_Annot::QuadPointCount(quad_points_array.Get());
    for (size_t i = 0; i < quad_point_count; ++i) {
      const std::optional<MarkupQuad> quad =
          ReadMarkupQuad(quad_points_array.Get(), i);
      if (quad && !IsAxisAlignedZigZag(*quad)) {
        EmitOrientedSquiggle(app_stream, *quad, kDelta);
        continue;
      }

      CFX_FloatRect rect = CPDF_Annot::RectFromQuadPoints(annot_dict, i);
      rect.Normalize();

      const float top = rect.bottom + kDelta;
      const float bottom = rect.bottom;
      app_stream << rect.left << " " << top << " m ";

      float x = rect.left + kDelta;
      bool isUpwards = false;
      while (x < rect.right) {
        app_stream << x << " " << (isUpwards ? top : bottom) << " l ";
        x += kDelta;
        isUpwards = !isUpwards;
      }

      float remainder = rect.right - (x - kDelta);
      if (isUpwards) {
        app_stream << rect.right << " " << bottom + remainder << " l ";
      } else {
        app_stream << rect.right << " " << top - remainder << " l ";
      }

      app_stream << "S\n";
    }
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &app_stream,
                       std::move(resources_dict),
                       true /*IsTextMarkupAnnotation*/);
  return true;
}

bool GenerateStrikeOutAP(APGenerationTarget* target,
                         CPDF_Dictionary* annot_dict,
                         const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color::MakeRGB(0, 0, 0), PaintOperation::kStroke);

  RetainPtr<const CPDF_Array> quad_points_array =
      annot_dict->GetArrayFor("QuadPoints");
  if (quad_points_array) {
    const size_t quad_point_count =
        CPDF_Annot::QuadPointCount(quad_points_array.Get());
    for (size_t i = 0; i < quad_point_count; ++i) {
      const std::optional<MarkupQuad> quad =
          ReadMarkupQuad(quad_points_array.Get(), i);
      if (quad && !IsAxisAlignedZigZag(*quad)) {
        static constexpr int kLineWidth = 1;
        app_stream << kLineWidth << " w ";
        WritePoint(app_stream, Midpoint(quad->upper_start, quad->lower_start))
            << " m ";
        WritePoint(app_stream, Midpoint(quad->upper_end, quad->lower_end))
            << " l S\n";
        continue;
      }

      CFX_FloatRect rect = CPDF_Annot::RectFromQuadPoints(annot_dict, i);
      rect.Normalize();

      float y = (rect.top + rect.bottom) / 2;
      static constexpr int kLineWidth = 1;
      app_stream << kLineWidth << " w " << rect.left << " " << y << " m "
                 << rect.right << " " << y << " l S\n";
    }
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &app_stream,
                       std::move(resources_dict),
                       true /*IsTextMarkupAnnotation*/);
  return true;
}

}  // namespace appearance_generator
