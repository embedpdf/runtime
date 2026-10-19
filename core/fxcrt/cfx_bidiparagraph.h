// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF(fonts): the Unicode bidirectional algorithm (UAX #9) over one
// paragraph, through ICU's ubidi: the level each character resolves to, the
// levels of one line of it, and the order a line's runs are drawn in.

#ifndef CORE_FXCRT_CFX_BIDIPARAGRAPH_H_
#define CORE_FXCRT_CFX_BIDIPARAGRAPH_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <vector>

#include "core/fxcrt/span.h"
#include "core/fxcrt/widestring.h"

class CFX_BidiParagraph {
 public:
  // Resolves |text| as one paragraph whose level is 1 when |rtl|, else 0.
  CFX_BidiParagraph(const WideString& text, bool rtl);
  ~CFX_BidiParagraph();

  uint8_t base_level() const { return base_level_; }

  // One level per character of the text; an odd level runs right to left.
  const std::vector<uint8_t>& levels() const { return levels_; }

  // The levels of [begin, end) laid out as one line: whitespace at the end of
  // the line takes the paragraph's level (rule L1), so it trails the line in
  // the paragraph's direction.
  std::vector<uint8_t> LineLevels(size_t begin, size_t end) const;

  // Whether |text| reads right to left by its first strong character outside
  // isolates (rules P2 and P3). nullopt when it has no strong character.
  static std::optional<bool> FirstStrongIsRtl(WideStringView text);

  // The order to draw runs with these levels in, left to right, as their
  // logical indices (rule L2).
  static std::vector<size_t> VisualOrder(pdfium::span<const uint8_t> levels);

 private:
  const WideString text_;
  const uint8_t base_level_;
  std::vector<uint8_t> levels_;
};

#endif  // CORE_FXCRT_CFX_BIDIPARAGRAPH_H_
