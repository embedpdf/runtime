// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_capture.h"

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "core/fpdfapi/edit/cpdf_stringarchivestream.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_base_document.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_syntax_parser.h"
#include "core/fpdfdoc/cpdf_bookmarktree.h"
#include "core/fpdfdoc/cpdf_nametree.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/cfx_read_only_span_stream.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_form_helpers.h"
#include "fpdfsdk/epdf_named_pages_helpers.h"
#include "fpdfsdk/epdf_object_helpers.h"
#include "fpdfsdk/epdf_outline_helpers.h"
#include "fpdfsdk/epdf_owned_buffer.h"

namespace {

constexpr char kHeader[] = "%EPDF-CAPTURE 1\n";

// How deep a field's ancestors go; matches EPDFForm_DeleteField().
constexpr int kMaxFieldDepth = 32;

// Keys a capture doesn't follow: each leads out of what it holds, to the
// page, a parent, the annotation a reply answers, or an optional content
// group.
bool IsBoundaryKey(ByteStringView key) {
  return key == "P" || key == "Parent" || key == "IRT" || key == "OC";
}

// Keys of an outline item a capture doesn't follow: its links to its
// siblings and children (a capture adds the items it holds one by one) and
// its structure element.
bool IsOutlineLinkKey(ByteStringView key) {
  return key == "Prev" || key == "Next" || key == "First" || key == "Last" ||
         key == "SE";
}

// The keys a page inherits from the page tree above it (ISO 32000-2
// 7.7.3.4).
constexpr const char* kInheritableKeys[] = {"Resources", "MediaBox", "CropBox",
                                            "Rotate"};

bool IsInheritableKey(ByteStringView key) {
  return std::ranges::any_of(kInheritableKeys, [key](const char* inheritable) {
    return key == inheritable;
  });
}

// The value the nearest /Pages node above |page| gives the inheritable |key|,
// as that node holds it (a reference stays one), or null when none does.
RetainPtr<const CPDF_Object> InheritedValueOf(const CPDF_Dictionary* page,
                                              const char* key) {
  std::set<const CPDF_Dictionary*> seen;
  for (RetainPtr<const CPDF_Dictionary> node = page->GetDictFor("Parent");
       node && seen.insert(node.Get()).second;
       node = node->GetDictFor("Parent")) {
    if (RetainPtr<const CPDF_Object> value = node->GetObjectFor(key)) {
      return value;
    }
  }
  return nullptr;
}

// |value| as PDF syntax, which tells two values apart: a reference reads as
// its object number, anything else as what it holds.
DataVector<uint8_t> SyntaxOf(const CPDF_Object* value) {
  fxcrt::ostringstream text;
  CPDF_StringArchiveStream archive(&text);
  if (!value || !value->WriteTo(&archive, nullptr)) {
    return {};
  }
  const auto bytes = text.str();
  return DataVector<uint8_t>(bytes.begin(), bytes.end());
}

// Whether |dict| can be an embedded file's file specification: it says so,
// or it holds the file (/EF).
bool IsFileSpec(const CPDF_Dictionary* dict) {
  return dict &&
         (dict->GetNameFor("Type") == "Filespec" || dict->KeyExist("EF"));
}

// Objects a capture never holds: the catalog, the page tree and its pages,
// save the page a page capture is of (CaptureWriter::AddPage()).
bool IsBoundaryObject(const CPDF_Object* object) {
  RetainPtr<const CPDF_Dictionary> dict = object->GetDict();
  if (!dict) {
    return false;
  }
  const ByteString type = dict->GetNameFor("Type");
  return type == "Catalog" || type == "Pages" || type == "Page";
}

// The layer under |document|, or null when it is not a layer.
CPDF_LayerDocument* LayerOf(FPDF_DOCUMENT document) {
  return CPDF_LayerDocument::FromDocument(
      CPDFDocumentFromFPDFDocument(document));
}

ByteString HexOf(const std::array<uint8_t, 32>& sha256) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  ByteString hex;
  for (uint8_t byte : sha256) {
    hex += kHexDigits[byte >> 4];
    hex += kHexDigits[byte & 0xf];
  }
  return hex;
}

// A capture's manifest: its |kind|, and the file it was taken on (Base: the
// SHA-256 of the layer's base, in hex) when the layer knows it already, so
// taking a capture never costs hashing the file. A host that keeps captures
// beyond a session supplies the hash (EPDF_SetBaseDocumentSha256()).
RetainPtr<CPDF_Dictionary> NewManifest(CPDF_LayerDocument* layer,
                                       const char* kind) {
  auto manifest = pdfium::MakeRetain<CPDF_Dictionary>();
  manifest->SetNewFor<CPDF_Name>("Kind", kind);
  if (const std::array<uint8_t, 32>* sha256 =
          layer->GetBaseDocument()->GetKnownRawBaseSha256()) {
    manifest->SetNewFor<CPDF_String>("Base", HexOf(*sha256));
  }
  return manifest;
}

// Whether |manifest| comes from |layer|'s file, as far as it says: one that
// names a file comes back only into a layer of that file.
bool IsOfFile(const CPDF_Dictionary* manifest, CPDF_LayerDocument* layer) {
  const ByteString base = manifest->GetByteStringFor("Base");
  return base.IsEmpty() ||
         base == HexOf(layer->GetBaseDocument()->GetRawBaseSha256());
}

// The object numbers |array| refers to, in order (0 for a direct entry).
std::vector<uint32_t> RefsIn(const CPDF_Array* array) {
  std::vector<uint32_t> refs;
  for (size_t i = 0; array && i < array->size(); ++i) {
    RetainPtr<const CPDF_Object> entry = array->GetObjectAt(i);
    const CPDF_Reference* ref = entry ? entry->AsReference() : nullptr;
    refs.push_back(ref ? ref->GetRefObjNum() : 0);
  }
  return refs;
}

// Where |objnum| is in |array|, or nullopt.
std::optional<size_t> PositionIn(const CPDF_Array* array, uint32_t objnum) {
  const std::vector<uint32_t> refs = RefsIn(array);
  auto it = std::ranges::find(refs, objnum);
  if (it == refs.end()) {
    return std::nullopt;
  }
  return static_cast<size_t>(it - refs.begin());
}

// Puts a reference to |objnum| into |array| at |index| (at the end when the
// array is shorter), unless it is there already.
void InsertReference(CPDF_Document* doc,
                     CPDF_Array* array,
                     uint32_t objnum,
                     size_t index) {
  if (PositionIn(array, objnum)) {
    return;
  }
  array->InsertNewAt<CPDF_Reference>(std::min(index, array->size()), doc,
                                     objnum);
}

// How a capture writes a stream: its bytes as stored, so it comes back byte
// for byte; or as it reads, so a save that only encodes it again changes
// nothing (the canonical bytes a guard compares).
enum class StreamBytes { kAsStored, kAsRead };

// Collects the layer's versions of what a capture's roots reach, and writes
// the capture.
class CaptureWriter {
 public:
  explicit CaptureWriter(CPDF_LayerDocument* layer,
                         StreamBytes stream_bytes = StreamBytes::kAsStored)
      : layer_(layer), stream_bytes_(stream_bytes) {}

  // Captures the layer's version of |objnum|, if it holds one, and what the
  // object reaches.
  void AddObject(uint32_t objnum) {
    Enqueue(objnum);
    Drain();
  }

  // Captures what |value|, a value the manifest holds, reaches.
  void AddReachOf(const CPDF_Object* value) {
    CollectReferences(value);
    Drain();
  }

  // Captures the outline item |objnum| as AddObject() does, without following
  // its outline links: the caller adds the items it means one by one.
  void AddOutlineItem(uint32_t objnum) {
    outline_items_.insert(objnum);
    AddObject(objnum);
  }

  // Captures the page |objnum| as AddObject() does: a page is a boundary
  // object, but not the page the capture is of.
  void AddPage(uint32_t objnum) {
    pages_.insert(objnum);
    AddObject(objnum);
  }

  // The header, |manifest|, then every captured object in number order.
  DataVector<uint8_t> Write(const CPDF_Dictionary* manifest) const {
    fxcrt::ostringstream text;
    CPDF_StringArchiveStream archive(&text);
    bool ok = archive.WriteString(kHeader) &&
              manifest->WriteTo(&archive, nullptr) && archive.WriteString("\n");
    for (const auto& [objnum, object] : objects_) {
      ok = ok && archive.WriteDWord(objnum) && archive.WriteString(" ") &&
           archive.WriteDWord(object->GetGenNum()) &&
           archive.WriteString(" obj\n") &&
           WriteBody(&archive, object.Get(), stream_bytes_) &&
           archive.WriteString("\nendobj\n");
    }
    if (!ok) {
      return {};
    }
    const auto bytes = text.str();
    return DataVector<uint8_t>(bytes.begin(), bytes.end());
  }

 private:
  void Enqueue(uint32_t objnum) {
    if (objnum && visited_.insert(objnum).second) {
      pending_.push_back(objnum);
    }
  }

  // Every reference |value| holds directly, past boundary keys.
  void CollectReferences(const CPDF_Object* value) {
    if (!value) {
      return;
    }
    if (const CPDF_Reference* ref = value->AsReference()) {
      Enqueue(ref->GetRefObjNum());
      return;
    }
    if (const CPDF_Array* array = value->AsArray()) {
      for (size_t i = 0; i < array->size(); ++i) {
        CollectReferences(array->GetObjectAt(i).Get());
      }
      return;
    }
    RetainPtr<const CPDF_Dictionary> dict =
        value->IsStream() ? value->GetDict()
                          : pdfium::WrapRetain(value->AsDictionary());
    if (!dict) {
      return;
    }
    CPDF_DictionaryLocker locker(dict);
    for (const auto& [key, entry] : locker) {
      if (!IsBoundaryKey(key.AsStringView())) {
        CollectReferences(entry.Get());
      }
    }
  }

  // Follows every reference found, through the uploaded file's objects too
  // (one of them can reach an object the layer edited), keeping only the
  // layer's own versions.
  void Drain() {
    CPDF_DocumentViewScope view(layer_);
    while (!pending_.empty()) {
      const uint32_t objnum = pending_.front();
      pending_.pop_front();
      RetainPtr<const CPDF_Object> object =
          layer_->GetOrParseIndirectObject(objnum);
      if (!object ||
          (IsBoundaryObject(object.Get()) && !pages_.contains(objnum))) {
        continue;
      }
      if (RetainPtr<CPDF_Object> version = layer_->FindLayerVersion(objnum)) {
        objects_[objnum] = std::move(version);
      }
      if (outline_items_.contains(objnum)) {
        CollectItemReferences(object->AsDictionary());
      } else {
        CollectReferences(object.Get());
      }
    }
  }

  // What an outline item's own keys reach, past its outline links. Only its
  // own: an action it holds directly follows its /Next as any action does.
  void CollectItemReferences(const CPDF_Dictionary* item) {
    if (!item) {
      return;
    }
    CPDF_DictionaryLocker locker(item);
    for (const auto& [key, entry] : locker) {
      if (!IsBoundaryKey(key.AsStringView()) &&
          !IsOutlineLinkKey(key.AsStringView())) {
        CollectReferences(entry.Get());
      }
    }
  }

  // An object's body. A stream is written with its bytes as stored, never
  // encoded again, so it comes back byte for byte; or, as it reads, its
  // decoded bytes without the keys that say how they were stored.
  static bool WriteBody(IFX_ArchiveStream* archive,
                        const CPDF_Object* object,
                        StreamBytes stream_bytes) {
    const CPDF_Stream* stream = object->AsStream();
    if (!stream) {
      return object->WriteTo(archive, nullptr);
    }
    RetainPtr<CPDF_Dictionary> dict = ToDictionary(stream->GetDict()->Clone());
    DataVector<uint8_t> raw;
    if (stream_bytes == StreamBytes::kAsRead) {
      auto read =
          pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
      read->LoadAllDataFiltered();
      raw = read->DetachData();
      for (const char* key : {"Filter", "DecodeParms", "DL"}) {
        dict->RemoveFor(key);
      }
    } else if (stream->IsMemoryBased()) {
      pdfium::span<const uint8_t> data = stream->GetInMemoryRawData();
      raw.assign(data.begin(), data.end());
    } else {
      raw = stream->ReadAllRawData();
    }
    dict->SetNewFor<CPDF_Number>("Length",
                                 pdfium::checked_cast<int>(raw.size()));
    return dict->WriteTo(archive, nullptr) &&
           archive->WriteString("stream\r\n") && archive->WriteBlock(raw) &&
           archive->WriteString("\r\nendstream");
  }

  UnownedPtr<CPDF_LayerDocument> const layer_;
  const StreamBytes stream_bytes_;
  std::set<uint32_t> outline_items_;
  std::set<uint32_t> pages_;
  std::set<uint32_t> visited_;
  std::deque<uint32_t> pending_;
  std::map<uint32_t, RetainPtr<CPDF_Object>> objects_;
};

// A capture read back: its manifest and its objects, by number.
struct Capture {
  RetainPtr<CPDF_Dictionary> manifest;
  std::map<uint32_t, RetainPtr<CPDF_Object>> objects;
};

// Reads |data| into objects |layer| holds, or nullopt when it is not a
// whole, well-formed capture of kind |kind|, names a file other than
// |layer|'s or a number above the layer's last one, or holds an object no
// capture holds (the catalog, the page tree, a page other than the one a
// page capture is of). Nothing is written.
std::optional<Capture> ReadCapture(CPDF_LayerDocument* layer,
                                   pdfium::span<const uint8_t> data,
                                   ByteStringView kind) {
  const ByteStringView header(kHeader);
  if (data.size() < header.GetLength() ||
      ByteStringView(data.first(header.GetLength())) != header) {
    return std::nullopt;
  }
  CPDF_SyntaxParser parser(pdfium::MakeRetain<CFX_ReadOnlySpanStream>(data));
  parser.SetPos(header.GetLength());
  Capture capture;
  capture.manifest = ToDictionary(parser.GetObjectBody(layer));
  if (!capture.manifest || capture.manifest->GetNameFor("Kind") != kind ||
      !IsOfFile(capture.manifest.Get(), layer)) {
    return std::nullopt;
  }
  const int own_page =
      kind == "Page" ? capture.manifest->GetIntegerFor("Page") : 0;
  while (true) {
    parser.ToNextWord();
    if (parser.GetPos() >= parser.GetDocumentSize()) {
      break;
    }
    RetainPtr<CPDF_Object> object =
        parser.GetIndirectObject(layer, CPDF_SyntaxParser::ParseType::kStrict);
    if (!object || parser.GetKeyword() != "endobj") {
      return std::nullopt;
    }
    const uint32_t objnum = object->GetObjNum();
    if (!objnum || objnum > layer->GetLastObjNum() ||
        capture.objects.contains(objnum) ||
        (IsBoundaryObject(object.Get()) &&
         objnum != static_cast<uint32_t>(own_page))) {
      return std::nullopt;
    }
    // A stream read from the capture keeps its bytes in memory, as stored.
    if (CPDF_Stream* stream = object->AsMutableStream();
        stream && stream->IsFileBased()) {
      stream->TakeData(stream->ReadAllRawData());
    }
    capture.objects[objnum] = std::move(object);
  }
  return capture;
}

// Puts each captured object back where the layer holds no version of it.
bool InstallObjects(CPDF_LayerDocument* layer, Capture& capture) {
  for (auto& [objnum, object] : capture.objects) {
    if (layer->FindLayerVersion(objnum)) {
      continue;
    }
    if (!layer->RestoreLayerVersion(objnum, std::move(object))) {
      return false;
    }
  }
  return true;
}

// The object number of each member of |members| (an array of dictionaries
// with /Obj), or nullopt when one has none.
std::optional<std::vector<uint32_t>> MemberNumbers(const CPDF_Array* members) {
  std::vector<uint32_t> numbers;
  for (size_t i = 0; members && i < members->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> member = members->GetDictAt(i);
    const int objnum = member ? member->GetIntegerFor("Obj") : 0;
    if (objnum <= 0) {
      return std::nullopt;
    }
    numbers.push_back(static_cast<uint32_t>(objnum));
  }
  return numbers;
}

void* ToOwnedBuffer(const DataVector<uint8_t>& bytes, unsigned long* out_size) {
  return epdf::CopyToOwnedBuffer(bytes, out_size);
}

// Where a field's widget is: the page whose /Annots holds it (its /P when
// that page does), and its position there; nullopt when no page does.
std::optional<std::pair<uint32_t, size_t>> WidgetPlacement(
    CPDF_Document* doc,
    uint32_t widget_objnum,
    const CPDF_Dictionary* widget) {
  auto placed_on =
      [&](uint32_t page_objnum) -> std::optional<std::pair<uint32_t, size_t>> {
    const int page_index = doc->GetPageIndex(page_objnum);
    if (page_index < 0) {
      return std::nullopt;
    }
    RetainPtr<const CPDF_Dictionary> page = doc->GetPageDictionary(page_index);
    RetainPtr<const CPDF_Array> annots =
        page ? page->GetArrayFor("Annots") : nullptr;
    std::optional<size_t> index = PositionIn(annots.Get(), widget_objnum);
    if (!index) {
      return std::nullopt;
    }
    return std::make_pair(page_objnum, *index);
  };
  if (RetainPtr<const CPDF_Dictionary> page = widget->GetDictFor("P")) {
    if (auto placed = placed_on(page->GetObjNum())) {
      return placed;
    }
  }
  for (int i = 0; i < doc->GetPageCount(); ++i) {
    RetainPtr<const CPDF_Dictionary> page = doc->GetPageDictionary(i);
    if (page && page->GetObjNum()) {
      if (auto placed = placed_on(page->GetObjNum())) {
        return placed;
      }
    }
  }
  return std::nullopt;
}

}  // namespace

FPDF_EXPORT void* FPDF_CALLCONV
EPDFPage_ExportAnnotsRawToOwnedBuffer(FPDF_DOCUMENT document,
                                      int page_index,
                                      const int* indexes,
                                      int count,
                                      unsigned long* out_size) {
  if (out_size) {
    *out_size = 0;
  }
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !indexes || count <= 0 || page_index < 0 ||
      page_index >= layer->GetPageCount()) {
    return nullptr;
  }
  CPDF_DocumentViewScope view(layer);
  RetainPtr<const CPDF_Dictionary> page = layer->GetPageDictionary(page_index);
  RetainPtr<const CPDF_Array> annots =
      page ? page->GetArrayFor("Annots") : nullptr;
  if (!page || !page->GetObjNum() || !annots) {
    return nullptr;
  }
  const std::vector<uint32_t> refs = RefsIn(annots.Get());

  RetainPtr<CPDF_Dictionary> manifest = NewManifest(layer, "Annots");
  manifest->SetNewFor<CPDF_Number>(
      "Page", pdfium::checked_cast<int>(page->GetObjNum()));
  auto members = manifest->SetNewFor<CPDF_Array>("Members");
  CaptureWriter writer(layer);
  // SAFETY: required from caller.
  for (int index :
       UNSAFE_BUFFERS(pdfium::span(indexes, static_cast<size_t>(count)))) {
    if (index < 0 || static_cast<size_t>(index) >= refs.size() ||
        !refs[index]) {
      return nullptr;  // a position the page doesn't have, or inline
    }
    auto member = members->AppendNew<CPDF_Dictionary>();
    member->SetNewFor<CPDF_Number>("Obj",
                                   pdfium::checked_cast<int>(refs[index]));
    member->SetNewFor<CPDF_Number>("Index", index);
    writer.AddObject(refs[index]);
  }
  return ToOwnedBuffer(writer.Write(manifest.Get()), out_size);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_ImportAnnotsRaw(FPDF_DOCUMENT document,
                         int page_index,
                         const void* capture,
                         unsigned long size) {
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !layer->InTransaction() || !capture || !size ||
      page_index < 0 || page_index >= layer->GetPageCount()) {
    return false;
  }
  CPDF_DocumentViewScope view(layer);
  // SAFETY: required from caller.
  std::optional<Capture> read = ReadCapture(
      layer,
      UNSAFE_BUFFERS(pdfium::span(static_cast<const uint8_t*>(capture), size)),
      "Annots");
  if (!read) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> page = layer->GetPageDictionary(page_index);
  if (!page || page->GetObjNum() == 0 ||
      read->manifest->GetIntegerFor("Page") !=
          static_cast<int>(page->GetObjNum())) {
    return false;
  }
  RetainPtr<const CPDF_Array> members = read->manifest->GetArrayFor("Members");
  std::optional<std::vector<uint32_t>> numbers = MemberNumbers(members.Get());
  if (!numbers) {
    return false;
  }
  std::vector<std::pair<size_t, uint32_t>> placements;
  for (size_t i = 0; i < numbers->size(); ++i) {
    const int index = members->GetDictAt(i)->GetIntegerFor("Index");
    placements.emplace_back(static_cast<size_t>(std::max(index, 0)),
                            (*numbers)[i]);
  }
  std::ranges::sort(placements);

  if (!InstallObjects(layer, *read)) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> mutable_page =
      layer->GetMutablePageDictionary(page_index);
  RetainPtr<CPDF_Array> annots = epdf::GetMutableArrayMember(
      layer, mutable_page.Get(), "Annots", /*create_if_missing=*/true);
  if (!annots) {
    return false;
  }
  for (const auto& [index, objnum] : placements) {
    InsertReference(layer, annots.Get(), objnum, index);
  }
  return true;
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDFDoc_ExportDictRawToOwnedBuffer(FPDF_DOCUMENT document,
                                   uint32_t objnum,
                                   FPDF_BYTESTRING deep_keys,
                                   unsigned long* out_size) {
  if (out_size) {
    *out_size = 0;
  }
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !objnum) {
    return nullptr;
  }
  CPDF_DocumentViewScope view(layer);
  RetainPtr<const CPDF_Dictionary> dict =
      ToDictionary(layer->GetOrParseIndirectObject(objnum));
  if (!dict) {
    return nullptr;
  }
  RetainPtr<CPDF_Dictionary> manifest = NewManifest(layer, "Dict");
  manifest->SetNewFor<CPDF_Number>("Obj", pdfium::checked_cast<int>(objnum));
  manifest->SetNewFor<CPDF_Number>(
      "Gen", pdfium::checked_cast<int>(dict->GetGenNum()));
  manifest->SetFor("Value", dict->Clone());
  auto deep = manifest->SetNewFor<CPDF_Array>("Deep");
  CaptureWriter writer(layer);
  for (const ByteString& key :
       fxcrt::Split(ByteString(deep_keys ? deep_keys : ""), ' ')) {
    if (key.IsEmpty()) {
      continue;
    }
    deep->AppendNew<CPDF_Name>(key);
    writer.AddReachOf(dict->GetObjectFor(key.AsStringView()).Get());
  }
  return ToOwnedBuffer(writer.Write(manifest.Get()), out_size);
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDFDoc_ExportDictCanonicalToOwnedBuffer(FPDF_DOCUMENT document,
                                         uint32_t objnum,
                                         FPDF_BYTESTRING deep_keys,
                                         FPDF_BYTESTRING skip_keys,
                                         unsigned long* out_size) {
  if (out_size) {
    *out_size = 0;
  }
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !objnum) {
    return nullptr;
  }
  CPDF_DocumentViewScope view(layer);
  RetainPtr<const CPDF_Dictionary> dict =
      ToDictionary(layer->GetOrParseIndirectObject(objnum));
  if (!dict) {
    return nullptr;
  }
  // No /Base: whether the layer knows its file's hash may change between two
  // reads, and the bytes must not.
  auto manifest = pdfium::MakeRetain<CPDF_Dictionary>();
  manifest->SetNewFor<CPDF_Name>("Kind", "Dict");
  manifest->SetNewFor<CPDF_Number>("Obj", pdfium::checked_cast<int>(objnum));
  RetainPtr<CPDF_Dictionary> value = ToDictionary(dict->Clone());
  for (const ByteString& key :
       fxcrt::Split(ByteString(skip_keys ? skip_keys : ""), ' ')) {
    if (!key.IsEmpty()) {
      value->RemoveFor(key.AsStringView());
    }
  }
  manifest->SetFor("Value", value);
  CaptureWriter writer(layer, StreamBytes::kAsRead);
  for (const ByteString& key :
       fxcrt::Split(ByteString(deep_keys ? deep_keys : ""), ' ')) {
    if (!key.IsEmpty()) {
      writer.AddReachOf(value->GetObjectFor(key.AsStringView()).Get());
    }
  }
  return ToOwnedBuffer(writer.Write(manifest.Get()), out_size);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_ImportDictRaw(FPDF_DOCUMENT document,
                      const void* capture,
                      unsigned long size) {
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !layer->InTransaction() || !capture || !size) {
    return false;
  }
  CPDF_DocumentViewScope view(layer);
  // SAFETY: required from caller.
  std::optional<Capture> read = ReadCapture(
      layer,
      UNSAFE_BUFFERS(pdfium::span(static_cast<const uint8_t*>(capture), size)),
      "Dict");
  if (!read) {
    return false;
  }
  const int objnum = read->manifest->GetIntegerFor("Obj");
  RetainPtr<const CPDF_Dictionary> value = read->manifest->GetDictFor("Value");
  if (objnum <= 0 || static_cast<uint32_t>(objnum) > layer->GetLastObjNum() ||
      !value) {
    return false;
  }
  RetainPtr<const CPDF_Object> current =
      layer->GetOrParseIndirectObject(objnum);
  if (current && !current->IsDictionary()) {
    return false;
  }
  if (!InstallObjects(layer, *read)) {
    return false;
  }
  // Gone (a save dropped it once nothing reached it): it comes back whole.
  if (!current) {
    RetainPtr<CPDF_Object> restored = value->Clone();
    restored->SetGenNum(read->manifest->GetIntegerFor("Gen"));
    return layer->RestoreLayerVersion(objnum, std::move(restored));
  }
  RetainPtr<CPDF_Dictionary> dict =
      ToDictionary(layer->GetMutableIndirectObject(objnum));
  if (!dict) {
    return false;
  }
  for (const ByteString& key : dict->GetKeys()) {
    dict->RemoveFor(key.AsStringView());
  }
  CPDF_DictionaryLocker locker(value);
  for (const auto& [key, entry] : locker) {
    dict->SetFor(key, entry->Clone());
  }
  return true;
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDFForm_ExportFieldRawToOwnedBuffer(FPDF_DOCUMENT document,
                                     uint32_t field_objnum,
                                     unsigned long* out_size) {
  if (out_size) {
    *out_size = 0;
  }
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !field_objnum) {
    return nullptr;
  }
  CPDF_DocumentViewScope view(layer);
  RetainPtr<const CPDF_Dictionary> field =
      epdf::ResolveFieldDict(layer, field_objnum);
  if (!field) {
    return nullptr;
  }

  // The widgets: the field itself when it is merged with its widget, else its
  // kids, none of which may be a field.
  std::vector<uint32_t> widgets;
  if (field->GetNameFor("Subtype") == "Widget") {
    widgets.push_back(field_objnum);
  } else {
    RetainPtr<const CPDF_Array> kids = field->GetArrayFor("Kids");
    for (uint32_t kid : RefsIn(kids.Get())) {
      RetainPtr<const CPDF_Dictionary> dict =
          ToDictionary(layer->GetOrParseIndirectObject(kid));
      if (!kid || (dict && dict->KeyExist("T"))) {
        return nullptr;  // a direct kid, or a child field
      }
      if (dict) {
        widgets.push_back(kid);
      }
    }
  }

  RetainPtr<CPDF_Dictionary> manifest = NewManifest(layer, "Field");
  manifest->SetNewFor<CPDF_Number>("Field",
                                   pdfium::checked_cast<int>(field_objnum));
  CaptureWriter writer(layer);
  writer.AddObject(field_objnum);

  // The chain the delete takes out: the field, then each ancestor it leaves
  // empty, each with its place in its container (0: /AcroForm /Fields).
  auto chain = manifest->SetNewFor<CPDF_Array>("Chain");
  uint32_t current = field_objnum;
  for (int depth = 0; depth < kMaxFieldDepth; ++depth) {
    RetainPtr<const CPDF_Dictionary> node =
        ToDictionary(layer->GetOrParseIndirectObject(current));
    RetainPtr<const CPDF_Dictionary> parent =
        node ? node->GetDictFor("Parent") : nullptr;
    RetainPtr<const CPDF_Array> container;
    if (parent && parent->GetObjNum()) {
      container = parent->GetArrayFor("Kids");
    } else {
      RetainPtr<const CPDF_Dictionary> acro_form =
          layer->GetRoot()->GetDictFor("AcroForm");
      container = acro_form ? acro_form->GetArrayFor("Fields") : nullptr;
    }
    std::optional<size_t> index = PositionIn(container.Get(), current);
    if (!index) {
      return nullptr;  // not where its parent says
    }
    auto link = chain->AppendNew<CPDF_Dictionary>();
    link->SetNewFor<CPDF_Number>("Obj", pdfium::checked_cast<int>(current));
    link->SetNewFor<CPDF_Number>(
        "In", parent ? pdfium::checked_cast<int>(parent->GetObjNum()) : 0);
    link->SetNewFor<CPDF_Number>("Index", pdfium::checked_cast<int>(*index));
    if (!parent || !parent->GetObjNum() || container->size() > 1 ||
        parent->KeyExist("FT")) {
      break;  // the parent keeps other children, or is a field itself
    }
    current = parent->GetObjNum();
    writer.AddObject(current);
  }

  auto placed = manifest->SetNewFor<CPDF_Array>("Widgets");
  for (uint32_t widget : widgets) {
    RetainPtr<const CPDF_Dictionary> dict =
        ToDictionary(layer->GetOrParseIndirectObject(widget));
    auto entry = placed->AppendNew<CPDF_Dictionary>();
    entry->SetNewFor<CPDF_Number>("Obj", pdfium::checked_cast<int>(widget));
    if (auto placement = WidgetPlacement(layer, widget, dict.Get())) {
      entry->SetNewFor<CPDF_Number>(
          "Page", pdfium::checked_cast<int>(placement->first));
      entry->SetNewFor<CPDF_Number>(
          "Index", pdfium::checked_cast<int>(placement->second));
    }
    writer.AddObject(widget);
  }
  return ToOwnedBuffer(writer.Write(manifest.Get()), out_size);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFForm_ImportFieldRaw(FPDF_DOCUMENT document,
                        const void* capture,
                        unsigned long size) {
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !layer->InTransaction() || !capture || !size) {
    return false;
  }
  CPDF_DocumentViewScope view(layer);
  // SAFETY: required from caller.
  std::optional<Capture> read = ReadCapture(
      layer,
      UNSAFE_BUFFERS(pdfium::span(static_cast<const uint8_t*>(capture), size)),
      "Field");
  if (!read) {
    return false;
  }
  const int field_objnum = read->manifest->GetIntegerFor("Field");
  RetainPtr<const CPDF_Array> chain = read->manifest->GetArrayFor("Chain");
  RetainPtr<const CPDF_Array> widgets = read->manifest->GetArrayFor("Widgets");
  if (field_objnum <= 0 || !chain || chain->IsEmpty() || !widgets ||
      !MemberNumbers(chain.Get()) || !MemberNumbers(widgets.Get())) {
    return false;
  }

  // Everything it goes back into must still be there.
  for (size_t i = 0; i < chain->size(); ++i) {
    const int in = chain->GetDictAt(i)->GetIntegerFor("In");
    if (in < 0 ||
        (in > 0 && !ToDictionary(layer->GetOrParseIndirectObject(in)))) {
      return false;
    }
  }
  for (size_t i = 0; i < widgets->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> widget = widgets->GetDictAt(i);
    if (widget->KeyExist("Page") &&
        layer->GetPageIndex(widget->GetIntegerFor("Page")) < 0) {
      return false;
    }
  }

  if (!InstallObjects(layer, *read)) {
    return false;
  }
  // From the top of the chain down, so each container exists before its
  // child goes into it.
  for (size_t i = chain->size(); i-- > 0;) {
    RetainPtr<const CPDF_Dictionary> link = chain->GetDictAt(i);
    const int in = link->GetIntegerFor("In");
    RetainPtr<CPDF_Dictionary> container =
        in > 0 ? ToDictionary(layer->GetMutableIndirectObject(in))
               : epdf::GetMutableAcroForm(layer, /*create_if_missing=*/true,
                                          nullptr);
    RetainPtr<CPDF_Array> array = epdf::GetMutableArrayMember(
        layer, container.Get(), in > 0 ? "Kids" : "Fields",
        /*create_if_missing=*/true);
    if (!array) {
      return false;
    }
    InsertReference(
        layer, array.Get(), link->GetIntegerFor("Obj"),
        static_cast<size_t>(std::max(link->GetIntegerFor("Index"), 0)));
  }
  for (size_t i = 0; i < widgets->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> entry = widgets->GetDictAt(i);
    const uint32_t widget = entry->GetIntegerFor("Obj");
    if (widget != static_cast<uint32_t>(field_objnum)) {
      RetainPtr<CPDF_Dictionary> dict =
          ToDictionary(layer->GetMutableIndirectObject(widget));
      if (!dict) {
        return false;
      }
      RetainPtr<const CPDF_Dictionary> parent = dict->GetDictFor("Parent");
      if (!parent ||
          parent->GetObjNum() != static_cast<uint32_t>(field_objnum)) {
        dict->SetNewFor<CPDF_Reference>("Parent", layer, field_objnum);
      }
    }
    if (!entry->KeyExist("Page")) {
      continue;  // it was on no page
    }
    const int page_index = layer->GetPageIndex(entry->GetIntegerFor("Page"));
    RetainPtr<CPDF_Dictionary> page =
        layer->GetMutablePageDictionary(page_index);
    RetainPtr<CPDF_Array> annots = epdf::GetMutableArrayMember(
        layer, page.Get(), "Annots", /*create_if_missing=*/true);
    if (!annots) {
      return false;
    }
    InsertReference(
        layer, annots.Get(), widget,
        static_cast<size_t>(std::max(entry->GetIntegerFor("Index"), 0)));
  }
  return true;
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDFOutline_ExportItemRawToOwnedBuffer(FPDF_DOCUMENT document,
                                       uint32_t objnum,
                                       unsigned long* out_size) {
  if (out_size) {
    *out_size = 0;
  }
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !objnum) {
    return nullptr;
  }
  CPDF_DocumentViewScope view(layer);
  std::optional<uint32_t> root = epdf::OutlineRootOf(layer);
  if (!root || !epdf::IsOutlineItem(layer, *root, objnum)) {
    return nullptr;
  }
  const uint32_t parent = epdf::OutlineParentOf(layer, objnum);
  std::optional<std::vector<uint32_t>> siblings =
      epdf::OutlineChildNumbers(layer, parent);
  if (!siblings) {
    return nullptr;
  }
  auto at = std::ranges::find(*siblings, objnum);
  const uint32_t after = at == siblings->begin() ? 0 : *(at - 1);

  // The item, where it was (In: its parent, 0 for the top level; After: the
  // sibling it followed, 0 when it was the first), and the items under it.
  RetainPtr<CPDF_Dictionary> manifest = NewManifest(layer, "OutlineItem");
  manifest->SetNewFor<CPDF_Number>("Item", pdfium::checked_cast<int>(objnum));
  manifest->SetNewFor<CPDF_Number>(
      "In", parent == *root ? 0 : pdfium::checked_cast<int>(parent));
  manifest->SetNewFor<CPDF_Number>("After", pdfium::checked_cast<int>(after));
  auto items = manifest->SetNewFor<CPDF_Array>("Items");
  CaptureWriter writer(layer);
  for (uint32_t item : epdf::OutlineSubtreeOf(layer, objnum)) {
    items->AppendNew<CPDF_Number>(pdfium::checked_cast<int>(item));
    writer.AddOutlineItem(item);
  }
  return ToOwnedBuffer(writer.Write(manifest.Get()), out_size);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFOutline_ImportItemRaw(FPDF_DOCUMENT document,
                          const void* capture,
                          unsigned long size) {
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !layer->InTransaction() || !capture || !size) {
    return false;
  }
  CPDF_DocumentViewScope view(layer);
  // SAFETY: required from caller.
  std::optional<Capture> read = ReadCapture(
      layer,
      UNSAFE_BUFFERS(pdfium::span(static_cast<const uint8_t*>(capture), size)),
      "OutlineItem");
  if (!read) {
    return false;
  }
  const int item = read->manifest->GetIntegerFor("Item");
  const int in = read->manifest->GetIntegerFor("In");
  const int after = read->manifest->GetIntegerFor("After");
  std::optional<uint32_t> root = epdf::OutlineRootOf(layer);
  if (item <= 0 || in < 0 || after < 0 || !root) {
    return false;
  }

  // Where it goes must still be there, and it must not be back already.
  if (in > 0 && !epdf::IsOutlineItem(layer, *root, in)) {
    return false;
  }
  if (epdf::IsOutlineItem(layer, *root, item)) {
    return false;
  }
  // What the number holds once the capture is in, which must be an outline
  // item: the layer's own version stays (InstallObjects()), else the
  // capture's comes back, else the file's original.
  auto captured = read->objects.find(static_cast<uint32_t>(item));
  RetainPtr<const CPDF_Object> comes_back =
      layer->FindLayerVersion(item) || captured == read->objects.end()
          ? layer->GetOrParseIndirectObject(item)
          : captured->second;
  if (!CPDF_BookmarkTree::IsOutlineNode(ToDictionary(comes_back).Get())) {
    return false;
  }
  std::vector<uint32_t> children;
  if (const uint32_t node = in > 0 ? in : *root) {
    std::optional<std::vector<uint32_t>> numbers =
        epdf::OutlineChildNumbers(layer, node);
    if (!numbers || std::ranges::find(*numbers, static_cast<uint32_t>(item)) !=
                        numbers->end()) {
      return false;
    }
    children = std::move(*numbers);
  }
  // After the sibling it followed while that is still a child here, else
  // first.
  size_t slot = 0;
  if (auto at = std::ranges::find(children, static_cast<uint32_t>(after));
      after > 0 && at != children.end()) {
    slot = static_cast<size_t>(at - children.begin()) + 1;
  }

  if (!InstallObjects(layer, *read)) {
    return false;
  }
  if (!*root) {
    root = epdf::CreateOutlineRoot(layer);
    if (!*root) {
      return false;
    }
  }
  const uint32_t node = in > 0 ? static_cast<uint32_t>(in) : *root;
  children.insert(children.begin() + slot, static_cast<uint32_t>(item));
  return epdf::WriteOutlineChildren(layer, node, children) &&
         epdf::RecountOutline(layer, *root, item);
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDFPage_ExportRawToOwnedBuffer(FPDF_DOCUMENT document,
                                uint32_t page_objnum,
                                unsigned long* out_size) {
  if (out_size) {
    *out_size = 0;
  }
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !page_objnum) {
    return nullptr;
  }
  CPDF_DocumentViewScope view(layer);
  const int page_index = layer->GetPageIndex(page_objnum);
  if (page_index < 0) {
    return nullptr;
  }
  RetainPtr<const CPDF_Dictionary> before =
      page_index > 0 ? layer->GetPageDictionary(page_index - 1) : nullptr;

  // The page, where it was (After: the page it followed, 0 when it was the
  // first), and the keys that name it.
  RetainPtr<CPDF_Dictionary> manifest = NewManifest(layer, "Page");
  manifest->SetNewFor<CPDF_Number>("Page",
                                   pdfium::checked_cast<int>(page_objnum));
  manifest->SetNewFor<CPDF_Number>(
      "After", before ? pdfium::checked_cast<int>(before->GetObjNum()) : 0);
  auto names = manifest->SetNewFor<CPDF_Array>("Names");
  for (const WideString& key : epdf::NamedPageKeysOf(layer, page_objnum)) {
    names->AppendNew<CPDF_String>(key.AsStringView());
  }
  CaptureWriter writer(layer);
  // What it takes from the page tree above it (Inherited): an import may put
  // it under another /Pages node, so it brings these back as its own.
  RetainPtr<const CPDF_Dictionary> page = layer->GetPageDictionary(page_index);
  auto inherited = manifest->SetNewFor<CPDF_Dictionary>("Inherited");
  for (const char* key : kInheritableKeys) {
    RetainPtr<const CPDF_Object> value = page && !page->KeyExist(key)
                                             ? InheritedValueOf(page.Get(), key)
                                             : nullptr;
    if (value) {
      inherited->SetFor(key, value->Clone());
      writer.AddReachOf(value.Get());
    }
  }
  writer.AddPage(page_objnum);
  return ToOwnedBuffer(writer.Write(manifest.Get()), out_size);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_ImportRaw(FPDF_DOCUMENT document,
                   const void* capture,
                   unsigned long size) {
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !layer->InTransaction() || !capture || !size) {
    return false;
  }
  CPDF_DocumentViewScope view(layer);
  // SAFETY: required from caller.
  std::optional<Capture> read = ReadCapture(
      layer,
      UNSAFE_BUFFERS(pdfium::span(static_cast<const uint8_t*>(capture), size)),
      "Page");
  if (!read) {
    return false;
  }
  const int page_number = read->manifest->GetIntegerFor("Page");
  const int after = read->manifest->GetIntegerFor("After");
  if (page_number <= 0 || after < 0 ||
      static_cast<uint32_t>(page_number) > layer->GetLastObjNum()) {
    return false;
  }
  const uint32_t page = static_cast<uint32_t>(page_number);
  // It must not be back already.
  if (layer->GetPageIndex(page) >= 0) {
    return false;
  }
  // It comes back from the capture, or as the page the layer reads now (the
  // file's original, when the layer never edited it).
  auto captured = read->objects.find(page);
  const bool in_capture = captured != read->objects.end();
  // What the number holds once the capture is in: the layer's own version
  // stays (InstallObjects()), save the null a deleted layer page left, which
  // the capture's page replaces; without a version of the layer's, the
  // capture's, or the file's original.
  RetainPtr<const CPDF_Object> held = layer->FindLayerVersion(page);
  const bool over_null = held && held->IsNull();
  RetainPtr<const CPDF_Dictionary> reads_now =
      ToDictionary(layer->GetOrParseIndirectObject(page));
  const CPDF_Dictionary* comes_back = in_capture && (!held || over_null)
                                          ? captured->second->AsDictionary()
                                          : reads_now.Get();
  if (!comes_back || comes_back->GetNameFor("Type") != "Page") {
    return false;
  }
  // After the page it followed while that is still in the page tree, else
  // first.
  const int anchor =
      after > 0 ? layer->GetPageIndex(static_cast<uint32_t>(after)) : -1;
  const int slot = anchor >= 0 ? anchor + 1 : 0;

  // A page the layer made reads as null once deleted (SetPageToNullObject(),
  // so nothing that still points at it saves it): it comes back over that
  // null.
  if (in_capture && over_null) {
    layer->DeleteIndirectObject(page);
  }
  if (!InstallObjects(layer, *read)) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> page_dict =
      ToDictionary(layer->GetMutableIndirectObject(page));
  if (!page_dict || !layer->InsertExistingPage(slot, page_dict)) {
    return false;
  }
  // What it took from the page tree where it was, as its own key where its
  // place now gives otherwise; back under the same node, nothing changes.
  if (RetainPtr<const CPDF_Dictionary> inherited =
          read->manifest->GetDictFor("Inherited")) {
    CPDF_DictionaryLocker locker(inherited);
    for (const auto& [key, value] : locker) {
      if (!IsInheritableKey(key.AsStringView()) ||
          page_dict->KeyExist(key.AsStringView())) {
        continue;
      }
      RetainPtr<const CPDF_Object> now =
          InheritedValueOf(page_dict.Get(), key.c_str());
      if (SyntaxOf(now.Get()) != SyntaxOf(value.Get())) {
        page_dict->SetFor(key, value->Clone());
      }
    }
  }
  // A key another page holds now stays with it.
  RetainPtr<const CPDF_Array> names = read->manifest->GetArrayFor("Names");
  for (size_t i = 0; names && i < names->size(); ++i) {
    epdf::AddNamedPageIfFree(layer, names->GetUnicodeTextAt(i), page);
  }
  return true;
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDFAttachment_ExportRawToOwnedBuffer(FPDF_DOCUMENT document,
                                      FPDF_WIDESTRING key,
                                      unsigned long* out_size) {
  if (out_size) {
    *out_size = 0;
  }
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !key) {
    return nullptr;
  }
  CPDF_DocumentViewScope view(layer);
  // SAFETY: required from caller.
  const WideString name = UNSAFE_BUFFERS(WideStringFromFPDFWideString(key));
  auto tree = CPDF_NameTree::CreateForReading(layer, "EmbeddedFiles");
  RetainPtr<const CPDF_Dictionary> spec =
      tree && !name.IsEmpty() ? ToDictionary(tree->LookupValue(name)) : nullptr;
  if (!spec) {
    return nullptr;
  }

  // The file, under its key (Key): its file specification, an object of its
  // own (Obj) or written in the tree (Value), and the objects the layer
  // holds that it reaches.
  RetainPtr<CPDF_Dictionary> manifest = NewManifest(layer, "Attachment");
  manifest->SetNewFor<CPDF_String>("Key", name.AsStringView());
  CaptureWriter writer(layer);
  if (const uint32_t objnum = spec->GetObjNum()) {
    manifest->SetNewFor<CPDF_Number>("Obj", pdfium::checked_cast<int>(objnum));
    writer.AddObject(objnum);
  } else {
    manifest->SetFor("Value", spec->Clone());
    writer.AddReachOf(spec.Get());
  }
  return ToOwnedBuffer(writer.Write(manifest.Get()), out_size);
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDFAttachment_ExportCanonicalToOwnedBuffer(FPDF_DOCUMENT document,
                                            FPDF_WIDESTRING key,
                                            unsigned long* out_size) {
  if (out_size) {
    *out_size = 0;
  }
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !key) {
    return nullptr;
  }
  CPDF_DocumentViewScope view(layer);
  // SAFETY: required from caller.
  const WideString name = UNSAFE_BUFFERS(WideStringFromFPDFWideString(key));
  auto tree = CPDF_NameTree::CreateForReading(layer, "EmbeddedFiles");
  RetainPtr<const CPDF_Dictionary> spec =
      tree && !name.IsEmpty() ? ToDictionary(tree->LookupValue(name)) : nullptr;
  if (!spec) {
    return nullptr;
  }
  // No /Base, as for EPDFDoc_ExportDictCanonicalToOwnedBuffer(). The number
  // is part of what it reads as: another file under the key, even with the
  // same bytes, is another file.
  auto manifest = pdfium::MakeRetain<CPDF_Dictionary>();
  manifest->SetNewFor<CPDF_Name>("Kind", "Attachment");
  manifest->SetNewFor<CPDF_String>("Key", name.AsStringView());
  if (const uint32_t objnum = spec->GetObjNum()) {
    manifest->SetNewFor<CPDF_Number>("Obj", pdfium::checked_cast<int>(objnum));
  }
  RetainPtr<CPDF_Object> value = spec->Clone();
  manifest->SetFor("Value", value);
  CaptureWriter writer(layer, StreamBytes::kAsRead);
  writer.AddReachOf(value.Get());
  return ToOwnedBuffer(writer.Write(manifest.Get()), out_size);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAttachment_ImportRaw(FPDF_DOCUMENT document,
                         const void* capture,
                         unsigned long size) {
  CPDF_LayerDocument* layer = LayerOf(document);
  if (!layer || !layer->InTransaction() || !capture || !size) {
    return false;
  }
  CPDF_DocumentViewScope view(layer);
  // SAFETY: required from caller.
  std::optional<Capture> read = ReadCapture(
      layer,
      UNSAFE_BUFFERS(pdfium::span(static_cast<const uint8_t*>(capture), size)),
      "Attachment");
  if (!read) {
    return false;
  }
  const WideString key = read->manifest->GetUnicodeTextFor("Key");
  if (key.IsEmpty()) {
    return false;
  }
  // A key another file holds now stays with it.
  if (auto tree = CPDF_NameTree::CreateForReading(layer, "EmbeddedFiles");
      tree && tree->LookupValue(key)) {
    return false;
  }

  // What goes back under the key: a reference to the file specification,
  // which is what its number holds once the capture is in (the layer's own
  // version stays, else the capture's, else the file's original); or the
  // specification written in the tree. Either must be a file specification.
  RetainPtr<CPDF_Object> entry;
  if (const int objnum = read->manifest->GetIntegerFor("Obj"); objnum > 0) {
    const uint32_t number = static_cast<uint32_t>(objnum);
    if (number > layer->GetLastObjNum()) {
      return false;
    }
    auto captured = read->objects.find(number);
    RetainPtr<const CPDF_Object> comes_back =
        layer->FindLayerVersion(number) || captured == read->objects.end()
            ? layer->GetOrParseIndirectObject(number)
            : captured->second;
    if (!IsFileSpec(ToDictionary(comes_back).Get())) {
      return false;
    }
    entry = pdfium::MakeRetain<CPDF_Reference>(layer, number);
  } else {
    RetainPtr<const CPDF_Dictionary> value =
        read->manifest->GetDictFor("Value");
    if (!IsFileSpec(value.Get())) {
      return false;
    }
    entry = value->Clone();
  }

  if (!InstallObjects(layer, *read)) {
    return false;
  }
  auto tree = CPDF_NameTree::CreateWithRootNameArray(layer, "EmbeddedFiles");
  return tree && tree->AddValueAndName(std::move(entry), key);
}
