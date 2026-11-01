// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fxge/cfx_sfnt.h"

#include <optional>
#include <vector>

#include "core/fxcrt/byteorder.h"

namespace {

// The versions a TrueType or an OpenType font starts with: 1.0, Apple's
// 'true', and 'OTTO' for CFF outlines.
constexpr uint32_t kTrueTypeVersion = 0x00010000;
constexpr uint32_t kAppleTrueTypeVersion = 0x74727565;  // 'true'
constexpr uint32_t kOpenTypeCffVersion = 0x4f54544f;    // 'OTTO'

// What head's checkSumAdjustment makes the whole font's checksum.
constexpr uint32_t kFontChecksum = 0xb1b0afba;
// Where head holds checkSumAdjustment.
constexpr size_t kChecksumAdjustmentOffset = 8;

constexpr size_t kHeaderSize = 12;
constexpr size_t kTableRecordSize = 16;

struct TableRecord {
  ByteString tag;
  size_t offset = 0;
  size_t length = 0;
};

struct Directory {
  uint32_t version = 0;
  std::vector<TableRecord> tables;
};

uint32_t U32At(pdfium::span<const uint8_t> bytes, size_t offset) {
  return fxcrt::GetUInt32MSBFirst(bytes.subspan(offset).first<4u>());
}

// |bytes|' table directory; nullopt when it isn't an sfnt or names a table
// that doesn't lie inside it.
std::optional<Directory> ReadDirectory(pdfium::span<const uint8_t> bytes) {
  if (bytes.size() < kHeaderSize) {
    return std::nullopt;
  }
  Directory directory;
  directory.version = U32At(bytes, 0);
  if (directory.version != kTrueTypeVersion &&
      directory.version != kAppleTrueTypeVersion &&
      directory.version != kOpenTypeCffVersion) {
    return std::nullopt;
  }
  const size_t count =
      fxcrt::GetUInt16MSBFirst(bytes.subspan<4u>().first<2u>());
  if (count == 0 || bytes.size() < kHeaderSize + count * kTableRecordSize) {
    return std::nullopt;
  }
  for (size_t i = 0; i < count; ++i) {
    const size_t record = kHeaderSize + i * kTableRecordSize;
    TableRecord table;
    table.tag = ByteString(ByteStringView(bytes.subspan(record, 4u)));
    table.offset = U32At(bytes, record + 8);
    table.length = U32At(bytes, record + 12);
    if (table.offset > bytes.size() ||
        table.length > bytes.size() - table.offset) {
      return std::nullopt;
    }
    directory.tables.push_back(std::move(table));
  }
  return directory;
}

// Each writes the low bytes of |value|, big-endian: the caller has checked it
// fits.
void PutU16(DataVector<uint8_t>* out, uint64_t value) {
  out->push_back(static_cast<uint8_t>(value >> 8));
  out->push_back(static_cast<uint8_t>(value));
}

void PutU32(DataVector<uint8_t>* out, uint64_t value) {
  PutU16(out, value >> 16);
  PutU16(out, value);
}

uint32_t Checksum(pdfium::span<const uint8_t> bytes) {
  uint32_t sum = 0;
  for (size_t i = 0; i < bytes.size(); i += 4) {
    uint32_t word = 0;
    for (size_t j = 0; j < 4; ++j) {
      word = (word << 8) | (i + j < bytes.size() ? bytes[i + j] : 0);
    }
    sum += word;
  }
  return sum;
}

}  // namespace

DataVector<uint8_t> WriteSfnt(
    uint32_t version,
    const std::map<ByteString, DataVector<uint8_t>>& tables) {
  const size_t count = tables.size();
  size_t entry_selector = 0;
  while ((size_t{2} << entry_selector) <= count) {
    ++entry_selector;
  }
  const size_t search_range = (size_t{1} << entry_selector) * 16;
  DataVector<uint8_t> font;
  PutU32(&font, version);
  PutU16(&font, count);
  PutU16(&font, search_range);
  PutU16(&font, entry_selector);
  PutU16(&font, count * 16 - search_range);

  // head's checksum, and the font's, are taken with checkSumAdjustment 0.
  std::map<ByteString, DataVector<uint8_t>> written = tables;
  auto head = written.find("head");
  const bool adjusts = head != written.end() &&
                       head->second.size() >= kChecksumAdjustmentOffset + 4;
  if (adjusts) {
    fxcrt::PutUInt32MSBFirst(0, pdfium::span<uint8_t>(head->second)
                                    .subspan(kChecksumAdjustmentOffset)
                                    .first<4u>());
  }
  size_t offset = kHeaderSize + count * kTableRecordSize;
  size_t head_offset = 0;
  for (const auto& [tag, table] : written) {
    if (tag == "head") {
      head_offset = offset;
    }
    for (char c : tag) {
      font.push_back(static_cast<uint8_t>(c));
    }
    PutU32(&font, Checksum(table));
    PutU32(&font, offset);
    PutU32(&font, table.size());
    offset += (table.size() + 3) / 4 * 4;
  }
  for (const auto& [tag, table] : written) {
    font.insert(font.end(), table.begin(), table.end());
    font.insert(font.end(), (4 - table.size() % 4) % 4, 0);
  }
  if (adjusts) {
    fxcrt::PutUInt32MSBFirst(
        kFontChecksum - Checksum(font),
        pdfium::span<uint8_t>(font)
            .subspan(head_offset + kChecksumAdjustmentOffset)
            .first<4u>());
  }
  return font;
}

bool SfntTablesAligned(pdfium::span<const uint8_t> bytes) {
  std::optional<Directory> directory = ReadDirectory(bytes);
  if (!directory.has_value()) {
    return false;
  }
  for (const TableRecord& table : directory->tables) {
    if (table.offset % 4 != 0) {
      return false;
    }
  }
  return true;
}

DataVector<uint8_t> AlignSfntTables(pdfium::span<const uint8_t> bytes) {
  std::optional<Directory> directory = ReadDirectory(bytes);
  if (!directory.has_value()) {
    return {};
  }
  std::map<ByteString, DataVector<uint8_t>> tables;
  for (const TableRecord& table : directory->tables) {
    pdfium::span<const uint8_t> data =
        bytes.subspan(table.offset, table.length);
    // A tag named twice: which table is meant can't be told.
    if (!tables
             .emplace(table.tag, DataVector<uint8_t>(data.begin(), data.end()))
             .second) {
      return {};
    }
  }
  return WriteSfnt(directory->version, tables);
}
