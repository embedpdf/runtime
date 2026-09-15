// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: annotation font map for registered runtime fonts. This lets
// FreeText appearance generation pick per-glyph fallback fonts and later embed
// only the glyph subset used by the annotation/layer.

#include "core/fpdfdoc/cpdf_annotfontmap.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_indirect_object_holder.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfdoc/cpdf_annotfontsubset.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/fx_codepage.h"
#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxge/cfx_font.h"

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
    if (!entry.font || entry.alias.IsEmpty()) {
      continue;
    }

    if (entry.registered_font_id != CFX_FontRegistry::kInvalidFontId) {
      // Entry 0 is the /DA font: its alias is what /DA names in /DR, so it
      // needs a resource even when no glyph of it was drawn (empty text, or
      // text drawn entirely by fallback fonts).
      const bool required = i == 0 && install_dr_entry_;
      if (g_fail_after_staged_fonts_for_testing > 0 &&
          staged_count >= g_fail_after_staged_fonts_for_testing) {
        return std::nullopt;  // the injected failure of the next resource
      }
      PreparedFontResources::StagedEntry staged;
      switch (CPDF_AnnotFontSubset::StageRegisteredFontResource(
          entry.registered_font_id, entry.glyph_to_unicode, required, embedding,
          &staged.resource)) {
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
      staged.install_in_dr = required;
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
  for (PreparedFontResources::StagedEntry& staged : prepared.registered) {
    RetainPtr<CPDF_Dictionary> font_resource =
        CPDF_AnnotFontSubset::PublishStagedFontResource(
            doc_, std::move(staged.resource));
    CHECK(font_resource);  // a staged resource is complete by construction
    prepared.font_resources->SetNewFor<CPDF_Reference>(
        staged.alias, doc_, font_resource->GetObjNum());
    if (staged.install_in_dr) {
      InstallDrEntry(staged.alias, font_resource.Get());
    }
  }
  return prepared.font_resources;
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
  return fxcrt::IndexInBounds(fonts_, font_index) ? fonts_[font_index].alias
                                                  : ByteString();
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
  fonts_.push_back(std::move(entry));
  return pdfium::checked_cast<int32_t>(fonts_.size() - 1);
}
