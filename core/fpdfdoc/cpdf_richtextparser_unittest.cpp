// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_richtextparser.h"

#include "core/fpdfdoc/cpdf_richtext.h"
#include "core/fxcrt/widestring.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

using Align = CPDF_RichTextParagraphProps::Align;
using Script = CPDF_RichTextStyle::Script;
using Code = CPDF_RichTextDiagnostic::Code;

// The /RC of the Acrobat 26 sample this work started from: five faces, a
// soft wrap, an Enter, per-span colours, a word underline, bold and italic.
constexpr const wchar_t kAcrobatSampleRC[] =
    LR"xml(<?xml version="1.0"?><body xmlns="http://www.w3.org/1999/xhtml" xmlns:xfa="http://www.xfa.org/schema/xfa-data/1.0/" xfa:APIVersion="Acrobat:26.2.0" xfa:spec="2.0.2"  style="font-size:22.0pt;text-align:left;color:#058A1B;font-weight:normal;font-style:normal;font-family:'Noto Sans';font-stretch:normal"><p dir="ltr"><span style="color:#FFC100">h</span><span style="color:#05891C">ello</span><span style="color:#FFC100"> </span><span style="text-decoration:word;color:#FFC100;font-weight:bold;font-family:Helvetica">how</span><span style="color:#FFC100"> </span><span style="color:#FFC100;font-family:'Hubot Sans Condensed Light'">t</span><span style="color:#FFC100;font-style:italic;font-family:'Hubot Sans Condensed Light'">est</span><span style="color:#FFC100;font-family:'Hubot Sans Condensed Light'">&#13;</span><span style="color:#FFC100;font-family:'Minion Pro'">po</span><span style="color:#FFC100;font-weight:bold;font-family:'Minion Pro'">p</span><span style="color:#FFC100;font-family:'Minion Pro'">ej</span><span style="text-decoration:word;color:#FFC100;font-family:'Minion Pro'">es</span></p></body>)xml";

CPDF_RichTextStyle Defaults() {
  CPDF_RichTextStyle defaults;
  defaults.family = L"Helvetica";
  defaults.size = 12.0f;
  return defaults;
}

CPDF_RichTextDocument Parse(const wchar_t* rc) {
  return CPDF_RichTextParser::ParseRichContent(
      WideString(rc), Defaults(), CPDF_RichTextParagraphProps(), L"fallback");
}

bool HasDiagnostic(const CPDF_RichTextDocument& doc, Code code) {
  for (const CPDF_RichTextDiagnostic& d : doc.diagnostics) {
    if (d.code == code) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(CPDF_RichTextParserTest, AcrobatSampleRuns) {
  CPDF_RichTextDocument doc = Parse(kAcrobatSampleRC);
  EXPECT_EQ(CPDF_RichTextDocument::Source::kRC, doc.source);
  EXPECT_EQ(L"Noto Sans", doc.body.family);
  EXPECT_FLOAT_EQ(22.0f, doc.body.size);
  EXPECT_EQ(0xFF058A1Bu, doc.body.color);
  EXPECT_EQ(400, doc.body.weight);
  EXPECT_FALSE(doc.body.italic);
  EXPECT_EQ(Align::kLeft, doc.body_paragraph.align);
  EXPECT_TRUE(doc.diagnostics.empty());

  ASSERT_EQ(1u, doc.paragraphs.size());
  const CPDF_RichTextParagraph& p = doc.paragraphs[0];
  EXPECT_FALSE(p.props.rtl);
  ASSERT_EQ(12u, p.runs.size());
  EXPECT_EQ(L"h", p.runs[0].text);
  EXPECT_EQ(0xFFFFC100u, p.runs[0].style.color.value());
  EXPECT_EQ(L"ello", p.runs[1].text);
  EXPECT_EQ(0xFF05891Cu, p.runs[1].style.color.value());
  EXPECT_EQ(L" ", p.runs[2].text);

  const CPDF_RichTextRun& how = p.runs[3];
  EXPECT_EQ(L"how", how.text);
  EXPECT_EQ(700, how.style.weight.value());
  EXPECT_EQ(L"Helvetica", how.style.family.value());
  EXPECT_EQ(CPDF_RichTextStyle::kWordUnderline, how.style.decoration.value());
  EXPECT_FALSE(how.style.italic.has_value());

  EXPECT_EQ(L"t", p.runs[5].text);
  EXPECT_EQ(L"Hubot Sans Condensed Light", p.runs[5].style.family.value());
  EXPECT_EQ(L"est", p.runs[6].text);
  EXPECT_TRUE(p.runs[6].style.italic.value());
  EXPECT_EQ(L"\r", p.runs[7].text);  // Enter is a run, inside the paragraph
  EXPECT_EQ(L"po", p.runs[8].text);
  EXPECT_EQ(L"Minion Pro", p.runs[8].style.family.value());
  EXPECT_EQ(700, p.runs[9].style.weight.value());
  EXPECT_EQ(L"es", p.runs[11].text);
  EXPECT_EQ(CPDF_RichTextStyle::kWordUnderline,
            p.runs[11].style.decoration.value());

  // Resolution against the body: the cascade is applied at layout time.
  CPDF_RichTextStyle resolved = ApplyRichTextStyleDelta(doc.body, how.style);
  EXPECT_EQ(L"Helvetica", resolved.family);
  EXPECT_EQ(700, resolved.weight);
  EXPECT_FLOAT_EQ(22.0f, resolved.size);
  EXPECT_EQ(0xFFFFC100u, resolved.color);
}

TEST(CPDF_RichTextParserTest, AcrobatDecorationsAndBareText) {
  // Experiment 05: bold / word underline / italic / line-through, bare text
  // between spans.
  CPDF_RichTextDocument doc = Parse(
      LR"xml(<?xml version="1.0"?><body xmlns="http://www.w3.org/1999/xhtml" style="font-size:24.0pt;text-align:left;color:#000000;font-weight:normal;font-style:normal;font-family:Helvetica;font-stretch:normal"><p dir="ltr"><span style="font-weight:bold">hello</span> <span style="text-decoration:word">how</span> <span style="font-style:italic">are</span> you <span style="text-decoration:line-through">doing?</span></p></body>)xml");
  ASSERT_EQ(1u, doc.paragraphs.size());
  const auto& runs = doc.paragraphs[0].runs;
  ASSERT_EQ(7u, runs.size());
  EXPECT_EQ(L"hello", runs[0].text);
  EXPECT_EQ(700, runs[0].style.weight.value());
  EXPECT_EQ(L" ", runs[1].text);
  EXPECT_TRUE(runs[1].style.IsEmpty());
  EXPECT_EQ(L"how", runs[2].text);
  EXPECT_EQ(CPDF_RichTextStyle::kWordUnderline,
            runs[2].style.decoration.value());
  EXPECT_EQ(L"are", runs[4].text);
  EXPECT_TRUE(runs[4].style.italic.value());
  EXPECT_EQ(L" you ", runs[5].text);
  EXPECT_EQ(L"doing?", runs[6].text);
  EXPECT_EQ(CPDF_RichTextStyle::kLineThrough, runs[6].style.decoration.value());
}

TEST(CPDF_RichTextParserTest, AcrobatTextAlignOnSpanAndSizeDelta) {
  // Experiment 07: alignment written on the spans, two Enters, a size change.
  CPDF_RichTextDocument doc = Parse(
      LR"xml(<?xml version="1.0"?><body style="font-size:26.0pt;text-align:left;color:#000000;font-weight:bold;font-style:normal;font-family:Helvetica;font-stretch:normal"><p dir="ltr"><span style="text-align:center">Welcome&#13;&#13;</span><span style="font-size:18.0pt;text-align:center;font-weight:normal">I want to tell you something about Adobe</span></p></body>)xml");
  EXPECT_EQ(700, doc.body.weight);
  EXPECT_FLOAT_EQ(26.0f, doc.body.size);
  ASSERT_EQ(1u, doc.paragraphs.size());
  EXPECT_EQ(Align::kCenter, doc.paragraphs[0].props.align);  // hoisted
  ASSERT_EQ(2u, doc.paragraphs[0].runs.size());
  EXPECT_EQ(L"Welcome\r\r", doc.paragraphs[0].runs[0].text);
  EXPECT_FLOAT_EQ(18.0f, doc.paragraphs[0].runs[1].style.size.value());
  EXPECT_EQ(400, doc.paragraphs[0].runs[1].style.weight.value());
}

TEST(CPDF_RichTextParserTest, AcrobatSignOnlyVerticalAlign) {
  CPDF_RichTextDocument doc = Parse(
      LR"xml(<body style="font-size:18.0pt;font-family:Helvetica"><p dir="ltr">test<span style="vertical-align:-0.0pt">hello&#13;</span>&#13;test<span style="vertical-align:+0.0pt">hello</span></p></body>)xml");
  ASSERT_EQ(1u, doc.paragraphs.size());
  const auto& runs = doc.paragraphs[0].runs;
  // "&#13;test" is one XML text node, so one run: the break and the word.
  ASSERT_EQ(4u, runs.size());
  EXPECT_EQ(L"test", runs[0].text);
  EXPECT_EQ(L"hello\r", runs[1].text);
  EXPECT_EQ(Script::kSub, runs[1].style.script.value());
  EXPECT_EQ(L"\rtest", runs[2].text);
  EXPECT_TRUE(runs[2].style.IsEmpty());
  EXPECT_EQ(L"hello", runs[3].text);
  EXPECT_EQ(Script::kSuper, runs[3].style.script.value());
}

TEST(CPDF_RichTextParserTest, DefaultStyleFontShorthand) {
  CPDF_RichTextParagraphProps paragraph;
  std::vector<CPDF_RichTextDiagnostic> diagnostics;
  CPDF_RichTextStyleDelta ds = CPDF_RichTextParser::ParseDefaultStyle(
      L"font: bold Helvetica,sans-serif 24.0pt; text-align:left; "
      L"color:#FFFFFF ",
      &paragraph, &diagnostics);
  EXPECT_EQ(L"Helvetica", ds.family.value());
  EXPECT_EQ(700, ds.weight.value());
  EXPECT_FLOAT_EQ(24.0f, ds.size.value());
  EXPECT_EQ(0xFFFFFFFFu, ds.color.value());
  EXPECT_EQ(Align::kLeft, paragraph.align);
  EXPECT_TRUE(diagnostics.empty());

  ds = CPDF_RichTextParser::ParseDefaultStyle(
      L"font: 'Noto Sans',sans-serif 22.0pt; text-align:center; color:#FF6200",
      &paragraph, &diagnostics);
  EXPECT_EQ(L"Noto Sans", ds.family.value());
  EXPECT_FALSE(ds.weight.has_value());
  EXPECT_FLOAT_EQ(22.0f, ds.size.value());
  EXPECT_EQ(Align::kCenter, paragraph.align);

  ds = CPDF_RichTextParser::ParseDefaultStyle(
      L"font: 'Minion Pro',serif 18.0pt;font-stretch:Normal", &paragraph,
      &diagnostics);
  EXPECT_EQ(L"Minion Pro", ds.family.value());
  EXPECT_TRUE(diagnostics.empty());  // font-stretch is known and ignored
}

TEST(CPDF_RichTextParserTest, ElementFormsAndBreaks) {
  CPDF_RichTextDocument doc = Parse(
      LR"xml(<body><p><b>x</b><i>y</i><sub>z</sub><sup>w</sup><br/>v</p><p>second</p></body>)xml");
  ASSERT_EQ(2u, doc.paragraphs.size());
  const auto& runs = doc.paragraphs[0].runs;
  ASSERT_EQ(6u, runs.size());
  EXPECT_EQ(700, runs[0].style.weight.value());
  EXPECT_TRUE(runs[1].style.italic.value());
  EXPECT_EQ(Script::kSub, runs[2].style.script.value());
  EXPECT_EQ(Script::kSuper, runs[3].style.script.value());
  EXPECT_EQ(L"\r", runs[4].text);
  EXPECT_EQ(L"v", runs[5].text);
  EXPECT_EQ(L"second", doc.paragraphs[1].runs[0].text);
  EXPECT_TRUE(doc.diagnostics.empty());
}

TEST(CPDF_RichTextParserTest, ListsDegradeToParagraphsWithMarkers) {
  CPDF_RichTextDocument doc = Parse(
      LR"xml(<body><ol><li>a</li><li>b</li></ol><ul><li>c</li></ul></body>)xml");
  ASSERT_EQ(3u, doc.paragraphs.size());
  ASSERT_EQ(2u, doc.paragraphs[0].runs.size());
  EXPECT_EQ(L"1.  ", doc.paragraphs[0].runs[0].text);
  EXPECT_EQ(L"a", doc.paragraphs[0].runs[1].text);
  EXPECT_EQ(L"2.  ", doc.paragraphs[1].runs[0].text);
  EXPECT_EQ(L"\x00b7  ", doc.paragraphs[2].runs[0].text);
  EXPECT_TRUE(HasDiagnostic(doc, Code::kListDegraded));
}

TEST(CPDF_RichTextParserTest, MalformedRCFallsBackToContents) {
  CPDF_RichTextDocument doc = Parse(L"this is <<not xml");
  EXPECT_EQ(CPDF_RichTextDocument::Source::kContents, doc.source);
  ASSERT_EQ(1u, doc.paragraphs.size());
  ASSERT_EQ(1u, doc.paragraphs[0].runs.size());
  EXPECT_EQ(L"fallback", doc.paragraphs[0].runs[0].text);
  EXPECT_TRUE(HasDiagnostic(doc, Code::kMalformedRC));
  EXPECT_EQ(L"Helvetica", doc.body.family);  // defaults survive
}

TEST(CPDF_RichTextParserTest, UnknownDeclarationsAndTagsAreKeptAndReported) {
  CPDF_RichTextDocument doc = Parse(
      LR"xml(<body><p><span style="kerning-mode:pair;color:#FF0000;xfa-tab-stops:left 2in;foo-bar:1">x</span><div>y</div></p></body>)xml");
  ASSERT_EQ(1u, doc.paragraphs.size());
  ASSERT_EQ(2u, doc.paragraphs[0].runs.size());
  EXPECT_EQ(0xFFFF0000u, doc.paragraphs[0].runs[0].style.color.value());
  // Recognised-but-unmodelled and truly unknown declarations both survive
  // verbatim; only the truly unknown one is reported.
  EXPECT_EQ(L"kerning-mode:pair;xfa-tab-stops:left 2in;foo-bar:1",
            doc.paragraphs[0].runs[0].style.unknown_declarations);
  size_t unknown_reports = 0;
  for (const CPDF_RichTextDiagnostic& d : doc.diagnostics) {
    if (d.code == Code::kUnknownDeclaration) {
      ++unknown_reports;
      EXPECT_EQ("foo-bar", d.detail);
    }
  }
  EXPECT_EQ(1u, unknown_reports);
  EXPECT_EQ(L"y", doc.paragraphs[0].runs[1].text);
  EXPECT_TRUE(HasDiagnostic(doc, Code::kUnknownDeclaration));
  EXPECT_TRUE(HasDiagnostic(doc, Code::kUnsupportedTag));
}

TEST(CPDF_RichTextParserTest, WhitespaceIsLiteralExceptPrettyPrinting) {
  CPDF_RichTextDocument doc =
      Parse(L"<body>\n  <p>a  b</p>\n  <p> c </p>\n</body>");
  ASSERT_EQ(2u, doc.paragraphs.size());
  EXPECT_EQ(L"a  b", doc.paragraphs[0].runs[0].text);
  EXPECT_EQ(L" c ", doc.paragraphs[1].runs[0].text);
}

TEST(CPDF_RichTextParserTest, Values) {
  CPDF_RichTextStyleDelta style;
  CPDF_RichTextParagraphProps paragraph;
  std::vector<CPDF_RichTextDiagnostic> diagnostics;
  CPDF_RichTextParser::ParseInlineStyle(
      L"font-size:16px;color:rgb(255, 0, "
      L"0);font-weight:600;letter-spacing:1.5pt;"
      L"xfa-font-horizontal-scale:80%;text-decoration:underline line-through;"
      L"line-height:1.5;margin-left:12pt;text-align:justify",
      &style, &paragraph, &diagnostics);
  EXPECT_FLOAT_EQ(12.0f, style.size.value());  // 16px = 12pt
  EXPECT_EQ(0xFFFF0000u, style.color.value());
  EXPECT_EQ(600, style.weight.value());
  EXPECT_FLOAT_EQ(1.5f, style.letter_spacing.value());
  EXPECT_FLOAT_EQ(0.8f, style.horz_scale.value());
  EXPECT_EQ(CPDF_RichTextStyle::kUnderline | CPDF_RichTextStyle::kLineThrough,
            style.decoration.value());
  EXPECT_FLOAT_EQ(18.0f, paragraph.line_height.value());  // 1.5 × 12pt base
  EXPECT_FLOAT_EQ(12.0f, paragraph.margin_left);
  EXPECT_EQ(Align::kJustify, paragraph.align);
  EXPECT_TRUE(diagnostics.empty());

  CPDF_RichTextParser::ParseInlineStyle(L"color:#12g;font-size:big", &style,
                                        &paragraph, &diagnostics);
  EXPECT_EQ(2u, diagnostics.size());
  EXPECT_EQ(Code::kInvalidValue, diagnostics[0].code);
}

TEST(CPDF_RichTextParserTest, PlainTextSplitsParagraphsOnBreaks) {
  CPDF_RichTextDocument doc = CPDF_RichTextParser::FromPlainText(
      L"one\r\ntwo\nthree", Defaults(), CPDF_RichTextParagraphProps());
  EXPECT_EQ(CPDF_RichTextDocument::Source::kContents, doc.source);
  ASSERT_EQ(3u, doc.paragraphs.size());
  EXPECT_EQ(L"one", doc.paragraphs[0].runs[0].text);
  EXPECT_EQ(L"two", doc.paragraphs[1].runs[0].text);
  EXPECT_EQ(L"three", doc.paragraphs[2].runs[0].text);
  EXPECT_TRUE(doc.paragraphs[0].runs[0].style.IsEmpty());
}

TEST(CPDF_RichTextParserTest, JsonShape) {
  CPDF_RichTextDocument doc = Parse(kAcrobatSampleRC);
  ByteString json = CPDF_RichTextParser::ToJSON(doc);
  EXPECT_TRUE(json.Contains("\"source\":\"rc\""));
  EXPECT_TRUE(json.Contains("\"family\":\"Noto Sans\""));
  EXPECT_TRUE(json.Contains("\"size\":22"));
  EXPECT_TRUE(json.Contains("\"color\":\"#058A1B\""));
  EXPECT_TRUE(json.Contains(
      "{\"text\":\"how\",\"style\":{\"family\":\"Helvetica\",\"weight\":700,"
      "\"color\":\"#FFC100\",\"decoration\":[\"word\"]}}"));
  EXPECT_TRUE(json.Contains("{\"text\":\"\\r\",\"style\":"));
  EXPECT_TRUE(json.Contains("\"diagnostics\":[]"));
  EXPECT_TRUE(json.Contains("\"align\":\"left\",\"dir\":\"ltr\""));  // the body

  // A paragraph names only what differs from the body.
  CPDF_RichTextDocument aligned;
  aligned.body_paragraph.align = CPDF_RichTextParagraphProps::Align::kCenter;
  CPDF_RichTextParagraph same;
  same.props = aligned.body_paragraph;
  same.runs.push_back({L"same", {}});
  CPDF_RichTextParagraph right;
  right.props = aligned.body_paragraph;
  right.props.align = CPDF_RichTextParagraphProps::Align::kRight;
  right.runs.push_back({L"right", {}});
  CPDF_RichTextParagraph rtl;
  rtl.props = aligned.body_paragraph;
  rtl.props.rtl = true;
  rtl.runs.push_back({L"rtl", {}});
  aligned.paragraphs = {same, right, rtl};
  const ByteString aligned_json = CPDF_RichTextParser::ToJSON(aligned);
  EXPECT_TRUE(aligned_json.Contains("\"align\":\"center\",\"dir\":\"ltr\"}"));
  EXPECT_TRUE(aligned_json.Contains("\"paragraphs\":[{\"runs\":[{\"text\":\"same\"}]}"));
  EXPECT_TRUE(aligned_json.Contains("{\"align\":\"right\",\"runs\":"));
  EXPECT_TRUE(aligned_json.Contains("{\"dir\":\"rtl\",\"runs\":"));

  CPDF_RichTextDocument plain = CPDF_RichTextParser::FromPlainText(
      L"a \"quoted\" \\ line\ttab", Defaults(), CPDF_RichTextParagraphProps());
  EXPECT_TRUE(CPDF_RichTextParser::ToJSON(plain).Contains(
      "{\"text\":\"a \\\"quoted\\\" \\\\ line\\ttab\"}"));
}
