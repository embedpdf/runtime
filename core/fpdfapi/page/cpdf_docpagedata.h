// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PAGE_CPDF_DOCPAGEDATA_H_
#define CORE_FPDFAPI_PAGE_CPDF_DOCPAGEDATA_H_

#include <map>
#include <memory>
#include <set>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_codepage_forward.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"

class CFX_Font;
class CPDF_Dictionary;
class CPDF_FontEncoding;
class CPDF_IccProfile;
class CPDF_Image;
class CPDF_Object;
class CPDF_Pattern;
class CPDF_Stream;
class CPDF_StreamAcc;

class CPDF_DocPageData final : public CPDF_Document::PageDataIface,
                               public CPDF_Font::FormFactoryIface {
 public:
  static CPDF_DocPageData* FromDocument(const CPDF_Document* doc);

  CPDF_DocPageData();
  explicit CPDF_DocPageData(CPDF_DocPageData* fallback);
  ~CPDF_DocPageData() override;

  void SetFallback(CPDF_DocPageData* fallback) { fallback_ = fallback; }

  // CPDF_Document::PageDataIface:
  void ClearStockFont() override;
  RetainPtr<CPDF_StreamAcc> GetFontFileStreamAcc(
      RetainPtr<const CPDF_Stream> font_stream) override;
  void MaybePurgeFontFileStreamAcc(
      RetainPtr<CPDF_StreamAcc>&& stream_acc) override;
  void MaybePurgeImage(uint32_t dwStreamObjNum) override;

  // CPDF_Font::FormFactoryIFace:
  std::unique_ptr<CPDF_Font::FormIface> CreateForm(
      CPDF_Document* document,
      RetainPtr<CPDF_Dictionary> pPageResources,
      RetainPtr<CPDF_Stream> pFormStream) override;

  bool IsForceClear() const { return force_clear_; }

  RetainPtr<CPDF_Font> AddFont(std::unique_ptr<CFX_Font> font,
                               FX_Charset charset);
  RetainPtr<CPDF_Font> GetFont(RetainPtr<CPDF_Dictionary> font_dict);
  // EmbedPDF: discard a render-only font before its scratch resource holder
  // dies. Call only after every form using this private dictionary is gone.
  void ForgetEphemeralFont(const CPDF_Dictionary* font_dict);
  RetainPtr<CPDF_Font> AddStandardFont(const ByteString& fontName,
                                       const CPDF_FontEncoding* pEncoding);
  RetainPtr<CPDF_Font> GetStandardFont(const ByteString& fontName,
                                       const CPDF_FontEncoding* pEncoding);
#if BUILDFLAG(IS_WIN)
  RetainPtr<CPDF_Font> AddWindowsFont(LOGFONTA* pLogFont);
#endif

  // Loads a colorspace.
  RetainPtr<CPDF_ColorSpace> GetColorSpace(const CPDF_Object* pCSObj,
                                           const CPDF_Dictionary* pResources);

  // Loads a colorspace in a context that might be while loading another
  // colorspace. |pVisited| is passed recursively to avoid circular calls
  // involving CPDF_ColorSpace::Load().
  RetainPtr<CPDF_ColorSpace> GetColorSpaceGuarded(
      const CPDF_Object* pCSObj,
      const CPDF_Dictionary* pResources,
      std::set<const CPDF_Object*>* pVisited);

  RetainPtr<CPDF_Pattern> GetPattern(RetainPtr<CPDF_Object> pPatternObj,
                                     const CFX_Matrix& matrix);
  RetainPtr<CPDF_ShadingPattern> GetShading(RetainPtr<CPDF_Object> pPatternObj,
                                            const CFX_Matrix& matrix);

  RetainPtr<CPDF_Image> GetImage(uint32_t dwStreamObjNum);

  RetainPtr<CPDF_IccProfile> GetIccProfile(
      RetainPtr<const CPDF_Stream> pProfileStream);

 private:
  struct HashIccProfileKey {
    HashIccProfileKey(DataVector<uint8_t> digest, uint32_t components);
    HashIccProfileKey(const HashIccProfileKey& that);
    ~HashIccProfileKey();

    bool operator<(const HashIccProfileKey& other) const;

    DataVector<uint8_t> digest;
    uint32_t components;
  };

  // Loads a colorspace in a context that might be while loading another
  // colorspace, or even in a recursive call from this method itself. |pVisited|
  // is passed recursively to avoid circular calls involving
  // CPDF_ColorSpace::Load() and |pVisitedInternal| is also passed recursively
  // to avoid circular calls with this method calling itself.
  RetainPtr<CPDF_ColorSpace> GetColorSpaceInternal(
      const CPDF_Object* pCSObj,
      const CPDF_Dictionary* pResources,
      std::set<const CPDF_Object*>* pVisited,
      std::set<const CPDF_Object*>* pVisitedInternal);
  bool CanUseFallbackForObject(const CPDF_Object* object) const;

  size_t CalculateEncodingDict(FX_Charset charset, CPDF_Dictionary* pBaseDict);
  RetainPtr<CPDF_Dictionary> ProcessbCJK(
      RetainPtr<CPDF_Dictionary> pBaseDict,
      FX_Charset charset,
      ByteString basefont,
      std::function<void(wchar_t, wchar_t, CPDF_Array*)> Insert);

  bool force_clear_ = false;
  UnownedPtr<CPDF_DocPageData> fallback_;

  // Specific destruction order may be required between maps.
  std::map<HashIccProfileKey, RetainPtr<const CPDF_Stream>>
      hash_icc_profile_map_;
  std::map<RetainPtr<const CPDF_Array>, RetainPtr<CPDF_ColorSpace>>
      color_space_map_;
  std::map<RetainPtr<const CPDF_Stream>, RetainPtr<CPDF_StreamAcc>>
      font_file_map_;
  std::map<RetainPtr<const CPDF_Stream>, RetainPtr<CPDF_IccProfile>>
      icc_profile_map_;
  std::map<RetainPtr<const CPDF_Object>, RetainPtr<CPDF_Pattern>> pattern_map_;
  std::map<uint32_t, RetainPtr<CPDF_Image>> image_map_;
  std::map<RetainPtr<const CPDF_Dictionary>, RetainPtr<CPDF_Font>> font_map_;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_DOCPAGEDATA_H_
