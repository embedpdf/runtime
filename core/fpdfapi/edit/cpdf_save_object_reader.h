// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFAPI_EDIT_CPDF_SAVE_OBJECT_READER_H_
#define CORE_FPDFAPI_EDIT_CPDF_SAVE_OBJECT_READER_H_

#include <stdint.h>

#include <map>
#include <vector>

#include "core/fpdfapi/parser/cpdf_indirect_object_holder.h"
#include "core/fpdfapi/parser/cpdf_object_stream_cache.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"

class CPDF_Document;
class CPDF_Parser;

// Reads the effective document without adding save-only objects to the live
// document or frozen base. Existing objects are borrowed; other objects and
// their parsing dependencies belong to this reader.
class CPDF_SaveObjectReader final : public CPDF_IndirectObjectHolder {
 public:
  enum class Version {
    kEffective,
    // Loaded bytes for a plain document, frozen base for a layer. Never reads
    // edited objects; a layer's loaded delta twins are compared separately.
    kOriginal,
  };

  explicit CPDF_SaveObjectReader(CPDF_Document* document,
                                 Version version = Version::kEffective);
  ~CPDF_SaveObjectReader() override;

  // Finish using the previous object before calling this again. Indirect
  // dependencies (for example /Length) are retained until the next call.
  RetainPtr<const CPDF_Object> Read(uint32_t object_number);
  bool IsCached(uint32_t object_number) const;

  // Valid until the next call. Live objects always override file-index rows.
  // Successful file reads may populate the parser's bounded reference index.
  pdfium::span<const uint32_t> ReferencesFor(uint32_t object_number);

 protected:
  CPDF_Object* GetOrParseIndirectObjectInternal(
      uint32_t object_number) override;

 private:
  RetainPtr<const CPDF_Object> ReadObject(uint32_t object_number);
  RetainPtr<const CPDF_Object> ReadCachedObject(uint32_t object_number) const;

  UnownedPtr<CPDF_Document> const document_;
  UnownedPtr<CPDF_Parser> const parser_;
  const Version version_;
  CPDF_ObjectStreamCache stream_cache_;
  std::vector<uint32_t> references_;
  std::map<uint32_t, RetainPtr<const CPDF_Object>> dependencies_;
};

#endif  // CORE_FPDFAPI_EDIT_CPDF_SAVE_OBJECT_READER_H_
