// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PAGE_CPDF_PAGEOBJECTGROUPS_H_
#define CORE_FPDFAPI_PAGE_CPDF_PAGEOBJECTGROUPS_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "core/fxcrt/fx_coordinates.h"

class CPDF_PageObjectHolder;

// The bounds of each run of kSize consecutive objects of a holder, so a render
// can pass over a run that its clip test would reject object by object.
//
// A run's bounds hold the least left and bottom and the greatest right and top
// of its objects' GetRect(). A test that rejects a rectangle for lying past a
// side of the clip therefore rejects a run's bounds only when it rejects every
// object in the run. A run with a side that is not finite, or with a missing
// object, gets bounds covering the whole plane, which such a test rejects only
// when it rejects every rectangle.
class CPDF_PageObjectGroups {
 public:
  static constexpr size_t kSize = 128;

  explicit CPDF_PageObjectGroups(const CPDF_PageObjectHolder& holder);
  ~CPDF_PageObjectGroups();

  // Whether no listed object's bounds changed since these were computed.
  // The holder drops them when its list changes.
  bool IsCurrent() const;

  // The bounds of the run that starts at `first`, a multiple of kSize.
  const CFX_FloatRect& GetBounds(size_t first) const {
    return bounds_[first / kSize];
  }

 private:
  std::vector<CFX_FloatRect> bounds_;
  const uint64_t generation_;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_PAGEOBJECTGROUPS_H_
