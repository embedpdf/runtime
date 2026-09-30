// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PAGE_CPDF_PAINTEDBOUNDS_H_
#define CORE_FPDFAPI_PAGE_CPDF_PAINTEDBOUNDS_H_

#include "core/fxcrt/fx_coordinates.h"

class CPDF_PageObjectHolder;

// The upright box around what the active objects of `holder` paint, in the
// holder's space, each object cut to its clip. A stroke counts what its
// renderer draws: each segment's width across it; its ends as the cap paints
// them (a butt cap stops at the end, a round one reaches the half width, a
// square one projects it); its joins as the join paints them (a miter reaches
// its tip within the miter limit, a round join the half width, a bevel no
// further); and a curve's own extremes, the half width past them. A curve
// counts the curve, not its control points, and a dash pattern counts as the
// whole stroke. Tighter than the objects' own rects, which count a stroke's
// full width on each side. Empty when nothing paints.
CFX_FloatRect GetPaintedBounds(const CPDF_PageObjectHolder& holder);

#endif  // CORE_FPDFAPI_PAGE_CPDF_PAINTEDBOUNDS_H_
