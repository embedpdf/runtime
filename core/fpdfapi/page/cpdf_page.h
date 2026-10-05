// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PAGE_CPDF_PAGE_H_
#define CORE_FPDFAPI_PAGE_CPDF_PAGE_H_

#include <memory>

#include <optional>
#include <utility>
#include "core/fpdfapi/parser/cpdf_measure_storage.h"

#include "core/fpdfapi/page/cpdf_contentversions.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"
#include "core/fpdfapi/page/cpdf_parsedsize.h"
#include "core/fpdfapi/page/ipdf_page.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/fx_memory.h"
#include "core/fxcrt/observed_ptr.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"

class CPDF_Array;
class CPDF_Dictionary;
class CPDF_Document;
class CPDF_Object;
class CPDF_PageImageCache;

class CPDF_Page final : public IPDF_Page, public CPDF_PageObjectHolder {
 public:
  // EmbedPDF: owned SDK measurement state; never part of the PDF graph.
  CPDF_MeasureStorage* GetMeasureStorage() const {
    return measure_storage_.get();
  }
  void SetMeasureStorage(std::unique_ptr<CPDF_MeasureStorage> storage) {
    measure_storage_ = std::move(storage);
  }

  // Caller implements as desired, exists here due to layering.
  class View : public Observable {
   public:
    virtual void ClearPage(CPDF_Page* pPage) = 0;
  };

  // Data for the render layer to attach to this page.
  class RenderContextIface {
   public:
    virtual ~RenderContextIface() = default;
  };

  class RenderContextClearer {
   public:
    FX_STACK_ALLOCATED();
    explicit RenderContextClearer(CPDF_Page* pPage);
    ~RenderContextClearer();

   private:
    UnownedPtr<CPDF_Page> const page_;
  };

  CONSTRUCT_VIA_MAKE_RETAIN;

  // IPDF_Page:
  CPDF_Page* AsPDFPage() override;
  CPDFXFA_Page* AsXFAPage() override;
  CPDF_Document* GetDocument() const override;
  float GetPageWidth() const override;
  float GetPageHeight() const override;
  CFX_Matrix GetDisplayMatrixForRect(const FX_RECT& rect,
                                     int rotation) const override;
  std::optional<CFX_PointF> DeviceToPage(
      const FX_RECT& rect,
      int rotation,
      const CFX_PointF& device_point) const override;
  std::optional<CFX_PointF> PageToDevice(
      const FX_RECT& rect,
      int rotation,
      const CFX_PointF& page_point) const override;

  // CPDF_PageObjectHolder:
  bool IsPage() const override;
  RetainPtr<const CPDF_Dictionary> GetResources() const override;
  RetainPtr<const CPDF_Dictionary> GetPageResources() const override;

  void ParseContent();
  const CFX_SizeF& GetPageSize() const { return page_size_; }
  const CFX_Matrix& GetPageMatrix() const { return page_matrix_; }
  CFX_Matrix GetDisplayMatrix() const;
  int GetPageRotation() const;

  // EmbedPDF extensions for rotation normalization
  // Returns the rotation value from the page dictionary, ignoring any override.
  int GetOriginalRotation() const;
  // Sets a rotation override and recalculates dimensions.
  // Pass -1 to clear the override.
  void SetRotationOverride(int rotation);

  RetainPtr<CPDF_Array> GetOrCreateAnnotsArray();
  RetainPtr<CPDF_Array> GetMutableAnnotsArray();
  RetainPtr<const CPDF_Array> GetAnnotsArray() const;

  void AddPageImageCache();
  CPDF_PageImageCache* GetPageImageCache() { return page_image_cache_.get(); }
  RenderContextIface* GetRenderContext() { return render_context_.get(); }

  // `context` cannot be null. `SetRenderContext()` cannot be called if the
  // page already has a render context. Use `ClearRenderContext()` to reset the
  // render context.
  void SetRenderContext(std::unique_ptr<RenderContextIface> context);
  void ClearRenderContext();

  void SetView(View* pView) { view_.Reset(pView); }
  void ClearView();
  void UpdateDimensions();

  // EmbedPDF: what the page's parsed objects cost, nested forms included.
  // While the page parses, it is what the parse has added so far; once it has
  // parsed, the objects are counted again after they change.
  const CPDF_ParsedSize& GetParsedSize() const;

  // EmbedPDF: whether the page's parsed objects still match the document:
  // the document resolves the page's boxes, rotation, /Contents streams and
  // the stream of every form placed on it to what the objects were parsed
  // from or last generated into. Only versions are compared (see
  // CPDF_ContentVersions): a write outside a layer transaction edits objects
  // in place, and this can't see it.
  bool IsContentCurrent() const;

  // EmbedPDF: what the content parser fills while it parses this page. It
  // starts both from empty (StartContentRecords()).
  void StartContentRecords();
  CPDF_ParsedSize* mutable_parsed_size() { return &parsed_size_; }
  CPDF_ContentVersions* mutable_content_versions() {
    return &content_versions_;
  }

  // EmbedPDF: after content was generated from this page's objects. A page
  // that matched the document before matches what was written: its content
  // versions are recorded again. The objects are counted again.
  void ContentGenerated(bool was_current);

 private:
  std::unique_ptr<CPDF_MeasureStorage> measure_storage_;
  CPDF_Page(CPDF_Document* document, RetainPtr<CPDF_Dictionary> pPageDict);
  ~CPDF_Page() override;

  // CPDF_PageObjectHolder:
  void EnsureMutableBackingObjectForResources() override;
  void EnsureMutableBackingObjectForPageResources() override;
  void RefreshResourcesIfNeeded() const;

  RetainPtr<CPDF_Object> GetMutablePageAttr(ByteStringView name);
  RetainPtr<const CPDF_Object> GetPageAttr(ByteStringView name) const;
  CFX_FloatRect GetBox(ByteStringView name) const;
  CFX_Matrix GetDisplayMatrixForFloatRect(const CFX_FloatRect& rect,
                                          int rotation) const;

  CFX_SizeF page_size_;
  CFX_Matrix page_matrix_;
  UnownedPtr<CPDF_Document> const pdf_document_;
  std::unique_ptr<CPDF_PageImageCache> page_image_cache_;
  std::unique_ptr<RenderContextIface> render_context_;
  ObservedPtr<View> view_;
  std::optional<int> rotation_override_;  // EmbedPDF: rotation normalization

  // EmbedPDF: the page's boxes and rotation as the last UpdateDimensions()
  // read them, the content versions of its objects, and the last check of
  // them, valid while the document's overlay epoch is the same.
  CFX_FloatRect read_mediabox_;
  CFX_FloatRect read_cropbox_;
  int read_rotation_ = 0;
  CPDF_ContentVersions content_versions_;
  mutable uint64_t content_checked_epoch_ = 0;
  mutable bool content_current_ = true;

  // EmbedPDF: the count of the parsed objects, and the holder's edit count
  // it was made at.
  mutable CPDF_ParsedSize parsed_size_;
  mutable uint64_t parsed_size_edits_ = 0;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_PAGE_H_
