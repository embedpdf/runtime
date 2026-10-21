// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_documentfonts.h"

#include <algorithm>
#include <bit>
#include <memory>
#include <set>
#include <utility>

#include "core/fdrm/fx_crypt_sha.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfdoc/cpdf_displayprogram.h"
#include "core/fpdfdoc/cpdf_fontface.h"
#include "core/fxcrt/utf16.h"
#include "core/fxge/cfx_face.h"
#include "core/fxge/cfx_font.h"
#include "core/fxge/cfx_sfnt.h"
#include "core/fxge/fx_fontencoding.h"

namespace {

// Deep enough for any real nesting of pages, forms and patterns; a cycle the
// walked set misses (a direct object repeated) stops here.
constexpr int kMaxDepth = 64;

// The font dictionaries the drawings of one place can use, each once: the
// AcroForm's, a page's, or the whole document's. Notes the object number of
// every indirect object it reads, so a change to any of them can be told.
class FontWalk {
 public:
  explicit FontWalk(CPDF_Document* doc) : doc_(doc) {}

  void Form() {
    RetainPtr<const CPDF_Dictionary> root =
        Noted(pdfium::WrapRetain(doc_->GetRoot()));
    RetainPtr<const CPDF_Dictionary> acroform =
        root ? Noted(root->GetDictFor("AcroForm")) : nullptr;
    if (acroform) {
      Resources(acroform->GetDictFor("DR"), 0);
      Fields(acroform->GetArrayFor("Fields"), 0);
    }
  }

  void Page(RetainPtr<const CPDF_Dictionary> page) {
    if (!Noted(page)) {
      return;
    }
    Resources(InheritedResources(page), 0);
    RetainPtr<const CPDF_Array> annots = Noted(page->GetArrayFor("Annots"));
    for (size_t j = 0; annots && j < annots->size(); ++j) {
      Appearance(annots->GetDictAt(j), 0);
    }
  }

  void Document() {
    Form();
    const int page_count = doc_->GetPageCount();
    for (int i = 0; i < page_count; ++i) {
      Page(doc_->GetPageDictionary(i));
    }
  }

  std::vector<RetainPtr<const CPDF_Dictionary>> TakeFonts() {
    return std::move(fonts_);
  }
  // Sorted.
  std::vector<uint32_t> TakeReads() {
    return std::vector<uint32_t>(reads_.begin(), reads_.end());
  }

 private:
  template <typename T>
  RetainPtr<T> Noted(RetainPtr<T> object) {
    if (object && object->GetObjNum() != 0) {
      reads_.insert(object->GetObjNum());
    }
    return object;
  }

  // A page's /Resources, or the nearest one up its /Parent chain.
  RetainPtr<const CPDF_Dictionary> InheritedResources(
      RetainPtr<const CPDF_Dictionary> page) {
    std::set<const CPDF_Dictionary*> seen;
    RetainPtr<const CPDF_Dictionary> node = std::move(page);
    for (int i = 0; node && i < kMaxDepth && seen.insert(node.Get()).second;
         ++i) {
      if (RetainPtr<const CPDF_Dictionary> resources =
              node->GetDictFor("Resources")) {
        return resources;
      }
      node = Noted(node->GetDictFor("Parent"));
    }
    return nullptr;
  }

  void Resources(RetainPtr<const CPDF_Dictionary> resources, int depth) {
    if (!Noted(resources) || depth > kMaxDepth ||
        !walked_.insert(resources.Get()).second) {
      return;
    }
    if (RetainPtr<const CPDF_Dictionary> fonts =
            Noted(resources->GetDictFor("Font"))) {
      CPDF_DictionaryLocker locker(fonts);
      for (const auto& [key, value] : locker) {
        Font(value ? ToDictionary(value->GetDirect()) : nullptr, depth);
      }
    }
    if (RetainPtr<const CPDF_Dictionary> xobjects =
            Noted(resources->GetDictFor("XObject"))) {
      CPDF_DictionaryLocker locker(xobjects);
      for (const auto& [key, value] : locker) {
        RetainPtr<const CPDF_Stream> stream =
            value ? ToStream(value->GetDirect()) : nullptr;
        if (stream && stream->GetDict()->GetNameFor("Subtype") == "Form") {
          FormStream(std::move(stream), depth);
        }
      }
    }
    // Tiling patterns are streams with their own resources; shading
    // patterns are dictionaries and draw no text.
    if (RetainPtr<const CPDF_Dictionary> patterns =
            Noted(resources->GetDictFor("Pattern"))) {
      CPDF_DictionaryLocker locker(patterns);
      for (const auto& [key, value] : locker) {
        FormStream(value ? ToStream(value->GetDirect()) : nullptr, depth);
      }
    }
    if (RetainPtr<const CPDF_Dictionary> states =
            Noted(resources->GetDictFor("ExtGState"))) {
      CPDF_DictionaryLocker locker(states);
      for (const auto& [key, value] : locker) {
        RetainPtr<const CPDF_Dictionary> state =
            Noted(value ? ToDictionary(value->GetDirect()) : nullptr);
        RetainPtr<const CPDF_Dictionary> mask =
            state ? Noted(state->GetDictFor("SMask")) : nullptr;
        FormStream(mask ? mask->GetStreamFor("G") : nullptr, depth);
      }
    }
  }

  void FormStream(RetainPtr<const CPDF_Stream> stream, int depth) {
    if (!Noted(stream) || depth > kMaxDepth ||
        !walked_.insert(stream.Get()).second) {
      return;
    }
    Resources(stream->GetDict()->GetDictFor("Resources"), depth + 1);
  }

  void Font(RetainPtr<const CPDF_Dictionary> font, int depth) {
    if (!Noted(font) || !font_set_.insert(font.Get()).second) {
      return;
    }
    if (font->GetNameFor("Subtype") == "Type3") {
      // A Type 3 glyph is a drawing, and may draw text in other fonts.
      Resources(font->GetDictFor("Resources"), depth + 1);
    }
    fonts_.push_back(std::move(font));
  }

  // Every state of the normal, rollover and down appearances.
  void Appearance(RetainPtr<const CPDF_Dictionary> annot, int depth) {
    RetainPtr<const CPDF_Dictionary> ap =
        Noted(annot) ? Noted(annot->GetDictFor("AP")) : nullptr;
    if (!ap) {
      return;
    }
    for (const char* key : {"N", "R", "D"}) {
      RetainPtr<const CPDF_Object> entry = Noted(ap->GetDirectObjectFor(key));
      if (const CPDF_Stream* stream = entry ? entry->AsStream() : nullptr) {
        FormStream(pdfium::WrapRetain(stream), depth);
        continue;
      }
      const CPDF_Dictionary* states = entry ? entry->AsDictionary() : nullptr;
      if (!states) {
        continue;
      }
      CPDF_DictionaryLocker locker(states);
      for (const auto& [state, value] : locker) {
        FormStream(value ? ToStream(value->GetDirect()) : nullptr, depth);
      }
    }
  }

  // Fields' own /DR, down their /Kids.
  void Fields(RetainPtr<const CPDF_Array> fields, int depth) {
    if (!Noted(fields)) {
      return;
    }
    for (size_t i = 0; depth <= kMaxDepth && i < fields->size(); ++i) {
      RetainPtr<const CPDF_Dictionary> field = Noted(fields->GetDictAt(i));
      if (!field || !walked_.insert(field.Get()).second) {
        continue;
      }
      Resources(field->GetDictFor("DR"), depth + 1);
      Fields(field->GetArrayFor("Kids"), depth + 1);
    }
  }

  UnownedPtr<CPDF_Document> const doc_;
  std::set<const CPDF_Object*> walked_;
  std::set<const CPDF_Dictionary*> font_set_;
  std::vector<RetainPtr<const CPDF_Dictionary>> fonts_;
  std::set<uint32_t> reads_;
};

// The program stream a font dictionary carries, and what kind it says it is.
// An /OpenType /FontFile3 says nothing about its outlines: its bytes do.
std::pair<RetainPtr<const CPDF_Stream>, CPDF_DocumentFonts::Format> ProgramOf(
    const CPDF_Dictionary* font) {
  using Format = CPDF_DocumentFonts::Format;
  if (font->GetNameFor("Subtype") == "Type3") {
    return {nullptr, Format::kType3};
  }
  RetainPtr<const CPDF_Dictionary> descriptor = FontDescriptorOf(font);
  if (!descriptor) {
    return {nullptr, Format::kNone};
  }
  if (RetainPtr<const CPDF_Stream> stream =
          descriptor->GetStreamFor("FontFile2")) {
    return {std::move(stream), Format::kTrueType};
  }
  if (RetainPtr<const CPDF_Stream> stream =
          descriptor->GetStreamFor("FontFile")) {
    return {std::move(stream), Format::kType1};
  }
  if (RetainPtr<const CPDF_Stream> stream =
          descriptor->GetStreamFor("FontFile3")) {
    const bool sfnt = stream->GetDict()->GetNameFor("Subtype") == "OpenType";
    return {std::move(stream), sfnt ? Format::kTrueType : Format::kCFF};
  }
  return {nullptr, Format::kNone};
}

// The /BaseFont that names the program: a Type0 font's descendant's.
ByteString ProgramBaseFont(const CPDF_Dictionary* font) {
  if (font->GetNameFor("Subtype") == "Type0") {
    RetainPtr<const CPDF_Array> descendants =
        font->GetArrayFor("DescendantFonts");
    RetainPtr<const CPDF_Dictionary> descendant =
        descendants ? descendants->GetDictAt(0) : nullptr;
    if (descendant && !descendant->GetNameFor("BaseFont").IsEmpty()) {
      return descendant->GetNameFor("BaseFont");
    }
  }
  return font->GetNameFor("BaseFont");
}

bool IsSfnt(CPDF_DocumentFonts::Format format) {
  return format == CPDF_DocumentFonts::Format::kTrueType ||
         format == CPDF_DocumentFonts::Format::kOpenTypeCFF;
}

// Whether a browser loads |program| as it is: an sfnt that maps characters
// through a Unicode cmap, its tables where the font format puts them.
bool LoadsAsIs(const CPDF_DocumentFonts::Program& program) {
  return IsSfnt(program.format) && program.maps_unicode &&
         program.tables_aligned;
}

// |scalars| as ranges, in order.
std::vector<CFX_CodePointRange> RangesOf(std::vector<uint32_t> scalars) {
  std::ranges::sort(scalars);
  scalars.erase(std::unique(scalars.begin(), scalars.end()), scalars.end());
  std::vector<CFX_CodePointRange> ranges;
  for (uint32_t scalar : scalars) {
    CFX_AppendCodePoint(&ranges, scalar);
  }
  return ranges;
}

}  // namespace

CPDF_DocumentFonts::CPDF_DocumentFonts(CPDF_Document* doc) : doc_(doc) {}

CPDF_DocumentFonts::~CPDF_DocumentFonts() = default;

// static
CPDF_DocumentFonts* CPDF_DocumentFonts::For(CPDF_Document* doc) {
  if (!doc->font_index()) {
    doc->SetFontIndex(std::make_unique<CPDF_DocumentFonts>(doc));
  }
  return static_cast<CPDF_DocumentFonts*>(doc->font_index());
}

// static
CPDF_DocumentFonts* CPDF_DocumentFonts::Find(const CPDF_Document* doc) {
  return doc ? static_cast<CPDF_DocumentFonts*>(doc->font_index()) : nullptr;
}

std::vector<CPDF_DocumentFonts::Face> CPDF_DocumentFonts::CollectFaces() {
  std::vector<Face> faces;
  std::map<ByteString, size_t> face_index;  // family key|weight|italic
  for (const RetainPtr<const CPDF_Dictionary>& font : FontDicts()) {
    const CPDF_FontFace face = FaceOfFontDict(font.Get());
    const ByteString key =
        ByteString::Format("%s|%d|%d", FontFamilyKey(face.family).c_str(),
                           face.weight, face.italic ? 1 : 0);
    auto [found, added] = face_index.emplace(key, faces.size());
    if (added) {
      faces.push_back({face.family, face.weight, face.italic, {}});
    }
    Face& entry = faces[found->second];

    auto [stream, format] = ProgramOf(font.Get());
    const ByteString base_font = ProgramBaseFont(font.Get());
    Program program;
    if (stream) {
      program = Inspect(stream, format);
      program.embedded =
          HasFontSubsetTag(base_font) ? Embedded::kSubset : Embedded::kWhole;
    } else {
      program.format = format;
    }
    program.postscript_name = BaseFontNameWithoutSubsetTag(base_font);
    if (stream) {
      program.display_sha256 = DisplayHashOf(program, font.Get(), face.family,
                                             face.weight, face.italic);
    }
    const bool known =
        std::ranges::any_of(entry.programs, [&](const Program& other) {
          return program.objnum != 0
                     ? other.objnum == program.objnum
                     : other.objnum == 0 && other.format == program.format &&
                           other.sha256 == program.sha256;
        });
    if (!known) {
      entry.programs.push_back(std::move(program));
    }
  }
  return faces;
}

DataVector<uint8_t> CPDF_DocumentFonts::FacesFingerprint() {
  // Every field fixed-width or length-prefixed: two different lists never
  // serialize alike.
  DataVector<uint8_t> bytes;
  auto put = [&](uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
  };
  auto put_bytes = [&](pdfium::span<const uint8_t> data) {
    put(static_cast<uint32_t>(data.size()));
    bytes.insert(bytes.end(), data.begin(), data.end());
  };
  const std::vector<Face> faces = CollectFaces();
  put(static_cast<uint32_t>(faces.size()));
  for (const Face& face : faces) {
    put_bytes(face.family.ToUTF8().unsigned_span());
    put(static_cast<uint32_t>(face.weight));
    put(face.italic);
    put(static_cast<uint32_t>(face.programs.size()));
    for (const Program& program : face.programs) {
      put(program.objnum);
      put(static_cast<uint32_t>(program.format));
      put(static_cast<uint32_t>(program.embedded));
      put_bytes(program.postscript_name.unsigned_span());
      put(program.permission.has_value()
              ? 1 + static_cast<uint32_t>(*program.permission)
              : 0);
      put(program.maps_unicode);
      put(program.tables_aligned);
      put_bytes(program.sha256);
      put_bytes(program.display_sha256);
      put(std::bit_cast<uint32_t>(program.ascent));
      put(std::bit_cast<uint32_t>(program.descent));
    }
  }
  return CryptSha256Generate(bytes);
}

std::vector<RetainPtr<const CPDF_Dictionary>> CPDF_DocumentFonts::FontDicts() {
  CPDF_LayerDocument* layer = CPDF_LayerDocument::FromDocument(doc_.get());
  if (!layer) {
    FontWalk walk(doc_);
    walk.Document();
    ++places_walked_;
    return walk.TakeFonts();
  }
  if (const CPDF_LayerTransaction* write = layer->open_transaction()) {
    // Mid-write: what the write touched so far is read as it is now. The
    // places walked here are forgotten again when the write ends.
    if (write->epoch_at_begin != known_epoch_) {
      ForgetPlaces();
      known_epoch_ = write->epoch_at_begin;
    }
    Forget(write->touched);
  } else if (layer->GetOverlayEpoch() != known_epoch_) {
    // Something changed outside any transaction this index was told of.
    ForgetPlaces();
    known_epoch_ = layer->GetOverlayEpoch();
  }
  auto walk_place = [&](auto&& walk_it) {
    FontWalk walk(doc_);
    walk_it(walk);
    ++places_walked_;
    return Place{walk.TakeFonts(), walk.TakeReads()};
  };
  std::vector<RetainPtr<const CPDF_Dictionary>> fonts;
  std::set<const CPDF_Dictionary*> seen;
  auto take = [&](const Place& place) {
    for (const RetainPtr<const CPDF_Dictionary>& font : place.fonts) {
      if (seen.insert(font.Get()).second) {
        fonts.push_back(font);
      }
    }
  };
  if (!form_place_.has_value()) {
    form_place_ = walk_place([](FontWalk& walk) { walk.Form(); });
  }
  take(*form_place_);
  // Pages a write removed drop out; pages it added are walked.
  std::map<uint32_t, Place> pages;
  const int page_count = doc_->GetPageCount();
  for (int i = 0; i < page_count; ++i) {
    RetainPtr<const CPDF_Dictionary> page = doc_->GetPageDictionary(i);
    if (!page) {
      continue;
    }
    const uint32_t objnum = page->GetObjNum();
    auto kept = page_places_.find(objnum);
    if (objnum != 0 && kept != page_places_.end()) {
      take(kept->second);
      pages.emplace(objnum, std::move(kept->second));
      page_places_.erase(kept);
      continue;
    }
    if (objnum != 0 && pages.contains(objnum)) {
      take(pages[objnum]);
      continue;
    }
    Place place = walk_place([&](FontWalk& walk) { walk.Page(page); });
    take(place);
    if (objnum != 0) {
      pages.emplace(objnum, std::move(place));
    }
  }
  page_places_ = std::move(pages);
  return fonts;
}

void CPDF_DocumentFonts::TransactionEnded(const std::set<uint32_t>& objnums,
                                          uint64_t epoch_at_begin,
                                          uint64_t epoch) {
  if (epoch_at_begin != known_epoch_) {
    // Something changed before the transaction began that this index wasn't
    // told of.
    ForgetPlaces();
  }
  Forget(objnums);
  known_epoch_ = epoch;
}

void CPDF_DocumentFonts::Forget(const std::set<uint32_t>& objnums) {
  if (objnums.empty()) {
    return;
  }
  auto reads_any = [&](const Place& place) {
    return std::ranges::any_of(objnums, [&](uint32_t objnum) {
      return std::ranges::binary_search(place.reads, objnum);
    });
  };
  if (form_place_.has_value() && reads_any(*form_place_)) {
    form_place_.reset();
  }
  std::erase_if(page_places_,
                [&](const auto& entry) { return reads_any(entry.second); });
  for (uint32_t objnum : objnums) {
    inspected_.erase(objnum);
  }
}

void CPDF_DocumentFonts::ForgetPlaces() {
  form_place_.reset();
  page_places_.clear();
}

CPDF_DocumentFonts::Program CPDF_DocumentFonts::Inspect(
    RetainPtr<const CPDF_Stream> stream,
    Format format) {
  const uint32_t objnum = stream->GetObjNum();
  if (objnum != 0) {
    auto it = inspected_.find(objnum);
    if (it != inspected_.end() && it->second.stream == stream &&
        it->second.raw_size == stream->GetRawSize()) {
      return it->second.facts;
    }
  }
  Program facts;
  facts.objnum = objnum;
  facts.format = format;
  CPDF_DocPageData* page_data = CPDF_DocPageData::FromDocument(doc_);
  RetainPtr<CPDF_StreamAcc> data = page_data->GetFontFileStreamAcc(stream);
  if (data && !data->GetSpan().empty()) {
    facts.sha256 = CryptSha256Generate(data->GetSpan());
    if (IsSfnt(format) &&
        CFX_FontRegistry::DetectProgramFormat(data->GetSpan()) ==
            CFX_FontRegistry::ProgramFormat::kOpenTypeCFF) {
      facts.format = Format::kOpenTypeCFF;
    }
    auto program = std::make_unique<CFX_Font>();
    if (program->LoadFaceZeroFromSpan(data->GetSpan(),
                                      /*force_vertical=*/false,
                                      /*object_tag=*/0) &&
        program->HasAnyGlyphs()) {
      ProgramLineMetrics(program.get(), &facts.ascent, &facts.descent);
      if (IsSfnt(facts.format)) {
        RetainPtr<CFX_Face> face = program->GetFace();
        facts.permission =
            CFX_FontRegistry::ClassifyFsType(face->GetFsTypeFlags());
        facts.maps_unicode = face->SelectCharMap(fxge::FontEncoding::kUnicode);
        facts.tables_aligned = SfntTablesAligned(data->GetSpan());
      }
    }
  }
  if (data) {
    page_data->MaybePurgeFontFileStreamAcc(std::move(data));
  }
  // An inline program is inspected again each time: nothing addresses it.
  if (objnum != 0) {
    inspected_[objnum] = {stream, stream->GetRawSize(), facts, std::nullopt};
  }
  return facts;
}

CPDF_DocumentFonts::Inspected* CPDF_DocumentFonts::FindInspected(
    uint32_t objnum) {
  if (objnum == 0) {
    return nullptr;
  }
  auto found = [&]() -> Inspected* {
    auto it = inspected_.find(objnum);
    if (it == inspected_.end()) {
      return nullptr;
    }
    // The version inspected must still be the document's.
    RetainPtr<const CPDF_Object> current = doc_->GetIndirectObject(objnum);
    if (current != it->second.stream ||
        it->second.stream->GetRawSize() != it->second.raw_size) {
      return nullptr;
    }
    return &it->second;
  };
  if (Inspected* inspected = found()) {
    return inspected;
  }
  // Not inspected yet, or changed since: walk again, which inspects what
  // the document uses now.
  CollectFaces();
  return found();
}

DataVector<uint8_t> CPDF_DocumentFonts::ProgramData(uint32_t objnum) {
  Inspected* inspected = FindInspected(objnum);
  if (!inspected || inspected->facts.sha256.empty()) {
    return {};
  }
  return BytesOf(*inspected);
}

DataVector<uint8_t> CPDF_DocumentFonts::BytesOf(const Inspected& inspected) {
  CPDF_DocPageData* page_data = CPDF_DocPageData::FromDocument(doc_);
  RetainPtr<CPDF_StreamAcc> data =
      page_data->GetFontFileStreamAcc(inspected.stream);
  if (!data) {
    return {};
  }
  DataVector<uint8_t> bytes(data->GetSpan().begin(), data->GetSpan().end());
  page_data->MaybePurgeFontFileStreamAcc(std::move(data));
  return bytes;
}

DataVector<uint8_t> CPDF_DocumentFonts::DisplayHashOf(
    const Program& program,
    const CPDF_Dictionary* font_dict,
    const WideString& family,
    int weight,
    bool italic) {
  if (program.sha256.empty()) {
    return {};
  }
  if (LoadsAsIs(program)) {
    return program.sha256;
  }
  // An inline program can't be addressed, so nothing could fetch it.
  if (program.objnum == 0) {
    return {};
  }
  auto it = inspected_.find(program.objnum);
  if (it == inspected_.end()) {
    return {};
  }
  Inspected& inspected = it->second;
  if (!inspected.display_built) {
    inspected.display_built = true;
    if (IsSfnt(program.format) && program.maps_unicode) {
      // Only its tables are out of place: the same program, realigned. Its
      // coverage is its own cmap's (ProgramCoverage).
      DataVector<uint8_t> aligned = AlignSfntTables(BytesOf(inspected));
      if (!aligned.empty()) {
        Display display;
        display.sha256 = CryptSha256Generate(aligned);
        display.bytes = std::move(aligned);
        inspected.display = std::move(display);
      }
    } else {
      std::optional<CPDF_DisplayProgram> built =
          BuildDisplayProgram(doc_, font_dict, program.postscript_name,
                              CPDF_FontFace{family, weight, italic});
      if (built.has_value()) {
        Display display;
        display.sha256 = CryptSha256Generate(built->bytes);
        display.coverage = RangesOf(std::move(built->characters));
        display.bytes = std::move(built->bytes);
        inspected.display = std::move(display);
      }
    }
  }
  return inspected.display.has_value() ? inspected.display->sha256
                                       : DataVector<uint8_t>();
}

DataVector<uint8_t> CPDF_DocumentFonts::DisplayProgramData(uint32_t objnum) {
  Inspected* inspected = FindInspected(objnum);
  if (!inspected) {
    return {};
  }
  if (inspected->display.has_value()) {
    return inspected->display->bytes;
  }
  if (LoadsAsIs(inspected->facts)) {
    return ProgramData(objnum);
  }
  return {};
}

std::vector<CFX_CodePointRange> CPDF_DocumentFonts::ProgramCoverage(
    uint32_t objnum) {
  Inspected* inspected = FindInspected(objnum);
  if (!inspected) {
    return {};
  }
  // A program that maps Unicode is drawn through its own cmap, its tables
  // realigned or not; any other through its display program's.
  if (!inspected->facts.maps_unicode) {
    return inspected->display.has_value() ? inspected->display->coverage
                                          : std::vector<CFX_CodePointRange>();
  }
  if (inspected->coverage.has_value()) {
    return *inspected->coverage;
  }
  std::vector<CFX_CodePointRange> ranges;
  {
    DataVector<uint8_t> bytes = ProgramData(objnum);
    auto program = std::make_unique<CFX_Font>();
    if (!bytes.empty() &&
        program->LoadFaceZeroFromSpan(bytes, /*force_vertical=*/false,
                                      /*object_tag=*/0) &&
        program->GetFace()->SelectCharMap(fxge::FontEncoding::kUnicode)) {
      ranges = program->GetFace()->GetMappedCodePointRanges(
          pdfium::kMaximumSupplementaryCodePoint);
    }
  }
  inspected->coverage = ranges;
  return ranges;
}

RetainPtr<CPDF_Stream> CPDF_DocumentFonts::ProgramStream(uint32_t objnum) {
  Inspected* inspected = FindInspected(objnum);
  if (!inspected) {
    return nullptr;
  }
  return pdfium::WrapRetain(const_cast<CPDF_Stream*>(inspected->stream.Get()));
}

bool CPDF_DocumentFonts::ReserveProgramAlias(uint32_t objnum,
                                             ByteString* alias) {
  if (objnum == 0) {
    return false;
  }
  for (const auto& [reserved, program] : program_aliases_) {
    if (program.objnum == objnum) {
      *alias = reserved;
      return true;
    }
  }
  std::optional<ReservedProgram> found;
  for (const Face& face : CollectFaces()) {
    for (const Program& program : face.programs) {
      if (program.objnum == objnum) {
        found = ReservedProgram{objnum, program.postscript_name, face.family,
                                face.weight, face.italic};
        break;
      }
    }
    if (found.has_value()) {
      break;
    }
  }
  if (!found.has_value()) {
    return false;
  }
  const CPDF_Dictionary* root = doc_->GetRoot();
  RetainPtr<const CPDF_Dictionary> acroform =
      root ? root->GetDictFor("AcroForm") : nullptr;
  RetainPtr<const CPDF_Dictionary> dr =
      acroform ? acroform->GetDictFor("DR") : nullptr;
  RetainPtr<const CPDF_Dictionary> dr_fonts =
      dr ? dr->GetDictFor("Font") : nullptr;
  const ByteString base = ByteString::Format("EDocF%u", objnum);
  ByteString candidate = base;
  for (int suffix = 1;
       (dr_fonts && dr_fonts->KeyExist(candidate.AsStringView())) ||
       program_aliases_.contains(candidate);
       ++suffix) {
    candidate = ByteString::Format("%s_%d", base.c_str(), suffix);
  }
  program_aliases_[candidate] = std::move(*found);
  *alias = candidate;
  return true;
}

std::optional<CPDF_DocumentFonts::ReservedProgram>
CPDF_DocumentFonts::ProgramForAlias(const ByteString& alias) const {
  auto it = program_aliases_.find(alias);
  if (it == program_aliases_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void CPDF_DocumentFonts::NoteMissingCharacter(const WideString& grapheme) {
  if (grapheme.IsEmpty() ||
      missing_characters_.size() >= kMaxMissingCharacters ||
      std::ranges::find(missing_characters_, grapheme) !=
          missing_characters_.end()) {
    return;
  }
  missing_characters_.push_back(grapheme);
}
