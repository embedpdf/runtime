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
  static std::optional<FontId> FindFallbackFont(uint32_t unicode,
                                                int weight,
                                                bool italic);
  static std::unique_ptr<CFX_Font> CreateFont(FontId font_id);

  static void DestroyGlobals();
};

#endif  // CORE_FXGE_CFX_FONTREGISTRY_H_
