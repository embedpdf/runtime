// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_checkpoint.h"

#include <memory>
#include <utility>
#include <vector>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fxcrt/unowned_ptr.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

// What a group of writes can change, recorded before the first of them.
class EpdfCheckpoint {
 public:
  explicit EpdfCheckpoint(CPDF_Document* doc)
      : doc_(doc), mark_(doc->GetLastObjNum()) {
    CPDF_DocumentViewScope document_view(doc_);
    const CPDF_Dictionary* root = doc_->GetRoot();
    if (!root) {
      return;
    }
    Record(root->GetObjNum());
    RetainPtr<const CPDF_Dictionary> form = root->GetDictFor("AcroForm");
    if (!form) {
      return;
    }
    Record(form->GetObjNum());
    RetainPtr<const CPDF_Dictionary> resources = form->GetDictFor("DR");
    if (!resources) {
      return;
    }
    Record(resources->GetObjNum());
    RetainPtr<const CPDF_Dictionary> fonts = resources->GetDictFor("Font");
    if (fonts) {
      Record(fonts->GetObjNum());
    }
  }

  bool RecordPage(int index) {
    for (const Page& recorded : pages_) {
      if (recorded.index == index) {
        return true;
      }
    }
    CPDF_DocumentViewScope document_view(doc_);
    RetainPtr<const CPDF_Dictionary> page = doc_->GetPageDictionary(index);
    if (!page) {
      return false;
    }
    Page recorded;
    recorded.index = index;
    RetainPtr<const CPDF_Object> annots = page->GetObjectFor("Annots");
    if (annots) {
      recorded.had_annots = true;
      if (const CPDF_Reference* reference = annots->AsReference()) {
        recorded.annots_number = reference->GetRefObjNum();
        RetainPtr<CPDF_Object> array =
            doc_->GetOrParseIndirectObject(recorded.annots_number);
        if (!array) {
          return false;
        }
        recorded.annots = array->Clone();
      } else {
        recorded.annots = annots->Clone();
      }
    }
    pages_.push_back(std::move(recorded));
    return true;
  }

  // An object the writes will change: its value now. One numbered above the
  // mark is new, and goes on a rollback anyway.
  bool RecordObject(uint32_t number) {
    if (number > mark_) {
      return true;
    }
    CPDF_DocumentViewScope document_view(doc_);
    RetainPtr<CPDF_Object> object = doc_->GetOrParseIndirectObject(number);
    if (!object || !object->IsDictionary()) {
      return false;
    }
    Record(number);
    return true;
  }

  bool Rollback() {
    CPDF_DocumentViewScope document_view(doc_);
    bool restored = true;
    for (const auto& [number, copy] : objects_) {
      RetainPtr<CPDF_Dictionary> live =
          doc_->GetRoot() && number == doc_->GetRoot()->GetObjNum()
              ? doc_->GetMutableRoot()
              : ToDictionary(doc_->GetMutableIndirectObject(number));
      if (!live) {
        restored = false;
        continue;
      }
      Replace(live.Get(), copy->AsDictionary());
    }
    for (const Page& recorded : pages_) {
      if (recorded.annots_number) {
        // An indirect /Annots: the page keeps its reference, and the array
        // gets its entries back.
        RetainPtr<CPDF_Array> array =
            ToArray(doc_->GetMutableIndirectObject(recorded.annots_number));
        if (!array) {
          restored = false;
          continue;
        }
        array->Clear();
        const CPDF_Array* copy = recorded.annots->AsArray();
        for (size_t i = 0; copy && i < copy->size(); ++i) {
          array->Append(copy->GetObjectAt(i)->Clone());
        }
        continue;
      }
      RetainPtr<CPDF_Dictionary> page =
          doc_->GetMutablePageDictionary(recorded.index);
      if (!page) {
        restored = false;
        continue;
      }
      if (recorded.had_annots) {
        page->SetFor("Annots", recorded.annots->Clone());
      } else {
        page->RemoveFor("Annots");
      }
    }
    for (uint32_t number = doc_->GetLastObjNum(); number > mark_; --number) {
      doc_->DeleteIndirectObject(number);
    }
    // Given back: without this, the next incremental save still writes the
    // larger /Size.
    doc_->SetLastObjNum(mark_);
    return restored;
  }

 private:
  struct Page {
    int index = 0;
    bool had_annots = false;
    uint32_t annots_number = 0;  // 0: a direct array.
    RetainPtr<CPDF_Object> annots;
  };

  // An indirect dictionary's value now; direct ones travel inside their
  // owner's copy.
  void Record(uint32_t number) {
    if (!number) {
      return;
    }
    for (const auto& [recorded, copy] : objects_) {
      if (recorded == number) {
        return;
      }
    }
    RetainPtr<CPDF_Object> object = doc_->GetOrParseIndirectObject(number);
    if (object && object->IsDictionary()) {
      objects_.emplace_back(number, object->Clone());
    }
  }

  // `live` gets `copy`'s entries, and only those.
  static void Replace(CPDF_Dictionary* live, const CPDF_Dictionary* copy) {
    std::vector<ByteString> keys;
    {
      CPDF_DictionaryLocker locker(live);
      for (const auto& entry : locker) {
        keys.push_back(entry.first);
      }
    }
    for (const ByteString& key : keys) {
      live->RemoveFor(key.AsStringView());
    }
    CPDF_DictionaryLocker locker(copy);
    for (const auto& entry : locker) {
      live->SetFor(entry.first, entry.second->Clone());
    }
  }

  UnownedPtr<CPDF_Document> const doc_;
  const uint32_t mark_;
  std::vector<Page> pages_;
  std::vector<std::pair<uint32_t, RetainPtr<CPDF_Object>>> objects_;
};

EpdfCheckpoint* CheckpointFromHandle(EPDF_CHECKPOINT checkpoint) {
  return reinterpret_cast<EpdfCheckpoint*>(checkpoint);
}

}  // namespace

FPDF_EXPORT EPDF_CHECKPOINT FPDF_CALLCONV
EPDFDoc_BeginCheckpoint(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return nullptr;
  }
  return reinterpret_cast<EPDF_CHECKPOINT>(new EpdfCheckpoint(doc));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_CheckpointPage(EPDF_CHECKPOINT checkpoint, int page_index) {
  EpdfCheckpoint* recorded = CheckpointFromHandle(checkpoint);
  return recorded && page_index >= 0 && recorded->RecordPage(page_index);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_CheckpointObject(EPDF_CHECKPOINT checkpoint,
                         unsigned int object_number) {
  EpdfCheckpoint* recorded = CheckpointFromHandle(checkpoint);
  return recorded && object_number > 0 && recorded->RecordObject(object_number);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_Rollback(EPDF_CHECKPOINT checkpoint) {
  EpdfCheckpoint* recorded = CheckpointFromHandle(checkpoint);
  return recorded && recorded->Rollback();
}

FPDF_EXPORT void FPDF_CALLCONV
EPDFDoc_EndCheckpoint(EPDF_CHECKPOINT checkpoint) {
  delete CheckpointFromHandle(checkpoint);
}
