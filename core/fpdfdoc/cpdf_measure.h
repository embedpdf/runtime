// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
#ifndef CORE_FPDFDOC_CPDF_MEASURE_H_
#define CORE_FPDFDOC_CPDF_MEASURE_H_

#include <optional>
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/widestring.h"

class CPDF_Array;
class CPDF_Dictionary;

// Dictionary grammar only. Measurement arithmetic/formatting belongs to the
// engine; appearance generation paints /Contents without computing a value.
class CPDF_Measure {
 public:
  enum class Axis { kX, kY, kDistance, kArea, kAngle, kSlope };
  enum class Fraction { kDecimal, kFraction, kRound, kTruncate };
  enum class LabelPosition { kSuffix, kPrefix };
  enum class Subtype { kUnknown, kRectilinear, kGeospatial };
  static ByteStringView KeyForAxis(Axis axis);
  static std::optional<Fraction> FractionFromName(ByteStringView name);
  static ByteStringView NameForFraction(Fraction fraction);
  static std::optional<LabelPosition> LabelPositionFromName(
      ByteStringView name);
  static ByteStringView NameForLabelPosition(LabelPosition position);
  static Subtype SubtypeOf(const CPDF_Dictionary* measure);
  static void ResetRectilinear(CPDF_Dictionary* measure);
  static int FindViewportForPoint(const CPDF_Dictionary* page,
                                  const CFX_PointF& point);
  // Detach each indirect link on the mutation path, including nested arrays
  // and dictionaries. Never obtain a mutable referent before copying it.
  static void DetachIndirect(CPDF_Dictionary* owner, ByteStringView key);
  static void DetachIndirect(CPDF_Array* owner, size_t index);
};
#endif  // CORE_FPDFDOC_CPDF_MEASURE_H_
