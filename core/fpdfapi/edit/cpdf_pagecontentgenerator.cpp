// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/edit/cpdf_pagecontentgenerator.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

#include "constants/page_object.h"
#include "core/fpdfapi/edit/cpdf_pagecontentmanager.h"
#include "core/fpdfapi/edit/cpdf_resourceuse.h"
#include "core/fpdfapi/edit/cpdf_stringarchivestream.h"
#include "core/fpdfapi/font/cpdf_truetypefont.h"
#include "core/fpdfapi/font/cpdf_type1font.h"
#include "core/fpdfapi/page/cpdf_clippath.h"
#include "core/fpdfapi/page/cpdf_contentmarks.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_path.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_pattern.h"
#include "core/fpdfapi/page/cpdf_shadingobject.h"
#include "core/fpdfapi/page/cpdf_shadingpattern.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/page/cpdf_color.h"
#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_iccprofile.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fpdfapi/parser/object_tree_traversal_util.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/containers/contains.h"
#include "core/fxcrt/notreached.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/stream_write_utils.h"
#include "core/fxcrt/zip.h"
#include "core/fxge/cfx_graphstate.h"

namespace {

// EmbedPDF(faithful-content-rewrite): kResourceKeys is now
// CPDF_ResourceUse::kTypes, with colour spaces, patterns and shadings.

// Key: The resource type.
// Value: The resource names of a given type.
using ResourcesMap = std::map<ByteString, std::set<ByteString>>;

// EmbedPDF(faithful-content-rewrite): whether `holder`, or a form it draws,
// shows text or clips with it (WriteInheritedTextState()).
bool ShowsText(const CPDF_PageObjectHolder* holder) {
  for (const auto& object : *holder) {
    if (!object->IsActive()) {
      continue;
    }
    const CPDF_ClipPath& clip_path = object->clip_path();
    const CPDF_FormObject* form = object->AsForm();
    if (object->IsText() ||
        (clip_path.HasRef() && clip_path.GetTextCount() > 0) ||
        (form && ShowsText(form->form()))) {
      return true;
    }
  }
  return false;
}

// EmbedPDF(faithful-content-rewrite): text shown with TJ adjustments is
// written back as a TJ array.
bool TextObjectNeedsTJ(const CPDF_TextObject* obj) {
  return std::ranges::any_of(obj->GetCharKernings(),
                             [](float kerning) { return kerning != 0; });
}

void WriteTextAsTJ(fxcrt::ostringstream& buf,
                   CPDF_TextObject* obj,
                   CPDF_Font* font) {
  buf << "[ ";
  ByteString hexChunk;
  for (auto [char_code, kerning] :
       fxcrt::Zip(obj->GetCharCodes(), obj->GetCharKernings())) {
    font->AppendChar(&hexChunk, char_code);
    if (kerning != 0) {
      // A TJ number is subtracted from the text position, as
      // CalcPositionDataInternal() applies the kerning; written as stored.
      buf << PDF_HexEncodeString(hexChunk.AsStringView()) << " ";
      hexChunk.clear();
      WriteFloat(buf, kerning) << " ";
    }
  }
  if (!hexChunk.IsEmpty()) {
    buf << PDF_HexEncodeString(hexChunk.AsStringView()) << " ";
  }
  buf << "] TJ";
}

// Balances the "q" operator ProcessGraphics() emitted.
void EndProcessGraphics(fxcrt::ostringstream& buf) {
  buf << " Q\n";
}

// EmbedPDF(faithful-content-rewrite): a colour space array as its PDF text.
// The holder's cache and the resources are matched on it, so the same colour
// space finds the same name whether it is held directly or by reference, or
// as the copy a layer transaction, or a form borrowing its parent's
// resources, makes of it.
ByteString ColorSpaceKey(const CPDF_Object* object) {
  RetainPtr<const CPDF_Object> array = object ? object->GetDirect() : nullptr;
  if (!array || !array->IsArray()) {
    return ByteString();
  }
  fxcrt::ostringstream buf;
  CPDF_StringArchiveStream archive(&buf);
  array->WriteTo(&archive, nullptr);
  return ByteString(buf);
}

// EmbedPDF(faithful-content-rewrite): the name `resources` give the colour
// space `key` describes, `hint` when that is one; empty when they give it
// none.
ByteString FindColorSpaceName(const CPDF_Dictionary* resources,
                              const ByteString& key,
                              const ByteString& hint) {
  RetainPtr<const CPDF_Dictionary> spaces =
      resources ? resources->GetDictFor("ColorSpace") : nullptr;
  if (!spaces) {
    return ByteString();
  }
  if (!hint.IsEmpty() &&
      ColorSpaceKey(spaces->GetObjectFor(hint.AsStringView()).Get()) == key) {
    return hint;
  }
  CPDF_DictionaryLocker locker(spaces);
  for (const auto& [name, value] : locker) {
    if (ColorSpaceKey(value.Get()) == key) {
      return name;
    }
  }
  return ByteString();
}

// EmbedPDF(faithful-content-rewrite): the line width, cap, join, miter limit
// and dash: each one the defaults don't already give, or `all` of them.
void WriteLineState(fxcrt::ostringstream& buf,
                    const CFX_GraphState& state,
                    bool all) {
  const float line_width = state.GetLineWidth();
  if (all || line_width != 1.0f) {
    WriteFloat(buf, line_width) << " w ";
  }
  const CFX_GraphStateData::LineCap cap = state.GetLineCap();
  if (all || cap != CFX_GraphStateData::LineCap::kButt) {
    buf << static_cast<int>(cap) << " J ";
  }
  const CFX_GraphStateData::LineJoin join = state.GetLineJoin();
  if (all || join != CFX_GraphStateData::LineJoin::kMiter) {
    buf << static_cast<int>(join) << " j ";
  }
  const float miter_limit = state.GetMiterLimit();
  if (all || miter_limit != 10.0f) {
    WriteFloat(buf, miter_limit) << " M ";
  }
  const std::vector<float> dash_array = state.GetLineDashArray();
  if (all || !dash_array.empty()) {
    buf << "[";
    for (size_t i = 0; i < dash_array.size(); ++i) {
      if (i > 0) {
        buf << " ";
      }
      WriteFloat(buf, dash_array[i]);
    }
    buf << "] ";
    WriteFloat(buf, state.GetLineDashPhase()) << " d ";
  }
}

// EmbedPDF(faithful-content-rewrite): whether one of the ExtGStates `names`
// sets a line parameter.
bool SetsLineState(const CPDF_Dictionary* resources,
                   pdfium::span<const ByteString> names) {
  RetainPtr<const CPDF_Dictionary> states =
      resources ? resources->GetDictFor("ExtGState") : nullptr;
  if (!states) {
    return false;
  }
  for (const ByteString& name : names) {
    RetainPtr<const CPDF_Dictionary> state =
        states->GetDictFor(name.AsStringView());
    if (state && (state->KeyExist("LW") || state->KeyExist("LC") ||
                  state->KeyExist("LJ") || state->KeyExist("ML") ||
                  state->KeyExist("D"))) {
      return true;
    }
  }
  return false;
}

void RecordPageObjectResourceUsage(const CPDF_PageObject* page_object,
                                   ResourcesMap& seen_resources) {
  const ByteString& resource_name = page_object->GetResourceName();
  if (!resource_name.IsEmpty()) {
    switch (page_object->GetType()) {
      case CPDF_PageObject::Type::kText:
        seen_resources["Font"].insert(resource_name);
        break;
      case CPDF_PageObject::Type::kImage:
      case CPDF_PageObject::Type::kForm:
        seen_resources["XObject"].insert(resource_name);
        break;
      case CPDF_PageObject::Type::kPath:
        break;
      case CPDF_PageObject::Type::kShading:
        // EmbedPDF(faithful-content-rewrite): shading objects are written too.
        seen_resources["Shading"].insert(resource_name);
        break;
    }
  }
  for (const auto& name : page_object->GetGraphicsResourceNames()) {
    CHECK(!name.IsEmpty());
    seen_resources["ExtGState"].insert(name);
  }
  // EmbedPDF(faithful-content-rewrite): the colour spaces and patterns the
  // object's colours were written with.
  const CPDF_ColorState& cs = page_object->color_state();
  if (cs.HasRef()) {
    if (!cs.GetFillColorSpaceResName().IsEmpty())
      seen_resources["ColorSpace"].insert(cs.GetFillColorSpaceResName());
    if (!cs.GetStrokeColorSpaceResName().IsEmpty())
      seen_resources["ColorSpace"].insert(cs.GetStrokeColorSpaceResName());
    if (!cs.GetFillPatternResName().IsEmpty())
      seen_resources["Pattern"].insert(cs.GetFillPatternResName());
    if (!cs.GetStrokePatternResName().IsEmpty())
      seen_resources["Pattern"].insert(cs.GetStrokePatternResName());
  }
  // EmbedPDF(faithful-content-rewrite): the fonts of the text it is clipped
  // by, written with it.
  const CPDF_ClipPath& clip_path = page_object->clip_path();
  if (clip_path.HasRef()) {
    for (size_t i = 0; i < clip_path.GetTextCount(); ++i) {
      const CPDF_TextObject* text = clip_path.GetText(i);
      if (text && !text->GetResourceName().IsEmpty()) {
        seen_resources["Font"].insert(text->GetResourceName());
      }
    }
  }
  // EmbedPDF(faithful-content-rewrite): an image's /ColorSpace can name one
  // of these resources (an inline image's may; some writers do it for image
  // XObjects too).
  if (const CPDF_ImageObject* image_object = page_object->AsImage()) {
    RetainPtr<CPDF_Image> image = image_object->GetImage();
    RetainPtr<const CPDF_Stream> stream = image ? image->GetStream() : nullptr;
    RetainPtr<const CPDF_Object> color_space =
        stream ? stream->GetDict()->GetDirectObjectFor("ColorSpace") : nullptr;
    if (color_space && color_space->IsName()) {
      seen_resources["ColorSpace"].insert(color_space->GetString());
    }
  }
}

CPDF_PageObjectHolder::RemovedResourceMap RemoveUnusedResources(
    CPDF_Dictionary* resource_dict,
    const std::vector<ByteString>& keys,
    const std::set<ByteString>* resource_in_use_of_current_type) {
  CPDF_PageObjectHolder::RemovedResourceMap removed_resource_map;
  for (const ByteString& key : keys) {
    if (resource_in_use_of_current_type &&
        pdfium::Contains(*resource_in_use_of_current_type, key)) {
      continue;
    }

    removed_resource_map[key] = resource_dict->RemoveFor(key.AsStringView());
  }
  return removed_resource_map;
}

void SaveUnusedResources(
    CPDF_PageObjectHolder::RemovedResourceMap& removed_resource_map,
    CPDF_PageObjectHolder::RemovedResourceMap& saved_resource_map) {
  for (auto& entry : removed_resource_map) {
    saved_resource_map[entry.first] = std::move(entry.second);
  }
}

std::vector<ByteString> FindKeysToRestore(
    const std::vector<ByteString>& keys,
    const std::set<ByteString>* resource_in_use_of_current_type,
    const CPDF_PageObjectHolder::RemovedResourceMap& saved_resource_map) {
  if (!resource_in_use_of_current_type) {
    return {};
  }

  std::vector<ByteString> missing_keys;
  for (const ByteString& key : *resource_in_use_of_current_type) {
    if (!pdfium::Contains(keys, key)) {
      missing_keys.push_back(key);
    }
  }

  std::vector<ByteString> keys_to_restore;
  for (const ByteString& key : missing_keys) {
    if (pdfium::Contains(saved_resource_map, key)) {
      keys_to_restore.push_back(key);
    }
  }
  return keys_to_restore;
}

// `keys_to_restore` entries must exist in `saved_resource_map`.
void RestoreUsedResources(
    CPDF_Dictionary* resource_dict,
    const std::vector<ByteString>& keys_to_restore,
    CPDF_PageObjectHolder::RemovedResourceMap& saved_resource_map) {
  for (const ByteString& key : keys_to_restore) {
    auto node = saved_resource_map.extract(key);
    CHECK(!node.empty());
    RetainPtr<CPDF_Object> value = std::move(node.mapped());
    // EmbedPDF(layer-transaction): a value saved during a layer transaction
    // that was aborted since may name an object the abort dropped (numbers
    // aren't reused, so it resolves to nothing), and belongs to that
    // transaction's dropped copy: skip the first, and attach a fresh copy of
    // the second.
    if (const CPDF_Reference* ref = value->AsReference();
        ref && !ref->GetDirect()) {
      continue;
    }
    if (!value->IsWritable()) {
      value = value->Clone();
    }
    resource_dict->SetFor(node.key(), std::move(value));
  }
}

void RemoveOrRestoreUnusedResources(
    RetainPtr<CPDF_Dictionary> resources_dict,
    const ResourcesMap& resources_in_use,
    CPDF_PageObjectHolder::AllRemovedResourcesMap& all_removed_resources_map) {
  // EmbedPDF(faithful-content-rewrite): the resource types the rewrite
  // tracks, colour spaces, patterns and shadings included.
  for (const char* resource_key : CPDF_ResourceUse::kTypes) {
    auto resources_in_use_it = resources_in_use.find(resource_key);
    const std::set<ByteString>* resource_in_use_of_current_type =
        resources_in_use_it != resources_in_use.end()
            ? &resources_in_use_it->second
            : nullptr;

    RetainPtr<CPDF_Dictionary> current_resource_dict =
        resources_dict->GetMutableDictFor(resource_key);
    if (!current_resource_dict && !resource_in_use_of_current_type) {
      continue;
    }

    // EmbedPDF(faithful-content-rewrite): a name can be in use with no
    // dictionary of its type here (a default colour space, a name borrowing
    // content mentions).
    const std::vector<ByteString> keys =
        current_resource_dict ? current_resource_dict->GetKeys()
                              : std::vector<ByteString>();
    CPDF_PageObjectHolder::RemovedResourceMap removed_resource_map =
        RemoveUnusedResources(current_resource_dict, keys,
                              resource_in_use_of_current_type);
    if (!removed_resource_map.empty()) {
      // Note that this may create a new entry in `all_removed_resources_map`.
      // Only do this if `removed_resource_map` is not empty.
      SaveUnusedResources(removed_resource_map,
                          all_removed_resources_map[resource_key]);
    }

    // Restore in-use objects if possible. To do so, first see if there is a
    // saved resource map to restore from.
    auto saved_resource_map_it = all_removed_resources_map.find(resource_key);
    if (saved_resource_map_it == all_removed_resources_map.end()) {
      continue;
    }

    auto& saved_resource_map = saved_resource_map_it->second;
    std::vector<ByteString> keys_to_restore = FindKeysToRestore(
        keys, resource_in_use_of_current_type, saved_resource_map);
    if (keys_to_restore.empty()) {
      continue;
    }

    // Create the dictionary if needed, so there is a place to restore to.
    if (!current_resource_dict) {
      current_resource_dict =
          resources_dict->SetNewFor<CPDF_Dictionary>(resource_key);
    }
    RestoreUsedResources(current_resource_dict, keys_to_restore,
                         saved_resource_map);
  }
}

bool IsPageResourceShared(CPDF_Document* doc,
                          RetainPtr<const CPDF_Dictionary> page_dict,
                          RetainPtr<const CPDF_Dictionary> resources_dict) {
  CHECK(doc);
  CHECK(page_dict);
  CHECK(resources_dict);

  const uint32_t resources_object_number = resources_dict->GetObjNum();
  if (resources_object_number) {
    // If `resources_dict` is not an inline object, then check to see if is a
    // shared object.
    if (pdfium::Contains(GetObjectsWithMultipleReferences(doc),
                         resources_object_number)) {
      return true;
    }
  }

  // The check above may not catch all cases. e.g. inline objects. Check all
  // pages in the document to see if another page is using the same resources.
  int page_dict_seen = 0;
  int resources_dict_seen = 0;
  for (int i = 0; i < doc->GetPageCount(); ++i) {
    RetainPtr<CPDF_Dictionary> current_page_dict =
        doc->GetMutablePageDictionary(i);
    if (!current_page_dict) {
      continue;
    }

    // Check to see if the current page's page dictionary is seen twice: Once
    // for this page, and once for another. If the same page dictionary is in
    // use twice, then the resource dictionary within must also be shared.
    if (current_page_dict == page_dict) {
      ++page_dict_seen;
      if (page_dict_seen == 2) {
        return true;
      }
    }

    // Similar check as above, for the current page's resource dictionary.
    auto page =
        pdfium::MakeRetain<CPDF_Page>(doc, std::move(current_page_dict));
    if (resources_dict == page->GetMutableResources()) {
      ++resources_dict_seen;
      if (resources_dict_seen == 2) {
        return true;
      }
    }
  }

  return false;
}

void CloneResourcesDictEntries(CPDF_Document* doc,
                               RetainPtr<CPDF_Dictionary> resources_dict) {
  struct KeyAndObject {
    ByteString key;
    RetainPtr<const CPDF_Object> object;
  };
  std::vector<KeyAndObject> entries_to_maybe_clone;
  {
    CPDF_DictionaryLocker locker(resources_dict);
    for (const auto& it : locker) {
      const ByteString& key = it.first;
      const RetainPtr<CPDF_Object>& object = it.second;
      // Clone resource dictionaries that may get modified. While out for
      // self-referencing loops.
      // EmbedPDF(faithful-content-rewrite): the resource types the rewrite
      // tracks, colour spaces, patterns and shadings included.
      if (object != resources_dict &&
          object->GetType() == CPDF_Object::kReference &&
          pdfium::Contains(CPDF_ResourceUse::kTypes, key)) {
        RetainPtr<const CPDF_Object> direct_object = object->GetDirect();
        if (direct_object) {
          entries_to_maybe_clone.emplace_back(key, std::move(direct_object));
        }
      }
    }
  }

  if (entries_to_maybe_clone.empty()) {
    return;
  }

  std::set<uint32_t> shared_objects = GetObjectsWithMultipleReferences(doc);
  if (shared_objects.empty()) {
    return;
  }

  // Must modify `resources_dict` after `locker` goes out of scope.
  for (const auto& entry : entries_to_maybe_clone) {
    if (pdfium::Contains(shared_objects, entry.object->GetObjNum())) {
      const uint32_t clone_object_number =
          doc->AddIndirectObject(entry.object->Clone());
      resources_dict->SetNewFor<CPDF_Reference>(entry.key, doc,
                                                clone_object_number);
    }
  }
}

}  // namespace

CPDF_PageContentGenerator::CPDF_PageContentGenerator(
    CPDF_PageObjectHolder* pObjHolder)
    : obj_holder_(pObjHolder), document_(pObjHolder->GetDocument()) {
  // Copy all page objects, even if they are inactive. They are needed in
  // GenerateModifiedStreams() below.
  for (const auto& pObj : *pObjHolder) {
    page_objects_.emplace_back(pObj.get());
  }
}

CPDF_PageContentGenerator::~CPDF_PageContentGenerator() = default;

void CPDF_PageContentGenerator::GenerateContent() {
  // EmbedPDF(view-scope): resource/reference traversal must see this layer's
  // promoted page-tree objects, including ancestors changed by redaction
  // resource cleanup.
  CPDF_DocumentViewScope document_view(document_);
  // EmbedPDF(page-residency): a page that matches the document now matches
  // what this writes.
  CPDF_Page* page = obj_holder_->IsPage()
                        ? static_cast<CPDF_Page*>(obj_holder_.get())
                        : nullptr;
  const bool page_was_current = page && page->IsContentCurrent();
  std::map<int32_t, fxcrt::ostringstream> new_stream_data =
      GenerateModifiedStreams();
  // If no streams were regenerated or removed, nothing to do here.
  if (new_stream_data.empty()) {
    return;
  }

  // EmbedPDF(faithful-content-rewrite): did this pass rewrite EVERY existing
  // content stream? Computed before the move — an append-only pass (a lone
  // kNoContentStream bucket) leaves the existing streams' ops out of
  // `page_objects_`' bookkeeping, so resource pruning must not run (see
  // UpdateResourcesDict).
  const int32_t existing_streams = CountExistingContentStreams();
  bool regenerated_all_streams = true;
  for (int32_t i = 0; i < existing_streams; ++i) {
    if (!pdfium::Contains(new_stream_data, i)) {
      regenerated_all_streams = false;
      break;
    }
  }

  UpdateContentStreams(std::move(new_stream_data));
  UpdateResourcesDict(regenerated_all_streams);
  // EmbedPDF(page-residency): the page's recorded content versions follow
  // what was written.
  if (page) {
    page->ContentGenerated(page_was_current);
  }
}

// EmbedPDF(faithful-content-rewrite): the holder's content streams, to tell
// whether a pass rewrote all of them.
int32_t CPDF_PageContentGenerator::CountExistingContentStreams() {
  if (obj_holder_->GetMutableFormStream()) {
    return 1;
  }
  RetainPtr<const CPDF_Object> contents =
      obj_holder_->GetDict()->GetObjectFor(pdfium::page_object::kContents);
  if (!contents) {
    return 0;
  }
  // Resolve indirection: /Contents is commonly an indirect reference to a
  // stream or to an array of streams.
  RetainPtr<const CPDF_Object> direct = contents->GetDirect();
  if (!direct) {
    return 0;
  }
  if (const CPDF_Array* arr = direct->AsArray()) {
    return pdfium::checked_cast<int32_t>(arr->size());
  }
  return direct->IsStream() ? 1 : 0;
}

std::map<int32_t, fxcrt::ostringstream>
CPDF_PageContentGenerator::GenerateModifiedStreams() {
  // Figure out which streams are dirty.
  std::set<int32_t> all_dirty_streams;
  for (auto& pPageObj : page_objects_) {
    // Must include dirty page objects even if they are marked as inactive.
    // Otherwise an inactive object will not be detected that its stream needs
    // to be removed as part of regeneration.
    if (pPageObj->IsDirty()) {
      all_dirty_streams.insert(pPageObj->GetContentStream());
    }
  }
  std::set<int32_t> marked_dirty_streams = obj_holder_->TakeDirtyStreams();
  all_dirty_streams.insert(marked_dirty_streams.begin(),
                           marked_dirty_streams.end());

  // EmbedPDF(faithful-content-rewrite): if an existing stream (or an object
  // living in one) changed, regenerate *all* page content streams. Rationale:
  // CTM / graphics-state handoff between streams means rewriting only a
  // subset can leave the concatenated effect inconsistent.
  //
  // A pure append is exempt: every dirty entry is the streamless bucket and
  // no stream was marked dirty. Regeneration serializes page objects only, so
  // rewriting an untouched stream drops ambient ops no page object owns —
  // e.g. a page-level `/C1 cs` prolog a bare-`scn` form inherits (see the
  // pruning guard in UpdateResourcesDict, and
  // FPDFAnnotEmbedderTest.ApplyRedactionPreservesInheritedColorspaceResources).
  const bool append_only =
      marked_dirty_streams.empty() && all_dirty_streams.size() == 1 &&
      all_dirty_streams.count(CPDF_PageObject::kNoContentStream) == 1;
  if (!append_only && !all_dirty_streams.empty()) {
    int32_t last_index = -1;
    // For Form XObjects, the content is in the XObject stream itself (index 0),
    // not in a separate "Contents" entry.
    if (obj_holder_->GetMutableFormStream()) {
      last_index = 0;
    } else if (RetainPtr<const CPDF_Object> contents =
            obj_holder_->GetDict()->GetObjectFor(pdfium::page_object::kContents)) {
      // Resolve indirection like CountExistingContentStreams(): /Contents is
      // commonly `4 0 R` pointing at the split-stream array
      // (split_streams.pdf), which must enumerate like a direct array.
      RetainPtr<const CPDF_Object> direct = contents->GetDirect();
      if (const CPDF_Array* arr = direct ? direct->AsArray() : nullptr) {
        last_index = static_cast<int32_t>(arr->size()) - 1;
      } else if (direct && direct->IsStream()) {
        last_index = 0;
      }
    }
    for (int32_t i = 0; i <= last_index; ++i) {
      all_dirty_streams.insert(i);
    }
  }
  // --- end embedpdf

  // Start regenerating dirty streams.
  std::map<int32_t, fxcrt::ostringstream> streams;
  std::set<int32_t> empty_streams;
  std::unique_ptr<const CPDF_ContentMarks> empty_content_marks =
      std::make_unique<CPDF_ContentMarks>();
  std::map<int32_t, const CPDF_ContentMarks*> current_content_marks;

  for (int32_t dirty_stream : all_dirty_streams) {
    fxcrt::ostringstream buf;

    // Set the default graphic state values. Update CTM to be the identity
    // matrix for the duration of this stream, if it is not already.
    // EmbedPDF(faithful-content-rewrite): a form drawn as page content begins
    // at its /Matrix, which its objects' matrices already include: the
    // inverse undoes it.
    buf << "q\n";
    const CFX_Matrix ctm = obj_holder_->GetCTMAtBeginningOfStream(dirty_stream);
    if (!ctm.IsIdentity()) {
      WriteMatrix(buf, ctm.GetInverse()) << " cm\n";
    }

    ProcessDefaultGraphics(&buf);
    // EmbedPDF(faithful-content-rewrite): text is written with its spacing
    // only when not 0. A form starts with the spacing of the content drawing
    // it (WriteInheritedTextState()), so it starts again from 0.
    if (!obj_holder_->IsPage()) {
      buf << "0 Tc 0 Tw ";
    }
    streams[dirty_stream] = std::move(buf);
    empty_streams.insert(dirty_stream);
    current_content_marks[dirty_stream] = empty_content_marks.get();
  }

  // Process the page objects, write into each dirty stream.
  for (auto& pPageObj : page_objects_) {
    if (!pPageObj->IsActive()) {
      continue;
    }

    int stream_index = pPageObj->GetContentStream();
    auto it = streams.find(stream_index);
    if (it == streams.end()) {
      continue;
    }

    fxcrt::ostringstream* buf = &it->second;
    empty_streams.erase(stream_index);
    current_content_marks[stream_index] =
        ProcessContentMarks(buf, pPageObj, current_content_marks[stream_index]);
    ProcessPageObject(buf, pPageObj);
  }

  // Finish dirty streams.
  for (int32_t dirty_stream : all_dirty_streams) {
    CFX_Matrix prev_ctm;
    CFX_Matrix ctm;
    bool affects_ctm;
    if (dirty_stream >= 0) {
      // EmbedPDF(faithful-content-rewrite): the first stream begins at the
      // CTM the content begins with (a form's /Matrix), not always the
      // identity.
      prev_ctm = obj_holder_->GetCTMAtBeginningOfStream(dirty_stream);
      ctm = obj_holder_->GetCTMAtEndOfStream(dirty_stream);
      affects_ctm = prev_ctm != ctm;
    } else {
      CHECK_EQ(CPDF_PageObject::kNoContentStream, dirty_stream);
      // This is the last stream, so there is no subsequent stream that it can
      // affect.
      affects_ctm = false;
    }

    const bool is_empty = pdfium::Contains(empty_streams, dirty_stream);

    fxcrt::ostringstream* buf = &streams[dirty_stream];
    if (is_empty && !affects_ctm) {
      // Clear to show that this stream needs to be deleted.
      buf->str("");
      continue;
    }

    if (!is_empty) {
      FinishMarks(buf, current_content_marks[dirty_stream]);
    }

    // Return graphics to original state.
    *buf << "Q\n";

    if (affects_ctm) {
      // Update CTM so the next stream gets the expected value.
      CFX_Matrix ctm_difference = prev_ctm.GetInverse() * ctm;
      if (!ctm_difference.IsIdentity()) {
        WriteMatrix(*buf, ctm_difference) << " cm\n";
      }
    }
  }

  return streams;
}

void CPDF_PageContentGenerator::UpdateContentStreams(
    std::map<int32_t, fxcrt::ostringstream>&& new_stream_data) {
  CHECK(!new_stream_data.empty());

  // Make sure default graphics are created.
  default_graphics_name_ = GetOrCreateDefaultGraphics();

  CPDF_PageContentManager page_content_manager(obj_holder_, document_);
  for (auto& pair : new_stream_data) {
    int32_t stream_index = pair.first;
    fxcrt::ostringstream* buf = &pair.second;

    if (stream_index == CPDF_PageObject::kNoContentStream) {
      int new_stream_index =
          pdfium::checked_cast<int>(page_content_manager.AddStream(buf));
      UpdateStreamlessPageObjects(new_stream_index);
      continue;
    }

    if (page_content_manager.HasStreamAtIndex(stream_index)) {
      page_content_manager.UpdateStream(stream_index, buf);
    } else {
      page_content_manager.AddStream(buf);
    }
  }
}

// EmbedPDF(faithful-content-rewrite): prunes only after a pass that rewrote
// every content stream (see the guard below).
void CPDF_PageContentGenerator::UpdateResourcesDict(bool regenerated_all_streams) {
  RetainPtr<CPDF_Dictionary> resources = obj_holder_->GetMutableResources();
  if (!resources) {
    return;
  }

  // Do not modify shared resource dictionaries. Give this page its own copy.
  if (IsPageResourceShared(document_, obj_holder_->GetDict(), resources)) {
    resources = pdfium::WrapRetain(resources->Clone()->AsMutableDictionary());
    const uint32_t clone_object_number =
        document_->AddIndirectObject(resources);
    obj_holder_->SetResources(resources);
    obj_holder_->GetMutableDict()->SetNewFor<CPDF_Reference>(
        pdfium::page_object::kResources, document_, clone_object_number);
  }

  // Even though `resources` itself is not shared, its dictionary entries may be
  // shared. Checked for that and clone those as well.
  CloneResourcesDictEntries(document_, resources);

  // EmbedPDF(redaction): objects the content is to draw instead of others - a
  // soft mask or a tiling pattern whose own content changed - under the same
  // names. After the copies above: they name what the content does.
  ApplyResourceReplacements(resources.Get());

  // EmbedPDF(faithful-content-rewrite): pruning is only sound when THIS pass
  // rewrote every content stream. `page_objects_` / `seen_resources` describe
  // the SERIALIZED output; an untouched stream's raw ops can reference
  // resources no page object records — e.g. a page-level `/C1 cs` prolog
  // whose colorspace is inherited by a bare-`scn` Form XObject. Pruning after
  // an append-only pass orphans those references (the classic symptom: the
  // whole page collapses to grayscale after applying a redaction that touched
  // nothing).
  if (!regenerated_all_streams) {
    return;
  }

  ResourcesMap seen_resources;
  for (auto& page_object : page_objects_) {
    if (!page_object->IsActive()) {
      continue;
    }
    RecordPageObjectResourceUsage(page_object, seen_resources);
  }
  if (!default_graphics_name_.IsEmpty()) {
    seen_resources["ExtGState"].insert(default_graphics_name_);
  }
  // EmbedPDF(faithful-content-rewrite): the fonts forms inherit, which no
  // page object names.
  seen_resources["Font"].insert(inherited_font_names_.begin(),
                                inherited_font_names_.end());
  // EmbedPDF(faithful-content-rewrite): the default colour spaces apply to
  // device colours without being named, and content reading these resources
  // without its own names what it uses only in its own stream.
  for (const char* name : {"DefaultGray", "DefaultRGB", "DefaultCMYK"}) {
    seen_resources["ColorSpace"].insert(name);
  }
  AddNamesBorrowed(seen_resources);

  RemoveOrRestoreUnusedResources(std::move(resources), seen_resources,
                                 obj_holder_->all_removed_resources_map());
}

// EmbedPDF(redaction): the copies the Type3 redactor made (a soft mask, a
// tiling pattern) replace the originals under the same names.
void CPDF_PageContentGenerator::ApplyResourceReplacements(
    CPDF_Dictionary* resources) {
  for (const auto& [entry, objnum] : obj_holder_->TakeResourceReplacements()) {
    const auto& [type, name] = entry;
    RetainPtr<CPDF_Dictionary> names =
        resources->GetMutableDictFor(type.AsStringView());
    if (names && names->KeyExist(name.AsStringView())) {
      names->SetNewFor<CPDF_Reference>(name, document_, objnum);
    }
  }
}

// EmbedPDF(faithful-content-rewrite): pruning keeps the names content uses
// without naming them in its own resources.
void CPDF_PageContentGenerator::AddNamesBorrowed(
    ResourcesMap& seen_resources) const {
  // Content can look names up in these resources without naming them in
  // its own: a form drawn here, the glyphs of a Type3 font and, on a page,
  // an annotation's appearance - and what those draw in turn. Only what is
  // still drawn is walked: what nothing draws any more goes, with what only
  // it drew.
  RetainPtr<const CPDF_Dictionary> resources = obj_holder_->GetResources();
  if (!resources) {
    return;
  }
  RetainPtr<const CPDF_Dictionary> page_resources =
      obj_holder_->IsPage() ? resources : obj_holder_->GetPageResources();
  CPDF_ResourceUse use(document_, resources);
  use.SetPageResources(page_resources);

  // The forms drawn here (an inactive object is not written), and the
  // glyphs of the Type3 fonts this holder's own text selects.
  for (const auto& object : *obj_holder_) {
    const CPDF_FormObject* form_object =
        object->IsActive() ? object->AsForm() : nullptr;
    RetainPtr<const CPDF_Stream> stream =
        form_object ? form_object->form()->GetStream() : nullptr;
    if (!stream) {
      continue;
    }
    RetainPtr<const CPDF_Dictionary> own =
        stream->GetDict()->GetDictFor("Resources");
    use.Walk(stream.Get(),
             CPDF_Form::ChooseResourcesDict(own.Get(), resources.Get(),
                                            page_resources.Get()),
             page_resources.Get());
  }
  use.WalkType3Glyphs(seen_resources["Font"], resources.Get(),
                      page_resources.Get());

  // Appearances: a form whose page resources are the page's
  // (CPDF_DocPageData::CreateForm()).
  RetainPtr<const CPDF_Array> annots =
      obj_holder_->IsPage() ? obj_holder_->GetDict()->GetArrayFor("Annots")
                            : nullptr;
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> annot = annots->GetDictAt(i);
    RetainPtr<const CPDF_Dictionary> ap =
        annot ? annot->GetDictFor("AP") : nullptr;
    if (!ap) {
      continue;
    }
    for (const char* mode : {"N", "R", "D"}) {
      RetainPtr<const CPDF_Object> entry = ap->GetDirectObjectFor(mode);
      std::vector<RetainPtr<const CPDF_Stream>> appearances;
      if (const CPDF_Stream* stream = entry ? entry->AsStream() : nullptr) {
        appearances.push_back(pdfium::WrapRetain(stream));
      } else if (const CPDF_Dictionary* states =
                     entry ? entry->AsDictionary() : nullptr) {
        CPDF_DictionaryLocker state_locker(pdfium::WrapRetain(states));
        for (const auto& state : state_locker) {
          if (RetainPtr<const CPDF_Stream> stream =
                  ToStream(state.second->GetDirect())) {
            appearances.push_back(std::move(stream));
          }
        }
      }
      for (const auto& stream : appearances) {
        RetainPtr<const CPDF_Dictionary> own =
            stream->GetDict()->GetDictFor("Resources");
        use.Walk(stream.Get(),
             CPDF_Form::ChooseResourcesDict(own.Get(), nullptr,
                                            page_resources.Get()),
             page_resources.Get());
      }
    }
  }

  for (const auto& [type, names] : use.used()) {
    seen_resources[type].insert(names.begin(), names.end());
  }
}

// EmbedPDF(faithful-content-rewrite): a colour space is written by the name the
// resources already give the array it was read from.
ByteString CPDF_PageContentGenerator::RealizeColorSpaceObject(
    const CPDF_ColorSpace* cs) {
  if (auto it = color_space_names_.find(cs); it != color_space_names_.end()) {
    return it->second;
  }
  RetainPtr<const CPDF_Array> array = cs ? cs->GetArray() : nullptr;
  const ByteString key = ColorSpaceKey(array.Get());
  if (key.IsEmpty()) {
    return ByteString();
  }

  // The name these resources give it: the one this holder gave it before,
  // or the /CS0 the content was written with. Only a colour space they don't
  // name yet gets a name of its own.
  const ByteString hint =
      obj_holder_->ColorSpaceMapSearch(key).value_or(ByteString());
  ByteString name =
      FindColorSpaceName(obj_holder_->GetResources().Get(), key, hint);
  if (name.IsEmpty()) {
    // A resource is written as a reference: a direct array gets a copy of
    // its own to refer to.
    RetainPtr<const CPDF_Object> resource = array;
    if (!array->GetObjNum()) {
      RetainPtr<CPDF_Object> copy = array->Clone();
      document_->AddIndirectObject(copy);
      resource = std::move(copy);
    }
    name = RealizeResource(resource.Get(), "ColorSpace");
  }
  obj_holder_->ColorSpaceMapInsert(key, name);
  color_space_names_[cs] = name;
  return name;
}

// EmbedPDF(faithful-content-rewrite): a colour as it was read: device
// colours by their operators, any other space by its name, patterns with
// their pattern colour space.
bool CPDF_PageContentGenerator::EmitColor(fxcrt::ostringstream& buf,
                                          const CPDF_Color* color,
                                          bool is_stroke,
                                          CPDF_PageObject* owner) {
  if (!color || !color->GetColorSpace()) {
    return false;
  }

  CPDF_ColorState& color_state = owner->mutable_color_state();
  auto set_color_space_name = [&](ByteString name) {
    if (is_stroke) {
      color_state.SetStrokeColorSpaceResName(std::move(name));
    } else {
      color_state.SetFillColorSpaceResName(std::move(name));
    }
  };
  const char* const select_color_space = is_stroke ? " CS " : " cs ";
  const char* const set_color = is_stroke ? " SCN " : " scn ";

  // Pattern colours: "/Pattern cs /P0 scn". An uncoloured pattern paints in a
  // colour of its pattern colour space's base, so that space is written
  // instead, with the colour before the pattern: "/CS0 cs 0 .6 0 /P0 scn".
  if (color->IsPattern()) {
    RetainPtr<CPDF_Pattern> pattern = color->GetPattern();
    if (!pattern)
      return false;
    
    // Get the pattern's underlying PDF object (stream or dict)
    RetainPtr<CPDF_Object> pattern_obj = pattern->pattern_obj();
    if (!pattern_obj)
      return false;
    
    // Realize the pattern as a resource in the Pattern dictionary
    ByteString pattern_name = RealizeResource(pattern_obj.Get(), "Pattern");
    if (pattern_name.IsEmpty())
      return false;
    
    // Store the pattern resource name for resource tracking
    if (is_stroke) {
      owner->mutable_color_state().SetStrokePatternResName(pattern_name);
    } else {
      owner->mutable_color_state().SetFillPatternResName(pattern_name);
    }

    const ByteString color_space =
        RealizeColorSpaceObject(color->GetColorSpace());
    set_color_space_name(color_space);
    buf << "/"
        << PDF_NameEncode(color_space.IsEmpty() ? ByteString("Pattern")
                                                : color_space)
        << select_color_space;
    for (float component : color->GetPatternComps()) {
      WriteFloat(buf, component) << " ";
    }
    buf << "/" << PDF_NameEncode(pattern_name) << set_color;
    return true;
  }

  if (color->IsColorSpaceGray()) {
    auto comps = color->GetRawNonPatternComps();
    if (comps.size() == 1) {
      WriteFloat(buf, comps[0]) << (is_stroke ? " G " : " g ");
      set_color_space_name({});
      return true;
    }
    return false;
  }

  if (color->IsColorSpaceRGB()) {
    auto rgb = color->GetRGB();
    if (!rgb) return false;
    WriteFloat(buf, rgb->red)   << " ";
    WriteFloat(buf, rgb->green) << " ";
    WriteFloat(buf, rgb->blue)  << (is_stroke ? " RG " : " rg ");
    set_color_space_name({});
    return true;
  }

  if (color->IsColorSpaceCMYK()) {
    auto comps = color->GetRawNonPatternComps(); // expect 4
    if (comps.size() == 4) {
      WriteFloat(buf, comps[0]) << " ";
      WriteFloat(buf, comps[1]) << " ";
      WriteFloat(buf, comps[2]) << " ";
      WriteFloat(buf, comps[3]) << (is_stroke ? " K " : " k ");
      set_color_space_name({});
      return true;
    }
    return false;
  }

  // Every other colour space - CalGray, CalRGB, Lab, ICCBased, Indexed,
  // Separation, DeviceN - is written as the array it was read from, by name:
  // "/CS0 cs 1 scn".
  const ByteString color_space = RealizeColorSpaceObject(color->GetColorSpace());
  if (color_space.IsEmpty()) {
    // One not read from an array: its RGB equivalent, never no colour (which
    // would leave the object black).
    std::optional<FX_RGB_STRUCT<float>> rgb = color->GetRGB();
    if (!rgb) {
      return false;
    }
    WriteFloat(buf, rgb->red) << " ";
    WriteFloat(buf, rgb->green) << " ";
    WriteFloat(buf, rgb->blue) << (is_stroke ? " RG " : " rg ");
    set_color_space_name({});
    return true;
  }

  set_color_space_name(color_space);
  buf << "/" << PDF_NameEncode(color_space) << select_color_space;
  auto comps = color->GetRawNonPatternComps();
  for (size_t i = 0; i < comps.size(); ++i) {
    if (i) buf << " ";
    WriteFloat(buf, comps[i]);
  }
  buf << set_color;
  return true;
}

ByteString CPDF_PageContentGenerator::RealizeResource(
    const CPDF_Object* pResource,
    ByteStringView type) const {
  DCHECK(pResource);
  if (!obj_holder_->GetResources()) {
    // EmbedPDF(holder-readback): name the new dictionary in /Resources before
    // the holder holds it, and never read it back through GetResources(). On
    // a layer document creating the dictionary is a write, and a page re-reads
    // /Resources from its dictionary after every write: read back first, it
    // finds none.
    RetainPtr<CPDF_Dictionary> resources =
        document_->NewIndirect<CPDF_Dictionary>();
    obj_holder_->GetMutableDict()->SetNewFor<CPDF_Reference>(
        pdfium::page_object::kResources, document_, resources->GetObjNum());
    obj_holder_->SetResources(std::move(resources));
  }

  RetainPtr<CPDF_Dictionary> resource_dict =
      obj_holder_->GetMutableResources()->GetOrCreateDictFor(type);
  const auto& all_removed_resources_map =
      obj_holder_->all_removed_resources_map();
  auto it = all_removed_resources_map.find(type);
  const CPDF_PageObjectHolder::RemovedResourceMap* removed_resource_map =
      it != all_removed_resources_map.end() ? &it->second : nullptr;
  ByteString name;
  int idnum = 1;
  while (true) {
    name = ByteString::Format("FX%c%d", type[0], idnum);
    // Avoid name collisions with existing `resource_dict` entries.
    if (resource_dict->KeyExist(name.AsStringView())) {
      idnum++;
      continue;
    }

    // Also avoid collisions with entries in `removed_resource_map`, since those
    // entries may move back into `resource_dict`.
    if (removed_resource_map && pdfium::Contains(*removed_resource_map, name)) {
      idnum++;
      continue;
    }

    resource_dict->SetNewFor<CPDF_Reference>(name, document_,
                                             pResource->GetObjNum());
    return name;
  }
}

bool CPDF_PageContentGenerator::ProcessPageObjects(fxcrt::ostringstream* buf) {
  bool bDirty = false;
  std::unique_ptr<const CPDF_ContentMarks> empty_content_marks =
      std::make_unique<CPDF_ContentMarks>();
  const CPDF_ContentMarks* content_marks = empty_content_marks.get();

  for (auto& pPageObj : page_objects_) {
    if (obj_holder_->IsPage() &&
        (!pPageObj->IsDirty() || !pPageObj->IsActive())) {
      continue;
    }

    bDirty = true;
    content_marks = ProcessContentMarks(buf, pPageObj, content_marks);
    ProcessPageObject(buf, pPageObj);
  }
  FinishMarks(buf, content_marks);
  return bDirty;
}

void CPDF_PageContentGenerator::UpdateStreamlessPageObjects(
    int new_content_stream_index) {
  for (auto& pPageObj : page_objects_) {
    if (!pPageObj->IsActive()) {
      continue;
    }

    if (pPageObj->GetContentStream() == CPDF_PageObject::kNoContentStream) {
      pPageObj->SetContentStream(new_content_stream_index);
    }
  }
}

const CPDF_ContentMarks* CPDF_PageContentGenerator::ProcessContentMarks(
    fxcrt::ostringstream* buf,
    const CPDF_PageObject* pPageObj,
    const CPDF_ContentMarks* pPrev) {
  const CPDF_ContentMarks* pNext = pPageObj->GetContentMarks();
  const size_t first_different = pPrev->FindFirstDifference(pNext);

  // Close all marks that are in prev but not in next.
  // Technically we should iterate backwards to close from the top to the
  // bottom, but since the EMC operators do not identify which mark they are
  // closing, it does not matter.
  for (size_t i = first_different; i < pPrev->CountItems(); ++i) {
    *buf << "EMC\n";
  }

  // Open all marks that are in next but not in prev.
  for (size_t i = first_different; i < pNext->CountItems(); ++i) {
    const CPDF_ContentMarkItem* item = pNext->GetItem(i);

    // Write mark tag.
    *buf << "/" << PDF_NameEncode(item->GetName()) << " ";

    // If there are no parameters, write a BMC (begin marked content) operator.
    if (item->GetParamType() == CPDF_ContentMarkItem::kNone) {
      *buf << "BMC\n";
      continue;
    }

    // If there are parameters, write properties, direct or indirect.
    switch (item->GetParamType()) {
      case CPDF_ContentMarkItem::kDirectDict: {
        CPDF_StringArchiveStream archive_stream(buf);
        item->GetParam()->WriteTo(&archive_stream, nullptr);
        *buf << " ";
        break;
      }
      case CPDF_ContentMarkItem::kPropertiesDict: {
        *buf << "/" << PDF_NameEncode(item->GetPropertyName()) << " ";
        break;
      }
      case CPDF_ContentMarkItem::kNone:
        NOTREACHED();
    }

    // Write BDC (begin dictionary content) operator.
    *buf << "BDC\n";
  }

  return pNext;
}

void CPDF_PageContentGenerator::FinishMarks(
    fxcrt::ostringstream* buf,
    const CPDF_ContentMarks* pContentMarks) {
  // Technically we should iterate backwards to close from the top to the
  // bottom, but since the EMC operators do not identify which mark they are
  // closing, it does not matter.
  for (size_t i = 0; i < pContentMarks->CountItems(); ++i) {
    *buf << "EMC\n";
  }
}

void CPDF_PageContentGenerator::ProcessPageObject(fxcrt::ostringstream* buf,
                                                  CPDF_PageObject* pPageObj) {
  // EmbedPDF(faithful-content-rewrite): shading objects are written too.
  if (CPDF_ImageObject* pImageObject = pPageObj->AsImage()) {
    ProcessImage(buf, pImageObject);
  } else if (CPDF_FormObject* pFormObj = pPageObj->AsForm()) {
    ProcessForm(buf, pFormObj);
  } else if (CPDF_PathObject* pPathObj = pPageObj->AsPath()) {
    ProcessPath(buf, pPathObj);
  } else if (CPDF_TextObject* pTextObj = pPageObj->AsText()) {
    ProcessText(buf, pTextObj);
  } else if (CPDF_ShadingObject* pShadingObj = pPageObj->AsShading()) {
    ProcessShading(buf, pShadingObj);
  }
  pPageObj->SetDirty(false);
}

void CPDF_PageContentGenerator::ProcessImage(fxcrt::ostringstream* buf,
                                             CPDF_ImageObject* pImageObj) {
  const CFX_Matrix& matrix = pImageObj->matrix();
  if ((matrix.a == 0 && matrix.b == 0) || (matrix.c == 0 && matrix.d == 0)) {
    return;
  }

  RetainPtr<CPDF_Image> pImage = pImageObj->GetImage();
  // EmbedPDF(faithful-content-rewrite): inline images (BI...ID...EI) are
  // converted to XObject images below; upstream dropped them. Do NOT return
  // early here - the conversion happens at ConvertStreamToIndirectObject().

  RetainPtr<const CPDF_Stream> pStream = pImage->GetStream();
  if (!pStream) {
    return;
  }

  ProcessGraphics(buf, pImageObj);

  if (!matrix.IsIdentity()) {
    WriteMatrix(*buf, matrix) << " cm ";
  }

  bool bWasInline = pStream->IsInline();
  if (bWasInline) {
    pImage->ConvertStreamToIndirectObject();
  }

  ByteString name = RealizeResource(pStream, "XObject");
  pImageObj->SetResourceName(name);

  if (bWasInline) {
    auto* pPageData = CPDF_DocPageData::FromDocument(document_);
    pImageObj->SetImage(pPageData->GetImage(pStream->GetObjNum()));
  }

  *buf << "/" << PDF_NameEncode(name) << " Do";
  EndProcessGraphics(*buf);
}

void CPDF_PageContentGenerator::ProcessForm(fxcrt::ostringstream* buf,
                                            CPDF_FormObject* pFormObj) {
  CPDF_Form* form_xobject = pFormObj->form();
  if (form_xobject->HasDirtyStreams()) {
    // The Form XObject itself has modified content (e.g., an object was
    // removed). We need to regenerate its content stream before using it.
    // EmbedPDF(faithful-content-rewrite): a form placed in more than one
    // place (a drawing several stamps share) is copied first, so the other
    // placements keep what they draw; this placement then draws the copy.
    RetainPtr<const CPDF_Stream> stream = form_xobject->GetStream();
    CPDF_Document* document = form_xobject->GetDocument();
    if (stream && stream->GetObjNum() && document &&
        pdfium::Contains(GetObjectsWithMultipleReferences(document),
                         stream->GetObjNum())) {
      form_xobject->CloneBackingStreamForWrite();
    }
    CPDF_PageContentGenerator form_content_generator(form_xobject);
    form_content_generator.GenerateContent();
  }

  const CFX_Matrix& matrix = pFormObj->form_matrix();
  if ((matrix.a == 0 && matrix.b == 0) || (matrix.c == 0 && matrix.d == 0)) {
    return;
  }

  RetainPtr<const CPDF_Stream> pStream = pFormObj->form()->GetStream();
  CHECK(pStream);
  ByteString name = RealizeResource(pStream.Get(), "XObject");
  pFormObj->SetResourceName(name);

  ProcessGraphics(buf, pFormObj);
  // EmbedPDF(faithful-content-rewrite): the text state the form inherits.
  WriteInheritedTextState(*buf, pFormObj);

  if (!matrix.IsIdentity()) {
    WriteMatrix(*buf, matrix) << " cm ";
  }

  *buf << "/" << PDF_NameEncode(name) << " Do";
  EndProcessGraphics(*buf);
}

// EmbedPDF(faithful-content-rewrite): shading objects (sh) are written back;
// upstream dropped them.
void CPDF_PageContentGenerator::ProcessShading(fxcrt::ostringstream* buf,
                                               CPDF_ShadingObject* pShadingObj) {
  const CPDF_ShadingPattern* pattern = pShadingObj->pattern();
  if (!pattern)
    return;

  // Get the underlying shading object (dictionary or stream)
  RetainPtr<const CPDF_Object> shading_obj = pattern->GetShadingObject();
  if (!shading_obj)
    return;

  ProcessGraphics(buf, pShadingObj);

  // Apply the shading object's transformation matrix
  const CFX_Matrix& matrix = pShadingObj->matrix();
  if (!matrix.IsIdentity()) {
    WriteMatrix(*buf, matrix) << " cm ";
  }

  // Realize the shading as a resource
  ByteString name = RealizeResource(shading_obj.Get(), "Shading");
  pShadingObj->SetResourceName(name);

  // Emit the shading operator with BX/EX compatibility markers
  *buf << "BX /" << PDF_NameEncode(name) << " sh EX";
  EndProcessGraphics(*buf);
}

// Processing path construction with operators from Table 4.9 of PDF spec 1.7:
// "re" appends a rectangle (here, used only if the whole path is a rectangle)
// "m" moves current point to the given coordinates
// "l" creates a line from current point to the new point
// "c" adds a Bezier curve from current to last point, using the two other
// points as the Bezier control points
// Note: "l", "c" change the current point
// "h" closes the subpath (appends a line from current to starting point)
void CPDF_PageContentGenerator::ProcessPathPoints(fxcrt::ostringstream* buf,
                                                  CPDF_Path* pPath) {
  pdfium::span<const CFX_Path::Point> points = pPath->GetPoints();
  std::optional<CFX_PointF> corner = pPath->GetRectOppositeCorner();
  if (corner.has_value()) {
    CFX_PointF diff = corner.value() - points[0].point_;
    WritePoint(*buf, points[0].point_) << " ";
    WritePoint(*buf, diff) << " re";
    return;
  }
  for (size_t i = 0; i < points.size(); ++i) {
    if (i > 0) {
      *buf << " ";
    }

    WritePoint(*buf, points[i].point_);

    CFX_Path::Point::Type point_type = points[i].type_;
    if (point_type == CFX_Path::Point::Type::kMove) {
      *buf << " m";
    } else if (point_type == CFX_Path::Point::Type::kLine) {
      *buf << " l";
    } else if (point_type == CFX_Path::Point::Type::kBezier) {
      if (i + 2 >= points.size() ||
          !points[i].IsTypeAndOpen(CFX_Path::Point::Type::kBezier) ||
          !points[i + 1].IsTypeAndOpen(CFX_Path::Point::Type::kBezier) ||
          points[i + 2].type_ != CFX_Path::Point::Type::kBezier) {
        // If format is not supported, close the path and paint
        *buf << " h";
        break;
      }
      *buf << " ";
      WritePoint(*buf, points[i + 1].point_) << " ";
      WritePoint(*buf, points[i + 2].point_) << " c";
      i += 2;
    }
    if (points[i].close_figure_) {
      *buf << " h";
    }
  }
}

// Processing path painting with operators from Table 4.10 of PDF spec 1.7:
// Path painting operators: "S", "n", "B", "f", "B*", "f*", depending on
// the filling mode and whether we want stroking the path or not.
void CPDF_PageContentGenerator::ProcessPath(fxcrt::ostringstream* buf,
                                            CPDF_PathObject* pPathObj) {
  ProcessGraphics(buf, pPathObj);

  const CFX_Matrix& matrix = pPathObj->matrix();
  if (!matrix.IsIdentity()) {
    WriteMatrix(*buf, matrix) << " cm ";
  }

  ProcessPathPoints(buf, &pPathObj->path());

  if (pPathObj->has_no_filltype()) {
    *buf << (pPathObj->stroke() ? " S" : " n");
  } else if (pPathObj->has_winding_filltype()) {
    *buf << (pPathObj->stroke() ? " B" : " f");
  } else if (pPathObj->has_alternate_filltype()) {
    *buf << (pPathObj->stroke() ? " B*" : " f*");
  }
  EndProcessGraphics(*buf);
}

// This method supports color operators rg and RGB from Table 4.24 of PDF spec
// 1.7. A color will not be set if the colorspace is not DefaultRGB or the RGB
// values cannot be obtained. The method also adds an external graphics
// dictionary, as described in Section 4.3.4.
// "rg" sets the fill color, "RG" sets the stroke color (using DefaultRGB)
// "w" sets the stroke line width.
// "ca" sets the fill alpha, "CA" sets the stroke alpha.
// "W" and "W*" modify the clipping path using the nonzero winding rule and
// even-odd rules, respectively.
// "q" saves the graphics state, so that the settings can later be reversed
void CPDF_PageContentGenerator::ProcessGraphics(fxcrt::ostringstream* buf,
                                                CPDF_PageObject* pPageObj) {
  *buf << "q ";
  // EmbedPDF(faithful-content-rewrite): colours in their own colour space, by
  // its name (EmitColor()). Upstream wrote RGB and gray only, as RGB.
  if (const CPDF_Color* fill = pPageObj->color_state().GetFillColor()) {
    EmitColor(*buf, fill, /*is_stroke=*/false, pPageObj);
  }
  if (const CPDF_Color* stroke = pPageObj->color_state().GetStrokeColor()) {
    EmitColor(*buf, stroke, /*is_stroke=*/true, pPageObj);
  }
  // EmbedPDF(layer-transaction): names the resources no longer define are
  // forgotten, and the state is made again (ProcessGraphicsState): an aborted
  // layer transaction drops the graphics state this generator made for the
  // object, while the object keeps its name.
  if (!std::ranges::all_of(pPageObj->GetGraphicsResourceNames(),
                           [this](const ByteString& name) {
                             return obj_holder_->ResourcesDefine("ExtGState",
                                                                 name);
                           })) {
    pPageObj->mutable_general_state().SetGraphicsResourceNames({});
  }
  // EmbedPDF(faithful-content-rewrite): an ExtGState can set the line width,
  // cap, join, miter limit and dash too. The object's values are the ones the
  // content set last, so after an ExtGState that sets any of them, all of
  // them are written.
  const CFX_GraphState& graph_state = pPageObj->graph_state();
  const bool graphics_state_sets_lines =
      SetsLineState(obj_holder_->GetResources().Get(),
                    pPageObj->GetGraphicsResourceNames());
  if (!graphics_state_sets_lines) {
    WriteLineState(*buf, graph_state, /*all=*/false);
  }
  ProcessClip(buf, pPageObj);
  ProcessGraphicsState(buf, pPageObj);
  if (graphics_state_sets_lines) {
    WriteLineState(*buf, graph_state, /*all=*/true);
  }
}

// EmbedPDF(faithful-content-rewrite): clip paths as upstream wrote them, then
// the text that clips, which upstream dropped.
void CPDF_PageContentGenerator::ProcessClip(fxcrt::ostringstream* buf,
                                            CPDF_PageObject* pPageObj) {
  const CPDF_ClipPath& clip_path = pPageObj->clip_path();
  if (!clip_path.HasRef()) {
    return;
  }
  for (size_t i = 0; i < clip_path.GetPathCount(); ++i) {
    CPDF_Path path = clip_path.GetPath(i);
    ProcessPathPoints(buf, &path);
    switch (clip_path.GetClipType(i)) {
      case CFX_FillRenderOptions::FillType::kWinding:
        *buf << " W ";
        break;
      case CFX_FillRenderOptions::FillType::kEvenOdd:
        *buf << " W* ";
        break;
      case CFX_FillRenderOptions::FillType::kNoFill:
        NOTREACHED();
    }

    // Use a no-op path-painting operator to terminate the path without
    // causing any marks to be placed on the page.
    *buf << "n ";
  }

  // Each text object of a BT ... ET that clipped intersects the clip with its
  // glyphs at the ET. They are written invisible (7 Tr): text that also
  // paints (modes 4 to 6) is drawn by its own object. A null entry ends each
  // BT ... ET.
  const size_t text_count = clip_path.GetTextCount();
  for (size_t i = 0; i < text_count; ++i) {
    *buf << "BT ";
    for (; i < text_count && clip_path.GetText(i); ++i) {
      CPDF_TextObject* text = clip_path.GetText(i);
      RetainPtr<CPDF_Font> font;
      const ByteString font_name = RealizeFont(text, &font);
      if (font_name.IsEmpty()) {
        continue;
      }
      WriteMatrix(*buf, text->GetTextMatrix()) << " Tm ";
      WriteTextShowing(*buf, text, font.Get(), font_name,
                       TextRenderingMode::MODE_CLIP);
      // Spacing is text state: the text drawn next would inherit it.
      if (text->GetCharSpace() != 0.0f) {
        *buf << " 0 Tc";
      }
      if (text->GetWordSpace() != 0.0f) {
        *buf << " 0 Tw";
      }
      *buf << " ";
    }
    *buf << "ET ";
  }
}

// EmbedPDF(faithful-content-rewrite): the ExtGStates the content set, by their
// names, so soft masks, overprint and the rest survive. Upstream only made one
// from the alpha and blend mode.
void CPDF_PageContentGenerator::ProcessGraphicsState(
    fxcrt::ostringstream* buf,
    CPDF_PageObject* pPageObj) {
  // Emit any existing graphics state resources first (these may contain
  // properties beyond alpha/blend like soft masks, overprint modes, etc.)
  pdfium::span<const ByteString> existing_gs_names =
      pPageObj->GetGraphicsResourceNames();
  if (!existing_gs_names.empty()) {
    // EmbedPDF(faithful-content-rewrite): a soft mask is placed by the CTM in
    // effect when the gs that set it ran, which the objects' matrices no
    // longer carry: set that CTM around the gs operators.
    const CPDF_GeneralState& general_state = pPageObj->general_state();
    const CFX_Matrix* mask_ctm = general_state.GetSoftMask()
                                     ? general_state.GetSMaskMatrix()
                                     : nullptr;
    const bool place_mask =
        mask_ctm && !mask_ctm->IsIdentity() && mask_ctm->IsInvertible();
    if (place_mask) {
      WriteMatrix(*buf, *mask_ctm) << " cm ";
    }
    // Emit all existing ExtGState resources with their original names
    for (const ByteString& gs_name : existing_gs_names) {
      *buf << "/" << PDF_NameEncode(gs_name) << " gs ";
    }
    if (place_mask) {
      WriteMatrix(*buf, mask_ctm->GetInverse()) << " cm ";
    }
    return;
  }

  // No existing graphics state - check if we need to create one for alpha/blend
  GraphicsData graphD;
  graphD.fillAlpha = pPageObj->general_state().GetFillAlpha();
  graphD.strokeAlpha = pPageObj->general_state().GetStrokeAlpha();
  graphD.blendType = pPageObj->general_state().GetBlendType();
  if (graphD.fillAlpha == 1.0f && graphD.strokeAlpha == 1.0f &&
      graphD.blendType == BlendMode::kNormal) {
    return;
  }

  ByteString name;
  std::optional<ByteString> maybe_name = obj_holder_->GraphicsMapSearch(graphD);
  if (maybe_name.has_value()) {
    name = std::move(maybe_name.value());
  } else {
    auto gsDict = document_->New<CPDF_Dictionary>();
    if (graphD.fillAlpha != 1.0f) {
      gsDict->SetNewFor<CPDF_Number>("ca", graphD.fillAlpha);
    }

    if (graphD.strokeAlpha != 1.0f) {
      gsDict->SetNewFor<CPDF_Number>("CA", graphD.strokeAlpha);
    }

    if (graphD.blendType != BlendMode::kNormal) {
      gsDict->SetNewFor<CPDF_Name>("BM",
                                   pPageObj->general_state().GetBlendMode());
    }
    document_->AddIndirectObject(gsDict);
    name = RealizeResource(std::move(gsDict), "ExtGState");
    pPageObj->mutable_general_state().SetGraphicsResourceNames({name});
    obj_holder_->GraphicsMapInsert(graphD, name);
  }
  *buf << "/" << PDF_NameEncode(name) << " gs ";
}

void CPDF_PageContentGenerator::ProcessDefaultGraphics(
    fxcrt::ostringstream* buf) {
  // EmbedPDF(faithful-content-rewrite): no default black (upstream wrote
  // `0 0 0 RG 0 0 0 rg`): objects write their own colours, and content that
  // sets none keeps the colour it inherits.
  *buf << "1 w "
       << static_cast<int>(CFX_GraphStateData::LineCap::kButt) << " J "
       << static_cast<int>(CFX_GraphStateData::LineJoin::kMiter) << " j\n";
  default_graphics_name_ = GetOrCreateDefaultGraphics();
  *buf << "/" << PDF_NameEncode(default_graphics_name_) << " gs ";
}

ByteString CPDF_PageContentGenerator::GetOrCreateDefaultGraphics() const {
  GraphicsData defaultGraphics;
  defaultGraphics.fillAlpha = 1.0f;
  defaultGraphics.strokeAlpha = 1.0f;
  defaultGraphics.blendType = BlendMode::kNormal;

  std::optional<ByteString> maybe_name =
      obj_holder_->GraphicsMapSearch(defaultGraphics);
  if (maybe_name.has_value()) {
    return maybe_name.value();
  }

  auto gsDict = document_->New<CPDF_Dictionary>();
  gsDict->SetNewFor<CPDF_Number>("ca", defaultGraphics.fillAlpha);
  gsDict->SetNewFor<CPDF_Number>("CA", defaultGraphics.strokeAlpha);
  gsDict->SetNewFor<CPDF_Name>("BM", "Normal");
  document_->AddIndirectObject(gsDict);
  ByteString name = RealizeResource(std::move(gsDict), "ExtGState");
  obj_holder_->GraphicsMapInsert(defaultGraphics, name);
  return name;
}

// This method adds text to the buffer, BT begins the text object, ET ends it.
// Tm sets the text matrix (allows positioning and transforming text).
// Tf sets the font name (from Font in Resources) and font size.
// Tr sets the text rendering mode.
// TJ sets the actual text, <####...> is used when specifying charcodes.
void CPDF_PageContentGenerator::ProcessText(fxcrt::ostringstream* buf,
                                            CPDF_TextObject* pTextObj) {
  ProcessGraphics(buf, pTextObj);

  // EmbedPDF(faithful-content-rewrite): the text's own font, spacing and TJ
  // adjustments are written (RealizeFont(), WriteTextShowing()). Separate
  // translation (cm) from pure text matrix (Tm).
  const CFX_Matrix& M = pTextObj->GetTextMatrix();
  if (M.e != 0 || M.f != 0) {
    WriteMatrix(*buf, CFX_Matrix(1, 0, 0, 1, M.e, M.f)) << " cm ";
  }

  *buf << "BT ";

  CFX_Matrix TmNoTranslate(M.a, M.b, M.c, M.d, 0, 0);
  if (!TmNoTranslate.IsIdentity()) {
    WriteMatrix(*buf, TmNoTranslate) << " Tm ";
  } else {
    *buf << "1 0 0 1 0 0 Tm ";
  }

  RetainPtr<CPDF_Font> font;
  const ByteString font_name = RealizeFont(pTextObj, &font);
  if (font_name.IsEmpty()) {
    *buf << "ET";  // bail out cleanly
    EndProcessGraphics(*buf);
    return;
  }
  WriteTextShowing(*buf, pTextObj, font.Get(), font_name,
                   pTextObj->GetTextRenderMode());
  *buf << " ET";
  EndProcessGraphics(*buf);
}

// EmbedPDF(faithful-content-rewrite): a font is named after its own object,
// so fonts sharing a type and base name keep their own dictionaries.
// Upstream named fonts by type and base name only (now the last resort).
ByteString CPDF_PageContentGenerator::RealizeFont(CPDF_TextObject* text,
                                                  RetainPtr<CPDF_Font>* font) {
  *font = text->GetFont();
  ByteString dict_name = RealizeFont(font);
  if (!dict_name.IsEmpty()) {
    text->SetResourceName(dict_name);
  }
  return dict_name;
}

ByteString CPDF_PageContentGenerator::RealizeFont(RetainPtr<CPDF_Font>* font) {
  // Ensure we have a font.
  if (!*font) {
    *font = CPDF_Font::GetStockFont(document_, "Helvetica");
  }

  // --- Object-number keyed font resource binding ---
  // Get the font dictionary; if it's inline, make it indirect so it has a stable objnum.
  RetainPtr<const CPDF_Object> pFontDict = (*font)->GetFontDict();
  if (pFontDict && pFontDict->IsInline()) {
    RetainPtr<CPDF_Object> clone = pFontDict->Clone();
    document_->AddIndirectObject(clone);
    pFontDict = std::move(clone);
  }

  // Some (very old/odd) fonts may not expose a dict; fall back safely.
  uint32_t font_objnum = pFontDict ? pFontDict->GetObjNum() : 0;

  ByteString dict_name;
  if (font_objnum) {
    if (auto hit = obj_holder_->FontsByObjnumSearch(font_objnum)) {
      dict_name = *hit;
    } else {
      // Realize this exact font object into Resources/Font and cache by objnum.
      dict_name = RealizeResource(pFontDict.Get(), "Font");
      obj_holder_->FontsByObjnumInsert(font_objnum, dict_name);
    }
  } else {
    // Last-resort path (should be rare): name by (type, base name) like before.
    FontData data;
    const CPDF_FontEncoding* pEncoding = nullptr;
    if ((*font)->IsType1Font()) {
      data.type = "Type1";
      pEncoding = (*font)->AsType1Font()->GetEncoding();
    } else if ((*font)->IsTrueTypeFont()) {
      data.type = "TrueType";
      pEncoding = (*font)->AsTrueTypeFont()->GetEncoding();
    } else if ((*font)->IsCIDFont()) {
      data.type = "Type0";
    } else {
      return ByteString();
    }
    data.baseFont = (*font)->GetBaseFontName();

    if (auto hit = obj_holder_->FontsMapSearch(data)) {
      dict_name = *hit;
    } else {
      // Build a minimal indirect font dict (same as your old code).
      auto font_dict = document_->New<CPDF_Dictionary>();
      font_dict->SetNewFor<CPDF_Name>("Type", "Font");
      font_dict->SetNewFor<CPDF_Name>("Subtype", data.type);
      font_dict->SetNewFor<CPDF_Name>("BaseFont", data.baseFont);
      if (pEncoding) {
        font_dict->SetFor("Encoding",
                          pEncoding->Realize(document_->GetByteStringPool()));
      }
      document_->AddIndirectObject(font_dict);
      dict_name = RealizeResource(std::move(font_dict), "Font");
      obj_holder_->FontsMapInsert(data, dict_name);
    }
  }
  return dict_name;
}

// EmbedPDF(faithful-content-rewrite): a form inheriting text state gets
// Tf, Tr, Tc and Tw written in its q..Q.
void CPDF_PageContentGenerator::WriteInheritedTextState(
    fxcrt::ostringstream& buf,
    CPDF_FormObject* form_object) {
  const CPDF_TextState& state = form_object->text_state();
  if (!state.HasRef() || !ShowsText(form_object->form())) {
    return;
  }
  if (RetainPtr<CPDF_Font> font = state.GetFont()) {
    const ByteString font_name = RealizeFont(&font);
    if (!font_name.IsEmpty()) {
      inherited_font_names_.insert(font_name);
      buf << "/" << PDF_NameEncode(font_name) << " ";
      WriteFloat(buf, state.GetFontSize()) << " Tf ";
    }
  }
  buf << static_cast<int>(state.GetTextMode()) << " Tr ";
  WriteFloat(buf, state.GetCharSpace()) << " Tc ";
  WriteFloat(buf, state.GetWordSpace()) << " Tw ";
}

// EmbedPDF(faithful-content-rewrite): spacing, and a TJ array where the text
// was shown with adjustments; upstream wrote one Tj without either.
void CPDF_PageContentGenerator::WriteTextShowing(
    fxcrt::ostringstream& buf,
    CPDF_TextObject* text,
    CPDF_Font* font,
    const ByteString& font_name,
    TextRenderingMode render_mode) {
  buf << "/" << PDF_NameEncode(font_name) << " ";
  WriteFloat(buf, text->GetFontSize()) << " Tf ";
  buf << static_cast<int>(render_mode) << " Tr ";

  const float tc = text->GetCharSpace();
  const float tw = text->GetWordSpace();

  if (tc != 0.0f) WriteFloat(buf, tc) << " Tc ";
  if (tw != 0.0f) WriteFloat(buf, tw) << " Tw ";

  if (TextObjectNeedsTJ(text)) {
    WriteTextAsTJ(buf, text, font);
  } else {
    ByteString showing;
    for (uint32_t charcode : text->GetCharCodes()) {
      font->AppendChar(&showing, charcode);
    }
    buf << PDF_HexEncodeString(showing.AsStringView()) << " Tj";
  }
}
