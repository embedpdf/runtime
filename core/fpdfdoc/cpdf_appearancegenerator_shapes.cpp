// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

// EmbedPDF: the appearance generator's shapes: square, circle, line, polygon,
// polyline and ink, with their line endings, cloudy borders and dimension
// captions.
//
// See docs/embedpdf/fork-ledger.md, rule own-appearance-generator. EmbedPDF's
// changes are Copyright 2026 CloudPDF LTD.

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "constants/annotation_common.h"
#include "core/fxcrt/stream_write_utils.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_appearancegenerator_dimension.h"
#include "core/fpdfdoc/cpdf_appearancegenerator_internal.h"
#include "core/fpdfdoc/cpdf_cloudy_border.h"
#include "core/fpdfdoc/cpdf_defaultappearance.h"
#include "core/fpdfdoc/cpdf_ink_curves.h"
#include "core/fpdfdoc/cpdf_richtext.h"
#include "core/fpdfdoc/cpdf_richtextlayout.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/fx_system.h"

using appearance_generator::APGenerationTarget;
using appearance_generator::ArrowStyle;
using appearance_generator::DaFontResolution;
using appearance_generator::EmitArrowPath;
using appearance_generator::EmitButtOrSlashPath;
using appearance_generator::EmitCirclePath;
using appearance_generator::EmitDiamondPath;
using appearance_generator::EmitEndingWithAngle;
using appearance_generator::EmitRichTextBody;
using appearance_generator::EmitSquarePath;
using appearance_generator::GenerateAndSetBoxAPDict;
using appearance_generator::GenerateAndSetPointsAPDict;
using appearance_generator::GenerateExtGStateDict;
using appearance_generator::GenerateResourcesDict;
using appearance_generator::GetBorderWidth;
using appearance_generator::GetColorStringWithDefault;
using appearance_generator::GetDashPatternString;
using appearance_generator::GetPaintOperatorString;
using appearance_generator::GetShapeRotationInfo;
using appearance_generator::GetStrokeColorString;
using appearance_generator::IsFinitePoint;
using appearance_generator::kButtLenFactor;
using appearance_generator::kGSDictName;
using appearance_generator::kSlashLenFactor;
using appearance_generator::PaintOperation;
using appearance_generator::ReadLineEndingToken;
using appearance_generator::ResolveDaFontForEphemeralTarget;
using appearance_generator::ShapeRotationInfo;
using appearance_generator::UnitVector;

namespace {

constexpr float kArrowAngle = FXSYS_PI / 6.0f;

constexpr float kArrowLenFactor = 9.0f;

// The operator that paints a closed circle, square or diamond ending.
const char* ClosedEndingOperator(bool filled, bool stroked) {
  if (!stroked) {
    return "f\n";
  }
  return filled ? "B\n" : "S\n";
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

  // Get styles needed for drawing the endings. A hairline (/W 0) still gets
  // the endings a 1 pt line has, as Acrobat draws them.
  const float border_w = GetBorderWidth(annot_dict);
  const float ending_w = border_w > 0 ? border_w : 1.0f;
  const bool has_stroke = GetStrokeColorString(annot_dict).has_value();
  RetainPtr<const CPDF_Array> interior_color = annot_dict->GetArrayFor("IC");
  const bool has_fill = interior_color && !interior_color->IsEmpty();

  // Lambda to emit a single ending with the correct transformation
  auto emit_one = [&](const CPDF_Annot::LineEnding ending,
                      const CFX_PointF& tip, const CFX_PointF& unit_dir) {
    if (ending == CPDF_Annot::LineEnding::kNone ||
        ending == CPDF_Annot::LineEnding::kUnknown) {
      return;
    }
    // Without a stroke colour only a filled closed ending paints.
    const bool fillable = ending == CPDF_Annot::LineEnding::kClosedArrow ||
                          ending == CPDF_Annot::LineEnding::kRClosedArrow ||
                          ending == CPDF_Annot::LineEnding::kCircle ||
                          ending == CPDF_Annot::LineEnding::kSquare ||
                          ending == CPDF_Annot::LineEnding::kDiamond;
    if (!has_stroke && !(fillable && has_fill)) {
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
          EmitArrowPath(ap, ending_w, ArrowStyle::kClosed, has_fill,
                        has_stroke);
          break;
        case CPDF_Annot::LineEnding::kOpenArrow:
        case CPDF_Annot::LineEnding::kROpenArrow:
          EmitArrowPath(ap, ending_w, ArrowStyle::kOpen, /*do_fill=*/false);
          break;
        case CPDF_Annot::LineEnding::kCircle:
          EmitCirclePath(ap, ending_w, has_fill, has_stroke);
          break;
        case CPDF_Annot::LineEnding::kSquare:
          EmitSquarePath(ap, ending_w, has_fill, has_stroke);
          break;
        case CPDF_Annot::LineEnding::kDiamond:
          EmitDiamondPath(ap, ending_w, has_fill, has_stroke);
          break;
        case CPDF_Annot::LineEnding::kButt:
          EmitButtOrSlashPath(ap, ending_w, kButtLenFactor);
          break;
        case CPDF_Annot::LineEnding::kSlash:
          EmitButtOrSlashPath(ap, ending_w, kSlashLenFactor);
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
  auto resources = GenerateResourcesDict(
      target->doc, GenerateExtGStateDict(*annot, blend_name),
      std::move(caption->fonts));
  return GenerateAndSetPointsAPDict(target, annot, ap, std::move(resources),
                                    points);
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
  const std::optional<ByteString> stroke_color = GetStrokeColorString(annot);
  auto interior = annot->GetArrayFor("IC");
  if (stroke_color) {
    ap << *stroke_color;
  }
  if (interior && !interior->IsEmpty()) {
    ap << GetColorStringWithDefault(interior.Get(), {}, PaintOperation::kFill);
  }
  auto segment = [&](CFX_PointF from, CFX_PointF to) {
    WritePoint(ap, from) << " m ";
    WritePoint(ap, to) << " l S\n";
  };
  // Without a stroke colour the lines go and the filled endings stay.
  if (border > 0 && stroke_color) {
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
  }
  if (border > 0) {
    GenerateLineEndings(ap, {line.start, line.end}, annot,
                        label.outside_arrows);
  }
  AppendDimensionCaption(ap, caption, label);
  auto resources = GenerateResourcesDict(
      target->doc, GenerateExtGStateDict(*annot, blend_name),
      std::move(caption.fonts));
  return GenerateAndSetPointsAPDict(target, annot, &ap, std::move(resources),
                                    points);
}

// An ink curve's number as Acrobat writes it: to four decimals, without
// trailing zeros (0.5, 527.6667).
void WriteInkNumber(fxcrt::ostringstream& stream, double value) {
  if (!std::isfinite(value)) {
    stream << "0";
    return;
  }
  ByteString text = ByteString::Format("%.4f", value);
  text.TrimBack('0');
  text.TrimBack('.');
  stream << (text == "-0" ? ByteString("0") : text);
}

void WriteInkPoint(fxcrt::ostringstream& stream, double x, double y) {
  WriteInkNumber(stream, x);
  stream << " ";
  WriteInkNumber(stream, y);
}

// One point of ink is Acrobat's dot: a circle of radius 0.5 about it, four
// quarter arcs from its top, clockwise, closed for the fill.
void WriteInkDot(fxcrt::ostringstream& stream, const CFX_PointF& center) {
  constexpr double kRadius = 0.5;
  // How far each arc's handles reach along its tangents, as Acrobat writes.
  constexpr double kHandle = 0.2778;
  const double x = center.x;
  const double y = center.y;
  WriteInkPoint(stream, x, y + kRadius);
  stream << " m\n";
  const double arcs[4][6] = {
      {x + kHandle, y + kRadius, x + kRadius, y + kHandle, x + kRadius, y},
      {x + kRadius, y - kHandle, x + kHandle, y - kRadius, x, y - kRadius},
      {x - kHandle, y - kRadius, x - kRadius, y - kHandle, x - kRadius, y},
      {x - kRadius, y + kHandle, x - kHandle, y + kRadius, x, y + kRadius}};
  for (const auto& arc : arcs) {
    WriteInkPoint(stream, arc[0], arc[1]);
    stream << " ";
    WriteInkPoint(stream, arc[2], arc[3]);
    stream << " ";
    WriteInkPoint(stream, arc[4], arc[5]);
    stream << " c\n";
  }
  stream << "h\n";
}

}  // namespace

namespace appearance_generator {

// Read one token from a /LE array and return it as a ByteString, accepting
// both Name and String objects (some generators are sloppy).
ByteString ReadLineEndingToken(const CPDF_Array* le, size_t idx) {
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

void EmitArrowPath(fxcrt::ostringstream& out,
                   float stroke_w,
                   ArrowStyle style,
                   bool do_fill,
                   bool stroked) {
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
      << (!stroked  ? "f\n"      // fill only
          : do_fill ? "b\n"      // fill + stroke
                    : "h S\n");  // just close and stroke (no fill)
}

void EmitCirclePath(fxcrt::ostringstream& out,
                    float stroke_w,
                    bool filled,
                    bool stroked) {
  const float r = (stroke_w * 5.0f) / 2.0f;
  // This is a standard approximation for drawing a circle with Bezier curves.
  constexpr float kL = 0.5523f;
  const float d = kL * r;

  out << r << " 0 m " << r << " " << d << " " << d << " " << r << " 0 " << r
      << " c " << -d << " " << r << " " << -r << " " << d << " " << -r
      << " 0 c " << -r << " " << -d << " " << -d << " " << -r << " 0 " << -r
      << " c " << d << " " << -r << " " << r << " " << -d << " " << r << " 0 c "
      << ClosedEndingOperator(filled, stroked);
}

void EmitSquarePath(fxcrt::ostringstream& out,
                    float stroke_w,
                    bool filled,
                    bool stroked) {
  const float h = (stroke_w * 6.0f) / 2.0f;
  out << -h << " " << -h << " m " << h << " " << -h << " l " << h << " " << h
      << " l " << -h << " " << h << " l h "
      << ClosedEndingOperator(filled, stroked);
}

void EmitDiamondPath(fxcrt::ostringstream& out,
                     float stroke_w,
                     bool filled,
                     bool stroked) {
  const float h = (stroke_w * 6.0f) / 2.0f;
  out << "0 " << -h << " m " << h << " 0 l "
      << "0 " << h << " l " << -h << " 0 l h "
      << ClosedEndingOperator(filled, stroked);
}

void EmitButtOrSlashPath(fxcrt::ostringstream& out,
                         float stroke_w,
                         float len_factor) {
  const float l = (stroke_w * len_factor) / 2.0f;
  out << -l << " 0 m " << l << " 0 l S\n";
}

bool GenerateCircleAP(APGenerationTarget* target,
                      CPDF_Dictionary* annot_dict,
                      const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  RetainPtr<const CPDF_Array> interior_color = annot_dict->GetArrayFor("IC");
  app_stream << GetColorStringWithDefault(interior_color.Get(),
                                          CFX_Color::MakeTransparent(),
                                          PaintOperation::kFill);

  const std::optional<ByteString> stroke_color =
      GetStrokeColorString(annot_dict);
  if (stroke_color) {
    app_stream << *stroke_color;
  }

  float border_width = GetBorderWidth(annot_dict);
  bool is_stroke_rect = stroke_color && border_width > 0;
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
    GenerateCloudyEllipsePath(app_stream, draw_rect, cloudy_info.intensity,
                              border_width);
  } else {
    // The outline's middle sits half its width inside /Rect, painted or not:
    // a fill without its stroke keeps the stroke's place, as Acrobat draws it.
    if (border_width > 0) {
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
  return GenerateAndSetBoxAPDict(target, annot_dict, &app_stream,
                                 std::move(resources_dict), rot_info.matrix,
                                 rot_info.bbox);
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
                                   CFX_Color::MakeTransparent(),
                                   PaintOperation::kFill);

  const std::optional<ByteString> stroke_color =
      GetStrokeColorString(annot_dict);
  if (stroke_color) {
    app << *stroke_color;
  }

  const float border_w = GetBorderWidth(annot_dict);
  const bool do_stroke = stroke_color && border_w > 0;
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

  std::vector<CFX_PointF> points;
  for (size_t i = 0; i + 1 < verts->size(); i += 2) {
    points.push_back({verts->GetFloatAt(i), verts->GetFloatAt(i + 1)});
  }
  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto res_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  return GenerateAndSetPointsAPDict(target, annot_dict, &app,
                                    std::move(res_dict), points);
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

  // Draw the main line segment. Width 0 is the thinnest line the device
  // draws (ISO 32000 8.4.3.2), the hairline Acrobat gives a line at /W 0.
  const std::optional<ByteString> stroke_color =
      GetStrokeColorString(annot_dict);
  if (stroke_color) {
    ap << *stroke_color << std::max(GetBorderWidth(annot_dict), 0.0f) << " w "
       << GetDashPatternString(annot_dict);
    ap << points[0].x << " " << points[0].y << " m " << points[1].x << " "
       << points[1].y << " l S\n";
  }

  // Draw the endings.
  GenerateLineEndings(ap, points, annot_dict);

  // Finalize and set the Appearance Stream.
  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto res_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  return GenerateAndSetPointsAPDict(target, annot_dict, &ap,
                                    std::move(res_dict), points);
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

  // Draw the main polyline path, a hairline at /W 0 as a line's is.
  const std::optional<ByteString> stroke_color =
      GetStrokeColorString(annot_dict);
  if (stroke_color) {
    ap << *stroke_color << std::max(GetBorderWidth(annot_dict), 0.0f) << " w "
       << GetDashPatternString(annot_dict);
    ap << points[0].x << " " << points[0].y << " m ";
    for (size_t i = 1; i < points.size(); ++i) {
      ap << points[i].x << " " << points[i].y << " l ";
    }
    ap << "S\n";
  }

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
  return GenerateAndSetPointsAPDict(target, annot_dict, &ap,
                                    std::move(res_dict), points);
}

bool GenerateInkAP(APGenerationTarget* target,
                   CPDF_Dictionary* annot_dict,
                   const ByteString& blend_name) {
  RetainPtr<const CPDF_Array> ink_list = annot_dict->GetArrayFor("InkList");
  if (!ink_list || ink_list->IsEmpty()) {
    return false;
  }

  // Ink is all stroke: with no stroke colour it paints nothing but keeps its
  // points. Width 0 is a hairline, as Acrobat draws it.
  const std::optional<ByteString> stroke_color =
      GetStrokeColorString(annot_dict);

  // The stroke as Acrobat sets it: the colour, the width unless it is the
  // default 1, and round caps. No join: the curves meet smoothly.
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs\n";
  if (stroke_color) {
    app_stream << *stroke_color;
    const float width = std::max(GetBorderWidth(annot_dict), 0.0f);
    if (width != 1.0f) {
      WriteInkNumber(app_stream, width);
      app_stream << " w\n";
    }
    app_stream << "1 J\n";
    app_stream << GetDashPatternString(annot_dict);
  }

  // Each stroke is drawn as Acrobat draws it (cpdf_ink_curves.h): fitted
  // cubics, or a dot for a stroke of one point.
  std::vector<CFX_PointF> points;
  bool dot_fill_set = false;
  for (size_t i = 0; i < ink_list->size(); i++) {
    RetainPtr<const CPDF_Array> coordinates_array = ink_list->GetArrayAt(i);
    // An ink stroke needs at least one point to start.
    if (!coordinates_array || coordinates_array->size() < 2) {
      continue;
    }

    const size_t first = points.size();
    for (size_t j = 0; j + 1 < coordinates_array->size(); j += 2) {
      points.push_back({coordinates_array->GetFloatAt(j),
                        coordinates_array->GetFloatAt(j + 1)});
    }
    if (!stroke_color) {
      continue;
    }
    const pdfium::span<const CFX_PointF> stroke =
        pdfium::span<const CFX_PointF>(points).subspan(first);
    const std::vector<InkCubic> curves = FitInkStroke(stroke);
    if (curves.empty()) {
      // Acrobat fills the dot without setting a fill colour (black under a
      // stroke of width 1 or more covers it); ours is the stroke's colour, so
      // a thinner dot has no black middle.
      if (!dot_fill_set) {
        app_stream << GetColorStringWithDefault(
            annot_dict->GetArrayFor(pdfium::annotation::kC).Get(), {},
            PaintOperation::kFill);
        dot_fill_set = true;
      }
      WriteInkDot(app_stream, stroke.front());
      app_stream << "B\n";
      continue;
    }
    WriteInkPoint(app_stream, stroke.front().x, stroke.front().y);
    app_stream << " m\n";
    for (const InkCubic& curve : curves) {
      WriteInkPoint(app_stream, curve.control1.x, curve.control1.y);
      app_stream << " ";
      WriteInkPoint(app_stream, curve.control2.x, curve.control2.y);
      app_stream << " ";
      WriteInkPoint(app_stream, curve.end.x, curve.end.y);
      app_stream << " c\n";
    }
    app_stream << "S\n";
  }

  if (points.empty()) {
    return false;
  }
  // /Rect is what the strokes paint, measured on every redraw: it shrinks
  // with a thinner line as it grows with a wider one.
  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  return GenerateAndSetPointsAPDict(target, annot_dict, &app_stream,
                                    std::move(resources_dict), points);
}

bool GenerateSquareAP(APGenerationTarget* target,
                      CPDF_Dictionary* annot_dict,
                      const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  RetainPtr<const CPDF_Array> interior_color = annot_dict->GetArrayFor("IC");
  app_stream << GetColorStringWithDefault(interior_color.Get(),
                                          CFX_Color::MakeTransparent(),
                                          PaintOperation::kFill);

  const std::optional<ByteString> stroke_color =
      GetStrokeColorString(annot_dict);
  if (stroke_color) {
    app_stream << *stroke_color;
  }

  float border_width = GetBorderWidth(annot_dict);
  const bool is_stroke_rect = stroke_color && border_width > 0;
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
    GenerateCloudyRectanglePath(app_stream, draw_rect, cloudy_info.intensity,
                                border_width);
  } else {
    // The outline's middle sits half its width inside /Rect, painted or not:
    // a fill without its stroke keeps the stroke's place, as Acrobat draws it.
    if (border_width > 0) {
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
  return GenerateAndSetBoxAPDict(target, annot_dict, &app_stream,
                                 std::move(resources_dict), rot_info.matrix,
                                 rot_info.bbox);
}

}  // namespace appearance_generator
