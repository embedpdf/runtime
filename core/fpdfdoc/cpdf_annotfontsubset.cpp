// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: builds PDF font dictionaries for registered annotation fonts,
// including per-annotation/layer subsets so large fallback fonts are not fully
// embedded into saved PDFs.

#include "core/fpdfdoc/cpdf_annotfontsubset.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/containers/contains.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_extension.h"
#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/fx_string.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/utf16.h"
#include "core/fxge/cfx_font.h"
#include "hb-subset.h"  // nogncheck

namespace {

constexpr char kRegisteredFontIdKey[] = "EmbedPDFRegisteredFontId";
constexpr uint32_t kMaxBfCharBfRangeEntries = 100;
constexpr uint32_t kMaxPdfCid = 0xffff;

enum class ObjectStorage {
  kDirect,
  kIndirect,
};

ByteString NormalizeBaseFontName(ByteString name) {
  name.Remove(' ');
  return name.IsEmpty() ? ByteString(CFX_Font::kUntitledFontName) : name;
}

ByteString BaseFontNameForRegisteredFont(CFX_FontRegistry::FontId font_id,
                                         const CFX_Font* font) {
  ByteString name = CFX_FontRegistry::GetBaseFontName(font_id);
  if (name.IsEmpty() && font) {
    name = font->GetBaseFontName();
  }
  return NormalizeBaseFontName(std::move(name));
}

RetainPtr<CPDF_Dictionary> NewDictionary(CPDF_Document* doc,
                                         ObjectStorage storage) {
  return storage == ObjectStorage::kIndirect
             ? doc->NewIndirect<CPDF_Dictionary>()
             : pdfium::MakeRetain<CPDF_Dictionary>();
}

RetainPtr<CPDF_Array> NewArray(CPDF_Document* doc, ObjectStorage storage) {
  return storage == ObjectStorage::kIndirect ? doc->NewIndirect<CPDF_Array>()
                                             : pdfium::MakeRetain<CPDF_Array>();
}

RetainPtr<CPDF_Stream> NewStream(CPDF_Document* doc,
                                 pdfium::span<const uint8_t> data,
                                 ObjectStorage storage) {
  return storage == ObjectStorage::kIndirect
             ? doc->NewIndirect<CPDF_Stream>(data)
             : pdfium::MakeRetain<CPDF_Stream>(data);
}

template <typename T>
void SetReferenceOrDirect(CPDF_Dictionary* dict,
                          const ByteString& key,
                          CPDF_Document* doc,
                          RetainPtr<T> object) {
  if (!object) {
    return;
  }

  const uint32_t obj_num = object->GetObjNum();
  if (obj_num != 0) {
    dict->SetNewFor<CPDF_Reference>(key, doc, obj_num);
    return;
  }

  dict->SetFor(key, RetainPtr<CPDF_Object>(std::move(object)));
}

template <typename T>
void AppendReferenceOrDirect(CPDF_Array* array,
                             CPDF_Document* doc,
                             RetainPtr<T> object) {
  if (!object) {
    return;
  }

  const uint32_t obj_num = object->GetObjNum();
  if (obj_num != 0) {
    array->AppendNew<CPDF_Reference>(doc, obj_num);
    return;
  }

  array->Append(RetainPtr<CPDF_Object>(std::move(object)));
}

ByteString MakeSubsetBaseFontName(
    const ByteString& base_font_name,
    const CPDF_AnnotFontSubset::GlyphUnicodeMap& glyph_to_unicode) {
  // EmbedPDF: a deterministic six-letter PDF subset tag is enough to keep
  // subsets distinguishable for readers/debugging. A theoretical hash collision
  // is harmless because each AP resource dictionary still points at its own
  // embedded subset font object.
  uint32_t hash = 2166136261u;
  auto mix = [&hash](uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      hash ^= (value >> (i * 8)) & 0xff;
      hash *= 16777619u;
    }
  };

  for (const auto& [glyph_id, unicode] : glyph_to_unicode) {
    mix(glyph_id);
    mix(unicode);
  }

  char prefix[7] = {};
  for (int i = 0; i < 6; ++i) {
    prefix[i] = static_cast<char>('A' + (hash % 26));
    hash = hash / 26 + 1;
  }
  return ByteString(prefix) + "+" + base_font_name;
}

RetainPtr<CPDF_Dictionary> CreateCompositeFontDict(CPDF_Document* doc,
                                                   const ByteString& name,
                                                   ObjectStorage storage) {
  auto font_dict = NewDictionary(doc, storage);
  font_dict->SetNewFor<CPDF_Name>("Type", "Font");
  font_dict->SetNewFor<CPDF_Name>("Subtype", "Type0");
  font_dict->SetNewFor<CPDF_Name>("Encoding", "Identity-H");
  font_dict->SetNewFor<CPDF_Name>("BaseFont", name);
  return font_dict;
}

RetainPtr<CPDF_Dictionary> CreateCidFontDict(CPDF_Document* doc,
                                             const ByteString& name,
                                             ObjectStorage storage) {
  auto cid_font_dict = NewDictionary(doc, storage);
  cid_font_dict->SetNewFor<CPDF_Name>("Type", "Font");
  cid_font_dict->SetNewFor<CPDF_Name>("Subtype", "CIDFontType2");
  cid_font_dict->SetNewFor<CPDF_Name>("BaseFont", name);
  cid_font_dict->SetNewFor<CPDF_Name>("CIDToGIDMap", "Identity");

  auto cid_system_info_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  cid_system_info_dict->SetNewFor<CPDF_String>("Registry", "Adobe");
  cid_system_info_dict->SetNewFor<CPDF_String>("Ordering", "Identity");
  cid_system_info_dict->SetNewFor<CPDF_Number>("Supplement", 0);
  cid_font_dict->SetFor("CIDSystemInfo", std::move(cid_system_info_dict));
  return cid_font_dict;
}

RetainPtr<CPDF_Dictionary> LoadFontDesc(
    CPDF_Document* doc,
    const ByteString& font_name,
    CFX_Font* font,
    pdfium::span<const uint8_t> font_data,
    const CPDF_AnnotFontSubset::FaceIdentity& identity,
    ObjectStorage storage,
    std::vector<uint32_t>* temporary_object_numbers) {
  auto font_descriptor_dict = NewDictionary(doc, storage);
  font_descriptor_dict->SetNewFor<CPDF_Name>("Type", "FontDescriptor");
  font_descriptor_dict->SetNewFor<CPDF_Name>("FontName", font_name);
  // EmbedPDF: persistent face identity (A1). Acrobat writes the same keys
  // for its own annotation fonts and re-resolves by them on edit.
  if (!identity.family.IsEmpty()) {
    font_descriptor_dict->SetNewFor<CPDF_String>(
        "FontFamily",
        WideString::FromUTF8(identity.family.AsStringView()).AsStringView());
  }
  font_descriptor_dict->SetNewFor<CPDF_Number>("FontWeight", identity.weight);

  int flags = pdfium::kFontStyleNonSymbolic;
  if (font->IsFixedWidth()) {
    flags |= pdfium::kFontStyleFixedPitch;
  }
  if (font_name.Contains("Serif")) {
    flags |= pdfium::kFontStyleSerif;
  }
  if (font->IsItalic()) {
    flags |= pdfium::kFontStyleItalic;
  }
  if (font->IsBold()) {
    flags |= pdfium::kFontStyleForceBold;
  }
  font_descriptor_dict->SetNewFor<CPDF_Number>("Flags", flags);

  FX_RECT bbox = font->GetBBox().value_or(FX_RECT());
  font_descriptor_dict->SetRectFor("FontBBox", CFX_FloatRect(bbox));
  font_descriptor_dict->SetNewFor<CPDF_Number>(
      "ItalicAngle", (identity.italic || font->IsItalic()) ? -12 : 0);
  font_descriptor_dict->SetNewFor<CPDF_Number>("Ascent", font->GetAscent());
  font_descriptor_dict->SetNewFor<CPDF_Number>("Descent", font->GetDescent());
  font_descriptor_dict->SetNewFor<CPDF_Number>("CapHeight", font->GetAscent());
  font_descriptor_dict->SetNewFor<CPDF_Number>("StemV",
                                               font->IsBold() ? 120 : 70);

  RetainPtr<CPDF_Stream> stream =
      storage == ObjectStorage::kDirect
          ? doc->NewIndirect<CPDF_Stream>(font_data)
          : NewStream(doc, font_data, ObjectStorage::kIndirect);
  stream->GetMutableDict()->SetNewFor<CPDF_Number>(
      "Length1", pdfium::checked_cast<int>(font_data.size()));
  if (temporary_object_numbers && storage == ObjectStorage::kDirect) {
    temporary_object_numbers->push_back(stream->GetObjNum());
  }
  font_descriptor_dict->SetNewFor<CPDF_Reference>("FontFile2", doc,
                                                  stream->GetObjNum());
  return font_descriptor_dict;
}

RetainPtr<CPDF_Array> CreateWidthsArray(
    CPDF_Document* doc,
    const std::map<uint32_t, uint32_t>& widths,
    ObjectStorage storage) {
  auto widths_array = NewArray(doc, storage);
  for (auto it = widths.begin(); it != widths.end(); ++it) {
    auto next_it = std::next(it);

    if (next_it != widths.end() && next_it->first == it->first + 1 &&
        next_it->second == it->second) {
      widths_array->AppendNew<CPDF_Number>(static_cast<int>(it->first));

      while (next_it != widths.end() && next_it->first == it->first + 1 &&
             next_it->second == it->second) {
        it = next_it;
        next_it = std::next(it);
      }
      widths_array->AppendNew<CPDF_Number>(static_cast<int>(it->first));
      widths_array->AppendNew<CPDF_Number>(static_cast<int>(it->second));
      continue;
    }

    widths_array->AppendNew<CPDF_Number>(static_cast<int>(it->first));
    auto current_width_array = pdfium::MakeRetain<CPDF_Array>();
    current_width_array->AppendNew<CPDF_Number>(static_cast<int>(it->second));

    while (next_it != widths.end() && next_it->first == it->first + 1) {
      it = next_it;
      next_it = std::next(it);
      current_width_array->AppendNew<CPDF_Number>(static_cast<int>(it->second));
    }
    widths_array->Append(std::move(current_width_array));
  }
  return widths_array;
}

const char kToUnicodeStart[] =
    "/CIDInit /ProcSet findresource begin\n"
    "12 dict begin\n"
    "begincmap\n"
    "/CIDSystemInfo\n"
    "<</Registry (Adobe)\n"
    "/Ordering (Identity)\n"
    "/Supplement 0\n"
    ">> def\n"
    "/CMapName /Adobe-Identity-H def\n"
    "/CMapType 2 def\n"
    "1 begincodespacerange\n"
    "<0000> <FFFF>\n"
    "endcodespacerange\n";

const char kToUnicodeEnd[] =
    "endcmap\n"
    "CMapName currentdict /CMap defineresource pop\n"
    "end\n"
    "end\n";

void AddCharcode(fxcrt::ostringstream& buffer, uint32_t number) {
  CHECK_LE(number, kMaxPdfCid);
  buffer << "<";
  char ans[4];
  FXSYS_IntToFourHexChars(number, ans);
  for (char c : ans) {
    buffer << c;
  }
  buffer << ">";
}

void AddUnicode(fxcrt::ostringstream& buffer, uint32_t unicode) {
  if (pdfium::IsHighSurrogate(unicode) || pdfium::IsLowSurrogate(unicode)) {
    unicode = 0;
  }

  char unicode_buf[8];
  pdfium::span<const char> unicode_span = FXSYS_ToUTF16BE(unicode, unicode_buf);
  CHECK(!unicode_span.empty());
  buffer << "<";
  for (char c : unicode_span) {
    buffer << c;
  }
  buffer << ">";
}

RetainPtr<CPDF_Stream> LoadUnicode(
    CPDF_Document* doc,
    const std::multimap<uint32_t, uint32_t>& to_unicode,
    ObjectStorage storage,
    std::vector<uint32_t>* temporary_object_numbers) {
  std::map<uint32_t, uint32_t> char_to_unicode_map;
  std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>>
      char_range_to_unicodes_map;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t>
      char_range_to_consecutive_unicodes_map;

  for (auto it = to_unicode.begin(); it != to_unicode.end(); ++it) {
    uint32_t first_charcode = it->first;
    uint32_t first_unicode = it->second;
    {
      auto next_it = std::next(it);
      if (next_it == to_unicode.end() || first_charcode + 1 != next_it->first) {
        char_to_unicode_map[first_charcode] = first_unicode;
        continue;
      }
    }

    ++it;
    uint32_t current_charcode = it->first;
    uint32_t current_unicode = it->second;
    if (current_charcode % 256 == 0) {
      char_to_unicode_map[first_charcode] = first_unicode;
      char_to_unicode_map[current_charcode] = current_unicode;
      continue;
    }

    const size_t max_extra = 255 - (current_charcode % 256);
    auto next_it = std::next(it);
    if (first_unicode + 1 != current_unicode) {
      std::vector<uint32_t> unicodes = {first_unicode, current_unicode};
      for (size_t i = 0; i < max_extra; ++i) {
        if (next_it == to_unicode.end() ||
            current_charcode + 1 != next_it->first) {
          break;
        }
        ++it;
        ++current_charcode;
        unicodes.push_back(it->second);
        next_it = std::next(it);
      }
      CHECK_EQ(it->first - first_charcode + 1, unicodes.size());
      char_range_to_unicodes_map[std::make_pair(first_charcode, it->first)] =
          std::move(unicodes);
      continue;
    }

    for (size_t i = 0; i < max_extra; ++i) {
      if (next_it == to_unicode.end() ||
          current_charcode + 1 != next_it->first ||
          current_unicode + 1 != next_it->second) {
        break;
      }
      ++it;
      ++current_charcode;
      ++current_unicode;
      next_it = std::next(it);
    }
    char_range_to_consecutive_unicodes_map[std::make_pair(
        first_charcode, current_charcode)] = first_unicode;
  }

  fxcrt::ostringstream buffer;
  buffer << kToUnicodeStart;

  uint32_t to_process =
      pdfium::checked_cast<uint32_t>(char_to_unicode_map.size());
  auto char_it = char_to_unicode_map.begin();
  while (to_process) {
    const uint32_t count = std::min(to_process, kMaxBfCharBfRangeEntries);
    buffer << count << " beginbfchar\n";
    for (uint32_t i = 0; i < count; ++i) {
      CHECK(char_it != char_to_unicode_map.end());
      AddCharcode(buffer, char_it->first);
      buffer << " ";
      AddUnicode(buffer, char_it->second);
      buffer << "\n";
      ++char_it;
    }
    buffer << "endbfchar\n";
    to_process -= count;
  }

  to_process =
      pdfium::checked_cast<uint32_t>(char_range_to_unicodes_map.size());
  auto range_it = char_range_to_unicodes_map.begin();
  while (to_process) {
    const uint32_t count = std::min(to_process, kMaxBfCharBfRangeEntries);
    buffer << count << " beginbfrange\n";
    for (uint32_t i = 0; i < count; ++i) {
      CHECK(range_it != char_range_to_unicodes_map.end());
      AddCharcode(buffer, range_it->first.first);
      buffer << " ";
      AddCharcode(buffer, range_it->first.second);
      buffer << " [";
      auto unicodes = pdfium::span(range_it->second);
      AddUnicode(buffer, unicodes[0]);
      for (uint32_t code : unicodes.subspan(1u)) {
        buffer << " ";
        AddUnicode(buffer, code);
      }
      buffer << "]\n";
      ++range_it;
    }
    buffer << "endbfrange\n";
    to_process -= count;
  }

  to_process = pdfium::checked_cast<uint32_t>(
      char_range_to_consecutive_unicodes_map.size());
  auto consecutive_it = char_range_to_consecutive_unicodes_map.begin();
  while (to_process) {
    const uint32_t count = std::min(to_process, kMaxBfCharBfRangeEntries);
    buffer << count << " beginbfrange\n";
    for (uint32_t i = 0; i < count; ++i) {
      CHECK(consecutive_it != char_range_to_consecutive_unicodes_map.end());
      AddCharcode(buffer, consecutive_it->first.first);
      buffer << " ";
      AddCharcode(buffer, consecutive_it->first.second);
      buffer << " ";
      AddUnicode(buffer, consecutive_it->second);
      buffer << "\n";
      ++consecutive_it;
    }
    buffer << "endbfrange\n";
    to_process -= count;
  }

  buffer << kToUnicodeEnd;
  RetainPtr<CPDF_Stream> stream = doc->NewIndirect<CPDF_Stream>(&buffer);
  if (temporary_object_numbers && storage == ObjectStorage::kDirect) {
    temporary_object_numbers->push_back(stream->GetObjNum());
  }
  return stream;
}

void CreateDescendantFontsArray(CPDF_Document* doc,
                                CPDF_Dictionary* font_dict,
                                RetainPtr<CPDF_Dictionary> cid_font_dict) {
  auto descendant_fonts_array =
      font_dict->SetNewFor<CPDF_Array>("DescendantFonts");
  AppendReferenceOrDirect(descendant_fonts_array.Get(), doc,
                          std::move(cid_font_dict));
}

DataVector<uint8_t> SubsetFontDataRetainGids(
    pdfium::span<const uint8_t> font_data,
    const CPDF_AnnotFontSubset::GlyphUnicodeMap& glyph_to_unicode) {
  // An empty map is a valid request: glyph 0 is always kept, and that
  // glyph-0-only program is the minimal /DA resource of an empty annotation.
  if (font_data.empty()) {
    return DataVector<uint8_t>();
  }

  hb_blob_t* source_blob =
      hb_blob_create(reinterpret_cast<const char*>(font_data.data()),
                     pdfium::checked_cast<unsigned int>(font_data.size()),
                     HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  if (!source_blob) {
    return DataVector<uint8_t>();
  }

  hb_face_t* source_face = hb_face_create(source_blob, 0);
  hb_blob_destroy(source_blob);
  if (!source_face) {
    return DataVector<uint8_t>();
  }

  hb_subset_input_t* input = hb_subset_input_create_or_fail();
  if (!input) {
    hb_face_destroy(source_face);
    return DataVector<uint8_t>();
  }

  hb_set_t* glyph_set = hb_subset_input_glyph_set(input);
  hb_set_add(glyph_set, 0);
  for (const auto& [glyph_id, unicode] : glyph_to_unicode) {
    if (glyph_id <= kMaxPdfCid) {
      hb_set_add(glyph_set, glyph_id);
    }
  }

  hb_subset_input_set_flags(
      input, HB_SUBSET_FLAGS_RETAIN_GIDS | HB_SUBSET_FLAGS_NO_HINTING);

  hb_face_t* subset_face = hb_subset_or_fail(source_face, input);
  hb_subset_input_destroy(input);
  hb_face_destroy(source_face);
  if (!subset_face) {
    return DataVector<uint8_t>();
  }

  hb_blob_t* subset_blob = hb_face_reference_blob(subset_face);
  hb_face_destroy(subset_face);
  if (!subset_blob) {
    return DataVector<uint8_t>();
  }

  unsigned int subset_length = 0;
  const char* subset_data = hb_blob_get_data(subset_blob, &subset_length);
  DataVector<uint8_t> result;
  if (subset_data && subset_length > 0) {
    result = DataVector<uint8_t>(
        reinterpret_cast<const uint8_t*>(subset_data),
        reinterpret_cast<const uint8_t*>(subset_data) + subset_length);
  }
  hb_blob_destroy(subset_blob);
  return result;
}

RetainPtr<CPDF_Dictionary> BuildCompositeFont(
    CPDF_Document* doc,
    CFX_Font* font,
    const ByteString& base_font_name,
    pdfium::span<const uint8_t> font_data,
    const CPDF_AnnotFontSubset::FaceIdentity& identity,
    const std::map<uint32_t, uint32_t>& widths,
    const std::multimap<uint32_t, uint32_t>& to_unicode,
    ObjectStorage storage,
    std::vector<uint32_t>* temporary_object_numbers) {
  if (!doc || !font || widths.empty()) {
    return nullptr;
  }

  RetainPtr<CPDF_Dictionary> font_dict =
      CreateCompositeFontDict(doc, base_font_name, storage);
  RetainPtr<CPDF_Dictionary> cid_font_dict =
      CreateCidFontDict(doc, base_font_name, storage);

  RetainPtr<CPDF_Dictionary> font_descriptor_dict =
      LoadFontDesc(doc, base_font_name, font, font_data, identity, storage,
                   temporary_object_numbers);
  SetReferenceOrDirect(cid_font_dict.Get(), "FontDescriptor", doc,
                       std::move(font_descriptor_dict));

  RetainPtr<CPDF_Array> widths_array = CreateWidthsArray(doc, widths, storage);
  SetReferenceOrDirect(cid_font_dict.Get(), "W", doc, std::move(widths_array));

  CreateDescendantFontsArray(doc, font_dict.Get(), std::move(cid_font_dict));

  if (!to_unicode.empty()) {  // /ToUnicode is optional; empty is meaningless
    RetainPtr<CPDF_Stream> to_unicode_stream =
        LoadUnicode(doc, to_unicode, storage, temporary_object_numbers);
    SetReferenceOrDirect(font_dict.Get(), "ToUnicode", doc,
                         std::move(to_unicode_stream));
  }
  return font_dict;
}

}  // namespace

CPDF_AnnotFontSubset::LayoutFont::LayoutFont() = default;

CPDF_AnnotFontSubset::LayoutFont::LayoutFont(LayoutFont&& that) noexcept =
    default;

CPDF_AnnotFontSubset::LayoutFont& CPDF_AnnotFontSubset::LayoutFont::operator=(
    LayoutFont&& that) noexcept = default;

CPDF_AnnotFontSubset::LayoutFont::~LayoutFont() = default;

// static
CPDF_AnnotFontSubset::LayoutFont CPDF_AnnotFontSubset::CreateLayoutFont(
    CPDF_Document* doc,
    CFX_FontRegistry::FontId font_id) {
  LayoutFont result;
  if (!doc || !CFX_FontRegistry::IsValidFont(font_id)) {
    return result;
  }

  std::unique_ptr<CFX_Font> font = CFX_FontRegistry::CreateFont(font_id);
  if (!font || !font->HasAnyGlyphs()) {
    return result;
  }

  auto char_codes_and_indices =
      font->GetCharCodesAndIndices(pdfium::kMaximumSupplementaryCodePoint);
  if (char_codes_and_indices.empty()) {
    return result;
  }

  std::multimap<uint32_t, uint32_t> to_unicode;
  std::map<uint32_t, uint32_t> widths;
  for (const auto& item : char_codes_and_indices) {
    if (item.glyph_index > kMaxPdfCid) {
      continue;
    }
    if (!pdfium::Contains(widths, item.glyph_index)) {
      widths[item.glyph_index] = font->GetGlyphWidth(item.glyph_index);
    }
    to_unicode.emplace(item.glyph_index, item.char_code);
  }
  if (widths.empty() || to_unicode.empty()) {
    return result;
  }

  const ByteString base_font_name =
      BaseFontNameForRegisteredFont(font_id, font.get());
  RetainPtr<CPDF_Dictionary> font_dict = BuildCompositeFont(
      doc, font.get(), base_font_name, font->GetFontSpan(),
      IdentityForRegisteredFont(font_id), widths, to_unicode,
      ObjectStorage::kDirect, &result.temporary_object_numbers);
  result.font = CPDF_Font::Create(doc, std::move(font_dict), nullptr);
  return result;
}

// static
RetainPtr<CPDF_Dictionary> CPDF_AnnotFontSubset::BuildRegisteredFontResource(
    CPDF_Document* doc,
    CFX_FontRegistry::FontId font_id,
    const GlyphUnicodeMap& glyph_to_unicode,
    bool required_by_default_appearance) {
  if (!doc || !CFX_FontRegistry::IsValidFont(font_id)) {
    return nullptr;
  }

  std::unique_ptr<CFX_Font> font = CFX_FontRegistry::CreateFont(font_id);
  if (!font || !font->HasAnyGlyphs()) {
    return nullptr;
  }

  GlyphUnicodeMap filtered_glyph_to_unicode;
  std::map<uint32_t, uint32_t> widths;
  std::multimap<uint32_t, uint32_t> to_unicode;
  for (const auto& [glyph_id, unicode] : glyph_to_unicode) {
    if (glyph_id == 0 || glyph_id > kMaxPdfCid) {
      continue;
    }
    filtered_glyph_to_unicode.emplace(glyph_id, unicode);
    widths[glyph_id] = font->GetGlyphWidth(glyph_id);
    to_unicode.emplace(glyph_id, unicode);
  }
  if (filtered_glyph_to_unicode.empty()) {
    if (!required_by_default_appearance) {
      return nullptr;  // an unused fallback font: nothing to embed
    }
    // Minimal resource: the subsetter always keeps glyph 0, so an empty
    // glyph set yields a valid program whose only glyph is .notdef.
    widths[0] = font->GetGlyphWidth(0);
  }

  // fsType "no subsetting" (0x0100): the licence allows embedding only the
  // whole program, so the resource carries it untagged.
  const bool may_subset = CFX_FontRegistry::AllowsSubsetting(font_id);
  DataVector<uint8_t> subset_font_data =
      may_subset ? SubsetFontDataRetainGids(font->GetFontSpan(),
                                            filtered_glyph_to_unicode)
                 : DataVector<uint8_t>();
  pdfium::span<const uint8_t> font_data = subset_font_data.empty()
                                              ? font->GetFontSpan()
                                              : pdfium::span(subset_font_data);

  const ByteString base_font_name =
      BaseFontNameForRegisteredFont(font_id, font.get());
  const ByteString subset_font_name =
      subset_font_data.empty()
          ? base_font_name
          : MakeSubsetBaseFontName(base_font_name, filtered_glyph_to_unicode);
  RetainPtr<CPDF_Dictionary> font_dict = BuildCompositeFont(
      doc, font.get(), subset_font_name, font_data,
      IdentityForRegisteredFont(font_id), widths, to_unicode,
      ObjectStorage::kIndirect, /*temporary_object_numbers=*/nullptr);
  if (font_dict) {
    // EmbedPDF: this dictionary is also what /DR names for the DA font, so it
    // carries the session hint. The descriptor's /FontFamily is the identity
    // that survives a new session; the hint is only a tie-breaker.
    font_dict->SetNewFor<CPDF_String>(kRegisteredFontIdKey,
                                      ByteString::Format("%u", font_id));
  }
  return font_dict;
}

// static
CPDF_AnnotFontSubset::FaceIdentity
CPDF_AnnotFontSubset::IdentityForRegisteredFont(
    CFX_FontRegistry::FontId font_id) {
  FaceIdentity identity;
  identity.family = CFX_FontRegistry::GetFamilyName(font_id);
  identity.weight = CFX_FontRegistry::GetStyleWeight(font_id);
  identity.italic = CFX_FontRegistry::IsStyleItalic(font_id);
  return identity;
}

// static
bool CPDF_AnnotFontSubset::IsEmbedPDFRegisteredFontDict(
    const CPDF_Dictionary* font_dict) {
  return font_dict && font_dict->KeyExist(kRegisteredFontIdKey);
}

// static
bool CPDF_AnnotFontSubset::IsLegacyMarkerFontDict(
    const CPDF_Dictionary* font_dict) {
  // Older files: a /Type1 stub with the hint and no descriptor. Acrobat
  // refuses to open the editor on it ("Bad parameter"), which is why A1
  // replaced it with the real subset.
  return IsEmbedPDFRegisteredFontDict(font_dict) &&
         font_dict->GetNameFor("Subtype") != "Type0" &&
         !font_dict->KeyExist("FontDescriptor");
}

namespace {

std::optional<uint32_t> ParseUnsignedDecimal(const ByteString& text) {
  if (text.IsEmpty()) {
    return std::nullopt;
  }
  uint32_t value = 0;
  for (char ch : text.AsStringView()) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    FX_SAFE_UINT32 safe_value = value;
    safe_value *= 10;
    safe_value += ch - '0';
    if (!safe_value.IsValid()) {
      return std::nullopt;
    }
    value = safe_value.ValueOrDie();
  }
  return value;
}

// The descriptor of a Type0 font lives on its descendant; simple fonts (the
// legacy marker has none) carry it directly.
RetainPtr<const CPDF_Dictionary> FontDescriptorOf(
    const CPDF_Dictionary* font_dict) {
  if (!font_dict) {
    return nullptr;
  }
  if (font_dict->GetNameFor("Subtype") == "Type0") {
    RetainPtr<const CPDF_Array> descendants =
        font_dict->GetArrayFor("DescendantFonts");
    RetainPtr<const CPDF_Dictionary> cid_font =
        descendants ? descendants->GetDictAt(0) : nullptr;
    return cid_font ? cid_font->GetDictFor("FontDescriptor") : nullptr;
  }
  return font_dict->GetDictFor("FontDescriptor");
}

}  // namespace

// static
std::optional<CFX_FontRegistry::FontId>
CPDF_AnnotFontSubset::ResolveRegisteredFont(const CPDF_Dictionary* font_dict) {
  if (!IsEmbedPDFRegisteredFontDict(font_dict)) {
    return std::nullopt;
  }

  std::optional<uint32_t> hint =
      ParseUnsignedDecimal(font_dict->GetByteStringFor(kRegisteredFontIdKey));
  const bool hint_valid =
      hint.has_value() && CFX_FontRegistry::IsValidFont(*hint);

  RetainPtr<const CPDF_Dictionary> descriptor = FontDescriptorOf(font_dict);
  ByteString family;
  if (descriptor) {
    family = descriptor->GetUnicodeTextFor("FontFamily").ToUTF8();
  }
  if (!family.IsEmpty()) {
    // The stored family is authoritative. If no registered font matches it,
    // the dictionary stays unresolved: the caller keeps the embedded program
    // and ordinary fallback. A numeric hint pointing at some other family
    // must never win (ids are session-local and get reused).
    const int weight = descriptor->GetIntegerFor("FontWeight", 400);
    const bool italic = descriptor->GetIntegerFor("ItalicAngle", 0) != 0;
    return CFX_FontRegistry::FindFont(family, weight, italic);
  }

  // Legacy marker (pre-A1, no descriptor): /BaseFont is the stored identity;
  // the hint is accepted only when it names the same base font.
  const ByteString base_font = font_dict->GetNameFor("BaseFont");
  if (base_font.IsEmpty()) {
    return std::nullopt;
  }
  if (std::optional<CFX_FontRegistry::FontId> by_name =
          CFX_FontRegistry::FindFont(base_font, 400, false)) {
    return by_name;
  }
  if (hint_valid && CFX_FontRegistry::GetBaseFontName(*hint) == base_font) {
    return hint;
  }
  return std::nullopt;
}
