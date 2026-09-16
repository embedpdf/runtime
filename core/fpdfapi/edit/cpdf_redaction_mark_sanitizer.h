// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_EDIT_CPDF_REDACTION_MARK_SANITIZER_H_
#define CORE_FPDFAPI_EDIT_CPDF_REDACTION_MARK_SANITIZER_H_

#include <map>
#include <set>
#include <vector>

#include "core/fpdfapi/page/cpdf_contentmarkitem.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"

class CPDF_PageObject;
class CPDF_PageObjectHolder;

// Replacement text describes an entire marked-content span, not individual
// glyphs. If part of a span is redacted, discard its stale
// replacement/alternate text on every surviving object in that span, and on its
// structure ancestors. Glyphs, MCIDs and unrelated structure siblings are
// preserved.
class CPDF_RedactionMarkSanitizer {
 public:
  using SanitizedProperties = std::vector<RetainPtr<const CPDF_Dictionary>>;

  CPDF_RedactionMarkSanitizer(CPDF_PageObjectHolder* holder,
                              SanitizedProperties* sanitized_properties);
  ~CPDF_RedactionMarkSanitizer();

  // Call before removing an affected object. Keeps its marks alive until Apply.
  void Record(CPDF_PageObject* object);

  // A Form holder must already have a private backing stream. Never writes to
  // a shared property dictionary: affected marks become private direct marks,
  // and unused names are detached from a private resource dictionary.
  void Apply();

 private:
  void RemoveUnusedProperties(
      std::map<ByteString, RetainPtr<const CPDF_Dictionary>> candidates);
  void RemoveUnusedPageTreeProperties();
  void SanitizeStructure(const std::set<int>& mcids);

  UnownedPtr<CPDF_PageObjectHolder> const holder_;
  // Captured before a Form backing stream/resources are cloned for writing.
  RetainPtr<const CPDF_Dictionary> const original_resources_;
  UnownedPtr<SanitizedProperties> const sanitized_properties_;
  std::map<const CPDF_ContentMarkItem*, RetainPtr<CPDF_ContentMarkItem>> marks_;
};

#endif  // CORE_FPDFAPI_EDIT_CPDF_REDACTION_MARK_SANITIZER_H_
