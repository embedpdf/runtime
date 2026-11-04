// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/edit/cpdf_image_redactor.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_dib.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fxcodec/flate/flatemodule.h"
#include "core/fxcodec/jpeg/jpeg_writer.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/dib/fx_dib.h"

namespace {

// -- Grids --------------------------------------------------------------------

// Maps page-space rects into the image's sample grid (image-local).
static void PageRectsToImageGrid(const CFX_Matrix& image_to_page,
                                 int img_w,
                                 int img_h,
                                 pdfium::span<const CFX_FloatRect> page_rects,
                                 std::vector<CFX_FloatRect>* out_image_rects) {
  out_image_rects->clear();
  if (img_w <= 0 || img_h <= 0 || page_rects.empty()) {
    return;
  }

  // Step 1: page -> unit image space
  const CFX_Matrix page_to_unit = image_to_page.GetInverse();

  out_image_rects->reserve(page_rects.size());
  for (const auto& pr : page_rects) {
    // Page -> unit
    CFX_FloatRect ur = page_to_unit.TransformRect(pr);
    ur.Normalize();

    // Step 2: unit -> pixel
    CFX_FloatRect ir(ur.left * img_w, ur.bottom * img_h, ur.right * img_w,
                     ur.top * img_h);
    ir.Normalize();

    // Clamp to pixel bounds
    ir.left = std::clamp(ir.left, 0.0f, static_cast<float>(img_w));
    ir.right = std::clamp(ir.right, 0.0f, static_cast<float>(img_w));
    ir.bottom = std::clamp(ir.bottom, 0.0f, static_cast<float>(img_h));
    ir.top = std::clamp(ir.top, 0.0f, static_cast<float>(img_h));

    if (ir.right > ir.left && ir.top > ir.bottom) {
      out_image_rects->push_back(ir);
    }
  }
}

// EmbedPDF: the samples of an image grid the rects (from
// PageRectsToImageGrid()) cover any part of, as whole-sample boxes. Rows
// count from the bottom; x1 and y1 are one past the box.
struct GridBox {
  int x0, y0, x1, y1;
};

static std::vector<GridBox> GridBoxesOf(
    pdfium::span<const CFX_FloatRect> grid_rects,
    int width,
    int height) {
  std::vector<GridBox> boxes;
  boxes.reserve(grid_rects.size());
  for (const CFX_FloatRect& r : grid_rects) {
    GridBox b;
    b.x0 = std::max(0, std::min(width, static_cast<int>(std::floor(r.left))));
    b.x1 = std::max(0, std::min(width, static_cast<int>(std::ceil(r.right))));
    b.y0 =
        std::max(0, std::min(height, static_cast<int>(std::floor(r.bottom))));
    b.y1 = std::max(0, std::min(height, static_cast<int>(std::ceil(r.top))));
    if (b.x1 > b.x0 && b.y1 > b.y0) {
      boxes.push_back(b);
    }
  }
  return boxes;
}

// -- Format kept --------------------------------------------------------------

// What a redacted sample becomes.
enum class Hole {
  kPaper,      // An image's: its colour space's paper (white, no ink).
  kUnpainted,  // A stencil image's: nothing painted.
  kPainted,    // An explicit mask's: painted, so the image's hole shows.
  kOpaque,     // A soft mask's: fully opaque, as an image's hole is.
};

// An image's samples as its filters leave them, before its colour space
// reads them: `height` rows of `pitch` bytes, the first row the top one.
struct Samples {
  int width = 0;
  int height = 0;
  int components = 0;
  int bpc = 0;
  size_t pitch = 0;
  DataVector<uint8_t> data;
};

bool IsSampleDepth(int bpc) {
  return bpc == 1 || bpc == 2 || bpc == 4 || bpc == 8 || bpc == 16;
}

// Whether the image reads its samples inverted from how CPDF_DIB hands out
// 1-bit rows: a stencil read with the default /Decode, which CPDF_DIB inverts
// so that 1 is painted.
bool DibInvertsRows(const CPDF_Dictionary& dict) {
  const bool stencil =
      dict.GetBooleanFor("ImageMask", false) || !dict.KeyExist("ColorSpace");
  RetainPtr<const CPDF_Array> decode = dict.GetArrayFor("Decode");
  return stencil && (!decode || !decode->GetIntegerAt(0));
}

// `stream`'s samples, `acc` its data through every filter that leaves
// samples; nullopt when its filters don't leave plain samples (JPEG, JPEG
// 2000) or they can't be read.
std::optional<Samples> DecodeSamples(CPDF_Document* doc,
                                     RetainPtr<const CPDF_Stream> stream,
                                     const CPDF_StreamAcc& acc,
                                     int components,
                                     int bpc) {
  RetainPtr<const CPDF_Dictionary> dict = stream->GetDict();
  Samples samples;
  samples.width = dict->GetIntegerFor("Width");
  samples.height = dict->GetIntegerFor("Height");
  samples.components = components;
  samples.bpc = bpc;
  if (samples.width <= 0 || samples.height <= 0 || components <= 0 ||
      !IsSampleDepth(bpc)) {
    return std::nullopt;
  }
  FX_SAFE_SIZE_T pitch = samples.width;
  pitch *= components;
  pitch *= bpc;
  pitch += 7;
  pitch /= 8;
  FX_SAFE_SIZE_T size = pitch;
  size *= samples.height;
  if (!size.IsValid()) {
    return std::nullopt;
  }
  samples.pitch = pitch.ValueOrDie();
  samples.data.resize(size.ValueOrDie());

  const ByteString decoder = acc.GetImageDecoder();
  if (decoder.IsEmpty()) {
    // Short data reads as zeros, as CPDF_DIB reads it.
    pdfium::span<const uint8_t> data = acc.GetSpan();
    const size_t n = std::min(data.size(), samples.data.size());
    fxcrt::Copy(data.first(n), samples.data);
    return samples;
  }
  if ((decoder != "CCITTFaxDecode" && decoder != "JBIG2Decode") ||
      components * bpc != 1) {
    return std::nullopt;
  }
  // These two decode only through CPDF_DIB (JBIG2 with its globals), whose
  // 1-bit rows are the samples.
  auto dib = pdfium::MakeRetain<CPDF_DIB>(doc, stream);
  if (!dib->Load()) {
    return std::nullopt;
  }
  CPDF_DIB::LoadState state = CPDF_DIB::LoadState::kSuccess;
  if (dib->IsJBigImage()) {
    do {
      state = dib->ContinueLoadDIBBase(nullptr);
    } while (state == CPDF_DIB::LoadState::kContinue);
  }
  if (state != CPDF_DIB::LoadState::kSuccess || dib->GetBPP() != 1 ||
      dib->GetWidth() != samples.width || dib->GetHeight() != samples.height) {
    return std::nullopt;
  }
  const uint8_t flip = DibInvertsRows(*dict) ? 0xFF : 0x00;
  for (int row = 0; row < samples.height; ++row) {
    pdfium::span<uint8_t> out =
        pdfium::span(samples.data)
            .subspan(static_cast<size_t>(row) * samples.pitch, samples.pitch);
    pdfium::span<const uint8_t> line = dib->GetScanline(row);
    if (line.size() < samples.pitch) {
      return std::nullopt;
    }
    for (size_t i = 0; i < samples.pitch; ++i) {
      out[i] = line[i] ^ flip;
    }
  }
  return samples;
}

// The range component `index` of `dict`'s samples decodes to: its /Decode
// pair, else the colour space's default (an index for Indexed).
std::pair<float, float> DecodeRange(const CPDF_Dictionary& dict,
                                    const CPDF_ColorSpace* color_space,
                                    int index,
                                    int components,
                                    uint32_t max_sample) {
  RetainPtr<const CPDF_Array> decode = dict.GetArrayFor("Decode");
  if (decode && decode->size() >= static_cast<size_t>(components) * 2) {
    return {decode->GetFloatAt(index * 2), decode->GetFloatAt(index * 2 + 1)};
  }
  if (!color_space) {
    return {0.0f, 1.0f};
  }
  if (color_space->GetFamily() == CPDF_ColorSpace::Family::kIndexed) {
    return {0.0f, static_cast<float>(max_sample)};
  }
  float value = 0.0f;
  float min = 0.0f;
  float max = 1.0f;
  color_space->GetDefaultValue(index, &value, &min, &max);
  return {min, max};
}

uint32_t SampleFor(float value,
                   std::pair<float, float> range,
                   uint32_t max_sample) {
  const float span = range.second - range.first;
  if (span == 0.0f) {
    return 0;
  }
  const float scaled = std::round((value - range.first) / span * max_sample);
  return static_cast<uint32_t>(
      std::clamp(scaled, 0.0f, static_cast<float>(max_sample)));
}

float Lightness(const CPDF_ColorSpace* color_space,
                pdfium::span<const float> values) {
  const FX_RGB_STRUCT<float> rgb = color_space->GetRGBOrZerosOnError(values);
  return rgb.red + rgb.green + rgb.blue;
}

// The decoded values of `color_space`'s paper: white, or no ink. Nullopt for
// a colour space an image can't use.
std::optional<std::vector<float>> PaperValues(
    const CPDF_Dictionary& dict,
    const CPDF_ColorSpace* color_space,
    int components,
    uint32_t max_sample) {
  using Family = CPDF_ColorSpace::Family;
  switch (color_space->GetFamily()) {
    case Family::kDeviceGray:
    case Family::kCalGray:
    case Family::kDeviceRGB:
    case Family::kCalRGB:
      return std::vector<float>(components, 1.0f);
    case Family::kDeviceCMYK:
    case Family::kSeparation:
    case Family::kDeviceN:
      return std::vector<float>(components, 0.0f);
    case Family::kLab:
      return std::vector<float>{100.0f, 0.0f, 0.0f};
    case Family::kICCBased: {
      // A profile can be gray, RGB, CMYK or Lab: the lighter of its range's
      // two ends.
      std::vector<float> low;
      std::vector<float> high;
      for (int i = 0; i < components; ++i) {
        const auto range =
            DecodeRange(dict, color_space, i, components, max_sample);
        low.push_back(range.first);
        high.push_back(range.second);
      }
      return Lightness(color_space, high) >= Lightness(color_space, low) ? high
                                                                         : low;
    }
    case Family::kIndexed: {
      // The lightest entry.
      float best = -1.0f;
      float best_index = 0.0f;
      for (uint32_t index = 0; index <= max_sample; ++index) {
        const float value = static_cast<float>(index);
        const float lightness =
            Lightness(color_space, pdfium::span_from_ref(value));
        if (lightness > best) {
          best = lightness;
          best_index = value;
        }
      }
      return std::vector<float>{best_index};
    }
    case Family::kPattern:
    case Family::kUnknown:
      return std::nullopt;
  }
  return std::nullopt;
}

// The sample, per component, a hole of `hole` holds in `dict`'s image.
std::optional<std::vector<uint32_t>> HoleSamples(
    Hole hole,
    const CPDF_Dictionary& dict,
    const CPDF_ColorSpace* color_space,
    int components,
    int bpc) {
  const uint32_t max_sample = (1u << bpc) - 1;
  if (hole == Hole::kPainted || hole == Hole::kUnpainted) {
    // A stencil paints where its sample decodes to 0 (ISO 32000-1 §8.9.6.2).
    RetainPtr<const CPDF_Array> decode = dict.GetArrayFor("Decode");
    const bool inverted = decode && decode->GetIntegerAt(0);
    const bool painted = hole == Hole::kPainted;
    return std::vector<uint32_t>{painted != inverted ? 0u : 1u};
  }
  std::vector<float> values;
  if (hole == Hole::kOpaque) {
    values.assign(components, 1.0f);
  } else {
    std::optional<std::vector<float>> paper =
        PaperValues(dict, color_space, components, max_sample);
    if (!paper.has_value() ||
        paper->size() != static_cast<size_t>(components)) {
      return std::nullopt;
    }
    values = std::move(paper.value());
  }
  std::vector<uint32_t> samples;
  for (int i = 0; i < components; ++i) {
    samples.push_back(SampleFor(
        values[i], DecodeRange(dict, color_space, i, components, max_sample),
        max_sample));
  }
  return samples;
}

void SetSample(pdfium::span<uint8_t> row,
               size_t index,
               int bpc,
               uint32_t value) {
  if (bpc == 8) {
    row[index] = static_cast<uint8_t>(value);
    return;
  }
  if (bpc == 16) {
    row[index * 2] = static_cast<uint8_t>(value >> 8);
    row[index * 2 + 1] = static_cast<uint8_t>(value);
    return;
  }
  const size_t bit = index * bpc;
  const int shift = 8 - bpc - static_cast<int>(bit % 8);
  const uint8_t mask = static_cast<uint8_t>(((1u << bpc) - 1) << shift);
  uint8_t& byte = row[bit / 8];
  byte = static_cast<uint8_t>((byte & ~mask) | ((value << shift) & mask));
}

// Sets every sample in `boxes` to `hole`.
void FillHole(Samples& samples,
              pdfium::span<const GridBox> boxes,
              pdfium::span<const uint32_t> hole) {
  for (const GridBox& box : boxes) {
    for (int y = box.y0; y < box.y1; ++y) {
      const size_t row = static_cast<size_t>(samples.height - 1 - y);
      pdfium::span<uint8_t> line =
          pdfium::span(samples.data)
              .subspan(row * samples.pitch, samples.pitch);
      for (int x = box.x0; x < box.x1; ++x) {
        for (int c = 0; c < samples.components; ++c) {
          SetSample(line, static_cast<size_t>(x) * samples.components + c,
                    samples.bpc, hole[c]);
        }
      }
    }
  }
}

// An image's redacted content, ready to be written: its samples, written
// with Flate, or its JPEG, written as is.
struct Content {
  int width = 0;
  int height = 0;
  std::optional<Samples> samples;
  DataVector<uint8_t> jpeg;
  // The JPEG's /ColorTransform, when the image's /DCTDecode had one.
  std::optional<int> color_transform;
  // A decoded picture's device colour space (8 bits a component), written
  // in place of the source's.
  const char* color_space = nullptr;
  // Samples written at zlib's fastest level: a decoded picture's, compressed
  // again on every further apply to the image.
  bool fast = false;
  // For a soft mask: the colour its image's colours are blended with, in
  // that image's colour space (/Matte, ISO 32000-1 §11.6.5.3).
  std::vector<float> matte;
};

// A new image stream holding `content`, with `keys` copied from `source` (and
// nothing else of it).
RetainPtr<CPDF_Stream> WriteContent(CPDF_Document* doc,
                                    const CPDF_Dictionary& source,
                                    pdfium::span<const char* const> keys,
                                    const Content& content) {
  RetainPtr<CPDF_Dictionary> dict = doc->New<CPDF_Dictionary>();
  dict->SetNewFor<CPDF_Name>("Type", "XObject");
  dict->SetNewFor<CPDF_Name>("Subtype", "Image");
  dict->SetNewFor<CPDF_Number>("Width", content.width);
  dict->SetNewFor<CPDF_Number>("Height", content.height);
  for (const char* key : keys) {
    if (RetainPtr<const CPDF_Object> value = source.GetObjectFor(key)) {
      dict->SetFor(key, value->CloneForHolder(doc));
    }
  }
  if (content.color_space) {
    dict->SetNewFor<CPDF_Name>("ColorSpace", content.color_space);
    dict->SetNewFor<CPDF_Number>("BitsPerComponent", 8);
  }
  if (!content.matte.empty()) {
    RetainPtr<CPDF_Array> matte = dict->SetNewFor<CPDF_Array>("Matte");
    for (float value : content.matte) {
      matte->AppendNew<CPDF_Number>(value);
    }
  }
  DataVector<uint8_t> data;
  if (content.samples.has_value()) {
    dict->SetNewFor<CPDF_Name>("Filter", "FlateDecode");
    data = content.fast ? fxcodec::FlateModule::Encode(content.samples->data, 1)
                        : fxcodec::FlateModule::Encode(content.samples->data);
  } else {
    dict->SetNewFor<CPDF_Name>("Filter", "DCTDecode");
    if (content.color_transform.has_value()) {
      dict->SetNewFor<CPDF_Dictionary>("DecodeParms")
          ->SetNewFor<CPDF_Number>("ColorTransform",
                                   content.color_transform.value());
    }
    data = content.jpeg;
  }
  auto stream =
      pdfium::MakeRetain<CPDF_Stream>(std::move(data), std::move(dict));
  if (doc->AddIndirectObject(stream) == 0) {
    return nullptr;
  }
  return stream;
}

// What a replacement keeps of the image, a soft mask and a mask: what says
// how, where and when it is drawn. Metadata, alternate versions and OPI
// links are other copies of the pixels; /Filter and /DecodeParms belong to
// the old bytes.
constexpr const char* kImageKeys[] = {
    "ColorSpace", "BitsPerComponent", "Decode", "ImageMask",
    "Intent",     "Interpolate",      "OC",     "StructParent"};
constexpr const char* kSoftMaskKeys[] = {"ColorSpace", "BitsPerComponent",
                                         "Decode", "Matte", "Interpolate"};
constexpr const char* kMaskKeys[] = {"ImageMask", "BitsPerComponent", "Decode",
                                     "Interpolate"};

// Writes `content`, a soft mask or mask copied from `source` with `keys`, and
// points `image`'s `key` at it. False when it can't be written.
bool WriteMask(CPDF_Document* doc,
               CPDF_Dictionary* image,
               const char* key,
               const CPDF_Dictionary& source,
               pdfium::span<const char* const> keys,
               const Content& content) {
  RetainPtr<CPDF_Stream> stream = WriteContent(doc, source, keys, content);
  if (!stream) {
    return false;
  }
  image->SetNewFor<CPDF_Reference>(key, doc, stream->GetObjNum());
  return true;
}

// Images (and their masks) may be reused by other placements or pages: only
// this placement draws `replacement`.
RedactResult ReplaceImage(CPDF_Page* page,
                          CPDF_ImageObject* image_object,
                          const CPDF_Stream& replacement) {
  image_object->SetImage(pdfium::MakeRetain<CPDF_Image>(
      page->GetDocument(), replacement.GetObjNum()));
  page->ClearRenderContext();
  image_object->SetDirty(true);
  return RedactResult{.changed = true};
}

// `dict`'s image grid boxes under `page_rects`; `image_to_page` places the
// unit square the image fills.
std::vector<GridBox> HoleBoxes(int width,
                               int height,
                               const CFX_Matrix& image_to_page,
                               pdfium::span<const CFX_FloatRect> page_rects) {
  std::vector<CFX_FloatRect> grid_rects;
  PageRectsToImageGrid(image_to_page, width, height, page_rects, &grid_rects);
  return GridBoxesOf(grid_rects, width, height);
}

// A JPEG's content with the hole filled in its blocks (FillJpegRects()):
// the blocks away from the hole are its own, the size about the same.
std::optional<Content> RedactedJpeg(
    const CPDF_StreamAcc& acc,
    const CPDF_Dictionary& dict,
    const CPDF_ColorSpace* color_space,
    Hole hole,
    const CFX_Matrix& image_to_page,
    pdfium::span<const CFX_FloatRect> page_rects) {
  Content content;
  content.width = dict.GetIntegerFor("Width");
  content.height = dict.GetIntegerFor("Height");
  if (!color_space || dict.GetIntegerFor("BitsPerComponent", 8) != 8) {
    return std::nullopt;
  }
  const int components = color_space->ComponentCount();
  std::optional<std::vector<uint32_t>> fill =
      HoleSamples(hole, dict, color_space, components, 8);
  if (!fill.has_value()) {
    return std::nullopt;
  }
  std::vector<uint8_t> fill_bytes(fill->begin(), fill->end());
  // Grid boxes count rows from the bottom; a JPEG's from the top.
  std::vector<fxcodec::JpegRect> rects;
  for (const GridBox& box :
       HoleBoxes(content.width, content.height, image_to_page, page_rects)) {
    rects.push_back({.left = box.x0,
                     .top = content.height - box.y1,
                     .right = box.x1,
                     .bottom = content.height - box.y0});
  }
  // The colour transform as CPDF_DIB reads it.
  RetainPtr<const CPDF_Dictionary> params = acc.GetImageParam();
  const bool color_transform =
      !params || params->GetIntegerFor("ColorTransform", 1);
  std::optional<DataVector<uint8_t>> jpeg =
      fxcodec::FillJpegRects(acc.GetSpan(), content.width, content.height,
                             components, color_transform, rects, fill_bytes);
  if (!jpeg.has_value()) {
    return std::nullopt;
  }
  content.jpeg = std::move(jpeg.value());
  if (params && params->KeyExist("ColorTransform")) {
    content.color_transform = params->GetIntegerFor("ColorTransform");
  }
  return content;
}

// `stream`'s content (the image's, its soft mask's or its mask's) with what
// lies under `page_rects` set to `hole`: a JPEG stays a JPEG, other images
// keep their samples. `image_to_page` places the unit square every one of
// them fills. Nullopt when it can't be kept.
std::optional<Content> RedactedContent(
    CPDF_Document* doc,
    RetainPtr<const CPDF_Stream> stream,
    const CPDF_ColorSpace* color_space,
    Hole hole,
    const CFX_Matrix& image_to_page,
    pdfium::span<const CFX_FloatRect> page_rects) {
  RetainPtr<const CPDF_Dictionary> dict = stream->GetDict();
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(stream);
  acc->LoadAllDataFiltered();
  if (acc->GetImageDecoder() == "DCTDecode") {
    return RedactedJpeg(*acc, *dict, color_space, hole, image_to_page,
                        page_rects);
  }
  const bool stencil = hole == Hole::kPainted || hole == Hole::kUnpainted;
  const int components = stencil ? 1 : color_space->ComponentCount();
  const int bpc = stencil ? 1 : dict->GetIntegerFor("BitsPerComponent");
  std::optional<Samples> samples =
      DecodeSamples(doc, stream, *acc, components, bpc);
  if (!samples.has_value()) {
    return std::nullopt;
  }
  std::optional<std::vector<uint32_t>> fill =
      HoleSamples(hole, *dict, color_space, components, bpc);
  if (!fill.has_value()) {
    return std::nullopt;
  }
  FillHole(
      samples.value(),
      HoleBoxes(samples->width, samples->height, image_to_page, page_rects),
      fill.value());
  Content content;
  content.width = samples->width;
  content.height = samples->height;
  content.samples = std::move(samples);
  return content;
}

// The colour space `dict`'s /ColorSpace names, read as CPDF_DIB reads it.
RetainPtr<CPDF_ColorSpace> ColorSpaceOf(CPDF_Document* doc,
                                        const CPDF_Dictionary& dict,
                                        const CPDF_Dictionary* resources,
                                        const CPDF_Dictionary* page_resources) {
  RetainPtr<const CPDF_Object> object = dict.GetDirectObjectFor("ColorSpace");
  if (!object) {
    return nullptr;
  }
  auto* page_data = CPDF_DocPageData::FromDocument(doc);
  RetainPtr<CPDF_ColorSpace> color_space =
      resources ? page_data->GetColorSpace(object.Get(), resources) : nullptr;
  return color_space ? color_space
                     : page_data->GetColorSpace(object.Get(), page_resources);
}

// Redacts `image_object` keeping its format (see RedactImageObject()), or
// nullopt when it can't be kept: then the image is rewritten as RGB.
std::optional<RedactResult> RedactKeepingFormat(
    CPDF_Page* page,
    CPDF_ImageObject* image_object,
    const CPDF_Dictionary* resources,
    pdfium::span<const CFX_FloatRect> page_rects,
    const CFX_Matrix& image_to_page) {
  CPDF_Image* image = image_object->GetImage();
  CPDF_Document* doc = page->GetDocument();
  RetainPtr<const CPDF_Stream> stream = image ? image->GetStream() : nullptr;
  if (!stream || !doc) {
    return std::nullopt;
  }
  RetainPtr<const CPDF_Dictionary> dict = stream->GetDict();
  RetainPtr<const CPDF_Dictionary> page_resources = page->GetResources();

  const bool stencil = image->IsMask();
  RetainPtr<CPDF_ColorSpace> color_space =
      stencil ? nullptr
              : ColorSpaceOf(doc, *dict, resources, page_resources.Get());
  if (!stencil && !color_space) {
    return std::nullopt;
  }

  // Nothing of it under the regions: unchanged.
  std::vector<CFX_FloatRect> grid_rects;
  PageRectsToImageGrid(image_to_page, image->GetPixelWidth(),
                       image->GetPixelHeight(), page_rects, &grid_rects);
  if (GridBoxesOf(grid_rects, image->GetPixelWidth(), image->GetPixelHeight())
          .empty()) {
    return RedactResult();
  }

  // Its masks are kept the same way, or the image is not.
  RetainPtr<const CPDF_Stream> soft_mask = dict->GetStreamFor("SMask");
  RetainPtr<CPDF_ColorSpace> soft_mask_space;
  if (soft_mask) {
    soft_mask_space = ColorSpaceOf(doc, *soft_mask->GetDict(), resources,
                                   page_resources.Get());
    if (!soft_mask_space || soft_mask_space->ComponentCount() != 1) {
      return std::nullopt;
    }
  }
  RetainPtr<const CPDF_Stream> mask =
      soft_mask ? nullptr : dict->GetStreamFor("Mask");

  // Everything is read before anything is written.
  std::optional<Content> content = RedactedContent(
      doc, stream, color_space.Get(), stencil ? Hole::kUnpainted : Hole::kPaper,
      image_to_page, page_rects);
  std::optional<Content> soft_mask_content;
  if (soft_mask) {
    soft_mask_content =
        RedactedContent(doc, soft_mask, soft_mask_space.Get(), Hole::kOpaque,
                        image_to_page, page_rects);
  }
  std::optional<Content> mask_content;
  if (mask) {
    mask_content = RedactedContent(doc, mask, nullptr, Hole::kPainted,
                                   image_to_page, page_rects);
  }
  if (!content.has_value() || (soft_mask && !soft_mask_content.has_value()) ||
      (mask && !mask_content.has_value())) {
    return std::nullopt;
  }

  RetainPtr<CPDF_Stream> replacement =
      WriteContent(doc, *dict, kImageKeys, content.value());
  if (!replacement) {
    return RedactResult{.succeeded = false};
  }
  RetainPtr<CPDF_Dictionary> replacement_dict = replacement->GetMutableDict();
  if (soft_mask) {
    if (!WriteMask(doc, replacement_dict.Get(), "SMask", *soft_mask->GetDict(),
                   kSoftMaskKeys, soft_mask_content.value())) {
      return RedactResult{.succeeded = false};
    }
  } else if (mask) {
    if (!WriteMask(doc, replacement_dict.Get(), "Mask", *mask->GetDict(),
                   kMaskKeys, mask_content.value())) {
      return RedactResult{.succeeded = false};
    }
  } else if (RetainPtr<const CPDF_Array> color_key =
                 dict->GetArrayFor("Mask")) {
    // Colour-key ranges are sample values, which the samples keep.
    replacement_dict->SetFor("Mask", color_key->CloneForHolder(doc));
  }
  return ReplaceImage(page, image_object, *replacement);
}

// -- Decoded ------------------------------------------------------------------

// The quality a decoded lossy image (a JPEG 2000, a damaged JPEG) is written
// at as a JPEG: one more generation, which doesn't show on a scan.
constexpr int kJpegQuality = 90;

// What a replacement keeps of an image it decoded: how, where and when it is
// drawn. The writer sets the colour space and bits.
constexpr const char* kDecodedKeys[] = {"Intent", "Interpolate", "OC",
                                        "StructParent"};
// What a decoded image's soft mask keeps, when it keeps its format and when
// it is decoded too. Not its /Matte: that named a colour of the image's old
// colour space, and is written in the new one (MatteOf()).
constexpr const char* kKeptSoftMaskOfDecodedKeys[] = {
    "ColorSpace", "BitsPerComponent", "Decode", "Interpolate"};
constexpr const char* kDecodedSoftMaskKeys[] = {"Interpolate"};

// What CPDF_DIB shows of an image: 8-bit gray or RGB, rows from the top, and
// its alpha (a colour key's, a palette's, a JPEG 2000's own, a stencil's) when
// it has any.
struct Picture {
  int width = 0;
  int height = 0;
  int components = 0;
  DataVector<uint8_t> pixels;
  DataVector<uint8_t> alpha;
  // Its colours are blended with white where `alpha` is less than opaque.
  bool white_blended = false;
};

// The DIBs an image decodes to: the image as CPDF_DIB shows it, and the alpha
// a JPEG 2000 keeps inside (/SMaskInData 1). CPDF_DIB blends such an image's
// colours with white and hands its alpha out as a mask.
struct Decoded {
  RetainPtr<CFX_DIBBase> image;
  RetainPtr<CFX_DIBBase> inline_alpha;
  // The colour (0x00RRGGBB) the image's colours are blended with, from its
  // soft mask's /Matte, when CPDF_DIB reads one: the renderer takes it out
  // again.
  std::optional<uint32_t> matte;
};

// `stream` decoded as CPDF_DIB decodes it to draw it (its /ColorSpace name
// read in `resources`, else `page_resources`), at full resolution.
std::optional<Decoded> Decode(CPDF_Document* doc,
                              RetainPtr<const CPDF_Stream> stream,
                              const CPDF_Dictionary* resources,
                              const CPDF_Dictionary* page_resources) {
  RetainPtr<const CPDF_Dictionary> dict = stream->GetDict();
  const bool inline_alpha = dict->GetIntegerFor("SMaskInData") == 1 &&
                            !dict->KeyExist("SMask") && !dict->KeyExist("Mask");
  RetainPtr<const CPDF_Stream> soft_mask = dict->GetStreamFor("SMask");
  const bool matte = soft_mask && soft_mask->GetDict()->KeyExist("Matte");
  // CPDF_DIB reads both only when it loads the image's mask.
  const bool load_mask = inline_alpha || matte;
  auto dib = pdfium::MakeRetain<CPDF_DIB>(doc, std::move(stream));
  CPDF_DIB::LoadState state = dib->StartLoadDIBBase(
      load_mask, resources, page_resources, CPDF_ColorSpace::Family::kUnknown,
      /*bLoadMask=*/load_mask, CFX_Size());
  while (state == CPDF_DIB::LoadState::kContinue) {
    state = dib->ContinueLoadDIBBase(nullptr);
  }
  if (state != CPDF_DIB::LoadState::kSuccess) {
    return std::nullopt;
  }
  Decoded decoded;
  if (inline_alpha) {
    decoded.inline_alpha = dib->DetachMask();
  }
  // 0xFFFFFFFF: none, or none that fits the image's colour space.
  if (matte && dib->GetMatteColor() != 0xFFFFFFFF) {
    decoded.matte = dib->GetMatteColor();
  }
  decoded.image = std::move(dib);
  return decoded;
}

bool IsGray(uint32_t argb) {
  return (argb >> 16 & 0xff) == (argb >> 8 & 0xff) &&
         (argb >> 8 & 0xff) == (argb & 0xff);
}

// What `decoded` shows. A stencil paints `stencil_fill` (0x00BBGGRR). Nullopt
// when it can't be read.
std::optional<Picture> DecodePicture(const Decoded& decoded,
                                     bool stencil,
                                     FX_COLORREF stencil_fill) {
  const RetainPtr<CFX_DIBBase>& dib = decoded.image;
  const int bpp = dib->GetBPP();
  if ((bpp != 1 && bpp != 8 && bpp != 24 && bpp != 32) ||
      (bpp == 8 && dib->IsMaskFormat())) {
    return std::nullopt;
  }
  const pdfium::span<const uint32_t> palette = dib->GetPaletteSpan();
  // Gray when every colour it can show is, and the colour it's blended with.
  const bool gray =
      !stencil && (bpp == 1 || bpp == 8) &&
      std::ranges::all_of(palette, IsGray) &&
      (!decoded.matte.has_value() || IsGray(decoded.matte.value()));
  const bool palette_alpha = std::ranges::any_of(
      palette, [](uint32_t argb) { return argb >> 24 != 0xff; });
  const bool has_alpha = stencil || palette_alpha ||
                         (bpp == 32 && dib->IsAlphaFormat()) ||
                         decoded.inline_alpha;

  Picture picture;
  picture.width = dib->GetWidth();
  picture.height = dib->GetHeight();
  picture.components = gray ? 1 : 3;
  FX_SAFE_SIZE_T area = picture.width;
  area *= picture.height;
  FX_SAFE_SIZE_T size = area;
  size *= picture.components;
  if (!size.IsValid() || picture.width <= 0 || picture.height <= 0) {
    return std::nullopt;
  }
  // A row the decoder can't give stays white and opaque.
  picture.pixels.resize(size.ValueOrDie(), 0xff);
  if (has_alpha) {
    picture.alpha.resize(area.ValueOrDie(), 0xff);
  }
  const uint32_t stencil_argb = 0xff000000 | (stencil_fill & 0xff) << 16 |
                                (stencil_fill & 0xff00) |
                                (stencil_fill >> 16 & 0xff);
  for (int row = 0; row < picture.height; ++row) {
    pdfium::span<const uint8_t> line = dib->GetScanline(row);
    if (line.empty()) {
      continue;
    }
    const size_t first = static_cast<size_t>(row) * picture.width;
    for (int x = 0; x < picture.width; ++x) {
      uint32_t argb = 0;
      if (bpp == 1) {
        const bool bit = line[x / 8] >> (7 - x % 8) & 1;
        if (stencil) {
          // CPDF_DIB reads a stencil with 1 as painted.
          argb = bit ? stencil_argb : 0x00ffffff;
        } else if (palette.empty()) {
          argb = bit ? 0xffffffff : 0xff000000;
        } else {
          argb = palette[bit];
        }
      } else if (bpp == 8) {
        const uint8_t value = line[x];
        argb = palette.empty() ? 0xff000000 | value << 16 | value << 8 | value
                               : palette[value];
      } else {
        const size_t at = static_cast<size_t>(x) * (bpp / 8);
        const uint32_t alpha =
            bpp == 32 && dib->IsAlphaFormat() ? line[at + 3] : 0xff;
        argb = alpha << 24 | line[at + 2] << 16 | line[at + 1] << 8 | line[at];
      }
      const size_t pixel = first + x;
      if (gray) {
        picture.pixels[pixel] = argb & 0xff;
      } else {
        picture.pixels[pixel * 3] = argb >> 16 & 0xff;
        picture.pixels[pixel * 3 + 1] = argb >> 8 & 0xff;
        picture.pixels[pixel * 3 + 2] = argb & 0xff;
      }
      if (has_alpha) {
        picture.alpha[pixel] = argb >> 24;
      }
    }
  }
  if (decoded.inline_alpha) {
    const RetainPtr<CFX_DIBBase>& alpha = decoded.inline_alpha;
    if (alpha->GetBPP() != 8 || alpha->GetWidth() != picture.width ||
        alpha->GetHeight() != picture.height) {
      return std::nullopt;
    }
    // A gray image: its palette, when it has one, holds the gray levels.
    const pdfium::span<const uint32_t> levels = alpha->GetPaletteSpan();
    for (int row = 0; row < picture.height; ++row) {
      pdfium::span<const uint8_t> line = alpha->GetScanline(row);
      if (line.size() < static_cast<size_t>(picture.width)) {
        return std::nullopt;
      }
      const size_t first = static_cast<size_t>(row) * picture.width;
      for (int x = 0; x < picture.width; ++x) {
        picture.alpha[first + x] =
            levels.empty() ? line[x] : levels[line[x]] & 0xff;
      }
    }
    picture.white_blended = true;
  }
  return picture;
}

// Sets the pixels in `boxes` (rows from the bottom) to `fill`, opaque.
void FillPicture(Picture& picture,
                 pdfium::span<const GridBox> boxes,
                 uint8_t fill) {
  const size_t pitch = static_cast<size_t>(picture.width) * picture.components;
  for (const GridBox& box : boxes) {
    const size_t width = static_cast<size_t>(box.x1 - box.x0);
    for (int y = box.y0; y < box.y1; ++y) {
      const size_t row = static_cast<size_t>(picture.height - 1 - y);
      std::ranges::fill(pdfium::span(picture.pixels)
                            .subspan(row * pitch + box.x0 * picture.components,
                                     width * picture.components),
                        fill);
      if (!picture.alpha.empty()) {
        std::ranges::fill(pdfium::span(picture.alpha)
                              .subspan(row * picture.width + box.x0, width),
                          0xff);
      }
    }
  }
}

// Whether `dict`'s image is stored lossy: its last filter is /DCTDecode or
// /JPXDecode.
bool IsLossy(const CPDF_Dictionary& dict) {
  std::optional<DecoderArray> decoders =
      GetDecoderArray(pdfium::WrapRetain(&dict));
  if (!decoders.has_value() || decoders->empty()) {
    return false;
  }
  const ByteString& last = decoders->back().first;
  return last == "DCTDecode" || last == "DCT" || last == "JPXDecode";
}

// `picture`'s pixels ready to be written: as a JPEG when the image was lossy
// already, else losslessly at zlib's fastest level.
std::optional<Content> PictureContent(const Picture& picture, bool lossy) {
  Content content;
  content.width = picture.width;
  content.height = picture.height;
  content.color_space = picture.components == 1 ? "DeviceGray" : "DeviceRGB";
  const size_t pitch = static_cast<size_t>(picture.width) * picture.components;
  if (lossy) {
    std::optional<DataVector<uint8_t>> jpeg = fxcodec::EncodeJpeg(
        picture.width, picture.height, picture.components, kJpegQuality,
        [&](int y, pdfium::span<uint8_t> out) {
          fxcrt::Copy(pdfium::span(picture.pixels)
                          .subspan(static_cast<size_t>(y) * pitch, pitch),
                      out);
        });
    if (!jpeg.has_value()) {
      return std::nullopt;
    }
    content.jpeg = std::move(jpeg.value());
    return content;
  }
  Samples samples;
  samples.width = picture.width;
  samples.height = picture.height;
  samples.components = picture.components;
  samples.bpc = 8;
  samples.pitch = pitch;
  samples.data = picture.pixels;
  content.samples = std::move(samples);
  content.fast = true;
  return content;
}

// `rgb` (0x00RRGGBB) as a /Matte of `picture`'s gray or RGB.
std::vector<float> MatteOf(const Picture& picture, uint32_t rgb) {
  const float red = (rgb >> 16 & 0xff) / 255.0f;
  const float green = (rgb >> 8 & 0xff) / 255.0f;
  const float blue = (rgb & 0xff) / 255.0f;
  // A gray picture's matte is gray (DecodePicture()).
  return picture.components == 1 ? std::vector<float>{blue}
                                 : std::vector<float>{red, green, blue};
}

// `picture`'s alpha as a soft mask.
Content AlphaContent(const Picture& picture) {
  Content content;
  content.width = picture.width;
  content.height = picture.height;
  content.color_space = "DeviceGray";
  if (picture.white_blended) {
    content.matte = MatteOf(picture, 0xffffff);
  }
  Samples samples;
  samples.width = picture.width;
  samples.height = picture.height;
  samples.components = 1;
  samples.bpc = 8;
  samples.pitch = static_cast<size_t>(picture.width);
  samples.data = picture.alpha;
  content.samples = std::move(samples);
  content.fast = true;
  return content;
}

// A soft mask whose format can't be kept (a JPEG 2000 one), decoded on its
// own grid and opaque under `page_rects`.
std::optional<Content> DecodedSoftMask(
    CPDF_Document* doc,
    const CPDF_Stream& soft_mask,
    const CFX_Matrix& image_to_page,
    pdfium::span<const CFX_FloatRect> page_rects) {
  std::optional<Decoded> decoded =
      Decode(doc, pdfium::WrapRetain(&soft_mask), nullptr, nullptr);
  if (!decoded.has_value()) {
    return std::nullopt;
  }
  std::optional<Picture> picture =
      DecodePicture(decoded.value(), /*stencil=*/false, 0);
  if (!picture.has_value() || picture->components != 1) {
    return std::nullopt;
  }
  FillPicture(
      picture.value(),
      HoleBoxes(picture->width, picture->height, image_to_page, page_rects),
      0xff);
  std::optional<Content> content = PictureContent(picture.value(), false);
  return content;
}

// Redacts `image_object` from its decoded pixels (see RedactImageObject()).
RedactResult RedactDecodedImage(CPDF_Page* page,
                                CPDF_ImageObject* image_object,
                                const CPDF_Dictionary* resources,
                                pdfium::span<const CFX_FloatRect> page_rects,
                                const CFX_Matrix& image_to_page,
                                bool fill_black) {
  CPDF_Image* image = image_object->GetImage();
  CPDF_Document* doc = page->GetDocument();
  RetainPtr<const CPDF_Stream> stream = image ? image->GetStream() : nullptr;
  if (!stream || !doc) {
    return {.succeeded = false};
  }
  RetainPtr<const CPDF_Dictionary> dict = stream->GetDict();
  RetainPtr<const CPDF_Dictionary> page_resources = page->GetResources();
  std::optional<Decoded> decoded =
      Decode(doc, stream, resources, page_resources.Get());
  if (!decoded.has_value()) {
    return {.succeeded = false};
  }
  // A stencil paints the fill colour; other images may have no colour state.
  const CPDF_ColorState& color_state = image_object->color_state();
  const FX_COLORREF stencil_fill = image->IsMask() && color_state.HasRef()
                                       ? color_state.GetFillColorRef()
                                       : 0;
  std::optional<Picture> picture =
      DecodePicture(decoded.value(), image->IsMask(), stencil_fill);
  if (!picture.has_value()) {
    return {.succeeded = false};
  }
  const std::vector<GridBox> boxes =
      HoleBoxes(picture->width, picture->height, image_to_page, page_rects);
  if (boxes.empty()) {
    return {};
  }
  FillPicture(picture.value(), boxes, fill_black ? 0x00 : 0xff);

  // Its masks keep their format where they can, else are decoded too; its
  // own transparency becomes a soft mask. Everything is read before anything
  // is written.
  RetainPtr<const CPDF_Stream> soft_mask = dict->GetStreamFor("SMask");
  RetainPtr<const CPDF_Stream> mask =
      soft_mask ? nullptr : dict->GetStreamFor("Mask");
  std::optional<Content> soft_mask_content;
  bool soft_mask_kept = false;
  if (soft_mask) {
    RetainPtr<CPDF_ColorSpace> gray = ColorSpaceOf(
        doc, *soft_mask->GetDict(), resources, page_resources.Get());
    if (gray && gray->ComponentCount() == 1) {
      soft_mask_content = RedactedContent(
          doc, soft_mask, gray.Get(), Hole::kOpaque, image_to_page, page_rects);
      soft_mask_kept = soft_mask_content.has_value();
    }
    if (!soft_mask_content.has_value()) {
      soft_mask_content =
          DecodedSoftMask(doc, *soft_mask, image_to_page, page_rects);
    }
    if (!soft_mask_content.has_value()) {
      return {.succeeded = false};
    }
  }
  std::optional<Content> mask_content;
  if (mask) {
    mask_content = RedactedContent(doc, mask, nullptr, Hole::kPainted,
                                   image_to_page, page_rects);
    if (!mask_content.has_value()) {
      return {.succeeded = false};
    }
  }
  if (soft_mask_content.has_value() && decoded->matte.has_value()) {
    soft_mask_content->matte = MatteOf(picture.value(), decoded->matte.value());
  }
  std::optional<Content> content =
      PictureContent(picture.value(), IsLossy(*dict));
  if (!content.has_value()) {
    return {.succeeded = false};
  }

  RetainPtr<CPDF_Stream> replacement =
      WriteContent(doc, *dict, kDecodedKeys, content.value());
  if (!replacement) {
    return {.succeeded = false};
  }
  RetainPtr<CPDF_Dictionary> replacement_dict = replacement->GetMutableDict();
  bool written = true;
  if (soft_mask) {
    written = WriteMask(
        doc, replacement_dict.Get(), "SMask", *soft_mask->GetDict(),
        soft_mask_kept
            ? pdfium::span<const char* const>(kKeptSoftMaskOfDecodedKeys)
            : pdfium::span<const char* const>(kDecodedSoftMaskKeys),
        soft_mask_content.value());
  } else if (mask) {
    written = WriteMask(doc, replacement_dict.Get(), "Mask", *mask->GetDict(),
                        kMaskKeys, mask_content.value());
  } else if (!picture->alpha.empty()) {
    written = WriteMask(doc, replacement_dict.Get(), "SMask", *dict, {},
                        AlphaContent(picture.value()));
  }
  if (!written) {
    return {.succeeded = false};
  }
  return ReplaceImage(page, image_object, *replacement);
}

}  // namespace

RedactResult RedactImageObject(CPDF_Page* page,
                               CPDF_ImageObject* image_object,
                               const CPDF_Dictionary* resources,
                               pdfium::span<const CFX_FloatRect> page_rects,
                               const CFX_Matrix& parent_to_page,
                               bool fill_black) {
  if (!page || !image_object) {
    return {.succeeded = false};
  }
  // The image's own matrix first, then its placement.
  const CFX_Matrix image_to_page = image_object->matrix() * parent_to_page;
  const CFX_FloatRect bounds =
      image_to_page.TransformRect(CFX_FloatRect(0, 0, 1.0f, 1.0f));
  const bool touches = std::ranges::any_of(page_rects, [&](const auto& r) {
    return bounds.right > r.left && bounds.left < r.right &&
           bounds.top > r.bottom && bounds.bottom < r.top;
  });
  if (!touches) {
    return {};
  }
  // A black fill shows only under the black boxes drawn over every region;
  // paper is as good there.
  std::optional<RedactResult> kept = RedactKeepingFormat(
      page, image_object, resources, page_rects, image_to_page);
  if (kept.has_value()) {
    return kept.value();
  }
  return RedactDecodedImage(page, image_object, resources, page_rects,
                            image_to_page, fill_black);
}
