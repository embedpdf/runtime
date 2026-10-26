// Copyright 2025 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Experimental EmbedPDF destination, named destination and action creators
// (public/epdf_doc.h). They are factored out of fpdf_doc.cpp to keep that
// file small; the outline's writers are in epdf_outline.cpp.

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
#include "core/fpdfdoc/cpdf_dest.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_object_helpers.h"
#include "public/epdf_doc.h"

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

  constexpr const char* kViewNames[] = {
    "Unknown", "XYZ", "Fit", "FitH", "FitV", "FitR", "FitB", "FitBH", "FitBV"};
  constexpr uint8_t kViewMaxParams[] = {0, 3, 0, 1, 1, 4, 0, 1, 1};

  // Non-XYZ: write N numeric params, padding with null up to |need|: a
  // missing /FitH top keeps the viewer's current one.
  static inline void AppendNonXYZParams(CPDF_Array* arr,
                                        const FS_FLOAT* params,
                                        unsigned long num_params,
                                        uint8_t need) {
    const unsigned long use = std::min<unsigned long>(num_params, need);
    for (unsigned long i = 0; i < need; ++i) {
      if (i < use && params) {
        arr->AppendNew<CPDF_Number>(static_cast<float>(params[i]));
      } else {
        arr->AppendNew<CPDF_Null>();
      }
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
  if (!epdf::IsIndirectObjectOf(doc, page_dict.Get()))
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

FPDF_EXPORT unsigned int FPDF_CALLCONV
EPDFDest_GetViewNullParams(FPDF_DEST dest) {
  const CPDF_Array* arr = CPDFArrayFromFPDFDest(dest);
  if (!arr)
    return 0;

  CPDF_Dest destination(pdfium::WrapRetain(arr));
  const size_t count = destination.GetNumParams();
  unsigned int mask = 0;
  for (size_t i = 0; i < count; ++i) {
    RetainPtr<const CPDF_Object> param = arr->GetDirectObjectAt(2 + i);
    if (!param || param->IsNull())
      mask |= 1u << i;
  }
  return mask;
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
  if (!epdf::IsIndirectObjectOf(pDoc, page_dict.Get()))
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

FPDF_EXPORT FPDF_ACTION FPDF_CALLCONV
EPDFAction_CreateNamed(FPDF_DOCUMENT document, FPDF_BYTESTRING name) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc || !name || !name[0])
    return nullptr;

  RetainPtr<CPDF_Dictionary> action = pDoc->NewIndirect<CPDF_Dictionary>();
  action->SetNewFor<CPDF_Name>("S", "Named");
  action->SetNewFor<CPDF_Name>("N", ByteString(name));

  return FPDFActionFromCPDFDictionary(action.Get());
}
