// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

// EmbedPDF: what the appearance generator's families share. Only
// core/fpdfdoc/cpdf_appearancegenerator*.cpp include it.
//
// See docs/embedpdf/fork-ledger.md, rule own-appearance-generator. EmbedPDF's
// changes are Copyright 2026 CloudPDF LTD.

#ifndef CORE_FPDFDOC_CPDF_APPEARANCEGENERATOR_INTERNAL_H_
#define CORE_FPDFDOC_CPDF_APPEARANCEGENERATOR_INTERNAL_H_

#include <cmath>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_appearancegenerator.h"
#include "core/fpdfdoc/cpdf_richtextlayout.h"
#include "core/fpdfdoc/cpvt_variabletext.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/stream_write_utils.h"
#include "core/fxcrt/widestring.h"
#include "core/fxge/cfx_color.h"
#include "core/fxge/cfx_fontregistry.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxge/dib/fx_dib.h"

class CPDF_Array;
class CPDF_Dictionary;
class CPDF_Document;
class CPDF_Stream;
class IPVT_FontMap;

namespace appearance_generator {

constexpr char kGSDictName[] = "GS";

// ISO 32000-1 Table 166: a dashed border that names no /D dashes 3 on,
// 3 off.
constexpr float kDefaultDash = 3;

enum class PaintOperation { kStroke, kFill };

constexpr float kButtLenFactor = 6.0f;

constexpr float kSlashLenFactor = 18.0f;

struct MarkupQuad {
  CFX_PointF upper_start;
  CFX_PointF upper_end;
  CFX_PointF lower_start;
  CFX_PointF lower_end;
};

enum class ArrowStyle { kOpen, kClosed };

// ISO 32000-1:2008 spec, table 166.
// ISO 32000-2:2020 spec, table 168.
struct BorderStyleInfo {
  float width = 1;
  BorderStyle style = BorderStyle::kSolid;
  std::vector<float> dash_pattern{kDefaultDash};
};

// ISO 32000-1:2008 spec, table 189.
// ISO 32000-2:2020 spec, table 192.
struct AppearanceCharacteristics {
  int rotation = 0;  // In degrees.
  CFX_Color border_color;
  CFX_Color background_color;
  // /CA, the normal caption. A check box or radio button keeps one
  // ZapfDingbats character there, naming the symbol it shows when on.
  WideString caption;
};

struct AnnotationDimensionsAndColor {
  CFX_FloatRect bbox;
  CFX_Matrix matrix;
  CFX_Color border_color;
  CFX_Color background_color;
};

// Where a box kind (square, circle, free text, caret) is drawn: its shape's
// box, and the turn that puts it on the page.
struct ShapeRotationInfo {
  CFX_FloatRect bbox;  // The shape's own box, before any turn, in page space.
  CFX_Matrix matrix;   // Turns the box about its middle; identity when upright.
  bool is_rotated;     // Whether the box is turned.
};

struct DefaultAppearanceInfo {
  ByteString font_name;
  float font_size;
  CFX_Color text_color;
};

// EmbedPDF (A1): how a /DA font name resolves against /DR.
//  - present in /DR: that dictionary, plus the registered font it stands for
//    when it is one of ours (family first, session hint second);
//  - absent but spelled like a reserved registered-font alias: the registered
//    font alone. The real /DR entry is installed by CPDF_AnnotFontMap once
//    the appearance is generated and the glyph set is known;
//  - otherwise the Helvetica fallback, as before.
struct DaFontResolution {
  RetainPtr<CPDF_Dictionary> font_dict;
  CFX_FontRegistry::FontId registered_font_id =
      CFX_FontRegistry::kInvalidFontId;
  // An alias this session reserved for a program already in the document
  // (CPDF_DocumentFonts): the program the /DA entry draws with until the
  // appearance installs its /DR entry.
  RetainPtr<CPDF_Stream> document_program;
  ByteString document_program_base_font;
  CPDF_AnnotFontSubset::FaceIdentity document_program_identity;

  // Makes |map|'s /DA entry the reserved document program, if there is one.
  void ApplyTo(CPDF_AnnotFontMap* map) const;
};

struct APGenerationTarget {
  CPDF_Document* const doc;
  CPDF_Dictionary* const persistent_annot_dict;
  std::unique_ptr<CPDF_AnnotFontMap> ephemeral_fonts;
  RetainPtr<CPDF_Stream> normal_stream;

  bool IsPersistent() const { return !!persistent_annot_dict; }
};

// cpdf_appearancegenerator_common.cpp
CFX_PointF UnitVector(const CFX_PointF& v);
float DotProduct(const CFX_PointF& lhs, const CFX_PointF& rhs);
float CrossProduct(const CFX_PointF& lhs, const CFX_PointF& rhs);
float VectorLength(const CFX_PointF& vector);
bool IsFinitePoint(const CFX_PointF& point);
CFX_PointF Midpoint(const CFX_PointF& first, const CFX_PointF& second);
ByteString BlendModeToPDFName(BlendMode bm);
BlendMode DefaultBlendModeFor(CPDF_Annot::Subtype subtype);
BorderStyleInfo GetBorderStyleInfo(const CPDF_Dictionary* border_style_dict);
AnnotationDimensionsAndColor GetAnnotationDimensionsAndColor(
    const CPDF_Dictionary* annot_dict);
ShapeRotationInfo GetShapeRotationInfo(const CPDF_Dictionary* annot_dict);
ByteString GenerateColorAP(const CFX_Color& color, PaintOperation operation);
ByteString GenerateBorderAP(const CFX_FloatRect& rect,
                            const BorderStyleInfo& border_style_info,
                            const CFX_Color& border_color);
ByteString GetColorStringWithDefault(const CPDF_Array* color_array,
                                     const CFX_Color& default_color,
                                     PaintOperation operation);
float GetBorderWidth(const CPDF_Dictionary* dict);
ByteString GetDashPatternString(const CPDF_Dictionary* dict);
ByteString GetPaintOperatorString(bool is_stroke_rect, bool is_fill_rect);
RetainPtr<CPDF_Dictionary> GenerateExtGStateDict(
    const CPDF_Dictionary& annot_dict,
    const ByteString& blend_mode);
RetainPtr<CPDF_Dictionary> GenerateResourcesDict(
    CPDF_Document* doc,
    RetainPtr<CPDF_Dictionary> gs_dict,
    RetainPtr<CPDF_Dictionary> font_resource_dict);
bool GenerateAPDict(APGenerationTarget* target,
                    const CPDF_Dictionary* annot_dict,
                    fxcrt::ostringstream* app_stream,
                    RetainPtr<CPDF_Dictionary> resource_dict,
                    bool is_text_markup_annotation,
                    const CFX_Matrix& matrix,
                    const CFX_FloatRect& bbox_override);
bool GenerateAndSetAPDict(APGenerationTarget* target,
                          const CPDF_Dictionary* annot_dict,
                          fxcrt::ostringstream* app_stream,
                          RetainPtr<CPDF_Dictionary> resource_dict,
                          bool is_text_markup_annotation);
bool GenerateAndSetBoxAPDict(APGenerationTarget* target,
                             const CPDF_Dictionary* annot_dict,
                             fxcrt::ostringstream* app_stream,
                             RetainPtr<CPDF_Dictionary> resource_dict,
                             const CFX_Matrix& matrix,
                             const CFX_FloatRect& footprint);
bool GenerateAndSetPointsAPDict(APGenerationTarget* target,
                                const CPDF_Dictionary* annot_dict,
                                fxcrt::ostringstream* app_stream,
                                RetainPtr<CPDF_Dictionary> resource_dict,
                                pdfium::span<const CFX_PointF> points);
bool GenerateAndSetAPDict(CPDF_Document* doc,
                          CPDF_Dictionary* annot_dict,
                          fxcrt::ostringstream* app_stream,
                          RetainPtr<CPDF_Dictionary> resource_dict,
                          bool is_text_markup_annotation);

// cpdf_appearancegenerator_text.cpp
ByteString StringFromFontNameAndSize(const ByteString& font_name,
                                     float font_size);
ByteString DefaultAppearanceFontPart(const ByteString& font_name,
                                     float font_size);
void SetVtFontSize(float font_size, CPVT_VariableText& vt);
std::optional<DefaultAppearanceInfo> GetDefaultAppearanceInfo(
    const CPDF_Dictionary* annot_dict,
    const CPDF_Dictionary* acroform_dict);
bool CloneResourcesDictIfMissingFromStream(CPDF_Dictionary* stream_dict,
                                           const CPDF_Dictionary* dr_dict);
bool ValidateOrCreateFontResources(CPDF_Document* doc,
                                   CPDF_Dictionary* stream_dict,
                                   const CPDF_Dictionary* font_dict,
                                   const ByteString& font_name);
ByteString GenerateEditAP(IPVT_FontMap* font_map,
                          CPVT_VariableText::Iterator* vt_iterator,
                          const CFX_PointF& offset,
                          bool use_continuous_formatting,
                          uint16_t sub_word);
CPDF_Annot::VerticalAlignment GetVerticalAlign(
    const CPDF_Dictionary* annot_dict);
RetainPtr<CPDF_Dictionary> GenerateFallbackFontDict(CPDF_Document* doc);
RetainPtr<CPDF_Dictionary> GenerateDirectFallbackFontDict();
RetainPtr<CPDF_Dictionary> GenerateEphemeralDefaultAcroFormDict();
RetainPtr<CPDF_Dictionary> GetFontFromDrFontDictOrGenerateFallback(
    CPDF_Document* doc,
    CPDF_Dictionary* dr_font_dict,
    const ByteString& font_name);
DaFontResolution ResolveDaFontForPersistentTarget(CPDF_Document* doc,
                                                  CPDF_Dictionary* dr_font_dict,
                                                  const ByteString& font_name);
DaFontResolution ResolveDaFontForEphemeralTarget(
    const CPDF_Document* doc,
    const CPDF_Dictionary* dr_font_dict,
    const ByteString& font_name);
RetainPtr<CPDF_Dictionary> GenerateResourceFontDict(CPDF_Document* doc,
                                                    const ByteString& font_name,
                                                    uint32_t font_dict_obj_num);
RetainPtr<CPDF_Dictionary> GenerateResourceFontDict(
    CPDF_Document* doc,
    const ByteString& font_name,
    const CPDF_Dictionary* font_dict);
ByteString EnsureFontInAcroFormDR(CPDF_Document* doc,
                                  CPDF_Dictionary* acroform_dict,
                                  const ByteString& base_font_name);

// cpdf_appearancegenerator_markup.cpp
std::optional<MarkupQuad> ReadMarkupQuad(const CPDF_Array* array, size_t index);
bool IsAxisAlignedZigZag(const MarkupQuad& quad);
bool GenerateHighlightAP(APGenerationTarget* target,
                         CPDF_Dictionary* annot_dict,
                         const ByteString& blend_name);
bool GenerateUnderlineAP(APGenerationTarget* target,
                         CPDF_Dictionary* annot_dict,
                         const ByteString& blend_name);
bool GenerateSquigglyAP(APGenerationTarget* target,
                        CPDF_Dictionary* annot_dict,
                        const ByteString& blend_name);
bool GenerateStrikeOutAP(APGenerationTarget* target,
                         CPDF_Dictionary* annot_dict,
                         const ByteString& blend_name);

// cpdf_appearancegenerator_shapes.cpp
ByteString ReadLineEndingToken(const CPDF_Array* le, size_t idx);
void EmitArrowPath(fxcrt::ostringstream& out,
                   float stroke_w,
                   ArrowStyle style,
                   bool do_fill);
void EmitCirclePath(fxcrt::ostringstream& out, float stroke_w, bool filled);
void EmitSquarePath(fxcrt::ostringstream& out, float stroke_w, bool filled);
void EmitDiamondPath(fxcrt::ostringstream& out, float stroke_w, bool filled);
void EmitButtOrSlashPath(fxcrt::ostringstream& out,
                         float stroke_w,
                         float len_factor);
bool GenerateCircleAP(APGenerationTarget* target,
                      CPDF_Dictionary* annot_dict,
                      const ByteString& blend_name);
bool GeneratePolygonAP(APGenerationTarget* target,
                       CPDF_Dictionary* annot_dict,
                       const ByteString& blend_name);
bool GenerateLineAP(APGenerationTarget* target,
                    CPDF_Dictionary* annot_dict,
                    const ByteString& blend_name);
bool GeneratePolyLineAP(APGenerationTarget* target,
                        CPDF_Dictionary* annot_dict,
                        const ByteString& blend_name);
bool GenerateInkAP(APGenerationTarget* target,
                   CPDF_Dictionary* annot_dict,
                   const ByteString& blend_name);
bool GenerateSquareAP(APGenerationTarget* target,
                      CPDF_Dictionary* annot_dict,
                      const ByteString& blend_name);

// cpdf_appearancegenerator_icons.cpp
bool GenerateTextAP(APGenerationTarget* target,
                    CPDF_Dictionary* annot_dict,
                    const ByteString& blend_name);
bool GenerateFileAttachmentAP(APGenerationTarget* target,
                              CPDF_Dictionary* annot_dict,
                              const ByteString& blend_name);
bool GeneratePopupAP(CPDF_Document* doc,
                     CPDF_Dictionary* annot_dict,
                     const ByteString& blend_name);
bool GenerateLinkAP(APGenerationTarget* target,
                    CPDF_Dictionary* annot_dict,
                    const ByteString& blend_name);
bool GenerateCaretAP(APGenerationTarget* target,
                     CPDF_Dictionary* annot_dict,
                     const ByteString& blend_name);

// cpdf_appearancegenerator_freetext.cpp
void EmitRichTextBody(fxcrt::ostringstream& s,
                      const CPDF_RichTextLayout::Result& layout,
                      CPDF_AnnotFontMap& map,
                      const CFX_FloatRect& area);
bool GenerateFreeTextAP(APGenerationTarget* target,
                        CPDF_Dictionary* annot_dict,
                        const ByteString& blend_name);

// cpdf_appearancegenerator_forms.cpp
AppearanceCharacteristics GetAppearanceCharacteristics(
    const CPDF_Dictionary* mk_dict);
std::optional<CPDF_AppearanceGenerator::FormType> GetWidgetFormType(
    const CPDF_Dictionary* annot_dict);

// cpdf_appearancegenerator_redaction.cpp
bool GenerateRedactAP(APGenerationTarget* target,
                      CPDF_Dictionary* annot_dict,
                      const ByteString& blend_name);

// Produce “q … Q” wrapper that translates + rotates local path so that:
///   • the **tip** of the ending sits at `pos`
///   • the **x‑axis** of the local coord points along the segment direction
template <typename F>
void EmitEndingWithAngle(fxcrt::ostringstream& out,
                         const CFX_PointF& pos,
                         float final_angle_rad,
                         const F& emitter) {
  const float cos_a = cos(final_angle_rad);
  const float sin_a = sin(final_angle_rad);

  // WriteMatrix, never raw `<<`: an axis-aligned segment has cos ≈ ±4.4e-8
  // and default ostream float formatting would emit it in scientific
  // notation — not legal PDF number syntax (Acrobat rejects the file).
  out << "q ";
  WriteMatrix(out, CFX_Matrix(cos_a, sin_a, -sin_a, cos_a, pos.x, pos.y))
      << " cm\n";
  emitter();
  out << "Q\n";
}

}  // namespace appearance_generator

#endif  // CORE_FPDFDOC_CPDF_APPEARANCEGENERATOR_INTERNAL_H_
