// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Attachments (public/epdf_attachment.h).

#include "public/epdf_attachment.h"

#include <cstdint>
#include <string>
#include <vector>

#include "public/epdf_save.h"
#include "public/fpdf_save.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"

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

class EPDFAttachmentEmbedderTest : public EmbedderTest {};

// EmbedPDF: the buffer contract FPDFAttachment_GetFile() documents. A buffer
// large enough is filled; a smaller one is left alone and only the size is
// reported. (The size check was inverted: a larger buffer came back unfilled,
// and a smaller one failed a CHECK.)
TEST_F(EPDFAttachmentEmbedderTest, GetFileHonoursTheBufferContract) {
  ScopedFPDFDocument doc(FPDF_CreateNewDocument());
  ScopedFPDFWideString name = GetFPDFWideString(L"a.txt");
  FPDF_ATTACHMENT file = FPDFDoc_AddAttachment(doc.get(), name.get());
  ASSERT_TRUE(file);
  ASSERT_TRUE(FPDFAttachment_SetFile(file, doc.get(), "hello", 5));

  unsigned long size = 0;
  char exact[5] = {};
  ASSERT_TRUE(FPDFAttachment_GetFile(file, exact, sizeof(exact), &size));
  EXPECT_EQ(5u, size);
  EXPECT_EQ("hello", std::string(exact, size));

  char larger[16] = {};
  ASSERT_TRUE(FPDFAttachment_GetFile(file, larger, sizeof(larger), &size));
  EXPECT_EQ(5u, size);
  EXPECT_EQ("hello", std::string(larger, size));

  char smaller[3] = {'x', 'x', 'x'};
  ASSERT_TRUE(FPDFAttachment_GetFile(file, smaller, sizeof(smaller), &size));
  EXPECT_EQ(5u, size);
  EXPECT_EQ("xxx", std::string(smaller, sizeof(smaller)));  // untouched
}

TEST_F(EPDFAttachmentEmbedderTest, ExtractFileMatchesGetFile) {
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

TEST_F(EPDFAttachmentEmbedderTest, ExtractFileSizeLimit) {
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

TEST_F(EPDFAttachmentEmbedderTest, ExtractFileNoFileStream) {
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

TEST_F(EPDFAttachmentEmbedderTest, ExtractFileInvalidWriter) {
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

TEST_F(EPDFAttachmentEmbedderTest, ExtractFileWriterFailure) {
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

TEST_F(EPDFAttachmentEmbedderTest, ExtractFileEmptyAttachment) {
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

TEST_F(EPDFAttachmentEmbedderTest, ExtractFileLargeAttachment) {
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

TEST_F(EPDFAttachmentEmbedderTest, ExtractFileToOwnedBufferBadArgs) {
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

TEST_F(EPDFAttachmentEmbedderTest, GetAttachmentKey) {
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

TEST_F(EPDFAttachmentEmbedderTest, GetAttachmentIndexByKey) {
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

TEST_F(EPDFAttachmentEmbedderTest, SetNameKeepsTheRestOfTheFileSpec) {
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
  ScopedSavedDoc saved_doc = OpenScopedSavedDocument();
  ASSERT_TRUE(saved_doc);
  FPDF_ATTACHMENT saved = FPDFDoc_GetAttachment(saved_document(), 0);
  ASSERT_TRUE(saved);
  length_bytes = FPDFAttachment_GetName(saved, nullptr, 0);
  buf = GetFPDFWideStringBuffer(length_bytes);
  ASSERT_EQ(length_bytes, FPDFAttachment_GetName(saved, buf.data(), length_bytes));
  EXPECT_EQ(L"r\u00e9sum\u00e9.txt", GetPlatformWString(buf.data()));
}
