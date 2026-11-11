#pragma once

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <vector>

// Flat scene. parentId empty means the node sits on the bed.
// A pattern stores its child as the node whose parentId is the pattern.
struct Node {
  enum class Kind { Shape, Group, Pattern };
  Kind kind = Kind::Shape;
  QString id;
  QString parentId;
  QString name;
  QString shape = QStringLiteral("box");
  bool hole = false;
  // Clearance on a hole (shapes and groups): it cuts this much wider than it is drawn sized, so a part
  // made to the same size fits into it. 0 = none (the default), 1 = the printer clearance from
  // Settings, 2 = holeClearance. Sides only grows it around its own height axis, not up and down.
  int clearanceMode = 0;
  double holeClearance = 0.2;
  bool clearanceSidesOnly = false;
  double size[3] = {30, 30, 30};
  double radius = 12;
  double radius2 = 4;
  int sides = 6;
  int segments = 128;
  // Cylinders: an ISO metric thread ("M8", or "custom" at this cylinder's own diameter); empty for
  // none. A solid cylinder becomes a threaded rod, a hole a threaded hole cut threadClearance
  // wider. threadPitch 0 = the standard coarse pitch.
  QString thread;
  double threadPitch = 0;
  double threadClearance = 0.2;
  // Diamond: 0 = pointed top and bottom (the classic); above 0 a cut gem, its flat top this share of the width.
  double gemTable = 0;
  QString meshPath;
  // Placed 2D shapes: the library SVG this part was made from (meshPath is a frozen copy) and,
  // for a shape placed one part per layer or per level, which one. Lets an edit in the library offer to update it.
  QString meshSource;
  int meshLayer = 0;
  int meshLevel = 0;
  // A part imported from a STEP file (shape "stl", its mesh in meshPath): the file, which of its
  // solids, and where that solid's centre was in the file (the mesh is centred on it). The STEP
  // export uses the file's exact solid for it.
  QString stepSource;
  int stepIndex = -1;
  // Made in the Sculpt window (shape "stl"): double-clicking it opens it there again, as it is.
  bool sculpt = false;
  double stepOffset[3] = {0, 0, 0};
  // Generators (shape "generator"): which one and its settings. See generators.h.
  QString genType;
  QJsonObject genParams;
  bool pivotBaked = false;
  bool faceExtrude = false;
  double extrudeDepth = 10;
  double edgeFillet = 0;
  double surfaceFillet = 0;
  // Empty means every edge. Keys stay stable per shape (a box is 0–11, a prism is sides × 3).
  std::vector<int> filletEdges;
  // Edge fillet (beta), for any solid or group: picked sharp edges as [radius, count, x y z...] per
  // edge chain, in this node's own frame before its scale. See edgefillet.h.
  std::vector<double> edgeBeta;
  int edgeBetaSegs = 12;
  double bend = 0;
  int along = 0;
  // Twist: degrees turned around the twistAxis from one end of the part to the other (0 = off).
  double twist = 0;
  int twistAxis = 0;
  // Taper: how wide the far end of the part is, in percent of the near end, along taperAxis (100 =
  // off). The near end is the low side of the axis; the slices between scale evenly.
  double taper = 100;
  int taperAxis = 2;
  // Hull (groups only): the group is the convex hull of its parts, as if shrink-wrapped, instead of
  // their union. hullMode 0 wraps all parts together; 1 is a chain hull: every part with the next
  // one (in the order of the list), joined, for rounded bars and paths. Holes in it cut the hull.
  bool hull = false;
  int hullMode = 0;
  // Intersection (groups only): the group keeps only what all its solid parts have in common.
  // Holes in it cut that common part.
  bool intersect = false;
  // Merged (groups only): the group is one solid. It is drawn, picked and edited as its finished
  // shape (no parts laid over each other), its parts are hidden in the list, and holes grouped with
  // it go inside it. Unmerge brings the parts back as they were. Exports still use the parts, so a
  // STEP keeps their exact surfaces.
  bool merged = false;
  // Smooth join (groups only, plain joins): mm. Where the parts meet they flow into each other with
  // a soft fillet this wide, like clay, and holes inside cut with the same soft edge. 0 = sharp.
  double blend = 0;
  // Keep part colours (groups only): the group is drawn and exported as one piece per colour of
  // its parts (for multi-colour prints), instead of all in the group's own colour.
  bool multiColor = false;
  // Hollow (shapes and groups): walls this thick (mm, 0 = solid), measured at the part's real
  // size. shellOpen: sides left open, in the part's own axes: 1 -X, 2 +X, 4 -Y, 8 +Y, 16 -Z (bottom), 32 +Z (top).
  double shellWall = 0;
  int shellOpen = 0;
  int toward = 2;
  bool faceBend = false;
  double faceAngle = 90;
  double faceRadius = 30;
  // How faceRadius is measured: from the face's inner edge to the line it turns round (the inside
  // radius of the bend, as on bent sheet metal; new bends), or from the middle of the face (bends
  // saved before 0.9.7's fix, kept as they were).
  bool faceInnerRadius = true;
  int faceSteps = 12;
  double faceRise = 0;
  bool faceSpiral = false;
  double faceRadialStep = 0;
  int faceAxisMode = 0;
  double faceOrigin[3] = {0, 0, 0};
  double faceNormal[3] = {0, 0, 1};
  double faceAxis[3] = {1, 0, 0};
  std::vector<double> faceLoop;
  std::vector<double> facePointNormals;
  // Fillet or chamfer: which solid, which slice outline, and the two settings.
  QString filletOf;
  QString filletOp;
  int filletIndex = 0;
  bool filletOuter = false;
  bool filletUp = false;
  // Text shape. Depth is size[2]; fontSize is the height of a capital letter in mm.
  QString text = QStringLiteral("Text");
  QString fontFamily;
  QString fontStyle;
  double fontSize = 20;
  double letterSpacing = 0;
  int textAlign = 1;
  int textDetail = 2;
  // Text on a curve: the radius of the circle the middle of its letters follows (0 = straight), and
  // whether it runs along the bottom of the circle (still upright, as on the lower half of a coin).
  double textArc = 0;
  bool textArcBelow = false;
  bool mirror[3] = {false, false, false};
  double pos[3] = {0, 0, 0};
  double rot[3] = {0, 0, 0};
  double scale[3] = {1, 1, 1};
  int count = 4;
  double move[3] = {40, 0, 0};
  double drot[3] = {0, 0, 0};
  double resize[3] = {1, 1, 1};
  QColor color = QColor("#d7c4a3");
};

struct Document {
  std::vector<Node> nodes;
  double workplaneZ = 0;
  // A workplane on a face (Workplane tool): new parts stand on it, upright to it. Off: the bed at
  // workplaneZ. Not saved with the project, like TinkerCAD's.
  bool faceWorkplane = false;
  double workplaneOrigin[3] = {0, 0, 0};
  double workplaneNormal[3] = {0, 0, 1};
  double workplaneX[3] = {1, 0, 0};
  double bedX = 256;
  double bedY = 256;
  bool gridOn = true;
  double snap = 1;
  bool snapOn = true;
  int quality = 128;
  int filletSegments = 12;

  Node* find(const QString& id);
  const Node* find(const QString& id) const;
  std::vector<Node*> children(const QString& parentId);
  std::vector<const Node*> children(const QString& parentId) const;
  QString addShape(const QString& shape, double x, double y, double edgeFillet = 0);
  void remove(const QString& id);
  bool groupSelected(const QStringList& ids, QString* error);
  // Groups the parts as a hull (see Node::hull); one part is enough. A chain follows `ids` in order.
  bool hullSelected(const QStringList& ids, bool chain, QString* error);
  bool ungroup(const QString& id, QString* error);
  // Merge: a group becomes merged; several parts are grouped first. Unmerge undoes it.
  bool merge(const QStringList& ids, QString* error);
  bool unmerge(const QString& id, QString* error);
  // Holes put inside a merged group (where they stand now), so it stays one merged part.
  bool addHolesToMerged(const QString& mergedId, const QStringList& holeIds, QString* error);

 private:
  bool makeGroup(const QStringList& ids, QString* error);  // groupSelected without the two-part minimum

 public:
  bool repeat(const QStringList& ids, int count, const double move[3], const double drot[3], const double resize[3],
              QString* error);
  void toggleHole(const QStringList& ids);
  void duplicate(const QString& id);
  void duplicateInPlace(const QString& id);
};

QString newId();

// The gap your printer needs between parts that fit together, in mm (Settings > Printer). One value
// for the whole program; holes and generators can use it or a value of their own.
double printerClearance();
void setPrinterClearance(double mm);
double halfHeight(const Node& node);

// Placements are worked out with Qt's float matrices, which leave a few micrometres of noise
// (14.0000076 instead of 14). Two tubes that should share a top face then sit 15 nm apart, and
// every boolean on that face leaves hairline slivers. Rounding to 0.1 µm / 0.0001° keeps values
// that should be equal exactly equal, far below anything a printer can show.
double tidyLength(double value);
double tidyAngle(double value);
double tidyScale(double value);
void tidyPlacement(Node& node);
