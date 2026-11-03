// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfdoc/cpdf_ink_curves.h"

#include <cmath>
#include <vector>

namespace {

using Point = InkCurvePoint;

Point operator+(const Point& a, const Point& b) {
  return {a.x + b.x, a.y + b.y};
}

Point operator-(const Point& a, const Point& b) {
  return {a.x - b.x, a.y - b.y};
}

Point operator*(const Point& a, double s) {
  return {a.x * s, a.y * s};
}

double Dot(const Point& a, const Point& b) {
  return a.x * b.x + a.y * b.y;
}

double Distance(const Point& a, const Point& b) {
  return std::hypot(a.x - b.x, a.y - b.y);
}

// `v` at length 1; no direction (0, 0) when it has no length.
Point Unit(const Point& v) {
  const double length = std::hypot(v.x, v.y);
  if (length == 0) {
    return {0, 0};
  }
  return {v.x / length, v.y / length};
}

// The Bernstein weights of a cubic at `u`.
double B0(double u) {
  return (1 - u) * (1 - u) * (1 - u);
}
double B1(double u) {
  return 3 * u * (1 - u) * (1 - u);
}
double B2(double u) {
  return 3 * u * u * (1 - u);
}
double B3(double u) {
  return u * u * u;
}

struct Cubic {
  Point p0;
  Point p1;
  Point p2;
  Point p3;
};

Point CubicAt(const Cubic& c, double u) {
  return (c.p0 * B0(u) + c.p1 * B1(u)) + (c.p2 * B2(u) + c.p3 * B3(u));
}

// A piece of the stroke to fit: its first and last point, and the directions
// the curve leaves the first along and arrives at the last against (pointing
// back into the curve), both unit or none.
struct Piece {
  size_t first;
  size_t last;
  Point leaving;
  Point arriving;
};

// Where each point of the piece sits along it, 0 to 1, by the length of the
// lines between them.
std::vector<double> ChordLengthPlaces(const std::vector<Point>& points,
                                      const Piece& piece) {
  std::vector<double> places = {0};
  for (size_t i = piece.first + 1; i <= piece.last; ++i) {
    places.push_back(places.back() + Distance(points[i], points[i - 1]));
  }
  const double length = places.back();
  for (double& place : places) {
    place /= length;
  }
  return places;
}

// The cubic from the piece's first point to its last, along its two
// directions, whose handle lengths put it closest to the points at their
// places (least squares). Handles that come out shorter than a millionth of
// the piece's span, or backwards, are a third of the span instead.
Cubic LeastSquaresCubic(const std::vector<Point>& points,
                        const Piece& piece,
                        const std::vector<double>& places) {
  const Point& start = points[piece.first];
  const Point& end = points[piece.last];
  double c00 = 0;
  double c01 = 0;
  double c11 = 0;
  double x0 = 0;
  double x1 = 0;
  for (size_t i = 0; i < places.size(); ++i) {
    const double u = places[i];
    const Point a0 = piece.leaving * B1(u);
    const Point a1 = piece.arriving * B2(u);
    c00 += Dot(a0, a0);
    c01 += Dot(a0, a1);
    c11 += Dot(a1, a1);
    const Point rest =
        points[piece.first + i] -
        ((start * B0(u) + start * B1(u)) + (end * B2(u) + end * B3(u)));
    x0 += Dot(a0, rest);
    x1 += Dot(a1, rest);
  }
  const double det = c00 * c11 - c01 * c01;
  double leaving_length = det == 0 ? 0 : (x0 * c11 - x1 * c01) / det;
  double arriving_length = det == 0 ? 0 : (c00 * x1 - c01 * x0) / det;
  const double span = Distance(end, start);
  const double epsilon = 1e-6 * span;
  if (leaving_length < epsilon || arriving_length < epsilon) {
    leaving_length = span / 3;
    arriving_length = span / 3;
  }
  return {start, start + piece.leaving * leaving_length,
          end + piece.arriving * arriving_length, end};
}

// The piece's point farthest from the curve at its place: the square of that
// distance, and which point (the last of equals).
struct WorstPoint {
  double squared_distance;
  size_t index;
};

WorstPoint FindWorstPoint(const std::vector<Point>& points,
                          const Piece& piece,
                          const std::vector<double>& places,
                          const Cubic& cubic) {
  WorstPoint worst = {0, (piece.last - piece.first + 1) / 2 + piece.first};
  for (size_t i = piece.first + 1; i < piece.last; ++i) {
    const Point off = CubicAt(cubic, places[i - piece.first]) - points[i];
    const double squared = Dot(off, off);
    if (squared >= worst.squared_distance) {
      worst = {squared, i};
    }
  }
  return worst;
}

double DistanceToSegment(const Point& p, const Point& a, const Point& b) {
  const Point ab = b - a;
  const double length_squared = Dot(ab, ab);
  double t = length_squared == 0 ? 0 : Dot(p - a, ab) / length_squared;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  return Distance(p, a + ab * t);
}

// How far the curve strays from the lines between the piece's points: the
// farthest of 65 places along it, each from its nearest line.
double DistanceFromLines(const std::vector<Point>& points,
                         const Piece& piece,
                         const Cubic& cubic) {
  constexpr int kSteps = 64;
  double farthest = 0;
  for (int k = 0; k <= kSteps; ++k) {
    const Point at = CubicAt(cubic, static_cast<double>(k) / kSteps);
    double nearest = INFINITY;
    for (size_t i = piece.first; i < piece.last; ++i) {
      nearest =
          std::fmin(nearest, DistanceToSegment(at, points[i], points[i + 1]));
    }
    farthest = std::fmax(farthest, nearest);
  }
  return farthest;
}

InkCubic ToInkCubic(const Cubic& cubic) {
  return {cubic.p1, cubic.p2, cubic.p3};
}

}  // namespace

std::vector<InkCubic> FitInkStroke(pdfium::span<const CFX_PointF> stroke,
                                   double tolerance) {
  std::vector<Point> points;
  for (const CFX_PointF& point : stroke) {
    if (!points.empty() && points.back().x == point.x &&
        points.back().y == point.y) {
      continue;
    }
    points.push_back({point.x, point.y});
  }
  std::vector<InkCubic> curves;
  if (points.size() < 2) {
    return curves;
  }

  // The pieces still to fit, the next on top: a split piece's first half is
  // fitted (and split) before its second, so the cubics come out in order.
  // A stack, not recursion: a long stroke can split once per point.
  std::vector<Piece> pending = {
      {0, points.size() - 1, Unit(points[1] - points[0]),
       Unit(points[points.size() - 2] - points.back())}};
  while (!pending.empty()) {
    const Piece piece = pending.back();
    pending.pop_back();
    const Point& start = points[piece.first];
    const Point& end = points[piece.last];
    if (piece.last - piece.first == 1) {
      const double handle = Distance(end, start) / 3;
      curves.push_back(ToInkCubic({start, start + piece.leaving * handle,
                                   end + piece.arriving * handle, end}));
      continue;
    }
    const std::vector<double> places = ChordLengthPlaces(points, piece);
    const Cubic cubic = LeastSquaresCubic(points, piece, places);
    const WorstPoint worst = FindWorstPoint(points, piece, places, cubic);
    if (worst.squared_distance < tolerance * tolerance &&
        DistanceFromLines(points, piece, cubic) < tolerance) {
      curves.push_back(ToInkCubic(cubic));
      continue;
    }
    // The halves meet at the worst point, along the line from the point
    // before it to the point after.
    const size_t split = worst.index;
    const Point back = Unit(points[split - 1] - points[split + 1]);
    pending.push_back({split, piece.last, back * -1, piece.arriving});
    pending.push_back({piece.first, split, piece.leaving, back});
  }
  return curves;
}
