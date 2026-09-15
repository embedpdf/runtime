// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: annotation font map for registered runtime fonts. This file is
// fork-owned and supports FreeText fallback font routing/subsetting.

#ifndef CORE_FPDFDOC_CPDF_ANNOTFONTMAP_H_
#define CORE_FPDFDOC_CPDF_ANNOTFONTMAP_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "core/fpdfdoc/cpdf_annotfontsubset.h"
#include "core/fpdfdoc/ipvt_fontmap.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"
#include "core/fxge/cfx_fontregistry.h"

class CPDF_Dictionary;
class CPDF_Document;
class CPDF_Font;
class CPDF_IndirectObjectHolder;

class CPDF_AnnotFontMap final : public IPVT_FontMap {
 public:
  // Whose text the appearance is, for the document's embedding policy (§2 of
  // the Phase C note): under the DEFAULT policy annotation text (FreeText,
  // redaction labels) is subset and form field text is embedded whole.
  enum class Owner : uint8_t { kAnnotation, kWidget };

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
                    bool install_dr_entry = false,
                    Owner owner = Owner::kAnnotation);
  ~CPDF_AnnotFontMap() override;

  // Pick the /DR resource name a DA string uses for |font_id|: "ERegF<id>",
  // suffixed "_N" when a foreign entry already owns that key. Choose reads
  // the document and writes nothing (a Prepare step may call it); Reserve
  // also records the choice on this document instance and makes sure
  // /AcroForm /DR /Font exists, so the alias resolves until the real font
  // dictionary is installed by the appearance that first uses it.
  static bool ChooseRegisteredFontAlias(const CPDF_Document* doc,
                                        CFX_FontRegistry::FontId font_id,
                                        ByteString* resource_key);
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

  // The appearance's /Resources/Font, in the two steps of §6 of the Phase C
  // note. Prepare builds every registered font's resource off to the side:
  // no object number is allocated, no alias reserved and no dictionary of
  // the document touched, so a failure part-way leaves the document exactly
  // as it was. It fails (nullopt) when the /DA font, or any font whose
  // glyphs the appearance uses, has no resource: an appearance must never
  // name a font that is not there. Publish adds the staged objects, installs
  // the /DR entry for the /DA font and returns the /Font dictionary.
  struct PreparedFontResources {
    PreparedFontResources();
    PreparedFontResources(PreparedFontResources&& that) noexcept;
    PreparedFontResources& operator=(PreparedFontResources&& that) noexcept;
    ~PreparedFontResources();

    struct StagedEntry {
      ByteString alias;
      bool install_in_dr = false;
      CPDF_AnnotFontSubset::StagedFontResource resource;
    };
    // Direct. Foreign (document) fonts are already in it; registered fonts
    // are added at Publish.
    RetainPtr<CPDF_Dictionary> font_resources;
    std::vector<StagedEntry> registered;
  };
  std::optional<PreparedFontResources> PrepareFontResources();
  RetainPtr<CPDF_Dictionary> PublishFontResources(
      PreparedFontResources prepared);

  // Prepare and Publish in one call, for callers with nothing else to stage.
  // Returns nullptr when Prepare fails; nothing was written then.
  RetainPtr<CPDF_Dictionary> CreateFontResourceDict();

  // Test-only. Makes PrepareFontResources() fail while staging the
  // registered font after the first |staged_count| were staged, the way an
  // unsupported program would. 0 disables.
  static void FailAfterStagedFontsForTesting(int staged_count);

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
  CPDF_AnnotFontSubset::Embedding EmbeddingForRegisteredFonts() const;
  void InstallDrEntry(const ByteString& alias,
                      const CPDF_Dictionary* font_dict);
  void ReleaseLayoutFonts();
  RetainPtr<CPDF_Font> CreateRegisteredLayoutFont(
      CFX_FontRegistry::FontId font_id);
  int32_t FindExistingRegisteredFont(CFX_FontRegistry::FontId font_id) const;
  int32_t AddRegisteredFallbackFont(CFX_FontRegistry::FontId font_id);

  UnownedPtr<CPDF_Document> const doc_;
  const bool allow_registered_fallbacks_;
  const bool install_dr_entry_;
  const Owner owner_;
  // The scratch holders of the layout fonts' streams. Declared before
  // |fonts_| so they are destroyed after it: the fonts' dictionaries
  // reference streams in them.
  std::vector<std::unique_ptr<CPDF_IndirectObjectHolder>> layout_scratch_;
  std::vector<FontEntry> fonts_;
};

#endif  // CORE_FPDFDOC_CPDF_ANNOTFONTMAP_H_
