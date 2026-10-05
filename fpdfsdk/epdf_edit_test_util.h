// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Helpers for embedder tests that make writes and compare what a document is
// before and after: small PDFs built from object bodies, the object graph as
// text, saves without their /ID, annotations and PNG stamps, pixel colours.

#ifndef FPDFSDK_EPDF_EDIT_TEST_UTIL_H_
#define FPDFSDK_EPDF_EDIT_TEST_UTIL_H_

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "core/fxcrt/span.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdf_annot.h"
#include "public/fpdfview.h"

class CPDF_Document;

// An 8 × 8 red PNG and the same in blue.
inline constexpr uint8_t kRedPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x08, 0x08, 0x02, 0x00, 0x00, 0x00, 0x4b, 0x6d, 0x29, 0xdc,
    0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63,
    0xf8, 0xcf, 0xc0, 0x80, 0x15, 0x61, 0x17, 0x1d, 0xb4, 0x12, 0x00,
    0x28, 0xff, 0x3f, 0xc1, 0x6e, 0xec, 0xdf, 0x61, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
inline constexpr uint8_t kBluePng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x08, 0x08, 0x02, 0x00, 0x00, 0x00, 0x4b, 0x6d, 0x29, 0xdc,
    0x00, 0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63,
    0x60, 0x60, 0xf8, 0x8f, 0x03, 0x0d, 0x29, 0x09, 0x00, 0xa9, 0x70,
    0x3f, 0xc1, 0x14, 0xca, 0xea, 0x73, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

// A PDF of the given object bodies, numbered from 1, with a classic xref.
std::string MakePdf(const std::vector<std::string>& objects);

// A stream object body holding `data`.
std::string Stream(const std::string& data);

// Every indirect object, by number, from 1 to the last, as text: streams with
// their raw bytes, so two graphs compare by value.
std::map<uint32_t, std::string> ObjectGraph(CPDF_Document* doc);

// A save with the digits of its /ID blanked: every save writes a new one.
std::string WithoutFileId(std::string bytes);

// An annotation of `subtype` on `page`, with `rect`.
ScopedFPDFAnnotation NewAnnot(FPDF_PAGE page,
                              FPDF_ANNOTATION_SUBTYPE subtype,
                              const FS_RECTF& rect);

// A stamp drawn from a PNG, as the stamp writer makes one.
void AddPngStamp(FPDF_DOCUMENT doc,
                 FPDF_PAGE page,
                 pdfium::span<const uint8_t> png,
                 const FS_RECTF& rect);

// The colour at a point of a page rendered with its annotations, as 0xRRGGBB.
uint32_t ColorAt(FPDF_PAGE page, int x, int y_from_bottom);

// An annotation's string value |key| as UTF-8; empty when it has none or the
// handle is invalid.
std::string AnnotString(FPDF_ANNOTATION annot, const char* key);

#endif  // FPDFSDK_EPDF_EDIT_TEST_UTIL_H_
