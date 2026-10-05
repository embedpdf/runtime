// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/parser/cpdf_layer_document.h"

#include <algorithm>
#include <utility>

#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_base_document.h"
#include "core/fpdfapi/parser/cpdf_concat_read_stream.h"
#include "core/fpdfapi/parser/cpdf_cross_ref_table.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_parse_only_holder.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_read_only_graph_guard.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_write_generation.h"
#include "core/fpdfapi/render/cpdf_docrenderdata.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/containers/contains.h"
#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/notreached.h"

namespace {

bool IsBaseObjectLive(const CPDF_Parser* base_parser, uint32_t objnum) {
  return objnum != 0 && base_parser->IsValidObjectNumber(objnum) &&
         !base_parser->IsObjectFree(objnum);
}

bool IsObjectOwnedByAppendedDelta(const CPDF_CrossRefTable* table,
                                  uint32_t objnum,
                                  const CPDF_CrossRefTable::ObjectInfo& info,
                                  FX_FILESIZE layer_append_base_offset) {
  if (objnum == table->trailer_object_number()) {
    return false;
  }

  switch (info.type) {
    case CPDF_CrossRefTable::ObjectType::kFree:
      return false;
    case CPDF_CrossRefTable::ObjectType::kNormal:
      return info.pos >= layer_append_base_offset;
    case CPDF_CrossRefTable::ObjectType::kCompressed: {
      const CPDF_CrossRefTable::ObjectInfo* archive_info =
          table->GetObjectInfo(info.archive.obj_num);
      return archive_info &&
             archive_info->type == CPDF_CrossRefTable::ObjectType::kNormal &&
             archive_info->pos >= layer_append_base_offset;
    }
  }
}

}  // namespace

CPDF_LayerDocument::CPDF_LayerDocument(
    RetainPtr<CPDF_BaseDocument> base,
    RetainPtr<IFX_SeekableReadStream> file_access)
    : CPDF_Document(std::make_unique<CPDF_DocRenderData>(
                        CPDF_DocRenderData::FromDocument(base.Get())),
                    std::make_unique<CPDF_DocPageData>(
                        CPDF_DocPageData::FromDocument(base.Get()))),
      base_(std::move(base)),
      file_access_(std::move(file_access)) {
  CHECK(base_);
  SetLastObjNum(base_->GetLastObjNum());
  {
    // Initialization deliberately copies the immutable base view.
    CPDF_DocumentViewScope frozen_view(base_.Get());
    InitializeFromBase();
  }
#if DCHECK_IS_ON()
  base_->RegisterLiveLayer(this);
#endif
  {
    CPDF_DocumentViewScope effective_view(this);
    IngestCurrentDelta();
  }
}

CPDF_LayerDocument::~CPDF_LayerDocument() {
  // Closing inside a transaction drops it, and closes its generation on this
  // thread with it.
  AbortTransaction();
#if DCHECK_IS_ON()
  base_->UnregisterLiveLayer(this);
#endif
}

// static
CPDF_LayerDocument* CPDF_LayerDocument::FromDocument(CPDF_Document* document) {
  return document && document->IsLayerDocument()
             ? static_cast<CPDF_LayerDocument*>(document)
             : nullptr;
}

// static
const CPDF_LayerDocument* CPDF_LayerDocument::FromDocument(
    const CPDF_Document* document) {
  return document && document->IsLayerDocument()
             ? static_cast<const CPDF_LayerDocument*>(document)
             : nullptr;
}

size_t CPDF_LayerDocument::GetPromotedObjectCount() const {
  return static_cast<size_t>(std::distance(begin(), end()));
}

CPDF_Parser* CPDF_LayerDocument::GetParser() const {
  return base_->GetParser();
}

const CPDF_Dictionary* CPDF_LayerDocument::GetRoot() const {
  const uint32_t root_objnum = base_->GetParser()->GetRootObjNum();
  if (RetainPtr<CPDF_Object> local = FindLayerVersion(root_objnum)) {
    // The returned pointer is owned by this layer's indirect object holder.
    // Const root reads must see the effective overlay after delta ingest even
    // when the mutable root cache has been invalidated.
    return local->AsDictionary();
  }
  return base_->GetRoot();
}

RetainPtr<CPDF_Dictionary> CPDF_LayerDocument::GetMutableRoot() {
  const uint32_t root_objnum = base_->GetParser()->GetRootObjNum();
  RetainPtr<CPDF_Object> live = GetMutableIndirectObject(root_objnum);
  RetainPtr<CPDF_Dictionary> root =
      live ? pdfium::WrapRetain(live->AsMutableDictionary()) : nullptr;
  SetCachedRootDict(root);
  return root;
}

RetainPtr<CPDF_Dictionary> CPDF_LayerDocument::GetMutableInfo() {
  RetainPtr<CPDF_Dictionary> current_info = GetInfo();
  if (!current_info) {
    return nullptr;
  }
  const uint32_t info_objnum = current_info->GetObjNum();
  DCHECK_NE(CPDF_Object::kInvalidObjNum, info_objnum);
  DCHECK_NE(0u, info_objnum);
  RetainPtr<CPDF_Object> live = GetMutableIndirectObject(info_objnum);
  RetainPtr<CPDF_Dictionary> info =
      live ? pdfium::WrapRetain(live->AsMutableDictionary()) : nullptr;
  SetCachedInfoDict(info);
  return info;
}

RetainPtr<const CPDF_Dictionary> CPDF_LayerDocument::GetPageDictionary(
    int iPage) {
  if (iPage < 0 || static_cast<size_t>(iPage) >= GetPageListSize()) {
    return nullptr;
  }

  const uint32_t objnum = GetPageObjNumAt(iPage);
  if (!objnum) {
    return nullptr;
  }

  return ToDictionary(GetOrParseIndirectObject(objnum));
}

RetainPtr<CPDF_Dictionary> CPDF_LayerDocument::GetMutablePageDictionary(
    int iPage) {
  if (iPage < 0 || static_cast<size_t>(iPage) >= GetPageListSize()) {
    return nullptr;
  }

  const uint32_t objnum = GetPageObjNumAt(iPage);
  if (!objnum) {
    return nullptr;
  }

  // EmbedPDF layer documents must never return a const-cast frozen base page
  // for mutation. Page moves reparent the moved page dictionary, so promote the
  // page object before returning a mutable handle.
  RetainPtr<CPDF_Object> live = GetMutableIndirectObject(objnum);
  return live ? pdfium::WrapRetain(live->AsMutableDictionary()) : nullptr;
}

uint32_t CPDF_LayerDocument::GetUserPermissions(bool get_owner_perms) const {
  return base_->GetUserPermissions(get_owner_perms);
}

// "Promoted" means "this layer has its own version", in an open transaction
// too: cache routing (CanUseFallbackForObject) and the image rebind follow it.
RetainPtr<CPDF_Object> CPDF_LayerDocument::FindPromotedObject(
    uint32_t objnum) const {
  return FindLayerVersion(objnum);
}

RetainPtr<CPDF_Object> CPDF_LayerDocument::FindLayerVersion(
    uint32_t objnum) const {
  if (transaction_) {
    auto it = transaction_->written.find(objnum);
    if (it != transaction_->written.end()) {
      return it->second;
    }
    if (transaction_->hidden.contains(objnum)) {
      return nullptr;  // the committed version is hidden; the base may answer
    }
  }
  return FindLocalIndirectObject(objnum);
}

bool CPDF_LayerDocument::IsVersionOf(uint32_t objnum,
                                     const CPDF_Object* object) const {
  if (!object || !objnum) {
    return false;
  }
  return FindLayerVersion(objnum).Get() == object ||
         FindLocalIndirectObject(objnum).Get() == object ||
         GetBaseTwin(objnum).Get() == object;
}

// The base's frozen object, only for an object the base's cross-reference
// table owns. An object created in a layer has no base twin, and asking the
// base to parse it would be a base read of a promoted number outside any
// view - exactly what the ambient-view detector forbids.
RetainPtr<const CPDF_Object> CPDF_LayerDocument::GetBaseTwin(
    uint32_t objnum) const {
  if (!IsBaseObjectLive(base_->GetParser(), objnum)) {
    return nullptr;
  }
  return base_->GetFrozenObjectForLayer(objnum);
}

RetainPtr<const CPDF_Object> CPDF_LayerDocument::GetLoadedTwin(
    uint32_t objnum) const {
  if (auto twin = FindLoadedDeltaTwin(objnum)) {
    return twin;
  }
  return GetBaseTwin(objnum);
}

RetainPtr<const CPDF_Object> CPDF_LayerDocument::FindLoadedDeltaTwin(
    uint32_t objnum) const {
  auto it = loaded_twins_.find(objnum);
  return it != loaded_twins_.end() ? it->second : nullptr;
}

bool CPDF_LayerDocument::SharesBackingStorageWith(
    const CPDF_Stream* stream) const {
  RetainPtr<IFX_SeekableReadStream> view = stream ? stream->BackingView() : nullptr;
  if (!view) {
    return false;
  }
  IFX_SeekableReadStream* underlying = view->GetUnderlyingStream();
  if (ingest_reader_ && underlying == ingest_reader_->GetUnderlyingStream()) {
    return true;  // parsed from the delta this layer retains
  }
  if (loaded_delta_ && underlying == loaded_delta_->GetUnderlyingStream()) {
    return true;
  }
  return CPDF_Document::SharesBackingStorageWith(stream);  // the base parser's file
}

uint64_t CPDF_LayerDocument::GetOverlayEpoch() const {
  return overlay_epoch_;
}

bool CPDF_LayerDocument::IsLayerDocument() const {
  return true;
}

FX_FILESIZE CPDF_LayerDocument::GetLayerAppendBaseOffset() const {
  return base_->GetLayerAppendBaseOffset();
}

bool CPDF_LayerDocument::ShouldReplaceDeletedPageWithNull(
    uint32_t page_obj_num) const {
  CPDF_Parser* parser = base_->GetParser();
  if (parser && parser->IsValidObjectNumber(page_obj_num) &&
      !parser->IsObjectFree(page_obj_num)) {
    return false;
  }

  // If the page object was created in this layer, nulling it is a local overlay
  // change. If it exists in the base document, deletion is represented solely
  // by the promoted page tree no longer referencing it.
  return FindPromotedObject(page_obj_num) != nullptr;
}

RetainPtr<CPDF_Object> CPDF_LayerDocument::ParseIndirectObject(
    uint32_t objnum) {
  NOTREACHED();
  return nullptr;
}

RetainPtr<CPDF_Object> CPDF_LayerDocument::GetMutableIndirectObject(
    uint32_t objnum) {
  if (!transaction_) {
    if (RetainPtr<CPDF_Object> local = FindLocalIndirectObject(objnum)) {
      return local;
    }
    return PromoteFromBase(objnum);
  }

  // Inside a transaction everything below it is read-only: the first write to
  // an object copies the version a read sees (the committed layer's, or the
  // frozen base's) up into the transaction, and every later write reuses it.
  CPDF_LayerTransaction& tx = *transaction_;
  auto it = tx.written.find(objnum);
  if (it != tx.written.end()) {
    return it->second;
  }
  if (!objnum || objnum == CPDF_Object::kInvalidObjNum) {
    return nullptr;
  }
  RetainPtr<const CPDF_Object> current = FindLayerVersion(objnum);
  if (!current) {
    current = base_->GetFrozenObjectForLayer(objnum);
  }
  if (!current) {
    return nullptr;
  }
  RetainPtr<CPDF_Object> copy = current->CloneForHolder(this);
  if (!copy) {
    return nullptr;
  }
  copy->SetObjNum(objnum);
  copy->SetGenNum(current->GetGenNum());
  copy->StampWriteGeneration(tx.generation);
  ++tx.stats.objects_copied;
  if (const CPDF_Stream* stream = current->AsStream();
      stream && stream->IsMemoryBased()) {
    tx.stats.stream_bytes_copied += stream->GetRawSize();
  }
  tx.written[objnum] = copy;
  tx.hidden.erase(objnum);
  tx.touched.insert(objnum);
  ++overlay_epoch_;
  InvalidateCachedDictsFor(objnum);
  return copy;
}

void CPDF_LayerDocument::DeleteIndirectObject(uint32_t objnum) {
  // Every delete moves the epoch, even one that leaves a base object
  // resolving: its caller just stopped referencing it, and handles that
  // check membership (CPDF_AnnotContext) must look again.
  ++overlay_epoch_;
  if (!transaction_) {
    if (FindLocalIndirectObject(objnum)) {
      CPDF_Document::DeleteIndirectObject(objnum);
    }
    return;
  }
  // A delete removes the layer's own versions. An object that exists in the
  // base keeps resolving to its base version - a layer can only stop
  // referencing it - exactly as outside a transaction.
  CPDF_LayerTransaction& tx = *transaction_;
  auto written = tx.written.find(objnum);
  const bool had_copy = written != tx.written.end();
  if (had_copy) {
    tx.dropped.push_back(std::move(written->second));
    tx.written.erase(written);
  }
  const bool committed = !!FindLocalIndirectObject(objnum);
  if (committed) {
    tx.hidden.insert(objnum);
  }
  if (had_copy || committed) {
    tx.touched.insert(objnum);
    InvalidateCachedDictsFor(objnum);
  }
}

uint32_t CPDF_LayerDocument::AddIndirectObject(RetainPtr<CPDF_Object> object) {
  if (!transaction_) {
    object->StampWriteGeneration(CPDF_WriteGeneration::kCommitted);
    return CPDF_Document::AddIndirectObject(std::move(object));
  }
  DCHECK_PDF_HOLDER_MUTABLE();
  CHECK(!object->GetObjNum());
  object->StampWriteGeneration(transaction_->generation);
  const uint32_t objnum = GetLastObjNum() + 1;
  SetLastObjNum(objnum);
  object->SetObjNum(objnum);
  ++transaction_->stats.objects_added;
  transaction_->written[objnum] = std::move(object);
  transaction_->touched.insert(objnum);
  ++overlay_epoch_;
  return objnum;
}

bool CPDF_LayerDocument::ReplaceIndirectObjectIfHigherGeneration(
    uint32_t objnum,
    RetainPtr<CPDF_Object> object) {
  if (!transaction_) {
    if (object) {
      object->StampWriteGeneration(CPDF_WriteGeneration::kCommitted);
    }
    return CPDF_Document::ReplaceIndirectObjectIfHigherGeneration(
        objnum, std::move(object));
  }
  DCHECK(objnum);
  if (!object || objnum == CPDF_Object::kInvalidObjNum) {
    return false;
  }
  RetainPtr<const CPDF_Object> current = FindLayerVersion(objnum);
  if (!current && !transaction_->hidden.contains(objnum)) {
    current = base_->GetFrozenObjectForLayer(objnum);
  }
  if (current && object->GetGenNum() <= current->GetGenNum()) {
    return false;
  }
  DCHECK_PDF_HOLDER_MUTABLE();
  object->SetObjNum(objnum);
  object->StampWriteGeneration(transaction_->generation);
  transaction_->written[objnum] = std::move(object);
  transaction_->hidden.erase(objnum);
  transaction_->touched.insert(objnum);
  if (objnum > GetLastObjNum()) {
    SetLastObjNum(objnum);
  }
  ++overlay_epoch_;
  InvalidateCachedDictsFor(objnum);
  return true;
}

bool CPDF_LayerDocument::BeginTransaction() {
  // One transaction per thread: the open generation is per thread, so a
  // second layer can't open one while another layer's is open.
  if (transaction_ || ingest_status_ != OpenStatus::kSuccess ||
      open_checkpoints_ > 0 || CPDF_WriteGeneration::Current() != 0) {
    return false;
  }
  transaction_ = std::make_unique<CPDF_LayerTransaction>();
  transaction_->object_mark = GetLastObjNum();
  transaction_->generation = CPDF_WriteGeneration::Next();
  CPDF_WriteGeneration::SetCurrent(transaction_->generation);
  transaction_->root_before = GetCachedRootDict();
  transaction_->info_before = GetCachedInfoDict();
  return true;
}

bool CPDF_LayerDocument::CommitTransaction() {
  if (!transaction_) {
    return false;
  }
  std::unique_ptr<CPDF_LayerTransaction> tx = std::move(transaction_);
  CPDF_WriteGeneration::SetCurrent(0);
  // What nobody can reach after this commit, so derived caches must not keep
  // it alive: the committed versions it replaces or deletes, and the objects
  // it made and deleted again itself.
  std::vector<RetainPtr<const CPDF_Object>> unreachable;
  for (RetainPtr<CPDF_Object>& object : tx->dropped) {
    unreachable.push_back(std::move(object));
  }
  for (auto& [objnum, object] : tx->written) {
    if (RetainPtr<CPDF_Object> old = FindLocalIndirectObject(objnum)) {
      unreachable.push_back(std::move(old));
    }
    // Copies and new objects were stamped when stored, and attaching stamps
    // detached children; this only catches a child stored without a setter.
    object->StampWriteGeneration(tx->generation);
    AddPromotedObject(objnum, std::move(object));  // replaces the old version
  }
  for (uint32_t objnum : tx->hidden) {
    if (RetainPtr<CPDF_Object> old = FindLocalIndirectObject(objnum)) {
      unreachable.push_back(std::move(old));
    }
    CPDF_Document::DeleteIndirectObject(objnum);
  }
  ForgetDerivedDataOf(unreachable);
  for (auto& [page_objnum, births] : tx->births) {
    AddCommittedBirths(page_objnum, std::move(births));
  }
  last_transaction_stats_ = tx->stats;
  ++overlay_epoch_;
  return true;
}

bool CPDF_LayerDocument::AbortTransaction() {
  if (!transaction_) {
    return false;
  }
  std::unique_ptr<CPDF_LayerTransaction> tx = std::move(transaction_);
  CPDF_WriteGeneration::SetCurrent(0);
  if (tx->page_list_before) {
    layer_page_list_ = std::move(*tx->page_list_before);
  }
  // The caches point at what they did at begin: committed or base versions,
  // which the transaction never wrote.
  SetCachedRootDict(std::move(tx->root_before));
  SetCachedInfoDict(std::move(tx->info_before));
  // Object numbers are not given back: anything that remembers a number from
  // this transaction must never meet a later object under it.
  std::vector<RetainPtr<const CPDF_Object>> unreachable;
  for (auto& [objnum, object] : tx->written) {
    unreachable.push_back(std::move(object));
  }
  for (RetainPtr<CPDF_Object>& object : tx->dropped) {
    unreachable.push_back(std::move(object));
  }
  ForgetDerivedDataOf(unreachable);
  last_transaction_stats_ = tx->stats;
  ++overlay_epoch_;
  // tx->written and tx->hidden are dropped with tx. Nothing is undone.
  return true;
}

int CPDF_LayerDocument::PromotePageAnnots(int page_index) {
  if (page_index < 0 || static_cast<size_t>(page_index) >= GetPageListSize()) {
    return -1;
  }
  const uint32_t page_objnum = GetPageObjNumAt(page_index);
  RetainPtr<const CPDF_Dictionary> page = GetPageDictionary(page_index);
  if (!page_objnum || !page) {
    return -1;
  }

  // Read first: a page with nothing inline is promoted without a write.
  RetainPtr<const CPDF_Array> annots = page->GetArrayFor("Annots");
  bool has_inline = false;
  for (size_t i = 0; annots && i < annots->size() && !has_inline; ++i) {
    RetainPtr<const CPDF_Object> entry = annots->GetObjectAt(i);
    has_inline = entry && entry->IsInline() && entry->IsDictionary();
  }

  std::map<uint32_t, uint32_t> moved;  // position -> its new object
  if (has_inline) {
    // Open the owner of the entries: an indirect /Annots array, or the page.
    RetainPtr<const CPDF_Reference> ref =
        ToReference(page->GetObjectFor("Annots"));
    RetainPtr<CPDF_Array> mutable_annots;
    if (ref) {
      mutable_annots = ToArray(GetMutableIndirectObject(ref->GetRefObjNum()));
    } else if (RetainPtr<CPDF_Dictionary> mutable_page =
                   GetMutablePageDictionary(page_index)) {
      mutable_annots = mutable_page->GetMutableArrayFor("Annots");
    }
    if (!mutable_annots) {
      return -1;
    }
    for (size_t i = 0; i < mutable_annots->size(); ++i) {
      RetainPtr<const CPDF_Object> entry = mutable_annots->GetObjectAt(i);
      if (!entry || !entry->IsInline() || !entry->IsDictionary()) {
        continue;
      }
      mutable_annots->ConvertToIndirectObjectAt(i, this);
      RetainPtr<const CPDF_Reference> now =
          ToReference(mutable_annots->GetObjectAt(i));
      CHECK(now);
      moved[static_cast<uint32_t>(i)] = now->GetRefObjNum();
    }
  }

  // Births, once per page, for a page whose base version has inline
  // annotations: where each one went. The page has kept every one at its
  // birth index until now - creates append and updates edit in place - so
  // the position each moved from is its birth index. (A page with none has
  // no birth names to record, and promoting it changes nothing.)
  if (!IsPagePromoted(page_objnum)) {
    std::optional<std::vector<uint32_t>> base_inline =
        GetBaseInlineAnnotPositions(page_objnum);
    if (base_inline && !base_inline->empty()) {
      CPDF_PageBirths births;
      for (uint32_t position : *base_inline) {
        auto it = moved.find(position);
        if (it != moved.end()) {
          births[position] = it->second;
        }
      }
      if (transaction_) {
        transaction_->births[page_objnum] = std::move(births);
      } else {
        AddCommittedBirths(page_objnum, std::move(births));
      }
      ++overlay_epoch_;
    }
  }
  return static_cast<int>(moved.size());
}

bool CPDF_LayerDocument::IsPagePromoted(uint32_t page_objnum) const {
  return (transaction_ && transaction_->births.contains(page_objnum)) ||
         births_.contains(page_objnum);
}

uint32_t CPDF_LayerDocument::FindBirth(uint32_t page_objnum,
                                       uint32_t birth_index) const {
  const CPDF_PageBirths* births = nullptr;
  if (transaction_) {
    auto it = transaction_->births.find(page_objnum);
    if (it != transaction_->births.end()) {
      births = &it->second;
    }
  }
  if (!births) {
    auto it = births_.find(page_objnum);
    if (it == births_.end()) {
      return 0;
    }
    births = &it->second;
  }
  auto it = births->find(birth_index);
  return it != births->end() ? it->second : 0;
}

std::optional<std::pair<uint32_t, uint32_t>> CPDF_LayerDocument::FindBirthName(
    uint32_t objnum) const {
  if (transaction_) {
    for (const auto& [page_objnum, births] : transaction_->births) {
      for (const auto& [birth_index, born] : births) {
        if (born == objnum) {
          return std::make_pair(page_objnum, birth_index);
        }
      }
    }
  }
  auto it = birth_names_.find(objnum);
  if (it == birth_names_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool CPDF_LayerDocument::RestoreArtifactState(
    std::map<uint32_t, CPDF_PageBirths> births,
    uint32_t last_object_number) {
  if (transaction_ || !births_.empty()) {
    return false;
  }
  // Numbers first: the delta carries only the objects that still exist, so
  // its highest is no high-water mark. The artifact's is, and can't be lower.
  if (last_object_number != 0) {
    if (last_object_number < GetLastObjNum()) {
      return false;
    }
    SetLastObjNum(last_object_number);
  }
  // A birth outlives its object: an annotation promoted and later deleted
  // keeps its birth, so its birth name says "deleted" instead of falling back
  // to whatever sits at its old position. So the object may be gone; what
  // every real birth still satisfies is checked instead. Its page is a base
  // page with an inline annotation at the birth index, and its object number
  // is one this layer handed out - above the base's, up to the last - and a
  // dictionary, if it still resolves.
  const uint32_t base_last = base_->GetLastObjNum();
  for (const auto& [page_objnum, page_births] : births) {
    std::optional<std::vector<uint32_t>> inline_positions =
        page_objnum ? GetBaseInlineAnnotPositions(page_objnum) : std::nullopt;
    if (!inline_positions) {
      return false;
    }
    for (const auto& [birth_index, objnum] : page_births) {
      if (objnum <= base_last || objnum > GetLastObjNum() ||
          !pdfium::Contains(*inline_positions, birth_index)) {
        return false;
      }
      RetainPtr<const CPDF_Object> born = FindLocalIndirectObject(objnum);
      if (born && !born->IsDictionary()) {
        return false;
      }
    }
  }
  if (births.empty()) {
    return true;
  }
  for (auto& [page_objnum, page_births] : births) {
    AddCommittedBirths(page_objnum, std::move(page_births));
  }
  ++overlay_epoch_;
  return true;
}

void CPDF_LayerDocument::AddCommittedBirths(uint32_t page_objnum,
                                            CPDF_PageBirths births) {
  DCHECK(!births_.contains(page_objnum));  // written once per page
  for (const auto& [birth_index, objnum] : births) {
    birth_names_[objnum] = {page_objnum, birth_index};
  }
  births_[page_objnum] = std::move(births);
}

std::optional<std::vector<uint32_t>>
CPDF_LayerDocument::GetBaseInlineAnnotPositions(uint32_t page_objnum) const {
  // Read the base's own page, through the base's frozen view: references in
  // it resolve to base objects, never to this layer's versions.
  CPDF_DocumentViewScope frozen_view(base_.Get());
  RetainPtr<const CPDF_Dictionary> page =
      ToDictionary(GetBaseTwin(page_objnum));
  if (!page) {
    return std::nullopt;
  }
  std::vector<uint32_t> positions;
  RetainPtr<const CPDF_Array> annots = page->GetArrayFor("Annots");
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    RetainPtr<const CPDF_Object> entry = annots->GetObjectAt(i);
    if (entry && entry->IsInline() && entry->IsDictionary()) {
      positions.push_back(static_cast<uint32_t>(i));
    }
  }
  return positions;
}

std::vector<uint32_t>& CPDF_LayerDocument::MutablePageList() {
  if (transaction_ && !transaction_->page_list_before) {
    transaction_->page_list_before = layer_page_list_;
  }
  return layer_page_list_;
}

void CPDF_LayerDocument::ForgetDerivedDataOf(
    const std::vector<RetainPtr<const CPDF_Object>>& versions) {
  CPDF_DocPageData* page_data = CPDF_DocPageData::FromDocument(this);
  CPDF_DocRenderData* render_data = CPDF_DocRenderData::FromDocument(this);
  for (const RetainPtr<const CPDF_Object>& version : versions) {
    if (page_data) {
      page_data->ForgetObjectTree(version.Get());
    }
    if (render_data) {
      render_data->ForgetObjectTree(version.Get());
    }
  }
}

void CPDF_LayerDocument::InvalidateCachedDictsFor(uint32_t objnum) {
  CPDF_Parser* parser = base_->GetParser();
  if (!parser) {
    return;
  }
  if (parser->GetRootObjNum() == objnum) {
    InvalidateCachedRootDict();
  }
  if (parser->GetInfoObjNum() == objnum) {
    InvalidateCachedInfoDict();
  }
}

const CPDF_Object* CPDF_LayerDocument::GetIndirectObjectInternal(
    uint32_t objnum) const {
  if (RetainPtr<CPDF_Object> local = FindLayerVersion(objnum)) {
    return local.Get();
  }
  return base_->GetFrozenObjectForLayer(objnum).Get();
}

CPDF_Object* CPDF_LayerDocument::GetOrParseIndirectObjectInternal(
    uint32_t objnum) {
  return const_cast<CPDF_Object*>(GetIndirectObjectInternal(objnum));
}

uint32_t CPDF_LayerDocument::GetPageObjNumAt(size_t index) const {
  CHECK_LT(index, layer_page_list_.size());
  return layer_page_list_[index];
}

void CPDF_LayerDocument::SetPageObjNumAt(size_t index, uint32_t objnum) {
  std::vector<uint32_t>& pages = MutablePageList();
  CHECK_LT(index, pages.size());
  pages[index] = objnum;
}

void CPDF_LayerDocument::InsertPageObjNum(size_t index, uint32_t objnum) {
  std::vector<uint32_t>& pages = MutablePageList();
  CHECK_LE(index, pages.size());
  pages.insert(pages.begin() + index, objnum);
}

void CPDF_LayerDocument::ErasePageObjNum(size_t index) {
  std::vector<uint32_t>& pages = MutablePageList();
  CHECK_LT(index, pages.size());
  pages.erase(pages.begin() + index);
}

void CPDF_LayerDocument::ResizePageList(size_t size) {
  MutablePageList().resize(size);
}

size_t CPDF_LayerDocument::GetPageListSize() const {
  return layer_page_list_.size();
}

void CPDF_LayerDocument::InitializeFromBase() {
  SetCachedRootDict(
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(base_->GetRoot())));
  SetCachedInfoDict(base_->GetInfo());

  const int page_count = base_->GetPageCount();
  if (page_count < 0) {
    ingest_status_ = OpenStatus::kOpenFailed;
    return;
  }

  layer_page_list_.reserve(static_cast<size_t>(page_count));
  for (int i = 0; i < page_count; ++i) {
    RetainPtr<const CPDF_Dictionary> page = base_->GetPageDictionary(i);
    layer_page_list_.push_back(page ? page->GetObjNum() : 0);
  }
}

void CPDF_LayerDocument::IngestCurrentDelta() {
  if (ingest_status_ != OpenStatus::kSuccess) {
    return;
  }

  CPDF_Parser* base_parser = base_->GetParser();
  if (!base_parser || !file_access_) {
    if (!file_access_) {
      return;
    }
    FailDeltaIngest(OpenStatus::kOpenFailed);
    return;
  }

  const FX_FILESIZE delta_size = file_access_->GetSize();
  if (delta_size == 0) {
    file_access_.Reset();
    return;
  }

  RetainPtr<IFX_SeekableReadStream> base_file = base_parser->GetFileAccess();
  if (!base_file) {
    FailDeltaIngest(OpenStatus::kOpenFailed);
    return;
  }

  const FX_FILESIZE layer_append_base_offset =
      base_->GetLayerAppendBaseOffset();
  CPDF_ParseOnlyHolder temp_holder;
  CPDF_Parser parser(&temp_holder);
  temp_holder.SetParser(&parser);
  ingest_reader_ = pdfium::MakeRetain<CPDF_ConcatReadStream>(
      std::move(base_file), file_access_);
  CPDF_Parser::Error parse_error =
      parser.StartParse(ingest_reader_, base_parser->GetPassword());
  if (parse_error != CPDF_Parser::SUCCESS) {
    FailDeltaIngest(OpenStatus::kMalformedDelta);
    return;
  }
  if (parser.GetLastXRefOffset() < layer_append_base_offset) {
    FailDeltaIngest(OpenStatus::kMalformedDelta);
    return;
  }

  const CPDF_CrossRefTable* table = parser.GetCrossRefTable();
  if (!table) {
    FailDeltaIngest(OpenStatus::kMalformedDelta);
    return;
  }

  for (const auto& [objnum, info] : table->objects_info()) {
    if (info.type == CPDF_CrossRefTable::ObjectType::kFree &&
        IsBaseObjectLive(base_parser, objnum)) {
      FailDeltaIngest(OpenStatus::kMalformedDelta);
      return;
    }
  }

  size_t selected_delta_object_count = 0;
  for (const auto& [objnum, info] : table->objects_info()) {
    if (!IsObjectOwnedByAppendedDelta(table, objnum, info,
                                      layer_append_base_offset)) {
      continue;
    }

    RetainPtr<CPDF_Object> parsed = parser.ParseIndirectObject(objnum);
    if (!parsed) {
      FailDeltaIngest(OpenStatus::kMalformedDelta);
      return;
    }

    RetainPtr<CPDF_Object> clone = parsed->CloneForHolder(this);
    // The loaded twin: a second pristine clone, references re-homed the same
    // way and never resolved, kept so a save can tell "changed since load"
    // apart from "changed from the base".
    RetainPtr<CPDF_Object> twin = parsed->CloneForHolder(this);
    if (!clone || !twin) {
      FailDeltaIngest(OpenStatus::kMalformedDelta);
      return;
    }
    clone->SetGenNum(info.gennum);
    clone->StampWriteGeneration(CPDF_WriteGeneration::kCommitted);
    twin->SetGenNum(info.gennum);
    loaded_twins_[objnum] = std::move(twin);
    AddPromotedObject(objnum, std::move(clone));
    ++overlay_epoch_;
    ++selected_delta_object_count;
  }

  if (FindLocalIndirectObject(base_parser->GetRootObjNum())) {
    InvalidateCachedRootDict();
  }
  if (FindLocalIndirectObject(base_parser->GetInfoObjNum())) {
    InvalidateCachedInfoDict();
  }
  const uint32_t delta_info_objnum = parser.GetInfoObjNum();
  if (delta_info_objnum && delta_info_objnum != CPDF_Object::kInvalidObjNum &&
      delta_info_objnum != base_parser->GetInfoObjNum()) {
    RetainPtr<CPDF_Object> local_info =
        FindLocalIndirectObject(delta_info_objnum);
    RetainPtr<CPDF_Dictionary> info =
        local_info ? pdfium::WrapRetain(local_info->AsMutableDictionary())
                   : nullptr;
    if (!info) {
      FailDeltaIngest(OpenStatus::kMalformedDelta);
      return;
    }
    SetCachedInfoDict(info);
  }
  if (selected_delta_object_count > 0 &&
      !RebuildPageListFromCurrentPageTree()) {
    FailDeltaIngest(OpenStatus::kMalformedDelta);
    return;
  }
  // The delta is now part of this layer's loaded bytes: keep it, so revision
  // analysis can read base + delta exactly as they were given.
  loaded_delta_ = std::move(file_access_);
  file_access_.Reset();
}

void CPDF_LayerDocument::FailDeltaIngest(OpenStatus status) {
  ingest_status_ = status;
  file_access_.Reset();
}

RetainPtr<CPDF_Object> CPDF_LayerDocument::PromoteFromBase(uint32_t objnum) {
  if (!objnum || objnum == CPDF_Object::kInvalidObjNum) {
    return nullptr;
  }
  if (RetainPtr<CPDF_Object> local = FindLocalIndirectObject(objnum)) {
    return local;
  }

  RetainPtr<const CPDF_Object> base_object =
      base_->GetFrozenObjectForLayer(objnum);
  if (!base_object) {
    return nullptr;
  }

  RetainPtr<CPDF_Object> clone = base_object->CloneForHolder(this);
  if (!clone) {
    return nullptr;
  }
  clone->SetGenNum(base_object->GetGenNum());
  clone->StampWriteGeneration(CPDF_WriteGeneration::kCommitted);
  AddPromotedObject(objnum, clone);
  ++overlay_epoch_;

  CPDF_Parser* parser = base_->GetParser();
  if (parser) {
    if (parser->GetRootObjNum() == objnum) {
      InvalidateCachedRootDict();
    }
    if (parser->GetInfoObjNum() == objnum) {
      InvalidateCachedInfoDict();
    }
  }

  return clone;
}
