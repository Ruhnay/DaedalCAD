#include "stepexport.h"

#include <QDateTime>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>

namespace step {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct V {
  double x = 0, y = 0, z = 0;
};
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator*(V a, double k) { return {a.x * k, a.y * k, a.z * k}; }
double dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(V a) { return std::sqrt(dot(a, a)); }
V unit(V a) {
  const double l = length(a);
  return l > 0 ? a * (1 / l) : V{0, 0, 1};
}

// A STEP real always has a decimal point: 12. 0.5 1.E-05
QByteArray real(double v) {
  if (!(std::abs(v) > 1e-13)) return "0.";
  char buffer[40];
  std::snprintf(buffer, sizeof buffer, "%.12G", v);  // a nanometre on a metre-long part
  std::string s(buffer);
  const size_t e = s.find('E');
  std::string mantissa = e == std::string::npos ? s : s.substr(0, e);
  const std::string exponent = e == std::string::npos ? std::string() : s.substr(e);
  if (mantissa.find('.') == std::string::npos) mantissa += '.';
  return QByteArray::fromStdString(mantissa + exponent);
}

// A STEP string: quotes doubled, anything outside plain ASCII as \X2\hhhh\X0\.
QByteArray text(const QString& s) {
  QByteArray out = "'";
  for (const QChar c : s) {
    const ushort u = c.unicode();
    if (u == '\'') out += "''";
    else if (u == '\\') out += "\\\\";
    else if (u >= 32 && u < 127) out += static_cast<char>(u);
    else out += "\\X2\\" + QByteArray::number(u, 16).toUpper().rightJustified(4, '0') + "\\X0\\";
  }
  return out + "'";
}

struct Emitter {
  QByteArray data;
  long last = 0;
  long add(const QByteArray& body) {
    ++last;
    data += '#';
    data += QByteArray::number(static_cast<qlonglong>(last));
    data += '=';
    data += body;
    data += ";\n";
    return last;
  }
};

QByteArray ref(long id) { return "#" + QByteArray::number(static_cast<qlonglong>(id)); }
QByteArray refs(const std::vector<long>& ids) {
  QByteArray out = "(";
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i) out += ',';
    out += ref(ids[i]);
  }
  return out + ")";
}
QByteArray triple(V p) { return "(" + real(p.x) + "," + real(p.y) + "," + real(p.z) + ")"; }

// One object as a MANIFOLD_SOLID_BREP; 0 (and an error) if its mesh is not a closed solid.
}  // namespace

bool analyseSolid(const std::vector<double>& points, const std::vector<uint32_t>& triangles, Faceted* result, QString* error) {
  auto fail = [&](const QString& why) {
    if (error) *error = why;
    return false;
  };
  struct ObjectView {
    const std::vector<double>& points;
    const std::vector<uint32_t>& triangles;
  } object{points, triangles};
  const size_t rawCount = object.points.size() / 3;
  if (rawCount < 4 || object.triangles.size() < 12) return fail(QStringLiteral("no geometry."));

  // Points that are exactly the same are one vertex.
  std::vector<V> pts;
  std::vector<uint32_t> remap(rawCount);
  {
    std::map<std::array<double, 3>, uint32_t> seen;
    for (size_t i = 0; i < rawCount; ++i) {
      const std::array<double, 3> key = {object.points[3 * i], object.points[3 * i + 1], object.points[3 * i + 2]};
      const auto [it, inserted] = seen.emplace(key, static_cast<uint32_t>(pts.size()));
      if (inserted) pts.push_back({key[0], key[1], key[2]});
      remap[i] = it->second;
    }
  }
  std::vector<std::array<uint32_t, 3>> tris;
  tris.reserve(object.triangles.size() / 3);
  for (size_t i = 0; i + 2 < object.triangles.size(); i += 3) {
    if (object.triangles[i] >= rawCount || object.triangles[i + 1] >= rawCount || object.triangles[i + 2] >= rawCount)
      return fail(QStringLiteral("a triangle points past the vertices."));
    const std::array<uint32_t, 3> t = {remap[object.triangles[i]], remap[object.triangles[i + 1]], remap[object.triangles[i + 2]]};
    if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
    tris.push_back(t);
  }
  const size_t nt = tris.size();

  V lo = pts.front(), hi = pts.front();
  for (const V& p : pts) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const double diagonal = std::max(length(hi - lo), 1e-6);
  // How far from its face's plane a point may be. Faces that are flat by design come out of the
  // modelling kernel flat to far less than this; neighbouring facets of a round surface tilt by
  // far more.
  const double tol = std::clamp(diagonal * 1e-8, 1e-9, 1e-6);
  const double degenerate = 1e-12 * diagonal * diagonal;

  // Every edge is used by exactly two triangles, once each way: the mesh is closed.
  std::unordered_map<uint64_t, uint32_t> directed;
  directed.reserve(nt * 3);
  auto key = [](uint32_t a, uint32_t b) { return (static_cast<uint64_t>(a) << 32) | b; };
  for (size_t t = 0; t < nt; ++t)
    for (int k = 0; k < 3; ++k)
      if (!directed.emplace(key(tris[t][k], tris[t][(k + 1) % 3]), static_cast<uint32_t>(t)).second)
        return fail(QStringLiteral("the mesh is not a closed solid (an edge is used twice the same way)."));
  auto twin = [&](uint32_t a, uint32_t b) -> int64_t {
    const auto it = directed.find(key(b, a));
    return it == directed.end() ? -1 : static_cast<int64_t>(it->second);
  };
  for (size_t t = 0; t < nt; ++t)
    for (int k = 0; k < 3; ++k)
      if (twin(tris[t][k], tris[t][(k + 1) % 3]) < 0) return fail(QStringLiteral("the mesh is not a closed solid (it has an open edge)."));

  std::vector<V> normal(nt);
  std::vector<double> area2(nt);
  for (size_t t = 0; t < nt; ++t) {
    normal[t] = cross(pts[tris[t][1]] - pts[tris[t][0]], pts[tris[t][2]] - pts[tris[t][0]]);
    area2[t] = length(normal[t]);
  }

  // Faces: connected triangles in one plane, grown from the biggest triangle left over.
  struct Face {
    V n;
    double d = 0;
    std::vector<uint32_t> tris;
  };
  std::vector<Face> faces;
  std::vector<int> faceOf(nt, -1);
  std::vector<uint32_t> order(nt);
  for (size_t t = 0; t < nt; ++t) order[t] = static_cast<uint32_t>(t);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return area2[a] > area2[b]; });
  auto onPlane = [&](uint32_t t, V n, double d) {
    for (int k = 0; k < 3; ++k)
      if (std::abs(dot(n, pts[tris[t][k]]) - d) > tol) return false;
    return true;
  };
  for (uint32_t seed : order) {
    if (faceOf[seed] >= 0) continue;
    if (area2[seed] <= degenerate) {
      // A sliver with no area: it belongs to the neighbour whose plane it lies in.
      int best = -1;
      for (int k = 0; k < 3 && best < 0; ++k) {
        const int64_t u = twin(tris[seed][k], tris[seed][(k + 1) % 3]);
        if (u >= 0 && faceOf[u] >= 0) best = faceOf[u];
      }
      if (best >= 0) {
        faceOf[seed] = best;
        faces[static_cast<size_t>(best)].tris.push_back(seed);
        continue;
      }
    }
    const int id = static_cast<int>(faces.size());
    Face face;
    face.n = unit(normal[seed]);
    face.d = dot(face.n, pts[tris[seed][0]]);
    std::deque<uint32_t> queue{seed};
    faceOf[seed] = id;
    while (!queue.empty()) {
      const uint32_t t = queue.front();
      queue.pop_front();
      face.tris.push_back(t);
      for (int k = 0; k < 3; ++k) {
        const int64_t u = twin(tris[t][k], tris[t][(k + 1) % 3]);
        if (u < 0 || faceOf[u] >= 0) continue;
        if (area2[u] > degenerate && dot(unit(normal[u]), face.n) < 0.5) continue;
        if (!onPlane(static_cast<uint32_t>(u), face.n, face.d)) continue;
        faceOf[u] = id;
        queue.push_back(static_cast<uint32_t>(u));
      }
    }
    faces.push_back(std::move(face));
  }
  // Each face's plane from all of its triangles.
  for (Face& face : faces) {
    V sum;
    V centre;
    double weight = 0;
    for (uint32_t t : face.tris) {
      sum = sum + normal[t];
      centre = centre + (pts[tris[t][0]] + pts[tris[t][1]] + pts[tris[t][2]]) * (area2[t] / 3);
      weight += area2[t];
    }
    if (length(sum) > 0) face.n = unit(sum);
    if (weight > 0) face.d = dot(face.n, centre * (1 / weight));
  }

  // The edges between faces, each way round, with the face on its other side.
  struct Boundary {
    uint32_t a, b;
    int other;
  };
  std::vector<std::vector<Boundary>> boundary(faces.size());
  // For the corners: the boundary edges at each vertex, and the pair of faces along each.
  struct Link {
    uint32_t to;
    uint64_t pair;
  };
  std::vector<std::vector<Link>> links(pts.size());
  auto pairOf = [](int f, int g) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(std::min(f, g))) << 32) | static_cast<uint32_t>(std::max(f, g));
  };
  for (size_t f = 0; f < faces.size(); ++f)
    for (uint32_t t : faces[f].tris)
      for (int k = 0; k < 3; ++k) {
        const uint32_t a = tris[t][k], b = tris[t][(k + 1) % 3];
        const int g = faceOf[static_cast<size_t>(twin(a, b))];
        if (g == static_cast<int>(f)) continue;
        boundary[f].push_back({a, b, g});
        if (a < b) {
          links[a].push_back({b, pairOf(static_cast<int>(f), g)});
          links[b].push_back({a, pairOf(static_cast<int>(f), g)});
        }
      }
  // A corner ends an edge of the STEP file: where more than two face edges meet, where the faces
  // on either side change, or where the line bends. Points in the middle of a straight edge
  // between the same two faces (left by the triangles) drop out.
  std::vector<char> corner(pts.size(), 1);
  for (size_t v = 0; v < pts.size(); ++v) {
    const auto& l = links[v];
    if (l.size() != 2 || l[0].pair != l[1].pair) continue;
    const V p = pts[l[0].to], q = pts[v], r = pts[l[1].to];
    const V along = r - p;
    const double span = length(along);
    if (span <= 0) continue;
    const double off = length(cross(q - p, along)) / span;
    if (off <= tol && dot(q - p, r - q) > 0) corner[v] = 0;
  }

  // The result: corners, straight edges between them (each used by two faces, once each way) and
  // planar faces bounded by loops of those edges.
  Faceted& out = *result;
  out = Faceted();
  out.triangles = static_cast<int>(nt);
  std::unordered_map<uint32_t, int> cornerIndex;
  auto cornerOf = [&](uint32_t v) {
    const auto it = cornerIndex.find(v);
    if (it != cornerIndex.end()) return it->second;
    out.points.push_back({pts[v].x, pts[v].y, pts[v].z});
    return cornerIndex.emplace(v, static_cast<int>(out.points.size() - 1)).first->second;
  };
  std::map<std::tuple<uint32_t, uint32_t, uint64_t>, int> edges;
  auto edgeFor = [&](uint32_t a, uint32_t b, uint64_t pair) -> std::pair<int, bool> {
    const auto k = std::make_tuple(std::min(a, b), std::max(a, b), pair);
    const auto it = edges.find(k);
    if (it != edges.end()) return {it->second, out.edges[static_cast<size_t>(it->second)].a == cornerOf(a)};
    out.edges.push_back({cornerOf(a), cornerOf(b)});
    const int id = static_cast<int>(out.edges.size() - 1);
    edges.emplace(k, id);
    return {id, true};
  };

  for (size_t f = 0; f < faces.size(); ++f) {
    const Face& face = faces[f];
    const auto& be = boundary[f];
    if (be.empty()) continue;
    std::unordered_map<uint32_t, std::vector<size_t>> outgoing;
    for (size_t i = 0; i < be.size(); ++i) outgoing[be[i].a].push_back(i);
    // Directions in the face's plane, to pick the way on where a face touches itself at a point.
    const V u = unit(std::abs(face.n.x) < 0.9 ? cross(face.n, V{1, 0, 0}) : cross(face.n, V{0, 1, 0}));
    const V w = cross(face.n, u);
    auto angle = [&](V d) { return std::atan2(dot(d, w), dot(d, u)); };
    std::vector<char> used(be.size(), 0);
    struct Loop {
      std::vector<uint32_t> verts;
      std::vector<int> others;
      double area = 0;
    };
    std::vector<Loop> loops;
    for (size_t first = 0; first < be.size(); ++first) {
      if (used[first]) continue;
      Loop loop;
      size_t i = first;
      const uint32_t start = be[first].a;
      for (size_t guard = 0; guard <= be.size(); ++guard) {
        used[i] = 1;
        loop.verts.push_back(be[i].a);
        loop.others.push_back(be[i].other);
        const uint32_t v = be[i].b;
        if (v == start) break;
        size_t next = be.size();
        double best = 1e9;
        const V back = pts[be[i].a] - pts[v];
        for (size_t c : outgoing[v]) {
          if (used[c]) continue;
          // The first edge turning clockwise from the way we came: the one that keeps the face
          // on our left.
          double turn = angle(back) - angle(pts[be[c].b] - pts[v]);
          while (turn <= 1e-12) turn += 2 * kPi;
          if (turn < best) {
            best = turn;
            next = c;
          }
        }
        if (next == be.size()) return fail(QStringLiteral("a face outline does not close."));
        i = next;
      }
      if (be[i].b != start) return fail(QStringLiteral("a face outline does not close."));
      V area;
      for (size_t k = 0; k < loop.verts.size(); ++k)
        area = area + cross(pts[loop.verts[k]], pts[loop.verts[(k + 1) % loop.verts.size()]]);
      loop.area = 0.5 * dot(area, face.n);
      loops.push_back(std::move(loop));
    }
    size_t outer = 0;
    for (size_t k = 1; k < loops.size(); ++k)
      if (loops[k].area > loops[outer].area) outer = k;
    if (outer != 0) std::swap(loops[0], loops[outer]);  // the outer bound first

    Faceted::Face result;
    for (const Loop& loop : loops) {
      const size_t n = loop.verts.size();
      size_t from = n;
      for (size_t j = 0; j < n; ++j)
        if (corner[loop.verts[j]]) {
          from = j;
          break;
        }
      if (from == n) return fail(QStringLiteral("a face outline has no corners."));
      Faceted::Bound bound;
      size_t j = from;
      do {
        const uint32_t a = loop.verts[j];
        const uint64_t pair = pairOf(static_cast<int>(f), loop.others[j]);
        size_t k2 = (j + 1) % n;
        while (!corner[loop.verts[k2]]) k2 = (k2 + 1) % n;
        const uint32_t b = loop.verts[k2];
        if (a == b) return fail(QStringLiteral("an edge starts and ends at one point."));
        bound.edges.push_back(edgeFor(a, b, pair));
        j = k2;
      } while (j != from);
      result.bounds.push_back(std::move(bound));
    }
    // The plane through the face: its normal points out of the solid.
    const V origin = pts[loops[0].verts.front()];
    const V onPlane = origin - face.n * (dot(face.n, origin) - face.d);
    result.origin = {onPlane.x, onPlane.y, onPlane.z};
    result.normal = {face.n.x, face.n.y, face.n.z};
    result.u = {u.x, u.y, u.z};
    out.faces.push_back(std::move(result));
  }
  return true;
}

namespace {

// One object as a MANIFOLD_SOLID_BREP; 0 (and an error) if its mesh is not a closed solid.
long writeSolid(Emitter& out, const ExportObject& object, Stats& stats, QString* error) {
  Faceted model;
  QString why;
  if (!analyseSolid(object.points, object.triangles, &model, &why)) {
    if (error) *error = QStringLiteral("%1: %2").arg(object.name, why);
    return 0;
  }
  auto v3 = [](const std::array<double, 3>& a) { return V{a[0], a[1], a[2]}; };
  // Points and vertices once each, edges once each (the second face uses them reversed).
  std::vector<std::pair<long, long>> vertexIds(model.points.size(), {0, 0});  // cartesian point, vertex point
  auto vertex = [&](int v) {
    auto& ids = vertexIds[static_cast<size_t>(v)];
    if (ids.second) return ids;
    ids.first = out.add("CARTESIAN_POINT(''," + triple(v3(model.points[static_cast<size_t>(v)])) + ")");
    ids.second = out.add("VERTEX_POINT(''," + ref(ids.first) + ")");
    ++stats.vertices;
    return ids;
  };
  std::vector<long> curves(model.edges.size(), 0);
  for (size_t e = 0; e < model.edges.size(); ++e) {
    const auto va = vertex(model.edges[e].a), vb = vertex(model.edges[e].b);
    const V dir = v3(model.points[static_cast<size_t>(model.edges[e].b)]) - v3(model.points[static_cast<size_t>(model.edges[e].a)]);
    const long direction = out.add("DIRECTION(''," + triple(unit(dir)) + ")");
    const long vector = out.add("VECTOR(''," + ref(direction) + "," + real(length(dir)) + ")");
    const long line = out.add("LINE(''," + ref(va.first) + "," + ref(vector) + ")");
    curves[e] = out.add("EDGE_CURVE(''," + ref(va.second) + "," + ref(vb.second) + "," + ref(line) + ",.T.)");
    ++stats.edges;
  }
  std::vector<long> faceIds;
  for (const Faceted::Face& face : model.faces) {
    std::vector<long> bounds;
    for (size_t k = 0; k < face.bounds.size(); ++k) {
      std::vector<long> oriented;
      for (const auto& [edge, forward] : face.bounds[k].edges)
        oriented.push_back(out.add("ORIENTED_EDGE('',*,*," + ref(curves[static_cast<size_t>(edge)]) + (forward ? ",.T.)" : ",.F.)")));
      const long edgeLoop = out.add("EDGE_LOOP(''," + refs(oriented) + ")");
      bounds.push_back(out.add(QByteArray(k == 0 ? "FACE_OUTER_BOUND" : "FACE_BOUND") + "(''," + ref(edgeLoop) + ",.T.)"));
    }
    const long location = out.add("CARTESIAN_POINT(''," + triple(v3(face.origin)) + ")");
    const long axis = out.add("DIRECTION(''," + triple(v3(face.normal)) + ")");
    const long refDir = out.add("DIRECTION(''," + triple(v3(face.u)) + ")");
    const long placement = out.add("AXIS2_PLACEMENT_3D(''," + ref(location) + "," + ref(axis) + "," + ref(refDir) + ")");
    const long plane = out.add("PLANE(''," + ref(placement) + ")");
    faceIds.push_back(out.add("ADVANCED_FACE(''," + refs(bounds) + "," + ref(plane) + ",.T.)"));
    ++stats.faces;
  }
  const long shell = out.add("CLOSED_SHELL(''," + refs(faceIds) + ")");
  ++stats.solids;
  stats.triangles += model.triangles;
  return out.add("MANIFOLD_SOLID_BREP(" + text(object.name) + "," + ref(shell) + ")");
}

}  // namespace

QByteArray write(const std::vector<ExportObject>& objects, const QString& fileName, const QString& timestamp, Stats* statsOut,
                 QString* error) {
  Stats stats;
  Emitter out;
  if (objects.empty()) {
    if (error) *error = QStringLiteral("Nothing to export.");
    return {};
  }
  const QString productName = objects.size() == 1 ? objects.front().name : QFileInfo(fileName).completeBaseName();
  // Application, product and units (millimetres), the frame of AP214.
  const long application = out.add("APPLICATION_CONTEXT('core data for automotive mechanical design processes')");
  out.add("APPLICATION_PROTOCOL_DEFINITION('international standard','automotive_design',2000," + ref(application) + ")");
  const long productContext = out.add("PRODUCT_CONTEXT(''," + ref(application) + ",'mechanical')");
  const long definitionContext = out.add("PRODUCT_DEFINITION_CONTEXT('part definition'," + ref(application) + ",'design')");
  const long product = out.add("PRODUCT(" + text(productName) + "," + text(productName) + ",''," + "(" + ref(productContext) + "))");
  out.add("PRODUCT_RELATED_PRODUCT_CATEGORY('part',$,(" + ref(product) + "))");
  const long formation = out.add("PRODUCT_DEFINITION_FORMATION('',''," + ref(product) + ")");
  const long definition = out.add("PRODUCT_DEFINITION('design',''," + ref(formation) + "," + ref(definitionContext) + ")");
  const long shapeDefinition = out.add("PRODUCT_DEFINITION_SHAPE('',''," + ref(definition) + ")");
  const long mm = out.add("(LENGTH_UNIT()NAMED_UNIT(*)SI_UNIT(.MILLI.,.METRE.))");
  const long radian = out.add("(NAMED_UNIT(*)PLANE_ANGLE_UNIT()SI_UNIT($,.RADIAN.))");
  const long steradian = out.add("(NAMED_UNIT(*)SI_UNIT($,.STERADIAN.)SOLID_ANGLE_UNIT())");
  const long uncertainty =
      out.add("UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-05)," + ref(mm) + ",'distance_accuracy_value','confusion accuracy')");
  const long context = out.add("(GEOMETRIC_REPRESENTATION_CONTEXT(3)GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT((" + ref(uncertainty) +
                               "))GLOBAL_UNIT_ASSIGNED_CONTEXT((" + ref(mm) + "," + ref(radian) + "," + ref(steradian) +
                               "))REPRESENTATION_CONTEXT('Context3D','3D Context with UNIT and UNCERTAINTY'))");
  const long originPoint = out.add("CARTESIAN_POINT('',(0.,0.,0.))");
  const long zAxis = out.add("DIRECTION('',(0.,0.,1.))");
  const long xAxis = out.add("DIRECTION('',(1.,0.,0.))");
  const long world = out.add("AXIS2_PLACEMENT_3D(''," + ref(originPoint) + "," + ref(zAxis) + "," + ref(xAxis) + ")");

  std::vector<long> items{world};
  for (const ExportObject& object : objects) {
    const long solid = writeSolid(out, object, stats, error);
    if (!solid) return {};
    items.push_back(solid);
  }
  const long representation = out.add("ADVANCED_BREP_SHAPE_REPRESENTATION(" + text(productName) + "," + refs(items) + "," + ref(context) + ")");
  out.add("SHAPE_DEFINITION_REPRESENTATION(" + ref(shapeDefinition) + "," + ref(representation) + ")");

  QByteArray file = "ISO-10303-21;\nHEADER;\n";
  file += "FILE_DESCRIPTION(('DaedalCAD model, faceted'),'2;1');\n";
  file += "FILE_NAME(" + text(QFileInfo(fileName).fileName()) + "," + text(timestamp) + ",(''),(''),'DaedalCAD','DaedalCAD','');\n";
  file += "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\nENDSEC;\nDATA;\n";
  file += out.data;
  file += "ENDSEC;\nEND-ISO-10303-21;\n";
  if (statsOut) *statsOut = stats;
  return file;
}

}  // namespace step

bool writeStep(const std::vector<ExportObject>& objects, const QString& path, QString* error, step::Stats* stats) {
  const QByteArray data =
      step::write(objects, path, QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")), stats, error);
  if (data.isEmpty()) return false;
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    if (error) *error = file.errorString();
    return false;
  }
  file.write(data);
  if (!file.commit()) {
    if (error) *error = file.errorString();
    return false;
  }
  return true;
}
