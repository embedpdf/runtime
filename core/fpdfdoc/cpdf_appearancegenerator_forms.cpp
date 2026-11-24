// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

// EmbedPDF: the appearance generator's form field widgets: text fields, combo
// and list boxes, push buttons, check boxes and radio buttons.
//
// See docs/embedpdf/fork-ledger.md, rule own-appearance-generator. EmbedPDF's
// changes are Copyright 2026 CloudPDF LTD.

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

#include "constants/annotation_common.h"
#include "constants/appearance.h"
#include "constants/form_fields.h"
#include "constants/form_flags.h"
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
#include "core/fpdfdoc/cpdf_annot_rotation.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_appearancegenerator_internal.h"
#include "core/fpdfdoc/cpdf_color_utils.h"
#include "core/fpdfdoc/cpdf_defaultappearance.h"
#include "core/fpdfdoc/cpdf_formfield.h"
#include "core/fpdfdoc/cpdf_iconfit.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fpdfdoc/cpdf_richtextlayout.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxcrt/stream_write_utils.h"
#include "core/fxge/cfx_fontregistry.h"
#include "core/fxge/cfx_renderdevice.h"

using appearance_generator::AnnotationDimensionsAndColor;
using appearance_generator::APGenerationTarget;
using appearance_generator::AppearanceCharacteristics;
using appearance_generator::BorderStyleInfo;
using appearance_generator::DaFontResolution;
using appearance_generator::DefaultAppearanceInfo;
using appearance_generator::EmitFieldTextBody;
using appearance_generator::GenerateAPDict;
using appearance_generator::GenerateBorderAP;
using appearance_generator::GenerateColorAP;
using appearance_generator::GenerateDirectFallbackFontDict;
using appearance_generator::GenerateEphemeralDefaultAcroFormDict;
using appearance_generator::GenerateExtGStateDict;
using appearance_generator::GenerateResourcesDict;
using appearance_generator::GetAnnotationDimensionsAndColor;
using appearance_generator::GetAnnotBorderStyleInfo;
using appearance_generator::GetAppearanceCharacteristics;
using appearance_generator::GetDefaultAppearanceInfo;
using appearance_generator::PaintOperation;
using appearance_generator::ResolveDaFontForEphemeralTarget;
using appearance_generator::ResolveDaFontForPersistentTarget;

namespace {

// A field's text laid out in |plate| (CPDF_RichTextLayout::ArrangeField())
// in the /DA font, which is |map|'s first entry.
CPDF_RichTextLayout::Result ArrangeFieldText(
    CPDF_AnnotFontMap& map,
    const CPDF_RichTextLayout::Options& options,
    CPDF_RichTextLayout::Field field,
    const CFX_FloatRect& plate) {
  const CPDF_AnnotFontMap::EntryFacts face = map.DescribeEntry(0);
  field.face_entry = 0;
  field.weight = face.weight;
  field.italic = face.italic;
  CPDF_RichTextLayout layout(&map, options);
  return layout.ArrangeField(field, plate);
}

// A field's text laid out in |plate| (ArrangeFieldText()) and written: Td, Tf
// and a string per run. |content_height| receives the height the lines take.
ByteString FieldText(CPDF_AnnotFontMap& map,
                     const CPDF_RichTextLayout::Options& options,
                     CPDF_RichTextLayout::Field field,
                     const CFX_FloatRect& plate,
                     float* content_height = nullptr) {
  const CPDF_RichTextLayout::Result laid =
      ArrangeFieldText(map, options, std::move(field), plate);
  if (content_height) {
    *content_height = laid.content_height;
  }
  fxcrt::ostringstream body;
  EmitFieldTextBody(body, laid, map, plate);
  return ByteString(body);
}

// /Q as an alignment: 0 left, 1 centre, 2 right; anything else is left.
int FieldAlignment(int32_t q) {
  return q == 1 || q == 2 ? q : 0;
}

ByteString GenerateTextFieldAP(const CPDF_Dictionary* annot_dict,
                               const CFX_FloatRect& body_rect,
                               float font_size,
                               CPDF_AnnotFontMap& map,
                               const CPDF_RichTextLayout::Options& options,
                               const WideString* value_override) {
  RetainPtr<const CPDF_Object> v_field =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, pdfium::form_fields::kV);
  RetainPtr<const CPDF_Object> q_field =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, "Q");
  RetainPtr<const CPDF_Object> ff_field =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, pdfium::form_fields::kFf);
  const uint32_t flags = ff_field ? ff_field->GetInteger() : 0;
  RetainPtr<const CPDF_Object> max_len_field =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, "MaxLen");
  const uint32_t max_len = max_len_field ? max_len_field->GetInteger() : 0;

  CPDF_RichTextLayout::Field field;
  field.text = value_override
                   ? *value_override
                   : (v_field ? v_field->GetUnicodeText() : WideString());
  field.size = font_size;
  field.alignment = FieldAlignment(q_field ? q_field->GetInteger() : 0);
  field.multiline = (flags >> 12) & 1;
  field.password = (flags >> 13) & 1;
  if ((flags >> 24) & 1) {
    field.cells = max_len;
  } else {
    field.limit = max_len;
  }
  field.centered = !field.multiline;
  return FieldText(map, options, std::move(field), body_rect);
}

ByteString GenerateComboBoxAP(const CPDF_Dictionary* annot_dict,
                              const CFX_FloatRect& body_rect,
                              const CFX_Color& text_color,
                              float font_size,
                              CPDF_AnnotFontMap& map,
                              const CPDF_RichTextLayout::Options& options,
                              const WideString* value_override) {
  fxcrt::ostringstream body_stream;

  RetainPtr<const CPDF_Object> v_field =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, pdfium::form_fields::kV);
  CFX_FloatRect button_rect = body_rect;
  button_rect.left = button_rect.right - 13;
  button_rect.Normalize();
  CFX_FloatRect edit_rect = body_rect;
  edit_rect.right = button_rect.left;
  edit_rect.Normalize();
  edit_rect.Deflate(4.0f, 0);

  CPDF_RichTextLayout::Field field;
  field.text = value_override
                   ? *value_override
                   : (v_field ? v_field->GetUnicodeText() : WideString());
  field.size = font_size;
  field.centered = true;
  const ByteString edit = FieldText(map, options, std::move(field), edit_rect);
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
                             CPDF_AnnotFontMap& map,
                             const CPDF_RichTextLayout::Options& options) {
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
      CPDF_RichTextLayout::Field field;
      if (opt->IsString()) {
        field.text = opt->GetUnicodeText();
      } else if (const CPDF_Array* opt_array = opt->AsArray()) {
        RetainPtr<const CPDF_Object> opt_item = opt_array->GetDirectObjectAt(1);
        if (opt_item) {
          field.text = opt_item->GetUnicodeText();
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
      // A row is one line, from the top of what is left.
      field.size = FXSYS_IsFloatZero(font_size) ? 12.0f : font_size;
      float item_height = 0;
      const ByteString row = FieldText(
          map, options, std::move(field),
          CFX_FloatRect(body_rect.left, fy, body_rect.right, fy), &item_height);
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
                    << row << "ET\n";
      } else {
        body_stream << "BT\n"
                    << GenerateColorAP(text_color, PaintOperation::kFill) << row
                    << "ET\n";
      }
      fy -= item_height;
    }
  }
  return ByteString(body_stream);
}

// A field's background and border, in isolated graphics state: what a text
// field draws under its text, and all a signature field's box is. A solid
// border is a stroked rectangle; the other styles draw as a combo box's and
// a list box's do.
void WriteFieldBox(fxcrt::ostringstream& app_stream,
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
}

void GenerateTextFieldFormAP(fxcrt::ostringstream& app_stream,
                             const CPDF_Dictionary* annot_dict,
                             const CFX_FloatRect& bbox,
                             const DefaultAppearanceInfo& da_info,
                             CPDF_AnnotFontMap& map,
                             const CPDF_RichTextLayout::Options& options,
                             const WideString* value_override) {
  const AppearanceCharacteristics mk =
      GetAppearanceCharacteristics(annot_dict->GetDictFor("MK"));
  const BorderStyleInfo bs = GetAnnotBorderStyleInfo(annot_dict);
  const float bw = bs.width;

  CFX_FloatRect body_rect = bbox;
  body_rect.Deflate(2.0f * bw, bw);

  WriteFieldBox(app_stream, mk, bs, bbox);

  // Text content in isolated graphics state with proper clipping
  app_stream << "/Tx BMC\nq\n";

  CFX_FloatRect clip_rect = bbox;
  clip_rect.Deflate(bw, bw);
  WriteRect(app_stream, clip_rect) << " re W n\n";

  ByteString body = GenerateTextFieldAP(
      annot_dict, body_rect, da_info.font_size, map, options, value_override);

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
                            CPDF_AnnotFontMap& map,
                            const CPDF_RichTextLayout::Options& options,
                            const WideString* value_override) {
  const AnnotationDimensionsAndColor dims =
      GetAnnotationDimensionsAndColor(annot_dict);
  const BorderStyleInfo border_info = GetAnnotBorderStyleInfo(annot_dict);

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
                                   da_info.font_size, map, options,
                                   value_override);
}

void GenerateListBoxFormAP(fxcrt::ostringstream& app_stream,
                           const CPDF_Dictionary* annot_dict,
                           const CFX_FloatRect& bbox,
                           const DefaultAppearanceInfo& da_info,
                           CPDF_AnnotFontMap& map,
                           const CPDF_RichTextLayout::Options& options) {
  const AnnotationDimensionsAndColor dims =
      GetAnnotationDimensionsAndColor(annot_dict);
  const BorderStyleInfo border_info = GetAnnotBorderStyleInfo(annot_dict);

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

  const ByteString body =
      GenerateListBoxAP(annot_dict, body_rect, da_info.text_color,
                        da_info.font_size, map, options);
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

// How a push button lays out its caption and its icon: /MK /TP (ISO 32000-2
// Table 192). A number the table doesn't list is 0, the caption alone, as an
// absent one is.
enum class ButtonLayout {
  kCaptionAlone = 0,
  kIconAlone = 1,
  kCaptionBelowIcon = 2,
  kCaptionAboveIcon = 3,
  kCaptionRightOfIcon = 4,
  kCaptionLeftOfIcon = 5,
  kCaptionOverIcon = 6,
};

ButtonLayout ButtonLayoutOf(const CPDF_Dictionary* annot_dict) {
  RetainPtr<const CPDF_Dictionary> mk = annot_dict->GetDictFor("MK");
  const int position = mk ? mk->GetIntegerFor("TP") : 0;
  return position >= 0 && position <= 6 ? static_cast<ButtonLayout>(position)
                                        : ButtonLayout::kCaptionAlone;
}

// The normal icon of a push button (/MK /I) when it can be drawn. An icon is
// drawn by reference, so only one that is an object of its own can be: a
// file always holds a stream so. And an icon is a form XObject (ISO 32000-2
// Table 192): an entry that names anything else, an image say, is no icon,
// here as for EPDFAnnot_GetMKIcon() and EPDFDoc_GetDrawings().
RetainPtr<const CPDF_Stream> ButtonIconOf(const CPDF_Dictionary* mk_dict) {
  RetainPtr<const CPDF_Stream> icon =
      mk_dict ? mk_dict->GetStreamFor(pdfium::appearance::kI) : nullptr;
  return icon && icon->GetObjNum() != 0 &&
                 icon->GetDict()->GetNameFor("Subtype") == "Form"
             ? icon
             : nullptr;
}

// A widget's /AP, to replace an appearance in: a dictionary of the widget's
// own. One that is an indirect object may be another widget's too, and a new
// /N in it would show in that widget as well. So the widget gets a copy
// first, which names the same appearances.
RetainPtr<CPDF_Dictionary> OwnAppearanceDict(CPDF_Document* doc,
                                             CPDF_Dictionary* annot_dict) {
  RetainPtr<const CPDF_Object> entry =
      annot_dict->GetObjectFor(pdfium::annotation::kAP);
  if (entry && entry->IsReference()) {
    RetainPtr<const CPDF_Dictionary> shared =
        annot_dict->GetDictFor(pdfium::annotation::kAP);
    if (shared) {
      annot_dict->SetFor(pdfium::annotation::kAP, shared->CloneForHolder(doc));
    }
  }
  return annot_dict->GetOrCreateDictFor(pdfium::annotation::kAP);
}

// True for a push button that draws no caption beside its icon: one that
// shows its icon alone (/MK /TP 1), and one that shows both (2 to 6) when it
// has an icon and no caption. GenerateIconButtonAP() draws these.
bool ButtonDrawsNoCaption(const CPDF_Dictionary* annot_dict) {
  switch (ButtonLayoutOf(annot_dict)) {
    case ButtonLayout::kCaptionAlone:
      return false;
    case ButtonLayout::kIconAlone:
      return true;
    default: {
      RetainPtr<const CPDF_Dictionary> mk = annot_dict->GetDictFor("MK");
      return ButtonIconOf(mk.Get()) &&
             mk->GetUnicodeTextFor(pdfium::appearance::kCA).IsEmpty();
    }
  }
}

// The box an icon takes where it is drawn: its /BBox through its /Matrix.
CFX_FloatRect IconBox(const CPDF_Stream* icon) {
  RetainPtr<const CPDF_Dictionary> dict = icon->GetDict();
  CFX_FloatRect box = dict->GetRectFor("BBox");
  box.Normalize();
  box = dict->GetMatrixFor("Matrix").TransformRect(box);
  box.Normalize();
  return box;
}

// The matrix that draws an icon whose box is `icon` in `plate`, as the icon
// fit dictionary says (ISO 32000-2 Table 250): when the icon is scaled (/SW),
// with or without its proportions (/S), and how the room left over is shared
// between its left and its bottom (/A). Without a dictionary these are the
// table's defaults: always scaled, keeping its proportions, in the middle.
// Nothing for an icon or a plate without area.
std::optional<CFX_Matrix> IconPlacement(const CFX_FloatRect& icon,
                                        const CFX_FloatRect& plate,
                                        const CPDF_IconFit& fit) {
  const float icon_width = icon.Width();
  const float icon_height = icon.Height();
  const float plate_width = plate.Width();
  const float plate_height = plate.Height();
  if (!(icon_width > 0) || !(icon_height > 0) || !(plate_width > 0) ||
      !(plate_height > 0)) {
    return std::nullopt;
  }

  float scale_x = 1;
  float scale_y = 1;
  switch (fit.GetScaleMethod()) {
    case CPDF_IconFit::ScaleMethod::kAlways:
      scale_x = plate_width / icon_width;
      scale_y = plate_height / icon_height;
      break;
    case CPDF_IconFit::ScaleMethod::kBigger:
      if (icon_width > plate_width) {
        scale_x = plate_width / icon_width;
      }
      if (icon_height > plate_height) {
        scale_y = plate_height / icon_height;
      }
      break;
    case CPDF_IconFit::ScaleMethod::kSmaller:
      if (icon_width < plate_width) {
        scale_x = plate_width / icon_width;
      }
      if (icon_height < plate_height) {
        scale_y = plate_height / icon_height;
      }
      break;
    case CPDF_IconFit::ScaleMethod::kNever:
      break;
  }
  if (fit.IsProportionalScale()) {
    scale_x = scale_y = std::min(scale_x, scale_y);
  }

  const CFX_PointF share = fit.GetIconBottomLeftPosition();
  const float left = plate.left + (plate_width - icon_width * scale_x) *
                                      std::clamp(share.x, 0.0f, 1.0f);
  const float bottom = plate.bottom + (plate_height - icon_height * scale_y) *
                                          std::clamp(share.y, 0.0f, 1.0f);
  return CFX_Matrix(scale_x, 0, 0, scale_y, left - icon.left * scale_x,
                    bottom - icon.bottom * scale_y);
}

// The part of a push button its icon is fitted into, alone or with the
// caption beside it: the button less the border's width at each edge, or the
// whole button when /MK /IF /FB says the icon reaches the edges.
CFX_FloatRect ButtonPlate(const CFX_FloatRect& bbox,
                          float border_width,
                          const CPDF_IconFit& fit) {
  CFX_FloatRect plate = bbox;
  if (!fit.GetFittingBounds()) {
    plate.Deflate(border_width, border_width);
  }
  return plate;
}

// Draws `icon` in `box`, fitted as `fit` says (IconPlacement()) and clipped
// to the box, by the name /FRM. False when nothing is drawn: an icon or a
// box without area.
bool WriteButtonIcon(fxcrt::ostringstream& content,
                     const CPDF_Stream* icon,
                     const CFX_FloatRect& box,
                     const CPDF_IconFit& fit) {
  const std::optional<CFX_Matrix> placement =
      IconPlacement(IconBox(icon), box, fit);
  if (!placement.has_value()) {
    return false;
  }
  content << "q\n";
  WriteRect(content, box) << " re W n\n";
  WriteMatrix(content, placement.value()) << " cm\n/FRM Do\nQ\n";
  return true;
}

// Names the icon an appearance draws /FRM in its resources, as Acrobat
// writes an image field, so the file holds the picture once, for /MK and
// for /AP.
void NameButtonIcon(CPDF_Dictionary* resources,
                    CPDF_Document* doc,
                    uint32_t icon_number) {
  resources->SetNewFor<CPDF_Dictionary>("XObject")->SetNewFor<CPDF_Reference>(
      "FRM", doc, icon_number);
}

// Where a push button that shows both draws its caption and its icon.
struct ButtonParts {
  CFX_FloatRect caption;
  CFX_FloatRect icon;  // without area when the caption takes the whole plate
};

// Shares `plate` between a caption and an icon by the rule of PDFium's form
// filler (GetPushButtonAppStream() in fpdfsdk/cpdfsdk_appstream.cpp), so a
// button looks the same whichever drew it. Below or above the icon, the
// caption takes a strip as high as its line; right or left of it, a strip
// as wide as its text; the icon takes the rest. A caption that needs more
// than the plate has takes all of it, and no icon is drawn. A caption whose
// size is fitted (a font size of 0) takes a third of the plate, and is
// fitted to that. Over the icon, both take the whole plate. `caption` is the
// room the caption takes at its font size, and is not read when `fitted`.
ButtonParts SplitButton(ButtonLayout layout,
                        const CFX_FloatRect& plate,
                        const CFX_SizeF& caption,
                        bool fitted) {
  ButtonParts parts = {plate, plate};
  if (layout == ButtonLayout::kCaptionOverIcon) {
    return parts;
  }
  const bool stacked = layout == ButtonLayout::kCaptionBelowIcon ||
                       layout == ButtonLayout::kCaptionAboveIcon;
  const float room = stacked ? plate.Height() : plate.Width();
  const float strip =
      fitted ? room / 3.0f : (stacked ? caption.height : caption.width);
  if (strip > room) {
    parts.icon = CFX_FloatRect();
    return parts;
  }
  switch (layout) {
    case ButtonLayout::kCaptionBelowIcon:
      parts.caption.top = plate.bottom + strip;
      parts.icon.bottom = parts.caption.top;
      break;
    case ButtonLayout::kCaptionAboveIcon:
      parts.caption.bottom = plate.top - strip;
      parts.icon.top = parts.caption.bottom;
      break;
    case ButtonLayout::kCaptionRightOfIcon:
      parts.caption.left = plate.right - strip;
      parts.icon.right = parts.caption.left;
      break;
    case ButtonLayout::kCaptionLeftOfIcon:
      parts.caption.right = plate.left + strip;
      parts.icon.left = parts.caption.right;
      break;
    default:
      break;
  }
  return parts;
}

// A push button's caption as the layout takes it: one line, centred both
// ways.
CPDF_RichTextLayout::Field ButtonCaptionField(const WideString& caption,
                                              float font_size) {
  CPDF_RichTextLayout::Field field;
  field.text = caption;
  field.size = font_size;
  field.alignment = 1;
  field.centered = true;
  return field;
}

// The room a caption takes at `font_size`: its line's width and height.
CFX_SizeF ButtonCaptionSize(CPDF_AnnotFontMap& map,
                            const CPDF_RichTextLayout::Options& options,
                            const WideString& caption,
                            float font_size,
                            const CFX_FloatRect& plate) {
  const CPDF_RichTextLayout::Result laid = ArrangeFieldText(
      map, options, ButtonCaptionField(caption, font_size), plate);
  float width = 0;
  for (const CPDF_RichTextLayout::Line& line : laid.lines) {
    width = std::max(width, line.width);
  }
  return CFX_SizeF(width, laid.content_height);
}

// Writes a push button's caption laid out in `body`, in the /DA font and
// colour, clipped to `clip`.
void WriteButtonCaption(fxcrt::ostringstream& app_stream,
                        const WideString& caption,
                        const CFX_FloatRect& body,
                        const CFX_FloatRect& clip,
                        const DefaultAppearanceInfo& da_info,
                        CPDF_AnnotFontMap& map,
                        const CPDF_RichTextLayout::Options& options) {
  const ByteString text = FieldText(
      map, options, ButtonCaptionField(caption, da_info.font_size), body);
  if (text.IsEmpty()) {
    return;
  }
  app_stream << "q\n";
  WriteRect(app_stream, clip) << " re W n\n";
  app_stream << "BT\n"
             << GenerateColorAP(da_info.text_color, PaintOperation::kFill)
             << text << "ET\nQ\n";
}

// A push button that draws a caption (GenerateIconButtonAP() draws the ones
// that draw none): background and border as a text field draws them, and the
// caption (/MK /CA) on one line, centred both ways, in the /DA font and
// colour.
//
// The caption alone (/MK /TP 0, or any layout without an icon) takes the
// whole button inside its border. In the layouts that show a caption and an
// icon (/MK /TP 2 to 6), a button that has an icon (/MK /I) draws both, each
// in the part SplitButton() gives it: the icon first, fitted into its part
// as /MK /IF says and clipped to it, then the caption in the middle of its
// part, so over the icon the caption is on top. When the caption leaves the
// icon no room, the caption is drawn alone.
//
// Returns the object number of the icon it drew, which the appearance's
// resources must name /FRM (NameButtonIcon()), or 0.
uint32_t GeneratePushButtonFormAP(fxcrt::ostringstream& app_stream,
                                  const CPDF_Dictionary* annot_dict,
                                  const CFX_FloatRect& bbox,
                                  const DefaultAppearanceInfo& da_info,
                                  CPDF_AnnotFontMap& map,
                                  const CPDF_RichTextLayout::Options& options) {
  RetainPtr<const CPDF_Dictionary> mk_dict = annot_dict->GetDictFor("MK");
  const AppearanceCharacteristics mk =
      GetAppearanceCharacteristics(mk_dict.Get());
  const BorderStyleInfo bs = GetAnnotBorderStyleInfo(annot_dict);
  const float bw = bs.width;

  app_stream << "q\n";
  BuildCheckboxBoxStream(app_stream, mk, bs, bbox);
  app_stream << "Q\n";
  if (mk.caption.IsEmpty()) {
    return 0;
  }

  const ButtonLayout layout = ButtonLayoutOf(annot_dict);
  RetainPtr<const CPDF_Stream> icon = layout == ButtonLayout::kCaptionAlone
                                          ? nullptr
                                          : ButtonIconOf(mk_dict.Get());
  if (icon) {
    const CPDF_IconFit fit(mk_dict->GetDictFor("IF"));
    const CFX_FloatRect plate = ButtonPlate(bbox, bw, fit);
    const bool fitted = FXSYS_IsFloatZero(da_info.font_size);
    const ButtonParts parts =
        SplitButton(layout, plate,
                    fitted ? CFX_SizeF()
                           : ButtonCaptionSize(map, options, mk.caption,
                                               da_info.font_size, plate),
                    fitted);
    if (WriteButtonIcon(app_stream, icon.Get(), parts.icon, fit)) {
      WriteButtonCaption(app_stream, mk.caption, parts.caption, parts.caption,
                         da_info, map, options);
      return icon->GetObjNum();
    }
  }

  CFX_FloatRect body_rect = bbox;
  body_rect.Deflate(2.0f * bw, bw);
  CFX_FloatRect clip_rect = bbox;
  clip_rect.Deflate(bw, bw);
  WriteButtonCaption(app_stream, mk.caption, body_rect, clip_rect, da_info, map,
                     options);
  return 0;
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

// The dictionary of a form XObject that draws in `bbox`, placed by `matrix`.
RetainPtr<CPDF_Dictionary> NewFormXObjectDict(
    const CFX_FloatRect& bbox,
    const CFX_Matrix& matrix,
    RetainPtr<CPDF_Dictionary> resources = nullptr) {
  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  stream_dict->SetNewFor<CPDF_Number>("FormType", 1);
  stream_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  stream_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  stream_dict->SetMatrixFor("Matrix", matrix);
  stream_dict->SetRectFor("BBox", bbox);
  if (resources) {
    stream_dict->SetFor("Resources", std::move(resources));
  }
  return stream_dict;
}

uint32_t CreateFormXObjectStream(
    CPDF_Document* doc,
    fxcrt::ostringstream& content,
    const CFX_FloatRect& bbox,
    const CFX_Matrix& matrix,
    RetainPtr<CPDF_Dictionary> resources = nullptr) {
  auto stream = doc->NewIndirect<CPDF_Stream>(
      NewFormXObjectDict(bbox, matrix, std::move(resources)));
  stream->SetDataFromStringstreamAndRemoveFilter(&content);
  return stream->GetObjNum();
}

// A push button that draws no caption (ButtonDrawsNoCaption()): background
// and border as every push button has them, and the normal icon (/MK /I), a
// form XObject of its own, drawn inside the border. The icon is fitted into
// the button's plate (ButtonPlate()) as /MK /IF says, and clipped to it. The
// button is laid out in the frame its quarter turn gives (/MK /R), as every
// field's contents are.
//
// It needs no /DA and no font, and without an icon it is the box alone.
// Written into the file, the appearance is a new stream, as a check box's
// are: the old /N may be another widget's too, or the icon itself. The old
// /N is not deleted, nor is an icon it drew: a save writes only what the
// document still reaches.
void GenerateIconButtonAP(APGenerationTarget* target,
                          const CPDF_Dictionary* annot_dict) {
  CPDF_Document* const doc = target->doc;
  RetainPtr<const CPDF_Dictionary> mk_dict = annot_dict->GetDictFor("MK");
  const AppearanceCharacteristics mk =
      GetAppearanceCharacteristics(mk_dict.Get());
  const BorderStyleInfo bs = GetAnnotBorderStyleInfo(annot_dict);
  const AnnotationDimensionsAndColor dims =
      GetAnnotationDimensionsAndColor(annot_dict);

  fxcrt::ostringstream content;
  content << "q\n";
  BuildCheckboxBoxStream(content, mk, bs, dims.bbox);
  content << "Q\n";

  RetainPtr<CPDF_Dictionary> resources;
  if (RetainPtr<const CPDF_Stream> icon = ButtonIconOf(mk_dict.Get())) {
    const CPDF_IconFit fit(mk_dict->GetDictFor("IF"));
    if (WriteButtonIcon(content, icon.Get(),
                        ButtonPlate(dims.bbox, bs.width, fit), fit)) {
      resources = doc->New<CPDF_Dictionary>();
      NameButtonIcon(resources.Get(), doc, icon->GetObjNum());
    }
  }

  if (!target->IsPersistent()) {
    // Drawn in memory: a stream of no document, and nothing is written.
    target->normal_stream = pdfium::MakeRetain<CPDF_Stream>(
        NewFormXObjectDict(dims.bbox, dims.matrix, std::move(resources)));
    target->normal_stream->SetDataFromStringstream(&content);
    return;
  }
  const uint32_t normal_obj_num = CreateFormXObjectStream(
      doc, content, dims.bbox, dims.matrix, std::move(resources));
  OwnAppearanceDict(doc, target->persistent_annot_dict)
      ->SetNewFor<CPDF_Reference>("N", doc, normal_obj_num);
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

  // A push button that draws no caption draws no text: it needs no /DA and
  // no font.
  if (type == CPDF_AppearanceGenerator::kPushButton &&
      ButtonDrawsNoCaption(annot_dict)) {
    GenerateIconButtonAP(target, annot_dict);
    return true;
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
    }
    da_font = ResolveDaFontForPersistentTarget(doc, mutable_dr_font_dict.Get(),
                                               font_name);
  } else {
    da_font =
        ResolveDaFontForEphemeralTarget(doc, dr_font_dict.Get(), font_name);
  }
  RetainPtr<CPDF_Dictionary> font_dict = da_font.font_dict;
  if (!font_dict &&
      da_font.registered_font_id == CFX_FontRegistry::kInvalidFontId &&
      !da_font.document_program) {
    // A /DA font the form doesn't have: draw with the fallback rather than
    // nothing.
    font_dict = GenerateDirectFallbackFontDict();
  }
  RetainPtr<CPDF_Font> default_font =
      font_dict ? CPDF_DocPageData::FromDocument(doc)->GetFont(font_dict)
                : nullptr;

  // The text lays out in the shared engine, through the annotation font
  // map: the /DA font first, then the fallbacks and the last resort, each
  // embedded as the field's appearance uses it.
  auto owned_map = std::make_unique<CPDF_AnnotFontMap>(
      doc, std::move(default_font), font_name,
      /*allow_registered_fallbacks=*/true, da_font.registered_font_id,
      /*install_dr_entry=*/target->IsPersistent(),
      CPDF_AnnotFontMap::Owner::kWidget);
  CPDF_AnnotFontMap& map = *owned_map;
  da_font.ApplyTo(&map);
  if (!map.HasDefaultFont()) {
    return false;
  }
  CPDF_RichTextLayout::Options options;
  options.typographic_features = doc->GetTypographicFeaturesEnabled();

  const AnnotationDimensionsAndColor dims =
      GetAnnotationDimensionsAndColor(annot_dict);
  fxcrt::ostringstream app_stream;
  // The icon a push button drew beside its caption, by object number.
  uint32_t drawn_icon = 0;
  switch (type) {
    case CPDF_AppearanceGenerator::kTextField:
      GenerateTextFieldFormAP(app_stream, annot_dict, dims.bbox,
                              default_appearance_info.value(), map, options,
                              value_override);
      break;
    case CPDF_AppearanceGenerator::kComboBox:
      GenerateComboBoxFormAP(app_stream, annot_dict, dims.bbox,
                             default_appearance_info.value(), map, options,
                             value_override);
      break;
    case CPDF_AppearanceGenerator::kListBox:
      GenerateListBoxFormAP(app_stream, annot_dict, dims.bbox,
                            default_appearance_info.value(), map, options);
      break;
    case CPDF_AppearanceGenerator::kPushButton:
      drawn_icon = GeneratePushButtonFormAP(app_stream, annot_dict, dims.bbox,
                                            default_appearance_info.value(),
                                            map, options);
      break;
  }

  if (!target->IsPersistent()) {
    RetainPtr<CPDF_Dictionary> fonts = map.CreateEphemeralFontResourceDict();
    if (!fonts) {
      return false;
    }
    auto resources_dict = GenerateResourcesDict(
        doc, GenerateExtGStateDict(*annot_dict, "Normal"), std::move(fonts));
    if (drawn_icon != 0) {
      NameButtonIcon(resources_dict.Get(), doc, drawn_icon);
    }
    target->ephemeral_fonts = std::move(owned_map);
    return GenerateAPDict(
        target, annot_dict, &app_stream, std::move(resources_dict),
        /*is_text_markup_annotation=*/false, dims.matrix, dims.bbox);
  }

  RetainPtr<CPDF_Dictionary> resource_font_dict = map.CreateFontResourceDict();
  if (!resource_font_dict) {
    return false;  // registered /DA font without a resource: no appearance
  }
  // A push button's appearance is a new stream every time, as a check box's
  // are. The stream it had may be another widget's too; it may be a picture
  // (the button's icon, now or before it was cleared), which is never drawn
  // over; and its resources may still name an icon the button no longer
  // shows, which would keep that picture in the file. The old stream is not
  // opened for writing, so nothing of it is copied into a layer. The other
  // fields write into the stream they have.
  RetainPtr<CPDF_Stream> normal_stream;
  if (type != CPDF_AppearanceGenerator::kPushButton) {
    RetainPtr<CPDF_Dictionary> ap_dict =
        annot_dict->GetMutableDictFor(pdfium::annotation::kAP);
    normal_stream = ap_dict ? ap_dict->GetMutableStreamFor("N") : nullptr;
  }
  if (!normal_stream) {
    normal_stream =
        doc->NewIndirect<CPDF_Stream>(pdfium::MakeRetain<CPDF_Dictionary>());
    OwnAppearanceDict(doc, annot_dict)
        ->SetNewFor<CPDF_Reference>("N", doc, normal_stream->GetObjNum());
  }
  normal_stream->SetDataFromStringstreamAndRemoveFilter(&app_stream);
  RetainPtr<CPDF_Dictionary> stream_dict = normal_stream->GetMutableDict();
  // A form XObject, said so: a stream made here has no type yet, and
  // readers look for /Subtype /Form.
  stream_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  stream_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  stream_dict->SetMatrixFor("Matrix", dims.matrix);
  stream_dict->SetRectFor("BBox", dims.bbox);
  RetainPtr<CPDF_Dictionary> resources =
      stream_dict->GetOrCreateDictFor("Resources");
  resources->SetFor("Font", std::move(resource_font_dict));
  if (drawn_icon != 0) {
    NameButtonIcon(resources.Get(), doc, drawn_icon);
  }
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

  appearance_characteristics.rotation = fpdfdoc::GetMKRotationDegrees(mk_dict);

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
void CPDF_AppearanceGenerator::GenerateBoxFormAP(CPDF_Document* doc,
                                                 CPDF_Dictionary* annot_dict) {
  const AppearanceCharacteristics mk =
      GetAppearanceCharacteristics(annot_dict->GetDictFor("MK"));
  const BorderStyleInfo bs = GetAnnotBorderStyleInfo(annot_dict);
  const AnnotationDimensionsAndColor dims =
      GetAnnotationDimensionsAndColor(annot_dict);

  fxcrt::ostringstream content;
  WriteFieldBox(content, mk, bs, dims.bbox);
  // A new stream, as a check box's are: the old /N may be another widget's
  // too, or a drawing someone placed in this one.
  const uint32_t normal_obj_num =
      CreateFormXObjectStream(doc, content, dims.bbox, dims.matrix);
  OwnAppearanceDict(doc, annot_dict)
      ->SetNewFor<CPDF_Reference>("N", doc, normal_obj_num);
}

// static
void CPDF_AppearanceGenerator::GenerateCheckboxFormAP(
    CPDF_Document* doc,
    CPDF_Dictionary* annot_dict) {
  const AppearanceCharacteristics mk =
      GetAppearanceCharacteristics(annot_dict->GetDictFor("MK"));
  const BorderStyleInfo bs = GetAnnotBorderStyleInfo(annot_dict);
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

  RetainPtr<CPDF_Dictionary> ap_dict = OwnAppearanceDict(doc, annot_dict);

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
  const BorderStyleInfo bs = GetAnnotBorderStyleInfo(annot_dict);
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

  RetainPtr<CPDF_Dictionary> ap_dict = OwnAppearanceDict(doc, annot_dict);

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
