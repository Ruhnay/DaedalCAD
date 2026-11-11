// Modelling tools: intersection groups, hollowing (closed and with open sides, also on a stretched
// part and on a group), multi-colour groups split into one closed piece per colour, and the colours
// those carry into 3MF (base materials, one model of several parts) and OBJ (a material file).

#include "document.h"
#include "evaluator.h"
#include "exporter.h"

#include <QApplication>
#include <QFile>
#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <map>

static constexpr double kPiTest = 3.14159265358979323846;
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

QString box(Document& doc, double x, double y, double z, double sx, double sy, double sz, const QColor& color = QColor("#d7c4a3")) {
  const QString id = doc.addShape("box", 0, 0);
  Node* n = doc.find(id);
  n->pos[0] = x;
  n->pos[1] = y;
  n->pos[2] = z;
  n->size[0] = sx;
  n->size[1] = sy;
  n->size[2] = sz;
  n->color = color;
  return id;
}

QString gOut;  // pictures: with a folder on the command line, the results are saved as STL there

void save(const Document& doc, const char* name) {
  if (gOut.isEmpty()) return;
  QString error;
  const CpuMesh mesh = evaluateSolid(doc, {}, &error);
  if (mesh.triangles > 0) writeStl(mesh, gOut + "/" + name + ".stl", &error);
}

double volumeOf(const Document& doc) {
  QString error;
  return evaluateSolid(doc, {}, &error).volume;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir dir;
  const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : dir.path();
  if (argc > 1) gOut = out;

  // Intersection: two 30 mm cubes overlapping by 10 mm keep a 10 x 30 x 30 slab.
  {
    Document doc;
    const QString a = box(doc, 0, 0, 15, 30, 30, 30);
    const QString b = box(doc, 20, 0, 15, 30, 30, 30);
    QString error;
    doc.groupSelected({a, b}, &error);
    doc.nodes.back().intersect = true;
    const double v = volumeOf(doc);
    std::printf("intersection: %.3f (expected 9000)\n", v);
    check(std::abs(v - 9000) < 1e-3, "intersection keeps only the overlap");
    save(doc, "intersection");
    // A hole inside still cuts it.
    const QString hole = doc.addShape("cylinder", 10, 0);
    Node* h = doc.find(hole);
    h->hole = true;
    h->radius = 2;
    h->size[2] = 40;
    h->pos[2] = 15;
    h->sides = 64;
    const QString group = doc.nodes[doc.nodes.size() - 2].id;
    h->parentId = group;
    h->pos[0] -= doc.find(group)->pos[0];
    h->pos[1] -= doc.find(group)->pos[1];
    h->pos[2] -= doc.find(group)->pos[2];
    const double cut = volumeOf(doc);
    std::printf("intersection with a hole: %.3f\n", cut);
    check(cut < 9000 - 300 && cut > 9000 - 400, "a hole in an intersection cuts it");
    // Nothing in common: nothing left, with a note.
    Document apart;
    const QString c = box(apart, 0, 0, 15, 10, 10, 10);
    const QString d = box(apart, 50, 0, 15, 10, 10, 10);
    apart.groupSelected({c, d}, &error);
    apart.nodes.back().intersect = true;
    QString why;
    const CpuMesh none = evaluateSolid(apart, {}, &why);
    check(none.triangles == 0 && why.contains("nothing in common"), "parts apart: an empty intersection, said so");
  }

  // Hollow: a 40 x 30 x 20 box with 2 mm walls; open on top; stretched; a group.
  {
    Document doc;
    const QString id = box(doc, 0, 0, 10, 40, 30, 20);
    doc.find(id)->shellWall = 2;
    const double closed = volumeOf(doc);
    std::printf("hollow box: %.3f (expected %.3f)\n", closed, 24000.0 - 36 * 26 * 16);
    check(std::abs(closed - (24000.0 - 36 * 26 * 16)) < 1e-3, "closed hollow box: walls of exactly 2 mm");
    doc.find(id)->shellOpen = 32;  // top
    const double cup = volumeOf(doc);
    std::printf("open on top: %.3f (expected %.3f)\n", cup, 24000.0 - 36 * 26 * 18);
    check(std::abs(cup - (24000.0 - 36 * 26 * 18)) < 1e-3, "open top: the cup has a floor and four walls");
    save(doc, "cup");
    doc.find(id)->shellOpen = 32 | 16 | 1;  // top, bottom and the -X side
    const double sleeve = volumeOf(doc);
    // Two walls along X (40 x 2 x 20 each) and the +X end (2 x 26 x 20).
    std::printf("open top, bottom, left: %.3f (expected %.3f)\n", sleeve, 2 * 40.0 * 2 * 20 + 2 * 26 * 20);
    check(std::abs(sleeve - (2 * 40.0 * 2 * 20 + 2 * 26 * 20)) < 1e-3, "open on three sides: a U-shaped sleeve");
    save(doc, "sleeve");
    // Stretched to twice its length: walls stay 2 mm.
    doc.find(id)->size[0] = 20;
    doc.find(id)->scale[0] = 2;
    doc.find(id)->shellOpen = 0;
    const double stretched = volumeOf(doc);
    check(std::abs(stretched - closed) < 1e-2, "a stretched part keeps 2 mm walls (" + std::to_string(stretched) + ")");
    // Too thick: left solid, with a note.
    doc.find(id)->shellWall = 20;
    check(std::abs(volumeOf(doc) - 24000) < 1e-3, "a wall too thick leaves it solid");
    // A group (two boxes side by side, joined) hollowed as one.
    Document g;
    const QString a = box(g, -10, 0, 10, 20, 30, 20);
    const QString b = box(g, 10, 0, 10, 20, 30, 20);
    QString error;
    g.groupSelected({a, b}, &error);
    g.nodes.back().shellWall = 2;
    const double group = volumeOf(g);
    check(std::abs(group - closed) < 1e-3, "a group hollows as one solid (" + std::to_string(group) + ")");
    // A sphere: walls even all round (a 16-sided ball, never thinner than asked).
    Document s;
    const QString ball = s.addShape("sphere", 0, 0);
    s.find(ball)->radius = 20;
    s.find(ball)->pos[2] = 20;
    s.find(ball)->shellWall = 3;
    const double shell = volumeOf(s);
    const double exact = 4.0 / 3 * M_PI * (20.0 * 20 * 20 - 17.0 * 17 * 17);
    std::printf("hollow sphere: %.1f (exact %.1f)\n", shell, exact);
    check(shell > exact * 0.95 && shell < exact * 1.08, "a hollow sphere is a shell of about the right thickness");
    s.find(ball)->shellOpen = 32;
    save(s, "bowl");
  }

  // Keep part colours: a red and a blue cube overlapping. One closed piece per colour; the red one
  // (first in the list) keeps the overlap.
  {
    Document doc;
    const QString red = box(doc, 0, 0, 15, 30, 30, 30, QColor("#d02020"));
    const QString blue = box(doc, 20, 0, 15, 30, 30, 30, QColor("#2040d0"));
    QString error;
    doc.groupSelected({red, blue}, &error);
    Node& group = doc.nodes.back();
    group.name = "Two colours";
    group.multiColor = true;
    const auto objects = evaluateObjects(doc, {}, &error);
    check(objects.size() == 2, "two colours: two objects (" + std::to_string(objects.size()) + ")");
    if (objects.size() == 2) {
      std::printf("colour pieces: %s %.1f, %s %.1f\n", qPrintable(objects[0].color.name()), objects[0].mesh.volume,
                  qPrintable(objects[1].color.name()), objects[1].mesh.volume);
      check(objects[0].color == QColor("#d02020") && std::abs(objects[0].mesh.volume - 27000) < 1e-3, "red keeps its whole cube");
      check(objects[1].color == QColor("#2040d0") && std::abs(objects[1].mesh.volume - 18000) < 1e-3, "blue gets the rest");
      check(objects[0].assembly == "Two colours" && objects[1].assembly == "Two colours", "both belong to the group's model");
    }
    // A hole in the group cuts both colours.
    const QString hole = doc.addShape("box", 10, 0);
    Node* h = doc.find(hole);
    h->hole = true;
    h->size[0] = 10;
    h->size[1] = 10;
    h->size[2] = 50;
    h->pos[2] = 15;
    h->parentId = group.id;
    for (int i = 0; i < 3; ++i) h->pos[i] -= doc.find(group.id)->pos[i];
    const auto cut = evaluateObjects(doc, {}, &error);
    double sum = 0;
    for (const auto& o : cut) sum += o.mesh.volume;
    check(cut.size() == 2 && std::abs(sum - (45000 - 3000)) < 1e-3, "a hole cuts through both colours (" + std::to_string(sum) + ")");

    // 3MF: base materials with both colours, each part pointing at its own, one model made of the two.
    const QString path3mf = out + "/colours.3mf";
    check(write3mf(cut, path3mf, &error), "3MF written");
    const QByteArray model = model3mf(cut);
    check(model.contains("<basematerials id=\"1\">") && model.contains("displaycolor=\"#D02020FF\"") &&
              model.contains("displaycolor=\"#2040D0FF\""),
          "3MF: both colours as base materials");
    check(model.contains("pid=\"1\" pindex=\"0\"") && model.contains("pid=\"1\" pindex=\"1\""), "3MF: each part has its colour");
    check(model.contains("<components>") && model.count("<component objectid=") == 2 && model.count("<item objectid=") == 1,
          "3MF: one model of two parts on the plate");
    // OBJ: a material file with both colours.
    const QString pathObj = out + "/colours.obj";
    check(writeObj(cut, pathObj, &error), "OBJ written");
    QFile obj(pathObj), mtl(out + "/colours.mtl");
    obj.open(QIODevice::ReadOnly);
    mtl.open(QIODevice::ReadOnly);
    const QByteArray objText = obj.readAll(), mtlText = mtl.readAll();
    check(objText.startsWith("# DaedalCAD millimetres\nmtllib colours.mtl\n") && objText.count("usemtl ") == 2, "OBJ: uses a material per part");
    check(mtlText.count("newmtl ") == 2 && mtlText.contains("Kd 0.8156"), "OBJ: the material file holds the colours");

    // Switched off: one object in the group's colour.
    group.multiColor = false;
    check(evaluateObjects(doc, {}, &error).size() == 1, "colours off: one object");
  }

  // Taper: a 20 x 20 x 40 box tapered to 50% along Z is a frustum (40/3 x (400 + 100 + 200)).
  {
    Document doc;
    const QString b = box(doc, 0, 0, 20, 20, 20, 40);
    doc.find(b)->taper = 50;
    doc.find(b)->taperAxis = 2;
    const double v = volumeOf(doc);
    std::printf("taper: %.2f (expected 9333.33)\n", v);
    check(std::abs(v - 40.0 / 3 * 700) < 1, "a 50% taper makes the box a frustum");
    QString error;
    const CpuMesh mesh = evaluateSolid(doc, {}, &error);
    double topHalf = 0;
    for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3)
      if (mesh.vertices[i + 2] > mesh.hi[2] - 1e-3) topHalf = std::max(topHalf, double(std::abs(mesh.vertices[i])));
    check(std::abs(topHalf - 5) < 0.01, "the top is half as wide (" + std::to_string(topHalf) + ")");
    save(doc, "taper");
  }

  // Bend 360: a 100 x 10 x 2 strip closes into one ring (a torus-like solid: Euler characteristic
  // 0), as heavy as the strip.
  {
    Document doc;
    const QString b = box(doc, 0, 0, 1, 100, 10, 2);
    Node* n = doc.find(b);
    n->bend = 360;
    n->along = 0;
    n->toward = 2;
    QString error;
    const CpuMesh mesh = evaluateSolid(doc, {}, &error);
    std::printf("ring: volume %.2f (expected about 2000), %zu triangles\n", mesh.volume, mesh.triCount());
    check(std::abs(mesh.volume - 2000) < 20, "the ring keeps the strip's volume");
    std::map<std::array<float, 3>, int> weld;
    std::map<std::pair<int, int>, int> edges;
    for (size_t t = 0; t < mesh.triCount(); ++t) {
      int id[3];
      for (int k = 0; k < 3; ++k) {
        const float* p = mesh.corner(t, k);
        id[k] = weld.emplace(std::array<float, 3>{p[0], p[1], p[2]}, int(weld.size())).first->second;
      }
      for (int k = 0; k < 3; ++k) ++edges[{std::min(id[k], id[(k + 1) % 3]), std::max(id[k], id[(k + 1) % 3])}];
    }
    int bad = 0;
    for (const auto& [edge, uses] : edges) bad += uses != 2;
    const long euler = long(weld.size()) - long(edges.size()) + long(mesh.triCount());
    check(bad == 0 && euler == 0, "the ring is one closed loop (Euler " + std::to_string(euler) + ", " + std::to_string(bad) + " bad edges)");
    save(doc, "ring");
  }

  // Text on a curve: about as much text as straight, taller (it curves), its ends down for text over
  // the top of the circle and up for text along the bottom.
  {
    Document doc;
    const QString id = doc.addShape("text", 0, 0);
    Node* n = doc.find(id);
    n->text = QStringLiteral("DAEDALCAD");
    n->fontSize = 6;
    n->size[2] = 2;
    QString error;
    const CpuMesh straight = evaluateSolid(doc, {}, &error);
    auto ends = [](const CpuMesh& m, double* endY, double* midY) {
      double maxX = 0;
      for (size_t i = 0; i + 2 < m.vertices.size(); i += 3) maxX = std::max(maxX, double(std::abs(m.vertices[i])));
      double e = 0, c = 0;
      int ne = 0, nc = 0;
      for (size_t i = 0; i + 2 < m.vertices.size(); i += 3) {
        const double x = std::abs(m.vertices[i]), y = m.vertices[i + 1];
        if (x > 0.85 * maxX) e += y, ++ne;
        if (x < 0.15 * maxX) c += y, ++nc;
      }
      *endY = ne ? e / ne : 0;
      *midY = nc ? c / nc : 0;
    };
    n = doc.find(id);
    n->textArc = 25;
    const CpuMesh top = evaluateSolid(doc, {}, &error);
    check(top.triCount() > 0 && std::abs(top.volume - straight.volume) < 0.15 * straight.volume,
          "curved text: about as much as straight (" + std::to_string(top.volume) + " vs " + std::to_string(straight.volume) + ")");
    check(top.hi[1] - top.lo[1] > straight.hi[1] - straight.lo[1] + 2, "curved text is taller: it follows the circle");
    double endY = 0, midY = 0;
    ends(top, &endY, &midY);
    check(endY < midY - 2, "over the top: the ends come down");
    n = doc.find(id);
    n->textArcBelow = true;
    const CpuMesh bottom = evaluateSolid(doc, {}, &error);
    ends(bottom, &endY, &midY);
    check(endY > midY + 2, "along the bottom: the ends go up");
    save(doc, "text_on_curve");
  }

  // Bend face on a face taller than twice the radius: the whole face turns (the hinge goes to its
  // inner edge), so the end is as long as the face. A 10 x 80 face turned 90 degrees round a hinge
  // at its edge sweeps (Pappus) 800 x (40 x pi / 2) mm3, and the end face spans the full 80 mm.
  {
    Document doc;
    const QString id = doc.addShape("elbow", 0, 0);
    Node* e = doc.find(id);
    e->faceBend = true;
    e->faceAngle = 90;
    e->faceRadius = 30;
    e->faceSteps = 48;
    e->faceAxisMode = 2;  // hinge along Y
    e->faceNormal[0] = 1;
    e->faceNormal[1] = 0;
    e->faceNormal[2] = 0;
    e->faceLoop = {0, -5, -40, 0, 5, -40, 0, 5, 40, 0, -5, 40};
    e->pos[0] = 0;
    e->pos[1] = 0;
    e->pos[2] = 40;
    QString error;
    const CpuMesh mesh = evaluateSolid(doc, {}, &error);
    const double expected = 800 * 40.05 * kPiTest / 2;
    std::printf("tall face bend: volume %.0f (expected %.0f), box %.1f x %.1f x %.1f\n", mesh.volume, expected, mesh.hi[0] - mesh.lo[0],
                mesh.hi[1] - mesh.lo[1], mesh.hi[2] - mesh.lo[2]);
    check(std::abs(mesh.volume - expected) < 0.01 * expected, "a tall face bends whole: the elbow is as big as the face swept");
    // The swept face turns from upright to lying flat: the elbow reaches 80 mm (the face) plus the
    // 0.05 mm inner radius out from the hinge, both ways.
    check(std::abs((mesh.hi[0] - mesh.lo[0]) - 80.05) < 0.5 && std::abs((mesh.hi[2] - mesh.lo[2]) - 80.05) < 0.5,
          "the end is as long as the face");
    save(doc, "tall_face_bend");
  }

  std::printf(failures ? "%d failure(s)\n" : "all modelling checks passed\n", failures);
  return failures ? 1 : 0;
}
