// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "fpdfsdk/epdf_revision_view.h"

#include <utility>

#include "core/fpdfapi/parser/cpdf_concat_read_stream.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_parse_only_holder.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fxcrt/fx_stream.h"

namespace epdf {

RevisionView::RevisionView() = default;

RevisionView::~RevisionView() = default;

// static
RevisionView* RevisionView::For(CPDF_Document* document) {
  if (!document) {
    return nullptr;
  }
  if (RevisionView* cached =
          static_cast<RevisionView*>(document->epdf_attachment())) {
    return cached;
  }
  std::unique_ptr<RevisionView> view = Create(document);
  if (!view) {
    return nullptr;
  }
  RevisionView* raw = view.get();
  document->SetEpdfAttachment(std::move(view));
  return raw;
}

// static
std::unique_ptr<RevisionView> RevisionView::Create(CPDF_Document* document) {
  CPDF_Parser* own_parser = document ? document->GetParser() : nullptr;
  if (!own_parser) {
    return nullptr;
  }
  std::unique_ptr<RevisionView> view(new RevisionView());
  const CPDF_LayerDocument* layer = CPDF_LayerDocument::FromDocument(document);
  if (!layer) {
    view->parser_ = own_parser;
    return view;
  }

  // A layer's own parser is the base parser. Its loaded bytes are the base
  // file plus the ingested delta; parse them privately, with the base
  // password, so every position and object value refers to those bytes.
  RetainPtr<IFX_SeekableReadStream> bytes = own_parser->GetFileAccess();
  if (!bytes) {
    return nullptr;
  }
  if (RetainPtr<IFX_SeekableReadStream> delta = layer->GetLoadedDeltaStream()) {
    bytes = pdfium::MakeRetain<CPDF_ConcatReadStream>(std::move(bytes),
                                                      std::move(delta));
  }
  view->holder_ = std::make_unique<CPDF_ParseOnlyHolder>();
  view->owned_parser_ = std::make_unique<CPDF_Parser>(view->holder_.get());
  view->holder_->SetParser(view->owned_parser_.get());
  if (view->owned_parser_->StartParse(std::move(bytes),
                                      own_parser->GetPassword()) !=
      CPDF_Parser::SUCCESS) {
    return nullptr;
  }
  view->parser_ = view->owned_parser_.get();
  return view;
}

RetainPtr<IFX_SeekableReadStream> RevisionView::file() const {
  return parser_ ? parser_->GetFileAccess() : nullptr;
}

RetainPtr<CPDF_Object> RevisionView::ParseObject(uint32_t objnum) const {
  return parser_ ? parser_->ParseIndirectObject(objnum) : nullptr;
}

}  // namespace epdf
