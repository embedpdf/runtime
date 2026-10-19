// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_documentfonts.h"

#include <algorithm>
#include <memory>
#include <set>
#include <utility>

#include "core/fdrm/fx_crypt_sha.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfdoc/cpdf_displayprogram.h"
#include "core/fpdfdoc/cpdf_fontface.h"
#include "core/fxcrt/utf16.h"
#include "core/fxge/cfx_face.h"
#include "core/fxge/cfx_font.h"
#include "core/fxge/fx_fontencoding.h"

namespace {

// Deep enough for any real nesting of pages, forms and patterns; a cycle the
// walked set misses (a direct object repeated) stops here.
constexpr int kMaxDepth = 64;

// Every font dictionary the document's drawings can use, each once.
class FontWalk {
 public:
  explicit FontWalk(CPDF_Document* doc) : doc_(doc) {}

  std::vector<RetainPtr<const CPDF_Dictionary>> Run() {
    const CPDF_Dictionary* root = doc_->GetRoot();
    RetainPtr<const CPDF_Dictionary> acroform =
        root ? root->GetDictFor("AcroForm") : nullptr;
    if (acroform) {
      Resources(acroform->GetDictFor("DR").Get(), 0);
      Fields(acroform->GetArrayFor("Fields").Get(), 0);
    }
    const int page_count = doc_->GetPageCount();
    for (int i = 0; i < page_count; ++i) {
      RetainPtr<const CPDF_Dictionary> page = doc_->GetPageDictionary(i);
      if (!page) {
        continue;
      }
      Resources(InheritedResources(page.Get()).Get(), 0);
      RetainPtr<const CPDF_Array> annots = page->GetArrayFor("Annots");
      for (size_t j = 0; annots && j < annots->size(); ++j) {
        Appearance(annots->GetDictAt(j).Get(), 0);
      }
    }
    return std::move(fonts_);
  }

 private:
  // A page's /Resources, or the nearest one up its /Parent chain.
  static RetainPtr<const CPDF_Dictionary> InheritedResources(
      const CPDF_Dictionary* page) {
    std::set<const CPDF_Dictionary*> seen;
    RetainPtr<const CPDF_Dictionary> node(page);
    for (int i = 0; node && i < kMaxDepth && seen.insert(node.Get()).second;
         ++i) {
      if (RetainPtr<const CPDF_Dictionary> resources =
              node->GetDictFor("Resources")) {
        return resources;
      }
      node = node->GetDictFor("Parent");
    }
    return nullptr;
  }

  void Resources(const CPDF_Dictionary* resources, int depth) {
    if (!resources || depth > kMaxDepth || !walked_.insert(resources).second) {
      return;
    }
    if (RetainPtr<const CPDF_Dictionary> fonts =
            resources->GetDictFor("Font")) {
      CPDF_DictionaryLocker locker(fonts);
      for (const auto& [key, value] : locker) {
        Font(value ? ToDictionary(value->GetDirect()) : nullptr, depth);
      }
    }
    if (RetainPtr<const CPDF_Dictionary> xobjects =
            resources->GetDictFor("XObject")) {
      CPDF_DictionaryLocker locker(xobjects);
      for (const auto& [key, value] : locker) {
        RetainPtr<const CPDF_Stream> stream =
            value ? ToStream(value->GetDirect()) : nullptr;
        if (stream && stream->GetDict()->GetNameFor("Subtype") == "Form") {
          FormStream(stream.Get(), depth);
        }
      }
    }
    // Tiling patterns are streams with their own resources; shading
    // patterns are dictionaries and draw no text.
    if (RetainPtr<const CPDF_Dictionary> patterns =
            resources->GetDictFor("Pattern")) {
      CPDF_DictionaryLocker locker(patterns);
      for (const auto& [key, value] : locker) {
        RetainPtr<const CPDF_Stream> stream =
            value ? ToStream(value->GetDirect()) : nullptr;
        FormStream(stream.Get(), depth);
      }
    }
    if (RetainPtr<const CPDF_Dictionary> states =
            resources->GetDictFor("ExtGState")) {
      CPDF_DictionaryLocker locker(states);
      for (const auto& [key, value] : locker) {
        RetainPtr<const CPDF_Dictionary> state =
            value ? ToDictionary(value->GetDirect()) : nullptr;
        RetainPtr<const CPDF_Dictionary> mask =
            state ? state->GetDictFor("SMask") : nullptr;
        FormStream(mask ? mask->GetStreamFor("G").Get() : nullptr, depth);
      }
    }
  }

  void FormStream(const CPDF_Stream* stream, int depth) {
    if (!stream || depth > kMaxDepth || !walked_.insert(stream).second) {
      return;
    }
    Resources(stream->GetDict()->GetDictFor("Resources").Get(), depth + 1);
  }

  void Font(RetainPtr<const CPDF_Dictionary> font, int depth) {
    if (!font || !font_set_.insert(font.Get()).second) {
      return;
    }
    if (font->GetNameFor("Subtype") == "Type3") {
      // A Type 3 glyph is a drawing, and may draw text in other fonts.
      Resources(font->GetDictFor("Resources").Get(), depth + 1);
    }
    fonts_.push_back(std::move(font));
  }

  // Every state of the normal, rollover and down appearances.
  void Appearance(const CPDF_Dictionary* annot, int depth) {
    RetainPtr<const CPDF_Dictionary> ap =
        annot ? annot->GetDictFor("AP") : nullptr;
    if (!ap) {
      return;
    }
    for (const char* key : {"N", "R", "D"}) {
      RetainPtr<const CPDF_Object> entry = ap->GetDirectObjectFor(key);
      if (const CPDF_Stream* stream = entry ? entry->AsStream() : nullptr) {
        FormStream(stream, depth);
        continue;
      }
      const CPDF_Dictionary* states = entry ? entry->AsDictionary() : nullptr;
      if (!states) {
        continue;
      }
      CPDF_DictionaryLocker locker(states);
      for (const auto& [state, value] : locker) {
        RetainPtr<const CPDF_Stream> stream =
            value ? ToStream(value->GetDirect()) : nullptr;
        FormStream(stream.Get(), depth);
      }
    }
  }

  // Fields' own /DR, down their /Kids.
  void Fields(const CPDF_Array* fields, int depth) {
    for (size_t i = 0; fields && depth <= kMaxDepth && i < fields->size();
         ++i) {
      RetainPtr<const CPDF_Dictionary> field = fields->GetDictAt(i);
      if (!field || !walked_.insert(field.Get()).second) {
        continue;
      }
      Resources(field->GetDictFor("DR").Get(), depth + 1);
      Fields(field->GetArrayFor("Kids").Get(), depth + 1);
    }
  }

  UnownedPtr<CPDF_Document> const doc_;
  std::set<const CPDF_Object*> walked_;
  std::set<const CPDF_Dictionary*> font_set_;
  std::vector<RetainPtr<const CPDF_Dictionary>> fonts_;
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

// |scalars| as [start, start + count) ranges, in order.
std::vector<std::pair<uint32_t, uint32_t>> RangesOf(
    std::vector<uint32_t> scalars) {
  std::ranges::sort(scalars);
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  for (uint32_t scalar : scalars) {
    if (!ranges.empty() &&
        ranges.back().first + ranges.back().second >= scalar) {
      ranges.back().second =
          std::max(ranges.back().second, scalar - ranges.back().first + 1);
      continue;
    }
    ranges.emplace_back(scalar, 1);
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
  for (const RetainPtr<const CPDF_Dictionary>& font : FontWalk(doc_).Run()) {
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
  CPDF_DocPageData* page_data = CPDF_DocPageData::FromDocument(doc_);
  RetainPtr<CPDF_StreamAcc> data =
      page_data->GetFontFileStreamAcc(inspected->stream);
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
  // A browser loads it as it is.
  if (IsSfnt(program.format) && program.maps_unicode) {
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
  if (IsSfnt(inspected->facts.format) && inspected->facts.maps_unicode) {
    return ProgramData(objnum);
  }
  return {};
}

std::vector<std::pair<uint32_t, uint32_t>> CPDF_DocumentFonts::ProgramCoverage(
    uint32_t objnum) {
  Inspected* inspected = FindInspected(objnum);
  if (!inspected) {
    return {};
  }
  if (inspected->display.has_value()) {
    return inspected->display->coverage;
  }
  if (!inspected->facts.maps_unicode) {
    return {};
  }
  if (inspected->coverage.has_value()) {
    return *inspected->coverage;
  }
  std::vector<uint32_t> scalars;
  {
    DataVector<uint8_t> bytes = ProgramData(objnum);
    auto program = std::make_unique<CFX_Font>();
    if (!bytes.empty() &&
        program->LoadFaceZeroFromSpan(bytes, /*force_vertical=*/false,
                                      /*object_tag=*/0) &&
        program->GetFace()->SelectCharMap(fxge::FontEncoding::kUnicode)) {
      for (const CharCodeAndIndex& item : program->GetCharCodesAndIndices(
               pdfium::kMaximumSupplementaryCodePoint)) {
        if (item.glyph_index != 0) {
          scalars.push_back(item.char_code);
        }
      }
    }
  }
  std::vector<std::pair<uint32_t, uint32_t>> ranges =
      RangesOf(std::move(scalars));
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
