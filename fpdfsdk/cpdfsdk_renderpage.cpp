// Copyright 2020 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "fpdfsdk/cpdfsdk_renderpage.h"

#include <memory>
#include <utility>

#include "build/build_config.h"
#include "core/fpdfapi/page/cpdf_pageimagecache.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/render/cpdf_pagerendercontext.h"
#include "core/fpdfapi/render/cpdf_progressiverenderer.h"
#include "core/fpdfapi/render/cpdf_renderoptions.h"
#include "core/fpdfdoc/cpdf_annotlist.h"
#include "core/fxcrt/pauseindicator_iface.h"
#include "core/fxge/cfx_renderdevice.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/cpdfsdk_pauseadapter.h"

namespace {

void RenderPageImpl(CPDF_PageRenderContext* context,
                    CPDF_Page* pPage,
                    const CFX_Matrix& matrix,
                    const FX_RECT& clipping_rect,
                    int flags,
                    const FPDF_COLORSCHEME* color_scheme,
                    bool need_to_restore,
                    PauseIndicatorIface* pause) {
  if (!context->options_) {
    context->options_ = std::make_unique<CPDF_RenderOptions>();
  }

  auto& options = context->options_->GetOptions();
  options.bClearType = !!(flags & FPDF_LCD_TEXT);
  options.bNoNativeText = !!(flags & FPDF_NO_NATIVETEXT);
  options.bLimitedImageCache = !!(flags & FPDF_RENDER_LIMITEDIMAGECACHE);
  options.bForceHalftone = !!(flags & FPDF_RENDER_FORCEHALFTONE);
  options.bNoTextSmooth = !!(flags & FPDF_RENDER_NO_SMOOTHTEXT);
  options.bNoImageSmooth = !!(flags & FPDF_RENDER_NO_SMOOTHIMAGE);
  options.bNoPathSmooth = !!(flags & FPDF_RENDER_NO_SMOOTHPATH);

  // Grayscale output
  if (flags & FPDF_GRAYSCALE) {
    context->options_->SetColorMode(CPDF_RenderOptions::kGray);
  }

  if (color_scheme) {
    context->options_->SetColorMode(CPDF_RenderOptions::kForcedColor);
    SetColorFromScheme(color_scheme, context->options_.get());
    options.bConvertFillToStroke = !!(flags & FPDF_CONVERT_FILL_TO_STROKE);
  }

  const CPDF_OCContext::UsageType usage =
      (flags & FPDF_PRINTING) ? CPDF_OCContext::kPrint : CPDF_OCContext::kView;
  context->options_->SetOCContext(
      pdfium::MakeRetain<CPDF_OCContext>(pPage->GetDocument(), usage));

  context->device_->SaveState();
  context->device_->SetBaseClip(clipping_rect);
  context->device_->SetClip_Rect(clipping_rect);
  context->context_ = std::make_unique<CPDF_RenderContext>(
      pPage->GetDocument(),
      pdfium::WrapRetain(
          const_cast<CPDF_Dictionary*>(pPage->GetPageResources().Get())),
      pPage->GetPageImageCache());

  context->context_->AppendLayer(pPage, matrix);

  // EmbedPDF: annotations (FPDF_ANNOT) and form field widgets
  // (EPDF_RENDER_WIDGETS) are drawn independently, so a page picture can hold
  // either, both or neither: a viewer paints its own layers over the rest.
  const bool bShowAnnots = !!(flags & FPDF_ANNOT);
  const bool bShowWidget = !!(flags & EPDF_RENDER_WIDGETS);
  if (bShowAnnots || bShowWidget) {
    auto pOwnedList = std::make_unique<CPDF_AnnotList>(pPage);
    CPDF_AnnotList* pList = pOwnedList.get();
    context->annots_ = std::move(pOwnedList);
    bool is_printing = (flags & FPDF_PRINTING);
#if BUILDFLAG(IS_WIN)
    is_printing |= context->device_->GetDeviceType() == DeviceType::kPrinter;
#endif

    pList->DisplayAnnots(context->context_.get(), is_printing, matrix,
                         bShowAnnots, bShowWidget);
  }

  context->renderer_ = std::make_unique<CPDF_ProgressiveRenderer>(
      context->context_.get(), context->device_.get(), context->options_.get());
  context->renderer_->Start(pause);
  if (need_to_restore) {
    context->device_->RestoreState(false);
  }
}

}  // namespace

void CPDFSDK_RenderPage(CPDF_PageRenderContext* context,
                        CPDF_Page* pPage,
                        const CFX_Matrix& matrix,
                        const FX_RECT& clipping_rect,
                        int flags,
                        const FPDF_COLORSCHEME* color_scheme) {
  RenderPageImpl(context, pPage, matrix, clipping_rect, flags, color_scheme,
                 /*need_to_restore=*/true, /*pause=*/nullptr);
}

void CPDFSDK_StartRenderPage(CPDF_PageRenderContext* context,
                             CPDF_Page* pPage,
                             const CFX_Matrix& matrix,
                             const FX_RECT& clipping_rect,
                             int flags,
                             PauseIndicatorIface* pause) {
  RenderPageImpl(context, pPage, matrix, clipping_rect, flags,
                 /*color_scheme=*/nullptr, /*need_to_restore=*/false, pause);
}

void CPDFSDK_RenderPageWithContext(CPDF_PageRenderContext* context,
                                   CPDF_Page* pPage,
                                   int start_x,
                                   int start_y,
                                   int size_x,
                                   int size_y,
                                   int rotate,
                                   int flags,
                                   const FPDF_COLORSCHEME* color_scheme,
                                   bool need_to_restore,
                                   CPDFSDK_PauseAdapter* pause) {
  const FX_RECT rect(start_x, start_y, start_x + size_x, start_y + size_y);
  RenderPageImpl(context, pPage, pPage->GetDisplayMatrixForRect(rect, rotate),
                 rect, flags, color_scheme, need_to_restore, pause);
}
