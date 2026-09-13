// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_signature.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "build/build_config.h"
#include "constants/form_fields.h"
#include "constants/form_flags.h"
#include "core/fpdfapi/edit/cpdf_creator.h"
#include "core/fdrm/fx_crypt_sha.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_base_document.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_concat_read_stream.h"
#include "core/fxcrt/cfx_fileaccess_stream.h"
#include "core/fpdfapi/parser/cpdf_layer_document.h"
#include "fpdfsdk/cpdfsdk_customaccess.h"
#include "fpdfsdk/cpdfsdk_filewriteadapter.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fpdfapi/render/cpdf_docrenderdata.h"
#include "core/fpdfdoc/cpdf_formcontrol.h"
#include "core/fpdfdoc/cpdf_formfield.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/span_util.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxcrt/fx_memory_wrappers.h"
#include "core/fxcrt/mask.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_form_helpers.h"
#include "fpdfsdk/epdf_revision_view.h"
#include "public/fpdf_save.h"

namespace {

// ---------------------------------------------------------------------------
// Revisions.
// ---------------------------------------------------------------------------

using epdf::RevisionInfo;

bool IsPdfWhitespace(uint8_t ch) {
  return ch == 0x00 || ch == 0x09 || ch == 0x0a || ch == 0x0c || ch == 0x0d ||
         ch == 0x20;
}

// Reads the integer that follows the last "startxref" keyword in the bytes
// immediately preceding |end| (file offset). Returns the header-relative
// value the parser would use, or nullopt when there is no such keyword.
std::optional<FX_FILESIZE> ReadStartXRefBefore(IFX_SeekableReadStream* file,
                                               FX_FILESIZE end) {
  static constexpr FX_FILESIZE kWindow = 64;
  const FX_FILESIZE start = std::max<FX_FILESIZE>(0, end - kWindow);
  const size_t size = static_cast<size_t>(end - start);
  if (size == 0) {
    return std::nullopt;
  }
  DataVector<uint8_t> window(size);
  if (!file->ReadBlockAtOffset(window, start)) {
    return std::nullopt;
  }
  static constexpr char kKeyword[] = "startxref";
  static constexpr size_t kKeywordLen = 9;
  if (size < kKeywordLen) {
    return std::nullopt;
  }
  std::optional<size_t> keyword_pos;
  for (size_t i = size - kKeywordLen + 1; i-- > 0;) {
    if (memcmp(window.data() + i, kKeyword, kKeywordLen) == 0) {
      keyword_pos = i;
      break;
    }
  }
  if (!keyword_pos.has_value()) {
    return std::nullopt;
  }
  size_t i = keyword_pos.value() + kKeywordLen;
  while (i < size && IsPdfWhitespace(window[i])) {
    ++i;
  }
  if (i >= size || !isdigit(window[i])) {
    return std::nullopt;
  }
  FX_FILESIZE value = 0;
  while (i < size && isdigit(window[i])) {
    value = value * 10 + (window[i] - '0');
    if (value > (FX_FILESIZE{1} << 40)) {
      return std::nullopt;
    }
    ++i;
  }
  return value;
}

std::optional<std::vector<RevisionInfo>> ComputeRevisions(CPDF_Parser* parser);

// The revisions of |view|'s bytes, computed once per view and cached on it
// (the bytes never change). A copy: callers index and iterate it locally.
std::optional<std::vector<RevisionInfo>> RevisionsOf(epdf::RevisionView* view) {
  if (!view) {
    return std::nullopt;
  }
  if (!view->has_revisions()) {
    CPDF_Parser* parser = view->parser();
    view->set_revisions(ComputeRevisions(parser));
  }
  return view->revisions();
}

// The revisions of |parser|'s file, oldest first, or nullopt when the chain
// cannot be trusted (see EPDFDoc_GetRevisionCount).
std::optional<std::vector<RevisionInfo>> ComputeRevisions(CPDF_Parser* parser) {
  if (!parser || parser->xref_table_rebuilt()) {
    return std::nullopt;
  }
  const std::vector<CPDF_Parser::CrossRefSection>& sections =
      parser->GetCrossRefSections();
  if (sections.empty()) {
    return std::nullopt;
  }
  RetainPtr<IFX_SeekableReadStream> file = parser->GetFileAccess();
  if (!file) {
    return std::nullopt;
  }
  const FX_FILESIZE header_offset = parser->GetFileHeaderOffset();
  const FX_FILESIZE file_size = file->GetSize();

  std::map<FX_FILESIZE, size_t> section_by_offset;
  for (size_t i = 0; i < sections.size(); ++i) {
    section_by_offset[sections[i].offset] = i;
  }

  // Every (%%EOF end, section it closes) pair, in file order.
  std::vector<RevisionInfo> revisions;
  bool last_section_closed = false;
  for (unsigned int end_rel : parser->GetCachedTrailerEnds()) {
    const FX_FILESIZE end = static_cast<FX_FILESIZE>(end_rel) + header_offset;
    if (end > file_size) {
      break;
    }
    std::optional<FX_FILESIZE> startxref = ReadStartXRefBefore(file.Get(), end);
    if (!startxref.has_value()) {
      continue;  // a stray "%%EOF" without a startxref: not a boundary
    }
    auto it = section_by_offset.find(startxref.value());
    if (it == section_by_offset.end()) {
      continue;  // points outside the chain
    }
    // The prefix is self-contained only if every section reachable from the
    // closing one lies below the boundary.
    bool contained = true;
    std::set<size_t> seen;
    size_t cursor = it->second;
    while (true) {
      if (!seen.insert(cursor).second) {
        contained = false;
        break;
      }
      const CPDF_Parser::CrossRefSection& s = sections[cursor];
      if (s.offset >= end_rel || s.hybrid_stream_offset >= end_rel) {
        contained = false;
        break;
      }
      if (s.prev_offset == 0) {
        break;
      }
      auto prev_it = section_by_offset.find(s.prev_offset);
      if (prev_it == section_by_offset.end()) {
        contained = false;
        break;
      }
      cursor = prev_it->second;
    }
    if (!contained) {
      continue;
    }
    // Later boundaries closing the same section (a linearized file's second
    // %%EOF) replace the earlier one rather than adding a revision.
    if (!revisions.empty() &&
        revisions.back().xref_offset == startxref.value() + header_offset) {
      revisions.back().end = end;
    } else {
      revisions.push_back({end, startxref.value() + header_offset});
    }
    last_section_closed = startxref.value() == parser->GetLastXRefOffset();
  }
  if (revisions.empty() || !last_section_closed) {
    return std::nullopt;
  }
  return revisions;
}

// A read-only view of the first |size| bytes of another stream.
class ClampedReadStream final : public IFX_SeekableReadStream {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  FX_FILESIZE GetSize() override { return size_; }

  bool ReadBlockAtOffset(pdfium::span<uint8_t> buffer,
                         FX_FILESIZE offset) override {
    if (offset < 0 || offset > size_ ||
        static_cast<FX_FILESIZE>(buffer.size()) > size_ - offset) {
      return false;
    }
    if (buffer.empty()) {
      return true;
    }
    return inner_->ReadBlockAtOffset(buffer, offset);
  }

  // A clamp changes no byte at any offset: it presents the wrapped stream.
  IFX_SeekableReadStream* GetUnderlyingStream() override {
    return inner_->GetUnderlyingStream();
  }
  bool IsSelfContained() const override { return inner_->IsSelfContained(); }

 private:
  ClampedReadStream(RetainPtr<IFX_SeekableReadStream> inner, FX_FILESIZE size)
      : inner_(std::move(inner)), size_(size) {}
  ~ClampedReadStream() override = default;

  RetainPtr<IFX_SeekableReadStream> inner_;
  const FX_FILESIZE size_;
};

// Bytes owned by the stream (a copy of the caller's buffer).
class OwnedBytesReadStream final : public IFX_SeekableReadStream {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  FX_FILESIZE GetSize() override {
    return static_cast<FX_FILESIZE>(bytes_.size());
  }
  bool IsSelfContained() const override { return true; }  // owns |bytes_|

  bool ReadBlockAtOffset(pdfium::span<uint8_t> buffer,
                         FX_FILESIZE offset) override {
    if (offset < 0 || static_cast<size_t>(offset) > bytes_.size() ||
        buffer.size() > bytes_.size() - static_cast<size_t>(offset)) {
      return false;
    }
    fxcrt::Copy(pdfium::span(bytes_).subspan(static_cast<size_t>(offset),
                                             buffer.size()),
                buffer);
    return true;
  }

 private:
  explicit OwnedBytesReadStream(DataVector<uint8_t> bytes)
      : bytes_(std::move(bytes)) {}
  ~OwnedBytesReadStream() override = default;

  const DataVector<uint8_t> bytes_;
};

// Opens bytes [0, end) of |parser|'s file as an independent document with
// the same password. Shared by EPDFDoc_OpenRevision and the coverage check.
std::unique_ptr<CPDF_Document> OpenPrefixDocument(CPDF_Parser* parser,
                                                  FX_FILESIZE end) {
  RetainPtr<IFX_SeekableReadStream> file = parser->GetFileAccess();
  if (!file) {
    return nullptr;
  }
  auto clamped = pdfium::MakeRetain<ClampedReadStream>(std::move(file), end);
  auto prefix = std::make_unique<CPDF_Document>(
      std::make_unique<CPDF_DocRenderData>(),
      std::make_unique<CPDF_DocPageData>());
  if (prefix->LoadDoc(std::move(clamped), parser->GetPassword()) !=
      CPDF_Parser::SUCCESS) {
    return nullptr;
  }
  return prefix;
}

// ---------------------------------------------------------------------------
// Signature model.
// ---------------------------------------------------------------------------

struct SignatureRecord {
  uint32_t field_objnum = 0;
  uint32_t widget_objnum = 0;
  uint32_t page_objnum = 0;
  uint32_t value_objnum = 0;
  WideString fqn;
  bool is_signed = false;
  int kind = EPDF_SIG_KIND_SIGNATURE;
  bool has_byte_range = false;
  std::array<uint64_t, 4> byte_range = {0, 0, 0, 0};
  int coverage = EPDF_SIG_COVERAGE_MALFORMED;
  int revision_index = -1;
  DataVector<uint8_t> contents_der;
  std::map<int, WideString> strings;  // EPDF_SIG_STRING_* -> text
  int docmdp_permission = 0;
  bool catalog_certification = false;
  int fieldmdp_action = EPDF_SIG_FIELD_ACTION_NONE;
  std::vector<WideString> fieldmdp_fields;
  int lock_action = EPDF_SIG_FIELD_ACTION_NONE;
  int lock_permission = 0;
  std::vector<WideString> lock_fields;
  bool has_seed_value = false;
  uint32_t sv_required = 0;
  uint32_t sv_present = 0;
  int sv_version = 0;
  int sv_mdp = 0;  // /SV /MDP /P; 0 with EPDF_SIG_SV_MDP present = approval only
  WideString sv_filter;
  std::vector<WideString> sv_subfilters;
  std::vector<WideString> sv_digest_methods;
  std::vector<WideString> sv_reasons;
};

struct SignatureModel {
  std::vector<SignatureRecord> records;
  std::map<uint32_t, int> index_by_field_objnum;
  bool chain_valid = false;
};

std::unique_ptr<SignatureModel> BuildModel(CPDF_Document* doc);

SignatureModel* ModelFromHandle(EPDF_SIGNATURE_MODEL model) {
  return reinterpret_cast<SignatureModel*>(model);
}

EPDF_SIGNATURE_MODEL HandleFromModel(SignatureModel* model) {
  return reinterpret_cast<EPDF_SIGNATURE_MODEL>(model);
}

const SignatureRecord* GetRecord(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureModel* m = ModelFromHandle(model);
  if (!m || index < 0 || index >= fxcrt::CollectionSize<int>(m->records)) {
    return nullptr;
  }
  return &m->records[index];
}

int ParseFieldAction(const CPDF_Dictionary* dict) {
  if (!dict) {
    return EPDF_SIG_FIELD_ACTION_NONE;
  }
  const ByteString action = dict->GetNameFor("Action");
  if (action == "All") {
    return EPDF_SIG_FIELD_ACTION_ALL;
  }
  if (action == "Include") {
    return EPDF_SIG_FIELD_ACTION_INCLUDE;
  }
  if (action == "Exclude") {
    return EPDF_SIG_FIELD_ACTION_EXCLUDE;
  }
  return EPDF_SIG_FIELD_ACTION_NONE;
}

std::vector<WideString> ReadTextArray(const CPDF_Dictionary* dict,
                                      ByteStringView key) {
  std::vector<WideString> out;
  if (!dict) {
    return out;
  }
  RetainPtr<const CPDF_Array> array = dict->GetArrayFor(key);
  if (!array) {
    return out;
  }
  for (size_t i = 0; i < array->size(); ++i) {
    RetainPtr<const CPDF_Object> item = array->GetDirectObjectAt(i);
    if (item && (item->IsString() || item->IsName())) {
      out.push_back(item->GetUnicodeText());
    }
  }
  return out;
}

int ParsePermission(const CPDF_Dictionary* dict,
                    ByteStringView key,
                    int default_value) {
  if (!dict || !dict->KeyExist(key)) {
    return default_value;
  }
  const int p = dict->GetIntegerFor(key);
  return (p >= 1 && p <= 3) ? p : 0;
}

// Reads a /V dictionary's /Reference array: DocMDP permission and FieldMDP
// action + fields (the first transform of each kind wins).
void ReadReference(const CPDF_Dictionary* value_dict, SignatureRecord* record) {
  RetainPtr<const CPDF_Array> references = value_dict->GetArrayFor("Reference");
  if (!references) {
    return;
  }
  bool saw_docmdp = false;
  bool saw_fieldmdp = false;
  for (size_t i = 0; i < references->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> ref = references->GetDictAt(i);
    if (!ref) {
      continue;
    }
    const ByteString method = ref->GetNameFor("TransformMethod");
    RetainPtr<const CPDF_Dictionary> params = ref->GetDictFor("TransformParams");
    if (method == "DocMDP" && !saw_docmdp) {
      saw_docmdp = true;
      // ISO 32000-2 table 257: /P defaults to 2.
      record->docmdp_permission =
          params ? ParsePermission(params.Get(), "P", 2) : 2;
    } else if (method == "FieldMDP" && !saw_fieldmdp) {
      saw_fieldmdp = true;
      record->fieldmdp_action = ParseFieldAction(params.Get());
      record->fieldmdp_fields = ReadTextArray(params.Get(), "Fields");
    }
  }
}

void ReadSeedValue(const CPDF_Dictionary* field_dict, SignatureRecord* record) {
  RetainPtr<const CPDF_Dictionary> sv = field_dict->GetDictFor("SV");
  if (!sv) {
    return;
  }
  record->has_seed_value = true;
  record->sv_required = static_cast<uint32_t>(sv->GetIntegerFor("Ff"));
  uint32_t present = 0;
  if (sv->KeyExist("Filter")) {
    present |= EPDF_SIG_SV_FILTER;
    record->sv_filter = sv->GetUnicodeTextFor("Filter");
  }
  if (sv->KeyExist("SubFilter")) {
    present |= EPDF_SIG_SV_SUBFILTER;
    record->sv_subfilters = ReadTextArray(sv.Get(), "SubFilter");
  }
  if (sv->KeyExist("V")) {
    present |= EPDF_SIG_SV_V;
    record->sv_version = sv->GetIntegerFor("V");
  }
  if (sv->KeyExist("Reasons")) {
    present |= EPDF_SIG_SV_REASONS;
    record->sv_reasons = ReadTextArray(sv.Get(), "Reasons");
  }
  if (sv->KeyExist("LegalAttestation")) {
    present |= EPDF_SIG_SV_LEGAL_ATTESTATION;
  }
  if (sv->KeyExist("AddRevInfo")) {
    present |= EPDF_SIG_SV_ADD_REV_INFO;
  }
  if (sv->KeyExist("DigestMethod")) {
    present |= EPDF_SIG_SV_DIGEST_METHOD;
    record->sv_digest_methods = ReadTextArray(sv.Get(), "DigestMethod");
  }
  if (sv->KeyExist("LockDocument")) {
    present |= EPDF_SIG_SV_LOCK_DOCUMENT;
  }
  if (sv->KeyExist("AppearanceFilter")) {
    present |= EPDF_SIG_SV_APPEARANCE_FILTER;
  }
  if (sv->KeyExist("Cert")) {
    present |= EPDF_SIG_SV_CERT;
  }
  if (sv->KeyExist("TimeStamp")) {
    present |= EPDF_SIG_SV_TIMESTAMP;
  }
  RetainPtr<const CPDF_Dictionary> mdp = sv->GetDictFor("MDP");
  if (mdp && mdp->KeyExist("P")) {
    // ISO 32000-2 table 240: an explicit /P 0 = no DocMDP (an approval
    // signature only); 1..3 = a certification signature with that
    // permission. An /MDP dictionary without /P, or with a value outside
    // 0..3, imposes no constraint and sets no flag.
    const int p_value = mdp->GetIntegerFor("P", -1);
    if (p_value >= 0 && p_value <= 3) {
      present |= EPDF_SIG_SV_MDP;
      record->sv_mdp = p_value;
    }
  }
  record->sv_present = present;
}

// Length of the ASN.1 object starting at |bytes|, definite or BER
// indefinite (walked child by child to its own end-of-contents octets, so
// nested indefinite objects and payloads ending in zero bytes are handled).
// nullopt when the encoding is malformed or does not fit. |depth| bounds
// nesting.
std::optional<size_t> Asn1ObjectLength(pdfium::span<const uint8_t> bytes,
                                       int depth) {
  static constexpr int kMaxDepth = 64;
  if (depth > kMaxDepth || bytes.size() < 2) {
    return std::nullopt;
  }
  size_t pos = 0;
  const uint8_t tag = bytes[pos++];
  const bool constructed = (tag & 0x20) != 0;
  if ((tag & 0x1f) == 0x1f) {  // high tag number form
    while (pos < bytes.size() && (bytes[pos] & 0x80)) {
      ++pos;
    }
    if (pos >= bytes.size()) {
      return std::nullopt;
    }
    ++pos;
  }
  if (pos >= bytes.size()) {
    return std::nullopt;
  }
  const uint8_t first = bytes[pos++];
  if (first == 0x80) {
    if (!constructed) {
      return std::nullopt;
    }
    // Children until end-of-contents (00 00).
    while (true) {
      if (pos + 2 > bytes.size()) {
        return std::nullopt;
      }
      if (bytes[pos] == 0x00 && bytes[pos + 1] == 0x00) {
        return pos + 2;
      }
      std::optional<size_t> child =
          Asn1ObjectLength(bytes.subspan(pos), depth + 1);
      if (!child.has_value()) {
        return std::nullopt;
      }
      pos += child.value();
    }
  }
  size_t length = first;
  if (first & 0x80) {
    const size_t count = first & 0x7f;
    if (count == 0 || count > 4 || bytes.size() < pos + count) {
      return std::nullopt;
    }
    length = 0;
    for (size_t i = 0; i < count; ++i) {
      length = (length << 8) | bytes[pos + i];
    }
    pos += count;
  }
  if (length > bytes.size() - pos) {
    return std::nullopt;
  }
  return pos + length;
}

// The outer /Contents object: a SEQUENCE (CMS ContentInfo), DER or BER.
std::optional<size_t> DerOuterLength(pdfium::span<const uint8_t> bytes) {
  if (bytes.size() < 2 || bytes[0] != 0x30) {
    return std::nullopt;
  }
  return Asn1ObjectLength(bytes, 0);
}

std::optional<uint8_t> HexNibble(uint8_t ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return std::nullopt;
}

// Decodes a hex string body (no delimiters); PDF whitespace is skipped, any
// other byte fails. An odd trailing nibble is padded with 0 like a parser.
std::optional<DataVector<uint8_t>> DecodeHexBody(
    pdfium::span<const uint8_t> body) {
  DataVector<uint8_t> out;
  out.reserve(body.size() / 2);
  std::optional<uint8_t> pending;
  for (uint8_t ch : body) {
    if (IsPdfWhitespace(ch)) {
      continue;
    }
    std::optional<uint8_t> nibble = HexNibble(ch);
    if (!nibble.has_value()) {
      return std::nullopt;
    }
    if (pending.has_value()) {
      out.push_back(static_cast<uint8_t>((pending.value() << 4) | nibble.value()));
      pending.reset();
    } else {
      pending = nibble;
    }
  }
  if (pending.has_value()) {
    out.push_back(static_cast<uint8_t>(pending.value() << 4));
  }
  return out;
}

// ---------------------------------------------------------------------------
// A small PDF syntax scanner over raw bytes: enough to find a dictionary
// entry's value by key at a given nesting level without being fooled by
// the same text inside a string. Used to locate a signature's /Contents and
// /ByteRange physically, both when sealing a candidate and when judging a
// signed file's coverage.
// ---------------------------------------------------------------------------

namespace pdfscan {

bool IsDelimiter(uint8_t ch) {
  return ch == '(' || ch == ')' || ch == '<' || ch == '>' || ch == '[' ||
         ch == ']' || ch == '{' || ch == '}' || ch == '/' || ch == '%';
}

bool IsRegular(uint8_t ch) {
  return !IsPdfWhitespace(ch) && !IsDelimiter(ch);
}

size_t SkipWhitespace(pdfium::span<const uint8_t> in, size_t pos) {
  while (pos < in.size()) {
    if (IsPdfWhitespace(in[pos])) {
      ++pos;
    } else if (in[pos] == '%') {
      while (pos < in.size() && in[pos] != '\r' && in[pos] != '\n') {
        ++pos;
      }
    } else {
      break;
    }
  }
  return pos;
}

// End (exclusive) of the value starting at |pos|, or nullopt.
std::optional<size_t> ScanValue(pdfium::span<const uint8_t> in,
                                size_t pos,
                                int depth);

std::optional<size_t> ScanLiteralString(pdfium::span<const uint8_t> in,
                                        size_t pos) {
  int nesting = 0;
  while (pos < in.size()) {
    const uint8_t ch = in[pos++];
    if (ch == '\\') {
      ++pos;  // whatever follows is escaped
    } else if (ch == '(') {
      ++nesting;
    } else if (ch == ')') {
      if (--nesting == 0) {
        return pos;
      }
    }
  }
  return std::nullopt;
}

std::optional<size_t> ScanHexString(pdfium::span<const uint8_t> in,
                                    size_t pos) {
  ++pos;  // '<'
  while (pos < in.size()) {
    if (in[pos] == '>') {
      return pos + 1;
    }
    ++pos;
  }
  return std::nullopt;
}

size_t ScanRegular(pdfium::span<const uint8_t> in, size_t pos) {
  while (pos < in.size() && IsRegular(in[pos])) {
    ++pos;
  }
  return pos;
}

bool IsDigits(pdfium::span<const uint8_t> in, size_t from, size_t to) {
  if (from >= to) {
    return false;
  }
  for (size_t i = from; i < to; ++i) {
    if (!isdigit(in[i])) {
      return false;
    }
  }
  return true;
}

std::optional<size_t> ScanDictionaryBody(pdfium::span<const uint8_t> in,
                                         size_t pos,
                                         int depth,
                                         ByteStringView wanted,
                                         size_t* value_start,
                                         size_t* value_end);

std::optional<size_t> ScanValue(pdfium::span<const uint8_t> in,
                                size_t pos,
                                int depth) {
  static constexpr int kMaxDepth = 64;
  if (depth > kMaxDepth || pos >= in.size()) {
    return std::nullopt;
  }
  const uint8_t ch = in[pos];
  if (ch == '(') {
    return ScanLiteralString(in, pos);
  }
  if (ch == '<') {
    if (pos + 1 < in.size() && in[pos + 1] == '<') {
      return ScanDictionaryBody(in, pos, depth + 1, ByteStringView(), nullptr,
                                nullptr);
    }
    return ScanHexString(in, pos);
  }
  if (ch == '[') {
    ++pos;
    while (true) {
      pos = SkipWhitespace(in, pos);
      if (pos >= in.size()) {
        return std::nullopt;
      }
      if (in[pos] == ']') {
        return pos + 1;
      }
      std::optional<size_t> end = ScanValue(in, pos, depth + 1);
      if (!end.has_value()) {
        return std::nullopt;
      }
      pos = end.value();
    }
  }
  if (ch == '/') {
    return ScanRegular(in, pos + 1);
  }
  if (IsDelimiter(ch)) {
    return std::nullopt;
  }
  const size_t token_end = ScanRegular(in, pos);
  if (token_end == pos) {
    return std::nullopt;
  }
  // "n g R": a reference is one value.
  if (IsDigits(in, pos, token_end)) {
    size_t p = SkipWhitespace(in, token_end);
    const size_t gen_end = ScanRegular(in, p);
    if (IsDigits(in, p, gen_end)) {
      p = SkipWhitespace(in, gen_end);
      if (p < in.size() && in[p] == 'R' &&
          (p + 1 >= in.size() || !IsRegular(in[p + 1]))) {
        return p + 1;
      }
    }
  }
  return token_end;
}

// Scans "<< ... >>" starting at |pos|; when |wanted| is non-empty and a
// top-level key decodes to it, its value span is reported. Returns the end
// of the dictionary, or nullopt when it does not scan or repeats a key:
// keys shall be unique (ISO 32000-2 7.3.7), and a parser that keeps the
// last occurrence would otherwise disagree with a scan that keeps the first.
std::optional<size_t> ScanDictionaryBody(pdfium::span<const uint8_t> in,
                                         size_t pos,
                                         int depth,
                                         ByteStringView wanted,
                                         size_t* value_start,
                                         size_t* value_end) {
  if (pos + 2 > in.size() || in[pos] != '<' || in[pos + 1] != '<') {
    return std::nullopt;
  }
  pos += 2;
  std::set<ByteString> seen_keys;
  while (true) {
    pos = SkipWhitespace(in, pos);
    if (pos + 2 <= in.size() && in[pos] == '>' && in[pos + 1] == '>') {
      return pos + 2;
    }
    if (pos >= in.size() || in[pos] != '/') {
      return std::nullopt;
    }
    const size_t name_end = ScanRegular(in, pos + 1);
    const ByteString key = PDF_NameDecode(ByteStringView(
        in.subspan(pos + 1, name_end - pos - 1)));
    if (!seen_keys.insert(key).second) {
      return std::nullopt;
    }
    pos = SkipWhitespace(in, name_end);
    std::optional<size_t> end = ScanValue(in, pos, depth);
    if (!end.has_value()) {
      return std::nullopt;
    }
    if (!wanted.IsEmpty() && value_start && value_end && key == wanted) {
      *value_start = pos;
      *value_end = end.value();
    }
    pos = end.value();
  }
}

// The value span of top-level entry |key| of the dictionary that starts at
// |dict_pos|, or nullopt when the dictionary does not scan or has no such
// key.
std::optional<std::pair<size_t, size_t>> FindEntry(
    pdfium::span<const uint8_t> in,
    size_t dict_pos,
    ByteStringView key) {
  size_t start = 0;
  size_t end = 0;
  if (!ScanDictionaryBody(in, dict_pos, 0, key, &start, &end).has_value() ||
      end == 0) {
    return std::nullopt;
  }
  return std::make_pair(start, end);
}

// Position of the first "<<" of an indirect object body "N G obj <<...".
std::optional<size_t> FindObjectDictionary(pdfium::span<const uint8_t> in) {
  size_t pos = SkipWhitespace(in, 0);
  for (int token = 0; token < 3; ++token) {  // "N", "G", "obj"
    const size_t end = ScanRegular(in, pos);
    if (end == pos) {
      return std::nullopt;
    }
    pos = SkipWhitespace(in, end);
  }
  if (pos + 2 <= in.size() && in[pos] == '<' && in[pos + 1] == '<') {
    return pos;
  }
  return std::nullopt;
}

}  // namespace pdfscan

// Physical location of this signature's /Contents value in the file: the
// /V dictionary is read at its cross-reference position and scanned, so a
// hole that merely holds the same bytes elsewhere is never accepted.
// Returns the value span as file offsets, or nullopt (a compressed or
// direct /V, or a dictionary that does not scan).
std::optional<std::pair<uint64_t, uint64_t>> LocateContentsValue(
    IFX_SeekableReadStream* file,
    CPDF_Parser* parser,
    uint32_t value_objnum,
    size_t contents_size) {
  if (!file || !parser || value_objnum == 0) {
    return std::nullopt;
  }
  const FX_FILESIZE pos_rel = parser->GetObjectPositionOrZero(value_objnum);
  if (pos_rel <= 0) {
    return std::nullopt;  // compressed, free, or unknown
  }
  const uint64_t pos = static_cast<uint64_t>(pos_rel) +
                       static_cast<uint64_t>(parser->GetFileHeaderOffset());
  const uint64_t file_size = static_cast<uint64_t>(file->GetSize());
  if (pos >= file_size) {
    return std::nullopt;
  }
  static constexpr uint64_t kWindowCap = 4u << 20;
  const uint64_t window = std::min<uint64_t>(
      file_size - pos,
      std::min<uint64_t>(kWindowCap, 2ull * contents_size + 64 * 1024));
  DataVector<uint8_t> bytes(static_cast<size_t>(window));
  if (!file->ReadBlockAtOffset(bytes, static_cast<FX_FILESIZE>(pos))) {
    return std::nullopt;
  }
  std::optional<size_t> dict = pdfscan::FindObjectDictionary(bytes);
  if (!dict.has_value()) {
    return std::nullopt;
  }
  std::optional<std::pair<size_t, size_t>> value =
      pdfscan::FindEntry(bytes, dict.value(), "Contents");
  if (!value.has_value() || bytes[value->first] != '<') {
    return std::nullopt;
  }
  return std::make_pair(pos + value->first, pos + value->second);
}

// Coverage and DER facts for a signed record, computed against the loaded
// bytes. Requires |record->byte_range| and the raw /Contents.
void ComputeCoverage(IFX_SeekableReadStream* file,
                     CPDF_Parser* parser,
                     const std::optional<std::vector<RevisionInfo>>& revisions,
                     pdfium::span<const uint8_t> contents_raw,
                     SignatureRecord* record) {
  record->coverage = EPDF_SIG_COVERAGE_MALFORMED;
  record->revision_index = -1;
  record->contents_der.clear();

  // The DER object and its padding are judged first: a malformed /Contents
  // is malformed regardless of the ranges.
  std::optional<size_t> der_len = DerOuterLength(contents_raw);
  if (!der_len.has_value()) {
    return;
  }
  for (size_t i = der_len.value(); i < contents_raw.size(); ++i) {
    if (contents_raw[i] != 0) {
      return;
    }
  }

  if (!record->has_byte_range || !file) {
    return;
  }
  const uint64_t file_size = static_cast<uint64_t>(file->GetSize());
  const std::array<uint64_t, 4>& r = record->byte_range;
  const uint64_t hole_start = r[0] + r[1];
  const uint64_t hole_end = r[2];
  const uint64_t signed_end = r[2] + r[3];
  if (hole_start > hole_end || hole_end > signed_end || signed_end > file_size ||
      r[1] > file_size || r[3] > file_size) {
    return;
  }
  // Well-formed ranges from here on: PARTIAL unless everything lines up.
  record->contents_der.assign(contents_raw.begin(),
                              contents_raw.begin() + der_len.value());
  record->coverage = EPDF_SIG_COVERAGE_PARTIAL;

  if (r[0] != 0 || hole_end - hole_start < 2) {
    return;
  }
  // The hole must be exactly "<hex>" for this signature's /Contents.
  const size_t hole_size = static_cast<size_t>(hole_end - hole_start);
  if (hole_size > (64u << 20)) {
    return;
  }
  DataVector<uint8_t> hole(hole_size);
  if (!file->ReadBlockAtOffset(hole, static_cast<FX_FILESIZE>(hole_start))) {
    return;
  }
  if (hole.front() != '<' || hole.back() != '>') {
    return;
  }
  std::optional<DataVector<uint8_t>> decoded =
      DecodeHexBody(pdfium::span(hole).subspan(1u, hole_size - 2));
  if (!decoded.has_value() || decoded->size() != contents_raw.size() ||
      !std::equal(decoded->begin(), decoded->end(), contents_raw.begin())) {
    return;
  }
  if (!revisions.has_value()) {
    return;
  }
  int revision_index = -1;
  for (size_t i = 0; i < revisions->size(); ++i) {
    if (static_cast<uint64_t>((*revisions)[i].end) == signed_end) {
      revision_index = static_cast<int>(i);
      break;
    }
  }
  if (revision_index < 0) {
    return;
  }
  // Equal bytes are not enough: the hole must be where THIS signature's
  // /Contents physically lives - judged with the cross-reference state of
  // the revision the signature seals. A later update may rewrite the /V
  // dictionary (PDFium's own incremental save rewrites every loaded
  // object), which moves the object in the current xref without changing
  // what was signed.
  std::optional<std::pair<uint64_t, uint64_t>> located;
  if (signed_end == file_size) {
    located = LocateContentsValue(file, parser, record->value_objnum,
                                  contents_raw.size());
  } else {
    std::unique_ptr<CPDF_Document> prefix =
        OpenPrefixDocument(parser, static_cast<FX_FILESIZE>(signed_end));
    if (prefix) {
      located = LocateContentsValue(file, prefix->GetParser(),
                                    record->value_objnum, contents_raw.size());
    }
  }
  if (!located.has_value() || located->first != hole_start ||
      located->second != hole_end) {
    return;
  }
  record->coverage = EPDF_SIG_COVERAGE_WHOLE_REVISION;
  record->revision_index = revision_index;
}

bool IsCatalogCertification(CPDF_Document* doc,
                            const CPDF_Dictionary* value_dict) {
  const CPDF_Dictionary* root = doc->GetRoot();
  if (!root || !value_dict) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> perms = root->GetDictFor("Perms");
  if (!perms) {
    return false;
  }
  RetainPtr<const CPDF_Object> entry = perms->GetObjectFor("DocMDP");
  if (!entry) {
    return false;
  }
  if (const CPDF_Reference* ref = entry->AsReference()) {
    return value_dict->GetObjNum() != 0 &&
           ref->GetRefObjNum() == value_dict->GetObjNum();
  }
  return entry.Get() == value_dict;
}

void ReadStringInto(const CPDF_Dictionary* dict,
                    ByteStringView key,
                    int slot,
                    SignatureRecord* record) {
  RetainPtr<const CPDF_Object> obj = dict->GetDirectObjectFor(key);
  if (!obj || !(obj->IsString() || obj->IsName())) {
    return;
  }
  record->strings[slot] = obj->GetUnicodeText();
}

SignatureRecord SnapshotSignature(
    CPDF_Document* doc,
    IFX_SeekableReadStream* file,
    CPDF_Parser* parser,
    const std::optional<std::vector<RevisionInfo>>& revisions,
    CPDF_FormField* field,
    const std::map<const CPDF_Dictionary*, uint32_t>& widget_pages) {
  SignatureRecord record;
  RetainPtr<const CPDF_Dictionary> field_dict = field->GetFieldDict();
  record.field_objnum = field_dict->GetObjNum();
  record.fqn = field->GetFullName();

  if (field->CountControls() > 0) {
    CPDF_FormControl* control = field->GetControl(0);
    RetainPtr<const CPDF_Dictionary> widget =
        control ? control->GetWidgetDict() : nullptr;
    if (widget) {
      record.widget_objnum = widget->GetObjNum();
      record.page_objnum = epdf::PageObjNumForWidget(widget_pages, widget.Get());
    }
  }

  RetainPtr<const CPDF_Dictionary> lock = field_dict->GetDictFor("Lock");
  if (lock) {
    record.lock_action = ParseFieldAction(lock.Get());
    record.lock_fields = ReadTextArray(lock.Get(), "Fields");
    record.lock_permission = ParsePermission(lock.Get(), "P", 0);
  }
  ReadSeedValue(field_dict.Get(), &record);

  RetainPtr<const CPDF_Object> value_obj = CPDF_FormField::GetFieldAttrForDict(
      field_dict.Get(), pdfium::form_fields::kV);
  RetainPtr<const CPDF_Dictionary> value_dict =
      value_obj ? ToDictionary(value_obj) : nullptr;
  if (!value_dict) {
    return record;
  }
  record.value_objnum = value_dict->GetObjNum();
  record.kind = value_dict->GetNameFor("Type") == "DocTimeStamp"
                    ? EPDF_SIG_KIND_DOC_TIMESTAMP
                    : EPDF_SIG_KIND_SIGNATURE;
  ReadStringInto(value_dict.Get(), "Filter", EPDF_SIG_STRING_FILTER, &record);
  ReadStringInto(value_dict.Get(), "SubFilter", EPDF_SIG_STRING_SUBFILTER,
                 &record);
  ReadStringInto(value_dict.Get(), "Name", EPDF_SIG_STRING_NAME, &record);
  ReadStringInto(value_dict.Get(), "Reason", EPDF_SIG_STRING_REASON, &record);
  ReadStringInto(value_dict.Get(), "Location", EPDF_SIG_STRING_LOCATION,
                 &record);
  ReadStringInto(value_dict.Get(), "ContactInfo", EPDF_SIG_STRING_CONTACT_INFO,
                 &record);
  ReadStringInto(value_dict.Get(), "M", EPDF_SIG_STRING_M, &record);
  ReadReference(value_dict.Get(), &record);
  record.catalog_certification = IsCatalogCertification(doc, value_dict.Get());

  RetainPtr<const CPDF_Object> contents_obj =
      value_dict->GetDirectObjectFor("Contents");
  if (!contents_obj || !contents_obj->IsString()) {
    return record;
  }
  record.is_signed = true;

  RetainPtr<const CPDF_Array> byte_range = value_dict->GetArrayFor("ByteRange");
  if (byte_range && byte_range->size() == 4) {
    bool ok = true;
    for (size_t i = 0; i < 4; ++i) {
      RetainPtr<const CPDF_Number> number = byte_range->GetNumberAt(i);
      if (!number || !number->IsInteger() || number->GetInteger() < 0) {
        ok = false;
        break;
      }
      record.byte_range[i] = static_cast<uint64_t>(number->GetInteger());
    }
    record.has_byte_range = ok;
  }
  const ByteString contents = contents_obj->GetString();
  ComputeCoverage(file, parser, revisions, contents.unsigned_span(), &record);
  return record;
}

unsigned long ReturnText(const WideString& text,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen) {
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      text, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

const std::vector<WideString>* FieldNameList(const SignatureRecord* record,
                                             int which) {
  switch (which) {
    case EPDF_SIG_FIELDS_FIELDMDP:
      return &record->fieldmdp_fields;
    case EPDF_SIG_FIELDS_LOCK:
      return &record->lock_fields;
    default:
      return nullptr;
  }
}

const std::vector<WideString>* SeedValueList(const SignatureRecord* record,
                                             int list) {
  switch (list) {
    case EPDF_SIG_SV_LIST_SUBFILTER:
      return &record->sv_subfilters;
    case EPDF_SIG_SV_LIST_DIGEST_METHOD:
      return &record->sv_digest_methods;
    case EPDF_SIG_SV_LIST_REASONS:
      return &record->sv_reasons;
    default:
      return nullptr;
  }
}

// ---------------------------------------------------------------------------
// Digests.
// ---------------------------------------------------------------------------

class Hasher {
 public:
  explicit Hasher(int algorithm) : algorithm_(algorithm) {
    switch (algorithm_) {
      case EPDF_DIGEST_SHA1:
        CRYPT_SHA1Start(&sha1_);
        break;
      case EPDF_DIGEST_SHA256:
        CRYPT_SHA256Start(&sha2_);
        break;
      case EPDF_DIGEST_SHA384:
        CRYPT_SHA384Start(&sha2_);
        break;
      case EPDF_DIGEST_SHA512:
        CRYPT_SHA512Start(&sha2_);
        break;
    }
  }

  static std::optional<size_t> DigestSize(int algorithm) {
    switch (algorithm) {
      case EPDF_DIGEST_SHA1:
        return 20;
      case EPDF_DIGEST_SHA256:
        return 32;
      case EPDF_DIGEST_SHA384:
        return 48;
      case EPDF_DIGEST_SHA512:
        return 64;
      default:
        return std::nullopt;
    }
  }

  void Update(pdfium::span<const uint8_t> data) {
    switch (algorithm_) {
      case EPDF_DIGEST_SHA1:
        CRYPT_SHA1Update(&sha1_, data);
        break;
      case EPDF_DIGEST_SHA256:
        CRYPT_SHA256Update(&sha2_, data);
        break;
      case EPDF_DIGEST_SHA384:
        CRYPT_SHA384Update(&sha2_, data);
        break;
      case EPDF_DIGEST_SHA512:
        CRYPT_SHA512Update(&sha2_, data);
        break;
    }
  }

  void Finish(pdfium::span<uint8_t> out) {
    switch (algorithm_) {
      case EPDF_DIGEST_SHA1:
        CRYPT_SHA1Finish(&sha1_, out.first<20>());
        break;
      case EPDF_DIGEST_SHA256:
        CRYPT_SHA256Finish(&sha2_, out.first<32>());
        break;
      case EPDF_DIGEST_SHA384:
        CRYPT_SHA384Finish(&sha2_, out.first<48>());
        break;
      case EPDF_DIGEST_SHA512:
        CRYPT_SHA512Finish(&sha2_, out.first<64>());
        break;
    }
  }

 private:
  const int algorithm_;
  CRYPT_sha1_context sha1_;
  CRYPT_sha2_context sha2_;
};

bool HashRange(IFX_SeekableReadStream* file,
               uint64_t start,
               uint64_t length,
               Hasher* hasher) {
  static constexpr size_t kChunk = 64 * 1024;
  DataVector<uint8_t> chunk(kChunk);
  uint64_t offset = start;
  uint64_t remaining = length;
  while (remaining > 0) {
    const size_t step =
        static_cast<size_t>(std::min<uint64_t>(remaining, kChunk));
    pdfium::span<uint8_t> block = pdfium::span(chunk).first(step);
    if (!file->ReadBlockAtOffset(block, static_cast<FX_FILESIZE>(offset))) {
      return false;
    }
    hasher->Update(block);
    offset += step;
    remaining -= step;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Revisions.
// ---------------------------------------------------------------------------

FPDF_EXPORT int FPDF_CALLCONV
EPDFDoc_GetRevisionCount(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  epdf::RevisionView* view = epdf::RevisionView::For(doc);
  if (!view) {
    return -1;
  }
  std::optional<std::vector<RevisionInfo>> revisions = RevisionsOf(view);
  return revisions.has_value() ? fxcrt::CollectionSize<int>(*revisions) : -1;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_GetRevision(FPDF_DOCUMENT document,
                    int index,
                    unsigned long long* out_end,
                    unsigned long long* out_xref_offset) {
  if (out_end) {
    *out_end = 0;
  }
  if (out_xref_offset) {
    *out_xref_offset = 0;
  }
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  epdf::RevisionView* view = epdf::RevisionView::For(doc);
  if (!view) {
    return false;
  }
  std::optional<std::vector<RevisionInfo>> revisions = RevisionsOf(view);
  if (!revisions.has_value() || index < 0 ||
      index >= fxcrt::CollectionSize<int>(*revisions)) {
    return false;
  }
  const RevisionInfo& revision = (*revisions)[index];
  if (out_end) {
    *out_end = static_cast<unsigned long long>(revision.end);
  }
  if (out_xref_offset) {
    *out_xref_offset = static_cast<unsigned long long>(revision.xref_offset);
  }
  return true;
}

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFDoc_OpenRevision(FPDF_DOCUMENT document, unsigned long long end) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  epdf::RevisionView* view = epdf::RevisionView::For(doc);
  if (!view) {
    return nullptr;
  }
  CPDF_Parser* parser = view->parser();
  std::optional<std::vector<RevisionInfo>> revisions = RevisionsOf(view);
  if (!revisions.has_value()) {
    return nullptr;
  }
  const bool known = std::any_of(
      revisions->begin(), revisions->end(), [end](const RevisionInfo& r) {
        return static_cast<unsigned long long>(r.end) == end;
      });
  if (!known) {
    return nullptr;
  }
  std::unique_ptr<CPDF_Document> prefix =
      OpenPrefixDocument(parser, static_cast<FX_FILESIZE>(end));
  if (!prefix) {
    ProcessParseError(CPDF_Parser::FORMAT_ERROR);
    return nullptr;
  }
  return FPDFDocumentFromCPDFDocument(prefix.release());
}

namespace {

// The document a layer's cumulative delta describes: its immutable base
// followed by |extra|, opened read-only. A layer's own parser is the BASE
// parser: its file access is the frozen base alone, which is exactly what a
// cumulative delta is relative to (never the base plus the loaded delta -
// that would misplace every offset the delta's cross-reference section
// declares).
FPDF_DOCUMENT OpenBaseOverlayWith(CPDF_Document* doc,
                                  RetainPtr<IFX_SeekableReadStream> extra) {
  CPDF_Parser* base_parser = doc->GetParser();
  RetainPtr<IFX_SeekableReadStream> base =
      base_parser ? base_parser->GetFileAccess() : nullptr;
  if (!base || !extra) {
    return nullptr;
  }
  auto bytes = pdfium::MakeRetain<CPDF_ConcatReadStream>(std::move(base),
                                                         std::move(extra));
  auto overlay = std::make_unique<CPDF_Document>(
      std::make_unique<CPDF_DocRenderData>(),
      std::make_unique<CPDF_DocPageData>());
  if (overlay->LoadDoc(std::move(bytes), base_parser->GetPassword()) !=
      CPDF_Parser::SUCCESS) {
    ProcessParseError(CPDF_Parser::FORMAT_ERROR);
    return nullptr;
  }
  return FPDFDocumentFromCPDFDocument(overlay.release());
}

}  // namespace

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFDoc_OpenBaseOverlayFromPath(FPDF_DOCUMENT document, FPDF_STRING delta_path) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  const CPDF_LayerDocument* layer =
      doc ? CPDF_LayerDocument::FromDocument(doc) : nullptr;
  if (!layer || !delta_path || !*delta_path) {
    return nullptr;
  }
  RetainPtr<IFX_SeekableReadStream> delta =
      CFX_FileAccessStream::CreateFromFilename(delta_path);
  if (!delta || delta->GetSize() <= 0) {
    return nullptr;
  }
  return OpenBaseOverlayWith(doc, std::move(delta));
}

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
EPDFDoc_OpenBaseOverlay(FPDF_DOCUMENT document,
                        const void* delta,
                        unsigned long delta_len) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  const CPDF_LayerDocument* layer =
      doc ? CPDF_LayerDocument::FromDocument(doc) : nullptr;
  if (!layer || !delta || delta_len == 0) {
    return nullptr;
  }
  // SAFETY: |delta_len| bytes at |delta|, required from the caller.
  auto delta_span = UNSAFE_BUFFERS(
      pdfium::span(static_cast<const uint8_t*>(delta), delta_len));
  return OpenBaseOverlayWith(
      doc, pdfium::MakeRetain<OwnedBytesReadStream>(
               DataVector<uint8_t>(delta_span.begin(), delta_span.end())));
}

// ---------------------------------------------------------------------------
// Signature model.
// ---------------------------------------------------------------------------

FPDF_EXPORT EPDF_SIGNATURE_MODEL FPDF_CALLCONV
EPDFSig_LoadModel(FPDF_DOCUMENT document) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return nullptr;
  }
  return HandleFromModel(BuildModel(doc).release());
}

namespace {

std::unique_ptr<SignatureModel> BuildModel(CPDF_Document* doc) {
  auto model = std::make_unique<SignatureModel>();

  // Identity, locks and seed values come from the document as it is; the
  // byte facts (revisions, coverage) from the bytes it was loaded from.
  epdf::RevisionView* view = epdf::RevisionView::For(doc);
  CPDF_Parser* parser = view ? view->parser() : nullptr;
  std::optional<std::vector<RevisionInfo>> revisions = RevisionsOf(view);
  model->chain_valid = revisions.has_value();
  RetainPtr<IFX_SeekableReadStream> file = view ? view->file() : nullptr;

  auto form = std::make_unique<CPDF_InteractiveForm>(doc);
  const std::map<const CPDF_Dictionary*, uint32_t> widget_pages =
      epdf::SweepPageWidgets(doc, form.get());
  const size_t field_count = epdf::CountFormFields(*form);
  for (size_t i = 0; i < field_count; ++i) {
    CPDF_FormField* field = form->GetField(i, WideString());
    if (!field || field->GetType() != CPDF_FormField::kSign) {
      continue;
    }
    SignatureRecord record = SnapshotSignature(doc, file.Get(), parser,
                                               revisions, field, widget_pages);
    if (record.field_objnum != 0) {
      model->index_by_field_objnum[record.field_objnum] =
          fxcrt::CollectionSize<int>(model->records);
    }
    model->records.push_back(std::move(record));
  }
  return model;
}

}  // namespace

FPDF_EXPORT void FPDF_CALLCONV EPDFSig_CloseModel(EPDF_SIGNATURE_MODEL model) {
  delete ModelFromHandle(model);
}

FPDF_EXPORT int FPDF_CALLCONV EPDFSig_Count(EPDF_SIGNATURE_MODEL model) {
  const SignatureModel* m = ModelFromHandle(model);
  return m ? fxcrt::CollectionSize<int>(m->records) : -1;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_IsRevisionChainValid(EPDF_SIGNATURE_MODEL model) {
  const SignatureModel* m = ModelFromHandle(model);
  return m && m->chain_valid;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetIndexByFieldObjNum(EPDF_SIGNATURE_MODEL model,
                              uint32_t field_objnum) {
  const SignatureModel* m = ModelFromHandle(model);
  if (!m || field_objnum == 0) {
    return -1;
  }
  const auto it = m->index_by_field_objnum.find(field_objnum);
  return it == m->index_by_field_objnum.end() ? -1 : it->second;
}

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_GetFieldObjNum(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->field_objnum : 0;
}

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_GetWidgetObjNum(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->widget_objnum : 0;
}

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_GetWidgetPageObjNum(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->page_objnum : 0;
}

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_GetValueObjNum(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->value_objnum : 0;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFSig_GetFieldName(EPDF_SIGNATURE_MODEL model,
                     int index,
                     FPDF_WCHAR* buffer,
                     unsigned long buflen) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? ReturnText(record->fqn, buffer, buflen) : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFSig_IsSigned(EPDF_SIGNATURE_MODEL model,
                                                   int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record && record->is_signed;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFSig_GetKind(EPDF_SIGNATURE_MODEL model,
                                            int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->kind : EPDF_SIG_KIND_SIGNATURE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_GetByteRange(EPDF_SIGNATURE_MODEL model,
                     int index,
                     unsigned long long out_range[4]) {
  const SignatureRecord* record = GetRecord(model, index);
  if (!record || !out_range) {
    return false;
  }
  if (!record->has_byte_range) {
    return false;
  }
  // SAFETY: caller provides four slots.
  auto out = UNSAFE_BUFFERS(pdfium::span(out_range, 4u));
  for (size_t i = 0; i < 4; ++i) {
    out[i] = record->byte_range[i];
  }
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFSig_GetCoverage(EPDF_SIGNATURE_MODEL model,
                                                int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->coverage : EPDF_SIG_COVERAGE_MALFORMED;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetRevisionIndex(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->revision_index : -1;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFSig_GetContents(EPDF_SIGNATURE_MODEL model,
                    int index,
                    void* buffer,
                    unsigned long buflen) {
  const SignatureRecord* record = GetRecord(model, index);
  if (!record || record->contents_der.empty()) {
    return 0;
  }
  // SAFETY: required from caller.
  auto result_span = UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen));
  fxcrt::try_spancpy(pdfium::as_writable_bytes(result_span),
                     pdfium::span(record->contents_der));
  return pdfium::checked_cast<unsigned long>(record->contents_der.size());
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFSig_GetString(EPDF_SIGNATURE_MODEL model,
                  int index,
                  int key,
                  FPDF_WCHAR* buffer,
                  unsigned long buflen) {
  const SignatureRecord* record = GetRecord(model, index);
  if (!record) {
    return 0;
  }
  const auto it = record->strings.find(key);
  if (it == record->strings.end()) {
    return 0;
  }
  return ReturnText(it->second, buffer, buflen);
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetDocMDPPermission(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->docmdp_permission : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_IsCatalogCertification(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record && record->catalog_certification;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetFieldMDPAction(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->fieldmdp_action : EPDF_SIG_FIELD_ACTION_NONE;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFSig_GetLockAction(EPDF_SIGNATURE_MODEL model,
                                                  int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->lock_action : EPDF_SIG_FIELD_ACTION_NONE;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetLockPermission(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->lock_permission : 0;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetFieldNameCount(EPDF_SIGNATURE_MODEL model, int index, int which) {
  const SignatureRecord* record = GetRecord(model, index);
  const std::vector<WideString>* list =
      record ? FieldNameList(record, which) : nullptr;
  return list ? fxcrt::CollectionSize<int>(*list) : -1;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFSig_GetFieldNameAt(EPDF_SIGNATURE_MODEL model,
                       int index,
                       int which,
                       int name_index,
                       FPDF_WCHAR* buffer,
                       unsigned long buflen) {
  const SignatureRecord* record = GetRecord(model, index);
  const std::vector<WideString>* list =
      record ? FieldNameList(record, which) : nullptr;
  if (!list || name_index < 0 || name_index >= fxcrt::CollectionSize<int>(*list)) {
    return 0;
  }
  return ReturnText((*list)[name_index], buffer, buflen);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_HasSeedValue(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record && record->has_seed_value;
}

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_GetSeedValueRequiredFlags(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->sv_required : 0;
}

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_GetSeedValuePresentFlags(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->sv_present : 0;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetSeedValueMDP(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->sv_mdp : 0;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetSeedValueVersion(EPDF_SIGNATURE_MODEL model, int index) {
  const SignatureRecord* record = GetRecord(model, index);
  return record ? record->sv_version : 0;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFSig_GetSeedValueFilter(EPDF_SIGNATURE_MODEL model,
                           int index,
                           FPDF_WCHAR* buffer,
                           unsigned long buflen) {
  const SignatureRecord* record = GetRecord(model, index);
  if (!record || !(record->sv_present & EPDF_SIG_SV_FILTER)) {
    return 0;
  }
  return ReturnText(record->sv_filter, buffer, buflen);
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFSig_GetSeedValueListCount(EPDF_SIGNATURE_MODEL model, int index, int list) {
  const SignatureRecord* record = GetRecord(model, index);
  const std::vector<WideString>* items =
      record ? SeedValueList(record, list) : nullptr;
  return items ? fxcrt::CollectionSize<int>(*items) : -1;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFSig_GetSeedValueListAt(EPDF_SIGNATURE_MODEL model,
                           int index,
                           int list,
                           int item,
                           FPDF_WCHAR* buffer,
                           unsigned long buflen) {
  const SignatureRecord* record = GetRecord(model, index);
  const std::vector<WideString>* items =
      record ? SeedValueList(record, list) : nullptr;
  if (!items || item < 0 || item >= fxcrt::CollectionSize<int>(*items)) {
    return 0;
  }
  return ReturnText((*items)[item], buffer, buflen);
}

// ---------------------------------------------------------------------------
// Digests.
// ---------------------------------------------------------------------------

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_DigestByteRange(FPDF_DOCUMENT document,
                        const unsigned long long range[4],
                        int algorithm,
                        unsigned char* out_digest,
                        unsigned long* inout_len) {
  if (!range || !inout_len) {
    return false;
  }
  std::optional<size_t> digest_size = Hasher::DigestSize(algorithm);
  if (!digest_size.has_value()) {
    return false;
  }
  if (!out_digest || *inout_len < digest_size.value()) {
    *inout_len = static_cast<unsigned long>(digest_size.value());
    return false;
  }
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  epdf::RevisionView* view = epdf::RevisionView::For(doc);
  RetainPtr<IFX_SeekableReadStream> file = view ? view->file() : nullptr;
  if (!file) {
    return false;
  }
  // SAFETY: caller provides four slots.
  auto r = UNSAFE_BUFFERS(pdfium::span(range, 4u));
  const uint64_t file_size = static_cast<uint64_t>(file->GetSize());
  if (r[1] > file_size || r[0] > file_size - r[1] || r[3] > file_size ||
      r[2] > file_size - r[3] || r[0] + r[1] > r[2]) {
    return false;
  }
  Hasher hasher(algorithm);
  if (!HashRange(file.Get(), r[0], r[1], &hasher) ||
      !HashRange(file.Get(), r[2], r[3], &hasher)) {
    return false;
  }
  // SAFETY: capacity checked above.
  hasher.Finish(UNSAFE_BUFFERS(pdfium::span(out_digest, digest_size.value())));
  *inout_len = static_cast<unsigned long>(digest_size.value());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_DigestFileRange(FPDF_FILEACCESS* file_access,
                        const unsigned long long range[4],
                        int algorithm,
                        unsigned char* out_digest,
                        unsigned long* inout_len) {
  if (!file_access || !range || !inout_len) {
    return false;
  }
  std::optional<size_t> digest_size = Hasher::DigestSize(algorithm);
  if (!digest_size.has_value()) {
    return false;
  }
  if (!out_digest || *inout_len < digest_size.value()) {
    *inout_len = static_cast<unsigned long>(digest_size.value());
    return false;
  }
  auto file = pdfium::MakeRetain<CPDFSDK_CustomAccess>(file_access);
  // SAFETY: caller provides four slots.
  auto r = UNSAFE_BUFFERS(pdfium::span(range, 4u));
  const uint64_t file_size = static_cast<uint64_t>(file->GetSize());
  if (r[1] > file_size || r[0] > file_size - r[1] || r[3] > file_size ||
      r[2] > file_size - r[3] || r[0] + r[1] > r[2]) {
    return false;
  }
  Hasher hasher(algorithm);
  if (!HashRange(file.Get(), r[0], r[1], &hasher) ||
      !HashRange(file.Get(), r[2], r[3], &hasher)) {
    return false;
  }
  // SAFETY: capacity checked above.
  hasher.Finish(UNSAFE_BUFFERS(pdfium::span(out_digest, digest_size.value())));
  *inout_len = static_cast<unsigned long>(digest_size.value());
  return true;
}

// ---------------------------------------------------------------------------
// Loaded bytes.
// ---------------------------------------------------------------------------

FPDF_EXPORT unsigned long long FPDF_CALLCONV
EPDFDoc_GetLoadedBytesSize(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  epdf::RevisionView* view = epdf::RevisionView::For(doc);
  RetainPtr<IFX_SeekableReadStream> file = view ? view->file() : nullptr;
  if (!file) {
    return 0;
  }
  const FX_FILESIZE size = file->GetSize();
  return size > 0 ? static_cast<unsigned long long>(size) : 0;
}

FPDF_EXPORT unsigned long long FPDF_CALLCONV
EPDFDoc_GetBaseBytesSize(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (const CPDF_LayerDocument* layer = CPDF_LayerDocument::FromDocument(doc)) {
    const FX_FILESIZE size = layer->GetBaseDocument()->GetRawBaseSize();
    return size > 0 ? static_cast<unsigned long long>(size) : 0;
  }
  return EPDFDoc_GetLoadedBytesSize(document);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_GetStructureObjectNumbers(FPDF_DOCUMENT document,
                                  unsigned int* out_root,
                                  unsigned int* out_acroform,
                                  unsigned int* out_pages) {
  if (out_root) {
    *out_root = 0;
  }
  if (out_acroform) {
    *out_acroform = 0;
  }
  if (out_pages) {
    *out_pages = 0;
  }
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  CPDF_Parser* parser = doc ? doc->GetParser() : nullptr;
  if (!parser) {
    return false;
  }
  const CPDF_Dictionary* root = doc->GetRoot();
  if (out_root) {
    *out_root = parser->GetRootObjNum();
  }
  if (root) {
    if (out_acroform) {
      RetainPtr<const CPDF_Dictionary> acroform = root->GetDictFor("AcroForm");
      *out_acroform = acroform ? acroform->GetObjNum() : 0;
    }
    if (out_pages) {
      RetainPtr<const CPDF_Dictionary> pages = root->GetDictFor("Pages");
      *out_pages = pages ? pages->GetObjNum() : 0;
    }
  }
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_ReadLoadedBytes(FPDF_DOCUMENT document,
                        unsigned long long offset,
                        void* buffer,
                        unsigned long length) {
  if (!buffer || length == 0) {
    return 0;
  }
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  epdf::RevisionView* view = epdf::RevisionView::For(doc);
  RetainPtr<IFX_SeekableReadStream> file = view ? view->file() : nullptr;
  if (!file || file->GetSize() < 0) {
    return 0;
  }
  const uint64_t size = static_cast<uint64_t>(file->GetSize());
  if (offset > size || length > size - offset) {
    return 0;
  }
  // SAFETY: the caller provides |length| bytes.
  auto out = UNSAFE_BUFFERS(pdfium::span(static_cast<uint8_t*>(buffer), length));
  if (!file->ReadBlockAtOffset(out, static_cast<FX_FILESIZE>(offset))) {
    return 0;
  }
  return length;
}

// ---------------------------------------------------------------------------
// Signing.
// ---------------------------------------------------------------------------

namespace {

constexpr int kByteRangeSentinel = 2147483647;
// What CPDF_Array/CPDF_Number serialise the sentinel array body as.
constexpr char kSentinelBody[] = " 0 2147483647 2147483647 2147483647";

const char* SubFilterName(int subfilter) {
  switch (subfilter) {
    case EPDF_SIG_SUBFILTER_ADBE_PKCS7_DETACHED:
      return "adbe.pkcs7.detached";
    case EPDF_SIG_SUBFILTER_ETSI_CADES_DETACHED:
      return "ETSI.CAdES.detached";
    case EPDF_SIG_SUBFILTER_ETSI_RFC3161:
      return "ETSI.RFC3161";
    default:
      return nullptr;
  }
}

const char* DigestMethodName(int digest) {
  switch (digest) {
    case EPDF_DIGEST_SHA256:
      return "SHA256";
    case EPDF_DIGEST_SHA384:
      return "SHA384";
    case EPDF_DIGEST_SHA512:
      return "SHA512";
    default:
      return nullptr;
  }
}

const char* FieldActionName(int action) {
  switch (action) {
    case EPDF_SIG_FIELD_ACTION_ALL:
      return "All";
    case EPDF_SIG_FIELD_ACTION_INCLUDE:
      return "Include";
    case EPDF_SIG_FIELD_ACTION_EXCLUDE:
      return "Exclude";
    default:
      return nullptr;
  }
}

ByteString NowAsPdfDate() {
  const time_t now = time(nullptr);
  struct tm utc = {};
#if BUILDFLAG(IS_WIN)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  return ByteString::Format("D:%04d%02d%02d%02d%02d%02dZ", utc.tm_year + 1900,
                            utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                            utc.tm_min, utc.tm_sec);
}

bool Contains(const std::vector<WideString>& list, const WideString& item) {
  return std::find(list.begin(), list.end(), item) != list.end();
}

bool SameNameSet(std::vector<WideString> a, std::vector<WideString> b) {
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  a.erase(std::unique(a.begin(), a.end()), a.end());
  b.erase(std::unique(b.begin(), b.end()), b.end());
  return a == b;
}

// A listed name covers the field of that name and every descendant
// ("group" covers "group.total").
bool Covers(const std::vector<WideString>& names, const WideString& fqn) {
  for (const WideString& name : names) {
    if (name == fqn) {
      return true;
    }
    if (fqn.GetLength() > name.GetLength() &&
        fqn.First(name.GetLength()) == name && fqn[name.GetLength()] == L'.') {
      return true;
    }
  }
  return false;
}

// Whether |fqn| is locked by the signed record |r| (its FieldMDP transform
// or, once signed, its /Lock).
bool LocksField(const SignatureRecord& r, const WideString& fqn) {
  if (!r.is_signed) {
    return false;
  }
  for (const auto& [action, fields] :
       {std::pair<int, const std::vector<WideString>*>{r.fieldmdp_action,
                                                        &r.fieldmdp_fields},
        std::pair<int, const std::vector<WideString>*>{r.lock_action,
                                                        &r.lock_fields}}) {
    switch (action) {
      case EPDF_SIG_FIELD_ACTION_ALL:
        return true;
      case EPDF_SIG_FIELD_ACTION_INCLUDE:
        if (Covers(*fields, fqn)) {
          return true;
        }
        break;
      case EPDF_SIG_FIELD_ACTION_EXCLUDE:
        if (!Covers(*fields, fqn)) {
          return true;
        }
        break;
      default:
        break;
    }
  }
  return false;
}

// The permission in force: the catalog certification's P and every signed
// field's /Lock /P, tightest wins; 0 when nothing restricts the document.
int PermissionInForce(const SignatureModel& model) {
  int permission = 0;
  auto tighten = [&permission](int p) {
    if (p >= 1 && p <= 3 && (permission == 0 || p < permission)) {
      permission = p;
    }
  };
  for (const SignatureRecord& r : model.records) {
    if (!r.is_signed) {
      continue;
    }
    if (r.catalog_certification) {
      tighten(r.docmdp_permission);
    }
    tighten(r.lock_permission);
  }
  return permission;
}

bool AnySigned(const SignatureModel& model) {
  return std::any_of(model.records.begin(), model.records.end(),
                     [](const SignatureRecord& r) { return r.is_signed; });
}

std::vector<WideString> WideStrings(const FPDF_WIDESTRING* items, int count) {
  std::vector<WideString> out;
  if (!items || count <= 0) {
    return out;
  }
  // SAFETY: caller provides |count| entries.
  auto span = UNSAFE_BUFFERS(pdfium::span(items, static_cast<size_t>(count)));
  for (FPDF_WIDESTRING item : span) {
    if (item) {
      out.push_back(WideStringFromFPDFWideString(item));
    }
  }
  return out;
}

RetainPtr<CPDF_Dictionary> MakeLockDict(CPDF_Document* doc,
                                        int action,
                                        const std::vector<WideString>& fields,
                                        int permission) {
  auto lock = doc->NewIndirect<CPDF_Dictionary>();
  lock->SetNewFor<CPDF_Name>("Type", "SigFieldLock");
  lock->SetNewFor<CPDF_Name>("Action", FieldActionName(action));
  if (action != EPDF_SIG_FIELD_ACTION_ALL) {
    auto array = lock->SetNewFor<CPDF_Array>("Fields");
    for (const WideString& name : fields) {
      array->AppendNew<CPDF_String>(name.AsStringView());
    }
  }
  if (permission >= 1 && permission <= 3) {
    lock->SetNewFor<CPDF_Number>("P", permission);
  }
  return lock;
}

// Sets Ff ReadOnly on every terminal field the FieldMDP names (or all but
// the signing field for /All), through the reconciled form so recovered
// fields and same-name twins are covered.
void LockFields(CPDF_Document* doc,
                int action,
                const std::vector<WideString>& names,
                uint32_t signing_field_objnum) {
  std::unique_ptr<CPDF_InteractiveForm> form = epdf::BuildReconciledForm(doc);
  const size_t count = epdf::CountFormFields(*form);
  for (size_t i = 0; i < count; ++i) {
    CPDF_FormField* field = form->GetField(i, WideString());
    if (!field) {
      continue;
    }
    RetainPtr<const CPDF_Dictionary> dict = field->GetFieldDict();
    if (!dict || dict->GetObjNum() == 0 ||
        dict->GetObjNum() == signing_field_objnum) {
      continue;
    }
    const bool listed = Covers(names, field->GetFullName());
    const bool lock = action == EPDF_SIG_FIELD_ACTION_ALL ||
                      (action == EPDF_SIG_FIELD_ACTION_INCLUDE && listed) ||
                      (action == EPDF_SIG_FIELD_ACTION_EXCLUDE && !listed);
    if (!lock) {
      continue;
    }
    // Ff is inheritable: start from the effective flags, not the local
    // entry, so a field inheriting Multiline keeps it.
    const uint32_t flags = field->GetFieldFlags();
    if (flags & pdfium::form_flags::kReadOnly) {
      continue;
    }
    RetainPtr<CPDF_Dictionary> mutable_dict =
        ToDictionary(doc->GetMutableIndirectObject(dict->GetObjNum()));
    if (!mutable_dict) {
      continue;
    }
    mutable_dict->SetNewFor<CPDF_Number>(
        "Ff", static_cast<int>(flags | pdfium::form_flags::kReadOnly));
  }
}

// A retainable write stream collecting into a heap string.
class OwnedBufferWriteStream final : public IFX_RetainableWriteStream {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  bool WriteBlock(pdfium::span<const uint8_t> data) override {
    data_.insert(data_.end(), data.begin(), data.end());
    return true;
  }

  const DataVector<uint8_t>& data() const { return data_; }

 private:
  OwnedBufferWriteStream() = default;
  ~OwnedBufferWriteStream() override = default;

  DataVector<uint8_t> data_;
};

// Counts the bytes that pass through to |inner|: a file writer reports no
// size of its own, and the candidate's object span must lie within it.
class CountingWriteStream final : public IFX_RetainableWriteStream {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  bool WriteBlock(pdfium::span<const uint8_t> data) override {
    if (!inner_->WriteBlock(data)) {
      return false;
    }
    written_ += data.size();
    return true;
  }

  uint64_t written() const { return written_; }

 private:
  explicit CountingWriteStream(RetainPtr<IFX_RetainableWriteStream> inner)
      : inner_(std::move(inner)) {}
  ~CountingWriteStream() override = default;

  RetainPtr<IFX_RetainableWriteStream> inner_;
  uint64_t written_ = 0;
};

// Where the serializer wrote the signature value object: [obj_offset,
// obj_end) in the saved bytes.
struct CandidateSpan {
  FX_FILESIZE obj_offset = 0;
  FX_FILESIZE obj_end = 0;
};

// An incremental save of |doc| through |archive| (the base bytes stream
// through it, the candidate's revision follows), reporting the span object
// |sig_objnum| landed in. Shared by the buffer and file candidate saves.
bool SaveCandidateThrough(CPDF_Document* doc,
                          uint32_t sig_objnum,
                          RetainPtr<IFX_RetainableWriteStream> archive,
                          CandidateSpan* out_span) {
  std::map<uint32_t, FX_FILESIZE> offsets;
  FX_FILESIZE xref_start = 0;
  {
    // The creator buffers through its own archive and flushes on
    // destruction; the offsets are copied out before that.
    CPDF_Creator creator(doc, std::move(archive));
    if (const CPDF_Document::PendingSecurity* pending =
            doc->GetPendingSecurity()) {
      if (pending->mode == CPDF_Document::PendingSecurityMode::kEncrypt) {
        creator.SetEncryption(pending->encrypt_dict, pending->security_handler);
      } else if (pending->mode == CPDF_Document::PendingSecurityMode::kRemove) {
        return false;  // removing security is a rewrite, never a signing save
      }
    }
    if (!creator.Create(
            Mask<CPDF_Creator::CreateFlags>{CPDF_Creator::kIncremental}, 0)) {
      return false;
    }
    offsets = creator.object_offsets();
    xref_start = creator.xref_start();
  }
  const auto it = offsets.find(sig_objnum);
  if (it == offsets.end() || it->second <= 0) {
    return false;
  }
  const FX_FILESIZE obj_offset = it->second;
  FX_FILESIZE obj_end = xref_start;
  for (const auto& [num, offset] : offsets) {
    if (offset > obj_offset && offset < obj_end) {
      obj_end = offset;
    }
  }
  if (obj_end <= obj_offset) {
    return false;
  }
  out_span->obj_offset = obj_offset;
  out_span->obj_end = obj_end;
  return true;
}

bool SpanStartsWith(pdfium::span<const uint8_t> haystack,
                    size_t at,
                    const char* needle) {
  const size_t len = strlen(needle);
  if (at > haystack.size() || haystack.size() - at < len) {
    return false;
  }
  return memcmp(haystack.data() + at, needle, len) == 0;
}

}  // namespace

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFSig_Prepare(FPDF_DOCUMENT candidate,
                uint32_t field_objnum,
                const EPDF_SIG_PREPARE* opts) {
  ScopedFPDFDocumentView document_view(candidate);
  CPDF_Document* doc = document_view.Get();
  if (!doc || !doc->GetRoot() || field_objnum == 0 || !opts) {
    return 0;
  }
  // ---- Plan (const reads only). ----
  const char* subfilter = SubFilterName(opts->subfilter);
  const char* digest_method = DigestMethodName(opts->digest);
  const bool timestamp = opts->subfilter == EPDF_SIG_SUBFILTER_ETSI_RFC3161;
  if (!subfilter || !digest_method || opts->contents_size < 256 ||
      opts->contents_size > (4u << 20) || opts->docmdp_permission < 0 ||
      opts->docmdp_permission > 3 || opts->lock_permission < 0 ||
      opts->lock_permission > 3 ||
      opts->fieldmdp_action < EPDF_SIG_FIELD_ACTION_NONE ||
      opts->fieldmdp_action > EPDF_SIG_FIELD_ACTION_EXCLUDE) {
    return 0;
  }
  if (timestamp && (opts->docmdp_permission != 0 ||
                    opts->fieldmdp_action != EPDF_SIG_FIELD_ACTION_NONE ||
                    opts->lock_permission != 0)) {
    return 0;
  }
  if (opts->fieldmdp_field_count < 0 ||
      (opts->fieldmdp_field_count > 0 && !opts->fieldmdp_fields)) {
    return 0;
  }

  std::unique_ptr<SignatureModel> model = BuildModel(doc);
  const int index = EPDFSig_GetIndexByFieldObjNum(HandleFromModel(model.get()),
                                                  field_objnum);
  if (index < 0) {
    return 0;
  }
  const SignatureRecord& record = model->records[index];
  if (record.is_signed || record.value_objnum != 0) {
    return 0;
  }
  const bool any_signed = AnySigned(*model);
  // Saving a layer candidate appends an update to its BASE bytes and
  // rewrites the layer's objects into it; signed bytes that live in the
  // layer's loaded delta would not survive that. They must become a base
  // (the completion flow's seal) before another signature can follow.
  if (const CPDF_LayerDocument* layer = CPDF_LayerDocument::FromDocument(doc)) {
    if (layer->GetLoadedDeltaStream()) {
      const uint64_t base_size =
          static_cast<uint64_t>(layer->GetBaseDocument()->GetRawBaseSize());
      for (const SignatureRecord& other : model->records) {
        if (other.is_signed && other.has_byte_range &&
            other.byte_range[2] + other.byte_range[3] > base_size) {
          return 0;
        }
      }
    }
  }
  RetainPtr<const CPDF_Dictionary> field =
      epdf::ResolveFieldDict(doc, field_objnum);
  if (!field) {
    return 0;
  }
  RetainPtr<const CPDF_Object> ff =
      CPDF_FormField::GetFieldAttrForDict(field.Get(), pdfium::form_fields::kFf);
  if (ff && (static_cast<uint32_t>(ff->GetInteger()) &
             pdfium::form_flags::kReadOnly)) {
    return 0;
  }
  for (const SignatureRecord& other : model->records) {
    if (other.field_objnum != field_objnum && LocksField(other, record.fqn)) {
      return 0;
    }
  }
  const int in_force = PermissionInForce(*model);
  if (in_force == 1 && !timestamp) {
    return 0;
  }

  int certification = opts->docmdp_permission;
  if (record.sv_present & EPDF_SIG_SV_MDP) {
    if (record.sv_mdp == 0) {
      if (certification != 0) {
        return 0;  // the seed value asks for an approval signature only
      }
    } else if (certification != 0 && certification != record.sv_mdp) {
      return 0;
    } else {
      certification = record.sv_mdp;
    }
  }
  if (certification != 0) {
    const CPDF_Dictionary* root = doc->GetRoot();
    RetainPtr<const CPDF_Dictionary> perms = root->GetDictFor("Perms");
    if (timestamp || AnySigned(*model) ||
        (perms && perms->KeyExist("DocMDP"))) {
      return 0;
    }
  }

  if (record.has_seed_value) {
    const uint32_t required = record.sv_required;
    // /SV /V names the seed-value parser capability the signer must have:
    // 1 = the PDF 1.5 entries, 2 = the PDF 1.7 entries. This implementation
    // recognises the PDF 1.7 set; anything newer, when required, is refused.
    static constexpr int kSupportedSeedValueVersion = 2;
    if ((required & EPDF_SIG_SV_V) &&
        (!(record.sv_present & EPDF_SIG_SV_V) ||
         record.sv_version > kSupportedSeedValueVersion)) {
      return 0;
    }
    if ((required & EPDF_SIG_SV_FILTER) &&
        record.sv_filter != L"Adobe.PPKLite") {
      return 0;
    }
    if ((required & EPDF_SIG_SV_SUBFILTER) &&
        !Contains(record.sv_subfilters, WideString::FromUTF8(subfilter))) {
      return 0;
    }
    if ((required & EPDF_SIG_SV_DIGEST_METHOD) &&
        !Contains(record.sv_digest_methods,
                  WideString::FromUTF8(digest_method))) {
      return 0;
    }
    if (required & EPDF_SIG_SV_REASONS) {
      if (!opts->reason ||
          !Contains(record.sv_reasons,
                    WideStringFromFPDFWideString(opts->reason))) {
        return 0;
      }
    }
    if (required & (EPDF_SIG_SV_LEGAL_ATTESTATION | EPDF_SIG_SV_ADD_REV_INFO |
                    EPDF_SIG_SV_LOCK_DOCUMENT |
                    EPDF_SIG_SV_APPEARANCE_FILTER)) {
      return 0;
    }
    RetainPtr<const CPDF_Dictionary> sv = field->GetDictFor("SV");
    RetainPtr<const CPDF_Dictionary> cert = sv ? sv->GetDictFor("Cert") : nullptr;
    if (cert && cert->GetIntegerFor("Ff") != 0) {
      return 0;
    }
    RetainPtr<const CPDF_Dictionary> ts =
        sv ? sv->GetDictFor("TimeStamp") : nullptr;
    if (ts && ts->GetIntegerFor("Ff") != 0) {
      return 0;
    }
  }

  // The effective FieldMDP: a /Lock already on the field is applied as is
  // (ISO 32000-2 12.7.5.5: the transform parameters are derived from it);
  // the request may repeat it but not contradict it. Without a /Lock the
  // request's values are used and mirrored into a new /Lock.
  int fieldmdp_action = opts->fieldmdp_action;
  std::vector<WideString> fieldmdp_fields =
      WideStrings(opts->fieldmdp_fields, opts->fieldmdp_field_count);
  int lock_permission = opts->lock_permission;
  const bool field_has_lock = record.lock_action != EPDF_SIG_FIELD_ACTION_NONE ||
                              record.lock_permission != 0;
  if (record.lock_action != EPDF_SIG_FIELD_ACTION_NONE) {
    if (fieldmdp_action == EPDF_SIG_FIELD_ACTION_NONE) {
      fieldmdp_action = record.lock_action;
      fieldmdp_fields = record.lock_fields;
    } else if (fieldmdp_action != record.lock_action ||
               !SameNameSet(fieldmdp_fields, record.lock_fields)) {
      return 0;
    }
  }
  if (record.lock_permission != 0) {
    if (lock_permission == 0) {
      lock_permission = record.lock_permission;
    } else if (lock_permission != record.lock_permission) {
      return 0;
    }
  }
  if (timestamp && fieldmdp_action != EPDF_SIG_FIELD_ACTION_NONE) {
    return 0;  // a locked field cannot host a document timestamp
  }

  // ---- Apply. ----
  RetainPtr<CPDF_Dictionary> mutable_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
  RetainPtr<CPDF_Dictionary> acro_form =
      root ? root->GetMutableDictFor("AcroForm") : nullptr;
  if (!mutable_field || !acro_form) {
    return 0;
  }

  auto value = doc->NewIndirect<CPDF_Dictionary>();
  value->SetNewFor<CPDF_Name>("Type", timestamp ? "DocTimeStamp" : "Sig");
  value->SetNewFor<CPDF_Name>("Filter", "Adobe.PPKLite");
  value->SetNewFor<CPDF_Name>("SubFilter", subfilter);
  auto byte_range = value->SetNewFor<CPDF_Array>("ByteRange");
  byte_range->AppendNew<CPDF_Number>(0);
  for (int i = 0; i < 3; ++i) {
    byte_range->AppendNew<CPDF_Number>(kByteRangeSentinel);
  }
  DataVector<uint8_t> zeros(opts->contents_size, 0);
  value->SetNewFor<CPDF_String>("Contents", pdfium::span<const uint8_t>(zeros),
                                CPDF_String::DataType::kIsHex);
  value->SetNewFor<CPDF_String>(
      "M", opts->signing_time ? ByteString(opts->signing_time) : NowAsPdfDate());
  const std::pair<const char*, FPDF_WIDESTRING> strings[] = {
      {"Name", opts->name},
      {"Reason", opts->reason},
      {"Location", opts->location},
      {"ContactInfo", opts->contact_info},
  };
  for (const auto& [key, text] : strings) {
    if (text) {
      value->SetNewFor<CPDF_String>(
          key, WideStringFromFPDFWideString(text).AsStringView());
    }
  }

  // Every value of a byte-range signature dictionary is a direct object
  // (ISO 32000-2 12.8.1), the reference dictionaries included.
  if (certification != 0 || fieldmdp_action != EPDF_SIG_FIELD_ACTION_NONE) {
    auto references = value->SetNewFor<CPDF_Array>("Reference");
    if (certification != 0) {
      auto ref = references->AppendNew<CPDF_Dictionary>();
      ref->SetNewFor<CPDF_Name>("Type", "SigRef");
      ref->SetNewFor<CPDF_Name>("TransformMethod", "DocMDP");
      auto params = ref->SetNewFor<CPDF_Dictionary>("TransformParams");
      params->SetNewFor<CPDF_Name>("Type", "TransformParams");
      params->SetNewFor<CPDF_Name>("V", "1.2");
      params->SetNewFor<CPDF_Number>("P", certification);
    }
    if (fieldmdp_action != EPDF_SIG_FIELD_ACTION_NONE) {
      auto ref = references->AppendNew<CPDF_Dictionary>();
      ref->SetNewFor<CPDF_Name>("Type", "SigRef");
      ref->SetNewFor<CPDF_Name>("TransformMethod", "FieldMDP");
      auto params = ref->SetNewFor<CPDF_Dictionary>("TransformParams");
      params->SetNewFor<CPDF_Name>("Type", "TransformParams");
      params->SetNewFor<CPDF_Name>("V", "1.2");
      params->SetNewFor<CPDF_Name>("Action", FieldActionName(fieldmdp_action));
      if (fieldmdp_action != EPDF_SIG_FIELD_ACTION_ALL) {
        auto fields = params->SetNewFor<CPDF_Array>("Fields");
        for (const WideString& name : fieldmdp_fields) {
          fields->AppendNew<CPDF_String>(name.AsStringView());
        }
      }
    }
  }

  mutable_field->SetNewFor<CPDF_Reference>(pdfium::form_fields::kV, doc,
                                           value->GetObjNum());
  // The /Lock mirror is authoring-time metadata, written only while no
  // signature is in place. Once one is, adding a key to an existing field
  // dictionary is a change no earlier signature permits (pyHanko and
  // Acrobat reject it); the FieldMDP transform above carries the lock.
  if (!field_has_lock && !any_signed &&
      (fieldmdp_action != EPDF_SIG_FIELD_ACTION_NONE ||
       lock_permission != 0)) {
    RetainPtr<CPDF_Dictionary> lock = MakeLockDict(
        doc,
        fieldmdp_action != EPDF_SIG_FIELD_ACTION_NONE
            ? fieldmdp_action
            : EPDF_SIG_FIELD_ACTION_INCLUDE,
        fieldmdp_fields, lock_permission);
    mutable_field->SetNewFor<CPDF_Reference>("Lock", doc, lock->GetObjNum());
  }
  if (fieldmdp_action != EPDF_SIG_FIELD_ACTION_NONE) {
    LockFields(doc, fieldmdp_action, fieldmdp_fields, field_objnum);
  }
  if (certification != 0) {
    RetainPtr<CPDF_Dictionary> perms = root->GetOrCreateDictFor("Perms");
    perms->SetNewFor<CPDF_Reference>("DocMDP", doc, value->GetObjNum());
  }
  acro_form->SetNewFor<CPDF_Number>("SigFlags",
                                    acro_form->GetIntegerFor("SigFlags") | 3);
  return value->GetObjNum();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_SetFieldLock(FPDF_DOCUMENT document,
                     uint32_t field_objnum,
                     int action,
                     const FPDF_WIDESTRING* fields,
                     int field_count,
                     int permission) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc || field_objnum == 0 || action < EPDF_SIG_FIELD_ACTION_NONE ||
      action > EPDF_SIG_FIELD_ACTION_EXCLUDE || permission < 0 ||
      permission > 3 || field_count < 0 || (field_count > 0 && !fields)) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> field =
      epdf::ResolveFieldDict(doc, field_objnum);
  RetainPtr<const CPDF_Object> ft =
      field ? CPDF_FormField::GetFieldAttrForDict(field.Get(),
                                                  pdfium::form_fields::kFT)
            : nullptr;
  if (!ft || ft->GetString() != pdfium::form_fields::kSig ||
      field->KeyExist(pdfium::form_fields::kV)) {
    return false;
  }
  // Authoring time only. Once any signature is in place, adding or removing
  // a /Lock is a change to a field dictionary that no earlier signature
  // permits: strict validators flag it as an illegitimate modification.
  if (AnySigned(*BuildModel(doc))) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> mutable_field =
      ToDictionary(doc->GetMutableIndirectObject(field_objnum));
  if (!mutable_field) {
    return false;
  }
  if (action == EPDF_SIG_FIELD_ACTION_NONE) {
    mutable_field->RemoveFor("Lock");
    return true;
  }
  RetainPtr<CPDF_Dictionary> lock =
      MakeLockDict(doc, action, WideStrings(fields, field_count), permission);
  mutable_field->SetNewFor<CPDF_Reference>("Lock", doc, lock->GetObjNum());
  return true;
}

FPDF_EXPORT void* FPDF_CALLCONV
EPDFSig_SaveCandidateToOwnedBuffer(FPDF_DOCUMENT candidate,
                                   uint32_t sig_objnum,
                                   unsigned long long* out_size,
                                   unsigned long long* out_obj_offset,
                                   unsigned long long* out_obj_len) {
  if (out_size) {
    *out_size = 0;
  }
  if (out_obj_offset) {
    *out_obj_offset = 0;
  }
  if (out_obj_len) {
    *out_obj_len = 0;
  }
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(candidate);
  if (!doc || sig_objnum == 0 || !out_size || !out_obj_offset || !out_obj_len) {
    return nullptr;
  }
  auto writer = pdfium::MakeRetain<OwnedBufferWriteStream>();
  CandidateSpan span;
  if (!SaveCandidateThrough(doc, sig_objnum, writer, &span)) {
    return nullptr;
  }
  const DataVector<uint8_t>& data = writer->data();
  if (static_cast<size_t>(span.obj_end) > data.size()) {
    return nullptr;
  }
  // malloc so EPDF_FreeBuffer() (free) releases it.
  void* buffer = malloc(data.size());
  if (!buffer) {
    return nullptr;
  }
  memcpy(buffer, data.data(), data.size());
  *out_size = data.size();
  *out_obj_offset = static_cast<unsigned long long>(span.obj_offset);
  *out_obj_len = static_cast<unsigned long long>(span.obj_end - span.obj_offset);
  return buffer;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_SaveCandidate(FPDF_DOCUMENT candidate,
                      uint32_t sig_objnum,
                      FPDF_FILEWRITE* file_write,
                      unsigned long long* out_size,
                      unsigned long long* out_obj_offset,
                      unsigned long long* out_obj_len) {
  if (out_size) {
    *out_size = 0;
  }
  if (out_obj_offset) {
    *out_obj_offset = 0;
  }
  if (out_obj_len) {
    *out_obj_len = 0;
  }
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(candidate);
  if (!doc || sig_objnum == 0 || !file_write || !out_size || !out_obj_offset ||
      !out_obj_len) {
    return false;
  }
  auto writer = pdfium::MakeRetain<CountingWriteStream>(
      pdfium::MakeRetain<CPDFSDK_FileWriteAdapter>(file_write));
  CandidateSpan span;
  if (!SaveCandidateThrough(doc, sig_objnum, writer, &span)) {
    return false;
  }
  const uint64_t size = writer->written();
  if (static_cast<uint64_t>(span.obj_end) > size) {
    return false;
  }
  *out_size = size;
  *out_obj_offset = static_cast<unsigned long long>(span.obj_offset);
  *out_obj_len = static_cast<unsigned long long>(span.obj_end - span.obj_offset);
  return true;
}

namespace {

// What sealing a candidate decided: the /ByteRange it patched in and where
// the zero-filled /Contents hex lives, all as absolute file offsets.
struct SealPlan {
  uint64_t r1 = 0;               // bytes [0, r1) precede '<'
  uint64_t r2 = 0;               // bytes [r2, r2 + r3) follow '>'
  uint64_t r3 = 0;
  uint64_t contents_offset = 0;  // first hex digit
  uint64_t hex_len = 0;
};

// Locate the sentinel /ByteRange and the zero-filled /Contents in |object|
// (the signature value object, starting at file offset |obj_offset| in a
// file of |file_length| bytes) and patch the real range in place. Shared by
// the whole-buffer seal and the span seal; hashes nothing.
std::optional<SealPlan> PatchSealPlaceholders(pdfium::span<uint8_t> object,
                                              uint64_t obj_offset,
                                              uint64_t file_length) {
  // Scan the object as PDF syntax: the placeholders are the top-level
  // /ByteRange and /Contents entries, never the same text inside a string
  // value such as /ContactInfo.
  std::optional<size_t> dict = pdfscan::FindObjectDictionary(object);
  if (!dict.has_value()) {
    return std::nullopt;
  }
  std::optional<std::pair<size_t, size_t>> range_value =
      pdfscan::FindEntry(object, dict.value(), "ByteRange");
  std::optional<std::pair<size_t, size_t>> contents_value =
      pdfscan::FindEntry(object, dict.value(), "Contents");
  if (!range_value.has_value() || !contents_value.has_value()) {
    return std::nullopt;
  }
  // The sentinel array body, exactly as the serializer wrote it.
  const size_t body_start = range_value->first + 1;
  const size_t body_len = strlen(kSentinelBody);
  if (object[range_value->first] != '[' ||
      range_value->second != body_start + body_len + 1 ||
      !SpanStartsWith(object, body_start, kSentinelBody) ||
      object[body_start + body_len] != ']') {
    return std::nullopt;
  }
  // The zero-filled hex string.
  const size_t hex_start = contents_value->first + 1;
  if (object[contents_value->first] != '<' ||
      contents_value->second < hex_start + 3 ||
      object[contents_value->second - 1] != '>') {
    return std::nullopt;
  }
  const size_t hex_len = contents_value->second - 1 - hex_start;
  if (hex_len % 2 != 0) {
    return std::nullopt;
  }
  for (size_t i = 0; i < hex_len; ++i) {
    if (object[hex_start + i] != '0') {
      return std::nullopt;
    }
  }

  SealPlan plan;
  plan.contents_offset = obj_offset + hex_start;
  plan.r1 = plan.contents_offset - 1;              // up to '<'
  plan.r2 = plan.contents_offset + hex_len + 1;    // past '>'
  if (plan.r2 > file_length) {
    return std::nullopt;
  }
  plan.r3 = file_length - plan.r2;
  plan.hex_len = hex_len;
  const ByteString patched =
      ByteString::Format("0 %llu %llu %llu", plan.r1, plan.r2, plan.r3);
  if (patched.GetLength() > body_len) {
    return std::nullopt;
  }
  auto body = object.subspan(body_start, body_len);
  std::fill(body.begin(), body.end(), ' ');
  std::copy(patched.span().begin(), patched.span().end(), body.begin());
  return plan;
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_Seal(unsigned char* buffer,
             unsigned long long length,
             unsigned long long obj_offset,
             unsigned long long obj_len,
             int algorithm,
             unsigned long long out_range[4],
             unsigned long long* out_contents_offset,
             unsigned long long* out_contents_hex_len,
             unsigned char* out_digest,
             unsigned long* inout_len) {
  if (!buffer || !out_range || !out_contents_offset || !out_contents_hex_len ||
      !inout_len || obj_offset >= length || obj_len > length - obj_offset) {
    return false;
  }
  std::optional<size_t> digest_size = Hasher::DigestSize(algorithm);
  if (!digest_size.has_value()) {
    return false;
  }
  if (!out_digest || *inout_len < digest_size.value()) {
    *inout_len = static_cast<unsigned long>(digest_size.value());
    return false;
  }
  // SAFETY: caller provides |length| bytes.
  auto file = UNSAFE_BUFFERS(pdfium::span(buffer, static_cast<size_t>(length)));
  auto object = file.subspan(static_cast<size_t>(obj_offset),
                             static_cast<size_t>(obj_len));
  std::optional<SealPlan> plan = PatchSealPlaceholders(object, obj_offset, length);
  if (!plan.has_value()) {
    return false;
  }

  Hasher hasher(algorithm);
  hasher.Update(file.first(static_cast<size_t>(plan->r1)));
  hasher.Update(file.subspan(static_cast<size_t>(plan->r2)));
  // SAFETY: capacity checked above.
  hasher.Finish(UNSAFE_BUFFERS(pdfium::span(out_digest, digest_size.value())));
  *inout_len = static_cast<unsigned long>(digest_size.value());

  // SAFETY: caller provides four slots.
  auto range = UNSAFE_BUFFERS(pdfium::span(out_range, 4u));
  range[0] = 0;
  range[1] = plan->r1;
  range[2] = plan->r2;
  range[3] = plan->r3;
  *out_contents_offset = plan->contents_offset;
  *out_contents_hex_len = plan->hex_len;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_SealSpan(unsigned char* span,
                 unsigned long long span_len,
                 unsigned long long obj_offset,
                 unsigned long long file_length,
                 unsigned long long out_range[4],
                 unsigned long long* out_contents_offset,
                 unsigned long long* out_contents_hex_len) {
  if (!span || !out_range || !out_contents_offset || !out_contents_hex_len ||
      obj_offset >= file_length || span_len > file_length - obj_offset) {
    return false;
  }
  // SAFETY: caller provides |span_len| bytes.
  auto object =
      UNSAFE_BUFFERS(pdfium::span(span, static_cast<size_t>(span_len)));
  std::optional<SealPlan> plan =
      PatchSealPlaceholders(object, obj_offset, file_length);
  if (!plan.has_value()) {
    return false;
  }
  // SAFETY: caller provides four slots.
  auto range = UNSAFE_BUFFERS(pdfium::span(out_range, 4u));
  range[0] = 0;
  range[1] = plan->r1;
  range[2] = plan->r2;
  range[3] = plan->r3;
  *out_contents_offset = plan->contents_offset;
  *out_contents_hex_len = plan->hex_len;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFSig_WriteContents(unsigned char* buffer,
                      unsigned long long length,
                      unsigned long long contents_offset,
                      unsigned long long contents_hex_len,
                      const unsigned char* der,
                      unsigned long der_len) {
  if (!buffer || !der || der_len == 0 || contents_offset >= length ||
      contents_hex_len > length - contents_offset ||
      2ull * der_len > contents_hex_len) {
    return false;
  }
  // SAFETY: caller provides |der_len| bytes.
  auto der_span = UNSAFE_BUFFERS(pdfium::span(der, static_cast<size_t>(der_len)));
  std::optional<size_t> declared = DerOuterLength(der_span);
  if (!declared.has_value() || declared.value() != der_len) {
    return false;
  }
  // SAFETY: bounds checked above.
  auto hex = UNSAFE_BUFFERS(pdfium::span(buffer + contents_offset,
                                         static_cast<size_t>(contents_hex_len)));
  static constexpr char kHex[] = "0123456789abcdef";
  size_t pos = 0;
  for (uint8_t byte : der_span) {
    hex[pos++] = kHex[byte >> 4];
    hex[pos++] = kHex[byte & 0x0f];
  }
  std::fill(hex.begin() + pos, hex.end(), '0');
  return true;
}
