// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: annotation font map for registered runtime fonts. This lets
// FreeText appearance generation pick per-glyph fallback fonts and later embed
// only the glyph subset used by the annotation/layer.

#include "core/fpdfdoc/cpdf_annotfontmap.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_indirect_object_holder.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfdoc/cpdf_annotfontsubset.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/code_point_view.h"
#include "core/fxcrt/fx_codepage.h"
#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxge/cfx_face.h"
#include "core/fxge/cfx_font.h"
#include "core/fxge/fx_font.h"
#include "core/fxge/fx_fontencoding.h"

namespace {

constexpr char kRegisteredFontResourcePrefix[] = "ERegF";

// Test-only injection, see FailAfterStagedFontsForTesting().
int g_fail_after_staged_fonts_for_testing = 0;

ByteString ResourceKeyForRegisteredFont(CFX_FontRegistry::FontId font_id) {
  return ByteString::Format("%s%u", kRegisteredFontResourcePrefix, font_id);
}

// /AcroForm /DR /Font as it is, or nullptr; creates nothing.
RetainPtr<const CPDF_Dictionary> GetDrFontDict(const CPDF_Document* doc) {
  const CPDF_Dictionary* root_dict = doc->GetRoot();
  RetainPtr<const CPDF_Dictionary> acroform_dict =
      root_dict ? root_dict->GetDictFor("AcroForm") : nullptr;
  RetainPtr<const CPDF_Dictionary> dr_dict =
      acroform_dict ? acroform_dict->GetDictFor("DR") : nullptr;
  return dr_dict ? dr_dict->GetDictFor("Font") : nullptr;
}

RetainPtr<CPDF_Dictionary> GetOrCreateDrFontDict(CPDF_Document* doc) {
  RetainPtr<CPDF_Dictionary> root_dict = doc->GetMutableRoot();
  if (!root_dict) {
    return nullptr;
  }
  RetainPtr<CPDF_Dictionary> acroform_dict =
      root_dict->GetMutableDictFor("AcroForm");
  if (!acroform_dict) {
    acroform_dict = CPDF_InteractiveForm::InitAcroFormDict(doc);
    CHECK(acroform_dict);
  }
  return acroform_dict->GetOrCreateDictFor("DR")->GetOrCreateDictFor("Font");
}

// ---- Rich text helpers ------------------------------------------------------

// Family names compare without case, spaces, hyphens and underscores:
// "Noto Sans" == "NotoSans" == "noto-sans".
ByteString FamilyKey(const WideString& family) {
  ByteString key;
  for (wchar_t ch : family) {
    if (ch == L' ' || ch == L'-' || ch == L'_' || ch == L'\'' || ch == L'"') {
      continue;
    }
    if (ch < 0x80) {
      key += static_cast<char>(std::tolower(static_cast<int>(ch)));
    } else {
      key += WideString(ch).ToUTF8();
    }
  }
  return key;
}

ByteString FamilyKey(const ByteString& family) {
  return FamilyKey(WideString::FromUTF8(family.AsStringView()));
}

// The standard-14 family a rich text family stands for, if any.
enum class StandardFamily {
  kNone,
  kHelvetica,
  kTimes,
  kCourier,
  kSymbol,
  kZapfDingbats
};

StandardFamily StandardFamilyFor(const WideString& family) {
  const ByteString key = FamilyKey(family);
  if (key == "helvetica" || key == "arial" || key == "arialmt" ||
      key == "sansserif") {
    return StandardFamily::kHelvetica;
  }
  if (key == "times" || key == "timesroman" || key == "timesnewroman" ||
      key == "timesnewromanpsmt" || key == "timesnewromanps" ||
      key == "serif") {
    return StandardFamily::kTimes;
  }
  if (key == "courier" || key == "couriernew" || key == "couriernewpsmt" ||
      key == "monospace") {
    return StandardFamily::kCourier;
  }
  if (key == "symbol") {
    return StandardFamily::kSymbol;
  }
  if (key == "zapfdingbats" || key == "dingbats") {
    return StandardFamily::kZapfDingbats;
  }
  return StandardFamily::kNone;
}

ByteString StandardBaseFontName(StandardFamily family,
                                int weight,
                                bool italic) {
  const bool bold = weight >= 600;
  switch (family) {
    case StandardFamily::kHelvetica:
      return bold ? (italic ? "Helvetica-BoldOblique" : "Helvetica-Bold")
                  : (italic ? "Helvetica-Oblique" : "Helvetica");
    case StandardFamily::kTimes:
      return bold ? (italic ? "Times-BoldItalic" : "Times-Bold")
                  : (italic ? "Times-Italic" : "Times-Roman");
    case StandardFamily::kCourier:
      return bold ? (italic ? "Courier-BoldOblique" : "Courier-Bold")
                  : (italic ? "Courier-Oblique" : "Courier");
    case StandardFamily::kSymbol:
      return "Symbol";
    case StandardFamily::kZapfDingbats:
      return "ZapfDingbats";
    case StandardFamily::kNone:
      break;
  }
  return ByteString();
}

// Acrobat's resource names for the standard 14, so a box we author reads
// like one Acrobat wrote.
ByteString StandardAliasFor(const ByteString& base_font_name) {
  struct Entry {
    const char* base_font;
    const char* alias;
  };
  static constexpr Entry kAliases[] = {
      {"Helvetica", "Helv"},
      {"Helvetica-Bold", "HeBo"},
      {"Helvetica-Oblique", "HeOb"},
      {"Helvetica-BoldOblique", "HeBO"},
      {"Times-Roman", "TiRo"},
      {"Times-Bold", "TiBo"},
      {"Times-Italic", "TiIt"},
      {"Times-BoldItalic", "TiBI"},
      {"Courier", "Cour"},
      {"Courier-Bold", "CoBo"},
      {"Courier-Oblique", "CoOb"},
      {"Courier-BoldOblique", "CoBO"},
      {"Symbol", "Symb"},
      {"ZapfDingbats", "ZaDb"},
  };
  for (const Entry& entry : kAliases) {
    if (base_font_name == entry.base_font) {
      return entry.alias;
    }
  }
  return "FXF_" + base_font_name;
}

// Acrobat's line metrics for the standard 14 (plan §6, experiments 4 and
// 11): the hhea values of its bundled substitutes, ascent + descent == 1.
void StandardMetrics(const ByteString& base_font_name,
                     const CPDF_Font* font,
                     float* ascent,
                     float* descent) {
  if (base_font_name.First(9) == "Helvetica") {
    *ascent = 0.830f;
    *descent = 0.170f;
    return;
  }
  if (base_font_name.First(5) == "Times") {
    *ascent = 0.784f;
    *descent = 0.216f;
    return;
  }
  if (base_font_name.First(7) == "Courier") {
    *ascent = 0.627f;
    *descent = 0.373f;
    return;
  }
  // Symbol and ZapfDingbats: the descriptor values, else the face's.
  int asc = font ? font->GetTypeAscent() : 0;
  int desc = font ? font->GetTypeDescent() : 0;
  if (asc == 0 && desc == 0 && font) {
    asc = font->GetFont()->GetAscent();
    desc = font->GetFont()->GetDescent();
  }
  if (asc <= 0) {
    *ascent = 0.830f;
    *descent = 0.170f;
    return;
  }
  *ascent = asc / 1000.0f;
  *descent = std::abs(desc) / 1000.0f;
}

// hhea ascender/descender of an embedded program (Acrobat's choice for
// TrueType and CFF, plan §6).
void ProgramMetrics(const CFX_Font* font, float* ascent, float* descent) {
  RetainPtr<CFX_Face> face = font ? font->GetFace() : nullptr;
  const uint16_t upem = face ? face->GetUnitsPerEm() : 0;
  if (!face || upem == 0) {
    *ascent = 0.830f;
    *descent = 0.170f;
    return;
  }
  *ascent = static_cast<float>(face->GetAscender()) / upem;
  *descent = static_cast<float>(-face->GetDescender()) / upem;
  if (*ascent <= 0 || *descent < 0) {
    *ascent = 0.830f;
    *descent = 0.170f;
  }
}

RetainPtr<const CPDF_Dictionary> DescriptorOfFontDict(
    const CPDF_Dictionary* font_dict) {
  if (!font_dict) {
    return nullptr;
  }
  RetainPtr<const CPDF_Dictionary> descriptor =
      font_dict->GetDictFor("FontDescriptor");
  if (descriptor) {
    return descriptor;
  }
  RetainPtr<const CPDF_Array> descendants =
      font_dict->GetArrayFor("DescendantFonts");
  RetainPtr<const CPDF_Dictionary> cid_font =
      descendants ? descendants->GetDictAt(0) : nullptr;
  return cid_font ? cid_font->GetDictFor("FontDescriptor") : nullptr;
}

// "ABCDEF+NotoSans": a producer's subset, cut for the text it was used for.
bool HasSubsetTag(const ByteString& base_font) {
  return base_font.GetLength() > 7 && base_font[6] == '+';
}

// "ABCDEF+NotoSans-Bold" -> "NotoSans"; "Arial,Bold" -> "Arial".
ByteString FamilyFromBaseFont(ByteString base_font) {
  if (base_font.GetLength() > 7 && base_font[6] == '+') {
    base_font = base_font.Substr(7);
  }
  std::optional<size_t> cut = base_font.Find('-');
  std::optional<size_t> comma = base_font.Find(',');
  if (comma.has_value() && (!cut.has_value() || *comma < *cut)) {
    cut = comma;
  }
  return cut.has_value() ? base_font.First(*cut) : base_font;
}

struct DocumentProgramCandidate {
  RetainPtr<CPDF_Stream> stream;
  ByteString base_font_name;
  CPDF_AnnotFontSubset::FaceIdentity identity;
  int score = 0;
};

// Reads what a font dictionary says about itself: family, weight, italic,
// and the sfnt program stream if it has one we can use.
std::optional<DocumentProgramCandidate> CandidateFromFontDict(
    const CPDF_Dictionary* font_dict) {
  RetainPtr<const CPDF_Dictionary> descriptor = DescriptorOfFontDict(font_dict);
  if (!descriptor) {
    return std::nullopt;
  }
  RetainPtr<CPDF_Stream> stream;
  if (RetainPtr<const CPDF_Stream> file2 =
          descriptor->GetStreamFor("FontFile2")) {
    stream = pdfium::WrapRetain(const_cast<CPDF_Stream*>(file2.Get()));
  } else if (RetainPtr<const CPDF_Stream> file3 =
                 descriptor->GetStreamFor("FontFile3")) {
    if (file3->GetDict()->GetNameFor("Subtype") == "OpenType") {
      stream = pdfium::WrapRetain(const_cast<CPDF_Stream*>(file3.Get()));
    }
  }
  if (!stream || stream->GetObjNum() == 0) {
    return std::nullopt;  // Type1, bare CFF, or an inline stream: not ours
  }
  DocumentProgramCandidate candidate;
  candidate.stream = std::move(stream);
  candidate.base_font_name = font_dict->GetNameFor("BaseFont");
  ByteString lowered_name = candidate.base_font_name;
  lowered_name.MakeLower();
  WideString family = descriptor->GetUnicodeTextFor("FontFamily");
  if (family.IsEmpty()) {
    family = WideString::FromUTF8(
        FamilyFromBaseFont(candidate.base_font_name).AsStringView());
  }
  candidate.identity.family = family.ToUTF8();
  const int flags = descriptor->GetIntegerFor("Flags", 0);
  if (descriptor->KeyExist("FontWeight")) {
    candidate.identity.weight = descriptor->GetIntegerFor("FontWeight", 400);
  } else if ((flags & pdfium::kFontStyleForceBold) ||
             lowered_name.Contains("bold")) {
    candidate.identity.weight = 700;
  }
  candidate.identity.italic =
      descriptor->GetIntegerFor("ItalicAngle", 0) != 0 ||
      (flags & pdfium::kFontStyleItalic) != 0 ||
      lowered_name.Contains("italic") || lowered_name.Contains("oblique");
  return candidate;
}

void CollectFontDicts(const CPDF_Dictionary* font_resources,
                      std::vector<RetainPtr<const CPDF_Dictionary>>* out) {
  if (!font_resources) {
    return;
  }
  CPDF_DictionaryLocker locker(font_resources);
  for (const auto& [key, value] : locker) {
    RetainPtr<const CPDF_Dictionary> dict =
        value ? ToDictionary(value->GetDirect()) : nullptr;
    if (dict) {
      out->push_back(std::move(dict));
    }
  }
}

// Every font dictionary a program could be borrowed from: /AcroForm /DR,
// each page's resources, and each page's annotation appearances.
std::vector<RetainPtr<const CPDF_Dictionary>> DocumentFontDicts(
    CPDF_Document* doc) {
  std::vector<RetainPtr<const CPDF_Dictionary>> dicts;
  const CPDF_Dictionary* root = doc->GetRoot();
  RetainPtr<const CPDF_Dictionary> acroform =
      root ? root->GetDictFor("AcroForm") : nullptr;
  RetainPtr<const CPDF_Dictionary> dr =
      acroform ? acroform->GetDictFor("DR") : nullptr;
  CollectFontDicts(dr ? dr->GetDictFor("Font").Get() : nullptr, &dicts);
  const int page_count = doc->GetPageCount();
  for (int i = 0; i < page_count; ++i) {
    RetainPtr<const CPDF_Dictionary> page = doc->GetPageDictionary(i);
    if (!page) {
      continue;
    }
    RetainPtr<const CPDF_Dictionary> resources = page->GetDictFor("Resources");
    CollectFontDicts(resources ? resources->GetDictFor("Font").Get() : nullptr,
                     &dicts);
    RetainPtr<const CPDF_Array> annots = page->GetArrayFor("Annots");
    for (size_t j = 0; annots && j < annots->size(); ++j) {
      RetainPtr<const CPDF_Dictionary> annot = annots->GetDictAt(j);
      RetainPtr<const CPDF_Dictionary> ap =
          annot ? annot->GetDictFor("AP") : nullptr;
      RetainPtr<const CPDF_Stream> normal =
          ap ? ap->GetStreamFor("N") : nullptr;
      RetainPtr<const CPDF_Dictionary> ap_resources =
          normal ? normal->GetDict()->GetDictFor("Resources") : nullptr;
      CollectFontDicts(
          ap_resources ? ap_resources->GetDictFor("Font").Get() : nullptr,
          &dicts);
    }
  }
  return dicts;
}

bool PDFontSupportsUnicode(const RetainPtr<CPDF_Font>& font, uint16_t word) {
  if (!font) {
    return false;
  }

  uint32_t charcode = font->CharCodeFromUnicode(word);
  if (charcode == CPDF_Font::kInvalidCharCode || (charcode == 0 && word != 0)) {
    return false;
  }

  bool vert_glyph = false;
  return font->GlyphFromCharCode(charcode, &vert_glyph) > 0;
}

}  // namespace

CPDF_AnnotFontMap::FontEntry::FontEntry() = default;

CPDF_AnnotFontMap::FontEntry::FontEntry(FontEntry&& that) noexcept = default;

CPDF_AnnotFontMap::FontEntry& CPDF_AnnotFontMap::FontEntry::operator=(
    FontEntry&& that) noexcept = default;

CPDF_AnnotFontMap::FontEntry::~FontEntry() = default;

CPDF_AnnotFontMap::PreparedFontResources::PreparedFontResources() = default;

CPDF_AnnotFontMap::PreparedFontResources::PreparedFontResources(
    PreparedFontResources&& that) noexcept = default;

CPDF_AnnotFontMap::PreparedFontResources&
CPDF_AnnotFontMap::PreparedFontResources::operator=(
    PreparedFontResources&& that) noexcept = default;

CPDF_AnnotFontMap::PreparedFontResources::~PreparedFontResources() = default;

CPDF_AnnotFontMap::CPDF_AnnotFontMap(
    CPDF_Document* doc,
    RetainPtr<CPDF_Font> default_font,
    const ByteString& default_font_alias,
    bool allow_registered_fallbacks,
    CFX_FontRegistry::FontId registered_font_id,
    bool install_dr_entry,
    Owner owner)
    : doc_(doc),
      allow_registered_fallbacks_(allow_registered_fallbacks),
      install_dr_entry_(install_dr_entry),
      owner_(owner) {
  FontEntry entry;
  entry.font = std::move(default_font);
  entry.alias = default_font_alias;

  std::optional<CFX_FontRegistry::FontId> font_id;
  if (CFX_FontRegistry::IsValidFont(registered_font_id)) {
    font_id = registered_font_id;
  } else if (entry.font) {
    // EmbedPDF: a /DR font written by us resolves by family first, then by
    // the session hint, so a document reopened with the fonts registered in
    // another order still lays out with the right face.
    font_id = CPDF_AnnotFontSubset::ResolveRegisteredFont(
        entry.font->GetFontDict().Get());
  }
  if (font_id.has_value()) {
    RetainPtr<CPDF_Font> registered_font = CreateRegisteredLayoutFont(*font_id);
    if (registered_font) {
      entry.font = std::move(registered_font);
      entry.registered_font_id = *font_id;
      entry.source = Source::kRegistered;
      ProgramMetrics(entry.font->GetFont(), &entry.ascent, &entry.descent);
    }
  }
  if (entry.source == Source::kDocumentFontDict && entry.font) {
    if (entry.font->IsStandardFont()) {
      StandardMetrics(entry.font->GetBaseFontName(), entry.font.Get(),
                      &entry.ascent, &entry.descent);
    } else {
      ProgramMetrics(entry.font->GetFont(), &entry.ascent, &entry.descent);
    }
  }
  fonts_.push_back(std::move(entry));
}

CPDF_AnnotFontMap::~CPDF_AnnotFontMap() {
  ReleaseLayoutFonts();
}

// static
bool CPDF_AnnotFontMap::ChooseRegisteredFontAlias(
    const CPDF_Document* doc,
    CFX_FontRegistry::FontId font_id,
    ByteString* resource_key) {
  if (!doc || !resource_key || !CFX_FontRegistry::IsValidFont(font_id)) {
    return false;
  }

  ByteString key = ResourceKeyForRegisteredFont(font_id);
  RetainPtr<const CPDF_Dictionary> font_res = GetDrFontDict(doc);
  if (!font_res) {
    *resource_key = key;  // nothing to collide with
    return true;
  }

  if (RetainPtr<const CPDF_Dictionary> existing_font_dict =
          font_res->GetDictFor(key.AsStringView())) {
    // EmbedPDF: identity lives in the dictionary (family, then hint), not in
    // the alias, so it survives alias suffixes and resource renaming during
    // save/merge. A legacy marker for the same font reuses its key and is
    // upgraded to the real subset when the appearance is generated.
    if (CPDF_AnnotFontSubset::ResolveRegisteredFont(existing_font_dict.Get()) ==
        font_id) {
      *resource_key = key;
      return true;
    }
  }

  const ByteString base_key = key;
  for (int suffix = 1; font_res->KeyExist(key.AsStringView()); ++suffix) {
    key = ByteString::Format("%s_%d", base_key.c_str(), suffix);
  }
  *resource_key = key;
  return true;
}

// static
bool CPDF_AnnotFontMap::ReserveRegisteredFontAlias(
    CPDF_Document* doc,
    CFX_FontRegistry::FontId font_id,
    ByteString* resource_key) {
  ByteString key;
  if (!ChooseRegisteredFontAlias(doc, font_id, &key)) {
    return false;
  }
  // The appearance generators expect /AcroForm /DR to exist once a /DA
  // names a font; the real entry under |key| arrives with the appearance.
  if (!GetOrCreateDrFontDict(doc)) {
    return false;
  }
  doc->ReserveSessionFontAlias(key, font_id);
  *resource_key = key;
  return true;
}

// static
std::optional<CFX_FontRegistry::FontId>
CPDF_AnnotFontMap::RegisteredFontIdFromAlias(const CPDF_Document* doc,
                                             const ByteString& alias) {
  if (!doc) {
    return std::nullopt;
  }
  std::optional<uint32_t> font_id = doc->LookupSessionFontAlias(alias);
  if (!font_id.has_value() || !CFX_FontRegistry::IsValidFont(*font_id)) {
    return std::nullopt;
  }
  return *font_id;
}

ByteString CPDF_AnnotFontMap::AllocateAppearanceAlias(
    const ByteString& preferred) const {
  // Resource names are per appearance. The /DA alias may be stale (a
  // registration order from another session), so a fallback's preferred
  // "ERegF<id>" can collide with it; never let two entries share a name.
  auto occupied = [&](const ByteString& candidate) {
    return std::ranges::any_of(fonts_, [&](const FontEntry& entry) {
      return entry.alias == candidate;
    });
  };
  ByteString candidate = preferred;
  for (uint32_t suffix = 1; occupied(candidate); ++suffix) {
    candidate = ByteString::Format("%s_%u", preferred.c_str(), suffix);
  }
  return candidate;
}

bool CPDF_AnnotFontMap::HasDefaultFont() const {
  return !fonts_.empty() && fonts_.front().font;
}

bool CPDF_AnnotFontMap::DefaultFontIsStandard() const {
  return HasDefaultFont() && fonts_.front().font->IsStandardFont();
}

CPDF_AnnotFontSubset::Embedding CPDF_AnnotFontMap::EmbeddingForRegisteredFonts()
    const {
  // §2 of the Phase C note: the document's policy, with DEFAULT meaning
  // subset for annotation text and the whole program for form field text.
  switch (doc_ ? doc_->GetFontEmbeddingPolicy()
               : CPDF_Document::FontEmbeddingPolicy::kDefault) {
    case CPDF_Document::FontEmbeddingPolicy::kSubset:
      return CPDF_AnnotFontSubset::Embedding::kSubset;
    case CPDF_Document::FontEmbeddingPolicy::kFull:
      return CPDF_AnnotFontSubset::Embedding::kFull;
    case CPDF_Document::FontEmbeddingPolicy::kDefault:
      return owner_ == Owner::kWidget
                 ? CPDF_AnnotFontSubset::Embedding::kFull
                 : CPDF_AnnotFontSubset::Embedding::kSubset;
  }
}

// static
void CPDF_AnnotFontMap::FailAfterStagedFontsForTesting(int staged_count) {
  g_fail_after_staged_fonts_for_testing = staged_count;
}

void CPDF_AnnotFontMap::InstallDrEntry(const ByteString& alias,
                                       const CPDF_Dictionary* font_dict) {
  if (!doc_ || alias.IsEmpty() || !font_dict || font_dict->GetObjNum() == 0) {
    return;
  }
  RetainPtr<CPDF_Dictionary> font_res = GetOrCreateDrFontDict(doc_);
  if (!font_res) {
    return;
  }

  // EmbedPDF (A1): /DR names the real embedded font the appearance uses.
  // The last generated appearance wins. Only this reference is replaced: the
  // object it pointed at (an older subset, or a pre-A1 marker) may be
  // referenced from elsewhere, and unreachable-object cleanup belongs to
  // document-wide save/collection, not to a local edit.
  RetainPtr<const CPDF_Dictionary> existing_dict =
      font_res->GetDictFor(alias.AsStringView());
  if (existing_dict.Get() == font_dict) {
    return;
  }
  font_res->SetNewFor<CPDF_Reference>(alias, doc_, font_dict->GetObjNum());
}

std::optional<CPDF_AnnotFontMap::PreparedFontResources>
CPDF_AnnotFontMap::PrepareFontResources() {
  if (!doc_) {
    return std::nullopt;
  }

  PreparedFontResources prepared;
  prepared.font_resources = doc_->New<CPDF_Dictionary>();
  const CPDF_AnnotFontSubset::Embedding embedding =
      EmbeddingForRegisteredFonts();
  int staged_count = 0;
  for (size_t i = 0; i < fonts_.size(); ++i) {
    const FontEntry& entry = fonts_[i];
    if ((!entry.font && !entry.program) || entry.alias.IsEmpty()) {
      continue;
    }

    if (entry.source == Source::kRegistered ||
        entry.source == Source::kDocumentProgram) {
      // The /DA font's alias is what /DA names in /DR, so it needs a
      // resource even when no glyph of it was drawn (empty text, or text
      // drawn entirely by fallback fonts). So does any font the content
      // names, even for glyph 0 alone: an appearance never names a font
      // that is not there. Only the /DA font is installed in /DR.
      const bool is_da_font =
          static_cast<int>(i) == da_entry_ && install_dr_entry_;
      const bool required = is_da_font || entry.named;
      if (g_fail_after_staged_fonts_for_testing > 0 &&
          staged_count >= g_fail_after_staged_fonts_for_testing) {
        return std::nullopt;  // the injected failure of the next resource
      }
      PreparedFontResources::StagedEntry staged;
      const CPDF_AnnotFontSubset::StageStatus status =
          entry.source == Source::kRegistered
              ? CPDF_AnnotFontSubset::StageRegisteredFontResource(
                    entry.registered_font_id, entry.glyph_to_unicode, required,
                    embedding, &staged.resource)
              : CPDF_AnnotFontSubset::StageDocumentProgramResource(
                    entry.program.get(), entry.program_stream,
                    entry.base_font_name, entry.identity,
                    entry.glyph_to_unicode, required, &staged.resource);
      switch (status) {
        case CPDF_AnnotFontSubset::StageStatus::kUnused:
          continue;  // a fallback no glyph of which was drawn
        case CPDF_AnnotFontSubset::StageStatus::kFailed:
          // The /DA font, or a font the appearance bytes name for glyphs it
          // drew: without its resource the appearance would be broken.
          return std::nullopt;
        case CPDF_AnnotFontSubset::StageStatus::kStaged:
          break;
      }
      staged.alias = entry.alias;
      staged.install_in_dr = is_da_font;
      prepared.registered.push_back(std::move(staged));
      ++staged_count;
      continue;
    }

    RetainPtr<const CPDF_Dictionary> font_dict = entry.font->GetFontDict();
    if (!font_dict) {
      continue;
    }

    const uint32_t font_obj_num = font_dict->GetObjNum();
    if (font_obj_num != 0) {
      prepared.font_resources->SetNewFor<CPDF_Reference>(entry.alias, doc_,
                                                         font_obj_num);
    } else {
      prepared.font_resources->SetFor(entry.alias, font_dict->Clone());
    }
  }
  return prepared;
}

RetainPtr<CPDF_Dictionary> CPDF_AnnotFontMap::PublishFontResources(
    PreparedFontResources prepared) {
  if (!doc_ || !prepared.font_resources) {
    return nullptr;
  }
  bool da_published = false;
  for (PreparedFontResources::StagedEntry& staged : prepared.registered) {
    RetainPtr<CPDF_Dictionary> font_resource =
        CPDF_AnnotFontSubset::PublishStagedFontResource(
            doc_, std::move(staged.resource));
    CHECK(font_resource);  // a staged resource is complete by construction
    prepared.font_resources->SetNewFor<CPDF_Reference>(
        staged.alias, doc_, font_resource->GetObjNum());
    if (staged.install_in_dr) {
      InstallDefaultAppearanceDrEntry(font_resource.Get());
      da_published = true;
    }
  }
  if (!da_published && install_dr_entry_ &&
      fxcrt::IndexInBounds(fonts_, da_entry_) &&
      fonts_[da_entry_].source == Source::kStandard14) {
    InstallDefaultAppearanceDrEntry(nullptr);
  }
  return prepared.font_resources;
}

void CPDF_AnnotFontMap::InstallDefaultAppearanceDrEntry(
    const CPDF_Dictionary* published) {
  if (!fxcrt::IndexInBounds(fonts_, da_entry_)) {
    return;
  }
  FontEntry& entry = fonts_[da_entry_];
  switch (entry.source) {
    case Source::kRegistered: {
      // C note §6: the alias is reserved at Publish, never at Prepare. The
      // key chosen at Prepare is the one the DA string names.
      ByteString key;
      if (ReserveRegisteredFontAlias(doc_, entry.registered_font_id, &key) &&
          key != entry.alias) {
        doc_->ReserveSessionFontAlias(entry.alias, entry.registered_font_id);
      }
      InstallDrEntry(entry.alias, published);
      return;
    }
    case Source::kDocumentProgram:
      InstallDrEntry(entry.alias, published);
      return;
    case Source::kStandard14: {
      RetainPtr<CPDF_Dictionary> font_res = GetOrCreateDrFontDict(doc_);
      if (!font_res || entry.alias.IsEmpty()) {
        return;
      }
      RetainPtr<const CPDF_Dictionary> existing =
          font_res->GetDictFor(entry.alias.AsStringView());
      if (existing &&
          existing->GetNameFor("BaseFont") == entry.base_font_name) {
        return;  // the key ChooseDefaultAppearanceAlias found
      }
      RetainPtr<CPDF_Dictionary> dr_font = doc_->NewIndirect<CPDF_Dictionary>();
      dr_font->SetNewFor<CPDF_Name>("Type", "Font");
      dr_font->SetNewFor<CPDF_Name>("Subtype", "Type1");
      dr_font->SetNewFor<CPDF_Name>("BaseFont", entry.base_font_name);
      if (entry.base_font_name != "Symbol" &&
          entry.base_font_name != "ZapfDingbats") {
        dr_font->SetNewFor<CPDF_Name>("Encoding", "WinAnsiEncoding");
      }
      font_res->SetNewFor<CPDF_Reference>(entry.alias, doc_,
                                          dr_font->GetObjNum());
      return;
    }
    case Source::kDocumentFontDict:
      return;  // already in /DR by definition
  }
}

RetainPtr<CPDF_Dictionary> CPDF_AnnotFontMap::CreateFontResourceDict() {
  std::optional<PreparedFontResources> prepared = PrepareFontResources();
  if (!prepared.has_value()) {
    return nullptr;  // never leave an appearance naming a font not there
  }
  return PublishFontResources(std::move(*prepared));
}

RetainPtr<CPDF_Font> CPDF_AnnotFontMap::GetPDFFont(int32_t font_index) {
  return fxcrt::IndexInBounds(fonts_, font_index) ? fonts_[font_index].font
                                                  : nullptr;
}

ByteString CPDF_AnnotFontMap::GetPDFFontAlias(int32_t font_index) {
  if (!fxcrt::IndexInBounds(fonts_, font_index)) {
    return ByteString();
  }
  // The appearance names this alias, so its resource must exist (C note
  // §6) even when every glyph it drew was glyph 0.
  fonts_[font_index].named = true;
  return fonts_[font_index].alias;
}

int32_t CPDF_AnnotFontMap::GetWordFontIndex(uint16_t word,
                                            FX_Charset charset,
                                            int32_t font_index) {
  if (SupportsWord(font_index, word)) {
    return font_index;
  }
  if (SupportsWord(0, word)) {
    return 0;
  }

  for (size_t i = 1; i < fonts_.size(); ++i) {
    if (SupportsWord(pdfium::checked_cast<int32_t>(i), word)) {
      return pdfium::checked_cast<int32_t>(i);
    }
  }

  if (!allow_registered_fallbacks_ || !fonts_.front().font) {
    return -1;
  }

  const int weight =
      fonts_.front().font->GetFontWeight().value_or(pdfium::kFontWeightNormal);
  const bool italic = fonts_.front().font->GetItalicAngle() != 0;
  std::optional<CFX_FontRegistry::FontId> font_id =
      CFX_FontRegistry::FindFallbackFont(word, weight, italic,
                                         /*for_authoring=*/true);
  if (!font_id.has_value()) {
    return -1;
  }

  int32_t existing_font_index = FindExistingRegisteredFont(*font_id);
  if (existing_font_index >= 0) {
    return existing_font_index;
  }

  return AddRegisteredFallbackFont(*font_id);
}

int32_t CPDF_AnnotFontMap::CharCodeFromUnicode(int32_t font_index,
                                               uint16_t word) {
  RetainPtr<CPDF_Font> font = GetPDFFont(font_index);
  if (!font) {
    return -1;
  }

  uint32_t charcode = font->CharCodeFromUnicode(word);
  if (charcode == CPDF_Font::kInvalidCharCode || (charcode == 0 && word != 0)) {
    return -1;
  }
  if (fxcrt::IndexInBounds(fonts_, font_index)) {
    FontEntry& entry = fonts_[font_index];
    if (entry.registered_font_id != CFX_FontRegistry::kInvalidFontId) {
      entry.glyph_to_unicode.emplace(charcode, word);
    }
  }
  return pdfium::checked_cast<int32_t>(charcode);
}

FX_Charset CPDF_AnnotFontMap::CharSetFromUnicode(uint16_t word,
                                                 FX_Charset old_charset) {
  if (word < 0x7F) {
    return FX_Charset::kANSI;
  }
  if (old_charset != FX_Charset::kDefault) {
    return old_charset;
  }
  return CFX_Font::GetCharSetFromUnicode(word);
}

bool CPDF_AnnotFontMap::SupportsWord(int32_t font_index, uint16_t word) const {
  if (!fxcrt::IndexInBounds(fonts_, font_index)) {
    return false;
  }
  const FontEntry& entry = fonts_[font_index];
  if (!entry.font) {
    return false;
  }
  if (entry.registered_font_id != CFX_FontRegistry::kInvalidFontId) {
    return CFX_FontRegistry::SupportsUnicode(entry.registered_font_id, word);
  }
  return PDFontSupportsUnicode(entry.font, word);
}

void CPDF_AnnotFontMap::ReleaseLayoutFonts() {
  // The fonts first: their dictionaries reference streams in the scratch
  // holders, which go second.
  fonts_.clear();
  layout_scratch_.clear();
}

RetainPtr<CPDF_Font> CPDF_AnnotFontMap::CreateRegisteredLayoutFont(
    CFX_FontRegistry::FontId font_id) {
  CPDF_AnnotFontSubset::LayoutFont layout_font =
      CPDF_AnnotFontSubset::CreateLayoutFont(doc_, font_id);
  if (layout_font.scratch) {
    layout_scratch_.push_back(std::move(layout_font.scratch));
  }
  return std::move(layout_font.font);
}

int32_t CPDF_AnnotFontMap::FindExistingRegisteredFont(
    CFX_FontRegistry::FontId font_id) const {
  for (size_t i = 0; i < fonts_.size(); ++i) {
    if (fonts_[i].registered_font_id == font_id) {
      return pdfium::checked_cast<int32_t>(i);
    }
  }
  return -1;
}

int32_t CPDF_AnnotFontMap::AddRegisteredFallbackFont(
    CFX_FontRegistry::FontId font_id) {
  RetainPtr<CPDF_Font> font = CreateRegisteredLayoutFont(font_id);
  if (!font) {
    return -1;
  }

  FontEntry entry;
  entry.font = std::move(font);
  entry.alias = AllocateAppearanceAlias(ResourceKeyForRegisteredFont(font_id));
  entry.registered_font_id = font_id;
  entry.source = Source::kRegistered;
  ProgramMetrics(entry.font->GetFont(), &entry.ascent, &entry.descent);
  fonts_.push_back(std::move(entry));
  return pdfium::checked_cast<int32_t>(fonts_.size() - 1);
}

// ---- Rich text
// ----------------------------------------------------------------

void CPDF_AnnotFontMap::PinRichFace(const WideString& family,
                                    int weight,
                                    bool italic,
                                    int entry) {
  if (entry < 0 || static_cast<size_t>(entry) >= fonts_.size()) {
    return;
  }
  pinned_faces_.push_back(
      {family, std::clamp(weight, 100, 900), italic, entry});
}

// static
ByteString CPDF_AnnotFontMap::FaceRequestKey(const WideString& family,
                                             int weight,
                                             bool italic) {
  return ByteString::Format("%s|%d|%d", FamilyKey(family).c_str(),
                            std::clamp(weight, 100, 900), italic ? 1 : 0);
}

bool CPDF_AnnotFontMap::CoversText(int entry, WideStringView text) {
  if (!fxcrt::IndexInBounds(fonts_, entry)) {
    return false;
  }
  // Scalars, not code units: on a 16-bit wchar_t platform a supplementary
  // character is a surrogate pair, and neither half has a glyph anywhere.
  for (char32_t cp : pdfium::CodePointView(text)) {
    if (cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) ||
        (cp >= 0xE0100 && cp <= 0xE01EF)) {
      continue;  // joiners and variation selectors, as the layout skips them
    }
    if (RichGlyphFor(entry, static_cast<uint32_t>(cp)) == 0) {
      return false;
    }
  }
  return true;
}

int CPDF_AnnotFontMap::ResolveRichFace(const WideString& family,
                                       int weight,
                                       bool italic,
                                       WideStringView text) {
  const ByteString request = FaceRequestKey(family, weight, italic);
  auto cached = resolved_faces_.find(request);
  if (cached != resolved_faces_.end()) {
    return cached->second;
  }
  const int entry = ResolveRichFaceUncached(family, weight, italic, text);
  if (entry >= 0) {
    resolved_faces_[request] = entry;
  }
  return entry;
}

int CPDF_AnnotFontMap::ResolveRichFaceUncached(const WideString& family,
                                               int weight,
                                               bool italic,
                                               WideStringView text) {
  weight = std::clamp(weight, 100, 900);
  bool degraded = false;

  // 0. A face the caller pinned to an entry (the /DA font of a regenerated
  // plain box), if it can draw the text; a box's own font that lacks a
  // glyph the edit needs is not the edit's face.
  const ByteString wanted_key = FamilyKey(family);
  for (const PinnedFace& pinned : pinned_faces_) {
    if (pinned.weight == weight && pinned.italic == italic &&
        FamilyKey(pinned.family) == wanted_key) {
      if (CoversText(pinned.entry, text)) {
        return pinned.entry;
      }
      break;
    }
  }

  // 1. A registered font, by family, weight and italic.
  std::optional<CFX_FontRegistry::FontId> registered =
      CFX_FontRegistry::FindFont(family.ToUTF8(), weight, italic);
  if (registered.has_value()) {
    if (CFX_FontRegistry::IsEditingAuthorized(*registered)) {
      int32_t existing = FindExistingRegisteredFont(*registered);
      if (existing >= 0) {
        return existing;
      }
      int32_t added = AddRegisteredFallbackFont(*registered);
      if (added >= 0) {
        return added;
      }
    }
    degraded = true;  // preview-and-print, or the program failed to load
  }

  // 2. A program already embedded in the document under that family, when
  //    it maps every character of the request.
  const int document_program =
      FindDocumentProgram(family, weight, italic, text);
  if (document_program >= 0) {
    fonts_[document_program].degraded = degraded;
    return document_program;
  }

  // 3. The standard 14, when the family is one of them.
  const StandardFamily standard = StandardFamilyFor(family);
  if (standard != StandardFamily::kNone) {
    const int entry =
        AddStandardFont(StandardBaseFontName(standard, weight, italic));
    if (entry >= 0) {
      fonts_[entry].degraded = degraded;
      return entry;
    }
  }

  // 4. Substitute: Helvetica in the requested style, and say so.
  const int entry = AddStandardFont(
      StandardBaseFontName(StandardFamily::kHelvetica, weight, italic));
  if (entry >= 0) {
    fonts_[entry].degraded = true;
  }
  return entry;
}

int CPDF_AnnotFontMap::FindRichFallback(uint32_t unicode,
                                        int weight,
                                        bool italic) {
  if (!allow_registered_fallbacks_) {
    return -1;
  }
  std::optional<CFX_FontRegistry::FontId> font_id =
      CFX_FontRegistry::FindFallbackFont(unicode, weight, italic,
                                         /*for_authoring=*/true);
  if (!font_id.has_value()) {
    return -1;
  }
  int32_t existing = FindExistingRegisteredFont(*font_id);
  if (existing >= 0) {
    return existing;
  }
  return AddRegisteredFallbackFont(*font_id);
}

CPDF_AnnotFontMap::RichFace CPDF_AnnotFontMap::GetRichFace(int entry) const {
  RichFace face;
  if (!fxcrt::IndexInBounds(fonts_, entry)) {
    return face;
  }
  const FontEntry& font_entry = fonts_[entry];
  face.entry = entry;
  face.source = font_entry.source;
  face.ascent = font_entry.ascent;
  face.descent = font_entry.descent;
  face.degraded = font_entry.degraded;
  switch (font_entry.source) {
    case Source::kRegistered:
      face.program = font_entry.font ? font_entry.font->GetFont() : nullptr;
      break;
    case Source::kDocumentProgram:
      face.program = font_entry.program.get();
      break;
    case Source::kStandard14:
    case Source::kDocumentFontDict:
      face.pdf_font = font_entry.font.Get();
      break;
  }
  return face;
}

uint32_t CPDF_AnnotFontMap::RichGlyphFor(int entry, uint32_t unicode) {
  if (!fxcrt::IndexInBounds(fonts_, entry)) {
    return 0;
  }
  FontEntry& font_entry = fonts_[entry];
  switch (font_entry.source) {
    case Source::kRegistered:
    case Source::kDocumentProgram: {
      const CFX_Font* program =
          font_entry.source == Source::kRegistered
              ? (font_entry.font ? font_entry.font->GetFont() : nullptr)
              : font_entry.program.get();
      RetainPtr<CFX_Face> face = program ? program->GetFace() : nullptr;
      if (!face) {
        return 0;
      }
      const int glyph = face->GetCharIndex(unicode);
      return glyph > 0 ? static_cast<uint32_t>(glyph) : 0;
    }
    case Source::kStandard14:
    case Source::kDocumentFontDict: {
      if (!font_entry.font || unicode > 0xFFFF) {
        return 0;
      }
      if (!PDFontSupportsUnicode(font_entry.font,
                                 static_cast<uint16_t>(unicode))) {
        return 0;
      }
      return font_entry.font->CharCodeFromUnicode(
          static_cast<wchar_t>(unicode));
    }
  }
}

const CPDF_AnnotFontSubset::GlyphIdentity* CPDF_AnnotFontMap::GlyphIdentityOf(
    FontEntry* entry) {
  if (!entry->glyph_identity) {
    const CFX_Font* program =
        entry->source == Source::kRegistered
            ? (entry->font ? entry->font->GetFont() : nullptr)
            : entry->program.get();
    entry->glyph_identity =
        std::make_unique<CPDF_AnnotFontSubset::GlyphIdentity>(program);
  }
  return entry->glyph_identity.get();
}

uint16_t CPDF_AnnotFontMap::EncodeRichGlyph(int entry,
                                            uint32_t gid,
                                            uint32_t unicode,
                                            bool* shared) {
  *shared = false;
  if (!fxcrt::IndexInBounds(fonts_, entry)) {
    return 0;
  }
  FontEntry& font_entry = fonts_[entry];
  if (font_entry.source == Source::kStandard14 ||
      font_entry.source == Source::kDocumentFontDict) {
    return static_cast<uint16_t>(gid);  // the glyph is the charcode already
  }
  std::optional<uint16_t> cid = GlyphIdentityOf(&font_entry)->CidOf(gid);
  if (!cid.has_value()) {
    return 0;
  }
  auto [it, inserted] = font_entry.glyph_to_unicode.emplace(*cid, unicode);
  *shared = !inserted && it->second != unicode;
  return *cid;
}

ByteString CPDF_AnnotFontMap::ChooseDefaultAppearanceAlias(int entry) const {
  if (!fxcrt::IndexInBounds(fonts_, entry)) {
    return ByteString();
  }
  const FontEntry& font_entry = fonts_[entry];
  switch (font_entry.source) {
    case Source::kRegistered: {
      ByteString key;
      ChooseRegisteredFontAlias(doc_, font_entry.registered_font_id, &key);
      return key;
    }
    case Source::kDocumentFontDict:
      return font_entry.alias;
    case Source::kStandard14:
    case Source::kDocumentProgram:
      break;
  }
  const ByteString preferred =
      font_entry.source == Source::kStandard14
          ? StandardAliasFor(font_entry.base_font_name)
          : ByteString::Format("EDocF%u",
                               font_entry.program_stream
                                   ? font_entry.program_stream->GetObjNum()
                                   : 0u);
  RetainPtr<const CPDF_Dictionary> font_res = GetDrFontDict(doc_);
  if (!font_res) {
    return preferred;
  }
  if (font_entry.source == Source::kStandard14) {
    // An existing /DR entry for the same face is reused, as the standard
    // DA setter does.
    CPDF_DictionaryLocker locker(font_res);
    for (const auto& [key, value] : locker) {
      RetainPtr<const CPDF_Dictionary> dict =
          value ? ToDictionary(value->GetDirect()) : nullptr;
      if (dict && dict->GetNameFor("BaseFont") == font_entry.base_font_name &&
          !CPDF_AnnotFontSubset::IsEmbedPDFRegisteredFontDict(dict.Get())) {
        return key;
      }
    }
  }
  ByteString key = preferred;
  for (int suffix = 1; font_res->KeyExist(key.AsStringView()); ++suffix) {
    key = ByteString::Format("%s_%d", preferred.c_str(), suffix);
  }
  return key;
}

void CPDF_AnnotFontMap::SetDefaultAppearanceEntry(int entry,
                                                  const ByteString& alias) {
  if (!fxcrt::IndexInBounds(fonts_, entry) || alias.IsEmpty()) {
    return;
  }
  // No two entries share a resource name: another entry that already uses
  // this alias moves to a suffixed one.
  for (size_t i = 0; i < fonts_.size(); ++i) {
    if (static_cast<int>(i) != entry && fonts_[i].alias == alias) {
      fonts_[i].alias = ByteString();
      fonts_[i].alias = AllocateAppearanceAlias(alias + "_1");
    }
  }
  fonts_[entry].alias = alias;
  da_entry_ = entry;
}

int CPDF_AnnotFontMap::FindDocumentProgram(const WideString& family,
                                           int weight,
                                           bool italic,
                                           WideStringView text) {
  if (!doc_) {
    return -1;
  }
  const ByteString wanted = FamilyKey(family);
  if (wanted.IsEmpty()) {
    return -1;
  }
  std::vector<DocumentProgramCandidate> candidates;
  std::set<const CPDF_Stream*> seen;
  for (const RetainPtr<const CPDF_Dictionary>& dict : DocumentFontDicts(doc_)) {
    std::optional<DocumentProgramCandidate> candidate =
        CandidateFromFontDict(dict.Get());
    if (!candidate.has_value() ||
        FamilyKey(candidate->identity.family) != wanted ||
        !seen.insert(candidate->stream.Get()).second) {
      continue;
    }
    candidate->score = std::abs(candidate->identity.weight - weight) +
                       (candidate->identity.italic == italic ? 0 : 1000);
    candidates.push_back(std::move(*candidate));
  }
  // The best style match first; among equals a whole program before a
  // subset, which was cut for someone else's text.
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const DocumentProgramCandidate& a,
                      const DocumentProgramCandidate& b) {
                     if (a.score != b.score) {
                       return a.score < b.score;
                     }
                     return !HasSubsetTag(a.base_font_name) &&
                            HasSubsetTag(b.base_font_name);
                   });
  for (DocumentProgramCandidate& candidate : candidates) {
    int entry = -1;
    for (size_t i = 0; i < fonts_.size(); ++i) {
      if (fonts_[i].source == Source::kDocumentProgram &&
          fonts_[i].program_stream == candidate.stream) {
        entry = static_cast<int>(i);
        break;
      }
    }
    if (entry < 0) {
      entry = AddDocumentProgram(std::move(candidate.stream),
                                 candidate.base_font_name, candidate.identity);
    }
    // Eligible on its bytes, and covering the request (C note §1.3): a
    // program that cannot draw the text is not its face; the next
    // candidate is, or the next rung.
    if (entry >= 0 && CoversText(entry, text)) {
      return entry;
    }
  }
  return -1;
}

int CPDF_AnnotFontMap::AddDocumentProgram(
    RetainPtr<CPDF_Stream> stream,
    const ByteString& base_font_name,
    const CPDF_AnnotFontSubset::FaceIdentity& identity) {
  auto* page_data = CPDF_DocPageData::FromDocument(doc_);
  RetainPtr<CPDF_StreamAcc> data = page_data->GetFontFileStreamAcc(stream);
  if (!data || data->GetSpan().empty()) {
    return -1;
  }
  // Eligibility on the bytes (C note §1.3): an sfnt we can emit, whose
  // licence allows authoring.
  if (CFX_FontRegistry::DetectProgramFormat(data->GetSpan()) ==
      CFX_FontRegistry::ProgramFormat::kUnsupported) {
    page_data->MaybePurgeFontFileStreamAcc(std::move(data));
    return -1;
  }
  auto program = std::make_unique<CFX_Font>();
  if (!program->LoadEmbedded(data->GetSpan(), /*force_vertical=*/false,
                             stream->KeyForCache()) ||
      !program->HasAnyGlyphs() ||
      // A program that cannot map a character (a renumbered subset
      // carrying glyf/loca/hmtx and no cmap, as producers emit for
      // Identity-H) renders what it was made for and authors nothing. The
      // layout looks glyphs up by scalar, so the charmap must be Unicode;
      // asked for explicitly, a Mac Roman-only cmap is refused too.
      !program->GetFace()->SelectCharMap(fxge::FontEncoding::kUnicode)) {
    page_data->MaybePurgeFontFileStreamAcc(std::move(data));
    return -1;
  }
  const CFX_FontRegistry::EmbeddingPermission permission =
      CFX_FontRegistry::ClassifyFsType(program->GetFace()->GetFsTypeFlags());
  if (permission != CFX_FontRegistry::EmbeddingPermission::kInstallable &&
      permission != CFX_FontRegistry::EmbeddingPermission::kEditable) {
    page_data->MaybePurgeFontFileStreamAcc(std::move(data));
    return -1;  // preview-and-print: renders, never authors (no override)
  }
  FontEntry entry;
  entry.source = Source::kDocumentProgram;
  entry.alias = AllocateAppearanceAlias(
      ByteString::Format("EDocF%u", stream->GetObjNum()));
  entry.program = std::move(program);
  entry.program_data = std::move(data);
  entry.program_stream = std::move(stream);
  entry.base_font_name = base_font_name;
  entry.identity = identity;
  ProgramMetrics(entry.program.get(), &entry.ascent, &entry.descent);
  fonts_.push_back(std::move(entry));
  return static_cast<int>(fonts_.size() - 1);
}

int CPDF_AnnotFontMap::AddStandardFont(const ByteString& base_font_name) {
  if (!doc_ || base_font_name.IsEmpty()) {
    return -1;
  }
  for (size_t i = 0; i < fonts_.size(); ++i) {
    const FontEntry& entry = fonts_[i];
    if (entry.source == Source::kStandard14 &&
        entry.base_font_name == base_font_name) {
      return static_cast<int>(i);
    }
    // The /DA font as found in /DR, when it is this very standard face
    // (Acrobat's /HeBo for Helvetica-Bold): one resource, one alias.
    if (entry.source == Source::kDocumentFontDict && entry.font &&
        entry.font->IsStandardFont() &&
        entry.font->GetBaseFontName() == base_font_name) {
      return static_cast<int>(i);
    }
  }
  // A direct dictionary of our own (the AP resource is a clone of it), so no
  // document object is touched before Publish.
  auto font_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  font_dict->SetNewFor<CPDF_Name>("Type", "Font");
  font_dict->SetNewFor<CPDF_Name>("Subtype", "Type1");
  font_dict->SetNewFor<CPDF_Name>("BaseFont", base_font_name);
  if (base_font_name != "Symbol" && base_font_name != "ZapfDingbats") {
    font_dict->SetNewFor<CPDF_Name>("Encoding", "WinAnsiEncoding");
  }
  RetainPtr<CPDF_Font> font = CPDF_Font::Create(doc_, font_dict, nullptr);
  if (!font) {
    return -1;
  }
  FontEntry entry;
  entry.source = Source::kStandard14;
  entry.font = std::move(font);
  entry.base_font_name = base_font_name;
  entry.alias = AllocateAppearanceAlias(StandardAliasFor(base_font_name));
  StandardMetrics(base_font_name, entry.font.Get(), &entry.ascent,
                  &entry.descent);
  fonts_.push_back(std::move(entry));
  return static_cast<int>(fonts_.size() - 1);
}
