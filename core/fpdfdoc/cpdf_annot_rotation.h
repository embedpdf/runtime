// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFDOC_CPDF_ANNOT_ROTATION_H_
#define CORE_FPDFDOC_CPDF_ANNOT_ROTATION_H_

#include <optional>

#include "core/fxcrt/fx_coordinates.h"

class CPDF_Dictionary;

namespace fpdfdoc {

// A box turned by `degrees` counterclockwise (the PDF convention, as a
// /Matrix turns), in [0, 360), about its middle; `box` is the box before
// turning, in page space.
struct AnnotRotation {
  float degrees;
  CFX_FloatRect box;
};

// The middle of a box.
CFX_PointF BoxCenter(const CFX_FloatRect& box);

// A counterclockwise turn by `degrees` about `center`. Near quarter turns the
// trigonometry is exact, so the numbers written carry no noise.
CFX_Matrix TurnAbout(const CFX_PointF& center, float degrees);

// The rotation our /EMBD_Metadata records (/Rotation and /UnrotatedRect),
// which the engine writes before it draws or places an annotation.
std::optional<AnnotRotation> GetRecordedAnnotRotation(
    const CPDF_Dictionary* annot_dict);

// /MK /R as a turn, in [0, 360): the number as written, so -90 is 270, as
// Acrobat reads it. 0 without a /MK or an /R.
int GetMKRotationDegrees(const CPDF_Dictionary* mk_dict);

// A widget's rotation, from the entries every PDF app reads: /MK /R turns its
// contents (GetMKRotationDegrees) and /Rect is where it stands, so the box is
// /Rect with its sides swapped about its middle at 90 and 270. A value that
// is no quarter turn is no turn.
std::optional<AnnotRotation> GetWidgetAnnotRotation(
    const CPDF_Dictionary* annot_dict);

}  // namespace fpdfdoc

#endif  // CORE_FPDFDOC_CPDF_ANNOT_ROTATION_H_
