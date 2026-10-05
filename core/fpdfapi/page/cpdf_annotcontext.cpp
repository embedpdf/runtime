// Copyright 2018 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/page/cpdf_annotcontext.h"

#include <utility>

#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"

namespace {

// Whether |annots| references object |objnum|.
bool AnnotsReference(const CPDF_Array* annots, uint32_t objnum) {
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    RetainPtr<const CPDF_Reference> ref = ToReference(annots->GetObjectAt(i));
    if (ref && ref->GetRefObjNum() == objnum) {
      return true;
    }
  }
  return false;
}

}  // namespace

CPDF_AnnotContext::CPDF_AnnotContext(RetainPtr<CPDF_Dictionary> pAnnotDict,
                                     IPDF_Page* pPage,
                                     int annot_index)
    : annot_dict_(std::move(pAnnotDict)),
      page_(pPage),
      annot_index_(annot_index) {
  DCHECK(annot_dict_);
  DCHECK(page_);
  DCHECK(page_->AsPDFPage());
  CPDF_Document* doc = page_->GetDocument();
  annot_dict_epoch_ = doc->GetOverlayEpoch();
  RetainPtr<const CPDF_Dictionary> page_dict = page_->AsPDFPage()->GetDict();
  page_objnum_ = page_dict ? page_dict->GetObjNum() : 0;
  objnum_ = annot_dict_->GetObjNum();
  if (objnum_ != 0) {
    was_member_ =
        AnnotsReference(page_->AsPDFPage()->GetAnnotsArray().Get(), objnum_);
    return;
  }
  // Inline: on a page a layer hasn't promoted, the position it sits at is
  // its birth index (creates append and updates edit in place, and
  // anything that would move an entry promotes the page first).
  const CPDF_LayerDocument* layer = CPDF_LayerDocument::FromDocument(doc);
  if (layer && annot_index_ >= 0 && page_objnum_ &&
      !layer->IsPagePromoted(page_objnum_)) {
    birth_index_ = annot_index_;
  }
}

CPDF_AnnotContext::~CPDF_AnnotContext() = default;

void CPDF_AnnotContext::SetForm(RetainPtr<CPDF_Stream> pStream) {
  CHECK(pStream);
  CPDF_DocumentViewScope document_view(page_->GetDocument());
  annot_form_ = std::make_unique<CPDF_Form>(
      page_->GetDocument(),
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(
          page_->AsPDFPage()->GetResources().Get())),
      pStream);

  // The annotation expects the form content to be parsed with the identity
  // matrix (ignoring the matrix defined in the stream). To achieve this without
  // mutating the stream, pass the inverse of the stream's matrix as the parent
  // matrix during parsing. The parent matrix is applied to the stream's matrix,
  // effectively canceling out to the identity matrix.
  CFX_Matrix inverse_stream_matrix =
      pStream->GetDict()->GetMatrixFor("Matrix").GetInverse();
  annot_form_->ParseContent(nullptr, &inverse_stream_matrix, nullptr);
}

RetainPtr<CPDF_Dictionary> CPDF_AnnotContext::GetMutableAnnotDict() {
  RefreshAnnotDictIfNeeded();

  CPDF_Page* page = page_ ? page_->AsPDFPage() : nullptr;
  CPDF_Document* doc = page ? page->GetDocument() : nullptr;
  if (!doc) {
    return annot_dict_;
  }

  const uint32_t objnum = annot_dict_->GetObjNum();
  if (objnum != 0) {
    RetainPtr<CPDF_Object> live = doc->GetMutableIndirectObject(objnum);
    if (live && live.Get() != annot_dict_.Get()) {
      annot_dict_ = pdfium::WrapRetain(live->AsMutableDictionary());
    }
    annot_dict_epoch_ = doc->GetOverlayEpoch();
    return annot_dict_;
  }

  // Inline: the dictionary is part of its owner (the page, or an indirect
  // /Annots array). When it can't be written in place - it is frozen, or a
  // layer transaction is open and it belongs to a committed version - open
  // the owner for writing (which copies it up in a transaction) and take the
  // dictionary from the owner's version.
  if (!annot_dict_->IsWritable()) {
    EnsureMutableBackingForAnnotDict();
  }
  annot_dict_epoch_ = doc->GetOverlayEpoch();
  return annot_dict_;
}

const CPDF_Dictionary* CPDF_AnnotContext::GetAnnotDict() const {
  RefreshAnnotDictIfNeeded();
  return annot_dict_.Get();
}

bool CPDF_AnnotContext::IsValid() const {
  RefreshAnnotDictIfNeeded();
  return valid_;
}

void CPDF_AnnotContext::RefreshAnnotDictIfNeeded() const {
  CPDF_Page* page = page_ ? page_->AsPDFPage() : nullptr;
  CPDF_Document* doc = page ? page->GetDocument() : nullptr;
  if (!doc) {
    return;
  }

  const uint64_t current_epoch = doc->GetOverlayEpoch();
  if (annot_dict_epoch_ == current_epoch) {
    return;
  }

  CPDF_DocumentViewScope document_view(doc);
  RetainPtr<const CPDF_Dictionary> effective = ResolveIdentity();
  // Invalid keeps the last version, so code already inside an API call
  // never meets a null dictionary; the API boundary checks IsValid().
  valid_ = !!effective;
  if (effective && effective.Get() != annot_dict_.Get()) {
    annot_dict_ =
        pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(effective.Get()));
    annot_form_.reset();
  }
  annot_dict_epoch_ = current_epoch;
}

RetainPtr<const CPDF_Dictionary> CPDF_AnnotContext::ResolveIdentity() const {
  CPDF_Page* page = page_->AsPDFPage();
  CPDF_Document* doc = page->GetDocument();
  // A deleted page takes its annotations with it.
  if (page_objnum_ && doc->GetPageIndex(page_objnum_) < 0) {
    return nullptr;
  }
  RetainPtr<const CPDF_Array> annots = page->GetAnnotsArray();

  // The identity is what the handle was made with, never the version it
  // resolved to last: after an aborted promotion, the object that version
  // was is gone, and the annotation is inline at its birth index again.
  uint32_t objnum = objnum_;
  if (birth_index_ >= 0) {
    // Born inline: promoted since (the birth list knows its object), or
    // still at its birth index.
    const CPDF_LayerDocument* layer = CPDF_LayerDocument::FromDocument(doc);
    objnum = layer ? layer->FindBirth(page_objnum_,
                                      static_cast<uint32_t>(birth_index_))
                   : 0;
    if (objnum == 0) {
      RetainPtr<const CPDF_Object> entry =
          annots && static_cast<size_t>(birth_index_) < annots->size()
              ? annots->GetObjectAt(static_cast<size_t>(birth_index_))
              : nullptr;
      return entry && entry->IsInline() ? ToDictionary(entry) : nullptr;
    }
  }

  if (objnum != 0) {
    RetainPtr<const CPDF_Dictionary> effective =
        ToDictionary(doc->GetIndirectObject(objnum));
    if (!effective) {
      return nullptr;  // created in an aborted transaction, or deleted
    }
    // A base annotation keeps resolving after it is deleted (rule 6): only
    // its page stops referencing it.
    if (AnnotsReference(annots.Get(), objnum)) {
      was_member_ = true;
    } else if (was_member_) {
      return nullptr;
    }
    return effective;
  }

  // Inline without a birth name (not a layer, or added inline later): by
  // the index it was found at, as before.
  if (annot_index_ >= 0) {
    RetainPtr<const CPDF_Dictionary> effective =
        annots && static_cast<size_t>(annot_index_) < annots->size()
            ? annots->GetDictAt(static_cast<size_t>(annot_index_))
            : nullptr;
    if (effective) {
      return effective;
    }
  }
  return annot_dict_;
}

void CPDF_AnnotContext::EnsureMutableBackingForAnnotDict() {
  CPDF_Page* page = page_->AsPDFPage();
  if (annot_index_ < 0) {
    // Made without its index (an annotation just appended, or reached through
    // a link): find it in the page's /Annots by identity.
    RetainPtr<const CPDF_Array> annots = page->GetAnnotsArray();
    for (size_t i = 0; annots && i < annots->size(); ++i) {
      if (annots->GetDictAt(i).Get() == annot_dict_.Get()) {
        annot_index_ = static_cast<int>(i);
        break;
      }
    }
    if (annot_index_ < 0 && !annot_dict_->IsFrozen()) {
      // Not in /Annots under this identity any more: nothing to open. The
      // write lands on this dictionary, as it always did; if that is a
      // committed version inside a transaction, the generation check reports
      // it. Locating it after its owner was copied needs the birth list (fork
      // plan T2, rule L3).
      return;
    }
  }
  CHECK_GE(annot_index_, 0);
  // The owner is the indirect object holding the dictionary: an indirect
  // /Annots array if the page has one (the page itself stays as it is), else
  // the page.
  RetainPtr<CPDF_Array> annots;
  RetainPtr<const CPDF_Reference> annots_ref =
      ToReference(page->GetDict()->GetObjectFor("Annots"));
  if (annots_ref) {
    annots = ToArray(page->GetDocument()->GetMutableIndirectObject(
        annots_ref->GetRefObjNum()));
  } else {
    annots = page->GetMutableDict()->GetMutableArrayFor("Annots");
  }
  CHECK(annots);
  annot_dict_ = annots->GetMutableDictAt(annot_index_);
  CHECK(annot_dict_);
}
