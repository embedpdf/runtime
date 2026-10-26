// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

// EmbedPDF: the appearance generator's redaction marks, and the overlay a
// redaction leaves behind.
//
// See docs/embedpdf/fork-ledger.md, rule own-appearance-generator. EmbedPDF's
// changes are Copyright 2026 CloudPDF LTD.

#include <cmath>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "constants/annotation_common.h"
#include "core/fxcrt/stream_write_utils.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_appearancegenerator_internal.h"
#include "core/fpdfdoc/cpdf_color_utils.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fpdfdoc/cpvt_variabletext.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxge/cfx_fontregistry.h"

using appearance_generator::APGenerationTarget;
using appearance_generator::DaFontResolution;
using appearance_generator::DefaultAppearanceInfo;
using appearance_generator::GenerateColorAP;
using appearance_generator::GenerateEditAP;
using appearance_generator::GenerateExtGStateDict;
using appearance_generator::GenerateFallbackFontDict;
using appearance_generator::GenerateResourcesDict;
using appearance_generator::GetBorderWidth;
using appearance_generator::GetColorStringWithDefault;
using appearance_generator::GetDashPatternString;
using appearance_generator::GetDefaultAppearanceInfo;
using appearance_generator::IsAxisAlignedZigZag;
using appearance_generator::kGSDictName;
using appearance_generator::MarkupQuad;
using appearance_generator::PaintOperation;
using appearance_generator::ReadMarkupQuad;
using appearance_generator::ResolveDaFontForPersistentTarget;
using appearance_generator::SetVtFontSize;

namespace {

// EmbedPDF: the regions a /Redact annotation targets — /QuadPoints quads when
// present (text redactions), else the annotation /Rect (area redactions).
// One overlay region: the AABB (label layout + area marks) plus the exact
// oriented quad when the marked cell is not axis-aligned (rotated text marks
// fill their true cells; the label tiling stays axis-aligned in the bbox,
// matching the live scene preview).
struct RedactOverlayRegion {
  CFX_FloatRect bbox;
  std::optional<MarkupQuad> quad;
};

bool RedactQuadValuesAreFinite(const CPDF_Array* array, size_t index) {
  const size_t offset = index * 8;
  for (size_t i = 0; i < 8; ++i) {
    if (!std::isfinite(array->GetFloatAt(offset + i))) {
      return false;
    }
  }
  return true;
}

std::vector<RedactOverlayRegion> GetRedactOverlayRegions(
    const CPDF_Dictionary* annot_dict) {
  std::vector<RedactOverlayRegion> regions;
  RetainPtr<const CPDF_Array> quad_points_array =
      annot_dict->GetArrayFor("QuadPoints");
  if (quad_points_array && quad_points_array->size() >= 8) {
    const size_t quad_count =
        CPDF_Annot::QuadPointCount(quad_points_array.Get());
    bool invalid_quad = false;
    for (size_t i = 0; i < quad_count; ++i) {
      if (!RedactQuadValuesAreFinite(quad_points_array.Get(), i)) {
        invalid_quad = true;
        break;
      }
      std::optional<MarkupQuad> quad =
          ReadMarkupQuad(quad_points_array.Get(), i);
      CFX_FloatRect rect = CPDF_Annot::RectFromQuadPoints(annot_dict, i);
      rect.Normalize();
      if (rect.IsEmpty()) {
        invalid_quad = true;
        break;
      }
      RedactOverlayRegion region;
      region.bbox = rect;
      if (quad && !IsAxisAlignedZigZag(*quad)) {
        region.quad = quad;
      }
      regions.push_back(region);
    }
    if (!invalid_quad && !regions.empty()) {
      return regions;
    }
    regions.clear();
  }
  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  rect.Normalize();
  if (!rect.IsEmpty()) {
    RedactOverlayRegion region;
    region.bbox = rect;
    regions.push_back(region);
  }
  return regions;
}

// EmbedPDF: the marking-stage /AP BBox and the overlay BBox share this rule:
// quad bounding box for text redactions, /Rect for area redactions.
CFX_FloatRect GetRedactOverlayBBox(const CPDF_Dictionary* annot_dict) {
  const std::vector<RedactOverlayRegion> regions =
      GetRedactOverlayRegions(annot_dict);
  if (regions.empty()) {
    return CFX_FloatRect();
  }
  CFX_FloatRect bbox = regions.front().bbox;
  for (size_t i = 1; i < regions.size(); ++i) {
    bbox.Union(regions[i].bbox);
  }
  return bbox;
}

RetainPtr<CPDF_Dictionary> MakeRedactFormDict(
    const CFX_FloatRect& bbox,
    RetainPtr<CPDF_Dictionary> resources) {
  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  stream_dict->SetNewFor<CPDF_Number>("FormType", 1);
  stream_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  stream_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  stream_dict->SetMatrixFor("Matrix", CFX_Matrix());
  stream_dict->SetRectFor("BBox", bbox);
  if (resources) {
    stream_dict->SetFor("Resources", std::move(resources));
  }
  return stream_dict;
}

RetainPtr<CPDF_Stream> MakeRedactFormStream(
    CPDF_Document* doc,
    const CFX_FloatRect& bbox,
    RetainPtr<CPDF_Dictionary> resources,
    fxcrt::ostringstream* ops) {
  auto stream = doc->NewIndirect<CPDF_Stream>(
      MakeRedactFormDict(bbox, std::move(resources)));
  stream->SetDataFromStringstream(ops);
  return stream;
}

constexpr float kRedactRepeatFallbackFontSize = 12.0f;

constexpr int kRedactMaxRepeatDoublings = 10;  // 2^10 = 1024 label instances

// EmbedPDF: lay out the /OverlayText label for one redacted region with the
// same CPVT + annotation-font-map stack as FreeText appearances, so /DA fonts
// (standard, DR-resolved, or registered runtime fonts) shape and subset
// identically. Top-aligned in reading order; ISO 32000-2 prescribes neither
// the vertical placement nor the /Repeat tiling, so /Repeat is expressed as
// "repeat the label, space-joined, wrapped to the region, clipped".
void AppendRedactLabelForRegion(CPDF_AnnotFontMap& map,
                                const CPDF_Dictionary* annot_dict,
                                const WideString& overlay_text,
                                float da_font_size,
                                const CFX_Color& label_color,
                                const CFX_FloatRect& region,
                                fxcrt::ostringstream& stream) {
  CPVT_VariableText::Provider provider(&map);
  CPVT_VariableText vt(&provider);
  vt.SetPlateRect(region);
  vt.SetAlignment(
      CPVT_VariableText::ToAlignment(annot_dict->GetIntegerFor("Q")));

  const bool repeat = annot_dict->GetBooleanFor("Repeat", false);
  // A label shown once at auto-size (0) fits the region on one line: CPVT
  // auto-sizes multi-line text only up to 12pt (the rule for multi-line form
  // fields), which would leave a large region mostly empty. A label at a set
  // size, or a repeated one, wraps to the region.
  const bool fit_one_line = !repeat && FXSYS_IsFloatZero(da_font_size);
  vt.SetMultiLine(!fit_one_line);
  vt.SetAutoReturn(!fit_one_line);
  // CPVT auto-sizing fits ALL text into the plate, so it cannot combine with
  // /Repeat (more repetitions would only shrink the font); pin a concrete
  // size for the repeat case when /DA asks for auto (size 0).
  float font_size = da_font_size;
  if (repeat && FXSYS_IsFloatZero(font_size)) {
    font_size = kRedactRepeatFallbackFontSize;
  }
  SetVtFontSize(font_size, vt);
  vt.Initialize();
  vt.SetText(overlay_text);
  vt.RearrangeAll();

  if (repeat) {
    // Double the space-joined text until the wrapped layout covers the region
    // vertically; the last row's surplus is removed by the region clip below.
    // Bounded to keep tiny-font/huge-region combinations sane.
    WideString tiled = overlay_text;
    for (int i = 0; i < kRedactMaxRepeatDoublings &&
                    vt.GetContentRect().Height() < region.Height();
         ++i) {
      WideString doubled = tiled;
      doubled += L' ';
      doubled += tiled;
      tiled = std::move(doubled);
      vt.SetText(tiled);
      vt.RearrangeAll();
    }
  }

  const ByteString body =
      GenerateEditAP(vt.GetProvider()->GetFontMap(), vt.GetIterator(),
                     CFX_PointF(0.0f, 0.0f), /*continuous=*/true,
                     /*sub_word=*/0);
  if (body.IsEmpty()) {
    return;
  }
  stream << "q\n";
  WriteRect(stream, region) << " re W n\n";
  stream << "BT\n"
         << GenerateColorAP(label_color, PaintOperation::kFill) << body
         << "ET\nQ\n";
}

// EmbedPDF: emit the final ("post-apply") overlay ops for a /Redact
// annotation: opaque /IC fill of every region, then the /OverlayText label.
// The marking-stage /CA opacity is deliberately not carried over — the
// content underneath is destroyed, so the replacement marking paints opaque,
// matching Acrobat. Returns false when the annotation defines neither a fill
// nor a label.
bool AppendRedactOverlayOps(CPDF_Document* doc,
                            const CPDF_Dictionary* annot_dict,
                            fxcrt::ostringstream& stream,
                            RetainPtr<CPDF_Dictionary>* out_font_resources) {
  const WideString overlay_text = annot_dict->GetUnicodeTextFor("OverlayText");
  RetainPtr<const CPDF_Array> interior_color = annot_dict->GetArrayFor("IC");
  const bool has_fill = interior_color && !interior_color->IsEmpty();
  if (!has_fill && overlay_text.IsEmpty()) {
    return false;
  }

  const std::vector<RedactOverlayRegion> regions =
      GetRedactOverlayRegions(annot_dict);
  if (regions.empty()) {
    return false;
  }

  if (has_fill) {
    stream << GetColorStringWithDefault(interior_color.Get(),
                                        CFX_Color::MakeTransparent(),
                                        PaintOperation::kFill);
    for (const RedactOverlayRegion& region : regions) {
      if (region.quad.has_value()) {
        // Fill the exact oriented cell (ring order, like the highlight AP).
        WritePoint(stream, region.quad->upper_start) << " m ";
        WritePoint(stream, region.quad->upper_end) << " l ";
        WritePoint(stream, region.quad->lower_end) << " l ";
        WritePoint(stream, region.quad->lower_start) << " l h f\n";
      } else {
        WriteRect(stream, region.bbox) << " re f\n";
      }
    }
  }

  if (overlay_text.IsEmpty()) {
    return true;
  }

  // /DA resolution mirrors the FreeText persistent path, but is forgiving:
  // redact annotations marked by other producers can lack /DA (ISO requires
  // it alongside /OverlayText, but such files exist) or an AcroForm /DR —
  // fall back to Helvetica rather than dropping the label.
  RetainPtr<CPDF_Dictionary> root_dict = doc->GetMutableRoot();
  RetainPtr<CPDF_Dictionary> form_dict;
  if (root_dict) {
    form_dict = root_dict->GetMutableDictFor("AcroForm");
    if (!form_dict) {
      form_dict = CPDF_InteractiveForm::InitAcroFormDict(doc);
    }
  }

  std::optional<DefaultAppearanceInfo> da_info =
      form_dict ? GetDefaultAppearanceInfo(annot_dict, form_dict.Get())
                : std::nullopt;
  // A /DA without a Tf parses as a font with no name: Helvetica too (files
  // from other producers, and marks written before /DA kept the font at
  // auto-size).
  const ByteString font_name =
      da_info.has_value() && !da_info.value().font_name.IsEmpty()
          ? da_info.value().font_name
          : ByteString("Helv");
  const float da_font_size =
      da_info.has_value() ? da_info.value().font_size : 0.0f;

  CFX_Color label_color =
      da_info.has_value() ? da_info.value().text_color : CFX_Color();
  if (label_color.nColorType == CFX_Color::Type::kTransparent) {
    // Legacy EmbedPDF v2 files carry the label colour in /OC; ISO keeps it
    // in the /DA string. Default to black when neither is present.
    RetainPtr<const CPDF_Array> oc = annot_dict->GetArrayFor("OC");
    label_color = (oc && oc->size() >= 3) ? fpdfdoc::CFXColorFromArray(*oc)
                                          : CFX_Color::MakeRGB(0, 0, 0);
  }

  DaFontResolution da_font;
  if (form_dict) {
    RetainPtr<CPDF_Dictionary> dr_font_dict =
        form_dict->GetOrCreateDictFor("DR")->GetOrCreateDictFor("Font");
    da_font =
        ResolveDaFontForPersistentTarget(doc, dr_font_dict.Get(), font_name);
  } else {
    da_font.font_dict = GenerateFallbackFontDict(doc);
  }
  RetainPtr<CPDF_Font> default_font =
      da_font.font_dict
          ? CPDF_DocPageData::FromDocument(doc)->GetFont(da_font.font_dict)
          : nullptr;
  if (!default_font &&
      da_font.registered_font_id == CFX_FontRegistry::kInvalidFontId) {
    return has_fill;
  }

  CPDF_AnnotFontMap map(doc, std::move(default_font), font_name,
                        /*allow_registered_fallbacks=*/true,
                        da_font.registered_font_id,
                        /*install_dr_entry=*/true);
  if (!map.HasDefaultFont()) {
    return has_fill;
  }
  for (const RedactOverlayRegion& region : regions) {
    AppendRedactLabelForRegion(map, annot_dict, overlay_text, da_font_size,
                               label_color, region.bbox, stream);
  }
  *out_font_resources = map.CreateFontResourceDict();
  return !!*out_font_resources;
}

}  // namespace

namespace appearance_generator {

bool GenerateRedactAP(APGenerationTarget* target,
                      CPDF_Dictionary* annot_dict,
                      const ByteString& blend_name) {
  CPDF_Document* const doc = target->doc;
  // Normal (marking-stage) appearance: border-only outline in /C, default
  // red. The filled preview is NOT drawn here — R/D and /RO all share the
  // final overlay from BuildRedactOverlayForm, so hovering a marked
  // redaction previews exactly what apply will paint.
  fxcrt::ostringstream normal_stream;
  normal_stream << "/" << kGSDictName << " gs ";
  normal_stream << GetColorStringWithDefault(
      annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
      CFX_Color::MakeRGB(1, 0, 0),  // default: red
      PaintOperation::kStroke);

  const float border_width = GetBorderWidth(annot_dict);
  if (border_width > 0) {
    normal_stream << border_width << " w ";
    normal_stream << GetDashPatternString(annot_dict);
    for (const RedactOverlayRegion& region :
         GetRedactOverlayRegions(annot_dict)) {
      CFX_FloatRect stroke_rect = region.bbox;
      stroke_rect.Deflate(border_width / 2, border_width / 2);
      normal_stream << stroke_rect.left << " " << stroke_rect.bottom << " "
                    << stroke_rect.Width() << " " << stroke_rect.Height()
                    << " re S\n";
    }
  }

  auto gs_dict = GenerateExtGStateDict(*annot_dict, blend_name);
  auto resources_dict = GenerateResourcesDict(doc, std::move(gs_dict), nullptr);
  const CFX_FloatRect bbox = GetRedactOverlayBBox(annot_dict);
  if (!target->IsPersistent()) {
    // EmbedPDF: drawing a mark without an appearance (one another producer
    // wrote) builds the outline in memory and writes nothing: not the
    // appearance streams, /AP or /RO.
    target->normal_stream = pdfium::MakeRetain<CPDF_Stream>(
        MakeRedactFormDict(bbox, std::move(resources_dict)));
    target->normal_stream->SetDataFromStringstream(&normal_stream);
    return true;
  }
  RetainPtr<CPDF_Stream> normal_pdf_stream = MakeRedactFormStream(
      doc, bbox, std::move(resources_dict), &normal_stream);

  // Rollover/Down and /RO share the final overlay (fill + label).
  RetainPtr<CPDF_Stream> overlay =
      CPDF_AppearanceGenerator::BuildRedactOverlayForm(doc, annot_dict);
  if (!overlay) {
    // Neither /IC nor /OverlayText: keep the AP structure (and the baked /RO
    // that pre-v3 clients flatten on apply) with an empty overlay.
    fxcrt::ostringstream empty_stream;
    overlay = MakeRedactFormStream(doc, bbox, nullptr, &empty_stream);
  }

  RetainPtr<CPDF_Dictionary> ap_dict =
      annot_dict->GetOrCreateDictFor(pdfium::annotation::kAP);
  ap_dict->SetNewFor<CPDF_Reference>("N", doc, normal_pdf_stream->GetObjNum());
  ap_dict->SetNewFor<CPDF_Reference>("R", doc, overlay->GetObjNum());
  ap_dict->SetNewFor<CPDF_Reference>("D", doc, overlay->GetObjNum());

  // /RO lives on the annotation dict, not inside /AP.
  annot_dict->SetNewFor<CPDF_Reference>("RO", doc, overlay->GetObjNum());
  return true;
}

}  // namespace appearance_generator

// static
RetainPtr<CPDF_Stream> CPDF_AppearanceGenerator::BuildRedactOverlayForm(
    CPDF_Document* doc,
    const CPDF_Dictionary* annot_dict) {
  if (!doc || !annot_dict) {
    return nullptr;
  }
  fxcrt::ostringstream ops;
  RetainPtr<CPDF_Dictionary> font_resources;
  if (!AppendRedactOverlayOps(doc, annot_dict, ops, &font_resources)) {
    return nullptr;
  }
  RetainPtr<CPDF_Dictionary> resources;
  if (font_resources) {
    resources = GenerateResourcesDict(doc, nullptr, std::move(font_resources));
  }
  return MakeRedactFormStream(doc, GetRedactOverlayBBox(annot_dict),
                              std::move(resources), &ops);
}
