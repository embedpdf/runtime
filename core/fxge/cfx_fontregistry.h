// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: fork-owned runtime font registry shared by page fallback rendering
// and annotation appearance/subset embedding.

#ifndef CORE_FXGE_CFX_FONTREGISTRY_H_
#define CORE_FXGE_CFX_FONTREGISTRY_H_

#include <stdint.h>

#include <memory>
#include <optional>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"

class CFX_Font;
class IFX_SeekableReadStream;

class CFX_FontRegistry {
 public:
  using FontId = uint32_t;

  static constexpr FontId kInvalidFontId = 0;

  // EmbedPDF: the OS/2 fsType embedding permission of a registered font
  // (OpenType spec, OS/2 table). Registration refuses kRestricted and
  // kBitmapOnly outright. kPreviewAndPrint fonts render existing text but
  // may not author new text until the app asserts a licence with
  // AuthorizeEditing(); kEditable and kInstallable author freely.
  enum class EmbeddingPermission : uint8_t {
    kInstallable = 0,
    kEditable = 1,
    kPreviewAndPrint = 2,
    kRestricted = 3,
    kBitmapOnly = 4,
  };

  // What a program is, decided from its first bytes. Registration accepts
  // kTrueType and kOpenTypeCFF only, so every registered program is one the
  // annotation writer can subset and emit (Phase C note §3): Type1, bare CFF,
  // collections and WOFF are refused at the door.
  enum class ProgramFormat : uint8_t { kTrueType, kOpenTypeCFF, kUnsupported };
  static ProgramFormat DetectProgramFormat(pdfium::span<const uint8_t> data);

  static FontId RegisterMemoryFont(const ByteString& family_name,
                                   int weight,
                                   int italic,
                                   pdfium::span<const uint8_t> data);
  static FontId RegisterFont(const ByteString& family_name,
                             int weight,
                             int italic,
                             RetainPtr<IFX_SeekableReadStream> stream);
  static void ClearRegisteredFonts();

  static bool AddFallbackFont(FontId font_id);
  static void ClearFallbackFonts();
  static bool HasFallbackFonts();

  // EmbedPDF: permissions (§3.4 of the rich text plan).
  static std::optional<EmbeddingPermission> GetEmbeddingPermission(
      FontId font_id);
  static bool IsEditingAuthorized(FontId font_id);
  static bool AuthorizeEditing(FontId font_id);
  // False when fsType forbids subsetting (bit 0x0100): embed the whole program.
  static bool AllowsSubsetting(FontId font_id);
  // SHA-256 of the registered (instanced) bytes: the source identity a font
  // pool shares programs by. Empty for an unknown id.
  static pdfium::span<const uint8_t> GetSourceHash(FontId font_id);
  // True when the registered bytes are a static instance made from a
  // variable font at registration. A variable font that cannot be instanced
  // is refused, so every registered program is static.
  static bool IsInstanced(FontId font_id);

  static bool IsValidFont(FontId font_id);
  static ByteString GetBaseFontName(FontId font_id);
  // EmbedPDF: the family exactly as registered (spaces kept). Written to the
  // font descriptor's /FontFamily so a saved document can be re-resolved by
  // family in a later session, and so Acrobat can re-resolve it on edit.
  static ByteString GetFamilyName(FontId font_id);
  static int GetStyleWeight(FontId font_id);
  static bool IsStyleItalic(FontId font_id);
  // EmbedPDF: resolve a face request by family (space- and case-insensitive
  // against the registered family and base font name), then by the closest
  // weight/italic. This is the persistent identity of a registered font;
  // numeric ids are session-local.
  static std::optional<FontId> FindFont(const ByteString& family_name,
                                        int weight,
                                        bool italic);
  static bool SupportsUnicode(FontId font_id, uint32_t unicode);
  // |for_authoring|: the glyph will be written into new text (annotation
  // appearance), so fonts not authorized for editing are skipped. Rendering
  // existing page text passes false.
  static std::optional<FontId> FindFallbackFont(uint32_t unicode,
                                                int weight,
                                                bool italic,
                                                bool for_authoring = false);
  static std::unique_ptr<CFX_Font> CreateFont(FontId font_id);

  static void DestroyGlobals();
};

#endif  // CORE_FXGE_CFX_FONTREGISTRY_H_
