// Copyright 2025 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/edit/cpdf_text_redactor.h"

#include <cmath>
#include <sstream>
#include <utility>
#include <vector>
#include <algorithm>

#include "core/fpdfapi/edit/cpdf_contentstream_write_utils.h"
#include "core/fpdfapi/edit/cpdf_path_redactor.h"
#include "core/fpdfapi/edit/cpdf_pagecontentgenerator.h"
#include "core/fpdfapi/edit/cpdf_pagecontentmanager.h"
#include "core/fpdfapi/edit/cpdf_redaction_mark_sanitizer.h"
#include "core/fpdfapi/font/cpdf_cidfont.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_shadingobject.h"
#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_dib.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcodec/jpeg/jpegmodule.h"
#include "core/fxcodec/scanlinedecoder.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/dib/fx_dib.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/span.h"

namespace {

static void AddBlackOverlayPaths(CPDF_Page* page,
                                 pdfium::span<const RedactRegion> regions) {
  if (!page || regions.empty())
    return;

  for (const RedactRegion& region : regions) {
    auto po = std::make_unique<CPDF_PathObject>();
    po->set_stroke(false);
    po->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
    if (region.has_quad) {
      // Fill the exact oriented mark (ring order US → UE → LE → LS).
      po->path().AppendPoint(region.quad[0], CFX_Path::Point::Type::kMove);
      po->path().AppendPoint(region.quad[1], CFX_Path::Point::Type::kLine);
      po->path().AppendPoint(region.quad[3], CFX_Path::Point::Type::kLine);
      po->path().AppendPointAndClose(region.quad[2],
                                     CFX_Path::Point::Type::kLine);
    } else {
      // left/bottom/right/top in PAGE USER SPACE
      po->path().AppendFloatRect(region.bbox);
    }
    po->SetPathMatrix(CFX_Matrix());      // identity
    po->CalcBoundingBox();
    po->SetDirty(true);
    page->AppendPageObject(std::move(po));  // appended last => paints on top
  }
}

enum class RedactOutcome { kUnchanged, kModified, kRemovedAll };

inline bool Intersects(const CFX_FloatRect& a, const CFX_FloatRect& b) {
  return a.right > b.left && a.left < b.right && a.top > b.bottom &&
         a.bottom < b.top;
}

inline bool IntersectsAny(const CFX_FloatRect& box,
                          pdfium::span<const CFX_FloatRect> rects) {
  for (const auto& r : rects) {
    if (Intersects(box, r))
      return true;
  }
  return false;
}

/* ── oriented-region geometry ─────────────────────────────────────────────
 * Region and glyph quads are stored in FS_QUADPOINTSF SLOT order
 * (upper-start, upper-end, lower-start, lower-end); the convex RING visits
 * slots {0, 1, 3, 2}. Intersection is a separating-axis test with the same
 * open-interval semantics as `Intersects` above: touching edges don't hit. */

constexpr int kQuadRing[4] = {0, 1, 3, 2};
constexpr float kRegionQuadTolerance = 1e-4f;

std::array<CFX_PointF, 4> QuadFromRect(const CFX_FloatRect& r) {
  return {CFX_PointF(r.left, r.top), CFX_PointF(r.right, r.top),
          CFX_PointF(r.left, r.bottom), CFX_PointF(r.right, r.bottom)};
}

CFX_FloatRect BBoxOfQuadCorners(const std::array<CFX_PointF, 4>& q) {
  for (const CFX_PointF& p : q) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
      return CFX_FloatRect();
    }
  }
  CFX_FloatRect bbox(q[0]);
  for (size_t i = 1; i < q.size(); ++i) {
    bbox.UpdateRect(q[i]);
  }
  return bbox;
}

bool ProjectionsSeparated(const std::array<CFX_PointF, 4>& a,
                          const std::array<CFX_PointF, 4>& b,
                          const CFX_PointF& axis) {
  if (axis.x == 0 && axis.y == 0)
    return false;  // degenerate edge — not a separating candidate
  float a_min = 0, a_max = 0, b_min = 0, b_max = 0;
  for (int i = 0; i < 4; ++i) {
    const float pa = a[i].x * axis.x + a[i].y * axis.y;
    const float pb = b[i].x * axis.x + b[i].y * axis.y;
    if (i == 0) {
      a_min = a_max = pa;
      b_min = b_max = pb;
    } else {
      a_min = std::min(a_min, pa);
      a_max = std::max(a_max, pa);
      b_min = std::min(b_min, pb);
      b_max = std::max(b_max, pb);
    }
  }
  return a_max <= b_min || b_max <= a_min;  // open interval: touch == miss
}

bool HasSeparatingAxis(const std::array<CFX_PointF, 4>& a,
                       const std::array<CFX_PointF, 4>& b) {
  for (int i = 0; i < 4; ++i) {
    const CFX_PointF& p = a[kQuadRing[i]];
    const CFX_PointF& q = a[kQuadRing[(i + 1) % 4]];
    const CFX_PointF axis(-(q.y - p.y), q.x - p.x);
    if (ProjectionsSeparated(a, b, axis))
      return true;
  }
  return false;
}

bool ConvexQuadsIntersect(const std::array<CFX_PointF, 4>& a,
                          const std::array<CFX_PointF, 4>& b) {
  return !HasSeparatingAxis(a, b) && !HasSeparatingAxis(b, a);
}

bool FiniteCorners(const std::array<CFX_PointF, 4>& q) {
  for (const CFX_PointF& p : q) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y))
      return false;
  }
  return true;
}

// Well-formed zigzag reading: same validation the AP generators apply —
// finite, usable edge lengths, opposite edges parallel same-direction,
// consistent winding on both ends.
bool CornersWellFormed(const std::array<CFX_PointF, 4>& q) {
  if (!FiniteCorners(q))
    return false;
  const CFX_PointF upper = q[1] - q[0];
  const CFX_PointF lower = q[3] - q[2];
  const CFX_PointF start_side = q[2] - q[0];
  const CFX_PointF end_side = q[3] - q[1];
  const float len_u = std::hypot(upper.x, upper.y);
  const float len_l = std::hypot(lower.x, lower.y);
  const float len_s = std::hypot(start_side.x, start_side.y);
  const float len_e = std::hypot(end_side.x, end_side.y);
  if (len_u <= kRegionQuadTolerance || len_l <= kRegionQuadTolerance ||
      len_s <= kRegionQuadTolerance || len_e <= kRegionQuadTolerance) {
    return false;
  }
  if (upper.x * lower.x + upper.y * lower.y <= 0)
    return false;
  if (start_side.x * end_side.x + start_side.y * end_side.y <= 0)
    return false;
  const float start_area = upper.x * start_side.y - upper.y * start_side.x;
  const float end_area = lower.x * end_side.y - lower.y * end_side.x;
  if (std::fabs(start_area) <= kRegionQuadTolerance * len_u * len_s)
    return false;
  if (std::fabs(end_area) <= kRegionQuadTolerance * len_l * len_e)
    return false;
  return std::signbit(start_area) == std::signbit(end_area);
}

bool CornersAxisAligned(const std::array<CFX_PointF, 4>& q) {
  const float scale = std::max(
      {1.0f, std::hypot(q[1].x - q[0].x, q[1].y - q[0].y),
       std::hypot(q[3].x - q[2].x, q[3].y - q[2].y),
       std::hypot(q[2].x - q[0].x, q[2].y - q[0].y),
       std::hypot(q[3].x - q[1].x, q[3].y - q[1].y)});
  const float tol = kRegionQuadTolerance * scale;
  return std::fabs(q[0].y - q[1].y) <= tol && std::fabs(q[2].y - q[3].y) <= tol &&
         std::fabs(q[0].x - q[2].x) <= tol && std::fabs(q[1].x - q[3].x) <= tol;
}

std::vector<CFX_FloatRect> BBoxesOfRegions(
    pdfium::span<const RedactRegion> regions) {
  std::vector<CFX_FloatRect> bboxes;
  bboxes.reserve(regions.size());
  for (const RedactRegion& r : regions)
    bboxes.push_back(r.bbox);
  return bboxes;
}

// Compute a glyph's bbox in PAGE USER SPACE.
//
// Note: CPDF_TextObject::GetItemInfo() origin_ is already adjusted for vertical
// writing, so we do not apply any extra vertical origin shift here.
CFX_FloatRect GlyphLocalBox(const CPDF_TextObject* to,
                            CPDF_Font* font,
                            uint32_t code,
                            const CPDF_TextObject::Item& it) {
  FX_RECT r_font_units = font->GetCharBBox(code);
  const float fs = to->GetFontSize();

  CFX_FloatRect glyph_box(
      r_font_units.left * fs / 1000.0f, r_font_units.bottom * fs / 1000.0f,
      r_font_units.right * fs / 1000.0f, r_font_units.top * fs / 1000.0f);

  // Position inside the text object’s local space.
  glyph_box.left += it.origin_.x;
  glyph_box.right += it.origin_.x;
  glyph_box.bottom += it.origin_.y;
  glyph_box.top += it.origin_.y;
  return glyph_box;
}

// `text_matrix` is the object's matrix AS PARSED. Callers must not read it
// back from the object while walking glyphs: the object is never mutated
// mid-walk (see RedactTextObjectMulti).
CFX_FloatRect GlyphBBoxInPage(const CPDF_TextObject* to,
                              CPDF_Font* font,
                              uint32_t code,
                              const CPDF_TextObject::Item& it,
                              const CFX_Matrix& text_matrix,
                              const CFX_Matrix& parent_to_page) {
  CFX_FloatRect glyph_box = GlyphLocalBox(to, font, code, it);

  // Text space to parent (page or form) space, then parent to page.
  glyph_box = text_matrix.TransformRect(glyph_box);
  return parent_to_page.TransformRect(glyph_box);
}

// Advance in thousandths for a single code, matching how PDFium applies widths
// and char/word spacing during layout.
float AdvanceThousandths(const CPDF_TextObject* to,
                         CPDF_Font* font,
                         uint32_t code) {
  float w_th = 0.0f;

  if (const CPDF_CIDFont* cid = font->AsCIDFont(); cid && cid->IsVertWriting()) {
    const uint16_t c = cid->CIDFromCharCode(code);
    w_th = static_cast<float>(cid->GetVertWidth(c));
  } else {
    w_th = static_cast<float>(font->GetCharWidthF(code));
  }

  const float fs = to->GetFontSize();

  // Apply word space only for ASCII space in typical (non-special) cases.
  if (code == ' ') {
    const CPDF_CIDFont* cid = font->AsCIDFont();
    if (!cid || cid->GetCharSize(' ') == 1)
      w_th += to->GetWordSpace() * 1000.0f / fs;
  }

  // Always apply char space.
  w_th += to->GetCharSpace() * 1000.0f / fs;
  return w_th;
}

// Round to nearest integer thousandth for stable TJ outputs.
inline int32_t RoundThousandths(float v) {
  return v >= 0 ? static_cast<int32_t>(v + 0.5f)
                : static_cast<int32_t>(v - 0.5f);
}

// Small deadband to tame float fuzz when synthesizing TJ from origins.
constexpr float kTJDeadband = 0.25f;  // thousandths

// State for building a TJ array from kept glyph runs.
struct RedactionState {
  CPDF_Font* font = nullptr;

  // Output buffers for SetSegments(): strings[i] followed by kernings[i] between
  // strings[i] and strings[i+1].
  std::vector<ByteString> strings;
  std::vector<float> kernings;

  // Accumulates original file TJ numbers and removal advances between kept runs.
  float kerning_accumulator = 0.0f;
  bool has_explicit_kerning = false;

  // For synthesized kerning from layout positions when no explicit TJ exists.
  // Raw writing-axis position (CPDF_TextObject::GetCharPositions()) of the
  // previous kept glyph: valid for horizontal and vertical writing alike,
  // unlike Item::origin_ which carries the per-glyph vertical-origin shift.
  float prev_glyph_pos = 0.0f;
  uint32_t prev_glyph_code = 0;

  void ResetBetweenRuns() {
    kerning_accumulator = 0.0f;
    has_explicit_kerning = false;
  }

  void AppendKeptGlyph(const CPDF_TextObject::Item& item, float raw_pos) {
    DCHECK(font);
    DCHECK(!strings.empty());
    font->AppendChar(&strings.back(), item.char_code_);
    prev_glyph_pos = raw_pos;
    prev_glyph_code = item.char_code_;
  }
};

// Push a kerning (integer thousandths) and open a new (initially empty) run.
void FlushSegment(RedactionState* s, float kerning_mth) {
  const int32_t rounded = RoundThousandths(kerning_mth);
  if (rounded == 0)
    return;
  s->kernings.push_back(static_cast<float>(rounded));
  s->strings.push_back(ByteString());  // next glyphs will fill this
}

RedactOutcome RedactTextObjectMulti(CPDF_TextObject* to,
                                    pdfium::span<const RedactRegion> regions,
                                    pdfium::span<const CFX_FloatRect> region_bboxes,
                                    const CFX_Matrix& parent_to_page) {
  CPDF_Font* font = to->GetFont();
  if (!font)
    return RedactOutcome::kUnchanged;

  const CPDF_CIDFont* cid = font->AsCIDFont();
  const bool is_vert = cid && cid->IsVertWriting();
  const float fs = to->GetFontSize();

  // Oriented dispatch. The legacy AABB test stays byte-identical for the
  // axis-aligned world (rect-only regions × upright text); a rotated marked
  // region OR rotated text switches glyph decisions to exact quad-vs-quad
  // intersection, so a rotated mark neither leaks its own glyphs nor
  // destroys the neighbouring line that merely shares its bounding box.
  bool any_region_quad = false;
  for (const RedactRegion& region : regions) {
    if (region.has_quad) {
      any_region_quad = true;
      break;
    }
  }
  // INVARIANT: every glyph is hit-tested against the matrix the object was
  // parsed with. The object is not mutated until the walk is over. Mutating
  // it mid-walk (an earlier version shifted the text matrix at the first
  // kept glyph) displaced every later glyph box along the line, so a second
  // region on the same text object removed the wrong glyphs: redacted text
  // survived under the overlay while neighbouring text was destroyed.
  const CFX_Matrix original_tm = to->GetTextMatrix();
  // The leading gap closure is accumulated here and applied exactly once
  // after the walk.
  CFX_Matrix final_tm = original_tm;
  auto local_to_page = [&](const CFX_PointF& p) {
    return parent_to_page.Transform(original_tm.Transform(p));
  };
  const CFX_PointF basis_o = local_to_page(CFX_PointF(0, 0));
  const CFX_PointF basis_x = local_to_page(CFX_PointF(1, 0)) - basis_o;
  const CFX_PointF basis_y = local_to_page(CFX_PointF(0, 1)) - basis_o;
  const float len_x = std::hypot(basis_x.x, basis_x.y);
  const float len_y = std::hypot(basis_y.x, basis_y.y);
  const bool object_upright =
      len_x > 0 && len_y > 0 && basis_x.x > 0 && basis_y.y > 0 &&
      std::fabs(basis_x.y) <= kRegionQuadTolerance * len_x &&
      std::fabs(basis_y.x) <= kRegionQuadTolerance * len_y;
  const bool oriented = any_region_quad || !object_upright;

  bool any_kept = false;
  bool any_removed = false;

  RedactionState st;
  st.font = font;
  st.strings.push_back(ByteString());  // start first run

  const size_t n = to->CountItems();
  for (size_t i = 0; i < n; ++i) {
    const CPDF_TextObject::Item it = to->GetItemInfo(i);

    // Original file kerning separator inside TJ.
    if (it.char_code_ == CPDF_Font::kInvalidCharCode) {
      float adj = 0.0f;
      if (to->GetSeparatorAdjustment(i, &adj)) {
        st.kerning_accumulator += adj;  // keep sign; PDF TJ semantics
        st.has_explicit_kerning = true;
      }
      continue;
    }

    // Decide keep/remove by intersection.
    bool hit;
    if (!oriented) {
      const CFX_FloatRect gbox = GlyphBBoxInPage(
          to, font, it.char_code_, it, original_tm, parent_to_page);
      hit = IntersectsAny(gbox, region_bboxes);
    } else {
      // The glyph's exact oriented cell: the SAME local box the AABB path
      // uses, with its corners transformed individually instead of collapsed.
      const CFX_FloatRect local = GlyphLocalBox(to, font, it.char_code_, it);
      const std::array<CFX_PointF, 4> glyph_quad = {
          local_to_page(CFX_PointF(local.left, local.top)),
          local_to_page(CFX_PointF(local.right, local.top)),
          local_to_page(CFX_PointF(local.left, local.bottom)),
          local_to_page(CFX_PointF(local.right, local.bottom))};
      const CFX_FloatRect glyph_bbox = BBoxOfQuadCorners(glyph_quad);
      hit = false;
      for (const RedactRegion& region : regions) {
        if (!Intersects(glyph_bbox, region.bbox))
          continue;
        const std::array<CFX_PointF, 4> region_quad =
            region.has_quad ? region.quad : QuadFromRect(region.bbox);
        if (ConvexQuadsIntersect(glyph_quad, region_quad)) {
          hit = true;
          break;
        }
      }
    }

    if (hit) {
      // Merge the removed glyph's advance into the pending kerning pool.
      st.kerning_accumulator -= AdvanceThousandths(to, font, it.char_code_);
      any_removed = true;
      continue;
    }

    // Raw layout position of this glyph along the writing axis (0 for the
    // first item). Exact for both writing modes and independent of how
    // faithfully AdvanceThousandths() reproduces the layout.
    const float raw_pos = i > 0 ? to->GetCharPositions()[i - 1] : 0.0f;

    // First kept glyph in the object.
    if (!any_kept) {
      // Whatever was dropped before this glyph (glyphs and TJ adjustments)
      // leaves a gap that TJ cannot express at the start of a run. Close it
      // by moving the text origin to this glyph's laid-out position along
      // the writing axis: X (a, b) for horizontal text, Y (c, d) for
      // vertical text. When nothing was dropped the position is 0 and the
      // object keeps its parsed origin.
      st.ResetBetweenRuns();
      if (raw_pos != 0.0f) {
        if (is_vert) {
          final_tm.e += raw_pos * final_tm.c;
          final_tm.f += raw_pos * final_tm.d;
        } else {
          final_tm.e += raw_pos * final_tm.a;
          final_tm.f += raw_pos * final_tm.b;
        }
      }
    } else {
      // Between kept runs: emit an inter-run kerning.
      if (st.has_explicit_kerning) {
        float k = st.kerning_accumulator;
        if (std::fabs(k) < kTJDeadband)
          k = 0.0f;
        FlushSegment(&st, k);
      } else {
        // Infer kerning from the layout positions of consecutive kept
        // glyphs along the writing axis.
        const float delta_user = raw_pos - st.prev_glyph_pos;
        const float delta_mth = delta_user * 1000.0f / fs;
        const float nominal_advance_mth =
            AdvanceThousandths(to, font, st.prev_glyph_code);
        float kerning_mth = nominal_advance_mth - delta_mth;
        if (std::fabs(kerning_mth) < kTJDeadband)
          kerning_mth = 0.0f;
        FlushSegment(&st, kerning_mth);
      }
    }

    // Keep this glyph.
    st.AppendKeptGlyph(it, raw_pos);
    st.ResetBetweenRuns();
    any_kept = true;
  }

  if (!any_kept)
    return any_removed ? RedactOutcome::kRemovedAll : RedactOutcome::kUnchanged;
  if (!any_removed) {
    // Nothing intersected: leave the object exactly as parsed. Rewriting it
    // would only widen the regenerated surface (and, for vertical text, used
    // to displace the run along the wrong axis).
    return RedactOutcome::kUnchanged;
  }

  // If the last operation opened a new (empty) run by flushing a kerning,
  // drop the dangling run and its paired kerning so we keep the invariant
  // kernings.size() == strings.size() - 1.
  if (!st.strings.empty() && st.strings.back().IsEmpty()) {
    st.strings.pop_back();
    if (!st.kernings.empty())
      st.kernings.pop_back();
  }

  CHECK(st.kernings.size() + 1 == st.strings.size());

  to->SetSegments(pdfium::span(st.strings), pdfium::span(st.kernings));
  // The single mutation of the text matrix: the parsed matrix plus the
  // accumulated leading shift. SetTextMatrix() also re-lays out the kept
  // glyphs and guarantees downstream writers see a changed object.
  to->SetTextMatrix(final_tm);
  to->SetDirty(true);

  return RedactOutcome::kModified;
}

// Map page-space rects into the image's sample grid (image-local).
static void PageRectsToImageGrid(const CFX_Matrix& image_to_page,
                                 int img_w, int img_h,
                                 pdfium::span<const CFX_FloatRect> page_rects,
                                 std::vector<CFX_FloatRect>* out_image_rects) {
  out_image_rects->clear();
  if (img_w <= 0 || img_h <= 0 || page_rects.empty())
    return;

  // Step 1: page -> unit image space
  const CFX_Matrix page_to_unit = image_to_page.GetInverse();

  out_image_rects->reserve(page_rects.size());
  for (const auto& pr : page_rects) {
    // Page -> unit
    CFX_FloatRect ur = page_to_unit.TransformRect(pr);
    ur.Normalize();

    // Step 2: unit -> pixel
    CFX_FloatRect ir(ur.left   * img_w,
                     ur.bottom * img_h,
                     ur.right  * img_w,
                     ur.top    * img_h);
    ir.Normalize();

    // Clamp to pixel bounds
    ir.left   = std::clamp(ir.left,   0.0f, static_cast<float>(img_w));
    ir.right  = std::clamp(ir.right,  0.0f, static_cast<float>(img_w));
    ir.bottom = std::clamp(ir.bottom, 0.0f, static_cast<float>(img_h));
    ir.top    = std::clamp(ir.top,    0.0f, static_cast<float>(img_h));

    if (ir.right > ir.left && ir.top > ir.bottom)
      out_image_rects->push_back(ir);
  }
}

// Helper: Manually decode a JPEG stream for SMask.
// Returns true on success and populates out_data.
static bool DecodeJpegSMask(RetainPtr<const CPDF_Stream> stream,
                            int width, int height,
                            DataVector<uint8_t>& out_data) {
  if (!stream)
    return false;
  
  // Get raw stream data
  pdfium::span<const uint8_t> raw_span;
  DataVector<uint8_t> raw_data_storage;
  if (stream->IsMemoryBased()) {
    raw_span = stream->GetInMemoryRawData();
  } else {
    raw_data_storage = const_cast<CPDF_Stream*>(stream.Get())->ReadAllRawData();
    raw_span = pdfium::span<const uint8_t>(raw_data_storage);
  }
  
  if (raw_span.size() < 2)
    return false;
  
  // Check for JPEG header
  if (raw_span[0] != 0xFF || raw_span[1] != 0xD8)
    return false;
  
  // Get SMask dimensions
  RetainPtr<const CPDF_Dictionary> smask_dict = stream->GetDict();
  int smask_w = smask_dict ? smask_dict->GetIntegerFor("Width") : width;
  int smask_h = smask_dict ? smask_dict->GetIntegerFor("Height") : height;
  
  // Create JPEG decoder
  auto jpeg_decoder = fxcodec::JpegModule::CreateDecoder(
      raw_span, smask_w, smask_h, 1, false);
  
  if (!jpeg_decoder)
    return false;
  
  out_data.resize(static_cast<size_t>(width) * height);
  
  // Decode scanlines
  for (int row = 0; row < smask_h && row < height; ++row) {
    pdfium::span<const uint8_t> scanline = jpeg_decoder->GetScanline(row);
    if (!scanline.empty()) {
      size_t copy_len = std::min<size_t>(scanline.size(), static_cast<size_t>(width));
      memcpy(out_data.data() + row * width, scanline.data(), copy_len);
    }
  }
  
  return true;
}

// Sanitizes an intersecting image. Once intersection is established, any
// decode or stream-replacement failure is reported as a hard failure so the
// caller cannot flatten a successful-looking overlay over recoverable pixels.
static RedactResult RedactImageObject(
    CPDF_Page* page,
    CPDF_ImageObject* iobj,
    pdfium::span<const CFX_FloatRect> page_rects,
    const CFX_Matrix& parent_to_page,
    bool fill_black) {
  if (!page || !iobj) {
    return {.succeeded = false};
  }
  // Object -> page for this placement.
  // Order matters: apply image's internal matrix first, THEN the form placement.
  const CFX_Matrix img_to_page = iobj->matrix() * parent_to_page;

  // Quick reject using unit bbox in page space.
  const CFX_FloatRect img_bbox_page =
      img_to_page.TransformRect(CFX_FloatRect(0, 0, 1.0f, 1.0f));
  bool touches = false;
  for (const auto& r : page_rects) {
    if (img_bbox_page.right > r.left && img_bbox_page.left < r.right &&
        img_bbox_page.top > r.bottom && img_bbox_page.bottom < r.top) {
      touches = true;
      break;
    }
  }
  if (!touches) {
    return {};
  }

  CPDF_Image* image = iobj->GetImage();
  CPDF_Document* doc = page->GetDocument();
  if (!image || !doc) {
    return {.succeeded = false};
  }
  const int W = image->GetPixelWidth();
  const int H = image->GetPixelHeight();
  if (W <= 0 || H <= 0) {
    return {.succeeded = false};
  }

  // Try to load the image via standard DIB path
  RetainPtr<CFX_DIBBase> dib = image->LoadDIBBase();
  if (!dib) {
    return {.succeeded = false};
  }

  const int bpp        = dib->GetBPP();
  const bool is_mask   = dib->IsMaskFormat();
  const bool has_alpha = dib->IsAlphaFormat();

  const bool is_1bit   = (bpp == 1);
  const bool is_gray8  = (bpp == 8)  && !is_mask;
  const bool is_rgb24  = (bpp == 24);
  const bool is_bgra32 = (bpp == 32) &&  has_alpha;
  const bool is_bgrx32 = (bpp == 32) && !has_alpha;

  // Check if this is an ImageMask - these use the fill color from graphics state
  const bool is_image_mask = image->IsMask();
  uint8_t mask_fill_r = 0, mask_fill_g = 0, mask_fill_b = 0;
  if (is_image_mask) {
    // Get fill color from the image object's color state
    // FX_COLORREF is BGR: 0x00BBGGRR
    FX_COLORREF fill_color = iobj->color_state().GetFillColorRef();
    mask_fill_r = static_cast<uint8_t>(fill_color & 0xFF);
    mask_fill_g = static_cast<uint8_t>((fill_color >> 8) & 0xFF);
    mask_fill_b = static_cast<uint8_t>((fill_color >> 16) & 0xFF);
  }

  // Palette detection for indexed-8 images.
  auto palette = dib->GetPaletteSpan();
  const bool is_indexed8 = is_gray8 && !palette.empty();

  bool palette_has_alpha = false;
  if (is_indexed8) {
    for (uint32_t c : palette) {
      if ((c >> 24) != 0xFF) { palette_has_alpha = true; break; }
    }
  }

  if (!(is_1bit || is_gray8 || is_rgb24 || is_bgra32 || is_bgrx32)) {
    return {.succeeded = false};
  }

  // If the image has an SMask, keep it so we preserve transparency.
  RetainPtr<const CPDF_Stream> orig_smask_stream;
  if (image->GetStream()) {
    RetainPtr<const CPDF_Dictionary> idict = image->GetStream()->GetDict();
    if (idict) {
      RetainPtr<const CPDF_Object> smask_obj = idict->GetDirectObjectFor("SMask");
      if (smask_obj && smask_obj->AsStream())
        orig_smask_stream = pdfium::WrapRetain(smask_obj->AsStream());
    }
  }

  // Map page-space rects into image pixel space (bottom-up).
  std::vector<CFX_FloatRect> img_rects;
  PageRectsToImageGrid(img_to_page, W, H, page_rects, &img_rects);
  if (img_rects.empty()) {
    return {};
  }

  struct IRect { int x0, y0, x1, y1; };
  std::vector<IRect> boxes;
  boxes.reserve(img_rects.size());
  for (const auto& r : img_rects) {
    IRect b;
    b.x0 = std::max(0, std::min(W, static_cast<int>(std::floor(r.left))));
    b.x1 = std::max(0, std::min(W, static_cast<int>(std::ceil (r.right))));
    b.y0 = std::max(0, std::min(H, static_cast<int>(std::floor(r.bottom))));
    b.y1 = std::max(0, std::min(H, static_cast<int>(std::ceil (r.top))));
    if (b.x1 > b.x0 && b.y1 > b.y0)
      boxes.push_back(b);
  }
  if (boxes.empty()) {
    return {};
  }

  const uint8_t fill_val = fill_black ? 0x00 : 0xFF;

  // Build new decoded buffers.
  DataVector<uint8_t> out_rgb(static_cast<size_t>(W) * static_cast<size_t>(H) * 3u);
  DataVector<uint8_t> out_a;

  // We need an alpha plane if: original was BGRA32, or there was an SMask, or
  // palette carries alpha (PNG paletted transparency), or it's an ImageMask.
  bool process_alpha = is_bgra32 || !!orig_smask_stream || (is_indexed8 && palette_has_alpha) || is_image_mask;

  if (process_alpha) {
    out_a.resize(static_cast<size_t>(W) * static_cast<size_t>(H));
    if (orig_smask_stream && !is_bgra32) {
      // Try to decode SMask as JPEG (for file-based streams in WASM)
      if (!DecodeJpegSMask(orig_smask_stream, W, H, out_a)) {
        // Fall back to LoadAllDataFiltered
        auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(orig_smask_stream);
        acc->LoadAllDataFiltered();
        pdfium::span<const uint8_t> span = acc->GetSpan();
        if (span.size() >= out_a.size()) {
          memcpy(out_a.data(), span.data(), out_a.size());
        } else if (!span.empty()) {
          memcpy(out_a.data(), span.data(), span.size());
          std::fill(out_a.begin() + static_cast<ptrdiff_t>(span.size()), out_a.end(), 0xFF);
        } else {
          std::fill(out_a.begin(), out_a.end(), 0xFF);
        }
      }
    } else {
      std::fill(out_a.begin(), out_a.end(), 0xFF);
    }
  }

  size_t total_redacted_px = 0;

  for (int row_top = 0; row_top < H; ++row_top) {
    const int y_img = H - 1 - row_top;
    const pdfium::span<const uint8_t> sline = dib->GetScanline(row_top);
    uint8_t* drow_rgb = out_rgb.data() + static_cast<size_t>(row_top) * static_cast<size_t>(W) * 3u;
    uint8_t* arow     = process_alpha ? (out_a.data() + static_cast<size_t>(row_top) * static_cast<size_t>(W)) : nullptr;

    if (sline.empty()) {
      std::fill(drow_rgb, drow_rgb + static_cast<size_t>(W) * 3u, fill_val);
      if (process_alpha)
        std::fill(arow, arow + static_cast<size_t>(W), 0xFF);
      total_redacted_px += static_cast<size_t>(W);
      continue;
    }

    for (int x = 0; x < W; ++x) {
      const bool red = IntersectsAny(
          {static_cast<float>(x), static_cast<float>(y_img),
           static_cast<float>(x + 1), static_cast<float>(y_img + 1)}, img_rects);

      if (red) {
        drow_rgb[3*x + 0] = fill_val;
        drow_rgb[3*x + 1] = fill_val;
        drow_rgb[3*x + 2] = fill_val;
        if (process_alpha)
          arow[x] = 0xFF;
        ++total_redacted_px;
        continue;
      }

      if (is_1bit) {
        // 1-bit image: each byte contains 8 pixels, MSB first
        const int byte_idx = x / 8;
        const int bit_idx = 7 - (x % 8);  // MSB first
        const uint8_t byte_val = sline[byte_idx];
        const uint8_t bit_val = (byte_val >> bit_idx) & 1;
        
        uint8_t r, g, b;
        if (is_image_mask) {
          // ImageMask: use fill color for painted pixels, transparent for others
          // PDFium's DIB loader applies the Decode array during decoding, so
          // the decoded bit values are already transformed.
          // After decoding: bit 1 = painted, bit 0 = transparent
          const bool is_paint = (bit_val == 1);
          if (is_paint) {
            r = mask_fill_r;
            g = mask_fill_g;
            b = mask_fill_b;
            if (process_alpha)
              arow[x] = 0xFF;  // opaque
          } else {
            r = g = b = 0xFF;  // background (will be transparent)
            if (process_alpha)
              arow[x] = 0x00;  // transparent
          }
        } else if (!palette.empty()) {
          // Use palette - it already reflects correct color mapping (BlackIs1, Decode, etc.)
          const uint32_t argb = palette[bit_val];
          r = static_cast<uint8_t>((argb >> 16) & 0xFF);
          g = static_cast<uint8_t>((argb >> 8) & 0xFF);
          b = static_cast<uint8_t>(argb & 0xFF);
        } else {
          // No palette: default is bit 0 = black, bit 1 = white
          const uint8_t v = bit_val ? 0xFF : 0x00;
          r = g = b = v;
        }
        drow_rgb[3*x + 0] = r;
        drow_rgb[3*x + 1] = g;
        drow_rgb[3*x + 2] = b;
      } else if (is_indexed8) {
        const uint8_t idx  = sline[x];
        const uint32_t argb = palette[idx];
        drow_rgb[3*x + 0] = static_cast<uint8_t>((argb >> 16) & 0xFF);
        drow_rgb[3*x + 1] = static_cast<uint8_t>((argb >>  8) & 0xFF);
        drow_rgb[3*x + 2] = static_cast<uint8_t>( argb        & 0xFF);
        if (process_alpha && !orig_smask_stream && !is_bgra32 && palette_has_alpha)
          arow[x] = static_cast<uint8_t>((argb >> 24) & 0xFF);
      } else if (is_gray8) {
        const uint8_t v = sline[x];
        drow_rgb[3*x + 0] = v;
        drow_rgb[3*x + 1] = v;
        drow_rgb[3*x + 2] = v;
      } else if (is_rgb24) {
        drow_rgb[3*x + 0] = sline[3*x + 2];
        drow_rgb[3*x + 1] = sline[3*x + 1];
        drow_rgb[3*x + 2] = sline[3*x + 0];
      } else {
        drow_rgb[3*x + 0] = sline[4*x + 2];
        drow_rgb[3*x + 1] = sline[4*x + 1];
        drow_rgb[3*x + 2] = sline[4*x + 0];
        if (process_alpha && is_bgra32)
          arow[x] = sline[4*x + 3];
      }
    }
  }

  if (total_redacted_px == 0) {
    return {.succeeded = false};
  }

  // Ensure redaction regions are fully opaque in the SMask/alpha plane.
  if (process_alpha) {
    for (const auto& box : boxes) {
      for (int y = box.y0; y < box.y1; ++y) {
        const int row_top = H - 1 - y;
        uint8_t* row_ptr = out_a.data() + static_cast<size_t>(row_top) * static_cast<size_t>(W);
        std::fill(row_ptr + box.x0, row_ptr + box.x1, 0xFF);
      }
    }
  }

  // Build main image dict (decoded RGB).
  RetainPtr<CPDF_Dictionary> ndict = doc->New<CPDF_Dictionary>();
  ndict->SetNewFor<CPDF_Name>("Type", "XObject");
  ndict->SetNewFor<CPDF_Name>("Subtype", "Image");
  ndict->SetNewFor<CPDF_Number>("Width", W);
  ndict->SetNewFor<CPDF_Number>("Height", H);
  ndict->SetNewFor<CPDF_Name>("ColorSpace", "DeviceRGB");
  ndict->SetNewFor<CPDF_Number>("BitsPerComponent", 8);

  // If we have/kept alpha, attach a soft mask.
  if (process_alpha) {
    RetainPtr<CPDF_Dictionary> smask_dict = doc->New<CPDF_Dictionary>();
    smask_dict->SetNewFor<CPDF_Name>("Type", "XObject");
    smask_dict->SetNewFor<CPDF_Name>("Subtype", "Image");
    smask_dict->SetNewFor<CPDF_Number>("Width", W);
    smask_dict->SetNewFor<CPDF_Number>("Height", H);
    smask_dict->SetNewFor<CPDF_Name>("ColorSpace", "DeviceGray");
    smask_dict->SetNewFor<CPDF_Number>("BitsPerComponent", 8);

    RetainPtr<CPDF_Stream> smask_stream =
        pdfium::MakeRetain<CPDF_Stream>(std::move(out_a), std::move(smask_dict));
    const uint32_t smask_objnum = doc->AddIndirectObject(smask_stream);
    ndict->SetFor("SMask", pdfium::MakeRetain<CPDF_Reference>(doc, smask_objnum));
  }

  // Images (and their masks) may be reused by other placements or pages.
  // Replace only this placement after all decoded buffers are ready. Editing
  // the old indirect stream in place would also redact the unmarked copies.
  auto replacement_stream = pdfium::MakeRetain<CPDF_Stream>(
      std::move(out_rgb), std::move(ndict));
  auto replacement = pdfium::MakeRetain<CPDF_Image>(doc, replacement_stream);
  replacement->ConvertStreamToIndirectObject();
  iobj->SetImage(std::move(replacement));
  page->ClearRenderContext();
  iobj->SetDirty(true);
  return {.changed = true};
}

// Redact all text objects inside a holder (page or form). If `recurse_forms` is
// true, also descends into nested Form XObjects via their placement matrices.
//
// `to_page` transforms holder-local space to PAGE USER SPACE.
// Redact all page objects inside a holder (page or form).
RedactResult RedactHolder(CPDF_Page* page_for_cache,
                          CPDF_PageObjectHolder* holder,
                          pdfium::span<const RedactRegion> regions,
                          pdfium::span<const CFX_FloatRect> region_bboxes,
                          const CPDF_PathRedactor& path_redactor,
                          const CFX_Matrix& to_page,
                          bool recurse_forms,
                          bool fill_black,
                          CPDF_RedactionMarkSanitizer::SanitizedProperties*
                              sanitized_properties) {
  RedactResult result;
  CPDF_RedactionMarkSanitizer mark_sanitizer(holder, sanitized_properties);
  std::vector<CPDF_PageObject*> to_remove;
  struct PendingPathInsertion {
    CPDF_PathObject* after = nullptr;
    std::unique_ptr<CPDF_PathObject> object;
  };
  std::vector<PendingPathInsertion> path_insertions;

  for (auto it = holder->begin(); it != holder->end(); ++it) {
    CPDF_PageObject* po = it->get();
    if (!po->IsActive())
      continue;

    if (CPDF_TextObject* to = po->AsText()) {
      const RedactOutcome out =
          RedactTextObjectMulti(to, regions, region_bboxes, to_page);
      if (out != RedactOutcome::kUnchanged) {
        mark_sanitizer.Record(po);
      }
      if (out == RedactOutcome::kRemovedAll) {
        to_remove.push_back(po);
        result.changed = true;
      } else if (out == RedactOutcome::kModified) {
        result.changed = true;
      }
      continue;
    }

    if (CPDF_ImageObject* io = po->AsImage()) {
      const RedactResult image_result = RedactImageObject(
          page_for_cache, io, region_bboxes, to_page, fill_black);
      result.changed |= image_result.changed;
      if (!image_result.succeeded) {
        result.succeeded = false;
        return result;
      }
      if (image_result.changed) {
        mark_sanitizer.Record(po);
      }
      continue;
    }

    if (CPDF_PathObject* path = po->AsPath()) {
      PathRedactionResult path_result = path_redactor.Redact(path, to_page);
      if (!path_result.succeeded) {
        result.succeeded = false;
        return result;
      }
      if (path_result.changed) {
        mark_sanitizer.Record(po);
      }
      if (path_result.remove_original) {
        to_remove.push_back(path);
      }
      if (path_result.trailing_object) {
        path_insertions.push_back(
            {.after = path, .object = std::move(path_result.trailing_object)});
      }
      result.changed |= path_result.changed;
      continue;
    }

    if (CPDF_ShadingObject* shading = po->AsShading()) {
      PathRedactionResult shading_result =
          path_redactor.RedactShading(shading, to_page);
      if (!shading_result.succeeded) {
        result.succeeded = false;
        return result;
      }
      if (shading_result.changed) {
        mark_sanitizer.Record(po);
      }
      if (shading_result.remove_original) {
        to_remove.push_back(shading);
      }
      result.changed |= shading_result.changed;
      continue;
    }

    if (recurse_forms) {
      if (CPDF_FormObject* fo = po->AsForm()) {
        CPDF_Form* form = fo->form();
        if (!form)
          continue;

        const CFX_Matrix placement = fo->form_matrix();
        // CFX_Matrix composes left-to-right, just like the renderer and text
        // extractor: apply this placement before its parent's page transform.
        const CFX_Matrix next_to_page = placement * to_page;
        const RedactResult form_result =
            RedactHolder(page_for_cache, form, regions, region_bboxes,
                         path_redactor, next_to_page, true, fill_black,
                         sanitized_properties);

        if (!form_result.succeeded) {
          result.succeeded = false;
          return result;
        }
        if (form_result.changed) {
          mark_sanitizer.Record(po);
          CPDF_PageContentGenerator form_gen(form);
          form_gen.GenerateContent();
          fo->SetDirty(true);
          result.changed = true;
        }
      }
    }
  }

  // Physically remove fully emptied text and path objects.
  if (!to_remove.empty()) {
    for (CPDF_PageObject* obj : to_remove) {
      holder->RemovePageObject(obj);
    }
    result.changed = true;
  }

  for (PendingPathInsertion& insertion : path_insertions) {
    size_t index = 0;
    while (index < holder->GetPageObjectCount() &&
           holder->GetPageObjectByIndex(index) != insertion.after) {
      ++index;
    }
    if (index == holder->GetPageObjectCount() ||
        !holder->InsertPageObjectAtIndex(index + 1,
                                         std::move(insertion.object))) {
      result.succeeded = false;
      return result;
    }
  }

  if (result.changed) {
    // This walker accepts only pages and Form XObjects. Give forms private
    // backing dictionaries before sanitizing their named property resources.
    if (!holder->IsPage() &&
        !static_cast<CPDF_Form*>(holder)->CloneBackingStreamForWrite()) {
      result.succeeded = false;
      return result;
    }
    mark_sanitizer.Apply();
  }

  return result;
}

}  // namespace

std::optional<RedactRegion> RedactRegionFromQuadCorners(
    const std::array<CFX_PointF, 4>& corners) {
  if (!FiniteCorners(corners)) {
    return std::nullopt;
  }
  RedactRegion region;
  region.bbox = BBoxOfQuadCorners(corners);
  region.bbox.Normalize();
  if (CornersWellFormed(corners) && !CornersAxisAligned(corners)) {
    region.has_quad = true;
    region.quad = corners;
  }
  return region;
}

bool RedactRegionIntersectsRect(const RedactRegion& region,
                                const CFX_FloatRect& rect) {
  CFX_FloatRect r = rect;
  r.Normalize();
  if (!Intersects(r, region.bbox))
    return false;
  if (!region.has_quad)
    return true;
  return ConvexQuadsIntersect(QuadFromRect(r), region.quad);
}

RedactResult RedactTextInRegions(CPDF_Page* page,
                                 pdfium::span<const RedactRegion> regions_in,
                                 bool recurse_forms,
                                 bool draw_black_boxes) {
  if (!page || regions_in.empty()) {
    return {.succeeded = false};
  }

  // Normalized copies (quads validated by the region constructors).
  std::vector<RedactRegion> regions(regions_in.begin(), regions_in.end());
  for (RedactRegion& region : regions)
    region.bbox.Normalize();
  const std::vector<CFX_FloatRect> bboxes = BBoxesOfRegions(regions);
  const CPDF_PathRedactor path_redactor{pdfium::span(regions)};
  if (!path_redactor.IsValid()) {
    return {.succeeded = false};
  }

  const CFX_Matrix identity;
  CPDF_RedactionMarkSanitizer::SanitizedProperties sanitized_properties;
  RedactResult result =
      RedactHolder(page, page, pdfium::span(regions), pdfium::span(bboxes),
                   path_redactor, identity, recurse_forms,
                   /*fill_black=*/draw_black_boxes, &sanitized_properties);
  if (!result.succeeded) {
    return result;
  }

  if (draw_black_boxes) {
    AddBlackOverlayPaths(page, pdfium::span(regions));  // paint on top
    result.changed = true;
  }

  return result;
}

RedactResult RedactTextInRect(CPDF_Page* page,
                              const CFX_FloatRect& page_space_rect_in,
                              bool recurse_forms,
                              bool draw_black_boxes) {
  if (!page) {
    return {.succeeded = false};
  }

  RedactRegion region;
  region.bbox = page_space_rect_in;
  const RedactRegion regions[] = {region};
  return RedactTextInRegions(page, pdfium::span(regions), recurse_forms,
                             draw_black_boxes);
}

RedactResult RedactTextInRects(
    CPDF_Page* page,
    pdfium::span<const CFX_FloatRect> page_space_rects_in,
    bool recurse_forms,
    bool draw_black_boxes) {
  if (!page || page_space_rects_in.empty()) {
    return {.succeeded = false};
  }

  std::vector<RedactRegion> regions;
  regions.reserve(page_space_rects_in.size());
  for (const CFX_FloatRect& rect : page_space_rects_in) {
    RedactRegion region;
    region.bbox = rect;
    regions.push_back(region);
  }
  return RedactTextInRegions(page, pdfium::span(regions), recurse_forms,
                             draw_black_boxes);
}
