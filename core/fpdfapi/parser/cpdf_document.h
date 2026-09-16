// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PARSER_CPDF_DOCUMENT_H_
#define CORE_FPDFAPI_PARSER_CPDF_DOCUMENT_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_security_handler.h"
#include "core/fxcrt/fx_memory.h"
#include "core/fxcrt/observed_ptr.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/unowned_ptr.h"

class CPDF_ReadValidator;
class CPDF_BaseDocument;
class CPDF_StreamAcc;
class IFX_SeekableReadStream;
class JBig2_DocumentContext;

class CPDF_Document : public Observable,
                      public CPDF_Parser::ParsedObjectsHolder {
 public:
  // Type from which the XFA extension can subclass itself.
  class Extension {
   public:
    virtual ~Extension() = default;
    virtual int GetPageCount() const = 0;
    virtual uint32_t DeletePage(int page_index) = 0;
    virtual bool ContainsExtensionForm() const = 0;
    virtual bool ContainsExtensionFullForm() const = 0;
    virtual bool ContainsExtensionForegroundForm() const = 0;
    virtual void PagesInserted(int page_index, size_t num_pages) = 0;
  };

  class LinkListIface {
   public:
    // CPDF_Document merely helps manage the lifetime.
    virtual ~LinkListIface() = default;
  };

  class PageDataIface {
   public:
    PageDataIface();
    virtual ~PageDataIface();

    virtual void ClearStockFont() = 0;
    virtual RetainPtr<CPDF_StreamAcc> GetFontFileStreamAcc(
        RetainPtr<const CPDF_Stream> font_stream) = 0;
    virtual void MaybePurgeFontFileStreamAcc(
        RetainPtr<CPDF_StreamAcc>&& stream_acc) = 0;
    virtual void MaybePurgeImage(uint32_t objnum) = 0;

    void SetDocument(CPDF_Document* doc) { doc_ = doc; }

   protected:
    CPDF_Document* GetDocument() const { return doc_; }

   private:
    UnownedPtr<CPDF_Document> doc_;
  };

  class RenderDataIface {
   public:
    RenderDataIface();
    virtual ~RenderDataIface();

    void SetDocument(CPDF_Document* doc) { doc_ = doc; }

   protected:
    CPDF_Document* GetDocument() const { return doc_; }

   private:
    UnownedPtr<CPDF_Document> doc_;
  };

  enum class PendingSecurityMode { kNone, kEncrypt, kRemove };

  struct PendingSecurity {
    PendingSecurityMode mode = PendingSecurityMode::kNone;
    RetainPtr<CPDF_Dictionary> encrypt_dict;
    RetainPtr<CPDF_SecurityHandler> security_handler;
  };

  static constexpr int kPageMaxNum = 0xFFFFF;

  static bool IsValidPageObject(const CPDF_Object* obj);

  CPDF_Document(std::unique_ptr<RenderDataIface> pRenderData,
                std::unique_ptr<PageDataIface> pPageData);
  ~CPDF_Document() override;

  Extension* GetExtension() const { return extension_.get(); }
  void SetExtension(std::unique_ptr<Extension> pExt) {
    extension_ = std::move(pExt);
  }

  // EmbedPDF: an opaque cache an SDK layer attaches to this document for its
  // lifetime. The bytes a document was loaded from never change, so nothing
  // invalidates it; it is destroyed before the parser it may refer to.
  class Attachment {
   public:
    virtual ~Attachment() = default;
  };
  Attachment* epdf_attachment() const { return epdf_attachment_.get(); }
  void SetEpdfAttachment(std::unique_ptr<Attachment> attachment) {
    epdf_attachment_ = std::move(attachment);
  }

  // EmbedPDF: session-scoped provenance for a resource name this document
  // instance handed out for a registered runtime font (a /DA font alias
  // reserved before its /DR entry exists). Never written to the file; a
  // reopened document starts empty, so an alias that merely looks reserved
  // resolves to nothing.
  void ReserveSessionFontAlias(const ByteString& alias, uint32_t font_id) {
    session_font_aliases_[alias] = font_id;
  }
  std::optional<uint32_t> LookupSessionFontAlias(
      const ByteString& alias) const {
    auto it = session_font_aliases_.find(alias);
    if (it == session_font_aliases_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  // EmbedPDF: how much of a registered font's program the resources built
  // for this document instance carry (§2 of the rich text Phase C note).
  // kDefault subsets annotation text and embeds form field text whole;
  // kSubset and kFull apply to both. Session state, never written to the
  // file; applies to resources built after the call. A font whose fsType
  // forbids subsetting is embedded whole under every policy.
  enum class FontEmbeddingPolicy : uint8_t { kDefault, kSubset, kFull };
  FontEmbeddingPolicy GetFontEmbeddingPolicy() const {
    return font_embedding_policy_;
  }
  void SetFontEmbeddingPolicy(FontEmbeddingPolicy policy) {
    font_embedding_policy_ = policy;
  }

  // EmbedPDF (rich text, Phase D): session switches for appearances laid
  // out by CPDF_RichTextLayout. Latin typographic features (kern, liga…)
  // are off for Acrobat parity unless turned on here. Plain FreeText (no
  // /RC) keeps the CPVT layout unless the rich engine is selected; an
  // annotation with /RC always uses the rich engine.
  bool GetTypographicFeaturesEnabled() const { return typographic_features_; }
  void SetTypographicFeaturesEnabled(bool enabled) {
    typographic_features_ = enabled;
  }

  virtual CPDF_Parser* GetParser() const;
  virtual const CPDF_Dictionary* GetRoot() const;
  virtual RetainPtr<CPDF_Dictionary> GetMutableRoot();
  virtual RetainPtr<CPDF_Dictionary> GetInfo();
  virtual RetainPtr<CPDF_Dictionary> GetMutableInfo();
  RetainPtr<CPDF_Dictionary> GetOrCreateInfo();
  RetainPtr<const CPDF_Array> GetFileIdentifier() const;

  // Returns the object number for the deleted page, or 0 on failure.
  uint32_t DeletePage(int iPage);
  // `page_obj_num` is the return value from DeletePage(). If it is non-zero,
  // and it is no longer used in the page tree, then replace the page object
  // with a null object.
  void SetPageToNullObject(uint32_t page_obj_num);
  bool MovePages(pdfium::span<const int> page_indices, int dest_page_index);

  int GetPageCount() const;
  bool IsPageLoaded(int iPage) const;
  virtual RetainPtr<const CPDF_Dictionary> GetPageDictionary(int iPage);
  virtual RetainPtr<CPDF_Dictionary> GetMutablePageDictionary(int iPage);
  int GetPageIndex(uint32_t objnum);
  // When `get_owner_perms` is true, returns full permissions if unlocked by
  // owner.
  virtual uint32_t GetUserPermissions(bool get_owner_perms) const;

  // PageDataIface wrappers, try to avoid explicit getter calls.
  RetainPtr<CPDF_StreamAcc> GetFontFileStreamAcc(
      RetainPtr<const CPDF_Stream> font_stream);
  void MaybePurgeFontFileStreamAcc(RetainPtr<CPDF_StreamAcc>&& stream_acc);
  void MaybePurgeImage(uint32_t objnum);

  // Returns a valid pointer, unless it is called during destruction.
  PageDataIface* GetPageData() const { return doc_page_.get(); }
  RenderDataIface* GetRenderData() const { return doc_render_.get(); }

  void SetPageObjNum(int iPage, uint32_t objNum);

  JBig2_DocumentContext* GetOrCreateCodecContext();
  LinkListIface* GetLinksContext() const { return links_context_.get(); }
  void SetLinksContext(std::unique_ptr<LinkListIface> context) {
    links_context_ = std::move(context);
  }

  // Behaves like NewIndirect<CPDF_Stream>(dict), but keeps track of the object
  // number assigned to the newly created stream.
  RetainPtr<CPDF_Stream> CreateModifiedAPStream(
      RetainPtr<CPDF_Dictionary> dict);

  // Returns whether CreateModifiedAPStream() created `stream`.
  bool IsModifiedAPStream(const CPDF_Stream* stream) const;

  void SetPendingSecurity(PendingSecurity pending);
  const PendingSecurity* GetPendingSecurity() const;
  void ClearPendingSecurity();

  // Returns whether `objnum` has been promoted from its base storage into a
  // document overlay. Always false for ordinary documents.
  virtual RetainPtr<CPDF_Object> FindPromotedObject(uint32_t objnum) const;
  bool IsObjectPromoted(uint32_t objnum) const;
  // EmbedPDF: the object as the document was LOADED with it. For a layer,
  // the ingested delta's version when the delta carried it, else the frozen
  // base object; for an ordinary document, a fresh parse of the object from
  // the loaded bytes. Null for an object the loaded bytes do not carry. A
  // twin is read-only, never mutated, never in an overlay.
  virtual RetainPtr<const CPDF_Object> GetLoadedTwin(uint32_t objnum) const;
  // EmbedPDF: the twin a SAVE compares against when it decides what to
  // write - the frozen base object for a layer, the loaded twin for an
  // ordinary document (its base IS its loaded bytes). Null when the base
  // does not carry the object. A removal that empties a container restores
  // the shape THIS twin has, so the object can be elided again.
  virtual RetainPtr<const CPDF_Object> GetBaseTwin(uint32_t objnum) const;
  // EmbedPDF: whether |stream|'s file-backed bytes are owned by something
  // this document retains for its whole life (its own parser's file, a
  // layer's base or loaded delta). Only then may a clone made for this
  // holder share the view instead of copying the bytes.
  bool SharesBackingStorageWith(const CPDF_Stream* stream) const override;
  // Changes whenever the effective identity of an indirect object can change.
  // Ordinary documents have no overlay and always return 0.
  virtual uint64_t GetOverlayEpoch() const;
  virtual bool IsLayerDocument() const;
  // Returns non-null only for the immutable base document itself. This avoids
  // RTTI in the ambient-view boundary while preserving exact frozen-view
  // identity for the debug detector.
  virtual const CPDF_BaseDocument* GetBaseDocumentForViewScope() const;
  virtual FX_FILESIZE GetLayerAppendBaseOffset() const;

  // CPDF_Parser::ParsedObjectsHolder:
  bool TryInit() override;
  RetainPtr<CPDF_Object> ParseIndirectObject(uint32_t objnum) override;

  CPDF_Parser::Error LoadDoc(RetainPtr<IFX_SeekableReadStream> pFileAccess,
                             const ByteString& password);
  CPDF_Parser::Error LoadLinearizedDoc(RetainPtr<CPDF_ReadValidator> validator,
                                       const ByteString& password);
  bool has_valid_cross_reference_table() const {
    return has_valid_cross_reference_table_;
  }

  void LoadPages();
  void CreateNewDoc();
  RetainPtr<CPDF_Dictionary> CreateNewPage(int iPage);

  void IncrementParsedPageCount() { ++parsed_page_count_; }
  uint32_t GetParsedPageCountForTesting() { return parsed_page_count_; }

  void SetRootForTesting(RetainPtr<CPDF_Dictionary> root);

 protected:
  void SetParser(std::unique_ptr<CPDF_Parser> pParser);

  void ResizePageListForTesting(size_t size);

  virtual uint32_t GetPageObjNumAt(size_t index) const;
  virtual void SetPageObjNumAt(size_t index, uint32_t objnum);
  virtual void InsertPageObjNum(size_t index, uint32_t objnum);
  virtual void ErasePageObjNum(size_t index);
  virtual void ResizePageList(size_t size);
  virtual size_t GetPageListSize() const;
  bool RebuildPageListFromCurrentPageTree();

  void SetCachedRootDict(RetainPtr<CPDF_Dictionary> root);
  void InvalidateCachedRootDict();
  void SetCachedInfoDict(RetainPtr<CPDF_Dictionary> info);
  void InvalidateCachedInfoDict();

  // EmbedPDF layer documents represent deletion by removing references from the
  // promoted owning container, e.g. /Pages /Kids or /Annots. Base objects
  // remain resolvable through the base xref and are not null-replaced in
  // append-only layer deltas.
  virtual bool ShouldReplaceDeletedPageWithNull(uint32_t page_obj_num) const;

 private:
  class StockFontClearer {
   public:
    FX_STACK_ALLOCATED();

    explicit StockFontClearer(CPDF_Document::PageDataIface* pPageData);
    ~StockFontClearer();

   private:
    UnownedPtr<CPDF_Document::PageDataIface> const page_data_;
  };

  // Retrieve page count information by getting count value from the tree nodes
  int RetrievePageCount();

  // When this method is called, tree_traversal_[level] exists.
  RetainPtr<CPDF_Dictionary> TraversePDFPages(int iPage,
                                              int* nPagesToGo,
                                              size_t level);

  RetainPtr<const CPDF_Dictionary> GetPagesDict() const;
  RetainPtr<CPDF_Dictionary> GetMutablePagesDict();

  bool InsertDeletePDFPage(RetainPtr<CPDF_Dictionary> pages_dict,
                           int pages_to_go,
                           RetainPtr<CPDF_Dictionary> page_dict,
                           bool is_insert,
                           std::set<RetainPtr<CPDF_Dictionary>>* visited);

  bool InsertNewPage(int iPage, RetainPtr<CPDF_Dictionary> pPageDict);
  void ResetTraversal();
  CPDF_Parser::Error HandleLoadResult(CPDF_Parser::Error error);

  std::unique_ptr<CPDF_Parser> parser_;
  RetainPtr<CPDF_Dictionary> root_dict_;
  RetainPtr<CPDF_Dictionary> info_dict_;

  // Vector of pairs to know current position in the page tree. The index in the
  // vector corresponds to the level being described. The pair contains a
  // pointer to the dictionary being processed at the level, and an index of the
  // of the child being processed within the dictionary's /Kids array.
  std::vector<std::pair<RetainPtr<CPDF_Dictionary>, size_t>> tree_traversal_;

  // True if the CPDF_Parser succeeded without having to rebuild the cross
  // reference table.
  bool has_valid_cross_reference_table_ = false;

  // Index of the next page that will be traversed from the page tree.
  bool reached_max_page_level_ = false;
  int next_page_to_traverse_ = 0;
  uint32_t parsed_page_count_ = 0;

  std::unique_ptr<RenderDataIface> const doc_render_;
  // Must be after `doc_render_`.
  std::unique_ptr<PageDataIface> const doc_page_;
  std::unique_ptr<JBig2_DocumentContext> codec_context_;
  std::unique_ptr<LinkListIface> links_context_;
  std::set<uint32_t> modified_apstream_ids_;
  std::optional<PendingSecurity> pending_security_;
  std::vector<uint32_t> page_list_;  // Page number to page's dict objnum.
  std::map<ByteString, uint32_t> session_font_aliases_;  // EmbedPDF, see above.
  FontEmbeddingPolicy font_embedding_policy_ =
      FontEmbeddingPolicy::kDefault;  // EmbedPDF, see above.
  bool typographic_features_ = false;  // EmbedPDF, see above.

  // EmbedPDF: destroyed before everything declared above it (the parser
  // included), after the extension and the stock font clearer.
  std::unique_ptr<Attachment> epdf_attachment_;

  // Must be second to last.
  StockFontClearer stock_font_clearer_;

  // Must be last. Destroy the extension before any non-extension teardown.
  std::unique_ptr<Extension> extension_;
};

#endif  // CORE_FPDFAPI_PARSER_CPDF_DOCUMENT_H_
