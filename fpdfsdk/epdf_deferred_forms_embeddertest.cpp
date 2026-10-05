// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// Nested forms parsed after the content that places them, in a parser stage
// of their own (CPDF_ContentParser::SetDeferNestedForms), must give exactly
// what parsing them where they are met gives: the same objects, with the same
// bounds, clips and flags, and the same pixels. Also when the parse pauses
// after every step.
//
// The corpus gate runs when EPDF_DEFERRED_FORMS_CORPUS names PDF files or
// directories (separated by ':'); EPDF_DEFERRED_FORMS_MAX_PAGES (default 20)
// limits the pages per file, EPDF_DEFERRED_FORMS_SKIP (':'-separated) skips
// files whose path contains one of its parts. EPDF_DEFER_NESTED_FORMS=1
// defers nested forms in every other test of the run too.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/fpdfapi/page/cpdf_clippath.h"
#include "core/fpdfapi/page/cpdf_contentparser.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fxcrt/pauseindicator_iface.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_edit_test_util.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdfview.h"
#include "testing/embedder_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

class PauseEveryStep final : public PauseIndicatorIface {
 public:
  bool NeedToPauseNow() override { return true; }
};

// EPDF_DEFER_NESTED_FORMS=1 defers nested forms in every test of the run,
// so the whole suite can be compared with and without.
bool DeferByDefault() {
  const char* value = getenv("EPDF_DEFER_NESTED_FORMS");
  return value && *value == '1';
}

class DeferNestedFormsEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    CPDF_ContentParser::SetDeferNestedForms(DeferByDefault());
  }
};

const ::testing::Environment* const g_defer_environment =
    ::testing::AddGlobalTestEnvironment(new DeferNestedFormsEnvironment);

// Parsers made in this scope defer nested forms, or don't.
class ScopedDeferNestedForms {
 public:
  explicit ScopedDeferNestedForms(bool defer) {
    CPDF_ContentParser::SetDeferNestedForms(defer);
  }
  ~ScopedDeferNestedForms() {
    CPDF_ContentParser::SetDeferNestedForms(DeferByDefault());
  }
};

std::string Number(float value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.9g", value);
  return buffer;
}

std::string Rect(const CFX_FloatRect& rect) {
  return "[" + Number(rect.left) + " " + Number(rect.bottom) + " " +
         Number(rect.right) + " " + Number(rect.top) + "]";
}

std::string Matrix(const CFX_Matrix& m) {
  return "[" + Number(m.a) + " " + Number(m.b) + " " + Number(m.c) + " " +
         Number(m.d) + " " + Number(m.e) + " " + Number(m.f) + "]";
}

// Everything parsing decides about a holder's objects, forms opened.
void Describe(const CPDF_PageObjectHolder& holder,
              const std::string& indent,
              std::string& out) {
  out += indent + "holder parsed=" +
         std::to_string(static_cast<int>(holder.GetParseState())) +
         " alpha=" + std::to_string(holder.BackgroundAlphaNeeded()) +
         " masks=" + std::to_string(holder.GetMaskBoundingBoxes().size()) +
         " objects=" + std::to_string(holder.GetPageObjectCount()) + "\n";
  for (const auto& object : holder) {
    out += indent + std::to_string(static_cast<int>(object->GetType())) +
           " rect=" + Rect(object->GetRect()) +
           " stream=" + std::to_string(object->GetContentStream()) +
           " active=" + std::to_string(object->IsActive());
    const CPDF_ClipPath& clip = object->clip_path();
    if (clip.HasRef()) {
      out += " clip=" + std::to_string(clip.GetPathCount()) + "/" +
             std::to_string(clip.GetTextCount()) + Rect(clip.GetClipBox());
    }
    if (const CPDF_PathObject* path = object->AsPath()) {
      out += " points=" + std::to_string(path->path().GetPoints().size()) +
             " fill=" + std::to_string(static_cast<int>(path->filltype())) +
             " stroke=" + std::to_string(path->stroke()) +
             " matrix=" + Matrix(path->matrix());
    } else if (const CPDF_TextObject* text = object->AsText()) {
      out += " chars=" + std::to_string(text->GetCharCodes().size()) +
             " at=" + Number(text->GetPos().x) + "," + Number(text->GetPos().y);
    } else if (const CPDF_ImageObject* image = object->AsImage()) {
      out += " image=" + Matrix(image->matrix());
    }
    out += "\n";
    if (const CPDF_FormObject* form = object->AsForm()) {
      out += indent + " form matrix=" + Matrix(form->form_matrix()) +
             " bbox=" + Rect(form->form()->GetBBox()) + "\n";
      Describe(*form->form(), indent + "  ", out);
    }
  }
}

struct Parsed {
  bool loaded = false;
  std::string tree;
  std::string pixels;
  EPDF_PAGE_PARSED_SIZE size = {};
};

// Renders at most `max_side` pixels on a side, with annotations.
std::string Pixels(FPDF_PAGE page, int max_side) {
  const float width = FPDF_GetPageWidthF(page);
  const float height = FPDF_GetPageHeightF(page);
  const float longest = std::max(width, height);
  const float scale = longest > max_side ? max_side / longest : 1.0f;
  const int w = std::max(1, static_cast<int>(width * scale));
  const int h = std::max(1, static_cast<int>(height * scale));
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(w, h, 0));
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, w, h, 0xffffffff);
  FPDF_RenderPageBitmap(bitmap.get(), page, 0, 0, w, h, 0, FPDF_ANNOT);

  // FNV-1a over the rows: equal renders give equal digests.
  const auto* pixels =
      static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get()));
  const int stride = FPDFBitmap_GetStride(bitmap.get());
  uint64_t digest = 0xcbf29ce484222325ull;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w * 4; ++x) {
      digest = (digest ^ pixels[y * stride + x]) * 0x100000001b3ull;
    }
  }
  return std::to_string(w) + "x" + std::to_string(h) + ":" +
         std::to_string(digest);
}

Parsed Describe(FPDF_PAGE page, int max_side) {
  Parsed parsed;
  parsed.loaded = true;
  Describe(*CPDFPageFromFPDFPage(page), "", parsed.tree);
  parsed.pixels = Pixels(page, max_side);
  EXPECT_TRUE(EPDFPage_GetParsedSize(page, &parsed.size));
  return parsed;
}

// Loads `page_index` of `doc`, parsing it in one go or pausing after every
// step, with nested forms deferred or not, and describes it. The renders
// parse annotation appearances, so they run with the same setting.
Parsed Load(FPDF_DOCUMENT doc, int page_index, bool defer, bool pause) {
  ScopedDeferNestedForms deferring(defer);
  if (!pause) {
    ScopedFPDFPage page(FPDF_LoadPage(doc, page_index));
    return page ? Describe(page.get(), 1200) : Parsed();
  }
  CPDF_Document* document = CPDFDocumentFromFPDFDocument(doc);
  auto page = pdfium::MakeRetain<CPDF_Page>(
      document, document->GetMutablePageDictionary(page_index));
  page->AddPageImageCache();
  page->StartParse(std::make_unique<CPDF_ContentParser>(page.Get()));
  PauseEveryStep every_step;
  while (page->GetParseState() != CPDF_PageObjectHolder::ParseState::kParsed) {
    page->ContinueParse(&every_step);
  }
  return Describe(FPDFPageFromIPDFPage(page.Get()), 1200);
}

void ExpectSame(const Parsed& expected, const Parsed& actual) {
  ASSERT_TRUE(expected.loaded);
  EXPECT_TRUE(actual.loaded);
  EXPECT_EQ(expected.tree, actual.tree);
  EXPECT_EQ(expected.pixels, actual.pixels);
  EXPECT_EQ(expected.size.objects, actual.size.objects);
  EXPECT_EQ(expected.size.path_points, actual.size.path_points);
  EXPECT_EQ(expected.size.text_chars, actual.size.text_chars);
  EXPECT_EQ(expected.size.estimated_bytes, actual.size.estimated_bytes);
}

std::string Form(const std::string& extra, const std::string& content) {
  return "<< /Type /XObject /Subtype /Form " + extra + " /Length " +
         std::to_string(content.size()) + " >>\nstream\n" + content +
         "\nendstream";
}

std::string OnePage(const std::string& resources,
                    const std::string& content,
                    const std::vector<std::string>& more_objects) {
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R "
      "/Resources " +
          resources + " >>",
      Stream(content),
  };
  objects.insert(objects.end(), more_objects.begin(), more_objects.end());
  return MakePdf(objects);
}

class EPDFDeferredFormsEmbedderTest : public EmbedderTest {
 protected:
  // The page parsed in one go without deferring, against deferring, and
  // against deferring with a pause after every step: each on a newly loaded
  // document, so no cache one parse fills serves another.
  void ExpectSameEveryWay(const std::string& pdf) {
    Parsed expected;
    {
      ScopedFPDFDocument doc(
          FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
      ASSERT_TRUE(doc);
      expected = Load(doc.get(), 0, /*defer=*/false, /*pause=*/false);
    }
    for (bool pause : {false, true}) {
      ScopedFPDFDocument doc(
          FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
      ASSERT_TRUE(doc);
      SCOPED_TRACE(pause ? "deferred, pausing" : "deferred");
      ExpectSame(expected, Load(doc.get(), 0, /*defer=*/true, pause));
    }
  }
};

}  // namespace

// Three forms deep, with matrices, clips to their boxes, a blend mode deep
// inside (which every holder above must learn of) and content after each
// placement.
TEST_F(EPDFDeferredFormsEmbedderTest, NestedFormsWithClipsAndTransparency) {
  const std::string pdf = OnePage(
      "<< /XObject << /Outer 5 0 R >> /Font << /F1 8 0 R >> >>",
      "0.9 g 0 0 300 300 re f /Outer Do "
      "BT /F1 18 Tf 10 280 Td (after outer) Tj ET "
      "0 0 1 RG 4 w 5 5 290 290 re S",
      {
          Form("/BBox [0 0 200 200] /Matrix [1 0 0 1 20 20] "
               "/Resources << /XObject << /Mid 6 0 R >> >>",
               "1 0 0 rg 0 0 200 200 re f q 0.5 0 0 0.5 10 10 cm /Mid Do Q "
               "0 1 0 rg 150 150 40 40 re f /Mid Do"),
          Form("/BBox [0 0 150 150] /Matrix [0.8 0.2 -0.2 0.8 30 0] "
               "/Resources << /XObject << /Inner 7 0 R >> "
               "/ExtGState << /G1 << /BM /Multiply /ca 0.6 >> >> >>",
               "0 0 1 rg 0 0 300 50 re f /G1 gs /Inner Do"),
          Form("/BBox [0 0 100 100] /Resources << /Font << /F1 8 0 R >> >> "
               "/Group << /S /Transparency >>",
               "1 1 0 rg 10 10 80 80 re f "
               "BT /F1 12 Tf 5 50 Td (inner) Tj ET "
               "0 0 0 RG 0 0 m 120 120 l S"),
          "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      });
  ExpectSameEveryWay(pdf);
}

// A form placed many times, more than a parse step holds, and placed inside
// itself's sibling: the order of placements is the order of objects.
TEST_F(EPDFDeferredFormsEmbedderTest, ManyPlacements) {
  std::string content;
  for (int i = 0; i < 260; ++i) {
    content +=
        "q 1 0 0 1 " + std::to_string(i % 20 * 14) + " " +
        std::to_string(i / 20 * 22) + " cm /Dot Do Q " +
        (i % 7 == 0 ? "0.5 g " + std::to_string(i % 290) + " 290 4 4 re f "
                    : "");
  }
  const std::string pdf =
      OnePage("<< /XObject << /Dot 5 0 R >> >>", content,
              {Form("/BBox [0 0 12 12] /Resources << /XObject << /Ring 6 0 R "
                    ">> >>",
                    "1 0 0 rg 0 0 10 10 re f /Ring Do"),
               Form("/BBox [0 0 12 12]", "0 0 1 RG 2 2 6 6 re S")});
  ExpectSameEveryWay(pdf);
}

// A form that draws itself: the recursion guard stops it at the same depth.
TEST_F(EPDFDeferredFormsEmbedderTest, AFormThatDrawsItself) {
  const std::string pdf = OnePage(
      "<< /XObject << /Self 5 0 R >> >>", "/Self Do",
      {Form("/BBox [0 0 300 300] /Resources << /XObject << /Self 5 0 R >> >>",
            "0 0 1 rg 0 0 20 20 re f 0.9 0 0 0.9 15 15 cm /Self Do")});
  ExpectSameEveryWay(pdf);
}

// Content in several streams, with forms in each: the guard keys on where
// each step starts.
TEST_F(EPDFDeferredFormsEmbedderTest, FormsAcrossContentStreams) {
  const std::string pdf = MakePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
      "/Contents [4 0 R 5 0 R] "
      "/Resources << /XObject << /A 6 0 R /B 7 0 R >> >> >>",
      Stream("q 1 0 0 1 10 10 cm /A Do Q 0 0 1 rg 100 100 20 20 re f"),
      Stream("q 1 0 0 1 150 150 cm /B Do Q /A Do"),
      Form("/BBox [0 0 50 50] /Resources << /XObject << /B 7 0 R >> >>",
           "1 0 0 rg 0 0 50 50 re f q 0.5 0 0 0.5 0 0 cm /B Do Q"),
      Form("/BBox [0 0 40 40]", "0 1 0 rg 5 5 30 30 re f"),
  });
  ExpectSameEveryWay(pdf);
}

// Deferred, a form waits for the content that placed it: at the first pause
// it is placed and not parsed, and its bounds come once it is. Without
// deferring, it is parsed in the step that meets it.
TEST_F(EPDFDeferredFormsEmbedderTest, FormsWaitForTheContentThatPlacesThem) {
  const std::string pdf =
      OnePage("<< /XObject << /F 5 0 R >> >>", "/F Do 0 0 1 rg 0 0 10 10 re f",
              {Form("/BBox [0 0 100 100]", "1 0 0 rg 20 20 30 30 re f")});
  for (bool defer : {false, true}) {
    ScopedDeferNestedForms deferring(defer);
    ScopedFPDFDocument doc(
        FPDF_LoadMemDocument(pdf.data(), pdf.size(), nullptr));
    ASSERT_TRUE(doc);
    CPDF_Document* document = CPDFDocumentFromFPDFDocument(doc.get());
    auto page = pdfium::MakeRetain<CPDF_Page>(
        document, document->GetMutablePageDictionary(0));
    page->StartParse(std::make_unique<CPDF_ContentParser>(page.Get()));
    PauseEveryStep every_step;
    page->ContinueParse(&every_step);
    ASSERT_EQ(2u, page->GetPageObjectCount());
    const auto* form = page->GetPageObjectByIndex(0)->AsForm();
    ASSERT_TRUE(form);
    EXPECT_EQ(defer ? CPDF_PageObjectHolder::ParseState::kNotParsed
                    : CPDF_PageObjectHolder::ParseState::kParsed,
              form->form()->GetParseState());
    while (page->GetParseState() !=
           CPDF_PageObjectHolder::ParseState::kParsed) {
      page->ContinueParse(&every_step);
    }
    EXPECT_EQ(CPDF_PageObjectHolder::ParseState::kParsed,
              form->form()->GetParseState());
    EXPECT_EQ(CFX_FloatRect(20, 20, 50, 50), form->GetRect());
  }
}

// The pages of real files, parsed both ways.
TEST_F(EPDFDeferredFormsEmbedderTest, Corpus) {
  const char* corpus = getenv("EPDF_DEFERRED_FORMS_CORPUS");
  if (!corpus || !*corpus) {
    GTEST_SKIP() << "set EPDF_DEFERRED_FORMS_CORPUS to PDF files or folders";
  }
  const char* max_pages_env = getenv("EPDF_DEFERRED_FORMS_MAX_PAGES");
  const int max_pages = max_pages_env ? atoi(max_pages_env) : 20;
  const char* skip_env = getenv("EPDF_DEFERRED_FORMS_SKIP");

  auto split = [](const std::string& list) {
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= list.size()) {
      size_t end = list.find(':', at);
      if (end == std::string::npos) {
        end = list.size();
      }
      if (end > at) {
        parts.push_back(list.substr(at, end - at));
      }
      at = end + 1;
    }
    return parts;
  };
  const std::vector<std::string> skips = split(skip_env ? skip_env : "");

  std::vector<std::string> files;
  std::vector<std::string> pending = split(corpus);
  while (!pending.empty()) {
    const std::string path = pending.back();
    pending.pop_back();
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
      continue;
    }
    if (S_ISDIR(info.st_mode)) {
      DIR* dir = opendir(path.c_str());
      if (!dir) {
        continue;
      }
      while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
          pending.push_back(path + "/" + name);
        }
      }
      closedir(dir);
      continue;
    }
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".pdf") {
      files.push_back(path);
    }
  }
  std::sort(files.begin(), files.end());

  int pages = 0;
  int forms_pages = 0;
  int unloadable_pages = 0;
  std::vector<std::string> differing;
  for (const std::string& file : files) {
    if (std::any_of(skips.begin(), skips.end(), [&](const std::string& skip) {
          return file.find(skip) != std::string::npos;
        })) {
      continue;
    }
    ScopedFPDFDocument straight(FPDF_LoadDocument(file.c_str(), nullptr));
    ScopedFPDFDocument deferred(FPDF_LoadDocument(file.c_str(), nullptr));
    if (!straight || !deferred) {
      continue;
    }
    const int count = std::min(FPDF_GetPageCount(straight.get()), max_pages);
    for (int i = 0; i < count; ++i) {
      std::cerr << "[deferred-forms] start " << file << " page " << i << "\n";
      const auto start = std::chrono::steady_clock::now();
      const Parsed expected = Load(straight.get(), i, false, false);
      const auto middle = std::chrono::steady_clock::now();
      const Parsed actual = Load(deferred.get(), i, true, false);
      const auto end = std::chrono::steady_clock::now();
      auto ms = [](auto from, auto to) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(to - from)
            .count();
      };
      ++pages;
      if (expected.tree.find(" form matrix=") != std::string::npos) {
        ++forms_pages;
      }
      if (!expected.loaded && !actual.loaded) {
        ++unloadable_pages;
        continue;
      }
      const bool same =
          expected.loaded == actual.loaded && expected.tree == actual.tree &&
          expected.pixels == actual.pixels &&
          expected.size.estimated_bytes == actual.size.estimated_bytes;
      if (!same) {
        std::string what;
        if (expected.tree != actual.tree) {
          size_t at = 0;
          while (at < expected.tree.size() &&
                 expected.tree[at] == actual.tree[at]) {
            ++at;
          }
          const size_t line = expected.tree.rfind('\n', at) + 1;
          what +=
              " tree at: " +
              expected.tree.substr(line, expected.tree.find('\n', at) - line) +
              " | " +
              actual.tree.substr(line, actual.tree.find('\n', at) - line);
        }
        if (expected.pixels != actual.pixels) {
          what += " pixels";
        }
        if (expected.size.estimated_bytes != actual.size.estimated_bytes) {
          what += " size";
        }
        differing.push_back(file + " page " + std::to_string(i) + ":" + what);
      }
      std::cerr << "[deferred-forms] " << (same ? "same " : "DIFFERS ") << file
                << " page " << i << " objects=" << expected.size.objects
                << " straight " << ms(start, middle) << " ms, deferred "
                << ms(middle, end) << " ms\n";
    }
  }
  std::cerr << "[deferred-forms] " << pages << " pages, " << forms_pages
            << " with forms, " << unloadable_pages << " that load neither way, "
            << differing.size() << " differ\n";
  for (const std::string& page : differing) {
    ADD_FAILURE() << "differs: " << page;
  }
}
