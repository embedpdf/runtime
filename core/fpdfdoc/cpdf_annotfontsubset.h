// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: fork-owned helper for registered annotation font layout and
// per-annotation/layer subset embedding.

#ifndef CORE_FPDFDOC_CPDF_ANNOTFONTSUBSET_H_
#define CORE_FPDFDOC_CPDF_ANNOTFONTSUBSET_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxge/cfx_fontregistry.h"

class CFX_Font;
class CPDF_Array;
class CPDF_Dictionary;
class CPDF_Document;
class CPDF_Font;
class CPDF_IndirectObjectHolder;
class CPDF_Stream;

class CPDF_AnnotFontSubset final {
 public:
  // Keyed by CHARCODE (== CID under Identity-H). CID == GID for TrueType and
  // for CFF without CIDFont operators; a CID-keyed CFF maps through its
  // charset (ISO 32000-2 9.7.4.2), see GlyphIdentity. Values are Unicode
  // scalars. The name is historical.
  using GlyphUnicodeMap = std::map<uint32_t, uint32_t>;

  // What a registered program is, decided from its bytes. Registration
  // refuses kUnsupported, so a registered font is always one of the other
  // two; the check stays here for programs that reach the writer by other
  // routes.
  using ProgramFormat = CFX_FontRegistry::ProgramFormat;
  static ProgramFormat DetectProgramFormat(pdfium::span<const uint8_t> data) {
    return CFX_FontRegistry::DetectProgramFormat(data);
  }

  // The three identifiers of a glyph (§1.1 of the Phase C note): subset
  // membership by GID, /W and glyph selection by CID, appearance bytes and
  // ToUnicode by charcode (== CID under Identity-H).
  struct EncodedGlyph {
    uint32_t gid = 0;
    uint16_t cid = 0;
    uint16_t charcode = 0;
  };

  // CID <-> GID for one program: identity unless it is an sfnt-wrapped
  // CID-keyed CFF, whose charset FreeType reads for us.
  class GlyphIdentity {
   public:
    explicit GlyphIdentity(const CFX_Font* font);
    bool IsIdentity() const { return identity_; }
    std::optional<uint16_t> CidOf(uint32_t gid) const;
    std::optional<uint32_t> GidOf(uint16_t cid) const;

   private:
    bool identity_ = true;
    std::map<uint32_t, uint16_t> gid_to_cid_;
    std::map<uint16_t, uint32_t> cid_to_gid_;
  };

  // A registered font as CPVT lays text out with it. Its program and
  // ToUnicode streams live in |scratch|, a holder of its own, never in the
  // document: laying out allocates no document object number and leaves
  // nothing behind. |scratch| must outlive |font| (it is declared first, so
  // it is destroyed last).
  struct LayoutFont {
    LayoutFont();
    LayoutFont(LayoutFont&& that) noexcept;
    LayoutFont& operator=(LayoutFont&& that) noexcept;
    ~LayoutFont();

    LayoutFont(const LayoutFont&) = delete;
    LayoutFont& operator=(const LayoutFont&) = delete;

    std::unique_ptr<CPDF_IndirectObjectHolder> scratch;
    RetainPtr<CPDF_Font> font;
  };

  static LayoutFont CreateLayoutFont(CPDF_Document* doc,
                                     CFX_FontRegistry::FontId font_id);

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

  // How much of the program a resource carries (§2 of the Phase C note).
  // The caller resolves the document's policy and the resource's owner into
  // one of these; fsType "no subsetting" forces kFull regardless.
  enum class Embedding : uint8_t { kSubset, kFull };

  // A persistent font resource built off to the side (§6 of the Phase C
  // note): no part has an object number and no document was touched to
  // build it. The parts are not linked to each other until they are
  // published, so a staged resource that is dropped costs nothing.
  struct StagedFontResource {
    StagedFontResource();
    StagedFontResource(StagedFontResource&& that) noexcept;
    StagedFontResource& operator=(StagedFontResource&& that) noexcept;
    ~StagedFontResource();

    StagedFontResource(const StagedFontResource&) = delete;
    StagedFontResource& operator=(const StagedFontResource&) = delete;

    CFX_FontRegistry::FontId font_id = CFX_FontRegistry::kInvalidFontId;
    ProgramFormat format = ProgramFormat::kTrueType;
    RetainPtr<CPDF_Dictionary> font_dict;      // Type0; carries the hint
    RetainPtr<CPDF_Dictionary> cid_font_dict;  // CIDFontType2 or CIDFontType0
    RetainPtr<CPDF_Dictionary> descriptor;
    RetainPtr<CPDF_Array> widths;       // /W, keyed by CID
    RetainPtr<CPDF_Stream> program;     // FontFile2, or FontFile3 /OpenType
    RetainPtr<CPDF_Stream> to_unicode;  // null when no glyph was drawn
  };

  // The persistent PDF font resource for a registered font, in two halves.
  //
  // Stage decides everything and builds the parts, touching no document. With
  // used glyphs the resource is the usual embedded subset (or the whole
  // program under Embedding::kFull). With none, but
  // |required_by_default_appearance| (the /DA font of an empty annotation, or
  // of text drawn entirely by fallback fonts), it is a minimal valid embedded
  // resource (glyph 0 only) that still carries the persistent identity, so
  // the /DA font survives a save. With none and not required, kUnused (an
  // unused fallback, nothing to embed). kFailed means a resource was needed
  // and could not be built; callers must then fail rather than leave a font
  // name dangling in an appearance or in /DA.
  enum class StageStatus { kStaged, kUnused, kFailed };
  static StageStatus StageRegisteredFontResource(
      CFX_FontRegistry::FontId font_id,
      const GlyphUnicodeMap& glyph_to_unicode,
      bool required_by_default_appearance,
      Embedding embedding,
      StagedFontResource* out);

  // The resource for a program already embedded in the document (Phase C
  // note §1.3): a new Type0 dictionary whose descriptor references the
  // existing FontFile stream, never a copy; widths and metrics come from
  // |font|, loaded over that stream's bytes. |base_font_name| is the
  // existing dictionary's BaseFont (its subset tag, if any, still applies).
  // The glyph map is keyed as for registered fonts (charcode == CID).
  static StageStatus StageDocumentProgramResource(
      const CFX_Font* font,
      RetainPtr<CPDF_Stream> program_stream,
      const ByteString& base_font_name,
      const FaceIdentity& identity,
      const GlyphUnicodeMap& glyph_to_unicode,
      bool required_by_default_appearance,
      StagedFontResource* out);

  // Publish adds every part of |staged| to |doc| as an indirect object
  // (program, ToUnicode, widths, descriptor, CIDFont, Type0, in that order;
  // a part that already has an object number, the existing stream of a
  // document program, is left alone), links them and raises the catalog
  // /Version when the program needs PDF 1.6. Returns the Type0 dictionary,
  // now indirect.
  static RetainPtr<CPDF_Dictionary> PublishStagedFontResource(
      CPDF_Document* doc,
      StagedFontResource staged);

  // Stage and Publish in one call, for callers with nothing else to stage.
  // nullptr for kUnused and kFailed alike.
  static RetainPtr<CPDF_Dictionary> BuildRegisteredFontResource(
      CPDF_Document* doc,
      CFX_FontRegistry::FontId font_id,
      const GlyphUnicodeMap& glyph_to_unicode,
      bool required_by_default_appearance,
      Embedding embedding = Embedding::kSubset);

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
