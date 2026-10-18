// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_outline.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <vector>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_object_helpers.h"
#include "fpdfsdk/epdf_outline_helpers.h"

namespace {

// The outline of the document a call works on: the document, and the number
// of its /Root /Outlines (0 when it has none). Empty when the document isn't
// one, or its /Outlines is a direct dictionary.
struct Outline {
  CPDF_Document* doc = nullptr;
  uint32_t root = 0;
};

std::optional<Outline> OutlineOf(CPDF_Document* doc) {
  if (!doc || !doc->GetRoot()) {
    return std::nullopt;
  }
  std::optional<uint32_t> root = epdf::OutlineRootOf(doc);
  if (!root) {
    return std::nullopt;
  }
  return Outline{doc, *root};
}

// The item |objnum| of |outline|, ready to write; null when it isn't one.
RetainPtr<CPDF_Dictionary> WritableItem(const Outline& outline,
                                        uint32_t objnum) {
  if (!epdf::IsOutlineItem(outline.doc, outline.root, objnum)) {
    return nullptr;
  }
  return ToDictionary(outline.doc->GetMutableIndirectObject(objnum));
}

// Where |after| is among |children|: the index an item put right after it
// takes, 0 for |after| 0, or nullopt when it isn't one of them.
std::optional<size_t> SlotAfter(const std::vector<uint32_t>& children,
                                uint32_t after) {
  if (!after) {
    return 0;
  }
  auto it = std::ranges::find(children, after);
  if (it == children.end()) {
    return std::nullopt;
  }
  return static_cast<size_t>(it - children.begin()) + 1;
}

// Whether |node| is |item| or under it: |item| is on |node|'s /Parent chain.
bool IsAtOrUnder(const Outline& outline, uint32_t node, uint32_t item) {
  std::set<uint32_t> seen;
  for (uint32_t current = node;
       current && current != outline.root && seen.insert(current).second;
       current = epdf::OutlineParentOf(outline.doc, current)) {
    if (current == item) {
      return true;
    }
  }
  return false;
}

WideString TitleOf(FPDF_WIDESTRING title) {
  // SAFETY: required from caller.
  return title ? UNSAFE_BUFFERS(WideStringFromFPDFWideString(title))
               : WideString();
}

bool IsColorComponent(float value) {
  return std::isfinite(value) && value >= 0 && value <= 1;
}

}  // namespace

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFOutline_GetObjectNumber(FPDF_BOOKMARK bookmark) {
  const CPDF_Dictionary* dict = CPDFDictionaryFromFPDFBookmark(bookmark);
  return dict ? dict->GetObjNum() : 0;
}

FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
EPDFOutline_GetItem(FPDF_DOCUMENT document, uint32_t objnum) {
  ScopedFPDFDocumentView document_view(document);
  std::optional<Outline> outline = OutlineOf(document_view.Get());
  if (!outline || !epdf::IsOutlineItem(outline->doc, outline->root, objnum)) {
    return nullptr;
  }
  return FPDFBookmarkFromCPDFDictionary(
      ToDictionary(outline->doc->GetOrParseIndirectObject(objnum)).Get());
}

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFOutline_CreateItem(FPDF_DOCUMENT document,
                       uint32_t objnum,
                       uint32_t parent,
                       uint32_t after,
                       FPDF_WIDESTRING title) {
  ScopedFPDFDocumentView document_view(document);
  std::optional<Outline> outline = OutlineOf(document_view.Get());
  if (!outline) {
    return 0;
  }
  CPDF_Document* doc = outline->doc;
  // Everything is checked before anything is written: a refusal changes
  // nothing.
  if (objnum && !doc->CanAddIndirectObjectAt(objnum)) {
    return 0;
  }
  if (parent && !epdf::IsOutlineItem(doc, outline->root, parent)) {
    return 0;
  }
  std::vector<uint32_t> children;
  if (const uint32_t node = parent ? parent : outline->root) {
    std::optional<std::vector<uint32_t>> numbers =
        epdf::OutlineChildNumbers(doc, node);
    if (!numbers) {
      return 0;
    }
    children = std::move(*numbers);
  }
  std::optional<size_t> slot = SlotAfter(children, after);
  if (!slot) {
    return 0;
  }

  if (!outline->root) {
    outline->root = epdf::CreateOutlineRoot(doc);
    if (!outline->root) {
      return 0;
    }
  }
  const uint32_t node = parent ? parent : outline->root;
  RetainPtr<CPDF_Dictionary> item =
      objnum ? doc->NewIndirectAt<CPDF_Dictionary>(objnum)
             : doc->NewIndirect<CPDF_Dictionary>();
  if (!item) {
    return 0;
  }
  item->SetNewFor<CPDF_String>("Title", TitleOf(title).AsStringView());
  children.insert(children.begin() + *slot, item->GetObjNum());
  if (!epdf::WriteOutlineChildren(doc, node, children) ||
      !epdf::RecountOutline(doc, outline->root, node)) {
    return 0;
  }
  return item->GetObjNum();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFOutline_MoveItem(FPDF_DOCUMENT document,
                                                         uint32_t objnum,
                                                         uint32_t parent,
                                                         uint32_t after) {
  ScopedFPDFDocumentView document_view(document);
  std::optional<Outline> outline = OutlineOf(document_view.Get());
  if (!outline || after == objnum ||
      !epdf::IsOutlineItem(outline->doc, outline->root, objnum)) {
    return false;
  }
  CPDF_Document* doc = outline->doc;
  if (parent && (!epdf::IsOutlineItem(doc, outline->root, parent) ||
                 IsAtOrUnder(*outline, parent, objnum))) {
    return false;
  }
  const uint32_t from = epdf::OutlineParentOf(doc, objnum);
  const uint32_t to = parent ? parent : outline->root;
  std::optional<std::vector<uint32_t>> old_children =
      epdf::OutlineChildNumbers(doc, from);
  if (!old_children) {
    return false;
  }
  std::erase(*old_children, objnum);
  std::optional<std::vector<uint32_t>> new_children =
      from == to ? old_children : epdf::OutlineChildNumbers(doc, to);
  if (!new_children) {
    return false;
  }
  std::optional<size_t> slot = SlotAfter(*new_children, after);
  if (!slot) {
    return false;
  }
  new_children->insert(new_children->begin() + *slot, objnum);

  if (from != to && !epdf::WriteOutlineChildren(doc, from, *old_children)) {
    return false;
  }
  return epdf::WriteOutlineChildren(doc, to, *new_children) &&
         epdf::RecountOutline(doc, outline->root, from) &&
         epdf::RecountOutline(doc, outline->root, to);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFOutline_DeleteItem(FPDF_DOCUMENT document, uint32_t objnum) {
  ScopedFPDFDocumentView document_view(document);
  std::optional<Outline> outline = OutlineOf(document_view.Get());
  if (!outline || !epdf::IsOutlineItem(outline->doc, outline->root, objnum)) {
    return false;
  }
  CPDF_Document* doc = outline->doc;
  const uint32_t parent = epdf::OutlineParentOf(doc, objnum);
  std::optional<std::vector<uint32_t>> siblings =
      epdf::OutlineChildNumbers(doc, parent);
  if (!siblings) {
    return false;
  }
  std::erase(*siblings, objnum);
  const std::vector<uint32_t> subtree = epdf::OutlineSubtreeOf(doc, objnum);
  if (!epdf::WriteOutlineChildren(doc, parent, *siblings)) {
    return false;
  }
  for (uint32_t item : subtree) {
    doc->DeleteIndirectObject(item);
  }
  return epdf::RecountOutline(doc, outline->root, parent);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFOutline_SetTitle(FPDF_DOCUMENT document,
                     uint32_t objnum,
                     FPDF_WIDESTRING title) {
  ScopedFPDFDocumentView document_view(document);
  std::optional<Outline> outline = OutlineOf(document_view.Get());
  RetainPtr<CPDF_Dictionary> item =
      outline ? WritableItem(*outline, objnum) : nullptr;
  if (!item) {
    return false;
  }
  item->SetNewFor<CPDF_String>("Title", TitleOf(title).AsStringView());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFOutline_SetOpen(FPDF_DOCUMENT document,
                                                        uint32_t objnum,
                                                        FPDF_BOOL open) {
  ScopedFPDFDocumentView document_view(document);
  std::optional<Outline> outline = OutlineOf(document_view.Get());
  if (!outline || !epdf::IsOutlineItem(outline->doc, outline->root, objnum)) {
    return false;
  }
  if (epdf::OutlineChildrenOf(outline->doc, objnum).empty()) {
    return true;  // a leaf has no open state
  }
  RetainPtr<CPDF_Dictionary> item = WritableItem(*outline, objnum);
  if (!item) {
    return false;
  }
  // Only the sign matters here: the count below makes it right.
  item->SetNewFor<CPDF_Number>("Count", open ? 1 : -1);
  return epdf::RecountOutline(outline->doc, outline->root, objnum);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFOutline_SetColor(FPDF_DOCUMENT document,
                                                         uint32_t objnum,
                                                         FPDF_BOOL has_color,
                                                         float r,
                                                         float g,
                                                         float b) {
  if (has_color &&
      (!IsColorComponent(r) || !IsColorComponent(g) || !IsColorComponent(b))) {
    return false;
  }
  ScopedFPDFDocumentView document_view(document);
  std::optional<Outline> outline = OutlineOf(document_view.Get());
  RetainPtr<CPDF_Dictionary> item =
      outline ? WritableItem(*outline, objnum) : nullptr;
  if (!item) {
    return false;
  }
  if (!has_color) {
    item->RemoveFor("C");
    return true;
  }
  RetainPtr<CPDF_Array> color = item->SetNewFor<CPDF_Array>("C");
  color->AppendNew<CPDF_Number>(r);
  color->AppendNew<CPDF_Number>(g);
  color->AppendNew<CPDF_Number>(b);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFOutline_SetStyle(FPDF_DOCUMENT document,
                                                         uint32_t objnum,
                                                         int flags) {
  if (flags < 0) {
    return false;
  }
  ScopedFPDFDocumentView document_view(document);
  std::optional<Outline> outline = OutlineOf(document_view.Get());
  RetainPtr<CPDF_Dictionary> item =
      outline ? WritableItem(*outline, objnum) : nullptr;
  if (!item) {
    return false;
  }
  if (flags) {
    item->SetNewFor<CPDF_Number>("F", flags);
  } else {
    item->RemoveFor("F");
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFOutline_SetAction(FPDF_DOCUMENT document,
                      uint32_t objnum,
                      FPDF_ACTION action) {
  ScopedFPDFDocumentView document_view(document);
  std::optional<Outline> outline = OutlineOf(document_view.Get());
  if (!outline) {
    return false;
  }
  const CPDF_Dictionary* action_dict = CPDFDictionaryFromFPDFAction(action);
  if (action && !epdf::IsIndirectObjectOf(outline->doc, action_dict)) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> item = WritableItem(*outline, objnum);
  if (!item) {
    return false;
  }
  item->RemoveFor("Dest");
  if (action_dict) {
    item->SetNewFor<CPDF_Reference>("A", outline->doc,
                                    action_dict->GetObjNum());
  } else {
    item->RemoveFor("A");
  }
  return true;
}
