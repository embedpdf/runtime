// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/edit/cpdf_creator.h"

#include <stdint.h>

#include <inttypes.h>
#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "core/fpdfapi/edit/cpdf_fontsubsetter.h"
#include "core/fpdfapi/edit/cpdf_save_object_reader.h"
#include "core/fpdfapi/edit/cpdf_save_trailer.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_crypto_handler.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_encryptor.h"
#include "core/fpdfapi/parser/cpdf_flateencoder.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object_equality.h"
#include "core/fpdfapi/parser/cpdf_object_walker.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_security_handler.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/containers/contains.h"
#include "core/fxcrt/fixed_size_data_vector.h"
#include "core/fxcrt/fx_extension.h"
#include "core/fxcrt/fx_random.h"
#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/mask.h"
#include "core/fxcrt/raw_span.h"
#include "core/fxcrt/span_util.h"
#include "core/fxcrt/stl_util.h"

namespace {

const size_t kArchiveBufferSize = 32768;

// EmbedPDF(layer-save): the append-only and skip-if-unchanged layer saves.
constexpr Mask<CPDF_Creator::CreateFlags> kAllValidFlags{
    CPDF_Creator::CreateFlags::kIncremental,
    CPDF_Creator::CreateFlags::kNoOriginal,
    CPDF_Creator::CreateFlags::kRemoveSecurity,
    CPDF_Creator::CreateFlags::kSubsetNewFonts,
    CPDF_Creator::CreateFlags::kIncrementalAppendOnly,
    CPDF_Creator::CreateFlags::kSkipIfUnchangedSinceLoad};
constexpr Mask<CPDF_Creator::CreateFlags> kConflictingFlags{
    CPDF_Creator::CreateFlags::kIncremental,
    CPDF_Creator::CreateFlags::kNoOriginal};
// EmbedPDF(layer-save): the largest offset a cross-reference entry can hold.
constexpr FX_FILESIZE kMaxFourByteXrefOffset = 0xffffffff;

class FileBufferArchive final : public IFX_ArchiveStream {
 public:
  explicit FileBufferArchive(RetainPtr<IFX_RetainableWriteStream> file);
  ~FileBufferArchive() override;

  bool WriteBlock(pdfium::span<const uint8_t> buffer) override;
  FX_FILESIZE CurrentOffset() const override { return offset_; }
  // EmbedPDF(layer-save): an append-only delta's offsets count from the end of
  // what it is appended to.
  void SetNotionalStartOffset(FX_FILESIZE offset) { offset_ = offset; }

 private:
  bool Flush();

  FX_FILESIZE offset_ = 0;
  FixedSizeDataVector<uint8_t> buffer_;
  pdfium::raw_span<uint8_t> available_;
  RetainPtr<IFX_RetainableWriteStream> const backing_file_;
};

FileBufferArchive::FileBufferArchive(RetainPtr<IFX_RetainableWriteStream> file)
    : buffer_(FixedSizeDataVector<uint8_t>::Uninit(kArchiveBufferSize)),
      available_(buffer_.span()),
      backing_file_(std::move(file)) {
  DCHECK(backing_file_);
}

FileBufferArchive::~FileBufferArchive() {
  Flush();
}

bool FileBufferArchive::Flush() {
  size_t used = buffer_.size() - available_.size();
  available_ = buffer_.span();
  return used == 0 || backing_file_->WriteBlock(available_.first(used));
}

bool FileBufferArchive::WriteBlock(pdfium::span<const uint8_t> buffer) {
  if (buffer.empty()) {
    return true;
  }

  pdfium::span<const uint8_t> src_span = buffer;
  while (!src_span.empty()) {
    size_t copy_size = std::min(available_.size(), src_span.size());
    available_ = fxcrt::spancpy(available_, src_span.first(copy_size));
    src_span = src_span.subspan(copy_size);
    if (available_.empty() && !Flush()) {
      return false;
    }
  }

  FX_SAFE_FILESIZE safe_offset = offset_;
  safe_offset += buffer.size();
  if (!safe_offset.IsValid()) {
    return false;
  }

  offset_ = safe_offset.ValueOrDie();
  return true;
}

// EmbedPDF(save-generations): a cross-reference stream entry carries its
// object's generation, in two bytes.
bool OutputIndex(IFX_ArchiveStream* archive,
                 FX_FILESIZE offset,
                 uint32_t generation) {
  return archive->WriteByte(static_cast<uint8_t>(offset >> 24)) &&
         archive->WriteByte(static_cast<uint8_t>(offset >> 16)) &&
         archive->WriteByte(static_cast<uint8_t>(offset >> 8)) &&
         archive->WriteByte(static_cast<uint8_t>(offset)) &&
         archive->WriteByte(static_cast<uint8_t>(generation >> 8)) &&
         archive->WriteByte(static_cast<uint8_t>(generation));
}

// EmbedPDF(layer-save): a 64-bit offset in ten digits; upstream formatted it
// with %d. An append-only delta's offsets start where its base ends.
ByteString FormatXrefOffset10(FX_FILESIZE offset) {
  return ByteString::Format("%010" PRId64, static_cast<int64_t>(offset));
}

// EmbedPDF(save-generations): the generations the input file, or a layer's
// loaded delta, gives its objects.
class InputWriteContext final : public CPDF_WriteContext {
 public:
  explicit InputWriteContext(const CPDF_Parser* parser,
                             const CPDF_LayerDocument* layer = nullptr)
      : parser_(parser), layer_(layer) {}

  uint32_t GetObjectGeneration(uint32_t object_number) const override {
    if (layer_) {
      if (auto twin = layer_->FindLoadedDeltaTwin(object_number)) {
        return twin->GetGenNum();
      }
    }
    const auto* info =
        parser_->GetCrossRefTable()->GetObjectInfo(object_number);
    return info && info->type == CPDF_CrossRefTable::ObjectType::kNormal
               ? info->gennum
               : 0;
  }

 private:
  UnownedPtr<const CPDF_Parser> const parser_;
  UnownedPtr<const CPDF_LayerDocument> const layer_;
};

// EmbedPDF(save-traversal): the queue a save walks from its roots, one bit
// per visited object. Only numbered objects enter the work queue. Inline
// containers are visited within one object; primitive values need no
// persistent traversal state.
class SaveObjectWorklist {
 public:
  explicit SaveObjectWorklist(CPDF_SaveObjectReader* preferred_reader = nullptr)
      : preferred_reader_(preferred_reader) {}

  void Add(uint32_t object_number) {
    if (object_number == 0 || object_number == CPDF_Object::kInvalidObjNum) {
      return;
    }

    // Sparse bitmap pages avoid allocating by an untrusted maximum object
    // number, while keeping dense documents to one bit per object.
    constexpr uint32_t kObjectsPerPage = 4096;
    auto& page = visited_[object_number / kObjectsPerPage];
    const uint32_t index = object_number % kObjectsPerPage;
    uint64_t& word = page[index / 64];
    const uint64_t mask = uint64_t{1} << (index % 64);
    if ((word & mask) != 0) {
      return;
    }
    word |= mask;
    if (preferred_reader_ && preferred_reader_->IsCached(object_number)) {
      preferred_.push_back(object_number);
    } else {
      pending_.push_back(object_number);
    }
  }

  void AddReferences(RetainPtr<const CPDF_Object> object) {
    for (uint32_t number : CPDF_CollectReferences(std::move(object))) {
      Add(number);
    }
  }

  bool empty() const { return preferred_.empty() && pending_.empty(); }

  uint32_t TakeNext() {
    auto& queue = preferred_.empty() ? pending_ : preferred_;
    const uint32_t object_number = queue.front();
    queue.pop_front();
    return object_number;
  }

 private:
  std::map<uint32_t, std::array<uint64_t, 64>> visited_;
  UnownedPtr<CPDF_SaveObjectReader> const preferred_reader_;
  std::deque<uint32_t> preferred_;
  std::deque<uint32_t> pending_;
};

}  // namespace

CPDF_Creator::CPDF_Creator(CPDF_Document* doc,
                           RetainPtr<IFX_RetainableWriteStream> archive)
    : document_(doc),
      parser_(doc->GetParser()),
      encrypt_dict_(parser_ ? parser_->GetEncryptDict() : nullptr),
      security_handler_(parser_ ? parser_->GetSecurityHandler() : nullptr),
      last_obj_num_(document_->GetLastObjNum()),
      archive_(std::make_unique<FileBufferArchive>(std::move(archive))) {}

CPDF_Creator::~CPDF_Creator() = default;

// EmbedPDF(layer-save): FormatXrefOffset10() for tests.
// static
ByteString CPDF_Creator::FormatXrefOffset10ForTesting(FX_FILESIZE offset) {
  return FormatXrefOffset10(offset);
}

// EmbedPDF(save-generations): an object keeps its generation, in its header,
// its encryption key and the references it writes.
bool CPDF_Creator::WriteIndirectObj(uint32_t objnum, const CPDF_Object* pObj) {
  const uint32_t generation = GetObjectGeneration(objnum);
  if (!archive_->WriteDWord(objnum) || !archive_->WriteString(" ") ||
      !archive_->WriteDWord(generation) || !archive_->WriteString(" obj\r\n")) {
    return false;
  }

  std::unique_ptr<CPDF_Encryptor> encryptor;
  // EmbedPDF(bug-fix): see IsClearMetadata().
  if (GetCryptoHandler() && pObj != encrypt_dict_ && !IsClearMetadata(objnum)) {
    encryptor = std::make_unique<CPDF_Encryptor>(GetCryptoHandler(), objnum,
                                                 generation);
  }

  if (!pObj->WriteTo(archive_.get(), encryptor.get(), this)) {
    return false;
  }

  return archive_->WriteString("\r\nendobj\r\n");
}

// EmbedPDF(bug-fix): the document's metadata stream stays in the clear when
// the encryption dictionary says /EncryptMetadata false, as the parser reads
// it. Every other stream is encrypted, other metadata streams included.
bool CPDF_Creator::IsClearMetadata(uint32_t objnum) const {
  if (!security_handler_ || security_handler_->IsMetadataEncrypted()) {
    return false;
  }
  const CPDF_Dictionary* root = document_->GetRoot();
  RetainPtr<const CPDF_Reference> metadata =
      root ? ToReference(root->GetObjectFor("Metadata")) : nullptr;
  return metadata && metadata->GetRefObjNum() == objnum;
}

// EmbedPDF(save-generations): the generation an incremental save writes for an
// object: the layer's version's, or the input file's. A full save writes 0.
uint32_t CPDF_Creator::GetObjectGeneration(uint32_t object_number) const {
  if (!is_incremental_) {
    return 0;
  }

  // A loaded layer delta may carry an identity newer than the base's xref.
  // This lookup is cache-only and never promotes or parses an object.
  if (auto local = document_->FindPromotedObject(object_number)) {
    return local->GetGenNum();
  }
  if (parser_) {
    return InputWriteContext(parser_).GetObjectGeneration(object_number);
  }
  // New objects and objects originally stored in object streams use zero.
  return 0;
}

// EmbedPDF(save-generations): a trailer reference with its target's
// generation.
bool CPDF_Creator::WriteReference(uint32_t object_number) {
  return archive_->WriteString(" ") && archive_->WriteDWord(object_number) &&
         archive_->WriteString(" ") &&
         archive_->WriteDWord(GetObjectGeneration(object_number)) &&
         archive_->WriteString(" R");
}

// EmbedPDF(save-traversal): a full save walks the live objects from the
// trailer's roots and reads them through CPDF_SaveObjectReader, without filling
// the document's caches. Replaces WriteOldIndirectObject() and WriteOldObjs().
bool CPDF_Creator::WriteFullDocument() {
  CPDF_DocumentViewScope view(document_);
  CPDF_SaveObjectReader reader(document_);
  SaveObjectWorklist pending;

  // Upstream subsets new fonts in WriteNewObjs(), which a full save doesn't
  // run.
  std::map<uint32_t, RetainPtr<const CPDF_Object>> font_obj_overrides;
  if (subset_new_fonts_) {
    CPDF_FontSubsetter subsetter(document_);
    font_obj_overrides = subsetter.GenerateObjectOverrides(NewObjectNumbers());
  }

  for (uint32_t number : trailer_->roots()) {
    pending.Add(number);
  }

  while (!pending.empty()) {
    const uint32_t object_number = pending.TakeNext();
    RetainPtr<const CPDF_Object> object = reader.Read(object_number);
    if (!object) {
      // Match the existing writer's treatment of unresolved references.
      continue;
    }
    if (auto it = font_obj_overrides.find(object_number);
        it != font_obj_overrides.end()) {
      object = it->second;
    }

    pending.AddReferences(object);
    object_offsets_[object_number] = archive_->CurrentOffset();
    if (!WriteIndirectObj(object_number, object.Get())) {
      return false;
    }
  }

  if (!object_offsets_.empty()) {
    last_obj_num_ = object_offsets_.rbegin()->first;
  }

  return true;
}

std::vector<uint32_t> CPDF_Creator::NewObjectNumbers() const {
  std::vector<uint32_t> numbers;
  for (const auto& [object_number, object] : *document_) {
    if (object->GetObjNum() == CPDF_Object::kInvalidObjNum) {
      continue;
    }
    if (parser_ && parser_->IsValidObjectNumber(object_number) &&
        !parser_->IsObjectFree(object_number)) {
      continue;
    }
    numbers.push_back(object_number);
  }
  return numbers;
}

// EmbedPDF(view-scope, layer-save): objects are read as the layer sees them,
// and the cross-reference lists only the ones written.
bool CPDF_Creator::WriteNewObjs() {
  CPDF_DocumentViewScope view(document_);
  std::map<uint32_t, RetainPtr<const CPDF_Object>> font_obj_overrides;
  if (subset_new_fonts_) {
    CPDF_FontSubsetter subsetter(document_);
    font_obj_overrides = subsetter.GenerateObjectOverrides(new_obj_num_array_);
  }
  std::vector<uint32_t> written_new_obj_nums;
  for (size_t i = cur_obj_num_; i < new_obj_num_array_.size(); ++i) {
    uint32_t objnum = new_obj_num_array_[i];
    RetainPtr<const CPDF_Object> obj = document_->GetIndirectObject(objnum);
    if (!obj) {
      continue;
    }

    object_offsets_[objnum] = archive_->CurrentOffset();

    auto it = font_obj_overrides.find(objnum);
    const CPDF_Object* obj_to_write =
        it != font_obj_overrides.end() ? it->second.Get() : obj.Get();
    if (!WriteIndirectObj(objnum, obj_to_write)) {
      return false;
    }
    written_new_obj_nums.push_back(objnum);
  }
  new_obj_num_array_ = std::move(written_new_obj_nums);
  return true;
}

// EmbedPDF(layer-save): refuse an offset a cross-reference entry can't hold;
// an append-only delta's offsets start where its base ends.
bool CPDF_Creator::CheckEmittedOffset(FX_FILESIZE offset) {
  if (offset <= kMaxFourByteXrefOffset) {
    return true;
  }
  failure_reason_ = FailureReason::kAppendOnlyOffsetTooLarge;
  return false;
}

// EmbedPDF(layer-save, save-traversal): an incremental save writes the objects
// that differ from the base and that a saved root still reaches, and records
// whether any differ from what the layer loaded. Replaces
// InitNewObjNumOffsets().
void CPDF_Creator::PrepareIncrementalObjects() {
  struct Change {
    bool differs_from_base;
    bool differs_from_loaded;
  };

  std::map<uint32_t, Change> candidates;
  const auto* layer = CPDF_LayerDocument::FromDocument(document_);
  const InputWriteContext original_context(parser_);
  const InputWriteContext loaded_context(parser_, layer);
  CPDF_SaveObjectReader original_reader(
      document_, CPDF_SaveObjectReader::Version::kOriginal);

  // Compare only objects already in the document/overlay. Parsing a twin uses
  // the original bytes and a private bounded cache, never the edited object
  // or the shared base's lazy-loading path.
  for (const auto& [object_number, object] : *document_) {
    if (object->GetObjNum() == CPDF_Object::kInvalidObjNum) {
      continue;
    }
    auto base_twin = original_reader.Read(object_number);
    const bool differs_from_base =
        !base_twin ||
        GetObjectGeneration(object_number) !=
            original_context.GetObjectGeneration(object_number) ||
        !CPDF_SameEffectiveValue(object.Get(), base_twin.Get(), this,
                                 &original_context);
    auto loaded_twin =
        layer ? layer->FindLoadedDeltaTwin(object_number) : nullptr;
    bool differs_from_loaded = differs_from_base;
    if (loaded_twin) {
      differs_from_loaded =
          GetObjectGeneration(object_number) !=
              loaded_context.GetObjectGeneration(object_number) ||
          !CPDF_SameEffectiveValue(object.Get(), loaded_twin.Get(), this,
                                   &loaded_context);
    }
    if (differs_from_base || differs_from_loaded) {
      candidates.emplace(object_number,
                         Change{differs_from_base, differs_from_loaded});
    }
  }

  if (candidates.empty()) {
    return;
  }

  // An unchanged document needs neither a graph walk nor an empty revision.
  // Changed candidates still need a path from a saved root: detached objects
  // must not resurrect an edit or prevent an otherwise unchanged save.
  CPDF_SaveObjectReader effective_reader(document_);
  // Editing a page usually loads its path through the page tree. Follow those
  // already-loaded edges first, before opening unrelated resource graphs.
  // Every visited object still needs a proven path from a saved root.
  SaveObjectWorklist pending(&effective_reader);
  for (uint32_t number : trailer_->roots()) {
    pending.Add(number);
  }

  while (!candidates.empty() && !pending.empty()) {
    const uint32_t object_number = pending.TakeNext();
    auto candidate = candidates.find(object_number);
    if (candidate != candidates.end()) {
      if (candidate->second.differs_from_base) {
        new_obj_num_array_.push_back(object_number);
      }
      changed_since_load_ |= candidate->second.differs_from_loaded;
      candidates.erase(candidate);
      if (candidates.empty()) {
        break;
      }
    }
    for (uint32_t reference : effective_reader.ReferencesFor(object_number)) {
      pending.Add(reference);
    }
  }
  std::ranges::sort(new_obj_num_array_);
}

CPDF_Creator::Stage CPDF_Creator::WriteDoc_Stage1() {
  DCHECK(stage_ > Stage::kInvalid || stage_ < Stage::kInitWriteObjs20);
  if (stage_ == Stage::kInit0) {
    if (!parser_ || (security_changed_ && is_original_)) {
      is_incremental_ = false;
    }

    stage_ = Stage::kWriteHeader10;
  }
  if (stage_ == Stage::kWriteHeader10) {
    if (!is_incremental_) {
      if (!archive_->WriteString("%PDF-1.")) {
        return Stage::kInvalid;
      }

      int32_t version = 7;
      if (file_version_) {
        version = file_version_;
      } else if (parser_) {
        version = parser_->GetFileVersion();
      }

      if (!archive_->WriteDWord(version % 10) ||
          !archive_->WriteString("\r\n%\xA1\xB3\xC5\xD7\r\n")) {
        return Stage::kInvalid;
      }
      stage_ = Stage::kInitWriteObjs20;
    } else {
      // EmbedPDF(layer-save): an append-only delta continues from the end of
      // the layer's base and loaded delta.
      saved_offset_ = is_incremental_append_only_
                          ? document_->GetLayerAppendBaseOffset()
                          : parser_->GetDocumentSize();
      stage_ = Stage::kWriteIncremental15;
    }
  }
  if (stage_ == Stage::kWriteIncremental15) {
    // EmbedPDF(layer-save): an append-only delta copies no original bytes.
    if (is_original_ && !is_incremental_append_only_ && saved_offset_ > 0) {
      if (!parser_->WriteToArchive(archive_.get(), saved_offset_)) {
        failure_reason_ = FailureReason::kArchiveError;
        return Stage::kInvalid;
      }
    }
    if (is_original_ && parser_->GetLastXRefOffset() == 0) {
      for (uint32_t num = 0; num <= parser_->GetLastObjNum(); ++num) {
        if (parser_->IsObjectFree(num)) {
          continue;
        }

        object_offsets_[num] = parser_->GetObjectPositionOrZero(num);
      }
    }
    stage_ = Stage::kInitWriteObjs20;
  }
  // EmbedPDF(layer-save): no InitNewObjNumOffsets() here: Create() picks the
  // objects an incremental save writes (PrepareIncrementalObjects()).
  return stage_;
}

CPDF_Creator::Stage CPDF_Creator::WriteDoc_Stage2() {
  DCHECK(stage_ >= Stage::kInitWriteObjs20 ||
         stage_ < Stage::kInitWriteXRefs80);
  if (stage_ == Stage::kInitWriteObjs20) {
    // EmbedPDF(save-traversal): a full save writes what WriteFullDocument()
    // reaches from the trailer's roots, new objects included; the old-objects
    // stage (kWriteOldObjs21) is gone.
    if (!is_incremental_) {
      if (!WriteFullDocument()) {
        return Stage::kInvalid;
      }
      stage_ = Stage::kWriteEncryptDict27;
    } else {
      stage_ = Stage::kInitWriteNewObjs25;
    }
  }
  if (stage_ == Stage::kInitWriteNewObjs25) {
    cur_obj_num_ = 0;
    stage_ = Stage::kWriteNewObjs26;
  }
  if (stage_ == Stage::kWriteNewObjs26) {
    if (!WriteNewObjs()) {
      return Stage::kInvalid;
    }

    stage_ = Stage::kWriteEncryptDict27;
  }
  if (stage_ == Stage::kWriteEncryptDict27) {
    if (encrypt_dict_ && encrypt_dict_->IsInline()) {
      // EmbedPDF(save-traversal): the number the trailer gives the encryption
      // dictionary (BuildTrailer()).
      const uint32_t encryption_number = trailer_->encryption_number();
      last_obj_num_ = std::max(last_obj_num_, encryption_number);
      FX_FILESIZE saveOffset = archive_->CurrentOffset();
      if (!WriteIndirectObj(encryption_number, encrypt_dict_.Get())) {
        return Stage::kInvalid;
      }

      object_offsets_[encryption_number] = saveOffset;
      if (is_incremental_) {
        new_obj_num_array_.push_back(encryption_number);
      }
    }
    stage_ = Stage::kInitWriteXRefs80;
  }
  return stage_;
}

CPDF_Creator::Stage CPDF_Creator::WriteDoc_Stage3() {
  DCHECK(stage_ >= Stage::kInitWriteXRefs80 ||
         stage_ < Stage::kWriteTrailerAndFinish90);

  uint32_t dwLastObjNum = last_obj_num_;
  if (stage_ == Stage::kInitWriteXRefs80) {
    if (is_incremental_ && skip_empty_revision_ &&
        new_obj_num_array_.empty()) {
      // EmbedPDF(layer-save): an incremental save of a layer with nothing to
      // write appends no revision at all - not even an empty cross-reference
      // section. Standalone output is the base bytes copied through; an
      // append-only delta is empty (the layer equals its base).
      stage_ = Stage::kComplete100;
      return stage_;
    }
    xref_start_ = archive_->CurrentOffset();
    // EmbedPDF(layer-save): an append-only delta writes a cross-reference
    // table, never a stream.
    if (!is_incremental_ || is_incremental_append_only_ ||
        !parser_->IsXRefStream()) {
      if (!is_incremental_ || parser_->GetLastXRefOffset() == 0) {
        // EmbedPDF(complete-xref): the subsection header is written with the
        // entries (kWriteXrefsNotIncremental81).
        if (!archive_->WriteString("xref\r\n")) {
          return Stage::kInvalid;
        }

        stage_ = Stage::kWriteXrefsNotIncremental81;
      } else {
        if (!archive_->WriteString("xref\r\n")) {
          return Stage::kInvalid;
        }

        cur_obj_num_ = 0;
        stage_ = Stage::kWriteXrefsIncremental82;
      }
    } else {
      stage_ = Stage::kWriteTrailerAndFinish90;
    }
  }
  if (stage_ == Stage::kWriteXrefsNotIncremental81) {
    // EmbedPDF(complete-xref): the file's only table is one subsection, from
    // object 0 through the last object number (ISO 32000-1 7.5.4). A number
    // no written object holds - reserved by an editing session and never
    // used, deleted, or no longer reached - is a free entry. Entry 0 heads
    // the list of free entries, each links to the next higher one, and the
    // last links back to 0. A free entry's generation is 0, the generation a
    // new object at that number starts with. Upstream wrote a subsection per
    // run of written objects and gave the numbers between runs no entry;
    // Acrobat then can't judge the changes a later revision makes.
    std::vector<uint32_t> free_numbers;
    for (uint32_t number = 1; number <= dwLastObjNum; ++number) {
      if (!pdfium::Contains(object_offsets_, number)) {
        free_numbers.push_back(number);
      }
    }
    FX_SAFE_UINT32 entry_count = dwLastObjNum;
    entry_count += 1;
    if (!entry_count.IsValid()) {
      return Stage::kInvalid;
    }
    const uint32_t first_free = free_numbers.empty() ? 0 : free_numbers[0];
    ByteString str = ByteString::Format("0 %u\r\n", entry_count.ValueOrDie()) +
                     FormatXrefOffset10(first_free) + " 65535 f\r\n";
    if (!archive_->WriteString(str.AsStringView())) {
      return Stage::kInvalid;
    }
    size_t next_free = 0;
    for (uint32_t objnum = 1; objnum <= dwLastObjNum; ++objnum) {
      if (next_free < free_numbers.size() &&
          free_numbers[next_free] == objnum) {
        ++next_free;
        const uint32_t link =
            next_free < free_numbers.size() ? free_numbers[next_free] : 0;
        str = FormatXrefOffset10(link) + " 00000 f\r\n";
      } else {
        // EmbedPDF(save-generations, layer-save): each entry carries its
        // object's generation, and only an offset an entry can hold.
        const FX_FILESIZE offset = object_offsets_[objnum];
        if (!CheckEmittedOffset(offset)) {
          return Stage::kInvalid;
        }
        str = FormatXrefOffset10(offset) +
              ByteString::Format(" %05u n\r\n", GetObjectGeneration(objnum));
      }
      if (!archive_->WriteString(str.AsStringView())) {
        return Stage::kInvalid;
      }
    }
    stage_ = Stage::kWriteTrailerAndFinish90;
  }
  if (stage_ == Stage::kWriteXrefsIncremental82) {
    ByteString str;
    uint32_t iCount = fxcrt::CollectionSize<uint32_t>(new_obj_num_array_);
    uint32_t i = cur_obj_num_;
    while (i < iCount) {
      size_t j = i;
      uint32_t objnum = new_obj_num_array_[i];
      while (j < iCount) {
        if (++j == iCount) {
          break;
        }
        uint32_t dwCurrent = new_obj_num_array_[j];
        if (dwCurrent - objnum > 1) {
          break;
        }
        objnum = dwCurrent;
      }
      objnum = new_obj_num_array_[i];
      if (objnum == 1) {
        str = ByteString::Format("0 %d\r\n0000000000 65535 f\r\n", j - i + 1);
      } else {
        str = ByteString::Format("%d %d\r\n", objnum, j - i);
      }

      if (!archive_->WriteString(str.AsStringView())) {
        return Stage::kInvalid;
      }

      while (i < j) {
        objnum = new_obj_num_array_[i++];
        // EmbedPDF(save-generations, layer-save): each entry carries its
        // object's generation, and only an offset an entry can hold.
        const FX_FILESIZE offset = object_offsets_[objnum];
        if (!CheckEmittedOffset(offset)) {
          return Stage::kInvalid;
        }
        str = FormatXrefOffset10(offset) +
              ByteString::Format(" %05u n\r\n", GetObjectGeneration(objnum));
        if (!archive_->WriteString(str.AsStringView())) {
          return Stage::kInvalid;
        }
      }
    }
    stage_ = Stage::kWriteTrailerAndFinish90;
  }
  return stage_;
}

CPDF_Creator::Stage CPDF_Creator::WriteDoc_Stage4() {
  DCHECK_GE(stage_, Stage::kWriteTrailerAndFinish90);

  // EmbedPDF(layer-save): an append-only delta writes a cross-reference
  // table, never a stream.
  bool bXRefStream = is_incremental_ && !is_incremental_append_only_ &&
                     parser_->IsXRefStream();
  if (!bXRefStream) {
    if (!archive_->WriteString("trailer\r\n<<")) {
      return Stage::kInvalid;
    }
  } else {
    // EmbedPDF(bug-fix): the number the stream's own entry lists (below).
    if (!archive_->WriteDWord(last_obj_num_ + 1) ||
        !archive_->WriteString(" 0 obj <<")) {
      return Stage::kInvalid;
    }
  }

  // EmbedPDF(save-traversal, save-generations): the trailer BuildTrailer()
  // chose: its copied entries, then /Root, /Info and /Encrypt with their
  // generations.
  for (const auto& [key, value] : trailer_->copied_entries()) {
    if (!archive_->WriteString("/") ||
        !archive_->WriteString(PDF_NameEncode(key).AsStringView()) ||
        !value->WriteTo(archive_.get(), nullptr, this)) {
      return Stage::kInvalid;
    }
  }
  if (trailer_->root_number()) {
    if (!archive_->WriteString("\r\n/Root") ||
        !WriteReference(trailer_->root_number()) ||
        !archive_->WriteString("\r\n")) {
      return Stage::kInvalid;
    }
  }
  if (trailer_->info_number()) {
    if (!archive_->WriteString("/Info") ||
        !WriteReference(trailer_->info_number()) ||
        !archive_->WriteString("\r\n")) {
      return Stage::kInvalid;
    }
  }
  if (trailer_->encryption_number()) {
    if (!archive_->WriteString("/Encrypt") ||
        !WriteReference(trailer_->encryption_number()) ||
        !archive_->WriteString(" ")) {
      return Stage::kInvalid;
    }
  }

  // EmbedPDF(save-traversal): /Size can't overflow, and an incremental save
  // never shrinks it below the input's.
  FX_SAFE_UINT32 trailer_size = last_obj_num_;
  trailer_size += bXRefStream ? 2 : 1;
  if (!trailer_size.IsValid()) {
    return Stage::kInvalid;
  }
  uint32_t size = trailer_size.ValueOrDie();
  if (is_incremental_) {
    const int original_size =
        parser_->GetCombinedTrailer()->GetIntegerFor("Size");
    if (original_size > 0) {
      size = std::max(size, static_cast<uint32_t>(original_size));
    }
  }
  if (!archive_->WriteString("/Size ") || !archive_->WriteDWord(size)) {
    return Stage::kInvalid;
  }
  if (is_incremental_) {
    FX_FILESIZE prev = parser_->GetLastXRefOffset();
    if (prev) {
      if (!archive_->WriteString("/Prev ") || !archive_->WriteFilesize(prev)) {
        return Stage::kInvalid;
      }
    }
  }
  if (id_array_) {
    // EmbedPDF(save-generations): writers carry the write context.
    if (!archive_->WriteString(("/ID")) ||
        !id_array_->WriteTo(archive_.get(), nullptr, this)) {
      return Stage::kInvalid;
    }
  }
  if (!bXRefStream) {
    if (!archive_->WriteString(">>")) {
      return Stage::kInvalid;
    }
  } else {
    // EmbedPDF(bug-fix, save-generations): a cross-reference stream is a
    // stream object with a /Type, closed by endobj like any other (ISO
    // 32000-2 7.5.8); upstream wrote neither, and readers that walk objects
    // (the trailer-end scanner, pyHanko) stop at a missing one. Include the
    // xref stream's own entry. Two bytes cover every generation allowed by
    // PDF, including generations above 255.
    const uint32_t xref_object_number = last_obj_num_ + 1;
    object_offsets_[xref_object_number] = xref_start_;
    if (!archive_->WriteString("/Type/XRef/W[0 4 2]/Index[")) {
      return Stage::kInvalid;
    }
    for (const auto& [object_number, offset] : object_offsets_) {
      if (!archive_->WriteDWord(object_number) ||
          !archive_->WriteString(" 1 ")) {
        return Stage::kInvalid;
      }
    }
    FX_SAFE_UINT32 length = object_offsets_.size();
    length *= 6;
    if (!length.IsValid() || !archive_->WriteString("]/Length ") ||
        !archive_->WriteDWord(length.ValueOrDie()) ||
        !archive_->WriteString(">>stream\r\n")) {
      return Stage::kInvalid;
    }
    for (const auto& [object_number, offset] : object_offsets_) {
      const uint32_t generation = object_number == xref_object_number
                                      ? 0
                                      : GetObjectGeneration(object_number);
      if (!CheckEmittedOffset(offset) ||
          !OutputIndex(archive_.get(), offset, generation)) {
        return Stage::kInvalid;
      }
    }
    if (!archive_->WriteString("\r\nendstream\r\nendobj")) {
      return Stage::kInvalid;
    }
  }

  if (!archive_->WriteString("\r\nstartxref\r\n") ||
      !archive_->WriteFilesize(xref_start_) ||
      !archive_->WriteString("\r\n%%EOF\r\n")) {
    return Stage::kInvalid;
  }

  stage_ = Stage::kComplete100;
  return stage_;
}

bool CPDF_Creator::Create(Mask<CreateFlags> flags, int32_t file_version) {
  // EmbedPDF(layer-save): GetFailureReason() says why a save failed.
  failure_reason_ = FailureReason::kNone;
  // EmbedPDF(layer-transaction): a layer's open transaction is not part of the
  // document until it commits, and saving walks only committed objects.
  // Refuse rather than write a document that is neither the state before the
  // transaction nor after it.
  if (const CPDF_LayerDocument* layer =
          CPDF_LayerDocument::FromDocument(document_.get());
      layer && layer->InTransaction()) {
    return false;
  }
  if (flags & ~kAllValidFlags) {
    flags = CreateFlags::kNone;
  }

  if (flags == CreateFlags::kRemoveSecurityDeprecated ||
      (flags & CreateFlags::kRemoveSecurity)) {
    RemoveSecurity();
  }

  if (flags.TestAll(kConflictingFlags)) {
    flags.Clear(kConflictingFlags);
  }

  is_incremental_ = !!(flags & CreateFlags::kIncremental);
  // EmbedPDF(layer-save): an append-only save writes only the new revision;
  // its offsets count from the end of the layer's base and loaded delta.
  is_incremental_append_only_ = !!(flags & CreateFlags::kIncrementalAppendOnly);
  if (is_incremental_append_only_ && !is_incremental_) {
    failure_reason_ = FailureReason::kOther;
    return false;
  }
  is_original_ = !(flags & CreateFlags::kNoOriginal);
  subset_new_fonts_ = !!(flags & CreateFlags::kSubsetNewFonts);
  if (is_incremental_append_only_ && parser_) {
    static_cast<FileBufferArchive*>(archive_.get())
        ->SetNotionalStartOffset(document_->GetLayerAppendBaseOffset());
  }

  if (file_version >= 10 && file_version <= 17) {
    file_version_ = file_version;
  }

  stage_ = Stage::kInit0;
  last_obj_num_ = document_->GetLastObjNum();
  object_offsets_.clear();
  new_obj_num_array_.clear();
  // EmbedPDF(layer-save): what this save finds about the layer.
  changed_since_load_ = false;
  decided_unchanged_ = false;
  skip_empty_revision_ = false;

  InitID();
  // EmbedPDF(save-traversal): pick the trailer, root, /Info and /Encrypt once,
  // before writing.
  if (!BuildTrailer()) {
    failure_reason_ = FailureReason::kOther;
    return false;
  }
  // EmbedPDF(layer-save): decided here as well as in WriteDoc_Stage1(), so an
  // incremental save picks the objects that changed before it writes. With
  // kSkipIfUnchangedSinceLoad and nothing changed since load, it writes
  // nothing.
  if (!parser_ || (security_changed_ && is_original_)) {
    is_incremental_ = false;
  }
  if (is_incremental_) {
    skip_empty_revision_ = true;
    PrepareIncrementalObjects();
    if (!changed_since_load_ &&
        !!(flags & CreateFlags::kSkipIfUnchangedSinceLoad)) {
      // A reopened layer may differ from the base but equal its loaded delta.
      // Let the caller retain those loaded bytes verbatim.
      decided_unchanged_ = true;
      stage_ = Stage::kInvalid;
      return true;
    }
  }
  const bool result = Continue();
  if (!result && failure_reason_ == FailureReason::kNone) {
    failure_reason_ = FailureReason::kOther;
  }
  return result;
}

// EmbedPDF(save-traversal): one trailer for the whole save: the root, /Info
// and /Encrypt it names and the entries it copies (CPDF_SaveTrailer).
bool CPDF_Creator::BuildTrailer() {
  uint32_t encryption_number = encrypt_dict_ ? encrypt_dict_->GetObjNum() : 0;
  if (encrypt_dict_ && encrypt_dict_->IsInline()) {
    FX_SAFE_UINT32 next_number = document_->GetLastObjNum();
    next_number += 1;
    if (!next_number.IsValid()) {
      return false;
    }
    encryption_number = next_number.ValueOrDie();
  }
  trailer_ = std::make_unique<CPDF_SaveTrailer>(
      parser_ ? parser_->GetCombinedTrailer() : nullptr,
      document_->GetRoot()->GetObjNum(), document_->GetInfoObjectNumber(),
      parser_ ? parser_->GetInfoObjNum() : 0, encrypt_dict_, encryption_number,
      id_array_);
  return true;
}

void CPDF_Creator::InitID() {
  DCHECK(!id_array_);

  id_array_ = document_->New<CPDF_Array>();
  // EmbedPDF(annot-appearance): a created document can carry a preset
  // identifier, so a canonical stamp drawing is the same bytes every time.
  RetainPtr<const CPDF_Array> pOldIDArray =
      parser_ ? parser_->GetIDArray() : document_->GetFileIdentifier();
  RetainPtr<const CPDF_Object> pID1 =
      pOldIDArray ? pOldIDArray->GetObjectAt(0) : nullptr;
  if (pID1) {
    id_array_->Append(pID1->Clone());
  } else {
    std::array<uint32_t, 4> file_id;
    FX_Random::Fill(file_id);
    id_array_->AppendNew<CPDF_String>(pdfium::as_byte_span(file_id),
                                      CPDF_String::DataType::kIsHex);
  }

  if (pOldIDArray) {
    RetainPtr<const CPDF_Object> pID2 = pOldIDArray->GetObjectAt(1);
    // EmbedPDF(annot-appearance): a preset identifier (a canonical stamp
    // drawing's) is written as given, both halves.
    if (!parser_ && pID2) {
      id_array_->Append(pID2->Clone());
      return;
    }
    if (is_incremental_ && encrypt_dict_ && pID2) {
      id_array_->Append(pID2->Clone());
      return;
    }
    std::array<uint32_t, 4> file_id;
    FX_Random::Fill(file_id);
    id_array_->AppendNew<CPDF_String>(pdfium::as_byte_span(file_id),
                                      CPDF_String::DataType::kIsHex);
    return;
  }

  id_array_->Append(id_array_->GetObjectAt(0)->Clone());
  if (encrypt_dict_) {
    DCHECK(parser_);
    int revision = encrypt_dict_->GetIntegerFor("R");
    if ((revision == 2 || revision == 3) &&
        encrypt_dict_->GetByteStringFor("Filter") == "Standard") {
      new_encrypt_dict_ = ToDictionary(encrypt_dict_->Clone());
      encrypt_dict_ = new_encrypt_dict_;
      security_handler_ = pdfium::MakeRetain<CPDF_SecurityHandler>();
      security_handler_->OnCreate(new_encrypt_dict_.Get(), id_array_.Get(),
                                  parser_->GetEncodedPassword());
      security_changed_ = true;
    }
  }
}

bool CPDF_Creator::Continue() {
  if (stage_ < Stage::kInit0) {
    return false;
  }

  Stage iRet = Stage::kInit0;
  while (stage_ < Stage::kComplete100) {
    if (stage_ < Stage::kInitWriteObjs20) {
      iRet = WriteDoc_Stage1();
    } else if (stage_ < Stage::kInitWriteXRefs80) {
      iRet = WriteDoc_Stage2();
    } else if (stage_ < Stage::kWriteTrailerAndFinish90) {
      iRet = WriteDoc_Stage3();
    } else {
      iRet = WriteDoc_Stage4();
    }

    if (iRet < stage_) {
      break;
    }
  }

  if (iRet <= Stage::kInit0 || stage_ == Stage::kComplete100) {
    stage_ = Stage::kInvalid;
    return iRet > Stage::kInit0;
  }

  return stage_ > Stage::kInvalid;
}

void CPDF_Creator::RemoveSecurity() {
  security_handler_.Reset();
  security_changed_ = true;
  encrypt_dict_ = nullptr;
  new_encrypt_dict_.Reset();
}

// EmbedPDF(save-security): encryption for a document that wasn't encrypted
// (EPDF_SetEncryption()).
void CPDF_Creator::SetEncryption(
    RetainPtr<CPDF_Dictionary> encrypt_dict,
    RetainPtr<CPDF_SecurityHandler> security_handler) {
  new_encrypt_dict_ = std::move(encrypt_dict);
  encrypt_dict_ = new_encrypt_dict_;
  security_handler_ = std::move(security_handler);
  security_changed_ = true;
}

CPDF_CryptoHandler* CPDF_Creator::GetCryptoHandler() {
  return security_handler_ ? security_handler_->GetCryptoHandler() : nullptr;
}
