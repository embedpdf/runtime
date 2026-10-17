// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef FPDFSDK_EPDF_OBJECT_COPIER_H_
#define FPDFSDK_EPDF_OBJECT_COPIER_H_

#include <stdint.h>

#include <map>
#include <vector>

#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"

class CPDF_Document;
class CPDF_Reference;
class CPDF_Stream;

namespace epdf {

// Copies PDF objects into one document, wherever they live: another
// document, or the store an appearance drawn in memory keeps its fonts in.
//
// A copy follows references by what they resolve to, never by object number
// alone: an appearance drawn in memory numbers its fonts from 1 in a store of
// its own, so the same number in its source document is another object.
//
// Every indirect object is copied once per copier, however many copies reach
// it. An object the destination already holds is referenced, not copied. A
// reference to a page or to the page tree is dropped: a copied resource never
// brings a document's pages with it.
class ObjectCopier {
 public:
  explicit ObjectCopier(CPDF_Document* dest);
  ~ObjectCopier();

  ObjectCopier(const ObjectCopier&) = delete;
  ObjectCopier& operator=(const ObjectCopier&) = delete;

  // A value for the destination: a reference stays a reference, to the
  // destination's copy of what it resolves to; any other object is copied
  // with everything it reaches. Null when a reference leads nowhere.
  RetainPtr<CPDF_Object> Copy(const CPDF_Object* object);

  // The destination's indirect copy of |stream| (the stream itself when the
  // destination holds it already). Null when it can't be copied.
  RetainPtr<CPDF_Stream> CopyStream(const CPDF_Stream* stream);

  // The indirect object |source| was copied to, if it was: for a caller that
  // looks at what a copy brought along (the optional content groups of the
  // copied pages, say).
  using CopiedObjects = std::map<const CPDF_Object*, uint32_t>;
  const CopiedObjects& copied() const { return copied_; }

 private:
  // The destination's object number for the indirect object |source|; 0 when
  // it is dropped or can't be copied.
  uint32_t CopyIndirect(const CPDF_Object* source);
  uint32_t CopyReferenced(const CPDF_Reference* reference);
  // Points every reference inside |copy| (a fresh clone) at the
  // destination's copies, and makes a direct stream inside it indirect.
  void Retarget(CPDF_Object* copy);

  UnownedPtr<CPDF_Document> const dest_;
  CopiedObjects copied_;
  // Keeps the sources alive while their addresses key |copied_|.
  std::vector<RetainPtr<const CPDF_Object>> sources_;
};

}  // namespace epdf

#endif  // FPDFSDK_EPDF_OBJECT_COPIER_H_
