#include "shape2d.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QSaveFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizeF>
#include <QSpinBox>
#include <QTextStream>
#include <QTransform>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <QXmlStreamReader>

#include <manifold/cross_section.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

bool readSimpleSvgPolygon(const QString& path, QVector<QPointF>* points) {
  if (!points) return false;
  points->clear();
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
  const QString text = QString::fromUtf8(file.readAll());
  QRegularExpression polygonRe(QStringLiteral(R"(<polygon\b[^>]*\bpoints\s*=\s*[\"']([^\"']+)[\"'][^>]*>)"),
                               QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch match = polygonRe.match(text);
  if (!match.hasMatch()) {
    const QRegularExpression rectRe(QStringLiteral("<rect\\b"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression widthRe(QStringLiteral("\\bwidth\\s*="), QRegularExpression::CaseInsensitiveOption);
    if (rectRe.match(text).hasMatch() && widthRe.match(text).hasMatch()) {
      points->push_back(QPointF(0, 0));
      points->push_back(QPointF(1, 0));
      points->push_back(QPointF(0, 1));
      return true;
    }
    return false;
  }
  const QString pointText = match.captured(1);
  const QRegularExpression numberRe(QStringLiteral(R"([-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?)"));
  QRegularExpressionMatchIterator it = numberRe.globalMatch(pointText);
  QVector<double> values;
  while (it.hasNext()) values.push_back(it.next().captured(0).toDouble());
  if (values.size() < 6 || values.size() % 2 != 0) return false;
  for (int i = 0; i < values.size(); i += 2) points->push_back(QPointF(values[i], -values[i + 1]));
  if (points->size() > 1 && (*points)[0] == points->back()) points->removeLast();
  return points->size() >= 3;
}

namespace {

bool zeroHandle(const QPointF& p) {
  return std::abs(p.x()) < 1e-6 && std::abs(p.y()) < 1e-6;
}

QPointF safeNormalized(const QPointF& p) {
  const double len = std::hypot(p.x(), p.y());
  return len > 1e-8 ? QPointF(p.x() / len, p.y() / len) : QPointF(1, 0);
}

QPointF defaultTangent(const QVector<Shape2DNode>& nodes, int index, bool out, bool closed = false) {
  if (nodes.isEmpty() || index < 0 || index >= nodes.size()) return {};
  const int n = nodes.size();
  const QPointF p = nodes[index].pos;
  const bool havePrev = closed ? n >= 2 : index > 0;
  const bool haveNext = closed ? n >= 2 : index + 1 < n;
  if (!havePrev && !haveNext) return {};
  auto at = [&](int i) {
    const int wrapped = (i % n + n) % n;
    return nodes[wrapped].pos;
  };
  QPointF direction;
  double length = 12.0;
  if (havePrev && haveNext) {
    const QPointF prev = at(index - 1);
    const QPointF next = at(index + 1);
    direction = safeNormalized(next - prev);
    length = std::max(2.0, std::min(std::hypot(p.x() - prev.x(), p.y() - prev.y()),
                                    std::hypot(next.x() - p.x(), next.y() - p.y())) * 0.28);
  } else if (haveNext) {
    const QPointF next = at(index + 1);
    direction = safeNormalized(next - p);
    length = std::max(2.0, std::hypot(next.x() - p.x(), next.y() - p.y()) * 0.28);
  } else {
    const QPointF prev = at(index - 1);
    direction = safeNormalized(p - prev);
    length = std::max(2.0, std::hypot(p.x() - prev.x(), p.y() - prev.y()) * 0.28);
  }
  if (!out) direction *= -1.0;
  return direction * length;
}

QVector<QPointF> sampleShape2D(const QVector<Shape2DNode>& nodes, bool closed, int segmentsPerCurve = 20) {
  QVector<QPointF> samples;
  if (nodes.size() < 2) return samples;
  const int segmentCount = closed ? nodes.size() : nodes.size() - 1;
  for (int i = 0; i < segmentCount; ++i) {
    const int nextIndex = (i + 1) % nodes.size();
    const QPointF p0 = nodes[i].pos;
    const QPointF p1 = nodes[nextIndex].pos;
    const QPointF c1 = p0 + nodes[i].outHandle;
    const QPointF c2 = p1 + nodes[nextIndex].inHandle;
    const bool curved = !zeroHandle(nodes[i].outHandle) || !zeroHandle(nodes[nextIndex].inHandle);
    const int steps = curved ? std::max(8, segmentsPerCurve) : 1;
    if (i == 0) samples.push_back(p0);
    if (!curved) {
      samples.push_back(p1);
      continue;
    }
    for (int s = 1; s <= steps; ++s) {
      const double t = static_cast<double>(s) / static_cast<double>(steps);
      const double u = 1.0 - t;
      const double b0 = u * u * u;
      const double b1 = 3.0 * u * u * t;
      const double b2 = 3.0 * u * t * t;
      const double b3 = t * t * t;
      samples.push_back(QPointF(b0 * p0.x() + b1 * c1.x() + b2 * c2.x() + b3 * p1.x(),
                                b0 * p0.y() + b1 * c1.y() + b2 * c2.y() + b3 * p1.y()));
    }
  }
  return samples;
}

QVector<Shape2DNode> shapeBetween(int kind, const QPointF& a, const QPointF& b, int sides, double corner) {
  const double x0 = std::min(a.x(), b.x());
  const double x1 = std::max(a.x(), b.x());
  const double y0 = std::min(a.y(), b.y());
  const double y1 = std::max(a.y(), b.y());
  const double w = x1 - x0;
  const double h = y1 - y0;
  if (w < 1e-6 && h < 1e-6) return {};
  auto node = [](double x, double y, double ix, double iy, double ox, double oy) {
    return Shape2DNode{QPointF(x, y), QPointF(ix, iy), QPointF(ox, oy)};
  };
  QVector<Shape2DNode> out;
  if (kind == 3) {
    out.push_back(node(x0, y0, 0, 0, 0, 0));
    out.push_back(node(x1, y0, 0, 0, 0, 0));
    out.push_back(node((x0 + x1) * 0.5, y1, 0, 0, 0, 0));
    return out;
  }
  if (kind == 4 || kind == 5) {
    const int count = kind == 4 ? 4 : std::clamp(sides, 3, 64);
    const double cx = (x0 + x1) * 0.5;
    const double cy = (y0 + y1) * 0.5;
    const double rx = std::max(w * 0.5, 1e-4);
    const double ry = std::max(h * 0.5, 1e-4);
    if (kind == 4) {
      const double kx = rx * 0.5522847498;
      const double ky = ry * 0.5522847498;
      out.push_back(node(cx + rx, cy, 0, -ky, 0, ky));
      out.push_back(node(cx, cy + ry, kx, 0, -kx, 0));
      out.push_back(node(cx - rx, cy, 0, ky, 0, -ky));
      out.push_back(node(cx, cy - ry, -kx, 0, kx, 0));
      return out;
    }
    for (int i = 0; i < count; ++i) {
      const double ang = -3.14159265358979323846 * 0.5 + 2.0 * 3.14159265358979323846 * i / count;
      out.push_back(node(cx + rx * std::cos(ang), cy + ry * std::sin(ang), 0, 0, 0, 0));
    }
    return out;
  }
  const double r = kind == 2 ? std::min(std::max(corner, 0.0), std::min(w, h) * 0.5) : 0.0;
  if (r < 1e-4) {
    out.push_back(node(x0, y0, 0, 0, 0, 0));
    out.push_back(node(x1, y0, 0, 0, 0, 0));
    out.push_back(node(x1, y1, 0, 0, 0, 0));
    out.push_back(node(x0, y1, 0, 0, 0, 0));
    return out;
  }
  const double k = 0.5522847498 * r;
  out.push_back(node(x0 + r, y0, -k, 0, 0, 0));
  out.push_back(node(x1 - r, y0, 0, 0, k, 0));
  out.push_back(node(x1, y0 + r, 0, -k, 0, 0));
  out.push_back(node(x1, y1 - r, 0, 0, 0, k));
  out.push_back(node(x1 - r, y1, k, 0, 0, 0));
  out.push_back(node(x0 + r, y1, 0, 0, -k, 0));
  out.push_back(node(x0, y1 - r, 0, k, 0, 0));
  out.push_back(node(x0, y0 + r, 0, 0, 0, -k));
  return out;
}

bool readSimpleSvgEditorNodes(const QString& path, QVector<Shape2DNode>* nodes, bool* closed) {
  if (!nodes) return false;
  nodes->clear();
  if (closed) *closed = true;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
  const QString text = QString::fromUtf8(file.readAll());
  const QRegularExpression dataRe(QStringLiteral(R"(data-(?:daedalcad|shapesynth|formwright)-nodes\s*=\s*[\"']([^\"']+)[\"'])"),
                                   QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch match = dataRe.match(text);
  if (!match.hasMatch()) return false;
  for (const QString& record : match.captured(1).split(';', Qt::SkipEmptyParts)) {
    const QStringList values = record.split(',', Qt::KeepEmptyParts);
    if (values.size() != 6) return false;
    bool ok = true;
    Shape2DNode node;
    node.pos = QPointF(values[0].toDouble(&ok), values[1].toDouble(&ok));
    node.inHandle = QPointF(values[2].toDouble(&ok), values[3].toDouble(&ok));
    node.outHandle = QPointF(values[4].toDouble(&ok), values[5].toDouble(&ok));
    if (!ok) return false;
    nodes->push_back(node);
  }
  return nodes->size() >= 3;
}

bool parseContourRecord(const QString& record, Shape2DContour* contour) {
  if (!contour) return false;
  contour->nodes.clear();
  contour->closed = true;
  contour->hole = false;
  contour->layer = 1;
  contour->cut = 1;
  QString body = record.trimmed();
  if (body.startsWith(QLatin1String("h")) || body.startsWith(QLatin1String("s"))) {
    contour->hole = body.startsWith(QLatin1String("h"));
    body = body.mid(1);
    const int colon = body.indexOf(QLatin1Char(':'));
    if (colon < 0) return false;
    if (colon > 0) {
      const QString head = body.left(colon);
      const int cutMark = head.indexOf(QLatin1Char('c'));
      bool layerOk = false;
      if (cutMark > 0) {
        contour->layer = std::max(1, head.left(cutMark).toInt(&layerOk));
        bool cutOk = false;
        contour->cut = std::max(1, head.mid(cutMark + 1).toInt(&cutOk));
        if (!cutOk) contour->cut = contour->layer;
      } else {
        contour->layer = std::max(1, head.toInt(&layerOk));
        contour->cut = contour->layer;
      }
      if (!layerOk) {
        contour->layer = 1;
        contour->cut = 1;
      }
    }
    body = body.mid(colon + 1);
  }
  if (body.isEmpty()) return false;
  for (const QString& item : body.split(';', Qt::SkipEmptyParts)) {
    const QStringList values = item.split(',', Qt::KeepEmptyParts);
    if (values.size() != 6) return false;
    bool ok = true;
    Shape2DNode node;
    node.pos = QPointF(values[0].toDouble(&ok), values[1].toDouble(&ok));
    if (!ok) return false;
    node.inHandle = QPointF(values[2].toDouble(&ok), values[3].toDouble(&ok));
    if (!ok) return false;
    node.outHandle = QPointF(values[4].toDouble(&ok), values[5].toDouble(&ok));
    if (!ok) return false;
    contour->nodes.push_back(node);
  }
  return contour->nodes.size() >= 3;
}

bool readShapeContours(const QString& path, QVector<Shape2DContour>* contours) {
  if (!contours) return false;
  contours->clear();
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
  const QString text = QString::fromUtf8(file.readAll());
  const QRegularExpression contoursRe(QStringLiteral(R"(data-(?:daedalcad|shapesynth|formwright)-contours\s*=\s*[\"']([^\"']+)[\"'])"),
                                       QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch contoursMatch = contoursRe.match(text);
  if (contoursMatch.hasMatch()) {
    for (const QString& record : contoursMatch.captured(1).split('|', Qt::SkipEmptyParts)) {
      Shape2DContour contour;
      const QString trimmed = record.trimmed();
      const bool prefixed = trimmed.startsWith(QLatin1String("h")) || trimmed.startsWith(QLatin1String("s"));
      if (!parseContourRecord(record, &contour)) return false;
      if (!prefixed) contour.hole = !contours->isEmpty();
      contours->push_back(contour);
    }
    return !contours->isEmpty();
  }
  QVector<Shape2DNode> nodes;
  bool closed = true;
  if (readSimpleSvgEditorNodes(path, &nodes, &closed)) {
    Shape2DContour contour;
    contour.nodes = nodes;
    contour.closed = closed && nodes.size() >= 3;
    contours->push_back(contour);
    return true;
  }
  const QRegularExpression polygonRe(QStringLiteral(R"(<polygon\b[^>]*\bpoints\s*=\s*[\"']([^\"']+)[\"'][^>]*>)"),
                                      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpression numberRe(QStringLiteral(R"([-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?)"));
  QRegularExpressionMatchIterator matches = polygonRe.globalMatch(text);
  while (matches.hasNext()) {
    const QRegularExpressionMatch match = matches.next();
    QRegularExpressionMatchIterator it = numberRe.globalMatch(match.captured(1));
    QVector<double> values;
    while (it.hasNext()) values.push_back(it.next().captured(0).toDouble());
    if (values.size() < 6 || values.size() % 2 != 0) continue;
    Shape2DContour contour;
    contour.closed = true;
    contour.hole = match.captured(0).contains(QStringLiteral("data-hole"));
    for (int i = 0; i < values.size(); i += 2) contour.nodes.push_back(Shape2DNode{QPointF(values[i], -values[i + 1]), {}, {}});
    if (contour.nodes.size() > 1 && contour.nodes.front().pos == contour.nodes.back().pos) contour.nodes.removeLast();
    if (contour.nodes.size() >= 3) contours->push_back(contour);
  }
  return !contours->isEmpty();
}

}  // namespace

bool readShape2DCurves(const QString& path, QVector<Shape2DContour>* contours) {
  if (!contours) return false;
  contours->clear();
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
  const QString text = QString::fromUtf8(file.readAll());
  // Only the editor's own records (nodes and handles) are curves; plain polygons are not.
  static const QRegularExpression records(QStringLiteral(R"(data-(?:daedalcad|shapesynth|formwright)-(?:contours|nodes)\s*=)"),
                                          QRegularExpression::CaseInsensitiveOption);
  if (!records.match(text).hasMatch()) return false;
  return readShapeContours(path, contours);
}

QColor readSvgShapeColor(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QColor("#e87400");
  const QString text = QString::fromUtf8(file.readAll());
  const QRegularExpression named(QStringLiteral(R"(data-(?:daedalcad|shapesynth|formwright)-color\s*=\s*[\"']([^\"']+)[\"'])"),
                                  QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch namedMatch = named.match(text);
  if (namedMatch.hasMatch()) {
    const QColor color(namedMatch.captured(1));
    if (color.isValid()) return color;
  }
  const QRegularExpression stroke(QStringLiteral(R"(<path\b[^>]*\bstroke\s*=\s*[\"']([^\"']+)[\"'])"),
                                   QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch strokeMatch = stroke.match(text);
  if (strokeMatch.hasMatch()) {
    const QString value = strokeMatch.captured(1);
    if (value.compare(QLatin1String("none"), Qt::CaseInsensitive) != 0) {
      const QColor color(value);
      if (color.isValid()) return color;
    }
  }
  return QColor("#e87400");
}

QString synthvgPathFor(const QString& svgPath) {
  const QFileInfo info(svgPath);
  return info.absolutePath() + QDir::separator() + info.completeBaseName() + QStringLiteral(".synthvg");
}

namespace {

bool writeSynthvg(const QString& path, const QString& name, const QColor& color, const QVector<Shape2DContour>& contours,
                  const QPointF& origin, const QMap<int, double>& levelHeights, const QVector<QLineF>& measures) {
  QJsonObject root;
  root["name"] = name;
  root["color"] = color.name(QColor::HexRgb);
  // The drawing point that sits on the part's origin in 3D. Kept across saves so placed parts do not shift.
  root["center"] = QJsonArray{origin.x(), origin.y()};
  QJsonArray items;
  for (const Shape2DContour& contour : contours) {
    if (!contour.closed || contour.nodes.size() < 3) continue;
    QJsonObject item;
    item["layer"] = std::max(1, contour.layer);
    item["cut"] = std::max(1, contour.cut > 0 ? contour.cut : contour.layer);
    item["hole"] = contour.hole;
    if (contour.level > 1) item["level"] = contour.level;
    if (contour.hole && contour.through) item["through"] = true;
    QJsonArray nodes;
    for (const Shape2DNode& node : contour.nodes) {
      nodes.append(QJsonArray{node.pos.x(), node.pos.y(), node.inHandle.x(), node.inHandle.y(), node.outHandle.x(),
                              node.outHandle.y()});
    }
    item["nodes"] = nodes;
    items.append(item);
  }
  if (items.isEmpty()) return false;
  root["contours"] = items;
  QJsonArray levels;
  for (auto it = levelHeights.cbegin(); it != levelHeights.cend(); ++it)
    levels.append(QJsonObject{{"level", it.key()}, {"height", it.value()}});
  if (!levels.isEmpty()) root["levels"] = levels;
  // Measurements are a guide in the editor only; the 3D side never reads them.
  QJsonArray lines;
  for (const QLineF& line : measures) lines.append(QJsonArray{line.x1(), line.y1(), line.x2(), line.y2()});
  if (!lines.isEmpty()) root["measurements"] = lines;
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
  file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  return file.commit();
}

}  // namespace

bool readSynthvg(const QString& path, QVector<Shape2DContour>* contours, QColor* color, QString* name, QPointF* origin,
                 bool* haveOrigin, QMap<int, double>* levelHeights, QVector<QLineF>* measures) {
  if (!contours) return false;
  contours->clear();
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
  const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
  if (name) *name = root["name"].toString();
  const QJsonArray center = root["center"].toArray();
  if (haveOrigin) *haveOrigin = center.size() == 2;
  if (origin && center.size() == 2) *origin = QPointF(center[0].toDouble(), center[1].toDouble());
  if (color) {
    const QColor ink(root["color"].toString());
    if (ink.isValid()) *color = ink;
  }
  for (const QJsonValue& value : root["contours"].toArray()) {
    const QJsonObject item = value.toObject();
    Shape2DContour contour;
    contour.closed = true;
    contour.hole = item["hole"].toBool();
    contour.layer = std::max(1, item["layer"].toInt(1));
    contour.cut = std::max(1, item["cut"].toInt(contour.layer));
    contour.level = std::max(1, item["level"].toInt(1));
    contour.through = contour.hole && item["through"].toBool();
    for (const QJsonValue& nodeValue : item["nodes"].toArray()) {
      const QJsonArray n = nodeValue.toArray();
      if (n.size() < 6) continue;
      contour.nodes.push_back(Shape2DNode{QPointF(n[0].toDouble(), n[1].toDouble()),
                                           QPointF(n[2].toDouble(), n[3].toDouble()),
                                           QPointF(n[4].toDouble(), n[5].toDouble())});
    }
    if (contour.nodes.size() >= 3) contours->push_back(contour);
  }
  if (levelHeights) {
    levelHeights->clear();
    for (const QJsonValue& value : root["levels"].toArray()) {
      const QJsonObject item = value.toObject();
      const int level = item["level"].toInt(0);
      const double height = item["height"].toDouble(0);
      if (level >= 1 && height > 0) levelHeights->insert(level, height);
    }
  }
  if (measures) {
    measures->clear();
    for (const QJsonValue& value : root["measurements"].toArray()) {
      const QJsonArray v = value.toArray();
      if (v.size() == 4) measures->push_back(QLineF(v[0].toDouble(), v[1].toDouble(), v[2].toDouble(), v[3].toDouble()));
    }
  }
  return !contours->isEmpty();
}

QList<int> shape2DLevels(const QVector<Shape2DContour>& contours) {
  QList<int> levels;
  for (const Shape2DContour& contour : contours)
    if (!contour.hole && contour.closed && contour.nodes.size() >= 3 && !levels.contains(std::max(1, contour.level)))
      levels.push_back(std::max(1, contour.level));
  std::sort(levels.begin(), levels.end());
  if (levels.isEmpty()) levels.push_back(1);
  return levels;
}

double shape2DLevelHeight(const QMap<int, double>& heights, int level) {
  const double height = heights.value(level, 1.0);
  return height > 0 ? height : 1.0;
}

namespace {

// What one level is made of in 3D: its own contours, plus every "through" hole of every level,
// copied into each cut group of this level so it cuts all of them.
QVector<Shape2DContour> contoursForLevel(const QVector<Shape2DContour>& all, int level) {
  QVector<Shape2DContour> out;
  QList<int> cuts;
  for (const Shape2DContour& contour : all) {
    if (std::max(1, contour.level) != level || !contour.closed || contour.nodes.size() < 3) continue;
    out.push_back(contour);
    const int cut = contour.cut > 0 ? contour.cut : contour.layer;
    if (!contour.hole && !cuts.contains(cut)) cuts.push_back(cut);
  }
  for (const Shape2DContour& hole : all) {
    if (!hole.hole || !hole.through || !hole.closed || hole.nodes.size() < 3) continue;
    const int ownCut = hole.cut > 0 ? hole.cut : hole.layer;
    for (int cut : cuts) {
      if (std::max(1, hole.level) == level && cut == ownCut) continue;  // already in there
      Shape2DContour copy = hole;
      copy.level = level;
      copy.layer = copy.cut = cut;
      out.push_back(copy);
    }
  }
  return out;
}

// Everything flattened into one drawing, each level's cut groups kept apart from the others'.
QVector<Shape2DContour> contoursAllLevels(const QVector<Shape2DContour>& all) {
  QVector<Shape2DContour> out;
  QList<int> levels;
  for (const Shape2DContour& contour : all)
    if (!levels.contains(std::max(1, contour.level))) levels.push_back(std::max(1, contour.level));
  std::sort(levels.begin(), levels.end());
  const bool stacked = levels.size() > 1;
  for (int level : levels) {
    for (Shape2DContour contour : contoursForLevel(all, level)) {
      if (stacked) contour.cut = (level - 1) * 1000 + (contour.cut > 0 ? contour.cut : contour.layer);
      out.push_back(contour);
    }
  }
  return out;
}

QString shape2DPathData(const QVector<Shape2DNode>& nodes, bool closed) {
  QString d;
  QTextStream out(&d);
  out.setRealNumberNotation(QTextStream::FixedNotation);
  out.setRealNumberPrecision(4);
  if (nodes.size() < 2) return d;
  out << "M " << nodes[0].pos.x() << ' ' << -nodes[0].pos.y();
  const int segmentCount = closed ? nodes.size() : nodes.size() - 1;
  for (int i = 0; i < segmentCount; ++i) {
    const int j = (i + 1) % nodes.size();
    const QPointF p = nodes[j].pos;
    const QPointF c1 = nodes[i].pos + nodes[i].outHandle;
    const QPointF c2 = p + nodes[j].inHandle;
    if (zeroHandle(nodes[i].outHandle) && zeroHandle(nodes[j].inHandle)) {
      out << " L " << p.x() << ' ' << -p.y();
    } else {
      out << " C " << c1.x() << ' ' << -c1.y() << ' ' << c2.x() << ' ' << -c2.y() << ' ' << p.x() << ' ' << -p.y();
    }
  }
  if (closed) out << " Z";
  return d;
}

QString contourRecord(const Shape2DContour& contour) {
  QString record;
  QTextStream out(&record);
  out.setRealNumberNotation(QTextStream::FixedNotation);
  out.setRealNumberPrecision(4);
  out << (contour.hole ? "h" : "s") << contour.layer << 'c' << std::max(1, contour.cut > 0 ? contour.cut : contour.layer) << ':';
  for (int i = 0; i < contour.nodes.size(); ++i) {
    if (i) out << ';';
    const Shape2DNode& node = contour.nodes[i];
    out << node.pos.x() << ',' << node.pos.y() << ',' << node.inHandle.x() << ',' << node.inHandle.y() << ','
        << node.outHandle.x() << ',' << node.outHandle.y();
  }
  return record;
}

bool writeSimpleSvgPath(const QString& path, const QVector<Shape2DContour>& contours, const QColor& color,
                        const QPointF* sharedCenter = nullptr, QPointF* writtenCenter = nullptr) {
  QVector<Shape2DContour> used;
  for (const Shape2DContour& contour : contours) {
    if (contour.closed && contour.nodes.size() >= 3) used.push_back(contour);
  }
  if (used.isEmpty()) return false;
  QVector<QVector<QPointF>> sampled;
  double minX = 1e300;
  double maxX = -1e300;
  double minY = 1e300;
  double maxY = -1e300;
  for (const Shape2DContour& contour : used) {
    const QVector<QPointF> points = sampleShape2D(contour.nodes, true, 24);
    if (points.size() < 3) return false;
    for (const QPointF& p : points) {
      minX = std::min(minX, p.x());
      maxX = std::max(maxX, p.x());
      minY = std::min(minY, p.y());
      maxY = std::max(maxY, p.y());
    }
    sampled.push_back(points);
  }
  const double pad = 2.0;
  const QPointF center = sharedCenter ? *sharedCenter : QPointF((minX + maxX) * 0.5, (minY + maxY) * 0.5);
  if (writtenCenter) *writtenCenter = center;
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
  QTextStream out(&file);
  out.setRealNumberNotation(QTextStream::FixedNotation);
  out.setRealNumberPrecision(4);
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out << "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" data-center=\""
      << center.x() << ',' << center.y() << "\" viewBox=\""
      << minX - pad << ' ' << -maxY - pad << ' ' << std::max(maxX - minX + 2 * pad, 1.0) << ' '
      << std::max(maxY - minY + 2 * pad, 1.0) << "\">\n";
  QString pathData;
  QString records;
  for (int i = 0; i < used.size(); ++i) {
    pathData += shape2DPathData(used[i].nodes, true);
    if (i) records += QLatin1Char('|');
    records += contourRecord(used[i]);
  }
  const QColor ink = color.isValid() ? color : QColor("#e87400");
  out << "  <path d=\"" << pathData << "\" data-daedalcad-contours=\"" << records
      << "\" data-daedalcad-color=\"" << ink.name(QColor::HexRgb)
      << "\" fill=\"" << ink.name(QColor::HexRgb)
      << "\" fill-rule=\"evenodd\" stroke=\"" << ink.name(QColor::HexRgb) << "\" stroke-width=\"1\"/>\n";
  for (int i = 0; i < sampled.size(); ++i) {
    out << "  <polygon points=\"";
    for (int p = 0; p < sampled[i].size(); ++p) {
      if (p) out << ' ';
      out << sampled[i][p].x() << ',' << -sampled[i][p].y();
    }
    out << "\"";
    if (used[i].hole) out << " data-hole=\"1\"";
    out << " data-layer=\"" << std::max(1, used[i].cut > 0 ? used[i].cut : used[i].layer) << "\"";
    out << " fill=\"none\" stroke=\"none\"/>\n";
  }
  out << "</svg>\n";
  out.flush();
  return out.status() == QTextStream::Ok && file.commit();
}


// ---------------------------------------------------------------------------------------------------
// Geometry helpers for the editor.

constexpr double kPi2D = 3.14159265358979323846;

QPainterPath contourWorldPath(const Shape2DContour& contour) {
  QPainterPath path;
  if (contour.nodes.size() < 2) return path;
  path.moveTo(contour.nodes[0].pos);
  const int segmentCount = contour.closed ? contour.nodes.size() : contour.nodes.size() - 1;
  for (int i = 0; i < segmentCount; ++i) {
    const int j = (i + 1) % contour.nodes.size();
    const QPointF end = contour.nodes[j].pos;
    if (zeroHandle(contour.nodes[i].outHandle) && zeroHandle(contour.nodes[j].inHandle)) {
      path.lineTo(end);
    } else {
      path.cubicTo(contour.nodes[i].pos + contour.nodes[i].outHandle, end + contour.nodes[j].inHandle, end);
    }
  }
  if (contour.closed) path.closeSubpath();
  return path;
}

// Outline box of the real curve (not just the nodes), in mm.
// The outline as fine straight segments, for filling. Qt's path unions and cut-outs flatten curves
// very coarsely, so a ring would show as an octagon; the 3D side samples the same way.
QPainterPath contourFillPath(const Shape2DContour& contour) {
  QPainterPath path;
  const QVector<QPointF> points = sampleShape2D(contour.nodes, contour.closed, 24);
  if (points.size() < 3) return contourWorldPath(contour);
  path.addPolygon(QPolygonF(points));
  path.closeSubpath();
  return path;
}

QRectF contourBounds(const Shape2DContour& contour) {
  const QVector<QPointF> points = sampleShape2D(contour.nodes, contour.closed, 48);
  if (points.isEmpty()) return {};
  double x0 = points[0].x(), x1 = x0, y0 = points[0].y(), y1 = y0;
  for (const QPointF& p : points) {
    x0 = std::min(x0, p.x());
    x1 = std::max(x1, p.x());
    y0 = std::min(y0, p.y());
    y1 = std::max(y1, p.y());
  }
  return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

double contourArea(const Shape2DContour& contour) {
  const QVector<QPointF> points = sampleShape2D(contour.nodes, true, 16);
  double area = 0;
  for (int i = 0; i < points.size(); ++i) {
    const QPointF& a = points[i];
    const QPointF& b = points[(i + 1) % points.size()];
    area += a.x() * b.y() - b.x() * a.y();
  }
  return std::abs(area) * 0.5;
}

QPointF rotated(const QPointF& p, double angle) {
  const double c = std::cos(angle);
  const double s = std::sin(angle);
  return QPointF(p.x() * c - p.y() * s, p.x() * s + p.y() * c);
}

// The ways a stamped primitive can be drawn from two clicks.
enum DrawFrom { FromCorners = 0, FromCentre = 1, FromDiameter = 2 };

// Shapes that are drawn round from their centre (a radius), not as a box.
bool roundKind(int kind) { return kind == 4 || kind == 5 || kind == 6 || kind == 8 || kind == 9 || kind == 12; }

// The shape `kind` filling [-hw, hw] x [-hh, hh], turned by `angle` and moved to `centre`.
QVector<Shape2DContour> placedShapes(int kind, const QPointF& centre, double hw, double hh, double angle,
                                     const shape2d::StampOptions& options) {
  QVector<Shape2DContour> out = shape2d::shapesInBox(kind, QPointF(-hw, -hh), QPointF(hw, hh), options);
  const QTransform place(std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), centre.x(), centre.y());
  for (Shape2DContour& contour : out) shape2d::transform(contour, place);
  return out;
}

// Which way a round shape turns so its first point (a polygon's corner, a star's point, the middle
// of an arc) faces the click that sized it.
double facing(int kind, const QPointF& towards) {
  const double a = std::atan2(towards.y(), towards.x());
  if (kind == 5) return a + kPi2D * 0.5;  // a polygon's first corner points down
  if (kind == 6 || kind == 9) return a - kPi2D * 0.5;  // a star's first point and an arc's middle point up
  if (kind == 12) return a;
  return 0;
}

// lock (Shift): corners/centre give equal width and height; diameter stays exactly horizontal or vertical.
QVector<Shape2DContour> stampedShapes(int kind, int from, const QPointF& a, QPointF b, bool lock,
                                      const shape2d::StampOptions& options) {
  const bool round = roundKind(kind);
  if (from == FromDiameter) {
    if (lock) {
      if (std::abs(b.x() - a.x()) >= std::abs(b.y() - a.y())) b.setY(a.y());
      else b.setX(a.x());
    }
    const QPointF d = b - a;
    const double length = std::hypot(d.x(), d.y());
    if (length < 1e-6) return {};
    const QPointF centre = (a + b) * 0.5;
    double angle = std::atan2(d.y(), d.x());
    if (kind == 4 || kind == 8) angle = 0;  // a circle looks the same either way; keep its nodes on the axes
    if (round && kind != 4 && kind != 8) angle = facing(kind, a - centre);
    return placedShapes(kind, centre, length * 0.5, length * 0.5, angle, options);
  }
  if (from == FromCentre) {
    QPointF d = b - a;
    if (round) {
      const double r = std::hypot(d.x(), d.y());
      if (r < 1e-6) return {};
      return placedShapes(kind, a, r, r, facing(kind, d), options);
    }
    double hw = std::abs(d.x());
    double hh = std::abs(d.y());
    if (lock) hw = hh = std::max(hw, hh);
    if (hw < 1e-6 && hh < 1e-6) return {};
    return placedShapes(kind, a, std::max(hw, 1e-4), std::max(hh, 1e-4), 0, options);
  }
  if (lock) {
    const double size = std::max(std::abs(b.x() - a.x()), std::abs(b.y() - a.y()));
    b = QPointF(a.x() + (b.x() >= a.x() ? size : -size), a.y() + (b.y() >= a.y() ? size : -size));
  }
  return shape2d::shapesInBox(kind, a, b, options);
}

// The mirror of an outline across the X axis line (left-right: x -> -x) or the Y axis line.
Shape2DContour mirrored(Shape2DContour contour, int mode) {
  shape2d::transform(contour, mode == 1 ? QTransform::fromScale(-1, 1) : QTransform::fromScale(1, -1));
  return contour;
}

QString mm(double value) { return QString::number(value, 'f', 3); }

bool readSvgCenter(const QString& path, QPointF* center) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
  const QRegularExpression centerRe(QStringLiteral(R"(data-center\s*=\s*[\"']([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)[\"'])"),
                                    QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch match = centerRe.match(QString::fromUtf8(file.readAll()));
  if (!match.hasMatch()) return false;
  *center = QPointF(match.captured(1).toDouble(), match.captured(2).toDouble());
  return true;
}

// ---------------------------------------------------------------------------------------------------
// Tools: the geometry behind combining, rounding corners, the stamped shapes and SVG import.

using manifold::CrossSection;

// Curves are traced this finely when an outline has to become straight segments.
constexpr int kTraceSteps = 32;

QVector<QPointF> closedSamples(const Shape2DContour& contour, int steps) {
  QVector<QPointF> points = sampleShape2D(contour.nodes, true, steps);
  if (points.size() > 1 && std::hypot(points.front().x() - points.back().x(), points.front().y() - points.back().y()) < 1e-9)
    points.removeLast();
  return points;
}

double signedArea(const QVector<QPointF>& points) {
  double area = 0;
  for (int i = 0; i < points.size(); ++i) {
    const QPointF& a = points[i];
    const QPointF& b = points[(i + 1) % points.size()];
    area += a.x() * b.y() - b.x() * a.y();
  }
  return area * 0.5;
}

// How many times the closed outline winds around p (0: outside).
int windingAt(const QVector<QPointF>& poly, const QPointF& p) {
  int winding = 0;
  for (int i = 0; i < poly.size(); ++i) {
    const QPointF& a = poly[i];
    const QPointF& b = poly[(i + 1) % poly.size()];
    const double side = (b.x() - a.x()) * (p.y() - a.y()) - (p.x() - a.x()) * (b.y() - a.y());
    if (a.y() <= p.y()) {
      if (b.y() > p.y() && side > 0) ++winding;
    } else if (b.y() <= p.y() && side < 0) {
      --winding;
    }
  }
  return winding;
}

manifold::SimplePolygon toPolygon(const QVector<QPointF>& points) {
  manifold::SimplePolygon out;
  out.reserve(points.size());
  for (const QPointF& p : points) out.push_back({p.x(), p.y()});
  return out;
}

// The area an outline covers, whichever way round it was drawn (a loop that crosses itself too).
CrossSection region(const Shape2DContour& contour) {
  const QVector<QPointF> points = closedSamples(contour, kTraceSteps);
  if (points.size() < 3) return {};
  return CrossSection(toPolygon(points), CrossSection::FillRule::NonZero);
}

// Straight-segment outlines from polygons, holes marked.
QVector<Shape2DContour> fromCrossSection(const CrossSection& area) {
  QVector<Shape2DContour> out;
  for (const manifold::SimplePolygon& poly : area.Simplify(0.002).ToPolygons()) {
    if (poly.size() < 3) continue;
    Shape2DContour contour;
    contour.closed = true;
    for (const manifold::vec2& p : poly) contour.nodes.push_back(Shape2DNode{QPointF(p.x, p.y), {}, {}});
    out.push_back(contour);
  }
  shape2d::assignHoles(out, true);
  return out;
}

// A line of round dots, each step the hull of two neighbours: a stroke with round ends and joins
// that may change width along the way.
CrossSection strokeLine(const QVector<QPointF>& line, const QVector<double>& halfWidths) {
  std::vector<CrossSection> pieces;
  auto dot = [](const QPointF& at, double r, manifold::SimplePolygon& out) {
    const int segs = std::clamp(static_cast<int>(std::ceil(2 * kPi2D * r / 0.1)), 16, 96);
    for (int i = 0; i < segs; ++i) {
      const double a = 2 * kPi2D * i / segs;
      out.push_back({at.x() + r * std::cos(a), at.y() + r * std::sin(a)});
    }
  };
  for (int i = 0; i < line.size(); ++i) {
    manifold::SimplePolygon pts;
    dot(line[i], halfWidths[i], pts);
    if (i > 0) dot(line[i - 1], halfWidths[i - 1], pts);
    else if (line.size() > 1) continue;
    pieces.push_back(CrossSection::Hull(pts));
  }
  if (pieces.empty()) return {};
  return CrossSection::BatchBoolean(pieces, manifold::OpType::Add);
}

// Removes nodes that sit on the one before, keeping the curve going in and out.
QVector<Shape2DNode> withoutRepeats(QVector<Shape2DNode> nodes, bool closed) {
  for (int i = nodes.size() - 1; i > 0 && nodes.size() > 1; --i) {
    if (std::hypot(nodes[i].pos.x() - nodes[i - 1].pos.x(), nodes[i].pos.y() - nodes[i - 1].pos.y()) > 1e-9) continue;
    nodes[i - 1].outHandle = nodes[i].outHandle;
    nodes.removeAt(i);
  }
  if (closed && nodes.size() > 1 &&
      std::hypot(nodes.front().pos.x() - nodes.back().pos.x(), nodes.front().pos.y() - nodes.back().pos.y()) < 1e-9) {
    nodes.front().inHandle = nodes.back().inHandle;
    nodes.removeLast();
  }
  return nodes;
}

Shape2DContour closedContour(const QVector<Shape2DNode>& nodes, bool hole = false) {
  Shape2DContour contour;
  contour.nodes = withoutRepeats(nodes, true);
  contour.closed = contour.nodes.size() >= 3;
  contour.hole = hole;
  return contour;
}

// An elliptical arc from angle `from` to `to` (radians, either direction) as curve nodes; both
// ends included, their outer handles left empty.
QVector<Shape2DNode> arcNodes(const QPointF& centre, double rx, double ry, double from, double to) {
  const int pieces = std::max(1, static_cast<int>(std::ceil(std::abs(to - from) / (kPi2D * 0.5) - 1e-9)));
  const double step = (to - from) / pieces;
  const double k = 4.0 / 3.0 * std::tan(step / 4);
  auto at = [&](double a) { return QPointF(centre.x() + rx * std::cos(a), centre.y() + ry * std::sin(a)); };
  auto tangent = [&](double a) { return QPointF(-rx * std::sin(a), ry * std::cos(a)); };
  QVector<Shape2DNode> out;
  for (int i = 0; i <= pieces; ++i) {
    const double a = from + step * i;
    Shape2DNode node{at(a), {}, {}};
    if (i > 0) node.inHandle = -k * tangent(a);
    if (i < pieces) node.outHandle = k * tangent(a);
    out.push_back(node);
  }
  return out;
}

QVector<Shape2DNode> ellipseNodes(const QPointF& centre, double rx, double ry) {
  QVector<Shape2DNode> nodes = arcNodes(centre, rx, ry, 0, 2 * kPi2D);
  nodes.front().inHandle = nodes.back().inHandle;
  nodes.removeLast();
  return nodes;
}

// The swirl's arms in a circle of radius 1: full line width in the middle, 40 % of it at the tips.
CrossSection swirlArms(int arms, double turns, double width) {
  arms = std::clamp(arms, 1, 12);
  const double sweep = std::max(0.05, turns) * 2 * kPi2D;
  const int steps = std::clamp(static_cast<int>(std::ceil((sweep + 1) / 0.03)), 40, 1200);
  std::vector<CrossSection> lines;
  for (int arm = 0; arm < arms; ++arm) {
    QVector<QPointF> line;
    QVector<double> half;
    for (int i = 0; i <= steps; ++i) {
      const double t = static_cast<double>(i) / steps;
      const double a = 2 * kPi2D * arm / arms - sweep * t;
      line.push_back(QPointF(t * std::cos(a), t * std::sin(a)));
      half.push_back(std::max(0.004, width * (1 - 0.6 * t)) * 0.5);
    }
    lines.push_back(strokeLine(line, half));
  }
  manifold::SimplePolygon rim;
  for (int i = 0; i < 128; ++i) rim.push_back({std::cos(2 * kPi2D * i / 128), std::sin(2 * kPi2D * i / 128)});
  return CrossSection::BatchBoolean(lines, manifold::OpType::Add) ^ CrossSection(rim);
}

// --- SVG ----------------------------------------------------------------------------------------

struct SvgStyle {
  bool fill = true;
  bool stroke = false;
  double strokeWidth = 1.0;
  bool evenOdd = false;
  bool hidden = false;
};

double svgUnit(const QString& unit) {
  if (unit == QLatin1String("mm")) return 1.0;
  if (unit == QLatin1String("cm")) return 10.0;
  if (unit == QLatin1String("in")) return 25.4;
  if (unit == QLatin1String("pt")) return 25.4 / 72.0;
  if (unit == QLatin1String("pc")) return 25.4 / 6.0;
  return 1.0;  // px and plain numbers: one unit is one mm here
}

// A length such as "12", "12.5mm" or "1e2px": the number and whether it had a real-world unit.
double svgLength(const QString& text, bool* physical = nullptr, double fallback = 0) {
  static const QRegularExpression re(QStringLiteral(R"(^\s*([-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?)\s*([a-zA-Z%]*)\s*$)"));
  const QRegularExpressionMatch match = re.match(text);
  if (physical) *physical = false;
  if (!match.hasMatch()) return fallback;
  const QString unit = match.captured(2).toLower();
  if (physical) *physical = unit == QLatin1String("mm") || unit == QLatin1String("cm") || unit == QLatin1String("in") ||
                            unit == QLatin1String("pt") || unit == QLatin1String("pc");
  return match.captured(1).toDouble() * svgUnit(unit);
}

QVector<double> svgNumbers(const QString& text) {
  static const QRegularExpression re(QStringLiteral(R"([-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?)"));
  QVector<double> out;
  QRegularExpressionMatchIterator it = re.globalMatch(text);
  while (it.hasNext()) out.push_back(it.next().captured(0).toDouble());
  return out;
}

QTransform svgTransform(const QString& text) {
  static const QRegularExpression item(QStringLiteral(R"((matrix|translate|scale|rotate|skewX|skewY)\s*\(([^)]*)\))"));
  QTransform total;
  QRegularExpressionMatchIterator it = item.globalMatch(text);
  while (it.hasNext()) {
    const QRegularExpressionMatch m = it.next();
    const QString kind = m.captured(1);
    const QVector<double> v = svgNumbers(m.captured(2));
    QTransform t;
    if (kind == QLatin1String("matrix") && v.size() >= 6) {
      t = QTransform(v[0], v[1], v[2], v[3], v[4], v[5]);
    } else if (kind == QLatin1String("translate") && !v.isEmpty()) {
      t = QTransform::fromTranslate(v[0], v.size() > 1 ? v[1] : 0);
    } else if (kind == QLatin1String("scale") && !v.isEmpty()) {
      t = QTransform::fromScale(v[0], v.size() > 1 ? v[1] : v[0]);
    } else if (kind == QLatin1String("rotate") && !v.isEmpty()) {
      const double a = v[0] * kPi2D / 180;
      const QTransform turn(std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0);
      if (v.size() >= 3) t = QTransform::fromTranslate(-v[1], -v[2]) * turn * QTransform::fromTranslate(v[1], v[2]);
      else t = turn;
    } else if (kind == QLatin1String("skewX") && !v.isEmpty()) {
      t = QTransform(1, 0, std::tan(v[0] * kPi2D / 180), 1, 0, 0);
    } else if (kind == QLatin1String("skewY") && !v.isEmpty()) {
      t = QTransform(1, std::tan(v[0] * kPi2D / 180), 0, 1, 0, 0);
    }
    // "A B" applies B first, then A.
    total = t * total;
  }
  return total;
}

SvgStyle svgStyle(const SvgStyle& inherited, const QXmlStreamAttributes& attributes) {
  SvgStyle style = inherited;
  QMap<QString, QString> values;
  for (const QXmlStreamAttribute& a : attributes) values[a.name().toString()] = a.value().toString().trimmed();
  // style="fill:none;stroke:#000" wins over the plain attributes.
  for (const QString& part : values.value(QStringLiteral("style")).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
    const int colon = part.indexOf(QLatin1Char(':'));
    if (colon > 0) values[part.left(colon).trimmed()] = part.mid(colon + 1).trimmed();
  }
  if (values.contains(QStringLiteral("fill"))) style.fill = values[QStringLiteral("fill")] != QLatin1String("none");
  if (values.contains(QStringLiteral("stroke"))) style.stroke = values[QStringLiteral("stroke")] != QLatin1String("none");
  if (values.contains(QStringLiteral("stroke-width"))) style.strokeWidth = svgLength(values[QStringLiteral("stroke-width")], nullptr, 1.0);
  if (values.contains(QStringLiteral("fill-rule"))) style.evenOdd = values[QStringLiteral("fill-rule")] == QLatin1String("evenodd");
  if (values.value(QStringLiteral("display")) == QLatin1String("none") ||
      values.value(QStringLiteral("visibility")) == QLatin1String("hidden"))
    style.hidden = true;
  return style;
}

// One subpath: nodes in the element's own units, and whether it was closed with Z.
struct SvgSubpath {
  QVector<Shape2DNode> nodes;
  bool closed = false;
};

// Path data (M L H V C S Q T A Z, upper and lower case) as curve nodes.
QVector<SvgSubpath> svgPathData(const QString& d) {
  QVector<SvgSubpath> out;
  int i = 0;
  const int n = d.size();
  auto skipSpace = [&] {
    while (i < n && (d[i].isSpace() || d[i] == QLatin1Char(','))) ++i;
  };
  // A number, or for arc flags a single 0 or 1 (they may be written without separators).
  auto number = [&](bool* ok, bool flag = false) -> double {
    skipSpace();
    *ok = false;
    if (i >= n) return 0;
    if (flag && (d[i] == QLatin1Char('0') || d[i] == QLatin1Char('1'))) {
      *ok = true;
      return d[i++] == QLatin1Char('1') ? 1 : 0;
    }
    const int start = i;
    if (d[i] == QLatin1Char('+') || d[i] == QLatin1Char('-')) ++i;
    bool digits = false, dot = false;
    while (i < n && (d[i].isDigit() || (d[i] == QLatin1Char('.') && !dot))) {
      if (d[i] == QLatin1Char('.')) dot = true;
      else digits = true;
      ++i;
    }
    if (digits && i < n && (d[i] == QLatin1Char('e') || d[i] == QLatin1Char('E'))) {
      int j = i + 1;
      if (j < n && (d[j] == QLatin1Char('+') || d[j] == QLatin1Char('-'))) ++j;
      if (j < n && d[j].isDigit()) {
        i = j;
        while (i < n && d[i].isDigit()) ++i;
      }
    }
    if (!digits) {
      i = start;
      return 0;
    }
    *ok = true;
    return QStringView(d).mid(start, i - start).toDouble();
  };
  SvgSubpath current;
  QPointF at, start, lastControl;
  QChar command, previous;
  auto flush = [&] {
    current.nodes = withoutRepeats(current.nodes, current.closed);
    if (current.nodes.size() >= 2) out.push_back(current);
    current = SvgSubpath{};
  };
  auto lineTo = [&](const QPointF& p) {
    if (current.nodes.isEmpty()) current.nodes.push_back(Shape2DNode{at, {}, {}});
    current.nodes.push_back(Shape2DNode{p, {}, {}});
    at = p;
  };
  auto cubicTo = [&](const QPointF& c1, const QPointF& c2, const QPointF& p) {
    if (current.nodes.isEmpty()) current.nodes.push_back(Shape2DNode{at, {}, {}});
    current.nodes.last().outHandle = c1 - at;
    current.nodes.push_back(Shape2DNode{p, c2 - p, {}});
    lastControl = c2;
    at = p;
  };
  while (true) {
    skipSpace();
    if (i >= n) break;
    if (d[i].isLetter()) {
      command = d[i++];
    } else if (command.isNull()) {
      break;  // numbers before any command: not path data
    }
    const bool relative = command.isLower();
    const QChar c = command.toUpper();
    const QPointF base = relative ? at : QPointF();
    bool ok = true;
    if (c == QLatin1Char('Z')) {
      if (!current.nodes.isEmpty()) {
        current.closed = true;
        flush();
      }
      at = start;
      previous = c;
      command = QChar();  // Z takes no numbers
      continue;
    }
    if (c == QLatin1Char('M')) {
      const double x = number(&ok), y = number(&ok);
      if (!ok) break;
      flush();
      at = start = base + QPointF(x, y);
      current.nodes.push_back(Shape2DNode{at, {}, {}});
      command = relative ? QLatin1Char('l') : QLatin1Char('L');  // more pairs are lines
    } else if (c == QLatin1Char('L')) {
      const double x = number(&ok), y = number(&ok);
      if (!ok) break;
      lineTo(base + QPointF(x, y));
    } else if (c == QLatin1Char('H')) {
      const double x = number(&ok);
      if (!ok) break;
      lineTo(QPointF(relative ? at.x() + x : x, at.y()));
    } else if (c == QLatin1Char('V')) {
      const double y = number(&ok);
      if (!ok) break;
      lineTo(QPointF(at.x(), relative ? at.y() + y : y));
    } else if (c == QLatin1Char('C')) {
      double v[6];
      for (double& value : v) value = number(&ok);
      if (!ok) break;
      cubicTo(base + QPointF(v[0], v[1]), base + QPointF(v[2], v[3]), base + QPointF(v[4], v[5]));
    } else if (c == QLatin1Char('S')) {
      double v[4];
      for (double& value : v) value = number(&ok);
      if (!ok) break;
      const QChar p = previous.toUpper();
      const QPointF c1 = (p == QLatin1Char('C') || p == QLatin1Char('S')) ? at * 2 - lastControl : at;
      cubicTo(c1, base + QPointF(v[0], v[1]), base + QPointF(v[2], v[3]));
    } else if (c == QLatin1Char('Q') || c == QLatin1Char('T')) {
      QPointF q, p;
      if (c == QLatin1Char('Q')) {
        double v[4];
        for (double& value : v) value = number(&ok);
        if (!ok) break;
        q = base + QPointF(v[0], v[1]);
        p = base + QPointF(v[2], v[3]);
      } else {
        const double x = number(&ok), y = number(&ok);
        if (!ok) break;
        const QChar prev = previous.toUpper();
        q = (prev == QLatin1Char('Q') || prev == QLatin1Char('T')) ? at * 2 - lastControl : at;
        p = base + QPointF(x, y);
      }
      const QPointF from = at;
      cubicTo(from + (q - from) * (2.0 / 3.0), p + (q - p) * (2.0 / 3.0), p);
      lastControl = q;  // a following T reflects the quadratic control point
    } else if (c == QLatin1Char('A')) {
      double rx = number(&ok), ry = number(&ok);
      const double rotation = number(&ok);
      const bool large = number(&ok, true) != 0;
      const bool sweep = number(&ok, true) != 0;
      const double x = number(&ok), y = number(&ok);
      if (!ok) break;
      const QPointF p = base + QPointF(x, y);
      const QPointF p0 = at;
      rx = std::abs(rx);
      ry = std::abs(ry);
      if (std::hypot(p.x() - p0.x(), p.y() - p0.y()) < 1e-12) {
      } else if (rx < 1e-12 || ry < 1e-12) {
        lineTo(p);
      } else {
        // Endpoint to centre form, as the SVG specification describes it, then pieces of at most 90 degrees.
        const double phi = rotation * kPi2D / 180, cs = std::cos(phi), sn = std::sin(phi);
        const double dx = (p0.x() - p.x()) / 2, dy = (p0.y() - p.y()) / 2;
        const double x1 = cs * dx + sn * dy, y1 = -sn * dx + cs * dy;
        const double lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
        if (lambda > 1) {
          rx *= std::sqrt(lambda);
          ry *= std::sqrt(lambda);
        }
        const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        const double coef = (large == sweep ? -1.0 : 1.0) * std::sqrt(std::max(0.0, den > 0 ? num / den : 0.0));
        const double cxp = coef * rx * y1 / ry, cyp = -coef * ry * x1 / rx;
        const double cx = cs * cxp - sn * cyp + (p0.x() + p.x()) / 2, cy = sn * cxp + cs * cyp + (p0.y() + p.y()) / 2;
        auto angle = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
        const double theta = angle(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
        double delta = angle((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
        if (!sweep && delta > 0) delta -= 2 * kPi2D;
        if (sweep && delta < 0) delta += 2 * kPi2D;
        const int pieces = std::max(1, static_cast<int>(std::ceil(std::abs(delta) / (kPi2D / 2) - 1e-9)));
        const double step = delta / pieces, k = 4.0 / 3.0 * std::tan(step / 4);
        auto onEllipse = [&](double t) {
          return QPointF(cx + rx * std::cos(t) * cs - ry * std::sin(t) * sn, cy + rx * std::cos(t) * sn + ry * std::sin(t) * cs);
        };
        auto slope = [&](double t) {
          return QPointF(-rx * std::sin(t) * cs - ry * std::cos(t) * sn, -rx * std::sin(t) * sn + ry * std::cos(t) * cs);
        };
        for (int s = 0; s < pieces; ++s) {
          const double t1 = theta + step * s, t2 = t1 + step;
          const QPointF end = s + 1 == pieces ? p : onEllipse(t2);
          cubicTo(onEllipse(t1) + slope(t1) * k, onEllipse(t2) - slope(t2) * k, end);
        }
      }
    } else {
      break;  // an unknown command ends the path, as in a browser
    }
    previous = c;
  }
  flush();
  return out;
}

class SvgReader {
 public:
  QVector<Shape2DContour> contours;

  bool read(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
      if (error) *error = QStringLiteral("Could not open the file.");
      return false;
    }
    QXmlStreamReader xml(&file);
    if (!xml.readNextStartElement() || xml.name() != QLatin1String("svg")) {
      if (error) *error = QStringLiteral("That is not an SVG file.");
      return false;
    }
    // The page: viewBox numbers map onto the width and height. Without a real-world unit, one
    // viewBox unit is one mm.
    const QXmlStreamAttributes attributes = xml.attributes();
    const QVector<double> box = svgNumbers(attributes.value(QStringLiteral("viewBox")).toString());
    double scale = 1.0;
    QTransform page;
    if (box.size() == 4 && box[2] > 0 && box[3] > 0) {
      bool wReal = false, hReal = false;
      const double w = svgLength(attributes.value(QStringLiteral("width")).toString(), &wReal, 0);
      const double h = svgLength(attributes.value(QStringLiteral("height")).toString(), &hReal, 0);
      if (w > 0 && h > 0) scale = std::min(w / box[2], h / box[3]);
      else if (w > 0) scale = w / box[2];
      else if (h > 0) scale = h / box[3];
      page = QTransform::fromTranslate(-box[0], -box[1]);
    }
    page = page * QTransform::fromScale(scale, -scale);  // SVG's Y points down, ours up
    const SvgStyle style = svgStyle(SvgStyle{}, attributes);
    if (style.hidden) return finish(error);
    const QTransform root = svgTransform(attributes.value(QStringLiteral("transform")).toString()) * page;
    while (xml.readNextStartElement()) walk(xml, root, style);
    return finish(error);
  }

 private:
  int nextCut = 1;

  bool finish(QString* error) {
    if (contours.isEmpty()) {
      if (error) *error = QStringLiteral("No shapes found in that SVG. Filled or outlined paths, rectangles, circles, "
                                         "ellipses, polygons and lines are read.");
      return false;
    }
    const QPointF centre = shape2d::bounds(contours).center();
    for (Shape2DContour& contour : contours) shape2d::transform(contour, QTransform::fromTranslate(-centre.x(), -centre.y()));
    return true;
  }

  void walk(QXmlStreamReader& xml, const QTransform& outer, const SvgStyle& inherited) {
    const QString name = xml.name().toString();
    static const QStringList skipped{"defs", "clipPath", "mask", "symbol", "marker", "pattern", "linearGradient",
                                     "radialGradient", "filter", "metadata", "title", "desc", "style", "script", "text",
                                     "foreignObject", "image", "use"};
    if (skipped.contains(name)) {
      xml.skipCurrentElement();
      return;
    }
    const QXmlStreamAttributes attributes = xml.attributes();
    const SvgStyle style = svgStyle(inherited, attributes);
    if (style.hidden) {
      xml.skipCurrentElement();
      return;
    }
    QTransform here = svgTransform(attributes.value(QStringLiteral("transform")).toString()) * outer;
    if (name == QLatin1String("svg"))
      here = QTransform::fromTranslate(svgLength(attributes.value(QStringLiteral("x")).toString()),
                                       svgLength(attributes.value(QStringLiteral("y")).toString())) * here;
    QVector<SvgSubpath> paths;
    auto number = [&](const char* key) { return svgLength(attributes.value(QLatin1String(key)).toString()); };
    if (name == QLatin1String("path")) {
      paths = svgPathData(attributes.value(QStringLiteral("d")).toString());
    } else if (name == QLatin1String("rect")) {
      const double x = number("x"), y = number("y"), w = number("width"), h = number("height");
      double rx = number("rx"), ry = number("ry");
      if (attributes.hasAttribute(QStringLiteral("rx")) && !attributes.hasAttribute(QStringLiteral("ry"))) ry = rx;
      if (attributes.hasAttribute(QStringLiteral("ry")) && !attributes.hasAttribute(QStringLiteral("rx"))) rx = ry;
      rx = std::clamp(rx, 0.0, w / 2);
      ry = std::clamp(ry, 0.0, h / 2);
      if (w > 0 && h > 0) {
        SvgSubpath rect;
        rect.closed = true;
        if (rx < 1e-9 || ry < 1e-9) {
          for (const QPointF& p : {QPointF(x, y), QPointF(x + w, y), QPointF(x + w, y + h), QPointF(x, y + h)})
            rect.nodes.push_back(Shape2DNode{p, {}, {}});
        } else {
          rect.nodes += arcNodes(QPointF(x + w - rx, y + ry), rx, ry, -kPi2D / 2, 0);
          rect.nodes += arcNodes(QPointF(x + w - rx, y + h - ry), rx, ry, 0, kPi2D / 2);
          rect.nodes += arcNodes(QPointF(x + rx, y + h - ry), rx, ry, kPi2D / 2, kPi2D);
          rect.nodes += arcNodes(QPointF(x + rx, y + ry), rx, ry, kPi2D, kPi2D * 1.5);
          rect.nodes = withoutRepeats(rect.nodes, true);
        }
        paths.push_back(rect);
      }
    } else if (name == QLatin1String("circle") || name == QLatin1String("ellipse")) {
      const bool circle = name == QLatin1String("circle");
      const double rx = circle ? number("r") : number("rx"), ry = circle ? number("r") : number("ry");
      if (rx > 0 && ry > 0) paths.push_back(SvgSubpath{ellipseNodes(QPointF(number("cx"), number("cy")), rx, ry), true});
    } else if (name == QLatin1String("polygon") || name == QLatin1String("polyline")) {
      const QVector<double> v = svgNumbers(attributes.value(QStringLiteral("points")).toString());
      SvgSubpath line;
      line.closed = name == QLatin1String("polygon");
      for (int k = 0; k + 1 < v.size(); k += 2) line.nodes.push_back(Shape2DNode{QPointF(v[k], v[k + 1]), {}, {}});
      line.nodes = withoutRepeats(line.nodes, line.closed);
      if (line.nodes.size() >= 2) paths.push_back(line);
    } else if (name == QLatin1String("line")) {
      SvgSubpath line;
      line.nodes = {Shape2DNode{QPointF(number("x1"), number("y1")), {}, {}}, Shape2DNode{QPointF(number("x2"), number("y2")), {}, {}}};
      paths.push_back(line);
    } else {
      // A group (or anything else that may hold shapes): its children, with its transform and style.
      while (xml.readNextStartElement()) walk(xml, here, style);
      return;
    }
    xml.skipCurrentElement();
    add(paths, here, style, name == QLatin1String("line") || name == QLatin1String("polyline"));
  }

  void add(const QVector<SvgSubpath>& paths, const QTransform& toMm, const SvgStyle& style, bool lineOnly) {
    if (paths.isEmpty()) return;
    // A filled area: every subpath counts as closed.
    if (style.fill && !lineOnly) {
      QVector<Shape2DContour> filled;
      for (const SvgSubpath& path : paths) {
        Shape2DContour contour = closedContour(path.nodes);
        if (!contour.closed) continue;
        shape2d::transform(contour, toMm);
        filled.push_back(contour);
      }
      shape2d::assignHoles(filled, style.evenOdd);
      keep(filled);
    }
    // An outline: the line drawn at its width, round at the ends.
    const double widthScale = std::sqrt(std::abs(toMm.determinant()));
    const double half = style.strokeWidth * widthScale * 0.5;
    if (style.stroke && half > 1e-6) {
      std::vector<CrossSection> strokes;
      for (const SvgSubpath& path : paths) {
        Shape2DContour line;
        line.nodes = path.nodes;
        line.closed = path.closed;
        shape2d::transform(line, toMm);
        QVector<QPointF> points = sampleShape2D(line.nodes, line.closed, kTraceSteps);
        if (line.closed && points.size() >= 3) {
          const CrossSection area(toPolygon(points), CrossSection::FillRule::NonZero);
          strokes.push_back(area.Offset(half, CrossSection::JoinType::Round) - area.Offset(-half, CrossSection::JoinType::Round));
        } else if (!points.isEmpty()) {
          strokes.push_back(strokeLine(points, QVector<double>(points.size(), half)));
        }
      }
      if (!strokes.empty()) keep(fromCrossSection(CrossSection::BatchBoolean(strokes, manifold::OpType::Add)));
    }
  }

  // Each element's holes cut only that element: its cut numbers are its own.
  void keep(QVector<Shape2DContour> found) {
    int highest = 0;
    for (Shape2DContour& contour : found) {
      highest = std::max(highest, contour.cut);
      contour.cut += nextCut - 1;
      contours.push_back(contour);
    }
    nextCut += highest;
  }
};

// True when two sets of outlines cover the same area (a shape that is its own mirror).
bool sameArea(const QVector<Shape2DContour>& a, const QVector<Shape2DContour>& b) {
  auto area = [](const QVector<Shape2DContour>& set) {
    std::vector<CrossSection> solids, holes;
    for (const Shape2DContour& contour : set)
      if (contour.closed && contour.nodes.size() >= 3) (contour.hole ? holes : solids).push_back(region(contour));
    CrossSection out = CrossSection::BatchBoolean(solids, manifold::OpType::Add);
    if (!holes.empty()) out = out - CrossSection::BatchBoolean(holes, manifold::OpType::Add);
    return out;
  };
  const CrossSection x = area(a), y = area(b);
  const double size = std::max(x.Area(), 1e-9);
  return ((x - y).Area() + (y - x).Area()) < size * 1e-4;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------

namespace shape2d {

void transform(Shape2DContour& contour, const QTransform& t) {
  for (Shape2DNode& node : contour.nodes) {
    const QPointF at = node.pos;
    node.pos = t.map(at);
    if (!zeroHandle(node.inHandle)) node.inHandle = t.map(at + node.inHandle) - node.pos;
    if (!zeroHandle(node.outHandle)) node.outHandle = t.map(at + node.outHandle) - node.pos;
  }
}

QRectF bounds(const QVector<Shape2DContour>& contours) {
  QRectF all;
  for (const Shape2DContour& contour : contours) {
    if (contour.nodes.size() < 2) continue;
    const QRectF box = contourBounds(contour);
    all = all.isNull() ? box : all.united(box);
  }
  return all;
}

QVector<Shape2DContour> combine(const QVector<Shape2DContour>& operands, Combine op) {
  std::vector<CrossSection> all, solids, holes;
  for (const Shape2DContour& contour : operands) {
    if (!contour.closed || contour.nodes.size() < 3) continue;
    all.push_back(region(contour));
    (contour.hole ? holes : solids).push_back(all.back());
  }
  if (all.empty()) return {};
  // Holes only: work on them as areas, and the result is holes again.
  const bool holesOnly = solids.empty();
  if (holesOnly) std::swap(solids, holes);
  auto unite = [](const std::vector<CrossSection>& parts) { return CrossSection::BatchBoolean(parts, manifold::OpType::Add); };
  CrossSection result;
  if (op == Combine::Cut) {
    // The first one minus everything else, shapes and holes alike.
    result = all.front();
    if (all.size() > 1) result = result - unite(std::vector<CrossSection>(all.begin() + 1, all.end()));
  } else {
    // What the selection looks like: its shapes together (or where they all overlap), minus its holes.
    result = solids.front();
    if (op == Combine::Merge) result = unite(solids);
    else
      for (size_t i = 1; i < solids.size(); ++i) result = result ^ solids[i];
    if (!holes.empty()) result = result - unite(holes);
  }
  QVector<Shape2DContour> out = fromCrossSection(result);
  if (holesOnly)
    for (Shape2DContour& contour : out) contour.hole = !contour.hole;
  return out;
}

bool roundCorner(Shape2DContour& contour, int index, double radius, double* used) {
  const int n = contour.nodes.size();
  if (n < 3 || index < 0 || index >= n || !(radius > 0)) return false;
  if (!contour.closed && (index == 0 || index == n - 1)) return false;
  const int prev = (index - 1 + n) % n, next = (index + 1) % n;
  const Shape2DNode node = contour.nodes[index];
  const Shape2DNode& before = contour.nodes[prev];
  const Shape2DNode& after = contour.nodes[next];
  // The directions the two sides leave the corner in: along a curve handle when there is one.
  const QPointF toPrev = !zeroHandle(node.inHandle) ? node.inHandle : (before.pos + before.outHandle) - node.pos;
  const QPointF toNext = !zeroHandle(node.outHandle) ? node.outHandle : (after.pos + after.inHandle) - node.pos;
  const double lenPrev = std::hypot(before.pos.x() - node.pos.x(), before.pos.y() - node.pos.y());
  const double lenNext = std::hypot(after.pos.x() - node.pos.x(), after.pos.y() - node.pos.y());
  if (lenPrev < 1e-9 || lenNext < 1e-9) return false;
  const QPointF u = safeNormalized(toPrev), v = safeNormalized(toNext);
  const double between = std::acos(std::clamp(u.x() * v.x() + u.y() * v.y(), -1.0, 1.0));
  if (between < 1e-3 || between > kPi2D - 1e-3) return false;  // folded back on itself, or straight
  const double half = between / 2;
  // Leave half of a side for a sharp neighbour that may be rounded too; a neighbour that is
  // already a curve can be met almost at its node.
  auto room = [](double length, const Shape2DNode& other) {
    return length * (zeroHandle(other.inHandle) && zeroHandle(other.outHandle) ? 0.5 : 0.98);
  };
  double r = radius;
  double t = r / std::tan(half);
  const double fit = std::min(room(lenPrev, before), room(lenNext, after));
  if (t > fit) {
    t = fit;
    r = t * std::tan(half);
  }
  const double k = 4.0 / 3.0 * std::tan((kPi2D - between) / 4) * r;
  Shape2DNode first{node.pos + u * t, {}, -u * k};
  Shape2DNode second{node.pos + v * t, -v * k, {}};
  // A side that was a curve keeps bending the same way into the corner, shortened with it.
  if (!zeroHandle(node.inHandle)) first.inHandle = node.inHandle * (1 - t / lenPrev);
  if (!zeroHandle(node.outHandle)) second.outHandle = node.outHandle * (1 - t / lenNext);
  contour.nodes[index] = first;
  contour.nodes.insert(index + 1, second);
  if (used) *used = r;
  return true;
}

int roundAllCorners(Shape2DContour& contour, double radius) {
  int count = 0;
  for (int i = contour.nodes.size() - 1; i >= 0; --i) {
    const Shape2DNode& node = contour.nodes[i];
    if (!zeroHandle(node.inHandle) || !zeroHandle(node.outHandle)) continue;
    if (roundCorner(contour, i, radius)) ++count;
  }
  return count;
}

void assignHoles(QVector<Shape2DContour>& contours, bool evenOdd) {
  const int n = contours.size();
  QVector<QVector<QPointF>> polys(n);
  for (int i = 0; i < n; ++i) polys[i] = closedSamples(contours[i], 16);
  for (int i = 0; i < n; ++i) {
    if (polys[i].size() < 3) continue;
    // A point on this outline: how the others wind around it says what is inside of what.
    const QPointF probe = polys[i].front();
    int around = 0, depth = 0;
    for (int j = 0; j < n; ++j) {
      if (j == i || polys[j].size() < 3) continue;
      const int w = windingAt(polys[j], probe);
      around += w;
      if (w != 0) ++depth;
    }
    bool hole;
    if (evenOdd) {
      hole = depth % 2 == 1;
    } else {
      const int own = signedArea(polys[i]) >= 0 ? 1 : -1;
      hole = around != 0 && around + own == 0;
    }
    // Holes share a cut number with the outline around them; a shape inside a hole (an island) gets
    // the next one, so that hole does not cut it.
    contours[i].hole = hole;
    contours[i].cut = 1 + depth / 2 + (!hole && depth % 2 == 1 ? 1 : 0);
  }
}

QVector<Shape2DContour> textOutline(const QString& text, const QFont& font) {
  QPainterPath path;
  path.addText(QPointF(0, 0), font, text);
  QVector<Shape2DContour> out;
  QVector<Shape2DNode> nodes;
  auto flush = [&] {
    Shape2DContour contour = closedContour(nodes);
    if (contour.closed) out.push_back(contour);
    nodes.clear();
  };
  for (int i = 0; i < path.elementCount(); ++i) {
    const QPainterPath::Element e = path.elementAt(i);
    const QPointF p(e.x, -e.y);  // Qt's Y points down
    if (e.type == QPainterPath::MoveToElement) {
      flush();
      nodes.push_back(Shape2DNode{p, {}, {}});
    } else if (e.type == QPainterPath::LineToElement) {
      nodes.push_back(Shape2DNode{p, {}, {}});
    } else if (e.type == QPainterPath::CurveToElement && i + 2 < path.elementCount() && !nodes.isEmpty()) {
      const QPainterPath::Element c2 = path.elementAt(i + 1);
      const QPainterPath::Element end = path.elementAt(i + 2);
      const QPointF to(end.x, -end.y);
      nodes.last().outHandle = p - nodes.last().pos;
      nodes.push_back(Shape2DNode{to, QPointF(c2.x, -c2.y) - to, {}});
      i += 2;
    }
  }
  flush();
  assignHoles(out, false);
  return out;
}

QVector<Shape2DContour> shapesInBox(int kind, const QPointF& a, const QPointF& b, const StampOptions& o) {
  const double x0 = std::min(a.x(), b.x()), x1 = std::max(a.x(), b.x());
  const double y0 = std::min(a.y(), b.y()), y1 = std::max(a.y(), b.y());
  const double w = x1 - x0, h = y1 - y0;
  if (w < 1e-6 && h < 1e-6) return {};
  const QPointF c((x0 + x1) / 2, (y0 + y1) / 2);
  const double rx = std::max(w / 2, 1e-4), ry = std::max(h / 2, 1e-4);
  // A point given on the circle of radius 1, stretched to the box.
  auto inBox = [&](double ux, double uy) { return QPointF(c.x() + ux * rx, c.y() + uy * ry); };
  const double width = std::clamp(o.inner, 1.0, 99.0) / 100.0;
  QVector<Shape2DContour> out;
  switch (kind) {
    case 6: {  // star
      const int points = std::clamp(o.sides, 3, 64);
      const double inner = std::clamp(1.0 - o.inner / 100.0, 0.05, 0.98);
      QVector<Shape2DNode> nodes;
      for (int i = 0; i < points * 2; ++i) {
        const double angle = kPi2D / 2 + kPi2D * i / points;
        const double r = i % 2 ? inner : 1.0;
        nodes.push_back(Shape2DNode{inBox(r * std::cos(angle), r * std::sin(angle)), {}, {}});
      }
      out.push_back(closedContour(nodes));
      break;
    }
    case 7: {  // slot: a bar with fully round ends
      const double r = std::min(w, h) / 2;
      if (std::abs(w - h) < 1e-6) out.push_back(closedContour(ellipseNodes(c, rx, ry)));
      else out.push_back(closedContour(shapeBetween(2, a, b, 0, r)));
      break;
    }
    case 8: {  // ring
      out.push_back(closedContour(ellipseNodes(c, rx, ry)));
      out.push_back(closedContour(ellipseNodes(c, rx * (1 - width), ry * (1 - width)), true));
      break;
    }
    case 9: {  // arc: part of a ring, centred at the top
      const double sweep = std::clamp(o.angle, 1.0, 360.0) * kPi2D / 180;
      if (sweep > 2 * kPi2D - 1e-6) return shapesInBox(8, a, b, o);
      const double from = kPi2D / 2 - sweep / 2, to = kPi2D / 2 + sweep / 2;
      QVector<Shape2DNode> nodes = arcNodes(c, rx, ry, from, to);
      nodes += arcNodes(c, rx * (1 - width), ry * (1 - width), to, from);
      out.push_back(closedContour(nodes));
      break;
    }
    case 10: {  // heart
      auto node = [&](double x, double y, double ix, double iy, double ox, double oy) {
        return Shape2DNode{inBox(x, y), QPointF(ix * rx, iy * ry), QPointF(ox * rx, oy * ry)};
      };
      out.push_back(closedContour({node(0, -1, 0.42, 0.5, -0.42, 0.5), node(-1, 0.36, 0, -0.42, 0, 0.36),
                                   node(-0.5, 1, -0.3, 0, 0.3, 0), node(0, 0.62, -0.06, 0.28, 0.06, 0.28),
                                   node(0.5, 1, -0.3, 0, 0.3, 0), node(1, 0.36, 0, 0.36, 0, -0.42)}));
      break;
    }
    case 11: {  // arrow pointing right
      QVector<Shape2DNode> nodes;
      for (const QPointF& p : {QPointF(-1, -0.32), QPointF(0.15, -0.32), QPointF(0.15, -1), QPointF(1, 0), QPointF(0.15, 1),
                               QPointF(0.15, 0.32), QPointF(-1, 0.32)})
        nodes.push_back(Shape2DNode{inBox(p.x(), p.y()), {}, {}});
      out.push_back(closedContour(nodes));
      break;
    }
    case 12: {  // swirl
      for (Shape2DContour contour : fromCrossSection(swirlArms(o.sides, o.turns, width))) {
        transform(contour, QTransform(rx, 0, 0, ry, c.x(), c.y()));
        out.push_back(contour);
      }
      break;
    }
    case 13: {  // text, fitted into the box with its proportions kept
      if (o.text.trimmed().isEmpty()) return {};
      QFont font(o.fontFamily.isEmpty() ? QFont().family() : o.fontFamily);
      font.setPointSizeF(100);
      font.setBold(o.bold);
      QVector<Shape2DContour> glyphs = textOutline(o.text, font);
      const QRectF box = bounds(glyphs);
      if (glyphs.isEmpty() || box.width() < 1e-9 || box.height() < 1e-9) return {};
      const double s = std::min(std::max(w, 1e-4) / box.width(), std::max(h, 1e-4) / box.height());
      const QTransform fit = QTransform::fromTranslate(-box.center().x(), -box.center().y()) * QTransform::fromScale(s, s) *
                             QTransform::fromTranslate(c.x(), c.y());
      for (Shape2DContour& contour : glyphs) transform(contour, fit);
      return glyphs;
    }
    default:
      out.push_back(closedContour(shapeBetween(kind, a, b, o.sides, o.corner)));
      break;
  }
  QVector<Shape2DContour> kept;
  for (const Shape2DContour& contour : out)
    if (contour.closed) kept.push_back(contour);
  return kept;
}

QVector<Shape2DContour> readSvg(const QString& path, QString* error) {
  SvgReader reader;
  if (!reader.read(path, error)) return {};
  return reader.contours;
}

}  // namespace shape2d

// ---------------------------------------------------------------------------------------------------

class Shape2DEditorWidget : public QWidget {
 public:
  explicit Shape2DEditorWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumSize(760, 540);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    contours.push_back({});
  }

  // --- settings -------------------------------------------------------------------------------------
  void setGridStep(double step) {
    gridStep = std::max(step, 0.001);
    update();
  }
  double grid() const { return gridStep; }
  void setSnapBends(bool on) {
    snapBends = on;
    update();
  }
  void setInk(const QColor& color) {
    const QColor next = color.isValid() ? color : QColor("#e87400");
    if (next != ink) dirty = true;
    ink = next;
    update();
  }
  QColor inkColor() const { return ink; }
  void setPrimitive(int kind) {
    primitive = std::clamp(kind, 0, 13);
    if (drawMode) {
      cancelStamp();
      applyMode();
      if (primitive == 0) updateStatus(QStringLiteral("Draw: click to place nodes, click the first node to close the loop."));
    }
    update();
  }
  void setDrawFrom(int from) { drawFrom = std::clamp(from, 0, 2); }
  void setPolygonSides(int count) {
    stampOptions.sides = std::clamp(count, 1, 64);
    update();
  }
  void setCornerRadius(double radius) { stampOptions.corner = std::max(0.0, radius); }
  // Star depth, ring and arc width, swirl line width: % of the size.
  void setStampInner(double percent) {
    stampOptions.inner = percent;
    update();
  }
  void setStampAngle(double degrees) {
    stampOptions.angle = degrees;
    update();
  }
  void setStampTurns(double turns) {
    stampOptions.turns = turns;
    update();
  }
  void setStampText(const QString& text, const QString& family, bool bold) {
    stampOptions.text = text;
    stampOptions.fontFamily = family;
    stampOptions.bold = bold;
    update();
  }
  // 0 off, 1 left-right (across the vertical line through 0,0), 2 top-bottom.
  void setMirrorMode(int mode) {
    mirrorMode = std::clamp(mode, 0, 2);
    if (mirrorMode)
      updateStatus(mirrorMode == 1 ? QStringLiteral("Mirror left–right: what you draw also appears mirrored across the dashed line. A loop that starts and ends on the line becomes one whole shape.")
                                   : QStringLiteral("Mirror top–bottom: what you draw also appears mirrored across the dashed line. A loop that starts and ends on the line becomes one whole shape."));
    update();
  }
  void setChangeHook(std::function<void()> hook) { changeHook = std::move(hook); }
  void setSelectionHook(std::function<void()> hook) { selectionHook = std::move(hook); }
  void setStatusSink(QLabel* label) { statusLabel = label; }

  // --- measuring ------------------------------------------------------------------------------------
  void setMeasuring(bool on) {
    measuring = on;
    measureHaveA = measureHaveB = measureHoverValid = false;
    if (on) {
      cancelStamp();
      updateStatus(QStringLiteral("Measure: click the first point. Nodes and shape centres snap, on every layer. Measurements stay until you delete them."));
    } else {
      applyMode();
      updateStatus(QStringLiteral("Measure off."));
    }
    notify();
    update();
  }
  bool isMeasuring() const { return measuring; }

  // --- dirty state and undo -------------------------------------------------------------------------
  bool isDirty() const { return dirty; }
  void markClean() { dirty = false; }
  bool canUndo() const { return !undoStack.isEmpty(); }
  bool canRedo() const { return !redoStack.isEmpty(); }
  void undo() {
    if (undoStack.isEmpty() || draggingNode || draggingHandle >= 0 || draggingShape) return;
    redoStack.push_back(snapshot());
    restore(undoStack.takeLast());
    updateStatus(QStringLiteral("Undone."));
  }
  void redo() {
    if (redoStack.isEmpty() || draggingNode || draggingHandle >= 0 || draggingShape) return;
    undoStack.push_back(snapshot());
    restore(redoStack.takeLast());
    updateStatus(QStringLiteral("Redone."));
  }

  // --- layers ---------------------------------------------------------------------------------------
  void setLayer(int layer) {
    currentLayer = std::max(1, layer);
    for (Shape2DContour& contour : contours) {
      if (!contour.closed && contour.nodes.isEmpty()) {
        contour.layer = currentLayer;
        contour.cut = currentLayer;
      }
    }
    if (activeContour >= 0 && activeContour < contours.size() && contours[activeContour].layer != currentLayer) {
      active = -1;
      selectedContour = -1;
      also.clear();
      draggingNode = false;
      draggingHandle = -1;
    }
    updateStatus(QStringLiteral("Layer %1. Other layers are locked.").arg(currentLayer));
    if (selectionHook) selectionHook();
    repaint();
  }
  int layer() const { return currentLayer; }
  QList<int> layerIds() const {
    QList<int> ids = openLayers;
    for (const Shape2DContour& contour : contours)
      if (contour.layer > 0 && !ids.contains(contour.layer)) ids.push_back(contour.layer);
    std::sort(ids.begin(), ids.end());
    if (ids.isEmpty()) ids.push_back(1);
    return ids;
  }
  int shapesOnLayer(int layer) const {
    int count = 0;
    for (const Shape2DContour& contour : contours)
      if (contour.layer == layer && contour.closed && contour.nodes.size() >= 3) ++count;
    return count;
  }
  int holesOnLayer(int layer) const {
    int count = 0;
    for (const Shape2DContour& contour : contours)
      if (contour.layer == layer && contour.hole && contour.closed && contour.nodes.size() >= 3) ++count;
    return count;
  }
  void addLayer() {
    pushUndo();
    // Past every layer number still in use, including those kept inside merged layers, so an
    // Unmerge never drops old shapes into a newer layer.
    int next = 1;
    for (int id : layerIds()) next = std::max(next, id + 1);
    for (const Shape2DContour& contour : contours) next = std::max(next, contour.cut + 1);
    openLayers.push_back(next);
    currentLayer = next;
    deselect();
    notify();
    update();
  }
  bool removeCurrentLayer() {
    const QList<int> ids = layerIds();
    if (ids.size() <= 1) return false;
    if (shapesOnLayer(currentLayer) > 0) return false;
    pushUndo();
    openLayers.removeAll(currentLayer);
    const int fallback = ids.front() == currentLayer ? ids.at(1) : ids.front();
    for (Shape2DContour& contour : contours) {
      if (contour.layer != currentLayer) continue;
      contour.layer = fallback;
      contour.cut = fallback;
    }
    currentLayer = layerIds().front();
    deselect();
    notify();
    update();
    return true;
  }
  bool mergeLayers(const QList<int>& ids) {
    if (ids.size() < 2) return false;
    pushUndo();
    int destination = ids.front();
    for (int id : ids) destination = std::min(destination, id);
    for (Shape2DContour& contour : contours) {
      if (!ids.contains(contour.layer)) continue;
      if (contour.cut <= 0) contour.cut = contour.layer;
      contour.layer = destination;
    }
    for (int id : ids)
      if (id != destination) openLayers.removeAll(id);
    if (!openLayers.contains(destination)) openLayers.push_back(destination);
    currentLayer = destination;
    deselect();
    notify();
    update();
    return true;
  }
  bool unmergeLayers() {
    bool any = false;
    for (const Shape2DContour& contour : contours)
      if (contour.cut > 0 && contour.layer != contour.cut) any = true;
    if (!any) return false;
    pushUndo();
    for (Shape2DContour& contour : contours) {
      if (contour.cut <= 0) contour.cut = contour.layer;
      contour.layer = contour.cut;
    }
    for (const Shape2DContour& contour : contours)
      if (contour.layer > 0 && !openLayers.contains(contour.layer)) openLayers.push_back(contour.layer);
    currentLayer = layerIds().front();
    deselect();
    notify();
    update();
    return true;
  }

  // --- levels ---------------------------------------------------------------------------------------
  // Levels stack in 3D: level 1 stands on the plate, level 2 on top of it, and so on, each level its
  // own part. Only the current level's contours live in `contours`; the others wait in otherLevels,
  // so everything that works on layers keeps working inside one level.
  int level() const { return currentLevel; }
  QList<int> levelIds() const {
    QList<int> ids = otherLevels.keys();
    if (!ids.contains(currentLevel)) ids.push_back(currentLevel);
    std::sort(ids.begin(), ids.end());
    return ids;
  }
  double levelHeight(int level) const { return shape2DLevelHeight(levelHeights, level); }
  QMap<int, double> levelHeightMap() const {
    QMap<int, double> out;
    for (int id : levelIds()) out[id] = levelHeight(id);
    return out;
  }
  void setLevelHeight(int level, double height) {
    if (!(height > 0) || std::abs(levelHeight(level) - height) < 1e-9) return;
    pushUndo(QStringLiteral("level-height-%1").arg(level));
    levelHeights[level] = height;
    notify();
    update();
  }
  int shapesOnLevel(int level) const {
    int count = 0;
    for (const Shape2DContour& contour : level == currentLevel ? contours : otherLevels.value(level).contours)
      if (!contour.hole && contour.closed && contour.nodes.size() >= 3) ++count;
    return count;
  }
  int holesOnLevel(int level) const {
    int count = 0;
    for (const Shape2DContour& contour : level == currentLevel ? contours : otherLevels.value(level).contours)
      if (contour.hole && contour.closed && contour.nodes.size() >= 3) ++count;
    return count;
  }
  void setLevel(int level) {
    level = std::max(1, level);
    if (level == currentLevel) return;
    switchLevel(level);
    updateStatus(level > 1 ? QStringLiteral("Level %1. Level %2 shows faded underneath; its nodes and centres snap.").arg(level).arg(level - 1)
                           : QStringLiteral("Level 1, the one on the plate."));
    notify();
    update();
  }
  void addLevel() {
    pushUndo();
    int next = 1;
    for (int id : levelIds()) next = std::max(next, id + 1);
    switchLevel(next);
    updateStatus(QStringLiteral("Level %1 added on top, %2 mm thick. Level %3 shows faded underneath.")
                     .arg(next)
                     .arg(levelHeight(next), 0, 'g', 4)
                     .arg(next - 1));
    notify();
    update();
  }
  // Removes an empty level; the levels above move down one.
  bool removeCurrentLevel() {
    const QList<int> ids = levelIds();
    if (ids.size() <= 1) return false;
    for (const Shape2DContour& contour : contours)
      if (!contour.nodes.isEmpty()) return false;
    pushUndo();
    const int gone = currentLevel;
    QMap<int, LevelState> moved;
    QMap<int, double> heights;
    for (auto it = otherLevels.cbegin(); it != otherLevels.cend(); ++it) moved[it.key() > gone ? it.key() - 1 : it.key()] = it.value();
    for (auto it = levelHeights.cbegin(); it != levelHeights.cend(); ++it) {
      if (it.key() == gone) continue;
      heights[it.key() > gone ? it.key() - 1 : it.key()] = it.value();
    }
    otherLevels = moved;
    levelHeights = heights;
    const int target = std::max(1, gone - 1);
    loadLevel(otherLevels.contains(target) ? target : otherLevels.firstKey());
    updateStatus(QStringLiteral("Level removed. The levels above moved down one."));
    notify();
    update();
    return true;
  }
  // A hole that cuts every level, such as a bolt hole through the floor and the tube above it.
  bool selectionIsThrough() const { return selectionKind() == 2 && contours[selectedContour].hole && contours[selectedContour].through; }
  void toggleSelectionThrough() {
    if (selectionKind() != 2 || !contours[selectedContour].hole) {
      updateStatus(QStringLiteral("Select a hole first."));
      return;
    }
    pushUndo();
    const bool through = !contours[selectedContour].through;
    for (int i : selectedShapes())
      if (contours[i].hole) contours[i].through = through;
    updateStatus(through ? QStringLiteral("This hole now cuts every layer on every level.")
                         : QStringLiteral("This hole now only cuts the shapes on its own layer and level."));
    notify();
    update();
  }
  // Copies the selected outline to the level above (made when there is none yet), on the same spot.
  void copySelectionToNextLevel() {
    if (selectionKind() != 2) {
      updateStatus(QStringLiteral("Click inside a shape or hole to select it first."));
      return;
    }
    pushUndo();
    QVector<Shape2DContour> copies;
    for (int i : selectedShapes()) copies.push_back(contours[i]);
    const int target = currentLevel + 1;
    switchLevel(target);
    if (!contours.isEmpty() && contours.back().nodes.isEmpty()) contours.removeLast();
    for (Shape2DContour& copy : copies) {
      copy.through = false;
      copy.layer = copy.cut = currentLayer;
      copy.level = target;
    }
    addAndSelect(copies);
    updateStatus(QStringLiteral("Copied to level %1, on the same spot.").arg(target));
    notify();
    update();
  }

  // --- whole drawing --------------------------------------------------------------------------------
  void setContours(const QVector<Shape2DContour>& value, const QMap<int, double>& heights = {}) {
    otherLevels.clear();
    levelHeights = heights;
    QList<int> levels;
    for (const Shape2DContour& contour : value)
      if (!levels.contains(std::max(1, contour.level))) levels.push_back(std::max(1, contour.level));
    std::sort(levels.begin(), levels.end());
    for (int id : heights.keys())
      if (!levels.contains(id)) levels.push_back(id);  // an empty level that was saved with a thickness
    std::sort(levels.begin(), levels.end());
    if (levels.isEmpty()) levels.push_back(1);
    for (int id : levels) {
      LevelState state;
      for (const Shape2DContour& contour : value)
        if (std::max(1, contour.level) == id) state.contours.push_back(contour);
      if (state.contours.isEmpty()) state.contours.push_back(Shape2DContour{});
      state.openLayers = {1};
      for (const Shape2DContour& contour : state.contours)
        if (contour.layer > 0 && !state.openLayers.contains(contour.layer)) state.openLayers.push_back(contour.layer);
      state.currentLayer = state.openLayers.back();
      otherLevels[id] = state;
    }
    currentLevel = levels.front();
    const LevelState first = otherLevels.take(currentLevel);
    contours = first.contours;
    activeContour = 0;
    active = -1;
    selectedContour = -1;
    also.clear();
    draggingNode = false;
    draggingHandle = -1;
    openLayers = first.openLayers;
    currentLayer = first.currentLayer;
    undoStack.clear();
    redoStack.clear();
    dirty = false;
    frameAll();
    notify();
    update();
  }
  // Every level's contours, each tagged with its level.
  QVector<Shape2DContour> shapeContours() const {
    QVector<Shape2DContour> out;
    for (int id : levelIds()) {
      for (Shape2DContour contour : id == currentLevel ? contours : otherLevels.value(id).contours) {
        contour.level = id;
        if (!contour.hole) contour.through = false;
        out.push_back(contour);
      }
    }
    return out;
  }
  bool outerClosed() const {
    for (const Shape2DContour& contour : shapeContours())
      if (!contour.hole && contour.closed && contour.nodes.size() >= 3) return true;
    return false;
  }
  bool hasOpenHole() const {
    for (const Shape2DContour& contour : shapeContours())
      if (!contour.nodes.isEmpty() && (!contour.closed || contour.nodes.size() < 3)) return true;
    return false;
  }
  int holeCount() const {
    int count = 0;
    for (const Shape2DContour& contour : contours)
      if (contour.hole && contour.closed && contour.nodes.size() >= 3) ++count;
    return count;
  }
  int shapeCount() const {
    int count = 0;
    for (const Shape2DContour& contour : contours)
      if (!contour.hole && contour.closed && contour.nodes.size() >= 3) ++count;
    return count;
  }
  // Holes with no shape in their own group: they cut nothing.
  int idleHoles() const {
    int count = 0;
    const QVector<Shape2DContour> all = shapeContours();
    for (const Shape2DContour& hole : all) {
      if (!hole.hole || !hole.closed || hole.nodes.size() < 3) continue;
      bool partner = false;
      for (const Shape2DContour& solid : all)
        if (!solid.hole && solid.closed && solid.nodes.size() >= 3 &&
            (hole.through || (solid.level == hole.level && solid.layer == hole.layer && solid.cut == hole.cut)))
          partner = true;
      if (!partner) ++count;
    }
    return count;
  }

  void clearShape() {
    pushUndo();
    contours = {Shape2DContour{}};
    contours[0].layer = contours[0].cut = 1;
    activeContour = 0;
    active = -1;
    selectedContour = -1;
    also.clear();
    draggingNode = false;
    draggingHandle = -1;
    openLayers = {1};
    currentLayer = 1;
    otherLevels.clear();
    levelHeights.clear();
    measures.clear();
    currentLevel = 1;
    applyMode();
    notify();
    updateStatus(QStringLiteral("Cleared. Click to draw the outer shape. Undo brings it back."));
    update();
  }
  void closeShape() {
    if (contours.isEmpty()) return;
    if (activeContour < 0 || activeContour >= contours.size()) activeContour = 0;
    Shape2DContour& contour = contours[activeContour];
    if (contour.nodes.size() < 3) {
      updateStatus(QStringLiteral("A loop needs at least three nodes."));
      return;
    }
    if (!contour.closed) {
      pushUndo();
      contours[activeContour].nodes.last().outHandle = {};
      contours[activeContour].nodes.first().inHandle = {};
      contours[activeContour].closed = true;
    }
    active = -1;
    updateStatus(contours[activeContour].hole ? QStringLiteral("Hole closed. It cuts the shapes on its own layer.")
                                              : QStringLiteral("Shape closed. Click to start the next one, or press Select to move things."));
    finishMirrored(activeContour);
    notify();
    update();
  }
  // Select: clicks pick nodes and shapes to move them; nothing gets drawn.
  // Draw: clicks draw with the shape chosen in the drop-down, nodes one by one for Freehand, or
  // two clicks for a square, circle and so on. New outlines are shapes; Make hole turns one into a hole.
  bool isDrawing() const { return drawMode; }
  void setDrawMode(bool on) {
    measuring = false;
    measureHaveA = measureHaveB = measureHoverValid = false;
    drawMode = on;
    if (on) {
      deselect();
    } else {
      // Keep what was just drawn selected, so it can be nudged or sized straight away.
      draggingNode = false;
      draggingHandle = -1;
      draggingShape = false;
      pruneContours();
    }
    applyMode();
    if (on && primitive == 0)
      updateStatus(QStringLiteral("Draw: click to place nodes, click the first node to close the loop."));
    else if (!on)
      updateStatus(hasOpenHole() ? QStringLiteral("Select. One loop is still open: switch back to Draw to finish it, or Undo it.")
                                 : QStringLiteral("Select: click a node or a shape to move it. Arrows nudge, Delete removes."));
    notify();
    update();
  }

  // --- selection: one node, or one or more whole shapes --------------------------------------------
  // 0 nothing, 1 a node, 2 one or more closed shapes or holes.
  int selectionKind() const {
    if (activeContour >= 0 && activeContour < contours.size() && active >= 0 && active < contours[activeContour].nodes.size())
      return 1;
    if (selectedContour >= 0 && selectedContour < contours.size() && contours[selectedContour].nodes.size() >= 2) return 2;
    return 0;
  }
  // The selected shapes and holes: the one picked first, then the others in the order picked.
  QList<int> selectedShapes() const {
    QList<int> out;
    if (selectionKind() != 2) return out;
    out.push_back(selectedContour);
    for (int i : also)
      if (i >= 0 && i < contours.size() && !out.contains(i) && contours[i].nodes.size() >= 2) out.push_back(i);
    return out;
  }
  int selectedCount() const { return selectedShapes().size(); }
  bool hasMeasureSelected() const { return selectedMeasure >= 0 && selectedMeasure < measures.size(); }
  QString selectionName() const {
    if (hasMeasureSelected() && selectionKind() == 0) {
      const QLineF m = measures[selectedMeasure];
      return QStringLiteral("Measurement, %1 mm").arg(mm(m.length()));
    }
    switch (selectionKind()) {
      case 1:
        return QStringLiteral("Node %1 of a %2").arg(active + 1).arg(contours[activeContour].hole ? QStringLiteral("hole") : QStringLiteral("shape"));
      case 2: {
        const QList<int> ids = selectedShapes();
        if (ids.size() == 1) return contours[ids.front()].hole ? QStringLiteral("Hole") : QStringLiteral("Shape");
        int holes = 0;
        for (int i : ids) holes += contours[i].hole ? 1 : 0;
        const int shapes = ids.size() - holes;
        QStringList parts;
        if (shapes) parts << QStringLiteral("%1 shape%2").arg(shapes).arg(shapes == 1 ? QString() : QStringLiteral("s"));
        if (holes) parts << QStringLiteral("%1 hole%2").arg(holes).arg(holes == 1 ? QString() : QStringLiteral("s"));
        return parts.join(QStringLiteral(", "));
      }
      default:
        return QStringLiteral("Nothing selected");
    }
  }
  QRectF selectionBounds() const {
    QVector<Shape2DContour> picked;
    for (int i : selectedShapes()) picked.push_back(contours[i]);
    return shape2d::bounds(picked);
  }
  // Node position, or the centre of the selection's outline box.
  QPointF selectionPos() const {
    if (selectionKind() == 1) return contours[activeContour].nodes[active].pos;
    if (selectionKind() == 2) return selectionBounds().center();
    return {};
  }
  QSizeF selectionSize() const {
    if (selectionKind() == 2) return selectionBounds().size();
    return {};
  }
  void setSelectionPos(const QPointF& p) {
    if (selectionKind() == 1) {
      pushUndo(QStringLiteral("node-pos"));
      contours[activeContour].nodes[active].pos = p;
    } else if (selectionKind() == 2) {
      pushUndo(QStringLiteral("shape-pos"));
      const QPointF delta = p - selectionBounds().center();
      for (int i : selectedShapes()) moveContour(i, delta);
    } else {
      return;
    }
    notify();
    update();
  }
  // Scales the selection about its centre to the given outline size.
  void setSelectionSize(double w, double h) {
    if (selectionKind() != 2) return;
    const QRectF box = selectionBounds();
    const double sx = box.width() > 1e-9 ? std::max(w, 1e-4) / box.width() : 1.0;
    const double sy = box.height() > 1e-9 ? std::max(h, 1e-4) / box.height() : 1.0;
    if (std::abs(sx - 1) < 1e-12 && std::abs(sy - 1) < 1e-12) return;
    pushUndo(QStringLiteral("shape-size"));
    const QPointF c = box.center();
    transformSelection(QTransform::fromTranslate(-c.x(), -c.y()) * QTransform::fromScale(sx, sy) * QTransform::fromTranslate(c.x(), c.y()));
    notify();
    update();
  }
  // Turns the selection about its centre, counter-clockwise for a positive angle.
  void turnSelection(double degrees) {
    if (selectionKind() != 2) {
      updateStatus(QStringLiteral("Click inside a shape to select it first."));
      return;
    }
    pushUndo();
    const QPointF c = selectionBounds().center();
    transformSelection(turnAbout(c, degrees));
    updateStatus(QStringLiteral("Turned %1°.").arg(degrees, 0, 'g', 6));
    notify();
    update();
  }
  // Mirrors the selection in place: left-right (horizontal) or top-bottom.
  void flipSelection(bool leftRight) {
    if (selectionKind() != 2) {
      updateStatus(QStringLiteral("Click inside a shape to select it first."));
      return;
    }
    pushUndo();
    const QPointF c = selectionBounds().center();
    transformSelection(QTransform::fromTranslate(-c.x(), -c.y()) * QTransform::fromScale(leftRight ? -1 : 1, leftRight ? 1 : -1) *
                       QTransform::fromTranslate(c.x(), c.y()));
    updateStatus(leftRight ? QStringLiteral("Flipped left to right.") : QStringLiteral("Flipped top to bottom."));
    notify();
    update();
  }
  // Lines the selected shapes up: 0 left, 1 centre, 2 right, 3 top, 4 middle, 5 bottom. Against the
  // box around all of them, or against 0,0 when one shape is selected.
  void alignSelection(int how) {
    const QList<int> ids = selectedShapes();
    if (ids.isEmpty()) {
      updateStatus(QStringLiteral("Select shapes to line up first (Shift-click or drag a box around them)."));
      return;
    }
    pushUndo();
    const QRectF all = ids.size() > 1 ? selectionBounds() : QRectF(QPointF(0, 0), QSizeF(0, 0));
    for (int i : ids) {
      const QRectF box = contourBounds(contours[i]);
      QPointF delta;
      switch (how) {
        case 0: delta.setX(all.left() - box.left()); break;
        case 1: delta.setX(all.center().x() - box.center().x()); break;
        case 2: delta.setX(all.right() - box.right()); break;
        case 3: delta.setY(all.bottom() - box.bottom()); break;  // Y points up: the top is the larger Y
        case 4: delta.setY(all.center().y() - box.center().y()); break;
        default: delta.setY(all.top() - box.top()); break;
      }
      moveContour(i, delta);
    }
    static const char* names[] = {"left edges", "centres", "right edges", "tops", "middles", "bottoms"};
    updateStatus(ids.size() > 1 ? QStringLiteral("Lined up the %1.").arg(QLatin1String(names[std::clamp(how, 0, 5)]))
                                : QStringLiteral("Lined up with 0,0 (select two or more shapes to line them up with each other)."));
    notify();
    update();
  }
  // Join, Cut out or Overlap the selected shapes into one outline (with holes where it has them).
  void combineSelection(shape2d::Combine op) {
    const QList<int> ids = selectedShapes();
    if (ids.size() < 2) {
      updateStatus(QStringLiteral("Select two or more shapes first: Shift-click them, or drag a box around them."));
      return;
    }
    QVector<Shape2DContour> operands;
    for (int i : ids) operands.push_back(contours[i]);
    QVector<Shape2DContour> result = shape2d::combine(operands, op);
    if (result.isEmpty()) {
      updateStatus(op == shape2d::Combine::Overlap ? QStringLiteral("Those shapes do not overlap.")
                                                   : QStringLiteral("Nothing is left of those shapes."));
      return;
    }
    pushUndo();
    const Shape2DContour& first = contours[ids.front()];
    placeNew(result, first.layer, first.hole ? first.cut : -1);
    for (Shape2DContour& contour : result) contour.through = contour.hole && first.through;
    removeContours(ids);
    addAndSelect(result);
    updateStatus(QStringLiteral("%1 into one outline. Its curves are now fine straight steps.")
                     .arg(op == shape2d::Combine::Merge ? QStringLiteral("Joined") : op == shape2d::Combine::Cut ? QStringLiteral("Cut out")
                                                                                                         : QStringLiteral("Overlapped")));
    notify();
    update();
  }
  // Rounds the selected corner, or every sharp corner of the selected shapes.
  void roundCorners(double radius) {
    if (selectionKind() == 1) {
      Shape2DContour contour = contours[activeContour];
      double used = radius;
      if (!shape2d::roundCorner(contour, active, radius, &used)) {
        updateStatus(QStringLiteral("That node is not a corner: its sides run straight on, or it ends an open line."));
        return;
      }
      pushUndo();
      contours[activeContour] = contour;
      active = -1;
      selectOnly(activeContour);
      updateStatus(used < radius - 1e-9 ? QStringLiteral("Rounded with %1 mm, as much as fits between the next corners.").arg(mm(used))
                                        : QStringLiteral("Corner rounded."));
    } else if (selectionKind() == 2) {
      pushUndo();
      int count = 0;
      for (int i : selectedShapes()) count += shape2d::roundAllCorners(contours[i], radius);
      if (count == 0) {
        dropLastUndo();
        updateStatus(QStringLiteral("No sharp corners to round."));
        return;
      }
      updateStatus(QStringLiteral("Rounded %1 corner%2. Where a side is short the radius is made smaller to fit.")
                       .arg(count)
                       .arg(count == 1 ? QString() : QStringLiteral("s")));
    } else {
      updateStatus(QStringLiteral("Select a corner node, or a shape to round all its corners."));
      return;
    }
    notify();
    update();
  }
  // Copies of the selection in a row, a grid or around a circle (the selection itself not included).
  // mode 0 row (count, stepX, stepY), 1 grid (columns, rows, stepX, stepY), 2 circle (count,
  // centre, turn copies with the circle, total angle).
  QVector<Shape2DContour> repeatCopies(int mode, int count, int rows, const QPointF& step, const QPointF& centre, bool turnCopies,
                                       double spread) const {
    QVector<Shape2DContour> out;
    const QList<int> ids = selectedShapes();
    if (ids.isEmpty()) return out;
    auto addCopy = [&](const QTransform& t) {
      for (int i : ids) {
        Shape2DContour copy = contours[i];
        shape2d::transform(copy, t);
        out.push_back(copy);
      }
    };
    if (mode == 0) {
      for (int k = 1; k < count; ++k) addCopy(QTransform::fromTranslate(step.x() * k, step.y() * k));
    } else if (mode == 1) {
      for (int r = 0; r < std::max(1, rows); ++r)
        for (int c = 0; c < std::max(1, count); ++c)
          if (r || c) addCopy(QTransform::fromTranslate(step.x() * c, -step.y() * r));
    } else {
      const QPointF own = selectionBounds().center();
      const bool full = std::abs(spread) >= 360 - 1e-9;
      const int gaps = full ? count : std::max(1, count - 1);
      for (int k = 1; k < count; ++k) {
        const double angle = spread * k / gaps;
        if (turnCopies) {
          addCopy(turnAbout(centre, angle));
        } else {
          // Only the position goes round; the copy keeps its own way up.
          const QPointF moved = turnAbout(centre, angle).map(own);
          addCopy(QTransform::fromTranslate(moved.x() - own.x(), moved.y() - own.y()));
        }
      }
    }
    return out;
  }
  void setPreview(const QVector<Shape2DContour>& value) {
    previewContours = value;
    update();
  }
  void addRepeats(const QVector<Shape2DContour>& copies) {
    previewContours.clear();
    if (copies.isEmpty()) {
      update();
      return;
    }
    pushUndo();
    QVector<Shape2DContour> added = copies;
    addAndSelect(added, true);
    updateStatus(QStringLiteral("Added %1 cop%2.").arg(copies.size()).arg(copies.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies")));
    notify();
    update();
  }
  // Outlines read from an SVG: on this layer, each part's holes cutting only that part, selected.
  void importContours(QVector<Shape2DContour> found) {
    if (found.isEmpty()) return;
    pushUndo();
    if (drawMode) setDrawMode(false);
    placeNew(found, currentLayer, -1);
    addAndSelect(found);
    frameAll();
    int holes = 0;
    for (const Shape2DContour& contour : found) holes += contour.hole ? 1 : 0;
    updateStatus(QStringLiteral("Imported %1 outline%2%3. They are selected: type a width on the right to size them.")
                     .arg(found.size())
                     .arg(found.size() == 1 ? QString() : QStringLiteral("s"))
                     .arg(holes ? QStringLiteral(" (%1 hole%2)").arg(holes).arg(holes == 1 ? QString() : QStringLiteral("s")) : QString()));
    notify();
    update();
  }
  void selectAllOnLayer() {
    QList<int> ids;
    for (int i = 0; i < contours.size(); ++i)
      if (contours[i].layer == currentLayer && contours[i].closed && contours[i].nodes.size() >= 3) ids.push_back(i);
    selectShapes(ids);
    updateStatus(ids.isEmpty() ? QStringLiteral("Nothing on this layer.") : QStringLiteral("Selected everything on layer %1.").arg(currentLayer));
    notify();
    update();
  }
  bool selectionIsHole() const { return selectionKind() == 2 && contours[selectedContour].hole; }
  // Turns the selected shapes into holes, or back.
  void toggleSelectionHole() {
    if (selectionKind() != 2) {
      updateStatus(QStringLiteral("Click inside a shape or hole to select it first."));
      return;
    }
    pushUndo();
    const bool hole = !contours[selectedContour].hole;
    for (int i : selectedShapes()) {
      contours[i].hole = hole;
      if (!hole) contours[i].through = false;
    }
    updateStatus(hole ? QStringLiteral("Now a hole. It cuts the shapes on layer %1; move a shape that should stay inside it to another layer.").arg(contours[selectedContour].layer)
                      : QStringLiteral("Now a shape."));
    notify();
    update();
  }
  // Puts the selected shapes on a layer of their own, so holes on their old layer no longer cut them.
  void moveSelectionToNewLayer() {
    if (selectionKind() != 2) {
      updateStatus(QStringLiteral("Click inside a shape or hole to select it first."));
      return;
    }
    pushUndo();
    const int next = freshCut();
    for (int i : selectedShapes()) {
      contours[i].layer = next;
      contours[i].cut = next;
    }
    openLayers.push_back(next);
    currentLayer = next;
    activeContour = selectedContour;
    active = -1;
    updateStatus(QStringLiteral("Moved to layer %1. Holes on other layers no longer cut it.").arg(next));
    notify();
    update();
  }
  void duplicateSelection() {
    if (selectionKind() != 2) {
      updateStatus(QStringLiteral("Click inside a shape to select it, then duplicate."));
      return;
    }
    pushUndo();
    const double step = std::max(1.0, gridStep * 10.0);
    QVector<Shape2DContour> copies;
    for (int i : selectedShapes()) {
      Shape2DContour copy = contours[i];
      for (Shape2DNode& node : copy.nodes) node.pos += QPointF(step, 0);
      copy.layer = currentLayer;
      if (copy.cut == contours[i].layer) copy.cut = currentLayer;
      copies.push_back(copy);
    }
    addAndSelect(copies, true);
    updateStatus(QStringLiteral("Duplicated %1 mm to the right.").arg(step, 0, 'g', 4));
    notify();
    update();
  }
  void deleteSelection() {
    if (selectionKind() == 1) {
      removeNode(activeContour, active);
      return;
    }
    if (selectionKind() == 0 && hasMeasureSelected()) {
      pushUndo();
      measures.removeAt(selectedMeasure);
      selectedMeasure = -1;
      updateStatus(QStringLiteral("Measurement deleted. Undo brings it back."));
      notify();
      update();
      return;
    }
    if (selectionKind() != 2) return;
    pushUndo();
    const int count = selectedCount();
    removeContours(selectedShapes());
    deselect();
    activeContour = 0;
    pruneContours();
    updateStatus(count == 1 ? QStringLiteral("Shape deleted. Undo brings it back.")
                            : QStringLiteral("%1 shapes deleted. Undo brings them back.").arg(count));
    notify();
    update();
  }
  // Measurements, kept with the shape as a visual aid (not part of the outline).
  QVector<QLineF> measureLines() const { return measures; }
  void setMeasureLines(const QVector<QLineF>& value) {
    measures = value;
    selectedMeasure = -1;
    update();
  }

 protected:
  void showEvent(QShowEvent* event) override {
    QWidget::showEvent(event);
    if (!framed) {
      framed = true;
      frameAll();
    }
  }
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#0f1318"));
    painter.setRenderHint(QPainter::Antialiasing);
    const QTransform toScreen = screenTransform();
    const QPointF origin = worldToScreen(QPointF(0, 0));
    const double gridPx = gridStep * zoom;
    int skip = 1;
    if (gridPx > 0.01 && gridPx < 5.0) skip = std::max(1, static_cast<int>(std::ceil(5.0 / gridPx)));
    const double visibleX = width() / (2.0 * zoom);
    const double visibleY = height() / (2.0 * zoom);
    const long long ix0 = static_cast<long long>(std::floor((pan.x() - visibleX) / gridStep));
    const long long ix1 = static_cast<long long>(std::ceil((pan.x() + visibleX) / gridStep));
    const long long iy0 = static_cast<long long>(std::floor((pan.y() - visibleY) / gridStep));
    const long long iy1 = static_cast<long long>(std::ceil((pan.y() + visibleY) / gridStep));
    const QPen minor(QColor("#222a33"), 1);
    const QPen major(QColor("#34404d"), 1);
    const long long majorEvery = static_cast<long long>(skip) * 10;
    for (long long i = ix0 - ((ix0 % skip) + skip) % skip; i <= ix1; i += skip) {
      const double x = static_cast<double>(i) * gridStep;
      painter.setPen(i % majorEvery == 0 ? major : minor);
      painter.drawLine(QPointF(origin.x() + x * zoom, 0), QPointF(origin.x() + x * zoom, height()));
    }
    for (long long i = iy0 - ((iy0 % skip) + skip) % skip; i <= iy1; i += skip) {
      const double y = static_cast<double>(i) * gridStep;
      painter.setPen(i % majorEvery == 0 ? major : minor);
      painter.drawLine(QPointF(0, origin.y() - y * zoom), QPointF(width(), origin.y() - y * zoom));
    }
    painter.setPen(QPen(QColor("#56616e"), 1.5));
    painter.drawLine(QPointF(origin.x(), 0), QPointF(origin.x(), height()));
    painter.drawLine(QPointF(0, origin.y()), QPointF(width(), origin.y()));

    // The level below, faded, with the through holes cut out of it as they will be in 3D.
    const QVector<Shape2DContour> elsewhereThrough = throughFromOtherLevels();
    if (currentLevel > 1) {
      const QVector<Shape2DContour> guides = guideContours();
      QVector<Shape2DContour> cutters = elsewhereThrough;
      for (const Shape2DContour& contour : contours)
        if (contour.hole && contour.through && contour.closed && contour.nodes.size() >= 3) cutters.push_back(contour);
      const QPainterPath below = levelFill(guides, cutters);
      if (!below.isEmpty()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(127, 167, 201, 34));
        painter.drawPath(toScreen.map(below));
      }
      for (const Shape2DContour& contour : guides) {
        painter.setPen(QPen(QColor(127, 167, 201, 150), 1.1, contour.hole ? Qt::DashLine : Qt::SolidLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(toScreen.map(contourWorldPath(contour)));
        drawCross(painter, worldToScreen(contourBounds(contour).center()), 3.5, QColor(127, 167, 201, 150));
      }
    }
    // Through holes from other levels cut this one too.
    for (const Shape2DContour& contour : elsewhereThrough) {
      painter.setPen(QPen(QColor(255, 224, 138, 170), 1.2, Qt::DashDotLine));
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(toScreen.map(contourWorldPath(contour)));
    }

    // Fills: each layer, and inside it each cut group, is its shapes minus its own holes. A hole is
    // never painted over other groups, because in 3D it does not cut them either. A through hole
    // cuts every group.
    QList<int> layers;
    for (const Shape2DContour& contour : contours)
      if (!layers.contains(contour.layer)) layers.push_back(contour.layer);
    std::sort(layers.begin(), layers.end());
    for (int layer : layers) {
      QList<int> cuts;
      for (const Shape2DContour& contour : contours) {
        if (contour.layer != layer) continue;
        const int cut = contour.cut > 0 ? contour.cut : layer;
        if (!cuts.contains(cut)) cuts.push_back(cut);
      }
      std::sort(cuts.begin(), cuts.end());
      for (int cut : cuts) {
        QPainterPath solids;
        QPainterPath cutouts;
        solids.setFillRule(Qt::WindingFill);
        cutouts.setFillRule(Qt::WindingFill);
        for (const Shape2DContour& contour : contours) {
          const int contourCut = contour.cut > 0 ? contour.cut : contour.layer;
          if (contour.layer != layer || contourCut != cut || !contour.closed || contour.nodes.size() < 3) continue;
          // Unite outline by outline. Adding them into one path would let the drawing direction
          // decide the fill: two shapes drawn opposite ways would cancel and look like a hole,
          // while the 3D side (rightly) adds them together.
          QPainterPath& target = contour.hole ? cutouts : solids;
          target = target.isEmpty() ? contourFillPath(contour) : target.united(contourFillPath(contour));
        }
        if (solids.isEmpty()) continue;
        for (const Shape2DContour& contour : contours) {
          const int contourCut = contour.cut > 0 ? contour.cut : contour.layer;
          if (!contour.hole || !contour.through || !contour.closed || contour.nodes.size() < 3) continue;
          if (contour.layer == layer && contourCut == cut) continue;  // already in
          cutouts = cutouts.isEmpty() ? contourFillPath(contour) : cutouts.united(contourFillPath(contour));
        }
        for (const Shape2DContour& contour : elsewhereThrough)
          cutouts = cutouts.isEmpty() ? contourFillPath(contour) : cutouts.united(contourFillPath(contour));
        QPainterPath filled = cutouts.isEmpty() ? solids : solids.subtracted(cutouts);
        const bool live = layer == currentLayer;
        QColor fill = ink;
        fill.setAlpha(live ? 70 : 32);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawPath(toScreen.map(filled));
      }
    }

    // The mirror line through 0,0.
    if (mirrorMode) {
      painter.setPen(QPen(QColor(255, 224, 138, 150), 1.2, Qt::DashDotLine));
      if (mirrorMode == 1) painter.drawLine(QPointF(origin.x(), 0), QPointF(origin.x(), height()));
      else painter.drawLine(QPointF(0, origin.y()), QPointF(width(), origin.y()));
    }

    if (stampWait && stampHaveAnchor && stampHoverValid) {
      const QVector<Shape2DContour> preview = stampedShapes(primitive, drawFrom, stampAnchor, stampHover, stampLock, stampOptions);
      if (!preview.isEmpty()) {
        QColor fill = ink;
        fill.setAlpha(stampHole ? 0 : 40);
        painter.setPen(QPen(QColor("#ffe08a"), 1.6, Qt::DashLine));
        painter.setBrush(fill);
        painter.drawPath(toScreen.map(previewPath(preview)));
        if (mirrorMode) {
          QVector<Shape2DContour> other;
          for (const Shape2DContour& contour : preview) other.push_back(mirrored(contour, mirrorMode));
          painter.setPen(QPen(QColor(255, 224, 138, 140), 1.2, Qt::DashLine));
          painter.drawPath(toScreen.map(previewPath(other)));
        }
        // Show what the two clicks mean.
        painter.setPen(QPen(QColor("#ffe08a"), 1, Qt::DotLine));
        painter.drawLine(worldToScreen(stampAnchor), worldToScreen(stampHover));
        const QRectF box = shape2d::bounds(preview);
        painter.setPen(QColor("#ffe08a"));
        painter.drawText(worldToScreen(QPointF(box.right(), box.top())) + QPointF(8, -6),
                         QStringLiteral("%1 × %2 mm").arg(mm(box.width()), mm(box.height())));
      }
    } else if (stampWait && primitive == 13 && stampOptions.text.trimmed().isEmpty()) {
      painter.setPen(QColor("#ffe08a"));
      painter.drawText(12, 76, QStringLiteral("Type the text in the box above, then click two corners."));
    }

    // Copies a Repeat would add.
    if (!previewContours.isEmpty()) {
      QColor fill = ink;
      fill.setAlpha(36);
      painter.setPen(QPen(QColor("#ffe08a"), 1.2, Qt::DashLine));
      painter.setBrush(fill);
      painter.drawPath(toScreen.map(previewPath(previewContours)));
    }

    for (int c = 0; c < contours.size(); ++c) {
      if (contours[c].nodes.size() < 2) continue;
      const bool live = contours[c].layer == currentLayer;
      const bool hole = contours[c].hole;
      QColor line = ink;
      if (!live) line.setAlpha(contours[c].layer < currentLayer ? 110 : 50);
      QPen pen(line, live ? (hole ? 1.6 : 2.0) : 1.2);
      if (hole) pen.setStyle(contours[c].through ? Qt::DashDotLine : Qt::DashLine);
      painter.setPen(pen);
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(toScreen.map(contourWorldPath(contours[c])));
    }

    // A loop being drawn, mirrored as it will be added.
    if (mirrorMode && drawMode && primitive == 0) {
      for (const Shape2DContour& contour : contours) {
        if (contour.closed || contour.nodes.size() < 2 || contour.layer != currentLayer) continue;
        painter.setPen(QPen(QColor(255, 224, 138, 150), 1.4, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(toScreen.map(contourWorldPath(mirrored(contour, mirrorMode))));
      }
    }

    if (selectionKind() == 2) {
      const QList<int> ids = selectedShapes();
      painter.setBrush(Qt::NoBrush);
      for (int i : ids) {
        painter.setPen(QPen(QColor("#fff2b8"), i == selectedContour && ids.size() > 1 ? 3.2 : 2.6));
        painter.drawPath(toScreen.map(contourWorldPath(contours[i])));
      }
      const QRectF box = selectionBounds();
      const QRectF onScreen = QRectF(worldToScreen(box.topLeft()), worldToScreen(box.bottomRight())).normalized();
      painter.setPen(QPen(QColor("#fff2b8"), 1, Qt::DashLine));
      painter.drawRect(onScreen);
      drawCross(painter, worldToScreen(box.center()), 6, QColor("#fff2b8"));
      if (!drawMode && !measuring) {
        // The turn handle above the box.
        const QPointF knob = rotateKnob();
        painter.setPen(QPen(QColor("#fff2b8"), 1));
        painter.drawLine(QPointF(onScreen.center().x(), onScreen.top()), knob);
        painter.setBrush(rotating ? QColor("#e87400") : QColor("#2b333d"));
        painter.setPen(QPen(QColor("#fff2b8"), 1.6));
        painter.drawEllipse(knob, 6.5, 6.5);
        painter.setBrush(Qt::NoBrush);
      }
    }

    if (boxSelecting) {
      const QRectF band = QRectF(worldToScreen(boxStart), worldToScreen(boxEnd)).normalized();
      painter.setPen(QPen(QColor("#fff2b8"), 1, Qt::DashLine));
      painter.setBrush(QColor(255, 242, 184, 24));
      painter.drawRect(band);
      painter.setBrush(Qt::NoBrush);
    }

    if (selectionKind() == 1) {
      const Shape2DContour& contour = contours[activeContour];
      const QPointF anchor = worldToScreen(contour.nodes[active].pos);
      const QPointF outVec = zeroHandle(contour.nodes[active].outHandle) ? defaultTangent(contour.nodes, active, true, contour.closed)
                                                                         : contour.nodes[active].outHandle;
      const QPointF inVec = zeroHandle(contour.nodes[active].inHandle) ? defaultTangent(contour.nodes, active, false, contour.closed)
                                                                       : contour.nodes[active].inHandle;
      const QPointF out = worldToScreen(contour.nodes[active].pos + outVec);
      const QPointF in = worldToScreen(contour.nodes[active].pos + inVec);
      painter.setPen(QPen(QColor("#73808c"), 1, Qt::DashLine));
      painter.drawLine(anchor, out);
      painter.drawLine(anchor, in);
      painter.setPen(QPen(QColor("#d2d8de"), 1.2));
      painter.setBrush(QColor("#2b333d"));
      painter.drawEllipse(out, 5.0, 5.0);
      painter.drawEllipse(in, 5.0, 5.0);
    }

    for (int c = 0; c < contours.size(); ++c) {
      const Shape2DContour& contour = contours[c];
      if (contour.layer != currentLayer) continue;
      for (int i = 0; i < contour.nodes.size(); ++i) {
        const QPointF p = worldToScreen(contour.nodes[i].pos);
        const bool hot = c == activeContour && i == active;
        painter.setPen(QPen(hot ? QColor("#fff2b8") : QColor("#e87400"), 1.5));
        painter.setBrush(hot ? QColor("#fff2b8") : QColor("#1b2028"));
        if (!contour.hole) painter.drawEllipse(p, 5.5, 5.5);
        else painter.drawRect(QRectF(p.x() - 4.5, p.y() - 4.5, 9, 9));
        if (i == 0 && !contour.closed && contour.nodes.size() >= 3 && c == activeContour) {
          painter.setPen(QPen(QColor("#f4f4f4"), 1));
          painter.setBrush(Qt::NoBrush);
          painter.drawEllipse(p, 9, 9);
        }
      }
    }

    paintMeasure(painter);

    painter.setPen(QColor("#99a5b3"));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 9));
    const QString hint = measuring ? QStringLiteral("Measure: click two points; each measurement stays. Right-click one to delete it. Esc clears the first point.")
                         : !drawMode ? QStringLiteral("Select: click a node or a shape; Shift-click or drag a box for more. Drag the round handle to turn. Delete removes.")
                         : primitive == 0 ? QStringLiteral("Draw: click to place nodes, click the first node to close. Esc switches to Select.")
                                          : QStringLiteral("Draw: two clicks place the shape (Shift keeps it square or level). Esc switches to Select.");
    painter.drawText(12, 22, hint);
    painter.drawText(12, 40, snapBends
                                 ? QStringLiteral("Curve handles snap to the grid and to nodes. Hold Shift for a free move.")
                                 : QStringLiteral("Curve handles move freely. Turn Snap bends on to land exactly on a node."));
    painter.drawText(12, 58, QStringLiteral("Middle-drag or Alt-drag pans · wheel zooms toward the cursor · Ctrl+Z / Ctrl+Y undo and redo"));
    painter.drawText(12, height() - 12, QStringLiteral("Grid %1 mm · zoom %2× · level %3 · layer %4 · %5 shape%6 · %7 hole%8 · cursor %9, %10")
                                             .arg(gridStep, 0, 'g', 4)
                                             .arg(zoom / 5.0, 0, 'f', 1)
                                             .arg(currentLevel)
                                             .arg(currentLayer)
                                             .arg(shapeCount())
                                             .arg(shapeCount() == 1 ? QString() : QStringLiteral("s"))
                                             .arg(holeCount())
                                             .arg(holeCount() == 1 ? QString() : QStringLiteral("s"))
                                             .arg(mm(cursorWorld.x()), mm(cursorWorld.y())));
  }

  void mousePressEvent(QMouseEvent* event) override {
    setFocus();
    const QPointF world = screenToWorld(event->position());
    if (event->button() == Qt::RightButton) {
      if (event->modifiers().testFlag(Qt::AltModifier)) {
        panning = true;
        panLast = event->position();
        return;
      }
      if (measuring) {
        const int hitMeasure = measureAt(event->position());
        if (measureHaveA && !measureHaveB) {
          measureHaveA = measureHaveB = false;
          updateStatus(QStringLiteral("First point cleared. Click a new one."));
        } else if (hitMeasure >= 0) {
          pushUndo();
          measures.removeAt(hitMeasure);
          selectedMeasure = -1;
          updateStatus(QStringLiteral("Measurement deleted. Undo brings it back."));
          notify();
        }
        update();
        return;
      }
      if (stampWait && stampHaveAnchor) {
        stampHaveAnchor = false;
        stampHoverValid = false;
        updateStatus(QStringLiteral("First point cleared. Click a new one."));
        update();
        return;
      }
      const Shape2DHit hit = hitPoint(event->position());
      if (hit.node >= 0) removeNode(hit.contour, hit.node);
      return;
    }
    if (event->button() == Qt::MiddleButton ||
        (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier))) {
      panning = true;
      panLast = event->position();
      return;
    }
    if (event->button() != Qt::LeftButton) return;
    lastUndoKey.clear();
    if (measuring) {
      QString what;
      const QPointF p = measureSnap(event->position(), &what);
      if (!measureHaveA || measureHaveB) {
        measureA = p;
        measureHaveA = true;
        measureHaveB = false;
        updateStatus(QStringLiteral("From %1 (%2, %3). Click the second point.").arg(what, mm(p.x()), mm(p.y())));
      } else {
        measureB = p;
        const QPointF d = measureB - measureA;
        if (std::hypot(d.x(), d.y()) > 1e-9) {
          // It stays on the drawing, as a guide, until it is deleted.
          pushUndo();
          measures.push_back(QLineF(measureA, measureB));
          selectedMeasure = measures.size() - 1;
          notify();
        }
        measureHaveA = measureHaveB = false;
        updateStatus(QStringLiteral("Distance %1 mm · ΔX %2 · ΔY %3 mm. It stays on the drawing: click to measure again, right-click a measurement to delete it.")
                         .arg(mm(std::hypot(d.x(), d.y())), mm(d.x()), mm(d.y())));
      }
      update();
      return;
    }
    if (stampWait) {
      const QPointF point = drawSnap(event->position(), stampHaveAnchor ? 5.0 : 10.0);
      if (!stampHaveAnchor) {
        stampAnchor = point;
        stampHover = point;
        stampHaveAnchor = true;
        stampHoverValid = true;
        updateStatus(drawFrom == FromCentre     ? QStringLiteral("Centre set. Move out to the size you want and click. Shift keeps it square.")
                     : drawFrom == FromDiameter ? QStringLiteral("First side set. Click the opposite side. Shift keeps it level.")
                                                : QStringLiteral("Hover the opposite corner. Click to place. Shift keeps it square."));
        update();
        return;
      }
      if (std::hypot(point.x() - stampAnchor.x(), point.y() - stampAnchor.y()) < std::max(gridStep * 0.5, 1e-4)) return;
      stampLock = event->modifiers().testFlag(Qt::ShiftModifier);
      commitStamp(point);
      return;
    }
    if (contours.isEmpty()) contours.push_back({});
    if (activeContour < 0 || activeContour >= contours.size()) activeContour = 0;
    // The turn handle above the selection.
    if (!drawMode && selectionKind() == 2) {
      const QPointF knob = rotateKnob();
      if (std::hypot(knob.x() - event->position().x(), knob.y() - event->position().y()) < 10.0) {
        pushUndo();
        dragMoved = false;
        rotating = true;
        rotateCentre = selectionBounds().center();
        rotateStart = std::atan2(world.y() - rotateCentre.y(), world.x() - rotateCentre.x());
        rememberDragStart();
        updateStatus(QStringLiteral("Turning: steps of 1°, Shift for 15°."));
        update();
        return;
      }
    }
    if (selectionKind() == 1) {
      const int handle = hitHandle(event->position());
      if (handle >= 0) {
        pushUndo();
        dragMoved = false;
        draggingHandle = handle;
        draggingNode = false;
        updateStatus(QStringLiteral("Dragging %1 handle of node %2")
                         .arg(handle == 1 ? QStringLiteral("out") : QStringLiteral("in"))
                         .arg(active + 1));
        return;
      }
    }
    const Shape2DHit hit = hitPoint(event->position());
    {
      Shape2DContour& current = contours[activeContour];
      if (hit.contour == activeContour && hit.node == 0 && !current.closed && current.nodes.size() >= 3) {
        pushUndo();
        current.nodes.last().outHandle = {};
        current.nodes.first().inHandle = {};
        current.closed = true;
        active = 0;
        draggingNode = false;
        updateStatus(current.hole ? QStringLiteral("Hole closed. It cuts the shapes on its own layer.")
                                  : QStringLiteral("Shape closed. Click to start the next one, or press Select to move things."));
        finishMirrored(activeContour);
        notify();
        update();
        return;
      }
    }
    if (hit.node >= 0) {
      pushUndo();
      dragMoved = false;
      activeContour = hit.contour;
      active = hit.node;
      selectedContour = hit.contour;
      also.clear();
      selectedMeasure = -1;
      draggingNode = true;
      draggingHandle = -1;
      const Shape2DNode& node = contours[activeContour].nodes[active];
      updateStatus(QStringLiteral("%1 node %2 at %3, %4 mm")
                       .arg(contours[activeContour].hole ? QStringLiteral("Hole") : QStringLiteral("Shape"))
                       .arg(hit.node + 1)
                       .arg(mm(node.pos.x()), mm(node.pos.y())));
      notify();
      update();
      return;
    }
    if (!drawMode) {
      const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
      const int hitMeasure = measureAt(event->position());
      if (hitMeasure >= 0 && !shift) {
        deselect();
        selectedMeasure = hitMeasure;
        updateStatus(QStringLiteral("Measurement selected. Delete removes it."));
        notify();
        update();
        return;
      }
      const int picked = contourAt(world);
      if (picked >= 0) {
        QList<int> ids = selectedShapes();
        if (shift) {
          // Shift-click adds a shape to the selection, or takes it out again.
          if (ids.contains(picked)) ids.removeAll(picked);
          else ids.push_back(picked);
          selectShapes(ids);
          updateStatus(ids.isEmpty() ? QStringLiteral("Nothing selected.") : QStringLiteral("%1 selected.").arg(selectionName()));
          notify();
          update();
          return;
        }
        // A click on a shape that is already selected keeps the others with it, so they move together;
        // if it turns out to be a click without a drag, only that shape stays selected.
        clickedInGroup = ids.contains(picked) && ids.size() > 1 ? picked : -1;
        if (!ids.contains(picked)) selectOnly(picked);
        pushUndo();
        dragMoved = false;
        draggingShape = true;
        shapeDragStart = world;
        rememberDragStart();
        updateStatus(QStringLiteral("%1 selected. Drag to move, or type the position and size on the right.").arg(selectionName()));
        notify();
        update();
        return;
      }
      // Empty space: drag a box to select what is inside it.
      if (!shift) deselect();
      boxSelecting = true;
      boxAdd = shift;
      boxStart = boxEnd = world;
      notify();
      update();
      return;
    }
    // Draw, freehand: add a node to the loop being drawn on this layer, or start a new one.
    pushUndo();
    int drawIndex = -1;
    for (int i = contours.size() - 1; i >= 0; --i) {
      if (contours[i].layer == currentLayer && !contours[i].closed) {
        drawIndex = i;
        break;
      }
    }
    if (drawIndex < 0) {
      Shape2DContour fresh;
      fresh.layer = currentLayer;
      fresh.cut = currentLayer;
      contours.push_back(fresh);
      drawIndex = contours.size() - 1;
    }
    selectedContour = -1;
    also.clear();
    activeContour = drawIndex;
    Shape2DContour& drawing = contours[drawIndex];
    drawing.hole = false;
    if (!drawing.nodes.isEmpty()) drawing.nodes.last().outHandle = {};
    drawing.nodes.push_back(Shape2DNode{drawSnap(event->position()), {}, {}});
    active = drawing.nodes.size() - 1;
    updateStatus(QStringLiteral("%1 node %2 at %3, %4 mm")
                     .arg(drawing.hole ? QStringLiteral("Hole") : QStringLiteral("Shape"))
                     .arg(drawing.nodes.size())
                     .arg(mm(drawing.nodes.back().pos.x()), mm(drawing.nodes.back().pos.y())));
    notify();
    update();
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    cursorWorld = snapPoint(screenToWorld(event->position()));
    if (panning) {
      const QPointF delta = event->position() - panLast;
      panLast = event->position();
      pan.setX(pan.x() - delta.x() / zoom);
      pan.setY(pan.y() + delta.y() / zoom);
      update();
      return;
    }
    if (measuring) {
      QString what;
      measureHover = measureSnap(event->position(), &what);
      measureHoverValid = true;
      if (measureHaveA && !measureHaveB) {
        const QPointF d = measureHover - measureA;
        updateStatus(QStringLiteral("Distance %1 mm · ΔX %2 · ΔY %3 mm (to %4)")
                         .arg(mm(std::hypot(d.x(), d.y())), mm(d.x()), mm(d.y()), what));
      }
      update();
      return;
    }
    if (stampWait && stampHaveAnchor) {
      stampHover = drawSnap(event->position(), 5.0);
      stampLock = event->modifiers().testFlag(Qt::ShiftModifier);
      stampHoverValid = true;
      updateStatus(QStringLiteral("Preview to %1, %2 mm. Click to place.").arg(mm(stampHover.x()), mm(stampHover.y())));
      update();
      return;
    }
    if (boxSelecting) {
      boxEnd = screenToWorld(event->position());
      update();
      return;
    }
    if (rotating) {
      const QPointF at = screenToWorld(event->position());
      double degrees = (std::atan2(at.y() - rotateCentre.y(), at.x() - rotateCentre.x()) - rotateStart) * 180 / kPi2D;
      const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 15.0 : 1.0;
      degrees = std::round(degrees / step) * step;
      while (degrees > 180) degrees -= 360;
      while (degrees <= -180) degrees += 360;
      const QTransform turn = turnAbout(rotateCentre, degrees);
      for (auto it = dragOriginals.cbegin(); it != dragOriginals.cend(); ++it) {
        if (it.key() >= contours.size()) continue;
        contours[it.key()].nodes = it.value();
        shape2d::transform(contours[it.key()], turn);
      }
      if (std::abs(degrees) > 1e-9) dragMoved = true;
      updateStatus(QStringLiteral("Turned %1°. Shift turns in steps of 15°.").arg(degrees, 0, 'g', 6));
      if (selectionHook) selectionHook();
      update();
      return;
    }
    if (draggingShape && selectionKind() == 2) {
      QPointF delta = snapPoint(screenToWorld(event->position())) - snapPoint(shapeDragStart);
      if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        if (std::abs(delta.x()) >= std::abs(delta.y())) delta.setY(0);
        else delta.setX(0);
      }
      for (auto it = dragOriginals.cbegin(); it != dragOriginals.cend(); ++it) {
        if (it.key() >= contours.size()) continue;
        QVector<Shape2DNode>& nodes = contours[it.key()].nodes;
        for (int i = 0; i < nodes.size() && i < it.value().size(); ++i) nodes[i].pos = it.value()[i].pos + delta;
      }
      if (!delta.isNull()) dragMoved = true;
      updateStatus(QStringLiteral("Moved %1, %2 mm. Shift keeps it level.").arg(mm(delta.x()), mm(delta.y())));
      if (selectionHook) selectionHook();
      update();
      return;
    }
    if (activeContour < 0 || activeContour >= contours.size()) {
      update();
      return;
    }
    Shape2DContour& contour = contours[activeContour];
    if (draggingHandle >= 0 && active >= 0 && active < contour.nodes.size()) {
      dragMoved = true;
      const QPointF handle = bendHandle(contour.nodes[active].pos, screenToWorld(event->position()),
                                        event->modifiers().testFlag(Qt::ShiftModifier));
      const int count = contour.nodes.size();
      if (draggingHandle == 1) {
        const bool hasNext = contour.closed || active + 1 < count;
        if (!hasNext) {
          contour.nodes[active].outHandle = {};
          if (active > 0) {
            const QPointF incoming(-handle.x(), -handle.y());
            contour.nodes[active].inHandle = incoming;
            const int prev = active - 1;
            contour.nodes[prev].outHandle = mirrorHandle(incoming, contour.nodes[active].pos, contour.nodes[prev].pos);
          }
        } else {
          contour.nodes[active].outHandle = handle;
          const int next = (active + 1) % count;
          contour.nodes[next].inHandle = mirrorHandle(handle, contour.nodes[active].pos, contour.nodes[next].pos);
        }
      } else {
        const bool hasPrev = contour.closed || active > 0;
        contour.nodes[active].inHandle = hasPrev ? handle : QPointF();
        if (hasPrev) {
          const int prev = (active - 1 + count) % count;
          contour.nodes[prev].outHandle = mirrorHandle(handle, contour.nodes[active].pos, contour.nodes[prev].pos);
        }
      }
      updateStatus(QStringLiteral("Arc stays on this segment. The next segment stays straight until you drag its handle."));
      update();
      return;
    }
    if (draggingNode && active >= 0 && active < contour.nodes.size()) {
      const QPointF next = snapPoint(screenToWorld(event->position()));
      if (next != contour.nodes[active].pos) dragMoved = true;
      contour.nodes[active].pos = next;
      updateStatus(QStringLiteral("Node %1 at %2, %3 mm").arg(active + 1).arg(mm(next.x()), mm(next.y())));
      if (selectionHook) selectionHook();
    }
    update();
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && boxSelecting) {
      boxSelecting = false;
      const QRectF band = QRectF(boxStart, boxEnd).normalized();
      const QRectF onScreen = QRectF(worldToScreen(boxStart), worldToScreen(boxEnd)).normalized();
      if (onScreen.width() > 3 || onScreen.height() > 3) {
        // Whatever lies wholly inside the box.
        QList<int> ids = boxAdd ? selectedShapes() : QList<int>{};
        for (int i = 0; i < contours.size(); ++i) {
          const Shape2DContour& contour = contours[i];
          if (contour.layer != currentLayer || !contour.closed || contour.nodes.size() < 3 || ids.contains(i)) continue;
          if (band.contains(contourBounds(contour))) ids.push_back(i);
        }
        selectShapes(ids);
        updateStatus(ids.isEmpty() ? QStringLiteral("Nothing wholly inside that box.") : QStringLiteral("%1 selected.").arg(selectionName()));
      }
      notify();
      update();
      return;
    }
    if (event->button() == Qt::LeftButton) {
      const bool wasDragging = draggingNode || draggingHandle >= 0 || draggingShape || rotating;
      if (draggingShape && !dragMoved && clickedInGroup >= 0) selectOnly(clickedInGroup);
      clickedInGroup = -1;
      draggingNode = false;
      draggingHandle = -1;
      draggingShape = false;
      rotating = false;
      // A click that moved nothing leaves no undo step behind.
      if (wasDragging && !dragMoved) dropLastUndo();
      if (wasDragging) notify();
    }
    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton ||
        (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier))) {
      panning = false;
    }
  }

  void wheelEvent(QWheelEvent* event) override {
    const double factor = event->angleDelta().y() > 0 ? 1.22 : 1.0 / 1.22;
    const double next = std::clamp(zoom * factor, 0.25, 4000.0);
    if (next != zoom) {
      const QPointF before = screenToWorld(event->position());
      zoom = next;
      const QPointF after = screenToWorld(event->position());
      pan += before - after;
    }
    update();
    event->accept();
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (event->key() == Qt::Key_Escape) {
      if (measuring && measureHaveA) {
        measureHaveA = measureHaveB = false;
        updateStatus(QStringLiteral("Measurement cleared."));
      } else if (stampWait && stampHaveAnchor) {
        stampHaveAnchor = false;
        stampHoverValid = false;
        updateStatus(QStringLiteral("First point cleared."));
      } else if (drawMode) {
        setDrawMode(false);
      } else {
        deselect();
        notify();
      }
      update();
      event->accept();
      return;
    }
    if (event->matches(QKeySequence::Undo)) {
      undo();
      event->accept();
      return;
    }
    if (event->matches(QKeySequence::Redo) ||
        (event->key() == Qt::Key_Y && event->modifiers().testFlag(Qt::ControlModifier))) {
      redo();
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_D && event->modifiers().testFlag(Qt::ControlModifier)) {
      duplicateSelection();
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_A && event->modifiers().testFlag(Qt::ControlModifier)) {
      if (drawMode) setDrawMode(false);
      selectAllOnLayer();
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
      deleteSelection();
      event->accept();
      return;
    }
    QPointF step;
    const double amount = gridStep * (event->modifiers().testFlag(Qt::ShiftModifier) ? 10.0 : 1.0);
    switch (event->key()) {
      case Qt::Key_Left: step = QPointF(-amount, 0); break;
      case Qt::Key_Right: step = QPointF(amount, 0); break;
      case Qt::Key_Up: step = QPointF(0, amount); break;
      case Qt::Key_Down: step = QPointF(0, -amount); break;
      default: break;
    }
    if (!step.isNull() && selectionKind() != 0) {
      pushUndo(QStringLiteral("nudge"));
      if (selectionKind() == 1) contours[activeContour].nodes[active].pos += step;
      else
        for (int i : selectedShapes()) moveContour(i, step);
      const QPointF at = selectionPos();
      updateStatus(QStringLiteral("Nudged to %1, %2 mm. Shift nudges 10 grid steps.").arg(mm(at.x()), mm(at.y())));
      notify();
      update();
      event->accept();
      return;
    }
    QWidget::keyPressEvent(event);
  }

 private:
  struct Shape2DHit {
    int contour = -1;
    int node = -1;
  };
  struct LevelState {
    QVector<Shape2DContour> contours{Shape2DContour{}};
    QList<int> openLayers{1};
    int currentLayer = 1;
  };
  struct Snapshot {
    QVector<Shape2DContour> contours;
    QList<int> openLayers;
    int currentLayer = 1;
    bool dirty = false;
    int currentLevel = 1;
    QMap<int, LevelState> otherLevels;
    QMap<int, double> levelHeights;
    QVector<QLineF> measures;
  };

  Snapshot snapshot() const {
    return Snapshot{contours, openLayers, currentLayer, dirty, currentLevel, otherLevels, levelHeights, measures};
  }
  void restore(const Snapshot& state) {
    contours = state.contours.isEmpty() ? QVector<Shape2DContour>{Shape2DContour{}} : state.contours;
    openLayers = state.openLayers;
    currentLayer = state.currentLayer;
    currentLevel = state.currentLevel;
    otherLevels = state.otherLevels;
    levelHeights = state.levelHeights;
    measures = state.measures;
    dirty = true;
    lastUndoKey.clear();
    deselect();
    applyMode();
    notify();
    update();
  }
  // Records the drawing before a change. Changes with the same key in a row (typing a value,
  // holding an arrow key) collapse into one step.
  void pushUndo(const QString& key = QString()) {
    if (key.isEmpty() || key != lastUndoKey) {
      undoStack.push_back(snapshot());
      if (undoStack.size() > 300) undoStack.removeFirst();
      tentativeRedo = redoStack;
      redoStack.clear();
    }
    lastUndoKey = key;
    dirty = true;
  }
  // Takes back the step a click recorded when the click turned out not to change anything.
  void dropLastUndo() {
    if (undoStack.isEmpty()) return;
    dirty = undoStack.last().dirty;
    undoStack.removeLast();
    redoStack = tentativeRedo;
  }

  // Puts the current level away and brings up another (a new, empty one when it does not exist yet).
  void switchLevel(int level) {
    deselect();
    cancelStamp();
    pruneContours();
    LevelState state;
    state.contours = contours;
    state.openLayers = openLayers;
    state.currentLayer = currentLayer;
    otherLevels[currentLevel] = state;
    loadLevel(level);
    applyMode();
  }
  void loadLevel(int level) {
    const LevelState state = otherLevels.take(level);
    currentLevel = level;
    contours = state.contours.isEmpty() ? QVector<Shape2DContour>{Shape2DContour{}} : state.contours;
    openLayers = state.openLayers.isEmpty() ? QList<int>{1} : state.openLayers;
    currentLayer = state.currentLayer;
    activeContour = 0;
    deselect();
  }
  // What a level looks like from above in 3D: each cut group's shapes minus its own holes and the
  // through holes, all groups together.
  static QPainterPath levelFill(const QVector<Shape2DContour>& level, const QVector<Shape2DContour>& extraCutters) {
    QMap<QPair<int, int>, QPair<QPainterPath, QPainterPath>> groups;
    for (const Shape2DContour& contour : level) {
      if (!contour.closed || contour.nodes.size() < 3) continue;
      auto& group = groups[qMakePair(contour.layer, contour.cut > 0 ? contour.cut : contour.layer)];
      QPainterPath& target = contour.hole ? group.second : group.first;
      target = target.isEmpty() ? contourFillPath(contour) : target.united(contourFillPath(contour));
    }
    QPainterPath through;
    for (const Shape2DContour& contour : level)
      if (contour.hole && contour.through && contour.closed && contour.nodes.size() >= 3)
        through = through.isEmpty() ? contourFillPath(contour) : through.united(contourFillPath(contour));
    for (const Shape2DContour& contour : extraCutters)
      through = through.isEmpty() ? contourFillPath(contour) : through.united(contourFillPath(contour));
    QPainterPath out;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
      if (it.value().first.isEmpty()) continue;
      QPainterPath filled = it.value().first;
      if (!it.value().second.isEmpty()) filled = filled.subtracted(it.value().second);
      if (!through.isEmpty()) filled = filled.subtracted(through);
      out = out.isEmpty() ? filled : out.united(filled);
    }
    return out;
  }
  // The level below, shown faded so the next level can be lined up with it.
  QVector<Shape2DContour> guideContours() const {
    QVector<Shape2DContour> out;
    for (const Shape2DContour& contour : otherLevels.value(currentLevel - 1).contours)
      if (contour.closed && contour.nodes.size() >= 3) out.push_back(contour);
    return out;
  }
  // Through holes drawn on other levels: they cut this level too.
  QVector<Shape2DContour> throughFromOtherLevels() const {
    QVector<Shape2DContour> out;
    for (auto it = otherLevels.cbegin(); it != otherLevels.cend(); ++it)
      for (const Shape2DContour& contour : it.value().contours)
        if (contour.hole && contour.through && contour.closed && contour.nodes.size() >= 3) out.push_back(contour);
    return out;
  }
  // Clicks that land near a node or centre of the level below (or of a through hole) snap onto it,
  // so a tube ring is drawn exactly around a bolt hole; otherwise the grid.
  // The size click of a stamped shape only snaps when right on top of a point (reach 5 px), so a
  // tube a little wider than the hole below is not pulled onto the hole's edge.
  QPointF drawSnap(const QPointF& screen, double reach = 10.0) const {
    QPointF found = snapPoint(screenToWorld(screen));
    double best = reach;
    QVector<Shape2DContour> guides = guideContours();
    guides += throughFromOtherLevels();
    for (const Shape2DContour& contour : guides) {
      QVector<QPointF> points{contourBounds(contour).center()};
      for (const Shape2DNode& node : contour.nodes) points.push_back(node.pos);
      for (const QPointF& p : points) {
        const QPointF s = worldToScreen(p);
        const double d = std::hypot(s.x() - screen.x(), s.y() - screen.y());
        if (d < best) {
          best = d;
          found = p;
        }
      }
    }
    return found;
  }

  QTransform screenTransform() const {
    QTransform t;
    t.translate(width() * 0.5, height() * 0.5);
    t.scale(zoom, -zoom);
    t.translate(-pan.x(), -pan.y());
    return t;
  }
  QPointF worldToScreen(const QPointF& p) const {
    return QPointF(width() * 0.5 + (p.x() - pan.x()) * zoom, height() * 0.5 - (p.y() - pan.y()) * zoom);
  }
  QPointF screenToWorld(const QPointF& p) const {
    return QPointF(pan.x() + (p.x() - width() * 0.5) / zoom, pan.y() + (height() * 0.5 - p.y()) / zoom);
  }
  QPointF snapPoint(const QPointF& p) const {
    return QPointF(std::round(p.x() / gridStep) * gridStep, std::round(p.y() / gridStep) * gridStep);
  }
  void frameAll() {
    QRectF all;
    for (const Shape2DContour& contour : contours)
      if (contour.nodes.size() >= 2) all = all.isNull() ? contourBounds(contour) : all.united(contourBounds(contour));
    if (all.isNull() || width() < 10 || height() < 10) return;
    pan = all.center();
    const double fit = std::min((width() - 120) / std::max(all.width(), 1.0), (height() - 160) / std::max(all.height(), 1.0));
    zoom = std::clamp(fit, 0.25, 400.0);
  }
  void drawCross(QPainter& painter, const QPointF& p, double size, const QColor& color) const {
    painter.setPen(QPen(color, 1.2));
    painter.drawLine(p - QPointF(size, 0), p + QPointF(size, 0));
    painter.drawLine(p - QPointF(0, size), p + QPointF(0, size));
  }
  // One measurement: the line, its ends, the X and Y legs when wanted, and its length.
  void paintMeasureLine(QPainter& painter, const QPointF& a, const QPointF& b, bool full, bool hot) const {
    const QColor tint = hot ? QColor("#d8f3ff") : QColor("#7fd3ff");
    const QPointF sa = worldToScreen(a);
    const QPointF sb = worldToScreen(b);
    if (full) {
      painter.setPen(QPen(tint, 1, Qt::DotLine));
      painter.drawLine(sa, QPointF(sb.x(), sa.y()));
      painter.drawLine(QPointF(sb.x(), sa.y()), sb);
    }
    painter.setPen(QPen(tint, hot ? 2.6 : 1.8));
    painter.drawLine(sa, sb);
    painter.setBrush(tint);
    painter.drawEllipse(sa, 3.5, 3.5);
    painter.drawEllipse(sb, 3.5, 3.5);
    const QPointF d = b - a;
    const QString label = full ? QStringLiteral("%1 mm   ΔX %2   ΔY %3").arg(mm(std::hypot(d.x(), d.y())), mm(d.x()), mm(d.y()))
                               : QStringLiteral("%1 mm").arg(mm(std::hypot(d.x(), d.y())));
    QFont font(QStringLiteral("Segoe UI"), full ? 10 : 9);
    font.setBold(full);
    painter.setFont(font);
    const QRectF textBox = QFontMetricsF(font).boundingRect(label).adjusted(-6, -3, 6, 3);
    const QPointF mid = (sa + sb) * 0.5 + QPointF(10, -14);
    const QRectF placed = textBox.translated(mid - textBox.bottomLeft());
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(15, 19, 24, 220));
    painter.drawRoundedRect(placed, 4, 4);
    painter.setPen(tint);
    painter.drawText(placed, Qt::AlignCenter, label);
  }
  void paintMeasure(QPainter& painter) const {
    const QColor tint("#7fd3ff");
    for (int i = 0; i < measures.size(); ++i)
      paintMeasureLine(painter, measures[i].p1(), measures[i].p2(), i == selectedMeasure, i == selectedMeasure);
    if (!measuring) return;
    for (const Shape2DContour& contour : contours) {
      if (!contour.closed || contour.nodes.size() < 3) continue;
      QColor dim = tint;
      dim.setAlpha(140);
      drawCross(painter, worldToScreen(contourBounds(contour).center()), 5, dim);
    }
    if (measureHoverValid) {
      painter.setPen(QPen(tint, 1.2));
      painter.setBrush(Qt::NoBrush);
      painter.drawEllipse(worldToScreen(measureHover), 7, 7);
    }
    if (measureHaveA && !measureHaveB) paintMeasureLine(painter, measureA, measureHoverValid ? measureHover : measureA, true, false);
  }
  // The measurement under a screen point (within 6 px of its line), or -1.
  int measureAt(const QPointF& screen) const {
    int best = -1;
    double bestDist = 6.0;
    for (int i = 0; i < measures.size(); ++i) {
      const QPointF a = worldToScreen(measures[i].p1()), b = worldToScreen(measures[i].p2());
      const QPointF ab = b - a;
      const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
      const double t = len2 > 1e-9 ? std::clamp(((screen.x() - a.x()) * ab.x() + (screen.y() - a.y()) * ab.y()) / len2, 0.0, 1.0) : 0.0;
      const QPointF nearest = a + ab * t;
      const double d = std::hypot(screen.x() - nearest.x(), screen.y() - nearest.y());
      if (d < bestDist) {
        bestDist = d;
        best = i;
      }
    }
    return best;
  }
  // Where the turn handle sits: above the middle of the selection's box, on screen.
  QPointF rotateKnob() const {
    const QRectF box = selectionBounds();
    const QRectF onScreen = QRectF(worldToScreen(box.topLeft()), worldToScreen(box.bottomRight())).normalized();
    return QPointF(onScreen.center().x(), onScreen.top() - 26);
  }
  static QPainterPath previewPath(const QVector<Shape2DContour>& set) {
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const Shape2DContour& contour : set) path.addPath(contourWorldPath(contour));
    return path;
  }
  // Nodes (any layer) and shape centres snap; otherwise the grid.
  QPointF measureSnap(const QPointF& screen, QString* what) const {
    double best = 12.0;
    QPointF found = snapPoint(screenToWorld(screen));
    if (what) *what = QStringLiteral("grid point");
    QVector<Shape2DContour> candidates = contours;
    candidates += guideContours();
    candidates += throughFromOtherLevels();
    for (const Shape2DContour& contour : candidates) {
      for (const Shape2DNode& node : contour.nodes) {
        const QPointF s = worldToScreen(node.pos);
        const double d = std::hypot(s.x() - screen.x(), s.y() - screen.y());
        if (d < best) {
          best = d;
          found = node.pos;
          if (what) *what = QStringLiteral("node");
        }
      }
      if (contour.closed && contour.nodes.size() >= 3) {
        const QPointF centre = contourBounds(contour).center();
        const QPointF s = worldToScreen(centre);
        const double d = std::hypot(s.x() - screen.x(), s.y() - screen.y());
        if (d < best) {
          best = d;
          found = centre;
          if (what) *what = contour.hole ? QStringLiteral("hole centre") : QStringLiteral("shape centre");
        }
      }
    }
    return found;
  }
  QPointF bendHandle(const QPointF& anchor, const QPointF& worldTip, bool shiftFree) const {
    if (!snapBends || shiftFree) return worldTip - anchor;
    const double reach = std::max(gridStep * 0.5, 10.0 / std::max(zoom, 0.4));
    QPointF snapped = snapPoint(worldTip);
    double best = reach;
    for (const Shape2DContour& contour : contours) {
      if (contour.layer != currentLayer) continue;
      for (const Shape2DNode& node : contour.nodes) {
        const double distance = std::hypot(node.pos.x() - worldTip.x(), node.pos.y() - worldTip.y());
        if (distance < best) {
          best = distance;
          snapped = node.pos;
        }
      }
    }
    return snapped - anchor;
  }
  QPointF mirrorHandle(const QPointF& handle, const QPointF& from, const QPointF& to) const {
    const QPointF chord = to - from;
    const double len = std::hypot(chord.x(), chord.y());
    if (len < 1e-6) return QPointF(-handle.x(), -handle.y());
    const QPointF u(chord.x() / len, chord.y() / len);
    const double along = handle.x() * u.x() + handle.y() * u.y();
    return handle - u * (2.0 * along);
  }
  Shape2DHit hitPoint(const QPointF& pos) const {
    Shape2DHit best;
    double bestDist = 11.0;
    for (int c = 0; c < contours.size(); ++c) {
      if (contours[c].layer != currentLayer) continue;
      for (int i = 0; i < contours[c].nodes.size(); ++i) {
        const QPointF screen = worldToScreen(contours[c].nodes[i].pos);
        const double d = std::hypot(screen.x() - pos.x(), screen.y() - pos.y());
        if (d < bestDist) {
          bestDist = d;
          best = {c, i};
        }
      }
    }
    return best;
  }
  // The smallest closed shape or hole on this layer under the point, so a hole inside a plate
  // is picked rather than the plate.
  int contourAt(const QPointF& world) const {
    int best = -1;
    double bestArea = 1e300;
    for (int c = 0; c < contours.size(); ++c) {
      const Shape2DContour& contour = contours[c];
      if (contour.layer != currentLayer || !contour.closed || contour.nodes.size() < 3) continue;
      QPainterPath path = contourWorldPath(contour);
      path.setFillRule(Qt::WindingFill);
      if (!path.contains(world)) continue;
      const double area = contourArea(contour);
      if (area < bestArea) {
        bestArea = area;
        best = c;
      }
    }
    return best;
  }
  int hitHandle(const QPointF& pos) const {
    if (selectionKind() != 1) return -1;
    const Shape2DContour& contour = contours[activeContour];
    const QPointF outVec = zeroHandle(contour.nodes[active].outHandle) ? defaultTangent(contour.nodes, active, true, contour.closed)
                                                                       : contour.nodes[active].outHandle;
    const QPointF inVec = zeroHandle(contour.nodes[active].inHandle) ? defaultTangent(contour.nodes, active, false, contour.closed)
                                                                     : contour.nodes[active].inHandle;
    const QPointF out = worldToScreen(contour.nodes[active].pos + outVec);
    const QPointF in = worldToScreen(contour.nodes[active].pos + inVec);
    if (std::hypot(out.x() - pos.x(), out.y() - pos.y()) < 10.0) return 1;
    if (std::hypot(in.x() - pos.x(), in.y() - pos.y()) < 10.0) return 0;
    return -1;
  }
  void moveContour(int index, const QPointF& delta) {
    if (index < 0 || index >= contours.size()) return;
    for (Shape2DNode& node : contours[index].nodes) node.pos += delta;
  }
  static QTransform turnAbout(const QPointF& c, double degrees) {
    const double a = degrees * kPi2D / 180;
    return QTransform::fromTranslate(-c.x(), -c.y()) * QTransform(std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0) *
           QTransform::fromTranslate(c.x(), c.y());
  }
  void rememberDragStart() {
    dragOriginals.clear();
    for (int i : selectedShapes()) dragOriginals[i] = contours[i].nodes;
  }
  void transformSelection(const QTransform& t) {
    for (int i : selectedShapes()) shape2d::transform(contours[i], t);
  }
  void selectOnly(int index) {
    selectShapes(index >= 0 ? QList<int>{index} : QList<int>{});
  }
  void selectShapes(const QList<int>& ids) {
    active = -1;
    selectedMeasure = -1;
    selectedContour = ids.isEmpty() ? -1 : ids.front();
    also = ids.mid(1);
    if (selectedContour >= 0) activeContour = selectedContour;
  }
  // A layer or cut number nothing uses yet, on any level.
  int freshCut() const {
    int next = 1;
    for (int id : layerIds()) next = std::max(next, id + 1);
    for (const Shape2DContour& contour : contours) next = std::max({next, contour.cut + 1, contour.layer + 1});
    for (const LevelState& state : otherLevels)
      for (const Shape2DContour& contour : state.contours) next = std::max({next, contour.cut + 1, contour.layer + 1});
    return next;
  }
  // Puts new outlines on a layer. Their cut numbers (1 and up, from the tools) become fresh ones when
  // they bring holes or islands of their own, so those holes cut only them; a lone shape cuts like
  // anything else drawn on the layer. keepCut >= 0 puts them all on that cut number.
  void placeNew(QVector<Shape2DContour>& found, int layer, int keepCut) {
    bool separate = false;
    for (const Shape2DContour& contour : found) separate = separate || contour.hole || contour.cut > 1;
    const int base = separate ? freshCut() : layer;
    for (Shape2DContour& contour : found) {
      contour.layer = layer;
      contour.level = currentLevel;
      contour.cut = keepCut >= 0 ? keepCut : separate ? base + std::max(1, contour.cut) - 1 : layer;
      contour.closed = contour.nodes.size() >= 3;
    }
  }
  // Removes these outlines, fixing the indices kept elsewhere.
  void removeContours(QList<int> ids) {
    std::sort(ids.begin(), ids.end(), std::greater<int>());
    for (int i : ids)
      if (i >= 0 && i < contours.size()) contours.removeAt(i);
    deselect();
    activeContour = 0;
  }
  void addAndSelect(const QVector<Shape2DContour>& added, bool keepLayers = false) {
    if (!contours.isEmpty() && contours.back().nodes.isEmpty()) contours.removeLast();
    QList<int> ids;
    for (Shape2DContour contour : added) {
      if (!keepLayers) contour.level = currentLevel;
      contours.push_back(contour);
      ids.push_back(contours.size() - 1);
    }
    selectShapes(ids);
  }
  void removeNode(int contourIndex, int nodeIndex) {
    if (contourIndex < 0 || contourIndex >= contours.size()) return;
    if (nodeIndex < 0 || nodeIndex >= contours[contourIndex].nodes.size()) return;
    pushUndo();
    Shape2DContour& contour = contours[contourIndex];
    contour.nodes.removeAt(nodeIndex);
    // A closed loop stays closed while it still has a shape.
    if (contour.nodes.size() < 3) contour.closed = false;
    activeContour = contourIndex;
    active = -1;
    selectedContour = contour.closed ? contourIndex : -1;
    also.clear();
    draggingNode = false;
    draggingHandle = -1;
    pruneContours();
    updateStatus(QStringLiteral("Node removed."));
    notify();
    update();
  }
  void pruneContours() {
    for (int i = contours.size() - 1; i >= 0; --i) {
      if (contours[i].nodes.isEmpty() && contours.size() > 1) {
        contours.removeAt(i);
        if (selectedContour == i) selectedContour = -1;
        else if (selectedContour > i) --selectedContour;
        also.removeAll(i);
        for (int& other : also)
          if (other > i) --other;
      }
    }
    if (contours.isEmpty()) contours.push_back({});
    if (activeContour >= contours.size()) activeContour = contours.size() - 1;
    if (activeContour < 0) activeContour = 0;
  }
  void deselect() {
    active = -1;
    selectedContour = -1;
    also.clear();
    selectedMeasure = -1;
    draggingNode = false;
    draggingHandle = -1;
    draggingShape = false;
    rotating = false;
    boxSelecting = false;
  }
  bool solidOnLayer(int layer) const {
    for (const Shape2DContour& contour : contours)
      if (!contour.hole && contour.closed && contour.nodes.size() >= 3 && contour.layer == layer) return true;
    return false;
  }
  void notify() {
    if (changeHook) changeHook();
    if (selectionHook) selectionHook();
  }
  void updateStatus(const QString& text) {
    if (statusLabel) statusLabel->setText(text);
  }
  // Stamped shapes wait for two clicks while Draw is on with a shape other than Freehand picked.
  void applyMode() {
    if (drawMode && primitive > 0 && !measuring) {
      if (!stampWait) armStamp(false);
    } else {
      cancelStamp();
    }
  }
  void cancelStamp() {
    stampWait = false;
    stampHaveAnchor = false;
    stampHoverValid = false;
  }
  void armStamp(bool hole) {
    stampWait = true;
    stampHole = hole;
    stampHaveAnchor = false;
    stampHoverValid = false;
    deselect();
    static const char* names[] = {"Freehand", "Square", "Rounded square", "Triangle", "Circle", "Polygon", "Star",
                                  "Slot", "Ring", "Arc", "Heart", "Arrow", "Swirl", "Text"};
    const QString kind = QLatin1String(names[std::clamp(primitive, 0, 13)]);
    const QString first = drawFrom == FromCentre     ? QStringLiteral("Click the centre")
                          : drawFrom == FromDiameter ? QStringLiteral("Click one side")
                                                     : QStringLiteral("Click the first corner");
    updateStatus(QStringLiteral("%1%2. %3 on layer %4.").arg(kind, hole ? QStringLiteral(" hole") : QString(), first).arg(currentLayer));
    notify();
    update();
  }
  void commitStamp(const QPointF& point) {
    QVector<Shape2DContour> made = stampedShapes(primitive, drawFrom, stampAnchor, point, stampLock, stampOptions);
    if (made.isEmpty()) {
      if (primitive == 13) updateStatus(QStringLiteral("Type the text in the box above first."));
      return;
    }
    pushUndo();
    if (stampHole)
      for (Shape2DContour& contour : made) contour.hole = true;
    placeNew(made, currentLayer, stampHole ? currentLayer : -1);
    // Mirror on: the mirrored copy too, unless the shape is its own mirror (a circle on the line).
    if (mirrorMode) {
      QVector<Shape2DContour> other;
      for (const Shape2DContour& contour : made) other.push_back(mirrored(contour, mirrorMode));
      if (!sameArea(made, other)) {
        bool separate = false;
        for (const Shape2DContour& contour : other) separate = separate || contour.cut != currentLayer;
        const int shift = separate ? freshCut() - other.front().cut : 0;
        for (Shape2DContour& contour : other) contour.cut += separate ? shift : 0;
        made += other;
      }
    }
    addAndSelect(made);
    stampHaveAnchor = false;
    stampHoverValid = false;
    const QRectF box = selectionBounds();
    updateStatus(QStringLiteral("Placed %1 × %2 mm on layer %3. Type an exact size on the right, or click to place another.")
                     .arg(mm(box.width()), mm(box.height()))
                     .arg(currentLayer));
    notify();
    update();
  }
  // A loop drawn with Mirror on was just closed: one that runs from the mirror line back to it is
  // completed into one whole shape with its mirror half; any other loop gets a mirrored copy.
  void finishMirrored(int index) {
    if (!mirrorMode || index < 0 || index >= contours.size() || !contours[index].closed) return;
    Shape2DContour& half = contours[index];
    const int n = half.nodes.size();
    auto onLine = [&](const QPointF& p) { return std::abs(mirrorMode == 1 ? p.x() : p.y()) < 1e-9; };
    auto flip = [&](const QPointF& v) { return mirrorMode == 1 ? QPointF(-v.x(), v.y()) : QPointF(v.x(), -v.y()); };
    if (n >= 3 && onLine(half.nodes.front().pos) && onLine(half.nodes.back().pos)) {
      // N0 .. Nk, then the mirror of N(k-1) .. N1 walked backwards: their handles swap and flip.
      QVector<Shape2DNode> whole = half.nodes;
      whole.front().inHandle = flip(half.nodes.front().outHandle);
      whole.back().outHandle = flip(half.nodes.back().inHandle);
      for (int i = n - 2; i >= 1; --i) {
        const Shape2DNode& node = half.nodes[i];
        whole.push_back(Shape2DNode{flip(node.pos), flip(node.outHandle), flip(node.inHandle)});
      }
      half.nodes = whole;
      updateStatus(QStringLiteral("Closed into one whole shape with its mirror half."));
      return;
    }
    Shape2DContour copy = mirrored(half, mirrorMode);
    if (sameArea({half}, {copy})) return;
    contours.push_back(copy);
    updateStatus(QStringLiteral("Closed, with its mirror on the other side."));
  }

  QVector<Shape2DContour> contours;
  QVector<Snapshot> undoStack;
  QVector<Snapshot> redoStack;
  QVector<Snapshot> tentativeRedo;
  QString lastUndoKey;
  bool framed = false;
  bool dirty = false;
  QLabel* statusLabel = nullptr;
  QPointF pan;
  QPointF panLast;
  QPointF cursorWorld;
  double gridStep = 1.0;
  double zoom = 5.0;
  int activeContour = 0;
  int active = -1;
  int selectedContour = -1;
  int draggingHandle = -1;
  bool draggingNode = false;
  bool draggingShape = false;
  bool dragMoved = false;
  QPointF shapeDragStart;
  bool panning = false;
  bool snapBends = true;
  int primitive = 0;
  int drawFrom = FromCorners;
  shape2d::StampOptions stampOptions;
  int mirrorMode = 0;
  QList<int> also;  // shapes selected besides selectedContour, in the order picked
  QMap<int, QVector<Shape2DNode>> dragOriginals;  // what a drag or turn started from
  bool rotating = false;
  QPointF rotateCentre;
  double rotateStart = 0;
  bool boxSelecting = false;
  bool boxAdd = false;
  int clickedInGroup = -1;
  QPointF boxStart;
  QPointF boxEnd;
  QVector<Shape2DContour> previewContours;
  QVector<QLineF> measures;
  int selectedMeasure = -1;
  int currentLayer = 1;
  QList<int> openLayers{1};
  int currentLevel = 1;
  QMap<int, LevelState> otherLevels;
  QMap<int, double> levelHeights;
  std::function<void()> changeHook;
  std::function<void()> selectionHook;
  bool stampWait = false;
  bool stampHole = false;
  bool stampHaveAnchor = false;
  bool stampHoverValid = false;
  bool stampLock = false;
  bool drawMode = true;
  QPointF stampAnchor;
  QPointF stampHover;
  bool measuring = false;
  bool measureHaveA = false;
  bool measureHaveB = false;
  bool measureHoverValid = false;
  QPointF measureA;
  QPointF measureB;
  QPointF measureHover;
  QColor ink = QColor("#e87400");
};


// ---------------------------------------------------------------------------------------------------

Shape2DEditorDialog::Shape2DEditorDialog(QWidget* parent, const QString& libraryRootIn, const QString& existing)
    : QDialog(parent), libraryRoot(libraryRootIn) {
  setWindowTitle(existing.isEmpty() ? QStringLiteral("2D Shape Editor") : QStringLiteral("Edit 2D Shape"));
  // A normal window, so it can be maximized (and minimized) like any other; it stays modal.
  setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint |
                 Qt::WindowCloseButtonHint);
  resize(1320, 880);
  {
    // Opens the size it was last closed at, maximized if it was.
    const QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("shape2dEditor/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
  }
  auto* root = new QVBoxLayout(this);

  // Row 1: mode, then what to draw.
  auto* drawRow = new QHBoxLayout();
  const QString modeStyle =
      QStringLiteral("QPushButton { color: #e87400; min-width: 64px; } QPushButton:checked { background: #e87400; color: #24180c; font-weight: 600; }");
  auto* selectButton = new QPushButton(QStringLiteral("Select"), this);
  selectButton->setCheckable(true);
  selectButton->setToolTip(QStringLiteral("Click nodes and shapes to move them. Nothing gets drawn. Esc also switches to Select."));
  selectButton->setStyleSheet(modeStyle);
  auto* drawButton = new QPushButton(QStringLiteral("Draw"), this);
  drawButton->setCheckable(true);
  drawButton->setToolTip(QStringLiteral("Draw with the shape picked in the list: Freehand places nodes one by one, the others take two clicks.\nNew outlines are shapes; select one and press Make hole to cut it out instead."));
  drawButton->setStyleSheet(modeStyle);
  drawRow->addWidget(selectButton);
  drawRow->addWidget(drawButton);
  drawRow->addSpacing(12);
  drawRow->addWidget(new QLabel(QStringLiteral("Grid"), this));
  auto* gridBox = new QComboBox(this);
  for (double step : {0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0})
    gridBox->addItem(QStringLiteral("%1 mm").arg(step, 0, 'g', 3), step);
  gridBox->setCurrentIndex(4);
  gridBox->setToolTip(QStringLiteral("Nodes, shapes and moves snap to this grid. 0.01 mm for fine work such as screw holes."));
  drawRow->addWidget(gridBox);
  auto* primitiveBox = new QComboBox(this);
  primitiveBox->addItem(QStringLiteral("Freehand"), 0);
  primitiveBox->addItem(QStringLiteral("Square"), 1);
  primitiveBox->addItem(QStringLiteral("Rounded square"), 2);
  primitiveBox->addItem(QStringLiteral("Triangle"), 3);
  primitiveBox->addItem(QStringLiteral("Circle"), 4);
  primitiveBox->addItem(QStringLiteral("Polygon"), 5);
  primitiveBox->addItem(QStringLiteral("Star"), 6);
  primitiveBox->addItem(QStringLiteral("Slot"), 7);
  primitiveBox->addItem(QStringLiteral("Ring"), 8);
  primitiveBox->addItem(QStringLiteral("Arc"), 9);
  primitiveBox->addItem(QStringLiteral("Heart"), 10);
  primitiveBox->addItem(QStringLiteral("Arrow"), 11);
  primitiveBox->addItem(QStringLiteral("Swirl"), 12);
  primitiveBox->addItem(QStringLiteral("Text"), 13);
  primitiveBox->setMaxVisibleItems(14);
  primitiveBox->setToolTip(QStringLiteral("What Draw draws. Freehand: click node by node. The others: two clicks on the grid."));
  drawRow->addWidget(primitiveBox);
  auto* fromBox = new QComboBox(this);
  fromBox->addItem(QStringLiteral("Corner to corner"), FromCorners);
  fromBox->addItem(QStringLiteral("From centre"), FromCentre);
  fromBox->addItem(QStringLiteral("Side to side"), FromDiameter);
  fromBox->setToolTip(QStringLiteral("Corner to corner: two opposite corners.\nFrom centre: the centre, then the size.\n"
                                     "Side to side: two opposite sides, e.g. a circle from its left edge to its right edge.\n"
                                     "Hold Shift for equal sides, or to keep side to side exactly level."));
  fromBox->hide();
  drawRow->addWidget(fromBox);
  auto* sidesSpin = new QSpinBox(this);
  sidesSpin->setRange(3, 64);
  sidesSpin->setValue(6);
  sidesSpin->setPrefix(QStringLiteral("Sides "));
  sidesSpin->setToolTip(QStringLiteral("Number of sides for the polygon. 6 is a hexagon."));
  sidesSpin->hide();
  drawRow->addWidget(sidesSpin);
  auto* cornerSpin = new QDoubleSpinBox(this);
  cornerSpin->setRange(0.0, 1000.0);
  cornerSpin->setDecimals(3);
  cornerSpin->setSingleStep(0.1);
  cornerSpin->setValue(1.0);
  cornerSpin->setPrefix(QStringLiteral("Corner "));
  cornerSpin->setSuffix(QStringLiteral(" mm"));
  cornerSpin->setToolTip(QStringLiteral("Corner radius of the rounded square."));
  cornerSpin->hide();
  drawRow->addWidget(cornerSpin);
  auto* innerSpin = new QDoubleSpinBox(this);
  innerSpin->setRange(1, 99);
  innerSpin->setDecimals(0);
  innerSpin->setSuffix(QStringLiteral(" %"));
  innerSpin->hide();
  drawRow->addWidget(innerSpin);
  auto* angleSpin = new QDoubleSpinBox(this);
  angleSpin->setRange(1, 360);
  angleSpin->setDecimals(0);
  angleSpin->setValue(180);
  angleSpin->setPrefix(QStringLiteral("Angle "));
  angleSpin->setSuffix(QStringLiteral("°"));
  angleSpin->setToolTip(QStringLiteral("How much of the ring the arc covers. 180° is half a ring, opening downward."));
  angleSpin->hide();
  drawRow->addWidget(angleSpin);
  auto* turnsSpin = new QDoubleSpinBox(this);
  turnsSpin->setRange(0.1, 5);
  turnsSpin->setDecimals(2);
  turnsSpin->setSingleStep(0.05);
  turnsSpin->setValue(0.75);
  turnsSpin->setPrefix(QStringLiteral("Turns "));
  turnsSpin->setToolTip(QStringLiteral("How far each arm winds round on its way out: 1 is a full turn."));
  turnsSpin->hide();
  drawRow->addWidget(turnsSpin);
  auto* textEdit = new QLineEdit(this);
  textEdit->setPlaceholderText(QStringLiteral("Text"));
  textEdit->setMinimumWidth(140);
  textEdit->setToolTip(QStringLiteral("The text to place. Click two corners: it fills that box, keeping its proportions."));
  textEdit->hide();
  drawRow->addWidget(textEdit);
  auto* fontBox = new QFontComboBox(this);
  fontBox->setMaximumWidth(180);
  fontBox->setToolTip(QStringLiteral("The font of the text."));
  fontBox->hide();
  drawRow->addWidget(fontBox);
  auto* boldBox = new QCheckBox(QStringLiteral("Bold"), this);
  boldBox->hide();
  drawRow->addWidget(boldBox);
  auto* mirrorBox = new QComboBox(this);
  mirrorBox->addItem(QStringLiteral("No mirror"), 0);
  mirrorBox->addItem(QStringLiteral("Mirror left–right"), 1);
  mirrorBox->addItem(QStringLiteral("Mirror top–bottom"), 2);
  mirrorBox->setToolTip(QStringLiteral("While drawing, everything also appears mirrored across the dashed line through 0,0.\n"
                                       "Draw half an outline from the line back to the line and it closes into one whole shape."));
  drawRow->addWidget(mirrorBox);
  auto* close = new QPushButton(QStringLiteral("Close shape"), this);
  auto* measure = new QPushButton(QStringLiteral("Measure"), this);
  measure->setCheckable(true);
  measure->setToolTip(QStringLiteral("Click two points to see the distance. Nodes and shape centres snap, on every layer."));
  measure->setStyleSheet(QStringLiteral("QPushButton { color: #7fd3ff; } QPushButton:checked { background: #7fd3ff; color: #0b1a22; font-weight: 600; }"));
  drawRow->addWidget(close);
  drawRow->addWidget(measure);
  drawRow->addStretch(1);
  root->addLayout(drawRow);

  // Row 2: editing.
  auto* editRow = new QHBoxLayout();
  auto* undo = new QPushButton(QStringLiteral("Undo"), this);
  undo->setToolTip(QStringLiteral("Ctrl+Z"));
  auto* redo = new QPushButton(QStringLiteral("Redo"), this);
  redo->setToolTip(QStringLiteral("Ctrl+Y"));
  auto* clear = new QPushButton(QStringLiteral("Clear"), this);
  auto* snapBends = new QPushButton(QStringLiteral("Snap bends"), this);
  snapBends->setCheckable(true);
  snapBends->setChecked(true);
  snapBends->setToolTip(QStringLiteral("Snap curve handles onto the grid and onto nodes, so a bend is not 0.01 mm off. Hold Shift while dragging for a free move."));
  snapBends->setStyleSheet(QStringLiteral("QPushButton:checked { background: #e87400; color: #24180c; }"));
  auto* colorButton = new QPushButton(QStringLiteral("Color"), this);
  colorButton->setToolTip(QStringLiteral("Color of this SVG, and of the solid when it is placed on the workplane."));
  auto* save = new QPushButton(QStringLiteral("Save SVG…"), this);
  auto* importSvg = new QPushButton(QStringLiteral("Import SVG…"), this);
  importSvg->setToolTip(QStringLiteral("Add the shapes of any SVG file (curves, outlines and holes too) to this layer.\n"
                                       "Sizes in mm, cm or inches are kept; plain numbers count as mm."));
  editRow->addWidget(undo);
  editRow->addWidget(redo);
  editRow->addWidget(clear);
  editRow->addWidget(snapBends);
  editRow->addWidget(colorButton);
  editRow->addWidget(importSvg);
  editRow->addStretch(1);
  editRow->addWidget(save);
  root->addLayout(editRow);

  auto* body = new QHBoxLayout();
  editor = new Shape2DEditorWidget(this);
  body->addWidget(editor, 1);
  auto* sidebar = new QFrame(this);
  sidebar->setFixedWidth(270);
  sidebar->setObjectName(QStringLiteral("shape2dSidebar"));
  sidebar->setStyleSheet(QStringLiteral("QFrame { background: #0b0d11; }"));
  auto* sideLayout = new QVBoxLayout(sidebar);
  sideLayout->setContentsMargins(8, 8, 8, 8);

  // Selection: exact numbers for the picked node or shape.
  auto* selTitle = new QLabel(QStringLiteral("Selection"), sidebar);
  selTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #e87400; background: transparent;"));
  sideLayout->addWidget(selTitle);
  auto* selName = new QLabel(sidebar);
  selName->setStyleSheet(QStringLiteral("color: #ece6da; background: transparent;"));
  sideLayout->addWidget(selName);
  auto* selForm = new QFormLayout();
  auto makeNumber = [sidebar](double lo, double hi) {
    auto* box = new QDoubleSpinBox(sidebar);
    box->setRange(lo, hi);
    box->setDecimals(3);
    box->setSingleStep(0.1);
    box->setSuffix(QStringLiteral(" mm"));
    box->setKeyboardTracking(false);  // apply on Enter or when leaving the box, not per keystroke
    return box;
  };
  auto* xSpin = makeNumber(-100000, 100000);
  auto* ySpin = makeNumber(-100000, 100000);
  auto* wSpin = makeNumber(0.001, 100000);
  auto* hSpin = makeNumber(0.001, 100000);
  auto* keepRatio = new QCheckBox(QStringLiteral("Keep proportions"), sidebar);
  keepRatio->setChecked(true);
  keepRatio->setStyleSheet(QStringLiteral("color: #ece6da; background: transparent;"));
  auto formLabel = [sidebar](const QString& text) {
    auto* label = new QLabel(text, sidebar);
    label->setStyleSheet(QStringLiteral("color: #ece6da; background: transparent;"));
    return label;
  };
  selForm->addRow(formLabel(QStringLiteral("X")), xSpin);
  selForm->addRow(formLabel(QStringLiteral("Y")), ySpin);
  selForm->addRow(formLabel(QStringLiteral("Width")), wSpin);
  selForm->addRow(formLabel(QStringLiteral("Height")), hSpin);
  sideLayout->addLayout(selForm);
  sideLayout->addWidget(keepRatio);
  auto* selButtons = new QHBoxLayout();
  auto* duplicate = new QPushButton(QStringLiteral("Duplicate"), sidebar);
  duplicate->setToolTip(QStringLiteral("Ctrl+D"));
  auto* remove = new QPushButton(QStringLiteral("Delete"), sidebar);
  remove->setToolTip(QStringLiteral("Delete key"));
  selButtons->addWidget(duplicate);
  selButtons->addWidget(remove);
  sideLayout->addLayout(selButtons);
  auto* selButtons2 = new QHBoxLayout();
  auto* makeHole = new QPushButton(QStringLiteral("Make hole"), sidebar);
  makeHole->setToolTip(QStringLiteral("Turn the selected shape into a hole, or a hole back into a shape."));
  auto* toLayer = new QPushButton(QStringLiteral("To new layer"), sidebar);
  toLayer->setToolTip(QStringLiteral("Move the selected shape to a layer of its own, so holes on this layer stop cutting it."));
  selButtons2->addWidget(makeHole);
  selButtons2->addWidget(toLayer);
  sideLayout->addLayout(selButtons2);
  auto* selButtons3 = new QHBoxLayout();
  auto* through = new QCheckBox(QStringLiteral("Through all levels"), sidebar);
  through->setToolTip(QStringLiteral("This hole cuts every layer on every level, e.g. a bolt hole through the floor and the tube above it."));
  through->setStyleSheet(QStringLiteral("color: #ece6da; background: transparent;"));
  auto* copyUp = new QPushButton(QStringLiteral("Copy up a level"), sidebar);
  copyUp->setToolTip(QStringLiteral("Copy the selected outline to the level above, on the same spot (adds that level when needed)."));
  selButtons3->addWidget(through);
  selButtons3->addWidget(copyUp);
  sideLayout->addLayout(selButtons3);
  auto* turnRow = new QHBoxLayout();
  auto* turnSpin = new QDoubleSpinBox(sidebar);
  turnSpin->setRange(-360, 360);
  turnSpin->setDecimals(1);
  turnSpin->setValue(90);
  turnSpin->setSuffix(QStringLiteral("°"));
  turnSpin->setToolTip(QStringLiteral("How far Turn turns the selection, counter-clockwise. A negative angle turns clockwise.\n"
                                      "Or drag the round handle above the selection."));
  auto* turnButton = new QPushButton(QStringLiteral("Turn"), sidebar);
  auto* flipX = new QPushButton(QStringLiteral("Flip ↔"), sidebar);
  flipX->setToolTip(QStringLiteral("Mirror the selection left to right, in place."));
  auto* flipY = new QPushButton(QStringLiteral("Flip ↕"), sidebar);
  flipY->setToolTip(QStringLiteral("Mirror the selection top to bottom, in place."));
  turnRow->addWidget(turnSpin);
  turnRow->addWidget(turnButton);
  turnRow->addWidget(flipX);
  turnRow->addWidget(flipY);
  sideLayout->addLayout(turnRow);
  auto* combineRow = new QHBoxLayout();
  auto* joinButton = new QPushButton(QStringLiteral("Join"), sidebar);
  joinButton->setToolTip(QStringLiteral("Make the selected shapes one outline: everything they cover (minus any selected holes)."));
  auto* cutButton = new QPushButton(QStringLiteral("Cut out"), sidebar);
  cutButton->setToolTip(QStringLiteral("Cut the other selected shapes out of the one you picked first."));
  auto* overlapButton = new QPushButton(QStringLiteral("Overlap"), sidebar);
  overlapButton->setToolTip(QStringLiteral("Keep only where the selected shapes overlap."));
  for (QPushButton* button : {joinButton, cutButton, overlapButton}) {
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    combineRow->addWidget(button);
  }
  sideLayout->addLayout(combineRow);
  auto* alignTitle = new QLabel(QStringLiteral("Line up"), sidebar);
  alignTitle->setStyleSheet(QStringLiteral("color: #99a5b3; background: transparent;"));
  sideLayout->addWidget(alignTitle);
  auto* alignRowA = new QHBoxLayout();
  auto* alignRowB = new QHBoxLayout();
  QList<QPushButton*> alignButtons;
  const QStringList alignNames{QStringLiteral("Left"), QStringLiteral("Centre"), QStringLiteral("Right"),
                               QStringLiteral("Top"), QStringLiteral("Middle"), QStringLiteral("Bottom")};
  for (int i = 0; i < alignNames.size(); ++i) {
    auto* button = new QPushButton(alignNames[i], sidebar);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setToolTip(QStringLiteral("Line up the selected shapes with each other. One shape lines up with 0,0."));
    (i < 3 ? alignRowA : alignRowB)->addWidget(button);
    alignButtons.push_back(button);
  }
  sideLayout->addLayout(alignRowA);
  sideLayout->addLayout(alignRowB);
  auto* roundRow = new QHBoxLayout();
  auto* roundSpin = new QDoubleSpinBox(sidebar);
  roundSpin->setRange(0.01, 10000);
  roundSpin->setDecimals(2);
  roundSpin->setValue(2);
  roundSpin->setSuffix(QStringLiteral(" mm"));
  roundSpin->setToolTip(QStringLiteral("Radius of the rounded corner."));
  roundSpin->setObjectName(QStringLiteral("shape2dRoundRadius"));
  auto* roundButton = new QPushButton(QStringLiteral("Round corners"), sidebar);
  roundButton->setToolTip(QStringLiteral("A selected corner node: round that corner.\nSelected shapes: round all their sharp corners."));
  roundRow->addWidget(roundSpin);
  roundRow->addWidget(roundButton, 1);
  sideLayout->addLayout(roundRow);
  auto* repeatButton = new QPushButton(QStringLiteral("Repeat…"), sidebar);
  repeatButton->setToolTip(QStringLiteral("Copies of the selection in a row, in a grid or around a circle."));
  sideLayout->addWidget(repeatButton);
  auto* selNote = new QLabel(QStringLiteral("X and Y are the centre of the selection, or the node itself. Width and Height are the outline size, so a circle's width is its diameter. Shift-click or drag a box to select more; Ctrl+A selects the whole layer."), sidebar);
  selNote->setWordWrap(true);
  selNote->setStyleSheet(QStringLiteral("color: #99a5b3; background: transparent;"));
  sideLayout->addWidget(selNote);
  sideLayout->addSpacing(10);

  // Levels: stacked on top of each other in 3D, one part per level.
  auto* levelTitle = new QLabel(QStringLiteral("Levels"), sidebar);
  levelTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #e87400; background: transparent;"));
  sideLayout->addWidget(levelTitle);
  auto* levelList = new QListWidget(sidebar);
  levelList->setObjectName(QStringLiteral("shape2dLevels"));
  levelList->setStyleSheet(QStringLiteral("QListWidget { background: #12161c; color: #ece6da; border: 1px solid #2a3340; } QListWidget::item:selected { background: #e87400; color: #24180c; }"));
  levelList->setMinimumHeight(96);
  levelList->setMaximumHeight(120);
  sideLayout->addWidget(levelList);
  auto* levelRow = new QHBoxLayout();
  auto* addLevel = new QPushButton(QStringLiteral("New level"), sidebar);
  addLevel->setToolTip(QStringLiteral("Add a level on top. In 3D it sits on the level below it."));
  auto* removeLevel = new QPushButton(QStringLiteral("Remove"), sidebar);
  removeLevel->setToolTip(QStringLiteral("Remove this level once it is empty. The levels above move down one."));
  for (QPushButton* button : {addLevel, removeLevel}) button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  levelRow->addWidget(addLevel);
  levelRow->addWidget(removeLevel);
  sideLayout->addLayout(levelRow);
  auto* levelForm = new QFormLayout();
  auto* thickness = makeNumber(0.01, 10000);
  thickness->setObjectName(QStringLiteral("shape2dLevelThickness"));
  thickness->setValue(1.0);
  thickness->setToolTip(QStringLiteral("How thick this level is when placed. You can still change each part's height in 3D."));
  levelForm->addRow(formLabel(QStringLiteral("Thickness")), thickness);
  sideLayout->addLayout(levelForm);
  auto* levelNote = new QLabel(QStringLiteral("Level 1 stands on the plate and each next level on top of the one below, each as its own part. The level below shows faded; its nodes and centres snap."), sidebar);
  levelNote->setWordWrap(true);
  levelNote->setStyleSheet(QStringLiteral("color: #99a5b3; background: transparent;"));
  sideLayout->addWidget(levelNote);
  sideLayout->addSpacing(10);

  auto* sideTitle = new QLabel(QStringLiteral("Layers"), sidebar);
  sideTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #e87400; background: transparent;"));
  sideLayout->addWidget(sideTitle);
  auto* layerList = new QListWidget(sidebar);
  layerList->setSelectionMode(QAbstractItemView::ExtendedSelection);
  layerList->setStyleSheet(QStringLiteral("QListWidget { background: #12161c; color: #ece6da; border: 1px solid #2a3340; } QListWidget::item:selected { background: #e87400; color: #24180c; }"));
  layerList->setMinimumHeight(96);
  sideLayout->addWidget(layerList, 1);
  auto* layerRowA = new QHBoxLayout();
  auto* layerRowB = new QHBoxLayout();
  auto* addLayer = new QPushButton(QStringLiteral("New layer"), sidebar);
  auto* mergeLayers = new QPushButton(QStringLiteral("Merge"), sidebar);
  auto* unmergeLayers = new QPushButton(QStringLiteral("Unmerge"), sidebar);
  auto* removeLayer = new QPushButton(QStringLiteral("Remove"), sidebar);
  for (QPushButton* button : {addLayer, mergeLayers, unmergeLayers, removeLayer})
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  layerRowA->addWidget(addLayer);
  layerRowA->addWidget(mergeLayers);
  layerRowB->addWidget(unmergeLayers);
  layerRowB->addWidget(removeLayer);
  sideLayout->addLayout(layerRowA);
  sideLayout->addLayout(layerRowB);
  auto* layerNote = new QLabel(QStringLiteral("A hole only cuts the shapes on its own layer, so a shape on another layer can sit inside it. Merge keeps that apart; Unmerge splits the layers again. Other layers stay dim and locked."), sidebar);
  layerNote->setWordWrap(true);
  layerNote->setStyleSheet(QStringLiteral("color: #99a5b3; background: transparent;"));
  sideLayout->addWidget(layerNote);
  // The panel scrolls rather than squeezing its boxes on a short screen.
  auto* sideScroll = new QScrollArea(this);
  sideScroll->setWidget(sidebar);
  sideScroll->setWidgetResizable(true);
  sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  sideScroll->setFrameShape(QFrame::NoFrame);
  sideScroll->setFixedWidth(270 + 14);
  sideScroll->setStyleSheet(QStringLiteral("QScrollArea { background: #0b0d11; }"));
  sidebar->setFixedWidth(270);
  body->addWidget(sideScroll);
  root->addLayout(body, 1);
  status = new QLabel(QStringLiteral("Click to draw a shape. Draw adds shapes; select one and press Make hole to cut it out."), this);
  status->setObjectName(QStringLiteral("shape2dStatus"));
  root->addWidget(status);
  editor->setStatusSink(status);

  connect(gridBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, gridBox] {
    editor->setGridStep(gridBox->currentData().toDouble());
  });
  // Each kind remembers its own count and width, so a star does not inherit a polygon's six sides.
  auto sidesByKind = std::make_shared<QMap<int, int>>(QMap<int, int>{{5, 6}, {6, 5}, {12, 2}});
  auto innerByKind = std::make_shared<QMap<int, double>>(QMap<int, double>{{6, 50}, {8, 30}, {9, 30}, {12, 15}});
  connect(primitiveBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this, primitiveBox, fromBox, sidesSpin, cornerSpin, innerSpin, angleSpin, turnsSpin, textEdit, fontBox, boldBox, sidesByKind, innerByKind] {
    const int kind = primitiveBox->currentData().toInt();
    editor->setPrimitive(kind);
    fromBox->setVisible(kind > 0);
    sidesSpin->setVisible(kind == 5 || kind == 6 || kind == 12);
    cornerSpin->setVisible(kind == 2);
    innerSpin->setVisible(kind == 6 || kind == 8 || kind == 9 || kind == 12);
    angleSpin->setVisible(kind == 9);
    turnsSpin->setVisible(kind == 12);
    for (QWidget* w : std::initializer_list<QWidget*>{textEdit, fontBox, boldBox}) w->setVisible(kind == 13);
    {
      const QSignalBlocker quiet(sidesSpin);
      sidesSpin->setPrefix(kind == 6 ? QStringLiteral("Points ") : kind == 12 ? QStringLiteral("Arms ") : QStringLiteral("Sides "));
      sidesSpin->setMinimum(kind == 12 ? 1 : 3);
      sidesSpin->setToolTip(kind == 6    ? QStringLiteral("How many points the star has.")
                            : kind == 12 ? QStringLiteral("How many arms wind out from the middle. 2 makes an S.")
                                         : QStringLiteral("Number of sides for the polygon. 6 is a hexagon."));
      if (sidesByKind->contains(kind)) sidesSpin->setValue(sidesByKind->value(kind));
    }
    {
      const QSignalBlocker quiet(innerSpin);
      innerSpin->setPrefix(kind == 6 ? QStringLiteral("Depth ") : kind == 12 ? QStringLiteral("Line ") : QStringLiteral("Width "));
      innerSpin->setToolTip(kind == 6    ? QStringLiteral("How deep the gaps between the points go, as a part of the radius.")
                            : kind == 12 ? QStringLiteral("How wide the arms are in the middle, as a part of the radius. They narrow toward the tips.")
                                         : QStringLiteral("How wide the ring is, as a part of its radius."));
      if (innerByKind->contains(kind)) innerSpin->setValue(innerByKind->value(kind));
    }
    editor->setPolygonSides(sidesSpin->value());
    editor->setStampInner(innerSpin->value());
    if (kind == 13) textEdit->setFocus();
  });
  connect(innerSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, primitiveBox, innerByKind](double value) {
    (*innerByKind)[primitiveBox->currentData().toInt()] = value;
    editor->setStampInner(value);
  });
  connect(angleSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { editor->setStampAngle(value); });
  connect(turnsSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { editor->setStampTurns(value); });
  auto applyText = [this, textEdit, fontBox, boldBox] { editor->setStampText(textEdit->text(), fontBox->currentFont().family(), boldBox->isChecked()); };
  connect(textEdit, &QLineEdit::textChanged, this, applyText);
  connect(fontBox, &QFontComboBox::currentFontChanged, this, applyText);
  connect(boldBox, &QCheckBox::toggled, this, applyText);
  applyText();
  connect(mirrorBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, mirrorBox] {
    editor->setMirrorMode(mirrorBox->currentData().toInt());
    editor->setFocus();
  });
  connect(fromBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, fromBox] {
    editor->setDrawFrom(fromBox->currentData().toInt());
  });
  connect(sidesSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this, primitiveBox, sidesByKind](int value) {
    (*sidesByKind)[primitiveBox->currentData().toInt()] = value;
    editor->setPolygonSides(value);
  });
  connect(cornerSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { editor->setCornerRadius(value); });

  auto refreshLayers = [this, layerList] {
    const int current = editor->layer();
    layerList->blockSignals(true);
    layerList->clear();
    int select = 0;
    const QList<int> ids = editor->layerIds();
    for (int i = 0; i < ids.size(); ++i) {
      const int count = editor->shapesOnLayer(ids[i]);
      const int holes = editor->holesOnLayer(ids[i]);
      const int solids = count - holes;
      QString detail = count == 0 ? QStringLiteral("empty")
                                  : QStringLiteral("%1 shape%2").arg(solids).arg(solids == 1 ? QString() : QStringLiteral("s"));
      if (holes > 0) detail += QStringLiteral(", %1 hole%2").arg(holes).arg(holes == 1 ? QString() : QStringLiteral("s"));
      auto* item = new QListWidgetItem(QStringLiteral("Layer %1\n%2").arg(ids[i]).arg(detail), layerList);
      item->setData(Qt::UserRole, ids[i]);
      item->setSizeHint(QSize(0, 40));
      if (ids[i] == current) select = i;
    }
    layerList->setCurrentRow(select);
    layerList->blockSignals(false);
  };
  auto refreshLevels = [this, levelList, thickness, removeLevel] {
    const int current = editor->level();
    levelList->blockSignals(true);
    levelList->clear();
    QList<int> ids = editor->levelIds();
    std::reverse(ids.begin(), ids.end());  // top level first, as they stack
    int select = 0;
    for (int i = 0; i < ids.size(); ++i) {
      const int solids = editor->shapesOnLevel(ids[i]);
      const int holes = editor->holesOnLevel(ids[i]);
      QString detail = solids + holes == 0 ? QStringLiteral("empty")
                                           : QStringLiteral("%1 shape%2").arg(solids).arg(solids == 1 ? QString() : QStringLiteral("s"));
      if (holes > 0) detail += QStringLiteral(", %1 hole%2").arg(holes).arg(holes == 1 ? QString() : QStringLiteral("s"));
      auto* item = new QListWidgetItem(QStringLiteral("Level %1 · %2 mm\n%3")
                                           .arg(ids[i])
                                           .arg(editor->levelHeight(ids[i]), 0, 'g', 6)
                                           .arg(detail),
                                       levelList);
      item->setData(Qt::UserRole, ids[i]);
      item->setSizeHint(QSize(0, 40));
      if (ids[i] == current) select = i;
    }
    levelList->setCurrentRow(select);
    levelList->blockSignals(false);
    const QSignalBlocker blocker(thickness);
    if (!thickness->hasFocus()) thickness->setValue(editor->levelHeight(current));
    removeLevel->setEnabled(ids.size() > 1);
  };
  editor->setChangeHook([refreshLayers, refreshLevels, selectButton, drawButton, measure, undo, redo, this] {
    refreshLayers();
    refreshLevels();
    for (QPushButton* button : {selectButton, drawButton, measure}) button->blockSignals(true);
    drawButton->setChecked(editor->isDrawing() && !editor->isMeasuring());
    selectButton->setChecked(!editor->isDrawing() && !editor->isMeasuring());
    measure->setChecked(editor->isMeasuring());
    for (QPushButton* button : {selectButton, drawButton, measure}) button->blockSignals(false);
    undo->setEnabled(editor->canUndo());
    redo->setEnabled(editor->canRedo());
  });
  auto syncing = std::make_shared<bool>(false);
  editor->setSelectionHook([this, selName, xSpin, ySpin, wSpin, hSpin, keepRatio, duplicate, remove, makeHole, toLayer, through, copyUp,
                            turnSpin, turnButton, flipX, flipY, joinButton, cutButton, overlapButton, alignButtons, roundButton,
                            repeatButton, syncing] {
    const int kind = editor->selectionKind();
    const int count = editor->selectedCount();
    for (QWidget* w : std::initializer_list<QWidget*>{turnSpin, turnButton, flipX, flipY, repeatButton}) w->setEnabled(kind == 2);
    for (QPushButton* button : {joinButton, cutButton, overlapButton}) button->setEnabled(count >= 2);
    for (QPushButton* button : alignButtons) button->setEnabled(kind == 2);
    roundButton->setEnabled(kind != 0);
    roundButton->setText(kind == 1 ? QStringLiteral("Round corner") : QStringLiteral("Round corners"));
    selName->setText(editor->selectionName());
    *syncing = true;
    for (QDoubleSpinBox* box : {xSpin, ySpin, wSpin, hSpin}) {
      const QSignalBlocker blocker(box);
      box->setEnabled(kind != 0);
    }
    wSpin->setEnabled(kind == 2);
    hSpin->setEnabled(kind == 2);
    keepRatio->setEnabled(kind == 2);
    duplicate->setEnabled(kind == 2);
    remove->setEnabled(kind != 0 || editor->hasMeasureSelected());
    makeHole->setEnabled(kind == 2);
    makeHole->setText(editor->selectionIsHole() ? QStringLiteral("Make shape") : QStringLiteral("Make hole"));
    toLayer->setEnabled(kind == 2);
    copyUp->setEnabled(kind == 2);
    {
      const QSignalBlocker blocker(through);
      through->setEnabled(editor->selectionIsHole());
      through->setChecked(editor->selectionIsThrough());
    }
    if (kind != 0) {
      const QPointF p = editor->selectionPos();
      const QSignalBlocker bx(xSpin);
      const QSignalBlocker by(ySpin);
      if (!xSpin->hasFocus()) xSpin->setValue(p.x());
      if (!ySpin->hasFocus()) ySpin->setValue(p.y());
    }
    if (kind == 2) {
      const QSizeF size = editor->selectionSize();
      const QSignalBlocker bw(wSpin);
      const QSignalBlocker bh(hSpin);
      if (!wSpin->hasFocus()) wSpin->setValue(size.width());
      if (!hSpin->hasFocus()) hSpin->setValue(size.height());
    }
    *syncing = false;
  });
  auto applyPos = [this, xSpin, ySpin, syncing] {
    if (*syncing || editor->selectionKind() == 0) return;
    editor->setSelectionPos(QPointF(xSpin->value(), ySpin->value()));
  };
  connect(xSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, applyPos);
  connect(ySpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, applyPos);
  auto applySize = [this, wSpin, hSpin, keepRatio, syncing](bool widthChanged) {
    if (*syncing || editor->selectionKind() != 2) return;
    const QSizeF now = editor->selectionSize();
    double w = wSpin->value();
    double h = hSpin->value();
    if (keepRatio->isChecked() && now.width() > 1e-9 && now.height() > 1e-9) {
      if (widthChanged) h = w * now.height() / now.width();
      else w = h * now.width() / now.height();
    }
    editor->setSelectionSize(w, h);
  };
  connect(wSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [applySize] { applySize(true); });
  connect(hSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [applySize] { applySize(false); });
  connect(duplicate, &QPushButton::clicked, this, [this] { editor->duplicateSelection(); });
  connect(remove, &QPushButton::clicked, this, [this] { editor->deleteSelection(); });
  connect(makeHole, &QPushButton::clicked, this, [this] { editor->toggleSelectionHole(); });
  connect(toLayer, &QPushButton::clicked, this, [this] { editor->moveSelectionToNewLayer(); });
  connect(through, &QCheckBox::toggled, this, [this] { editor->toggleSelectionThrough(); });
  connect(copyUp, &QPushButton::clicked, this, [this] { editor->copySelectionToNextLevel(); });
  connect(turnButton, &QPushButton::clicked, this, [this, turnSpin] { editor->turnSelection(turnSpin->value()); });
  connect(flipX, &QPushButton::clicked, this, [this] { editor->flipSelection(true); });
  connect(flipY, &QPushButton::clicked, this, [this] { editor->flipSelection(false); });
  connect(joinButton, &QPushButton::clicked, this, [this] { editor->combineSelection(shape2d::Combine::Merge); });
  connect(cutButton, &QPushButton::clicked, this, [this] { editor->combineSelection(shape2d::Combine::Cut); });
  connect(overlapButton, &QPushButton::clicked, this, [this] { editor->combineSelection(shape2d::Combine::Overlap); });
  for (int i = 0; i < alignButtons.size(); ++i)
    connect(alignButtons[i], &QPushButton::clicked, this, [this, i] { editor->alignSelection(i); });
  connect(roundButton, &QPushButton::clicked, this, [this, roundSpin] { editor->roundCorners(roundSpin->value()); });
  connect(repeatButton, &QPushButton::clicked, this, [this] { openRepeat(); });
  connect(importSvg, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import SVG"), QString(), QStringLiteral("SVG drawing (*.svg *.SVG)"));
    if (path.isEmpty()) return;
    QString error;
    const QVector<Shape2DContour> found = shape2d::readSvg(path, &error);
    if (found.isEmpty()) {
      status->setText(error);
      return;
    }
    editor->importContours(found);
    editor->setFocus();
  });
  refreshLevels();
  connect(levelList, &QListWidget::itemPressed, this, [this](QListWidgetItem* item) {
    if (item) editor->setLevel(item->data(Qt::UserRole).toInt());
  });
  connect(addLevel, &QPushButton::clicked, this, [this] { editor->addLevel(); });
  connect(removeLevel, &QPushButton::clicked, this, [this] {
    if (!editor->removeCurrentLevel())
      status->setText(editor->levelIds().size() <= 1 ? QStringLiteral("The last level stays.")
                                                     : QStringLiteral("That level still has shapes. Delete them before removing it."));
  });
  connect(thickness, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
    editor->setLevelHeight(editor->level(), value);
  });

  refreshLayers();
  connect(layerList, &QListWidget::itemPressed, this, [this](QListWidgetItem* item) {
    if (!item) return;
    editor->setLayer(item->data(Qt::UserRole).toInt());
  });
  connect(addLayer, &QPushButton::clicked, this, [this] { editor->addLayer(); });
  connect(mergeLayers, &QPushButton::clicked, this, [this, layerList] {
    QList<int> ids;
    for (QListWidgetItem* item : layerList->selectedItems()) ids.push_back(item->data(Qt::UserRole).toInt());
    if (!editor->mergeLayers(ids))
      status->setText(QStringLiteral("Shift-click two or more layers, then Merge. A hole keeps cutting only the shapes it was drawn with."));
  });
  connect(unmergeLayers, &QPushButton::clicked, this, [this] {
    if (!editor->unmergeLayers())
      status->setText(QStringLiteral("Nothing to unmerge. Merge keeps the original layers so they can be split again."));
  });
  connect(removeLayer, &QPushButton::clicked, this, [this] {
    if (!editor->removeCurrentLayer())
      status->setText(editor->layerIds().size() <= 1 ? QStringLiteral("The last layer stays.")
                                                      : QStringLiteral("That layer still has shapes. Delete them before removing it."));
  });
  connect(clear, &QPushButton::clicked, this, [this] { editor->clearShape(); });
  connect(undo, &QPushButton::clicked, this, [this] { editor->undo(); });
  connect(redo, &QPushButton::clicked, this, [this] { editor->redo(); });
  connect(close, &QPushButton::clicked, this, [this] { editor->closeShape(); });
  connect(selectButton, &QPushButton::clicked, this, [this] {
    editor->setDrawMode(false);
    editor->setFocus();
  });
  connect(drawButton, &QPushButton::clicked, this, [this] {
    editor->setDrawMode(true);
    editor->setFocus();
  });
  connect(measure, &QPushButton::toggled, this, [this](bool on) {
    editor->setMeasuring(on);
    editor->setFocus();
  });
  connect(snapBends, &QPushButton::toggled, this, [this](bool on) { editor->setSnapBends(on); });
  auto paintSwatch = [colorButton](const QColor& color) {
    const QString text = color.lightness() > 150 ? QStringLiteral("#1b140c") : QStringLiteral("#f4efe6");
    colorButton->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; font-weight: 600; }").arg(color.name(QColor::HexRgb), text));
  };
  paintSwatch(editor->inkColor());
  connect(colorButton, &QPushButton::clicked, this, [this, paintSwatch] {
    const QColor chosen = QColorDialog::getColor(editor->inkColor(), this, QStringLiteral("SVG color"));
    if (!chosen.isValid()) return;
    editor->setInk(chosen);
    paintSwatch(chosen);
  });
  connect(save, &QPushButton::clicked, this, [this] { saveShape(); });

  auto* done = new QPushButton(QStringLiteral("Done"), this);
  root->addWidget(done, 0, Qt::AlignRight);
  connect(done, &QPushButton::clicked, this, [this] { accept(); });

  if (!existing.isEmpty()) {
    QVector<Shape2DContour> loaded;
    QColor ink = readSvgShapeColor(existing);
    QString loadedName;
    const QString steps = synthvgPathFor(existing);
    QMap<int, double> heights;
    QVector<QLineF> lines;
    const bool fromSteps = QFile::exists(steps) && readSynthvg(steps, &loaded, &ink, &loadedName, &origin, &haveOrigin, &heights, &lines);
    if (fromSteps || readShapeContours(existing, &loaded)) {
      // Older files carry their origin only in the SVG. Reuse it so placed parts do not move.
      if (!haveOrigin) haveOrigin = readSvgCenter(existing, &origin);
      editor->setContours(loaded, heights);
      editor->setMeasureLines(lines);
      editor->setInk(ink);
      editor->markClean();
      paintSwatch(ink);
      savedPath = existing;
    } else {
      // A drawing from elsewhere: read its shapes; saving makes a new editable shape and leaves the file as it is.
      QString error;
      loaded = shape2d::readSvg(existing, &error);
      if (!loaded.isEmpty()) {
        editor->setContours(loaded);
        importedName = QFileInfo(existing).completeBaseName();
        status->setText(QStringLiteral("Read from an SVG drawing. Save keeps it as a new shape you can edit; the file itself stays as it is."));
      } else {
        status->setText(error);
      }
    }
  }
  editor->setDrawMode(existing.isEmpty());
  editor->markClean();
  editor->setFocus();
}

void Shape2DEditorDialog::openRepeat() {
  if (editor->selectionKind() != 2) {
    status->setText(QStringLiteral("Select the shapes to repeat first."));
    return;
  }
  const QSizeF size = editor->selectionSize();
  QDialog box(this);
  box.setWindowTitle(QStringLiteral("Repeat"));
  auto* form = new QFormLayout(&box);
  auto* pattern = new QComboBox(&box);
  pattern->addItems({QStringLiteral("In a row"), QStringLiteral("In a grid"), QStringLiteral("Around a circle")});
  auto* count = new QSpinBox(&box);
  count->setRange(2, 1000);
  count->setValue(4);
  auto* rows = new QSpinBox(&box);
  rows->setRange(1, 1000);
  rows->setValue(3);
  auto number = [&box](double value, const QString& suffix = QStringLiteral(" mm")) {
    auto* spin = new QDoubleSpinBox(&box);
    spin->setRange(-100000, 100000);
    spin->setDecimals(3);
    spin->setSuffix(suffix);
    spin->setValue(value);
    return spin;
  };
  auto* stepX = number(std::round((size.width() + 2) * 1000) / 1000);
  auto* stepY = number(0);
  auto* centreX = number(0);
  auto* centreY = number(0);
  auto* spread = number(360, QStringLiteral("°"));
  spread->setRange(1, 360);
  spread->setDecimals(1);
  auto* turnCopies = new QCheckBox(QStringLiteral("Turn each copy with the circle"), &box);
  turnCopies->setChecked(true);
  form->addRow(QStringLiteral("Pattern"), pattern);
  form->addRow(QStringLiteral("How many"), count);
  form->addRow(QStringLiteral("Rows"), rows);
  form->addRow(QStringLiteral("Step across"), stepX);
  form->addRow(QStringLiteral("Step up"), stepY);
  form->addRow(QStringLiteral("Circle centre X"), centreX);
  form->addRow(QStringLiteral("Circle centre Y"), centreY);
  form->addRow(QStringLiteral("Spread over"), spread);
  form->addRow(QString(), turnCopies);
  auto* note = new QLabel(&box);
  note->setWordWrap(true);
  note->setStyleSheet(QStringLiteral("color: #99a5b3;"));
  form->addRow(note);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &box);
  buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Add copies"));
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &box, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &box, &QDialog::reject);
  auto copies = [=] {
    return editor->repeatCopies(pattern->currentIndex(), count->value(), rows->value(), QPointF(stepX->value(), stepY->value()),
                                QPointF(centreX->value(), centreY->value()), turnCopies->isChecked(), spread->value());
  };
  auto refresh = [=] {
    const int mode = pattern->currentIndex();
    form->setRowVisible(rows, mode == 1);
    form->setRowVisible(stepX, mode != 2);
    form->setRowVisible(stepY, mode != 2);
    for (QWidget* w : std::initializer_list<QWidget*>{centreX, centreY, spread, turnCopies}) form->setRowVisible(w, mode == 2);
    form->labelForField(count)->setProperty("text", mode == 1 ? QStringLiteral("Columns") : QStringLiteral("How many"));
    note->setText(mode == 0   ? QStringLiteral("How many in all, the selection included, each one step further on.")
                  : mode == 1 ? QStringLiteral("Columns across and rows down from the selection, a step apart.")
                              : QStringLiteral("How many in all around the circle, the selection included. Full circle: spread over 360°."));
    editor->setPreview(copies());
  };
  connect(pattern, qOverload<int>(&QComboBox::currentIndexChanged), &box, [=](int mode) {
    if (mode == 1 && std::abs(stepY->value()) < 1e-9) {
      const QSignalBlocker quiet(stepY);
      stepY->setValue(std::round((size.height() + 2) * 1000) / 1000);
    }
    refresh();
  });
  for (QSpinBox* spin : {count, rows}) connect(spin, qOverload<int>(&QSpinBox::valueChanged), &box, refresh);
  for (QDoubleSpinBox* spin : {stepX, stepY, centreX, centreY, spread}) connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), &box, refresh);
  connect(turnCopies, &QCheckBox::toggled, &box, refresh);
  refresh();
  if (box.exec() == QDialog::Accepted) editor->addRepeats(copies());
  else editor->setPreview({});
  editor->setFocus();
}

void Shape2DEditorDialog::done(int result) {
  QSettings().setValue(QStringLiteral("shape2dEditor/geometry"), saveGeometry());
  if (editor && editor->isDirty()) {
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("2D Shape Editor"));
    box.setText(QStringLiteral("This shape has changes that are not saved."));
    box.setInformativeText(QStringLiteral("Save them before closing?"));
    QPushButton* saveButton = box.addButton(QStringLiteral("Save"), QMessageBox::AcceptRole);
    QPushButton* discardButton = box.addButton(QStringLiteral("Discard changes"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(saveButton);
    box.exec();
    if (box.clickedButton() == saveButton) {
      if (!saveShape()) return;  // stay open; the status line says why
      QDialog::done(QDialog::Accepted);
      return;
    }
    if (box.clickedButton() != discardButton) return;  // Cancel: keep editing
  } else if (editor && result == QDialog::Accepted && editor->hasOpenHole()) {
    status->setText(QStringLiteral("Close the open loop first, or undo it."));
    return;
  }
  QDialog::done(result);
}

bool Shape2DEditorDialog::saveShape() {
  if (!editor->outerClosed()) {
    status->setText(QStringLiteral("Close a shape first."));
    return false;
  }
  if (editor->hasOpenHole()) {
    status->setText(QStringLiteral("Close the open loop before saving."));
    return false;
  }
  const QFileInfo current(savedPath);
  QString suggested = savedPath.isEmpty() && !importedName.isEmpty() ? importedName : current.completeBaseName();
  if (suggested.isEmpty() || suggested.startsWith(QLatin1String("layer-")) || suggested.startsWith(QLatin1String("level-")))
    suggested = QStringLiteral("shape");
  bool accepted = false;
  const QString typed = QInputDialog::getText(this, QStringLiteral("Save 2D shape"), QStringLiteral("Name"), QLineEdit::Normal,
                                              suggested, &accepted);
  if (!accepted) return false;
  QString name = typed.trimmed();
  name.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("-"));
  if (name.isEmpty()) {
    status->setText(QStringLiteral("The shape needs a name."));
    return false;
  }
  // Same name as the file being edited: save over it where it is. A new name: a new shape in the library.
  const bool sameShape = !savedPath.isEmpty() && name.compare(current.completeBaseName(), Qt::CaseInsensitive) == 0;
  const QString dirPath = sameShape ? current.absolutePath() : QDir(libraryRoot).absoluteFilePath(name);
  const QString svgPath = QDir(dirPath).absoluteFilePath(name + QStringLiteral(".svg"));
  if (!sameShape && QFileInfo::exists(svgPath)) {
    if (QMessageBox::question(this, QStringLiteral("Save 2D shape"),
                              QStringLiteral("A shape called “%1” already exists. Replace it?").arg(name)) != QMessageBox::Yes)
      return false;
  }
  if (!QDir().mkpath(dirPath)) {
    status->setText(QStringLiteral("Could not create the shape folder."));
    return false;
  }
  const QVector<Shape2DContour> contours = editor->shapeContours();
  const QString stepsPath = QDir(dirPath).absoluteFilePath(name + QStringLiteral(".synthvg"));
  // The origin is fixed the first time a shape is saved and kept after that, so growing the
  // drawing on one side does not shift parts that are already placed. Every layer and level file
  // shares it, so they line up when placed.
  QPointF written;
  if (!writeSimpleSvgPath(svgPath, contoursAllLevels(contours), editor->inkColor(), haveOrigin ? &origin : nullptr, &written)) {
    status->setText(QStringLiteral("Could not write that shape."));
    return false;
  }
  origin = written;
  haveOrigin = true;
  if (!writeSynthvg(stepsPath, name, editor->inkColor(), contours, origin, editor->levelHeightMap(), editor->measureLines())) {
    status->setText(QStringLiteral("Could not write that shape."));
    return false;
  }
  const QList<int> levels = shape2DLevels(contours);
  const bool stacked = levels.size() > 1;
  // Stacked: one file per level. Otherwise one file per layer, as before.
  QSet<int> layers;
  if (stacked) {
    for (int level : levels) {
      const QString levelPath = QDir(dirPath).absoluteFilePath(QStringLiteral("level-%1.svg").arg(level));
      if (!writeSimpleSvgPath(levelPath, contoursForLevel(contours, level), editor->inkColor(), &origin, nullptr)) {
        status->setText(QStringLiteral("Could not write level %1.").arg(level));
        return false;
      }
    }
  } else {
    const QVector<Shape2DContour> flat = contoursForLevel(contours, levels.front());
    for (const Shape2DContour& contour : flat)
      if (contour.closed && contour.nodes.size() >= 3) layers.insert(contour.layer);
    for (int layer : layers) {
      QVector<Shape2DContour> only;
      for (const Shape2DContour& contour : flat)
        if (contour.layer == layer) only.push_back(contour);
      const QString layerPath = QDir(dirPath).absoluteFilePath(QStringLiteral("layer-%1.svg").arg(layer));
      if (!writeSimpleSvgPath(layerPath, only, editor->inkColor(), &origin, nullptr)) {
        status->setText(QStringLiteral("Could not write a layer."));
        return false;
      }
    }
  }
  // Layer and level files that no longer belong to the shape would otherwise linger and be picked up later.
  const QRegularExpression partName(QStringLiteral(R"(^(layer|level)-(\d+)\.svg$)"), QRegularExpression::CaseInsensitiveOption);
  for (const QFileInfo& info : QDir(dirPath).entryInfoList(QStringList{QStringLiteral("layer-*.svg"), QStringLiteral("level-*.svg")}, QDir::Files)) {
    const QRegularExpressionMatch match = partName.match(info.fileName());
    if (!match.hasMatch()) continue;
    const bool isLevel = match.captured(1).compare(QLatin1String("level"), Qt::CaseInsensitive) == 0;
    const int number = match.captured(2).toInt();
    const bool keep = isLevel ? (stacked && levels.contains(number)) : (!stacked && layers.contains(number));
    if (!keep) QFile::remove(info.absoluteFilePath());
  }
  savedPath = svgPath;
  savedNow = true;
  editor->markClean();
  const int idle = editor->idleHoles();
  const QString saved = stacked ? QStringLiteral("Saved %1 with %2 levels.").arg(name).arg(levels.size()) : QStringLiteral("Saved %1.").arg(name);
  status->setText(idle > 0 ? QStringLiteral("%1 Note: %2 hole%3 sit on a layer with no shape and cut nothing.")
                                 .arg(saved)
                                 .arg(idle)
                                 .arg(idle == 1 ? QString() : QStringLiteral("s"))
                           : saved);
  return true;
}
