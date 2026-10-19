// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fxge/cfx_glyphoutline.h"

#include <optional>

#include "core/fxge/cfx_face.h"

#if defined(PDF_ENABLE_FREETYPE)
#include "core/fxge/freetype/fx_freetype.h"
#endif

#if defined(PDF_ENABLE_FREETYPE)
namespace {

struct Decomposition {
  CFX_GlyphOutline* outline;
  CFX_PointF current;
};

CFX_PointF PointOf(const FT_Vector* vector) {
  return CFX_PointF(static_cast<float>(vector->x),
                    static_cast<float>(vector->y));
}

int MoveTo(const FT_Vector* to, void* user) {
  auto* decomposition = static_cast<Decomposition*>(user);
  decomposition->current = PointOf(to);
  decomposition->outline->segments.push_back(
      {CFX_GlyphOutline::Op::kMove, {decomposition->current, {}, {}}});
  return 0;
}

int LineTo(const FT_Vector* to, void* user) {
  auto* decomposition = static_cast<Decomposition*>(user);
  decomposition->current = PointOf(to);
  decomposition->outline->segments.push_back(
      {CFX_GlyphOutline::Op::kLine, {decomposition->current, {}, {}}});
  return 0;
}

// A quadratic Bézier is the cubic whose control points are two thirds of the
// way from each end to its one control point.
int ConicTo(const FT_Vector* control, const FT_Vector* to, void* user) {
  auto* decomposition = static_cast<Decomposition*>(user);
  const CFX_PointF from = decomposition->current;
  const CFX_PointF via = PointOf(control);
  const CFX_PointF end = PointOf(to);
  const CFX_PointF first(from.x + (via.x - from.x) * 2 / 3,
                         from.y + (via.y - from.y) * 2 / 3);
  const CFX_PointF second(end.x + (via.x - end.x) * 2 / 3,
                          end.y + (via.y - end.y) * 2 / 3);
  decomposition->current = end;
  decomposition->outline->segments.push_back(
      {CFX_GlyphOutline::Op::kCubic, {first, second, end}});
  return 0;
}

int CubicTo(const FT_Vector* first,
            const FT_Vector* second,
            const FT_Vector* to,
            void* user) {
  auto* decomposition = static_cast<Decomposition*>(user);
  decomposition->current = PointOf(to);
  decomposition->outline->segments.push_back(
      {CFX_GlyphOutline::Op::kCubic,
       {PointOf(first), PointOf(second), decomposition->current}});
  return 0;
}

}  // namespace
#endif  // defined(PDF_ENABLE_FREETYPE)

std::optional<CFX_GlyphOutline> CFX_Face::LoadUnscaledGlyphOutline(
    uint32_t glyph_index) {
#if defined(PDF_ENABLE_FREETYPE)
  FT_FaceRec* rec = GetRec();
  if (!rec) {
    return std::nullopt;
  }
  if (FT_Load_Glyph(
          rec, glyph_index,
          FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) {
    return std::nullopt;
  }
  if (rec->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
    return std::nullopt;
  }
  CFX_GlyphOutline outline;
  Decomposition decomposition{&outline, CFX_PointF()};
  FT_Outline_Funcs funcs;
  funcs.move_to = MoveTo;
  funcs.line_to = LineTo;
  funcs.conic_to = ConicTo;
  funcs.cubic_to = CubicTo;
  funcs.shift = 0;
  funcs.delta = 0;
  if (FT_Outline_Decompose(&rec->glyph->outline, &funcs, &decomposition)) {
    return std::nullopt;
  }
  return outline;
#else
  return std::nullopt;
#endif  // defined(PDF_ENABLE_FREETYPE)
}
