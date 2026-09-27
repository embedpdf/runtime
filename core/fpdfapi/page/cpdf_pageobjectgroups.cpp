// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/page/cpdf_pageobjectgroups.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"

namespace {

CFX_FloatRect WholePlane() {
  constexpr float kInfinity = std::numeric_limits<float>::infinity();
  CFX_FloatRect rect;
  rect.left = -kInfinity;
  rect.bottom = -kInfinity;
  rect.right = kInfinity;
  rect.top = kInfinity;
  return rect;
}

}  // namespace

CPDF_PageObjectGroups::CPDF_PageObjectGroups(
    const CPDF_PageObjectHolder& holder)
    : generation_(CPDF_PageObject::HeldBoundsGeneration()) {
  bounds_.reserve((holder.GetPageObjectCount() + kSize - 1) / kSize);
  constexpr float kInfinity = std::numeric_limits<float>::infinity();
  auto it = holder.begin();
  const auto end = holder.end();
  while (it != end) {
    CFX_FloatRect run;
    run.left = kInfinity;
    run.bottom = kInfinity;
    run.right = -kInfinity;
    run.top = -kInfinity;
    // The sum of every side in the run: finite only when every side is, since
    // an infinity or NaN never cancels out. A finite sum that overflows only
    // makes the run whole-plane, which is still correct.
    float sum = 0;
    for (size_t i = 0; i < kSize && it != end; ++i, ++it) {
      const CPDF_PageObject* object = it->get();
      if (!object) {
        sum = std::numeric_limits<float>::quiet_NaN();
        continue;
      }
      const CFX_FloatRect& rect = object->GetRect();
      sum += (rect.left + rect.bottom) + (rect.right + rect.top);
      run.left = std::min(run.left, rect.left);
      run.bottom = std::min(run.bottom, rect.bottom);
      run.right = std::max(run.right, rect.right);
      run.top = std::max(run.top, rect.top);
    }
    bounds_.push_back(std::isfinite(sum) ? run : WholePlane());
  }
}

CPDF_PageObjectGroups::~CPDF_PageObjectGroups() = default;

bool CPDF_PageObjectGroups::IsCurrent() const {
  return generation_ == CPDF_PageObject::HeldBoundsGeneration();
}
