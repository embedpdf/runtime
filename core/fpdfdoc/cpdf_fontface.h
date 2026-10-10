// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFDOC_CPDF_FONTFACE_H_
#define CORE_FPDFDOC_CPDF_FONTFACE_H_

#include <initializer_list>
#include <optional>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/widestring.h"

class CPDF_Dictionary;
class CPDF_Document;

// A font as a face: its family, its weight (100 to 900) and whether it's
// italic. What a font descriptor says, and how rich text names a font.
struct CPDF_FontFace {
  WideString family;
  int weight = 400;
  bool italic = false;
};

// The face a standard 14 name or one of Acrobat's aliases (Helv, TiBo, ...)
// stands for: its standard family ("Arial" is Helvetica).
std::optional<CPDF_FontFace> StandardFontFace(const ByteString& name);

// The face a /BaseFont name says it is. "ABCDEF+MinionPro-BoldItalic" is
// MinionPro, 700, italic: the subset tag goes, the style after '-' or ','
// sets the weight and italic, and a standard name maps to its family.
CPDF_FontFace FaceOfBaseFontName(ByteString base_font);

// A font dictionary's /FontDescriptor, or its descendant font's for Type0.
RetainPtr<const CPDF_Dictionary> FontDescriptorOf(
    const CPDF_Dictionary* font_dict);

// The face a font dictionary says it is: what its /BaseFont name says, with
// its descriptor's /FontFamily and /FontWeight where it has them; italic when
// the name, the /ItalicAngle or the Italic flag says so, bold (without a
// /FontWeight) when the ForceBold flag does.
CPDF_FontFace FaceOfFontDict(const CPDF_Dictionary* font_dict);

// The face /DA's font |font_name| stands for: the font the first of
// |default_resources| (/DR dictionaries, nulls skipped) holding that name is;
// else the registered font the name is an alias of in |doc|; else a standard
// alias; else the name itself as a family.
CPDF_FontFace FaceOfDefaultAppearanceFont(
    const CPDF_Document* doc,
    const ByteString& font_name,
    std::initializer_list<const CPDF_Dictionary*> default_resources);

#endif  // CORE_FPDFDOC_CPDF_FONTFACE_H_
