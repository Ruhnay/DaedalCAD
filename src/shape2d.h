#pragma once

// The 2D shape editor: draw outlines with nodes and curve handles (or stamp squares, circles and
// polygons), cut holes, keep shapes apart on layers, stack levels on top of each other, and save the
// result as an SVG that the 3D side extrudes. Files written per shape, in <library>/<name>/:
//   <name>.svg       every layer; the 3D side builds one solid per layer and unions them
//   layer-<n>.svg    one layer each, for placing a multi-layer shape as one part per layer
//   level-<n>.svg    one level each (all its layers), for placing a stacked shape as one part per level
//   <name>.synthvg   the editable steps (nodes, handles, layers, levels, origin) the editor reopens

#include <QColor>
#include <QDialog>
#include <QMap>
#include <QPointF>
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
bool readSynthvg(const QString& path, QVector<Shape2DContour>* contours, QColor* color, QString* name,
                 QPointF* origin = nullptr, bool* haveOrigin = nullptr, QMap<int, double>* levelHeights = nullptr);
// Levels that have at least one closed shape, lowest first (just {1} for a shape without levels).
QList<int> shape2DLevels(const QVector<Shape2DContour>& contours);
// Thickness of a level in mm; 1 mm when the file does not say.
double shape2DLevelHeight(const QMap<int, double>& heights, int level);

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
  Shape2DEditorWidget* editor = nullptr;
  QLabel* status = nullptr;
  QString savedPath;
  QString libraryRoot;
  QPointF origin;
  bool haveOrigin = false;
  bool savedNow = false;
};
