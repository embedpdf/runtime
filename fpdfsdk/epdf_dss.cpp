// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// The document security store functions declared in public/epdf_signature.h.
//
// The store is a dictionary in the catalog with three lists of streams and an
// optional index whose entries hold lists of the same three kinds. Reading
// flattens those into one list per kind; adding appends one stream to a
// kind's list. Neither looks inside a stream's bytes.

#include <stdint.h>

#include <set>
#include <utility>
#include <vector>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/epdf_signature.h"

namespace {

// Where one kind lives: the key of its list in the store, and the key of
// the same kind inside an index entry.
struct KindKeys {
  const char* list;
  const char* index_entry;
};

const KindKeys* KeysOf(int kind) {
  static constexpr KindKeys kCertificates = {"Certs", "Cert"};
  static constexpr KindKeys kOcspResponses = {"OCSPs", "OCSP"};
  static constexpr KindKeys kCrls = {"CRLs", "CRL"};
  switch (kind) {
    case EPDF_DSS_CERTIFICATES:
      return &kCertificates;
    case EPDF_DSS_OCSP_RESPONSES:
      return &kOcspResponses;
    case EPDF_DSS_CRLS:
      return &kCrls;
    default:
      return nullptr;
  }
}

// Walks the streams of one kind in the order the API counts them: the
// store's list, then what only index entries name, each stream once. The walk
// stops as soon as |wanted| is reached, so reading an item of the list never
// walks the index.
class StoreWalk {
 public:
  // |wanted| is the index to stop at, or -1 to count every stream.
  StoreWalk(const CPDF_Document* doc, const KindKeys& keys, int wanted)
      : wanted_(wanted) {
    const CPDF_Dictionary* catalog = doc->GetRoot();
    RetainPtr<const CPDF_Dictionary> store =
        catalog ? catalog->GetDictFor("DSS") : nullptr;
    if (!store) {
      return;
    }
    if (Visit(store->GetArrayFor(keys.list).Get())) {
      return;
    }
    RetainPtr<const CPDF_Dictionary> index = store->GetDictFor("VRI");
    if (!index) {
      return;
    }
    // CPDF_Dictionary iterates its std::map: the entries come in key order.
    for (const ByteString& key : index->GetKeys()) {
      RetainPtr<const CPDF_Dictionary> entry =
          index->GetDictFor(key.AsStringView());
      if (entry && Visit(entry->GetArrayFor(keys.index_entry).Get())) {
        return;
      }
    }
  }

  int count() const { return count_; }
  RetainPtr<const CPDF_Stream> found() const { return found_; }

 private:
  // Returns true when the wanted stream was reached.
  bool Visit(const CPDF_Array* array) {
    if (!array) {
      return false;
    }
    for (size_t i = 0; i < array->size(); ++i) {
      // A stream is always an indirect object, and its number is what makes
      // two references the same stream.
      RetainPtr<const CPDF_Reference> reference =
          ToReference(array->GetObjectAt(i));
      if (!reference) {
        continue;
      }
      RetainPtr<const CPDF_Stream> stream = array->GetStreamAt(i);
      if (!stream || !seen_.insert(reference->GetRefObjNum()).second) {
        continue;
      }
      if (count_ == wanted_) {
        found_ = std::move(stream);
        return true;
      }
      ++count_;
    }
    return false;
  }

  const int wanted_;
  int count_ = 0;
  std::set<uint32_t> seen_;
  RetainPtr<const CPDF_Stream> found_;
};

}  // namespace

FPDF_EXPORT int FPDF_CALLCONV EPDFDss_GetCount(FPDF_DOCUMENT document,
                                               int kind) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  const KindKeys* keys = KeysOf(kind);
  if (!doc || !keys) {
    return -1;
  }
  // References a base document's objects hold resolve through the layer.
  CPDF_DocumentViewScope document_view(doc);
  return StoreWalk(doc, *keys, -1).count();
}

FPDF_EXPORT unsigned long FPDF_CALLCONV EPDFDss_GetData(FPDF_DOCUMENT document,
                                                        int kind,
                                                        int index,
                                                        void* buffer,
                                                        unsigned long length) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  const KindKeys* keys = KeysOf(kind);
  if (!doc || !keys || index < 0) {
    return 0;
  }
  CPDF_DocumentViewScope document_view(doc);
  RetainPtr<const CPDF_Stream> stream = StoreWalk(doc, *keys, index).found();
  if (!stream) {
    return 0;
  }
  if (!buffer) {
    length = 0;
  }
  // SAFETY: required from caller.
  return DecodeStreamMaybeCopyAndReturnLength(
      std::move(stream),
      UNSAFE_BUFFERS(pdfium::span(static_cast<uint8_t*>(buffer),
                                  static_cast<size_t>(length))));
}

FPDF_EXPORT uint32_t FPDF_CALLCONV EPDFDss_Add(FPDF_DOCUMENT document,
                                               int kind,
                                               const void* data,
                                               unsigned long size) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  const KindKeys* keys = KeysOf(kind);
  if (!doc || !keys || !data || size == 0) {
    return 0;
  }
  CPDF_DocumentViewScope document_view(doc);
  const CPDF_Dictionary* catalog = doc->GetRoot();
  if (!catalog) {
    return 0;
  }

  // ---- Validate. ----
  // A store or a list that is there and is not what it should be is left as
  // it is: writing over it would drop whatever it holds.
  RetainPtr<const CPDF_Object> store_entry = catalog->GetObjectFor("DSS");
  RetainPtr<const CPDF_Dictionary> store = catalog->GetDictFor("DSS");
  if (store_entry && !store) {
    return 0;
  }
  RetainPtr<const CPDF_Object> list_entry =
      store ? store->GetObjectFor(keys->list) : nullptr;
  if (list_entry && !store->GetArrayFor(keys->list)) {
    return 0;
  }

  // ---- Apply. ----
  // Objects of their own are written through their numbers, so on a layer
  // the write lands in the layer's copy and never in the base's.
  RetainPtr<CPDF_Dictionary> writable_store;
  if (const CPDF_Reference* reference = ToReference(store_entry.Get())) {
    writable_store =
        ToDictionary(doc->GetMutableIndirectObject(reference->GetRefObjNum()));
  } else {
    RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
    if (!root) {
      return 0;
    }
    if (store_entry) {
      writable_store = root->GetMutableDictFor("DSS");
    } else {
      writable_store = doc->NewIndirect<CPDF_Dictionary>();
      root->SetNewFor<CPDF_Reference>("DSS", doc, writable_store->GetObjNum());
    }
  }
  if (!writable_store) {
    return 0;
  }
  RetainPtr<CPDF_Array> writable_list;
  if (const CPDF_Reference* reference = ToReference(list_entry.Get())) {
    writable_list =
        ToArray(doc->GetMutableIndirectObject(reference->GetRefObjNum()));
  } else if (list_entry) {
    writable_list = writable_store->GetMutableArrayFor(keys->list);
  } else {
    writable_list = writable_store->SetNewFor<CPDF_Array>(keys->list);
  }
  if (!writable_list) {
    return 0;
  }

  // SAFETY: required from caller.
  RetainPtr<CPDF_Stream> stream =
      doc->NewIndirect<CPDF_Stream>(UNSAFE_BUFFERS(pdfium::span(
          static_cast<const uint8_t*>(data), static_cast<size_t>(size))));
  writable_list->AppendNew<CPDF_Reference>(doc, stream->GetObjNum());
  return stream->GetObjNum();
}
