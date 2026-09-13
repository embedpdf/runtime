// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_PARSER_CPDF_BASE_DOCUMENT_H_
#define CORE_FPDFAPI_PARSER_CPDF_BASE_DOCUMENT_H_

#include <stdint.h>

#include <array>
#include <optional>
#include <vector>

#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/fx_types.h"
#include "core/fxcrt/retain_ptr.h"

class IFX_SeekableReadStream;
class CPDF_LayerDocument;

class CPDF_BaseDocument final : public CPDF_Document, public Retainable {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  // Load and freeze. Only what loading itself touches is parsed; the rest
  // of the file is parsed on first touch (see GetFrozenObjectForLayer).
  CPDF_Parser::Error LoadBaseDoc(RetainPtr<IFX_SeekableReadStream> file_access,
                                 const ByteString& password);
  // Optional warm-up: parse every object reachable from the catalog now,
  // so later reads never pay for a parse. Not part of loading.
  bool EagerlyParseAllReachable();

  // The frozen, shared copy of |objnum| for layers to read (and clone from
  // when they write). Parses the object from the base's bytes on a cache
  // miss, freezes it, and keeps it for every layer; null only when the file
  // has no such object.
  RetainPtr<const CPDF_Object> GetFrozenObjectForLayer(uint32_t objnum) const;
  FX_FILESIZE GetRawBaseSize() const { return raw_base_size_; }
  FX_FILESIZE GetLayerAppendBaseOffset() const override {
    return layer_append_base_offset_;
  }
  const CPDF_BaseDocument* GetBaseDocumentForViewScope() const override {
    return this;
  }
  // SHA-256 of the raw base bytes: the identity layer artifacts bind to and
  // the version a completed signature reports. Computed on first use — a
  // full pass over the file — unless the embedder supplied it through
  // SetKnownRawBaseSha256(). Reads all-zero when the file cannot be read.
  const std::array<uint8_t, 32>& GetRawBaseSha256() const;
  // A host that already hashed the exact bytes it handed us (a storage layer
  // verifying a download) need not pay for a second pass. Only an identity
  // claim: a wrong value makes this host's own artifacts fail to open on the
  // real bytes and nothing else.
  void SetKnownRawBaseSha256(const std::array<uint8_t, 32>& sha256) {
    raw_base_sha256_ = sha256;
  }

#if DCHECK_IS_ON()
  void RegisterLiveLayer(const CPDF_LayerDocument* layer);
  void UnregisterLiveLayer(const CPDF_LayerDocument* layer);
#endif

 private:
  CPDF_BaseDocument();
  ~CPDF_BaseDocument() override;

  bool CacheBaseIdentity();

  // CPDF_IndirectObjectHolder:
  // This override is intentionally limited to the non-const reference
  // resolution path. Const base lookups must remain frozen so layer promotion
  // always clones from the shared base rather than resolving back into the
  // layer overlay.
  CPDF_Object* GetOrParseIndirectObjectInternal(uint32_t objnum) override;

#if DCHECK_IS_ON()
  bool IsObjectPromotedInAnyLiveLayer(uint32_t objnum) const;
  std::vector<const CPDF_LayerDocument*> live_layers_;
#endif

  FX_FILESIZE raw_base_size_ = 0;
  // PDFium parser offsets are logical PDF offsets after the syntax parser's
  // header offset has been subtracted. Layer append-only xref offsets must use
  // the same coordinate system, not the raw stream byte size.
  FX_FILESIZE layer_append_base_offset_ = 0;
  mutable std::optional<std::array<uint8_t, 32>> raw_base_sha256_;
};

#endif  // CORE_FPDFAPI_PARSER_CPDF_BASE_DOCUMENT_H_
