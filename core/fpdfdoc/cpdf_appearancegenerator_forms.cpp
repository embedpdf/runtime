// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

// EmbedPDF: the appearance generator's form field widgets: text fields, combo
// and list boxes, push buttons, check boxes and radio buttons.
//
// See docs/embedpdf/fork-ledger.md, rule own-appearance-generator. EmbedPDF's
// changes are Copyright 2026 CloudPDF LTD.

#include <array>
#include <cmath>
#include <optional>
#include <sstream>
#include <utility>

#include "constants/annotation_common.h"
#include "constants/appearance.h"
#include "constants/form_fields.h"
#include "constants/form_flags.h"
#include "core/fxcrt/stream_write_utils.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_appearancegenerator_internal.h"
#include "core/fpdfdoc/cpdf_color_utils.h"
#include "core/fpdfdoc/cpdf_defaultappearance.h"
#include "core/fpdfdoc/cpdf_formfield.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fpdfdoc/cpvt_fontmap.h"
#include "core/fpdfdoc/cpvt_variabletext.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxge/cfx_fontregistry.h"
#include "core/fxge/cfx_renderdevice.h"

using appearance_generator::AnnotationDimensionsAndColor;
using appearance_generator::APGenerationTarget;
using appearance_generator::AppearanceCharacteristics;
using appearance_generator::BorderStyleInfo;
using appearance_generator::CloneResourcesDictIfMissingFromStream;
using appearance_generator::DaFontResolution;
using appearance_generator::DefaultAppearanceInfo;
using appearance_generator::GenerateAPDict;
using appearance_generator::GenerateBorderAP;
using appearance_generator::GenerateColorAP;
using appearance_generator::GenerateDirectFallbackFontDict;
using appearance_generator::GenerateEditAP;
using appearance_generator::GenerateEphemeralDefaultAcroFormDict;
using appearance_generator::GenerateExtGStateDict;
using appearance_generator::GenerateResourceFontDict;
using appearance_generator::GenerateResourcesDict;
using appearance_generator::GetAnnotationDimensionsAndColor;
using appearance_generator::GetAppearanceCharacteristics;
using appearance_generator::GetBorderStyleInfo;
using appearance_generator::GetDefaultAppearanceInfo;
using appearance_generator::PaintOperation;
using appearance_generator::ResolveDaFontForEphemeralTarget;
using appearance_generator::ResolveDaFontForPersistentTarget;
using appearance_generator::SetVtFontSize;
using appearance_generator::ValidateOrCreateFontResources;

namespace {

ByteString GenerateTextFieldAP(const CPDF_Dictionary* annot_dict,
                               const CFX_FloatRect& body_rect,
                               float font_size,
                               CPVT_VariableText& vt,
                               const WideString* value_override) {
  RetainPtr<const CPDF_Object> v_field =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, pdfium::form_fields::kV);
  WideString value = value_override
                         ? *value_override
                         : (v_field ? v_field->GetUnicodeText() : WideString());
  RetainPtr<const CPDF_Object> q_field =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, "Q");
  const int32_t align = q_field ? q_field->GetInteger() : 0;
  RetainPtr<const CPDF_Object> ff_field =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, pdfium::form_fields::kFf);
  const uint32_t flags = ff_field ? ff_field->GetInteger() : 0;
  RetainPtr<const CPDF_Object> max_len_field =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, "MaxLen");
  const uint32_t max_len = max_len_field ? max_len_field->GetInteger() : 0;
  vt.SetPlateRect(body_rect);
  vt.SetAlignment(CPVT_VariableText::ToAlignment(align));
  SetVtFontSize(font_size, vt);

  bool is_multi_line = (flags >> 12) & 1;
  if (is_multi_line) {
    vt.SetMultiLine(true);
    vt.SetAutoReturn(true);
  }
  uint16_t sub_word = 0;
  if ((flags >> 13) & 1) {
    sub_word = '*';
    vt.SetPasswordChar(sub_word);
  }
  bool is_char_array = (flags >> 24) & 1;
  if (is_char_array) {
    vt.SetCharArray(max_len);
  } else {
    vt.SetLimitChar(max_len);
  }

  vt.Initialize();
  vt.SetText(value);
  vt.RearrangeAll();
  CFX_PointF offset;
  if (!is_multi_line) {
    offset = CFX_PointF(
        0.0f, (vt.GetContentRect().Height() - body_rect.Height()) / 2.0f);
  }
  return GenerateEditAP(vt.GetProvider()->GetFontMap(), vt.GetIterator(),
                        offset, !is_char_array, sub_word);
}

ByteString GenerateComboBoxAP(const CPDF_Dictionary* annot_dict,
                              const CFX_FloatRect& body_rect,
                              const CFX_Color& text_color,
                              float font_size,
                              CPVT_VariableText::Provider& provider,
                              const WideString* value_override) {
  fxcrt::ostringstream body_stream;

  RetainPtr<const CPDF_Object> v_field =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, pdfium::form_fields::kV);
  WideString value = value_override
                         ? *value_override
                         : (v_field ? v_field->GetUnicodeText() : WideString());
  CPVT_VariableText vt(&provider);
  CFX_FloatRect button_rect = body_rect;
  button_rect.left = button_rect.right - 13;
  button_rect.Normalize();
  CFX_FloatRect edit_rect = body_rect;
  edit_rect.right = button_rect.left;
  edit_rect.Normalize();
  edit_rect.Deflate(4.0f, 0);
  vt.SetPlateRect(edit_rect);
  SetVtFontSize(font_size, vt);

  vt.Initialize();
  vt.SetText(value);
  vt.RearrangeAll();
  CFX_FloatRect content_rect = vt.GetContentRect();
  CFX_PointF offset =
      CFX_PointF(0.0f, (content_rect.Height() - edit_rect.Height()) / 2.0f);
  ByteString edit =
      GenerateEditAP(provider.GetFontMap(), vt.GetIterator(), offset, true, 0);
  if (edit.GetLength() > 0) {
    body_stream << "/Tx BMC\nq\n";
    WriteRect(body_stream, edit_rect) << " re\nW\nn\n";
    body_stream << "BT\n"
                << GenerateColorAP(text_color, PaintOperation::kFill) << edit
                << "ET\n"
                << "Q\nEMC\n";
  }
  if (!button_rect.IsEmpty()) {
    CFX_PointF center((button_rect.left + button_rect.right) / 2,
                      (button_rect.top + button_rect.bottom) / 2);
    if (FXSYS_IsFloatBigger(button_rect.Width(), 6) &&
        FXSYS_IsFloatBigger(button_rect.Height(), 6)) {
      body_stream << "q\n0 g\n";
      WritePoint(body_stream, {center.x - 3, center.y + 1.5f}) << " m\n";
      WritePoint(body_stream, {center.x + 3, center.y + 1.5f}) << " l\n";
      WritePoint(body_stream, {center.x, center.y - 1.5f}) << " l\n";
      WritePoint(body_stream, {center.x - 3, center.y + 1.5f}) << " l f\n";
      body_stream << "Q\n";
    }
  }
  return ByteString(body_stream);
}

ByteString GenerateListBoxAP(const CPDF_Dictionary* annot_dict,
                             const CFX_FloatRect& body_rect,
                             const CFX_Color& text_color,
                             float font_size,
                             CPVT_VariableText::Provider& provider) {
  RetainPtr<const CPDF_Array> opts =
      ToArray(CPDF_FormField::GetFieldAttrForDict(annot_dict, "Opt"));
  if (!opts) {
    return ByteString();
  }

  RetainPtr<const CPDF_Array> selections =
      ToArray(CPDF_FormField::GetFieldAttrForDict(annot_dict, "I"));
  RetainPtr<const CPDF_Object> top_index =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, "TI");
  const int32_t top = top_index ? top_index->GetInteger() : 0;
  fxcrt::ostringstream body_stream;

  float fy = body_rect.top;
  for (size_t i = top, sz = opts->size(); i < sz; i++) {
    if (FXSYS_IsFloatSmaller(fy, body_rect.bottom)) {
      break;
    }

    if (RetainPtr<const CPDF_Object> opt = opts->GetDirectObjectAt(i)) {
      WideString item;
      if (opt->IsString()) {
        item = opt->GetUnicodeText();
      } else if (const CPDF_Array* opt_array = opt->AsArray()) {
        RetainPtr<const CPDF_Object> opt_item = opt_array->GetDirectObjectAt(1);
        if (opt_item) {
          item = opt_item->GetUnicodeText();
        }
      }
      bool is_selected = false;
      if (selections) {
        for (size_t s = 0, ssz = selections->size(); s < ssz; s++) {
          int value = selections->GetIntegerAt(s);
          if (value >= 0 && i == static_cast<size_t>(value)) {
            is_selected = true;
            break;
          }
        }
      }
      CPVT_VariableText vt(&provider);
      vt.SetPlateRect(
          CFX_FloatRect(body_rect.left, 0.0f, body_rect.right, 0.0f));
      vt.SetFontSize(FXSYS_IsFloatZero(font_size) ? 12.0f : font_size);
      vt.Initialize();
      vt.SetText(item);
      vt.RearrangeAll();

      const float item_height = vt.GetContentRect().Height();
      if (is_selected) {
        CFX_FloatRect item_rect = CFX_FloatRect(
            body_rect.left, fy - item_height, body_rect.right, fy);
        body_stream << "q\n"
                    << GenerateColorAP(CFX_Color::MakeRGB(0, 51.0f / 255.0f,
                                                          113.0f / 255.0f),
                                       PaintOperation::kFill);
        WriteRect(body_stream, item_rect) << " re f\nQ\n";
        body_stream << "BT\n"
                    << GenerateColorAP(CFX_Color::MakeGray(1),
                                       PaintOperation::kFill)
                    << GenerateEditAP(provider.GetFontMap(), vt.GetIterator(),
                                      CFX_PointF(0.0f, fy), true, 0)
                    << "ET\n";
      } else {
        body_stream << "BT\n"
                    << GenerateColorAP(text_color, PaintOperation::kFill)
                    << GenerateEditAP(provider.GetFontMap(), vt.GetIterator(),
                                      CFX_PointF(0.0f, fy), true, 0)
                    << "ET\n";
      }
      fy -= item_height;
    }
  }
  return ByteString(body_stream);
}

void GenerateTextFieldFormAP(fxcrt::ostringstream& app_stream,
                             const CPDF_Dictionary* annot_dict,
                             const CFX_FloatRect& bbox,
                             const DefaultAppearanceInfo& da_info,
                             CPVT_VariableText::Provider& provider,
                             const WideString* value_override) {
  const AppearanceCharacteristics mk =
      GetAppearanceCharacteristics(annot_dict->GetDictFor("MK"));
  const bool has_bg =
      mk.background_color.nColorType != CFX_Color::Type::kTransparent;
  const bool has_bc =
      mk.border_color.nColorType != CFX_Color::Type::kTransparent;

  const BorderStyleInfo bs = GetBorderStyleInfo(annot_dict->GetDictFor("BS"));
  const float bw = bs.width;
  const float half_bw = bw / 2.0f;

  CFX_FloatRect stroke_rect = bbox;
  if (bw > 0) {
    stroke_rect.Deflate(half_bw, half_bw);
  }

  CFX_FloatRect body_rect = bbox;
  body_rect.Deflate(2.0f * bw, bw);

  // Background + border in isolated graphics state. A solid border is a
  // stroked rectangle; the other styles draw as a combo box's and a list
  // box's do.
  app_stream << "q\n";
  if (has_bg) {
    app_stream << GenerateColorAP(mk.background_color, PaintOperation::kFill);
    WriteRect(app_stream, bbox) << " re f*\n";
  }
  if (bs.style == BorderStyle::kSolid) {
    if (has_bc && bw > 0) {
      app_stream << GenerateColorAP(mk.border_color, PaintOperation::kStroke);
      WriteFloat(app_stream, bw) << " w\n";
      WriteRect(app_stream, stroke_rect) << " re s\n";
    }
  } else {
    app_stream << GenerateBorderAP(bbox, bs, mk.border_color);
  }
  app_stream << "Q\n";

  // Text content in isolated graphics state with proper clipping
  app_stream << "/Tx BMC\nq\n";

  CFX_FloatRect clip_rect = bbox;
  clip_rect.Deflate(bw, bw);
  WriteRect(app_stream, clip_rect) << " re W n\n";

  CPVT_VariableText vt(&provider);
  ByteString body = GenerateTextFieldAP(annot_dict, body_rect,
                                        da_info.font_size, vt, value_override);

  app_stream << "BT\n";
  app_stream << GenerateColorAP(da_info.text_color, PaintOperation::kStroke);
  app_stream << GenerateColorAP(da_info.text_color, PaintOperation::kFill);

  if (body.GetLength() > 0) {
    app_stream << body;
  }

  app_stream << "0 G 0 g\n";
  app_stream << "ET\n";
  app_stream << "Q\nEMC\n";
}

void GenerateComboBoxFormAP(fxcrt::ostringstream& app_stream,
                            const CPDF_Dictionary* annot_dict,
                            const CFX_FloatRect& bbox,
                            const DefaultAppearanceInfo& da_info,
                            CPVT_VariableText::Provider& provider,
                            const WideString* value_override) {
  const AnnotationDimensionsAndColor dims =
      GetAnnotationDimensionsAndColor(annot_dict);
  const BorderStyleInfo border_info =
      GetBorderStyleInfo(annot_dict->GetDictFor("BS"));

  const ByteString background =
      GenerateColorAP(dims.background_color, PaintOperation::kFill);
  if (background.GetLength() > 0) {
    app_stream << "q\n" << background;
    WriteRect(app_stream, bbox) << " re f\nQ\n";
  }

  const ByteString border_stream =
      GenerateBorderAP(bbox, border_info, dims.border_color);
  if (border_stream.GetLength() > 0) {
    app_stream << "q\n" << border_stream << "Q\n";
  }

  CFX_FloatRect body_rect = bbox;
  body_rect.Deflate(border_info.width, border_info.width);

  app_stream << GenerateComboBoxAP(annot_dict, body_rect, da_info.text_color,
                                   da_info.font_size, provider, value_override);
}

void GenerateListBoxFormAP(fxcrt::ostringstream& app_stream,
                           const CPDF_Dictionary* annot_dict,
                           const CFX_FloatRect& bbox,
                           const DefaultAppearanceInfo& da_info,
                           CPVT_VariableText::Provider& provider) {
  const AnnotationDimensionsAndColor dims =
      GetAnnotationDimensionsAndColor(annot_dict);
  const BorderStyleInfo border_info =
      GetBorderStyleInfo(annot_dict->GetDictFor("BS"));

  const ByteString background =
      GenerateColorAP(dims.background_color, PaintOperation::kFill);
  if (background.GetLength() > 0) {
    app_stream << "q\n" << background;
    WriteRect(app_stream, bbox) << " re f\nQ\n";
  }

  const ByteString border_stream =
      GenerateBorderAP(bbox, border_info, dims.border_color);
  if (border_stream.GetLength() > 0) {
    app_stream << "q\n" << border_stream << "Q\n";
  }

  CFX_FloatRect body_rect = bbox;
  body_rect.Deflate(border_info.width, border_info.width);

  const ByteString body = GenerateListBoxAP(
      annot_dict, body_rect, da_info.text_color, da_info.font_size, provider);
  if (body.GetLength() > 0) {
    app_stream << "/Tx BMC\nq\n";
    WriteRect(app_stream, body_rect) << " re\nW\nn\n" << body << "Q\nEMC\n";
  }
}

void GenerateCheckmarkPath(fxcrt::ostringstream& stream,
                           const CFX_FloatRect& bbox) {
  const float w = bbox.Width();
  const float h = bbox.Height();

  using PointRow = std::array<CFX_PointF, 3>;
  std::array<PointRow, 8> pts = {{
      {{CFX_PointF(0.28f, 0.52f), CFX_PointF(0.27f, 0.48f),
        CFX_PointF(0.29f, 0.40f)}},
      {{CFX_PointF(0.30f, 0.33f), CFX_PointF(0.31f, 0.29f),
        CFX_PointF(0.31f, 0.28f)}},
      {{CFX_PointF(0.39f, 0.28f), CFX_PointF(0.49f, 0.29f),
        CFX_PointF(0.77f, 0.67f)}},
      {{CFX_PointF(0.76f, 0.68f), CFX_PointF(0.78f, 0.69f),
        CFX_PointF(0.76f, 0.75f)}},
      {{CFX_PointF(0.76f, 0.75f), CFX_PointF(0.73f, 0.80f),
        CFX_PointF(0.68f, 0.75f)}},
      {{CFX_PointF(0.68f, 0.74f), CFX_PointF(0.68f, 0.74f),
        CFX_PointF(0.44f, 0.47f)}},
      {{CFX_PointF(0.43f, 0.47f), CFX_PointF(0.40f, 0.47f),
        CFX_PointF(0.41f, 0.58f)}},
      {{CFX_PointF(0.40f, 0.60f), CFX_PointF(0.28f, 0.66f),
        CFX_PointF(0.30f, 0.56f)}},
  }};

  for (auto& row : pts) {
    for (auto& pt : row) {
      pt.x = pt.x * w + bbox.left;
      pt.y = pt.y * h + bbox.bottom;
    }
  }

  WritePoint(stream, pts[0][0]) << " m\n";
  for (size_t i = 0; i < pts.size(); ++i) {
    const size_t next = (i + 1) % pts.size();
    const CFX_PointF& pt_next = pts[next][0];
    const float px1 = pts[i][1].x - pts[i][0].x;
    const float py1 = pts[i][1].y - pts[i][0].y;
    const float px2 = pts[i][2].x - pt_next.x;
    const float py2 = pts[i][2].y - pt_next.y;
    WritePoint(stream, {pts[i][0].x + px1 * FXSYS_BEZIER,
                        pts[i][0].y + py1 * FXSYS_BEZIER})
        << " ";
    WritePoint(stream,
               {pt_next.x + px2 * FXSYS_BEZIER, pt_next.y + py2 * FXSYS_BEZIER})
        << " ";
    WritePoint(stream, pt_next) << " c\n";
  }
}

void BuildCheckboxBoxStream(fxcrt::ostringstream& stream,
                            const AppearanceCharacteristics& mk,
                            const BorderStyleInfo& bs,
                            const CFX_FloatRect& bbox) {
  const bool has_bg =
      mk.background_color.nColorType != CFX_Color::Type::kTransparent;
  const bool has_bc =
      mk.border_color.nColorType != CFX_Color::Type::kTransparent;
  const float bw = bs.width;
  const float half_bw = bw / 2.0f;

  CFX_FloatRect stroke_rect = bbox;
  if (bw > 0) {
    stroke_rect.Deflate(half_bw, half_bw);
  }

  if (has_bg) {
    stream << GenerateColorAP(mk.background_color, PaintOperation::kFill);
    WriteRect(stream, stroke_rect) << " re f*\n";
  }

  if (has_bc && bw > 0) {
    stream << GenerateColorAP(mk.border_color, PaintOperation::kStroke);
    WriteFloat(stream, bw) << " w\n";
    WriteRect(stream, stroke_rect) << " re S\n";
  }
}

// A push button: background and border as a text field draws them, and the
// caption (/MK /CA) on one line, centred both ways, in the /DA font and
// colour. An icon (/MK /I) is not drawn, so the caption takes the whole
// button whatever /MK /TP says.
void GeneratePushButtonFormAP(fxcrt::ostringstream& app_stream,
                              const CPDF_Dictionary* annot_dict,
                              const CFX_FloatRect& bbox,
                              const DefaultAppearanceInfo& da_info,
                              CPVT_VariableText::Provider& provider) {
  const AppearanceCharacteristics mk =
      GetAppearanceCharacteristics(annot_dict->GetDictFor("MK"));
  const BorderStyleInfo bs = GetBorderStyleInfo(annot_dict->GetDictFor("BS"));
  const float bw = bs.width;

  app_stream << "q\n";
  BuildCheckboxBoxStream(app_stream, mk, bs, bbox);
  app_stream << "Q\n";
  if (mk.caption.IsEmpty()) {
    return;
  }

  CFX_FloatRect body_rect = bbox;
  body_rect.Deflate(2.0f * bw, bw);
  CPVT_VariableText vt(&provider);
  vt.SetPlateRect(body_rect);
  vt.SetAlignment(CPVT_VariableText::Alignment::kCenter);
  SetVtFontSize(da_info.font_size, vt);
  vt.Initialize();
  vt.SetText(mk.caption);
  vt.RearrangeAll();
  const CFX_PointF offset(
      0.0f, (vt.GetContentRect().Height() - body_rect.Height()) / 2.0f);
  const ByteString caption =
      GenerateEditAP(provider.GetFontMap(), vt.GetIterator(), offset, true, 0);
  if (caption.IsEmpty()) {
    return;
  }

  app_stream << "q\n";
  CFX_FloatRect clip_rect = bbox;
  clip_rect.Deflate(bw, bw);
  WriteRect(app_stream, clip_rect) << " re W n\n";
  app_stream << "BT\n"
             << GenerateColorAP(da_info.text_color, PaintOperation::kFill)
             << caption << "ET\nQ\n";
}

// The symbol a check box or radio button shows when on, named by the
// ZapfDingbats character in its /MK /CA (the characters Acrobat writes).
enum class CheckStyle { kCheck, kCircle, kCross, kDiamond, kSquare, kStar };

std::optional<CheckStyle> CheckStyleFromCaption(const WideString& caption) {
  if (caption.IsEmpty()) {
    return std::nullopt;
  }
  switch (caption[0]) {
    case L'4':
      return CheckStyle::kCheck;
    case L'8':
      return CheckStyle::kCross;
    case L'H':
      return CheckStyle::kStar;
    case L'l':
      return CheckStyle::kCircle;
    case L'n':
      return CheckStyle::kSquare;
    case L'u':
      return CheckStyle::kDiamond;
    default:
      return std::nullopt;
  }
}

// The colour the symbol is painted in: the widget's /DA colour, inherited
// from its field or the form, and black when none names one.
CFX_Color CheckSymbolColor(CPDF_Document* doc,
                           const CPDF_Dictionary* annot_dict) {
  const CPDF_Dictionary* root = doc->GetRoot();
  RetainPtr<const CPDF_Dictionary> acroform =
      root ? root->GetDictFor("AcroForm") : nullptr;
  std::optional<CFX_Color> color =
      CPDF_DefaultAppearance(annot_dict, acroform.Get()).GetColor();
  if (!color.has_value() ||
      color->nColorType == CFX_Color::Type::kTransparent) {
    return CFX_Color::MakeRGB(0, 0, 0);
  }
  return color.value();
}

// Where the symbol is drawn: the middle square of `body` (the widget inside
// its border), shrunk for the filled shapes as PDFium's form filler shrinks
// them (CPDFSDK_AppStream), so a symbol looks the same whichever drew it.
CFX_FloatRect CheckSymbolRect(const CFX_FloatRect& body,
                              CheckStyle style,
                              bool radio) {
  CFX_FloatRect rect = body.GetCenterSquare();
  switch (style) {
    case CheckStyle::kCheck:
    case CheckStyle::kCross:
      break;
    case CheckStyle::kCircle:
      rect.ScaleFromCenterPoint(radio ? 1.0f / 2.0f : 2.0f / 3.0f);
      break;
    case CheckStyle::kDiamond:
    case CheckStyle::kSquare:
    case CheckStyle::kStar:
      rect.ScaleFromCenterPoint(2.0f / 3.0f);
      break;
  }
  return rect;
}

void WriteClosedPolygon(fxcrt::ostringstream& stream,
                        std::initializer_list<CFX_PointF> points) {
  bool first = true;
  for (const CFX_PointF& point : points) {
    WritePoint(stream, point) << (first ? " m\n" : " l\n");
    first = false;
  }
  stream << "h\n";
}

// The symbol of `style` filling `rect`, painted in `color`: a cross is
// stroked, every other symbol filled. The shapes are PDFium's form filler's.
void WriteCheckSymbol(fxcrt::ostringstream& stream,
                      CheckStyle style,
                      const CFX_FloatRect& rect,
                      const CFX_Color& color) {
  const bool stroked = style == CheckStyle::kCross;
  stream << "q\n"
         << GenerateColorAP(color, stroked ? PaintOperation::kStroke
                                           : PaintOperation::kFill);
  const float mid_x = (rect.left + rect.right) / 2.0f;
  const float mid_y = (rect.bottom + rect.top) / 2.0f;
  switch (style) {
    case CheckStyle::kCheck:
      GenerateCheckmarkPath(stream, rect);
      break;
    case CheckStyle::kCircle: {
      // Four quarter arcs, from the left middle clockwise.
      const float rx = rect.Width() / 2.0f;
      const float ry = rect.Height() / 2.0f;
      const CFX_PointF left(rect.left, mid_y);
      const CFX_PointF top(mid_x, rect.top);
      const CFX_PointF right(rect.right, mid_y);
      const CFX_PointF bottom(mid_x, rect.bottom);
      WritePoint(stream, left) << " m\n";
      WritePoint(stream, {left.x, left.y + ry * FXSYS_BEZIER}) << " ";
      WritePoint(stream, {top.x - rx * FXSYS_BEZIER, top.y}) << " ";
      WritePoint(stream, top) << " c\n";
      WritePoint(stream, {top.x + rx * FXSYS_BEZIER, top.y}) << " ";
      WritePoint(stream, {right.x, right.y + ry * FXSYS_BEZIER}) << " ";
      WritePoint(stream, right) << " c\n";
      WritePoint(stream, {right.x, right.y - ry * FXSYS_BEZIER}) << " ";
      WritePoint(stream, {bottom.x + rx * FXSYS_BEZIER, bottom.y}) << " ";
      WritePoint(stream, bottom) << " c\n";
      WritePoint(stream, {bottom.x - rx * FXSYS_BEZIER, bottom.y}) << " ";
      WritePoint(stream, {left.x, left.y - ry * FXSYS_BEZIER}) << " ";
      WritePoint(stream, left) << " c\n";
      break;
    }
    case CheckStyle::kCross:
      WritePoint(stream, {rect.left, rect.top}) << " m\n";
      WritePoint(stream, {rect.right, rect.bottom}) << " l\n";
      WritePoint(stream, {rect.left, rect.bottom}) << " m\n";
      WritePoint(stream, {rect.right, rect.top}) << " l\n";
      break;
    case CheckStyle::kDiamond:
      WriteClosedPolygon(stream, {{rect.left, mid_y},
                                  {mid_x, rect.top},
                                  {rect.right, mid_y},
                                  {mid_x, rect.bottom}});
      break;
    case CheckStyle::kSquare:
      WriteClosedPolygon(stream, {{rect.left, rect.top},
                                  {rect.right, rect.top},
                                  {rect.right, rect.bottom},
                                  {rect.left, rect.bottom}});
      break;
    case CheckStyle::kStar: {
      // A five-pointed star, its points joined every second one, filled by
      // the nonzero rule so the middle is solid.
      const float radius =
          (rect.top - rect.bottom) / (1 + cosf(FXSYS_PI / 5.0f));
      std::array<CFX_PointF, 5> tips;
      float angle = FXSYS_PI / 10.0f;
      for (CFX_PointF& tip : tips) {
        tip = {mid_x + radius * cosf(angle), mid_y + radius * sinf(angle)};
        angle += FXSYS_PI * 2 / 5.0f;
      }
      WriteClosedPolygon(stream, {tips[0], tips[2], tips[4], tips[1], tips[3]});
      break;
    }
  }
  stream << (stroked ? "S\n" : "f\n") << "Q\n";
}

uint32_t CreateFormXObjectStream(CPDF_Document* doc,
                                 fxcrt::ostringstream& content,
                                 const CFX_FloatRect& bbox,
                                 const CFX_Matrix& matrix) {
  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  stream_dict->SetNewFor<CPDF_Number>("FormType", 1);
  stream_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  stream_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  stream_dict->SetMatrixFor("Matrix", matrix);
  stream_dict->SetRectFor("BBox", bbox);

  auto stream = doc->NewIndirect<CPDF_Stream>(std::move(stream_dict));
  stream->SetDataFromStringstreamAndRemoveFilter(&content);
  return stream->GetObjNum();
}

bool GenerateFormAPToTarget(APGenerationTarget* target,
                            CPDF_Dictionary* annot_dict,
                            CPDF_AppearanceGenerator::FormType type,
                            const WideString* value_override) {
  CPDF_Document* const doc = target->doc;
  const CPDF_Dictionary* root_dict = doc->GetRoot();
  if (!root_dict) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> form_dict =
      root_dict->GetDictFor("AcroForm");
  RetainPtr<CPDF_Dictionary> ephemeral_form_dict;
  if (!form_dict) {
    if (target->IsPersistent()) {
      form_dict = CPDF_InteractiveForm::InitAcroFormDict(doc);
      CHECK(form_dict);
    } else {
      ephemeral_form_dict = GenerateEphemeralDefaultAcroFormDict();
      form_dict = ephemeral_form_dict;
    }
  }

  std::optional<DefaultAppearanceInfo> default_appearance_info =
      GetDefaultAppearanceInfo(annot_dict, form_dict.Get());
  if (!default_appearance_info.has_value()) {
    return false;
  }

  // A missing or font-less /DR must not veto appearance generation — the
  // widget's own /DA names the font it wants, and DR-less AcroForms are
  // common in flattened government forms (the IRS f1040 class). Persistent
  // targets seed /DR/Font with a fallback the same way redaction overlays
  // do; ephemeral targets fall back without mutating the document.
  RetainPtr<const CPDF_Dictionary> dr_dict = form_dict->GetDictFor("DR");
  RetainPtr<const CPDF_Dictionary> dr_font_dict =
      dr_dict ? dr_dict->GetDictFor("Font") : nullptr;
  if (!ValidateFontResourceDict(dr_font_dict.Get())) {
    dr_font_dict.Reset();
  }

  const ByteString& font_name = default_appearance_info.value().font_name;
  RetainPtr<CPDF_Dictionary> font_dict;
  DaFontResolution da_font;
  if (target->IsPersistent()) {
    RetainPtr<CPDF_Dictionary> mutable_dr_font_dict;
    if (dr_font_dict) {
      mutable_dr_font_dict =
          pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(dr_font_dict.Get()));
    } else {
      RetainPtr<CPDF_Dictionary> mutable_root = doc->GetMutableRoot();
      RetainPtr<CPDF_Dictionary> mutable_form_dict =
          mutable_root ? mutable_root->GetMutableDictFor("AcroForm") : nullptr;
      if (!mutable_form_dict) {
        return false;
      }
      mutable_dr_font_dict =
          mutable_form_dict->GetOrCreateDictFor("DR")->GetOrCreateDictFor(
              "Font");
      dr_dict = mutable_form_dict->GetDictFor("DR");
    }
    da_font = ResolveDaFontForPersistentTarget(doc, mutable_dr_font_dict.Get(),
                                               font_name);
  } else {
    da_font =
        ResolveDaFontForEphemeralTarget(doc, dr_font_dict.Get(), font_name);
  }
  font_dict = da_font.font_dict;
  const bool use_registered_font_map =
      target->IsPersistent() &&
      (CFX_FontRegistry::HasFallbackFonts() ||
       da_font.registered_font_id != CFX_FontRegistry::kInvalidFontId);
  if (!font_dict && !use_registered_font_map) {
    // Ephemeral render of a widget whose /DA names a registered font that has
    // no /DR entry yet: draw with the fallback rather than nothing.
    font_dict = GenerateDirectFallbackFontDict();
  }
  auto* doc_page_data = CPDF_DocPageData::FromDocument(doc);
  RetainPtr<CPDF_Font> default_font =
      font_dict ? doc_page_data->GetFont(font_dict) : nullptr;
  if (!default_font && !use_registered_font_map) {
    return false;
  }

  const AnnotationDimensionsAndColor dims =
      GetAnnotationDimensionsAndColor(annot_dict);

  RetainPtr<CPDF_Dictionary> resources_dict;
  RetainPtr<CPDF_Stream> normal_stream;
  if (target->IsPersistent()) {
    RetainPtr<CPDF_Dictionary> ap_dict =
        annot_dict->GetOrCreateDictFor(pdfium::annotation::kAP);
    normal_stream = ap_dict->GetMutableStreamFor("N");
    if (normal_stream) {
      RetainPtr<CPDF_Dictionary> stream_dict = normal_stream->GetMutableDict();
      const bool cloned =
          CloneResourcesDictIfMissingFromStream(stream_dict, dr_dict.Get());
      if (!cloned) {
        if (!ValidateOrCreateFontResources(doc, stream_dict, font_dict,
                                           font_name)) {
          return false;
        }
      }
      resources_dict = stream_dict->GetMutableDictFor("Resources");
    } else {
      normal_stream =
          doc->NewIndirect<CPDF_Stream>(pdfium::MakeRetain<CPDF_Dictionary>());
      ap_dict->SetNewFor<CPDF_Reference>("N", doc, normal_stream->GetObjNum());
    }
  } else {
    auto gs_dict = GenerateExtGStateDict(*annot_dict, "Normal");
    auto resource_font_dict =
        GenerateResourceFontDict(doc, font_name, font_dict.Get());
    resources_dict = GenerateResourcesDict(doc, std::move(gs_dict),
                                           std::move(resource_font_dict));
  }

  auto generate_form_stream = [&](CPVT_VariableText::Provider& provider,
                                  fxcrt::ostringstream& app_stream) {
    switch (type) {
      case CPDF_AppearanceGenerator::kTextField:
        GenerateTextFieldFormAP(app_stream, annot_dict, dims.bbox,
                                default_appearance_info.value(), provider,
                                value_override);
        break;
      case CPDF_AppearanceGenerator::kComboBox:
        GenerateComboBoxFormAP(app_stream, annot_dict, dims.bbox,
                               default_appearance_info.value(), provider,
                               value_override);
        break;
      case CPDF_AppearanceGenerator::kListBox:
        GenerateListBoxFormAP(app_stream, annot_dict, dims.bbox,
                              default_appearance_info.value(), provider);
        break;
      case CPDF_AppearanceGenerator::kPushButton:
        GeneratePushButtonFormAP(app_stream, annot_dict, dims.bbox,
                                 default_appearance_info.value(), provider);
        break;
    }
  };

  if (use_registered_font_map) {
    // EmbedPDF: form widgets need the same registered fallback/subset path as
    // FreeText when their value/options contain glyphs outside the DA font.
    // Keep the old CPVT_FontMap path unless a registered font is actually
    // involved so existing form AP output remains stable by default.
    CPDF_AnnotFontMap map(
        doc, std::move(default_font), font_name,
        /*allow_registered_fallbacks=*/true, da_font.registered_font_id,
        /*install_dr_entry=*/true, CPDF_AnnotFontMap::Owner::kWidget);
    if (!map.HasDefaultFont()) {
      return false;
    }
    CPVT_VariableText::Provider provider(&map);

    fxcrt::ostringstream app_stream;
    generate_form_stream(provider, app_stream);

    normal_stream->SetDataFromStringstreamAndRemoveFilter(&app_stream);
    RetainPtr<CPDF_Dictionary> stream_dict = normal_stream->GetMutableDict();
    stream_dict->SetMatrixFor("Matrix", dims.matrix);
    stream_dict->SetRectFor("BBox", dims.bbox);
    RetainPtr<CPDF_Dictionary> stream_resources =
        stream_dict->GetOrCreateDictFor("Resources");
    RetainPtr<CPDF_Dictionary> resource_font_dict =
        map.CreateFontResourceDict();
    if (!resource_font_dict) {
      return false;  // registered /DA font without a resource: no appearance
    }
    stream_resources->SetFor("Font", std::move(resource_font_dict));
    return true;
  }

  RetainPtr<CPDF_Dictionary> ephemeral_resources_dict = resources_dict;
  CPVT_FontMap map(doc, std::move(resources_dict), std::move(default_font),
                   font_name);
  CPVT_VariableText::Provider provider(&map);

  fxcrt::ostringstream app_stream;
  generate_form_stream(provider, app_stream);

  if (!target->IsPersistent()) {
    return GenerateAPDict(
        target, annot_dict, &app_stream, std::move(ephemeral_resources_dict),
        /*is_text_markup_annotation=*/false, dims.matrix, dims.bbox);
  }

  normal_stream->SetDataFromStringstreamAndRemoveFilter(&app_stream);
  RetainPtr<CPDF_Dictionary> stream_dict = normal_stream->GetMutableDict();
  stream_dict->SetMatrixFor("Matrix", dims.matrix);
  stream_dict->SetRectFor("BBox", dims.bbox);

  const bool cloned =
      CloneResourcesDictIfMissingFromStream(stream_dict, dr_dict);
  if (cloned) {
    return true;
  }

  ValidateOrCreateFontResources(doc, stream_dict, font_dict, font_name);
  return true;
}

void BuildRadioCircleStream(fxcrt::ostringstream& stream,
                            const AppearanceCharacteristics& mk,
                            const BorderStyleInfo& bs,
                            const CFX_FloatRect& bbox) {
  const bool has_bg =
      mk.background_color.nColorType != CFX_Color::Type::kTransparent;
  const bool has_bc =
      mk.border_color.nColorType != CFX_Color::Type::kTransparent;
  const float bw = bs.width;
  const float half_bw = bw / 2.0f;

  const float cx = (bbox.left + bbox.right) / 2.0f;
  const float cy = (bbox.bottom + bbox.top) / 2.0f;
  const float rx = (bbox.right - bbox.left) / 2.0f;
  const float ry = (bbox.top - bbox.bottom) / 2.0f;

  constexpr float kKappa = 0.5523f;

  auto WriteEllipse = [&](fxcrt::ostringstream& s, float erx, float ery) {
    const float dx = kKappa * erx;
    const float dy = kKappa * ery;
    WriteFloat(s, cx) << " ";
    WriteFloat(s, cy + ery) << " m\n";
    WriteFloat(s, cx + dx) << " ";
    WriteFloat(s, cy + ery) << " ";
    WriteFloat(s, cx + erx) << " ";
    WriteFloat(s, cy + dy) << " ";
    WriteFloat(s, cx + erx) << " ";
    WriteFloat(s, cy) << " c\n";
    WriteFloat(s, cx + erx) << " ";
    WriteFloat(s, cy - dy) << " ";
    WriteFloat(s, cx + dx) << " ";
    WriteFloat(s, cy - ery) << " ";
    WriteFloat(s, cx) << " ";
    WriteFloat(s, cy - ery) << " c\n";
    WriteFloat(s, cx - dx) << " ";
    WriteFloat(s, cy - ery) << " ";
    WriteFloat(s, cx - erx) << " ";
    WriteFloat(s, cy - dy) << " ";
    WriteFloat(s, cx - erx) << " ";
    WriteFloat(s, cy) << " c\n";
    WriteFloat(s, cx - erx) << " ";
    WriteFloat(s, cy + dy) << " ";
    WriteFloat(s, cx - dx) << " ";
    WriteFloat(s, cy + ery) << " ";
    WriteFloat(s, cx) << " ";
    WriteFloat(s, cy + ery) << " c\n";
    s << "h\n";
  };

  if (has_bg) {
    stream << GenerateColorAP(mk.background_color, PaintOperation::kFill);
    WriteEllipse(stream, rx - half_bw, ry - half_bw);
    stream << "f*\n";
  }

  if (has_bc && bw > 0) {
    stream << GenerateColorAP(mk.border_color, PaintOperation::kStroke);
    WriteFloat(stream, bw) << " w\n";
    WriteEllipse(stream, rx - half_bw, ry - half_bw);
    stream << "S\n";
  }
}

}  // namespace

namespace appearance_generator {

AppearanceCharacteristics GetAppearanceCharacteristics(
    const CPDF_Dictionary* mk_dict) {
  AppearanceCharacteristics appearance_characteristics;
  if (!mk_dict) {
    return appearance_characteristics;
  }

  appearance_characteristics.rotation =
      mk_dict->GetIntegerFor(pdfium::appearance::kR);

  RetainPtr<const CPDF_Array> border_color_array =
      mk_dict->GetArrayFor(pdfium::appearance::kBC);
  if (border_color_array) {
    appearance_characteristics.border_color =
        fpdfdoc::CFXColorFromArray(*border_color_array);
  }
  RetainPtr<const CPDF_Array> background_color_array =
      mk_dict->GetArrayFor(pdfium::appearance::kBG);
  if (background_color_array) {
    appearance_characteristics.background_color =
        fpdfdoc::CFXColorFromArray(*background_color_array);
  }
  appearance_characteristics.caption =
      mk_dict->GetUnicodeTextFor(pdfium::appearance::kCA);
  return appearance_characteristics;
}

std::optional<CPDF_AppearanceGenerator::FormType> GetWidgetFormType(
    const CPDF_Dictionary* annot_dict) {
  RetainPtr<const CPDF_Object> field_type_obj =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, pdfium::form_fields::kFT);
  if (!field_type_obj) {
    return std::nullopt;
  }

  const ByteString field_type = field_type_obj->GetString();
  if (field_type == pdfium::form_fields::kTx) {
    return CPDF_AppearanceGenerator::kTextField;
  }

  if (field_type != pdfium::form_fields::kCh) {
    return std::nullopt;
  }

  RetainPtr<const CPDF_Object> field_flags_obj =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, pdfium::form_fields::kFf);
  const uint32_t flags = field_flags_obj ? field_flags_obj->GetInteger() : 0;
  return (flags & pdfium::form_flags::kChoiceCombo)
             ? CPDF_AppearanceGenerator::kComboBox
             : CPDF_AppearanceGenerator::kListBox;
}

}  // namespace appearance_generator

// static
void CPDF_AppearanceGenerator::GenerateFormAP(CPDF_Document* doc,
                                              CPDF_Dictionary* annot_dict,
                                              FormType type) {
  APGenerationTarget target{doc, annot_dict};
  GenerateFormAPToTarget(&target, annot_dict, type, nullptr);
}

// static
bool CPDF_AppearanceGenerator::GenerateFormAPWithValueOverride(
    CPDF_Document* doc,
    CPDF_Dictionary* annot_dict,
    FormType type,
    const WideString& value_override) {
  APGenerationTarget target{doc, annot_dict};
  return GenerateFormAPToTarget(&target, annot_dict, type, &value_override);
}

// static
std::optional<CPDF_AppearanceGenerator::GeneratedAP>
CPDF_AppearanceGenerator::GenerateEphemeralFormAP(
    CPDF_Document* doc,
    const CPDF_Dictionary* annot_dict,
    FormType type) {
  APGenerationTarget target{doc, nullptr};
  if (!GenerateFormAPToTarget(&target, const_cast<CPDF_Dictionary*>(annot_dict),
                              type, nullptr)) {
    return std::nullopt;
  }
  return GeneratedAP{std::move(target.ephemeral_fonts),
                     std::move(target.normal_stream)};
}

// static
void CPDF_AppearanceGenerator::GenerateCheckboxFormAP(
    CPDF_Document* doc,
    CPDF_Dictionary* annot_dict) {
  const AppearanceCharacteristics mk =
      GetAppearanceCharacteristics(annot_dict->GetDictFor("MK"));
  const BorderStyleInfo bs = GetBorderStyleInfo(annot_dict->GetDictFor("BS"));
  const AnnotationDimensionsAndColor dims =
      GetAnnotationDimensionsAndColor(annot_dict);

  CFX_FloatRect body_rect = dims.bbox;
  body_rect.Deflate(bs.width, bs.width);
  const CheckStyle style =
      CheckStyleFromCaption(mk.caption).value_or(CheckStyle::kCheck);

  // Off state: box only (background fill + border stroke).
  fxcrt::ostringstream off_content;
  BuildCheckboxBoxStream(off_content, mk, bs, dims.bbox);

  // On state: the same box and the symbol /MK /CA names, in the /DA colour.
  fxcrt::ostringstream yes_content;
  BuildCheckboxBoxStream(yes_content, mk, bs, dims.bbox);
  WriteCheckSymbol(yes_content, style,
                   CheckSymbolRect(body_rect, style, /*radio=*/false),
                   CheckSymbolColor(doc, annot_dict));

  const uint32_t off_obj_num =
      CreateFormXObjectStream(doc, off_content, dims.bbox, dims.matrix);
  const uint32_t yes_obj_num =
      CreateFormXObjectStream(doc, yes_content, dims.bbox, dims.matrix);

  RetainPtr<CPDF_Dictionary> ap_dict =
      annot_dict->GetOrCreateDictFor(pdfium::annotation::kAP);

  ByteString on_state;
  RetainPtr<const CPDF_Dictionary> old_n = ap_dict->GetDictFor("N");
  if (old_n) {
    CPDF_DictionaryLocker locker(old_n);
    for (const auto& it : locker) {
      if (it.first != "Off") {
        on_state = it.first;
        break;
      }
    }
  }
  if (on_state.IsEmpty()) {
    on_state = "Yes";
  }

  RetainPtr<CPDF_Dictionary> n_dict = ap_dict->SetNewFor<CPDF_Dictionary>("N");
  n_dict->SetNewFor<CPDF_Reference>("Off", doc, off_obj_num);
  n_dict->SetNewFor<CPDF_Reference>(on_state, doc, yes_obj_num);

  if (!annot_dict->KeyExist("AS")) {
    annot_dict->SetNewFor<CPDF_Name>("AS", "Off");
  }
}

// static
void CPDF_AppearanceGenerator::GenerateRadioButtonFormAP(
    CPDF_Document* doc,
    CPDF_Dictionary* annot_dict) {
  const AppearanceCharacteristics mk =
      GetAppearanceCharacteristics(annot_dict->GetDictFor("MK"));
  const BorderStyleInfo bs = GetBorderStyleInfo(annot_dict->GetDictFor("BS"));
  const AnnotationDimensionsAndColor dims =
      GetAnnotationDimensionsAndColor(annot_dict);

  const float cx = (dims.bbox.left + dims.bbox.right) / 2.0f;
  const float cy = (dims.bbox.bottom + dims.bbox.top) / 2.0f;
  const float rx = (dims.bbox.right - dims.bbox.left) / 2.0f;
  const float ry = (dims.bbox.top - dims.bbox.bottom) / 2.0f;
  const float inner_rx = (rx - bs.width) * 0.5f;
  const float inner_ry = (ry - bs.width) * 0.5f;

  constexpr float kKappa = 0.5523f;

  auto WriteEllipse = [&](fxcrt::ostringstream& s, float erx, float ery) {
    const float dx = kKappa * erx;
    const float dy = kKappa * ery;
    WriteFloat(s, cx) << " ";
    WriteFloat(s, cy + ery) << " m\n";
    WriteFloat(s, cx + dx) << " ";
    WriteFloat(s, cy + ery) << " ";
    WriteFloat(s, cx + erx) << " ";
    WriteFloat(s, cy + dy) << " ";
    WriteFloat(s, cx + erx) << " ";
    WriteFloat(s, cy) << " c\n";
    WriteFloat(s, cx + erx) << " ";
    WriteFloat(s, cy - dy) << " ";
    WriteFloat(s, cx + dx) << " ";
    WriteFloat(s, cy - ery) << " ";
    WriteFloat(s, cx) << " ";
    WriteFloat(s, cy - ery) << " c\n";
    WriteFloat(s, cx - dx) << " ";
    WriteFloat(s, cy - ery) << " ";
    WriteFloat(s, cx - erx) << " ";
    WriteFloat(s, cy - dy) << " ";
    WriteFloat(s, cx - erx) << " ";
    WriteFloat(s, cy) << " c\n";
    WriteFloat(s, cx - erx) << " ";
    WriteFloat(s, cy + dy) << " ";
    WriteFloat(s, cx - dx) << " ";
    WriteFloat(s, cy + ery) << " ";
    WriteFloat(s, cx) << " ";
    WriteFloat(s, cy + ery) << " c\n";
    s << "h\n";
  };

  const CheckStyle style =
      CheckStyleFromCaption(mk.caption).value_or(CheckStyle::kCircle);
  const CFX_Color symbol_color = CheckSymbolColor(doc, annot_dict);

  fxcrt::ostringstream off_content;
  fxcrt::ostringstream yes_content;
  if (style == CheckStyle::kCircle) {
    // Off state: circle only (background fill + border stroke).
    BuildRadioCircleStream(off_content, mk, bs, dims.bbox);

    // On state: the same circle and a filled dot, in the /DA colour.
    BuildRadioCircleStream(yes_content, mk, bs, dims.bbox);
    yes_content << "q\n"
                << GenerateColorAP(symbol_color, PaintOperation::kFill);
    WriteEllipse(yes_content, inner_rx, inner_ry);
    yes_content << "f*\nQ\n";
  } else {
    // Any other symbol sits in a box, as PDFium's form filler draws it.
    CFX_FloatRect body_rect = dims.bbox;
    body_rect.Deflate(bs.width, bs.width);
    BuildCheckboxBoxStream(off_content, mk, bs, dims.bbox);
    BuildCheckboxBoxStream(yes_content, mk, bs, dims.bbox);
    WriteCheckSymbol(yes_content, style,
                     CheckSymbolRect(body_rect, style, /*radio=*/true),
                     symbol_color);
  }

  const uint32_t off_obj_num =
      CreateFormXObjectStream(doc, off_content, dims.bbox, dims.matrix);
  const uint32_t yes_obj_num =
      CreateFormXObjectStream(doc, yes_content, dims.bbox, dims.matrix);

  RetainPtr<CPDF_Dictionary> ap_dict =
      annot_dict->GetOrCreateDictFor(pdfium::annotation::kAP);

  ByteString on_state;
  RetainPtr<const CPDF_Dictionary> old_n = ap_dict->GetDictFor("N");
  if (old_n) {
    CPDF_DictionaryLocker locker(old_n);
    for (const auto& it : locker) {
      if (it.first != "Off") {
        on_state = it.first;
        break;
      }
    }
  }
  if (on_state.IsEmpty()) {
    WideString nm = annot_dict->GetUnicodeTextFor("NM");
    if (!nm.IsEmpty()) {
      on_state = nm.ToUTF8();
    } else {
      on_state = "Yes";
    }
  }

  RetainPtr<CPDF_Dictionary> n_dict = ap_dict->SetNewFor<CPDF_Dictionary>("N");
  n_dict->SetNewFor<CPDF_Reference>("Off", doc, off_obj_num);
  n_dict->SetNewFor<CPDF_Reference>(on_state, doc, yes_obj_num);

  if (!annot_dict->KeyExist("AS")) {
    annot_dict->SetNewFor<CPDF_Name>("AS", "Off");
  }
}
