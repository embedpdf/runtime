// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/parser/cpdf_document.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_linearized_header.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_null.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_read_validator.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fxcodec/jbig2/JBig2_DocumentContext.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/containers/contains.h"
#include "core/fxcrt/fx_codepage.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/scoped_set_insertion.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/stl_util.h"

namespace {

const int kMaxPageLevel = 1024;

enum class NodeType : bool {
  kBranch,  // /Type /Pages, AKA page tree node.
  kLeaf,    // /Type /Page, AKA page object.
};

// Note that this function may modify `kid_dict` to correct PDF spec violations.
// Same reasoning as CountPages() below.
NodeType GetNodeType(RetainPtr<CPDF_Dictionary> kid_dict) {
  const ByteString kid_type_value = kid_dict->GetNameFor("Type");
  if (kid_type_value == "Pages") {
    return NodeType::kBranch;
  }
  if (kid_type_value == "Page") {
    return NodeType::kLeaf;
  }

  // Even though /Type is required for page tree nodes and page objects, PDFs
  // may not have them or have the wrong type. Tolerate these errors and guess
  // the type. Then fix the in-memory representation.
  const bool has_kids = kid_dict->KeyExist("Kids");
  kid_dict->SetNewFor<CPDF_Name>("Type", has_kids ? "Pages" : "Page");
  return has_kids ? NodeType::kBranch : NodeType::kLeaf;
}

NodeType GetNodeTypeForTraversal(const CPDF_Dictionary* kid_dict) {
  const ByteString kid_type_value = kid_dict->GetNameFor("Type");
  if (kid_type_value == "Pages") {
    return NodeType::kBranch;
  }
  if (kid_type_value == "Page") {
    return NodeType::kLeaf;
  }

  return kid_dict->KeyExist("Kids") ? NodeType::kBranch : NodeType::kLeaf;
}

// Returns a value in the range [0, `CPDF_Document::kPageMaxNum`), or nullopt on
// error. Note that this function may modify `pages_dict` to correct PDF spec
// violations. By normalizing the in-memory representation, other code that
// reads the object do not have to deal with the same spec violations again.
// If the PDF gets saved, the saved copy will also be more spec-compliant.
std::optional<int> CountPages(
    RetainPtr<CPDF_Dictionary> pages_dict,
    std::set<RetainPtr<CPDF_Dictionary>>* visited_pages) {
  // Required. See ISO 32000-1:2008 spec, table 29, but tolerate page tree nodes
  // that violate the spec.
  int count_from_dict = pages_dict->GetIntegerFor("Count");
  if (count_from_dict > 0 && count_from_dict < CPDF_Document::kPageMaxNum) {
    return count_from_dict;
  }

  RetainPtr<CPDF_Array> kids_array = pages_dict->GetMutableArrayFor("Kids");
  if (!kids_array) {
    return 0;
  }

  int count = 0;
  for (size_t i = 0; i < kids_array->size(); i++) {
    RetainPtr<CPDF_Dictionary> kid_dict = kids_array->GetMutableDictAt(i);
    if (!kid_dict || pdfium::Contains(*visited_pages, kid_dict)) {
      continue;
    }

    NodeType kid_type = GetNodeType(kid_dict);
    if (kid_type == NodeType::kBranch) {
      // Use |visited_pages| to help detect circular references of pages.
      ScopedSetInsertion local_add(visited_pages, kid_dict);
      std::optional<int> local_count =
          CountPages(std::move(kid_dict), visited_pages);
      if (!local_count.has_value()) {
        return std::nullopt;  // Propagate error.
      }
      count += local_count.value();
    } else {
      CHECK_EQ(kid_type, NodeType::kLeaf);
      count++;
    }

    if (count >= CPDF_Document::kPageMaxNum) {
      return std::nullopt;  // Error: too many pages.
    }
  }
  // Fix the in-memory representation for page tree nodes that violate the spec.
  pages_dict->SetNewFor<CPDF_Number>("Count", count);
  return count;
}

int FindPageIndex(const CPDF_Dictionary* pNode,
                  uint32_t* skip_count,
                  uint32_t objnum,
                  int* index,
                  int level) {
  if (!pNode->KeyExist("Kids")) {
    if (objnum == pNode->GetObjNum()) {
      return *index;
    }

    if (*skip_count != 0) {
      (*skip_count)--;
    }

    (*index)++;
    return -1;
  }

  RetainPtr<const CPDF_Array> pKidList = pNode->GetArrayFor("Kids");
  if (!pKidList) {
    return -1;
  }

  if (level >= kMaxPageLevel) {
    return -1;
  }

  size_t count = pNode->GetIntegerFor("Count");
  if (count <= *skip_count) {
    (*skip_count) -= count;
    (*index) += count;
    return -1;
  }

  if (count && count == pKidList->size()) {
    for (size_t i = 0; i < count; i++) {
      RetainPtr<const CPDF_Reference> pKid =
          ToReference(pKidList->GetObjectAt(i));
      if (pKid && pKid->GetRefObjNum() == objnum) {
        return static_cast<int>(*index + i);
      }
    }
  }

  for (size_t i = 0; i < pKidList->size(); i++) {
    RetainPtr<const CPDF_Dictionary> pKid = pKidList->GetDictAt(i);
    if (!pKid || pKid == pNode) {
      continue;
    }

    int found_index =
        FindPageIndex(pKid.Get(), skip_count, objnum, index, level + 1);
    if (found_index >= 0) {
      return found_index;
    }
  }
  return -1;
}

bool AppendPageObjNumsConst(const CPDF_Dictionary* node,
                            const CPDF_Document* document,
                            size_t level,
                            std::set<const CPDF_Dictionary*>* visited,
                            std::vector<uint32_t>* page_objnums) {
  if (!node || !visited->insert(node).second || level >= kMaxPageLevel) {
    return false;
  }

  RetainPtr<const CPDF_Array> kids = node->GetArrayFor("Kids");
  if (!kids) {
    if (!ValidateDictType(node, "Page") || !node->GetObjNum()) {
      return false;
    }
    page_objnums->push_back(node->GetObjNum());
    return true;
  }

  CPDF_ArrayLocker locker(kids.Get());
  for (const auto& kid : locker) {
    RetainPtr<const CPDF_Object> direct = kid;
    if (RetainPtr<const CPDF_Reference> ref = ToReference(direct)) {
      direct = document->GetIndirectObject(ref->GetRefObjNum());
    } else if (kid) {
      direct = kid->GetDirect();
    }
    RetainPtr<const CPDF_Dictionary> kid_dict = ToDictionary(direct);
    if (!kid_dict ||
        !AppendPageObjNumsConst(kid_dict.Get(), document, level + 1, visited,
                                page_objnums)) {
      return false;
    }
  }
  return true;
}

}  // namespace

CPDF_Document::CPDF_Document(std::unique_ptr<RenderDataIface> pRenderData,
                             std::unique_ptr<PageDataIface> pPageData)
    : doc_render_(std::move(pRenderData)),
      doc_page_(std::move(pPageData)),
      stock_font_clearer_(doc_page_.get()) {
  doc_render_->SetDocument(this);
  doc_page_->SetDocument(this);
}

CPDF_Document::~CPDF_Document() {
  // Be absolutely certain that |extension_| is null before destroying
  // the extension, to avoid re-entering it while being destroyed. clang
  // seems to already do this for us, but the C++ standards seem to
  // indicate the opposite.
  extension_.reset();
}

// static
bool CPDF_Document::IsValidPageObject(const CPDF_Object* obj) {
  // See ISO 32000-1:2008 spec, table 30.
  return ValidateDictType(ToDictionary(obj), "Page");
}

CPDF_Parser* CPDF_Document::GetParser() const {
  return parser_.get();
}

const CPDF_Dictionary* CPDF_Document::GetRoot() const {
  return root_dict_.Get();
}

RetainPtr<CPDF_Dictionary> CPDF_Document::GetMutableRoot() {
  return root_dict_;
}

RetainPtr<CPDF_Object> CPDF_Document::ParseIndirectObject(uint32_t objnum) {
  CPDF_Parser* parser = GetParser();
  return parser ? parser->ParseIndirectObject(objnum) : nullptr;
}

bool CPDF_Document::TryInit() {
  CPDF_Parser* parser = GetParser();
  SetLastObjNum(parser->GetLastObjNum());

  RetainPtr<CPDF_Object> pRootObj =
      GetOrParseIndirectObject(parser->GetRootObjNum());
  if (pRootObj) {
    SetCachedRootDict(pRootObj->GetMutableDict());
  }

  LoadPages();
  return GetRoot() && GetPageCount() > 0;
}

CPDF_Parser::Error CPDF_Document::LoadDoc(
    RetainPtr<IFX_SeekableReadStream> pFileAccess,
    const ByteString& password) {
  if (!GetParser()) {
    SetParser(std::make_unique<CPDF_Parser>(this));
  }

  return HandleLoadResult(
      GetParser()->StartParse(std::move(pFileAccess), password));
}

CPDF_Parser::Error CPDF_Document::LoadLinearizedDoc(
    RetainPtr<CPDF_ReadValidator> validator,
    const ByteString& password) {
  if (!GetParser()) {
    SetParser(std::make_unique<CPDF_Parser>(this));
  }

  return HandleLoadResult(
      GetParser()->StartLinearizedParse(std::move(validator), password));
}

void CPDF_Document::LoadPages() {
  const CPDF_LinearizedHeader* linearized_header =
      GetParser()->GetLinearizedHeader();
  if (!linearized_header) {
    ResizePageList(RetrievePageCount());
    return;
  }

  uint32_t objnum = linearized_header->GetFirstPageObjNum();
  if (!IsValidPageObject(GetOrParseIndirectObject(objnum).Get())) {
    ResizePageList(RetrievePageCount());
    return;
  }

  uint32_t first_page_num = linearized_header->GetFirstPageNo();
  uint32_t page_count = linearized_header->GetPageCount();
  DCHECK(first_page_num < page_count);
  ResizePageList(page_count);
  SetPageObjNumAt(first_page_num, objnum);
}

RetainPtr<CPDF_Dictionary> CPDF_Document::TraversePDFPages(int iPage,
                                                           int* nPagesToGo,
                                                           size_t level) {
  if (*nPagesToGo < 0 || reached_max_page_level_) {
    return nullptr;
  }

  RetainPtr<CPDF_Dictionary> pPages = tree_traversal_[level].first;
  RetainPtr<CPDF_Array> pKidList = pPages->GetMutableArrayFor("Kids");
  if (!pKidList) {
    tree_traversal_.pop_back();
    if (*nPagesToGo != 1) {
      return nullptr;
    }
    SetPageObjNumAt(iPage, pPages->GetObjNum());
    return pPages;
  }
  if (level >= kMaxPageLevel) {
    tree_traversal_.pop_back();
    reached_max_page_level_ = true;
    return nullptr;
  }
  RetainPtr<CPDF_Dictionary> page;
  for (size_t i = tree_traversal_[level].second; i < pKidList->size(); i++) {
    if (*nPagesToGo == 0) {
      break;
    }
    pKidList->ConvertToIndirectObjectAt(i, this);
    RetainPtr<CPDF_Dictionary> pKid = pKidList->GetMutableDictAt(i);
    if (!pKid) {
      (*nPagesToGo)--;
      tree_traversal_[level].second++;
      continue;
    }
    if (pKid == pPages) {
      tree_traversal_[level].second++;
      continue;
    }
    if (!pKid->KeyExist("Kids")) {
      SetPageObjNumAt(iPage - (*nPagesToGo) + 1, pKid->GetObjNum());
      (*nPagesToGo)--;
      tree_traversal_[level].second++;
      if (*nPagesToGo == 0) {
        page = std::move(pKid);
        break;
      }
    } else {
      // If the vector has size level+1, the child is not in yet
      if (tree_traversal_.size() == level + 1) {
        tree_traversal_.emplace_back(std::move(pKid), 0);
      }
      // Now tree_traversal_[level+1] should exist and be equal to pKid.
      RetainPtr<CPDF_Dictionary> pPageKid =
          TraversePDFPages(iPage, nPagesToGo, level + 1);
      // Check if child was completely processed, i.e. it popped itself out
      if (tree_traversal_.size() == level + 1) {
        tree_traversal_[level].second++;
      }
      // If child did not finish, no pages to go, or max level reached, end
      if (tree_traversal_.size() != level + 1 || *nPagesToGo == 0 ||
          reached_max_page_level_) {
        page = std::move(pPageKid);
        break;
      }
    }
  }
  if (tree_traversal_[level].second == pKidList->size()) {
    tree_traversal_.pop_back();
  }
  return page;
}

void CPDF_Document::ResetTraversal() {
  next_page_to_traverse_ = 0;
  reached_max_page_level_ = false;
  tree_traversal_.clear();
}

bool CPDF_Document::RebuildPageListFromCurrentPageTree() {
  RetainPtr<const CPDF_Dictionary> root = pdfium::WrapRetain(GetRoot());
  if (!root && GetParser()) {
    root = ToDictionary(GetIndirectObject(GetParser()->GetRootObjNum()));
  }
  RetainPtr<const CPDF_Object> pages_object =
      root ? root->GetObjectFor("Pages") : nullptr;
  if (RetainPtr<const CPDF_Reference> ref = ToReference(pages_object)) {
    pages_object = GetIndirectObject(ref->GetRefObjNum());
  } else if (pages_object) {
    pages_object = pages_object->GetDirect();
  }
  RetainPtr<const CPDF_Dictionary> pages = ToDictionary(pages_object);
  std::set<const CPDF_Dictionary*> visited;
  std::vector<uint32_t> page_objnums;
  if (!AppendPageObjNumsConst(pages.Get(), this, /*level=*/0, &visited,
                              &page_objnums) ||
      page_objnums.empty() || page_objnums.size() >= kPageMaxNum) {
    return false;
  }

  ResizePageList(page_objnums.size());
  for (size_t i = 0; i < page_objnums.size(); ++i) {
    SetPageObjNumAt(i, page_objnums[i]);
  }
  ResetTraversal();
  return true;
}

void CPDF_Document::SetParser(std::unique_ptr<CPDF_Parser> pParser) {
  DCHECK(!parser_);
  parser_ = std::move(pParser);
}

CPDF_Parser::Error CPDF_Document::HandleLoadResult(CPDF_Parser::Error error) {
  if (error == CPDF_Parser::SUCCESS) {
    has_valid_cross_reference_table_ = !GetParser()->xref_table_rebuilt();
  }
  return error;
}

RetainPtr<const CPDF_Dictionary> CPDF_Document::GetPagesDict() const {
  const CPDF_Dictionary* pRoot = GetRoot();
  return pRoot ? pRoot->GetDictFor("Pages") : nullptr;
}

RetainPtr<CPDF_Dictionary> CPDF_Document::GetMutablePagesDict() {
  return pdfium::WrapRetain(
      const_cast<CPDF_Dictionary*>(this->GetPagesDict().Get()));
}

bool CPDF_Document::IsPageLoaded(int iPage) const {
  return !!GetPageObjNumAt(iPage);
}

RetainPtr<const CPDF_Dictionary> CPDF_Document::GetPageDictionary(int iPage) {
  if (iPage < 0 || static_cast<size_t>(iPage) >= GetPageListSize()) {
    return nullptr;
  }

  const uint32_t objnum = GetPageObjNumAt(iPage);
  if (objnum) {
    RetainPtr<CPDF_Dictionary> result =
        ToDictionary(GetOrParseIndirectObject(objnum));
    if (result) {
      return result;
    }
  }

  RetainPtr<CPDF_Dictionary> pPages = GetMutablePagesDict();
  if (!pPages) {
    return nullptr;
  }

  if (tree_traversal_.empty()) {
    ResetTraversal();
    tree_traversal_.emplace_back(std::move(pPages), 0);
  }
  int nPagesToGo = iPage - next_page_to_traverse_ + 1;
  RetainPtr<CPDF_Dictionary> pPage = TraversePDFPages(iPage, &nPagesToGo, 0);
  next_page_to_traverse_ = iPage + 1;
  return pPage;
}

RetainPtr<CPDF_Dictionary> CPDF_Document::GetMutablePageDictionary(int iPage) {
  return pdfium::WrapRetain(
      const_cast<CPDF_Dictionary*>(GetPageDictionary(iPage).Get()));
}

void CPDF_Document::SetPageObjNum(int iPage, uint32_t objNum) {
  SetPageObjNumAt(iPage, objNum);
}

JBig2_DocumentContext* CPDF_Document::GetOrCreateCodecContext() {
  if (!codec_context_) {
    codec_context_ = std::make_unique<JBig2_DocumentContext>();
  }
  return codec_context_.get();
}

RetainPtr<CPDF_Stream> CPDF_Document::CreateModifiedAPStream(
    RetainPtr<CPDF_Dictionary> dict) {
  auto stream = NewIndirect<CPDF_Stream>(std::move(dict));
  modified_apstream_ids_.insert(stream->GetObjNum());
  return stream;
}

bool CPDF_Document::IsModifiedAPStream(const CPDF_Stream* stream) const {
  return stream &&
         pdfium::Contains(modified_apstream_ids_, stream->GetObjNum());
}

void CPDF_Document::SetPendingSecurity(PendingSecurity pending) {
  pending_security_ = std::move(pending);
}

const CPDF_Document::PendingSecurity* CPDF_Document::GetPendingSecurity()
    const {
  return pending_security_ ? &pending_security_.value() : nullptr;
}

void CPDF_Document::ClearPendingSecurity() {
  pending_security_.reset();
}

RetainPtr<CPDF_Object> CPDF_Document::FindPromotedObject(
    uint32_t objnum) const {
  return nullptr;
}

bool CPDF_Document::IsObjectPromoted(uint32_t objnum) const {
  return !!FindPromotedObject(objnum);
}

RetainPtr<const CPDF_Object> CPDF_Document::GetBaseTwin(
    uint32_t objnum) const {
  return GetLoadedTwin(objnum);
}

bool CPDF_Document::SharesBackingStorageWith(const CPDF_Stream* stream) const {
  CPDF_Parser* parser = GetParser();
  RetainPtr<IFX_SeekableReadStream> file = parser ? parser->GetFileAccess() : nullptr;
  RetainPtr<IFX_SeekableReadStream> view = stream ? stream->BackingView() : nullptr;
  return file && view && view->GetUnderlyingStream() == file->GetUnderlyingStream();
}

RetainPtr<const CPDF_Object> CPDF_Document::GetLoadedTwin(
    uint32_t objnum) const {
  // The object as it is in the loaded bytes, parsed afresh: the parser
  // hands out a fresh root object, while retaining decoded object streams in
  // its normal cache. An in-place edit never reaches the twin. Null if the file
  // does not carry the object (created in memory).
  CPDF_Parser* parser = GetParser();
  if (!parser || objnum == 0 || !parser->IsValidObjectNumber(objnum) ||
      parser->IsObjectFree(objnum)) {
    return nullptr;
  }
  return parser->ParseIndirectObject(objnum);
}

uint64_t CPDF_Document::GetOverlayEpoch() const {
  return 0;
}

bool CPDF_Document::IsLayerDocument() const {
  return false;
}

const CPDF_BaseDocument* CPDF_Document::GetBaseDocumentForViewScope() const {
  return nullptr;
}

FX_FILESIZE CPDF_Document::GetLayerAppendBaseOffset() const {
  CPDF_Parser* parser = GetParser();
  return parser ? parser->GetDocumentSize() : 0;
}

int CPDF_Document::GetPageIndex(uint32_t objnum) {
  uint32_t skip_count = 0;
  bool bSkipped = false;
  for (size_t i = 0; i < GetPageListSize(); ++i) {
    if (GetPageObjNumAt(i) == objnum) {
      return pdfium::checked_cast<int>(i);
    }

    if (!bSkipped && GetPageObjNumAt(i) == 0) {
      skip_count = pdfium::checked_cast<uint32_t>(i);
      bSkipped = true;
    }
  }
  RetainPtr<const CPDF_Dictionary> pPages = GetPagesDict();
  if (!pPages) {
    return -1;
  }

  int start_index = 0;
  int found_index = FindPageIndex(pPages, &skip_count, objnum, &start_index, 0);

  // Corrupt page tree may yield out-of-range results.
  if (found_index < 0 ||
      static_cast<size_t>(found_index) >= GetPageListSize()) {
    return -1;
  }

  // Only update |page_list_| when |objnum| points to a /Page object.
  if (IsValidPageObject(GetOrParseIndirectObject(objnum).Get())) {
    SetPageObjNumAt(found_index, objnum);
  }
  return found_index;
}

int CPDF_Document::GetPageCount() const {
  return pdfium::checked_cast<int>(GetPageListSize());
}

int CPDF_Document::RetrievePageCount() {
  RetainPtr<CPDF_Dictionary> pPages = GetMutablePagesDict();
  if (!pPages) {
    return 0;
  }

  if (!pPages->KeyExist("Kids")) {
    return 1;
  }

  std::set<RetainPtr<CPDF_Dictionary>> visited_pages = {pPages};
  return CountPages(std::move(pPages), &visited_pages).value_or(0);
}

uint32_t CPDF_Document::GetUserPermissions(bool get_owner_perms) const {
  CPDF_Parser* parser = GetParser();
  return parser ? parser->GetPermissions(get_owner_perms) : 0;
}

RetainPtr<CPDF_StreamAcc> CPDF_Document::GetFontFileStreamAcc(
    RetainPtr<const CPDF_Stream> font_stream) {
  return doc_page_->GetFontFileStreamAcc(std::move(font_stream));
}

void CPDF_Document::MaybePurgeFontFileStreamAcc(
    RetainPtr<CPDF_StreamAcc>&& stream_acc) {
  doc_page_->MaybePurgeFontFileStreamAcc(std::move(stream_acc));
}

void CPDF_Document::MaybePurgeImage(uint32_t objnum) {
  doc_page_->MaybePurgeImage(objnum);
}

void CPDF_Document::CreateNewDoc() {
  DCHECK(!GetRoot());
  DCHECK(!GetInfo());
  RetainPtr<CPDF_Dictionary> root = NewIndirect<CPDF_Dictionary>();
  SetCachedRootDict(root);
  root->SetNewFor<CPDF_Name>("Type", "Catalog");

  auto pPages = NewIndirect<CPDF_Dictionary>();
  pPages->SetNewFor<CPDF_Name>("Type", "Pages");
  pPages->SetNewFor<CPDF_Number>("Count", 0);
  pPages->SetNewFor<CPDF_Array>("Kids");
  root->SetNewFor<CPDF_Reference>("Pages", this, pPages->GetObjNum());
  SetCachedInfoDict(NewIndirect<CPDF_Dictionary>());
}

RetainPtr<CPDF_Dictionary> CPDF_Document::CreateNewPage(int iPage) {
  auto dict = NewIndirect<CPDF_Dictionary>();
  dict->SetNewFor<CPDF_Name>("Type", "Page");
  uint32_t dwObjNum = dict->GetObjNum();
  if (!InsertNewPage(iPage, dict)) {
    DeleteIndirectObject(dwObjNum);
    return nullptr;
  }
  return dict;
}

bool CPDF_Document::InsertDeletePDFPage(
    RetainPtr<CPDF_Dictionary> pages_dict,
    int pages_to_go,
    RetainPtr<CPDF_Dictionary> page_dict,
    bool is_insert,
    std::set<RetainPtr<CPDF_Dictionary>>* visited) {
  RetainPtr<CPDF_Array> kids_list = pages_dict->GetMutableArrayFor("Kids");
  if (!kids_list) {
    return false;
  }

  for (size_t i = 0; i < kids_list->size(); i++) {
    // EmbedPDF layer documents must not promote a base /Page object merely to
    // decide whether traversal should skip it or remove its reference from
    // /Kids. Use const access first, and only request a mutable object for the
    // branch /Pages node that will be structurally edited.
    RetainPtr<const CPDF_Dictionary> const_kid_dict = kids_list->GetDictAt(i);
    if (!const_kid_dict) {
      return false;
    }

    NodeType kid_type = GetNodeTypeForTraversal(const_kid_dict.Get());
    if (kid_type == NodeType::kLeaf) {
      if (pages_to_go != 0) {
        pages_to_go--;
        continue;
      }
      if (is_insert) {
        kids_list->InsertNewAt<CPDF_Reference>(i, this, page_dict->GetObjNum());
        page_dict->SetNewFor<CPDF_Reference>("Parent", this,
                                             pages_dict->GetObjNum());
      } else {
        kids_list->RemoveAt(i);
      }
      pages_dict->SetNewFor<CPDF_Number>(
          "Count", pages_dict->GetIntegerFor("Count") + (is_insert ? 1 : -1));
      ResetTraversal();
      break;
    }

    CHECK_EQ(kid_type, NodeType::kBranch);
    int page_count = const_kid_dict->GetIntegerFor("Count");
    if (pages_to_go >= page_count) {
      pages_to_go -= page_count;
      continue;
    }

    RetainPtr<CPDF_Dictionary> kid_dict = kids_list->GetMutableDictAt(i);
    if (!kid_dict) {
      return false;
    }
    if (pdfium::Contains(*visited, kid_dict)) {
      return false;
    }

    ScopedSetInsertion insertion(visited, kid_dict);
    if (!InsertDeletePDFPage(std::move(kid_dict), pages_to_go, page_dict,
                             is_insert, visited)) {
      return false;
    }
    pages_dict->SetNewFor<CPDF_Number>(
        "Count", pages_dict->GetIntegerFor("Count") + (is_insert ? 1 : -1));
    break;
  }
  return true;
}

bool CPDF_Document::InsertNewPage(int iPage,
                                  RetainPtr<CPDF_Dictionary> pPageDict) {
  RetainPtr<CPDF_Dictionary> pRoot = GetMutableRoot();
  if (!pRoot) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pPages = pRoot->GetMutableDictFor("Pages");
  if (!pPages) {
    return false;
  }

  int nPages = GetPageCount();
  if (iPage < 0 || iPage > nPages) {
    return false;
  }

  if (iPage == nPages) {
    RetainPtr<CPDF_Array> pPagesList = pPages->GetOrCreateArrayFor("Kids");
    pPagesList->AppendNew<CPDF_Reference>(this, pPageDict->GetObjNum());
    pPages->SetNewFor<CPDF_Number>("Count", nPages + 1);
    pPageDict->SetNewFor<CPDF_Reference>("Parent", this, pPages->GetObjNum());
    ResetTraversal();
  } else {
    std::set<RetainPtr<CPDF_Dictionary>> stack = {pPages};
    if (!InsertDeletePDFPage(std::move(pPages), iPage, pPageDict, true,
                             &stack)) {
      return false;
    }
  }
  InsertPageObjNum(iPage, pPageDict->GetObjNum());
  return true;
}

RetainPtr<CPDF_Dictionary> CPDF_Document::GetInfo() {
  if (info_dict_) {
    return info_dict_;
  }

  CPDF_Parser* parser = GetParser();
  if (!parser) {
    return nullptr;
  }

  uint32_t info_obj_num = parser->GetInfoObjNum();
  if (info_obj_num == 0) {
    return nullptr;
  }

  auto ref = pdfium::MakeRetain<CPDF_Reference>(this, info_obj_num);
  SetCachedInfoDict(ToDictionary(ref->GetMutableDirect()));
  return info_dict_;
}

RetainPtr<CPDF_Dictionary> CPDF_Document::GetMutableInfo() {
  return GetInfo();
}

uint32_t CPDF_Document::GetInfoObjectNumber() const {
  if (info_dict_) {
    return info_dict_->GetObjNum();
  }
  CPDF_Parser* parser = GetParser();
  return parser ? parser->GetInfoObjNum() : 0;
}

RetainPtr<CPDF_Dictionary> CPDF_Document::GetOrCreateInfo() {
  // If the document already has an Info object, return the mutable view of it.
  // Layer documents override GetMutableInfo() to promote the base /Info into
  // the layer overlay before returning it.
  if (RetainPtr<CPDF_Dictionary> existing = GetMutableInfo()) {
    return existing;
  }

  // No Info present: create a new indirect dictionary and cache it.
  SetCachedInfoDict(NewIndirect<CPDF_Dictionary>());
  return info_dict_;
}

RetainPtr<const CPDF_Array> CPDF_Document::GetFileIdentifier() const {
  CPDF_Parser* parser = GetParser();
  return parser ? parser->GetIDArray() : nullptr;
}

uint32_t CPDF_Document::DeletePage(int iPage) {
  RetainPtr<CPDF_Dictionary> pRoot = GetMutableRoot();
  if (!pRoot) {
    return 0;
  }

  // Use the mutable catalog path so layer documents promote the page tree
  // before editing /Kids and /Count. A const-cast mutable page tree would leave
  // no layer overlay object to save.
  RetainPtr<CPDF_Dictionary> pPages = pRoot->GetMutableDictFor("Pages");
  if (!pPages) {
    return 0;
  }

  int nPages = pPages->GetIntegerFor("Count");
  if (iPage < 0 || iPage >= nPages) {
    return 0;
  }

  RetainPtr<const CPDF_Dictionary> page_dict = GetPageDictionary(iPage);
  if (!page_dict) {
    return 0;
  }

  std::set<RetainPtr<CPDF_Dictionary>> stack = {pPages};
  if (!InsertDeletePDFPage(std::move(pPages), iPage, nullptr, false, &stack)) {
    return 0;
  }

  ErasePageObjNum(iPage);
  return page_dict->GetObjNum();
}

void CPDF_Document::SetPageToNullObject(uint32_t page_obj_num) {
  if (!page_obj_num || GetPageListSize() == 0) {
    return;
  }

  // Load all pages so `page_list_` has all the object numbers.
  for (size_t i = 0; i < GetPageListSize(); ++i) {
    GetPageDictionary(pdfium::checked_cast<int>(i));
  }

  for (size_t i = 0; i < GetPageListSize(); ++i) {
    if (GetPageObjNumAt(i) == page_obj_num) {
      return;
    }
  }

  if (!ShouldReplaceDeletedPageWithNull(page_obj_num)) {
    return;
  }

  // If `page_dict` is no longer in the page tree, replace it with an object of
  // type null.
  //
  // Delete the object first from this container, so the conditional in the
  // replacement call always evaluates to true.
  DeleteIndirectObject(page_obj_num);
  const bool replaced = ReplaceIndirectObjectIfHigherGeneration(
      page_obj_num, pdfium::MakeRetain<CPDF_Null>());
  CHECK(replaced);
}

bool CPDF_Document::ShouldReplaceDeletedPageWithNull(
    uint32_t page_obj_num) const {
  return true;
}

void CPDF_Document::SetRootForTesting(RetainPtr<CPDF_Dictionary> root) {
  SetCachedRootDict(std::move(root));
}

bool CPDF_Document::MovePages(pdfium::span<const int> page_indices,
                              int dest_page_index) {
  const CPDF_Dictionary* pages = GetPagesDict();
  const int num_pages_signed = pages ? pages->GetIntegerFor("Count") : 0;
  if (num_pages_signed <= 0) {
    return false;
  }
  const size_t num_pages = num_pages_signed;

  // Check the number of pages is in range.
  if (page_indices.empty() || page_indices.size() > num_pages) {
    return false;
  }

  // Check that destination page index is in range.
  if (dest_page_index < 0 ||
      static_cast<size_t>(dest_page_index) > num_pages - page_indices.size()) {
    return false;
  }

  // Check for if XFA is enabled.
  Extension* extension = GetExtension();
  if (extension && extension->ContainsExtensionFullForm()) {
    // Don't manipulate XFA-full PDFs.
    return false;
  }

  // Check for duplicate and out-of-range page indices
  std::set<int> unique_page_indices;
  // Store the pages that need to be moved. They'll be deleted then reinserted.
  std::vector<RetainPtr<CPDF_Dictionary>> pages_to_move;
  pages_to_move.reserve(page_indices.size());
  // Store the page indices that will be deleted (and moved).
  std::vector<int> page_indices_to_delete;
  page_indices_to_delete.reserve(page_indices.size());
  for (const int page_index : page_indices) {
    bool inserted = unique_page_indices.insert(page_index).second;
    if (!inserted) {
      // Duplicate page index found
      return false;
    }
    RetainPtr<CPDF_Dictionary> page = GetMutablePageDictionary(page_index);
    if (!page) {
      // Page not found, index might be out of range.
      return false;
    }
    pages_to_move.push_back(std::move(page));
    page_indices_to_delete.push_back(page_index);
  }

  // Sort the page indices to be deleted in descending order.
  std::sort(page_indices_to_delete.begin(), page_indices_to_delete.end(),
            std::greater<int>());
  // Delete the pages in descending order.
  if (extension) {
    for (int page_index : page_indices_to_delete) {
      extension->DeletePage(page_index);
    }
  } else {
    for (int page_index : page_indices_to_delete) {
      DeletePage(page_index);
    }
  }

  // Insert the deleted pages back into the document at the destination page
  // index.
  for (size_t i = 0; i < pages_to_move.size(); ++i) {
    if (!InsertNewPage(i + dest_page_index, pages_to_move[i])) {
      // Fail in an indeterminate state.
      return false;
    }
  }

  if (extension) {
    extension->PagesInserted(dest_page_index, pages_to_move.size());
  }

  return true;
}

void CPDF_Document::ResizePageListForTesting(size_t size) {
  ResizePageList(size);
}

uint32_t CPDF_Document::GetPageObjNumAt(size_t index) const {
  return page_list_[index];
}

void CPDF_Document::SetPageObjNumAt(size_t index, uint32_t objnum) {
  page_list_[index] = objnum;
}

void CPDF_Document::InsertPageObjNum(size_t index, uint32_t objnum) {
  page_list_.insert(page_list_.begin() + index, objnum);
}

void CPDF_Document::ErasePageObjNum(size_t index) {
  page_list_.erase(page_list_.begin() + index);
}

void CPDF_Document::ResizePageList(size_t size) {
  page_list_.resize(size);
}

size_t CPDF_Document::GetPageListSize() const {
  return page_list_.size();
}

void CPDF_Document::SetCachedRootDict(RetainPtr<CPDF_Dictionary> root) {
  root_dict_ = std::move(root);
}

void CPDF_Document::InvalidateCachedRootDict() {
  root_dict_.Reset();
}

void CPDF_Document::SetCachedInfoDict(RetainPtr<CPDF_Dictionary> info) {
  info_dict_ = std::move(info);
}

void CPDF_Document::InvalidateCachedInfoDict() {
  info_dict_.Reset();
}

CPDF_Document::StockFontClearer::StockFontClearer(
    CPDF_Document::PageDataIface* pPageData)
    : page_data_(pPageData) {}

CPDF_Document::StockFontClearer::~StockFontClearer() {
  page_data_->ClearStockFont();
}

CPDF_Document::PageDataIface::PageDataIface() = default;

CPDF_Document::PageDataIface::~PageDataIface() = default;

CPDF_Document::RenderDataIface::RenderDataIface() = default;

CPDF_Document::RenderDataIface::~RenderDataIface() = default;
