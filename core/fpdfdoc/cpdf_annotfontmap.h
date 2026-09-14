// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: annotation font map for registered runtime fonts. This file is
// fork-owned and supports FreeText fallback font routing/subsetting.

#ifndef CORE_FPDFDOC_CPDF_ANNOTFONTMAP_H_
#define CORE_FPDFDOC_CPDF_ANNOTFONTMAP_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <vector>

#include "core/fpdfdoc/ipvt_fontmap.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"
#include "core/fxge/cfx_fontregistry.h"

class CPDF_Dictionary;
class CPDF_Document;
class CPDF_Font;

class CPDF_AnnotFontMap final : public IPVT_FontMap {
 public:
  // |default_font| is the /DA font as found in /DR (may be null when only a
  // registered id is known); |registered_font_id| overrides resolution when
  // the caller already knows the DA alias names a registered font that has
  // no /DR entry yet. With |install_dr_entry| the subset built for the DA
  // font is also installed as the /DR entry under |default_font_alias|, so
  // /DR always names the real embedded font the appearance uses (A1).
  CPDF_AnnotFontMap(CPDF_Document* doc,
                    RetainPtr<CPDF_Font> default_font,
                    const ByteString& default_font_alias,
                    bool allow_registered_fallbacks,
                    CFX_FontRegistry::FontId registered_font_id =
                        CFX_FontRegistry::kInvalidFontId,
                    bool install_dr_entry = false);
  ~CPDF_AnnotFontMap() override;

  // Pick the /DR resource name a DA string uses for |font_id|: "ERegF<id>",
  // suffixed "_N" when a foreign entry already owns that key. Nothing is
  // written to /DR here; the real font dictionary is installed once the
  // appearance is generated and the glyph set is known.
  static bool ReserveRegisteredFontAlias(CPDF_Document* doc,
                                         CFX_FontRegistry::FontId font_id,
                                         ByteString* resource_key);

  // The registered font an alias reserved above (in THIS document instance)
  // stands for, if the id is still valid. An alias is a resource name, not
  // proof of registration: one that merely looks reserved resolves to nothing.
  static std::optional<CFX_FontRegistry::FontId> RegisteredFontIdFromAlias(
      const CPDF_Document* doc,
      const ByteString& alias);

  bool HasDefaultFont() const;

  // The appearance's /Resources/Font. Returns nullptr only when the /DA font
  // is a registered font whose resource could not be built; the appearance
  // must then not be generated, so /DA never dangles.
  RetainPtr<CPDF_Dictionary> CreateFontResourceDict();

  // IPVT_FontMap:
  RetainPtr<CPDF_Font> GetPDFFont(int32_t font_index) override;
  ByteString GetPDFFontAlias(int32_t font_index) override;
  int32_t GetWordFontIndex(uint16_t word,
                           FX_Charset charset,
                           int32_t font_index) override;
  int32_t CharCodeFromUnicode(int32_t font_index, uint16_t word) override;
  FX_Charset CharSetFromUnicode(uint16_t word, FX_Charset old_charset) override;

 private:
  struct FontEntry {
    RetainPtr<CPDF_Font> font;
    ByteString alias;
    CFX_FontRegistry::FontId registered_font_id =
        CFX_FontRegistry::kInvalidFontId;
    std::map<uint32_t, uint32_t> glyph_to_unicode;
  };

  bool SupportsWord(int32_t font_index, uint16_t word) const;
  ByteString AllocateAppearanceAlias(const ByteString& preferred) const;
  void InstallDrEntry(const ByteString& alias,
                      const CPDF_Dictionary* font_dict);
  void DeleteTemporaryLayoutObjects();
  RetainPtr<CPDF_Font> CreateRegisteredLayoutFont(
      CFX_FontRegistry::FontId font_id);
  int32_t FindExistingRegisteredFont(CFX_FontRegistry::FontId font_id) const;
  int32_t AddRegisteredFallbackFont(CFX_FontRegistry::FontId font_id);

  UnownedPtr<CPDF_Document> const doc_;
  const bool allow_registered_fallbacks_;
  const bool install_dr_entry_;
  std::vector<FontEntry> fonts_;
  std::vector<uint32_t> temporary_layout_object_numbers_;
};

#endif  // CORE_FPDFDOC_CPDF_ANNOTFONTMAP_H_
