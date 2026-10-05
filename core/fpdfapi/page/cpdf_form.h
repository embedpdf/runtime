// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PAGE_CPDF_FORM_H_
#define CORE_FPDFAPI_PAGE_CPDF_FORM_H_

#include <stdint.h>

#include <set>
#include <utility>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"
#include "core/fxcrt/retain_ptr.h"

class CFX_Matrix;
class CPDF_AllStates;
class CPDF_Dictionary;
class CPDF_Document;
class CPDF_Stream;
class CPDF_Type3Char;

class CPDF_Form final : public CPDF_PageObjectHolder,
                        public CPDF_Font::FormIface {
 public:
  struct RecursionState {
    RecursionState();
    ~RecursionState();

    std::set<const uint8_t*> parsed_set;
  };

  // Helper method to choose the first non-null resources dictionary.
  static const CPDF_Dictionary* ChooseResourcesDict(
      const CPDF_Dictionary* pResources,
      const CPDF_Dictionary* pParentResources,
      const CPDF_Dictionary* pPageResources);

  CPDF_Form(CPDF_Document* document,
            RetainPtr<CPDF_Dictionary> pPageResources,
            RetainPtr<CPDF_Stream> pFormStream);
  CPDF_Form(CPDF_Document* document,
            RetainPtr<CPDF_Dictionary> pPageResources,
            RetainPtr<CPDF_Stream> pFormStream,
            CPDF_Dictionary* pParentResources);
  ~CPDF_Form() override;

  // CPDF_Font::FormIface:
  void ParseContentForType3Char(CPDF_Type3Char* pType3Char) override;
  bool HasPageObjects() const override;
  CFX_FloatRect CalcBoundingBox() const override;
  std::optional<std::pair<RetainPtr<CFX_DIBitmap>, CFX_Matrix>>
  GetBitmapAndMatrixFromSoleImageOfForm() const override;

  // CPDF_PageObjectHolder:
  RetainPtr<CPDF_Stream> GetMutableFormStream() override;

  void ParseContent();
  void ParseContent(const CPDF_AllStates* pGraphicStates,
                    const CFX_Matrix* pParentMatrix,
                    RecursionState* recursion_state);

  // Never returns nullptr.
  RetainPtr<const CPDF_Stream> GetStream() const;

  // Rebinds this parsed form to a private clone of its backing stream. Call
  // before serializing per-placement edits so another use of the same Form
  // XObject cannot be changed or overwrite this form's sanitized content.
  bool CloneBackingStreamForWrite();

 private:
  // CPDF_PageObjectHolder:
  void EnsureMutableBackingObjectForDict() override;
  void EnsureMutableBackingObjectForResources() override;

  void RebindFormStream(RetainPtr<CPDF_Stream> stream,
                        bool reset_parsed_content);
  void ParseContentInternal(const CPDF_AllStates* pGraphicStates,
                            const CFX_Matrix* pParentMatrix,
                            CPDF_Type3Char* pType3Char,
                            RecursionState* recursion_state);

  RecursionState recursion_state_;
  RetainPtr<CPDF_Dictionary> const fallback_resources_;
  mutable RetainPtr<CPDF_Stream> form_stream_;
  mutable uint64_t form_stream_epoch_ = 0;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_FORM_H_
