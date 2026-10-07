// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef CORE_FPDFDOC_CPDF_EMBED_METADATA_H_
#define CORE_FPDFDOC_CPDF_EMBED_METADATA_H_

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/widestring.h"

class CPDF_Dictionary;

namespace fpdfdoc {

// /EMBD_Metadata: the dictionary of EmbedPDF's own keys that an annotation or
// a form field carries, and that other PDF readers ignore. Annotations and
// fields read and write it through these, so both behave the same.

inline constexpr char kEmbedMetadataKey[] = "EMBD_Metadata";

// The key that holds an app's own data, as JSON text.
inline constexpr char kEmbedMetadataCustomJSONKey[] = "CustomJSON";

// The /EMBD_Metadata dictionary of |owner|, or null.
RetainPtr<const CPDF_Dictionary> GetEmbedMetadata(const CPDF_Dictionary* owner);

// Reads from a /EMBD_Metadata dictionary; a null one reads as empty. A typed
// read succeeds only when the key holds that type.
WideString GetEmbedMetadataString(const CPDF_Dictionary* metadata,
                                  ByteStringView key);
bool GetEmbedMetadataNumber(const CPDF_Dictionary* metadata,
                            ByteStringView key,
                            float* value);
bool GetEmbedMetadataBoolean(const CPDF_Dictionary* metadata,
                             ByteStringView key,
                             bool* value);
bool GetEmbedMetadataRect(const CPDF_Dictionary* metadata,
                          ByteStringView key,
                          CFX_FloatRect* rect);

// Writes on |owner|. The first write creates /EMBD_Metadata, and removing its
// last key removes it.
void SetEmbedMetadataString(CPDF_Dictionary* owner,
                            const ByteString& key,
                            const WideString& value);
void SetEmbedMetadataNumber(CPDF_Dictionary* owner,
                            const ByteString& key,
                            float value);
void SetEmbedMetadataBoolean(CPDF_Dictionary* owner,
                             const ByteString& key,
                             bool value);
void SetEmbedMetadataRect(CPDF_Dictionary* owner,
                          const ByteString& key,
                          const CFX_FloatRect& rect);
void RemoveEmbedMetadataKey(CPDF_Dictionary* owner, ByteStringView key);
void RemoveEmbedMetadata(CPDF_Dictionary* owner);

}  // namespace fpdfdoc

#endif  // CORE_FPDFDOC_CPDF_EMBED_METADATA_H_
