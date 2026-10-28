// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_ppo.h"

#include <stdint.h>

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <utility>
#include <vector>

#include "constants/form_fields.h"
#include "core/fpdfapi/edit/cpdf_pageexporter.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_form_helpers.h"
#include "fpdfsdk/epdf_object_helpers.h"

namespace {

using pdfium::form_fields::kDA;
using pdfium::form_fields::kFT;
using pdfium::form_fields::kKids;
using pdfium::form_fields::kParent;
using pdfium::form_fields::kQ;

constexpr char kAcroForm[] = "AcroForm";
constexpr char kAnnots[] = "Annots";
constexpr char kCO[] = "CO";
constexpr char kDR[] = "DR";
constexpr char kFields[] = "Fields";
constexpr char kFont[] = "Font";
constexpr char kNeedAppearances[] = "NeedAppearances";
constexpr char kSigFlags[] = "SigFlags";

// The deepest field tree followed, as CPDF_InteractiveForm loads one.
constexpr int kMaxFieldDepth = 32;

// ISO 32000-2 table 224.
constexpr int kSignaturesExist = 1;

// What the pages bring of the source's form, by the source's object numbers.
struct FieldPlan {
  // The widgets the pages' /Annots name, each once, in page order.
  std::vector<uint32_t> widgets;
  std::set<uint32_t> widget_set;
  // The fields above them, each once, in the order they were reached.
  std::vector<uint32_t> fields;
  std::set<uint32_t> field_set;
  // The field above each widget and each field; none at the top.
  std::map<uint32_t, uint32_t> parent;
  // The top-level fields, in the order they were reached: a field, or a
  // widget that is its own field.
  std::vector<uint32_t> roots;
};

RetainPtr<const CPDF_Dictionary> AcroFormOf(const CPDF_Document* doc) {
  const CPDF_Dictionary* root = doc->GetRoot();
  return root ? root->GetDictFor(kAcroForm) : nullptr;
}

// The parent each field of the source's /AcroForm /Fields tree lists in its
// /Kids: the parent of a member whose own /Parent names none.
std::map<uint32_t, uint32_t> ParentsByKids(CPDF_Document* doc) {
  std::map<uint32_t, uint32_t> parents;
  RetainPtr<const CPDF_Dictionary> acro_form = AcroFormOf(doc);
  RetainPtr<const CPDF_Array> fields =
      acro_form ? acro_form->GetArrayFor(kFields) : nullptr;
  if (!fields) {
    return parents;
  }
  std::set<uint32_t> visited;
  std::vector<std::pair<uint32_t, int>> pending;
  for (size_t i = 0; i < fields->size(); ++i) {
    RetainPtr<const CPDF_Reference> ref = ToReference(fields->GetObjectAt(i));
    if (ref) {
      pending.emplace_back(ref->GetRefObjNum(), 0);
    }
  }
  while (!pending.empty()) {
    const auto [objnum, depth] = pending.back();
    pending.pop_back();
    if (depth >= kMaxFieldDepth || !visited.insert(objnum).second) {
      continue;
    }
    RetainPtr<const CPDF_Dictionary> node = epdf::ResolveFieldDict(doc, objnum);
    RetainPtr<const CPDF_Array> kids =
        node ? node->GetArrayFor(kKids) : nullptr;
    if (!kids) {
      continue;
    }
    for (size_t i = 0; i < kids->size(); ++i) {
      RetainPtr<const CPDF_Reference> ref = ToReference(kids->GetObjectAt(i));
      if (!ref || ref->GetRefObjNum() == objnum) {
        continue;
      }
      parents.try_emplace(ref->GetRefObjNum(), objnum);
      pending.emplace_back(ref->GetRefObjNum(), depth + 1);
    }
  }
  return parents;
}

// The field above |dict| (|objnum|): the one its /Parent names, else the one
// whose /Kids lists it. 0 for none.
uint32_t ParentOf(const CPDF_Dictionary* dict,
                  uint32_t objnum,
                  const std::map<uint32_t, uint32_t>& parents_by_kids) {
  RetainPtr<const CPDF_Reference> ref =
      ToReference(dict->GetObjectFor(kParent));
  if (ref && ref->GetRefObjNum() != objnum && ToDictionary(ref->GetDirect())) {
    return ref->GetRefObjNum();
  }
  const auto it = parents_by_kids.find(objnum);
  return it != parents_by_kids.end() ? it->second : 0;
}

bool IsWidget(const CPDF_Dictionary* dict) {
  return dict && dict->GetNameFor("Subtype") == "Widget";
}

FieldPlan PlanFields(CPDF_Document* doc,
                     pdfium::span<const uint32_t> page_indices) {
  FieldPlan plan;
  const std::map<uint32_t, uint32_t> parents_by_kids = ParentsByKids(doc);
  for (uint32_t page_index : page_indices) {
    RetainPtr<const CPDF_Dictionary> page =
        doc->GetPageDictionary(pdfium::checked_cast<int>(page_index));
    RetainPtr<const CPDF_Array> annots =
        page ? page->GetArrayFor(kAnnots) : nullptr;
    if (!annots) {
      continue;
    }
    for (size_t i = 0; i < annots->size(); ++i) {
      // An annotation written into /Annots is no field's member: /Kids can't
      // name it.
      RetainPtr<const CPDF_Reference> ref = ToReference(annots->GetObjectAt(i));
      RetainPtr<const CPDF_Dictionary> widget =
          ref ? ToDictionary(ref->GetDirect()) : nullptr;
      if (!IsWidget(widget.Get()) ||
          !plan.widget_set.insert(ref->GetRefObjNum()).second) {
        continue;
      }
      const uint32_t widget_objnum = ref->GetRefObjNum();
      plan.widgets.push_back(widget_objnum);

      uint32_t parent = ParentOf(widget.Get(), widget_objnum, parents_by_kids);
      if (!parent) {
        if (widget->KeyExist(kFT)) {
          plan.roots.push_back(widget_objnum);
        }
        continue;
      }
      plan.parent[widget_objnum] = parent;
      // Up to the top, or to a field reached before (its way up is known).
      for (int depth = 0; parent && depth < kMaxFieldDepth; ++depth) {
        if (plan.widget_set.count(parent) ||
            !plan.field_set.insert(parent).second) {
          break;
        }
        plan.fields.push_back(parent);
        RetainPtr<const CPDF_Dictionary> field =
            epdf::ResolveFieldDict(doc, parent);
        const uint32_t above =
            field ? ParentOf(field.Get(), parent, parents_by_kids) : 0;
        if (!above) {
          plan.roots.push_back(parent);
          break;
        }
        plan.parent[parent] = above;
        parent = above;
      }
    }
  }
  return plan;
}

// The destination's number for the source's |objnum|, or 0.
uint32_t CopyOf(const CPDF_PageExporter& exporter, uint32_t objnum) {
  const auto& map = exporter.object_number_map();
  const auto it = map.find(objnum);
  return it != map.end() ? it->second : 0;
}

RetainPtr<CPDF_Dictionary> MutableDictOf(CPDF_Document* doc, uint32_t objnum) {
  RetainPtr<CPDF_Object> object =
      objnum ? doc->GetMutableIndirectObject(objnum) : nullptr;
  if (!object) {
    return nullptr;
  }
  if (CPDF_Stream* stream = object->AsMutableStream()) {
    return stream->GetMutableDict();
  }
  return ToDictionary(std::move(object));
}

// The dictionary |dict| holds under |key|, ready to write; a new direct one
// when it has none. |dict| must already be writable.
RetainPtr<CPDF_Dictionary> MutableDictMember(CPDF_Document* doc,
                                             CPDF_Dictionary* dict,
                                             const ByteString& key) {
  RetainPtr<const CPDF_Object> entry = dict->GetObjectFor(key.AsStringView());
  if (!entry) {
    return dict->SetNewFor<CPDF_Dictionary>(key);
  }
  if (const CPDF_Reference* ref = entry->AsReference()) {
    return ToDictionary(doc->GetMutableIndirectObject(ref->GetRefObjNum()));
  }
  return dict->GetMutableDictFor(key.AsStringView());
}

// The copy names what its source named, as a copy in the destination; a
// /Parent naming what wasn't copied is removed: the page copy leaves every
// /Parent naming an object of the source.
void RetargetParent(CPDF_Document* dest,
                    const CPDF_PageExporter& exporter,
                    CPDF_Dictionary* copy) {
  RetainPtr<CPDF_Object> parent = copy->GetMutableObjectFor(kParent);
  if (!parent) {
    return;
  }
  CPDF_Reference* ref = parent->AsMutableReference();
  const uint32_t target = ref ? CopyOf(exporter, ref->GetRefObjNum()) : 0;
  if (target) {
    ref->SetRef(dest, target);
  } else {
    copy->RemoveFor(kParent);
  }
}

// A field copied with what is below it among the copies: its /Kids in its
// order, then the copies whose field it is that its /Kids doesn't list.
void CopyField(CPDF_Document* src,
               CPDF_Document* dest,
               CPDF_PageExporter& exporter,
               const FieldPlan& plan,
               const std::map<uint32_t, std::vector<uint32_t>>& children,
               uint32_t field) {
  RetainPtr<const CPDF_Dictionary> source = epdf::ResolveFieldDict(src, field);
  RetainPtr<CPDF_Dictionary> copy =
      MutableDictOf(dest, CopyOf(exporter, field));
  if (!source || !copy) {
    return;
  }
  {
    CPDF_DictionaryLocker locker(source);
    for (const auto& [key, value] : locker) {
      if (key != kParent && key != kKids) {
        copy->SetFor(key, value->Clone());
      }
    }
  }
  exporter.RetargetReferences(copy);

  const auto parent = plan.parent.find(field);
  if (parent != plan.parent.end()) {
    copy->SetNewFor<CPDF_Reference>(kParent, dest,
                                    CopyOf(exporter, parent->second));
  }
  const auto below = children.find(field);
  if (below == children.end()) {
    return;
  }
  std::vector<uint32_t> kids;
  std::set<uint32_t> listed;
  RetainPtr<const CPDF_Array> source_kids = source->GetArrayFor(kKids);
  for (size_t i = 0; source_kids && i < source_kids->size(); ++i) {
    RetainPtr<const CPDF_Reference> ref =
        ToReference(source_kids->GetObjectAt(i));
    if (ref && listed.insert(ref->GetRefObjNum()).second) {
      kids.push_back(ref->GetRefObjNum());
    }
  }
  const std::set<uint32_t> members(below->second.begin(), below->second.end());
  auto copied_kids = copy->SetNewFor<CPDF_Array>(kKids);
  for (uint32_t kid : kids) {
    if (members.count(kid) && CopyOf(exporter, kid)) {
      copied_kids->AppendNew<CPDF_Reference>(dest, CopyOf(exporter, kid));
    }
  }
  for (uint32_t member : below->second) {
    if (!listed.count(member) && CopyOf(exporter, member)) {
      copied_kids->AppendNew<CPDF_Reference>(dest, CopyOf(exporter, member));
    }
  }
}

// Whether two fonts' /Encoding entries are the same: none, the same name, or
// dictionaries with the same base encoding and /Differences (Acrobat gives
// every /Helv the same one).
bool IsSameEncoding(const CPDF_Object* a, const CPDF_Object* b) {
  if (!a || !b) {
    return !a && !b;
  }
  if (a->IsName() || b->IsName()) {
    return a->IsName() && b->IsName() && a->GetString() == b->GetString();
  }
  const CPDF_Dictionary* dict_a = a->AsDictionary();
  const CPDF_Dictionary* dict_b = b->AsDictionary();
  if (!dict_a || !dict_b ||
      dict_a->GetNameFor("BaseEncoding") !=
          dict_b->GetNameFor("BaseEncoding")) {
    return false;
  }
  RetainPtr<const CPDF_Array> differences_a =
      dict_a->GetArrayFor("Differences");
  RetainPtr<const CPDF_Array> differences_b =
      dict_b->GetArrayFor("Differences");
  if (!differences_a || !differences_b) {
    return !differences_a && !differences_b;
  }
  if (differences_a->size() != differences_b->size()) {
    return false;
  }
  for (size_t i = 0; i < differences_a->size(); ++i) {
    RetainPtr<const CPDF_Object> entry_a = differences_a->GetDirectObjectAt(i);
    RetainPtr<const CPDF_Object> entry_b = differences_b->GetDirectObjectAt(i);
    if (!entry_a || !entry_b || entry_a->GetType() != entry_b->GetType() ||
        entry_a->GetString() != entry_b->GetString()) {
      return false;
    }
  }
  return true;
}

// Whether two /DR fonts draw the same: the same standard (not embedded)
// simple font, in the same encoding.
bool IsSameStandardFont(const CPDF_Dictionary* a, const CPDF_Dictionary* b) {
  auto standard = [](const CPDF_Dictionary* font) {
    if (!font || font->GetNameFor("Subtype") == "Type0") {
      return false;
    }
    RetainPtr<const CPDF_Dictionary> descriptor =
        font->GetDictFor("FontDescriptor");
    return !descriptor || !(descriptor->KeyExist("FontFile") ||
                            descriptor->KeyExist("FontFile2") ||
                            descriptor->KeyExist("FontFile3"));
  };
  if (!standard(a) || !standard(b) ||
      a->GetNameFor("Subtype") != b->GetNameFor("Subtype") ||
      a->GetNameFor("BaseFont") != b->GetNameFor("BaseFont")) {
    return false;
  }
  return IsSameEncoding(a->GetDirectObjectFor("Encoding").Get(),
                        b->GetDirectObjectFor("Encoding").Get());
}

// The source's /DR entries the destination's /DR doesn't name, copied in. A
// font under a name the destination gives another font gets a free name,
// returned old name to new.
std::map<ByteString, ByteString> MergeResources(
    CPDF_Document* dest,
    CPDF_PageExporter& exporter,
    CPDF_Dictionary* dest_form,
    const CPDF_Dictionary* src_resources) {
  std::map<ByteString, ByteString> renamed_fonts;
  RetainPtr<CPDF_Dictionary> dest_resources =
      MutableDictMember(dest, dest_form, kDR);
  if (!dest_resources) {
    return renamed_fonts;
  }
  CPDF_DictionaryLocker categories(src_resources);
  for (const auto& [category, value] : categories) {
    RetainPtr<const CPDF_Dictionary> src_entries =
        ToDictionary(value->GetDirect());
    if (!src_entries) {
      continue;
    }
    RetainPtr<CPDF_Dictionary> dest_entries =
        MutableDictMember(dest, dest_resources.Get(), category);
    if (!dest_entries) {
      continue;
    }
    CPDF_DictionaryLocker entries(src_entries);
    for (const auto& [name, entry] : entries) {
      ByteString target = name;
      if (dest_entries->KeyExist(name.AsStringView())) {
        if (category != kFont ||
            IsSameStandardFont(dest_entries->GetDictFor(name.AsStringView()),
                               ToDictionary(entry->GetDirect()))) {
          continue;
        }
        for (int n = 1; dest_entries->KeyExist(target.AsStringView()); ++n) {
          target = name + "_" + ByteString::FormatInteger(n);
        }
        renamed_fonts[name] = target;
      }
      RetainPtr<CPDF_Object> copy = entry->Clone();
      if (exporter.RetargetReferences(copy)) {
        dest_entries->SetFor(target, std::move(copy));
      }
    }
  }
  return renamed_fonts;
}

// |appearance| (a /DA string) with the font it sets renamed as |renamed|
// says: the name operand of its Tf.
ByteString RenameAppearanceFont(
    const ByteString& appearance,
    const std::map<ByteString, ByteString>& renamed) {
  if (renamed.empty()) {
    return appearance;
  }
  // The tokens, as [start, end) offsets.
  std::vector<std::pair<size_t, size_t>> tokens;
  const ByteStringView text = appearance.AsStringView();
  for (size_t i = 0; i < text.GetLength();) {
    const uint8_t c = text[i];
    if (PDFCharIsWhitespace(c)) {
      ++i;
      continue;
    }
    size_t end = i + 1;
    if (c == '/' || !PDFCharIsDelimiter(c)) {
      while (end < text.GetLength() && !PDFCharIsWhitespace(text[end]) &&
             !PDFCharIsDelimiter(text[end])) {
        ++end;
      }
    }
    tokens.emplace_back(i, end);
    i = end;
  }
  ByteString result;
  size_t written = 0;
  for (size_t t = 2; t < tokens.size(); ++t) {
    const auto [start, end] = tokens[t - 2];
    if (text.Substr(tokens[t].first, tokens[t].second - tokens[t].first) !=
            "Tf" ||
        text[start] != '/') {
      continue;
    }
    const auto it =
        renamed.find(PDF_NameDecode(text.Substr(start + 1, end - start - 1)));
    if (it == renamed.end()) {
      continue;
    }
    result += text.Substr(written, start - written);
    result += ByteString("/") + PDF_NameEncode(it->second);
    written = end;
  }
  result += text.Substr(written);
  return result;
}

void RenameFontsIn(CPDF_Dictionary* dict,
                   const std::map<ByteString, ByteString>& renamed) {
  if (renamed.empty() || !dict->KeyExist(kDA)) {
    return;
  }
  const ByteString appearance = dict->GetByteStringFor(kDA);
  const ByteString changed = RenameAppearanceFont(appearance, renamed);
  if (changed != appearance) {
    dict->SetNewFor<CPDF_String>(kDA, changed);
  }
}

// The destination's /AcroForm, ready to write; an empty one when it has
// none (and |*created| is set).
RetainPtr<CPDF_Dictionary> MutableAcroForm(CPDF_Document* dest, bool* created) {
  if (RetainPtr<CPDF_Dictionary> acro_form = epdf::GetMutableAcroForm(
          dest, /*create_if_missing=*/false, nullptr)) {
    return acro_form;
  }
  RetainPtr<CPDF_Dictionary> root = dest->GetMutableRoot();
  if (!root) {
    return nullptr;
  }
  auto acro_form = dest->NewIndirect<CPDF_Dictionary>();
  root->SetNewFor<CPDF_Reference>(kAcroForm, dest, acro_form->GetObjNum());
  *created = true;
  return acro_form;
}

// The copied top-level fields into the destination's /AcroForm, with what
// the source's /AcroForm gave them.
void LinkFields(CPDF_Document* src,
                CPDF_Document* dest,
                CPDF_PageExporter& exporter,
                const FieldPlan& plan) {
  std::vector<uint32_t> roots;
  for (uint32_t root : plan.roots) {
    if (CopyOf(exporter, root)) {
      roots.push_back(CopyOf(exporter, root));
    }
  }
  if (roots.empty()) {
    return;
  }
  bool created = false;
  RetainPtr<CPDF_Dictionary> dest_form = MutableAcroForm(dest, &created);
  RetainPtr<CPDF_Array> fields =
      dest_form ? epdf::GetMutableArrayMember(dest, dest_form.Get(), kFields,
                                              /*create_if_missing=*/true)
                : nullptr;
  if (!fields) {
    return;
  }
  for (uint32_t root : roots) {
    fields->AppendNew<CPDF_Reference>(dest, root);
  }

  // Every copy the form names, the fields and the widgets.
  std::vector<RetainPtr<CPDF_Dictionary>> copies;
  for (const std::vector<uint32_t>* members : {&plan.fields, &plan.widgets}) {
    for (uint32_t member : *members) {
      if (RetainPtr<CPDF_Dictionary> copy =
              MutableDictOf(dest, CopyOf(exporter, member))) {
        copies.push_back(std::move(copy));
      }
    }
  }
  if (std::any_of(copies.begin(), copies.end(), [](const auto& copy) {
        return copy->GetNameFor(kFT) == pdfium::form_fields::kSig;
      })) {
    dest_form->SetNewFor<CPDF_Number>(
        kSigFlags, dest_form->GetIntegerFor(kSigFlags) | kSignaturesExist);
  }

  RetainPtr<const CPDF_Dictionary> src_form = AcroFormOf(src);
  if (!src_form) {
    return;
  }
  std::map<ByteString, ByteString> renamed_fonts;
  if (RetainPtr<const CPDF_Dictionary> src_resources =
          src_form->GetDictFor(kDR)) {
    renamed_fonts =
        MergeResources(dest, exporter, dest_form.Get(), src_resources.Get());
  }
  for (const auto& copy : copies) {
    RenameFontsIn(copy.Get(), renamed_fonts);
  }

  // What the source's /AcroForm gives fields that say nothing themselves.
  const ByteString src_appearance =
      RenameAppearanceFont(src_form->GetByteStringFor(kDA), renamed_fonts);
  const int src_quadding = src_form->GetIntegerFor(kQ, 0);
  if (created) {
    if (!src_appearance.IsEmpty()) {
      dest_form->SetNewFor<CPDF_String>(kDA, src_appearance);
    }
    if (src_form->KeyExist(kQ)) {
      dest_form->SetNewFor<CPDF_Number>(kQ, src_quadding);
    }
  }
  const ByteString dest_appearance = dest_form->GetByteStringFor(kDA);
  const int dest_quadding = dest_form->GetIntegerFor(kQ, 0);
  for (uint32_t root : roots) {
    RetainPtr<CPDF_Dictionary> copy = MutableDictOf(dest, root);
    if (!copy) {
      continue;
    }
    if (!src_appearance.IsEmpty() && src_appearance != dest_appearance &&
        !copy->KeyExist(kDA)) {
      copy->SetNewFor<CPDF_String>(kDA, src_appearance);
    }
    if (src_quadding != dest_quadding && !copy->KeyExist(kQ)) {
      copy->SetNewFor<CPDF_Number>(kQ, src_quadding);
    }
  }

  if (src_form->GetBooleanFor(kNeedAppearances, false)) {
    dest_form->SetNewFor<CPDF_Boolean>(kNeedAppearances, true);
  }

  RetainPtr<const CPDF_Array> src_order = src_form->GetArrayFor(kCO);
  std::vector<uint32_t> order;
  for (size_t i = 0; src_order && i < src_order->size(); ++i) {
    RetainPtr<const CPDF_Reference> ref =
        ToReference(src_order->GetObjectAt(i));
    const uint32_t objnum = ref ? ref->GetRefObjNum() : 0;
    if ((plan.field_set.count(objnum) || plan.widget_set.count(objnum)) &&
        CopyOf(exporter, objnum)) {
      order.push_back(CopyOf(exporter, objnum));
    }
  }
  if (!order.empty()) {
    RetainPtr<CPDF_Array> dest_order = epdf::GetMutableArrayMember(
        dest, dest_form.Get(), kCO, /*create_if_missing=*/true);
    for (uint32_t objnum : order) {
      if (dest_order) {
        dest_order->AppendNew<CPDF_Reference>(dest, objnum);
      }
    }
  }
}

// The copies' /Parent entries, now that every copy is made: a widget's names
// its field's copy, a field's its parent's (CopyField), and any other copy's
// what its source's named when that was copied.
void FixParents(CPDF_Document* src,
                CPDF_Document* dest,
                CPDF_PageExporter& exporter,
                const FieldPlan& plan,
                pdfium::span<const uint32_t> page_indices) {
  for (uint32_t widget : plan.widgets) {
    RetainPtr<CPDF_Dictionary> copy =
        MutableDictOf(dest, CopyOf(exporter, widget));
    if (!copy) {
      continue;
    }
    const auto parent = plan.parent.find(widget);
    const uint32_t field =
        parent != plan.parent.end() ? CopyOf(exporter, parent->second) : 0;
    if (field) {
      copy->SetNewFor<CPDF_Reference>(kParent, dest, field);
    } else {
      copy->RemoveFor(kParent);
    }
  }

  // Copied in the loop below: a copy of a page holds its own /Parent.
  std::set<uint32_t> pages;
  for (uint32_t page_index : page_indices) {
    RetainPtr<const CPDF_Dictionary> page =
        src->GetPageDictionary(pdfium::checked_cast<int>(page_index));
    if (page && CopyOf(exporter, page->GetObjNum())) {
      pages.insert(page->GetObjNum());
    }
  }
  for (const auto& [source, copied] : exporter.object_number_map()) {
    if (pages.count(source) || plan.field_set.count(source) ||
        plan.widget_set.count(source)) {
      continue;
    }
    if (RetainPtr<CPDF_Dictionary> copy = MutableDictOf(dest, copied)) {
      RetargetParent(dest, exporter, copy.Get());
    }
  }
  // Annotations written into a copied page's /Annots.
  for (uint32_t page : pages) {
    RetainPtr<CPDF_Dictionary> copy =
        MutableDictOf(dest, CopyOf(exporter, page));
    RetainPtr<CPDF_Array> annots =
        copy ? epdf::GetMutableArrayMember(dest, copy.Get(), kAnnots,
                                           /*create_if_missing=*/false)
             : nullptr;
    for (size_t i = 0; annots && i < annots->size(); ++i) {
      RetainPtr<CPDF_Dictionary> annot =
          ToDictionary(annots->GetMutableObjectAt(i));
      if (annot) {
        RetargetParent(dest, exporter, annot.Get());
      }
    }
  }
}

std::vector<uint32_t> PageIndices(const CPDF_Document* doc,
                                  const int* page_indices,
                                  unsigned long length,
                                  bool* valid) {
  const int page_count = doc->GetPageCount();
  *valid = true;
  if (!page_indices) {
    std::vector<uint32_t> all(page_count);
    std::iota(all.begin(), all.end(), 0);
    return all;
  }
  // SAFETY: required from caller.
  auto span = UNSAFE_BUFFERS(
      pdfium::span(page_indices, pdfium::checked_cast<size_t>(length)));
  std::vector<uint32_t> indices;
  indices.reserve(span.size());
  for (int page_index : span) {
    if (page_index < 0 || page_index >= page_count) {
      *valid = false;
      return {};
    }
    indices.push_back(static_cast<uint32_t>(page_index));
  }
  return indices;
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFDoc_ImportPages(FPDF_DOCUMENT dest_doc,
                                                        FPDF_DOCUMENT src_doc,
                                                        const int* page_indices,
                                                        unsigned long length,
                                                        int index) {
  CPDF_Document* dest = CPDFDocumentFromFPDFDocument(dest_doc);
  CPDF_Document* src = CPDFDocumentFromFPDFDocument(src_doc);
  if (!dest || !src || dest == src || (page_indices && length == 0)) {
    return false;
  }
  // One document is read through its layer: the source when an extract
  // copies a session's pages out, else the destination.
  CPDF_DocumentViewScope view(CPDF_LayerDocument::FromDocument(src) ? src
                                                                    : dest);
  bool valid = false;
  const std::vector<uint32_t> indices =
      PageIndices(src, page_indices, length, &valid);
  if (!valid || indices.empty()) {
    return false;
  }

  const FieldPlan plan = PlanFields(src, indices);
  CPDF_PageExporter exporter(dest, src);
  // The fields' copies are named before the pages are copied, so what the
  // pages reach of a field (a reset action naming it) reaches its copy.
  for (uint32_t field : plan.fields) {
    exporter.MapObjectNumber(field,
                             dest->NewIndirect<CPDF_Dictionary>()->GetObjNum());
  }
  if (!exporter.ExportPages(indices, index)) {
    return false;
  }
  if (CPDF_Document::Extension* extension = dest->GetExtension()) {
    extension->PagesInserted(index, indices.size());
  }

  std::map<uint32_t, std::vector<uint32_t>> children;
  for (const std::vector<uint32_t>* members : {&plan.fields, &plan.widgets}) {
    for (uint32_t member : *members) {
      const auto parent = plan.parent.find(member);
      if (parent != plan.parent.end()) {
        children[parent->second].push_back(member);
      }
    }
  }
  for (uint32_t field : plan.fields) {
    CopyField(src, dest, exporter, plan, children, field);
  }
  FixParents(src, dest, exporter, plan, indices);
  LinkFields(src, dest, exporter, plan);
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetImportFieldRoots(FPDF_DOCUMENT document,
                            const int* page_indices,
                            unsigned long length,
                            uint32_t* buffer,
                            unsigned long buffer_count) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return 0;
  }
  bool valid = false;
  const std::vector<uint32_t> indices =
      PageIndices(doc, page_indices, length, &valid);
  if (!valid) {
    return 0;
  }
  const FieldPlan plan = PlanFields(doc, indices);
  if (buffer) {
    // SAFETY: required from caller.
    auto out = UNSAFE_BUFFERS(
        pdfium::span(buffer, pdfium::checked_cast<size_t>(buffer_count)));
    for (size_t i = 0; i < out.size() && i < plan.roots.size(); ++i) {
      out[i] = plan.roots[i];
    }
  }
  return pdfium::checked_cast<unsigned long>(plan.roots.size());
}
