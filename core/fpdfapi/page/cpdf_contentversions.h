// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PAGE_CPDF_CONTENTVERSIONS_H_
#define CORE_FPDFAPI_PAGE_CPDF_CONTENTVERSIONS_H_

#include <set>
#include <vector>

#include "core/fxcrt/retain_ptr.h"

class CPDF_Dictionary;
class CPDF_Document;
class CPDF_Stream;

// The versions of the streams a page's parsed objects were read from, or last
// written to: the page's /Contents streams, in order, and the stream of every
// form placed on the page, at any depth.
//
// A layer makes a new version of an object for every write inside a
// transaction, and an abort brings back the one before. So these versions,
// compared with the ones the document resolves now, tell whether the page's
// objects still match its content. A write outside a transaction edits an
// object in place, which no version shows: whoever makes one must close the
// pages it changes.
class CPDF_ContentVersions {
 public:
  CPDF_ContentVersions();
  ~CPDF_ContentVersions();

  // The /Contents streams of `page_dict`, in the order the content parser
  // reads them: one for a stream, one per entry for an array (null for an
  // entry that is not a stream), none when there is no content.
  static std::vector<RetainPtr<const CPDF_Stream>> ContentStreamsOf(
      const CPDF_Dictionary* page_dict);

  void Clear();

  // `stream` may be null: an entry of /Contents that is not a stream.
  void AddContentStream(RetainPtr<const CPDF_Stream> stream);
  void AddFormStream(RetainPtr<const CPDF_Stream> stream);

  // Whether `page_dict`, the page's dictionary as the document resolves it
  // now, has the recorded /Contents streams, and `doc` resolves every
  // recorded form stream to the recorded version.
  bool Match(CPDF_Document* doc, const CPDF_Dictionary* page_dict) const;

 private:
  std::vector<RetainPtr<const CPDF_Stream>> contents_;
  std::set<RetainPtr<const CPDF_Stream>> forms_;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_CONTENTVERSIONS_H_
