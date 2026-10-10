// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_fontface.h"

#include <algorithm>
#include <optional>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fxge/cfx_fontregistry.h"
#include "core/fxge/fx_font.h"

namespace {

struct StandardAlias {
  const char* alias;
  const wchar_t* family;
  int weight;
  bool italic;
};

constexpr StandardAlias kStandardAliases[] = {
    {"Helv", L"Helvetica", 400, false},
    {"HeBo", L"Helvetica", 700, false},
    {"HeOb", L"Helvetica", 400, true},
    {"HeBO", L"Helvetica", 700, true},
    {"Helvetica", L"Helvetica", 400, false},
    {"Helvetica-Bold", L"Helvetica", 700, false},
    {"Helvetica-Oblique", L"Helvetica", 400, true},
    {"Helvetica-BoldOblique", L"Helvetica", 700, true},
    {"Arial", L"Helvetica", 400, false},
    {"Arial-BoldMT", L"Helvetica", 700, false},
    {"ArialMT", L"Helvetica", 400, false},
    {"TiRo", L"Times", 400, false},
    {"TiBo", L"Times", 700, false},
    {"TiIt", L"Times", 400, true},
    {"TiBI", L"Times", 700, true},
    {"Times-Roman", L"Times", 400, false},
    {"Times-Bold", L"Times", 700, false},
    {"Times-Italic", L"Times", 400, true},
    {"Times-BoldItalic", L"Times", 700, true},
    {"Cour", L"Courier", 400, false},
    {"CoBo", L"Courier", 700, false},
    {"CoOb", L"Courier", 400, true},
    {"CoBO", L"Courier", 700, true},
    {"Courier", L"Courier", 400, false},
    {"Courier-Bold", L"Courier", 700, false},
    {"Courier-Oblique", L"Courier", 400, true},
    {"Courier-BoldOblique", L"Courier", 700, true},
    {"Symb", L"Symbol", 400, false},
    {"Symbol", L"Symbol", 400, false},
    {"ZaDb", L"ZapfDingbats", 400, false},
    {"ZapfDingbats", L"ZapfDingbats", 400, false},
};

}  // namespace

std::optional<CPDF_FontFace> StandardFontFace(const ByteString& name) {
  for (const StandardAlias& entry : kStandardAliases) {
    if (name == entry.alias) {
      return CPDF_FontFace{entry.family, entry.weight, entry.italic};
    }
  }
  return std::nullopt;
}

CPDF_FontFace FaceOfBaseFontName(ByteString base_font) {
  if (base_font.GetLength() > 7 && base_font[6] == '+') {
    base_font = base_font.Substr(7);
  }
  if (std::optional<CPDF_FontFace> standard = StandardFontFace(base_font)) {
    return *standard;
  }
  std::optional<size_t> cut = base_font.Find('-');
  std::optional<size_t> comma = base_font.Find(',');
  if (comma.has_value() && (!cut.has_value() || *comma < *cut)) {
    cut = comma;
  }
  CPDF_FontFace face;
  face.family = WideString::FromUTF8(
      (cut.has_value() ? base_font.First(*cut) : base_font).AsStringView());
  if (!cut.has_value()) {
    return face;
  }
  const ByteString suffix = base_font.Substr(*cut + 1);
  if (suffix.Contains("Bold")) {
    face.weight = 700;
  } else if (suffix.Contains("Light")) {
    face.weight = 300;
  } else if (suffix.Contains("Medium")) {
    face.weight = 500;
  } else if (suffix.Contains("Semibold") || suffix.Contains("Demibold")) {
    face.weight = 600;
  } else if (suffix.Contains("Black") || suffix.Contains("Heavy")) {
    face.weight = 900;
  }
  face.italic = suffix.Contains("Italic") || suffix.Contains("Oblique");
  return face;
}

RetainPtr<const CPDF_Dictionary> FontDescriptorOf(
    const CPDF_Dictionary* font_dict) {
  if (!font_dict) {
    return nullptr;
  }
  if (RetainPtr<const CPDF_Dictionary> descriptor =
          font_dict->GetDictFor("FontDescriptor")) {
    return descriptor;
  }
  RetainPtr<const CPDF_Array> descendants =
      font_dict->GetArrayFor("DescendantFonts");
  RetainPtr<const CPDF_Dictionary> cid_font =
      descendants ? descendants->GetDictAt(0) : nullptr;
  return cid_font ? cid_font->GetDictFor("FontDescriptor") : nullptr;
}

CPDF_FontFace FaceOfFontDict(const CPDF_Dictionary* font_dict) {
  CPDF_FontFace face = FaceOfBaseFontName(font_dict->GetNameFor("BaseFont"));
  RetainPtr<const CPDF_Dictionary> descriptor = FontDescriptorOf(font_dict);
  if (!descriptor) {
    return face;
  }
  const WideString family = descriptor->GetUnicodeTextFor("FontFamily");
  if (!family.IsEmpty()) {
    face.family = family;
  }
  const int flags = descriptor->GetIntegerFor("Flags", 0);
  if (descriptor->KeyExist("FontWeight")) {
    face.weight = descriptor->GetIntegerFor("FontWeight", face.weight);
  } else if (flags & pdfium::kFontStyleForceBold) {
    face.weight = std::max(face.weight, 700);
  }
  if (descriptor->GetIntegerFor("ItalicAngle", 0) != 0 ||
      (flags & pdfium::kFontStyleItalic)) {
    face.italic = true;
  }
  return face;
}

CPDF_FontFace FaceOfDefaultAppearanceFont(
    const CPDF_Document* doc,
    const ByteString& font_name,
    std::initializer_list<const CPDF_Dictionary*> default_resources) {
  for (const CPDF_Dictionary* dr : default_resources) {
    RetainPtr<const CPDF_Dictionary> fonts =
        dr ? dr->GetDictFor("Font") : nullptr;
    RetainPtr<const CPDF_Dictionary> font_dict =
        fonts ? fonts->GetDictFor(font_name.AsStringView()) : nullptr;
    if (font_dict) {
      return FaceOfFontDict(font_dict.Get());
    }
  }
  // A registered font's alias before its /DR entry exists (a draft ahead of
  // its first appearance): the registry knows the face.
  if (doc) {
    std::optional<CFX_FontRegistry::FontId> registered =
        CPDF_AnnotFontMap::RegisteredFontIdFromAlias(doc, font_name);
    if (registered.has_value()) {
      return CPDF_FontFace{
          WideString::FromUTF8(
              CFX_FontRegistry::GetFamilyName(*registered).AsStringView()),
          CFX_FontRegistry::GetStyleWeight(*registered),
          CFX_FontRegistry::IsStyleItalic(*registered)};
    }
  }
  if (std::optional<CPDF_FontFace> standard = StandardFontFace(font_name)) {
    return *standard;
  }
  return CPDF_FontFace{WideString::FromUTF8(font_name.AsStringView())};
}
