// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "fpdfsdk/epdf_edit_test_util.h"

#include <sstream>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "public/fpdf_edit.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

void Write(std::ostringstream& out, const CPDF_Object* object);

void WriteValue(std::ostringstream& out, const CPDF_Object* value) {
  if (!value->IsInline()) {
    out << " " << value->GetObjNum() << " R";
    return;
  }
  Write(out, value);
}

void Write(std::ostringstream& out, const CPDF_Object* object) {
  switch (object->GetType()) {
    case CPDF_Object::kNullobj:
      out << " null";
      break;
    case CPDF_Object::kBoolean:
    case CPDF_Object::kNumber:
      out << " " << object->GetString();
      break;
    case CPDF_Object::kString:
      out << " " << object->AsString()->EncodeString();
      break;
    case CPDF_Object::kName:
      out << " /" << object->GetString();
      break;
    case CPDF_Object::kReference:
      out << " " << object->AsReference()->GetRefObjNum() << " R";
      break;
    case CPDF_Object::kArray: {
      const CPDF_Array* array = object->AsArray();
      out << " [";
      for (size_t i = 0; i < array->size(); ++i) {
        WriteValue(out, array->GetObjectAt(i).Get());
      }
      out << " ]";
      break;
    }
    case CPDF_Object::kDictionary: {
      CPDF_DictionaryLocker locker(object->AsDictionary());
      out << " <<";
      for (const auto& entry : locker) {
        out << " /" << entry.first;
        WriteValue(out, entry.second.Get());
      }
      out << " >>";
      break;
    }
    case CPDF_Object::kStream: {
      const CPDF_Stream* stream = object->AsStream();
      Write(out, stream->GetDict().Get());
      auto access =
          pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
      access->LoadAllDataRaw();
      pdfium::span<const uint8_t> bytes = access->GetSpan();
      out << " stream " << std::string(bytes.begin(), bytes.end());
      break;
    }
  }
}

}  // namespace

std::string MakePdf(const std::vector<std::string>& objects) {
  std::string pdf = "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const size_t xref = pdf.size();
  pdf += "xref\n0 " + std::to_string(objects.size() + 1) +
         "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    std::string number = std::to_string(offset);
    pdf += std::string(10 - number.size(), '0') + number + " 00000 n \n";
  }
  pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
         " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return pdf;
}

std::string Stream(const std::string& data) {
  return "<< /Length " + std::to_string(data.size()) + " >>\nstream\n" + data +
         "\nendstream";
}

std::map<uint32_t, std::string> ObjectGraph(CPDF_Document* doc) {
  std::map<uint32_t, std::string> graph;
  for (uint32_t number = 1; number <= doc->GetLastObjNum(); ++number) {
    RetainPtr<CPDF_Object> object = doc->GetOrParseIndirectObject(number);
    if (!object) {
      continue;
    }
    std::ostringstream out;
    Write(out, object.Get());
    graph[number] = out.str();
  }
  return graph;
}

std::string WithoutFileId(std::string bytes) {
  size_t at = 0;
  while ((at = bytes.find("/ID", at)) != std::string::npos) {
    size_t close = bytes.find(']', at);
    if (close == std::string::npos) {
      break;
    }
    bool in_hex = false;
    for (size_t i = at + 3; i < close; ++i) {
      if (bytes[i] == '<') {
        in_hex = true;
      } else if (bytes[i] == '>') {
        in_hex = false;
      } else if (in_hex) {
        bytes[i] = '0';
      }
    }
    at = close;
  }
  return bytes;
}

ScopedFPDFAnnotation NewAnnot(FPDF_PAGE page,
                              FPDF_ANNOTATION_SUBTYPE subtype,
                              const FS_RECTF& rect) {
  ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page, subtype));
  EXPECT_TRUE(annot);
  if (annot) {
    EXPECT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  }
  return annot;
}

void AddPngStamp(FPDF_DOCUMENT doc,
                 FPDF_PAGE page,
                 pdfium::span<const uint8_t> png,
                 const FS_RECTF& rect) {
  ScopedFPDFAnnotation stamp = NewAnnot(page, FPDF_ANNOT_STAMP, rect);
  FPDF_PAGEOBJECT image = FPDFPageObj_NewImageObj(doc);
  ASSERT_TRUE(image);
  ASSERT_TRUE(EPDFImageObj_SetPng(nullptr, 0, image, png.data(), png.size()));
  const FS_MATRIX matrix{rect.right - rect.left, 0,         0,
                         rect.top - rect.bottom, rect.left, rect.bottom};
  ASSERT_TRUE(FPDFPageObj_SetMatrix(image, &matrix));
  ASSERT_TRUE(FPDFAnnot_AppendObject(stamp.get(), image));
  ASSERT_TRUE(
      EPDFAnnot_UpdateAppearanceToRect(stamp.get(), EPDF_STAMP_FIT_STRETCH));
}

uint32_t ColorAt(FPDF_PAGE page, int x, int y_from_bottom) {
  ScopedFPDFBitmap bitmap =
      EmbedderTest::RenderPageWithFlags(page, nullptr, FPDF_ANNOT);
  const int height = FPDFBitmap_GetHeight(bitmap.get());
  const int stride = FPDFBitmap_GetStride(bitmap.get());
  const auto* pixels =
      static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get()));
  const uint8_t* pixel = pixels + (height - 1 - y_from_bottom) * stride + x * 4;
  return (pixel[2] << 16) | (pixel[1] << 8) | pixel[0];
}

std::string AnnotString(FPDF_ANNOTATION annot, const char* key) {
  const unsigned long length = FPDFAnnot_GetStringValue(annot, key, nullptr, 0);
  if (length < sizeof(FPDF_WCHAR)) {
    return std::string();
  }
  std::vector<FPDF_WCHAR> buffer(length / sizeof(FPDF_WCHAR));
  FPDFAnnot_GetStringValue(annot, key, buffer.data(), length);
  return GetPlatformString(buffer.data());
}
