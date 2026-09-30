// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/page/cpdf_paintedbounds.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <vector>

#include "core/fpdfapi/page/cpdf_clippath.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fxge/cfx_graphstate.h"
#include "core/fxge/cfx_graphstatedata.h"
#include "core/fxge/cfx_path.h"

namespace {

// Shorter than this, a vector has no direction: a segment of no length, or
// two directions that are the same or opposite.
constexpr double kNoLength = 1e-9;

// A box that grows point by point, and holds nothing until the first.
class Bounds {
 public:
  void Add(const CFX_PointF& point) {
    if (!box_.has_value()) {
      box_ = CFX_FloatRect(point.x, point.y, point.x, point.y);
      return;
    }
    box_->UpdateRect(point);
  }

  const std::optional<CFX_FloatRect>& box() const { return box_; }

 private:
  std::optional<CFX_FloatRect> box_;
};

CFX_PointF Direction(const CFX_PointF& from, const CFX_PointF& to) {
  const double dx = to.x - from.x;
  const double dy = to.y - from.y;
  const double length = std::hypot(dx, dy);
  if (length < kNoLength) {
    return CFX_PointF();
  }
  return CFX_PointF(static_cast<float>(dx / length),
                    static_cast<float>(dy / length));
}

bool HasDirection(const CFX_PointF& direction) {
  return direction.x != 0 || direction.y != 0;
}

// The first direction of `points` away from `from`: a Bezier's tangent at
// its start (or end, with the points reversed) even when a control point
// sits on the end.
CFX_PointF FirstDirection(const CFX_PointF& from,
                          std::initializer_list<CFX_PointF> points) {
  for (const CFX_PointF& point : points) {
    const CFX_PointF direction = Direction(from, point);
    if (HasDirection(direction)) {
      return direction;
    }
  }
  return CFX_PointF();
}

// The cubic Bezier `p0..p3` at `t`.
CFX_PointF BezierAt(const CFX_PointF& p0,
                    const CFX_PointF& p1,
                    const CFX_PointF& p2,
                    const CFX_PointF& p3,
                    double t) {
  const double s = 1 - t;
  const double w0 = s * s * s;
  const double w1 = 3 * s * s * t;
  const double w2 = 3 * s * t * t;
  const double w3 = t * t * t;
  return CFX_PointF(
      static_cast<float>(w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x),
      static_cast<float>(w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y));
}

// Where one coordinate of a cubic Bezier turns: the parameters in (0, 1) at
// which its derivative is zero.
std::vector<double> Turns(double p0, double p1, double p2, double p3) {
  // The derivative, over 3: a t^2 + b t + c.
  const double a = -p0 + 3 * p1 - 3 * p2 + p3;
  const double b = 2 * (p0 - 2 * p1 + p2);
  const double c = p1 - p0;
  std::vector<double> turns;
  auto keep = [&turns](double t) {
    if (t > 0 && t < 1) {
      turns.push_back(t);
    }
  };
  if (std::fabs(a) < kNoLength) {
    if (std::fabs(b) >= kNoLength) {
      keep(-c / b);
    }
    return turns;
  }
  const double discriminant = b * b - 4 * a * c;
  if (discriminant < 0) {
    return turns;
  }
  const double root = std::sqrt(discriminant);
  keep((-b + root) / (2 * a));
  keep((-b - root) / (2 * a));
  return turns;
}

// Where a curve turns in the holder's space: the point, and the axis its
// tangent runs across there (an extreme across x has an upright tangent).
struct CurveTurn {
  CFX_PointF at;
  bool across_x;
};

// The curve `p0..p3`, not its control points; each point where it turns is
// kept in `turns`.
void AddBezier(const CFX_PointF& p0,
               const CFX_PointF& p1,
               const CFX_PointF& p2,
               const CFX_PointF& p3,
               Bounds* bounds,
               std::vector<CurveTurn>* turns) {
  bounds->Add(p0);
  bounds->Add(p3);
  for (double t : Turns(p0.x, p1.x, p2.x, p3.x)) {
    const CFX_PointF at = BezierAt(p0, p1, p2, p3, t);
    bounds->Add(at);
    turns->push_back({at, true});
  }
  for (double t : Turns(p0.y, p1.y, p2.y, p3.y)) {
    const CFX_PointF at = BezierAt(p0, p1, p2, p3, t);
    bounds->Add(at);
    turns->push_back({at, false});
  }
}

// A piece of a subpath with a direction, in the path's own space.
struct Segment {
  CFX_PointF from;
  CFX_PointF to;
  // Unit directions: leaving `from`, arriving at `to`.
  CFX_PointF leaving;
  CFX_PointF arriving;
};

struct Subpath {
  CFX_PointF start;
  CFX_PointF current;
  std::vector<Segment> segments;
  bool closed = false;

  void Append(const CFX_PointF& to,
              const CFX_PointF& leaving,
              const CFX_PointF& arriving) {
    if (HasDirection(leaving) && HasDirection(arriving)) {
      segments.push_back({current, to, leaving, arriving});
    }
    current = to;
  }
};

// The tip of a miter join at `at` (arriving along `arriving`, leaving along
// `leaving`) as the renderer draws it: within the miter limit. Past the limit
// the join is beveled, and a bevel stays within the half width.
std::optional<CFX_PointF> MiterTip(const CFX_PointF& at,
                                   const CFX_PointF& arriving,
                                   const CFX_PointF& leaving,
                                   float half_width,
                                   float miter_limit) {
  const double sum_x = arriving.x + leaving.x;
  const double sum_y = arriving.y + leaving.y;
  const double outward_x = arriving.x - leaving.x;
  const double outward_y = arriving.y - leaving.y;
  const double sum = std::hypot(sum_x, sum_y);
  const double outward = std::hypot(outward_x, outward_y);
  if (sum < kNoLength || outward < kNoLength) {
    return std::nullopt;
  }
  // One over the sine of half the angle between the two segments.
  const double ratio = 2 / sum;
  if (ratio > miter_limit) {
    return std::nullopt;
  }
  const double reach = half_width * ratio / outward;
  return CFX_PointF(static_cast<float>(at.x + outward_x * reach),
                    static_cast<float>(at.y + outward_y * reach));
}

// The outer corners of a square cap at `end`, facing `outward`.
void AddSquareCap(const CFX_PointF& end,
                  const CFX_PointF& outward,
                  float half_width,
                  const CFX_Matrix& matrix,
                  Bounds* bounds) {
  const CFX_PointF along(outward.x * half_width, outward.y * half_width);
  const CFX_PointF across(-outward.y * half_width, outward.x * half_width);
  bounds->Add(matrix.Transform(end + along + across));
  bounds->Add(matrix.Transform(end + along - across));
}

// The stroke's two edges at `point`, across `direction`.
void AddAcross(const CFX_PointF& point,
               const CFX_PointF& direction,
               float half_width,
               const CFX_Matrix& matrix,
               Bounds* bounds) {
  const CFX_PointF across(-direction.y * half_width, direction.x * half_width);
  bounds->Add(matrix.Transform(point + across));
  bounds->Add(matrix.Transform(point - across));
}

// The pen at `at` (in the holder's space): a circle of the half width in the
// path's space, reaching `reach_x` and `reach_y` in the holder's.
void AddPen(const CFX_PointF& at, float reach_x, float reach_y, Bounds* bounds) {
  bounds->Add(CFX_PointF(at.x - reach_x, at.y));
  bounds->Add(CFX_PointF(at.x + reach_x, at.y));
  bounds->Add(CFX_PointF(at.x, at.y - reach_y));
  bounds->Add(CFX_PointF(at.x, at.y + reach_y));
}

// A path's subpaths, its center line, and where its curves turn, in the
// holder's space.
std::vector<Subpath> WalkPath(const std::vector<CFX_Path::Point>& points,
                              const CFX_Matrix& matrix,
                              Bounds* line,
                              std::vector<CurveTurn>* turns) {
  std::vector<Subpath> subpaths;
  for (size_t i = 0; i < points.size(); ++i) {
    const CFX_Path::Point& point = points[i];
    if (point.type_ == CFX_Path::Point::Type::kMove) {
      subpaths.push_back({point.point_, point.point_});
      continue;
    }
    if (subpaths.empty() || subpaths.back().closed) {
      // A path goes on from the start of the subpath it closed.
      const CFX_PointF from =
          subpaths.empty() ? CFX_PointF() : subpaths.back().start;
      subpaths.push_back({from, from});
    }
    Subpath& subpath = subpaths.back();
    const CFX_PointF from = subpath.current;
    if (point.type_ == CFX_Path::Point::Type::kBezier) {
      // A path holds Beziers in threes.
      if (i + 2 >= points.size()) {
        break;
      }
      const CFX_PointF control1 = point.point_;
      const CFX_PointF control2 = points[i + 1].point_;
      const CFX_PointF to = points[i + 2].point_;
      AddBezier(matrix.Transform(from), matrix.Transform(control1),
                matrix.Transform(control2), matrix.Transform(to), line, turns);
      subpath.Append(to, FirstDirection(from, {control1, control2, to}),
                     -1.0f * FirstDirection(to, {control2, control1, from}));
      i += 2;
    } else {
      line->Add(matrix.Transform(from));
      line->Add(matrix.Transform(point.point_));
      const CFX_PointF direction = Direction(from, point.point_);
      subpath.Append(point.point_, direction, direction);
    }
    if (points[i].close_figure_) {
      const CFX_PointF direction = Direction(subpath.current, subpath.start);
      subpath.Append(subpath.start, direction, direction);
      subpath.closed = true;
    }
  }
  return subpaths;
}

// The box around what a path paints: its fill, and its stroke as the
// renderer draws it (see the header).
std::optional<CFX_FloatRect> PaintedPathBounds(const CPDF_PathObject& object) {
  const bool fills = !object.has_no_filltype();
  const bool strokes = object.stroke();
  if (!fills && !strokes) {
    return std::nullopt;
  }
  const CFX_Matrix matrix = object.matrix();
  Bounds line;
  std::vector<CurveTurn> turns;
  const std::vector<Subpath> subpaths =
      WalkPath(object.path().GetPoints(), matrix, &line, &turns);
  if (!line.box().has_value()) {
    return std::nullopt;
  }
  CFX_FloatRect box = line.box().value();
  const CFX_GraphState& state = object.graph_state();
  const float half_width = strokes ? std::max(state.GetLineWidth(), 0.0f) / 2
                                   : 0.0f;
  if (half_width <= 0) {
    return box;
  }

  // The pen: a circle of the half width in the path's space, an ellipse in
  // the holder's.
  const float reach_x = half_width * std::hypot(matrix.a, matrix.c);
  const float reach_y = half_width * std::hypot(matrix.b, matrix.d);
  const CFX_GraphStateData::LineCap cap = state.GetLineCap();
  const CFX_GraphStateData::LineJoin join = state.GetLineJoin();
  const float miter_limit = state.GetMiterLimit();
  Bounds stroke;
  for (const Subpath& subpath : subpaths) {
    const std::vector<Segment>& segments = subpath.segments;
    if (segments.empty()) {
      continue;
    }
    // Each segment is its width across it, from end to end.
    for (const Segment& segment : segments) {
      AddAcross(segment.from, segment.leaving, half_width, matrix, &stroke);
      AddAcross(segment.to, segment.arriving, half_width, matrix, &stroke);
    }
    // Where two segments meet, as the join paints it.
    auto add_join = [&](const Segment& in, const Segment& out) {
      if (join == CFX_GraphStateData::LineJoin::kRound) {
        AddPen(matrix.Transform(in.to), reach_x, reach_y, &stroke);
      } else if (join == CFX_GraphStateData::LineJoin::kMiter) {
        if (std::optional<CFX_PointF> tip = MiterTip(
                in.to, in.arriving, out.leaving, half_width, miter_limit)) {
          stroke.Add(matrix.Transform(tip.value()));
        }
      }
    };
    for (size_t k = 0; k + 1 < segments.size(); ++k) {
      add_join(segments[k], segments[k + 1]);
    }
    if (subpath.closed) {
      add_join(segments.back(), segments.front());
      continue;
    }
    // An open subpath's two ends, as the cap paints them.
    if (cap == CFX_GraphStateData::LineCap::kRound) {
      AddPen(matrix.Transform(segments.front().from), reach_x, reach_y,
             &stroke);
      AddPen(matrix.Transform(segments.back().to), reach_x, reach_y, &stroke);
    } else if (cap == CFX_GraphStateData::LineCap::kSquare) {
      AddSquareCap(segments.front().from, -1.0f * segments.front().leaving,
                   half_width, matrix, &stroke);
      AddSquareCap(segments.back().to, segments.back().arriving, half_width,
                   matrix, &stroke);
    }
  }
  // A curve's own extremes: the tangent runs across the axis there, so the
  // stroke reaches the half width past them.
  for (const CurveTurn& turn : turns) {
    if (turn.across_x) {
      stroke.Add(CFX_PointF(turn.at.x - reach_x, turn.at.y));
      stroke.Add(CFX_PointF(turn.at.x + reach_x, turn.at.y));
    } else {
      stroke.Add(CFX_PointF(turn.at.x, turn.at.y - reach_y));
      stroke.Add(CFX_PointF(turn.at.x, turn.at.y + reach_y));
    }
  }
  if (stroke.box().has_value()) {
    box.Union(stroke.box().value());
  }
  return box;
}

}  // namespace

CFX_FloatRect GetPaintedBounds(const CPDF_PageObjectHolder& holder) {
  std::optional<CFX_FloatRect> painted;
  for (const auto& object : holder) {
    if (!object->IsActive()) {
      continue;
    }
    std::optional<CFX_FloatRect> box;
    if (const CPDF_PathObject* path = object->AsPath()) {
      box = PaintedPathBounds(*path);
    } else {
      box = object->GetRect();
    }
    if (!box.has_value()) {
      continue;
    }
    if (object->clip_path().HasRef()) {
      box->Intersect(object->clip_path().GetClipBox());
    }
    if (box->IsEmpty()) {
      continue;
    }
    if (painted.has_value()) {
      painted->Union(box.value());
    } else {
      painted = box;
    }
  }
  return painted.value_or(CFX_FloatRect());
}
