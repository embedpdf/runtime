// Copyright 2020 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef FPDFSDK_CPDFSDK_RENDERPAGE_H_
#define FPDFSDK_CPDFSDK_RENDERPAGE_H_

#include "public/fpdfview.h"

class CFX_Matrix;
class CPDFSDK_PauseAdapter;
class CPDF_Page;
class CPDF_PageRenderContext;
class PauseIndicatorIface;
struct FX_RECT;

void CPDFSDK_RenderPage(CPDF_PageRenderContext* context,
                        CPDF_Page* pPage,
                        const CFX_Matrix& matrix,
                        const FX_RECT& clipping_rect,
                        int flags,
                        const FPDF_COLORSCHEME* color_scheme);

// Starts rendering `pPage` as CPDFSDK_RenderPage() renders it, pausing when
// `pause` asks to; the render context keeps the render until it is cleared.
void CPDFSDK_StartRenderPage(CPDF_PageRenderContext* context,
                             CPDF_Page* pPage,
                             const CFX_Matrix& matrix,
                             const FX_RECT& clipping_rect,
                             int flags,
                             PauseIndicatorIface* pause);

// TODO(thestig): Consider giving this a better name, and make its parameters
// more similar to those of CPDFSDK_RenderPage().
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
                                   CPDFSDK_PauseAdapter* pause);

#endif  // FPDFSDK_CPDFSDK_RENDERPAGE_H_
