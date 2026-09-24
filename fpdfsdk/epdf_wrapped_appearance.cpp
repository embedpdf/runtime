// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "fpdfsdk/epdf_wrapped_appearance.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/fpdfapi/page/cpdf_streamparser.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"

namespace {

constexpr char kLayerState[] = "R0";
constexpr char kLayerForm[] = "MWFOForm";
// The bytes Acrobat writes: it recognises its layer by these names.
constexpr char kLayerContent[] = "/R0 gs\n/MWFOForm Do\n";
constexpr char kWrapperChild[] = "EPDFWRAP";

// Forms deeper than this are not looked through.
constexpr int kMaxDepth = 16;

// A content stream that is one `Do`, optionally after one `gs`, optionally
// inside `q ... Q`.
struct SingleDo {
  ByteString state_token;  // empty without `gs`
  ByteString form_token;
};

std::optional<SingleDo> ParseSingleDo(const CPDF_Stream* stream) {
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
  acc->LoadAllDataFiltered();
  CPDF_StreamParser parser(acc->GetSpan());
  auto keyword = [&parser](ByteStringView word) {
    return parser.ParseNextElement() == CPDF_StreamParser::kKeyword &&
           parser.GetWord() == word;
  };
  CPDF_StreamParser::ElementType element = parser.ParseNextElement();
  const bool saved =
      element == CPDF_StreamParser::kKeyword && parser.GetWord() == "q";
  if (saved) {
    element = parser.ParseNextElement();
  }
  if (element != CPDF_StreamParser::kName) {
    return std::nullopt;
  }
  const ByteString first(parser.GetWord());
  if (parser.ParseNextElement() != CPDF_StreamParser::kKeyword) {
    return std::nullopt;
  }
  SingleDo result;
  if (parser.GetWord() == "gs") {
    result.state_token = first;
    if (parser.ParseNextElement() != CPDF_StreamParser::kName) {
      return std::nullopt;
    }
    result.form_token = ByteString(parser.GetWord());
    if (!keyword("Do")) {
      return std::nullopt;
    }
  } else if (parser.GetWord() == "Do") {
    result.form_token = first;
  } else {
    return std::nullopt;
  }
  if ((saved && !keyword("Q")) ||
      parser.ParseNextElement() != CPDF_StreamParser::kEndOfData) {
    return std::nullopt;
  }
  return result;
}

// The form `owner` draws under `token`, a Form XObject other than itself.
RetainPtr<const CPDF_Stream> ResolveForm(const CPDF_Stream* owner,
                                         const ByteString& token) {
  RetainPtr<const CPDF_Dictionary> resources =
      owner->GetDict()->GetDictFor("Resources");
  RetainPtr<const CPDF_Dictionary> xobjects =
      resources ? resources->GetDictFor("XObject") : nullptr;
  RetainPtr<const CPDF_Stream> form =
      xobjects ? xobjects->GetStreamFor(
                     PDF_NameDecode(token.AsStringView().Substr(1)).AsStringView())
               : nullptr;
  if (!form || form.Get() == owner ||
      form->GetDict()->GetNameFor("Subtype") != "Form") {
    return nullptr;
  }
  return form;
}

// Whether a form's dictionary holds nothing that changes how it draws: no
// optional content (/OC), reference XObject (/Ref) or other rendering entry,
// only these, and a transparency group only if it is a plain one. Metadata
// entries change nothing on the page.
bool HasOnlyPlainEntries(const CPDF_Dictionary* dict) {
  CPDF_DictionaryLocker locker(dict);
  for (const auto& entry : locker) {
    const ByteString& key = entry.first;
    if (key == "Type" || key == "Subtype" || key == "FormType" ||
        key == "BBox" || key == "Matrix" || key == "Resources" ||
        key == "Length" || key == "Filter" || key == "DecodeParms" ||
        key == "Name" || key == "Metadata" || key == "PieceInfo" ||
        key == "LastModified" || key == "StructParent" ||
        key == "StructParents") {
      continue;
    }
    if (key == "Group") {
      RetainPtr<const CPDF_Dictionary> group = dict->GetDictFor("Group");
      if (group && group->GetNameFor("S") == "Transparency" &&
          !group->GetBooleanFor("I", false) &&
          !group->GetBooleanFor("K", false) && !group->KeyExist("CS")) {
        continue;
      }
    }
    return false;
  }
  return true;
}

// Where a form shows what it draws: its /BBox through its /Matrix.
CFX_FloatRect ShownBox(const CPDF_Dictionary* form_dict) {
  CFX_FloatRect box = form_dict->GetRectFor("BBox");
  box.Normalize();
  box = form_dict->GetMatrixFor("Matrix").TransformRect(box);
  box.Normalize();
  return box;
}

// Whether `state` only sets one constant opacity for strokes and fills alike
// (`opacity`, when given): an annotation's /CA, not part of its drawing.
// Anything that changes how the drawing composites (a soft mask, a blend mode
// other than Normal) makes it part of the drawing.
bool IsOpacityOnlyState(const CPDF_Dictionary* state,
                        std::optional<float> opacity) {
  if (!state || !state->KeyExist("CA") || !state->KeyExist("ca")) {
    return false;
  }
  // One step of an 8-bit alpha: writers round (ours to 1/255), and a step
  // apart is no visible difference.
  constexpr float kTolerance = 1.0f / 255.0f;
  const float stroke = state->GetFloatFor("CA");
  if (std::fabs(state->GetFloatFor("ca") - stroke) > kTolerance ||
      (opacity && std::fabs(stroke - *opacity) > kTolerance)) {
    return false;
  }
  CPDF_DictionaryLocker locker(state);
  for (const auto& entry : locker) {
    const ByteString& key = entry.first;
    if (key == "CA" || key == "ca") {
      continue;
    }
    if (key == "Type" && state->GetNameFor("Type") == "ExtGState") {
      continue;
    }
    if (key == "AIS" && !state->GetBooleanFor("AIS", false)) {
      continue;
    }
    if (key == "BM" && (state->GetNameFor("BM") == "Normal" ||
                        state->GetNameFor("BM") == "Compatible")) {
      continue;
    }
    return false;
  }
  return true;
}

// Our wrapper's drawing, when `stream` is our wrapper: its whole content is
// `q a b c d e f cm /EPDFWRAP Do Q`. Private metadata can outlive an editor
// replacing the appearance, so the content decides, not a marker.
RetainPtr<const CPDF_Stream> WrapperDrawing(const CPDF_Stream* stream) {
  RetainPtr<const CPDF_Dictionary> resources =
      stream->GetDict()->GetDictFor("Resources");
  RetainPtr<const CPDF_Dictionary> xobjects =
      resources ? resources->GetDictFor("XObject") : nullptr;
  RetainPtr<const CPDF_Stream> drawing =
      xobjects ? xobjects->GetStreamFor(kWrapperChild) : nullptr;
  if (!drawing || drawing.Get() == stream ||
      drawing->GetDict()->GetNameFor("Subtype") != "Form") {
    return nullptr;
  }

  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
  acc->LoadAllDataFiltered();
  CPDF_StreamParser parser(acc->GetSpan());
  auto keyword = [&parser](ByteStringView word) {
    return parser.ParseNextElement() == CPDF_StreamParser::kKeyword &&
           parser.GetWord() == word;
  };
  if (!keyword("q")) {
    return nullptr;
  }
  for (int numbers = 0; numbers < 6; ++numbers) {
    if (parser.ParseNextElement() != CPDF_StreamParser::kNumber) {
      return nullptr;
    }
  }
  if (!keyword("cm") || parser.ParseNextElement() != CPDF_StreamParser::kName ||
      parser.GetWord() != "/EPDFWRAP" || !keyword("Do") || !keyword("Q") ||
      parser.ParseNextElement() != CPDF_StreamParser::kEndOfData) {
    return nullptr;
  }
  return drawing;
}

// Whether `matrix` only moves: no scale, rotation or skew.
bool IsTranslation(const CFX_Matrix& matrix) {
  constexpr float kTolerance = 1e-4f;
  return std::fabs(matrix.a - 1) < kTolerance && std::fabs(matrix.b) < kTolerance &&
         std::fabs(matrix.c) < kTolerance && std::fabs(matrix.d - 1) < kTolerance;
}

// The form `form` draws, when `form` adds nothing to it: its whole content is
// `[q] /Form Do [Q]`, its /Matrix at most moves what it draws, its box doesn't
// clip it, and its dictionary has only plain entries. Editors wrap
// appearances in these; Acrobat adds some whenever it edits a stamp, moving
// the drawing when its own arithmetic puts the box a fraction off the origin.
RetainPtr<const CPDF_Stream> PassThroughChild(const CPDF_Stream* form) {
  RetainPtr<const CPDF_Dictionary> dict = form->GetDict();
  if (!IsTranslation(dict->GetMatrixFor("Matrix")) ||
      !HasOnlyPlainEntries(dict.Get())) {
    return nullptr;
  }
  const std::optional<SingleDo> content = ParseSingleDo(form);
  if (!content || !content->state_token.IsEmpty()) {
    return nullptr;
  }
  RetainPtr<const CPDF_Stream> child = ResolveForm(form, content->form_token);
  if (!child) {
    return nullptr;
  }
  CFX_FloatRect box = dict->GetRectFor("BBox");
  box.Normalize();
  const CFX_FloatRect shown = ShownBox(child->GetDict().Get());
  // Rounding in the writer's numbers, not a clip anyone could see.
  constexpr float kTolerance = 0.05f;
  if (shown.IsEmpty() || shown.left < box.left - kTolerance ||
      shown.bottom < box.bottom - kTolerance ||
      shown.right > box.right + kTolerance ||
      shown.top > box.top + kTolerance) {
    return nullptr;
  }
  return child;
}

// A layer that paints one opacity over one form, `opacity` when given.
std::optional<EpdfOpacityLayer> FindOpacityLayer(const CPDF_Stream* ap,
                                                 std::optional<float> opacity) {
  if (!HasOnlyPlainEntries(ap->GetDict().Get())) {
    return std::nullopt;
  }
  const std::optional<SingleDo> content = ParseSingleDo(ap);
  if (!content || content->state_token.IsEmpty()) {
    return std::nullopt;
  }
  RetainPtr<const CPDF_Stream> form = ResolveForm(ap, content->form_token);
  RetainPtr<const CPDF_Dictionary> resources =
      ap->GetDict()->GetDictFor("Resources");
  RetainPtr<const CPDF_Dictionary> states =
      resources ? resources->GetDictFor("ExtGState") : nullptr;
  const ByteString state =
      PDF_NameDecode(content->state_token.AsStringView().Substr(1));
  if (!form || !states ||
      !IsOpacityOnlyState(states->GetDictFor(state.AsStringView()).Get(),
                          opacity)) {
    return std::nullopt;
  }
  return EpdfOpacityLayer{state, content->form_token, std::move(form)};
}

}  // namespace

float EpdfGetAnnotOpacity(const CPDF_Dictionary* annot_dict) {
  if (!annot_dict || !annot_dict->KeyExist("CA")) {
    return 1.0f;
  }
  return std::clamp(annot_dict->GetFloatFor("CA"), 0.0f, 1.0f);
}

std::optional<EpdfOpacityLayer> EpdfFindOpacityLayer(const CPDF_Stream* ap,
                                                     float opacity) {
  return FindOpacityLayer(ap, opacity);
}

std::optional<EpdfWrappedAppearance> EpdfFindWrappedAppearance(
    const CPDF_Stream* ap,
    std::optional<float> opacity) {
  RetainPtr<const CPDF_Stream> current = pdfium::WrapRetain(ap);
  // The /Matrix of the forms around `current`, innermost first.
  CFX_Matrix around;
  if (std::optional<EpdfOpacityLayer> layer = FindOpacityLayer(ap, opacity)) {
    around = ap->GetDict()->GetMatrixFor("Matrix");
    current = std::move(layer->form);
  }
  for (int depth = 0; current && depth < kMaxDepth; ++depth) {
    if (RetainPtr<const CPDF_Stream> drawing = WrapperDrawing(current.Get())) {
      CFX_Matrix wrapper_to_appearance =
          current->GetDict()->GetMatrixFor("Matrix");
      wrapper_to_appearance.Concat(around);
      return EpdfWrappedAppearance{std::move(current), std::move(drawing),
                                   wrapper_to_appearance};
    }
    RetainPtr<const CPDF_Stream> inner = PassThroughChild(current.Get());
    if (inner) {
      CFX_Matrix moved = current->GetDict()->GetMatrixFor("Matrix");
      moved.Concat(around);
      around = moved;
    }
    current = std::move(inner);
  }
  return std::nullopt;
}

RetainPtr<CPDF_Stream> EpdfNewOpacityLayer(CPDF_Document* doc,
                                           const CPDF_Stream* wrapper,
                                           float opacity) {
  auto dict = pdfium::MakeRetain<CPDF_Dictionary>();
  dict->SetNewFor<CPDF_Name>("Type", "XObject");
  dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  dict->SetRectFor("BBox", ShownBox(wrapper->GetDict().Get()));
  RetainPtr<CPDF_Dictionary> resources =
      dict->SetNewFor<CPDF_Dictionary>("Resources");
  RetainPtr<CPDF_Dictionary> state =
      resources->SetNewFor<CPDF_Dictionary>("ExtGState")
          ->SetNewFor<CPDF_Dictionary>(kLayerState);
  state->SetNewFor<CPDF_Name>("Type", "ExtGState");
  state->SetNewFor<CPDF_Number>("CA", opacity);
  state->SetNewFor<CPDF_Number>("ca", opacity);
  state->SetNewFor<CPDF_Boolean>("AIS", false);
  resources->SetNewFor<CPDF_Dictionary>("XObject")
      ->SetNewFor<CPDF_Reference>(kLayerForm, doc, wrapper->GetObjNum());

  auto layer = pdfium::MakeRetain<CPDF_Stream>(std::move(dict));
  layer->SetData(ByteStringView(kLayerContent).unsigned_span());
  return layer;
}
