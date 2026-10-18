// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "fpdfsdk/epdf_outline_helpers.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"

namespace epdf {

namespace {

RetainPtr<const CPDF_Dictionary> DictOf(CPDF_Document* doc, uint32_t objnum) {
  return objnum ? ToDictionary(doc->GetOrParseIndirectObject(objnum)) : nullptr;
}

// The object number |dict| refers to under |key|, or 0.
uint32_t LinkOf(const CPDF_Dictionary* dict, ByteStringView key) {
  RetainPtr<const CPDF_Object> value = dict ? dict->GetObjectFor(key) : nullptr;
  const CPDF_Reference* ref = value ? value->AsReference() : nullptr;
  return ref ? ref->GetRefObjNum() : 0;
}

bool IsOpen(const CPDF_Dictionary* item) {
  return item->GetIntegerFor("Count") > 0;
}

// The children of |dict|, whose own number is |self| (0 when it has none).
std::vector<OutlineChild> ChildrenOfDict(CPDF_Document* doc,
                                         const CPDF_Dictionary* dict,
                                         uint32_t self) {
  std::vector<OutlineChild> children;
  if (!dict) {
    return children;
  }
  std::set<uint32_t> seen;
  if (self) {
    seen.insert(self);
  }
  RetainPtr<const CPDF_Object> link = dict->GetObjectFor("First");
  while (link) {
    OutlineChild child;
    if (const CPDF_Reference* ref = link->AsReference()) {
      child.objnum = ref->GetRefObjNum();
      if (!seen.insert(child.objnum).second) {
        break;  // a loop
      }
      child.dict = DictOf(doc, child.objnum);
    } else {
      child.dict = ToDictionary(link);
    }
    if (!child.dict) {
      break;  // a reference to nothing, or not a dictionary
    }
    link = child.dict->GetObjectFor("Next");
    children.push_back(std::move(child));
  }
  return children;
}

// How many items show under |node| when it is open: each child, and what
// shows under each open child, read from the tree. |known| holds what was
// worked out already, by object number.
int VisibleUnder(CPDF_Document* doc,
                 uint32_t node,
                 const std::map<uint32_t, int>& known) {
  int total = 0;
  std::set<uint32_t> expanded = {node};
  std::vector<OutlineChild> pending =
      ChildrenOfDict(doc, DictOf(doc, node).Get(), node);
  while (!pending.empty()) {
    OutlineChild child = std::move(pending.back());
    pending.pop_back();
    ++total;
    if (!IsOpen(child.dict.Get())) {
      continue;
    }
    if (child.objnum) {
      if (auto it = known.find(child.objnum); it != known.end()) {
        total += it->second;
        continue;
      }
      if (!expanded.insert(child.objnum).second) {
        continue;  // a loop
      }
    }
    for (OutlineChild& grandchild :
         ChildrenOfDict(doc, child.dict.Get(), child.objnum)) {
      pending.push_back(std::move(grandchild));
    }
  }
  return total;
}

RetainPtr<CPDF_Dictionary> WritableDictOf(CPDF_Document* doc, uint32_t objnum) {
  return ToDictionary(doc->GetMutableIndirectObject(objnum));
}

// Makes |objnum|'s |key| refer to |target| (0 removes it), unless it does.
bool SetLink(CPDF_Document* doc,
             uint32_t objnum,
             const char* key,
             uint32_t target) {
  RetainPtr<const CPDF_Dictionary> dict = DictOf(doc, objnum);
  if (!dict) {
    return false;
  }
  const bool present = dict->KeyExist(key);
  if (target ? present && LinkOf(dict.Get(), key) == target : !present) {
    return true;
  }
  RetainPtr<CPDF_Dictionary> writable = WritableDictOf(doc, objnum);
  if (!writable) {
    return false;
  }
  if (target) {
    writable->SetNewFor<CPDF_Reference>(key, doc, target);
  } else {
    writable->RemoveFor(key);
  }
  return true;
}

// Makes |objnum|'s /Count |count| (0 removes it), unless it is.
bool SetCount(CPDF_Document* doc, uint32_t objnum, int count) {
  RetainPtr<const CPDF_Dictionary> dict = DictOf(doc, objnum);
  if (!dict) {
    return false;
  }
  RetainPtr<const CPDF_Object> value = dict->GetObjectFor("Count");
  if (count ? value && value->IsNumber() && value->GetInteger() == count
            : !value) {
    return true;
  }
  RetainPtr<CPDF_Dictionary> writable = WritableDictOf(doc, objnum);
  if (!writable) {
    return false;
  }
  if (count) {
    writable->SetNewFor<CPDF_Number>("Count", count);
  } else {
    writable->RemoveFor("Count");
  }
  return true;
}

}  // namespace

std::optional<uint32_t> OutlineRootOf(CPDF_Document* doc) {
  const CPDF_Dictionary* catalog = doc->GetRoot();
  RetainPtr<const CPDF_Object> value =
      catalog ? catalog->GetObjectFor("Outlines") : nullptr;
  if (!value) {
    return 0;
  }
  const CPDF_Reference* ref = value->AsReference();
  if (!ref) {
    return value->IsDictionary() ? std::nullopt : std::optional<uint32_t>(0);
  }
  return DictOf(doc, ref->GetRefObjNum()) ? ref->GetRefObjNum() : 0;
}

uint32_t CreateOutlineRoot(CPDF_Document* doc) {
  RetainPtr<CPDF_Dictionary> catalog = doc->GetMutableRoot();
  if (!catalog) {
    return 0;
  }
  RetainPtr<CPDF_Dictionary> outlines = doc->NewIndirect<CPDF_Dictionary>();
  outlines->SetNewFor<CPDF_Name>("Type", "Outlines");
  catalog->SetNewFor<CPDF_Reference>("Outlines", doc, outlines->GetObjNum());
  return outlines->GetObjNum();
}

std::vector<OutlineChild> OutlineChildrenOf(CPDF_Document* doc, uint32_t node) {
  return ChildrenOfDict(doc, DictOf(doc, node).Get(), node);
}

std::optional<std::vector<uint32_t>> OutlineChildNumbers(CPDF_Document* doc,
                                                         uint32_t node) {
  std::vector<uint32_t> numbers;
  for (const OutlineChild& child : OutlineChildrenOf(doc, node)) {
    if (!child.objnum) {
      return std::nullopt;
    }
    numbers.push_back(child.objnum);
  }
  return numbers;
}

bool IsOutlineItem(CPDF_Document* doc, uint32_t root, uint32_t objnum) {
  if (!root || !objnum || objnum == root) {
    return false;
  }
  std::set<uint32_t> seen;
  for (uint32_t node = objnum; node != root;) {
    if (!seen.insert(node).second) {
      return false;  // a loop in the /Parent chain
    }
    const uint32_t parent = OutlineParentOf(doc, node);
    if (!parent) {
      return false;
    }
    const std::vector<OutlineChild> siblings = OutlineChildrenOf(doc, parent);
    if (std::ranges::none_of(siblings, [node](const OutlineChild& child) {
          return child.objnum == node;
        })) {
      return false;
    }
    node = parent;
  }
  return true;
}

uint32_t OutlineParentOf(CPDF_Document* doc, uint32_t objnum) {
  return LinkOf(DictOf(doc, objnum).Get(), "Parent");
}

std::vector<uint32_t> OutlineSubtreeOf(CPDF_Document* doc, uint32_t objnum) {
  std::vector<uint32_t> items = {objnum};
  std::set<uint32_t> seen = {objnum};
  for (size_t i = 0; i < items.size(); ++i) {
    const uint32_t parent = items[i];
    for (const OutlineChild& child : OutlineChildrenOf(doc, parent)) {
      if (child.objnum && LinkOf(child.dict.Get(), "Parent") == parent &&
          seen.insert(child.objnum).second) {
        items.push_back(child.objnum);
      }
    }
  }
  return items;
}

bool WriteOutlineChildren(CPDF_Document* doc,
                          uint32_t node,
                          const std::vector<uint32_t>& children) {
  for (size_t i = 0; i < children.size(); ++i) {
    const uint32_t prev = i > 0 ? children[i - 1] : 0;
    const uint32_t next = i + 1 < children.size() ? children[i + 1] : 0;
    if (!SetLink(doc, children[i], "Parent", node) ||
        !SetLink(doc, children[i], "Prev", prev) ||
        !SetLink(doc, children[i], "Next", next)) {
      return false;
    }
  }
  return SetLink(doc, node, "First", children.empty() ? 0 : children.front()) &&
         SetLink(doc, node, "Last", children.empty() ? 0 : children.back());
}

bool RecountOutline(CPDF_Document* doc, uint32_t root, uint32_t start) {
  std::map<uint32_t, int> known;
  std::set<uint32_t> seen;
  for (uint32_t node = start; node && seen.insert(node).second;) {
    RetainPtr<const CPDF_Dictionary> dict = DictOf(doc, node);
    if (!dict) {
      return false;
    }
    const int visible = VisibleUnder(doc, node, known);
    known[node] = visible;
    int count = 0;
    if (visible > 0) {
      count = node == root || IsOpen(dict.Get()) ? visible : -visible;
    }
    if (!SetCount(doc, node, count)) {
      return false;
    }
    if (node == root) {
      break;
    }
    node = LinkOf(dict.Get(), "Parent");
  }
  return true;
}

}  // namespace epdf
