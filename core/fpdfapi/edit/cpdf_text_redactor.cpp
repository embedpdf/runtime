// Copyright 2025 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/edit/cpdf_text_redactor.h"

#include <cmath>
#include <sstream>
#include <utility>
#include <vector>
#include <algorithm>

#include "core/fxcrt/stream_write_utils.h"
#include "core/fpdfapi/edit/cpdf_image_redactor.h"
#include "core/fpdfapi/edit/cpdf_path_redactor.h"
#include "core/fpdfapi/edit/cpdf_pagecontentgenerator.h"
#include "core/fpdfapi/edit/cpdf_pagecontentmanager.h"
#include "core/fpdfapi/edit/cpdf_redaction_copies.h"
#include "core/fpdfapi/edit/cpdf_redaction_mark_sanitizer.h"
#include "core/fpdfapi/edit/cpdf_type3_redactor.h"
#include "core/fpdfapi/font/cpdf_cidfont.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_clippath.h"
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
    w_th = static_cast<float>(font->GetCharWidth(code));
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

  // The original TJ number after a glyph, kept or removed, joins the gap that
  // follows it; sign kept, PDF TJ semantics.
  void AddKerningAfterGlyph(float kerning) {
    if (kerning != 0) {
      kerning_accumulator += kerning;
      has_explicit_kerning = true;
    }
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

  // The TJ number after glyph i is kernings[i]; positions[i] is glyph i's
  // raw layout position along the writing axis (0 for the first glyph).
  const std::vector<float>& kernings = to->GetCharKernings();
  const std::vector<float>& positions = to->GetCharPositions();
  const size_t n = to->CharCount();
  for (size_t i = 0; i < n; ++i) {
    const CPDF_TextObject::Item it = to->GetItemInfo(i);

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
      st.AddKerningAfterGlyph(kernings[i]);
      continue;
    }

    // Raw layout position of this glyph along the writing axis. Exact for
    // both writing modes and independent of how faithfully
    // AdvanceThousandths() reproduces the layout.
    const float raw_pos = positions[i];

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
    st.AddKerningAfterGlyph(kernings[i]);
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

// EmbedPDF: text in render modes 4 to 7 leaves a copy of itself in the clip
// of what is drawn after it, and writing the page writes that copy as clip
// text. Its glyphs inside the regions go as they go from the text itself,
// or the copy would write them back. Returns whether any went; adds the
// Type3 fonts that lost glyphs to `type3_fonts`.
bool RedactClipTexts(CPDF_PageObject* object,
                     pdfium::span<const RedactRegion> regions,
                     pdfium::span<const CFX_FloatRect> region_bboxes,
                     const CFX_Matrix& to_page,
                     std::set<RetainPtr<CPDF_Font>>* type3_fonts) {
  const CPDF_ClipPath& clip_path = object->clip_path();
  if (!clip_path.HasRef()) {
    return false;
  }
  bool hit = false;
  for (size_t i = 0; i < clip_path.GetTextCount() && !hit; ++i) {
    const CPDF_TextObject* text = clip_path.GetText(i);
    hit = text && IntersectsAny(to_page.TransformRect(text->GetRect()),
                                region_bboxes);
  }
  if (!hit) {
    return false;
  }
  bool changed = false;
  object->mutable_clip_path().EditTexts([&](CPDF_TextObject* text) {
    const RedactOutcome outcome =
        RedactTextObjectMulti(text, regions, region_bboxes, to_page);
    if (outcome != RedactOutcome::kUnchanged) {
      changed = true;
      if (RetainPtr<CPDF_Font> font = text->GetFont();
          font && font->IsType3Font()) {
        type3_fonts->insert(std::move(font));
      }
    }
    return outcome != RedactOutcome::kRemovedAll;
  });
  return changed;
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

    if (RedactClipTexts(po, regions, region_bboxes, to_page,
                        &result.type3_fonts)) {
      mark_sanitizer.Record(po);
      result.changed = true;
    }

    if (CPDF_TextObject* to = po->AsText()) {
      const RedactOutcome out =
          RedactTextObjectMulti(to, regions, region_bboxes, to_page);
      if (out != RedactOutcome::kUnchanged) {
        mark_sanitizer.Record(po);
        // EmbedPDF: a Type3 font's glyphs are content of their own
        // (RedactType3Glyphs()).
        if (RetainPtr<CPDF_Font> font = to->GetFont();
            font && font->IsType3Font()) {
          result.type3_fonts.insert(std::move(font));
        }
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
      const RedactResult image_result =
          RedactImageObject(page_for_cache, io, holder->GetResources().Get(),
                            region_bboxes, to_page, fill_black);
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
        result.type3_fonts.insert(form_result.type3_fonts.begin(),
                                  form_result.type3_fonts.end());
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
    if (!holder->IsPage()) {
      auto* form = static_cast<CPDF_Form*>(holder);
      if (!form->CloneBackingStreamForWrite()) {
        result.succeeded = false;
        return result;
      }
      // EmbedPDF: the copy holds the redacted drawing, and none of the
      // original's other copies of it.
      RemoveContentCopies(form->GetMutableDict().Get());
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
  // EmbedPDF: what only the redacted glyphs of a Type3 font drew goes with
  // them, from every use of the font on the page.
  if (RedactType3Glyphs(page, result.type3_fonts)) {
    result.changed = true;
  }
  // EmbedPDF: copies made before the redaction still show what it removed:
  // the page's thumbnail, application data and metadata, and the previews in
  // the document's XMP metadata. A page redacted loses them, whether or not
  // content changed under the regions.
  RemovePageContentCopies(page);
  RemoveDocumentXmpThumbnails(page->GetDocument());

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
