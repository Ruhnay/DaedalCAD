#pragma once

// Technical drawings: front, top and side views of a mesh with hidden lines, holes found as true
// circles, dimensions, a title block and a filament estimate, laid out on a paper sheet and written
// as PDF, SVG, DXF or PNG. Sheet coordinates are millimetres with the origin at the top left and
// y pointing down; view coordinates are model millimetres with y pointing up.

#include "evaluator.h"

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector3D>

#include <utility>
#include <vector>

class QPainter;

namespace drawing {

// --- The model, seen from several sides ------------------------------------------------------

using Segment = std::pair<QPointF, QPointF>;

struct ViewCircle {
  QPointF centre;
  double radius = 0;
  bool visible = true;
};

struct View {
  QVector3D dir;    // looking along this
  QVector3D right;  // view x
  QVector3D up;     // view y
  std::vector<Segment> visible;
  std::vector<Segment> hidden;
  std::vector<ViewCircle> circles;  // round edges seen square on: holes, pegs, rims
  QRectF bounds;                    // of the whole model in this view (x right, y up)
  bool valid = false;
};

struct Model {
  View front, top, left, right, iso;
  double volume = 0;  // mm3
  double area = 0;    // mm2
  QVector3D lo, hi;
  bool valid = false;
};

// Works out every view of a finished solid (world coordinates, as evaluateSolid gives it).
Model analyse(const CpuMesh& mesh);

// --- Filament ------------------------------------------------------------------------------------

struct Material {
  QString name;
  double density;  // g/cm3
};
const std::vector<Material>& materials();

struct Filament {
  QString material = QStringLiteral("PLA");
  double density = 1.24;
  double infill = 15;      // %
  double walls = 0.8;      // mm: the shell all round (walls, top and bottom)
  double pricePerKg = 20;  // in `currency`
  QString currency = QStringLiteral("€");
  double diameter = 1.75;  // mm
};

struct FilamentEstimate {
  double grams = 0;
  double metres = 0;
  double cost = 0;
};

// The shell (surface area x walls, at most the whole part) printed solid, the rest at the infill.
FilamentEstimate estimate(double volume, double area, const Filament& filament);

// --- The sheet -----------------------------------------------------------------------------------

enum class Stroke { Visible, Hidden, Thin, Centre, Frame };

struct Line {
  QPointF a, b;
  Stroke stroke = Stroke::Visible;
};

struct Text {
  QPointF at;
  QString text;
  double height = 3.5;  // capital letter height, mm
  double angle = 0;     // degrees, anticlockwise
  int align = 0;        // Qt::Alignment: left/centre/right and top/middle/bottom (bottom = baseline)
  bool bold = false;
};

struct Arrow {
  QPointF tip;
  QPointF dir;  // unit, towards the tip
};

struct Sheet {
  QSizeF size;
  std::vector<Line> lines;
  std::vector<Text> texts;
  std::vector<Arrow> arrows;
};

struct Options {
  QString paper = QStringLiteral("A3");  // A4, A3 or A2, landscape
  bool thirdAngle = false;               // false: first angle (ISO, Europe)
  double scale = 0;                      // 0: the largest standard scale that fits
  bool hidden = true;
  bool dimensions = true;
  bool iso = true;
  bool filamentTable = true;
  QString title;
  QString date;
  Filament filament;
};

QSizeF paperSize(const QString& paper);
QString scaleText(double scale);  // 2 -> "2:1", 0.5 -> "1:2"

// Lays the views out. One sheet when they fit at the scale asked for; otherwise the first sheet
// shows the whole part at a scale that fits, and each view follows on a sheet of its own at the
// scale asked for (in pieces over several sheets when one sheet is too small for it).
// scaleUsed: the scale the views came out at.
std::vector<Sheet> layoutSheets(const Model& model, const Options& options, double* scaleUsed = nullptr);
Sheet layout(const Model& model, const Options& options, double* scaleUsed = nullptr);  // the first sheet

// Draws the sheet; the painter's units must be millimetres.
void paint(QPainter& painter, const Sheet& sheet);
bool writeSvg(const Sheet& sheet, const QString& path, QString* error);
bool writeDxf(const Sheet& sheet, const QString& path, QString* error);
bool writePdf(const Sheet& sheet, const QString& path, QString* error);
bool writePdf(const std::vector<Sheet>& sheets, const QString& path, QString* error);  // one page each
bool writePng(const Sheet& sheet, const QString& path, int dpi, QString* error);

}  // namespace drawing
