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
#include <array>
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
// The reachability walk records one edge per inbound reference of every
// reachable object; past these the index is reported incomplete rather than
// letting one document own the process.
constexpr size_t kWalkObjectCap = 1u << 20;
constexpr size_t kWalkEdgeCap = 1u << 22;

// ---------------------------------------------------------------------------
// Result storage.
// ---------------------------------------------------------------------------

struct RevisionSide {
  ByteString value;
  bool value_truncated = false;
  bool present = false;
  int gen = -1;
  int read = EPDF_DIFF_READ_ABSENT;
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

struct RevisionHealth {
  bool sparse_xref = false;
  unsigned int bare_references = 0;
  bool referrers_complete = true;
};

struct ObjectDiff {
  std::vector<DiffEntry> entries;
  ReferrerIndex referrers[2];  // EPDF_DIFF_OLD, EPDF_DIFF_NEW
  RevisionHealth health[2];
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

// The raw stream data's length and SHA-256, hashed through the stream's own
// block reader 64 KiB at a time: the data is never copied whole, whether it
// lives in the file or in memory. Nullopt when a block cannot be read - that
// is missing evidence, not an empty stream.
std::optional<ByteString> StreamSignature(const CPDF_Stream* stream) {
  static constexpr size_t kChunk = 64 * 1024;
  const size_t size = stream->GetRawSize();
  CRYPT_sha2_context context;
  CRYPT_SHA256Start(&context);
  if (size > 0) {
    DataVector<uint8_t> buffer(std::min(kChunk, size));
    for (size_t offset = 0; offset < size;) {
      const size_t count = std::min(kChunk, size - offset);
      pdfium::span<uint8_t> chunk = pdfium::span(buffer).first(count);
      if (!stream->ReadRawBlock(chunk, static_cast<FX_FILESIZE>(offset))) {
        return std::nullopt;
      }
      CRYPT_SHA256Update(&context, chunk);
      offset += count;
    }
  }
  std::array<uint8_t, 32> digest;
  CRYPT_SHA256Finish(&context, digest);
  return ByteString::Format("stream(%u,", static_cast<unsigned>(size)) +
         HexOf(digest) + ")";
}

class Serializer {
 public:
  // |stream_signature| is the entry's own stream data signature, computed
  // once by the caller and shared with the stream_data_changed decision; a
  // stream is always an indirect object, so it is only ever the root here.
  explicit Serializer(const std::optional<ByteString>* stream_signature)
      : stream_signature_(stream_signature) {}

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
      // PDFium keeps no generation on a reference (the object's own
      // generation is reported per side); every reference reads "n 0 R".
      Append(ByteString::Format("%u %u R", ref->GetRefObjNum(), 0u));
      return;
    }
    if (const CPDF_Stream* stream = obj->AsStream()) {
      if (!stream_signature_ || !stream_signature_->has_value()) {
        // Unreadable data: the value carries no evidence at all.
        unreadable_ = true;
        Append("null");
        return;
      }
      Append(stream_signature_->value());
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
  bool unreadable() const { return unreadable_; }

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
  bool unreadable_ = false;
  UnownedPtr<const std::optional<ByteString>> const stream_signature_;
};

// Fills a present side. A live mapping whose bytes do not parse (|obj| null)
// or whose stream data cannot be read is READ_FAILED: its value is "null"
// on the wire, but the read status says that this null is not evidence.
void FillValue(const CPDF_Object* obj,
               const std::optional<ByteString>* stream_signature,
               RevisionSide* side) {
  side->present = true;
  Serializer serializer(stream_signature);
  serializer.Write(obj, 0);
  side->value_truncated = serializer.truncated();
  side->value = side->value_truncated ? ByteString() : serializer.Take();
  side->read = (!obj || serializer.unreadable()) ? EPDF_DIFF_READ_FAILED
                                                  : EPDF_DIFF_READ_OK;
}

// ---------------------------------------------------------------------------
// Revision health: the cross-reference table's coverage.
// ---------------------------------------------------------------------------

// The object-number ranges a revision's cross-reference sections cover,
// read from the sections themselves (the parser's merged table drops free
// entries with generation 0, so it cannot tell "listed as free" from "not
// listed at all" - and that difference is exactly what matters here).
struct NumberRange {
  uint32_t start = 0;
  uint32_t count = 0;
};

bool ReadAt(IFX_SeekableReadStream* file,
            FX_FILESIZE pos,
            pdfium::span<uint8_t> out,
            size_t* out_read) {
  const FX_FILESIZE size = file->GetSize();
  if (pos < 0 || pos >= size) {
    return false;
  }
  const size_t count = static_cast<size_t>(
      std::min<FX_FILESIZE>(static_cast<FX_FILESIZE>(out.size()), size - pos));
  if (!file->ReadBlockAtOffset(out.first(count), pos)) {
    return false;
  }
  *out_read = count;
  return true;
}

bool IsPdfSpace(uint8_t c) {
  return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' || c == 0;
}

// Parses "start count" subsection headers of a classic table at |pos|
// (just past the "xref" keyword), skipping each subsection's entries.
bool CoverageOfClassicTable(IFX_SeekableReadStream* file,
                            FX_FILESIZE pos,
                            std::vector<NumberRange>* ranges) {
  std::array<uint8_t, 64> buf;
  for (int guard = 0; guard < 100000; ++guard) {
    size_t got = 0;
    if (!ReadAt(file, pos, pdfium::span(buf), &got)) {
      return false;
    }
    size_t i = 0;
    while (i < got && IsPdfSpace(buf[i])) {
      ++i;
    }
    if (i >= got) {
      return false;
    }
    if (buf[i] == 't') {
      return true;  // "trailer": the table is complete
    }
    if (!isdigit(buf[i])) {
      return false;
    }
    uint64_t start = 0;
    while (i < got && isdigit(buf[i])) {
      start = start * 10 + (buf[i] - '0');
      if (start > CPDF_Parser::kMaxObjectNumber) {
        return false;
      }
      ++i;
    }
    while (i < got && (buf[i] == ' ' || buf[i] == '\t')) {
      ++i;
    }
    if (i >= got || !isdigit(buf[i])) {
      return false;
    }
    uint64_t count = 0;
    while (i < got && isdigit(buf[i])) {
      count = count * 10 + (buf[i] - '0');
      if (count > CPDF_Parser::kMaxObjectNumber) {
        return false;
      }
      ++i;
    }
    // The header's line ending, then |count| entries of exactly 20 bytes.
    while (i < got && IsPdfSpace(buf[i])) {
      ++i;
    }
    if (i >= got) {
      return false;
    }
    ranges->push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(count)});
    pos += static_cast<FX_FILESIZE>(i) + static_cast<FX_FILESIZE>(count) * 20;
  }
  return false;
}

// The /Index of a cross-reference stream (default [0 /Size]).
bool CoverageOfXrefStream(const epdf::RevisionView* view,
                          IFX_SeekableReadStream* file,
                          FX_FILESIZE pos,
                          std::vector<NumberRange>* ranges) {
  std::array<uint8_t, 32> buf;
  size_t got = 0;
  if (!ReadAt(file, pos, pdfium::span(buf), &got)) {
    return false;
  }
  size_t i = 0;
  uint64_t objnum = 0;
  while (i < got && isdigit(buf[i])) {
    objnum = objnum * 10 + (buf[i] - '0');
    if (objnum > CPDF_Parser::kMaxObjectNumber) {
      return false;
    }
    ++i;
  }
  if (i == 0) {
    return false;
  }
  RetainPtr<const CPDF_Object> obj = view->ParseObject(static_cast<uint32_t>(objnum));
  RetainPtr<const CPDF_Stream> stream = obj ? ToStream(obj) : nullptr;
  if (!stream || stream->GetDict()->GetNameFor("Type") != "XRef") {
    return false;
  }
  RetainPtr<const CPDF_Array> index = stream->GetDict()->GetArrayFor("Index");
  if (!index) {
    const int size = stream->GetDict()->GetIntegerFor("Size");
    if (size > 0) {
      ranges->push_back({0, static_cast<uint32_t>(size)});
    }
    return true;
  }
  for (size_t k = 0; k + 1 < index->size(); k += 2) {
    const int start = index->GetIntegerAt(k);
    const int count = index->GetIntegerAt(k + 1);
    if (start < 0 || count < 0) {
      return false;
    }
    ranges->push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(count)});
  }
  return true;
}

// The document's ORIGINAL cross-reference section (the oldest in the chain)
// leaves some object number below the highest number it lists without an
// entry of any kind - not "n", not "f". Later update sections are sparse
// by nature and tolerated (pyHanko's updates skip numbers; corpus v3/85 is
// valid), as is an overstated /Size (v3/55). A first section with holes is
// legal to a lenient reader, but Acrobat's update analysis treats the
// document as corrupted once any revision follows the signed one (the
// 2026-09-13 ebook, rounds 3-5). Unknown (a section that cannot be read)
// counts as not sparse: this is a reason to withhold a verdict, and it must
// be established, not guessed.
bool HasSparseXref(const CPDF_Parser* parser, const epdf::RevisionView* view) {
  if (!parser || !view) {
    return false;
  }
  RetainPtr<IFX_SeekableReadStream> file = parser->GetFileAccess();
  if (!file) {
    return false;
  }
  const FX_FILESIZE header = parser->GetFileHeaderOffset();
  const std::vector<CPDF_Parser::CrossRefSection>& sections =
      parser->GetCrossRefSections();
  if (sections.empty()) {
    return false;
  }
  std::vector<NumberRange> ranges;
  {
    const CPDF_Parser::CrossRefSection& section = sections.front();
    std::array<uint8_t, 8> head;
    size_t got = 0;
    if (!ReadAt(file.Get(), section.offset + header, pdfium::span(head), &got) ||
        got < 4) {
      return false;
    }
    const bool classic = memcmp(head.data(), "xref", 4) == 0;
    const bool ok =
        classic ? CoverageOfClassicTable(file.Get(), section.offset + header + 4,
                                         &ranges)
                : CoverageOfXrefStream(view, file.Get(), section.offset + header,
                                       &ranges);
    if (!ok) {
      return false;
    }
    if (section.hybrid_stream_offset != 0 &&
        !CoverageOfXrefStream(view, file.Get(),
                              section.hybrid_stream_offset + header, &ranges)) {
      return false;
    }
  }
  if (ranges.empty()) {
    return false;
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const NumberRange& a, const NumberRange& b) {
              return a.start < b.start;
            });
  uint64_t next = 0;  // the first number not yet covered
  for (const NumberRange& r : ranges) {
    if (r.start > next) {
      return true;  // a hole below a later listed number
    }
    next = std::max<uint64_t>(next, static_cast<uint64_t>(r.start) + r.count);
  }
  return false;
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
  ReferrerIndexBuilder(const epdf::RevisionView* view,
                       ReferrerIndex* out,
                       RevisionHealth* health)
      : view_(view), out_(out), health_(health) {}

  void Build(const CPDF_Dictionary* trailer) {
    std::vector<uint32_t> queue;
    std::set<uint32_t> seen;
    VisitDirect(trailer, 0, ByteString(), &queue, &seen);
    while (!queue.empty()) {
      if (objects_ >= kWalkObjectCap || edges_ >= kWalkEdgeCap) {
        health_->referrers_complete = false;
        return;
      }
      const uint32_t obj_num = queue.back();
      queue.pop_back();
      ++objects_;
      RetainPtr<const CPDF_Object> obj = view_->ParseObject(obj_num);
      if (!obj) {
        continue;
      }
      if (obj->IsReference()) {
        // An indirect object whose whole body is a reference: reachable,
        // so it counts against the revision (Acrobat: corrupted once any
        // revision follows). Its target is still walked.
        ++health_->bare_references;
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
      ++edges_;
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
                    Join(prefix, ByteString::Format("[%zu]", index)),
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
  UnownedPtr<RevisionHealth> const health_;
  size_t objects_ = 0;
  size_t edges_ = 0;
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
  ReferrerIndexBuilder(older_view, &result->referrers[EPDF_DIFF_OLD],
                       &result->health[EPDF_DIFF_OLD])
      .Build(older_trailer.Get());
  ReferrerIndexBuilder(newer_view, &result->referrers[EPDF_DIFF_NEW],
                       &result->health[EPDF_DIFF_NEW])
      .Build(newer_trailer.Get());
  result->health[EPDF_DIFF_OLD].sparse_xref =
      HasSparseXref(older_parser, older_view);
  result->health[EPDF_DIFF_NEW].sparse_xref =
      HasSparseXref(newer_parser, newer_view);

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
    // A live mapping that does not parse serialises as null but reads as
    // FAILED: the side is present, its value is not evidence.
    entry.kind = KindOf(new_obj ? new_obj.Get() : old_obj.Get());
    // Each side's stream data is hashed once; the value and the
    // stream_data_changed flag share the result.
    std::optional<ByteString> old_signature;
    std::optional<ByteString> new_signature;
    if (old_obj && old_obj->IsStream()) {
      old_signature = StreamSignature(old_obj->AsStream());
    }
    if (new_obj && new_obj->IsStream()) {
      new_signature = StreamSignature(new_obj->AsStream());
    }
    if (change != EPDF_DIFF_ADDED) {
      RevisionSide& side = entry.side[EPDF_DIFF_OLD];
      const auto it = older_info.find(num);
      side.gen = it != older_info.end() ? it->second.gennum : -1;
      FillValue(old_obj.Get(), &old_signature, &side);
    }
    if (change != EPDF_DIFF_FREED) {
      RevisionSide& side = entry.side[EPDF_DIFF_NEW];
      const auto it = newer_info.find(num);
      side.gen = it != newer_info.end() ? it->second.gennum : -1;
      FillValue(new_obj.Get(), &new_signature, &side);
    }
    if (old_obj && new_obj && old_obj->IsStream() && new_obj->IsStream()) {
      // Unreadable data on either side is reported through the read
      // status, never as "unchanged".
      entry.stream_data_changed = !old_signature.has_value() ||
                                  !new_signature.has_value() ||
                                  old_signature.value() != new_signature.value();
    }
    result->entries.push_back(std::move(entry));
  }

  // The trailer, last, only when it differs.
  {
    DiffEntry entry;
    entry.obj_num = 0;
    entry.change = EPDF_DIFF_MODIFIED;
    entry.kind = EPDF_DIFF_OBJ_TRAILER;
    FillValue(older_trailer.Get(), nullptr, &entry.side[EPDF_DIFF_OLD]);
    FillValue(newer_trailer.Get(), nullptr, &entry.side[EPDF_DIFF_NEW]);
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

FPDF_EXPORT int FPDF_CALLCONV EPDFObjectDiff_GetReadStatus(EPDF_OBJECT_DIFF diff,
                                                           int index,
                                                           int which) {
  const RevisionSide* side = GetSide(diff, index, which);
  return side ? side->read : -1;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFObjectDiff_GetRevisionHealth(EPDF_OBJECT_DIFF diff,
                                 int which,
                                 FPDF_BOOL* out_sparse_xref,
                                 unsigned int* out_bare_reference_count,
                                 FPDF_BOOL* out_referrers_complete) {
  const ObjectDiff* d = DiffFromHandle(diff);
  if (!d || (which != EPDF_DIFF_OLD && which != EPDF_DIFF_NEW)) {
    return false;
  }
  const RevisionHealth& health = d->health[which];
  if (out_sparse_xref) {
    *out_sparse_xref = health.sparse_xref;
  }
  if (out_bare_reference_count) {
    *out_bare_reference_count = health.bare_references;
  }
  if (out_referrers_complete) {
    *out_referrers_complete = health.referrers_complete;
  }
  return true;
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
