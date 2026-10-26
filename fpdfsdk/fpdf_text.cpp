// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "public/fpdf_text.h"
#include "public/epdf_text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

#include "build/build_config.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfdoc/cpdf_viewerpreferences.h"
#include "core/fpdftext/cpdf_linkextract.h"
#include "core/fpdftext/cpdf_textpage.h"
#include "core/fpdftext/cpdf_textpagefind.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/span_util.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxcrt/utf16.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

// EmbedPDF(epdf-api): layout checks and tolerances for the EPDFText_* types.
static_assert(sizeof(EPDF_CHAR_GEOMETRY) == 124);
static_assert(offsetof(EPDF_CHAR_GEOMETRY, loose_box) == 0);
static_assert(offsetof(EPDF_CHAR_GEOMETRY, tight_box) == 16);
static_assert(offsetof(EPDF_CHAR_GEOMETRY, loose_quad) == 32);
static_assert(offsetof(EPDF_CHAR_GEOMETRY, tight_quad) == 64);

static_assert(sizeof(EPDF_CHAR_MAP_ANCHOR) == 8);
static_assert(offsetof(EPDF_CHAR_MAP_ANCHOR, char_index) == 0);
static_assert(offsetof(EPDF_CHAR_MAP_ANCHOR, text_offset) == 4);
static_assert(offsetof(EPDF_CHAR_GEOMETRY, matrix) == 96);
static_assert(offsetof(EPDF_CHAR_GEOMETRY, flags) == 120);

constexpr float kUprightTolerance = 1e-4f;
constexpr float kSingularTolerance = 1e-6f;

CPDF_TextPage* GetTextPageForValidIndex(FPDF_TEXTPAGE text_page, int index) {
  if (!text_page || index < 0) {
    return nullptr;
  }

  CPDF_TextPage* textpage = CPDFTextPageFromFPDFTextPage(text_page);
  return static_cast<size_t>(index) < textpage->size() ? textpage : nullptr;
}

// EmbedPDF(epdf-api): helpers for EPDFText_GetCharGeometry.
bool HasUsableBasis(const CFX_Matrix& matrix) {
  if (!std::isfinite(matrix.a) || !std::isfinite(matrix.b) ||
      !std::isfinite(matrix.c) || !std::isfinite(matrix.d)) {
    return false;
  }
  const float x_scale = std::hypot(matrix.a, matrix.b);
  const float y_scale = std::hypot(matrix.c, matrix.d);
  if (!std::isfinite(x_scale) || !std::isfinite(y_scale) || x_scale == 0.0f ||
      y_scale == 0.0f) {
    return false;
  }
  const float determinant = matrix.a * matrix.d - matrix.b * matrix.c;
  return std::isfinite(determinant) &&
         std::abs(determinant) > kSingularTolerance * x_scale * y_scale;
}

bool IsUpright(const CFX_Matrix& matrix) {
  if (!std::isfinite(matrix.a) || !std::isfinite(matrix.b) ||
      !std::isfinite(matrix.c) || !std::isfinite(matrix.d)) {
    return false;
  }
  const float x_scale = std::hypot(matrix.a, matrix.b);
  const float y_scale = std::hypot(matrix.c, matrix.d);
  return matrix.a > 0.0f && matrix.d > 0.0f && x_scale > 0.0f &&
         y_scale > 0.0f && std::abs(matrix.b) <= kUprightTolerance * x_scale &&
         std::abs(matrix.c) <= kUprightTolerance * y_scale;
}

std::array<CFX_PointF, 4> TransformLocalBox(const CFX_FloatRect& box,
                                            const CFX_Matrix& matrix) {
  CFX_FloatRect normalized = box;
  normalized.Normalize();
  return {matrix.Transform({normalized.left, normalized.top}),
          matrix.Transform({normalized.right, normalized.top}),
          matrix.Transform({normalized.left, normalized.bottom}),
          matrix.Transform({normalized.right, normalized.bottom})};
}

CFX_FloatRect BoundsOfPoints(const std::array<CFX_PointF, 4>& points) {
  CFX_FloatRect bounds(points[0]);
  for (size_t i = 1; i < points.size(); ++i) {
    bounds.UpdateRect(points[i]);
  }
  return bounds;
}

FS_QUADPOINTSF QuadFromPoints(const std::array<CFX_PointF, 4>& points) {
  return {points[0].x, points[0].y, points[1].x, points[1].y,
          points[2].x, points[2].y, points[3].x, points[3].y};
}

}  // namespace

FPDF_EXPORT FPDF_TEXTPAGE FPDF_CALLCONV FPDFText_LoadPage(FPDF_PAGE page) {
  // EmbedPDF(view-scope): read through the document's view scope.
  ScopedFPDFPageView page_view(page);
  if (!page_view) {
    return nullptr;
  }
  CPDF_Page* pPDFPage = page_view.Get();

  CPDF_ViewerPreferences viewRef(pPDFPage->GetDocument());
  auto textpage =
      std::make_unique<CPDF_TextPage>(pPDFPage, viewRef.IsDirectionR2L());

  // Caller takes ownership.
  return FPDFTextPageFromCPDFTextPage(textpage.release());
}

FPDF_EXPORT void FPDF_CALLCONV FPDFText_ClosePage(FPDF_TEXTPAGE text_page) {
  // PDFium takes ownership.
  std::unique_ptr<CPDF_TextPage> textpage_deleter(
      CPDFTextPageFromFPDFTextPage(text_page));
}

FPDF_EXPORT int FPDF_CALLCONV FPDFText_CountChars(FPDF_TEXTPAGE text_page) {
  CPDF_TextPage* textpage = CPDFTextPageFromFPDFTextPage(text_page);
  return textpage ? textpage->CountChars() : -1;
}

FPDF_EXPORT unsigned int FPDF_CALLCONV
FPDFText_GetUnicode(FPDF_TEXTPAGE text_page, int index) {
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return 0;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  return charinfo.unicode();
}

FPDF_EXPORT FPDF_PAGEOBJECT FPDF_CALLCONV
FPDFText_GetTextObject(FPDF_TEXTPAGE text_page, int index) {
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return nullptr;
  }

  return FPDFPageObjectFromCPDFPageObject(
      textpage->GetCharInfo(index).text_object());
}

FPDF_EXPORT int FPDF_CALLCONV FPDFText_IsGenerated(FPDF_TEXTPAGE text_page,
                                                   int index) {
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return -1;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  return charinfo.char_type() == CPDF_TextPage::CharType::kGenerated ? 1 : 0;
}

FPDF_EXPORT int FPDF_CALLCONV FPDFText_IsHyphen(FPDF_TEXTPAGE text_page,
                                                int index) {
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return -1;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  return charinfo.char_type() == CPDF_TextPage::CharType::kHyphen;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFText_HasUnicodeMapError(FPDF_TEXTPAGE text_page, int index) {
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return -1;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  return charinfo.char_type() == CPDF_TextPage::CharType::kNotUnicode;
}

FPDF_EXPORT double FPDF_CALLCONV FPDFText_GetFontSize(FPDF_TEXTPAGE text_page,
                                                      int index) {
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return 0;
  }

  return textpage->GetCharFontSize(index);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFText_GetFontInfo(FPDF_TEXTPAGE text_page,
                     int index,
                     void* buffer,
                     unsigned long buflen,
                     int* flags) {
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return 0;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  if (!charinfo.text_object()) {
    return 0;
  }

  RetainPtr<CPDF_Font> font = charinfo.text_object()->GetFont();
  if (flags) {
    *flags = font->GetFontFlags();
  }

  // SAFETY: required from caller.
  auto result_span = UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen));
  ByteString basefont = font->GetBaseFontName();
  auto basefont_span = basefont.span_with_terminator();
  fxcrt::try_spancpy(result_span, basefont_span);
  return pdfium::checked_cast<unsigned long>(basefont_span.size());
}

FPDF_EXPORT int FPDF_CALLCONV FPDFText_GetFontWeight(FPDF_TEXTPAGE text_page,
                                                     int index) {
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return -1;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  if (!charinfo.text_object()) {
    return -1;
  }

  return charinfo.text_object()->GetFont()->GetFontWeight().value_or(-1);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFText_GetFillColor(FPDF_TEXTPAGE text_page,
                      int index,
                      unsigned int* R,
                      unsigned int* G,
                      unsigned int* B,
                      unsigned int* A) {
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage || !R || !G || !B || !A) {
    return false;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  if (!charinfo.text_object()) {
    return false;
  }

  FX_COLORREF fill_color =
      charinfo.text_object()->color_state().GetFillColorRef();
  *R = FXSYS_GetRValue(fill_color);
  *G = FXSYS_GetGValue(fill_color);
  *B = FXSYS_GetBValue(fill_color);
  *A = FXSYS_GetUnsignedAlpha(
      charinfo.text_object()->general_state().GetFillAlpha());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFText_GetStrokeColor(FPDF_TEXTPAGE text_page,
                        int index,
                        unsigned int* R,
                        unsigned int* G,
                        unsigned int* B,
                        unsigned int* A) {
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage || !R || !G || !B || !A) {
    return false;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  if (!charinfo.text_object()) {
    return false;
  }

  FX_COLORREF stroke_color =
      charinfo.text_object()->color_state().GetStrokeColorRef();
  *R = FXSYS_GetRValue(stroke_color);
  *G = FXSYS_GetGValue(stroke_color);
  *B = FXSYS_GetBValue(stroke_color);
  *A = FXSYS_GetUnsignedAlpha(
      charinfo.text_object()->general_state().GetStrokeAlpha());
  return true;
}

FPDF_EXPORT float FPDF_CALLCONV FPDFText_GetCharAngle(FPDF_TEXTPAGE text_page,
                                                      int index) {
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return -1.0f;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  // On the left is our current Matrix and on the right a generic rotation
  // matrix for our coordinate space.
  // | a  b  0 |    | cos(t)  -sin(t)  0 |
  // | c  d  0 |    | sin(t)   cos(t)  0 |
  // | e  f  1 |    |   0        0     1 |
  // Calculate the angle of the vector
  float angle = atan2f(charinfo.matrix().c, charinfo.matrix().a);
  if (angle < 0) {
    angle = 2 * FXSYS_PI + angle;
  }

  return angle;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFText_GetCharBox(FPDF_TEXTPAGE text_page,
                                                        int index,
                                                        double* left,
                                                        double* right,
                                                        double* bottom,
                                                        double* top) {
  if (!left || !right || !bottom || !top) {
    return false;
  }

  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return false;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  *left = charinfo.char_box().left;
  *right = charinfo.char_box().right;
  *bottom = charinfo.char_box().bottom;
  *top = charinfo.char_box().top;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFText_GetLooseCharBox(FPDF_TEXTPAGE text_page, int index, FS_RECTF* rect) {
  if (!rect) {
    return false;
  }

  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return false;
  }

  *rect = FSRectFFromCFXFloatRect(textpage->GetCharLooseBounds(index));
  return true;
}

// EmbedPDF(epdf-api): EPDFText_GetCharGeometry, EPDFText_GetTextFull and
// EPDFText_GetCharToTextMap.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFText_GetCharGeometry(FPDF_TEXTPAGE text_page,
                         int index,
                         EPDF_CHAR_GEOMETRY* geometry) {
  if (!geometry) {
    return false;
  }
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return false;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  EPDF_CHAR_GEOMETRY result = {};
  result.matrix = FSMatrixFromCFXMatrix(charinfo.matrix());
  result.loose_box = FSRectFFromCFXFloatRect(charinfo.loose_char_box());
  result.tight_box = FSRectFFromCFXFloatRect(charinfo.char_box());

  if (!charinfo.char_box().IsEmpty()) {
    result.flags |= EPDF_CHARGEO_HAS_TIGHT_BOX;
  }
  if (charinfo.unicode() == L' ') {
    result.flags |= EPDF_CHARGEO_SPACE;
  }
  if (charinfo.char_type() == CPDF_TextPage::CharType::kGenerated ||
      charinfo.char_type() == CPDF_TextPage::CharType::kPiece ||
      charinfo.char_type() == CPDF_TextPage::CharType::kActualText) {
    result.flags |= EPDF_CHARGEO_SYNTHESIZED;
  }
  if (IsUpright(charinfo.matrix())) {
    result.flags |= EPDF_CHARGEO_UPRIGHT;
  }

  std::optional<CPDF_TextPage::CharLocalGeometry> local_geometry =
      textpage->GetCharLocalGeometry(index);
  if (local_geometry && HasUsableBasis(charinfo.matrix())) {
    const std::array<CFX_PointF, 4> loose_points =
        TransformLocalBox(local_geometry->loose_box, charinfo.matrix());
    result.loose_quad = QuadFromPoints(loose_points);
    result.loose_box = FSRectFFromCFXFloatRect(BoundsOfPoints(loose_points));
    result.flags |= EPDF_CHARGEO_HAS_LOOSE_QUAD;

    if (local_geometry->tight_box) {
      const std::array<CFX_PointF, 4> tight_points =
          TransformLocalBox(*local_geometry->tight_box, charinfo.matrix());
      result.tight_quad = QuadFromPoints(tight_points);
      result.tight_box = FSRectFFromCFXFloatRect(BoundsOfPoints(tight_points));
      result.flags |= EPDF_CHARGEO_HAS_TIGHT_BOX | EPDF_CHARGEO_HAS_TIGHT_QUAD;
    }
  }

  const CFX_FloatRect result_loose = CFXFloatRectFromFSRectF(result.loose_box);
  if (result_loose.IsEmpty()) {
    result.flags |= EPDF_CHARGEO_EMPTY;
    result.flags &= ~(EPDF_CHARGEO_HAS_LOOSE_QUAD |
                      EPDF_CHARGEO_HAS_TIGHT_QUAD | EPDF_CHARGEO_HAS_TIGHT_BOX);
    result.loose_quad = {};
    result.tight_quad = {};
    result.tight_box = {};
  }

  *geometry = result;
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFText_GetTextFull(FPDF_TEXTPAGE text_page,
                                                   unsigned short* buffer,
                                                   int buffer_len) {
  CPDF_TextPage* textpage = CPDFTextPageFromFPDFTextPage(text_page);
  if (!textpage) {
    return 0;
  }
  const int char_count = textpage->CountChars();
  WideString str =
      char_count > 0 ? textpage->GetPageText(0, char_count) : WideString();
  // True UTF-16LE (surrogate pairs preserved), unlike FPDFText_GetText's
  // ToUCS2LE which silently skips supplementary code points. Includes the
  // two-byte terminator in the string data itself.
  ByteString encoded = str.ToUTF16LE();
  auto encoded_span =
      fxcrt::reinterpret_span<const unsigned short>(encoded.span());
  const int required = pdfium::checked_cast<int>(encoded_span.size());
  if (buffer && buffer_len >= required) {
    // SAFETY: required from caller. The two-call contract states that
    // `buffer` must hold `buffer_len` UTF-16 units.
    fxcrt::Copy(encoded_span, UNSAFE_BUFFERS(pdfium::span(
                                  buffer, static_cast<size_t>(buffer_len))));
  }
  return required;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFText_GetCharToTextMap(FPDF_TEXTPAGE text_page,
                          EPDF_CHAR_MAP_ANCHOR* anchors,
                          int anchors_len) {
  CPDF_TextPage* textpage = CPDFTextPageFromFPDFTextPage(text_page);
  if (!textpage) {
    return -1;
  }
  // SAFETY: required from caller. The two-call contract states that
  // `anchors` must hold `anchors_len` entries.
  pdfium::span<EPDF_CHAR_MAP_ANCHOR> out =
      anchors && anchors_len > 0
          ? UNSAFE_BUFFERS(
                pdfium::span(anchors, static_cast<size_t>(anchors_len)))
          : pdfium::span<EPDF_CHAR_MAP_ANCHOR>();
  const int char_count = textpage->CountChars();
  int required = 0;
  int offset = 0;
  for (int i = 0; i < char_count; ++i) {
    int units = 1;
    if (textpage->TextIndexFromCharIndex(i) < 0) {
      // Non-printing: occupies a character slot, contributes no text.
      units = 0;
    } else if (pdfium::IsSupplementary(
                   static_cast<uint32_t>(
                       textpage->GetCharInfo(static_cast<size_t>(i))
                           .unicode()))) {
      // Mirrors FX_UTF16Encode exactly: a supplementary code point becomes
      // a surrogate pair in EPDFText_GetTextFull output; every other wchar
      // (lone surrogate halves and out-of-range values included) is one
      // truncated code unit.
      units = 2;
    }
    if (units != 1) {
      if (static_cast<size_t>(required) < out.size()) {
        out[static_cast<size_t>(required)] = {i + 1, offset + units};
      }
      ++required;
    }
    offset += units;
  }
  return required;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFText_GetMatrix(FPDF_TEXTPAGE text_page,
                                                       int index,
                                                       FS_MATRIX* matrix) {
  if (!matrix) {
    return false;
  }

  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return false;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  *matrix = FSMatrixFromCFXMatrix(charinfo.matrix());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFText_GetCharOrigin(FPDF_TEXTPAGE text_page,
                       int index,
                       double* x,
                       double* y) {
  CPDF_TextPage* textpage = GetTextPageForValidIndex(text_page, index);
  if (!textpage) {
    return false;
  }

  const CPDF_TextPage::CharInfo& charinfo = textpage->GetCharInfo(index);
  *x = charinfo.origin().x;
  *y = charinfo.origin().y;
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFText_GetCharIndexAtPos(FPDF_TEXTPAGE text_page,
                           double x,
                           double y,
                           double xTolerance,
                           double yTolerance) {
  CPDF_TextPage* textpage = CPDFTextPageFromFPDFTextPage(text_page);
  if (!textpage) {
    return -3;
  }

  return textpage->GetIndexAtPos(
      CFX_PointF(static_cast<float>(x), static_cast<float>(y)),
      CFX_SizeF(static_cast<float>(xTolerance),
                static_cast<float>(yTolerance)));
}

FPDF_EXPORT int FPDF_CALLCONV FPDFText_GetText(FPDF_TEXTPAGE page,
                                               int start_index,
                                               int char_count,
                                               unsigned short* result) {
  CPDF_TextPage* textpage = CPDFTextPageFromFPDFTextPage(page);
  if (!textpage || start_index < 0 || char_count < 0 || !result) {
    return 0;
  }
  int char_available = textpage->CountChars() - start_index;
  if (char_available <= 0) {
    return 0;
  }
  char_count = std::min(char_count, char_available);
  if (char_count == 0) {
    // Writing out "", which has a character count of 1 due to the NUL.
    *result = '\0';
    return 1;
  }
  // SAFETY: Required from caller. Public API description states that
  // `result` must be able to hold `char_count` characters plus a
  // terminator.
  CHECK_LT(char_count, std::numeric_limits<int>::max());
  pdfium::span<unsigned short> result_span =
      UNSAFE_BUFFERS(pdfium::span(result, static_cast<size_t>(char_count + 1)));

  // Includes two-byte terminator in string data itself.
  ByteString str = textpage->GetPageText(start_index, char_count).ToUCS2LE();
  auto str_span = fxcrt::reinterpret_span<const unsigned short>(str.span());

  // Hard CHECK() in Copy() if retrieved text is too long.
  fxcrt::Copy(str_span, result_span);
  return pdfium::checked_cast<int>(str_span.size());
}

FPDF_EXPORT int FPDF_CALLCONV FPDFText_CountRects(FPDF_TEXTPAGE text_page,
                                                  int start,
                                                  int count) {
  CPDF_TextPage* textpage = CPDFTextPageFromFPDFTextPage(text_page);
  return textpage ? textpage->CountRects(start, count) : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFText_GetRect(FPDF_TEXTPAGE text_page,
                                                     int rect_index,
                                                     double* left,
                                                     double* top,
                                                     double* right,
                                                     double* bottom) {
  CPDF_TextPage* textpage = CPDFTextPageFromFPDFTextPage(text_page);
  if (!textpage) {
    return false;
  }

  CFX_FloatRect rect;
  bool result = textpage->GetRect(rect_index, &rect);

  *left = rect.left;
  *top = rect.top;
  *right = rect.right;
  *bottom = rect.bottom;
  return result;
}

FPDF_EXPORT int FPDF_CALLCONV FPDFText_GetBoundedText(FPDF_TEXTPAGE text_page,
                                                      double left,
                                                      double top,
                                                      double right,
                                                      double bottom,
                                                      unsigned short* buffer,
                                                      int buflen) {
  CPDF_TextPage* textpage = CPDFTextPageFromFPDFTextPage(text_page);
  if (!textpage) {
    return 0;
  }
  CFX_FloatRect rect((float)left, (float)bottom, (float)right, (float)top);
  WideString wstr = textpage->GetTextByRect(rect);
  if (buflen <= 0 || !buffer) {
    return pdfium::checked_cast<int>(wstr.GetLength());
  }

  // SAFETY: Required from caller. Public API states that buflen
  // describes the number of values buffer can hold.
  const auto buffer_span =
      UNSAFE_BUFFERS(pdfium::span(buffer, static_cast<size_t>(buflen)));

  ByteString str = wstr.ToUTF16LE();
  pdfium::span<const char> str_span = str.span();
  auto copy_span = fxcrt::reinterpret_span<const unsigned short>(str_span);
  if (copy_span.size() > buffer_span.size()) {
    copy_span = copy_span.first(buffer_span.size());
  }
  fxcrt::Copy(copy_span, buffer_span);
  return pdfium::checked_cast<int>(copy_span.size());
}

FPDF_EXPORT FPDF_SCHHANDLE FPDF_CALLCONV
FPDFText_FindStart(FPDF_TEXTPAGE text_page,
                   FPDF_WIDESTRING findwhat,
                   unsigned long flags,
                   int start_index) {
  CPDF_TextPage* textpage = CPDFTextPageFromFPDFTextPage(text_page);
  if (!textpage) {
    return nullptr;
  }

  CPDF_TextPageFind::Options options;
  options.bMatchCase = !!(flags & FPDF_MATCHCASE);
  options.bMatchWholeWord = !!(flags & FPDF_MATCHWHOLEWORD);
  options.bConsecutive = !!(flags & FPDF_CONSECUTIVE);

  // SAFETY: required from caller.
  auto find = CPDF_TextPageFind::Create(
      textpage, UNSAFE_BUFFERS(WideStringFromFPDFWideString(findwhat)), options,
      start_index >= 0 ? std::optional<size_t>(start_index) : std::nullopt);

  // Caller takes ownership.
  return FPDFSchHandleFromCPDFTextPageFind(find.release());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFText_FindNext(FPDF_SCHHANDLE handle) {
  if (!handle) {
    return false;
  }

  CPDF_TextPageFind* textpageFind = CPDFTextPageFindFromFPDFSchHandle(handle);
  return textpageFind->FindNext();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFText_FindPrev(FPDF_SCHHANDLE handle) {
  if (!handle) {
    return false;
  }

  CPDF_TextPageFind* textpageFind = CPDFTextPageFindFromFPDFSchHandle(handle);
  return textpageFind->FindPrev();
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFText_GetSchResultIndex(FPDF_SCHHANDLE handle) {
  if (!handle) {
    return 0;
  }

  CPDF_TextPageFind* textpageFind = CPDFTextPageFindFromFPDFSchHandle(handle);
  return textpageFind->GetCurOrder();
}

FPDF_EXPORT int FPDF_CALLCONV FPDFText_GetSchCount(FPDF_SCHHANDLE handle) {
  if (!handle) {
    return 0;
  }

  CPDF_TextPageFind* textpageFind = CPDFTextPageFindFromFPDFSchHandle(handle);
  return textpageFind->GetMatchedCount();
}

FPDF_EXPORT void FPDF_CALLCONV FPDFText_FindClose(FPDF_SCHHANDLE handle) {
  if (!handle) {
    return;
  }

  // Take ownership back from caller and destroy.
  std::unique_ptr<CPDF_TextPageFind> textpageFind(
      CPDFTextPageFindFromFPDFSchHandle(handle));
}

// web link
FPDF_EXPORT FPDF_PAGELINK FPDF_CALLCONV
FPDFLink_LoadWebLinks(FPDF_TEXTPAGE text_page) {
  CPDF_TextPage* textpage = CPDFTextPageFromFPDFTextPage(text_page);
  if (!textpage) {
    return nullptr;
  }

  auto pagelink = std::make_unique<CPDF_LinkExtract>(textpage);
  pagelink->ExtractLinks();

  // Caller takes ownership.
  return FPDFPageLinkFromCPDFLinkExtract(pagelink.release());
}

FPDF_EXPORT int FPDF_CALLCONV FPDFLink_CountWebLinks(FPDF_PAGELINK link_page) {
  if (!link_page) {
    return 0;
  }

  CPDF_LinkExtract* pageLink = CPDFLinkExtractFromFPDFPageLink(link_page);
  return pdfium::checked_cast<int>(pageLink->CountLinks());
}

FPDF_EXPORT int FPDF_CALLCONV FPDFLink_GetURL(FPDF_PAGELINK link_page,
                                              int link_index,
                                              unsigned short* buffer,
                                              int buflen) {
  WideString wsUrl(L"");
  if (link_page && link_index >= 0) {
    CPDF_LinkExtract* pageLink = CPDFLinkExtractFromFPDFPageLink(link_page);
    wsUrl = pageLink->GetURL(link_index);
  }
  ByteString cbUTF16URL = wsUrl.ToUTF16LE();
  auto url_span =
      fxcrt::reinterpret_span<const unsigned short>(cbUTF16URL.span());
  if (!buffer || buflen <= 0) {
    return pdfium::checked_cast<int>(url_span.size());
  }

  // SAFETY: required from caller.
  pdfium::span<unsigned short> result_span =
      UNSAFE_BUFFERS(pdfium::span(buffer, static_cast<size_t>(buflen)));

  size_t size = std::min(url_span.size(), result_span.size());
  fxcrt::Copy(url_span.first(size), result_span);
  return pdfium::checked_cast<int>(size);
}

FPDF_EXPORT int FPDF_CALLCONV FPDFLink_CountRects(FPDF_PAGELINK link_page,
                                                  int link_index) {
  if (!link_page || link_index < 0) {
    return 0;
  }

  CPDF_LinkExtract* pageLink = CPDFLinkExtractFromFPDFPageLink(link_page);
  return fxcrt::CollectionSize<int>(pageLink->GetRects(link_index));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFLink_GetRect(FPDF_PAGELINK link_page,
                                                     int link_index,
                                                     int rect_index,
                                                     double* left,
                                                     double* top,
                                                     double* right,
                                                     double* bottom) {
  if (!link_page || link_index < 0 || rect_index < 0) {
    return false;
  }

  CPDF_LinkExtract* pageLink = CPDFLinkExtractFromFPDFPageLink(link_page);
  std::vector<CFX_FloatRect> rectArray = pageLink->GetRects(link_index);
  if (rect_index >= fxcrt::CollectionSize<int>(rectArray)) {
    return false;
  }

  *left = rectArray[rect_index].left;
  *right = rectArray[rect_index].right;
  *top = rectArray[rect_index].top;
  *bottom = rectArray[rect_index].bottom;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFLink_GetTextRange(FPDF_PAGELINK link_page,
                      int link_index,
                      int* start_char_index,
                      int* char_count) {
  if (!link_page || link_index < 0) {
    return false;
  }

  CPDF_LinkExtract* page_link = CPDFLinkExtractFromFPDFPageLink(link_page);
  auto maybe_range = page_link->GetTextRange(link_index);
  if (!maybe_range.has_value()) {
    return false;
  }

  *start_char_index = pdfium::checked_cast<int>(maybe_range.value().start_);
  *char_count = pdfium::checked_cast<int>(maybe_range.value().count_);
  return true;
}

FPDF_EXPORT void FPDF_CALLCONV FPDFLink_CloseWebLinks(FPDF_PAGELINK link_page) {
  delete CPDFLinkExtractFromFPDFPageLink(link_page);
}
