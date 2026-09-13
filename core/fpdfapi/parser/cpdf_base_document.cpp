// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/parser/cpdf_base_document.h"

#include <algorithm>
#include <array>
#include <memory>
#include <queue>
#include <set>
#include <utility>

#include "core/fdrm/fx_crypt_sha.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/render/cpdf_docrenderdata.h"
#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/span.h"

namespace {

constexpr size_t kSha256DigestSize = 32;

void PushIfNew(RetainPtr<const CPDF_Object> object,
               std::set<const CPDF_Object*>* visited,
               std::queue<RetainPtr<const CPDF_Object>>* worklist) {
  if (!object || !visited->insert(object.Get()).second) {
    return;
  }
  worklist->push(std::move(object));
}

bool ComputeStreamSha256(IFX_SeekableReadStream* stream,
                         FX_FILESIZE size,
                         std::array<uint8_t, kSha256DigestSize>* digest) {
  if (!stream || size < 0 || !digest) {
    return false;
  }

  CRYPT_sha2_context context;
  CRYPT_SHA256Start(&context);
  std::array<uint8_t, 8192> buffer = {};
  FX_FILESIZE offset = 0;
  while (offset < size) {
    const size_t read_size = static_cast<size_t>(
        std::min<FX_FILESIZE>(buffer.size(), size - offset));
    if (!stream->ReadBlockAtOffset(pdfium::span(buffer).first(read_size),
                                   offset)) {
      return false;
    }
    CRYPT_SHA256Update(&context, pdfium::span(buffer).first(read_size));
    offset += read_size;
  }

  CRYPT_SHA256Finish(&context, *digest);
  return true;
}

}  // namespace

CPDF_BaseDocument::CPDF_BaseDocument()
    : CPDF_Document(std::make_unique<CPDF_DocRenderData>(),
                    std::make_unique<CPDF_DocPageData>()) {}

CPDF_BaseDocument::~CPDF_BaseDocument() = default;

CPDF_Parser::Error CPDF_BaseDocument::LoadBaseDoc(
    RetainPtr<IFX_SeekableReadStream> file_access,
    const ByteString& password) {
  CPDF_Parser::Error error = LoadDoc(std::move(file_access), password);
  if (error != CPDF_Parser::SUCCESS) {
    return error;
  }
  if (!CacheBaseIdentity()) {
    return CPDF_Parser::FORMAT_ERROR;
  }
  if (!GetRoot()) {
    return CPDF_Parser::FORMAT_ERROR;
  }
  // Freeze now: whatever loading touched is immutable, and every object
  // parsed from here on (see GetFrozenObjectForLayer) is frozen on arrival.
  // Nothing is walked eagerly; a layer that touches an object nobody has
  // loaded yet triggers its parse, once, into this shared cache.
  Freeze();
  return CPDF_Parser::SUCCESS;
}

bool CPDF_BaseDocument::CacheBaseIdentity() {
  CPDF_Parser* parser = GetParser();
  RetainPtr<IFX_SeekableReadStream> stream =
      parser ? parser->GetFileAccess() : nullptr;
  if (!parser || !stream) {
    return false;
  }

  raw_base_size_ = stream->GetSize();
  if (raw_base_size_ < 0) {
    return false;
  }
  layer_append_base_offset_ = parser->GetDocumentSize();
  // The hash is deliberately NOT computed here: it is a full pass over the
  // file that most opens never need (see GetRawBaseSha256).
  return true;
}

const std::array<uint8_t, 32>& CPDF_BaseDocument::GetRawBaseSha256() const {
  static const std::array<uint8_t, 32> kUnknown = {};
  if (raw_base_sha256_.has_value()) {
    return *raw_base_sha256_;
  }
  CPDF_Parser* parser = GetParser();
  RetainPtr<IFX_SeekableReadStream> stream =
      parser ? parser->GetFileAccess() : nullptr;
  std::array<uint8_t, 32> digest = {};
  if (!stream || !ComputeStreamSha256(stream.Get(), raw_base_size_, &digest)) {
    return kUnknown;
  }
  raw_base_sha256_ = digest;
  return *raw_base_sha256_;
}

bool CPDF_BaseDocument::EagerlyParseAllReachable() {
  if (!GetParser() || !GetRoot()) {
    return false;
  }

  std::set<const CPDF_Object*> visited;
  std::queue<RetainPtr<const CPDF_Object>> worklist;
  PushIfNew(pdfium::WrapRetain(GetParser()->GetTrailer()), &visited, &worklist);
  PushIfNew(pdfium::WrapRetain(GetRoot()), &visited, &worklist);
  PushIfNew(GetInfo(), &visited, &worklist);
  PushIfNew(GetParser()->GetEncryptDict(), &visited, &worklist);

  while (!worklist.empty()) {
    RetainPtr<const CPDF_Object> object = worklist.front();
    worklist.pop();

    switch (object->GetType()) {
      case CPDF_Object::kReference: {
        const uint32_t ref_objnum = object->AsReference()->GetRefObjNum();
        PushIfNew(GetOrParseIndirectObject(ref_objnum), &visited, &worklist);
        break;
      }
      case CPDF_Object::kArray: {
        CPDF_ArrayLocker locker(object->AsArray());
        for (const auto& child : locker) {
          PushIfNew(child, &visited, &worklist);
        }
        break;
      }
      case CPDF_Object::kDictionary: {
        CPDF_DictionaryLocker locker(object->AsDictionary());
        for (const auto& child : locker) {
          PushIfNew(child.second, &visited, &worklist);
        }
        break;
      }
      case CPDF_Object::kStream: {
        PushIfNew(object->AsStream()->GetDict(), &visited, &worklist);
        break;
      }
      default:
        break;
    }
  }

  Freeze();
  return true;
}

RetainPtr<const CPDF_Object> CPDF_BaseDocument::GetFrozenObjectForLayer(
    uint32_t objnum) const {
  if (RetainPtr<const CPDF_Object> cached = GetIndirectObject(objnum)) {
    return cached;
  }
  // Cache miss: parse the object from the base's own bytes, once, for
  // every layer. The cache is logically immutable state derived from the
  // file, hence the cast. The frozen view scope makes sure nothing resolved
  // while parsing (a stream's indirect /Length, say) routes through a
  // layer's overlay - a promoted clone must never enter the shared cache -
  // and the qualified call bypasses this document's own view-aware
  // override for the same reason.
  CPDF_BaseDocument* self = const_cast<CPDF_BaseDocument*>(this);
  CPDF_DocumentViewScope frozen_view(self);
  return pdfium::WrapRetain(
      self->CPDF_IndirectObjectHolder::GetOrParseIndirectObjectInternal(
          objnum));
}

#if DCHECK_IS_ON()
void CPDF_BaseDocument::RegisterLiveLayer(
    const CPDF_LayerDocument* layer) {
  DCHECK(layer);
  DCHECK_EQ(layer->GetBaseDocument(), this);
  DCHECK_EQ(std::find(live_layers_.begin(), live_layers_.end(), layer),
            live_layers_.end());
  live_layers_.push_back(layer);
}

void CPDF_BaseDocument::UnregisterLiveLayer(
    const CPDF_LayerDocument* layer) {
  auto it = std::find(live_layers_.begin(), live_layers_.end(), layer);
  DCHECK(it != live_layers_.end());
  live_layers_.erase(it);
}

bool CPDF_BaseDocument::IsObjectPromotedInAnyLiveLayer(
    uint32_t objnum) const {
  return std::any_of(
      live_layers_.begin(), live_layers_.end(),
      [objnum](const CPDF_LayerDocument* layer) {
        return layer->FindPromotedObject(objnum) != nullptr;
      });
}
#endif

CPDF_Object* CPDF_BaseDocument::GetOrParseIndirectObjectInternal(
    uint32_t objnum) {
  const CPDF_LayerDocument* layer =
      CPDF_DocumentViewScope::GetCurrentLayerForBase(this);
  if (layer && layer->HasPromotedObjects()) {
    RetainPtr<CPDF_Object> promoted = layer->FindPromotedObject(objnum);
    if (promoted) {
      return promoted.Get();
    }
  }
#if DCHECK_IS_ON()
  // A promoted object resolved without an effective or explicitly frozen view
  // is path-dependent: the answer silently depends on which holder owns the
  // reference that happened to lead here.
  DCHECK(layer || !IsObjectPromotedInAnyLiveLayer(objnum) ||
         CPDF_DocumentViewScope::IsFrozenForBase(this));
#endif
  return CPDF_Document::GetOrParseIndirectObjectInternal(objnum);
}
