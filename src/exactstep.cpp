#include "exactstep.h"

#ifndef DAEDALCAD_WITH_OCCT

namespace exact {
bool available() { return false; }
bool writeStep(const Document&, const QStringList&, const std::vector<ExportObject>&, const QString&, QString* error, Report*) {
  if (error) *error = QStringLiteral("This copy of DaedalCAD was built without OpenCASCADE.");
  return false;
}
bool readStep(const QString&, std::vector<ImportedPart>*, QString* error) {
  if (error) *error = QStringLiteral("This copy of DaedalCAD was built without OpenCASCADE.");
  return false;
}
}  // namespace exact

#else

#include "edgefillet.h"
#include "generators.h"
#include "shape2d.h"
#include "stepexport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeCircle.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_NurbsConvert.hxx>
#include <BRepTools_Modification.hxx>
#include <BRepTools_Modifier.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <BRepLib.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <gp_Lin2d.hxx>
#include <Geom_BezierCurve.hxx>
#include <Geom2d_Line.hxx>
#include <NCollection_Array1.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <Precision.hxx>
#include <Interface_Static.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <Poly_Triangulation.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <TDF_ChildIterator.hxx>
#include <NCollection_List.hxx>
#include <NCollection_Sequence.hxx>
#include <TDF_Label.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>

namespace exact {

bool available() { return true; }

namespace {
// OpenCASCADE 8 made Standard_Failure a std::exception.
const char* failureText(const Standard_Failure& failure) {
#if OCC_VERSION_HEX >= 0x080000
  return failure.what();
#else
  return failure.GetMessageString();
#endif
}
}  // namespace

namespace {
// OpenCASCADE prints transfer statistics to the console; DaedalCAD reports in its own words.
void quiet() {
  static std::once_flag once;
  std::call_once(once, [] {
    const Handle(Message_Messenger)& messenger = Message::DefaultMessenger();
    messenger->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
  });
}
}  // namespace

namespace {

constexpr double kPi = 3.14159265358979323846;
const double kZero[3] = {0, 0, 0};

// Above this many flat faces, a mesh piece makes the exact booleans too slow: the whole part is
// written from its mesh instead.
constexpr int kMostMeshFaces = 20000;

// --- Building blocks ---------------------------------------------------------------------------

// A solid from flat faces (see step::analyseSolid), its edges shared.
TopoDS_Shape facetedSolid(const step::Faceted& model) {
  std::vector<TopoDS_Vertex> vertices;
  vertices.reserve(model.points.size());
  for (const auto& p : model.points) vertices.push_back(BRepBuilderAPI_MakeVertex(gp_Pnt(p[0], p[1], p[2])));
  std::vector<TopoDS_Edge> edges;
  edges.reserve(model.edges.size());
  for (const auto& e : model.edges) {
    // Corners closer than OpenCASCADE's tolerance (a mesh can have them) make no edge: then this
    // mesh cannot be a solid here, and the caller uses its other way.
    BRepBuilderAPI_MakeEdge edge(vertices[static_cast<size_t>(e.a)], vertices[static_cast<size_t>(e.b)]);
    if (!edge.IsDone()) return {};
    edges.push_back(edge.Edge());
  }
  BRep_Builder builder;
  TopoDS_Shell shell;
  builder.MakeShell(shell);
  for (const auto& face : model.faces) {
    const gp_Pln plane(gp_Ax3(gp_Pnt(face.origin[0], face.origin[1], face.origin[2]), gp_Dir(face.normal[0], face.normal[1], face.normal[2]),
                              gp_Dir(face.u[0], face.u[1], face.u[2])));
    std::vector<TopoDS_Wire> wires;
    for (const auto& bound : face.bounds) {
      BRepBuilderAPI_MakeWire wire;
      for (const auto& [index, forward] : bound.edges) {
        const TopoDS_Edge& edge = edges[static_cast<size_t>(index)];
        wire.Add(forward ? edge : TopoDS::Edge(edge.Reversed()));
      }
      if (!wire.IsDone() || wire.Wire().IsNull()) return {};
      wires.push_back(wire.Wire());
    }
    // Built directly: the outline already runs anticlockwise round the outward normal and the
    // holes the other way (analyseSolid), so there is nothing for MakeFace's inside check to find,
    // which on a mesh of tens of thousands of faces takes minutes.
    TopoDS_Face made;
    builder.MakeFace(made, new Geom_Plane(plane), Precision::Confusion());
    for (const TopoDS_Wire& w : wires) builder.Add(made, w);
    builder.Add(shell, made);
  }
  shell.Closed(true);
  TopoDS_Solid solid;
  builder.MakeSolid(solid);
  builder.Add(solid, shell);
  return solid;
}

TopoDS_Shape facetedSolid(const std::vector<double>& points, const std::vector<uint32_t>& triangles, int* faces, QString* why) {
  step::Faceted model;
  if (!step::analyseSolid(points, triangles, &model, why)) return {};
  if (faces) *faces = static_cast<int>(model.faces.size());
  return facetedSolid(model);
}

gp_Trsf rotationXYZ(const double degrees[3]) {
  // X first, then Y, then Z, as Manifold's Rotate and the view's placement.
  gp_Trsf x, y, z;
  x.SetRotation(gp_Ax1(gp::Origin(), gp::DX()), degrees[0] * kPi / 180);
  y.SetRotation(gp_Ax1(gp::Origin(), gp::DY()), degrees[1] * kPi / 180);
  z.SetRotation(gp_Ax1(gp::Origin(), gp::DZ()), degrees[2] * kPi / 180);
  return z.Multiplied(y).Multiplied(x);
}

// Where a placed part goes: turned by rot, then by extra, then moved to pos.
gp_Trsf rigid(const double pos[3], const double rot[3], const double extra[3]) {
  gp_Trsf move;
  move.SetTranslation(gp_Vec(pos[0], pos[1], pos[2]));
  return move.Multiplied(rotationXYZ(extra)).Multiplied(rotationXYZ(rot));
}

TopoDS_Shape moved(const TopoDS_Shape& shape, const gp_Trsf& trsf) {
  if (shape.IsNull()) return shape;
  return BRepBuilderAPI_Transform(shape, trsf, true).Shape();
}

bool same(double a, double b) { return std::abs(std::abs(a) - std::abs(b)) < 1e-9; }

// Scaled about the origin: a plain (uniform) scale and mirrors where it can, a general one
// (which turns surfaces into splines) where the axes differ.
TopoDS_Shape scaled(const TopoDS_Shape& shape, const double s[3]) {
  if (shape.IsNull()) return shape;
  TopoDS_Shape out = shape;
  if (same(s[0], s[1]) && same(s[1], s[2])) {
    if (std::abs(std::abs(s[0]) - 1) > 1e-12) {
      gp_Trsf t;
      t.SetScale(gp::Origin(), std::abs(s[0]));
      out = moved(out, t);
    }
  } else {
    gp_GTrsf g;
    g.SetValue(1, 1, std::abs(s[0]));
    g.SetValue(2, 2, std::abs(s[1]));
    g.SetValue(3, 3, std::abs(s[2]));
    out = BRepBuilderAPI_GTransform(out, g, true).Shape();
  }
  for (int axis = 0; axis < 3; ++axis) {
    if (s[axis] >= 0) continue;
    gp_Trsf m;
    m.SetMirror(gp_Ax2(gp::Origin(), axis == 0 ? gp::DX() : axis == 1 ? gp::DY() : gp::DZ()));
    out = moved(out, m);
  }
  return out;
}

TopoDS_Shape boolean(int op, const std::vector<TopoDS_Shape>& args, const std::vector<TopoDS_Shape>& tools) {
  NCollection_List<TopoDS_Shape> a, t;
  for (const auto& s : args) a.Append(s);
  for (const auto& s : tools) t.Append(s);
  auto run = [&](BRepAlgoAPI_BooleanOperation& algo) -> TopoDS_Shape {
    algo.SetArguments(a);
    algo.SetTools(t);
    algo.SetFuzzyValue(1e-6);
    algo.SetRunParallel(true);
    algo.SetNonDestructive(true);
    algo.Build();
    if (!algo.IsDone() || algo.HasErrors()) return {};
    return algo.Shape();
  };
  if (op == 0) {
    BRepAlgoAPI_Fuse fuse;
    return run(fuse);
  }
  if (op == 1) {
    BRepAlgoAPI_Cut cut;
    return run(cut);
  }
  BRepAlgoAPI_Common common;
  return run(common);
}

TopoDS_Shape fuseAll(const std::vector<TopoDS_Shape>& shapes) {
  if (shapes.empty()) return {};
  if (shapes.size() == 1) return shapes.front();
  return boolean(0, {shapes.front()}, std::vector<TopoDS_Shape>(shapes.begin() + 1, shapes.end()));
}

bool solidIn(const TopoDS_Shape& shape) {
  if (shape.IsNull()) return false;
  TopExp_Explorer e(shape, TopAbs_SOLID);
  return e.More();
}

int splineFaces(const TopoDS_Shape& shape) {
  int count = 0;
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    const GeomAbs_SurfaceType type = BRepAdaptor_Surface(TopoDS::Face(e.Current())).GetType();
    if (type == GeomAbs_BSplineSurface || type == GeomAbs_BezierSurface || type == GeomAbs_OffsetSurface) ++count;
  }
  return count;
}


// --- STEP files read back for parts that came from one ----------------------------------------

struct StepPart {
  QString name;
  QColor color;
  bool hasColor = false;
  TopoDS_Shape shape;  // in the file's coordinates, millimetres
};

bool loadStep(const QString& path, std::vector<StepPart>* parts, QString* error);

const TopoDS_Shape* cachedStepSolid(const QString& path, int index) {
  static std::mutex mutex;
  static std::map<QString, std::pair<qint64, std::vector<StepPart>>> files;
  std::lock_guard<std::mutex> lock(mutex);
  const qint64 stamp = QFileInfo(path).lastModified().toMSecsSinceEpoch();
  auto it = files.find(path);
  if (it == files.end() || it->second.first != stamp) {
    std::vector<StepPart> parts;
    QString error;
    if (!loadStep(path, &parts, &error)) return nullptr;
    it = files.insert_or_assign(path, std::make_pair(stamp, std::move(parts))).first;
  }
  if (index < 0 || index >= static_cast<int>(it->second.second.size())) return nullptr;
  return &it->second.second[static_cast<size_t>(index)].shape;
}

// --- The design, exactly -----------------------------------------------------------------------

struct XParts {
  TopoDS_Shape solid, hole;
  bool hasSolid = false, hasHole = false;
};

// --- Flat shapes from curves: text and the 2D editor's shapes ----------------------------------

// A closed outline of straight lines and cubic Béziers in the XY plane, in mm. A line uses its
// first and last point.
struct Outline {
  std::vector<TextSegment> segments;
  std::vector<std::pair<double, double>> points;  // a fine polygon of it, for areas and insides
};

void sampleOutline(Outline* outline) {
  outline->points.clear();
  for (const TextSegment& s : outline->segments) {
    const int steps = s.cubic ? 16 : 1;
    for (int i = 0; i < steps; ++i) {
      const double t = static_cast<double>(i) / steps, u = 1 - t;
      const double b[4] = {u * u * u, 3 * u * u * t, 3 * u * t * t, t * t * t};
      if (!s.cubic) outline->points.emplace_back(s.x[0], s.y[0]);
      else outline->points.emplace_back(b[0] * s.x[0] + b[1] * s.x[1] + b[2] * s.x[2] + b[3] * s.x[3],
                                        b[0] * s.y[0] + b[1] * s.y[1] + b[2] * s.y[2] + b[3] * s.y[3]);
    }
  }
}

double outlineArea(const Outline& outline) {
  double area = 0;
  const auto& p = outline.points;
  for (size_t i = 0; i < p.size(); ++i) {
    const auto& a = p[i];
    const auto& b = p[(i + 1) % p.size()];
    area += a.first * b.second - b.first * a.second;
  }
  return area * 0.5;
}

bool insideOutline(const Outline& outline, double x, double y) {
  bool in = false;
  const auto& p = outline.points;
  for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
    if ((p[i].second > y) != (p[j].second > y) &&
        x < (p[j].first - p[i].first) * (y - p[i].second) / (p[j].second - p[i].second) + p[i].first)
      in = !in;
  }
  return in;
}

// The outline as a wire at height z0, anticlockwise seen from above (or clockwise: a hole's).
TopoDS_Wire outlineWire(const Outline& outline, double z0, bool anticlockwise) {
  const size_t n = outline.segments.size();
  if (n == 0) return {};
  std::vector<TopoDS_Vertex> corners;
  for (const TextSegment& s : outline.segments) corners.push_back(BRepBuilderAPI_MakeVertex(gp_Pnt(s.x[0], s.y[0], z0)));
  BRepBuilderAPI_MakeWire wire;
  for (size_t i = 0; i < n; ++i) {
    const TextSegment& s = outline.segments[i];
    const TopoDS_Vertex& from = corners[i];
    const TopoDS_Vertex& to = corners[(i + 1) % n];
    TopoDS_Edge edge;
    if (s.cubic) {
      NCollection_Array1<gp_Pnt> poles(1, 4);
      for (int k = 0; k < 4; ++k) poles.SetValue(k + 1, gp_Pnt(s.x[k], s.y[k], z0));
      // The last pole is the next segment's start exactly, so the curve meets its corner.
      const TextSegment& next = outline.segments[(i + 1) % n];
      poles.SetValue(4, gp_Pnt(next.x[0], next.y[0], z0));
      BRepBuilderAPI_MakeEdge make(new Geom_BezierCurve(poles), from, to);
      if (!make.IsDone()) return {};
      edge = make.Edge();
    } else {
      BRepBuilderAPI_MakeEdge make(from, to);
      if (!make.IsDone()) return {};
      edge = make.Edge();
    }
    wire.Add(edge);
    if (!wire.IsDone()) return {};
  }
  TopoDS_Wire w = wire.Wire();
  if ((outlineArea(outline) > 0) != anticlockwise) w = TopoDS::Wire(w.Reversed());
  return w;
}

// The outline (less `holes` inside it, when given) as a face at height z0, pushed up by `height`
// into a solid. Null when the face does not check out (a hole crossing the outline, say).
TopoDS_Shape outlinePrism(const Outline& outline, double z0, double height, const std::vector<const Outline*>& holes = {}) {
  const TopoDS_Wire outer = outlineWire(outline, z0, true);
  if (outer.IsNull()) return {};
  BRepBuilderAPI_MakeFace face(gp_Pln(gp_Pnt(0, 0, z0), gp::DZ()), outer, true);
  if (!face.IsDone()) return {};
  for (const Outline* hole : holes) {
    const TopoDS_Wire inner = outlineWire(*hole, z0, false);
    if (inner.IsNull()) return {};
    face.Add(inner);
  }
  if (!face.IsDone()) return {};
  if (!holes.empty() && !BRepCheck_Analyzer(face.Face()).IsValid()) return {};
  BRepPrimAPI_MakePrism prism(face.Face(), gp_Vec(0, 0, height));
  if (!prism.IsDone()) return {};
  return prism.Shape();
}

// Text: the font's outlines, filled the way the mesh fills them (non-zero: a hole belongs to the
// smallest outline round it, overlapping strokes join), centred on the outline box like the mesh,
// as tall as the part, centred on z.
TopoDS_Shape textExact(const Node& n, const double stretch[3], QString* why) {
  std::vector<TextLoop> loops;
  std::vector<TextCurveLoop> curves;
  QString note;
  if (!textOutlines(textSpecFor(n), &loops, &note, &curves) || curves.size() != loops.size()) {
    *why = note.isEmpty() ? QStringLiteral("its outlines could not be read") : note;
    return {};
  }
  std::vector<Outline> outlines(curves.size());
  std::vector<double> areas(curves.size());
  size_t biggest = 0;
  for (size_t i = 0; i < curves.size(); ++i) {
    outlines[i].segments = curves[i];
    sampleOutline(&outlines[i]);
    areas[i] = outlineArea(outlines[i]);
    if (std::abs(areas[i]) > std::abs(areas[biggest])) biggest = i;
  }
  const bool outerSign = areas[biggest] > 0;
  // Centred on the box of the flattened outlines, as the mesh is.
  double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
  for (size_t i = 0; i < loops.size(); ++i) {
    if ((areas[i] > 0) != outerSign) continue;
    for (size_t k = 0; k + 1 < loops[i].size(); k += 2) {
      lo[0] = std::min(lo[0], loops[i][k]), hi[0] = std::max(hi[0], loops[i][k]);
      lo[1] = std::min(lo[1], loops[i][k + 1]), hi[1] = std::max(hi[1], loops[i][k + 1]);
    }
  }
  // Centred, then stretched by the part's scale: a Bézier stretched is its control points stretched,
  // so the curves stay exact and the flat faces flat.
  const double cx = (lo[0] + hi[0]) * 0.5, cy = (lo[1] + hi[1]) * 0.5;
  for (Outline& o : outlines) {
    for (TextSegment& s : o.segments)
      for (int k = 0; k < 4; ++k) s.x[k] = (s.x[k] - cx) * stretch[0], s.y[k] = (s.y[k] - cy) * stretch[1];
    for (auto& p : o.points) p.first = (p.first - cx) * stretch[0], p.second = (p.second - cy) * stretch[1];
  }
  for (double& a : areas) a *= stretch[0] * stretch[1];
  const double depth = std::max(n.size[2], 0.05) * stretch[2];
  std::vector<std::vector<size_t>> holesOf(outlines.size());
  for (size_t h = 0; h < outlines.size(); ++h) {
    if ((areas[h] > 0) == outerSign) continue;
    size_t owner = outlines.size();
    for (size_t o = 0; o < outlines.size(); ++o) {
      if ((areas[o] > 0) != outerSign) continue;
      if (!insideOutline(outlines[o], outlines[h].points[0].first, outlines[h].points[0].second)) continue;
      if (owner == outlines.size() || std::abs(areas[o]) < std::abs(areas[owner])) owner = o;
    }
    if (owner < outlines.size()) holesOf[owner].push_back(h);
  }
  std::vector<TopoDS_Shape> pieces;
  for (size_t o = 0; o < outlines.size(); ++o) {
    if ((areas[o] > 0) != outerSign) continue;
    // Usually a letter's holes lie well inside it: one face with them as inner outlines.
    std::vector<const Outline*> inner;
    for (size_t h : holesOf[o]) inner.push_back(&outlines[h]);
    if (!inner.empty()) {
      const TopoDS_Shape direct = outlinePrism(outlines[o], -depth * 0.5, depth, inner);
      if (!direct.IsNull() && BRepCheck_Analyzer(direct).IsValid()) {
        pieces.push_back(direct);
        continue;
      }
    }
    TopoDS_Shape piece = outlinePrism(outlines[o], -depth * 0.5, depth);
    if (piece.IsNull()) {
      *why = QStringLiteral("a letter's outline could not be made");
      return {};
    }
    std::vector<TopoDS_Shape> holes;
    for (size_t h : holesOf[o]) {
      // A hole a little taller, so its top and bottom are not flush with the letter's.
      const TopoDS_Shape hole = outlinePrism(outlines[h], -depth * 0.5 - 1, depth + 2);
      if (hole.IsNull()) {
        *why = QStringLiteral("a hole in a letter could not be made");
        return {};
      }
      holes.push_back(hole);
    }
    if (!holes.empty()) piece = boolean(1, {piece}, holes);
    if (!solidIn(piece)) {
      *why = QStringLiteral("a letter's holes could not be cut");
      return {};
    }
    pieces.push_back(piece);
  }
  TopoDS_Shape all = fuseAll(pieces);
  if (!solidIn(all)) {
    *why = QStringLiteral("the letters could not be joined");
    return {};
  }
  ShapeUpgrade_UnifySameDomain unify(all, true, true, false);
  unify.Build();
  return unify.Shape().IsNull() ? all : unify.Shape();
}

// A shape from the 2D editor: its SVG keeps the exact outlines (nodes and curve handles) beside the
// flattened polygons the mesh is made from. Built like the mesh: per layer its shapes joined less
// its holes, the layers joined, moved by the file's centre, as tall as the part, centred on z.
TopoDS_Shape svgExact(const Node& n, const double stretch[3], QString* why) {
  QVector<Shape2DContour> contours;
  if (!readShape2DCurves(n.meshPath, &contours)) {
    *why = QFileInfo::exists(n.meshPath)
               ? QStringLiteral("this SVG has only its outline points (not made in DaedalCAD's 2D editor)")
               : QStringLiteral("its SVG file is missing (%1)").arg(QDir::toNativeSeparators(n.meshPath));
    return {};
  }
  QFile file(n.meshPath);
  QString text;
  if (file.open(QIODevice::ReadOnly | QIODevice::Text)) text = QString::fromUtf8(file.readAll());
  // The same centre as the mesh: the file's own, else (an older file) the middle of its outlines,
  // or none for a layer file.
  double cx = 0, cy = 0;
  const QRegularExpressionMatch centre =
      QRegularExpression(QStringLiteral(R"(data-center\s*=\s*["']([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)["'])")).match(text);
  std::vector<Outline> outlines;
  std::vector<bool> holes;
  std::vector<int> layerOf;
  auto zero = [](const QPointF& p) { return std::abs(p.x()) < 1e-6 && std::abs(p.y()) < 1e-6; };
  for (const Shape2DContour& contour : contours) {
    if (!contour.closed || contour.nodes.size() < 3) continue;
    Outline outline;
    const int count = contour.nodes.size();
    for (int i = 0; i < count; ++i) {
      const Shape2DNode& a = contour.nodes[i];
      const Shape2DNode& b = contour.nodes[(i + 1) % count];
      TextSegment s;
      s.cubic = !zero(a.outHandle) || !zero(b.inHandle);
      s.x[0] = a.pos.x(), s.y[0] = a.pos.y(), s.x[3] = b.pos.x(), s.y[3] = b.pos.y();
      s.x[1] = a.pos.x() + a.outHandle.x(), s.y[1] = a.pos.y() + a.outHandle.y();
      s.x[2] = b.pos.x() + b.inHandle.x(), s.y[2] = b.pos.y() + b.inHandle.y();
      if (!s.cubic && std::abs(s.x[0] - s.x[3]) < 1e-9 && std::abs(s.y[0] - s.y[3]) < 1e-9) continue;
      outline.segments.push_back(s);
    }
    if (outline.segments.size() < 2) continue;
    sampleOutline(&outline);
    outlines.push_back(outline);
    holes.push_back(contour.hole);
    layerOf.push_back(std::max(1, contour.cut > 0 ? contour.cut : contour.layer));
  }
  if (centre.hasMatch()) {
    cx = centre.captured(1).toDouble();
    cy = centre.captured(2).toDouble();
  } else if (!QFileInfo(n.meshPath).fileName().startsWith(QLatin1String("layer-"), Qt::CaseInsensitive)) {
    double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
    for (size_t i = 0; i < outlines.size(); ++i)
      if (!holes[i])
        for (const auto& p : outlines[i].points) {
          lo[0] = std::min(lo[0], p.first), hi[0] = std::max(hi[0], p.first);
          lo[1] = std::min(lo[1], p.second), hi[1] = std::max(hi[1], p.second);
        }
    if (hi[0] > lo[0]) cx = (lo[0] + hi[0]) * 0.5, cy = (lo[1] + hi[1]) * 0.5;
  }
  // Centred, then stretched through the control points, like the text.
  for (Outline& o : outlines) {
    for (TextSegment& s : o.segments)
      for (int k = 0; k < 4; ++k) s.x[k] = (s.x[k] - cx) * stretch[0], s.y[k] = (s.y[k] - cy) * stretch[1];
    for (auto& p : o.points) p.first = (p.first - cx) * stretch[0], p.second = (p.second - cy) * stretch[1];
  }
  const double depth = std::max(n.size[2], 0.05) * stretch[2];
  std::map<int, std::pair<std::vector<TopoDS_Shape>, std::vector<TopoDS_Shape>>> layers;  // shapes, holes
  for (size_t i = 0; i < outlines.size(); ++i) {
    const TopoDS_Shape prism =
        holes[i] ? outlinePrism(outlines[i], -depth * 0.5 - 1, depth + 2) : outlinePrism(outlines[i], -depth * 0.5, depth);
    if (prism.IsNull()) {
      *why = QStringLiteral("an outline could not be made");
      return {};
    }
    auto& layer = layers[layerOf[i]];
    (holes[i] ? layer.second : layer.first).push_back(prism);
  }
  std::vector<TopoDS_Shape> joined;
  for (auto& [number, layer] : layers) {
    if (layer.first.empty()) continue;
    TopoDS_Shape shape = fuseAll(layer.first);
    if (!layer.second.empty()) shape = boolean(1, {shape}, layer.second);
    if (solidIn(shape)) joined.push_back(shape);
  }
  if (joined.empty()) {
    *why = QStringLiteral("nothing solid is left of its outlines");
    return {};
  }
  TopoDS_Shape all = fuseAll(joined);
  if (!solidIn(all)) {
    *why = QStringLiteral("its layers could not be joined");
    return {};
  }
  ShapeUpgrade_UnifySameDomain unify(all, true, true, false);
  unify.Build();
  return unify.Shape().IsNull() ? all : unify.Shape();
}

// --- Threads ---------------------------------------------------------------------------------

// The ISO thread the mesh makes (generators.cpp, isoThread and threadedRod): radius `major`, a
// tooth `depth` deep with flat crests and roots, right handed, its crest centred at phaseZ.
struct Thread {
  double major = 5, depth = 0.5, pitch = 1, crest = 0.125, root = 0.25;
};

// A closed polyline in the XZ plane (x the radius), as a wire.
TopoDS_Wire profileWire(const std::vector<std::pair<double, double>>& rz) {
  BRepBuilderAPI_MakeWire wire;
  for (size_t i = 0; i < rz.size(); ++i) {
    const auto& a = rz[i];
    const auto& b = rz[(i + 1) % rz.size()];
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(a.first, 0, a.second), gp_Pnt(b.first, 0, b.second)));
  }
  return wire.IsDone() ? wire.Wire() : TopoDS_Wire();
}

// A solid of revolution about Z from a closed (radius, z) outline.
TopoDS_Shape revolved(const std::vector<std::pair<double, double>>& rz) {
  const TopoDS_Wire wire = profileWire(rz);
  if (wire.IsNull()) return {};
  BRepBuilderAPI_MakeFace face(wire, true);
  if (!face.IsDone()) return {};
  BRepPrimAPI_MakeRevol revol(face.Face(), gp_Ax1(gp::Origin(), gp::DZ()), 2 * kPi);
  return revol.IsDone() ? revol.Shape() : TopoDS_Shape();
}

// A threaded rod from z0 to z1, its radius grown by `grow` (a nut's or a tapped hole's clearance):
// one turn of the tooth swept along a helix, copied up the rod a pitch at a time and joined to the
// core. (One long sweep would be simpler, but OpenCASCADE does not reliably trace where a core
// meets a tooth that winds round dozens of times; it does for one turn.)
constexpr int kMostThreadTurns = 100;
TopoDS_Shape threadedRod(const Thread& form, double z0, double z1, double phaseZ, double grow = 0) {
  const double major = form.major + grow, minor = major - form.depth, p = form.pitch;
  // The first turn starts a turn below z0, on a crest; the copies run a turn past z1.
  const double start = phaseZ + (std::floor((z0 - phaseZ) / p) - 1) * p;
  const int turns = static_cast<int>(std::ceil((z1 - start) / p)) + 1;
  if (turns > kMostThreadTurns + 3) return {};
  Handle(Geom_CylindricalSurface) surface = new Geom_CylindricalSurface(gp_Ax3(gp::XOY()), minor);
  const gp_Lin2d line(gp_Pnt2d(0, start), gp_Dir2d(2 * kPi, p));
  // The segment of that line (GCE2d_MakeSegment is deprecated in OpenCASCADE 8; this is what it made).
  const Handle(Geom2d_TrimmedCurve) path = new Geom2d_TrimmedCurve(new Geom2d_Line(line), 0, std::hypot(2 * kPi, p));
  TopoDS_Edge helix = BRepBuilderAPI_MakeEdge(path, surface);
  BRepLib::BuildCurves3d(helix, 1e-7, GeomAbs_C2, 14, 32);
  // One tooth across the angle-0 plane, the flat crest at the outside. Its flanks run on into the
  // core a little, so the core's surface cuts through them (a tooth whose corner only touches
  // the core is a contact booleans do not handle reliably), but not so far that turns meet.
  const double flankFrom = (0.5 - form.root * 0.5) * p, crest = form.crest * 0.5 * p;
  const double slope = (flankFrom - crest) / form.depth;  // z per radius along a flank
  const double in = std::min(form.depth * 0.1, form.root * p * 0.4 / std::max(slope, 1e-6));
  const double inside = minor - in, reach = flankFrom + in * slope;
  const TopoDS_Wire tooth =
      profileWire({{inside, start - reach}, {major, start - crest}, {major, start + crest}, {inside, start + reach}});
  if (tooth.IsNull()) return {};
  BRepOffsetAPI_MakePipeShell pipe(BRepBuilderAPI_MakeWire(helix).Wire());
  pipe.SetMode(gp_Dir(0, 0, 1));  // the tooth stays upright, turning round the axis
  pipe.Add(tooth);
  pipe.Build();
  if (!pipe.IsDone() || !pipe.MakeSolid()) return {};
  std::vector<TopoDS_Shape> parts{BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, z0 - 1), gp::DZ()), minor, z1 - z0 + 2).Shape()};
  for (int k = 0; k < turns; ++k) {
    gp_Trsf up;
    up.SetTranslation(gp_Vec(0, 0, k * p));
    parts.push_back(moved(pipe.Shape(), up));
  }
  const TopoDS_Shape both = fuseAll(parts);
  const TopoDS_Shape span = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, z0), gp::DZ()), major + 1, z1 - z0).Shape();
  const TopoDS_Shape out = boolean(2, {both}, {span});
  return solidIn(out) ? out : TopoDS_Shape();
}

// A threaded cylinder as the mesh makes it (generators.cpp, threadedCylinder): a bolt shank with
// chamfered ends, or the cutter of a tapped hole with countersinks.
TopoDS_Shape threadExact(const Node& n, QString* why) {
  const double diameter = std::max(n.radius, 0.3) * 2;
  double pitch = n.threadPitch > 0 ? n.threadPitch : gen::coarsePitch(diameter);
  pitch = std::clamp(pitch, std::max(0.2, diameter / 60), std::max(0.3, diameter * 0.4));
  Thread form;
  form.major = diameter * 0.5;
  form.pitch = pitch;
  form.depth = 0.5413 * pitch;
  const double length = std::max(n.size[2], 0.5), hz = length * 0.5;
  TopoDS_Shape out;
  if (!n.hole) {
    out = threadedRod(form, -hz, hz, -hz);
    if (!out.IsNull() && length > 2 * form.depth + 1.6) {
      // boltShank: chamfered from just inside the root to 0.5 outside the crest at both ends.
      const double rMin = form.major - form.depth, edge = form.depth + 0.2, R = form.major + 0.5;
      const TopoDS_Shape chamfers = revolved({{0, -hz}, {rMin - 0.2, -hz}, {R, -hz + edge + 0.5}, {R, hz - edge - 0.5},
                                              {rMin - 0.2, hz}, {0, hz}});
      out = chamfers.IsNull() ? TopoDS_Shape() : boolean(2, {out}, {chamfers});
    }
  } else {
    const double clearance = std::clamp(n.threadClearance, 0.0, 2.0);
    TopoDS_Shape rod = threadedRod(form, -hz - pitch, hz + pitch, -hz, clearance);
    if (!rod.IsNull()) {
      const TopoDS_Shape span = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -hz), gp::DZ()), form.major + clearance + 1, length).Shape();
      rod = boolean(2, {rod}, {span});
      const double outer = form.major + clearance + 0.3, inner = form.major - form.depth + clearance;
      const double sink = std::min(outer - inner, hz * 0.5);
      const TopoDS_Shape low = BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0, 0, -hz), gp::DZ()), outer, inner, sink).Shape();
      const TopoDS_Shape high = BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0, 0, hz - sink), gp::DZ()), inner, outer, sink).Shape();
      out = fuseAll({rod, low, high});
    }
  }
  if (!solidIn(out)) {
    const int turns = static_cast<int>(std::ceil(length / pitch));
    *why = turns > kMostThreadTurns ? QStringLiteral("%1 turns is more than an exact thread is made with (%2)").arg(turns).arg(kMostThreadTurns)
                                    : QStringLiteral("OpenCASCADE could not sweep the thread");
    return {};
  }
  return out;
}

// --- Bends and twists ---------------------------------------------------------------------------

// A warp of space (the mesh's bend or twist, in the frame the mesh warps in).
using Warp = std::function<gp_Pnt(const gp_Pnt&)>;

// Moves every control point of the solid's (spline) surfaces and edges through the warp, after
// raising them to cubic and splitting them finely enough that the result stays within `tolerance`
// of the warped shape: checked on a grid of each surface, and split further where it is not.
// The parameters of every surface and edge stay as they were, so the outlines of the faces (their
// curves in the surfaces' parameters) still fit.
class WarpModification : public BRepTools_Modification {
 public:
  WarpModification(Warp warp, double step, double tolerance) : warp_(std::move(warp)), step_(step), tolerance_(tolerance) {}
  double worst = 0;  // the largest distance from the warped shape found while checking

  bool NewSurface(const TopoDS_Face& F, Handle(Geom_Surface)& S, TopLoc_Location& L, double& Tol, bool& RevWires,
                  bool& RevFace) override {
    TopLoc_Location location;
    const Handle(Geom_BSplineSurface) source = Handle(Geom_BSplineSurface)::DownCast(BRep_Tool::Surface(F, location));
    if (source.IsNull()) return false;
    Handle(Geom_BSplineSurface) base = Handle(Geom_BSplineSurface)::DownCast(source->Transformed(location.Transformation()));
    double u0, u1, v0, v1;
    base->Bounds(u0, u1, v0, v1);
    // How many pieces each way: by the surface's size along each parameter.
    auto extent = [&](bool alongU) {
      double longest = 0;
      for (int i = 0; i <= 4; ++i) {
        const double fixed = (alongU ? v0 + (v1 - v0) * i / 4 : u0 + (u1 - u0) * i / 4);
        double length = 0;
        gp_Pnt last;
        for (int k = 0; k <= 16; ++k) {
          const double t = (alongU ? u0 + (u1 - u0) * k / 16 : v0 + (v1 - v0) * k / 16);
          const gp_Pnt p = alongU ? base->Value(t, fixed) : base->Value(fixed, t);
          if (k) length += p.Distance(last);
          last = p;
        }
        longest = std::max(longest, length);
      }
      return longest;
    };
    int nu = std::clamp(static_cast<int>(std::ceil(extent(true) / step_)), 1, 96);
    int nv = std::clamp(static_cast<int>(std::ceil(extent(false) / step_)), 1, 96);
    for (int attempt = 0; attempt < 4; ++attempt) {
      Handle(Geom_BSplineSurface) bent = Handle(Geom_BSplineSurface)::DownCast(base->Copy());
      bent->IncreaseDegree(std::max(3, bent->UDegree()), std::max(3, bent->VDegree()));
      for (int i = 1; i < nu; ++i) bent->InsertUKnot(u0 + (u1 - u0) * i / nu, 1, 1e-9);
      for (int j = 1; j < nv; ++j) bent->InsertVKnot(v0 + (v1 - v0) * j / nv, 1, 1e-9);
      for (int i = 1; i <= bent->NbUPoles(); ++i)
        for (int j = 1; j <= bent->NbVPoles(); ++j) bent->SetPole(i, j, warp_(bent->Pole(i, j)));
      double error = 0;
      for (int i = 0; i <= 2 * nu; ++i)
        for (int j = 0; j <= 2 * nv; ++j) {
          const double u = u0 + (u1 - u0) * i / (2 * nu), v = v0 + (v1 - v0) * j / (2 * nv);
          error = std::max(error, warp_(base->Value(u, v)).Distance(bent->Value(u, v)));
        }
      if (error <= tolerance_ || attempt == 3 || (nu >= 96 && nv >= 96)) {
        worst = std::max(worst, error);
        S = bent;
        L = TopLoc_Location();
        Tol = std::max(BRep_Tool::Tolerance(F), error * 1.5);
        RevWires = false;
        RevFace = false;
        return true;
      }
      nu = std::min(nu * 2, 96);
      nv = std::min(nv * 2, 96);
    }
    return false;
  }

  bool NewCurve(const TopoDS_Edge& E, Handle(Geom_Curve)& C, TopLoc_Location& L, double& Tol) override {
    TopLoc_Location location;
    double first = 0, last = 0;
    const Handle(Geom_Curve) curve = BRep_Tool::Curve(E, location, first, last);
    if (curve.IsNull()) return false;
    const Handle(Geom_BSplineCurve) source = Handle(Geom_BSplineCurve)::DownCast(curve);
    if (source.IsNull()) return false;
    Handle(Geom_BSplineCurve) base = Handle(Geom_BSplineCurve)::DownCast(source->Transformed(location.Transformation()));
    const double t0 = base->FirstParameter(), t1 = base->LastParameter();
    double length = 0;
    gp_Pnt previous;
    for (int k = 0; k <= 32; ++k) {
      const gp_Pnt p = base->Value(t0 + (t1 - t0) * k / 32);
      if (k) length += p.Distance(previous);
      previous = p;
    }
    int n = std::clamp(static_cast<int>(std::ceil(length / step_)), 1, 192);
    for (int attempt = 0; attempt < 4; ++attempt) {
      Handle(Geom_BSplineCurve) bent = Handle(Geom_BSplineCurve)::DownCast(base->Copy());
      bent->IncreaseDegree(std::max(3, bent->Degree()));
      for (int i = 1; i < n; ++i) bent->InsertKnot(t0 + (t1 - t0) * i / n, 1, 1e-9);
      for (int i = 1; i <= bent->NbPoles(); ++i) bent->SetPole(i, warp_(bent->Pole(i)));
      double error = 0;
      for (int i = 0; i <= 4 * n; ++i) {
        const double t = t0 + (t1 - t0) * i / (4 * n);
        error = std::max(error, warp_(base->Value(t)).Distance(bent->Value(t)));
      }
      if (error <= tolerance_ || attempt == 3 || n >= 192) {
        worst = std::max(worst, error);
        C = bent;
        L = TopLoc_Location();
        Tol = std::max(BRep_Tool::Tolerance(E), tolerance_ * 2 + error);
        return true;
      }
      n = std::min(n * 2, 192);
    }
    return false;
  }

  bool NewPoint(const TopoDS_Vertex& V, gp_Pnt& P, double& Tol) override {
    P = warp_(BRep_Tool::Pnt(V));
    Tol = std::max(BRep_Tool::Tolerance(V), tolerance_ * 2);
    return true;
  }

  bool NewCurve2d(const TopoDS_Edge&, const TopoDS_Face&, const TopoDS_Edge&, const TopoDS_Face&, Handle(Geom2d_Curve)&,
                  double&) override {
    return false;  // the parameters did not change: the outlines in them stay
  }

  bool NewParameter(const TopoDS_Vertex&, const TopoDS_Edge&, double&, double&) override { return false; }

  GeomAbs_Shape Continuity(const TopoDS_Edge& E, const TopoDS_Face& F1, const TopoDS_Face& F2, const TopoDS_Edge&,
                           const TopoDS_Face&, const TopoDS_Face&) override {
    return BRep_Tool::Continuity(E, F1, F2);
  }

 private:
  Warp warp_;
  double step_, tolerance_;
};

// The shape warped (see WarpModification); `length` is how long the warped part is, for the step.
TopoDS_Shape warped(const TopoDS_Shape& shape, const Warp& warp, double length, QString* why) {
  BRepBuilderAPI_NurbsConvert nurbs(shape, true);
  if (!nurbs.IsDone()) {
    *why = QStringLiteral("it could not be turned into splines");
    return {};
  }
  Handle(WarpModification) modification = new WarpModification(warp, std::max(length / 48.0, 0.2), 0.005);
  BRepTools_Modifier modifier(nurbs.Shape(), modification);
  if (!modifier.IsDone()) {
    *why = QStringLiteral("its surfaces could not be warped");
    return {};
  }
  TopoDS_Shape out = modifier.ModifiedShape(nurbs.Shape());
  if (!BRepCheck_Analyzer(out).IsValid()) {
    ShapeFix_Shape fix(out);
    fix.Perform();
    out = fix.Shape();
  }
  if (!solidIn(out) || !BRepCheck_Analyzer(out).IsValid()) {
    *why = QStringLiteral("the warped solid did not check out");
    return {};
  }
  return out;
}

double coordinate(const gp_Pnt& p, int axis) { return axis == 0 ? p.X() : axis == 1 ? p.Y() : p.Z(); }
void setCoordinate(gp_Pnt& p, int axis, double value) {
  if (axis == 0) p.SetX(value);
  else if (axis == 1) p.SetY(value);
  else p.SetZ(value);
}

// The mesh's bend (evaluator.cpp, applyBend) for a part whose box was lo..hi before it.
Warp bendWarp(double degrees, int along, int toward, const double lo[3], const double hi[3]) {
  const double len = hi[along] - lo[along], mid = (lo[along] + hi[along]) / 2, neutral = (lo[toward] + hi[toward]) / 2;
  const double angle = degrees * kPi / 180.0, radius0 = len / angle;
  return [=](const gp_Pnt& v) {
    gp_Pnt out = v;
    const double a = ((coordinate(v, along) - mid) / len) * angle;
    const double radius = radius0 + (coordinate(v, toward) - neutral);
    setCoordinate(out, along, mid + std::sin(a) * radius);
    setCoordinate(out, toward, neutral + std::cos(a) * radius - radius0);
    return out;
  };
}

// The mesh's twist (applyTwist): made at the part's real size (`scale`), its box lo..hi there.
Warp twistWarp(double degrees, int axis, const double scale[3], const double lo[3], const double hi[3]) {
  const int b = (axis + 1) % 3, c = (axis + 2) % 3;
  const double len = hi[axis] - lo[axis], cb = (lo[b] + hi[b]) * 0.5, cc = (lo[c] + hi[c]) * 0.5;
  const double total = degrees * kPi / 180.0;
  const double s[3] = {scale[0], scale[1], scale[2]};
  return [=](const gp_Pnt& local) {
    gp_Pnt v(local.X() * s[0], local.Y() * s[1], local.Z() * s[2]);
    const double t = ((coordinate(v, axis) - lo[axis]) / len - 0.5) * total;
    const double pb = coordinate(v, b) - cb, pc = coordinate(v, c) - cc;
    setCoordinate(v, b, cb + pb * std::cos(t) - pc * std::sin(t));
    setCoordinate(v, c, cc + pb * std::sin(t) + pc * std::cos(t));
    return gp_Pnt(v.X() / s[0], v.Y() / s[1], v.Z() / s[2]);
  };
}

// --- Shape recognition ---------------------------------------------------------------------------
//
// A mesh made by a generator (or an STL) has its round parts as rings of narrow flat faces. Areas
// whose corners all lie on one cylinder or cone, ending square to its axis, are recognised; the
// solid of flat faces then gets a thin exact shell there, a little thicker than the gap between
// the flat faces and the true surface: added where the surface bulges out (posts, rounded corners),
// cut where it curves in (holes, inside corners). The flat faces end up inside the material or in
// the air, and the true surface is left. A shell only ever changes the solid by that sliver.

struct Axisym {
  gp_Pnt origin;   // on the axis, where t = 0
  gp_Dir axis;
  gp_Dir u, v;     // across the axis; v = axis x u
  double r0 = 0, k = 0;  // radius r0 + k t at axial coordinate t
  double radius(double t) const { return r0 + k * t; }
};

double axialOf(const Axisym& s, const gp_Pnt& p) { return gp_Vec(s.origin, p).Dot(gp_Vec(s.axis)); }
double angleOf(const Axisym& s, const gp_Pnt& p) {
  const gp_Vec d(s.origin, p);
  return std::atan2(d.Dot(gp_Vec(s.v)), d.Dot(gp_Vec(s.u)));
}
double offSurface(const Axisym& s, const gp_Pnt& p) {
  const gp_Vec d(s.origin, p);
  const double t = d.Dot(gp_Vec(s.axis));
  const double across = (d - gp_Vec(s.axis) * t).Magnitude();
  return std::abs(across - s.radius(t));
}

// The axis a set of face normals turns round: the direction the normals vary least along (their
// spread's smallest eigenvector, by Jacobi). Null when they do not turn round one axis.
bool axisOfNormals(const std::vector<gp_Vec>& normals, gp_Dir* axis) {
  if (normals.size() < 3) return false;
  gp_Vec mean(0, 0, 0);
  for (const gp_Vec& n : normals) mean += n;
  mean /= static_cast<double>(normals.size());
  double m[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  for (const gp_Vec& n : normals) {
    const double d[3] = {n.X() - mean.X(), n.Y() - mean.Y(), n.Z() - mean.Z()};
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) m[i][j] += d[i] * d[j];
  }
  double vec[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int sweep = 0; sweep < 50; ++sweep) {
    int p = 0, q = 1;
    for (int i = 0; i < 3; ++i)
      for (int j = i + 1; j < 3; ++j)
        if (std::abs(m[i][j]) > std::abs(m[p][q])) p = i, q = j;
    if (std::abs(m[p][q]) < 1e-15) break;
    const double theta = 0.5 * std::atan2(2 * m[p][q], m[q][q] - m[p][p]);
    const double c = std::cos(theta), sn = std::sin(theta);
    for (int k = 0; k < 3; ++k) {
      const double mkp = m[k][p], mkq = m[k][q];
      m[k][p] = c * mkp - sn * mkq;
      m[k][q] = sn * mkp + c * mkq;
    }
    for (int k = 0; k < 3; ++k) {
      const double mpk = m[p][k], mqk = m[q][k];
      m[p][k] = c * mpk - sn * mqk;
      m[q][k] = sn * mpk + c * mqk;
    }
    for (int k = 0; k < 3; ++k) {
      const double vkp = vec[k][p], vkq = vec[k][q];
      vec[k][p] = c * vkp - sn * vkq;
      vec[k][q] = sn * vkp + c * vkq;
    }
  }
  int smallest = 0, largest = 0;
  for (int i = 1; i < 3; ++i) {
    if (m[i][i] < m[smallest][smallest]) smallest = i;
    if (m[i][i] > m[largest][largest]) largest = i;
  }
  // The normals must spread in a plane: one direction (almost) without spread, another with.
  if (!(m[largest][largest] > 1e-8) || m[smallest][smallest] > 1e-6 * m[largest][largest] + 1e-12) return false;
  const gp_Vec a(vec[0][smallest], vec[1][smallest], vec[2][smallest]);
  if (a.Magnitude() < 1e-9) return false;
  *axis = gp_Dir(a);
  return true;
}

// Fits a cylinder or cone with the given axis direction to points: a circle at each height the
// points lie at (three points or more), their centres on one line along the axis, their radii in a
// straight line with height. False when it does not fit within `tolerance`.
bool fitAxisym(const std::vector<gp_Pnt>& points, const gp_Dir& axis, double tolerance, Axisym* out) {
  if (points.size() < 5) return false;
  Axisym s;
  s.axis = axis;
  s.u = gp_Dir(std::abs(axis.X()) < 0.9 ? gp_Vec(axis).Crossed(gp_Vec(1, 0, 0)) : gp_Vec(axis).Crossed(gp_Vec(0, 1, 0)));
  s.v = gp_Dir(gp_Vec(axis).Crossed(gp_Vec(s.u)));
  struct P2 {
    double t, x, y;
  };
  std::vector<P2> ps;
  for (const gp_Pnt& p : points) {
    const gp_Vec d(p.XYZ());
    ps.push_back({d.Dot(gp_Vec(axis)), d.Dot(gp_Vec(s.u)), d.Dot(gp_Vec(s.v))});
  }
  std::sort(ps.begin(), ps.end(), [](const P2& a, const P2& b) { return a.t < b.t; });
  struct Level {
    double t, cx, cy, r;
  };
  std::vector<Level> levels;
  for (size_t i = 0; i < ps.size();) {
    size_t j = i;
    while (j < ps.size() && ps[j].t - ps[i].t < tolerance * 10) ++j;
    if (j - i >= 3) {
      // Kasa: x^2 + y^2 = 2a x + 2b y + c, least squares.
      double m[3][4] = {};
      double tSum = 0;
      for (size_t k = i; k < j; ++k) {
        const double row[3] = {2 * ps[k].x, 2 * ps[k].y, 1};
        const double rhs = ps[k].x * ps[k].x + ps[k].y * ps[k].y;
        for (int a = 0; a < 3; ++a) {
          for (int b = 0; b < 3; ++b) m[a][b] += row[a] * row[b];
          m[a][3] += row[a] * rhs;
        }
        tSum += ps[k].t;
      }
      bool solved = true;
      for (int a = 0; a < 3 && solved; ++a) {
        int pivot = a;
        for (int r = a + 1; r < 3; ++r)
          if (std::abs(m[r][a]) > std::abs(m[pivot][a])) pivot = r;
        if (std::abs(m[pivot][a]) < 1e-30) solved = false;
        else {
          std::swap(m[a], m[pivot]);
          for (int r = 0; r < 3; ++r) {
            if (r == a) continue;
            const double f = m[r][a] / m[a][a];
            for (int c = a; c < 4; ++c) m[r][c] -= f * m[a][c];
          }
        }
      }
      if (solved) {
        const double cx = m[0][3] / m[0][0], cy = m[1][3] / m[1][1], c = m[2][3] / m[2][2];
        const double rr = c + cx * cx + cy * cy;
        if (rr > 0) levels.push_back({tSum / static_cast<double>(j - i), cx, cy, std::sqrt(rr)});
      }
    }
    i = j;
  }
  if (levels.size() < 2) return false;
  // One axis: the centres agree.
  double cx = 0, cy = 0;
  for (const Level& l : levels) cx += l.cx, cy += l.cy;
  cx /= static_cast<double>(levels.size());
  cy /= static_cast<double>(levels.size());
  for (const Level& l : levels)
    if (std::hypot(l.cx - cx, l.cy - cy) > tolerance * 10) return false;
  // Radius in a straight line with height.
  double st = 0, sr = 0, stt = 0, str = 0;
  for (const Level& l : levels) st += l.t, sr += l.r, stt += l.t * l.t, str += l.t * l.r;
  const double n = static_cast<double>(levels.size()), det = n * stt - st * st;
  s.k = std::abs(det) > 1e-18 ? (n * str - st * sr) / det : 0;
  s.r0 = (sr - s.k * st) / n;
  s.origin = gp_Pnt((gp_Vec(s.u) * cx + gp_Vec(s.v) * cy).XYZ());
  for (const gp_Pnt& p : points)
    if (offSurface(s, p) > tolerance) return false;
  *out = s;
  return true;
}

struct Recognised {
  int cylinders = 0, cones = 0;
  bool complete = false;  // every curved area recognised: what is left are true flat faces
  QString summary() const {
    QStringList parts;
    if (cylinders) parts << QStringLiteral("%1 cylinder%2").arg(cylinders).arg(cylinders == 1 ? "" : "s");
    if (cones) parts << QStringLiteral("%1 cone%2").arg(cones).arg(cones == 1 ? "" : "s");
    return parts.join(QStringLiteral(" and "));
  }
};

// The solid of a closed mesh, with its cylinders and cones recognised (see above). Null when the
// mesh is not a closed solid. `found` says what was recognised.
TopoDS_Shape recognisedSolid(const std::vector<double>& points, const std::vector<uint32_t>& triangles, int* faces,
                             QString* why, Recognised* found) {
  step::Faceted model;
  if (!step::analyseSolid(points, triangles, &model, why)) return {};
  if (faces) *faces = static_cast<int>(model.faces.size());
  const TopoDS_Shape plain = facetedSolid(model);
  if (plain.IsNull()) return {};
  // On a very large mesh (a long thread, a fine sphere) the shells' booleans take minutes, and a
  // part that size is written from its mesh anyway.
  if (model.faces.size() > 12000) return plain;
  const size_t count = model.faces.size();
  // Each face: its corners and normal; each edge: its two faces.
  std::vector<std::vector<int>> corners(count);
  std::vector<gp_Vec> normals(count);
  std::vector<std::vector<int>> facesOfEdge(model.edges.size());
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (const auto& p : model.points)
    for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], p[static_cast<size_t>(a)]), hi[a] = std::max(hi[a], p[static_cast<size_t>(a)]);
  const double extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-3});
  const double tolerance = std::max(1e-6, extent * 2e-6);
  for (size_t f = 0; f < count; ++f) {
    const auto& face = model.faces[f];
    normals[f] = gp_Vec(face.normal[0], face.normal[1], face.normal[2]);
    for (const auto& bound : face.bounds)
      for (const auto& [edge, forward] : bound.edges) {
        const auto& e = model.edges[static_cast<size_t>(edge)];
        corners[f].push_back(forward ? e.a : e.b);
        facesOfEdge[static_cast<size_t>(edge)].push_back(static_cast<int>(f));
      }
  }
  std::vector<std::vector<std::pair<int, int>>> neighbours(count);  // face, shared edge
  for (size_t e = 0; e < facesOfEdge.size(); ++e)
    if (facesOfEdge[e].size() == 2) {
      const int a = facesOfEdge[e][0], b = facesOfEdge[e][1];
      const double turn = normals[static_cast<size_t>(a)].Angle(normals[static_cast<size_t>(b)]);
      if (turn > 1e-5 && turn < 0.6) {  // the joins of a ring of narrow faces
        neighbours[static_cast<size_t>(a)].push_back({b, static_cast<int>(e)});
        neighbours[static_cast<size_t>(b)].push_back({a, static_cast<int>(e)});
      }
    }
  auto point = [&](int i) {
    const auto& p = model.points[static_cast<size_t>(i)];
    return gp_Pnt(p[0], p[1], p[2]);
  };
  std::vector<int> owner(count, -1);
  std::vector<char> shelled(count, 0);
  std::vector<TopoDS_Shape> outward, inward;
  int regionNumber = 0;
  for (size_t seed = 0; seed < count; ++seed) {
    if (owner[seed] >= 0 || neighbours[seed].empty()) continue;
    // A seed: this face and two neighbours in a row, turning round one axis.
    std::vector<int> region{static_cast<int>(seed)};
    for (const auto& [next, edge] : neighbours[seed])
      if (owner[static_cast<size_t>(next)] < 0 && region.size() < 3) region.push_back(next);
    if (region.size() < 3)
      for (const auto& [next, edge] : neighbours[static_cast<size_t>(region.back())])
        if (owner[static_cast<size_t>(next)] < 0 && std::find(region.begin(), region.end(), next) == region.end() && region.size() < 3)
          region.push_back(next);
    if (region.size() < 3) continue;
    auto fit = [&](const std::vector<int>& faces, Axisym* out) {
      std::vector<gp_Vec> ns;
      std::vector<gp_Pnt> ps;
      std::vector<char> seen(model.points.size(), 0);
      for (int f : faces) {
        ns.push_back(normals[static_cast<size_t>(f)]);
        for (int c : corners[static_cast<size_t>(f)])
          if (!seen[static_cast<size_t>(c)]) seen[static_cast<size_t>(c)] = 1, ps.push_back(point(c));
      }
      gp_Dir axis;
      return axisOfNormals(ns, &axis) && fitAxisym(ps, axis, tolerance, out);
    };
    Axisym surface;
    if (!fit(region, &surface) || surface.r0 > extent * 1000) continue;
    // Grow: neighbours whose corners all lie on the surface.
    std::vector<char> in(count, 0);
    for (int f : region) in[static_cast<size_t>(f)] = 1;
    for (size_t i = 0; i < region.size(); ++i) {
      for (const auto& [next, edge] : neighbours[static_cast<size_t>(region[i])]) {
        if (in[static_cast<size_t>(next)] || owner[static_cast<size_t>(next)] >= 0) continue;
        bool on = true;
        for (int c : corners[static_cast<size_t>(next)])
          if (offSurface(surface, point(c)) > tolerance) on = false;
        if (!on) continue;
        in[static_cast<size_t>(next)] = 1;
        region.push_back(next);
        if (region.size() % 16 == 0) {
          Axisym better;
          if (fit(region, &better)) surface = better;
        }
      }
    }
    Axisym final;
    if (!fit(region, &final)) continue;
    surface = final;
    for (int f : region) owner[static_cast<size_t>(f)] = regionNumber;
    ++regionNumber;
    // Four faces at least: the corners of three faces in a symmetric row (a trapezoid) always lie
    // on some circle. And no wider than twice the part: a few nearly flat faces fit huge circles.
    if (region.size() < 4 || surface.r0 > extent * 2 || std::abs(surface.k) > 50) continue;
    // Its outline must be rings square to the axis (at the lowest and highest t) and, for a part
    // of a turn, straight sides along it: the shape the shell is made in.
    double t1 = 1e300, t2 = -1e300;
    for (int f : region)
      for (int c : corners[static_cast<size_t>(f)]) {
        const double t = axialOf(surface, point(c));
        t1 = std::min(t1, t), t2 = std::max(t2, t);
      }
    if (t2 - t1 < tolerance * 10) continue;
    bool square = true;
    std::vector<double> sides;
    double longestChord = 0;
    for (int f : region)
      for (const auto& bound : model.faces[static_cast<size_t>(f)].bounds)
        for (const auto& [edge, forward] : bound.edges) {
          const auto& e = model.edges[static_cast<size_t>(edge)];
          const gp_Pnt a = point(e.a), b = point(e.b);
          const double ta = axialOf(surface, a), tb = axialOf(surface, b);
          if (std::abs(ta - tb) < tolerance * 10) longestChord = std::max(longestChord, a.Distance(b));
          const auto& pair = facesOfEdge[static_cast<size_t>(edge)];
          const int other = pair.size() == 2 ? (pair[0] == f ? pair[1] : pair[0]) : -1;
          if (other >= 0 && in[static_cast<size_t>(other)]) continue;  // inside the area
          const bool ring = std::abs(ta - tb) < tolerance * 10 &&
                            (std::abs(ta - t1) < tolerance * 10 || std::abs(ta - t2) < tolerance * 10);
          const double angleA = angleOf(surface, a), angleB = angleOf(surface, b);
          double turn = std::abs(angleA - angleB);
          turn = std::min(turn, 2 * kPi - turn);
          const bool side = turn < 1e-4 && std::abs(std::abs(ta - tb) - (t2 - t1)) < tolerance * 10;
          if (side) sides.push_back(angleA);
          if (!ring && !side) square = false;
        }
    if (!square) continue;
    // The part of the turn it covers: all of it, or between its two sides.
    double from = 0, sweep = 2 * kPi;
    if (!sides.empty()) {
      std::sort(sides.begin(), sides.end());
      const double first = sides.front();
      double second = first;
      for (double a : sides)
        if (std::min(std::abs(a - first), 2 * kPi - std::abs(a - first)) > 1e-4) second = a;
      if (second == first) continue;
      // The arc from `first` to `second` (anticlockwise round the axis), or the other one: the one
      // a face of the area lies in.
      gp_Vec middle(0, 0, 0);
      for (int c : corners[static_cast<size_t>(region.front())]) middle += gp_Vec(point(c).XYZ());
      middle /= static_cast<double>(corners[static_cast<size_t>(region.front())].size());
      double at = angleOf(surface, gp_Pnt(middle.XYZ())) - first;
      while (at < 0) at += 2 * kPi;
      double span = second - first;
      while (span < 0) span += 2 * kPi;
      if (at < span) from = first, sweep = span;
      else from = second, sweep = 2 * kPi - span;
    }
    // Outward or inward: the faces' normals against the direction away from the axis.
    const auto& face0 = model.faces[static_cast<size_t>(region.front())];
    const gp_Pnt on0 = point(corners[static_cast<size_t>(region.front())][0]);
    gp_Vec away(surface.origin, on0);
    away -= gp_Vec(surface.axis) * away.Dot(gp_Vec(surface.axis));
    const bool bulges = gp_Vec(face0.normal[0], face0.normal[1], face0.normal[2]).Dot(away) > 0;
    // The shell: from the true surface in by a little more than the flat faces' sag.
    const double rMin = std::min(surface.radius(t1), surface.radius(t2));
    if (!(rMin > tolerance * 100)) continue;
    const double half = std::asin(std::min(1.0, longestChord / (2 * rMin)));
    const double sag = std::max(surface.radius(t1), surface.radius(t2)) * (1 - std::cos(half));
    // Rings of at least 20 sides (fewer is a polygon on purpose: the corners of a hexagon lie on a
    // circle too; the exact export treats a cylinder of fewer sides the same way), and flat faces
    // within 0.2 mm of the curve.
    if (half > kPi / 20 + 1e-6 || sag > 0.2) continue;
    const double thick = std::min(std::max(3 * sag + extent * 1e-5, 0.002), rMin * 0.5);
    auto at3 = [&](double rho, double t) {
      return gp_Pnt((gp_Vec(surface.origin.XYZ()) + gp_Vec(surface.axis) * t +
                     (gp_Vec(surface.u) * std::cos(from) + gp_Vec(surface.v) * std::sin(from)) * rho)
                        .XYZ());
    };
    const gp_Pnt q[4] = {at3(surface.radius(t1) - thick, t1), at3(surface.radius(t1), t1), at3(surface.radius(t2), t2),
                         at3(surface.radius(t2) - thick, t2)};
    BRepBuilderAPI_MakeWire outline;
    for (int i = 0; i < 4; ++i) outline.Add(BRepBuilderAPI_MakeEdge(q[i], q[(i + 1) % 4]));
    if (!outline.IsDone()) continue;
    BRepBuilderAPI_MakeFace profile(outline.Wire(), true);
    if (!profile.IsDone()) continue;
    BRepPrimAPI_MakeRevol shell(profile.Face(), gp_Ax1(surface.origin, surface.axis), sweep);
    if (!shell.IsDone() || !solidIn(shell.Shape())) continue;
    (bulges ? outward : inward).push_back(shell.Shape());
    for (int f : region) shelled[static_cast<size_t>(f)] = 1;
    if (std::abs(surface.k) < 1e-6) ++found->cylinders;
    else ++found->cones;
  }
  // Complete when no face that meets a neighbour at a shallow angle (part of a curve) is left:
  // then every face left is a true flat face (a mesh with no curves at all is exact as it is).
  found->complete = true;
  for (size_t f = 0; f < count; ++f)
    if (!shelled[f] && !neighbours[f].empty()) found->complete = false;
  if (outward.empty() && inward.empty()) return plain;
  TopoDS_Shape out = plain;
  if (!outward.empty()) {
    std::vector<TopoDS_Shape> all{plain};
    all.insert(all.end(), outward.begin(), outward.end());
    out = fuseAll(all);
  }
  if (solidIn(out) && !inward.empty()) out = boolean(1, {out}, inward);
  if (solidIn(out)) {
    ShapeUpgrade_UnifySameDomain unify(out, true, true, false);
    unify.Build();
    if (!unify.Shape().IsNull()) out = unify.Shape();
  }
  if (!solidIn(out) || !BRepCheck_Analyzer(out).IsValid()) {
    *found = Recognised();
    return plain;
  }
  return out;
}

// --- Rounding edges ------------------------------------------------------------------------------

struct P3 {
  double x, y, z;
};
P3 sub(const P3& a, const P3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(const P3& a, const P3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
P3 add(const P3& a, const P3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
P3 mul(const P3& a, double k) { return {a.x * k, a.y * k, a.z * k}; }

double segmentDistance(const P3& p, const P3& a, const P3& b) {
  const P3 ab = sub(b, a);
  const double len2 = dot(ab, ab);
  const double t = len2 > 0 ? std::clamp(dot(sub(p, a), ab) / len2, 0.0, 1.0) : 0.0;
  const P3 d = sub(p, add(a, mul(ab, t)));
  return std::sqrt(dot(d, d));
}

// Closest distance from p to triangle abc (Ericson, Real-Time Collision Detection 5.1.5).
double triangleDistance(const P3& p, const P3& a, const P3& b, const P3& c) {
  const P3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
  const double d1 = dot(ab, ap), d2 = dot(ac, ap);
  auto len = [&](const P3& q) {
    const P3 d = sub(p, q);
    return std::sqrt(dot(d, d));
  };
  if (d1 <= 0 && d2 <= 0) return len(a);
  const P3 bp = sub(p, b);
  const double d3 = dot(ab, bp), d4 = dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return len(b);
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return len(add(a, mul(ab, d1 / (d1 - d3))));
  const P3 cp = sub(p, c);
  const double d5 = dot(ab, cp), d6 = dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return len(c);
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return len(add(a, mul(ac, d2 / (d2 - d6))));
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return len(add(b, mul(sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6)))));
  const double denom = 1.0 / (va + vb + vc);
  return len(add(a, add(mul(ab, vb * denom), mul(ac, vc * denom))));
}

// The edges of a solid, each once, with the faces on either side.
struct EdgeOf {
  TopoDS_Edge edge;
  std::vector<TopoDS_Face> faces;
};
std::vector<EdgeOf> edgesOf(const TopoDS_Shape& shape) {
  std::vector<EdgeOf> out;
  for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
    const TopoDS_Face face = TopoDS::Face(f.Current());
    for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
      const TopoDS_Edge edge = TopoDS::Edge(e.Current());
      if (BRep_Tool::Degenerated(edge)) continue;
      auto found = std::find_if(out.begin(), out.end(), [&](const EdgeOf& known) { return known.edge.IsSame(edge); });
      if (found == out.end()) out.push_back({edge, {face}});
      else if (std::none_of(found->faces.begin(), found->faces.end(), [&](const TopoDS_Face& known) { return known.IsSame(face); }))
        found->faces.push_back(face);
    }
  }
  return out;
}

std::vector<P3> edgeSamples(const TopoDS_Edge& edge, int count) {
  BRepAdaptor_Curve curve(edge);
  const double t0 = curve.FirstParameter(), t1 = curve.LastParameter();
  std::vector<P3> out;
  for (int i = 1; i <= count; ++i) {
    const gp_Pnt p = curve.Value(t0 + (t1 - t0) * i / (count + 1));
    out.push_back({p.X(), p.Y(), p.Z()});
  }
  return out;
}

// The angle between the outward normals of the two faces at the middle of the edge (0: the faces
// continue smoothly, 90°: a box edge), or -1 when it is not between two faces.
double edgeTurn(const EdgeOf& e) {
  if (e.faces.size() != 2) return -1;
  BRepAdaptor_Curve curve(e.edge);
  const gp_Pnt mid = curve.Value((curve.FirstParameter() + curve.LastParameter()) * 0.5);
  gp_Vec normals[2];
  for (int i = 0; i < 2; ++i) {
    const Handle(Geom_Surface) surface = BRep_Tool::Surface(e.faces[static_cast<size_t>(i)]);
    GeomAPI_ProjectPointOnSurf project(mid, surface);
    if (!project.IsDone() || project.NbPoints() == 0) return -1;
    double u = 0, v = 0;
    project.LowerDistanceParameters(u, v);
    GeomLProp_SLProps props(surface, u, v, 1, 1e-7);
    if (!props.IsNormalDefined()) return -1;
    gp_Vec n(props.Normal());
    if (e.faces[static_cast<size_t>(i)].Orientation() == TopAbs_REVERSED) n.Reverse();
    normals[i] = n;
  }
  return normals[0].Angle(normals[1]);
}

// The edges a mesh rounded: where the sharp edge of the exact solid lies off the rounded mesh by
// about what a fillet of radius r takes off (r (1/cos(turn/2) - 1)), not on it.
std::vector<TopoDS_Edge> edgesRoundedIn(const TopoDS_Shape& shape, const std::vector<double>& points,
                                        const std::vector<uint32_t>& triangles, double r) {
  std::vector<TopoDS_Edge> out;
  for (const EdgeOf& e : edgesOf(shape)) {
    const double turn = edgeTurn(e);
    if (turn < 0.05) continue;  // smooth, or not between two faces
    const double expected = r * (1.0 / std::cos(turn * 0.5) - 1.0);
    if (!(expected > 1e-4)) continue;
    // Most of the edge off the mesh: the mesh may leave its ends sharp where it could not round a
    // corner, and the samples of a round edge that is not rounded touch its facets' corners.
    int off = 0;
    for (const P3& p : edgeSamples(e.edge, 9)) {
      double d = 1e300;
      for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
        auto at = [&](uint32_t i) { return P3{points[3 * i], points[3 * i + 1], points[3 * i + 2]}; };
        d = std::min(d, triangleDistance(p, at(triangles[t]), at(triangles[t + 1]), at(triangles[t + 2])));
        if (d < expected * 0.5) break;
      }
      if (d > expected * 0.5) ++off;
    }
    if (off >= 5) out.push_back(e.edge);
  }
  return out;
}

// The edges of `shape` lying along a picked chain (points along it, x y z each): every sample of
// the edge within `tolerance` of the chain.
std::vector<EdgeOf> edgesAlong(const std::vector<EdgeOf>& edges, const std::vector<double>& chain, double tolerance) {
  std::vector<EdgeOf> out;
  std::vector<P3> line;
  for (size_t i = 0; i + 2 < chain.size(); i += 3) line.push_back({chain[i], chain[i + 1], chain[i + 2]});
  if (line.size() < 2) return out;
  // A picked round edge is the mesh's polygon, which lies inside the true curve by up to
  // L tan(a/4) / 2 on a segment of length L that turns by a at its ends: each segment gets that
  // much more room (a straight chain gets none).
  const size_t count = line.size() - 1;
  auto direction = [&](size_t k) {
    P3 d = sub(line[k + 1], line[k]);
    const double len = std::sqrt(dot(d, d));
    return len > 0 ? mul(d, 1.0 / len) : P3{0, 0, 0};
  };
  const P3 gap = sub(line.front(), line.back());
  const bool closed = count > 2 && std::sqrt(dot(gap, gap)) < tolerance;
  std::vector<double> room(count, tolerance);
  for (size_t k = 0; k < count; ++k) {
    double turn = 0;
    if (k > 0 || closed) turn = std::max(turn, std::acos(std::clamp(dot(direction(k), direction(k > 0 ? k - 1 : count - 1)), -1.0, 1.0)));
    if (k + 1 < count || closed) turn = std::max(turn, std::acos(std::clamp(dot(direction(k), direction(k + 1 < count ? k + 1 : 0)), -1.0, 1.0)));
    const P3 d = sub(line[k + 1], line[k]);
    room[k] += 1.5 * std::sqrt(dot(d, d)) * std::tan(turn * 0.25) * 0.5;
  }
  for (const EdgeOf& e : edges) {
    bool on = true;
    for (const P3& p : edgeSamples(e.edge, 7)) {
      bool near = false;
      for (size_t k = 0; k < count && !near; ++k) near = segmentDistance(p, line[k], line[k + 1]) <= room[k];
      if (!near) {
        on = false;
        break;
      }
    }
    if (on) out.push_back(e);
  }
  return out;
}

// Rounds `edges` with radius r (several radii: one call per radius list entry). Null when
// OpenCASCADE cannot.
TopoDS_Shape filleted(const TopoDS_Shape& shape, const std::vector<std::pair<TopoDS_Edge, double>>& edges) {
  if (edges.empty()) return shape;
  BRepFilletAPI_MakeFillet make(shape);
  for (const auto& [edge, r] : edges) make.Add(r, edge);
  make.Build();
  if (!make.IsDone()) return {};
  const TopoDS_Shape out = make.Shape();
  return solidIn(out) ? out : TopoDS_Shape();
}

TopoDS_Shape chamfered(const TopoDS_Shape& shape, const std::vector<std::pair<TopoDS_Edge, double>>& edges) {
  if (edges.empty()) return shape;
  BRepFilletAPI_MakeChamfer make(shape);
  for (const auto& [edge, d] : edges) make.Add(d, edge);
  make.Build();
  if (!make.IsDone()) return {};
  const TopoDS_Shape out = make.Shape();
  return solidIn(out) ? out : TopoDS_Shape();
}

struct Builder {
  explicit Builder(const Document& d) : doc(d) {}
  const Document& doc;
  int exactPieces = 0;
  int meshPieces = 0;
  int meshFaces = 0;
  int recognisedPieces = 0;  // mesh pieces with cylinders or cones recognised in them
  bool failed = false;
  QString why;
  QString curveWhy;  // why a text or 2D shape could not be built from its curves
  std::vector<std::pair<QString, QString>> meshWhy;  // each piece that came from its mesh: name, why

  void fail(const QString& reason) {
    if (!failed) why = reason;
    failed = true;
  }

  // Like the evaluator's combine: solids joined, holes joined; holes cut the solids, and stay
  // holes only where there is nothing solid.
  XParts combine(const std::vector<XParts>& kids, bool intersect) {
    std::vector<TopoDS_Shape> solids, holes;
    for (const XParts& k : kids) {
      if (k.hasSolid) solids.push_back(k.solid);
      if (k.hasHole) holes.push_back(k.hole);
    }
    XParts out;
    if (!solids.empty()) {
      if (intersect) {
        TopoDS_Shape common = solids.front();
        for (size_t i = 1; i < solids.size() && !common.IsNull(); ++i) common = boolean(2, {common}, {solids[i]});
        out.solid = common;
      } else {
        out.solid = fuseAll(solids);
      }
      out.hasSolid = solidIn(out.solid);
      if (!out.hasSolid && !solids.empty()) fail(intersect ? QStringLiteral("an intersection is empty") : QStringLiteral("a join failed"));
    }
    if (!holes.empty()) {
      out.hole = fuseAll(holes);
      out.hasHole = solidIn(out.hole);
    }
    if (out.hasSolid && out.hasHole) {
      out.solid = boolean(1, {out.solid}, {out.hole});
      out.hasHole = false;
      out.hasSolid = solidIn(out.solid);
      if (!out.hasSolid) fail(QStringLiteral("a hole cut failed or took everything"));
    }
    return out;
  }

  // The part grown by its hole clearance, all round or on the sides only (its own Z kept).
  TopoDS_Shape grown(const TopoDS_Shape& shape, double clearance, bool sidesOnly) {
    if (!(clearance > 1e-6)) return shape;
    BRepOffsetAPI_MakeOffsetShape offset;
    offset.PerformByJoin(shape, clearance, 1e-6, BRepOffset_Skin, false, false, GeomAbs_Intersection);
    if (!offset.IsDone()) return {};
    TopoDS_Shape out = offset.Shape();
    if (sidesOnly) {
      Bnd_Box box;
      BRepBndLib::AddOptimal(shape, box, false, false);
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      const double m = clearance * 2 + 1;
      const TopoDS_Shape slab = BRepPrimAPI_MakeBox(gp_Pnt(x0 - m, y0 - m, z0), gp_Pnt(x1 + m, y1 + m, z1)).Shape();
      out = boolean(2, {out}, {slab});
    }
    return solidIn(out) ? out : TopoDS_Shape();
  }

  // Closed hollow: the part less itself moved in by the wall.
  TopoDS_Shape hollowed(const TopoDS_Shape& shape, double wall) {
    BRepOffsetAPI_MakeOffsetShape offset;
    offset.PerformByJoin(shape, -wall, 1e-6, BRepOffset_Skin, false, false, GeomAbs_Intersection);
    if (!offset.IsDone() || !solidIn(offset.Shape())) return {};
    const TopoDS_Shape out = boolean(1, {shape}, {offset.Shape()});
    return solidIn(out) ? out : TopoDS_Shape();
  }

  // A shape at its real size, before its own mirror and placement, when it has an exact form.
  // Its scale goes into its sizes where that keeps it the same kind of surface.
  TopoDS_Shape primitive(const Node& n, const double s[3], bool* scaledIn) {
    *scaledIn = true;
    const QString& shape = n.shape;
    const double x = std::max(n.size[0], 0.05), y = std::max(n.size[1], 0.05), z = std::max(n.size[2], 0.05);
    double ax = std::abs(s[0]), ay = std::abs(s[1]), az = std::abs(s[2]);
    // Round shapes take their scale into their sizes when it keeps them round; otherwise they are
    // built at their own size and stretched afterwards (into exact spline surfaces).
    const bool roundKind = shape == QLatin1String("cylinder") || shape == QLatin1String("cone") || shape == QLatin1String("tube");
    const bool evenKind = shape == QLatin1String("sphere") || shape == QLatin1String("hemisphere") || shape == QLatin1String("torus") ||
                          shape == QLatin1String("capsule");
    if ((roundKind && !same(ax, ay)) || (evenKind && !(same(ax, ay) && same(ay, az)))) {
      ax = ay = az = 1;
      *scaledIn = false;
    }
    if (shape == QLatin1String("box")) {
      return BRepPrimAPI_MakeBox(gp_Pnt(-x * ax / 2, -y * ay / 2, -z * az / 2), x * ax, y * ay, z * az).Shape();
    }
    if (shape == QLatin1String("cylinder") && n.thread.isEmpty() && n.sides >= 20) {
      const double r = std::max(n.radius, 0.05) * ax, h = z * az;
      return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -h / 2), gp::DZ()), r, h).Shape();
    }
    if (shape == QLatin1String("cone")) {
      const double r1 = std::max(n.radius, 0.05) * ax, r2 = std::max(n.radius2, 0.0) * ax, h = z * az;
      if (std::abs(r1 - r2) < 1e-9) return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -h / 2), gp::DZ()), r1, h).Shape();
      return BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0, 0, -h / 2), gp::DZ()), r1, r2, h).Shape();
    }
    if (shape == QLatin1String("tube")) {
      const double outer = std::max(n.radius, 0.4) * ax;
      const double inner = std::min(std::max(n.radius2, 0.05) * ax, outer - 0.05 * ax);
      const double h = z * az;
      const gp_Ax2 base(gp_Pnt(0, 0, -h / 2), gp::DZ());
      BRepBuilderAPI_MakeFace ring(BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(gp_Circ(base, outer))).Wire(), true);
      ring.Add(TopoDS::Wire(BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(gp_Circ(base, inner))).Wire().Reversed()));
      return BRepPrimAPI_MakePrism(ring.Face(), gp_Vec(0, 0, h)).Shape();
    }
    if (shape == QLatin1String("sphere")) return BRepPrimAPI_MakeSphere(gp::Origin(), std::max(n.radius, 0.05) * ax).Shape();
    if (shape == QLatin1String("hemisphere")) {
      const double r = std::max(n.radius, 0.05) * ax;
      return BRepPrimAPI_MakeSphere(gp_Ax2(gp_Pnt(0, 0, -r / 2), gp::DZ()), r, 0, kPi / 2).Shape();
    }
    if (shape == QLatin1String("torus")) {
      const double major = std::max(n.radius, 0.2);
      const double minor = std::min(std::max(n.radius2, 0.05), major * 0.95);
      return BRepPrimAPI_MakeTorus(major * ax, minor * ax).Shape();
    }
    if (shape == QLatin1String("capsule")) {
      const double r = std::max(n.radius, 0.05) * ax, h = z * az;
      const TopoDS_Shape body = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -h / 2), gp::DZ()), r, h).Shape();
      const TopoDS_Shape top = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, h / 2), r).Shape();
      const TopoDS_Shape bottom = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, -h / 2), r).Shape();
      return fuseAll({body, top, bottom});
    }
    if (shape == QLatin1String("cylinder") && !n.thread.isEmpty()) {
      *scaledIn = false;  // built at its own size, scaled after
      QString why;
      TopoDS_Shape out = threadExact(n, &why);
      if (out.IsNull()) curveWhy = why;
      return out;
    }
    if (shape == QLatin1String("text") || shape == QLatin1String("svg")) {
      // Built at its real size, stretched through its control points.
      const double stretch[3] = {ax, ay, az};
      QString why;
      TopoDS_Shape out = shape == QLatin1String("text") ? textExact(n, stretch, &why) : svgExact(n, stretch, &why);
      if (out.IsNull()) curveWhy = why;
      return out;
    }
    if (shape == QLatin1String("stl") && !n.stepSource.isEmpty()) {
      // A part from a STEP file: the file's own solid, moved the way its mesh was centred.
      const TopoDS_Shape* source = cachedStepSolid(n.stepSource, n.stepIndex);
      if (!source) return {};
      gp_Trsf centre;
      centre.SetTranslation(gp_Vec(-n.stepOffset[0], -n.stepOffset[1], -n.stepOffset[2]));
      *scaledIn = false;
      return moved(*source, centre);
    }
    // Shapes with only flat faces: their mesh is their exact form.
    static const QStringList flat = {QStringLiteral("prism"), QStringLiteral("pyramid"), QStringLiteral("wedge"),
                                     QStringLiteral("roof"),  QStringLiteral("star"),    QStringLiteral("starflat"),
                                     QStringLiteral("diamond")};
    if (flat.contains(shape) || (shape == QLatin1String("cylinder") && n.thread.isEmpty() && n.sides < 20)) {
      std::vector<double> points;
      std::vector<uint32_t> triangles;
      Node plain = n;
      plain.hole = false;
      plain.shellWall = 0;  // hollowed (and mirrored, placed) below, the exact way
      plain.mirror[0] = plain.mirror[1] = plain.mirror[2] = false;
      plain.edgeFillet = 0;  // rounded below, the exact way
      plain.edgeBeta.clear();
      plain.bend = plain.twist = 0;  // bent and twisted below
      bool hole = false;
      if (!nodeLocalMesh(doc, plain, &points, &triangles, &hole)) return {};
      for (size_t i = 0; i + 2 < points.size(); i += 3) {
        points[i] *= ax;
        points[i + 1] *= ay;
        points[i + 2] *= az;
      }
      int faces = 0;
      return facetedSolid(points, triangles, &faces, nullptr);
    }
    return {};
  }

  // Whether a shape's own settings leave it exact (no mesh-made fillet, bend, twist, thread...).
  // (Rounded edges, the shape's own and picked ones, are made exact too: see roundOwn, roundPicks.)
  static bool exactSettings(const Node& n) {
    return !(n.surfaceFillet > 0.02) && !n.faceBend && !n.faceExtrude && !(n.shellWall > 0.01 && n.shellOpen != 0) &&
           !(std::abs(n.taper - 100) > 0.05) && !(n.kind == Node::Kind::Shape && n.shape == QLatin1String("text") && n.textArc > 0.01);
  }

  // The shape's own Fillet as the mesh made it: the edges it rounded, with the radius it used.
  // `frame` scales the mesh to the frame `shape` is in; r is the radius in that frame.
  TopoDS_Shape roundOwn(const TopoDS_Shape& shape, const Node& n, const double frame[3], double r, QString* why,
                        bool mirrored = true) {
    Node plain = n;
    plain.hole = false;
    plain.shellWall = 0;
    plain.edgeBeta.clear();
    plain.bend = plain.twist = 0;  // rounded before a bend or twist
    if (!mirrored) plain.mirror[0] = plain.mirror[1] = plain.mirror[2] = false;
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    bool hole = false;
    if (!nodeLocalMesh(doc, plain, &points, &triangles, &hole)) {
      *why = QStringLiteral("its rounded mesh could not be made");
      return {};
    }
    for (size_t i = 0; i + 2 < points.size(); i += 3)
      for (int a = 0; a < 3; ++a) points[i + static_cast<size_t>(a)] *= frame[a];
    std::vector<std::pair<TopoDS_Edge, double>> edges;
    for (const TopoDS_Edge& edge : edgesRoundedIn(shape, points, triangles, r)) edges.emplace_back(edge, r);
    const TopoDS_Shape out = filleted(shape, edges);
    if (out.IsNull()) *why = QStringLiteral("OpenCASCADE could not round its edges (%1 mm)").arg(r, 0, 'g', 4);
    return out;
  }

  // Picked edge fillets and chamfers, on the part at its real size (`size`: the scale, positive).
  TopoDS_Shape roundPicks(const TopoDS_Shape& shape, const Node& n, const double size[3], QString* why) {
    std::vector<ef::Pick> picks = ef::decodePicks(n.edgeBeta);
    if (picks.empty()) return shape;
    for (ef::Pick& pick : picks)
      for (size_t i = 0; i + 2 < pick.pts.size(); i += 3)
        for (int a = 0; a < 3; ++a) pick.pts[i + static_cast<size_t>(a)] *= size[a];
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    const double tolerance = std::max(0.03, std::sqrt(box.SquareExtent()) * 2e-4);
    auto collect = [&](const TopoDS_Shape& on, bool chamfers, std::vector<std::pair<TopoDS_Edge, double>>* out) {
      const std::vector<EdgeOf> edges = edgesOf(on);
      int number = 0;
      for (const ef::Pick& pick : picks) {
        ++number;
        if (pick.chamfer != chamfers) continue;
        const std::vector<EdgeOf> along = edgesAlong(edges, pick.pts, tolerance);
        if (along.empty()) {
          *why = QStringLiteral("picked edge %1 is not on the exact solid").arg(number);
          return false;
        }
        for (const EdgeOf& e : along) {
          if (std::any_of(out->begin(), out->end(), [&](const auto& known) { return known.first.IsSame(e.edge); })) continue;
          double size = std::abs(pick.radius);
          if (chamfers) {
            // The mesh's chamfer joins the two points where a fillet of that radius would start.
            const double turn = edgeTurn(e);
            if (turn > 0.05) size *= std::tan(turn * 0.5);
          }
          out->emplace_back(e.edge, size);
        }
      }
      return true;
    };
    std::vector<std::pair<TopoDS_Edge, double>> rounds, flats;
    if (!collect(shape, false, &rounds)) return {};
    TopoDS_Shape out = filleted(shape, rounds);
    if (out.IsNull()) {
      *why = QStringLiteral("OpenCASCADE could not round the picked edges");
      return {};
    }
    if (!collect(out, true, &flats)) return {};
    out = chamfered(out, flats);
    if (out.IsNull()) *why = QStringLiteral("OpenCASCADE could not chamfer the picked edges");
    return out;
  }

  // Why a shape's own settings keep it from being exact.
  static QString settingsReason(const Node& n) {
    QStringList what;
    if (n.surfaceFillet > 0.02) what << QStringLiteral("a surface fillet");
    if (n.faceBend) what << QStringLiteral("a bent face");
    if (n.faceExtrude) what << QStringLiteral("a pulled face");
    if (n.shellWall > 0.01 && n.shellOpen != 0) what << QStringLiteral("a hollow open on a side");
    if (n.kind == Node::Kind::Group && n.hull) what << QStringLiteral("a hull");
    if (std::abs(n.taper - 100) > 0.05) what << QStringLiteral("a taper");
    if (n.kind == Node::Kind::Shape && n.shape == QLatin1String("text") && n.textArc > 0.01) what << QStringLiteral("text on a curve");
    return what.isEmpty() ? QStringLiteral("its settings") : what.join(QStringLiteral(", ")) + QStringLiteral(" (made on the mesh)");
  }

  // Why a shape with plain settings still has no exact form.
  static QString kindReason(const Node& n) {
    const QString& shape = n.shape;
    if (shape == QLatin1String("text")) return QStringLiteral("text: ") + QStringLiteral("its curves could not be used");
    if (shape == QLatin1String("svg")) return QStringLiteral("2D shape: ") + QStringLiteral("its curves could not be used");
    if (shape == QLatin1String("stl") && n.sculpt) return QStringLiteral("a sculpt is a freeform mesh");
    if (shape == QLatin1String("stl"))
      return n.stepSource.isEmpty() ? QStringLiteral("an STL is a mesh")
                                    : QStringLiteral("its STEP file could not be read (%1)").arg(QDir::toNativeSeparators(n.stepSource));
    if (shape == QLatin1String("generator")) {
      const gen::Spec* spec = gen::spec(n.genType);
      return QStringLiteral("generated parts (%1) are made from triangles").arg(spec ? spec->name : n.genType);
    }
    if (!n.thread.isEmpty()) return QStringLiteral("threads are made from triangles");
    if (shape == QLatin1String("facefillet")) return QStringLiteral("Fillet surface pieces are made from triangles");
    return QStringLiteral("a %1 has no exact form here").arg(shape);
  }

  // A piece from its mesh, at real size, before placement; `reason` says why, for the log.
  TopoDS_Shape fromMesh(const Node& n, const double s[3], bool* hole, const QString& reason) {
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    if (!nodeLocalMesh(doc, n, &points, &triangles, hole)) return {};
    for (size_t i = 0; i + 2 < points.size(); i += 3) {
      points[i] *= s[0];
      points[i + 1] *= s[1];
      points[i + 2] *= s[2];
    }
    if (s[0] * s[1] * s[2] < 0)  // a mirror turns the triangles inside out
      for (size_t t = 0; t + 2 < triangles.size(); t += 3) std::swap(triangles[t + 1], triangles[t + 2]);
    int faces = 0;
    QString why;
    Recognised found;
    TopoDS_Shape shape = recognisedSolid(points, triangles, &faces, &why, &found);
    if (shape.IsNull()) {
      fail(QStringLiteral("%1: %2").arg(n.name, why));
      return {};
    }
    meshFaces += faces;
    if (found.complete) {
      ++exactPieces;  // every curve in it recognised: as exact as a part built exactly
    } else if (found.cylinders || found.cones) {
      ++meshPieces;
      ++recognisedPieces;
      meshWhy.emplace_back(n.name, reason + QStringLiteral("; recognised and made exact: %1").arg(found.summary()));
    } else {
      ++meshPieces;
      meshWhy.emplace_back(n.name, reason);
    }
    if (meshFaces > kMostMeshFaces) fail(QStringLiteral("too much of it is mesh"));
    return shape;
  }

  // The box of a node's mesh as it is just before its twist (at its real size) or its bend.
  bool meshBox(Node probe, bool forTwist, const double scale[3], double lo[3], double hi[3]) {
    probe.hole = false;
    probe.shellWall = 0;
    probe.edgeBeta.clear();
    probe.bend = 0;
    if (forTwist) probe.twist = 0;
    probe.mirror[0] = probe.mirror[1] = probe.mirror[2] = false;
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    bool hole = false;
    if (!nodeLocalMesh(doc, probe, &points, &triangles, &hole) || points.empty()) return false;
    for (int a = 0; a < 3; ++a) lo[a] = 1e300, hi[a] = -1e300;
    for (size_t i = 0; i + 2 < points.size(); i += 3)
      for (int a = 0; a < 3; ++a) {
        const double v = points[i + static_cast<size_t>(a)] * (forTwist ? scale[a] : 1.0);
        lo[a] = std::min(lo[a], v);
        hi[a] = std::max(hi[a], v);
      }
    return true;
  }

  // A node's twist and bend, as the mesh makes them (at its own size, before its mirror and its
  // placement): its surfaces warped through the same formulas, from the same boxes.
  TopoDS_Shape twistedAndBent(TopoDS_Shape shape, const Node& n, QString* why) {
    if (std::abs(n.twist) >= 0.05) {
      double s[3], lo[3], hi[3];
      for (int a = 0; a < 3; ++a) s[a] = std::abs(n.scale[a]) > 1e-9 ? n.scale[a] : 1.0;
      const int axis = std::clamp(n.twistAxis, 0, 2);
      if (meshBox(n, true, s, lo, hi) && hi[axis] - lo[axis] > 1e-4) {
        shape = warped(shape, twistWarp(n.twist, axis, s, lo, hi), (hi[axis] - lo[axis]) / std::abs(s[axis]), why);
        if (shape.IsNull()) {
          *why = QStringLiteral("its twist: ") + *why;
          return {};
        }
      }
    }
    const int along = std::clamp(n.along, 0, 2), toward = std::clamp(n.toward, 0, 2);
    if (std::abs(n.bend) >= 0.05 && along != toward) {
      const double one[3] = {1, 1, 1};
      double lo[3], hi[3];
      if (meshBox(n, false, one, lo, hi) && hi[along] - lo[along] > 1e-4) {
        shape = warped(shape, bendWarp(n.bend, along, toward, lo, hi), hi[along] - lo[along], why);
        if (shape.IsNull()) {
          *why = QStringLiteral("its bend: ") + *why;
          return {};
        }
      }
    }
    return shape;
  }

  // A node placed at pos/rot/extra/scale (its own, or a repeat's copy).
  XParts node(const Node& n, const double pos[3], const double rot[3], const double extra[3], const double s[3]) {
    XParts out;
    if (failed) return out;
    const gp_Trsf place = rigid(pos, rot, extra);
    if (n.kind == Node::Kind::Pattern) {
      const auto kids = doc.children(n.id);
      if (kids.empty()) return out;
      const Node& child = *kids.front();
      std::vector<XParts> copies;
      for (int i = 0; i < std::max(1, n.count); ++i) {
        double p[3], e[3], cs[3];
        for (int a = 0; a < 3; ++a) {
          p[a] = child.pos[a] + n.move[a] * i;
          e[a] = n.drot[a] * i;
          cs[a] = child.scale[a] * std::pow(n.resize[a], i);
        }
        copies.push_back(node(child, p, child.rot, e, cs));
      }
      XParts all = combine(copies, false);
      if (all.hasSolid) all.solid = moved(scaled(all.solid, s), place);
      if (all.hasHole) all.hole = moved(scaled(all.hole, s), place);
      return all;
    }
    if (n.kind == Node::Kind::Group) {
      const bool groupExact = !n.hull && exactSettings(n);
      if (groupExact) {
        const int exactBefore = exactPieces, meshBefore = meshPieces, facesBefore = meshFaces;
        const auto whyBefore = meshWhy;
        std::vector<XParts> kids;
        for (const Node* child : doc.children(n.id)) kids.push_back(node(*child, child->pos, child->rot, kZero, child->scale));
        if (failed) return out;
        XParts inner = combine(kids, n.intersect);
        if (failed) return out;
        QString roundWhy;
        TopoDS_Shape solid;
        const bool warpedGroup = std::abs(n.twist) >= 0.05 || std::abs(n.bend) >= 0.05;
        if (warpedGroup && inner.hasHole) roundWhy = QStringLiteral("a bent or twisted group with a loose hole in it");
        if (inner.hasSolid && !(warpedGroup && inner.hasHole)) {
          const double size[3] = {std::abs(s[0]), std::abs(s[1]), std::abs(s[2])};
          const double signs[3] = {s[0] < 0 ? -1.0 : 1.0, s[1] < 0 ? -1.0 : 1.0, s[2] < 0 ? -1.0 : 1.0};
          solid = warpedGroup ? twistedAndBent(inner.solid, n, &roundWhy) : inner.solid;
          if (!solid.IsNull()) solid = scaled(solid, size);
          if (!n.edgeBeta.empty()) {
            // Faces and edges the join split along one surface become one again first, so a
            // picked edge is one edge here too.
            if (splineFaces(solid) <= 4) {
              ShapeUpgrade_UnifySameDomain unify(solid, true, true, false);
              unify.Build();
              if (!unify.Shape().IsNull()) solid = unify.Shape();
            }
            solid = roundPicks(solid, n, size, &roundWhy);
          }
          if (!solid.IsNull()) solid = scaled(solid, signs);
        }
        if (inner.hasSolid && solid.IsNull()) {
          // Its picked edges could not be rounded exactly: the whole group from its mesh.
          exactPieces = exactBefore;
          meshPieces = meshBefore;
          meshFaces = facesBefore;
          meshWhy = whyBefore;
          bool hole = false;
          const TopoDS_Shape piece = fromMesh(n, s, &hole, roundWhy);
          if (piece.IsNull()) return out;
          (hole ? out.hole : out.solid) = moved(piece, place);
          (hole ? out.hasHole : out.hasSolid) = true;
          return out;
        }
        if (inner.hasSolid) {
          if (n.shellWall > 0.01) {
            solid = hollowed(solid, n.shellWall);
            if (solid.IsNull()) return fail(QStringLiteral("hollowing failed")), out;
          }
          if (n.hole) {
            const double c = n.clearanceMode == 0 ? 0 : std::clamp(n.clearanceMode == 1 ? printerClearance() : n.holeClearance, 0.0, 5.0);
            solid = grown(solid, c, n.clearanceSidesOnly);
            if (solid.IsNull()) return fail(QStringLiteral("a hole clearance failed")), out;
            out.hole = moved(solid, place);
            out.hasHole = true;
          } else {
            out.solid = moved(solid, place);
            out.hasSolid = true;
          }
        }
        if (inner.hasHole) {
          out.hole = moved(scaled(inner.hole, s), place);
          out.hasHole = true;
        }
        return out;
      }
      bool hole = false;
      const TopoDS_Shape piece = fromMesh(n, s, &hole, settingsReason(n));
      if (piece.IsNull()) return out;
      (hole ? out.hole : out.solid) = moved(piece, place);
      (hole ? out.hasHole : out.hasSolid) = true;
      return out;
    }
    // A shape.
    bool scaledIn = true;
    QString reason;
    TopoDS_Shape shape;
    const bool warpedShape = std::abs(n.twist) >= 0.05 || std::abs(n.bend) >= 0.05;
    const double unitScale[3] = {1, 1, 1};
    if (!exactSettings(n)) reason = settingsReason(n);
    else {
      curveWhy.clear();
      // A bent or twisted shape is made at its own size: rounded, twisted, bent, mirrored there.
      shape = primitive(n, warpedShape ? unitScale : s, &scaledIn);
      if (shape.IsNull())
        reason = curveWhy.isEmpty() ? kindReason(n)
                                    : QStringLiteral("%1: %2").arg(n.shape == QLatin1String("text") ? QStringLiteral("text")
                                                                   : n.shape == QLatin1String("svg") ? QStringLiteral("2D shape")
                                                                                                     : QStringLiteral("thread"),
                                                                   curveWhy);
    }
    bool hole = n.hole;
    const double size[3] = {std::abs(s[0]), std::abs(s[1]), std::abs(s[2])};
    const double signs[3] = {s[0] < 0 ? -1.0 : 1.0, s[1] < 0 ? -1.0 : 1.0, s[2] < 0 ? -1.0 : 1.0};
    const double unit[3] = {1, 1, 1};
    if (!shape.IsNull() && warpedShape) {
      const double r = edgeFilletUsed(n);
      const bool plainScale = same(size[0], 1) && same(size[1], 1) && same(size[2], 1);
      if (r > 0 && n.shape == QLatin1String("box") && !plainScale) {
        shape.Nullify();
        reason = QStringLiteral("rounded edges on a stretched box that is bent or twisted (made on the mesh)");
      } else if (r > 0) {
        shape = roundOwn(shape, n, unitScale, r, &reason, false);
      }
      if (!shape.IsNull()) shape = twistedAndBent(shape, n, &reason);
      if (!shape.IsNull())
        for (int axis = 0; axis < 3; ++axis)
          if (n.mirror[axis]) {
            gp_Trsf m;
            m.SetMirror(gp_Ax2(gp::Origin(), axis == 0 ? gp::DX() : axis == 1 ? gp::DY() : gp::DZ()));
            shape = moved(shape, m);
          }
      if (!shape.IsNull()) shape = scaled(shape, size);
      if (!shape.IsNull() && !n.edgeBeta.empty()) shape = roundPicks(shape, n, size, &reason);
      if (!shape.IsNull()) shape = scaled(shape, signs);
    } else if (!shape.IsNull()) {
      // In the order the mesh is made: its own mirror and Fillet (in the frame the mesh rounds
      // in), at its real size, picked edges, then the mirror of a negative scale.
      for (int axis = 0; axis < 3; ++axis)
        if (n.mirror[axis]) {
          gp_Trsf m;
          m.SetMirror(gp_Ax2(gp::Origin(), axis == 0 ? gp::DX() : axis == 1 ? gp::DY() : gp::DZ()));
          shape = moved(shape, m);
        }
      const double r = edgeFilletUsed(n);
      if (r > 0) {
        const bool box = n.shape == QLatin1String("box");
        if (!box && scaledIn && !(same(size[0], size[1]) && same(size[1], size[2]))) {
          shape.Nullify();
          reason = QStringLiteral("rounded edges on a stretched %1 (made on the mesh)").arg(n.shape);
        } else {
          shape = roundOwn(shape, n, scaledIn ? size : unit, box || !scaledIn ? r : r * size[0], &reason);
        }
      }
      if (!shape.IsNull() && !scaledIn) shape = scaled(shape, size);
      if (!shape.IsNull() && !n.edgeBeta.empty()) shape = roundPicks(shape, n, size, &reason);
      if (!shape.IsNull()) shape = scaled(shape, signs);
    }
    if (!shape.IsNull()) {
      if (n.hole) {
        const double c = n.clearanceMode == 0 ? 0 : std::clamp(n.clearanceMode == 1 ? printerClearance() : n.holeClearance, 0.0, 5.0);
        shape = grown(shape, c, n.clearanceSidesOnly);
        if (shape.IsNull()) reason = QStringLiteral("OpenCASCADE could not grow it by its hole clearance");
      } else if (n.shellWall > 0.01) {
        shape = hollowed(shape, n.shellWall);
        if (shape.IsNull()) reason = QStringLiteral("OpenCASCADE could not hollow it");
      }
      if (!shape.IsNull()) ++exactPieces;
    }
    if (shape.IsNull()) {
      shape = fromMesh(n, s, &hole, reason);
      if (shape.IsNull()) return out;
    }
    (hole ? out.hole : out.solid) = moved(shape, place);
    (hole ? out.hasHole : out.hasSolid) = true;
    return out;
  }
};

// --- Writing ---------------------------------------------------------------------------------------

bool listed(const QStringList& only, const QString& id) { return only.contains(id); }
bool ancestorListed(const Document& doc, const QStringList& only, const QString& id) {
  const Node* n = doc.find(id);
  for (QString parent = n ? n->parentId : QString(); !parent.isEmpty();) {
    if (only.contains(parent)) return true;
    const Node* p = doc.find(parent);
    parent = p ? p->parentId : QString();
  }
  return false;
}

TDF_Label addPart(const Handle(XCAFDoc_ShapeTool)& shapes, const Handle(XCAFDoc_ColorTool)& colours, const TopoDS_Shape& shape,
                  const QString& name, const QColor& color) {
  const TDF_Label label = shapes->AddShape(shape, false);
  TDataStd_Name::Set(label, TCollection_ExtendedString(reinterpret_cast<const char16_t*>(name.utf16())));
  colours->SetColor(label, Quantity_Color(color.redF(), color.greenF(), color.blueF(), Quantity_TOC_sRGB), XCAFDoc_ColorSurf);
  return label;
}

// "6 planes, 2 cylinders, 1 cone": the kinds of surface an exact solid has.
QString surfaceSummary(const TopoDS_Shape& solid) {
  std::map<int, int> counts;
  for (TopExp_Explorer e(solid, TopAbs_FACE); e.More(); e.Next()) ++counts[BRepAdaptor_Surface(TopoDS::Face(e.Current())).GetType()];
  static const std::map<int, std::pair<const char*, const char*>> names = {
      {GeomAbs_Plane, {"plane", "planes"}},          {GeomAbs_Cylinder, {"cylinder", "cylinders"}},
      {GeomAbs_Cone, {"cone", "cones"}},             {GeomAbs_Sphere, {"sphere", "spheres"}},
      {GeomAbs_Torus, {"torus", "tori"}},            {GeomAbs_BSplineSurface, {"spline surface", "spline surfaces"}},
      {GeomAbs_SurfaceOfRevolution, {"revolved surface", "revolved surfaces"}},
      {GeomAbs_SurfaceOfExtrusion, {"curved side", "curved sides"}},
      {GeomAbs_BezierSurface, {"Bézier surface", "Bézier surfaces"}}};
  QStringList parts;
  int other = 0;
  for (const auto& [type, count] : counts) {
    const auto found = names.find(type);
    if (found == names.end()) {
      other += count;
      continue;
    }
    parts << QStringLiteral("%1 %2").arg(count).arg(QString::fromLatin1(count == 1 ? found->second.first : found->second.second));
  }
  if (other) parts << QStringLiteral("%1 other").arg(other);
  return parts.join(QStringLiteral(", "));
}

}  // namespace

bool writeStep(const Document& doc, const QStringList& only, const std::vector<ExportObject>& objects, const QString& path,
               QString* error, Report* report) {
  Report local;
  Report& r = report ? *report : local;
  const auto progress = r.progress;
  r = Report();
  r.progress = progress;
  quiet();
  int step = 0, total = 1;
  auto say = [&](const QString& text) {
    const QString line = text.isEmpty() ? QStringLiteral(" ") : text;  // empty means "no line" to progress
    r.log << line;
    if (r.progress) r.progress(step, total, line);
  };
  auto tick = [&] {
    ++step;
    if (r.progress) r.progress(step, total, QString());
  };
  // When OpenCASCADE cannot take even a part's mesh, the whole file is written the faceted way,
  // which always works for closed meshes.
  auto fallBack = [&](const QString& name) {
    const QStringList before = r.log;
    r = Report();
    r.progress = progress;
    r.log = before;
    r.faceted = static_cast<int>(objects.size());
    r.notes << QStringLiteral("All parts written from their meshes (%1 could not be read as a solid by OpenCASCADE).").arg(name);
    say(QString());
    say(QStringLiteral("OpenCASCADE could not read %1 as a solid, so the whole file is written the faceted way: "
                       "every part from its mesh.").arg(name));
    const bool ok = ::writeStep(objects, path, error);
    step = total;
    say(ok ? QStringLiteral("Written.") : QStringLiteral("Not written: %1").arg(error ? *error : QString()));
    return ok;
  };
  try {
    // The same parts as the mesh exports: everything on the plate, or what is selected.
    struct Item {
      const Node* node;
      std::vector<const Node*> ancestors;
    };
    std::vector<Item> items;
    std::function<void(const Node&, std::vector<const Node*>)> walk = [&](const Node& n, std::vector<const Node*> up) {
      const bool chosen = only.isEmpty() || (listed(only, n.id) && !ancestorListed(doc, only, n.id));
      if (only.isEmpty() || chosen) {
        items.push_back({&n, up});
        return;
      }
      up.push_back(&n);
      for (const Node* child : doc.children(n.id)) walk(*child, up);
    };
    for (const Node* n : doc.children(QString())) walk(*n, {});
    total = static_cast<int>(items.size()) * 2 + 1;
    say(QStringLiteral("Building %1 part%2 as exact solids…").arg(items.size()).arg(items.size() == 1 ? "" : "s"));

    auto build = [&](const Item& item, Builder& b) {
      XParts parts = b.node(*item.node, item.node->pos, item.node->rot, kZero, item.node->scale);
      for (auto it = item.ancestors.rbegin(); it != item.ancestors.rend(); ++it) {
        const gp_Trsf place = rigid((*it)->pos, (*it)->rot, kZero);
        if (parts.hasSolid) parts.solid = moved(scaled(parts.solid, (*it)->scale), place);
        if (parts.hasHole) parts.hole = moved(scaled(parts.hole, (*it)->scale), place);
      }
      return parts;
    };
    // Every part built once; holes left loose on the plate cut every part they reach.
    struct Built {
      XParts parts;
      Builder builder;
    };
    std::vector<Built> built;
    built.reserve(items.size());
    std::vector<TopoDS_Shape> looseHoles;
    bool looseExact = true;
    for (const Item& item : items) {
      Built one{XParts(), Builder{doc}};
      try {
        one.parts = build(item, one.builder);
      } catch (const Standard_Failure& failure) {
        one.parts = XParts();
        one.builder.fail(QString::fromUtf8(failureText(failure)));
      }
      if (one.parts.hasHole && !one.parts.hasSolid) {
        looseHoles.push_back(one.parts.hole);
        if (one.builder.failed) looseExact = false;
      }
      built.push_back(std::move(one));
      tick();
    }
    const TopoDS_Shape cutter = fuseAll(looseHoles);
    if (!looseHoles.empty() && !solidIn(cutter)) looseExact = false;

    Handle(TDocStd_Document) xdoc;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", xdoc);
    const Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(xdoc->Main());
    const Handle(XCAFDoc_ColorTool) colours = XCAFDoc_DocumentTool::ColorTool(xdoc->Main());

    say(QString());
    std::map<QString, bool> done;  // parts written exactly, by node id
    const QString indent = QStringLiteral("      ");
    auto meshLines = [&](const Builder& b, const Node& part) {
      for (const auto& [piece, why] : b.meshWhy)
        say(indent + (piece == part.name && b.meshWhy.size() == 1 ? why
                                                                   : QStringLiteral("from the mesh: “%1”: %2").arg(piece, why)));
    };
    for (size_t index = 0; index < items.size(); ++index) {
      const Item& item = items[index];
      const QString id = item.node->id;
      const QString name = QStringLiteral("“%1”").arg(item.node->name);
      const Builder& b = built[index].builder;
      const XParts& parts = built[index].parts;
      // Its mesh objects: one, or one per colour for a multi-colour group (those stay meshes).
      bool multi = false, any = false;
      for (const ExportObject& o : objects)
        if (o.nodeId == id) {
          any = true;
          multi = multi || !o.assembly.isEmpty();
        }
      if (parts.hasHole && !parts.hasSolid && !any) {
        say(QStringLiteral("%1: a hole on the plate, cuts the parts it reaches%2")
                .arg(name, b.failed || b.meshPieces ? QStringLiteral(" (from its mesh)") : QStringLiteral(" (exact)")));
        if (b.failed) say(indent + QStringLiteral("OpenCASCADE: ") + b.why);
        meshLines(b, *item.node);
        tick();
        continue;
      }
      if (!any) {
        tick();
        continue;
      }
      if (multi) {
        say(QStringLiteral("%1: FROM THE MESH, one solid per colour (a group that keeps its parts' colours)").arg(name));
        tick();
        continue;
      }
      auto fromMeshBecause = [&](const QString& why) {
        say(QStringLiteral("%1: FROM THE MESH").arg(name));
        if (!why.isEmpty()) say(indent + why);
        tick();
      };
      if (b.failed) {
        fromMeshBecause(QStringLiteral("OpenCASCADE: ") + b.why);
        if (b.exactPieces > 0) r.notes << QStringLiteral("%1: written from its mesh (%2).").arg(item.node->name, b.why);
        meshLines(b, *item.node);
        continue;
      }
      if (!parts.hasSolid) {
        fromMeshBecause(QStringLiteral("nothing solid came out of the exact build"));
        continue;
      }
      if (b.exactPieces == 0 && b.recognisedPieces == 0) {
        fromMeshBecause(QString());
        meshLines(b, *item.node);
        continue;
      }
      if (!looseExact) {
        fromMeshBecause(QStringLiteral("a hole lying loose on the plate could not be made exact, so every part is written as it cuts them: "
                                       "from the mesh"));
        continue;
      }
      const ExportObject* named = nullptr;
      for (const ExportObject& o : objects)
        if (o.nodeId == id) named = &o;
      TopoDS_Shape solid = parts.solid;
      QString problem;
      try {
        if (!looseHoles.empty()) {
          solid = boolean(1, {solid}, {cutter});
          if (!solidIn(solid)) problem = QStringLiteral("the loose holes could not be cut");
        }
        if (problem.isEmpty()) {
          // Faces the booleans split that lie on one surface become one face again. (Not on a
          // part with many spline faces, a thread or a bend: there it takes minutes and finds little.)
          if (splineFaces(solid) <= 4) {
            ShapeUpgrade_UnifySameDomain unify(solid, true, true, false);
            unify.Build();
            if (!unify.Shape().IsNull()) solid = unify.Shape();
          }
          if (!BRepCheck_Analyzer(solid).IsValid()) {
            ShapeFix_Shape fix(solid);
            fix.Perform();
            solid = fix.Shape();
            if (!BRepCheck_Analyzer(solid).IsValid()) problem = QStringLiteral("the exact solid did not check out");
          }
        }
        // The same amount of material as the mesh (which has curves as flat facets, so a little
        // off), or something went wrong on the way.
        if (problem.isEmpty() && named && named->mesh.volume > 0) {
          GProp_GProps props;
          BRepGProp::VolumeProperties(solid, props);
          const double exactVolume = std::abs(props.Mass());
          if (std::abs(exactVolume - named->mesh.volume) > 0.05 * named->mesh.volume + 0.5)
            problem = QStringLiteral("the exact solid came out %1 cm³ instead of %2 cm³")
                          .arg(exactVolume / 1000.0, 0, 'f', 2)
                          .arg(named->mesh.volume / 1000.0, 0, 'f', 2);
        }
      } catch (const Standard_Failure& failure) {
        problem = QString::fromUtf8(failureText(failure));
      }
      if (!problem.isEmpty()) {
        r.notes << QStringLiteral("%1: written from its mesh (%2).").arg(item.node->name, problem);
        fromMeshBecause(problem);
        continue;
      }
      addPart(shapes, colours, solid, named ? named->name : item.node->name, named ? named->color : item.node->color);
      done[id] = true;
      if (b.meshPieces > 0) ++r.partly;
      else ++r.exact;
      say(QStringLiteral("%1: %2 · %3").arg(name, b.meshPieces > 0 ? QStringLiteral("PARTLY EXACT") : QStringLiteral("EXACT"),
                                              surfaceSummary(solid)));
      meshLines(b, *item.node);
      tick();
    }
    // Nothing exact at all: then the file is the faceted STEP, which DaedalCAD's own writer makes
    // in a fraction of the time OpenCASCADE's takes on a large mesh.
    if (done.empty()) {
      say(QString());
      say(QStringLiteral("Writing %1 (every part from its mesh)…").arg(QDir::toNativeSeparators(path)));
      r.faceted = static_cast<int>(objects.size());
      const bool ok = ::writeStep(objects, path, error);
      step = total;
      say(ok ? QStringLiteral("Done: %1 from the mesh.").arg(r.faceted) : QStringLiteral("Not written: %1").arg(error ? *error : QString()));
      return ok;
    }
    // Everything else from its mesh, as flat faces.
    for (const ExportObject& o : objects) {
      if (done.count(o.nodeId)) continue;
      int faces = 0;
      QString why;
      TopoDS_Shape solid;
      try {
        solid = facetedSolid(o.points, o.triangles, &faces, &why);
      } catch (const Standard_Failure&) {
        solid.Nullify();
      }
      if (solid.IsNull()) return fallBack(o.name);
      addPart(shapes, colours, solid, o.name, o.color);
      ++r.faceted;
    }
    say(QString());
    say(QStringLiteral("Writing %1…").arg(QDir::toNativeSeparators(path)));
    STEPCAFControl_Writer writer;
    writer.SetColorMode(true);
    writer.SetNameMode(true);
    Interface_Static::SetCVal("write.step.schema", "AP214IS");
    Interface_Static::SetCVal("write.step.unit", "MM");
    Interface_Static::SetCVal("write.step.product.name", "DaedalCAD");
    if (!writer.Transfer(xdoc, STEPControl_AsIs)) {
      if (error) *error = QStringLiteral("Could not put the parts into a STEP file.");
      return false;
    }
    const QString native = QDir::toNativeSeparators(path);
    if (writer.Write(native.toUtf8().constData()) != IFSelect_RetDone) {
      if (error) *error = QStringLiteral("Could not write %1.").arg(native);
      return false;
    }
    step = total;
    QStringList counts;
    if (r.exact) counts << QStringLiteral("%1 exact").arg(r.exact);
    if (r.partly) counts << QStringLiteral("%1 partly exact").arg(r.partly);
    if (r.faceted) counts << QStringLiteral("%1 from the mesh").arg(r.faceted);
    say(QStringLiteral("Done: %1.").arg(counts.join(QStringLiteral(", "))));
    return true;
  } catch (const Standard_Failure& failure) {
    return fallBack(QStringLiteral("a part (%1)").arg(QString::fromUtf8(failureText(failure))));
  } catch (const std::exception& failure) {
    return fallBack(QStringLiteral("a part (%1)").arg(QString::fromUtf8(failure.what())));
  }
}

// --- Reading ---------------------------------------------------------------------------------------

namespace {

QString labelName(const TDF_Label& label) {
  Handle(TDataStd_Name) name;
  if (label.FindAttribute(TDataStd_Name::GetID(), name))
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(name->Get().ToExtString()), name->Get().Length()).trimmed();
  return {};
}

bool labelColour(const Handle(XCAFDoc_ColorTool)& colours, const TDF_Label& label, QColor* out) {
  Quantity_Color c;
  if (colours->GetColor(label, XCAFDoc_ColorSurf, c) || colours->GetColor(label, XCAFDoc_ColorGen, c)) {
    double r, g, b;
    c.Values(r, g, b, Quantity_TOC_sRGB);
    *out = QColor::fromRgbF(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b));
    return true;
  }
  return false;
}

bool loadStep(const QString& path, std::vector<StepPart>* parts, QString* error) {
  STEPCAFControl_Reader reader;
  reader.SetColorMode(true);
  reader.SetNameMode(true);
  if (reader.ReadFile(QDir::toNativeSeparators(path).toUtf8().constData()) != IFSelect_RetDone) {
    if (error) *error = QStringLiteral("Could not read %1 as a STEP file.").arg(QFileInfo(path).fileName());
    return false;
  }
  Handle(TDocStd_Document) xdoc;
  XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", xdoc);
  if (!reader.Transfer(xdoc)) {
    if (error) *error = QStringLiteral("%1 holds nothing DaedalCAD can use.").arg(QFileInfo(path).fileName());
    return false;
  }
  const Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(xdoc->Main());
  const Handle(XCAFDoc_ColorTool) colours = XCAFDoc_DocumentTool::ColorTool(xdoc->Main());
  // Walk assemblies down to their parts, placing each where the assembly puts it; every solid
  // becomes a part of its own.
  std::function<void(const TDF_Label&, const TopLoc_Location&, const QString&, const QColor*)> walk =
      [&](const TDF_Label& label, const TopLoc_Location& at, const QString& outerName, const QColor* outerColour) {
        TDF_Label ref = label;
        TopLoc_Location here = at;
        if (shapes->IsReference(label)) {
          shapes->GetReferredShape(label, ref);
          here = at * shapes->GetLocation(label);
        }
        QString name = labelName(label);
        if (name.isEmpty() || shapes->IsReference(label)) {
          const QString refName = labelName(ref);
          if (!refName.isEmpty()) name = refName;
        }
        if (name.isEmpty()) name = outerName;
        QColor colour;
        bool hasColour = labelColour(colours, label, &colour) || labelColour(colours, ref, &colour);
        if (!hasColour && outerColour) {
          colour = *outerColour;
          hasColour = true;
        }
        if (shapes->IsAssembly(ref)) {
          NCollection_Sequence<TDF_Label> components;
          XCAFDoc_ShapeTool::GetComponents(ref, components);
          for (int i = 1; i <= components.Length(); ++i) walk(components.Value(i), here, name, hasColour ? &colour : nullptr);
          return;
        }
        const TopoDS_Shape shape = shapes->GetShape(ref).Moved(here);
        int count = 0;
        for (TopExp_Explorer e(shape, TopAbs_SOLID); e.More(); e.Next()) ++count;
        int k = 0;
        for (TopExp_Explorer e(shape, TopAbs_SOLID); e.More(); e.Next()) {
          StepPart part;
          part.name = count > 1 ? QStringLiteral("%1 %2").arg(name.isEmpty() ? QStringLiteral("Part") : name).arg(++k)
                                : (name.isEmpty() ? QStringLiteral("Part") : name);
          part.color = colour;
          part.hasColor = hasColour;
          part.shape = e.Current();
          parts->push_back(std::move(part));
        }
      };
  NCollection_Sequence<TDF_Label> free;
  shapes->GetFreeShapes(free);
  for (int i = 1; i <= free.Length(); ++i) walk(free.Value(i), TopLoc_Location(), QFileInfo(path).completeBaseName(), nullptr);
  if (parts->empty()) {
    if (error) *error = QStringLiteral("%1 has no solids (only surfaces or curves).").arg(QFileInfo(path).fileName());
    return false;
  }
  return true;
}

}  // namespace

bool readStep(const QString& path, std::vector<ImportedPart>* out, QString* error) {
  quiet();
  try {
    std::vector<StepPart> parts;
    if (!loadStep(path, &parts, error)) return false;
    for (size_t i = 0; i < parts.size(); ++i) {
      const TopoDS_Shape& shape = parts[i].shape;
      Bnd_Box box;
      BRepBndLib::Add(shape, box);
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      const double diagonal = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
      // Fine enough to print: a hundredth of a millimetre on small parts, a little more on big ones.
      BRepMesh_IncrementalMesh mesher(shape, std::clamp(diagonal * 2e-4, 0.005, 0.1), false, 0.2, true);
      ImportedPart part;
      part.name = parts[i].name;
      part.color = parts[i].color;
      part.hasColor = parts[i].hasColor;
      part.index = static_cast<int>(i);
      std::vector<double> raw;
      for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
        const TopoDS_Face& face = TopoDS::Face(e.Current());
        TopLoc_Location loc;
        const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull()) continue;
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        for (int t = 1; t <= tri->NbTriangles(); ++t) {
          int a, b, c;
          tri->Triangle(t).Get(a, b, c);
          if (reversed) std::swap(b, c);
          for (int v : {a, b, c}) {
            const gp_Pnt p = tri->Node(v).Transformed(loc.Transformation());
            raw.insert(raw.end(), {p.X(), p.Y(), p.Z()});
          }
        }
      }
      if (raw.empty()) continue;
      double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
      for (size_t k = 0; k + 2 < raw.size(); k += 3)
        for (int a = 0; a < 3; ++a) {
          lo[a] = std::min(lo[a], raw[k + static_cast<size_t>(a)]);
          hi[a] = std::max(hi[a], raw[k + static_cast<size_t>(a)]);
        }
      for (int a = 0; a < 3; ++a) part.centre[a] = (lo[a] + hi[a]) * 0.5;
      part.triangles.reserve(raw.size());
      for (size_t k = 0; k < raw.size(); ++k) part.triangles.push_back(static_cast<float>(raw[k] - part.centre[k % 3]));
      out->push_back(std::move(part));
    }
    if (out->empty()) {
      if (error) *error = QStringLiteral("Could not turn the solids of %1 into triangles.").arg(QFileInfo(path).fileName());
      return false;
    }
    return true;
  } catch (const Standard_Failure& failure) {
    if (error) *error = QStringLiteral("OpenCASCADE: %1").arg(QString::fromUtf8(failureText(failure)));
    return false;
  }
}

}  // namespace exact

#endif
