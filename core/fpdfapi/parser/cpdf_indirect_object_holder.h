// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PARSER_CPDF_INDIRECT_OBJECT_HOLDER_H_
#define CORE_FPDFAPI_PARSER_CPDF_INDIRECT_OBJECT_HOLDER_H_

#include <stdint.h>

#include <map>
#include <type_traits>
#include <utility>

#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fxcrt/bytestring_pool.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/weak_ptr.h"

// EmbedPDF(layer-door): see SharesBackingStorageWith().
class CPDF_Stream;

class CPDF_IndirectObjectHolder {
 public:
  using const_iterator =
      std::map<uint32_t, RetainPtr<CPDF_Object>>::const_iterator;

  CPDF_IndirectObjectHolder();
  virtual ~CPDF_IndirectObjectHolder();

  // EmbedPDF(layer-seam): virtual, so a layer document reads through its
  // overlay and writes through its door.
  virtual RetainPtr<CPDF_Object> GetOrParseIndirectObject(uint32_t objnum);
  virtual RetainPtr<const CPDF_Object> GetIndirectObject(uint32_t objnum) const;
  virtual RetainPtr<CPDF_Object> GetMutableIndirectObject(uint32_t objnum);
  virtual void DeleteIndirectObject(uint32_t objnum);

  // Creates and adds a new object retained by the indirect object holder,
  // and returns a retained pointer to it.
  template <typename T, typename... Args>
  RetainPtr<T> NewIndirect(Args&&... args) {
    auto obj = New<T>(std::forward<Args>(args)...);
    AddIndirectObject(obj);
    return obj;
  }

  // EmbedPDF(chosen-object-numbers): creates a new object at |objnum| (see
  // AddIndirectObjectAt()), or returns null when that number can't take one.
  template <typename T, typename... Args>
  RetainPtr<T> NewIndirectAt(uint32_t objnum, Args&&... args) {
    auto obj = New<T>(std::forward<Args>(args)...);
    if (!AddIndirectObjectAt(objnum, obj)) {
      return nullptr;
    }
    return obj;
  }

  // Creates and adds a new object not retained by the indirect object holder,
  // but which can intern strings from it. We have a special cast to handle
  // objects that can intern strings from our ByteStringPool.
  template <typename T, typename... Args>
    requires(CanInternStrings<T>::value)
  RetainPtr<T> New(Args&&... args) {
    return pdfium::MakeRetain<T>(byte_string_pool_,
                                 std::forward<Args>(args)...);
  }
  template <typename T, typename... Args>
    requires(!CanInternStrings<T>::value)
  RetainPtr<T> New(Args&&... args) {
    return pdfium::MakeRetain<T>(std::forward<Args>(args)...);
  }

  // Always Retains |pObj|, returns its new object number.
  // EmbedPDF(layer-seam): virtual, so a layer document can route a new object
  // into an open transaction instead of its committed objects.
  virtual uint32_t AddIndirectObject(RetainPtr<CPDF_Object> pObj);

  // EmbedPDF(chosen-object-numbers): whether a new object can be stored as
  // |objnum|, a number handed out earlier that holds nothing. Only a layer
  // document can do that (see CPDF_LayerDocument::CanAddIndirectObjectAt());
  // false everywhere else.
  virtual bool CanAddIndirectObjectAt(uint32_t objnum) const;
  // EmbedPDF(chosen-object-numbers): stores |pObj| as object |objnum| when
  // CanAddIndirectObjectAt() allows it, without moving the last object
  // number. Returns whether it did.
  virtual bool AddIndirectObjectAt(uint32_t objnum,
                                   RetainPtr<CPDF_Object> pObj);

  // If higher generation number, retains |pObj| and returns true.
  // EmbedPDF(layer-seam): virtual for the same reason as AddIndirectObject().
  virtual bool ReplaceIndirectObjectIfHigherGeneration(
      uint32_t objnum,
      RetainPtr<CPDF_Object> pObj);

  uint32_t GetLastObjNum() const { return last_obj_num_; }
  void SetLastObjNum(uint32_t objnum) { last_obj_num_ = objnum; }
  // EmbedPDF(write-generation): a base document's objects are frozen; every
  // layer over it shares them.
  // Freeze every cached object and refuse mutation from here on (adding,
  // replacing, deleting, promoting). Parsing stays open: an object the
  // parser produces later from the same immutable bytes enters the cache
  // frozen. That is what lets a base document parse on first touch instead
  // of walking its whole reachable graph at load.
  void Freeze();
  bool IsHolderFrozen() const { return frozen_; }

  WeakPtr<ByteStringPool> GetByteStringPool() const {
    return byte_string_pool_;
  }

  const_iterator begin() const { return indirect_objs_.begin(); }
  const_iterator end() const { return indirect_objs_.end(); }

  // EmbedPDF(layer-door): whether a file-backed stream's bytes are owned by
  // storage this holder retains for its whole life, so a clone made for this
  // holder may share the view (see CPDF_Stream::CloneForHolderNonCyclic). A
  // bare holder owns no storage.
  virtual bool SharesBackingStorageWith(const CPDF_Stream* stream) const;

 protected:
  virtual RetainPtr<CPDF_Object> ParseIndirectObject(uint32_t objnum);
  // EmbedPDF(layer-seam): protected and virtual, so a layer resolves objects
  // through its overlay. Upstream keeps them private.
  virtual const CPDF_Object* GetIndirectObjectInternal(uint32_t objnum) const;
  virtual CPDF_Object* GetOrParseIndirectObjectInternal(uint32_t objnum);

  // EmbedPDF(layer-door): a layer's own objects, and the copies it promotes
  // from its base.
  RetainPtr<CPDF_Object> FindLocalIndirectObject(uint32_t objnum) const;
  void AddPromotedObject(uint32_t objnum, RetainPtr<CPDF_Object> object);

 private:
  friend class CPDF_Reference;

  // EmbedPDF(layer-seam): the Get*IndirectObjectInternal() pair moved to
  // protected, above.
  uint32_t last_obj_num_ = 0;
  // EmbedPDF(write-generation): a frozen holder refuses writes (see Freeze()).
  bool frozen_ = false;
  std::map<uint32_t, RetainPtr<CPDF_Object>> indirect_objs_;
  WeakPtr<ByteStringPool> byte_string_pool_;
};

#endif  // CORE_FPDFAPI_PARSER_CPDF_INDIRECT_OBJECT_HOLDER_H_
