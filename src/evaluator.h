#pragma once

#include "document.h"
#include "textshape.h"

#include <QStringList>

#include <QColor>
#include <QMatrix4x4>
#include <QString>
#include <QStringList>
#include <QVector3D>

#include <cstdint>
#include <memory>
#include <vector>

struct CpuMesh {
  // Indexed triangles: x, y, z per vertex and three vertex numbers per triangle (counter-clockwise
  // seen from outside). The indices keep Manifold's connectivity, including MeshGL's
  // mergeFromVert/mergeToVert, so exports are watertight. The viewport draws these as they are and
  // works out each face's normal on the graphics card (about 18 bytes a triangle, not 90).
  std::vector<float> vertices;
  std::vector<uint32_t> indices;
  size_t triCount() const { return indices.size() / 3; }
  // Corner k (0..2) of triangle t.
  const float* corner(size_t t, int k) const { return &vertices[static_cast<size_t>(indices[t * 3 + static_cast<size_t>(k)]) * 3]; }
  // Sets lo, hi and hasBounds from the vertices.
  void updateBounds() {
    hasBounds = false;
    if (vertices.size() < 3) return;
    for (int axis = 0; axis < 3; ++axis) lo[axis] = 1e30f, hi[axis] = -1e30f;
    for (size_t i = 0; i + 2 < vertices.size(); i += 3)
      for (int axis = 0; axis < 3; ++axis) {
        lo[axis] = vertices[i + static_cast<size_t>(axis)] < lo[axis] ? vertices[i + static_cast<size_t>(axis)] : lo[axis];
        hi[axis] = vertices[i + static_cast<size_t>(axis)] > hi[axis] ? vertices[i + static_cast<size_t>(axis)] : hi[axis];
      }
    hasBounds = lo[0] <= hi[0];
  }
  int triangles = 0;
  double volume = 0;
  bool hasBounds = false;
  float lo[3] = {0, 0, 0};
  float hi[3] = {0, 0, 0};
};

struct DrawItem {
  QString id;
  int copyIndex = 0;
  QMatrix4x4 model;
  QMatrix4x4 parentWorld;
  QColor color;
  bool hole = false;
  bool selected = false;
  bool gizmo = false;
  std::shared_ptr<const CpuMesh> mesh;
};

inline const CpuMesh& meshOf(const std::shared_ptr<const CpuMesh>& mesh) {
  static const CpuMesh empty;
  return mesh ? *mesh : empty;
}

QMatrix4x4 placementMatrix(const double pos[3], const double rot[3], const double extra[3], const double scale[3]);

void decomposePlacement(const QMatrix4x4& matrix, double pos[3], double rot[3], double scale[3]);
void decomposePlacementNear(const QMatrix4x4& matrix, const double referenceRot[3], double pos[3], double rot[3], double scale[3]);

struct PickEdge {
  int key = 0;
  QVector3D a;
  QVector3D b;
  // Outward normals of the two faces at this edge (Edge fillet beta only; zero for primitives).
  QVector3D nA;
  QVector3D nB;
};

std::vector<PickEdge> pickEdges(const Document& doc, const Node& node);

// Edge fillet (beta): the sharp edges of a node's solid as it is WITHOUT its edge fillets, in the
// node's draw frame (what DrawItem::model maps). key = chain index into `chains`.
struct EdgeBetaView {
  std::vector<PickEdge> edges;
  std::vector<std::vector<double>> chains;  // polyline per chain, node frame (a loop repeats its first point)
  std::vector<bool> concave;                // inside edge (filled) per chain
  std::vector<int> picked;                  // chains the node's current edge fillets use
  double radius = 0;                        // radius of the node's current edge fillets (first one)
  bool chamfer = false;                     // ... and whether they are chamfers
  int lost = 0;                             // current fillets that match no edge any more
  int pieces = 0;                           // separate solids in the object (parts that do not touch)
};
EdgeBetaView edgeBetaView(const Document& doc, const Node& node, QString* warning);

// The placed solid of `only` (as evaluateSolid exports it) in double precision: x y z per point,
// three point indices per triangle. False when there is nothing solid.
bool worldMesh64(const Document& doc, const QStringList& only, std::vector<double>* vertices, std::vector<uint64_t>* triangles);

// live: an interactive edit is under way (a drag, the arrows of a size box). A heavy group with a
// selected part inside whose result is not ready yet is then drawn as its loose parts (holes as
// see-through shapes) rather than cut again for every step; *deferred says if any was, so the
// caller builds it properly once the edit pauses.
std::vector<DrawItem> buildEditView(const Document& doc, const QStringList& selected, QString* warning, bool live = false,
                                    bool* deferred = nullptr);

void warmShape(const Node& node, const Document& doc, QString* warning);
void clearFilletLog();
QStringList takeFilletLog();

// A slice of one solid on the stored workplane. One outline per contour.
std::vector<std::vector<double>> sliceSolidForFillet(const Document& doc, const QString& sourceId, const double originPt[3],
                                                      const double normal[3], const double axisPt[3], QString* warning);
// Exact outlines (outer counter-clockwise about the normal, holes clockwise) of the flat faces of one
// solid that lie on the given plane. World space, x y z per point. These come straight off the mesh,
// so they carry the solid's own vertices rather than the face picker's welded approximation.
// With nearRing, only the one face matching that rough outline is returned; without it, every face.
// walls, when given, receives for each returned ring the outward normal of the solid's face across
// each of its edges (x y z per edge), which lets the fillet follow drafted walls.
std::vector<std::vector<double>> planarFaceRings(const Document& doc, const QString& sourceId, const double originPt[3],
                                                 const double normal[3], const double axisPt[3], QString* warning,
                                                 const std::vector<double>* nearRing = nullptr,
                                                 std::vector<std::vector<double>>* walls = nullptr);
// The same, for the face on that plane under `point` (see sf::faceRingsAt).
std::vector<std::vector<double>> planarFaceRingsAt(const Document& doc, const QString& sourceId, const double originPt[3],
                                                   const double normal[3], const double axisPt[3], const double point[3],
                                                   QString* warning, std::vector<std::vector<double>>* walls = nullptr);
QMatrix4x4 nodeWorld(const Document& doc, const QString& id);

bool localBounds(const Document& doc, const Node& node, double lo[3], double hi[3]);
// False for a group whose merged mesh has not been built yet (localBounds would build it, which can
// take long for a big group); true otherwise.
bool groupMeshReady(const Document& doc, const Node& group);
// The same for any part: false also for an imported mesh (an STL, a sculpt, a lattice) not loaded
// yet, which for a big one takes seconds. Cheap shapes always count as ready.
bool meshReady(const Document& doc, const Node& node);

// Gives the generators the text engine for their labels (name tags, drill index, test pieces).
// Called before generators are built; safe to call more than once.
void hookGeneratorText();

// Centre of an SVG in mesh space, before it is moved onto the origin.
// Zero for an old layer file that must stay lined up with its siblings.
bool svgPivotOffset(const QString& path, double* x, double* y);

struct ExportObject {
  QString name;
  CpuMesh mesh;
  // The same mesh in full precision (x y z per point, three point indices per triangle), for
  // formats that keep exact coordinates (STEP). Filled by evaluateObjects.
  std::vector<double> points;
  std::vector<uint32_t> triangles;
  QColor color = QColor("#d7c4a3");
  // Non-empty: this object is one colour of a multi-colour group of that name; formats that can
  // (3MF) keep the colours together as the parts of one model.
  QString assembly;
  QString nodeId;  // the part (or group) on the plate this object was made from
};

CpuMesh evaluateSolid(const Document& doc, const QStringList& only, QString* error);
// One finished part per object: a part on the bed, or a group after its own holes are cut.
// A hole left on the bed is not an object; it only cuts when evaluateSolid merges the plate.
std::vector<ExportObject> evaluateObjects(const Document& doc, const QStringList& only, QString* error);

// A node's own solid (or, for a hole, what it cuts: clearance included) in its own frame, before
// its own placement, exactly as the view builds it, as a mesh in doubles. False if it has none.
// For exports that rebuild the design another way and need a part's mesh where they cannot.
// The text settings of a text node, as its mesh is built from (textshape.h).
TextSpec textSpecFor(const Node& node);

// The radius a shape's own Fillet really gets (see evaluator.cpp), 0 when it rounds nothing.
double edgeFilletUsed(const Node& node);

bool nodeLocalMesh(const Document& doc, const Node& node, std::vector<double>* points, std::vector<uint32_t>* triangles, bool* hole);

// Closed outlines where the finished solid crosses the plane through `origin` with `normal`.
// Each ring is x y pairs, in millimetres, lying flat on that plane. Empty when the plane misses.
// `note` says when the cut was moved 0.05 mm into the solid because it sat exactly on the skin.
std::vector<std::vector<double>> sliceContours(const Document& doc, const QStringList& only, const double origin[3],
                                                const double normal[3], QString* error, QString* note = nullptr);
