// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/render/cpdf_progressiverenderer.h"

#include <algorithm>
#include <iterator>

#include "build/build_config.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_pageimagecache.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pageobjectgroups.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"
#include "core/fpdfapi/render/cpdf_renderoptions.h"
#include "core/fpdfapi/render/cpdf_renderstatus.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/pauseindicator_iface.h"
#include "core/fxge/cfx_renderdevice.h"

CPDF_ProgressiveRenderer::CPDF_ProgressiveRenderer(
    CPDF_RenderContext* context,
    CFX_RenderDevice* pDevice,
    const CPDF_RenderOptions* pOptions)
    : context_(context), device_(pDevice), options_(pOptions) {
  CHECK(context_);
  CHECK(device_);
}

CPDF_ProgressiveRenderer::~CPDF_ProgressiveRenderer() {
  if (render_status_) {
    render_status_.reset();  // Release first.
    device_->RestoreState(false);
  }
}

void CPDF_ProgressiveRenderer::Start(PauseIndicatorIface* pPause) {
  if (status_ != kReady) {
    status_ = kFailed;
    return;
  }
  status_ = kToBeContinued;
  Continue(pPause);
}

void CPDF_ProgressiveRenderer::Continue(PauseIndicatorIface* pPause) {
  while (status_ == kToBeContinued) {
    if (!current_layer_) {
      if (layer_index_ >= context_->CountLayers()) {
        status_ = kDone;
        return;
      }
      current_layer_ = context_->GetLayer(layer_index_);
      last_object_rendered_ = current_layer_->GetObjectHolder()->end();
      render_status_ = std::make_unique<CPDF_RenderStatus>(context_, device_);
      if (options_) {
        render_status_->SetOptions(*options_);
      }
      render_status_->SetTransparency(
          current_layer_->GetObjectHolder()->GetTransparency());
      render_status_->Initialize(nullptr, nullptr);
      device_->SaveState();
      clip_rect_ = current_layer_->GetMatrix().GetInverse().TransformRect(
          CFX_FloatRect(device_->GetClipBox()));
    }
    const CPDF_PageObjectHolder* holder = current_layer_->GetObjectHolder();
    CPDF_PageObjectHolder::const_iterator iter;
    CPDF_PageObjectHolder::const_iterator iterEnd = holder->end();
    if (last_object_rendered_ != iterEnd) {
      iter = last_object_rendered_;
      ++iter;
    } else {
      iter = holder->begin();
    }
    auto touches_clip = [this](const CFX_FloatRect& rect) {
      return rect.left <= clip_rect_.right && rect.right >= clip_rect_.left &&
             rect.bottom <= clip_rect_.top && rect.top >= clip_rect_.bottom;
    };
    const CPDF_PageObjectGroups* groups = holder->GetObjectGroups(clip_rect_);
    const size_t count = holder->GetPageObjectCount();
    size_t index = std::distance(holder->begin(), iter);
    int nObjsToGo = kStepLimit;
    bool is_mask = false;
    while (iter != iterEnd) {
      if (groups && index % CPDF_PageObjectGroups::kSize == 0 &&
          !touches_clip(groups->GetBounds(index))) {
        // No object of this run passes the test below.
        const size_t run = std::min(CPDF_PageObjectGroups::kSize, count - index);
        std::advance(iter, run - 1);
        last_object_rendered_ = iter;
        ++iter;
        index += run;
        continue;
      }
      CPDF_PageObject* pCurObj = iter->get();
      if (pCurObj->IsActive() && touches_clip(pCurObj->GetRect())) {
        if (options_->GetOptions().bBreakForMasks && pCurObj->IsImage() &&
            pCurObj->AsImage()->GetImage()->IsMask()) {
#if BUILDFLAG(IS_WIN)
          if (device_->GetDeviceType() == DeviceType::kPrinter) {
            last_object_rendered_ = iter;
            render_status_->ProcessClipPath(pCurObj->clip_path(),
                                            current_layer_->GetMatrix());
            return;
          }
#endif
          is_mask = true;
        }
        if (render_status_->ContinueSingleObject(
                pCurObj, current_layer_->GetMatrix(), pPause)) {
          return;
        }
        if (pCurObj->IsImage() && render_status_->GetRenderOptions()
                                      .GetOptions()
                                      .bLimitedImageCache) {
          context_->GetPageCache()->CacheOptimization(
              render_status_->GetRenderOptions().GetCacheSizeLimit());
        }
        if (pCurObj->IsForm() || pCurObj->IsShading()) {
          nObjsToGo = 0;
        } else {
          --nObjsToGo;
        }
      }
      last_object_rendered_ = iter;
      if (nObjsToGo == 0) {
        if (pPause && pPause->NeedToPauseNow()) {
          return;
        }
        nObjsToGo = kStepLimit;
      }
      ++iter;
      ++index;
      if (is_mask && iter != iterEnd) {
        return;
      }
    }
    if (current_layer_->GetObjectHolder()->GetParseState() ==
        CPDF_PageObjectHolder::ParseState::kParsed) {
      render_status_.reset();
      device_->RestoreState(false);
      current_layer_ = nullptr;
      layer_index_++;
      if (is_mask || (pPause && pPause->NeedToPauseNow())) {
        return;
      }
    } else if (is_mask) {
      return;
    } else {
      current_layer_->GetObjectHolder()->ContinueParse(pPause);
      if (current_layer_->GetObjectHolder()->GetParseState() !=
          CPDF_PageObjectHolder::ParseState::kParsed) {
        return;
      }
    }
  }
}
