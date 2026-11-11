// Export: OBJ, 3MF and DXF keep every part as its own object, loose holes on the bed still cut them,
// the DXF files are well-formed R12, and slices of flat faces keep the bed's X and Y.

#include "document.h"
#include "evaluator.h"
#include "exporter.h"

#include <QApplication>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>

#include <array>
#include <cstring>
#include <map>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

QList<QPair<int, QByteArray>> dxfPairs(const QString& path) {
  QFile file(path);
  QList<QPair<int, QByteArray>> pairs;
  if (!file.open(QIODevice::ReadOnly)) return pairs;
  const QList<QByteArray> lines = file.readAll().split('\n');
  for (int i = 0; i + 1 < lines.size(); i += 2) pairs.append({lines[i].trimmed().toInt(), lines[i + 1].trimmed()});
  return pairs;
}

// What an R12 reader insists on: version, declared layers with legal names, closed POLYLINE / VERTEX
// / SEQEND runs, nothing from later versions, and EOF at the end.
void checkDxf(const QString& path, const std::string& label, QSet<QByteArray>* layersUsed, int* polylines, int* faces) {
  const auto pairs = dxfPairs(path);
  check(!pairs.isEmpty() && pairs.last().second == "EOF", label + ": ends with EOF");
  bool version = false;
  QSet<QByteArray> declared;
  QSet<QByteArray> used;
  bool inLayerTable = false;
  int open = 0;
  for (int i = 0; i < pairs.size(); ++i) {
    const auto& [code, value] = pairs[i];
    if (code == 9 && value == "$ACADVER" && i + 1 < pairs.size()) version = pairs[i + 1].second == "AC1009";
    if (code == 0 && value == "TABLE" && i + 1 < pairs.size()) inLayerTable = pairs[i + 1].second == "LAYER";
    if (code == 0 && value == "ENDTAB") inLayerTable = false;
    if (inLayerTable && code == 0 && value == "LAYER" && i + 1 < pairs.size() && pairs[i + 1].first == 2)
      declared.insert(pairs[i + 1].second);
    if (code == 8) used.insert(value);
    if (code == 0) {
      check(value != "LWPOLYLINE", label + ": no R2000 entities");
      if (value == "POLYLINE") {
        check(open == 0, label + ": POLYLINE inside a POLYLINE");
        open = 1;
        ++*polylines;
      } else if (value == "VERTEX") {
        check(open == 1, label + ": VERTEX outside a POLYLINE");
      } else if (value == "SEQEND") {
        check(open == 1, label + ": SEQEND without POLYLINE");
        open = 0;
      } else if (value == "3DFACE") {
        ++*faces;
      }
    }
  }
  check(version, label + ": $ACADVER AC1009");
  check(open == 0, label + ": every POLYLINE ends with SEQEND");
  const QRegularExpression legal(QStringLiteral("^[A-Z0-9$_-]{1,31}$"));
  for (const QByteArray& layer : used) {
    check(declared.contains(layer), label + ": layer " + layer.toStdString() + " declared");
    check(layer == "0" || legal.match(QString::fromLatin1(layer)).hasMatch(), label + ": layer name " + layer.toStdString() + " is legal R12");
  }
  *layersUsed = used;
}

double ringArea(const std::vector<double>& ring) {
  double a = 0;
  const size_t n = ring.size() / 2;
  for (size_t i = 0; i < n; ++i) {
    const size_t j = (i + 1) % n;
    a += ring[2 * i] * ring[2 * j + 1] - ring[2 * j] * ring[2 * i + 1];
  }
  return a * 0.5;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir dir;

  // Two 30 mm cubes, and a loose (ungrouped) hole on the bed through the left one.
  Document doc;
  const QString left = doc.addShape("box", 0, 0);
  const QString right = doc.addShape("box", 60, 10);
  const QString hole = doc.addShape("cylinder", 0, 0);
  doc.find(hole)->hole = true;
  doc.find(hole)->radius = 5;
  doc.find(hole)->size[2] = 40;
  doc.find(hole)->pos[2] = 20;
  doc.find(left)->name = "Left cube";
  doc.find(right)->name = QStringLiteral("Right cube: \"the other one\"");

  QString error;
  const CpuMesh merged = evaluateSolid(doc, {}, &error);
  const auto objects = evaluateObjects(doc, {}, &error);
  check(objects.size() == 2, "two objects (" + std::to_string(objects.size()) + ")");
  double sum = 0;
  for (const auto& object : objects) sum += object.mesh.volume;
  check(std::abs(sum - merged.volume) < 1, "objects keep the bed hole: " + std::to_string(sum) + " vs " + std::to_string(merged.volume));
  check(merged.volume < 2 * 27000 - 1000, "the hole cuts at all");
  if (objects.size() == 2) {
    check(objects[0].name == "Left cube" && objects[0].mesh.volume < 27000 - 1000, "left cube named and cut");
    check(std::abs(objects[1].mesh.volume - 27000) < 1, "right cube whole");
  }

  // A hole that swallows a whole part leaves no empty object behind.
  {
    Document eaten = doc;
    Node* big = eaten.find(hole);
    big->radius = 40;
    big->pos[0] = 60;
    big->pos[1] = 10;
    const auto rest = evaluateObjects(eaten, {}, &error);
    check(rest.size() == 1, "a part eaten by a hole is left out (" + std::to_string(rest.size()) + ")");
  }

  // Selection: only the right cube.
  {
    const auto picked = evaluateObjects(doc, {right}, &error);
    check(picked.size() == 1 && std::abs(picked[0].mesh.volume - 27000) < 1, "selected object only");
  }

  // OBJ: one o-line per object.
  const QString obj = dir.filePath("parts.obj");
  check(writeObj(objects, obj, &error), "OBJ written");
  {
    QFile file(obj);
    file.open(QIODevice::ReadOnly);
    const QByteArray text = file.readAll();
    check(text.count("\no ") == 2, "OBJ has two objects");
    check(text.contains("\no Left cube\n"), "OBJ object named after the part");
  }
  check(write3mf(objects, dir.filePath("parts.3mf"), &error), "3MF written");

  // DXF mesh: R12, one legal layer per part.
  {
    const QString path = dir.filePath("parts.dxf");
    check(writeDxf(objects, path, &error), "DXF written");
    QSet<QByteArray> layers;
    int polylines = 0, faces = 0;
    checkDxf(path, "mesh DXF", &layers, &polylines, &faces);
    check(layers.contains("LEFT_CUBE") && layers.contains("RIGHT_CUBE_THE_OTHER_ONE"), "a layer per part");
    check(faces > 20, "3DFACEs written");
  }

  // Slices.
  {
    const double mid[3] = {0, 0, 15};
    const double up[3] = {0, 0, 1};
    QString note;
    const auto rings = sliceContours(doc, {}, mid, up, &error, &note);
    check(rings.size() == 3, "slice at 15 mm: two squares and the hole (" + std::to_string(rings.size()) + ")");
    int holes = 0;
    for (const auto& ring : rings) holes += ringArea(ring) < 0 ? 1 : 0;
    check(holes == 1, "one ring runs the other way (the hole)");
    const QString path = dir.filePath("slice.dxf");
    check(writeDxfSlice(rings, path, &error), "slice DXF written");
    QSet<QByteArray> layers;
    int polylines = 0, faces = 0;
    checkDxf(path, "slice DXF", &layers, &polylines, &faces);
    check(polylines == 3 && layers.contains("OUTLINE") && layers.contains("HOLES"), "outlines and holes on their own layers");

    // The bottom face, clicked from below (normal down), gives the same drawing as the top: no mirror.
    auto minMax = [](const std::vector<std::vector<double>>& r, double* ylo, double* yhi) {
      *ylo = 1e9;
      *yhi = -1e9;
      for (const auto& ring : r)
        for (size_t i = 1; i < ring.size(); i += 2) *ylo = std::min(*ylo, ring[i]), *yhi = std::max(*yhi, ring[i]);
    };
    const double bottom[3] = {60, 10, 0};
    const double down[3] = {0, 0, -1};
    const auto under = sliceContours(doc, {}, bottom, down, &error, &note);
    const double top[3] = {60, 10, 30};
    const auto over = sliceContours(doc, {}, top, up, &error, &note);
    check(!under.empty() && !over.empty(), "slices on the bottom and top faces");
    double a0, a1, b0, b1;
    minMax(under, &a0, &a1);
    minMax(over, &b0, &b1);
    check(std::abs(a0 - b0) < 1e-3 && std::abs(a1 - b1) < 1e-3, "bottom face not mirrored (" + std::to_string(a0) + " vs " + std::to_string(b0) + ")");
    check(std::abs(b0 - (-15)) < 1e-3 && std::abs(b1 - 25) < 1e-3,
          "bed X and Y kept, wherever the face was clicked (" + std::to_string(b0) + " to " + std::to_string(b1) + ")");
    int ccw = 0;
    for (const auto& ring : under) ccw += ringArea(ring) > 0 ? 1 : 0;
    check(ccw == 2, "outlines from below still run counter clockwise");
  }

  // Hole clearance. A 30 mm cube with a 10 x 10 mm square hole through it (a scaled box, so the
  // clearance must be measured at real size). Grown by c, the hole's section is a square c bigger
  // on every side.
  {
    // Grown faces with square corners (the offset keeps a box a box): (side + 2c)^2.
    auto section = [](double side, double c) { return (side + 2 * c) * (side + 2 * c); };
    auto plate = [](double holeTop, double holeHeight) {
      Document d;
      const QString cube = d.addShape("box", 0, 0);
      const QString cut = d.addShape("box", 0, 0);
      Node* h = d.find(cut);
      h->hole = true;
      h->scale[0] = h->scale[1] = 10.0 / 30;
      h->scale[2] = holeHeight / 30;
      h->pos[2] = holeTop - holeHeight / 2;
      (void)cube;
      return std::make_pair(d, cut);
    };
    QString e;
    auto [through, cut] = plate(35, 40);  // z -5 .. 35: right through
    const double none = evaluateSolid(through, {}, &e).volume;
    check(std::abs(none - (27000 - 30 * 100)) < 0.5, "no clearance: exact 10 mm hole (" + std::to_string(none) + ")");
    through.find(cut)->clearanceMode = 2;
    through.find(cut)->holeClearance = 0.1;
    const double own = evaluateSolid(through, {}, &e).volume;
    const double want = 27000 - 30 * section(10, 0.1);
    check(std::abs(own - want) < 0.05, "own clearance 0.1: " + std::to_string(own) + " want " + std::to_string(want));
    double lo[3], hi[3];
    localBounds(through, *through.find(cut), lo, hi);
    check(std::abs((hi[0] - lo[0]) * through.find(cut)->scale[0] - 10) < 1e-6, "the hole's size stays the size drawn");

    // Printer clearance follows the setting.
    through.find(cut)->clearanceMode = 1;
    setPrinterClearance(0.3);
    const double p3 = evaluateSolid(through, {}, &e).volume;
    check(std::abs(p3 - (27000 - 30 * section(10, 0.3))) < 0.5, "printer clearance 0.3 (" + std::to_string(p3) + ")");
    setPrinterClearance(0.05);
    const double p05 = evaluateSolid(through, {}, &e).volume;
    check(std::abs(p05 - (27000 - 30 * section(10, 0.05))) < 0.3, "printer clearance changed to 0.05 is picked up (" + std::to_string(p05) + ")");

    // A blind pocket 5 mm deep (z 25 .. 35): all round deepens it by c, sides only keeps the depth.
    auto [pocket, pc] = plate(35, 10);
    pocket.find(pc)->clearanceMode = 2;
    pocket.find(pc)->holeClearance = 0.2;
    const double all = 27000 - evaluateSolid(pocket, {}, &e).volume;
    pocket.find(pc)->clearanceSidesOnly = true;
    const double sides = 27000 - evaluateSolid(pocket, {}, &e).volume;
    check(std::abs(sides - 5 * section(10, 0.2)) < 0.3, "sides only keeps the depth (" + std::to_string(sides) + ")");
    check(all - sides > 0.9 * 100 * 0.2, "all round also goes 0.2 deeper (" + std::to_string(all - sides) + ")");

    // A horizontal pin hole (a cylinder turned on its side), sides only: the radius grows, the length not.
    Document pin;
    pin.addShape("box", 0, 0);
    const QString rod = pin.addShape("cylinder", 0, 0);
    Node* r = pin.find(rod);
    r->hole = true;
    r->radius = 4;
    r->sides = 128;
    r->size[2] = 40;
    r->rot[0] = 90;
    r->pos[2] = 15;
    const double before = 27000 - evaluateSolid(pin, {}, &e).volume;
    r->clearanceMode = 2;
    r->holeClearance = 0.15;
    r->clearanceSidesOnly = true;
    const double after = 27000 - evaluateSolid(pin, {}, &e).volume;
    const double ratio = after / before;
    const double wantRatio = (4.15 * 4.15) / (4.0 * 4.0);
    check(std::abs(ratio - wantRatio) < 0.003, "tilted pin hole grows around its own axis (" + std::to_string(ratio) + " vs " + std::to_string(wantRatio) + ")");

    // A group used as a hole grows as a whole.
    Document grouped;
    grouped.addShape("box", 0, 0);
    const QString a = grouped.addShape("box", 0, 0);
    grouped.find(a)->scale[0] = grouped.find(a)->scale[1] = 10.0 / 30;
    grouped.find(a)->scale[2] = 40.0 / 30;
    grouped.find(a)->pos[2] = 15;
    const QString b2 = grouped.addShape("box", 0, 0);
    grouped.find(b2)->scale[0] = grouped.find(b2)->scale[1] = 4.0 / 30;
    grouped.find(b2)->scale[2] = 40.0 / 30;
    grouped.find(b2)->pos[0] = 20;  // outside the cube: does not cut
    grouped.find(b2)->pos[2] = 15;
    QString groupError;
    grouped.groupSelected({a, b2}, &groupError);
    Node& g = grouped.nodes.back();
    g.hole = true;
    g.clearanceMode = 2;
    g.holeClearance = 0.1;
    const double gv = evaluateSolid(grouped, {}, &e).volume;
    check(std::abs(gv - want) < 0.3, "group hole grows (" + std::to_string(gv) + " want " + std::to_string(want) + ")");
    // A detailed non-convex hole (a torus of 32768 triangles) must not take a minute.
    {
      Document big;
      big.addShape("box", 0, 0);
      const QString t = big.addShape("torus", 0, 0);
      Node* tn = big.find(t);
      tn->hole = true;
      tn->segments = 128;
      const double before = 27000 - evaluateSolid(big, {}, &e).volume;
      tn->clearanceMode = 2;
      tn->holeClearance = 0.1;
      const auto start = std::chrono::steady_clock::now();
      const double after = 27000 - evaluateSolid(big, {}, &e).volume;
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      check(seconds < 5, "clearance on a 32k-triangle hole is quick (" + std::to_string(seconds) + " s)");
      // Torus inside the cube: radius 16 / 5, the cube cuts it. Growth ~ surface x c; just check it grew sensibly.
      check(after > before && after < before * 1.15, "torus hole grew a little (" + std::to_string(before) + " -> " + std::to_string(after) + ")");
    }
    std::printf("hole clearance: none %.2f, own 0.1 %.2f (want %.2f), printer 0.3 %.2f, 0.05 %.2f, pocket all %.2f / sides %.2f, "
                "pin ratio %.4f (want %.4f), group %.2f\n",
                none, own, want, p3, p05, all, sides, ratio, wantRatio, gv);
    setPrinterClearance(0.2);
  }

  // New basic shapes: closed solids with the volumes their shapes give.
  {
    auto volumeOf = [](const Node& n) {
      Document d;
      d.nodes.push_back(n);
      QString e;
      return evaluateSolid(d, {}, &e).volume;
    };
    Document d;
    const double pi = 3.14159265358979;
    const QString star = d.addShape("star", 0, 0), flat = d.addShape("starflat", 0, 0), roof = d.addShape("roof", 0, 0),
                  gem = d.addShape("diamond", 0, 0);
    const Node& s1 = *d.find(star);
    const double starArea = s1.sides * s1.radius * s1.radius2 * std::sin(pi / s1.sides);
    const double pointed = volumeOf(s1), flatV = volumeOf(*d.find(flat));
    check(std::abs(pointed - starArea * s1.size[2] / 3) < 0.5, "pointed star is a star pyramid (" + std::to_string(pointed) + ")");
    check(std::abs(flatV - starArea * d.find(flat)->size[2]) < 0.5, "flat star is an extruded star (" + std::to_string(flatV) + ")");
    const Node& r = *d.find(roof);
    const double roofV = volumeOf(r);
    check(std::abs(roofV - r.size[0] * r.size[1] * r.size[2] / 2) < 0.5, "roof is half a box (" + std::to_string(roofV) + ")");
    double lo[3], hi[3];
    localBounds(d, r, lo, hi);
    check(std::abs((hi[0] - lo[0]) - r.size[0]) < 1e-6 && std::abs((hi[1] - lo[1]) - r.size[1]) < 1e-6 &&
              std::abs((hi[2] - lo[2]) - r.size[2]) < 1e-6 && std::abs(lo[2] + r.size[2] / 2) < 1e-6,
          "roof fills its size, centered");
    Node pointedGem = *d.find(gem);
    Node cutGem = pointedGem;
    cutGem.gemTable = 0.6;
    const double pv = volumeOf(pointedGem), cv = volumeOf(cutGem);
    check(pv > 0 && cv > pv, "a cut gem holds more than the pointed diamond (" + std::to_string(cv) + " vs " + std::to_string(pv) + ")");
    {
      Document g;
      g.nodes.push_back(cutGem);
      QString e;
      const double top[3] = {0, 0, cutGem.pos[2] + cutGem.size[2] / 2};
      const double up[3] = {0, 0, 1};
      const auto rings = sliceContours(g, {}, top, up, &e);
      // The flat top: the table, a square (4 sides) 60% of the width, turned 45°.
      double area = 0;
      for (const auto& ring : rings) area += std::abs(ringArea(ring));
      const double half = 0.6 * cutGem.size[0] / 2;
      check(std::abs(area - 2 * half * half) < 2.5, "the gem has a flat top (" + std::to_string(area) + ")");
    }
  }

  // A plate and a spoke meant to be flush, the spoke 0.00001 mm low (a 30 mm box scaled to 0.083333
  // instead of 1/12, as the scale rounding used to do). Exported, the tops are one height again, and
  // no facet is left tilted by the nanometre step (slicers printed grooves at those joins).
  {
    check(tidyScale(2.5 / 30.0) == 2.5 / 30.0, "tidyScale keeps a scale such as 1/12 exact");
    check(tidyScale(0.99999994) == 1.0 && tidyScale(0.50000003) == 0.5, "tidyScale still takes off float noise");
    Document flush;
    const QString plate = flush.addShape("box", 0, 0);
    const QString spoke = flush.addShape("box", 20, 0);
    Node* p = flush.find(plate);
    p->scale[2] = 2.5 / 30;
    p->pos[2] = 1.3;  // 0.05 .. 2.55
    Node* s = flush.find(spoke);
    s->scale[0] = 0.5;
    s->scale[2] = 0.083333;  // 2.49999 high
    s->pos[2] = 0.05 + 30 * 0.083333 / 2;
    for (Node& node : flush.nodes) node.edgeFillet = 0;
    const CpuMesh joined = evaluateSolid(flush, {}, &error);
    auto topHeights = [](const std::vector<float>& xyz) {
      QSet<float> tops;
      for (size_t i = 2; i < xyz.size(); i += 3)
        if (xyz[i] > 2.5f) tops.insert(xyz[i]);
      return tops;
    };
    check(topHeights(joined.vertices).size() == 2, "the test spoke really is a step below the plate");
    const QString stl = dir.filePath("flush.stl");
    check(writeStl(joined, stl, &error), "flush STL written");
    QFile file(stl);
    if (file.open(QIODevice::ReadOnly)) {
      const QByteArray data = file.readAll();
      std::vector<float> xyz;
      int tilted = 0;
      const quint32 count = data.size() >= 84 ? *reinterpret_cast<const quint32*>(data.constData() + 80) : 0;
      for (quint32 t = 0; t < count && 84 + (t + 1) * 50 <= quint32(data.size()); ++t) {
        const float* f = reinterpret_cast<const float*>(data.constData() + 84 + t * 50);
        for (int i = 3; i < 12; ++i) xyz.push_back(f[i]);
        const bool level = f[5] == f[8] && f[8] == f[11];
        if (!level && std::abs(f[2]) > 0.999f) ++tilted;
      }
      check(topHeights(xyz).size() == 1, "STL: plate and spoke tops at one height");
      check(tilted == 0, "STL: no facet tilted by the step (" + std::to_string(tilted) + ")");
    } else {
      check(false, "flush STL readable");
    }
    std::vector<ExportObject> one(1);
    one[0].name = "flush";
    one[0].mesh = joined;
    const QByteArray model = model3mf(one);
    QSet<QByteArray> modelTops;
    for (const QByteArray& part : model.split(' '))
      if (part.startsWith("z=\"2.5") && part.mid(3).split('"').front().toDouble() > 2.5) modelTops.insert(part.mid(3).split('"').front());
    check(modelTops.size() == 1, "3MF: plate and spoke tops at one height (" + std::to_string(modelTops.size()) + ")");
    // Real steps stay: a top 0.01 mm lower is a different height.
    CpuMesh stepped = joined;
    for (size_t i = 2; i < stepped.vertices.size(); i += 3)
      if (std::abs(stepped.vertices[i] - 2.54999f) < 1e-6f) stepped.vertices[i] = 2.54f;
    std::vector<CpuMesh*> list{&stepped};
    levelFlatHeights(list);
    check(topHeights(stepped.vertices).size() == 2, "a real 0.01 mm step is kept");
  }

  // Imported STLs whose shells touch (a sealed pocket on the outside wall, a pocket that meets
  // itself as a hollow hat's did at its brim). Exported, every edge is
  // used by exactly two triangles once the file's points are welded, as a slicer does.
  {
    std::vector<float> tris;
    auto box = [&](const int lo[3], const int hi[3], bool inward) {
      // Each face as a grid of unit squares, u x v pointing out.
      const int faces[6][3] = {{0, 2, 1}, {0, 1, 2}, {1, 0, 2}, {1, 2, 0}, {2, 1, 0}, {2, 0, 1}};
      for (int f = 0; f < 6; ++f) {
        const int axis = faces[f][0], u = faces[f][1], v = faces[f][2];
        const bool high = (f % 2) == 1;
        for (int i = lo[u]; i < hi[u]; ++i)
          for (int j = lo[v]; j < hi[v]; ++j) {
            auto at = [&](int di, int dj) {
              float p[3];
              p[axis] = float(high ? hi[axis] : lo[axis]);
              p[u] = float(i + di);
              p[v] = float(j + dj);
              return std::array<float, 3>{p[0], p[1], p[2]};
            };
            const std::array<float, 3> q[2][3] = {{at(0, 0), at(1, 0), at(1, 1)}, {at(0, 0), at(1, 1), at(0, 1)}};
            for (const auto& t : q) {
              const std::array<float, 3> c[3] = {t[0], inward ? t[2] : t[1], inward ? t[1] : t[2]};
              for (const auto& pt : c) tris.insert(tris.end(), pt.begin(), pt.end());
            }
          }
      }
    };
    // Writes the boxes as one STL, imports it, exports the plate; gives the volume and how many
    // welded edges are not used by exactly two triangles.
    auto roundTrip = [&](const std::vector<std::array<int, 7>>& boxes, const QString& name, double* volume) {
      tris.clear();
      for (const auto& b : boxes) {
        const int lo[3] = {b[0], b[1], b[2]}, hi[3] = {b[3], b[4], b[5]};
        box(lo, hi, b[6] != 0);
      }
      const QString path = dir.filePath(name);
      QFile file(path);
      if (!file.open(QIODevice::WriteOnly)) return -1;
      file.write(QByteArray(80, '\0'));
      const quint32 count = quint32(tris.size() / 9);
      file.write(reinterpret_cast<const char*>(&count), 4);
      for (quint32 t = 0; t < count; ++t) {
        float rec[12] = {0};
        std::memcpy(rec + 3, &tris[t * 9], 36);
        file.write(reinterpret_cast<const char*>(rec), 48);
        file.write("\0\0", 2);
      }
      file.close();
      Document doc;
      const QString id = doc.addShape("stl", 0, 0);
      doc.find(id)->meshPath = path;
      const CpuMesh out = evaluateSolid(doc, {}, &error);
      if (out.triCount() == 0) return -1;
      std::map<std::array<float, 3>, int> weld;
      std::map<std::pair<int, int>, int> uses;
      *volume = 0;
      for (size_t t = 0; t < out.triCount(); ++t) {
        int ids[3];
        const float *a = out.corner(t, 0), *b = out.corner(t, 1), *c = out.corner(t, 2);
        *volume += (double(a[0]) * (double(b[1]) * c[2] - double(b[2]) * c[1]) - double(a[1]) * (double(b[0]) * c[2] - double(b[2]) * c[0]) +
                    double(a[2]) * (double(b[0]) * c[1] - double(b[1]) * c[0])) / 6.0;
        for (int k = 0; k < 3; ++k) {
          const float* p = out.corner(t, k);
          ids[k] = weld.emplace(std::array<float, 3>{p[0], p[1], p[2]}, int(weld.size())).first->second;
        }
        for (int k = 0; k < 3; ++k) ++uses[{std::min(ids[k], ids[(k + 1) % 3]), std::max(ids[k], ids[(k + 1) % 3])}];
      }
      int bad = 0;
      for (const auto& [edge, n] : uses) bad += n != 2;
      return bad;
    };
    double volume = 0;
    // A pocket whose wall lies on the outside.
    int bad = roundTrip({{0, 0, 0, 10, 10, 10, 0}, {0, 3, 3, 4, 7, 7, 1}}, "wall.stl", &volume);
    check(bad == 0, "pocket on the outside: every welded edge used twice (" + std::to_string(bad) + " not)");
    // A pocket of two rooms meeting along an edge (it touches itself).
    bad = roundTrip({{0, 0, 0, 10, 10, 10, 0}, {2, 2, 2, 4, 4, 4, 1}, {4, 4, 2, 6, 6, 4, 1}}, "rooms.stl", &volume);
    check(bad == 0, "pocket meeting itself: every welded edge used twice (" + std::to_string(bad) + " not)");
    check(volume > 983.9 && volume < 1000.1, "pocket meeting itself: a sensible volume (" + std::to_string(volume) + ")");
    // A clean pocket (as Hollow makes) stays.
    bad = roundTrip({{0, 0, 0, 10, 10, 10, 0}, {3, 3, 3, 7, 7, 7, 1}}, "hollow.stl", &volume);
    check(bad == 0 && std::abs(volume - 936) < 0.01, "a clean pocket is kept (" + std::to_string(volume) + ")");
  }

  std::printf(failures ? "%d failure(s)\n" : "all export checks passed\n", failures);
  return failures ? 1 : 0;
}
