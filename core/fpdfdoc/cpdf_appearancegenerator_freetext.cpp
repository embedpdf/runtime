// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

// EmbedPDF: the appearance generator's free text, plain and rich, and the /DA
// its font and colour setters write.
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
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_appearancegenerator_internal.h"
#include "core/fpdfdoc/cpdf_color_utils.h"
#include "core/fpdfdoc/cpdf_defaultappearance.h"
#include "core/fpdfdoc/cpdf_documentfonts.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fpdfdoc/cpdf_richtext.h"
#include "core/fpdfdoc/cpdf_richtextlayout.h"
#include "core/fpdfdoc/cpdf_richtextparser.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxcrt/stream_write_utils.h"
#include "core/fxcrt/utf16.h"
#include "core/fxge/cfx_fontregistry.h"

using appearance_generator::APGenerationTarget;
using appearance_generator::ArrowStyle;
using appearance_generator::BlendModeToPDFName;
using appearance_generator::BorderStyleInfo;
using appearance_generator::DaFontResolution;
using appearance_generator::DefaultAppearanceFontPart;
using appearance_generator::DefaultAppearanceInfo;
using appearance_generator::DefaultBlendModeFor;
using appearance_generator::EmitArrowPath;
using appearance_generator::EmitButtOrSlashPath;
using appearance_generator::EmitCirclePath;
using appearance_generator::EmitDiamondPath;
using appearance_generator::EmitEndingWithAngle;
using appearance_generator::EmitRichTextBody;
using appearance_generator::EmitSquarePath;
using appearance_generator::EnsureFontInAcroFormDR;
using appearance_generator::GenerateAndSetBoxAPDict;
using appearance_generator::GenerateBorderAP;
using appearance_generator::GenerateCircleAP;
using appearance_generator::GenerateColorAP;
using appearance_generator::GenerateEphemeralDefaultAcroFormDict;
using appearance_generator::GenerateExtGStateDict;
using appearance_generator::GenerateResourcesDict;
using appearance_generator::GenerateSquareAP;
using appearance_generator::GetBorderStyleInfo;
using appearance_generator::GetBorderWidth;
using appearance_generator::GetColorStringWithDefault;
using appearance_generator::GetDefaultAppearanceInfo;
using appearance_generator::GetFontFromDrFontDictOrGenerateFallback;
using appearance_generator::GetPaintOperatorString;
using appearance_generator::GetShapeRotationInfo;
using appearance_generator::GetVerticalAlign;
using appearance_generator::kButtLenFactor;
using appearance_generator::kGSDictName;
using appearance_generator::kSlashLenFactor;
using appearance_generator::PaintOperation;
using appearance_generator::ReadLineEndingToken;
using appearance_generator::ResolveDaFontForEphemeralTarget;
using appearance_generator::ResolveDaFontForPersistentTarget;
using appearance_generator::ShapeRotationInfo;
using appearance_generator::StringFromFontNameAndSize;
using appearance_generator::UnitVector;

namespace {

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

  // (b) The text box: /Rect less /RD, or our keys' box when turned. For a
  // callout the /EMBD_Metadata pair turns the TEXT BOX only, about its
  // middle. The /CL leader stays page-space, so the rotation is baked INLINE
  // (a `q cm … Q` around the box + text below), never as the form /Matrix.
  const ShapeRotationInfo box_rot = GetShapeRotationInfo(annot_dict);
  const CFX_FloatRect text_box = box_rot.bbox;

  // (c) Border width and colors.
  const float border_w = GetBorderWidth(annot_dict);

  // (d) Set fill (from /C, default transparent), stroke (from DA), and line
  // width. When /C is absent we emit nothing for the fill and pick a
  // stroke-only paint operator below, so the text box doesn't fall back to
  // PDF's default black fill. Mirrors GenerateCircleAP / GenerateSquareAP.
  auto color_array = annot_dict->GetArrayFor(pdfium::annotation::kC);
  appearance_stream << GetColorStringWithDefault(
      color_array.Get(), CFX_Color::MakeTransparent(), PaintOperation::kFill);
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

// The scalar whose first code unit is at |index|: on a 16-bit wchar_t
// platform a supplementary character is a surrogate pair, and its glyph's
// ToUnicode entry needs the character, not the high half.
uint32_t ScalarAt(const WideString& text, size_t index) {
  if (index >= text.GetLength()) {
    return 0;
  }
  const uint32_t unit = static_cast<uint32_t>(text[index]);
  if constexpr (sizeof(wchar_t) == 2) {
    if (pdfium::IsHighSurrogate(unit) && index + 1 < text.GetLength()) {
      const uint32_t low = static_cast<uint32_t>(text[index + 1]);
      if (pdfium::IsLowSurrogate(low)) {
        return pdfium::SurrogatePair(static_cast<char16_t>(unit),
                                     static_cast<char16_t>(low))
            .ToCodePoint();
      }
    }
  }
  return unit;
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

struct RichFreeTextInputs {
  const CPDF_RichTextDocument* document = nullptr;
  CFX_Color da_color;
  RetainPtr<CPDF_Font> default_font;  // the /DA font as found in /DR
  ByteString font_name;               // its alias
  CFX_FontRegistry::FontId registered_font_id =
      CFX_FontRegistry::kInvalidFontId;
  // The /DA alias resolved to a reserved document program, if it did.
  DaFontResolution da_font;
  bool persistent = true;
  bool author_default_appearance = false;  // writer: body face -> /DA
};

std::unique_ptr<CPDF_AppearanceGenerator::PreparedRichFreeTextAP>
PrepareRichFreeTextAPInternal(CPDF_Document* doc,
                              const CPDF_Dictionary* annot_dict,
                              const ByteString& blend_name,
                              RichFreeTextInputs in) {
  if (!doc || !annot_dict || !in.document) {
    return nullptr;
  }
  auto prepared =
      std::make_unique<CPDF_AppearanceGenerator::PreparedRichFreeTextAP>();
  prepared->fonts = std::make_unique<CPDF_AnnotFontMap>(
      doc, std::move(in.default_font), in.font_name,
      /*allow_registered_fallbacks=*/true, in.registered_font_id,
      /*install_dr_entry=*/in.persistent,
      CPDF_AnnotFontMap::Owner::kAnnotation);
  CPDF_AnnotFontMap& map = *prepared->fonts;
  in.da_font.ApplyTo(&map);
  if (!in.author_default_appearance &&
      in.document->source == CPDF_RichTextDocument::Source::kContents &&
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
    // The leader is drawn on the page as it is: the form stays upright, and
    // the shape it's drawn about is where the turned text box reaches.
    prepared->footprint =
        inline_rotation ? envelope.box_rot.matrix.TransformRect(box) : box;
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
    prepared->matrix = rot_info.matrix;
    prepared->footprint = rot_info.bbox;
  }
  // An empty BOX is invalid input (nothing to draw at all); a valid box whose
  // border swallowed its plate still paints its envelope above — the text
  // has nowhere to go and lays out nothing. A /Rect authored upside down
  // (top below bottom) is a normal box, as it always was for CPVT.
  box.Normalize();
  if (box.Width() <= 0 || box.Height() <= 0) {
    return nullptr;
  }
  std::optional<CPDF_RichTextLayout::Result> arranged;
  if (text_area.Width() > 0 && text_area.Height() > 0) {
    CPDF_RichTextLayout::Options options;
    options.typographic_features = doc->GetTypographicFeaturesEnabled();
    CPDF_RichTextLayout layout(&map, options);
    arranged =
        layout.Arrange(*in.document, text_area, GetVerticalAlign(annot_dict));
    prepared->body_size = arranged->body_size;
    prepared->degraded = arranged->degraded;
    prepared->auto_size_fell_back = arranged->auto_size_fell_back;
  }
  if (in.author_default_appearance) {
    // The /DA font is the body's face as the layout resolved it over the
    // body's own text (cached by request), so /DA and the appearance never
    // disagree when coverage substitutes; with no plate to lay out, the
    // request resolves here without text. Aliases are settled before the
    // content names them.
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
  }
  if (arranged.has_value()) {
    stream << "/Tx BMC\nq\n";
    WriteRect(stream, text_area) << " re W n\n";
    EmitRichTextBody(stream, *arranged, map, text_area);
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
    std::unique_ptr<CPDF_AppearanceGenerator::PreparedRichFreeTextAP>
        prepared) {
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
  return GenerateAndSetBoxAPDict(target, annot_dict, &stream,
                                 std::move(resources_dict), prepared->matrix,
                                 prepared->footprint);
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
  in.da_font = da_font;
  in.persistent = target->IsPersistent();
  std::unique_ptr<CPDF_AppearanceGenerator::PreparedRichFreeTextAP> prepared =
      PrepareRichFreeTextAPInternal(doc, annot_dict, blend_name, std::move(in));
  if (!prepared) {
    return false;
  }
  return PublishRichFreeTextAPToTarget(target, annot_dict, std::move(prepared));
}

// The runs of |layout|, inside a text object: relative Td per string, state
// operators only when they change, ActualText around runs whose glyphs do
// not spell their text. A FreeText writes a string per word in its runs'
// colours, as Acrobat does, and collects its decorations; a form field
// writes a string per run in the colour set before it, as form fillers do.
void EmitTextRuns(fxcrt::ostringstream& s,
                  const CPDF_RichTextLayout::Result& layout,
                  CPDF_AnnotFontMap& map,
                  const CFX_FloatRect& area,
                  bool field,
                  std::vector<DecorationSegment>* decorations) {
  using GlyphRun = CPDF_RichTextLayout::GlyphRun;
  using ShapedGlyph = CPDF_RichTextLayout::ShapedGlyph;

  int cur_entry = -1;
  float cur_size = -1;
  std::optional<FX_ARGB> cur_color;
  float cur_tc = 0;
  float cur_ts = 0;
  float cur_tz = 100;
  CFX_PointF origin(0, 0);  // the last Td, in the form's space

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
      if (!field && (!cur_color.has_value() || *cur_color != run.style.color)) {
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
        const uint32_t unicode = ScalarAt(run.text, glyph.cluster);
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
                ? (code ? face.pdf_font->GetCharWidth(code) : 0)
                : (face.program ? face.program->GetGlyphWidth(glyph.gid) : 0);
        return (width * size / 1000.0f + run.style.letter_spacing) * scale;
      };

      // Words: a space ends the word it follows, as in Acrobat's streams;
      // a field's run is one string. Run positions are relative to the text
      // area's left edge.
      const float run_left = area.left + run.x;
      float x = run_left;
      size_t i = 0;
      const size_t count = run.glyphs.size();
      while (i < count) {
        size_t j = field ? count : i;
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
      if (decoration && decorations) {
        const float run_baseline = baseline + run.rise;
        const float descent = face.descent * size;
        const float ascent = face.ascent * size;
        if (decoration & CPDF_RichTextStyle::kUnderline) {
          decorations->push_back({run_left, run_left + run.width,
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
                decorations->push_back({word_start, wx,
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
            decorations->push_back({word_start, wx,
                                    run_baseline - 0.44f * descent,
                                    0.16f * descent, run.style.color});
          }
        }
        if (decoration & CPDF_RichTextStyle::kLineThrough) {
          decorations->push_back({run_left, run_left + run.width,
                                  run_baseline + 0.39f * ascent, 0.04f * ascent,
                                  run.style.color});
        }
      }
    }
  }
}

}  // namespace

namespace appearance_generator {

void EmitRichTextBody(fxcrt::ostringstream& s,
                      const CPDF_RichTextLayout::Result& layout,
                      CPDF_AnnotFontMap& map,
                      const CFX_FloatRect& area) {
  std::vector<DecorationSegment> decorations;
  s << "BT\n";
  EmitTextRuns(s, layout, map, area, /*field=*/false, &decorations);
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

void EmitFieldTextBody(fxcrt::ostringstream& s,
                       const CPDF_RichTextLayout::Result& layout,
                       CPDF_AnnotFontMap& map,
                       const CFX_FloatRect& plate) {
  EmitTextRuns(s, layout, map, plate, /*field=*/true, /*decorations=*/nullptr);
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
      da_font.registered_font_id == CFX_FontRegistry::kInvalidFontId &&
      !da_font.document_program) {
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

}  // namespace appearance_generator

// static
bool CPDF_AppearanceGenerator::GenerateDefaultAppearanceWithColor(
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

bool CPDF_AppearanceGenerator::UpdateDefaultAppearance(
    CPDF_Document* doc,
    CPDF_Dictionary* annot_dict,
    CPDF_Annot::StandardFont font,
    float font_size,
    const CFX_Color& color) {
  ByteString resource_key;

  // When font is kUnknown, preserve the font resource key the /DA in effect
  // names (the annotation's own, its field's, or the form's) instead of
  // failing. This allows updating fontSize and fontColor without replacing
  // the original font.
  if (font == CPDF_Annot::StandardFont::kUnknown) {
    const CPDF_Dictionary* root = doc->GetRoot();
    RetainPtr<const CPDF_Dictionary> acroform =
        root ? root->GetDictFor("AcroForm") : nullptr;
    CPDF_DefaultAppearance current_da(annot_dict, acroform.Get());
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

  ByteString da_font_part = DefaultAppearanceFontPart(resource_key, font_size);
  if (da_font_part.IsEmpty()) {
    return false;
  }
  ByteString da_color_part = GenerateColorAP(color, PaintOperation::kFill);
  // EmbedPDF: Strip trailing newlines and write color before font.
  // See comment in GenerateDefaultAppearanceWithColor for rationale.
  da_color_part.TrimBack('\n');

  annot_dict->SetNewFor<CPDF_String>("DA", da_color_part + " " + da_font_part);
  return true;
}

bool CPDF_AppearanceGenerator::UpdateDefaultAppearanceRegisteredFont(
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

  ByteString da_font_part = DefaultAppearanceFontPart(resource_key, font_size);
  if (da_font_part.IsEmpty()) {
    return false;
  }
  ByteString da_color_part = GenerateColorAP(color, PaintOperation::kFill);
  da_color_part.TrimBack('\n');

  annot_dict->SetNewFor<CPDF_String>("DA", da_color_part + " " + da_font_part);
  return true;
}

bool CPDF_AppearanceGenerator::UpdateDefaultAppearanceDocumentFont(
    CPDF_Document* doc,
    CPDF_Dictionary* annot_dict,
    uint32_t program_objnum,
    float font_size,
    const CFX_Color& color) {
  if (!doc || !annot_dict || program_objnum == 0) {
    return false;
  }
  CPDF_DocumentFonts* fonts = CPDF_DocumentFonts::For(doc);
  // The program must be one the walk found, and one the layout can author
  // with (the same test the font map applies before it writes with one).
  bool can_author = false;
  for (const CPDF_DocumentFonts::Face& face : fonts->CollectFaces()) {
    for (const CPDF_DocumentFonts::Program& program : face.programs) {
      if (program.objnum != program_objnum) {
        continue;
      }
      can_author = program.maps_unicode && program.permission.has_value() &&
                   (*program.permission ==
                        CFX_FontRegistry::EmbeddingPermission::kInstallable ||
                    *program.permission ==
                        CFX_FontRegistry::EmbeddingPermission::kEditable);
    }
  }
  ByteString resource_key;
  if (!can_author ||
      !fonts->ReserveProgramAlias(program_objnum, &resource_key)) {
    return false;
  }
  ByteString da_font_part = DefaultAppearanceFontPart(resource_key, font_size);
  if (da_font_part.IsEmpty()) {
    return false;
  }
  ByteString da_color_part = GenerateColorAP(color, PaintOperation::kFill);
  da_color_part.TrimBack('\n');

  annot_dict->SetNewFor<CPDF_String>("DA", da_color_part + " " + da_font_part);
  return true;
}

CPDF_AppearanceGenerator::PreparedRichFreeTextAP::PreparedRichFreeTextAP() =
    default;

CPDF_AppearanceGenerator::PreparedRichFreeTextAP::PreparedRichFreeTextAP(
    PreparedRichFreeTextAP&& that) noexcept = default;

CPDF_AppearanceGenerator::PreparedRichFreeTextAP&
CPDF_AppearanceGenerator::PreparedRichFreeTextAP::operator=(
    PreparedRichFreeTextAP&& that) noexcept = default;

CPDF_AppearanceGenerator::PreparedRichFreeTextAP::~PreparedRichFreeTextAP() =
    default;

// static
std::unique_ptr<CPDF_AppearanceGenerator::PreparedRichFreeTextAP>
CPDF_AppearanceGenerator::PrepareRichFreeTextAP(
    CPDF_Document* doc,
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
bool CPDF_AppearanceGenerator::PublishRichFreeTextAP(
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
