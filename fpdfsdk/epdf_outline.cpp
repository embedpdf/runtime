// Copyright 2025 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Experimental EmbedPDF outline/destination/action helpers and APIs.
// These functions are factored out of fpdf_doc.cpp to keep that file small.

#include "public/fpdf_doc.h"

#include <utility>

#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_null.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {  
  // Appends the page dictionary reference as the first element of |arr|.
  bool AppendPageDict(CPDF_Array* arr, CPDF_Page* page) {
    if (!arr || !page)
      return false;
    CPDF_Document* doc = page->GetDocument();
    if (!doc)
      return false;
    RetainPtr<const CPDF_Dictionary> dict = page->GetDict();
    if (!dict)
      return false;
    arr->AppendNew<CPDF_Reference>(doc, dict->GetObjNum());
    return true;
  }
  
  // Returns the document root's /Outlines dict, creating one if needed.
  RetainPtr<CPDF_Dictionary> GetOrCreateOutlines(CPDF_Document* doc) {
    CPDF_Dictionary* root = doc->GetMutableRoot();
    if (!root)
      return nullptr;
  
    // We plan to write under /Outlines, so use the mutable getter.
    RetainPtr<CPDF_Dictionary> outlines = root->GetMutableDictFor("Outlines");
    if (outlines)
      return outlines;
  
    outlines = doc->NewIndirect<CPDF_Dictionary>();
    outlines->SetNewFor<CPDF_Name>("Type", "Outlines");
    root->SetNewFor<CPDF_Reference>("Outlines", doc, outlines->GetObjNum());
    return outlines;
  }
  
  // Does |obj| belong to |doc| as an indirect object?
  bool BelongsTo(const CPDF_Document* doc, const CPDF_Object* obj) {
    if (!doc || !obj)
      return false;
    const uint32_t num = obj->GetObjNum();
    return num != 0 && doc->GetIndirectObject(num) == obj;
  }
  
  // Create a new indirect bookmark dictionary with a UTF-16 title.
  RetainPtr<CPDF_Dictionary> CreateBookmarkDict(CPDF_Document* doc,
                                                const WideString& title) {
    RetainPtr<CPDF_Dictionary> node = doc->NewIndirect<CPDF_Dictionary>();
    if (!title.IsEmpty())
      node->SetNewFor<CPDF_String>("Title", title.AsStringView());
    return node;
  }
  
  // Unlink |node| from its current parent/sibling list. Clears node's /Prev,/Next.
  // Leaves /Parent in place (caller will overwrite on insert or may inspect it).
  void UnlinkFromParent(CPDF_Document* doc, CPDF_Dictionary* node) {
    if (!node)
      return;
  
    RetainPtr<CPDF_Dictionary> parent = node->GetMutableDictFor("Parent");
    RetainPtr<CPDF_Dictionary> prev   = node->GetMutableDictFor("Prev");
    RetainPtr<CPDF_Dictionary> next   = node->GetMutableDictFor("Next");
  
    // Fix sibling pointers.
    if (prev) {
      if (next)
        prev->SetNewFor<CPDF_Reference>("Next", doc, next->GetObjNum());
      else
        prev->RemoveFor("Next");
    }
    if (next) {
      if (prev)
        next->SetNewFor<CPDF_Reference>("Prev", doc, prev->GetObjNum());
      else
        next->RemoveFor("Prev");
    }
  
    // Fix parent's /First and /Last.
    if (parent) {
      RetainPtr<CPDF_Dictionary> first = parent->GetMutableDictFor("First");
      RetainPtr<CPDF_Dictionary> last  = parent->GetMutableDictFor("Last");
      if (first && first.Get() == node) {
        if (next)
          parent->SetNewFor<CPDF_Reference>("First", doc, next->GetObjNum());
        else
          parent->RemoveFor("First");
      }
      if (last && last.Get() == node) {
        if (prev)
          parent->SetNewFor<CPDF_Reference>("Last", doc, prev->GetObjNum());
        else
          parent->RemoveFor("Last");
      }
    }
  
    node->RemoveFor("Prev");
    node->RemoveFor("Next");
  }
  
  // Insert |child| as the last child of |parent_like|.
  void AppendChild(CPDF_Document* doc,
                   CPDF_Dictionary* parent_like,
                   CPDF_Dictionary* child) {
    child->SetNewFor<CPDF_Reference>("Parent", doc, parent_like->GetObjNum());
  
    RetainPtr<CPDF_Dictionary> last = parent_like->GetMutableDictFor("Last");
    if (!last) {
      // First child.
      parent_like->SetNewFor<CPDF_Reference>("First", doc, child->GetObjNum());
      parent_like->SetNewFor<CPDF_Reference>("Last",  doc, child->GetObjNum());
      child->RemoveFor("Prev");
      child->RemoveFor("Next");
      return;
    }
  
    last->SetNewFor<CPDF_Reference>("Next", doc, child->GetObjNum());
    child->SetNewFor<CPDF_Reference>("Prev", doc, last->GetObjNum());
    child->RemoveFor("Next");
    parent_like->SetNewFor<CPDF_Reference>("Last", doc, child->GetObjNum());
  }
  
  // Insert |child| under |parent_like| right after |after_sibling|.
  // If |after_sibling| is null, insert as the first child.
  bool InsertAfter(CPDF_Document* doc,
                   CPDF_Dictionary* parent_like,
                   CPDF_Dictionary* after_sibling,  // may be null
                   CPDF_Dictionary* child) {
    child->SetNewFor<CPDF_Reference>("Parent", doc, parent_like->GetObjNum());
  
    if (!after_sibling) {
      // Insert as first child.
      RetainPtr<CPDF_Dictionary> first = parent_like->GetMutableDictFor("First");
      if (first) {
        first->SetNewFor<CPDF_Reference>("Prev", doc, child->GetObjNum());
        child->SetNewFor<CPDF_Reference>("Next", doc, first->GetObjNum());
      } else {
        // No children previously.
        parent_like->SetNewFor<CPDF_Reference>("Last", doc, child->GetObjNum());
        child->RemoveFor("Next");
      }
      parent_like->SetNewFor<CPDF_Reference>("First", doc, child->GetObjNum());
      child->RemoveFor("Prev");
      return true;
    }
  
    // Validate parent of after_sibling.
    RetainPtr<const CPDF_Dictionary> sib_parent =
        after_sibling->GetDictFor("Parent");
    if (!sib_parent || sib_parent.Get() != parent_like)
      return false;
  
    RetainPtr<CPDF_Dictionary> next = after_sibling->GetMutableDictFor("Next");
  
    after_sibling->SetNewFor<CPDF_Reference>("Next", doc, child->GetObjNum());
    child->SetNewFor<CPDF_Reference>("Prev", doc, after_sibling->GetObjNum());
  
    if (next) {
      next->SetNewFor<CPDF_Reference>("Prev", doc, child->GetObjNum());
      child->SetNewFor<CPDF_Reference>("Next", doc, next->GetObjNum());
    } else {
      parent_like->SetNewFor<CPDF_Reference>("Last", doc, child->GetObjNum());
      child->RemoveFor("Next");
    }
    return true;
  }
  
  // Recursively delete the subtree rooted at |node|.
  void DeleteSubtree(CPDF_Document* doc, CPDF_Dictionary* node) {
    // Delete children (depth-first).
    for (RetainPtr<CPDF_Dictionary> cur = node->GetMutableDictFor("First"); cur; ) {
      RetainPtr<CPDF_Dictionary> next = cur->GetMutableDictFor("Next");
      DeleteSubtree(doc, cur.Get());
      cur = std::move(next);
    }
  
    // Unlink self and delete indirect object.
    UnlinkFromParent(doc, node);
    node->RemoveFor("First");
    node->RemoveFor("Last");
    node->RemoveFor("Parent");
  
    const uint32_t num = node->GetObjNum();
    if (num)
      doc->DeleteIndirectObject(num);
  }
  

  constexpr const char* kViewNames[] = {
    "Unknown", "XYZ", "Fit", "FitH", "FitV", "FitR", "FitB", "FitBH", "FitBV"};
  constexpr uint8_t kViewMaxParams[] = {0, 3, 0, 1, 1, 4, 0, 1, 1};

  // Non-XYZ: write N numeric params, padding with 0.0 up to |need|.
  static inline void AppendNonXYZParams(CPDF_Array* arr,
                                        const FS_FLOAT* params,
                                        unsigned long num_params,
                                        uint8_t need) {
    const unsigned long use = std::min<unsigned long>(num_params, need);
    for (unsigned long i = 0; i < need; ++i) {
      const float v = (i < use && params) ? static_cast<float>(params[i]) : 0.0f;
      arr->AppendNew<CPDF_Number>(v);
    }
  }

  // XYZ: each of {left, top, zoom} may be null; zoom==0 means "unspecified".
  static inline void AppendXYZParams(CPDF_Array* arr,
                                    FPDF_BOOL has_left, FS_FLOAT left,
                                    FPDF_BOOL has_top,  FS_FLOAT top,
                                    FPDF_BOOL has_zoom, FS_FLOAT zoom) {
    if (has_left) {
      arr->AppendNew<CPDF_Number>(static_cast<float>(left));
    } else {
      arr->AppendNew<CPDF_Null>();
    }

    if (has_top) {
      arr->AppendNew<CPDF_Number>(static_cast<float>(top));
    } else {
      arr->AppendNew<CPDF_Null>();
    }

    if (has_zoom && zoom != 0.0f) {
      arr->AppendNew<CPDF_Number>(static_cast<float>(zoom));
    } else {
      arr->AppendNew<CPDF_Null>();
    }
  }

  static RetainPtr<CPDF_Array> GetOrCreateDestsNamesArray(CPDF_Document* doc) {
    CPDF_Dictionary* root = doc->GetMutableRoot();
    if (!root)
      return nullptr;
  
    RetainPtr<CPDF_Dictionary> names = root->GetMutableDictFor("Names");
    if (!names) {
      names = doc->NewIndirect<CPDF_Dictionary>();
      root->SetNewFor<CPDF_Reference>("Names", doc, names->GetObjNum());
    }
  
    RetainPtr<CPDF_Dictionary> dests = names->GetMutableDictFor("Dests");
    if (!dests) {
      dests = doc->NewIndirect<CPDF_Dictionary>();
      names->SetNewFor<CPDF_Reference>("Dests", doc, dests->GetObjNum());
    }
  
    RetainPtr<CPDF_Array> arr = dests->GetMutableArrayFor("Names");
    if (!arr) {
      arr = doc->NewIndirect<CPDF_Array>();
      dests->SetNewFor<CPDF_Reference>("Names", doc, arr->GetObjNum());
    }
    return arr;
  }
  
  // Insert/replace a (name, value) pair in the flat /Names array, keeping it sorted.
  // `value` may be DIRECT (that’s what we want).
  static bool SetNameTreePairSorted(CPDF_Document* doc,
                                    CPDF_Array* names_arr,
                                    const ByteString& key,
                                    RetainPtr<CPDF_Object> value) {
    if (!names_arr)
      return false;

    // Default insertion point is "append to end".
    int insert_at = static_cast<int>(names_arr->size());

    // Entries are (key, value) pairs at indices (0,1), (2,3), ...
    for (size_t i = 0; i + 1 < names_arr->size(); i += 2) {
      RetainPtr<const CPDF_Object> key_obj = names_arr->GetDirectObjectAt(i);
      ByteString cur = key_obj ? key_obj->GetString() : ByteString();

      const int cmp = key.Compare(cur.AsStringView());
      if (cmp == 0) {
        // Replace existing value at i+1.
        names_arr->SetAt(i + 1, std::move(value));
        return true;
      }
      if (cmp < 0) {
        insert_at = static_cast<int>(i);
        break;
      }
    }

    // Insert new (key, value) pair at `insert_at`.
    names_arr->InsertAt(insert_at, doc->New<CPDF_String>(key));
    names_arr->InsertAt(insert_at + 1, std::move(value));
    return true;
  }

  // Remove a (name, value) pair if present.
  static bool RemoveNameTreePair(CPDF_Array* names_arr, const ByteString& key) {
    if (!names_arr)
      return false;

    for (size_t i = 0; i + 1 < names_arr->size(); i += 2) {
      RetainPtr<const CPDF_Object> key_obj = names_arr->GetDirectObjectAt(i);
      ByteString cur = key_obj ? key_obj->GetString() : ByteString();
      if (cur == key.AsStringView()) {
        // Remove value then key.
        names_arr->RemoveAt(i + 1);
        names_arr->RemoveAt(i);
        return true;
      }
    }
    return false;
  }
}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNamedDest_SetDest(FPDF_DOCUMENT fdoc,
                      FPDF_BYTESTRING name,
                      FPDF_DEST fdest) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(fdoc);
  CPDF_Array* dest = CPDFArrayFromFPDFDest(fdest);
  if (!doc || !name || !dest)
    return false;

  // Must be indirect in this document.
  const uint32_t objnum = dest->GetObjNum();
  if (objnum == 0 || doc->GetIndirectObject(objnum) != dest)
    return false;

  RetainPtr<const CPDF_Object> first = dest->GetDirectObjectAt(0);
  RetainPtr<const CPDF_Dictionary> page_dict = ToDictionary(first);
  if (!page_dict)
    return false;
  if (!BelongsTo(doc, page_dict.Get()))
    return false;

  RetainPtr<CPDF_Array> names_arr = GetOrCreateDestsNamesArray(doc);
  if (!names_arr)
    return false;

  // Value is a reference to the dest array object.
  auto ref = pdfium::MakeRetain<CPDF_Reference>(doc, objnum);
  return SetNameTreePairSorted(doc, names_arr.Get(), ByteString(name), std::move(ref));
}

// Remove a named destination mapping (no orphaning; the value was DIRECT).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNamedDest_Remove(FPDF_DOCUMENT fdoc, FPDF_BYTESTRING name) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(fdoc);
  if (!doc || !name)
    return false;

  RetainPtr<CPDF_Array> names_arr = GetOrCreateDestsNamesArray(doc);
  if (!names_arr)
    return false;

  return RemoveNameTreePair(names_arr.Get(), ByteString(name));
}

FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDest_CreateView(FPDF_PAGE fpage,
                    unsigned long view,
                    const FS_FLOAT* params,
                    unsigned long num_params) {
  CPDF_Page* page = CPDFPageFromFPDFPage(fpage);
  if (!page)
    return nullptr;
  CPDF_Document* doc = page->GetDocument();
  if (!doc)
    return nullptr;

  // Validate view (must be a non-XYZ mode we know).
  if (view == PDFDEST_VIEW_XYZ || view < PDFDEST_VIEW_FIT || view > PDFDEST_VIEW_FITBV)
    return nullptr;

  const uint8_t need = kViewMaxParams[view];

  // INDIRECT array: [ pageRef /Fit* ... ]
  auto arr = doc->NewIndirect<CPDF_Array>();
  if (!AppendPageDict(arr.Get(), page))
    return nullptr;

  arr->AppendNew<CPDF_Name>(kViewNames[view]);
  AppendNonXYZParams(arr.Get(), params, num_params, need);
  return FPDFDestFromCPDFArray(arr.Get());
}

FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDest_CreateXYZ(FPDF_PAGE fpage,
                   FPDF_BOOL has_left, FS_FLOAT left,
                   FPDF_BOOL has_top,  FS_FLOAT top,
                   FPDF_BOOL has_zoom, FS_FLOAT zoom) {
  CPDF_Page* page = CPDFPageFromFPDFPage(fpage);
  if (!page)
    return nullptr;
  CPDF_Document* doc = page->GetDocument();
  if (!doc)
    return nullptr;

  // INDIRECT array: [ pageRef /XYZ left? top? zoom? ]
  auto arr = doc->NewIndirect<CPDF_Array>();
  if (!AppendPageDict(arr.Get(), page))
    return nullptr;

  arr->AppendNew<CPDF_Name>("XYZ");
  AppendXYZParams(arr.Get(), has_left, left, has_top, top, has_zoom, zoom);
  return FPDFDestFromCPDFArray(arr.Get());
}

FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDest_CreateRemoteView(FPDF_DOCUMENT fdoc,
                          int page_index,
                          unsigned long view,
                          const FS_FLOAT* params,
                          unsigned long num_params) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(fdoc);
  if (!doc || page_index < 0)
    return nullptr;
  if (view == PDFDEST_VIEW_XYZ || view < PDFDEST_VIEW_FIT || view > PDFDEST_VIEW_FITBV)
    return nullptr;

  const uint8_t need = kViewMaxParams[view];

  // Create an INDIRECT array: [ pageIndex /Fit* ... ]
  auto arr = doc->NewIndirect<CPDF_Array>();
  arr->AppendNew<CPDF_Number>(page_index);
  arr->AppendNew<CPDF_Name>(kViewNames[view]);
  AppendNonXYZParams(arr.Get(), params, num_params, need);
  return FPDFDestFromCPDFArray(arr.Get());
}

FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDest_CreateRemoteXYZ(FPDF_DOCUMENT fdoc,
                         int page_index,
                         FPDF_BOOL has_left, FS_FLOAT left,
                         FPDF_BOOL has_top,  FS_FLOAT top,
                         FPDF_BOOL has_zoom, FS_FLOAT zoom) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(fdoc);
  if (!doc || page_index < 0)
    return nullptr;

  // INDIRECT array: [ pageIndex /XYZ left? top? zoom? ]
  auto arr = doc->NewIndirect<CPDF_Array>();
  arr->AppendNew<CPDF_Number>(page_index);
  arr->AppendNew<CPDF_Name>("XYZ");
  AppendXYZParams(arr.Get(), has_left, left, has_top, top, has_zoom, zoom);
  return FPDFDestFromCPDFArray(arr.Get());
}

FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDest_CreateXYZByObjectNumber(FPDF_DOCUMENT fdoc,
                                 unsigned int page_obj_num,
                                 FPDF_BOOL has_left, FS_FLOAT left,
                                 FPDF_BOOL has_top,  FS_FLOAT top,
                                 FPDF_BOOL has_zoom, FS_FLOAT zoom) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(fdoc);
  if (!doc || page_obj_num == 0 || doc->GetPageIndex(page_obj_num) < 0)
    return nullptr;

  // INDIRECT array: [ pageRef /XYZ left? top? zoom? ]
  auto arr = doc->NewIndirect<CPDF_Array>();
  arr->AppendNew<CPDF_Reference>(doc, page_obj_num);
  arr->AppendNew<CPDF_Name>("XYZ");
  AppendXYZParams(arr.Get(), has_left, left, has_top, top, has_zoom, zoom);
  return FPDFDestFromCPDFArray(arr.Get());
}

FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDest_CreateViewByObjectNumber(FPDF_DOCUMENT fdoc,
                                  unsigned int page_obj_num,
                                  unsigned long view,
                                  const FS_FLOAT* params,
                                  unsigned long num_params) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(fdoc);
  if (!doc || page_obj_num == 0 || doc->GetPageIndex(page_obj_num) < 0)
    return nullptr;
  if (view == PDFDEST_VIEW_XYZ || view < PDFDEST_VIEW_FIT || view > PDFDEST_VIEW_FITBV)
    return nullptr;

  // INDIRECT array: [ pageRef /Fit* ... ]
  auto arr = doc->NewIndirect<CPDF_Array>();
  arr->AppendNew<CPDF_Reference>(doc, page_obj_num);
  arr->AppendNew<CPDF_Name>(kViewNames[view]);
  AppendNonXYZParams(arr.Get(), params, num_params, kViewMaxParams[view]);
  return FPDFDestFromCPDFArray(arr.Get());
}

FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateGoTo(FPDF_DOCUMENT document, FPDF_DEST dest) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc || !dest)
    return nullptr;

  CPDF_Array* pDestArray = CPDFArrayFromFPDFDest(dest);
  if (!pDestArray)
    return nullptr;

  // We expect |dest| to belong to |document| and to be indirect (created by EPDFDest_*()).
  // Bail out if it's not indirect to avoid cross-doc or inline ownership issues.
  if (pDestArray->GetObjNum() == 0)
    return nullptr;

  RetainPtr<const CPDF_Object> first = pDestArray->GetDirectObjectAt(0);
  RetainPtr<const CPDF_Dictionary> page_dict = ToDictionary(first);
  if (!page_dict)
    return nullptr;
  if (!BelongsTo(pDoc, page_dict.Get()))
    return nullptr;

  RetainPtr<CPDF_Dictionary> action = pDoc->NewIndirect<CPDF_Dictionary>();
  action->SetNewFor<CPDF_Name>("S", "GoTo");
  action->SetNewFor<CPDF_Reference>("D", pDoc, pDestArray->GetObjNum());

  return FPDFActionFromCPDFDictionary(action.Get());
}

FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateGoToNamed(FPDF_DOCUMENT fdoc, FPDF_BYTESTRING name) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(fdoc);
  if (!doc || !name)
    return nullptr;
  RetainPtr<CPDF_Dictionary> action = doc->NewIndirect<CPDF_Dictionary>();
  action->SetNewFor<CPDF_Name>("S", "GoTo");
  action->SetNewFor<CPDF_String>("D", ByteString(name));  // name-as-string
  return FPDFActionFromCPDFDictionary(action.Get());
}

FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateLaunch(FPDF_DOCUMENT document, FPDF_WIDESTRING file_path) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc || !file_path)
    return nullptr;

  WideString wpath = UNSAFE_BUFFERS(WideStringFromFPDFWideString(file_path));

  RetainPtr<CPDF_Dictionary> action = pDoc->NewIndirect<CPDF_Dictionary>();
  action->SetNewFor<CPDF_Name>("S", "Launch");
  // Simple FileSpec-as-string. Callers can upgrade to a full FileSpec later.
  action->SetNewFor<CPDF_String>("F", wpath.AsStringView());
  return FPDFActionFromCPDFDictionary(action.Get());
}

FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateRemoteGoToByName(FPDF_DOCUMENT document,
                                  FPDF_WIDESTRING file_path,
                                  FPDF_WIDESTRING named_dest) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc || !file_path || !named_dest)
    return nullptr;

  WideString wpath = UNSAFE_BUFFERS(WideStringFromFPDFWideString(file_path));
  WideString wname = UNSAFE_BUFFERS(WideStringFromFPDFWideString(named_dest));

  RetainPtr<CPDF_Dictionary> action = pDoc->NewIndirect<CPDF_Dictionary>();
  action->SetNewFor<CPDF_Name>("S", "GoToR");
  action->SetNewFor<CPDF_String>("F", wpath.AsStringView());
  // Named destination in the *target* doc, stored as a text string.
  action->SetNewFor<CPDF_String>("D", wname.AsStringView());
  return FPDFActionFromCPDFDictionary(action.Get());
}

FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateRemoteGoToDest(FPDF_DOCUMENT fdoc,
                                FPDF_WIDESTRING file_path,
                                FPDF_DEST fdest) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(fdoc);
  if (!doc || !file_path || !fdest)
    return nullptr;

  CPDF_Array* dest = CPDFArrayFromFPDFDest(fdest);
  if (!dest)
    return nullptr;

  // The dest must be an INDIRECT object in this doc.
  const uint32_t objnum = dest->GetObjNum();
  if (objnum == 0 || doc->GetIndirectObject(objnum) != dest)
    return nullptr;

  // Basic shape check for remote explicit destinations:
  // First element must be a page index (number).
  RetainPtr<const CPDF_Object> first = dest->GetDirectObjectAt(0);
  if (!first || !first->IsNumber())
    return nullptr;

  WideString wpath = UNSAFE_BUFFERS(WideStringFromFPDFWideString(file_path));

  auto action = doc->NewIndirect<CPDF_Dictionary>();
  action->SetNewFor<CPDF_Name>("S", "GoToR");
  action->SetNewFor<CPDF_String>("F", wpath.AsStringView());
  action->SetNewFor<CPDF_Reference>("D", doc, objnum);
  return FPDFActionFromCPDFDictionary(action.Get());
}

FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateURI(FPDF_DOCUMENT document, FPDF_BYTESTRING uri) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc || !uri)
    return nullptr;

  RetainPtr<CPDF_Dictionary> action = pDoc->NewIndirect<CPDF_Dictionary>();
  action->SetNewFor<CPDF_Name>("S", "URI");
  // Store as a byte string (UTF-8 as provided by caller).
  action->SetNewFor<CPDF_String>("URI", ByteString(uri));

  return FPDFActionFromCPDFDictionary(action.Get());
}

FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
EPDFBookmark_Create(FPDF_DOCUMENT doc, FPDF_WIDESTRING title) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(doc);
  if (!pDoc)
    return nullptr;

  RetainPtr<CPDF_Dictionary> outlines = GetOrCreateOutlines(pDoc);
  if (!outlines)
    return nullptr;

  WideString wtitle =
      title ? UNSAFE_BUFFERS(WideStringFromFPDFWideString(title)) : WideString();

  RetainPtr<CPDF_Dictionary> node = CreateBookmarkDict(pDoc, wtitle);
  AppendChild(pDoc, outlines.Get(), node.Get());
  return FPDFBookmarkFromCPDFDictionary(node.Get());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFBookmark_Delete(FPDF_DOCUMENT doc, FPDF_BOOKMARK bookmark) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(doc);
  RetainPtr<CPDF_Dictionary> bm(
      pdfium::WrapRetain(CPDFDictionaryFromFPDFBookmark(bookmark)));
  if (!pDoc || !bm || !BelongsTo(pDoc, bm.Get()))
    return false;

  DeleteSubtree(pDoc, bm.Get());
  return true;
}

FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
EPDFBookmark_AppendChild(FPDF_DOCUMENT doc,
                         FPDF_BOOKMARK parent,
                         FPDF_WIDESTRING title) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(doc);
  if (!pDoc)
    return nullptr;

  RetainPtr<CPDF_Dictionary> parent_like =
      parent ? pdfium::WrapRetain(CPDFDictionaryFromFPDFBookmark(parent))
             : GetOrCreateOutlines(pDoc);
  if (!parent_like || !BelongsTo(pDoc, parent_like.Get()))
    return nullptr;

  WideString wtitle =
      title ? UNSAFE_BUFFERS(WideStringFromFPDFWideString(title)) : WideString();
  RetainPtr<CPDF_Dictionary> child = CreateBookmarkDict(pDoc, wtitle);
  AppendChild(pDoc, parent_like.Get(), child.Get());
  return FPDFBookmarkFromCPDFDictionary(child.Get());
}

FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
EPDFBookmark_InsertAfter(FPDF_DOCUMENT doc,
                         FPDF_BOOKMARK parent,
                         FPDF_BOOKMARK after_sibling,
                         FPDF_WIDESTRING title) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(doc);
  if (!pDoc)
    return nullptr;

  RetainPtr<CPDF_Dictionary> parent_like =
      parent ? pdfium::WrapRetain(CPDFDictionaryFromFPDFBookmark(parent))
             : GetOrCreateOutlines(pDoc);
  if (!parent_like || !BelongsTo(pDoc, parent_like.Get()))
    return nullptr;

  CPDF_Dictionary* after_dict =
      after_sibling ? CPDFDictionaryFromFPDFBookmark(after_sibling) : nullptr;
  if (after_dict && !BelongsTo(pDoc, after_dict))
    return nullptr;

  WideString wtitle =
      title ? UNSAFE_BUFFERS(WideStringFromFPDFWideString(title)) : WideString();
  RetainPtr<CPDF_Dictionary> child = CreateBookmarkDict(pDoc, wtitle);

  if (!InsertAfter(pDoc, parent_like.Get(), after_dict, child.Get()))
    return nullptr;

  return FPDFBookmarkFromCPDFDictionary(child.Get());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFBookmark_Clear(FPDF_DOCUMENT fdoc) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(fdoc);
  if (!doc)
    return false;

  CPDF_Dictionary* root = doc->GetMutableRoot();
  if (!root)
    return false;

  RetainPtr<CPDF_Dictionary> outlines = root->GetMutableDictFor("Outlines");
  if (!outlines)
    return true;  // nothing to clear

  // Delete all top-level nodes.
  for (RetainPtr<CPDF_Dictionary> cur = outlines->GetMutableDictFor("First"); cur; ) {
    RetainPtr<CPDF_Dictionary> next = cur->GetMutableDictFor("Next");
    DeleteSubtree(doc, cur.Get());
    cur = std::move(next);
  }

  // Remove /Outlines from root and delete the dict object.
  root->RemoveFor("Outlines");
  const uint32_t num = outlines->GetObjNum();
  if (num)
    doc->DeleteIndirectObject(num);

  return true;
}


FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFBookmark_SetTitle(FPDF_BOOKMARK bookmark, FPDF_WIDESTRING title) {
  if (!bookmark)
    return false;

  RetainPtr<CPDF_Dictionary> bm_dict(
      pdfium::WrapRetain(CPDFDictionaryFromFPDFBookmark(bookmark)));
  if (!bm_dict)
    return false;

  WideString wide =
      title ? UNSAFE_BUFFERS(WideStringFromFPDFWideString(title)) : WideString();

  // Store as Unicode CPDF_String (matches existing patterns, e.g., Info keys).
  bm_dict->SetNewFor<CPDF_String>("Title", wide.AsStringView());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFBookmark_SetDest(FPDF_DOCUMENT doc, FPDF_BOOKMARK bookmark, FPDF_DEST dest) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(doc);
  if (!pDoc || !bookmark || !dest)
    return false;

  RetainPtr<CPDF_Dictionary> bm_dict(
      pdfium::WrapRetain(CPDFDictionaryFromFPDFBookmark(bookmark)));
  if (!bm_dict)
    return false;

  CPDF_Array* dest_arr = CPDFArrayFromFPDFDest(dest);
  if (!dest_arr)
    return false;

  // Require |dest| to be indirect to avoid cross-doc or inline ownership issues.
  if (dest_arr->GetObjNum() == 0)
    return false;

  RetainPtr<const CPDF_Object> first = dest_arr->GetDirectObjectAt(0);
  RetainPtr<const CPDF_Dictionary> page_dict = ToDictionary(first);
  if (!page_dict)
    return false;
  if (!BelongsTo(pDoc, page_dict.Get()))
    return false;

  // Clear any action.
  bm_dict->RemoveFor("A");

  // Set /Dest as an indirect reference into this document.
  bm_dict->SetNewFor<CPDF_Reference>("Dest", pDoc, dest_arr->GetObjNum());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFBookmark_SetAction(FPDF_DOCUMENT document,
                       FPDF_BOOKMARK bookmark,
                       FPDF_ACTION action) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc || !bookmark || !action)
    return false;

  RetainPtr<CPDF_Dictionary> bm_dict(
      pdfium::WrapRetain(CPDFDictionaryFromFPDFBookmark(bookmark)));
  if (!bm_dict)
    return false;

  CPDF_Dictionary* act_dict = CPDFDictionaryFromFPDFAction(action);
  if (!act_dict)
    return false;

  // Require the action to be indirect so we can reference it.
  if (act_dict->GetObjNum() == 0)
    return false;

  // Clear any /Dest when setting /A, per PDF spec.
  bm_dict->RemoveFor("Dest");

  // Set /A as an indirect reference to |action|.
  bm_dict->SetNewFor<CPDF_Reference>("A", pDoc, act_dict->GetObjNum());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFBookmark_ClearTarget(FPDF_BOOKMARK bookmark) {
  if (!bookmark)
    return false;
  RetainPtr<CPDF_Dictionary> bm_dict(
      pdfium::WrapRetain(CPDFDictionaryFromFPDFBookmark(bookmark)));
  if (!bm_dict)
    return false;

  bm_dict->RemoveFor("Dest");
  bm_dict->RemoveFor("A");
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFBookmark_SetNamedDest(FPDF_BOOKMARK bookmark, FPDF_BYTESTRING name) {
  if (!bookmark || !name)
    return false;
  RetainPtr<CPDF_Dictionary> bm(
      pdfium::WrapRetain(CPDFDictionaryFromFPDFBookmark(bookmark)));
  if (!bm)
    return false;
  bm->RemoveFor("A");
  bm->SetNewFor<CPDF_String>("Dest", ByteString(name));  // name-as-string
  return true;
}
