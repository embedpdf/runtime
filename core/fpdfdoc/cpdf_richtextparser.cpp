// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_richtextparser.h"

#include <math.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfdoc/cpdf_defaultappearance.h"
#include "core/fxcrt/cfx_read_only_span_stream.h"
#include "core/fxcrt/fx_string.h"
#include "core/fxcrt/xml/cfx_xmldocument.h"
#include "core/fxcrt/xml/cfx_xmlelement.h"
#include "core/fxcrt/xml/cfx_xmlnode.h"
#include "core/fxcrt/xml/cfx_xmlparser.h"
#include "core/fxcrt/xml/cfx_xmltext.h"
#include "core/fxge/cfx_color.h"

namespace {

using Align = CPDF_RichTextParagraphProps::Align;
using Script = CPDF_RichTextStyle::Script;
using Diagnostic = CPDF_RichTextDiagnostic;

constexpr wchar_t kWhitespace[] = L" \t\r\n";

WideString Trimmed(WideString text) {
  text.Trim(WideStringView(kWhitespace));
  return text;
}

WideString Lowered(WideString text) {
  text.MakeLower();
  return text;
}

void AddDiagnostic(std::vector<Diagnostic>* diagnostics,
                   Diagnostic::Code code,
                   const WideString& detail) {
  if (diagnostics) {
    diagnostics->push_back({code, detail.ToUTF8()});
  }
}

// ---- values ---------------------------------------------------------------

// "22pt", "22.0pt", "16px", "1.2em", "10" (pt). |em_base| resolves em.
std::optional<float> ParseLength(const WideString& raw, float em_base) {
  WideString value = Lowered(Trimmed(raw));
  if (value.IsEmpty()) {
    return std::nullopt;
  }
  float scale = 1.0f;
  size_t unit_start = value.GetLength();
  while (unit_start > 0 && !isdigit(value[unit_start - 1]) &&
         value[unit_start - 1] != L'.') {
    --unit_start;
  }
  WideString number = value.First(unit_start);
  WideString unit = value.Substr(unit_start);
  unit.Trim(WideStringView(kWhitespace));
  if (unit == L"pt" || unit.IsEmpty()) {
    scale = 1.0f;
  } else if (unit == L"px") {
    scale = 0.75f;
  } else if (unit == L"em") {
    scale = em_base;
  } else if (unit == L"in") {
    scale = 72.0f;
  } else if (unit == L"mm") {
    scale = 72.0f / 25.4f;
  } else if (unit == L"cm") {
    scale = 72.0f / 2.54f;
  } else if (unit == L"%") {
    scale = em_base / 100.0f;
  } else {
    return std::nullopt;
  }
  bool numeric = false;
  for (wchar_t ch : number) {
    if (isdigit(ch)) {
      numeric = true;
    } else if (ch != L'.' && ch != L'-' && ch != L'+') {
      return std::nullopt;
    }
  }
  if (!numeric) {
    return std::nullopt;
  }
  return StringToFloat(number.AsStringView()) * scale;
}

std::optional<int> HexNibble(wchar_t ch) {
  if (ch >= L'0' && ch <= L'9') {
    return ch - L'0';
  }
  if (ch >= L'a' && ch <= L'f') {
    return ch - L'a' + 10;
  }
  if (ch >= L'A' && ch <= L'F') {
    return ch - L'A' + 10;
  }
  return std::nullopt;
}

std::optional<FX_ARGB> ParseColor(const WideString& raw) {
  WideString value = Lowered(Trimmed(raw));
  if (value.IsEmpty()) {
    return std::nullopt;
  }
  if (value[0] == L'#') {
    WideString hex = value.Substr(1);
    if (hex.GetLength() == 3) {
      uint32_t rgb[3];
      for (size_t i = 0; i < 3; ++i) {
        std::optional<int> n = HexNibble(hex[i]);
        if (!n.has_value()) {
          return std::nullopt;
        }
        rgb[i] = static_cast<uint32_t>(*n * 17);
      }
      return ArgbEncode(255, rgb[0], rgb[1], rgb[2]);
    }
    if (hex.GetLength() == 6) {
      uint32_t rgb[3];
      for (size_t i = 0; i < 3; ++i) {
        std::optional<int> hi = HexNibble(hex[i * 2]);
        std::optional<int> lo = HexNibble(hex[i * 2 + 1]);
        if (!hi.has_value() || !lo.has_value()) {
          return std::nullopt;
        }
        rgb[i] = static_cast<uint32_t>(*hi * 16 + *lo);
      }
      return ArgbEncode(255, rgb[0], rgb[1], rgb[2]);
    }
    return std::nullopt;
  }
  if (value.First(4) == L"rgb(" && value.Back() == L')') {
    std::vector<WideString> parts =
        fxcrt::Split(value.Substr(4, value.GetLength() - 5), L',');
    if (parts.size() != 3) {
      return std::nullopt;
    }
    uint32_t rgb[3];
    for (size_t i = 0; i < 3; ++i) {
      WideString part = Trimmed(parts[i]);
      float f = StringToFloat(part.AsStringView());
      if (part.Back() == L'%') {
        f = f * 255.0f / 100.0f;
      }
      rgb[i] = static_cast<uint32_t>(std::clamp(f, 0.0f, 255.0f));
    }
    return ArgbEncode(255, rgb[0], rgb[1], rgb[2]);
  }
  struct Named {
    const wchar_t* name;
    uint32_t rgb;
  };
  static constexpr Named kNamed[] = {
      {L"black", 0x000000},   {L"white", 0xffffff},  {L"red", 0xff0000},
      {L"green", 0x008000},   {L"blue", 0x0000ff},   {L"yellow", 0xffff00},
      {L"gray", 0x808080},    {L"grey", 0x808080},   {L"silver", 0xc0c0c0},
      {L"maroon", 0x800000},  {L"olive", 0x808000},  {L"lime", 0x00ff00},
      {L"aqua", 0x00ffff},    {L"teal", 0x008080},   {L"navy", 0x000080},
      {L"fuchsia", 0xff00ff}, {L"purple", 0x800080}, {L"orange", 0xffa500},
  };
  for (const Named& named : kNamed) {
    if (value == named.name) {
      return 0xff000000u | named.rgb;
    }
  }
  return std::nullopt;
}

std::optional<int> ParseWeight(const WideString& raw) {
  WideString value = Lowered(Trimmed(raw));
  if (value == L"normal") {
    return 400;
  }
  if (value == L"bold") {
    return 700;
  }
  if (value == L"bolder") {
    return 700;
  }
  if (value == L"lighter") {
    return 300;
  }
  if (value.IsEmpty()) {
    return std::nullopt;
  }
  for (wchar_t ch : value) {
    if (!isdigit(ch)) {
      return std::nullopt;
    }
  }
  int weight = FXSYS_wtoi(value.c_str());
  if (weight < 1 || weight > 1000) {
    return std::nullopt;
  }
  return weight;
}

// The first family of a list, unquoted: "'Noto Sans',sans-serif" -> Noto Sans.
WideString FirstFamily(const WideString& raw) {
  WideString value = Trimmed(raw);
  WideString first;
  wchar_t quote = 0;
  for (wchar_t ch : value) {
    if (quote) {
      if (ch == quote) {
        break;
      }
      first += ch;
      continue;
    }
    if (ch == L'\'' || ch == L'"') {
      if (!Trimmed(first).IsEmpty()) {
        break;
      }
      quote = ch;
      continue;
    }
    if (ch == L',') {
      break;
    }
    first += ch;
  }
  return Trimmed(first);
}

std::optional<uint8_t> ParseDecoration(const WideString& raw) {
  uint8_t bits = 0;
  bool any = false;
  for (const WideString& token : fxcrt::Split(Lowered(raw), L' ')) {
    WideString t = Trimmed(token);
    if (t.IsEmpty()) {
      continue;
    }
    any = true;
    if (t == L"none") {
      bits = 0;
    } else if (t == L"underline") {
      bits |= CPDF_RichTextStyle::kUnderline;
    } else if (t == L"line-through") {
      bits |= CPDF_RichTextStyle::kLineThrough;
    } else if (t == L"word") {
      bits |= CPDF_RichTextStyle::kWordUnderline;
    } else if (t == L"overline") {
      // not representable; ignore
    } else {
      return std::nullopt;
    }
  }
  if (!any) {
    return std::nullopt;
  }
  return bits;
}

// sub | super | baseline | a length. Acrobat writes "+0.0pt" for superscript
// and "-0.0pt" for subscript: the sign is the information.
std::optional<Script> ParseScript(const WideString& raw) {
  WideString value = Lowered(Trimmed(raw));
  if (value == L"sub") {
    return Script::kSub;
  }
  if (value == L"super") {
    return Script::kSuper;
  }
  if (value == L"baseline" || value == L"middle" || value == L"top" ||
      value == L"bottom") {
    return Script::kNormal;
  }
  if (value.IsEmpty()) {
    return std::nullopt;
  }
  if (value[0] == L'+') {
    return Script::kSuper;
  }
  if (value[0] == L'-') {
    return Script::kSub;
  }
  std::optional<float> length = ParseLength(value, 1.0f);
  if (!length.has_value()) {
    return std::nullopt;
  }
  if (*length > 0) {
    return Script::kSuper;
  }
  if (*length < 0) {
    return Script::kSub;
  }
  return Script::kNormal;
}

std::optional<Align> ParseAlign(const WideString& raw) {
  WideString value = Lowered(Trimmed(raw));
  if (value == L"left" || value == L"start") {
    return Align::kLeft;
  }
  if (value == L"center" || value == L"centre") {
    return Align::kCenter;
  }
  if (value == L"right" || value == L"end") {
    return Align::kRight;
  }
  if (value == L"justify") {
    return Align::kJustify;
  }
  return std::nullopt;
}

void AppendUnknownDeclaration(WideString* unknown,
                              const WideString& name,
                              const WideString& value) {
  if (!unknown->IsEmpty()) {
    *unknown += L";";
  }
  *unknown += name + L":" + value;
}

// "k:v; k:v" -> pairs, quotes respected only in that a ';' inside quotes does
// not split (Acrobat never quotes a ';', but font names may contain them).
std::vector<std::pair<WideString, WideString>> SplitDeclarations(
    const WideString& css) {
  std::vector<std::pair<WideString, WideString>> result;
  WideString current;
  wchar_t quote = 0;
  auto flush = [&]() {
    std::optional<size_t> colon = current.Find(L':');
    if (colon.has_value()) {
      WideString name = Lowered(Trimmed(current.First(colon.value())));
      WideString value = Trimmed(current.Substr(colon.value() + 1));
      if (!name.IsEmpty()) {
        result.emplace_back(name, value);
      }
    }
    current.clear();
  };
  for (wchar_t ch : css) {
    if (quote) {
      if (ch == quote) {
        quote = 0;
      }
      current += ch;
      continue;
    }
    if (ch == L'\'' || ch == L'"') {
      quote = ch;
      current += ch;
      continue;
    }
    if (ch == L';') {
      flush();
      continue;
    }
    current += ch;
  }
  flush();
  return result;
}

// Whether a property is a paragraph property (goes to the paragraph) or a
// character property (goes to the style delta). Returns true when consumed.
// Part of the XFA rich text vocabulary but without effect on our layout.
// Kept verbatim (so a rewrite does not drop it) and not reported: Acrobat
// writes font-stretch on every body style.
bool IsRecognisedUnmodelled(const WideString& name) {
  return name == L"font-stretch" || name == L"xfa-spacerun" ||
         name == L"xfa-tab-stops" || name == L"xfa-tab-count" ||
         name == L"kerning-mode" || name == L"font-variant" ||
         name == L"xfa-font-vertical-scale";
}

bool ApplyDeclaration(const WideString& name,
                      const WideString& value,
                      float em_base,
                      CPDF_RichTextStyleDelta* style,
                      CPDF_RichTextParagraphProps* paragraph,
                      std::vector<Diagnostic>* diagnostics) {
  auto invalid = [&]() {
    AddDiagnostic(diagnostics, Diagnostic::Code::kInvalidValue,
                  name + L":" + value);
    return true;
  };
  if (name == L"font-family") {
    WideString family = FirstFamily(value);
    if (family.IsEmpty()) {
      return invalid();
    }
    style->family = family;
    return true;
  }
  if (name == L"font-size") {
    std::optional<float> size = ParseLength(value, em_base);
    if (!size.has_value() || *size < 0) {
      return invalid();
    }
    style->size = *size;
    return true;
  }
  if (name == L"font-weight") {
    std::optional<int> weight = ParseWeight(value);
    if (!weight.has_value()) {
      return invalid();
    }
    style->weight = *weight;
    return true;
  }
  if (name == L"font-style") {
    WideString v = Lowered(Trimmed(value));
    if (v == L"italic" || v == L"oblique") {
      style->italic = true;
    } else if (v == L"normal") {
      style->italic = false;
    } else {
      return invalid();
    }
    return true;
  }
  if (name == L"color") {
    std::optional<FX_ARGB> color = ParseColor(value);
    if (!color.has_value()) {
      return invalid();
    }
    style->color = *color;
    return true;
  }
  if (name == L"text-decoration") {
    std::optional<uint8_t> decoration = ParseDecoration(value);
    if (!decoration.has_value()) {
      return invalid();
    }
    style->decoration = *decoration;
    return true;
  }
  if (name == L"vertical-align") {
    std::optional<Script> script = ParseScript(value);
    if (!script.has_value()) {
      return invalid();
    }
    style->script = *script;
    return true;
  }
  if (name == L"letter-spacing") {
    WideString v = Lowered(Trimmed(value));
    if (v == L"normal") {
      style->letter_spacing = 0.0f;
      return true;
    }
    std::optional<float> spacing = ParseLength(value, em_base);
    if (!spacing.has_value()) {
      return invalid();
    }
    style->letter_spacing = *spacing;
    return true;
  }
  if (name == L"xfa-font-horizontal-scale") {
    WideString v = Trimmed(value);
    if (v.Back() == L'%') {
      v = v.First(v.GetLength() - 1);
    }
    float percent = StringToFloat(v.AsStringView());
    if (percent <= 0) {
      return invalid();
    }
    style->horz_scale = percent / 100.0f;
    return true;
  }
  // Paragraph properties.
  if (name == L"text-align") {
    std::optional<Align> align = ParseAlign(value);
    if (!align.has_value()) {
      return invalid();
    }
    if (paragraph) {
      paragraph->align = *align;
    }
    return true;
  }
  if (name == L"line-height") {
    if (!paragraph) {
      return true;
    }
    WideString v = Lowered(Trimmed(value));
    if (v == L"normal") {
      paragraph->line_height.reset();
      return true;
    }
    bool unitless = !v.IsEmpty();
    for (wchar_t ch : v) {
      if (!isdigit(ch) && ch != L'.') {
        unitless = false;
      }
    }
    if (unitless) {
      paragraph->line_height = StringToFloat(v.AsStringView()) * em_base;
      return true;
    }
    std::optional<float> height = ParseLength(value, em_base);
    if (!height.has_value()) {
      return invalid();
    }
    paragraph->line_height = *height;
    return true;
  }
  if (name == L"margin-top" || name == L"margin-bottom" ||
      name == L"margin-left" || name == L"margin-right" ||
      name == L"text-indent") {
    std::optional<float> length = ParseLength(value, em_base);
    if (!length.has_value()) {
      return invalid();
    }
    if (paragraph) {
      if (name == L"margin-top") {
        paragraph->margin_top = *length;
      } else if (name == L"margin-bottom") {
        paragraph->margin_bottom = *length;
      } else if (name == L"margin-left") {
        paragraph->margin_left = *length;
      } else if (name == L"margin-right") {
        paragraph->margin_right = *length;
      } else {
        paragraph->text_indent = *length;
      }
    }
    return true;
  }
  return false;
}

// The `font:` shorthand as Acrobat writes it in /DS:
//   font: [bold] [italic] 'Noto Sans',sans-serif 22.0pt
// The size is the last token; keywords come first; the family list is what
// remains in between.
void ApplyFontShorthand(const WideString& value,
                        float em_base,
                        CPDF_RichTextStyleDelta* style,
                        std::vector<Diagnostic>* diagnostics) {
  WideString rest = Trimmed(value);
  // Size: the trailing token.
  std::optional<size_t> last_space = rest.ReverseFind(L' ');
  if (last_space.has_value()) {
    std::optional<float> size =
        ParseLength(rest.Substr(last_space.value() + 1), em_base);
    if (size.has_value()) {
      style->size = *size;
      rest = Trimmed(rest.First(last_space.value()));
    }
  } else if (std::optional<float> size = ParseLength(rest, em_base)) {
    style->size = *size;
    return;
  }
  // Leading keywords.
  while (true) {
    std::optional<size_t> space = rest.Find(L' ');
    WideString token =
        Lowered(space.has_value() ? rest.First(space.value()) : rest);
    if (token == L"bold" || token == L"bolder") {
      style->weight = 700;
    } else if (token == L"lighter") {
      style->weight = 300;
    } else if (token == L"italic" || token == L"oblique") {
      style->italic = true;
    } else if (token == L"normal" || token == L"small-caps") {
      // no-op
    } else if (!token.IsEmpty() && token[0] >= L'1' && token[0] <= L'9' &&
               ParseWeight(token).has_value() &&
               !ParseLength(token, em_base).has_value()) {
      style->weight = *ParseWeight(token);
    } else {
      break;
    }
    if (!space.has_value()) {
      rest.clear();
      break;
    }
    rest = Trimmed(rest.Substr(space.value() + 1));
  }
  if (!rest.IsEmpty()) {
    WideString family = FirstFamily(rest);
    if (family.IsEmpty()) {
      AddDiagnostic(diagnostics, Diagnostic::Code::kInvalidValue,
                    L"font:" + value);
    } else {
      style->family = family;
    }
  }
}

// ---- XML walk -------------------------------------------------------------

bool IsFormattingWhitespace(const WideString& text) {
  bool has_newline = false;
  for (wchar_t ch : text) {
    if (ch == L'\n' || ch == L'\r') {
      has_newline = true;
    } else if (ch != L' ' && ch != L'\t') {
      return false;
    }
  }
  return has_newline;
}

WideString NormalizeBreaks(const WideString& text) {
  WideString out;
  for (size_t i = 0; i < text.GetLength(); ++i) {
    wchar_t ch = text[i];
    if (ch == L'\r') {
      out += L'\r';
      if (i + 1 < text.GetLength() && text[i + 1] == L'\n') {
        ++i;
      }
    } else if (ch == L'\n') {
      out += L'\r';
    } else {
      out += ch;
    }
  }
  return out;
}

struct WalkState {
  CPDF_RichTextDocument* document;
  CPDF_RichTextParagraph* current = nullptr;
  int list_depth = 0;
  bool list_ordered = false;
  int list_item_index = 0;
};

CPDF_RichTextParagraph* EnsureParagraph(WalkState* state) {
  if (!state->current) {
    state->document->paragraphs.emplace_back();
    state->current = &state->document->paragraphs.back();
    state->current->props = state->document->body_paragraph;
  }
  return state->current;
}

void AppendRun(WalkState* state,
               const WideString& text,
               const CPDF_RichTextStyleDelta& style) {
  if (text.IsEmpty()) {
    return;
  }
  CPDF_RichTextParagraph* paragraph = EnsureParagraph(state);
  paragraph->runs.push_back({text, style});
}

bool IsKnownTag(const WideString& tag) {
  static constexpr const wchar_t* kKnown[] = {
      L"body", L"html", L"p",  L"span", L"a",  L"b",  L"i",
      L"sub",  L"sup",  L"br", L"ol",   L"ul", L"li",
  };
  for (const wchar_t* known : kKnown) {
    if (tag == known) {
      return true;
    }
  }
  return false;
}

void WalkNode(const CFX_XMLNode* node,
              CPDF_RichTextStyleDelta inherited,
              WalkState* state,
              float em_base);

void WalkChildren(const CFX_XMLNode* node,
                  const CPDF_RichTextStyleDelta& inherited,
                  WalkState* state,
                  float em_base) {
  for (const CFX_XMLNode* child = node->GetFirstChild(); child;
       child = child->GetNextSibling()) {
    WalkNode(child, inherited, state, em_base);
  }
}

// Merge a child's declarations over the inherited delta (both are deltas
// relative to the body; the child overrides what it sets).
CPDF_RichTextStyleDelta Inherit(const CPDF_RichTextStyleDelta& parent,
                                const CPDF_RichTextStyleDelta& child) {
  CPDF_RichTextStyleDelta out = parent;
  if (child.family) {
    out.family = child.family;
  }
  if (child.weight) {
    out.weight = child.weight;
  }
  if (child.italic) {
    out.italic = child.italic;
  }
  if (child.size) {
    out.size = child.size;
  }
  if (child.color) {
    out.color = child.color;
  }
  if (child.decoration) {
    out.decoration = child.decoration;
  }
  if (child.script) {
    out.script = child.script;
  }
  if (child.letter_spacing) {
    out.letter_spacing = child.letter_spacing;
  }
  if (child.horz_scale) {
    out.horz_scale = child.horz_scale;
  }
  if (!child.unknown_declarations.IsEmpty()) {
    if (!out.unknown_declarations.IsEmpty()) {
      out.unknown_declarations += L";";
    }
    out.unknown_declarations += child.unknown_declarations;
  }
  return out;
}

void WalkNode(const CFX_XMLNode* node,
              CPDF_RichTextStyleDelta inherited,
              WalkState* state,
              float em_base) {
  const CFX_XMLNode::Type type = node->GetType();
  if (type == CFX_XMLNode::Type::kText ||
      type == CFX_XMLNode::Type::kCharData) {
    const WideString& text = ToXMLText(node)->GetText();
    if (text.IsEmpty()) {
      return;
    }
    // Whitespace is literal inside a paragraph (Acrobat writes Enter as a
    // bare "&#13;" and every space is drawn). Outside one, whitespace with a
    // newline is pretty-printing between block elements, never content.
    if (!state->current && IsFormattingWhitespace(text)) {
      return;
    }
    AppendRun(state, NormalizeBreaks(text), inherited);
    return;
  }
  if (type != CFX_XMLNode::Type::kElement) {
    return;
  }
  const CFX_XMLElement* element = ToXMLElement(node);
  const WideString tag = Lowered(element->GetLocalTagName());
  CPDF_RichTextDocument* document = state->document;

  if (tag == L"p" || tag == L"li") {
    document->paragraphs.emplace_back();
    state->current = &document->paragraphs.back();
    state->current->props = document->body_paragraph;
    if (Lowered(element->GetAttribute(L"dir")) == L"rtl") {
      state->current->props.rtl = true;
    }
    CPDF_RichTextStyleDelta own;
    CPDF_RichTextParser::ParseInlineStyle(element->GetAttribute(L"style"), &own,
                                          &state->current->props,
                                          &document->diagnostics);
    CPDF_RichTextStyleDelta merged = Inherit(inherited, own);
    if (tag == L"li") {
      ++state->list_item_index;
      WideString marker =
          state->list_ordered
              ? WideString::Format(L"%d.  ", state->list_item_index)
              : WideString(L"\x00b7  ");
      AppendRun(state, marker, merged);
    }
    WalkChildren(node, merged, state, em_base);
    state->current = nullptr;  // the next bare text starts a new paragraph
    return;
  }
  if (tag == L"ol" || tag == L"ul") {
    AddDiagnostic(&document->diagnostics, Diagnostic::Code::kListDegraded, tag);
    const bool saved_ordered = state->list_ordered;
    const int saved_index = state->list_item_index;
    state->list_ordered = tag == L"ol";
    state->list_item_index = 0;
    ++state->list_depth;
    WalkChildren(node, inherited, state, em_base);
    --state->list_depth;
    state->list_ordered = saved_ordered;
    state->list_item_index = saved_index;
    state->current = nullptr;
    return;
  }
  if (tag == L"br") {
    AppendRun(state, L"\r", inherited);
    return;
  }

  CPDF_RichTextStyleDelta own;
  if (tag == L"b") {
    own.weight = 700;
  } else if (tag == L"i") {
    own.italic = true;
  } else if (tag == L"sub") {
    own.script = Script::kSub;
  } else if (tag == L"sup") {
    own.script = Script::kSuper;
  } else if (!IsKnownTag(tag)) {
    AddDiagnostic(&document->diagnostics, Diagnostic::Code::kUnsupportedTag,
                  tag);
  }
  // A span may carry paragraph properties (Acrobat writes text-align on the
  // span); they reach the paragraph that contains the span.
  CPDF_RichTextParagraphProps* paragraph =
      state->current ? &state->current->props : &document->body_paragraph;
  CPDF_RichTextParser::ParseInlineStyle(element->GetAttribute(L"style"), &own,
                                        paragraph, &document->diagnostics);
  WalkChildren(node, Inherit(inherited, own), state, em_base);
}

const CFX_XMLElement* FindBody(const CFX_XMLElement* root) {
  if (!root) {
    return nullptr;
  }
  if (Lowered(root->GetLocalTagName()) == L"body") {
    return root;
  }
  for (const CFX_XMLNode* child = root->GetFirstChild(); child;
       child = child->GetNextSibling()) {
    if (child->GetType() != CFX_XMLNode::Type::kElement) {
      continue;
    }
    if (const CFX_XMLElement* found = FindBody(ToXMLElement(child))) {
      return found;
    }
  }
  return nullptr;
}

// ---- defaults from the annotation ----------------------------------------

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

bool ApplyStandardAlias(const ByteString& name, CPDF_RichTextStyle* style) {
  for (const StandardAlias& entry : kStandardAliases) {
    if (name == entry.alias) {
      style->family = entry.family;
      style->weight = entry.weight;
      style->italic = entry.italic;
      return true;
    }
  }
  return false;
}

// "ABCDEF+MinionPro-BoldItalic" -> family MinionPro, 700, italic.
void ApplyBaseFontName(ByteString base_font, CPDF_RichTextStyle* style) {
  if (base_font.GetLength() > 7 && base_font[6] == '+') {
    base_font = base_font.Substr(7);
  }
  if (ApplyStandardAlias(base_font, style)) {
    return;
  }
  ByteString family = base_font;
  ByteString suffix;
  std::optional<size_t> dash = base_font.Find('-');
  if (!dash.has_value()) {
    dash = base_font.Find(',');
  }
  if (dash.has_value()) {
    family = base_font.First(dash.value());
    suffix = base_font.Substr(dash.value() + 1);
  }
  style->family = WideString::FromUTF8(family.AsStringView());
  if (suffix.Contains("Bold")) {
    style->weight = 700;
  } else if (suffix.Contains("Light")) {
    style->weight = 300;
  } else if (suffix.Contains("Medium")) {
    style->weight = 500;
  } else if (suffix.Contains("Semibold") || suffix.Contains("Demibold")) {
    style->weight = 600;
  } else if (suffix.Contains("Black") || suffix.Contains("Heavy")) {
    style->weight = 900;
  }
  if (suffix.Contains("Italic") || suffix.Contains("Oblique")) {
    style->italic = true;
  }
}

RetainPtr<const CPDF_Dictionary> FontDescriptorOf(
    const CPDF_Dictionary* font_dict) {
  if (font_dict->GetNameFor("Subtype") == "Type0") {
    RetainPtr<const CPDF_Array> descendants =
        font_dict->GetArrayFor("DescendantFonts");
    RetainPtr<const CPDF_Dictionary> cid_font =
        descendants ? descendants->GetDictAt(0) : nullptr;
    return cid_font ? cid_font->GetDictFor("FontDescriptor") : nullptr;
  }
  return font_dict->GetDictFor("FontDescriptor");
}

// The face a /DA font name stands for, from /DR when it is there.
void ApplyDaFontFace(const ByteString& font_name,
                     const CPDF_Dictionary* acroform_dict,
                     CPDF_RichTextStyle* style) {
  RetainPtr<const CPDF_Dictionary> font_dict;
  if (acroform_dict) {
    RetainPtr<const CPDF_Dictionary> dr = acroform_dict->GetDictFor("DR");
    RetainPtr<const CPDF_Dictionary> fonts =
        dr ? dr->GetDictFor("Font") : nullptr;
    font_dict = fonts ? fonts->GetDictFor(font_name.AsStringView()) : nullptr;
  }
  if (!font_dict) {
    if (!ApplyStandardAlias(font_name, style)) {
      style->family = WideString::FromUTF8(font_name.AsStringView());
    }
    return;
  }
  ApplyBaseFontName(font_dict->GetNameFor("BaseFont"), style);
  RetainPtr<const CPDF_Dictionary> descriptor =
      FontDescriptorOf(font_dict.Get());
  if (!descriptor) {
    return;
  }
  WideString family = descriptor->GetUnicodeTextFor("FontFamily");
  if (!family.IsEmpty()) {
    style->family = family;
  }
  if (descriptor->KeyExist("FontWeight")) {
    style->weight = descriptor->GetIntegerFor("FontWeight", style->weight);
  }
  if (descriptor->KeyExist("ItalicAngle")) {
    style->italic = descriptor->GetIntegerFor("ItalicAngle", 0) != 0;
  }
}

FX_ARGB ArgbFromCfxColor(const CFX_Color& color) {
  switch (color.nColorType) {
    case CFX_Color::Type::kGray: {
      const uint32_t g = static_cast<uint32_t>(
          std::clamp(color.fColor1, 0.0f, 1.0f) * 255.0f + 0.5f);
      return ArgbEncode(255, g, g, g);
    }
    case CFX_Color::Type::kRGB:
      return ArgbEncode(
          255,
          static_cast<uint32_t>(std::clamp(color.fColor1, 0.0f, 1.0f) * 255.0f +
                                0.5f),
          static_cast<uint32_t>(std::clamp(color.fColor2, 0.0f, 1.0f) * 255.0f +
                                0.5f),
          static_cast<uint32_t>(std::clamp(color.fColor3, 0.0f, 1.0f) * 255.0f +
                                0.5f));
    case CFX_Color::Type::kCMYK: {
      const float k = std::clamp(color.fColor4, 0.0f, 1.0f);
      auto channel = [&](float c) {
        return static_cast<uint32_t>(
            (1.0f - std::min(1.0f, std::clamp(c, 0.0f, 1.0f) + k)) * 255.0f +
            0.5f);
      };
      return ArgbEncode(255, channel(color.fColor1), channel(color.fColor2),
                        channel(color.fColor3));
    }
    case CFX_Color::Type::kTransparent:
      break;
  }
  return 0xFF000000;
}

// ---- JSON -----------------------------------------------------------------

ByteString JsonString(const WideString& text) {
  ByteString utf8 = text.ToUTF8();
  ByteString out("\"");
  for (char ch : utf8) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += ByteString::Format("\\u%04x", c);
        } else {
          out += ch;
        }
    }
  }
  out += "\"";
  return out;
}

ByteString JsonNumber(float value) {
  if (isnan(value) || isinf(value)) {
    return "0";
  }
  ByteString text = ByteString::Format("%.4f", value);
  if (text.Contains(".")) {
    while (text.Back() == '0') {
      text = text.First(text.GetLength() - 1);
    }
    if (text.Back() == '.') {
      text = text.First(text.GetLength() - 1);
    }
  }
  if (text == "-0") {
    return "0";
  }
  return text;
}

ByteString JsonColor(FX_ARGB color) {
  return ByteString::Format("\"#%02X%02X%02X\"", FXARGB_R(color),
                            FXARGB_G(color), FXARGB_B(color));
}

ByteString JsonDecoration(uint8_t bits) {
  ByteString out("[");
  bool first = true;
  auto add = [&](const char* name) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += "\"";
    out += name;
    out += "\"";
  };
  if (bits & CPDF_RichTextStyle::kUnderline) {
    add("underline");
  }
  if (bits & CPDF_RichTextStyle::kLineThrough) {
    add("line-through");
  }
  if (bits & CPDF_RichTextStyle::kWordUnderline) {
    add("word");
  }
  out += "]";
  return out;
}

const char* ScriptName(Script script) {
  switch (script) {
    case Script::kSub:
      return "sub";
    case Script::kSuper:
      return "super";
    case Script::kNormal:
      break;
  }
  return "normal";
}

const char* AlignName(Align align) {
  switch (align) {
    case Align::kCenter:
      return "center";
    case Align::kRight:
      return "right";
    case Align::kJustify:
      return "justify";
    case Align::kLeft:
      break;
  }
  return "left";
}

const char* DiagnosticName(Diagnostic::Code code) {
  switch (code) {
    case Diagnostic::Code::kMalformedRC:
      return "malformed-rc";
    case Diagnostic::Code::kUnsupportedTag:
      return "unsupported-tag";
    case Diagnostic::Code::kListDegraded:
      return "list-degraded";
    case Diagnostic::Code::kUnknownDeclaration:
      return "unknown-declaration";
    case Diagnostic::Code::kInvalidValue:
      return "invalid-value";
  }
  return "unknown";
}

ByteString JsonStyleDelta(const CPDF_RichTextStyleDelta& delta) {
  ByteString out("{");
  bool first = true;
  auto field = [&](const char* key, const ByteString& value) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += "\"";
    out += key;
    out += "\":";
    out += value;
  };
  if (delta.family) {
    field("family", JsonString(*delta.family));
  }
  if (delta.weight) {
    field("weight", ByteString::FormatInteger(*delta.weight));
  }
  if (delta.italic) {
    field("italic", *delta.italic ? "true" : "false");
  }
  if (delta.size) {
    field("size", JsonNumber(*delta.size));
  }
  if (delta.color) {
    field("color", JsonColor(*delta.color));
  }
  if (delta.decoration) {
    field("decoration", JsonDecoration(*delta.decoration));
  }
  if (delta.script) {
    field("script", ByteString("\"") + ScriptName(*delta.script) + "\"");
  }
  if (delta.letter_spacing) {
    field("letterSpacing", JsonNumber(*delta.letter_spacing));
  }
  if (delta.horz_scale) {
    field("horizontalScale", JsonNumber(*delta.horz_scale));
  }
  if (!delta.unknown_declarations.IsEmpty()) {
    field("unknown", JsonString(delta.unknown_declarations));
  }
  out += "}";
  return out;
}

ByteString JsonParagraphProps(const CPDF_RichTextParagraphProps& props) {
  ByteString out;
  out += ByteString("\"align\":\"") + AlignName(props.align) + "\"";
  out += ByteString(",\"dir\":\"") + (props.rtl ? "rtl" : "ltr") + "\"";
  if (props.line_height) {
    out += ",\"lineHeight\":" + JsonNumber(*props.line_height);
  }
  if (props.margin_top != 0 || props.margin_bottom != 0 ||
      props.margin_left != 0 || props.margin_right != 0) {
    out += ",\"margins\":{\"top\":" + JsonNumber(props.margin_top) +
           ",\"bottom\":" + JsonNumber(props.margin_bottom) +
           ",\"left\":" + JsonNumber(props.margin_left) +
           ",\"right\":" + JsonNumber(props.margin_right) + "}";
  }
  if (props.text_indent != 0) {
    out += ",\"textIndent\":" + JsonNumber(props.text_indent);
  }
  if (!props.unknown_declarations.IsEmpty()) {
    out += ",\"unknown\":" + JsonString(props.unknown_declarations);
  }
  return out;
}

}  // namespace

bool CPDF_RichTextStyleDelta::IsEmpty() const {
  return !family && !weight && !italic && !size && !color && !decoration &&
         !script && !letter_spacing && !horz_scale &&
         unknown_declarations.IsEmpty();
}

CPDF_RichTextStyle ApplyRichTextStyleDelta(
    const CPDF_RichTextStyle& base,
    const CPDF_RichTextStyleDelta& delta) {
  CPDF_RichTextStyle out = base;
  if (delta.family) {
    out.family = *delta.family;
  }
  if (delta.weight) {
    out.weight = *delta.weight;
  }
  if (delta.italic) {
    out.italic = *delta.italic;
  }
  if (delta.size) {
    out.size = *delta.size;
  }
  if (delta.color) {
    out.color = *delta.color;
  }
  if (delta.decoration) {
    out.decoration = *delta.decoration;
  }
  if (delta.script) {
    out.script = *delta.script;
  }
  if (delta.letter_spacing) {
    out.letter_spacing = *delta.letter_spacing;
  }
  if (delta.horz_scale) {
    out.horz_scale = *delta.horz_scale;
  }
  return out;
}

// static
void CPDF_RichTextParser::ParseInlineStyle(
    const WideString& css,
    CPDF_RichTextStyleDelta* style,
    CPDF_RichTextParagraphProps* paragraph,
    std::vector<CPDF_RichTextDiagnostic>* diagnostics) {
  const float em_base = style->size.value_or(12.0f);
  for (const auto& [name, value] : SplitDeclarations(css)) {
    if (name == L"font") {
      ApplyFontShorthand(value, em_base, style, diagnostics);
      continue;
    }
    if (ApplyDeclaration(name, value, em_base, style, paragraph, diagnostics)) {
      continue;
    }
    AppendUnknownDeclaration(&style->unknown_declarations, name, value);
    if (!IsRecognisedUnmodelled(name)) {
      AddDiagnostic(diagnostics, Diagnostic::Code::kUnknownDeclaration, name);
    }
  }
}

// static
CPDF_RichTextStyleDelta CPDF_RichTextParser::ParseDefaultStyle(
    const WideString& ds,
    CPDF_RichTextParagraphProps* paragraph,
    std::vector<CPDF_RichTextDiagnostic>* diagnostics) {
  CPDF_RichTextStyleDelta delta;
  ParseInlineStyle(ds, &delta, paragraph, diagnostics);
  return delta;
}

// static
CPDF_RichTextDocument CPDF_RichTextParser::FromPlainText(
    const WideString& contents,
    const CPDF_RichTextStyle& defaults,
    const CPDF_RichTextParagraphProps& paragraph_defaults) {
  CPDF_RichTextDocument document;
  document.source = CPDF_RichTextDocument::Source::kContents;
  document.body = defaults;
  document.body_paragraph = paragraph_defaults;
  for (const WideString& line :
       fxcrt::Split(NormalizeBreaks(contents), L'\r')) {
    CPDF_RichTextParagraph paragraph;
    paragraph.props = paragraph_defaults;
    if (!line.IsEmpty()) {
      paragraph.runs.push_back({line, CPDF_RichTextStyleDelta()});
    }
    document.paragraphs.push_back(std::move(paragraph));
  }
  if (document.paragraphs.empty()) {
    CPDF_RichTextParagraph paragraph;
    paragraph.props = paragraph_defaults;
    document.paragraphs.push_back(std::move(paragraph));
  }
  return document;
}

// static
CPDF_RichTextDocument CPDF_RichTextParser::ParseRichContent(
    const WideString& rc_xml,
    const CPDF_RichTextStyle& defaults,
    const CPDF_RichTextParagraphProps& paragraph_defaults,
    const WideString& contents_fallback) {
  ByteString utf8 = rc_xml.ToUTF8();
  auto stream =
      pdfium::MakeRetain<CFX_ReadOnlySpanStream>(utf8.unsigned_span());
  CFX_XMLParser xml(stream);
  std::unique_ptr<CFX_XMLDocument> xml_document = xml.Parse();
  const CFX_XMLElement* body =
      xml_document ? FindBody(xml_document->GetRoot()) : nullptr;
  if (!body) {
    // Fall back to the root element: some producers omit <body>.
    body = xml_document ? xml_document->GetRoot() : nullptr;
  }
  if (!body || !body->GetFirstChild()) {
    CPDF_RichTextDocument document =
        FromPlainText(contents_fallback, defaults, paragraph_defaults);
    document.diagnostics.push_back(
        {Diagnostic::Code::kMalformedRC, ByteString("/RC did not parse")});
    return document;
  }

  CPDF_RichTextDocument document;
  document.source = CPDF_RichTextDocument::Source::kRC;
  document.body_paragraph = paragraph_defaults;
  CPDF_RichTextStyleDelta body_delta;
  body_delta.size = defaults.size;  // em base for the body declarations
  body_delta = CPDF_RichTextStyleDelta();
  {
    CPDF_RichTextStyleDelta parsed;
    parsed.size = defaults.size;
    ParseInlineStyle(body->GetAttribute(L"style"), &parsed,
                     &document.body_paragraph, &document.diagnostics);
    if (parsed.size == defaults.size) {
      parsed.size.reset();
    }
    body_delta = parsed;
  }
  document.body = ApplyRichTextStyleDelta(defaults, body_delta);
  if (Lowered(body->GetAttribute(L"dir")) == L"rtl") {
    document.body_paragraph.rtl = true;
  }

  WalkState state;
  state.document = &document;
  WalkChildren(body, CPDF_RichTextStyleDelta(), &state, document.body.size);
  if (document.paragraphs.empty()) {
    CPDF_RichTextParagraph paragraph;
    paragraph.props = document.body_paragraph;
    document.paragraphs.push_back(std::move(paragraph));
  }
  return document;
}

// static
CPDF_RichTextDocument CPDF_RichTextParser::FromAnnotation(
    const CPDF_Dictionary* annot_dict,
    const CPDF_Dictionary* acroform_dict) {
  CPDF_RichTextStyle defaults;
  CPDF_RichTextParagraphProps paragraph_defaults;
  std::vector<Diagnostic> diagnostics;
  if (!annot_dict) {
    return FromPlainText(WideString(), defaults, paragraph_defaults);
  }

  // /DA: font name (→ face through /DR), size, colour.
  CPDF_DefaultAppearance da(annot_dict, acroform_dict);
  std::optional<CPDF_DefaultAppearance::FontNameAndSize> da_font = da.GetFont();
  if (da_font.has_value()) {
    ApplyDaFontFace(da_font->name, acroform_dict, &defaults);
    defaults.size = da_font->size;
  }
  if (std::optional<CFX_Color> da_color = da.GetColor()) {
    defaults.color = ArgbFromCfxColor(*da_color);
  }
  // EmbedPDF's own text colour override for plain FreeText.
  if (RetainPtr<const CPDF_Array> text_color =
          annot_dict->GetArrayFor("TextColor");
      text_color && text_color->size() >= 3) {
    defaults.color = ArgbEncode(
        255,
        static_cast<uint32_t>(
            std::clamp(text_color->GetFloatAt(0), 0.0f, 1.0f) * 255.0f + 0.5f),
        static_cast<uint32_t>(
            std::clamp(text_color->GetFloatAt(1), 0.0f, 1.0f) * 255.0f + 0.5f),
        static_cast<uint32_t>(
            std::clamp(text_color->GetFloatAt(2), 0.0f, 1.0f) * 255.0f + 0.5f));
  }
  // /Q
  switch (annot_dict->GetIntegerFor("Q", 0)) {
    case 1:
      paragraph_defaults.align = Align::kCenter;
      break;
    case 2:
      paragraph_defaults.align = Align::kRight;
      break;
    default:
      break;
  }
  // /DS refines the defaults (family list, size, alignment; its colour is the
  // DA colour again, which for Acrobat is the border, so it is applied but
  // the RC body colour wins afterwards).
  const WideString ds = annot_dict->GetUnicodeTextFor("DS");
  if (!ds.IsEmpty()) {
    CPDF_RichTextStyleDelta ds_delta;
    ds_delta.size = defaults.size > 0 ? defaults.size : 12.0f;
    ParseInlineStyle(ds, &ds_delta, &paragraph_defaults, &diagnostics);
    if (ds_delta.size == (defaults.size > 0 ? defaults.size : 12.0f)) {
      ds_delta.size.reset();
    }
    defaults = ApplyRichTextStyleDelta(defaults, ds_delta);
  }
  if (defaults.size <= 0 && annot_dict->GetNameFor("Subtype") == "FreeText") {
    defaults.size = 12.0f;  // DA size 0 (auto) has no meaning for rich text
  }

  const bool is_widget = annot_dict->GetNameFor("Subtype") == "Widget";
  WideString rich = annot_dict->GetUnicodeTextFor(is_widget ? "RV" : "RC");
  if (rich.IsEmpty()) {
    RetainPtr<const CPDF_Stream> stream =
        annot_dict->GetStreamFor(is_widget ? "RV" : "RC");
    if (stream) {
      rich = stream->GetUnicodeText();
    }
  }
  const WideString contents =
      annot_dict->GetUnicodeTextFor(is_widget ? "V" : "Contents");
  CPDF_RichTextDocument document =
      rich.IsEmpty()
          ? FromPlainText(contents, defaults, paragraph_defaults)
          : ParseRichContent(rich, defaults, paragraph_defaults, contents);
  document.diagnostics.insert(document.diagnostics.begin(), diagnostics.begin(),
                              diagnostics.end());
  return document;
}

// static
ByteString CPDF_RichTextParser::ToJSON(const CPDF_RichTextDocument& document) {
  ByteString out("{\"source\":\"");
  out +=
      document.source == CPDF_RichTextDocument::Source::kRC ? "rc" : "contents";
  out += "\",\"body\":{";
  out += "\"family\":" + JsonString(document.body.family);
  out += ",\"weight\":" + ByteString::FormatInteger(document.body.weight);
  out += ByteString(",\"italic\":") + (document.body.italic ? "true" : "false");
  out += ",\"size\":" + JsonNumber(document.body.size);
  out += ",\"color\":" + JsonColor(document.body.color);
  out += ",\"decoration\":" + JsonDecoration(document.body.decoration);
  out += ByteString(",\"script\":\"") + ScriptName(document.body.script) + "\"";
  out += ",\"letterSpacing\":" + JsonNumber(document.body.letter_spacing);
  out += ",\"horizontalScale\":" + JsonNumber(document.body.horz_scale);
  out += "," + JsonParagraphProps(document.body_paragraph);
  out += "},\"paragraphs\":[";
  bool first_paragraph = true;
  for (const CPDF_RichTextParagraph& paragraph : document.paragraphs) {
    if (!first_paragraph) {
      out += ",";
    }
    first_paragraph = false;
    out += "{" + JsonParagraphProps(paragraph.props) + ",\"runs\":[";
    bool first_run = true;
    for (const CPDF_RichTextRun& run : paragraph.runs) {
      if (!first_run) {
        out += ",";
      }
      first_run = false;
      out += "{\"text\":" + JsonString(run.text);
      if (!run.style.IsEmpty()) {
        out += ",\"style\":" + JsonStyleDelta(run.style);
      }
      out += "}";
    }
    out += "]}";
  }
  out += "],\"diagnostics\":[";
  bool first_diag = true;
  for (const Diagnostic& diagnostic : document.diagnostics) {
    if (!first_diag) {
      out += ",";
    }
    first_diag = false;
    out += ByteString("{\"code\":\"") + DiagnosticName(diagnostic.code) +
           "\",\"detail\":" +
           JsonString(WideString::FromUTF8(diagnostic.detail.AsStringView())) +
           "}";
  }
  out += "]}";
  return out;
}
