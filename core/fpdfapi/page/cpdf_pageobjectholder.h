// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PAGE_CPDF_PAGEOBJECTHOLDER_H_
#define CORE_FPDFAPI_PAGE_CPDF_PAGEOBJECTHOLDER_H_

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "core/fpdfapi/page/cpdf_transparency.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"
#include "core/fxge/dib/fx_dib.h"

class CPDF_ContentParser;
class CPDF_Document;
class CPDF_PageObject;
// EmbedPDF(render-speedup, faithful-content-rewrite): for GetObjectGroups()
// and GetMutableFormStream().
class CPDF_PageObjectGroups;
class CPDF_Stream;
class PauseIndicatorIface;

// These structs are used to keep track of resources that have already been
// generated in the page object holder.
struct GraphicsData {
  float fillAlpha;
  float strokeAlpha;
  BlendMode blendType;

  bool operator<(const GraphicsData& other) const;
};

struct FontData {
  ByteString baseFont;
  ByteString type;

  bool operator<(const FontData& other) const;
};

class CPDF_PageObjectHolder {
 public:
  enum class ParseState : uint8_t { kNotParsed, kParsing, kParsed };

  // Key: The stream index.
  // Value: The current transformation matrix at the end of the stream.
  using CTMMap = std::map<int32_t, CFX_Matrix>;

  // Key: The dictionary key for a given removed resource.
  // Value: The removed object.
  using RemovedResourceMap = std::map<ByteString, RetainPtr<CPDF_Object>>;

  // Key: The resource dictionary name.
  // Value: The entries removed from that dictionary.
  using AllRemovedResourcesMap =
      std::map<ByteString, RemovedResourceMap, std::less<>>;

  using iterator = std::deque<std::unique_ptr<CPDF_PageObject>>::iterator;
  using const_iterator =
      std::deque<std::unique_ptr<CPDF_PageObject>>::const_iterator;

  CPDF_PageObjectHolder(CPDF_Document* doc,
                        RetainPtr<CPDF_Dictionary> dict,
                        RetainPtr<CPDF_Dictionary> pPageResources,
                        RetainPtr<CPDF_Dictionary> pResources);
  virtual ~CPDF_PageObjectHolder();

  virtual bool IsPage() const;

  // EmbedPDF(faithful-content-rewrite): returns the mutable Form XObject
  // stream for CPDF_Form, or nullptr for Pages. Used by
  // CPDF_PageContentManager to update Form XObject content directly.
  virtual RetainPtr<CPDF_Stream> GetMutableFormStream();

  void StartParse(std::unique_ptr<CPDF_ContentParser> pParser);
  void ContinueParse(PauseIndicatorIface* pPause);
  ParseState GetParseState() const { return parse_state_; }

  // EmbedPDF(page-residency): moves whenever the objects change once parsed:
  // an object added or removed, or content generated from edited objects.
  uint64_t GetObjectEdits() const { return object_edits_; }
  void NoteObjectsEdited() { ++object_edits_; }

  CPDF_Document* GetDocument() const { return document_; }
  // EmbedPDF(overlay-epoch, layer-door): the getters re-read the dictionary
  // and resources when the overlay epoch moved, and the mutable ones copy up
  // through the layer's door when used. Virtual so CPDF_Page can re-read
  // inherited resources.
  virtual RetainPtr<const CPDF_Dictionary> GetDict() const;
  RetainPtr<CPDF_Dictionary> GetMutableDict();
  virtual RetainPtr<const CPDF_Dictionary> GetResources() const;
  RetainPtr<CPDF_Dictionary> GetMutableResources();
  void SetResources(RetainPtr<CPDF_Dictionary> dict);
  virtual RetainPtr<const CPDF_Dictionary> GetPageResources() const;
  RetainPtr<CPDF_Dictionary> GetMutablePageResources();
  size_t GetPageObjectCount() const { return page_object_list_.size(); }
  size_t GetActivePageObjectCount() const;
  CPDF_PageObject* GetPageObjectByIndex(size_t index) const;
  void AppendPageObject(std::unique_ptr<CPDF_PageObject> pPageObj);

  bool InsertPageObjectAtIndex(size_t index,
                               std::unique_ptr<CPDF_PageObject> page_obj);

  // Remove `pPageObj` if present, and transfer ownership to the caller.
  std::unique_ptr<CPDF_PageObject> RemovePageObject(CPDF_PageObject* pPageObj);
  bool ErasePageObjectAtIndex(size_t index);

  iterator begin() { return page_object_list_.begin(); }
  const_iterator begin() const { return page_object_list_.begin(); }

  iterator end() { return page_object_list_.end(); }
  const_iterator end() const { return page_object_list_.end(); }

  // EmbedPDF(render-speedup): the bounds of runs of consecutive objects, for
  // a render whose clip test is `clip`. Returns nullptr while the list is
  // still being parsed, when there are too few objects for runs to pay, and
  // when out-of-date runs would have to be computed for a clip that shows
  // more than half of the page's box or the form's /BBox, where few runs
  // miss it.
  const CPDF_PageObjectGroups* GetObjectGroups(const CFX_FloatRect& clip) const;

  // Turns the object groups off, so a test can render without them and
  // compare.
  static void SetObjectGroupsEnabledForTesting(bool enabled);

  const CFX_FloatRect& GetBBox() const { return bbox_; }

  const CPDF_Transparency& GetTransparency() const { return transparency_; }
  bool BackgroundAlphaNeeded() const { return background_alpha_needed_; }
  void SetBackgroundAlphaNeeded(bool needed) {
    background_alpha_needed_ = needed;
  }

  bool HasImageMask() const { return !mask_bounding_boxes_.empty(); }
  const std::vector<CFX_FloatRect>& GetMaskBoundingBoxes() const {
    return mask_bounding_boxes_;
  }
  void AddImageMaskBoundingBox(const CFX_FloatRect& box);
  bool HasDirtyStreams() const { return !dirty_streams_.empty(); }
  std::set<int32_t> TakeDirtyStreams();
  // EmbedPDF(redaction): content stream `stream` is to be written again
  // though none of its objects changed: what changed is what it may name.
  void MarkStreamDirty(int32_t stream) { dirty_streams_.insert(stream); }

  // EmbedPDF(redaction): when content is next written, the resources' entry
  // `name` of `type` names object `replacement` (CPDF_PageContentGenerator).
  using ResourceReplacements =
      std::map<std::pair<ByteString, ByteString>, uint32_t>;
  void ReplaceResource(const ByteString& type,
                       const ByteString& name,
                       uint32_t replacement) {
    resource_replacements_[{type, name}] = replacement;
  }
  ResourceReplacements TakeResourceReplacements() {
    return std::move(resource_replacements_);
  }

  // EmbedPDF(layer-transaction): the name caches below remember which
  // resource name the content generator gave a font, graphics state or
  // colour space. A cached name counts only while the resources still define
  // it: an aborted layer transaction drops the names it added, and generated
  // content must never use a name nothing defines.
  bool ResourcesDefine(ByteStringView category, const ByteString& name) const;
  std::optional<ByteString> GraphicsMapSearch(const GraphicsData& gd);
  void GraphicsMapInsert(const GraphicsData& gd, const ByteString& str);

  std::optional<ByteString> FontsMapSearch(const FontData& fd);
  void FontsMapInsert(const FontData& fd, const ByteString& str);

  // EmbedPDF(faithful-content-rewrite): a font is named by its object, so
  // rewritten text keeps the font it was read with; a colour space by the
  // array it was read from.
  std::optional<ByteString> FontsByObjnumSearch(uint32_t objnum);
  void FontsByObjnumInsert(uint32_t objnum, const ByteString& name);

  std::optional<ByteString> ColorSpaceMapSearch(const ByteString& key);
  void ColorSpaceMapInsert(const ByteString& key, const ByteString& name);

  // `stream` must be non-negative or `CPDF_PageObject::kNoContentStream`.
  CFX_Matrix GetCTMAtBeginningOfStream(int32_t stream);

  // `stream` must be non-negative.
  CFX_Matrix GetCTMAtEndOfStream(int32_t stream);

  // EmbedPDF(faithful-content-rewrite): for a rewrite of every content
  // stream. The parser recorded the CTM each stream ended at, which the next
  // one began with. Streams written again take nothing from each other, so
  // every stream now begins and ends at the CTM the content begins with.
  void ForgetStreamCTMs();

  AllRemovedResourcesMap& all_removed_resources_map() {
    return all_removed_resources_map_;
  }

 protected:
  void LoadTransparencyInfo();
  // EmbedPDF(overlay-epoch): a form drops its parsed objects when its stream
  // has a newer version.
  void ResetParsedContent();
  // EmbedPDF(layer-door): a holder opens its dictionary and resources for
  // writing on their first actual write.
  virtual void EnsureMutableBackingObjectForDict();
  virtual void EnsureMutableBackingObjectForResources();
  virtual void EnsureMutableBackingObjectForPageResources();

  RetainPtr<CPDF_Dictionary> page_resources_;
  RetainPtr<CPDF_Dictionary> resources_;
  // EmbedPDF(overlay-epoch): the dictionary and resources, and the overlay
  // epoch each was read at.
  RetainPtr<CPDF_Dictionary> dict_;
  mutable uint64_t page_resources_epoch_ = 0;
  mutable uint64_t resources_epoch_ = 0;
  mutable uint64_t dict_epoch_ = 0;
  std::map<GraphicsData, ByteString> graphics_map_;
  std::map<FontData, ByteString> fonts_map_;
  // EmbedPDF(faithful-content-rewrite): see FontsByObjnumSearch().
  std::map<uint32_t, ByteString> fonts_by_objnum_;
  std::map<ByteString, ByteString> colorspace_map_;
  CFX_FloatRect bbox_;
  CPDF_Transparency transparency_;

 private:
  bool background_alpha_needed_ = false;
  ParseState parse_state_ = ParseState::kNotParsed;
  // EmbedPDF(page-residency, overlay-epoch): see GetObjectEdits(). Upstream's
  // const `dict_` was here; it is protected now, so a holder can swap it for
  // another version of the dictionary.
  uint64_t object_edits_ = 0;
  UnownedPtr<CPDF_Document> document_;
  std::vector<CFX_FloatRect> mask_bounding_boxes_;
  std::unique_ptr<CPDF_ContentParser> parser_;
  std::deque<std::unique_ptr<CPDF_PageObject>> page_object_list_;
  // EmbedPDF(render-speedup): computed on demand; dropped whenever
  // `page_object_list_` changes.
  mutable std::unique_ptr<CPDF_PageObjectGroups> object_groups_;

  CTMMap all_ctms_;
  // EmbedPDF(faithful-content-rewrite): the CTM the content begins with, in
  // the space of page objects' matrices: the identity for a page, a form's
  // /Matrix for a form drawn as page content (its objects' matrices include
  // it).
  CFX_Matrix ctm_at_beginning_of_content_;

  // The indexes of Content streams that are dirty and need to be regenerated.
  std::set<int32_t> dirty_streams_;
  // EmbedPDF(redaction): see ReplaceResource().
  ResourceReplacements resource_replacements_;

  // All the resources from `resources_` that are unused. Hold on to them here
  // in case they need to be restored.
  AllRemovedResourcesMap all_removed_resources_map_;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_PAGEOBJECTHOLDER_H_
