// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
#include "core/fpdfdoc/cpdf_measure.h"

#include <cmath>
#include <limits>
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"

ByteStringView CPDF_Measure::KeyForAxis(Axis axis) {
  switch (axis) {
    case Axis::kX:
      return "X";
    case Axis::kY:
      return "Y";
    case Axis::kDistance:
      return "D";
    case Axis::kArea:
      return "A";
    case Axis::kAngle:
      return "T";
    case Axis::kSlope:
      return "S";
  }
  return {};
}
std::optional<CPDF_Measure::Fraction> CPDF_Measure::FractionFromName(
    ByteStringView name) {
  if (name == "D") {
    return Fraction::kDecimal;
  }
  if (name == "F") {
    return Fraction::kFraction;
  }
  if (name == "R") {
    return Fraction::kRound;
  }
  if (name == "T") {
    return Fraction::kTruncate;
  }
  return std::nullopt;
}
ByteStringView CPDF_Measure::NameForFraction(Fraction fraction) {
  switch (fraction) {
    case Fraction::kDecimal:
      return "D";
    case Fraction::kFraction:
      return "F";
    case Fraction::kRound:
      return "R";
    case Fraction::kTruncate:
      return "T";
  }
  return {};
}
std::optional<CPDF_Measure::LabelPosition> CPDF_Measure::LabelPositionFromName(
    ByteStringView name) {
  if (name == "S") {
    return LabelPosition::kSuffix;
  }
  if (name == "P") {
    return LabelPosition::kPrefix;
  }
  return std::nullopt;
}
ByteStringView CPDF_Measure::NameForLabelPosition(LabelPosition position) {
  switch (position) {
    case LabelPosition::kSuffix:
      return "S";
    case LabelPosition::kPrefix:
      return "P";
  }
  return {};
}
CPDF_Measure::Subtype CPDF_Measure::SubtypeOf(const CPDF_Dictionary* measure) {
  if (!measure) {
    return Subtype::kUnknown;
  }
  if (!measure->KeyExist("Subtype")) {
    return Subtype::kRectilinear;
  }
  ByteString name = measure->GetNameFor("Subtype");
  if (name == "RL") {
    return Subtype::kRectilinear;
  }
  if (name == "GEO") {
    return Subtype::kGeospatial;
  }
  return Subtype::kUnknown;
}
void CPDF_Measure::ResetRectilinear(CPDF_Dictionary* measure) {
  for (const char* key : {"R", "X", "Y", "D", "A", "T", "S", "O", "CYX"}) {
    measure->RemoveFor(key);
  }
  measure->SetNewFor<CPDF_Name>("Type", "Measure");
  measure->SetNewFor<CPDF_Name>("Subtype", "RL");
}
int CPDF_Measure::FindViewportForPoint(const CPDF_Dictionary* page,
                                       const CFX_PointF& point) {
  if (!page || !std::isfinite(point.x) || !std::isfinite(point.y)) {
    return -1;
  }
  auto vp = page->GetArrayFor("VP");
  if (!vp ||
      vp->size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return -1;
  }
  for (size_t i = vp->size(); i > 0; --i) {
    auto dict = vp->GetDictAt(i - 1);
    auto box = dict ? dict->GetArrayFor("BBox") : nullptr;
    if (!box || box->size() != 4) {
      continue;
    }
    bool valid = true;
    for (size_t j = 0; j < 4; ++j) {
      auto value = box->GetNumberAt(j);
      valid &= value && std::isfinite(value->GetNumber());
    }
    if (!valid) {
      continue;
    }
    CFX_FloatRect rect = box->GetRect();
    rect.Normalize();
    if (rect.Contains(point)) {
      return static_cast<int>(i - 1);
    }
  }
  return -1;
}
void CPDF_Measure::DetachIndirect(CPDF_Dictionary* owner, ByteStringView key) {
  auto object = owner->GetObjectFor(key);
  if (object && object->IsReference()) {
    owner->SetFor(ByteString(key), object->CloneDirectObject());
  }
}
void CPDF_Measure::DetachIndirect(CPDF_Array* owner, size_t index) {
  auto object = owner->GetObjectAt(index);
  if (object && object->IsReference()) {
    auto clone = object->CloneDirectObject();
    if (clone) {
      owner->SetAt(index, std::move(clone));
    }
  }
}
