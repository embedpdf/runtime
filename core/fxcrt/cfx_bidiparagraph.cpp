// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fxcrt/cfx_bidiparagraph.h"

#include <limits>
#include <memory>
#include <numeric>
#include <string>

#include "core/fxcrt/check_op.h"
#include "core/fxcrt/code_point_view.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/utf16.h"

#if defined(USE_SYSTEM_ICUUC)
#include <unicode/ubidi.h>
#include <unicode/uchar.h>
#else
#include "third_party/icu/source/common/unicode/ubidi.h"
#include "third_party/icu/source/common/unicode/uchar.h"
#endif

namespace {

struct UBiDiDeleter {
  void operator()(UBiDi* bidi) const { ubidi_close(bidi); }
};

// What rule L1 resets at the end of a line: whitespace, separators, and the
// formatting characters the algorithm otherwise passes over.
bool TrailsAtParagraphLevel(uint32_t code_point) {
  switch (u_charDirection(static_cast<UChar32>(code_point))) {
    case U_WHITE_SPACE_NEUTRAL:
    case U_SEGMENT_SEPARATOR:
    case U_BLOCK_SEPARATOR:
    case U_BOUNDARY_NEUTRAL:
    case U_LEFT_TO_RIGHT_EMBEDDING:
    case U_LEFT_TO_RIGHT_OVERRIDE:
    case U_RIGHT_TO_LEFT_EMBEDDING:
    case U_RIGHT_TO_LEFT_OVERRIDE:
    case U_POP_DIRECTIONAL_FORMAT:
    case U_LEFT_TO_RIGHT_ISOLATE:
    case U_RIGHT_TO_LEFT_ISOLATE:
    case U_FIRST_STRONG_ISOLATE:
    case U_POP_DIRECTIONAL_ISOLATE:
      return true;
    default:
      return false;
  }
}

}  // namespace

CFX_BidiParagraph::CFX_BidiParagraph(const WideString& text, bool rtl)
    : text_(text),
      base_level_(rtl ? 1 : 0),
      levels_(text.GetLength(), base_level_) {
  // ICU counts UTF-16 code units; each character's level is its first
  // unit's.
  std::u16string units;
  std::vector<size_t> first_unit;
  first_unit.reserve(text.GetLength());
  for (wchar_t ch : text) {
    first_unit.push_back(units.size());
    const uint32_t code_point = static_cast<uint32_t>(ch);
    if (pdfium::IsSupplementary(code_point)) {
      const pdfium::SurrogatePair pair(static_cast<char32_t>(code_point));
      units += pair.high();
      units += pair.low();
    } else if (code_point > 0xFFFF) {
      units += u'�';
    } else {
      units += static_cast<char16_t>(code_point);
    }
  }
  // ICU's run bookkeeping overflows past this length (crbug.com/504629701).
  constexpr size_t kMaxUnits =
      std::numeric_limits<int32_t>::max() / (3 * sizeof(int32_t));
  if (units.empty() || units.size() > kMaxUnits) {
    return;
  }
  const int32_t length = pdfium::checked_cast<int32_t>(units.size());
  UErrorCode status = U_ZERO_ERROR;
  std::unique_ptr<UBiDi, UBiDiDeleter> bidi(
      ubidi_openSized(length, /*maxRunCount=*/0, &status));
  if (U_FAILURE(status)) {
    return;
  }
  ubidi_setPara(bidi.get(), units.data(), length, base_level_,
                /*embeddingLevels=*/nullptr, &status);
  if (U_FAILURE(status)) {
    return;
  }
  const UBiDiLevel* levels = ubidi_getLevels(bidi.get(), &status);
  if (U_FAILURE(status) || !levels) {
    return;
  }
  for (size_t i = 0; i < first_unit.size(); ++i) {
    levels_[i] = levels[first_unit[i]];
  }
}

CFX_BidiParagraph::~CFX_BidiParagraph() = default;

std::vector<uint8_t> CFX_BidiParagraph::LineLevels(size_t begin,
                                                   size_t end) const {
  CHECK_LE(begin, end);
  CHECK_LE(end, levels_.size());
  std::vector<uint8_t> line(levels_.begin() + begin, levels_.begin() + end);
  for (size_t i = line.size();
       i > 0 &&
       TrailsAtParagraphLevel(static_cast<uint32_t>(text_[begin + i - 1]));
       --i) {
    line[i - 1] = base_level_;
  }
  return line;
}

// static
std::optional<bool> CFX_BidiParagraph::FirstStrongIsRtl(WideStringView text) {
  int open_isolates = 0;
  for (char32_t code_point : pdfium::CodePointView(text)) {
    switch (u_charDirection(static_cast<UChar32>(code_point))) {
      case U_LEFT_TO_RIGHT:
        if (open_isolates == 0) {
          return false;
        }
        break;
      case U_RIGHT_TO_LEFT:
      case U_RIGHT_TO_LEFT_ARABIC:
        if (open_isolates == 0) {
          return true;
        }
        break;
      case U_LEFT_TO_RIGHT_ISOLATE:
      case U_RIGHT_TO_LEFT_ISOLATE:
      case U_FIRST_STRONG_ISOLATE:
        ++open_isolates;
        break;
      case U_POP_DIRECTIONAL_ISOLATE:
        if (open_isolates > 0) {
          --open_isolates;
        }
        break;
      default:
        break;
    }
  }
  return std::nullopt;
}

// static
std::vector<size_t> CFX_BidiParagraph::VisualOrder(
    pdfium::span<const uint8_t> levels) {
  std::vector<size_t> order(levels.size());
  std::iota(order.begin(), order.end(), 0);
  if (levels.empty()) {
    return order;
  }
  std::vector<int32_t> logical_index(levels.size());
  std::iota(logical_index.begin(), logical_index.end(), 0);
  ubidi_reorderVisual(levels.data(),
                      pdfium::checked_cast<int32_t>(levels.size()),
                      logical_index.data());
  for (size_t i = 0; i < order.size(); ++i) {
    order[i] = pdfium::checked_cast<size_t>(logical_index[i]);
  }
  return order;
}
