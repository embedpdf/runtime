// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PARSER_CPDF_LAYER_DOCUMENT_H_
#define CORE_FPDFAPI_PARSER_CPDF_LAYER_DOCUMENT_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_layer_transaction.h"
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

  // Transactions. Between Begin and Commit or Abort, every write lands in an
  // overlay above the committed objects; reads look there first. Commit makes
  // the overlay part of the layer; Abort drops it, and the layer is exactly as
  // at Begin (object numbers handed out meanwhile are not given back). One at
  // a time, never nested. Each returns false when it does not apply.
  bool BeginTransaction();
  bool CommitTransaction();
  bool AbortTransaction();
  bool InTransaction() const { return !!transaction_; }
  // What the last ended (or the open) transaction cost.
  const CPDF_LayerTransactionStats& GetTransactionStats() const {
    return transaction_ ? transaction_->stats : last_transaction_stats_;
  }

  // The birth list. An inline annotation in the base has no object number;
  // its permanent name is its position in the base page's /Annots, its birth
  // index (platform docs/plans/2026-10-04-optimistic-writes-forms-history.md
  // §2.6). While a page keeps every inline annotation at its birth index, the
  // name resolves by position. Promoting the page moves each one into its own
  // object, in place, and records where each went: the page's births,
  // written once and never changed.
  //
  // Promotes page |page_index|: every inline annotation becomes an object
  // with the same content at the same position. The first promotion of a
  // page whose base version has inline annotations records its births (in
  // the open transaction, if any). Returns how many moved, or -1.
  int PromotePageAnnots(int page_index);
  // Whether |page_objnum| has births recorded: promoted, by this layer.
  bool IsPagePromoted(uint32_t page_objnum) const;
  // The object the annotation born at |birth_index| on |page_objnum| became,
  // or 0 when the page isn't promoted or had no inline annotation there.
  uint32_t FindBirth(uint32_t page_objnum, uint32_t birth_index) const;
  // The birth name {page object number, birth index} of |objnum|, when it is
  // an inline annotation this layer promoted.
  std::optional<std::pair<uint32_t, uint32_t>> FindBirthName(
      uint32_t objnum) const;
  // The committed births, for the layer artifact (saving is refused inside
  // a transaction).
  const std::map<uint32_t, CPDF_PageBirths>& GetCommittedBirths() const {
    return births_;
  }
  // What a layer artifact carries besides its delta, restored once the delta
  // is ingested: the last object number the layer had handed out (0 when the
  // artifact predates it), so none is handed out again, and the births.
  // False when they don't fit the delta: a malformed artifact.
  bool RestoreArtifactState(std::map<uint32_t, CPDF_PageBirths> births,
                            uint32_t last_object_number);

  // The layer's own version of |objnum|: written in the open transaction,
  // else committed (unless the transaction hid it), else null - and then the
  // base answers. Every lookup of "this layer's version" goes through here.
  RetainPtr<CPDF_Object> FindLayerVersion(uint32_t objnum) const;
  // Whether |object| is a version of |objnum| in this layer: the effective
  // one, the committed one an open transaction copied, or the base's. A
  // handle read before a transaction holds one of these; another document's
  // object never is one.
  bool IsVersionOf(uint32_t objnum, const CPDF_Object* object) const;
  size_t GetPromotedObjectCount() const;
  // Whether FindLayerVersion() can answer anything: an open transaction's
  // copies count, or a base parse under this layer's view would miss them.
  bool HasPromotedObjects() const {
    return begin() != end() || (transaction_ && !transaction_->written.empty());
  }
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

  // CPDF_IndirectObjectHolder:
  uint32_t AddIndirectObject(RetainPtr<CPDF_Object> object) override;
  bool ReplaceIndirectObjectIfHigherGeneration(
      uint32_t objnum,
      RetainPtr<CPDF_Object> object) override;

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
  // The page list, saved first when an open transaction changes it.
  std::vector<uint32_t>& MutablePageList();
  // Root and Info are cached dictionaries: forget them when |objnum| is one.
  void InvalidateCachedDictsFor(uint32_t objnum);
  // Derived caches (fonts, colour spaces, patterns, ICC, transfer
  // functions) forget |versions| and their direct objects: versions nobody
  // can reach any more (fork plan L4). Image objects follow their stream's
  // version themselves (CPDF_Image).
  void ForgetDerivedDataOf(
      const std::vector<RetainPtr<const CPDF_Object>>& versions);
  // The positions in the base page's /Annots that hold inline annotations,
  // or nullopt when the base has no page |page_objnum|.
  std::optional<std::vector<uint32_t>> GetBaseInlineAnnotPositions(
      uint32_t page_objnum) const;
  void AddCommittedBirths(uint32_t page_objnum, CPDF_PageBirths births);

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
  std::unique_ptr<CPDF_LayerTransaction> transaction_;
  CPDF_LayerTransactionStats last_transaction_stats_;
  // The committed birth list, by page object number, and its reverse: each
  // promoted object's birth name.
  std::map<uint32_t, CPDF_PageBirths> births_;
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> birth_names_;
  OpenStatus ingest_status_ = OpenStatus::kSuccess;
};

#endif  // CORE_FPDFAPI_PARSER_CPDF_LAYER_DOCUMENT_H_
