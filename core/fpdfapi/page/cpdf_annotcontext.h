// Copyright 2018 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PAGE_CPDF_ANNOTCONTEXT_H_
#define CORE_FPDFAPI_PAGE_CPDF_ANNOTCONTEXT_H_

#include <stdint.h>

#include <memory>
#include <utility>

#include "core/fpdfapi/parser/cpdf_measure_storage.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"

class CPDF_Dictionary;
class CPDF_Form;
class CPDF_Stream;
class IPDF_Page;

class CPDF_AnnotContext {
 public:
  // EmbedPDF: owned SDK measurement state; never part of the PDF graph.
  CPDF_MeasureStorage* GetMeasureStorage() const {
    return measure_storage_.get();
  }
  void SetMeasureStorage(std::unique_ptr<CPDF_MeasureStorage> storage) {
    measure_storage_ = std::move(storage);
  }

  CPDF_AnnotContext(RetainPtr<CPDF_Dictionary> pAnnotDict,
                    IPDF_Page* pPage,
                    int annot_index = -1);
  ~CPDF_AnnotContext();

  void SetForm(RetainPtr<CPDF_Stream> pStream);
  bool HasForm() const { return !!annot_form_; }
  CPDF_Form* GetForm() const { return annot_form_.get(); }

  // Never nullptr.
  RetainPtr<CPDF_Dictionary> GetMutableAnnotDict();
  const CPDF_Dictionary* GetAnnotDict() const;

  // Never nullptr.
  IPDF_Page* GetPage() const { return page_; }

  // Index at the time the annotation handle was created, or -1 when the
  // handle was not created from a page annotation lookup.
  int GetAnnotIndex() const { return annot_index_; }

 private:
  std::unique_ptr<CPDF_MeasureStorage> measure_storage_;
  void RefreshAnnotDictIfNeeded() const;
  void EnsureMutableBackingForAnnotDict();

  mutable std::unique_ptr<CPDF_Form> annot_form_;
  mutable RetainPtr<CPDF_Dictionary> annot_dict_;
  UnownedPtr<IPDF_Page> const page_;
  const int annot_index_ = -1;
  mutable uint64_t annot_dict_epoch_ = 0;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_ANNOTCONTEXT_H_
