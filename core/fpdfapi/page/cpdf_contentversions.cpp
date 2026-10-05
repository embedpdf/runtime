// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/page/cpdf_contentversions.h"

#include <utility>

#include "constants/page_object.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_stream.h"

CPDF_ContentVersions::CPDF_ContentVersions() = default;

CPDF_ContentVersions::~CPDF_ContentVersions() = default;

// static
std::vector<RetainPtr<const CPDF_Stream>>
CPDF_ContentVersions::ContentStreamsOf(const CPDF_Dictionary* page_dict) {
  std::vector<RetainPtr<const CPDF_Stream>> streams;
  RetainPtr<const CPDF_Object> contents =
      page_dict ? page_dict->GetDirectObjectFor(pdfium::page_object::kContents)
                : nullptr;
  if (!contents) {
    return streams;
  }
  if (const CPDF_Stream* stream = contents->AsStream()) {
    streams.push_back(pdfium::WrapRetain(stream));
    return streams;
  }
  if (const CPDF_Array* array = contents->AsArray()) {
    for (size_t i = 0; i < array->size(); ++i) {
      streams.push_back(ToStream(array->GetDirectObjectAt(i)));
    }
  }
  return streams;
}

void CPDF_ContentVersions::Clear() {
  contents_.clear();
  forms_.clear();
}

void CPDF_ContentVersions::AddContentStream(
    RetainPtr<const CPDF_Stream> stream) {
  contents_.push_back(std::move(stream));
}

void CPDF_ContentVersions::AddFormStream(RetainPtr<const CPDF_Stream> stream) {
  if (stream) {
    forms_.insert(std::move(stream));
  }
}

bool CPDF_ContentVersions::Match(CPDF_Document* doc,
                                 const CPDF_Dictionary* page_dict) const {
  if (ContentStreamsOf(page_dict) != contents_) {
    return false;
  }
  for (const auto& form : forms_) {
    // A stream without a number is not in the document: only this page's
    // objects hold it, so nothing else can make a version of it.
    const uint32_t objnum = form->GetObjNum();
    if (objnum && doc->GetIndirectObject(objnum).Get() != form.Get()) {
      return false;
    }
  }
  return true;
}
