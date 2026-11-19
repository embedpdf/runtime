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

// EmbedPDF(epdf-api): final. FPDFPage_CloseAnnot() deletes a handle as this
// type and the destructor is not virtual, so a subclass's members would never
// be destroyed: how a *Raw handle used to leak its page.
class CPDF_AnnotContext final {
 public:
  // EmbedPDF(measurement): owned SDK measurement state; never part of the
  // PDF graph.
  CPDF_MeasureStorage* GetMeasureStorage() const {
    return measure_storage_.get();
  }
  void SetMeasureStorage(std::unique_ptr<CPDF_MeasureStorage> storage) {
    measure_storage_ = std::move(storage);
  }

  // EmbedPDF(layer-door, annot-identity): the index the handle was found at
  // locates an inline annotation in its page's /Annots.
  CPDF_AnnotContext(RetainPtr<CPDF_Dictionary> pAnnotDict,
                    IPDF_Page* pPage,
                    int annot_index = -1);
  ~CPDF_AnnotContext();

  // EmbedPDF(epdf-api): a handle for a page nobody loaded (the *Raw calls).
  // It keeps |page|, the page object it was built on, and closing the handle
  // frees that page. Every other handle borrows a page its caller keeps open.
  static std::unique_ptr<CPDF_AnnotContext> CreateOwningPage(
      RetainPtr<CPDF_Dictionary> annot_dict,
      RetainPtr<IPDF_Page> page,
      int annot_index);

  // EmbedPDF(epdf-api): a handle to another annotation reached from this one
  // (/Popup, /IRT, /Parent), on the same page. It shares a page this handle
  // owns, so the two handles close in either order.
  std::unique_ptr<CPDF_AnnotContext> CreateLinked(
      RetainPtr<CPDF_Dictionary> linked_dict) const;

  // EmbedPDF(epdf-api): whether the handle keeps its own page (see
  // CreateOwningPage()). No form environment shows such a page, so the
  // form-fill calls fail for the handle without making a view for the page.
  bool OwnsPage() const { return !!owned_page_; }

  void SetForm(RetainPtr<CPDF_Stream> pStream);
  bool HasForm() const { return !!annot_form_; }
  CPDF_Form* GetForm() const { return annot_form_.get(); }

  // EmbedPDF(overlay-epoch, layer-door): both re-resolve when the overlay
  // epoch moved; the mutable one copies up through the layer's door.
  // Never nullptr. After the handle became invalid, the last version it
  // resolved to; callers at the API boundary check IsValid() first.
  RetainPtr<CPDF_Dictionary> GetMutableAnnotDict();
  const CPDF_Dictionary* GetAnnotDict() const;

  // EmbedPDF(annot-identity): whether the annotation this handle names is
  // still there: its object resolves, it is still in its page's /Annots
  // (once it has been), and its page is still in the document. Checked again
  // whenever the overlay epoch moves, so an abort that brings the
  // annotation back makes the handle valid again. Always true for ordinary
  // documents, whose epoch never moves.
  bool IsValid() const;

  // Never nullptr.
  IPDF_Page* GetPage() const { return page_; }

  // EmbedPDF(annot-identity): index at the time the annotation handle was
  // created, or -1 when the handle was not created from a page annotation
  // lookup.
  int GetAnnotIndex() const { return annot_index_; }

 private:
  // EmbedPDF(epdf-api): the page a *Raw handle keeps, else null. Declared
  // first so that it is destroyed last: the page outlives every member below
  // that points at it.
  RetainPtr<IPDF_Page> owned_page_;
  // EmbedPDF(measurement): see GetMeasureStorage().
  std::unique_ptr<CPDF_MeasureStorage> measure_storage_;
  // EmbedPDF(overlay-epoch): re-resolve the annotation when the overlay
  // epoch moved.
  void RefreshAnnotDictIfNeeded() const;
  // EmbedPDF(annot-identity): the annotation this handle names, as the
  // document has it now, or null when it is gone. Follows a promotion
  // through the birth list.
  RetainPtr<const CPDF_Dictionary> ResolveIdentity() const;
  // EmbedPDF(layer-door): open an inline annotation's owner for writing.
  void EnsureMutableBackingForAnnotDict();

  // EmbedPDF(overlay-epoch, layer-door): the dictionary and its form are
  // re-read when the overlay epoch moves, and the dictionary is swapped for
  // the layer's copy on a write.
  mutable std::unique_ptr<CPDF_Form> annot_form_;
  mutable RetainPtr<CPDF_Dictionary> annot_dict_;
  UnownedPtr<IPDF_Page> const page_;
  int annot_index_ = -1;
  mutable uint64_t annot_dict_epoch_ = 0;

  // EmbedPDF(annot-identity): an annotation with an object number is named
  // by it. An inline one on an unpromoted page of a layer is named by its
  // page and birth index: its position there, which promotion turns into an
  // object number through the layer's birth list.
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
