// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// Cloudy (scalloped) border path generator for PDF annotations.
// Derived from Apache PDFBox's CloudyBorder.java:
// https://github.com/apache/pdfbox/blob/trunk/pdfbox/src/main/java/org/apache/pdfbox/pdmodel/interactive/annotation/handlers/CloudyBorder.java
// Original code licensed under the Apache License, Version 2.0:
// https://www.apache.org/licenses/LICENSE-2.0
//
// Substantially modified: ported from Java to C++, adapted for PDF content
// stream output, and integrated with PDFium.

#include "core/fpdfdoc/cpdf_cloudy_border.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

namespace {

// ── Constants ────────────────────────────────────────────────────────────────

constexpr float kPi = 3.14159265358979323846f;
constexpr float kAngle180 = kPi;
constexpr float kAngle90 = kPi / 2.0f;
constexpr float kAngle34 = 34.0f * kPi / 180.0f;
constexpr float kAngle30 = 30.0f * kPi / 180.0f;
constexpr float kAngle22 = 22.0f * kPi / 180.0f;
constexpr float kAngle12 = 12.0f * kPi / 180.0f;

// ── Geometry helpers ─────────────────────────────────────────────────────────

float CloudyDistance(CFX_PointF a, CFX_PointF b) {
  return std::hypot(b.x - a.x, b.y - a.y);
}

float CloudyCosine(float dx, float hypot) {
  return hypot == 0.0f ? 0.0f : dx / hypot;
}

float CloudySine(float dy, float hypot) {
  return hypot == 0.0f ? 0.0f : dy / hypot;
}

float CloudyPolygonDirection(const std::vector<CFX_PointF>& pts) {
  float a = 0.0f;
  size_t len = pts.size();
  for (size_t i = 0; i < len; i++) {
    size_t j = (i + 1) % len;
    a += pts[i].x * pts[j].y - pts[i].y * pts[j].x;
  }
  return a;
}

void EnsurePositiveWinding(std::vector<CFX_PointF>& pts) {
  if (CloudyPolygonDirection(pts) < 0.0f)
    std::reverse(pts.begin(), pts.end());
}

std::vector<CFX_PointF> RemoveZeroLengthSegments(
    const std::vector<CFX_PointF>& polygon) {
  if (polygon.size() <= 2)
    return polygon;

  constexpr float kTolerance = 0.5f;
  std::vector<CFX_PointF> result;
  result.push_back(polygon[0]);
  for (size_t i = 1; i < polygon.size(); i++) {
    const CFX_PointF& prev = result.back();
    const CFX_PointF& cur = polygon[i];
    if (std::abs(cur.x - prev.x) >= kTolerance ||
        std::abs(cur.y - prev.y) >= kTolerance) {
      result.push_back(cur);
    }
  }
  return result;
}

// ── Arc generation (Bézier approximation of circular arcs) ───────────────────

void CloudyArcSegment(float start_ang,
                      float end_ang,
                      float cx,
                      float cy,
                      float rx,
                      float ry,
                      fxcrt::ostringstream& out,
                      bool add_move_to) {
  float cos_a = std::cos(start_ang);
  float sin_a = std::sin(start_ang);
  float cos_b = std::cos(end_ang);
  float sin_b = std::sin(end_ang);
  float half_ang = (end_ang - start_ang) / 2.0f;
  float denom = std::sin(half_ang);

  if (denom == 0.0f) {
    if (add_move_to)
      out << cx + rx * cos_a << " " << cy + ry * sin_a << " m\n";
    return;
  }

  float bcp =
      (4.0f / 3.0f) * (1.0f - std::cos(half_ang)) / denom;
  float p1x = cx + rx * (cos_a - bcp * sin_a);
  float p1y = cy + ry * (sin_a + bcp * cos_a);
  float p2x = cx + rx * (cos_b + bcp * sin_b);
  float p2y = cy + ry * (sin_b - bcp * cos_b);
  float p3x = cx + rx * cos_b;
  float p3y = cy + ry * sin_b;

  if (add_move_to)
    out << cx + rx * cos_a << " " << cy + ry * sin_a << " m\n";

  out << p1x << " " << p1y << " " << p2x << " " << p2y << " " << p3x << " "
      << p3y << " c\n";
}

void CloudyArcSegmentToPoints(float start_ang,
                              float end_ang,
                              float cx,
                              float cy,
                              float rx,
                              float ry,
                              std::vector<CFX_PointF>& pts) {
  float cos_a = std::cos(start_ang);
  float sin_a = std::sin(start_ang);
  float cos_b = std::cos(end_ang);
  float sin_b = std::sin(end_ang);
  float half_ang = (end_ang - start_ang) / 2.0f;
  float denom = std::sin(half_ang);

  if (denom == 0.0f)
    return;

  float bcp =
      (4.0f / 3.0f) * (1.0f - std::cos(half_ang)) / denom;
  pts.push_back(
      {cx + rx * (cos_a - bcp * sin_a), cy + ry * (sin_a + bcp * cos_a)});
  pts.push_back(
      {cx + rx * (cos_b + bcp * sin_b), cy + ry * (sin_b - bcp * cos_b)});
  pts.push_back({cx + rx * cos_b, cy + ry * sin_b});
}

void CloudyGetArc(float start_ang,
                  float end_ang,
                  float rx,
                  float ry,
                  float cx,
                  float cy,
                  fxcrt::ostringstream& out,
                  bool add_move_to) {
  float angle_todo = end_ang - start_ang;
  while (angle_todo < 0.0f)
    angle_todo += 2.0f * kPi;
  float sweep = angle_todo;
  float angle_done = 0.0f;

  if (add_move_to) {
    out << cx + rx * std::cos(start_ang) << " "
        << cy + ry * std::sin(start_ang) << " m\n";
  }

  while (angle_todo > kAngle90) {
    CloudyArcSegment(start_ang + angle_done,
                     start_ang + angle_done + kAngle90, cx, cy, rx, ry, out,
                     false);
    angle_done += kAngle90;
    angle_todo -= kAngle90;
  }

  if (angle_todo > 0.0f) {
    CloudyArcSegment(start_ang + angle_done, start_ang + sweep, cx, cy, rx, ry,
                     out, false);
  }
}

// ── Curl generation ──────────────────────────────────────────────────────────

void CloudyAddCornerCurl(float angle_prev,
                         float angle_cur,
                         float radius,
                         float cx,
                         float cy,
                         float alpha,
                         float alpha_prev,
                         fxcrt::ostringstream& out,
                         bool add_move_to) {
  float a = angle_prev + kAngle180 + alpha_prev;
  float b = a - kAngle22;
  CloudyArcSegment(a, b, cx, cy, radius, radius, out, add_move_to);

  float b_end = angle_cur - alpha;
  CloudyGetArc(b, b_end, radius, radius, cx, cy, out, false);
}

void CloudyAddFirstIntermediateCurl(float angle_cur,
                                    float r,
                                    float alpha,
                                    float cx,
                                    float cy,
                                    fxcrt::ostringstream& out) {
  float a = angle_cur + kAngle180;
  CloudyArcSegment(a + alpha, a + alpha - kAngle30, cx, cy, r, r, out, false);
  CloudyArcSegment(a + alpha - kAngle30, a + kAngle90, cx, cy, r, r, out,
                   false);
  CloudyArcSegment(a + kAngle90, a + kAngle180 - kAngle34, cx, cy, r, r, out,
                   false);
}

std::vector<CFX_PointF> CloudyGetIntermediateCurlTemplate(float angle_cur,
                                                          float r) {
  std::vector<CFX_PointF> pts;
  float a = angle_cur + kAngle180;
  CloudyArcSegmentToPoints(a + kAngle34, a + kAngle12, 0.0f, 0.0f, r, r, pts);
  CloudyArcSegmentToPoints(a + kAngle12, a + kAngle90, 0.0f, 0.0f, r, r, pts);
  CloudyArcSegmentToPoints(a + kAngle90, a + kAngle180 - kAngle34, 0.0f, 0.0f,
                           r, r, pts);
  return pts;
}

void CloudyOutputCurlTemplate(const std::vector<CFX_PointF>& tmpl,
                              float x,
                              float y,
                              fxcrt::ostringstream& out) {
  for (size_t i = 0; i + 2 < tmpl.size(); i += 3) {
    out << tmpl[i].x + x << " " << tmpl[i].y + y << " "
        << tmpl[i + 1].x + x << " " << tmpl[i + 1].y + y << " "
        << tmpl[i + 2].x + x << " " << tmpl[i + 2].y + y << " c\n";
  }
}

// ── Cloud radius formulas (deduced from Acrobat by PDFBox) ───────────────────

float GetEllipseCloudRadius(float intensity, float line_width) {
  return 4.75f * intensity + 0.5f * line_width;
}

float GetPolygonCloudRadius(float intensity, float line_width) {
  return 4.0f * intensity + 0.5f * line_width;
}

// ── Core polygon algorithm ───────────────────────────────────────────────────

struct CloudyPolygonParams {
  int n;
  float adjusted_radius;
};

CloudyPolygonParams CloudyComputeParamsPolygon(float ideal_radius,
                                               float k,
                                               float length) {
  if (length == 0.0f)
    return {-1, ideal_radius};

  float corner_space = 2.0f * k * ideal_radius;
  float remaining = length - corner_space;

  if (remaining <= 0.0f)
    return {0, ideal_radius};

  float ideal_advance = 2.0f * k * ideal_radius;
  int n = std::max(1, static_cast<int>(std::ceil(remaining / ideal_advance)));
  float adjusted_radius = remaining / (n * 2.0f * k);

  return {n, adjusted_radius};
}

void CloudyPolygonImpl(std::vector<CFX_PointF> vertices,
                       bool is_ellipse,
                       float intensity,
                       float line_width,
                       fxcrt::ostringstream& out) {
  std::vector<CFX_PointF> polygon = RemoveZeroLengthSegments(vertices);
  EnsurePositiveWinding(polygon);
  size_t num_points = polygon.size();

  if (num_points < 2)
    return;

  if (intensity <= 0.0f) {
    out << polygon[0].x << " " << polygon[0].y << " m\n";
    for (size_t i = 1; i < num_points; i++) {
      out << polygon[i].x << " " << polygon[i].y << " " << polygon[i].x << " "
          << polygon[i].y << " " << polygon[i].x << " " << polygon[i].y
          << " c\n";
    }
    return;
  }

  float ideal_radius = is_ellipse
                            ? GetEllipseCloudRadius(intensity, line_width)
                            : GetPolygonCloudRadius(intensity, line_width);
  if (ideal_radius < 0.5f)
    ideal_radius = 0.5f;

  float k = std::cos(kAngle34);

  // Pre-compute per-edge alpha for short-edge corner-arc merging.
  std::vector<float> edge_alphas;
  for (size_t j = 0; j + 1 < num_points; j++) {
    float len = CloudyDistance(polygon[j], polygon[j + 1]);
    if (len <= 0.0f || len >= 2.0f * k * ideal_radius) {
      edge_alphas.push_back(kAngle34);
    } else {
      edge_alphas.push_back(
          std::acos(std::min(1.0f, len / (2.0f * ideal_radius))));
    }
  }

  float angle_prev = 0.0f;
  bool output_started = false;

  for (size_t j = 0; j + 1 < num_points; j++) {
    CFX_PointF pt = polygon[j];
    CFX_PointF pt_next = polygon[j + 1];
    float len = CloudyDistance(pt, pt_next);
    if (len == 0.0f)
      continue;

    CloudyPolygonParams params =
        CloudyComputeParamsPolygon(ideal_radius, k, len);
    if (params.n < 0) {
      if (!output_started) {
        out << pt.x << " " << pt.y << " m\n";
        output_started = true;
      }
      continue;
    }

    float edge_radius = std::max(0.5f, params.adjusted_radius);
    float interm_advance = 2.0f * k * edge_radius;
    float first_advance = k * ideal_radius + k * edge_radius;

    float angle_cur = std::atan2(pt_next.y - pt.y, pt_next.x - pt.x);
    if (j == 0) {
      CFX_PointF pt_prev = polygon[num_points - 2];
      angle_prev = std::atan2(pt.y - pt_prev.y, pt.x - pt_prev.x);
    }

    float cos_e = CloudyCosine(pt_next.x - pt.x, len);
    float sin_e = CloudySine(pt_next.y - pt.y, len);
    float x = pt.x;
    float y = pt.y;

    float alpha = edge_alphas[j];
    size_t prev_edge_idx = (j == 0) ? num_points - 2 : j - 1;
    float alpha_prev_edge = (prev_edge_idx < edge_alphas.size())
                                ? edge_alphas[prev_edge_idx]
                                : kAngle34;

    CloudyAddCornerCurl(angle_prev, angle_cur, ideal_radius, pt.x, pt.y, alpha,
                        alpha_prev_edge, out, !output_started);
    output_started = true;

    if (params.n == 0) {
      x += len * cos_e;
      y += len * sin_e;
    } else {
      x += first_advance * cos_e;
      y += first_advance * sin_e;

      int num_interm = params.n;
      if (params.n >= 1) {
        CloudyAddFirstIntermediateCurl(angle_cur, edge_radius, kAngle34, x, y,
                                      out);
        x += interm_advance * cos_e;
        y += interm_advance * sin_e;
        num_interm = params.n - 1;
      }

      std::vector<CFX_PointF> tmpl =
          CloudyGetIntermediateCurlTemplate(angle_cur, edge_radius);
      for (int i = 0; i < num_interm; i++) {
        CloudyOutputCurlTemplate(tmpl, x, y, out);
        x += interm_advance * cos_e;
        y += interm_advance * sin_e;
      }
    }

    angle_prev = angle_cur;
  }
}

// ── Ellipse helpers ──────────────────────────────────────────────────────────

std::vector<CFX_PointF> CloudyFlattenEllipse(float left,
                                             float bottom,
                                             float right,
                                             float top) {
  float cx = (left + right) / 2.0f;
  float cy = (bottom + top) / 2.0f;
  float rx = (right - left) / 2.0f;
  float ry = (top - bottom) / 2.0f;

  std::vector<CFX_PointF> points;
  if (rx <= 0.0f || ry <= 0.0f)
    return points;

  int num_segments =
      std::max(32, static_cast<int>(std::ceil(std::max(rx, ry) * 2.0f)));
  points.reserve(num_segments + 1);

  for (int i = 0; i <= num_segments; i++) {
    float angle = (2.0f * kPi * i) / num_segments;
    points.push_back({cx + rx * std::cos(angle), cy + ry * std::sin(angle)});
  }
  return points;
}

float CloudyComputeParamsEllipse(CFX_PointF pt,
                                 CFX_PointF pt_next,
                                 float r,
                                 float curl_adv) {
  float len = CloudyDistance(pt, pt_next);
  if (len == 0.0f)
    return kAngle34;
  float e = len - curl_adv;
  float arg = (curl_adv / 2.0f + e / 2.0f) / r;
  return (arg < -1.0f || arg > 1.0f) ? 0.0f : std::acos(arg);
}

// ── Cloudy ellipse core ──────────────────────────────────────────────────────

void CloudyEllipseImpl(float left,
                       float bottom,
                       float right,
                       float top,
                       float intensity,
                       float line_width,
                       fxcrt::ostringstream& out) {
  if (intensity <= 0.0f) {
    float rx = std::abs(right - left) / 2.0f;
    float ry = std::abs(top - bottom) / 2.0f;
    float cx = (left + right) / 2.0f;
    float cy = (bottom + top) / 2.0f;
    CloudyGetArc(0.0f, 2.0f * kPi, rx, ry, cx, cy, out, true);
    return;
  }

  float width = right - left;
  float height = top - bottom;
  float cloud_radius = GetEllipseCloudRadius(intensity, line_width);

  float threshold1 = 0.5f * cloud_radius;
  if (width < threshold1 && height < threshold1) {
    float rx = std::abs(right - left) / 2.0f;
    float ry = std::abs(top - bottom) / 2.0f;
    float cx = (left + right) / 2.0f;
    float cy = (bottom + top) / 2.0f;
    CloudyGetArc(0.0f, 2.0f * kPi, rx, ry, cx, cy, out, true);
    return;
  }

  constexpr float kThreshold2 = 5.0f;
  if ((width < kThreshold2 && height > 20.0f) ||
      (width > 20.0f && height < kThreshold2)) {
    std::vector<CFX_PointF> rect_polygon = {
        {left, bottom}, {right, bottom}, {right, top}, {left, top},
        {left, bottom}};
    CloudyPolygonImpl(rect_polygon, true, intensity, line_width, out);
    return;
  }

  float radius_adj = std::sin(kAngle12) * cloud_radius - 1.5f;
  float adj_left = left;
  float adj_right = right;
  float adj_bottom = bottom;
  float adj_top = top;

  if (width > 2.0f * radius_adj) {
    adj_left += radius_adj;
    adj_right -= radius_adj;
  } else {
    float mid = (left + right) / 2.0f;
    adj_left = mid - 0.1f;
    adj_right = mid + 0.1f;
  }

  if (height > 2.0f * radius_adj) {
    adj_bottom += radius_adj;
    adj_top -= radius_adj;
  } else {
    float mid = (top + bottom) / 2.0f;
    adj_top = mid + 0.1f;
    adj_bottom = mid - 0.1f;
  }

  std::vector<CFX_PointF> flat_polygon =
      CloudyFlattenEllipse(adj_left, adj_bottom, adj_right, adj_top);
  size_t num_flat_pts = flat_polygon.size();
  if (num_flat_pts < 2)
    return;

  float tot_len = 0.0f;
  for (size_t i = 1; i < num_flat_pts; i++)
    tot_len += CloudyDistance(flat_polygon[i - 1], flat_polygon[i]);

  float k = std::cos(kAngle34);
  float curl_advance = 2.0f * k * cloud_radius;
  int n = static_cast<int>(std::ceil(tot_len / curl_advance));
  if (n < 2) {
    float rx = std::abs(right - left) / 2.0f;
    float ry = std::abs(top - bottom) / 2.0f;
    float cx = (left + right) / 2.0f;
    float cy = (bottom + top) / 2.0f;
    CloudyGetArc(0.0f, 2.0f * kPi, rx, ry, cx, cy, out, true);
    return;
  }

  curl_advance = tot_len / n;
  cloud_radius = curl_advance / (2.0f * k);

  if (cloud_radius < 0.5f) {
    cloud_radius = 0.5f;
    curl_advance = 2.0f * k * cloud_radius;
  } else if (cloud_radius < 3.0f) {
    float rx = std::abs(right - left) / 2.0f;
    float ry = std::abs(top - bottom) / 2.0f;
    float cx = (left + right) / 2.0f;
    float cy = (bottom + top) / 2.0f;
    CloudyGetArc(0.0f, 2.0f * kPi, rx, ry, cx, cy, out, true);
    return;
  }

  // Distribute curl center points along the flattened perimeter.
  std::vector<CFX_PointF> center_points;
  float length_remain = 0.0f;
  float comparison_toler = line_width * 0.1f;

  for (size_t i = 0; i + 1 < num_flat_pts; i++) {
    CFX_PointF p1 = flat_polygon[i];
    CFX_PointF p2 = flat_polygon[i + 1];
    float seg_dx = p2.x - p1.x;
    float seg_dy = p2.y - p1.y;
    float seg_len = CloudyDistance(p1, p2);
    if (seg_len == 0.0f)
      continue;

    float length_todo = seg_len + length_remain;
    if (length_todo >= curl_advance - comparison_toler ||
        i == num_flat_pts - 2) {
      float cos_s = CloudyCosine(seg_dx, seg_len);
      float sin_s = CloudySine(seg_dy, seg_len);
      float d = curl_advance - length_remain;
      while (length_todo >= curl_advance - comparison_toler) {
        center_points.push_back({p1.x + d * cos_s, p1.y + d * sin_s});
        length_todo -= curl_advance;
        d += curl_advance;
      }
      length_remain = std::max(0.0f, length_todo);
    } else {
      length_remain += seg_len;
    }
  }

  // Place curls at each center point.
  size_t cp_len = center_points.size();
  if (cp_len == 0)
    return;

  float ep_angle_prev = 0.0f;
  float ep_alpha_prev = 0.0f;

  for (size_t i = 0; i < cp_len; i++) {
    size_t idx_next = (i + 1) % cp_len;
    CFX_PointF pt = center_points[i];
    CFX_PointF pt_next = center_points[idx_next];

    if (i == 0) {
      CFX_PointF pt_prev = center_points[cp_len - 1];
      ep_angle_prev = std::atan2(pt.y - pt_prev.y, pt.x - pt_prev.x);
      ep_alpha_prev =
          CloudyComputeParamsEllipse(pt_prev, pt, cloud_radius, curl_advance);
    }

    float angle_cur = std::atan2(pt_next.y - pt.y, pt_next.x - pt.x);
    float alpha =
        CloudyComputeParamsEllipse(pt, pt_next, cloud_radius, curl_advance);

    CloudyAddCornerCurl(ep_angle_prev, angle_cur, cloud_radius, pt.x, pt.y,
                        alpha, ep_alpha_prev, out, i == 0);

    ep_angle_prev = angle_cur;
    ep_alpha_prev = alpha;
  }
}

}  // namespace

// ── Public entry-points ──────────────────────────────────────────────────────

void GenerateCloudyRectanglePath(fxcrt::ostringstream& out,
                                 const CFX_FloatRect& box,
                                 float intensity,
                                 float line_width) {
  std::vector<CFX_PointF> polygon = {{box.left, box.bottom},
                                     {box.right, box.bottom},
                                     {box.right, box.top},
                                     {box.left, box.top},
                                     {box.left, box.bottom}};

  CloudyPolygonImpl(polygon, false, intensity, line_width, out);
  out << "h\n";
}

void GenerateCloudyEllipsePath(fxcrt::ostringstream& out,
                               const CFX_FloatRect& box,
                               float intensity,
                               float line_width) {
  CloudyEllipseImpl(box.left, box.bottom, box.right, box.top, intensity,
                    line_width, out);
  out << "h\n";
}

void GenerateCloudyPolygonPath(fxcrt::ostringstream& out,
                               const std::vector<CFX_PointF>& vertices,
                               float intensity,
                               float line_width) {
  if (vertices.size() < 3)
    return;

  std::vector<CFX_PointF> pts = vertices;
  if (pts.front().x != pts.back().x || pts.front().y != pts.back().y)
    pts.push_back(pts.front());

  CloudyPolygonImpl(pts, false, intensity, line_width, out);
  out << "h\n";
}
