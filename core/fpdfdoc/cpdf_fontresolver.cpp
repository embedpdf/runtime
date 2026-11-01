// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_fontresolver.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfdoc/cpdf_appearancegenerator_internal.h"
#include "core/fpdfdoc/cpdf_documentfonts.h"
#include "core/fpdfdoc/cpdf_fontface.h"
#include "core/fpdfdoc/cpdf_richtextlayout.h"

namespace {

// What a browser loads for the program |face| draws with: a document font's
// program as its dictionary reads it, a document program as it is.
DataVector<uint8_t> DisplayHashOf(CPDF_Document* doc,
                                  const CPDF_AnnotFontMap::EntryFacts& face) {
  if (face.source == CPDF_AnnotFontMap::Source::kDocumentFontDict) {
    return CPDF_DocumentFonts::For(doc)->DisplayHashOfFont(
        face.font_dict.Get());
  }
  if (face.source == CPDF_AnnotFontMap::Source::kDocumentProgram) {
    return CPDF_DocumentFonts::For(doc)->DisplayHashOfProgram(
        face.program_objnum);
  }
  return {};
}

}  // namespace

// static
CPDF_FontResolver::Result CPDF_FontResolver::Resolve(
    CPDF_Document* doc,
    const CPDF_Dictionary* annot_dict,
    const std::vector<Run>& runs) {
  Result result;
  if (!doc) {
    return result;
  }
  const CPDF_Dictionary* root = doc->GetRoot();
  RetainPtr<const CPDF_Dictionary> acroform =
      root ? root->GetDictFor("AcroForm") : nullptr;
  RetainPtr<const CPDF_Dictionary> dr =
      acroform ? acroform->GetDictFor("DR") : nullptr;
  RetainPtr<const CPDF_Dictionary> dr_fonts =
      dr ? dr->GetDictFor("Font") : nullptr;

  // The box's /DA font, as its appearance would find it (read only).
  RetainPtr<CPDF_Font> default_font;
  ByteString font_name;
  appearance_generator::DaFontResolution da_font;
  if (annot_dict) {
    std::optional<appearance_generator::DefaultAppearanceInfo> info =
        appearance_generator::GetDefaultAppearanceInfo(annot_dict,
                                                       acroform.Get());
    if (info.has_value()) {
      font_name = info->font_name;
      da_font = appearance_generator::ResolveDaFontForEphemeralTarget(
          doc, dr_fonts.Get(), font_name);
      if (da_font.font_dict) {
        default_font =
            CPDF_DocPageData::FromDocument(doc)->GetFont(da_font.font_dict);
      }
    }
  }
  const bool widget =
      annot_dict && annot_dict->GetNameFor("Subtype") == "Widget";
  CPDF_AnnotFontMap map(doc, std::move(default_font), font_name,
                        /*allow_registered_fallbacks=*/true,
                        da_font.registered_font_id,
                        /*install_dr_entry=*/false,
                        widget ? CPDF_AnnotFontMap::Owner::kWidget
                               : CPDF_AnnotFontMap::Owner::kAnnotation);
  da_font.ApplyTo(&map);
  // The box keeps its own font while that font covers the text: requests
  // for its face answer with it first.
  if (map.HasDefaultFont() && !map.DefaultFontIsStandard()) {
    const CPDF_FontFace face =
        FaceOfDefaultAppearanceFont(doc, font_name, {dr.Get()});
    map.PinRichFace(face.family, face.weight, face.italic, 0);
  }

  // Each face resolves once over every character asked of it, as the
  // layout primes it, so two runs of one face get one program.
  auto key_of = [](const Run& run) {
    return CPDF_AnnotFontMap::FaceRequestKey(run.family, run.weight, run.italic,
                                             run.program);
  };
  std::map<ByteString, WideString> text_of;
  for (const Run& run : runs) {
    text_of[key_of(run)] += run.text;
  }
  for (const Run& run : runs) {
    map.ResolveRichFace(run.family, run.weight, run.italic,
                        text_of[key_of(run)].AsStringView(), run.program);
  }

  for (const Run& run : runs) {
    const int entry = map.ResolveRichFace(run.family, run.weight, run.italic,
                                          WideStringView(), run.program);
    ResolvedRun resolved;
    resolved.face = map.DescribeEntry(entry);
    resolved.display_sha256 = DisplayHashOf(doc, resolved.face);
    resolved.fallbacks = CFX_FontRegistry::GetFallbackOrder(
        run.weight, run.italic, /*for_authoring=*/true);
    const std::vector<bool> starts =
        CPDF_RichTextLayout::GraphemeStarts(run.text);
    const size_t length = run.text.GetLength();
    for (size_t begin = 0; begin < length;) {
      size_t end = begin + 1;
      while (end < length && !starts[end]) {
        ++end;
      }
      const WideStringView grapheme =
          run.text.AsStringView().Substr(begin, end - begin);
      const CPDF_RichTextLayout::GraphemeMatch match =
          CPDF_RichTextLayout::MatchGrapheme(&map, entry, grapheme, run.weight,
                                             run.italic);
      if (!match.covered &&
          !CPDF_RichTextLayout::IsInvisibleGrapheme(grapheme) &&
          std::ranges::none_of(
              result.missing_characters,
              [&](const WideString& missing) { return missing == grapheme; })) {
        result.missing_characters.emplace_back(grapheme);
      }
      begin = end;
    }
    result.runs.push_back(std::move(resolved));
  }
  return result;
}
