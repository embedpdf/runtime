// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: the fonts a write would draw text with, found the way the write
// finds them, without writing: the same /DA resolution, the same font map,
// each face resolved once over all of its text, and the same per-grapheme
// fallback. What an editor shows while someone types.

#ifndef CORE_FPDFDOC_CPDF_FONTRESOLVER_H_
#define CORE_FPDFDOC_CPDF_FONTRESOLVER_H_

#include <vector>

#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fxcrt/widestring.h"
#include "core/fxge/cfx_fontregistry.h"

class CPDF_Dictionary;
class CPDF_Document;

class CPDF_FontResolver {
 public:
  // A run of text and the face it asks for.
  struct Run {
    WideString text;
    WideString family;  // a family name, or a registered font's family
    int weight = 400;
    bool italic = false;
  };

  struct ResolvedRun {
    // The entry the run's face resolves to: where its program comes from,
    // which one, the face it draws, and whether it substitutes the request.
    CPDF_AnnotFontMap::EntryFacts face;
    // The registered fallback fonts in the order the layout tries them for
    // the run's style (authorized for editing only).
    std::vector<CFX_FontRegistry::FontId> fallbacks;
  };

  struct Result {
    std::vector<ResolvedRun> runs;
    // Graphemes no font covers, first seen first: drawn as missing glyphs.
    std::vector<WideString> missing_characters;
  };

  // |annot_dict| is the box being edited (its /DA font is the box's own,
  // kept while it covers the text), or null for a new box.
  static Result Resolve(CPDF_Document* doc,
                        const CPDF_Dictionary* annot_dict,
                        const std::vector<Run>& runs);
};

#endif  // CORE_FPDFDOC_CPDF_FONTRESOLVER_H_
