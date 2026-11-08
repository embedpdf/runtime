// Copyright 2023 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_FPDFAPI_PARSER_OBJECT_TREE_TRAVERSAL_UTIL_H_
#define CORE_FPDFAPI_PARSER_OBJECT_TREE_TRAVERSAL_UTIL_H_

#include <stdint.h>

#include <set>

class CPDF_Document;

// EmbedPDF(layer-save): collect objects through a layer's view.
enum class ObjectTreeReferenceResolveMode {
  // Resolve references through the holder stored on each CPDF_Reference. This
  // preserves the historical traversal behavior for ordinary documents.
  kReferenceHolder,

  // Resolve all references through the document being traversed. This is needed
  // when the document can override referenced objects, such as a layer document
  // whose overlay should take precedence over the frozen base graph.
  kEffectiveDocument,
};

// Traverses `document` starting with its trailer, if it has one, and also from
// the catalog if not reached from the trailer. The traversal avoids cycles.
//
// In `kReferenceHolder` mode, references are followed through their
// CPDF_IndirectObjectHolder. In `kEffectiveDocument` mode, references are
// resolved through `document` so any overlay it provides is honored.
//
// Returns all the PDF objects (not CPDF_Objects) the traversal reached as a set
// of object numbers.
std::set<uint32_t> GetObjectsWithReferences(
    const CPDF_Document* document,
    ObjectTreeReferenceResolveMode resolve_mode =
        ObjectTreeReferenceResolveMode::kReferenceHolder);

// Same as GetObjectsWithReferences(), but only returns the objects with
// multiple references. References that would create a cycle are ignored.
//
// In this example, where (A) is the root node:
//
//     A -> B
//     A -> C
//     B -> D
//     C -> D
//
// GetObjectsWithMultipleReferences() returns {D}, since both (B) and (C)
// references to (D), and there are no cycles.
//
// In this example, where (A) is the root node:
//
//     A -> B
//     B -> C
//     C -> B
//
// GetObjectsWithMultipleReferences() returns {}, even though both (A) and (C)
// references (B). Since (B) -> (C) -> (B) creates a cycle, the (C) -> (B)
// reference does not count.
// EmbedPDF(layer-save): collect objects through a layer's view.
std::set<uint32_t> GetObjectsWithMultipleReferences(
    const CPDF_Document* document,
    ObjectTreeReferenceResolveMode resolve_mode =
        ObjectTreeReferenceResolveMode::kReferenceHolder);

#endif  // CORE_FPDFAPI_PARSER_OBJECT_TREE_TRAVERSAL_UTIL_H_
