// Exact STEP (OpenCASCADE): designs written by exact::writeStep and read back with OpenCASCADE's
// own STEP reader must be valid solids with true round surfaces where the design has them, the
// exact volume, and their names and colours; parts it cannot make exactly come from their mesh;
// STEP files read by exact::readStep give closed meshes and stay exact when written out again.

#include "document.h"
#include "evaluator.h"
#include "edgefillet.h"
#include "exactstep.h"
#include "generators.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Reader.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <functional>
#include <cstdio>
#include <string>

namespace {

constexpr double kPi = 3.14159265358979323846;
int failures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

struct ReadBack {
  bool ok = false, valid = false;
  int solids = 0, faces = 0, cylinders = 0, cones = 0, spheres = 0, tori = 0, planes = 0, other = 0;
  double volume = 0;
};

ReadBack readBack(const QString& path) {
  ReadBack r;
  STEPControl_Reader reader;
  if (reader.ReadFile(path.toUtf8().constData()) != IFSelect_RetDone) return r;
  reader.TransferRoots();
  const TopoDS_Shape shape = reader.OneShape();
  r.ok = !shape.IsNull();
  r.valid = r.ok && BRepCheck_Analyzer(shape).IsValid();
  for (TopExp_Explorer e(shape, TopAbs_SOLID); e.More(); e.Next()) ++r.solids;
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    ++r.faces;
    switch (BRepAdaptor_Surface(TopoDS::Face(e.Current())).GetType()) {
      case GeomAbs_Plane: ++r.planes; break;
      case GeomAbs_Cylinder: ++r.cylinders; break;
      case GeomAbs_Cone: ++r.cones; break;
      case GeomAbs_Sphere: ++r.spheres; break;
      case GeomAbs_Torus: ++r.tori; break;
      default: ++r.other; break;
    }
  }
  GProp_GProps props;
  BRepGProp::VolumeProperties(shape, props, 1e-6);  // adaptive: spline faces (a stretched fillet) need it
  r.volume = props.Mass();
  return r;
}

QString add(Document& doc, const char* shape, double x, double y, double z) {
  const QString id = doc.addShape(shape, 0, 0);
  Node* n = doc.find(id);
  n->pos[0] = x;
  n->pos[1] = y;
  n->pos[2] = z;
  return id;
}

struct Result {
  ReadBack back;
  exact::Report report;
  double meshVolume = 0;
};

Result exportAndRead(const Document& doc, const QString& path, const std::string& label, const QStringList& only = {}) {
  Result out;
  QString error;
  const auto objects = evaluateObjects(doc, only, &error);
  for (const auto& o : objects) out.meshVolume += o.mesh.volume;
  int lastStep = 0, steps = 0;
  out.report.progress = [&](int done, int total, const QString&) {
    check(done >= lastStep && done <= total, label + ": progress only goes forward");
    lastStep = done;
    steps = total;
  };
  const bool written = exact::writeStep(doc, only, objects, path, &error, &out.report);
  check(!written || lastStep == steps, label + ": progress reaches the end");
  for (const QString& line : out.report.log) std::printf("    | %s\n", line.toStdString().c_str());
  check(written, label + ": written (" + error.toStdString() + ")");
  out.back = readBack(path);
  std::printf("%-30s solids %d faces %3d (plane %d cyl %d cone %d sphere %d torus %d other %d) volume %.4f, mesh %.4f; exact %d partly %d faceted %d %s\n",
              label.c_str(), out.back.solids, out.back.faces, out.back.planes, out.back.cylinders, out.back.cones, out.back.spheres,
              out.back.tori, out.back.other, out.back.volume, out.meshVolume, out.report.exact, out.report.partly, out.report.faceted,
              qPrintable(out.report.notes.join(' ')));
  check(out.back.ok && out.back.valid, label + ": OpenCASCADE reads a valid shape");
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir dir;
  const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : dir.path();
  check(exact::available(), "built with OpenCASCADE");

  // 1. A block with a round hole through it: a true cylinder, the exact volume.
  {
    Document doc;
    const QString block = add(doc, "box", 0, 0, 5);
    Node* b = doc.find(block);
    b->size[0] = 40;
    b->size[1] = 30;
    b->size[2] = 10;
    b->name = QStringLiteral("Block 'A'");
    b->color = QColor("#3070d0");
    const QString hole = add(doc, "cylinder", 5, 0, 5);
    Node* h = doc.find(hole);
    h->hole = true;
    h->radius = 4;
    h->size[2] = 20;
    QString error;
    doc.groupSelected({block, hole}, &error);
    doc.nodes.back().name = QStringLiteral("Block 'A'");
    doc.nodes.back().color = QColor("#3070d0");
    const Result r = exportAndRead(doc, out + "/block.step", "block with hole");
    const double exactVolume = 40.0 * 30 * 10 - M_PI * 16 * 10;
    check(std::abs(r.back.volume - exactVolume) < 1e-3, "block: the exact volume (" + std::to_string(r.back.volume) + ")");
    check(r.back.solids == 1 && r.back.cylinders >= 1 && r.back.faces <= 9, "block: six flat faces and a true cylinder");
    check(r.report.exact == 1 && r.report.faceted == 0, "block: reported exact");
    // Names and colours, through DaedalCAD's own reader.
    std::vector<exact::ImportedPart> parts;
    check(exact::readStep(out + "/block.step", &parts, &error), "block: read back (" + error.toStdString() + ")");
    check(parts.size() == 1 && parts[0].name == QStringLiteral("Block 'A'"), "block: its name came along");
    check(parts.size() == 1 && parts[0].hasColor && std::abs(parts[0].color.red() - 0x30) <= 1 && std::abs(parts[0].color.blue() - 0xd0) <= 1,
          "block: its colour came along");
    if (!parts.empty()) {
      double v = 0;
      const auto& t = parts[0].triangles;
      for (size_t i = 0; i + 8 < t.size(); i += 9)
        v += (t[i] * (t[i + 4] * t[i + 8] - t[i + 5] * t[i + 7]) - t[i + 1] * (t[i + 3] * t[i + 8] - t[i + 5] * t[i + 6]) +
              t[i + 2] * (t[i + 3] * t[i + 7] - t[i + 4] * t[i + 6])) / 6.0;
      std::printf("imported mesh volume %.4f, centre %.3f %.3f %.3f\n", v, parts[0].centre[0], parts[0].centre[1], parts[0].centre[2]);
      check(std::abs(v - exactVolume) < exactVolume * 2e-3, "block: the imported mesh is closed and close to the exact volume");
      check(std::abs(parts[0].centre[2] - 5) < 1e-6, "block: the imported part knows where it was");
    }
    // Imported back into a design (as DaedalCAD does it: a mesh part that remembers its STEP file),
    // then written out again: still exact.
    if (!parts.empty()) {
      QFile stl(out + "/imported.stl");
      stl.open(QIODevice::WriteOnly);
      QByteArray header(80, 0);
      stl.write(header);
      const quint32 count = static_cast<quint32>(parts[0].triangles.size() / 9);
      stl.write(reinterpret_cast<const char*>(&count), 4);
      for (size_t i = 0; i + 8 < parts[0].triangles.size(); i += 9) {
        const float normal[3] = {0, 0, 0};
        stl.write(reinterpret_cast<const char*>(normal), 12);
        stl.write(reinterpret_cast<const char*>(&parts[0].triangles[i]), 36);
        stl.write("\0\0", 2);
      }
      stl.close();
      Document again;
      const QString part = add(again, "stl", 100, 0, 5);
      Node* p = again.find(part);
      p->meshPath = out + "/imported.stl";
      p->stepSource = out + "/block.step";
      p->stepIndex = 0;
      for (int a = 0; a < 3; ++a) p->stepOffset[a] = parts[0].centre[a];
      p->rot[2] = 90;
      const Result re = exportAndRead(again, out + "/reexport.step", "imported, turned, re-exported");
      check(std::abs(re.back.volume - exactVolume) < 1e-3 && re.back.cylinders >= 1 && re.report.exact == 1,
            "re-export: still the exact block with its true hole");
    }
  }

  // 2. Hole clearance, sides only: the hole 0.3 mm wider all round, its height kept.
  {
    Document doc;
    const QString block = add(doc, "box", 0, 0, 5);
    doc.find(block)->size[2] = 10;
    const QString hole = add(doc, "cylinder", 0, 0, 5);
    Node* h = doc.find(hole);
    h->hole = true;
    h->radius = 5;
    h->size[2] = 6;  // a blind pocket from z = 2 to 8
    h->clearanceMode = 2;
    h->holeClearance = 0.3;
    h->clearanceSidesOnly = true;
    QString error;
    doc.groupSelected({block, hole}, &error);
    const Result r = exportAndRead(doc, out + "/clearance.step", "hole with side clearance");
    const double exactVolume = 30.0 * 30 * 10 - M_PI * 5.3 * 5.3 * 6;
    check(std::abs(r.back.volume - exactVolume) < 1e-3, "clearance: the hole is 0.3 mm wider, as tall as drawn (" + std::to_string(r.back.volume) + ")");
  }

  // 3. Intersect a box and a sphere: a cube with round corners, spherical faces in it.
  {
    Document doc;
    const QString cube = add(doc, "box", 0, 0, 15);
    const QString ball = add(doc, "sphere", 0, 0, 15);
    doc.find(ball)->radius = 19;
    QString error;
    doc.groupSelected({cube, ball}, &error);
    doc.nodes.back().intersect = true;
    const Result r = exportAndRead(doc, out + "/intersect.step", "box x sphere");
    check(r.back.spheres >= 1 && r.back.planes == 6 && std::abs(r.back.volume - r.meshVolume) < r.meshVolume * 0.01,
          "intersection: six flat faces cut round by the sphere, about the mesh's volume");
  }

  // 4. Closed hollow box: walls of exactly 2 mm.
  {
    Document doc;
    const QString box = add(doc, "box", 0, 0, 10);
    Node* b = doc.find(box);
    b->size[0] = 40;
    b->size[1] = 30;
    b->size[2] = 20;
    b->shellWall = 2;
    const Result r = exportAndRead(doc, out + "/hollow.step", "hollow box");
    check(std::abs(r.back.volume - (24000.0 - 36 * 26 * 16)) < 1e-3 && r.back.faces == 12, "hollow: 2 mm walls, inside and outside flat");
  }

  // 5. Turned, stretched and mirrored parts: a box stretched (still flat faces), a cylinder scaled
  // evenly across (still a cylinder), a sphere stretched (an ellipsoid), a mirrored wedge.
  {
    Document doc;
    const QString box = add(doc, "box", 0, 0, 20);
    Node* b = doc.find(box);
    b->scale[0] = 2;
    b->scale[2] = 0.5;
    b->rot[0] = 30;
    b->rot[2] = 45;
    const QString cyl = add(doc, "cylinder", 80, 0, 20);
    Node* c = doc.find(cyl);
    c->scale[0] = c->scale[1] = 1.5;
    c->scale[2] = 0.7;
    c->rot[1] = 60;
    const QString egg = add(doc, "sphere", 0, 80, 20);
    Node* e = doc.find(egg);
    e->radius = 10;
    e->scale[2] = 1.6;
    const QString wedge = add(doc, "wedge", 80, 80, 15);
    doc.find(wedge)->mirror[0] = true;
    doc.find(wedge)->rot[2] = 20;
    const Result r = exportAndRead(doc, out + "/placed.step", "turned and stretched");
    check(r.back.solids == 4 && r.report.exact == 4, "placed: four exact solids");
    check(std::abs(r.back.volume - r.meshVolume) < r.meshVolume * 0.01, "placed: the same volume as the mesh, to the facets");
    check(r.back.cylinders >= 1, "placed: the evenly scaled cylinder stays a cylinder");
  }

  // 6. A repeat (three cylinders 20 mm apart) and a hole left loose on the plate.
  {
    Document doc;
    const QString cyl = add(doc, "cylinder", 0, 0, 10);
    doc.find(cyl)->radius = 5;
    doc.find(cyl)->size[2] = 20;
    const double move[3] = {20, 0, 0}, drot[3] = {0, 0, 0}, resize[3] = {1, 1, 1};
    QString error;
    doc.repeat({cyl}, 3, move, drot, resize, &error);
    const QString slab = add(doc, "box", 0, 60, 2);
    doc.find(slab)->size[2] = 4;
    const QString loose = add(doc, "cylinder", 0, 60, 2);
    doc.find(loose)->hole = true;
    doc.find(loose)->radius = 6;
    doc.find(loose)->size[2] = 10;
    const Result r = exportAndRead(doc, out + "/repeat.step", "repeat and loose hole");
    const double expected = 3 * M_PI * 25 * 20 + (30.0 * 30 * 4 - M_PI * 36 * 4);
    check(std::abs(r.back.volume - expected) < 1e-3, "repeat and loose hole: exact volumes (" + std::to_string(r.back.volume) + ")");
  }

  // 7. A part that is partly mesh (a generator in a group with a box), and one that is all mesh.
  {
    Document doc;
    const QString block = add(doc, "box", 0, 0, 5);
    doc.find(block)->size[2] = 10;
    const QString nut = doc.addShape("generator", 0, 0);
    Node* n = doc.find(nut);
    n->genType = "stdnut";
    n->genParams = QJsonObject{{"standard", "4032"}, {"size", "M8"}};
    n->pos[2] = 13;
    QString error;
    doc.groupSelected({block, nut}, &error);
    const QString lone = doc.addShape("generator", 80, 0);
    Node* g = doc.find(lone);
    g->genType = "stdwasher";
    g->genParams = QJsonObject{{"size", "M8"}};
    const Result r = exportAndRead(doc, out + "/mixed.step", "partly mesh, all mesh");
    // The washer's mesh has its cylinders and cone recognised now: partly exact too.
    check(r.report.partly + r.report.exact == 2 && r.report.faceted == 0 && r.back.solids == 2, "mixed: both parts (partly) exact");
    check(std::abs(r.back.volume - r.meshVolume) < r.meshVolume * 0.01, "mixed: the volume of the meshes");
  }

  // 8. A mesh with corners closer than OpenCASCADE's tolerance (the sealed enclosure) is still written.
  {
    Document doc;
    const QString id = doc.addShape("generator", 0, 0);
    Node* n = doc.find(id);
    n->genType = "enclosure";
    n->genParams = gen::defaults("enclosure");
    n->genParams["seal"] = true;
    const Result r = exportAndRead(doc, out + "/enclosure.step", "sealed enclosure");
    check(r.back.solids == 2, "enclosure: written, box and lid");
    check(std::abs(r.back.volume - r.meshVolume) < r.meshVolume * 0.01, "enclosure: the volume of the mesh");
  }

  // 9. Rounded edges, exact: each shape's own Fillet, picked edge fillets and chamfers, on shapes
  // and on a group. The same edges as the mesh, so the same material give or take its facets.
  {
    // A rounded box's volume is known exactly (Steiner): check against that too.
    auto roundedBox = [](double a, double b, double c, double r) {
      const double x = a - 2 * r, y = b - 2 * r, z = c - 2 * r;
      return x * y * z + 2 * (x * y + y * z + z * x) * r + kPi * (x + y + z) * r * r + 4.0 / 3.0 * kPi * r * r * r;
    };
    auto one = [&](const char* shape, const std::string& label, const std::function<void(Node&)>& setup, int wantTori,
                   int wantCylinders, double tolerance = 0.01) {
      Document doc;
      Node* n = doc.find(add(doc, shape, 0, 0, 10));
      setup(*n);
      const Result r = exportAndRead(doc, out + "/" + QString::fromStdString(label).replace(' ', '_') + ".step", label);
      check(r.report.exact == 1, label + ": exact");
      check(r.back.valid, label + ": a valid solid");
      if (wantTori >= 0) check(r.back.tori == wantTori, label + ": " + std::to_string(wantTori) + " torus faces");
      if (wantCylinders >= 0) check(r.back.cylinders == wantCylinders, label + ": " + std::to_string(wantCylinders) + " cylinder faces");
      check(std::abs(r.back.volume - r.meshVolume) < r.meshVolume * tolerance,
            label + ": the volume of the mesh (" + std::to_string(r.back.volume) + " vs " + std::to_string(r.meshVolume) + ")");
      return r;
    };
    {
      const Result r = one("box", "box all edges", [](Node& n) { n.size[0] = 30; n.size[1] = 20; n.size[2] = 10; n.edgeFillet = 2; }, 0, 12);
      check(r.back.spheres == 8 && std::abs(r.back.volume - roundedBox(30, 20, 10, 2)) < 1e-3, "box all edges: sphere corners, exact volume");
    }
    one("box", "box four edges", [](Node& n) {
      n.size[0] = 30; n.size[1] = 20; n.size[2] = 10; n.edgeFillet = 3; n.filletEdges = {0, 1, 2, 3};
    }, -1, -1);
    {
      const Result r = one("box", "box radius too big", [](Node& n) { n.size[0] = 30; n.size[1] = 20; n.size[2] = 4; n.edgeFillet = 9; }, 0, 12, 0.015);
      check(std::abs(r.back.volume - roundedBox(30, 20, 4, 1.92)) < 1e-3, "box radius too big: capped at 1.92 like the mesh, exact volume");
    }
    one("box", "stretched box", [](Node& n) {
      n.size[0] = 20; n.size[1] = 20; n.size[2] = 10; n.edgeFillet = 2; n.scale[0] = 2;
    }, 0, 12);
    one("cylinder", "cylinder", [](Node& n) { n.radius = 10; n.size[2] = 20; n.sides = 64; n.edgeFillet = 2; }, 2, 1);
    one("cylinder", "stretched cylinder", [](Node& n) {
      n.radius = 10; n.size[2] = 20; n.sides = 64; n.edgeFillet = 2; n.scale[0] = 1.5;
    }, -1, -1, 0.02);
    one("cone", "cone", [](Node& n) { n.radius = 12; n.radius2 = 5; n.size[2] = 20; n.edgeFillet = 2; }, 2, -1);
    one("tube", "tube", [](Node& n) { n.radius = 12; n.radius2 = 8; n.size[2] = 20; n.edgeFillet = 1; }, 4, 2);
    one("hemisphere", "dome", [](Node& n) { n.radius = 12; n.edgeFillet = 2; }, -1, -1);
    one("prism", "hex prism", [](Node& n) { n.radius = 10; n.sides = 6; n.size[2] = 15; n.edgeFillet = 1.5; }, -1, 18, 0.015);
    {
      const Result w = one("wedge", "wedge", [](Node& n) { n.size[0] = 30; n.size[1] = 20; n.size[2] = 15; n.edgeFillet = 1.5; }, -1, -1, 0.015);
      check(w.back.volume < 30 * 20 * 15 * 0.5 - 50, "wedge: its edges are rounded");
      const Result p = one("pyramid", "pyramid", [](Node& n) { n.size[0] = 30; n.size[1] = 20; n.size[2] = 15; n.edgeFillet = 1.5; }, -1, -1, 0.015);
      check(p.back.volume < 30 * 20 * 15 / 3.0 - 30, "pyramid: its edges are rounded");
    }
    // Picked edges: the top front edge filleted, the top back edge chamfered.
    one("box", "picked fillet and chamfer", [](Node& n) {
      n.size[0] = 30; n.size[1] = 20; n.size[2] = 10;
      ef::Pick round{{-15, -10, 5, 15, -10, 5}, 3, false};
      ef::Pick flat{{-15, 10, 5, 15, 10, 5}, 2, true};
      n.edgeBeta = ef::encodePicks({round, flat});
    }, 0, 1);
    // A pick on a big round edge: the mesh's 48-sided rim, the true circle in the exact solid.
    {
      const Result r = one("cylinder", "picked rim of a big cylinder", [](Node& n) {
        n.radius = 40; n.size[2] = 10; n.sides = 48;
        ef::Pick rim;
        for (int i = 0; i <= 48; ++i) {
          const double a = 2 * kPi * i / 48;
          rim.pts.insert(rim.pts.end(), {40 * std::cos(a), 40 * std::sin(a), 5});
        }
        rim.radius = 3;
        n.edgeBeta = ef::encodePicks({rim});
      }, 1, 1, 0.012);
      check(r.back.tori == 1, "picked rim: rounded with a torus");
    }
    // A group: two blocks in an L, the inside corner where they meet filleted (a concave edge).
    {
      Document doc;
      const QString base = add(doc, "box", 0, 0, 2.5);
      doc.find(base)->size[0] = 40; doc.find(base)->size[1] = 20; doc.find(base)->size[2] = 5;
      const QString wall = add(doc, "box", -17.5, 0, 12.5);
      doc.find(wall)->size[0] = 5; doc.find(wall)->size[1] = 20; doc.find(wall)->size[2] = 25;
      QString error;
      doc.groupSelected({base, wall}, &error);
      Node& group = doc.nodes.back();
      // The pick is in the group's own frame.
      const double gx = group.pos[0], gy = group.pos[1], gz = group.pos[2];
      ef::Pick inside{{-15 - gx, -10 - gy, 5 - gz, -15 - gx, 10 - gy, 5 - gz}, 4, false};
      group.edgeBeta = ef::encodePicks({inside});
      const Result r = exportAndRead(doc, out + "/bracket.step", "bracket with a filleted inside corner");
      check(r.report.exact == 1 && r.back.valid, "bracket: exact");
      check(r.back.cylinders == 1, "bracket: the inside fillet is a cylinder");
      check(std::abs(r.back.volume - r.meshVolume) < r.meshVolume * 0.01, "bracket: the volume of the mesh");
      check(r.back.volume > 40 * 20 * 5 + 5 * 20 * 20, "bracket: the fillet adds material");
    }
  }

  // 10. Text and 2D shapes from their curves: the font's Béziers, the 2D editor's nodes and handles.
  {
    auto textPart = [&](const QString& words, const std::string& label, double stretch, int mostFaces = 400) {
      Document doc;
      Node* n = doc.find(add(doc, "text", 0, 0, 2.5));
      n->text = words;
      n->fontSize = 12;
      n->size[2] = 5;
      n->scale[0] = stretch;
      const Result r = exportAndRead(doc, out + "/" + QString::fromStdString(label).replace(' ', '_') + ".step", label);
      check(r.report.exact == 1 && r.back.valid, label + ": exact and valid");
      check(r.back.other > 0, label + ": curved sides (not flat facets)");
      check(r.back.faces < mostFaces, label + ": a face per curve, not per facet (" + std::to_string(r.back.faces) + ")");
      check(std::abs(r.back.volume - r.meshVolume) < r.meshVolume * 0.004,
            label + ": the volume of the mesh (" + std::to_string(r.back.volume) + " vs " + std::to_string(r.meshVolume) + ")");
      return r;
    };
    textPart("Bob 8&4", "text with holes", 1);
    textPart("DaedalCAD", "stretched text", 1.5);
    textPart("ab\ncd", "two lines of text", 1);
    {
      QElapsedTimer timer;
      timer.start();
      textPart("The quick brown fox jumps over the lazy dog.\nPACK MY BOX WITH FIVE DOZEN LIQUOR JUGS 0123456789", "a long text", 1, 1600);
      std::printf("  a long text took %lld ms\n", static_cast<long long>(timer.elapsed()));
      check(timer.elapsed() < 30000, "a long text: done in reasonable time");
    }

    // A 2D-editor shape: a round disc (four curved nodes) with a square hole, on one layer, and a
    // second layer's triangle that sits apart. Written the way the editor writes its SVG.
    const QString svg = out + "/disc.svg";
    {
      const double k = 0.5522847498 * 10;
      struct N { double x, y, ix, iy, ox, oy; };
      const std::vector<N> disc = {{10, 0, 0, -k, 0, k}, {0, 10, k, 0, -k, 0}, {-10, 0, 0, k, 0, -k}, {0, -10, -k, 0, k, 0}};
      const std::vector<N> square = {{-3, -3, 0, 0, 0, 0}, {3, -3, 0, 0, 0, 0}, {3, 3, 0, 0, 0, 0}, {-3, 3, 0, 0, 0, 0}};
      const std::vector<N> triangle = {{14, -5, 0, 0, 0, 0}, {24, -5, 0, 0, 0, 0}, {19, 5, 0, 0, 0, 0}};
      auto record = [](const char* kind, int layer, const std::vector<N>& nodes) {
        QString r = QStringLiteral("%1%2c%2:").arg(QLatin1String(kind)).arg(layer);
        for (size_t i = 0; i < nodes.size(); ++i) {
          if (i) r += QLatin1Char(';');
          r += QStringLiteral("%1,%2,%3,%4,%5,%6").arg(nodes[i].x, 0, 'f', 4).arg(nodes[i].y, 0, 'f', 4).arg(nodes[i].ix, 0, 'f', 4)
                   .arg(nodes[i].iy, 0, 'f', 4).arg(nodes[i].ox, 0, 'f', 4).arg(nodes[i].oy, 0, 'f', 4);
        }
        return r;
      };
      auto polygon = [](const std::vector<N>& nodes) {  // as sampleShape2D, 24 steps a curve
        QString pts;
        for (size_t i = 0; i < nodes.size(); ++i) {
          const N& a = nodes[i];
          const N& b = nodes[(i + 1) % nodes.size()];
          const bool curved = std::abs(a.ox) + std::abs(a.oy) + std::abs(b.ix) + std::abs(b.iy) > 1e-6;
          const int steps = curved ? 24 : 1;
          for (int s = i == 0 ? 0 : 1; s <= steps; ++s) {
            const double t = double(s) / steps, u = 1 - t;
            const double x = u * u * u * a.x + 3 * u * u * t * (a.x + a.ox) + 3 * u * t * t * (b.x + b.ix) + t * t * t * b.x;
            const double y = u * u * u * a.y + 3 * u * u * t * (a.y + a.oy) + 3 * u * t * t * (b.y + b.iy) + t * t * t * b.y;
            pts += QStringLiteral("%1,%2 ").arg(x, 0, 'f', 4).arg(-y, 0, 'f', 4);
          }
        }
        return pts.trimmed();
      };
      QFile file(svg);
      file.open(QIODevice::WriteOnly | QIODevice::Text);
      const QString records = record("s", 1, disc) + "|" + record("h", 1, square) + "|" + record("s", 2, triangle);
      file.write(QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" data-center=\"7.0000,0.0000\">\n"
                                "  <path d=\"\" data-daedalcad-contours=\"%1\"/>\n"
                                "  <polygon points=\"%2\" data-layer=\"1\"/>\n"
                                "  <polygon points=\"%3\" data-hole=\"1\" data-layer=\"1\"/>\n"
                                "  <polygon points=\"%4\" data-layer=\"2\"/>\n</svg>\n")
                     .arg(records, polygon(disc), polygon(square), polygon(triangle))
                     .toUtf8());
    }
    Document doc;
    Node* n = doc.find(add(doc, "svg", 0, 0, 2));
    n->meshPath = svg;
    n->size[2] = 4;
    const Result r = exportAndRead(doc, out + "/disc.step", "2D shape with curves");
    check(r.report.exact == 1 && r.back.valid, "2D shape: exact and valid");
    check(r.back.solids == 2, "2D shape: the disc and the triangle apart");
    // The disc of four Béziers (radius 10, k 0.5523) is 0.027% bigger than a circle; less the hole.
    const double expected = (kPi * 100 * 1.00027 - 36) * 4 + 0.5 * 10 * 10 * 4;
    check(std::abs(r.back.volume - expected) < expected * 2e-4,
          "2D shape: the volume of its curves (" + std::to_string(r.back.volume) + " vs " + std::to_string(expected) + ")");
    check(std::abs(r.back.volume - r.meshVolume) < r.meshVolume * 0.003, "2D shape: the volume of the mesh");
  }

  // 11. Threads: the ISO tooth swept along a helix, on a bolt and in a tapped hole.
  {
    auto threaded = [&](double radius, double length, bool hole, const std::string& label, double pitch = 0) {
      Document doc;
      QString id = add(doc, "cylinder", 0, 0, length * 0.5);
      Node* n = doc.find(id);
      n->radius = radius;
      n->size[2] = length;
      n->sides = 64;
      n->thread = QStringLiteral("M");
      n->threadPitch = pitch;
      if (hole) {
        n->hole = true;
        n->threadClearance = 0.2;
        const QString block = add(doc, "box", 0, 0, length * 0.5);
        doc.find(block)->size[0] = doc.find(block)->size[1] = radius * 4;
        doc.find(block)->size[2] = length;
        QString error;
        doc.groupSelected({block, id}, &error);
      }
      QElapsedTimer timer;
      timer.start();
      const Result r = exportAndRead(doc, out + "/" + QString::fromStdString(label).replace(' ', '_') + ".step", label);
      std::printf("  %s took %lld ms\n", label.c_str(), static_cast<long long>(timer.elapsed()));
      check(r.report.exact == 1 && r.back.valid, label + ": exact and valid");
      check(r.back.faces < 8 * length / std::max(pitch, 0.5) + 40, label + ": a few smooth faces a turn (" + std::to_string(r.back.faces) + ")");
      check(std::abs(r.back.volume - r.meshVolume) < r.meshVolume * 0.006,
            label + ": the volume of the mesh (" + std::to_string(r.back.volume) + " vs " + std::to_string(r.meshVolume) + ")");
      return r;
    };
    threaded(4, 20, false, "M8 bolt");
    threaded(1.5, 40, false, "long M3 rod");
    threaded(4, 12, true, "M8 tapped hole");
    threaded(10, 15, false, "M20 fine", 1.5);
  }

  // 12. Bends and twists: smooth spline surfaces through the mesh's own warp.
  {
    auto warpedPart = [&](const char* shape, const std::string& label, const std::function<void(Node&)>& setup, double tolerance) {
      Document doc;
      Node* n = doc.find(add(doc, shape, 0, 0, 10));
      setup(*n);
      QElapsedTimer timer;
      timer.start();
      const Result r = exportAndRead(doc, out + "/" + QString::fromStdString(label).replace(' ', '_') + ".step", label);
      std::printf("  %s took %lld ms\n", label.c_str(), static_cast<long long>(timer.elapsed()));
      check(r.report.exact == 1 && r.back.valid, label + ": exact and valid");
      check(r.back.faces < 60, label + ": a few smooth faces (" + std::to_string(r.back.faces) + ")");
      check(std::abs(r.back.volume - r.meshVolume) < r.meshVolume * tolerance,
            label + ": the volume of the mesh (" + std::to_string(r.back.volume) + " vs " + std::to_string(r.meshVolume) + ")");
    };
    warpedPart("box", "bent bar", [](Node& n) { n.size[0] = 60; n.size[1] = 10; n.size[2] = 5; n.bend = 90; n.along = 0; n.toward = 2; }, 0.01);
    warpedPart("box", "twisted bar", [](Node& n) { n.size[0] = 8; n.size[1] = 20; n.size[2] = 50; n.twist = 120; n.twistAxis = 2; }, 0.01);
    warpedPart("cylinder", "bent rod", [](Node& n) { n.radius = 4; n.size[2] = 40; n.sides = 64; n.bend = 60; n.along = 2; n.toward = 0; }, 0.01);
    warpedPart("box", "twisted and bent", [](Node& n) {
      n.size[0] = 10; n.size[1] = 6; n.size[2] = 60; n.twist = 90; n.twistAxis = 2; n.bend = 45; n.along = 2; n.toward = 0;
    }, 0.015);
    warpedPart("box", "rounded twisted bar", [](Node& n) { n.size[0] = 10; n.size[1] = 10; n.size[2] = 40; n.edgeFillet = 1.5; n.twist = 90; n.twistAxis = 2; }, 0.01);
  }

  // 13. Recognition: the cylinders and cones in a generator's mesh and in an imported STL.
  {
    Document doc;
    Node* g = doc.find(doc.addShape("generator", 0, 0));
    g->genType = "gridbin";
    g->genParams = gen::defaults("gridbin");
    const Result r = exportAndRead(doc, out + "/gridbin.step", "Gridfinity bin recognised");
    check(r.report.partly + r.report.exact == 1 && r.back.valid, "Gridfinity bin: (partly) exact and valid");
    check(r.back.cylinders >= 20 && r.back.cones >= 4, "Gridfinity bin: its rounded corners as cylinders and cones");
    check(std::abs(r.back.volume - r.meshVolume) < r.meshVolume * 0.005, "Gridfinity bin: the volume of the mesh");

    // A tube as an STL from elsewhere: 64-sided rings, outer radius 10, inner 6, 20 tall.
    const QString stl = out + "/tube.stl";
    {
      std::vector<float> tris;
      const int n = 64;
      auto at = [&](double r, int i, double z) {
        const double a = 2 * kPi * i / n;
        return std::array<float, 3>{float(r * std::cos(a)), float(r * std::sin(a)), float(z)};
      };
      auto tri = [&](std::array<float, 3> a, std::array<float, 3> b, std::array<float, 3> c) {
        tris.insert(tris.end(), {a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]});
      };
      for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        tri(at(10, i, 0), at(10, j, 0), at(10, j, 20)), tri(at(10, i, 0), at(10, j, 20), at(10, i, 20));  // outside
        tri(at(6, j, 0), at(6, i, 0), at(6, i, 20)), tri(at(6, j, 0), at(6, i, 20), at(6, j, 20));        // inside
        tri(at(6, i, 20), at(10, i, 20), at(10, j, 20)), tri(at(6, i, 20), at(10, j, 20), at(6, j, 20));  // top
        tri(at(6, i, 0), at(10, j, 0), at(10, i, 0)), tri(at(6, i, 0), at(6, j, 0), at(10, j, 0));        // bottom
      }
      QFile file(stl);
      file.open(QIODevice::WriteOnly);
      file.write(QByteArray(80, 0));
      const quint32 count = static_cast<quint32>(tris.size() / 9);
      file.write(reinterpret_cast<const char*>(&count), 4);
      const float normal[3] = {0, 0, 0};
      for (size_t i = 0; i < tris.size(); i += 9) {
        file.write(reinterpret_cast<const char*>(normal), 12);
        file.write(reinterpret_cast<const char*>(&tris[i]), 36);
        file.write("\0\0", 2);
      }
    }
    Document tube;
    Node* t = tube.find(tube.addShape("stl", 0, 0));
    t->meshPath = stl;
    t->pos[2] = 10;
    const Result u = exportAndRead(tube, out + "/tube.step", "STL tube recognised");
    check(u.report.exact == 1 && u.back.valid, "STL tube: every curve recognised, so exact; valid");
    check(u.back.cylinders == 2 && u.back.faces <= 6, "STL tube: two true cylinders, a handful of faces (" + std::to_string(u.back.faces) + ")");
    const double exactTube = kPi * (100 - 36) * 20;
    check(std::abs(u.back.volume - exactTube) < exactTube * 1e-4,
          "STL tube: the volume of the true tube (" + std::to_string(u.back.volume) + " vs " + std::to_string(exactTube) + ")");
  }

  std::printf(failures ? "%d failure(s)\n" : "all exact STEP checks passed\n", failures);
  return failures ? 1 : 0;
}
