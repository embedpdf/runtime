// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfdoc/cpdf_bookmarktree.h"

#include <utility>

#include "constants/catalog.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"

CPDF_BookmarkTree::CPDF_BookmarkTree(const CPDF_Document* doc)
    : document_(doc) {}

CPDF_BookmarkTree::~CPDF_BookmarkTree() = default;

// EmbedPDF(bug-fix): see the declaration. An item has no /Type, and the
// outline's dictionary may say /Outlines; the other keys belong to pages and
// the page tree (/Kids /Contents /MediaBox /Resources), annotations and
// fields (/Subtype /FT) and actions (/S, whose /Next chains actions).
// static
bool CPDF_BookmarkTree::IsOutlineNode(const CPDF_Dictionary* dict) {
  if (!dict) {
    return false;
  }
  if (dict->KeyExist("Type") && dict->GetNameFor("Type") != "Outlines") {
    return false;
  }
  for (const char* key :
       {"Subtype", "FT", "S", "Kids", "Contents", "MediaBox", "Resources"}) {
    if (dict->KeyExist(key)) {
      return false;
    }
  }
  return true;
}

namespace {

// EmbedPDF(bug-fix): |dict| as a bookmark when IsOutlineNode() takes it.
CPDF_Bookmark OutlineItem(RetainPtr<const CPDF_Dictionary> dict) {
  return CPDF_BookmarkTree::IsOutlineNode(dict.Get())
             ? CPDF_Bookmark(std::move(dict))
             : CPDF_Bookmark();
}

}  // namespace

CPDF_Bookmark CPDF_BookmarkTree::GetFirstChild(
    const CPDF_Bookmark& parent) const {
  const CPDF_Dictionary* parent_dict = parent.GetDict();
  if (parent_dict) {
    // EmbedPDF(bug-fix): only an item IsOutlineNode() takes.
    return OutlineItem(parent_dict->GetDictFor("First"));
  }

  const CPDF_Dictionary* root = document_->GetRoot();
  if (!root) {
    return CPDF_Bookmark();
  }

  RetainPtr<const CPDF_Dictionary> outlines =
      root->GetDictFor(pdfium::catalog::kOutlines);
  // EmbedPDF(bug-fix): only an outline and item IsOutlineNode() takes.
  return IsOutlineNode(outlines.Get())
             ? OutlineItem(outlines->GetDictFor("First"))
             : CPDF_Bookmark();
}

CPDF_Bookmark CPDF_BookmarkTree::GetNextSibling(
    const CPDF_Bookmark& bookmark) const {
  const CPDF_Dictionary* dict = bookmark.GetDict();
  if (!dict) {
    return CPDF_Bookmark();
  }

  RetainPtr<const CPDF_Dictionary> next = dict->GetDictFor("Next");
  // EmbedPDF(bug-fix): only an item IsOutlineNode() takes.
  return next != dict ? OutlineItem(std::move(next)) : CPDF_Bookmark();
}
