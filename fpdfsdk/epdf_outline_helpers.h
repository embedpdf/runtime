// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// The mechanics of the document outline that its writers (epdf_outline.cpp)
// and its captures (epdf_capture.cpp) share: reading its lists, naming its
// items, and writing lists and counts the way public/epdf_outline.h promises.
// Callers open the document's view first (ScopedFPDFDocumentView).

#ifndef FPDFSDK_EPDF_OUTLINE_HELPERS_H_
#define FPDFSDK_EPDF_OUTLINE_HELPERS_H_

#include <stdint.h>

#include <optional>
#include <vector>

#include "core/fxcrt/retain_ptr.h"

class CPDF_Dictionary;
class CPDF_Document;

namespace epdf {

// One entry of a parent's list of children.
struct OutlineChild {
  // Its object number; 0 when it isn't an indirect object.
  uint32_t objnum = 0;
  RetainPtr<const CPDF_Dictionary> dict;
};

// The outline's own dictionary, /Root /Outlines: its object number, 0 when
// the document has none (or links something that isn't an outline there, see
// CPDF_BookmarkTree::IsOutlineNode()), or nullopt when it is a direct
// dictionary, which no item's /Parent can refer to.
std::optional<uint32_t> OutlineRootOf(CPDF_Document* doc);

// Makes a new /Root /Outlines and returns its object number, or 0.
uint32_t CreateOutlineRoot(CPDF_Document* doc);

// The children of |node| (the outline's dictionary or an item): its /First
// and the /Next that follow. A repeat, a reference to nothing, the outline's
// own dictionary, or a dictionary that isn't an item (a page, an action:
// CPDF_BookmarkTree::IsOutlineNode()) ends the list.
std::vector<OutlineChild> OutlineChildrenOf(CPDF_Document* doc, uint32_t node);

// The object numbers of |node|'s children, or nullopt when one of them isn't
// an indirect object: a list no write may change.
std::optional<std::vector<uint32_t>> OutlineChildNumbers(CPDF_Document* doc,
                                                         uint32_t node);

// Whether |objnum| is an item of the outline whose dictionary is |root|:
// each step up its /Parent chain is among the parent's children, up to
// |root|, in at most 128 steps (the depth reads of the outline go to).
bool IsOutlineItem(CPDF_Document* doc, uint32_t root, uint32_t objnum);

// The object number the item's /Parent refers to, or 0.
uint32_t OutlineParentOf(CPDF_Document* doc, uint32_t objnum);

// The item |objnum| and the items under it, the item first. A child goes
// with its parent only when its /Parent says so, so a list that wandered
// into another part of the tree never takes that part along, and each item
// is looked at once.
std::vector<uint32_t> OutlineSubtreeOf(CPDF_Document* doc, uint32_t objnum);

// Makes |children| the children of |node|, in order: their /Parent, /Prev
// and /Next, and |node|'s /First and /Last (both removed when there are
// none). Only the keys whose value changes are written.
bool WriteOutlineChildren(CPDF_Document* doc,
                          uint32_t node,
                          const std::vector<uint32_t>& children);

// Counts again from |start| up to |root|: each one's /Count from the tree,
// with its sign kept; |root|'s is never negative. An item without children
// loses its /Count.
bool RecountOutline(CPDF_Document* doc, uint32_t root, uint32_t start);

}  // namespace epdf

#endif  // FPDFSDK_EPDF_OUTLINE_HELPERS_H_
