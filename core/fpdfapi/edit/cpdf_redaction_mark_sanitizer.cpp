// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/edit/cpdf_redaction_mark_sanitizer.h"

#include <utility>

#include "core/fpdfapi/page/cpdf_contentmarks.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"

namespace {

constexpr const char* kReplacementTextKeys[] = {"ActualText", "Alt", "E"};

bool HasReplacementText(const CPDF_Dictionary* dict) {
  for (const char* key : kReplacementTextKeys) {
    if (dict->KeyExist(key)) {
      return true;
    }
  }
  return false;
}

void RemoveReplacementText(CPDF_Dictionary* dict) {
  for (const char* key : kReplacementTextKeys) {
    dict->RemoveFor(key);
  }
}

bool SameDictionary(const CPDF_Dictionary* a, const CPDF_Dictionary* b) {
  return a && b &&
         (a == b || (a->GetObjNum() != 0 && a->GetObjNum() == b->GetObjNum()));
}

bool PropertyIsUsed(const CPDF_PageObjectHolder* holder,
                    const CPDF_Dictionary* property) {
  std::vector<const CPDF_PageObjectHolder*> pending{holder};
  std::set<const CPDF_PageObjectHolder*> visited;
  while (!pending.empty()) {
    const auto* current = pending.back();
    pending.pop_back();
    if (!visited.insert(current).second) {
      continue;
    }
    for (const auto& object : *current) {
      if (!object->IsActive()) {
        continue;
      }
      const CPDF_ContentMarks* marks = object->GetContentMarks();
      for (size_t i = 0; i < marks->CountItems(); ++i) {
        const auto* item = marks->GetItem(i);
        if (item->GetParamType() == CPDF_ContentMarkItem::kPropertiesDict &&
            SameDictionary(item->GetParam().Get(), property)) {
          return true;
        }
      }
      if (const CPDF_FormObject* form = object->AsForm()) {
        if (form->form()) {
          pending.push_back(form->form());
        }
      }
    }
  }
  return false;
}

// Mutable number-tree lookup. Following GetMutable* accessors ensures layer
// objects are promoted instead of mutating the immutable base graph. Bound
// recursion and detect cycles in malformed trees.
RetainPtr<CPDF_Array> FindParents(CPDF_Dictionary* node,
                                  int key,
                                  std::set<const CPDF_Dictionary*>* visited,
                                  int depth = 0) {
  if (depth > 64 || !visited->insert(node).second) {
    return nullptr;
  }
  if (auto limits = node->GetArrayFor("Limits")) {
    if (key < limits->GetIntegerAt(0) || key > limits->GetIntegerAt(1)) {
      return nullptr;
    }
  }
  if (auto nums = node->GetMutableArrayFor("Nums")) {
    for (size_t i = 0; i + 1 < nums->size(); i += 2) {
      if (nums->GetIntegerAt(i) == key) {
        return ToArray(nums->GetMutableDirectObjectAt(i + 1));
      }
    }
  }
  if (auto kids = node->GetMutableArrayFor("Kids")) {
    for (size_t i = 0; i < kids->size(); ++i) {
      auto kid = kids->GetMutableDictAt(i);
      if (kid) {
        auto found = FindParents(kid.Get(), key, visited, depth + 1);
        if (found) {
          return found;
        }
      }
    }
  }
  return nullptr;
}

}  // namespace

CPDF_RedactionMarkSanitizer::CPDF_RedactionMarkSanitizer(
    CPDF_PageObjectHolder* holder,
    SanitizedProperties* sanitized_properties)
    : holder_(holder),
      original_resources_(holder->GetResources()),
      sanitized_properties_(sanitized_properties) {}

CPDF_RedactionMarkSanitizer::~CPDF_RedactionMarkSanitizer() = default;

void CPDF_RedactionMarkSanitizer::Record(CPDF_PageObject* object) {
  CPDF_ContentMarks* content_marks = object->GetContentMarks();
  for (size_t i = 0; i < content_marks->CountItems(); ++i) {
    auto* item = content_marks->GetItem(i);
    marks_.try_emplace(item, pdfium::WrapRetain(item));
  }
}

void CPDF_RedactionMarkSanitizer::Apply() {
  CPDF_DocumentViewScope document_view(holder_->GetDocument());
  std::map<ByteString, RetainPtr<const CPDF_Dictionary>> properties;
  std::set<int> mcids;
  std::set<const CPDF_ContentMarkItem*> changed_marks;
  for (auto& [identity, item] : marks_) {
    auto params = std::as_const(*item).GetParam();
    if (!params) {
      continue;
    }
    const int mcid = params->GetIntegerFor("MCID", -1);
    if (mcid >= 0) {
      mcids.insert(mcid);
    }
    if (!HasReplacementText(params.Get())) {
      continue;
    }
    if (item->GetParamType() == CPDF_ContentMarkItem::kPropertiesDict) {
      properties[item->GetPropertyName()] = params;
      sanitized_properties_->push_back(params);
    }
    auto sanitized =
        ToDictionary(params->CloneForHolder(holder_->GetDocument()));
    RemoveReplacementText(sanitized.Get());
    item->SetDirectDict(std::move(sanitized));
    changed_marks.insert(identity);
  }

  // Several objects can share one BDC/EMC span, including objects in different
  // content streams. All survivors must serialize the updated mark.
  for (auto& object : *holder_) {
    const CPDF_ContentMarks* content_marks = object->GetContentMarks();
    for (size_t i = 0; i < content_marks->CountItems(); ++i) {
      if (changed_marks.contains(content_marks->GetItem(i))) {
        object->SetDirty(true);
        break;
      }
    }
  }
  RemoveUnusedProperties(std::move(properties));
  RemoveUnusedPageTreeProperties();
  SanitizeStructure(mcids);
}

void CPDF_RedactionMarkSanitizer::RemoveUnusedProperties(
    std::map<ByteString, RetainPtr<const CPDF_Dictionary>> candidates) {
  // A child Form may have inherited a property from this holder. Sanitizing
  // its private copy must also detach the now-unused original here; otherwise
  // a full rewrite still writes the secret through the parent's resources.
  auto original_properties = original_resources_
                                 ? original_resources_->GetDictFor("Properties")
                                 : nullptr;
  if (original_properties) {
    for (const auto& name : original_properties->GetKeys()) {
      auto property = original_properties->GetDictFor(name.AsStringView());
      for (const auto& sanitized : *sanitized_properties_) {
        if (SameDictionary(property.Get(), sanitized.Get())) {
          candidates[name] = property;
          break;
        }
      }
    }
  }
  std::set<ByteString> unused;
  for (const auto& [name, property] : candidates) {
    if (!PropertyIsUsed(holder_.get(), property.Get())) {
      unused.insert(name);
    }
  }
  auto resources = holder_->GetResources();
  auto properties = resources ? resources->GetDictFor("Properties") : nullptr;
  if (unused.empty() || !properties) {
    return;
  }
  CPDF_Document* doc = holder_->GetDocument();
  auto private_resources = ToDictionary(resources->CloneForHolder(doc));
  auto private_properties = ToDictionary(properties->CloneForHolder(doc));
  for (const auto& name : unused) {
    private_properties->RemoveFor(name.AsStringView());
  }
  private_resources->SetFor("Properties", std::move(private_properties));
  holder_->SetResources(private_resources);
  holder_->GetMutableDict()->SetFor("Resources", std::move(private_resources));
}

void CPDF_RedactionMarkSanitizer::RemoveUnusedPageTreeProperties() {
  if (!holder_->IsPage() || sanitized_properties_->empty()) {
    return;
  }

  // Installing local page resources does not detach inherited resources from
  // the file: /Parent keeps every /Pages ancestor reachable. Include shadowed
  // ancestors too, but only consider properties sanitized by this operation.
  std::vector<RetainPtr<const CPDF_Dictionary>> ancestor_resources;
  std::map<const CPDF_Dictionary*, RetainPtr<const CPDF_Dictionary>> unused;
  std::set<const CPDF_Dictionary*> visited;
  auto ancestor = holder_->GetDict()->GetDictFor("Parent");
  while (ancestor && visited.insert(ancestor.Get()).second) {
    auto resources = ancestor->GetDictFor("Resources");
    ancestor_resources.push_back(resources);
    auto properties = resources ? resources->GetDictFor("Properties") : nullptr;
    if (properties) {
      for (const auto& name : properties->GetKeys()) {
        auto property = properties->GetDictFor(name.AsStringView());
        for (const auto& sanitized : *sanitized_properties_) {
          if (SameDictionary(property.Get(), sanitized.Get())) {
            unused.emplace(property.Get(), property);
            break;
          }
        }
      }
    }
    ancestor = ancestor->GetDictFor("Parent");
  }

  auto preserve_used = [&unused](const CPDF_PageObjectHolder* page) {
    std::erase_if(unused, [page](const auto& entry) {
      return PropertyIsUsed(page, entry.second.Get());
    });
  };
  // Use the live, edited page, not a new parse of its old content stream.
  preserve_used(holder_.get());
  if (unused.empty()) {
    return;
  }

  CPDF_Document* doc = holder_->GetDocument();
  for (int i = 0; i < doc->GetPageCount(); ++i) {
    auto page_dict = doc->GetPageDictionary(i);
    if (!page_dict) {
      // Do not remove a shared definition if a sibling cannot be inspected.
      return;
    }
    if (SameDictionary(page_dict.Get(), holder_->GetDict().Get())) {
      continue;
    }
    // Parsing is read-only. In a layer the view scope resolves references
    // through that layer while the shared base remains frozen.
    auto page = pdfium::MakeRetain<CPDF_Page>(
        doc, pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(page_dict.Get())));
    page->ParseContent();
    preserve_used(page.Get());
    if (unused.empty()) {
      return;
    }
  }

  // Promote the parent chain through mutable accessors for layer isolation.
  // Clone resource dictionaries instead of mutating aliases held elsewhere.
  auto parent = holder_->GetMutableDict()->GetMutableDictFor("Parent");
  for (const auto& resources : ancestor_resources) {
    if (!parent) {
      break;
    }
    auto properties = resources ? resources->GetDictFor("Properties") : nullptr;
    std::set<ByteString> names;
    if (properties) {
      for (const auto& name : properties->GetKeys()) {
        if (unused.contains(
                properties->GetDictFor(name.AsStringView()).Get())) {
          names.insert(name);
        }
      }
    }
    if (!names.empty()) {
      auto private_resources = ToDictionary(resources->CloneForHolder(doc));
      auto private_properties = ToDictionary(properties->CloneForHolder(doc));
      for (const auto& name : names) {
        private_properties->RemoveFor(name.AsStringView());
      }
      private_resources->SetFor("Properties", std::move(private_properties));
      parent->SetFor("Resources", std::move(private_resources));
    }
    parent = parent->GetMutableDictFor("Parent");
  }
}

void CPDF_RedactionMarkSanitizer::SanitizeStructure(
    const std::set<int>& mcids) {
  const int key = holder_->GetDict()->GetIntegerFor("StructParents", -1);
  if (mcids.empty() || key < 0) {
    return;
  }
  auto root = holder_->GetDocument()->GetMutableRoot();
  auto tree = root ? root->GetMutableDictFor("StructTreeRoot") : nullptr;
  auto parent_tree = tree ? tree->GetMutableDictFor("ParentTree") : nullptr;
  if (!parent_tree) {
    return;
  }
  std::set<const CPDF_Dictionary*> visited;
  auto parents = FindParents(parent_tree.Get(), key, &visited);
  if (!parents) {
    return;
  }
  visited.clear();
  for (int mcid : mcids) {
    auto element = parents->GetMutableDictAt(static_cast<size_t>(mcid));
    while (element && element->GetNameFor("Type") != "StructTreeRoot" &&
           visited.insert(element.Get()).second) {
      // A containing paragraph may also carry an aggregate ActualText. There
      // is no reliable mapping from a partial visual edit to that string.
      RemoveReplacementText(element.Get());
      element = element->GetMutableDictFor("P");
    }
  }
}
