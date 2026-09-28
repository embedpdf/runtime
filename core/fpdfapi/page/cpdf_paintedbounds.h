// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PAGE_CPDF_PAINTEDBOUNDS_H_
#define CORE_FPDFAPI_PAGE_CPDF_PAINTEDBOUNDS_H_

#include "core/fxcrt/fx_coordinates.h"

class CPDF_PageObjectHolder;

// The upright box around what the active objects of `holder` paint, in the
// holder's space, each object cut to its clip. A stroke counts its half width
// and the miter joins and square caps its renderer draws; a curve counts the
// curve, not its control points. Tighter than the objects' own rects, which
// count a stroke's full width on each side. Empty when nothing paints.
CFX_FloatRect GetPaintedBounds(const CPDF_PageObjectHolder& holder);

#endif  // CORE_FPDFAPI_PAGE_CPDF_PAINTEDBOUNDS_H_
