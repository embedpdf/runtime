// Copyright 2024 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/fpdfdoc/cpdf_interactiveform.h"

#include <memory>

#include "constants/catalog.h"
#include "core/fpdfapi/page/test_with_page_module.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_read_only_graph_guard.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_test_document.h"
#include "core/fxcrt/string_view_template.h"
#include "testing/gtest/include/gtest/gtest.h"

using CPDFInteractiveFormTest = TestWithPageModule;

TEST_F(CPDFInteractiveFormTest, LoadFieldsWithReferencedNames) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();
  RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
  ASSERT_TRUE(root);

  // For use in the field dictionaries below in the /T field. The field type
  // should be string.
  auto good_string = doc->NewIndirect<CPDF_String>("good_string");
  auto bad_name = doc->NewIndirect<CPDF_Name>("bad_name");

  auto bad_stream = doc->NewIndirect<CPDF_Stream>(doc->New<CPDF_Dictionary>());
  bad_stream->SetData(ByteStringView("bad_stream").unsigned_span());

  auto acroform_dict =
      root->SetNewFor<CPDF_Dictionary>(pdfium::catalog::kAcroForm);
  auto fields_array = acroform_dict->SetNewFor<CPDF_Array>("Fields");

  auto good_string_field_dict = fields_array->AppendNew<CPDF_Dictionary>();
  good_string_field_dict->SetNewFor<CPDF_Name>("Type", "Annot");
  good_string_field_dict->SetNewFor<CPDF_Name>("Subtype", "Widget");
  good_string_field_dict->SetNewFor<CPDF_Name>("FT", "Btn");
  good_string_field_dict->SetNewFor<CPDF_Reference>("T", doc.get(),
                                                    good_string->GetObjNum());

  auto bad_name_field_dict = fields_array->AppendNew<CPDF_Dictionary>();
  bad_name_field_dict->SetNewFor<CPDF_Name>("Type", "Annot");
  bad_name_field_dict->SetNewFor<CPDF_Name>("Subtype", "Widget");
  bad_name_field_dict->SetNewFor<CPDF_Name>("FT", "Btn");
  bad_name_field_dict->SetNewFor<CPDF_Reference>("T", doc.get(),
                                                 bad_name->GetObjNum());

  auto bad_stream_field_dict = fields_array->AppendNew<CPDF_Dictionary>();
  bad_stream_field_dict->SetNewFor<CPDF_Name>("Type", "Annot");
  bad_stream_field_dict->SetNewFor<CPDF_Name>("Subtype", "Widget");
  bad_stream_field_dict->SetNewFor<CPDF_Name>("FT", "Btn");
  bad_stream_field_dict->SetNewFor<CPDF_Reference>("T", doc.get(),
                                                   bad_stream->GetObjNum());

  const uint32_t last_obj_num = doc->GetLastObjNum();

  // Let `interactive_form` parse the dictionaries above.
  std::unique_ptr<CPDF_InteractiveForm> interactive_form;
  {
    CPDF_ReadOnlyGraphGuard guard;
    interactive_form = std::make_unique<CPDF_InteractiveForm>(doc.get());
  }

  EXPECT_EQ(last_obj_num, doc->GetLastObjNum());
  EXPECT_TRUE(ToReference(good_string_field_dict->GetObjectFor("T")));
  EXPECT_TRUE(ToReference(bad_name_field_dict->GetObjectFor("T")));
  EXPECT_TRUE(ToReference(bad_stream_field_dict->GetObjectFor("T")));

  EXPECT_EQ(
      1u, interactive_form->CountFields(WideString::FromASCII("good_string")));
  EXPECT_EQ(0u,
            interactive_form->CountFields(WideString::FromASCII("bad_name")));
  EXPECT_EQ(1u, interactive_form->CountFields(WideString()));
}

TEST_F(CPDFInteractiveFormTest, LoadFieldDoesNotCopyInheritedTypeToParent) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();
  RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
  ASSERT_TRUE(root);

  auto acroform_dict = root->SetNewFor<CPDF_Dictionary>("AcroForm");
  auto fields_array = acroform_dict->SetNewFor<CPDF_Array>("Fields");

  auto parent_dict = doc->NewIndirect<CPDF_Dictionary>();
  parent_dict->SetNewFor<CPDF_String>("T", "Parent");
  fields_array->AppendNew<CPDF_Reference>(doc.get(), parent_dict->GetObjNum());

  auto kids = parent_dict->SetNewFor<CPDF_Array>("Kids");
  auto widget_dict = kids->AppendNew<CPDF_Dictionary>();
  widget_dict->SetNewFor<CPDF_Name>("Type", "Annot");
  widget_dict->SetNewFor<CPDF_Name>("Subtype", "Widget");
  widget_dict->SetNewFor<CPDF_Name>("FT", "Tx");
  widget_dict->SetNewFor<CPDF_Number>("Ff", 123);
  widget_dict->SetNewFor<CPDF_Reference>("Parent", doc.get(),
                                         parent_dict->GetObjNum());

  ASSERT_FALSE(parent_dict->KeyExist("FT"));
  ASSERT_FALSE(parent_dict->KeyExist("Ff"));
  const uint32_t last_obj_num = doc->GetLastObjNum();

  std::unique_ptr<CPDF_InteractiveForm> interactive_form;
  {
    CPDF_ReadOnlyGraphGuard guard;
    interactive_form = std::make_unique<CPDF_InteractiveForm>(doc.get());
  }

  EXPECT_EQ(last_obj_num, doc->GetLastObjNum());
  EXPECT_FALSE(parent_dict->KeyExist("FT"));
  EXPECT_FALSE(parent_dict->KeyExist("Ff"));
  EXPECT_EQ(1u,
            interactive_form->CountFields(WideString::FromASCII("Parent")));
  CPDF_FormField* field =
      interactive_form->GetField(0, WideString::FromASCII("Parent"));
  ASSERT_TRUE(field);
  EXPECT_EQ(FormFieldType::kTextField, field->GetFieldType());
  EXPECT_EQ(1, field->CountControls());
}
