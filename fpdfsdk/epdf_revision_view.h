// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef FPDFSDK_EPDF_REVISION_VIEW_H_
#define FPDFSDK_EPDF_REVISION_VIEW_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <vector>

#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fxcrt/fx_types.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"

class CPDF_Object;
class CPDF_ParseOnlyHolder;
class CPDF_Parser;
class IFX_SeekableReadStream;

namespace epdf {

// One revision of a file: where its bytes end (just past the closing EOL of
// its %%EOF) and where the cross-reference section that closes it starts.
struct RevisionInfo {
  FX_FILESIZE end = 0;
  FX_FILESIZE xref_offset = 0;
};

// The immutable bytes a document was loaded from, with a parser over exactly
// those bytes. Revision analysis - revision boundaries, signature coverage,
// digests, the cross-revision object diff - reads these bytes and nothing
// else: an edit that has not been saved is not part of any revision.
//
// An ordinary document is its own view: its parser holds the loaded file.
// A layer document is not - its parser is the BASE parser, and the bytes it
// was loaded from are the base file followed by the delta it ingested. The
// view parses that concatenation privately, so a layer reports the same
// revisions, coverage, digests and object values as the same bytes opened as
// a plain document, and two layers over one base with different deltas no
// longer compare as identical.
//
// A view is created once per document and cached on it (`For`): the loaded
// bytes never change, so neither does the view, and the private parse of a
// layer's bytes is paid once rather than per API call. The cached revision
// list rides along for the same reason.
class RevisionView final : public CPDF_Document::Attachment {
 public:
  // The document's cached view, created on first use and owned by the
  // document. Null when the document has no parser or its loaded bytes do
  // not parse (nothing is cached then; the next call tries again).
  static RevisionView* For(CPDF_Document* document);
  ~RevisionView() override;

  CPDF_Parser* parser() const { return parser_; }
  RetainPtr<IFX_SeekableReadStream> file() const;

  // Parses |objnum| from the loaded bytes. Never consults a document's
  // object cache, so a value changed in memory is not mistaken for the
  // value a revision holds.
  RetainPtr<CPDF_Object> ParseObject(uint32_t objnum) const;

  // The revision list, computed at most once per view by the caller that
  // owns the computation (see RevisionsOf in epdf_signature.cpp).
  bool has_revisions() const { return revisions_computed_; }
  const std::optional<std::vector<RevisionInfo>>& revisions() const {
    return revisions_;
  }
  void set_revisions(std::optional<std::vector<RevisionInfo>> revisions) {
    revisions_ = std::move(revisions);
    revisions_computed_ = true;
  }

 private:
  RevisionView();
  static std::unique_ptr<RevisionView> Create(CPDF_Document* document);

  // Layers only: a private parser over base + delta. Declared before
  // |parser_| and destroyed after it; the parser must not outlive the holder.
  std::unique_ptr<CPDF_ParseOnlyHolder> holder_;
  std::unique_ptr<CPDF_Parser> owned_parser_;
  UnownedPtr<CPDF_Parser> parser_;

  bool revisions_computed_ = false;
  std::optional<std::vector<RevisionInfo>> revisions_;
};

}  // namespace epdf

#endif  // FPDFSDK_EPDF_REVISION_VIEW_H_
