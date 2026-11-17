#pragma once

// The 2D shape editor: draw outlines with nodes and curve handles (or stamp squares, circles and
// polygons), cut holes, keep shapes apart on layers, stack levels on top of each other, and save the
// result as an SVG that the 3D side extrudes. Files written per shape, in <library>/<name>/:
//   <name>.svg       every layer; the 3D side builds one solid per layer and unions them
//   layer-<n>.svg    one layer each, for placing a multi-layer shape as one part per layer
//   level-<n>.svg    one level each (all its layers), for placing a stacked shape as one part per level
//   <name>.synthvg   the editable steps (nodes, handles, layers, levels, origin) the editor reopens

#include <QColor>
#include <QLineF>
#include <QDialog>
#include <QMap>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

struct Shape2DNode {
  QPointF pos;
  QPointF inHandle;
  QPointF outHandle;
};

struct Shape2DContour {
  QVector<Shape2DNode> nodes;
  bool closed = false;
  bool hole = false;
  int layer = 1;  // the layer it is shown and edited on
  int cut = 1;    // the layer it was drawn on: a hole only cuts shapes with the same cut, even after a merge
  int level = 1;  // stacking level: 1 stands on the plate, 2 on top of 1, and so on
  bool through = false;  // a hole that cuts every layer on every level
};

bool readSimpleSvgPolygon(const QString& path, QVector<QPointF>* points);
// The exact outlines (nodes and curve handles) a 2D-editor SVG keeps beside its polygons, as the
// editor reads them (older files too). False when the file has only polygons.
bool readShape2DCurves(const QString& path, QVector<Shape2DContour>* contours);
QColor readSvgShapeColor(const QString& path);
QString synthvgPathFor(const QString& svgPath);
// measures: the measurement lines kept in the editor (a guide only, not part of the shape).
bool readSynthvg(const QString& path, QVector<Shape2DContour>* contours, QColor* color, QString* name,
                 QPointF* origin = nullptr, bool* haveOrigin = nullptr, QMap<int, double>* levelHeights = nullptr,
                 QVector<QLineF>* measures = nullptr);
// Levels that have at least one closed shape, lowest first (just {1} for a shape without levels).
QList<int> shape2DLevels(const QVector<Shape2DContour>& contours);
// Thickness of a level in mm; 1 mm when the file does not say.
double shape2DLevelHeight(const QMap<int, double>& heights, int level);

class QFont;
class QTransform;

// Geometry the editor's tools are built on, in mm with Y up.
namespace shape2d {

// Moves, turns, flips or scales an outline exactly: the curve handles go along.
void transform(Shape2DContour& contour, const QTransform& t);
// The box around the real curves of these outlines.
QRectF bounds(const QVector<Shape2DContour>& contours);

// Several outlines made into one. Merge: everything the shapes cover, minus the holes among them.
// Cut: the first one minus all the others (shapes and holes alike). Overlap: only where all the
// shapes overlap, minus the holes. Holes alone combine as areas and stay holes. The result is
// straight segments (curves are traced finely), with holes marked; outlines that sit inside a hole
// (islands) get a cut number of their own so the hole does not cut them. Cut numbers start at 1.
enum class Combine { Merge, Cut, Overlap };
QVector<Shape2DContour> combine(const QVector<Shape2DContour>& operands, Combine op);

// Rounds a corner: the node becomes two nodes joined by a circular arc of that radius. The radius
// is made smaller when the sides next to it are too short; *used gets the radius used. False when
// the node is not a corner (its neighbours are in a straight line, or it is the end of an open line).
bool roundCorner(Shape2DContour& contour, int node, double radius, double* used = nullptr);
// Rounds every sharp corner (a node without curve handles); returns how many.
int roundAllCorners(Shape2DContour& contour, double radius);

// Marks which outlines of one drawing are holes, the way SVG fills them (evenOdd: holes inside
// holes alternate; otherwise by winding, SVG's default), and gives islands their own cut number
// (1 and up) so a hole only cuts what it surrounds.
void assignHoles(QVector<Shape2DContour>& contours, bool evenOdd);

// Text as outlines with their curves, holes marked, baseline at Y 0.
QVector<Shape2DContour> textOutline(const QString& text, const QFont& font);

// Shapes stamped with two clicks, filling the box from `a` to `b` (kind as in the editor's list:
// 6 star, 7 slot, 8 ring, 9 arc, 10 heart, 11 arrow, 12 swirl, 13 text; the older kinds 1-5 too).
struct StampOptions {
  int sides = 6;            // polygon sides, star points, swirl arms
  double corner = 1.0;      // rounded square corner radius, mm
  double inner = 50.0;      // star point depth, ring and arc width, swirl line width: % of the size
  double angle = 180.0;     // arc sweep, degrees
  double turns = 0.75;      // swirl turns
  QString text;
  QString fontFamily;
  bool bold = false;
};
QVector<Shape2DContour> shapesInBox(int kind, const QPointF& a, const QPointF& b, const StampOptions& options);

// Reads any SVG (paths with curves and arcs, rectangles, circles, ellipses, polygons, lines and
// strokes, with transforms) into outlines in mm, Y up, centred on 0,0. Sizes given in mm, cm, in or
// pt are kept; plain numbers and px count as mm. Each element gets its own cut numbers (1 and up),
// so its holes only cut itself. Empty, with *error set, when there is nothing to use.
QVector<Shape2DContour> readSvg(const QString& path, QString* error = nullptr);

}  // namespace shape2d

class QLabel;
class Shape2DEditorWidget;

class Shape2DEditorDialog : public QDialog {
 public:
  // libraryRoot: the folder new shapes are saved into, one sub-folder per shape.
  // existing: an SVG to open for editing (empty for a new shape).
  Shape2DEditorDialog(QWidget* parent, const QString& libraryRoot, const QString& existing = QString());
  // The SVG last saved or opened (empty when nothing was saved).
  QString savedPathValue() const { return savedPath; }
  // True when Save ran at least once while the dialog was open.
  bool savedThisSession() const { return savedNow; }
  void done(int result) override;

 private:
  bool saveShape();
  // Copies of the selection in a row, a grid or around a circle, shown as they will be added.
  void openRepeat();
  Shape2DEditorWidget* editor = nullptr;
  QLabel* status = nullptr;
  QString savedPath;
  QString libraryRoot;
  QString importedName;  // the file name of an outside SVG that was opened, as the name to save under
  QPointF origin;
  bool haveOrigin = false;
  bool savedNow = false;
};
