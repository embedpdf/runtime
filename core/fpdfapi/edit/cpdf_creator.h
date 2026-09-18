// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_EDIT_CPDF_CREATOR_H_
#define CORE_FPDFAPI_EDIT_CPDF_CREATOR_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/fx_string.h"
#include "core/fxcrt/mask.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"

class CPDF_Array;
class CPDF_CryptoHandler;
class CPDF_SecurityHandler;
class CPDF_Dictionary;
class CPDF_Document;
class CPDF_Object;
class CPDF_Parser;

class CPDF_Creator {
 public:
  enum CreateFlags : uint32_t {
    kNone = 0,
    kIncremental = (1 << 0),
    kNoOriginal = (1 << 1),
    kRemoveSecurityDeprecated = 3,
    kRemoveSecurity = (1 << 2),
    // TODO(crbug.com/42270430): Implement font subsetting.
    kSubsetNewFonts = (1 << 3),
    kIncrementalAppendOnly = (1 << 4),
    // EmbedPDF: an incremental save of a layer document writes nothing at
    // all when no reachable object differs from the document the layer was
    // opened with; IsUnchangedSinceLoad() then reports it and the caller
    // returns the loaded bytes verbatim.
    kSkipIfUnchangedSinceLoad = (1 << 5),
  };

  enum class FailureReason {
    kNone,
    kAppendOnlyOffsetTooLarge,
    kArchiveError,
    kOther,
  };

  CPDF_Creator(CPDF_Document* doc,
               RetainPtr<IFX_RetainableWriteStream> archive);
  ~CPDF_Creator();

  bool Create(Mask<CreateFlags> flags, int32_t file_version);
  FailureReason GetFailureReason() const { return failure_reason_; }

  // Experimental EmbedPDF Extension: where the last Create() wrote each
  // object it emitted, and where its cross-reference section starts. Valid
  // after a successful Create(); used to locate a signature dictionary's
  // placeholders in the saved bytes without searching.
  const std::map<uint32_t, FX_FILESIZE>& object_offsets() const {
    return object_offsets_;
  }
  FX_FILESIZE xref_start() const { return xref_start_; }

  static ByteString FormatXrefOffset10ForTesting(FX_FILESIZE offset);

  // EmbedPDF: what the last incremental save of a layer found. A save
  // writes what changed from the BASE (overlay objects equal to their base
  // twin are elided); whether anything reachable changed since the layer
  // was LOADED is a separate answer, used by callers to keep what they
  // have. Valid after Create() on a layer document.
  bool changed_since_load() const { return changed_since_load_; }
  // True when Create() wrote nothing because kSkipIfUnchangedSinceLoad was
  // set and nothing changed since load.
  bool IsUnchangedSinceLoad() const { return decided_unchanged_; }

  // Experimental EmbedPDF Extension: Set encryption for documents that weren't
  // originally encrypted. This sets both encrypt_dict_ (for trailer writing)
  // and security_handler_ (for GetCryptoHandler() used in stream/string
  // encryption).
  void SetEncryption(RetainPtr<CPDF_Dictionary> encrypt_dict,
                     RetainPtr<CPDF_SecurityHandler> security_handler);

 private:
  enum class Stage {
    kInvalid = -1,
    kInit0 = 0,
    kWriteHeader10 = 10,
    kWriteIncremental15 = 15,
    kInitWriteObjs20 = 20,
    kInitWriteNewObjs25 = 25,
    kWriteNewObjs26 = 26,
    kWriteEncryptDict27 = 27,
    kInitWriteXRefs80 = 80,
    kWriteXrefsNotIncremental81 = 81,
    kWriteXrefsIncremental82 = 82,
    kWriteTrailerAndFinish90 = 90,
    kComplete100 = 100,
  };

  bool Continue();
  void Clear();

  void InitNewObjNumOffsets();
  void InitID();

  CPDF_Creator::Stage WriteDoc_Stage1();
  CPDF_Creator::Stage WriteDoc_Stage2();
  CPDF_Creator::Stage WriteDoc_Stage3();
  CPDF_Creator::Stage WriteDoc_Stage4();

  bool WriteFullDocument();
  bool WriteNewObjs();
  bool WriteIndirectObj(uint32_t objnum, const CPDF_Object* pObj);
  bool CheckEmittedOffset(FX_FILESIZE offset);

  void RemoveSecurity();

  CPDF_CryptoHandler* GetCryptoHandler();

  UnownedPtr<CPDF_Document> const document_;
  UnownedPtr<CPDF_Parser> const parser_;
  RetainPtr<const CPDF_Dictionary> encrypt_dict_;
  RetainPtr<CPDF_Dictionary> new_encrypt_dict_;
  RetainPtr<CPDF_SecurityHandler> security_handler_;
  uint32_t last_obj_num_;
  std::unique_ptr<IFX_ArchiveStream> archive_;
  FX_FILESIZE saved_offset_ = 0;
  Stage stage_ = Stage::kInvalid;
  uint32_t cur_obj_num_ = 0;
  FX_FILESIZE xref_start_ = 0;
  std::map<uint32_t, FX_FILESIZE> object_offsets_;
  std::vector<uint32_t> new_obj_num_array_;  // Sorted, ascending.
  std::set<uint32_t> objects_with_refs_;
  // EmbedPDF L2: overlay objects equal to their base twin (not written).
  std::set<uint32_t> elided_;
  bool changed_since_load_ = false;
  bool decided_unchanged_ = false;
  // A layer save with nothing to write appends no revision at all.
  bool skip_empty_revision_ = false;
  RetainPtr<CPDF_Array> id_array_;
  int32_t file_version_ = 0;
  bool security_changed_ = false;
  bool is_incremental_ = false;
  bool is_original_ = false;
  bool is_incremental_append_only_ = false;
  FailureReason failure_reason_ = FailureReason::kNone;
};

#endif  // CORE_FPDFAPI_EDIT_CPDF_CREATOR_H_
