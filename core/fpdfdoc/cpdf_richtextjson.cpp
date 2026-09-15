// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_richtextjson.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/fxcrt/fx_string.h"
#include "core/fxcrt/widestring.h"

namespace {

// A small JSON value tree: enough for the document shape, nothing more.
struct JsonValue {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  double number = 0;
  WideString string;
  std::vector<JsonValue> array;
  std::map<std::string, JsonValue> object;

  const JsonValue* Get(const char* key) const {
    if (type != Type::kObject) {
      return nullptr;
    }
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
  }
};

class JsonReader {
 public:
  explicit JsonReader(const ByteString& text) : text_(text) {}

  bool Read(JsonValue* out) {
    SkipSpace();
    if (!ReadValue(out, 0)) {
      return false;
    }
    SkipSpace();
    return pos_ == text_.GetLength();
  }

 private:
  static constexpr int kMaxDepth = 64;

  bool ReadValue(JsonValue* out, int depth) {
    if (depth > kMaxDepth || pos_ >= text_.GetLength()) {
      return false;
    }
    const char ch = text_[pos_];
    if (ch == '{') {
      return ReadObject(out, depth);
    }
    if (ch == '[') {
      return ReadArray(out, depth);
    }
    if (ch == '"') {
      out->type = JsonValue::Type::kString;
      return ReadString(&out->string);
    }
    if (ch == 't' && Match("true")) {
      out->type = JsonValue::Type::kBool;
      out->boolean = true;
      return true;
    }
    if (ch == 'f' && Match("false")) {
      out->type = JsonValue::Type::kBool;
      out->boolean = false;
      return true;
    }
    if (ch == 'n' && Match("null")) {
      out->type = JsonValue::Type::kNull;
      return true;
    }
    return ReadNumber(out);
  }

  bool Match(const char* literal) {
    const size_t length = strlen(literal);
    if (pos_ + length > text_.GetLength() ||
        text_.Substr(pos_, length) != literal) {
      return false;
    }
    pos_ += length;
    return true;
  }

  bool ReadObject(JsonValue* out, int depth) {
    out->type = JsonValue::Type::kObject;
    ++pos_;  // '{'
    SkipSpace();
    if (pos_ < text_.GetLength() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipSpace();
      if (pos_ >= text_.GetLength() || text_[pos_] != '"') {
        return false;
      }
      WideString key;
      if (!ReadString(&key)) {
        return false;
      }
      SkipSpace();
      if (pos_ >= text_.GetLength() || text_[pos_] != ':') {
        return false;
      }
      ++pos_;
      SkipSpace();
      JsonValue value;
      if (!ReadValue(&value, depth + 1)) {
        return false;
      }
      out->object[std::string(key.ToUTF8().c_str())] = std::move(value);
      SkipSpace();
      if (pos_ >= text_.GetLength()) {
        return false;
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool ReadArray(JsonValue* out, int depth) {
    out->type = JsonValue::Type::kArray;
    ++pos_;  // '['
    SkipSpace();
    if (pos_ < text_.GetLength() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipSpace();
      JsonValue value;
      if (!ReadValue(&value, depth + 1)) {
        return false;
      }
      out->array.push_back(std::move(value));
      SkipSpace();
      if (pos_ >= text_.GetLength()) {
        return false;
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool ReadHex4(uint32_t* out) {
    if (pos_ + 4 > text_.GetLength()) {
      return false;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char ch = text_[pos_++];
      value <<= 4;
      if (ch >= '0' && ch <= '9') {
        value |= ch - '0';
      } else if (ch >= 'a' && ch <= 'f') {
        value |= ch - 'a' + 10;
      } else if (ch >= 'A' && ch <= 'F') {
        value |= ch - 'A' + 10;
      } else {
        return false;
      }
    }
    *out = value;
    return true;
  }

  bool ReadString(WideString* out) {
    ++pos_;  // opening quote
    std::string utf8;
    while (pos_ < text_.GetLength()) {
      const char ch = text_[pos_++];
      if (ch == '"') {
        *out = WideString::FromUTF8(ByteStringView(utf8.c_str(), utf8.size()));
        return true;
      }
      if (ch != '\\') {
        utf8 += ch;
        continue;
      }
      if (pos_ >= text_.GetLength()) {
        return false;
      }
      const char escaped = text_[pos_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          utf8 += escaped;
          break;
        case 'b':
          utf8 += '\b';
          break;
        case 'f':
          utf8 += '\f';
          break;
        case 'n':
          utf8 += '\n';
          break;
        case 'r':
          utf8 += '\r';
          break;
        case 't':
          utf8 += '\t';
          break;
        case 'u': {
          uint32_t code = 0;
          if (!ReadHex4(&code)) {
            return false;
          }
          if (code >= 0xD800 && code <= 0xDBFF) {
            uint32_t low = 0;
            if (pos_ + 6 <= text_.GetLength() && text_[pos_] == '\\' &&
                text_[pos_ + 1] == 'u') {
              pos_ += 2;
              if (!ReadHex4(&low) || low < 0xDC00 || low > 0xDFFF) {
                return false;
              }
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
          }
          AppendUtf8(code, &utf8);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  static void AppendUtf8(uint32_t code, std::string* out) {
    if (code < 0x80) {
      *out += static_cast<char>(code);
    } else if (code < 0x800) {
      *out += static_cast<char>(0xC0 | (code >> 6));
      *out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
      *out += static_cast<char>(0xE0 | (code >> 12));
      *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      *out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
      *out += static_cast<char>(0xF0 | (code >> 18));
      *out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      *out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }

  bool ReadNumber(JsonValue* out) {
    const size_t start = pos_;
    if (pos_ < text_.GetLength() && text_[pos_] == '-') {
      ++pos_;
    }
    bool digits = false;
    while (pos_ < text_.GetLength() && isdigit(text_[pos_])) {
      ++pos_;
      digits = true;
    }
    if (pos_ < text_.GetLength() && text_[pos_] == '.') {
      ++pos_;
      while (pos_ < text_.GetLength() && isdigit(text_[pos_])) {
        ++pos_;
        digits = true;
      }
    }
    if (!digits) {
      return false;
    }
    if (pos_ < text_.GetLength() &&
        (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.GetLength() &&
          (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      bool exp_digits = false;
      while (pos_ < text_.GetLength() && isdigit(text_[pos_])) {
        ++pos_;
        exp_digits = true;
      }
      if (!exp_digits) {
        return false;
      }
    }
    out->type = JsonValue::Type::kNumber;
    out->number =
        StringToDouble(text_.AsStringView().Substr(start, pos_ - start));
    return true;
  }

  void SkipSpace() {
    while (pos_ < text_.GetLength() &&
           (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' ||
            text_[pos_] == '\t')) {
      ++pos_;
    }
  }

  const ByteString& text_;
  size_t pos_ = 0;
};

// ---- Document shape
// -------------------------------------------------------------

std::optional<float> Number(const JsonValue* value) {
  if (!value || value->type != JsonValue::Type::kNumber) {
    return std::nullopt;
  }
  return static_cast<float>(value->number);
}

std::optional<bool> Bool(const JsonValue* value) {
  if (!value || value->type != JsonValue::Type::kBool) {
    return std::nullopt;
  }
  return value->boolean;
}

std::optional<WideString> String(const JsonValue* value) {
  if (!value || value->type != JsonValue::Type::kString) {
    return std::nullopt;
  }
  return value->string;
}

std::optional<FX_ARGB> Color(const JsonValue* value) {
  std::optional<WideString> text = String(value);
  if (!text.has_value() || text->GetLength() != 7 || (*text)[0] != L'#') {
    return std::nullopt;
  }
  uint32_t rgb = 0;
  for (size_t i = 1; i < 7; ++i) {
    const wchar_t ch = (*text)[i];
    rgb <<= 4;
    if (ch >= L'0' && ch <= L'9') {
      rgb |= ch - L'0';
    } else if (ch >= L'a' && ch <= L'f') {
      rgb |= ch - L'a' + 10;
    } else if (ch >= L'A' && ch <= L'F') {
      rgb |= ch - L'A' + 10;
    } else {
      return std::nullopt;
    }
  }
  return 0xFF000000 | rgb;
}

std::optional<uint8_t> Decoration(const JsonValue* value) {
  if (!value || value->type != JsonValue::Type::kArray) {
    return std::nullopt;
  }
  uint8_t bits = 0;
  for (const JsonValue& item : value->array) {
    std::optional<WideString> name = String(&item);
    if (!name.has_value()) {
      return std::nullopt;
    }
    if (*name == L"underline") {
      bits |= CPDF_RichTextStyle::kUnderline;
    } else if (*name == L"line-through") {
      bits |= CPDF_RichTextStyle::kLineThrough;
    } else if (*name == L"word") {
      bits |= CPDF_RichTextStyle::kWordUnderline;
    } else {
      return std::nullopt;
    }
  }
  return bits;
}

std::optional<CPDF_RichTextStyle::Script> ScriptOf(const JsonValue* value) {
  std::optional<WideString> name = String(value);
  if (!name.has_value()) {
    return std::nullopt;
  }
  if (*name == L"normal") {
    return CPDF_RichTextStyle::Script::kNormal;
  }
  if (*name == L"sub") {
    return CPDF_RichTextStyle::Script::kSub;
  }
  if (*name == L"super") {
    return CPDF_RichTextStyle::Script::kSuper;
  }
  return std::nullopt;
}

// A style object into a delta; every key is optional, a wrong type fails.
bool ReadStyleDelta(const JsonValue* object, CPDF_RichTextStyleDelta* delta) {
  if (!object || object->type != JsonValue::Type::kObject) {
    return false;
  }
  for (const auto& [key, value] : object->object) {
    if (key == "family") {
      delta->family = String(&value);
      if (!delta->family) {
        return false;
      }
    } else if (key == "weight") {
      std::optional<float> weight = Number(&value);
      if (!weight || *weight < 100 || *weight > 900) {
        return false;
      }
      delta->weight = static_cast<int>(*weight);
    } else if (key == "italic") {
      delta->italic = Bool(&value);
      if (!delta->italic) {
        return false;
      }
    } else if (key == "size") {
      delta->size = Number(&value);
      if (!delta->size || *delta->size < 0) {
        return false;
      }
    } else if (key == "color") {
      delta->color = Color(&value);
      if (!delta->color) {
        return false;
      }
    } else if (key == "decoration") {
      delta->decoration = Decoration(&value);
      if (!delta->decoration) {
        return false;
      }
    } else if (key == "script") {
      delta->script = ScriptOf(&value);
      if (!delta->script) {
        return false;
      }
    } else if (key == "letterSpacing") {
      delta->letter_spacing = Number(&value);
      if (!delta->letter_spacing) {
        return false;
      }
    } else if (key == "horizontalScale") {
      delta->horz_scale = Number(&value);
      if (!delta->horz_scale || *delta->horz_scale <= 0) {
        return false;
      }
    } else if (key == "unknown") {
      std::optional<WideString> unknown = String(&value);
      if (!unknown) {
        return false;
      }
      delta->unknown_declarations = *unknown;
    }
    // Other keys (degraded, source…) are output-only and ignored.
  }
  return true;
}

bool ReadParagraphProps(const JsonValue* object,
                        CPDF_RichTextParagraphProps* props) {
  if (!object || object->type != JsonValue::Type::kObject) {
    return false;
  }
  if (const JsonValue* align = object->Get("align")) {
    std::optional<WideString> name = String(align);
    if (!name) {
      return false;
    }
    if (*name == L"left") {
      props->align = CPDF_RichTextParagraphProps::Align::kLeft;
    } else if (*name == L"center") {
      props->align = CPDF_RichTextParagraphProps::Align::kCenter;
    } else if (*name == L"right") {
      props->align = CPDF_RichTextParagraphProps::Align::kRight;
    } else if (*name == L"justify") {
      props->align = CPDF_RichTextParagraphProps::Align::kJustify;
    } else {
      return false;
    }
  }
  if (const JsonValue* dir = object->Get("dir")) {
    std::optional<WideString> name = String(dir);
    if (!name || (*name != L"ltr" && *name != L"rtl")) {
      return false;
    }
    props->rtl = *name == L"rtl";
  }
  if (const JsonValue* line_height = object->Get("lineHeight")) {
    props->line_height = Number(line_height);
    if (!props->line_height) {
      return false;
    }
  }
  if (const JsonValue* margins = object->Get("margins")) {
    if (margins->type != JsonValue::Type::kObject) {
      return false;
    }
    props->margin_top = Number(margins->Get("top")).value_or(0);
    props->margin_bottom = Number(margins->Get("bottom")).value_or(0);
    props->margin_left = Number(margins->Get("left")).value_or(0);
    props->margin_right = Number(margins->Get("right")).value_or(0);
  }
  if (const JsonValue* indent = object->Get("textIndent")) {
    std::optional<float> value = Number(indent);
    if (!value) {
      return false;
    }
    props->text_indent = *value;
  }
  if (const JsonValue* unknown = object->Get("unknown")) {
    std::optional<WideString> value = String(unknown);
    if (!value) {
      return false;
    }
    props->unknown_declarations = *value;
  }
  return true;
}

}  // namespace

// static
bool CPDF_RichTextJson::Parse(const ByteString& json_utf8,
                              CPDF_RichTextDocument* out,
                              bool* has_body) {
  *has_body = false;
  JsonValue root;
  JsonReader reader(json_utf8);
  if (!reader.Read(&root) || root.type != JsonValue::Type::kObject) {
    return false;
  }
  CPDF_RichTextDocument document;

  if (const JsonValue* body = root.Get("body")) {
    CPDF_RichTextStyleDelta delta;
    if (!ReadStyleDelta(body, &delta) ||
        !ReadParagraphProps(body, &document.body_paragraph)) {
      return false;
    }
    document.body = ApplyRichTextStyleDelta(CPDF_RichTextStyle(), delta);
    *has_body = true;
  }

  const JsonValue* paragraphs = root.Get("paragraphs");
  if (!paragraphs || paragraphs->type != JsonValue::Type::kArray) {
    return false;
  }
  for (const JsonValue& paragraph_value : paragraphs->array) {
    CPDF_RichTextParagraph paragraph;
    paragraph.props = document.body_paragraph;
    if (!ReadParagraphProps(&paragraph_value, &paragraph.props)) {
      return false;
    }
    const JsonValue* runs = paragraph_value.Get("runs");
    if (!runs || runs->type != JsonValue::Type::kArray) {
      return false;
    }
    for (const JsonValue& run_value : runs->array) {
      if (run_value.type != JsonValue::Type::kObject) {
        return false;
      }
      CPDF_RichTextRun run;
      std::optional<WideString> text = String(run_value.Get("text"));
      if (!text) {
        return false;
      }
      run.text = *text;
      if (const JsonValue* style = run_value.Get("style")) {
        if (!ReadStyleDelta(style, &run.style)) {
          return false;
        }
      }
      paragraph.runs.push_back(std::move(run));
    }
    document.paragraphs.push_back(std::move(paragraph));
  }
  document.source = CPDF_RichTextDocument::Source::kRC;
  *out = std::move(document);
  return true;
}
