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

  // Never nullptr. After the handle became invalid, the last version it
  // resolved to; callers at the API boundary check IsValid() first.
  RetainPtr<CPDF_Dictionary> GetMutableAnnotDict();
  const CPDF_Dictionary* GetAnnotDict() const;

  // EmbedPDF: whether the annotation this handle names is still there (fork
  // plan L1): its object resolves, it is still in its page's /Annots (once
  // it has been), and its page is still in the document. Checked again
  // whenever the overlay epoch moves, so an abort that brings the
  // annotation back makes the handle valid again. Always true for ordinary
  // documents, whose epoch never moves.
  bool IsValid() const;

  // Never nullptr.
  IPDF_Page* GetPage() const { return page_; }

  // Index at the time the annotation handle was created, or -1 when the
  // handle was not created from a page annotation lookup.
  int GetAnnotIndex() const { return annot_index_; }

 private:
  std::unique_ptr<CPDF_MeasureStorage> measure_storage_;
  void RefreshAnnotDictIfNeeded() const;
  // The annotation this handle names, as the document has it now, or null
  // when it is gone (L1). Follows a promotion through the birth list (L3).
  RetainPtr<const CPDF_Dictionary> ResolveIdentity() const;
  void EnsureMutableBackingForAnnotDict();

  mutable std::unique_ptr<CPDF_Form> annot_form_;
  mutable RetainPtr<CPDF_Dictionary> annot_dict_;
  UnownedPtr<IPDF_Page> const page_;
  int annot_index_ = -1;
  mutable uint64_t annot_dict_epoch_ = 0;

  // EmbedPDF identity (fork plan §4.14). An annotation with an object number
  // is named by it. An inline one on an unpromoted page of a layer is named
  // by its page and birth index: its position there, which promotion turns
  // into an object number through the layer's birth list.
  uint32_t objnum_ = 0;
  uint32_t page_objnum_ = 0;
  int birth_index_ = -1;
  mutable bool valid_ = true;
  // Seen in its page's /Annots: from then on, leaving it ends the handle.
  // A handle to an annotation that never was a member there (a linked one on
  // another page) is checked by resolution only.
  mutable bool was_member_ = false;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_ANNOTCONTEXT_H_
