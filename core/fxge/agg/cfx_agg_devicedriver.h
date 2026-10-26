// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FXGE_AGG_CFX_AGG_DEVICEDRIVER_H_
#define CORE_FXGE_AGG_CFX_AGG_DEVICEDRIVER_H_

#include <memory>
#include <vector>

#include "build/build_config.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxge/cfx_fillrenderoptions.h"
#include "core/fxge/renderdevicedriver_iface.h"

#if BUILDFLAG(IS_APPLE)
#include "core/fxcrt/unowned_ptr_exclusion.h"
#endif

class CFX_AggClipRgn;
class CFX_GraphStateData;
class CFX_Matrix;
class CFX_Path;

namespace pdfium {

namespace agg {
class rasterizer_scanline_aa;
}  // namespace agg

class CFX_AggDeviceDriver final : public RenderDeviceDriverIface {
 public:
  CFX_AggDeviceDriver(RetainPtr<CFX_DIBitmap> pBitmap,
                      bool bRgbByteOrder,
                      RetainPtr<CFX_DIBitmap> pBackdropBitmap);
  ~CFX_AggDeviceDriver() override;

  void InitPlatform();
  void DestroyPlatform();

  // RenderDeviceDriverIface:
  DeviceType GetDeviceType() const override;
  bool RenderCapGetBits() const override;
  bool RenderCapAlphaImage() const override;
  bool RenderCapBlendMode() const override;
  bool RenderCapSoftClip() const override;
  bool RenderCapAlphaOutput() const override;
  bool RenderCapByteMaskOutput() const override;
  int GetPixelWidth() const override;
  int GetPixelHeight() const override;
  int GetBitsPerPixel() const override;
  void Clear(uint32_t color) override;
  void SaveState() override;
  void RestoreState(bool bKeepSaved) override;
  bool SetClip_PathFill(const CFX_Path& path,
                        const CFX_Matrix* pObject2Device,
                        const CFX_FillRenderOptions& fill_options) override;
  bool SetClip_PathStroke(const CFX_Path& path,
                          const CFX_Matrix* pObject2Device,
                          const CFX_GraphStateData* pGraphState) override;
  bool DrawPath(const CFX_Path& path,
                const CFX_Matrix* pObject2Device,
                const CFX_GraphStateData* pGraphState,
                uint32_t fill_color,
                uint32_t stroke_color,
                bool group_knockout,
                const CFX_FillRenderOptions& fill_options) override;
  bool FillRect(const FX_RECT& rect, uint32_t fill_color) override;
  FX_RECT GetClipBox() const override;
  bool GetDIBits(RetainPtr<CFX_DIBitmap> bitmap,
                 int left,
                 int top) const override;
  RetainPtr<const CFX_DIBitmap> GetBackDrop() const override;
  bool SetDIBits(RetainPtr<const CFX_DIBBase> bitmap,
                 uint32_t argb,
                 const FX_RECT& src_rect,
                 int left,
                 int top,
                 BlendMode blend_type) override;
  bool StretchDIBits(RetainPtr<const CFX_DIBBase> bitmap,
                     uint32_t argb,
                     int dest_left,
                     int dest_top,
                     int dest_width,
                     int dest_height,
                     const FX_RECT* pClipRect,
                     const FXDIB_ResampleOptions& options,
                     BlendMode blend_type) override;
  StartResult StartDIBits(RetainPtr<const CFX_DIBBase> bitmap,
                          float alpha,
                          uint32_t argb,
                          const CFX_Matrix& matrix,
                          const FXDIB_ResampleOptions& options,
                          BlendMode blend_type) override;
  bool ContinueDIBits(RenderDeviceDriverIface::Continuation* continuation,
                      PauseIndicatorIface* pPause) override;
  bool DrawDeviceText(pdfium::span<const TextCharPos> pCharPos,
                      CFX_Font* font,
                      const CFX_Matrix& mtObject2Device,
                      float font_size,
                      uint32_t color,
                      const CFX_TextRenderOptions& options) override;
  int GetDriverType() const override;
  bool MultiplyAlpha(float alpha) override;
  bool MultiplyAlphaMask(RetainPtr<const CFX_DIBitmap> mask) override;

  // EmbedPDF(render-speedup): the off switch the off/on comparisons use.
  // Turns off the reuse of a repeated path's rasterizer output, so a test can
  // render without it and compare.
  static void SetPathMemoEnabledForTesting(bool enabled);

 private:
  // EmbedPDF(render-speedup): reused path storage and the path memo.
  // The path storage, rasterizer, scanline and stroker every path of this
  // device is drawn with. Building them per path costs more than rasterizing
  // the small paths that vector-heavy pages consist of.
  class Workspace;
  // The scanlines of one rasterizer pass, recorded to be composited again.
  class RecordedPass;
  // What DrawPath() needs to recognize a repeated small path: the last path
  // object, and the inputs and recorded passes of the call that drew it
  // again. Later calls with identical inputs composite the recorded passes
  // instead of rasterizing: the rasterizer output depends on nothing else.
  class PathMemo;

  Workspace& GetWorkspace();
  // Frees the workspace when one large path grew it, instead of keeping that
  // memory until the device is destroyed.
  void ReleaseLargeWorkspace();

  void DrawPathPasses(const CFX_Path& path,
                      const CFX_Matrix* pObject2Device,
                      const CFX_GraphStateData* pGraphState,
                      uint32_t fill_color,
                      uint32_t stroke_color,
                      bool group_knockout,
                      const CFX_FillRenderOptions& fill_options,
                      PathMemo* record);

  // EmbedPDF(render-speedup): `record` and ReplayPass() serve the path memo.
  // Composites `rasterizer`'s output; `record`, when given, keeps a copy.
  void RenderRasterizer(pdfium::agg::rasterizer_scanline_aa& rasterizer,
                        uint32_t color,
                        bool bFullCover,
                        bool bGroupKnockout,
                        RecordedPass* record = nullptr);
  // Composites a recorded pass exactly as RenderRasterizer() composited it.
  void ReplayPass(const RecordedPass& pass,
                  uint32_t color,
                  bool bFullCover,
                  bool bGroupKnockout);

  void SetClipMask(pdfium::agg::rasterizer_scanline_aa& rasterizer);

  RetainPtr<CFX_DIBitmap> const bitmap_;
  std::unique_ptr<CFX_AggClipRgn> clip_rgn_;
  std::vector<std::unique_ptr<CFX_AggClipRgn>> state_stack_;
#if BUILDFLAG(IS_APPLE)
  UNOWNED_PTR_EXCLUSION void* platform_graphics_ = nullptr;
#endif
  CFX_FillRenderOptions fill_options_;
  const bool rgb_byte_order_;
  RetainPtr<CFX_DIBitmap> backdrop_bitmap_;
  // EmbedPDF(render-speedup): reused path storage and the path memo.
  std::unique_ptr<Workspace> workspace_;
  std::unique_ptr<PathMemo> path_memo_;
};

}  // namespace pdfium

#endif  // CORE_FXGE_AGG_CFX_AGG_DEVICEDRIVER_H_
