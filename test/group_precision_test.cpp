// Grouping precision: parts that should touch the plate (or each other) but are a hair off.
// A hole whose bottom ends 0.00005 mm above the solid's bottom must still cut all the way through,
// with no paper-thin floor left behind, also inside a group that was moved and scaled.
#include <QGuiApplication>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "document.h"
#include "evaluator.h"

static int failures = 0;

static void expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) ++failures;
}

// Box primitives are 30 mm cubes centred on pos.
struct Case {
  double boxBottom;   // world z of the solid's bottom
  double holeBottom;  // world z of the hole's bottom
};

// Triangles lying on the plate inside the hole footprint: the leftover floor.
static int floorTriangles(const CpuMesh& m, double x0, double x1, double y0, double y1, double z) {
  int n = 0;
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    double c[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k)
      for (int i = 0; i < 3; ++i) c[i] += m.vertices[m.indices[t + k] * 3 + i] / 3.0;
    if (std::abs(c[2] - z) < 1e-2 && c[0] > x0 + 0.01 && c[0] < x1 - 0.01 && c[1] > y0 + 0.01 && c[1] < y1 - 0.01) ++n;
  }
  return n;
}

static void holeCase(const char* name, Case c, bool nest) {
  Document doc;
  const QString a = doc.addShape("box", 0, 0);
  const QString h = doc.addShape("box", 0, 0);
  Node* A = doc.find(a);
  Node* H = doc.find(h);
  A->pos[0] = 0; A->pos[1] = 0; A->pos[2] = c.boxBottom + 15;
  H->hole = true;
  H->pos[0] = 15; H->pos[1] = -15; H->pos[2] = c.holeBottom + 15;
  QString error;
  expect(doc.groupSelected({a, h}, &error), "group");
  if (nest) {
    // The group inside another group: the plate has to be found through both frames.
    QString g;
    for (const Node& n : doc.nodes) if (n.kind == Node::Kind::Group && n.parentId.isEmpty()) g = n.id;
    const QString other = doc.addShape("box", 200, 0);
    expect(doc.groupSelected({g, other}, &error), "nest group");
  }
  const CpuMesh m = evaluateSolid(doc, {}, &error);
  // Box [-15,15]^2 minus the hole's quarter [0,15] x [-15,0]. Both bottoms settle onto the plate;
  // the tops stay where they are, so a hole that ends lower leaves a (real) lid above it.
  const double boxTop = c.boxBottom + 30, holeTop = c.holeBottom + 30;
  const double want = 30.0 * 30 * boxTop - 15.0 * 15 * std::min(boxTop, holeTop) + (nest ? 27000.0 : 0.0);
  const int floor = floorTriangles(m, 0, 15, -15, 0, 0);
  char line[200];
  std::snprintf(line, sizeof line, "%s: volume %.6f (want %.6f), %d floor triangles", name, m.volume, want, floor);
  expect(std::abs(m.volume - want) < 1e-4 && floor == 0, line);
}

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  std::printf("hole and solid on the plate, grouped\n");
  holeCase("exact", {0, 0}, false);
  holeCase("hole 0.00005 up", {0, 5e-5}, false);
  holeCase("box 0.00005 up", {5e-5, 0}, false);
  holeCase("hole 0.0004 down", {0, -4e-4}, false);
  holeCase("float noise", {1e-12, -3e-13}, false);
  holeCase("nested, hole 0.00005 up", {0, 5e-5}, true);
  std::printf("%s\n", failures ? "group_precision: FAILED" : "group_precision: all passed");
  return failures ? 1 : 0;
}
