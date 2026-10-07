// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "public/fpdf_flatten.h"

#include <limits.h>

#include <algorithm>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "constants/annotation_common.h"
#include "constants/annotation_flags.h"
#include "constants/font_encodings.h"
#include "constants/page_object.h"
#include "core/fpdfapi/edit/cpdf_contentstream_write_utils.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_appearance_exporter.h"
#include "fpdfsdk/epdf_form_helpers.h"
#include "fpdfsdk/epdf_object_helpers.h"
#include "public/fpdf_edit.h"

namespace {

struct FlattenCandidate {
  size_t annotation_index = 0;
  uint32_t annotation_object_number = 0;
  RetainPtr<const CPDF_Dictionary> annotation;
  RetainPtr<const CPDF_Stream> appearance;
  CFX_FloatRect annotation_rect;
  CFX_FloatRect appearance_rect;
  CFX_Matrix appearance_matrix;
};

struct FlattenTarget {
  uint32_t annotation_object_number = 0;
  int annotation_index = -1;
  const CPDF_Dictionary* annotation = nullptr;
};

// Outcome slots in FlattenPlan::target_outcomes.
constexpr int kTargetSkipped = -1;    // on the page, but not a candidate
constexpr int kTargetNotOnPage = -2;  // never matched an /Annots entry

struct FlattenPlan {
  std::vector<FlattenCandidate> candidates;
  // One entry per requested target: the index into `candidates` when
  // eligible, or one of the kTarget* sentinels. Empty for a whole-page plan.
  std::vector<int> target_outcomes;
};

struct PreparedAppearance {
  FlattenCandidate candidate;
  RetainPtr<CPDF_Stream> stream;  // the appearance, living in the TARGET doc
};

bool IsValidUsage(int usage) {
  return usage == FLAT_NORMALDISPLAY || usage == FLAT_PRINT;
}

uint32_t GetObjectNumber(const CPDF_Object* entry,
                         const CPDF_Dictionary* dictionary) {
  const CPDF_Reference* reference = ToReference(entry);
  return reference ? reference->GetRefObjNum()
                   : (dictionary ? dictionary->GetObjNum() : 0);
}

bool IsEligibleForUsage(const CPDF_Dictionary* annotation, int usage) {
  if (!annotation ||
      annotation->GetByteStringFor(pdfium::annotation::kSubtype) == "Popup") {
    return false;
  }

  const int flags = annotation->GetIntegerFor("F");
  if (flags & pdfium::annotation_flags::kHidden) {
    return false;
  }
  return usage == FLAT_NORMALDISPLAY
             ? !(flags & pdfium::annotation_flags::kInvisible)
             : !!(flags & pdfium::annotation_flags::kPrint);
}

RetainPtr<const CPDF_Object> GetEffectiveObject(CPDF_Document* document,
                                                const CPDF_Object* entry) {
  if (!entry) {
    return nullptr;
  }
  const CPDF_Reference* reference = ToReference(entry);
  return reference && document
             ? document->GetOrParseIndirectObject(reference->GetRefObjNum())
             : pdfium::WrapRetain(entry);
}

RetainPtr<const CPDF_Stream> GetNormalAppearance(
    CPDF_Document* document,
    const CPDF_Dictionary* annotation) {
  RetainPtr<const CPDF_Object> appearance_entry =
      annotation ? annotation->GetObjectFor(pdfium::annotation::kAP) : nullptr;
  RetainPtr<const CPDF_Dictionary> appearance =
      ToDictionary(GetEffectiveObject(document, appearance_entry.Get()));
  if (!appearance) {
    return nullptr;
  }

  RetainPtr<const CPDF_Object> normal_entry = appearance->GetObjectFor("N");
  RetainPtr<const CPDF_Object> normal =
      GetEffectiveObject(document, normal_entry.Get());
  if (!normal) {
    return nullptr;
  }
  if (const CPDF_Stream* stream = normal->AsStream()) {
    return pdfium::WrapRetain(stream);
  }

  const CPDF_Dictionary* states = normal->AsDictionary();
  if (!states) {
    return nullptr;
  }

  const ByteString state = annotation->GetByteStringFor("AS");
  if (!state.IsEmpty()) {
    RetainPtr<const CPDF_Object> state_entry =
        states->GetObjectFor(state.AsStringView());
    return ToStream(GetEffectiveObject(document, state_entry.Get()));
  }

  CPDF_DictionaryLocker locker(states);
  for (const auto& item : locker) {
    RetainPtr<const CPDF_Object> direct =
        GetEffectiveObject(document, item.second.Get());
    if (direct && direct->IsStream()) {
      return pdfium::WrapRetain(direct->AsStream());
    }
  }
  return nullptr;
}

std::optional<FlattenCandidate> MakeCandidate(
    CPDF_Document* document,
    size_t annotation_index,
    const CPDF_Object* entry,
    RetainPtr<const CPDF_Dictionary> annotation,
    int usage) {
  if (!IsEligibleForUsage(annotation.Get(), usage)) {
    return std::nullopt;
  }

  RetainPtr<const CPDF_Stream> appearance =
      GetNormalAppearance(document, annotation.Get());
  if (!appearance) {
    return std::nullopt;
  }

  CFX_FloatRect annotation_rect =
      annotation->GetRectFor(pdfium::annotation::kRect);
  annotation_rect.Normalize();
  if (annotation_rect.IsEmpty()) {
    return std::nullopt;
  }

  RetainPtr<const CPDF_Dictionary> appearance_dict = appearance->GetDict();
  CFX_FloatRect appearance_rect;
  if (appearance_dict->KeyExist("Rect")) {
    appearance_rect = appearance_dict->GetRectFor("Rect");
  } else {
    appearance_rect = appearance_dict->GetRectFor("BBox");
  }
  appearance_rect.Normalize();
  if (appearance_rect.IsEmpty()) {
    return std::nullopt;
  }
  const CFX_Matrix appearance_matrix = appearance_dict->GetMatrixFor("Matrix");
  CFX_FloatRect transformed_appearance_rect =
      appearance_matrix.TransformRect(appearance_rect);
  transformed_appearance_rect.Normalize();
  if (transformed_appearance_rect.IsEmpty()) {
    return std::nullopt;
  }

  FlattenCandidate candidate;
  candidate.annotation_index = annotation_index;
  candidate.annotation_object_number = GetObjectNumber(entry, annotation.Get());
  candidate.annotation = std::move(annotation);
  candidate.appearance = std::move(appearance);
  candidate.annotation_rect = annotation_rect;
  candidate.appearance_rect = appearance_rect;
  candidate.appearance_matrix = appearance_matrix;
  return candidate;
}

// Builds the candidate plan for `page`: every eligible annotation when
// `targets` is empty, else exactly the matching ones. The ONE place the
// three flatten-shaped operations (whole page, a chosen set, export to a new
// page) decide what gets painted — so they can never disagree.
FlattenPlan BuildFlattenPlan(CPDF_Document* document,
                             const CPDF_Dictionary* page,
                             pdfium::span<const FlattenTarget> targets,
                             int usage) {
  FlattenPlan plan;
  plan.target_outcomes.assign(targets.size(), kTargetNotOnPage);
  RetainPtr<const CPDF_Object> annotations_entry =
      page ? page->GetObjectFor("Annots") : nullptr;
  RetainPtr<const CPDF_Array> annotations =
      ToArray(GetEffectiveObject(document, annotations_entry.Get()));
  if (!annotations) {
    return plan;
  }

  for (size_t i = 0; i < annotations->size(); ++i) {
    RetainPtr<const CPDF_Object> entry = annotations->GetObjectAt(i);
    RetainPtr<const CPDF_Dictionary> annotation =
        ToDictionary(GetEffectiveObject(document, entry.Get()));
    if (!annotation) {
      continue;
    }

    const uint32_t object_number =
        GetObjectNumber(entry.Get(), annotation.Get());
    std::optional<size_t> slot;
    if (!targets.empty()) {
      for (size_t t = 0; t < targets.size(); ++t) {
        if (plan.target_outcomes[t] != kTargetNotOnPage) {
          continue;  // already matched an earlier /Annots entry
        }
        const FlattenTarget& target = targets[t];
        const bool matches =
            target.annotation_object_number != 0
                ? object_number == target.annotation_object_number
                : annotation.Get() == target.annotation ||
                      (target.annotation_index >= 0 &&
                       i == static_cast<size_t>(target.annotation_index));
        if (matches) {
          slot = t;
          break;
        }
      }
      if (!slot.has_value()) {
        continue;
      }
    }

    std::optional<FlattenCandidate> candidate =
        MakeCandidate(document, i, entry.Get(), std::move(annotation), usage);
    if (candidate) {
      if (slot.has_value()) {
        plan.target_outcomes[*slot] = static_cast<int>(plan.candidates.size());
      }
      plan.candidates.push_back(std::move(*candidate));
    } else if (slot.has_value()) {
      plan.target_outcomes[*slot] = kTargetSkipped;
    }
  }
  return plan;
}

bool IsSamePage(CPDF_Page* page, CPDF_AnnotContext* annotation) {
  CPDF_Page* annotation_page = annotation && annotation->GetPage()
                                   ? annotation->GetPage()->AsPDFPage()
                                   : nullptr;
  if (!page || !annotation_page ||
      page->GetDocument() != annotation_page->GetDocument()) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> page_dictionary = page->GetDict();
  RetainPtr<const CPDF_Dictionary> annotation_page_dictionary =
      annotation_page->GetDict();
  if (!page_dictionary || !annotation_page_dictionary) {
    return false;
  }

  const uint32_t page_object_number = page_dictionary->GetObjNum();
  const uint32_t annotation_page_object_number =
      annotation_page_dictionary->GetObjNum();
  return page_object_number != 0 && annotation_page_object_number != 0
             ? page_object_number == annotation_page_object_number
             : page_dictionary.Get() == annotation_page_dictionary.Get();
}

ByteString GenerateFlattenedContent(const ByteString& key) {
  return "q 1 0 0 1 0 0 cm /" + key + " Do Q";
}

RetainPtr<CPDF_Reference> NewIndirectContentsStreamReference(
    CPDF_Document* document,
    const ByteString& contents) {
  auto pNewContents =
      document->NewIndirect<CPDF_Stream>(document->New<CPDF_Dictionary>());
  pNewContents->SetData(contents.unsigned_span());
  return pNewContents->MakeReference(document);
}

void AppendExistingContentStream(CPDF_Document* document,
                                 const CPDF_Object* entry,
                                 CPDF_Array* destination) {
  if (!document || !entry || !destination) {
    return;
  }

  RetainPtr<const CPDF_Object> direct = entry->GetDirect();
  const CPDF_Stream* stream = ToStream(direct.Get());
  if (!stream) {
    return;
  }

  const CPDF_Reference* reference = ToReference(entry);
  const uint32_t object_number =
      reference ? reference->GetRefObjNum() : stream->GetObjNum();
  if (object_number != 0) {
    destination->AppendNew<CPDF_Reference>(document, object_number);
    return;
  }

  RetainPtr<CPDF_Stream> clone = ToStream(stream->CloneForHolder(document));
  if (!clone) {
    return;
  }
  const uint32_t clone_object_number = document->AddIndirectObject(clone);
  destination->AppendNew<CPDF_Reference>(document, clone_object_number);
}

void AppendExistingContents(CPDF_Document* document,
                            const CPDF_Object* contents,
                            CPDF_Array* destination) {
  if (!contents) {
    return;
  }

  RetainPtr<const CPDF_Object> direct = contents->GetDirect();
  const CPDF_Array* array = ToArray(direct.Get());
  if (!array) {
    AppendExistingContentStream(document, contents, destination);
    return;
  }

  for (size_t i = 0; i < array->size(); ++i) {
    RetainPtr<const CPDF_Object> entry = array->GetObjectAt(i);
    AppendExistingContentStream(document, entry.Get(), destination);
  }
}

void SetPageContents(const ByteString& key,
                     CPDF_Dictionary* page,
                     CPDF_Document* document) {
  RetainPtr<const CPDF_Object> existing =
      page->GetObjectFor(pdfium::page_object::kContents);
  if (!existing) {
    page->SetFor(pdfium::page_object::kContents,
                 NewIndirectContentsStreamReference(
                     document, GenerateFlattenedContent(key)));
    return;
  }

  auto contents = document->NewIndirect<CPDF_Array>();
  contents->Append(NewIndirectContentsStreamReference(document, "q"));
  AppendExistingContents(document, existing.Get(), contents.Get());
  contents->Append(NewIndirectContentsStreamReference(document, "Q"));
  contents->Append(NewIndirectContentsStreamReference(
      document, GenerateFlattenedContent(key)));
  page->SetNewFor<CPDF_Reference>(pdfium::page_object::kContents, document,
                                  contents->GetObjNum());
}

CFX_Matrix GetMatrix(const CFX_FloatRect& rcAnnot,
                     const CFX_FloatRect& rcStream,
                     const CFX_Matrix& matrix) {
  if (rcStream.IsEmpty()) {
    return CFX_Matrix();
  }

  CFX_FloatRect rcTransformed = matrix.TransformRect(rcStream);
  rcTransformed.Normalize();

  float a = rcAnnot.Width() / rcTransformed.Width();
  float d = rcAnnot.Height() / rcTransformed.Height();

  float e = rcAnnot.left - rcTransformed.left * a;
  float f = rcAnnot.bottom - rcTransformed.bottom * d;
  return CFX_Matrix(a, 0.0f, 0.0f, d, e, f);
}

bool IsValidBaseEncoding(ByteString base_encoding) {
  // ISO 32000-1:2008 spec, table 114.
  // ISO 32000-2:2020 spec, table 112.
  //
  // Since /BaseEncoding is optional, `base_encoding` can be empty.
  return base_encoding.IsEmpty() ||
         base_encoding == pdfium::font_encodings::kWinAnsiEncoding ||
         base_encoding == pdfium::font_encodings::kMacRomanEncoding ||
         base_encoding == pdfium::font_encodings::kMacExpertEncoding;
}

void SanitizeFont(RetainPtr<CPDF_Dictionary> font_dict) {
  if (!font_dict) {
    return;
  }

  RetainPtr<CPDF_Dictionary> encoding_dict =
      font_dict->GetMutableDictFor("Encoding");
  if (encoding_dict) {
    if (!IsValidBaseEncoding(encoding_dict->GetNameFor("BaseEncoding"))) {
      font_dict->RemoveFor("Encoding");
    }
  }
}

void SanitizeFontResources(RetainPtr<CPDF_Dictionary> font_resource_dict) {
  if (!font_resource_dict) {
    return;
  }

  CPDF_DictionaryLocker locker(font_resource_dict);
  for (auto it : locker) {
    SanitizeFont(ToDictionary(it.second->GetMutableDirect()));
  }
}

void SanitizeResources(RetainPtr<CPDF_Dictionary> resources_dict) {
  if (!resources_dict) {
    return;
  }

  SanitizeFontResources(resources_dict->GetMutableDictFor("Font"));
}

RetainPtr<const CPDF_Dictionary> GetInheritedDictionary(
    const CPDF_Dictionary* page,
    ByteStringView key) {
  std::set<const CPDF_Dictionary*> visited;
  const CPDF_Dictionary* current = page;
  while (current && !visited.contains(current)) {
    RetainPtr<const CPDF_Dictionary> value = current->GetDictFor(key);
    if (value) {
      return value;
    }
    visited.insert(current);
    current = current->GetDictFor(pdfium::page_object::kParent).Get();
  }
  return nullptr;
}

CFX_FloatRect GetInheritedRect(const CPDF_Dictionary* page,
                               ByteStringView key) {
  std::set<const CPDF_Dictionary*> visited;
  const CPDF_Dictionary* current = page;
  while (current && !visited.contains(current)) {
    if (current->KeyExist(key)) {
      CFX_FloatRect rect = current->GetRectFor(key);
      rect.Normalize();
      return rect;
    }
    visited.insert(current);
    current = current->GetDictFor(pdfium::page_object::kParent).Get();
  }
  return CFX_FloatRect();
}

// Give the page a private resource dictionary and a private /XObject child.
// This both preserves inherited resources and avoids mutating a resource
// dictionary shared by the base document or another page.
RetainPtr<CPDF_Dictionary> CreateLocalPageXObjects(CPDF_Document* document,
                                                   CPDF_Dictionary* page) {
  if (!document || !page) {
    return nullptr;
  }

  RetainPtr<const CPDF_Dictionary> effective_resources =
      GetInheritedDictionary(page, pdfium::page_object::kResources);
  RetainPtr<CPDF_Dictionary> local_resources =
      effective_resources
          ? ToDictionary(effective_resources->CloneForHolder(document))
          : document->New<CPDF_Dictionary>();
  if (!local_resources) {
    return nullptr;
  }

  RetainPtr<const CPDF_Dictionary> existing_xobjects =
      local_resources->GetDictFor("XObject");
  RetainPtr<CPDF_Dictionary> local_xobjects =
      existing_xobjects
          ? ToDictionary(existing_xobjects->CloneForHolder(document))
          : document->New<CPDF_Dictionary>();
  if (!local_xobjects) {
    return nullptr;
  }

  local_resources->SetFor("XObject", local_xobjects);
  page->SetFor(pdfium::page_object::kResources, local_resources);
  return local_xobjects;
}

bool RemoveObjectFromArray(CPDF_Array* array,
                           uint32_t object_number,
                           const CPDF_Dictionary* dictionary) {
  if (!array) {
    return false;
  }

  bool removed = false;
  for (size_t i = array->size(); i > 0; --i) {
    RetainPtr<const CPDF_Object> entry = array->GetObjectAt(i - 1);
    const CPDF_Reference* reference = ToReference(entry.Get());
    if ((reference && object_number != 0 &&
         reference->GetRefObjNum() == object_number) ||
        (!reference && entry.Get() == dictionary)) {
      array->RemoveAt(i - 1);
      removed = true;
    }
  }
  return removed;
}

void UnlinkMergedFieldAndPruneAncestors(CPDF_Document* document,
                                        uint32_t field_object_number) {
  uint32_t current = field_object_number;
  for (int depth = 0; current != 0 && depth < 32; ++depth) {
    RetainPtr<const CPDF_Dictionary> node =
        ToDictionary(document->GetOrParseIndirectObject(current));
    if (!node) {
      return;
    }

    RetainPtr<const CPDF_Dictionary> parent = node->GetDictFor("Parent");
    if (parent && parent->GetObjNum() != 0) {
      RetainPtr<CPDF_Dictionary> mutable_parent =
          ToDictionary(document->GetMutableIndirectObject(parent->GetObjNum()));
      RetainPtr<CPDF_Array> parent_kids = epdf::GetMutableArrayMember(
          document, mutable_parent.Get(), "Kids", /*create_if_missing=*/false);
      if (!mutable_parent ||
          !RemoveObjectFromArray(parent_kids.Get(), current, node.Get())) {
        return;
      }
      if (!parent_kids->IsEmpty() || mutable_parent->KeyExist("FT")) {
        return;
      }
      mutable_parent->RemoveFor("Kids");
      current = parent->GetObjNum();
      continue;
    }

    RetainPtr<CPDF_Dictionary> acro_form = epdf::GetMutableAcroForm(
        document, /*create_if_missing=*/false, nullptr);
    RetainPtr<CPDF_Array> fields = epdf::GetMutableArrayMember(
        document, acro_form.Get(), "Fields", /*create_if_missing=*/false);
    RemoveObjectFromArray(fields.Get(), current, node.Get());
    return;
  }
}

// A flattened widget must no longer remain reachable through the AcroForm
// field tree. Separate widgets leave their logical field behind as an
// unplaced field. A merged field/widget is removed from its parent field array.
void DetachFlattenedWidget(CPDF_Document* document,
                           const FlattenCandidate& candidate,
                           CPDF_Dictionary* mutable_annotation) {
  if (!document || !mutable_annotation ||
      candidate.annotation->GetNameFor(pdfium::annotation::kSubtype) !=
          "Widget") {
    return;
  }

  RetainPtr<const CPDF_Dictionary> original_parent =
      candidate.annotation->GetDictFor("Parent");
  RetainPtr<CPDF_Dictionary> mutable_parent;
  if (original_parent && original_parent->GetObjNum() != 0) {
    mutable_parent = ToDictionary(
        document->GetMutableIndirectObject(original_parent->GetObjNum()));
  } else if (original_parent) {
    mutable_parent = mutable_annotation->GetMutableDictFor("Parent");
  }

  const bool is_merged_field = candidate.annotation->KeyExist("FT");
  if (is_merged_field && candidate.annotation_object_number != 0) {
    UnlinkMergedFieldAndPruneAncestors(document,
                                       candidate.annotation_object_number);
    mutable_annotation->RemoveFor("Parent");
    return;
  }

  if (mutable_parent) {
    RetainPtr<CPDF_Array> kids = epdf::GetMutableArrayMember(
        document, mutable_parent.Get(), "Kids", /*create_if_missing=*/false);
    RemoveObjectFromArray(kids.Get(), candidate.annotation_object_number,
                          mutable_annotation);

    if (kids && kids->IsEmpty()) {
      // A separate widget's terminal field remains visible as an unplaced
      // field.
      mutable_parent->RemoveFor("Kids");
    }
    mutable_annotation->RemoveFor("Parent");
    return;
  }

  if (!is_merged_field) {
    return;  // An orphan widget was never part of the AcroForm field tree.
  }

  RetainPtr<CPDF_Dictionary> acro_form =
      epdf::GetMutableAcroForm(document, /*create_if_missing=*/false, nullptr);
  RetainPtr<CPDF_Array> fields = epdf::GetMutableArrayMember(
      document, acro_form.Get(), "Fields", /*create_if_missing=*/false);
  RemoveObjectFromArray(fields.Get(), candidate.annotation_object_number,
                        mutable_annotation);
}

// Paints `prepared` appearances onto `page` (a page of `document`) as one
// wrapper form: the in-place flatten's writer, shared verbatim with the
// export-to-new-page path. `origin` shifts every placement — export passes
// the union rect's lower-left so the new page starts at (0, 0).
bool PaintAppearances(CPDF_Document* document,
                      CPDF_Dictionary* page,
                      pdfium::span<const PreparedAppearance> prepared,
                      const CFX_PointF& origin) {
  if (!document || !page || prepared.empty()) {
    return false;
  }

  CFX_FloatRect media_box =
      GetInheritedRect(page, pdfium::page_object::kMediaBox);
  if (media_box.IsEmpty()) {
    media_box = CFX_FloatRect(0.0f, 0.0f, 612.0f, 792.0f);
  }

  CFX_FloatRect crop_box =
      GetInheritedRect(page, pdfium::page_object::kCropBox);
  if (crop_box.IsEmpty()) {
    crop_box = media_box;
  }

  page->SetRectFor(pdfium::page_object::kMediaBox, media_box);
  page->SetRectFor(pdfium::page_object::kCropBox, crop_box);

  RetainPtr<CPDF_Dictionary> page_xobjects =
      CreateLocalPageXObjects(document, page);
  if (!page_xobjects) {
    return false;
  }

  ByteString page_form_name;
  for (int i = 0; i < INT_MAX; ++i) {
    ByteString candidate_name = ByteString::Format("FFT%d", i);
    if (!page_xobjects->KeyExist(candidate_name.AsStringView())) {
      page_form_name = std::move(candidate_name);
      break;
    }
  }
  if (page_form_name.IsEmpty()) {
    return false;
  }

  auto page_form =
      document->NewIndirect<CPDF_Stream>(document->New<CPDF_Dictionary>());
  RetainPtr<CPDF_Dictionary> page_form_dict = page_form->GetMutableDict();
  RetainPtr<CPDF_Dictionary> page_form_resources =
      page_form_dict->SetNewFor<CPDF_Dictionary>("Resources");
  RetainPtr<CPDF_Dictionary> form_xobjects =
      page_form_resources->SetNewFor<CPDF_Dictionary>("XObject");
  page_form_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  page_form_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  page_form_dict->SetNewFor<CPDF_Number>("FormType", 1);
  page_form_dict->SetRectFor("BBox", crop_box);

  ByteString form_content;
  for (size_t i = 0; i < prepared.size(); ++i) {
    const PreparedAppearance& item = prepared[i];
    RetainPtr<CPDF_Dictionary> appearance_dict = item.stream->GetMutableDict();
    appearance_dict->SetNewFor<CPDF_Name>("Type", "XObject");
    appearance_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
    SanitizeResources(appearance_dict->GetMutableDictFor("Resources"));

    const uint32_t appearance_object_number =
        item.stream->GetObjNum() != 0
            ? item.stream->GetObjNum()
            : document->AddIndirectObject(item.stream);
    const ByteString form_name = ByteString::Format("F%zu", i);
    form_xobjects->SetNewFor<CPDF_Reference>(form_name, document,
                                             appearance_object_number);

    // ISO 32000-2 12.5.5 algorithm 8.1: the form's own /Matrix is applied by
    // the Do operator; this `cm` maps the transformed BBox onto /Rect (an
    // axis-aligned scale + translate — rotation lives inside the form).
    CFX_Matrix matrix = GetMatrix(item.candidate.annotation_rect,
                                  item.candidate.appearance_rect,
                                  item.candidate.appearance_matrix);
    matrix.e -= origin.x;
    matrix.f -= origin.y;
    fxcrt::ostringstream buffer;
    WriteMatrix(buffer, matrix);
    form_content += ByteString::Format(
        "q %s cm /%s Do Q\n", ByteString(buffer).c_str(), form_name.c_str());
  }
  page_form->SetDataAndRemoveFilter(form_content.unsigned_span());
  page_xobjects->SetNewFor<CPDF_Reference>(page_form_name, document,
                                           page_form->GetObjNum());
  SetPageContents(page_form_name, page, document);
  return true;
}

int ApplyFlattenPlan(CPDF_Document* document,
                     RetainPtr<CPDF_Dictionary> page,
                     std::vector<FlattenCandidate> candidates) {
  if (!document || !page || candidates.empty()) {
    return FLATTEN_FAIL;
  }

  std::vector<PreparedAppearance> prepared;
  prepared.reserve(candidates.size());
  for (FlattenCandidate& candidate : candidates) {
    RetainPtr<CPDF_Stream> stream =
        ToStream(candidate.appearance->CloneForHolder(document));
    if (!stream) {
      continue;
    }
    prepared.push_back({std::move(candidate), std::move(stream)});
  }
  if (prepared.empty()) {
    return FLATTEN_FAIL;
  }

  if (!PaintAppearances(document, page.Get(), prepared, CFX_PointF())) {
    return FLATTEN_FAIL;
  }

  RetainPtr<CPDF_Array> annotations = epdf::GetMutableArrayMember(
      document, page.Get(), "Annots", /*create_if_missing=*/false);
  if (!annotations) {
    return FLATTEN_FAIL;
  }

  // Resolve widget dictionaries while their page-array entries still exist.
  for (PreparedAppearance& item : prepared) {
    if (item.candidate.annotation->GetNameFor(pdfium::annotation::kSubtype) !=
        "Widget") {
      continue;
    }
    RetainPtr<CPDF_Dictionary> mutable_annotation;
    if (item.candidate.annotation_object_number != 0) {
      mutable_annotation = ToDictionary(document->GetMutableIndirectObject(
          item.candidate.annotation_object_number));
    } else if (item.candidate.annotation_index < annotations->size()) {
      mutable_annotation =
          annotations->GetMutableDictAt(item.candidate.annotation_index);
    }
    if (mutable_annotation) {
      DetachFlattenedWidget(document, item.candidate, mutable_annotation.Get());
    }
  }

  std::sort(prepared.begin(), prepared.end(),
            [](const PreparedAppearance& lhs, const PreparedAppearance& rhs) {
              return lhs.candidate.annotation_index >
                     rhs.candidate.annotation_index;
            });
  for (const PreparedAppearance& item : prepared) {
    if (item.candidate.annotation_index < annotations->size()) {
      annotations->RemoveAt(item.candidate.annotation_index);
    }
  }
  if (annotations->IsEmpty()) {
    page->RemoveFor("Annots");
  }
  return FLATTEN_SUCCESS;
}

// Resolve annotation handles to targets of `page`. Returns false (and marks
// the offending entries NOT_ON_PAGE in `statuses`, when given) for a null
// handle or an annotation of another page/document — the whole call fails
// before anything is mutated.
bool CollectTargets(CPDF_Page* page,
                    FPDF_ANNOTATION* annots,
                    int annot_count,
                    std::vector<FlattenTarget>* targets,
                    int* statuses) {
  bool ok = true;
  for (int i = 0; i < annot_count; ++i) {
    CPDF_AnnotContext* annotation =
        CPDFAnnotContextFromFPDFAnnotation(annots[i]);
    const CPDF_Dictionary* dictionary =
        annotation ? annotation->GetAnnotDict() : nullptr;
    if (!dictionary || !IsSamePage(page, annotation)) {
      if (statuses) {
        statuses[i] = EPDF_FLATTEN_STATUS_NOT_ON_PAGE;
      }
      ok = false;
      continue;
    }
    if (statuses) {
      statuses[i] = EPDF_FLATTEN_STATUS_SKIPPED;
    }
    targets->push_back(
        {dictionary->GetObjNum(), annotation->GetAnnotIndex(), dictionary});
  }
  return ok;
}

int FlattenPage(CPDF_Page* page,
                pdfium::span<const FlattenTarget> targets,
                int usage,
                int* statuses) {
  CPDF_Document* document = page ? page->GetDocument() : nullptr;
  RetainPtr<const CPDF_Dictionary> const_page =
      page ? page->GetDict() : nullptr;
  if (!document || !const_page || !IsValidUsage(usage)) {
    return FLATTEN_FAIL;
  }

  FlattenPlan plan =
      BuildFlattenPlan(document, const_page.Get(), targets, usage);
  for (int outcome : plan.target_outcomes) {
    if (outcome == kTargetNotOnPage) {
      return FLATTEN_FAIL;  // CollectTargets vetted the page; /Annots disagrees
    }
  }
  if (plan.candidates.empty()) {
    return FLATTEN_NOTHINGTODO;
  }
  RetainPtr<CPDF_Dictionary> mutable_page = page->GetMutableDict();
  if (!mutable_page) {
    return FLATTEN_FAIL;
  }
  const int rc = ApplyFlattenPlan(document, std::move(mutable_page),
                                  std::move(plan.candidates));
  if (rc == FLATTEN_SUCCESS && statuses) {
    for (size_t t = 0; t < plan.target_outcomes.size(); ++t) {
      statuses[t] = plan.target_outcomes[t] >= 0 ? EPDF_FLATTEN_STATUS_APPLIED
                                                 : EPDF_FLATTEN_STATUS_SKIPPED;
    }
  }
  return rc;
}

}  // namespace

FPDF_EXPORT int FPDF_CALLCONV EPDFPage_Flatten(FPDF_PAGE page, int usage) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!pdf_page || !IsValidUsage(usage)) {
    return FLATTEN_FAIL;
  }
  return FlattenPage(pdf_page, {}, usage, nullptr);
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFPage_FlattenAnnotations(FPDF_PAGE page,
                            FPDF_ANNOTATION* annots,
                            int annot_count,
                            int usage,
                            int* statuses) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!pdf_page || !annots || annot_count <= 0 || !IsValidUsage(usage)) {
    return FLATTEN_FAIL;
  }
  std::vector<FlattenTarget> targets;
  targets.reserve(annot_count);
  if (!CollectTargets(pdf_page, annots, annot_count, &targets, statuses)) {
    return FLATTEN_FAIL;
  }
  return FlattenPage(pdf_page, targets, usage, statuses);
}

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFPage_ExportAnnotationsAsDocument(FPDF_PAGE page,
                                     FPDF_ANNOTATION* annots,
                                     int annot_count) {
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!pdf_page || !annots || annot_count <= 0) {
    return nullptr;
  }
  CPDF_Document* src_doc = pdf_page->GetDocument();
  RetainPtr<const CPDF_Dictionary> src_page = pdf_page->GetDict();
  if (!src_doc || !src_page) {
    return nullptr;
  }

  std::vector<FlattenTarget> targets;
  targets.reserve(annot_count);
  if (!CollectTargets(pdf_page, annots, annot_count, &targets, nullptr)) {
    return nullptr;
  }

  // Same plan as an in-place flatten of this set — all-or-nothing here.
  FlattenPlan plan =
      BuildFlattenPlan(src_doc, src_page.Get(), targets, FLAT_NORMALDISPLAY);
  for (int outcome : plan.target_outcomes) {
    if (outcome < 0) {
      return nullptr;
    }
  }
  if (plan.candidates.empty()) {
    return nullptr;
  }

  CFX_FloatRect union_rect = plan.candidates.front().annotation_rect;
  for (const FlattenCandidate& candidate : plan.candidates) {
    union_rect.Union(candidate.annotation_rect);
  }
  union_rect.Normalize();
  if (union_rect.IsEmpty()) {
    return nullptr;
  }

  FPDF_DOCUMENT exported = FPDF_CreateNewDocument();
  if (!exported) {
    return nullptr;
  }
  CPDF_Document* dest_doc = CPDFDocumentFromFPDFDocument(exported);
  FPDF_PAGE dest_page_handle =
      FPDFPage_New(exported, 0, union_rect.Width(), union_rect.Height());
  CPDF_Page* dest_page =
      dest_page_handle ? CPDFPageFromFPDFPage(dest_page_handle) : nullptr;
  if (!dest_doc || !dest_page) {
    if (dest_page_handle) {
      FPDF_ClosePage(dest_page_handle);
    }
    FPDF_CloseDocument(exported);
    return nullptr;
  }

  // One exporter for the whole set: resources shared between appearances
  // (a font used by three stamps) are cloned once.
  AnnotAppearanceExporter exporter(dest_doc, src_doc);
  std::vector<PreparedAppearance> prepared;
  prepared.reserve(plan.candidates.size());
  bool ok = true;
  for (FlattenCandidate& candidate : plan.candidates) {
    RetainPtr<CPDF_Stream> stream =
        exporter.ExportFormXObject(candidate.appearance);
    if (!stream) {
      ok = false;
      break;
    }
    prepared.push_back({std::move(candidate), std::move(stream)});
  }
  if (ok) {
    RetainPtr<CPDF_Dictionary> dest_dict = dest_page->GetMutableDict();
    ok = dest_dict &&
         PaintAppearances(dest_doc, dest_dict.Get(), prepared,
                          CFX_PointF(union_rect.left, union_rect.bottom));
  }
  FPDF_ClosePage(dest_page_handle);
  if (!ok) {
    FPDF_CloseDocument(exported);
    return nullptr;
  }
  return exported;
}
