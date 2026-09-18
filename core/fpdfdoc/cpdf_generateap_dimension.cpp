// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
#include "core/fpdfdoc/cpdf_generateap_dimension.h"

#include <algorithm>
#include <cmath>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_number.h"

namespace pdfium::dimension {
namespace {
constexpr float kCaptionPadding = 2;
constexpr float kOutsideCaptionPadding = 7;
constexpr float kArrowReserve = 24;

float Length(CFX_PointF p) {
  return std::hypot(p.x, p.y);
}

float Dot(CFX_PointF a, CFX_PointF b) {
  return a.x * b.x + a.y * b.y;
}

void Include(CFX_FloatRect& box, CFX_PointF p) {
  box.left = std::min(box.left, p.x);
  box.right = std::max(box.right, p.x);
  box.bottom = std::min(box.bottom, p.y);
  box.top = std::max(box.top, p.y);
}

bool Inside(CFX_PointF p, pdfium::span<const CFX_PointF> vertices) {
  bool inside = false;
  for (size_t i = 0, j = vertices.size() - 1; i < vertices.size(); j = i++) {
    const auto a = vertices[i];
    const auto b = vertices[j];
    if ((a.y > p.y) != (b.y > p.y) &&
        p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
      inside = !inside;
    }
  }
  return inside;
}

std::vector<Segment> CaptionConnector(CFX_PointF midpoint,
                                      CFX_PointF center,
                                      const LineLayout& line,
                                      float width,
                                      float height) {
  const float perpendicular = Dot(center - midpoint, line.normal);
  const float along = Dot(center - midpoint, line.along);
  const float half_width = width / 2 + kCaptionPadding;
  const float half_height = height / 2 + kCaptionPadding;

  if (std::fabs(perpendicular) <= half_height) {
    return {};
  }

  if (std::fabs(along) <= half_width) {
    const float distance =
        perpendicular - std::copysign(half_height, perpendicular);
    return {{midpoint, midpoint + distance * line.normal}};
  }

  const CFX_PointF knee = midpoint + perpendicular * line.normal;
  const CFX_PointF end = center - std::copysign(half_width, along) * line.along;
  return {{midpoint, knee}, {knee, end}};
}
}  // namespace

LineLayout LayoutLine(CFX_PointF start,
                      CFX_PointF end,
                      float length,
                      float extension,
                      float offset,
                      float stroke_width) {
  LineLayout result;
  result.length = Length(end - start);
  result.along = result.length > 0 ? (1.0f / result.length) * (end - start)
                                   : CFX_PointF(1, 0);
  result.normal = {-result.along.y, result.along.x};
  // Acrobat fixtures: positive LL displaces the dimension line to the LEFT
  // of the directed measured segment in y-up space, p + normal * LL.
  result.start = start + length * result.normal;
  result.end = end + length * result.normal;
  result.bounds = {result.start.x, result.start.y, result.start.x,
                   result.start.y};
  Include(result.bounds, result.end);
  if (length != 0) {
    const float side = length > 0 ? 1.0f : -1.0f;
    for (CFX_PointF p : {start, end}) {
      Segment leader{p + (side * offset) * result.normal,
                     p + (length + side * extension) * result.normal};
      result.leaders.push_back(leader);
      Include(result.bounds, leader.from);
      Include(result.bounds, leader.to);
    }
  }
  // Conservative envelope for all supported endings, plus stroke.
  result.bounds.Inflate(std::max(1.0f, 8 * stroke_width),
                        std::max(1.0f, 8 * stroke_width));
  return result;
}

CaptionLayout LayoutLineCaption(const CPDF_Dictionary* annot,
                                const LineLayout& line,
                                float width,
                                float height,
                                float stroke_width) {
  CaptionLayout result;
  CFX_PointF u = line.along;
  if (u.x < 0 || (u.x == 0 && u.y < 0)) {
    u = -1.0f * u;
  }
  const CFX_PointF midpoint = 0.5f * line.start + 0.5f * line.end;
  CFX_PointF center = midpoint;
  bool displaced = false;
  auto offset = annot->GetArrayFor("CO");
  if (offset && offset->size() == 2) {
    const float along = offset->GetFloatAt(0);
    const float perpendicular = offset->GetFloatAt(1);
    if (std::isfinite(along) && std::isfinite(perpendicular)) {
      center += along * line.along + perpendicular * line.normal;
      displaced = along != 0 || perpendicular != 0;
    }
  }
  const bool top = annot->GetNameFor("CP") == "Top";
  // Explicit fit policy, based on text width + breathing room + arrow length.
  // Constrained by the Acrobat 1.75 m / 2.01 m cases, not an undocumented
  // Adobe threshold. Keep the live layout in core/annotation in agreement.
  result.outside_arrows =
      width + 2 * kCaptionPadding + kArrowReserve * stroke_width > line.length;
  if (top || result.outside_arrows) {
    const float side =
        result.outside_arrows && annot->GetFloatFor("LL") < 0 ? -1.0f : 1.0f;
    const float padding =
        result.outside_arrows ? kOutsideCaptionPadding : kCaptionPadding;
    // Keep the anchor attached to the directed line as it turns. Flipping
    // the glyph orientation for readability must not move the caption.
    center += (side * (height / 2 + padding)) * line.normal;
  }
  result.matrix = {u.x, u.y, -u.y, u.x, center.x, center.y};
  result.bounds = result.matrix.TransformRect(
      CFX_FloatRect(-width / 2, -height / 2, width / 2, height / 2));
  if (displaced) {
    result.connector = CaptionConnector(midpoint, center, line, width, height);
    for (const auto& segment : result.connector) {
      Include(result.bounds, segment.from);
      Include(result.bounds, segment.to);
    }
  }
  // Cut only where the text actually intersects the line, including /CO.
  const float perpendicular = Dot(center - line.start, line.normal);
  if (!top && !result.outside_arrows &&
      std::fabs(perpendicular) < height / 2 + kCaptionPadding) {
    const float along = Dot(center - line.start, line.along);
    result.gap_start =
        std::clamp(along - width / 2 - kCaptionPadding, 0.0f, line.length);
    result.gap_end =
        std::clamp(along + width / 2 + kCaptionPadding, 0.0f, line.length);
  }
  return result;
}

static CFX_PointF AutomaticShapeCaptionCenter(
    pdfium::span<const CFX_PointF> vertices,
    bool closed,
    float height) {
  if (vertices.empty()) {
    return {};
  }
  if (!closed) {
    double length = 0;
    for (size_t i = 1; i < vertices.size(); ++i) {
      length += Length(vertices[i] - vertices[i - 1]);
    }
    double remaining = length / 2;
    for (size_t i = 1; i < vertices.size(); ++i) {
      float segment = Length(vertices[i] - vertices[i - 1]);
      if (segment > 0 && remaining <= segment) {
        return vertices[i - 1] +
               static_cast<float>(remaining / segment) *
                   (vertices[i] - vertices[i - 1]) +
               CFX_PointF(0, height / 2 + 2);
      }
      remaining -= segment;
    }
    return vertices.front();
  }
  // Compute relative to the first vertex to avoid cancellation at large page
  // origins. A concave polygon's centroid may be outside: use an interior
  // scanline interval in that case (not the center of /Rect).
  const CFX_PointF origin = vertices.front();
  double area = 0;
  double x = 0;
  double y = 0;
  CFX_FloatRect box(origin.x, origin.y, origin.x, origin.y);
  for (size_t i = 0; i < vertices.size(); ++i) {
    CFX_PointF a = vertices[i] - origin;
    CFX_PointF b = vertices[(i + 1) % vertices.size()] - origin;
    double cross =
        static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    area += cross;
    x += (static_cast<double>(a.x) + b.x) * cross;
    y += (static_cast<double>(a.y) + b.y) * cross;
    Include(box, vertices[i]);
  }
  if (std::fabs(area) > 1e-8) {
    CFX_PointF centroid =
        origin + CFX_PointF(static_cast<float>(x / (3 * area)),
                            static_cast<float>(y / (3 * area)));
    if (Inside(centroid, vertices)) {
      return centroid;
    }
  }
  CFX_PointF fallback((box.left + box.right) / 2, (box.bottom + box.top) / 2);
  std::vector<float> intersections;
  for (size_t i = 0; i < vertices.size(); ++i) {
    const auto a = vertices[i];
    const auto b = vertices[(i + 1) % vertices.size()];
    if ((a.y > fallback.y) != (b.y > fallback.y)) {
      intersections.push_back(a.x +
                              (fallback.y - a.y) * (b.x - a.x) / (b.y - a.y));
    }
  }
  std::sort(intersections.begin(), intersections.end());
  float widest = -1;
  for (size_t i = 1; i < intersections.size(); i += 2) {
    if (intersections[i] - intersections[i - 1] > widest) {
      widest = intersections[i] - intersections[i - 1];
      fallback.x = (intersections[i] + intersections[i - 1]) / 2;
    }
  }
  return fallback;
}

CFX_PointF ShapeCaptionCenter(const CPDF_Dictionary* annot,
                              pdfium::span<const CFX_PointF> vertices,
                              bool closed,
                              float height) {
  auto metadata = annot->GetDictFor("EMBD_Metadata");
  auto center =
      metadata ? metadata->GetArrayFor("MeasurementCaptionCenter") : nullptr;
  if (center && center->size() == 2 && center->GetNumberAt(0) &&
      center->GetNumberAt(1) && std::isfinite(center->GetFloatAt(0)) &&
      std::isfinite(center->GetFloatAt(1))) {
    return {center->GetFloatAt(0), center->GetFloatAt(1)};
  }
  const CFX_Matrix rotation = ShapeCaptionRotation(annot);
  const CFX_Matrix inverse = rotation.GetInverse();
  std::vector<CFX_PointF> local;
  for (const auto& point : vertices) {
    local.push_back(inverse.Transform(point));
  }
  return rotation.Transform(AutomaticShapeCaptionCenter(local, closed, height));
}

CFX_Matrix ShapeCaptionRotation(const CPDF_Dictionary* annot) {
  auto metadata = annot->GetDictFor("EMBD_Metadata");
  const float degrees = metadata ? metadata->GetFloatFor("Rotation") : 0;
  if (!std::isfinite(degrees)) {
    return {};
  }
  const float radians = degrees * 3.14159265358979323846f / 180;
  const float cosine = std::cos(radians);
  const float sine = std::sin(radians);
  return {cosine, sine, -sine, cosine, 0, 0};
}

CaptionLayout LayoutShapeCaption(const CPDF_Dictionary* annot,
                                 CFX_PointF center,
                                 float width,
                                 float height) {
  CaptionLayout result;
  result.matrix = ShapeCaptionRotation(annot);
  result.matrix.e = center.x;
  result.matrix.f = center.y;
  result.bounds = result.matrix.TransformRect(
      CFX_FloatRect(-width / 2, -height / 2, width / 2, height / 2));
  return result;
}

}  // namespace pdfium::dimension
