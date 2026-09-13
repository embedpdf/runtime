// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_signature.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/fdrm/fx_crypt_sha.h"
#include "core/fxcrt/data_vector.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_form.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdf_signature.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

std::wstring ReadModelText(unsigned long (*getter)(EPDF_SIGNATURE_MODEL,
                                                   int,
                                                   int,
                                                   FPDF_WCHAR*,
                                                   unsigned long),
                           EPDF_SIGNATURE_MODEL model,
                           int index,
                           int key) {
  const unsigned long len = getter(model, index, key, nullptr, 0);
  if (len == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer(len / sizeof(FPDF_WCHAR));
  EXPECT_EQ(len, getter(model, index, key, buffer.data(), len));
  return GetPlatformWString(buffer.data());
}

std::wstring ReadFieldName(EPDF_SIGNATURE_MODEL model, int index) {
  const unsigned long len = EPDFSig_GetFieldName(model, index, nullptr, 0);
  if (len == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer(len / sizeof(FPDF_WCHAR));
  EXPECT_EQ(len, EPDFSig_GetFieldName(model, index, buffer.data(), len));
  return GetPlatformWString(buffer.data());
}

std::wstring ReadFieldNameAt(EPDF_SIGNATURE_MODEL model, int index, int which, int name_index) {
  const unsigned long len = EPDFSig_GetFieldNameAt(model, index, which, name_index, nullptr, 0);
  if (len == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer(len / sizeof(FPDF_WCHAR));
  EXPECT_EQ(len, EPDFSig_GetFieldNameAt(model, index, which, name_index, buffer.data(), len));
  return GetPlatformWString(buffer.data());
}

std::string HexDigest(FPDF_DOCUMENT document,
                      const unsigned long long range[4],
                      int algorithm) {
  unsigned char digest[64];
  unsigned long len = sizeof(digest);
  if (!EPDFSig_DigestByteRange(document, range, algorithm, digest, &len)) {
    return std::string();
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (unsigned long i = 0; i < len; ++i) {
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0x0f]);
  }
  return out;
}

struct DiffRow {
  unsigned int obj_num = 0;
  int change = -1;
  int kind = -1;
  int old_gen = -1;
  int new_gen = -1;
  bool stream_data_changed = false;
};

std::vector<DiffRow> ReadDiffRows(EPDF_OBJECT_DIFF diff) {
  std::vector<DiffRow> rows;
  const int count = EPDFObjectDiff_GetCount(diff);
  for (int i = 0; i < count; ++i) {
    DiffRow row;
    FPDF_BOOL changed = false;
    EXPECT_TRUE(EPDFObjectDiff_GetEntry(diff, i, &row.obj_num, &row.change,
                                        &row.kind, &row.old_gen, &row.new_gen,
                                        &changed));
    row.stream_data_changed = !!changed;
    rows.push_back(row);
  }
  return rows;
}

int FindRow(const std::vector<DiffRow>& rows, unsigned int obj_num) {
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].obj_num == obj_num) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::string ReadDiffValue(EPDF_OBJECT_DIFF diff, int index, int which) {
  FPDF_BOOL truncated = false;
  const unsigned long len =
      EPDFObjectDiff_GetValue(diff, index, which, nullptr, 0, &truncated);
  EXPECT_FALSE(truncated);
  if (len == 0) {
    return std::string();
  }
  std::vector<char> buffer(len);
  EXPECT_EQ(len, EPDFObjectDiff_GetValue(diff, index, which, buffer.data(), len,
                                         &truncated));
  return std::string(buffer.data());
}

// "parent:label" for every inbound reference, sorted.
std::vector<std::string> ReadReferrers(EPDF_OBJECT_DIFF diff,
                                       int which,
                                       unsigned int obj_num) {
  std::vector<std::string> out;
  const int count = EPDFObjectDiff_GetReferrerCount(diff, which, obj_num);
  for (int i = 0; i < count; ++i) {
    unsigned int parent = 0;
    const unsigned long len = EPDFObjectDiff_GetReferrer(diff, which, obj_num,
                                                         i, &parent, nullptr, 0);
    std::vector<char> buffer(len);
    EXPECT_EQ(len, EPDFObjectDiff_GetReferrer(diff, which, obj_num, i, &parent,
                                              buffer.data(), len));
    out.push_back(std::to_string(parent) + ":" + std::string(buffer.data()));
  }
  std::sort(out.begin(), out.end());
  return out;
}

class EPDFSignatureEmbedderTest : public EmbedderTest {};

}  // namespace

TEST_F(EPDFSignatureEmbedderTest, BadArguments) {
  EXPECT_EQ(-1, EPDFDoc_GetRevisionCount(nullptr));
  EXPECT_FALSE(EPDFDoc_GetRevision(nullptr, 0, nullptr, nullptr));
  EXPECT_FALSE(EPDFDoc_OpenRevision(nullptr, 0));
  EXPECT_FALSE(EPDFSig_LoadModel(nullptr));
  EXPECT_EQ(-1, EPDFSig_Count(nullptr));
  EXPECT_FALSE(EPDFSig_IsRevisionChainValid(nullptr));
  EXPECT_EQ(0u, EPDFSig_GetFieldObjNum(nullptr, 0));
  EXPECT_EQ(EPDF_SIG_COVERAGE_MALFORMED, EPDFSig_GetCoverage(nullptr, 0));
  EXPECT_EQ(-1, EPDFSig_GetFieldNameCount(nullptr, 0, EPDF_SIG_FIELDS_LOCK));
  unsigned long len = 64;
  unsigned char digest[64];
  const unsigned long long range[4] = {0, 1, 2, 1};
  EXPECT_FALSE(EPDFSig_DigestByteRange(nullptr, range, EPDF_DIGEST_SHA256,
                                       digest, &len));
  EPDFSig_CloseModel(nullptr);  // must be a no-op
}

TEST_F(EPDFSignatureEmbedderTest, SingleRevisionNoSignatures) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_EQ(1, EPDFDoc_GetRevisionCount(document()));
  unsigned long long end = 0;
  unsigned long long xref = 0;
  ASSERT_TRUE(EPDFDoc_GetRevision(document(), 0, &end, &xref));
  EXPECT_GT(end, xref);
  EXPECT_GT(xref, 0u);
  // Out-of-range indexes fail and zero the out-params.
  unsigned long long scratch_end = 1;
  unsigned long long scratch_xref = 1;
  EXPECT_FALSE(EPDFDoc_GetRevision(document(), 1, &scratch_end, &scratch_xref));
  EXPECT_EQ(0u, scratch_end);
  EXPECT_EQ(0u, scratch_xref);
  EXPECT_FALSE(EPDFDoc_GetRevision(document(), -1, nullptr, nullptr));

  // A revision end that is not a boundary never opens.
  EXPECT_FALSE(EPDFDoc_OpenRevision(document(), end - 1));
  ScopedFPDFDocument prefix(EPDFDoc_OpenRevision(document(), end));
  ASSERT_TRUE(prefix);
  EXPECT_EQ(1, FPDF_GetPageCount(prefix.get()));

  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_TRUE(EPDFSig_IsRevisionChainValid(model));
  EXPECT_EQ(0, EPDFSig_Count(model));
  EXPECT_EQ(-1, EPDFSig_GetIndexByFieldObjNum(model, 1));
  EPDFSig_CloseModel(model);

  // Digest of the whole file in two halves equals SHA-256 of the file, which
  // the two-call length protocol reports as 32 bytes.
  unsigned long needed = 0;
  const unsigned long long whole[4] = {0, end / 2, end / 2, end - end / 2};
  EXPECT_FALSE(EPDFSig_DigestByteRange(document(), whole, EPDF_DIGEST_SHA256,
                                       nullptr, &needed));
  EXPECT_EQ(32u, needed);
  EXPECT_EQ(64u, HexDigest(document(), whole, EPDF_DIGEST_SHA256).size());
  EXPECT_EQ(40u, HexDigest(document(), whole, EPDF_DIGEST_SHA1).size());
  EXPECT_EQ(96u, HexDigest(document(), whole, EPDF_DIGEST_SHA384).size());
  EXPECT_EQ(128u, HexDigest(document(), whole, EPDF_DIGEST_SHA512).size());
  // Ranges past the file, overlapping, or out of order are refused.
  const unsigned long long beyond[4] = {0, end, end, 1};
  EXPECT_TRUE(HexDigest(document(), beyond, EPDF_DIGEST_SHA256).empty());
  const unsigned long long overlap[4] = {0, 10, 5, 10};
  EXPECT_TRUE(HexDigest(document(), overlap, EPDF_DIGEST_SHA256).empty());
  EXPECT_TRUE(HexDigest(document(), whole, 99).empty());
}

TEST_F(EPDFSignatureEmbedderTest, UnchainedUpdatesAreOneRevision) {
  // The upstream fixture's incremental updates carry complete cross-reference
  // tables and no /Prev, so nothing but the final section is reachable from
  // startxref: the chain is the authority, and that is one revision.
  ASSERT_TRUE(OpenDocument("two_signatures.pdf"));
  EXPECT_EQ(1, EPDFDoc_GetRevisionCount(document()));
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_TRUE(EPDFSig_IsRevisionChainValid(model));
  ASSERT_EQ(2, EPDFSig_Count(model));
  // A BER indefinite-length /Contents without end-of-contents octets is
  // not a complete object: MALFORMED, and no DER is returned.
  EXPECT_EQ(EPDF_SIG_COVERAGE_MALFORMED, EPDFSig_GetCoverage(model, 0));
  EXPECT_EQ(0u, EPDFSig_GetContents(model, 0, nullptr, 0));
  EPDFSig_CloseModel(model);
}

TEST_F(EPDFSignatureEmbedderTest, ThreeRevisionsTwoSignatures) {
  // two_signatures.in with chained /Prev trailers: original + two incremental
  // updates, each adding a signature whose /ByteRange is a placeholder and
  // whose /Contents is a small definite-length DER object.
  ASSERT_TRUE(OpenDocument("signature_chain.pdf"));
  ASSERT_EQ(3, EPDFDoc_GetRevisionCount(document()));
  unsigned long long ends[3];
  for (int i = 0; i < 3; ++i) {
    unsigned long long xref = 0;
    ASSERT_TRUE(EPDFDoc_GetRevision(document(), i, &ends[i], &xref));
    EXPECT_LT(xref, ends[i]);
    if (i > 0) {
      EXPECT_LT(ends[i - 1], ends[i]);
    }
  }

  // Each prefix is a complete document with the signatures of its time.
  {
    ScopedFPDFDocument rev0(EPDFDoc_OpenRevision(document(), ends[0]));
    ASSERT_TRUE(rev0);
    EXPECT_EQ(1, FPDF_GetPageCount(rev0.get()));
    EXPECT_EQ(0, FPDF_GetSignatureCount(rev0.get()));
    EXPECT_EQ(1, EPDFDoc_GetRevisionCount(rev0.get()));
  }
  {
    ScopedFPDFDocument rev1(EPDFDoc_OpenRevision(document(), ends[1]));
    ASSERT_TRUE(rev1);
    EXPECT_EQ(1, FPDF_GetSignatureCount(rev1.get()));
    EXPECT_EQ(2, EPDFDoc_GetRevisionCount(rev1.get()));
  }
  EXPECT_EQ(2, FPDF_GetSignatureCount(document()));

  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_TRUE(EPDFSig_IsRevisionChainValid(model));
  ASSERT_EQ(2, EPDFSig_Count(model));
  for (int i = 0; i < 2; ++i) {
    EXPECT_TRUE(EPDFSig_IsSigned(model, i));
    EXPECT_EQ(EPDF_SIG_KIND_SIGNATURE, EPDFSig_GetKind(model, i));
    EXPECT_NE(0u, EPDFSig_GetFieldObjNum(model, i));
    EXPECT_EQ(i, EPDFSig_GetIndexByFieldObjNum(
                     model, EPDFSig_GetFieldObjNum(model, i)));
    unsigned long long range[4];
    ASSERT_TRUE(EPDFSig_GetByteRange(model, i, range));
    EXPECT_EQ(0u, range[0]);
    EXPECT_EQ(i == 0 ? 10u : 40u, range[1]);
    EXPECT_EQ(i == 0 ? 30u : 50u, range[2]);
    EXPECT_EQ(10u, range[3]);
    // Well-formed ranges that are not this signature's revision: PARTIAL.
    EXPECT_EQ(EPDF_SIG_COVERAGE_PARTIAL, EPDFSig_GetCoverage(model, i));
    EXPECT_EQ(-1, EPDFSig_GetRevisionIndex(model, i));
    // The DER object is returned exactly as long as its TLV declares.
    EXPECT_EQ(13u, EPDFSig_GetContents(model, i, nullptr, 0));
    EXPECT_EQ(L"ETSI.CAdES.detached",
              ReadModelText(EPDFSig_GetString, model, i,
                            EPDF_SIG_STRING_SUBFILTER));
    EXPECT_EQ(L"Adobe.PPKMS", ReadModelText(EPDFSig_GetString, model, i,
                                            EPDF_SIG_STRING_FILTER));
    EXPECT_EQ(0, EPDFSig_GetDocMDPPermission(model, i));
    EXPECT_FALSE(EPDFSig_IsCatalogCertification(model, i));
    EXPECT_EQ(EPDF_SIG_FIELD_ACTION_NONE, EPDFSig_GetFieldMDPAction(model, i));
    EXPECT_EQ(EPDF_SIG_FIELD_ACTION_NONE, EPDFSig_GetLockAction(model, i));
    EXPECT_FALSE(EPDFSig_HasSeedValue(model, i));
  }
  EXPECT_EQ(L"D:20200624093114+02'00'",
            ReadModelText(EPDFSig_GetString, model, 0, EPDF_SIG_STRING_M));
  EXPECT_EQ(L"D:20200624093118+02'00'",
            ReadModelText(EPDFSig_GetString, model, 1, EPDF_SIG_STRING_M));
  EXPECT_EQ(0u, ReadModelText(EPDFSig_GetString, model, 0,
                              EPDF_SIG_STRING_REASON)
                    .size());
  EXPECT_EQ(EPDF_SIG_COVERAGE_MALFORMED, EPDFSig_GetCoverage(model, 2));
  EPDFSig_CloseModel(model);
}

// A ValidSign-produced document: PDF 1.6, AES-128 with an empty user
// password, four chained cross-reference-stream revisions with object
// streams, two adbe.pkcs7.detached approval signatures (the first seals the
// original revision, the second the last one and carries a FieldMDP lock),
// and two unsigned form-fill revisions in between. Not part of the tree:
// the test is skipped when the file is absent.
TEST_F(EPDFSignatureEmbedderTest, EncryptedTwoSignaturesFourRevisions) {
  if (!OpenDocument("embedpdf_two_signatures_encrypted.pdf")) {
    GTEST_SKIP() << "fixture not available";
  }
  ASSERT_EQ(4, EPDFDoc_GetRevisionCount(document()));
  const unsigned long long kExpectedEnds[4] = {313124, 333857, 353951, 655271};
  for (int i = 0; i < 4; ++i) {
    unsigned long long end = 0;
    unsigned long long xref = 0;
    ASSERT_TRUE(EPDFDoc_GetRevision(document(), i, &end, &xref));
    EXPECT_EQ(kExpectedEnds[i], end) << "revision " << i;
    EXPECT_LT(xref, end);
  }

  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(document());
  ASSERT_TRUE(model);
  EXPECT_TRUE(EPDFSig_IsRevisionChainValid(model));
  ASSERT_EQ(2, EPDFSig_Count(model));

  // Signature 1: seals revision 0 (the file was produced and signed in one
  // save). Its digest matches the CMS messageDigest computed offline.
  int first = -1;
  int second = -1;
  for (int i = 0; i < 2; ++i) {
    if (ReadFieldName(model, i) == L"PriXGPsZ4kk9") {
      first = i;
    } else if (ReadFieldName(model, i) == L"faJaNXPG7AgX") {
      second = i;
    }
  }
  ASSERT_NE(-1, first);
  ASSERT_NE(-1, second);

  EXPECT_TRUE(EPDFSig_IsSigned(model, first));
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, EPDFSig_GetCoverage(model, first));
  EXPECT_EQ(0, EPDFSig_GetRevisionIndex(model, first));
  EXPECT_EQ(23527u, EPDFSig_GetContents(model, first, nullptr, 0));
  EXPECT_EQ(L"adbe.pkcs7.detached",
            ReadModelText(EPDFSig_GetString, model, first,
                          EPDF_SIG_STRING_SUBFILTER));
  EXPECT_EQ(EPDF_SIG_FIELD_ACTION_NONE,
            EPDFSig_GetFieldMDPAction(model, first));
  unsigned long long range1[4];
  ASSERT_TRUE(EPDFSig_GetByteRange(model, first, range1));
  EXPECT_EQ(313124u, range1[2] + range1[3]);
  EXPECT_EQ("fabf721af36ed9f3f1ef8c613258fa99b128bbf86473affae41f1c93543b4694",
            HexDigest(document(), range1, EPDF_DIGEST_SHA256));

  // Signature 2: seals revision 3, FieldMDP /Include over two fields, /Lock
  // on the field, /Name and /Reason decrypted from the encrypted strings.
  EXPECT_TRUE(EPDFSig_IsSigned(model, second));
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION,
            EPDFSig_GetCoverage(model, second));
  EXPECT_EQ(3, EPDFSig_GetRevisionIndex(model, second));
  EXPECT_EQ(22879u, EPDFSig_GetContents(model, second, nullptr, 0));
  EXPECT_EQ(EPDF_SIG_FIELD_ACTION_INCLUDE,
            EPDFSig_GetFieldMDPAction(model, second));
  EXPECT_EQ(2, EPDFSig_GetFieldNameCount(model, second,
                                         EPDF_SIG_FIELDS_FIELDMDP));
  EXPECT_NE(EPDF_SIG_FIELD_ACTION_NONE, EPDFSig_GetLockAction(model, second));
  EXPECT_EQ(0, EPDFSig_GetDocMDPPermission(model, second));
  EXPECT_FALSE(EPDFSig_IsCatalogCertification(model, second));
  EXPECT_EQ(L"ValidSign", ReadModelText(EPDFSig_GetString, model, second,
                                        EPDF_SIG_STRING_NAME));
  EXPECT_EQ(L"D:20260407111227Z",
            ReadModelText(EPDFSig_GetString, model, second, EPDF_SIG_STRING_M));
  unsigned long long range2[4];
  ASSERT_TRUE(EPDFSig_GetByteRange(model, second, range2));
  EXPECT_EQ(655271u, range2[2] + range2[3]);
  EXPECT_EQ("618c57abc870858b96f1645f4551a67684b6c5ddc7dbd0c46be78b138e0815d5",
            HexDigest(document(), range2, EPDF_DIGEST_SHA256));
  EPDFSig_CloseModel(model);

  // Every prefix opens with the parent's (empty) password and reports the
  // state of its time: the second field is unsigned before revision 3.
  for (int i = 0; i < 4; ++i) {
    ScopedFPDFDocument prefix(EPDFDoc_OpenRevision(document(), kExpectedEnds[i]));
    ASSERT_TRUE(prefix) << "revision " << i;
    EXPECT_EQ(i + 1, EPDFDoc_GetRevisionCount(prefix.get()));
    EPDF_SIGNATURE_MODEL prefix_model = EPDFSig_LoadModel(prefix.get());
    ASSERT_TRUE(prefix_model);
    ASSERT_EQ(2, EPDFSig_Count(prefix_model));
    int signed_count = 0;
    for (int j = 0; j < 2; ++j) {
      if (EPDFSig_IsSigned(prefix_model, j)) {
        ++signed_count;
        EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION,
                  EPDFSig_GetCoverage(prefix_model, j));
      }
    }
    EXPECT_EQ(i == 3 ? 2 : 1, signed_count) << "revision " << i;
    EPDFSig_CloseModel(prefix_model);
  }
}

TEST_F(EPDFSignatureEmbedderTest, DiffBadArguments) {
  ASSERT_TRUE(OpenDocument("signature_chain.pdf"));
  EXPECT_FALSE(EPDFDoc_CompareRevisions(nullptr, document()));
  EXPECT_FALSE(EPDFDoc_CompareRevisions(document(), nullptr));
  EXPECT_FALSE(EPDFDoc_CompareRevisions(document(), document()));
  unsigned long long end0 = 0;
  ASSERT_TRUE(EPDFDoc_GetRevision(document(), 0, &end0, nullptr));
  ScopedFPDFDocument rev0(EPDFDoc_OpenRevision(document(), end0));
  ASSERT_TRUE(rev0);
  // The older document must be a prefix revision of the newer one.
  EXPECT_FALSE(EPDFDoc_CompareRevisions(document(), rev0.get()));
  EXPECT_EQ(-1, EPDFObjectDiff_GetCount(nullptr));
  EXPECT_FALSE(EPDFObjectDiff_GetEntry(nullptr, 0, nullptr, nullptr, nullptr,
                                       nullptr, nullptr, nullptr));
  EXPECT_EQ(0u, EPDFObjectDiff_GetValue(nullptr, 0, EPDF_DIFF_NEW, nullptr, 0,
                                        nullptr));
  EXPECT_EQ(-1, EPDFObjectDiff_GetReferrerCount(nullptr, EPDF_DIFF_NEW, 1));
  EPDFObjectDiff_Close(nullptr);
}

TEST_F(EPDFSignatureEmbedderTest, DiffChainedFixture) {
  ASSERT_TRUE(OpenDocument("signature_chain.pdf"));
  unsigned long long ends[3];
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(EPDFDoc_GetRevision(document(), i, &ends[i], nullptr));
  }
  ScopedFPDFDocument rev0(EPDFDoc_OpenRevision(document(), ends[0]));
  ScopedFPDFDocument rev1(EPDFDoc_OpenRevision(document(), ends[1]));
  ASSERT_TRUE(rev0);
  ASSERT_TRUE(rev1);

  // Revision 0 -> 1: the catalog gains /AcroForm, the page gains /Annots,
  // the signature value, appearance, and widget appear, the trailer grows.
  EPDF_OBJECT_DIFF diff = EPDFDoc_CompareRevisions(rev0.get(), rev1.get());
  ASSERT_TRUE(diff);
  std::vector<DiffRow> rows = ReadDiffRows(diff);
  ASSERT_EQ(6u, rows.size());
  EXPECT_EQ(1u, rows[0].obj_num);
  EXPECT_EQ(EPDF_DIFF_MODIFIED, rows[0].change);
  EXPECT_EQ(EPDF_DIFF_OBJ_DICTIONARY, rows[0].kind);
  EXPECT_EQ(3u, rows[1].obj_num);
  EXPECT_EQ(EPDF_DIFF_MODIFIED, rows[1].change);
  EXPECT_EQ(5u, rows[2].obj_num);
  EXPECT_EQ(EPDF_DIFF_ADDED, rows[2].change);
  EXPECT_EQ(EPDF_DIFF_OBJ_DICTIONARY, rows[2].kind);
  EXPECT_EQ(6u, rows[3].obj_num);
  EXPECT_EQ(EPDF_DIFF_OBJ_STREAM, rows[3].kind);
  EXPECT_EQ(7u, rows[4].obj_num);
  EXPECT_EQ(0u, rows[5].obj_num);
  EXPECT_EQ(EPDF_DIFF_OBJ_TRAILER, rows[5].kind);
  EXPECT_EQ(0, rows[0].old_gen);
  EXPECT_EQ(0, rows[0].new_gen);
  EXPECT_EQ(-1, rows[2].old_gen);
  EXPECT_EQ(0, rows[2].new_gen);

  // Values: key-level evidence in canonical (sorted) form.
  EXPECT_EQ("<</Contents 4 0 R/Parent 2 0 R/Type /Page>>",
            ReadDiffValue(diff, 1, EPDF_DIFF_OLD));
  EXPECT_EQ("<</Annots [7 0 R]/Contents 4 0 R/Parent 2 0 R/Type /Page>>",
            ReadDiffValue(diff, 1, EPDF_DIFF_NEW));
  EXPECT_EQ("", ReadDiffValue(diff, 2, EPDF_DIFF_OLD));
  const std::string sig_value = ReadDiffValue(diff, 2, EPDF_DIFF_NEW);
  EXPECT_NE(std::string::npos, sig_value.find("/ByteRange [0 10 30 10]"));
  EXPECT_NE(std::string::npos, sig_value.find("/SubFilter /ETSI.CAdES.detached"));
  const std::string ap_value = ReadDiffValue(diff, 3, EPDF_DIFF_NEW);
  EXPECT_EQ(0u, ap_value.find("stream(0,"));
  EXPECT_NE(std::string::npos, ap_value.find("/Subtype /Form"));
  EXPECT_NE(std::string::npos,
            ReadDiffValue(diff, 5, EPDF_DIFF_NEW).find("/Size 8"));

  // Usage in both revisions: every inbound reference, back-links included.
  EXPECT_EQ((std::vector<std::string>{"0:Root"}),
            ReadReferrers(diff, EPDF_DIFF_OLD, 1u));
  EXPECT_EQ((std::vector<std::string>{"2:Kids/[0]"}),
            ReadReferrers(diff, EPDF_DIFF_OLD, 3u));
  EXPECT_EQ((std::vector<std::string>{"2:Kids/[0]", "7:P"}),
            ReadReferrers(diff, EPDF_DIFF_NEW, 3u));
  EXPECT_EQ((std::vector<std::string>{"1:AcroForm/Fields/[0]", "3:Annots/[0]"}),
            ReadReferrers(diff, EPDF_DIFF_NEW, 7u));
  EXPECT_EQ((std::vector<std::string>{"7:DV", "7:V"}),
            ReadReferrers(diff, EPDF_DIFF_NEW, 5u));
  EXPECT_EQ((std::vector<std::string>{"7:AP/N"}),
            ReadReferrers(diff, EPDF_DIFF_NEW, 6u));
  EXPECT_EQ(0, EPDFObjectDiff_GetReferrerCount(diff, EPDF_DIFF_OLD, 5u));
  EXPECT_EQ(0, EPDFObjectDiff_GetReferrerCount(diff, EPDF_DIFF_NEW, 999u));
  // Untouched objects are indexed too, so a caller can walk to the root.
  EXPECT_EQ((std::vector<std::string>{"1:Pages", "3:Parent"}),
            ReadReferrers(diff, EPDF_DIFF_NEW, 2u));
  EPDFObjectDiff_Close(diff);

  // Revision 0 -> current: both updates at once.
  diff = EPDFDoc_CompareRevisions(rev0.get(), document());
  ASSERT_TRUE(diff);
  rows = ReadDiffRows(diff);
  EXPECT_EQ(9u, rows.size());  // 1, 3, 5..10, trailer
  EXPECT_NE(-1, FindRow(rows, 10u));
  EPDFObjectDiff_Close(diff);
}

TEST_F(EPDFSignatureEmbedderTest, DiffEncryptedFixture) {
  if (!OpenDocument("embedpdf_two_signatures_encrypted.pdf")) {
    GTEST_SKIP() << "fixture not available";
  }
  unsigned long long ends[4];
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(EPDFDoc_GetRevision(document(), i, &ends[i], nullptr));
  }
  ScopedFPDFDocument rev0(EPDFDoc_OpenRevision(document(), ends[0]));
  ScopedFPDFDocument rev1(EPDFDoc_OpenRevision(document(), ends[1]));
  ASSERT_TRUE(rev0);
  ASSERT_TRUE(rev1);

  // Revision 0 -> 1 is a form fill by the signing service: /Info, /Metadata,
  // one widget, /AcroForm and /Encrypt rewritten (the last two identically),
  // a new appearance with its font, one object stream, one xref stream.
  EPDF_OBJECT_DIFF diff = EPDFDoc_CompareRevisions(rev0.get(), rev1.get());
  ASSERT_TRUE(diff);
  std::vector<DiffRow> rows = ReadDiffRows(diff);
  EXPECT_EQ(17u, rows.size());
  const int info = FindRow(rows, 21u);
  const int metadata = FindRow(rows, 77u);
  const int widget = FindRow(rows, 83u);
  const int acroform = FindRow(rows, 84u);
  const int encrypt = FindRow(rows, 107u);
  const int objstm = FindRow(rows, 147u);
  const int xref = FindRow(rows, 148u);
  ASSERT_NE(-1, info);
  ASSERT_NE(-1, metadata);
  ASSERT_NE(-1, widget);
  ASSERT_NE(-1, acroform);
  ASSERT_NE(-1, encrypt);
  ASSERT_NE(-1, objstm);
  ASSERT_NE(-1, xref);
  EXPECT_EQ(EPDF_DIFF_MODIFIED, rows[widget].change);
  EXPECT_EQ(EPDF_DIFF_OBJ_STREAM, rows[metadata].kind);
  EXPECT_TRUE(rows[metadata].stream_data_changed);
  EXPECT_EQ(EPDF_DIFF_OBJ_OBJSTM, rows[objstm].kind);
  EXPECT_EQ(EPDF_DIFF_OBJ_XREF, rows[xref].kind);
  EXPECT_EQ(EPDF_DIFF_ADDED, rows[xref].change);
  EXPECT_EQ(EPDF_DIFF_OBJ_TRAILER, rows.back().kind);

  // The identical /Encrypt rewrite carries identical evidence. /AcroForm
  // only looks identical from afar: its /DR now names the new font.
  EXPECT_EQ(ReadDiffValue(diff, encrypt, EPDF_DIFF_OLD),
            ReadDiffValue(diff, encrypt, EPDF_DIFF_NEW));
  const std::string acroform_old = ReadDiffValue(diff, acroform, EPDF_DIFF_OLD);
  const std::string acroform_new = ReadDiffValue(diff, acroform, EPDF_DIFF_NEW);
  EXPECT_NE(acroform_old, acroform_new);
  EXPECT_NE(std::string::npos, acroform_old.find("/C2_0 98 0 R"));
  EXPECT_NE(std::string::npos, acroform_new.find("/C2_0 139 0 R"));
  EXPECT_NE(std::string::npos, acroform_new.find("/SigFlags 3"));
  const std::string widget_old = ReadDiffValue(diff, widget, EPDF_DIFF_OLD);
  const std::string widget_new = ReadDiffValue(diff, widget, EPDF_DIFF_NEW);
  EXPECT_NE(widget_old, widget_new);
  // /V went from a bare UTF-16BE byte-order mark (an empty text string, as
  // this producer writes it) to a UTF-16BE text string.
  EXPECT_NE(std::string::npos, widget_old.find("/V (\xFE\xFF)/"));
  EXPECT_EQ(std::string::npos, widget_new.find("/V (\xFE\xFF)/"));
  EXPECT_NE(std::string::npos, widget_new.find("/V (\xFE\xFF"));
  EXPECT_NE(std::string::npos, widget_new.find("/Ff 1"));
  EXPECT_EQ(std::string::npos, widget_old.find("/Ff"));
  EXPECT_NE(std::string::npos, widget_old.find("/T (4ZTqqUOe5pc3)"));
  // The /Info dictionary's strings decrypt in both revisions.
  EXPECT_NE(std::string::npos,
            ReadDiffValue(diff, info, EPDF_DIFF_OLD).find("D:20260312193745Z"));
  EXPECT_NE(std::string::npos,
            ReadDiffValue(diff, info, EPDF_DIFF_NEW).find("D:20260407111227Z"));

  // The widget is referenced by the field tree and by its page's /Annots -
  // an indirect array here (object 85), which in turn hangs off page 3 - in
  // both revisions. Walking referrers reaches the root either way.
  for (int which : {EPDF_DIFF_OLD, EPDF_DIFF_NEW}) {
    EXPECT_EQ((std::vector<std::string>{"84:Fields/[0]", "85:[0]"}),
              ReadReferrers(diff, which, 83u))
        << "which " << which;
    EXPECT_EQ((std::vector<std::string>{"3:Annots"}),
              ReadReferrers(diff, which, 85u));
    EXPECT_EQ((std::vector<std::string>{"1:AcroForm"}),
              ReadReferrers(diff, which, 84u));
  }
  // The /Info dictionary hangs off the trailer; the xref stream is an orphan.
  EXPECT_EQ((std::vector<std::string>{"0:Info"}),
            ReadReferrers(diff, EPDF_DIFF_NEW, 21u));
  EXPECT_EQ(0, EPDFObjectDiff_GetReferrerCount(diff, EPDF_DIFF_NEW, 148u));
  EXPECT_EQ((std::vector<std::string>{"0:Root"}),
            ReadReferrers(diff, EPDF_DIFF_NEW, 1u));
  EPDFObjectDiff_Close(diff);

  // Revision 2 -> 3 is the second signature.
  ScopedFPDFDocument rev2(EPDFDoc_OpenRevision(document(), ends[2]));
  ASSERT_TRUE(rev2);
  diff = EPDFDoc_CompareRevisions(rev2.get(), document());
  ASSERT_TRUE(diff);
  rows = ReadDiffRows(diff);
  const int sig_widget = FindRow(rows, 91u);
  const int sig_value = FindRow(rows, 160u);
  ASSERT_NE(-1, sig_widget);
  ASSERT_NE(-1, sig_value);
  EXPECT_EQ(EPDF_DIFF_ADDED, rows[sig_value].change);
  EXPECT_NE(std::string::npos,
            ReadDiffValue(diff, sig_widget, EPDF_DIFF_NEW).find("/V 160 0 R"));
  EXPECT_EQ(std::string::npos,
            ReadDiffValue(diff, sig_widget, EPDF_DIFF_OLD).find("/V "));
  EXPECT_EQ((std::vector<std::string>{"91:V"}),
            ReadReferrers(diff, EPDF_DIFF_NEW, 160u));
  EXPECT_EQ(0, EPDFObjectDiff_GetReferrerCount(diff, EPDF_DIFF_OLD, 160u));
  EPDFObjectDiff_Close(diff);
}

namespace {

// Creates an unsigned signature field "name" with a widget on page 0.
uint32_t CreateSignatureField(FPDF_DOCUMENT doc, FPDF_PAGE page, const wchar_t* name) {
  ScopedFPDFWideString wide = GetFPDFWideString(name);
  const uint32_t field = EPDFForm_CreateField(doc, EPDF_FORMFIELD_FAMILY_SIGNATURE, wide.get());
  if (field == 0) {
    ADD_FAILURE() << "EPDFForm_CreateField failed";
    return 0;
  }
  ScopedFPDFAnnotation widget(EPDFPage_CreateAnnot(page, FPDF_ANNOT_WIDGET));
  if (!widget) {
    ADD_FAILURE() << "EPDFPage_CreateAnnot failed";
    return 0;
  }
  FS_RECTF rect = {50, 50, 250, 110};
  FPDFAnnot_SetRect(widget.get(), &rect);
  const unsigned int widget_objnum = EPDFAnnot_GetObjectNumber(widget.get());
  widget.reset();
  if (widget_objnum == 0) {
    ADD_FAILURE() << "widget has no object number";
    return 0;
  }
  if (!EPDFForm_AttachWidget(doc, field, widget_objnum, nullptr)) {
    ADD_FAILURE() << "EPDFForm_AttachWidget failed for widget " << widget_objnum;
    return 0;
  }
  return field;
}

// A minimal, valid DER SEQUENCE standing in for a CMS blob.
const unsigned char kFakeCms[] = {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x02};

struct Sealed {
  std::vector<unsigned char> bytes;
  unsigned long long range[4] = {0, 0, 0, 0};
  unsigned long long contents_offset = 0;
  unsigned long long contents_hex_len = 0;
  std::string digest_hex;
};

// save -> seal -> write for an already prepared /V |value|.
bool SealPrepared(FPDF_DOCUMENT doc, uint32_t value, int digest_algorithm, Sealed* out) {
  unsigned long long size = 0;
  unsigned long long obj_offset = 0;
  unsigned long long obj_len = 0;
  void* buffer = EPDFSig_SaveCandidateToOwnedBuffer(doc, value, &size, &obj_offset, &obj_len);
  if (!buffer) {
    ADD_FAILURE() << "EPDFSig_SaveCandidateToOwnedBuffer failed for value " << value;
    return false;
  }
  out->bytes.assign(static_cast<unsigned char*>(buffer), static_cast<unsigned char*>(buffer) + size);
  EPDF_FreeBuffer(buffer);
  unsigned char digest[64];
  unsigned long len = sizeof(digest);
  if (!EPDFSig_Seal(out->bytes.data(), size, obj_offset, obj_len, digest_algorithm, out->range,
                    &out->contents_offset, &out->contents_hex_len, digest, &len)) {
    ADD_FAILURE() << "EPDFSig_Seal failed; object span: "
                  << std::string(reinterpret_cast<const char*>(out->bytes.data()) + obj_offset,
                                 static_cast<size_t>(std::min<unsigned long long>(obj_len, 200)));
    return false;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  out->digest_hex.clear();
  for (unsigned long i = 0; i < len; ++i) {
    out->digest_hex.push_back(kHex[digest[i] >> 4]);
    out->digest_hex.push_back(kHex[digest[i] & 0x0f]);
  }
  return EPDFSig_WriteContents(out->bytes.data(), size, out->contents_offset, out->contents_hex_len,
                               kFakeCms, sizeof(kFakeCms));
}

// prepare -> save -> seal -> write, returning the signed bytes.
bool SignField(FPDF_DOCUMENT doc, uint32_t field, const EPDF_SIG_PREPARE& opts, Sealed* out) {
  const uint32_t value = EPDFSig_Prepare(doc, field, &opts);
  if (value == 0) {
    ADD_FAILURE() << "EPDFSig_Prepare refused";
    return false;
  }
  return SealPrepared(doc, value, opts.digest, out);
}

}  // namespace

TEST_F(EPDFSignatureEmbedderTest, CreateSignatureFieldAndLock) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(document(), 0));
  ASSERT_TRUE(page);
  const uint32_t field = CreateSignatureField(document(), page.get(), L"sig1");
  ASSERT_NE(0u, field);

  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(document());
  ASSERT_TRUE(model);
  ASSERT_EQ(1, EPDFSig_Count(model));
  EXPECT_FALSE(EPDFSig_IsSigned(model, 0));
  EXPECT_EQ(field, EPDFSig_GetFieldObjNum(model, 0));
  EXPECT_NE(0u, EPDFSig_GetWidgetObjNum(model, 0));
  EXPECT_NE(0u, EPDFSig_GetWidgetPageObjNum(model, 0));
  EXPECT_EQ(L"sig1", ReadFieldName(model, 0));
  EXPECT_EQ(EPDF_SIG_FIELD_ACTION_NONE, EPDFSig_GetLockAction(model, 0));
  EPDFSig_CloseModel(model);

  ScopedFPDFWideString text_box = GetFPDFWideString(L"Text Box");
  const FPDF_WIDESTRING names[] = {text_box.get()};
  EXPECT_FALSE(EPDFSig_SetFieldLock(document(), field, 99, names, 1, 0));
  EXPECT_FALSE(EPDFSig_SetFieldLock(document(), field, EPDF_SIG_FIELD_ACTION_INCLUDE, names, 1, 4));
  ASSERT_TRUE(EPDFSig_SetFieldLock(document(), field, EPDF_SIG_FIELD_ACTION_INCLUDE, names, 1, 2));
  model = EPDFSig_LoadModel(document());
  EXPECT_EQ(EPDF_SIG_FIELD_ACTION_INCLUDE, EPDFSig_GetLockAction(model, 0));
  EXPECT_EQ(2, EPDFSig_GetLockPermission(model, 0));
  EXPECT_EQ(1, EPDFSig_GetFieldNameCount(model, 0, EPDF_SIG_FIELDS_LOCK));
  EXPECT_EQ(L"Text Box", ReadFieldNameAt(model, 0, EPDF_SIG_FIELDS_LOCK, 0));
  EPDFSig_CloseModel(model);
  ASSERT_TRUE(EPDFSig_SetFieldLock(document(), field, EPDF_SIG_FIELD_ACTION_NONE, nullptr, 0, 0));
  model = EPDFSig_LoadModel(document());
  EXPECT_EQ(EPDF_SIG_FIELD_ACTION_NONE, EPDFSig_GetLockAction(model, 0));
  EPDFSig_CloseModel(model);
}

TEST_F(EPDFSignatureEmbedderTest, SignRoundTrip) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(document(), 0));
  ASSERT_TRUE(page);
  const uint32_t field = CreateSignatureField(document(), page.get(), L"sig1");
  ASSERT_NE(0u, field);
  page.reset();

  ScopedFPDFWideString reason = GetFPDFWideString(L"Approved");
  ScopedFPDFWideString text_box = GetFPDFWideString(L"Text Box");
  const FPDF_WIDESTRING locked[] = {text_box.get()};
  EPDF_SIG_PREPARE opts = {};
  opts.subfilter = EPDF_SIG_SUBFILTER_ETSI_CADES_DETACHED;
  opts.digest = EPDF_DIGEST_SHA256;
  opts.contents_size = 2048;
  opts.reason = reason.get();
  opts.signing_time = "D:20260910120000Z";
  opts.fieldmdp_action = EPDF_SIG_FIELD_ACTION_INCLUDE;
  opts.fieldmdp_fields = locked;
  opts.fieldmdp_field_count = 1;

  // Inconsistent requests are refused before anything is written.
  EPDF_SIG_PREPARE bad = opts;
  bad.digest = EPDF_DIGEST_SHA1;
  EXPECT_EQ(0u, EPDFSig_Prepare(document(), field, &bad));
  bad = opts;
  bad.contents_size = 16;
  EXPECT_EQ(0u, EPDFSig_Prepare(document(), field, &bad));
  bad = opts;
  bad.subfilter = EPDF_SIG_SUBFILTER_ETSI_RFC3161;  // a timestamp with a FieldMDP
  EXPECT_EQ(0u, EPDFSig_Prepare(document(), field, &bad));
  EXPECT_EQ(0u, EPDFSig_Prepare(document(), 999999u, &opts));

  Sealed sealed;
  ASSERT_TRUE(SignField(document(), field, opts, &sealed));
  EXPECT_EQ(0u, sealed.range[0]);
  EXPECT_EQ(sealed.range[2] + sealed.range[3], sealed.bytes.size());
  EXPECT_EQ(4096u, sealed.contents_hex_len);
  EXPECT_EQ(64u, sealed.digest_hex.size());
  // A second signature on the same (now prepared) field is refused.
  EXPECT_EQ(0u, EPDFSig_Prepare(document(), field, &opts));

  // The sealed bytes are a complete, signed, two-revision document.
  ScopedFPDFDocument signed_doc(FPDF_LoadMemDocument64(sealed.bytes.data(), sealed.bytes.size(), nullptr));
  ASSERT_TRUE(signed_doc);
  EXPECT_EQ(2, EPDFDoc_GetRevisionCount(signed_doc.get()));
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(signed_doc.get());
  ASSERT_TRUE(model);
  ASSERT_EQ(1, EPDFSig_Count(model));
  EXPECT_TRUE(EPDFSig_IsSigned(model, 0));
  EXPECT_EQ(EPDF_SIG_KIND_SIGNATURE, EPDFSig_GetKind(model, 0));
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, EPDFSig_GetCoverage(model, 0));
  EXPECT_EQ(1, EPDFSig_GetRevisionIndex(model, 0));
  unsigned long long range[4];
  ASSERT_TRUE(EPDFSig_GetByteRange(model, 0, range));
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(sealed.range[i], range[i]) << i;
  }
  EXPECT_EQ(sizeof(kFakeCms), EPDFSig_GetContents(model, 0, nullptr, 0));
  std::vector<unsigned char> contents(sizeof(kFakeCms));
  EXPECT_EQ(sizeof(kFakeCms), EPDFSig_GetContents(model, 0, contents.data(), contents.size()));
  EXPECT_EQ(0, memcmp(contents.data(), kFakeCms, sizeof(kFakeCms)));
  EXPECT_EQ(L"ETSI.CAdES.detached", ReadModelText(EPDFSig_GetString, model, 0, EPDF_SIG_STRING_SUBFILTER));
  EXPECT_EQ(L"Adobe.PPKLite", ReadModelText(EPDFSig_GetString, model, 0, EPDF_SIG_STRING_FILTER));
  EXPECT_EQ(L"Approved", ReadModelText(EPDFSig_GetString, model, 0, EPDF_SIG_STRING_REASON));
  EXPECT_EQ(L"D:20260910120000Z", ReadModelText(EPDFSig_GetString, model, 0, EPDF_SIG_STRING_M));
  EXPECT_EQ(0, EPDFSig_GetDocMDPPermission(model, 0));
  EXPECT_FALSE(EPDFSig_IsCatalogCertification(model, 0));
  EXPECT_EQ(EPDF_SIG_FIELD_ACTION_INCLUDE, EPDFSig_GetFieldMDPAction(model, 0));
  EXPECT_EQ(1, EPDFSig_GetFieldNameCount(model, 0, EPDF_SIG_FIELDS_FIELDMDP));
  EXPECT_EQ(L"Text Box", ReadFieldNameAt(model, 0, EPDF_SIG_FIELDS_FIELDMDP, 0));
  EXPECT_EQ(EPDF_SIG_FIELD_ACTION_INCLUDE, EPDFSig_GetLockAction(model, 0));
  EPDFSig_CloseModel(model);
  // The digest the seal reported is the digest of the signed bytes.
  EXPECT_EQ(sealed.digest_hex, HexDigest(signed_doc.get(), range, EPDF_DIGEST_SHA256));

  // The locked text field became ReadOnly; a second signature field on the
  // signed document is refused while the first field's lock covers it, but
  // an unrelated field can still be prepared (level 'fill' stays in force).
  EPDF_FORM_MODEL form = EPDFForm_LoadModel(signed_doc.get());
  ASSERT_TRUE(form);
  bool saw_text_box = false;
  for (int i = 0; i < EPDFForm_CountFields(form); ++i) {
    std::vector<FPDF_WCHAR> buffer(64);
    EPDFForm_GetFieldName(form, i, buffer.data(), buffer.size() * sizeof(FPDF_WCHAR));
    if (GetPlatformWString(buffer.data()) == L"Text Box") {
      saw_text_box = true;
      EXPECT_TRUE(EPDFForm_GetFieldFlags(form, i) & 1u);
    }
  }
  EXPECT_TRUE(saw_text_box);
  EPDFForm_CloseModel(form);

  // Revision 0 -> 1 through the diff: the field gained /V, the value and
  // its reference dictionaries appeared, the text field gained ReadOnly.
  unsigned long long end0 = 0;
  ASSERT_TRUE(EPDFDoc_GetRevision(signed_doc.get(), 0, &end0, nullptr));
  ScopedFPDFDocument rev0(EPDFDoc_OpenRevision(signed_doc.get(), end0));
  ASSERT_TRUE(rev0);
  EPDF_OBJECT_DIFF diff = EPDFDoc_CompareRevisions(rev0.get(), signed_doc.get());
  ASSERT_TRUE(diff);
  const std::vector<DiffRow> rows = ReadDiffRows(diff);
  // The field itself was created in this session, so it is ADDED relative
  // to the file on disk; its value carries /V and the mirrored /Lock.
  const int field_row = FindRow(rows, field);
  ASSERT_NE(-1, field_row);
  EXPECT_EQ(EPDF_DIFF_ADDED, rows[field_row].change);
  EXPECT_EQ("", ReadDiffValue(diff, field_row, EPDF_DIFF_OLD));
  EXPECT_NE(std::string::npos, ReadDiffValue(diff, field_row, EPDF_DIFF_NEW).find("/V "));
  EXPECT_NE(std::string::npos, ReadDiffValue(diff, field_row, EPDF_DIFF_NEW).find("/Lock "));
  EPDFObjectDiff_Close(diff);
}

TEST_F(EPDFSignatureEmbedderTest, CertifyThenRefuseSecondCertification) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(document(), 0));
  ASSERT_TRUE(page);
  const uint32_t first = CreateSignatureField(document(), page.get(), L"cert");
  const uint32_t second = CreateSignatureField(document(), page.get(), L"approval");
  ASSERT_NE(0u, first);
  ASSERT_NE(0u, second);
  page.reset();

  EPDF_SIG_PREPARE opts = {};
  opts.subfilter = EPDF_SIG_SUBFILTER_ADBE_PKCS7_DETACHED;
  opts.digest = EPDF_DIGEST_SHA256;
  opts.contents_size = 2048;
  opts.docmdp_permission = 2;
  Sealed sealed;
  ASSERT_TRUE(SignField(document(), first, opts, &sealed));

  ScopedFPDFDocument signed_doc(FPDF_LoadMemDocument64(sealed.bytes.data(), sealed.bytes.size(), nullptr));
  ASSERT_TRUE(signed_doc);
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(signed_doc.get());
  ASSERT_TRUE(model);
  ASSERT_EQ(2, EPDFSig_Count(model));
  const int cert = EPDFSig_GetIndexByFieldObjNum(model, first);
  ASSERT_NE(-1, cert);
  EXPECT_TRUE(EPDFSig_IsSigned(model, cert));
  EXPECT_EQ(2, EPDFSig_GetDocMDPPermission(model, cert));
  EXPECT_TRUE(EPDFSig_IsCatalogCertification(model, cert));
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, EPDFSig_GetCoverage(model, cert));
  EPDFSig_CloseModel(model);

  // A certification must be the first signature: refused now, while an
  // approval signature on the second field is fine.
  EPDF_SIG_PREPARE again = opts;
  EXPECT_EQ(0u, EPDFSig_Prepare(signed_doc.get(), second, &again));
  again.docmdp_permission = 0;
  EXPECT_NE(0u, EPDFSig_Prepare(signed_doc.get(), second, &again));
}

TEST_F(EPDFSignatureEmbedderTest, PermissionOneAllowsOnlyTimestamps) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(document(), 0));
  ASSERT_TRUE(page);
  const uint32_t first = CreateSignatureField(document(), page.get(), L"cert");
  const uint32_t second = CreateSignatureField(document(), page.get(), L"later");
  ASSERT_NE(0u, first);
  ASSERT_NE(0u, second);
  page.reset();

  EPDF_SIG_PREPARE opts = {};
  opts.subfilter = EPDF_SIG_SUBFILTER_ETSI_CADES_DETACHED;
  opts.digest = EPDF_DIGEST_SHA256;
  opts.contents_size = 256;
  opts.docmdp_permission = 1;
  Sealed sealed;
  ASSERT_TRUE(SignField(document(), first, opts, &sealed));
  ScopedFPDFDocument signed_doc(FPDF_LoadMemDocument64(sealed.bytes.data(), sealed.bytes.size(), nullptr));
  ASSERT_TRUE(signed_doc);

  EPDF_SIG_PREPARE approval = opts;
  approval.docmdp_permission = 0;
  EXPECT_EQ(0u, EPDFSig_Prepare(signed_doc.get(), second, &approval));
  EPDF_SIG_PREPARE timestamp = approval;
  timestamp.subfilter = EPDF_SIG_SUBFILTER_ETSI_RFC3161;
  const uint32_t value = EPDFSig_Prepare(signed_doc.get(), second, &timestamp);
  ASSERT_NE(0u, value);
  Sealed stamped;
  unsigned long long size = 0;
  unsigned long long obj_offset = 0;
  unsigned long long obj_len = 0;
  void* buffer = EPDFSig_SaveCandidateToOwnedBuffer(signed_doc.get(), value, &size, &obj_offset, &obj_len);
  ASSERT_TRUE(buffer);
  stamped.bytes.assign(static_cast<unsigned char*>(buffer), static_cast<unsigned char*>(buffer) + size);
  EPDF_FreeBuffer(buffer);
  unsigned char digest[64];
  unsigned long len = sizeof(digest);
  ASSERT_TRUE(EPDFSig_Seal(stamped.bytes.data(), size, obj_offset, obj_len, EPDF_DIGEST_SHA256,
                           stamped.range, &stamped.contents_offset, &stamped.contents_hex_len, digest, &len));
  ASSERT_TRUE(EPDFSig_WriteContents(stamped.bytes.data(), size, stamped.contents_offset,
                                    stamped.contents_hex_len, kFakeCms, sizeof(kFakeCms)));
  ScopedFPDFDocument stamped_doc(FPDF_LoadMemDocument64(stamped.bytes.data(), stamped.bytes.size(), nullptr));
  ASSERT_TRUE(stamped_doc);
  EXPECT_EQ(3, EPDFDoc_GetRevisionCount(stamped_doc.get()));
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(stamped_doc.get());
  const int ts = EPDFSig_GetIndexByFieldObjNum(model, second);
  ASSERT_NE(-1, ts);
  EXPECT_EQ(EPDF_SIG_KIND_DOC_TIMESTAMP, EPDFSig_GetKind(model, ts));
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, EPDFSig_GetCoverage(model, ts));
  EXPECT_EQ(2, EPDFSig_GetRevisionIndex(model, ts));
  EPDFSig_CloseModel(model);
}

TEST_F(EPDFSignatureEmbedderTest, SealRejectsDecoysAndBadDer) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(document(), 0));
  ASSERT_TRUE(page);
  const uint32_t field = CreateSignatureField(document(), page.get(), L"sig1");
  ASSERT_NE(0u, field);
  page.reset();
  // A /Reason that looks like the placeholders must not fool the seal: the
  // key order the serializer uses puts strings after /Contents, and the
  // sentinel shapes are verified exactly.
  ScopedFPDFWideString decoy = GetFPDFWideString(L"/ByteRange[ 0 2147483647 2147483647 2147483647] /Contents<0000>");
  EPDF_SIG_PREPARE opts = {};
  opts.subfilter = EPDF_SIG_SUBFILTER_ETSI_CADES_DETACHED;
  opts.digest = EPDF_DIGEST_SHA256;
  opts.contents_size = 256;
  opts.reason = decoy.get();
  const uint32_t value = EPDFSig_Prepare(document(), field, &opts);
  ASSERT_NE(0u, value);
  unsigned long long size = 0;
  unsigned long long obj_offset = 0;
  unsigned long long obj_len = 0;
  void* buffer = EPDFSig_SaveCandidateToOwnedBuffer(document(), value, &size, &obj_offset, &obj_len);
  ASSERT_TRUE(buffer);
  std::vector<unsigned char> bytes(static_cast<unsigned char*>(buffer), static_cast<unsigned char*>(buffer) + size);
  EPDF_FreeBuffer(buffer);
  unsigned long long range[4];
  unsigned long long contents_offset = 0;
  unsigned long long hex_len = 0;
  unsigned char digest[64];
  unsigned long len = sizeof(digest);
  ASSERT_TRUE(EPDFSig_Seal(bytes.data(), size, obj_offset, obj_len, EPDF_DIGEST_SHA256, range,
                           &contents_offset, &hex_len, digest, &len));
  EXPECT_EQ(512u, hex_len);
  EXPECT_EQ('<', bytes[contents_offset - 1]);
  EXPECT_EQ('>', bytes[contents_offset + hex_len]);
  // A span that is not the signature object fails; so does a DER whose
  // declared length disagrees, or one that does not fit.
  EXPECT_FALSE(EPDFSig_Seal(bytes.data(), size, 0, obj_offset, EPDF_DIGEST_SHA256, range,
                            &contents_offset, &hex_len, digest, &len));
  const unsigned char short_der[] = {0x30, 0x05, 0x02, 0x01, 0x01};
  EXPECT_FALSE(EPDFSig_WriteContents(bytes.data(), size, contents_offset, hex_len, short_der, sizeof(short_der)));
  std::vector<unsigned char> huge(300, 0);
  huge[0] = 0x30;
  huge[1] = 0x82;
  huge[2] = 0x01;
  huge[3] = 0x28;  // 296 content bytes: 300 total, 600 hex > 512
  EXPECT_FALSE(EPDFSig_WriteContents(bytes.data(), size, contents_offset, hex_len, huge.data(), huge.size()));
  ASSERT_TRUE(EPDFSig_WriteContents(bytes.data(), size, contents_offset, hex_len, kFakeCms, sizeof(kFakeCms)));
  ScopedFPDFDocument signed_doc(FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr));
  ASSERT_TRUE(signed_doc);
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(signed_doc.get());
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, EPDFSig_GetCoverage(model, 0));
  EPDFSig_CloseModel(model);
}

TEST_F(EPDFSignatureEmbedderTest, SignEncryptedDocument) {
  if (!OpenDocument("embedpdf_two_signatures_encrypted.pdf")) {
    GTEST_SKIP() << "fixture not available";
  }
  ScopedFPDFPage page(FPDF_LoadPage(document(), 0));
  ASSERT_TRUE(page);
  const uint32_t field = CreateSignatureField(document(), page.get(), L"third");
  ASSERT_NE(0u, field);
  page.reset();
  // The existing second signature locks two fields by FieldMDP; the new
  // field is not among them, so an approval signature is allowed.
  EPDF_SIG_PREPARE opts = {};
  opts.subfilter = EPDF_SIG_SUBFILTER_ADBE_PKCS7_DETACHED;
  opts.digest = EPDF_DIGEST_SHA256;
  opts.contents_size = 2048;
  Sealed sealed;
  ASSERT_TRUE(SignField(document(), field, opts, &sealed));
  // Encrypted with the empty user password: reopens without one, /Contents
  // was written in the clear, and the digest covers the encrypted bytes.
  ScopedFPDFDocument signed_doc(FPDF_LoadMemDocument64(sealed.bytes.data(), sealed.bytes.size(), nullptr));
  ASSERT_TRUE(signed_doc);
  EXPECT_EQ(5, EPDFDoc_GetRevisionCount(signed_doc.get()));
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(signed_doc.get());
  ASSERT_TRUE(model);
  ASSERT_EQ(3, EPDFSig_Count(model));
  const int third = EPDFSig_GetIndexByFieldObjNum(model, field);
  ASSERT_NE(-1, third);
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, EPDFSig_GetCoverage(model, third));
  EXPECT_EQ(4, EPDFSig_GetRevisionIndex(model, third));
  EXPECT_EQ(sizeof(kFakeCms), EPDFSig_GetContents(model, third, nullptr, 0));
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, EPDFSig_GetCoverage(model, i)) << i;
  }
  unsigned long long range[4];
  ASSERT_TRUE(EPDFSig_GetByteRange(model, third, range));
  EXPECT_EQ(sealed.digest_hex, HexDigest(signed_doc.get(), range, EPDF_DIGEST_SHA256));
  EPDFSig_CloseModel(model);
}

// ---------------------------------------------------------------------------
// Review probes: hand-built documents that pin down evidence, placeholder,
// coverage, locking, provenance, and BER behaviour.
// ---------------------------------------------------------------------------

namespace {

struct RawPdf {
  std::string bytes;
  size_t xref = 0;
};

// A classic-xref document from object bodies (object i+1 = objects[i]).
RawPdf MakeRawPdf(const std::vector<std::string>& objects) {
  RawPdf p;
  p.bytes = "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(p.bytes.size());
    p.bytes += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  p.xref = p.bytes.size();
  p.bytes += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
  char row[40];
  for (size_t offset : offsets) {
    snprintf(row, sizeof(row), "%010zu 00000 n \n", offset);
    p.bytes += row;
  }
  p.bytes += "trailer\n<</Root 1 0 R /Size " + std::to_string(objects.size() + 1) +
             ">>\nstartxref\n" + std::to_string(p.xref) + "\n%%EOF\n";
  return p;
}

// One incremental update redefining object |num|.
RawPdf AppendRawUpdate(RawPdf p, unsigned num, const std::string& object, unsigned size) {
  const size_t old_xref = p.xref;
  const size_t offset = p.bytes.size();
  p.bytes += std::to_string(num) + " 0 obj\n" + object + "\nendobj\n";
  p.xref = p.bytes.size();
  char row[40];
  snprintf(row, sizeof(row), "%010zu 00000 n \n", offset);
  p.bytes += "xref\n" + std::to_string(num) + " 1\n" + row + "trailer\n<</Root 1 0 R /Size " +
             std::to_string(size) + " /Prev " + std::to_string(old_xref) + ">>\nstartxref\n" +
             std::to_string(p.xref) + "\n%%EOF\n";
  return p;
}

std::vector<std::string> BasicObjects(const std::string& catalog_extra = "") {
  return {"<</Type/Catalog /Pages 2 0 R " + catalog_extra + ">>",
          "<</Type/Pages /Kids[3 0 R] /Count 1>>",
          "<</Type/Page /Parent 2 0 R /MediaBox[0 0 100 100]>>"};
}

ScopedFPDFDocument OpenRaw(const RawPdf& p) {
  return ScopedFPDFDocument(FPDF_LoadMemDocument64(p.bytes.data(), p.bytes.size(), nullptr));
}

// A form with a group field carrying an inherited Multiline flag, a text
// kid "group.total", and an unsigned signature field (object 6).
RawPdf FormObjects(const std::string& sig_extra = "") {
  std::vector<std::string> b = BasicObjects("/AcroForm<</Fields[4 0 R 6 0 R]>>");
  b.push_back("<</T(group) /FT/Tx /Ff 4096 /Kids[5 0 R]>>");
  b.push_back("<</T(total) /Parent 4 0 R /V(text)>>");
  b.push_back("<</FT/Sig /T(sig) " + sig_extra + ">>");
  return MakeRawPdf(b);
}

EPDF_SIG_PREPARE DefaultPrepare() {
  EPDF_SIG_PREPARE o = {};
  o.subfilter = EPDF_SIG_SUBFILTER_ETSI_CADES_DETACHED;
  o.digest = EPDF_DIGEST_SHA256;
  o.contents_size = 256;
  return o;
}

int LocalFieldFlags(FPDF_DOCUMENT doc, uint32_t objnum) {
  CPDF_Document* native = CPDFDocumentFromFPDFDocument(doc);
  RetainPtr<const CPDF_Dictionary> dict = ToDictionary(native->GetOrParseIndirectObject(objnum));
  return dict ? dict->GetIntegerFor("Ff") : -1;
}

}  // namespace

TEST_F(EPDFSignatureEmbedderTest, ProbeUnrelatedFilesDoNotShareHistory) {
  // Same layout, same offsets, different bytes: not a revision pair.
  RawPdf a = MakeRawPdf(BasicObjects("/Tag(A)"));
  RawPdf b = MakeRawPdf(BasicObjects("/Tag(B)"));
  ScopedFPDFDocument da = OpenRaw(a);
  ScopedFPDFDocument db = OpenRaw(b);
  ASSERT_TRUE(da);
  ASSERT_TRUE(db);
  EXPECT_FALSE(EPDFDoc_CompareRevisions(da.get(), db.get()));
}

TEST_F(EPDFSignatureEmbedderTest, ProbeDictionaryEvidenceDoesNotCollide) {
  std::vector<std::string> b = BasicObjects("/Extra 4 0 R");
  b.push_back("<</A 1 /B 2>>");
  RawPdf old = MakeRawPdf(b);
  RawPdf newer = AppendRawUpdate(old, 4, "<</A#201#2fB 2>>", 5);
  ScopedFPDFDocument dnew = OpenRaw(newer);
  ASSERT_TRUE(dnew);
  ScopedFPDFDocument dold(EPDFDoc_OpenRevision(dnew.get(), old.bytes.size()));
  ASSERT_TRUE(dold);
  EPDF_OBJECT_DIFF diff = EPDFDoc_CompareRevisions(dold.get(), dnew.get());
  ASSERT_TRUE(diff);
  const std::vector<DiffRow> rows = ReadDiffRows(diff);
  const int row = FindRow(rows, 4u);
  ASSERT_NE(-1, row);
  const std::string old_value = ReadDiffValue(diff, row, EPDF_DIFF_OLD);
  const std::string new_value = ReadDiffValue(diff, row, EPDF_DIFF_NEW);
  EXPECT_EQ("<</A 1/B 2>>", old_value);
  EXPECT_NE(old_value, new_value);
  EXPECT_NE(std::string::npos, new_value.find("/A#20"));
  EPDFObjectDiff_Close(diff);
}

TEST_F(EPDFSignatureEmbedderTest, ProbeReferrerLabelsEscapeKeys) {
  std::vector<std::string> b = BasicObjects("/A#2FB 4 0 R /Arr[5 0 R]");
  b.push_back("<</Kind/X>>");
  b.push_back("<</Kind/Y>>");
  RawPdf old = MakeRawPdf(b);
  RawPdf newer = AppendRawUpdate(old, 4, "<</Kind/Z>>", 6);
  ScopedFPDFDocument dnew = OpenRaw(newer);
  ASSERT_TRUE(dnew);
  ScopedFPDFDocument dold(EPDFDoc_OpenRevision(dnew.get(), old.bytes.size()));
  ASSERT_TRUE(dold);
  EPDF_OBJECT_DIFF diff = EPDFDoc_CompareRevisions(dold.get(), dnew.get());
  ASSERT_TRUE(diff);
  EXPECT_EQ((std::vector<std::string>{"1:A#2FB"}), ReadReferrers(diff, EPDF_DIFF_NEW, 4u));
  EXPECT_EQ((std::vector<std::string>{"1:Arr/[0]"}), ReadReferrers(diff, EPDF_DIFF_NEW, 5u));
  EPDFObjectDiff_Close(diff);
}

TEST_F(EPDFSignatureEmbedderTest, ProbeCoverageRequiresPhysicalContents) {
  // The hole excludes an unrelated object holding the same bytes as
  // /Contents, while the real /Contents sits inside the hashed range.
  std::vector<std::string> b = BasicObjects("/AcroForm<</Fields[4 0 R]>>");
  b.push_back("<</FT/Sig /T(sig) /V 5 0 R>>");
  const std::string placeholder = "0000000000 0000000000 0000000000 0000000000";
  b.push_back("<</Type/Sig /ByteRange[" + placeholder + "] /Contents<3006020101020102>>>");
  b.push_back("<3006020101020102>");
  RawPdf p = MakeRawPdf(b);
  const size_t decoy = p.bytes.rfind("<3006020101020102>");
  char range[64];
  snprintf(range, sizeof(range), "%010u %010zu %010zu %010zu", 0u, decoy, decoy + 18,
           p.bytes.size() - decoy - 18);
  p.bytes.replace(p.bytes.find(placeholder), placeholder.size(), range);
  ScopedFPDFDocument doc = OpenRaw(p);
  ASSERT_TRUE(doc);
  EPDF_SIGNATURE_MODEL m = EPDFSig_LoadModel(doc.get());
  ASSERT_TRUE(m);
  ASSERT_EQ(1, EPDFSig_Count(m));
  EXPECT_TRUE(EPDFSig_IsSigned(m, 0));
  EXPECT_EQ(EPDF_SIG_COVERAGE_PARTIAL, EPDFSig_GetCoverage(m, 0));
  EXPECT_EQ(8u, EPDFSig_GetContents(m, 0, nullptr, 0));
  EPDFSig_CloseModel(m);

  // The same document with the hole on the real /Contents is whole.
  RawPdf q = MakeRawPdf(b);
  const size_t real = q.bytes.find("/Contents<") + strlen("/Contents");
  snprintf(range, sizeof(range), "%010u %010zu %010zu %010zu", 0u, real, real + 18,
           q.bytes.size() - real - 18);
  q.bytes.replace(q.bytes.find(placeholder), placeholder.size(), range);
  ScopedFPDFDocument doc2 = OpenRaw(q);
  ASSERT_TRUE(doc2);
  m = EPDFSig_LoadModel(doc2.get());
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, EPDFSig_GetCoverage(m, 0));
  EXPECT_EQ(0, EPDFSig_GetRevisionIndex(m, 0));
  EPDFSig_CloseModel(m);
}

TEST_F(EPDFSignatureEmbedderTest, ProbeContactInfoCannotBecomePlaceholder) {
  RawPdf p = FormObjects();
  ScopedFPDFDocument doc = OpenRaw(p);
  ASSERT_TRUE(doc);
  EPDF_SIG_PREPARE o = DefaultPrepare();
  // Sorts before /Contents and reads exactly like the placeholder.
  ScopedFPDFWideString decoy = GetFPDFWideString(L"/Contents<0000> /ByteRange[ 0 2147483647 2147483647 2147483647]");
  o.contact_info = decoy.get();
  const uint32_t sig = EPDFSig_Prepare(doc.get(), 6, &o);
  ASSERT_NE(0u, sig);
  unsigned long long size = 0;
  unsigned long long offset = 0;
  unsigned long long len = 0;
  void* raw = EPDFSig_SaveCandidateToOwnedBuffer(doc.get(), sig, &size, &offset, &len);
  ASSERT_TRUE(raw);
  unsigned long long range[4];
  unsigned long long co = 0;
  unsigned long long ch = 0;
  unsigned char digest[64];
  unsigned long cap = sizeof(digest);
  EXPECT_TRUE(EPDFSig_Seal(static_cast<unsigned char*>(raw), size, offset, len, EPDF_DIGEST_SHA256,
                           range, &co, &ch, digest, &cap));
  EXPECT_EQ(512u, ch);
  EPDF_FreeBuffer(raw);
}

TEST_F(EPDFSignatureEmbedderTest, ProbeLockPreservesInheritedFlags) {
  RawPdf p = FormObjects();
  ScopedFPDFDocument doc = OpenRaw(p);
  ASSERT_TRUE(doc);
  EPDF_SIG_PREPARE o = DefaultPrepare();
  ScopedFPDFWideString name = GetFPDFWideString(L"group.total");
  const FPDF_WIDESTRING names[] = {name.get()};
  o.fieldmdp_action = EPDF_SIG_FIELD_ACTION_INCLUDE;
  o.fieldmdp_fields = names;
  o.fieldmdp_field_count = 1;
  ASSERT_NE(0u, EPDFSig_Prepare(doc.get(), 6, &o));
  EXPECT_EQ(4097, LocalFieldFlags(doc.get(), 5));  // Multiline kept, ReadOnly added
}

TEST_F(EPDFSignatureEmbedderTest, ProbeLockParentCoversChildren) {
  RawPdf p = FormObjects();
  ScopedFPDFDocument doc = OpenRaw(p);
  ASSERT_TRUE(doc);
  EPDF_SIG_PREPARE o = DefaultPrepare();
  ScopedFPDFWideString name = GetFPDFWideString(L"group");
  const FPDF_WIDESTRING names[] = {name.get()};
  o.fieldmdp_action = EPDF_SIG_FIELD_ACTION_INCLUDE;
  o.fieldmdp_fields = names;
  o.fieldmdp_field_count = 1;
  ASSERT_NE(0u, EPDFSig_Prepare(doc.get(), 6, &o));
  EXPECT_TRUE(LocalFieldFlags(doc.get(), 5) & 1);
  EXPECT_FALSE(EPDFSig_Prepare(doc.get(), 6, &o));  // already prepared
  // Bad counts are refused, not converted.
  RawPdf p2 = FormObjects();
  ScopedFPDFDocument doc2 = OpenRaw(p2);
  o.fieldmdp_field_count = -1;
  EXPECT_EQ(0u, EPDFSig_Prepare(doc2.get(), 6, &o));
  EXPECT_FALSE(EPDFSig_SetFieldLock(doc2.get(), 6, EPDF_SIG_FIELD_ACTION_INCLUDE, names, -1, 0));
}

TEST_F(EPDFSignatureEmbedderTest, ProbeExistingLockBecomesFieldMdp) {
  RawPdf p = FormObjects("/Lock<</Type/SigFieldLock /Action/All /P 2>>");
  ScopedFPDFDocument doc = OpenRaw(p);
  ASSERT_TRUE(doc);
  // A request that contradicts the field's lock is refused ...
  EPDF_SIG_PREPARE conflicting = DefaultPrepare();
  ScopedFPDFWideString name = GetFPDFWideString(L"group");
  const FPDF_WIDESTRING names[] = {name.get()};
  conflicting.fieldmdp_action = EPDF_SIG_FIELD_ACTION_INCLUDE;
  conflicting.fieldmdp_fields = names;
  conflicting.fieldmdp_field_count = 1;
  EXPECT_EQ(0u, EPDFSig_Prepare(doc.get(), 6, &conflicting));
  conflicting = DefaultPrepare();
  conflicting.lock_permission = 1;
  EXPECT_EQ(0u, EPDFSig_Prepare(doc.get(), 6, &conflicting));
  // ... and default options apply the lock as the FieldMDP transform.
  EPDF_SIG_PREPARE o = DefaultPrepare();
  ASSERT_NE(0u, EPDFSig_Prepare(doc.get(), 6, &o));
  EPDF_SIGNATURE_MODEL m = EPDFSig_LoadModel(doc.get());
  ASSERT_TRUE(m);
  EXPECT_EQ(EPDF_SIG_FIELD_ACTION_ALL, EPDFSig_GetFieldMDPAction(m, 0));
  EXPECT_EQ(EPDF_SIG_FIELD_ACTION_ALL, EPDFSig_GetLockAction(m, 0));
  EXPECT_EQ(2, EPDFSig_GetLockPermission(m, 0));
  EPDFSig_CloseModel(m);
  EXPECT_TRUE(LocalFieldFlags(doc.get(), 5) & 1);  // /All locked the text field
}

TEST_F(EPDFSignatureEmbedderTest, ProbeNestedBerContentsLength) {
  std::vector<std::string> b = BasicObjects("/AcroForm<</Fields[4 0 R]>>");
  b.push_back("<</FT/Sig /T(sig) /V 5 0 R>>");
  // 30 80 { 30 80 { 02 01 01 } 00 00 } 00 00, then zero padding.
  b.push_back("<</Type/Sig /ByteRange[0 0 0 0] /Contents<308030800201010000000000000000>>>");
  RawPdf p = MakeRawPdf(b);
  ScopedFPDFDocument doc = OpenRaw(p);
  ASSERT_TRUE(doc);
  EPDF_SIGNATURE_MODEL m = EPDFSig_LoadModel(doc.get());
  ASSERT_TRUE(m);
  EXPECT_EQ(11u, EPDFSig_GetContents(m, 0, nullptr, 0));
  EPDFSig_CloseModel(m);
  // A BER object that never closes is malformed.
  b.back() = "<</Type/Sig /ByteRange[0 0 0 0] /Contents<30803080020101000000>>>";
  RawPdf q = MakeRawPdf(b);
  ScopedFPDFDocument doc2 = OpenRaw(q);
  ASSERT_TRUE(doc2);
  m = EPDFSig_LoadModel(doc2.get());
  EXPECT_EQ(0u, EPDFSig_GetContents(m, 0, nullptr, 0));
  EXPECT_EQ(EPDF_SIG_COVERAGE_MALFORMED, EPDFSig_GetCoverage(m, 0));
  EPDFSig_CloseModel(m);
}

TEST_F(EPDFSignatureEmbedderTest, ProbeMdpStructuresAreDirect) {
  RawPdf p = FormObjects();
  ScopedFPDFDocument doc = OpenRaw(p);
  ASSERT_TRUE(doc);
  EPDF_SIG_PREPARE o = DefaultPrepare();
  o.docmdp_permission = 2;
  ScopedFPDFWideString name = GetFPDFWideString(L"group.total");
  const FPDF_WIDESTRING names[] = {name.get()};
  o.fieldmdp_action = EPDF_SIG_FIELD_ACTION_INCLUDE;
  o.fieldmdp_fields = names;
  o.fieldmdp_field_count = 1;
  const uint32_t sig = EPDFSig_Prepare(doc.get(), 6, &o);
  ASSERT_NE(0u, sig);
  CPDF_Document* native = CPDFDocumentFromFPDFDocument(doc.get());
  RetainPtr<const CPDF_Dictionary> value = ToDictionary(native->GetOrParseIndirectObject(sig));
  ASSERT_TRUE(value);
  RetainPtr<const CPDF_Object> reference = value->GetObjectFor("Reference");
  ASSERT_TRUE(reference);
  EXPECT_FALSE(reference->IsReference());
  const CPDF_Array* references = reference->AsArray();
  ASSERT_TRUE(references);
  ASSERT_EQ(2u, references->size());
  for (size_t i = 0; i < references->size(); ++i) {
    RetainPtr<const CPDF_Object> item = references->GetObjectAt(i);
    EXPECT_FALSE(item->IsReference());
    RetainPtr<const CPDF_Dictionary> ref_dict = item->AsDictionary() ? ToDictionary(item) : nullptr;
    ASSERT_TRUE(ref_dict);
    RetainPtr<const CPDF_Object> params = ref_dict->GetObjectFor("TransformParams");
    ASSERT_TRUE(params);
    EXPECT_FALSE(params->IsReference());
  }
  EXPECT_FALSE(value->GetObjectFor("ByteRange")->IsReference());
  EXPECT_FALSE(value->GetObjectFor("Contents")->IsReference());
}

TEST_F(EPDFSignatureEmbedderTest, ProbeDuplicateContentsKeyIsNotWhole) {
  // PDFium keeps the last duplicate key; a raw scan that kept the first
  // would judge the wrong value. Duplicate keys are malformed: never whole.
  std::vector<std::string> b = BasicObjects("/AcroForm<</Fields[4 0 R]>>");
  b.push_back("<</FT/Sig /T(sig) /V 5 0 R>>");
  const std::string placeholder = "0000000000 0000000000 0000000000 0000000000";
  b.push_back("<</Type/Sig /ByteRange[" + placeholder +
              "] /Contents<3006020101020102> /Contents<3006020101020102>>>");
  RawPdf p = MakeRawPdf(b);
  const size_t first = p.bytes.find("<3006020101020102>");
  char range[64];
  snprintf(range, sizeof(range), "%010u %010zu %010zu %010zu", 0u, first, first + 18,
           p.bytes.size() - first - 18);
  p.bytes.replace(p.bytes.find(placeholder), placeholder.size(), range);
  ScopedFPDFDocument doc = OpenRaw(p);
  ASSERT_TRUE(doc);
  EPDF_SIGNATURE_MODEL m = EPDFSig_LoadModel(doc.get());
  ASSERT_TRUE(m);
  EXPECT_NE(EPDF_SIG_COVERAGE_WHOLE_REVISION, EPDFSig_GetCoverage(m, 0));
  EPDFSig_CloseModel(m);
}

TEST_F(EPDFSignatureEmbedderTest, ProbeSeedValueVersionAndApprovalOnly) {
  // A required parser capability newer than the PDF 1.7 entry set: refused.
  RawPdf p = FormObjects("/SV<</Ff 4 /V 99>>");
  ScopedFPDFDocument doc = OpenRaw(p);
  ASSERT_TRUE(doc);
  EPDF_SIG_PREPARE o = DefaultPrepare();
  EXPECT_EQ(0u, EPDFSig_Prepare(doc.get(), 6, &o));
  EPDF_SIGNATURE_MODEL m = EPDFSig_LoadModel(doc.get());
  EXPECT_EQ(99, EPDFSig_GetSeedValueVersion(m, 0));
  EXPECT_TRUE(EPDFSig_GetSeedValueRequiredFlags(m, 0) & EPDF_SIG_SV_V);
  EPDFSig_CloseModel(m);
  // The same requirement at version 2 is fine.
  RawPdf p2 = FormObjects("/SV<</Ff 4 /V 2>>");
  ScopedFPDFDocument doc2 = OpenRaw(p2);
  ASSERT_TRUE(doc2);
  EXPECT_NE(0u, EPDFSig_Prepare(doc2.get(), 6, &o));

  // /MDP /P 0: approval only. A certification is refused, an approval is not.
  RawPdf p3 = FormObjects("/SV<</MDP<</P 0>>>>");
  ScopedFPDFDocument doc3 = OpenRaw(p3);
  ASSERT_TRUE(doc3);
  m = EPDFSig_LoadModel(doc3.get());
  EXPECT_TRUE(EPDFSig_GetSeedValuePresentFlags(m, 0) & EPDF_SIG_SV_MDP);
  EXPECT_EQ(0, EPDFSig_GetSeedValueMDP(m, 0));
  EPDFSig_CloseModel(m);
  EPDF_SIG_PREPARE certify = DefaultPrepare();
  certify.docmdp_permission = 2;
  EXPECT_EQ(0u, EPDFSig_Prepare(doc3.get(), 6, &certify));
  EXPECT_NE(0u, EPDFSig_Prepare(doc3.get(), 6, &o));

  // /MDP without /P imposes no constraint: a certification is fine.
  RawPdf p5 = FormObjects("/SV<</MDP<<>>>>");
  ScopedFPDFDocument doc5 = OpenRaw(p5);
  ASSERT_TRUE(doc5);
  m = EPDFSig_LoadModel(doc5.get());
  EXPECT_FALSE(EPDFSig_GetSeedValuePresentFlags(m, 0) & EPDF_SIG_SV_MDP);
  EPDFSig_CloseModel(m);
  EXPECT_NE(0u, EPDFSig_Prepare(doc5.get(), 6, &certify));

  // /MDP /P 2: a certification at that level is forced; other levels refused.
  RawPdf p4 = FormObjects("/SV<</MDP<</P 2>>>>");
  ScopedFPDFDocument doc4 = OpenRaw(p4);
  ASSERT_TRUE(doc4);
  certify.docmdp_permission = 1;
  EXPECT_EQ(0u, EPDFSig_Prepare(doc4.get(), 6, &certify));
  const uint32_t value = EPDFSig_Prepare(doc4.get(), 6, &o);
  ASSERT_NE(0u, value);
  m = EPDFSig_LoadModel(doc4.get());
  EXPECT_EQ(2, EPDFSig_GetDocMDPPermission(m, 0));
  EXPECT_TRUE(EPDFSig_IsCatalogCertification(m, 0));
  EPDFSig_CloseModel(m);
}

// ---------------------------------------------------------------------------
// Layer documents. A layer's own parser is the base parser and its delta
// objects are in-memory clones; the bytes it was loaded from are base +
// delta. Revision analysis must read exactly those bytes - never the base
// alone, never the base document's reachable-only object cache - while
// candidate editing stays on the layer.
// ---------------------------------------------------------------------------

namespace {

// A memory-backed base document; the bytes must outlive it, so keep a copy.
class ScopedBase {
 public:
  explicit ScopedBase(std::string bytes) : bytes_(std::move(bytes)) {
    base_ = EPDF_LoadMemBaseDocument64(bytes_.data(), bytes_.size(), nullptr);
  }
  ~ScopedBase() {
    if (base_) {
      EPDF_ReleaseBaseDocument(base_);
    }
  }
  EPDF_BASE_DOCUMENT get() const { return base_; }
  const std::string& bytes() const { return bytes_; }

 private:
  std::string bytes_;
  EPDF_BASE_DOCUMENT base_ = nullptr;
};

// Opens a layer over |base| with |delta| bytes. The FPDF_FILEACCESS and the
// buffer it reads live only inside this call, on purpose: a layer keeps the
// delta it ingested as part of its loaded bytes, so it must have copied it.
ScopedFPDFDocument OpenLayer(EPDF_BASE_DOCUMENT base, const std::string& delta = std::string()) {
  std::string scratch = delta;
  FPDF_FILEACCESS access = {};
  access.m_FileLen = static_cast<unsigned long>(scratch.size());
  access.m_Param = &scratch;
  access.m_GetBlock = [](void* param, unsigned long pos, unsigned char* out,
                         unsigned long size) -> int {
    const std::string& b = *static_cast<std::string*>(param);
    if (pos > b.size() || size > b.size() - pos) {
      return 0;
    }
    memcpy(out, b.data() + pos, size);
    return 1;
  };
  EPDFLayerOpenStatus status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument doc(
      EPDFLayer_OpenLayer(base, scratch.empty() ? nullptr : &access, nullptr, &status));
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, status);
  return doc;
}

std::string SaveDelta(FPDF_DOCUMENT layer) {
  unsigned long size = 0;
  EPDFLayerSaveStatus status = EPDFLayerSaveStatus_kSaveFailed;
  void* raw = EPDFLayer_SaveDeltaToOwnedBuffer(layer, &size, &status);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, status);
  if (!raw) {
    return std::string();
  }
  std::string delta(static_cast<char*>(raw), size);
  EPDF_FreeBuffer(raw);
  return delta;
}

ScopedFPDFDocument OpenBytes(const std::string& bytes) {
  return ScopedFPDFDocument(FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr));
}

std::string BytesOf(const Sealed& sealed) {
  return std::string(reinterpret_cast<const char*>(sealed.bytes.data()), sealed.bytes.size());
}

int CoverageOf(FPDF_DOCUMENT doc, int index) {
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(doc);
  if (!model) {
    return -1;
  }
  const int coverage = EPDFSig_GetCoverage(model, index);
  EPDFSig_CloseModel(model);
  return coverage;
}

// SHA-256 over signature |index|'s /ByteRange, as hex; empty when there is
// no usable range.
std::string DigestOf(FPDF_DOCUMENT doc, int index) {
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(doc);
  if (!model) {
    return std::string();
  }
  unsigned long long range[4];
  const bool has_range = EPDFSig_GetByteRange(model, index, range);
  EPDFSig_CloseModel(model);
  return has_range ? HexDigest(doc, range, EPDF_DIGEST_SHA256) : std::string();
}

// Two unsigned signature fields (objects 4 and 5) and a text field (6).
RawPdf TwoSignatureFields() {
  std::vector<std::string> b = BasicObjects("/AcroForm<</Fields[4 0 R 5 0 R 6 0 R]>>");
  b.push_back("<</FT/Sig /T(first)>>");
  b.push_back("<</FT/Sig /T(second)>>");
  b.push_back("<</FT/Tx /T(note) /V(before)>>");
  return MakeRawPdf(b);
}

std::string NotarialFixtureBytes() {
  const std::string path =
      PathService::GetTestFilePath("embedpdf_two_signatures_encrypted.pdf");
  std::vector<uint8_t> bytes = GetFileContents(path.c_str());
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace

TEST_F(EPDFSignatureEmbedderTest, LayerModelReadsDoNotPromote) {
  RawPdf p = FormObjects();
  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument a = OpenLayer(base.get());
  ScopedFPDFDocument b = OpenLayer(base.get());
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(a.get());
  ASSERT_TRUE(model);
  EXPECT_EQ(1, EPDFSig_Count(model));
  EXPECT_FALSE(EPDFSig_IsSigned(model, 0));
  EXPECT_EQ(1, EPDFDoc_GetRevisionCount(a.get()));
  // Reading promotes nothing, on either layer.
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(a.get()));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(b.get()));
  // The model is a snapshot: it outlives the layer it was read from.
  a.reset();
  EXPECT_EQ(1, EPDFSig_Count(model));
  EXPECT_EQ(L"sig", ReadFieldName(model, 0));
  EPDFSig_CloseModel(model);
}

TEST_F(EPDFSignatureEmbedderTest, LayerPrepareAndSaveStayInCandidate) {
  RawPdf p = FormObjects("/Lock<</Action/All>>");
  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument candidate = OpenLayer(base.get());
  ScopedFPDFDocument sibling = OpenLayer(base.get());
  ASSERT_TRUE(candidate);
  ASSERT_TRUE(sibling);
  EPDF_SIG_PREPARE opts = DefaultPrepare();
  const uint32_t value = EPDFSig_Prepare(candidate.get(), 6, &opts);
  ASSERT_NE(0u, value);
  // The lock's ReadOnly landed on the kid in the candidate only.
  EXPECT_EQ(4097, LocalFieldFlags(candidate.get(), 5));
  EXPECT_EQ(0, LocalFieldFlags(sibling.get(), 5));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(sibling.get()));
  EPDF_SIGNATURE_MODEL other = EPDFSig_LoadModel(sibling.get());
  ASSERT_TRUE(other);
  EXPECT_FALSE(EPDFSig_IsSigned(other, 0));
  EPDFSig_CloseModel(other);

  Sealed sealed;
  ASSERT_TRUE(SealPrepared(candidate.get(), value, opts.digest, &sealed));
  const std::string signed_bytes = BytesOf(sealed);
  // Saving a layer appends to the base bytes verbatim.
  EXPECT_EQ(p.bytes, signed_bytes.substr(0, p.bytes.size()));
  ScopedFPDFDocument signed_doc = OpenBytes(signed_bytes);
  ASSERT_TRUE(signed_doc);
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, CoverageOf(signed_doc.get(), 0));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(sibling.get()));
}

TEST_F(EPDFSignatureEmbedderTest, LayerRejectedPrepareLeavesOverlayEmpty) {
  RawPdf p = FormObjects("/SV<</Ff 4 /V 99>>");
  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument layer = OpenLayer(base.get());
  ASSERT_TRUE(layer);
  EPDF_SIG_PREPARE opts = DefaultPrepare();
  EXPECT_EQ(0u, EPDFSig_Prepare(layer.get(), 6, &opts));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(layer.get()));
}

TEST_F(EPDFSignatureEmbedderTest, LayerFieldLockSurvivesDeltaReopen) {
  RawPdf p = FormObjects();
  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument a = OpenLayer(base.get());
  ScopedFPDFDocument b = OpenLayer(base.get());
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  ASSERT_TRUE(EPDFSig_SetFieldLock(a.get(), 6, EPDF_SIG_FIELD_ACTION_ALL, nullptr, 0, 2));
  const std::string delta = SaveDelta(a.get());
  ASSERT_FALSE(delta.empty());
  ScopedFPDFDocument reopened = OpenLayer(base.get(), delta);
  ASSERT_TRUE(reopened);
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(reopened.get());
  ASSERT_TRUE(model);
  EXPECT_EQ(EPDF_SIG_FIELD_ACTION_ALL, EPDFSig_GetLockAction(model, 0));
  EXPECT_EQ(2, EPDFSig_GetLockPermission(model, 0));
  EPDFSig_CloseModel(model);
  model = EPDFSig_LoadModel(b.get());
  ASSERT_TRUE(model);
  EXPECT_EQ(EPDF_SIG_FIELD_ACTION_NONE, EPDFSig_GetLockAction(model, 0));
  EPDFSig_CloseModel(model);
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(b.get()));
}

TEST_F(EPDFSignatureEmbedderTest, LayerReopenedSignedDeltaReportsItsLoadedBytes) {
  // Sign as a plain document, then replay the very same bytes as base +
  // delta. The layer was loaded from base + delta, so it must report what
  // the plain document reports: two revisions, WHOLE coverage, one digest.
  RawPdf p = FormObjects();
  ScopedFPDFDocument plain = OpenRaw(p);
  ASSERT_TRUE(plain);
  Sealed sealed;
  ASSERT_TRUE(SignField(plain.get(), 6, DefaultPrepare(), &sealed));
  const std::string signed_bytes = BytesOf(sealed);
  ScopedFPDFDocument standalone = OpenBytes(signed_bytes);
  ASSERT_TRUE(standalone);
  ASSERT_EQ(2, EPDFDoc_GetRevisionCount(standalone.get()));
  ASSERT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, CoverageOf(standalone.get(), 0));

  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument replay = OpenLayer(base.get(), signed_bytes.substr(p.bytes.size()));
  ASSERT_TRUE(replay);
  EXPECT_EQ(2, EPDFDoc_GetRevisionCount(replay.get()));
  unsigned long long end = 0;
  ASSERT_TRUE(EPDFDoc_GetRevision(replay.get(), 1, &end, nullptr));
  EXPECT_EQ(signed_bytes.size(), end);
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, CoverageOf(replay.get(), 0));
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(replay.get());
  ASSERT_TRUE(model);
  EXPECT_EQ(1, EPDFSig_GetRevisionIndex(model, 0));
  EPDFSig_CloseModel(model);
  EXPECT_EQ(DigestOf(standalone.get(), 0), DigestOf(replay.get(), 0));
  EXPECT_EQ(sealed.digest_hex, DigestOf(replay.get(), 0));
  // The original revision opens from the layer's bytes as well.
  ScopedFPDFDocument original(EPDFDoc_OpenRevision(replay.get(), p.bytes.size()));
  ASSERT_TRUE(original);
  EXPECT_EQ(1, EPDFDoc_GetRevisionCount(original.get()));
  // The delta's objects are promoted clones; the analysis above did not
  // read them, it read the bytes.
  EXPECT_GT(EPDFLayer_GetPromotedObjectCount(replay.get()), 0ul);
  // And the diff between them is the signing update, read from the bytes.
  EPDF_OBJECT_DIFF diff = EPDFDoc_CompareRevisions(original.get(), replay.get());
  ASSERT_TRUE(diff);
  const std::vector<DiffRow> rows = ReadDiffRows(diff);
  EXPECT_GT(rows.size(), 0u);
  const int field_row = FindRow(rows, 6);
  ASSERT_GE(field_row, 0);
  EXPECT_EQ(EPDF_DIFF_MODIFIED, rows[field_row].change);
  EXPECT_NE(std::string::npos, ReadDiffValue(diff, field_row, EPDF_DIFF_NEW).find("/V "));
  EPDFObjectDiff_Close(diff);
}

TEST_F(EPDFSignatureEmbedderTest, LayerCompareLoadedDeltaReportsChanges) {
  RawPdf p = FormObjects();
  RawPdf n = AppendRawUpdate(p, 5, "<</T(total) /Parent 4 0 R /V(changed)>>", 7);
  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument a = OpenLayer(base.get());
  ScopedFPDFDocument b = OpenLayer(base.get(), n.bytes.substr(p.bytes.size()));
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  // Two layers over one base: the one with a delta has one more revision.
  EXPECT_EQ(1, EPDFDoc_GetRevisionCount(a.get()));
  EXPECT_EQ(2, EPDFDoc_GetRevisionCount(b.get()));
  EPDF_OBJECT_DIFF diff = EPDFDoc_CompareRevisions(a.get(), b.get());
  ASSERT_TRUE(diff);
  const std::vector<DiffRow> rows = ReadDiffRows(diff);
  // Object 5 only: the update's trailer carries the same values.
  ASSERT_EQ(1u, rows.size());
  EXPECT_EQ(5u, rows[0].obj_num);
  EXPECT_EQ(EPDF_DIFF_MODIFIED, rows[0].change);
  EXPECT_NE(std::string::npos, ReadDiffValue(diff, 0, EPDF_DIFF_OLD).find("(text)"));
  EXPECT_NE(std::string::npos, ReadDiffValue(diff, 0, EPDF_DIFF_NEW).find("(changed)"));
  EPDFObjectDiff_Close(diff);
  // The layered comparison equals the plain one over the same bytes.
  ScopedFPDFDocument older = OpenRaw(p);
  ScopedFPDFDocument newer = OpenRaw(n);
  EPDF_OBJECT_DIFF plain = EPDFDoc_CompareRevisions(older.get(), newer.get());
  ASSERT_TRUE(plain);
  const std::vector<DiffRow> plain_rows = ReadDiffRows(plain);
  ASSERT_EQ(rows.size(), plain_rows.size());
  EPDFObjectDiff_Close(plain);
  // Two fresh layers over one base are the same revision: an empty diff.
  ScopedFPDFDocument c = OpenLayer(base.get());
  diff = EPDFDoc_CompareRevisions(a.get(), c.get());
  ASSERT_TRUE(diff);
  EXPECT_EQ(0, EPDFObjectDiff_GetCount(diff));
  EPDFObjectDiff_Close(diff);
}

TEST_F(EPDFSignatureEmbedderTest, LayerFreshBaseDiffKeepsUnreachableObjectValues) {
  // Object 4 is referenced by nothing; a base document's cache holds
  // reachable objects only. Values come from the bytes, so the layered diff
  // reports the same value as the plain one.
  std::vector<std::string> b = BasicObjects();
  b.push_back("(before)");
  RawPdf p = MakeRawPdf(b);
  RawPdf n = AppendRawUpdate(p, 4, "(after)", 5);
  ScopedBase base(n.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument layer = OpenLayer(base.get());
  ScopedFPDFDocument older = OpenRaw(p);
  ScopedFPDFDocument newer = OpenRaw(n);
  ASSERT_TRUE(layer);
  ASSERT_TRUE(older);
  ASSERT_TRUE(newer);
  EPDF_OBJECT_DIFF plain = EPDFDoc_CompareRevisions(older.get(), newer.get());
  EPDF_OBJECT_DIFF layered = EPDFDoc_CompareRevisions(older.get(), layer.get());
  ASSERT_TRUE(plain);
  ASSERT_TRUE(layered);
  const std::vector<DiffRow> plain_rows = ReadDiffRows(plain);
  const std::vector<DiffRow> layered_rows = ReadDiffRows(layered);
  const int plain_row = FindRow(plain_rows, 4);
  const int layered_row = FindRow(layered_rows, 4);
  ASSERT_GE(plain_row, 0);
  ASSERT_GE(layered_row, 0);
  EXPECT_EQ("(after)", ReadDiffValue(plain, plain_row, EPDF_DIFF_NEW));
  EXPECT_EQ(ReadDiffValue(plain, plain_row, EPDF_DIFF_NEW),
            ReadDiffValue(layered, layered_row, EPDF_DIFF_NEW));
  EXPECT_EQ("(before)", ReadDiffValue(layered, layered_row, EPDF_DIFF_OLD));
  EPDFObjectDiff_Close(plain);
  EPDFObjectDiff_Close(layered);
}

TEST_F(EPDFSignatureEmbedderTest, PlainDiffValuesComeFromBytesNotCache) {
  // An unsaved in-memory edit is not part of any revision: the diff keeps
  // reporting the value the bytes hold.
  std::vector<std::string> b = BasicObjects();
  b.push_back("<</K(before)>>");
  RawPdf p = MakeRawPdf(b);
  RawPdf n = AppendRawUpdate(p, 4, "<</K(after)>>", 5);
  ScopedFPDFDocument newer = OpenRaw(n);
  ASSERT_TRUE(newer);
  ScopedFPDFDocument older(EPDFDoc_OpenRevision(newer.get(), p.bytes.size()));
  ASSERT_TRUE(older);
  {
    CPDF_Document* native = CPDFDocumentFromFPDFDocument(newer.get());
    RetainPtr<CPDF_Dictionary> dict = ToDictionary(native->GetMutableIndirectObject(4));
    ASSERT_TRUE(dict);
    dict->SetNewFor<CPDF_String>("K", "edited");
  }
  EPDF_OBJECT_DIFF diff = EPDFDoc_CompareRevisions(older.get(), newer.get());
  ASSERT_TRUE(diff);
  const std::vector<DiffRow> rows = ReadDiffRows(diff);
  const int row = FindRow(rows, 4);
  ASSERT_GE(row, 0);
  EXPECT_EQ("<</K (after)>>", ReadDiffValue(diff, row, EPDF_DIFF_NEW));
  EXPECT_EQ("<</K (before)>>", ReadDiffValue(diff, row, EPDF_DIFF_OLD));
  EPDFObjectDiff_Close(diff);
}

TEST_F(EPDFSignatureEmbedderTest, LayerFinalizedBytesAsNewBasePreserveConsecutiveSignatures) {
  RawPdf p = TwoSignatureFields();
  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument first_candidate = OpenLayer(base.get());
  ASSERT_TRUE(first_candidate);
  Sealed first;
  ASSERT_TRUE(SignField(first_candidate.get(), 4, DefaultPrepare(), &first));
  const std::string first_bytes = BytesOf(first);

  // Completion: the signed bytes become a new immutable base.
  ScopedBase signed_base(first_bytes);
  ASSERT_TRUE(signed_base.get());
  ScopedFPDFDocument next = OpenLayer(signed_base.get());
  ASSERT_TRUE(next);
  ASSERT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, CoverageOf(next.get(), 0));
  const std::string digest = DigestOf(next.get(), 0);
  ASSERT_FALSE(digest.empty());
  EXPECT_EQ(first.digest_hex, digest);
  Sealed second;
  ASSERT_TRUE(SignField(next.get(), 5, DefaultPrepare(), &second));
  const std::string second_bytes = BytesOf(second);
  EXPECT_EQ(first_bytes, second_bytes.substr(0, first_bytes.size()));

  ScopedFPDFDocument reopened = OpenBytes(second_bytes);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(3, EPDFDoc_GetRevisionCount(reopened.get()));
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, CoverageOf(reopened.get(), 0));
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, CoverageOf(reopened.get(), 1));
  EXPECT_EQ(digest, DigestOf(reopened.get(), 0));
  EXPECT_EQ(second.digest_hex, DigestOf(reopened.get(), 1));

  // The historic revision opens from the layer, and the diff is real.
  ScopedFPDFDocument historic(EPDFDoc_OpenRevision(next.get(), p.bytes.size()));
  ASSERT_TRUE(historic);
  EPDF_OBJECT_DIFF diff = EPDFDoc_CompareRevisions(historic.get(), next.get());
  ASSERT_TRUE(diff);
  EXPECT_GT(EPDFObjectDiff_GetCount(diff), 0);
  EPDFObjectDiff_Close(diff);
}

TEST_F(EPDFSignatureEmbedderTest, LayerPrepareRefusesSignedBytesInLoadedDelta) {
  // A layer whose loaded delta carries the signed bytes cannot take another
  // signature: saving it would append to the BASE and rewrite the delta,
  // dropping what the first signature sealed. The same bytes as a base, or
  // a delta holding only unsigned edits over a signed base, are fine.
  RawPdf p = TwoSignatureFields();
  ScopedFPDFDocument plain = OpenRaw(p);
  ASSERT_TRUE(plain);
  Sealed first;
  ASSERT_TRUE(SignField(plain.get(), 4, DefaultPrepare(), &first));
  const std::string signed_bytes = BytesOf(first);

  ScopedBase unsigned_base(p.bytes);
  ASSERT_TRUE(unsigned_base.get());
  ScopedFPDFDocument signed_delta_layer =
      OpenLayer(unsigned_base.get(), signed_bytes.substr(p.bytes.size()));
  ASSERT_TRUE(signed_delta_layer);
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, CoverageOf(signed_delta_layer.get(), 0));
  EPDF_SIG_PREPARE opts = DefaultPrepare();
  EXPECT_EQ(0u, EPDFSig_Prepare(signed_delta_layer.get(), 5, &opts));
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, CoverageOf(signed_delta_layer.get(), 0));

  ScopedBase signed_base(signed_bytes);
  ASSERT_TRUE(signed_base.get());
  ScopedFPDFDocument editing = OpenLayer(signed_base.get());
  ASSERT_TRUE(editing);
  // A lock is authoring-time only: after a signature it would be a change
  // to a field dictionary that the first signature does not permit.
  EXPECT_FALSE(EPDFSig_SetFieldLock(editing.get(), 5, EPDF_SIG_FIELD_ACTION_ALL, nullptr, 0, 0));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(editing.get()));
  // Filling a text field is a permitted change: an unsigned delta.
  ScopedFPDFWideString after = GetFPDFWideString(L"after");
  ASSERT_TRUE(EPDFForm_SetTextValue(editing.get(), 6, after.get(), nullptr, 0, nullptr));
  const std::string edits = SaveDelta(editing.get());
  ASSERT_FALSE(edits.empty());
  ScopedFPDFDocument unsigned_delta_layer = OpenLayer(signed_base.get(), edits);
  ASSERT_TRUE(unsigned_delta_layer);
  // Original + signing update + the delta's edits.
  EXPECT_EQ(3, EPDFDoc_GetRevisionCount(unsigned_delta_layer.get()));
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, CoverageOf(unsigned_delta_layer.get(), 0));
  Sealed second;
  ASSERT_TRUE(SignField(unsigned_delta_layer.get(), 5, opts, &second));
  const std::string second_bytes = BytesOf(second);
  EXPECT_EQ(signed_bytes, second_bytes.substr(0, signed_bytes.size()));
  ScopedFPDFDocument reopened = OpenBytes(second_bytes);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, CoverageOf(reopened.get(), 0));
  EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, CoverageOf(reopened.get(), 1));
  EXPECT_EQ(first.digest_hex, DigestOf(reopened.get(), 0));
}

TEST_F(EPDFSignatureEmbedderTest, LayerPromotedAncestorIsUsedForSignatureFieldIdentity) {
  std::vector<std::string> b = BasicObjects("/AcroForm<</Fields[4 0 R]>>");
  b.push_back("<</T(group) /Kids[5 0 R]>>");
  b.push_back("<</T(sig) /FT/Sig /Parent 4 0 R>>");
  RawPdf p = MakeRawPdf(b);
  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument a = OpenLayer(base.get());
  ScopedFPDFDocument sibling = OpenLayer(base.get());
  ASSERT_TRUE(a);
  ASSERT_TRUE(sibling);
  ScopedFPDFWideString renamed = GetFPDFWideString(L"renamed");
  ASSERT_TRUE(EPDFForm_SetFieldName(a.get(), 4, renamed.get()));
  const unsigned long promoted = EPDFLayer_GetPromotedObjectCount(a.get());
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(a.get());
  ASSERT_TRUE(model);
  EXPECT_EQ(L"renamed.sig", ReadFieldName(model, 0));
  EXPECT_EQ(promoted, EPDFLayer_GetPromotedObjectCount(a.get()));
  EPDFSig_CloseModel(model);
  EPDF_SIG_PREPARE opts = DefaultPrepare();
  ASSERT_NE(0u, EPDFSig_Prepare(a.get(), 5, &opts));
  model = EPDFSig_LoadModel(sibling.get());
  ASSERT_TRUE(model);
  EXPECT_FALSE(EPDFSig_IsSigned(model, 0));
  EXPECT_EQ(L"group.sig", ReadFieldName(model, 0));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(sibling.get()));
  EPDFSig_CloseModel(model);
}

TEST_F(EPDFSignatureEmbedderTest, LayerEncryptedBaseModelMatchesOrdinaryDocument) {
  const std::string bytes = NotarialFixtureBytes();
  if (bytes.empty()) {
    GTEST_SKIP() << "fixture not available";
  }
  ScopedBase base(bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument layer = OpenLayer(base.get());
  ScopedFPDFDocument plain = OpenBytes(bytes);
  ASSERT_TRUE(layer);
  ASSERT_TRUE(plain);
  EPDF_SIGNATURE_MODEL from_layer = EPDFSig_LoadModel(layer.get());
  EPDF_SIGNATURE_MODEL from_plain = EPDFSig_LoadModel(plain.get());
  ASSERT_TRUE(from_layer);
  ASSERT_TRUE(from_plain);
  EXPECT_EQ(EPDFSig_Count(from_plain), EPDFSig_Count(from_layer));
  EXPECT_EQ(4, EPDFDoc_GetRevisionCount(layer.get()));
  EXPECT_EQ(EPDFDoc_GetRevisionCount(plain.get()), EPDFDoc_GetRevisionCount(layer.get()));
  for (int i = 0; i < EPDFSig_Count(from_layer); ++i) {
    EXPECT_EQ(EPDFSig_GetCoverage(from_plain, i), EPDFSig_GetCoverage(from_layer, i));
    EXPECT_EQ(EPDFSig_GetRevisionIndex(from_plain, i), EPDFSig_GetRevisionIndex(from_layer, i));
    EXPECT_EQ(DigestOf(plain.get(), i), DigestOf(layer.get(), i));
  }
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(layer.get()));
  EPDFSig_CloseModel(from_layer);
  EPDFSig_CloseModel(from_plain);
}

TEST_F(EPDFSignatureEmbedderTest, LayerEncryptedSigningPreservesExistingSignatures) {
  const std::string bytes = NotarialFixtureBytes();
  if (bytes.empty()) {
    GTEST_SKIP() << "fixture not available";
  }
  ScopedBase base(bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument candidate = OpenLayer(base.get());
  ScopedFPDFDocument sibling = OpenLayer(base.get());
  ASSERT_TRUE(candidate);
  ASSERT_TRUE(sibling);
  const std::string first = DigestOf(candidate.get(), 0);
  const std::string second = DigestOf(candidate.get(), 1);
  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  ScopedFPDFWideString name = GetFPDFWideString(L"third");
  const uint32_t field =
      EPDFForm_CreateField(candidate.get(), EPDF_FORMFIELD_FAMILY_SIGNATURE, name.get());
  ASSERT_NE(0u, field);
  EPDF_SIG_PREPARE opts = DefaultPrepare();
  opts.subfilter = EPDF_SIG_SUBFILTER_ADBE_PKCS7_DETACHED;
  Sealed sealed;
  ASSERT_TRUE(SignField(candidate.get(), field, opts, &sealed));
  const std::string signed_bytes = BytesOf(sealed);
  EXPECT_EQ(bytes, signed_bytes.substr(0, bytes.size()));
  ScopedFPDFDocument signed_doc = OpenBytes(signed_bytes);
  ASSERT_TRUE(signed_doc);
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(signed_doc.get());
  ASSERT_TRUE(model);
  EXPECT_EQ(3, EPDFSig_Count(model));
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(EPDF_SIG_COVERAGE_WHOLE_REVISION, EPDFSig_GetCoverage(model, i)) << i;
  }
  EPDFSig_CloseModel(model);
  EXPECT_EQ(first, DigestOf(signed_doc.get(), 0));
  EXPECT_EQ(second, DigestOf(signed_doc.get(), 1));
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(sibling.get()));
}

TEST_F(EPDFSignatureEmbedderTest, LoadedBytesPlainDocument) {
  RawPdf p = FormObjects();
  RawPdf n = AppendRawUpdate(p, 5, "<</T(total) /Parent 4 0 R /V(changed)>>", 7);
  ScopedFPDFDocument doc = OpenRaw(n);
  ASSERT_TRUE(doc);
  EXPECT_EQ(n.bytes.size(), EPDFDoc_GetLoadedBytesSize(doc.get()));
  EXPECT_EQ(n.bytes.size(), EPDFDoc_GetBaseBytesSize(doc.get()));
  std::string out(n.bytes.size(), '\0');
  EXPECT_EQ(n.bytes.size(),
            EPDFDoc_ReadLoadedBytes(doc.get(), 0, out.data(), out.size()));
  EXPECT_EQ(n.bytes, out);
  // A revision prefix reads exactly.
  std::string prefix(p.bytes.size(), '\0');
  EXPECT_EQ(p.bytes.size(),
            EPDFDoc_ReadLoadedBytes(doc.get(), 0, prefix.data(), prefix.size()));
  EXPECT_EQ(p.bytes, prefix);
  // Out of range, NULL, and empty reads copy nothing.
  EXPECT_EQ(0ul, EPDFDoc_ReadLoadedBytes(doc.get(), n.bytes.size() - 1, out.data(), 2));
  EXPECT_EQ(0ul, EPDFDoc_ReadLoadedBytes(doc.get(), 0, nullptr, 1));
  EXPECT_EQ(0ul, EPDFDoc_ReadLoadedBytes(doc.get(), 0, out.data(), 0));
  EXPECT_EQ(0u, EPDFDoc_GetLoadedBytesSize(nullptr));
  EXPECT_EQ(0u, EPDFDoc_GetBaseBytesSize(nullptr));
}

TEST_F(EPDFSignatureEmbedderTest, LoadedBytesLayerDocument) {
  RawPdf p = FormObjects();
  RawPdf n = AppendRawUpdate(p, 5, "<</T(total) /Parent 4 0 R /V(changed)>>", 7);
  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  const std::string delta = n.bytes.substr(p.bytes.size());
  ScopedFPDFDocument layer = OpenLayer(base.get(), delta);
  ScopedFPDFDocument fresh = OpenLayer(base.get());
  ASSERT_TRUE(layer);
  ASSERT_TRUE(fresh);
  // Base followed by the delta it was opened with; the base alone for a
  // fresh layer.
  EXPECT_EQ(n.bytes.size(), EPDFDoc_GetLoadedBytesSize(layer.get()));
  EXPECT_EQ(p.bytes.size(), EPDFDoc_GetBaseBytesSize(layer.get()));
  EXPECT_EQ(p.bytes.size(), EPDFDoc_GetLoadedBytesSize(fresh.get()));
  EXPECT_EQ(p.bytes.size(), EPDFDoc_GetBaseBytesSize(fresh.get()));
  std::string out(n.bytes.size(), '\0');
  EXPECT_EQ(n.bytes.size(),
            EPDFDoc_ReadLoadedBytes(layer.get(), 0, out.data(), out.size()));
  EXPECT_EQ(n.bytes, out);
  std::string tail(delta.size(), '\0');
  EXPECT_EQ(delta.size(),
            EPDFDoc_ReadLoadedBytes(layer.get(), p.bytes.size(), tail.data(), tail.size()));
  EXPECT_EQ(delta, tail);
  // An unsaved edit on the layer changes nothing about its loaded bytes.
  ASSERT_TRUE(EPDFSig_SetFieldLock(fresh.get(), 6, EPDF_SIG_FIELD_ACTION_ALL, nullptr, 0, 0));
  EXPECT_EQ(p.bytes.size(), EPDFDoc_GetLoadedBytesSize(fresh.get()));
}

TEST_F(EPDFSignatureEmbedderTest, BaseSha256IsLazyAndCanBeSupplied) {
  RawPdf p = FormObjects();
  // Lazy: the base hashes its own bytes on first use, and it is the real hash.
  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument layer = OpenLayer(base.get());
  ASSERT_TRUE(layer);
  unsigned char reported[32];
  ASSERT_TRUE(EPDFLayer_GetBaseSha256(layer.get(), reported));
  const DataVector<uint8_t> expected = CRYPT_SHA256Generate(
      pdfium::span(reinterpret_cast<const uint8_t*>(p.bytes.data()), p.bytes.size()));
  ASSERT_EQ(32u, expected.size());
  EXPECT_EQ(0, memcmp(reported, expected.data(), 32));
  EXPECT_FALSE(EPDFLayer_GetBaseSha256(layer.get(), nullptr));
  ScopedFPDFDocument plain = OpenRaw(p);
  EXPECT_FALSE(EPDFLayer_GetBaseSha256(plain.get(), reported));

  // Supplied: a host that already hashed the bytes hands the value over and
  // the runtime takes its word. Artifacts written on that base carry it.
  ScopedBase supplied(p.bytes);
  ASSERT_TRUE(supplied.get());
  unsigned char claimed[32];
  for (int i = 0; i < 32; ++i) claimed[i] = static_cast<unsigned char>(i);
  EPDF_SetBaseDocumentSha256(supplied.get(), claimed);
  ScopedFPDFDocument on_supplied = OpenLayer(supplied.get());
  ASSERT_TRUE(on_supplied);
  ASSERT_TRUE(EPDFLayer_GetBaseSha256(on_supplied.get(), reported));
  EXPECT_EQ(0, memcmp(reported, claimed, 32));
  ASSERT_TRUE(EPDFSig_SetFieldLock(on_supplied.get(), 6, EPDF_SIG_FIELD_ACTION_ALL, nullptr, 0, 0));
  unsigned long size = 0;
  EPDFLayerSaveStatus status = EPDFLayerSaveStatus_kSaveFailed;
  void* raw = EPDFLayer_SaveLayerArtifactToOwnedBuffer(on_supplied.get(), &size, &status);
  ASSERT_TRUE(raw);
  ASSERT_EQ(EPDFLayerSaveStatus_kSuccess, status);
  std::string artifact(static_cast<char*>(raw), size);
  EPDF_FreeBuffer(raw);

  // The trust boundary: the artifact opens on a base with the same claim and
  // is refused by a base that hashed the real bytes.
  FPDF_FILEACCESS access = {};
  access.m_FileLen = static_cast<unsigned long>(artifact.size());
  access.m_Param = &artifact;
  access.m_GetBlock = [](void* param, unsigned long pos, unsigned char* out,
                         unsigned long len) -> int {
    const std::string& b = *static_cast<std::string*>(param);
    if (pos > b.size() || len > b.size() - pos) return 0;
    memcpy(out, b.data() + pos, len);
    return 1;
  };
  EPDFLayerOpenStatus open_status = EPDFLayerOpenStatus_kOpenFailed;
  ScopedFPDFDocument reopened(
      EPDFLayer_OpenLayerArtifact(supplied.get(), &access, nullptr, &open_status));
  EXPECT_TRUE(reopened);
  EXPECT_EQ(EPDFLayerOpenStatus_kSuccess, open_status);
  ScopedFPDFDocument refused(
      EPDFLayer_OpenLayerArtifact(base.get(), &access, nullptr, &open_status));
  EXPECT_FALSE(refused);
  EXPECT_EQ(EPDFLayerOpenStatus_kBaseLayerMismatch, open_status);
}

TEST_F(EPDFSignatureEmbedderTest, StructureObjectNumbers) {
  RawPdf p = FormObjects();
  ScopedFPDFDocument doc = OpenRaw(p);
  ASSERT_TRUE(doc);
  unsigned int root = 0;
  unsigned int acroform = 0;
  unsigned int pages = 0;
  ASSERT_TRUE(EPDFDoc_GetStructureObjectNumbers(doc.get(), &root, &acroform, &pages));
  EXPECT_EQ(1u, root);
  EXPECT_EQ(0u, acroform);  // direct dictionary in the fixture
  EXPECT_EQ(2u, pages);
  // A revision prefix answers for itself; NULL out-params are fine.
  ScopedFPDFDocument prefix(EPDFDoc_OpenRevision(doc.get(), p.bytes.size()));
  ASSERT_TRUE(prefix);
  ASSERT_TRUE(EPDFDoc_GetStructureObjectNumbers(prefix.get(), &root, nullptr, nullptr));
  EXPECT_EQ(1u, root);
  EXPECT_FALSE(EPDFDoc_GetStructureObjectNumbers(nullptr, &root, &acroform, &pages));
  EXPECT_EQ(0u, root);
}

// ---------------------------------------------------------------------------
// Base overlays and the cached revision view.
// ---------------------------------------------------------------------------

namespace {

int FindDiffEntryFor(EPDF_OBJECT_DIFF diff, unsigned int wanted) {
  const int count = EPDFObjectDiff_GetCount(diff);
  for (int i = 0; i < count; ++i) {
    unsigned int num = 0;
    int change = 0;
    int kind = 0;
    int old_gen = 0;
    int new_gen = 0;
    FPDF_BOOL stream = false;
    if (EPDFObjectDiff_GetEntry(diff, i, &num, &change, &kind, &old_gen, &new_gen, &stream) &&
        num == wanted) {
      return i;
    }
  }
  return -1;
}

}  // namespace

// The working copy of a layer is the BASE plus the cumulative delta a save
// writes - never base + loaded delta + new delta, which would misplace every
// offset. An overlay opens as one document whose revisions are the base's
// plus exactly one, and that one replaces the loaded delta's revision.
TEST_F(EPDFSignatureEmbedderTest, LayerBaseOverlayReplacesTheLoadedDelta) {
  RawPdf p = TwoSignatureFields();
  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument first = OpenLayer(base.get());
  ASSERT_TRUE(first);
  ScopedFPDFWideString v1 = GetFPDFWideString(L"first");
  ASSERT_TRUE(EPDFForm_SetTextValue(first.get(), 6, v1.get(), nullptr, 0, nullptr));
  const std::string delta1 = SaveDelta(first.get());
  ASSERT_FALSE(delta1.empty());
  ScopedFPDFDocument reopened = OpenLayer(base.get(), delta1);
  ASSERT_TRUE(reopened);
  ASSERT_EQ(2, EPDFDoc_GetRevisionCount(reopened.get()));
  ScopedFPDFWideString v2 = GetFPDFWideString(L"second");
  ASSERT_TRUE(EPDFForm_SetTextValue(reopened.get(), 6, v2.get(), nullptr, 0, nullptr));
  const std::string delta2 = SaveDelta(reopened.get());
  ASSERT_FALSE(delta2.empty());

  ScopedFPDFDocument overlay(
      EPDFDoc_OpenBaseOverlay(reopened.get(), delta2.data(), delta2.size()));
  ASSERT_TRUE(overlay);
  EXPECT_EQ(2, EPDFDoc_GetRevisionCount(overlay.get()));
  unsigned long long end = 0;
  unsigned long long xref = 0;
  ASSERT_TRUE(EPDFDoc_GetRevision(overlay.get(), 1, &end, &xref));
  EXPECT_EQ(p.bytes.size() + delta2.size(), end);
  ASSERT_TRUE(EPDFDoc_GetRevision(overlay.get(), 0, &end, &xref));
  EXPECT_EQ(p.bytes.size(), end);

  // Base revision -> overlay: object 6 went from the base value to the
  // SECOND value, read from the overlay's own bytes.
  ScopedFPDFDocument older(EPDFDoc_OpenRevision(overlay.get(), end));
  ASSERT_TRUE(older);
  EPDF_OBJECT_DIFF diff = EPDFDoc_CompareRevisions(older.get(), overlay.get());
  ASSERT_TRUE(diff);
  const int index = FindDiffEntryFor(diff, 6);
  ASSERT_GE(index, 0);
  EXPECT_NE(std::string::npos, ReadDiffValue(diff, index, EPDF_DIFF_OLD).find("(before)"));
  EXPECT_NE(std::string::npos, ReadDiffValue(diff, index, EPDF_DIFF_NEW).find("(second)"));
  EXPECT_EQ(std::string::npos, ReadDiffValue(diff, index, EPDF_DIFF_NEW).find("(first)"));
  EPDFObjectDiff_Close(diff);

  // The layer itself is untouched: its loaded bytes still end with delta1.
  EXPECT_EQ(p.bytes.size() + delta1.size(),
            static_cast<size_t>(EPDFDoc_GetLoadedBytesSize(reopened.get())));

  // Refused for a plain document and for an empty delta.
  ScopedFPDFDocument plain = OpenBytes(p.bytes);
  ASSERT_TRUE(plain);
  EXPECT_FALSE(EPDFDoc_OpenBaseOverlay(plain.get(), delta2.data(), delta2.size()));
  EXPECT_FALSE(EPDFDoc_OpenBaseOverlay(reopened.get(), delta2.data(), 0));
}

// The revision view is created once per document and cached on it: repeated
// reads answer the same, prefixes opened from it compare (two clamps of one
// stream take the identity shortcut), and the view survives the model.
TEST_F(EPDFSignatureEmbedderTest, LayerRevisionReadsAreCachedAndStable) {
  RawPdf p = TwoSignatureFields();
  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument editing = OpenLayer(base.get());
  ASSERT_TRUE(editing);
  ScopedFPDFWideString v1 = GetFPDFWideString(L"edited");
  ASSERT_TRUE(EPDFForm_SetTextValue(editing.get(), 6, v1.get(), nullptr, 0, nullptr));
  const std::string delta = SaveDelta(editing.get());
  ScopedFPDFDocument layer = OpenLayer(base.get(), delta);
  ASSERT_TRUE(layer);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(2, EPDFDoc_GetRevisionCount(layer.get()));
  }
  unsigned long long end0 = 0;
  unsigned long long end1 = 0;
  unsigned long long xref = 0;
  ASSERT_TRUE(EPDFDoc_GetRevision(layer.get(), 0, &end0, &xref));
  ASSERT_TRUE(EPDFDoc_GetRevision(layer.get(), 1, &end1, &xref));
  ScopedFPDFDocument r0(EPDFDoc_OpenRevision(layer.get(), end0));
  ScopedFPDFDocument r1(EPDFDoc_OpenRevision(layer.get(), end1));
  ASSERT_TRUE(r0);
  ASSERT_TRUE(r1);
  EPDF_OBJECT_DIFF diff = EPDFDoc_CompareRevisions(r0.get(), r1.get());
  ASSERT_TRUE(diff);
  EXPECT_GE(FindDiffEntryFor(diff, 6), 0);
  EPDFObjectDiff_Close(diff);
  EPDF_SIGNATURE_MODEL model = EPDFSig_LoadModel(layer.get());
  ASSERT_TRUE(model);
  EXPECT_EQ(2, EPDFSig_Count(model));
  EPDFSig_CloseModel(model);
  EXPECT_EQ(2, EPDFDoc_GetRevisionCount(layer.get()));
}

// ---------------------------------------------------------------------------
// File-backed candidate saves: the writer variant, the span seal and the
// file digest agree byte for byte with the buffer variants.
// ---------------------------------------------------------------------------

TEST_F(EPDFSignatureEmbedderTest, FileCandidateSaveSealSpanAndFileDigestAgree) {
  RawPdf p = FormObjects();
  ScopedBase base(p.bytes);
  ASSERT_TRUE(base.get());
  ScopedFPDFDocument candidate = OpenLayer(base.get());
  ASSERT_TRUE(candidate);
  EPDF_SIG_PREPARE opts = DefaultPrepare();
  const uint32_t value = EPDFSig_Prepare(candidate.get(), 6, &opts);
  ASSERT_NE(0u, value);

  // The buffer save.
  unsigned long long size = 0;
  unsigned long long off = 0;
  unsigned long long len = 0;
  void* raw = EPDFSig_SaveCandidateToOwnedBuffer(candidate.get(), value, &size, &off, &len);
  ASSERT_TRUE(raw);
  std::string buffered(static_cast<char*>(raw), static_cast<size_t>(size));
  EPDF_FreeBuffer(raw);

  // The writer save (this fixture collects FPDF_FILEWRITE output): the same
  // size and object span. The bytes differ only in the fresh /ID each save
  // draws, so the seal comparisons below run on ONE save.
  ClearString();
  unsigned long long fsize = 0;
  unsigned long long foff = 0;
  unsigned long long flen = 0;
  ASSERT_TRUE(EPDFSig_SaveCandidate(candidate.get(), value, this, &fsize, &foff, &flen));
  const std::string filed = GetString();
  EXPECT_EQ(buffered.size(), static_cast<size_t>(fsize));
  EXPECT_EQ(buffered.size(), filed.size());
  EXPECT_EQ(off, foff);
  EXPECT_EQ(len, flen);
  EXPECT_NE(std::string::npos, filed.substr(static_cast<size_t>(foff), static_cast<size_t>(flen))
                                    .find("/ByteRange[ 0 2147483647"));

  // Whole-buffer seal versus span seal + file digest, on the buffered save.
  std::vector<unsigned char> whole(buffered.begin(), buffered.end());
  unsigned long long range[4] = {0, 0, 0, 0};
  unsigned long long co = 0;
  unsigned long long ch = 0;
  unsigned char digest[64];
  unsigned long dlen = sizeof(digest);
  ASSERT_TRUE(EPDFSig_Seal(whole.data(), whole.size(), off, len, EPDF_DIGEST_SHA256, range, &co,
                           &ch, digest, &dlen));

  std::string patched = buffered;
  std::vector<unsigned char> span(patched.begin() + static_cast<ptrdiff_t>(off),
                                  patched.begin() + static_cast<ptrdiff_t>(off + len));
  unsigned long long srange[4] = {0, 0, 0, 0};
  unsigned long long sco = 0;
  unsigned long long sch = 0;
  ASSERT_TRUE(EPDFSig_SealSpan(span.data(), span.size(), off, patched.size(), srange, &sco, &sch));
  for (int k = 0; k < 4; ++k) {
    EXPECT_EQ(range[k], srange[k]) << "range[" << k << "]";
  }
  EXPECT_EQ(co, sco);
  EXPECT_EQ(ch, sch);
  std::copy(span.begin(), span.end(), patched.begin() + static_cast<ptrdiff_t>(off));
  EXPECT_EQ(std::string(whole.begin(), whole.end()), patched);

  FPDF_FILEACCESS access = {};
  access.m_FileLen = static_cast<unsigned long>(patched.size());
  access.m_Param = &patched;
  access.m_GetBlock = [](void* param, unsigned long pos, unsigned char* out,
                         unsigned long size) -> int {
    const std::string& b = *static_cast<std::string*>(param);
    if (pos > b.size() || size > b.size() - pos) {
      return 0;
    }
    memcpy(out, b.data() + pos, size);
    return 1;
  };
  unsigned char fdigest[64];
  unsigned long fdlen = sizeof(fdigest);
  ASSERT_TRUE(EPDFSig_DigestFileRange(&access, srange, EPDF_DIGEST_SHA256, fdigest, &fdlen));
  EXPECT_EQ(dlen, fdlen);
  EXPECT_EQ(0, memcmp(digest, fdigest, dlen));

  // Sealing twice fails: the sentinel is gone.
  EXPECT_FALSE(EPDFSig_SealSpan(span.data(), span.size(), off, patched.size(), srange, &sco, &sch));
  // The digest refuses a range past the file.
  unsigned long long bad[4] = {0, 10, static_cast<unsigned long long>(patched.size()), 10};
  EXPECT_FALSE(EPDFSig_DigestFileRange(&access, bad, EPDF_DIGEST_SHA256, fdigest, &fdlen));
}
