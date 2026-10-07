// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_embed_metadata.h"

#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_string.h"

namespace fpdfdoc {

namespace {

RetainPtr<CPDF_Dictionary> GetOrCreateEmbedMetadata(CPDF_Dictionary* owner) {
  RetainPtr<CPDF_Dictionary> metadata =
      owner->GetMutableDictFor(kEmbedMetadataKey);
  if (!metadata) {
    metadata = owner->SetNewFor<CPDF_Dictionary>(kEmbedMetadataKey);
  }
  return metadata;
}

// The value at |key| when it has |type|, else null.
RetainPtr<const CPDF_Object> GetTyped(const CPDF_Dictionary* metadata,
                                      ByteStringView key,
                                      CPDF_Object::Type type) {
  RetainPtr<const CPDF_Object> object =
      metadata ? metadata->GetObjectFor(key) : nullptr;
  return object && object->GetType() == type ? object : nullptr;
}

}  // namespace

RetainPtr<const CPDF_Dictionary> GetEmbedMetadata(
    const CPDF_Dictionary* owner) {
  return owner ? owner->GetDictFor(kEmbedMetadataKey) : nullptr;
}

WideString GetEmbedMetadataString(const CPDF_Dictionary* metadata,
                                  ByteStringView key) {
  return metadata ? metadata->GetUnicodeTextFor(key) : WideString();
}

bool GetEmbedMetadataNumber(const CPDF_Dictionary* metadata,
                            ByteStringView key,
                            float* value) {
  RetainPtr<const CPDF_Object> object =
      GetTyped(metadata, key, CPDF_Object::Type::kNumber);
  if (!object) {
    return false;
  }
  *value = object->GetNumber();
  return true;
}

bool GetEmbedMetadataBoolean(const CPDF_Dictionary* metadata,
                             ByteStringView key,
                             bool* value) {
  RetainPtr<const CPDF_Object> object =
      GetTyped(metadata, key, CPDF_Object::Type::kBoolean);
  if (!object) {
    return false;
  }
  *value = object->GetInteger() != 0;
  return true;
}

bool GetEmbedMetadataRect(const CPDF_Dictionary* metadata,
                          ByteStringView key,
                          CFX_FloatRect* rect) {
  if (!GetTyped(metadata, key, CPDF_Object::Type::kArray)) {
    return false;
  }
  *rect = metadata->GetRectFor(key);
  return true;
}

void SetEmbedMetadataString(CPDF_Dictionary* owner,
                            const ByteString& key,
                            const WideString& value) {
  GetOrCreateEmbedMetadata(owner)->SetNewFor<CPDF_String>(key,
                                                          value.AsStringView());
}

void SetEmbedMetadataNumber(CPDF_Dictionary* owner,
                            const ByteString& key,
                            float value) {
  GetOrCreateEmbedMetadata(owner)->SetNewFor<CPDF_Number>(key, value);
}

void SetEmbedMetadataBoolean(CPDF_Dictionary* owner,
                             const ByteString& key,
                             bool value) {
  GetOrCreateEmbedMetadata(owner)->SetNewFor<CPDF_Boolean>(key, value);
}

void SetEmbedMetadataRect(CPDF_Dictionary* owner,
                          const ByteString& key,
                          const CFX_FloatRect& rect) {
  GetOrCreateEmbedMetadata(owner)->SetRectFor(key, rect);
}

void RemoveEmbedMetadataKey(CPDF_Dictionary* owner, ByteStringView key) {
  RetainPtr<CPDF_Dictionary> metadata =
      owner->GetMutableDictFor(kEmbedMetadataKey);
  if (!metadata) {
    return;
  }
  metadata->RemoveFor(key);
  if (metadata->size() == 0) {
    owner->RemoveFor(kEmbedMetadataKey);
  }
}

void RemoveEmbedMetadata(CPDF_Dictionary* owner) {
  owner->RemoveFor(kEmbedMetadataKey);
}

}  // namespace fpdfdoc
