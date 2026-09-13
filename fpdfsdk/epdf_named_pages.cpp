// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_named_pages.h"

#include <memory>
#include <optional>
#include <utility>

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfdoc/cpdf_nametree.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

std::optional<ByteStringView> CategoryOf(int tree) {
  switch (tree) {
    case EPDF_NAMED_PAGE_TREE_PAGES:
      return ByteStringView("Pages");
    case EPDF_NAMED_PAGE_TREE_TEMPLATES:
      return ByteStringView("Templates");
    default:
      return std::nullopt;
  }
}

// Classify a name-tree value. The tree hands back the DIRECT object (a
// reference is resolved), so an indirect page dictionary still carries its
// object number while a deleted page (SetPageToNullObject) resolves to null.
int ClassifyValue(CPDF_Document* doc,
                  const CPDF_Object* value,
                  unsigned int* obj_num) {
  if (obj_num) {
    *obj_num = 0;
  }
  const CPDF_Dictionary* dict = value ? value->AsDictionary() : nullptr;
  if (!dict) {
    return EPDF_NAMED_PAGE_KIND_DANGLING;
  }
  const uint32_t objnum = dict->GetObjNum();
  if (objnum == 0) {
    // An inline dictionary can be neither a page nor a template.
    return EPDF_NAMED_PAGE_KIND_DANGLING;
  }
  if (obj_num) {
    *obj_num = objnum;
  }
  if (doc->GetPageIndex(objnum) >= 0) {
    return EPDF_NAMED_PAGE_KIND_PAGE;
  }
  if (dict->GetNameFor("Type") == "Template") {
    return EPDF_NAMED_PAGE_KIND_TEMPLATE;
  }
  if (obj_num) {
    *obj_num = 0;
  }
  return EPDF_NAMED_PAGE_KIND_DANGLING;
}

// Index of the entry whose decoded key equals |key|, or nullopt.
std::optional<size_t> FindIndexByKey(const CPDF_NameTree& tree,
                                     const WideString& key) {
  const size_t count = tree.GetCount();
  for (size_t i = 0; i < count; ++i) {
    WideString candidate;
    tree.LookupValueAndName(i, &candidate);
    if (candidate == key) {
      return i;
    }
  }
  return std::nullopt;
}

// Remove every entry matching |key|; the tree may hold duplicates from
// other producers. Returns how many were removed.
int RemoveAllByKey(CPDF_NameTree* tree, const WideString& key) {
  int removed = 0;
  while (true) {
    std::optional<size_t> index = FindIndexByKey(*tree, key);
    if (!index.has_value() || !tree->DeleteValueAndName(index.value())) {
      return removed;
    }
    ++removed;
  }
}

}  // namespace

FPDF_EXPORT int FPDF_CALLCONV EPDFDoc_GetNamedPageCount(FPDF_DOCUMENT document,
                                                        int tree) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  // Resolve through the effective view: a name tree reached from a frozen
  // base node must still see the layer's promoted nodes.
  CPDF_DocumentViewScope document_view(doc);
  std::optional<ByteStringView> category = CategoryOf(tree);
  if (!doc || !category.has_value()) {
    return -1;
  }
  auto name_tree = CPDF_NameTree::CreateForReading(doc, category.value());
  return name_tree ? pdfium::checked_cast<int>(name_tree->GetCount()) : 0;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetNamedPageAt(FPDF_DOCUMENT document,
                       int tree,
                       int index,
                       FPDF_WCHAR* buffer,
                       unsigned long buflen,
                       unsigned int* obj_num,
                       int* kind) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  // Resolve through the effective view: a name tree reached from a frozen
  // base node must still see the layer's promoted nodes.
  CPDF_DocumentViewScope document_view(doc);
  std::optional<ByteStringView> category = CategoryOf(tree);
  if (!doc || !category.has_value() || index < 0) {
    return 0;
  }
  auto name_tree = CPDF_NameTree::CreateForReading(doc, category.value());
  if (!name_tree || static_cast<size_t>(index) >= name_tree->GetCount()) {
    return 0;
  }

  WideString key;
  RetainPtr<CPDF_Object> value = name_tree->LookupValueAndName(index, &key);
  const int resolved = ClassifyValue(doc, value.Get(), obj_num);
  if (kind) {
    *kind = resolved;
  }
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      key, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_SetNamedPage(FPDF_DOCUMENT document,
                     FPDF_WIDESTRING key,
                     unsigned int page_obj_num) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  // Resolve through the effective view: a name tree reached from a frozen
  // base node must still see the layer's promoted nodes.
  CPDF_DocumentViewScope document_view(doc);
  if (!doc || !key || page_obj_num == 0) {
    return false;
  }
  const WideString wide_key = WideStringFromFPDFWideString(key);
  if (wide_key.IsEmpty()) {
    return false;
  }
  // Only pages in the page tree may be registered here; templates and
  // arbitrary dictionaries are refused.
  if (doc->GetPageIndex(page_obj_num) < 0) {
    return false;
  }

  // Mutable catalog path: creates /Names and /Names /Pages when absent and
  // lets layer documents promote the catalog before the edit.
  auto name_tree = CPDF_NameTree::CreateWithRootNameArray(doc, "Pages");
  if (!name_tree) {
    return false;
  }
  // Replace semantics: a set never leaves two entries for one key.
  RemoveAllByKey(name_tree.get(), wide_key);
  return name_tree->AddValueAndName(
      pdfium::MakeRetain<CPDF_Reference>(doc, page_obj_num), wide_key);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_RemoveNamedPage(FPDF_DOCUMENT document, FPDF_WIDESTRING key) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  // Resolve through the effective view: a name tree reached from a frozen
  // base node must still see the layer's promoted nodes.
  CPDF_DocumentViewScope document_view(doc);
  if (!doc || !key) {
    return false;
  }
  const WideString wide_key = WideStringFromFPDFWideString(key);
  if (wide_key.IsEmpty()) {
    return false;
  }
  // Create() (not CreateWithRootNameArray) so removing from a document
  // without a tree stays a no-op instead of manufacturing an empty one.
  auto name_tree = CPDF_NameTree::Create(doc, "Pages");
  if (!name_tree) {
    return false;
  }
  return RemoveAllByKey(name_tree.get(), wide_key) > 0;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFDoc_RemoveNamedPagesForPage(FPDF_DOCUMENT document,
                                unsigned int page_obj_num) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  // Resolve through the effective view: a name tree reached from a frozen
  // base node must still see the layer's promoted nodes.
  CPDF_DocumentViewScope document_view(doc);
  if (!doc || page_obj_num == 0) {
    return -1;
  }
  auto name_tree = CPDF_NameTree::Create(doc, "Pages");
  if (!name_tree) {
    return 0;
  }
  int removed = 0;
  // Walk from the back so deletions never shift entries still to be visited.
  size_t index = name_tree->GetCount();
  while (index > 0) {
    --index;
    WideString key;
    RetainPtr<CPDF_Object> value = name_tree->LookupValueAndName(index, &key);
    const CPDF_Dictionary* dict = value ? value->AsDictionary() : nullptr;
    if (dict && dict->GetObjNum() == page_obj_num &&
        name_tree->DeleteValueAndName(index)) {
      ++removed;
    }
  }
  return removed;
}
