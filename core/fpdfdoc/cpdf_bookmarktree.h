// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFDOC_CPDF_BOOKMARKTREE_H_
#define CORE_FPDFDOC_CPDF_BOOKMARKTREE_H_

#include "core/fpdfdoc/cpdf_bookmark.h"
#include "core/fxcrt/unowned_ptr.h"

// EmbedPDF(bug-fix): IsOutlineNode() below reads one.
class CPDF_Dictionary;
class CPDF_Document;

class CPDF_BookmarkTree {
 public:
  explicit CPDF_BookmarkTree(const CPDF_Document* doc);
  ~CPDF_BookmarkTree();

  // EmbedPDF(bug-fix): upstream took any dictionary that /Outlines, /First or
  // /Next reaches as part of the outline, so a broken file that links a page,
  // a field or an action there showed it as a bookmark, and an outline
  // writer rewrote its links. Whether |dict| can be the outline's dictionary
  // or one of its items: it has no key that gives a dictionary another role.
  static bool IsOutlineNode(const CPDF_Dictionary* dict);

  CPDF_Bookmark GetFirstChild(const CPDF_Bookmark& parent) const;
  CPDF_Bookmark GetNextSibling(const CPDF_Bookmark& bookmark) const;

 private:
  UnownedPtr<const CPDF_Document> const document_;
};

#endif  // CORE_FPDFDOC_CPDF_BOOKMARKTREE_H_
