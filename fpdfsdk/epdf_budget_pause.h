// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef FPDFSDK_EPDF_BUDGET_PAUSE_H_
#define FPDFSDK_EPDF_BUDGET_PAUSE_H_

#include <algorithm>
#include <chrono>

#include "core/fxcrt/pauseindicator_iface.h"

// Asks to pause once a time budget has passed since it was made: the slices
// of a sliced render or page load. A budget of 0 or less pauses at every
// chance.
class EPDF_BudgetPause final : public PauseIndicatorIface {
 public:
  explicit EPDF_BudgetPause(int budget_ms)
      : deadline_(std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(std::max(budget_ms, 0))) {}

  bool NeedToPauseNow() override {
    return std::chrono::steady_clock::now() >= deadline_;
  }

 private:
  const std::chrono::steady_clock::time_point deadline_;
};

#endif  // FPDFSDK_EPDF_BUDGET_PAUSE_H_
