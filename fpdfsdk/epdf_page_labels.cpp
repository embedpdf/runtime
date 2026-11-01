// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Page label sections (public/epdf_doc.h): /PageLabels read as sections, and
// written from them.

#include <algorithm>
#include <array>
#include <set>
#include <utility>
#include <vector>

#include "constants/catalog.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/epdf_doc.h"

namespace {

// As deep as PDFium reads a name tree.
constexpr int kMaxTreeDepth = 32;

// /S of each EPDF_PAGE_LABEL_STYLE_*, by value; NONE writes none.
constexpr std::array<const char*, 6> kStyleNames = {nullptr, "D", "R",
                                                    "r",     "A", "a"};

struct Section {
  int first_page = 0;
  int style = EPDF_PAGE_LABEL_STYLE_DECIMAL;
  int first_number = 1;
  WideString prefix;
};

struct Entry {
  int key = 0;
  RetainPtr<const CPDF_Object> value;
};

int StyleOf(const ByteString& name) {
  for (size_t i = 1; i < kStyleNames.size(); ++i) {
    if (name == kStyleNames[i]) {
      return static_cast<int>(i);
    }
  }
  return EPDF_PAGE_LABEL_STYLE_NONE;
}

// The entries of the number tree at |node|, in tree order. A node with /Nums
// is a leaf, as PDFium reads it: its /Kids, if any, are not read.
void CollectEntries(const CPDF_Dictionary* node,
                    int depth,
                    std::set<const CPDF_Dictionary*>* visited,
                    std::vector<Entry>* entries) {
  if (!node || depth > kMaxTreeDepth || !visited->insert(node).second) {
    return;
  }
  RetainPtr<const CPDF_Array> nums = node->GetArrayFor("Nums");
  if (nums) {
    for (size_t i = 0; i + 1 < nums->size(); i += 2) {
      entries->push_back(
          {nums->GetIntegerAt(i), nums->GetDirectObjectAt(i + 1)});
    }
    return;
  }
  RetainPtr<const CPDF_Array> kids = node->GetArrayFor("Kids");
  if (!kids) {
    return;
  }
  for (size_t i = 0; i < kids->size(); ++i) {
    CollectEntries(kids->GetDictAt(i).Get(), depth + 1, visited, entries);
  }
}

// The page label sections of |doc|, as EPDFDoc_GetPageLabelSectionCount()
// describes them. Read through the document's view.
std::vector<Section> ReadSections(CPDF_Document* doc) {
  const CPDF_Dictionary* catalog = doc->GetRoot();
  RetainPtr<const CPDF_Dictionary> labels =
      catalog ? catalog->GetDictFor(pdfium::catalog::kPageLabels) : nullptr;
  const int page_count = doc->GetPageCount();
  if (!labels || page_count <= 0) {
    return {};
  }

  std::vector<Entry> entries;
  std::set<const CPDF_Dictionary*> visited;
  CollectEntries(labels.Get(), 0, &visited, &entries);
  std::erase_if(entries, [page_count](const Entry& entry) {
    return entry.key < 0 || entry.key >= page_count;
  });
  std::stable_sort(
      entries.begin(), entries.end(),
      [](const Entry& a, const Entry& b) { return a.key < b.key; });

  std::vector<Section> sections;
  if (entries.empty() || entries.front().key > 0) {
    sections.push_back(Section());
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    // Of two entries with one key, the later one counts.
    if (i + 1 < entries.size() && entries[i + 1].key == entries[i].key) {
      continue;
    }
    const Entry& entry = entries[i];
    const CPDF_Dictionary* label =
        entry.value ? entry.value->AsDictionary() : nullptr;
    Section section;
    section.first_page = entry.key;
    if (!label) {
      section.first_number = entry.key + 1;
      sections.push_back(std::move(section));
      continue;
    }
    section.style = StyleOf(label->GetByteStringFor("S"));
    section.first_number = label->GetIntegerFor("St", 1);
    if (label->KeyExist("P")) {
      section.prefix = label->GetUnicodeTextFor("P");
    }
    sections.push_back(std::move(section));
  }
  return sections;
}

}  // namespace

FPDF_EXPORT int FPDF_CALLCONV
EPDFDoc_GetPageLabelSectionCount(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return -1;
  }
  CPDF_DocumentViewScope document_view(doc);
  return pdfium::checked_cast<int>(ReadSections(doc).size());
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetPageLabelSectionAt(FPDF_DOCUMENT document,
                              int index,
                              FPDF_WCHAR* buffer,
                              unsigned long buflen,
                              int* first_page,
                              int* style,
                              int* first_number) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || index < 0) {
    return 0;
  }
  CPDF_DocumentViewScope document_view(doc);
  const std::vector<Section> sections = ReadSections(doc);
  if (static_cast<size_t>(index) >= sections.size()) {
    return 0;
  }
  const Section& section = sections[index];
  if (first_page) {
    *first_page = section.first_page;
  }
  if (style) {
    *style = section.style;
  }
  if (first_number) {
    *first_number = section.first_number;
  }
  // SAFETY: required from caller.
  return Utf16EncodeMaybeCopyAndReturnLength(
      section.prefix, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_SetPageLabelSections(FPDF_DOCUMENT document,
                             const int* first_pages,
                             const int* styles,
                             const int* first_numbers,
                             const FPDF_WIDESTRING* prefixes,
                             unsigned long count) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || (count > 0 && (!first_pages || !styles || !first_numbers))) {
    return false;
  }
  CPDF_DocumentViewScope document_view(doc);
  const CPDF_Dictionary* catalog = doc->GetRoot();
  if (!catalog) {
    return false;
  }
  if (count == 0) {
    if (catalog->KeyExist(pdfium::catalog::kPageLabels)) {
      RetainPtr<CPDF_Dictionary> writable = doc->GetMutableRoot();
      if (!writable) {
        return false;
      }
      writable->RemoveFor(pdfium::catalog::kPageLabels);
    }
    return true;
  }

  const size_t size = pdfium::checked_cast<size_t>(count);
  // SAFETY: required from caller, |count| of each.
  const auto pages = UNSAFE_BUFFERS(pdfium::span(first_pages, size));
  const auto style_values = UNSAFE_BUFFERS(pdfium::span(styles, size));
  const auto numbers = UNSAFE_BUFFERS(pdfium::span(first_numbers, size));
  const pdfium::span<const FPDF_WIDESTRING> prefix_values =
      prefixes ? UNSAFE_BUFFERS(pdfium::span(prefixes, size))
               : pdfium::span<const FPDF_WIDESTRING>();
  const int page_count = doc->GetPageCount();
  for (size_t i = 0; i < size; ++i) {
    const int previous = i == 0 ? -1 : pages[i - 1];
    if ((i == 0 && pages[i] != 0) || pages[i] <= previous ||
        pages[i] >= page_count || style_values[i] < 0 ||
        static_cast<size_t>(style_values[i]) >= kStyleNames.size()) {
      return false;
    }
  }

  // Its own object keeps its number; on a layer the write lands in the
  // layer's copy of it.
  RetainPtr<const CPDF_Reference> reference =
      ToReference(catalog->GetObjectFor(pdfium::catalog::kPageLabels));
  RetainPtr<CPDF_Dictionary> labels =
      reference ? ToDictionary(
                      doc->GetMutableIndirectObject(reference->GetRefObjNum()))
                : nullptr;
  if (labels) {
    labels->RemoveFor("Kids");
    labels->RemoveFor("Limits");
  } else {
    RetainPtr<CPDF_Dictionary> writable = doc->GetMutableRoot();
    if (!writable) {
      return false;
    }
    labels = writable->SetNewFor<CPDF_Dictionary>(pdfium::catalog::kPageLabels);
  }

  auto nums = labels->SetNewFor<CPDF_Array>("Nums");
  for (size_t i = 0; i < size; ++i) {
    nums->AppendNew<CPDF_Number>(pages[i]);
    auto label = nums->AppendNew<CPDF_Dictionary>();
    if (const char* name = kStyleNames[style_values[i]]) {
      label->SetNewFor<CPDF_Name>("S", name);
    }
    const WideString prefix =
        i < prefix_values.size()
            ? WideStringFromFPDFWideString(prefix_values[i])
            : WideString();
    if (!prefix.IsEmpty()) {
      label->SetNewFor<CPDF_String>("P", prefix.AsStringView());
    }
    if (numbers[i] != 1) {
      label->SetNewFor<CPDF_Number>("St", numbers[i]);
    }
  }
  return true;
}
