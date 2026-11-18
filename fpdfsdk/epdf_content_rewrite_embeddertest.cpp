// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// A rewrite of content - FPDFPage_GenerateContent after a redaction or an
// edit inside a form, or an edited annotation appearance - writes the page
// again from its page objects. Whatever it did not change must render as it
// did: each test changes one thing, saves, reloads and compares the page
// with how it rendered before, everywhere outside the change. Every test runs
// on a document and on a layer in a transaction, as engine jobs do.
//
// Where a page's content is cut into streams must make no difference either:
// a reader treats the streams as one. See
// WhereContentIsCutIntoStreamsChangesNothing.

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "core/fpdfapi/font/cpdf_tounicodemap.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_streamparser.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/epdf_layer.h"
#include "public/epdf_redact.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdf_text.h"
#include "public/fpdfview.h"
#include "testing/document_sentinels.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

namespace {

// How far outside a changed area a pixel may differ: glyphs and
// anti-aliasing reach a little past the boxes text search reports.
constexpr float kSlack = 3.0f;

// The fixture pages with a case each (see redact_rewrite_cases.in).
enum CasePage : int {
  kSoftMaskUnderATransform = 0,
  kTextClip = 1,
  kMiterLimit = 2,
  kLineStateAfterAnExtGState = 3,
  kInlineImageNamingAColorSpace = 4,
  kImageNamingAColorSpace = 5,
  kSeparationNoneAndAll = 6,
  kFormBorrowingPageResources = 7,
  kFormInheritingAColorSpace = 8,
  kSharedFormTwoRegions = 9,
  kSharedFormOneRegion = 10,
  kTextScalingRiseAndSpacing = 11,
  kHiddenOptionalContent = 12,
  kRotatedFormPathAndImage = 13,
  kType3FontBorrowingPageResources = 14,
  kAppearanceBorrowingPageResources = 15,
  kDefaultColorSpace = 16,
  kAppearanceInASpotColor = 17,
  kTextClipAcrossTextObjects = 18,
  kMirroredSkewedForm = 19,
  kNestedAppearancesBorrowingPageResources = 20,
  kType3GlyphFormBorrowingPageResources = 21,
  kICCBasedDefaultsAndCMYKPattern = 22,
  kRedactionInHiddenOptionalContent = 23,
  kNameForTwoResourceTypes = 24,
  kFormReadingOtherResourcesInEachPlacement = 25,
  kAllTextInAType3FontRedacted = 26,
  kType3FontBorrowedByAnAppearance = 27,
  kType3FontPartlyRedacted = 28,
  kType3GlyphsUsedElsewhere = 29,
  kType3FontWithResourcesPartlyRedacted = 30,
  kFormInheritingTextState = 31,
  kFormSettingSpacingBack = 32,
  kType3GlyphNamesAndText = 33,
  kAppearancesSharedWithAnotherPage = 34,
  kPageSharingAppearances = 35,
  kType3GlyphDrawingAnotherFont = 36,
  kType3GlyphDrawnByARedactedGlyph = 37,
  kType3GlyphBorrowingAFont = 38,
  kType3GlyphDrawingItsOwnFont = 39,
  kFormNamingAType3FontItNeverShows = 40,
  kType3GlyphSoftMaskDrawingAFont = 41,
  kSoftMaskDrawingAType3Font = 42,
  kTilingPatternDrawingAType3Font = 43,
  kType3FontInTwoContextsInAGlyph = 44,
  kType3FontInheritedPastAnUnusedAlias = 45,
  kType3FontInTwoContextsSharingItsName = 46,
  kType3FontNamingAnotherUnselected = 47,
  kType3FontSelectingAnotherWithoutText = 48,
  kType3FontDrawnByARedactedGlyph = 49,
  kType3FontSelectedAgainByAForm = 50,
};

// The fixture pages of redact_split_content_streams.in.
enum SplitStreamsPage : int {
  kCutAsPrintToPDFDoes = 0,
  kCutInsideATransformWithoutAnInverse = 1,
};

// How CutIntoStreams() cuts a page's content.
enum class Cut {
  kIntoThree,
  kAfterEveryOperator,
};

// The page on white at a pixel per point, its annotations included.
ScopedFPDFBitmap RenderAtPageSize(FPDF_PAGE page) {
  const int width = static_cast<int>(FPDF_GetPageWidthF(page));
  const int height = static_cast<int>(FPDF_GetPageHeightF(page));
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(width, height, 0));
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xffffffff);
  FPDF_RenderPageBitmap(bitmap.get(), page, 0, 0, width, height, 0, FPDF_ANNOT);
  return bitmap;
}

// The colour at page point (`x`, `y`) of a RenderAtPageSize() bitmap.
uint32_t PixelAt(FPDF_BITMAP bitmap, float x, float y) {
  const int row = FPDFBitmap_GetHeight(bitmap) - static_cast<int>(y);
  const auto* data = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
  const uint8_t* pixel =
      data + row * FPDFBitmap_GetStride(bitmap) + static_cast<int>(x) * 4;
  return (uint32_t{pixel[2]} << 16) | (uint32_t{pixel[1]} << 8) | pixel[0];
}

bool Contains(const FS_RECTF& area, float x, float y, float slack) {
  return x > area.left - slack && x < area.right + slack &&
         y > area.bottom - slack && y < area.top + slack;
}

// How many pixels differ between two renders of a page outside `areas`;
// `where` gets the page-space box around them.
int CountChangedPixelsOutside(FPDF_BITMAP before,
                              FPDF_BITMAP after,
                              const std::vector<FS_RECTF>& areas,
                              std::string* where = nullptr) {
  float min_x = 1e9f;
  float min_y = 1e9f;
  float max_x = -1e9f;
  float max_y = -1e9f;
  const int width = FPDFBitmap_GetWidth(before);
  const int height = FPDFBitmap_GetHeight(before);
  EXPECT_EQ(width, FPDFBitmap_GetWidth(after));
  EXPECT_EQ(height, FPDFBitmap_GetHeight(after));
  const auto* a = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(before));
  const auto* b = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(after));
  const int stride = FPDFBitmap_GetStride(before);
  int changed = 0;
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      const float x = column + 0.5f;
      const float y = height - row - 0.5f;
      bool inside = false;
      for (const FS_RECTF& area : areas) {
        inside = inside || Contains(area, x, y, kSlack);
      }
      if (inside) {
        continue;
      }
      const int offset = row * stride + column * 4;
      for (int channel = 0; channel < 3; ++channel) {
        if (std::abs(a[offset + channel] - b[offset + channel]) > 8) {
          ++changed;
          min_x = std::min(min_x, x);
          min_y = std::min(min_y, y);
          max_x = std::max(max_x, x);
          max_y = std::max(max_y, y);
          break;
        }
      }
    }
  }
  if (where && changed) {
    *where = "changed pixels in [" + std::to_string(min_x) + " " +
             std::to_string(min_y) + " " + std::to_string(max_x) + " " +
             std::to_string(max_y) + "] (left bottom right top)";
  }
  return changed;
}

// How many pixels inside `areas` (less the slack at their edges) are drawn
// on: none, once what was there is redacted without an overlay.
int CountInkInside(FPDF_BITMAP bitmap, const std::vector<FS_RECTF>& areas) {
  const int width = FPDFBitmap_GetWidth(bitmap);
  const int height = FPDFBitmap_GetHeight(bitmap);
  const auto* data = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
  const int stride = FPDFBitmap_GetStride(bitmap);
  int ink = 0;
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      const float x = column + 0.5f;
      const float y = height - row - 0.5f;
      bool inside = false;
      for (const FS_RECTF& area : areas) {
        inside = inside || Contains(area, x, y, -kSlack);
      }
      const int offset = row * stride + column * 4;
      if (inside && (data[offset] < 240 || data[offset + 1] < 240 ||
                     data[offset + 2] < 240)) {
        ++ink;
      }
    }
  }
  return ink;
}

// `areas`, each grown by `by` on every side: large glyphs reach past the box
// text search gives them.
std::vector<FS_RECTF> Grown(std::vector<FS_RECTF> areas, float by) {
  for (FS_RECTF& area : areas) {
    area = {area.left - by, area.top + by, area.right + by, area.bottom - by};
  }
  return areas;
}

// The boxes text search finds `word` in on `page`, in page space; one box a
// match on one line.
std::vector<FS_RECTF> AreasOf(FPDF_PAGE page, const std::wstring& word) {
  std::vector<FS_RECTF> areas;
  ScopedFPDFTextPage text(FPDFText_LoadPage(page));
  ScopedFPDFWideString needle = GetFPDFWideString(word);
  ScopedFPDFTextFind find(FPDFText_FindStart(text.get(), needle.get(), 0, 0));
  while (FPDFText_FindNext(find.get())) {
    const int count =
        FPDFText_CountRects(text.get(), FPDFText_GetSchResultIndex(find.get()),
                            FPDFText_GetSchCount(find.get()));
    for (int i = 0; i < count; ++i) {
      double left;
      double top;
      double right;
      double bottom;
      FPDFText_GetRect(text.get(), i, &left, &top, &right, &bottom);
      areas.push_back(
          FS_RECTF{static_cast<float>(left), static_cast<float>(top),
                   static_cast<float>(right), static_cast<float>(bottom)});
    }
  }
  return areas;
}

// The characters on `page` whose box centre lies inside one of `areas`.
std::wstring TextInside(FPDF_PAGE page, const std::vector<FS_RECTF>& areas) {
  std::wstring found;
  ScopedFPDFTextPage text(FPDFText_LoadPage(page));
  for (int i = 0; i < FPDFText_CountChars(text.get()); ++i) {
    const unsigned int unicode = FPDFText_GetUnicode(text.get(), i);
    double left;
    double right;
    double bottom;
    double top;
    if (unicode <= ' ' ||
        !FPDFText_GetCharBox(text.get(), i, &left, &right, &bottom, &top)) {
      continue;
    }
    const float x = static_cast<float>((left + right) / 2);
    const float y = static_cast<float>((bottom + top) / 2);
    for (const FS_RECTF& area : areas) {
      if (Contains(area, x, y, 0)) {
        found += static_cast<wchar_t>(unicode);
        break;
      }
    }
  }
  return found;
}

// A character `page` shows, and its origin in page space.
struct PlacedChar {
  unsigned int unicode;
  double x;
  double y;
};

// The characters `page` shows whose box centre lies outside `areas`.
std::vector<PlacedChar> CharsOutside(FPDF_PAGE page,
                                     const std::vector<FS_RECTF>& areas) {
  std::vector<PlacedChar> chars;
  ScopedFPDFTextPage text(FPDFText_LoadPage(page));
  for (int i = 0; i < FPDFText_CountChars(text.get()); ++i) {
    PlacedChar placed = {FPDFText_GetUnicode(text.get(), i), 0, 0};
    double left;
    double right;
    double bottom;
    double top;
    if (placed.unicode <= ' ' ||
        !FPDFText_GetCharBox(text.get(), i, &left, &right, &bottom, &top) ||
        !FPDFText_GetCharOrigin(text.get(), i, &placed.x, &placed.y)) {
      continue;
    }
    const float x = static_cast<float>((left + right) / 2);
    const float y = static_cast<float>((bottom + top) / 2);
    bool inside = false;
    for (const FS_RECTF& area : areas) {
      inside = inside || Contains(area, x, y, 0);
    }
    if (!inside) {
      chars.push_back(placed);
    }
  }
  return chars;
}

// The decoded content of each content stream of page `index` of `doc`.
std::vector<std::string> PageContentStreams(FPDF_DOCUMENT doc, int index) {
  CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(doc);
  RetainPtr<const CPDF_Object> contents =
      pdf->GetPageDictionary(index)->GetDirectObjectFor("Contents");
  std::vector<RetainPtr<const CPDF_Stream>> streams;
  if (const CPDF_Array* array = contents ? contents->AsArray() : nullptr) {
    for (size_t i = 0; i < array->size(); ++i) {
      streams.push_back(ToStream(array->GetDirectObjectAt(i)));
    }
  } else if (contents && contents->IsStream()) {
    streams.push_back(pdfium::WrapRetain(contents->AsStream()));
  }
  std::vector<std::string> content;
  for (const auto& stream : streams) {
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(stream);
    acc->LoadAllDataFiltered();
    const auto bytes = acc->GetSpan();
    content.emplace_back(bytes.begin(), bytes.end());
  }
  return content;
}

// The decoded content of page `index` of `doc`.
std::string PageContent(FPDF_DOCUMENT doc, int index) {
  std::string content;
  for (const std::string& stream : PageContentStreams(doc, index)) {
    content += stream;
  }
  return content;
}

// Calls `visit` with each operator of `content` and the offset just after
// it. An inline image (BI ... EI) counts as one operator, EI: its data is no
// tokens.
void ForEachOperator(const std::string& content,
                     const std::function<void(ByteStringView, size_t)>& visit) {
  CPDF_StreamParser parser(pdfium::as_byte_span(content));
  bool in_image = false;
  while (true) {
    const CPDF_StreamParser::ElementType type = parser.ParseNextElement();
    if (type == CPDF_StreamParser::ElementType::kEndOfData) {
      return;
    }
    if (type != CPDF_StreamParser::ElementType::kKeyword) {
      continue;
    }
    const ByteStringView op = parser.GetWord();
    if (op == "BI") {
      in_image = true;
    } else if (op == "EI") {
      in_image = false;
    }
    if (!in_image) {
      visit(op, parser.GetPos());
    }
  }
}

// Expects each content stream of page `index` of `doc` to stand alone: it
// restores what it saves (q ... Q) and sets no CTM outside of that. Such a
// stream leaves the one after it nothing, so where one stream ends and the
// next begins has no effect.
void ExpectEachStreamStandsAlone(FPDF_DOCUMENT doc, int index) {
  for (const std::string& content : PageContentStreams(doc, index)) {
    int depth = 0;
    ForEachOperator(content, [&](ByteStringView op, size_t) {
      if (op == "q") {
        ++depth;
      } else if (op == "Q") {
        --depth;
        EXPECT_GE(depth, 0)
            << "a Q without its q on page " << index << ": " << content;
      } else if (op == "cm") {
        EXPECT_GT(depth, 0)
            << "a cm outside q ... Q on page " << index << ": " << content;
      }
    });
    EXPECT_EQ(0, depth) << "a q without its Q on page " << index << ": "
                        << content;
  }
}

}  // namespace

// The parameter: true to work on a layer in a transaction, false on a
// document.
class EPDFContentRewriteEmbedderTest
    : public EmbedderTest,
      public testing::WithParamInterface<bool> {
 protected:
  void TearDown() override {
    doc_.reset();
    base_.reset();
    EmbedderTest::TearDown();
  }

  bool layer() const { return GetParam(); }

  // Opens the test file `name`, as a layer of it when the parameter says so.
  FPDF_DOCUMENT Open(const char* name) {
    const std::string path = PathService::GetTestFilePath(name);
    return OpenBytes(GetFileContents(path.c_str()));
  }

  // Opens the file `bytes` hold, as Open() does, in place of the open one.
  FPDF_DOCUMENT OpenBytes(std::vector<uint8_t> bytes) {
    saved_.reset();
    doc_.reset();
    base_.reset();
    bytes_ = std::move(bytes);
    if (layer()) {
      base_.reset(
          EPDF_LoadMemBaseDocument64(bytes_.data(), bytes_.size(), nullptr));
      if (!base_) {
        return nullptr;
      }
      doc_.reset(EPDFLayer_OpenLayer(base_.get(), nullptr, nullptr, nullptr));
    } else {
      // Plain in layer mode too: the layer is the other case.
      doc_.reset(FPDF_LoadMemDocument64(bytes_.data(), bytes_.size(), nullptr));
    }
    return doc_.get();
  }

  // Runs `change`, in a transaction on a layer.
  void Change(const std::function<void()>& change) {
    if (layer()) {
      ASSERT_TRUE(EPDFLayer_BeginTransaction(doc_.get()));
    }
    change();
    if (HasFatalFailure()) {
      return;
    }
    if (layer()) {
      ASSERT_TRUE(EPDFLayer_CommitTransaction(doc_.get()));
    }
  }

  // Saves the document in full and opens the saved copy, until
  // CloseSaved().
  void SaveAndReopen() {
    ClearString();
    ASSERT_TRUE(FPDF_SaveAsCopy(doc_.get(), this, FPDF_NO_INCREMENTAL));
    saved_.reset();
    saved_.emplace(OpenScopedSavedDocument());
    ASSERT_TRUE(*saved_);
  }

  void CloseSaved() { saved_.reset(); }

  void ChangeAndSave(const std::function<void()>& change) {
    Change(change);
    if (HasFatalFailure()) {
      return;
    }
    SaveAndReopen();
  }

  // A redaction mark over each of `areas`, without an overlay.
  static void Mark(FPDF_PAGE page, const std::vector<FS_RECTF>& areas) {
    for (const FS_RECTF& area : areas) {
      ScopedFPDFAnnotation mark(FPDFPage_CreateAnnot(page, FPDF_ANNOT_REDACT));
      ASSERT_TRUE(FPDFAnnot_SetRect(mark.get(), &area));
    }
  }

  void ApplyRedactions(FPDF_PAGE page) {
    ASSERT_TRUE(EPDFPage_ApplyRedactions(page, nullptr, 0, nullptr));
    ASSERT_TRUE(FPDFPage_GenerateContent(page));
  }

  // Checks page `index` of the saved copy against `before`: unchanged
  // outside `areas`, nothing drawn and no character left inside them.
  // `inspect` may look at the saved document and page further.
  void ExpectSavedPageChangedOnlyIn(
      int index,
      FPDF_BITMAP before,
      const std::vector<FS_RECTF>& areas,
      const std::function<void(FPDF_DOCUMENT, FPDF_PAGE)>& inspect = {}) {
    {
      ScopedSavedPage saved = LoadScopedSavedPage(index);
      ASSERT_TRUE(saved);
      ScopedFPDFBitmap after = RenderAtPageSize(saved.get());
      std::string where;
      EXPECT_EQ(0,
                CountChangedPixelsOutside(before, after.get(), areas, &where))
          << where;
      EXPECT_EQ(0, CountInkInside(after.get(), areas));
      EXPECT_EQ(L"", TextInside(saved.get(), areas));
      if (inspect) {
        inspect(saved_document(), saved.get());
      }
    }
    CloseSaved();
  }

  // Redacts `areas_of(page)` on page `index` of the test file `name` in one
  // apply, saves, and checks the saved page (ExpectSavedPageChangedOnlyIn()).
  void ExpectRedactionChangesOnlyItsAreas(
      const char* name,
      int index,
      const std::function<std::vector<FS_RECTF>(FPDF_PAGE)>& areas_of,
      const std::function<void(FPDF_DOCUMENT, FPDF_PAGE)>& inspect = {}) {
    ASSERT_TRUE(Open(name));
    ScopedFPDFPage page(FPDF_LoadPage(doc_.get(), index));
    ASSERT_TRUE(page);
    ScopedFPDFBitmap before = RenderAtPageSize(page.get());
    const std::vector<FS_RECTF> areas = areas_of(page.get());
    ASSERT_FALSE(areas.empty());
    Mark(page.get(), areas);
    ChangeAndSave([&] { ApplyRedactions(page.get()); });
    if (HasFatalFailure()) {
      return;
    }
    ExpectSavedPageChangedOnlyIn(index, before.get(), areas, inspect);
  }

  void ExpectRedactingWordChangesOnlyIt(
      const char* name,
      int index,
      const std::wstring& word,
      const std::function<void(FPDF_DOCUMENT, FPDF_PAGE)>& inspect = {}) {
    ExpectRedactionChangesOnlyItsAreas(
        name, index, [&](FPDF_PAGE page) { return AreasOf(page, word); },
        inspect);
  }

  void ExpectRedactingACaseChangesOnlyIt(
      CasePage index,
      const std::function<void(FPDF_DOCUMENT, FPDF_PAGE)>& inspect = {}) {
    ExpectRedactingWordChangesOnlyIt("redact_rewrite_cases.pdf", index,
                                     L"REDACTME", inspect);
  }

  // A copy of the test file `name` with every page's content cut into
  // streams: after the operators at a third and at two thirds, or after every
  // operator, wherever that falls - inside q ... Q, a path, a text object. A
  // reader treats a page's streams as one, so the copy draws what the file
  // draws.
  std::vector<uint8_t> CutIntoStreams(const char* name, Cut cut) {
    const std::string path = PathService::GetTestFilePath(name);
    const std::vector<uint8_t> bytes = GetFileContents(path.c_str());
    ScopedFPDFDocument source(
        FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr));
    EXPECT_TRUE(source);
    if (!source) {
      return {};
    }
    CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(source.get());
    for (int i = 0; i < FPDF_GetPageCount(source.get()); ++i) {
      std::string content;
      for (const std::string& stream : PageContentStreams(source.get(), i)) {
        content += stream + "\n";
      }
      std::vector<size_t> ends;
      ForEachOperator(content,
                      [&](ByteStringView, size_t end) { ends.push_back(end); });
      if (ends.size() < 3) {
        continue;
      }
      if (cut == Cut::kIntoThree) {
        ends = {ends[ends.size() / 3 - 1], ends[2 * ends.size() / 3 - 1],
                ends.back()};
      }
      auto streams = pdfium::MakeRetain<CPDF_Array>();
      size_t begin = 0;
      for (size_t end : ends) {
        auto stream = pdf->NewIndirect<CPDF_Stream>(
            pdfium::as_byte_span(content).subspan(begin, end - begin));
        streams->AppendNew<CPDF_Reference>(pdf, stream->GetObjNum());
        begin = end;
      }
      pdf->GetMutablePageDictionary(i)->SetFor("Contents", std::move(streams));
    }
    ClearString();
    EXPECT_TRUE(FPDF_SaveAsCopy(source.get(), this, FPDF_NO_INCREMENTAL));
    const std::string& saved = GetString();
    return std::vector<uint8_t>(saved.begin(), saved.end());
  }

  // Opens the file `bytes` hold and writes every page's content again in one
  // change: a page showing REDACTME, SECRET or Jane Doe has it redacted, any
  // other has its first object marked as changed. Gives each page's render
  // from the saved copy, and in `before` each page's render before the
  // change. Expects the content written to stand alone
  // (ExpectEachStreamStandsAlone()).
  std::vector<ScopedFPDFBitmap> RewriteEveryPage(
      std::vector<uint8_t> bytes,
      std::vector<ScopedFPDFBitmap>* before) {
    std::vector<ScopedFPDFBitmap> after;
    if (!OpenBytes(std::move(bytes))) {
      ADD_FAILURE() << "the file does not open";
      return after;
    }
    const int count = FPDF_GetPageCount(doc_.get());
    std::vector<ScopedFPDFPage> pages;
    std::vector<bool> redacts;
    for (int i = 0; i < count; ++i) {
      pages.emplace_back(FPDF_LoadPage(doc_.get(), i));
      FPDF_PAGE page = pages.back().get();
      EXPECT_TRUE(page) << i;
      if (!page) {
        return after;
      }
      before->push_back(RenderAtPageSize(page));
      std::vector<FS_RECTF> areas;
      for (const wchar_t* word : {L"REDACTME", L"SECRET", L"Jane Doe"}) {
        if (areas.empty()) {
          areas = AreasOf(page, word);
        }
      }
      Mark(page, areas);
      redacts.push_back(!areas.empty());
    }
    std::vector<bool> written(count);
    ChangeAndSave([&] {
      for (int i = 0; i < count; ++i) {
        FPDF_PAGE page = pages[i].get();
        if (redacts[i]) {
          ApplyRedactions(page);
          written[i] = true;
        } else if (FPDFPage_CountObjects(page) > 0) {
          CPDFPageObjectFromFPDFPageObject(FPDFPage_GetObject(page, 0))
              ->SetDirty(true);
          ASSERT_TRUE(FPDFPage_GenerateContent(page));
          written[i] = true;
        }
      }
    });
    if (HasFatalFailure()) {
      return after;
    }
    for (int i = 0; i < count; ++i) {
      ScopedSavedPage saved = LoadScopedSavedPage(i);
      EXPECT_TRUE(saved) << i;
      if (!saved) {
        return after;
      }
      after.push_back(RenderAtPageSize(saved.get()));
      if (written[i]) {
        ExpectEachStreamStandsAlone(saved_document(), i);
      }
    }
    CloseSaved();
    return after;
  }

  // Page `index` of the open document, rendered (RenderAtPageSize()).
  ScopedFPDFBitmap RenderOtherPage(int index) {
    ScopedFPDFPage page(FPDF_LoadPage(doc_.get(), index));
    EXPECT_TRUE(page);
    return page ? RenderAtPageSize(page.get()) : nullptr;
  }

  // Expects page `index` of `saved` to draw as `before`: changing another
  // page took nothing it draws.
  static void ExpectOtherPageUnchanged(FPDF_DOCUMENT saved,
                                       int index,
                                       FPDF_BITMAP before) {
    ASSERT_TRUE(before);
    ScopedFPDFPage page(FPDF_LoadPage(saved, index));
    ASSERT_TRUE(page);
    ScopedFPDFBitmap after = RenderAtPageSize(page.get());
    std::string where;
    EXPECT_EQ(0, CountChangedPixelsOutside(before, after.get(), {}, &where))
        << where;
  }

  ScopedFPDFDocument doc_;
  // The saved copy SaveAndReopen() opened, until CloseSaved().
  std::optional<ScopedSavedDoc> saved_;

 private:
  std::vector<uint8_t> bytes_;
  std::unique_ptr<std::remove_pointer_t<EPDF_BASE_DOCUMENT>,
                  decltype(&EPDF_ReleaseBaseDocument)>
      base_{nullptr, EPDF_ReleaseBaseDocument};
};

INSTANTIATE_TEST_SUITE_P(All,
                         EPDFContentRewriteEmbedderTest,
                         testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) {
                           return info.param ? "LayerTransaction" : "Document";
                         });

// Colours in every colour space keep their colour: Separation (its array
// direct and by reference) for fills, strokes, text and an image mask,
// DeviceN, Indexed, Lab, CalRGB, CalGray and an uncoloured tiling pattern
// (embedpdf/runtime#58). The content names them as it did.
TEST_P(EPDFContentRewriteEmbedderTest, KeepsEveryColorSpace) {
  ExpectRedactingWordChangesOnlyIt(
      "redact_special_colorspaces.pdf", 0, L"Jane Doe",
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        const std::string content = PageContent(saved, 0);
        EXPECT_NE(std::string::npos, content.find("/CSsep cs 1 scn"))
            << content;
        EXPECT_NE(std::string::npos, content.find("/CSsep CS 1 SCN"))
            << content;
        EXPECT_NE(std::string::npos, content.find("/CSdn cs .3 1 scn"))
            << content;
        EXPECT_NE(std::string::npos, content.find("/CSpat cs 0 .6 0 /"))
            << content;
      });
}

TEST_P(EPDFContentRewriteEmbedderTest, KeepsSeparationNoneAndAll) {
  ExpectRedactingACaseChangesOnlyIt(kSeparationNoneAndAll);
}

// The form paints with a bare scn in the colour space the page selected
// before drawing it.
TEST_P(EPDFContentRewriteEmbedderTest, KeepsAColorSpaceAFormInherits) {
  ExpectRedactingACaseChangesOnlyIt(kFormInheritingAColorSpace);
}

// No object names /DefaultRGB, but device RGB colours are drawn in it.
TEST_P(EPDFContentRewriteEmbedderTest, KeepsDefaultColorSpaces) {
  ExpectRedactingACaseChangesOnlyIt(
      kDefaultColorSpace, [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        RetainPtr<const CPDF_Dictionary> spaces =
            CPDFDocumentFromFPDFDocument(saved)
                ->GetPageDictionary(kDefaultColorSpace)
                ->GetDictFor("Resources")
                ->GetDictFor("ColorSpace");
        ASSERT_TRUE(spaces);
        EXPECT_TRUE(spaces->KeyExist("DefaultRGB"));
      });
}

// Editing one object of an appearance writes the appearance again: its
// other objects keep their spot colour.
TEST_P(EPDFContentRewriteEmbedderTest, EditingAnAppearanceKeepsItsSpotColor) {
  ASSERT_TRUE(Open("redact_rewrite_cases.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(doc_.get(), kAppearanceInASpotColor));
  ASSERT_TRUE(page);
  ScopedFPDFBitmap before = RenderAtPageSize(page.get());
  // The appearance's blue bar, at the top of the stamp.
  const FS_RECTF bar = {100, 550, 400, 520};
  ChangeAndSave([&] {
    ScopedFPDFAnnotation stamp(FPDFPage_GetAnnot(page.get(), 0));
    ASSERT_TRUE(stamp);
    FPDF_PAGEOBJECT object = FPDFAnnot_GetObject(stamp.get(), 1);
    ASSERT_TRUE(object);
    ASSERT_TRUE(FPDFPageObj_SetFillColor(object, 255, 0, 0, 255));
    ASSERT_TRUE(FPDFAnnot_UpdateObject(stamp.get(), object));
  });

  {
    ScopedSavedPage saved = LoadScopedSavedPage(kAppearanceInASpotColor);
    ASSERT_TRUE(saved);
    ScopedFPDFBitmap after = RenderAtPageSize(saved.get());
    EXPECT_EQ(0xff0000u, PixelAt(after.get(), 250, 535));
    EXPECT_EQ(0, CountChangedPixelsOutside(before.get(), after.get(), {bar}));
  }
  CloseSaved();
}

// A form with a /Matrix: its objects carry the matrix, so the content written
// for them must not get it a second time (embedpdf/runtime#59).
TEST_P(EPDFContentRewriteEmbedderTest,
       KeepsFormContentInPlaceUnderATranslatingMatrix) {
  ExpectRedactingWordChangesOnlyIt("edit_form_with_matrix.pdf", 0, L"SECRET");
}

TEST_P(EPDFContentRewriteEmbedderTest,
       KeepsFormContentInPlaceUnderARotatingMatrix) {
  ExpectRedactingWordChangesOnlyIt("edit_form_with_matrix.pdf", 1, L"SECRET");
}

TEST_P(EPDFContentRewriteEmbedderTest,
       KeepsFormContentInPlaceInNestedFormsWithMatrices) {
  ExpectRedactingWordChangesOnlyIt("edit_form_with_matrix.pdf", 2, L"SECRET");
}

// Text, a path and an image in a rotated form, each partly redacted.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactsPathAndImageInPlaceInsideARotatedForm) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kRotatedFormPathAndImage, [](FPDF_PAGE page) {
        std::vector<FS_RECTF> areas = AreasOf(page, L"REDACTME");
        areas.push_back(FS_RECTF{350, 340, 390, 300});  // the green bar
        areas.push_back(FS_RECTF{400, 340, 460, 300});  // the image
        return areas;
      });
}

TEST_P(EPDFContentRewriteEmbedderTest,
       RemovingAnObjectFromAFormKeepsTheRestInPlace) {
  ASSERT_TRUE(Open("edit_form_with_matrix.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(doc_.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFBitmap before = RenderAtPageSize(page.get());
  // The form's underline: 20 290 180 2 re, moved by the form's /Matrix.
  const FS_RECTF underline = {77, 334, 257, 332};
  ChangeAndSave([&] {
    FPDF_PAGEOBJECT form = FPDFPage_GetObject(page.get(), 0);
    ASSERT_EQ(2, FPDFFormObj_CountObjects(form));
    FPDF_PAGEOBJECT path = FPDFFormObj_GetObject(form, 1);
    ASSERT_EQ(FPDF_PAGEOBJ_PATH, FPDFPageObj_GetType(path));
    ASSERT_TRUE(FPDFFormObj_RemoveObject(form, path));
    // The caller owns what it removed.
    ScopedFPDFPageObject removed(path);
    ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  });

  {
    ScopedSavedPage saved = LoadScopedSavedPage(0);
    ASSERT_TRUE(saved);
    ScopedFPDFBitmap after = RenderAtPageSize(saved.get());
    EXPECT_EQ(0xffffffu, PixelAt(after.get(), 150, 333));
    EXPECT_EQ(
        0, CountChangedPixelsOutside(before.get(), after.get(), {underline}));
  }
  CloseSaved();
}

// A soft mask set under a transform is placed by it.
TEST_P(EPDFContentRewriteEmbedderTest, KeepsASoftMaskSetUnderATransform) {
  ExpectRedactingACaseChangesOnlyIt(kSoftMaskUnderATransform);
}

// Text in render mode 7 clips the rectangle drawn after it.
TEST_P(EPDFContentRewriteEmbedderTest, KeepsTextClipping) {
  ExpectRedactingACaseChangesOnlyIt(kTextClip);
}

// PDFium draws a miter join the same way with any miter limit; other viewers
// don't, so the content must keep it.
TEST_P(EPDFContentRewriteEmbedderTest, KeepsTheMiterLimit) {
  ExpectRedactingACaseChangesOnlyIt(
      kMiterLimit, [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        const std::string content = PageContent(saved, kMiterLimit);
        EXPECT_NE(std::string::npos, content.find(" 1.5 M ")) << content;
      });
}

// The ExtGState sets a 12-point dashed line; the content then sets a 2-point
// solid one.
TEST_P(EPDFContentRewriteEmbedderTest, LineStateSetAfterAnExtGStateWins) {
  ExpectRedactingACaseChangesOnlyIt(kLineStateAfterAnExtGState);
}

TEST_P(EPDFContentRewriteEmbedderTest, KeepsAnInlineImageNamingAColorSpace) {
  ExpectRedactingACaseChangesOnlyIt(kInlineImageNamingAColorSpace);
}

TEST_P(EPDFContentRewriteEmbedderTest, KeepsAColorSpaceAnImageNames) {
  ExpectRedactingACaseChangesOnlyIt(kImageNamingAColorSpace);
}

// A form without /Resources draws with the page's font and colour space.
TEST_P(EPDFContentRewriteEmbedderTest, KeepsResourcesAFormBorrows) {
  ExpectRedactingACaseChangesOnlyIt(kFormBorrowingPageResources);
}

// A Type3 font without /Resources draws its glyph in the page's colour space.
TEST_P(EPDFContentRewriteEmbedderTest, KeepsResourcesAType3FontBorrows) {
  ExpectRedactingACaseChangesOnlyIt(kType3FontBorrowingPageResources);
}

// An appearance without /Resources draws with the page's font and colour
// space.
TEST_P(EPDFContentRewriteEmbedderTest, KeepsResourcesAnAppearanceBorrows) {
  ExpectRedactingACaseChangesOnlyIt(kAppearanceBorrowingPageResources);
}

// One form drawn twice: ALPHA redacted where it is drawn first, BRAVO where
// it is drawn second.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactsEachPlacementOfASharedFormOnItsOwn) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kSharedFormTwoRegions, [](FPDF_PAGE page) {
        const std::vector<FS_RECTF> alpha = AreasOf(page, L"ALPHA");
        const std::vector<FS_RECTF> bravo = AreasOf(page, L"BRAVO");
        EXPECT_EQ(2u, alpha.size());
        EXPECT_EQ(2u, bravo.size());
        return alpha.size() == 2 && bravo.size() == 2
                   ? std::vector<FS_RECTF>{alpha[0], bravo[1]}
                   : std::vector<FS_RECTF>();
      });
}

TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingOnePlacementOfASharedFormKeepsTheOther) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kSharedFormOneRegion, [](FPDF_PAGE page) {
        const std::vector<FS_RECTF> alpha = AreasOf(page, L"ALPHA");
        EXPECT_EQ(2u, alpha.size());
        return alpha.empty() ? alpha : std::vector<FS_RECTF>{alpha[0]};
      });
}

TEST_P(EPDFContentRewriteEmbedderTest, KeepsTextScalingRiseAndSpacing) {
  ExpectRedactingACaseChangesOnlyIt(kTextScalingRiseAndSpacing);
}

TEST_P(EPDFContentRewriteEmbedderTest, KeepsHiddenOptionalContentHidden) {
  ExpectRedactingACaseChangesOnlyIt(kHiddenOptionalContent);
}

// Text that clips (render mode 7) keeps a copy of itself in the clip of what
// is drawn after it. Redacting the text redacts the copy too: the saved page
// must not hold the word again as clip text. With all of it gone, the
// rectangle it clipped shows nowhere.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingClippingTextRemovesItEverywhere) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kTextClip,
      [](FPDF_PAGE page) { return Grown(AreasOf(page, L"CLIP"), 4); });
}

TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingPartOfClippingTextKeepsTheRest) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kTextClip,
      [](FPDF_PAGE page) { return Grown(AreasOf(page, L"LI"), 2); });
}

// Fill and clip (mode 4), two text objects in one BT: redacting one keeps
// the other drawn and clipping.
TEST_P(EPDFContentRewriteEmbedderTest, RedactsClippingTextAcrossTextObjects) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kTextClipAcrossTextObjects,
      [](FPDF_PAGE page) { return Grown(AreasOf(page, L"CD"), 2); });
}

// A mirrored, skewed and unevenly scaled form: a word, the text that clips
// and the image redacted; its soft mask and the rest stay in place. Whole
// words and the whole image: an upright area over part of slanted content
// also takes the glyphs and image pixels whose slanted boxes it touches.
TEST_P(EPDFContentRewriteEmbedderTest, RedactsInsideAMirroredSkewedForm) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kMirroredSkewedForm, [](FPDF_PAGE page) {
        std::vector<FS_RECTF> areas = AreasOf(page, L"REDACTME");
        for (const FS_RECTF& area : Grown(AreasOf(page, L"CLIPPED"), 2)) {
          areas.push_back(area);
        }
        areas.push_back(FS_RECTF{288, 317, 504, 223});  // the image
        return areas;
      });
}

// An appearance draws a form that borrows the page's font and colour space,
// one through an appearance without /Resources, one through an appearance
// whose /Resources have no /ColorSpace.
TEST_P(EPDFContentRewriteEmbedderTest, KeepsResourcesNestedAppearancesBorrow) {
  ExpectRedactingACaseChangesOnlyIt(kNestedAppearancesBorrowingPageResources);
}

// A Type3 glyph draws a form that borrows the page's colour space.
TEST_P(EPDFContentRewriteEmbedderTest, KeepsResourcesAType3GlyphFormBorrows) {
  ExpectRedactingACaseChangesOnlyIt(kType3GlyphFormBorrowingPageResources);
}

TEST_P(EPDFContentRewriteEmbedderTest, KeepsICCBasedDefaultsAndCMYKPatterns) {
  ExpectRedactingACaseChangesOnlyIt(
      kICCBasedDefaultsAndCMYKPattern, [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        const std::string content =
            PageContent(saved, kICCBasedDefaultsAndCMYKPattern);
        EXPECT_NE(std::string::npos, content.find("/CSicc cs .2 .4"))
            << content;
        EXPECT_NE(std::string::npos,
                  content.find("/CSpatk cs 0 .6 1 0 /"))
            << content;
        RetainPtr<const CPDF_Dictionary> spaces =
            CPDFDocumentFromFPDFDocument(saved)
                ->GetPageDictionary(kICCBasedDefaultsAndCMYKPattern)
                ->GetDictFor("Resources")
                ->GetDictFor("ColorSpace");
        ASSERT_TRUE(spaces);
        EXPECT_TRUE(spaces->KeyExist("DefaultGray"));
        EXPECT_TRUE(spaces->KeyExist("DefaultCMYK"));
      });
}

// A word redacted in content that is hidden (optional content that is off)
// is removed, and the rest stays hidden.
TEST_P(EPDFContentRewriteEmbedderTest, RedactsInsideHiddenOptionalContent) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kRedactionInHiddenOptionalContent,
      [](FPDF_PAGE) {
        // SECRET in HIDDEN SECRET, Helvetica 24 at (100, 600).
        return std::vector<FS_RECTF>{{196, 622, 300, 593}};
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        const std::string content =
            PageContent(saved, kRedactionInHiddenOptionalContent);
        // SECRET, as the generator writes text (hex).
        EXPECT_EQ(std::string::npos, content.find("534543524554")) << content;
        EXPECT_NE(std::string::npos, content.find("/OC")) << content;
      });
}

// Two redactions applied one after the other give what one apply of both
// gives.
TEST_P(EPDFContentRewriteEmbedderTest, RedactingInTurnChangesOnlyTheAreas) {
  ASSERT_TRUE(Open("redact_rewrite_cases.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(doc_.get(), kSharedFormTwoRegions));
  ASSERT_TRUE(page);
  ScopedFPDFBitmap before = RenderAtPageSize(page.get());
  const std::vector<FS_RECTF> alpha = AreasOf(page.get(), L"ALPHA");
  const std::vector<FS_RECTF> bravo = AreasOf(page.get(), L"BRAVO");
  ASSERT_EQ(2u, alpha.size());
  ASSERT_EQ(2u, bravo.size());
  for (const FS_RECTF& area : {alpha[0], bravo[1]}) {
    Mark(page.get(), {area});
    Change([&] { ApplyRedactions(page.get()); });
    ASSERT_FALSE(HasFatalFailure());
  }
  SaveAndReopen();
  ASSERT_FALSE(HasFatalFailure());
  ExpectSavedPageChangedOnlyIn(kSharedFormTwoRegions, before.get(),
                               {alpha[0], bravo[1]});
}

// A layer's transaction aborted after a redaction leaves nothing of it, and
// the redaction made again comes out as made once.
TEST_P(EPDFContentRewriteEmbedderTest, RedactsAgainAfterAnAbortedTransaction) {
  if (!layer()) {
    GTEST_SKIP() << "transactions are a layer's";
  }
  ASSERT_TRUE(Open("redact_special_colorspaces.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(doc_.get(), 0));
  ASSERT_TRUE(page);
  ScopedFPDFBitmap before = RenderAtPageSize(page.get());
  const std::vector<FS_RECTF> areas = AreasOf(page.get(), L"Jane Doe");
  ASSERT_FALSE(areas.empty());
  Mark(page.get(), areas);

  ASSERT_TRUE(EPDFLayer_BeginTransaction(doc_.get()));
  ApplyRedactions(page.get());
  ASSERT_TRUE(EPDFLayer_AbortTransaction(doc_.get()));
  page.reset(FPDF_LoadPage(doc_.get(), 0));
  ASSERT_TRUE(page);
  ASSERT_EQ(static_cast<int>(areas.size()), FPDFPage_GetAnnotCount(page.get()));

  ChangeAndSave([&] { ApplyRedactions(page.get()); });
  ASSERT_FALSE(HasFatalFailure());
  ExpectSavedPageChangedOnlyIn(0, before.get(), areas);
}

// One name for two resources: a form borrowing the page's resources uses the
// font /F1, and the page's XObject /F1 draws CROSSTYPE. Redacting CROSSTYPE
// leaves the XObject unused: the font of the same name must not keep it, and
// its original content, in the file.
TEST_P(EPDFContentRewriteEmbedderTest, AFontKeepsNoXObjectOfTheSameName) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kNameForTwoResourceTypes,
      [](FPDF_PAGE page) { return AreasOf(page, L"CROSSTYPE"); },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved), "CROSSTYPE");
      });
}

// One form drawn by two appearances: the first gives it a font and colour
// space, the second only its /XObject, so there it reads the page's. The
// page keeps what the second placement reads.
TEST_P(EPDFContentRewriteEmbedderTest,
       KeepsWhatAFormReadsInEachOfItsPlacements) {
  ExpectRedactingACaseChangesOnlyIt(kFormReadingOtherResourcesInEachPlacement);
}

// All text in a Type3 font redacted: the font goes, and so does the form its
// glyph drew from the page's resources - and what that form wrote.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3FontsTextReleasesItsGlyphForms) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kAllTextInAType3FontRedacted,
      [](FPDF_PAGE page) { return AreasOf(page, L"AAA"); },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved), "TYPE3SECRET");
      });
}

// A form made inactive is not written, and nothing it borrowed is kept for
// it: the colour space only it used goes.
TEST_P(EPDFContentRewriteEmbedderTest, AnInactiveFormKeepsNothingItBorrowed) {
  ASSERT_TRUE(Open("redact_rewrite_cases.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(doc_.get(), kFormBorrowingPageResources));
  ASSERT_TRUE(page);
  FPDF_PAGEOBJECT form = FPDFPage_GetObject(page.get(), 0);
  ASSERT_EQ(FPDF_PAGEOBJ_FORM, FPDFPageObj_GetType(form));
  ChangeAndSave([&] {
    ASSERT_TRUE(FPDFPageObj_SetIsActive(form, false));
    ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
  });
  ASSERT_FALSE(HasFatalFailure());
  RetainPtr<const CPDF_Dictionary> resources =
      CPDFDocumentFromFPDFDocument(saved_document())
          ->GetPageDictionary(kFormBorrowingPageResources)
          ->GetDictFor("Resources");
  ASSERT_TRUE(resources);
  RetainPtr<const CPDF_Dictionary> spaces = resources->GetDictFor("ColorSpace");
  EXPECT_FALSE(spaces && spaces->KeyExist("CSsep"));
  CloseSaved();
}

// An appearance borrows the page's Type3 font. Its glyph draws /FmG from the
// resources of the content using the font - the appearance's, a green square
// - not the page's /FmG, which writes TYPE3CTXSECRET. Redacting that text
// must not keep the page's /FmG, and what it wrote, for the glyph.
TEST_P(EPDFContentRewriteEmbedderTest,
       AType3GlyphReadsTheResourcesOfTheContentUsingTheFont) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3FontBorrowedByAnAppearance,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 140, 590));
        return AreasOf(page, L"TYPE3CTXSECRET");
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinel(CPDFDocumentFromFPDFDocument(saved), "TYPE3CTXSECRET");
      });
}

// One Type3 font, two glyphs: A draws a form from the page's resources that
// writes TYPE3PARTIAL, B a green square. Redacting the text drawn in A keeps
// B drawing, and takes A's glyph - and what only it drew - off the page.
// Another page drawing A keeps it.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingOneType3GlyphTakesWhatOnlyItDrew) {
  ScopedFPDFBitmap other_before;
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3FontPartlyRedacted,
      [&](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 130, 680));  // B
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // A
        other_before = RenderOtherPage(kType3GlyphsUsedElsewhere);
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};  // A
      },
      [&](FPDF_DOCUMENT saved, FPDF_PAGE) {
        CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(saved);
        ExpectNoSentinelOnPage(pdf, kType3FontPartlyRedacted, "TYPE3PARTIAL");
        ExpectOtherPageUnchanged(saved, kType3GlyphsUsedElsewhere,
                                 other_before.get());
        // A's procedure and the form it draws.
        EXPECT_EQ(2, CountSentinelOnPage(pdf, kType3GlyphsUsedElsewhere,
                                         "TYPE3PARTIAL"));
      });
}

// A Type3 font whose glyphs draw from its own /Resources: A draws /ImA,
// which writes PRIVATEA. The text in A is redacted; the rest of the page
// shows B and C through ' and a form inheriting the font, and an appearance
// shows them through ', " and a nested form, never A. Each of those draws
// with a copy of the font holding only the glyphs it shows: holding on to
// the font would keep A's procedure and /ImA reachable from the page.
// Another page drawing A keeps it.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingOneType3GlyphTakesItFromEveryUseOnThePage) {
  ScopedFPDFBitmap other_before;
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3FontWithResourcesPartlyRedacted,
      [&](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // A
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 130, 680));  // B by '
        EXPECT_EQ(0xff0000u, PixelAt(before.get(), 330, 680));  // C by a form
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 128, 368));  // B by '
        EXPECT_EQ(0xff0000u, PixelAt(before.get(), 128, 328));  // C by "
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 268, 348));  // B by a form
        other_before = RenderOtherPage(kType3GlyphsUsedElsewhere);
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};  // A
      },
      [&](FPDF_DOCUMENT saved, FPDF_PAGE) {
        CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(saved);
        ExpectNoSentinelOnPage(pdf, kType3FontWithResourcesPartlyRedacted,
                               "PRIVATEA");
        ExpectOtherPageUnchanged(saved, kType3GlyphsUsedElsewhere,
                                 other_before.get());
        // A's procedure and /ImA.
        EXPECT_EQ(
            2, CountSentinelOnPage(pdf, kType3GlyphsUsedElsewhere, "PRIVATEA"));
      });
}

// A form shows text in the font, spacing and stroke mode the page set before
// drawing it, not set in the form. Writing the page again keeps them.
TEST_P(EPDFContentRewriteEmbedderTest, KeepsTheTextStateAFormInherits) {
  ExpectRedactingACaseChangesOnlyIt(kFormInheritingTextState);
}

// The page sets character spacing before drawing a form whose text sets it
// back to 0; redaction writes the form again. Its text keeps spacing 0.
TEST_P(EPDFContentRewriteEmbedderTest,
       AFormWrittenAgainStartsFromItsOwnTextState) {
  ExpectRedactingACaseChangesOnlyIt(kFormSettingSpacingBack);
}

// Glyph A of a Type3 font is named TYPE3NAMESECRET (in the encoding, the
// procedures and the descriptor's /CharSet) and stands for the text
// TYPE3UNICODESECRET in /ToUnicode. Redacting the text in A takes both off
// the page; B keeps its own text.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphTakesItsNameAndText) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3GlyphNamesAndText,
      [](FPDF_PAGE page) {
        EXPECT_EQ(L"B", TextInside(page, {{98, 724, 174, 648}}));
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};  // A
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE page) {
        CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(saved);
        ExpectNoSentinelOnPage(pdf, kType3GlyphNamesAndText,
                               "TYPE3UNICODESECRET");
        ExpectNoSentinelOnPage(pdf, kType3GlyphNamesAndText, "TYPE3NAMESECRET");
        EXPECT_EQ(L"B", TextInside(page, {{98, 724, 174, 648}}));
        // No font on the page maps A's code to anything.
        RetainPtr<const CPDF_Dictionary> fonts =
            pdf->GetPageDictionary(kType3GlyphNamesAndText)
                ->GetDictFor("Resources")
                ->GetDictFor("Font");
        ASSERT_TRUE(fonts);
        CPDF_DictionaryLocker locker(fonts);
        for (const auto& [name, font] : locker) {
          RetainPtr<const CPDF_Dictionary> dict =
              ToDictionary(font->GetDirect());
          RetainPtr<const CPDF_Stream> to_unicode =
              dict ? dict->GetStreamFor("ToUnicode") : nullptr;
          if (to_unicode) {
            EXPECT_EQ(L"", CPDF_ToUnicodeMap(to_unicode).Lookup(0x41));
          }
        }
      });
}

// Two annotations on the page show B through appearances that read the
// page's resources. The first's /AP, and the second's dictionary of
// appearances by state, are shared with annotations on another page, whose
// resources differ. Redacting A rewrites the appearances for this page
// only: the other page draws as it did.
TEST_P(EPDFContentRewriteEmbedderTest,
       AnAppearanceSharedWithAnotherPageKeepsDrawingThere) {
  ScopedFPDFBitmap other_before;
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kAppearancesSharedWithAnotherPage,
      [&](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 300, 350));  // /Sq here
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 300, 200));
        other_before = RenderOtherPage(kPageSharingAppearances);
        EXPECT_EQ(0x0000ffu, PixelAt(other_before.get(), 300, 350));  // there
        EXPECT_EQ(0x0000ffu, PixelAt(other_before.get(), 300, 200));
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};  // A
      },
      [&](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectOtherPageUnchanged(saved, kPageSharingAppearances,
                                 other_before.get());
      });
}

// Glyph W of a Type3 font draws B of another Type3 font named in its
// /Resources; that font's glyph A draws what writes NESTEDPRIVATE. Redacting
// the text in A takes A from the font W draws too.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphTakesItFromTheGlyphsDrawingItsFont) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3GlyphDrawingAnotherFont,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 130, 680));  // W draws B
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // A
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};      // A
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kType3GlyphDrawingAnotherFont, "NESTEDPRIVATE");
      });
}

// Glyphs W and X of a Type3 font draw B and A of another. Redacting the text
// in X takes X, and A of the font it drew: W draws a copy holding B only.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphTakesWhatItDrewInAnotherFont) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3GlyphDrawnByARedactedGlyph,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 130, 680));  // W draws B
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // X draws A
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};      // X
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kType3GlyphDrawnByARedactedGlyph,
                               "INNERPRIVATE");
      });
}

// Glyph W of a Type3 font without /Resources draws B of the page's Type3
// font /I. Redacting the text in A of /I: W draws a copy that reads
// resources of its own, holding a copy of /I with B only.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphTakesItFromAGlyphReadingThePage) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3GlyphBorrowingAFont,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 130, 680));  // W draws B
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // A
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};      // A
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kType3GlyphBorrowingAFont, "BORROWPRIVATE");
      });
}

// Glyph B of a Type3 font draws itself and a form showing B of another Type3
// font from the form's own /Resources. Redacting the text in A of the first
// font: B draws a copy of its font that names itself, and a copy of the form
// naming a copy of the other font. Neither A stays.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphFollowsAGlyphDrawingItsOwnFont) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3GlyphDrawingItsOwnFont,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 110, 660));  // B's square
        EXPECT_EQ(0xff00ffu, PixelAt(before.get(), 110, 700));  // the form
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // A
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};      // A
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        CPDF_Document* pdf = CPDFDocumentFromFPDFDocument(saved);
        ExpectNoSentinelOnPage(pdf, kType3GlyphDrawingItsOwnFont,
                               "CYCLEPRIVATE");
        ExpectNoSentinelOnPage(pdf, kType3GlyphDrawingItsOwnFont,
                               "FORMPRIVATE");
      });
}

// A form names the page's Type3 font in resources of its own and shows no
// text in it. Redacting the text in A of that font writes the form again,
// without the name: nothing keeps A.
TEST_P(EPDFContentRewriteEmbedderTest,
       AFormNamingARedactedType3FontItNeverShowsLetsGoOfIt) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kFormNamingAType3FontItNeverShows,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 320, 320));  // the form
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};      // A
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kFormNamingAType3FontItNeverShows,
                               "UNUSEDPRIVATE");
      });
}

// Glyph W of a Type3 font paints through a soft mask whose group shows B of
// another Type3 font. Redacting the text in A of that font: W's copy reads
// a copy of the mask, whose group names a copy of the font holding B.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphFollowsAGlyphsSoftMask) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3GlyphSoftMaskDrawingAFont,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_NE(0xffffffu, PixelAt(before.get(), 130, 680));  // W
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // A
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};      // A
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kType3GlyphSoftMaskDrawingAFont, "MASKPRIVATE");
      });
}

// The page paints through a soft mask, its graphics state written inline,
// whose group shows B of the page's Type3 font. Redacting the text in A: the
// page's graphics state of that name becomes a copy whose group names a copy
// of the font holding B.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphFollowsASoftMaskOnThePage) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kSoftMaskDrawingAType3Font,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_NE(0xffffffu, PixelAt(before.get(), 350, 350));  // mask
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // A
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};      // A
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kSoftMaskDrawingAType3Font, "PAGEMASKPRIVATE");
      });
}

// The page fills a square with a tiling pattern whose cell shows B of the
// page's Type3 font. Redacting the text in A: the page's pattern becomes a
// copy naming a copy of the font holding B.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphFollowsATilingPattern) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kTilingPatternDrawingAType3Font,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 350, 350));  // pattern
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // A
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};      // A
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kTilingPatternDrawingAType3Font,
                               "PATTERNPRIVATE");
      });
}

// Glyph W of a Type3 font draws two forms with resources of their own,
// showing B and C of one Type3 font whose glyphs draw /Im from the content
// showing them: green in one form, red in the other. Redacting the text in A
// of that font gives each form its own copy, reading its own /Im.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphKeepsEachContextOfAFontInAGlyph) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3FontInTwoContextsInAGlyph,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 115, 680));  // B
        EXPECT_EQ(0xff0000u, PixelAt(before.get(), 155, 680));  // C
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // A
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};      // A
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kType3FontInTwoContextsInAGlyph,
                               "CONTEXTPRIVATE");
      });
}

// Glyph W of a Type3 font selects another and draws a form whose text shows
// B in it without selecting a font; the form's own /Resources name the font
// too, unused. Redacting the text in A of that font replaces it where W
// selected it: nothing keeps A.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphFollowsAFontAFormInherits) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3FontInheritedPastAnUnusedAlias,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 130, 680));  // W draws B
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // A
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};      // A
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kType3FontInheritedPastAnUnusedAlias,
                               "INHERITPRIVATE");
      });
}

// Glyph W of a Type3 font draws two forms whose own /Resources hold only an
// /Im (green, red); the font each selects is named once, in W's resources, a
// Type3 font whose glyphs draw /Im from the content selecting them.
// Redacting the text in A of that font gives each form its own copy, and its
// own name for it.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphKeepsContextsThatShareAFontsName) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3FontInTwoContextsSharingItsName,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 115, 680));  // B
        EXPECT_EQ(0xff0000u, PixelAt(before.get(), 155, 680));  // C
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // A
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};      // A
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kType3FontInTwoContextsSharingItsName,
                               "FALLBACKPRIVATE");
      });
}

// A Type3 font's /Resources name another, whose A was redacted, without its
// glyphs ever selecting it. The font the page keeps names nothing it doesn't
// use.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphTakesAFontNamedButNeverSelected) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3FontNamingAnotherUnselected,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 130, 680));  // W
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kType3FontNamingAnotherUnselected,
                               "UNSELECTEDPRIVATE");
      });
}

// A Type3 glyph selects another font, whose A was redacted, and shows no
// text in it. The name stays; it names a copy holding no glyph.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphTakesAFontSelectedWithoutText) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3FontSelectingAnotherWithoutText,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 130, 680));  // W
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kType3FontSelectingAnotherWithoutText,
                               "SELECTEDPRIVATE");
      });
}

// Glyph X of a Type3 font drew A of another font, which the page also shows
// B of directly. Redacting the text in X: every Type3 font the page reaches
// holds only what the page shows, so A goes from that font too.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphTakesWhatItDrewFromAFontThePageShows) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3FontDrawnByARedactedGlyph,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 130, 680));  // W draws B
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 330, 680));  // the page's B
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kType3FontDrawnByARedactedGlyph, "DRAWNPRIVATE");
      });
}

// Glyph W selects a Type3 font without /Resources and shows no text in it,
// then draws a form that selects the same font itself, under its own
// resources, and shows B. The form's text counts where the form selected the
// font: W's selection holds no glyph, and W's unused /Im (SELECTIONPRIVATE)
// goes.
TEST_P(EPDFContentRewriteEmbedderTest,
       RedactingAType3GlyphCountsTextWhereItsFontWasSelected) {
  ExpectRedactionChangesOnlyItsAreas(
      "redact_rewrite_cases.pdf", kType3FontSelectedAgainByAForm,
      [](FPDF_PAGE page) {
        ScopedFPDFBitmap before = RenderAtPageSize(page);
        EXPECT_EQ(0x00ff00u, PixelAt(before.get(), 130, 680));  // W draws B
        EXPECT_EQ(0x0000ffu, PixelAt(before.get(), 130, 530));  // A
        return std::vector<FS_RECTF>{{98, 574, 174, 498}};      // A
      },
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectNoSentinelOnPage(CPDFDocumentFromFPDFDocument(saved),
                               kType3FontSelectedAgainByAForm,
                               "SELECTIONPRIVATE");
      });
}

// A page as "Microsoft: Print To PDF" writes it: one cm for the whole page,
// never undone, and the content cut into streams in the middle of
// q ... cm ... Q, so each stream ends at another CTM than it began with.
// Every stream is written again and stands alone: none hands the next a CTM.
// (Upstream's cm for that multiplies the wrong way round, which moved
// everything from the third stream on.)
TEST_P(EPDFContentRewriteEmbedderTest, KeepsContentCutIntoStreams) {
  ExpectRedactingWordChangesOnlyIt(
      "redact_split_content_streams.pdf", kCutAsPrintToPDFDoes, L"SECRET",
      [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectEachStreamStandsAlone(saved, kCutAsPrintToPDFDoes);
      });
}

// The cut falls after a cm with no inverse (a strip of no height): no stream
// written again can undo it to draw in place, so none may begin under it.
TEST_P(EPDFContentRewriteEmbedderTest,
       KeepsContentCutInsideATransformWithoutAnInverse) {
  ExpectRedactingWordChangesOnlyIt(
      "redact_split_content_streams.pdf", kCutInsideATransformWithoutAnInverse,
      L"SECRET", [](FPDF_DOCUMENT saved, FPDF_PAGE) {
        ExpectEachStreamStandsAlone(saved,
                                    kCutInsideATransformWithoutAnInverse);
      });
}

// A second rewrite of the loaded page starts from the streams the first one
// wrote, not from the CTMs the page was parsed with.
TEST_P(EPDFContentRewriteEmbedderTest,
       RewritingAgainKeepsContentCutIntoStreams) {
  ASSERT_TRUE(Open("redact_split_content_streams.pdf"));
  ScopedFPDFPage page(FPDF_LoadPage(doc_.get(), kCutAsPrintToPDFDoes));
  ASSERT_TRUE(page);
  ScopedFPDFBitmap before = RenderAtPageSize(page.get());
  std::vector<FS_RECTF> areas;
  for (const wchar_t* word : {L"SECRET", L"STAYS"}) {
    const std::vector<FS_RECTF> found = AreasOf(page.get(), word);
    ASSERT_EQ(1u, found.size());
    areas.push_back(found[0]);
    Mark(page.get(), found);
    Change([&] { ApplyRedactions(page.get()); });
    ASSERT_FALSE(HasFatalFailure());
  }
  SaveAndReopen();
  ASSERT_FALSE(HasFatalFailure());
  ExpectSavedPageChangedOnlyIn(kCutAsPrintToPDFDoes, before.get(), areas,
                               [](FPDF_DOCUMENT saved, FPDF_PAGE) {
                                 ExpectEachStreamStandsAlone(
                                     saved, kCutAsPrintToPDFDoes);
                               });
}

// An object added to the page goes into a stream of its own, which begins by
// undoing the CTM the last stream ended at. Added after a rewrite, it finds
// the streams the rewrite wrote; added before one, its stream is written
// again with the others. Either way it is drawn where it was put.
TEST_P(EPDFContentRewriteEmbedderTest,
       AnObjectAddedAroundARewriteIsDrawnInPlace) {
  for (const bool add_first : {false, true}) {
    SCOPED_TRACE(add_first ? "added before the rewrite"
                           : "added after the rewrite");
    ASSERT_TRUE(Open("redact_split_content_streams.pdf"));
    ScopedFPDFPage page(FPDF_LoadPage(doc_.get(), kCutAsPrintToPDFDoes));
    ASSERT_TRUE(page);
    ScopedFPDFBitmap before = RenderAtPageSize(page.get());
    std::vector<FS_RECTF> areas = AreasOf(page.get(), L"SECRET");
    ASSERT_EQ(1u, areas.size());
    Mark(page.get(), areas);
    // A red square on empty paper.
    const FS_RECTF square = {200, 60, 240, 40};
    const auto add = [&] {
      FPDF_PAGEOBJECT rect = FPDFPageObj_CreateNewRect(200, 40, 40, 20);
      ASSERT_TRUE(FPDFPageObj_SetFillColor(rect, 255, 0, 0, 255));
      ASSERT_TRUE(FPDFPath_SetDrawMode(rect, FPDF_FILLMODE_ALTERNATE, false));
      FPDFPage_InsertObject(page.get(), rect);
      ASSERT_TRUE(FPDFPage_GenerateContent(page.get()));
    };
    const auto redact = [&] { ApplyRedactions(page.get()); };
    Change(add_first ? std::function<void()>(add) : redact);
    ASSERT_FALSE(HasFatalFailure());
    Change(add_first ? std::function<void()>(redact) : add);
    ASSERT_FALSE(HasFatalFailure());
    SaveAndReopen();
    ASSERT_FALSE(HasFatalFailure());
    {
      ScopedSavedPage saved = LoadScopedSavedPage(kCutAsPrintToPDFDoes);
      ASSERT_TRUE(saved);
      ScopedFPDFBitmap after = RenderAtPageSize(saved.get());
      EXPECT_EQ(0xff0000u, PixelAt(after.get(), 220, 50));
      areas.push_back(square);
      std::string where;
      EXPECT_EQ(0, CountChangedPixelsOutside(before.get(), after.get(), areas,
                                             &where))
          << where;
    }
    CloseSaved();
  }
}

// Where a page's content is cut into streams makes no difference to a
// reader, and so none to a rewrite. Every page of the rewrite cases is
// written again - redacted, or unchanged where it shows no word to redact -
// with its content as it is, cut into three streams, and with every operator
// in a stream of its own: each cut copy draws what the file draws, before
// the rewrite and after it.
TEST_P(EPDFContentRewriteEmbedderTest,
       WhereContentIsCutIntoStreamsChangesNothing) {
  for (const char* name :
       {"redact_rewrite_cases.pdf", "redact_special_colorspaces.pdf",
        "edit_form_with_matrix.pdf", "redact_split_content_streams.pdf"}) {
    const std::string path = PathService::GetTestFilePath(name);
    std::vector<ScopedFPDFBitmap> uncut_before;
    const std::vector<ScopedFPDFBitmap> uncut_after =
        RewriteEveryPage(GetFileContents(path.c_str()), &uncut_before);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_FALSE(uncut_after.empty());
    for (const Cut cut : {Cut::kIntoThree, Cut::kAfterEveryOperator}) {
      std::vector<ScopedFPDFBitmap> cut_before;
      const std::vector<ScopedFPDFBitmap> cut_after =
          RewriteEveryPage(CutIntoStreams(name, cut), &cut_before);
      ASSERT_FALSE(HasFatalFailure());
      ASSERT_EQ(uncut_after.size(), cut_after.size());
      for (size_t i = 0; i < cut_after.size(); ++i) {
        SCOPED_TRACE(std::string(name) + " page " + std::to_string(i) +
                     (cut == Cut::kIntoThree ? ", cut into three streams"
                                             : ", cut after every operator"));
        std::string where;
        EXPECT_EQ(0, CountChangedPixelsOutside(uncut_before[i].get(),
                                               cut_before[i].get(), {}, &where))
            << "the cut copy draws another page: " << where;
        EXPECT_EQ(0, CountChangedPixelsOutside(uncut_after[i].get(),
                                               cut_after[i].get(), {}, &where))
            << "the rewrite of the cut copy draws another page: " << where;
      }
    }
  }
}

// The text left after a redaction starts a text object of its own, and each
// character of it keeps its origin. Renders can't show this: PDFium draws
// each glyph on a whole pixel, so text that drifts by less than half a pixel
// renders as before - and a glyph lying on a pixel edge may be drawn a pixel
// over when its origin moves by a millionth of a point, as float rounding
// moves it.
TEST_P(EPDFContentRewriteEmbedderTest, TextLeftAfterARedactionKeepsItsPlace) {
  struct Case {
    const char* name;
    int index;
    const wchar_t* word;
  };
  for (const Case& test :
       {Case{"redact_split_content_streams.pdf", kCutAsPrintToPDFDoes, L"tail"},
        Case{"redact_split_content_streams.pdf",
             kCutInsideATransformWithoutAnInverse, L"SECRET"},
        Case{"redact_rewrite_cases.pdf", kTextScalingRiseAndSpacing, L"and"}}) {
    SCOPED_TRACE(std::string(test.name) + " page " +
                 std::to_string(test.index));
    ASSERT_TRUE(Open(test.name));
    ScopedFPDFPage page(FPDF_LoadPage(doc_.get(), test.index));
    ASSERT_TRUE(page);
    const std::vector<FS_RECTF> areas = AreasOf(page.get(), test.word);
    ASSERT_FALSE(areas.empty());
    const std::vector<PlacedChar> before = CharsOutside(page.get(), areas);
    ASSERT_FALSE(before.empty());
    Mark(page.get(), areas);
    ChangeAndSave([&] { ApplyRedactions(page.get()); });
    ASSERT_FALSE(HasFatalFailure());
    {
      ScopedSavedPage saved = LoadScopedSavedPage(test.index);
      ASSERT_TRUE(saved);
      const std::vector<PlacedChar> after = CharsOutside(saved.get(), {});
      ASSERT_EQ(before.size(), after.size());
      for (size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before[i].unicode, after[i].unicode) << i;
        EXPECT_NEAR(before[i].x, after[i].x, 0.001) << i;
        EXPECT_NEAR(before[i].y, after[i].y, 0.001) << i;
      }
    }
    CloseSaved();
  }
}
