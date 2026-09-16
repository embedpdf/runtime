// Copyright 2025
// Use of this source code is governed by a BSD-style license.

#include "core/fpdfapi/edit/cpdf_text_redactor.h"

#include <cmath>
#include <sstream>
#include <utility>
#include <vector>
#include <algorithm>

#include "core/fpdfapi/edit/cpdf_contentstream_write_utils.h"
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

// Represents a single subpath within a complex path (e.g., one letter in a vector logo).
struct Subpath {
  std::vector<CFX_Path::Point> points;
  CFX_FloatRect bounding_box;
};

// Calculate bounding box for a set of path points.
CFX_FloatRect CalculateSubpathBoundingBox(const std::vector<CFX_Path::Point>& points) {
  if (points.empty())
    return CFX_FloatRect();
  
  float min_x = points[0].point_.x;
  float max_x = points[0].point_.x;
  float min_y = points[0].point_.y;
  float max_y = points[0].point_.y;
  
  for (const auto& pt : points) {
    min_x = std::min(min_x, pt.point_.x);
    max_x = std::max(max_x, pt.point_.x);
    min_y = std::min(min_y, pt.point_.y);
    max_y = std::max(max_y, pt.point_.y);
  }
  
  return CFX_FloatRect(min_x, min_y, max_x, max_y);
}

// Extract individual subpaths from a complex path.
// Each subpath starts with a kMove point and ends at the next kMove or end of path.
std::vector<Subpath> ExtractSubpaths(const CFX_Path& path) {
  std::vector<Subpath> subpaths;
  const std::vector<CFX_Path::Point>& points = path.GetPoints();
  
  if (points.empty())
    return subpaths;
  
  Subpath current;
  for (size_t i = 0; i < points.size(); ++i) {
    const auto& pt = points[i];
    
    // A kMove point starts a new subpath (unless it's the first point or current is empty)
    if (pt.type_ == CFX_Path::Point::Type::kMove && !current.points.empty()) {
      // Finish current subpath
      current.bounding_box = CalculateSubpathBoundingBox(current.points);
      subpaths.push_back(std::move(current));
      current = Subpath();
    }
    
    current.points.push_back(pt);
  }
  
  // Don't forget the last subpath
  if (!current.points.empty()) {
    current.bounding_box = CalculateSubpathBoundingBox(current.points);
    subpaths.push_back(std::move(current));
  }
  
  return subpaths;
}

// Rebuild a CFX_Path from a vector of subpaths.
void RebuildPath(CPDF_Path& path, const std::vector<Subpath>& subpaths) {
  // Create a new path and copy points from remaining subpaths
  CPDF_Path new_path;
  new_path.Emplace();
  
  for (const auto& subpath : subpaths) {
    for (const auto& pt : subpath.points) {
      if (pt.close_figure_) {
        new_path.AppendPointAndClose(pt.point_, pt.type_);
      } else {
        new_path.AppendPoint(pt.point_, pt.type_);
      }
    }
  }
  
  path = new_path;
}

// Check if a subpath's bounding box (transformed to page space) is inside any redaction rect.
bool IsSubpathInsideAnyRedactRect(const CFX_FloatRect& subpath_bbox,
                                   const CFX_Matrix& total_transform,
                                   pdfium::span<const CFX_FloatRect> page_rects) {
  CFX_FloatRect bbox_page = total_transform.TransformRect(subpath_bbox);
  bbox_page.Normalize();
  
  for (const auto& redact_rect : page_rects) {
    if (bbox_page.left >= redact_rect.left &&
        bbox_page.right <= redact_rect.right &&
        bbox_page.bottom >= redact_rect.bottom &&
        bbox_page.top <= redact_rect.top) {
      return true;
    }
  }
  return false;
}

static void AddBlackOverlayPaths(CPDF_Page* page,
                                 pdfium::span<const CFX_FloatRect> rects_page_space) {
  if (!page || rects_page_space.empty())
    return;

  for (const auto& r : rects_page_space) {
    auto po = std::make_unique<CPDF_PathObject>();
    po->set_stroke(false);
    po->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
    po->path().AppendFloatRect(r);        // left/bottom/right/top in PAGE USER SPACE
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

// Compute a glyph's bbox in PAGE USER SPACE.
//
// Note: CPDF_TextObject::GetItemInfo() origin_ is already adjusted for vertical
// writing, so we do not apply any extra vertical origin shift here.
CFX_FloatRect GlyphBBoxInPage(const CPDF_TextObject* to,
                              CPDF_Font* font,
                              uint32_t code,
                              const CPDF_TextObject::Item& it,
                              const CFX_Matrix& text_matrix,
                              const CFX_Matrix& parent_to_page) {
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

  // Text matrix to page space (for this text object), then parent to page.
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

  // Use raw writing-axis positions, without vertical glyph-origin offsets.
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
                                    pdfium::span<const CFX_FloatRect> page_rects,
                                    const CFX_Matrix& parent_to_page) {
  CPDF_Font* font = to->GetFont();
  if (!font)
    return RedactOutcome::kUnchanged;

  const CPDF_CIDFont* cid = font->AsCIDFont();
  const bool is_vert = cid && cid->IsVertWriting();
  const float fs = to->GetFontSize();

  bool any_kept = false;
  bool any_removed = false;

  // Hit-test every glyph against the original matrix. Shifting the object
  // during the walk would displace later regions and leave redacted text behind.
  const CFX_Matrix original_tm = to->GetTextMatrix();
  CFX_Matrix final_tm = original_tm;

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
    const CFX_FloatRect gbox =
        GlyphBBoxInPage(to, font, it.char_code_, it, original_tm, parent_to_page);
    const bool hit = IntersectsAny(gbox, page_rects);

    if (hit) {
      // Merge the removed glyph's advance into the pending kerning pool.
      st.kerning_accumulator -= AdvanceThousandths(to, font, it.char_code_);
      any_removed = true;
      continue;
    }

    // Raw layout position along the writing axis, including original TJ gaps.
    const float raw_pos = i > 0 ? to->GetCharPositions()[i - 1] : 0.0f;

    // First kept glyph in the object. TJ cannot express a leading gap, so
    // preserve its position by shifting the final matrix along the writing axis.
    if (!any_kept) {
      st.ResetBetweenRuns();
      if (is_vert) {
        final_tm.e += raw_pos * final_tm.c;
        final_tm.f += raw_pos * final_tm.d;
      } else {
        final_tm.e += raw_pos * final_tm.a;
        final_tm.f += raw_pos * final_tm.b;
      }
    } else {
      // Between kept runs: emit an inter-run kerning.
      if (st.has_explicit_kerning) {
        float k = st.kerning_accumulator;
        if (std::fabs(k) < kTJDeadband)
          k = 0.0f;
        FlushSegment(&st, k);
      } else {
        // Infer kerning from consecutive kept glyphs' layout positions.
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
  if (!any_removed)
    return RedactOutcome::kUnchanged;

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
  // Apply the accumulated leading shift only after all glyphs are tested.
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

// Returns true if the image stream was overwritten.
static bool RedactImageObject(CPDF_Page* page,
                              CPDF_ImageObject* iobj,
                              pdfium::span<const CFX_FloatRect> page_rects,
                              const CFX_Matrix& parent_to_page,
                              bool fill_black) {
  if (!iobj)
    return false;
  CPDF_Image* image = iobj->GetImage();
  if (!image)
    return false;

  CPDF_Document* doc = page->GetDocument();
  const int W = image->GetPixelWidth();
  const int H = image->GetPixelHeight();
  if (W <= 0 || H <= 0)
    return false;

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
  if (!touches)
    return false;

  // Try to load the image via standard DIB path
  RetainPtr<CFX_DIBBase> dib = image->LoadDIBBase();
  if (!dib)
    return false;

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

  if (!(is_1bit || is_gray8 || is_rgb24 || is_bgra32 || is_bgrx32))
    return false;

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
  if (img_rects.empty())
    return false;

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
  if (boxes.empty())
    return false;

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

  if (total_redacted_px == 0)
    return false;

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

  const bool ok = image->OverwriteStreamInPlace(std::move(out_rgb), std::move(ndict),
                                                /*data_is_decoded=*/true);
  if (ok) {
    image->ResetCache(page);
    page->ClearRenderContext();
    iobj->SetDirty(true);
  }
  return ok;
}

// Redact all text objects inside a holder (page or form). If `recurse_forms` is
// true, also descends into nested Form XObjects via their placement matrices.
//
// `to_page` transforms holder-local space to PAGE USER SPACE.
// Redact all page objects inside a holder (page or form).
bool RedactHolder(CPDF_Page* page_for_cache,
                  CPDF_PageObjectHolder* holder,
                  pdfium::span<const CFX_FloatRect> page_rects,
                  const CFX_Matrix& to_page,
                  bool recurse_forms,
                  bool fill_black,
                  CPDF_RedactionMarkSanitizer::SanitizedProperties*
                      sanitized_properties) {
  bool changed = false;
  CPDF_RedactionMarkSanitizer mark_sanitizer(holder, sanitized_properties);
  std::vector<CPDF_PageObject*> to_remove;

  for (auto it = holder->begin(); it != holder->end(); ++it) {
    CPDF_PageObject* po = it->get();
    if (!po->IsActive())
      continue;

    if (CPDF_TextObject* to = po->AsText()) {
      const RedactOutcome out = RedactTextObjectMulti(to, page_rects, to_page);
      if (out != RedactOutcome::kUnchanged) {
        mark_sanitizer.Record(po);
      }
      if (out == RedactOutcome::kRemovedAll) {
        to_remove.push_back(po);
        changed = true;
      } else if (out == RedactOutcome::kModified) {
        changed = true;
      }
      continue;
    }

    if (CPDF_ImageObject* io = po->AsImage()) {
      if (RedactImageObject(page_for_cache, io, page_rects, to_page, fill_black)) {
        mark_sanitizer.Record(po);
        changed = true;
      }
      continue;
    }

    if (CPDF_PathObject* path = po->AsPath()) {
      // Order matters: apply path's internal matrix first, THEN the form placement.
      CFX_Matrix total_transform = path->matrix() * to_page;
      
      // Extract subpaths from the path (e.g., individual letters in a vector logo)
      const CFX_Path* cfx_path = path->path().GetObject();
      if (!cfx_path) {
        continue;
      }
      
      std::vector<Subpath> subpaths = ExtractSubpaths(*cfx_path);
      
      if (subpaths.empty()) {
        continue;
      }
      
      // Check each subpath individually against redaction rects
      std::vector<Subpath> remaining_subpaths;
      bool any_removed = false;
      
      for (const auto& subpath : subpaths) {
        if (IsSubpathInsideAnyRedactRect(subpath.bounding_box, total_transform, page_rects)) {
          // This subpath should be redacted
          any_removed = true;
        } else {
          // Keep this subpath
          remaining_subpaths.push_back(subpath);
        }
      }
      
      if (any_removed) {
        mark_sanitizer.Record(po);
        if (remaining_subpaths.empty()) {
          // All subpaths were redacted - remove the entire path object
          to_remove.push_back(path);
        } else {
          // Some subpaths remain - rebuild the path with only the remaining subpaths
          RebuildPath(path->path(), remaining_subpaths);
          path->CalcBoundingBox();
          path->SetDirty(true);
        }
        changed = true;
      }
      continue;
    }

    if (CPDF_ShadingObject* shading = po->AsShading()) {
      // Shading objects have a bounding box - check if it's fully inside any redaction rect
      CFX_FloatRect shading_bbox = shading->GetRect();
      
      // Transform to page space
      CFX_FloatRect bbox_page = to_page.TransformRect(shading_bbox);
      bbox_page.Normalize();
      
      // Check if the shading bbox is fully inside any redaction rect
      for (const auto& redact_rect : page_rects) {
        if (bbox_page.left >= redact_rect.left &&
            bbox_page.right <= redact_rect.right &&
            bbox_page.bottom >= redact_rect.bottom &&
            bbox_page.top <= redact_rect.top) {
          mark_sanitizer.Record(po);
          to_remove.push_back(shading);
          changed = true;
          break;
        }
      }
      continue;
    }

    if (recurse_forms) {
      if (CPDF_FormObject* fo = po->AsForm()) {
        CPDF_Form* form = fo->form();
        if (!form)
          continue;

        const CFX_Matrix placement = fo->form_matrix();
        const CFX_Matrix next_to_page = placement * to_page;
        const bool form_changed =
            RedactHolder(page_for_cache, form, page_rects, next_to_page, true,
                         fill_black, sanitized_properties);

        if (form_changed) {
          mark_sanitizer.Record(po);
          CPDF_PageContentGenerator form_gen(form);
          form_gen.GenerateContent();
          fo->SetDirty(true);
          changed = true;
        }
      }
    }
  }

  // Physically remove fully emptied text and path objects.
  if (!to_remove.empty()) {
    for (CPDF_PageObject* obj : to_remove) {
      holder->RemovePageObject(obj);
    }
    changed = true;
  }

  if (changed) {
    // Form placements may share the original stream and its dictionaries.
    // Detach before sanitizing properties or regenerating the form content.
    if (!holder->IsPage()) {
      static_cast<CPDF_Form*>(holder)->CloneBackingStreamForWrite();
    }
    mark_sanitizer.Apply();
  }

  return changed;
}

}  // namespace

bool RedactTextInRect(CPDF_Page* page,
                      const CFX_FloatRect& page_space_rect_in,
                      bool recurse_forms,
                      bool draw_black_boxes) {
  if (!page)
    return false;

  CFX_FloatRect r = page_space_rect_in;
  r.Normalize();
  const CFX_Matrix identity;

  const CFX_FloatRect rects[] = {r};
  CPDF_RedactionMarkSanitizer::SanitizedProperties sanitized_properties;
  const bool changed =
      RedactHolder(page, page, pdfium::span(rects), identity, recurse_forms,
                   /*fill_black=*/draw_black_boxes, &sanitized_properties);

  if (draw_black_boxes) {
    AddBlackOverlayPaths(page, pdfium::span(rects));  // paint on top
  }

  // Adding a stream is a change; reflect that.
  return changed || draw_black_boxes;
}

bool RedactTextInRects(CPDF_Page* page,
                       pdfium::span<const CFX_FloatRect> page_space_rects_in,
                       bool recurse_forms,
                       bool draw_black_boxes) {
  if (!page || page_space_rects_in.empty())
    return false;

  // Normalize copies.
  std::vector<CFX_FloatRect> rects;
  rects.reserve(page_space_rects_in.size());
  for (const auto& rr : page_space_rects_in) {
    CFX_FloatRect r = rr;
    r.Normalize();
    rects.push_back(r);
  }

  const CFX_Matrix identity;
  CPDF_RedactionMarkSanitizer::SanitizedProperties sanitized_properties;
  const bool changed =
      RedactHolder(page, page, pdfium::span(rects), identity, recurse_forms,
                   /*fill_black=*/draw_black_boxes, &sanitized_properties);

  if (draw_black_boxes) {
    AddBlackOverlayPaths(page, pdfium::span(rects));  // paint on top
  }

  return changed || draw_black_boxes;
}