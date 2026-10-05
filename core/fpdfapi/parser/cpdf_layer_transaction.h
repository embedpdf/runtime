// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PARSER_CPDF_LAYER_TRANSACTION_H_
#define CORE_FPDFAPI_PARSER_CPDF_LAYER_TRANSACTION_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <optional>
#include <set>
#include <vector>

#include "core/fxcrt/retain_ptr.h"

class CPDF_Dictionary;
class CPDF_Object;

// One promoted page's births: for each inline annotation the base has on the
// page, keyed by its position in the base's /Annots (its birth index), the
// object it became when the page was promoted. See
// CPDF_LayerDocument::PromotePageAnnots().
using CPDF_PageBirths = std::map<uint32_t, uint32_t>;

// What one transaction cost: how many objects it copied up, how many stream
// bytes those copies duplicated, and how many objects it created. Kept after
// the transaction ends, for tests and diagnostics.
struct CPDF_LayerTransactionStats {
  size_t objects_copied = 0;
  size_t stream_bytes_copied = 0;
  size_t objects_added = 0;
};

// One open transaction on a layer document (see CPDF_LayerDocument): an
// overlay above the layer's committed objects. Reads look here first; every
// write lands here; commit moves it down, abort drops it.
struct CPDF_LayerTransaction {
  // The objects this transaction wrote: a copied-up version, or a new object.
  std::map<uint32_t, RetainPtr<CPDF_Object>> written;

  // Committed layer versions this transaction deleted. Hiding the layer's
  // version is all a delete can do: an object that exists in the base keeps
  // resolving to its base version, exactly as outside a transaction.
  std::set<uint32_t> hidden;

  // Every number this transaction wrote, created or hid at any point, even
  // when a later delete removed it again: what derived caches must forget.
  std::set<uint32_t> touched;

  // Versions this transaction made and deleted again: kept until it ends, so
  // derived caches can forget them without a scan.
  std::vector<RetainPtr<CPDF_Object>> dropped;

  // Object numbers above this were handed out by this transaction. They are
  // never given back: a number names one object for the whole session.
  uint32_t object_mark = 0;

  // Stamped on everything this transaction stores (see CPDF_WriteGeneration):
  // while it is open, only these objects (and detached ones) are writable.
  uint32_t generation = 0;

  // The committed page list, saved the first time this transaction changes
  // it. Annotation and form writes never touch it, so never pay for it.
  std::optional<std::vector<uint32_t>> page_list_before;

  // Births of the pages this transaction promoted, keyed by page object
  // number. Commit adds them to the layer's; abort drops them with the
  // objects they name.
  std::map<uint32_t, CPDF_PageBirths> births;

  // The document's cached catalog and Info at begin, put back on abort. A
  // layer-created Info is reachable only through that cache, and re-resolving
  // an emptied cache goes through the write door.
  RetainPtr<CPDF_Dictionary> root_before;
  RetainPtr<CPDF_Dictionary> info_before;

  CPDF_LayerTransactionStats stats;
};

#endif  // CORE_FPDFAPI_PARSER_CPDF_LAYER_TRANSACTION_H_
