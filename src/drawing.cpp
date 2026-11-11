#include "drawing.h"

#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QSaveFile>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>

namespace drawing {
namespace {

constexpr double kPi = 3.14159265358979323846;

// --- Mesh topology ---------------------------------------------------------------------------------

struct Topology {
  std::vector<QVector3D> point;
  std::vector<uint32_t> corner;      // 3 per triangle
  std::vector<QVector3D> normal;     // per triangle
  struct Edge {
    uint32_t a, b;
    int t0, t1;  // t1 = -1 on an open edge
  };
  std::vector<Edge> edges;
};

Topology buildTopology(const CpuMesh& mesh) {
  Topology topo;
  const size_t tris = mesh.triCount();
  const size_t vertexCount = mesh.vertices.size() / 3;
  topo.corner = mesh.indices;
  for (uint32_t& i : topo.corner)
    if (i >= vertexCount) i = 0;
  for (size_t i = 0; i < vertexCount; ++i) topo.point.emplace_back(mesh.vertices[i * 3], mesh.vertices[i * 3 + 1], mesh.vertices[i * 3 + 2]);
  topo.normal.resize(tris);
  std::unordered_map<uint64_t, size_t> edgeAt;
  edgeAt.reserve(tris * 2);
  for (size_t t = 0; t < tris; ++t) {
    const QVector3D& a = topo.point[topo.corner[t * 3]];
    const QVector3D& b = topo.point[topo.corner[t * 3 + 1]];
    const QVector3D& c = topo.point[topo.corner[t * 3 + 2]];
    const QVector3D n = QVector3D::crossProduct(b - a, c - a);
    topo.normal[t] = n.length() > 1e-12f ? n.normalized() : QVector3D();
    for (int e = 0; e < 3; ++e) {
      uint32_t u = topo.corner[t * 3 + e], v = topo.corner[t * 3 + (e + 1) % 3];
      if (u == v) continue;
      if (u > v) std::swap(u, v);
      const uint64_t key = (static_cast<uint64_t>(u) << 32) | v;
      const auto found = edgeAt.find(key);
      if (found == edgeAt.end()) {
        edgeAt.emplace(key, topo.edges.size());
        topo.edges.push_back({u, v, static_cast<int>(t), -1});
      } else if (topo.edges[found->second].t1 < 0) {
        topo.edges[found->second].t1 = static_cast<int>(t);
      }
    }
  }
  return topo;
}

bool isFeature(const Topology& topo, const Topology::Edge& e) {
  if (e.t1 < 0) return true;
  const QVector3D& n0 = topo.normal[static_cast<size_t>(e.t0)];
  const QVector3D& n1 = topo.normal[static_cast<size_t>(e.t1)];
  if (n0.isNull() || n1.isNull()) return false;
  return QVector3D::dotProduct(n0, n1) < 0.906f;  // sharper than 25 degrees
}

// --- Round edges in 3D: closed chains of sharp edges that form a flat circle ----------------------------

struct Circle3 {
  QVector3D centre;
  QVector3D normal;
  double radius = 0;
};

std::vector<Circle3> findCircles(const Topology& topo, const std::vector<char>& feature) {
  std::vector<std::vector<std::pair<uint32_t, size_t>>> around(topo.point.size());
  for (size_t i = 0; i < topo.edges.size(); ++i) {
    if (!feature[i]) continue;
    around[topo.edges[i].a].push_back({topo.edges[i].b, i});
    around[topo.edges[i].b].push_back({topo.edges[i].a, i});
  }
  std::vector<char> used(topo.edges.size(), 0);
  std::vector<Circle3> circles;
  for (size_t start = 0; start < topo.edges.size(); ++start) {
    if (!feature[start] || used[start]) continue;
    const auto& e0 = topo.edges[start];
    if (around[e0.a].size() != 2 || around[e0.b].size() != 2) continue;
    std::vector<uint32_t> ring{e0.a};
    used[start] = 1;
    uint32_t previous = e0.a, current = e0.b;
    bool closed = false;
    for (size_t guard = 0; guard < 100000; ++guard) {
      if (current == e0.a) {
        closed = true;
        break;
      }
      if (around[current].size() != 2) break;
      ring.push_back(current);
      const auto& next = around[current][0].first == previous ? around[current][1] : around[current][0];
      used[next.second] = 1;
      previous = current;
      current = next.first;
    }
    if (!closed || ring.size() < 12) continue;
    QVector3D centre, newell;
    double total = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
      const QVector3D& p = topo.point[ring[i]];
      const QVector3D& q = topo.point[ring[(i + 1) % ring.size()]];
      const double length = (q - p).length();
      centre += (p + q) * static_cast<float>(0.5 * length);
      total += length;
      newell += QVector3D((p.y() - q.y()) * (p.z() + q.z()), (p.z() - q.z()) * (p.x() + q.x()), (p.x() - q.x()) * (p.y() + q.y()));
    }
    if (total <= 1e-9 || newell.lengthSquared() < 1e-20f) continue;
    centre /= static_cast<float>(total);
    const QVector3D normal = newell.normalized();
    double mean = 0;
    for (uint32_t v : ring) mean += (topo.point[v] - centre).length();
    mean /= static_cast<double>(ring.size());
    if (mean < 1e-4) continue;
    bool round = true;
    for (uint32_t v : ring) {
      const QVector3D d = topo.point[v] - centre;
      if (std::abs(d.length() - mean) > 0.015 * mean || std::abs(QVector3D::dotProduct(d, normal)) > 0.01 * mean) {
        round = false;
        break;
      }
    }
    if (round) circles.push_back({centre, normal, mean});
  }
  return circles;
}

// --- Collinear pieces joined, hidden pieces under visible ones dropped -------------------------------

struct LineKey {
  long long angle, offset;
  bool operator<(const LineKey& o) const { return angle != o.angle ? angle < o.angle : offset < o.offset; }
};

struct LineGroup {
  QPointF dir, normal;
  double offset = 0;
  std::vector<std::pair<double, double>> spans;
};

void addToGroups(std::map<LineKey, LineGroup>& groups, const Segment& s) {
  const QPointF d = s.second - s.first;
  const double length = std::hypot(d.x(), d.y());
  if (length < 1e-6) return;
  double angle = std::atan2(d.y(), d.x());
  if (angle < 0) angle += kPi;
  if (angle >= kPi - 1e-7) angle -= kPi;
  const QPointF dir(std::cos(angle), std::sin(angle));
  const QPointF normal(-dir.y(), dir.x());
  const double offset = s.first.x() * normal.x() + s.first.y() * normal.y();
  const LineKey key{std::llround(angle * 2e4), std::llround(offset * 2e3)};
  LineGroup& group = groups[key];
  if (group.spans.empty()) {
    group.dir = dir;
    group.normal = normal;
    group.offset = offset;
  }
  double a = s.first.x() * group.dir.x() + s.first.y() * group.dir.y();
  double b = s.second.x() * group.dir.x() + s.second.y() * group.dir.y();
  if (a > b) std::swap(a, b);
  group.spans.push_back({a, b});
}

std::vector<std::pair<double, double>> unite(std::vector<std::pair<double, double>> spans) {
  std::sort(spans.begin(), spans.end());
  std::vector<std::pair<double, double>> out;
  for (const auto& s : spans) {
    if (!out.empty() && s.first <= out.back().second + 1e-3) out.back().second = std::max(out.back().second, s.second);
    else out.push_back(s);
  }
  return out;
}

void tidy(std::vector<Segment>& visible, std::vector<Segment>& hidden) {
  std::map<LineKey, LineGroup> seen, behind;
  for (const auto& s : visible) addToGroups(seen, s);
  for (const auto& s : hidden) addToGroups(behind, s);
  auto rebuild = [](const LineGroup& g, double a, double b) {
    const QPointF base(g.normal.x() * g.offset, g.normal.y() * g.offset);
    return Segment(base + g.dir * a, base + g.dir * b);
  };
  visible.clear();
  hidden.clear();
  for (auto& [key, group] : seen) {
    group.spans = unite(group.spans);
    for (const auto& s : group.spans)
      if (s.second - s.first > 1e-3) visible.push_back(rebuild(group, s.first, s.second));
  }
  for (auto& [key, group] : behind) {
    auto spans = unite(group.spans);
    const auto over = seen.find(key);
    if (over != seen.end()) {
      // Cut away the parts a visible line already covers.
      std::vector<std::pair<double, double>> left;
      for (auto span : spans) {
        for (const auto& v : over->second.spans) {
          if (v.second <= span.first || v.first >= span.second) continue;
          if (v.first > span.first) left.push_back({span.first, v.first});
          span.first = std::max(span.first, v.second);
          if (span.first >= span.second) break;
        }
        if (span.second > span.first) left.push_back(span);
      }
      spans = left;
    }
    for (const auto& s : spans)
      if (s.second - s.first > 0.05) hidden.push_back(rebuild(group, s.first, s.second));
  }
}

// --- One view ----------------------------------------------------------------------------------------

struct DepthBuffer {
  int width = 0, height = 0;
  double x0 = 0, y0 = 0, pixel = 1;
  std::vector<float> depth;
  float at(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) return std::numeric_limits<float>::infinity();
    return depth[static_cast<size_t>(y) * width + x];
  }
};

View makeView(const Topology& topo, const std::vector<char>& feature, const std::vector<Circle3>& circles, QVector3D dir,
              QVector3D up, bool withHidden) {
  View view;
  view.dir = dir.normalized();
  view.up = (up - view.dir * QVector3D::dotProduct(up, view.dir)).normalized();
  view.right = QVector3D::crossProduct(view.dir, view.up);
  auto project = [&](const QVector3D& p) { return QPointF(QVector3D::dotProduct(p, view.right), QVector3D::dotProduct(p, view.up)); };
  auto depthOf = [&](const QVector3D& p) { return QVector3D::dotProduct(p, view.dir); };
  if (topo.point.empty()) return view;
  double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
  for (const QVector3D& p : topo.point) {
    const QPointF q = project(p);
    minX = std::min(minX, q.x());
    maxX = std::max(maxX, q.x());
    minY = std::min(minY, q.y());
    maxY = std::max(maxY, q.y());
  }
  view.bounds = QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
  const double extent = std::max({maxX - minX, maxY - minY, 1e-3});

  // Depth of the nearest surface per pixel.
  DepthBuffer buffer;
  buffer.pixel = std::max(extent / 1800.0, 0.005);
  buffer.x0 = minX - 2 * buffer.pixel;
  buffer.y0 = minY - 2 * buffer.pixel;
  buffer.width = static_cast<int>(std::ceil((maxX - minX) / buffer.pixel)) + 5;
  buffer.height = static_cast<int>(std::ceil((maxY - minY) / buffer.pixel)) + 5;
  buffer.depth.assign(static_cast<size_t>(buffer.width) * buffer.height, std::numeric_limits<float>::infinity());
  const size_t tris = topo.corner.size() / 3;
  for (size_t t = 0; t < tris; ++t) {
    double px[3], py[3], pz[3];
    for (int c = 0; c < 3; ++c) {
      const QVector3D& p = topo.point[topo.corner[t * 3 + c]];
      const QPointF q = project(p);
      px[c] = (q.x() - buffer.x0) / buffer.pixel;
      py[c] = (q.y() - buffer.y0) / buffer.pixel;
      pz[c] = depthOf(p);
    }
    const double area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
    if (std::abs(area) < 1e-12) continue;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({px[0], px[1], px[2]}))));
    const int x1 = std::min(buffer.width - 1, static_cast<int>(std::ceil(std::max({px[0], px[1], px[2]}))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({py[0], py[1], py[2]}))));
    const int y1 = std::min(buffer.height - 1, static_cast<int>(std::ceil(std::max({py[0], py[1], py[2]}))));
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        const double cx = x + 0.5, cy = y + 0.5;
        const double w0 = ((px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy)) / area;
        const double w1 = ((px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy)) / area;
        const double w2 = 1 - w0 - w1;
        if (w0 < -1e-6 || w1 < -1e-6 || w2 < -1e-6) continue;
        const float z = static_cast<float>(w0 * pz[0] + w1 * pz[1] + w2 * pz[2]);
        float& slot = buffer.depth[static_cast<size_t>(y) * buffer.width + x];
        if (z < slot) slot = z;
      }
    }
  }
  const double slack = 3 * buffer.pixel + 1e-4 * extent;
  // A point shows when any pixel around it has nothing nearer (or nothing at all).
  auto shows = [&](const QPointF& q, double depth) {
    const int x = static_cast<int>(std::floor((q.x() - buffer.x0) / buffer.pixel));
    const int y = static_cast<int>(std::floor((q.y() - buffer.y0) / buffer.pixel));
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx)
        if (buffer.at(x + dx, y + dy) >= depth - slack) return true;
    return false;
  };

  // Sharp edges, and the outline of round surfaces as seen from here.
  std::vector<Segment> visible, hidden;
  for (size_t i = 0; i < topo.edges.size(); ++i) {
    const auto& e = topo.edges[i];
    bool use = feature[i] != 0;
    if (!use && e.t1 >= 0) {
      // The outline of a round surface: where it turns from facing us to facing away, or onto
      // a face seen exactly edge on (the side of a block with rounded corners).
      auto facing = [&](int t) {
        const float d = QVector3D::dotProduct(topo.normal[static_cast<size_t>(t)], view.dir);
        return d < -1e-5f ? -1 : d > 1e-5f ? 1 : 0;
      };
      use = facing(e.t0) != facing(e.t1);
    }
    if (!use) continue;
    const QVector3D& a = topo.point[e.a];
    const QVector3D& b = topo.point[e.b];
    const QPointF pa = project(a), pb = project(b);
    const double length = std::hypot(pb.x() - pa.x(), pb.y() - pa.y());
    if (length < 1e-5) continue;  // seen end on
    const int samples = std::clamp(static_cast<int>(std::ceil(length / (buffer.pixel * 0.7))), 2, 6000);
    const double da = depthOf(a), db = depthOf(b);
    bool state = shows(pa, da);
    double runStart = 0;
    for (int k = 1; k <= samples; ++k) {
      const double t = static_cast<double>(k) / samples;
      const bool now = shows(pa + (pb - pa) * t, da + (db - da) * t);
      if (now == state && k < samples) continue;
      const double end = now == state ? 1.0 : t - 0.5 / samples;
      const Segment piece(pa + (pb - pa) * runStart, pa + (pb - pa) * end);
      (state ? visible : hidden).push_back(piece);
      runStart = end;
      state = now;
      if (k == samples && end < 1.0) (state ? visible : hidden).push_back(Segment(pa + (pb - pa) * end, pb));
    }
  }
  if (!withHidden) hidden.clear();
  tidy(visible, hidden);
  view.visible = std::move(visible);
  view.hidden = std::move(hidden);

  // Round edges seen square on.
  for (const Circle3& c : circles) {
    if (std::abs(QVector3D::dotProduct(c.normal, view.dir)) < 0.9995f) continue;
    const QPointF centre = project(c.centre);
    const double depth = depthOf(c.centre);
    int seen = 0;
    for (int k = 0; k < 48; ++k) {
      const double a = 2 * kPi * k / 48;
      if (shows(centre + QPointF(std::cos(a), std::sin(a)) * c.radius, depth)) ++seen;
    }
    const bool show = seen >= 24;
    bool merged = false;
    for (ViewCircle& other : view.circles) {
      if (std::hypot(other.centre.x() - centre.x(), other.centre.y() - centre.y()) < 0.01 && std::abs(other.radius - c.radius) < 0.01) {
        other.visible = other.visible || show;
        merged = true;
      }
    }
    if (!merged) view.circles.push_back({centre, c.radius, show});
  }
  view.valid = true;
  return view;
}

}  // namespace

Model analyse(const CpuMesh& mesh) {
  Model model;
  if (mesh.triCount() == 0) return model;
  const Topology topo = buildTopology(mesh);
  std::vector<char> feature(topo.edges.size(), 0);
  for (size_t i = 0; i < topo.edges.size(); ++i) feature[i] = isFeature(topo, topo.edges[i]) ? 1 : 0;
  const std::vector<Circle3> circles = findCircles(topo, feature);
  const size_t tris = topo.corner.size() / 3;
  for (size_t t = 0; t < tris; ++t) {
    const QVector3D& a = topo.point[topo.corner[t * 3]];
    const QVector3D& b = topo.point[topo.corner[t * 3 + 1]];
    const QVector3D& c = topo.point[topo.corner[t * 3 + 2]];
    model.area += 0.5 * QVector3D::crossProduct(b - a, c - a).length();
  }
  model.volume = std::abs(mesh.volume);
  model.lo = QVector3D(1e30f, 1e30f, 1e30f);
  model.hi = -model.lo;
  for (const QVector3D& p : topo.point) {
    for (int i = 0; i < 3; ++i) {
      model.lo[i] = std::min(model.lo[i], p[i]);
      model.hi[i] = std::max(model.hi[i], p[i]);
    }
  }
  const QVector3D X(1, 0, 0), Y(0, 1, 0), Z(0, 0, 1);
  model.front = makeView(topo, feature, circles, Y, Z, true);   // from the front (-Y), looking back
  model.top = makeView(topo, feature, circles, -Z, Y, true);    // from above
  model.left = makeView(topo, feature, circles, X, Z, true);    // from the left (-X)
  model.right = makeView(topo, feature, circles, -X, Z, true);  // from the right (+X)
  model.iso = makeView(topo, feature, circles, QVector3D(-1, 1, -1), Z, false);
  model.valid = true;
  return model;
}

// --- Filament --------------------------------------------------------------------------------------

const std::vector<Material>& materials() {
  static const std::vector<Material> list = {
      {QStringLiteral("PLA"), 1.24},  {QStringLiteral("PETG"), 1.27}, {QStringLiteral("ABS"), 1.04},
      {QStringLiteral("ASA"), 1.07},  {QStringLiteral("TPU"), 1.21},  {QStringLiteral("PA (nylon)"), 1.14},
      {QStringLiteral("PC"), 1.20},   {QStringLiteral("PLA-CF"), 1.29}, {QStringLiteral("PETG-CF"), 1.30},
  };
  return list;
}

FilamentEstimate estimate(double volume, double area, const Filament& f) {
  FilamentEstimate e;
  if (volume <= 0) return e;
  const double shell = std::min(volume, std::max(0.0, area) * std::max(0.0, f.walls));
  const double printed = shell + std::clamp(f.infill, 0.0, 100.0) / 100.0 * (volume - shell);  // mm3
  e.grams = printed / 1000.0 * f.density;
  const double section = kPi * f.diameter * f.diameter / 4.0;
  e.metres = section > 0 ? printed / section / 1000.0 : 0;
  e.cost = e.grams / 1000.0 * f.pricePerKg;
  return e;
}

// --- Layout ----------------------------------------------------------------------------------------

QSizeF paperSize(const QString& paper) {
  if (paper == QLatin1String("A4")) return QSizeF(297, 210);
  if (paper == QLatin1String("A2")) return QSizeF(594, 420);
  return QSizeF(420, 297);
}

QString scaleText(double scale) {
  if (scale <= 0) return QStringLiteral("-");
  auto plain = [](double v) {
    QString t = QString::number(v, 'f', 2);
    while (t.contains(QLatin1Char('.')) && (t.endsWith(QLatin1Char('0')) || t.endsWith(QLatin1Char('.')))) t.chop(1);
    return t;
  };
  return scale >= 1 ? QStringLiteral("%1:1").arg(plain(scale)) : QStringLiteral("1:%1").arg(plain(1.0 / scale));
}

namespace {

QString number(double v) {
  if (std::abs(v) < 0.005) v = 0;
  QString t = QString::number(v, 'f', 2);
  while (t.contains(QLatin1Char('.')) && (t.endsWith(QLatin1Char('0')) || t.endsWith(QLatin1Char('.')))) t.chop(1);
  return t;
}

double textWidth(const QString& text, double height) { return 0.62 * height * text.size(); }

struct Pen {
  Sheet& sheet;
  void line(QPointF a, QPointF b, Stroke stroke) { sheet.lines.push_back({a, b, stroke}); }
  void rect(const QRectF& r, Stroke stroke) {
    line(r.topLeft(), r.topRight(), stroke);
    line(r.topRight(), r.bottomRight(), stroke);
    line(r.bottomRight(), r.bottomLeft(), stroke);
    line(r.bottomLeft(), r.topLeft(), stroke);
  }
  void text(QPointF at, const QString& t, double height, int align, double angle = 0, bool bold = false) {
    sheet.texts.push_back({at, t, height, angle, align, bold});
  }
  void arrow(QPointF tip, QPointF dir) {
    const double length = std::hypot(dir.x(), dir.y());
    if (length > 1e-9) sheet.arrows.push_back({tip, dir / length});
  }

  // A horizontal dimension between x = a and x = b. objA/objB: where the extension lines start
  // (the feature), y: the dimension line.
  void dimensionX(double a, double b, double objA, double objB, double y, double value) {
    if (a > b) {
      std::swap(a, b);
      std::swap(objA, objB);
    }
    auto extension = [&](double x, double from) {
      const double sign = y > from ? 1 : -1;
      line(QPointF(x, from + sign * 1.0), QPointF(x, y + sign * 2.0), Stroke::Thin);
    };
    extension(a, objA);
    extension(b, objB);
    const QString label = number(value);
    const double room = b - a;
    if (room >= 8) {
      line(QPointF(a, y), QPointF(b, y), Stroke::Thin);
      arrow(QPointF(a, y), QPointF(-1, 0));
      arrow(QPointF(b, y), QPointF(1, 0));
    } else {
      line(QPointF(a - 5, y), QPointF(b + 5, y), Stroke::Thin);
      arrow(QPointF(a, y), QPointF(1, 0));
      arrow(QPointF(b, y), QPointF(-1, 0));
    }
    if (room >= textWidth(label, 3.5) + 6) text(QPointF((a + b) * 0.5, y - 1.0), label, 3.5, Qt::AlignHCenter | Qt::AlignBottom);
    else text(QPointF(b + 6, y - 1.0), label, 3.5, Qt::AlignLeft | Qt::AlignBottom);
  }

  void dimensionY(double a, double b, double objA, double objB, double x, double value) {
    if (a > b) {
      std::swap(a, b);
      std::swap(objA, objB);
    }
    auto extension = [&](double y, double from) {
      const double sign = x > from ? 1 : -1;
      line(QPointF(from + sign * 1.0, y), QPointF(x + sign * 2.0, y), Stroke::Thin);
    };
    extension(a, objA);
    extension(b, objB);
    const QString label = number(value);
    const double room = b - a;
    if (room >= 8) {
      line(QPointF(x, a), QPointF(x, b), Stroke::Thin);
      arrow(QPointF(x, a), QPointF(0, -1));
      arrow(QPointF(x, b), QPointF(0, 1));
    } else {
      line(QPointF(x, a - 5), QPointF(x, b + 5), Stroke::Thin);
      arrow(QPointF(x, a), QPointF(0, 1));
      arrow(QPointF(x, b), QPointF(0, -1));
    }
    // Rotated a quarter turn, reading from the bottom (or the right, standing on its side).
    if (room >= textWidth(label, 3.5) + 6) text(QPointF(x - 1.0, (a + b) * 0.5), label, 3.5, Qt::AlignHCenter | Qt::AlignBottom, 90);
    else text(QPointF(x - 1.0, a - 6), label, 3.5, Qt::AlignLeft | Qt::AlignBottom, 90);
  }
};

// What each view will carry, so the room around it can be set aside before the scale is chosen.
struct Plan {
  const View* view = nullptr;
  bool overallX = false, overallY = false;
  std::vector<std::vector<const ViewCircle*>> groups;  // visible circles by diameter
  std::vector<const ViewCircle*> byX, byY;             // circles whose position gets dimensioned
  double padLeft = 6, padRight = 6, padTop = 6, padBottom = 6;
};

Plan planView(const View& view, bool overallX, bool overallY, bool dimensions) {
  Plan plan;
  plan.view = &view;
  if (!dimensions) return plan;
  plan.overallX = overallX;
  plan.overallY = overallY;
  if (overallX) plan.padBottom = 15;
  if (overallY) plan.padLeft = 15;
  std::map<long long, std::vector<const ViewCircle*>> byDiameter;
  for (const ViewCircle& c : view.circles)
    if (c.visible) byDiameter[std::llround(c.radius * 200)].push_back(&c);
  for (auto& [key, list] : byDiameter) plan.groups.push_back(list);
  // Positions: one dimension per distinct centre line, at most four each way.
  std::map<long long, const ViewCircle*> xs, ys;
  for (const ViewCircle& c : view.circles) {
    if (!c.visible) continue;
    xs.emplace(std::llround(c.centre.x() * 100), &c);
    ys.emplace(std::llround(c.centre.y() * 100), &c);
  }
  // A circle centred on the part needs no position: the overall size already says it.
  auto centred = [](double v, double lo, double hi) { return std::abs(v - (lo + hi) * 0.5) < 0.01; };
  for (const auto& [k, c] : xs)
    if (plan.byX.size() < 4 && !centred(c->centre.x(), view.bounds.left(), view.bounds.right())) plan.byX.push_back(c);
  for (const auto& [k, c] : ys)
    if (plan.byY.size() < 4 && !centred(c->centre.y(), view.bounds.top(), view.bounds.bottom())) plan.byY.push_back(c);
  if (!plan.byX.empty()) plan.padTop = std::max(plan.padTop, 9.0 + 7.0 * plan.byX.size());
  if (!plan.byY.empty()) plan.padRight = std::max(plan.padRight, 9.0 + 7.0 * plan.byY.size());
  // Diameter callouts stand in a column to the right of the view, past its dimensions.
  if (!plan.groups.empty()) {
    double widest = 0;
    for (const auto& group : plan.groups)
      widest = std::max(widest, textWidth(QStringLiteral("00\u00d7 \u00d8") + number(group.front()->radius * 2), 3.5));
    plan.padRight = (plan.byY.empty() ? 6.0 : plan.padRight) + 6 + widest + 3;
  }
  return plan;
}

// Lines, circles' centre marks and dimensions of one view, placed with its model point
// (bounds.left, bounds.bottom) at sheet `topLeft`.
void drawView(Pen& pen, const Plan& plan, QPointF topLeft, double scale, bool hiddenLines) {
  const View& view = *plan.view;
  const QRectF& b = view.bounds;
  auto map = [&](QPointF p) { return QPointF(topLeft.x() + scale * (p.x() - b.left()), topLeft.y() + scale * (b.bottom() - p.y())); };
  for (const Segment& s : view.visible) pen.line(map(s.first), map(s.second), Stroke::Visible);
  if (hiddenLines)
    for (const Segment& s : view.hidden) pen.line(map(s.first), map(s.second), Stroke::Hidden);
  const double left = topLeft.x(), top = topLeft.y();
  const double right = left + scale * b.width(), bottom = top + scale * b.height();
  for (const ViewCircle& c : view.circles) {
    if (!c.visible) continue;
    const QPointF centre = map(c.centre);
    const double r = c.radius * scale + 2;
    pen.line(centre - QPointF(r, 0), centre + QPointF(r, 0), Stroke::Centre);
    pen.line(centre - QPointF(0, r), centre + QPointF(0, r), Stroke::Centre);
  }
  if (plan.overallX) pen.dimensionX(left, right, bottom, bottom, bottom + 9, b.width());
  if (plan.overallY) pen.dimensionY(top, bottom, left, left, left - 9, b.height());
  // Diameters: one callout per size, in a column right of the view, with a leader to the circle
  // furthest up and right of that size.
  struct Callout {
    const ViewCircle* circle;
    QString label;
  };
  std::vector<Callout> callouts;
  for (const auto& group : plan.groups) {
    const ViewCircle* pick = group.front();
    for (const ViewCircle* c : group)
      if (c->centre.x() + c->centre.y() > pick->centre.x() + pick->centre.y()) pick = c;
    callouts.push_back({pick, (group.size() > 1 ? QStringLiteral("%1\u00d7 ").arg(group.size()) : QString()) + QStringLiteral("\u00d8") +
                                  number(pick->radius * 2)});
  }
  // Top to bottom in the order the circles' tops come, biggest first when they share a centre,
  // so the leaders cross as little as possible.
  std::sort(callouts.begin(), callouts.end(), [](const Callout& a, const Callout& b) {
    const double ta = a.circle->centre.y() + a.circle->radius, tb = b.circle->centre.y() + b.circle->radius;
    return std::abs(ta - tb) > 1e-6 ? ta > tb : a.circle->radius > b.circle->radius;
  });
  const double column = right + (plan.byY.empty() ? 6.0 : 9.0 + 7.0 * plan.byY.size()) + 6;
  for (size_t i = 0; i < callouts.size(); ++i) {
    const double y = top + 4 + 7.0 * static_cast<double>(i);
    const QPointF centre = map(callouts[i].circle->centre);
    const double r = callouts[i].circle->radius * scale;
    const QPointF elbow(column - 3, y);
    QPointF toward = elbow - centre;
    const double distance = std::hypot(toward.x(), toward.y());
    toward = distance > 1e-9 ? toward / distance : QPointF(1, 0);
    const QPointF onCircle = centre + toward * r;
    pen.line(onCircle, elbow, Stroke::Thin);
    pen.line(elbow, QPointF(column + textWidth(callouts[i].label, 3.5), y), Stroke::Thin);
    pen.arrow(onCircle, onCircle - elbow);
    pen.text(QPointF(column, y - 1), callouts[i].label, 3.5, Qt::AlignLeft | Qt::AlignBottom);
  }
  // Positions from the left and bottom edges, stacked above and to the right.
  int level = 0;
  for (const ViewCircle* c : plan.byX) {
    const QPointF centre = map(c->centre);
    const double y = top - 9 - 7 * level++;
    pen.dimensionX(left, centre.x(), top, centre.y() - c->radius * scale - 1, y, c->centre.x() - b.left());
  }
  level = 0;
  for (const ViewCircle* c : plan.byY) {
    const QPointF centre = map(c->centre);
    const double x = right + 9 + 7 * level++;
    pen.dimensionY(centre.y(), bottom, centre.x() + c->radius * scale + 1, right, x, c->centre.y() - b.top());
  }
}

}  // namespace

namespace {

constexpr double kTableW = 84, kTableRow = 6, kTableH = kTableRow * 6;
constexpr double kTitleW = 140, kTitleH = 24;

QRectF frameOf(const Sheet& sheet) { return QRectF(10, 10, sheet.size.width() - 20, sheet.size.height() - 20); }

void filamentTable(Pen& pen, const QRectF& frame, const Model& model, const Options& o) {
  const QRectF t(frame.left(), frame.top(), kTableW, kTableH);
  pen.rect(t, Stroke::Frame);
  const FilamentEstimate e = estimate(model.volume, model.area, o.filament);
  const QString approx = QStringLiteral("≈ ");
  const QString grams = e.grams < 10 ? QString::number(e.grams, 'f', 1) : QString::number(std::round(e.grams), 'f', 0);
  const QStringList labels = {QStringLiteral("Material"), QStringLiteral("Weight"), QStringLiteral("Filament"), QStringLiteral("Cost")};
  const QStringList values = {
      QStringLiteral("%1, %2 g/cm³").arg(o.filament.material, QString::number(o.filament.density, 'f', 2)),
      approx + grams + QStringLiteral(" g"),
      approx + QStringLiteral("%1 m of Ø%2").arg(QString::number(e.metres, 'f', e.metres < 10 ? 2 : 1), number(o.filament.diameter)),
      approx + QStringLiteral("%1 %2  (%1 %3/kg)").arg(o.filament.currency, QString::number(e.cost, 'f', 2), QString::number(o.filament.pricePerKg, 'f', 2))};
  pen.text(QPointF(t.left() + 2, t.top() + kTableRow * 0.5), QStringLiteral("Filament estimate"), 3.0, Qt::AlignLeft | Qt::AlignVCenter, 0, true);
  for (int i = 0; i < 4; ++i) {
    const double y = t.top() + kTableRow * (i + 1);
    pen.line(QPointF(t.left(), y), QPointF(t.right(), y), Stroke::Thin);
    pen.text(QPointF(t.left() + 2, y + kTableRow * 0.5), labels[i], 2.5, Qt::AlignLeft | Qt::AlignVCenter);
    pen.text(QPointF(t.left() + 22, y + kTableRow * 0.5), values[i], 2.5, Qt::AlignLeft | Qt::AlignVCenter);
  }
  pen.line(QPointF(t.left() + 20, t.top() + kTableRow), QPointF(t.left() + 20, t.top() + kTableRow * 5), Stroke::Thin);
  const double y = t.top() + kTableRow * 5;
  pen.line(QPointF(t.left(), y), QPointF(t.right(), y), Stroke::Thin);
  pen.text(QPointF(t.left() + 2, y + kTableRow * 0.5),
           QStringLiteral("%1 % infill, %2 mm walls. A guess: your slicer knows better.").arg(number(o.filament.infill), number(o.filament.walls)),
           1.8, Qt::AlignLeft | Qt::AlignVCenter);
}

// The title block, bottom right. what: the view on this sheet (empty on the first sheet).
void titleBlock(Pen& pen, const QRectF& frame, const Options& o, double scale, const QString& what, int index, int count) {
  const QRectF tb(frame.right() - kTitleW, frame.bottom() - kTitleH, kTitleW, kTitleH);
  pen.rect(tb, Stroke::Frame);
  pen.line(QPointF(tb.left(), tb.top() + 10), QPointF(tb.right(), tb.top() + 10), Stroke::Thin);
  pen.line(QPointF(tb.left(), tb.top() + 17), QPointF(tb.right(), tb.top() + 17), Stroke::Thin);
  QString title = o.title.isEmpty() ? QStringLiteral("Untitled") : o.title;
  const double titleHeight = what.isEmpty() ? 5 : 4;
  if (!what.isEmpty()) title += QStringLiteral(" · ") + what;
  while (title.size() > 4 && textWidth(title, titleHeight) > kTitleW - 6) title = title.left(title.size() - 2) + QStringLiteral("…");
  pen.text(QPointF(tb.left() + 3, tb.top() + 5), title, titleHeight, Qt::AlignLeft | Qt::AlignVCenter, 0, true);
  const QStringList heads = {QStringLiteral("Scale"), QStringLiteral("Paper"), QStringLiteral("Units"), QStringLiteral("Projection"),
                             QStringLiteral("Sheet")};
  const QStringList cells = {scaleText(scale), o.paper, QStringLiteral("mm"),
                             o.thirdAngle ? QStringLiteral("Third angle") : QStringLiteral("First angle"),
                             QStringLiteral("%1 of %2").arg(index).arg(count)};
  const double cw[5] = {26, 22, 18, 40, 34};
  double x = tb.left();
  for (int i = 0; i < 5; ++i) {
    if (i > 0) pen.line(QPointF(x, tb.top() + 10), QPointF(x, tb.top() + 17), Stroke::Thin);
    pen.text(QPointF(x + 1.5, tb.top() + 11.6), heads[i], 1.6, Qt::AlignLeft | Qt::AlignVCenter);
    pen.text(QPointF(x + 1.5, tb.top() + 14.5), cells[i], 2.5, Qt::AlignLeft | Qt::AlignVCenter);
    x += cw[i];
  }
  pen.line(QPointF(tb.left() + 70, tb.top() + 17), QPointF(tb.left() + 70, tb.bottom()), Stroke::Thin);
  pen.text(QPointF(tb.left() + 1.5, tb.top() + 18.6), QStringLiteral("Date"), 1.6, Qt::AlignLeft | Qt::AlignVCenter);
  pen.text(QPointF(tb.left() + 1.5, tb.top() + 21.5), o.date, 2.5, Qt::AlignLeft | Qt::AlignVCenter);
  pen.text(QPointF(tb.left() + 71.5, tb.top() + 20.5), QStringLiteral("Drawn with DaedalCAD"), 2.5, Qt::AlignLeft | Qt::AlignVCenter);
}

double standardScaleUpTo(double fit) {
  static const double standard[] = {20, 10, 5, 2, 1, 0.5, 0.2, 0.1, 0.05, 0.02, 0.01};
  for (double s : standard)
    if (s <= fit * (1 + 1e-9)) return s;
  return 0.01;
}

// The front, top and side views (and the isometric one) together on the first sheet. With
// scale <= 0 only works out the largest scale they fit at, and returns it.
double threeViews(Pen* pen, const Model& model, const Options& o, const QRectF& frame, double scale) {
  if (!model.valid || !model.front.valid) return 1;
  const View& side = o.thirdAngle ? model.right : model.left;
  Plan front = planView(model.front, true, true, o.dimensions);
  Plan top = planView(model.top, false, true, o.dimensions);
  Plan sidePlan = planView(side, false, false, o.dimensions);
  const double X = std::max(model.front.bounds.width(), 1e-3);
  const double Z = std::max(model.front.bounds.height(), 1e-3);
  const double Y = std::max(model.top.bounds.height(), 1e-3);
  const double gridLeft = frame.left() + 4, gridRight = frame.right() - 4;
  const double gridTop = frame.top() + (o.filamentTable ? kTableH : 0) + 6;
  const double gridBottom = frame.bottom() - kTitleH - 6;
  const double c0L = std::max(front.padLeft, top.padLeft), c0R = std::max(front.padRight, top.padRight);
  const double c1L = sidePlan.padLeft, c1R = sidePlan.padRight;
  const double fT = std::max(front.padTop, sidePlan.padTop), fB = std::max(front.padBottom, sidePlan.padBottom);
  const double tT = top.padTop, tB = top.padBottom;
  const double availW = gridRight - gridLeft, availH = gridBottom - gridTop;
  if (!pen) {
    const double fitW = (availW - c0L - c0R - c1L - c1R) / (X + Y);
    const double fitH = (availH - fT - fB - tT - tB) / (Z + Y);
    return std::min(fitW, fitH);
  }
  // Views close together (as a drawing reads best), the rest of the room around them.
  const double col0 = c0L + scale * X + c0R, col1 = c1L + scale * Y + c1R;
  const double spareX = std::max(0.0, availW - col0 - col1), betweenX = std::min(spareX / 3, 20.0);
  const double col0Left = gridLeft + (spareX - betweenX) / 2, col1Left = col0Left + col0 + betweenX;
  const double frontRow = fT + scale * Z + fB, topRow = tT + scale * Y + tB;
  const double spareY = std::max(0.0, availH - frontRow - topRow), betweenY = std::min(spareY / 3, 15.0);
  const double row0Top = gridTop + (spareY - betweenY) / 2, row1Top = row0Top + (o.thirdAngle ? topRow : frontRow) + betweenY;
  const double frontTop = (o.thirdAngle ? row1Top : row0Top) + fT;
  const double topTop = (o.thirdAngle ? row0Top : row1Top) + tT;
  drawView(*pen, front, QPointF(col0Left + c0L, frontTop), scale, o.hidden);
  drawView(*pen, top, QPointF(col0Left + c0L, topTop), scale, o.hidden);
  drawView(*pen, sidePlan, QPointF(col1Left + c1L, frontTop), scale, o.hidden);
  // The isometric view in the free corner, at its own size.
  if (o.iso && model.iso.valid) {
    QRectF slot = o.thirdAngle ? QRectF(QPointF(col1Left, frame.top() + 4), QPointF(gridRight, row1Top - 2))
                               : QRectF(QPointF(col1Left, row1Top), QPointF(gridRight, frame.bottom() - kTitleH - 4));
    slot.adjust(6, 6, -6, -10);
    const QRectF& b = model.iso.bounds;
    if (slot.width() > 10 && slot.height() > 10 && b.width() > 0 && b.height() > 0) {
      const double s = std::min({slot.width() / b.width(), slot.height() / b.height(), scale});
      const QPointF at(slot.center().x() - s * b.width() / 2, slot.center().y() - s * b.height() / 2);
      Plan iso = planView(model.iso, false, false, false);
      drawView(*pen, iso, at, s, false);
      pen->text(QPointF(slot.center().x(), at.y() + s * b.height() + 6), QStringLiteral("Isometric view (not to scale)"), 2.5,
                Qt::AlignHCenter | Qt::AlignBottom);
    }
  }
  return scale;
}

// Cuts a line to a rectangle (Liang-Barsky); false when none of it is inside.
bool clipLine(QPointF& a, QPointF& b, const QRectF& r) {
  const double dx = b.x() - a.x(), dy = b.y() - a.y();
  double t0 = 0, t1 = 1;
  const double p[4] = {-dx, dx, -dy, dy};
  const double q[4] = {a.x() - r.left(), r.right() - a.x(), a.y() - r.top(), r.bottom() - a.y()};
  for (int i = 0; i < 4; ++i) {
    if (std::abs(p[i]) < 1e-12) {
      if (q[i] < 0) return false;
      continue;
    }
    const double t = q[i] / p[i];
    if (p[i] < 0) t0 = std::max(t0, t);
    else t1 = std::min(t1, t);
    if (t0 > t1) return false;
  }
  const QPointF start = a;
  a = start + QPointF(dx, dy) * t0;
  b = start + QPointF(dx, dy) * t1;
  return true;
}

// Copies the part of `source` inside `window` onto `target`, moved by `shift`.
void copyWindow(Pen& target, const Sheet& source, const QRectF& window, QPointF shift) {
  for (Line l : source.lines)
    if (clipLine(l.a, l.b, window)) target.line(l.a + shift, l.b + shift, l.stroke);
  for (const Arrow& a : source.arrows)
    if (window.contains(a.tip)) target.sheet.arrows.push_back({a.tip + shift, a.dir});
  for (Text t : source.texts)
    if (window.contains(t.at)) {
      t.at += shift;
      target.sheet.texts.push_back(t);
    }
}

}  // namespace

std::vector<Sheet> layoutSheets(const Model& model, const Options& o, double* scaleUsed) {
  std::vector<Sheet> sheets(1);
  sheets[0].size = paperSize(o.paper);
  const QRectF frame = frameOf(sheets[0]);
  const double fit = threeViews(nullptr, model, o, frame, 0);
  const double overview = standardScaleUpTo(fit);
  const double scale = o.scale > 0 ? o.scale : overview;
  if (scaleUsed) *scaleUsed = scale;
  // Too big for one sheet at the scale asked for: the whole part on the first sheet at a scale
  // that fits, and each view at the scale asked for on sheets of its own.
  const bool split = o.scale > 0 && model.valid && model.front.valid && o.scale > fit * (1 + 1e-9);
  {
    Pen pen{sheets[0]};
    pen.rect(frame, Stroke::Frame);
    if (o.filamentTable) filamentTable(pen, frame, model, o);
    threeViews(&pen, model, o, frame, split ? overview : scale);
  }
  std::vector<QString> what(1);  // the view on each sheet, for its title block
  if (split) {
    const QRectF area(frame.left() + 4, frame.top() + 4, frame.width() - 8, frame.height() - kTitleH - 10);
    const bool third = o.thirdAngle;
    const std::pair<const View*, QString> views[] = {
        {&model.front, QStringLiteral("Front view")},
        {&model.top, QStringLiteral("Top view")},
        {third ? &model.right : &model.left, third ? QStringLiteral("View from the right") : QStringLiteral("View from the left")}};
    constexpr int kMostTiles = 24;
    QStringList skipped;
    for (const auto& [view, name] : views) {
      if (!view->valid) continue;
      // The view on a sheet of its own, with every overall size dimensioned.
      const Plan plan = planView(*view, true, true, o.dimensions);
      Sheet drawn;
      Pen pen{drawn};
      const QPointF at(plan.padLeft, plan.padTop + 8);
      drawView(pen, plan, at, o.scale, o.hidden);
      const double w = plan.padLeft + o.scale * view->bounds.width() + plan.padRight;
      const double h = 8 + plan.padTop + o.scale * view->bounds.height() + plan.padBottom;
      const QString heading = QStringLiteral("%1, %2").arg(name, scaleText(o.scale));
      // Pieces of the view where it is bigger than a sheet, all the same size and overlapping a
      // little so they join; a line at the top of each says which piece it is.
      const double overlap = 10;
      const QRectF pieceArea = area.adjusted(0, 8, 0, 0);
      const int cols = w <= area.width() ? 1 : static_cast<int>(std::ceil((w - overlap) / (pieceArea.width() - overlap)));
      const int rows = h <= area.height() && cols == 1 ? 1 : std::max(1, static_cast<int>(std::ceil((h - overlap) / (pieceArea.height() - overlap))));
      if (cols * rows > kMostTiles) {
        skipped << QStringLiteral("%1 (%2 sheets)").arg(name).arg(cols * rows);
        continue;
      }
      const double pieceW = cols == 1 ? std::min(w, pieceArea.width()) : (w + (cols - 1) * overlap) / cols;
      const double pieceH = rows == 1 ? std::min(h, pieceArea.height()) : (h + (rows - 1) * overlap) / rows;
      for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
          Sheet sheet;
          sheet.size = sheets[0].size;
          Pen out{sheet};
          out.rect(frame, Stroke::Frame);
          QRectF window;
          QPointF shift;
          if (cols * rows == 1) {
            // Centred, its name above it.
            shift = area.center() - QPointF(w / 2, h / 2);
            window = QRectF(-1e4, -1e4, 2e4, 2e4);  // all of it
            out.text(shift + QPointF(at.x(), 4), heading, 3.5, Qt::AlignLeft | Qt::AlignVCenter, 0, true);
          } else {
            const QPointF origin(c * (pieceW - overlap), r * (pieceH - overlap));
            // The first and last pieces keep whatever sticks out past the view's edge (labels).
            window = QRectF(origin, QSizeF(pieceW, pieceH));
            if (c == 0) window.setLeft(window.left() - 3);
            if (r == 0) window.setTop(window.top() - 3);
            if (c == cols - 1) window.setRight(window.right() + 3);
            if (r == rows - 1) window.setBottom(window.bottom() + 3);
            const QRectF placed(pieceArea.topLeft(), QSizeF(pieceW, pieceH));
            shift = placed.topLeft() - origin;
            // Chain lines where the view carries on onto the next sheet.
            if (c > 0) out.line(placed.topLeft(), placed.bottomLeft(), Stroke::Centre);
            if (c < cols - 1) out.line(placed.topRight(), placed.bottomRight(), Stroke::Centre);
            if (r > 0) out.line(placed.topLeft(), placed.topRight(), Stroke::Centre);
            if (r < rows - 1) out.line(placed.bottomLeft(), placed.bottomRight(), Stroke::Centre);
            out.text(QPointF(area.left() + 2, area.top() + 3),
                     QStringLiteral("%1, piece %2 of %3 (row %4, column %5). The pieces overlap by %6 mm.")
                         .arg(heading).arg(r * cols + c + 1).arg(cols * rows).arg(r + 1).arg(c + 1).arg(number(overlap)),
                     3.0, Qt::AlignLeft | Qt::AlignVCenter, 0, true);
          }
          copyWindow(out, drawn, window, shift);
          sheets.push_back(sheet);
          what.push_back(cols * rows == 1 ? name : QStringLiteral("%1 %2/%3").arg(name).arg(r * cols + c + 1).arg(cols * rows));
        }
    }
    // Bottom left of the first sheet: where the views at full scale are.
    Pen pen{sheets[0]};
    QStringList note = {QStringLiteral("Too big for one sheet at %1.").arg(scaleText(o.scale))};
    if (sheets.size() > 1)
      note << QStringLiteral("This sheet: the whole part at %1. Sheets 2 to %2: the views at %3.")
                  .arg(scaleText(overview)).arg(sheets.size()).arg(scaleText(o.scale));
    else
      note << QStringLiteral("Drawn at %1 instead.").arg(scaleText(overview));
    if (!skipped.isEmpty())
      note << QStringLiteral("Left out (too many sheets): %1. Choose a smaller scale or bigger paper.").arg(skipped.join(QStringLiteral(", ")));
    for (int i = 0; i < note.size(); ++i)
      pen.text(QPointF(frame.left() + 3, frame.bottom() - 3 - 4.5 * (note.size() - 1 - i)), note[i], 2.5, Qt::AlignLeft | Qt::AlignBottom,
               0, i == 0);
  }
  const int count = static_cast<int>(sheets.size());
  for (int i = 0; i < count; ++i) {
    Pen pen{sheets[static_cast<size_t>(i)]};
    titleBlock(pen, frame, o, i == 0 && split ? overview : scale, what[static_cast<size_t>(i)], i + 1, count);
  }
  return sheets;
}

Sheet layout(const Model& model, const Options& o, double* scaleUsed) { return layoutSheets(model, o, scaleUsed).front(); }

// --- Output ----------------------------------------------------------------------------------------

namespace {

double strokeWidth(Stroke s) {
  switch (s) {
    case Stroke::Visible: return 0.5;
    case Stroke::Hidden: return 0.25;
    case Stroke::Frame: return 0.5;
    case Stroke::Thin:
    case Stroke::Centre: return 0.18;
  }
  return 0.25;
}

// Dash lengths in mm (on, off, on, off...); empty: solid.
std::vector<double> dashes(Stroke s) {
  if (s == Stroke::Hidden) return {3, 1.5};
  if (s == Stroke::Centre) return {8, 1.5, 1, 1.5};
  return {};
}

void arrowCorners(const Arrow& a, QPointF corners[3]) {
  const QPointF back = a.tip - a.dir * 3.0;
  const QPointF side(-a.dir.y() * 0.5, a.dir.x() * 0.5);
  corners[0] = a.tip;
  corners[1] = back + side;
  corners[2] = back - side;
}

// Where the baseline starts, in the text's own (unrotated) frame, for a text `width` wide.
QPointF textOffset(int align, double width, double cap) {
  double dx = 0, dy = 0;
  if (align & Qt::AlignHCenter) dx = -width / 2;
  else if (align & Qt::AlignRight) dx = -width;
  if (align & Qt::AlignVCenter) dy = cap / 2;
  else if (align & Qt::AlignTop) dy = cap;
  return QPointF(dx, dy);
}

}  // namespace

void paint(QPainter& painter, const Sheet& sheet) {
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);
  painter.fillRect(QRectF(QPointF(0, 0), sheet.size), Qt::white);
  for (const Line& l : sheet.lines) {
    const double w = strokeWidth(l.stroke);
    QPen pen(Qt::black, w, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
    const auto d = dashes(l.stroke);
    if (!d.empty()) {
      QVector<qreal> pattern;
      for (double v : d) pattern << v / w;
      pen.setDashPattern(pattern);
    }
    painter.setPen(pen);
    painter.drawLine(l.a, l.b);
  }
  painter.setPen(Qt::NoPen);
  painter.setBrush(Qt::black);
  for (const Arrow& a : sheet.arrows) {
    QPointF c[3];
    arrowCorners(a, c);
    painter.drawPolygon(c, 3);
  }
  painter.setBrush(Qt::NoBrush);
  painter.setPen(Qt::black);
  for (const Text& t : sheet.texts) {
    QFont font(QStringLiteral("Arial"));
    font.setPixelSize(100);
    font.setBold(t.bold);
    const QFontMetricsF metrics(font, painter.device());
    const double cap = metrics.capHeight() > 0 ? metrics.capHeight() : 70;
    const double k = t.height / cap;
    painter.save();
    painter.setFont(font);
    painter.translate(t.at);
    painter.rotate(-t.angle);
    painter.scale(k, k);
    painter.drawText(textOffset(t.align, metrics.horizontalAdvance(t.text), cap), t.text);
    painter.restore();
  }
  painter.restore();
}

bool writeSvg(const Sheet& sheet, const QString& path, QString* error) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    if (error) *error = file.errorString();
    return false;
  }
  QTextStream out(&file);
  out.setRealNumberPrecision(4);
  out.setRealNumberNotation(QTextStream::FixedNotation);
  const double w = sheet.size.width(), h = sheet.size.height();
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "mm\" height=\"" << h << "mm\" viewBox=\"0 0 " << w << ' ' << h << "\">\n";
  out << "<rect width=\"" << w << "\" height=\"" << h << "\" fill=\"white\"/>\n";
  for (Stroke s : {Stroke::Frame, Stroke::Visible, Stroke::Hidden, Stroke::Thin, Stroke::Centre}) {
    QString d;
    QTextStream path(&d);
    path.setRealNumberPrecision(3);
    path.setRealNumberNotation(QTextStream::FixedNotation);
    for (const Line& l : sheet.lines)
      if (l.stroke == s) path << 'M' << l.a.x() << ' ' << l.a.y() << 'L' << l.b.x() << ' ' << l.b.y();
    path.flush();
    if (d.isEmpty()) continue;
    out << "<path fill=\"none\" stroke=\"black\" stroke-width=\"" << strokeWidth(s) << "\"";
    const auto dash = dashes(s);
    if (!dash.empty()) {
      out << " stroke-dasharray=\"";
      for (size_t i = 0; i < dash.size(); ++i) out << (i ? "," : "") << dash[i];
      out << "\"";
    }
    out << " d=\"" << d << "\"/>\n";
  }
  for (const Arrow& a : sheet.arrows) {
    QPointF c[3];
    arrowCorners(a, c);
    out << "<path fill=\"black\" d=\"M" << c[0].x() << ' ' << c[0].y() << 'L' << c[1].x() << ' ' << c[1].y() << 'L' << c[2].x() << ' '
        << c[2].y() << "Z\"/>\n";
  }
  for (const Text& t : sheet.texts) {
    const double size = t.height / 0.716;  // Arial's capitals are 0.716 of the font size
    const char* anchor = (t.align & Qt::AlignHCenter) ? "middle" : (t.align & Qt::AlignRight) ? "end" : "start";
    const double dy = (t.align & Qt::AlignVCenter) ? t.height / 2 : (t.align & Qt::AlignTop) ? t.height : 0;
    out << "<text transform=\"translate(" << t.at.x() << ' ' << t.at.y() << ") rotate(" << -t.angle << ")\" y=\"" << dy
        << "\" font-family=\"Arial, Helvetica, sans-serif\" font-size=\"" << size << "\" text-anchor=\"" << anchor << "\""
        << (t.bold ? " font-weight=\"bold\"" : "") << ">" << t.text.toHtmlEscaped() << "</text>\n";
  }
  out << "</svg>\n";
  out.flush();
  if (!file.commit()) {
    if (error) *error = file.errorString();
    return false;
  }
  return true;
}

bool writeDxf(const Sheet& sheet, const QString& path, QString* error) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    if (error) *error = file.errorString();
    return false;
  }
  QTextStream out(&file);
  out.setRealNumberPrecision(4);
  out.setRealNumberNotation(QTextStream::FixedNotation);
  const double h = sheet.size.height();
  auto layerOf = [](Stroke s) {
    switch (s) {
      case Stroke::Visible: return "OUTLINE";
      case Stroke::Hidden: return "HIDDEN";
      case Stroke::Centre: return "CENTER";
      case Stroke::Frame: return "FRAME";
      case Stroke::Thin: return "DIMENSIONS";
    }
    return "0";
  };
  auto line = [&](const char* layer, QPointF a, QPointF b) {
    out << "0\nLINE\n8\n" << layer << "\n10\n" << a.x() << "\n20\n" << h - a.y() << "\n30\n0.0\n11\n" << b.x() << "\n21\n" << h - b.y()
        << "\n31\n0.0\n";
  };
  // DXF R12 text is plain ASCII: the diameter sign is %%c, the degree sign %%d.
  auto ascii = [](const QString& text) {
    QString t = text;
    t.replace(QChar(0x00d8), QStringLiteral("%%c"));
    t.replace(QChar(0x00b0), QStringLiteral("%%d"));
    t.replace(QChar(0x00d7), QStringLiteral("x"));
    t.replace(QChar(0x2248), QStringLiteral("~"));
    t.replace(QChar(0x20ac), QStringLiteral("EUR"));
    t.replace(QChar(0x00b3), QStringLiteral("3"));
    QString plain;
    for (const QChar c : t) plain += c.unicode() < 128 ? c : QLatin1Char('?');
    return plain;
  };
  out << "0\nSECTION\n2\nENTITIES\n";
  for (const Line& l : sheet.lines) {
    const auto d = dashes(l.stroke);
    if (d.empty()) {
      line(layerOf(l.stroke), l.a, l.b);
      continue;
    }
    // Dashes drawn out as short lines, so every program shows them the same.
    const QPointF along = l.b - l.a;
    const double length = std::hypot(along.x(), along.y());
    if (length < 1e-9) continue;
    const QPointF unit = along / length;
    double at = 0;
    for (size_t i = 0; at < length; i = (i + 1) % d.size()) {
      const double next = std::min(length, at + d[i]);
      if (i % 2 == 0) line(layerOf(l.stroke), l.a + unit * at, l.a + unit * next);
      at = next;
    }
  }
  for (const Arrow& a : sheet.arrows) {
    QPointF c[3];
    arrowCorners(a, c);
    out << "0\nSOLID\n8\nDIMENSIONS\n";
    const QPointF corners[4] = {c[0], c[1], c[2], c[2]};
    for (int i = 0; i < 4; ++i) out << 10 + i << "\n" << corners[i].x() << "\n" << 20 + i << "\n" << h - corners[i].y() << "\n" << 30 + i << "\n0.0\n";
  }
  for (const Text& t : sheet.texts) {
    const int hAlign = (t.align & Qt::AlignHCenter) ? 1 : (t.align & Qt::AlignRight) ? 2 : 0;
    const int vAlign = (t.align & Qt::AlignVCenter) ? 2 : (t.align & Qt::AlignTop) ? 3 : 0;
    out << "0\nTEXT\n8\nTEXT\n10\n" << t.at.x() << "\n20\n" << h - t.at.y() << "\n30\n0.0\n40\n" << t.height << "\n1\n" << ascii(t.text)
        << "\n50\n" << t.angle << "\n";
    if (hAlign || vAlign)
      out << "72\n" << hAlign << "\n11\n" << t.at.x() << "\n21\n" << h - t.at.y() << "\n31\n0.0\n73\n" << vAlign << "\n";
  }
  out << "0\nENDSEC\n0\nEOF\n";
  out.flush();
  if (!file.commit()) {
    if (error) *error = file.errorString();
    return false;
  }
  return true;
}

bool writePdf(const Sheet& sheet, const QString& path, QString* error) { return writePdf(std::vector<Sheet>{sheet}, path, error); }

bool writePdf(const std::vector<Sheet>& sheets, const QString& path, QString* error) {
  if (sheets.empty()) return false;
  QPdfWriter writer(path);
  // Qt wants the paper upright and turned with the orientation.
  auto setPage = [&](const Sheet& sheet) {
    const QSizeF upright(std::min(sheet.size.width(), sheet.size.height()), std::max(sheet.size.width(), sheet.size.height()));
    writer.setPageSize(QPageSize(upright, QPageSize::Millimeter, QString(), QPageSize::ExactMatch));
    writer.setPageOrientation(sheet.size.width() > sheet.size.height() ? QPageLayout::Landscape : QPageLayout::Portrait);
    writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter);
  };
  setPage(sheets.front());
  writer.setResolution(1200);
  writer.setTitle(QStringLiteral("Technical drawing"));
  writer.setCreator(QStringLiteral("DaedalCAD"));
  QPainter painter;
  if (!painter.begin(&writer)) {
    if (error) *error = QStringLiteral("Could not write %1.").arg(path);
    return false;
  }
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (i > 0) {
      setPage(sheets[i]);
      writer.newPage();
    }
    painter.save();
    const double k = writer.width() / sheets[i].size.width();
    painter.scale(k, k);
    paint(painter, sheets[i]);
    painter.restore();
  }
  painter.end();
  return true;
}

bool writePng(const Sheet& sheet, const QString& path, int dpi, QString* error) {
  const double k = dpi / 25.4;
  QImage image(static_cast<int>(std::round(sheet.size.width() * k)), static_cast<int>(std::round(sheet.size.height() * k)), QImage::Format_RGB32);
  image.fill(Qt::white);
  image.setDotsPerMeterX(static_cast<int>(std::round(dpi / 0.0254)));
  image.setDotsPerMeterY(static_cast<int>(std::round(dpi / 0.0254)));
  QPainter painter(&image);
  painter.scale(k, k);
  paint(painter, sheet);
  painter.end();
  if (!image.save(path, "PNG")) {
    if (error) *error = QStringLiteral("Could not write %1.").arg(path);
    return false;
  }
  return true;
}

}  // namespace drawing
