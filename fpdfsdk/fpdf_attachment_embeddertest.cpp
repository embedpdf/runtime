// Copyright 2017 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <vector>

#include "public/fpdf_attachment.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/utils/hash.h"

static constexpr char kDateKey[] = "CreationDate";
static constexpr char kChecksumKey[] = "CheckSum";

class FPDFAttachmentEmbedderTest : public EmbedderTest {};

TEST_F(FPDFAttachmentEmbedderTest, ExtractAttachments) {
  // Open a file with two attachments.
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));
  EXPECT_EQ(2, FPDFDoc_GetAttachmentCount(document()));

  // Try to retrieve attachments at bad indices.
  EXPECT_FALSE(FPDFDoc_GetAttachment(document(), -1));
  EXPECT_FALSE(FPDFDoc_GetAttachment(document(), 2));

  // Retrieve the first attachment.
  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);

  // Check that the name of the first attachment is correct.
  unsigned long length_bytes = FPDFAttachment_GetName(attachment, nullptr, 0);
  ASSERT_EQ(12u, length_bytes);
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(12u, FPDFAttachment_GetName(attachment, buf.data(), length_bytes));
  EXPECT_EQ(L"1.txt", GetPlatformWString(buf.data()));

  // Check some unsuccessful cases of FPDFAttachment_GetFile.
  EXPECT_FALSE(FPDFAttachment_GetFile(attachment, nullptr, 0, nullptr));
  EXPECT_FALSE(FPDFAttachment_GetFile(nullptr, nullptr, 0, &length_bytes));

  // Check that the content of the first attachment is correct.
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, nullptr, 0, &length_bytes));
  std::vector<uint8_t> content_buf(length_bytes);
  unsigned long actual_length_bytes;
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, content_buf.data(),
                                     length_bytes, &actual_length_bytes));
  ASSERT_THAT(content_buf, testing::ElementsAre('t', 'e', 's', 't'));

  // Check that a non-existent key does not exist.
  EXPECT_FALSE(FPDFAttachment_HasKey(attachment, "none"));

  // Check that the string value of a non-string dictionary entry is empty.
  static constexpr char kSizeKey[] = "Size";
  EXPECT_EQ(FPDF_OBJECT_NUMBER,
            FPDFAttachment_GetValueType(attachment, kSizeKey));
  EXPECT_EQ(2u,
            FPDFAttachment_GetStringValue(attachment, kSizeKey, nullptr, 0));

  // Check that the creation date of the first attachment is correct.
  length_bytes =
      FPDFAttachment_GetStringValue(attachment, kDateKey, nullptr, 0);
  ASSERT_EQ(48u, length_bytes);
  buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(48u, FPDFAttachment_GetStringValue(attachment, kDateKey, buf.data(),
                                               length_bytes));
  EXPECT_EQ(L"D:20170712214438-07'00'", GetPlatformWString(buf.data()));

  // Retrieve the second attachment.
  attachment = FPDFDoc_GetAttachment(document(), 1);
  ASSERT_TRUE(attachment);

  // Retrieve the second attachment file.
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, nullptr, 0, &length_bytes));
  content_buf.clear();
  content_buf.resize(length_bytes);
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, content_buf.data(),
                                     length_bytes, &actual_length_bytes));
  ASSERT_EQ(5869u, actual_length_bytes);

  // Check that the calculated checksum of the file data matches expectation.
  const char kCheckSum[] = "72afcddedf554dda63c0c88e06f1ce18";
  const wchar_t kCheckSumW[] = L"<72AFCDDEDF554DDA63C0C88E06F1CE18>";
  const std::string generated_checksum = GenerateMD5Base16(content_buf);
  EXPECT_EQ(kCheckSum, generated_checksum);

  // Check that the stored checksum matches expectation.
  length_bytes =
      FPDFAttachment_GetStringValue(attachment, kChecksumKey, nullptr, 0);
  ASSERT_EQ(70u, length_bytes);
  buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(70u, FPDFAttachment_GetStringValue(attachment, kChecksumKey,
                                               buf.data(), length_bytes));
  EXPECT_EQ(kCheckSumW, GetPlatformWString(buf.data()));
}

TEST_F(FPDFAttachmentEmbedderTest, NoAttachmentToExtract) {
  // Open a file with no attachments.
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_EQ(0, FPDFDoc_GetAttachmentCount(document()));

  // Try to retrieve attachments at bad indices.
  EXPECT_FALSE(FPDFDoc_GetAttachment(document(), -1));
  EXPECT_FALSE(FPDFDoc_GetAttachment(document(), 0));
}

TEST_F(FPDFAttachmentEmbedderTest, InvalidAttachmentData) {
  // Open a file with an attachment that is missing the embedded file (/EF).
  ASSERT_TRUE(OpenDocument("embedded_attachments_invalid_data.pdf"));
  ASSERT_EQ(1, FPDFDoc_GetAttachmentCount(document()));

  // Retrieve the first attachment.
  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);

  // Check that the name of the attachment is correct.
  unsigned long length_bytes = FPDFAttachment_GetName(attachment, nullptr, 0);
  ASSERT_EQ(12u, length_bytes);
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(12u, FPDFAttachment_GetName(attachment, buf.data(), length_bytes));
  EXPECT_EQ("1.txt", GetPlatformString(buf.data()));

  // Check that is is not possible to retrieve the file data.
  EXPECT_FALSE(FPDFAttachment_GetFile(attachment, nullptr, 0, &length_bytes));

  // Check that the attachment can be deleted.
  EXPECT_TRUE(FPDFDoc_DeleteAttachment(document(), 0));
  EXPECT_EQ(0, FPDFDoc_GetAttachmentCount(document()));
}

TEST_F(FPDFAttachmentEmbedderTest, AddAttachments) {
  // Open a file with two attachments.
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));
  EXPECT_EQ(2, FPDFDoc_GetAttachmentCount(document()));

  // Check that adding an attachment with an empty name would fail.
  EXPECT_FALSE(FPDFDoc_AddAttachment(document(), nullptr));

  // Add an attachment to the beginning of the embedded file list.
  ScopedFPDFWideString file_name = GetFPDFWideString(L"0.txt");
  FPDF_ATTACHMENT attachment =
      FPDFDoc_AddAttachment(document(), file_name.get());
  ASSERT_TRUE(attachment);

  // Check that writing to a file with nullptr but non-zero bytes would fail.
  EXPECT_FALSE(FPDFAttachment_SetFile(attachment, document(), nullptr, 10));

  // Set the new attachment's file.
  static constexpr char kContents1[] = "Hello!";
  EXPECT_TRUE(FPDFAttachment_SetFile(attachment, document(), kContents1,
                                     strlen(kContents1)));
  EXPECT_EQ(3, FPDFDoc_GetAttachmentCount(document()));

  // Verify the name of the new attachment (i.e. the first attachment).
  attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);
  unsigned long length_bytes = FPDFAttachment_GetName(attachment, nullptr, 0);
  ASSERT_EQ(12u, length_bytes);
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(12u, FPDFAttachment_GetName(attachment, buf.data(), length_bytes));
  EXPECT_EQ(L"0.txt", GetPlatformWString(buf.data()));

  // Verify the content of the new attachment (i.e. the first attachment).
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, nullptr, 0, &length_bytes));
  std::vector<char> content_buf(length_bytes);
  unsigned long actual_length_bytes;
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, content_buf.data(),
                                     length_bytes, &actual_length_bytes));
  ASSERT_EQ(6u, actual_length_bytes);
  EXPECT_EQ(std::string(kContents1), std::string(content_buf.data(), 6));

  // Add an attachment to the end of the embedded file list and set its file.
  file_name = GetFPDFWideString(L"z.txt");
  attachment = FPDFDoc_AddAttachment(document(), file_name.get());
  ASSERT_TRUE(attachment);
  static constexpr char kContents2[] = "World!";
  EXPECT_TRUE(FPDFAttachment_SetFile(attachment, document(), kContents2,
                                     strlen(kContents2)));
  EXPECT_EQ(4, FPDFDoc_GetAttachmentCount(document()));

  // Verify the name of the new attachment (i.e. the fourth attachment).
  attachment = FPDFDoc_GetAttachment(document(), 3);
  ASSERT_TRUE(attachment);
  length_bytes = FPDFAttachment_GetName(attachment, nullptr, 0);
  ASSERT_EQ(12u, length_bytes);
  buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(12u, FPDFAttachment_GetName(attachment, buf.data(), length_bytes));
  EXPECT_EQ(L"z.txt", GetPlatformWString(buf.data()));

  // Verify the content of the new attachment (i.e. the fourth attachment).
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, nullptr, 0, &length_bytes));
  content_buf.clear();
  content_buf.resize(length_bytes);
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, content_buf.data(),
                                     length_bytes, &actual_length_bytes));
  ASSERT_EQ(6u, actual_length_bytes);
  EXPECT_EQ(std::string(kContents2), std::string(content_buf.data(), 6));
}

TEST_F(FPDFAttachmentEmbedderTest, AddAttachmentsWithParams) {
  // Open a file with two attachments.
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));
  EXPECT_EQ(2, FPDFDoc_GetAttachmentCount(document()));

  // Add an attachment to the embedded file list.
  ScopedFPDFWideString file_name = GetFPDFWideString(L"5.txt");
  FPDF_ATTACHMENT attachment =
      FPDFDoc_AddAttachment(document(), file_name.get());
  ASSERT_TRUE(attachment);
  static constexpr char kContents[] = "Hello World!";
  EXPECT_TRUE(FPDFAttachment_SetFile(attachment, document(), kContents,
                                     strlen(kContents)));

  // Set the date to be an arbitrary value.
  static constexpr wchar_t kDateW[] = L"D:20170720161527-04'00'";
  ScopedFPDFWideString ws_date = GetFPDFWideString(kDateW);
  EXPECT_TRUE(
      FPDFAttachment_SetStringValue(attachment, kDateKey, ws_date.get()));

  // Set the checksum to be an arbitrary value.
  static constexpr wchar_t kCheckSumW[] = L"<ABCDEF01234567899876543210FEDCBA>";
  ScopedFPDFWideString ws_checksum = GetFPDFWideString(kCheckSumW);
  EXPECT_TRUE(FPDFAttachment_SetStringValue(attachment, kChecksumKey,
                                            ws_checksum.get()));

  // Verify the name of the new attachment (i.e. the second attachment).
  attachment = FPDFDoc_GetAttachment(document(), 1);
  ASSERT_TRUE(attachment);
  unsigned long length_bytes = FPDFAttachment_GetName(attachment, nullptr, 0);
  ASSERT_EQ(12u, length_bytes);
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(12u, FPDFAttachment_GetName(attachment, buf.data(), length_bytes));
  EXPECT_EQ(L"5.txt", GetPlatformWString(buf.data()));

  // Verify the content of the new attachment.
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, nullptr, 0, &length_bytes));
  std::vector<char> content_buf(length_bytes);
  unsigned long actual_length_bytes;
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, content_buf.data(),
                                     length_bytes, &actual_length_bytes));
  ASSERT_EQ(12u, actual_length_bytes);
  EXPECT_EQ(std::string(kContents), std::string(content_buf.data(), 12));

  // Verify the creation date of the new attachment.
  length_bytes =
      FPDFAttachment_GetStringValue(attachment, kDateKey, nullptr, 0);
  ASSERT_EQ(48u, length_bytes);
  buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(48u, FPDFAttachment_GetStringValue(attachment, kDateKey, buf.data(),
                                               length_bytes));
  EXPECT_EQ(kDateW, GetPlatformWString(buf.data()));

  // Verify the checksum of the new attachment.
  length_bytes =
      FPDFAttachment_GetStringValue(attachment, kChecksumKey, nullptr, 0);
  ASSERT_EQ(70u, length_bytes);
  buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(70u, FPDFAttachment_GetStringValue(attachment, kChecksumKey,
                                               buf.data(), length_bytes));
  EXPECT_EQ(kCheckSumW, GetPlatformWString(buf.data()));

  // Overwrite the existing file with empty content, and check that the checksum
  // gets updated to the correct value.
  EXPECT_TRUE(FPDFAttachment_SetFile(attachment, document(), nullptr, 0));
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, nullptr, 0, &length_bytes));
  EXPECT_EQ(0u, length_bytes);
  length_bytes =
      FPDFAttachment_GetStringValue(attachment, kChecksumKey, nullptr, 0);
  ASSERT_EQ(70u, length_bytes);
  buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(70u, FPDFAttachment_GetStringValue(attachment, kChecksumKey,
                                               buf.data(), length_bytes));
  EXPECT_EQ(L"<D41D8CD98F00B204E9800998ECF8427E>",
            GetPlatformWString(buf.data()));
}

TEST_F(FPDFAttachmentEmbedderTest, AddAttachmentsToFileWithNoAttachments) {
  // Open a file with no attachments.
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  EXPECT_EQ(0, FPDFDoc_GetAttachmentCount(document()));

  // Add an attachment to the beginning of the embedded file list.
  ScopedFPDFWideString file_name = GetFPDFWideString(L"0.txt");
  FPDF_ATTACHMENT attachment =
      FPDFDoc_AddAttachment(document(), file_name.get());
  ASSERT_TRUE(attachment);

  // Set the new attachment's file.
  static constexpr char kContents1[] = "Hello!";
  EXPECT_TRUE(FPDFAttachment_SetFile(attachment, document(), kContents1,
                                     strlen(kContents1)));
  EXPECT_EQ(1, FPDFDoc_GetAttachmentCount(document()));

  // Verify the name of the new attachment (i.e. the first attachment).
  attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);
  unsigned long length_bytes = FPDFAttachment_GetName(attachment, nullptr, 0);
  ASSERT_EQ(12u, length_bytes);
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(12u, FPDFAttachment_GetName(attachment, buf.data(), length_bytes));
  EXPECT_EQ(L"0.txt", GetPlatformWString(buf.data()));

  // Verify the content of the new attachment (i.e. the first attachment).
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, nullptr, 0, &length_bytes));
  std::vector<char> content_buf(length_bytes);
  unsigned long actual_length_bytes;
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, content_buf.data(),
                                     length_bytes, &actual_length_bytes));
  ASSERT_EQ(6u, actual_length_bytes);
  EXPECT_EQ(std::string(kContents1), std::string(content_buf.data(), 6));

  // Add an attachment to the end of the embedded file list and set its file.
  file_name = GetFPDFWideString(L"z.txt");
  attachment = FPDFDoc_AddAttachment(document(), file_name.get());
  ASSERT_TRUE(attachment);
  static constexpr char kContents2[] = "World!";
  EXPECT_TRUE(FPDFAttachment_SetFile(attachment, document(), kContents2,
                                     strlen(kContents2)));
  EXPECT_EQ(2, FPDFDoc_GetAttachmentCount(document()));

  // Verify the name of the new attachment (i.e. the second attachment).
  attachment = FPDFDoc_GetAttachment(document(), 1);
  ASSERT_TRUE(attachment);
  length_bytes = FPDFAttachment_GetName(attachment, nullptr, 0);
  ASSERT_EQ(12u, length_bytes);
  buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(12u, FPDFAttachment_GetName(attachment, buf.data(), length_bytes));
  EXPECT_EQ(L"z.txt", GetPlatformWString(buf.data()));

  // Verify the content of the new attachment (i.e. the second attachment).
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, nullptr, 0, &length_bytes));
  content_buf.clear();
  content_buf.resize(length_bytes);
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, content_buf.data(),
                                     length_bytes, &actual_length_bytes));
  ASSERT_EQ(6u, actual_length_bytes);
  EXPECT_EQ(std::string(kContents2), std::string(content_buf.data(), 6));
}

TEST_F(FPDFAttachmentEmbedderTest, DeleteAttachment) {
  // Open a file with two attachments.
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));
  EXPECT_EQ(2, FPDFDoc_GetAttachmentCount(document()));

  // Verify the name of the first attachment.
  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);
  unsigned long length_bytes = FPDFAttachment_GetName(attachment, nullptr, 0);
  ASSERT_EQ(12u, length_bytes);
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(12u, FPDFAttachment_GetName(attachment, buf.data(), length_bytes));
  EXPECT_EQ(L"1.txt", GetPlatformWString(buf.data()));

  // Delete the first attachment.
  EXPECT_TRUE(FPDFDoc_DeleteAttachment(document(), 0));
  EXPECT_EQ(1, FPDFDoc_GetAttachmentCount(document()));

  // Verify the name of the new first attachment.
  attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);
  length_bytes = FPDFAttachment_GetName(attachment, nullptr, 0);
  ASSERT_EQ(26u, length_bytes);
  buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(26u, FPDFAttachment_GetName(attachment, buf.data(), length_bytes));
  EXPECT_EQ(L"attached.pdf", GetPlatformWString(buf.data()));
}

TEST_F(FPDFAttachmentEmbedderTest, GetStringValueForChecksumNotString) {
  ASSERT_TRUE(OpenDocument("embedded_attachments_invalid_types.pdf"));
  EXPECT_EQ(2, FPDFDoc_GetAttachmentCount(document()));

  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);

  // The checksum key is a name, which violates the spec. This will still return
  // the value, but should not crash.
  static constexpr unsigned long kExpectedLength = 8u;
  ASSERT_EQ(kExpectedLength, FPDFAttachment_GetStringValue(
                                 attachment, kChecksumKey, nullptr, 0));
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(kExpectedLength);
  EXPECT_EQ(kExpectedLength,
            FPDFAttachment_GetStringValue(attachment, kChecksumKey, buf.data(),
                                          kExpectedLength));
  EXPECT_EQ(L"Bad", GetPlatformWString(buf.data()));
}

TEST_F(FPDFAttachmentEmbedderTest, GetStringValueForNotString) {
  ASSERT_TRUE(OpenDocument("embedded_attachments_invalid_types.pdf"));
  EXPECT_EQ(2, FPDFDoc_GetAttachmentCount(document()));

  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 1);
  ASSERT_TRUE(attachment);

  // The checksum key is a stream, while the API requires a string or name.
  static constexpr unsigned long kExpectedLength = 2u;
  ASSERT_EQ(kExpectedLength, FPDFAttachment_GetStringValue(
                                 attachment, kChecksumKey, nullptr, 0));
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(kExpectedLength);
  EXPECT_EQ(kExpectedLength,
            FPDFAttachment_GetStringValue(attachment, kChecksumKey, buf.data(),
                                          kExpectedLength));
  EXPECT_EQ(L"", GetPlatformWString(buf.data()));
}

TEST_F(FPDFAttachmentEmbedderTest, GetSubtype) {
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));
  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);

  // Test getting Subtype (MIME type)
  constexpr char kExpectedSubtype[] = "text/plain";
  unsigned long length = FPDFAttachment_GetSubtype(attachment, nullptr, 0);
  ASSERT_EQ(2u * (strlen(kExpectedSubtype) + 1), length);

  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length, FPDFAttachment_GetSubtype(attachment, buf.data(), length));
  EXPECT_EQ(kExpectedSubtype, GetPlatformString(buf.data()));

  // Test with buffer too small
  std::vector<FPDF_WCHAR> small_buf(length - 1);
  const FPDF_WCHAR kPattern = 0xDEAD;
  std::ranges::fill(small_buf, kPattern);
  EXPECT_EQ(length, FPDFAttachment_GetSubtype(attachment, small_buf.data(),
                                              length - 1));
  EXPECT_THAT(small_buf, testing::Each(kPattern));
}

TEST_F(FPDFAttachmentEmbedderTest, GetSubtypeInvalid) {
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));
  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);

  std::vector<FPDF_WCHAR> buf(1);
  EXPECT_EQ(0u, FPDFAttachment_GetSubtype(nullptr, buf.data(), 1));

  constexpr char kExpectedSubtype[] = "text/plain";
  EXPECT_EQ(2u * (strlen(kExpectedSubtype) + 1),
            FPDFAttachment_GetSubtype(attachment, nullptr, 10));
}

namespace {

class CollectingFileWriter final : public FPDF_FILEWRITE {
 public:
  CollectingFileWriter() {
    version = 1;
    WriteBlock = WriteBlockImpl;
  }

  const std::string& data() const { return data_; }
  int write_calls() const { return write_calls_; }

 private:
  static int WriteBlockImpl(FPDF_FILEWRITE* self,
                            const void* data,
                            unsigned long size) {
    auto* writer = static_cast<CollectingFileWriter*>(self);
    ++writer->write_calls_;
    writer->data_.append(static_cast<const char*>(data), size);
    return 1;
  }

  std::string data_;
  int write_calls_ = 0;
};

class FailingFileWriter final : public FPDF_FILEWRITE {
 public:
  FailingFileWriter() {
    version = 1;
    WriteBlock = WriteBlockImpl;
  }

 private:
  static int WriteBlockImpl(FPDF_FILEWRITE*, const void*, unsigned long) {
    return 0;
  }
};

std::string GetFileViaStockApi(FPDF_ATTACHMENT attachment) {
  unsigned long length = 0;
  if (!FPDFAttachment_GetFile(attachment, nullptr, 0, &length)) {
    ADD_FAILURE() << "stock FPDFAttachment_GetFile failed";
    return std::string();
  }
  std::vector<char> buf(length);
  unsigned long actual = 0;
  EXPECT_TRUE(FPDFAttachment_GetFile(attachment, buf.data(), length, &actual));
  return std::string(buf.data(), actual);
}

}  // namespace

TEST_F(FPDFAttachmentEmbedderTest, ExtractFileMatchesGetFile) {
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));
  ASSERT_EQ(2, FPDFDoc_GetAttachmentCount(document()));

  for (int i = 0; i < 2; ++i) {
    FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), i);
    ASSERT_TRUE(attachment);
    const std::string expected = GetFileViaStockApi(attachment);
    ASSERT_FALSE(expected.empty());

    // FPDF_FILEWRITE variant produces byte-identical output.
    CollectingFileWriter writer;
    uint32_t size = 0;
    EPDFAttachmentExtractStatus status =
        EPDFAttachmentExtractStatus_kWriteFailed;
    ASSERT_TRUE(EPDFAttachment_ExtractFile(attachment, &writer,
                                           /*max_decoded_bytes=*/0, &size,
                                           &status))
        << " for attachment " << i;
    EXPECT_EQ(EPDFAttachmentExtractStatus_kSuccess, status);
    EXPECT_EQ(expected.size(), static_cast<size_t>(size));
    EXPECT_EQ(expected, writer.data());

    // Owned-buffer variant too.
    void* buffer = nullptr;
    uint32_t buffer_size = 0;
    ASSERT_TRUE(EPDFAttachment_ExtractFileToOwnedBuffer(
        attachment, /*max_decoded_bytes=*/0, &buffer, &buffer_size, &status));
    EXPECT_EQ(EPDFAttachmentExtractStatus_kSuccess, status);
    ASSERT_EQ(expected.size(), static_cast<size_t>(buffer_size));
    ASSERT_TRUE(buffer);
    EXPECT_EQ(expected, std::string(static_cast<const char*>(buffer),
                                    buffer_size));
    EPDF_FreeBuffer(buffer);
  }
}

TEST_F(FPDFAttachmentEmbedderTest, ExtractFileSizeLimit) {
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));

  // The second attachment is 5869 bytes.
  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 1);
  ASSERT_TRUE(attachment);

  CollectingFileWriter writer;
  uint32_t size = 0;
  EPDFAttachmentExtractStatus status = EPDFAttachmentExtractStatus_kSuccess;
  EXPECT_FALSE(EPDFAttachment_ExtractFile(attachment, &writer,
                                          /*max_decoded_bytes=*/100, &size,
                                          &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kSizeLimitExceeded, status);
  EXPECT_EQ(0u, size);

  void* buffer = nullptr;
  uint32_t buffer_size = 0;
  EXPECT_FALSE(EPDFAttachment_ExtractFileToOwnedBuffer(
      attachment, /*max_decoded_bytes=*/100, &buffer, &buffer_size, &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kSizeLimitExceeded, status);
  EXPECT_FALSE(buffer);
  EXPECT_EQ(0u, buffer_size);

  // A limit exactly equal to the file size succeeds.
  CollectingFileWriter exact_writer;
  EXPECT_TRUE(EPDFAttachment_ExtractFile(attachment, &exact_writer,
                                         /*max_decoded_bytes=*/5869, &size,
                                         &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kSuccess, status);
  EXPECT_EQ(5869u, size);
}

TEST_F(FPDFAttachmentEmbedderTest, ExtractFileNoFileStream) {
  // This fixture's attachment is missing the embedded file (/EF).
  ASSERT_TRUE(OpenDocument("embedded_attachments_invalid_data.pdf"));
  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);

  CollectingFileWriter writer;
  uint32_t size = 0;
  EPDFAttachmentExtractStatus status = EPDFAttachmentExtractStatus_kSuccess;
  EXPECT_FALSE(EPDFAttachment_ExtractFile(attachment, &writer,
                                          /*max_decoded_bytes=*/0, &size,
                                          &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kNoFileStream, status);
  EXPECT_EQ(0, writer.write_calls());

  void* buffer = nullptr;
  uint32_t buffer_size = 0;
  EXPECT_FALSE(EPDFAttachment_ExtractFileToOwnedBuffer(
      attachment, /*max_decoded_bytes=*/0, &buffer, &buffer_size, &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kNoFileStream, status);
  EXPECT_FALSE(buffer);

  // A null attachment behaves the same.
  EXPECT_FALSE(EPDFAttachment_ExtractFile(nullptr, &writer,
                                          /*max_decoded_bytes=*/0, &size,
                                          &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kNoFileStream, status);
}

TEST_F(FPDFAttachmentEmbedderTest, ExtractFileInvalidWriter) {
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));
  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);

  uint32_t size = 1;
  EPDFAttachmentExtractStatus status = EPDFAttachmentExtractStatus_kSuccess;
  EXPECT_FALSE(EPDFAttachment_ExtractFile(attachment, nullptr,
                                          /*max_decoded_bytes=*/0, &size,
                                          &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kWriteFailed, status);
  EXPECT_EQ(0u, size);

  CollectingFileWriter bad_version;
  bad_version.version = 2;
  EXPECT_FALSE(EPDFAttachment_ExtractFile(attachment, &bad_version,
                                          /*max_decoded_bytes=*/0, &size,
                                          &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kWriteFailed, status);

  CollectingFileWriter no_callback;
  no_callback.WriteBlock = nullptr;
  EXPECT_FALSE(EPDFAttachment_ExtractFile(attachment, &no_callback,
                                          /*max_decoded_bytes=*/0, &size,
                                          &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kWriteFailed, status);
}

TEST_F(FPDFAttachmentEmbedderTest, ExtractFileWriterFailure) {
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));
  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);

  FailingFileWriter writer;
  uint32_t size = 1;
  EPDFAttachmentExtractStatus status = EPDFAttachmentExtractStatus_kSuccess;
  EXPECT_FALSE(EPDFAttachment_ExtractFile(attachment, &writer,
                                          /*max_decoded_bytes=*/0, &size,
                                          &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kWriteFailed, status);
  EXPECT_EQ(0u, size);
}

TEST_F(FPDFAttachmentEmbedderTest, ExtractFileEmptyAttachment) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedFPDFWideString file_name = GetFPDFWideString(L"empty.bin");
  FPDF_ATTACHMENT attachment =
      FPDFDoc_AddAttachment(document(), file_name.get());
  ASSERT_TRUE(attachment);
  ASSERT_TRUE(FPDFAttachment_SetFile(attachment, document(), nullptr, 0));

  // A zero-byte embedded file extracts successfully without any writes.
  CollectingFileWriter writer;
  uint32_t size = 1;
  EPDFAttachmentExtractStatus status = EPDFAttachmentExtractStatus_kWriteFailed;
  EXPECT_TRUE(EPDFAttachment_ExtractFile(attachment, &writer,
                                         /*max_decoded_bytes=*/0, &size,
                                         &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kSuccess, status);
  EXPECT_EQ(0u, size);
  EXPECT_EQ(0, writer.write_calls());

  // The owned-buffer variant reports success with a null buffer.
  void* buffer = reinterpret_cast<void*>(1);
  uint32_t buffer_size = 1;
  EXPECT_TRUE(EPDFAttachment_ExtractFileToOwnedBuffer(
      attachment, /*max_decoded_bytes=*/0, &buffer, &buffer_size, &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kSuccess, status);
  EXPECT_FALSE(buffer);
  EXPECT_EQ(0u, buffer_size);
}

TEST_F(FPDFAttachmentEmbedderTest, ExtractFileLargeAttachment) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedFPDFWideString file_name = GetFPDFWideString(L"big.bin");
  FPDF_ATTACHMENT attachment =
      FPDFDoc_AddAttachment(document(), file_name.get());
  ASSERT_TRUE(attachment);

  std::string contents(2 * 1024 * 1024 + 17, '\0');
  for (size_t i = 0; i < contents.size(); ++i) {
    contents[i] = static_cast<char>((i * 31 + i / 997) & 0xff);
  }
  ASSERT_TRUE(FPDFAttachment_SetFile(attachment, document(), contents.data(),
                                     contents.size()));

  CollectingFileWriter writer;
  uint32_t size = 0;
  EPDFAttachmentExtractStatus status = EPDFAttachmentExtractStatus_kWriteFailed;
  ASSERT_TRUE(EPDFAttachment_ExtractFile(attachment, &writer,
                                         /*max_decoded_bytes=*/0, &size,
                                         &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kSuccess, status);
  ASSERT_EQ(contents.size(), static_cast<size_t>(size));
  EXPECT_EQ(contents, writer.data());
}

TEST_F(FPDFAttachmentEmbedderTest, ExtractFileToOwnedBufferBadArgs) {
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));
  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);

  void* buffer = nullptr;
  uint32_t size = 0;
  EPDFAttachmentExtractStatus status = EPDFAttachmentExtractStatus_kSuccess;
  EXPECT_FALSE(EPDFAttachment_ExtractFileToOwnedBuffer(
      attachment, /*max_decoded_bytes=*/0, nullptr, &size, &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kWriteFailed, status);
  EXPECT_FALSE(EPDFAttachment_ExtractFileToOwnedBuffer(
      attachment, /*max_decoded_bytes=*/0, &buffer, nullptr, &status));
  EXPECT_EQ(EPDFAttachmentExtractStatus_kWriteFailed, status);
  EXPECT_FALSE(buffer);
}

TEST_F(FPDFAttachmentEmbedderTest, GetAttachmentKey) {
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));
  ASSERT_EQ(2, FPDFDoc_GetAttachmentCount(document()));

  // This fixture's tree keys equal the /UF names (as do all
  // FPDFDoc_AddAttachment-created entries).
  unsigned long length_bytes =
      EPDFDoc_GetAttachmentKey(document(), 0, nullptr, 0);
  ASSERT_EQ(12u, length_bytes);
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(12u,
            EPDFDoc_GetAttachmentKey(document(), 0, buf.data(), length_bytes));
  EXPECT_EQ(L"1.txt", GetPlatformWString(buf.data()));

  length_bytes = EPDFDoc_GetAttachmentKey(document(), 1, nullptr, 0);
  ASSERT_EQ(26u, length_bytes);
  buf = GetFPDFWideStringBuffer(length_bytes);
  EXPECT_EQ(26u,
            EPDFDoc_GetAttachmentKey(document(), 1, buf.data(), length_bytes));
  EXPECT_EQ(L"attached.pdf", GetPlatformWString(buf.data()));

  // Bad indices / bad document.
  EXPECT_EQ(0u, EPDFDoc_GetAttachmentKey(document(), -1, nullptr, 0));
  EXPECT_EQ(0u, EPDFDoc_GetAttachmentKey(document(), 2, nullptr, 0));
  EXPECT_EQ(0u, EPDFDoc_GetAttachmentKey(nullptr, 0, nullptr, 0));
}

TEST_F(FPDFAttachmentEmbedderTest, GetAttachmentIndexByKey) {
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));

  ScopedFPDFWideString key1 = GetFPDFWideString(L"1.txt");
  ScopedFPDFWideString key2 = GetFPDFWideString(L"attached.pdf");
  ScopedFPDFWideString missing = GetFPDFWideString(L"nope.bin");
  EXPECT_EQ(0, EPDFDoc_GetAttachmentIndexByKey(document(), key1.get()));
  EXPECT_EQ(1, EPDFDoc_GetAttachmentIndexByKey(document(), key2.get()));
  EXPECT_EQ(-1, EPDFDoc_GetAttachmentIndexByKey(document(), missing.get()));
  EXPECT_EQ(-1, EPDFDoc_GetAttachmentIndexByKey(nullptr, key1.get()));
  EXPECT_EQ(-1, EPDFDoc_GetAttachmentIndexByKey(document(), nullptr));

  // The name tree is sorted, so adding "0.txt" shifts every index. Keys
  // keep resolving to the CURRENT position.
  ScopedFPDFWideString key0 = GetFPDFWideString(L"0.txt");
  FPDF_ATTACHMENT attachment =
      FPDFDoc_AddAttachment(document(), key0.get());
  ASSERT_TRUE(attachment);
  EXPECT_EQ(0, EPDFDoc_GetAttachmentIndexByKey(document(), key0.get()));
  EXPECT_EQ(1, EPDFDoc_GetAttachmentIndexByKey(document(), key1.get()));
  EXPECT_EQ(2, EPDFDoc_GetAttachmentIndexByKey(document(), key2.get()));

  // Deleting shifts them back; the deleted key stops resolving.
  EXPECT_TRUE(FPDFDoc_DeleteAttachment(document(), 0));
  EXPECT_EQ(-1, EPDFDoc_GetAttachmentIndexByKey(document(), key0.get()));
  EXPECT_EQ(0, EPDFDoc_GetAttachmentIndexByKey(document(), key1.get()));
  EXPECT_EQ(1, EPDFDoc_GetAttachmentIndexByKey(document(), key2.get()));
}

TEST_F(FPDFAttachmentEmbedderTest, SetNameKeepsTheRestOfTheFileSpec) {
  ASSERT_TRUE(OpenDocument("embedded_attachments.pdf"));
  FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(document(), 0);
  ASSERT_TRUE(attachment);

  ScopedFPDFWideString desc = GetFPDFWideString(L"Kept description");
  ASSERT_TRUE(EPDFAttachment_SetDescription(attachment, desc.get()));

  ScopedFPDFWideString empty = GetFPDFWideString(L"");
  EXPECT_FALSE(EPDFAttachment_SetName(attachment, empty.get()));
  EXPECT_FALSE(EPDFAttachment_SetName(nullptr, empty.get()));

  ScopedFPDFWideString name = GetFPDFWideString(L"r\u00e9sum\u00e9.txt");
  ASSERT_TRUE(EPDFAttachment_SetName(attachment, name.get()));

  unsigned long length_bytes = FPDFAttachment_GetName(attachment, nullptr, 0);
  std::vector<FPDF_WCHAR> buf = GetFPDFWideStringBuffer(length_bytes);
  ASSERT_EQ(length_bytes,
            FPDFAttachment_GetName(attachment, buf.data(), length_bytes));
  EXPECT_EQ(L"r\u00e9sum\u00e9.txt", GetPlatformWString(buf.data()));

  // The file's bytes and description are untouched.
  unsigned long out_len = 0;
  ASSERT_TRUE(FPDFAttachment_GetFile(attachment, nullptr, 0, &out_len));
  EXPECT_EQ(4u, out_len);
  length_bytes = EPDFAttachment_GetDescription(attachment, nullptr, 0);
  buf = GetFPDFWideStringBuffer(length_bytes);
  ASSERT_EQ(length_bytes,
            EPDFAttachment_GetDescription(attachment, buf.data(), length_bytes));
  EXPECT_EQ(L"Kept description", GetPlatformWString(buf.data()));

  // The name survives a save and reload.
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ASSERT_TRUE(OpenSavedDocument());
  FPDF_ATTACHMENT saved = FPDFDoc_GetAttachment(saved_document(), 0);
  ASSERT_TRUE(saved);
  length_bytes = FPDFAttachment_GetName(saved, nullptr, 0);
  buf = GetFPDFWideStringBuffer(length_bytes);
  ASSERT_EQ(length_bytes, FPDFAttachment_GetName(saved, buf.data(), length_bytes));
  EXPECT_EQ(L"r\u00e9sum\u00e9.txt", GetPlatformWString(buf.data()));
  CloseSavedDocument();
}
