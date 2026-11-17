// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_annot_rotation.h"

#include <cmath>

#include "constants/annotation_common.h"
#include "constants/appearance.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfdoc/cpdf_embed_metadata.h"

namespace fpdfdoc {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float NormalizeDegrees(float degrees) {
  float normalized = std::fmod(degrees, 360.0f);
  if (normalized < 0) {
    normalized += 360.0f;
  }
  return normalized >= 360.0f ? 0.0f : normalized;
}

}  // namespace

CFX_PointF BoxCenter(const CFX_FloatRect& box) {
  return CFX_PointF((box.left + box.right) / 2, (box.bottom + box.top) / 2);
}

CFX_Matrix TurnAbout(const CFX_PointF& center, float degrees) {
  const float theta = NormalizeDegrees(degrees) * kPi / 180.0f;
  auto snap = [](float value) {
    if (std::fabs(value) < 1e-6f) {
      return 0.0f;
    }
    if (std::fabs(value - 1.0f) < 1e-6f) {
      return 1.0f;
    }
    if (std::fabs(value + 1.0f) < 1e-6f) {
      return -1.0f;
    }
    return value;
  };
  const float cos_t = snap(std::cos(theta));
  const float sin_t = snap(std::sin(theta));
  // T(center) * R(theta) * T(-center)
  return CFX_Matrix(cos_t, sin_t, -sin_t, cos_t,
                    center.x * (1.0f - cos_t) + center.y * sin_t,
                    center.y * (1.0f - cos_t) - center.x * sin_t);
}

std::optional<AnnotRotation> GetRecordedAnnotRotation(
    const CPDF_Dictionary* annot_dict) {
  RetainPtr<const CPDF_Dictionary> metadata = GetEmbedMetadata(annot_dict);
  if (!metadata) {
    return std::nullopt;
  }
  const float degrees = NormalizeDegrees(metadata->GetFloatFor("Rotation"));
  CFX_FloatRect box = metadata->GetRectFor("UnrotatedRect");
  box.Normalize();
  // As before: a turn under 0.01 degrees either way is none.
  if (degrees < 0.01f || degrees > 359.99f || box.IsEmpty()) {
    return std::nullopt;
  }
  return AnnotRotation{degrees, box};
}

int GetMKRotationDegrees(const CPDF_Dictionary* mk_dict) {
  const int written =
      mk_dict ? mk_dict->GetIntegerFor(pdfium::appearance::kR) : 0;
  return ((written % 360) + 360) % 360;
}

std::optional<AnnotRotation> GetWidgetAnnotRotation(
    const CPDF_Dictionary* annot_dict) {
  const int degrees = GetMKRotationDegrees(annot_dict->GetDictFor("MK").Get());
  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  rect.Normalize();
  if (degrees == 0 || degrees % 90 != 0 || rect.IsEmpty()) {
    return std::nullopt;
  }
  if (degrees == 180) {
    return AnnotRotation{180.0f, rect};
  }
  const CFX_PointF center = BoxCenter(rect);
  const float half_width = rect.Height() / 2;
  const float half_height = rect.Width() / 2;
  return AnnotRotation{
      static_cast<float>(degrees),
      CFX_FloatRect(center.x - half_width, center.y - half_height,
                    center.x + half_width, center.y + half_height)};
}

}  // namespace fpdfdoc
