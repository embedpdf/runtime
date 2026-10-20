// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fxge/cfx_opentypewriter.h"

#include <stdio.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxge/cfx_sfnt.h"

CFX_OpenTypeFont::CFX_OpenTypeFont() = default;
CFX_OpenTypeFont::CFX_OpenTypeFont(CFX_OpenTypeFont&&) noexcept = default;
CFX_OpenTypeFont& CFX_OpenTypeFont::operator=(CFX_OpenTypeFont&&) noexcept =
    default;
CFX_OpenTypeFont::~CFX_OpenTypeFont() = default;

namespace {

// Big-endian bytes, as every OpenType table and CFF structure is.
class Writer {
 public:
  // Each writes the low bytes of |value|: the caller has checked it fits.
  Writer& U8(uint64_t value) {
    bytes_.push_back(static_cast<uint8_t>(value));
    return *this;
  }
  Writer& U16(uint64_t value) { return U8(value >> 8).U8(value); }
  Writer& I16(int value) { return U16(static_cast<uint16_t>(value)); }
  Writer& U32(uint64_t value) { return U16(value >> 16).U16(value); }
  // A 16.16 fixed-point number.
  Writer& Fixed(double value) {
    return U32(static_cast<uint32_t>(
        static_cast<int32_t>(std::lround(value * 65536.0))));
  }
  Writer& Tag(const char* tag) {
    for (int i = 0; i < 4; ++i) {
      U8(static_cast<uint8_t>(tag[i]));
    }
    return *this;
  }
  Writer& Zeros(size_t count) {
    bytes_.insert(bytes_.end(), count, 0);
    return *this;
  }
  Writer& Bytes(const DataVector<uint8_t>& bytes) {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    return *this;
  }
  size_t size() const { return bytes_.size(); }
  DataVector<uint8_t> Take() { return std::move(bytes_); }

 private:
  DataVector<uint8_t> bytes_;
};

// ---- Bounds ----------------------------------------------------------------

struct Bounds {
  float x_min = std::numeric_limits<float>::max();
  float y_min = std::numeric_limits<float>::max();
  float x_max = std::numeric_limits<float>::lowest();
  float y_max = std::numeric_limits<float>::lowest();

  bool empty() const { return x_min > x_max; }
  void Add(const CFX_PointF& point) {
    x_min = std::min(x_min, point.x);
    y_min = std::min(y_min, point.y);
    x_max = std::max(x_max, point.x);
    y_max = std::max(y_max, point.y);
  }
  void Add(const Bounds& other) {
    if (!other.empty()) {
      Add(CFX_PointF(other.x_min, other.y_min));
      Add(CFX_PointF(other.x_max, other.y_max));
    }
  }
};

// The bounds of every point the outline names, control points included: the
// glyph lies inside them.
Bounds BoundsOf(const CFX_GlyphOutline& outline) {
  Bounds bounds;
  for (const CFX_GlyphOutline::Segment& segment : outline.segments) {
    const size_t points = segment.op == CFX_GlyphOutline::Op::kCubic ? 3 : 1;
    for (size_t i = 0; i < points; ++i) {
      bounds.Add(segment.points[i]);
    }
  }
  return bounds;
}

int Floor(float value) {
  return pdfium::saturated_cast<int>(std::floor(value));
}

int Ceil(float value) {
  return pdfium::saturated_cast<int>(std::ceil(value));
}

// ---- CFF -------------------------------------------------------------------

// A Type 2 charstring operand: an integer in its shortest form, anything
// else as 16.16 fixed point.
void CharstringNumber(DataVector<uint8_t>* out, float value) {
  const float rounded = std::round(value);
  if (std::fabs(value - rounded) < 1.0f / 65536 && std::fabs(rounded) < 32768) {
    const int number = static_cast<int>(rounded);
    if (number >= -107 && number <= 107) {
      out->push_back(static_cast<uint8_t>(number + 139));
    } else if (number >= 108 && number <= 1131) {
      const int offset = number - 108;
      out->push_back(static_cast<uint8_t>((offset >> 8) + 247));
      out->push_back(static_cast<uint8_t>(offset & 0xff));
    } else if (number >= -1131 && number <= -108) {
      const int offset = -number - 108;
      out->push_back(static_cast<uint8_t>((offset >> 8) + 251));
      out->push_back(static_cast<uint8_t>(offset & 0xff));
    } else {
      out->push_back(28);
      out->push_back(static_cast<uint8_t>((number >> 8) & 0xff));
      out->push_back(static_cast<uint8_t>(number & 0xff));
    }
    return;
  }
  const uint32_t fixed = static_cast<uint32_t>(
      static_cast<int32_t>(std::lround(static_cast<double>(value) * 65536.0)));
  out->push_back(255);
  out->push_back(static_cast<uint8_t>(fixed >> 24));
  out->push_back(static_cast<uint8_t>(fixed >> 16));
  out->push_back(static_cast<uint8_t>(fixed >> 8));
  out->push_back(static_cast<uint8_t>(fixed));
}

// What a charstring operand encodes as: the value a reader gets back.
float CharstringValue(float value) {
  const float rounded = std::round(value);
  if (std::fabs(value - rounded) < 1.0f / 65536 && std::fabs(rounded) < 32768) {
    return rounded;
  }
  return static_cast<float>(std::lround(static_cast<double>(value) * 65536.0) /
                            65536.0);
}

// The glyph as Type 2 operators: rmoveto, rlineto and rrcurveto from the
// pen's position, then endchar. Each contour closes at the next rmoveto or
// at endchar. No width: the advance is hmtx's.
DataVector<uint8_t> Charstring(const CFX_GlyphOutline& outline) {
  constexpr uint8_t kRLineTo = 5;
  constexpr uint8_t kRRCurveTo = 8;
  constexpr uint8_t kEndChar = 14;
  constexpr uint8_t kRMoveTo = 21;
  DataVector<uint8_t> out;
  // The pen as a reader will have it, so rounding never accumulates.
  CFX_PointF pen;
  auto relative = [&](const CFX_PointF& point) {
    const float dx = CharstringValue(point.x - pen.x);
    const float dy = CharstringValue(point.y - pen.y);
    CharstringNumber(&out, dx);
    CharstringNumber(&out, dy);
    pen = CFX_PointF(pen.x + dx, pen.y + dy);
  };
  for (const CFX_GlyphOutline::Segment& segment : outline.segments) {
    switch (segment.op) {
      case CFX_GlyphOutline::Op::kMove:
        relative(segment.points[0]);
        out.push_back(kRMoveTo);
        break;
      case CFX_GlyphOutline::Op::kLine:
        relative(segment.points[0]);
        out.push_back(kRLineTo);
        break;
      case CFX_GlyphOutline::Op::kCubic:
        relative(segment.points[0]);
        relative(segment.points[1]);
        relative(segment.points[2]);
        out.push_back(kRRCurveTo);
        break;
    }
  }
  out.push_back(kEndChar);
  return out;
}

// A CFF DICT integer operand in its shortest form.
void DictInteger(Writer* out, int number) {
  if (number >= -107 && number <= 107) {
    out->U8(number + 139);
  } else if (number >= 108 && number <= 1131) {
    out->U8(((number - 108) >> 8) + 247).U8((number - 108) & 0xff);
  } else if (number >= -1131 && number <= -108) {
    out->U8(((-number - 108) >> 8) + 251).U8((-number - 108) & 0xff);
  } else if (number >= -32768 && number <= 32767) {
    out->U8(28).I16(number);
  } else {
    out->U8(29).U32(static_cast<uint32_t>(number));
  }
}

// An offset operand, always five bytes, so a DICT's size doesn't depend on
// the offsets it holds.
void DictOffset(Writer* out, uint32_t offset) {
  out->U8(29).U32(offset);
}

// A CFF DICT real operand: its decimal digits as nibbles.
void DictReal(Writer* out, double value) {
  char text[32];
  snprintf(text, sizeof(text), "%.10g", value);
  std::vector<uint8_t> nibbles;
  for (const char* c = text; *c; ++c) {
    if (*c >= '0' && *c <= '9') {
      nibbles.push_back(static_cast<uint8_t>(*c - '0'));
    } else if (*c == '.') {
      nibbles.push_back(0xa);
    } else if (*c == '-') {
      nibbles.push_back(0xe);
    } else if (*c == 'e' || *c == 'E') {
      if (c[1] == '-') {
        nibbles.push_back(0xc);
        ++c;
      } else {
        nibbles.push_back(0xb);
        if (c[1] == '+') {
          ++c;
        }
      }
    }
  }
  nibbles.push_back(0xf);
  if (nibbles.size() % 2) {
    nibbles.push_back(0xf);
  }
  out->U8(30);
  for (size_t i = 0; i < nibbles.size(); i += 2) {
    out->U8((nibbles[i] << 4) | nibbles[i + 1]);
  }
}

// A CFF INDEX of |items|.
void Index(Writer* out, const std::vector<DataVector<uint8_t>>& items) {
  out->U16(pdfium::checked_cast<uint16_t>(items.size()));
  if (items.empty()) {
    return;
  }
  size_t total = 1;
  for (const DataVector<uint8_t>& item : items) {
    total += item.size();
  }
  const uint8_t offset_size = total <= 0xff       ? 1
                              : total <= 0xffff   ? 2
                              : total <= 0xffffff ? 3
                                                  : 4;
  out->U8(offset_size);
  auto offset = [&](size_t value) {
    for (int shift = (offset_size - 1) * 8; shift >= 0; shift -= 8) {
      out->U8(static_cast<uint32_t>(value >> shift));
    }
  };
  size_t at = 1;
  offset(at);
  for (const DataVector<uint8_t>& item : items) {
    at += item.size();
    offset(at);
  }
  for (const DataVector<uint8_t>& item : items) {
    out->Bytes(item);
  }
}

DataVector<uint8_t> BytesOf(const ByteString& text) {
  return DataVector<uint8_t>(text.unsigned_span().begin(),
                             text.unsigned_span().end());
}

// The first string ID after CFF's standard strings.
constexpr int kFirstCustomString = 391;

// A name-keyed CFF font: glyph names "g1", "g2", … (the cmap names the
// characters), no hints, no subroutines.
DataVector<uint8_t> CffTable(const CFX_OpenTypeFont& font,
                             const Bounds& font_bounds) {
  const size_t glyph_count = font.glyphs.size();
  std::vector<DataVector<uint8_t>> names = {BytesOf(font.postscript_name)};
  std::vector<DataVector<uint8_t>> strings;
  for (size_t glyph = 1; glyph < glyph_count; ++glyph) {
    strings.push_back(BytesOf(ByteString::Format("g%zu", glyph)));
  }
  std::vector<DataVector<uint8_t>> charstrings;
  for (const CFX_OpenTypeFont::Glyph& glyph : font.glyphs) {
    charstrings.push_back(Charstring(glyph.outline));
  }

  Writer charset;
  charset.U8(0);
  for (size_t glyph = 1; glyph < glyph_count; ++glyph) {
    charset.U16(pdfium::checked_cast<uint16_t>(kFirstCustomString + glyph - 1));
  }
  Writer private_dict;
  DictInteger(&private_dict, 0);
  private_dict.U8(20);  // defaultWidthX
  DictInteger(&private_dict, 0);
  private_dict.U8(21);  // nominalWidthX

  // The top DICT, with the offsets of what follows it.
  auto top_dict = [&](uint32_t charset_offset, uint32_t charstrings_offset,
                      uint32_t private_offset) {
    Writer dict;
    DictInteger(&dict, font_bounds.empty() ? 0 : Floor(font_bounds.x_min));
    DictInteger(&dict, font_bounds.empty() ? 0 : Floor(font_bounds.y_min));
    DictInteger(&dict, font_bounds.empty() ? 0 : Ceil(font_bounds.x_max));
    DictInteger(&dict, font_bounds.empty() ? 0 : Ceil(font_bounds.y_max));
    dict.U8(5);  // FontBBox
    if (font.units_per_em != 1000) {
      const double scale = 1.0 / font.units_per_em;
      for (double value : {scale, 0.0, 0.0, scale, 0.0, 0.0}) {
        DictReal(&dict, value);
      }
      dict.U8(12).U8(7);  // FontMatrix
    }
    DictOffset(&dict, charset_offset);
    dict.U8(15);  // charset
    DictOffset(&dict, charstrings_offset);
    dict.U8(17);  // CharStrings
    DictInteger(&dict, pdfium::checked_cast<int>(private_dict.size()));
    DictOffset(&dict, private_offset);
    dict.U8(18);  // Private
    return dict.Take();
  };

  // Lay it out once to learn where each part starts, then write it.
  auto layout = [&](uint32_t charset_offset, uint32_t charstrings_offset,
                    uint32_t private_offset, Writer* out) {
    out->U8(1).U8(0).U8(4).U8(4);  // header: version 1.0, offsets of 4 bytes
    Index(out, names);
    Index(out, {top_dict(charset_offset, charstrings_offset, private_offset)});
    Index(out, strings);
    Index(out, {});  // global subroutines
  };
  Writer probe;
  layout(0, 0, 0, &probe);
  const uint32_t charset_offset = pdfium::checked_cast<uint32_t>(probe.size());
  const uint32_t charstrings_offset =
      charset_offset + pdfium::checked_cast<uint32_t>(charset.size());
  Writer charstrings_index;
  Index(&charstrings_index, charstrings);
  const uint32_t private_offset =
      charstrings_offset +
      pdfium::checked_cast<uint32_t>(charstrings_index.size());

  Writer cff;
  layout(charset_offset, charstrings_offset, private_offset, &cff);
  cff.Bytes(charset.Take());
  cff.Bytes(charstrings_index.Take());
  cff.Bytes(private_dict.Take());
  return cff.Take();
}

// ---- The other tables
// --------------------------------------------------------

// cmap: Windows Unicode BMP (format 4), runs of consecutive characters on
// consecutive glyphs as one segment; when that doesn't fit its 16-bit
// lengths, Windows Unicode full (format 12) instead.
DataVector<uint8_t> CmapTable(
    const std::vector<std::pair<uint32_t, uint16_t>>& cmap) {
  struct Run {
    uint32_t start;
    uint32_t end;
    uint16_t glyph;
  };
  std::vector<Run> runs;
  for (const auto& [scalar, glyph] : cmap) {
    if (!runs.empty() && runs.back().end + 1 == scalar &&
        runs.back().glyph + (scalar - runs.back().start) == glyph) {
      runs.back().end = scalar;
      continue;
    }
    runs.push_back({scalar, scalar, glyph});
  }
  const bool bmp = cmap.empty() || cmap.back().first <= 0xfffe;
  const size_t seg_count = runs.size() + 1;
  const size_t format4_length = 16 + seg_count * 8;
  Writer table;
  table.U16(0).U16(1);
  if (bmp && format4_length <= 0xffff) {
    table.U16(3).U16(1).U32(12);
    size_t entry_selector = 0;
    while ((size_t{2} << entry_selector) <= seg_count) {
      ++entry_selector;
    }
    const size_t search_range = size_t{2} << entry_selector;
    table.U16(4).U16(format4_length).U16(0);
    table.U16(seg_count * 2)
        .U16(search_range)
        .U16(entry_selector)
        .U16(seg_count * 2 - search_range);
    for (const Run& run : runs) {
      table.U16(run.end);
    }
    table.U16(0xffff).U16(0);
    for (const Run& run : runs) {
      table.U16(run.start);
    }
    table.U16(0xffff);
    for (const Run& run : runs) {
      table.U16((run.glyph - run.start) & 0xffff);
    }
    table.U16(1);
    table.Zeros(seg_count * 2);  // idRangeOffset
    return table.Take();
  }
  table.U16(3).U16(10).U32(12);
  table.U16(12).U16(0).U32(16 + runs.size() * 12).U32(0).U32(runs.size());
  for (const Run& run : runs) {
    table.U32(run.start).U32(run.end).U32(run.glyph);
  }
  return table.Take();
}

DataVector<uint8_t> NameTable(
    const std::vector<std::pair<uint16_t, ByteString>>& records) {
  Writer table;
  table.U16(0).U16(records.size()).U16(6 + records.size() * 12);
  size_t offset = 0;
  for (const auto& [name_id, text] : records) {
    table.U16(3)
        .U16(1)
        .U16(0x409)
        .U16(name_id)
        .U16(text.GetLength() * 2)
        .U16(offset);
    offset += text.GetLength() * 2;
  }
  for (const auto& [name_id, text] : records) {
    for (char c : text) {
      table.U16(static_cast<uint8_t>(c));
    }
  }
  return table.Take();
}

ByteString StyleName(const CFX_OpenTypeFont& font) {
  const bool bold = font.weight >= 600;
  if (bold && font.italic) {
    return "Bold Italic";
  }
  return bold ? "Bold" : font.italic ? "Italic" : "Regular";
}

}  // namespace

DataVector<uint8_t> WriteOpenTypeCFF(const CFX_OpenTypeFont& font) {
  const size_t glyph_count = font.glyphs.size();
  if (glyph_count == 0 || glyph_count > 0xffff || font.units_per_em < 16) {
    return {};
  }
  const float em = font.units_per_em / 1000.0f;
  auto units = [&](float thousandths) {
    return static_cast<int>(std::lround(thousandths * em));
  };

  Bounds font_bounds;
  std::vector<Bounds> glyph_bounds;
  int advance_max = 0;
  int min_lsb = std::numeric_limits<int>::max();
  int min_rsb = std::numeric_limits<int>::max();
  int x_max_extent = 0;
  int advance_sum = 0;
  int advance_count = 0;
  for (const CFX_OpenTypeFont::Glyph& glyph : font.glyphs) {
    glyph_bounds.push_back(BoundsOf(glyph.outline));
    const Bounds& bounds = glyph_bounds.back();
    font_bounds.Add(bounds);
    advance_max = std::max<int>(advance_max, glyph.advance);
    if (glyph.advance > 0) {
      advance_sum += glyph.advance;
      ++advance_count;
    }
    if (!bounds.empty()) {
      min_lsb = std::min(min_lsb, Floor(bounds.x_min));
      min_rsb = std::min(min_rsb, glyph.advance - Ceil(bounds.x_max));
      x_max_extent = std::max(x_max_extent, Ceil(bounds.x_max));
    }
  }
  if (min_lsb == std::numeric_limits<int>::max()) {
    min_lsb = 0;
    min_rsb = 0;
  }
  const bool bold = font.weight >= 600;

  std::map<ByteString, DataVector<uint8_t>> tables;
  tables["CFF "] = CffTable(font, font_bounds);

  Writer head;
  head.Fixed(1)
      .Fixed(1)
      .U32(0)  // checkSumAdjustment, set once the font is assembled
      .U32(0x5f0f3cf5)
      .U16(0x0003)
      .U16(font.units_per_em)
      .Zeros(16)  // created and modified: none, so the bytes never change
      .I16(font_bounds.empty() ? 0 : Floor(font_bounds.x_min))
      .I16(font_bounds.empty() ? 0 : Floor(font_bounds.y_min))
      .I16(font_bounds.empty() ? 0 : Ceil(font_bounds.x_max))
      .I16(font_bounds.empty() ? 0 : Ceil(font_bounds.y_max))
      .U16((bold ? 1 : 0) | (font.italic ? 2 : 0))
      .U16(8)
      .I16(2)
      .I16(0)
      .I16(0);
  tables["head"] = head.Take();

  Writer hhea;
  hhea.Fixed(1)
      .I16(font.ascent)
      .I16(font.descent)
      .I16(0)
      .U16(advance_max)
      .I16(min_lsb)
      .I16(min_rsb)
      .I16(x_max_extent)
      .I16(1)
      .I16(0)
      .I16(0)
      .Zeros(8)
      .I16(0)
      .U16(glyph_count);
  tables["hhea"] = hhea.Take();

  Writer hmtx;
  for (size_t i = 0; i < glyph_count; ++i) {
    hmtx.U16(font.glyphs[i].advance)
        .I16(glyph_bounds[i].empty() ? 0 : Floor(glyph_bounds[i].x_min));
  }
  tables["hmtx"] = hmtx.Take();

  Writer maxp;
  maxp.U32(0x00005000).U16(glyph_count);
  tables["maxp"] = maxp.Take();

  uint32_t first_char = 0xffff;
  uint32_t last_char = 0;
  for (const auto& [scalar, glyph] : font.cmap) {
    first_char = std::min<uint32_t>(first_char, scalar);
    last_char = std::max<uint32_t>(last_char, scalar);
  }
  if (font.cmap.empty()) {
    first_char = 0;
  }
  Writer os2;
  os2.U16(4)
      .I16(advance_count ? advance_sum / advance_count : 0)
      .U16(std::clamp(font.weight, 100, 900))
      .U16(5)
      .U16(0)  // fsType: no restriction; the engine decides who receives it
      .I16(units(650))
      .I16(units(600))
      .I16(0)
      .I16(units(75))
      .I16(units(650))
      .I16(units(600))
      .I16(0)
      .I16(units(350))
      .I16(units(50))
      .I16(units(250))
      .I16(0)
      .Zeros(10)  // panose
      .Zeros(16)  // ulUnicodeRange
      .Tag("EPDF")
      .U16((font.italic ? 1 : 0) | (bold ? 0x20 : 0) |
           (!bold && !font.italic ? 0x40 : 0) | 0x80)
      .U16(std::min<uint32_t>(first_char, 0xffff))
      .U16(std::min<uint32_t>(last_char, 0xffff))
      .I16(font.ascent)
      .I16(font.descent)
      .I16(0)
      .U16(std::max(font.ascent, 0))
      .U16(std::max(-font.descent, 0))
      .U32(1)  // ulCodePageRange1: Latin 1
      .U32(0)
      .I16(0)
      .I16(0)
      .U16(0)
      .U16(32)
      .U16(1);
  tables["OS/2"] = os2.Take();

  Writer post;
  post.Fixed(3)
      .Fixed(font.italic_angle)
      .I16(units(-100))
      .I16(units(50))
      .U32(font.fixed_pitch ? 1 : 0)
      .Zeros(16);
  tables["post"] = post.Take();

  const ByteString style = StyleName(font);
  const ByteString full_name =
      style == "Regular" ? font.family : font.family + " " + style;
  tables["name"] = NameTable({{1, font.family},
                              {2, style},
                              {3, font.postscript_name + ";EmbedPDF"},
                              {4, full_name},
                              {6, font.postscript_name}});
  tables["cmap"] = CmapTable(font.cmap);
  return WriteSfnt(0x4f54544f, tables);  // 'OTTO'
}
