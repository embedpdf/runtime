// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFDOC_CPDF_GENERATEAP_H_
#define CORE_FPDFDOC_CPDF_GENERATEAP_H_

#include <memory>
#include <optional>

#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/widestring.h"
#include "core/fxge/cfx_color.h"
#include "core/fxge/cfx_fontregistry.h"

class CPDF_Dictionary;
class CPDF_Document;
class CPDF_Stream;
struct CPDF_RichTextDocument;
enum class BlendMode;

class CPDF_GenerateAP {
 public:
  enum FormType { kTextField, kComboBox, kListBox };

  static void GenerateFormAP(CPDF_Document* doc,
                             CPDF_Dictionary* pAnnotDict,
                             FormType type);

  // EmbedPDF: regenerate a text/combo widget appearance from display text
  // without changing the field's semantic /V value.
  static bool GenerateFormAPWithValueOverride(CPDF_Document* doc,
                                              CPDF_Dictionary* annot_dict,
                                              FormType type,
                                              const WideString& value_override);

  static void GenerateCheckboxFormAP(CPDF_Document* doc,
                                     CPDF_Dictionary* annot_dict);

  static void GenerateRadioButtonFormAP(CPDF_Document* doc,
                                        CPDF_Dictionary* annot_dict);

  static void GenerateEmptyAP(CPDF_Document* doc, CPDF_Dictionary* pAnnotDict);

  static bool GenerateAnnotAP(CPDF_Document* doc,
                              CPDF_Dictionary* pAnnotDict,
                              CPDF_Annot::Subtype subtype);

  static bool GenerateAnnotAP(CPDF_Document* doc,
                              CPDF_Dictionary* annot_dict,
                              CPDF_Annot::Subtype subtype,
                              BlendMode blend_mode);

  struct GeneratedAP {
    RetainPtr<CPDF_Stream> normal_stream;
  };

  static std::optional<GeneratedAP> GenerateEphemeralAnnotAP(
      CPDF_Document* doc,
      const CPDF_Dictionary* annot_dict,
      CPDF_Annot::Subtype subtype);

  static std::optional<GeneratedAP> GenerateEphemeralAnnotAP(
      CPDF_Document* doc,
      const CPDF_Dictionary* annot_dict,
      CPDF_Annot::Subtype subtype,
      BlendMode blend_mode);

  static std::optional<GeneratedAP> GenerateEphemeralFormAP(
      CPDF_Document* doc,
      const CPDF_Dictionary* annot_dict,
      FormType type);

  static bool CanGenerateEphemeralAnnotAP(CPDF_Annot::Subtype subtype);

  // EmbedPDF: build the final redaction overlay for a /Redact annotation as
  // an indirect Form XObject: opaque /IC fill plus the /OverlayText label per
  // /DA, /Q and /Repeat. The single source of truth for what an applied
  // redaction looks like — marking-stage AP generation bakes it as the R/D
  // and /RO streams, and the apply path synthesizes it when a file carries no
  // pre-baked /RO. Returns null when the annotation defines neither fill nor
  // label.
  static RetainPtr<CPDF_Stream> BuildRedactOverlayForm(
      CPDF_Document* doc,
      const CPDF_Dictionary* annot_dict);

  static bool GenerateDefaultAppearanceWithColor(CPDF_Document* doc,
                                                 CPDF_Dictionary* annot_dict,
                                                 const CFX_Color& color);

  static bool UpdateDefaultAppearance(CPDF_Document* doc,
                                      CPDF_Dictionary* annot_dict,
                                      CPDF_Annot::StandardFont font,
                                      float font_size,
                                      const CFX_Color& color);

  // EmbedPDF: Set FreeText DA to a registered runtime font. The actual AP path
  // later embeds a subset for only the characters used by the annotation/layer.
  static bool UpdateDefaultAppearanceRegisteredFont(
      CPDF_Document* doc,
      CPDF_Dictionary* annot_dict,
      CFX_FontRegistry::FontId font_id,
      float font_size,
      const CFX_Color& color);

  // EmbedPDF (Phase D): a FreeText appearance laid out from a rich text
  // document, in the two halves of the Phase C note §6. Prepare lays out
  // and builds the stream and its font resources off to the side: no
  // object number, no /DR change, no alias reserved. Publish adds them and
  // sets /AP /N. Dropping a prepared appearance costs the document nothing.
  struct PreparedRichFreeTextAP {
    PreparedRichFreeTextAP();
    PreparedRichFreeTextAP(PreparedRichFreeTextAP&& that) noexcept;
    PreparedRichFreeTextAP& operator=(PreparedRichFreeTextAP&& that) noexcept;
    ~PreparedRichFreeTextAP();

    std::unique_ptr<CPDF_AnnotFontMap> fonts;
    std::optional<CPDF_AnnotFontMap::PreparedFontResources> resources;
    ByteString content;                         // the whole stream
    RetainPtr<CPDF_Dictionary> graphics_state;  // direct
    CFX_Matrix matrix;
    CFX_FloatRect bbox;
    bool use_transform = false;
    ByteString da_alias;  // the /DR key the DA string should name
    float body_size = 0;  // the size laid out with (auto size resolved)
    bool degraded = false;
    bool auto_size_fell_back = false;
  };
  struct RichFreeTextRequest {
    const CPDF_RichTextDocument* document = nullptr;
    CFX_Color da_color;  // border stroke and DA colour
  };
  // The body face becomes the /DA font: its alias is chosen (not reserved)
  // and returned in |da_alias|. nullptr when the rect is empty, no face
  // resolves at all, or a font resource cannot be staged.
  static std::unique_ptr<PreparedRichFreeTextAP> PrepareRichFreeTextAP(
      CPDF_Document* doc,
      const CPDF_Dictionary* annot_dict,
      const RichFreeTextRequest& request);
  static bool PublishRichFreeTextAP(
      CPDF_Document* doc,
      CPDF_Dictionary* annot_dict,
      std::unique_ptr<PreparedRichFreeTextAP> prepared);

  CPDF_GenerateAP() = delete;
  CPDF_GenerateAP(const CPDF_GenerateAP&) = delete;
  CPDF_GenerateAP& operator=(const CPDF_GenerateAP&) = delete;
};

#endif  // CORE_FPDFDOC_CPDF_GENERATEAP_H_
