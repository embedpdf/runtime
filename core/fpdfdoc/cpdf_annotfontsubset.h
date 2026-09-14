// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: fork-owned helper for registered annotation font layout and
// per-annotation/layer subset embedding.

#ifndef CORE_FPDFDOC_CPDF_ANNOTFONTSUBSET_H_
#define CORE_FPDFDOC_CPDF_ANNOTFONTSUBSET_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <vector>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxge/cfx_fontregistry.h"

class CPDF_Dictionary;
class CPDF_Document;
class CPDF_Font;

class CPDF_AnnotFontSubset final {
 public:
  using GlyphUnicodeMap = std::map<uint32_t, uint32_t>;

  struct LayoutFont {
    LayoutFont();
    LayoutFont(LayoutFont&& that) noexcept;
    LayoutFont& operator=(LayoutFont&& that) noexcept;
    ~LayoutFont();

    LayoutFont(const LayoutFont&) = delete;
    LayoutFont& operator=(const LayoutFont&) = delete;

    RetainPtr<CPDF_Font> font;
    std::vector<uint32_t> temporary_object_numbers;
  };

  static LayoutFont CreateLayoutFont(CPDF_Document* doc,
                                     CFX_FontRegistry::FontId font_id);

  // The persistent PDF font resource for a registered font. With used glyphs
  // it is the usual embedded subset. With none, but
  // |required_by_default_appearance| (the /DA font of an empty annotation, or
  // of text drawn entirely by fallback fonts), it is a minimal valid embedded
  // resource (glyph 0 only) that still carries the persistent identity, so the
  // /DA font survives a save. With none and not required, nullptr (an unused
  // fallback). A required resource that cannot be built also returns nullptr;
  // callers must then fail rather than leave /DA dangling.
  static RetainPtr<CPDF_Dictionary> BuildRegisteredFontResource(
      CPDF_Document* doc,
      CFX_FontRegistry::FontId font_id,
      const GlyphUnicodeMap& glyph_to_unicode,
      bool required_by_default_appearance);

  // The persistent identity written into every font dictionary built for a
  // registered font: /FontFamily, /FontWeight and /ItalicAngle in the font
  // descriptor (what a later session and Acrobat resolve by) plus a private
  // session hint (EmbedPDFRegisteredFontId) on the top-level font dictionary.
  struct FaceIdentity {
    ByteString family;
    int weight = 400;
    bool italic = false;
  };
  static FaceIdentity IdentityForRegisteredFont(CFX_FontRegistry::FontId id);

  // True when |font_dict| was written by EmbedPDF for a registered font: a
  // real Type0 subset (A1 and later) or the legacy descriptor-less /Type1
  // marker that older files carry in /DR.
  static bool IsEmbedPDFRegisteredFontDict(const CPDF_Dictionary* font_dict);
  static bool IsLegacyMarkerFontDict(const CPDF_Dictionary* font_dict);

  // Resolve the registered font a dictionary written by EmbedPDF stands for:
  // family/weight/italic from its descriptor first (stable across sessions),
  // the numeric session hint only as a tie-breaker or for legacy markers
  // without a descriptor. Returns nullopt for dictionaries that are not ours
  // or whose font is not registered in this session.
  static std::optional<CFX_FontRegistry::FontId> ResolveRegisteredFont(
      const CPDF_Dictionary* font_dict);
};

#endif  // CORE_FPDFDOC_CPDF_ANNOTFONTSUBSET_H_
