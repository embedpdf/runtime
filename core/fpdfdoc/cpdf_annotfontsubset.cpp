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
#include <set>
#include <sstream>
#include <utility>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_indirect_object_holder.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
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
#include "core/fxge/cfx_face.h"
#include "core/fxge/cfx_font.h"
#include "hb-subset.h"  // nogncheck

namespace {

constexpr char kRegisteredFontIdKey[] = "EmbedPDFRegisteredFontId";
constexpr uint32_t kMaxBfCharBfRangeEntries = 100;
constexpr uint32_t kMaxPdfCid = 0xffff;

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

// Links one part of a composite font into its parent: a part that already
// has an object number (published, or a stream in the layout scratch holder)
// is referenced through |holder|; one without is nested directly.
template <typename T>
void SetReferenceOrDirect(CPDF_Dictionary* dict,
                          const ByteString& key,
                          CPDF_IndirectObjectHolder* holder,
                          RetainPtr<T> object) {
  if (!object) {
    return;
  }

  const uint32_t obj_num = object->GetObjNum();
  if (obj_num != 0) {
    dict->SetNewFor<CPDF_Reference>(key, holder, obj_num);
    return;
  }

  dict->SetFor(key, RetainPtr<CPDF_Object>(std::move(object)));
}

template <typename T>
void AppendReferenceOrDirect(CPDF_Array* array,
                             CPDF_IndirectObjectHolder* holder,
                             RetainPtr<T> object) {
  if (!object) {
    return;
  }

  const uint32_t obj_num = object->GetObjNum();
  if (obj_num != 0) {
    array->AppendNew<CPDF_Reference>(holder, obj_num);
    return;
  }

  array->Append(RetainPtr<CPDF_Object>(std::move(object)));
}

// A FontFile3 /OpenType program needs PDF 1.6. Raise the catalog's /Version
// when the loaded file is older; new documents are written as 1.7 already.
void EnsureMinimumPdfVersion16(CPDF_Document* doc) {
  const CPDF_Parser* parser = doc->GetParser();
  if (!parser || parser->GetFileVersion() >= 16) {
    return;
  }
  RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
  if (!root) {
    return;
  }
  const ByteString current = root->GetNameFor("Version");
  if (!current.IsEmpty() && current.Compare("1.6") >= 0) {
    return;
  }
  root->SetNewFor<CPDF_Name>("Version", "1.6");
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

RetainPtr<CPDF_Dictionary> CreateCompositeFontDict(const ByteString& name) {
  auto font_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  font_dict->SetNewFor<CPDF_Name>("Type", "Font");
  font_dict->SetNewFor<CPDF_Name>("Subtype", "Type0");
  font_dict->SetNewFor<CPDF_Name>("Encoding", "Identity-H");
  font_dict->SetNewFor<CPDF_Name>("BaseFont", name);
  return font_dict;
}

RetainPtr<CPDF_Dictionary> CreateCidFontDict(
    const ByteString& name,
    CPDF_AnnotFontSubset::ProgramFormat format) {
  auto cid_font_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  cid_font_dict->SetNewFor<CPDF_Name>("Type", "Font");
  cid_font_dict->SetNewFor<CPDF_Name>("BaseFont", name);
  if (format == CPDF_AnnotFontSubset::ProgramFormat::kOpenTypeCFF) {
    // ISO 32000-2 9.7.4.2: a CFF program is a Type 0 CIDFont; CIDs select
    // glyphs directly, or through the charset when the CFF is CID-keyed. No
    // CIDToGIDMap exists for this type.
    cid_font_dict->SetNewFor<CPDF_Name>("Subtype", "CIDFontType0");
  } else {
    cid_font_dict->SetNewFor<CPDF_Name>("Subtype", "CIDFontType2");
    cid_font_dict->SetNewFor<CPDF_Name>("CIDToGIDMap", "Identity");
  }

  auto cid_system_info_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  cid_system_info_dict->SetNewFor<CPDF_String>("Registry", "Adobe");
  cid_system_info_dict->SetNewFor<CPDF_String>("Ordering", "Identity");
  cid_system_info_dict->SetNewFor<CPDF_Number>("Supplement", 0);
  cid_font_dict->SetFor("CIDSystemInfo", std::move(cid_system_info_dict));
  return cid_font_dict;
}

// The descriptor without its FontFile entry; LinkCompositeFont adds that
// once the program stream has an object number.
RetainPtr<CPDF_Dictionary> LoadFontDesc(
    const ByteString& font_name,
    CFX_Font* font,
    const CPDF_AnnotFontSubset::FaceIdentity& identity) {
  auto font_descriptor_dict = pdfium::MakeRetain<CPDF_Dictionary>();
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
  return font_descriptor_dict;
}

// The embedded program as an unowned stream (a copy of |font_data|).
RetainPtr<CPDF_Stream> CreateProgramStream(
    pdfium::span<const uint8_t> font_data,
    CPDF_AnnotFontSubset::ProgramFormat format) {
  auto stream = pdfium::MakeRetain<CPDF_Stream>(font_data);
  if (format == CPDF_AnnotFontSubset::ProgramFormat::kOpenTypeCFF) {
    // The whole OpenType program (PDF 1.6, Table 124): FontFile3 /OpenType.
    stream->GetMutableDict()->SetNewFor<CPDF_Name>("Subtype", "OpenType");
  } else {
    stream->GetMutableDict()->SetNewFor<CPDF_Number>(
        "Length1", pdfium::checked_cast<int>(font_data.size()));
  }
  return stream;
}

RetainPtr<CPDF_Array> CreateWidthsArray(
    const std::map<uint32_t, uint32_t>& widths) {
  auto widths_array = pdfium::MakeRetain<CPDF_Array>();
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

// The ToUnicode CMap as an unowned stream.
RetainPtr<CPDF_Stream> LoadUnicode(
    const std::multimap<uint32_t, uint32_t>& to_unicode) {
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
  return pdfium::MakeRetain<CPDF_Stream>(&buffer);
}

DataVector<uint8_t> SubsetFontDataRetainGids(
    pdfium::span<const uint8_t> font_data,
    const std::set<uint32_t>& glyph_ids) {
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
  for (uint32_t glyph_id : glyph_ids) {
    hb_set_add(glyph_set, glyph_id);
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

// The parts of a composite font, built but not linked: no part references
// another and none has an object number. LinkCompositeFont joins them once
// the streams have numbers (in a layout scratch holder, or in the document
// at publish time). Returns false for a program the writer cannot emit.
bool BuildCompositeFontParts(
    CFX_Font* font,
    const ByteString& base_font_name,
    pdfium::span<const uint8_t> font_data,
    const CPDF_AnnotFontSubset::FaceIdentity& identity,
    const std::map<uint32_t, uint32_t>& widths,
    const std::multimap<uint32_t, uint32_t>& to_unicode,
    CPDF_AnnotFontSubset::StagedFontResource* parts) {
  if (!font || widths.empty()) {
    return false;
  }
  const CPDF_AnnotFontSubset::ProgramFormat format =
      CPDF_AnnotFontSubset::DetectProgramFormat(font_data);
  if (format == CPDF_AnnotFontSubset::ProgramFormat::kUnsupported) {
    return false;
  }

  parts->format = format;
  parts->font_dict = CreateCompositeFontDict(base_font_name);
  parts->cid_font_dict = CreateCidFontDict(base_font_name, format);
  parts->descriptor = LoadFontDesc(base_font_name, font, identity);
  parts->widths = CreateWidthsArray(widths);
  parts->program = CreateProgramStream(font_data, format);
  // /ToUnicode is optional; an empty map is meaningless.
  parts->to_unicode = to_unicode.empty() ? nullptr : LoadUnicode(to_unicode);
  return true;
}

// Joins the parts. Streams must have object numbers in |holder| by now (a
// stream cannot be nested directly); dictionaries and the widths array are
// referenced when they have one and nested when they do not.
void LinkCompositeFont(CPDF_AnnotFontSubset::StagedFontResource* parts,
                       CPDF_IndirectObjectHolder* holder) {
  CHECK(parts->program->GetObjNum() != 0);
  parts->descriptor->SetNewFor<CPDF_Reference>(
      parts->format == CPDF_AnnotFontSubset::ProgramFormat::kOpenTypeCFF
          ? "FontFile3"
          : "FontFile2",
      holder, parts->program->GetObjNum());
  SetReferenceOrDirect(parts->cid_font_dict.Get(), "FontDescriptor", holder,
                       parts->descriptor);
  SetReferenceOrDirect(parts->cid_font_dict.Get(), "W", holder, parts->widths);
  auto descendant_fonts_array =
      parts->font_dict->SetNewFor<CPDF_Array>("DescendantFonts");
  AppendReferenceOrDirect(descendant_fonts_array.Get(), holder,
                          parts->cid_font_dict);
  if (parts->to_unicode) {
    CHECK(parts->to_unicode->GetObjNum() != 0);
    parts->font_dict->SetNewFor<CPDF_Reference>("ToUnicode", holder,
                                                parts->to_unicode->GetObjNum());
  }
}

}  // namespace

CPDF_AnnotFontSubset::LayoutFont::LayoutFont() = default;

CPDF_AnnotFontSubset::LayoutFont::LayoutFont(LayoutFont&& that) noexcept =
    default;

CPDF_AnnotFontSubset::LayoutFont& CPDF_AnnotFontSubset::LayoutFont::operator=(
    LayoutFont&& that) noexcept = default;

CPDF_AnnotFontSubset::LayoutFont::~LayoutFont() = default;

CPDF_AnnotFontSubset::StagedFontResource::StagedFontResource() = default;

CPDF_AnnotFontSubset::StagedFontResource::StagedFontResource(
    StagedFontResource&& that) noexcept = default;

CPDF_AnnotFontSubset::StagedFontResource&
CPDF_AnnotFontSubset::StagedFontResource::operator=(
    StagedFontResource&& that) noexcept = default;

CPDF_AnnotFontSubset::StagedFontResource::~StagedFontResource() = default;

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

  // The layout font has the same shape as the resource that will be written,
  // so the charcodes it hands out (CIDs under Identity-H) are the ones the
  // appearance bytes carry. For a CID-keyed CFF that is the charset's CID.
  const GlyphIdentity identity(font.get());
  std::multimap<uint32_t, uint32_t> to_unicode;
  std::map<uint32_t, uint32_t> widths;
  for (const auto& item : char_codes_and_indices) {
    std::optional<uint16_t> cid = identity.CidOf(item.glyph_index);
    if (!cid.has_value()) {
      continue;
    }
    if (!pdfium::Contains(widths, *cid)) {
      widths[*cid] = font->GetGlyphWidth(item.glyph_index);
    }
    to_unicode.emplace(*cid, item.char_code);
  }
  if (widths.empty() || to_unicode.empty()) {
    return result;
  }

  const ByteString base_font_name =
      BaseFontNameForRegisteredFont(font_id, font.get());
  StagedFontResource parts;
  if (!BuildCompositeFontParts(font.get(), base_font_name, font->GetFontSpan(),
                               IdentityForRegisteredFont(font_id), widths,
                               to_unicode, &parts)) {
    return result;
  }
  // The streams go into a scratch holder of their own: CPDF_Font resolves
  // the descriptor's FontFile reference through the holder the reference
  // names, so the document's object space is never touched for layout.
  result.scratch = std::make_unique<CPDF_IndirectObjectHolder>();
  result.scratch->AddIndirectObject(parts.program);
  if (parts.to_unicode) {
    result.scratch->AddIndirectObject(parts.to_unicode);
  }
  LinkCompositeFont(&parts, result.scratch.get());
  result.font = CPDF_Font::Create(doc, std::move(parts.font_dict), nullptr);
  return result;
}

// static
CPDF_AnnotFontSubset::StageStatus
CPDF_AnnotFontSubset::StageRegisteredFontResource(
    CFX_FontRegistry::FontId font_id,
    const GlyphUnicodeMap& glyph_to_unicode,
    bool required,
    Embedding embedding,
    StagedFontResource* out) {
  if (!out || !CFX_FontRegistry::IsValidFont(font_id)) {
    return StageStatus::kFailed;
  }

  std::unique_ptr<CFX_Font> font = CFX_FontRegistry::CreateFont(font_id);
  if (!font || !font->HasAnyGlyphs()) {
    return StageStatus::kFailed;
  }

  // The map is keyed by charcode (== CID). Resolve every glyph's three
  // identities: subset membership by GID, /W by CID, ToUnicode by charcode.
  const GlyphIdentity identity(font.get());
  GlyphUnicodeMap filtered_glyph_to_unicode;  // charcode → unicode, for the tag
  std::set<uint32_t> subset_glyphs;
  std::map<uint32_t, uint32_t> widths;           // CID → width
  std::multimap<uint32_t, uint32_t> to_unicode;  // charcode → unicode
  for (const auto& [charcode, unicode] : glyph_to_unicode) {
    if (charcode == 0 || charcode > kMaxPdfCid) {
      continue;
    }
    std::optional<uint32_t> gid =
        identity.GidOf(static_cast<uint16_t>(charcode));
    if (!gid.has_value() || *gid == 0) {
      continue;
    }
    filtered_glyph_to_unicode.emplace(charcode, unicode);
    subset_glyphs.insert(*gid);
    widths[charcode] = font->GetGlyphWidth(*gid);
    to_unicode.emplace(charcode, unicode);
  }
  if (filtered_glyph_to_unicode.empty()) {
    if (!required) {
      return StageStatus::kUnused;  // an unused fallback: nothing to embed
    }
    // Minimal resource: the subsetter always keeps glyph 0, so an empty
    // glyph set yields a valid program whose only glyph is .notdef.
    widths[0] = font->GetGlyphWidth(0);
  }

  // fsType "no subsetting" (0x0100): the licence allows embedding only the
  // whole program, whatever the policy says. Otherwise the caller's choice
  // (§2 of the Phase C note) decides; the whole program is untagged.
  const bool may_subset = embedding == Embedding::kSubset &&
                          CFX_FontRegistry::AllowsSubsetting(font_id);
  DataVector<uint8_t> subset_font_data =
      may_subset ? SubsetFontDataRetainGids(font->GetFontSpan(), subset_glyphs)
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
  StagedFontResource staged;
  if (!BuildCompositeFontParts(font.get(), subset_font_name, font_data,
                               IdentityForRegisteredFont(font_id), widths,
                               to_unicode, &staged)) {
    return StageStatus::kFailed;
  }
  staged.font_id = font_id;
  // EmbedPDF: this dictionary is also what /DR names for the DA font, so it
  // carries the session hint. The descriptor's /FontFamily is the identity
  // that survives a new session; the hint is only a tie-breaker.
  staged.font_dict->SetNewFor<CPDF_String>(kRegisteredFontIdKey,
                                           ByteString::Format("%u", font_id));
  *out = std::move(staged);
  return StageStatus::kStaged;
}

// static
CPDF_AnnotFontSubset::StageStatus
CPDF_AnnotFontSubset::StageDocumentProgramResource(
    const CFX_Font* font,
    RetainPtr<CPDF_Stream> program_stream,
    const ByteString& base_font_name,
    const FaceIdentity& identity,
    const GlyphUnicodeMap& glyph_to_unicode,
    bool required,
    StagedFontResource* out) {
  if (!out || !font || !program_stream || program_stream->GetObjNum() == 0) {
    return StageStatus::kFailed;
  }
  const GlyphIdentity glyph_identity(font);
  std::map<uint32_t, uint32_t> widths;
  std::multimap<uint32_t, uint32_t> to_unicode;
  for (const auto& [charcode, unicode] : glyph_to_unicode) {
    if (charcode == 0 || charcode > kMaxPdfCid) {
      continue;
    }
    std::optional<uint32_t> gid =
        glyph_identity.GidOf(static_cast<uint16_t>(charcode));
    if (!gid.has_value() || *gid == 0) {
      continue;
    }
    widths[charcode] = font->GetGlyphWidth(*gid);
    to_unicode.emplace(charcode, unicode);
  }
  if (widths.empty()) {
    if (!required) {
      return StageStatus::kUnused;
    }
    widths[0] = font->GetGlyphWidth(0);
  }
  // The parts are built against the program's bytes for the format; the
  // stream object itself is what the descriptor will reference.
  StagedFontResource staged;
  if (!BuildCompositeFontParts(
          const_cast<CFX_Font*>(font), NormalizeBaseFontName(base_font_name),
          font->GetFontSpan(), identity, widths, to_unicode, &staged)) {
    return StageStatus::kFailed;
  }
  staged.program = std::move(program_stream);  // never a copy
  *out = std::move(staged);
  return StageStatus::kStaged;
}

// static
RetainPtr<CPDF_Dictionary> CPDF_AnnotFontSubset::PublishStagedFontResource(
    CPDF_Document* doc,
    StagedFontResource staged) {
  if (!doc || !staged.font_dict || !staged.cid_font_dict ||
      !staged.descriptor || !staged.widths || !staged.program) {
    return nullptr;
  }
  // Leaves first, so every reference written by LinkCompositeFont names an
  // object that is already in the document. A document program's stream is
  // already there.
  if (staged.program->GetObjNum() == 0) {
    doc->AddIndirectObject(staged.program);
  }
  if (staged.to_unicode) {
    doc->AddIndirectObject(staged.to_unicode);
  }
  doc->AddIndirectObject(staged.widths);
  doc->AddIndirectObject(staged.descriptor);
  doc->AddIndirectObject(staged.cid_font_dict);
  doc->AddIndirectObject(staged.font_dict);
  LinkCompositeFont(&staged, doc);
  if (staged.format == ProgramFormat::kOpenTypeCFF) {
    EnsureMinimumPdfVersion16(doc);  // FontFile3 /OpenType is PDF 1.6
  }
  return staged.font_dict;
}

// static
RetainPtr<CPDF_Dictionary> CPDF_AnnotFontSubset::BuildRegisteredFontResource(
    CPDF_Document* doc,
    CFX_FontRegistry::FontId font_id,
    const GlyphUnicodeMap& glyph_to_unicode,
    bool required,
    Embedding embedding) {
  if (!doc) {
    return nullptr;
  }
  StagedFontResource staged;
  if (StageRegisteredFontResource(font_id, glyph_to_unicode,
                                  required, embedding,
                                  &staged) != StageStatus::kStaged) {
    return nullptr;
  }
  return PublishStagedFontResource(doc, std::move(staged));
}

CPDF_AnnotFontSubset::GlyphIdentity::GlyphIdentity(const CFX_Font* font) {
  RetainPtr<CFX_Face> face = font ? font->GetFace() : nullptr;
  if (!face || !face->IsTtOt() || !face->IsCidKeyed()) {
    return;  // identity: CID == GID
  }
  identity_ = false;
  const int glyph_count = face->GetGlyphCount();
  for (uint32_t gid = 0; static_cast<int>(gid) < glyph_count; ++gid) {
    std::optional<uint32_t> cid = face->GetCidFromGlyphIndex(gid);
    if (!cid.has_value() || *cid > kMaxPdfCid) {
      continue;
    }
    gid_to_cid_.emplace(gid, static_cast<uint16_t>(*cid));
    cid_to_gid_.emplace(static_cast<uint16_t>(*cid), gid);
  }
}

std::optional<uint16_t> CPDF_AnnotFontSubset::GlyphIdentity::CidOf(
    uint32_t gid) const {
  if (identity_) {
    return gid <= kMaxPdfCid
               ? std::optional<uint16_t>(static_cast<uint16_t>(gid))
               : std::nullopt;
  }
  auto it = gid_to_cid_.find(gid);
  return it != gid_to_cid_.end() ? std::optional<uint16_t>(it->second)
                                 : std::nullopt;
}

std::optional<uint32_t> CPDF_AnnotFontSubset::GlyphIdentity::GidOf(
    uint16_t cid) const {
  if (identity_) {
    return cid;
  }
  auto it = cid_to_gid_.find(cid);
  return it != cid_to_gid_.end() ? std::optional<uint32_t>(it->second)
                                 : std::nullopt;
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
