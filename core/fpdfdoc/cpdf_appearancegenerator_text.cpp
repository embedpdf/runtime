// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

// EmbedPDF: the appearance generator's text for free text and form fields: /DA
// strings, fonts resolved against /DR, and the variable-text edit appearance.
//
// See docs/embedpdf/fork-ledger.md, rule own-appearance-generator. EmbedPDF's
// changes are Copyright 2026 CloudPDF LTD.

#include <optional>
#include <sstream>

#include "constants/font_encodings.h"
#include "core/fxcrt/stream_write_utils.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_annotfontsubset.h"
#include "core/fpdfdoc/cpdf_appearancegenerator_internal.h"
#include "core/fpdfdoc/cpdf_defaultappearance.h"
#include "core/fpdfdoc/cpdf_embed_metadata.h"
#include "core/fpdfdoc/cpvt_variabletext.h"
#include "core/fpdfdoc/cpvt_word.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxge/cfx_fontregistry.h"

using appearance_generator::DaFontResolution;
using appearance_generator::DefaultAppearanceInfo;
using appearance_generator::GenerateDirectFallbackFontDict;
using appearance_generator::GenerateFallbackFontDict;
using appearance_generator::GetFontFromDrFontDictOrGenerateFallback;
using appearance_generator::StringFromFontNameAndSize;

namespace {

ByteString GetPDFWordString(IPVT_FontMap* font_map,
                            int32_t font_index,
                            uint16_t word,
                            uint16_t sub_word) {
  if (sub_word > 0) {
    return ByteString::Format("%c", sub_word);
  }

  if (!font_map) {
    return ByteString();
  }

  RetainPtr<CPDF_Font> pdf_font = font_map->GetPDFFont(font_index);
  if (!pdf_font) {
    return ByteString();
  }

  if (pdf_font->GetBaseFontName() == "Symbol" ||
      pdf_font->GetBaseFontName() == "ZapfDingbats") {
    return ByteString::Format("%c", word);
  }

  ByteString word_string;
  // EmbedPDF: route unicode-to-charcode mapping through IPVT_FontMap so
  // CPDF_AnnotFontMap can use registered fallback fonts and subset-local glyph
  // ids when generating FreeText appearance streams.
  int32_t char_code = font_map->CharCodeFromUnicode(font_index, word);
  if (char_code >= 0) {
    pdf_font->AppendChar(&word_string, static_cast<uint32_t>(char_code));
  }
  return word_string;
}

ByteString GetWordRenderString(ByteStringView words) {
  if (words.IsEmpty()) {
    return ByteString();
  }
  return PDF_EncodeString(words) + " Tj\n";
}

ByteString GetFontSetString(IPVT_FontMap* font_map,
                            int32_t font_index,
                            float font_size) {
  if (!font_map) {
    return ByteString();
  }
  return StringFromFontNameAndSize(font_map->GetPDFFontAlias(font_index),
                                   font_size);
}

constexpr char kEmbedMetadataVerticalAlignmentKey[] = "VerticalAlignment";

int GetEmbedMetadataIntegerFor(const CPDF_Dictionary* annot_dict,
                               ByteStringView key) {
  RetainPtr<const CPDF_Dictionary> metadata =
      fpdfdoc::GetEmbedMetadata(annot_dict);
  return metadata ? metadata->GetIntegerFor(key) : 0;
}

}  // namespace

namespace appearance_generator {

ByteString StringFromFontNameAndSize(const ByteString& font_name,
                                     float font_size) {
  fxcrt::ostringstream font_stream;
  if (font_name.GetLength() > 0 && font_size > 0) {
    font_stream << "/" << font_name << " ";
    WriteFloat(font_stream, font_size) << " Tf\n";
  }
  return ByteString(font_stream);
}

// EmbedPDF: the font part of a /DA string, "/Helv 0 Tf".
// StringFromFontNameAndSize is for content streams, where a size of 0 would
// draw invisible text, so it writes nothing for it. In /DA a size of 0 means
// auto-size (ISO 32000, variable text), and the font must still be named:
// without it nothing can draw the text. Empty when there is no font name.
ByteString DefaultAppearanceFontPart(const ByteString& font_name,
                                     float font_size) {
  fxcrt::ostringstream font_stream;
  if (font_name.GetLength() > 0 && font_size >= 0) {
    font_stream << "/" << font_name << " ";
    WriteFloat(font_stream, font_size) << " Tf";
  }
  return ByteString(font_stream);
}

void SetVtFontSize(float font_size, CPVT_VariableText& vt) {
  if (FXSYS_IsFloatZero(font_size)) {
    vt.SetAutoFontSize(true);
  } else {
    vt.SetFontSize(font_size);
  }
}

std::optional<DefaultAppearanceInfo> GetDefaultAppearanceInfo(
    const CPDF_Dictionary* annot_dict,
    const CPDF_Dictionary* acroform_dict) {
  CPDF_DefaultAppearance appearance(annot_dict, acroform_dict);
  auto maybe_font_name_and_size = appearance.GetFont();
  if (!maybe_font_name_and_size.has_value()) {
    return std::nullopt;
  }

  return DefaultAppearanceInfo{
      .font_name = maybe_font_name_and_size.value().name,
      .font_size = maybe_font_name_and_size.value().size,
      .text_color = appearance.GetColor().value_or(CFX_Color())};
}

bool CloneResourcesDictIfMissingFromStream(CPDF_Dictionary* stream_dict,
                                           const CPDF_Dictionary* dr_dict) {
  RetainPtr<CPDF_Dictionary> resources_dict =
      stream_dict->GetMutableDictFor("Resources");
  if (resources_dict) {
    return false;
  }

  stream_dict->SetFor("Resources", dr_dict->Clone());
  return true;
}

bool ValidateOrCreateFontResources(CPDF_Document* doc,
                                   CPDF_Dictionary* stream_dict,
                                   const CPDF_Dictionary* font_dict,
                                   const ByteString& font_name) {
  RetainPtr<CPDF_Dictionary> resources_dict =
      stream_dict->GetMutableDictFor("Resources");
  RetainPtr<CPDF_Dictionary> font_resource_dict =
      resources_dict->GetMutableDictFor("Font");
  if (!font_resource_dict) {
    font_resource_dict = resources_dict->SetNewFor<CPDF_Dictionary>("Font");
  }

  if (!ValidateFontResourceDict(font_resource_dict.Get())) {
    return false;
  }

  if (!font_resource_dict->KeyExist(font_name.AsStringView())) {
    font_resource_dict->SetNewFor<CPDF_Reference>(font_name, doc,
                                                  font_dict->GetObjNum());
  }
  return true;
}

// `use_continuous_formatting`: When true, attempts to group consecutive words
// on the same line into a single text rendering string to reduce the generated
// AP stream size. This optimization is internally bypassed for individual RTL
// words where precise positioning is required.
ByteString GenerateEditAP(IPVT_FontMap* font_map,
                          CPVT_VariableText::Iterator* vt_iterator,
                          const CFX_PointF& offset,
                          bool use_continuous_formatting,
                          uint16_t sub_word) {
  fxcrt::ostringstream edit_stream;
  fxcrt::ostringstream line_stream;
  CFX_PointF old_point;
  CFX_PointF new_point;
  int32_t current_font_index = -1;
  CPVT_WordPlace oldplace;
  ByteString words;
  vt_iterator->SetAt(0);
  while (vt_iterator->NextWord()) {
    CPVT_WordPlace place = vt_iterator->GetWordPlace();
    CPVT_Word word;
    bool has_word = vt_iterator->GetWord(word);
    const bool is_rtl = has_word && word.is_rtl();

    if (use_continuous_formatting && !is_rtl) {
      if (place.LineCmp(oldplace) != 0) {
        if (!words.IsEmpty()) {
          line_stream << GetWordRenderString(words.AsStringView());
          edit_stream << line_stream.str();
          line_stream.str("");
          words.clear();
        }

        if (has_word) {
          new_point = word.location() + offset;
        } else {
          CPVT_Line line;
          if (vt_iterator->GetLine(line)) {
            new_point = line.ptLine + offset;
          }
        }
        if (new_point != old_point) {
          WritePoint(line_stream, new_point - old_point) << " Td\n";
          old_point = new_point;
        }
      } else if (words.IsEmpty() && has_word) {
        new_point = word.location() + offset;
        if (new_point != old_point) {
          WritePoint(line_stream, new_point - old_point) << " Td\n";
          old_point = new_point;
        }
      }

      if (has_word) {
        int32_t font_index = word.font_index();
        if (font_index != current_font_index) {
          if (!words.IsEmpty()) {
            line_stream << GetWordRenderString(words.AsStringView());
            words.clear();
          }
          line_stream << GetFontSetString(font_map, font_index,
                                          word.font_size());
          current_font_index = font_index;
        }
        words += GetPDFWordString(font_map, current_font_index, word.word(),
                                  sub_word);
      }
    } else {
      if (!words.IsEmpty()) {
        line_stream << GetWordRenderString(words.AsStringView());
        edit_stream << line_stream.str();
        line_stream.str("");
        words.clear();
      }

      if (has_word) {
        new_point = word.location() + offset;
        if (new_point != old_point) {
          WritePoint(edit_stream, new_point - old_point) << " Td\n";
          old_point = new_point;
        }
        int32_t font_index = word.font_index();
        if (font_index != current_font_index) {
          edit_stream << GetFontSetString(font_map, font_index,
                                          word.font_size());
          current_font_index = font_index;
        }
        edit_stream << GetWordRenderString(
            GetPDFWordString(font_map, current_font_index, word.word(),
                             sub_word)
                .AsStringView());
      }
    }
    oldplace = place;
  }
  if (!words.IsEmpty()) {
    line_stream << GetWordRenderString(words.AsStringView());
    edit_stream << line_stream.str();
  }
  return ByteString(edit_stream);
}

CPDF_Annot::VerticalAlignment GetVerticalAlign(
    const CPDF_Dictionary* annot_dict) {
  const int v = GetEmbedMetadataIntegerFor(annot_dict,
                                           kEmbedMetadataVerticalAlignmentKey);
  if (v < static_cast<int>(CPDF_Annot::VerticalAlignment::kTop) ||
      v > static_cast<int>(CPDF_Annot::VerticalAlignment::kBottom)) {
    return CPDF_Annot::VerticalAlignment::kTop;  // fallback
  }
  return static_cast<CPDF_Annot::VerticalAlignment>(v);
}

RetainPtr<CPDF_Dictionary> GenerateFallbackFontDict(CPDF_Document* doc) {
  auto font_dict = doc->NewIndirect<CPDF_Dictionary>();
  font_dict->SetNewFor<CPDF_Name>("Type", "Font");
  font_dict->SetNewFor<CPDF_Name>("Subtype", "Type1");
  font_dict->SetNewFor<CPDF_Name>("BaseFont", CFX_Font::kDefaultAnsiFontName);
  font_dict->SetNewFor<CPDF_Name>("Encoding",
                                  pdfium::font_encodings::kWinAnsiEncoding);
  return font_dict;
}

RetainPtr<CPDF_Dictionary> GenerateDirectFallbackFontDict() {
  auto font_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  font_dict->SetNewFor<CPDF_Name>("Type", "Font");
  font_dict->SetNewFor<CPDF_Name>("Subtype", "Type1");
  font_dict->SetNewFor<CPDF_Name>("BaseFont", CFX_Font::kDefaultAnsiFontName);
  font_dict->SetNewFor<CPDF_Name>("Encoding",
                                  pdfium::font_encodings::kWinAnsiEncoding);
  return font_dict;
}

RetainPtr<CPDF_Dictionary> GenerateEphemeralDefaultAcroFormDict() {
  auto acroform_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  acroform_dict->SetNewFor<CPDF_String>("DA", "/Helv 12 Tf 0 g");

  auto dr_dict = acroform_dict->SetNewFor<CPDF_Dictionary>("DR");
  auto font_dict = dr_dict->SetNewFor<CPDF_Dictionary>("Font");
  font_dict->SetFor("Helv", GenerateDirectFallbackFontDict());
  return acroform_dict;
}

RetainPtr<CPDF_Dictionary> GetFontFromDrFontDictOrGenerateFallback(
    CPDF_Document* doc,
    CPDF_Dictionary* dr_font_dict,
    const ByteString& font_name) {
  RetainPtr<CPDF_Dictionary> font_dict =
      dr_font_dict->GetMutableDictFor(font_name.AsStringView());
  if (font_dict) {
    return font_dict;
  }

  RetainPtr<CPDF_Dictionary> new_font_dict = GenerateFallbackFontDict(doc);
  dr_font_dict->SetNewFor<CPDF_Reference>(font_name, doc,
                                          new_font_dict->GetObjNum());
  return new_font_dict;
}

DaFontResolution ResolveDaFontForPersistentTarget(CPDF_Document* doc,
                                                  CPDF_Dictionary* dr_font_dict,
                                                  const ByteString& font_name) {
  DaFontResolution result;
  result.font_dict = dr_font_dict->GetMutableDictFor(font_name.AsStringView());
  if (result.font_dict) {
    result.registered_font_id =
        CPDF_AnnotFontSubset::ResolveRegisteredFont(result.font_dict.Get())
            .value_or(CFX_FontRegistry::kInvalidFontId);
    return result;
  }
  if (std::optional<CFX_FontRegistry::FontId> font_id =
          CPDF_AnnotFontMap::RegisteredFontIdFromAlias(doc, font_name)) {
    result.registered_font_id = *font_id;
    return result;
  }
  // EmbedPDF: the fallback is a write, so it goes into /DR/Font as the
  // document opens it for writing - a layer transaction copies the catalog's
  // chain up first. |dr_font_dict| may be a read-path view of a committed
  // version; writing into it would land outside the transaction.
  RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
  RetainPtr<CPDF_Dictionary> acroform =
      root ? root->GetMutableDictFor("AcroForm") : nullptr;
  if (!acroform) {
    return result;
  }
  RetainPtr<CPDF_Dictionary> writable_dr_font_dict =
      acroform->GetOrCreateDictFor("DR")->GetOrCreateDictFor("Font");
  result.font_dict = GetFontFromDrFontDictOrGenerateFallback(
      doc, writable_dr_font_dict.Get(), font_name);
  return result;
}

DaFontResolution ResolveDaFontForEphemeralTarget(
    const CPDF_Document* doc,
    const CPDF_Dictionary* dr_font_dict,
    const ByteString& font_name) {
  DaFontResolution result;
  RetainPtr<const CPDF_Dictionary> font_dict =
      dr_font_dict ? dr_font_dict->GetDictFor(font_name.AsStringView())
                   : nullptr;
  if (font_dict) {
    // The font loader still takes a mutable dictionary handle. Ephemeral AP
    // generation treats this as a read-only boundary and never writes
    // through it.
    result.font_dict =
        pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(font_dict.Get()));
    result.registered_font_id =
        CPDF_AnnotFontSubset::ResolveRegisteredFont(font_dict.Get())
            .value_or(CFX_FontRegistry::kInvalidFontId);
    return result;
  }
  if (std::optional<CFX_FontRegistry::FontId> font_id =
          CPDF_AnnotFontMap::RegisteredFontIdFromAlias(doc, font_name)) {
    result.registered_font_id = *font_id;
    return result;
  }
  result.font_dict = GenerateDirectFallbackFontDict();
  return result;
}

RetainPtr<CPDF_Dictionary> GenerateResourceFontDict(
    CPDF_Document* doc,
    const ByteString& font_name,
    uint32_t font_dict_obj_num) {
  auto resource_font_dict = doc->New<CPDF_Dictionary>();
  resource_font_dict->SetNewFor<CPDF_Reference>(font_name, doc,
                                                font_dict_obj_num);
  return resource_font_dict;
}

RetainPtr<CPDF_Dictionary> GenerateResourceFontDict(
    CPDF_Document* doc,
    const ByteString& font_name,
    const CPDF_Dictionary* font_dict) {
  auto resource_font_dict = doc->New<CPDF_Dictionary>();
  const uint32_t font_obj_num = font_dict->GetObjNum();
  if (font_obj_num != 0) {
    resource_font_dict->SetNewFor<CPDF_Reference>(font_name, doc, font_obj_num);
  } else {
    resource_font_dict->SetFor(font_name, font_dict->Clone());
  }
  return resource_font_dict;
}

// Returns a PDF-name-safe alias for |base_font_name|, guaranteed unique inside
// /AcroForm/DR/Font.  Re-uses any existing alias that already maps to the same
// BaseFont, otherwise creates a lean “standard-14” stub (or a fallback font
// dict) and registers it.
//
// The returned ByteString is always non-empty when the operation succeeds.
// On failure (e.g. |doc| == nullptr, empty |base_font_name|, OOM) an empty
// string is returned and the caller should abort the appearance generation.
//
// NOTE: This helper does *not* embed a font program – viewers provide the
// standard 14 fonts out of the box.  For non-standard faces we fall back to
// GenerateFallbackFontDict(), which produces a minimal Type1 replacement.
//
ByteString EnsureFontInAcroFormDR(CPDF_Document* doc,
                                  CPDF_Dictionary* acroform_dict,
                                  const ByteString& base_font_name) {
  if (!doc || !acroform_dict || base_font_name.IsEmpty()) {
    return ByteString();
  }

  // /DR /Font
  RetainPtr<CPDF_Dictionary> dr_dict = acroform_dict->GetOrCreateDictFor("DR");
  RetainPtr<CPDF_Dictionary> font_res = dr_dict->GetOrCreateDictFor("Font");

  // Is that font already present?
  {
    CPDF_DictionaryLocker locker(font_res);
    for (const auto& kv : locker) {
      const CPDF_Reference* ref =
          kv.second ? kv.second->AsReference() : nullptr;
      if (!ref) {
        continue;
      }

      RetainPtr<CPDF_Object> obj =
          doc->GetOrParseIndirectObject(ref->GetRefObjNum());
      const CPDF_Dictionary* dict = obj ? obj->AsDictionary() : nullptr;
      if (dict && dict->GetNameFor("BaseFont") == base_font_name) {
        return kv.first;
      }
    }
  }

  const ByteString base_key =
      ByteString::Format("FXF_%s", PDF_NameEncode(base_font_name).c_str());
  ByteString resource_key = base_key;
  // The first free FXF_<name>_<n>, counted per call: a function-local static
  // counter was shared by every thread and made names depend on earlier
  // documents (rule thread-confined-globals).
  for (int suffix = 1; font_res->KeyExist(resource_key.AsStringView());
       ++suffix) {
    resource_key = ByteString::Format("%s_%d", base_key.c_str(), suffix);
  }

  // Build a minimal Type1 font dictionary
  RetainPtr<CPDF_Dictionary> new_font_dict = GenerateFallbackFontDict(doc);
  new_font_dict->SetNewFor<CPDF_Name>("BaseFont", base_font_name);

  // Register and return the key
  font_res->SetNewFor<CPDF_Reference>(resource_key, doc,
                                      new_font_dict->GetObjNum());
  return resource_key;
}

}  // namespace appearance_generator
