// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
#include "public/epdf_measure.h"

#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <tuple>
#include <utility>

#include "constants/annotation_common.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfdoc/cpdf_measure.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {
using Axis = CPDF_Measure::Axis;
constexpr char kShapeCaption[] = "MeasurementCaption";
constexpr char kShapeCenter[] = "MeasurementCaptionCenter";
enum class Kind { kViewport, kMeasure, kFormat };
struct MeasureEpochs {
  uint64_t generation = 0;
  std::array<uint64_t, 6> formats = {};
};
struct OwnerEpochs {
  uint64_t viewports = 0;
  std::map<size_t, MeasureEpochs> measures;
};
// Object identity, not a CPDF_AnnotContext/page pointer: simultaneous opens of
// the same owner must invalidate one another. Direct annotations use
// page/index.
using OwnerKey = std::tuple<bool, uint32_t, int>;
struct Registry final : CPDF_MeasureStorage {
  std::map<OwnerKey, OwnerEpochs> owners;
};
struct Path {
  Kind kind;
  CPDF_AnnotContext* annot = nullptr;  // Borrowed until this annot closes.
  CPDF_Page* page = nullptr;           // Borrowed until this page closes.
  OwnerEpochs* epochs = nullptr;       // Document-owned, stable map node.
  size_t viewport = 0;
  Axis axis = Axis::kX;
  size_t index = 0;
  uint64_t viewport_generation = 0;
  uint64_t measure_generation = 0;
  uint64_t format_generation = 0;

  bool Current() const {
    if (!annot && viewport_generation != epochs->viewports) {
      return false;
    }
    if (kind == Kind::kViewport) {
      return true;
    }
    const auto& measure = epochs->measures.at(viewport);
    if (measure_generation != measure.generation) {
      return false;
    }
    return kind != Kind::kFormat ||
           format_generation == measure.formats[static_cast<size_t>(axis)];
  }
};
using PathKey =
    std::tuple<Kind, size_t, Axis, size_t, uint64_t, uint64_t, uint64_t>;
struct Arena final : CPDF_MeasureStorage {
  // Keep stale paths allocated until the host closes so stale handles fail
  // closed, never aliasing a newly allocated path at the same address.
  std::map<PathKey, std::unique_ptr<Path>> paths;
};
template <typename Host>
Arena* ArenaFor(Host* host) {
  if (!host->GetMeasureStorage()) {
    host->SetMeasureStorage(std::make_unique<Arena>());
  }
  return static_cast<Arena*>(host->GetMeasureStorage());
}
OwnerEpochs* EpochsFor(CPDF_Page* page, CPDF_AnnotContext* annot) {
  auto* doc = page->GetDocument();
  if (!doc->GetMeasureStorage()) {
    doc->SetMeasureStorage(std::make_unique<Registry>());
  }
  auto* registry = static_cast<Registry*>(doc->GetMeasureStorage());
  uint32_t obj =
      annot ? annot->GetAnnotDict()->GetObjNum() : page->GetDict()->GetObjNum();
  int direct_index = -1;
  if (annot && !obj) {
    obj = page->GetDict()->GetObjNum();
    direct_index = annot->GetAnnotIndex();
  }
  return &registry->owners[{annot != nullptr, obj, direct_index}];
}
Path* Intern(Path path) {
  Arena* arena = path.annot ? ArenaFor(path.annot) : ArenaFor(path.page);
  path.viewport_generation = path.annot ? 0 : path.epochs->viewports;
  auto& measure = path.epochs->measures[path.viewport];
  path.measure_generation =
      path.kind == Kind::kViewport ? 0 : measure.generation;
  path.format_generation = path.kind == Kind::kFormat
                               ? measure.formats[static_cast<size_t>(path.axis)]
                               : 0;
  PathKey key{path.kind,
              path.viewport,
              path.axis,
              path.index,
              path.viewport_generation,
              path.measure_generation,
              path.format_generation};
  auto& value = arena->paths[key];
  if (!value) {
    value = std::make_unique<Path>(path);
  }
  return value.get();
}
Path* AnnotPath(CPDF_AnnotContext* annot) {
  auto* page = annot->GetPage()->AsPDFPage();
  return Intern({Kind::kMeasure, annot, page, EpochsFor(page, annot)});
}
Path* ViewportPath(CPDF_Page* page, size_t index) {
  return Intern(
      {Kind::kViewport, nullptr, page, EpochsFor(page, nullptr), index});
}
RetainPtr<const CPDF_Dictionary> ReadHost(const Path& p) {
  if (!p.Current()) {
    return nullptr;
  }
  if (p.annot) {
    return pdfium::WrapRetain(p.annot->GetAnnotDict());
  }
  auto array = p.page->GetDict()->GetArrayFor("VP");
  return array ? array->GetDictAt(p.viewport) : nullptr;
}
RetainPtr<const CPDF_Dictionary> Read(const Path& p) {
  auto host = ReadHost(p);
  if (!host || p.kind == Kind::kViewport) {
    return host;
  }
  auto measure = host->GetDictFor("Measure");
  if (!measure || p.kind == Kind::kMeasure) {
    return measure;
  }
  auto array = measure->GetArrayFor(CPDF_Measure::KeyForAxis(p.axis));
  return array ? array->GetDictAt(p.index) : nullptr;
}
RetainPtr<CPDF_Dictionary> MutableHost(const Path& p) {
  if (p.annot) {
    return p.annot->GetMutableAnnotDict();
  }
  auto page = p.page->GetMutableDict();
  CPDF_Measure::DetachIndirect(page.Get(), "VP");
  auto array = page->GetMutableArrayFor("VP");
  if (!array) {
    return nullptr;
  }
  CPDF_Measure::DetachIndirect(array.Get(), p.viewport);
  return array->GetMutableDictAt(p.viewport);
}
RetainPtr<CPDF_Dictionary> Mutable(const Path& p) {
  auto host = MutableHost(p);
  if (!host || p.kind == Kind::kViewport) {
    return host;
  }
  CPDF_Measure::DetachIndirect(host.Get(), "Measure");
  auto measure = host->GetMutableDictFor("Measure");
  if (!measure || p.kind == Kind::kMeasure) {
    return measure;
  }
  auto key = CPDF_Measure::KeyForAxis(p.axis);
  CPDF_Measure::DetachIndirect(measure.Get(), key);
  auto array = measure->GetMutableArrayFor(key);
  if (!array) {
    return nullptr;
  }
  CPDF_Measure::DetachIndirect(array.Get(), p.index);
  return array->GetMutableDictAt(p.index);
}
// Own the document-view scope for the ENTIRE public call: nested references
// must resolve through this layer even when their base holder is shared.
class PathView {
 public:
  PathView(const void* handle, Kind kind)
      : path_(static_cast<const Path*>(handle)),
        scope_(path_ ? path_->page->GetDocument() : nullptr) {
    if (path_ && path_->kind == kind) {
      dict_ = Read(*path_);
    }
  }
  const CPDF_Dictionary* Get() const { return dict_.Get(); }
  const Path* path() const { return path_; }
  RetainPtr<CPDF_Dictionary> Edit() const {
    if (!dict_) {
      return nullptr;
    }
    if (path_->kind != Kind::kViewport) {
      auto host = ReadHost(*path_);
      auto measure = host ? host->GetDictFor("Measure") : nullptr;
      if (CPDF_Measure::SubtypeOf(measure.Get()) !=
          CPDF_Measure::Subtype::kRectilinear) {
        return nullptr;
      }
    }
    return Mutable(*path_);
  }

 private:
  const Path* path_;
  CPDF_DocumentViewScope scope_;
  RetainPtr<const CPDF_Dictionary> dict_;
};
bool ValidAxis(EPDF_MEASURE_AXIS axis) {
  return axis >= EPDF_MEASURE_AXIS_X && axis <= EPDF_MEASURE_AXIS_SLOPE;
}
bool Finite(const FS_POINTF* p) {
  return p && std::isfinite(p->x) && std::isfinite(p->y);
}
bool Finite(const FS_RECTF* r) {
  return r && std::isfinite(r->left) && std::isfinite(r->right) &&
         std::isfinite(r->bottom) && std::isfinite(r->top);
}
bool ReadPoint(const CPDF_Dictionary* dict,
               ByteStringView key,
               FS_POINTF* out) {
  if (!dict || !out) {
    return false;
  }
  auto array = dict->GetArrayFor(key);
  if (!array || array->size() != 2 || !array->GetNumberAt(0) ||
      !array->GetNumberAt(1)) {
    return false;
  }
  FS_POINTF value{array->GetFloatAt(0), array->GetFloatAt(1)};
  if (!Finite(&value)) {
    return false;
  }
  *out = value;
  return true;
}
void WritePoint(CPDF_Dictionary* dict, const char* key, const FS_POINTF* p) {
  if (!p) {
    dict->RemoveFor(key);
    return;
  }
  auto array = dict->SetNewFor<CPDF_Array>(key);
  array->AppendNew<CPDF_Number>(p->x);
  array->AppendNew<CPDF_Number>(p->y);
}
unsigned long Text(const CPDF_Dictionary* dict,
                   ByteStringView key,
                   FPDF_WCHAR* buffer,
                   unsigned long buflen) {
  if (!dict || !dict->GetStringFor(key)) {
    return 0;
  }
  // SAFETY: caller supplies a buffer of buflen bytes, per the public contract.
  return Utf16EncodeMaybeCopyAndReturnLength(
      dict->GetUnicodeTextFor(key),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}
void WriteText(CPDF_Dictionary* dict, const char* key, FPDF_WIDESTRING text) {
  if (!text) {
    dict->RemoveFor(key);
    return;
  }
  // SAFETY: caller supplies NUL-terminated UTF-16LE.
  dict->SetNewFor<CPDF_String>(
      key, UNSAFE_BUFFERS(WideStringFromFPDFWideString(text)).AsStringView());
}
bool Number(const CPDF_Dictionary* dict, ByteStringView key, float* out) {
  if (!dict || !out) {
    return false;
  }
  auto value = dict->GetNumberFor(key);
  if (!value || !std::isfinite(value->GetNumber())) {
    return false;
  }
  *out = value->GetNumber();
  return true;
}
const char* TextKey(EPDF_MEASURE_TEXT kind) {
  switch (kind) {
    case EPDF_MEASURE_TEXT_THOUSANDS_SEPARATOR:
      return "RT";
    case EPDF_MEASURE_TEXT_DECIMAL_SEPARATOR:
      return "RD";
    case EPDF_MEASURE_TEXT_PREFIX_SPACING:
      return "PS";
    case EPDF_MEASURE_TEXT_SUFFIX_SPACING:
      return "SS";
  }
  return nullptr;
}
Path* AddMeasure(const Path& path) {
  auto host = ReadHost(path);
  if (!host) {
    return nullptr;
  }
  auto existing = host->GetDictFor("Measure");
  if (host->KeyExist("Measure") && CPDF_Measure::SubtypeOf(existing.Get()) !=
                                       CPDF_Measure::Subtype::kRectilinear) {
    return nullptr;
  }
  auto mutable_host = MutableHost(path);
  CPDF_Measure::DetachIndirect(mutable_host.Get(), "Measure");
  auto measure = mutable_host->GetOrCreateDictFor("Measure");
  CPDF_Measure::ResetRectilinear(measure.Get());
  ++path.epochs->measures[path.viewport].generation;
  Path result = path;
  result.kind = Kind::kMeasure;
  return Intern(result);
}
bool RemoveMeasure(const Path& path) {
  auto host = ReadHost(path);
  if (!host) {
    return false;
  }
  if (!host->KeyExist("Measure")) {
    return true;
  }
  MutableHost(path)->RemoveFor("Measure");
  ++path.epochs->measures[path.viewport].generation;
  return true;
}
bool IsLine(const CPDF_AnnotContext* context) {
  return context && context->GetAnnotDict()->GetNameFor("Subtype") == "Line";
}
bool IsShape(const CPDF_AnnotContext* context) {
  if (!context) {
    return false;
  }
  auto type = context->GetAnnotDict()->GetNameFor("Subtype");
  return type == "Polygon" || type == "PolyLine";
}
}  // namespace

FPDF_EXPORT EPDF_MEASURE FPDF_CALLCONV
EPDFAnnot_GetMeasure(FPDF_ANNOTATION annot) {
  ScopedFPDFAnnotationView view(annot);
  if (!view || !view.Get()->GetAnnotDict()->GetDictFor("Measure")) {
    return nullptr;
  }
  return reinterpret_cast<EPDF_MEASURE>(AnnotPath(view.Get()));
}
FPDF_EXPORT EPDF_MEASURE FPDF_CALLCONV
EPDFAnnot_AddMeasure(FPDF_ANNOTATION annot) {
  ScopedFPDFAnnotationView view(annot);
  return view ? reinterpret_cast<EPDF_MEASURE>(
                    AddMeasure(*AnnotPath(view.Get())))
              : nullptr;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_RemoveMeasure(FPDF_ANNOTATION annot) {
  ScopedFPDFAnnotationView view(annot);
  return view && RemoveMeasure(*AnnotPath(view.Get()));
}
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFPage_CountViewports(FPDF_PAGE page) {
  ScopedFPDFPageView view(page);
  auto array = view ? view.Get()->GetDict()->GetArrayFor("VP") : nullptr;
  return array ? array->size() : 0;
}
FPDF_EXPORT EPDF_VIEWPORT FPDF_CALLCONV
EPDFPage_GetViewport(FPDF_PAGE page, unsigned long index) {
  ScopedFPDFPageView view(page);
  auto array = view ? view.Get()->GetDict()->GetArrayFor("VP") : nullptr;
  if (!array || !array->GetDictAt(index)) {
    return nullptr;
  }
  return reinterpret_cast<EPDF_VIEWPORT>(ViewportPath(view.Get(), index));
}
FPDF_EXPORT EPDF_VIEWPORT FPDF_CALLCONV
EPDFPage_AddViewport(FPDF_PAGE page, const FS_RECTF* bbox) {
  if (!Finite(bbox)) {
    return nullptr;
  }
  ScopedFPDFPageView view(page);
  if (!view) {
    return nullptr;
  }
  auto current = view.Get()->GetDict();
  if (current->KeyExist("VP") && !current->GetArrayFor("VP")) {
    return nullptr;
  }
  auto dict = view.Get()->GetMutableDict();
  CPDF_Measure::DetachIndirect(dict.Get(), "VP");
  auto array = dict->GetOrCreateArrayFor("VP");
  auto viewport = array->AppendNew<CPDF_Dictionary>();
  viewport->SetNewFor<CPDF_Name>("Type", "Viewport");
  CFX_FloatRect rect(bbox->left, bbox->bottom, bbox->right, bbox->top);
  rect.Normalize();
  viewport->SetRectFor("BBox", rect);
  return reinterpret_cast<EPDF_VIEWPORT>(
      ViewportPath(view.Get(), array->size() - 1));
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_RemoveViewport(FPDF_PAGE page, unsigned long index) {
  ScopedFPDFPageView view(page);
  auto current = view ? view.Get()->GetDict()->GetArrayFor("VP") : nullptr;
  if (!current || index >= current->size()) {
    return false;
  }
  auto dict = view.Get()->GetMutableDict();
  CPDF_Measure::DetachIndirect(dict.Get(), "VP");
  auto array = dict->GetMutableArrayFor("VP");
  array->RemoveAt(index);
  if (array->IsEmpty()) {
    dict->RemoveFor("VP");
  }
  auto* epochs = EpochsFor(view.Get(), nullptr);
  ++epochs->viewports;
  epochs->measures.clear();
  return true;
}
FPDF_EXPORT int FPDF_CALLCONV EPDFPage_FindViewport(FPDF_PAGE page,
                                                    const FS_POINTF* point) {
  ScopedFPDFPageView view(page);
  return view && Finite(point)
             ? CPDF_Measure::FindViewportForPoint(view.Get()->GetDict().Get(),
                                                  {point->x, point->y})
             : -1;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFViewport_GetBBox(EPDF_VIEWPORT viewport,
                                                         FS_RECTF* bbox) {
  PathView view(viewport, Kind::kViewport);
  if (!view.Get() || !bbox) {
    return false;
  }
  auto array = view.Get()->GetArrayFor("BBox");
  if (!array || array->size() != 4) {
    return false;
  }
  for (size_t i = 0; i < 4; ++i) {
    if (!array->GetNumberAt(i) || !std::isfinite(array->GetFloatAt(i))) {
      return false;
    }
  }
  auto rect = array->GetRect();
  rect.Normalize();
  *bbox = {rect.left, rect.top, rect.right, rect.bottom};
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFViewport_SetBBox(EPDF_VIEWPORT viewport,
                                                         const FS_RECTF* bbox) {
  if (!Finite(bbox)) {
    return false;
  }
  PathView view(viewport, Kind::kViewport);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  CFX_FloatRect rect(bbox->left, bbox->bottom, bbox->right, bbox->top);
  rect.Normalize();
  dict->SetRectFor("BBox", rect);
  return true;
}
FPDF_EXPORT EPDF_MEASURE FPDF_CALLCONV
EPDFViewport_GetMeasure(EPDF_VIEWPORT viewport) {
  PathView view(viewport, Kind::kViewport);
  if (!view.Get() || !view.Get()->GetDictFor("Measure")) {
    return nullptr;
  }
  Path path = *view.path();
  path.kind = Kind::kMeasure;
  return reinterpret_cast<EPDF_MEASURE>(Intern(path));
}
FPDF_EXPORT EPDF_MEASURE FPDF_CALLCONV
EPDFViewport_AddMeasure(EPDF_VIEWPORT viewport) {
  PathView view(viewport, Kind::kViewport);
  return view.Get() ? reinterpret_cast<EPDF_MEASURE>(AddMeasure(*view.path()))
                    : nullptr;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFViewport_RemoveMeasure(EPDF_VIEWPORT viewport) {
  PathView view(viewport, Kind::kViewport);
  return view.Get() && RemoveMeasure(*view.path());
}
FPDF_EXPORT EPDF_MEASURE_SUBTYPE FPDF_CALLCONV
EPDFMeasure_GetSubtype(EPDF_MEASURE measure) {
  PathView view(measure, Kind::kMeasure);
  return static_cast<EPDF_MEASURE_SUBTYPE>(CPDF_Measure::SubtypeOf(view.Get()));
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFMeasure_GetOrigin(EPDF_MEASURE measure,
                                                          FS_POINTF* origin) {
  PathView view(measure, Kind::kMeasure);
  return ReadPoint(view.Get(), "O", origin);
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFMeasure_SetOrigin(EPDF_MEASURE measure, const FS_POINTF* origin) {
  if (origin && !Finite(origin)) {
    return false;
  }
  PathView view(measure, Kind::kMeasure);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  WritePoint(dict.Get(), "O", origin);
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFMeasure_GetCYX(EPDF_MEASURE measure,
                                                       float* cyx) {
  PathView view(measure, Kind::kMeasure);
  return Number(view.Get(), "CYX", cyx);
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFMeasure_SetCYX(EPDF_MEASURE measure,
                                                       const float* cyx) {
  if (cyx && (!std::isfinite(*cyx) || *cyx <= 0)) {
    return false;
  }
  PathView view(measure, Kind::kMeasure);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  if (cyx) {
    dict->SetNewFor<CPDF_Number>("CYX", *cyx);
  } else {
    dict->RemoveFor("CYX");
  }
  return true;
}
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFMeasure_CountFormats(EPDF_MEASURE measure, EPDF_MEASURE_AXIS axis) {
  if (!ValidAxis(axis)) {
    return 0;
  }
  PathView view(measure, Kind::kMeasure);
  auto array = view.Get() ? view.Get()->GetArrayFor(CPDF_Measure::KeyForAxis(
                                static_cast<Axis>(axis)))
                          : nullptr;
  return array ? array->size() : 0;
}
FPDF_EXPORT EPDF_NUMBERFORMAT FPDF_CALLCONV
EPDFMeasure_GetFormat(EPDF_MEASURE measure,
                      EPDF_MEASURE_AXIS axis,
                      unsigned long index) {
  if (!ValidAxis(axis)) {
    return nullptr;
  }
  PathView view(measure, Kind::kMeasure);
  auto array = view.Get() ? view.Get()->GetArrayFor(CPDF_Measure::KeyForAxis(
                                static_cast<Axis>(axis)))
                          : nullptr;
  if (!array || !array->GetDictAt(index)) {
    return nullptr;
  }
  Path path = *view.path();
  path.kind = Kind::kFormat;
  path.axis = static_cast<Axis>(axis);
  path.index = index;
  return reinterpret_cast<EPDF_NUMBERFORMAT>(Intern(path));
}
FPDF_EXPORT EPDF_NUMBERFORMAT FPDF_CALLCONV
EPDFMeasure_AddFormat(EPDF_MEASURE measure,
                      EPDF_MEASURE_AXIS axis,
                      FPDF_WIDESTRING unit,
                      float conversion) {
  if (!ValidAxis(axis) || !unit || !std::isfinite(conversion) ||
      conversion <= 0) {
    return nullptr;
  }
  PathView view(measure, Kind::kMeasure);
  auto key = CPDF_Measure::KeyForAxis(static_cast<Axis>(axis));
  if (!view.Get() ||
      (view.Get()->KeyExist(key) && !view.Get()->GetArrayFor(key))) {
    return nullptr;
  }
  auto dict = view.Edit();
  if (!dict) {
    return nullptr;
  }
  CPDF_Measure::DetachIndirect(dict.Get(), key);
  auto array = dict->GetOrCreateArrayFor(key);
  auto format = array->AppendNew<CPDF_Dictionary>();
  format->SetNewFor<CPDF_Name>("Type", "NumberFormat");
  WriteText(format.Get(), "U", unit);
  format->SetNewFor<CPDF_Number>("C", conversion);
  Path path = *view.path();
  path.kind = Kind::kFormat;
  path.axis = static_cast<Axis>(axis);
  path.index = array->size() - 1;
  return reinterpret_cast<EPDF_NUMBERFORMAT>(Intern(path));
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFMeasure_RemoveFormats(EPDF_MEASURE measure, EPDF_MEASURE_AXIS axis) {
  if (!ValidAxis(axis)) {
    return false;
  }
  PathView view(measure, Kind::kMeasure);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  dict->RemoveFor(CPDF_Measure::KeyForAxis(static_cast<Axis>(axis)));
  ++view.path()
        ->epochs->measures[view.path()->viewport]
        .formats[static_cast<size_t>(axis)];
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFViewport_GetName(EPDF_VIEWPORT handle,
                     FPDF_WCHAR* buffer,
                     unsigned long buflen) {
  PathView view(handle, Kind::kViewport);
  return Text(view.Get(), "Name", buffer, buflen);
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFViewport_SetName(EPDF_VIEWPORT handle,
                                                         FPDF_WIDESTRING text) {
  PathView view(handle, Kind::kViewport);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  WriteText(dict.Get(), "Name", text);
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFMeasure_GetRatio(EPDF_MEASURE handle,
                     FPDF_WCHAR* buffer,
                     unsigned long buflen) {
  PathView view(handle, Kind::kMeasure);
  return Text(view.Get(), "R", buffer, buflen);
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFMeasure_SetRatio(EPDF_MEASURE handle,
                                                         FPDF_WIDESTRING text) {
  if (!text) {
    return false;
  }
  PathView view(handle, Kind::kMeasure);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  WriteText(dict.Get(), "R", text);
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFNumberFormat_GetUnit(EPDF_NUMBERFORMAT handle,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen) {
  PathView view(handle, Kind::kFormat);
  return Text(view.Get(), "U", buffer, buflen);
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetUnit(EPDF_NUMBERFORMAT handle, FPDF_WIDESTRING text) {
  if (!text) {
    return false;
  }
  PathView view(handle, Kind::kFormat);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  WriteText(dict.Get(), "U", text);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_GetConversion(EPDF_NUMBERFORMAT format, float* conversion) {
  PathView view(format, Kind::kFormat);
  return Number(view.Get(), "C", conversion);
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetConversion(EPDF_NUMBERFORMAT format, float conversion) {
  if (!std::isfinite(conversion) || conversion <= 0) {
    return false;
  }
  PathView view(format, Kind::kFormat);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  dict->SetNewFor<CPDF_Number>("C", conversion);
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_GetPrecision(EPDF_NUMBERFORMAT format, int* precision) {
  PathView view(format, Kind::kFormat);
  if (!view.Get() || !precision) {
    return false;
  }
  auto number = view.Get()->GetNumberFor("D");
  if (!number || !number->IsInteger() || number->GetInteger() < 1) {
    return false;
  }
  *precision = number->GetInteger();
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetPrecision(EPDF_NUMBERFORMAT format, int precision) {
  if (precision < 1) {
    return false;
  }
  PathView view(format, Kind::kFormat);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  dict->SetNewFor<CPDF_Number>("D", precision);
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_GetFixedDenominator(EPDF_NUMBERFORMAT format,
                                     FPDF_BOOL* fixed) {
  PathView view(format, Kind::kFormat);
  if (!view.Get() || !fixed) {
    return false;
  }
  auto value = view.Get()->GetDirectObjectFor("FD");
  if (!value || !value->IsBoolean()) {
    return false;
  }
  *fixed = value->GetInteger() != 0;
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetFixedDenominator(EPDF_NUMBERFORMAT format,
                                     FPDF_BOOL fixed) {
  PathView view(format, Kind::kFormat);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  dict->SetNewFor<CPDF_Boolean>("FD", !!fixed);
  return true;
}
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFNumberFormat_GetText(EPDF_NUMBERFORMAT format,
                         EPDF_MEASURE_TEXT kind,
                         FPDF_WCHAR* buffer,
                         unsigned long buflen) {
  const char* key = TextKey(kind);
  if (!key) {
    return 0;
  }
  PathView view(format, Kind::kFormat);
  return Text(view.Get(), key, buffer, buflen);
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetText(EPDF_NUMBERFORMAT format,
                         EPDF_MEASURE_TEXT kind,
                         FPDF_WIDESTRING text) {
  const char* key = TextKey(kind);
  if (!key) {
    return false;
  }
  PathView view(format, Kind::kFormat);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  WriteText(dict.Get(), key, text);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_GetFraction(EPDF_NUMBERFORMAT format,
                             EPDF_MEASURE_FRACTION* out) {
  PathView view(format, Kind::kFormat);
  if (!view.Get() || !out) {
    return false;
  }
  auto value = CPDF_Measure::FractionFromName(
      view.Get()->GetNameFor("F").AsStringView());
  if (!value) {
    return false;
  }
  *out = static_cast<EPDF_MEASURE_FRACTION>(*value);
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetFraction(EPDF_NUMBERFORMAT format,
                             EPDF_MEASURE_FRACTION value) {
  if (value < 0 || value > EPDF_MEASURE_FRACTION_TRUNCATE) {
    return false;
  }
  PathView view(format, Kind::kFormat);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  dict->SetNewFor<CPDF_Name>("F",
                             ByteString(CPDF_Measure::NameForFraction(
                                 static_cast<CPDF_Measure::Fraction>(value))));
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_GetLabelPosition(EPDF_NUMBERFORMAT format,
                                  EPDF_MEASURE_LABEL_POSITION* out) {
  PathView view(format, Kind::kFormat);
  if (!view.Get() || !out) {
    return false;
  }
  auto value = CPDF_Measure::LabelPositionFromName(
      view.Get()->GetNameFor("O").AsStringView());
  if (!value) {
    return false;
  }
  *out = static_cast<EPDF_MEASURE_LABEL_POSITION>(*value);
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFNumberFormat_SetLabelPosition(EPDF_NUMBERFORMAT format,
                                  EPDF_MEASURE_LABEL_POSITION value) {
  if (value < 0 || value > EPDF_MEASURE_LABEL_PREFIX) {
    return false;
  }
  PathView view(format, Kind::kFormat);
  auto dict = view.Edit();
  if (!dict) {
    return false;
  }
  dict->SetNewFor<CPDF_Name>(
      "O", ByteString(CPDF_Measure::NameForLabelPosition(
               static_cast<CPDF_Measure::LabelPosition>(value))));
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetLineLeader(FPDF_ANNOTATION annot,
                        float length,
                        float extension,
                        float offset) {
  if (!std::isfinite(length) || !std::isfinite(extension) ||
      !std::isfinite(offset) || extension < 0 || offset < 0) {
    return false;
  }
  ScopedFPDFAnnotationView view(annot);
  if (!IsLine(view.Get())) {
    return false;
  }
  auto dict = view.Get()->GetMutableAnnotDict();
  if (length == 0 && extension == 0 && offset == 0) {
    for (const char* key : {"LL", "LLE", "LLO"}) {
      dict->RemoveFor(key);
    }
  } else {
    dict->SetNewFor<CPDF_Number>("LL", length);
    dict->SetNewFor<CPDF_Number>("LLE", extension);
    dict->SetNewFor<CPDF_Number>("LLO", offset);
  }
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetLineLeader(FPDF_ANNOTATION annot,
                        float* length,
                        float* extension,
                        float* offset) {
  ScopedFPDFAnnotationView view(annot);
  if (!IsLine(view.Get()) || !length || !extension || !offset) {
    return false;
  }
  const auto* dict = view.Get()->GetAnnotDict();
  float l = 0, e = 0, o = 0;
  if ((dict->KeyExist("LL") && !Number(dict, "LL", &l)) ||
      (dict->KeyExist("LLE") && !Number(dict, "LLE", &e)) ||
      (dict->KeyExist("LLO") && !Number(dict, "LLO", &o)) || e < 0 || o < 0) {
    return false;
  }
  *length = l;
  *extension = e;
  *offset = o;
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetLineCaption(FPDF_ANNOTATION annot,
                         FPDF_BOOL enabled,
                         EPDF_CAPTION_POSITION position,
                         const EPDF_CAPTION_OFFSET* offset) {
  if (position < EPDF_CAPTION_INLINE || position > EPDF_CAPTION_TOP ||
      (offset && (!std::isfinite(offset->along) ||
                  !std::isfinite(offset->perpendicular)))) {
    return false;
  }
  ScopedFPDFAnnotationView view(annot);
  if (!IsLine(view.Get())) {
    return false;
  }
  auto dict = view.Get()->GetMutableAnnotDict();
  dict->SetNewFor<CPDF_Boolean>("Cap", !!enabled);
  dict->SetNewFor<CPDF_Name>("CP",
                             position == EPDF_CAPTION_TOP ? "Top" : "Inline");
  const FS_POINTF displacement =
      offset ? FS_POINTF{offset->along, offset->perpendicular} : FS_POINTF{};
  WritePoint(dict.Get(), "CO", offset ? &displacement : nullptr);
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetLineCaption(FPDF_ANNOTATION annot,
                         FPDF_BOOL* enabled,
                         EPDF_CAPTION_POSITION* position,
                         EPDF_CAPTION_OFFSET* offset) {
  ScopedFPDFAnnotationView view(annot);
  if (!IsLine(view.Get()) || !enabled || !position || !offset) {
    return false;
  }
  const auto* dict = view.Get()->GetAnnotDict();
  FS_POINTF value = {};
  if (dict->KeyExist("CO") && !ReadPoint(dict, "CO", &value)) {
    return false;
  }
  *enabled = dict->GetBooleanFor("Cap", false);
  *position =
      dict->GetNameFor("CP") == "Top" ? EPDF_CAPTION_TOP : EPDF_CAPTION_INLINE;
  *offset = {value.x, value.y};
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_SetShapeCaption(FPDF_ANNOTATION annot,
                          FPDF_BOOL enabled,
                          const FS_POINTF* center) {
  if (center && !Finite(center)) {
    return false;
  }
  ScopedFPDFAnnotationView view(annot);
  if (!IsShape(view.Get())) {
    return false;
  }
  const auto* current = view.Get()->GetAnnotDict();
  if (current->KeyExist("EMBD_Metadata") &&
      !current->GetDictFor("EMBD_Metadata")) {
    return false;
  }
  auto dict = view.Get()->GetMutableAnnotDict();
  CPDF_Measure::DetachIndirect(dict.Get(), "EMBD_Metadata");
  auto metadata = dict->GetOrCreateDictFor("EMBD_Metadata");
  if (!metadata->KeyExist("SchemaVersion")) {
    metadata->SetNewFor<CPDF_Number>("SchemaVersion", 1);
  }
  metadata->SetNewFor<CPDF_Boolean>(kShapeCaption, !!enabled);
  WritePoint(metadata.Get(), kShapeCenter, center);
  return true;
}
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFAnnot_GetShapeCaption(FPDF_ANNOTATION annot,
                          FPDF_BOOL* enabled,
                          FPDF_BOOL* has_center,
                          FS_POINTF* center) {
  ScopedFPDFAnnotationView view(annot);
  if (!IsShape(view.Get()) || !enabled || !has_center || !center) {
    return false;
  }
  auto metadata = view.Get()->GetAnnotDict()->GetDictFor("EMBD_Metadata");
  FS_POINTF value = {};
  bool present = metadata && metadata->KeyExist(kShapeCenter);
  if (present && !ReadPoint(metadata.Get(), kShapeCenter, &value)) {
    return false;
  }
  *enabled = metadata && metadata->GetBooleanFor(kShapeCaption, false);
  *has_center = present;
  *center = value;
  return true;
}
