// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_richtextwriter.h"

#include <cmath>
#include <optional>
#include <sstream>
#include <utility>

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_defaultappearance.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxge/cfx_color.h"

namespace {

constexpr wchar_t kApiVersion[] = L"EmbedPDF:1.0";

// Three decimals, trailing zeros trimmed: the appearance's number format.
ByteString Number(float value) {
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

WideString EscapeXml(const WideString& text) {
  WideString out;
  out.Reserve(text.GetLength());
  for (wchar_t ch : text) {
    switch (ch) {
      case L'&':
        out += L"&amp;";
        break;
      case L'<':
        out += L"&lt;";
        break;
      case L'>':
        out += L"&gt;";
        break;
      case L'"':
        out += L"&quot;";
        break;
      case L'\r':
        out += L"&#13;";
        break;
      default:
        out += ch;
    }
  }
  return out;
}

WideString HexColor(FX_ARGB color) {
  return WideString::Format(L"#%02X%02X%02X", FXARGB_R(color), FXARGB_G(color),
                            FXARGB_B(color));
}

WideString Points(float value) {
  return WideString::Format(L"%.1fpt", value);
}

WideString WeightName(int weight) {
  if (weight == 700) {
    return L"bold";
  }
  if (weight == 400) {
    return L"normal";
  }
  return WideString::Format(L"%d", weight);
}

// Families with a space are quoted, as Acrobat quotes 'Noto Sans'.
WideString FamilyValue(const WideString& family) {
  if (family.Contains(L' ')) {
    return L"'" + family + L"'";
  }
  return family;
}

WideString AlignName(CPDF_RichTextParagraphProps::Align align) {
  switch (align) {
    case CPDF_RichTextParagraphProps::Align::kLeft:
      return L"left";
    case CPDF_RichTextParagraphProps::Align::kCenter:
      return L"center";
    case CPDF_RichTextParagraphProps::Align::kRight:
      return L"right";
    case CPDF_RichTextParagraphProps::Align::kJustify:
      return L"justify";
  }
  return L"left";
}

WideString DecorationValue(uint8_t bits) {
  WideString out;
  auto add = [&](const wchar_t* name) {
    if (!out.IsEmpty()) {
      out += L" ";
    }
    out += name;
  };
  if (bits & CPDF_RichTextStyle::kUnderline) {
    add(L"underline");
  }
  if (bits & CPDF_RichTextStyle::kLineThrough) {
    add(L"line-through");
  }
  if (bits & CPDF_RichTextStyle::kWordUnderline) {
    add(L"word");
  }
  return out.IsEmpty() ? WideString(L"none") : out;
}

void AppendDeclaration(WideString* css,
                       const wchar_t* name,
                       const WideString& value) {
  if (!css->IsEmpty() && css->Back() != L';') {
    *css += L";";
  }
  *css += name;
  *css += L":";
  *css += value;
}

// Paragraph declarations that differ from the body's paragraph defaults.
WideString ParagraphStyle(const CPDF_RichTextParagraphProps& props,
                          const CPDF_RichTextParagraphProps& body) {
  WideString css;
  if (props.align != body.align) {
    AppendDeclaration(&css, L"text-align", AlignName(props.align));
  }
  if (props.margin_top != body.margin_top) {
    AppendDeclaration(&css, L"margin-top", Points(props.margin_top));
  }
  if (props.margin_bottom != body.margin_bottom) {
    AppendDeclaration(&css, L"margin-bottom", Points(props.margin_bottom));
  }
  if (props.margin_left != body.margin_left) {
    AppendDeclaration(&css, L"margin-left", Points(props.margin_left));
  }
  if (props.margin_right != body.margin_right) {
    AppendDeclaration(&css, L"margin-right", Points(props.margin_right));
  }
  if (props.text_indent != body.text_indent) {
    AppendDeclaration(&css, L"text-indent", Points(props.text_indent));
  }
  if (props.line_height != body.line_height && props.line_height.has_value()) {
    AppendDeclaration(&css, L"line-height", Points(*props.line_height));
  }
  if (!props.unknown_declarations.IsEmpty()) {
    if (!css.IsEmpty() && css.Back() != L';') {
      css += L";";
    }
    css += props.unknown_declarations;
  }
  return css;
}

// A run's deltas in Acrobat's key order (C note §6).
WideString SpanStyle(const CPDF_RichTextStyleDelta& delta) {
  WideString css;
  if (delta.decoration) {
    AppendDeclaration(&css, L"text-decoration",
                      DecorationValue(*delta.decoration));
  }
  if (delta.color) {
    AppendDeclaration(&css, L"color", HexColor(*delta.color));
  }
  if (delta.weight) {
    AppendDeclaration(&css, L"font-weight", WeightName(*delta.weight));
  }
  if (delta.italic) {
    AppendDeclaration(&css, L"font-style",
                      *delta.italic ? L"italic" : L"normal");
  }
  if (delta.size) {
    AppendDeclaration(&css, L"font-size", Points(*delta.size));
  }
  if (delta.family) {
    AppendDeclaration(&css, L"font-family", FamilyValue(*delta.family));
  }
  if (delta.script) {
    switch (*delta.script) {
      case CPDF_RichTextStyle::Script::kNormal:
        AppendDeclaration(&css, L"vertical-align", L"0.0pt");
        break;
      case CPDF_RichTextStyle::Script::kSuper:
        AppendDeclaration(&css, L"vertical-align", L"+0.0pt");
        break;
      case CPDF_RichTextStyle::Script::kSub:
        AppendDeclaration(&css, L"vertical-align", L"-0.0pt");
        break;
    }
  }
  if (delta.letter_spacing) {
    AppendDeclaration(&css, L"letter-spacing", Points(*delta.letter_spacing));
  }
  if (delta.horz_scale) {
    AppendDeclaration(
        &css, L"xfa-font-horizontal-scale",
        WideString::Format(L"%d",
                           static_cast<int>(*delta.horz_scale * 100 + 0.5f)));
  }
  if (!delta.unknown_declarations.IsEmpty()) {
    if (!css.IsEmpty() && css.Back() != L';') {
      css += L";";
    }
    css += delta.unknown_declarations;
  }
  return css;
}

}  // namespace

CPDF_RichTextWriter::Prepared::Prepared() = default;

CPDF_RichTextWriter::Prepared::Prepared(Prepared&& that) noexcept = default;

CPDF_RichTextWriter::Prepared& CPDF_RichTextWriter::Prepared::operator=(
    Prepared&& that) noexcept = default;

CPDF_RichTextWriter::Prepared::~Prepared() = default;

// static
WideString CPDF_RichTextWriter::SerializeRC(
    const CPDF_RichTextDocument& document) {
  const CPDF_RichTextStyle& body = document.body;
  WideString body_css;
  AppendDeclaration(&body_css, L"font-size", Points(body.size));
  AppendDeclaration(&body_css, L"text-align",
                    AlignName(document.body_paragraph.align));
  AppendDeclaration(&body_css, L"color", HexColor(body.color));
  AppendDeclaration(&body_css, L"font-weight", WeightName(body.weight));
  AppendDeclaration(&body_css, L"font-style",
                    body.italic ? L"italic" : L"normal");
  AppendDeclaration(&body_css, L"font-family", FamilyValue(body.family));
  AppendDeclaration(&body_css, L"font-stretch", L"normal");
  if (body.decoration) {
    AppendDeclaration(&body_css, L"text-decoration",
                      DecorationValue(body.decoration));
  }
  if (body.letter_spacing != 0) {
    AppendDeclaration(&body_css, L"letter-spacing",
                      Points(body.letter_spacing));
  }
  if (body.horz_scale != 1.0f) {
    AppendDeclaration(
        &body_css, L"xfa-font-horizontal-scale",
        WideString::Format(L"%d",
                           static_cast<int>(body.horz_scale * 100 + 0.5f)));
  }
  const CPDF_RichTextParagraphProps defaults;
  const WideString paragraph_css =
      ParagraphStyle(document.body_paragraph, defaults);
  // text-align is already in the body style; the rest of the paragraph
  // defaults follow it.
  WideString extra_paragraph;
  {
    CPDF_RichTextParagraphProps without_align = document.body_paragraph;
    without_align.align = defaults.align;
    extra_paragraph = ParagraphStyle(without_align, defaults);
  }
  if (!extra_paragraph.IsEmpty()) {
    body_css += L";" + extra_paragraph;
  }

  WideString xml =
      L"<?xml version=\"1.0\"?><body xmlns=\"http://www.w3.org/1999/xhtml\" "
      L"xmlns:xfa=\"http://www.xfa.org/schema/xfa-data/1.0/\" "
      L"xfa:APIVersion=\"";
  xml += kApiVersion;
  xml += L"\" xfa:spec=\"2.0.2\" style=\"" + EscapeXml(body_css) + L"\">";
  for (const CPDF_RichTextParagraph& paragraph : document.paragraphs) {
    xml += L"<p dir=\"";
    xml += paragraph.props.rtl ? L"rtl" : L"ltr";
    xml += L"\"";
    const WideString css =
        ParagraphStyle(paragraph.props, document.body_paragraph);
    if (!css.IsEmpty()) {
      xml += L" style=\"" + EscapeXml(css) + L"\"";
    }
    xml += L">";
    for (const CPDF_RichTextRun& run : paragraph.runs) {
      const WideString span_css = SpanStyle(run.style);
      if (span_css.IsEmpty()) {
        xml += EscapeXml(run.text);
      } else {
        xml += L"<span style=\"" + EscapeXml(span_css) + L"\">" +
               EscapeXml(run.text) + L"</span>";
      }
    }
    xml += L"</p>";
  }
  xml += L"</body>";
  return xml;
}

// static
WideString CPDF_RichTextWriter::BuildDefaultStyle(
    const CPDF_RichTextDocument& document,
    FX_ARGB da_color) {
  // From the first run's resolved style (Acrobat's convention); the body
  // when there is none.
  CPDF_RichTextStyle style = document.body;
  for (const CPDF_RichTextParagraph& paragraph : document.paragraphs) {
    if (!paragraph.runs.empty()) {
      style = ApplyRichTextStyleDelta(document.body, paragraph.runs[0].style);
      break;
    }
  }
  WideString ds = L"font: ";
  if (style.weight >= 600) {
    ds += L"bold ";
  }
  if (style.italic) {
    ds += L"italic ";
  }
  ds += FamilyValue(style.family) + L",sans-serif " + Points(style.size) +
        L"; text-align:" + AlignName(document.body_paragraph.align) +
        L"; color:" + HexColor(da_color);
  return ds;
}

// static
WideString CPDF_RichTextWriter::ProjectPlainText(
    const CPDF_RichTextDocument& document) {
  WideString text;
  bool first = true;
  for (const CPDF_RichTextParagraph& paragraph : document.paragraphs) {
    if (!first) {
      text += L'\r';
    }
    first = false;
    for (const CPDF_RichTextRun& run : paragraph.runs) {
      text += run.text;
    }
  }
  return text;
}

// static
void CPDF_RichTextWriter::FailAfterStagedFontsForTesting(int staged_count) {
  CPDF_AnnotFontMap::FailAfterStagedFontsForTesting(staged_count);
}

// static
bool CPDF_RichTextWriter::Prepare(CPDF_Document* doc,
                                  const CPDF_Dictionary* annot_dict,
                                  const CPDF_RichTextDocument& document,
                                  Prepared* out) {
  if (!doc || !annot_dict || !out) {
    return false;
  }
  // DA and border colour: the annotation's, unchanged (plan §4.7 keeps
  // `color` and `fontColor` distinct); black for a box that has none yet.
  const CPDF_Dictionary* root = doc->GetRoot();
  RetainPtr<const CPDF_Dictionary> acroform =
      root ? root->GetDictFor("AcroForm") : nullptr;
  CFX_Color da_color(CFX_Color::Type::kRGB, 0, 0, 0);
  {
    CPDF_DefaultAppearance da(annot_dict, acroform.Get());
    std::optional<CFX_Color> existing = da.GetColor();
    if (existing.has_value() &&
        existing->nColorType != CFX_Color::Type::kTransparent) {
      da_color = *existing;
    }
  }
  FX_ARGB da_argb = 0xFF000000;
  {
    const CFX_Color rgb = da_color.ConvertColorType(CFX_Color::Type::kRGB);
    da_argb = ArgbEncode(255, static_cast<int>(rgb.fColor1 * 255 + 0.5f),
                         static_cast<int>(rgb.fColor2 * 255 + 0.5f),
                         static_cast<int>(rgb.fColor3 * 255 + 0.5f));
  }

  CPDF_GenerateAP::RichFreeTextRequest request;
  request.document = &document;
  request.da_color = da_color;
  Prepared prepared;
  prepared.appearance =
      CPDF_GenerateAP::PrepareRichFreeTextAP(doc, annot_dict, request);
  if (!prepared.appearance) {
    return false;
  }
  // DA: the colour, then the alias and the size laid out with.
  const CFX_Color rgb = da_color.ConvertColorType(CFX_Color::Type::kRGB);
  prepared.da = ByteString::Format(
      "%s %s %s rg /%s %s Tf", Number(rgb.fColor1).c_str(),
      Number(rgb.fColor2).c_str(), Number(rgb.fColor3).c_str(),
      prepared.appearance->da_alias.c_str(),
      Number(prepared.appearance->body_size).c_str());
  CPDF_RichTextDocument sized = document;
  sized.body.size = prepared.appearance->body_size;
  prepared.ds = BuildDefaultStyle(sized, da_argb);
  prepared.rc = SerializeRC(sized);
  prepared.contents = ProjectPlainText(sized);
  prepared.degraded = prepared.appearance->degraded;
  *out = std::move(prepared);
  return true;
}

// static
void CPDF_RichTextWriter::Publish(CPDF_Document* doc,
                                  CPDF_Dictionary* annot_dict,
                                  Prepared prepared) {
  if (!doc || !annot_dict || !prepared.appearance) {
    return;
  }
  annot_dict->SetNewFor<CPDF_String>("DS", prepared.ds.AsStringView());
  annot_dict->SetNewFor<CPDF_String>("DA", prepared.da);
  annot_dict->SetNewFor<CPDF_String>("RC", prepared.rc.AsStringView());
  annot_dict->SetNewFor<CPDF_String>("Contents",
                                     prepared.contents.AsStringView());
  CPDF_GenerateAP::PublishRichFreeTextAP(doc, annot_dict,
                                         std::move(prepared.appearance));
}

// static
bool CPDF_RichTextWriter::Apply(CPDF_Document* doc,
                                CPDF_Dictionary* annot_dict,
                                const CPDF_RichTextDocument& document,
                                bool* degraded) {
  Prepared prepared;
  if (!Prepare(doc, annot_dict, document, &prepared)) {
    return false;
  }
  if (degraded) {
    *degraded = prepared.degraded;
  }
  Publish(doc, annot_dict, std::move(prepared));
  return true;
}
