// Copyright 2017 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>
#include <set>
#include <vector>

#include "constants/annotation_common.h"
#include "core/fpdfapi/edit/cpdf_contentstream_write_utils.h"
#include "core/fpdfapi/edit/cpdf_pagecontentgenerator.h"
#include "core/fpdfapi/edit/cpdf_pageorganizer.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_streamparser.h"
#include "core/fdrm/fx_crypt.h"
#include "core/fpdfapi/edit/cpdf_stringarchivestream.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_color_utils.h"
#include "core/fpdfdoc/cpdf_formfield.h"
#include "core/fpdfdoc/cpdf_generateap.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fpdfdoc/cpdf_richtextjson.h"
#include "core/fpdfdoc/cpdf_richtextparser.h"
#include "core/fpdfdoc/cpdf_richtextwriter.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/containers/contains.h"
#include "core/fxcrt/containers/unique_ptr_adapters.h"
#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/ptr_util.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxge/cfx_color.h"
#include "core/fxge/cfx_fontregistry.h"
#include "fpdfsdk/cpdfsdk_formfillenvironment.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/cpdfsdk_interactiveform.h"
#include "fpdfsdk/epdf_appearance_exporter.h"
#include "fpdfsdk/epdf_wrapped_appearance.h"

namespace {

// These checks ensure the consistency of annotation subtype values across core/
// and public.
static_assert(static_cast<int>(CPDF_Annot::Subtype::UNKNOWN) ==
                  FPDF_ANNOT_UNKNOWN,
              "CPDF_Annot::UNKNOWN value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::TEXT) == FPDF_ANNOT_TEXT,
              "CPDF_Annot::TEXT value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::LINK) == FPDF_ANNOT_LINK,
              "CPDF_Annot::LINK value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::FREETEXT) ==
                  FPDF_ANNOT_FREETEXT,
              "CPDF_Annot::FREETEXT value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::LINE) == FPDF_ANNOT_LINE,
              "CPDF_Annot::LINE value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::SQUARE) ==
                  FPDF_ANNOT_SQUARE,
              "CPDF_Annot::SQUARE value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::CIRCLE) ==
                  FPDF_ANNOT_CIRCLE,
              "CPDF_Annot::CIRCLE value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::POLYGON) ==
                  FPDF_ANNOT_POLYGON,
              "CPDF_Annot::POLYGON value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::POLYLINE) ==
                  FPDF_ANNOT_POLYLINE,
              "CPDF_Annot::POLYLINE value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::HIGHLIGHT) ==
                  FPDF_ANNOT_HIGHLIGHT,
              "CPDF_Annot::HIGHLIGHT value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::UNDERLINE) ==
                  FPDF_ANNOT_UNDERLINE,
              "CPDF_Annot::UNDERLINE value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::SQUIGGLY) ==
                  FPDF_ANNOT_SQUIGGLY,
              "CPDF_Annot::SQUIGGLY value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::STRIKEOUT) ==
                  FPDF_ANNOT_STRIKEOUT,
              "CPDF_Annot::STRIKEOUT value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::STAMP) == FPDF_ANNOT_STAMP,
              "CPDF_Annot::STAMP value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::CARET) == FPDF_ANNOT_CARET,
              "CPDF_Annot::CARET value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::INK) == FPDF_ANNOT_INK,
              "CPDF_Annot::INK value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::POPUP) == FPDF_ANNOT_POPUP,
              "CPDF_Annot::POPUP value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::FILEATTACHMENT) ==
                  FPDF_ANNOT_FILEATTACHMENT,
              "CPDF_Annot::FILEATTACHMENT value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::SOUND) == FPDF_ANNOT_SOUND,
              "CPDF_Annot::SOUND value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::MOVIE) == FPDF_ANNOT_MOVIE,
              "CPDF_Annot::MOVIE value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::WIDGET) ==
                  FPDF_ANNOT_WIDGET,
              "CPDF_Annot::WIDGET value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::SCREEN) ==
                  FPDF_ANNOT_SCREEN,
              "CPDF_Annot::SCREEN value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::PRINTERMARK) ==
                  FPDF_ANNOT_PRINTERMARK,
              "CPDF_Annot::PRINTERMARK value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::TRAPNET) ==
                  FPDF_ANNOT_TRAPNET,
              "CPDF_Annot::TRAPNET value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::WATERMARK) ==
                  FPDF_ANNOT_WATERMARK,
              "CPDF_Annot::WATERMARK value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::THREED) ==
                  FPDF_ANNOT_THREED,
              "CPDF_Annot::THREED value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::RICHMEDIA) ==
                  FPDF_ANNOT_RICHMEDIA,
              "CPDF_Annot::RICHMEDIA value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::XFAWIDGET) ==
                  FPDF_ANNOT_XFAWIDGET,
              "CPDF_Annot::XFAWIDGET value mismatch");
static_assert(static_cast<int>(CPDF_Annot::Subtype::REDACT) ==
                  FPDF_ANNOT_REDACT,
              "CPDF_Annot::REDACT value mismatch");

// These checks ensure the consistency of annotation appearance mode values
// across core/ and public.
static_assert(static_cast<int>(CPDF_Annot::AppearanceMode::kNormal) ==
                  FPDF_ANNOT_APPEARANCEMODE_NORMAL,
              "CPDF_Annot::AppearanceMode::Normal value mismatch");
static_assert(static_cast<int>(CPDF_Annot::AppearanceMode::kRollover) ==
                  FPDF_ANNOT_APPEARANCEMODE_ROLLOVER,
              "CPDF_Annot::AppearanceMode::Rollover value mismatch");
static_assert(static_cast<int>(CPDF_Annot::AppearanceMode::kDown) ==
                  FPDF_ANNOT_APPEARANCEMODE_DOWN,
              "CPDF_Annot::AppearanceMode::Down value mismatch");

// These checks ensure the consistency of dictionary value types across core/
// and public/.
static_assert(static_cast<int>(CPDF_Object::Type::kBoolean) ==
                  FPDF_OBJECT_BOOLEAN,
              "CPDF_Object::kBoolean value mismatch");
static_assert(static_cast<int>(CPDF_Object::Type::kNumber) ==
                  FPDF_OBJECT_NUMBER,
              "CPDF_Object::kNumber value mismatch");
static_assert(static_cast<int>(CPDF_Object::Type::kString) ==
                  FPDF_OBJECT_STRING,
              "CPDF_Object::kString value mismatch");
static_assert(static_cast<int>(CPDF_Object::Type::kName) == FPDF_OBJECT_NAME,
              "CPDF_Object::kName value mismatch");
static_assert(static_cast<int>(CPDF_Object::Type::kArray) == FPDF_OBJECT_ARRAY,
              "CPDF_Object::kArray value mismatch");
static_assert(static_cast<int>(CPDF_Object::Type::kDictionary) ==
                  FPDF_OBJECT_DICTIONARY,
              "CPDF_Object::kDictionary value mismatch");
static_assert(static_cast<int>(CPDF_Object::Type::kStream) ==
                  FPDF_OBJECT_STREAM,
              "CPDF_Object::kStream value mismatch");
static_assert(static_cast<int>(CPDF_Object::Type::kNullobj) ==
                  FPDF_OBJECT_NULLOBJ,
              "CPDF_Object::kNullobj value mismatch");
static_assert(static_cast<int>(CPDF_Object::Type::kReference) ==
                  FPDF_OBJECT_REFERENCE,
              "CPDF_Object::kReference value mismatch");

// These checks ensure the consistency of annotation additional action event
// values across core/ and public.
static_assert(static_cast<int>(CPDF_AAction::kKeyStroke) ==
                  FPDF_ANNOT_AACTION_KEY_STROKE,
              "CPDF_AAction::kKeyStroke value mismatch");
static_assert(static_cast<int>(CPDF_AAction::kFormat) ==
                  FPDF_ANNOT_AACTION_FORMAT,
              "CPDF_AAction::kFormat value mismatch");
static_assert(static_cast<int>(CPDF_AAction::kValidate) ==
                  FPDF_ANNOT_AACTION_VALIDATE,
              "CPDF_AAction::kValidate value mismatch");
static_assert(static_cast<int>(CPDF_AAction::kCalculate) ==
                  FPDF_ANNOT_AACTION_CALCULATE,
              "CPDF_AAction::kCalculate value mismatch");

// These checks ensure the consistency of annotation border style values
// across core/ and public.
static_assert(static_cast<int>(CPDF_Annot::BorderStyle::kSolid) ==
                  FPDF_ANNOT_BS_SOLID,
              "CPDF_Annot::BorderStyle::kSolid value mismatch");
static_assert(static_cast<int>(CPDF_Annot::BorderStyle::kDashed) ==
                  FPDF_ANNOT_BS_DASHED,
              "CPDF_Annot::BorderStyle::kDashed value mismatch");
static_assert(static_cast<int>(CPDF_Annot::BorderStyle::kBeveled) ==
                  FPDF_ANNOT_BS_BEVELED,
              "CPDF_Annot::BorderStyle::kBeveled value mismatch");
static_assert(static_cast<int>(CPDF_Annot::BorderStyle::kInset) ==
                  FPDF_ANNOT_BS_INSET,
              "CPDF_Annot::BorderStyle::kInset value mismatch");
static_assert(static_cast<int>(CPDF_Annot::BorderStyle::kUnderline) ==
                  FPDF_ANNOT_BS_UNDERLINE,
              "CPDF_Annot::BorderStyle::kUnderline value mismatch");
static_assert(static_cast<int>(CPDF_Annot::BorderStyle::kUnknown) ==
                  FPDF_ANNOT_BS_UNKNOWN,
              "CPDF_Annot::BorderStyle::kUnknown value mismatch");

// These checks ensure the consistency of blend mode values across core/ and
// public.
static_assert(static_cast<int>(BlendMode::kNormal) == FPDF_BLENDMODE_Normal,
              "BlendMode::kNormal value mismatch");
static_assert(static_cast<int>(BlendMode::kMultiply) == FPDF_BLENDMODE_Multiply,
              "BlendMode::kMultiply value mismatch");
static_assert(static_cast<int>(BlendMode::kScreen) == FPDF_BLENDMODE_Screen,
              "BlendMode::kScreen value mismatch");
static_assert(static_cast<int>(BlendMode::kOverlay) == FPDF_BLENDMODE_Overlay,
              "BlendMode::kOverlay value mismatch");
static_assert(static_cast<int>(BlendMode::kDarken) == FPDF_BLENDMODE_Darken,
              "BlendMode::kDarken value mismatch");
static_assert(static_cast<int>(BlendMode::kLighten) == FPDF_BLENDMODE_Lighten,
              "BlendMode::kLighten value mismatch");
static_assert(static_cast<int>(BlendMode::kColorDodge) ==
                  FPDF_BLENDMODE_ColorDodge,
              "BlendMode::kColorDodge value mismatch");
static_assert(static_cast<int>(BlendMode::kColorBurn) ==
                  FPDF_BLENDMODE_ColorBurn,
              "BlendMode::kColorBurn value mismatch");
static_assert(static_cast<int>(BlendMode::kHardLight) ==
                  FPDF_BLENDMODE_HardLight,
              "BlendMode::kHardLight value mismatch");
static_assert(static_cast<int>(BlendMode::kSoftLight) ==
                  FPDF_BLENDMODE_SoftLight,
              "BlendMode::kSoftLight value mismatch");
static_assert(static_cast<int>(BlendMode::kDifference) ==
                  FPDF_BLENDMODE_Difference,
              "BlendMode::kDifference value mismatch");
static_assert(static_cast<int>(BlendMode::kExclusion) ==
                  FPDF_BLENDMODE_Exclusion,
              "BlendMode::kExclusion value mismatch");
static_assert(static_cast<int>(BlendMode::kHue) == FPDF_BLENDMODE_Hue,
              "BlendMode::kHue value mismatch");
static_assert(static_cast<int>(BlendMode::kSaturation) ==
                  FPDF_BLENDMODE_Saturation,
              "BlendMode::kSaturation value mismatch");
static_assert(static_cast<int>(BlendMode::kColor) == FPDF_BLENDMODE_Color,
              "BlendMode::kColor value mismatch");
static_assert(static_cast<int>(BlendMode::kLuminosity) ==
                  FPDF_BLENDMODE_Luminosity,
              "BlendMode::kLuminosity value mismatch");

// These checks ensure the consistency of line ending values across core/ and
// public.
static_assert(static_cast<int>(CPDF_Annot::LineEnding::kNone) ==
                  FPDF_ANNOT_LE_None,
              "LineEnding::kNone mismatch");
static_assert(static_cast<int>(CPDF_Annot::LineEnding::kSquare) ==
                  FPDF_ANNOT_LE_Square,
              "LineEnding::kSquare mismatch");
static_assert(static_cast<int>(CPDF_Annot::LineEnding::kCircle) ==
                  FPDF_ANNOT_LE_Circle,
              "LineEnding::kCircle mismatch");
static_assert(static_cast<int>(CPDF_Annot::LineEnding::kDiamond) ==
                  FPDF_ANNOT_LE_Diamond,
              "LineEnding::kDiamond mismatch");
static_assert(static_cast<int>(CPDF_Annot::LineEnding::kOpenArrow) ==
                  FPDF_ANNOT_LE_OpenArrow,
              "LineEnding::kOpenArrow mismatch");
static_assert(static_cast<int>(CPDF_Annot::LineEnding::kClosedArrow) ==
                  FPDF_ANNOT_LE_ClosedArrow,
              "LineEnding::kClosedArrow mismatch");
static_assert(static_cast<int>(CPDF_Annot::LineEnding::kButt) ==
                  FPDF_ANNOT_LE_Butt,
              "LineEnding::kButt mismatch");
static_assert(static_cast<int>(CPDF_Annot::LineEnding::kROpenArrow) ==
                  FPDF_ANNOT_LE_ROpenArrow,
              "LineEnding::kROpenArrow mismatch");
static_assert(static_cast<int>(CPDF_Annot::LineEnding::kRClosedArrow) ==
                  FPDF_ANNOT_LE_RClosedArrow,
              "LineEnding::kRClosedArrow mismatch");
static_assert(static_cast<int>(CPDF_Annot::LineEnding::kSlash) ==
                  FPDF_ANNOT_LE_Slash,
              "LineEnding::kSlash mismatch");
static_assert(static_cast<int>(CPDF_Annot::LineEnding::kUnknown) ==
                  FPDF_ANNOT_LE_Unknown,
              "LineEnding::kUnknown mismatch");

// These checks ensure the consistency of standard font values across core/ and
// public.
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kCourier) ==
                  FPDF_FONT_COURIER,
              "CPDF_Annot::StandardFont::kCourier mismatch");
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kCourier_Bold) ==
                  FPDF_FONT_COURIER_BOLD,
              "CPDF_Annot::StandardFont::kCourier_Bold mismatch");
static_assert(
    static_cast<int>(CPDF_Annot::StandardFont::kCourier_BoldOblique) ==
        FPDF_FONT_COURIER_BOLDITALIC,
    "CPDF_Annot::StandardFont::kCourier_BoldOblique mismatch");
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kCourier_Oblique) ==
                  FPDF_FONT_COURIER_ITALIC,
              "CPDF_Annot::StandardFont::kCourier_Oblique mismatch");
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kHelvetica) ==
                  FPDF_FONT_HELVETICA,
              "CPDF_Annot::StandardFont::kHelvetica mismatch");
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kHelvetica_Bold) ==
                  FPDF_FONT_HELVETICA_BOLD,
              "CPDF_Annot::StandardFont::kHelvetica_Bold mismatch");
static_assert(
    static_cast<int>(CPDF_Annot::StandardFont::kHelvetica_BoldOblique) ==
        FPDF_FONT_HELVETICA_BOLDITALIC,
    "CPDF_Annot::StandardFont::kHelvetica_BoldOblique mismatch");
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kHelvetica_Oblique) ==
                  FPDF_FONT_HELVETICA_ITALIC,
              "CPDF_Annot::StandardFont::kHelvetica_Oblique mismatch");
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kTimes_Roman) ==
                  FPDF_FONT_TIMES_ROMAN,
              "CPDF_Annot::StandardFont::kTimes_Roman mismatch");
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kTimes_Bold) ==
                  FPDF_FONT_TIMES_BOLD,
              "CPDF_Annot::StandardFont::kTimes_Bold mismatch");
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kTimes_BoldItalic) ==
                  FPDF_FONT_TIMES_BOLDITALIC,
              "CPDF_Annot::StandardFont::kTimes_BoldItalic mismatch");
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kTimes_Italic) ==
                  FPDF_FONT_TIMES_ITALIC,
              "CPDF_Annot::StandardFont::kTimes_Italic mismatch");
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kSymbol) ==
                  FPDF_FONT_SYMBOL,
              "CPDF_Annot::StandardFont::kSymbol mismatch");
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kZapfDingbats) ==
                  FPDF_FONT_ZAPFDINGBATS,
              "CPDF_Annot::StandardFont::kZapfDingbats mismatch");
static_assert(static_cast<int>(CPDF_Annot::StandardFont::kUnknown) ==
                  FPDF_FONT_UNKNOWN,
              "CPDF_Annot::StandardFont::kUnknown mismatch");

// These checks ensure consistency between the public API and internal enums.
static_assert(static_cast<int>(CPDF_Annot::TextAlignment::kLeft) ==
                  FPDF_TEXT_ALIGNMENT_LEFT,
              "CPDF_Annot::TextAlignment::kLeft mismatch");
static_assert(static_cast<int>(CPDF_Annot::TextAlignment::kCenter) ==
                  FPDF_TEXT_ALIGNMENT_CENTER,
              "CPDF_Annot::TextAlignment::kCenter mismatch");
static_assert(static_cast<int>(CPDF_Annot::TextAlignment::kRight) ==
                  FPDF_TEXT_ALIGNMENT_RIGHT,
              "CPDF_Annot::TextAlignment::kRight mismatch");

// These checks ensure the consistency of vertical alignment values across core/
// and public.
static_assert(static_cast<int>(CPDF_Annot::VerticalAlignment::kTop) ==
                  FPDF_VERTICAL_ALIGNMENT_TOP,
              "CPDF_Annot::VerticalAlignment::kTop mismatch");
static_assert(static_cast<int>(CPDF_Annot::VerticalAlignment::kMiddle) ==
                  FPDF_VERTICAL_ALIGNMENT_MIDDLE,
              "CPDF_Annot::VerticalAlignment::kMiddle mismatch");
static_assert(static_cast<int>(CPDF_Annot::VerticalAlignment::kBottom) ==
                  FPDF_VERTICAL_ALIGNMENT_BOTTOM,
              "CPDF_Annot::VerticalAlignment::kBottom mismatch");

// These checks ensure the consistency of reply type values across core/ and
// public.
static_assert(static_cast<int>(CPDF_Annot::ReplyType::kUnknown) ==
                  FPDF_ANNOT_RT_UNKNOWN,
              "ReplyType::kUnknown mismatch");
static_assert(static_cast<int>(CPDF_Annot::ReplyType::kReply) ==
                  FPDF_ANNOT_RT_REPLY,
              "ReplyType::kReply mismatch");
static_assert(static_cast<int>(CPDF_Annot::ReplyType::kGroup) ==
                  FPDF_ANNOT_RT_GROUP,
              "ReplyType::kGroup mismatch");

class RawAnnotContext final : public CPDF_AnnotContext {
 public:
  // Takes ownership of |unparsed_page| by value (RetainPtr).
  RawAnnotContext(RetainPtr<CPDF_Dictionary> dict,
                  RetainPtr<CPDF_Page> unparsed_page,
                  int annot_index)
      : CPDF_AnnotContext(dict, unparsed_page.Get(), annot_index),
        owned_page_(std::move(unparsed_page)) {}

 private:
  // Keeps the page alive as long as the annot context lives.
  const RetainPtr<CPDF_Page> owned_page_;
};

// Checks if an annotation subtype can have a /Name entry.
bool IsNameSubtype(FPDF_ANNOTATION_SUBTYPE subtype) {
  return subtype == FPDF_ANNOT_TEXT || subtype == FPDF_ANNOT_FILEATTACHMENT ||
         subtype == FPDF_ANNOT_SOUND || subtype == FPDF_ANNOT_STAMP;
}

bool HasAPStream(const CPDF_Dictionary* pAnnotDict) {
  return !!GetAnnotAP(pAnnotDict, CPDF_Annot::AppearanceMode::kNormal);
}

void UpdateContentStream(CPDF_Form* pForm, CPDF_Stream* pStream) {
  DCHECK(pForm);
  DCHECK(pStream);

  CPDF_PageContentGenerator generator(pForm);
  fxcrt::ostringstream buf;
  generator.ProcessPageObjects(&buf);
  pStream->SetDataFromStringstreamAndRemoveFilter(&buf);
}

void SetQuadPointsAtIndex(CPDF_Array* array,
                          size_t quad_index,
                          const FS_QUADPOINTSF* quad_points) {
  DCHECK(array);
  DCHECK(quad_points);
  DCHECK(IsValidQuadPointsIndex(array, quad_index));

  size_t nIndex = quad_index * 8;
  array->SetNewAt<CPDF_Number>(nIndex, quad_points->x1);
  array->SetNewAt<CPDF_Number>(++nIndex, quad_points->y1);
  array->SetNewAt<CPDF_Number>(++nIndex, quad_points->x2);
  array->SetNewAt<CPDF_Number>(++nIndex, quad_points->y2);
  array->SetNewAt<CPDF_Number>(++nIndex, quad_points->x3);
  array->SetNewAt<CPDF_Number>(++nIndex, quad_points->y3);
  array->SetNewAt<CPDF_Number>(++nIndex, quad_points->x4);
  array->SetNewAt<CPDF_Number>(++nIndex, quad_points->y4);
}

void AppendQuadPoints(CPDF_Array* array, const FS_QUADPOINTSF* quad_points) {
  DCHECK(quad_points);
  DCHECK(array);

  array->AppendNew<CPDF_Number>(quad_points->x1);
  array->AppendNew<CPDF_Number>(quad_points->y1);
  array->AppendNew<CPDF_Number>(quad_points->x2);
  array->AppendNew<CPDF_Number>(quad_points->y2);
  array->AppendNew<CPDF_Number>(quad_points->x3);
  array->AppendNew<CPDF_Number>(quad_points->y3);
  array->AppendNew<CPDF_Number>(quad_points->x4);
  array->AppendNew<CPDF_Number>(quad_points->y4);
}

void UpdateBBox(CPDF_Dictionary* annot_dict) {
  DCHECK(annot_dict);
  // Update BBox entry in appearance stream based on the bounding rectangle
  // of the annotation's quadpoints.
  RetainPtr<CPDF_Stream> pStream =
      GetAnnotAP(annot_dict, CPDF_Annot::AppearanceMode::kNormal);
  if (pStream) {
    CFX_FloatRect boundingRect =
        CPDF_Annot::BoundingRectFromQuadPoints(annot_dict);
    if (boundingRect.Contains(pStream->GetDict()->GetRectFor("BBox"))) {
      pStream->GetMutableDict()->SetRectFor("BBox", boundingRect);
    }
  }
}

BlendMode GetEffectiveAnnotBlendMode(CPDF_AnnotContext* ctx) {
  if (!ctx) {
    return BlendMode::kNormal;
  }

  CPDF_DocumentViewScope document_view(ctx->GetPage()->GetDocument());
  const CPDF_Dictionary* annot_dict = ctx->GetAnnotDict();
  if (!annot_dict) {
    return BlendMode::kNormal;
  }

  // Get (or detect absence of) normal appearance stream.
  RetainPtr<CPDF_Stream> ap_stream =
      GetAnnotAP(annot_dict, CPDF_Annot::AppearanceMode::kNormal);
  if (!ap_stream) {
    // Heuristic: highlight annotations without AP are effectively Multiply.
    const CPDF_Annot::Subtype subtype = CPDF_Annot::StringToAnnotSubtype(
        annot_dict->GetNameFor(pdfium::annotation::kSubtype));
    if (subtype == CPDF_Annot::Subtype::HIGHLIGHT) {
      return BlendMode::kMultiply;
    }
    return BlendMode::kNormal;
  }

  // Ensure form is parsed.
  if (!ctx->HasForm()) {
    ctx->SetForm(ap_stream);
  }

  CPDF_Form* form = ctx->GetForm();
  if (!form) {
    return BlendMode::kNormal;
  }

  // Iterate objects in creation order; pick first non-Normal encountered.
  for (const auto& obj : *form) {
    if (!obj) {
      continue;
    }
    const CPDF_GeneralState& gs = obj->general_state();
    BlendMode bm = gs.GetBlendType();
    if (bm != BlendMode::kNormal) {
      return bm;
    }
  }
  return BlendMode::kNormal;
}

const CPDF_Dictionary* GetAnnotDictFromFPDFAnnotation(
    const FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  return context ? context->GetAnnotDict() : nullptr;
}

RetainPtr<CPDF_Dictionary> GetMutableAnnotDictFromFPDFAnnotation(
    FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  return context ? context->GetMutableAnnotDict() : nullptr;
}

constexpr char kEmbedMetadataKey[] = "EMBD_Metadata";
constexpr char kEmbedMetadataCustomJSONKey[] = "CustomJSON";

RetainPtr<const CPDF_Dictionary> GetEmbedMetadataDict(
    const CPDF_Dictionary* annot_dict) {
  return annot_dict ? annot_dict->GetDictFor(kEmbedMetadataKey) : nullptr;
}

RetainPtr<CPDF_Dictionary> GetOrCreateEmbedMetadataDict(
    RetainPtr<CPDF_Dictionary> annot_dict) {
  if (!annot_dict) {
    return nullptr;
  }

  RetainPtr<CPDF_Dictionary> metadata =
      annot_dict->GetMutableDictFor(kEmbedMetadataKey);
  if (!metadata) {
    metadata = annot_dict->SetNewFor<CPDF_Dictionary>(kEmbedMetadataKey);
  }
  return metadata;
}

bool EmbedMetadataIsEmpty(const CPDF_Dictionary* metadata) {
  return !metadata || metadata->size() == 0;
}

void RemoveEmbedMetadataIfEmpty(RetainPtr<CPDF_Dictionary> annot_dict) {
  RetainPtr<const CPDF_Dictionary> metadata =
      GetEmbedMetadataDict(annot_dict.Get());
  if (EmbedMetadataIsEmpty(metadata.Get())) {
    annot_dict->RemoveFor(kEmbedMetadataKey);
  }
}

float GetEmbedMetadataFloatFor(const CPDF_Dictionary* annot_dict,
                               ByteStringView key) {
  RetainPtr<const CPDF_Dictionary> metadata = GetEmbedMetadataDict(annot_dict);
  return metadata ? metadata->GetFloatFor(key) : 0.0f;
}

CFX_FloatRect GetEmbedMetadataRectFor(const CPDF_Dictionary* annot_dict,
                                      ByteStringView key) {
  RetainPtr<const CPDF_Dictionary> metadata = GetEmbedMetadataDict(annot_dict);
  return metadata ? metadata->GetRectFor(key) : CFX_FloatRect();
}

static uint32_t EnsureIndirect(CPDF_Document* doc,
                               RetainPtr<CPDF_Dictionary> dict) {
  uint32_t objnum = dict->GetObjNum();
  if (objnum == 0) {
    objnum = doc->AddIndirectObject(dict);
  }
  return objnum;
}

RetainPtr<CPDF_Dictionary> SetExtGStateInResourceDict(
    CPDF_Document* doc,
    const CPDF_Dictionary* pAnnotDict,
    const ByteString& sBlendMode) {
  auto pGSDict =
      pdfium::MakeRetain<CPDF_Dictionary>(pAnnotDict->GetByteStringPool());

  // ExtGState represents a graphics state parameter dictionary.
  pGSDict->SetNewFor<CPDF_Name>("Type", "ExtGState");

  // CA respresents current stroking alpha specifying constant opacity
  // value that should be used in transparent imaging model.
  float fOpacity = pAnnotDict->GetFloatFor("CA");

  pGSDict->SetNewFor<CPDF_Number>("CA", fOpacity);

  // ca represents fill color alpha specifying constant opacity
  // value that should be used in transparent imaging model.
  pGSDict->SetNewFor<CPDF_Number>("ca", fOpacity);

  // AIS represents alpha source flag specifying whether current alpha
  // constant shall be interpreted as shape value (true) or opacity value
  // (false).
  pGSDict->SetNewFor<CPDF_Boolean>("AIS", false);

  // BM represents Blend Mode
  pGSDict->SetNewFor<CPDF_Name>("BM", sBlendMode);

  auto pExtGStateDict =
      pdfium::MakeRetain<CPDF_Dictionary>(pAnnotDict->GetByteStringPool());

  pExtGStateDict->SetFor("GS", pGSDict);

  auto pResourceDict = doc->New<CPDF_Dictionary>();
  pResourceDict->SetFor("ExtGState", pExtGStateDict);
  return pResourceDict;
}

CPDF_FormField* GetFormField(FPDF_FORMHANDLE hHandle, FPDF_ANNOTATION annot) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return nullptr;
  }

  CPDFSDK_InteractiveForm* pForm = FormHandleToInteractiveForm(hHandle);
  if (!pForm) {
    return nullptr;
  }

  CPDF_InteractiveForm* pPDFForm = pForm->GetInteractiveForm();
  return pPDFForm->GetFieldByDict(pAnnotDict);
}

// If `allowed_types` is empty, then match all types.
const CPDFSDK_Widget* GetWidgetOfTypes(
    FPDF_FORMHANDLE hHandle,
    FPDF_ANNOTATION annot,
    pdfium::span<const CPDF_FormField::Type> allowed_types) {
  const CPDF_Dictionary* annot_dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict) {
    return nullptr;
  }

  CPDFSDK_InteractiveForm* form = FormHandleToInteractiveForm(hHandle);
  if (!form) {
    return nullptr;
  }

  CPDF_InteractiveForm* pdf_form = form->GetInteractiveForm();
  CPDF_FormField* form_field = pdf_form->GetFieldByDict(annot_dict);
  if (!form_field) {
    return nullptr;
  }

  if (!allowed_types.empty()) {
    if (!pdfium::Contains(allowed_types, form_field->GetType())) {
      return nullptr;
    }
  }

  CPDF_FormControl* form_control = pdf_form->GetControlByDict(annot_dict);
  return form_control ? form->GetWidget(form_control) : nullptr;
}

const CPDFSDK_Widget* GetRadioButtonOrCheckBoxWidget(FPDF_FORMHANDLE handle,
                                                     FPDF_ANNOTATION annot) {
  static constexpr std::array<CPDF_FormField::Type, 2> kAllowedTypes = {
      CPDF_FormField::kCheckBox, CPDF_FormField::kRadioButton};
  return GetWidgetOfTypes(handle, annot, kAllowedTypes);
}

RetainPtr<const CPDF_Array> GetInkList(FPDF_ANNOTATION annot) {
  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_INK) {
    return nullptr;
  }

  const CPDF_Dictionary* annot_dict = GetAnnotDictFromFPDFAnnotation(annot);
  return annot_dict ? annot_dict->GetArrayFor(pdfium::annotation::kInkList)
                    : nullptr;
}

std::optional<CFX_Color::TypeAndARGB> GetFreetextFontColor(
    FPDF_FORMHANDLE handle,
    FPDF_ANNOTATION annot) {
  const CPDF_Dictionary* annot_dict = GetAnnotDictFromFPDFAnnotation(annot);
  CHECK(annot_dict);  // Has to be true to determine `annot` is for Freetext.

  CPDFSDK_InteractiveForm* form = FormHandleToInteractiveForm(handle);
  CPDF_Document* doc = form ? form->GetInteractiveForm()->document() : nullptr;
  const CPDF_Dictionary* root_dict = doc ? doc->GetRoot() : nullptr;
  RetainPtr<const CPDF_Dictionary> acroform_dict =
      root_dict ? root_dict->GetDictFor("AcroForm") : nullptr;
  CPDF_DefaultAppearance default_appearance(annot_dict, acroform_dict);
  std::optional<CFX_Color::TypeAndARGB> color =
      default_appearance.GetColorARGB();
  if (color.has_value()) {
    return color;
  }
  return CFX_Color::TypeAndARGB(CFX_Color::Type::kGray,
                                ArgbEncode(255, 0, 0, 0));
}

std::optional<FX_COLORREF> GetWidgetFontColor(FPDF_FORMHANDLE handle,
                                              FPDF_ANNOTATION annot) {
  const CPDFSDK_Widget* widget = GetWidgetOfTypes(handle, annot, {});
  return widget ? widget->GetTextColor() : std::nullopt;
}

enum class EPDFStampFitCpp { kContain = 0, kCover = 1, kStretch = 2 };

inline EPDFStampFitCpp ToCpp(EPDF_STAMP_FIT v) {
  switch (v) {
    case EPDF_STAMP_FIT_COVER:
      return EPDFStampFitCpp::kCover;
    case EPDF_STAMP_FIT_STRETCH:
      return EPDFStampFitCpp::kStretch;
    case EPDF_STAMP_FIT_CONTAIN:
      return EPDFStampFitCpp::kContain;
  }
  return EPDFStampFitCpp::kContain;
}

static bool FitImageIntoBox(float box_w,
                            float box_h,
                            float img_w,
                            float img_h,
                            EPDFStampFitCpp fit,
                            float* out_drawn_w,
                            float* out_drawn_h,
                            float* out_dx,
                            float* out_dy) {
  if (box_w <= 0 || box_h <= 0 || img_w <= 0 || img_h <= 0) {
    return false;
  }

  const float sx = box_w / img_w;
  const float sy = box_h / img_h;

  // Default to kStretch semantics; this also serves as a safe fallback if a
  // future enumerator is added without updating this switch.
  float scale_x = sx;
  float scale_y = sy;
  switch (fit) {
    case EPDFStampFitCpp::kContain: {
      float s = std::min(sx, sy);
      scale_x = scale_y = s;
      break;
    }
    case EPDFStampFitCpp::kCover: {
      float s = std::max(sx, sy);
      scale_x = scale_y = s;
      break;
    }
    case EPDFStampFitCpp::kStretch:
      break;
  }

  *out_drawn_w = img_w * scale_x;
  *out_drawn_h = img_h * scale_y;
  *out_dx = (box_w - *out_drawn_w) * 0.5f;
  *out_dy = (box_h - *out_drawn_h) * 0.5f;
  return true;
}

// Returns the bounding rect of the actual painted page objects inside a Form
// XObject.  CPDF_Form::ParseContent() (called with no parent matrix) reads the
// stream's /Matrix and folds it into the CTM during content parsing, so
// CalcBoundingBox() already returns bounds in the post-Matrix display space.
// We therefore must NOT apply the Matrix again here.
// Falls back to the raw /BBox if the form has no parseable page objects.
static CFX_FloatRect GetPaintedFormBounds(CPDF_Document* doc,
                                          CPDF_Stream* stream) {
  if (!doc || !stream) {
    return CFX_FloatRect();
  }

  RetainPtr<CPDF_Dictionary> stream_dict = stream->GetMutableDict();
  if (!stream_dict) {
    return CFX_FloatRect();
  }

  auto form = std::make_unique<CPDF_Form>(
      doc, stream_dict->GetMutableDictFor("Resources"),
      pdfium::WrapRetain(stream));
  form->ParseContent();

  CFX_FloatRect bounds = form->CalcBoundingBox();
  bounds.Normalize();
  if (bounds.IsEmpty()) {
    bounds = stream_dict->GetRectFor("BBox");
    bounds.Normalize();
  }
  if (bounds.IsEmpty()) {
    return CFX_FloatRect();
  }
  return bounds;
}

// Fallback for when GetPaintedFormBounds returns empty (e.g. the form stream
// has no parseable page objects, or all objects are outside the BBox clip).
// Computes the display box by applying the stream's /Matrix to its /BBox.
static CFX_FloatRect GetFormDisplayBox(const CPDF_Dictionary* stream_dict) {
  if (!stream_dict) {
    return CFX_FloatRect();
  }

  CFX_FloatRect bbox = stream_dict->GetRectFor("BBox");
  bbox.Normalize();
  if (bbox.IsEmpty()) {
    return CFX_FloatRect();
  }

  CFX_Matrix matrix = stream_dict->GetMatrixFor("Matrix");
  if (!matrix.IsIdentity()) {
    bbox = matrix.TransformRect(bbox);
    bbox.Normalize();
  }
  return bbox;
}

// `resources` without the named graphics state. The /ExtGState dictionary is
// copied first: it may be shared with other appearances.
static void RemoveExtGState(CPDF_Dictionary* resources,
                            const ByteString& name) {
  RetainPtr<const CPDF_Dictionary> states = resources->GetDictFor("ExtGState");
  if (!states) {
    return;
  }
  RetainPtr<CPDF_Dictionary> copy = ToDictionary(states->Clone());
  copy->RemoveFor(name.AsStringView());
  if (copy->size() == 0) {
    resources->RemoveFor("ExtGState");
  } else {
    resources->SetFor("ExtGState", std::move(copy));
  }
}

// Detach the reference path as well as the stream: /AP and /AP/N state
// dictionaries may themselves be indirect objects shared by annotations.
static void SetDetachedNormalAppearance(CPDF_Document* doc,
                                        CPDF_Dictionary* annot,
                                        RetainPtr<CPDF_Stream> stream) {
  RetainPtr<const CPDF_Dictionary> original_ap = annot->GetDictFor("AP");
  RetainPtr<CPDF_Dictionary> ap = ToDictionary(original_ap->Clone());
  RetainPtr<const CPDF_Dictionary> original_states =
      ToDictionary(original_ap->GetDirectObjectFor("N"));
  doc->AddIndirectObject(stream);
  if (original_states) {
    // Match GetAnnotAPInternal's selection, including widgets without /AS.
    ByteString state = annot->GetByteStringFor("AS");
    if (state.IsEmpty()) {
      ByteString value = annot->GetByteStringFor("V");
      if (value.IsEmpty()) {
        RetainPtr<const CPDF_Dictionary> parent = annot->GetDictFor("Parent");
        value = parent ? parent->GetByteStringFor("V") : ByteString();
      }
      state =
          !value.IsEmpty() && original_states->KeyExist(value.AsStringView())
              ? value
              : "Off";
    }
    RetainPtr<CPDF_Dictionary> states = ToDictionary(original_states->Clone());
    states->SetNewFor<CPDF_Reference>(state, doc, stream->GetObjNum());
    ap->SetFor("N", std::move(states));
  } else {
    ap->SetNewFor<CPDF_Reference>("N", doc, stream->GetObjNum());
  }
  annot->SetFor("AP", std::move(ap));
}

// Wraps the current AP stream content into a child Form XObject stored under
// Resources/XObject/EPDFWRAP, so the outer AP content can be a simple
// "q ... cm /EPDFWRAP Do Q" that handles all scaling.  Returns false on
// failure; on success the caller must write the new wrapper content stream.
// A layer that only repeats the annotation's `opacity` stays behind: our own
// layer paints /CA over the wrapper, so keeping it would paint it twice.
static bool WrapAPContentIntoFormXObject(CPDF_Stream* ap,
                                         CPDF_Document* doc,
                                         float opacity) {
  const std::optional<EpdfOpacityLayer> layer =
      EpdfFindOpacityLayer(ap, opacity);

  RetainPtr<CPDF_Dictionary> ap_dict = ap->GetMutableDict();
  if (!ap_dict) {
    return false;
  }

  // Build the child Form XObject dictionary.
  auto child_dict = doc->New<CPDF_Dictionary>();
  child_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kType, "XObject");
  child_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype, "Form");

  // Preserve the original child form geometry exactly as imported/authored.
  // The parent wrapper will align the painted bounds via its cm matrix.
  // Fallback to Matrix-transformed BBox if the raw BBox is missing/empty.
  CFX_FloatRect bbox = ap_dict->GetRectFor("BBox");
  bbox.Normalize();
  if (bbox.IsEmpty()) {
    bbox = GetFormDisplayBox(ap_dict.Get());
  }
  if (bbox.IsEmpty()) {
    return false;
  }
  child_dict->SetRectFor("BBox", bbox);

  CFX_Matrix child_matrix = ap_dict->GetMatrixFor("Matrix");
  if (!child_matrix.IsIdentity()) {
    child_dict->SetMatrixFor("Matrix", child_matrix);
  } else {
    child_dict->RemoveFor("Matrix");
  }

  // Every other entry belongs to the drawing, not to how we place it: its
  // transparency group, optional content (/OC), metadata. It moves with the
  // content, so the drawing a copy is made from draws the same.
  std::vector<ByteString> drawing_keys;
  {
    CPDF_DictionaryLocker locker(ap_dict.Get());
    for (const auto& entry : locker) {
      const ByteString& key = entry.first;
      if (key != "Type" && key != "Subtype" && key != "BBox" &&
          key != "Matrix" && key != "Resources" && key != "Length" &&
          key != "Filter" && key != "DecodeParms" && key != "DL" &&
          key != "EPDFOrigContentRect") {
        drawing_keys.push_back(key);
      }
    }
  }
  for (const ByteString& key : drawing_keys) {
    child_dict->SetFor(key, ap_dict->RemoveFor(key.AsStringView()));
  }

  // Move Resources to the child (avoids deep-clone cost).
  RetainPtr<CPDF_Dictionary> res = ap_dict->GetMutableDictFor("Resources");
  if (res) {
    RetainPtr<CPDF_Dictionary> child_res = ToDictionary(res->Clone());
    if (layer) {
      RemoveExtGState(child_res.Get(), layer->state);
    }
    child_dict->SetFor("Resources", std::move(child_res));
    ap_dict->RemoveFor("Resources");
  }

  // Create the child stream with the original AP content bytes, or only the
  // drawing when the content was an opacity layer around it.
  DataVector<uint8_t> content_bytes;
  if (layer) {
    const ByteString drawing = layer->form_token + " Do";
    content_bytes.assign(drawing.unsigned_span().begin(),
                         drawing.unsigned_span().end());
  } else {
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(ap));
    acc->LoadAllDataFiltered();
    auto span = acc->GetSpan();
    content_bytes.assign(span.begin(), span.end());
  }

  auto child_stream = doc->NewIndirect<CPDF_Stream>(std::move(child_dict));
  child_stream->SetData(content_bytes);

  // Store child as Resources/XObject/EPDFWRAP on the AP stream.
  RetainPtr<CPDF_Dictionary> new_res =
      ap_dict->SetNewFor<CPDF_Dictionary>("Resources");
  RetainPtr<CPDF_Dictionary> xobj =
      new_res->SetNewFor<CPDF_Dictionary>("XObject");
  xobj->SetNewFor<CPDF_Reference>("EPDFWRAP", doc, child_stream->GetObjNum());
  return true;
}

CPDF_Annot::StandardFont ResolveStandardFontFromDefaultResources(
    const CPDF_Dictionary* dr_dict,
    const ByteString& resource_name) {
  if (!dr_dict || resource_name.IsEmpty()) {
    return CPDF_Annot::StandardFont::kUnknown;
  }

  RetainPtr<const CPDF_Dictionary> font_resources = dr_dict->GetDictFor("Font");
  RetainPtr<const CPDF_Dictionary> font_dict =
      font_resources ? font_resources->GetDictFor(resource_name.AsStringView())
                     : nullptr;
  if (!font_dict) {
    return CPDF_Annot::StandardFont::kUnknown;
  }

  return CPDF_Annot::StringToStandardFont(font_dict->GetNameFor("BaseFont"));
}
}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_IsSupportedSubtype(FPDF_ANNOTATION_SUBTYPE subtype) {
  // The supported subtypes must also be communicated in the user doc.
  switch (subtype) {
    case FPDF_ANNOT_CIRCLE:
    case FPDF_ANNOT_FILEATTACHMENT:
    case FPDF_ANNOT_FREETEXT:
    case FPDF_ANNOT_HIGHLIGHT:
    case FPDF_ANNOT_INK:
    case FPDF_ANNOT_LINK:
    case FPDF_ANNOT_POPUP:
    case FPDF_ANNOT_REDACT:
    case FPDF_ANNOT_SQUARE:
    case FPDF_ANNOT_SQUIGGLY:
    case FPDF_ANNOT_STAMP:
    case FPDF_ANNOT_STRIKEOUT:
    case FPDF_ANNOT_TEXT:
    case FPDF_ANNOT_UNDERLINE:
    case FPDF_ANNOT_POLYGON:
    case FPDF_ANNOT_POLYLINE:
    case FPDF_ANNOT_LINE:
    case FPDF_ANNOT_CARET:
    // EmbedPDF: widgets are born through the annotation API and adopted by a
    // form field via EPDFForm_AttachWidget (public/epdf_form.h). An
    // unattached widget is an ordinary, inert annotation.
    case FPDF_ANNOT_WIDGET:
      return true;
    default:
      return false;
  }
}

FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDFPage_CreateAnnot(FPDF_PAGE page, FPDF_ANNOTATION_SUBTYPE subtype) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage || !FPDFAnnot_IsSupportedSubtype(subtype)) {
    return nullptr;
  }

  auto dict = pPage->GetDocument()->New<CPDF_Dictionary>();
  dict->SetNewFor<CPDF_Name>(pdfium::annotation::kType, "Annot");
  dict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype,
                             CPDF_Annot::AnnotSubtypeToString(
                                 static_cast<CPDF_Annot::Subtype>(subtype)));
  auto pNewAnnot =
      std::make_unique<CPDF_AnnotContext>(dict, IPDFPageFromFPDFPage(page));

  RetainPtr<CPDF_Array> pAnnotList = pPage->GetOrCreateAnnotsArray();
  pAnnotList->Append(dict);

  // Caller takes ownership.
  return FPDFAnnotationFromCPDFAnnotContext(pNewAnnot.release());
}

FPDF_EXPORT int FPDF_CALLCONV FPDFPage_GetAnnotCount(FPDF_PAGE page) {
  const CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return 0;
  }

  RetainPtr<const CPDF_Array> pAnnots = pPage->GetAnnotsArray();
  return pAnnots ? fxcrt::CollectionSize<int>(*pAnnots) : 0;
}

FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV FPDFPage_GetAnnot(FPDF_PAGE page,
                                                            int index) {
  ScopedFPDFPageView page_view(page);
  if (!page_view || index < 0) {
    return nullptr;
  }
  const CPDF_Page* pPage = page_view.Get();

  RetainPtr<const CPDF_Array> pAnnots = pPage->GetAnnotsArray();
  if (!pAnnots || static_cast<size_t>(index) >= pAnnots->size()) {
    return nullptr;
  }

  RetainPtr<const CPDF_Dictionary> const_dict =
      ToDictionary(pAnnots->GetDirectObjectAt(index));
  RetainPtr<CPDF_Dictionary> dict =
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(const_dict.Get()));
  if (!dict) {
    return nullptr;
  }

  auto pNewAnnot = std::make_unique<CPDF_AnnotContext>(
      std::move(dict), IPDFPageFromFPDFPage(page), index);

  // Caller takes ownership.
  return FPDFAnnotationFromCPDFAnnotContext(pNewAnnot.release());
}

FPDF_EXPORT int FPDF_CALLCONV FPDFPage_GetAnnotIndex(FPDF_PAGE page,
                                                     FPDF_ANNOTATION annot) {
  ScopedFPDFPageView page_view(page);
  if (!page_view) {
    return -1;
  }
  const CPDF_Page* pPage = page_view.Get();

  ScopedFPDFAnnotationView annot_view(annot);
  if (!annot_view) {
    return -1;
  }
  const CPDF_Dictionary* pAnnotDict = annot_view.Get()->GetAnnotDict();

  RetainPtr<const CPDF_Array> pAnnots = pPage->GetAnnotsArray();
  if (!pAnnots) {
    return -1;
  }

  CPDF_ArrayLocker locker(pAnnots);
  auto it = std::ranges::find_if(
      locker, [pAnnotDict](const RetainPtr<CPDF_Object>& candidate) {
        return candidate->GetDirect() == pAnnotDict;
      });

  if (it == locker.end()) {
    return -1;
  }

  return pdfium::checked_cast<int>(it - locker.begin());
}

FPDF_EXPORT void FPDF_CALLCONV FPDFPage_CloseAnnot(FPDF_ANNOTATION annot) {
  delete CPDFAnnotContextFromFPDFAnnotation(annot);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFPage_RemoveAnnot(FPDF_PAGE page,
                                                         int index) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage || index < 0) {
    return false;
  }

  RetainPtr<CPDF_Array> pAnnots = pPage->GetMutableAnnotsArray();
  if (!pAnnots || static_cast<size_t>(index) >= pAnnots->size()) {
    return false;
  }

  pAnnots->RemoveAt(index);
  return true;
}

FPDF_EXPORT FPDF_ANNOTATION_SUBTYPE FPDF_CALLCONV
FPDFAnnot_GetSubtype(FPDF_ANNOTATION annot) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return FPDF_ANNOT_UNKNOWN;
  }

  return static_cast<FPDF_ANNOTATION_SUBTYPE>(CPDF_Annot::StringToAnnotSubtype(
      pAnnotDict->GetNameFor(pdfium::annotation::kSubtype)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_IsObjectSupportedSubtype(FPDF_ANNOTATION_SUBTYPE subtype) {
  // The supported subtypes must also be communicated in the user doc.
  return subtype == FPDF_ANNOT_INK || subtype == FPDF_ANNOT_STAMP;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_UpdateObject(FPDF_ANNOTATION annot, FPDF_PAGEOBJECT obj) {
  CPDF_AnnotContext* pAnnot = CPDFAnnotContextFromFPDFAnnotation(annot);
  CPDF_PageObject* pObj = CPDFPageObjectFromFPDFPageObject(obj);
  if (!pAnnot || !pAnnot->HasForm() || !pObj) {
    return false;
  }

  // Check that the annotation type is supported by this method.
  if (!FPDFAnnot_IsObjectSupportedSubtype(FPDFAnnot_GetSubtype(annot))) {
    return false;
  }

  // Check that the annotation already has an appearance stream, since an
  // existing object is to be updated.
  RetainPtr<CPDF_Dictionary> pAnnotDict = pAnnot->GetMutableAnnotDict();
  RetainPtr<CPDF_Stream> pStream =
      GetAnnotAP(pAnnotDict.Get(), CPDF_Annot::AppearanceMode::kNormal);
  if (!pStream) {
    return false;
  }

  // Check that the object is already in this annotation's object list.
  CPDF_Form* pForm = pAnnot->GetForm();
  if (std::ranges::find_if(*pForm, pdfium::MatchesUniquePtr(pObj)) ==
      pForm->end()) {
    return false;
  }

  // Update the content stream data in the annotation's AP stream.
  UpdateContentStream(pForm, pStream.Get());
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV FPDFAnnot_AddInkStroke(FPDF_ANNOTATION annot,
                                                     const FS_POINTF* points,
                                                     size_t point_count) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_INK || !points ||
      point_count == 0 ||
      !pdfium::IsValueInRangeForNumericType<int32_t>(point_count)) {
    return -1;
  }

  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  RetainPtr<CPDF_Array> inklist = annot_dict->GetOrCreateArrayFor("InkList");
  FX_SAFE_SIZE_T safe_ink_size = inklist->size();
  safe_ink_size += 1;
  if (!safe_ink_size.IsValid<int32_t>()) {
    return -1;
  }

  // SAFETY: required from caller.
  auto points_span = UNSAFE_BUFFERS(pdfium::span(points, point_count));
  auto ink_coord_list = inklist->AppendNew<CPDF_Array>();
  for (const auto& point : points_span) {
    ink_coord_list->AppendNew<CPDF_Number>(point.x);
    ink_coord_list->AppendNew<CPDF_Number>(point.y);
  }
  return static_cast<int>(inklist->size() - 1);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_RemoveInkList(FPDF_ANNOTATION annot) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_INK) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> annot_dict =
      CPDFAnnotContextFromFPDFAnnotation(annot)->GetMutableAnnotDict();
  annot_dict->RemoveFor("InkList");
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_AppendObject(FPDF_ANNOTATION annot, FPDF_PAGEOBJECT obj) {
  CPDF_AnnotContext* pAnnot = CPDFAnnotContextFromFPDFAnnotation(annot);
  CPDF_PageObject* pObj = CPDFPageObjectFromFPDFPageObject(obj);
  if (!pAnnot || !pObj) {
    return false;
  }

  // Check that the annotation type is supported by this method.
  if (!FPDFAnnot_IsObjectSupportedSubtype(FPDFAnnot_GetSubtype(annot))) {
    return false;
  }

  // If the annotation does not have an AP stream yet, generate and set it.
  RetainPtr<CPDF_Dictionary> pAnnotDict = pAnnot->GetMutableAnnotDict();
  RetainPtr<CPDF_Stream> pStream =
      GetAnnotAP(pAnnotDict.Get(), CPDF_Annot::AppearanceMode::kNormal);
  if (!pStream) {
    CPDF_GenerateAP::GenerateEmptyAP(pAnnot->GetPage()->GetDocument(),
                                     pAnnotDict.Get());
    pStream = GetAnnotAP(pAnnotDict.Get(), CPDF_Annot::AppearanceMode::kNormal);
    if (!pStream) {
      return false;
    }
  }

  // Get the annotation's corresponding form object for parsing its AP stream.
  if (!pAnnot->HasForm()) {
    pAnnot->SetForm(pStream);
  }

  // Check that the object did not come from the same annotation. If this check
  // succeeds, then it is assumed that the object came from
  // FPDFPageObj_CreateNew{Path|Rect}() or FPDFPageObj_New{Text|Image}Obj().
  // Note that an object that came from a different annotation must not be
  // passed here, since an object cannot belong to more than one annotation.
  CPDF_Form* pForm = pAnnot->GetForm();
  if (std::ranges::find_if(*pForm, pdfium::MatchesUniquePtr(pObj)) !=
      pForm->end()) {
    return false;
  }

  // Append the object to the object list.
  pForm->AppendPageObject(pdfium::WrapUnique(pObj));

  // Set the content stream data in the annotation's AP stream.
  UpdateContentStream(pForm, pStream.Get());
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV FPDFAnnot_GetObjectCount(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* pAnnot = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!pAnnot) {
    return 0;
  }

  CPDF_DocumentViewScope document_view(pAnnot->GetPage()->GetDocument());
  if (!pAnnot->HasForm()) {
    const CPDF_Dictionary* dict = pAnnot->GetAnnotDict();
    RetainPtr<CPDF_Stream> pStream =
        GetAnnotAP(dict, CPDF_Annot::AppearanceMode::kNormal);
    if (!pStream) {
      return 0;
    }

    pAnnot->SetForm(std::move(pStream));
  }
  return pdfium::checked_cast<int>(pAnnot->GetForm()->GetPageObjectCount());
}

FPDF_EXPORT FPDF_PAGEOBJECT FPDF_CALLCONV
FPDFAnnot_GetObject(FPDF_ANNOTATION annot, int index) {
  CPDF_AnnotContext* pAnnot = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!pAnnot || index < 0) {
    return nullptr;
  }

  CPDF_DocumentViewScope document_view(pAnnot->GetPage()->GetDocument());
  if (!pAnnot->HasForm()) {
    const CPDF_Dictionary* pAnnotDict = pAnnot->GetAnnotDict();
    RetainPtr<CPDF_Stream> pStream =
        GetAnnotAP(pAnnotDict, CPDF_Annot::AppearanceMode::kNormal);
    if (!pStream) {
      return nullptr;
    }

    pAnnot->SetForm(std::move(pStream));
  }

  return FPDFPageObjectFromCPDFPageObject(
      pAnnot->GetForm()->GetPageObjectByIndex(index));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_RemoveObject(FPDF_ANNOTATION annot, int index) {
  CPDF_AnnotContext* pAnnot = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!pAnnot || !pAnnot->HasForm() || index < 0) {
    return false;
  }

  // Check that the annotation type is supported by this method.
  if (!FPDFAnnot_IsObjectSupportedSubtype(FPDFAnnot_GetSubtype(annot))) {
    return false;
  }

  // Check that the annotation already has an appearance stream, since an
  // existing object is to be deleted.
  RetainPtr<CPDF_Dictionary> pAnnotDict = pAnnot->GetMutableAnnotDict();
  RetainPtr<CPDF_Stream> pStream =
      GetAnnotAP(pAnnotDict.Get(), CPDF_Annot::AppearanceMode::kNormal);
  if (!pStream) {
    return false;
  }

  if (!pAnnot->GetForm()->ErasePageObjectAtIndex(index)) {
    return false;
  }

  UpdateContentStream(pAnnot->GetForm(), pStream.Get());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_SetColor(FPDF_ANNOTATION annot,
                                                       FPDFANNOT_COLORTYPE type,
                                                       unsigned int R,
                                                       unsigned int G,
                                                       unsigned int B,
                                                       unsigned int A) {
  RetainPtr<CPDF_Dictionary> pAnnotDict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);

  if (!pAnnotDict || R > 255 || G > 255 || B > 255 || A > 255) {
    return false;
  }

  // For annotations with their appearance streams already defined, the path
  // stream's own color definitions take priority over the annotation color
  // definitions set by this method, hence this method will simply fail.
  if (HasAPStream(pAnnotDict.Get())) {
    return false;
  }

  // Set the opacity of the annotation.
  pAnnotDict->SetNewFor<CPDF_Number>("CA", A / 255.f);

  // Set the color of the annotation.
  ByteStringView key = type == FPDFANNOT_COLORTYPE_InteriorColor ? "IC" : "C";
  RetainPtr<CPDF_Array> pColor = pAnnotDict->GetMutableArrayFor(key);
  if (pColor) {
    pColor->Clear();
  } else {
    pColor = pAnnotDict->SetNewFor<CPDF_Array>(ByteString(key));
  }

  pColor->AppendNew<CPDF_Number>(R / 255.f);
  pColor->AppendNew<CPDF_Number>(G / 255.f);
  pColor->AppendNew<CPDF_Number>(B / 255.f);

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_GetColor(FPDF_ANNOTATION annot,
                                                       FPDFANNOT_COLORTYPE type,
                                                       unsigned int* R,
                                                       unsigned int* G,
                                                       unsigned int* B,
                                                       unsigned int* A) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);

  if (!pAnnotDict || !R || !G || !B || !A) {
    return false;
  }

  // For annotations with their appearance streams already defined, the path
  // stream's own color definitions take priority over the annotation color
  // definitions retrieved by this method, hence this method will simply fail.
  if (HasAPStream(pAnnotDict)) {
    return false;
  }

  RetainPtr<const CPDF_Array> pColor = pAnnotDict->GetArrayFor(
      type == FPDFANNOT_COLORTYPE_InteriorColor ? "IC" : "C");
  *A = (pAnnotDict->KeyExist("CA") ? pAnnotDict->GetFloatFor("CA") : 1) * 255.f;
  if (!pColor) {
    // Use default color. The default colors must be consistent with the ones
    // used to generate AP. See calls to GetColorStringWithDefault() in
    // CPDF_GenerateAP::Generate*AP().
    if (pAnnotDict->GetNameFor(pdfium::annotation::kSubtype) == "Highlight") {
      *R = 255;
      *G = 255;
      *B = 0;
    } else {
      *R = 0;
      *G = 0;
      *B = 0;
    }
    return true;
  }

  CFX_Color color = fpdfdoc::CFXColorFromArray(*pColor);
  switch (color.nColorType) {
    case CFX_Color::Type::kRGB:
      *R = color.fColor1 * 255.f;
      *G = color.fColor2 * 255.f;
      *B = color.fColor3 * 255.f;
      break;
    case CFX_Color::Type::kGray:
      *R = 255.f * color.fColor1;
      *G = 255.f * color.fColor1;
      *B = 255.f * color.fColor1;
      break;
    case CFX_Color::Type::kCMYK:
      *R = 255.f * (1 - color.fColor1) * (1 - color.fColor4);
      *G = 255.f * (1 - color.fColor2) * (1 - color.fColor4);
      *B = 255.f * (1 - color.fColor3) * (1 - color.fColor4);
      break;
    case CFX_Color::Type::kTransparent:
      *R = 0;
      *G = 0;
      *B = 0;
      break;
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_HasAttachmentPoints(FPDF_ANNOTATION annot) {
  if (!annot) {
    return false;
  }

  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  return subtype == FPDF_ANNOT_LINK || subtype == FPDF_ANNOT_HIGHLIGHT ||
         subtype == FPDF_ANNOT_UNDERLINE || subtype == FPDF_ANNOT_SQUIGGLY ||
         subtype == FPDF_ANNOT_STRIKEOUT || subtype == FPDF_ANNOT_REDACT;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetAttachmentPoints(FPDF_ANNOTATION annot,
                              size_t quad_index,
                              const FS_QUADPOINTSF* quad_points) {
  if (!FPDFAnnot_HasAttachmentPoints(annot) || !quad_points) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pAnnotDict =
      CPDFAnnotContextFromFPDFAnnotation(annot)->GetMutableAnnotDict();
  RetainPtr<CPDF_Array> pQuadPointsArray =
      GetMutableQuadPointsArrayFromDictionary(pAnnotDict.Get());
  if (!IsValidQuadPointsIndex(pQuadPointsArray.Get(), quad_index)) {
    return false;
  }

  SetQuadPointsAtIndex(pQuadPointsArray.Get(), quad_index, quad_points);
  UpdateBBox(pAnnotDict.Get());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_AppendAttachmentPoints(FPDF_ANNOTATION annot,
                                 const FS_QUADPOINTSF* quad_points) {
  if (!FPDFAnnot_HasAttachmentPoints(annot) || !quad_points) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pAnnotDict =
      CPDFAnnotContextFromFPDFAnnotation(annot)->GetMutableAnnotDict();
  RetainPtr<CPDF_Array> pQuadPointsArray =
      GetMutableQuadPointsArrayFromDictionary(pAnnotDict.Get());
  if (!pQuadPointsArray) {
    pQuadPointsArray = AddQuadPointsArrayToDictionary(pAnnotDict.Get());
  }
  AppendQuadPoints(pQuadPointsArray.Get(), quad_points);
  UpdateBBox(pAnnotDict.Get());
  return true;
}

FPDF_EXPORT size_t FPDF_CALLCONV
FPDFAnnot_CountAttachmentPoints(FPDF_ANNOTATION annot) {
  if (!FPDFAnnot_HasAttachmentPoints(annot)) {
    return 0;
  }

  const CPDF_Dictionary* pAnnotDict =
      CPDFAnnotContextFromFPDFAnnotation(annot)->GetAnnotDict();
  RetainPtr<const CPDF_Array> pArray =
      GetQuadPointsArrayFromDictionary(pAnnotDict);
  return pArray ? pArray->size() / 8 : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetAttachmentPoints(FPDF_ANNOTATION annot,
                              size_t quad_index,
                              FS_QUADPOINTSF* quad_points) {
  if (!FPDFAnnot_HasAttachmentPoints(annot) || !quad_points) {
    return false;
  }

  const CPDF_Dictionary* pAnnotDict =
      CPDFAnnotContextFromFPDFAnnotation(annot)->GetAnnotDict();
  RetainPtr<const CPDF_Array> pArray =
      GetQuadPointsArrayFromDictionary(pAnnotDict);
  if (!pArray) {
    return false;
  }

  return GetQuadPointsAtIndex(std::move(pArray), quad_index, quad_points);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_SetRect(FPDF_ANNOTATION annot,
                                                      const FS_RECTF* rect) {
  RetainPtr<CPDF_Dictionary> pAnnotDict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict || !rect) {
    return false;
  }

  CFX_FloatRect newRect = CFXFloatRectFromFSRectF(*rect);

  // Update the "Rect" entry in the annotation dictionary.
  pAnnotDict->SetRectFor(pdfium::annotation::kRect, newRect);

  // If the annotation's appearance stream is defined, the annotation is of a
  // type that does not have quadpoints, and the new rectangle is bigger than
  // the current bounding box, then update the "BBox" entry in the AP
  // dictionary too, since its "BBox" entry comes from annotation dictionary's
  // "Rect" entry.
  if (FPDFAnnot_HasAttachmentPoints(annot)) {
    return true;
  }

  RetainPtr<CPDF_Stream> pStream =
      GetAnnotAP(pAnnotDict.Get(), CPDF_Annot::AppearanceMode::kNormal);
  if (pStream && newRect.Contains(pStream->GetDict()->GetRectFor("BBox"))) {
    pStream->GetMutableDict()->SetRectFor("BBox", newRect);
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_GetRect(FPDF_ANNOTATION annot,
                                                      FS_RECTF* rect) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict || !rect) {
    return false;
  }

  *rect = FSRectFFromCFXFloatRect(
      pAnnotDict->GetRectFor(pdfium::annotation::kRect));
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetVertices(FPDF_ANNOTATION annot,
                      FS_POINTF* buffer,
                      unsigned long length) {
  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_POLYGON && subtype != FPDF_ANNOT_POLYLINE) {
    return 0;
  }

  const CPDF_Dictionary* annot_dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict) {
    return 0;
  }

  RetainPtr<const CPDF_Array> vertices =
      annot_dict->GetArrayFor(pdfium::annotation::kVertices);
  if (!vertices) {
    return 0;
  }

  // Truncate to an even number.
  const unsigned long points_len =
      fxcrt::CollectionSize<unsigned long>(*vertices) / 2;
  if (buffer && length >= points_len) {
    // SAFETY: required from caller.
    auto buffer_span = UNSAFE_BUFFERS(pdfium::span(buffer, length));
    for (unsigned long i = 0; i < points_len; ++i) {
      buffer_span[i].x = vertices->GetFloatAt(i * 2);
      buffer_span[i].y = vertices->GetFloatAt(i * 2 + 1);
    }
  }
  return points_len;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetInkListCount(FPDF_ANNOTATION annot) {
  RetainPtr<const CPDF_Array> ink_list = GetInkList(annot);
  return ink_list ? fxcrt::CollectionSize<unsigned long>(*ink_list) : 0;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetInkListPath(FPDF_ANNOTATION annot,
                         unsigned long path_index,
                         FS_POINTF* buffer,
                         unsigned long length) {
  RetainPtr<const CPDF_Array> ink_list = GetInkList(annot);
  if (!ink_list) {
    return 0;
  }

  RetainPtr<const CPDF_Array> path = ink_list->GetArrayAt(path_index);
  if (!path) {
    return 0;
  }

  // Truncate to an even number.
  const unsigned long points_len =
      fxcrt::CollectionSize<unsigned long>(*path) / 2;
  if (buffer && length >= points_len) {
    // SAFETY: required from caller.
    auto buffer_span = UNSAFE_BUFFERS(pdfium::span(buffer, length));
    for (unsigned long i = 0; i < points_len; ++i) {
      buffer_span[i].x = path->GetFloatAt(i * 2);
      buffer_span[i].y = path->GetFloatAt(i * 2 + 1);
    }
  }
  return points_len;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_GetLine(FPDF_ANNOTATION annot,
                                                      FS_POINTF* start,
                                                      FS_POINTF* end) {
  if (!start || !end) {
    return false;
  }

  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_LINE) {
    return false;
  }

  const CPDF_Dictionary* annot_dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict) {
    return false;
  }

  RetainPtr<const CPDF_Array> line =
      annot_dict->GetArrayFor(pdfium::annotation::kL);
  if (!line || line->size() < 4) {
    return false;
  }

  start->x = line->GetFloatAt(0);
  start->y = line->GetFloatAt(1);
  end->x = line->GetFloatAt(2);
  end->y = line->GetFloatAt(3);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_SetBorder(FPDF_ANNOTATION annot,
                                                        float horizontal_radius,
                                                        float vertical_radius,
                                                        float border_width) {
  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict) {
    return false;
  }

  // Remove the appearance stream. Otherwise PDF viewers will render that and
  // not use the border values.
  annot_dict->RemoveFor(pdfium::annotation::kAP);

  auto border = annot_dict->SetNewFor<CPDF_Array>(pdfium::annotation::kBorder);
  border->AppendNew<CPDF_Number>(horizontal_radius);
  border->AppendNew<CPDF_Number>(vertical_radius);
  border->AppendNew<CPDF_Number>(border_width);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetBorder(FPDF_ANNOTATION annot,
                    float* horizontal_radius,
                    float* vertical_radius,
                    float* border_width) {
  if (!horizontal_radius || !vertical_radius || !border_width) {
    return false;
  }

  const CPDF_Dictionary* annot_dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict) {
    return false;
  }

  RetainPtr<const CPDF_Array> border =
      annot_dict->GetArrayFor(pdfium::annotation::kBorder);
  if (!border || border->size() < 3) {
    return false;
  }

  *horizontal_radius = border->GetFloatAt(0);
  *vertical_radius = border->GetFloatAt(1);
  *border_width = border->GetFloatAt(2);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_HasKey(FPDF_ANNOTATION annot,
                                                     FPDF_BYTESTRING key) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  return pAnnotDict && pAnnotDict->KeyExist(key);
}

FPDF_EXPORT FPDF_OBJECT_TYPE FPDF_CALLCONV
FPDFAnnot_GetValueType(FPDF_ANNOTATION annot, FPDF_BYTESTRING key) {
  if (!FPDFAnnot_HasKey(annot, key)) {
    return FPDF_OBJECT_UNKNOWN;
  }

  const CPDF_AnnotContext* pAnnot = CPDFAnnotContextFromFPDFAnnotation(annot);
  RetainPtr<const CPDF_Object> pObj = pAnnot->GetAnnotDict()->GetObjectFor(key);
  return pObj ? pObj->GetType() : FPDF_OBJECT_UNKNOWN;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetStringValue(FPDF_ANNOTATION annot,
                         FPDF_BYTESTRING key,
                         FPDF_WIDESTRING value) {
  RetainPtr<CPDF_Dictionary> pAnnotDict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  // SAFETY: required from caller.
  pAnnotDict->SetNewFor<CPDF_String>(
      key, UNSAFE_BUFFERS(WideStringFromFPDFWideString(value).AsStringView()));
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetStringValue(FPDF_ANNOTATION annot,
                         FPDF_BYTESTRING key,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return 0;
  }
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      pAnnotDict->GetUnicodeTextFor(key),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetNumberValue(FPDF_ANNOTATION annot,
                         FPDF_BYTESTRING key,
                         float* value) {
  if (!value) {
    return false;
  }

  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  RetainPtr<const CPDF_Object> p = pAnnotDict->GetObjectFor(key);
  if (!p || p->GetType() != FPDF_OBJECT_NUMBER) {
    return false;
  }

  *value = p->GetNumber();
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetNumberValue(FPDF_ANNOTATION annot,
                         FPDF_BYTESTRING key,
                         float value) {
  RetainPtr<CPDF_Dictionary> pAnnotDict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }
  pAnnotDict->SetNewFor<CPDF_Number>(key, static_cast<int>(value));
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_HasEmbedMetadata(FPDF_ANNOTATION annot) {
  const CPDF_Dictionary* annot_dict = GetAnnotDictFromFPDFAnnotation(annot);
  return !!GetEmbedMetadataDict(annot_dict);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearEmbedMetadata(FPDF_ANNOTATION annot) {
  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict) {
    return false;
  }

  annot_dict->RemoveFor(kEmbedMetadataKey);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearEmbedMetadataKey(FPDF_ANNOTATION annot, FPDF_BYTESTRING key) {
  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict || !key) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> metadata =
      annot_dict->GetMutableDictFor(kEmbedMetadataKey);
  if (!metadata) {
    return true;
  }

  metadata->RemoveFor(key);
  RemoveEmbedMetadataIfEmpty(annot_dict);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetEmbedMetadataString(FPDF_ANNOTATION annot,
                                 FPDF_BYTESTRING key,
                                 FPDF_WIDESTRING value) {
  if (!key) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> metadata = GetOrCreateEmbedMetadataDict(
      GetMutableAnnotDictFromFPDFAnnotation(annot));
  if (!metadata) {
    return false;
  }

  metadata->SetNewFor<CPDF_String>(
      key, UNSAFE_BUFFERS(WideStringFromFPDFWideString(value).AsStringView()));
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetEmbedMetadataString(FPDF_ANNOTATION annot,
                                 FPDF_BYTESTRING key,
                                 FPDF_WCHAR* buffer,
                                 unsigned long buflen) {
  if (!key) {
    return 0;
  }

  const CPDF_Dictionary* annot_dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict) {
    return 0;
  }

  RetainPtr<const CPDF_Dictionary> metadata = GetEmbedMetadataDict(annot_dict);
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      metadata ? metadata->GetUnicodeTextFor(key) : WideString(),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetEmbedMetadataNumber(FPDF_ANNOTATION annot,
                                 FPDF_BYTESTRING key,
                                 float value) {
  if (!key) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> metadata = GetOrCreateEmbedMetadataDict(
      GetMutableAnnotDictFromFPDFAnnotation(annot));
  if (!metadata) {
    return false;
  }

  metadata->SetNewFor<CPDF_Number>(key, value);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetEmbedMetadataNumber(FPDF_ANNOTATION annot,
                                 FPDF_BYTESTRING key,
                                 float* value) {
  if (!key || !value) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> metadata =
      GetEmbedMetadataDict(GetAnnotDictFromFPDFAnnotation(annot));
  if (!metadata) {
    return false;
  }

  RetainPtr<const CPDF_Object> object = metadata->GetObjectFor(key);
  if (!object || object->GetType() != CPDF_Object::Type::kNumber) {
    return false;
  }

  *value = object->GetNumber();
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetEmbedMetadataBoolean(FPDF_ANNOTATION annot,
                                  FPDF_BYTESTRING key,
                                  FPDF_BOOL value) {
  if (!key) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> metadata = GetOrCreateEmbedMetadataDict(
      GetMutableAnnotDictFromFPDFAnnotation(annot));
  if (!metadata) {
    return false;
  }

  metadata->SetNewFor<CPDF_Boolean>(key, !!value);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetEmbedMetadataBoolean(FPDF_ANNOTATION annot,
                                  FPDF_BYTESTRING key,
                                  FPDF_BOOL* value) {
  if (!key || !value) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> metadata =
      GetEmbedMetadataDict(GetAnnotDictFromFPDFAnnotation(annot));
  if (!metadata) {
    return false;
  }

  RetainPtr<const CPDF_Object> object = metadata->GetObjectFor(key);
  if (!object || object->GetType() != CPDF_Object::Type::kBoolean) {
    return false;
  }

  *value = object->GetInteger() != 0;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetEmbedMetadataRect(FPDF_ANNOTATION annot,
                               FPDF_BYTESTRING key,
                               const FS_RECTF* rect) {
  if (!key) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict) {
    return false;
  }

  if (!rect) {
    RetainPtr<CPDF_Dictionary> metadata =
        annot_dict->GetMutableDictFor(kEmbedMetadataKey);
    if (metadata) {
      metadata->RemoveFor(key);
      RemoveEmbedMetadataIfEmpty(annot_dict);
    }
    return true;
  }

  RetainPtr<CPDF_Dictionary> metadata =
      GetOrCreateEmbedMetadataDict(annot_dict);
  if (!metadata) {
    return false;
  }

  metadata->SetRectFor(key, CFXFloatRectFromFSRectF(*rect));
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetEmbedMetadataRect(FPDF_ANNOTATION annot,
                               FPDF_BYTESTRING key,
                               FS_RECTF* rect) {
  if (!key || !rect) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> metadata =
      GetEmbedMetadataDict(GetAnnotDictFromFPDFAnnotation(annot));
  if (!metadata) {
    return false;
  }

  RetainPtr<const CPDF_Object> object = metadata->GetObjectFor(key);
  if (!object || object->GetType() != CPDF_Object::Type::kArray) {
    return false;
  }

  *rect = FSRectFFromCFXFloatRect(metadata->GetRectFor(key));
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetEmbedMetadataJSON(FPDF_ANNOTATION annot, FPDF_WIDESTRING json) {
  return EPDFAnnot_SetEmbedMetadataString(annot, kEmbedMetadataCustomJSONKey,
                                          json);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetEmbedMetadataJSON(FPDF_ANNOTATION annot,
                               FPDF_WCHAR* buffer,
                               unsigned long buflen) {
  return EPDFAnnot_GetEmbedMetadataString(annot, kEmbedMetadataCustomJSONKey,
                                          buffer, buflen);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDocument_ClearEmbedMetadata(FPDF_DOCUMENT document) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(document);
  if (!pdf) {
    return false;
  }

  for (int page_index = 0; page_index < pdf->GetPageCount(); ++page_index) {
    RetainPtr<CPDF_Dictionary> page_dict =
        pdf->GetMutablePageDictionary(page_index);
    if (!page_dict) {
      continue;
    }

    RetainPtr<CPDF_Array> annots = page_dict->GetMutableArrayFor("Annots");
    if (!annots) {
      continue;
    }

    for (size_t annot_index = 0; annot_index < annots->size(); ++annot_index) {
      RetainPtr<CPDF_Dictionary> annot_dict =
          annots->GetMutableDictAt(annot_index);
      if (annot_dict) {
        annot_dict->RemoveFor(kEmbedMetadataKey);
      }
    }
  }

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetAP(FPDF_ANNOTATION annot,
                FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                FPDF_WIDESTRING value) {
  RetainPtr<CPDF_Dictionary> pAnnotDict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  if (appearanceMode < 0 || appearanceMode >= FPDF_ANNOT_APPEARANCEMODE_COUNT) {
    return false;
  }

  static constexpr auto kModeKeyForMode =
      std::to_array<const char*>({"N", "R", "D"});
  static_assert(kModeKeyForMode.size() == FPDF_ANNOT_APPEARANCEMODE_COUNT,
                "length of kModeKeyForMode should be equal to "
                "FPDF_ANNOT_APPEARANCEMODE_COUNT");

  const char* mode_key = kModeKeyForMode[appearanceMode];

  RetainPtr<CPDF_Dictionary> ap_dict =
      pAnnotDict->GetMutableDictFor(pdfium::annotation::kAP);

  // If `value` is null, then the action is to remove.
  if (!value) {
    if (ap_dict) {
      if (appearanceMode == FPDF_ANNOT_APPEARANCEMODE_NORMAL) {
        pAnnotDict->RemoveFor(pdfium::annotation::kAP);
      } else {
        ap_dict->RemoveFor(mode_key);
      }
    }
    return true;
  }

  // Otherwise, add/update when `value` is non-null.
  //
  // Annotation object's non-empty bounding rect will be used as the /BBox
  // for the associated /XObject object
  CFX_FloatRect rect = pAnnotDict->GetRectFor(pdfium::annotation::kRect);
  static constexpr float kMinSize = 0.000001f;
  if (rect.Width() < kMinSize || rect.Height() < kMinSize) {
    return false;
  }

  CPDF_AnnotContext* pAnnotContext = CPDFAnnotContextFromFPDFAnnotation(annot);

  CPDF_Document* doc = pAnnotContext->GetPage()->GetDocument();
  if (!doc) {
    return false;
  }

  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  stream_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kType, "XObject");
  stream_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype, "Form");
  stream_dict->SetRectFor("BBox", rect);
  // Transparency values are specified in range [0.0f, 1.0f]. We are strictly
  // checking for value < 1 and not <= 1 so that the output PDF size does not
  // unnecessarily bloat up by creating a new dictionary in case of solid
  // color.
  if (pAnnotDict->KeyExist("CA") && pAnnotDict->GetFloatFor("CA") < 1.0f) {
    stream_dict->SetFor("Resources", SetExtGStateInResourceDict(
                                         doc, pAnnotDict.Get(), "Normal"));
  }
  // SAFETY: required from caller.
  ByteString new_stream_data = PDF_EncodeText(
      UNSAFE_BUFFERS(WideStringFromFPDFWideString(value).AsStringView()));
  auto new_stream = doc->NewIndirect<CPDF_Stream>(std::move(stream_dict));
  new_stream->SetData(new_stream_data.unsigned_span());

  // Storing reference to indirect object in annotation's AP
  if (!ap_dict) {
    ap_dict = pAnnotDict->SetNewFor<CPDF_Dictionary>(pdfium::annotation::kAP);
  }
  ap_dict->SetNewFor<CPDF_Reference>(mode_key, doc, new_stream->GetObjNum());

  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetAP(FPDF_ANNOTATION annot,
                FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                FPDF_WCHAR* buffer,
                unsigned long buflen) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return 0;
  }

  if (appearanceMode < 0 || appearanceMode >= FPDF_ANNOT_APPEARANCEMODE_COUNT) {
    return 0;
  }

  CPDF_Annot::AppearanceMode mode =
      static_cast<CPDF_Annot::AppearanceMode>(appearanceMode);

  RetainPtr<CPDF_Stream> pStream = GetAnnotAPNoFallback(pAnnotDict, mode);
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      pStream ? pStream->GetUnicodeText() : WideString(),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDFAnnot_GetLinkedAnnot(FPDF_ANNOTATION annot, FPDF_BYTESTRING key) {
  CPDF_AnnotContext* pAnnot = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!pAnnot) {
    return nullptr;
  }

  CPDF_DocumentViewScope document_view(pAnnot->GetPage()->GetDocument());
  const CPDF_Dictionary* annot_dict = pAnnot->GetAnnotDict();
  RetainPtr<const CPDF_Dictionary> const_linked_dict =
      annot_dict ? annot_dict->GetDictFor(key) : nullptr;
  RetainPtr<CPDF_Dictionary> pLinkedDict =
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(const_linked_dict.Get()));
  if (!pLinkedDict || pLinkedDict->GetNameFor("Type") != "Annot") {
    return nullptr;
  }

  auto pLinkedAnnot = std::make_unique<CPDF_AnnotContext>(
      std::move(pLinkedDict), pAnnot->GetPage());

  // Caller takes ownership.
  return FPDFAnnotationFromCPDFAnnotContext(pLinkedAnnot.release());
}

FPDF_EXPORT int FPDF_CALLCONV FPDFAnnot_GetFlags(FPDF_ANNOTATION annot) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  return pAnnotDict ? pAnnotDict->GetIntegerFor(pdfium::annotation::kF)
                    : FPDF_ANNOT_FLAG_NONE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_SetFlags(FPDF_ANNOTATION annot,
                                                       int flags) {
  RetainPtr<CPDF_Dictionary> pAnnotDict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  pAnnotDict->SetNewFor<CPDF_Number>(pdfium::annotation::kF, flags);
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFAnnot_GetFormFieldFlags(FPDF_FORMHANDLE hHandle, FPDF_ANNOTATION annot) {
  CPDF_FormField* pFormField = GetFormField(hHandle, annot);
  return pFormField ? pFormField->GetFieldFlags() : FPDF_FORMFLAG_NONE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetFormFieldFlags(FPDF_FORMHANDLE handle,
                            FPDF_ANNOTATION annot,
                            int flags) {
  CPDF_FormField* form_field = GetFormField(handle, annot);
  if (!form_field) {
    return false;
  }

  form_field->SetFieldFlags(flags);
  return true;
}

FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDFAnnot_GetFormFieldAtPoint(FPDF_FORMHANDLE hHandle,
                              FPDF_PAGE page,
                              const FS_POINTF* point) {
  if (!point) {
    return nullptr;
  }

  const CPDFSDK_InteractiveForm* pForm = FormHandleToInteractiveForm(hHandle);
  if (!pForm) {
    return nullptr;
  }

  const CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return nullptr;
  }

  const CPDF_InteractiveForm* pPDFForm = pForm->GetInteractiveForm();
  int annot_index = -1;
  const CPDF_FormControl* pFormCtrl = pPDFForm->GetControlAtPoint(
      pPage, CFXPointFFromFSPointF(*point), &annot_index);
  if (!pFormCtrl || annot_index == -1) {
    return nullptr;
  }
  return FPDFPage_GetAnnot(page, annot_index);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetFormFieldName(FPDF_FORMHANDLE hHandle,
                           FPDF_ANNOTATION annot,
                           FPDF_WCHAR* buffer,
                           unsigned long buflen) {
  const CPDF_FormField* pFormField = GetFormField(hHandle, annot);
  if (!pFormField) {
    return 0;
  }
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      pFormField->GetFullName(),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFAnnot_GetFormFieldType(FPDF_FORMHANDLE hHandle, FPDF_ANNOTATION annot) {
  const CPDF_FormField* pFormField = GetFormField(hHandle, annot);
  return pFormField ? static_cast<int>(pFormField->GetFieldType()) : -1;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetFormAdditionalActionJavaScript(FPDF_FORMHANDLE hHandle,
                                            FPDF_ANNOTATION annot,
                                            int event,
                                            FPDF_WCHAR* buffer,
                                            unsigned long buflen) {
  const CPDF_FormField* pFormField = GetFormField(hHandle, annot);
  if (!pFormField) {
    return 0;
  }

  if (event < FPDF_ANNOT_AACTION_KEY_STROKE ||
      event > FPDF_ANNOT_AACTION_CALCULATE) {
    return 0;
  }

  auto type = static_cast<CPDF_AAction::AActionType>(event);
  CPDF_AAction additional_action = pFormField->GetAdditionalAction();
  CPDF_Action action = additional_action.GetAction(type);
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      action.GetJavaScript(),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetFormFieldAlternateName(FPDF_FORMHANDLE hHandle,
                                    FPDF_ANNOTATION annot,
                                    FPDF_WCHAR* buffer,
                                    unsigned long buflen) {
  const CPDF_FormField* pFormField = GetFormField(hHandle, annot);
  if (!pFormField) {
    return 0;
  }
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      pFormField->GetAlternateName(),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetFormFieldValue(FPDF_FORMHANDLE hHandle,
                            FPDF_ANNOTATION annot,
                            FPDF_WCHAR* buffer,
                            unsigned long buflen) {
  const CPDF_FormField* pFormField = GetFormField(hHandle, annot);
  if (!pFormField) {
    return 0;
  }
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      pFormField->GetValue(),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT int FPDF_CALLCONV FPDFAnnot_GetOptionCount(FPDF_FORMHANDLE hHandle,
                                                       FPDF_ANNOTATION annot) {
  const CPDF_FormField* form_field = GetFormField(hHandle, annot);
  if (!form_field || !form_field->HasOptField()) {
    return -1;
  }
  return form_field->CountOptions();
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetOptionLabel(FPDF_FORMHANDLE hHandle,
                         FPDF_ANNOTATION annot,
                         int index,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen) {
  if (index < 0) {
    return 0;
  }

  const CPDF_FormField* form_field = GetFormField(hHandle, annot);
  if (!form_field || !form_field->HasOptField() ||
      index >= form_field->CountOptions()) {
    return 0;
  }

  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      form_field->GetOptionLabel(index),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_IsOptionSelected(FPDF_FORMHANDLE handle,
                           FPDF_ANNOTATION annot,
                           int index) {
  if (index < 0) {
    return false;
  }

  const CPDF_FormField* form_field = GetFormField(handle, annot);
  if (!form_field) {
    return false;
  }

  if (form_field->GetFieldType() != FormFieldType::kComboBox &&
      form_field->GetFieldType() != FormFieldType::kListBox) {
    return false;
  }

  return index < form_field->CountOptions() &&
         form_field->IsItemSelected(index);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetFontSize(FPDF_FORMHANDLE hHandle,
                      FPDF_ANNOTATION annot,
                      float* value) {
  if (!value) {
    return false;
  }

  const CPDFSDK_Widget* widget = GetWidgetOfTypes(hHandle, annot, {});
  if (!widget) {
    return false;
  }

  *value = widget->GetFontSize();
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetFontColor(FPDF_FORMHANDLE handle,
                       FPDF_ANNOTATION annot,
                       unsigned int R,
                       unsigned int G,
                       unsigned int B) {
  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict || R > 255 || G > 255 || B > 255) {
    return false;
  }

  const CPDF_Annot::Subtype subtype = CPDF_Annot::StringToAnnotSubtype(
      annot_dict->GetNameFor(pdfium::annotation::kSubtype));
  if (subtype != CPDF_Annot::Subtype::FREETEXT) {
    // TODO(thestig): Consider adding widget support to mirror
    // FPDFAnnot_GetFontColor().
    return false;
  }

  CPDFSDK_InteractiveForm* form = FormHandleToInteractiveForm(handle);
  if (!form) {
    return false;
  }

  CPDF_Document* doc = form->GetInteractiveForm()->document();
  bool updated = CPDF_GenerateAP::GenerateDefaultAppearanceWithColor(
      doc, annot_dict, CFX_Color(R, G, B));
  if (!updated) {
    return false;
  }

  return CPDF_GenerateAP::GenerateAnnotAP(doc, annot_dict.Get(),
                                          CPDF_Annot::Subtype::FREETEXT);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetFontColor(FPDF_FORMHANDLE hHandle,
                       FPDF_ANNOTATION annot,
                       unsigned int* R,
                       unsigned int* G,
                       unsigned int* B) {
  if (!R || !G || !B) {
    return false;
  }

  FX_COLORREF font_color;
  switch (FPDFAnnot_GetSubtype(annot)) {
    case FPDF_ANNOT_FREETEXT: {
      auto maybe_font_color = GetFreetextFontColor(hHandle, annot);
      if (!maybe_font_color.has_value()) {
        return false;
      }
      font_color = ArgbToColorRef(maybe_font_color.value().argb);
      break;
    }
    case FPDF_ANNOT_WIDGET: {
      auto maybe_font_color = GetWidgetFontColor(hHandle, annot);
      if (!maybe_font_color.has_value()) {
        return false;
      }
      font_color = maybe_font_color.value();
      break;
    }
    default: {
      return false;
    }
  }

  *R = FXSYS_GetRValue(font_color);
  *G = FXSYS_GetGValue(font_color);
  *B = FXSYS_GetBValue(font_color);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_IsChecked(FPDF_FORMHANDLE hHandle,
                                                        FPDF_ANNOTATION annot) {
  const CPDFSDK_Widget* pWidget =
      GetRadioButtonOrCheckBoxWidget(hHandle, annot);
  return pWidget && pWidget->IsChecked();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetFocusableSubtypes(FPDF_FORMHANDLE hHandle,
                               const FPDF_ANNOTATION_SUBTYPE* subtypes,
                               size_t count) {
  CPDFSDK_FormFillEnvironment* pFormFillEnv =
      CPDFSDKFormFillEnvironmentFromFPDFFormHandle(hHandle);
  if (!pFormFillEnv) {
    return false;
  }

  if (count > 0 && !subtypes) {
    return false;
  }

  // SAFETY: required from caller.
  auto subtypes_span = UNSAFE_BUFFERS(pdfium::span(subtypes, count));
  std::vector<CPDF_Annot::Subtype> focusable_annot_types;
  focusable_annot_types.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    focusable_annot_types.push_back(
        static_cast<CPDF_Annot::Subtype>(subtypes_span[i]));
  }

  pFormFillEnv->SetFocusableAnnotSubtypes(focusable_annot_types);
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFAnnot_GetFocusableSubtypesCount(FPDF_FORMHANDLE hHandle) {
  CPDFSDK_FormFillEnvironment* pFormFillEnv =
      CPDFSDKFormFillEnvironmentFromFPDFFormHandle(hHandle);
  if (!pFormFillEnv) {
    return -1;
  }

  return fxcrt::CollectionSize<int>(pFormFillEnv->GetFocusableAnnotSubtypes());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetFocusableSubtypes(FPDF_FORMHANDLE hHandle,
                               FPDF_ANNOTATION_SUBTYPE* subtypes,
                               size_t count) {
  CPDFSDK_FormFillEnvironment* pFormFillEnv =
      CPDFSDKFormFillEnvironmentFromFPDFFormHandle(hHandle);
  if (!pFormFillEnv) {
    return false;
  }

  if (!subtypes) {
    return false;
  }

  const std::vector<CPDF_Annot::Subtype>& focusable_annot_types =
      pFormFillEnv->GetFocusableAnnotSubtypes();

  // Host should allocate enough memory to get the list of currently supported
  // focusable subtypes.
  if (count < focusable_annot_types.size()) {
    return false;
  }

  // SAFETY: required from caller.
  auto subtypes_span = UNSAFE_BUFFERS(pdfium::span(subtypes, count));
  for (size_t i = 0; i < focusable_annot_types.size(); ++i) {
    subtypes_span[i] =
        static_cast<FPDF_ANNOTATION_SUBTYPE>(focusable_annot_types[i]);
  }
  return true;
}

FPDF_EXPORT FPDF_LINK FPDF_CALLCONV FPDFAnnot_GetLink(FPDF_ANNOTATION annot) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_LINK) {
    return nullptr;
  }

  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context) {
    return nullptr;
  }

  // Unretained reference in public API. NOLINTNEXTLINE
  return FPDFLinkFromCPDFDictionary(
      const_cast<CPDF_Dictionary*>(context->GetAnnotDict()));
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFAnnot_GetFormControlCount(FPDF_FORMHANDLE hHandle, FPDF_ANNOTATION annot) {
  CPDF_FormField* pFormField = GetFormField(hHandle, annot);
  return pFormField ? pFormField->CountControls() : -1;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFAnnot_GetFormControlIndex(FPDF_FORMHANDLE hHandle, FPDF_ANNOTATION annot) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return -1;
  }

  CPDFSDK_InteractiveForm* pForm = FormHandleToInteractiveForm(hHandle);
  if (!pForm) {
    return -1;
  }

  CPDF_InteractiveForm* pPDFForm = pForm->GetInteractiveForm();
  CPDF_FormField* pFormField = pPDFForm->GetFieldByDict(pAnnotDict);
  CPDF_FormControl* pFormControl = pPDFForm->GetControlByDict(pAnnotDict);
  return pFormField ? pFormField->GetControlIndex(pFormControl) : -1;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetFormFieldExportValue(FPDF_FORMHANDLE hHandle,
                                  FPDF_ANNOTATION annot,
                                  FPDF_WCHAR* buffer,
                                  unsigned long buflen) {
  const CPDFSDK_Widget* pWidget =
      GetRadioButtonOrCheckBoxWidget(hHandle, annot);
  if (!pWidget) {
    return 0;
  }
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      pWidget->GetExportValue(),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFAnnot_SetURI(FPDF_ANNOTATION annot,
                                                     const char* uri) {
  if (!uri || FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_LINK) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  auto action = annot_dict->SetNewFor<CPDF_Dictionary>("A");
  action->SetNewFor<CPDF_Name>("Type", "Action");
  action->SetNewFor<CPDF_Name>("S", "URI");
  action->SetNewFor<CPDF_String>("URI", uri);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetAction(FPDF_ANNOTATION annot,
                                                        FPDF_ACTION action) {
  if (!action || FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_LINK) {
    return false;
  }

  CPDF_AnnotContext* pAnnotContext = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!pAnnotContext) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> annot_dict = pAnnotContext->GetMutableAnnotDict();
  if (!annot_dict) {
    return false;
  }

  CPDF_Dictionary* act_dict = CPDFDictionaryFromFPDFAction(action);
  if (!act_dict) {
    return false;
  }

  // Require the action to be indirect so we can reference it.
  if (act_dict->GetObjNum() == 0) {
    return false;
  }

  CPDF_Document* pDoc = pAnnotContext->GetPage()->GetDocument();

  // Set /A as an indirect reference to the action. A link dictionary must
  // not carry both /A and /Dest (ISO 32000-1 Table 173), so drop any
  // pre-existing direct destination while we are at it.
  annot_dict->SetNewFor<CPDF_Reference>("A", pDoc, act_dict->GetObjNum());
  annot_dict->RemoveFor("Dest");
  return true;
}

namespace {

// Shared body of the two link-entry removers: single-purpose, idempotent.
FPDF_BOOL RemoveLinkDictEntry(FPDF_ANNOTATION annot, const char* key) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_LINK) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict) {
    return false;
  }

  annot_dict->RemoveFor(key);
  return true;
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_RemoveAction(FPDF_ANNOTATION annot) {
  return RemoveLinkDictEntry(annot, "A");
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_RemoveDest(FPDF_ANNOTATION annot) {
  return RemoveLinkDictEntry(annot, "Dest");
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_RemoveKey(FPDF_ANNOTATION annot, FPDF_BYTESTRING key) {
  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict || !key) {
    return false;
  }

  annot_dict->RemoveFor(key);
  return true;
}

FPDF_EXPORT FPDF_ATTACHMENT FPDF_CALLCONV
FPDFAnnot_GetFileAttachment(FPDF_ANNOTATION annot) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_FILEATTACHMENT) {
    return nullptr;
  }

  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context) {
    return nullptr;
  }
  CPDF_DocumentViewScope document_view(context->GetPage()->GetDocument());
  const CPDF_Dictionary* annot_dict = context->GetAnnotDict();
  if (!annot_dict) {
    return nullptr;
  }

  RetainPtr<const CPDF_Object> file_spec = annot_dict->GetDirectObjectFor("FS");
  return FPDFAttachmentFromCPDFObject(
      const_cast<CPDF_Object*>(file_spec.Get()));
}

FPDF_EXPORT FPDF_ATTACHMENT FPDF_CALLCONV
FPDFAnnot_AddFileAttachment(FPDF_ANNOTATION annot, FPDF_WIDESTRING name) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_FILEATTACHMENT) {
    return nullptr;
  }

  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context) {
    return nullptr;
  }

  // SAFETY: required from caller.
  WideString ws_name = UNSAFE_BUFFERS(WideStringFromFPDFWideString(name));
  if (ws_name.IsEmpty()) {
    return nullptr;
  }

  CPDF_Document* doc = context->GetPage()->GetDocument();
  auto fs_obj = doc->NewIndirect<CPDF_Dictionary>();

  fs_obj->SetNewFor<CPDF_Name>("Type", "Filespec");
  fs_obj->SetNewFor<CPDF_String>("UF", ws_name.AsStringView());
  fs_obj->SetNewFor<CPDF_String>("F", ws_name.AsStringView());

  context->GetMutableAnnotDict()->SetNewFor<CPDF_Reference>(
      "FS", doc, fs_obj->GetObjNum());
  return FPDFAttachmentFromCPDFObject(fs_obj);
}

static ByteString GetColorKeyForType(FPDFANNOT_COLORTYPE type) {
  switch (type) {
    case FPDFANNOT_COLORTYPE_InteriorColor:
      return "IC";
    case FPDFANNOT_COLORTYPE_OverlayColor:
      return "OC";
    case FPDFANNOT_COLORTYPE_TextColor:
      return "TextColor";
    case FPDFANNOT_COLORTYPE_Color:
      return "C";
  }
  return "C";
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetColor(FPDF_ANNOTATION annot,
                                                       FPDFANNOT_COLORTYPE type,
                                                       unsigned int R,
                                                       unsigned int G,
                                                       unsigned int B) {
  RetainPtr<CPDF_Dictionary> pAnnotDict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict || R > 255 || G > 255 || B > 255) {
    return false;
  }

  // OverlayColor (OC) is only valid for Redact annotations.
  if (type == FPDFANNOT_COLORTYPE_OverlayColor &&
      FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_REDACT) {
    return false;
  }

  ByteString key = GetColorKeyForType(type);
  RetainPtr<CPDF_Array> pColor =
      pAnnotDict->GetMutableArrayFor(key.AsStringView());
  if (pColor) {
    pColor->Clear();
  } else {
    pColor = pAnnotDict->SetNewFor<CPDF_Array>(key);
  }

  pColor->AppendNew<CPDF_Number>(R / 255.f);
  pColor->AppendNew<CPDF_Number>(G / 255.f);
  pColor->AppendNew<CPDF_Number>(B / 255.f);

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_GetColor(FPDF_ANNOTATION annot,
                                                       FPDFANNOT_COLORTYPE type,
                                                       unsigned int* R,
                                                       unsigned int* G,
                                                       unsigned int* B) {
  if (!R || !G || !B) {
    return false;
  }

  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  // OverlayColor (OC) is only valid for Redact annotations.
  if (type == FPDFANNOT_COLORTYPE_OverlayColor &&
      FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_REDACT) {
    return false;
  }

  ByteString key = GetColorKeyForType(type);
  RetainPtr<const CPDF_Array> pColor = dict->GetArrayFor(key.AsStringView());
  if (!pColor) {
    return false;  // "no colour set"
  }

  CFX_Color color = fpdfdoc::CFXColorFromArray(*pColor);
  switch (color.nColorType) {
    case CFX_Color::Type::kRGB:
      *R = color.fColor1 * 255.f;
      *G = color.fColor2 * 255.f;
      *B = color.fColor3 * 255.f;
      break;
    case CFX_Color::Type::kGray:
      *R = *G = *B = color.fColor1 * 255.f;
      break;
    case CFX_Color::Type::kCMYK:  // convert roughly
      *R = 255.f * (1 - color.fColor1) * (1 - color.fColor4);
      *G = 255.f * (1 - color.fColor2) * (1 - color.fColor4);
      *B = 255.f * (1 - color.fColor3) * (1 - color.fColor4);
      break;
    default:
      return false;
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearColor(FPDF_ANNOTATION annot, FPDFANNOT_COLORTYPE type) {
  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  // OverlayColor (OC) is only valid for Redact annotations.
  if (type == FPDFANNOT_COLORTYPE_OverlayColor &&
      FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_REDACT) {
    return false;
  }

  ByteString key = GetColorKeyForType(type);
  dict->RemoveFor(key.AsStringView());

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetOpacity(FPDF_ANNOTATION annot,
                                                         unsigned int alpha) {
  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict || alpha > 255) {
    return false;
  }

  if (alpha == 255) {
    dict->RemoveFor("CA");
  } else {
    dict->SetNewFor<CPDF_Number>("CA", alpha / 255.f);
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_GetOpacity(FPDF_ANNOTATION annot,
                                                         unsigned int* alpha) {
  if (!alpha) {
    return false;
  }
  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  float ca = dict->KeyExist("CA") ? dict->GetFloatFor("CA") : 1.0f;
  *alpha = std::clamp(ca, 0.f, 1.f) * 255.f + 0.5f;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetBorderEffect(FPDF_ANNOTATION annot, float* intensity) {
  if (!intensity) {
    return false;
  }

  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  // The Border Effect is defined in the /BE dictionary.
  RetainPtr<const CPDF_Dictionary> pBEDict = pAnnotDict->GetDictFor("BE");
  if (!pBEDict) {
    // No /BE dictionary means no special effect.
    return false;
  }

  // The style must be 'Cloudy' for the intensity to be meaningful.
  if (pBEDict->GetNameFor("S") != "C") {
    return false;
  }

  // The intensity is in the /I key. Default is 1 if not present.
  if (pBEDict->KeyExist("I")) {
    *intensity = pBEDict->GetFloatFor("I");
  } else {
    *intensity = 1.0f;  // Default intensity for cloudy border
  }

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetBorderEffect(FPDF_ANNOTATION annot, float intensity) {
  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_SQUARE && subtype != FPDF_ANNOT_CIRCLE &&
      subtype != FPDF_ANNOT_POLYGON && subtype != FPDF_ANNOT_FREETEXT) {
    return false;
  }

  CPDF_Dictionary* pAnnotDict = GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pBEDict =
      pAnnotDict->SetNewFor<CPDF_Dictionary>("BE");
  pBEDict->SetNewFor<CPDF_Name>("S", "C");
  pBEDict->SetNewFor<CPDF_Number>("I", intensity);

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearBorderEffect(FPDF_ANNOTATION annot) {
  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_SQUARE && subtype != FPDF_ANNOT_CIRCLE &&
      subtype != FPDF_ANNOT_POLYGON && subtype != FPDF_ANNOT_FREETEXT) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pAnnotDict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  pAnnotDict->RemoveFor("BE");
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetRectangleDifferences(FPDF_ANNOTATION annot,
                                  float* left,
                                  float* bottom,
                                  float* right,
                                  float* top) {
  if (!left || !bottom || !right || !top) {
    return false;
  }

  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_SQUARE && subtype != FPDF_ANNOT_CIRCLE &&
      subtype != FPDF_ANNOT_CARET && subtype != FPDF_ANNOT_FREETEXT &&
      subtype != FPDF_ANNOT_POLYGON) {
    return false;
  }

  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  RetainPtr<const CPDF_Array> pRDArray = pAnnotDict->GetArrayFor("RD");
  if (!pRDArray || pRDArray->size() < 4) {
    *left = 0;
    *bottom = 0;
    *right = 0;
    *top = 0;
    return false;
  }

  *left = pRDArray->GetFloatAt(0);
  *bottom = pRDArray->GetFloatAt(1);
  *right = pRDArray->GetFloatAt(2);
  *top = pRDArray->GetFloatAt(3);

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetRectangleDifferences(FPDF_ANNOTATION annot,
                                  float left,
                                  float bottom,
                                  float right,
                                  float top) {
  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_SQUARE && subtype != FPDF_ANNOT_CIRCLE &&
      subtype != FPDF_ANNOT_CARET && subtype != FPDF_ANNOT_FREETEXT &&
      subtype != FPDF_ANNOT_POLYGON) {
    return false;
  }

  CPDF_Dictionary* pAnnotDict = GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  RetainPtr<CPDF_Array> pRDArray = pAnnotDict->SetNewFor<CPDF_Array>("RD");
  pRDArray->AppendNew<CPDF_Number>(left);
  pRDArray->AppendNew<CPDF_Number>(bottom);
  pRDArray->AppendNew<CPDF_Number>(right);
  pRDArray->AppendNew<CPDF_Number>(top);

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearRectangleDifferences(FPDF_ANNOTATION annot) {
  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_SQUARE && subtype != FPDF_ANNOT_CIRCLE &&
      subtype != FPDF_ANNOT_CARET && subtype != FPDF_ANNOT_FREETEXT &&
      subtype != FPDF_ANNOT_POLYGON) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pAnnotDict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  pAnnotDict->RemoveFor("RD");
  return true;
}

namespace {

RetainPtr<const CPDF_Array> GetExplicitBorderDashArray(
    const CPDF_Dictionary* pAnnotDict) {
  RetainPtr<const CPDF_Dictionary> pBSDict = pAnnotDict->GetDictFor("BS");
  if (pBSDict) {
    // /BS takes precedence over /Border. A missing or unrecognised /S uses
    // the solid default, so no dash pattern applies.
    if (pBSDict->GetNameFor("S") != "D") {
      return nullptr;
    }
    return pBSDict->GetArrayFor("D");
  }

  RetainPtr<const CPDF_Array> pBorderArray =
      pAnnotDict->GetArrayFor(pdfium::annotation::kBorder);
  if (!pBorderArray || pBorderArray->size() < 4) {
    return nullptr;
  }
  return pBorderArray->GetArrayAt(3);
}

}  // namespace

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetBorderDashPatternCount(FPDF_ANNOTATION annot) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return 0;
  }

  RetainPtr<const CPDF_Array> pDashArray =
      GetExplicitBorderDashArray(pAnnotDict);
  return pDashArray ? pDashArray->size() : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetBorderDashPattern(FPDF_ANNOTATION annot,
                               float* dash_array,
                               unsigned long count) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict || !dash_array) {
    return false;
  }

  RetainPtr<const CPDF_Array> pDashArray =
      GetExplicitBorderDashArray(pAnnotDict);
  if (!pDashArray || pDashArray->size() < count) {
    return false;
  }

  for (unsigned long i = 0; i < count; ++i) {
    dash_array[i] = pDashArray->GetFloatAt(i);
  }

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetBorderDashPattern(FPDF_ANNOTATION annot,
                               const float* dash_array,
                               unsigned long count) {
  if (!annot) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> bs_dict = annot_dict->GetMutableDictFor("BS");
  if (!bs_dict) {
    bs_dict = annot_dict->SetNewFor<CPDF_Dictionary>("BS");
  }

  // --- Removal branch (PDFium style) ---
  if (!dash_array || count == 0) {
    bs_dict->RemoveFor("D");
    // Optional: if style was dashed only because of the array, you can revert.
    // Leaving it unchanged matches PDFium's permissive style.
    if (bs_dict->size() == 0) {
      annot_dict->RemoveFor("BS");
    }
    return true;
  }

  // --- Set branch ---
  bs_dict->SetNewFor<CPDF_Name>("S", "D");

  RetainPtr<CPDF_Array> d_array = bs_dict->GetMutableArrayFor("D");
  if (d_array) {
    d_array->Clear();
  } else {
    d_array = bs_dict->SetNewFor<CPDF_Array>("D");
  }

  // SAFETY: caller guarantees `dash_array` has `count` elements.
  for (unsigned long i = 0; i < count; ++i) {
    d_array->AppendNew<CPDF_Number>(dash_array[i]);
  }

  return true;
}

FPDF_EXPORT FPDF_ANNOT_BORDER_STYLE FPDF_CALLCONV
EPDFAnnot_GetBorderStyle(FPDF_ANNOTATION annot, float* width) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    if (width) {
      *width = 0;
    }
    return FPDF_ANNOT_BS_UNKNOWN;
  }

  RetainPtr<const CPDF_Dictionary> pBSDict = pAnnotDict->GetDictFor("BS");
  if (pBSDict) {
    if (width) {
      *width = pBSDict->KeyExist("W") ? pBSDict->GetFloatFor("W") : 1.0f;
    }

    CPDF_Annot::BorderStyle nStyle =
        CPDF_Annot::StringToBorderStyle(pBSDict->GetNameFor("S"));
    if (nStyle == CPDF_Annot::BorderStyle::kUnknown) {
      nStyle = CPDF_Annot::BorderStyle::kSolid;
    }
    return static_cast<FPDF_ANNOT_BORDER_STYLE>(nStyle);
  }

  RetainPtr<const CPDF_Array> pBorderArray =
      pAnnotDict->GetArrayFor(pdfium::annotation::kBorder);
  if (width) {
    *width = pBorderArray && pBorderArray->size() >= 3
                 ? pBorderArray->GetFloatAt(2)
                 : 1.0f;
  }

  RetainPtr<const CPDF_Array> pDashArray =
      GetExplicitBorderDashArray(pAnnotDict);
  if (pDashArray && !pDashArray->IsEmpty()) {
    return FPDF_ANNOT_BS_DASHED;
  }
  return FPDF_ANNOT_BS_SOLID;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetBorderStyle(FPDF_ANNOTATION annot,
                         FPDF_ANNOT_BORDER_STYLE style,
                         float width) {
  RetainPtr<CPDF_Dictionary> pAnnotDict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pBSDict = pAnnotDict->GetMutableDictFor("BS");
  if (!pBSDict) {
    pBSDict = pAnnotDict->SetNewFor<CPDF_Dictionary>("BS");
  }

  if (width >= 0) {
    pBSDict->SetNewFor<CPDF_Number>("W", width);
  } else {
    pBSDict->RemoveFor("W");
  }

  if (style == FPDF_ANNOT_BS_UNKNOWN) {
    pBSDict->RemoveFor("S");
    return true;
  }

  auto internal_style = static_cast<CPDF_Annot::BorderStyle>(style);
  ByteString style_name = CPDF_Annot::BorderStyleToString(internal_style);

  if (style_name.IsEmpty()) {
    return false;
  }

  pBSDict->SetNewFor<CPDF_Name>("S", style_name);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GenerateAppearance(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* pContext = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!pContext) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pAnnotDict = pContext->GetMutableAnnotDict();
  if (!pAnnotDict) {
    return false;
  }

  // CPDF_GenerateAP needs the document, which we can get from the page context.
  CPDF_Document* pDoc = pContext->GetPage()->GetDocument();
  if (!pDoc) {
    return false;
  }

  // Get the annotation subtype to pass to the generator.
  const CPDF_Annot::Subtype subtype = CPDF_Annot::StringToAnnotSubtype(
      pAnnotDict->GetNameFor(pdfium::annotation::kSubtype));

  // This is the key: call the internal AP generator.
  return CPDF_GenerateAP::GenerateAnnotAP(pDoc, pAnnotDict.Get(), subtype);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GenerateAppearanceWithBlend(FPDF_ANNOTATION annot,
                                      FPDF_BLENDMODE blend) {
  CPDF_AnnotContext* ctx = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!ctx) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> annot_dict = ctx->GetMutableAnnotDict();
  if (!annot_dict) {
    return false;
  }

  CPDF_Document* doc = ctx->GetPage()->GetDocument();
  if (!doc) {
    return false;
  }

  const CPDF_Annot::Subtype subtype = CPDF_Annot::StringToAnnotSubtype(
      annot_dict->GetNameFor(pdfium::annotation::kSubtype));

  // Validate blend enum range if needed (already static_assert aligned).
  auto internal = static_cast<BlendMode>(blend);

  return CPDF_GenerateAP::GenerateAnnotAP(doc, annot_dict.Get(), subtype,
                                          internal);
}

FPDF_EXPORT FPDF_BLENDMODE FPDF_CALLCONV
EPDFAnnot_GetBlendMode(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* ctx = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!ctx) {
    return FPDF_BLENDMODE_Normal;
  }

  BlendMode bm = GetEffectiveAnnotBlendMode(ctx);
  // Safe cast due to static_asserts above.
  return static_cast<FPDF_BLENDMODE>(bm);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetIntent(FPDF_ANNOTATION annot, FPDF_BYTESTRING intent) {
  if (!annot || !intent || !*intent) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  // Allow leading slash from caller; strip it.
  if (intent[0] == '/') {
    ++intent;
  }

  if (!*intent) {
    return false;
  }

  // Minimal validation (PDFium typically trusts caller). Could reject spaces /
  // delimiters (),<>[]{}/%# if you want to be stricter. Keeping permissive.
  dict->SetNewFor<CPDF_Name>("IT", intent);
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetIntent(FPDF_ANNOTATION annot,
                    FPDF_WCHAR* buffer,
                    unsigned long buflen) {
  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return 0;
  }

  ByteString name = dict->GetNameFor("IT");
  if (name.IsEmpty()) {
    return 0;
  }

  // Name objects are ASCII (or PDF name syntax). For normal ASCII we can
  // construct a WideString directly. (If you later want to decode #XX escapes
  // you could add a small routine; PDFium generally leaves raw name.)
  WideString wname = WideString::FromASCII(name.c_str());

  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      wname, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetRichContent(FPDF_ANNOTATION annot,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen) {
  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return 0;
  }

  // /RC may be a text string or a stream (PDF 2.0 §12.5.6.5).
  RetainPtr<const CPDF_Object> rc_obj = dict->GetObjectFor("RC");
  if (!rc_obj) {
    return 0;
  }

  WideString ws;
  if (rc_obj->IsString() || rc_obj->IsName()) {
    ws = rc_obj->GetUnicodeText();  // handles PDFDocEncoding / UTF‑16BE
  } else if (rc_obj->IsStream()) {
    ws = rc_obj->AsStream()->GetUnicodeText();
  } else {
    return 0;  // some exotic type we don't handle
  }

  // SAFETY: same pattern as other getters.
  return Utf16EncodeMaybeCopyAndReturnLength(
      ws, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

namespace {

const CPDF_Document* DocOf(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  return context ? context->GetPage()->GetDocument() : nullptr;
}

}  // namespace

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetRichTextJSON(FPDF_ANNOTATION annot,
                          char* buffer,
                          unsigned long buflen) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context) {
    return 0;
  }
  const CPDF_Dictionary* annot_dict = context->GetAnnotDict();
  if (!annot_dict) {
    return 0;
  }
  CPDF_Document* doc = context->GetPage()->GetDocument();
  const CPDF_Dictionary* root = doc ? doc->GetRoot() : nullptr;
  RetainPtr<const CPDF_Dictionary> acroform =
      root ? root->GetDictFor("AcroForm") : nullptr;
  const ByteString json = CPDF_RichTextParser::ToJSON(
      CPDF_RichTextParser::FromAnnotation(annot_dict, acroform.Get(),
                                          DocOf(annot)));
  // SAFETY: same pattern as other getters.
  return NulTerminateMaybeCopyAndReturnLength(
      json, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

namespace {

// The rich text setters share one path: build the document, then write
// all four forms and the appearance through CPDF_RichTextWriter, or nothing.
bool ApplyRichTextDocument(FPDF_ANNOTATION annot,
                           const CPDF_RichTextDocument& document) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> annot_dict = context->GetMutableAnnotDict();
  if (!annot_dict || annot_dict->GetNameFor("Subtype") != "FreeText") {
    return false;
  }
  CPDF_Document* doc = context->GetPage()->GetDocument();
  if (!doc) {
    return false;
  }
  return CPDF_RichTextWriter::Apply(doc, annot_dict.Get(), document, nullptr);
}

RetainPtr<const CPDF_Dictionary> AcroFormDictOf(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  CPDF_Document* doc = context ? context->GetPage()->GetDocument() : nullptr;
  const CPDF_Dictionary* root = doc ? doc->GetRoot() : nullptr;
  return root ? root->GetDictFor("AcroForm") : nullptr;
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetRichTextJSON(FPDF_ANNOTATION annot, FPDF_BYTESTRING json_utf8) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context || !json_utf8) {
    return false;
  }
  // The annotation's current paragraph defaults (/Q, /DS, the /RC body) are
  // the base for everything the JSON leaves unsaid — a body without an
  // alignment, a paragraph without one — so a centred box stays centred
  // when its paragraphs are replaced or its body restyled. (The STYLE of a
  // given body stays engine defaults under the given keys, as documented.)
  RetainPtr<const CPDF_Dictionary> acroform = AcroFormDictOf(annot);
  const CPDF_RichTextDocument current = CPDF_RichTextParser::FromAnnotation(
      context->GetAnnotDict(), acroform.Get(), DocOf(annot));
  CPDF_RichTextDocument document;
  bool has_body = false;
  if (!CPDF_RichTextJson::Parse(ByteString(json_utf8), &document, &has_body,
                                &current.body_paragraph)) {
    return false;
  }
  if (!has_body) {
    // The annotation's current body style (DA ∪ DS ∪ RC body) stays.
    document.body = current.body;
  }
  return ApplyRichTextDocument(annot, document);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetRichTextXHTML(FPDF_ANNOTATION annot, FPDF_WIDESTRING xhtml) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context || !xhtml) {
    return false;
  }
  const WideString xml = WideStringFromFPDFWideString(xhtml);
  if (xml.IsEmpty()) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> acroform = AcroFormDictOf(annot);
  CPDF_RichTextStyle defaults;
  CPDF_RichTextParagraphProps paragraph_defaults;
  std::vector<CPDF_RichTextDiagnostic> diagnostics;
  CPDF_RichTextParser::DefaultsFromAnnotation(
      context->GetAnnotDict(), acroform.Get(), &defaults, &paragraph_defaults,
      &diagnostics, DocOf(annot));
  const CPDF_RichTextDocument document = CPDF_RichTextParser::ParseRichContent(
      xml, defaults, paragraph_defaults, WideString());
  for (const CPDF_RichTextDiagnostic& diagnostic : document.diagnostics) {
    if (diagnostic.code == CPDF_RichTextDiagnostic::Code::kMalformedRC) {
      return false;
    }
  }
  return ApplyRichTextDocument(annot, document);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_SetTypographicFeatures(FPDF_DOCUMENT document, FPDF_BOOL enabled) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return false;
  }
  doc->SetTypographicFeaturesEnabled(!!enabled);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_GetTypographicFeatures(FPDF_DOCUMENT document) {
  const CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  return doc && doc->GetTypographicFeaturesEnabled();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetLineEndings(FPDF_ANNOTATION annot,
                         FPDF_ANNOT_LINE_END start_style,
                         FPDF_ANNOT_LINE_END end_style) {
  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_LINE && subtype != FPDF_ANNOT_POLYLINE &&
      subtype != FPDF_ANNOT_FREETEXT) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  auto to_core = [](FPDF_ANNOT_LINE_END v) {
    return static_cast<CPDF_Annot::LineEnding>(v);
  };

  CPDF_Annot::LineEnding s = to_core(start_style);
  CPDF_Annot::LineEnding e = to_core(end_style);

  // If both are unknown, remove the entry (PDFium style).
  if (s == CPDF_Annot::LineEnding::kUnknown &&
      e == CPDF_Annot::LineEnding::kUnknown) {
    dict->RemoveFor("LE");
    return true;
  }

  if (subtype == FPDF_ANNOT_FREETEXT) {
    // FreeText uses a single name for /LE (Acrobat convention).
    ByteString e_name = CPDF_Annot::LineEndingToString(e);
    if (e_name.IsEmpty()) {
      e_name = "None";
    }
    dict->SetNewFor<CPDF_Name>("LE", e_name);
  } else {
    ByteString s_name = CPDF_Annot::LineEndingToString(s);
    ByteString e_name = CPDF_Annot::LineEndingToString(e);

    if (s_name.IsEmpty()) {
      s_name = "None";
    }
    if (e_name.IsEmpty()) {
      e_name = "None";
    }

    RetainPtr<CPDF_Array> le = dict->GetMutableArrayFor("LE");
    if (le) {
      le->Clear();
    } else {
      le = dict->SetNewFor<CPDF_Array>("LE");
    }

    le->AppendNew<CPDF_Name>(s_name);
    le->AppendNew<CPDF_Name>(e_name);
  }

  return true;
}

static ByteString ReadLineEndingToken(const CPDF_Array* le, size_t idx) {
  RetainPtr<const CPDF_Object> obj = le->GetDirectObjectAt(idx);
  if (!obj) {
    return ByteString();
  }

  if (const CPDF_Name* n = obj->AsName()) {
    return n->GetString();  // e.g. "OpenArrow"
  }

  if (const CPDF_String* s = obj->AsString()) {
    return s->GetString();  // tolerate a stray string
  }

  return ByteString();  // anything else -> empty
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetLineEndings(FPDF_ANNOTATION annot,
                         FPDF_ANNOT_LINE_END* start_style,
                         FPDF_ANNOT_LINE_END* end_style) {
  if (!start_style || !end_style) {
    return false;
  }

  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_LINE && subtype != FPDF_ANNOT_POLYLINE &&
      subtype != FPDF_ANNOT_FREETEXT) {
    return false;
  }

  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  // Try reading as a 2-element array first (spec-compliant for all types).
  RetainPtr<const CPDF_Array> le = dict->GetArrayFor("LE");
  if (le && le->size() >= 2) {
    ByteString s_name = ReadLineEndingToken(le.Get(), 0);
    ByteString e_name = ReadLineEndingToken(le.Get(), 1);

    CPDF_Annot::LineEnding s =
        CPDF_Annot::StringToLineEnding(s_name.IsEmpty() ? "None" : s_name);
    CPDF_Annot::LineEnding e =
        CPDF_Annot::StringToLineEnding(e_name.IsEmpty() ? "None" : e_name);

    *start_style = static_cast<FPDF_ANNOT_LINE_END>(s);
    *end_style = static_cast<FPDF_ANNOT_LINE_END>(e);
    return true;
  }

  // Fall back to single name (Acrobat FreeText convention).
  ByteString name = dict->GetNameFor("LE");
  if (name.IsEmpty()) {
    return false;
  }

  *start_style = FPDF_ANNOT_LE_None;
  *end_style =
      static_cast<FPDF_ANNOT_LINE_END>(CPDF_Annot::StringToLineEnding(name));
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetVertices(FPDF_ANNOTATION annot,
                      const FS_POINTF* points,
                      unsigned long count) {
  // Accept only Polygon / Polyline annotations.
  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_POLYGON && subtype != FPDF_ANNOT_POLYLINE) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  // ───── Removal branch ────────────────────────────────────────────────
  // If caller passes nullptr or zero, delete the /Vertices array.
  if (!points || count == 0) {
    dict->RemoveFor(pdfium::annotation::kVertices);
    return true;
  }

  // ───── Replacement branch ───────────────────────────────────────────
  RetainPtr<CPDF_Array> verts =
      dict->GetMutableArrayFor(pdfium::annotation::kVertices);
  if (verts) {
    verts->Clear();
  } else {
    verts = dict->SetNewFor<CPDF_Array>(pdfium::annotation::kVertices);
  }

  // SAFETY: caller guarantees |points| has |count| entries.
  auto pts = UNSAFE_BUFFERS(pdfium::span(points, count));
  for (unsigned long i = 0; i < count; ++i) {
    verts->AppendNew<CPDF_Number>(pts[i].x);
    verts->AppendNew<CPDF_Number>(pts[i].y);
  }

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetLine(FPDF_ANNOTATION annot,
                                                      const FS_POINTF* start,
                                                      const FS_POINTF* end) {
  if (!annot || !start || !end) {
    return false;
  }

  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_LINE) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  // (Re‑)create the /L array: [ x1 y1 x2 y2 ]
  RetainPtr<CPDF_Array> line_arr =
      dict->GetMutableArrayFor(pdfium::annotation::kL);
  if (line_arr) {
    line_arr->Clear();
  } else {
    line_arr = dict->SetNewFor<CPDF_Array>(pdfium::annotation::kL);
  }

  line_arr->AppendNew<CPDF_Number>(start->x);
  line_arr->AppendNew<CPDF_Number>(start->y);
  line_arr->AppendNew<CPDF_Number>(end->x);
  line_arr->AppendNew<CPDF_Number>(end->y);

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetDefaultAppearance(FPDF_ANNOTATION annot,
                               FPDF_STANDARD_FONT font,
                               float font_size,
                               unsigned int R,
                               unsigned int G,
                               unsigned int B) {
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> annot_dict = context->GetMutableAnnotDict();
  if (!annot_dict) {
    return false;
  }

  // Allow FREETEXT, WIDGET, and REDACT annotations (all use DA for text
  // styling)
  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_FREETEXT && subtype != FPDF_ANNOT_WIDGET &&
      subtype != FPDF_ANNOT_REDACT) {
    return false;
  }

  CPDF_Document* doc = context->GetPage()->GetDocument();
  if (!doc) {
    return false;
  }

  // Validate parameters. Allow FPDF_FONT_UNKNOWN to preserve non-standard
  // fonts.
  if (font != FPDF_FONT_UNKNOWN &&
      (font < FPDF_FONT_COURIER || font > FPDF_FONT_ZAPFDINGBATS)) {
    return false;
  }
  if (font_size < 0 || R > 255 || G > 255 || B > 255) {
    return false;
  }

  auto internal_font = static_cast<CPDF_Annot::StandardFont>(font);
  return CPDF_GenerateAP::UpdateDefaultAppearance(
      doc, annot_dict.Get(), internal_font, font_size, CFX_Color(R, G, B));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetDefaultAppearanceRegisteredFont(FPDF_ANNOTATION annot,
                                             EPDF_FONT_ID font_id,
                                             float font_size,
                                             unsigned int R,
                                             unsigned int G,
                                             unsigned int B) {
  // EmbedPDF: annotation-specific bridge from public API to the registered font
  // AP pipeline. Generic EPDFFont_* registration lives in epdf_font.cpp because
  // the same registry is also used for page-rendering fallback.
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> annot_dict = context->GetMutableAnnotDict();
  if (!annot_dict) {
    return false;
  }

  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_FREETEXT && subtype != FPDF_ANNOT_WIDGET &&
      subtype != FPDF_ANNOT_REDACT) {
    return false;
  }

  CPDF_Document* doc = context->GetPage()->GetDocument();
  if (!doc) {
    return false;
  }

  if (!CFX_FontRegistry::IsValidFont(font_id) || font_size < 0 || R > 255 ||
      G > 255 || B > 255) {
    return false;
  }

  return CPDF_GenerateAP::UpdateDefaultAppearanceRegisteredFont(
      doc, annot_dict.Get(), font_id, font_size, CFX_Color(R, G, B));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetDefaultAppearance(FPDF_ANNOTATION annot,
                               FPDF_STANDARD_FONT* font,
                               float* font_size,
                               unsigned int* R,
                               unsigned int* G,
                               unsigned int* B) {
  // Standard validation for output parameters and annotation handle.
  if (!font || !font_size || !R || !G || !B) {
    return false;
  }
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context) {
    return false;
  }

  // Allow FREETEXT, WIDGET, and REDACT annotations (all use DA for text
  // styling)
  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_FREETEXT && subtype != FPDF_ANNOT_WIDGET &&
      subtype != FPDF_ANNOT_REDACT) {
    return false;
  }

  CPDF_Document* doc = context->GetPage()->GetDocument();
  if (!doc) {
    return false;
  }
  CPDF_DocumentViewScope document_view(doc);
  const CPDF_Dictionary* annot_dict = context->GetAnnotDict();
  if (!annot_dict) {
    return false;
  }

  // Get AcroForm for inherited /DA and /DR.
  const CPDF_Dictionary* root_dict = doc->GetRoot();
  RetainPtr<const CPDF_Dictionary> acroform_dict =
      root_dict ? root_dict->GetDictFor("AcroForm") : nullptr;

  // Parse the /DA string, inheriting from AcroForm if necessary.
  CPDF_DefaultAppearance da(annot_dict, acroform_dict.Get());

  // Get Font and Font Size
  std::optional<CPDF_DefaultAppearance::FontNameAndSize> font_info =
      da.GetFont();
  if (!font_info.has_value()) {
    return false;  // Hard fail: /DA string must specify a font.
  }
  *font_size = font_info.value().size;
  ByteString font_name = font_info.value().name;

  CPDF_Annot::StandardFont standard_font =
      CPDF_Annot::StringToStandardFont(font_name);
  if (standard_font == CPDF_Annot::StandardFont::kUnknown) {
    RetainPtr<const CPDF_Dictionary> inherited_dr =
        ToDictionary(CPDF_FormField::GetFieldAttrForDict(annot_dict, "DR"));
    standard_font =
        ResolveStandardFontFromDefaultResources(inherited_dr.Get(), font_name);
  }
  if (standard_font == CPDF_Annot::StandardFont::kUnknown) {
    RetainPtr<const CPDF_Dictionary> acroform_dr =
        acroform_dict ? acroform_dict->GetDictFor("DR") : nullptr;
    standard_font =
        ResolveStandardFontFromDefaultResources(acroform_dr.Get(), font_name);
  }
  *font = static_cast<FPDF_STANDARD_FONT>(standard_font);

  // Get Color (with default fallback)
  std::optional<CFX_Color::TypeAndARGB> color_info = da.GetColorARGB();
  if (color_info.has_value()) {
    // If color is found, use it.
    uint32_t argb = color_info.value().argb;
    *R = (argb >> 16) & 0xFF;
    *G = (argb >> 8) & 0xFF;
    *B = argb & 0xFF;
  } else {
    // If no color is defined in the /DA string, provide black as the default.
    *R = 0;
    *G = 0;
    *B = 0;
  }

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetTextAlignment(FPDF_ANNOTATION annot,
                           FPDF_TEXT_ALIGNMENT alignment) {
  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict) {
    return false;
  }

  // This property is valid for FreeText and Redact annotations.
  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_FREETEXT && subtype != FPDF_ANNOT_REDACT) {
    return false;
  }

  // Validate the enum range to ensure a valid value is passed.
  if (alignment < FPDF_TEXT_ALIGNMENT_LEFT ||
      alignment > FPDF_TEXT_ALIGNMENT_RIGHT) {
    return false;
  }

  // Set the /Q key in the annotation dictionary to the integer value of the
  // enum.
  annot_dict->SetNewFor<CPDF_Number>("Q", static_cast<int>(alignment));

  return true;
}

FPDF_EXPORT FPDF_TEXT_ALIGNMENT FPDF_CALLCONV
EPDFAnnot_GetTextAlignment(FPDF_ANNOTATION annot) {
  // The default alignment is left (0) if not specified.
  constexpr FPDF_TEXT_ALIGNMENT kDefaultAlignment = FPDF_TEXT_ALIGNMENT_LEFT;

  const CPDF_Dictionary* annot_dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict) {
    return kDefaultAlignment;
  }

  // This property is valid for FreeText and Redact annotations.
  FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_FREETEXT && subtype != FPDF_ANNOT_REDACT) {
    return kDefaultAlignment;
  }

  // GetIntegerFor() conveniently returns 0 if the key doesn't exist,
  // which matches the PDF specification's default.
  int alignment_value = annot_dict->GetIntegerFor("Q");

  // Validate the value is within the known enum range before casting.
  if (alignment_value >= FPDF_TEXT_ALIGNMENT_LEFT &&
      alignment_value <= FPDF_TEXT_ALIGNMENT_RIGHT) {
    return static_cast<FPDF_TEXT_ALIGNMENT>(alignment_value);
  }

  return kDefaultAlignment;
}

FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
EPDFPage_GetAnnotByName(FPDF_PAGE page, FPDF_WIDESTRING nm) {
  if (!page || !nm || !*nm) {
    return nullptr;
  }
  ScopedFPDFPageView page_view(page);
  if (!page_view) {
    return nullptr;
  }
  const CPDF_Page* pPage = page_view.Get();

  RetainPtr<const CPDF_Array> annots = pPage->GetAnnotsArray();
  if (!annots) {
    return nullptr;
  }

  WideString target = UNSAFE_BUFFERS(WideStringFromFPDFWideString(nm));

  for (size_t i = 0; i < annots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> const_dict =
        ToDictionary(annots->GetDirectObjectAt(i));
    RetainPtr<CPDF_Dictionary> d =
        pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(const_dict.Get()));
    if (d && d->GetUnicodeTextFor("NM") == target) {
      auto ctx = std::make_unique<CPDF_AnnotContext>(
          std::move(d), IPDFPageFromFPDFPage(page),
          pdfium::checked_cast<int>(i));
      return FPDFAnnotationFromCPDFAnnotContext(ctx.release());
    }
  }
  return nullptr;
}

namespace {

// EmbedPDF: annotation removal reads before it writes. Scanning /Annots for a
// match must not promote a single sibling into a layer's overlay (reads never
// promote); only the match takes mutable handles, and only for what changes:
// the page's /Annots. The removed object is deleted when it is local and left
// alone when it belongs to the base - it is never edited, so never promoted.

// The object number an /Annots entry names, without resolving it: a reference
// carries its number; an inline dictionary has none (0). Pure.
uint32_t AnnotEntryObjNum(const CPDF_Object* entry) {
  return entry && entry->IsReference() ? entry->AsReference()->GetRefObjNum()
                                       : 0;
}

// Index of the /Annots entry that is |obj_num|, or nullopt. A const scan.
std::optional<size_t> FindAnnotIndexByObjNum(const CPDF_Array* annots,
                                             uint32_t obj_num) {
  if (!annots || obj_num == 0) {
    return std::nullopt;
  }
  for (size_t i = 0; i < annots->size(); ++i) {
    if (AnnotEntryObjNum(annots->GetObjectAt(i).Get()) == obj_num) {
      return i;
    }
  }
  return std::nullopt;
}

// Same for /NM. Resolving an entry to read a name is a const resolve
// (GetDirectObjectAt -> GetIndirectObject -> the frozen base object).
std::optional<size_t> FindAnnotIndexByName(const CPDF_Array* annots,
                                           const WideString& name) {
  if (!annots) {
    return std::nullopt;
  }
  for (size_t i = 0; i < annots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> dict =
        ToDictionary(annots->GetDirectObjectAt(i));
    if (dict && dict->GetUnicodeTextFor("NM") == name) {
      return i;
    }
  }
  return std::nullopt;
}

// The one mutation: |page_dict|'s /Annots shrinks (this promotes the page,
// which is right - it changed). A local object is deleted; a base object is
// simply no longer referenced (DeleteIndirectObject is a no-op for it).
// Afterwards the page keeps the shape its BASE has: an add created /Annots
// when the base page had none, and the reverse of that add is not an empty
// array - but a page whose base carries `/Annots []` keeps it.
bool RemoveAnnotEntryAt(CPDF_Document* doc,
                        CPDF_Dictionary* page_dict,
                        size_t index) {
  if (!doc || !page_dict) {
    return false;
  }
  RetainPtr<CPDF_Array> annots = page_dict->GetMutableArrayFor("Annots");
  if (!annots || index >= annots->size()) {
    return false;
  }
  const uint32_t objnum = AnnotEntryObjNum(annots->GetObjectAt(index).Get());
  annots->RemoveAt(index);
  if (objnum) {
    doc->DeleteIndirectObject(objnum);
  }
  // The BASE twin decides: it is what a save compares against when it
  // elides the page. The loaded twin would be wrong after a reopen - the
  // delta's page carried the annotation just removed.
  if (annots->IsEmpty()) {
    RetainPtr<const CPDF_Dictionary> twin =
        ToDictionary(doc->GetBaseTwin(page_dict->GetObjNum()));
    if (!twin || !twin->KeyExist("Annots")) {
      page_dict->RemoveFor("Annots");
    }
  }
  return true;
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_RemoveAnnotByName(FPDF_PAGE page, FPDF_WIDESTRING nm) {
  if (!page || !nm || !*nm) {
    return false;
  }

  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }

  WideString target = UNSAFE_BUFFERS(WideStringFromFPDFWideString(nm));
  RetainPtr<const CPDF_Array> annots = pPage->GetAnnotsArray();
  std::optional<size_t> index = FindAnnotIndexByName(annots.Get(), target);
  if (!index.has_value()) {
    return false;
  }
  return RemoveAnnotEntryAt(pPage->GetDocument(), pPage->GetMutableDict().Get(),
                            *index);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetLinkedAnnot(FPDF_ANNOTATION annot,
                         FPDF_BYTESTRING key,
                         FPDF_ANNOTATION linked_annot) {
  if (!annot || !key) {
    return false;
  }

  CPDF_AnnotContext* src = CPDFAnnotContextFromFPDFAnnotation(annot);
  CPDF_AnnotContext* dst = CPDFAnnotContextFromFPDFAnnotation(linked_annot);
  if (!src) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> src_dict = src->GetMutableAnnotDict();
  if (!src_dict) {
    return false;
  }

  if (!linked_annot) {
    src_dict->RemoveFor(key);
    return true;
  }

  if (!dst) {
    return false;
  }

  IPDF_Page* sp = src->GetPage();
  IPDF_Page* dp = dst->GetPage();
  if (!sp || !dp) {
    return false;
  }

  CPDF_Document* doc = sp->GetDocument();
  if (doc != dp->GetDocument()) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> dst_dict = dst->GetMutableAnnotDict();
  if (!dst_dict) {
    return false;
  }

  const uint32_t objnum = EnsureIndirect(doc, dst_dict);
  if (objnum == 0) {
    return false;
  }

  src_dict->SetNewFor<CPDF_Reference>(key, doc, objnum);
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFPage_GetAnnotCountRaw(FPDF_DOCUMENT doc,
                                                        int page_index) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  if (!pdf || page_index < 0 || page_index >= pdf->GetPageCount()) {
    return 0;
  }
  CPDF_DocumentViewScope document_view(pdf);
  RetainPtr<const CPDF_Dictionary> page_dict =
      pdf->GetPageDictionary(page_index);
  if (!page_dict) {
    return 0;
  }
  RetainPtr<const CPDF_Array> annots = page_dict->GetArrayFor("Annots");
  return annots ? fxcrt::CollectionSize<int>(*annots) : 0;
}

FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
EPDFPage_GetAnnotRaw(FPDF_DOCUMENT doc, int page_index, int index) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  if (!pdf || index < 0 || page_index < 0 ||
      page_index >= pdf->GetPageCount()) {
    return nullptr;
  }
  CPDF_DocumentViewScope document_view(pdf);

  RetainPtr<const CPDF_Dictionary> const_page_dict =
      pdf->GetPageDictionary(page_index);
  RetainPtr<CPDF_Dictionary> page_dict =
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(const_page_dict.Get()));
  if (!page_dict) {
    return nullptr;
  }

  RetainPtr<const CPDF_Array> annots = page_dict->GetArrayFor("Annots");
  if (!annots || static_cast<size_t>(index) >= annots->size()) {
    return nullptr;
  }

  RetainPtr<const CPDF_Dictionary> const_annot_dict =
      ToDictionary(annots->GetDirectObjectAt(index));
  RetainPtr<CPDF_Dictionary> annot_dict =
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(const_annot_dict.Get()));
  if (!annot_dict) {
    return nullptr;
  }

  // Use the standard MakeRetain to create the page object.
  // This works because of the CONSTRUCT_VIA_MAKE_RETAIN macro in cpdf_page.h.
  auto page = pdfium::MakeRetain<CPDF_Page>(pdf, page_dict);

  // Create the context, which now takes the RetainPtr directly.
  auto ctx = std::make_unique<RawAnnotContext>(std::move(annot_dict),
                                               std::move(page), index);

  // The lifetime is now perfectly managed by smart pointers.
  return FPDFAnnotationFromCPDFAnnotContext(ctx.release());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFPage_RemoveAnnotRaw(FPDF_DOCUMENT doc,
                                                            int page_index,
                                                            int index) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  if (!pdf || page_index < 0 || page_index >= pdf->GetPageCount() ||
      index < 0) {
    return false;
  }

  // Bounds are checked on the const view; only a real removal takes the
  // mutable page dictionary.
  RetainPtr<const CPDF_Dictionary> page_view = pdf->GetPageDictionary(page_index);
  RetainPtr<const CPDF_Array> annots =
      page_view ? page_view->GetArrayFor("Annots") : nullptr;
  if (!annots || static_cast<size_t>(index) >= annots->size()) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> page_dict =
      pdf->GetMutablePageDictionary(page_index);
  return RemoveAnnotEntryAt(pdf, page_dict.Get(), static_cast<size_t>(index));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetName(FPDF_ANNOTATION annot,
                                                      FPDF_BYTESTRING name) {
  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict || !name || !*name) {
    return false;
  }
  if (!IsNameSubtype(FPDFAnnot_GetSubtype(annot))) {
    return false;
  }
  // A name object, not a string: /Name /#23LBG... is what Acrobat reads
  // back as "#LBG...". Appearance streams are deliberately left alone —
  // stamps own theirs, icon subtypes are rebaked by the caller. Any name
  // is accepted: the predefined sets are a reader obligation (ISO 32000-2
  // tables 175/184/187/188 all allow additional names).
  dict->SetNewFor<CPDF_Name>("Name", ByteString(name));
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetName(FPDF_ANNOTATION annot, char* buffer, unsigned long buflen) {
  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return 0;
  }
  const ByteString name_str = dict->GetNameFor("Name");
  if (name_str.IsEmpty()) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      name_str, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

// Where a stamp's drawing is placed: its unrotated box and the rotation, when
// /EMBD_Metadata records both, else /Rect.
struct PlacementBox {
  CFX_FloatRect rect;
  bool rotated = false;
  float degrees = 0;
};

static PlacementBox ReadPlacementBox(const CPDF_Dictionary* annot_dict) {
  float degrees = GetEmbedMetadataFloatFor(annot_dict, "Rotation");
  degrees = fmod(fmod(degrees, 360.0f) + 360.0f, 360.0f);
  const CFX_FloatRect unrotated =
      GetEmbedMetadataRectFor(annot_dict, "UnrotatedRect");
  PlacementBox box;
  box.rotated = degrees > 0.01f && degrees < 359.99f && !unrotated.IsEmpty();
  box.degrees = degrees;
  box.rect = box.rotated ? unrotated
                         : annot_dict->GetRectFor(pdfium::annotation::kRect);
  return box;
}

static bool HasArea(const PlacementBox& box) {
  return box.rect.Width() > 0 && box.rect.Height() > 0;
}

// Makes `ap`, a detached wrapper around a drawing, place the drawing's
// `content_rect` in `box` with `fit`, and sets it as the annotation's normal
// appearance: under a layer that paints /CA, below full opacity. The drawing
// itself is never written.
static bool PlaceWrapper(CPDF_AnnotContext* ctx,
                         CPDF_Dictionary* ad,
                         CPDF_Document* doc,
                         RetainPtr<CPDF_Stream> ap,
                         const CFX_FloatRect& content_rect,
                         const PlacementBox& box,
                         EPDF_STAMP_FIT fit) {
  RetainPtr<CPDF_Dictionary> ap_dict = ap ? ap->GetMutableDict() : nullptr;
  if (!ap_dict || !HasArea(box)) {
    return false;
  }
  ap_dict->SetRectFor("EPDFOrigContentRect", content_rect);

  const float box_w = box.rect.Width();
  const float box_h = box.rect.Height();
  const float orig_w = content_rect.Width();
  const float orig_h = content_rect.Height();
  if (orig_w <= 0 || orig_h <= 0) {
    return false;
  }

  // The placement matrix, from the fit.
  float drawn_w, drawn_h, dx, dy;
  if (!FitImageIntoBox(box_w, box_h, orig_w, orig_h, ToCpp(fit), &drawn_w,
                       &drawn_h, &dx, &dy)) {
    return false;
  }

  // The wrapper content stream:
  //    q sx 0 0 sy tx ty cm /EPDFWRAP Do Q
  //    Form XObjects render in their own coordinate space.  content_rect.left
  //    and .bottom are the offset of the painted content within the child form,
  //    so the translation compensates for that offset after scaling to align
  //    the visible content's origin with the target placement box.
  {
    const float sx = drawn_w / orig_w;
    const float sy = drawn_h / orig_h;
    const float tx = dx - content_rect.left * sx;
    const float ty = dy - content_rect.bottom * sy;
    fxcrt::ostringstream buf;
    buf << "q ";
    WriteFloat(buf, sx) << " 0 0 ";
    WriteFloat(buf, sy) << " ";
    WriteFloat(buf, tx) << " ";
    WriteFloat(buf, ty) << " cm /EPDFWRAP Do Q";
    ap->SetDataFromStringstreamAndRemoveFilter(&buf);
  }
  // A transparency group, so the opacity layer above it fades the drawing
  // once, as a whole: overlapping parts don't show through one another.
  if (!ap_dict->KeyExist("Group")) {
    ap_dict->SetNewFor<CPDF_Dictionary>("Group")->SetNewFor<CPDF_Name>(
        "S", "Transparency");
  }

  // The BBox is the target box.
  ap_dict->SetRectFor("BBox", CFX_FloatRect(0, 0, box_w, box_h));

  // Rotation is the AP /Matrix; clear any stale one.
  // The box turns about the origin and moves so the turned box starts there
  // too: /Rect places it on the page, and a form around the wrapper (our
  // opacity layer, or another editor's) then needs no matrix of its own.
  if (box.rotated) {
    const float theta = box.degrees * 3.14159265358979323846f / 180.0f;
    const float cos_t = cosf(theta);
    const float sin_t = sinf(theta);
    CFX_Matrix rotation(cos_t, sin_t, -sin_t, cos_t, 0, 0);
    CFX_FloatRect turned =
        rotation.TransformRect(CFX_FloatRect(0, 0, box_w, box_h));
    turned.Normalize();
    rotation.Translate(-turned.left, -turned.bottom);
    ap_dict->SetMatrixFor("Matrix", rotation);
  } else {
    ap_dict->RemoveFor("Matrix");
  }

  // Below full opacity, /CA is painted by a layer over the wrapper, in the
  // form Acrobat writes and recognises (epdf_wrapped_appearance.h).
  const float opacity = EpdfGetAnnotOpacity(ad);
  RetainPtr<CPDF_Stream> normal = ap;
  if (opacity < 1.0f) {
    doc->AddIndirectObject(ap);
    normal = EpdfNewOpacityLayer(doc, ap.Get(), opacity);
  }
  SetDetachedNormalAppearance(doc, ad, normal);
  if (ctx->HasForm()) {
    ctx->SetForm(normal);
  }
  return true;
}

// Re-fit a placed drawing's appearance into its box, painting the annotation's
// /CA. `painted_opacity` is the opacity the current appearance was painted
// with: a layer that paints it is the opacity, not part of the drawing.
static bool RefitPlacedAppearance(FPDF_ANNOTATION annot,
                                  EPDF_STAMP_FIT fit,
                                  float painted_opacity) {
  CPDF_AnnotContext* ctx = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!ctx) {
    return false;
  }

  // Stamps, and widgets whose appearance is an imported page (a signature
  // field's mark via EPDFAnnot_SetAppearanceFromPage): both are one wrapped
  // form to place inside /Rect.
  const FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
  if (subtype != FPDF_ANNOT_STAMP && subtype != FPDF_ANNOT_WIDGET) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> ad = ctx->GetMutableAnnotDict();
  if (!ad) {
    return false;
  }

  // The box: the unrotated one when rotated, otherwise /Rect.
  const PlacementBox box = ReadPlacementBox(ad.Get());
  if (!HasArea(box)) {
    return false;
  }

  // Fetch/create AP(N).
  RetainPtr<CPDF_Stream> ap =
      GetAnnotAP(ad.Get(), CPDF_Annot::AppearanceMode::kNormal);
  if (!ap) {
    CPDF_GenerateAP::GenerateEmptyAP(ctx->GetPage()->GetDocument(), ad.Get());
    ap = GetAnnotAP(ad.Get(), CPDF_Annot::AppearanceMode::kNormal);
    if (!ap) {
      return false;
    }
  }

  CPDF_Document* doc = ctx->GetPage()->GetDocument();
  if (!doc) {
    return false;
  }

  // Find our wrapper, under whatever other editors put around it, even if
  // one stripped private metadata. Edit a detached copy: mutating a shared AP
  // would also resize other annotations (or inactive states) that refer to
  // it. Publish only on success.
  CFX_FloatRect content_rect;
  if (std::optional<EpdfWrappedAppearance> ours =
          EpdfFindWrappedAppearance(ap.Get(), painted_opacity)) {
    ap = ToStream(ours->wrapper->Clone());
    content_rect = GetFormDisplayBox(ours->drawing->GetDict().Get());
  } else {
    ap = ToStream(ap->Clone());
  }
  if (!ap) {
    return false;
  }

  // Otherwise the current appearance is the drawing to wrap.
  if (content_rect.IsEmpty()) {
    content_rect = GetFormDisplayBox(ap->GetDict().Get());
    if (content_rect.IsEmpty()) {
      content_rect = GetPaintedFormBounds(doc, ap.Get());
    }
    content_rect.Normalize();
    if (content_rect.IsEmpty() || content_rect.Width() <= 0 ||
        content_rect.Height() <= 0) {
      return false;
    }

    if (!WrapAPContentIntoFormXObject(ap.Get(), doc, painted_opacity)) {
      return false;
    }
  }
  return PlaceWrapper(ctx, ad.Get(), doc, std::move(ap), content_rect, box,
                      fit);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_UpdateAppearanceToRect(FPDF_ANNOTATION annot, EPDF_STAMP_FIT fit) {
  return RefitPlacedAppearance(
      annot, fit, EpdfGetAnnotOpacity(GetAnnotDictFromFPDFAnnotation(annot)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetStampOpacity(FPDF_ANNOTATION annot,
                          EPDF_STAMP_FIT fit,
                          unsigned int alpha) {
  RetainPtr<CPDF_Dictionary> dict = GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict || alpha > 255) {
    return false;
  }
  // The appearance still paints the old value; read it with that.
  RetainPtr<const CPDF_Object> previous = dict->GetObjectFor("CA");
  const float painted_opacity = EpdfGetAnnotOpacity(dict.Get());
  if (!EPDFAnnot_SetOpacity(annot, alpha)) {
    return false;
  }
  if (RefitPlacedAppearance(annot, fit, painted_opacity)) {
    return true;
  }
  if (previous) {
    dict->SetFor("CA", previous->Clone());
  } else {
    dict->RemoveFor("CA");
  }
  return false;
}

FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
EPDFPage_CreateAnnot(FPDF_PAGE page, FPDF_ANNOTATION_SUBTYPE subtype) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage || !FPDFAnnot_IsSupportedSubtype(subtype)) {
    return nullptr;
  }

  CPDF_Document* doc = pPage->GetDocument();

  // Create the annotation dictionary as an INDIRECT object
  RetainPtr<CPDF_Dictionary> dict = doc->NewIndirect<CPDF_Dictionary>();
  dict->SetNewFor<CPDF_Name>(pdfium::annotation::kType, "Annot");
  dict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype,
                             CPDF_Annot::AnnotSubtypeToString(
                                 static_cast<CPDF_Annot::Subtype>(subtype)));

  // Append a REFERENCE to /Annots instead of the direct dict
  RetainPtr<CPDF_Array> annots = pPage->GetOrCreateAnnotsArray();
  annots->AppendNew<CPDF_Reference>(doc, dict->GetObjNum());

  // Build the public handle
  auto ctx =
      std::make_unique<CPDF_AnnotContext>(dict, IPDFPageFromFPDFPage(page));
  return FPDFAnnotationFromCPDFAnnotContext(ctx.release());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetRotate(FPDF_ANNOTATION annot,
                                                        float rotation) {
  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  if (rotation == 0.0f) {
    // 0 is the default, so remove the key to keep the PDF clean.
    dict->RemoveFor("Rotate");
  } else {
    dict->SetNewFor<CPDF_Number>("Rotate", rotation);
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_GetRotate(FPDF_ANNOTATION annot,
                                                        float* rotation) {
  if (!rotation) {
    return false;
  }

  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  *rotation = dict->GetFloatFor("Rotate");
  return true;
}

FPDF_EXPORT FPDF_ANNOT_REPLY_TYPE FPDF_CALLCONV
EPDFAnnot_GetReplyType(FPDF_ANNOTATION annot) {
  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return FPDF_ANNOT_RT_UNKNOWN;
  }

  // Read the /RT name from the annotation dictionary.
  // Per PDF spec, if RT is missing, the default is /R (Reply).
  ByteString rt_name = dict->GetNameFor("RT");
  CPDF_Annot::ReplyType rt = CPDF_Annot::StringToReplyType(rt_name);
  return static_cast<FPDF_ANNOT_REPLY_TYPE>(rt);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetReplyType(FPDF_ANNOTATION annot, FPDF_ANNOT_REPLY_TYPE rt) {
  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  // If unknown, remove the RT entry.
  if (rt == FPDF_ANNOT_RT_UNKNOWN) {
    dict->RemoveFor("RT");
    return true;
  }

  // Convert the public enum to internal and get the string representation.
  auto internal_rt = static_cast<CPDF_Annot::ReplyType>(rt);
  ByteString rt_name = CPDF_Annot::ReplyTypeToString(internal_rt);
  if (rt_name.IsEmpty()) {
    return false;
  }

  dict->SetNewFor<CPDF_Name>("RT", rt_name);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetBooleanValue(FPDF_ANNOTATION annot,
                          FPDF_BYTESTRING key,
                          FPDF_BOOL* value) {
  const CPDF_Dictionary* annot_dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict || !key || !value) {
    return false;
  }
  RetainPtr<const CPDF_Object> entry = annot_dict->GetDirectObjectFor(key);
  if (!entry || !entry->IsBoolean()) {
    return false;
  }
  *value = entry->GetInteger() != 0;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetBooleanValue(FPDF_ANNOTATION annot,
                          FPDF_BYTESTRING key,
                          FPDF_BOOL value) {
  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict || !key) {
    return false;
  }
  annot_dict->SetNewFor<CPDF_Boolean>(key, !!value);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetOverlayText(FPDF_ANNOTATION annot, FPDF_WIDESTRING text) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_REDACT) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  if (!text || !*text) {
    dict->RemoveFor("OverlayText");
    return true;
  }

  // SAFETY: required from caller.
  WideString ws = UNSAFE_BUFFERS(WideStringFromFPDFWideString(text));
  dict->SetNewFor<CPDF_String>("OverlayText", ws.AsStringView());
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetOverlayText(FPDF_ANNOTATION annot,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_REDACT) {
    return 0;
  }

  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return 0;
  }

  WideString text = dict->GetUnicodeTextFor("OverlayText");
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      text, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetOverlayTextRepeat(FPDF_ANNOTATION annot, FPDF_BOOL repeat) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_REDACT) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  if (repeat) {
    dict->SetNewFor<CPDF_Boolean>("Repeat", true);
  } else {
    dict->RemoveFor("Repeat");
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetOverlayTextRepeat(FPDF_ANNOTATION annot) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_REDACT) {
    return false;
  }

  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  return dict->GetBooleanFor("Repeat", false);
}

// A transform too close to the identity to move a drawing: what a folded
// placement reads back as once its numbers have been written and parsed.
static bool IsNearIdentity(const CFX_Matrix& matrix) {
  return fabsf(matrix.a - 1.0f) < 1e-5f && fabsf(matrix.b) < 1e-5f &&
         fabsf(matrix.c) < 1e-5f && fabsf(matrix.d - 1.0f) < 1e-5f &&
         fabsf(matrix.e) < 1e-3f && fabsf(matrix.f) < 1e-3f;
}

// A number as a file holds it: written, then read back.
static float AsWritten(float value) {
  const ByteString text = pdfium::MakeRetain<CPDF_Number>(value)->GetString();
  return pdfium::MakeRetain<CPDF_Number>(text.AsStringView())->GetNumber();
}

static CFX_Matrix AsWritten(const CFX_Matrix& matrix) {
  return CFX_Matrix(AsWritten(matrix.a), AsWritten(matrix.b),
                    AsWritten(matrix.c), AsWritten(matrix.d),
                    AsWritten(matrix.e), AsWritten(matrix.f));
}

// A new document for a drawing: no /Info, whose creation date would make
// each export different bytes.
static FPDF_DOCUMENT NewDrawingDocument() {
  FPDF_DOCUMENT handle = FPDF_CreateNewDocument();
  if (CPDF_Document* doc = CPDFDocumentFromFPDFDocument(handle)) {
    doc->DropCreatedDocumentInfo();
  }
  return handle;
}

// Saves `doc` under an identifier made from its content: a digest of every
// object in it. The same drawing is then the same bytes, on every runtime.
static bool SetContentFileIdentifier(CPDF_Document* doc) {
  fxcrt::ostringstream text;
  CPDF_StringArchiveStream archive(&text);
  for (uint32_t number = 1; number <= doc->GetLastObjNum(); ++number) {
    RetainPtr<const CPDF_Object> object = doc->GetIndirectObject(number);
    if (!object) {
      continue;
    }
    text << number << " obj ";
    if (!object->WriteTo(&archive, nullptr)) {
      return false;
    }
    text << "\n";
  }
  const auto bytes = text.str();
  std::array<uint8_t, 16> digest;
  CRYPT_MD5Generate(pdfium::as_byte_span(bytes), digest);
  auto identifier = pdfium::MakeRetain<CPDF_Array>();
  identifier->AppendNew<CPDF_String>(digest, CPDF_String::DataType::kIsHex);
  identifier->AppendNew<CPDF_String>(digest, CPDF_String::DataType::kIsHex);
  doc->SetPresetFileIdentifier(std::move(identifier));
  return true;
}

// The drawing a page holds when EPDFAnnot_ExportAppearance made it: content
// that only draws one form, `/EPDFDRAWING Do`, and no other resources. Null
// for any other page.
static RetainPtr<const CPDF_Stream> CanonicalDrawingOf(
    const CPDF_Dictionary* page_dict) {
  RetainPtr<const CPDF_Object> contents = page_dict->GetObjectFor("Contents");
  RetainPtr<const CPDF_Stream> content =
      contents ? ToStream(contents->GetDirect()) : nullptr;
  if (!content) {
    return nullptr;
  }
  auto access = pdfium::MakeRetain<CPDF_StreamAcc>(content);
  access->LoadAllDataFiltered();
  ByteString text(ByteStringView(access->GetSpan()));
  text.TrimWhitespace();
  if (text != "/EPDFDRAWING Do") {
    return nullptr;
  }
  RetainPtr<const CPDF_Dictionary> resources =
      page_dict->GetDictFor("Resources");
  if (!resources) {
    return nullptr;
  }
  {
    CPDF_DictionaryLocker locker(resources);
    for (const auto& entry : locker) {
      if (entry.first != "XObject" && entry.first != "ProcSet") {
        return nullptr;
      }
    }
  }
  RetainPtr<const CPDF_Dictionary> xobjects = resources->GetDictFor("XObject");
  if (!xobjects || xobjects->size() != 1) {
    return nullptr;
  }
  RetainPtr<const CPDF_Stream> drawing = xobjects->GetStreamFor("EPDFDRAWING");
  if (!drawing || drawing->GetDict()->GetNameFor("Subtype") != "Form") {
    return nullptr;
  }
  return drawing;
}

// The drawing page `page_index` of `src_doc` makes in `dest_doc`, as an
// object of its own: a canonical page's form (EPDFAnnot_ExportAppearance's)
// adopted as it is, or any other page's content and resources as a new form.
// A layer in the page that only paints `opacity` is left out.
static RetainPtr<CPDF_Stream> ImportPageAsDrawing(CPDF_Document* dest_doc,
                                                  CPDF_Document* src_doc,
                                                  int page_index,
                                                  float opacity) {
  RetainPtr<CPDF_Dictionary> src_page_dict =
      src_doc->GetMutablePageDictionary(page_index);
  if (!src_page_dict) {
    return nullptr;
  }

  CFX_FloatRect media_box = src_page_dict->GetRectFor("MediaBox");
  media_box.Normalize();
  if (media_box.IsEmpty()) {
    return nullptr;
  }

  // A drawing EPDFAnnot_ExportAppearance made is adopted as it is: its form
  // becomes the drawing, not the content of a new form around it. Exporting
  // it again then gives the same bytes.
  if (RetainPtr<const CPDF_Stream> canonical =
          CanonicalDrawingOf(src_page_dict.Get())) {
    AnnotAppearanceExporter exporter(dest_doc, src_doc);
    RetainPtr<CPDF_Stream> drawing = exporter.ExportFormXObject(canonical);
    if (!drawing || GetFormDisplayBox(drawing->GetDict().Get()).IsEmpty()) {
      return nullptr;
    }
    return drawing;
  }

  // Collect page content bytes (Contents can be a stream or an array of
  // streams).
  RetainPtr<const CPDF_Object> contents_obj =
      src_page_dict->GetObjectFor("Contents");
  if (!contents_obj) {
    return nullptr;
  }

  DataVector<uint8_t> content_data;
  const CPDF_Object* direct = contents_obj->GetDirect();
  if (!direct) {
    return nullptr;
  }

  if (direct->IsStream()) {
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(
        pdfium::WrapRetain(direct->AsStream()));
    acc->LoadAllDataFiltered();
    auto span = acc->GetSpan();
    content_data.assign(span.begin(), span.end());
  } else if (direct->IsArray()) {
    const CPDF_Array* arr = direct->AsArray();
    for (size_t i = 0; i < arr->size(); ++i) {
      RetainPtr<const CPDF_Stream> stream = arr->GetStreamAt(i);
      if (!stream) {
        continue;
      }
      auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(std::move(stream));
      acc->LoadAllDataFiltered();
      auto span = acc->GetSpan();
      if (!content_data.empty()) {
        content_data.push_back(' ');
      }
      content_data.insert(content_data.end(), span.begin(), span.end());
    }
  }
  if (content_data.empty()) {
    return nullptr;
  }

  // Build a Form XObject stream in the source document so that
  // AnnotAppearanceExporter can deep-clone it with all resource dependencies.
  auto xobj_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  xobj_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kType, "XObject");
  xobj_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype, "Form");
  xobj_dict->SetRectFor("BBox", media_box);

  RetainPtr<const CPDF_Dictionary> src_resources =
      src_page_dict->GetDictFor("Resources");
  if (src_resources) {
    xobj_dict->SetFor("Resources", src_resources->Clone());
  }

  auto src_stream = src_doc->NewIndirect<CPDF_Stream>(std::move(xobj_dict));
  src_stream->SetData(content_data);

  // Clone the stream (and all its resource references) into dest_doc.
  AnnotAppearanceExporter exporter(dest_doc, src_doc);
  RetainPtr<CPDF_Stream> cloned_stream = exporter.ExportFormXObject(src_stream);

  // Clean up temporary object from source document.
  src_doc->DeleteIndirectObject(src_stream->GetObjNum());

  if (!cloned_stream) {
    return nullptr;
  }

  RetainPtr<CPDF_Dictionary> cloned_dict = cloned_stream->GetMutableDict();
  if (!cloned_dict) {
    return nullptr;
  }

  if (!WrapAPContentIntoFormXObject(cloned_stream.Get(), dest_doc, opacity)) {
    return nullptr;
  }
  // The page's content and resources now live in the drawing. The form that
  // held them is left without them, and nothing refers to it.
  RetainPtr<CPDF_Dictionary> xobjects =
      cloned_dict->GetMutableDictFor("Resources")->GetMutableDictFor("XObject");
  RetainPtr<CPDF_Stream> drawing = xobjects->GetMutableStreamFor("EPDFWRAP");
  dest_doc->DeleteIndirectObject(cloned_stream->GetObjNum());
  return drawing;
}

// A wrapper around `drawing` that places it as it is, not yet an object of
// its own: what RefitPlacedAppearance fits into an annotation's box.
static RetainPtr<CPDF_Stream> NewDrawingWrapper(CPDF_Document* doc,
                                                const CPDF_Stream* drawing) {
  const CFX_FloatRect box = GetFormDisplayBox(drawing->GetDict().Get());
  if (box.IsEmpty()) {
    return nullptr;
  }
  auto dict = doc->New<CPDF_Dictionary>();
  dict->SetNewFor<CPDF_Name>(pdfium::annotation::kType, "XObject");
  dict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype, "Form");
  dict->SetRectFor("BBox", box);
  dict->SetRectFor("EPDFOrigContentRect", box);
  dict->SetNewFor<CPDF_Dictionary>("Resources")
      ->SetNewFor<CPDF_Dictionary>("XObject")
      ->SetNewFor<CPDF_Reference>("EPDFWRAP", doc, drawing->GetObjNum());
  auto wrapper = doc->New<CPDF_Stream>(std::move(dict));
  fxcrt::ostringstream buf;
  buf << "q 1 0 0 1 0 0 cm /EPDFWRAP Do Q";
  wrapper->SetDataFromStringstreamAndRemoveFilter(&buf);
  return wrapper;
}

// The drawing our wrapper places on a stamp, or null: a stamp without our
// wrapper, or not a stamp.
static RetainPtr<const CPDF_Stream> StampDrawingOf(
    const CPDF_Dictionary* annot_dict) {
  if (!annot_dict ||
      annot_dict->GetNameFor(pdfium::annotation::kSubtype) != "Stamp") {
    return nullptr;
  }
  RetainPtr<CPDF_Stream> ap =
      GetAnnotAP(annot_dict, CPDF_Annot::AppearanceMode::kNormal);
  if (!ap) {
    return nullptr;
  }
  std::optional<EpdfWrappedAppearance> ours =
      EpdfFindWrappedAppearance(ap.Get(), EpdfGetAnnotOpacity(annot_dict));
  if (!ours || ours->drawing->GetObjNum() == 0) {
    return nullptr;
  }
  return ours->drawing;
}

// `drawing` of `src_doc` as a new single-page document, the canonical page:
//   - content that is exactly `/EPDFDRAWING Do`, and no other resources;
//   - `placement` (from the drawing's space to the page's) folded into the
//     form's /Matrix, with its numbers as a file holds them;
//   - a page that is the drawing's box, `box` in the drawing's space;
//   - no /Info, no private keys, and an /ID made from the content.
// Importing it adopts the form as it is, so exporting again gives the same
// bytes.
static FPDF_DOCUMENT ExportDrawingDocument(CPDF_Document* src_doc,
                                           RetainPtr<const CPDF_Stream> drawing,
                                           const CFX_FloatRect& box,
                                           CFX_Matrix placement) {
  if (!drawing || box.IsEmpty()) {
    return nullptr;
  }
  placement.Translate(-box.left, -box.bottom);

  FPDF_DOCUMENT exported = NewDrawingDocument();
  CPDF_Document* dest_doc = CPDFDocumentFromFPDFDocument(exported);
  AnnotAppearanceExporter exporter(dest_doc, src_doc);
  RetainPtr<CPDF_Stream> form =
      dest_doc ? exporter.ExportFormXObject(drawing) : nullptr;
  if (!form) {
    if (exported) {
      FPDF_CloseDocument(exported);
    }
    return nullptr;
  }
  RetainPtr<CPDF_Dictionary> form_dict = form->GetMutableDict();
  form_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kType, "XObject");
  form_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype, "Form");
  // The canonical page draws the form and nothing else; where the drawing
  // sits is the form's own /Matrix. Importing it adopts the form as it is.
  // The matrix holds its numbers as a file does, so an export made after an
  // import computes the page from the same numbers and writes the same bytes.
  if (!IsNearIdentity(placement)) {
    CFX_Matrix matrix = form_dict->GetMatrixFor("Matrix");
    matrix.Concat(placement);
    form_dict->SetMatrixFor("Matrix", AsWritten(matrix));
  }
  // The page is the drawing's own box, however the drawing got here.
  const CFX_FloatRect page_box = GetFormDisplayBox(form_dict.Get());
  FPDF_PAGE page_handle =
      page_box.IsEmpty()
          ? nullptr
          : FPDFPage_New(exported, 0, page_box.Width(), page_box.Height());
  CPDF_Page* dest_page = page_handle ? CPDFPageFromFPDFPage(page_handle) : nullptr;
  RetainPtr<CPDF_Dictionary> page_dict =
      dest_page ? dest_page->GetMutableDict() : nullptr;
  if (!page_dict) {
    if (page_handle) {
      FPDF_ClosePage(page_handle);
    }
    FPDF_CloseDocument(exported);
    return nullptr;
  }

  RetainPtr<CPDF_Dictionary> resources =
      page_dict->SetNewFor<CPDF_Dictionary>("Resources");
  RetainPtr<CPDF_Dictionary> xobjects =
      resources->SetNewFor<CPDF_Dictionary>("XObject");
  xobjects->SetNewFor<CPDF_Reference>("EPDFDRAWING", dest_doc,
                                      form->GetObjNum());
  fxcrt::ostringstream buf;
  buf << "/EPDFDRAWING Do";
  auto contents = dest_doc->NewIndirect<CPDF_Stream>(
      pdfium::MakeRetain<CPDF_Dictionary>());
  contents->SetDataFromStringstreamAndRemoveFilter(&buf);
  page_dict->SetNewFor<CPDF_Reference>("Contents", dest_doc,
                                       contents->GetObjNum());

  // Nothing private in a copy: how our wrapper placed a drawing is ours.
  for (uint32_t number = 1; number <= dest_doc->GetLastObjNum(); ++number) {
    RetainPtr<CPDF_Object> object = dest_doc->GetMutableIndirectObject(number);
    RetainPtr<CPDF_Dictionary> dict =
        object && object->IsStream()
            ? object->AsMutableStream()->GetMutableDict()
            : ToDictionary(object);
    if (dict) {
      dict->RemoveFor("EPDFOrigContentRect");
    }
  }
  if (!SetContentFileIdentifier(dest_doc)) {
    FPDF_ClosePage(page_handle);
    FPDF_CloseDocument(exported);
    return nullptr;
  }
  FPDF_ClosePage(page_handle);
  return exported;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetAppearanceFromPage(FPDF_ANNOTATION annot,
                                FPDF_DOCUMENT src_doc_handle,
                                int page_index) {
  CPDF_AnnotContext* ctx = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!ctx) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> annot_dict = ctx->GetMutableAnnotDict();
  IPDF_Page* annot_page = ctx->GetPage();
  CPDF_Document* dest_doc = annot_page ? annot_page->GetDocument() : nullptr;
  CPDF_Document* src_doc = CPDFDocumentFromFPDFDocument(src_doc_handle);
  if (!annot_dict || !dest_doc || !src_doc) {
    return false;
  }

  RetainPtr<CPDF_Stream> drawing =
      ImportPageAsDrawing(dest_doc, src_doc, page_index,
                          EpdfGetAnnotOpacity(annot_dict.Get()));
  RetainPtr<CPDF_Stream> wrapper =
      drawing ? NewDrawingWrapper(dest_doc, drawing.Get()) : nullptr;
  if (!wrapper) {
    return false;
  }
  // The wrapper places the drawing in its own box, which a viewer maps to
  // /Rect. A caller wanting a uniform fit into the annotation's rect rewrites
  // it through EPDFAnnot_UpdateAppearanceToRect.
  dest_doc->AddIndirectObject(wrapper);
  annot_dict->GetOrCreateDictFor(pdfium::annotation::kAP)
      ->SetNewFor<CPDF_Reference>("N", dest_doc, wrapper->GetObjNum());
  return true;
}

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFAnnot_ExportAppearance(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* ctx = CPDFAnnotContextFromFPDFAnnotation(annot);
  IPDF_Page* annot_page = ctx ? ctx->GetPage() : nullptr;
  CPDF_Document* src_doc = annot_page ? annot_page->GetDocument() : nullptr;
  const CPDF_Dictionary* annot_dict = ctx ? ctx->GetAnnotDict() : nullptr;
  if (!src_doc || !annot_dict) {
    return nullptr;
  }
  RetainPtr<CPDF_Stream> ap =
      GetAnnotAP(annot_dict, CPDF_Annot::AppearanceMode::kNormal);
  if (!ap) {
    return nullptr;
  }

  // What the data doesn't describe: the drawing, and where it sits. `box` is
  // the page it is exported on; `placement` maps the drawing's space into it.
  RetainPtr<const CPDF_Stream> drawing;
  CFX_FloatRect box;
  CFX_Matrix placement;
  const float opacity = EpdfGetAnnotOpacity(annot_dict);
  if (std::optional<EpdfWrappedAppearance> ours =
          EpdfFindWrappedAppearance(ap.Get(), opacity)) {
    // Our wrapper: its placement, opacity and rotation are the data's `fit`,
    // `opacity` and `rotation`, so the drawing is the child, in its own box.
    drawing = std::move(ours->drawing);
    box = GetFormDisplayBox(drawing->GetDict().Get());
    return ExportDrawingDocument(src_doc, std::move(drawing), box, placement);
  }

  // Another producer's appearance. A layer that only repeats the annotation's
  // /CA is the data's `opacity`, so it stays out.
  if (const std::optional<EpdfOpacityLayer> layer =
          EpdfFindOpacityLayer(ap.Get(), opacity)) {
    RetainPtr<CPDF_Stream> copy = ToStream(ap->Clone());
    const ByteString content = layer->form_token + " Do";
    copy->SetData(content.unsigned_span());
    RetainPtr<CPDF_Dictionary> resources =
        copy->GetMutableDict()->GetMutableDictFor("Resources");
    if (resources) {
      RetainPtr<CPDF_Dictionary> own = ToDictionary(resources->Clone());
      RemoveExtGState(own.Get(), layer->state);
      copy->GetMutableDict()->SetFor("Resources", std::move(own));
    }
    drawing = std::move(copy);
  } else {
    drawing = ap;
  }

  // The drawing in its own box, as the appearance's /BBox and /Matrix show
  // it: the data's `rect` and `fit` (none recorded, so a fill: how ISO
  // 32000-2 12.5.5 places any appearance in /Rect) place it again. Identical
  // artwork is then the same drawing at any size.
  box = GetFormDisplayBox(drawing->GetDict().Get());

  // Our rotation metadata without our wrapper (another editor replaced the
  // appearance): the data's `rotation` will be applied again on import, so
  // it is taken out here, placed as ISO 32000-2 12.5.5 places it in /Rect and
  // turned back in the unrotated box the data describes.
  float rotate_deg = GetEmbedMetadataFloatFor(annot_dict, "Rotation");
  rotate_deg = fmod(fmod(rotate_deg, 360.0f) + 360.0f, 360.0f);
  const CFX_FloatRect unrotated =
      GetEmbedMetadataRectFor(annot_dict, "UnrotatedRect");
  if (rotate_deg > 0.01f && rotate_deg < 359.99f && !unrotated.IsEmpty()) {
    CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
    rect.Normalize();
    if (rect.IsEmpty() || box.IsEmpty()) {
      return nullptr;
    }
    const float sx = rect.Width() / box.Width();
    const float sy = rect.Height() / box.Height();
    placement = CFX_Matrix(sx, 0, 0, sy, rect.left - box.left * sx,
                           rect.bottom - box.bottom * sy);
    const float theta = -rotate_deg * 3.14159265358979323846f / 180.0f;
    const float cos_t = cosf(theta);
    const float sin_t = sinf(theta);
    const float cx = (unrotated.left + unrotated.right) / 2.0f;
    const float cy = (unrotated.bottom + unrotated.top) / 2.0f;
    placement.Concat(CFX_Matrix(cos_t, sin_t, -sin_t, cos_t,
                                cx * (1.0f - cos_t) + cy * sin_t,
                                cy * (1.0f - cos_t) - cx * sin_t));
    box = unrotated;
    box.Normalize();
  }
  return ExportDrawingDocument(src_doc, std::move(drawing), box, placement);
}

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFDoc_CanonicalDrawing(FPDF_DOCUMENT src_doc, int page_index) {
  CPDF_Document* src = CPDFDocumentFromFPDFDocument(src_doc);
  if (!src) {
    return nullptr;
  }
  // Made where a stamp would make it, in a document of its own, and exported
  // as a stamp's drawing is: the bytes a stamp made from this page exports.
  // The drawing depends on the page alone, not on any stamp's opacity.
  FPDF_DOCUMENT scratch = NewDrawingDocument();
  CPDF_Document* scratch_doc = CPDFDocumentFromFPDFDocument(scratch);
  if (!scratch_doc) {
    return nullptr;
  }
  RetainPtr<CPDF_Stream> drawing =
      ImportPageAsDrawing(scratch_doc, src, page_index, 1.0f);
  FPDF_DOCUMENT canonical =
      drawing ? ExportDrawingDocument(
                    scratch_doc, drawing,
                    GetFormDisplayBox(drawing->GetDict().Get()), CFX_Matrix())
              : nullptr;
  FPDF_CloseDocument(scratch);
  return canonical;
}

FPDF_EXPORT unsigned int FPDF_CALLCONV
EPDFDoc_ImportDrawing(FPDF_DOCUMENT document, FPDF_DOCUMENT canonical_doc) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  CPDF_Document* src = CPDFDocumentFromFPDFDocument(canonical_doc);
  if (!doc || !src || src->GetPageCount() != 1) {
    return 0;
  }
  RetainPtr<const CPDF_Dictionary> page = src->GetPageDictionary(0);
  RetainPtr<const CPDF_Stream> canonical =
      page ? CanonicalDrawingOf(page.Get()) : nullptr;
  if (!canonical) {
    return 0;
  }
  AnnotAppearanceExporter exporter(doc, src);
  RetainPtr<CPDF_Stream> drawing = exporter.ExportFormXObject(canonical);
  return drawing ? drawing->GetObjNum() : 0;
}

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFDoc_ExportDrawing(FPDF_DOCUMENT document, unsigned int drawing) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || drawing == 0) {
    return nullptr;
  }
  CPDF_DocumentViewScope document_view(doc);
  RetainPtr<const CPDF_Stream> form =
      ToStream(doc->GetOrParseIndirectObject(drawing));
  if (!form || form->GetDict()->GetNameFor("Subtype") != "Form") {
    return nullptr;
  }
  const CFX_FloatRect box = GetFormDisplayBox(form->GetDict().Get());
  return ExportDrawingDocument(doc, std::move(form), box, CFX_Matrix());
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetStampDrawings(FPDF_DOCUMENT document,
                         unsigned int* buffer,
                         unsigned long buflen) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return 0;
  }
  // The page dictionaries' /Annots, not loaded pages: no content is parsed.
  CPDF_DocumentViewScope document_view(doc);
  std::vector<uint32_t> drawings;
  std::set<uint32_t> seen;
  for (int i = 0; i < doc->GetPageCount(); ++i) {
    RetainPtr<const CPDF_Dictionary> page = doc->GetPageDictionary(i);
    RetainPtr<const CPDF_Array> annots =
        page ? page->GetArrayFor("Annots") : nullptr;
    if (!annots) {
      continue;
    }
    for (size_t j = 0; j < annots->size(); ++j) {
      RetainPtr<const CPDF_Dictionary> annot = annots->GetDictAt(j);
      RetainPtr<const CPDF_Stream> drawing = StampDrawingOf(annot.Get());
      if (drawing && seen.insert(drawing->GetObjNum()).second) {
        drawings.push_back(drawing->GetObjNum());
      }
    }
  }
  if (buffer) {
    // SAFETY: required from caller.
    auto out = UNSAFE_BUFFERS(pdfium::span(buffer, buflen));
    for (size_t i = 0; i < drawings.size() && i < out.size(); ++i) {
      out[i] = drawings[i];
    }
  }
  return static_cast<unsigned long>(drawings.size());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetStampDrawing(FPDF_ANNOTATION annot,
                          unsigned int drawing,
                          EPDF_STAMP_FIT fit) {
  CPDF_AnnotContext* ctx = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!ctx || drawing == 0 || FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_STAMP) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> ad = ctx->GetMutableAnnotDict();
  CPDF_Document* doc = ctx->GetPage() ? ctx->GetPage()->GetDocument() : nullptr;
  if (!ad || !doc) {
    return false;
  }
  RetainPtr<const CPDF_Stream> form =
      ToStream(doc->GetOrParseIndirectObject(drawing));
  if (!form || form->GetDict()->GetNameFor("Subtype") != "Form") {
    return false;
  }
  const PlacementBox box = ReadPlacementBox(ad.Get());
  RetainPtr<CPDF_Stream> wrapper = NewDrawingWrapper(doc, form.Get());
  if (!HasArea(box) || !wrapper) {
    return false;
  }
  ad->GetOrCreateDictFor(pdfium::annotation::kAP);
  return PlaceWrapper(ctx, ad.Get(), doc, std::move(wrapper),
                      GetFormDisplayBox(form->GetDict().Get()), box, fit);
}

FPDF_EXPORT unsigned int FPDF_CALLCONV
EPDFAnnot_GetStampDrawing(FPDF_ANNOTATION annot) {
  RetainPtr<const CPDF_Stream> drawing =
      StampDrawingOf(GetAnnotDictFromFPDFAnnotation(annot));
  return drawing ? drawing->GetObjNum() : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_GetRect(FPDF_ANNOTATION annot,
                                                      FS_RECTF* rect) {
  if (!FPDFAnnot_GetRect(annot, rect)) {
    return false;
  }

  // Normalize: upstream FPDFAnnot_GetRect does not normalize the rect read
  // from the dictionary. PDFs may store Rect as [x1,y1,x2,y2] with y1>y2,
  // resulting in inverted top/bottom. CFX_FloatRect::Normalize() fixes this.
  CFX_FloatRect float_rect = CFXFloatRectFromFSRectF(*rect);
  float_rect.Normalize();
  *rect = FSRectFFromCFXFloatRect(float_rect);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetRect(FPDF_ANNOTATION annot,
                                                      const FS_RECTF* rect) {
  RetainPtr<CPDF_Dictionary> annot_dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!annot_dict || !rect) {
    return false;
  }

  annot_dict->SetRectFor(pdfium::annotation::kRect,
                         CFXFloatRectFromFSRectF(*rect));
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetAPMatrix(FPDF_ANNOTATION annot,
                      FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                      const FS_MATRIX* matrix) {
  if (!matrix) {
    return false;
  }
  if (appearanceMode < 0 || appearanceMode >= FPDF_ANNOT_APPEARANCEMODE_COUNT) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  // Map mode to AP stream key: N, R, D
  static constexpr auto kModeKey = std::to_array<const char*>({"N", "R", "D"});
  RetainPtr<CPDF_Dictionary> ap =
      dict->GetMutableDictFor(pdfium::annotation::kAP);
  if (!ap) {
    return false;
  }

  RetainPtr<CPDF_Stream> stream =
      ap->GetMutableStreamFor(kModeKey[appearanceMode]);
  if (!stream) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> stream_dict = stream->GetMutableDict();
  if (!stream_dict) {
    return false;
  }

  stream_dict->SetMatrixFor("Matrix", CFXMatrixFromFSMatrix(*matrix));
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetAPMatrix(FPDF_ANNOTATION annot,
                      FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                      FS_MATRIX* matrix) {
  if (!matrix) {
    return false;
  }
  if (appearanceMode < 0 || appearanceMode >= FPDF_ANNOT_APPEARANCEMODE_COUNT) {
    return false;
  }

  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  // Map mode to AP stream key: N, R, D
  static constexpr auto kModeKey = std::to_array<const char*>({"N", "R", "D"});
  RetainPtr<const CPDF_Dictionary> ap =
      dict->GetDictFor(pdfium::annotation::kAP);
  if (!ap) {
    return false;
  }

  RetainPtr<const CPDF_Stream> stream =
      ap->GetStreamFor(kModeKey[appearanceMode]);
  if (!stream) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> stream_dict = stream->GetDict();
  if (!stream_dict) {
    return false;
  }

  *matrix = FSMatrixFromCFXMatrix(stream_dict->GetMatrixFor("Matrix"));
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFAnnot_GetAvailableAppearanceModes(FPDF_ANNOTATION annot) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return 0;
  }

  RetainPtr<const CPDF_Dictionary> pAP =
      pAnnotDict->GetDictFor(pdfium::annotation::kAP);
  if (!pAP) {
    return 0;
  }

  int modes = 0;
  if (pAP->KeyExist("N")) {
    modes |= 1;  // bit 0 = Normal
  }
  if (pAP->KeyExist("R")) {
    modes |= 2;  // bit 1 = Rollover
  }
  if (pAP->KeyExist("D")) {
    modes |= 4;  // bit 2 = Down
  }
  return modes;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_HasAppearanceStream(FPDF_ANNOTATION annot,
                              FPDF_ANNOT_APPEARANCEMODE appearanceMode) {
  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  auto mode = static_cast<CPDF_Annot::AppearanceMode>(appearanceMode);
  return !!GetAnnotAP(pAnnotDict, mode);
}

static ByteString GetMKColorKey(EPDF_MK_COLORTYPE type) {
  switch (type) {
    case EPDF_MK_COLOR_BG:
      return "BG";
    case EPDF_MK_COLOR_BC:
      return "BC";
  }
  return "BC";
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_SetMKColor(FPDF_ANNOTATION annot,
                                                         EPDF_MK_COLORTYPE type,
                                                         unsigned int R,
                                                         unsigned int G,
                                                         unsigned int B) {
  RetainPtr<CPDF_Dictionary> pAnnotDict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict || R > 255 || G > 255 || B > 255) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pMK = pAnnotDict->GetOrCreateDictFor("MK");
  ByteString key = GetMKColorKey(type);

  RetainPtr<CPDF_Array> pColor = pMK->GetMutableArrayFor(key.AsStringView());
  if (pColor) {
    pColor->Clear();
  } else {
    pColor = pMK->SetNewFor<CPDF_Array>(key);
  }

  pColor->AppendNew<CPDF_Number>(R / 255.f);
  pColor->AppendNew<CPDF_Number>(G / 255.f);
  pColor->AppendNew<CPDF_Number>(B / 255.f);

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFAnnot_GetMKColor(FPDF_ANNOTATION annot,
                                                         EPDF_MK_COLORTYPE type,
                                                         unsigned int* R,
                                                         unsigned int* G,
                                                         unsigned int* B) {
  if (!R || !G || !B) {
    return false;
  }

  const CPDF_Dictionary* pAnnotDict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> pMK = pAnnotDict->GetDictFor("MK");
  if (!pMK) {
    return false;
  }

  ByteString key = GetMKColorKey(type);
  RetainPtr<const CPDF_Array> pColor = pMK->GetArrayFor(key.AsStringView());
  if (!pColor || pColor->size() < 3) {
    return false;
  }

  *R = static_cast<unsigned int>(pColor->GetFloatAt(0) * 255.f + 0.5f);
  *G = static_cast<unsigned int>(pColor->GetFloatAt(1) * 255.f + 0.5f);
  *B = static_cast<unsigned int>(pColor->GetFloatAt(2) * 255.f + 0.5f);

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_ClearMKColor(FPDF_ANNOTATION annot, EPDF_MK_COLORTYPE type) {
  RetainPtr<CPDF_Dictionary> pAnnotDict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!pAnnotDict) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pMK = pAnnotDict->GetMutableDictFor("MK");
  if (!pMK) {
    return true;
  }

  ByteString key = GetMKColorKey(type);
  pMK->RemoveFor(key.AsStringView());

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GenerateFormFieldAP(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* pContext = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!pContext) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pAnnotDict = pContext->GetMutableAnnotDict();
  if (!pAnnotDict) {
    return false;
  }

  CPDF_Document* pDoc = pContext->GetPage()->GetDocument();
  if (!pDoc) {
    return false;
  }

  // Look for /FT on this dict or on /Parent
  ByteString ft;
  RetainPtr<const CPDF_Dictionary> pLookup = pAnnotDict;
  for (int depth = 0; depth < 32 && pLookup; depth++) {
    ByteString val = pLookup->GetNameFor("FT");
    if (!val.IsEmpty()) {
      ft = val;
      break;
    }
    pLookup = pLookup->GetDictFor("Parent");
  }

  if (ft.IsEmpty()) {
    return false;
  }

  uint32_t ff = 0;
  RetainPtr<const CPDF_Dictionary> pFfLookup = pAnnotDict;
  for (int depth = 0; depth < 32 && pFfLookup; depth++) {
    const CPDF_Object* pFfObj = pFfLookup->GetObjectFor("Ff");
    if (pFfObj) {
      ff = pFfObj->GetInteger();
      break;
    }
    pFfLookup = pFfLookup->GetDictFor("Parent");
  }

  if (ft == "Tx") {
    CPDF_GenerateAP::GenerateFormAP(pDoc, pAnnotDict.Get(),
                                    CPDF_GenerateAP::kTextField);
    return true;
  }
  if (ft == "Ch") {
    if (ff & (1 << 17)) {  // kCombo
      CPDF_GenerateAP::GenerateFormAP(pDoc, pAnnotDict.Get(),
                                      CPDF_GenerateAP::kComboBox);
    } else {
      CPDF_GenerateAP::GenerateFormAP(pDoc, pAnnotDict.Get(),
                                      CPDF_GenerateAP::kListBox);
    }
    return true;
  }
  if (ft == "Btn") {
    const bool is_pushbutton = ff & (1 << 16);
    const bool is_radio = ff & (1 << 15);
    if (is_radio) {
      CPDF_GenerateAP::GenerateRadioButtonFormAP(pDoc, pAnnotDict.Get());
    } else if (!is_pushbutton) {
      CPDF_GenerateAP::GenerateCheckboxFormAP(pDoc, pAnnotDict.Get());
    }
    return true;
  }

  return false;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetCalloutLineCount(FPDF_ANNOTATION annot) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_FREETEXT) {
    return 0;
  }

  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return 0;
  }

  RetainPtr<const CPDF_Array> cl = dict->GetArrayFor("CL");
  if (!cl) {
    return 0;
  }

  // /CL must have 4 (2 points) or 6 (3 points) numbers.
  const size_t sz = cl->size();
  if (sz == 4) {
    return 2;
  }
  if (sz >= 6) {
    return 3;
  }
  return 0;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAnnot_GetCalloutLine(FPDF_ANNOTATION annot,
                         FS_POINTF* buffer,
                         unsigned long length) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_FREETEXT) {
    return 0;
  }

  const CPDF_Dictionary* dict = GetAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return 0;
  }

  RetainPtr<const CPDF_Array> cl = dict->GetArrayFor("CL");
  if (!cl) {
    return 0;
  }

  const size_t sz = cl->size();
  unsigned long points_len = 0;
  if (sz == 4) {
    points_len = 2;
  } else if (sz >= 6) {
    points_len = 3;
  } else {
    return 0;
  }

  if (buffer && length >= points_len) {
    auto buffer_span = UNSAFE_BUFFERS(pdfium::span(buffer, length));
    for (unsigned long i = 0; i < points_len; ++i) {
      buffer_span[i].x = cl->GetFloatAt(i * 2);
      buffer_span[i].y = cl->GetFloatAt(i * 2 + 1);
    }
  }
  return points_len;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetCalloutLine(FPDF_ANNOTATION annot,
                         const FS_POINTF* points,
                         unsigned long count) {
  if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_FREETEXT) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> dict =
      GetMutableAnnotDictFromFPDFAnnotation(annot);
  if (!dict) {
    return false;
  }

  if (!points || count == 0) {
    dict->RemoveFor("CL");
    return true;
  }

  // /CL must be 2 points (4 numbers) or 3 points (6 numbers).
  if (count != 2 && count != 3) {
    return false;
  }

  RetainPtr<CPDF_Array> cl = dict->GetMutableArrayFor("CL");
  if (cl) {
    cl->Clear();
  } else {
    cl = dict->SetNewFor<CPDF_Array>("CL");
  }

  auto pts = UNSAFE_BUFFERS(pdfium::span(points, count));
  for (unsigned long i = 0; i < count; ++i) {
    cl->AppendNew<CPDF_Number>(pts[i].x);
    cl->AppendNew<CPDF_Number>(pts[i].y);
  }

  return true;
}

FPDF_EXPORT unsigned int FPDF_CALLCONV
EPDFAnnot_GetObjectNumber(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* pCtx = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!pCtx) {
    return 0;
  }
  const CPDF_Dictionary* dict = pCtx->GetAnnotDict();
  if (!dict) {
    return 0;
  }
  return dict->GetObjNum();
}

FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
EPDFPage_GetAnnotByObjectNumber(FPDF_PAGE page, unsigned int obj_num) {
  if (!page || obj_num == 0) {
    return nullptr;
  }

  ScopedFPDFPageView page_view(page);
  if (!page_view) {
    return nullptr;
  }
  const CPDF_Page* pPage = page_view.Get();

  RetainPtr<const CPDF_Array> annots = pPage->GetAnnotsArray();
  if (!annots) {
    return nullptr;
  }

  for (size_t i = 0; i < annots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> const_dict =
        ToDictionary(annots->GetDirectObjectAt(i));
    RetainPtr<CPDF_Dictionary> d =
        pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(const_dict.Get()));
    if (d && d->GetObjNum() == obj_num) {
      auto ctx = std::make_unique<CPDF_AnnotContext>(
          std::move(d), IPDFPageFromFPDFPage(page));
      return FPDFAnnotationFromCPDFAnnotContext(ctx.release());
    }
  }
  return nullptr;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFPage_RemoveAnnot(FPDF_PAGE page,
                                                         int index) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage || index < 0) {
    return false;
  }

  RetainPtr<const CPDF_Array> annots = pPage->GetAnnotsArray();
  if (!annots || static_cast<size_t>(index) >= annots->size()) {
    return false;
  }
  return RemoveAnnotEntryAt(pPage->GetDocument(), pPage->GetMutableDict().Get(),
                            static_cast<size_t>(index));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_RemoveAnnotByObjectNumber(FPDF_PAGE page, unsigned int obj_num) {
  if (!page || obj_num == 0) {
    return false;
  }

  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }

  RetainPtr<const CPDF_Array> annots = pPage->GetAnnotsArray();
  std::optional<size_t> index = FindAnnotIndexByObjNum(annots.Get(), obj_num);
  if (!index.has_value()) {
    return false;
  }
  return RemoveAnnotEntryAt(pPage->GetDocument(), pPage->GetMutableDict().Get(),
                            *index);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFPage_MoveAnnots(FPDF_PAGE page,
                                                        const int* from_indices,
                                                        int from_indices_len,
                                                        int to_index) {
  if (!page || !from_indices || from_indices_len <= 0) {
    return false;
  }

  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }

  RetainPtr<CPDF_Array> annots = pPage->GetMutableAnnotsArray();
  if (!annots) {
    return false;
  }

  const int count = static_cast<int>(annots->size());

  // 1. Validate every source index is in range and unique. Duplicate
  //    detection is O(N^2) but N is small (selection size); avoiding
  //    a hash table keeps the dependency surface minimal.
  std::vector<int> seen;
  seen.reserve(static_cast<size_t>(from_indices_len));
  for (int i = 0; i < from_indices_len; ++i) {
    int idx = from_indices[i];
    if (idx < 0 || idx >= count) {
      return false;
    }
    for (int s : seen) {
      if (s == idx) {
        return false;
      }
    }
    seen.push_back(idx);
  }
  const int post_count = count - from_indices_len;
  if (to_index < 0 || to_index > post_count) {
    return false;
  }

  // 2. Detach each entry in caller order. RetainPtr keeps the underlying
  //    CPDF_Object alive across the subsequent RemoveAt calls so the
  //    indirect annotation object survives the array relocation.
  std::vector<RetainPtr<CPDF_Object>> entries;
  entries.reserve(static_cast<size_t>(from_indices_len));
  for (int i = 0; i < from_indices_len; ++i) {
    entries.push_back(
        annots->GetMutableObjectAt(static_cast<size_t>(from_indices[i])));
  }

  // 3. Remove in descending index order so earlier indices stay valid
  //    during the loop. We do NOT call DeleteIndirectObject — this is a
  //    relocation, not a destruction.
  std::vector<int> sorted_desc(seen);
  std::sort(sorted_desc.begin(), sorted_desc.end(), std::greater<int>());
  for (int idx : sorted_desc) {
    annots->RemoveAt(static_cast<size_t>(idx));
  }

  // 4. Re-insert at to_index in original caller order. The array takes
  //    a fresh reference to each entry; our local RetainPtr drops at
  //    scope exit.
  for (int i = 0; i < from_indices_len; ++i) {
    annots->InsertAt(static_cast<size_t>(to_index + i), std::move(entries[i]));
  }
  return true;
}
