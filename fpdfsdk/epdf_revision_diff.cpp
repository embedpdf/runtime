// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// The cross-revision object diff declared in public/epdf_signature.h.
//
// Three passes over two documents that share their bytes:
//   1. Touched set: the object numbers whose effective cross-reference
//      mapping differs, members of changed object streams, freed numbers,
//      and the trailer.
//   2. Values: both objects shallow-serialised (references never followed,
//      stream data replaced by length + SHA-256), so a rule engine can decide
//      key-level questions ("only /V changed", "the array was appended to")
//      without touching the PDF again.
//   3. Usage: a referrer index over each revision, built by walking every
//      reachable indirect object once, listing every inbound reference to
//      every object - so a rule that permits an object can prove it
//      inspected every use, and can walk any object up to the trailer.

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "core/fdrm/fx_crypt_sha.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_cross_ref_table.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_object_stream.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxcrt/unowned_ptr.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_revision_view.h"
#include "public/epdf_signature.h"

namespace {

constexpr size_t kValueCap = 1u << 20;  // bytes of serialisation per value
constexpr size_t kSerializeDepthCap = 32;

// ---------------------------------------------------------------------------
// Result storage.
// ---------------------------------------------------------------------------

struct RevisionSide {
  ByteString value;
  bool value_truncated = false;
  bool present = false;
  int gen = -1;
};

struct Edge {
  uint32_t parent;   // 0 = the trailer
  ByteString label;  // keys and indexes inside the parent, "/"-joined
};

using ReferrerIndex = std::map<uint32_t, std::vector<Edge>>;

struct DiffEntry {
  uint32_t obj_num = 0;
  int change = EPDF_DIFF_MODIFIED;
  int kind = EPDF_DIFF_OBJ_SCALAR;
  bool stream_data_changed = false;
  RevisionSide side[2];  // EPDF_DIFF_OLD, EPDF_DIFF_NEW
};

struct ObjectDiff {
  std::vector<DiffEntry> entries;
  ReferrerIndex referrers[2];  // EPDF_DIFF_OLD, EPDF_DIFF_NEW
};

ObjectDiff* DiffFromHandle(EPDF_OBJECT_DIFF diff) {
  return reinterpret_cast<ObjectDiff*>(const_cast<epdf_object_diff_t__*>(diff));
}

EPDF_OBJECT_DIFF HandleFromDiff(ObjectDiff* diff) {
  return reinterpret_cast<EPDF_OBJECT_DIFF>(diff);
}

const DiffEntry* GetEntry(EPDF_OBJECT_DIFF diff, int index) {
  const ObjectDiff* d = DiffFromHandle(diff);
  if (!d || index < 0 || index >= fxcrt::CollectionSize<int>(d->entries)) {
    return nullptr;
  }
  return &d->entries[index];
}

const RevisionSide* GetSide(EPDF_OBJECT_DIFF diff, int index, int which) {
  const DiffEntry* entry = GetEntry(diff, index);
  if (!entry || (which != EPDF_DIFF_OLD && which != EPDF_DIFF_NEW)) {
    return nullptr;
  }
  return &entry->side[which];
}

// ---------------------------------------------------------------------------
// Pass 1: the touched set.
// ---------------------------------------------------------------------------

using ObjectInfo = CPDF_CrossRefTable::ObjectInfo;
using ObjectType = CPDF_CrossRefTable::ObjectType;

bool IsLive(const ObjectInfo& info) {
  return info.type == ObjectType::kNormal || info.type == ObjectType::kCompressed;
}

bool SameMapping(const ObjectInfo& a, const ObjectInfo& b) {
  if (a.type != b.type || a.gennum != b.gennum) {
    return false;
  }
  if (a.type == ObjectType::kNormal) {
    return a.pos == b.pos;
  }
  if (a.type == ObjectType::kCompressed) {
    return a.archive.obj_num == b.archive.obj_num &&
           a.archive.obj_index == b.archive.obj_index;
  }
  return true;
}

// Members of the object stream |obj_num| holds in |view|, if it is one.
std::vector<uint32_t> ObjectStreamMembers(const epdf::RevisionView* view,
                                          uint32_t obj_num) {
  std::vector<uint32_t> members;
  RetainPtr<const CPDF_Object> obj = view->ParseObject(obj_num);
  RetainPtr<const CPDF_Stream> stream = obj ? ToStream(obj) : nullptr;
  if (!stream || stream->GetDict()->GetNameFor("Type") != "ObjStm") {
    return members;
  }
  std::unique_ptr<CPDF_ObjectStream> object_stream =
      CPDF_ObjectStream::Create(std::move(stream));
  if (!object_stream) {
    return members;
  }
  for (const auto& info : object_stream->object_info()) {
    members.push_back(info.obj_num);
  }
  return members;
}

// True when |older| really is a prefix revision of |newer|: newer's chain
// passes through the section older was closed by AND older's bytes are
// byte-for-byte the prefix of newer's file. Matching offsets alone would
// accept two unrelated files that happen to lay out their objects alike.
bool ShareByteHistory(CPDF_Parser* older, CPDF_Parser* newer) {
  if (older->GetDocumentSize() > newer->GetDocumentSize()) {
    return false;
  }
  const FX_FILESIZE older_last_xref = older->GetLastXRefOffset();
  if (older_last_xref <= 0) {
    return false;
  }
  bool chained = false;
  for (const CPDF_Parser::CrossRefSection& section :
       newer->GetCrossRefSections()) {
    if (section.offset == older_last_xref) {
      chained = true;
      break;
    }
  }
  if (!chained) {
    return false;
  }
  RetainPtr<IFX_SeekableReadStream> older_file = older->GetFileAccess();
  RetainPtr<IFX_SeekableReadStream> newer_file = newer->GetFileAccess();
  if (!older_file || !newer_file) {
    return false;
  }
  const FX_FILESIZE older_size = older_file->GetSize();
  if (older_size <= 0 || older_size > newer_file->GetSize()) {
    return false;
  }
  // Two clamps of one immutable stream (two prefixes of the same document's
  // bytes, the analyzer's case) are byte-identical over their common length
  // by construction: identity proves it, no compare needed. Streams that
  // merely name the same file are distinct objects and take the slow path.
  if (older_file->GetUnderlyingStream() == newer_file->GetUnderlyingStream()) {
    return true;
  }
  static constexpr size_t kChunk = 64 * 1024;
  DataVector<uint8_t> a(kChunk);
  DataVector<uint8_t> b(kChunk);
  for (FX_FILESIZE offset = 0; offset < older_size;) {
    const size_t step =
        static_cast<size_t>(std::min<FX_FILESIZE>(kChunk, older_size - offset));
    pdfium::span<uint8_t> a_span = pdfium::span(a).first(step);
    pdfium::span<uint8_t> b_span = pdfium::span(b).first(step);
    if (!older_file->ReadBlockAtOffset(a_span, offset) ||
        !newer_file->ReadBlockAtOffset(b_span, offset) ||
        memcmp(a_span.data(), b_span.data(), step) != 0) {
      return false;
    }
    offset += step;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Pass 2: values.
// ---------------------------------------------------------------------------

ByteString HexOf(pdfium::span<const uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  ByteString out;
  for (uint8_t b : bytes) {
    out += kHex[b >> 4];
    out += kHex[b & 0x0f];
  }
  return out;
}

ByteString StreamSignature(const CPDF_Stream* stream) {
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
  acc->LoadAllDataRaw();
  pdfium::span<const uint8_t> raw = acc->GetSpan();
  DataVector<uint8_t> digest = CRYPT_SHA256Generate(raw);
  return ByteString::Format("stream(%u,", static_cast<unsigned>(raw.size())) +
         HexOf(digest) + ")";
}

class Serializer {
 public:
  Serializer() = default;

  void Write(const CPDF_Object* obj, size_t depth) {
    if (truncated_ || !obj) {
      Append("null");
      return;
    }
    if (depth > kSerializeDepthCap) {
      truncated_ = true;
      return;
    }
    if (const CPDF_Reference* ref = obj->AsReference()) {
      Append(ByteString::Format("%u %u R", ref->GetRefObjNum(), 0u));
      return;
    }
    if (const CPDF_Stream* stream = obj->AsStream()) {
      Append(StreamSignature(stream));
      WriteDictionary(stream->GetDict().Get(), depth);
      return;
    }
    if (const CPDF_Dictionary* dict = obj->AsDictionary()) {
      WriteDictionary(dict, depth);
      return;
    }
    if (const CPDF_Array* array = obj->AsArray()) {
      Append("[");
      CPDF_ArrayLocker locker(array);
      bool first = true;
      for (const auto& item : locker) {
        if (!first) {
          Append(" ");
        }
        first = false;
        Write(item.Get(), depth + 1);
        if (truncated_) {
          return;
        }
      }
      Append("]");
      return;
    }
    // Scalars, canonically: names escaped, strings as literals (hex strings
    // included, so the same text serialises the same way), numbers and
    // booleans as their text, no writer-specific whitespace.
    switch (obj->GetType()) {
      case CPDF_Object::Type::kName:
        Append("/" + PDF_NameEncode(obj->GetString()));
        return;
      case CPDF_Object::Type::kString:
        Append(PDF_EncodeString(obj->GetString().AsStringView()));
        return;
      case CPDF_Object::Type::kNumber:
      case CPDF_Object::Type::kBoolean:
        Append(obj->GetString());
        return;
      default:
        Append("null");
        return;
    }
  }

  ByteString Take() { return std::move(out_); }
  bool truncated() const { return truncated_; }

 private:
  void WriteDictionary(const CPDF_Dictionary* dict, size_t depth) {
    Append("<<");
    if (dict) {
      // CPDF_Dictionary iterates its std::map: keys come out sorted, so the
      // serialisation is canonical regardless of the source order.
      CPDF_DictionaryLocker locker(dict);
      for (const auto& [key, value] : locker) {
        // Keys are escaped exactly like the writer does, so a key that
        // contains a delimiter or whitespace can never read like two keys.
        Append("/");
        Append(PDF_NameEncode(key));
        Append(" ");
        Write(value.Get(), depth + 1);
        if (truncated_) {
          return;
        }
      }
    }
    Append(">>");
  }

  void Append(const ByteString& text) {
    if (truncated_) {
      return;
    }
    if (out_.GetLength() + text.GetLength() > kValueCap) {
      truncated_ = true;
      return;
    }
    out_ += text;
  }

  ByteString out_;
  bool truncated_ = false;
};

void FillValue(const CPDF_Object* obj, RevisionSide* side) {
  Serializer serializer;
  serializer.Write(obj, 0);
  side->value_truncated = serializer.truncated();
  side->value = side->value_truncated ? ByteString() : serializer.Take();
}

int KindOf(const CPDF_Object* obj) {
  if (!obj) {
    return EPDF_DIFF_OBJ_SCALAR;
  }
  if (const CPDF_Stream* stream = obj->AsStream()) {
    const ByteString type = stream->GetDict()->GetNameFor("Type");
    if (type == "XRef") {
      return EPDF_DIFF_OBJ_XREF;
    }
    if (type == "ObjStm") {
      return EPDF_DIFF_OBJ_OBJSTM;
    }
    return EPDF_DIFF_OBJ_STREAM;
  }
  if (obj->IsDictionary()) {
    return EPDF_DIFF_OBJ_DICTIONARY;
  }
  if (obj->IsArray()) {
    return EPDF_DIFF_OBJ_ARRAY;
  }
  return EPDF_DIFF_OBJ_SCALAR;
}

// ---------------------------------------------------------------------------
// Pass 3: usage.
// ---------------------------------------------------------------------------

// Walks the whole revision once from the trailer, recording every inbound
// reference of every reachable object.
class ReferrerIndexBuilder {
 public:
  ReferrerIndexBuilder(const epdf::RevisionView* view, ReferrerIndex* out)
      : view_(view), out_(out) {}

  void Build(const CPDF_Dictionary* trailer) {
    std::vector<uint32_t> queue;
    std::set<uint32_t> seen;
    VisitDirect(trailer, 0, ByteString(), &queue, &seen);
    while (!queue.empty()) {
      const uint32_t obj_num = queue.back();
      queue.pop_back();
      RetainPtr<const CPDF_Object> obj = view_->ParseObject(obj_num);
      if (!obj) {
        continue;
      }
      if (const CPDF_Stream* stream = obj->AsStream()) {
        VisitDirect(stream->GetDict().Get(), obj_num, ByteString(), &queue,
                    &seen);
      } else {
        VisitDirect(obj.Get(), obj_num, ByteString(), &queue, &seen);
      }
    }
  }

 private:
  void VisitDirect(const CPDF_Object* obj,
                   uint32_t owner,
                   const ByteString& prefix,
                   std::vector<uint32_t>* queue,
                   std::set<uint32_t>* seen) {
    if (!obj) {
      return;
    }
    if (const CPDF_Reference* ref = obj->AsReference()) {
      const uint32_t target = ref->GetRefObjNum();
      if (target == 0) {
        return;
      }
      (*out_)[target].push_back({owner, prefix});
      if (seen->insert(target).second) {
        queue->push_back(target);
      }
      return;
    }
    if (const CPDF_Dictionary* dict = obj->AsDictionary()) {
      CPDF_DictionaryLocker locker(dict);
      for (const auto& [key, value] : locker) {
        // Keys are escaped (a "/" inside a key becomes "#2F"), so the label
        // separator stays unambiguous; array indexes are "[n]", which an
        // escaped key can never look like.
        VisitDirect(value.Get(), owner, Join(prefix, PDF_NameEncode(key)),
                    queue, seen);
      }
      return;
    }
    if (const CPDF_Array* array = obj->AsArray()) {
      CPDF_ArrayLocker locker(array);
      size_t index = 0;
      for (const auto& item : locker) {
        VisitDirect(item.Get(), owner,
                    Join(prefix, "[" + ByteString::FormatInteger(index) + "]"),
                    queue, seen);
        ++index;
      }
      return;
    }
    if (const CPDF_Stream* stream = obj->AsStream()) {
      VisitDirect(stream->GetDict().Get(), owner, prefix, queue, seen);
    }
  }

  static ByteString Join(const ByteString& prefix, const ByteString& part) {
    return prefix.IsEmpty() ? part : prefix + "/" + part;
  }

  UnownedPtr<const epdf::RevisionView> const view_;
  UnownedPtr<ReferrerIndex> const out_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

FPDF_EXPORT EPDF_OBJECT_DIFF FPDF_CALLCONV
EPDFDoc_CompareRevisions(FPDF_DOCUMENT older_document,
                         FPDF_DOCUMENT newer_document) {
  CPDF_Document* older = CPDFDocumentFromFPDFDocument(older_document);
  CPDF_Document* newer = CPDFDocumentFromFPDFDocument(newer_document);
  if (!older || !newer || older == newer) {
    return nullptr;
  }
  // Both sides are read from the bytes they were loaded from - for a layer,
  // base + ingested delta - never from a document's in-memory objects.
  epdf::RevisionView* older_view = epdf::RevisionView::For(older);
  epdf::RevisionView* newer_view = epdf::RevisionView::For(newer);
  if (!older_view || !newer_view) {
    return nullptr;
  }
  CPDF_Parser* older_parser = older_view->parser();
  CPDF_Parser* newer_parser = newer_view->parser();
  if (older_parser->xref_table_rebuilt() || newer_parser->xref_table_rebuilt() ||
      !ShareByteHistory(older_parser, newer_parser)) {
    return nullptr;
  }

  // Pass 1.
  const auto& older_info = older_parser->GetCrossRefTable()->objects_info();
  const auto& newer_info = newer_parser->GetCrossRefTable()->objects_info();
  std::map<uint32_t, int> touched;  // obj_num -> change
  for (const auto& [num, info] : newer_info) {
    if (num == 0) {
      continue;
    }
    const auto it = older_info.find(num);
    const bool old_live = it != older_info.end() && IsLive(it->second);
    const bool new_live = IsLive(info);
    if (!old_live && !new_live) {
      continue;
    }
    if (!old_live) {
      touched[num] = EPDF_DIFF_ADDED;
    } else if (!new_live) {
      touched[num] = EPDF_DIFF_FREED;
    } else if (!SameMapping(it->second, info)) {
      touched[num] = EPDF_DIFF_MODIFIED;
    }
  }
  for (const auto& [num, info] : older_info) {
    if (num != 0 && IsLive(info) && !newer_info.count(num)) {
      touched[num] = EPDF_DIFF_FREED;
    }
  }
  // A changed object stream drags its members in, in both revisions.
  std::vector<std::pair<uint32_t, int>> container_seeds(touched.begin(),
                                                        touched.end());
  for (const auto& [num, change] : container_seeds) {
    std::vector<uint32_t> members;
    if (change != EPDF_DIFF_FREED) {
      members = ObjectStreamMembers(newer_view, num);
    }
    if (change != EPDF_DIFF_ADDED) {
      std::vector<uint32_t> old_members =
          ObjectStreamMembers(older_view, num);
      members.insert(members.end(), old_members.begin(), old_members.end());
    }
    for (uint32_t member : members) {
      if (touched.count(member)) {
        continue;
      }
      const auto old_it = older_info.find(member);
      const auto new_it = newer_info.find(member);
      const bool old_live = old_it != older_info.end() && IsLive(old_it->second);
      const bool new_live = new_it != newer_info.end() && IsLive(new_it->second);
      if (old_live && new_live) {
        touched[member] = EPDF_DIFF_MODIFIED;
      } else if (new_live) {
        touched[member] = EPDF_DIFF_ADDED;
      } else if (old_live) {
        touched[member] = EPDF_DIFF_FREED;
      }
    }
  }

  auto result = std::make_unique<ObjectDiff>();

  // Pass 2 + kind. Objects that serialise identically in both revisions are
  // still reported (the mapping changed), so a rule engine can apply its
  // identical-rewrite rule with evidence.
  RetainPtr<CPDF_Dictionary> older_trailer = older_parser->GetCombinedTrailer();
  RetainPtr<CPDF_Dictionary> newer_trailer = newer_parser->GetCombinedTrailer();
  ReferrerIndexBuilder(older_view, &result->referrers[EPDF_DIFF_OLD])
      .Build(older_trailer.Get());
  ReferrerIndexBuilder(newer_view, &result->referrers[EPDF_DIFF_NEW])
      .Build(newer_trailer.Get());

  for (const auto& [num, change] : touched) {
    DiffEntry entry;
    entry.obj_num = num;
    entry.change = change;
    RetainPtr<const CPDF_Object> old_obj;
    RetainPtr<const CPDF_Object> new_obj;
    if (change != EPDF_DIFF_ADDED) {
      old_obj = older_view->ParseObject(num);
    }
    if (change != EPDF_DIFF_FREED) {
      new_obj = newer_view->ParseObject(num);
    }
    // Absent objects (a live mapping that does not parse) count as null.
    entry.kind = KindOf(new_obj ? new_obj.Get() : old_obj.Get());
    if (change != EPDF_DIFF_ADDED) {
      RevisionSide& side = entry.side[EPDF_DIFF_OLD];
      side.present = true;
      const auto it = older_info.find(num);
      side.gen = it != older_info.end() ? it->second.gennum : -1;
      FillValue(old_obj.Get(), &side);
    }
    if (change != EPDF_DIFF_FREED) {
      RevisionSide& side = entry.side[EPDF_DIFF_NEW];
      side.present = true;
      const auto it = newer_info.find(num);
      side.gen = it != newer_info.end() ? it->second.gennum : -1;
      FillValue(new_obj.Get(), &side);
    }
    if (old_obj && new_obj && old_obj->IsStream() && new_obj->IsStream()) {
      entry.stream_data_changed = StreamSignature(old_obj->AsStream()) !=
                                  StreamSignature(new_obj->AsStream());
    }
    result->entries.push_back(std::move(entry));
  }

  // The trailer, last, only when it differs.
  {
    DiffEntry entry;
    entry.obj_num = 0;
    entry.change = EPDF_DIFF_MODIFIED;
    entry.kind = EPDF_DIFF_OBJ_TRAILER;
    FillValue(older_trailer.Get(), &entry.side[EPDF_DIFF_OLD]);
    FillValue(newer_trailer.Get(), &entry.side[EPDF_DIFF_NEW]);
    entry.side[EPDF_DIFF_OLD].present = true;
    entry.side[EPDF_DIFF_NEW].present = true;
    if (entry.side[EPDF_DIFF_OLD].value != entry.side[EPDF_DIFF_NEW].value ||
        entry.side[EPDF_DIFF_OLD].value_truncated ||
        entry.side[EPDF_DIFF_NEW].value_truncated) {
      result->entries.push_back(std::move(entry));
    }
  }
  return HandleFromDiff(result.release());
}

FPDF_EXPORT void FPDF_CALLCONV EPDFObjectDiff_Close(EPDF_OBJECT_DIFF diff) {
  delete DiffFromHandle(diff);
}

FPDF_EXPORT int FPDF_CALLCONV EPDFObjectDiff_GetCount(EPDF_OBJECT_DIFF diff) {
  const ObjectDiff* d = DiffFromHandle(diff);
  return d ? fxcrt::CollectionSize<int>(d->entries) : -1;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFObjectDiff_GetEntry(EPDF_OBJECT_DIFF diff,
                        int index,
                        unsigned int* out_obj_num,
                        int* out_change,
                        int* out_kind,
                        int* out_old_gen,
                        int* out_new_gen,
                        FPDF_BOOL* out_stream_data_changed) {
  const DiffEntry* entry = GetEntry(diff, index);
  if (!entry) {
    return false;
  }
  if (out_obj_num) {
    *out_obj_num = entry->obj_num;
  }
  if (out_change) {
    *out_change = entry->change;
  }
  if (out_kind) {
    *out_kind = entry->kind;
  }
  if (out_old_gen) {
    *out_old_gen = entry->side[EPDF_DIFF_OLD].gen;
  }
  if (out_new_gen) {
    *out_new_gen = entry->side[EPDF_DIFF_NEW].gen;
  }
  if (out_stream_data_changed) {
    *out_stream_data_changed = entry->stream_data_changed;
  }
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFObjectDiff_GetValue(EPDF_OBJECT_DIFF diff,
                        int index,
                        int which,
                        char* buffer,
                        unsigned long buflen,
                        FPDF_BOOL* out_truncated) {
  if (out_truncated) {
    *out_truncated = false;
  }
  const RevisionSide* side = GetSide(diff, index, which);
  if (!side || !side->present) {
    return 0;
  }
  if (out_truncated) {
    *out_truncated = side->value_truncated;
  }
  if (side->value_truncated) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      side->value, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFObjectDiff_GetReferrerCount(EPDF_OBJECT_DIFF diff,
                                int which,
                                unsigned int obj_num) {
  const ObjectDiff* d = DiffFromHandle(diff);
  if (!d || (which != EPDF_DIFF_OLD && which != EPDF_DIFF_NEW)) {
    return -1;
  }
  const auto it = d->referrers[which].find(obj_num);
  return it == d->referrers[which].end()
             ? 0
             : fxcrt::CollectionSize<int>(it->second);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFObjectDiff_GetReferrer(EPDF_OBJECT_DIFF diff,
                           int which,
                           unsigned int obj_num,
                           int referrer_index,
                           unsigned int* out_parent_obj_num,
                           char* buffer,
                           unsigned long buflen) {
  if (out_parent_obj_num) {
    *out_parent_obj_num = 0;
  }
  const ObjectDiff* d = DiffFromHandle(diff);
  if (!d || (which != EPDF_DIFF_OLD && which != EPDF_DIFF_NEW)) {
    return 0;
  }
  const auto it = d->referrers[which].find(obj_num);
  if (it == d->referrers[which].end() || referrer_index < 0 ||
      referrer_index >= fxcrt::CollectionSize<int>(it->second)) {
    return 0;
  }
  const Edge& edge = it->second[referrer_index];
  if (out_parent_obj_num) {
    *out_parent_obj_num = edge.parent;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      edge.label, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}
