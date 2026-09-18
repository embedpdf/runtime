// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PARSER_CPDF_LAYER_DOCUMENT_H_
#define CORE_FPDFAPI_PARSER_CPDF_LAYER_DOCUMENT_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <vector>

#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/retain_ptr.h"

class CPDF_BaseDocument;

class CPDF_LayerDocument final : public CPDF_Document {
 public:
  enum class OpenStatus {
    kSuccess,
    kMalformedDelta,
    kBaseLayerMismatch,
    kOpenFailed,
  };

  // |file_access| is the raw delta to ingest (null or empty for a fresh
  // layer). A successfully ingested delta is retained for the life of the
  // layer as its loaded bytes (see GetLoadedDeltaStream()), so the stream
  // must stay readable and unchanged for that long: pass an owned copy, not
  // a view of caller memory.
  CPDF_LayerDocument(RetainPtr<CPDF_BaseDocument> base,
                     RetainPtr<IFX_SeekableReadStream> file_access);
  ~CPDF_LayerDocument() override;

  static CPDF_LayerDocument* FromDocument(CPDF_Document* document);
  static const CPDF_LayerDocument* FromDocument(const CPDF_Document* document);

  OpenStatus ingest_status() const { return ingest_status_; }
  size_t GetPromotedObjectCount() const;
  bool HasPromotedObjects() const { return begin() != end(); }
  CPDF_BaseDocument* GetBaseDocument() const { return base_.Get(); }

  // The delta this layer ingested at open time, or null when it was opened
  // fresh. Together with the base file these are the bytes the layer was
  // loaded from - the only bytes revision analysis may read, because the
  // parser this document reports is the base parser and the promoted
  // objects are in-memory clones. Unsaved edits are not part of it.
  RetainPtr<IFX_SeekableReadStream> GetLoadedDeltaStream() const {
    return loaded_delta_;
  }

  // The creator compares effective values using separate generation contexts:
  // the frozen base determines the cumulative delta, while the loaded delta
  // twin (falling back to that base) determines changed-since-load. Saving does
  // not advance either baseline. GetBaseTwin() is inherited from CPDF_Document.
  // Cache-only access to the pristine version carried by the loaded delta.
  // A missing entry means the loaded version is the base version.
  RetainPtr<const CPDF_Object> FindLoadedDeltaTwin(uint32_t objnum) const;

  // CPDF_Document:
  CPDF_Parser* GetParser() const override;
  const CPDF_Dictionary* GetRoot() const override;
  RetainPtr<CPDF_Dictionary> GetMutableRoot() override;
  RetainPtr<CPDF_Dictionary> GetMutableInfo() override;
  RetainPtr<const CPDF_Dictionary> GetPageDictionary(int iPage) override;
  RetainPtr<CPDF_Dictionary> GetMutablePageDictionary(int iPage) override;
  uint32_t GetUserPermissions(bool get_owner_perms) const override;
  RetainPtr<CPDF_Object> FindPromotedObject(uint32_t objnum) const override;
  RetainPtr<const CPDF_Object> GetLoadedTwin(uint32_t objnum) const override;
  RetainPtr<const CPDF_Object> GetBaseTwin(uint32_t objnum) const override;
  bool SharesBackingStorageWith(const CPDF_Stream* stream) const override;
  uint64_t GetOverlayEpoch() const override;
  bool IsLayerDocument() const override;
  FX_FILESIZE GetLayerAppendBaseOffset() const override;
  bool ShouldReplaceDeletedPageWithNull(uint32_t page_obj_num) const override;

  // CPDF_Parser::ParsedObjectsHolder:
  RetainPtr<CPDF_Object> ParseIndirectObject(uint32_t objnum) override;
  RetainPtr<CPDF_Object> GetMutableIndirectObject(uint32_t objnum) override;
  void DeleteIndirectObject(uint32_t objnum) override;

 protected:
  // CPDF_IndirectObjectHolder:
  const CPDF_Object* GetIndirectObjectInternal(uint32_t objnum) const override;
  CPDF_Object* GetOrParseIndirectObjectInternal(uint32_t objnum) override;

  // CPDF_Document page-list storage:
  uint32_t GetPageObjNumAt(size_t index) const override;
  void SetPageObjNumAt(size_t index, uint32_t objnum) override;
  void InsertPageObjNum(size_t index, uint32_t objnum) override;
  void ErasePageObjNum(size_t index) override;
  void ResizePageList(size_t size) override;
  size_t GetPageListSize() const override;

 private:
  void InitializeFromBase();
  void IngestCurrentDelta();
  void FailDeltaIngest(OpenStatus status);
  RetainPtr<CPDF_Object> PromoteFromBase(uint32_t objnum);

  RetainPtr<CPDF_BaseDocument> const base_;
  RetainPtr<IFX_SeekableReadStream> file_access_;
  RetainPtr<IFX_SeekableReadStream> loaded_delta_;
  // The reader the delta was ingested through (base bytes followed by the
  // delta). Every file-backed stream the delta carried is a view into it;
  // retaining it here is what lets those views be shared, never copied.
  RetainPtr<IFX_SeekableReadStream> ingest_reader_;
  std::vector<uint32_t> layer_page_list_;
  // Pristine clones of what the loaded delta carried, keyed by object number,
  // made at ingest next to the overlay clone and never mutated: the loaded
  // twins. O(delta) memory; a delta is small by construction.
  std::map<uint32_t, RetainPtr<const CPDF_Object>> loaded_twins_;
  // Generation for caches that retain effective-object pointers.
  uint64_t overlay_epoch_ = 0;
  OpenStatus ingest_status_ = OpenStatus::kSuccess;
};

#endif  // CORE_FPDFAPI_PARSER_CPDF_LAYER_DOCUMENT_H_
