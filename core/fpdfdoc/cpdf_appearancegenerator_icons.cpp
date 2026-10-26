// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

// EmbedPDF: the appearance generator's icons and marks: note, file attachment,
// pop-up, link and caret.
//
// See docs/embedpdf/fork-ledger.md, rule own-appearance-generator. EmbedPDF's
// changes are Copyright 2026 CloudPDF LTD.

#include <sstream>
#include <utility>

#include "constants/annotation_common.h"
#include "constants/form_fields.h"
#include "core/fxcrt/stream_write_utils.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_appearancegenerator_internal.h"
#include "core/fpdfdoc/cpdf_color_utils.h"
#include "core/fpdfdoc/cpvt_fontmap.h"
#include "core/fpdfdoc/cpvt_variabletext.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/notreached.h"
#include "core/fxge/cfx_renderdevice.h"

using appearance_generator::APGenerationTarget;
using appearance_generator::BorderStyleInfo;
using appearance_generator::GenerateAndSetAPDict;
using appearance_generator::GenerateAndSetBoxAPDict;
using appearance_generator::GenerateCircleAP;
using appearance_generator::GenerateColorAP;
using appearance_generator::GenerateEditAP;
using appearance_generator::GenerateExtGStateDict;
using appearance_generator::GenerateFallbackFontDict;
using appearance_generator::GenerateResourceFontDict;
using appearance_generator::GenerateResourcesDict;
using appearance_generator::GetBorderStyleInfo;
using appearance_generator::GetBorderWidth;
using appearance_generator::GetColorStringWithDefault;
using appearance_generator::GetDashPatternString;
using appearance_generator::GetShapeRotationInfo;
using appearance_generator::kGSDictName;
using appearance_generator::PaintOperation;
using appearance_generator::ShapeRotationInfo;

namespace {

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

  ByteString color =
      GenerateColorAP(CFX_Color::MakeRGB(0, 0, 0), PaintOperation::kFill);

  return ByteString{"BT\n", color.AsStringView(), content.AsStringView(),
                    "ET\n", "Q\n"};
}

// The square the note and file icons are designed in. An icon is drawn in
// it, then scaled onto its annotation's /Rect.
constexpr CFX_FloatRect kIconDesign(0, 0, 20, 20);

// The matrix that puts the icon's design square onto `rect`, each axis on its
// own: a rect that isn't square stretches the icon, as it does in every PDF
// app. WriteMatrix keeps every number in PDF's number syntax.
ByteString IconDesignOnto(CFX_FloatRect rect) {
  rect.Normalize();
  fxcrt::ostringstream out;
  WriteMatrix(out, CFX_Matrix(rect.Width() / kIconDesign.Width(), 0, 0,
                              rect.Height() / kIconDesign.Height(), rect.left,
                              rect.bottom))
      << " cm\n";
  return ByteString(out);
}

void AppendEllipsePath(fxcrt::ostringstream& app_stream,
                       const CFX_FloatRect& bounds);

// Appends the glyph of a note icon other than Comment, drawn in the icon's
// 20x20 design square: the fill and stroke colours are already set.
void AppendNoteGlyph(fxcrt::ostringstream& app_stream,
                     const CFX_FloatRect& rect,
                     CPDF_Annot::Icon icon,
                     const CFX_Color& stroke_color,
                     float half_width) {
  CFX_FloatRect box = rect;
  box.Deflate(half_width, half_width);
  auto at = [&](float x, float y) {
    return CFX_PointF(box.left + x / 20 * box.Width(),
                      box.bottom + y / 20 * box.Height());
  };
  auto move = [&](float x, float y) {
    WritePoint(app_stream, at(x, y)) << " m\n";
  };
  auto line = [&](float x, float y) {
    WritePoint(app_stream, at(x, y)) << " l\n";
  };
  auto curve = [&](float x1, float y1, float x2, float y2, float x3,
                   float y3) {
    WritePoint(app_stream, at(x1, y1)) << " ";
    WritePoint(app_stream, at(x2, y2)) << " ";
    WritePoint(app_stream, at(x3, y3)) << " c\n";
  };
  auto rectangle = [&](float x1, float y1, float x2, float y2) {
    move(x1, y1);
    line(x2, y1);
    line(x2, y2);
    line(x1, y2);
    app_stream << "h\n";
  };
  auto ellipse = [&](float x1, float y1, float x2, float y2) {
    const CFX_PointF low = at(x1, y1);
    const CFX_PointF high = at(x2, y2);
    AppendEllipsePath(app_stream, CFX_FloatRect(low.x, low.y, high.x, high.y));
  };

  switch (icon) {
    case CPDF_Annot::Icon::kText_Note: {
      // A page with its top right corner folded over, and three lines.
      move(3.5f, 1);
      line(3.5f, 19);
      line(12.5f, 19);
      line(16.5f, 15);
      line(16.5f, 1);
      app_stream << "h\nB\n";
      move(12.5f, 19);
      line(12.5f, 15);
      line(16.5f, 15);
      for (float y : {12.0f, 8.5f, 5.0f}) {
        move(6, y);
        line(14, y);
      }
      app_stream << "S\n";
      break;
    }
    case CPDF_Annot::Icon::kText_Help: {
      // A disc with a question mark in the contrast colour.
      ellipse(1, 1, 19, 19);
      app_stream << "B\n1 J\n1 j\n";
      WriteFloat(app_stream, 1.8f) << " w\n";
      move(7.2f, 12.8f);
      curve(7.2f, 15.2f, 8.6f, 16.2f, 10.2f, 16.2f);
      curve(11.9f, 16.2f, 13, 15.1f, 13, 13.6f);
      curve(13, 11.6f, 10.2f, 11.3f, 10.2f, 9);
      line(10.2f, 7.8f);
      app_stream << "S\n";
      app_stream << GenerateColorAP(stroke_color, PaintOperation::kFill);
      ellipse(9, 4, 11.4f, 6.4f);
      app_stream << "f\n";
      break;
    }
    case CPDF_Annot::Icon::kText_Key: {
      // Its ring (even-odd punches the hole), shaft and two teeth.
      ellipse(1.5f, 8.5f, 10.5f, 17.5f);
      ellipse(4.2f, 11.2f, 7.8f, 14.8f);
      rectangle(10.5f, 11.8f, 18.5f, 14.2f);
      rectangle(13.5f, 8.8f, 15, 11.8f);
      rectangle(16.5f, 9.8f, 18.5f, 11.8f);
      app_stream << "B*\n";
      break;
    }
    case CPDF_Annot::Icon::kText_NewParagraph: {
      // A triangle pointing up over a line: a new paragraph starts here.
      move(3, 8);
      line(17, 8);
      line(10, 18);
      app_stream << "h\n";
      rectangle(3, 2, 17, 5);
      app_stream << "B*\n";
      break;
    }
    case CPDF_Annot::Icon::kText_Paragraph: {
      // The pilcrow as one outline: two stems and the bowl on the left.
      move(15.5f, 18);
      line(15.5f, 2);
      line(13.7f, 2);
      line(13.7f, 16.2f);
      line(11.8f, 16.2f);
      line(11.8f, 2);
      line(10, 2);
      line(10, 9.6f);
      curve(6.6f, 9.6f, 4, 11.4f, 4, 13.8f);
      curve(4, 16.2f, 6.6f, 18, 10, 18);
      app_stream << "h\nB\n";
      break;
    }
    case CPDF_Annot::Icon::kText_Insert: {
      // A caret.
      move(2.5f, 3);
      line(10, 17);
      line(17.5f, 3);
      line(14.3f, 3);
      line(10, 11.2f);
      line(5.7f, 3);
      app_stream << "h\nB\n";
      break;
    }
    default:
      NOTREACHED();
  }
}

ByteString GenerateTextSymbolAP(const CFX_FloatRect& rect,
                                const CPDF_Dictionary& annot_dict) {
  fxcrt::ostringstream app_stream;

  // Read fill color from /C array; default to yellow.
  CFX_Color fill_color = CFX_Color::MakeRGB(1, 1, 0);
  RetainPtr<const CPDF_Array> color_array =
      annot_dict.GetArrayFor(pdfium::annotation::kC);
  if (color_array) {
    fill_color = fpdfdoc::CFXColorFromArray(*color_array);
  }

  // Compute luminance-based contrast stroke (matches JS
  // getContrastStrokeColor).
  float luminance = 0.299f * fill_color.fColor1 + 0.587f * fill_color.fColor2 +
                    0.114f * fill_color.fColor3;
  CFX_Color stroke_color = luminance < 0.45f ? CFX_Color::MakeRGB(1, 1, 1)
                                             : CFX_Color::MakeRGB(0, 0, 0);

  // /Name picks the glyph. Absent or foreign names mean Note, the
  // ISO 32000 default icon for text annotations.
  CPDF_Annot::Icon icon =
      CPDF_Annot::StringToIcon(annot_dict.GetNameFor("Name"));
  switch (icon) {
    case CPDF_Annot::Icon::kText_Comment:
    case CPDF_Annot::Icon::kText_Key:
    case CPDF_Annot::Icon::kText_Help:
    case CPDF_Annot::Icon::kText_NewParagraph:
    case CPDF_Annot::Icon::kText_Paragraph:
    case CPDF_Annot::Icon::kText_Insert:
      break;
    default:
      icon = CPDF_Annot::Icon::kText_Note;
      break;
  }

  app_stream << GenerateColorAP(fill_color, PaintOperation::kFill);
  app_stream << GenerateColorAP(stroke_color, PaintOperation::kStroke);

  static constexpr int kBorderWidth = 1;
  app_stream << kBorderWidth << " w\n";

  static constexpr float kHalfWidth = kBorderWidth / 2.0f;

  if (icon != CPDF_Annot::Icon::kText_Comment) {
    AppendNoteGlyph(app_stream, rect, icon, stroke_color, kHalfWidth);
    return ByteString(app_stream);
  }

  // Comment: a speech balloon with three lines of text.
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
  CFX_Color fill_color = CFX_Color::MakeRGB(1, 1, 0);
  RetainPtr<const CPDF_Array> color_array =
      annot_dict.GetArrayFor(pdfium::annotation::kC);
  if (color_array) {
    fill_color = fpdfdoc::CFXColorFromArray(*color_array);
  }

  // Same luminance-based contrast stroke as GenerateTextSymbolAP.
  float luminance = 0.299f * fill_color.fColor1 + 0.587f * fill_color.fColor2 +
                    0.114f * fill_color.fColor3;
  CFX_Color stroke_color = luminance < 0.45f ? CFX_Color::MakeRGB(1, 1, 1)
                                             : CFX_Color::MakeRGB(0, 0, 0);

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

}  // namespace

namespace appearance_generator {

bool GenerateTextAP(APGenerationTarget* target,
                    CPDF_Dictionary* annot_dict,
                    const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  // The note icon fills /Rect, as every PDF app draws an appearance onto
  // its /Rect: a bigger rect is a bigger icon.
  app_stream << IconDesignOnto(annot_dict->GetRectFor(pdfium::annotation::kRect));
  app_stream << GenerateTextSymbolAP(kIconDesign, *annot_dict);

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &app_stream,
                       std::move(resources_dict),
                       false /*IsTextMarkupAnnotation*/);
  return true;
}

bool GenerateFileAttachmentAP(APGenerationTarget* target,
                              CPDF_Dictionary* annot_dict,
                              const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  // Like the note icon, the file icon fills /Rect.
  app_stream << IconDesignOnto(annot_dict->GetRectFor(pdfium::annotation::kRect));
  app_stream << GenerateFileAttachmentSymbolAP(kIconDesign, *annot_dict);

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &app_stream,
                       std::move(resources_dict),
                       false /*IsTextMarkupAnnotation*/);
  return true;
}

bool GeneratePopupAP(CPDF_Document* doc,
                     CPDF_Dictionary* annot_dict,
                     const ByteString& blend_name) {
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs\n";

  app_stream << GenerateColorAP(CFX_Color::MakeRGB(1, 1, 0),
                                PaintOperation::kFill);
  app_stream << GenerateColorAP(CFX_Color::MakeRGB(0, 0, 0),
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

bool GenerateLinkAP(APGenerationTarget* target,
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
      CFX_Color::MakeRGB(0, 0, 1),  // Default: blue
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
  auto resources_dict =
      GenerateResourcesDict(target->doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(target, annot_dict, &app_stream,
                       std::move(resources_dict),
                       /*is_text_markup_annotation=*/false);
  return true;
}

bool GenerateCaretAP(APGenerationTarget* target,
                     CPDF_Dictionary* annot_dict,
                     const ByteString& blend_name) {
  CPDF_Document* doc = target->doc;
  fxcrt::ostringstream app_stream;
  app_stream << "/" << kGSDictName << " gs ";

  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color::MakeRGB(0, 0, 0), PaintOperation::kStroke);
  app_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color::MakeRGB(0, 0, 0), PaintOperation::kFill);

  // A caret anchored to rotated text carries the box-family /EMBD_Metadata
  // rotation pair: draw in the UNROTATED box and let the AP form matrix turn
  // the symbol onto its text's baseline (the shape/free-text machinery).
  const ShapeRotationInfo rot_info = GetShapeRotationInfo(annot_dict);
  CFX_FloatRect rect = rot_info.bbox;
  rect.Normalize();

  const float draw_left = rect.left;
  const float draw_bottom = rect.bottom;
  const float draw_right = rect.right;
  const float draw_top = rect.top;

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
  return GenerateAndSetBoxAPDict(target, annot_dict, &app_stream,
                                 std::move(resources_dict), rot_info.matrix,
                                 rot_info.bbox);
}

}  // namespace appearance_generator
