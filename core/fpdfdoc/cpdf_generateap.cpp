// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfdoc/cpdf_generateap.h"

#include "core/fpdfdoc/cpdf_generateap_dimension.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <sstream>
#include <utility>

#include "constants/annotation_common.h"
#include "constants/appearance.h"
#include "constants/font_encodings.h"
#include "constants/form_fields.h"
#include "constants/form_flags.h"
#include "constants/transparency.h"
#include "core/fpdfapi/edit/cpdf_contentstream_write_utils.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_annotfontsubset.h"
#include "core/fpdfdoc/cpdf_cloudy_border.h"
#include "core/fpdfdoc/cpdf_color_utils.h"
#include "core/fpdfdoc/cpdf_defaultappearance.h"
#include "core/fpdfdoc/cpdf_formfield.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fpdfdoc/cpdf_richtext.h"
#include "core/fpdfdoc/cpdf_richtextlayout.h"
#include "core/fpdfdoc/cpdf_richtextparser.h"
#include "core/fpdfdoc/cpvt_fontmap.h"
#include "core/fpdfdoc/cpvt_variabletext.h"
#include "core/fpdfdoc/cpvt_word.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxcrt/notreached.h"
#include "core/fxge/cfx_fontregistry.h"
#include "core/fxge/cfx_renderdevice.h"

namespace {

constexpr char kGSDictName[] = "GS";

struct CPVT_Dash {
  CPVT_Dash(int32_t dash, int32_t gap, int32_t phase)
      : dash(dash), gap(gap), phase(phase) {}

  int32_t dash;
  int32_t gap;
  int32_t phase;
};

enum class PaintOperation { kStroke, kFill };

constexpr float kArrowAngle = FXSYS_PI / 6.0f;
constexpr float kArrowLenFactor = 9.0f;
constexpr float kButtLenFactor = 6.0f;
constexpr float kSlashLenFactor = 18.0f;

// Return a unit‑length copy of `v`.  If the vector has zero length, fall back
// to the X axis so that later maths cannot explode.
static CFX_PointF UnitVector(const CFX_PointF& v) {
  float len = std::hypot(v.x, v.y);
  if (len <= 0.0f) {
    return CFX_PointF(1.0f, 0.0f);
  }
  return CFX_PointF(v.x / len, v.y / len);
}

struct MarkupQuad {
  CFX_PointF upper_start;
  CFX_PointF upper_end;
  CFX_PointF lower_start;
  CFX_PointF lower_end;
};

constexpr float kMarkupQuadTolerance = 1e-4f;

float DotProduct(const CFX_PointF& lhs, const CFX_PointF& rhs) {
  return lhs.x * rhs.x + lhs.y * rhs.y;
}

float CrossProduct(const CFX_PointF& lhs, const CFX_PointF& rhs) {
  return lhs.x * rhs.y - lhs.y * rhs.x;
}

float VectorLength(const CFX_PointF& vector) {
  return std::hypot(vector.x, vector.y);
}

bool IsFinitePoint(const CFX_PointF& point) {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

std::optional<MarkupQuad> ReadMarkupQuad(const CPDF_Array* array,
                                         size_t index) {
  const size_t offset = index * 8;
  MarkupQuad quad = {
      {array->GetFloatAt(offset), array->GetFloatAt(offset + 1)},
      {array->GetFloatAt(offset + 2), array->GetFloatAt(offset + 3)},
      {array->GetFloatAt(offset + 4), array->GetFloatAt(offset + 5)},
      {array->GetFloatAt(offset + 6), array->GetFloatAt(offset + 7)}};
  if (!IsFinitePoint(quad.upper_start) || !IsFinitePoint(quad.upper_end) ||
      !IsFinitePoint(quad.lower_start) || !IsFinitePoint(quad.lower_end)) {
    return std::nullopt;
  }

  const CFX_PointF upper_edge = quad.upper_end - quad.upper_start;
  const CFX_PointF lower_edge = quad.lower_end - quad.lower_start;
  const CFX_PointF start_edge = quad.lower_start - quad.upper_start;
  const CFX_PointF end_edge = quad.lower_end - quad.upper_end;
  const float upper_length = VectorLength(upper_edge);
  const float lower_length = VectorLength(lower_edge);
  const float start_length = VectorLength(start_edge);
  const float end_length = VectorLength(end_edge);
  if (!std::isfinite(upper_length) || !std::isfinite(lower_length) ||
      !std::isfinite(start_length) || !std::isfinite(end_length) ||
      upper_length <= kMarkupQuadTolerance ||
      lower_length <= kMarkupQuadTolerance ||
      start_length <= kMarkupQuadTolerance ||
      end_length <= kMarkupQuadTolerance) {
    return std::nullopt;
  }

  // The PDF text-markup order is upper-start, upper-end, lower-start,
  // lower-end. Reject crossed and reversed producer data before interpreting
  // those pairs as a local frame.
  const float horizontal_alignment = DotProduct(upper_edge, lower_edge);
  const float vertical_alignment = DotProduct(start_edge, end_edge);
  if (!std::isfinite(horizontal_alignment) ||
      !std::isfinite(vertical_alignment) || horizontal_alignment <= 0.0f ||
      vertical_alignment <= 0.0f) {
    return std::nullopt;
  }
  const float start_area = CrossProduct(upper_edge, start_edge);
  const float end_area = CrossProduct(lower_edge, end_edge);
  if (!std::isfinite(start_area) || !std::isfinite(end_area) ||
      std::signbit(start_area) != std::signbit(end_area) ||
      std::abs(start_area) <=
          kMarkupQuadTolerance * upper_length * start_length ||
      std::abs(end_area) <= kMarkupQuadTolerance * lower_length * end_length) {
    return std::nullopt;
  }
  return quad;
}

bool IsAxisAlignedZigZag(const MarkupQuad& quad) {
  const float scale =
      std::max({1.0f, VectorLength(quad.upper_end - quad.upper_start),
                VectorLength(quad.lower_end - quad.lower_start),
                VectorLength(quad.lower_start - quad.upper_start),
                VectorLength(quad.lower_end - quad.upper_end)});
  const float tolerance = kMarkupQuadTolerance * scale;
  return std::abs(quad.upper_start.y - quad.upper_end.y) <= tolerance &&
         std::abs(quad.lower_start.y - quad.lower_end.y) <= tolerance &&
         std::abs(quad.upper_start.x - quad.lower_start.x) <= tolerance &&
         std::abs(quad.upper_end.x - quad.lower_end.x) <= tolerance;
}

CFX_PointF Midpoint(const CFX_PointF& first, const CFX_PointF& second) {
  return 0.5f * (first + second);
}

CFX_PointF LowerToUpperUnit(const MarkupQuad& quad) {
  return UnitVector((quad.upper_start - quad.lower_start) +
                    (quad.upper_end - quad.lower_end));
}

void EmitOrientedSquiggle(fxcrt::ostringstream& stream,
                          const MarkupQuad& quad,
                          float delta) {
  const CFX_PointF lower_edge = quad.lower_end - quad.lower_start;
  const float length = VectorLength(lower_edge);
  const CFX_PointF along = UnitVector(lower_edge);
  const CFX_PointF toward_upper = LowerToUpperUnit(quad);
  WritePoint(stream, quad.lower_start + delta * toward_upper) << " m ";

  float distance = delta;
  bool is_upwards = false;
  while (distance < length) {
    CFX_PointF point = quad.lower_start + distance * along;
    if (is_upwards) {
      point += delta * toward_upper;
    }
    WritePoint(stream, point) << " l ";
    distance += delta;
    is_upwards = !is_upwards;
  }

  const float remainder = length - (distance - delta);
  const float height = is_upwards ? remainder : delta - remainder;
  WritePoint(stream, quad.lower_end + height * toward_upper) << " l S\n";
}

// Read one token from a /LE array and return it as a ByteString, accepting
// both Name and String objects (some generators are sloppy).
static ByteString ReadLineEndingToken(const CPDF_Array* le, size_t idx) {
  if (!le || idx >= le->size()) {
    return ByteString();
  }

  RetainPtr<const CPDF_Object> obj = le->GetDirectObjectAt(idx);
  if (!obj) {
    return ByteString();
  }

  if (const CPDF_Name* n = obj->AsName()) {
    return n->GetString();
  }

  if (const CPDF_String* s = obj->AsString()) {
    return s->GetString();
  }

  return ByteString();  // unsupported type
}

// Produce “q … Q” wrapper that translates + rotates local path so that:
///   • the **tip** of the ending sits at `pos`
///   • the **x‑axis** of the local coord points along the segment direction
template <typename F>
void EmitEndingWithAngle(fxcrt::ostringstream& out,
                         const CFX_PointF& pos,
                         float final_angle_rad,
                         const F& emitter) {
  const float cos_a = cos(final_angle_rad);
  const float sin_a = sin(final_angle_rad);

  // WriteMatrix, never raw `<<`: an axis-aligned segment has cos ≈ ±4.4e-8
  // and default ostream float formatting would emit it in scientific
  // notation — not legal PDF number syntax (Acrobat rejects the file).
  out << "q ";
  WriteMatrix(out, CFX_Matrix(cos_a, sin_a, -sin_a, cos_a, pos.x, pos.y))
      << " cm\n";
  emitter();
  out << "Q\n";
}

enum class ArrowStyle { kOpen, kClosed };

static void EmitArrowPath(fxcrt::ostringstream& out,
                          float stroke_w,
                          ArrowStyle style,
                          bool do_fill) {
  const float len = kArrowLenFactor * stroke_w;
  const float a = kArrowAngle;  // 30°
  const float x = -len * std::cos(a);
  const float y = len * std::sin(a);

  if (style == ArrowStyle::kOpen) {
    // OpenArrow / ROpenArrow
    out << x << " " << y << " m 0 0 l " << x << " " << -y << " l S\n";
    return;
  }

  // ClosedArrow / RClosedArrow
  out << "0 0 m " << x << " " << y << " l " << x << " " << -y << " l "
      << (do_fill ? "b\n"      // fill + stroke
                  : "h S\n");  // just close and stroke (no fill)
}

void EmitCirclePath(fxcrt::ostringstream& out, float stroke_w, bool filled) {
  const float r = (stroke_w * 5.0f) / 2.0f;
  // This is a standard approximation for drawing a circle with Bezier curves.
  constexpr float kL = 0.5523f;
  const float d = kL * r;

  out << r << " 0 m " << r << " " << d << " " << d << " " << r << " 0 " << r
      << " c " << -d << " " << r << " " << -r << " " << d << " " << -r
      << " 0 c " << -r << " " << -d << " " << -d << " " << -r << " 0 " << -r
      << " c " << d << " " << -r << " " << r << " " << -d << " " << r << " 0 c "
      << (filled ? "B\n" : "S\n");
}

void EmitSquarePath(fxcrt::ostringstream& out, float stroke_w, bool filled) {
  const float h = (stroke_w * 6.0f) / 2.0f;
  out << -h << " " << -h << " m " << h << " " << -h << " l " << h << " " << h
      << " l " << -h << " " << h << " l h " << (filled ? "B\n" : "S\n");
}

void EmitDiamondPath(fxcrt::ostringstream& out, float stroke_w, bool filled) {
  const float h = (stroke_w * 6.0f) / 2.0f;
  out << "0 " << -h << " m " << h << " 0 l "
      << "0 " << h << " l " << -h << " 0 l h " << (filled ? "B\n" : "S\n");
}

void EmitButtOrSlashPath(fxcrt::ostringstream& out,
                         float stroke_w,
                         float len_factor) {
  const float l = (stroke_w * len_factor) / 2.0f;
  out << -l << " 0 m " << l << " 0 l S\n";
}

ByteString BlendModeToPDFName(BlendMode bm) {
  switch (bm) {
    case BlendMode::kNormal:
      return ByteString(pdfium::transparency::kNormal);
    case BlendMode::kMultiply:
      return ByteString(pdfium::transparency::kMultiply);
    case BlendMode::kScreen:
      return ByteString(pdfium::transparency::kScreen);
    case BlendMode::kOverlay:
      return ByteString(pdfium::transparency::kOverlay);
    case BlendMode::kDarken:
      return ByteString(pdfium::transparency::kDarken);
    case BlendMode::kLighten:
      return ByteString(pdfium::transparency::kLighten);
    case BlendMode::kColorDodge:
      return ByteString(pdfium::transparency::kColorDodge);
    case BlendMode::kColorBurn:
      return ByteString(pdfium::transparency::kColorBurn);
    case BlendMode::kHardLight:
      return ByteString(pdfium::transparency::kHardLight);
    case BlendMode::kSoftLight:
      return ByteString(pdfium::transparency::kSoftLight);
    case BlendMode::kDifference:
      return ByteString(pdfium::transparency::kDifference);
    case BlendMode::kExclusion:
      return ByteString(pdfium::transparency::kExclusion);
    case BlendMode::kHue:
      return ByteString(pdfium::transparency::kHue);
    case BlendMode::kSaturation:
      return ByteString(pdfium::transparency::kSaturation);
    case BlendMode::kColor:
      return ByteString(pdfium::transparency::kColor);
    case BlendMode::kLuminosity:
      return ByteString(pdfium::transparency::kLuminosity);
  }
  return ByteString(pdfium::transparency::kNormal);
}

static BlendMode DefaultBlendModeFor(CPDF_Annot::Subtype subtype) {
  switch (subtype) {
    case CPDF_Annot::Subtype::HIGHLIGHT:
      return BlendMode::kMultiply;
    default:
      return BlendMode::kNormal;
  }
}

bool SupportsEphemeralAnnotAP(CPDF_Annot::Subtype subtype) {
  switch (subtype) {
    case CPDF_Annot::Subtype::CIRCLE:
    case CPDF_Annot::Subtype::FREETEXT:
    case CPDF_Annot::Subtype::HIGHLIGHT:
    case CPDF_Annot::Subtype::INK:
    case CPDF_Annot::Subtype::LINE:
    case CPDF_Annot::Subtype::POLYGON:
    case CPDF_Annot::Subtype::POLYLINE:
    case CPDF_Annot::Subtype::SQUARE:
    case CPDF_Annot::Subtype::SQUIGGLY:
    case CPDF_Annot::Subtype::STRIKEOUT:
    case CPDF_Annot::Subtype::UNDERLINE:
    case CPDF_Annot::Subtype::WIDGET:
      return true;
    default:
      return false;
  }
}

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

ByteString StringFromFontNameAndSize(const ByteString& font_name,
                                     float font_size) {
  fxcrt::ostringstream font_stream;
  if (font_name.GetLength() > 0 && font_size > 0) {
    font_stream << "/" << font_name << " ";
    WriteFloat(font_stream, font_size) << " Tf\n";
  }
  return ByteString(font_stream);
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

void SetVtFontSize(float font_size, CPVT_VariableText& vt) {
  if (FXSYS_IsFloatZero(font_size)) {
    vt.SetAutoFontSize(true);
  } else {
    vt.SetFontSize(font_size);
  }
}

// ISO 32000-1:2008 spec, table 166.
// ISO 32000-2:2020 spec, table 168.
struct BorderStyleInfo {
  float width = 1;
  BorderStyle style = BorderStyle::kSolid;
  CPVT_Dash dash_pattern{3, 0, 0};
};

BorderStyleInfo GetBorderStyleInfo(const CPDF_Dictionary* border_style_dict) {
  BorderStyleInfo border_style_info;
  if (!border_style_dict) {
    return border_style_info;
  }

  if (border_style_dict->KeyExist("W")) {
    border_style_info.width = border_style_dict->GetFloatFor("W");
  }

  const ByteString border_style_string =
      border_style_dict->GetByteStringFor("S");
  if (border_style_string.GetLength()) {
    switch (border_style_string[0]) {
      case 'S':
        border_style_info.style = BorderStyle::kSolid;
        break;
      case 'D':
        border_style_info.style = BorderStyle::kDash;
        break;
      case 'B':
        border_style_info.style = BorderStyle::kBeveled;
        border_style_info.width *= 2;
        break;
      case 'I':
        border_style_info.style = BorderStyle::kInset;
        border_style_info.width *= 2;
        break;
      case 'U':
        border_style_info.style = BorderStyle::kUnderline;
        break;
    }
  }

  RetainPtr<const CPDF_Array> dash_array = border_style_dict->GetArrayFor("D");
  if (dash_array) {
    border_style_info.dash_pattern =
        CPVT_Dash(dash_array->GetIntegerAt(0), dash_array->GetIntegerAt(1),
                  dash_array->GetIntegerAt(2));
  }

  return border_style_info;
}

// ISO 32000-1:2008 spec, table 189.
// ISO 32000-2:2020 spec, table 192.
struct AppearanceCharacteristics {
  int rotation = 0;  // In degrees.
  CFX_Color border_color;
  CFX_Color background_color;
};

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
  return appearance_characteristics;
}

struct AnnotationDimensionsAndColor {
  CFX_FloatRect bbox;
  CFX_Matrix matrix;
  CFX_Color border_color;
  CFX_Color background_color;
};

AnnotationDimensionsAndColor GetAnnotationDimensionsAndColor(
    const CPDF_Dictionary* annot_dict) {
  const AppearanceCharacteristics appearance_characteristics =
      GetAppearanceCharacteristics(annot_dict->GetDictFor("MK"));
  const CFX_FloatRect annot_rect =
      annot_dict->GetRectFor(pdfium::annotation::kRect);

  CFX_FloatRect bbox_rect;
  CFX_Matrix matrix;
  switch (appearance_characteristics.rotation % 360) {
    case 0:
      bbox_rect = CFX_FloatRect(0, 0, annot_rect.right - annot_rect.left,
                                annot_rect.top - annot_rect.bottom);
      break;
    case 90:
      matrix = CFX_Matrix(0, 1, -1, 0, annot_rect.right - annot_rect.left, 0);
      bbox_rect = CFX_FloatRect(0, 0, annot_rect.top - annot_rect.bottom,
                                annot_rect.right - annot_rect.left);
      break;
    case 180:
      matrix = CFX_Matrix(-1, 0, 0, -1, annot_rect.right - annot_rect.left,
                          annot_rect.top - annot_rect.bottom);
      bbox_rect = CFX_FloatRect(0, 0, annot_rect.right - annot_rect.left,
                                annot_rect.top - annot_rect.bottom);
      break;
    case 270:
      matrix = CFX_Matrix(0, -1, 1, 0, 0, annot_rect.top - annot_rect.bottom);
      bbox_rect = CFX_FloatRect(0, 0, annot_rect.top - annot_rect.bottom,
                                annot_rect.right - annot_rect.left);
      break;
  }

  return {
      .bbox = bbox_rect,
      .matrix = matrix,
      .border_color = appearance_characteristics.border_color,
      .background_color = appearance_characteristics.background_color,
  };
}

constexpr char kEmbedMetadataKey[] = "EMBD_Metadata";
constexpr char kEmbedMetadataRotationKey[] = "Rotation";
constexpr char kEmbedMetadataUnrotatedRectKey[] = "UnrotatedRect";
constexpr char kEmbedMetadataVerticalAlignmentKey[] = "VerticalAlignment";

RetainPtr<const CPDF_Dictionary> GetEmbedMetadataDict(
    const CPDF_Dictionary* annot_dict) {
  return annot_dict ? annot_dict->GetDictFor(kEmbedMetadataKey) : nullptr;
}

float GetEmbedMetadataFloatFor(const CPDF_Dictionary* annot_dict,
                               ByteStringView key) {
  RetainPtr<const CPDF_Dictionary> metadata = GetEmbedMetadataDict(annot_dict);
  return metadata ? metadata->GetFloatFor(key) : 0.0f;
}

CFX_FloatRect GetEmbedMetadataRectFor(const CPDF_Dictionary* annot_dict,
                                      ByteStringView key) {
  RetainPtr<const CPDF_Dictionary> metadata = GetEmbedMetadataDict(annot_dict);
  return metadata ? metadata->GetRectFor(key) : CFX_FloatRect();
}

int GetEmbedMetadataIntegerFor(const CPDF_Dictionary* annot_dict,
                               ByteStringView key) {
  RetainPtr<const CPDF_Dictionary> metadata = GetEmbedMetadataDict(annot_dict);
  return metadata ? metadata->GetIntegerFor(key) : 0;
}

// Rotation info for shape annotations (Square, Circle) using EmbedPDF's
// /EMBD_Metadata rotation fields.
struct ShapeRotationInfo {
  CFX_FloatRect bbox;  // BBox for the AP stream (unrotated rect in page coords)
  CFX_Matrix matrix;   // Transforms from local BBox space to page/AABB space
  bool is_rotated;     // Whether rotation was applied
};

ShapeRotationInfo GetShapeRotationInfo(const CPDF_Dictionary* annot_dict) {
  ShapeRotationInfo info;
  info.is_rotated = false;
  info.matrix = CFX_Matrix();
  info.bbox = annot_dict->GetRectFor(pdfium::annotation::kRect);

  float rotate_deg =
      GetEmbedMetadataFloatFor(annot_dict, kEmbedMetadataRotationKey);
  // Normalize to [0, 360)
  rotate_deg = fmod(fmod(rotate_deg, 360.0f) + 360.0f, 360.0f);
  if (rotate_deg < 0.01f || rotate_deg > 359.99f) {
    return info;  // No rotation
  }

  CFX_FloatRect unrotated =
      GetEmbedMetadataRectFor(annot_dict, kEmbedMetadataUnrotatedRectKey);
  if (unrotated.IsEmpty()) {
    return info;  // No unrotated rect stored -> no rotation in AP
  }

  info.is_rotated = true;
  info.bbox = unrotated;

  const float theta = rotate_deg * 3.14159265358979323846f / 180.0f;
  // Snap the trig to exact 0/±1 near quarter turns (the values `upright`
  // authoring produces): float cos(90°) is ~-4.4e-8, which would otherwise
  // leak near-zero noise into the emitted matrix numbers.
  auto snap = [](float v) {
    if (fabsf(v) < 1e-6f) {
      return 0.0f;
    }
    if (fabsf(v - 1.0f) < 1e-6f) {
      return 1.0f;
    }
    if (fabsf(v + 1.0f) < 1e-6f) {
      return -1.0f;
    }
    return v;
  };
  const float cos_t = snap(cosf(theta));
  const float sin_t = snap(sinf(theta));
  const float cx = (unrotated.left + unrotated.right) / 2.0f;
  const float cy = (unrotated.bottom + unrotated.top) / 2.0f;

  // Matrix: rotate around center of unrotated rect
  // M = T(cx, cy) * R(theta) * T(-cx, -cy)
  info.matrix =
      CFX_Matrix(cos_t, sin_t, -sin_t, cos_t, cx * (1.0f - cos_t) + cy * sin_t,
                 cy * (1.0f - cos_t) - cx * sin_t);

  return info;
}

struct DefaultAppearanceInfo {
  ByteString font_name;
  float font_size;
  CFX_Color text_color;
};

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

ByteString GenerateEditAP(IPVT_FontMap* font_map,
                          CPVT_VariableText::Iterator* vt_iterator,
                          const CFX_PointF& offset,
                          bool continuous,
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
    if (continuous) {
      if (place.LineCmp(oldplace) != 0) {
        if (!words.IsEmpty()) {
          line_stream << GetWordRenderString(words.AsStringView());
          edit_stream << line_stream.str();
          line_stream.str("");
          words.clear();
        }
        CPVT_Word word;
        if (vt_iterator->GetWord(word)) {
          new_point =
              CFX_PointF(word.ptWord.x + offset.x, word.ptWord.y + offset.y);
        } else {
          CPVT_Line line;
          vt_iterator->GetLine(line);
          new_point =
              CFX_PointF(line.ptLine.x + offset.x, line.ptLine.y + offset.y);
        }
        if (new_point != old_point) {
          WritePoint(line_stream, new_point - old_point) << " Td\n";
          old_point = new_point;
        }
      }
      CPVT_Word word;
      if (vt_iterator->GetWord(word)) {
        if (word.nFontIndex != current_font_index) {
          if (!words.IsEmpty()) {
            line_stream << GetWordRenderString(words.AsStringView());
            words.clear();
          }
          line_stream << GetFontSetString(font_map, word.nFontIndex,
                                          word.fFontSize);
          current_font_index = word.nFontIndex;
        }
        words +=
            GetPDFWordString(font_map, current_font_index, word.Word, sub_word);
      }
      oldplace = place;
    } else {
      CPVT_Word word;
      if (vt_iterator->GetWord(word)) {
        new_point =
            CFX_PointF(word.ptWord.x + offset.x, word.ptWord.y + offset.y);
        if (new_point != old_point) {
          WritePoint(edit_stream, new_point - old_point) << " Td\n";
          old_point = new_point;
        }
        if (word.nFontIndex != current_font_index) {
          edit_stream << GetFontSetString(font_map, word.nFontIndex,
                                          word.fFontSize);
          current_font_index = word.nFontIndex;
        }
        edit_stream << GetWordRenderString(
            GetPDFWordString(font_map, current_font_index, word.Word, sub_word)
                .AsStringView());
      }
    }
  }
  if (!words.IsEmpty()) {
    line_stream << GetWordRenderString(words.AsStringView());
    edit_stream << line_stream.str();
  }
  return ByteString(edit_stream);
}

ByteString GenerateColorAP(const CFX_Color& color, PaintOperation operation) {
  fxcrt::ostringstream color_stream;
  switch (color.nColorType) {
    case CFX_Color::Type::kRGB:
      WriteFloat(color_stream, color.fColor1) << " ";
      WriteFloat(color_stream, color.fColor2) << " ";
      WriteFloat(color_stream, color.fColor3) << " ";
      color_stream << (operation == PaintOperation::kStroke ? "RG" : "rg")
                   << "\n";
      return ByteString(color_stream);
    case CFX_Color::Type::kGray:
      WriteFloat(color_stream, color.fColor1) << " ";
      color_stream << (operation == PaintOperation::kStroke ? "G" : "g")
                   << "\n";
      return ByteString(color_stream);
    case CFX_Color::Type::kCMYK:
      WriteFloat(color_stream, color.fColor1) << " ";
      WriteFloat(color_stream, color.fColor2) << " ";
      WriteFloat(color_stream, color.fColor3) << " ";
      WriteFloat(color_stream, color.fColor4) << " ";
      color_stream << (operation == PaintOperation::kStroke ? "K" : "k")
                   << "\n";
      return ByteString(color_stream);
    case CFX_Color::Type::kTransparent:
      return ByteString();
  }
  NOTREACHED();
}

ByteString GenerateBorderAP(const CFX_FloatRect& rect,
                            const BorderStyleInfo& border_style_info,
                            const CFX_Color& border_color) {
  const float width = border_style_info.width;
  if (width <= 0) {
    return ByteString();
  }

  fxcrt::ostringstream app_stream;
  const float left = rect.left;
  const float bottom = rect.bottom;
  const float right = rect.right;
  const float top = rect.top;
  const float half_width = width / 2.0f;
  switch (border_style_info.style) {
    case BorderStyle::kSolid: {
      ByteString color_string =
          GenerateColorAP(border_color, PaintOperation::kFill);
      if (color_string.GetLength() > 0) {
        app_stream << color_string;
        WriteRect(app_stream, rect) << " re\n";
        CFX_FloatRect inner_rect = rect;
        inner_rect.Deflate(width, width);
        WriteRect(app_stream, inner_rect) << " re f*\n";
      }
      return ByteString(app_stream);
    }
    case BorderStyle::kDash: {
      ByteString color_string =
          GenerateColorAP(border_color, PaintOperation::kStroke);
      if (color_string.GetLength() > 0) {
        const auto& dash = border_style_info.dash_pattern;
        app_stream << color_string;
        WriteFloat(app_stream, width) << " w [" << dash.dash << " " << dash.gap
                                      << "] " << dash.phase << " d\n";
        WritePoint(app_stream, {left + half_width, bottom + half_width})
            << " m\n";
        WritePoint(app_stream, {left + half_width, top - half_width}) << " l\n";
        WritePoint(app_stream, {right - half_width, top - half_width})
            << " l\n";
        WritePoint(app_stream, {right - half_width, bottom + half_width})
            << " l\n";
        WritePoint(app_stream, {left + half_width, bottom + half_width})
            << " l S\n";
      }
      return ByteString(app_stream);
    }
    case BorderStyle::kBeveled:
    case BorderStyle::kInset: {
      const float left_top_gray_value =
          border_style_info.style == BorderStyle::kBeveled ? 1.0f : 0.5f;
      app_stream << GenerateColorAP(
          CFX_Color(CFX_Color::Type::kGray, left_top_gray_value),
          PaintOperation::kFill);
      WritePoint(app_stream, {left + half_width, bottom + half_width})
          << " m\n";
      WritePoint(app_stream, {left + half_width, top - half_width}) << " l\n";
      WritePoint(app_stream, {right - half_width, top - half_width}) << " l\n";
      WritePoint(app_stream, {right - width, top - width}) << " l\n";
      WritePoint(app_stream, {left + width, top - width}) << " l\n";
      WritePoint(app_stream, {left + width, bottom + width}) << " l f\n";

      const float right_bottom_gray_value =
          border_style_info.style == BorderStyle::kBeveled ? 0.5f : 0.75f;
      app_stream << GenerateColorAP(
          CFX_Color(CFX_Color::Type::kGray, right_bottom_gray_value),
          PaintOperation::kFill);
      WritePoint(app_stream, {right - half_width, top - half_width}) << " m\n";
      WritePoint(app_stream, {right - half_width, bottom + half_width})
          << " l\n";
      WritePoint(app_stream, {left + half_width, bottom + half_width})
          << " l\n";
      WritePoint(app_stream, {left + width, bottom + width}) << " l\n";
      WritePoint(app_stream, {right - width, bottom + width}) << " l\n";
      WritePoint(app_stream, {right - width, top - width}) << " l f\n";

      ByteString color_string =
          GenerateColorAP(border_color, PaintOperation::kFill);
      if (color_string.GetLength() > 0) {
        app_stream << color_string;
        WriteRect(app_stream, rect) << " re\n";
        CFX_FloatRect inner_rect = rect;
        inner_rect.Deflate(half_width, half_width);
        WriteRect(app_stream, inner_rect) << " re f*\n";
      }
      return ByteString(app_stream);
    }
    case BorderStyle::kUnderline: {
      ByteString color_string =
          GenerateColorAP(border_color, PaintOperation::kStroke);
      if (color_string.GetLength() > 0) {
        app_stream << color_string;
        WriteFloat(app_stream, width) << " w\n";
        WritePoint(app_stream, {left, bottom + half_width}) << " m\n";
        WritePoint(app_stream, {right, bottom + half_width}) << " l S\n";
      }
      return ByteString(app_stream);
    }
  }
  NOTREACHED();
}

ByteString GetColorStringWithDefault(const CPDF_Array* color_array,
                                     const CFX_Color& default_color,
                                     PaintOperation operation) {
  if (color_array) {
    CFX_Color color = fpdfdoc::CFXColorFromArray(*color_array);
    return GenerateColorAP(color, operation);
  }

  return GenerateColorAP(default_color, operation);
}

float GetBorderWidth(const CPDF_Dictionary* dict) {
  RetainPtr<const CPDF_Dictionary> border_style_dict = dict->GetDictFor("BS");
  if (border_style_dict) {
    return border_style_dict->KeyExist("W")
               ? border_style_dict->GetFloatFor("W")
               : 1.0f;
  }

  auto border_array = dict->GetArrayFor(pdfium::annotation::kBorder);
  if (border_array && border_array->size() > 2) {
    return border_array->GetFloatAt(2);
  }

  return 1.0f;
}

RetainPtr<const CPDF_Array> GetDashArray(const CPDF_Dictionary* dict) {
  RetainPtr<const CPDF_Dictionary> border_style_dict = dict->GetDictFor("BS");
  if (border_style_dict) {
    return border_style_dict->GetByteStringFor("S") == "D"
               ? border_style_dict->GetArrayFor("D")
               : nullptr;
  }

  RetainPtr<const CPDF_Array> border_array =
      dict->GetArrayFor(pdfium::annotation::kBorder);
  if (border_array && border_array->size() == 4) {
    return border_array->GetArrayAt(3);
  }

  return nullptr;
}

inline CPDF_Annot::VerticalAlignment GetVerticalAlign(
    const CPDF_Dictionary* annot_dict) {
  const int v = GetEmbedMetadataIntegerFor(annot_dict,
                                           kEmbedMetadataVerticalAlignmentKey);
  if (v < static_cast<int>(CPDF_Annot::VerticalAlignment::kTop) ||
      v > static_cast<int>(CPDF_Annot::VerticalAlignment::kBottom)) {
    return CPDF_Annot::VerticalAlignment::kTop;  // fallback
  }
  return static_cast<CPDF_Annot::VerticalAlignment>(v);
}

ByteString GetDashPatternString(const CPDF_Dictionary* dict) {
  RetainPtr<const CPDF_Array> dash_array = GetDashArray(dict);
  if (!dash_array || dash_array->IsEmpty()) {
    return ByteString();
  }

  // Support maximum of ten elements in the dash array.
  size_t dash_arrayCount = std::min<size_t>(dash_array->size(), 10);
  fxcrt::ostringstream dash_stream;

  dash_stream << "[";
  for (size_t i = 0; i < dash_arrayCount; ++i) {
    WriteFloat(dash_stream, dash_array->GetFloatAt(i)) << " ";
  }
  dash_stream << "] 0 d\n";

  return ByteString(dash_stream);
}

ByteString GetPopupContentsString(CPDF_Document* doc,
                                  const CPDF_Dictionary& annot_dict,
                                  RetainPtr<CPDF_Font> default_font,
                                  const ByteString& font_name) {
  WideString value(annot_dict.GetUnicodeTextFor(pdfium::form_fields::kT));
  value += L'\n';
  value += annot_dict.GetUnicodeTextFor(pdfium::annotation::kContents);

  CPVT_FontMap map(doc, nullptr, std::move(default_font), font_name);
  CPVT_VariableText::Provider prd(&map);
  CPVT_VariableText vt(&prd);
  vt.SetPlateRect(annot_dict.GetRectFor(pdfium::annotation::kRect));
  vt.SetFontSize(12);
  vt.SetAutoReturn(true);
  vt.SetMultiLine(true);
  vt.Initialize();
  vt.SetText(value);
  vt.RearrangeAll();

  CFX_PointF offset(3.0f, -3.0f);
  ByteString content = GenerateEditAP(&map, vt.GetIterator(), offset, false, 0);

  if (content.IsEmpty()) {
    return ByteString();
  }

  ByteString color = GenerateColorAP(CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0),
                                     PaintOperation::kFill);

  return ByteString{"BT\n", color.AsStringView(), content.AsStringView(),
                    "ET\n", "Q\n"};
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

// EmbedPDF (A1): how a /DA font name resolves against /DR.
//  - present in /DR: that dictionary, plus the registered font it stands for
//    when it is one of ours (family first, session hint second);
//  - absent but spelled like a reserved registered-font alias: the registered
//    font alone. The real /DR entry is installed by CPDF_AnnotFontMap once
//    the appearance is generated and the glyph set is known;
//  - otherwise the Helvetica fallback, as before.
struct DaFontResolution {
  RetainPtr<CPDF_Dictionary> font_dict;
  CFX_FontRegistry::FontId registered_font_id =
      CFX_FontRegistry::kInvalidFontId;
};

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
  result.font_dict =
      GetFontFromDrFontDictOrGenerateFallback(doc, dr_font_dict, font_name);
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

  ByteString resource_key =
      ByteString::Format("FXF_%s", PDF_NameEncode(base_font_name).c_str());

  while (font_res->KeyExist(resource_key.AsStringView())) {
    static int suffix = 1;
    resource_key = ByteString::Format("%s_%d", resource_key.c_str(), suffix++);
  }

  // Build a minimal Type1 font dictionary
  RetainPtr<CPDF_Dictionary> new_font_dict = GenerateFallbackFontDict(doc);
  new_font_dict->SetNewFor<CPDF_Name>("BaseFont", base_font_name);

  // Register and return the key
  font_res->SetNewFor<CPDF_Reference>(resource_key, doc,
                                      new_font_dict->GetObjNum());
  return resource_key;
}

ByteString GetPaintOperatorString(bool is_stroke_rect, bool is_fill_rect) {
  if (is_stroke_rect) {
    return is_fill_rect ? "b" : "s";
  }
  return is_fill_rect ? "f" : "n";
}

struct CloudyBorderInfo {
  bool is_cloudy = false;
  float intensity = 0;
};

CloudyBorderInfo GetCloudyBorderInfo(const CPDF_Dictionary* annot_dict) {
  CloudyBorderInfo info;
  RetainPtr<const CPDF_Dictionary> be = annot_dict->GetDictFor("BE");
  if (!be || be->GetNameFor("S") != "C") {
    return info;
  }
  info.is_cloudy = true;
  info.intensity = be->KeyExist("I") ? be->GetFloatFor("I") : 1.0f;
  return info;
}

CFX_FloatRect GetRectDifferences(const CPDF_Dictionary* annot_dict) {
  RetainPtr<const CPDF_Array> rd = annot_dict->GetArrayFor("RD");
  if (!rd || rd->size() < 4) {
    return CFX_FloatRect();
  }
  return CFX_FloatRect(rd->GetFloatAt(0), rd->GetFloatAt(1), rd->GetFloatAt(2),
                       rd->GetFloatAt(3));
}

CPDF_Annot::LineEnding ReadCalloutLineEnding(
    const CPDF_Dictionary* annot_dict) {
  // Per spec (Table 174), FreeText /LE is a single Name.
  ByteString name = annot_dict->GetNameFor("LE");
  if (!name.IsEmpty()) {
    return CPDF_Annot::StringToLineEnding(name);
  }
  // Tolerance fallback: some writers store LE as an array.
  if (RetainPtr<const CPDF_Array> le = annot_dict->GetArrayFor("LE"); le) {
    if (le->size() >= 1) {
      return CPDF_Annot::StringToLineEnding(ReadLineEndingToken(le.Get(), 0));
    }
  }
  return CPDF_Annot::LineEnding::kNone;
}

ByteString GenerateTextSymbolAP(const CFX_FloatRect& rect,
                                const CPDF_Dictionary& annot_dict) {
  fxcrt::ostringstream app_stream;

  // Read fill color from /C array; default to yellow.
  CFX_Color fill_color(CFX_Color::Type::kRGB, 1, 1, 0);
  RetainPtr<const CPDF_Array> color_array =
      annot_dict.GetArrayFor(pdfium::annotation::kC);
  if (color_array) {
    fill_color = fpdfdoc::CFXColorFromArray(*color_array);
  }

  // Compute luminance-based contrast stroke (matches JS
  // getContrastStrokeColor).
  float luminance = 0.299f * fill_color.fColor1 + 0.587f * fill_color.fColor2 +
                    0.114f * fill_color.fColor3;
  CFX_Color stroke_color = luminance < 0.45f
                               ? CFX_Color(CFX_Color::Type::kRGB, 1, 1, 1)
                               : CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0);

  app_stream << GenerateColorAP(fill_color, PaintOperation::kFill);
  app_stream << GenerateColorAP(stroke_color, PaintOperation::kStroke);

  static constexpr int kBorderWidth = 1;
  app_stream << kBorderWidth << " w\n";

  static constexpr float kHalfWidth = kBorderWidth / 2.0f;
  static constexpr int kTipDelta = 4;

  CFX_FloatRect outer_rect1 = rect;
  outer_rect1.Deflate(kHalfWidth, kHalfWidth);
  outer_rect1.bottom += kTipDelta;

  CFX_FloatRect outer_rect2 = outer_rect1;
  outer_rect2.left += kTipDelta;
  outer_rect2.right = outer_rect2.left + kTipDelta;
  outer_rect2.top = outer_rect2.bottom - kTipDelta;
  float outer_rect2_middle = (outer_rect2.left + outer_rect2.right) / 2;

  // Draw outer boxes.
  WritePoint(app_stream, {outer_rect1.left, outer_rect1.bottom}) << " m\n";
  WritePoint(app_stream, {outer_rect1.left, outer_rect1.top}) << " l\n";
  WritePoint(app_stream, {outer_rect1.right, outer_rect1.top}) << " l\n";
  WritePoint(app_stream, {outer_rect1.right, outer_rect1.bottom}) << " l\n";
  WritePoint(app_stream, {outer_rect2.right, outer_rect2.bottom}) << " l\n";
  WritePoint(app_stream, {outer_rect2_middle, outer_rect2.top}) << " l\n";
  WritePoint(app_stream, {outer_rect2.left, outer_rect2.bottom}) << " l\n";
  WritePoint(app_stream, {outer_rect1.left, outer_rect1.bottom}) << " l\n";

  // Draw inner lines.
  CFX_FloatRect line_rect = outer_rect1;
  const float delta_x = 2;
  const float delta_y = (line_rect.top - line_rect.bottom) / 4;

  line_rect.left += delta_x;
  line_rect.right -= delta_x;
  for (int i = 0; i < 3; ++i) {
    line_rect.top -= delta_y;
    WritePoint(app_stream, {line_rect.left, line_rect.top}) << " m\n";
    WritePoint(app_stream, {line_rect.right, line_rect.top}) << " l\n";
  }
  app_stream << "B*\n";

  return ByteString(app_stream);
}

// Appends a closed ellipse path inscribed in |bounds| (same four-bezier
// construction as GenerateCircleAP).
void AppendEllipsePath(fxcrt::ostringstream& app_stream,
                       const CFX_FloatRect& bounds) {
  const float middle_x = (bounds.left + bounds.right) / 2;
  const float middle_y = (bounds.top + bounds.bottom) / 2;

  static constexpr float kL = 0.5523f;
  const float delta_x = kL * bounds.Width() / 2.0f;
  const float delta_y = kL * bounds.Height() / 2.0f;

  app_stream << middle_x << " " << bounds.top << " m\n";
  app_stream << middle_x + delta_x << " " << bounds.top << " " << bounds.right
             << " " << middle_y + delta_y << " " << bounds.right << " "
             << middle_y << " c\n";
  app_stream << bounds.right << " " << middle_y - delta_y << " "
             << middle_x + delta_x << " " << bounds.bottom << " " << middle_x
             << " " << bounds.bottom << " c\n";
  app_stream << middle_x - delta_x << " " << bounds.bottom << " "
             << bounds.left << " " << middle_y - delta_y << " " << bounds.left
             << " " << middle_y << " c\n";
  app_stream << bounds.left << " " << middle_y + delta_y << " "
             << middle_x - delta_x << " " << bounds.top << " " << middle_x
             << " " << bounds.top << " c\nh\n";
}

ByteString GenerateFileAttachmentSymbolAP(const CFX_FloatRect& rect,
                                          const CPDF_Dictionary& annot_dict) {
  fxcrt::ostringstream app_stream;

  // Read fill color from /C array; default to yellow (the note-icon
  // default in GenerateTextSymbolAP).
  CFX_Color fill_color(CFX_Color::Type::kRGB, 1, 1, 0);
  RetainPtr<const CPDF_Array> color_array =
      annot_dict.GetArrayFor(pdfium::annotation::kC);
  if (color_array) {
    fill_color = fpdfdoc::CFXColorFromArray(*color_array);
  }

  // Same luminance-based contrast stroke as GenerateTextSymbolAP.
  float luminance = 0.299f * fill_color.fColor1 + 0.587f * fill_color.fColor2 +
                    0.114f * fill_color.fColor3;
  CFX_Color stroke_color = luminance < 0.45f
                               ? CFX_Color(CFX_Color::Type::kRGB, 1, 1, 1)
                               : CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0);

  // /Name picks the glyph. Absent or foreign names mean PushPin, the
  // ISO 32000 default icon for file attachment annotations.
  CPDF_Annot::Icon icon =
      CPDF_Annot::StringToIcon(annot_dict.GetNameFor("Name"));
  if (icon != CPDF_Annot::Icon::kFile_Graph &&
      icon != CPDF_Annot::Icon::kFile_Paperclip &&
      icon != CPDF_Annot::Icon::kFile_Tag) {
    icon = CPDF_Annot::Icon::kFile_PushPin;
  }

  static constexpr int kBorderWidth = 1;
  static constexpr float kHalfWidth = kBorderWidth / 2.0f;
  CFX_FloatRect box = rect;
  box.Deflate(kHalfWidth, kHalfWidth);
  const float w = box.Width();
  const float h = box.Height();
  // Glyph coordinates below are fractions of the icon box.
  auto px = [&](float fx) { return box.left + fx * w; };
  auto py = [&](float fy) { return box.bottom + fy * h; };

  if (icon == CPDF_Annot::Icon::kFile_Paperclip) {
    // A paperclip is a wire, not a closed region, so /C colours the
    // stroked wire itself; a wider contrast pass underneath is the wire
    // counterpart of the note icon's contrast border.
    fxcrt::ostringstream path;
    WritePoint(path, {px(0.32f), py(0.28f)}) << " m\n";
    WritePoint(path, {px(0.32f), py(0.72f)}) << " l\n";
    path << px(0.32f) << " " << py(0.85f) << " " << px(0.68f) << " "
         << py(0.85f) << " " << px(0.68f) << " " << py(0.72f) << " c\n";
    WritePoint(path, {px(0.68f), py(0.20f)}) << " l\n";
    path << px(0.68f) << " " << py(0.10f) << " " << px(0.50f) << " "
         << py(0.10f) << " " << px(0.50f) << " " << py(0.20f) << " c\n";
    WritePoint(path, {px(0.50f), py(0.65f)}) << " l\n";
    path << px(0.50f) << " " << py(0.72f) << " " << px(0.41f) << " "
         << py(0.72f) << " " << px(0.41f) << " " << py(0.65f) << " c\n";
    WritePoint(path, {px(0.41f), py(0.30f)}) << " l\n";
    const ByteString wire(path);

    app_stream << "1 J\n1 j\n";
    app_stream << GenerateColorAP(stroke_color, PaintOperation::kStroke);
    WriteFloat(app_stream, 2.6f) << " w\n" << wire << "S\n";
    app_stream << GenerateColorAP(fill_color, PaintOperation::kStroke);
    WriteFloat(app_stream, 1.4f) << " w\n" << wire << "S\n";
    return ByteString(app_stream);
  }

  app_stream << GenerateColorAP(fill_color, PaintOperation::kFill);
  app_stream << GenerateColorAP(stroke_color, PaintOperation::kStroke);
  app_stream << kBorderWidth << " w\n";

  switch (icon) {
    case CPDF_Annot::Icon::kFile_PushPin: {
      // Round head, collar, and a tapering needle. Painted with the
      // nonzero rule (`B`) so touching subpaths merge instead of
      // punching even-odd holes.
      AppendEllipsePath(app_stream, CFX_FloatRect(px(0.33f), py(0.53f),
                                                  px(0.67f), py(0.87f)));
      app_stream << px(0.37f) << " " << py(0.46f) << " " << px(0.63f) - px(0.37f)
                 << " " << py(0.525f) - py(0.46f) << " re\n";
      WritePoint(app_stream, {px(0.47f), py(0.455f)}) << " m\n";
      WritePoint(app_stream, {px(0.53f), py(0.455f)}) << " l\n";
      WritePoint(app_stream, {px(0.50f), py(0.10f)}) << " l\nh\n";
      app_stream << "B\n";
      break;
    }
    case CPDF_Annot::Icon::kFile_Graph: {
      // Even-odd turns the outer+inner rectangles into a frame ring; the
      // three bars sit inside the ring on its bottom edge.
      app_stream << px(0.10f) << " " << py(0.10f) << " " << px(0.90f) - px(0.10f)
                 << " " << py(0.90f) - py(0.10f) << " re\n";
      app_stream << px(0.16f) << " " << py(0.16f) << " " << px(0.84f) - px(0.16f)
                 << " " << py(0.84f) - py(0.16f) << " re\n";
      app_stream << px(0.22f) << " " << py(0.16f) << " " << px(0.36f) - px(0.22f)
                 << " " << py(0.40f) - py(0.16f) << " re\n";
      app_stream << px(0.43f) << " " << py(0.16f) << " " << px(0.57f) - px(0.43f)
                 << " " << py(0.56f) - py(0.16f) << " re\n";
      app_stream << px(0.64f) << " " << py(0.16f) << " " << px(0.78f) - px(0.64f)
                 << " " << py(0.76f) - py(0.16f) << " re\n";
      app_stream << "B*\n";
      break;
    }
    case CPDF_Annot::Icon::kFile_Tag: {
      // Label pentagon pointing left; even-odd punches the eyelet hole.
      WritePoint(app_stream, {px(0.10f), py(0.50f)}) << " m\n";
      WritePoint(app_stream, {px(0.34f), py(0.80f)}) << " l\n";
      WritePoint(app_stream, {px(0.90f), py(0.80f)}) << " l\n";
      WritePoint(app_stream, {px(0.90f), py(0.20f)}) << " l\n";
      WritePoint(app_stream, {px(0.34f), py(0.20f)}) << " l\nh\n";
      AppendEllipsePath(app_stream, CFX_FloatRect(px(0.305f), py(0.445f),
                                                  px(0.415f), py(0.555f)));
      app_stream << "B*\n";
      break;
    }
    default: {
      NOTREACHED();
    }
  }

  return ByteString(app_stream);
}

RetainPtr<CPDF_Dictionary> GenerateExtGStateDict(
    const CPDF_Dictionary& annot_dict,
    const ByteString& blend_mode) {
  auto gs_dict =
      pdfium::MakeRetain<CPDF_Dictionary>(annot_dict.GetByteStringPool());
  gs_dict->SetNewFor<CPDF_Name>("Type", "ExtGState");

  float opacity = annot_dict.KeyExist("CA") ? annot_dict.GetFloatFor("CA") : 1;
  gs_dict->SetNewFor<CPDF_Number>("CA", opacity);
  gs_dict->SetNewFor<CPDF_Number>("ca", opacity);
  gs_dict->SetNewFor<CPDF_Boolean>("AIS", false);
  gs_dict->SetNewFor<CPDF_Name>("BM", blend_mode);

  auto resources_dict =
      pdfium::MakeRetain<CPDF_Dictionary>(annot_dict.GetByteStringPool());
  resources_dict->SetFor(kGSDictName, std::move(gs_dict));
  return resources_dict;
}

RetainPtr<CPDF_Dictionary> GenerateResourcesDict(
    CPDF_Document* doc,
    RetainPtr<CPDF_Dictionary> gs_dict,
    RetainPtr<CPDF_Dictionary> font_resource_dict) {
  auto resources_dict = doc->New<CPDF_Dictionary>();
  if (gs_dict) {
    resources_dict->SetFor("ExtGState", gs_dict);
  }
  if (font_resource_dict) {
    resources_dict->SetFor("Font", font_resource_dict);
  }
  return resources_dict;
}

struct APGenerationTarget {
  CPDF_Document* const doc;
  CPDF_Dictionary* const persistent_annot_dict;
  std::unique_ptr<CPDF_AnnotFontMap> ephemeral_fonts;
  RetainPtr<CPDF_Stream> normal_stream;

  bool IsPersistent() const { return !!persistent_annot_dict; }
};

RetainPtr<CPDF_Dictionary> BuildAPStreamDict(
    const CPDF_Dictionary* annot_dict,
    RetainPtr<CPDF_Dictionary> resource_dict,
    bool is_text_markup_annotation,
    const CFX_Matrix& matrix,
    const CFX_FloatRect& bbox_override) {
  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  stream_dict->SetNewFor<CPDF_Number>("FormType", 1);
  stream_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  stream_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  stream_dict->SetMatrixFor("Matrix", matrix);

  CFX_FloatRect rect = !bbox_override.IsEmpty() ? bbox_override
                       : is_text_markup_annotation
                           ? CPDF_Annot::BoundingRectFromQuadPoints(annot_dict)
                           : annot_dict->GetRectFor(pdfium::annotation::kRect);
  stream_dict->SetRectFor("BBox", rect);
  stream_dict->SetFor("Resources", std::move(resource_dict));
  return stream_dict;
}

bool GenerateAPDict(APGenerationTarget* target,
                    const CPDF_Dictionary* annot_dict,
                    fxcrt::ostringstream* app_stream,
                    RetainPtr<CPDF_Dictionary> resource_dict,
                    bool is_text_markup_annotation,
                    const CFX_Matrix& matrix,
                    const CFX_FloatRect& bbox_override) {
  RetainPtr<CPDF_Dictionary> stream_dict =
      BuildAPStreamDict(annot_dict, std::move(resource_dict),
                        is_text_markup_annotation, matrix, bbox_override);

  target->normal_stream =
      target->IsPersistent()
          ? target->doc->NewIndirect<CPDF_Stream>(std::move(stream_dict))
          : pdfium::MakeRetain<CPDF_Stream>(std::move(stream_dict));
  target->normal_stream->SetDataFromStringstream(app_stream);

  if (!target->IsPersistent()) {
    return true;
  }

  RetainPtr<CPDF_Dictionary> ap_dict =
      target->persistent_annot_dict->GetOrCreateDictFor(
          pdfium::annotation::kAP);
  ap_dict->SetNewFor<CPDF_Reference>("N", target->doc,
                                     target->normal_stream->GetObjNum());
  return true;
}

bool GenerateAndSetAPDict(APGenerationTarget* target,
                          const CPDF_Dictionary* annot_dict,
                          fxcrt::ostringstream* app_stream,
                          RetainPtr<CPDF_Dictionary> resource_dict,
                          bool is_text_markup_annotation) {
  return GenerateAPDict(target, annot_dict, app_stream,
                        std::move(resource_dict), is_text_markup_annotation,
                        CFX_Matrix(), CFX_FloatRect());
}

// Overload that accepts explicit Matrix and BBox, used by rotation-aware
// shape annotation generators (Square, Circle).
bool GenerateAndSetAPDictWithTransform(APGenerationTarget* target,
                                       const CPDF_Dictionary* annot_dict,
                                       fxcrt::ostringstream* app_stream,
                                       RetainPtr<CPDF_Dictionary> resource_dict,
                                       const CFX_Matrix& matrix,
                                       const CFX_FloatRect& bbox) {
  return GenerateAPDict(target, annot_dict, app_stream,
                        std::move(resource_dict),
                        /*is_text_markup_annotation=*/false, matrix, bbox);
}

bool GenerateAndSetAPDictWithBBox(APGenerationTarget* target,
                                  const CPDF_Dictionary* annot_dict,
                                  fxcrt::ostringstream* app_stream,
                                  RetainPtr<CPDF_Dictionary> resource_dict,
                                  const CFX_FloatRect& bbox) {
  return GenerateAPDict(
      target, annot_dict, app_stream, std::move(resource_dict),
      /*is_text_markup_annotation=*/false, CFX_Matrix(), bbox);
}

bool GenerateAndSetAPDict(CPDF_Document* doc,
                          CPDF_Dictionary* annot_dict,
                          fxcrt::ostringstream* app_stream,
                          RetainPtr<CPDF_Dictionary> resource_dict,
                          bool is_text_markup_annotation) {
  APGenerationTarget target{doc, annot_dict};
  return GenerateAndSetAPDict(&target, annot_dict, app_stream,
                              std::move(resource_dict),
                              is_text_markup_annotation);
}

// This helper encapsulates all logic for drawing the start and end caps.
void GenerateLineEndings(fxcrt::ostringstream& ap,
                         const std::vector<CFX_PointF>& points,
                         const CPDF_Dictionary* annot_dict,
                         bool reverse_arrows = false) {
  if (points.size() < 2) {
    return;
  }

  // Get ending styles from the /LE array
  CPDF_Annot::LineEnding start_ending = CPDF_Annot::LineEnding::kNone;
  CPDF_Annot::LineEnding end_ending = CPDF_Annot::LineEnding::kNone;

  if (RetainPtr<const CPDF_Array> le = annot_dict->GetArrayFor("LE"); le) {
    if (le->size() >= 1) {
      start_ending =
          CPDF_Annot::StringToLineEnding(ReadLineEndingToken(le.Get(), 0));
    }
    if (le->size() >= 2) {
      end_ending =
          CPDF_Annot::StringToLineEnding(ReadLineEndingToken(le.Get(), 1));
    }
  }

  if (start_ending == CPDF_Annot::LineEnding::kNone &&
      end_ending == CPDF_Annot::LineEnding::kNone) {
    return;
  }

  // Get styles needed for drawing the endings
  const float border_w = GetBorderWidth(annot_dict);
  RetainPtr<const CPDF_Array> interior_color = annot_dict->GetArrayFor("IC");
  const bool has_fill = interior_color && !interior_color->IsEmpty();

  // Lambda to emit a single ending with the correct transformation
  auto emit_one = [&](const CPDF_Annot::LineEnding ending,
                      const CFX_PointF& tip, const CFX_PointF& unit_dir) {
    if (ending == CPDF_Annot::LineEnding::kNone ||
        ending == CPDF_Annot::LineEnding::kUnknown) {
      return;
    }

    const float line_angle = atan2(unit_dir.y, unit_dir.x);
    float final_angle = line_angle;

    switch (ending) {
      case CPDF_Annot::LineEnding::kButt:
        final_angle += FXSYS_PI / 2.0f;
        break;
      case CPDF_Annot::LineEnding::kSlash:
        final_angle -= FXSYS_PI / 1.5f;
        break;
      case CPDF_Annot::LineEnding::kRClosedArrow:
      case CPDF_Annot::LineEnding::kROpenArrow:
        final_angle += FXSYS_PI;
        break;
      default:
        break;
    }

    if (reverse_arrows && (ending == CPDF_Annot::LineEnding::kOpenArrow ||
                           ending == CPDF_Annot::LineEnding::kClosedArrow ||
                           ending == CPDF_Annot::LineEnding::kROpenArrow ||
                           ending == CPDF_Annot::LineEnding::kRClosedArrow)) {
      final_angle += FXSYS_PI;
    }

    EmitEndingWithAngle(ap, tip, final_angle, [&]() {
      switch (ending) {
        case CPDF_Annot::LineEnding::kClosedArrow:
        case CPDF_Annot::LineEnding::kRClosedArrow:
          EmitArrowPath(ap, border_w, ArrowStyle::kClosed, has_fill);
          break;
        case CPDF_Annot::LineEnding::kOpenArrow:
        case CPDF_Annot::LineEnding::kROpenArrow:
          EmitArrowPath(ap, border_w, ArrowStyle::kOpen, /*do_fill=*/false);
          break;
        case CPDF_Annot::LineEnding::kCircle:
          EmitCirclePath(ap, border_w, has_fill);
          break;
        case CPDF_Annot::LineEnding::kSquare:
          EmitSquarePath(ap, border_w, has_fill);
          break;
        case CPDF_Annot::LineEnding::kDiamond:
          EmitDiamondPath(ap, border_w, has_fill);
          break;
        case CPDF_Annot::LineEnding::kButt:
          EmitButtOrSlashPath(ap, border_w, kButtLenFactor);
          break;
        case CPDF_Annot::LineEnding::kSlash:
          EmitButtOrSlashPath(ap, border_w, kSlashLenFactor);
          break;
        default:
          break;
      }
    });
  };

  // Calculate directions and emit the start and end caps
  CFX_PointF start_tip = points.front();
  CFX_PointF end_tip = points.back();

  // The direction for the start ending is reversed from the first segment.
  CFX_PointF first_segment_dir = UnitVector(points[1] - points[0]);
  CFX_PointF start_dir_rev = {-first_segment_dir.x, -first_segment_dir.y};

  // The direction for the end ending is forward along the last segment.
  CFX_PointF end_dir = UnitVector(points.back() - points[points.size() - 2]);

  emit_one(start_ending, start_tip, start_dir_rev);
  emit_one(end_ending, end_tip, end_dir);
}

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
  vt.SetAlignment(align);
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
                    << GenerateColorAP(
                           CFX_Color(CFX_Color::Type::kRGB, 0, 51.0f / 255.0f,
                                     113.0f / 255.0f),
                           PaintOperation::kFill);
        WriteRect(body_stream, item_rect) << " re f\nQ\n";
        body_stream << "BT\n"
                    << GenerateColorAP(CFX_Color(CFX_Color::Type::kGray, 1),
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

bool GenerateCircleAP(APGenerationTarget* target,
                      CPDF_Dictionary* annot_dict,
                      const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  RetainPtr<const CPDF_Array> interior_color = annot_dict->GetArrayFor("IC");
  app_stream << GetColorStringWithDefault(
      interior_color.Get(), CFX_Color(CFX_Color::Type::kTransparent),
      PaintOperation::kFill);

  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0), PaintOperation::kStroke);

  float border_width = GetBorderWidth(annot_dict);
  bool is_stroke_rect = border_width > 0;
  CloudyBorderInfo cloudy_info = GetCloudyBorderInfo(annot_dict);

  if (is_stroke_rect) {
    app_stream << border_width << " w ";
    if (cloudy_info.is_cloudy) {
      app_stream << "1 j ";
    } else {
      app_stream << GetDashPatternString(annot_dict);
    }
  }

  const ShapeRotationInfo rot_info = GetShapeRotationInfo(annot_dict);
  CFX_FloatRect draw_rect = rot_info.bbox;
  draw_rect.Normalize();

  if (cloudy_info.is_cloudy) {
    CFX_FloatRect rd = GetRectDifferences(annot_dict);
    GenerateCloudyEllipsePath(app_stream, draw_rect, rd, cloudy_info.intensity,
                              border_width);
  } else {
    if (is_stroke_rect) {
      draw_rect.Deflate(border_width / 2, border_width / 2);
    }

    const float middle_x = (draw_rect.left + draw_rect.right) / 2;
    const float middle_y = (draw_rect.top + draw_rect.bottom) / 2;

    static constexpr float kL = 0.5523f;
    const float delta_x = kL * draw_rect.Width() / 2.0;
    const float delta_y = kL * draw_rect.Height() / 2.0;

    app_stream << middle_x << " " << draw_rect.top << " m\n";
    app_stream << middle_x + delta_x << " " << draw_rect.top << " "
               << draw_rect.right << " " << middle_y + delta_y << " "
               << draw_rect.right << " " << middle_y << " c\n";
    app_stream << draw_rect.right << " " << middle_y - delta_y << " "
               << middle_x + delta_x << " " << draw_rect.bottom << " "
               << middle_x << " " << draw_rect.bottom << " c\n";
    app_stream << middle_x - delta_x << " " << draw_rect.bottom << " "
               << draw_rect.left << " " << middle_y - delta_y << " "
               << draw_rect.left << " " << middle_y << " c\n";
    app_stream << draw_rect.left << " " << middle_y + delta_y << " "
               << middle_x - delta_x << " " << draw_rect.top << " " << middle_x
               << " " << draw_rect.top << " c\n";
  }

  bool is_fill_rect = interior_color && !interior_color->IsEmpty();
  app_stream << GetPaintOperatorString(is_stroke_rect, is_fill_rect) << "\n";

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);

  if (rot_info.is_rotated) {
    GenerateAndSetAPDictWithTransform(target, annot_dict, &app_stream,
                                      std::move(resources_dict),
                                      rot_info.matrix, rot_info.bbox);
  } else {
    GenerateAndSetAPDict(target, annot_dict, &app_stream,
                         std::move(resources_dict),
                         false /*IsTextMarkupAnnotation*/);
  }
  return true;
}

// ---- Rich text FreeText (Phase D)
// ---------------------------------------------

// The callout leader, arrow and text box, up to and including the box
// outline; shared by the CPVT and the rich text bodies. The text box comes
// back for the caller to fill.
struct CalloutEnvelope {
  CFX_FloatRect text_box;
  float border_w = 1;
  ShapeRotationInfo box_rot;
};

CalloutEnvelope AppendCalloutEnvelope(fxcrt::ostringstream& appearance_stream,
                                      const CPDF_Dictionary* annot_dict,
                                      const CPDF_Array* cl,
                                      const CFX_Color& da_color) {
  // (a) Read CL points.
  CFX_PointF tip(cl->GetFloatAt(0), cl->GetFloatAt(1));
  const bool has_knee = (cl->size() == 6);
  CFX_PointF knee(cl->GetFloatAt(2), cl->GetFloatAt(3));
  CFX_PointF conn =
      has_knee ? CFX_PointF(cl->GetFloatAt(4), cl->GetFloatAt(5)) : knee;

  // (b) Compute text box from Rect + RD.
  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  rect.Normalize();
  CFX_FloatRect rd = GetRectDifferences(annot_dict);
  CFX_FloatRect text_box(rect.left + rd.left, rect.bottom + rd.bottom,
                         rect.right - rd.right, rect.top - rd.top);

  // (b') EmbedPDF upright tilt: for a callout the /EMBD_Metadata pair means
  // the TEXT BOX only — `UnrotatedRect` is the logical text box, `Rotation`
  // its tilt about the box centre. The /CL leader stays page-space, so the
  // rotation is baked INLINE (a `q cm … Q` around the box + text below),
  // never as the form /Matrix — /Rect keeps placing the whole appearance
  // (RD then recovers the rotated box's AABB, the best axis-aligned box a
  // viewer regenerating this AP can draw).
  const ShapeRotationInfo box_rot = GetShapeRotationInfo(annot_dict);
  if (box_rot.is_rotated) {
    text_box = box_rot.bbox;
  }

  // (c) Border width and colors.
  const float border_w = GetBorderWidth(annot_dict);

  // (d) Set fill (from /C, default transparent), stroke (from DA), and line
  // width. When /C is absent we emit nothing for the fill and pick a
  // stroke-only paint operator below, so the text box doesn't fall back to
  // PDF's default black fill. Mirrors GenerateCircleAP / GenerateSquareAP.
  auto color_array = annot_dict->GetArrayFor(pdfium::annotation::kC);
  appearance_stream << GetColorStringWithDefault(
      color_array.Get(), CFX_Color(CFX_Color::Type::kTransparent),
      PaintOperation::kFill);
  appearance_stream << GenerateColorAP(da_color, PaintOperation::kStroke);
  if (border_w > 0) {
    appearance_stream << border_w << " w\n";
  }

  // (e) Draw callout polyline.
  // Extend conn along the incoming segment direction by half the border
  // width so the line slides under the text box rect's stroke area,
  // eliminating the angular gap at the connection point.
  const float half_bw = border_w / 2.0f;
  CFX_PointF last_start = has_knee ? knee : tip;
  CFX_PointF line_dir = UnitVector(conn - last_start);
  CFX_PointF adjusted_conn(conn.x + line_dir.x * half_bw,
                           conn.y + line_dir.y * half_bw);

  appearance_stream << tip.x << " " << tip.y << " m\n";
  if (has_knee) {
    appearance_stream << knee.x << " " << knee.y << " l\n";
  }
  appearance_stream << adjusted_conn.x << " " << adjusted_conn.y << " l S\n";

  // (f) Draw line ending at tip.
  CPDF_Annot::LineEnding le = ReadCalloutLineEnding(annot_dict);
  if (le != CPDF_Annot::LineEnding::kNone &&
      le != CPDF_Annot::LineEnding::kUnknown) {
    CFX_PointF dir = UnitVector(knee - tip);
    CFX_PointF dir_rev = {-dir.x, -dir.y};
    float angle = atan2(dir_rev.y, dir_rev.x);

    switch (le) {
      case CPDF_Annot::LineEnding::kRClosedArrow:
      case CPDF_Annot::LineEnding::kROpenArrow:
        angle += FXSYS_PI;
        break;
      case CPDF_Annot::LineEnding::kButt:
        angle += FXSYS_PI / 2.0f;
        break;
      case CPDF_Annot::LineEnding::kSlash:
        angle -= FXSYS_PI / 1.5f;
        break;
      default:
        break;
    }

    EmitEndingWithAngle(appearance_stream, tip, angle, [&]() {
      switch (le) {
        case CPDF_Annot::LineEnding::kOpenArrow:
        case CPDF_Annot::LineEnding::kROpenArrow:
          EmitArrowPath(appearance_stream, border_w, ArrowStyle::kOpen,
                        /*do_fill=*/false);
          break;
        case CPDF_Annot::LineEnding::kClosedArrow:
        case CPDF_Annot::LineEnding::kRClosedArrow:
          EmitArrowPath(appearance_stream, border_w, ArrowStyle::kClosed,
                        /*do_fill=*/false);
          break;
        case CPDF_Annot::LineEnding::kCircle:
          EmitCirclePath(appearance_stream, border_w, /*do_fill=*/false);
          break;
        case CPDF_Annot::LineEnding::kSquare:
          EmitSquarePath(appearance_stream, border_w, /*do_fill=*/false);
          break;
        case CPDF_Annot::LineEnding::kDiamond:
          EmitDiamondPath(appearance_stream, border_w, /*do_fill=*/false);
          break;
        case CPDF_Annot::LineEnding::kButt:
          EmitButtOrSlashPath(appearance_stream, border_w, kButtLenFactor);
          break;
        case CPDF_Annot::LineEnding::kSlash:
          EmitButtOrSlashPath(appearance_stream, border_w, kSlashLenFactor);
          break;
        default:
          break;
      }
    });
  }

  // (g) Draw text box rectangle. Pick the paint operator dynamically so a
  // missing /C means "no fill" (stroke-only) rather than falling back to
  // PDF's default black fill. Mirrors GenerateCircleAP / GenerateSquareAP.
  // An upright-tilted box (see (b')) authors in the logical box frame and
  // spins it about its centre via an inline `cm` — box + text only; the
  // leader/arrow above already drew in page space. WriteMatrix, never raw
  // `<<`: default ostream float formatting uses scientific notation for tiny
  // magnitudes (cos of a right angle ≈ -4.4e-8), which is not legal PDF
  // number syntax — Acrobat rejects the whole file as corrupt.
  if (box_rot.is_rotated) {
    appearance_stream << "q ";
    WriteMatrix(appearance_stream, box_rot.matrix) << " cm\n";
  }
  const bool is_fill_rect = color_array != nullptr;
  const bool is_stroke_rect = border_w > 0;
  CFX_FloatRect text_box_stroke = text_box;
  text_box_stroke.Deflate(half_bw, half_bw);
  WriteRect(appearance_stream, text_box_stroke)
      << " re " << GetPaintOperatorString(is_stroke_rect, is_fill_rect) << "\n";
  return {text_box, border_w, box_rot};
}

// Acrobat's text PLATE — where the text lays out, clips and scrolls — is the
// box deflated by TWICE the border width on every side: the ink band and an
// equal breathing band, so the text never touches the stroke. Measured from
// Acrobat 26 appearance streams at 1–12 pt on plain boxes and 1–7 pt on
// callouts (plan `2026-09-15-free-text-plate-inset.md`); no /RD is involved,
// the inset is derived at appearance time. One formula for every branch —
// rich and CPVT, plain box and callout — and the TypeScript `textPlateInset`
// mirrors it so the live editor sits exactly where the baked text lands.
// Acrobat's thinnest border is 1 pt; a width of 0 is ours alone and gives no
// inset (the plate is the box) rather than an invented minimum.
float FreeTextPlateInset(float border_width) {
  return 2.0f * std::max(border_width, 0.0f);
}

// The plate of a box. A border that swallows the box leaves an EMPTY plate
// (zero width and/or height at the box centre): the envelope still paints,
// the text has nowhere to go. Never an inverted rect.
CFX_FloatRect FreeTextPlate(const CFX_FloatRect& box, float border_width) {
  CFX_FloatRect plate = box;
  plate.Normalize();
  const float inset = FreeTextPlateInset(border_width);
  if (plate.Width() > 2 * inset && plate.Height() > 2 * inset) {
    plate.Deflate(inset, inset);
    return plate;
  }
  const float width = std::max(0.0f, plate.Width() - 2 * inset);
  const float height = std::max(0.0f, plate.Height() - 2 * inset);
  const float cx = (plate.left + plate.right) / 2;
  const float cy = (plate.bottom + plate.top) / 2;
  return CFX_FloatRect(cx - width / 2, cy - height / 2, cx + width / 2,
                       cy + height / 2);
}

// Numbers with three decimals, trailing zeros trimmed: what Acrobat writes
// ("128.982", "-26.4", "11.88"), and enough for the 0.05 pt parity.
ByteString RichNumber(float value) {
  if (std::fabs(value) < 0.0005f) {
    return "0";
  }
  ByteString text = ByteString::Format("%.3f", value);
  if (text.Contains('.')) {
    while (text.Back() == '0') {
      text = text.First(text.GetLength() - 1);
    }
    if (text.Back() == '.') {
      text = text.First(text.GetLength() - 1);
    }
  }
  return text;
}

fxcrt::ostringstream& RichNum(fxcrt::ostringstream& s, float value) {
  s << RichNumber(value);
  return s;
}

ByteString RichColorOperator(FX_ARGB argb, bool fill) {
  return RichNumber(FXARGB_R(argb) / 255.0f) + " " +
         RichNumber(FXARGB_G(argb) / 255.0f) + " " +
         RichNumber(FXARGB_B(argb) / 255.0f) + (fill ? " rg\n" : " RG\n");
}

ByteString HexUtf16(const WideString& text) {
  ByteString hex("<FEFF");
  for (wchar_t wch : text) {
    uint32_t code = static_cast<uint32_t>(wch);
    if (code > 0xFFFF) {
      code -= 0x10000;
      hex += ByteString::Format("%04X%04X", 0xD800 + (code >> 10),
                                0xDC00 + (code & 0x3FF));
    } else {
      hex += ByteString::Format("%04X", code);
    }
  }
  hex += ">";
  return hex;
}

// A stroked decoration segment, drawn after ET the way Acrobat does.
struct DecorationSegment {
  float x0 = 0;
  float x1 = 0;
  float y = 0;
  float thickness = 0;
  FX_ARGB color = 0xFF000000;
};

// The text body of a rich FreeText appearance (C note §5): one Tj per
// word, relative Td per word, state operators only when they change,
// ActualText around runs whose glyphs do not spell their text.
void EmitRichTextBody(fxcrt::ostringstream& s,
                      const CPDF_RichTextLayout::Result& layout,
                      CPDF_AnnotFontMap& map,
                      const CFX_FloatRect& area) {
  using GlyphRun = CPDF_RichTextLayout::GlyphRun;
  using ShapedGlyph = CPDF_RichTextLayout::ShapedGlyph;

  s << "BT\n";
  int cur_entry = -1;
  float cur_size = -1;
  std::optional<FX_ARGB> cur_color;
  float cur_tc = 0;
  float cur_ts = 0;
  float cur_tz = 100;
  CFX_PointF origin(0, 0);  // the last Td, in the form's space
  std::vector<DecorationSegment> decorations;

  for (const CPDF_RichTextLayout::Line& line : layout.lines) {
    const float baseline = area.top - line.baseline_y;
    for (const GlyphRun& run : line.runs) {
      if (run.glyphs.empty()) {
        continue;
      }
      const CPDF_AnnotFontMap::RichFace face = map.GetRichFace(run.font_entry);
      const bool simple = face.pdf_font != nullptr;
      const float size = run.style.size;
      const float scale = run.style.horz_scale > 0 ? run.style.horz_scale : 1;

      if (run.font_entry != cur_entry || size != cur_size) {
        s << "/" << map.GetPDFFontAlias(run.font_entry) << " ";
        RichNum(s, size) << " Tf\n";
        cur_entry = run.font_entry;
        cur_size = size;
      }
      if (!cur_color.has_value() || *cur_color != run.style.color) {
        s << RichColorOperator(run.style.color, /*fill=*/true);
        cur_color = run.style.color;
      }
      if (run.style.letter_spacing != cur_tc) {
        RichNum(s, run.style.letter_spacing) << " Tc\n";
        cur_tc = run.style.letter_spacing;
      }
      if (run.rise != cur_ts) {
        RichNum(s, run.rise) << " Ts\n";
        cur_ts = run.rise;
      }
      if (scale * 100 != cur_tz) {
        RichNum(s, scale * 100) << " Tz\n";
        cur_tz = scale * 100;
      }

      // Charcodes first: a code that already stands for another scalar
      // makes the run's text unrecoverable from ToUnicode (C note §1.4).
      std::vector<uint16_t> codes;
      codes.reserve(run.glyphs.size());
      bool actual_text = run.needs_actual_text;
      for (const ShapedGlyph& glyph : run.glyphs) {
        const uint32_t unicode =
            glyph.cluster < run.text.GetLength()
                ? static_cast<uint32_t>(run.text[glyph.cluster])
                : 0;
        bool shared = false;
        codes.push_back(
            map.EncodeRichGlyph(run.font_entry, glyph.gid, unicode, &shared));
        actual_text = actual_text || shared;
      }
      if (actual_text) {
        s << "/Span <</ActualText " << HexUtf16(run.text) << ">> BDC\n";
      }

      auto is_space = [&](const ShapedGlyph& glyph) {
        return glyph.cluster < run.text.GetLength() &&
               (run.text[glyph.cluster] == L' ' ||
                run.text[glyph.cluster] == 0x00A0);
      };
      auto nominal_advance = [&](const ShapedGlyph& glyph, uint16_t code) {
        const int width =
            simple
                ? (code ? face.pdf_font->GetCharWidthF(code) : 0)
                : (face.program ? face.program->GetGlyphWidth(glyph.gid) : 0);
        return (width * size / 1000.0f + run.style.letter_spacing) * scale;
      };

      // Words: a space ends the word it follows, as in Acrobat's streams.
      // Run positions are relative to the text area's left edge.
      const float run_left = area.left + run.x;
      float x = run_left;
      size_t i = 0;
      const size_t count = run.glyphs.size();
      while (i < count) {
        size_t j = i;
        while (j < count) {
          const bool space = is_space(run.glyphs[j]);
          ++j;
          if (space) {
            break;
          }
        }
        // Marks with a vertical offset get their own Td.
        size_t k = i;
        while (k < j) {
          size_t m = k;
          const bool lifted = run.glyphs[m].y_offset != 0;
          if (lifted) {
            m = k + 1;
          } else {
            while (m < j && run.glyphs[m].y_offset == 0) {
              ++m;
            }
          }
          // Pen to the start of this piece, relative to the previous Td.
          const float piece_x = x + (lifted ? run.glyphs[k].x_offset : 0);
          const float piece_y =
              baseline + (lifted ? run.glyphs[k].y_offset : 0);
          RichNum(s, piece_x - origin.x) << " ";
          RichNum(s, piece_y - origin.y) << " Td\n";
          origin = CFX_PointF(piece_x, piece_y);

          // Glyph string with TJ adjustments where the shaped advance or a
          // horizontal offset departs from the font's width.
          std::vector<std::pair<float, ByteString>>
              pieces;  // adj before, string
          float pending = 0;
          ByteString bytes;
          const float unit = 1000.0f / (size * scale);
          for (size_t g = k; g < m; ++g) {
            const ShapedGlyph& glyph = run.glyphs[g];
            const float x_off = lifted ? 0 : glyph.x_offset;
            if (x_off != 0) {
              if (!bytes.IsEmpty()) {
                pieces.emplace_back(pending, bytes);
                bytes.clear();
                pending = 0;
              }
              pending += -x_off * unit;
            }
            if (simple) {
              bytes += static_cast<char>(codes[g] & 0xFF);
            } else {
              bytes += static_cast<char>(codes[g] >> 8);
              bytes += static_cast<char>(codes[g] & 0xFF);
            }
            const float delta =
                glyph.x_advance - nominal_advance(glyph, codes[g]);
            const float after = (x_off - delta) * unit;
            if (std::fabs(after) > 0.5f) {
              pieces.emplace_back(pending, bytes);
              bytes.clear();
              pending = after;
            }
          }
          if (!bytes.IsEmpty() || pending != 0) {
            pieces.emplace_back(pending, bytes);
          }
          const bool needs_tj =
              pieces.size() > 1 ||
              (!pieces.empty() && std::fabs(pieces.front().first) > 0.5f);
          auto encode = [&](const ByteString& raw) {
            return simple ? PDF_EncodeString(raw.AsStringView())
                          : PDF_HexEncodeString(raw.AsStringView());
          };
          if (!needs_tj) {
            if (!pieces.empty() && !pieces.front().second.IsEmpty()) {
              s << encode(pieces.front().second) << " Tj\n";
            }
          } else {
            s << "[";
            for (const auto& [adjust, raw] : pieces) {
              if (std::fabs(adjust) > 0.5f) {
                RichNum(s, adjust) << " ";
              }
              if (!raw.IsEmpty()) {
                s << encode(raw) << " ";
              }
            }
            s << "] TJ\n";
          }
          for (size_t g = k; g < m; ++g) {
            x += run.glyphs[g].x_advance;
          }
          k = m;
        }
        i = j;
      }
      if (actual_text) {
        s << "EMC\n";
      }

      // Decorations (plan §4.5): underline 0.44 · descent below the
      // baseline, 0.16 · descent thick; line-through 0.39 · ascent above,
      // 0.04 · ascent thick; `word` skips the spaces.
      const uint8_t decoration = run.style.decoration;
      if (decoration) {
        const float run_baseline = baseline + run.rise;
        const float descent = face.descent * size;
        const float ascent = face.ascent * size;
        if (decoration & CPDF_RichTextStyle::kUnderline) {
          decorations.push_back({run_left, run_left + run.width,
                                 run_baseline - 0.44f * descent,
                                 0.16f * descent, run.style.color});
        }
        if (decoration & CPDF_RichTextStyle::kWordUnderline) {
          float wx = run_left;
          float word_start = run_left;
          bool in_word = false;
          for (const ShapedGlyph& glyph : run.glyphs) {
            if (is_space(glyph)) {
              if (in_word) {
                decorations.push_back({word_start, wx,
                                       run_baseline - 0.44f * descent,
                                       0.16f * descent, run.style.color});
                in_word = false;
              }
            } else if (!in_word) {
              word_start = wx;
              in_word = true;
            }
            wx += glyph.x_advance;
          }
          if (in_word) {
            decorations.push_back({word_start, wx,
                                   run_baseline - 0.44f * descent,
                                   0.16f * descent, run.style.color});
          }
        }
        if (decoration & CPDF_RichTextStyle::kLineThrough) {
          decorations.push_back({run_left, run_left + run.width,
                                 run_baseline + 0.39f * ascent, 0.04f * ascent,
                                 run.style.color});
        }
      }
    }
  }
  s << "ET\n";
  std::optional<FX_ARGB> stroke_color;
  for (const DecorationSegment& segment : decorations) {
    if (segment.x1 - segment.x0 <= 0) {
      continue;
    }
    if (!stroke_color.has_value() || *stroke_color != segment.color) {
      s << RichColorOperator(segment.color, /*fill=*/false);
      stroke_color = segment.color;
    }
    RichNum(s, segment.thickness) << " w\n";
    RichNum(s, segment.x0) << " ";
    RichNum(s, segment.y) << " m\n";
    RichNum(s, segment.x1) << " ";
    RichNum(s, segment.y) << " l S\n";
  }
}

struct RichFreeTextInputs {
  const CPDF_RichTextDocument* document = nullptr;
  CFX_Color da_color;
  RetainPtr<CPDF_Font> default_font;  // the /DA font as found in /DR
  ByteString font_name;               // its alias
  CFX_FontRegistry::FontId registered_font_id =
      CFX_FontRegistry::kInvalidFontId;
  bool persistent = true;
  bool author_default_appearance = false;  // writer: body face -> /DA
};

std::unique_ptr<CPDF_GenerateAP::PreparedRichFreeTextAP>
PrepareRichFreeTextAPInternal(CPDF_Document* doc,
                              const CPDF_Dictionary* annot_dict,
                              const ByteString& blend_name,
                              RichFreeTextInputs in) {
  if (!doc || !annot_dict || !in.document) {
    return nullptr;
  }
  auto prepared = std::make_unique<CPDF_GenerateAP::PreparedRichFreeTextAP>();
  prepared->fonts = std::make_unique<CPDF_AnnotFontMap>(
      doc, std::move(in.default_font), in.font_name,
      /*allow_registered_fallbacks=*/true, in.registered_font_id,
      /*install_dr_entry=*/in.persistent,
      CPDF_AnnotFontMap::Owner::kAnnotation);
  CPDF_AnnotFontMap& map = *prepared->fonts;
  if (in.author_default_appearance) {
    const CPDF_RichTextStyle& body = in.document->body;
    const int body_entry =
        map.ResolveRichFace(body.family, body.weight, body.italic);
    if (body_entry < 0) {
      return nullptr;
    }
    prepared->da_alias = map.ChooseDefaultAppearanceAlias(body_entry);
    if (prepared->da_alias.IsEmpty()) {
      return nullptr;
    }
    map.SetDefaultAppearanceEntry(body_entry, prepared->da_alias);
  } else if (in.document->source == CPDF_RichTextDocument::Source::kContents &&
             map.HasDefaultFont() && !map.DefaultFontIsStandard()) {
    // Regenerating a PLAIN box: the parser derived the body face from the
    // /DA font, so that entry IS the body face and the layout keeps naming
    // the /DA alias — the contract every reader of a plain box, and CPVT
    // before us, relied on. A standard-14 /DA font resolves by family to
    // the Acrobat-parity face instead; an /RC body names its own family.
    const CPDF_RichTextStyle& body = in.document->body;
    map.PinRichFace(body.family, body.weight, body.italic,
                    map.GetDefaultAppearanceEntry() >= 0
                        ? map.GetDefaultAppearanceEntry()
                        : 0);
  }

  fxcrt::ostringstream stream;
  stream << "/" << kGSDictName << " gs ";

  const ByteString intent = annot_dict->GetNameFor("IT");
  RetainPtr<const CPDF_Array> cl = annot_dict->GetArrayFor("CL");
  const bool is_callout =
      intent == "FreeTextCallout" && cl && (cl->size() == 4 || cl->size() == 6);
  CFX_FloatRect box;
  CFX_FloatRect text_area;
  bool inline_rotation = false;
  if (is_callout) {
    const CalloutEnvelope envelope =
        AppendCalloutEnvelope(stream, annot_dict, cl.Get(), in.da_color);
    box = envelope.text_box;
    text_area = FreeTextPlate(box, envelope.border_w);
    inline_rotation = envelope.box_rot.is_rotated;
  } else {
    const BorderStyleInfo border_style_info =
        GetBorderStyleInfo(annot_dict->GetDictFor("BS"));
    const ShapeRotationInfo rot_info = GetShapeRotationInfo(annot_dict);
    const CFX_FloatRect rect = rot_info.bbox;
    const float half_border_width = border_style_info.width / 2.0f;
    CFX_FloatRect background_rect = rect;
    background_rect.Deflate(half_border_width, half_border_width);
    auto color_array = annot_dict->GetArrayFor(pdfium::annotation::kC);
    if (color_array) {
      CFX_Color color = fpdfdoc::CFXColorFromArray(*color_array);
      stream << "q\n" << GenerateColorAP(color, PaintOperation::kFill);
      WriteRect(stream, background_rect) << " re f\nQ\n";
    }
    const ByteString border_stream =
        GenerateBorderAP(rect, border_style_info, in.da_color);
    if (border_stream.GetLength() > 0) {
      stream << "q\n" << border_stream << "Q\n";
    }
    box = rect;
    text_area = FreeTextPlate(box, border_style_info.width);
    prepared->use_transform = rot_info.is_rotated;
    prepared->matrix = rot_info.matrix;
    prepared->bbox = rot_info.bbox;
  }
  // An empty BOX is invalid input (nothing to draw at all); a valid box whose
  // border swallowed its plate still paints its envelope above — the text
  // has nowhere to go and lays out nothing. A /Rect authored upside down
  // (top below bottom) is a normal box, as it always was for CPVT.
  box.Normalize();
  if (box.Width() <= 0 || box.Height() <= 0) {
    return nullptr;
  }
  if (text_area.Width() > 0 && text_area.Height() > 0) {
    CPDF_RichTextLayout::Options options;
    options.typographic_features = doc->GetTypographicFeaturesEnabled();
    CPDF_RichTextLayout layout(&map, options);
    const CPDF_RichTextLayout::Result result =
        layout.Arrange(*in.document, text_area, GetVerticalAlign(annot_dict));
    prepared->body_size = result.body_size;
    prepared->degraded = result.degraded;
    prepared->auto_size_fell_back = result.auto_size_fell_back;

    stream << "/Tx BMC\nq\n";
    WriteRect(stream, text_area) << " re W n\n";
    EmitRichTextBody(stream, result, map, text_area);
    stream << "Q\nEMC\n";
  }
  if (inline_rotation) {
    stream << "Q\n";  // closes the inline box rotation of the callout
  }

  prepared->resources = map.PrepareFontResources();
  if (!prepared->resources.has_value()) {
    return nullptr;
  }
  prepared->graphics_state = GenerateExtGStateDict(*annot_dict, blend_name);
  prepared->content = ByteString(stream);
  return prepared;
}

bool PublishRichFreeTextAPToTarget(
    APGenerationTarget* target,
    const CPDF_Dictionary* annot_dict,
    std::unique_ptr<CPDF_GenerateAP::PreparedRichFreeTextAP> prepared) {
  if (!prepared || !prepared->fonts || !prepared->resources.has_value()) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> font_dict =
      prepared->fonts->PublishFontResources(std::move(*prepared->resources));
  if (!font_dict) {
    return false;
  }
  auto resources_dict = GenerateResourcesDict(
      target->doc, std::move(prepared->graphics_state), std::move(font_dict));
  fxcrt::ostringstream stream;
  stream.write(prepared->content.c_str(), prepared->content.GetLength());
  if (prepared->use_transform) {
    GenerateAndSetAPDictWithTransform(target, annot_dict, &stream,
                                      std::move(resources_dict),
                                      prepared->matrix, prepared->bbox);
  } else {
    GenerateAndSetAPDict(target, annot_dict, &stream, std::move(resources_dict),
                         /*is_text_markup_annotation=*/false);
  }
  return true;
}

// The generator's rich path: the annotation's own /RC (or /Contents) laid
// out by the rich engine; Prepare and Publish back to back.
bool GenerateRichFreeTextAP(APGenerationTarget* target,
                            const CPDF_Dictionary* annot_dict,
                            const ByteString& blend_name,
                            const DaFontResolution& da_font,
                            RetainPtr<CPDF_Font> default_font,
                            const ByteString& font_name,
                            const DefaultAppearanceInfo& da_info) {
  CPDF_Document* doc = target->doc;
  const CPDF_Dictionary* root = doc->GetRoot();
  RetainPtr<const CPDF_Dictionary> acroform =
      root ? root->GetDictFor("AcroForm") : nullptr;
  const CPDF_RichTextDocument document =
      CPDF_RichTextParser::FromAnnotation(annot_dict, acroform.Get(), doc);
  RichFreeTextInputs in;
  in.document = &document;
  in.da_color = da_info.text_color;
  in.default_font = std::move(default_font);
  in.font_name = font_name;
  in.registered_font_id = da_font.registered_font_id;
  in.persistent = target->IsPersistent();
  std::unique_ptr<CPDF_GenerateAP::PreparedRichFreeTextAP> prepared =
      PrepareRichFreeTextAPInternal(doc, annot_dict, blend_name, std::move(in));
  if (!prepared) {
    return false;
  }
  return PublishRichFreeTextAPToTarget(target, annot_dict, std::move(prepared));
}

bool GenerateFreeTextAP(APGenerationTarget* target,
                        CPDF_Dictionary* annot_dict,
                        const ByteString& blend_name) {
  CPDF_Document* const doc = target->doc;
  const CPDF_Dictionary* root_dict = doc->GetRoot();
  if (!root_dict) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> form_dict =
      root_dict->GetDictFor("AcroForm");
  RetainPtr<CPDF_Dictionary> ephemeral_form_dict;
  if (!form_dict) {
    if (!target->IsPersistent()) {
      ephemeral_form_dict = GenerateEphemeralDefaultAcroFormDict();
      form_dict = ephemeral_form_dict;
    } else {
      form_dict = CPDF_InteractiveForm::InitAcroFormDict(doc);
      CHECK(form_dict);
    }
  }

  std::optional<DefaultAppearanceInfo> default_appearance_info =
      GetDefaultAppearanceInfo(annot_dict, form_dict.Get());
  if (!default_appearance_info.has_value()) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> dr_dict = form_dict->GetDictFor("DR");
  if (!dr_dict) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> dr_font_dict = dr_dict->GetDictFor("Font");
  if (!ValidateFontResourceDict(dr_font_dict.Get())) {
    return false;
  }

  const ByteString& font_name = default_appearance_info.value().font_name;
  const DaFontResolution da_font =
      target->IsPersistent()
          ? ResolveDaFontForPersistentTarget(
                doc, const_cast<CPDF_Dictionary*>(dr_font_dict.Get()),
                font_name)
          : ResolveDaFontForEphemeralTarget(doc, dr_font_dict.Get(), font_name);
  auto* doc_page_data = CPDF_DocPageData::FromDocument(doc);
  RetainPtr<CPDF_Font> default_font =
      da_font.font_dict ? doc_page_data->GetFont(da_font.font_dict) : nullptr;
  if (!default_font &&
      da_font.registered_font_id == CFX_FontRegistry::kInvalidFontId) {
    return false;
  }

  // EmbedPDF (Phase D, D4): every FreeText lays out through
  // CPDF_RichTextLayout — a box with /RC from its rich document, a plain box
  // from the document the parser synthesises out of /Contents, /DA and /Q.
  // One engine, one line model, one plate; the CPVT path is gone.
  return GenerateRichFreeTextAP(target, annot_dict, blend_name, da_font,
                                std::move(default_font), font_name,
                                default_appearance_info.value());
}

bool GenerateHighlightAP(APGenerationTarget* target,
                         CPDF_Dictionary* annot_dict,
                         const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 1, 1, 0), PaintOperation::kFill);

  RetainPtr<const CPDF_Array> quad_points_array =
      annot_dict->GetArrayFor("QuadPoints");
  if (quad_points_array) {
    const size_t quad_point_count =
        CPDF_Annot::QuadPointCount(quad_points_array.Get());
    for (size_t i = 0; i < quad_point_count; ++i) {
      const std::optional<MarkupQuad> quad =
          ReadMarkupQuad(quad_points_array.Get(), i);
      if (quad && !IsAxisAlignedZigZag(*quad)) {
        WritePoint(app_stream, quad->upper_start) << " m ";
        WritePoint(app_stream, quad->upper_end) << " l ";
        WritePoint(app_stream, quad->lower_end) << " l ";
        WritePoint(app_stream, quad->lower_start) << " l h f\n";
        continue;
      }

      CFX_FloatRect rect = CPDF_Annot::RectFromQuadPoints(annot_dict, i);
      rect.Normalize();

      app_stream << rect.left << " " << rect.top << " m " << rect.right << " "
                 << rect.top << " l " << rect.right << " " << rect.bottom
                 << " l " << rect.left << " " << rect.bottom << " l h f\n";
    }
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &app_stream,
                       std::move(resources_dict),
                       true /*IsTextMarkupAnnotation*/);

  return true;
}

// EmbedPDF: measurement captions are plain /Contents. Neither the scale nor
// /RC participates in deriving or painting their value here.
struct DimensionCaption {
  ByteString text_ops;
  RetainPtr<CPDF_Dictionary> fonts;
  float width = 0;
  float height = 0;
};

std::optional<DimensionCaption> PrepareDimensionCaption(
    APGenerationTarget* target,
    const CPDF_Dictionary* annot) {
  DimensionCaption result;
  const WideString text = annot->GetUnicodeTextFor("Contents");
  if (text.IsEmpty()) {
    return result;
  }
  CPDF_DefaultAppearance da(annot->GetByteStringFor("DA"));
  auto font_info = da.GetFont();
  const ByteString alias = font_info ? font_info->name : ByteString("Helv");
  float size = font_info ? font_info->size : 9.0f;
  if (!std::isfinite(size) || size <= 0) {
    size = 9;
  }
  // Resolve from the existing AP first (Acrobat keeps its caption fonts here),
  // then /DR. Both are read-only; captions never create an AcroForm.
  auto ap = annot->GetDictFor("AP");
  auto normal = ap ? ap->GetStreamFor("N") : nullptr;
  auto resources =
      normal ? normal->GetDict()->GetDictFor("Resources") : nullptr;
  auto font_dict = resources ? resources->GetDictFor("Font") : nullptr;
  if (!font_dict || !font_dict->KeyExist(alias.AsStringView())) {
    const auto* root = target->doc->GetRoot();
    auto form = root ? root->GetDictFor("AcroForm") : nullptr;
    auto dr = form ? form->GetDictFor("DR") : nullptr;
    font_dict = dr ? dr->GetDictFor("Font") : nullptr;
  }
  const DaFontResolution resolved =
      ResolveDaFontForEphemeralTarget(target->doc, font_dict.Get(), alias);
  auto* page_data = CPDF_DocPageData::FromDocument(target->doc);
  auto font =
      resolved.font_dict ? page_data->GetFont(resolved.font_dict) : nullptr;
  auto owned_fonts = std::make_unique<CPDF_AnnotFontMap>(
      target->doc, std::move(font), alias,
      /*allow_registered_fallbacks=*/true, resolved.registered_font_id,
      /*install_dr_entry=*/false);
  CPDF_AnnotFontMap& fonts = *owned_fonts;
  if (!fonts.HasDefaultFont()) {
    return std::nullopt;
  }
  CPDF_RichTextDocument document;
  document.source = CPDF_RichTextDocument::Source::kContents;
  document.body.family = L"Helvetica";
  document.body.size = size;
  document.body.color =
      da.GetColorARGB().has_value() ? da.GetColorARGB()->argb : 0xFF000000;
  // Keep the actual /DA font/encoding, including Acrobat's Differences table.
  fonts.PinRichFace(document.body.family, 400, false, 0);
  CPDF_RichTextParagraph paragraph;
  paragraph.runs.push_back({text, {}});
  // A line caption's box is one font size high, matching the live dimension
  // layout. Shape captions keep their existing rich-text line metrics.
  if (annot->GetNameFor("Subtype") == "Line") {
    paragraph.props.line_height = size;
  }
  document.paragraphs.push_back(std::move(paragraph));
  CPDF_RichTextLayout layout(&fonts, {});
  auto arranged = layout.Arrange(document, {0, 0, 1000000, 1000000},
                                 CPDF_Annot::VerticalAlignment::kTop);
  for (const auto& line : arranged.lines) {
    result.width = std::max(result.width, line.width);
  }
  result.height = arranged.content_height;
  if (!std::isfinite(result.width) || !std::isfinite(result.height)) {
    return std::nullopt;
  }
  fxcrt::ostringstream text_stream;
  EmitRichTextBody(text_stream, arranged, fonts,
                   {-result.width / 2, -result.height / 2, result.width / 2,
                    result.height / 2});
  result.text_ops = ByteString(text_stream);
  if (target->IsPersistent()) {
    result.fonts = fonts.CreateFontResourceDict();
  } else {
    result.fonts = fonts.CreateEphemeralFontResourceDict();
    target->ephemeral_fonts = std::move(owned_fonts);
  }
  if (!result.fonts) {
    return std::nullopt;
  }
  return result;
}

void AppendDimensionCaption(fxcrt::ostringstream& ap,
                            const DimensionCaption& text,
                            const pdfium::dimension::CaptionLayout& layout) {
  if (text.text_ops.IsEmpty()) {
    return;
  }
  ap << "q ";
  WriteMatrix(ap, layout.matrix) << " cm\n";
  ap << text.text_ops << "Q\n";
}

bool ShapeCaptionEnabled(const CPDF_Dictionary* annot) {
  auto metadata = annot->GetDictFor("EMBD_Metadata");
  return metadata && metadata->GetBooleanFor("MeasurementCaption", false);
}

bool FinishShapeDimensionAP(APGenerationTarget* target,
                            CPDF_Dictionary* annot,
                            const ByteString& blend_name,
                            pdfium::span<const CFX_PointF> points,
                            bool closed,
                            fxcrt::ostringstream* ap) {
  auto caption = PrepareDimensionCaption(target, annot);
  if (!caption) {
    return false;
  }
  auto center = pdfium::dimension::ShapeCaptionCenter(annot, points, closed,
                                                      caption->height);
  auto layout = pdfium::dimension::LayoutShapeCaption(
      annot, center, caption->width, caption->height);
  AppendDimensionCaption(*ap, *caption, layout);
  // Start from the current path, never the previous /Rect: moving a caption
  // inward must shrink the appearance frame again. Include the PDF default
  // miter limit and, for open paths, the supported line-ending envelope.
  CFX_FloatRect bounds(points.front().x, points.front().y, points.front().x,
                       points.front().y);
  for (const auto& point : points) {
    bounds.UpdateRect(point);
  }
  const float stroke = std::max(0.0f, GetBorderWidth(annot));
  float padding = 5 * stroke;
  const auto cloudy = GetCloudyBorderInfo(annot);
  if (closed && cloudy.is_cloudy) {
    padding = 4 * cloudy.intensity + stroke;
  } else if (!closed) {
    auto endings = annot->GetArrayFor("LE");
    if (endings && (endings->GetByteStringAt(0) != "None" ||
                    endings->GetByteStringAt(1) != "None")) {
      padding = 8 * stroke;
    }
  }
  bounds.Inflate(padding, padding);
  if (!caption->text_ops.IsEmpty()) {
    layout.bounds.Inflate(1, 1);
    bounds.Union(layout.bounds);
  }
  if (target->IsPersistent()) {
    annot->SetRectFor("Rect", bounds);
  }
  auto resources = GenerateResourcesDict(
      target->doc, GenerateExtGStateDict(*annot, blend_name),
      std::move(caption->fonts));
  return GenerateAndSetAPDictWithBBox(target, annot, ap, std::move(resources),
                                      bounds);
}

bool GenerateDimensionLineAP(APGenerationTarget* target,
                             CPDF_Dictionary* annot,
                             const ByteString& blend_name,
                             const std::vector<CFX_PointF>& points) {
  const float border = GetBorderWidth(annot);
  const float ll = annot->GetFloatFor("LL");
  const float lle = annot->GetFloatFor("LLE");
  const float llo = annot->GetFloatFor("LLO");
  if (!IsFinitePoint(points[0]) || !IsFinitePoint(points[1]) ||
      !std::isfinite(ll) || !std::isfinite(lle) || !std::isfinite(llo) ||
      lle < 0 || llo < 0) {
    return false;
  }
  auto line =
      pdfium::dimension::LayoutLine(points[0], points[1], ll, lle, llo, border);
  DimensionCaption caption;
  pdfium::dimension::CaptionLayout label;
  if (annot->GetBooleanFor("Cap", false)) {
    auto prepared = PrepareDimensionCaption(target, annot);
    if (!prepared) {
      return false;
    }
    caption = std::move(*prepared);
    if (!caption.text_ops.IsEmpty()) {
      label = pdfium::dimension::LayoutLineCaption(annot, line, caption.width,
                                                   caption.height, border);
    }
  }
  fxcrt::ostringstream ap;
  ap << "/" << kGSDictName << " gs\n";
  auto color = annot->GetArrayFor("C");
  auto interior = annot->GetArrayFor("IC");
  ap << GetColorStringWithDefault(color.Get(),
                                  CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0),
                                  PaintOperation::kStroke);
  if (interior && !interior->IsEmpty()) {
    ap << GetColorStringWithDefault(interior.Get(), {}, PaintOperation::kFill);
  }
  auto segment = [&](CFX_PointF from, CFX_PointF to) {
    WritePoint(ap, from) << " m ";
    WritePoint(ap, to) << " l S\n";
  };
  if (border > 0) {
    WriteFloat(ap, border) << " w " << GetDashPatternString(annot);
    if (label.outside_arrows) {
      const float stub = 20 * border;
      segment(line.start - stub * line.along, line.start);
      segment(line.end, line.end + stub * line.along);
    } else if (label.gap_end > label.gap_start) {
      if (label.gap_start > 0) {
        segment(line.start, line.start + label.gap_start * line.along);
      }
      if (label.gap_end < line.length) {
        segment(line.start + label.gap_end * line.along, line.end);
      }
    } else {
      segment(line.start, line.end);
    }
    for (const auto& leader : line.leaders) {
      segment(leader.from, leader.to);
    }
    for (const auto& connector : label.connector) {
      segment(connector.from, connector.to);
    }
    GenerateLineEndings(ap, {line.start, line.end}, annot,
                        label.outside_arrows);
  }
  AppendDimensionCaption(ap, caption, label);
  CFX_FloatRect bounds = line.bounds;
  if (label.outside_arrows) {
    const CFX_PointF start = line.start - 20 * border * line.along;
    const CFX_PointF end = line.end + 20 * border * line.along;
    CFX_FloatRect stubs(start.x, start.y, end.x, end.y);
    stubs.Normalize();
    stubs.Inflate(border / 2, border / 2);
    bounds.Union(stubs);
  }
  if (!caption.text_ops.IsEmpty()) {
    bounds.Union(label.bounds);
  }
  bounds.Inflate(1, 1);
  // A regenerated distance owns its full appearance. Derive a fresh rectangle
  // so moving a caption or leader back inward also shrinks the saved bounds.
  if (target->IsPersistent()) {
    annot->SetRectFor("Rect", bounds);
  }
  auto resources = GenerateResourcesDict(
      target->doc, GenerateExtGStateDict(*annot, blend_name),
      std::move(caption.fonts));
  return GenerateAndSetAPDictWithBBox(target, annot, &ap, std::move(resources),
                                      bounds);
}

bool GeneratePolygonAP(APGenerationTarget* target,
                       CPDF_Dictionary* annot_dict,
                       const ByteString& blend_name) {
  RetainPtr<const CPDF_Array> verts =
      annot_dict->GetArrayFor(pdfium::annotation::kVertices);
  // A polygon needs ≥ 3 points  (= 6 floats).
  if (!verts || verts->size() < 6) {
    return false;
  }

  fxcrt::ostringstream app;
  app << "/" << kGSDictName << " gs ";

  RetainPtr<const CPDF_Array> interior_color = annot_dict->GetArrayFor("IC");
  app << GetColorStringWithDefault(interior_color.Get(),
                                   CFX_Color(CFX_Color::Type::kTransparent),
                                   PaintOperation::kFill);

  app << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0), PaintOperation::kStroke);

  const float border_w = GetBorderWidth(annot_dict);
  const bool do_stroke = border_w > 0;
  CloudyBorderInfo cloudy_info = GetCloudyBorderInfo(annot_dict);

  if (do_stroke) {
    app << border_w << " w ";
    if (cloudy_info.is_cloudy) {
      app << "1 j ";
    } else {
      app << GetDashPatternString(annot_dict);
    }
  }

  if (cloudy_info.is_cloudy) {
    std::vector<CFX_PointF> points;
    for (size_t i = 0; i + 1 < verts->size(); i += 2) {
      points.push_back({verts->GetFloatAt(i), verts->GetFloatAt(i + 1)});
    }
    GenerateCloudyPolygonPath(app, points, cloudy_info.intensity, border_w);
  } else {
    app << verts->GetFloatAt(0) << " " << verts->GetFloatAt(1) << " m ";
    for (size_t i = 2; i + 1 < verts->size(); i += 2) {
      app << verts->GetFloatAt(i) << " " << verts->GetFloatAt(i + 1) << " l ";
    }
    app << "h ";
  }

  const bool do_fill = interior_color && !interior_color->IsEmpty();
  app << GetPaintOperatorString(do_stroke, do_fill) << "\n";

  if (ShapeCaptionEnabled(annot_dict)) {
    std::vector<CFX_PointF> points;
    for (size_t i = 0; i + 1 < verts->size(); i += 2) {
      CFX_PointF point(verts->GetFloatAt(i), verts->GetFloatAt(i + 1));
      if (!IsFinitePoint(point)) {
        return false;
      }
      points.push_back(point);
    }
    return FinishShapeDimensionAP(target, annot_dict, blend_name, points, true,
                                  &app);
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto res_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &app, std::move(res_dict),
                       /*is_text_markup=*/false);
  return true;
}

bool GenerateLineAP(APGenerationTarget* target,
                    CPDF_Dictionary* annot_dict,
                    const ByteString& blend_name) {
  RetainPtr<const CPDF_Array> L =
      annot_dict->GetArrayFor(pdfium::annotation::kL);
  if (!L || L->size() < 4) {
    return false;
  }

  std::vector<CFX_PointF> points;
  points.push_back({L->GetFloatAt(0), L->GetFloatAt(1)});
  points.push_back({L->GetFloatAt(2), L->GetFloatAt(3)});

  if (annot_dict->GetBooleanFor("Cap", false) || annot_dict->KeyExist("LL") ||
      annot_dict->KeyExist("LLE") || annot_dict->KeyExist("LLO")) {
    return GenerateDimensionLineAP(target, annot_dict, blend_name, points);
  }

  fxcrt::ostringstream ap;
  ap << "/" << kGSDictName << " gs\n";

  // Set colors and border styles.
  RetainPtr<const CPDF_Array> interior_color = annot_dict->GetArrayFor("IC");
  if (interior_color && !interior_color->IsEmpty()) {
    ap << GetColorStringWithDefault(interior_color.Get(), {},
                                    PaintOperation::kFill);
  }
  ap << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0), PaintOperation::kStroke);

  const float border_w = GetBorderWidth(annot_dict);
  if (border_w > 0) {
    ap << border_w << " w " << GetDashPatternString(annot_dict);
  }

  // Draw the main line segment.
  ap << points[0].x << " " << points[0].y << " m " << points[1].x << " "
     << points[1].y << " l S\n";

  // Draw the endings.
  GenerateLineEndings(ap, points, annot_dict);

  // Finalize and set the Appearance Stream.
  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto res_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &ap, std::move(res_dict),
                       /*is_text_markup=*/false);
  return true;
}

bool GeneratePolyLineAP(APGenerationTarget* target,
                        CPDF_Dictionary* annot_dict,
                        const ByteString& blend_name) {
  RetainPtr<const CPDF_Array> verts =
      annot_dict->GetArrayFor(pdfium::annotation::kVertices);
  if (!verts || verts->size() < 4) {
    return false;
  }

  std::vector<CFX_PointF> points;
  for (size_t i = 0; i + 1 < verts->size(); i += 2) {
    points.push_back({verts->GetFloatAt(i), verts->GetFloatAt(i + 1)});
  }

  fxcrt::ostringstream ap;
  ap << "/" << kGSDictName << " gs\n";

  // Set colors and border styles.
  RetainPtr<const CPDF_Array> interior_color = annot_dict->GetArrayFor("IC");
  if (interior_color && !interior_color->IsEmpty()) {
    ap << GetColorStringWithDefault(interior_color.Get(), {},
                                    PaintOperation::kFill);
  }
  ap << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0), PaintOperation::kStroke);

  const float border_w = GetBorderWidth(annot_dict);
  if (border_w > 0) {
    ap << border_w << " w " << GetDashPatternString(annot_dict);
  }

  // Draw the main polyline path.
  ap << points[0].x << " " << points[0].y << " m ";
  for (size_t i = 1; i < points.size(); ++i) {
    ap << points[i].x << " " << points[i].y << " l ";
  }
  ap << "S\n";

  // Draw the endings.
  GenerateLineEndings(ap, points, annot_dict);

  // Finalize and set the Appearance Stream.
  if (ShapeCaptionEnabled(annot_dict)) {
    for (const auto& point : points) {
      if (!IsFinitePoint(point)) {
        return false;
      }
    }
    return FinishShapeDimensionAP(target, annot_dict, blend_name, points, false,
                                  &ap);
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto res_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &ap, std::move(res_dict),
                       /*is_text_markup=*/false);
  return true;
}

bool GenerateInkAP(APGenerationTarget* target,
                   CPDF_Dictionary* annot_dict,
                   const ByteString& blend_name) {
  RetainPtr<const CPDF_Array> ink_list = annot_dict->GetArrayFor("InkList");
  if (!ink_list || ink_list->IsEmpty()) {
    return false;
  }

  float border_width = GetBorderWidth(annot_dict);
  const bool is_stroke = border_width > 0;
  if (!is_stroke) {
    return false;
  }

  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";
  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0), PaintOperation::kStroke);

  app_stream << border_width << " w ";

  app_stream << "1 J ";  // Rounded Line Caps
  app_stream << "1 j ";  // Rounded Line Joins (for the corners in the middle)

  app_stream << GetDashPatternString(annot_dict);

  // Track the stroked ink's true bounds while writing the path: the union of
  // every /InkList point, inflated by half the border width below (the round
  // caps/joins set above — `1 J 1 j` — extend exactly border_width / 2 past a
  // point). This is what the appearance actually PAINTS, independent of what
  // /Rect currently claims.
  CFX_FloatRect ink_bounds;
  bool has_ink_point = false;

  for (size_t i = 0; i < ink_list->size(); i++) {
    RetainPtr<const CPDF_Array> coordinates_array = ink_list->GetArrayAt(i);
    // An ink stroke needs at least one point to start.
    if (!coordinates_array || coordinates_array->size() < 2) {
      continue;
    }

    const float x0 = coordinates_array->GetFloatAt(0);
    const float y0 = coordinates_array->GetFloatAt(1);
    app_stream << x0 << " " << y0 << " m ";
    if (has_ink_point) {
      ink_bounds.UpdateRect(CFX_PointF(x0, y0));
    } else {
      ink_bounds = CFX_FloatRect(x0, y0, x0, y0);
      has_ink_point = true;
    }

    // Start loop at the second point (index 2) ---
    // The 'm' command already moves to the first point.
    for (size_t j = 2; j < coordinates_array->size(); j += 2) {
      const float x = coordinates_array->GetFloatAt(j);
      const float y = coordinates_array->GetFloatAt(j + 1);
      app_stream << x << " " << y << " l ";
      ink_bounds.UpdateRect(CFX_PointF(x, y));
    }

    app_stream << "S\n";
  }

  // ENSURE-FIT, never blind-inflate. The caller owns /Rect (EmbedPDF's
  // writers author it as the stroked visual bounds already); grow it only
  // when the painted ink would actually be clipped, by the minimal union —
  // so regeneration is IDEMPOTENT. Upstream PDFium instead inflated /Rect by
  // border_width / 2 unconditionally on every call: harmless on its one-shot
  // "synthesize a missing /AP at load" path, but unbounded growth once the
  // appearance is re-baked after each edit.
  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  rect.Normalize();
  if (has_ink_point) {
    ink_bounds.Inflate(border_width / 2, border_width / 2);
    if (!rect.Contains(ink_bounds)) {
      rect.Union(ink_bounds);
      if (target->IsPersistent()) {
        annot_dict->SetRectFor(pdfium::annotation::kRect, rect);
      }
    }
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  if (target->IsPersistent()) {
    GenerateAndSetAPDict(target, annot_dict, &app_stream,
                         std::move(resources_dict),
                         false /*IsTextMarkupAnnotation*/);
  } else {
    GenerateAndSetAPDictWithBBox(target, annot_dict, &app_stream,
                                 std::move(resources_dict), rect);
  }
  return true;
}

bool GenerateTextAP(CPDF_Document* doc,
                    CPDF_Dictionary* annot_dict,
                    const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  const float note_length = 20;
  CFX_FloatRect note_rect(rect.left, rect.bottom, rect.left + note_length,
                          rect.bottom + note_length);
  annot_dict->SetRectFor(pdfium::annotation::kRect, note_rect);

  app_stream << GenerateTextSymbolAP(note_rect, *annot_dict);

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict = GenerateResourcesDict(doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(doc, annot_dict, &app_stream, std::move(resources_dict),
                       false /*IsTextMarkupAnnotation*/);
  return true;
}

bool GenerateFileAttachmentAP(CPDF_Document* doc,
                              CPDF_Dictionary* annot_dict,
                              const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  // Like the note icon, a file attachment renders at a fixed icon size
  // anchored at the /Rect's bottom-left corner.
  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  const float icon_length = 20;
  CFX_FloatRect icon_rect(rect.left, rect.bottom, rect.left + icon_length,
                          rect.bottom + icon_length);
  annot_dict->SetRectFor(pdfium::annotation::kRect, icon_rect);

  app_stream << GenerateFileAttachmentSymbolAP(icon_rect, *annot_dict);

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict = GenerateResourcesDict(doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(doc, annot_dict, &app_stream, std::move(resources_dict),
                       false /*IsTextMarkupAnnotation*/);
  return true;
}

bool GenerateUnderlineAP(APGenerationTarget* target,
                         CPDF_Dictionary* annot_dict,
                         const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0), PaintOperation::kStroke);

  RetainPtr<const CPDF_Array> quad_points_array =
      annot_dict->GetArrayFor("QuadPoints");
  if (quad_points_array) {
    static constexpr int kLineWidth = 1;
    app_stream << kLineWidth << " w ";
    const size_t quad_point_count =
        CPDF_Annot::QuadPointCount(quad_points_array.Get());
    for (size_t i = 0; i < quad_point_count; ++i) {
      const std::optional<MarkupQuad> quad =
          ReadMarkupQuad(quad_points_array.Get(), i);
      if (quad && !IsAxisAlignedZigZag(*quad)) {
        const CFX_PointF inset = LowerToUpperUnit(*quad);
        WritePoint(app_stream, quad->lower_start + kLineWidth * inset) << " m ";
        WritePoint(app_stream, quad->lower_end + kLineWidth * inset)
            << " l S\n";
        continue;
      }

      CFX_FloatRect rect = CPDF_Annot::RectFromQuadPoints(annot_dict, i);
      rect.Normalize();
      app_stream << rect.left << " " << rect.bottom + kLineWidth << " m "
                 << rect.right << " " << rect.bottom + kLineWidth << " l S\n";
    }
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &app_stream,
                       std::move(resources_dict),
                       true /*IsTextMarkupAnnotation*/);
  return true;
}

bool GeneratePopupAP(CPDF_Document* doc,
                     CPDF_Dictionary* annot_dict,
                     const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs\n";

  app_stream << GenerateColorAP(CFX_Color(CFX_Color::Type::kRGB, 1, 1, 0),
                                PaintOperation::kFill);
  app_stream << GenerateColorAP(CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0),
                                PaintOperation::kStroke);

  const float border_width = 1;
  app_stream << border_width << " w\n";

  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  rect.Normalize();
  rect.Deflate(border_width / 2, border_width / 2);

  app_stream << rect.left << " " << rect.bottom << " " << rect.Width() << " "
             << rect.Height() << " re b\n";

  RetainPtr<CPDF_Dictionary> font_dict = GenerateFallbackFontDict(doc);
  auto* doc_page_data = CPDF_DocPageData::FromDocument(doc);
  RetainPtr<CPDF_Font> default_font = doc_page_data->GetFont(font_dict);
  if (!default_font) {
    return false;
  }

  const ByteString font_name = "FONT";
  RetainPtr<CPDF_Dictionary> resource_font_dict =
      GenerateResourceFontDict(doc, font_name, font_dict->GetObjNum());
  RetainPtr<CPDF_Dictionary> gs_dict =
      GenerateExtGStateDict(*annot_dict, blend_name);
  RetainPtr<CPDF_Dictionary> resources_dict = GenerateResourcesDict(
      doc, std::move(gs_dict), std::move(resource_font_dict));

  app_stream << GetPopupContentsString(doc, *annot_dict,
                                       std::move(default_font), font_name);
  GenerateAndSetAPDict(doc, annot_dict, &app_stream, std::move(resources_dict),
                       false /*IsTextMarkupAnnotation*/);
  return true;
}

bool GenerateSquareAP(APGenerationTarget* target,
                      CPDF_Dictionary* annot_dict,
                      const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  RetainPtr<const CPDF_Array> interior_color = annot_dict->GetArrayFor("IC");
  app_stream << GetColorStringWithDefault(
      interior_color.Get(), CFX_Color(CFX_Color::Type::kTransparent),
      PaintOperation::kFill);

  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0), PaintOperation::kStroke);

  float border_width = GetBorderWidth(annot_dict);
  const bool is_stroke_rect = border_width > 0;
  CloudyBorderInfo cloudy_info = GetCloudyBorderInfo(annot_dict);

  if (is_stroke_rect) {
    app_stream << border_width << " w ";
    if (cloudy_info.is_cloudy) {
      app_stream << "1 j ";
    } else {
      app_stream << GetDashPatternString(annot_dict);
    }
  }

  const ShapeRotationInfo rot_info = GetShapeRotationInfo(annot_dict);
  CFX_FloatRect draw_rect = rot_info.bbox;
  draw_rect.Normalize();

  if (cloudy_info.is_cloudy) {
    CFX_FloatRect rd = GetRectDifferences(annot_dict);
    GenerateCloudyRectanglePath(app_stream, draw_rect, rd,
                                cloudy_info.intensity, border_width);
  } else {
    if (is_stroke_rect) {
      draw_rect.Deflate(border_width / 2, border_width / 2);
    }
    app_stream << draw_rect.left << " " << draw_rect.bottom << " "
               << draw_rect.Width() << " " << draw_rect.Height() << " re ";
  }

  const bool is_fill_rect = interior_color && (interior_color->size() > 0);
  app_stream << GetPaintOperatorString(is_stroke_rect, is_fill_rect) << "\n";

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);

  if (rot_info.is_rotated) {
    GenerateAndSetAPDictWithTransform(target, annot_dict, &app_stream,
                                      std::move(resources_dict),
                                      rot_info.matrix, rot_info.bbox);
  } else {
    GenerateAndSetAPDict(target, annot_dict, &app_stream,
                         std::move(resources_dict),
                         false /*IsTextMarkupAnnotation*/);
  }
  return true;
}

bool GenerateSquigglyAP(APGenerationTarget* target,
                        CPDF_Dictionary* annot_dict,
                        const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0), PaintOperation::kStroke);

  RetainPtr<const CPDF_Array> quad_points_array =
      annot_dict->GetArrayFor("QuadPoints");
  if (quad_points_array) {
    static constexpr int kLineWidth = 1;
    static constexpr int kDelta = 2;
    app_stream << kLineWidth << " w ";
    const size_t quad_point_count =
        CPDF_Annot::QuadPointCount(quad_points_array.Get());
    for (size_t i = 0; i < quad_point_count; ++i) {
      const std::optional<MarkupQuad> quad =
          ReadMarkupQuad(quad_points_array.Get(), i);
      if (quad && !IsAxisAlignedZigZag(*quad)) {
        EmitOrientedSquiggle(app_stream, *quad, kDelta);
        continue;
      }

      CFX_FloatRect rect = CPDF_Annot::RectFromQuadPoints(annot_dict, i);
      rect.Normalize();

      const float top = rect.bottom + kDelta;
      const float bottom = rect.bottom;
      app_stream << rect.left << " " << top << " m ";

      float x = rect.left + kDelta;
      bool isUpwards = false;
      while (x < rect.right) {
        app_stream << x << " " << (isUpwards ? top : bottom) << " l ";
        x += kDelta;
        isUpwards = !isUpwards;
      }

      float remainder = rect.right - (x - kDelta);
      if (isUpwards) {
        app_stream << rect.right << " " << bottom + remainder << " l ";
      } else {
        app_stream << rect.right << " " << top - remainder << " l ";
      }

      app_stream << "S\n";
    }
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &app_stream,
                       std::move(resources_dict),
                       true /*IsTextMarkupAnnotation*/);
  return true;
}

bool GenerateStrikeOutAP(APGenerationTarget* target,
                         CPDF_Dictionary* annot_dict,
                         const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0), PaintOperation::kStroke);

  RetainPtr<const CPDF_Array> quad_points_array =
      annot_dict->GetArrayFor("QuadPoints");
  if (quad_points_array) {
    const size_t quad_point_count =
        CPDF_Annot::QuadPointCount(quad_points_array.Get());
    for (size_t i = 0; i < quad_point_count; ++i) {
      const std::optional<MarkupQuad> quad =
          ReadMarkupQuad(quad_points_array.Get(), i);
      if (quad && !IsAxisAlignedZigZag(*quad)) {
        static constexpr int kLineWidth = 1;
        app_stream << kLineWidth << " w ";
        WritePoint(app_stream, Midpoint(quad->upper_start, quad->lower_start))
            << " m ";
        WritePoint(app_stream, Midpoint(quad->upper_end, quad->lower_end))
            << " l S\n";
        continue;
      }

      CFX_FloatRect rect = CPDF_Annot::RectFromQuadPoints(annot_dict, i);
      rect.Normalize();

      float y = (rect.top + rect.bottom) / 2;
      static constexpr int kLineWidth = 1;
      app_stream << kLineWidth << " w " << rect.left << " " << y << " m "
                 << rect.right << " " << y << " l S\n";
    }
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &app_stream,
                       std::move(resources_dict),
                       true /*IsTextMarkupAnnotation*/);
  return true;
}

bool GenerateLinkAP(CPDF_Document* doc,
                    CPDF_Dictionary* annot_dict,
                    const ByteString& blend_name) {
  // Get border width - default to 1 if not specified
  float border_width = GetBorderWidth(annot_dict);
  if (border_width <= 0) {
    return true;  // No visible border, no AP needed
  }

  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  rect.Normalize();

  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  // Set stroke color from /C array (default: blue for links)
  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 0, 0, 1),  // Default: blue
      PaintOperation::kStroke);

  app_stream << border_width << " w ";
  app_stream << GetDashPatternString(annot_dict);

  // Determine border style
  BorderStyleInfo border_info =
      GetBorderStyleInfo(annot_dict->GetDictFor("BS"));

  switch (border_info.style) {
    case BorderStyle::kUnderline: {
      // Draw underline: 1pt above bottom, inset 1pt from sides
      float y = rect.bottom + 1.0f;
      app_stream << (rect.left + 1.0f) << " " << y << " m "
                 << (rect.right - 1.0f) << " " << y << " l S\n";
      break;
    }
    case BorderStyle::kSolid:
    case BorderStyle::kDash:
    default: {
      // Draw rectangle border
      CFX_FloatRect stroke_rect = rect;
      stroke_rect.Deflate(border_width / 2.0f, border_width / 2.0f);
      app_stream << stroke_rect.left << " " << stroke_rect.bottom << " "
                 << stroke_rect.Width() << " " << stroke_rect.Height()
                 << " re S\n";
      break;
    }
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict = GenerateResourcesDict(doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(doc, annot_dict, &app_stream, std::move(resources_dict),
                       /*is_text_markup_annotation=*/false);
  return true;
}

// EmbedPDF: the regions a /Redact annotation targets — /QuadPoints quads when
// present (text redactions), else the annotation /Rect (area redactions).
// One overlay region: the AABB (label layout + area marks) plus the exact
// oriented quad when the marked cell is not axis-aligned (rotated text marks
// fill their true cells; the label tiling stays axis-aligned in the bbox,
// matching the live scene preview).
struct RedactOverlayRegion {
  CFX_FloatRect bbox;
  std::optional<MarkupQuad> quad;
};

bool RedactQuadValuesAreFinite(const CPDF_Array* array, size_t index) {
  const size_t offset = index * 8;
  for (size_t i = 0; i < 8; ++i) {
    if (!std::isfinite(array->GetFloatAt(offset + i))) {
      return false;
    }
  }
  return true;
}

std::vector<RedactOverlayRegion> GetRedactOverlayRegions(
    const CPDF_Dictionary* annot_dict) {
  std::vector<RedactOverlayRegion> regions;
  RetainPtr<const CPDF_Array> quad_points_array =
      annot_dict->GetArrayFor("QuadPoints");
  if (quad_points_array && quad_points_array->size() >= 8) {
    const size_t quad_count =
        CPDF_Annot::QuadPointCount(quad_points_array.Get());
    bool invalid_quad = false;
    for (size_t i = 0; i < quad_count; ++i) {
      if (!RedactQuadValuesAreFinite(quad_points_array.Get(), i)) {
        invalid_quad = true;
        break;
      }
      std::optional<MarkupQuad> quad =
          ReadMarkupQuad(quad_points_array.Get(), i);
      CFX_FloatRect rect = CPDF_Annot::RectFromQuadPoints(annot_dict, i);
      rect.Normalize();
      if (rect.IsEmpty()) {
        invalid_quad = true;
        break;
      }
      RedactOverlayRegion region;
      region.bbox = rect;
      if (quad && !IsAxisAlignedZigZag(*quad)) {
        region.quad = quad;
      }
      regions.push_back(region);
    }
    if (!invalid_quad && !regions.empty()) {
      return regions;
    }
    regions.clear();
  }
  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  rect.Normalize();
  if (!rect.IsEmpty()) {
    RedactOverlayRegion region;
    region.bbox = rect;
    regions.push_back(region);
  }
  return regions;
}

// EmbedPDF: the marking-stage /AP BBox and the overlay BBox share this rule:
// quad bounding box for text redactions, /Rect for area redactions.
CFX_FloatRect GetRedactOverlayBBox(const CPDF_Dictionary* annot_dict) {
  const std::vector<RedactOverlayRegion> regions =
      GetRedactOverlayRegions(annot_dict);
  if (regions.empty()) {
    return CFX_FloatRect();
  }
  CFX_FloatRect bbox = regions.front().bbox;
  for (size_t i = 1; i < regions.size(); ++i) {
    bbox.Union(regions[i].bbox);
  }
  return bbox;
}

RetainPtr<CPDF_Stream> MakeRedactFormStream(
    CPDF_Document* doc,
    const CFX_FloatRect& bbox,
    RetainPtr<CPDF_Dictionary> resources,
    fxcrt::ostringstream* ops) {
  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  stream_dict->SetNewFor<CPDF_Number>("FormType", 1);
  stream_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  stream_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  stream_dict->SetMatrixFor("Matrix", CFX_Matrix());
  stream_dict->SetRectFor("BBox", bbox);
  if (resources) {
    stream_dict->SetFor("Resources", std::move(resources));
  }
  auto stream = doc->NewIndirect<CPDF_Stream>(std::move(stream_dict));
  stream->SetDataFromStringstream(ops);
  return stream;
}

constexpr float kRedactRepeatFallbackFontSize = 12.0f;
constexpr int kRedactMaxRepeatDoublings = 10;  // 2^10 = 1024 label instances

// EmbedPDF: lay out the /OverlayText label for one redacted region with the
// same CPVT + annotation-font-map stack as FreeText appearances, so /DA fonts
// (standard, DR-resolved, or registered runtime fonts) shape and subset
// identically. Top-aligned in reading order; ISO 32000-2 prescribes neither
// the vertical placement nor the /Repeat tiling, so /Repeat is expressed as
// "repeat the label, space-joined, wrapped to the region, clipped".
void AppendRedactLabelForRegion(CPDF_AnnotFontMap& map,
                                const CPDF_Dictionary* annot_dict,
                                const WideString& overlay_text,
                                float da_font_size,
                                const CFX_Color& label_color,
                                const CFX_FloatRect& region,
                                fxcrt::ostringstream& stream) {
  CPVT_VariableText::Provider provider(&map);
  CPVT_VariableText vt(&provider);
  vt.SetPlateRect(region);
  vt.SetAlignment(annot_dict->GetIntegerFor("Q"));
  vt.SetMultiLine(true);
  vt.SetAutoReturn(true);

  const bool repeat = annot_dict->GetBooleanFor("Repeat", false);
  // CPVT auto-sizing fits ALL text into the plate, so it cannot combine with
  // /Repeat (more repetitions would only shrink the font); pin a concrete
  // size for the repeat case when /DA asks for auto (size 0).
  float font_size = da_font_size;
  if (repeat && FXSYS_IsFloatZero(font_size)) {
    font_size = kRedactRepeatFallbackFontSize;
  }
  SetVtFontSize(font_size, vt);
  vt.Initialize();
  vt.SetText(overlay_text);
  vt.RearrangeAll();

  if (repeat) {
    // Double the space-joined text until the wrapped layout covers the region
    // vertically; the last row's surplus is removed by the region clip below.
    // Bounded to keep tiny-font/huge-region combinations sane.
    WideString tiled = overlay_text;
    for (int i = 0; i < kRedactMaxRepeatDoublings &&
                    vt.GetContentRect().Height() < region.Height();
         ++i) {
      WideString doubled = tiled;
      doubled += L' ';
      doubled += tiled;
      tiled = std::move(doubled);
      vt.SetText(tiled);
      vt.RearrangeAll();
    }
  }

  const ByteString body =
      GenerateEditAP(vt.GetProvider()->GetFontMap(), vt.GetIterator(),
                     CFX_PointF(0.0f, 0.0f), /*continuous=*/true,
                     /*sub_word=*/0);
  if (body.IsEmpty()) {
    return;
  }
  stream << "q\n";
  WriteRect(stream, region) << " re W n\n";
  stream << "BT\n"
         << GenerateColorAP(label_color, PaintOperation::kFill) << body
         << "ET\nQ\n";
}

// EmbedPDF: emit the final ("post-apply") overlay ops for a /Redact
// annotation: opaque /IC fill of every region, then the /OverlayText label.
// The marking-stage /CA opacity is deliberately not carried over — the
// content underneath is destroyed, so the replacement marking paints opaque,
// matching Acrobat. Returns false when the annotation defines neither a fill
// nor a label.
bool AppendRedactOverlayOps(CPDF_Document* doc,
                            const CPDF_Dictionary* annot_dict,
                            fxcrt::ostringstream& stream,
                            RetainPtr<CPDF_Dictionary>* out_font_resources) {
  const WideString overlay_text = annot_dict->GetUnicodeTextFor("OverlayText");
  RetainPtr<const CPDF_Array> interior_color = annot_dict->GetArrayFor("IC");
  const bool has_fill = interior_color && !interior_color->IsEmpty();
  if (!has_fill && overlay_text.IsEmpty()) {
    return false;
  }

  const std::vector<RedactOverlayRegion> regions =
      GetRedactOverlayRegions(annot_dict);
  if (regions.empty()) {
    return false;
  }

  if (has_fill) {
    stream << GetColorStringWithDefault(
        interior_color.Get(), CFX_Color(CFX_Color::Type::kTransparent),
        PaintOperation::kFill);
    for (const RedactOverlayRegion& region : regions) {
      if (region.quad.has_value()) {
        // Fill the exact oriented cell (ring order, like the highlight AP).
        WritePoint(stream, region.quad->upper_start) << " m ";
        WritePoint(stream, region.quad->upper_end) << " l ";
        WritePoint(stream, region.quad->lower_end) << " l ";
        WritePoint(stream, region.quad->lower_start) << " l h f\n";
      } else {
        WriteRect(stream, region.bbox) << " re f\n";
      }
    }
  }

  if (overlay_text.IsEmpty()) {
    return true;
  }

  // /DA resolution mirrors the FreeText persistent path, but is forgiving:
  // redact annotations marked by other producers can lack /DA (ISO requires
  // it alongside /OverlayText, but such files exist) or an AcroForm /DR —
  // fall back to Helvetica rather than dropping the label.
  RetainPtr<CPDF_Dictionary> root_dict = doc->GetMutableRoot();
  RetainPtr<CPDF_Dictionary> form_dict;
  if (root_dict) {
    form_dict = root_dict->GetMutableDictFor("AcroForm");
    if (!form_dict) {
      form_dict = CPDF_InteractiveForm::InitAcroFormDict(doc);
    }
  }

  std::optional<DefaultAppearanceInfo> da_info =
      form_dict ? GetDefaultAppearanceInfo(annot_dict, form_dict.Get())
                : std::nullopt;
  const ByteString font_name =
      da_info.has_value() ? da_info.value().font_name : ByteString("Helv");
  const float da_font_size =
      da_info.has_value() ? da_info.value().font_size : 0.0f;

  CFX_Color label_color =
      da_info.has_value() ? da_info.value().text_color : CFX_Color();
  if (label_color.nColorType == CFX_Color::Type::kTransparent) {
    // Legacy EmbedPDF v2 files carry the label colour in /OC; ISO keeps it
    // in the /DA string. Default to black when neither is present.
    RetainPtr<const CPDF_Array> oc = annot_dict->GetArrayFor("OC");
    label_color = (oc && oc->size() >= 3)
                      ? fpdfdoc::CFXColorFromArray(*oc)
                      : CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0);
  }

  DaFontResolution da_font;
  if (form_dict) {
    RetainPtr<CPDF_Dictionary> dr_font_dict =
        form_dict->GetOrCreateDictFor("DR")->GetOrCreateDictFor("Font");
    da_font =
        ResolveDaFontForPersistentTarget(doc, dr_font_dict.Get(), font_name);
  } else {
    da_font.font_dict = GenerateFallbackFontDict(doc);
  }
  RetainPtr<CPDF_Font> default_font =
      da_font.font_dict
          ? CPDF_DocPageData::FromDocument(doc)->GetFont(da_font.font_dict)
          : nullptr;
  if (!default_font &&
      da_font.registered_font_id == CFX_FontRegistry::kInvalidFontId) {
    return has_fill;
  }

  CPDF_AnnotFontMap map(doc, std::move(default_font), font_name,
                        /*allow_registered_fallbacks=*/true,
                        da_font.registered_font_id,
                        /*install_dr_entry=*/true);
  if (!map.HasDefaultFont()) {
    return has_fill;
  }
  for (const RedactOverlayRegion& region : regions) {
    AppendRedactLabelForRegion(map, annot_dict, overlay_text, da_font_size,
                               label_color, region.bbox, stream);
  }
  *out_font_resources = map.CreateFontResourceDict();
  return !!*out_font_resources;
}

bool GenerateRedactAP(CPDF_Document* doc,
                      CPDF_Dictionary* annot_dict,
                      const ByteString& blend_name) {
  // Normal (marking-stage) appearance: border-only outline in /C, default
  // red. The filled preview is NOT drawn here — R/D and /RO all share the
  // final overlay from BuildRedactOverlayForm, so hovering a marked
  // redaction previews exactly what apply will paint.
  fxcrt::ostringstream normal_stream;
  normal_stream << "/" << kGSDictName << " gs ";
  normal_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 1, 0, 0),  // default: red
      PaintOperation::kStroke);

  const float border_width = GetBorderWidth(annot_dict);
  if (border_width > 0) {
    normal_stream << border_width << " w ";
    normal_stream << GetDashPatternString(annot_dict);
    for (const RedactOverlayRegion& region :
         GetRedactOverlayRegions(annot_dict)) {
      CFX_FloatRect stroke_rect = region.bbox;
      stroke_rect.Deflate(border_width / 2, border_width / 2);
      normal_stream << stroke_rect.left << " " << stroke_rect.bottom << " "
                    << stroke_rect.Width() << " " << stroke_rect.Height()
                    << " re S\n";
    }
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict = GenerateResourcesDict(doc, std::move(gs_dict), nullptr);
  const CFX_FloatRect bbox = GetRedactOverlayBBox(annot_dict);
  RetainPtr<CPDF_Stream> normal_pdf_stream = MakeRedactFormStream(
      doc, bbox, std::move(resources_dict), &normal_stream);

  // Rollover/Down and /RO share the final overlay (fill + label).
  RetainPtr<CPDF_Stream> overlay =
      CPDF_GenerateAP::BuildRedactOverlayForm(doc, annot_dict);
  if (!overlay) {
    // Neither /IC nor /OverlayText: keep the AP structure (and the baked /RO
    // that pre-v3 clients flatten on apply) with an empty overlay.
    fxcrt::ostringstream empty_stream;
    overlay = MakeRedactFormStream(doc, bbox, nullptr, &empty_stream);
  }

  RetainPtr<CPDF_Dictionary> ap_dict =
      annot_dict->GetOrCreateDictFor(pdfium::annotation::kAP);
  ap_dict->SetNewFor<CPDF_Reference>("N", doc, normal_pdf_stream->GetObjNum());
  ap_dict->SetNewFor<CPDF_Reference>("R", doc, overlay->GetObjNum());
  ap_dict->SetNewFor<CPDF_Reference>("D", doc, overlay->GetObjNum());

  // /RO lives on the annotation dict, not inside /AP.
  annot_dict->SetNewFor<CPDF_Reference>("RO", doc, overlay->GetObjNum());
  return true;
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

  // Background + border in isolated graphics state
  app_stream << "q\n";
  if (has_bg) {
    app_stream << GenerateColorAP(mk.background_color, PaintOperation::kFill);
    WriteRect(app_stream, bbox) << " re f*\n";
  }
  if (has_bc && bw > 0) {
    app_stream << GenerateColorAP(mk.border_color, PaintOperation::kStroke);
    WriteFloat(app_stream, bw) << " w\n";
    WriteRect(app_stream, stroke_rect) << " re s\n";
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

std::optional<CPDF_GenerateAP::FormType> GetWidgetFormType(
    const CPDF_Dictionary* annot_dict) {
  RetainPtr<const CPDF_Object> field_type_obj =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, pdfium::form_fields::kFT);
  if (!field_type_obj) {
    return std::nullopt;
  }

  const ByteString field_type = field_type_obj->GetString();
  if (field_type == pdfium::form_fields::kTx) {
    return CPDF_GenerateAP::kTextField;
  }

  if (field_type != pdfium::form_fields::kCh) {
    return std::nullopt;
  }

  RetainPtr<const CPDF_Object> field_flags_obj =
      CPDF_FormField::GetFieldAttrForDict(annot_dict, pdfium::form_fields::kFf);
  const uint32_t flags = field_flags_obj ? field_flags_obj->GetInteger() : 0;
  return (flags & pdfium::form_flags::kChoiceCombo) ? CPDF_GenerateAP::kComboBox
                                                    : CPDF_GenerateAP::kListBox;
}

bool GenerateFormAPToTarget(APGenerationTarget* target,
                            CPDF_Dictionary* annot_dict,
                            CPDF_GenerateAP::FormType type,
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
      case CPDF_GenerateAP::kTextField:
        GenerateTextFieldFormAP(app_stream, annot_dict, dims.bbox,
                                default_appearance_info.value(), provider,
                                value_override);
        break;
      case CPDF_GenerateAP::kComboBox:
        GenerateComboBoxFormAP(app_stream, annot_dict, dims.bbox,
                               default_appearance_info.value(), provider,
                               value_override);
        break;
      case CPDF_GenerateAP::kListBox:
        GenerateListBoxFormAP(app_stream, annot_dict, dims.bbox,
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

}  // namespace

// static
void CPDF_GenerateAP::GenerateFormAP(CPDF_Document* doc,
                                     CPDF_Dictionary* annot_dict,
                                     FormType type) {
  APGenerationTarget target{doc, annot_dict};
  GenerateFormAPToTarget(&target, annot_dict, type, nullptr);
}

// static
bool CPDF_GenerateAP::GenerateFormAPWithValueOverride(
    CPDF_Document* doc,
    CPDF_Dictionary* annot_dict,
    FormType type,
    const WideString& value_override) {
  APGenerationTarget target{doc, annot_dict};
  return GenerateFormAPToTarget(&target, annot_dict, type, &value_override);
}

// static
std::optional<CPDF_GenerateAP::GeneratedAP>
CPDF_GenerateAP::GenerateEphemeralFormAP(CPDF_Document* doc,
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
void CPDF_GenerateAP::GenerateCheckboxFormAP(CPDF_Document* doc,
                                             CPDF_Dictionary* annot_dict) {
  const AppearanceCharacteristics mk =
      GetAppearanceCharacteristics(annot_dict->GetDictFor("MK"));
  const BorderStyleInfo bs = GetBorderStyleInfo(annot_dict->GetDictFor("BS"));
  const AnnotationDimensionsAndColor dims =
      GetAnnotationDimensionsAndColor(annot_dict);

  CFX_FloatRect body_rect = dims.bbox;
  body_rect.Deflate(bs.width, bs.width);
  CFX_FloatRect check_rect = body_rect.GetCenterSquare();

  // Off state: box only (background fill + border stroke).
  fxcrt::ostringstream off_content;
  BuildCheckboxBoxStream(off_content, mk, bs, dims.bbox);

  // Yes state: same box + checkmark path.
  fxcrt::ostringstream yes_content;
  BuildCheckboxBoxStream(yes_content, mk, bs, dims.bbox);
  yes_content << "q\n";
  yes_content << "0 0 0 rg\n";
  GenerateCheckmarkPath(yes_content, check_rect);
  yes_content << "f\nQ\n";

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

// static
void CPDF_GenerateAP::GenerateRadioButtonFormAP(CPDF_Document* doc,
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

  // Off state: circle only (background fill + border stroke).
  fxcrt::ostringstream off_content;
  BuildRadioCircleStream(off_content, mk, bs, dims.bbox);

  // Yes state: same circle + filled inner dot.
  fxcrt::ostringstream yes_content;
  BuildRadioCircleStream(yes_content, mk, bs, dims.bbox);
  yes_content << "q\n";
  yes_content << "0 0 0 rg\n";
  WriteEllipse(yes_content, inner_rx, inner_ry);
  yes_content << "f*\nQ\n";

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

// static
void CPDF_GenerateAP::GenerateEmptyAP(CPDF_Document* doc,
                                      CPDF_Dictionary* annot_dict) {
  auto gs_dict = GenerateExtGStateDict(*annot_dict, "Normal");
  auto resources_dict = GenerateResourcesDict(doc, std::move(gs_dict), nullptr);

  fxcrt::ostringstream stream;
  GenerateAndSetAPDict(doc, annot_dict, &stream, std::move(resources_dict),
                       false);
}

bool GenerateCaretAP(APGenerationTarget* target,
                     CPDF_Dictionary* annot_dict,
                     const ByteString& blend_name) {
  CPDF_Document* doc = target->doc;
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0), PaintOperation::kStroke);
  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color(CFX_Color::Type::kRGB, 0, 0, 0), PaintOperation::kFill);

  // A caret anchored to rotated text carries the box-family /EMBD_Metadata
  // rotation pair: draw in the UNROTATED box and let the AP form matrix turn
  // the symbol onto its text's baseline (the shape/free-text machinery).
  const ShapeRotationInfo rot_info = GetShapeRotationInfo(annot_dict);
  CFX_FloatRect rect = rot_info.bbox;
  rect.Normalize();

  float draw_left = rect.left;
  float draw_bottom = rect.bottom;
  float draw_right = rect.right;
  float draw_top = rect.top;

  RetainPtr<const CPDF_Array> rd_array = annot_dict->GetArrayFor("RD");
  if (rd_array && rd_array->size() == 4) {
    draw_left += rd_array->GetFloatAt(0);
    draw_bottom += rd_array->GetFloatAt(1);
    draw_right -= rd_array->GetFloatAt(2);
    draw_top -= rd_array->GetFloatAt(3);
  }

  float width = draw_right - draw_left;
  float height = draw_top - draw_bottom;
  float mid_x = (draw_left + draw_right) / 2.0f;

  app_stream << "0.5 w\n";

  // Left bezier: from (draw_left, draw_bottom) to (mid_x, draw_top)
  app_stream << draw_left << " " << draw_bottom << " m\n";
  app_stream << (draw_left + width * 0.27f) << " " << draw_bottom << " "
             << mid_x << " " << (draw_bottom + height * 0.44f) << " " << mid_x
             << " " << draw_top << " c\n";

  // Right bezier: from (mid_x, draw_top) to (draw_right, draw_bottom)
  app_stream << mid_x << " " << (draw_bottom + height * 0.44f) << " "
             << (draw_right - width * 0.27f) << " " << draw_bottom << " "
             << draw_right << " " << draw_bottom << " c\n";

  app_stream << "B*\n";

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict = GenerateResourcesDict(doc, std::move(gs_dict), nullptr);
  if (rot_info.is_rotated) {
    GenerateAndSetAPDictWithTransform(target, annot_dict, &app_stream,
                                      std::move(resources_dict),
                                      rot_info.matrix, rot_info.bbox);
  } else {
    GenerateAndSetAPDict(target, annot_dict, &app_stream,
                         std::move(resources_dict),
                         false /*IsTextMarkupAnnotation*/);
  }
  return true;
}

bool GenerateAnnotAPToTarget(APGenerationTarget* target,
                             CPDF_Dictionary* annot_dict,
                             CPDF_Annot::Subtype subtype,
                             BlendMode blend_mode) {
  ByteString blend_name = BlendModeToPDFName(blend_mode);
  switch (subtype) {
    case CPDF_Annot::Subtype::CIRCLE:
      return GenerateCircleAP(target, annot_dict, blend_name);
    case CPDF_Annot::Subtype::HIGHLIGHT:
      return GenerateHighlightAP(target, annot_dict, blend_name);
    case CPDF_Annot::Subtype::INK:
      return GenerateInkAP(target, annot_dict, blend_name);
    case CPDF_Annot::Subtype::LINE:
      return GenerateLineAP(target, annot_dict, blend_name);
    case CPDF_Annot::Subtype::POLYGON:
      return GeneratePolygonAP(target, annot_dict, blend_name);
    case CPDF_Annot::Subtype::POLYLINE:
      return GeneratePolyLineAP(target, annot_dict, blend_name);
    case CPDF_Annot::Subtype::SQUARE:
      return GenerateSquareAP(target, annot_dict, blend_name);
    case CPDF_Annot::Subtype::SQUIGGLY:
      return GenerateSquigglyAP(target, annot_dict, blend_name);
    case CPDF_Annot::Subtype::STRIKEOUT:
      return GenerateStrikeOutAP(target, annot_dict, blend_name);
    case CPDF_Annot::Subtype::UNDERLINE:
      return GenerateUnderlineAP(target, annot_dict, blend_name);
    default:
      return false;
  }
}

// static
bool CPDF_GenerateAP::GenerateAnnotAP(CPDF_Document* doc,
                                      CPDF_Dictionary* annot_dict,
                                      CPDF_Annot::Subtype subtype) {
  return GenerateAnnotAP(doc, annot_dict, subtype,
                         DefaultBlendModeFor(subtype));
}

// static
bool CPDF_GenerateAP::GenerateAnnotAP(CPDF_Document* doc,
                                      CPDF_Dictionary* annot_dict,
                                      CPDF_Annot::Subtype subtype,
                                      BlendMode blend_mode) {
  APGenerationTarget target{doc, annot_dict};
  if (GenerateAnnotAPToTarget(&target, annot_dict, subtype, blend_mode)) {
    return true;
  }

  ByteString blend_name = BlendModeToPDFName(blend_mode);
  switch (subtype) {
    case CPDF_Annot::Subtype::FREETEXT:
      return GenerateFreeTextAP(&target, annot_dict, blend_name);
    case CPDF_Annot::Subtype::POPUP:
      return GeneratePopupAP(doc, annot_dict, blend_name);
    case CPDF_Annot::Subtype::TEXT:
      return GenerateTextAP(doc, annot_dict, blend_name);
    case CPDF_Annot::Subtype::FILEATTACHMENT:
      return GenerateFileAttachmentAP(doc, annot_dict, blend_name);
    case CPDF_Annot::Subtype::LINK:
      return GenerateLinkAP(doc, annot_dict, blend_name);
    case CPDF_Annot::Subtype::REDACT:
      return GenerateRedactAP(doc, annot_dict, blend_name);
    case CPDF_Annot::Subtype::CARET:
      return GenerateCaretAP(&target, annot_dict, blend_name);
    default:
      return false;
  }
}

// static
std::optional<CPDF_GenerateAP::GeneratedAP>
CPDF_GenerateAP::GenerateEphemeralAnnotAP(CPDF_Document* doc,
                                          const CPDF_Dictionary* annot_dict,
                                          CPDF_Annot::Subtype subtype) {
  return GenerateEphemeralAnnotAP(doc, annot_dict, subtype,
                                  DefaultBlendModeFor(subtype));
}

// static
std::optional<CPDF_GenerateAP::GeneratedAP>
CPDF_GenerateAP::GenerateEphemeralAnnotAP(CPDF_Document* doc,
                                          const CPDF_Dictionary* annot_dict,
                                          CPDF_Annot::Subtype subtype,
                                          BlendMode blend_mode) {
  if (!SupportsEphemeralAnnotAP(subtype)) {
    return std::nullopt;
  }

  if (subtype == CPDF_Annot::Subtype::WIDGET) {
    std::optional<FormType> type = GetWidgetFormType(annot_dict);
    return type.has_value()
               ? GenerateEphemeralFormAP(doc, annot_dict, type.value())
               : std::nullopt;
  }

  APGenerationTarget target{doc, nullptr};
  CPDF_Dictionary* mutable_annot_dict =
      const_cast<CPDF_Dictionary*>(annot_dict);
  if (subtype == CPDF_Annot::Subtype::FREETEXT) {
    if (!GenerateFreeTextAP(&target, mutable_annot_dict,
                            BlendModeToPDFName(blend_mode))) {
      return std::nullopt;
    }
    return GeneratedAP{std::move(target.ephemeral_fonts),
                       std::move(target.normal_stream)};
  }

  if (!GenerateAnnotAPToTarget(&target, mutable_annot_dict, subtype,
                               blend_mode)) {
    return std::nullopt;
  }

  return GeneratedAP{std::move(target.ephemeral_fonts),
                     std::move(target.normal_stream)};
}

// static
bool CPDF_GenerateAP::CanGenerateEphemeralAnnotAP(CPDF_Annot::Subtype subtype) {
  return SupportsEphemeralAnnotAP(subtype);
}

// static
RetainPtr<CPDF_Stream> CPDF_GenerateAP::BuildRedactOverlayForm(
    CPDF_Document* doc,
    const CPDF_Dictionary* annot_dict) {
  if (!doc || !annot_dict) {
    return nullptr;
  }
  fxcrt::ostringstream ops;
  RetainPtr<CPDF_Dictionary> font_resources;
  if (!AppendRedactOverlayOps(doc, annot_dict, ops, &font_resources)) {
    return nullptr;
  }
  RetainPtr<CPDF_Dictionary> resources;
  if (font_resources) {
    resources = GenerateResourcesDict(doc, nullptr, std::move(font_resources));
  }
  return MakeRedactFormStream(doc, GetRedactOverlayBBox(annot_dict),
                              std::move(resources), &ops);
}

// static
bool CPDF_GenerateAP::GenerateDefaultAppearanceWithColor(
    CPDF_Document* doc,
    CPDF_Dictionary* annot_dict,
    const CFX_Color& color) {
  RetainPtr<CPDF_Dictionary> root_dict = doc->GetMutableRoot();
  if (!root_dict) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> acroform_dict =
      root_dict->GetMutableDictFor("AcroForm");
  if (!acroform_dict) {
    acroform_dict = CPDF_InteractiveForm::InitAcroFormDict(doc);
    CHECK(acroform_dict);
  }

  CPDF_DefaultAppearance default_appearance(annot_dict, acroform_dict);
  auto maybe_font_name_and_size = default_appearance.GetFont();
  if (!maybe_font_name_and_size.has_value()) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> dr_font_dict =
      acroform_dict->GetOrCreateDictFor("DR")->GetOrCreateDictFor("Font");
  if (!GetFontFromDrFontDictOrGenerateFallback(
          doc, dr_font_dict.Get(), maybe_font_name_and_size.value().name)) {
    return false;
  }

  ByteString new_default_appearance_font_name_and_size =
      StringFromFontNameAndSize(maybe_font_name_and_size.value().name,
                                maybe_font_name_and_size.value().size);
  if (new_default_appearance_font_name_and_size.IsEmpty()) {
    return false;
  }

  ByteString new_default_appearance_color =
      GenerateColorAP(color, PaintOperation::kFill);
  CHECK(!new_default_appearance_color.IsEmpty());
  // EmbedPDF: Strip trailing newlines and write color before font.
  // GenerateColorAP/StringFromFontNameAndSize append '\n' for content streams,
  // but the /DA string value must be a compact single-line format for Adobe
  // Acrobat compatibility. Adobe's FreeText callout AP regeneration fails to
  // parse the color when the DA contains embedded newlines, causing the
  // border/line color to fall back to black. Color-first order also matches
  // the format Adobe produce.
  new_default_appearance_color.TrimBack('\n');
  new_default_appearance_font_name_and_size.TrimBack('\n');
  annot_dict->SetNewFor<CPDF_String>(
      "DA", new_default_appearance_color + " " +
                new_default_appearance_font_name_and_size);

  // TODO(thestig): Call GenerateAnnotAP();
  return true;
}

bool CPDF_GenerateAP::UpdateDefaultAppearance(CPDF_Document* doc,
                                              CPDF_Dictionary* annot_dict,
                                              CPDF_Annot::StandardFont font,
                                              float font_size,
                                              const CFX_Color& color) {
  ByteString resource_key;

  // When font is kUnknown, preserve the existing non-standard font resource
  // key from the current DA string instead of failing. This allows updating
  // fontSize and fontColor without replacing the original font.
  if (font == CPDF_Annot::StandardFont::kUnknown) {
    ByteString existing_da = annot_dict->GetByteStringFor("DA");
    CPDF_DefaultAppearance current_da(existing_da);
    auto font_info = current_da.GetFont();
    if (!font_info.has_value() || font_info->name.IsEmpty()) {
      return false;
    }
    resource_key = font_info->name;
  } else {
    RetainPtr<CPDF_Dictionary> root_dict = doc->GetMutableRoot();
    if (!root_dict) {
      return false;
    }

    RetainPtr<CPDF_Dictionary> acroform_dict =
        root_dict->GetMutableDictFor("AcroForm");
    if (!acroform_dict) {
      acroform_dict = CPDF_InteractiveForm::InitAcroFormDict(doc);
      CHECK(acroform_dict);
    }

    ByteString base_font_name = CPDF_Annot::StandardFontToString(font);
    if (base_font_name.IsEmpty()) {
      return false;
    }

    resource_key =
        EnsureFontInAcroFormDR(doc, acroform_dict.Get(), base_font_name);
    if (resource_key.IsEmpty()) {
      return false;
    }
  }

  ByteString da_font_part = StringFromFontNameAndSize(resource_key, font_size);
  ByteString da_color_part = GenerateColorAP(color, PaintOperation::kFill);
  // EmbedPDF: Strip trailing newlines and write color before font.
  // See comment in GenerateDefaultAppearanceWithColor for rationale.
  da_color_part.TrimBack('\n');
  da_font_part.TrimBack('\n');

  annot_dict->SetNewFor<CPDF_String>("DA", da_color_part + " " + da_font_part);
  return true;
}

bool CPDF_GenerateAP::UpdateDefaultAppearanceRegisteredFont(
    CPDF_Document* doc,
    CPDF_Dictionary* annot_dict,
    CFX_FontRegistry::FontId font_id,
    float font_size,
    const CFX_Color& color) {
  // EmbedPDF: allow FreeText DA to reference a registered runtime font. The DA
  // names a reserved alias; the real /DR font (the embedded subset) is
  // installed when AP generation knows the characters used by this
  // annotation/layer. No placeholder is written: Acrobat refuses to edit a
  // FreeText whose /DR font is a descriptor-less stub.
  if (!doc || !annot_dict || !CFX_FontRegistry::IsValidFont(font_id)) {
    return false;
  }
  // A preview-and-print font renders existing text but may not author new
  // text until the app asserts a licence (EPDFFont_AuthorizeEditing).
  if (!CFX_FontRegistry::IsEditingAuthorized(font_id)) {
    return false;
  }

  ByteString resource_key;
  if (!CPDF_AnnotFontMap::ReserveRegisteredFontAlias(doc, font_id,
                                                     &resource_key) ||
      resource_key.IsEmpty()) {
    return false;
  }

  ByteString da_font_part = StringFromFontNameAndSize(resource_key, font_size);
  ByteString da_color_part = GenerateColorAP(color, PaintOperation::kFill);
  da_color_part.TrimBack('\n');
  da_font_part.TrimBack('\n');

  annot_dict->SetNewFor<CPDF_String>("DA", da_color_part + " " + da_font_part);
  return true;
}

// ---- Rich text FreeText, the writer's halves (Phase D)
// --------------------------

CPDF_GenerateAP::PreparedRichFreeTextAP::PreparedRichFreeTextAP() = default;

CPDF_GenerateAP::PreparedRichFreeTextAP::PreparedRichFreeTextAP(
    PreparedRichFreeTextAP&& that) noexcept = default;

CPDF_GenerateAP::PreparedRichFreeTextAP&
CPDF_GenerateAP::PreparedRichFreeTextAP::operator=(
    PreparedRichFreeTextAP&& that) noexcept = default;

CPDF_GenerateAP::PreparedRichFreeTextAP::~PreparedRichFreeTextAP() = default;

// static
std::unique_ptr<CPDF_GenerateAP::PreparedRichFreeTextAP>
CPDF_GenerateAP::PrepareRichFreeTextAP(CPDF_Document* doc,
                                       const CPDF_Dictionary* annot_dict,
                                       const RichFreeTextRequest& request) {
  RichFreeTextInputs in;
  in.document = request.document;
  in.da_color = request.da_color;
  in.persistent = true;
  in.author_default_appearance = true;
  return PrepareRichFreeTextAPInternal(
      doc, annot_dict,
      BlendModeToPDFName(DefaultBlendModeFor(CPDF_Annot::Subtype::FREETEXT)),
      std::move(in));
}

// static
bool CPDF_GenerateAP::PublishRichFreeTextAP(
    CPDF_Document* doc,
    CPDF_Dictionary* annot_dict,
    std::unique_ptr<PreparedRichFreeTextAP> prepared) {
  if (!doc || !annot_dict) {
    return false;
  }
  APGenerationTarget target{doc, annot_dict};
  return PublishRichFreeTextAPToTarget(&target, annot_dict,
                                       std::move(prepared));
}
