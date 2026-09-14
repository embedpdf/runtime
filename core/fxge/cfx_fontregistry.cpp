// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: process/thread-local registry for runtime fonts. Registered fonts
// are used both for page-rendering fallback and for annotation authoring.

#include "core/fxge/cfx_fontregistry.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "core/fxcrt/containers/contains.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/epdf_tls.h"
#include "core/fxcrt/fx_codepage.h"
#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxcrt/utf16.h"
#include "core/fxge/cfx_face.h"
#include "core/fxge/cfx_font.h"

namespace {

struct RegisteredFont {
  CFX_FontRegistry::FontId id = CFX_FontRegistry::kInvalidFontId;
  ByteString base_font_name;
  ByteString family_name;  // as registered, spaces kept
  int weight = pdfium::kFontWeightNormal;
  bool italic = false;
  std::vector<uint32_t> supported_unicodes;
  DataVector<uint8_t> memory_data;
  RetainPtr<IFX_SeekableReadStream> stream;
};

struct RegistryState {
  CFX_FontRegistry::FontId next_font_id = 1;
  std::vector<std::unique_ptr<RegisteredFont>> fonts;
  std::vector<CFX_FontRegistry::FontId> fallback_order;
};

EPDF_TLS RegistryState* g_registry = nullptr;

RegistryState* GetRegistry() {
  if (!g_registry) {
    g_registry = new RegistryState();
  }
  return g_registry;
}

RegisteredFont* GetRegisteredFont(CFX_FontRegistry::FontId font_id) {
  if (font_id == CFX_FontRegistry::kInvalidFontId || !g_registry) {
    return nullptr;
  }

  for (const auto& font : g_registry->fonts) {
    if (font && font->id == font_id) {
      return font.get();
    }
  }
  return nullptr;
}

ByteString NormalizeBaseFontName(ByteString name) {
  name.Remove(' ');
  return name.IsEmpty() ? ByteString(CFX_Font::kUntitledFontName) : name;
}

// EmbedPDF: family comparison key. "Noto Sans", "NotoSans" and "noto sans"
// all name the same family; the PDF BaseFont has no spaces and Acrobat's
// /FontFamily keeps them.
ByteString NormalizeFamilyKey(ByteString name) {
  name.Remove(' ');
  name.MakeLower();
  return name;
}

int NormalizeWeight(int weight, const CFX_Font& font) {
  if (weight >= 100 && weight <= 900) {
    return weight;
  }
  return font.IsBold() ? pdfium::kFontWeightBold : pdfium::kFontWeightNormal;
}

bool NormalizeItalic(int italic, const CFX_Font& font) {
  if (italic == 0 || italic == 1) {
    return italic == 1;
  }
  return font.IsItalic();
}

DataVector<uint8_t> ReadStreamToData(IFX_SeekableReadStream* stream) {
  if (!stream || stream->GetSize() <= 0 ||
      !pdfium::IsValueInRangeForNumericType<size_t>(stream->GetSize())) {
    return {};
  }

  DataVector<uint8_t> data(pdfium::checked_cast<size_t>(stream->GetSize()));
  if (!stream->ReadBlockAtOffset(pdfium::span(data), /*offset=*/0)) {
    return {};
  }
  return data;
}

std::unique_ptr<CFX_Font> LoadFont(pdfium::span<const uint8_t> data) {
  if (data.empty()) {
    return nullptr;
  }

  auto font = std::make_unique<CFX_Font>();
  if (!font->LoadEmbedded(data, /*force_vertical=*/false, /*object_tag=*/0)) {
    return nullptr;
  }
  return font;
}

std::vector<uint32_t> CollectSupportedUnicodes(CFX_Font* font) {
  if (!font) {
    return {};
  }

  auto char_codes_and_indices =
      font->GetCharCodesAndIndices(pdfium::kMaximumSupplementaryCodePoint);
  std::vector<uint32_t> supported_unicodes;
  supported_unicodes.reserve(char_codes_and_indices.size());
  for (const auto& item : char_codes_and_indices) {
    if (item.glyph_index != 0) {
      supported_unicodes.push_back(item.char_code);
    }
  }

  std::ranges::sort(supported_unicodes);
  supported_unicodes.erase(
      std::unique(supported_unicodes.begin(), supported_unicodes.end()),
      supported_unicodes.end());
  return supported_unicodes;
}

CFX_FontRegistry::FontId RegisterLoadedFontSource(
    const ByteString& family_name,
    int weight,
    int italic,
    pdfium::span<const uint8_t> data,
    DataVector<uint8_t> memory_data,
    RetainPtr<IFX_SeekableReadStream> stream) {
  if (data.empty()) {
    return CFX_FontRegistry::kInvalidFontId;
  }

  RegistryState* registry = GetRegistry();
  if (registry->next_font_id ==
      std::numeric_limits<CFX_FontRegistry::FontId>::max()) {
    return CFX_FontRegistry::kInvalidFontId;
  }

  std::unique_ptr<CFX_Font> font = LoadFont(data);
  if (!font || !font->HasAnyGlyphs()) {
    return CFX_FontRegistry::kInvalidFontId;
  }

  std::vector<uint32_t> supported_unicodes =
      CollectSupportedUnicodes(font.get());
  if (supported_unicodes.empty()) {
    return CFX_FontRegistry::kInvalidFontId;
  }

  auto registered_font = std::make_unique<RegisteredFont>();
  registered_font->id = registry->next_font_id++;
  registered_font->base_font_name = NormalizeBaseFontName(
      family_name.IsEmpty() ? font->GetBaseFontName() : family_name);
  registered_font->family_name =
      family_name.IsEmpty() ? font->GetBaseFontName() : family_name;
  registered_font->family_name.Trim(' ');
  registered_font->weight = NormalizeWeight(weight, *font);
  registered_font->italic = NormalizeItalic(italic, *font);
  registered_font->supported_unicodes = std::move(supported_unicodes);
  registered_font->memory_data = std::move(memory_data);
  registered_font->stream = std::move(stream);

  const CFX_FontRegistry::FontId id = registered_font->id;
  registry->fonts.push_back(std::move(registered_font));
  return id;
}

int StyleScore(const RegisteredFont& font, int weight, bool italic) {
  const int weight_score = std::abs(font.weight - weight);
  const int italic_score = font.italic == italic ? 0 : 1000;
  return weight_score + italic_score;
}

}  // namespace

// static
CFX_FontRegistry::FontId CFX_FontRegistry::RegisterMemoryFont(
    const ByteString& family_name,
    int weight,
    int italic,
    pdfium::span<const uint8_t> data) {
  if (data.empty()) {
    return kInvalidFontId;
  }

  DataVector<uint8_t> memory_data(data.begin(), data.end());
  pdfium::span<const uint8_t> font_data(memory_data);
  return RegisterLoadedFontSource(family_name, weight, italic, font_data,
                                  std::move(memory_data), nullptr);
}

// static
CFX_FontRegistry::FontId CFX_FontRegistry::RegisterFont(
    const ByteString& family_name,
    int weight,
    int italic,
    RetainPtr<IFX_SeekableReadStream> stream) {
  DataVector<uint8_t> data = ReadStreamToData(stream.Get());
  return RegisterLoadedFontSource(family_name, weight, italic,
                                  pdfium::span(data), {}, std::move(stream));
}

// static
void CFX_FontRegistry::ClearRegisteredFonts() {
  if (!g_registry) {
    return;
  }
  g_registry->fallback_order.clear();
  g_registry->fonts.clear();
  // EmbedPDF: do not reset next_font_id. Documents can keep registered-font
  // marker resources after ClearRegisteredFonts(); reusing ids could make an
  // old marker resolve to a different font registered later in the same
  // runtime/thread.
}

// static
bool CFX_FontRegistry::AddFallbackFont(FontId font_id) {
  if (!IsValidFont(font_id)) {
    return false;
  }

  RegistryState* registry = GetRegistry();
  if (pdfium::Contains(registry->fallback_order, font_id)) {
    return true;
  }
  registry->fallback_order.push_back(font_id);
  return true;
}

// static
void CFX_FontRegistry::ClearFallbackFonts() {
  if (!g_registry) {
    return;
  }
  g_registry->fallback_order.clear();
}

// static
bool CFX_FontRegistry::HasFallbackFonts() {
  return g_registry && !g_registry->fallback_order.empty();
}

// static
bool CFX_FontRegistry::IsValidFont(FontId font_id) {
  return GetRegisteredFont(font_id) != nullptr;
}

// static
ByteString CFX_FontRegistry::GetBaseFontName(FontId font_id) {
  RegisteredFont* font = GetRegisteredFont(font_id);
  return font ? font->base_font_name : ByteString();
}

// static
ByteString CFX_FontRegistry::GetFamilyName(FontId font_id) {
  RegisteredFont* font = GetRegisteredFont(font_id);
  return font ? font->family_name : ByteString();
}

// static
std::optional<CFX_FontRegistry::FontId> CFX_FontRegistry::FindFont(
    const ByteString& family_name,
    int weight,
    bool italic) {
  if (!g_registry || family_name.IsEmpty()) {
    return std::nullopt;
  }

  ByteString exact = family_name;
  exact.Trim(' ');
  const ByteString wanted = NormalizeFamilyKey(family_name);
  if (wanted.IsEmpty()) {
    return std::nullopt;
  }

  // Exact family names outrank normalised ones: "Noto Sans" and "NotoSans"
  // may be two different registrations, and the one spelled as requested
  // wins before style score and registration order get a say.
  auto best_matching = [&](auto matches) -> std::optional<FontId> {
    std::optional<FontId> best_font_id;
    int best_score = std::numeric_limits<int>::max();
    for (const auto& font : g_registry->fonts) {
      if (!font || !matches(*font)) {
        continue;
      }
      const int score = StyleScore(*font, weight, italic);
      if (!best_font_id.has_value() || score < best_score) {
        best_font_id = font->id;
        best_score = score;
      }
    }
    return best_font_id;
  };
  if (std::optional<FontId> id = best_matching(
          [&](const RegisteredFont& f) { return f.family_name == exact; })) {
    return id;
  }
  return best_matching([&](const RegisteredFont& f) {
    return NormalizeFamilyKey(f.family_name) == wanted ||
           NormalizeFamilyKey(f.base_font_name) == wanted;
  });
}

// static
int CFX_FontRegistry::GetStyleWeight(FontId font_id) {
  RegisteredFont* font = GetRegisteredFont(font_id);
  return font ? font->weight : pdfium::kFontWeightNormal;
}

// static
bool CFX_FontRegistry::IsStyleItalic(FontId font_id) {
  RegisteredFont* font = GetRegisteredFont(font_id);
  return font && font->italic;
}

// static
bool CFX_FontRegistry::SupportsUnicode(FontId font_id, uint32_t unicode) {
  RegisteredFont* font = GetRegisteredFont(font_id);
  if (!font) {
    return false;
  }
  return std::ranges::binary_search(font->supported_unicodes, unicode);
}

// static
std::optional<CFX_FontRegistry::FontId>
CFX_FontRegistry::FindFallbackFont(uint32_t unicode, int weight, bool italic) {
  if (!g_registry) {
    return std::nullopt;
  }

  std::optional<FontId> best_font_id;
  int best_score = std::numeric_limits<int>::max();
  for (FontId font_id : g_registry->fallback_order) {
    RegisteredFont* font = GetRegisteredFont(font_id);
    if (!font || !SupportsUnicode(font_id, unicode)) {
      continue;
    }

    const int score = StyleScore(*font, weight, italic);
    if (!best_font_id.has_value() || score < best_score) {
      best_font_id = font_id;
      best_score = score;
    }
  }
  return best_font_id;
}

// static
std::unique_ptr<CFX_Font> CFX_FontRegistry::CreateFont(FontId font_id) {
  RegisteredFont* registered_font = GetRegisteredFont(font_id);
  if (!registered_font) {
    return nullptr;
  }

  if (!registered_font->memory_data.empty()) {
    return LoadFont(pdfium::span(registered_font->memory_data));
  }

  DataVector<uint8_t> data = ReadStreamToData(registered_font->stream.Get());
  return LoadFont(pdfium::span(data));
}

// static
void CFX_FontRegistry::DestroyGlobals() {
  delete g_registry;
  g_registry = nullptr;
}
