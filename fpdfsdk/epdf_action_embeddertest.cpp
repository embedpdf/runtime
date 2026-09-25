// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_action.h"

#include <memory>
#include <string>
#include <vector>

#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/epdf_form.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_doc.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_javascript.h"
#include "testing/embedder_test.h"
#include "testing/fx_string_testhelpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

namespace {

struct ActionModelDeleter {
  void operator()(EPDF_ACTION_MODEL model) const {
    EPDFAction_CloseModel(model);
  }
};

using ScopedEPDFActionModel =
    std::unique_ptr<epdf_action_model_t__, ActionModelDeleter>;

std::wstring GetActionJavaScript(EPDF_ACTION_MODEL model,
                                 EPDF_ACTION_NODE_ID node) {
  const unsigned long length =
      EPDFAction_GetNodeJavaScript(model, node, nullptr, 0);
  if (length == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length,
            EPDFAction_GetNodeJavaScript(model, node, buffer.data(), length));
  return GetPlatformWString(buffer.data());
}

std::wstring GetLegacyJavaScript(FPDF_JAVASCRIPT_ACTION action) {
  const unsigned long length =
      FPDFJavaScriptAction_GetScript(action, nullptr, 0);
  if (length == 0) {
    return std::wstring();
  }
  std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
  EXPECT_EQ(length,
            FPDFJavaScriptAction_GetScript(action, buffer.data(), length));
  return GetPlatformWString(buffer.data());
}

std::string GetActionSubtype(EPDF_ACTION_MODEL model,
                             EPDF_ACTION_NODE_ID node) {
  const unsigned long length =
      EPDFAction_GetNodeSubtype(model, node, nullptr, 0);
  if (length == 0) {
    return std::string();
  }
  std::vector<char> buffer(length);
  EXPECT_EQ(length,
            EPDFAction_GetNodeSubtype(model, node, buffer.data(), length));
  return std::string(buffer.data());
}

RetainPtr<CPDF_Dictionary> MakeJavaScriptAction(const wchar_t* script) {
  auto action = pdfium::MakeRetain<CPDF_Dictionary>();
  action->SetNewFor<CPDF_Name>("S", "JavaScript");
  action->SetNewFor<CPDF_String>("JS", script);
  return action;
}

std::string GetTargetName(EPDF_ACTION_MODEL model,
                          EPDF_ACTION_NODE_ID node,
                          int index) {
  const unsigned long length =
      EPDFAction_GetNodeTargetName(model, node, index, nullptr, 0);
  if (length == 0) {
    return std::string();
  }
  std::vector<char> buffer(length);
  EXPECT_EQ(length, EPDFAction_GetNodeTargetName(model, node, index,
                                                 buffer.data(), length));
  return std::string(buffer.data());
}

}  // namespace

class EPDFActionEmbedderTest : public EmbedderTest {};

TEST_F(EPDFActionEmbedderTest, InvalidArguments) {
  EXPECT_EQ(EPDF_ACTION_NODE_INVALID, EPDFAction_GetRootNode(nullptr));
  EXPECT_EQ(0, EPDFAction_GetNodeCount(nullptr));
  EXPECT_FALSE(EPDFAction_IsComplete(nullptr));
  EXPECT_FALSE(EPDFDoc_GetOpenActionModel(nullptr));
  EXPECT_FALSE(EPDFDoc_GetAdditionalActionModel(
      nullptr, EPDF_DOCUMENT_ACTION_WILL_CLOSE));
  EXPECT_FALSE(EPDFDoc_GetPageActionModel(nullptr, 1, EPDF_PAGE_ACTION_OPEN));
  EXPECT_FALSE(EPDFAnnot_GetActionModel(nullptr, EPDF_ANNOT_ACTION_ACTIVATE));
}

TEST_F(EPDFActionEmbedderTest, NamedJavaScriptIndexPairing) {
  ASSERT_TRUE(OpenDocument("js.pdf"));
  ASSERT_EQ(5, FPDFDoc_GetJavaScriptActionCount(document()));

  for (int index = 0; index < 5; ++index) {
    ScopedFPDFJavaScriptAction legacy(
        FPDFDoc_GetJavaScriptAction(document(), index));
    ScopedEPDFActionModel model(
        EPDFDoc_GetNamedJavaScriptActionModel(document(), index));
    ASSERT_EQ(!!legacy, !!model) << "index " << index;
    if (!legacy) {
      continue;
    }
    const EPDF_ACTION_NODE_ID root = EPDFAction_GetRootNode(model.get());
    ASSERT_NE(EPDF_ACTION_NODE_INVALID, root);
    EXPECT_EQ(EPDF_ACTION_TYPE_JAVASCRIPT,
              EPDFAction_GetNodeType(model.get(), root));
    EXPECT_TRUE(EPDFAction_NodeHasJavaScript(model.get(), root));
    EXPECT_EQ(GetLegacyJavaScript(legacy.get()),
              GetActionJavaScript(model.get(), root));
  }

  EXPECT_FALSE(EPDFDoc_GetNamedJavaScriptActionModel(document(), -1));
  EXPECT_FALSE(EPDFDoc_GetNamedJavaScriptActionModel(document(), 5));
}

TEST_F(EPDFActionEmbedderTest, NextRenditionCycleAndMalformedEntry) {
  ScopedFPDFDocument document(FPDF_CreateNewDocument());
  ASSERT_TRUE(document);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document.get());
  ASSERT_TRUE(doc);

  RetainPtr<CPDF_Dictionary> root = doc->NewIndirect<CPDF_Dictionary>();
  root->SetNewFor<CPDF_Name>("S", "JavaScript");
  root->SetNewFor<CPDF_String>("JS", L"root();");

  RetainPtr<CPDF_Dictionary> rendition = doc->NewIndirect<CPDF_Dictionary>();
  rendition->SetNewFor<CPDF_Name>("S", "Rendition");
  const ByteString rendition_script = "rendition();";
  RetainPtr<CPDF_Stream> stream =
      doc->NewIndirect<CPDF_Stream>(rendition_script.unsigned_span());
  rendition->SetNewFor<CPDF_Reference>("JS", doc, stream->GetObjNum());
  rendition->SetNewFor<CPDF_Reference>("Next", doc, root->GetObjNum());

  RetainPtr<CPDF_Dictionary> future = doc->NewIndirect<CPDF_Dictionary>();
  future->SetNewFor<CPDF_Name>("S", "FutureAction");
  RetainPtr<CPDF_Array> malformed_next = future->SetNewFor<CPDF_Array>("Next");
  malformed_next->AppendNew<CPDF_Number>(7);

  RetainPtr<CPDF_Array> next = root->SetNewFor<CPDF_Array>("Next");
  next->AppendNew<CPDF_Reference>(doc, rendition->GetObjNum());
  next->AppendNew<CPDF_Reference>(doc, future->GetObjNum());

  ScopedEPDFActionModel model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(root.Get())));
  ASSERT_TRUE(model);
  ASSERT_EQ(3, EPDFAction_GetNodeCount(model.get()));
  const EPDF_ACTION_NODE_ID model_root = EPDFAction_GetRootNode(model.get());
  ASSERT_EQ(0u, model_root);
  ASSERT_EQ(2, EPDFAction_GetNextCount(model.get(), model_root));

  const EPDF_ACTION_NODE_ID rendition_node =
      EPDFAction_GetNextAt(model.get(), model_root, 0);
  ASSERT_NE(EPDF_ACTION_NODE_INVALID, rendition_node);
  EXPECT_EQ(EPDF_ACTION_TYPE_RENDITION,
            EPDFAction_GetNodeType(model.get(), rendition_node));
  EXPECT_TRUE(EPDFAction_NodeHasJavaScript(model.get(), rendition_node));
  EXPECT_EQ(L"rendition();", GetActionJavaScript(model.get(), rendition_node));
  EXPECT_EQ(0, EPDFAction_GetNextCount(model.get(), rendition_node));

  const EPDF_ACTION_NODE_ID future_node =
      EPDFAction_GetNextAt(model.get(), model_root, 1);
  ASSERT_NE(EPDF_ACTION_NODE_INVALID, future_node);
  EXPECT_EQ(EPDF_ACTION_TYPE_UNKNOWN,
            EPDFAction_GetNodeType(model.get(), future_node));
  EXPECT_EQ("FutureAction", GetActionSubtype(model.get(), future_node));
  EXPECT_FALSE(EPDFAction_NodeHasJavaScript(model.get(), future_node));

  const uint32_t warnings = EPDFAction_GetWarningFlags(model.get());
  EXPECT_TRUE(warnings & EPDF_ACTION_WARNING_CYCLE_DROPPED);
  EXPECT_TRUE(warnings & EPDF_ACTION_WARNING_MALFORMED_NEXT);
  EXPECT_FALSE(warnings & EPDF_ACTION_WARNING_INCOMPLETE);
  EXPECT_TRUE(EPDFAction_IsComplete(model.get()));
}

TEST_F(EPDFActionEmbedderTest, DepthLimitMarksModelIncomplete) {
  RetainPtr<CPDF_Dictionary> root = MakeJavaScriptAction(L"root();");
  RetainPtr<CPDF_Dictionary> current = root;
  for (int i = 0; i < 64; ++i) {
    RetainPtr<CPDF_Dictionary> child = MakeJavaScriptAction(L"next();");
    current->SetFor("Next", child);
    current = std::move(child);
  }

  ScopedEPDFActionModel model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(root.Get())));
  ASSERT_TRUE(model);
  EXPECT_EQ(64, EPDFAction_GetNodeCount(model.get()));
  EXPECT_TRUE(EPDFAction_GetWarningFlags(model.get()) &
              EPDF_ACTION_WARNING_INCOMPLETE);
  EXPECT_FALSE(EPDFAction_IsComplete(model.get()));
}

TEST_F(EPDFActionEmbedderTest, DocumentAndPageModelsAreDetached) {
  ScopedFPDFDocument document(FPDF_CreateNewDocument());
  ASSERT_TRUE(document);
  ScopedFPDFPage page(FPDFPage_New(document.get(), 0, 300, 300));
  ASSERT_TRUE(page);

  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document.get());
  ASSERT_TRUE(doc);
  RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
  ASSERT_TRUE(root);
  root->SetFor("OpenAction", MakeJavaScriptAction(L"open();"));
  RetainPtr<CPDF_Dictionary> document_aa =
      root->SetNewFor<CPDF_Dictionary>("AA");
  document_aa->SetFor("WS", MakeJavaScriptAction(L"willSave();"));

  CPDF_Page* cpdf_page = CPDFPageFromFPDFPage(page.get());
  ASSERT_TRUE(cpdf_page);
  RetainPtr<CPDF_Dictionary> page_dict = cpdf_page->GetMutableDict();
  ASSERT_TRUE(page_dict);
  RetainPtr<CPDF_Dictionary> page_aa =
      page_dict->SetNewFor<CPDF_Dictionary>("AA");
  page_aa->SetFor("O", MakeJavaScriptAction(L"pageOpen();"));
  const uint32_t page_objnum = EPDFPage_GetObjectNumber(page.get());
  ASSERT_GT(page_objnum, 0u);

  ScopedEPDFActionModel open(EPDFDoc_GetOpenActionModel(document.get()));
  ScopedEPDFActionModel will_save(EPDFDoc_GetAdditionalActionModel(
      document.get(), EPDF_DOCUMENT_ACTION_WILL_SAVE));
  ScopedEPDFActionModel page_open(EPDFDoc_GetPageActionModel(
      document.get(), page_objnum, EPDF_PAGE_ACTION_OPEN));
  ASSERT_TRUE(open);
  ASSERT_TRUE(will_save);
  ASSERT_TRUE(page_open);
  EXPECT_FALSE(EPDFDoc_GetAdditionalActionModel(document.get(), -1));
  EXPECT_FALSE(EPDFDoc_GetPageActionModel(document.get(), page_objnum, 9));

  page.reset();
  document.reset();

  EXPECT_EQ(L"open();", GetActionJavaScript(open.get(), 0));
  EXPECT_EQ(L"willSave();", GetActionJavaScript(will_save.get(), 0));
  EXPECT_EQ(L"pageOpen();", GetActionJavaScript(page_open.get(), 0));
}

TEST_F(EPDFActionEmbedderTest, LayerActionReadsDoNotPromote) {
  const std::string path =
      PathService::GetTestFilePath("annots_action_handling.pdf");
  ASSERT_FALSE(path.empty());
  const std::vector<uint8_t> bytes = GetFileContents(path.c_str());
  ASSERT_FALSE(bytes.empty());
  EPDF_BASE_DOCUMENT base = EPDF_LoadMemBaseDocument(
      bytes.data(), static_cast<int>(bytes.size()), nullptr);
  ASSERT_TRUE(base);
  EPDFLayerOpenStatus status;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status));
  ASSERT_TRUE(layer);
  ASSERT_EQ(EPDFLayerOpenStatus_kSuccess, status);
  ASSERT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(layer.get()));

  bool found_action = false;
  const int script_count = FPDFDoc_GetJavaScriptActionCount(layer.get());
  ASSERT_GE(script_count, 0);
  for (int index = 0; index < script_count; ++index) {
    ScopedEPDFActionModel action(
        EPDFDoc_GetNamedJavaScriptActionModel(layer.get(), index));
    found_action = found_action || !!action;
  }
  for (int event = EPDF_DOCUMENT_ACTION_WILL_CLOSE;
       event <= EPDF_DOCUMENT_ACTION_DID_PRINT; ++event) {
    ScopedEPDFActionModel action(
        EPDFDoc_GetAdditionalActionModel(layer.get(), event));
    found_action = found_action || !!action;
  }
  ScopedEPDFActionModel open(EPDFDoc_GetOpenActionModel(layer.get()));
  found_action = found_action || !!open;

  const int page_count = FPDF_GetPageCount(layer.get());
  for (int page_index = 0; page_index < page_count; ++page_index) {
    ScopedFPDFPage page(FPDF_LoadPage(layer.get(), page_index));
    ASSERT_TRUE(page);
    const uint32_t page_objnum = EPDFPage_GetObjectNumber(page.get());
    for (int event = EPDF_PAGE_ACTION_OPEN; event <= EPDF_PAGE_ACTION_CLOSE;
         ++event) {
      ScopedEPDFActionModel action(
          EPDFDoc_GetPageActionModel(layer.get(), page_objnum, event));
      found_action = found_action || !!action;
    }
    const int annotation_count = FPDFPage_GetAnnotCount(page.get());
    for (int annotation_index = 0; annotation_index < annotation_count;
         ++annotation_index) {
      ScopedFPDFAnnotation annotation(
          FPDFPage_GetAnnot(page.get(), annotation_index));
      ASSERT_TRUE(annotation);
      for (int event = EPDF_ANNOT_ACTION_ACTIVATE;
           event <= EPDF_ANNOT_ACTION_PAGE_INVISIBLE; ++event) {
        ScopedEPDFActionModel action(
            EPDFAnnot_GetActionModel(annotation.get(), event));
        found_action = found_action || !!action;
      }
    }
  }
  EXPECT_TRUE(found_action);
  EXPECT_EQ(0ul, EPDFLayer_GetPromotedObjectCount(layer.get()));

  layer.reset();
  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFActionEmbedderTest,
       NamedJavaScriptModelReadsPromotedActionFromLayer) {
  const std::string path = PathService::GetTestFilePath("js.pdf");
  ASSERT_FALSE(path.empty());
  const std::vector<uint8_t> bytes = GetFileContents(path.c_str());
  ASSERT_FALSE(bytes.empty());
  EPDF_BASE_DOCUMENT base = EPDF_LoadMemBaseDocument(
      bytes.data(), static_cast<int>(bytes.size()), nullptr);
  ASSERT_TRUE(base);
  EPDFLayerOpenStatus status;
  ScopedFPDFDocument layer(
      EPDFLayer_OpenLayer(base, nullptr, nullptr, &status));
  ASSERT_TRUE(layer);
  ASSERT_EQ(EPDFLayerOpenStatus_kSuccess, status);

  CPDF_Document* layer_doc =
      CPDFDocumentFromFPDFDocument(layer.get());
  ASSERT_TRUE(layer_doc);
  RetainPtr<CPDF_Dictionary> action =
      ToDictionary(layer_doc->GetMutableIndirectObject(5u));
  ASSERT_TRUE(action);
  action->SetNewFor<CPDF_String>("JS", L"layer();");
  EXPECT_TRUE(EPDFLayer_IsObjectPromoted(layer.get(), 5u));

  ScopedEPDFActionModel model(
      EPDFDoc_GetNamedJavaScriptActionModel(layer.get(), 0));
  ASSERT_TRUE(model);
  EXPECT_EQ(L"layer();", GetActionJavaScript(model.get(), 0));

  layer.reset();
  EPDF_ReleaseBaseDocument(base);
}

TEST_F(EPDFActionEmbedderTest,
       MergedFieldAndWidgetAdditionalActionsStaySeparate) {
  ASSERT_TRUE(OpenDocument("text_form.pdf"));
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(doc);
  RetainPtr<CPDF_Dictionary> merged =
      ToDictionary(doc->GetMutableIndirectObject(4));
  ASSERT_TRUE(merged);
  merged->SetNewFor<CPDF_String>("DV", L"default text");
  RetainPtr<CPDF_Dictionary> aa = merged->SetNewFor<CPDF_Dictionary>("AA");
  aa->SetFor("V", MakeJavaScriptAction(L"validate();"));
  aa->SetFor("C", MakeJavaScriptAction(L"calculate();"));
  aa->SetFor("Fo", MakeJavaScriptAction(L"focus();"));
  aa->SetFor("E", MakeJavaScriptAction(L"enter();"));

  RetainPtr<CPDF_Dictionary> acro_form =
      doc->GetMutableRoot()->GetMutableDictFor("AcroForm");
  ASSERT_TRUE(acro_form);
  RetainPtr<CPDF_Array> calculation_order =
      acro_form->SetNewFor<CPDF_Array>("CO");
  calculation_order->AppendNew<CPDF_Reference>(doc, merged->GetObjNum());

  EPDF_FORM_MODEL form = EPDFForm_LoadModel(document());
  ASSERT_TRUE(form);
  ASSERT_EQ(1, EPDFForm_CountFields(form));
  EXPECT_EQ(L"default text", [&]() {
    const unsigned long length =
        EPDFForm_GetFieldDefaultValueAt(form, 0, 0, nullptr, 0);
    std::vector<FPDF_WCHAR> buffer = GetFPDFWideStringBuffer(length);
    EXPECT_EQ(length, EPDFForm_GetFieldDefaultValueAt(form, 0, 0, buffer.data(),
                                                      length));
    return GetPlatformWString(buffer.data());
  }());
  ASSERT_EQ(1, EPDFForm_CountCalculationOrder(form));
  EXPECT_EQ(0, EPDFForm_GetCalculationOrderFieldIndex(form, 0));

  ScopedEPDFActionModel validate(
      EPDFForm_GetFieldActionModel(form, 0, EPDF_FORM_ACTION_VALIDATE));
  ScopedEPDFActionModel calculate(
      EPDFForm_GetFieldActionModel(form, 0, EPDF_FORM_ACTION_CALCULATE));
  EXPECT_FALSE(
      EPDFForm_GetFieldActionModel(form, 0, EPDF_FORM_ACTION_KEYSTROKE));
  EXPECT_FALSE(EPDFForm_GetFieldActionModel(form, 0, EPDF_FORM_ACTION_FORMAT));
  ASSERT_TRUE(validate);
  ASSERT_TRUE(calculate);
  EPDFForm_CloseModel(form);

  ScopedEPDFActionModel focus;
  ScopedEPDFActionModel enter;
  {
    FPDF_PAGE page = LoadPage(0);
    ASSERT_TRUE(page);
    {
      ScopedFPDFAnnotation widget(FPDFPage_GetAnnot(page, 0));
      ASSERT_TRUE(widget);
      focus.reset(
          EPDFAnnot_GetActionModel(widget.get(), EPDF_ANNOT_ACTION_FOCUS));
      enter.reset(EPDFAnnot_GetActionModel(widget.get(),
                                           EPDF_ANNOT_ACTION_CURSOR_ENTER));
      // /X is absent. The field /V at the same numeric event position must
      // never leak through the annotation event mapping.
      EXPECT_FALSE(EPDFAnnot_GetActionModel(widget.get(),
                                            EPDF_ANNOT_ACTION_CURSOR_EXIT));
    }
    UnloadPage(page);
  }
  ASSERT_TRUE(focus);
  ASSERT_TRUE(enter);

  CloseDocument();
  EXPECT_EQ(L"validate();", GetActionJavaScript(validate.get(), 0));
  EXPECT_EQ(L"calculate();", GetActionJavaScript(calculate.get(), 0));
  EXPECT_EQ(L"focus();", GetActionJavaScript(focus.get(), 0));
  EXPECT_EQ(L"enter();", GetActionJavaScript(enter.get(), 0));
}

TEST_F(EPDFActionEmbedderTest, NodeUriPayloadFromRealDocument) {
  ASSERT_TRUE(OpenDocument("annots_action_handling.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  bool checked_link = false;
  const int annotation_count = FPDFPage_GetAnnotCount(page.get());
  for (int i = 0; i < annotation_count; ++i) {
    ScopedFPDFAnnotation annotation(FPDFPage_GetAnnot(page.get(), i));
    ASSERT_TRUE(annotation);
    if (FPDFAnnot_GetSubtype(annotation.get()) != FPDF_ANNOT_LINK) {
      continue;
    }
    ScopedEPDFActionModel model(
        EPDFAnnot_GetActionModel(annotation.get(), EPDF_ANNOT_ACTION_ACTIVATE));
    // This fixture also contains destination-only links. They correctly have
    // no activate action model; keep looking for its URI-action link.
    if (!model) {
      continue;
    }
    const EPDF_ACTION_NODE_ID root = EPDFAction_GetRootNode(model.get());
    ASSERT_NE(EPDF_ACTION_NODE_INVALID, root);
    ASSERT_EQ(EPDF_ACTION_TYPE_URI, EPDFAction_GetNodeType(model.get(), root));

    const unsigned long len =
        EPDFAction_GetNodeURI(document(), model.get(), root, nullptr, 0);
    ASSERT_GT(len, 1ul);
    std::vector<char> buffer(len);
    ASSERT_EQ(len, EPDFAction_GetNodeURI(document(), model.get(), root,
                                         buffer.data(), len));
    const ByteString uri(buffer.data());
    EXPECT_EQ(0u, uri.Find("https://").value_or(1u));

    // Wrong-type payload getters answer empty rather than lying.
    EXPECT_FALSE(EPDFAction_GetNodeDest(document(), model.get(), root));
    EXPECT_EQ(0ul, EPDFAction_GetNodeFilePath(model.get(), root, nullptr, 0));
    EXPECT_EQ(0ul, EPDFAction_GetNodeName(model.get(), root, nullptr, 0));
    checked_link = true;
  }
  EXPECT_TRUE(checked_link);
}

TEST_F(EPDFActionEmbedderTest, NodeDestPayloadFromCreatedGoTo) {
  ScopedFPDFDocument document(FPDF_CreateNewDocument());
  ASSERT_TRUE(document);
  ScopedFPDFPage page(FPDFPage_New(document.get(), 0, 612, 792));
  ASSERT_TRUE(page);

  FPDF_DEST dest = EPDFDest_CreateXYZ(page.get(), /*has_left=*/true, 30.0f,
                                      /*has_top=*/true, 500.0f,
                                      /*has_zoom=*/false, 0.0f);
  ASSERT_TRUE(dest);
  FPDF_ACTION action = EPDFAction_CreateGoTo(document.get(), dest);
  ASSERT_TRUE(action);

  ScopedEPDFActionModel model(EPDFAction_LoadModel(action));
  ASSERT_TRUE(model);
  const EPDF_ACTION_NODE_ID root = EPDFAction_GetRootNode(model.get());
  ASSERT_NE(EPDF_ACTION_NODE_INVALID, root);
  ASSERT_EQ(EPDF_ACTION_TYPE_GOTO, EPDFAction_GetNodeType(model.get(), root));

  FPDF_DEST node_dest =
      EPDFAction_GetNodeDest(document.get(), model.get(), root);
  ASSERT_TRUE(node_dest);
  FPDF_BOOL has_x = false;
  FPDF_BOOL has_y = false;
  FPDF_BOOL has_zoom = false;
  FS_FLOAT x = 0;
  FS_FLOAT y = 0;
  FS_FLOAT zoom = 0;
  ASSERT_TRUE(FPDFDest_GetLocationInPage(node_dest, &has_x, &has_y, &has_zoom,
                                         &x, &y, &zoom));
  EXPECT_TRUE(has_x);
  EXPECT_TRUE(has_y);
  EXPECT_FALSE(has_zoom);
  EXPECT_FLOAT_EQ(30.0f, x);
  EXPECT_FLOAT_EQ(500.0f, y);

  // A goto node has no URI/file/name payload.
  EXPECT_EQ(0ul, EPDFAction_GetNodeURI(document.get(), model.get(), root,
                                       nullptr, 0));
  EXPECT_EQ(0ul, EPDFAction_GetNodeName(model.get(), root, nullptr, 0));
}

TEST_F(EPDFActionEmbedderTest, DestinationsByPageObjectNumber) {
  ScopedFPDFDocument document(FPDF_CreateNewDocument());
  ASSERT_TRUE(document);
  ScopedFPDFPage first(FPDFPage_New(document.get(), 0, 612, 792));
  ScopedFPDFPage second(FPDFPage_New(document.get(), 1, 612, 792));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document.get());
  const uint32_t second_obj_num = doc->GetPageDictionary(1)->GetObjNum();
  ASSERT_NE(0u, second_obj_num);

  FPDF_DEST xyz = EPDFDest_CreateXYZByObjectNumber(
      document.get(), second_obj_num, /*has_left=*/true, 30.0f,
      /*has_top=*/true, 500.0f, /*has_zoom=*/false, 0.0f);
  ASSERT_TRUE(xyz);
  EXPECT_EQ(1, FPDFDest_GetDestPageIndex(document.get(), xyz));
  FPDF_BOOL has_x = false;
  FPDF_BOOL has_y = false;
  FPDF_BOOL has_zoom = true;
  FS_FLOAT x = 0;
  FS_FLOAT y = 0;
  FS_FLOAT zoom = 0;
  ASSERT_TRUE(FPDFDest_GetLocationInPage(xyz, &has_x, &has_y, &has_zoom, &x,
                                         &y, &zoom));
  EXPECT_TRUE(has_x);
  EXPECT_TRUE(has_y);
  EXPECT_FALSE(has_zoom);
  EXPECT_FLOAT_EQ(30.0f, x);
  EXPECT_FLOAT_EQ(500.0f, y);

  const FS_FLOAT top = 400.0f;
  FPDF_DEST fit_h = EPDFDest_CreateViewByObjectNumber(
      document.get(), second_obj_num, PDFDEST_VIEW_FITH, &top, 1);
  ASSERT_TRUE(fit_h);
  EXPECT_EQ(1, FPDFDest_GetDestPageIndex(document.get(), fit_h));
  unsigned long num_params = 0;
  FS_FLOAT params[4] = {};
  EXPECT_EQ(static_cast<unsigned long>(PDFDEST_VIEW_FITH),
            FPDFDest_GetView(fit_h, &num_params, params));
  ASSERT_EQ(1ul, num_params);
  EXPECT_FLOAT_EQ(400.0f, params[0]);

  // Only a page's dictionary is a destination page.
  const uint32_t catalog_obj_num = doc->GetRoot()->GetObjNum();
  EXPECT_FALSE(EPDFDest_CreateXYZByObjectNumber(
      document.get(), catalog_obj_num, true, 0, true, 0, false, 0));
  EXPECT_FALSE(EPDFDest_CreateViewByObjectNumber(document.get(), 0,
                                                 PDFDEST_VIEW_FIT, nullptr, 0));
  EXPECT_FALSE(EPDFDest_CreateViewByObjectNumber(
      document.get(), second_obj_num, PDFDEST_VIEW_XYZ, nullptr, 0));
}

TEST_F(EPDFActionEmbedderTest, NodeFilePathAndNamePayloadsFromSyntheticDicts) {
  ScopedFPDFDocument document(FPDF_CreateNewDocument());
  ASSERT_TRUE(document);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document.get());
  ASSERT_TRUE(doc);

  RetainPtr<CPDF_Dictionary> launch = doc->NewIndirect<CPDF_Dictionary>();
  launch->SetNewFor<CPDF_Name>("S", "Launch");
  launch->SetNewFor<CPDF_String>("F", "app.exe");
  ScopedEPDFActionModel launch_model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(launch.Get())));
  ASSERT_TRUE(launch_model);
  const EPDF_ACTION_NODE_ID launch_root =
      EPDFAction_GetRootNode(launch_model.get());
  ASSERT_EQ(EPDF_ACTION_TYPE_LAUNCH,
            EPDFAction_GetNodeType(launch_model.get(), launch_root));
  unsigned long len =
      EPDFAction_GetNodeFilePath(launch_model.get(), launch_root, nullptr, 0);
  ASSERT_GT(len, 1ul);
  std::vector<char> path(len);
  ASSERT_EQ(len, EPDFAction_GetNodeFilePath(launch_model.get(), launch_root,
                                            path.data(), len));
  EXPECT_STREQ("app.exe", path.data());

  RetainPtr<CPDF_Dictionary> named = doc->NewIndirect<CPDF_Dictionary>();
  named->SetNewFor<CPDF_Name>("S", "Named");
  named->SetNewFor<CPDF_Name>("N", "NextPage");
  ScopedEPDFActionModel named_model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(named.Get())));
  ASSERT_TRUE(named_model);
  const EPDF_ACTION_NODE_ID named_root =
      EPDFAction_GetRootNode(named_model.get());
  ASSERT_EQ(EPDF_ACTION_TYPE_NAMED,
            EPDFAction_GetNodeType(named_model.get(), named_root));
  len = EPDFAction_GetNodeName(named_model.get(), named_root, nullptr, 0);
  ASSERT_GT(len, 1ul);
  std::vector<char> name(len);
  ASSERT_EQ(len, EPDFAction_GetNodeName(named_model.get(), named_root,
                                        name.data(), len));
  EXPECT_STREQ("NextPage", name.data());

  // Cross-type checks: a launch node answers nothing for uri/name and a
  // named node nothing for file paths.
  EXPECT_EQ(0ul, EPDFAction_GetNodeURI(document.get(), launch_model.get(),
                                       launch_root, nullptr, 0));
  EXPECT_EQ(0ul, EPDFAction_GetNodeName(launch_model.get(), launch_root,
                                        nullptr, 0));
  EXPECT_EQ(0ul, EPDFAction_GetNodeFilePath(named_model.get(), named_root,
                                            nullptr, 0));
}

TEST_F(EPDFActionEmbedderTest, HideTargetPayloadsMixedForms) {
  ScopedFPDFDocument document(FPDF_CreateNewDocument());
  ASSERT_TRUE(document);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document.get());
  ASSERT_TRUE(doc);

  RetainPtr<CPDF_Dictionary> widget = doc->NewIndirect<CPDF_Dictionary>();
  RetainPtr<CPDF_Dictionary> hide = doc->NewIndirect<CPDF_Dictionary>();
  hide->SetNewFor<CPDF_Name>("S", "Hide");
  hide->SetNewFor<CPDF_Boolean>("H", false);
  RetainPtr<CPDF_Array> targets = hide->SetNewFor<CPDF_Array>("T");
  targets->AppendNew<CPDF_String>(L"note1");
  targets->AppendNew<CPDF_Reference>(doc, widget->GetObjNum());

  ScopedEPDFActionModel model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(hide.Get())));
  ASSERT_TRUE(model);
  const EPDF_ACTION_NODE_ID root = EPDFAction_GetRootNode(model.get());
  ASSERT_EQ(EPDF_ACTION_TYPE_HIDE, EPDFAction_GetNodeType(model.get(), root));

  ASSERT_EQ(2, EPDFAction_GetNodeTargetCount(model.get(), root));
  EXPECT_EQ("note1", GetTargetName(model.get(), root, 0));
  unsigned int object_number = 0;
  EXPECT_FALSE(EPDFAction_GetNodeTargetObjectNumber(model.get(), root, 0,
                                                    &object_number));
  ASSERT_TRUE(EPDFAction_GetNodeTargetObjectNumber(model.get(), root, 1,
                                                   &object_number));
  EXPECT_EQ(widget->GetObjNum(), object_number);
  EXPECT_EQ(0ul,
            EPDFAction_GetNodeTargetName(model.get(), root, 1, nullptr, 0));
  EXPECT_EQ(0ul,
            EPDFAction_GetNodeTargetName(model.get(), root, 2, nullptr, 0));

  FPDF_BOOL hide_flag = true;
  ASSERT_TRUE(EPDFAction_GetNodeHideFlag(model.get(), root, &hide_flag));
  EXPECT_FALSE(hide_flag);

  // Wrong-type gates answer empty/false rather than lying.
  FPDF_BOOL has_fields = false;
  FPDF_BOOL exclude = false;
  EXPECT_FALSE(
      EPDFAction_GetNodeResetForm(model.get(), root, &has_fields, &exclude));
  FPDF_BOOL is_map = false;
  EXPECT_FALSE(EPDFAction_GetNodeURIIsMap(model.get(), root, &is_map));
}

TEST_F(EPDFActionEmbedderTest, HideScalarTargetAndDefaultFlag) {
  auto hide = pdfium::MakeRetain<CPDF_Dictionary>();
  hide->SetNewFor<CPDF_Name>("S", "Hide");
  hide->SetNewFor<CPDF_String>("T", L"fieldA");

  ScopedEPDFActionModel model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(hide.Get())));
  ASSERT_TRUE(model);
  const EPDF_ACTION_NODE_ID root = EPDFAction_GetRootNode(model.get());
  ASSERT_EQ(1, EPDFAction_GetNodeTargetCount(model.get(), root));
  EXPECT_EQ("fieldA", GetTargetName(model.get(), root, 0));

  FPDF_BOOL hide_flag = false;
  ASSERT_TRUE(EPDFAction_GetNodeHideFlag(model.get(), root, &hide_flag));
  EXPECT_TRUE(hide_flag);

  // A JavaScript node carries no target list at all.
  RetainPtr<CPDF_Dictionary> script = MakeJavaScriptAction(L"noop();");
  ScopedEPDFActionModel script_model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(script.Get())));
  ASSERT_TRUE(script_model);
  EXPECT_EQ(0, EPDFAction_GetNodeTargetCount(script_model.get(), 0));
  EXPECT_FALSE(EPDFAction_GetNodeHideFlag(script_model.get(), 0, &hide_flag));
}

TEST_F(EPDFActionEmbedderTest, PartialHideTargetListIsWithheld) {
  auto hide = pdfium::MakeRetain<CPDF_Dictionary>();
  hide->SetNewFor<CPDF_Name>("S", "Hide");
  RetainPtr<CPDF_Array> targets = hide->SetNewFor<CPDF_Array>("T");
  targets->AppendNew<CPDF_String>(L"kept");
  // A direct inline dictionary has no durable identity. Executing the
  // remaining targets would hide only part of what the author intended, so
  // the whole list is withheld while the node and its chain stay readable.
  targets->AppendNew<CPDF_Dictionary>();

  ScopedEPDFActionModel model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(hide.Get())));
  ASSERT_TRUE(model);
  const EPDF_ACTION_NODE_ID root = EPDFAction_GetRootNode(model.get());
  ASSERT_EQ(EPDF_ACTION_TYPE_HIDE, EPDFAction_GetNodeType(model.get(), root));
  EXPECT_EQ(-1, EPDFAction_GetNodeTargetCount(model.get(), root));
  EXPECT_EQ(0ul,
            EPDFAction_GetNodeTargetName(model.get(), root, 0, nullptr, 0));
  unsigned int object_number = 0;
  EXPECT_FALSE(EPDFAction_GetNodeTargetObjectNumber(model.get(), root, 0,
                                                    &object_number));
  // Semantic withholding is per-node, not a resource fault: the model as a
  // whole remains complete.
  EXPECT_FALSE(EPDFAction_GetWarningFlags(model.get()) &
               EPDF_ACTION_WARNING_INCOMPLETE);
  EXPECT_TRUE(EPDFAction_IsComplete(model.get()));
}

TEST_F(EPDFActionEmbedderTest, ResetFormThreeStates) {
  ScopedFPDFDocument document(FPDF_CreateNewDocument());
  ASSERT_TRUE(document);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document.get());
  ASSERT_TRUE(doc);
  RetainPtr<CPDF_Dictionary> field = doc->NewIndirect<CPDF_Dictionary>();

  // /Fields ABSENT: reset everything; flags are meaningless.
  auto reset_all = pdfium::MakeRetain<CPDF_Dictionary>();
  reset_all->SetNewFor<CPDF_Name>("S", "ResetForm");
  reset_all->SetNewFor<CPDF_Number>("Flags", 1);
  ScopedEPDFActionModel all_model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(reset_all.Get())));
  ASSERT_TRUE(all_model);
  FPDF_BOOL has_fields = true;
  FPDF_BOOL exclude = false;
  ASSERT_TRUE(
      EPDFAction_GetNodeResetForm(all_model.get(), 0, &has_fields, &exclude));
  EXPECT_FALSE(has_fields);
  EXPECT_TRUE(exclude);
  EXPECT_EQ(0, EPDFAction_GetNodeTargetCount(all_model.get(), 0));

  // /Fields present but EMPTY: a different state from absent.
  auto reset_none = pdfium::MakeRetain<CPDF_Dictionary>();
  reset_none->SetNewFor<CPDF_Name>("S", "ResetForm");
  reset_none->SetNewFor<CPDF_Array>("Fields");
  ScopedEPDFActionModel none_model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(reset_none.Get())));
  ASSERT_TRUE(none_model);
  ASSERT_TRUE(
      EPDFAction_GetNodeResetForm(none_model.get(), 0, &has_fields, &exclude));
  EXPECT_TRUE(has_fields);
  EXPECT_FALSE(exclude);
  EXPECT_EQ(0, EPDFAction_GetNodeTargetCount(none_model.get(), 0));

  // Non-empty exclusion list mixing a name and a field reference.
  auto reset_some = pdfium::MakeRetain<CPDF_Dictionary>();
  reset_some->SetNewFor<CPDF_Name>("S", "ResetForm");
  reset_some->SetNewFor<CPDF_Number>("Flags", 1);
  RetainPtr<CPDF_Array> fields = reset_some->SetNewFor<CPDF_Array>("Fields");
  fields->AppendNew<CPDF_String>(L"calc1");
  fields->AppendNew<CPDF_Reference>(doc, field->GetObjNum());
  ScopedEPDFActionModel some_model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(reset_some.Get())));
  ASSERT_TRUE(some_model);
  ASSERT_TRUE(
      EPDFAction_GetNodeResetForm(some_model.get(), 0, &has_fields, &exclude));
  EXPECT_TRUE(has_fields);
  EXPECT_TRUE(exclude);
  ASSERT_EQ(2, EPDFAction_GetNodeTargetCount(some_model.get(), 0));
  EXPECT_EQ("calc1", GetTargetName(some_model.get(), 0, 0));
  unsigned int object_number = 0;
  ASSERT_TRUE(EPDFAction_GetNodeTargetObjectNumber(some_model.get(), 0, 1,
                                                   &object_number));
  EXPECT_EQ(field->GetObjNum(), object_number);
}

TEST_F(EPDFActionEmbedderTest, UriIsMapPayload) {
  auto uri = pdfium::MakeRetain<CPDF_Dictionary>();
  uri->SetNewFor<CPDF_Name>("S", "URI");
  uri->SetNewFor<CPDF_String>("URI", "https://example.test/map");
  uri->SetNewFor<CPDF_Boolean>("IsMap", true);
  ScopedEPDFActionModel model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(uri.Get())));
  ASSERT_TRUE(model);
  FPDF_BOOL is_map = false;
  ASSERT_TRUE(EPDFAction_GetNodeURIIsMap(model.get(), 0, &is_map));
  EXPECT_TRUE(is_map);

  auto plain = pdfium::MakeRetain<CPDF_Dictionary>();
  plain->SetNewFor<CPDF_Name>("S", "URI");
  plain->SetNewFor<CPDF_String>("URI", "https://example.test/");
  ScopedEPDFActionModel plain_model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(plain.Get())));
  ASSERT_TRUE(plain_model);
  is_map = true;
  ASSERT_TRUE(EPDFAction_GetNodeURIIsMap(plain_model.get(), 0, &is_map));
  EXPECT_FALSE(is_map);

  RetainPtr<CPDF_Dictionary> named = pdfium::MakeRetain<CPDF_Dictionary>();
  named->SetNewFor<CPDF_Name>("S", "Named");
  named->SetNewFor<CPDF_Name>("N", "NextPage");
  ScopedEPDFActionModel named_model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(named.Get())));
  ASSERT_TRUE(named_model);
  EXPECT_FALSE(EPDFAction_GetNodeURIIsMap(named_model.get(), 0, &is_map));
}

TEST_F(EPDFActionEmbedderTest, TargetBudgetOverflowDropsNodeAndMarksIncomplete) {
  auto hide = pdfium::MakeRetain<CPDF_Dictionary>();
  hide->SetNewFor<CPDF_Name>("S", "Hide");
  RetainPtr<CPDF_Array> targets = hide->SetNewFor<CPDF_Array>("T");
  for (int i = 0; i < 2049; ++i) {
    targets->AppendNew<CPDF_String>("t");
  }

  ScopedEPDFActionModel model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(hide.Get())));
  ASSERT_TRUE(model);
  // The node under construction is dropped whole — never a partial list.
  EXPECT_EQ(0, EPDFAction_GetNodeCount(model.get()));
  EXPECT_EQ(EPDF_ACTION_NODE_INVALID, EPDFAction_GetRootNode(model.get()));
  EXPECT_TRUE(EPDFAction_GetWarningFlags(model.get()) &
              EPDF_ACTION_WARNING_INCOMPLETE);
  EXPECT_FALSE(EPDFAction_IsComplete(model.get()));
}

TEST_F(EPDFActionEmbedderTest, PayloadByteBudgetOverflowDropsNode) {
  const std::string big((1 << 20) + 1, 'a');
  auto uri = pdfium::MakeRetain<CPDF_Dictionary>();
  uri->SetNewFor<CPDF_Name>("S", "URI");
  uri->SetNewFor<CPDF_String>("URI", ByteString(big.c_str()));

  ScopedEPDFActionModel model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(uri.Get())));
  ASSERT_TRUE(model);
  EXPECT_EQ(0, EPDFAction_GetNodeCount(model.get()));
  EXPECT_TRUE(EPDFAction_GetWarningFlags(model.get()) &
              EPDF_ACTION_WARNING_INCOMPLETE);
  EXPECT_FALSE(EPDFAction_IsComplete(model.get()));
}

TEST_F(EPDFActionEmbedderTest, OpenActionDestinationForms) {
  // Absent /OpenAction: both readers answer null.
  ScopedFPDFDocument empty(FPDF_CreateNewDocument());
  ASSERT_TRUE(empty);
  EXPECT_FALSE(EPDFDoc_GetOpenActionDest(empty.get()));
  EXPECT_FALSE(EPDFDoc_GetOpenActionModel(empty.get()));
  EXPECT_FALSE(EPDFDoc_GetOpenActionDest(nullptr));

  // Destination form: the dest reads, the action model stays null.
  ScopedFPDFDocument document(FPDF_CreateNewDocument());
  ASSERT_TRUE(document);
  ScopedFPDFPage page(FPDFPage_New(document.get(), 0, 612, 792));
  ASSERT_TRUE(page);
  const uint32_t page_objnum = EPDFPage_GetObjectNumber(page.get());
  ASSERT_GT(page_objnum, 0u);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document.get());
  ASSERT_TRUE(doc);
  RetainPtr<CPDF_Array> open_action =
      doc->GetMutableRoot()->SetNewFor<CPDF_Array>("OpenAction");
  open_action->AppendNew<CPDF_Reference>(doc, page_objnum);
  open_action->AppendNew<CPDF_Name>("XYZ");
  open_action->AppendNew<CPDF_Number>(10);
  open_action->AppendNew<CPDF_Number>(700);
  open_action->AppendNew<CPDF_Number>(1.5f);

  FPDF_DEST dest = EPDFDoc_GetOpenActionDest(document.get());
  ASSERT_TRUE(dest);
  EXPECT_FALSE(EPDFDoc_GetOpenActionModel(document.get()));
  EXPECT_EQ(page_objnum,
            EPDFDest_GetPageObjectNumber(document.get(), dest));
  FPDF_BOOL has_x = false;
  FPDF_BOOL has_y = false;
  FPDF_BOOL has_zoom = false;
  FS_FLOAT x = 0;
  FS_FLOAT y = 0;
  FS_FLOAT zoom = 0;
  ASSERT_TRUE(
      FPDFDest_GetLocationInPage(dest, &has_x, &has_y, &has_zoom, &x, &y,
                                 &zoom));
  EXPECT_TRUE(has_x);
  EXPECT_TRUE(has_y);
  EXPECT_TRUE(has_zoom);
  EXPECT_FLOAT_EQ(10.0f, x);
  EXPECT_FLOAT_EQ(700.0f, y);
  EXPECT_FLOAT_EQ(1.5f, zoom);

  // A named value without a name tree resolves to nothing rather than lying.
  doc->GetMutableRoot()->SetNewFor<CPDF_Name>("OpenAction", "missing");
  EXPECT_FALSE(EPDFDoc_GetOpenActionDest(document.get()));

  // Action form: the model reads, the dest stays null.
  doc->GetMutableRoot()->SetFor("OpenAction", MakeJavaScriptAction(L"open();"));
  ScopedEPDFActionModel action_model(
      EPDFDoc_GetOpenActionModel(document.get()));
  EXPECT_TRUE(action_model);
  EXPECT_FALSE(EPDFDoc_GetOpenActionDest(document.get()));
}

TEST_F(EPDFActionEmbedderTest, DestinationArrayJunkBeyondViewIsIgnored) {
  ScopedFPDFDocument document(FPDF_CreateNewDocument());
  ASSERT_TRUE(document);
  ScopedFPDFPage page(FPDFPage_New(document.get(), 0, 612, 792));
  ASSERT_TRUE(page);
  const uint32_t page_objnum = EPDFPage_GetObjectNumber(page.get());
  ASSERT_GT(page_objnum, 0u);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document.get());
  ASSERT_TRUE(doc);

  RetainPtr<CPDF_Dictionary> go_to = doc->NewIndirect<CPDF_Dictionary>();
  go_to->SetNewFor<CPDF_Name>("S", "GoTo");
  RetainPtr<CPDF_Array> destination = go_to->SetNewFor<CPDF_Array>("D");
  destination->AppendNew<CPDF_Reference>(doc, page_objnum);
  destination->AppendNew<CPDF_Name>("XYZ");
  destination->AppendNew<CPDF_Number>(1);
  destination->AppendNew<CPDF_Number>(2);
  destination->AppendNew<CPDF_Number>(3);
  for (int i = 0; i < 5; ++i) {
    destination->AppendNew<CPDF_Number>(99);
  }

  ScopedEPDFActionModel model(
      EPDFAction_LoadModel(FPDFActionFromCPDFDictionary(go_to.Get())));
  ASSERT_TRUE(model);
  const EPDF_ACTION_NODE_ID root = EPDFAction_GetRootNode(model.get());
  ASSERT_EQ(EPDF_ACTION_TYPE_GOTO, EPDFAction_GetNodeType(model.get(), root));

  FPDF_DEST dest = EPDFAction_GetNodeDest(document.get(), model.get(), root);
  ASSERT_TRUE(dest);
  EXPECT_EQ(page_objnum,
            EPDFDest_GetPageObjectNumber(document.get(), dest));
  FPDF_BOOL has_x = false;
  FPDF_BOOL has_y = false;
  FPDF_BOOL has_zoom = false;
  FS_FLOAT x = 0;
  FS_FLOAT y = 0;
  FS_FLOAT zoom = 0;
  ASSERT_TRUE(
      FPDFDest_GetLocationInPage(dest, &has_x, &has_y, &has_zoom, &x, &y,
                                 &zoom));
  EXPECT_FLOAT_EQ(1.0f, x);
  EXPECT_FLOAT_EQ(2.0f, y);
  EXPECT_FLOAT_EQ(3.0f, zoom);
}
