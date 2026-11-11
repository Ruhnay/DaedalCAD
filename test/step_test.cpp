// STEP export: the file is read back with a small STEP parser and checked the way a CAD program
// would see it: every reference resolves, every face is flat with its corners on its plane, its
// edge loops close, every edge of a solid is used by exactly two faces in opposite directions
// (so the shell is closed), and the volume worked out from the STEP faces alone matches the part.

#include "document.h"
#include "evaluator.h"
#include "stepexport.h"

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

// --- A small reader for what the exporter writes ------------------------------------------------

struct Arg {
  enum Kind { Ref, Real, Str, Enum, List, Star, Dollar, Typed } kind = Dollar;
  long ref = 0;
  double real = 0;
  std::string text;  // string, enum or the type of a typed value
  std::vector<Arg> list;
  bool integer = false;  // a number written without a decimal point
};

struct Entity {
  std::string type;  // empty for a complex entity
  std::vector<Arg> args;
};

struct Parser {
  const std::string& s;
  size_t i = 0;
  bool ok = true;
  void space() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r')) ++i;
  }
  Arg value() {
    space();
    Arg a;
    if (i >= s.size()) {
      ok = false;
      return a;
    }
    const char c = s[i];
    if (c == '#') {
      ++i;
      a.kind = Arg::Ref;
      a.ref = std::strtol(s.c_str() + i, nullptr, 10);
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    } else if (c == '\'') {
      ++i;
      a.kind = Arg::Str;
      while (i < s.size()) {
        if (s[i] == '\'') {
          if (i + 1 < s.size() && s[i + 1] == '\'') {
            a.text += '\'';
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        a.text += s[i++];
      }
    } else if (c == '.') {
      ++i;
      a.kind = Arg::Enum;
      while (i < s.size() && s[i] != '.') a.text += s[i++];
      ++i;
    } else if (c == '(') {
      ++i;
      a.kind = Arg::List;
      space();
      if (s[i] == ')') {
        ++i;
        return a;
      }
      while (ok) {
        a.list.push_back(value());
        space();
        if (s[i] == ',') {
          ++i;
          continue;
        }
        if (s[i] == ')') {
          ++i;
          break;
        }
        // A complex entity lists its parts with nothing between them: (A()B(*)C(.X.))
        if (std::isalpha(static_cast<unsigned char>(s[i]))) continue;
        ok = false;
      }
    } else if (c == '*') {
      ++i;
      a.kind = Arg::Star;
    } else if (c == '$') {
      ++i;
      a.kind = Arg::Dollar;
    } else if (std::isalpha(static_cast<unsigned char>(c))) {
      a.kind = Arg::Typed;
      while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) a.text += s[i++];
      Arg inner = value();
      a.list = inner.list;
    } else {
      // A real must have a decimal point.
      a.kind = Arg::Real;
      size_t start = i;
      while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == '-' || s[i] == '+' || s[i] == 'E'))
        ++i;
      const std::string number = s.substr(start, i - start);
      a.integer = number.find('.') == std::string::npos;
      a.real = std::strtod(number.c_str(), nullptr);
    }
    return a;
  }
};

struct StepFile {
  std::map<long, Entity> entities;
  bool ok = false;
  std::string header;
};

StepFile parse(const QByteArray& data) {
  StepFile file;
  const std::string all = data.toStdString();
  const size_t dataAt = all.find("DATA;\n");
  const size_t endAt = all.find("ENDSEC;", dataAt);
  if (all.rfind("ISO-10303-21;", 0) != 0 || dataAt == std::string::npos || endAt == std::string::npos ||
      all.find("END-ISO-10303-21;") == std::string::npos)
    return file;
  file.header = all.substr(0, dataAt);
  size_t at = dataAt + 6;
  file.ok = true;
  while (at < endAt) {
    const size_t end = all.find(";\n", at);
    if (end == std::string::npos || end > endAt) break;
    const std::string line = all.substr(at, end - at);
    at = end + 2;
    const size_t eq = line.find('=');
    if (line.empty() || line[0] != '#' || eq == std::string::npos) {
      if (file.ok) std::printf("  not an entity: %s\n", line.c_str());
      file.ok = false;
      continue;
    }
    const long id = std::strtol(line.c_str() + 1, nullptr, 10);
    Entity e;
    std::string body = line.substr(eq + 1);
    size_t paren = body.find('(');
    if (paren != 0) e.type = body.substr(0, paren);
    Parser p{body, paren};
    Arg list = p.value();
    if (!p.ok) {
      if (file.ok) std::printf("  cannot read: %s\n", line.c_str());
      file.ok = false;
    }
    e.args = list.list;
    if (!file.entities.emplace(id, e).second) file.ok = false;
  }
  return file;
}

struct Checked {
  int solids = 0, faces = 0, edges = 0, problems = 0;
  double volume = 0;
  std::vector<std::string> names;
};

struct Vec {
  double x = 0, y = 0, z = 0;
};
Vec sub(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec crossv(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dotv(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Walks every solid of the file the way a reader builds its shapes.
Checked verify(const StepFile& file, const std::string& label) {
  Checked out;
  auto problem = [&](const std::string& what) {
    if (out.problems++ < 5) std::printf("  %s: %s\n", label.c_str(), what.c_str());
  };
  auto get = [&](long id, const char* type) -> const Entity* {
    const auto it = file.entities.find(id);
    if (it == file.entities.end()) {
      problem("#" + std::to_string(id) + " is missing");
      return nullptr;
    }
    if (type && it->second.type != type) {
      problem("#" + std::to_string(id) + " is " + it->second.type + ", not " + type);
      return nullptr;
    }
    return &it->second;
  };
  // Every reference anywhere resolves.
  std::function<void(const Arg&)> refsResolve = [&](const Arg& a) {
    if (a.kind == Arg::Ref && !file.entities.count(a.ref)) problem("dangling #" + std::to_string(a.ref));
    for (const Arg& b : a.list) refsResolve(b);
  };
  for (const auto& [id, e] : file.entities)
    for (const Arg& a : e.args) refsResolve(a);

  auto point = [&](long id) -> Vec {
    const Entity* p = get(id, "CARTESIAN_POINT");
    if (!p || p->args.size() < 2 || p->args[1].list.size() != 3) return {};
    for (const Arg& c : p->args[1].list)
      if (c.integer) problem("a coordinate without a decimal point");
    return {p->args[1].list[0].real, p->args[1].list[1].real, p->args[1].list[2].real};
  };
  auto direction = [&](long id) -> Vec {
    const Entity* d = get(id, "DIRECTION");
    if (!d || d->args.size() < 2 || d->args[1].list.size() != 3) return {};
    return {d->args[1].list[0].real, d->args[1].list[1].real, d->args[1].list[2].real};
  };
  auto vertexPoint = [&](long id) -> Vec {
    const Entity* v = get(id, "VERTEX_POINT");
    return v ? point(v->args[1].ref) : Vec{};
  };

  double scaleRef = 1;
  for (const auto& [id, e] : file.entities)
    if (e.type == "CARTESIAN_POINT")
      for (const Arg& c : e.args[1].list) scaleRef = std::max(scaleRef, std::abs(c.real));
  const double tolerance = 1e-5;

  for (const auto& [id, e] : file.entities) {
    if (e.type != "MANIFOLD_SOLID_BREP") continue;
    ++out.solids;
    out.names.push_back(e.args[0].text);
    const Entity* shell = get(e.args[1].ref, "CLOSED_SHELL");
    if (!shell) continue;
    std::map<long, std::pair<int, int>> uses;  // edge curve: forward and backward uses
    for (const Arg& faceRef : shell->args[1].list) {
      const Entity* face = get(faceRef.ref, "ADVANCED_FACE");
      if (!face) continue;
      ++out.faces;
      const Entity* plane = get(face->args[2].ref, "PLANE");
      if (!plane) continue;
      const Entity* placement = get(plane->args[1].ref, "AXIS2_PLACEMENT_3D");
      if (!placement) continue;
      const Vec origin = point(placement->args[1].ref);
      const Vec normal = direction(placement->args[2].ref);
      if (std::abs(dotv(normal, normal) - 1) > 1e-9) problem("a plane's normal is not a unit vector");
      if (std::abs(dotv(normal, direction(placement->args[3].ref))) > 1e-9) problem("a plane's axes are not square");
      if (face->args[3].text != "T") problem("a face against its plane");
      int outerBounds = 0;
      Vec faceArea;
      for (const Arg& boundRef : face->args[1].list) {
        const auto it = file.entities.find(boundRef.ref);
        if (it == file.entities.end()) continue;
        const Entity& bound = it->second;
        if (bound.type == "FACE_OUTER_BOUND") ++outerBounds;
        else if (bound.type != "FACE_BOUND") problem("a bound of type " + bound.type);
        const Entity* loop = get(bound.args[1].ref, "EDGE_LOOP");
        if (!loop) continue;
        std::vector<Vec> corners;
        Vec previousEnd;
        bool firstEdge = true;
        Vec loopStart;
        for (const Arg& orientedRef : loop->args[1].list) {
          const Entity* oriented = get(orientedRef.ref, "ORIENTED_EDGE");
          if (!oriented) continue;
          const Entity* curve = get(oriented->args[3].ref, "EDGE_CURVE");
          if (!curve) continue;
          const bool forward = oriented->args[4].text == "T";
          auto& u = uses[oriented->args[3].ref];
          (forward ? u.first : u.second)++;
          Vec a = vertexPoint(curve->args[1].ref), b = vertexPoint(curve->args[2].ref);
          // The line runs through the edge's ends.
          const Entity* line = get(curve->args[3].ref, "LINE");
          if (line) {
            const Vec p = point(line->args[1].ref);
            const Entity* vector = get(line->args[2].ref, "VECTOR");
            const Vec d = vector ? direction(vector->args[1].ref) : Vec{};
            const Vec ab = sub(b, a);
            const double along = std::sqrt(dotv(ab, ab));
            const Vec off = crossv(sub(b, p), d);
            if (std::sqrt(dotv(off, off)) > tolerance || std::abs(dotv(sub(a, p), sub(a, p))) > tolerance * tolerance ||
                (vector && std::abs(vector->args[2].real - along) > tolerance))
              problem("an edge does not lie on its line");
          }
          if (!forward) std::swap(a, b);
          if (firstEdge) loopStart = a;
          else if (std::sqrt(dotv(sub(a, previousEnd), sub(a, previousEnd))) > 1e-12) problem("an edge loop is broken");
          firstEdge = false;
          previousEnd = b;
          corners.push_back(a);
          if (std::abs(dotv(sub(a, origin), normal)) > tolerance) problem("a corner is off its face's plane");
        }
        if (!firstEdge && std::sqrt(dotv(sub(loopStart, previousEnd), sub(loopStart, previousEnd))) > 1e-12)
          problem("an edge loop does not close");
        if (corners.size() < 3) problem("a loop with fewer than three corners");
        for (size_t k = 0; k < corners.size(); ++k) {
          const Vec c = crossv(corners[k], corners[(k + 1) % corners.size()]);
          faceArea = {faceArea.x + c.x * 0.5, faceArea.y + c.y * 0.5, faceArea.z + c.z * 0.5};
        }
      }
      if (outerBounds != 1) problem("a face with " + std::to_string(outerBounds) + " outer bounds");
      if (dotv(faceArea, normal) <= 0) problem("a face whose outline runs the wrong way round");
      // Divergence theorem: the volume from the faces alone.
      out.volume += dotv(origin, faceArea) / 3;
    }
    for (const auto& [curve, u] : uses) {
      ++out.edges;
      if (u.first != 1 || u.second != 1) problem("edge #" + std::to_string(curve) + " used " + std::to_string(u.first) + "+" +
                                                 std::to_string(u.second) + " times: the shell is not closed");
    }
  }
  return out;
}

// --- The models -----------------------------------------------------------------------------------

struct Case {
  std::string label;
  Document doc;
};

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir dir;
  const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : dir.path();
  std::vector<Case> cases;

  {
    // A block with a hole through it and a pocket: faces with holes, a round hole as facets.
    Case c{"block with holes", {}};
    const QString block = c.doc.addShape("box", 0, 0);
    Node* b = c.doc.find(block);
    b->size[0] = 40;
    b->size[1] = 30;
    b->size[2] = 10;
    b->pos[2] = 5;
    b->name = "Block";
    const QString hole = c.doc.addShape("cylinder", 0, 0);
    Node* h = c.doc.find(hole);
    h->hole = true;
    h->radius = 4;
    h->size[2] = 20;
    h->pos[2] = 5;
    const QString pocket = c.doc.addShape("box", 10, 5);
    Node* p = c.doc.find(pocket);
    p->hole = true;
    p->size[0] = 8;
    p->size[1] = 8;
    p->size[2] = 6;
    p->pos[2] = 10;
    c.doc.groupSelected({block, hole, pocket}, nullptr);
    c.doc.nodes.back().name = "Block with holes";
    cases.push_back(std::move(c));
  }
  {
    // An L of two boxes merged: straight edges that the triangles split end up as one edge.
    Case c{"L-shape", {}};
    const QString a = c.doc.addShape("box", 0, 0);
    const QString b = c.doc.addShape("box", 25, 0);
    c.doc.find(a)->size[0] = 30;
    c.doc.find(b)->size[0] = 20;
    c.doc.find(b)->size[1] = 60;
    c.doc.groupSelected({a, b}, nullptr);
    c.doc.nodes.back().name = "L";
    cases.push_back(std::move(c));
  }
  {
    // A sphere, a rotated and bent bar, and a cone: many small faces, slanted planes.
    Case c{"sphere, bent bar, cone", {}};
    c.doc.addShape("sphere", 0, 0);
    const QString bar = c.doc.addShape("box", 60, 0);
    Node* b = c.doc.find(bar);
    b->size[0] = 60;
    b->size[1] = 8;
    b->size[2] = 8;
    b->rot[2] = 30;
    b->bend = 90;
    c.doc.addShape("cone", -60, 0);
    cases.push_back(std::move(c));
  }
  {
    // A generator: an ISO socket head screw with a modelled thread, and a gear.
    Case c{"screw and gear", {}};
    const QString screw = c.doc.addShape("generator", 0, 0);
    Node* s = c.doc.find(screw);
    s->genType = "stdscrew";
    s->genParams = QJsonObject{{"standard", "4762"}, {"size", "M5"}, {"length", 16}, {"thread", "modelled"}};
    s->name = QStringLiteral("Screw M5×16 'socket'");
    const QString gear = c.doc.addShape("generator", 60, 0);
    c.doc.find(gear)->genType = "gear";
    c.doc.find(gear)->genParams = QJsonObject{};
    cases.push_back(std::move(c));
  }

  for (Case& c : cases) {
    QString error;
    const auto objects = evaluateObjects(c.doc, {}, &error);
    check(!objects.empty(), c.label + ": something to export");
    double volume = 0;
    int triangles = 0;
    for (const auto& o : objects) {
      volume += o.mesh.volume;
      triangles += o.mesh.triangles;
      check(o.points.size() / 3 > 3 && o.triangles.size() == static_cast<size_t>(o.mesh.triangles) * 3, c.label + ": full-precision mesh kept");
    }
    const auto start = std::chrono::steady_clock::now();
    step::Stats stats;
    const QString path = out + "/" + QString::fromStdString(c.label).replace(' ', '_').replace(',', "") + ".step";
    const bool written = writeStep(objects, path, &error, &stats);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    check(written, c.label + ": written (" + error.toStdString() + ")");
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    const QByteArray data = f.readAll();
    const StepFile file = parse(data);
    check(file.ok, c.label + ": the file parses");
    const Checked result = verify(file, c.label);
    std::printf("%-24s %zu object(s), %6d triangles -> %5d faces, %5d edges, %5d vertices, %7.1f KB, %6.1f ms; volume %.3f (mesh %.3f)\n",
                c.label.c_str(), objects.size(), triangles, stats.faces, stats.edges, stats.vertices, data.size() / 1024.0, ms,
                result.volume, volume);
    check(result.problems == 0, c.label + ": " + std::to_string(result.problems) + " problem(s) in the file");
    check(result.solids == static_cast<int>(objects.size()), c.label + ": one solid per object");
    check(result.faces == stats.faces && result.edges == stats.edges, c.label + ": every face and edge reached from the solids");
    check(std::abs(result.volume - volume) < 1e-6 * std::max(1.0, volume), c.label + ": the faces enclose the part's volume");
    check(stats.faces < triangles, c.label + ": flat triangles joined into faces");
    check(file.header.find("AUTOMOTIVE_DESIGN") != std::string::npos && data.contains("SI_UNIT(.MILLI.,.METRE.)"), c.label + ": AP214, in mm");
    if (c.label == "block with holes") {
      // 6 sides + 1 pocket floor + 4 pocket walls, plus the round hole's facets.
      int bounds = 0;
      for (const auto& [id, e] : file.entities) bounds += e.type == "FACE_BOUND";
      check(bounds == 3 && stats.faces > 11, "block: the top face holds the hole and the pocket, the bottom the hole (" +
                                                  std::to_string(bounds) + " inner bounds)");
    }
    if (c.label == "L-shape")
      check(stats.faces == 10 && stats.edges == 24 && stats.vertices == 16,
            "L: an eight-sided prism, 10 faces, 24 edges, 16 corners (" + std::to_string(stats.faces) + ", " + std::to_string(stats.edges) +
                ", " + std::to_string(stats.vertices) + ")");
    if (c.label == "screw and gear")
      check(data.contains("MANIFOLD_SOLID_BREP('Screw M5\\X2\\00D7\\X0\\16 ''socket'''"),
            "names: quotes doubled, × encoded");
  }

  std::printf(failures ? "%d failure(s)\n" : "all STEP checks passed\n", failures);
  return failures ? 1 : 0;
}
