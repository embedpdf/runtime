// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
#include "public/epdf_measure.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object_equality.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfdoc/cpdf_annotfontmap.h"
#include "core/fpdfdoc/cpdf_generateap.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_font.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "testing/embedder_test.h"
#include "testing/embedder_test_environment.h"
#include "testing/embedpdf_layer_fixture.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/test_fonts.h"

namespace {
constexpr auto kX = EPDF_MEASURE_AXIS_X;
constexpr auto kD = EPDF_MEASURE_AXIS_DISTANCE;
const FS_RECTF kBox = {0, 792, 612, 0};
std::wstring Ratio(EPDF_MEASURE measure) {
  auto length = EPDFMeasure_GetRatio(measure, nullptr, 0);
  if (!length) {
    return {};
  }
  auto buffer = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length, EPDFMeasure_GetRatio(measure, buffer.data(), length));
  return GetPlatformWString(buffer.data());
}
std::wstring Appearance(FPDF_ANNOTATION annot) {
  auto length =
      FPDFAnnot_GetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0);
  auto buffer = GetFPDFWideStringBuffer(length);
  if (!length) {
    return {};
  }
  EXPECT_EQ(length, FPDFAnnot_GetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                                    buffer.data(), length));
  return GetPlatformWString(buffer.data());
}
void SetText(FPDF_ANNOTATION annot, const char* key, const wchar_t* text) {
  auto wide = GetFPDFWideString(text);
  ASSERT_TRUE(FPDFAnnot_SetStringValue(annot, key, wide.get()));
}
EPDF_NUMBERFORMAT AddFormat(EPDF_MEASURE measure, EPDF_MEASURE_AXIS axis = kX) {
  auto unit = GetFPDFWideString(L"ft");
  return EPDFMeasure_AddFormat(measure, axis, unit.get(),
                               axis == kX ? 1.0f / 72 : 1);
}
void SetRatio(EPDF_MEASURE measure, const wchar_t* text) {
  auto wide = GetFPDFWideString(text);
  ASSERT_TRUE(EPDFMeasure_SetRatio(measure, wide.get()));
}
void MakeLine(FPDF_ANNOTATION annot) {
  FS_RECTF rect{99, 101, 301, 99};
  FS_POINTF a{100, 100}, b{300, 100};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot, &rect));
  ASSERT_TRUE(EPDFAnnot_SetLine(annot, &a, &b));
  SetText(annot, "Contents", L"3.45 m");
}
struct Delta {
  std::vector<uint8_t> bytes;
  bool changed;
};
Delta SaveDelta(FPDF_DOCUMENT doc) {
  unsigned long size = 0;
  EPDFLayerSaveStatus status;
  FPDF_BOOL changed;
  void* buffer =
      EPDFLayer_SaveDeltaToOwnedBufferEx(doc, &size, &status, &changed);
  EXPECT_EQ(EPDFLayerSaveStatus_kSuccess, status);
  Delta result{{}, !!changed};
  if (buffer) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    result.bytes.assign(bytes, bytes + size);
    EPDF_FreeBuffer(buffer);
  }
  return result;
}
class EPDFMeasureEmbedderTest : public EmbedderTest {
 protected:
  std::vector<uint8_t> Base(bool indirect = false) {
    ScopedFPDFDocument doc(FPDF_CreateNewDocument());
    ScopedFPDFPage page(FPDFPage_New(doc.get(), 0, 612, 792));
    ScopedFPDFAnnotation a(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINE));
    ScopedFPDFAnnotation b(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINE));
    MakeLine(a.get());
    MakeLine(b.get());
    auto measure = EPDFAnnot_AddMeasure(a.get());
    SetRatio(measure, L"1 in = 1 ft");
    EXPECT_TRUE(AddFormat(measure));
    auto viewport = EPDFPage_AddViewport(page.get(), &kBox);
    EXPECT_TRUE(viewport);
    SetRatio(EPDFViewport_AddMeasure(viewport), L"page scale");
    auto* document = CPDFDocumentFromFPDFDocument(doc.get());
    auto ad =
        CPDFAnnotContextFromFPDFAnnotation(a.get())->GetMutableAnnotDict();
    auto bd =
        CPDFAnnotContextFromFPDFAnnotation(b.get())->GetMutableAnnotDict();
    if (indirect) {
      auto m = ad->GetMutableDictFor("Measure");
      auto formats = m->GetMutableArrayFor("X");
      formats->ConvertToIndirectObjectAt(0, document);
      m->ConvertToIndirectObjectFor("X", document);
      ad->ConvertToIndirectObjectFor("Measure", document);
      bd->SetFor("Measure", ad->GetObjectFor("Measure")->Clone());
    } else {
      bd->SetFor("Measure", ad->GetDictFor("Measure")->Clone());
    }
    ClearString();
    EXPECT_TRUE(FPDF_SaveAsCopy(doc.get(), this, 0));
    return {GetString().begin(), GetString().end()};
  }
};
}  // namespace

TEST_F(EPDFMeasureEmbedderTest, BadArgumentsAndRejectedWrites) {
  EXPECT_EQ(nullptr, EPDFAnnot_GetMeasure(nullptr));
  EXPECT_EQ(nullptr, EPDFAnnot_AddMeasure(nullptr));
  EXPECT_FALSE(EPDFAnnot_RemoveMeasure(nullptr));
  EXPECT_EQ(0u, EPDFPage_CountViewports(nullptr));
  EXPECT_EQ(nullptr, EPDFPage_GetViewport(nullptr, 0));
  EXPECT_EQ(nullptr, EPDFPage_AddViewport(nullptr, &kBox));
  EXPECT_FALSE(EPDFPage_RemoveViewport(nullptr, 0));
  EXPECT_EQ(-1, EPDFPage_FindViewport(nullptr, nullptr));
  EXPECT_FALSE(EPDFViewport_GetBBox(nullptr, nullptr));
  EXPECT_FALSE(EPDFViewport_SetBBox(nullptr, &kBox));
  EXPECT_FALSE(EPDFViewport_SetName(nullptr, nullptr));
  EXPECT_EQ(nullptr, EPDFViewport_GetMeasure(nullptr));
  EXPECT_EQ(nullptr, EPDFViewport_AddMeasure(nullptr));
  EXPECT_FALSE(EPDFViewport_RemoveMeasure(nullptr));
  EXPECT_EQ(EPDF_MEASURE_SUBTYPE_UNKNOWN, EPDFMeasure_GetSubtype(nullptr));
  EXPECT_EQ(0u, EPDFMeasure_GetRatio(nullptr, nullptr, 0));
  EXPECT_FALSE(EPDFMeasure_SetRatio(nullptr, nullptr));
  EXPECT_FALSE(EPDFMeasure_GetOrigin(nullptr, nullptr));
  EXPECT_FALSE(EPDFMeasure_SetOrigin(nullptr, nullptr));
  EXPECT_FALSE(EPDFMeasure_GetCYX(nullptr, nullptr));
  EXPECT_FALSE(EPDFMeasure_SetCYX(nullptr, nullptr));
  EXPECT_EQ(0u, EPDFMeasure_CountFormats(nullptr, kX));
  EXPECT_EQ(nullptr, EPDFMeasure_GetFormat(nullptr, kX, 0));
  EXPECT_EQ(nullptr, AddFormat(nullptr));
  EXPECT_FALSE(EPDFMeasure_RemoveFormats(nullptr, kX));
  EXPECT_EQ(0u, EPDFNumberFormat_GetUnit(nullptr, nullptr, 0));
  EXPECT_FALSE(EPDFNumberFormat_SetUnit(nullptr, nullptr));
  EXPECT_FALSE(EPDFNumberFormat_GetConversion(nullptr, nullptr));
  EXPECT_FALSE(EPDFNumberFormat_SetConversion(nullptr, 1));
  EXPECT_FALSE(EPDFNumberFormat_GetFraction(nullptr, nullptr));
  EXPECT_FALSE(
      EPDFNumberFormat_SetFraction(nullptr, EPDF_MEASURE_FRACTION_DECIMAL));
  EXPECT_FALSE(EPDFNumberFormat_GetPrecision(nullptr, nullptr));
  EXPECT_FALSE(EPDFNumberFormat_SetPrecision(nullptr, 100));
  EXPECT_FALSE(EPDFNumberFormat_GetFixedDenominator(nullptr, nullptr));
  EXPECT_FALSE(EPDFNumberFormat_SetFixedDenominator(nullptr, false));
  EXPECT_FALSE(EPDFNumberFormat_GetLabelPosition(nullptr, nullptr));
  EXPECT_FALSE(
      EPDFNumberFormat_SetLabelPosition(nullptr, EPDF_MEASURE_LABEL_SUFFIX));
  EXPECT_EQ(0u, EPDFNumberFormat_GetText(
                    nullptr, EPDF_MEASURE_TEXT_DECIMAL_SEPARATOR, nullptr, 0));
  EXPECT_FALSE(EPDFNumberFormat_SetText(
      nullptr, EPDF_MEASURE_TEXT_DECIMAL_SEPARATOR, nullptr));
  EXPECT_FALSE(EPDFAnnot_SetLineLeader(nullptr, 0, 0, 0));
  EXPECT_FALSE(EPDFAnnot_GetLineLeader(nullptr, nullptr, nullptr, nullptr));
  EXPECT_FALSE(
      EPDFAnnot_SetLineCaption(nullptr, true, EPDF_CAPTION_INLINE, nullptr));
  EXPECT_FALSE(EPDFAnnot_GetLineCaption(nullptr, nullptr, nullptr, nullptr));
  EXPECT_FALSE(EPDFAnnot_SetShapeCaption(nullptr, true, nullptr));
  EXPECT_FALSE(EPDFAnnot_GetShapeCaption(nullptr, nullptr, nullptr, nullptr));
  embedpdf_test::LayerFixture layer;
  ASSERT_TRUE(layer.OpenBytes(Base()));
  ScopedFPDFPage page(FPDF_LoadPage(layer.layer, 0));
  ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page.get(), 0));
  auto m = EPDFAnnot_GetMeasure(annot.get());
  auto f = EPDFMeasure_GetFormat(m, kX, 0);
  auto invalid = static_cast<EPDF_MEASURE_AXIS>(99);
  EXPECT_EQ(nullptr, EPDFMeasure_GetFormat(m, invalid, 0));
  EXPECT_EQ(nullptr, EPDFMeasure_GetFormat(m, kX, 10));
  EXPECT_FALSE(EPDFMeasure_RemoveFormats(m, invalid));
  EXPECT_FALSE(EPDFNumberFormat_SetPrecision(f, 0));
  EXPECT_FALSE(EPDFNumberFormat_SetConversion(
      f, std::numeric_limits<float>::infinity()));
  EXPECT_FALSE(EPDFNumberFormat_SetConversion(f, -1));
  EXPECT_FALSE(EPDFAnnot_SetLineLeader(annot.get(), 1, -1, 0));
  FS_POINTF bad{NAN, 0};
  EXPECT_FALSE(EPDFMeasure_SetOrigin(m, &bad));
  EXPECT_FALSE(EPDFAnnot_SetShapeCaption(annot.get(), true, nullptr));
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.layer));
}

TEST_F(EPDFMeasureEmbedderTest, NumberFormatsRoundTripAndAbsentVersusEmpty) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINE));
  auto m = EPDFAnnot_AddMeasure(annot.get());
  ASSERT_TRUE(m);
  EXPECT_EQ(m, EPDFAnnot_GetMeasure(annot.get()));
  SetRatio(m, L"1 in = 1 ft");
  auto f = AddFormat(m);
  ASSERT_TRUE(f);
  int precision = 7;
  FPDF_BOOL fixed = 7;
  EPDF_MEASURE_FRACTION fraction;
  EPDF_MEASURE_LABEL_POSITION position;
  EXPECT_FALSE(EPDFNumberFormat_GetPrecision(f, &precision));
  EXPECT_EQ(7, precision);
  EXPECT_FALSE(EPDFNumberFormat_GetFraction(f, &fraction));
  EXPECT_FALSE(EPDFNumberFormat_GetFixedDenominator(f, &fixed));
  EXPECT_FALSE(EPDFNumberFormat_GetLabelPosition(f, &position));
  auto empty = GetFPDFWideString(L"");
  for (auto key :
       {EPDF_MEASURE_TEXT_THOUSANDS_SEPARATOR,
        EPDF_MEASURE_TEXT_DECIMAL_SEPARATOR, EPDF_MEASURE_TEXT_PREFIX_SPACING,
        EPDF_MEASURE_TEXT_SUFFIX_SPACING}) {
    EXPECT_EQ(0u, EPDFNumberFormat_GetText(f, key, nullptr, 0));
    EXPECT_TRUE(EPDFNumberFormat_SetText(f, key, empty.get()));
    EXPECT_EQ(2u, EPDFNumberFormat_GetText(f, key, nullptr, 0));
    EXPECT_TRUE(EPDFNumberFormat_SetText(f, key, nullptr));
    EXPECT_EQ(0u, EPDFNumberFormat_GetText(f, key, nullptr, 0));
  }
  for (int axis = 1; axis <= 5; ++axis) {
    EXPECT_TRUE(AddFormat(m, static_cast<EPDF_MEASURE_AXIS>(axis)));
  }
  auto inches = GetFPDFWideString(L"in");
  auto last = EPDFMeasure_AddFormat(m, kD, inches.get(), 12);
  ASSERT_TRUE(last);
  EXPECT_TRUE(
      EPDFNumberFormat_SetFraction(last, EPDF_MEASURE_FRACTION_FRACTION));
  EXPECT_TRUE(EPDFNumberFormat_SetPrecision(last, 16));
  EXPECT_TRUE(EPDFNumberFormat_SetFixedDenominator(last, true));
  EXPECT_TRUE(
      EPDFNumberFormat_SetLabelPosition(last, EPDF_MEASURE_LABEL_PREFIX));
  FS_POINTF origin{-20, 30};
  float cyx = 2;
  EXPECT_TRUE(EPDFMeasure_SetOrigin(m, &origin));
  EXPECT_TRUE(EPDFMeasure_SetCYX(m, &cyx));
  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  auto saved = OpenScopedSavedDocument();
  ASSERT_TRUE(saved);
  ScopedFPDFPage reopened(FPDF_LoadPage(saved.get(), 0));
  ScopedFPDFAnnotation a(FPDFPage_GetAnnot(reopened.get(), 0));
  auto read = EPDFAnnot_GetMeasure(a.get());
  EXPECT_EQ(L"1 in = 1 ft", Ratio(read));
  EXPECT_EQ(2u, EPDFMeasure_CountFormats(read, kD));
  last = EPDFMeasure_GetFormat(read, kD, 1);
  EXPECT_TRUE(EPDFNumberFormat_GetFraction(last, &fraction));
  EXPECT_EQ(EPDF_MEASURE_FRACTION_FRACTION, fraction);
  EXPECT_TRUE(EPDFNumberFormat_GetPrecision(last, &precision));
  EXPECT_EQ(16, precision);
  EXPECT_TRUE(EPDFNumberFormat_GetFixedDenominator(last, &fixed));
  EXPECT_TRUE(fixed);
  EXPECT_TRUE(EPDFNumberFormat_GetLabelPosition(last, &position));
  EXPECT_EQ(EPDF_MEASURE_LABEL_PREFIX, position);
  FS_POINTF output;
  EXPECT_TRUE(EPDFMeasure_GetOrigin(read, &output));
  EXPECT_FLOAT_EQ(-20, output.x);
  EXPECT_TRUE(EPDFMeasure_GetCYX(read, &cyx));
  EXPECT_FLOAT_EQ(2, cyx);
  EXPECT_TRUE(EPDFMeasure_SetOrigin(read, nullptr));
  EXPECT_FALSE(EPDFMeasure_GetOrigin(read, &output));
  EXPECT_TRUE(EPDFMeasure_SetCYX(read, nullptr));
  EXPECT_FALSE(EPDFMeasure_GetCYX(read, &cyx));
}

TEST_F(EPDFMeasureEmbedderTest, HandlesInvalidateAcrossTwoAnnotationHandles) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ScopedFPDFAnnotation a(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINE));
  ScopedFPDFAnnotation b(FPDFPage_GetAnnot(page.get(), 0));
  auto ma = EPDFAnnot_AddMeasure(a.get());
  auto old = AddFormat(ma);
  ASSERT_TRUE(old);
  auto mb = EPDFAnnot_GetMeasure(b.get());
  auto sibling = EPDFMeasure_GetFormat(mb, kX, 0);
  ASSERT_TRUE(AddFormat(mb));
  float value;
  EXPECT_TRUE(EPDFNumberFormat_GetConversion(old, &value));
  ASSERT_TRUE(EPDFMeasure_RemoveFormats(mb, kX));
  ASSERT_TRUE(AddFormat(ma));
  EXPECT_FALSE(EPDFNumberFormat_SetConversion(old, 100));
  EXPECT_FALSE(EPDFNumberFormat_GetConversion(sibling, &value));
  EXPECT_EQ(EPDF_MEASURE_SUBTYPE_RL, EPDFMeasure_GetSubtype(ma));
  auto fresh = EPDFAnnot_AddMeasure(b.get());
  ASSERT_TRUE(fresh);
  EXPECT_EQ(EPDF_MEASURE_SUBTYPE_UNKNOWN, EPDFMeasure_GetSubtype(ma));
  EXPECT_FALSE(EPDFMeasure_SetRatio(mb, GetFPDFWideString(L"stale").get()));
  EXPECT_TRUE(EPDFAnnot_RemoveMeasure(a.get()));
  EXPECT_EQ(EPDF_MEASURE_SUBTYPE_UNKNOWN, EPDFMeasure_GetSubtype(fresh));
  EXPECT_TRUE(EPDFAnnot_AddMeasure(a.get()));
  EXPECT_EQ(EPDF_MEASURE_SUBTYPE_UNKNOWN, EPDFMeasure_GetSubtype(ma));
}

TEST_F(EPDFMeasureEmbedderTest, ViewportsFindLastAndInvalidateOnRemoval) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  auto a = EPDFPage_AddViewport(page.get(), &kBox);
  const FS_RECTF box{50, 300, 300, 50};
  auto b = EPDFPage_AddViewport(page.get(), &box);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  SetRatio(EPDFViewport_AddMeasure(a), L"first");
  auto mb = EPDFViewport_AddMeasure(b);
  SetRatio(mb, L"second");
  auto fb = AddFormat(mb);
  FS_POINTF point{100, 100};
  EXPECT_EQ(1, EPDFPage_FindViewport(page.get(), &point));
  point = {10, 10};
  EXPECT_EQ(0, EPDFPage_FindViewport(page.get(), &point));
  point = {-1, -1};
  EXPECT_EQ(-1, EPDFPage_FindViewport(page.get(), &point));
  // Separate page handles share the structural generation too.
  ScopedFPDFPage other(FPDF_LoadPage(document(), 0));
  auto other_b = EPDFPage_GetViewport(other.get(), 1);
  ASSERT_TRUE(EPDFPage_RemoveViewport(other.get(), 0));
  FS_RECTF out;
  EXPECT_FALSE(EPDFViewport_GetBBox(a, &out));
  EXPECT_FALSE(EPDFViewport_GetBBox(b, &out));
  EXPECT_FALSE(EPDFViewport_GetBBox(other_b, &out));
  EXPECT_EQ(EPDF_MEASURE_SUBTYPE_UNKNOWN, EPDFMeasure_GetSubtype(mb));
  EXPECT_FALSE(EPDFNumberFormat_SetPrecision(fb, 10));
  auto remaining = EPDFPage_GetViewport(page.get(), 0);
  EXPECT_EQ(L"second", Ratio(EPDFViewport_GetMeasure(remaining)));
  EXPECT_TRUE(EPDFPage_RemoveViewport(page.get(), 0));
  EXPECT_FALSE(CPDFPageFromFPDFPage(page.get())->GetDict()->KeyExist("VP"));
}

TEST_F(EPDFMeasureEmbedderTest,
       UnknownMeasureIsPreservedAndGeoSettersRejectBeforePromotion) {
  auto bytes = Base();
  ScopedFPDFDocument doc(
      FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr));
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ScopedFPDFAnnotation a(FPDFPage_GetAnnot(page.get(), 0));
  auto dict = CPDFAnnotContextFromFPDFAnnotation(a.get())
                  ->GetMutableAnnotDict()
                  ->GetMutableDictFor("Measure");
  dict->SetNewFor<CPDF_Name>("Subtype", "GEO");
  dict->SetNewFor<CPDF_String>("Vendor", "keep");
  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(doc.get(), this, 0));
  embedpdf_test::LayerFixture layer;
  ASSERT_TRUE(layer.OpenBytes({GetString().begin(), GetString().end()}));
  ScopedFPDFPage lp(FPDF_LoadPage(layer.layer, 0));
  ScopedFPDFAnnotation la(FPDFPage_GetAnnot(lp.get(), 0));
  auto m = EPDFAnnot_GetMeasure(la.get());
  ASSERT_TRUE(m);
  EXPECT_EQ(EPDF_MEASURE_SUBTYPE_GEO, EPDFMeasure_GetSubtype(m));
  EXPECT_EQ(nullptr, EPDFAnnot_AddMeasure(la.get()));
  EXPECT_FALSE(EPDFMeasure_SetRatio(m, GetFPDFWideString(L"bad").get()));
  EXPECT_FALSE(EPDFMeasure_SetOrigin(m, nullptr));
  EXPECT_FALSE(EPDFMeasure_SetCYX(m, nullptr));
  EXPECT_EQ(nullptr, AddFormat(m));
  EXPECT_FALSE(EPDFMeasure_RemoveFormats(m, kX));
  EXPECT_FALSE(
      EPDFNumberFormat_SetConversion(EPDFMeasure_GetFormat(m, kX, 0), 100));
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.layer));
}

TEST_F(EPDFMeasureEmbedderTest, SharedIndirectMeasureAndNestedFormatsDetach) {
  embedpdf_test::LayerFixture layer;
  ASSERT_TRUE(layer.OpenBytes(Base(true)));
  ScopedFPDFPage page(FPDF_LoadPage(layer.layer, 0));
  ScopedFPDFAnnotation a(FPDFPage_GetAnnot(page.get(), 0)),
      b(FPDFPage_GetAnnot(page.get(), 1));
  auto ma = EPDFAnnot_GetMeasure(a.get()), mb = EPDFAnnot_GetMeasure(b.get());
  auto fa = EPDFMeasure_GetFormat(ma, kX, 0),
       fb = EPDFMeasure_GetFormat(mb, kX, 0);
  ASSERT_TRUE(EPDFNumberFormat_SetConversion(fa, 2));
  float value;
  ASSERT_TRUE(EPDFNumberFormat_GetConversion(fb, &value));
  EXPECT_FLOAT_EQ(1.0f / 72, value);
  EXPECT_EQ(1u, EPDFLayer_GetPromotedObjectCount(layer.layer));
  auto* context = CPDFAnnotContextFromFPDFAnnotation(a.get());
  EXPECT_TRUE(context->GetAnnotDict()->GetObjectFor("Measure")->IsDictionary());
  EXPECT_EQ(L"1 in = 1 ft", Ratio(ma));  // path survives dictionary promotion
  auto delta = SaveDelta(layer.layer);
  ASSERT_TRUE(delta.changed);
  a.reset();
  b.reset();
  page.reset();
  ASSERT_TRUE(layer.Reopen(std::move(delta.bytes)));
  page.reset(FPDF_LoadPage(layer.layer, 0));
  a.reset(FPDFPage_GetAnnot(page.get(), 0));
  b.reset(FPDFPage_GetAnnot(page.get(), 1));
  EXPECT_TRUE(EPDFNumberFormat_GetConversion(
      EPDFMeasure_GetFormat(EPDFAnnot_GetMeasure(a.get()), kX, 0), &value));
  EXPECT_FLOAT_EQ(2, value);
  EXPECT_TRUE(EPDFNumberFormat_GetConversion(
      EPDFMeasure_GetFormat(EPDFAnnot_GetMeasure(b.get()), kX, 0), &value));
  EXPECT_FLOAT_EQ(1.0f / 72, value);
}

TEST_F(EPDFMeasureEmbedderTest, LayerReadsPromoteNothingAndPageWritesOnlyPage) {
  embedpdf_test::LayerFixture layer;
  ASSERT_TRUE(layer.OpenBytes(Base()));
  ScopedFPDFPage page(FPDF_LoadPage(layer.layer, 0));
  ScopedFPDFAnnotation a(FPDFPage_GetAnnot(page.get(), 0));
  auto m = EPDFAnnot_GetMeasure(a.get());
  EXPECT_EQ(L"1 in = 1 ft", Ratio(m));
  float conversion;
  EXPECT_TRUE(EPDFNumberFormat_GetConversion(EPDFMeasure_GetFormat(m, kX, 0),
                                             &conversion));
  auto viewport = EPDFPage_GetViewport(page.get(), 0);
  FS_RECTF box;
  EXPECT_TRUE(EPDFViewport_GetBBox(viewport, &box));
  EXPECT_EQ(L"page scale", Ratio(EPDFViewport_GetMeasure(viewport)));
  EXPECT_EQ(0u, EPDFLayer_GetPromotedObjectCount(layer.layer));
  EXPECT_TRUE(
      EPDFViewport_SetName(viewport, GetFPDFWideString(L"EmbedPDF").get()));
  EXPECT_EQ(1u, EPDFLayer_GetPromotedObjectCount(layer.layer));
  auto delta = SaveDelta(layer.layer);
  ASSERT_TRUE(delta.changed);
  a.reset();
  page.reset();
  ASSERT_TRUE(layer.Reopen(std::move(delta.bytes)));
  const auto loaded_count = EPDFLayer_GetPromotedObjectCount(layer.layer);
  page.reset(FPDF_LoadPage(layer.layer, 0));
  viewport = EPDFPage_GetViewport(page.get(), 0);
  EXPECT_EQ(18u, EPDFViewport_GetName(viewport, nullptr, 0));
  EXPECT_EQ(loaded_count, EPDFLayer_GetPromotedObjectCount(layer.layer));
  EXPECT_TRUE(
      EPDFViewport_SetName(viewport, GetFPDFWideString(L"EmbedPDF").get()));
  EXPECT_FALSE(SaveDelta(layer.layer).changed);
}

TEST_F(EPDFMeasureEmbedderTest, LineAndShapeCaptionEntriesStaySeparate) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ScopedFPDFAnnotation line(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINE));
  ScopedFPDFAnnotation shape(
      EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_POLYGON));
  EPDF_CAPTION_OFFSET offset{12, 8}, displacement;
  FS_POINTF center{-20, 430}, out;
  ASSERT_TRUE(
      EPDFAnnot_SetLineCaption(line.get(), true, EPDF_CAPTION_TOP, &offset));
  FPDF_BOOL enabled, has_center;
  EPDF_CAPTION_POSITION position;
  EXPECT_TRUE(
      EPDFAnnot_GetLineCaption(line.get(), &enabled, &position, &displacement));
  EXPECT_TRUE(enabled);
  EXPECT_EQ(EPDF_CAPTION_TOP, position);
  EXPECT_FLOAT_EQ(12, displacement.along);
  ASSERT_TRUE(
      EPDFAnnot_SetLineCaption(line.get(), false, position, &displacement));
  ASSERT_TRUE(
      EPDFAnnot_GetLineCaption(line.get(), &enabled, &position, &displacement));
  EXPECT_FALSE(enabled);
  EXPECT_EQ(EPDF_CAPTION_TOP, position);
  EXPECT_FLOAT_EQ(12, displacement.along);
  EXPECT_FLOAT_EQ(8, displacement.perpendicular);
  ASSERT_TRUE(
      EPDFAnnot_SetLineCaption(line.get(), true, EPDF_CAPTION_INLINE, nullptr));
  ASSERT_TRUE(
      EPDFAnnot_GetLineCaption(line.get(), &enabled, &position, &displacement));
  EXPECT_TRUE(enabled);
  EXPECT_EQ(EPDF_CAPTION_INLINE, position);
  EXPECT_FLOAT_EQ(0, displacement.along);
  EXPECT_FLOAT_EQ(0, displacement.perpendicular);
  EXPECT_FALSE(EPDFAnnot_SetLineCaption(shape.get(), true, EPDF_CAPTION_INLINE,
                                        nullptr));
  EXPECT_FALSE(EPDFAnnot_SetShapeCaption(line.get(), true, nullptr));
  ASSERT_TRUE(EPDFAnnot_SetShapeCaption(shape.get(), true, &center));
  EXPECT_TRUE(
      EPDFAnnot_GetShapeCaption(shape.get(), &enabled, &has_center, &out));
  EXPECT_TRUE(enabled);
  EXPECT_TRUE(has_center);
  EXPECT_FLOAT_EQ(-20, out.x);
  EXPECT_FLOAT_EQ(430, out.y);
  ASSERT_TRUE(EPDFAnnot_SetShapeCaption(shape.get(), false, &center));
  ASSERT_TRUE(
      EPDFAnnot_GetShapeCaption(shape.get(), &enabled, &has_center, &out));
  EXPECT_FALSE(enabled);
  EXPECT_TRUE(has_center);
  EXPECT_FLOAT_EQ(-20, out.x);
  EXPECT_FLOAT_EQ(430, out.y);
  auto* dict = CPDFAnnotContextFromFPDFAnnotation(shape.get())->GetAnnotDict();
  EXPECT_FALSE(dict->KeyExist("Cap"));
  EXPECT_FALSE(dict->KeyExist("CO"));
  EXPECT_FALSE(dict->KeyExist("CP"));
  EXPECT_TRUE(EPDFAnnot_SetShapeCaption(shape.get(), true, nullptr));
  EXPECT_TRUE(
      EPDFAnnot_GetShapeCaption(shape.get(), &enabled, &has_center, &out));
  EXPECT_FALSE(has_center);
  float ll, lle, llo;
  EXPECT_TRUE(EPDFAnnot_SetLineLeader(line.get(), -15, 5, 2));
  EXPECT_TRUE(EPDFAnnot_GetLineLeader(line.get(), &ll, &lle, &llo));
  EXPECT_FLOAT_EQ(-15, ll);
  EXPECT_FLOAT_EQ(5, lle);
  EXPECT_FLOAT_EQ(2, llo);
  EXPECT_FALSE(EPDFAnnot_SetLineLeader(shape.get(), 15, 5, 0));
}

TEST_F(EPDFMeasureEmbedderTest, LineAppearanceLeadersInlineGapAndStableBounds) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ScopedFPDFAnnotation a(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINE));
  MakeLine(a.get());
  ASSERT_TRUE(EPDFAnnot_SetLineLeader(a.get(), 30, 5, 0));
  ASSERT_TRUE(
      EPDFAnnot_SetLineCaption(a.get(), true, EPDF_CAPTION_INLINE, nullptr));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(a.get()));
  const auto ap = Appearance(a.get());
  EXPECT_NE(std::wstring::npos, ap.find(L"100 100 m 100 135 l S"));
  EXPECT_NE(std::wstring::npos, ap.find(L"BT"));
  EXPECT_NE(std::wstring::npos, ap.find(L"9 Tf"));
  EXPECT_NE(std::wstring::npos, ap.find(L"3.45"));
  EXPECT_EQ(std::wstring::npos,
            ap.find(L"100 130 m 300 130 l S"));  // inline gap
  FS_RECTF before, after;
  ASSERT_TRUE(FPDFAnnot_GetRect(a.get(), &before));
  EXPECT_GT(before.top, 135);
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(a.get()));
  ASSERT_TRUE(FPDFAnnot_GetRect(a.get(), &after));
  EXPECT_FLOAT_EQ(before.top, after.top);
  EXPECT_FLOAT_EQ(before.bottom, after.bottom);
  EXPECT_FALSE(CPDFDocumentFromFPDFDocument(document())
                   ->GetRoot()
                   ->KeyExist("AcroForm"));
  auto* dict = CPDFAnnotContextFromFPDFAnnotation(a.get())->GetAnnotDict();
  auto stream = dict->GetDictFor("AP")->GetStreamFor("N");
  auto fonts = stream->GetDict()->GetDictFor("Resources")->GetDictFor("Font");
  ASSERT_TRUE(fonts);
  EXPECT_TRUE(fonts->GetObjectFor("Helv")->IsDictionary());
  SetText(a.get(), "DA", L"/Helv 14 Tf 1 0 0 rg");
  EXPECT_TRUE(EPDFAnnot_GenerateAppearance(a.get()));
  EXPECT_NE(std::wstring::npos, Appearance(a.get()).find(L"14 Tf"));
  EXPECT_NE(std::wstring::npos, Appearance(a.get()).find(L"1 0 0 rg"));
}

TEST_F(EPDFMeasureEmbedderTest, ShortDistanceOmitsShaftAndBoundsCanShrink) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ScopedFPDFAnnotation annot(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINE));
  MakeLine(annot.get());
  const FS_POINTF start{100, 100};
  const FS_POINTF end{130, 100};
  ASSERT_TRUE(EPDFAnnot_SetLine(annot.get(), &start, &end));
  ASSERT_TRUE(EPDFAnnot_SetLineLeader(annot.get(), -15, 5, 0));
  ASSERT_TRUE(EPDFAnnot_SetLineEndings(annot.get(), FPDF_ANNOT_LE_ClosedArrow,
                                    FPDF_ANNOT_LE_ClosedArrow));
  ASSERT_TRUE(EPDFAnnot_SetLineCaption(annot.get(), true, EPDF_CAPTION_INLINE,
                                    nullptr));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));

  const auto appearance = Appearance(annot.get());
  EXPECT_EQ(std::wstring::npos, appearance.find(L"100 85 m 130 85 l S"));
  EXPECT_NE(std::wstring::npos, appearance.find(L"80 85 m 100 85 l S"));
  EXPECT_NE(std::wstring::npos, appearance.find(L"130 85 m 150 85 l S"));
  FS_RECTF original;
  ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &original));
  EXPECT_LT(original.left, 80);
  EXPECT_GT(original.right, 150);

  EPDF_CAPTION_OFFSET offset{100, -150};
  ASSERT_TRUE(EPDFAnnot_SetLineCaption(annot.get(), true, EPDF_CAPTION_INLINE,
                                    &offset));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  FS_RECTF expanded;
  ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &expanded));
  EXPECT_GT(expanded.right, original.right);
  EXPECT_LT(expanded.bottom, original.bottom);

  ASSERT_TRUE(EPDFAnnot_SetLineCaption(annot.get(), true, EPDF_CAPTION_INLINE,
                                    nullptr));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  FS_RECTF restored;
  ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &restored));
  EXPECT_FLOAT_EQ(original.left, restored.left);
  EXPECT_FLOAT_EQ(original.right, restored.right);
  EXPECT_FLOAT_EQ(original.bottom, restored.bottom);
  EXPECT_FLOAT_EQ(original.top, restored.top);
}

TEST_F(EPDFMeasureEmbedderTest,
       ShapeCaptionCenterRoundTripsAndOnlyChangesVisualBounds) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ScopedFPDFAnnotation a(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_POLYGON));
  FS_POINTF points[] = {{100, 100}, {200, 100}, {200, 200}, {100, 200}};
  FS_RECTF rect{99, 201, 201, 99};
  ASSERT_TRUE(FPDFAnnot_SetRect(a.get(), &rect));
  ASSERT_TRUE(EPDFAnnot_SetVertices(a.get(), points, 4));
  SetText(a.get(), "Contents", L"42 m²");
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(a.get()));
  EXPECT_EQ(std::wstring::npos, Appearance(a.get()).find(L"BT"));
  FS_POINTF center{340, 450};
  ASSERT_TRUE(EPDFAnnot_SetShapeCaption(a.get(), true, &center));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(a.get()));
  EXPECT_NE(std::wstring::npos, Appearance(a.get()).find(L"340 450 cm"));
  FS_RECTF before, after;
  ASSERT_TRUE(FPDFAnnot_GetRect(a.get(), &before));
  EXPECT_GT(before.top, 450);
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(a.get()));
  ASSERT_TRUE(FPDFAnnot_GetRect(a.get(), &after));
  EXPECT_FLOAT_EQ(before.top, after.top);
  auto vertices =
      CPDFAnnotContextFromFPDFAnnotation(a.get())->GetAnnotDict()->GetArrayFor(
          "Vertices");
  EXPECT_FLOAT_EQ(100, vertices->GetFloatAt(0));
  EXPECT_FLOAT_EQ(200, vertices->GetFloatAt(5));
  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  auto saved = OpenScopedSavedDocument();
  ASSERT_TRUE(saved);
  ScopedFPDFPage sp(FPDF_LoadPage(saved.get(), 0));
  ScopedFPDFAnnotation sa(FPDFPage_GetAnnot(sp.get(), 0));
  FPDF_BOOL enabled, present;
  FS_POINTF out;
  ASSERT_TRUE(EPDFAnnot_GetShapeCaption(sa.get(), &enabled, &present, &out));
  EXPECT_TRUE(enabled);
  EXPECT_TRUE(present);
  EXPECT_FLOAT_EQ(340, out.x);
  EXPECT_FLOAT_EQ(450, out.y);
}

TEST_F(EPDFMeasureEmbedderTest, ShapeCaptionBoundsShrinkWhenMovedInward) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ScopedFPDFAnnotation annot(
      EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_POLYGON));
  FS_POINTF points[] = {{100, 100}, {200, 100}, {200, 200}, {100, 200}};
  FS_RECTF rect{99, 201, 201, 99};
  ASSERT_TRUE(FPDFAnnot_SetRect(annot.get(), &rect));
  ASSERT_TRUE(EPDFAnnot_SetVertices(annot.get(), points, 4));
  SetText(annot.get(), "Contents", L"42 m²");
  FS_POINTF center{340, 450};
  ASSERT_TRUE(EPDFAnnot_SetShapeCaption(annot.get(), true, &center));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
  EXPECT_GT(rect.top, 450);

  ASSERT_TRUE(EPDFAnnot_SetShapeCaption(annot.get(), true, nullptr));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(annot.get()));
  ASSERT_TRUE(FPDFAnnot_GetRect(annot.get(), &rect));
  EXPECT_LT(rect.top, 210);
  EXPECT_LT(rect.right, 210);
  EXPECT_NE(std::wstring::npos, Appearance(annot.get()).find(L"150 150 cm"));
}

TEST_F(EPDFMeasureEmbedderTest, LayerAppearanceAndEphemeralRead) {
  embedpdf_test::LayerFixture layer;
  ASSERT_TRUE(layer.OpenBytes(Base()));
  ScopedFPDFPage page(FPDF_LoadPage(layer.layer, 0));
  ScopedFPDFAnnotation a(FPDFPage_GetAnnot(page.get(), 0));
  ASSERT_TRUE(
      EPDFAnnot_SetLineCaption(a.get(), true, EPDF_CAPTION_INLINE, nullptr));
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(a.get()));
  EXPECT_EQ(2u, EPDFLayer_GetPromotedObjectCount(
                    layer.layer));  // annotation and AP, no font/catalog/page
  auto delta = SaveDelta(layer.layer);
  ASSERT_TRUE(delta.changed);
  a.reset();
  page.reset();
  ASSERT_TRUE(layer.Reopen(std::move(delta.bytes)));
  page.reset(FPDF_LoadPage(layer.layer, 0));
  a.reset(FPDFPage_GetAnnot(page.get(), 0));
  EXPECT_NE(std::wstring::npos, Appearance(a.get()).find(L"3.45"));
  auto* doc = CPDFDocumentFromFPDFDocument(layer.layer);
  CPDF_DocumentViewScope scope(doc);
  const auto* dict =
      CPDFAnnotContextFromFPDFAnnotation(a.get())->GetAnnotDict();
  auto before = doc->GetLastObjNum();
  const auto loaded_count = EPDFLayer_GetPromotedObjectCount(layer.layer);
  auto generated = CPDF_GenerateAP::GenerateEphemeralAnnotAP(
      doc, dict, CPDF_Annot::Subtype::LINE);
  ASSERT_TRUE(generated);
  EXPECT_EQ(before, doc->GetLastObjNum());
  EXPECT_EQ(loaded_count, EPDFLayer_GetPromotedObjectCount(layer.layer));
}

TEST_F(EPDFMeasureEmbedderTest, ReadsAllAcrobatMetricMeasurements) {
  ASSERT_TRUE(OpenDocument("measure_acrobat_metric.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(document(), 0));
  int count = 0;
  for (int i = 0; i < FPDFPage_GetAnnotCount(page.get()); ++i) {
    ScopedFPDFAnnotation a(FPDFPage_GetAnnot(page.get(), i));
    auto m = EPDFAnnot_GetMeasure(a.get());
    if (!m) {
      continue;
    }
    ++count;
    EXPECT_EQ(EPDF_MEASURE_SUBTYPE_RL, EPDFMeasure_GetSubtype(m));
    EXPECT_EQ(L"1 cm = 1 m ", Ratio(m));
    EXPECT_EQ(1u, EPDFMeasure_CountFormats(m, kX));
    float factor;
    ASSERT_TRUE(EPDFNumberFormat_GetConversion(EPDFMeasure_GetFormat(m, kX, 0),
                                               &factor));
    EXPECT_NEAR(.0352778f, factor, 1e-8f);
    int precision;
    ASSERT_TRUE(EPDFNumberFormat_GetPrecision(EPDFMeasure_GetFormat(m, kD, 0),
                                              &precision));
    EXPECT_EQ(100, precision);
    auto format = EPDFMeasure_GetFormat(m, kX, 0);
    EXPECT_EQ(2u,
              EPDFNumberFormat_GetText(
                  format, EPDF_MEASURE_TEXT_THOUSANDS_SEPARATOR, nullptr, 0));
    EXPECT_EQ(0u, EPDFNumberFormat_GetText(
                      format, EPDF_MEASURE_TEXT_PREFIX_SPACING, nullptr, 0));
    if (FPDFAnnot_GetSubtype(a.get()) == FPDF_ANNOT_LINE) {
      FPDF_BOOL enabled;
      EPDF_CAPTION_POSITION position;
      EPDF_CAPTION_OFFSET offset;
      EXPECT_TRUE(
          EPDFAnnot_GetLineCaption(a.get(), &enabled, &position, &offset));
      EXPECT_TRUE(enabled);
      EXPECT_EQ(EPDF_CAPTION_INLINE, position);
      float ll, lle, llo;
      EXPECT_TRUE(EPDFAnnot_GetLineLeader(a.get(), &ll, &lle, &llo));
      EXPECT_FLOAT_EQ(5, lle);
    } else {
      FPDF_BOOL enabled, present;
      FS_POINTF center;
      EXPECT_TRUE(
          EPDFAnnot_GetShapeCaption(a.get(), &enabled, &present, &center));
      EXPECT_FALSE(enabled);
      EXPECT_FALSE(present);
    }
  }
  EXPECT_EQ(11, count);
  EXPECT_EQ(0u, EPDFPage_CountViewports(page.get()));
}

TEST_F(EPDFMeasureEmbedderTest, ReadsAllAcrobatImperialMeasurements) {
  ASSERT_TRUE(OpenDocument("measure_acrobat_imperial.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(document(), 0));
  int count = 0;
  for (int i = 0; i < FPDFPage_GetAnnotCount(page.get()); ++i) {
    ScopedFPDFAnnotation a(FPDFPage_GetAnnot(page.get(), i));
    auto m = EPDFAnnot_GetMeasure(a.get());
    if (!m) {
      continue;
    }
    ++count;
    EXPECT_EQ(L"1 in = 1 ft", Ratio(m));
    float factor;
    EXPECT_TRUE(EPDFNumberFormat_GetConversion(EPDFMeasure_GetFormat(m, kX, 0),
                                               &factor));
    EXPECT_NEAR(.0138889f, factor, 1e-8f);
  }
  EXPECT_EQ(4, count);
}

TEST_F(EPDFMeasureEmbedderTest,
       DirectMeasureWithSharedNestedFormatDetachesOnlyItsOwner) {
  auto bytes = Base();
  ScopedFPDFDocument doc(
      FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr));
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), 0));
  ScopedFPDFAnnotation a(FPDFPage_GetAnnot(page.get(), 0)),
      b(FPDFPage_GetAnnot(page.get(), 1));
  auto* document = CPDFDocumentFromFPDFDocument(doc.get());
  auto ma = CPDFAnnotContextFromFPDFAnnotation(a.get())
                ->GetMutableAnnotDict()
                ->GetMutableDictFor("Measure");
  auto mb = CPDFAnnotContextFromFPDFAnnotation(b.get())
                ->GetMutableAnnotDict()
                ->GetMutableDictFor("Measure");
  auto array = ma->GetMutableArrayFor("X");
  array->GetMutableDictAt(0)->SetNewFor<CPDF_String>("Vendor", "keep");
  array->ConvertToIndirectObjectAt(0, document);
  ma->ConvertToIndirectObjectFor("X", document);
  mb->SetFor("X", ma->GetObjectFor("X")->Clone());
  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(doc.get(), this, 0));
  embedpdf_test::LayerFixture layer;
  ASSERT_TRUE(layer.OpenBytes({GetString().begin(), GetString().end()}));
  ScopedFPDFPage lp(FPDF_LoadPage(layer.layer, 0));
  ScopedFPDFAnnotation la(FPDFPage_GetAnnot(lp.get(), 0)),
      lb(FPDFPage_GetAnnot(lp.get(), 1));
  auto fa = EPDFMeasure_GetFormat(EPDFAnnot_GetMeasure(la.get()), kX, 0);
  ASSERT_TRUE(EPDFNumberFormat_SetPrecision(fa, 1000));
  int precision;
  EXPECT_FALSE(EPDFNumberFormat_GetPrecision(
      EPDFMeasure_GetFormat(EPDFAnnot_GetMeasure(lb.get()), kX, 0),
      &precision));
  auto edited = CPDFAnnotContextFromFPDFAnnotation(la.get())
                    ->GetAnnotDict()
                    ->GetDictFor("Measure")
                    ->GetArrayFor("X")
                    ->GetDictAt(0);
  EXPECT_EQ("keep", edited->GetByteStringFor("Vendor"));
  EXPECT_EQ(1u, EPDFLayer_GetPromotedObjectCount(layer.layer));
}

TEST_F(EPDFMeasureEmbedderTest,
       SharedIndirectViewportTreeDetachesAndForeignEntriesSurvive) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ScopedFPDFPage other(FPDFPage_New(document(), 1, 612, 792));
  auto first = EPDFPage_AddViewport(page.get(), &kBox);
  ASSERT_TRUE(first);
  SetRatio(EPDFViewport_AddMeasure(first), L"foreign");
  auto* doc = CPDFDocumentFromFPDFDocument(document());
  auto pd = CPDFPageFromFPDFPage(page.get())->GetMutableDict();
  auto vp = pd->GetMutableArrayFor("VP");
  auto foreign = vp->GetMutableDictAt(0);
  foreign->SetNewFor<CPDF_String>("Vendor", "preserve byte-for-byte");
  auto measure = foreign->GetMutableDictFor("Measure");
  measure->SetNewFor<CPDF_Name>("Subtype", "GEO");
  foreign->SetNewFor<CPDF_Array>("PtData")->AppendNew<CPDF_Number>(42);
  foreign->ConvertToIndirectObjectFor("Measure", doc);
  vp->ConvertToIndirectObjectAt(0, doc);
  pd->ConvertToIndirectObjectFor("VP", doc);
  CPDFPageFromFPDFPage(other.get())
      ->GetMutableDict()
      ->SetFor("VP", pd->GetObjectFor("VP")->Clone());
  ClearString();
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  embedpdf_test::LayerFixture layer;
  ASSERT_TRUE(layer.OpenBytes({GetString().begin(), GetString().end()}));
  ScopedFPDFPage lp(FPDF_LoadPage(layer.layer, 0)),
      lo(FPDF_LoadPage(layer.layer, 1));
  ScopedFPDFPageView scope(lp.get());
  auto before = scope.Get()
                    ->GetDict()
                    ->GetArrayFor("VP")
                    ->GetDictAt(0)
                    ->CloneDirectObject();
  auto own = EPDFPage_AddViewport(lp.get(), &kBox);
  ASSERT_TRUE(own);
  EXPECT_TRUE(EPDFViewport_SetName(own, GetFPDFWideString(L"EmbedPDF").get()));
  SetRatio(EPDFViewport_AddMeasure(own), L"our scale");
  EXPECT_EQ(1u, EPDFPage_CountViewports(lo.get()));
  EXPECT_EQ(2u, EPDFPage_CountViewports(lp.get()));
  EXPECT_EQ(EPDF_MEASURE_SUBTYPE_GEO,
            EPDFMeasure_GetSubtype(
                EPDFViewport_GetMeasure(EPDFPage_GetViewport(lp.get(), 0))));
  auto after = scope.Get()
                   ->GetDict()
                   ->GetArrayFor("VP")
                   ->GetDictAt(0)
                   ->CloneDirectObject();
  EXPECT_TRUE(CPDF_SameEffectiveValue(before.Get(), after.Get()));
  EXPECT_EQ(1u, EPDFLayer_GetPromotedObjectCount(layer.layer));
  auto delta = SaveDelta(layer.layer);
  EXPECT_TRUE(delta.changed);
  EXPECT_TRUE(EPDFPage_RemoveViewport(lp.get(), 1));
  after = scope.Get()
              ->GetDict()
              ->GetArrayFor("VP")
              ->GetDictAt(0)
              ->CloneDirectObject();
  EXPECT_TRUE(CPDF_SameEffectiveValue(before.Get(), after.Get()));
}

TEST_F(EPDFMeasureEmbedderTest, MissingOrMalformedConversionNeverBecomesOne) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ScopedFPDFAnnotation a(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINE));
  auto m = EPDFAnnot_AddMeasure(a.get());
  ASSERT_TRUE(AddFormat(m));
  auto dict = CPDFAnnotContextFromFPDFAnnotation(a.get())
                  ->GetMutableAnnotDict()
                  ->GetMutableDictFor("Measure")
                  ->GetMutableArrayFor("X")
                  ->GetMutableDictAt(0);
  dict->RemoveFor("C");
  float factor = 7;
  auto f = EPDFMeasure_GetFormat(m, kX, 0);
  EXPECT_FALSE(EPDFNumberFormat_GetConversion(f, &factor));
  EXPECT_FLOAT_EQ(7, factor);
  dict->SetNewFor<CPDF_String>("C", "1");
  f = EPDFMeasure_GetFormat(m, kX, 0);
  EXPECT_FALSE(EPDFNumberFormat_GetConversion(f, &factor));
  EXPECT_FLOAT_EQ(7, factor);
}

TEST_F(EPDFMeasureEmbedderTest,
       LayerAddThenRemoveCaptionedMeasurementWritesNothing) {
  embedpdf_test::LayerFixture layer;
  ASSERT_TRUE(layer.OpenBytes(Base()));
  ScopedFPDFPage page(FPDF_LoadPage(layer.layer, 0));
  const int count = FPDFPage_GetAnnotCount(page.get());
  {
    ScopedFPDFAnnotation a(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINE));
    MakeLine(a.get());
    ASSERT_TRUE(AddFormat(EPDFAnnot_AddMeasure(a.get())));
    ASSERT_TRUE(
        EPDFAnnot_SetLineCaption(a.get(), true, EPDF_CAPTION_INLINE, nullptr));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(a.get()));
  }
  ASSERT_TRUE(FPDFPage_RemoveAnnot(page.get(), count));
  EXPECT_FALSE(SaveDelta(layer.layer).changed);
}

TEST_F(EPDFMeasureEmbedderTest, CaptionAppearanceGolden) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 500));
  const FS_POINTF start[] = {{60, 430},  {330, 430}, {60, 300},
                             {330, 300}, {60, 160},  {330, 160}};
  const FS_POINTF end[] = {{260, 430}, {530, 430}, {260, 340},
                           {360, 300}, {250, 160}, {470, 230}};
  const float leader[] = {25, -25, 20, -15, 0, 0};
  for (size_t i = 0; i < 6; ++i) {
    ScopedFPDFAnnotation a(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINE));
    FS_RECTF rect{start[i].x - 1, std::max(start[i].y, end[i].y) + 1,
                  end[i].x + 1, std::min(start[i].y, end[i].y) - 1};
    ASSERT_TRUE(FPDFAnnot_SetRect(a.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_SetLine(a.get(), &start[i], &end[i]));
    ASSERT_TRUE(FPDFAnnot_SetColor(a.get(), FPDFANNOT_COLORTYPE_Color, 220, 35,
                                   35, 255));
    ASSERT_TRUE(EPDFAnnot_SetLineEndings(a.get(), FPDF_ANNOT_LE_ClosedArrow,
                                         FPDF_ANNOT_LE_ClosedArrow));
    SetText(a.get(), "Contents", i == 3 ? L"0.32 m" : L"3.45 m");
    ASSERT_TRUE(EPDFAnnot_SetLineLeader(a.get(), leader[i], 5, 0));
    EPDF_CAPTION_OFFSET offset{20, 15};
    ASSERT_TRUE(EPDFAnnot_SetLineCaption(
        a.get(), true, i == 4 ? EPDF_CAPTION_TOP : EPDF_CAPTION_INLINE,
        i == 5 ? &offset : nullptr));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(a.get()));
  }
  for (int i = 0; i < 2; ++i) {
    ScopedFPDFAnnotation a(EPDFPage_CreateAnnot(
        page.get(), i ? FPDF_ANNOT_POLYLINE : FPDF_ANNOT_POLYGON));
    const float x = i ? 350 : 80;
    FS_POINTF points[] = {{x, 30}, {x + 120, 30}, {x + 100, 100}, {x, 100}};
    FS_RECTF rect{x - 1, 101, x + 121, 29};
    ASSERT_TRUE(FPDFAnnot_SetRect(a.get(), &rect));
    ASSERT_TRUE(EPDFAnnot_SetVertices(a.get(), points, 4));
    SetText(a.get(), "Contents", i ? L"14.85 m" : L"13.57 m²");
    FS_POINTF center{x + 150, 70};
    ASSERT_TRUE(
        EPDFAnnot_SetShapeCaption(a.get(), true, i ? &center : nullptr));
    ASSERT_TRUE(EPDFAnnot_GenerateAppearance(a.get()));
  }
  auto bitmap = RenderPageWithFlags(page.get(), nullptr, FPDF_ANNOT);
  ASSERT_TRUE(bitmap);
  CompareBitmapWithFuzzyExpectationSuffix(bitmap.get(), "measure_captions");
}

TEST_F(EPDFMeasureEmbedderTest,
       RegisteredCaptionFontsStayOutOfCatalogAndEphemeralWrites) {
  struct FontScope {
    FontScope() { EPDFFont_ClearRegisteredFonts(); }
    ~FontScope() { EPDFFont_ClearRegisteredFonts(); }
  } fonts;
  auto bytes =
      GetFileContents(PathService::GetThirdPartyFilePath(
                          "harfbuzz-ng/src/perf/fonts/Roboto-Regular.ttf")
                          .c_str());
  ASSERT_FALSE(bytes.empty());
  auto id = EPDFFont_RegisterMemFont64("Roboto", 400, false, bytes.data(),
                                       bytes.size());
  ASSERT_NE(0u, id);
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ScopedFPDFAnnotation a(EPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINE));
  MakeLine(a.get());
  auto* doc = CPDFDocumentFromFPDFDocument(document());
  ByteString alias;
  ASSERT_TRUE(CPDF_AnnotFontMap::ChooseRegisteredFontAlias(doc, id, &alias));
  doc->ReserveSessionFontAlias(alias, id);
  SetText(a.get(), "DA",
          (L"/" + std::wstring(alias.begin(), alias.end()) + L" 14 Tf 0 g")
              .c_str());
  ASSERT_TRUE(
      EPDFAnnot_SetLineCaption(a.get(), true, EPDF_CAPTION_INLINE, nullptr));
  const auto* dict =
      CPDFAnnotContextFromFPDFAnnotation(a.get())->GetAnnotDict();
  auto original = dict->Clone();
  auto objects = doc->GetLastObjNum();
  auto generated = CPDF_GenerateAP::GenerateEphemeralAnnotAP(
      doc, dict, CPDF_Annot::Subtype::LINE);
  ASSERT_TRUE(generated);
  ASSERT_TRUE(generated->font_lifetime);
  EXPECT_EQ(objects, doc->GetLastObjNum());
  EXPECT_TRUE(CPDF_SameEffectiveValue(dict, original.Get()));
  EXPECT_FALSE(doc->GetRoot()->KeyExist("AcroForm"));
  // Exercise the renderer's cached form and font-cache cleanup directly.
  // The page-level renderer also builds unrelated synthetic popup appearances.
  {
    CPDF_Annot render_annot(
        pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(dict)), doc);
    ASSERT_TRUE(render_annot.GetAPForm(CPDFPageFromFPDFPage(page.get()),
                                       CPDF_Annot::AppearanceMode::kNormal));
    render_annot.ClearCachedAP();
    ASSERT_TRUE(render_annot.GetAPForm(CPDFPageFromFPDFPage(page.get()),
                                       CPDF_Annot::AppearanceMode::kNormal));
  }
  EXPECT_EQ(objects, doc->GetLastObjNum());
  ASSERT_TRUE(EPDFAnnot_GenerateAppearance(a.get()));
  EXPECT_FALSE(doc->GetRoot()->KeyExist("AcroForm"));
  EXPECT_NE(std::wstring::npos, Appearance(a.get()).find(L"14 Tf"));
  auto normal = dict->GetDictFor("AP")->GetStreamFor("N");
  auto resource = normal->GetDict()
                      ->GetDictFor("Resources")
                      ->GetDictFor("Font")
                      ->GetDictFor(alias.AsStringView());
  ASSERT_TRUE(resource);
  EXPECT_EQ("Type0", resource->GetNameFor("Subtype"));
}
