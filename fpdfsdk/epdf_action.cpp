// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "public/epdf_action.h"

#include <array>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfdoc/cpdf_action.h"
#include "core/fpdfdoc/cpdf_dest.h"
#include "core/fpdfdoc/cpdf_nametree.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_action_helpers.h"

namespace epdf {

namespace {

constexpr size_t kMaxActionDepth = 64;
constexpr size_t kMaxActionNodes = 1024;
constexpr size_t kMaxJavaScriptCodeUnits = 8 * 1024 * 1024;
// Hide /T + ResetForm /Fields entries, cumulative across one model.
constexpr size_t kMaxActionTargets = 2048;
// URI, file path, named-action, named-destination, and target-name bytes,
// cumulative across one model.
constexpr size_t kMaxActionPayloadBytes = 1 << 20;
// FPDFDest reads at most the page, the view name, and four view parameters;
// destination array elements beyond that carry no meaning.
constexpr size_t kMaxDestinationElements = 6;

struct ActionTargetRecord {
  // Exactly one is meaningful: a field FQN (UTF-8) when |object_number| is
  // 0, otherwise an indirect annotation/field dictionary object number.
  ByteString name;
  uint32_t object_number = 0;
};

struct ActionNodeRecord {
  ByteString subtype;
  int type = EPDF_ACTION_TYPE_UNKNOWN;
  std::optional<WideString> javascript;
  RetainPtr<const CPDF_Array> destination;
  ByteString named_destination;
  ByteString uri;
  ByteString file_path;
  ByteString name;
  // Hide /T or ResetForm /Fields entries. |targets_valid| == false means an
  // entry could not be represented; a partial target list must never
  // execute, so the whole list is withheld while the node's siblings and
  // /Next chain stay usable.
  std::vector<ActionTargetRecord> targets;
  bool targets_valid = true;
  // Hide /H: true hides (the spec default), false shows.
  bool hide = true;
  // ResetForm: whether /Fields was PRESENT. Absent means "reset every
  // field" and |exclude| is meaningless — presence and emptiness are
  // different states (PDFium's executor branches on HasFields() first).
  bool reset_has_fields = false;
  // ResetForm /Flags bit 0: set = /Fields lists EXCLUDED fields.
  bool exclude = false;
  // URI /IsMap.
  bool is_map = false;
  // SubmitForm /F resolved to a URL (UTF-8). |submit_url_valid| == false
  // means the REQUIRED /F was absent, not a URL file specification, or
  // unrepresentable — the payload is withheld so the reader degrades the
  // node instead of executing a half payload.
  ByteString submit_url;
  bool submit_url_valid = false;
  // SubmitForm raw /Flags word (ISO 32000-2 Table 240, bits numbered from 1).
  uint32_t submit_flags = 0;
  // SubmitForm /CharSet (PDF 2.0); empty when absent.
  ByteString submit_charset;
  // SubmitForm: whether /Fields was PRESENT — same presence-vs-empty
  // distinction as |reset_has_fields|.
  bool submit_has_fields = false;
  std::vector<EPDF_ACTION_NODE_ID> next;
};

int NormalizeActionType(CPDF_Action::Type type) {
  switch (type) {
    case CPDF_Action::Type::kUnknown:
      return EPDF_ACTION_TYPE_UNKNOWN;
    case CPDF_Action::Type::kGoTo:
      return EPDF_ACTION_TYPE_GOTO;
    case CPDF_Action::Type::kGoToR:
      return EPDF_ACTION_TYPE_GOTO_REMOTE;
    case CPDF_Action::Type::kGoToE:
      return EPDF_ACTION_TYPE_GOTO_EMBEDDED;
    case CPDF_Action::Type::kLaunch:
      return EPDF_ACTION_TYPE_LAUNCH;
    case CPDF_Action::Type::kThread:
      return EPDF_ACTION_TYPE_THREAD;
    case CPDF_Action::Type::kURI:
      return EPDF_ACTION_TYPE_URI;
    case CPDF_Action::Type::kSound:
      return EPDF_ACTION_TYPE_SOUND;
    case CPDF_Action::Type::kMovie:
      return EPDF_ACTION_TYPE_MOVIE;
    case CPDF_Action::Type::kHide:
      return EPDF_ACTION_TYPE_HIDE;
    case CPDF_Action::Type::kNamed:
      return EPDF_ACTION_TYPE_NAMED;
    case CPDF_Action::Type::kSubmitForm:
      return EPDF_ACTION_TYPE_SUBMIT_FORM;
    case CPDF_Action::Type::kResetForm:
      return EPDF_ACTION_TYPE_RESET_FORM;
    case CPDF_Action::Type::kImportData:
      return EPDF_ACTION_TYPE_IMPORT_DATA;
    case CPDF_Action::Type::kJavaScript:
      return EPDF_ACTION_TYPE_JAVASCRIPT;
    case CPDF_Action::Type::kSetOCGState:
      return EPDF_ACTION_TYPE_SET_OCG_STATE;
    case CPDF_Action::Type::kRendition:
      return EPDF_ACTION_TYPE_RENDITION;
    case CPDF_Action::Type::kTrans:
      return EPDF_ACTION_TYPE_TRANSITION;
    case CPDF_Action::Type::kGoTo3DView:
      return EPDF_ACTION_TYPE_GOTO_3D_VIEW;
  }
  return EPDF_ACTION_TYPE_UNKNOWN;
}

RetainPtr<const CPDF_Array> SnapshotDestination(const CPDF_Action& action,
                                                CPDF_Document* document) {
  const CPDF_Dictionary* dict = action.GetDict();
  if (!dict) {
    return nullptr;
  }

  RetainPtr<const CPDF_Array> source;
  if (document) {
    CPDF_Dest destination = action.GetDest(document);
    source = pdfium::WrapRetain(destination.GetArray());
  } else {
    source = ToArray(dict->GetDirectObjectFor("D"));
  }
  if (!source) {
    return nullptr;
  }

  auto snapshot = pdfium::MakeRetain<CPDF_Array>();
  // Copy only the elements FPDFDest can read; trailing junk in an oversized
  // destination array neither grows the snapshot nor poisons the payload.
  const size_t element_count = source->size() < kMaxDestinationElements
                                   ? source->size()
                                   : kMaxDestinationElements;
  for (size_t i = 0; i < element_count; ++i) {
    RetainPtr<const CPDF_Object> value = source->GetDirectObjectAt(i);
    if (!value) {
      return nullptr;
    }
    if (i == 0 && value->IsDictionary()) {
      // FPDFDest only needs the page dictionary's object number. A minimal
      // detached stand-in preserves that identity without retaining the live
      // page graph.
      RetainPtr<CPDF_Dictionary> page =
          snapshot->AppendNew<CPDF_Dictionary>();
      page->SetObjNum(value->GetObjNum());
      continue;
    }
    RetainPtr<CPDF_Object> clone = value->CloneDirectObject();
    if (!clone) {
      return nullptr;
    }
    snapshot->Append(std::move(clone));
  }
  snapshot->Freeze();
  return snapshot;
}

}  // namespace

struct ActionModelData {
  std::vector<ActionNodeRecord> nodes;
  uint32_t warning_flags = 0;
};

namespace {

// Cumulative per-model build budgets. Overflow marks the model INCOMPLETE
// and drops the node under construction, exactly like the depth and node
// bounds — a dropped node never leaves a partial payload behind.
struct BuildBudget {
  size_t javascript_code_units = 0;
  size_t target_entries = 0;
  size_t payload_bytes = 0;
};

bool ChargePayloadBytes(size_t length,
                        BuildBudget* budget,
                        ActionModelData* model) {
  if (length > kMaxActionPayloadBytes - budget->payload_bytes) {
    model->warning_flags |= EPDF_ACTION_WARNING_INCOMPLETE;
    return false;
  }
  budget->payload_bytes += length;
  return true;
}

// Capture one Hide /T or ResetForm/SubmitForm /Fields entry list. Mirrors
// CPDF_Action::GetAllFields()'s shape normalization (scalar-or-array,
// strings and dictionary references) but never skips an unrepresentable
// entry silently: executing a partial target list would hide or reset only
// some of the intended objects, so such a list withholds itself via
// |targets_valid| while the node's siblings and /Next chain stay usable.
// Returns false only when an aggregate build budget was exhausted; the
// model is then already marked INCOMPLETE and the caller drops the node.
bool CaptureActionTargets(const CPDF_Dictionary* dict,
                          BuildBudget* budget,
                          ActionModelData* model,
                          ActionNodeRecord* node) {
  RetainPtr<const CPDF_Object> fields =
      node->type == EPDF_ACTION_TYPE_HIDE ? dict->GetDirectObjectFor("T")
                                          : dict->GetDirectObjectFor("Fields");
  if (!fields) {
    // No list: valid and empty. ResetForm presence rides |reset_has_fields|.
    return true;
  }

  enum class Appended { kOk, kUnrepresentable, kOverBudget };
  auto append = [&](const CPDF_Object* entry) {
    if (!entry) {
      return Appended::kUnrepresentable;
    }
    ActionTargetRecord target;
    if (entry->IsString() || entry->IsName()) {
      target.name = entry->GetUnicodeText().ToUTF8();
    } else if (entry->IsDictionary() && entry->GetObjNum() != 0) {
      // An indirect annotation/field dictionary reference. A direct inline
      // dictionary has no durable identity and is unrepresentable.
      target.object_number = entry->GetObjNum();
    } else {
      return Appended::kUnrepresentable;
    }
    if (budget->target_entries >= kMaxActionTargets ||
        !ChargePayloadBytes(target.name.GetLength(), budget, model)) {
      model->warning_flags |= EPDF_ACTION_WARNING_INCOMPLETE;
      return Appended::kOverBudget;
    }
    ++budget->target_entries;
    node->targets.push_back(std::move(target));
    return Appended::kOk;
  };

  const auto withhold = [node]() {
    node->targets.clear();
    node->targets_valid = false;
  };

  if (const CPDF_Array* array = fields->AsArray()) {
    for (size_t i = 0; i < array->size(); ++i) {
      RetainPtr<const CPDF_Object> entry = array->GetDirectObjectAt(i);
      switch (append(entry.Get())) {
        case Appended::kOk:
          break;
        case Appended::kUnrepresentable:
          withhold();
          return true;
        case Appended::kOverBudget:
          return false;
      }
    }
    return true;
  }
  switch (append(fields.Get())) {
    case Appended::kOk:
      return true;
    case Appended::kUnrepresentable:
      withhold();
      return true;
    case Appended::kOverBudget:
      return false;
  }
  return true;
}

// Resolve SubmitForm's REQUIRED /F into a URL. The conforming target is a
// URL file specification — << /FS /URL /F (uri) >> (ISO 32000-2 7.11.5),
// with /UF preferred over /F when both exist (7.11.2). A bare string or
// name /F is accepted as a producer-compat extension and read verbatim.
// Anything else (missing /F, a non-URL file specification, an empty URL)
// leaves |submit_url_valid| false: the required payload stays withheld and
// the reader degrades the node — never a half payload. Returns false only
// when the aggregate payload budget was exhausted (the model is then
// already marked INCOMPLETE and the caller drops the node).
bool CaptureSubmitFormUrl(const CPDF_Dictionary* dict,
                          BuildBudget* budget,
                          ActionModelData* model,
                          ActionNodeRecord* node) {
  RetainPtr<const CPDF_Object> file = dict->GetDirectObjectFor("F");
  ByteString url;
  if (file && (file->IsString() || file->IsName())) {
    url = file->GetUnicodeText().ToUTF8();
  } else if (file && file->IsDictionary()) {
    const CPDF_Dictionary* spec = file->AsDictionary();
    if (spec->GetNameFor("FS") != "URL") {
      return true;
    }
    RetainPtr<const CPDF_Object> uf = spec->GetDirectObjectFor("UF");
    if (uf && (uf->IsString() || uf->IsName())) {
      url = uf->GetUnicodeText().ToUTF8();
    } else {
      RetainPtr<const CPDF_Object> f = spec->GetDirectObjectFor("F");
      if (f && (f->IsString() || f->IsName())) {
        url = f->GetUnicodeText().ToUTF8();
      }
    }
  }
  if (url.IsEmpty()) {
    return true;
  }
  if (!ChargePayloadBytes(url.GetLength(), budget, model)) {
    return false;
  }
  node->submit_url = std::move(url);
  node->submit_url_valid = true;
  return true;
}

std::optional<EPDF_ACTION_NODE_ID> AppendAction(
    const CPDF_Action& action,
    CPDF_Document* document,
    size_t depth,
    BuildBudget* budget,
    std::set<const CPDF_Dictionary*>* active_path,
    ActionModelData* model) {
  const CPDF_Dictionary* dict = action.GetDict();
  if (!dict) {
    model->warning_flags |= EPDF_ACTION_WARNING_MALFORMED_NEXT;
    return std::nullopt;
  }
  if (depth >= kMaxActionDepth || model->nodes.size() >= kMaxActionNodes) {
    model->warning_flags |= EPDF_ACTION_WARNING_INCOMPLETE;
    return std::nullopt;
  }

  ActionNodeRecord node;
  node.subtype = dict->GetNameFor("S");
  node.type = NormalizeActionType(action.GetType());
  if (node.type == EPDF_ACTION_TYPE_JAVASCRIPT ||
      node.type == EPDF_ACTION_TYPE_RENDITION) {
    node.javascript = action.MaybeGetJavaScript();
    if (node.javascript.has_value()) {
      const size_t length = node.javascript->GetLength();
      if (length > kMaxJavaScriptCodeUnits - budget->javascript_code_units) {
        model->warning_flags |= EPDF_ACTION_WARNING_INCOMPLETE;
        return std::nullopt;
      }
      budget->javascript_code_units += length;
    }
  }
  if (node.type == EPDF_ACTION_TYPE_GOTO ||
      node.type == EPDF_ACTION_TYPE_GOTO_REMOTE ||
      node.type == EPDF_ACTION_TYPE_GOTO_EMBEDDED) {
    node.destination = SnapshotDestination(action, document);
    RetainPtr<const CPDF_Object> raw_destination =
        dict->GetDirectObjectFor("D");
    if (!node.destination && raw_destination &&
        (raw_destination->IsName() || raw_destination->IsString())) {
      node.named_destination = raw_destination->GetString();
      if (!ChargePayloadBytes(node.named_destination.GetLength(), budget,
                              model)) {
        return std::nullopt;
      }
    }
  }
  if (node.type == EPDF_ACTION_TYPE_URI) {
    node.uri =
        document ? action.GetURI(document) : dict->GetByteStringFor("URI");
    node.is_map = dict->GetBooleanFor("IsMap", false);
    if (!ChargePayloadBytes(node.uri.GetLength(), budget, model)) {
      return std::nullopt;
    }
  }
  if (node.type == EPDF_ACTION_TYPE_GOTO_REMOTE ||
      node.type == EPDF_ACTION_TYPE_GOTO_EMBEDDED ||
      node.type == EPDF_ACTION_TYPE_LAUNCH) {
    node.file_path = action.GetFilePath().ToUTF8();
    if (!ChargePayloadBytes(node.file_path.GetLength(), budget, model)) {
      return std::nullopt;
    }
  }
  if (node.type == EPDF_ACTION_TYPE_NAMED) {
    node.name = dict->GetNameFor("N");
    if (!ChargePayloadBytes(node.name.GetLength(), budget, model)) {
      return std::nullopt;
    }
  }
  if (node.type == EPDF_ACTION_TYPE_HIDE ||
      node.type == EPDF_ACTION_TYPE_RESET_FORM) {
    if (node.type == EPDF_ACTION_TYPE_HIDE) {
      node.hide = action.GetHideStatus();
    } else {
      node.reset_has_fields = action.HasFields();
      node.exclude = (action.GetFlags() & 1) != 0;
    }
    if (!CaptureActionTargets(dict, budget, model, &node)) {
      return std::nullopt;
    }
  }
  if (node.type == EPDF_ACTION_TYPE_SUBMIT_FORM) {
    node.submit_has_fields = action.HasFields();
    node.submit_flags = action.GetFlags();
    node.submit_charset = dict->GetUnicodeTextFor("CharSet").ToUTF8();
    if (!ChargePayloadBytes(node.submit_charset.GetLength(), budget, model)) {
      return std::nullopt;
    }
    if (!CaptureSubmitFormUrl(dict, budget, model, &node) ||
        !CaptureActionTargets(dict, budget, model, &node)) {
      return std::nullopt;
    }
  }

  const EPDF_ACTION_NODE_ID node_id =
      pdfium::checked_cast<EPDF_ACTION_NODE_ID>(model->nodes.size());
  model->nodes.push_back(std::move(node));
  active_path->insert(dict);

  if (dict->KeyExist("Next")) {
    RetainPtr<const CPDF_Object> next = dict->GetDirectObjectFor("Next");
    if (!next || (!next->IsDictionary() && !next->IsArray())) {
      model->warning_flags |= EPDF_ACTION_WARNING_MALFORMED_NEXT;
    }
  }
  const size_t child_count = action.GetSubActionsCount();
  for (size_t i = 0; i < child_count; ++i) {
    CPDF_Action child = action.GetSubAction(i);
    const CPDF_Dictionary* child_dict = child.GetDict();
    if (!child_dict) {
      model->warning_flags |= EPDF_ACTION_WARNING_MALFORMED_NEXT;
      continue;
    }
    if (active_path->contains(child_dict)) {
      model->warning_flags |= EPDF_ACTION_WARNING_CYCLE_DROPPED;
      continue;
    }
    std::optional<EPDF_ACTION_NODE_ID> child_id =
        AppendAction(child, document, depth + 1, budget, active_path, model);
    if (child_id.has_value()) {
      model->nodes[node_id].next.push_back(child_id.value());
    }
  }

  active_path->erase(dict);
  return node_id;
}

const ActionModelData* DataFromHandle(EPDF_ACTION_MODEL model);

}  // namespace

ActionModelDataPtr BuildActionModel(const CPDF_Action& action,
                                    CPDF_Document* document) {
  if (!action.HasDict()) {
    return nullptr;
  }
  auto model = std::make_shared<ActionModelData>();
  BuildBudget budget;
  std::set<const CPDF_Dictionary*> active_path;
  AppendAction(action, document, 0, &budget, &active_path, model.get());
  return model;
}

}  // namespace epdf

struct epdf_action_model_t__ {
  explicit epdf_action_model_t__(epdf::ActionModelDataPtr data)
      : data(std::move(data)) {}

  epdf::ActionModelDataPtr data;
};

namespace epdf {

namespace {

const ActionModelData* DataFromHandle(EPDF_ACTION_MODEL model) {
  return model && model->data ? model->data.get() : nullptr;
}

const ActionNodeRecord* GetNode(EPDF_ACTION_MODEL model,
                                EPDF_ACTION_NODE_ID node) {
  const ActionModelData* data = DataFromHandle(model);
  return data && node < data->nodes.size() ? &data->nodes[node] : nullptr;
}

EPDF_ACTION_MODEL MakeModelFromDictionary(
    RetainPtr<const CPDF_Dictionary> dictionary,
    CPDF_Document* document) {
  return dictionary ? MakeActionModelHandle(
                          BuildActionModel(CPDF_Action(std::move(dictionary)),
                                           document))
                    : nullptr;
}

RetainPtr<const CPDF_Dictionary> GetPageDictionaryByObjectNumber(
    CPDF_Document* doc,
    uint32_t page_object_number) {
  if (!doc || page_object_number == 0) {
    return nullptr;
  }
#ifdef PDF_ENABLE_XFA
  if (doc->GetExtension()) {
    return nullptr;
  }
#endif  // PDF_ENABLE_XFA
  const int page_index = doc->GetPageIndex(page_object_number);
  return page_index >= 0 ? doc->GetPageDictionary(page_index) : nullptr;
}

}  // namespace

EPDF_ACTION_MODEL MakeActionModelHandle(ActionModelDataPtr data) {
  return data ? new epdf_action_model_t__(std::move(data)) : nullptr;
}

}  // namespace epdf

FPDF_EXPORT void FPDF_CALLCONV EPDFAction_CloseModel(EPDF_ACTION_MODEL model) {
  delete model;
}

FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFAction_LoadModel(FPDF_ACTION action) {
  CPDF_Dictionary* dictionary = CPDFDictionaryFromFPDFAction(action);
  return dictionary ? epdf::MakeActionModelHandle(epdf::BuildActionModel(
                          CPDF_Action(pdfium::WrapRetain(dictionary))))
                    : nullptr;
}

FPDF_EXPORT EPDF_ACTION_NODE_ID FPDF_CALLCONV
EPDFAction_GetRootNode(EPDF_ACTION_MODEL model) {
  const epdf::ActionModelData* data = epdf::DataFromHandle(model);
  return data && !data->nodes.empty() ? 0 : EPDF_ACTION_NODE_INVALID;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFAction_GetNodeCount(EPDF_ACTION_MODEL model) {
  const epdf::ActionModelData* data = epdf::DataFromHandle(model);
  return data ? pdfium::checked_cast<int>(data->nodes.size()) : 0;
}

FPDF_EXPORT int FPDF_CALLCONV EPDFAction_GetNodeType(EPDF_ACTION_MODEL model,
                                                     EPDF_ACTION_NODE_ID node) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  return record ? record->type : EPDF_ACTION_TYPE_UNKNOWN;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeSubtype(EPDF_ACTION_MODEL model,
                          EPDF_ACTION_NODE_ID node,
                          char* buffer,
                          unsigned long buflen) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!record) {
    return 0;
  }
  return NulTerminateMaybeCopyAndReturnLength(
      record->subtype, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_NodeHasJavaScript(EPDF_ACTION_MODEL model,
                             EPDF_ACTION_NODE_ID node) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  return record && record->javascript.has_value();
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeJavaScript(EPDF_ACTION_MODEL model,
                             EPDF_ACTION_NODE_ID node,
                             FPDF_WCHAR* buffer,
                             unsigned long buflen) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!record || !record->javascript.has_value()) {
    return 0;
  }
  return Utf16EncodeMaybeCopyAndReturnLength(
      record->javascript.value(),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFAction_GetNodeDest(FPDF_DOCUMENT document,
                       EPDF_ACTION_MODEL model,
                       EPDF_ACTION_NODE_ID node) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!doc || !record) {
    return nullptr;
  }
  // Same type gate as FPDFAction_GetDest.
  if (record->type != EPDF_ACTION_TYPE_GOTO &&
      record->type != EPDF_ACTION_TYPE_GOTO_REMOTE &&
      record->type != EPDF_ACTION_TYPE_GOTO_EMBEDDED) {
    return nullptr;
  }
  if (record->destination) {
    return FPDFDestFromCPDFArray(record->destination.Get());
  }
  RetainPtr<const CPDF_Array> named =
      record->named_destination.IsEmpty()
          ? nullptr
          : CPDF_NameTree::LookupNamedDest(doc, record->named_destination);
  return FPDFDestFromCPDFArray(named.Get());
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeURI(FPDF_DOCUMENT document,
                      EPDF_ACTION_MODEL model,
                      EPDF_ACTION_NODE_ID node,
                      void* buffer,
                      unsigned long buflen) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!doc || !record || record->type != EPDF_ACTION_TYPE_URI) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      record->uri, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeFilePath(EPDF_ACTION_MODEL model,
                           EPDF_ACTION_NODE_ID node,
                           void* buffer,
                           unsigned long buflen) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!record) {
    return 0;
  }
  // Same type gate as FPDFAction_GetFilePath.
  if (record->type != EPDF_ACTION_TYPE_GOTO_REMOTE &&
      record->type != EPDF_ACTION_TYPE_GOTO_EMBEDDED &&
      record->type != EPDF_ACTION_TYPE_LAUNCH) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      record->file_path,
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeName(EPDF_ACTION_MODEL model,
                       EPDF_ACTION_NODE_ID node,
                       void* buffer,
                       unsigned long buflen) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!record || record->type != EPDF_ACTION_TYPE_NAMED) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      record->name, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

namespace {

const epdf::ActionNodeRecord* GetTargetListNode(EPDF_ACTION_MODEL model,
                                                EPDF_ACTION_NODE_ID node) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!record || (record->type != EPDF_ACTION_TYPE_HIDE &&
                  record->type != EPDF_ACTION_TYPE_RESET_FORM &&
                  record->type != EPDF_ACTION_TYPE_SUBMIT_FORM)) {
    return nullptr;
  }
  return record;
}

const epdf::ActionTargetRecord* GetActionTarget(EPDF_ACTION_MODEL model,
                                                EPDF_ACTION_NODE_ID node,
                                                int index) {
  const epdf::ActionNodeRecord* record = GetTargetListNode(model, node);
  if (!record || !record->targets_valid || index < 0 ||
      static_cast<size_t>(index) >= record->targets.size()) {
    return nullptr;
  }
  return &record->targets[static_cast<size_t>(index)];
}

}  // namespace

FPDF_EXPORT int FPDF_CALLCONV
EPDFAction_GetNodeTargetCount(EPDF_ACTION_MODEL model,
                              EPDF_ACTION_NODE_ID node) {
  const epdf::ActionNodeRecord* record = GetTargetListNode(model, node);
  if (!record) {
    return 0;
  }
  if (!record->targets_valid) {
    return -1;
  }
  return pdfium::checked_cast<int>(record->targets.size());
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeTargetName(EPDF_ACTION_MODEL model,
                             EPDF_ACTION_NODE_ID node,
                             int index,
                             void* buffer,
                             unsigned long buflen) {
  const epdf::ActionTargetRecord* target = GetActionTarget(model, node, index);
  if (!target || target->object_number != 0) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      target->name, UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_GetNodeTargetObjectNumber(EPDF_ACTION_MODEL model,
                                     EPDF_ACTION_NODE_ID node,
                                     int index,
                                     unsigned int* object_number) {
  const epdf::ActionTargetRecord* target = GetActionTarget(model, node, index);
  if (!target || target->object_number == 0 || !object_number) {
    return false;
  }
  *object_number = target->object_number;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_GetNodeHideFlag(EPDF_ACTION_MODEL model,
                           EPDF_ACTION_NODE_ID node,
                           FPDF_BOOL* hide) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!record || record->type != EPDF_ACTION_TYPE_HIDE || !hide) {
    return false;
  }
  *hide = record->hide;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_GetNodeResetForm(EPDF_ACTION_MODEL model,
                            EPDF_ACTION_NODE_ID node,
                            FPDF_BOOL* has_fields,
                            FPDF_BOOL* exclude) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!record || record->type != EPDF_ACTION_TYPE_RESET_FORM || !has_fields ||
      !exclude) {
    return false;
  }
  *has_fields = record->reset_has_fields;
  *exclude = record->exclude;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_GetNodeSubmitForm(EPDF_ACTION_MODEL model,
                             EPDF_ACTION_NODE_ID node,
                             FPDF_BOOL* has_fields,
                             unsigned int* flags) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!record || record->type != EPDF_ACTION_TYPE_SUBMIT_FORM ||
      !record->submit_url_valid || !has_fields || !flags) {
    return false;
  }
  *has_fields = record->submit_has_fields;
  *flags = record->submit_flags;
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeSubmitFormURL(EPDF_ACTION_MODEL model,
                                EPDF_ACTION_NODE_ID node,
                                void* buffer,
                                unsigned long buflen) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!record || record->type != EPDF_ACTION_TYPE_SUBMIT_FORM ||
      !record->submit_url_valid) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      record->submit_url,
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFAction_GetNodeSubmitFormCharSet(EPDF_ACTION_MODEL model,
                                    EPDF_ACTION_NODE_ID node,
                                    void* buffer,
                                    unsigned long buflen) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!record || record->type != EPDF_ACTION_TYPE_SUBMIT_FORM ||
      record->submit_charset.IsEmpty()) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      record->submit_charset,
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_GetNodeURIIsMap(EPDF_ACTION_MODEL model,
                           EPDF_ACTION_NODE_ID node,
                           FPDF_BOOL* is_map) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!record || record->type != EPDF_ACTION_TYPE_URI || !is_map) {
    return false;
  }
  *is_map = record->is_map;
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV
EPDFAction_GetNextCount(EPDF_ACTION_MODEL model, EPDF_ACTION_NODE_ID node) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  return record ? pdfium::checked_cast<int>(record->next.size()) : 0;
}

FPDF_EXPORT EPDF_ACTION_NODE_ID FPDF_CALLCONV
EPDFAction_GetNextAt(EPDF_ACTION_MODEL model,
                     EPDF_ACTION_NODE_ID node,
                     int index) {
  const epdf::ActionNodeRecord* record = epdf::GetNode(model, node);
  if (!record || index < 0 ||
      static_cast<size_t>(index) >= record->next.size()) {
    return EPDF_ACTION_NODE_INVALID;
  }
  return record->next[static_cast<size_t>(index)];
}

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFAction_GetWarningFlags(EPDF_ACTION_MODEL model) {
  const epdf::ActionModelData* data = epdf::DataFromHandle(model);
  return data ? data->warning_flags : EPDF_ACTION_WARNING_INCOMPLETE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAction_IsComplete(EPDF_ACTION_MODEL model) {
  const epdf::ActionModelData* data = epdf::DataFromHandle(model);
  return data && !data->nodes.empty() &&
         !(data->warning_flags & EPDF_ACTION_WARNING_INCOMPLETE);
}

FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFDoc_GetOpenActionModel(FPDF_DOCUMENT document) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  const CPDF_Dictionary* root = doc ? doc->GetRoot() : nullptr;
  return root ? epdf::MakeModelFromDictionary(root->GetDictFor("OpenAction"),
                                              doc)
              : nullptr;
}

FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
EPDFDoc_GetOpenActionDest(FPDF_DOCUMENT document) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  const CPDF_Dictionary* root = doc ? doc->GetRoot() : nullptr;
  if (!root) {
    return nullptr;
  }
  RetainPtr<const CPDF_Object> open_action =
      root->GetDirectObjectFor("OpenAction");
  if (!open_action) {
    return nullptr;
  }
  if (const CPDF_Array* array = open_action->AsArray()) {
    // The handle points into the live catalog, like FPDFLink_GetDest.
    return FPDFDestFromCPDFArray(array);
  }
  if (open_action->IsString() || open_action->IsName()) {
    RetainPtr<const CPDF_Array> named =
        CPDF_NameTree::LookupNamedDest(doc, open_action->GetString());
    return FPDFDestFromCPDFArray(named.Get());
  }
  // The action form reads through EPDFDoc_GetOpenActionModel.
  return nullptr;
}

FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFDoc_GetAdditionalActionModel(FPDF_DOCUMENT document, int event) {
  static constexpr std::array<const char*, 5> kKeys = {"WC", "WS", "DS", "WP",
                                                       "DP"};
  if (event < 0 || event >= static_cast<int>(kKeys.size())) {
    return nullptr;
  }
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  const CPDF_Dictionary* root = doc ? doc->GetRoot() : nullptr;
  RetainPtr<const CPDF_Dictionary> additional =
      root ? root->GetDictFor("AA") : nullptr;
  return additional ? epdf::MakeModelFromDictionary(
                          additional->GetDictFor(kKeys[event]), doc)
                    : nullptr;
}

FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFDoc_GetPageActionModel(FPDF_DOCUMENT document,
                           uint32_t page_object_number,
                           int event) {
  if (event != EPDF_PAGE_ACTION_OPEN && event != EPDF_PAGE_ACTION_CLOSE) {
    return nullptr;
  }
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  RetainPtr<const CPDF_Dictionary> page =
      epdf::GetPageDictionaryByObjectNumber(doc, page_object_number);
  RetainPtr<const CPDF_Dictionary> additional =
      page ? page->GetDictFor("AA") : nullptr;
  const char* key = event == EPDF_PAGE_ACTION_OPEN ? "O" : "C";
  return additional
             ? epdf::MakeModelFromDictionary(additional->GetDictFor(key), doc)
             : nullptr;
}

FPDF_EXPORT EPDF_ACTION_MODEL FPDF_CALLCONV
EPDFAnnot_GetActionModel(FPDF_ANNOTATION annotation, int event) {
  if (event < EPDF_ANNOT_ACTION_ACTIVATE ||
      event > EPDF_ANNOT_ACTION_PAGE_INVISIBLE) {
    return nullptr;
  }
  ScopedFPDFAnnotationView annotation_view(annotation);
  CPDF_AnnotContext* context = annotation_view.Get();
  const CPDF_Dictionary* annotation_dict =
      context ? context->GetAnnotDict() : nullptr;
  if (!annotation_dict) {
    return nullptr;
  }
  if (event == EPDF_ANNOT_ACTION_ACTIVATE) {
    return epdf::MakeModelFromDictionary(
        annotation_dict->GetDictFor("A"), context->GetPage()->GetDocument());
  }
  RetainPtr<const CPDF_Dictionary> additional =
      annotation_dict->GetDictFor("AA");
  const int additional_index = event - EPDF_ANNOT_ACTION_CURSOR_ENTER;
  return additional ? epdf::MakeModelFromDictionary(
                          additional->GetDictFor(
                              epdf::kAnnotEventKeys[additional_index]),
                          context->GetPage()->GetDocument())
                    : nullptr;
}
