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
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_syntax_parser.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/cfx_read_only_span_stream.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_form_helpers.h"
#include "fpdfsdk/epdf_object_helpers.h"
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

// Objects a capture never holds: the catalog, the page tree and its pages.
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

// Collects the layer's versions of what a capture's roots reach, and writes
// the capture.
class CaptureWriter {
 public:
  explicit CaptureWriter(CPDF_LayerDocument* layer) : layer_(layer) {}

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

  // The header, |manifest|, then every captured object in number order.
  DataVector<uint8_t> Write(const CPDF_Dictionary* manifest) const {
    fxcrt::ostringstream text;
    CPDF_StringArchiveStream archive(&text);
    bool ok = archive.WriteString(kHeader) &&
              manifest->WriteTo(&archive, nullptr) && archive.WriteString("\n");
    for (const auto& [objnum, object] : objects_) {
      ok = ok && archive.WriteDWord(objnum) && archive.WriteString(" ") &&
           archive.WriteDWord(object->GetGenNum()) &&
           archive.WriteString(" obj\n") && WriteBody(&archive, object.Get()) &&
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
      if (!object || IsBoundaryObject(object.Get())) {
        continue;
      }
      if (RetainPtr<CPDF_Object> version = layer_->FindLayerVersion(objnum)) {
        objects_[objnum] = std::move(version);
      }
      CollectReferences(object.Get());
    }
  }

  // An object's body. A stream is written with its bytes as stored, never
  // encoded again, so it comes back byte for byte.
  static bool WriteBody(IFX_ArchiveStream* archive, const CPDF_Object* object) {
    const CPDF_Stream* stream = object->AsStream();
    if (!stream) {
      return object->WriteTo(archive, nullptr);
    }
    DataVector<uint8_t> raw;
    if (stream->IsMemoryBased()) {
      pdfium::span<const uint8_t> data = stream->GetInMemoryRawData();
      raw.assign(data.begin(), data.end());
    } else {
      raw = stream->ReadAllRawData();
    }
    RetainPtr<CPDF_Dictionary> dict = ToDictionary(stream->GetDict()->Clone());
    dict->SetNewFor<CPDF_Number>("Length",
                                 pdfium::checked_cast<int>(raw.size()));
    return dict->WriteTo(archive, nullptr) &&
           archive->WriteString("stream\r\n") && archive->WriteBlock(raw) &&
           archive->WriteString("\r\nendstream");
  }

  UnownedPtr<CPDF_LayerDocument> const layer_;
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
// whole, well-formed capture of kind |kind|, or names a number above the
// layer's last one. Nothing is written.
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
  if (!capture.manifest || capture.manifest->GetNameFor("Kind") != kind) {
    return std::nullopt;
  }
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
        capture.objects.contains(objnum)) {
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

  auto manifest = pdfium::MakeRetain<CPDF_Dictionary>();
  manifest->SetNewFor<CPDF_Name>("Kind", "Annots");
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
  auto manifest = pdfium::MakeRetain<CPDF_Dictionary>();
  manifest->SetNewFor<CPDF_Name>("Kind", "Dict");
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

  auto manifest = pdfium::MakeRetain<CPDF_Dictionary>();
  manifest->SetNewFor<CPDF_Name>("Kind", "Field");
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
