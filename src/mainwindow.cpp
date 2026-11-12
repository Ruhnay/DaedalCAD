#include "mainwindow.h"
#include "appdirs.h"
#include "updater.h"
#include "splashoverlay.h"

#include "drawing.h"
#include "drawingdialog.h"
#include "usage.h"
#include "exporter.h"
#include "stepexport.h"
#include "exactstep.h"
#include "cloudsync.h"
#include "evaluator.h"
#include "generators.h"
#include "shape2d.h"
#include "sculpt.h"
#include "sculptdialog.h"
#include "latticedialog.h"
#include "effectdialog.h"
#include "effects.h"
#include "textshape.h"
#include "viewport.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryFile>
#include <QUrl>
#include <QDateTime>
#include <QDesktopServices>
#include <QCheckBox>
#include <QClipboard>
#include <QGuiApplication>
#include <QRadioButton>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QSaveFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPair>
#include <QPen>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFont>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QSlider>
#include <QTimer>
#include <QInputDialog>
#include <QTabBar>
#include <QTabWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <QQuaternion>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <iterator>
#include <QSizePolicy>
#include <QGridLayout>
#include <QSettings>
#include <QKeySequenceEdit>
#include <QWidgetAction>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QShortcut>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QThread>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QDrag>

#include <algorithm>
#include <map>
#include <array>
#include <utility>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <thread>
#include <vector>

class ShapeList : public QListWidget {
 public:
  bool consumedDrag = false;
  bool suppressClick = false;
  explicit ShapeList(QWidget* parent = nullptr) : QListWidget(parent) {}

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    suppressClick = event->button() == Qt::RightButton;
    if (event->button() == Qt::LeftButton) suppressClick = false;
    QListWidget::mousePressEvent(event);
    origin = event->pos();
    consumedDrag = false;
  }
  void mouseMoveEvent(QMouseEvent* event) override {
    if (!(event->buttons() & Qt::LeftButton) || !currentItem()) return;
    const QString payload = currentItem()->data(Qt::UserRole).toString();
    if (payload.isEmpty() || payload == QLatin1String("header")) return;
    if ((event->pos() - origin).manhattanLength() < QApplication::startDragDistance()) return;
    consumedDrag = true;
    auto* drag = new QDrag(this);
    auto* mime = new QMimeData();
    mime->setData("application/x-daedalcad", currentItem()->data(Qt::UserRole).toByteArray());
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction);
  }

 private:
  QPoint origin;
};


// Library tags: a tags.json next to the files maps relative path to a list of tags.
QHash<QString, QStringList> loadLibraryTags(const QString& rootPath) {
  QHash<QString, QStringList> tags;
  const QString path = QDir(rootPath).filePath(QStringLiteral("tags.json"));
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return tags;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
  if (!doc.isObject()) return tags;
  const QJsonObject obj = doc.object();
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    QStringList list;
    for (const QJsonValue& v : it.value().toArray()) list << v.toString();
    tags.insert(it.key(), list);
  }
  return tags;
}

bool saveLibraryTags(const QString& rootPath, const QHash<QString, QStringList>& tags) {
  QJsonObject obj;
  for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) {
    QJsonArray arr;
    for (const QString& t : it.value()) arr.append(t);
    obj.insert(it.key(), arr);
  }
  QFile file(QDir(rootPath).filePath(QStringLiteral("tags.json")));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
  return true;
}

QString relativeLibraryPath(const QString& root, const QString& absolute) {
  return QDir(root).relativeFilePath(absolute);
}

// The library folder a file belongs to (the deepest one that holds it), so its tags are kept in
// that folder's tags.json under the same name the list reads them by.
QString libraryRootFor(const QStringList& roots, const QString& file) {
  const QString clean = QDir::cleanPath(QFileInfo(file).absoluteFilePath());
  QString best;
  for (const QString& root : roots) {
    const QString r = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    if (!r.isEmpty() && clean.startsWith(r + QLatin1Char('/'), Qt::CaseInsensitive) && r.size() > best.size()) best = r;
  }
  return best.isEmpty() ? QFileInfo(clean).absolutePath() : best;
}

// Shows the items whose name or tags contain the text. Without text, every folder shows as it
// was left: open or folded.
void filterLibraryList(QListWidget* list, const QString& text) {
  if (!list) return;
  const QString wanted = text.trimmed().toLower();
  bool open = true;
  for (int i = 0; i < list->count(); ++i) {
    QListWidgetItem* item = list->item(i);
    const QString payload = item->data(Qt::UserRole).toString();
    if (payload == QLatin1String("header")) {
      open = item->data(Qt::UserRole + 1).toBool();
      item->setHidden(false);
      continue;
    }
    if (wanted.isEmpty()) {
      item->setHidden(!open);
      continue;
    }
    if (payload.isEmpty()) {  // "No files" and the like
      item->setHidden(true);
      continue;
    }
    bool match = item->text().trimmed().toLower().contains(wanted);
    for (const QString& tag : item->data(Qt::UserRole + 3).toStringList())
      if (!match && tag.toLower().contains(wanted)) match = true;
    item->setHidden(!match);
  }
}

// Asks for an item's tags and saves them in its library folder's tags.json. False when cancelled
// or not saved.
bool editLibraryTags(QWidget* parent, const QStringList& roots, const QString& file) {
  const QString root = libraryRootFor(roots, file);
  const QString rel = relativeLibraryPath(root, file);
  QHash<QString, QStringList> all = loadLibraryTags(root);
  bool ok = false;
  const QString text = QInputDialog::getText(parent, QStringLiteral("Edit tags"),
                                             QStringLiteral("Tags for %1 (comma separated)").arg(QFileInfo(file).fileName()),
                                             QLineEdit::Normal, all.value(rel).join(QStringLiteral(", ")), &ok);
  if (!ok) return false;
  QStringList tags;
  for (const QString& t : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    const QString trimmed = t.trimmed();
    if (!trimmed.isEmpty() && !tags.contains(trimmed, Qt::CaseInsensitive)) tags << trimmed;
  }
  if (tags.isEmpty()) all.remove(rel);
  else all.insert(rel, tags);
  return saveLibraryTags(root, all);
}

class LibraryItemDelegate : public QStyledItemDelegate {
 public:
  using QStyledItemDelegate::QStyledItemDelegate;
  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    if (index.data(Qt::UserRole).toString() != QLatin1String("header")) {
      QStyledItemDelegate::paint(painter, option, index);
      return;
    }
    painter->save();
    painter->fillRect(option.rect, (option.state & QStyle::State_Selected) ? QColor("#3a2414") : QColor("#2a1c10"));
    painter->setPen(QColor("#e87400"));
    QFont font = option.font;
    font.setBold(true);
    painter->setFont(font);
    painter->drawText(option.rect.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, index.data(Qt::DisplayRole).toString());
    painter->restore();
  }
};

namespace {

QString defaultAppdataRoot();
QString defaultAppdataPath(const QString& name);



QJsonArray vecJson(const double v[3]) { return QJsonArray{v[0], v[1], v[2]}; }

void vecRead(const QJsonValue& value, double out[3]) {
  const QJsonArray array = value.toArray();
  for (int i = 0; i < 3; ++i)
    if (i < array.size()) out[i] = array[i].toDouble(out[i]);
}

QJsonObject toJson(const Document& doc) {
  QJsonArray nodes;
  for (const Node& node : doc.nodes) {
    QJsonObject obj;
    obj["id"] = node.id;
    obj["parent"] = node.parentId;
    obj["name"] = node.name;
    obj["kind"] = node.kind == Node::Kind::Group ? "group" : node.kind == Node::Kind::Pattern ? "pattern" : "shape";
    obj["shape"] = node.shape;
    obj["hole"] = node.hole;
    if (node.clearanceMode != 0) {
      obj["clearanceMode"] = node.clearanceMode == 1 ? QStringLiteral("printer") : QStringLiteral("own");
      obj["holeClearance"] = node.holeClearance;
      obj["clearanceSidesOnly"] = node.clearanceSidesOnly;
    }
    obj["size"] = vecJson(node.size);
    obj["radius"] = node.radius;
    obj["radius2"] = node.radius2;
    obj["sides"] = node.sides;
    obj["segments"] = node.segments;
    if (node.shape == QLatin1String("diamond") && node.gemTable > 0) obj["gemTable"] = node.gemTable;
    if (!node.thread.isEmpty()) {
      obj["thread"] = node.thread;
      obj["threadPitch"] = node.threadPitch;
      obj["threadClearance"] = node.threadClearance;
    }
    obj["meshPath"] = node.meshPath;
    if (node.sculpt) obj["sculpt"] = true;
    if (!node.stepSource.isEmpty()) {
      obj["stepSource"] = node.stepSource;
      obj["stepIndex"] = node.stepIndex;
      obj["stepOffset"] = QJsonArray{node.stepOffset[0], node.stepOffset[1], node.stepOffset[2]};
    }
    if (!node.meshSource.isEmpty()) {
      obj["meshSource"] = node.meshSource;
      obj["meshLayer"] = node.meshLayer;
      if (node.meshLevel > 0) obj["meshLevel"] = node.meshLevel;
    }
    if (node.shape == QLatin1String("generator")) {
      obj["genType"] = node.genType;
      obj["genParams"] = node.genParams;
    }
    obj["pivotBaked"] = node.pivotBaked;
    obj["faceExtrude"] = node.faceExtrude;
    obj["extrudeDepth"] = node.extrudeDepth;
    obj["edge"] = node.edgeFillet;
    obj["surface"] = node.surfaceFillet;
    QJsonArray edgeKeys;
    for (int key : node.filletEdges) edgeKeys.append(key);
    obj["filletEdges"] = edgeKeys;
    if (!node.edgeBeta.empty()) {
      QJsonArray edgeBeta;
      for (double value : node.edgeBeta) edgeBeta.append(value);
      obj["edgeBeta"] = edgeBeta;
      obj["edgeBetaSegs"] = node.edgeBetaSegs;
    }
    obj["bend"] = node.bend;
    obj["along"] = node.along;
    if (std::abs(node.twist) > 0) {
      obj["twist"] = node.twist;
      obj["twistAxis"] = node.twistAxis;
    }
    if (std::abs(node.taper - 100) > 0.05) {
      obj["taper"] = node.taper;
      obj["taperAxis"] = node.taperAxis;
    }
    if (node.hull) {
      obj["hull"] = true;
      obj["hullMode"] = node.hullMode == 1 ? QStringLiteral("chain") : QStringLiteral("all");
    }
    if (node.intersect) obj["intersect"] = true;
    if (node.merged) obj["merged"] = true;
    if (node.blend > 0) obj["blend"] = node.blend;
    if (node.multiColor) obj["multiColor"] = true;
    if (node.shellWall > 0) {
      obj["shellWall"] = node.shellWall;
      obj["shellOpen"] = node.shellOpen;
    }
    obj["toward"] = node.toward;
    obj["faceBend"] = node.faceBend;
    obj["faceAngle"] = node.faceAngle;
    obj["faceRadius"] = node.faceRadius;
    obj["faceInnerRadius"] = node.faceInnerRadius;
    obj["faceSteps"] = node.faceSteps;
    obj["faceRise"] = node.faceRise;
    obj["faceSpiral"] = node.faceSpiral;
    obj["faceRadialStep"] = node.faceRadialStep;
    obj["faceAxisMode"] = node.faceAxisMode;
    obj["faceOrigin"] = vecJson(node.faceOrigin);
    obj["faceNormal"] = vecJson(node.faceNormal);
    obj["faceAxis"] = vecJson(node.faceAxis);
    obj["filletOf"] = node.filletOf;
    obj["filletOp"] = node.filletOp;
    obj["filletIndex"] = node.filletIndex;
    obj["filletOuter"] = node.filletOuter;
    obj["filletUp"] = node.filletUp;
    if (node.shape == QLatin1String("text")) {
      obj["text"] = node.text;
      obj["fontFamily"] = node.fontFamily;
      obj["fontStyle"] = node.fontStyle;
      obj["fontSize"] = node.fontSize;
      obj["letterSpacing"] = node.letterSpacing;
      obj["textAlign"] = node.textAlign;
      obj["textDetail"] = node.textDetail;
      if (node.textArc > 0.01) {
        obj["textArc"] = node.textArc;
        obj["textArcBelow"] = node.textArcBelow;
      }
    }
    QJsonArray loop;
    for (double value : node.faceLoop) loop.append(value);
    obj["faceLoop"] = loop;
    QJsonArray pointNormals;
    for (double value : node.facePointNormals) pointNormals.append(value);
    obj["facePointNormals"] = pointNormals;
    obj["mirror"] = QJsonArray{node.mirror[0], node.mirror[1], node.mirror[2]};
    obj["pos"] = vecJson(node.pos);
    obj["rot"] = vecJson(node.rot);
    obj["scale"] = vecJson(node.scale);
    obj["count"] = node.count;
    obj["move"] = vecJson(node.move);
    obj["drot"] = vecJson(node.drot);
    obj["resize"] = vecJson(node.resize);
    obj["color"] = node.color.name(QColor::HexRgb);
    nodes.append(obj);
  }
  QJsonObject root;
  root["version"] = 1;
  root["workplaneZ"] = doc.workplaneZ;
  root["bedX"] = doc.bedX;
  root["bedY"] = doc.bedY;
  root["gridOn"] = doc.gridOn;
  root["snap"] = doc.snap;
  root["snapOn"] = doc.snapOn;
  root["quality"] = doc.quality;
  root["filletSegments"] = doc.filletSegments;
  root["nodes"] = nodes;
  return root;
}

Document fromJson(const QJsonObject& root) {
  Document doc;
  doc.workplaneZ = root["workplaneZ"].toDouble(0);
  doc.bedX = root["bedX"].toDouble(doc.bedX);
  doc.bedY = root["bedY"].toDouble(doc.bedY);
  doc.gridOn = root["gridOn"].toBool(true);
  doc.snap = root["snap"].toDouble(1);
  doc.snapOn = root["snapOn"].toBool(true);
  doc.quality = root["quality"].toInt(128);
  doc.filletSegments = std::clamp(root["filletSegments"].toInt(12), 8, 72);
  for (const QJsonValue& value : root["nodes"].toArray()) {
    const QJsonObject obj = value.toObject();
    Node node;
    node.id = obj["id"].toString();
    node.parentId = obj["parent"].toString();
    node.name = obj["name"].toString("part");
    const QString kind = obj["kind"].toString("shape");
    node.kind = kind == "group" ? Node::Kind::Group : kind == "pattern" ? Node::Kind::Pattern : Node::Kind::Shape;
    node.shape = obj["shape"].toString("box");
    node.hole = obj["hole"].toBool(false);
    {
      const QString mode = obj["clearanceMode"].toString();
      node.clearanceMode = mode == QLatin1String("printer") ? 1 : mode == QLatin1String("own") ? 2 : 0;
      node.holeClearance = std::clamp(obj["holeClearance"].toDouble(0.2), 0.0, 2.0);
      node.clearanceSidesOnly = obj["clearanceSidesOnly"].toBool(false);
    }
    vecRead(obj["size"], node.size);
    node.radius = obj["radius"].toDouble(node.radius);
    node.radius2 = obj["radius2"].toDouble(node.radius2);
    node.sides = obj["sides"].toInt(node.sides);
    node.segments = obj.contains(QStringLiteral("segments"))
                        ? std::clamp(obj["segments"].toInt(doc.quality), 8, 256)
                        : std::clamp(doc.quality, 8, 256);
    node.meshPath = obj["meshPath"].toString();
    node.sculpt = obj["sculpt"].toBool(false);
    node.stepSource = obj["stepSource"].toString();
    node.stepIndex = obj["stepIndex"].toInt(-1);
    {
      const QJsonArray offset = obj["stepOffset"].toArray();
      for (int a = 0; a < 3 && a < offset.size(); ++a) node.stepOffset[a] = offset[a].toDouble();
    }
    node.gemTable = std::clamp(obj["gemTable"].toDouble(0), 0.0, 0.95);
    node.thread = obj["thread"].toString();
    node.threadPitch = std::clamp(obj["threadPitch"].toDouble(0), 0.0, 10.0);
    node.threadClearance = std::clamp(obj["threadClearance"].toDouble(0.2), 0.0, 2.0);
    node.meshSource = obj["meshSource"].toString();
    node.meshLayer = obj["meshLayer"].toInt(0);
    node.meshLevel = obj["meshLevel"].toInt(0);
    if (node.shape == QLatin1String("generator")) {
      node.genType = obj["genType"].toString();
      QJsonObject params = obj["genParams"].toObject();
      // Saved before "Use printer clearance" existed: keep the clearance that was set then.
      if (!params.contains(QStringLiteral("usePrinter"))) params.insert(QStringLiteral("usePrinter"), false);
      node.genParams = gen::normalized(node.genType, params);
    }
    node.pivotBaked = obj["pivotBaked"].toBool(false);
    node.faceExtrude = obj["faceExtrude"].toBool(false);
    node.extrudeDepth = obj["extrudeDepth"].toDouble(node.extrudeDepth);
    node.edgeFillet = obj["edge"].toDouble(0);
    node.surfaceFillet = obj["surface"].toDouble(0);
    node.edgeBeta.clear();
    for (const QJsonValue& value : obj["edgeBeta"].toArray()) node.edgeBeta.push_back(value.toDouble());
    node.edgeBetaSegs = std::clamp(obj["edgeBetaSegs"].toInt(12), 2, 48);
    node.filletEdges.clear();
    for (const QJsonValue& edgeValue : obj["filletEdges"].toArray()) {
      const int key = edgeValue.toInt(-1);
      if (key >= 0 && key < 512) node.filletEdges.push_back(key);
    }
    node.bend = obj["bend"].toDouble(0);
    node.along = obj["along"].toInt(0);
    node.twist = obj["twist"].toDouble(0);
    node.twistAxis = std::clamp(obj["twistAxis"].toInt(0), 0, 2);
    node.taper = std::clamp(obj["taper"].toDouble(100), 1.0, 1000.0);
    node.taperAxis = std::clamp(obj["taperAxis"].toInt(2), 0, 2);
    node.hull = node.kind == Node::Kind::Group && obj["hull"].toBool(false);
    node.hullMode = obj["hullMode"].toString() == QLatin1String("chain") ? 1 : 0;
    node.intersect = node.kind == Node::Kind::Group && !node.hull && obj["intersect"].toBool(false);
    node.merged = node.kind == Node::Kind::Group && obj["merged"].toBool(false);
    node.blend = node.kind == Node::Kind::Group ? std::clamp(obj["blend"].toDouble(0), 0.0, 100.0) : 0.0;
    node.multiColor = node.kind == Node::Kind::Group && obj["multiColor"].toBool(false);
    node.shellWall = std::clamp(obj["shellWall"].toDouble(0), 0.0, 1000.0);
    node.shellOpen = obj["shellOpen"].toInt(0) & 63;
    node.toward = obj["toward"].toInt(2);
    node.faceBend = obj["faceBend"].toBool(false);
    node.faceAngle = obj["faceAngle"].toDouble(node.faceAngle);
    node.faceRadius = obj["faceRadius"].toDouble(node.faceRadius);
    // Bends saved before the inside radius measured from the middle of the face: kept that way.
    node.faceInnerRadius = obj["faceInnerRadius"].toBool(false);
    node.faceSteps = obj["faceSteps"].toInt(node.faceSteps);
    node.faceRise = obj["faceRise"].toDouble(0);
    node.faceSpiral = obj["faceSpiral"].toBool(false);
    node.faceRadialStep = obj["faceRadialStep"].toDouble(0);
    node.faceAxisMode = obj["faceAxisMode"].toInt(0);
    vecRead(obj["faceOrigin"], node.faceOrigin);
    vecRead(obj["faceNormal"], node.faceNormal);
    vecRead(obj["faceAxis"], node.faceAxis);
    node.filletOf = obj["filletOf"].toString();
    node.filletOp = obj["filletOp"].toString();
    node.filletIndex = obj["filletIndex"].toInt(0);
    node.filletOuter = obj["filletOuter"].toBool(false);
    node.filletUp = obj["filletUp"].toBool(false);
    node.text = obj["text"].toString(node.text);
    node.fontFamily = obj["fontFamily"].toString();
    node.fontStyle = obj["fontStyle"].toString();
    node.fontSize = std::max(0.1, obj["fontSize"].toDouble(node.fontSize));
    node.letterSpacing = obj["letterSpacing"].toDouble(0);
    node.textAlign = std::clamp(obj["textAlign"].toInt(node.textAlign), 0, 2);
    node.textDetail = std::clamp(obj["textDetail"].toInt(node.textDetail), 0, textDetailCount() - 1);
    node.textArc = std::max(0.0, obj["textArc"].toDouble(0));
    node.textArcBelow = obj["textArcBelow"].toBool(false);
    node.faceLoop.clear();
    for (const QJsonValue& value : obj["faceLoop"].toArray()) node.faceLoop.push_back(value.toDouble());
    node.facePointNormals.clear();
    for (const QJsonValue& value : obj["facePointNormals"].toArray()) node.facePointNormals.push_back(value.toDouble());
    const QJsonArray mirror = obj["mirror"].toArray();
    for (int i = 0; i < 3 && i < mirror.size(); ++i) node.mirror[i] = mirror[i].toBool();
    vecRead(obj["pos"], node.pos);
    vecRead(obj["rot"], node.rot);
    vecRead(obj["scale"], node.scale);
    node.count = obj["count"].toInt(node.count);
    vecRead(obj["move"], node.move);
    vecRead(obj["drot"], node.drot);
    vecRead(obj["resize"], node.resize);
    node.color = QColor(obj["color"].toString("#d7c4a3"));
    if (!node.id.isEmpty()) doc.nodes.push_back(node);
  }
  return doc;
}

QIcon shapeIcon(const QString& key) {
  // Drawn at twice the size, so they stay sharp on high-resolution screens.
  QPixmap pix(64, 64);
  pix.setDevicePixelRatio(2.0);
  pix.fill(Qt::transparent);
  QPainter painter(&pix);
  painter.setRenderHint(QPainter::Antialiasing);
  const QColor ink("#e87400");
  const QColor dark("#12151a");
  painter.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(QColor(232, 116, 0, 90));
  // Three shades of the same orange: tops catch the light, the left side less, the right side least.
  auto shade = [](int alpha) { return QColor(232, 116, 0, alpha); };
  const int top = 165, left = 95, right = 45;
  auto face = [&](const QPolygonF& poly, int alpha) {
    painter.setBrush(shade(alpha));
    painter.drawPolygon(poly);
  };
  // Isometric view of a point (x right-front, y left-front, z up) around the icon's middle.
  auto iso = [](double x, double y, double z, double s = 7.0, double cy = 16.5) {
    return QPointF(16 + (x - y) * 0.866 * s, cy + (x + y) * 0.5 * s - z * s);
  };
  // A polygon with its corners rounded (for the round box).
  auto rounded = [](const QPolygonF& poly, double cut) {
    QPainterPath path;
    const int n = poly.size();
    for (int i = 0; i < n; ++i) {
      const QPointF prev = poly[(i + n - 1) % n], cur = poly[i], next = poly[(i + 1) % n];
      const QPointF a = cur + (prev - cur) * cut, b = cur + (next - cur) * cut;
      if (i == 0) path.moveTo(a);
      else path.lineTo(a);
      path.quadTo(cur, b);
    }
    path.closeSubpath();
    return path;
  };
  auto shaded = [&](const QRectF& box) {
    QLinearGradient g(box.topLeft(), box.topRight());
    g.setColorAt(0, shade(150));
    g.setColorAt(1, shade(40));
    return QBrush(g);
  };
  auto starPolygon = [](QPointF c, double outer, double inner, double squash) {
    QPolygonF poly;
    for (int i = 0; i < 10; ++i) {
      const double a = -3.14159265 / 2 + 3.14159265 * i / 5;
      const double r = (i % 2 == 0) ? outer : inner;
      poly << QPointF(c.x() + r * std::cos(a), c.y() + r * std::sin(a) * squash);
    }
    return poly;
  };

  if (key == "box" || key == "roundbox") {
    const QPolygonF topFace{iso(-1, -1, 1), iso(1, -1, 1), iso(1, 1, 1), iso(-1, 1, 1)};
    const QPolygonF rightFace{iso(1, -1, 1), iso(1, 1, 1), iso(1, 1, -1), iso(1, -1, -1)};
    const QPolygonF leftFace{iso(1, 1, 1), iso(-1, 1, 1), iso(-1, 1, -1), iso(1, 1, -1)};
    if (key == "box") {
      face(rightFace, right);
      face(leftFace, left);
      face(topFace, top);
    } else {
      for (const auto& [poly, alpha] : {std::pair{rightFace, right}, std::pair{leftFace, left}, std::pair{topFace, top}}) {
        painter.setBrush(shade(alpha));
        painter.drawPath(rounded(poly, 0.28));
      }
    }
  } else if (key == "sphere") {
    QRadialGradient g(QPointF(12, 11), 15);
    g.setColorAt(0, shade(190));
    g.setColorAt(1, shade(45));
    painter.setBrush(g);
    painter.drawEllipse(QPointF(16, 16), 12, 12);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, 0.9));
    painter.drawArc(QRectF(4, 12.5, 24, 7), 180 * 16, 180 * 16);
  } else if (key == "hemisphere") {
    QPainterPath dome;
    dome.moveTo(4, 21);
    dome.arcTo(QRectF(4, 9, 24, 24), 180, -180);
    dome.arcTo(QRectF(4, 18, 24, 6), 0, -180);
    QRadialGradient g(QPointF(12, 13), 15);
    g.setColorAt(0, shade(190));
    g.setColorAt(1, shade(45));
    painter.setBrush(g);
    painter.drawPath(dome);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, 0.9));
    painter.drawArc(QRectF(4, 18, 24, 6), 0, 180 * 16);
  } else if (key == "cylinder" || key == "tube") {
    QPainterPath body;
    body.moveTo(8, 8);
    body.lineTo(8, 25);
    body.arcTo(QRectF(8, 22, 16, 6), 180, 180);
    body.lineTo(24, 8);
    painter.setBrush(shaded(QRectF(8, 8, 16, 20)));
    painter.drawPath(body);
    painter.setBrush(shade(top));
    painter.drawEllipse(QRectF(8, 5, 16, 6));
    if (key == "tube") {
      painter.setBrush(dark);
      painter.drawEllipse(QRectF(12, 6.6, 8, 2.8));
    }
  } else if (key == "capsule") {
    QPainterPath body;
    body.addRoundedRect(QRectF(9, 3, 14, 26), 7, 7);
    painter.setBrush(shaded(QRectF(9, 3, 14, 26)));
    painter.drawPath(body);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, 0.9));
    painter.drawArc(QRectF(9, 8, 14, 4), 180 * 16, 180 * 16);
    painter.drawArc(QRectF(9, 20, 14, 4), 180 * 16, 180 * 16);
  } else if (key == "cone") {
    QPainterPath body;
    body.moveTo(16, 4);
    body.lineTo(5, 24);
    body.arcTo(QRectF(5, 21, 22, 6), 180, 180);
    body.closeSubpath();
    painter.setBrush(shaded(QRectF(5, 4, 22, 23)));
    painter.drawPath(body);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, 0.9));
    painter.drawArc(QRectF(5, 21, 22, 6), 0, 180 * 16);
  } else if (key == "torus") {
    QRadialGradient g(QPointF(13, 13), 16);
    g.setColorAt(0, shade(180));
    g.setColorAt(1, shade(45));
    painter.setBrush(g);
    painter.drawEllipse(QRectF(3, 8, 26, 17));
    painter.setBrush(dark);
    painter.drawEllipse(QRectF(11, 13.5, 10, 5));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, 0.9));
    painter.drawArc(QRectF(9, 11.5, 14, 9), 20 * 16, 140 * 16);
  } else if (key == "diamond") {
    // Seen from the side, faceted: upper facets light, lower ones darker.
    const QPointF up(16, 3), down(16, 29), l(4, 14), r(28, 14), mid(16, 16.5);
    face(QPolygonF{up, l, mid}, top);
    face(QPolygonF{up, mid, r}, left + 20);
    face(QPolygonF{l, mid, down}, left);
    face(QPolygonF{mid, r, down}, right);
  } else if (key == "heart") {
    auto heart = [](double dx, double dy) {
      QPainterPath path;
      path.moveTo(16 + dx, 26 + dy);
      path.cubicTo(3 + dx, 18 + dy, 4 + dx, 7 + dy, 10 + dx, 7 + dy);
      path.cubicTo(14 + dx, 7 + dy, 16 + dx, 11 + dy, 16 + dx, 11 + dy);
      path.cubicTo(16 + dx, 11 + dy, 18 + dx, 7 + dy, 22 + dx, 7 + dy);
      path.cubicTo(28 + dx, 7 + dy, 29 + dx, 18 + dy, 16 + dx, 26 + dy);
      return path;
    };
    painter.setBrush(shade(right + 10));
    painter.drawPath(heart(1.5, 2.5));
    painter.setBrush(shade(top));
    painter.drawPath(heart(0, 0));
  } else if (key == "prism") {
    // A hexagonal prism seen from above at an angle.
    std::vector<QPointF> topRing, bottomRing;
    for (int i = 0; i < 6; ++i) {
      const double a = (i * 60.0) * 3.14159265 / 180.0;
      topRing.push_back(QPointF(16 + 12 * std::cos(a), 10 + 6 * std::sin(a)));
      bottomRing.push_back(QPointF(16 + 12 * std::cos(a), 23 + 6 * std::sin(a)));
    }
    // The three faces toward the viewer (between corners 0-1, 1-2, 2-3: the front half).
    const int alphas[3] = {right, left + 15, left};
    for (int i = 0; i < 3; ++i)
      face(QPolygonF{topRing[i], topRing[i + 1], bottomRing[i + 1], bottomRing[i]}, alphas[i]);
    QPolygonF cap;
    for (const QPointF& p : topRing) cap << p;
    face(cap, top);
  } else if (key == "pyramid") {
    const QPointF apex = iso(0, 0, 1.6, 7.5, 19);
    face(QPolygonF{apex, iso(1, -1, -1, 7.5, 19), iso(1, 1, -1, 7.5, 19)}, right + 25);
    face(QPolygonF{apex, iso(1, 1, -1, 7.5, 19), iso(-1, 1, -1, 7.5, 19)}, left + 30);
  } else if (key == "wedge") {
    // A ramp: high at the back, down to nothing at the front.
    const double h = 1.4;
    face(QPolygonF{iso(-1, 1, -1, 7, 18), iso(1, 1, -1, 7, 18), iso(-1, 1, h, 7, 18)}, left);
    face(QPolygonF{iso(-1, -1, h, 7, 18), iso(1, -1, -1, 7, 18), iso(1, 1, -1, 7, 18), iso(-1, 1, h, 7, 18)}, top - 20);
  } else if (key == "roof") {
    // A gable roof seen from the front, a little from above: the gable triangle, and both slopes
    // running back to the ridge.
    const QPointF depth(4.5, -7.5);
    const QPointF a(4, 27), b(23, 27), c(13.5, 15.5);
    face(QPolygonF{a, c, c + depth, a + depth}, top);
    face(QPolygonF{c, b, b + depth, c + depth}, right + 10);
    face(QPolygonF{a, b, c}, left);
  } else if (key == "star") {
    // Facets from the points and the notches up to the peak, light and dark in turn.
    const QPolygonF outline = starPolygon(QPointF(16, 17), 14, 6, 0.92);
    const QPointF peak(16, 15.5);
    for (int i = 0; i < 10; ++i)
      face(QPolygonF{peak, outline[i], outline[(i + 1) % 10]}, (i % 2 == 0) ? top : right + 15);
  } else if (key == "starflat") {
    const QPolygonF back = starPolygon(QPointF(16, 19.5), 14, 6, 0.8);
    const QPolygonF front = starPolygon(QPointF(16, 15.5), 14, 6, 0.8);
    face(back, right + 10);
    // The edges down from the points that face the viewer.
    painter.setPen(QPen(ink, 1.0));
    for (int i : {2, 4, 6, 8, 3, 7}) painter.drawLine(front[i], back[i]);
    painter.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    face(front, top);
  } else if (key == "text") {
    QFont font(QStringLiteral("Serif"));
    font.setPixelSize(26);
    font.setBold(true);
    QPainterPath path;
    path.addText(0, 0, font, QStringLiteral("T"));
    const QRectF box = path.boundingRect();
    const QPainterPath glyph = path.translated(16 - box.center().x(), 16 - box.center().y());
    painter.setBrush(shade(right + 10));
    painter.drawPath(glyph.translated(1.6, 2.2));
    painter.setBrush(shade(top));
    painter.drawPath(glyph);
  } else if (key == "2d") {
    painter.drawLine(QPointF(7, 24), QPointF(13, 8));
    painter.drawLine(QPointF(13, 8), QPointF(25, 18));
    painter.drawLine(QPointF(25, 18), QPointF(7, 24));
    painter.drawEllipse(QPointF(7, 24), 2.2, 2.2);
    painter.drawEllipse(QPointF(13, 8), 2.2, 2.2);
    painter.drawEllipse(QPointF(25, 18), 2.2, 2.2);
  } else if (key == "stl") {
    QPolygonF front;
    front << QPointF(7, 10) << QPointF(19, 6) << QPointF(26, 11) << QPointF(14, 15);
    QPolygonF side;
    side << QPointF(14, 15) << QPointF(26, 11) << QPointF(26, 24) << QPointF(14, 28);
    QPolygonF face;
    face << QPointF(7, 10) << QPointF(14, 15) << QPointF(14, 28) << QPointF(7, 23);
    painter.drawPolygon(front);
    painter.drawPolygon(side);
    painter.drawPolygon(face);
  } else if (key.startsWith(QLatin1String("gen:"))) {
    const QString g = key.mid(4);
    const QColor dark("#12151a");
    auto hexagon = [](QPointF c, double r, double start) {
      QPolygonF poly;
      for (int i = 0; i < 6; ++i) {
        const double a = (start + 60.0 * i) * 3.14159265 / 180.0;
        poly << QPointF(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
      }
      return poly;
    };
    if (g == "box") {
      painter.drawRoundedRect(QRectF(6, 13, 20, 14), 2, 2);
      painter.drawRoundedRect(QRectF(5, 6, 22, 4.5), 1.5, 1.5);
      painter.drawLine(QPointF(9, 10.5), QPointF(9, 13));
      painter.drawLine(QPointF(23, 10.5), QPointF(23, 13));
    } else if (g == "gridbin") {
      painter.drawRoundedRect(QRectF(5, 7, 22, 15), 2, 2);
      QPolygonF left, right;
      left << QPointF(6, 22) << QPointF(15, 22) << QPointF(13.5, 27) << QPointF(7.5, 27);
      right << QPointF(17, 22) << QPointF(26, 22) << QPointF(24.5, 27) << QPointF(18.5, 27);
      painter.drawPolygon(left);
      painter.drawPolygon(right);
      painter.setBrush(dark);
      painter.drawRect(QRectF(8, 10, 16, 9));
    } else if (g == "gridplate") {
      painter.drawRoundedRect(QRectF(4, 4, 24, 24), 2, 2);
      painter.setBrush(dark);
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) painter.drawRoundedRect(QRectF(6 + i * 7.2, 6 + j * 7.2, 5.6, 5.6), 1, 1);
    } else if (g == "jar") {
      painter.drawRoundedRect(QRectF(8, 12, 16, 16), 3, 3);
      painter.drawRoundedRect(QRectF(6.5, 5, 19, 6), 1.5, 1.5);
      for (double x : {10.0, 14.0, 18.0, 22.0}) painter.drawLine(QPointF(x, 6), QPointF(x, 10));
    } else if (g == "bolt") {
      painter.drawPolygon(QPolygonF{QPointF(9, 4), QPointF(23, 4), QPointF(24.5, 7), QPointF(23, 10), QPointF(9, 10), QPointF(7.5, 7)});
      painter.drawRect(QRectF(12.5, 10, 7, 18));
      for (int i = 0; i < 6; ++i) painter.drawLine(QPointF(12.5, 13 + i * 2.6), QPointF(19.5, 11.6 + i * 2.6));
    } else if (g == "nut") {
      painter.drawPolygon(hexagon(QPointF(16, 16), 12, 0));
      painter.setBrush(dark);
      painter.drawEllipse(QPointF(16, 16), 5, 5);
    } else if (g == "stdscrew") {
      // A socket head cap screw.
      painter.drawRoundedRect(QRectF(9, 3.5, 14, 8), 1.5, 1.5);
      painter.drawRect(QRectF(12.5, 11.5, 7, 17));
      for (int i = 0; i < 6; ++i) painter.drawLine(QPointF(12.5, 14 + i * 2.5), QPointF(19.5, 12.7 + i * 2.5));
      painter.setBrush(dark);
      painter.drawPolygon(hexagon(QPointF(16, 7.5), 2.6, 0));
    } else if (g == "stdnut") {
      painter.drawPolygon(hexagon(QPointF(16, 16), 12, 30));
      painter.setBrush(Qt::NoBrush);
      painter.drawEllipse(QPointF(16, 16), 9, 9);
      painter.setBrush(dark);
      painter.drawEllipse(QPointF(16, 16), 4.5, 4.5);
    } else if (g == "stdwasher") {
      painter.drawEllipse(QPointF(16, 16), 12.5, 12.5);
      painter.setBrush(dark);
      painter.drawEllipse(QPointF(16, 16), 5.5, 5.5);
    } else if (g == "stdpin") {
      painter.drawPolygon(QPolygonF{QPointF(13, 4), QPointF(19, 4), QPointF(20.5, 6), QPointF(20.5, 26), QPointF(19, 28),
                                    QPointF(13, 28), QPointF(11.5, 26), QPointF(11.5, 6)});
    } else if (g == "stdrod") {
      painter.drawRect(QRectF(12.5, 3, 7, 26));
      for (int i = 0; i < 10; ++i) painter.drawLine(QPointF(12.5, 5 + i * 2.5), QPointF(19.5, 3.7 + i * 2.5));
    } else if (g == "stdinsert") {
      painter.drawRect(QRectF(9, 5, 14, 22));
      for (double y : {7.0, 17.0})
        for (int i = 0; i < 4; ++i) painter.drawLine(QPointF(9 + i * 4, y), QPointF(11 + i * 4, y + 8));
      painter.setBrush(dark);
      painter.drawRect(QRectF(13.5, 5, 5, 22));
    } else if (g == "standoff") {
      painter.drawRect(QRectF(3.5, 25, 25, 3.5));
      painter.drawRect(QRectF(7, 10, 6, 15));
      painter.drawRect(QRectF(19, 10, 6, 15));
      painter.setBrush(dark);
      painter.drawRect(QRectF(9, 10, 2, 7));
      painter.drawRect(QRectF(21, 10, 2, 7));
    } else if (g == "gear") {
      QPolygonF poly;
      const int teeth = 10;
      for (int i = 0; i < teeth * 4; ++i) {
        const double a = (i / (teeth * 4.0)) * 2 * 3.14159265;
        const double r = (i % 4 == 0 || i % 4 == 1) ? 13 : 10;
        poly << QPointF(16 + r * std::cos(a), 16 + r * std::sin(a));
      }
      painter.drawPolygon(poly);
      painter.setBrush(dark);
      painter.drawEllipse(QPointF(16, 16), 3.5, 3.5);
    } else if (g == "hinge") {
      painter.drawRect(QRectF(3.5, 7, 10, 18));
      painter.drawRect(QRectF(18.5, 7, 10, 18));
      for (int i = 0; i < 5; ++i) painter.drawRoundedRect(QRectF(13, 7 + i * 3.6, 6, 3.2), 1.5, 1.5);
      painter.setBrush(dark);
      painter.drawEllipse(QPointF(8.5, 12), 1.6, 1.6);
      painter.drawEllipse(QPointF(8.5, 20), 1.6, 1.6);
      painter.drawEllipse(QPointF(23.5, 12), 1.6, 1.6);
      painter.drawEllipse(QPointF(23.5, 20), 1.6, 1.6);
    } else if (g == "enclosure") {
      painter.drawRoundedRect(QRectF(4, 10, 24, 17), 2, 2);
      painter.drawRoundedRect(QRectF(3, 6, 26, 4), 1.5, 1.5);
      painter.setBrush(dark);
      painter.drawRoundedRect(QRectF(8, 18, 6, 3), 1.5, 1.5);
      for (double x : {18.0, 21.0, 24.0}) painter.drawRect(QRectF(x, 14, 1.4, 8));
    } else if (g == "battery") {
      for (double x : {5.0, 13.0, 21.0}) {
        painter.drawRoundedRect(QRectF(x, 9, 6.5, 19), 1.5, 1.5);
        painter.drawRect(QRectF(x + 2, 6, 2.5, 3));
      }
    } else if (g == "rack") {
      QPolygonF rack;
      rack << QPointF(3, 28) << QPointF(3, 22);
      for (int i = 0; i < 5; ++i) {
        const double x = 3 + i * 5.2;
        rack << QPointF(x + 0.8, 22) << QPointF(x + 1.8, 19) << QPointF(x + 3.4, 19) << QPointF(x + 4.4, 22);
      }
      rack << QPointF(29, 22) << QPointF(29, 28);
      painter.drawPolygon(rack);
      QPolygonF gear;
      for (int i = 0; i < 32; ++i) {
        const double a = (i / 32.0) * 2 * 3.14159265;
        const double r = (i % 4 == 0 || i % 4 == 1) ? 8 : 6;
        gear << QPointF(16 + r * std::cos(a), 10 + r * std::sin(a));
      }
      painter.drawPolygon(gear);
      painter.setBrush(dark);
      painter.drawEllipse(QPointF(16, 10), 2, 2);
    } else if (g == "balljoint") {
      painter.drawEllipse(QPointF(16, 10), 6.5, 6.5);
      painter.drawRect(QRectF(13.5, 16, 5, 7));
      painter.drawRoundedRect(QRectF(7, 23, 18, 5), 1.5, 1.5);
      painter.setBrush(Qt::NoBrush);
      painter.drawArc(QRectF(7.5, 1.5, 17, 17), 200 * 16, 140 * 16);
    } else if (g == "hose") {
      QPolygonF body;
      body << QPointF(11, 28) << QPointF(11, 20) << QPointF(6, 14) << QPointF(6, 4) << QPointF(26, 4) << QPointF(26, 14)
           << QPointF(21, 20) << QPointF(21, 28);
      painter.drawPolygon(body);
      for (double y : {22.0, 25.0}) painter.drawLine(QPointF(10, y), QPointF(22, y));
      painter.setBrush(dark);
      painter.drawRect(QRectF(9, 4, 14, 8));
    } else if (g == "honeycomb") {
      painter.drawRoundedRect(QRectF(3.5, 4, 25, 24), 2, 2);
      painter.setBrush(dark);
      painter.drawPolygon(hexagon(QPointF(10.5, 10.5), 4, 0));
      painter.drawEllipse(QPointF(21.5, 10.5), 3.8, 3.8);
      painter.drawRect(QRectF(7, 18, 7, 7));
      QPolygonF star;
      for (int i = 0; i < 10; ++i) {
        const double a = -3.14159265 / 2 + 3.14159265 * i / 5;
        const double r = (i % 2 == 0) ? 4.6 : 2.0;
        star << QPointF(21.5 + r * std::cos(a), 21.8 + r * std::sin(a));
      }
      painter.drawPolygon(star);
    } else if (g == "patterntube") {
      QPolygonF pot;
      pot << QPointF(8, 28) << QPointF(5, 4) << QPointF(27, 4) << QPointF(24, 28);
      painter.drawPolygon(pot);
      painter.setBrush(dark);
      for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
          const double x = 10.5 + col * 5.5 + (row % 2 ? 2.75 : 0) - row * 0.5, y = 9 + row * 5.5;
          if (row % 2 && col == 2) continue;
          painter.drawPolygon(hexagon(QPointF(x, y), 2.2, 30));
        }
    } else if (g == "fittest") {
      painter.drawRoundedRect(QRectF(3, 10, 26, 13), 2, 2);
      painter.setBrush(dark);
      painter.drawRect(QRectF(6, 14, 4, 4));
      painter.drawRect(QRectF(13.5, 13.75, 4.5, 4.5));
      painter.drawRect(QRectF(21, 13.5, 5, 5));
    } else if (g == "pulley") {
      painter.drawRoundedRect(QRectF(5, 5, 22, 3), 1, 1);
      painter.drawRoundedRect(QRectF(5, 24, 22, 3), 1, 1);
      painter.drawRect(QRectF(8, 8, 16, 16));
      for (double x : {10.5, 13.5, 16.5, 19.5, 22.5}) painter.drawLine(QPointF(x, 8), QPointF(x, 24));
    } else if (g == "bearing") {
      painter.drawEllipse(QPointF(16, 16), 13, 13);
      painter.setBrush(dark);
      painter.drawEllipse(QPointF(16, 16), 7, 7);
      for (int i = 0; i < 3; ++i) {
        const double a = -3.14159265 / 2 + 2 * 3.14159265 * i / 3;
        painter.drawEllipse(QPointF(16 + 10.3 * std::cos(a), 16 + 10.3 * std::sin(a)), 1.4, 1.4);
      }
      painter.setBrush(Qt::NoBrush);
      painter.drawEllipse(QPointF(16, 16), 4, 4);
    } else if (g == "coupler") {
      painter.drawRoundedRect(QRectF(9, 3.5, 14, 25), 2, 2);
      painter.setBrush(dark);
      painter.drawRect(QRectF(13.5, 3.5, 5, 11));
      painter.drawRect(QRectF(12, 17.5, 8, 11));
      painter.drawEllipse(QPointF(20.5, 9), 1.3, 1.3);
      painter.drawEllipse(QPointF(20.5, 23), 1.3, 1.3);
    } else if (g == "spring") {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(ink, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      for (int i = 0; i < 6; ++i) painter.drawArc(QRectF(6, 4 + i * 4, 20, 5), 0, 180 * 16);
      painter.drawLine(QPointF(6, 6.5), QPointF(26, 6.5));
      painter.drawLine(QPointF(6, 26.5), QPointF(26, 26.5));
    } else if (g == "snapfit") {
      painter.drawRect(QRectF(3, 8, 6, 16));
      painter.drawRect(QRectF(9, 15, 15, 3));
      painter.drawPolygon(QPolygonF{QPointF(20, 15), QPointF(20, 10), QPointF(27, 15)});
      painter.drawRect(QRectF(14, 4, 3.5, 9));
      painter.drawRect(QRectF(14, 19.5, 3.5, 9));
    } else if (g == "livinghinge") {
      painter.drawRect(QRectF(3, 13, 12, 6));
      painter.drawRect(QRectF(17, 13, 12, 6));
      painter.drawRect(QRectF(14, 17.5, 4, 1.5));
    } else if (g == "cablechain") {
      for (int i = 0; i < 3; ++i) painter.drawRoundedRect(QRectF(2 + i * 9, 9 + i * 2, 12, 9), 4.5, 4.5);
      painter.setBrush(dark);
      for (int i = 0; i < 3; ++i) painter.drawEllipse(QPointF(6.5 + i * 9, 13.5 + i * 2), 1.4, 1.4);
    } else if (g == "tubeconnector") {
      painter.drawRect(QRectF(11, 11, 10, 10));
      painter.drawRect(QRectF(2, 12.5, 9, 7));
      painter.drawRect(QRectF(21, 12.5, 9, 7));
      painter.drawRect(QRectF(12.5, 2, 7, 9));
      painter.setBrush(dark);
      painter.drawRect(QRectF(2, 14, 2, 4));
      painter.drawRect(QRectF(28, 14, 2, 4));
      painter.drawRect(QRectF(14, 2, 4, 2));
    } else if (g == "toolholder") {
      painter.drawRoundedRect(QRectF(3, 8, 26, 16), 2, 2);
      painter.setBrush(dark);
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 2; ++j) painter.drawPolygon(hexagon(QPointF(7.5 + i * 5.7, 12.5 + j * 7), 2.3, 30));
    } else if (g == "dividers") {
      painter.drawRect(QRectF(4, 6, 24, 20));
      painter.drawLine(QPointF(12, 6), QPointF(12, 26));
      painter.drawLine(QPointF(20, 6), QPointF(20, 26));
      painter.drawLine(QPointF(4, 16), QPointF(28, 16));
    } else if (g == "cable") {
      painter.drawRect(QRectF(2, 21, 28, 4));
      painter.drawRoundedRect(QRectF(9, 9, 14, 16), 6, 6);
      painter.setBrush(dark);
      painter.drawEllipse(QPointF(16, 18), 4.2, 4.2);
      painter.drawEllipse(QPointF(5, 23), 1.2, 1.2);
      painter.drawEllipse(QPointF(27, 23), 1.2, 1.2);
    } else if (g == "nametag") {
      painter.drawRoundedRect(QRectF(3, 9, 26, 14), 3, 3);
      painter.setBrush(dark);
      painter.drawEllipse(QPointF(7.5, 16), 2, 2);
      QFont font(QStringLiteral("Sans"));
      font.setPixelSize(11);
      font.setBold(true);
      QPainterPath path;
      path.addText(0, 0, font, QStringLiteral("Ab"));
      const QRectF box = path.boundingRect();
      painter.setBrush(ink);
      painter.setPen(Qt::NoPen);
      painter.drawPath(path.translated(19 - box.center().x(), 16 - box.center().y()));
    } else if (g == "dice") {
      // A die, seen a little from above: three faces with pips.
      QPolygonF top{QPointF(16, 4), QPointF(27, 9), QPointF(16, 14), QPointF(5, 9)};
      QPolygonF left{QPointF(5, 9), QPointF(16, 14), QPointF(16, 28), QPointF(5, 22)};
      QPolygonF right{QPointF(16, 14), QPointF(27, 9), QPointF(27, 22), QPointF(16, 28)};
      painter.drawPolygon(top);
      painter.drawPolygon(left);
      painter.drawPolygon(right);
      painter.setBrush(dark);
      painter.setPen(Qt::NoPen);
      painter.drawEllipse(QPointF(16, 9), 1.6, 1.1);
      painter.drawEllipse(QPointF(8.5, 14), 1.1, 1.4);
      painter.drawEllipse(QPointF(12.5, 22), 1.1, 1.4);
      for (const QPointF& p : {QPointF(19.5, 16.5), QPointF(23.5, 14.5), QPointF(19.5, 24), QPointF(23.5, 22)}) painter.drawEllipse(p, 1.1, 1.4);
    } else if (g == "lithophane") {
      // A framed picture: a sun over hills.
      painter.drawRoundedRect(QRectF(4, 6, 24, 20), 2, 2);
      painter.setBrush(dark);
      painter.drawEllipse(QPointF(21, 12), 2.6, 2.6);
      QPainterPath hills;
      hills.moveTo(6, 23);
      hills.cubicTo(10, 15, 14, 17, 17, 20);
      hills.cubicTo(20, 17, 23, 16, 26, 19);
      hills.lineTo(26, 24);
      hills.lineTo(6, 24);
      hills.closeSubpath();
      painter.drawPath(hills);
    } else if (g == "holetest") {
      painter.drawRoundedRect(QRectF(2, 10, 28, 12), 2, 2);
      painter.setBrush(dark);
      double x = 5;
      for (double r : {1.0, 1.6, 2.3, 3.0}) {
        painter.drawEllipse(QPointF(x + r, 16), r, r);
        x += 2 * r + 2.2;
      }
    } else if (g == "threadtest") {
      painter.drawRoundedRect(QRectF(2, 12, 20, 12), 2, 2);
      painter.setBrush(dark);
      painter.drawEllipse(QPointF(8, 18), 3, 3);
      painter.drawEllipse(QPointF(16, 18), 3.3, 3.3);
      painter.setBrush(shade(right + 10));
      painter.drawRect(QRectF(23, 6, 6, 18));
      for (int i = 0; i < 5; ++i) painter.drawLine(QPointF(23, 9 + i * 3), QPointF(29, 7.5 + i * 3));
    } else if (g == "overhang") {
      painter.drawRect(QRectF(2, 24, 28, 3));
      painter.drawPolygon(QPolygonF{QPointF(4, 24), QPointF(7, 24), QPointF(10, 10), QPointF(7, 10)});
      painter.drawPolygon(QPolygonF{QPointF(12, 24), QPointF(15, 24), QPointF(22, 12), QPointF(19, 12)});
      painter.drawPolygon(QPolygonF{QPointF(22, 24), QPointF(25, 24), QPointF(30, 17), QPointF(27, 17)});
    } else {
      painter.drawRoundedRect(QRectF(6, 8, 20, 16), 2, 2);
    }
  } else {
    painter.drawRoundedRect(QRectF(6, 8, 20, 16), 2, 2);
  }
  return QIcon(pix);
}

QIcon toolIcon(const QString& name) {
  QString key = name.toLower();
  QString stem = key;
  stem.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
  // "Edge fillet (Beta)" -> edge_fillet_beta, not edge_fillet_beta_
  while (stem.endsWith(QLatin1Char('_'))) stem.chop(1);
  while (stem.startsWith(QLatin1Char('_'))) stem.remove(0, 1);
  const bool badge = key == QLatin1String("2d shape") || key == QLatin1String("sculpt");
  const QString appIcons = resourcePath(QStringLiteral("icons"));
  const QString legacyIcons = QCoreApplication::applicationDirPath() + QDir::separator() + QStringLiteral("icons");
  const QStringList svgCandidates = {appIcons + QDir::separator() + stem + QStringLiteral(".svg"),
                                    appIcons + QDir::separator() + key + QStringLiteral(".svg"),
                                    legacyIcons + QDir::separator() + stem + QStringLiteral(".svg"),
                                    legacyIcons + QDir::separator() + key + QStringLiteral(".svg")};
  const qreal dpr = [] {
    if (const QScreen* screen = QGuiApplication::primaryScreen()) return std::max(screen->devicePixelRatio(), 1.0);
    return 1.0;
  }();
  auto compose = [&](const QIcon& source) {
    constexpr int slot = 32;
    const int glyph = badge ? 20 : 28;
    QPixmap canvas(qRound(slot * dpr), qRound(slot * dpr));
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(badge ? QColor("#e87400") : Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRect target((slot - glyph) / 2, (slot - glyph) / 2, glyph, glyph);
    if (badge) {
      QPixmap raw(qRound(glyph * dpr), qRound(glyph * dpr));
      raw.setDevicePixelRatio(dpr);
      raw.fill(Qt::transparent);
      {
        QPainter glyphPainter(&raw);
        glyphPainter.setRenderHint(QPainter::Antialiasing);
        source.paint(&glyphPainter, QRect(0, 0, glyph, glyph));
      }
      QPainter tint(&raw);
      tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
      tint.fillRect(raw.rect(), Qt::black);
      painter.drawPixmap(target, raw);
    } else {
      source.paint(&painter, target);
    }
    QIcon icon;
    icon.addPixmap(canvas);
    return icon;
  };
  for (const QString& candidate : svgCandidates) {
    if (!QFileInfo::exists(candidate)) continue;
    const QIcon source(candidate);
    if (source.isNull()) continue;
    return compose(source);
  }
  const QStringList pngCandidates = {appIcons + QDir::separator() + stem + QStringLiteral(".png"),
                                     appIcons + QDir::separator() + key + QStringLiteral(".png"),
                                     legacyIcons + QDir::separator() + stem + QStringLiteral(".png"),
                                     legacyIcons + QDir::separator() + key + QStringLiteral(".png")};
  for (const QString& candidate : pngCandidates) {
    if (!QFileInfo::exists(candidate)) continue;
    const QIcon icon(candidate);
    if (icon.isNull()) continue;
    return compose(icon);
  }
  QPixmap pix(qRound(32 * dpr), qRound(32 * dpr));
  pix.setDevicePixelRatio(dpr);
  pix.fill(badge ? QColor("#e87400") : Qt::transparent);
  QPainter painter(&pix);
  painter.setRenderHint(QPainter::Antialiasing);
  QPen pen(badge ? QColor("#000000") : QColor("#e87400"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  if (key == "move") {
    painter.drawLine(QPointF(16, 6), QPointF(16, 26));
    painter.drawLine(QPointF(6, 16), QPointF(26, 16));
    painter.drawLine(QPointF(16, 6), QPointF(12, 10));
    painter.drawLine(QPointF(16, 6), QPointF(20, 10));
    painter.drawLine(QPointF(16, 26), QPointF(12, 22));
    painter.drawLine(QPointF(16, 26), QPointF(20, 22));
    painter.drawLine(QPointF(6, 16), QPointF(10, 12));
    painter.drawLine(QPointF(6, 16), QPointF(10, 20));
    painter.drawLine(QPointF(26, 16), QPointF(22, 12));
    painter.drawLine(QPointF(26, 16), QPointF(22, 20));
  } else if (key == "rotate") {
    painter.drawArc(QRectF(7, 7, 18, 18), 40 * 16, 280 * 16);
    painter.drawLine(QPointF(23, 9), QPointF(26, 6));
    painter.drawLine(QPointF(23, 9), QPointF(18, 7));
  } else if (key == "bend face") {
    painter.setBrush(QColor(232, 116, 0, 70));
    painter.drawRect(QRectF(5, 15, 11, 11));
    painter.drawArc(QRectF(11, 5, 14, 14), 0, 90 * 16);
    painter.drawLine(QPointF(18, 12), QPointF(26, 12));
    painter.drawLine(QPointF(26, 12), QPointF(26, 5));
  } else if (key == "extrude face") {
    painter.drawRect(QRectF(5, 11, 11, 13));
    painter.setBrush(QColor(232, 116, 0, 110));
    painter.drawRect(QRectF(16, 8, 11, 12));
    painter.drawLine(QPointF(13, 16), QPointF(18, 14));
    painter.drawLine(QPointF(18, 14), QPointF(16, 19));
    painter.drawLine(QPointF(18, 14), QPointF(22, 16));
  } else if (key == "fillet") {
    painter.drawLine(QPointF(7, 26), QPointF(7, 16));
    painter.drawArc(QRectF(7, 7, 16, 16), 180 * 16, -90 * 16);
    painter.drawLine(QPointF(15, 7), QPointF(26, 7));
  } else if (key == "detected edge filleting") {
    painter.drawLine(QPointF(5, 26), QPointF(5, 17));
    painter.drawArc(QRectF(5, 8, 18, 18), 180 * 16, -90 * 16);
    painter.drawLine(QPointF(14, 8), QPointF(20, 8));
    QFont beta = painter.font();
    beta.setPixelSize(12);
    beta.setBold(true);
    painter.setFont(beta);
    painter.drawText(QRectF(16, 14, 14, 14), Qt::AlignCenter, QString(QChar(0x03B2)));
  } else if (key == "fillet surface") {
    painter.drawLine(QPointF(6, 24), QPointF(6, 14));
    painter.drawArc(QRectF(6, 8, 12, 12), 180 * 16, -90 * 16);
    painter.drawLine(QPointF(12, 8), QPointF(24, 8));
    painter.drawEllipse(QRectF(16, 14, 8, 8));
  } else if (key == "bend") {
    painter.drawArc(QRectF(6, 8, 20, 16), 20 * 16, 140 * 16);
    painter.drawLine(QPointF(8, 20), QPointF(24, 20));
  } else if (key == "mirror") {
    painter.drawLine(QPointF(16, 5), QPointF(16, 27));
    painter.drawLine(QPointF(16, 16), QPointF(7, 9));
    painter.drawLine(QPointF(16, 16), QPointF(7, 23));
    painter.drawLine(QPointF(7, 9), QPointF(7, 23));
    painter.drawLine(QPointF(16, 16), QPointF(25, 9));
    painter.drawLine(QPointF(16, 16), QPointF(25, 23));
    painter.drawLine(QPointF(25, 9), QPointF(25, 23));
  } else if (key == "hole") {
    painter.drawEllipse(QRectF(7, 7, 18, 18));
    painter.drawLine(QPointF(11, 16), QPointF(21, 16));
  } else if (key == "group") {
    painter.drawRect(QRectF(6, 8, 12, 12));
    painter.drawRect(QRectF(14, 12, 12, 12));
  } else if (key == "ungroup") {
    painter.drawRect(QRectF(5, 7, 10, 10));
    painter.drawRect(QRectF(17, 15, 10, 10));
  } else if (key == "repeat") {
    painter.drawRect(QRectF(5, 12, 6, 6));
    painter.drawRect(QRectF(13, 12, 6, 6));
    painter.drawRect(QRectF(21, 12, 6, 6));
  } else if (key == "drop") {
    painter.drawLine(QPointF(16, 6), QPointF(16, 20));
    painter.drawLine(QPointF(16, 20), QPointF(11, 15));
    painter.drawLine(QPointF(16, 20), QPointF(21, 15));
    painter.drawLine(QPointF(7, 25), QPointF(25, 25));
  } else if (key == "drop onto part") {
    painter.drawLine(QPointF(16, 4), QPointF(16, 15));
    painter.drawLine(QPointF(16, 15), QPointF(12, 11));
    painter.drawLine(QPointF(16, 15), QPointF(20, 11));
    painter.setBrush(QColor(232, 116, 0, 90));
    painter.drawRect(QRectF(8, 18, 16, 8));
  } else if (key == "copy") {
    painter.drawRect(QRectF(10, 8, 12, 14));
    painter.drawRect(QRectF(6, 12, 12, 14));
  } else if (key == "delete") {
    painter.drawRect(QRectF(9, 10, 14, 14));
    painter.drawLine(QPointF(8, 10), QPointF(24, 10));
    painter.drawLine(QPointF(13, 7), QPointF(19, 7));
    painter.drawLine(QPointF(13, 7), QPointF(13, 10));
    painter.drawLine(QPointF(19, 7), QPointF(19, 10));
  } else if (key == "snap") {
    painter.drawEllipse(QRectF(8, 8, 16, 16));
    painter.drawEllipse(QRectF(13, 13, 6, 6));
  } else if (key == "grid") {
    painter.drawRect(QRectF(6, 6, 20, 20));
    painter.drawLine(QPointF(6, 16), QPointF(26, 16));
    painter.drawLine(QPointF(16, 6), QPointF(16, 26));
  } else if (key == "plate") {
    painter.drawRect(QRectF(5, 10, 22, 14));
    painter.drawLine(QPointF(5, 10), QPointF(10, 6));
    painter.drawLine(QPointF(27, 10), QPointF(22, 6));
    painter.drawLine(QPointF(10, 6), QPointF(22, 6));
  } else if (key == "lay flat") {
    painter.drawRect(QRectF(9, 10, 14, 10));
    painter.drawLine(QPointF(5, 25), QPointF(27, 25));
    painter.drawLine(QPointF(16, 20), QPointF(16, 24));
  } else if (key == "section") {
    painter.drawRect(QRectF(6, 8, 20, 16));
    painter.drawLine(QPointF(16, 4), QPointF(16, 28));
  } else if (key == "workplane") {
    painter.drawPolygon(QPolygonF({QPointF(4, 22), QPointF(16, 27), QPointF(28, 18), QPointF(16, 13)}));
    painter.drawLine(QPointF(16, 13), QPointF(16, 5));
  } else if (key == "align") {
    painter.drawLine(QPointF(8, 7), QPointF(8, 25));
    painter.drawLine(QPointF(8, 9), QPointF(24, 9));
    painter.drawLine(QPointF(8, 16), QPointF(20, 16));
    painter.drawLine(QPointF(8, 23), QPointF(24, 23));
  } else if (key == "measure") {
    painter.drawLine(QPointF(7, 22), QPointF(25, 10));
    painter.setBrush(QColor("#e87400"));
    painter.drawEllipse(QPointF(7, 22), 2.4, 2.4);
    painter.drawEllipse(QPointF(25, 10), 2.4, 2.4);
  } else if (key == "2d shape") {
    painter.drawLine(QPointF(9, 23), QPointF(23, 23));
    painter.drawLine(QPointF(23, 23), QPointF(23, 13));
    painter.drawLine(QPointF(9, 23), QPointF(9, 13));
    painter.drawArc(QRectF(9, 8, 14, 10), 0, 180 * 16);
    painter.drawRect(QRectF(7.3, 21.3, 3.4, 3.4));
    painter.drawRect(QRectF(21.3, 21.3, 3.4, 3.4));
    painter.drawRect(QRectF(7.3, 11.3, 3.4, 3.4));
    painter.drawRect(QRectF(21.3, 11.3, 3.4, 3.4));
  } else {
    painter.drawEllipse(QRectF(12, 12, 8, 8));
  }
  return QIcon(pix);
}

QString toolTipFor(const QString& name) {
  const QString key = name.toLower();
  if (key == "move") return QStringLiteral("Move");
  if (key == "rotate") return QStringLiteral("Rotate");
  if (key == "bend face") return QStringLiteral("Bend face");
  if (key == "extrude face") return QStringLiteral("Extrude face");
  if (key == "fillet") return QStringLiteral("Fillet edge");
  if (key == "fillet surface") return QStringLiteral("Fillet surface");
  if (key == "detected edge filleting")
    return QStringLiteral("Detected Edge Filleting (beta): finds the sharp edges of any part, group or STL; pick them to round or chamfer them");
  if (key == "bend")
    return QStringLiteral("Bend, twist and taper: curve a part into an arc or a closed ring, wring it, or make one end narrower");
  if (key == "mirror") return QStringLiteral("Mirror");
  if (key == "hole") return QStringLiteral("Hole");
  if (key == "group") return QStringLiteral("Group");
  if (key == "hull") return QStringLiteral("Hull: shrink-wrap the selected parts into one smooth solid");
  if (key == "intersect") return QStringLiteral("Intersect: keep only what the selected parts have in common");
  if (key == "hollow") return QStringLiteral("Hollow: make a part a shell with even walls, with sides left open if you like");
  if (key == "split") return QStringLiteral("Split: cut a part in two with a plane");
  if (key == "lattice")
    return QStringLiteral("Lattice: turn a part into a Voronoi skin, a gyroid, a honeycomb, a Voronoi lattice, a sponge or "
                          "bubbles: light, open and printable");
  if (key == "texture")
    return QStringLiteral("Texture: knurling, ribs, hexagons, wood grain, leather, carbon weave or your own picture, as real "
                          "geometry on the part's surface");
  if (key == "wrap")
    return QStringLiteral("Wrap: put text or a logo onto a curved part (select the text and the part), raised or cut in");
  if (key == "articulate")
    return QStringLiteral("Articulate: cut a part into segments with print-in-place joints, a flexi that wiggles off the plate");
  if (key == "simplify")
    return QStringLiteral("Simplify: fewer triangles for the same shape, for heavy sculpts and STLs");
  if (key == "mold")
    return QStringLiteral("Mold: a two-piece casting mould of a part, with a pour hole, air vents and keys, ready to print");
  if (key == "restyle") return QStringLiteral("Restyle: make a part low-poly (big flat facets) or build it from voxel cubes");
  if (key == "overhangs") return QStringLiteral("Overhangs: stripe the faces that hang over too steeply to print without support");
  if (key == "ungroup") return QStringLiteral("Ungroup");
  if (key == "merge")
    return QStringLiteral("Merge: join a group (or the selected parts) into one solid part, picked and filleted as its finished "
                          "shape. Click again to unmerge");
  if (key == "repeat") return QStringLiteral("Repeat copies");
  if (key == "drop") return QStringLiteral("Drop onto the plate (straight through other parts, e.g. a hole into a part)");
  if (key == "drop onto part") return QStringLiteral("Drop onto part: down until it touches the part below, exactly");
  if (key == "copy") return QStringLiteral("Copy");
  if (key == "delete") return QStringLiteral("Delete");
  if (key == "snap") return QStringLiteral("Snap");
  if (key == "grid") return QStringLiteral("Grid");
  if (key == "plate") return QStringLiteral("Plate");
  if (key == "align") return QStringLiteral("Align");
  if (key == "lay flat") return QStringLiteral("Lay flat: click a face of a part, and the part turns so that face lies on the plate");
  if (key == "section") return QStringLiteral("Section view: cut the view open to see inside (walls, holes, seals); drag the cut with the bar below");
  if (key == "workplane") return QStringLiteral("Workplane: click a face, and new parts stand on it; click the empty plate to go back to it");
  if (key == "measure") return QStringLiteral("Measure");
  if (key == "2d shape") return QStringLiteral("2D shape editor");
  return name;
}


// Your data: Documents\DaedalCAD for an installed copy, appdata/ next to the exe for a portable
// one (see appdirs.h). Icons, help pictures and the splash come from resourcePath().
QString defaultAppdataRoot() { return userDataRoot(); }

QString defaultAppdataPath(const QString& name) {
  return defaultAppdataRoot() + QDir::separator() + name;
}

QStringList normalizeDirectoryList(QStringList values, const QString& fallback) {
  QStringList out;
  auto add = [&](const QString& raw) {
    if (raw.isEmpty()) return;
    const QString normalized = QDir::cleanPath(QFileInfo(raw).absoluteFilePath());
    for (const QString& have : out) if (QDir::cleanPath(have) == normalized) return;
    out << normalized;
  };
  for (const QString& value : values) add(value);
  add(fallback);
  return out;
}

QStringList loadDirectoryList(QSettings& settings, const QString& key, const QString& legacyKey, const QString& fallback) {
  QStringList values = settings.value(key).toStringList();
  if (values.isEmpty()) {
    const QString legacy = settings.value(legacyKey).toString();
    if (!legacy.isEmpty()) values << legacy;
  }
  return normalizeDirectoryList(values, fallback);
}

void alignLibraryNames(QStringList& paths, QStringList& names, const QString& fallbackPath, const QString& fallbackName) {
  paths = normalizeDirectoryList(paths, fallbackPath);
  const QString fallbackClean = QDir::cleanPath(QFileInfo(fallbackPath).absoluteFilePath());
  QStringList aligned;
  for (int i = 0; i < paths.size(); ++i) {
    QString name = i < names.size() ? names[i].trimmed() : QString();
    if (name.isEmpty()) {
      if (QDir::cleanPath(paths[i]) == fallbackClean) name = fallbackName;
      else name = QFileInfo(paths[i]).fileName();
      if (name.isEmpty()) name = paths[i];
    }
    aligned << name;
  }
  names = aligned;
}

void toggleLibraryCategory(QListWidget* list, QListWidgetItem* header) {
  if (!list || !header || header->data(Qt::UserRole).toString() != QLatin1String("header")) return;
  const bool open = !header->data(Qt::UserRole + 1).toBool();
  header->setData(Qt::UserRole + 1, open);
  header->setText((open ? QStringLiteral("▾ ") : QStringLiteral("▸ ")) + header->data(Qt::UserRole + 2).toString());
  const int start = list->row(header);
  for (int i = start + 1; i < list->count(); ++i) {
    QListWidgetItem* item = list->item(i);
    if (item->data(Qt::UserRole).toString() == QLatin1String("header")) break;
    item->setHidden(!open);
  }
}


class LibrarySettingsDialog : public QDialog {
 public:
  LibrarySettingsDialog(QWidget* parent, const QStringList& parts, const QStringList& partNames, const QStringList& shapes,
                        const QStringList& shapeNames, const QString& defaultPartsPath, const QString& defaultShapesPath,
                        QWidget* platePage = nullptr)
      : QDialog(parent) {
    setWindowTitle(QStringLiteral("DaedalCAD Settings"));
    resize(860, 560);
    auto* root = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    QListWidget* partsList = nullptr;
    QListWidget* shapesList = nullptr;

    auto makePage = [&](const QString& defaultPath, const QString& defaultName, const QStringList& values, const QStringList& names,
                        QListWidget** outList) {
      auto* page = new QWidget(tabs);
      auto* layout = new QVBoxLayout(page);
      auto* list = new QListWidget(page);
      list->setSelectionMode(QAbstractItemView::SingleSelection);
      const QString normalizedDefault = QDir::cleanPath(QFileInfo(defaultPath).absoluteFilePath());
      auto addRow = [list](int row, const QString& name, const QString& path, bool locked) {
        auto* item = new QListWidgetItem();
        item->setData(Qt::UserRole, path);
        item->setData(Qt::UserRole + 1, locked);
        item->setSizeHint(QSize(0, 36));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        list->insertItem(row, item);
        auto* wrap = new QWidget(list);
        auto* line = new QHBoxLayout(wrap);
        line->setContentsMargins(4, 2, 4, 2);
        auto* nameEdit = new QLineEdit(name, wrap);
        nameEdit->setPlaceholderText(QStringLiteral("Category name"));
        nameEdit->setClearButtonEnabled(!locked);
        nameEdit->setReadOnly(locked);
        nameEdit->setMinimumWidth(150);
        if (locked) nameEdit->setStyleSheet(QStringLiteral("color: #99a5b3;"));
        auto* pathLabel = new QLabel(QDir::toNativeSeparators(path), wrap);
        pathLabel->setToolTip(QDir::toNativeSeparators(path));
        pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        line->addWidget(nameEdit, 2);
        line->addWidget(pathLabel, 3);
        list->setItemWidget(item, wrap);
      };
      auto rowName = [list](int index) {
        QWidget* widget = list->itemWidget(list->item(index));
        auto* edit = widget ? widget->findChild<QLineEdit*>() : nullptr;
        return edit ? edit->text().trimmed() : QString();
      };
      auto rowPath = [list](int index) { return list->item(index)->data(Qt::UserRole).toString(); };
      auto rowLocked = [list](int index) { return list->item(index)->data(Qt::UserRole + 1).toBool(); };
      auto hasPath = [list](const QString& path) {
        for (int i = 0; i < list->count(); ++i) {
          if (QDir::cleanPath(list->item(i)->data(Qt::UserRole).toString()) == path) return true;
        }
        return false;
      };
      addRow(0, defaultName, normalizedDefault, true);
      for (int i = 0; i < values.size(); ++i) {
        const QString path = QDir::cleanPath(QFileInfo(values[i]).absoluteFilePath());
        if (path == normalizedDefault || hasPath(path)) continue;
        QString name = i < names.size() ? names[i].trimmed() : QString();
        if (name.isEmpty()) name = QFileInfo(path).fileName();
        addRow(list->count(), name, path, false);
      }
      layout->addWidget(list, 1);
      auto* row = new QHBoxLayout();
      auto* add = new QPushButton(QStringLiteral("Add folder…"), page);
      auto* remove = new QPushButton(QStringLiteral("Remove"), page);
      auto* up = new QPushButton(QStringLiteral("Up"), page);
      auto* down = new QPushButton(QStringLiteral("Down"), page);
      row->addWidget(add);
      row->addWidget(remove);
      row->addWidget(up);
      row->addWidget(down);
      row->addStretch(1);
      layout->addLayout(row);
      connect(add, &QPushButton::clicked, this, [this, list, addRow, hasPath] {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Add library folder"));
        if (dir.isEmpty()) return;
        const QString normalized = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
        if (hasPath(normalized)) return;
        QString name = QFileInfo(normalized).fileName();
        if (name.isEmpty()) name = normalized;
        addRow(list->count(), name, normalized, false);
        list->setCurrentRow(list->count() - 1);
      });
      connect(remove, &QPushButton::clicked, this, [list, rowLocked] {
        const int rowIndex = list->currentRow();
        if (rowIndex < 0 || rowLocked(rowIndex)) return;
        delete list->takeItem(rowIndex);
      });
      connect(up, &QPushButton::clicked, this, [list, addRow, rowName, rowPath, rowLocked] {
        const int rowIndex = list->currentRow();
        if (rowIndex <= 1 || rowLocked(rowIndex)) return;
        const QString name = rowName(rowIndex);
        const QString path = rowPath(rowIndex);
        delete list->takeItem(rowIndex);
        addRow(rowIndex - 1, name, path, false);
        list->setCurrentRow(rowIndex - 1);
      });
      connect(down, &QPushButton::clicked, this, [list, addRow, rowName, rowPath, rowLocked] {
        const int rowIndex = list->currentRow();
        if (rowIndex < 1 || rowIndex >= list->count() - 1 || rowLocked(rowIndex)) return;
        const QString name = rowName(rowIndex);
        const QString path = rowPath(rowIndex);
        delete list->takeItem(rowIndex);
        addRow(rowIndex + 1, name, path, false);
        list->setCurrentRow(rowIndex + 1);
      });
      *outList = list;
      return page;
    };

    tabs->addTab(makePage(defaultPartsPath, QStringLiteral("Parts"), parts, partNames, &partsList), QStringLiteral("Parts libraries"));
    tabs->addTab(makePage(defaultShapesPath, QStringLiteral("2D shapes"), shapes, shapeNames, &shapesList), QStringLiteral("2D shape libraries"));
    if (platePage) {
      auto* plateTab = new QWidget(tabs);
      auto* plateLayout = new QVBoxLayout(plateTab);
      auto* plateNote = new QLabel(QStringLiteral("Build plate size, workplane height, and snap distance."), plateTab);
      plateNote->setWordWrap(true);
      plateLayout->addWidget(plateNote);
      platePage->setParent(plateTab);
      platePage->setVisible(true);
      plateLayout->addWidget(platePage);
      plateLayout->addStretch(1);
      tabs->addTab(plateTab, QStringLiteral("Build plate"));
    }
    root->addWidget(tabs, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, partsList, shapesList] {
      auto collect = [](QListWidget* list, QStringList* paths, QStringList* names) {
        paths->clear();
        names->clear();
        for (int i = 0; i < list->count(); ++i) {
          const QString path = QDir::cleanPath(QFileInfo(list->item(i)->data(Qt::UserRole).toString()).absoluteFilePath());
          QWidget* widget = list->itemWidget(list->item(i));
          auto* edit = widget ? widget->findChild<QLineEdit*>() : nullptr;
          QString name = edit ? edit->text().trimmed() : QString();
          if (name.isEmpty()) name = QFileInfo(path).fileName();
          if (name.isEmpty()) name = path;
          *paths << path;
          *names << name;
        }
      };
      collect(partsList, &partsPaths, &partsNames);
      collect(shapesList, &shapesPaths, &shapesNames);
      accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  }

  QStringList partsDirectories() const { return partsPaths; }
  QStringList partsDirectoryNames() const { return partsNames; }
  QStringList twoDShapesDirectories() const { return shapesPaths; }
  QStringList twoDShapesDirectoryNames() const { return shapesNames; }
 private:
  QStringList partsPaths;
  QStringList partsNames;
  QStringList shapesPaths;
  QStringList shapesNames;
};

QMatrix4x4 alignmentWorldMatrix(const Document& doc, const Node& node) {
  std::vector<const Node*> chain;
  const Node* cursor = &node;
  while (cursor) {
    chain.push_back(cursor);
    if (cursor->parentId.isEmpty()) break;
    cursor = doc.find(cursor->parentId);
  }
  QMatrix4x4 world;
  const double zero[3] = {0, 0, 0};
  for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    world = world * placementMatrix((*it)->pos, (*it)->rot, zero, (*it)->scale);
  return world;
}

bool alignmentWorldBounds(const Document& doc, const Node& node, QVector3D* minOut, QVector3D* maxOut) {
  double lo[3];
  double hi[3];
  if (!localBounds(doc, node, lo, hi)) return false;
  const QMatrix4x4 world = alignmentWorldMatrix(doc, node);
  QVector3D mn(1e9f, 1e9f, 1e9f);
  QVector3D mx(-1e9f, -1e9f, -1e9f);
  for (int i = 0; i < 8; ++i) {
    const QVector3D local((i & 1) ? static_cast<float>(hi[0]) : static_cast<float>(lo[0]),
                          (i & 2) ? static_cast<float>(hi[1]) : static_cast<float>(lo[1]),
                          (i & 4) ? static_cast<float>(hi[2]) : static_cast<float>(lo[2]));
    const QVector3D p = world.map(local);
    mn.setX(std::min(mn.x(), p.x()));
    mn.setY(std::min(mn.y(), p.y()));
    mn.setZ(std::min(mn.z(), p.z()));
    mx.setX(std::max(mx.x(), p.x()));
    mx.setY(std::max(mx.y(), p.y()));
    mx.setZ(std::max(mx.z(), p.z()));
  }
  if (mn.x() > mx.x()) return false;
  *minOut = mn;
  *maxOut = mx;
  return true;
}

QMatrix4x4 alignmentParentWorldMatrix(const Document& doc, const Node& node) {
  if (node.parentId.isEmpty()) return QMatrix4x4();
  const Node* parent = doc.find(node.parentId);
  return parent ? alignmentWorldMatrix(doc, *parent) : QMatrix4x4();
}

}  // namespace

QString copyStlBeside(const QString& sourcePath, const QString& directory);
QString sanitizeProjectName(QString name);
bool pathIsInside(const QString& path, const QString& root);

// Closing: the child widgets go after this, and the one with the focus still reports losing it
// (a name box would rename a part in a document that is already gone).
MainWindow::~MainWindow() {
  loading = true;
  for (QObject* child : findChildren<QObject*>()) disconnect(child, nullptr, this, nullptr);
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("DaedalCAD");
  resize(1280, 800);
  const QString defaultParts = defaultAppdataPath(QStringLiteral("parts"));
  const QString default2DShapes = defaultAppdataPath(QStringLiteral("2d_shapes"));
  QDir().mkpath(defaultAppdataPath(QStringLiteral("projects")));
  QDir().mkpath(defaultAppdataPath(QStringLiteral("Imported")));
  QDir().mkpath(defaultParts);
  QDir().mkpath(default2DShapes);
  QSettings settings;
  setPrinterClearance(settings.value(QStringLiteral("printerClearance"), 0.2).toDouble());
  // A portable copy remembers where its data is, so an installed copy can offer to bring it over.
  if (!isInstalledCopy()) settings.setValue(QStringLiteral("portableDataRoot"), QDir::cleanPath(userDataRoot()));
  partsDirectories = loadDirectoryList(settings, QStringLiteral("partsDirectories"), QStringLiteral("partsDirectory"), defaultParts);
  partsDirectoryNames = settings.value(QStringLiteral("partsDirectoryNames")).toStringList();
  alignLibraryNames(partsDirectories, partsDirectoryNames, defaultParts, QStringLiteral("Parts"));
  twoDShapesDirectories = loadDirectoryList(settings, QStringLiteral("twoDShapesDirectories"), QStringLiteral("twoDShapesDirectory"), default2DShapes);
  twoDShapesDirectoryNames = settings.value(QStringLiteral("twoDShapesDirectoryNames")).toStringList();
  alignLibraryNames(twoDShapesDirectories, twoDShapesDirectoryNames, default2DShapes, QStringLiteral("2D shapes"));
  settings.setValue(QStringLiteral("partsDirectories"), partsDirectories);
  settings.setValue(QStringLiteral("partsDirectoryNames"), partsDirectoryNames);
  settings.setValue(QStringLiteral("twoDShapesDirectories"), twoDShapesDirectories);
  settings.setValue(QStringLiteral("twoDShapesDirectoryNames"), twoDShapesDirectoryNames);
  const QString iconDir = resourcePath(QStringLiteral("icons")).replace('\\', '/');
  const auto spinIcon = [&](const char* name) {
    return QStringLiteral("\"") + QUrl::fromLocalFile(iconDir + QLatin1Char('/') + QString::fromLatin1(name)).toString() +
           QStringLiteral("\"");
  };
  setStyleSheet(
      "QMainWindow, QWidget { background: #12151a; color: #ece6da; }"
      "QTreeWidget, QListWidget, QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit {"
      " background: #1b2028; color: #ece6da; border: 1px solid #343c48; padding: 3px; }"
      "QSpinBox, QDoubleSpinBox { min-height: 22px; padding-right: 16px; }"
      "QSpinBox::up-button, QDoubleSpinBox::up-button {"
      " subcontrol-origin: border; subcontrol-position: top right; width: 16px; height: 11px;"
      " background: #2a3140; border: none; border-bottom: 1px solid #1b2028; border-left: 1px solid #1b2028;"
      " image: url(" + spinIcon("spin_up.png") + "); }"
      "QSpinBox::down-button, QDoubleSpinBox::down-button {"
      " subcontrol-origin: border; subcontrol-position: bottom right; width: 16px; height: 11px;"
      " background: #2a3140; border: none; border-left: 1px solid #1b2028;"
      " image: url(" + spinIcon("spin_down.png") + "); }"
      "QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover {"
      " background: #e87400; image: url(" + spinIcon("spin_up_on.png") + "); }"
      "QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {"
      " background: #e87400; image: url(" + spinIcon("spin_down_on.png") + "); }"
      "QSpinBox::up-button:pressed, QDoubleSpinBox::up-button:pressed {"
      " background: #ff9a33; image: url(" + spinIcon("spin_up_on.png") + "); }"
      "QSpinBox::down-button:pressed, QDoubleSpinBox::down-button:pressed {"
      " background: #ff9a33; image: url(" + spinIcon("spin_down_on.png") + "); }"
      "QSpinBox::up-arrow, QDoubleSpinBox::up-arrow, QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {"
      " image: none; width: 0; height: 0; }"
      // Tabs drawn by the stylesheet, not by Windows: on Windows 10 Qt uses the "windowsvista"
      // style, whose native tabs are white, and the light text on them vanished.
      "QTabWidget::pane { background: #12151a; border: 1px solid #2c3340; top: -1px; }"
      "QTabBar::tab { background: #1b2028; color: #b8b1a5; border: 1px solid #2c3340; border-bottom: none;"
      " padding: 6px 12px; margin-right: 2px; }"
      "QTabBar::tab:selected { background: #252c36; color: #ece6da; border-top: 2px solid #e87400; }"
      "QTabBar::tab:hover:!selected { background: #222833; color: #ece6da; }"
      "QTabBar::tab:disabled { color: #626b77; }"
      "QListWidget { cursor: arrow; }"
      "QListWidget::item { padding: 4px 2px; }"
      "QDockWidget { titlebar-close-icon: none; }"
      "QDockWidget::title { background: #1b2028; padding: 6px; }"
      "QPushButton { background: #252c36; color: #ece6da; border: 1px solid #343c48; padding: 6px 10px; }"
      "QPushButton:checked { background: #e87400; color: #24180c; }"
      "QPushButton:disabled { background: #1d222a; color: #626b77; border: 1px solid #2a3039; }"
      "QMenuBar { background: #1b2028; color: #e87400; }"
      "QMenuBar::item { background: transparent; color: #e87400; padding: 8px 16px; }"
      "QMenuBar::item:selected { background: #2a1c10; color: #ffb15a; }"
      "QMenu { background: #1b2028; color: #ece6da; border: 1px solid #3a2414; }"
      "QMenu::item { padding: 8px 28px; color: #ece6da; }"
      "QMenu::item:selected { background: #2a1c10; color: #e87400; }"
      "QStatusBar { background: #1b2028; color: #ece6da; }"
      "QStatusBar QToolButton { background: transparent; color: #ece6da; border: 1px solid transparent; padding: 1px 6px; margin: 0 2px; font-size: 11px; }"
      "QStatusBar QToolButton:hover { background: #252c36; }"
      "QStatusBar QToolButton:checked { background: #3a2a1c; border: 1px solid #e87400; color: #ffb15a; }"
      "QToolBar { background: #1b2028; color: #ece6da; spacing: 0px; padding: 0px; border: none; }"
      "QToolBar QToolButton { background: transparent; border: 1px solid transparent; padding: 2px; margin: 0px; }"
      "QToolBar QToolButton:hover { background: #252c36; }"
      "QToolBar QToolButton:checked { background: #3a2a1c; border: 1px solid #e87400; }"
      "QToolBar::separator:horizontal { background: #9a4a00; width: 2px; margin: 6px 9px; }"
      "QToolBar::separator:vertical { background: #9a4a00; height: 2px; margin: 9px 6px; }"
      "QToolBar QToolButton#shape2dButton { background: #e87400; border: 1px solid #c45e00; border-radius: 4px; padding: 4px; margin: 2px; }"
      "QToolBar QToolButton#shape2dButton:hover { background: #ff9a2e; }"
      "QToolBar QToolButton#sculptButton { background: #e87400; border: 1px solid #c45e00; border-radius: 4px; padding: 4px; margin: 2px; }"
      "QToolBar QToolButton#sculptButton:hover { background: #ff9a2e; }"
      "QWidget#viewHost { background: #12151a; }"
      "QDockWidget#modelDock, QDockWidget#modelDock QWidget { background: #0b0d11; color: #ece6da; }"
      "QDockWidget#modelDock::title { background: #0b0d11; padding: 6px; }"
      "QDockWidget#modelDock QTreeWidget, QDockWidget#modelDock QLineEdit, QDockWidget#modelDock QSpinBox,"
      "QDockWidget#modelDock QDoubleSpinBox, QDockWidget#modelDock QComboBox {"
      " background: #161b22; color: #ece6da; border: 1px solid #2c3340; }"
      "QDockWidget#modelDock QScrollArea { border: none; background: #0b0d11; }"
      "QFrame#filletPopup, QFrame#repeatPopup, QFrame#alignPopup, QFrame#faceFilletPopup, QFrame#colorPopup, QFrame#toolPopup, QFrame#sectionBar { background: #0b0d11; border: 1px solid #e87400; }"
      "QFrame#filletPopup QLabel, QFrame#repeatPopup QLabel, QFrame#alignPopup QLabel, QFrame#faceFilletPopup QLabel, QFrame#colorPopup QLabel, QFrame#toolPopup QLabel { background: #0b0d11; color: #ece6da; }"
      "QFrame#busyWrap { background: #0b0d11; border: 1px solid #e87400; }"
      "QFrame#busyWrap QLabel { background: transparent; color: #e87400; }"
      "QProgressBar#busyBar { background: #1b2028; border: none; min-height: 10px; max-height: 10px; }"
      "QProgressBar#busyBar::chunk { background: #e87400; }");
  buildUi();
  refreshTree();
  loadInspector();
  refreshScene();
  placeOverlays();
  refreshPartsLibrary();
  refresh2DShapeLibrary();
  refreshSculptLibrary();
  status("Select an object for its size box. Fillet picks an edge on most shapes. Repeat makes separate copies.");
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  const bool undoField = qobject_cast<QAbstractSpinBox*>(watched) || (textEdit && watched == textEdit) ||
                         (genInner && qobject_cast<QLineEdit*>(watched) && genInner->isAncestorOf(static_cast<QWidget*>(watched)));
  if (!loading && event->type() == QEvent::FocusIn && undoField) {
    if (!gesture) {
      pushUndo();
      gesture = true;
    }
  }
  if (event->type() == QEvent::FocusOut && undoField) {
    gesture = false;
    // Leaving the text box: build now rather than waiting for the timer.
    if (textEdit && watched == textEdit && textTimer && textTimer->isActive()) {
      textTimer->stop();
      refreshScene();
    }
  }
  if (view && watched == view->parentWidget() && event->type() == QEvent::Resize) {
    auto* host = view->parentWidget();
    view->setGeometry(0, 0, host->width(), host->height());
    placeOverlays();
  }
  return QMainWindow::eventFilter(watched, event);
}

QDoubleSpinBox* MainWindow::makeSpin(double min, double max, double step) {
  auto* box = new QDoubleSpinBox(this);
  box->setRange(min, max);
  box->setDecimals(3);
  box->setSingleStep(step);
  box->installEventFilter(this);
  return box;
}

void MainWindow::bindSpin(QDoubleSpinBox* box, const std::function<void(double)>& apply, bool quick) {
  connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, apply, quick](double value) {
    if (loading) return;
    apply(value);
    refreshScene(quick);
  });
}

void MainWindow::buildUi() {
  auto* host = new QWidget(this);
  host->setObjectName("viewHost");
  view = new Viewport(host);
  setCentralWidget(host);
  host->installEventFilter(this);
  connect(view, &Viewport::alignHandleClicked, this, [this](int axis, int mode) { alignSelection(axis, mode); });
  connect(view, &Viewport::worldFacePicked, this, [this](const QString& id, QVector3D centre, QVector3D normal) {
    if (tool == 12) {
      if (id.isEmpty()) status(QStringLiteral("Click a face of a part: that face goes down onto the plate."));
      else layFlat(id, normal);
    } else if (tool == 13) {
      if (id.isEmpty()) clearFaceWorkplane();
      else setWorkplaneOnFace(centre, normal);
    }
  });
  connect(view, &Viewport::doubleClicked, this, [this](const QString& id) {
    if (const Node* node = doc.find(id); node && node->sculpt) sculptPart(id);
  });
  connect(view, &Viewport::picked, this, [this](const QString& id, Qt::KeyboardModifiers mods) {
    // Align: clicking one of the parts being aligned makes it the one that stays (again clears it).
    if (alignPopup && alignPopup->isVisible() && !id.isEmpty() && !mods.testFlag(Qt::ControlModifier)) {
      const QString root = alignRootFor(id);
      if (!root.isEmpty()) {
        alignAnchorId = alignAnchorId == root ? QString() : root;
        updateAlignOverlay();
        const Node* anchor = doc.find(alignAnchorId);
        status(anchor ? QStringLiteral("%1 stays. Click a dot to line the others up with it.").arg(anchor->name)
                      : QStringLiteral("Nothing fixed: the parts line up on the selection as a whole. Click a part to keep it in place."));
        return;
      }
    }
    if (id.isEmpty()) selection.clear();
    else if (mods.testFlag(Qt::ShiftModifier)) {
      if (!selection.contains(id)) selection << id;
    } else if (mods.testFlag(Qt::ControlModifier)) {
      if (selection.contains(id)) selection.removeAll(id);
      else selection << id;
    } else selection = QStringList{id};
    for (int i = 0; i < 3; ++i) {
      multiPosLast[i] = 0;
      multiRotLast[i] = 0;
      multiScaleLast[i] = 100.0;
    }
    refreshTree();
    loadInspector();
    refreshScene();
    // Picking a part makes its middle the focus point (the view turns and zooms around it).
    if (!id.isEmpty() && !selection.isEmpty() && tool == 0) view->focusOn(selection);
  });
  connect(view, &Viewport::marqueeSelected, this, [this](const QStringList& ids, Qt::KeyboardModifiers mods) {
    if (mods.testFlag(Qt::ShiftModifier)) {
      for (const QString& id : ids) {
        if (!selection.contains(id)) selection << id;
      }
    } else if (mods.testFlag(Qt::ControlModifier)) {
      for (const QString& id : ids) {
        if (selection.contains(id)) selection.removeAll(id);
        else selection << id;
      }
    } else {
      selection = ids;
    }
    for (int i = 0; i < 3; ++i) {
      multiPosLast[i] = 0;
      multiRotLast[i] = 0;
      multiScaleLast[i] = 100.0;
    }
    refreshTree();
    loadInspector();
    refreshScene();
  });
  connect(view, &Viewport::dropped, this, [this](const QString& shape, double x, double y, double fillet) {
    // On a face workplane x and y are the spot on it (u, v); the part is stood there afterwards.
    placingDropped = true;
    placingU = x;
    placingV = y;
    if (shape.startsWith(QStringLiteral("stl:"))) importStlPart(shape.mid(4), x, y);
    else if (shape.startsWith(QStringLiteral("svg:"))) importSvgShape(shape.mid(4), x, y);
    else if (shape.startsWith(QStringLiteral("sculpt:"))) placeLibrarySculpt(shape.mid(7), x, y);
    else if (shape.startsWith(QStringLiteral("gen:"))) placeGenerator(shape.mid(4), x, y);
    else placeShape(shape, x, y, fillet);
    placingDropped = false;
  });
  connect(view, &Viewport::nudgeRequested, this, [this](int dx, int dy) { nudgeSelection(dx, dy); });
  connect(view, &Viewport::transformStarted, this, [this] {
    multiTransformStart.clear();
    if (selection.size() > 1) {
      for (const QString& selectedId : selection) {
        const Node* selected = doc.find(selectedId);
        if (!selected) continue;
        MultiTransformStart start;
        start.id = selected->id;
        for (int i = 0; i < 3; ++i) {
          start.pos[i] = selected->pos[i];
          start.rot[i] = selected->rot[i];
          start.scale[i] = selected->scale[i];
        }
        multiTransformStart.push_back(start);
      }
    }
    pushUndo();
  });
  connect(view, &Viewport::transformed, this, [this](const QString& id, QVector3D pos, QVector3D rot, QVector3D scale) {
    if (selection.size() > 1 && !multiTransformStart.empty()) {
      const auto anchor = std::find_if(multiTransformStart.begin(), multiTransformStart.end(),
                                       [&](const MultiTransformStart& start) { return start.id == id; });
      if (anchor != multiTransformStart.end()) {
        double deltaPos[3] = {pos.x() - anchor->pos[0], pos.y() - anchor->pos[1], pos.z() - anchor->pos[2]};
        for (double& d : deltaPos)
          if (std::abs(d) < 5e-5) d = 0;  // float noise on an axis the drag did not move
        double deltaRot[3] = {rot.x() - anchor->rot[0], rot.y() - anchor->rot[1], rot.z() - anchor->rot[2]};
        double ratio[3] = {1, 1, 1};
        const double currentScale[3] = {scale.x(), scale.y(), scale.z()};
        for (int axis = 0; axis < 3; ++axis) {
          ratio[axis] = std::abs(anchor->scale[axis]) > 1e-8 ? currentScale[axis] / anchor->scale[axis] : 1;
        }
        for (const MultiTransformStart& start : multiTransformStart) {
          Node* selected = doc.find(start.id);
          if (!selected) continue;
          for (int axis = 0; axis < 3; ++axis) {
            selected->pos[axis] = start.pos[axis] + deltaPos[axis];
            selected->rot[axis] = start.rot[axis] + deltaRot[axis];
            selected->scale[axis] = start.scale[axis] * ratio[axis];
          }
          tidyPlacement(*selected);
          for (int axis = 0; axis < 3; ++axis)
            if (deltaPos[axis] == 0) selected->pos[axis] = start.pos[axis];
        }
        refreshScene(true);
        loadInspector();
        return;
      }
    }
    Node* node = doc.find(id);
    if (!node) return;
    // The viewport hands over floats. A value the drag did not change comes back a float step
    // off; keep the exact one (Drop onto part may have set it between the 0.0001 mm steps).
    const Node before = *node;
    node->pos[0] = pos.x();
    node->pos[1] = pos.y();
    node->pos[2] = pos.z();
    node->rot[0] = rot.x();
    node->rot[1] = rot.y();
    node->rot[2] = rot.z();
    node->scale[0] = scale.x();
    node->scale[1] = scale.y();
    node->scale[2] = scale.z();
    tidyPlacement(*node);
    for (int i = 0; i < 3; ++i) {
      if (std::abs(double(pos[i]) - before.pos[i]) < 5e-5) node->pos[i] = before.pos[i];
      if (std::abs(double(rot[i]) - before.rot[i]) < 5e-5) node->rot[i] = before.rot[i];
      if (std::abs(double(scale[i]) - before.scale[i]) < 5e-7) node->scale[i] = before.scale[i];
    }
    refreshScene(true);
    loadInspector();
  });

  // Moving or sizing a part by hand: whatever other tool or panel was open closes, and Move is on.
  connect(view, &Viewport::moveDragStarted, this, [this] {
    const bool popupOpen = (alignPopup && alignPopup->isVisible()) || (repeatPopup && repeatPopup->isVisible()) ||
                           (filletPopup && filletPopup->isVisible()) || (faceFilletPopup && faceFilletPopup->isVisible()) ||
                           (edgeBetaPopup && edgeBetaPopup->isVisible());
    if (tool != 0 || panel != 0 || popupOpen) backToMove(true);
  });

  tree = new QTreeWidget(this);
  tree->setHeaderHidden(true);
  tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  connect(tree, &QTreeWidget::itemSelectionChanged, this, [this] {
    if (loading) return;
    selection.clear();
    for (QTreeWidgetItem* item : tree->selectedItems()) selection << item->data(0, Qt::UserRole).toString();
    for (int i = 0; i < 3; ++i) {
      multiPosLast[i] = 0;
      multiRotLast[i] = 0;
      multiScaleLast[i] = 100.0;
    }
    loadInspector();
    refreshScene();
    if (!selection.isEmpty()) view->focusOn(selection);
  });
  connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
    if (!item || loading) return;
    const QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty()) return;
    if (const Node* node = doc.find(id); node && node->sculpt) {
      QTimer::singleShot(0, this, [this, id] { sculptPart(id); });
      return;
    }
    if (tree->selectedItems() != QList<QTreeWidgetItem*>{item}) {
      tree->clearSelection();
      item->setSelected(true);
    }
    backToMove();
  });

  auto* formHost = new QWidget(this);
  auto* form = new QFormLayout(formHost);
  nameEdit = new QLineEdit(formHost);
  form->addRow("Name", nameEdit);

  colorButton = new QPushButton("Color", formHost);
  colorButton->setEnabled(false);
  form->addRow("Color", colorButton);
  connect(colorButton, &QPushButton::clicked, this, [this] { chooseObjectColor(); });

  connect(nameEdit, &QLineEdit::editingFinished, this, [this] {
    if (loading || selection.size() != 1) return;
    if (Node* node = doc.find(selection.front())) {
      pushUndo();
      node->name = nameEdit->text();
      refreshTree();
    }
  });
  const char* axes = "XYZ";
  for (int i = 0; i < 3; ++i) {
    posSpin[i] = makeSpin(-10000, 10000, 1);
    form->addRow(QString("Position %1").arg(axes[i]), posSpin[i]);
    connect(posSpin[i], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, i](double value) {
      if (loading || selection.isEmpty()) return;
      if (selection.size() == 1) {
        if (Node* node = doc.find(selection.front())) node->pos[i] = value;
      } else {
        const double delta = value - multiPosLast[i];
        multiPosLast[i] = value;
        if (std::abs(delta) < 1e-12) return;
        for (const QString& id : selection) {
          if (Node* node = doc.find(id)) node->pos[i] += delta;
        }
      }
      refreshScene(true);
    });
  }
  rotPage = new QWidget(formHost);
  auto* rotForm = new QFormLayout(rotPage);
  rotForm->setContentsMargins(0, 0, 0, 0);
  rotationStepSpin = makeSpin(0, 180, 1);
  rotationStepSpin->setDecimals(1);
  rotationStepSpin->setValue(0);
  rotationStepSpin->setToolTip(QStringLiteral("The steps a drag snaps to in the inner band of the rotate disk. 0 uses 15 degrees. "
                                              "In the outer band it always turns freely."));
  rotForm->addRow("Rotation step deg", rotationStepSpin);
  connect(rotationStepSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
    if (view) view->setRotationSnap(value);
  });
  for (int i = 0; i < 3; ++i) {
    rotSpin[i] = makeSpin(-3600, 3600, 5);
    rotForm->addRow(QString("Rotate %1 deg").arg(axes[i]), rotSpin[i]);
    connect(rotSpin[i], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, i](double value) {
      if (loading || selection.isEmpty()) return;
      if (selection.size() == 1) {
        if (Node* node = doc.find(selection.front())) node->rot[i] = value;
      } else {
        const double delta = value - multiRotLast[i];
        multiRotLast[i] = value;
        if (std::abs(delta) < 1e-12) return;
        for (const QString& id : selection) {
          if (Node* node = doc.find(id)) node->rot[i] += delta;
        }
      }
      refreshScene(true);
    });
  }
  form->addRow(rotPage);

  shapePage = new QWidget(formHost);
  auto* shapeForm = new QFormLayout(shapePage);
  const char* sizeNames[3] = {"Length X mm", "Width Y mm", "Height Z mm"};
  for (int i = 0; i < 3; ++i) {
    sizeSpin[i] = makeSpin(0.05, 10000, 1);
    shapeForm->addRow(sizeNames[i], sizeSpin[i]);
    connect(sizeSpin[i], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, i](double value) {
      if (loading || selection.isEmpty()) return;
      if (selection.size() > 1) {
        const double previous = std::max(0.01, multiScaleLast[i]);
        const double ratio = std::max(0.01, value) / previous;
        multiScaleLast[i] = std::max(0.01, value);
        if (std::abs(ratio - 1.0) < 1e-12) return;
        for (const QString& id : selection) {
          if (Node* selected = doc.find(id)) selected->scale[i] *= ratio;
        }
        refreshScene(true);
        return;
      }
      Node* node = doc.find(selection.front());
      if (!node) return;
      double lo[3];
      double hi[3];
      if (!localBounds(doc, *node, lo, hi)) return;
      const double extent = hi[i] - lo[i];
      if (extent < 1e-4) return;
      const QVector3D center((lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5);
      const QQuaternion q = QQuaternion::fromAxisAndAngle(0, 0, 1, node->rot[2]) * QQuaternion::fromAxisAndAngle(0, 1, 0, node->rot[1]) *
                            QQuaternion::fromAxisAndAngle(1, 0, 0, node->rot[0]);
      auto offset = [&](const double sc[3]) {
        return q.rotatedVector(QVector3D(center.x() * sc[0], center.y() * sc[1], center.z() * sc[2]));
      };
      auto lowestZ = [&](const double sc[3], const QVector3D& origin) {
        double minZ = 1e9;
        for (int corner = 0; corner < 8; ++corner) {
          const QVector3D p((corner & 1) ? hi[0] : lo[0], (corner & 2) ? hi[1] : lo[1], (corner & 4) ? hi[2] : lo[2]);
          const QVector3D world = origin + q.rotatedVector(QVector3D(p.x() * sc[0], p.y() * sc[1], p.z() * sc[2]));
          minZ = std::min(minZ, static_cast<double>(world.z()));
        }
        return minZ;
      };
      double sc[3] = {node->scale[0], node->scale[1], node->scale[2]};
      const QVector3D origin(node->pos[0], node->pos[1], node->pos[2]);
      const double oldBottom = lowestZ(sc, origin);
      const QVector3D anchor = origin + offset(sc);
      const double sign = sc[i] < 0 ? -1 : 1;
      sc[i] = sign * (value / extent);
      const QVector3D pos = anchor - offset(sc);
      node->pos[0] = pos.x();
      node->pos[1] = pos.y();
      node->pos[2] = pos.z();
      node->scale[0] = sc[0];
      node->scale[1] = sc[1];
      node->scale[2] = sc[2];
      node->pos[2] += oldBottom - lowestZ(sc, QVector3D(node->pos[0], node->pos[1], node->pos[2]));
      tidyPlacement(*node);
      refreshScene(true);
    });
  }
  radiusSpin = makeSpin(0.05, 10000, 1);
  radius2Spin = makeSpin(0, 10000, 1);
  sidesSpin = new QSpinBox(shapePage);
  sidesSpin->setRange(3, 256);
  sidesSpin->installEventFilter(this);
  shapeForm->addRow("Radius", radiusSpin);
  shapeForm->addRow("Second radius", radius2Spin);
  shapeForm->addRow("Sides", sidesSpin);
  gemTableSpin = new QDoubleSpinBox(shapePage);
  gemTableSpin->setRange(0, 95);
  gemTableSpin->setDecimals(0);
  gemTableSpin->setSingleStep(5);
  gemTableSpin->setSuffix(QStringLiteral(" %"));
  gemTableSpin->setSpecialValueText(QStringLiteral("Pointed"));
  gemTableSpin->setToolTip(QStringLiteral("0: pointed at the top, like the classic diamond. Above 0: a cut gem with a flat top "
                                          "this wide (in % of the full width), a short crown and a pointed bottom."));
  gemTableSpin->installEventFilter(this);
  shapeForm->addRow("Flat top", gemTableSpin);
  gemTableSpin->hide();
  if (QWidget* label = shapeForm->labelForField(gemTableSpin)) label->hide();
  connect(gemTableSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
    if (loading) return;
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    if (!node || node->shape != QLatin1String("diamond")) return;
    node->gemTable = value / 100.0;
    refreshScene(true);
  });
  // Threads on cylinders: a metric size (sets the diameter), the pitch and, for a hole, its play.
  threadBox = new QComboBox(shapePage);
  threadBox->addItem(QStringLiteral("None"), QString());
  for (const gen::ThreadSize& size : gen::threadSizes()) threadBox->addItem(size.name, size.name);
  threadBox->addItem(QStringLiteral("Custom (this diameter)"), QStringLiteral("custom"));
  threadBox->setToolTip(QStringLiteral("An ISO metric thread. A solid cylinder becomes a threaded rod; as a hole it cuts a "
                                       "threaded hole a bolt of that size screws into. A size sets the diameter."));
  shapeForm->addRow("Thread", threadBox);
  threadPitchSpin = new QDoubleSpinBox(shapePage);
  threadPitchSpin->setRange(0, 6);
  threadPitchSpin->setDecimals(2);
  threadPitchSpin->setSingleStep(0.05);
  threadPitchSpin->setSuffix(QStringLiteral(" mm"));
  threadPitchSpin->setSpecialValueText(QStringLiteral("Standard"));
  threadPitchSpin->setToolTip(QStringLiteral("Distance from one thread crest to the next. Standard: the usual coarse pitch for the size."));
  threadPitchSpin->installEventFilter(this);
  shapeForm->addRow("Pitch", threadPitchSpin);
  threadClearanceSpin = new QDoubleSpinBox(shapePage);
  threadClearanceSpin->setRange(0, 2);
  threadClearanceSpin->setDecimals(2);
  threadClearanceSpin->setSingleStep(0.05);
  threadClearanceSpin->setSuffix(QStringLiteral(" mm"));
  threadClearanceSpin->setToolTip(QStringLiteral("How much wider the threaded hole is cut all round, so a real or printed bolt "
                                                 "turns in it. 0.15 to 0.3 suits most printers; the thread fit test tells."));
  threadClearanceSpin->installEventFilter(this);
  shapeForm->addRow("Thread play", threadClearanceSpin);
  for (QWidget* field : std::initializer_list<QWidget*>{threadBox, threadPitchSpin, threadClearanceSpin}) {
    field->hide();
    if (QWidget* label = shapeForm->labelForField(field)) label->hide();
  }
  connect(threadBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    if (loading) return;
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    if (!node || node->shape != QLatin1String("cylinder")) return;
    pushUndo();
    node->thread = threadBox->currentData().toString();
    for (const gen::ThreadSize& size : gen::threadSizes())
      if (size.name == node->thread) node->radius = size.diameter * 0.5;
    node->threadPitch = 0;
    node->threadClearance = gen::threadPlay(gen::coarsePitch(node->radius * 2));
    loadInspector();
    refreshScene(true);
    if (!node->thread.isEmpty())
      status(node->hole ? QStringLiteral("Threaded hole: a %1 bolt screws into it. Thread play sets how freely.").arg(threadBox->currentText())
                        : QStringLiteral("Threaded rod. Turn it into a hole for a threaded hole instead."));
  });
  connect(threadPitchSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
    if (loading) return;
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    if (!node || node->shape != QLatin1String("cylinder") || node->thread.isEmpty()) return;
    node->threadPitch = value;
    refreshScene(true);
  });
  connect(threadClearanceSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
    if (loading) return;
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    if (!node || node->shape != QLatin1String("cylinder") || node->thread.isEmpty()) return;
    node->threadClearance = value;
    refreshScene(true);
  });
  filletSideBox = new QComboBox(shapePage);
  filletSideBox->addItems({"Inner", "Outer"});
  filletDirBox = new QComboBox(shapePage);
  filletDirBox->addItems({"Down", "Up"});
  shapeForm->addRow("Inner/Outer", filletSideBox);
  shapeForm->addRow("Up/Down", filletDirBox);
  auto applyFilletFlags = [this] {
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    if (!node || node->shape != QLatin1String("facefillet") || !filletSideBox || !filletDirBox) return;
    const bool outer = filletSideBox->currentIndex() == 1;
    const bool up = filletDirBox->currentIndex() == 1;
    auto write = [&](Node& item) {
      item.filletOuter = outer;
      item.filletUp = up;
      item.hole = !outer;
    };
    write(*node);
    if (!node->filletOp.isEmpty()) {
      for (Node& other : doc.nodes)
        if (other.filletOp == node->filletOp) write(other);
    }
    refreshScene();
  };
  connect(filletSideBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, applyFilletFlags](int index) {
    if (loading) return;
    if (filletDirBox) {
      const QSignalBlocker blocker(filletDirBox);
      filletDirBox->setCurrentIndex(index == 1 ? 1 : 0);
    }
    applyFilletFlags();
  });
  connect(filletDirBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, applyFilletFlags](int) {
    if (loading) return;
    applyFilletFlags();
  });
  filletSideBox->hide();
  filletDirBox->hide();
  if (QWidget* label = shapeForm->labelForField(filletSideBox)) label->hide();
  if (QWidget* label = shapeForm->labelForField(filletDirBox)) label->hide();

  // Text. Any installed font; the outline is taken from the font's own curves and flattened to the
  // chosen smoothness, so letters stay round at any size.
  textEdit = new QPlainTextEdit(shapePage);
  textEdit->setPlaceholderText(QStringLiteral("Type your text"));
  textEdit->setTabChangesFocus(true);
  textEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
  textEdit->setFixedHeight(static_cast<int>(textEdit->fontMetrics().lineSpacing() * 3 + 14));
  textEdit->installEventFilter(this);
  fontBox = new QFontComboBox(shapePage);
  fontBox->setFontFilters(QFontComboBox::ScalableFonts);
  fontBox->setMaxVisibleItems(16);
  fontBox->setMinimumContentsLength(10);
  fontBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  fontStyleBox = new QComboBox(shapePage);
  fontSizeSpin = makeSpin(0.5, 1000, 1);
  fontSizeSpin->setToolTip(QStringLiteral("Height of a capital letter."));
  letterSpacingSpin = makeSpin(-50, 200, 0.25);
  letterSpacingSpin->setToolTip(QStringLiteral("Extra space between letters. Negative pulls them together."));
  textAlignBox = new QComboBox(shapePage);
  textAlignBox->addItems({QStringLiteral("Left"), QStringLiteral("Centre"), QStringLiteral("Right")});
  textAlignBox->setToolTip(QStringLiteral("How lines line up when the text has more than one line."));
  textDetailBox = new QComboBox(shapePage);
  for (int i = 0; i < textDetailCount(); ++i) textDetailBox->addItem(textDetailName(i));
  textDetailBox->setToolTip(QStringLiteral("How closely curves follow the font. Finer is smoother and uses more triangles."));
  shapeForm->addRow("Text", textEdit);
  shapeForm->addRow("Font", fontBox);
  shapeForm->addRow("Style", fontStyleBox);
  shapeForm->addRow("Font size mm", fontSizeSpin);
  shapeForm->addRow("Letter spacing mm", letterSpacingSpin);
  // Text on a curve: along a circle of this radius, over the top or along the bottom.
  textArcSpin = makeSpin(0, 5000, 1);
  textArcSpin->setSpecialValueText(QStringLiteral("Straight"));
  textArcSpin->setToolTip(QStringLiteral("Bends the text along a circle of this radius (through the middle of the letters), "
                                         "for badges, coins and round name plates. Straight: no curve."));
  textArcBox = new QComboBox(shapePage);
  textArcBox->addItems({QStringLiteral("Over the top"), QStringLiteral("Along the bottom")});
  textArcBox->setToolTip(QStringLiteral("Over the top of the circle, or along its bottom (the letters still stand upright), "
                                        "like the two halves of the writing on a coin."));
  shapeForm->addRow("Align lines", textAlignBox);
  shapeForm->addRow("Smoothness", textDetailBox);
  shapeForm->addRow("Curve radius mm", textArcSpin);
  shapeForm->addRow("Curve", textArcBox);
  for (QWidget* field : std::initializer_list<QWidget*>{textEdit, fontBox, fontStyleBox, fontSizeSpin, letterSpacingSpin,
                                                         textAlignBox, textDetailBox, textArcSpin, textArcBox}) {
    field->hide();
    if (QWidget* label = shapeForm->labelForField(field)) label->hide();
  }
  textTimer = new QTimer(this);
  textTimer->setSingleShot(true);
  textTimer->setInterval(250);
  auto selectedText = [this]() -> Node* {
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    return node && node->shape == QLatin1String("text") ? node : nullptr;
  };
  // New text changes the part's size; show it without reloading the whole panel (that would fight
  // with whatever box the user is typing in).
  auto showTextSize = [this, selectedText] {
    const Node* node = selectedText();
    double lo[3];
    double hi[3];
    if (!node || !localBounds(doc, *node, lo, hi)) return;
    for (int i = 0; i < 3; ++i) {
      const QSignalBlocker blocker(sizeSpin[i]);
      sizeSpin[i]->setValue(std::max(0.05, (hi[i] - lo[i]) * std::abs(node->scale[i])));
    }
  };
  connect(textTimer, &QTimer::timeout, this, [this, showTextSize] {
    refreshScene();
    showTextSize();
  });
  connect(textEdit, &QPlainTextEdit::textChanged, this, [this, selectedText] {
    if (loading) return;
    Node* node = selectedText();
    if (!node) return;
    const QString text = textEdit->toPlainText();
    if (text == node->text) return;
    node->text = text;
    textTimer->start();  // rebuild once typing pauses
  });
  connect(fontBox, &QFontComboBox::currentFontChanged, this, [this, selectedText, showTextSize](const QFont& font) {
    if (loading) return;
    Node* node = selectedText();
    if (!node || node->fontFamily == font.family()) return;
    pushUndo();
    node->fontFamily = font.family();
    fillFontStyles(node->fontFamily, node->fontStyle);
    node->fontStyle = fontStyleBox->currentText();
    refreshScene();
    showTextSize();
  });
  connect(fontStyleBox, &QComboBox::currentTextChanged, this, [this, selectedText, showTextSize](const QString& style) {
    if (loading) return;
    Node* node = selectedText();
    if (!node || style.isEmpty() || node->fontStyle == style) return;
    pushUndo();
    node->fontStyle = style;
    refreshScene();
    showTextSize();
  });
  bindSpin(fontSizeSpin, [selectedText](double value) {
    if (Node* node = selectedText()) node->fontSize = value;
  });
  bindSpin(letterSpacingSpin, [selectedText](double value) {
    if (Node* node = selectedText()) node->letterSpacing = value;
  });
  bindSpin(textArcSpin, [selectedText](double value) {
    if (Node* node = selectedText()) node->textArc = value;
  });
  connect(textArcBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, selectedText, showTextSize](int index) {
    if (loading) return;
    Node* node = selectedText();
    if (!node || node->textArcBelow == (index == 1)) return;
    pushUndo();
    node->textArcBelow = index == 1;
    refreshScene();
    showTextSize();
  });
  connect(textArcSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double value) { textArcBox->setEnabled(value > 0.01); });
  for (QDoubleSpinBox* box : {fontSizeSpin, letterSpacingSpin, textArcSpin}) {
    connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, showTextSize](double) {
      if (!loading) showTextSize();
    });
  }
  connect(textAlignBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, selectedText, showTextSize](int index) {
    if (loading) return;
    Node* node = selectedText();
    if (!node || node->textAlign == index) return;
    pushUndo();
    node->textAlign = index;
    refreshScene();
    showTextSize();
  });
  connect(textDetailBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, selectedText, showTextSize](int index) {
    if (loading) return;
    Node* node = selectedText();
    if (!node || node->textDetail == index) return;
    pushUndo();
    node->textDetail = index;
    refreshScene();
    showTextSize();
  });
  segmentsSpin = new QSpinBox(shapePage);
  segmentsSpin->setRange(8, 256);
  segmentsSpin->setSingleStep(4);
  segmentsSpin->setValue(128);
  segmentsSpin->installEventFilter(this);
  shapeForm->addRow("Curve segments", segmentsSpin);
  bindSpin(radiusSpin, [this](double value) {
    if (Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr) {
      node->radius = value;
      if (node->shape == QLatin1String("facefillet") && !node->filletOp.isEmpty()) {
        for (Node& other : doc.nodes)
          if (other.filletOp == node->filletOp) other.radius = value;
      }
    }
  });
  bindSpin(radius2Spin, [this](double value) {
    if (Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr) node->radius2 = value;
  });
  connect(sidesSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
    if (loading) return;
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    const bool star = node && (node->shape == QLatin1String("star") || node->shape == QLatin1String("starflat"));
    if (!node || (node->shape != QLatin1String("prism") && node->shape != QLatin1String("diamond") &&
                  node->shape != QLatin1String("cylinder") && node->shape != QLatin1String("facefillet") && !star))
      return;
    int minimum = 3;
    int maximum = 256;
    if (node->shape == QLatin1String("diamond")) minimum = 4;
    if (star) maximum = 64;
    if (node->shape == QLatin1String("facefillet")) {
      minimum = 1;
      maximum = 20;
    }
    node->sides = std::clamp(value, minimum, maximum);
    if (node->shape == QLatin1String("facefillet") && !node->filletOp.isEmpty()) {
      for (Node& other : doc.nodes)
        if (other.filletOp == node->filletOp) other.sides = node->sides;
    }
    refreshScene();
  });
  connect(segmentsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
    if (loading) return;
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    if (!node) return;
    node->segments = std::clamp(value, 8, 256);
    refreshScene();
  });
  form->addRow(shapePage);

  filletPopup = new QFrame(host);
  filletPopup->setObjectName("filletPopup");
  filletPopup->setFixedWidth(248);
  auto* filletLayout = new QVBoxLayout(filletPopup);
  filletLayout->setContentsMargins(10, 10, 10, 10);
  filletLayout->setSpacing(6);
  auto* filletTitle = new QLabel("Fillet", filletPopup);
  filletTitle->setStyleSheet("font-weight: 600; color: #e87400; background: transparent;");
  filletLayout->addWidget(filletTitle);
  filletCount = new QLabel("No edges yet", filletPopup);
  filletLayout->addWidget(filletCount);
  filletHint = new QLabel(filletPopup);
  filletHint->setWordWrap(true);
  filletHint->setText("Click an edge. Shift adds or removes one. Apply rounds that selection.");
  filletLayout->addWidget(filletHint);
  auto* filletForm = new QFormLayout();
  filletForm->setContentsMargins(0, 0, 0, 0);
  edgeSpin = new QDoubleSpinBox(filletPopup);
  edgeSpin->setRange(0, 200);
  edgeSpin->setDecimals(3);
  edgeSpin->setSingleStep(0.5);
  surfaceSpin = new QDoubleSpinBox(filletPopup);
  surfaceSpin->setRange(0, 200);
  surfaceSpin->setDecimals(3);
  surfaceSpin->setSingleStep(0.5);
  filletSegSpin = new QSpinBox(filletPopup);
  filletSegSpin->setRange(8, 72);
  filletSegSpin->setSingleStep(2);
  filletSegSpin->setValue(std::clamp(doc.filletSegments, 8, 72));
  filletForm->addRow("Edge mm", edgeSpin);
  filletForm->addRow("Surface mm", surfaceSpin);
  filletForm->addRow("Segments", filletSegSpin);
  filletLayout->addLayout(filletForm);
  auto* filletButtons = new QHBoxLayout();
  auto* applyFilletButton = new QPushButton("Apply", filletPopup);
  auto* allEdgesButton = new QPushButton("All edges", filletPopup);
  auto* clearFilletButton = new QPushButton("Clear", filletPopup);
  filletButtons->addWidget(applyFilletButton);
  filletButtons->addWidget(allEdgesButton);
  filletButtons->addWidget(clearFilletButton);
  filletLayout->addLayout(filletButtons);
  filletPopup->hide();
  connect(applyFilletButton, &QPushButton::clicked, this, [this] { applyFillet(false); });
  connect(allEdgesButton, &QPushButton::clicked, this, [this] { applyFillet(true); });
  connect(clearFilletButton, &QPushButton::clicked, this, [this] { clearFillet(); });

  faceFilletPopup = new QFrame(host);
  faceFilletPopup->setObjectName("faceFilletPopup");
  faceFilletPopup->setFixedWidth(248);
  auto* faceFilletLayout = new QVBoxLayout(faceFilletPopup);
  faceFilletLayout->setContentsMargins(10, 10, 10, 10);
  faceFilletLayout->setSpacing(6);
  auto* faceFilletTitle = new QLabel("Fillet surface", faceFilletPopup);
  faceFilletTitle->setStyleSheet("font-weight: 600; color: #e87400; background: transparent;");
  faceFilletLayout->addWidget(faceFilletTitle);
  auto* faceFilletHint = new QLabel("Click a face. Its outer edge and each hole get their own fillet object. Inner rounds an edge (group it with the solid to cut). Outer fills an inside corner (group it to add).", faceFilletPopup);
  faceFilletHint->setWordWrap(true);
  faceFilletLayout->addWidget(faceFilletHint);
  auto* faceFilletForm = new QFormLayout();
  faceFilletForm->setContentsMargins(0, 0, 0, 0);
  faceFilletRadius = new QDoubleSpinBox(faceFilletPopup);
  faceFilletRadius->setRange(0.1, 100);
  faceFilletRadius->setDecimals(3);
  faceFilletRadius->setSingleStep(0.5);
  faceFilletRadius->setValue(2);
  faceFilletSegs = new QSpinBox(faceFilletPopup);
  faceFilletSegs->setRange(1, 20);
  faceFilletSegs->setValue(12);
  faceFilletForm->addRow("Radius mm", faceFilletRadius);
  faceFilletForm->addRow("Segments", faceFilletSegs);
  faceFilletSide = new QComboBox(faceFilletPopup);
  faceFilletSide->addItems({"Inner", "Outer"});
  faceFilletDir = new QComboBox(faceFilletPopup);
  faceFilletDir->addItems({"Down", "Up"});
  connect(faceFilletSide, qOverload<int>(&QComboBox::currentIndexChanged), faceFilletPopup, [this](int index) {
    if (faceFilletDir) faceFilletDir->setCurrentIndex(index == 1 ? 1 : 0);
  });
  faceFilletForm->addRow("Inner/Outer", faceFilletSide);
  faceFilletForm->addRow("Up/Down", faceFilletDir);
  faceFilletLayout->addLayout(faceFilletForm);
  faceFilletPopup->hide();

  edgeBetaPopup = new QFrame(host);
  edgeBetaPopup->setObjectName("faceFilletPopup");  // same look as the other fillet popups
  edgeBetaPopup->setFixedWidth(260);
  auto* edgeBetaLayout = new QVBoxLayout(edgeBetaPopup);
  edgeBetaLayout->setContentsMargins(10, 10, 10, 10);
  edgeBetaLayout->setSpacing(6);
  auto* edgeBetaTitle = new QLabel(QStringLiteral("Round or chamfer edges (beta)"), edgeBetaPopup);
  edgeBetaTitle->setStyleSheet("font-weight: 600; color: #e87400; background: transparent;");
  edgeBetaLayout->addWidget(edgeBetaTitle);
  edgeBetaCount = new QLabel(QStringLiteral("No edges yet"), edgeBetaPopup);
  edgeBetaCount->setStyleSheet("font-weight: 600; background: transparent;");
  edgeBetaLayout->addWidget(edgeBetaCount);
  edgeBetaHint = new QLabel(edgeBetaPopup);
  edgeBetaHint->setWordWrap(true);
  edgeBetaLayout->addWidget(edgeBetaHint);
  auto* edgeBetaForm = new QFormLayout();
  edgeBetaForm->setContentsMargins(0, 0, 0, 0);
  edgeBetaRadius = new QDoubleSpinBox(edgeBetaPopup);
  edgeBetaRadius->setRange(0.05, 100);
  edgeBetaRadius->setDecimals(2);
  edgeBetaRadius->setSingleStep(0.5);
  edgeBetaRadius->setValue(2);
  edgeBetaSegSpin = new QSpinBox(edgeBetaPopup);
  edgeBetaSegSpin->setRange(2, 48);
  edgeBetaSegSpin->setValue(12);
  edgeBetaSegSpin->setToolTip(QStringLiteral("Steps per quarter turn of the round. 12 is smooth; fewer is faster."));
  edgeBetaShape = new QComboBox(edgeBetaPopup);
  edgeBetaShape->addItem(QStringLiteral("Round (fillet)"));
  edgeBetaShape->addItem(QStringLiteral("Flat (chamfer)"));
  edgeBetaShape->setToolTip(QStringLiteral("Round the picked edges, or cut them off flat. A chamfer of 1 mm on a square edge "
                                           "goes 1 mm back along both faces (45 degrees)."));
  edgeBetaForm->addRow(QStringLiteral("Edge"), edgeBetaShape);
  edgeBetaForm->addRow(QStringLiteral("Radius mm"), edgeBetaRadius);
  connect(edgeBetaShape, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, edgeBetaForm](int index) {
    if (auto* label = qobject_cast<QLabel*>(edgeBetaForm->labelForField(edgeBetaRadius)))
      label->setText(index == 1 ? QStringLiteral("Size mm") : QStringLiteral("Radius mm"));
    edgeBetaForm->setRowVisible(edgeBetaSegSpin, index == 0);
  });
  edgeBetaForm->addRow(QStringLiteral("Segments"), edgeBetaSegSpin);
  edgeBetaLayout->addLayout(edgeBetaForm);
  auto* edgeBetaApply = new QPushButton(QStringLiteral("Apply"), edgeBetaPopup);
  auto* edgeBetaNone = new QPushButton(QStringLiteral("Pick none"), edgeBetaPopup);
  auto* edgeBetaClear = new QPushButton(QStringLiteral("Remove all"), edgeBetaPopup);
  edgeBetaApply->setToolTip(QStringLiteral("Round or chamfer the picked edges. Edges you un-picked go back to sharp."));
  edgeBetaNone->setToolTip(QStringLiteral("Un-pick every edge (nothing changes until Apply)."));
  edgeBetaClear->setToolTip(QStringLiteral("Remove every edge fillet from this object."));
  auto* edgeBetaButtons = new QHBoxLayout();
  edgeBetaButtons->addWidget(edgeBetaApply);
  edgeBetaButtons->addWidget(edgeBetaNone);
  edgeBetaButtons->addWidget(edgeBetaClear);
  edgeBetaLayout->addLayout(edgeBetaButtons);
  connect(edgeBetaApply, &QPushButton::clicked, this, [this] { applyEdgeBetaFillet(); });
  connect(edgeBetaNone, &QPushButton::clicked, this, [this] {
    edgeBetaPending.clear();
    syncEdgeBetaChrome();
  });
  connect(edgeBetaClear, &QPushButton::clicked, this, [this] { clearEdgeBetaFillet(); });
  edgeBetaPopup->hide();

  repeatPopup = new QFrame(host);
  repeatPopup->setObjectName("repeatPopup");
  repeatPopup->setFixedWidth(260);
  auto* repeatLayout = new QVBoxLayout(repeatPopup);
  repeatLayout->setContentsMargins(10, 10, 10, 10);
  repeatLayout->setSpacing(6);
  auto* repeatTitle = new QLabel("Repeat", repeatPopup);
  repeatTitle->setStyleSheet("font-weight: 600; color: #e87400; background: transparent;");
  repeatLayout->addWidget(repeatTitle);
  auto* repeatHint = new QLabel("Makes separate copies. The original stays put.", repeatPopup);
  repeatHint->setWordWrap(true);
  repeatLayout->addWidget(repeatHint);
  auto* repeatForm = new QFormLayout();
  repeatForm->setContentsMargins(0, 0, 0, 0);
  repeatCount = new QSpinBox(repeatPopup);
  repeatCount->setRange(1, 48);
  repeatCount->setValue(3);
  repeatForm->addRow("Copies", repeatCount);
  for (int i = 0; i < 3; ++i) {
    repeatMove[i] = new QDoubleSpinBox(repeatPopup);
    repeatMove[i]->setRange(-10000, 10000);
    repeatMove[i]->setDecimals(2);
    repeatMove[i]->setValue(i == 0 ? 40 : 0);
    repeatForm->addRow(QString("Move %1").arg(axes[i]), repeatMove[i]);
  }
  for (int i = 0; i < 3; ++i) {
    repeatRot[i] = new QDoubleSpinBox(repeatPopup);
    repeatRot[i]->setRange(-360, 360);
    repeatRot[i]->setDecimals(1);
    repeatForm->addRow(QString("Rotate %1").arg(axes[i]), repeatRot[i]);
  }
  for (int i = 0; i < 3; ++i) {
    repeatResize[i] = new QDoubleSpinBox(repeatPopup);
    repeatResize[i]->setRange(0.05, 10);
    repeatResize[i]->setDecimals(3);
    repeatResize[i]->setSingleStep(0.1);
    repeatResize[i]->setValue(1);
    repeatForm->addRow(QString("Resize %1").arg(axes[i]), repeatResize[i]);
  }
  repeatLayout->addLayout(repeatForm);
  auto* runRepeatButton = new QPushButton("Run", repeatPopup);
  repeatLayout->addWidget(runRepeatButton);
  repeatPopup->hide();
  connect(runRepeatButton, &QPushButton::clicked, this, [this] { runRepeat(); });

  // The section view's bar, along the bottom of the view.
  sectionBar = new QFrame(host);
  sectionBar->setObjectName("sectionBar");
  {
    auto* row = new QHBoxLayout(sectionBar);
    row->setContentsMargins(10, 6, 6, 6);
    row->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("<b>Section</b>"), sectionBar);
    sectionAxisBox = new QComboBox(sectionBar);
    sectionAxisBox->addItems({QStringLiteral("X (left to right)"), QStringLiteral("Y (front to back)"), QStringLiteral("Z (bottom to top)")});
    sectionAxisBox->setCurrentIndex(1);
    sectionAxisBox->setToolTip(QStringLiteral("Which way the cut runs through the model."));
    sectionSlider = new QSlider(Qt::Horizontal, sectionBar);  // tenths of a millimetre across the print area
    sectionSlider->setRange(-1280, 1280);
    sectionSlider->setValue(0);
    sectionSlider->setMinimumWidth(300);
    sectionSlider->setToolTip(QStringLiteral("Drag the cut through the model."));
    sectionSpin = new QDoubleSpinBox(sectionBar);
    sectionSpin->setRange(-10000, 10000);
    sectionSpin->setDecimals(2);
    sectionSpin->setSuffix(QStringLiteral(" mm"));
    sectionSpin->setFixedWidth(110);
    sectionFlipButton = new QToolButton(sectionBar);
    sectionFlipButton->setText(QStringLiteral("Flip"));
    sectionFlipButton->setCheckable(true);
    sectionFlipButton->setToolTip(QStringLiteral("Keep the other side of the cut."));
    auto* close = new QToolButton(sectionBar);
    close->setText(QStringLiteral("\u2715"));
    close->setAutoRaise(true);
    close->setToolTip(QStringLiteral("Close the section view."));
    for (QWidget* widget : std::initializer_list<QWidget*>{title, sectionAxisBox, sectionSlider, sectionSpin, sectionFlipButton, close})
      row->addWidget(widget);
    connect(sectionAxisBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateSection(true); });
    connect(sectionSlider, &QSlider::valueChanged, this, [this](int value) {
      sectionSpin->blockSignals(true);
      sectionSpin->setValue(value / 10.0);
      sectionSpin->blockSignals(false);
      updateSection(false);
    });
    connect(sectionSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double at) {
      sectionSlider->blockSignals(true);
      sectionSlider->setValue(static_cast<int>(std::round(at * 10)));
      sectionSlider->blockSignals(false);
      updateSection(false);
    });
    connect(sectionFlipButton, &QToolButton::toggled, this, [this] { updateSection(false); });
    connect(close, &QToolButton::clicked, this, [this] { setSection(false); });
  }
  sectionBar->hide();

  busyWrap = new QFrame(host);
  busyWrap->setObjectName("busyWrap");
  auto* busyLayout = new QHBoxLayout(busyWrap);
  busyLayout->setContentsMargins(8, 5, 8, 5);
  busyLayout->setSpacing(8);
  auto* busyLabel = new QLabel("Synthesizing", busyWrap);
  busy = new QProgressBar(busyWrap);
  busy->setObjectName("busyBar");
  busy->setRange(0, 0);
  busy->setTextVisible(false);
  busy->setFixedSize(148, 10);
  busyLayout->addWidget(busyLabel);
  busyLayout->addWidget(busy);
  busyWrap->hide();

  bendPage = new QWidget(formHost);
  auto* bendForm = new QFormLayout(bendPage);
  bendForm->setContentsMargins(0, 0, 0, 0);
  bendSpin = makeSpin(-360, 360, 5);
  alongBox = new QComboBox(bendPage);
  towardBox = new QComboBox(bendPage);
  for (auto* box : {alongBox, towardBox}) box->addItems({"X", "Y", "Z"});
  bendForm->addRow("Bend deg", bendSpin);
  bendForm->addRow("Along", alongBox);
  bendForm->addRow("Toward", towardBox);
  bindSpin(bendSpin, [this](double value) {
    if (Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr) node->bend = value;
  });
  connect(alongBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    if (loading) return;
    if (Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr) node->along = index;
    refreshScene();
  });
  connect(towardBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    if (loading) return;
    if (Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr) node->toward = index;
    refreshScene();
  });
  // Twist: wring the part around one axis, from one end to the other. Made before the bend, so a
  // twisted strip can also be curved.
  twistSpin = makeSpin(-3600, 3600, 15);
  twistSpin->setToolTip(QStringLiteral("Degrees the part turns from one end to the other. 180 is a half turn, 720 two full turns. 0 is off."));
  twistAxisBox = new QComboBox(bendPage);
  twistAxisBox->addItems({"X", "Y", "Z"});
  twistAxisBox->setToolTip(QStringLiteral("The axis it twists around: the long direction of a strip."));
  bendForm->addRow("Twist deg", twistSpin);
  bendForm->addRow("Twist around", twistAxisBox);
  bindSpin(twistSpin, [this](double value) {
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    if (!node) return;
    // Switching twist on: keep the axis shown (the part's longest side unless picked otherwise).
    if (std::abs(node->twist) < 0.05) node->twistAxis = twistAxisBox->currentIndex();
    node->twist = value;
  });
  connect(twistAxisBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    if (loading) return;
    if (Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr) node->twistAxis = index;
    refreshScene();
  });
  // Taper: one end narrower (or wider) than the other. Made first, so a tapered part can still be
  // twisted and bent.
  taperSpin = makeSpin(1, 1000, 5);
  taperSpin->setSuffix(QStringLiteral(" %"));
  taperSpin->setToolTip(QStringLiteral("How wide the far (high) end is, in percent of the near end: 50 is half as wide, 0 "
                                       "would be a point, 200 twice as wide. 100 is off."));
  taperAxisBox = new QComboBox(bendPage);
  taperAxisBox->addItems({"X", "Y", "Z"});
  taperAxisBox->setToolTip(QStringLiteral("The axis it tapers along: the far end is its high side."));
  bendForm->addRow("Taper end", taperSpin);
  bendForm->addRow("Taper along", taperAxisBox);
  bindSpin(taperSpin, [this](double value) {
    if (Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr) node->taper = value;
  });
  connect(taperAxisBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    if (loading) return;
    if (Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr) node->taperAxis = index;
    refreshScene();
  });
  form->addRow(bendPage);

  facePage = new QWidget(formHost);
  auto* faceForm = new QFormLayout(facePage);
  faceForm->setContentsMargins(0, 0, 0, 0);
  faceAngleSpin = makeSpin(-3600, 3600, 5);
  faceRadiusSpin = makeSpin(0, 2000, 1);
  faceRiseSpin = makeSpin(-200, 200, 0.5);
  faceStepsSpin = new QSpinBox(facePage);
  faceStepsSpin->setRange(2, 256);
  faceStepsSpin->setValue(draftSteps);
  faceStepsSpin->installEventFilter(this);
  faceAxisBox = new QComboBox(facePage);
  faceAxisBox->addItems({"From face", "X", "Y", "Z"});
  facePresetBox = new QComboBox(facePage);
  facePresetBox->addItems({"0°", "-90°", "90°", "180°", "270°", "360°", "-270°", "-360°", "Custom"});
  faceSpiralBox = new QCheckBox("Spiral", facePage);
  faceRadialSpin = makeSpin(-10000, 10000, 1);
  faceRadialSpin->setDecimals(3);
  faceRadialSpin->setValue(0);
  faceRadialSpin->setVisible(false);
  faceAngleSpin->setRange(-3600, 3600);
  faceAngleSpin->setSingleStep(90);
  faceAngleSpin->setValue(draftAngle);
  faceRadiusSpin->setValue(draftRadius);
  faceRiseSpin->setValue(draftRise);
  faceForm->addRow("Turn", facePresetBox);
  faceForm->addRow("Turn deg", faceAngleSpin);
  faceForm->addRow("Radius mm", faceRadiusSpin);
  faceRadiusSpin->setToolTip(
      "The inside radius of the bend: from the face's inner edge to the line it turns round. The whole face turns, so the "
      "end is as long as the face; a bigger radius gives a rounder inside.");
  faceForm->addRow("Steps", faceStepsSpin);
  faceForm->addRow("Z per step", faceRiseSpin);
  faceRiseSpin->setToolTip(
      "Millimetres of slide along the hinge over one full turn. "
      "A 90° bend moves a quarter of this. It does not climb by this amount on every step.");
  faceForm->addRow("Spiral", faceSpiralBox);
  faceForm->addRow("Pitch mm / turn", faceRadialSpin);
  faceRadialSpin->setToolTip(
      "Helix pitch along the face normal. At 360° the end sits this far off the face "
      "the bend was taken from, above or below that part. It does not grow the radius.");
  faceForm->addRow("Hinge", faceAxisBox);
  connect(facePresetBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    if (loading || index < 0 || index > 7) return;
    static const double presets[] = {0.0, -90.0, 90.0, 180.0, 270.0, 360.0, -270.0, -360.0};
    updatingFacePreset = true;
    faceAngleSpin->setValue(presets[index]);
    updatingFacePreset = false;
  });
  auto faceNode = [this]() -> Node* {
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    return node && node->shape == QLatin1String("elbow") ? node : nullptr;
  };
  bindSpin(faceAngleSpin, [this, faceNode](double value) {
    if (!updatingFacePreset && facePresetBox) {
      const double presetValues[] = {0.0, -90.0, 90.0, 180.0, 270.0, 360.0, -270.0, -360.0};
      int preset = 8;
      for (int i = 0; i < 8; ++i) {
        if (std::abs(value - presetValues[i]) < 0.001) { preset = i; break; }
      }
      facePresetBox->setCurrentIndex(preset);
    }
    if (Node* node = faceNode()) node->faceAngle = value;
    else draftAngle = value;
  });
  bindSpin(faceRadiusSpin, [this, faceNode](double value) {
    if (Node* node = faceNode()) node->faceRadius = value;
    else draftRadius = value;
  });
  bindSpin(faceRiseSpin, [this, faceNode](double value) {
    if (Node* node = faceNode()) node->faceRise = value;
    else draftRise = value;
  });
  connect(faceStepsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this, faceNode](int value) {
    if (loading) return;
    if (Node* node = faceNode()) node->faceSteps = value;
    else draftSteps = value;
    if (faceNode()) refreshScene();
  });
  connect(faceAxisBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, faceNode](int index) {
    if (loading) return;
    if (Node* node = faceNode()) node->faceAxisMode = index;
    else draftAxis = index;
    if (faceNode()) refreshScene();
  });
  connect(faceSpiralBox, &QCheckBox::toggled, this, [this, faceNode](bool on) {
    faceRadialSpin->setVisible(on);
    if (loading) return;
    if (Node* node = faceNode()) node->faceSpiral = on;
    if (faceNode()) refreshScene();
  });
  bindSpin(faceRadialSpin, [this, faceNode](double value) {
    if (Node* node = faceNode()) node->faceRadialStep = value;
  });
  form->addRow(facePage);

  extrudePage = new QWidget(formHost);
  auto* extrudeForm = new QFormLayout(extrudePage);
  extrudeForm->setContentsMargins(0, 0, 0, 0);
  extrudeDepthSpin = makeSpin(-1000, 1000, 1);
  extrudeDepthSpin->setValue(10);
  extrudeForm->addRow("Depth mm", extrudeDepthSpin);
  extrudeForm->addRow(new QLabel("Click a face in the viewport to create and join the extrusion."));
  form->addRow(extrudePage);

  mirrorPage = new QWidget(formHost);
  auto* mirrorForm = new QFormLayout(mirrorPage);
  mirrorForm->setContentsMargins(0, 0, 0, 0);
  auto* mirrorRow = new QWidget(mirrorPage);
  auto* mirrorLayout = new QHBoxLayout(mirrorRow);
  mirrorLayout->setContentsMargins(0, 0, 0, 0);
  for (int i = 0; i < 3; ++i) {
    mirrorBox[i] = new QCheckBox(QString(axes[i]), mirrorRow);
    mirrorLayout->addWidget(mirrorBox[i]);
    connect(mirrorBox[i], &QCheckBox::toggled, this, [this, i](bool on) {
      if (loading) return;
      Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
      if (!node || node->kind != Node::Kind::Shape) return;
      pushUndo();
      node->mirror[i] = on;
      refreshScene();
    });
  }
  mirrorForm->addRow("Flip", mirrorRow);
  form->addRow(mirrorPage);

  patternPage = new QWidget(formHost);
  auto* patternForm = new QFormLayout(patternPage);
  countSpin = new QSpinBox(patternPage);
  countSpin->setRange(1, 48);
  countSpin->installEventFilter(this);
  patternForm->addRow("Copies", countSpin);
  connect(countSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
    if (loading) return;
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    if (!node || node->kind != Node::Kind::Pattern) return;
    node->count = value;
    node->name = QString("Repeat ×%1").arg(value);
    refreshScene();
    refreshTree();
  });
  for (int i = 0; i < 3; ++i) {
    moveSpin[i] = makeSpin(-10000, 10000, 1);
    drotSpin[i] = makeSpin(-360, 360, 5);
    resizeSpin[i] = makeSpin(0.05, 10, 0.05);
    patternForm->addRow(QString("Move %1").arg(axes[i]), moveSpin[i]);
    patternForm->addRow(QString("Rotate %1").arg(axes[i]), drotSpin[i]);
    patternForm->addRow(QString("Resize %1").arg(axes[i]), resizeSpin[i]);
    bindSpin(moveSpin[i], [this, i](double value) {
      if (Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr) node->move[i] = value;
    }, true);
    bindSpin(drotSpin[i], [this, i](double value) {
      if (Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr) node->drot[i] = value;
    }, true);
    bindSpin(resizeSpin[i], [this, i](double value) {
      if (Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr) node->resize[i] = value;
    }, true);
  }
  form->addRow(patternPage);

  platePage = new QWidget(formHost);
  auto* docForm = new QFormLayout(platePage);
  docForm->setContentsMargins(0, 0, 0, 0);
  workplaneSpin = makeSpin(-1000, 1000, 1);
  snapSpin = makeSpin(0.1, 100, 0.5);
  docForm->addRow("Workplane Z", workplaneSpin);
  bedPreset = new QComboBox(platePage);
  bedPreset->addItems({"180 × 180", "220 × 220", "250 × 210", "256 × 256", "300 × 300", "Custom"});
  bedXSpin = makeSpin(20, 2000, 1);
  bedYSpin = makeSpin(20, 2000, 1);
  docForm->addRow("Build plate", bedPreset);
  docForm->addRow("Plate X mm", bedXSpin);
  docForm->addRow("Plate Y mm", bedYSpin);
  docForm->addRow("Snap mm", snapSpin);
  bindSpin(workplaneSpin, [this](double value) { doc.workplaneZ = value; }, true);
  connect(bedPreset, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    if (loading) return;
    const double presets[][2] = {{180, 180}, {220, 220}, {250, 210}, {256, 256}, {300, 300}};
    if (index < 0 || index > 4) return;
    doc.bedX = presets[index][0];
    doc.bedY = presets[index][1];
    loading = true;
    bedXSpin->setValue(doc.bedX);
    bedYSpin->setValue(doc.bedY);
    loading = false;
    view->setBed(doc.bedX, doc.bedY);
  });
  bindSpin(bedXSpin, [this](double value) {
    doc.bedX = value;
    view->setBed(doc.bedX, doc.bedY);
    if (!loading && bedPreset->currentIndex() != 5) {
      loading = true;
      bedPreset->setCurrentIndex(5);
      loading = false;
    }
  });
  bindSpin(bedYSpin, [this](double value) {
    doc.bedY = value;
    view->setBed(doc.bedX, doc.bedY);
    if (!loading && bedPreset->currentIndex() != 5) {
      loading = true;
      bedPreset->setCurrentIndex(5);
      loading = false;
    }
  });
  bindSpin(snapSpin, [this](double value) {
    doc.snap = value;
    view->setSnap(doc.snapOn, doc.snap);
    if (snapBar && std::abs(snapBar->value() - value) > 1e-6) {
      snapBar->blockSignals(true);
      snapBar->setValue(value);
      snapBar->blockSignals(false);
    }
  }, true);
  form->addRow(platePage);

  auto detachFormRow = [form](QWidget* widget) {
    if (!widget) return;
    int row = -1;
    QFormLayout::ItemRole role = QFormLayout::SpanningRole;
    form->getWidgetPosition(widget, &row, &role);
    if (row < 0) {
      for (int i = 0; i < form->rowCount(); ++i) {
        QLayoutItem* field = form->itemAt(i, QFormLayout::FieldRole);
        QLayoutItem* span = form->itemAt(i, QFormLayout::SpanningRole);
        if ((field && field->widget() == widget) || (span && span->widget() == widget)) {
          row = i;
          break;
        }
      }
    }
    if (row < 0) return;
    const QFormLayout::TakeRowResult taken = form->takeRow(row);
    QWidget* label = taken.labelItem ? taken.labelItem->widget() : nullptr;
    delete taken.labelItem;
    delete taken.fieldItem;
    if (label) delete label;
  };
  detachFormRow(colorButton);
  for (int i = 0; i < 3; ++i) detachFormRow(posSpin[i]);
  detachFormRow(rotPage);
  detachFormRow(shapePage);
  detachFormRow(bendPage);
  detachFormRow(facePage);
  detachFormRow(extrudePage);
  detachFormRow(mirrorPage);
  detachFormRow(patternPage);
  detachFormRow(platePage);

  colorPopup = new QFrame(host);
  colorPopup->setObjectName("colorPopup");
  colorPopup->setFixedWidth(280);
  auto* colorLayout = new QHBoxLayout(colorPopup);
  colorLayout->setContentsMargins(10, 8, 10, 8);
  colorLayout->setSpacing(8);
  auto* colorTitle = new QLabel("Color", colorPopup);
  colorTitle->setStyleSheet("font-weight: 600; color: #e87400; background: transparent;");
  colorLayout->addWidget(colorTitle);
  colorButton->setParent(colorPopup);
  colorLayout->addWidget(colorButton, 1);
  colorPopup->show();

  toolPopup = new QFrame(host);
  toolPopup->setObjectName("toolPopup");
  toolPopup->setFixedWidth(280);
  auto* toolLayout = new QVBoxLayout(toolPopup);
  toolLayout->setContentsMargins(10, 8, 10, 8);
  toolLayout->setSpacing(6);
  toolTitle = new QLabel("Move", toolPopup);
  toolTitle->setStyleSheet("font-weight: 600; color: #e87400; background: transparent;");
  toolHint = new QLabel(toolPopup);
  toolHint->setWordWrap(true);
  toolHint->setFixedWidth(260);
  toolLayout->addWidget(toolTitle);
  toolLayout->addWidget(toolHint);
  movePage = new QWidget(toolPopup);
  auto* moveForm = new QFormLayout(movePage);
  moveForm->setContentsMargins(0, 0, 0, 0);
  const char* moveAxes = "XYZ";
  for (int i = 0; i < 3; ++i) {
    posSpin[i]->setParent(movePage);
    moveForm->addRow(QString("Position %1").arg(moveAxes[i]), posSpin[i]);
  }
  toolLayout->addWidget(movePage);
  buildGeneratorPage();
  buildHolePage();
  buildHullPage();
  buildHollowPage();
  buildSplitPage();
  for (QWidget* page : {genPage, holePage, hullPage, hollowPage, splitPage, shapePage, rotPage, bendPage, facePage, extrudePage,
                        mirrorPage, patternPage}) {
    page->setParent(toolPopup);
    toolLayout->addWidget(page);
  }
  platePage->setParent(this);
  platePage->hide();
  toolPopup->hide();

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setWidget(formHost);
  formHost->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  scroll->setMaximumHeight(88);
  auto* split = new QSplitter(Qt::Vertical, this);
  split->addWidget(tree);
  split->addWidget(scroll);
  split->setStretchFactor(0, 1);
  split->setStretchFactor(1, 0);
  auto* left = new QDockWidget("Model Tree", this);
  left->setObjectName("modelDock");
  left->setWidget(split);
  addDockWidget(Qt::LeftDockWidgetArea, left);

  auto configureList = [](ShapeList* list) {
    list->setIconSize(QSize(28, 28));
    list->setCursor(Qt::ArrowCursor);
    list->viewport()->setCursor(Qt::ArrowCursor);
    list->setDragDropMode(QAbstractItemView::NoDragDrop);
  };
  shapes = new ShapeList(this);
  configureList(shapes);
  const struct {
    const char* label;
    const char* key;
    double fillet;
    const char* icon;
  } entries[] = {{"Cube", "box", 0, "box"},           {"Round box", "box", 2, "roundbox"},  {"Sphere", "sphere", 0, "sphere"},
                 {"Dome", "hemisphere", 0, "hemisphere"}, {"Cylinder", "cylinder", 0, "cylinder"}, {"Cone", "cone", 0, "cone"},
                 {"Tube", "tube", 0, "tube"},         {"Capsule", "capsule", 0, "capsule"}, {"Torus", "torus", 0, "torus"},
                 {"Diamond", "diamond", 0, "diamond"}, {"Heart", "heart", 0, "heart"},     {"Hex prism", "prism", 0, "prism"},
                 {"Pyramid", "pyramid", 0, "pyramid"}, {"Wedge", "wedge", 0, "wedge"},     {"Roof", "roof", 0, "roof"},
                 {"Star", "star", 0, "star"},         {"Flat star", "starflat", 0, "starflat"}, {"Text", "text", 0, "text"}};
  for (const auto& entry : entries) {
    auto* item = new QListWidgetItem(shapeIcon(entry.icon), entry.label, shapes);
    const QString payload = entry.fillet > 0 ? QString("%1|%2").arg(entry.key).arg(entry.fillet) : QString(entry.key);
    item->setData(Qt::UserRole, payload);
    item->setToolTip("Drag onto the bed, or click to place it.");
  }
  connect(shapes, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
    if (shapes->consumedDrag) { shapes->consumedDrag = false; return; }
    const QStringList bits = item->data(Qt::UserRole).toString().split('|');
    const int n = static_cast<int>(doc.nodes.size());
    placeShape(bits[0], ((n % 4) - 1.5) * 45.0, (n / 4 % 3) * 45.0, bits.size() > 1 ? bits[1].toDouble() : 0);
  });

  parts = new ShapeList(this);
  configureList(parts);
  parts->setItemDelegate(new LibraryItemDelegate(parts));
  auto* partsWrap = new QWidget(this);
  auto* partsLayout = new QVBoxLayout(partsWrap);
  auto* importStlButton = new QPushButton("Import STL…", partsWrap);
  partsLayout->addWidget(new QLabel("STL library", partsWrap));
  partsLayout->addWidget(importStlButton);

  auto* partsSearch = new QLineEdit(partsWrap);
  partsSearch->setObjectName(QStringLiteral("partsSearch"));
  partsSearch->setPlaceholderText(QStringLiteral("Search parts or tags…"));
  partsSearch->setClearButtonEnabled(true);
  partsLayout->addWidget(partsSearch);
  connect(partsSearch, &QLineEdit::textChanged, this, [this](const QString& text) { filterLibraryList(parts, text); });

  parts->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(parts, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    QListWidgetItem* item = parts->itemAt(parts->viewport()->mapFrom(parts, pos));
    if (!item) return;
    const QString payload = item->data(Qt::UserRole).toString();
    if (!payload.startsWith(QStringLiteral("stl:"))) return;
    QMenu menu(parts);
    QAction* editTags = menu.addAction(QStringLiteral("Edit tags…"));
    QAction* chosen = menu.exec(parts->viewport()->mapToGlobal(parts->viewport()->mapFrom(parts, pos)));
    if (chosen == editTags && editLibraryTags(this, partsDirectories, payload.mid(4))) refreshPartsLibrary();
  });

  partsLayout->addWidget(parts, 1);
  connect(importStlButton, &QPushButton::clicked, this, [this] {
    const QString start = partsDirectories.isEmpty() ? defaultAppdataPath(QStringLiteral("parts")) : partsDirectories.front();
    const QString path = QFileDialog::getOpenFileName(this, "Import STL", start, "STL mesh (*.stl)");
    if (!path.isEmpty()) {
      const QString copied = copyStlBeside(path, defaultAppdataPath(QStringLiteral("parts")));
      const QString used = copied.isEmpty() ? path : copied;
      if (!copied.isEmpty()) refreshPartsLibrary();
      const int n = static_cast<int>(doc.nodes.size());
      importStlPart(used, ((n % 4) - 1.5) * 45.0, (n / 4 % 3) * 45.0);
      if (copied.isEmpty())
        status(QStringLiteral("Imported the STL, but could not copy it into the parts folder."));
      else if (QDir::cleanPath(copied) != QDir::cleanPath(QFileInfo(path).absoluteFilePath()))
        status(QStringLiteral("Imported %1 and copied it into the parts folder.").arg(QFileInfo(used).completeBaseName()));
    }
  });
  connect(parts, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
    if (!item) return;
    if (item->data(Qt::UserRole).toString() == QLatin1String("header")) {
      toggleLibraryCategory(parts, item);
      return;
    }
    if (parts->consumedDrag) { parts->consumedDrag = false; return; }
    const QString payload = item->data(Qt::UserRole).toString();
    if (!payload.startsWith("stl:")) return;
    const int n = static_cast<int>(doc.nodes.size());
    importStlPart(payload.mid(4), ((n % 4) - 1.5) * 45.0, (n / 4 % 3) * 45.0);
  });

  shapes2d = new ShapeList(this);
  configureList(shapes2d);
  shapes2d->setItemDelegate(new LibraryItemDelegate(shapes2d));
  auto* shapes2dWrap = new QWidget(this);
  auto* shapes2dLayout = new QVBoxLayout(shapes2dWrap);
  auto* new2dButton = new QPushButton("New 2D shape…", shapes2dWrap);
  shapes2dLayout->addWidget(new2dButton);
  shapes2dLayout->addWidget(shapes2d, 1);
  connect(new2dButton, &QPushButton::clicked, this, [this] { create2DShape(); });
  connect(shapes2d, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
    if (!item) return;
    if (shapes2d->suppressClick) {
      shapes2d->suppressClick = false;
      return;
    }
    if (item->data(Qt::UserRole).toString() == QLatin1String("header")) {
      toggleLibraryCategory(shapes2d, item);
      return;
    }
    if (shapes2d->consumedDrag) { shapes2d->consumedDrag = false; return; }
    const QString payload = item->data(Qt::UserRole).toString();
    if (!payload.startsWith("svg:")) return;
    // Wait out the double-click time: a double-click opens the shape for editing and should not
    // also drop a copy on the bed.
    pending2DPlacement = payload.mid(4);
    pending2DTimer->start(QApplication::doubleClickInterval());
  });
  pending2DTimer = new QTimer(this);
  pending2DTimer->setSingleShot(true);
  connect(pending2DTimer, &QTimer::timeout, this, [this] {
    if (pending2DPlacement.isEmpty()) return;
    const QString path = pending2DPlacement;
    pending2DPlacement.clear();
    const int n = static_cast<int>(doc.nodes.size());
    importSvgShape(path, ((n % 4) - 1.5) * 45.0, (n / 4 % 3) * 45.0);
  });
  connect(shapes2d, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
    pending2DTimer->stop();
    pending2DPlacement.clear();
    if (!item) return;
    const QString payload = item->data(Qt::UserRole).toString();
    if (!payload.startsWith("svg:")) return;
    const QString path = payload.mid(4);
    if (pathIsInside(path, defaultAppdataPath(QStringLiteral("Imported")))) {
      status(QStringLiteral("Imported 2D shapes cannot be edited."));
      return;
    }
    edit2DShape(path);
  });

  auto* shapes2dSearch = new QLineEdit(shapes2dWrap);
  shapes2dSearch->setObjectName(QStringLiteral("shapes2dSearch"));
  shapes2dSearch->setPlaceholderText(QStringLiteral("Search 2D shapes or tags…"));
  shapes2dSearch->setClearButtonEnabled(true);
  shapes2dLayout->insertWidget(1, shapes2dSearch);
  connect(shapes2dSearch, &QLineEdit::textChanged, this, [this](const QString& text) { filterLibraryList(shapes2d, text); });
  shapes2d->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(shapes2d, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    QListWidgetItem* item = shapes2d->itemAt(shapes2d->viewport()->mapFrom(shapes2d, pos));
    if (!item) return;
    const QString payload = item->data(Qt::UserRole).toString();
    if (!payload.startsWith(QStringLiteral("svg:"))) return;
    QMenu menu(shapes2d);
    QAction* edit = menu.addAction(QStringLiteral("Edit"));
    const bool importedShape = pathIsInside(payload.mid(4), defaultAppdataPath(QStringLiteral("Imported")));
    edit->setEnabled(!importedShape);
    QAction* place = menu.addAction(QStringLiteral("Place on workplane"));
    QAction* editTags = menu.addAction(QStringLiteral("Edit tags…"));
    QAction* chosen = menu.exec(shapes2d->viewport()->mapToGlobal(shapes2d->viewport()->mapFrom(shapes2d, pos)));
    if (chosen == editTags) {
      if (editLibraryTags(this, twoDShapesDirectories + QStringList{defaultAppdataPath(QStringLiteral("Imported"))}, payload.mid(4)))
        refresh2DShapeLibrary();
    } else if (chosen == edit) edit2DShape(payload.mid(4));
    else if (chosen == place) {
      const int n = static_cast<int>(doc.nodes.size());
      importSvgShape(payload.mid(4), ((n % 4) - 1.5) * 45.0, (n / 4 % 3) * 45.0);
    }
  });

  sculptsList = new ShapeList(this);
  configureList(sculptsList);
  sculptsList->setIconSize(QSize(56, 56));
  sculptsList->setItemDelegate(new LibraryItemDelegate(sculptsList));
  auto* sculptsWrap = new QWidget(this);
  auto* sculptsLayout = new QVBoxLayout(sculptsWrap);
  auto* newSculptButton = new QPushButton(QStringLiteral("New sculpt…"), sculptsWrap);
  sculptsLayout->addWidget(newSculptButton);
  sculptsLayout->addWidget(sculptsList, 1);
  connect(newSculptButton, &QPushButton::clicked, this, [this] { newSculpt(); });
  pendingSculptTimer = new QTimer(this);
  pendingSculptTimer->setSingleShot(true);
  connect(pendingSculptTimer, &QTimer::timeout, this, [this] {
    const QString path = std::exchange(pendingSculptPlacement, QString());
    if (path.isEmpty()) return;
    const int n = static_cast<int>(doc.nodes.size());
    placeLibrarySculpt(path, ((n % 4) - 1.5) * 45.0, (n / 4 % 3) * 45.0);
  });
  connect(sculptsList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
    if (!item) return;
    if (sculptsList->suppressClick) {
      sculptsList->suppressClick = false;
      return;
    }
    if (sculptsList->consumedDrag) { sculptsList->consumedDrag = false; return; }
    const QString payload = item->data(Qt::UserRole).toString();
    if (!payload.startsWith(QStringLiteral("sculpt:"))) return;
    // Wait out the double-click time: a double-click opens it for sculpting instead.
    pendingSculptPlacement = payload.mid(7);
    pendingSculptTimer->start(QApplication::doubleClickInterval());
  });
  connect(sculptsList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
    pendingSculptTimer->stop();
    pendingSculptPlacement.clear();
    if (!item) return;
    const QString payload = item->data(Qt::UserRole).toString();
    if (payload.startsWith(QStringLiteral("sculpt:"))) editLibrarySculpt(payload.mid(7));
  });
  sculptsList->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(sculptsList, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    QListWidgetItem* item = sculptsList->itemAt(sculptsList->viewport()->mapFrom(sculptsList, pos));
    if (!item) return;
    const QString payload = item->data(Qt::UserRole).toString();
    if (!payload.startsWith(QStringLiteral("sculpt:"))) return;
    const QString path = payload.mid(7);
    const QFileInfo info(path);
    const QString picture = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".png");
    QMenu menu(sculptsList);
    QAction* edit = menu.addAction(QStringLiteral("Sculpt it"));
    QAction* place = menu.addAction(QStringLiteral("Place on workplane"));
    menu.addSeparator();
    QAction* rename = menu.addAction(QStringLiteral("Rename…"));
    QAction* remove = menu.addAction(QStringLiteral("Delete"));
    QAction* chosen = menu.exec(sculptsList->viewport()->mapToGlobal(sculptsList->viewport()->mapFrom(sculptsList, pos)));
    if (chosen == edit) {
      editLibrarySculpt(path);
    } else if (chosen == place) {
      const int n = static_cast<int>(doc.nodes.size());
      placeLibrarySculpt(path, ((n % 4) - 1.5) * 45.0, (n / 4 % 3) * 45.0);
    } else if (chosen == rename) {
      bool ok = false;
      QString name = QInputDialog::getText(this, QStringLiteral("Rename sculpt"), QStringLiteral("Name"), QLineEdit::Normal,
                                           info.completeBaseName(), &ok)
                         .trimmed();
      name.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
      if (!ok || name.isEmpty() || name == info.completeBaseName()) return;
      const QString to = info.absolutePath() + QLatin1Char('/') + name + QStringLiteral(".stl");
      if (QFileInfo::exists(to)) {
        QMessageBox::warning(this, QStringLiteral("Rename sculpt"), QStringLiteral("There is already a sculpt called “%1”.").arg(name));
        return;
      }
      if (!QFile::rename(path, to)) {
        QMessageBox::warning(this, QStringLiteral("Rename sculpt"), QStringLiteral("Could not rename it."));
        return;
      }
      if (QFileInfo::exists(picture)) QFile::rename(picture, info.absolutePath() + QLatin1Char('/') + name + QStringLiteral(".png"));
      refreshSculptLibrary();
    } else if (chosen == remove) {
      if (QMessageBox::question(this, QStringLiteral("Delete sculpt"),
                                QStringLiteral("Delete “%1” from the Sculpts library? Copies already placed in projects stay.")
                                    .arg(info.completeBaseName())) != QMessageBox::Yes)
        return;
      QFile::remove(path);
      QFile::remove(picture);
      refreshSculptLibrary();
    }
  });

  generators = new ShapeList(this);
  configureList(generators);
  generators->setItemDelegate(new LibraryItemDelegate(generators));
  for (const QString& category : gen::categories()) {
    auto* header = new QListWidgetItem(QStringLiteral("▾ ") + category, generators);
    header->setData(Qt::UserRole, QStringLiteral("header"));
    header->setData(Qt::UserRole + 1, true);
    header->setData(Qt::UserRole + 2, category);
    header->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    for (const gen::Spec& spec : gen::specs()) {
      if (spec.category != category) continue;
      auto* item = new QListWidgetItem(shapeIcon(QStringLiteral("gen:") + spec.id), QStringLiteral("    ") + spec.name, generators);
      item->setData(Qt::UserRole, QStringLiteral("gen:") + spec.id);
      item->setToolTip(spec.blurb + QStringLiteral("\n\nDrag onto the bed, or click to place it."));
    }
  }
  connect(generators, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
    if (!item) return;
    if (item->data(Qt::UserRole).toString() == QLatin1String("header")) {
      toggleLibraryCategory(generators, item);
      return;
    }
    if (generators->consumedDrag) { generators->consumedDrag = false; return; }
    const QString payload = item->data(Qt::UserRole).toString();
    if (!payload.startsWith(QStringLiteral("gen:"))) return;
    const int n = static_cast<int>(doc.nodes.size());
    placeGenerator(payload.mid(4), ((n % 4) - 1.5) * 45.0, (n / 4 % 3) * 45.0);
  });

  auto* tabs = new QTabWidget(this);
  tabs->addTab(shapes, "Shapes");
  tabs->addTab(partsWrap, "Parts");
  tabs->addTab(shapes2dWrap, "2D Shapes");
  tabs->addTab(sculptsWrap, "Sculpts");
  tabs->addTab(generators, "Generators");
  auto* rightWrap = new QWidget(this);
  auto* rightLayout = new QVBoxLayout(rightWrap);
  rightLayout->setContentsMargins(0, 0, 0, 0);
  auto* libraryHint = new QLabel(QStringLiteral("Drag a shape, STL, 2D Shape, sculpt or generator onto the bed."), rightWrap);
  libraryHint->setWordWrap(true);
  libraryHint->setStyleSheet(QStringLiteral("padding: 10px 12px 8px 12px;"));
  rightLayout->addWidget(libraryHint);
  rightLayout->addWidget(tabs, 1);
  auto* right = new QDockWidget("Library", this);
  right->setObjectName("libraryDock");
  right->setWidget(rightWrap);
  addDockWidget(Qt::RightDockWidgetArea, right);
  // Wide enough that all four tabs show at once, without the scroll arrows. The stylesheet
  // (tab padding) is set by now, so polishing first gives the real tab widths.
  tabs->ensurePolished();
  tabs->tabBar()->ensurePolished();
  const int tabsWide = tabs->tabBar()->sizeHint().width() + 8;
  rightWrap->setMinimumWidth(tabsWide);
  QTimer::singleShot(0, this, [this, right, tabsWide] { resizeDocks({right}, {std::max(tabsWide, 340)}, Qt::Horizontal); });

  auto* tools = addToolBar("Tools");
  tools->setToolButtonStyle(Qt::ToolButtonIconOnly);
  tools->setIconSize(QSize(32, 32));
  auto dress = [](QAction* action) {
    action->setIcon(toolIcon(action->text()));
    action->setToolTip(toolTipFor(action->text()));
  };
  auto addTool = [&](const QString& label, int mode) {
    auto* button = tools->addAction(label);
    button->setCheckable(true);
    button->setChecked(mode == 0);
    connect(button, &QAction::triggered, this, [this, mode] {
      if (alignPopup) alignPopup->hide();
      if (repeatPopup) repeatPopup->hide();
      if (filletPopup) filletPopup->hide();
      if (faceFilletPopup) faceFilletPopup->hide();
      if (edgeBetaPopup) edgeBetaPopup->hide();
      tool = mode;
      panel = 0;
      view->setTool(mode);
      syncActions();
      loadInspector();
      status(mode == 0 ? "Drag the body on X/Y. White corners scale all sides. Black squares scale one side."
                       : "Drag a ring to rotate: near the ring it snaps, further out it turns freely. Or type the degrees below.");
    });
    dress(button);
  };
  auto addPanel = [&](const QString& label, int which) {
    auto* button = tools->addAction(label);
    button->setCheckable(true);
    connect(button, &QAction::triggered, this, [this, which] { showPanel(which); });
    dress(button);
  };
  addTool("Move", 0);
  addTool("Rotate", 1);
  addPanel("Mirror", 4);
  addPanel("Hollow", 7);
  auto* latticeAction = tools->addAction(QStringLiteral("Lattice"));
  dress(latticeAction);
  connect(latticeAction, &QAction::triggered, this, [this] { openLattice(); });
  auto* textureAction = tools->addAction(QStringLiteral("Texture"));
  dress(textureAction);
  connect(textureAction, &QAction::triggered, this, [this] { openTexture(); });
  auto* wrapAction = tools->addAction(QStringLiteral("Wrap"));
  dress(wrapAction);
  connect(wrapAction, &QAction::triggered, this, [this] { openWrap(); });
  auto* articulateAction = tools->addAction(QStringLiteral("Articulate"));
  dress(articulateAction);
  connect(articulateAction, &QAction::triggered, this, [this] { openArticulate(); });
  auto* moldAction = tools->addAction(QStringLiteral("Mold"));
  dress(moldAction);
  connect(moldAction, &QAction::triggered, this, [this] { openMold(); });
  auto* simplifyAction = tools->addAction(QStringLiteral("Simplify"));
  dress(simplifyAction);
  connect(simplifyAction, &QAction::triggered, this, [this] { openSimplify(); });
  auto* restyleAction = tools->addAction(QStringLiteral("Restyle"));
  dress(restyleAction);
  connect(restyleAction, &QAction::triggered, this, [this] { openRestyle(); });
  addPanel("Split", 8);
  auto* align = tools->addAction("Align");
  align->setCheckable(true);
  dress(align);
  connect(align, &QAction::triggered, this, [this] { showAlignTool(); });
  auto* measure = tools->addAction("Measure");
  measure->setCheckable(true);
  dress(measure);
  connect(measure, &QAction::triggered, this, [this] { showMeasureTool(); });
  tools->addSeparator();
  addPanel("Bend", 2);
  addPanel("Bend face", 3);
  auto* fillet = tools->addAction("Fillet");
  fillet->setCheckable(true);
  dress(fillet);
  connect(fillet, &QAction::triggered, this, [this] { showFilletTool(); });
  auto* surfaceFillet = tools->addAction(QStringLiteral("Fillet surface"));
  surfaceFillet->setCheckable(true);
  dress(surfaceFillet);
  connect(surfaceFillet, &QAction::triggered, this, [this] { showSurfaceFilletTool(); });
  auto* edgeBeta = tools->addAction(QStringLiteral("Detected Edge Filleting"));
  edgeBeta->setCheckable(true);
  dress(edgeBeta);
  connect(edgeBeta, &QAction::triggered, this, [this] { showEdgeBetaTool(); });
  auto* extrudeFace = tools->addAction("Extrude face");
  extrudeFace->setCheckable(true);
  dress(extrudeFace);
  connect(extrudeFace, &QAction::triggered, this, [this] {
    if (panel == 6) {
      panel = 0;
      tool = 0;
      view->setTool(0);
      syncActions();
      loadInspector();
      status("Extrude face closed.");
      return;
    }
    if (alignPopup) alignPopup->hide();
    if (repeatPopup) repeatPopup->hide();
    if (filletPopup) filletPopup->hide();
    if (faceFilletPopup) faceFilletPopup->hide();
  if (edgeBetaPopup) edgeBetaPopup->hide();
    if (edgeBetaPopup) edgeBetaPopup->hide();
    tool = 6;
    panel = 6;
    view->setTool(6);
    syncActions();
    loadInspector();
    status("Click a face to extrude it. Positive depth grows outward; negative depth grows inward.");
  });
  connect(view, &Viewport::measurePicked, this, [this](QVector3D point) {
    if (!measureHaveA || measureHaveB) {
      measureA = point;
      measureHaveA = true;
      measureHaveB = false;
      status(QStringLiteral("Point A set. Click the second point. Esc clears."));
    } else {
      measureB = point;
      measureHaveB = true;
      const QVector3D delta = measureB - measureA;
      status(QStringLiteral("A to B  %1 mm    ΔX %2  ΔY %3  ΔZ %4")
                 .arg(static_cast<double>(delta.length()), 0, 'f', 2)
                 .arg(delta.x(), 0, 'f', 2)
                 .arg(delta.y(), 0, 'f', 2)
                 .arg(delta.z(), 0, 'f', 2));
    }
    updateMeasureOverlay();
  });
  connect(view, &Viewport::measureCleared, this, [this] {
    if (tool != 8 || (!measureHaveA && !measureHaveB)) return;
    measureHaveA = false;
    measureHaveB = false;
    updateMeasureOverlay();
    status(QStringLiteral("Measure cleared."));
  });
  connect(view, &Viewport::sliceFacePicked, this, [this](QVector3D point, QVector3D normal) {
    if (!slicePicking) return;
    sliceHaveFace = true;
    slicePoint = point;
    sliceNormal = normal;
    if (!sliceFaceLabel) return;
    if (std::abs(normal.z()) > 0.999f)
      sliceFaceLabel->setText(QStringLiteral("Face at Z %1 mm. The drawing keeps the bed's X and Y.").arg(point.z(), 0, 'f', 2));
    else
      sliceFaceLabel->setText(QStringLiteral("Tilted face. The drawing is flattened onto that face, as seen from outside."));
  });
  connect(view, &Viewport::faceChosen, this, [this](const QString& id, const QVector<QVector3D>& loop, QVector3D normal,
                                                     QVector3D axis) {
    if (loop.size() < 3) return;
    if (tool == 6) { extrudePickedFace(id, loop, normal, axis); return; }
    QVector<QVector3D> outline;
    bool innerBoundary = false;
    for (const QVector3D& point : loop) {
      if (std::abs(point.x()) > 1e19f) {
        innerBoundary = true;
        break;
      }
      outline.push_back(point);
    }
    if (outline.size() < 3) return;
    const Node* source = doc.find(id);
    if (!source) return;
    std::vector<const Node*> chain;
    const Node* cursor = source;
    while (cursor) {
      chain.push_back(cursor);
      if (cursor->parentId.isEmpty()) break;
      const Node* next = doc.find(cursor->parentId);
      if (!next) break;
      cursor = next;
    }
    QMatrix4x4 world;
    const double zero[3] = {0, 0, 0};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
      world = world * placementMatrix((*it)->pos, (*it)->rot, zero, (*it)->scale);
    QVector3D n = normal;
    if (n.lengthSquared() < 1e-8f) n = QVector3D(0, 0, 1);
    else n.normalize();
    // The bend starts right on the face, with no gap: grouped (or exported) with the part it
    // joins it, the shared face welding away. (It used to start 0.05 mm off the face.)
    QVector3D centroid;
    for (const QVector3D& point : outline) centroid += point;
    centroid /= static_cast<float>(outline.size());
    const QVector3D centroidRaw = centroid;
    double ignoredPos[3];
    double rot[3];
    double scale[3];
    decomposePlacement(world, ignoredPos, rot, scale);
    const QVector3D origin = world.map(centroid);
    pushUndo();
    Node elbow;
    elbow.id = newId();
    elbow.kind = Node::Kind::Shape;
    elbow.shape = QStringLiteral("elbow");
    elbow.name = QString("Bend %1").arg(doc.nodes.size() + 1);
    elbow.faceBend = true;
    elbow.color = QColor("#6f9eae");
    elbow.pos[0] = origin.x();
    elbow.pos[1] = origin.y();
    elbow.pos[2] = origin.z();
    // The bend is made at the part's real size: a part stretched with its scale (a box made
    // longer with the handles, say) has its face scaled up here, and the bend itself is not
    // scaled. (Taking the part's scale along made the bend on the unstretched face and then
    // stretched it, so a long face came out with a short end.)
    for (int i = 0; i < 3; ++i) {
      elbow.rot[i] = rot[i];
      elbow.scale[i] = 1;
    }
    const QVector3D stretch{static_cast<float>(scale[0]), static_cast<float>(scale[1]), static_cast<float>(scale[2])};
    auto safe = [](float v) { return std::abs(v) > 1e-6f ? v : 1e-6f; };
    n = QVector3D(n.x() / safe(stretch.x()), n.y() / safe(stretch.y()), n.z() / safe(stretch.z())).normalized();
    axis = QVector3D(axis.x() * stretch.x(), axis.y() * stretch.y(), axis.z() * stretch.z());
    if (axis.lengthSquared() > 1e-12f) axis.normalize();
    elbow.faceAngle = faceAngleSpin->value();
    elbow.faceRadius = faceRadiusSpin->value();
    elbow.faceSteps = faceStepsSpin->value();
    elbow.faceRise = faceRiseSpin->value();
    elbow.faceSpiral = faceSpiralBox && faceSpiralBox->isChecked();
    elbow.faceRadialStep = faceRadialSpin ? faceRadialSpin->value() : 0.0;
    elbow.faceAxisMode = std::clamp(faceAxisBox->currentIndex(), 0, 3);
    elbow.faceNormal[0] = n.x();
    elbow.faceNormal[1] = n.y();
    elbow.faceNormal[2] = n.z();
    elbow.faceAxis[0] = axis.x();
    elbow.faceAxis[1] = axis.y();
    elbow.faceAxis[2] = axis.z();
    elbow.faceLoop.reserve(static_cast<size_t>(loop.size()) * 3);
    bool gap = false;
    for (const QVector3D& point : loop) {
      if (std::abs(point.x()) > 1e19f || std::abs(point.y()) > 1e19f || std::abs(point.z()) > 1e19f) {
        gap = true;
        continue;
      }
      if (gap && !elbow.faceLoop.empty()) {
        elbow.faceLoop.push_back(1e20);
        elbow.faceLoop.push_back(1e20);
        elbow.faceLoop.push_back(1e20);
        gap = false;
      }
      const QVector3D local = (point - centroidRaw) * stretch;
      elbow.faceLoop.push_back(local.x());
      elbow.faceLoop.push_back(local.y());
      elbow.faceLoop.push_back(local.z());
    }
    doc.nodes.push_back(std::move(elbow));
    selection = QStringList{doc.nodes.back().id};
    refreshTree();
    loadInspector();
    refreshScene();
    status(innerBoundary
               ? "New bend. The inner boundary is cut as a hole and bent with the solid."
               : "New bend. It is its own object. Change turn, radius, steps, and Z per step, then drag it away.");
  });
  connect(view, &Viewport::smoothFaceChosen, this,
          [this](const QString& id, const QVector<QVector3D>& loop, const QVector<QVector3D>& normals, QVector3D hit) {
            if (tool == 7) filletPickedSurface(id, loop, normals, hit);
          });
  tools->addSeparator();
  tools->addAction("Hole", this, [this] {
    if (selection.isEmpty()) return;
    pushUndo();
    doc.toggleHole(selection);
    refreshTree();
    refreshScene();
    status("Marked as a hole. Group it with a solid and the hole is cut out.");
  });
  tools->addAction("Group", this, [this] {
    QString error;
    // A merged part grouped with holes: the holes go inside it, and it stays one merged part.
    {
      QString mergedId;
      bool onlyHoles = selection.size() >= 2;
      for (const QString& id : selection) {
        const Node* node = doc.find(id);
        if (!node) continue;
        if (node->kind == Node::Kind::Group && node->merged && !node->hole && mergedId.isEmpty()) mergedId = id;
        else if (!node->hole) onlyHoles = false;
      }
      if (onlyHoles && !mergedId.isEmpty()) {
        QStringList holes = selection;
        holes.removeAll(mergedId);
        pushUndo();
        if (!doc.addHolesToMerged(mergedId, holes, &error)) {
          undoStack.pop_back();
          status(error);
          return;
        }
        selection = QStringList{mergedId};
        refreshTree();
        loadInspector();
        refreshScene();
        status(QStringLiteral("The hole%1 went into “%2”, which stays one merged part. Unmerge shows %3 among its parts.")
                   .arg(holes.size() == 1 ? QString() : QStringLiteral("s"), doc.find(mergedId)->name,
                        holes.size() == 1 ? QStringLiteral("it") : QStringLiteral("them")));
        return;
      }
    }
    pushUndo();
    if (!doc.groupSelected(selection, &error)) {
      undoStack.pop_back();
      status(error);
      return;
    }
    selection = QStringList{doc.nodes.back().id};
    refreshTree();
    loadInspector();
    refreshScene();
    status("Grouped. Holes in this group are already cut out. Drag the group as one object.");
  });
  tools->addAction("Hull", this, [this] {
    QString error;
    pushUndo();
    if (!doc.hullSelected(selection, false, &error)) {
      undoStack.pop_back();
      status(error);
      return;
    }
    selection = QStringList{doc.nodes.back().id};
    refreshTree();
    loadInspector();
    refreshScene();
    status("Wrapped in a hull. Move the parts inside to reshape it; Ungroup gives them back. "
           "A chain hull (each part to the next) is in the panel.");
  });
  tools->addAction("Intersect", this, [this] {
    QString error;
    pushUndo();
    if (selection.size() < 2 || !doc.groupSelected(selection, &error)) {
      undoStack.pop_back();
      status(error.isEmpty() ? QStringLiteral("Select two or more parts that overlap.") : error);
      return;
    }
    Node& group = doc.nodes.back();
    group.intersect = true;
    group.name = QStringLiteral("Intersection");
    selection = QStringList{group.id};
    refreshTree();
    loadInspector();
    refreshScene();
    status(QStringLiteral("Kept only what the parts have in common. Move them inside to change it; Ungroup gives them back."));
  });
  tools->addAction("Ungroup", this, [this] {
    if (selection.size() != 1) {
      status("Select one group.");
      return;
    }
    QString error;
    pushUndo();
    const Node* picked = doc.find(selection.front());
    const QString outerId = picked ? picked->parentId : QString();
    if (!doc.ungroup(selection.front(), &error)) {
      undoStack.pop_back();
      status(error);
      return;
    }
    // A group inside another group: its parts land in the outer group, which still merges them, so
    // nothing looks different. Select the outer group and say so; Ungroup again takes them apart.
    const Node* outer = outerId.isEmpty() ? nullptr : doc.find(outerId);
    while (outer && outer->kind != Node::Kind::Group && !outer->parentId.isEmpty()) outer = doc.find(outer->parentId);
    if (outer && outer->kind == Node::Kind::Group) {
      selection = QStringList{outer->id};
      refreshTree();
      loadInspector();
      refreshScene();
      status(QStringLiteral("Ungrouped into “%1”, which still merges its parts. Press Ungroup again to take “%1” apart too.").arg(outer->name));
      return;
    }
    selection.clear();
    refreshTree();
    loadInspector();
    refreshScene();
    status("Ungrouped. A fillet on the group itself is dropped.");
  });
  auto* mergeAction = tools->addAction(QStringLiteral("Merge"));
  mergeAction->setCheckable(true);
  connect(mergeAction, &QAction::triggered, this, [this] {
    QString error;
    const Node* single = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    pushUndo();
    if (single && single->kind == Node::Kind::Group && single->merged) {
      const QString name = single->name;
      doc.unmerge(single->id, &error);
      refreshTree();
      loadInspector();
      refreshScene();
      syncActions();
      status(QStringLiteral("Unmerged “%1”: its parts are back, as they were. Merge joins them into one again.").arg(name));
      return;
    }
    if (!doc.merge(selection, &error)) {
      undoStack.pop_back();
      syncActions();
      status(error);
      return;
    }
    if (selection.size() > 1) selection = QStringList{doc.nodes.back().id};
    refreshTree();
    loadInspector();
    refreshScene();
    syncActions();
    status(QStringLiteral("Merged into one part: picked, filleted and measured as its finished shape. Holes grouped with it go "
                          "inside it. Merge again (or Unmerge) brings the parts back."));
  });
  auto* repeat = tools->addAction("Repeat");
  repeat->setCheckable(true);
  dress(repeat);
  connect(repeat, &QAction::triggered, this, [this] { showRepeatTool(); });
  tools->addSeparator();
  tools->addAction("Copy", this, [this] {
    if (selection.size() != 1) return;
    pushUndo();
    doc.duplicate(selection.front());
    selection = QStringList{doc.nodes.back().id};
    refreshTree();
    loadInspector();
    refreshScene();
  });
  tools->addAction("Delete", this, [this] {
    if (selection.isEmpty()) return;
    pushUndo();
    const QStringList doomed = selection;
    for (const QString& id : doomed) doc.remove(id);
    selection.clear();
    refreshTree();
    loadInspector();
    refreshScene();
  });
  tools->addAction("Drop", this, [this] {
    // Straight down to the plate, through anything in the way: a hole goes down into the part it
    // has to cut.
    if (selection.size() != 1) {
      status("Select one object to drop it onto the plate.");
      return;
    }
    Node* node = doc.find(selection.front());
    if (!node) {
      status("Select one object to drop it onto the plate.");
      return;
    }
    pushUndo();
    // Exact: the part's lowest point from its real (double precision) geometry, and the height
    // kept as it comes out instead of rounded to 0.0001 mm, which left it a hair off the plate.
    {
      Document shapeOnly = doc;
      if (Node* self = shapeOnly.find(node->id)) self->hole = false;
      std::vector<double> verts;
      std::vector<uint64_t> tris;
      if (worldMesh64(shapeOnly, QStringList{node->id}, &verts, &tris) && verts.size() >= 3) {
        double bottom = 1e300;
        for (size_t i = 2; i < verts.size(); i += 3) bottom = std::min(bottom, verts[i]);
        const double rise = doc.workplaneZ - bottom;
        if (node->parentId.isEmpty()) {
          node->pos[2] += rise;
        } else {
          bool ok = false;
          const QMatrix4x4 parentInverse = nodeWorld(doc, node->parentId).inverted(&ok);
          const QVector3D delta = ok ? parentInverse.mapVector(QVector3D(0, 0, static_cast<float>(rise)))
                                     : QVector3D(0, 0, static_cast<float>(rise));
          node->pos[0] += delta.x();
          node->pos[1] += delta.y();
          node->pos[2] += delta.z();
        }
        refreshScene();
        loadInspector();
        status(QStringLiteral("Dropped onto the plate."));
        return;
      }
    }
    double lo[3];
    double hi[3];
    if (localBounds(doc, *node, lo, hi)) {
      const double zero[3] = {0, 0, 0};
      const QMatrix4x4 placed = placementMatrix(node->pos, node->rot, zero, node->scale);
      double minZ = 1e9;
      for (int i = 0; i < 8; ++i) {
        const QVector3D corner((i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]);
        minZ = std::min(minZ, static_cast<double>(placed.map(corner).z()));
      }
      node->pos[2] += doc.workplaneZ - minZ;
    } else {
      const double bottom = node->pos[2] - halfHeight(*node) * std::abs(node->scale[2]);
      node->pos[2] += doc.workplaneZ - bottom;
    }
    tidyPlacement(*node);
    refreshScene();
    loadInspector();
    status(QStringLiteral("Dropped onto the plate."));
  });
  tools->addAction("Drop onto part", this, [this] {
    // Straight down until it touches the first part under it, touching exactly, so the two merge
    // when grouped. A part sunk into the one below comes up to rest on it.
    if (selection.size() != 1) {
      status("Select one object to drop it onto the part below it.");
      return;
    }
    Node* node = doc.find(selection.front());
    if (!node) {
      status("Select one object to drop it onto the part below it.");
      return;
    }
    QString landedOn;
    const double fall = dropDistanceBelow(*node, &landedOn);
    if (std::isnan(fall)) {
      status(QStringLiteral("There is no part under it. Use Drop to put it on the plate."));
      return;
    }
    pushUndo();
    if (node->parentId.isEmpty()) {
      node->pos[2] -= fall;
    } else {
      // Inside a group: straight down in the world is some direction in the group's frame.
      bool ok = false;
      const QMatrix4x4 parentInverse = nodeWorld(doc, node->parentId).inverted(&ok);
      const QVector3D delta = ok ? parentInverse.mapVector(QVector3D(0, 0, static_cast<float>(-fall)))
                                 : QVector3D(0, 0, static_cast<float>(-fall));
      node->pos[0] += delta.x();
      node->pos[1] += delta.y();
      node->pos[2] += delta.z();
    }
    // Not rounded to 0.0001 mm like other moves: the face it lands on may sit between those steps
    // (a resized part's top), and the rounding would leave it hovering again.
    if (node->parentId.isEmpty()) {
      const double exactZ = node->pos[2];
      tidyPlacement(*node);
      node->pos[2] = exactZ;
    }
    refreshScene();
    loadInspector();
    status(std::abs(fall) < 1e-7 ? QStringLiteral("Already resting on %1.").arg(landedOn)
           : fall > 0 ? QStringLiteral("Dropped %1 mm onto %2. It touches exactly, so grouping them merges them.")
                            .arg(fall, 0, 'f', 3)
                            .arg(landedOn)
                      : QStringLiteral("Lifted %1 mm to rest on %2. It touches exactly, so grouping them merges them.")
                            .arg(-fall, 0, 'f', 3)
                            .arg(landedOn));
  });
  auto* layFlatAction = tools->addAction("Lay flat");
  layFlatAction->setCheckable(true);
  connect(layFlatAction, &QAction::triggered, this, [this] { showFaceTool(12); });
  auto* workplaneAction = tools->addAction("Workplane");
  workplaneAction->setCheckable(true);
  connect(workplaneAction, &QAction::triggered, this, [this] { showFaceTool(13); });
  auto* sectionAction = tools->addAction("Section");
  sectionAction->setCheckable(true);
  connect(sectionAction, &QAction::toggled, this, [this](bool on) {
    if (on != sectionOn) setSection(on);
  });
  auto* overhangAction = tools->addAction("Overhangs");
  overhangAction->setCheckable(true);
  connect(overhangAction, &QAction::toggled, this, [this](bool on) {
    const double angle = QSettings().value(QStringLiteral("overhangAngle"), 45.0).toDouble();
    view->setOverhangs(on, angle);
    status(on ? QStringLiteral("Faces striped red hang over more than %1° from upright: they need support (or a chamfer). "
                               "The angle is under Settings > Printer.").arg(angle, 0, 'f', 0)
              : QStringLiteral("Overhangs hidden."));
  });
  auto* snap = new QAction("Snap", this);
  snap->setCheckable(true);
  snap->setChecked(doc.snapOn);
  snap->setIcon(toolIcon(QStringLiteral("Snap")));
  snap->setToolTip(QStringLiteral("Snap. The millimetre box beside this button is the snap distance."));
  connect(snap, &QAction::toggled, this, [this](bool on) {
    doc.snapOn = on;
    view->setSnap(on, doc.snap);
    status(on ? QString("Snap %1 mm.").arg(doc.snap) : "Snap off.");
  });
  auto* grid = new QAction("Grid", this);
  grid->setCheckable(true);
  grid->setChecked(doc.gridOn);
  grid->setIcon(toolIcon(QStringLiteral("Grid")));
  grid->setToolTip(QStringLiteral("Grid"));
  connect(grid, &QAction::toggled, this, [this](bool on) {
    doc.gridOn = on;
    view->setGrid(on);
    status(on ? "1 mm grid on. Major lines every 10 mm." : "Grid hidden. The plate border stays.");
  });
  auto* snapButton = new QToolButton(this);
  snapButton->setDefaultAction(snap);
  snapButton->setIconSize(QSize(16, 16));
  snapButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  snapButton->setAutoRaise(true);
  snapButton->setFocusPolicy(Qt::NoFocus);
  auto* gridButton = new QToolButton(this);
  gridButton->setDefaultAction(grid);
  gridButton->setIconSize(QSize(16, 16));
  gridButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  gridButton->setAutoRaise(true);
  gridButton->setFocusPolicy(Qt::NoFocus);
  statusBar()->addPermanentWidget(snapButton);
  snapBar = new QDoubleSpinBox(this);
  snapBar->setRange(0.1, 100);
  snapBar->setDecimals(2);
  snapBar->setSingleStep(0.5);
  snapBar->setSuffix(QStringLiteral(" mm"));
  snapBar->setValue(doc.snap);
  snapBar->setToolTip(QStringLiteral("Snap distance in millimetres."));
  snapBar->setFixedWidth(136);
  connect(snapBar, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
    doc.snap = value;
    if (view) view->setSnap(doc.snapOn, doc.snap);
    if (snapSpin && std::abs(snapSpin->value() - value) > 1e-6) {
      const bool was = loading;
      loading = true;
      snapSpin->setValue(value);
      loading = was;
    }
    status(doc.snapOn ? QString("Snap %1 mm.").arg(doc.snap) : "Snap off.");
  });
  statusBar()->addPermanentWidget(snapBar);
  statusBar()->addPermanentWidget(gridButton);
  auto* focus = new QAction(QStringLiteral("Focus"), this);
  focus->setCheckable(true);
  focus->setChecked(false);
  focus->setToolTip(QStringLiteral("Show the point the view orbits and zooms around. Click the bed or drag the marker to move it."));
  connect(focus, &QAction::toggled, this, [this](bool on) {
    if (view) view->setFocusMarker(on);
    status(on ? QStringLiteral("Focus point on. Click the bed or drag the marker. The view turns and zooms around it.")
              : QStringLiteral("Focus point hidden."));
  });
  usageLabel = new QLabel(this);
  usageLabel->setMinimumWidth(170);
  usageLabel->setStyleSheet(QStringLiteral("color: #8a8074; padding: 0 10px 0 4px;"));
  messageLabel = new QLabel(this);
  messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  statusBar()->addWidget(usageLabel);
  statusBar()->addWidget(messageLabel, 1);
  usageMeter = new UsageMeter();
  updateUsage();
  auto* usageTimer = new QTimer(this);
  connect(usageTimer, &QTimer::timeout, this, [this] { updateUsage(); });
  usageTimer->start(1000);
  auto* focusButton = new QToolButton(this);
  focusButton->setDefaultAction(focus);
  focusButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  focusButton->setAutoRaise(true);
  focusButton->setFocusPolicy(Qt::NoFocus);
  statusBar()->addPermanentWidget(focusButton);

  alignPopup = new QFrame(host);
  alignPopup->setObjectName("alignPopup");
  alignPopup->setFixedWidth(260);
  auto* alignLayout = new QVBoxLayout(alignPopup);
  alignLayout->setContentsMargins(10, 10, 10, 10);
  alignLayout->setSpacing(6);
  auto* alignTitle = new QLabel(QStringLiteral("Align"), alignPopup);
  alignTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #e87400; background: transparent;"));
  alignLayout->addWidget(alignTitle);
  auto* alignHint = new QLabel(QStringLiteral("Click a dot in the view to line the parts up. Click a part first to keep it in place (orange); the others move to it."), alignPopup);
  alignHint->setWordWrap(true);
  alignLayout->addWidget(alignHint);
  const QString axisLabels[3] = {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")};
  const QString modeLabels[3] = {QStringLiteral("Min"), QStringLiteral("Center"), QStringLiteral("Max")};
  for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
    auto* row = new QHBoxLayout();
    auto* label = new QLabel(axisLabels[axisIndex], alignPopup);
    label->setFixedWidth(18);
    row->addWidget(label);
    for (int modeIndex = 0; modeIndex < 3; ++modeIndex) {
      auto* button = new QPushButton(modeLabels[modeIndex], alignPopup);
      button->setMinimumWidth(68);
      row->addWidget(button);
      connect(button, &QPushButton::clicked, this, [this, axisIndex, modeIndex] { alignSelection(axisIndex, modeIndex); });
    }
    alignLayout->addLayout(row);
  }
  alignPopup->hide();

  // The toolbar in groups: placing and handling parts; shaping their surface; combining and
  // cutting; setting them on the plate and looking at them. (Anything not listed keeps its place
  // at the end of the group it was in.)
  {
    const QList<QStringList> groups = {
        {"Move", "Rotate", "Mirror", "Align", "Measure", "Copy", "Repeat", "Delete"},
        {"Bend", "Bend face", "Fillet", "Fillet surface", "Detected Edge Filleting", "Extrude face", "Texture", "Wrap"},
        {"Hole", "Group", "Ungroup", "Merge", "Hull", "Intersect", "Lattice", "Restyle", "Simplify", "Hollow", "Split", "Articulate"},
        {"Drop", "Drop onto part", "Lay flat", "Workplane", "Section", "Overhangs"}};
    QList<QAction*> existing = tools->actions();
    QHash<QString, QAction*> byName;
    for (QAction* action : existing) {
      tools->removeAction(action);
      if (action->isSeparator()) action->deleteLater();
      else byName.insert(action->text(), action);
    }
    for (int g = 0; g < groups.size(); ++g) {
      if (g > 0) tools->addSeparator();
      for (const QString& name : groups[g])
        if (QAction* action = byName.take(name)) tools->addAction(action);
    }
    for (QAction* action : existing)  // anything new and not placed yet
      if (!action->isSeparator() && byName.contains(action->text())) tools->addAction(byName.take(action->text()));
  }

  auto* toolbarSpacer = new QWidget(tools);
  toolbarSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  tools->addWidget(toolbarSpacer);
  auto* shape2dAction = tools->addAction("2D Shape");
  shape2dAction->setToolTip(QStringLiteral("Open the 2D shape editor"));
  shape2dAction->setIcon(toolIcon(QStringLiteral("2D Shape")));
  if (QWidget* shape2dButton = tools->widgetForAction(shape2dAction))
    shape2dButton->setObjectName(QStringLiteral("shape2dButton"));
  connect(shape2dAction, &QAction::triggered, this, [this] { create2DShape(); });
  auto* sculptAction = tools->addAction("Sculpt");
  sculptAction->setToolTip(QStringLiteral("Open the Sculpt window: push and pull a shape's surface with brushes. With a part "
                                          "selected it sculpts that part."));
  sculptAction->setIcon(toolIcon(QStringLiteral("Sculpt")));
  if (QWidget* sculptButton = tools->widgetForAction(sculptAction)) sculptButton->setObjectName(QStringLiteral("sculptButton"));
  connect(sculptAction, &QAction::triggered, this, [this] { openSculpt(); });
  for (QAction* action : tools->actions()) {
    if (action->isSeparator() || !action->icon().isNull()) continue;
    dress(action);
  }
  connect(view, &Viewport::edgePicked, this, [this](int key, Qt::KeyboardModifiers mods) {
    if (tool == 9) {
      // Edge fillet (beta): every click adds or removes one edge chain.
      const auto it = std::find(edgeBetaPending.begin(), edgeBetaPending.end(), key);
      const bool adding = it == edgeBetaPending.end();
      if (adding) edgeBetaPending.push_back(key);
      else edgeBetaPending.erase(it);
      syncEdgeBetaChrome();
      const bool inside = key >= 0 && key < static_cast<int>(edgeBetaConcave.size()) && edgeBetaConcave[static_cast<size_t>(key)];
      status(QStringLiteral("%1 %2 edge. %3 picked. Apply in the popup.")
                 .arg(adding ? QStringLiteral("Added an") : QStringLiteral("Removed an"),
                      inside ? QStringLiteral("inside (it gets filled)") : QStringLiteral("outside (it gets rounded off)"))
                 .arg(edgeBetaPending.size()));
      return;
    }
    if (mods.testFlag(Qt::ShiftModifier)) {
      if (pendingAll) {
        pendingEdges.clear();
        const Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
        if (node) {
          for (const PickEdge& edge : pickEdges(doc, *node)) {
            if (std::find(pendingEdges.begin(), pendingEdges.end(), edge.key) == pendingEdges.end())
              pendingEdges.push_back(edge.key);
          }
        }
        pendingAll = false;
      }
      const auto it = std::find(pendingEdges.begin(), pendingEdges.end(), key);
      if (it == pendingEdges.end()) pendingEdges.push_back(key);
      else pendingEdges.erase(it);
    } else {
      pendingAll = false;
      pendingEdges.assign(1, key);
    }
    syncFilletChrome();
    status(pendingEdges.empty() ? QStringLiteral("No edges selected.")
                                : QString("Edge %1. %2 selected. Shift adds or removes. Apply in the popup.")
                                      .arg(key + 1)
                                      .arg(pendingEdges.size()));
  });

  auto* file = menuBar()->addMenu("File");
  file->addAction("New", QKeySequence::New, this, [this] {
    if (!doc.nodes.empty() &&
        QMessageBox::question(this, "New", "Clear the bed?") != QMessageBox::Yes)
      return;
    pushUndo();
    doc.nodes.clear();
    selection.clear();
    projectDirectory.clear();
    refreshTree();
    loadInspector();
    refreshScene();
    markSaved();
    status("New project. The bed is empty.");
  });
  file->addAction("Open…", QKeySequence::Open, this, [this] { openDocument(); });
  recentMenu = file->addMenu(QStringLiteral("Recent"));
  refreshRecentMenu();
  file->addAction(QStringLiteral("Recent projects…"), this, [this] { offerRecentProjects(true); });
  file->addAction("Save…", QKeySequence::Save, this, [this] { saveDocument(); });
  auto addExportFormats = [this](QMenu* menu, bool selected) {
    menu->addAction(QStringLiteral("STL"), this, [this, selected] { exportMesh(selected, QStringLiteral("stl")); });
    menu->addAction(QStringLiteral("OBJ"), this, [this, selected] { exportMesh(selected, QStringLiteral("obj")); });
    menu->addAction(QStringLiteral("3MF"), this, [this, selected] { exportMesh(selected, QStringLiteral("3mf")); });
    menu->addAction(exact::available() ? QStringLiteral("STEP") : QStringLiteral("STEP (faceted)"), this,
                    [this, selected] { exportMesh(selected, QStringLiteral("step")); });
    menu->addAction(QStringLiteral("DXF"), this, [this, selected] { exportMesh(selected, QStringLiteral("dxf")); });
    menu->addAction(QStringLiteral("Project Files"), this, [this, selected] { exportProjectZip(selected); });
  };
  addExportFormats(file->addMenu(QStringLiteral("Export")), false);
  addExportFormats(file->addMenu(QStringLiteral("Export selected object")), true);
  file->addAction("Import project…", this, [this] { importProject(); });
  if (exact::available()) file->addAction(QStringLiteral("Import STEP…"), this, [this] { importStep(); });
  file->addAction("Open projects folder", this, [this] { openProjectsFolder(); });
  file->addAction("Export slice…", this, [this] { exportSlice(); });
  file->addAction("Technical drawing…", this, [this] { openDrawing(); });
  file->addSeparator();
  file->addAction("Check for updates…", this, [this] { openUpdatesDialog(this, updateHooks()); });
  file->addSeparator();
  file->addAction("Quit", QKeySequence::Quit, qApp, &QApplication::quit);
  auto* edit = menuBar()->addMenu("Edit");
  edit->addAction("Undo", QKeySequence::Undo, this, [this] { undo(); });
  edit->addAction("Redo", QKeySequence::Redo, this, [this] { redo(); });

  auto* settingsAction = menuBar()->addAction(QStringLiteral("Settings"));
  connect(settingsAction, &QAction::triggered, this, [this] { openSettings(); });

  auto* cloudAction = menuBar()->addAction(QStringLiteral("CloudSync"));
  connect(cloudAction, &QAction::triggered, this, [this] { openCloudSync(); });

  auto* helpAction = menuBar()->addAction(QStringLiteral("Help"));
  connect(helpAction, &QAction::triggered, this, [this] { showHelp(); });
  auto* aboutAction = menuBar()->addAction(QStringLiteral("About"));
  connect(aboutAction, &QAction::triggered, this, [this] {
    QDialog about(this);
    about.setWindowTitle(QStringLiteral("About DaedalCAD"));
    auto* layout = new QVBoxLayout(&about);
    layout->setContentsMargins(18, 18, 18, 18);
    auto* logo = new QLabel(&about);
    logo->setAlignment(Qt::AlignCenter);
    const QPixmap splash(QCoreApplication::applicationDirPath() + QStringLiteral("/appdata/splash.png"));
    if (!splash.isNull()) logo->setPixmap(splash.scaledToWidth(420, Qt::SmoothTransformation));
    layout->addWidget(logo);
    auto* title = new QLabel(QStringLiteral("DaedalCAD"), &about);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600; color: #e87400;"));
    layout->addWidget(title);
#ifndef DAEDALCAD_VERSION
#define DAEDALCAD_VERSION "0.0.0-dev"
#endif
    auto* version = new QLabel(QStringLiteral("Version %1").arg(QStringLiteral(DAEDALCAD_VERSION)), &about);
    version->setAlignment(Qt::AlignCenter);
    layout->addWidget(version);
    auto* aboutButtons = new QHBoxLayout();
    auto* updates = new QPushButton(QStringLiteral("Check for updates…"), &about);
    auto* close = new QPushButton(QStringLiteral("Close"), &about);
    aboutButtons->addWidget(updates);
    aboutButtons->addStretch(1);
    aboutButtons->addWidget(close);
    layout->addLayout(aboutButtons);
    connect(close, &QPushButton::clicked, &about, &QDialog::accept);
    connect(updates, &QPushButton::clicked, &about, [this, &about] {
      about.accept();
      openUpdatesDialog(this, updateHooks());
    });
    about.exec();
  });

  // Keyboard shortcuts: the toolbar and menus, plus these commands of their own. Every key can be
  // changed in Settings > Keyboard.
  buildCommands();
  // The projection is remembered from one run to the next.
  view->setOrthographic(QSettings().value(QStringLiteral("view/orthographic"), false).toBool());
  connect(view, &Viewport::projectionChanged, this, [this](bool on) {
    QSettings().setValue(QStringLiteral("view/orthographic"), on);
    status(on ? QStringLiteral("Orthographic view: no perspective, so sizes compare across the view.")
              : QStringLiteral("Perspective view."));
  });
  view->setTool(0);
  view->setRotationSnap(0);
  view->setSnap(true, doc.snap);
  view->setGrid(doc.gridOn);
  view->setGeometry(0, 0, std::max(host->width(), 640), std::max(host->height(), 480));
}

void MainWindow::refreshTree() {
  loading = true;
  tree->clear();
  std::function<void(const QString&, QTreeWidgetItem*)> walk = [&](const QString& parent, QTreeWidgetItem* parentItem) {
    for (const Node* node : doc.children(parent)) {
      auto* item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(tree);
      QString label = node->name;
      if (node->hole) label += QStringLiteral("  · hole");
      if (node->kind == Node::Kind::Group && node->merged) label += QStringLiteral("  · merged");
      item->setText(0, label);
      item->setData(0, Qt::UserRole, node->id);
      item->setSelected(selection.contains(node->id));
      // A merged part's own parts stay out of sight until it is unmerged.
      if (!(node->kind == Node::Kind::Group && node->merged)) walk(node->id, item);
    }
  };
  walk(QString(), nullptr);
  tree->expandAll();
  loading = false;
}

bool drawMeshCenter(const CpuMesh& mesh, double center[3]) {
  if (mesh.vertices.size() < 3) return false;
  double lo[3] = {1e300, 1e300, 1e300};
  double hi[3] = {-1e300, -1e300, -1e300};
  for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3) {
    for (int axis = 0; axis < 3; ++axis) {
      const double value = static_cast<double>(mesh.vertices[i + static_cast<size_t>(axis)]);
      lo[axis] = std::min(lo[axis], value);
      hi[axis] = std::max(hi[axis], value);
    }
  }
  if (!(lo[0] <= hi[0])) return false;
  for (int axis = 0; axis < 3; ++axis) center[axis] = (lo[axis] + hi[axis]) * 0.5;
  return true;
}

// Old groups stored the gizmo at the group origin while the mesh sat elsewhere.
// Shift the origin onto the mesh centre and pull the children back by the same
// amount, so the solid does not move.
bool recenterSelectedGroups(Document& doc, const std::vector<DrawItem>& items) {
  bool changed = false;
  const double zero[3] = {0, 0, 0};
  for (const DrawItem& item : items) {
    if (!item.selected || item.copyIndex != 0 || meshOf(item.mesh).triangles <= 0) continue;
    Node* node = doc.find(item.id);
    if (!node || node->kind != Node::Kind::Group) continue;
    double center[3];
    if (!drawMeshCenter(meshOf(item.mesh), center)) continue;
    const double offset = center[0] * center[0] + center[1] * center[1] + center[2] * center[2];
    if (offset < 0.05 * 0.05) continue;
    for (double& value : center) value = tidyLength(value);
    const QVector3D shifted = placementMatrix(node->pos, node->rot, zero, node->scale)
                                  .map(QVector3D(center[0], center[1], center[2]));
    node->pos[0] = tidyLength(shifted.x());
    node->pos[1] = tidyLength(shifted.y());
    node->pos[2] = tidyLength(shifted.z());
    for (Node& child : doc.nodes) {
      if (child.parentId != node->id) continue;
      child.pos[0] -= center[0];
      child.pos[1] -= center[1];
      child.pos[2] -= center[2];
    }
    changed = true;
  }
  return changed;
}

void bakeSvgPivots(Document& doc) {
  const double origin[3] = {0, 0, 0};
  for (Node& node : doc.nodes) {
    if (node.pivotBaked || node.shape != QLatin1String("svg") || node.meshPath.isEmpty()) continue;
    double x = 0;
    double y = 0;
    if (!svgPivotOffset(node.meshPath, &x, &y)) {
      node.pivotBaked = true;
      continue;
    }
    const QVector3D delta = placementMatrix(origin, node.rot, origin, node.scale).map(QVector3D(x, y, 0));
    node.pos[0] += delta.x();
    node.pos[1] += delta.y();
    node.pos[2] += delta.z();
    tidyPlacement(node);
    node.pivotBaked = true;
  }
}

void MainWindow::refreshScene(bool quick) {
  if (rebuilding) return;
  bakeSvgPivots(doc);
  view->setSnap(doc.snapOn, doc.snap);
  view->setGrid(doc.gridOn);
  view->setBed(doc.bedX, doc.bedY);
  static bool centering = false;
  auto publish = [&](std::vector<DrawItem> items, const QString& warning) {
    if (!quick && !gesture && !centering && recenterSelectedGroups(doc, items)) {
      centering = true;
      refreshScene(false);
      centering = false;
      if (!loading) loadInspector();
      status(QStringLiteral("Gizmo moved to the center of the group."));
      return;
    }
    view->setItems(std::move(items), doc.workplaneZ);
    if (!warning.isEmpty()) status(warning);
  };
  if (quick || !sceneIsHeavy()) {
    QString warning;
    bool deferred = false;
    publish(buildEditView(doc, selection, &warning, quick, &deferred), warning);
    if (deferred) {
      // The cut comes once the dragging or typing pauses.
      if (!settleTimer) {
        settleTimer = new QTimer(this);
        settleTimer->setSingleShot(true);
        settleTimer->setInterval(450);
        connect(settleTimer, &QTimer::timeout, this, [this] {
          if (view && QApplication::mouseButtons() != Qt::NoButton) {
            settleTimer->start();  // still holding the part: wait for the button to come up
            return;
          }
          refreshScene(false);
        });
      }
      settleTimer->start();
      status(QStringLiteral("Shown uncut while you move it; the cut follows when you pause."));
    }
    if (inspectorWaitsForScene && !loading) {
      inspectorWaitsForScene = false;
      loadInspector();
    }
    return;
  }
  Document snapshot = doc;
  const QStringList selected = selection;
  std::vector<DrawItem> items;
  QString warning;
  runBusy([&] { items = buildEditView(snapshot, selected, &warning); }, false);
  publish(std::move(items), warning);
  if (inspectorWaitsForScene && !loading) {
    inspectorWaitsForScene = false;
    loadInspector();
  }
}

void MainWindow::duplicateSelectionInPlace() {
  if (selection.size() != 1) { status("Select one object to duplicate."); return; }
  const QString id = selection.front();
  const int before = static_cast<int>(doc.nodes.size());
  pushUndo();
  doc.duplicateInPlace(id);
  if (static_cast<int>(doc.nodes.size()) <= before) { undoStack.pop_back(); return; }
  selection = QStringList{doc.nodes[static_cast<size_t>(before)].id};
  refreshTree();
  loadInspector();
  refreshScene();
  status("Duplicated in place.");
}

void MainWindow::nudgeSelection(int dx, int dy) {
  if (selection.isEmpty() || (!dx && !dy)) return;
  const double step = doc.snap > 0 ? doc.snap : 1.0;
  std::vector<QString> roots;
  for (const QString& id : selection) {
    const Node* node = doc.find(id);
    if (!node) continue;
    bool parentSelected = false;
    QString parent = node->parentId;
    while (!parent.isEmpty()) {
      if (selection.contains(parent)) { parentSelected = true; break; }
      const Node* p = doc.find(parent);
      if (!p) break;
      parent = p->parentId;
    }
    if (!parentSelected) roots.push_back(id);
  }
  if (roots.empty()) return;
  pushUndo();
  for (const QString& id : roots) {
    if (Node* node = doc.find(id)) {
      node->pos[0] += dx * step;
      node->pos[1] += dy * step;
    }
  }
  refreshScene(true);
  loadInspector();
  status(QString("Nudge %1 × %2 mm.").arg(dx * step, 0, 'f', 3).arg(dy * step, 0, 'f', 3));
}

QString MainWindow::keyFor(const Command& command) const {
  const QString key = QStringLiteral("keys/") + command.id;
  QSettings settings;
  return settings.contains(key) ? settings.value(key).toString() : command.fallback;
}

void MainWindow::buildCommands() {
  commands.clear();
  auto idOf = [](const QString& prefix, QString text) {
    text.remove(QChar(0x2026));
    return prefix + text.trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('-'));
  };
  // The toolbar, group by group as it is laid out.
  const QHash<QString, QString> toolKeys = {
      {QStringLiteral("Move"), QStringLiteral("1")},         {QStringLiteral("Rotate"), QStringLiteral("2")},
      {QStringLiteral("Mirror"), QStringLiteral("3")},       {QStringLiteral("Align"), QStringLiteral("4")},
      {QStringLiteral("Bend"), QStringLiteral("5")},         {QStringLiteral("Bend face"), QStringLiteral("6")},
      {QStringLiteral("Fillet"), QStringLiteral("7")},       {QStringLiteral("Extrude face"), QStringLiteral("8")},
      {QStringLiteral("Hole"), QStringLiteral("H")},         {QStringLiteral("Delete"), QStringLiteral("Del")},
      {QStringLiteral("Group"), QStringLiteral("Ctrl+G")},   {QStringLiteral("Ungroup"), QStringLiteral("Ctrl+U")}};
  const QStringList toolGroups = {QStringLiteral("Placing and handling parts"), QStringLiteral("Shaping"),
                                  QStringLiteral("Combining and cutting"), QStringLiteral("On the plate and looking")};
  QToolBar* tools = nullptr;
  for (QToolBar* bar : findChildren<QToolBar*>())
    if (bar->windowTitle() == QLatin1String("Tools")) tools = bar;
  if (tools) {
    int group = 0;
    for (QAction* action : tools->actions()) {
      if (action->isSeparator()) {
        ++group;
        continue;
      }
      if (action->text().isEmpty() || qobject_cast<QWidgetAction*>(action)) continue;
      Command command;
      command.id = idOf(QStringLiteral("tool."), action->text());
      command.name = action->text();
      command.group = toolGroups.value(group, QStringLiteral("More tools"));
      command.fallback = toolKeys.value(action->text());
      command.action = action;
      commands.push_back(command);
    }
  }
  // The menus.
  const QHash<QString, QString> menuKeys = {
      {QStringLiteral("New"), QStringLiteral("Ctrl+N")},  {QStringLiteral("Open…"), QStringLiteral("Ctrl+O")},
      {QStringLiteral("Save…"), QStringLiteral("Ctrl+S")}, {QStringLiteral("Quit"), QStringLiteral("Ctrl+Q")},
      {QStringLiteral("Undo"), QStringLiteral("Ctrl+Z")}, {QStringLiteral("Redo"), QStringLiteral("Ctrl+Y")},
      {QStringLiteral("Help"), QStringLiteral("F1")}};
  std::function<void(QMenu*, const QString&)> walkMenu = [&](QMenu* menu, const QString& path) {
    for (QAction* action : menu->actions()) {
      if (action->isSeparator() || action->text().isEmpty()) continue;
      if (action->menu()) {
        if (action->text() == QLatin1String("Recent")) continue;  // changes as files are opened
        walkMenu(action->menu(), path + action->text() + QStringLiteral(" › "));
        continue;
      }
      Command command;
      command.id = idOf(QStringLiteral("menu."), path + action->text());
      command.name = path + action->text();
      command.group = QStringLiteral("Menus");
      command.fallback = menuKeys.value(action->text());
      command.action = action;
      commands.push_back(command);
    }
  };
  for (QAction* top : menuBar()->actions()) {
    if (top->menu()) {
      walkMenu(top->menu(), top->text() + QStringLiteral(" › "));
    } else if (!top->text().isEmpty()) {
      Command command;
      command.id = idOf(QStringLiteral("menu."), top->text());
      command.name = top->text();
      command.group = QStringLiteral("Menus");
      command.fallback = menuKeys.value(top->text());
      command.action = top;
      commands.push_back(command);
    }
  }
  // Commands with no button of their own.
  auto own = [&](const QString& id, const QString& name, const QString& group, const QString& fallback,
                 const std::function<void()>& run) {
    Command command;
    command.id = id;
    command.name = name;
    command.group = group;
    command.fallback = fallback;
    command.shortcut = new QShortcut(this);
    command.shortcut->setContext(Qt::WindowShortcut);
    connect(command.shortcut, &QShortcut::activated, this, run);
    commands.push_back(command);
  };
  const QString selecting = QStringLiteral("Selection"), looking = QStringLiteral("View");
  own(QStringLiteral("own.duplicate-in-place"), QStringLiteral("Duplicate in place"), selecting, QStringLiteral("Ctrl+D"),
      [this] { duplicateSelectionInPlace(); });
  own(QStringLiteral("own.select-all"), QStringLiteral("Select everything"), selecting, QStringLiteral("Ctrl+A"), [this] {
    selection.clear();
    for (const Node* node : doc.children(QString())) selection << node->id;
    refreshTree();
    loadInspector();
    refreshScene();
    status(QStringLiteral("%1 selected.").arg(selection.size()));
  });
  // Esc: clears a measurement while measuring, otherwise deselects everything.
  own(QStringLiteral("own.deselect"), QStringLiteral("Deselect (clears a measurement first)"), selecting, QStringLiteral("Esc"), [this] {
    if (tool == 8 && (measureHaveA || measureHaveB)) {
      emit view->measureCleared();
      return;
    }
    if (QWidget* focused = QApplication::focusWidget(); focused && focused != view) focused->clearFocus();
    if (selection.isEmpty()) return;
    selection.clear();
    refreshTree();
    loadInspector();
    refreshScene();
    status(QStringLiteral("Nothing selected."));
  });
  own(QStringLiteral("own.home"), QStringLiteral("Home view"), looking, QStringLiteral("Home"), [this] { view->goHome(); });
  own(QStringLiteral("own.orthographic"), QStringLiteral("Perspective / orthographic"), looking, QStringLiteral("O"),
      [this] { view->setOrthographic(!view->orthographic()); });
  own(QStringLiteral("own.isometric"), QStringLiteral("Isometric view"), looking, QStringLiteral("I"), [this] { view->isometricView(); });
  const QStringList faces = {QStringLiteral("Right"), QStringLiteral("Left"), QStringLiteral("Back"),
                             QStringLiteral("Front"), QStringLiteral("Top"), QStringLiteral("Bottom")};
  for (int face : {4, 3, 0, 1, 2, 5})
    own(QStringLiteral("own.view-") + faces[face].toLower(), faces[face] + QStringLiteral(" view"), looking, QString(),
        [this, face] { view->viewFrom(face); });
  applyKeys();
}

void MainWindow::applyKeys() {
  for (Command& command : commands) {
    const QKeySequence key(keyFor(command), QKeySequence::PortableText);
    if (command.shortcut) command.shortcut->setKey(key);
    if (!command.action) continue;
    command.action->setShortcut(key);
    if (command.group == QLatin1String("Menus") || command.action->text() == QLatin1String("Merge")) continue;
    const QString tip = toolTipFor(command.action->text());
    command.action->setToolTip(key.isEmpty() ? tip : QStringLiteral("%1  (%2)").arg(tip, key.toString(QKeySequence::NativeText)));
  }
}

// Settings > Keyboard: a key for every command. `commit` saves what was changed.
QWidget* MainWindow::buildKeysPage(QWidget* parent, std::function<void()>* commit) {
  auto* page = new QWidget(parent);
  auto* layout = new QVBoxLayout(page);
  auto* intro = new QLabel(QStringLiteral("Click a key and press the new combination. The × clears it; a key another "
                                          "command uses is moved over after asking."), page);
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto* filter = new QLineEdit(page);
  filter->setPlaceholderText(QStringLiteral("Find a command or a key…"));
  filter->setClearButtonEnabled(true);
  layout->addWidget(filter);
  auto* list = new QTreeWidget(page);
  list->setColumnCount(2);
  list->setHeaderLabels({QStringLiteral("Command"), QStringLiteral("Key")});
  list->setRootIsDecorated(false);
  list->setSelectionMode(QAbstractItemView::NoSelection);
  list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  list->header()->resizeSection(1, 190);
  list->header()->setStretchLastSection(false);
  layout->addWidget(list, 1);
  auto* reset = new QPushButton(QStringLiteral("Reset all to defaults"), page);
  auto* resetRow = new QHBoxLayout;
  resetRow->addStretch(1);
  resetRow->addWidget(reset);
  layout->addLayout(resetRow);

  struct Row {
    const Command* command;
    QTreeWidgetItem* item;
    QKeySequenceEdit* edit;
  };
  auto rows = std::make_shared<std::vector<Row>>();
  auto guard = std::make_shared<bool>(false);
  QHash<QString, QTreeWidgetItem*> groups;
  for (const Command& command : commands) {
    QTreeWidgetItem*& header = groups[command.group];
    if (!header) {
      header = new QTreeWidgetItem(list, QStringList{command.group});
      QFont bold = header->font(0);
      bold.setBold(true);
      header->setFont(0, bold);
      header->setForeground(0, QColor("#e87400"));
      header->setFirstColumnSpanned(true);
      header->setExpanded(true);
    }
    auto* item = new QTreeWidgetItem(header, QStringList{command.name});
    auto* edit = new QKeySequenceEdit(QKeySequence(keyFor(command), QKeySequence::PortableText), list);
    edit->setMaximumSequenceLength(1);
    edit->setClearButtonEnabled(true);
    edit->setToolTip(command.fallback.isEmpty() ? QStringLiteral("No key by default")
                                                : QStringLiteral("Default: %1").arg(QKeySequence(command.fallback).toString(
                                                      QKeySequence::NativeText)));
    list->setItemWidget(item, 1, edit);
    rows->push_back({&command, item, edit});
  }
  list->expandAll();
  for (const Row& row : *rows) {
    QKeySequenceEdit* edit = row.edit;
    const Command* command = row.command;
    connect(edit, &QKeySequenceEdit::editingFinished, page, [this, rows, guard, edit, command] {
      if (*guard || edit->keySequence().isEmpty()) return;
      for (const Row& other : *rows) {
        if (other.edit == edit || other.edit->keySequence() != edit->keySequence()) continue;
        const QString key = edit->keySequence().toString(QKeySequence::NativeText);
        const auto answer = QMessageBox::question(
            edit->window(), QStringLiteral("Key in use"),
            QStringLiteral("%1 is already the key for “%2”. Use it for “%3” instead?").arg(key, other.command->name, command->name));
        *guard = true;
        if (answer == QMessageBox::Yes) other.edit->clear();
        else edit->setKeySequence(QKeySequence(keyFor(*command), QKeySequence::PortableText));
        *guard = false;
        return;
      }
    });
  }
  connect(filter, &QLineEdit::textChanged, page, [list, rows](const QString& text) {
    const QString needle = text.trimmed();
    for (int g = 0; g < list->topLevelItemCount(); ++g) {
      QTreeWidgetItem* header = list->topLevelItem(g);
      bool any = false;
      for (const Row& row : *rows) {
        if (row.item->parent() != header) continue;
        const bool show = needle.isEmpty() || row.command->name.contains(needle, Qt::CaseInsensitive) ||
                          row.edit->keySequence().toString(QKeySequence::NativeText).contains(needle, Qt::CaseInsensitive);
        row.item->setHidden(!show);
        any = any || show;
      }
      header->setHidden(!any);
    }
  });
  connect(reset, &QPushButton::clicked, page, [rows, guard] {
    *guard = true;
    for (const Row& row : *rows) row.edit->setKeySequence(QKeySequence(row.command->fallback, QKeySequence::PortableText));
    *guard = false;
  });
  *commit = [this, rows] {
    QSettings settings;
    for (const Row& row : *rows) {
      const QString key = row.edit->keySequence().toString(QKeySequence::PortableText);
      const QString name = QStringLiteral("keys/") + row.command->id;
      if (key == row.command->fallback) settings.remove(name);
      else settings.setValue(name, key);
    }
    applyKeys();
  };
  return page;
}

// The Merge button is lit while the one selected part is merged (it then unmerges).
void MainWindow::updateMergeButton() {
  const Node* single = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
  const bool merged = single && single->kind == Node::Kind::Group && single->merged;
  for (QAction* action : findChildren<QAction*>()) {
    if (action->text() != QLatin1String("Merge")) continue;
    action->setChecked(merged);
    action->setToolTip(merged ? QStringLiteral("Unmerge: bring back the parts of this merged part") : toolTipFor(QStringLiteral("Merge")));
  }
}

void MainWindow::syncActions() {
  for (QAction* action : findChildren<QAction*>()) {
    const QString text = action->text();
    if (text == "Move") action->setChecked(tool == 0 && panel == 0);
    else if (text == "Rotate") action->setChecked(tool == 1 && panel == 0);
    else if (text == "Bend face") action->setChecked(panel == 3);
    else if (text == "Extrude face") action->setChecked(panel == 6);
    else if (text == "Fillet") action->setChecked(tool == 4);
    else if (text == "Fillet surface") action->setChecked(tool == 7);
    else if (text == "Detected Edge Filleting") action->setChecked(tool == 9);
    else if (text == "Merge") updateMergeButton();
    else if (text == "Repeat") action->setChecked(repeatPopup && repeatPopup->isVisible());
    else if (text == "Align") action->setChecked(alignPopup && alignPopup->isVisible());
    else if (text == "Measure") action->setChecked(tool == 8);
    else if (text == "Lay flat") action->setChecked(tool == 12);
    else if (text == "Workplane") action->setChecked(tool == 13);
    else if (text == "Bend") action->setChecked(panel == 2);
    else if (text == "Mirror") action->setChecked(panel == 4);
    else if (text == "Hollow") action->setChecked(panel == 7);
    else if (text == "Split") action->setChecked(panel == 8);
  }
}

void MainWindow::showPanel(int next) {
  if (alignPopup) alignPopup->hide();
  if (repeatPopup) repeatPopup->hide();
  if (filletPopup) filletPopup->hide();
  if (faceFilletPopup) faceFilletPopup->hide();
  if (edgeBetaPopup) edgeBetaPopup->hide();
  const bool opening = panel != next;
  panel = opening ? next : 0;
  if (panel == 3) {
    tool = 3;
    view->setTool(3);
  } else if (panel == 2 || panel == 4) {
    tool = 0;
    view->setTool(0);
  } else if (tool == 3 || tool == 4 || tool == 6 || tool == 7 || tool == 9 || tool == 12 || tool == 13) {
    tool = 0;
    view->setTool(0);
  }
  syncActions();
  loadInspector();
  if (panel == 2) status("Bend curves this whole solid. Bend face makes a separate object.");
  else if (panel == 3) status("Click a face. A new bend object is created. The original is left alone.");
  else if (panel == 4) status("Mirror flips the shape through its own center.");
  else if (panel == 6) status("Select a face, set depth, and click the face in the viewport.");
  else if (panel == 7) status("Set the wall and the open sides, then Hollow. Detailed parts take a moment.");
  else if (panel == 8) status("Pick the plane and where it cuts, then Split. Both halves stay editable groups.");
  if (panel == 7) loadHollowPage();
  if (panel == 8) loadSplitPage();
}

void MainWindow::loadInspector() {
  updateMergeButton();
  loading = true;
  const Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
  const bool hasSelection = !selection.isEmpty();
  const bool multiSelection = selection.size() > 1;
  const bool shape = node && node->kind == Node::Kind::Shape;
  const bool pattern = node && node->kind == Node::Kind::Pattern;
  const bool elbow = shape && node->shape == QLatin1String("elbow");
  const bool editable = node && node->kind != Node::Kind::Pattern;
  nameEdit->setEnabled(node != nullptr);
  nameEdit->setText(node ? node->name : QString());
  if (colorButton) {
    QColor representative;
    bool canColor = false;
    bool sameColor = true;
    for (const QString& id : selection) {
      const Node* candidate = doc.find(id);
      if (!candidate || candidate->kind == Node::Kind::Pattern) continue;
      if (!canColor) representative = candidate->color;
      else if (representative != candidate->color) sameColor = false;
      canColor = true;
    }
    colorButton->setEnabled(canColor);
    if (canColor) {
      if (sameColor) {
        const int luminance = qRound(0.299 * representative.red() + 0.587 * representative.green() + 0.114 * representative.blue());
        const QString fg = luminance < 140 ? QStringLiteral("#f4f4f4") : QStringLiteral("#171717");
        colorButton->setText(representative.name(QColor::HexRgb));
        colorButton->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; border: 1px solid #343c48; } QPushButton:hover { border: 1px solid #e87400; }").arg(representative.name(QColor::HexRgb), fg));
      } else {
        colorButton->setText(QStringLiteral("Mixed colors"));
        colorButton->setStyleSheet(QStringLiteral("QPushButton { background: #343c48; color: #f4f4f4; border: 1px solid #343c48; } QPushButton:hover { border: 1px solid #e87400; }"));
      }
    } else {
      colorButton->setText(QStringLiteral("Color"));
      colorButton->setStyleSheet(QString());
    }
  }
  for (int i = 0; i < 3; ++i) {
    posSpin[i]->setEnabled(hasSelection);
    rotSpin[i]->setEnabled(hasSelection);
    posSpin[i]->setPrefix(multiSelection ? QStringLiteral("Δ ") : QString());
    rotSpin[i]->setPrefix(multiSelection ? QStringLiteral("Δ ") : QString());
    if (!node) {
      posSpin[i]->setValue(multiSelection ? 0 : posSpin[i]->value());
      rotSpin[i]->setValue(multiSelection ? 0 : rotSpin[i]->value());
      continue;
    }
    if (multiSelection) {
      posSpin[i]->setValue(multiPosLast[i]);
      rotSpin[i]->setValue(multiRotLast[i]);
    } else {
      posSpin[i]->setValue(node->pos[i]);
      rotSpin[i]->setValue(node->rot[i]);
    }
  }
  const bool moveTool = tool == 0 && panel == 0;
  const bool rotateTool = tool == 1 && panel == 0;
  if (movePage) movePage->setVisible(moveTool && hasSelection);
  shapePage->setVisible(moveTool && ((shape && !multiSelection) || multiSelection));
  auto* shapeForm = qobject_cast<QFormLayout*>(shapePage ? shapePage->layout() : nullptr);
  if (shapeForm) {
    const QStringList sizeLabels = multiSelection ? QStringList{QStringLiteral("Scale X %"), QStringLiteral("Scale Y %"), QStringLiteral("Scale Z %")}
                                                   : QStringList{QStringLiteral("Length X mm"), QStringLiteral("Width Y mm"), QStringLiteral("Height Z mm")};
    for (int i = 0; i < 3; ++i) {
      if (QLabel* label = qobject_cast<QLabel*>(shapeForm->labelForField(sizeSpin[i]))) label->setText(sizeLabels[i]);
      sizeSpin[i]->setSuffix(multiSelection ? QStringLiteral(" %") : QString());
      if (multiSelection) {
        sizeSpin[i]->setRange(1.0, 1000.0);
        sizeSpin[i]->setSingleStep(1.0);
        sizeSpin[i]->setDecimals(1);
        sizeSpin[i]->setValue(multiScaleLast[i]);
      } else {
        sizeSpin[i]->setRange(0.05, 10000.0);
        sizeSpin[i]->setSingleStep(1.0);
        sizeSpin[i]->setDecimals(3);
      }
    }
  }
  const bool generator = shape && !multiSelection && node->shape == QLatin1String("generator");
  if (genPage) {
    genPage->setVisible(moveTool && generator);
    if (generator) loadGeneratorPage(node);
  }
  // A generator's size comes from its settings; the size rows stay hidden, so drop the empty page.
  if (generator) shapePage->setVisible(false);
  if (holePage) {
    const bool holes = moveTool && !selectedHoles().empty();
    holePage->setVisible(holes);
    if (holes) loadHolePage();
  }
  if (hullPage) {
    const bool group = moveTool && !multiSelection && node && node->kind == Node::Kind::Group;
    hullPage->setVisible(group);
    if (group) loadHullPage();
  }
  rotPage->setVisible(rotateTool);
  bendPage->setVisible(panel == 2 && !pattern);
  facePage->setVisible(panel == 3 || (elbow && moveTool));
  if (faceRadialSpin) faceRadialSpin->setVisible((panel == 3 || (elbow && moveTool)) && faceSpiralBox && faceSpiralBox->isChecked());
  extrudePage->setVisible(panel == 6);
  mirrorPage->setVisible(panel == 4);
  if (hollowPage) {
    hollowPage->setVisible(panel == 7);
    if (panel == 7) loadHollowPage();
  }
  if (splitPage) {
    splitPage->setVisible(panel == 8);
    if (panel == 8) loadSplitPage();
  }
  for (int i = 0; i < 3; ++i) if (mirrorBox[i]) mirrorBox[i]->setEnabled(shape && !elbow);
  patternPage->setVisible(pattern);
  if (platePage && platePage->parentWidget() == this) platePage->hide();
  auto setShapeRowVisible = [shapeForm](QWidget* field, bool visible) {
    if (!field) return;
    field->setVisible(visible);
    if (shapeForm) {
      if (QWidget* label = shapeForm->labelForField(field)) label->setVisible(visible);
    }
  };
  for (int i = 0; i < 3; ++i) setShapeRowVisible(sizeSpin[i], node != nullptr || multiSelection);
  setShapeRowVisible(radiusSpin, false);
  setShapeRowVisible(radius2Spin, false);
  setShapeRowVisible(gemTableSpin, false);
  setShapeRowVisible(sidesSpin, false);
  for (QWidget* field : std::initializer_list<QWidget*>{threadBox, threadPitchSpin, threadClearanceSpin}) setShapeRowVisible(field, false);
  setShapeRowVisible(segmentsSpin, false);
  for (QWidget* field : std::initializer_list<QWidget*>{textEdit, fontBox, fontStyleBox, fontSizeSpin, letterSpacingSpin,
                                                         textAlignBox, textDetailBox, textArcSpin, textArcBox})
    setShapeRowVisible(field, false);
  if (shape && !multiSelection) {
    double inspectorLo[3] = {0.0, 0.0, 0.0};
    double inspectorHi[3] = {0.0, 0.0, 0.0};
    // A big group is merged (and an imported mesh loaded) in the background by refreshScene; until
    // then show its own size, and come back here when that is done instead of doing it here and
    // freezing the window.
    const bool boundsReady = meshReady(doc, *node);
    if (!boundsReady) inspectorWaitsForScene = true;
    const bool haveInspectorBounds = boundsReady && localBounds(doc, *node, inspectorLo, inspectorHi);
    radiusSpin->setValue(node->radius);
    radius2Spin->setValue(node->radius2);
    const QString& kind = node->shape;
    const int minimumSides = kind == QLatin1String("diamond") ? 4 : 3;
    const bool star = kind == "star" || kind == "starflat";
    const int maxSides = star ? 64 : 256;  // the most points a star is built with
    sidesSpin->setRange(minimumSides, maxSides);
    sidesSpin->setValue(std::clamp(node->sides, minimumSides, maxSides));
    setShapeRowVisible(radiusSpin, (kind != "box" && kind != "wedge" && kind != "pyramid" && kind != "elbow" && kind != "stl" &&
                                     kind != "heart" && kind != "diamond" && kind != "text" && kind != "generator" && kind != "roof") ||
                                        kind == "facefillet");
    setShapeRowVisible(radius2Spin, kind == "cone" || kind == "tube" || kind == "torus" || star);
    setShapeRowVisible(sidesSpin, kind == "prism" || kind == "diamond" || kind == "cylinder" || kind == "facefillet" || star);
    if (gemTableSpin) {
      setShapeRowVisible(gemTableSpin, kind == "diamond");
      if (kind == "diamond") gemTableSpin->setValue(std::round(node->gemTable * 100));
    }
    if (threadBox && kind == "cylinder") {
      setShapeRowVisible(threadBox, true);
      const int at = threadBox->findData(node->thread);
      threadBox->setCurrentIndex(at < 0 ? 0 : at);
      const bool on = !node->thread.isEmpty();
      setShapeRowVisible(threadPitchSpin, on);
      setShapeRowVisible(threadClearanceSpin, on && node->hole);
      threadPitchSpin->setValue(node->threadPitch);
      threadClearanceSpin->setValue(node->threadClearance);
      // A thread is round by its own resolution; the curve segments do nothing on it.
      if (on) setShapeRowVisible(sidesSpin, false);
    }
    if (QLabel* innerLabel = qobject_cast<QLabel*>(shapeForm->labelForField(radius2Spin)))
      innerLabel->setText(star ? "Inner radius" : "Second radius");
    const bool roundCurve = kind == "sphere" || kind == "hemisphere" || kind == "cone" || kind == "torus" || kind == "tube" ||
                            kind == "capsule" || kind == "heart";
    setShapeRowVisible(segmentsSpin, roundCurve);
    if (segmentsSpin && roundCurve) segmentsSpin->setValue(std::clamp(node->segments, 8, 256));
    if (QLabel* sideLabel = qobject_cast<QLabel*>(shapeForm->labelForField(sidesSpin)))
      sideLabel->setText(kind == "facefillet" ? "Segments" : kind == "cylinder" ? "Curve segments" : star ? "Points" : "Sides");
    if (QLabel* radiusLabel = qobject_cast<QLabel*>(shapeForm->labelForField(radiusSpin)))
      radiusLabel->setText(kind == "facefillet" ? "Radius mm" : star ? "Outer radius" : "Radius");
    if (kind == "facefillet") {
      sidesSpin->setRange(1, 20);
      for (int i = 0; i < 3; ++i) setShapeRowVisible(sizeSpin[i], false);
    }
    // A generator is sized by its own settings (scaling it would scale the clearances too).
    if (kind == "generator")
      for (int i = 0; i < 3; ++i) setShapeRowVisible(sizeSpin[i], false);
    {
      const bool isText = kind == "text";
      for (QWidget* field : std::initializer_list<QWidget*>{textEdit, fontBox, fontStyleBox, fontSizeSpin, letterSpacingSpin,
                                                             textAlignBox, textDetailBox, textArcSpin, textArcBox})
        setShapeRowVisible(field, isText);
      if (isText) {
        // Only touch the text box when it differs, so the cursor does not jump while typing.
        if (textEdit->toPlainText() != node->text) textEdit->setPlainText(node->text);
        const QString family = node->fontFamily.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::GeneralFont).family()
                                                           : node->fontFamily;
        if (fontBox->currentFont().family() != family) fontBox->setCurrentFont(QFont(family));
        fillFontStyles(family, node->fontStyle);
        fontSizeSpin->setValue(node->fontSize);
        letterSpacingSpin->setValue(node->letterSpacing);
        textAlignBox->setCurrentIndex(std::clamp(node->textAlign, 0, 2));
        textDetailBox->setCurrentIndex(std::clamp(node->textDetail, 0, textDetailCount() - 1));
        textArcSpin->setValue(node->textArc);
        textArcBox->setCurrentIndex(node->textArcBelow ? 1 : 0);
        textArcBox->setEnabled(node->textArc > 0.01);
      }
    }
    if (filletSideBox && filletDirBox) {
      const bool face = kind == "facefillet";
      setShapeRowVisible(filletSideBox, face);
      setShapeRowVisible(filletDirBox, face);
      if (face) {
        filletSideBox->setCurrentIndex(node->filletOuter ? 1 : 0);
        filletDirBox->setCurrentIndex(node->filletUp ? 1 : 0);
      }
    }
    for (int i = 0; i < 3; ++i) {
      mirrorBox[i]->setChecked(node->mirror[i]);
      sizeSpin[i]->setSuffix(QString());
      sizeSpin[i]->setRange(0.05, 10000.0);
      sizeSpin[i]->setSingleStep(1.0);
      sizeSpin[i]->setDecimals(3);
      const double effectiveSize = haveInspectorBounds
                                        ? (inspectorHi[i] - inspectorLo[i]) * std::abs(node->scale[i])
                                        : node->size[i] * std::abs(node->scale[i]);
      sizeSpin[i]->setValue(std::max(0.05, effectiveSize));
    }
  }
  if (editable) {
    if (tool != 4) {
      edgeSpin->setValue(node->edgeFillet);
      surfaceSpin->setValue(node->surfaceFillet);
    }
    bendSpin->setValue(node->bend);
    alongBox->setCurrentIndex(std::clamp(node->along, 0, 2));
    towardBox->setCurrentIndex(std::clamp(node->toward, 0, 2));
    twistSpin->setValue(node->twist);
    int twistAxis = std::clamp(node->twistAxis, 0, 2);
    if (std::abs(node->twist) < 0.05) {
      // Not twisted yet: offer the part's longest side, which is what a strip twists around.
      double lo[3];
      double hi[3];
      if (!meshReady(doc, *node)) inspectorWaitsForScene = true;
      else if (localBounds(doc, *node, lo, hi)) {
        double best = -1;
        for (int axis = 0; axis < 3; ++axis) {
          const double extent = (hi[axis] - lo[axis]) * std::abs(node->scale[axis]);
          if (extent > best + 1e-9) {
            best = extent;
            twistAxis = axis;
          }
        }
      }
    }
    twistAxisBox->setCurrentIndex(twistAxis);
    taperSpin->setValue(node->taper);
    taperAxisBox->setCurrentIndex(std::clamp(node->taperAxis, 0, 2));
  }
  if (elbow) {
    faceAngleSpin->setValue(node->faceAngle);
    faceRadiusSpin->setValue(node->faceRadius);
    faceRiseSpin->setValue(node->faceRise);
    faceStepsSpin->setValue(node->faceSteps);
    if (faceSpiralBox) faceSpiralBox->setChecked(node->faceSpiral);
    if (faceRadialSpin) faceRadialSpin->setValue(node->faceRadialStep);
    faceAxisBox->setCurrentIndex(std::clamp(node->faceAxisMode, 0, 3));
  } else if (panel == 3) {
    faceAngleSpin->setValue(draftAngle);
    faceRadiusSpin->setValue(draftRadius);
    faceRiseSpin->setValue(draftRise);
    faceStepsSpin->setValue(draftSteps);
    if (faceSpiralBox) faceSpiralBox->setChecked(false);
    if (faceRadialSpin) faceRadialSpin->setValue(0);
    if (faceRadialSpin) faceRadialSpin->setVisible(false);
    faceAxisBox->setCurrentIndex(std::clamp(draftAxis, 0, 3));
  }
  if (view && rotationStepSpin) view->setRotationSnap(rotationStepSpin->value());
  if (facePresetBox) {
    const double angle = elbow ? faceAngleSpin->value() : draftAngle;
    const double presetValues[] = {0.0, -90.0, 90.0, 180.0, 270.0, 360.0, -270.0, -360.0};
    int preset = 8;
    for (int i = 0; i < 8; ++i) {
      if (std::abs(angle - presetValues[i]) < 0.001) { preset = i; break; }
    }
    facePresetBox->setCurrentIndex(preset);
  }
  if (pattern) {
    countSpin->setValue(node->count);
    for (int i = 0; i < 3; ++i) {
      moveSpin[i]->setValue(node->move[i]);
      drotSpin[i]->setValue(node->drot[i]);
      resizeSpin[i]->setValue(node->resize[i]);
    }
  }
  workplaneSpin->setValue(doc.workplaneZ);
  bedXSpin->setValue(doc.bedX);
  bedYSpin->setValue(doc.bedY);
  int preset = 5;
  const double presets[][2] = {{180, 180}, {220, 220}, {250, 210}, {256, 256}, {300, 300}};
  for (int i = 0; i < 5; ++i) {
    if (std::abs(doc.bedX - presets[i][0]) < 0.1 && std::abs(doc.bedY - presets[i][1]) < 0.1) preset = i;
  }
  bedPreset->setCurrentIndex(preset);
  snapSpin->setValue(doc.snap);
  if (tool != 4) filletSegSpin->setValue(std::clamp(doc.filletSegments, 8, 72));
  syncFilletChrome();
  syncEdgeBetaChrome();
  if (toolTitle && toolHint && toolPopup) {
    QString title = QStringLiteral("Move");
    QString hint = QStringLiteral("Drag the body on X/Y. White corners scale all sides. Black squares scale one side. Position and size are in this window.");
    if (panel == 2) {
      title = QStringLiteral("Bend");
      hint = QStringLiteral("Curves this whole solid: pick the axis it runs along and the direction it bends toward. "
                            "Twist wrings it around an axis from one end to the other; both can be used together.");
    } else if (panel == 3) {
      title = QStringLiteral("Bend face");
      hint = QStringLiteral("Click a face. A new bend object is created. The original is left alone.");
    } else if (panel == 4) {
      title = QStringLiteral("Mirror");
      hint = QStringLiteral("Flip the shape through its own center.");
    } else if (panel == 6) {
      title = QStringLiteral("Extrude face");
      hint = QStringLiteral("Set the depth, then click a face. Positive depth grows outward. Negative grows inward.");
    } else if (panel == 7) {
      title = QStringLiteral("Hollow");
      hint = QStringLiteral("Makes the selected part (or group) a shell with walls of one thickness, closed or open on the "
                            "sides you tick. Boxes, cups, cases.");
    } else if (panel == 8) {
      title = QStringLiteral("Split");
      hint = QStringLiteral("Cuts the selected part in two with a plane. Each half is a group (the part and a cutter), so you "
                            "can still move the cut or ungroup it.");
    } else if (tool == 1) {
      title = QStringLiteral("Rotate");
      hint = QStringLiteral("Drag a ring to rotate: in the disk's inner band it snaps to the rotation step (0: 15 degrees), "
                            "in the outer band it turns freely. Or type the degrees.");
    } else if (pattern) {
      title = QStringLiteral("Repeat");
      hint = QStringLiteral("Copies, move, rotate, and resize for this repeat.");
    } else if (elbow && moveTool) {
      title = QStringLiteral("Bend face");
      hint = QStringLiteral("Turn, radius, steps, and rise of this bend. Position and size are here too.");
    } else if (moveTool && generator) {
      const gen::Spec* spec = gen::spec(node->genType);
      title = spec ? spec->name : QStringLiteral("Generator");
      hint = spec ? spec->blurb : QStringLiteral("This generator is not known to this version of DaedalCAD.");
    }
    toolTitle->setText(title);
    toolHint->setText(hint);
    const bool special = (filletPopup && filletPopup->isVisible()) || (faceFilletPopup && faceFilletPopup->isVisible()) ||
                         (repeatPopup && repeatPopup->isVisible()) || (alignPopup && alignPopup->isVisible());
    toolPopup->setVisible(!special);
  }
  for (QAction* action : findChildren<QAction*>()) {
    if (action->text() == QLatin1String("Snap")) {
      action->blockSignals(true);
      action->setChecked(doc.snapOn);
      action->blockSignals(false);
    } else if (action->text() == QLatin1String("Grid")) {
      action->blockSignals(true);
      action->setChecked(doc.gridOn);
      action->blockSignals(false);
    }
  }
  loading = false;
  placeOverlays();
  updateAlignOverlay();
  updateMeasureOverlay();
}

void MainWindow::pushUndo() {
  undoStack.push_back(doc);
  if (undoStack.size() > 200) undoStack.erase(undoStack.begin());
  redoStack.clear();
}

void MainWindow::undo() {
  if (undoStack.empty()) return;
  redoStack.push_back(doc);
  doc = std::move(undoStack.back());
  undoStack.pop_back();
  selection.clear();
  refreshTree();
  loadInspector();
  refreshScene();
  status("Undone.");
}

void MainWindow::redo() {
  if (redoStack.empty()) return;
  undoStack.push_back(doc);
  doc = std::move(redoStack.back());
  redoStack.pop_back();
  selection.clear();
  refreshTree();
  loadInspector();
  refreshScene();
  status("Redone.");
}

void MainWindow::status(const QString& text) {
  // Messages go in a label of their own: Qt's own status messages would hide the usage readout.
  if (messageLabel) messageLabel->setText(text);
  else statusBar()->showMessage(text);
}

void MainWindow::updateUsage() {
  if (!usageMeter || !usageLabel) return;
  const Usage u = usageMeter->sample();
  if (!u.valid) {
    usageLabel->hide();
    return;
  }
  const QString memory = u.memoryMb >= 1024 ? QStringLiteral("%1 GB").arg(u.memoryMb / 1024.0, 0, 'f', 2)
                                            : QStringLiteral("%1 MB").arg(u.memoryMb, 0, 'f', 0);
  usageLabel->setText(QStringLiteral("CPU %1 %  \u00b7  RAM %2").arg(u.cpu, 2, 'f', 0).arg(memory));
  // Busy (a big merge) shows orange, so a slow moment explains itself.
  usageLabel->setStyleSheet(u.cpu >= 50 ? QStringLiteral("color: #e87400; padding: 0 10px 0 4px;")
                                        : QStringLiteral("color: #8a8074; padding: 0 10px 0 4px;"));
  usageLabel->setToolTip(QStringLiteral("DaedalCAD's share of all %1 CPU cores over the last second, and the memory it holds "
                                        "now (most so far: %2 MB, of %3 GB in this computer).")
                             .arg(u.cores)
                             .arg(u.peakMb, 0, 'f', 0)
                             .arg(u.totalMb / 1024.0, 0, 'f', 1));
}

// Help: one page per tool. A picture for a page is optional: put help_<key>.png (or .jpg) in
// appdata/help next to the program and it shows above the text. The keys are listed in the
// first column below and in appdata/help/README.txt.
void MainWindow::showHelp() {
  struct Topic {
    const char* key;
    const char* title;
    const char* text;
  };
  static const Topic topics[] = {
      {"view", "Moving the view",
       "<b>Home</b> (under the view cube, or the Home key) glides to the whole plate seen from its front-right corner. "
       "<b>Persp / Ortho</b> beside it switches between perspective and orthographic (no perspective: parallel edges stay "
       "parallel and sizes compare across the view; O toggles it), and I gives the isometric view. Every key can be changed "
       "in <b>Settings › Keyboard</b>.<br><br>"
       "<b>Orbit:</b> drag with the right mouse button (or Alt + left). <b>Pan:</b> drag with the middle button (or Shift + left, "
       "or hold Space and drag with the left button); "
       "the spot you grab stays under the cursor. <b>Zoom:</b> the mouse wheel.<br><br>"
       "Hold <b>Ctrl</b> while orbiting, panning or zooming for fine control at a quarter of the speed. The view glides to where "
       "you point it, so slow, small moves come out smooth.<br><br>"
       "The cube in the corner turns to a straight view of the face you click and puts the focus point back in the middle of the plate (10 mm "
       "up). The arrows round it turn to the face on that side (from the front, the up arrow goes to the top) and keep the focus "
       "point, lifting it above the plate if it was below. <b>Focus</b> shows the point the view turns and zooms around: drag the "
       "marker (the view waits and glides over when you let go) or click the bed. The focus point stays within one plate size "
       "of the plate's edges.<br><br>Picking a part (in the view or the list) or placing a new one makes its middle the focus "
       "point, so the view turns and zooms around it straight away; move the focus afterwards as you like."},
      {"move", "Move",
       "Select a part and drag it across the bed to move it in X and Y. Drag the blue arrow to lift it in Z.<br><br>"
       "<b>White corner cubes</b> scale all sides together. <b>Black squares</b> on the faces scale one side.<br><br>"
       "The Move window shows the position and size. Type exact millimetres there. With Snap on, moves land on the snap step."},
      {"rotate", "Rotate",
       "Drag one of the three rings to turn the selection around that axis: red X, green Y, blue Z.<br><br>"
       "While you drag, a disk shows the angle. Keep the cursor in its <b>inner band</b> and the turn snaps in steps "
       "(15 degrees, or the <b>Rotation step</b> you set); move it out to the <b>outer band</b> and it turns freely. "
       "The angle so far is shown next to the cursor.<br><br>"
       "The degrees can also be typed."},
      {"mirror", "Mirror",
       "Flip the selected part through its own centre on X, Y or Z with the Flip boxes. Flipping twice brings it back."},
      {"align", "Align",
       "Select two or more parts and press Align. Dots appear around the selection in the 3D view: three for each axis, at the "
       "low side, the middle and the high side.<br><br>"
       "<b>Click a dot</b> to line every part up on that side. <b>Click one of the parts</b> first to make it the one that stays "
       "(it turns orange); the others move to it. Without one, they line up on the selection as a whole. Click it again to clear it.<br><br>"
       "The buttons in the Align window do the same as the dots."},
      {"drawing", "Technical drawing",
       "<b>File &gt; Technical drawing</b> makes a sheet of the selection, or of everything on the plate when nothing is "
       "selected: front, top and side views, an isometric view, a title block and a filament estimate.<br><br>"
       "<b>Views:</b> first angle (ISO, usual in Europe) puts the top view under the front view and the view from the left on "
       "its right; third angle (US) puts the top view above and the view from the right on the right. Edges hidden behind the "
       "part are dashed. The scale is the largest standard one that fits the paper, or pick one.<br><br>"
       "<b>More sheets:</b> when the scale you pick is too big for the paper, the first sheet shows the whole part at a scale "
       "that fits, and each view follows on a sheet of its own at your scale. A view bigger than a whole sheet comes in "
       "pieces that overlap by 10 mm, each marked with its row and column. Previous and Next under the picture page through "
       "them. PDF puts every sheet in one file; SVG, DXF and PNG save one file per sheet (name, name-2, name-3...).<br><br>"
       "<b>Dimensions:</b> the overall width, height and depth, and every round hole or peg seen square on: its diameter "
       "(\"4\u00d7 \u00d85\" for four the same) and how far its centre is from the left and bottom edges. Sizes come from the "
       "finished solid, so they include clearances and cuts.<br><br>"
       "<b>Filament estimate:</b> the walls (all round, top and bottom) count as solid and the inside at the infill you set, times "
       "the material's density. Length is for 1.75 mm filament; cost uses your price per kg. Supports, brims and purge are not "
       "in it, so the slicer's number is the one to trust; this one tells you roughly what a part costs before you slice it.<br><br>"
       "Save it as <b>PDF</b> to print, <b>SVG</b> for drawing programs, <b>DXF</b> for CAD and laser cutters (outlines, hidden "
       "lines, dimensions and text on their own layers) or <b>PNG</b>. Paper, projection and filament settings are kept for "
       "next time."},
      {"export", "Export",
       "<b>File &gt; Export</b> writes everything on the plate, <b>Export selected object</b> only what is selected. Holes "
       "left loose on the bed still cut the parts they reach.<br><br>"
       "<b>STL</b> is one merged mesh, for the slicer. <b>OBJ</b> and <b>3MF</b> keep every part (a part on the plate, or a "
       "whole group) as its own named object. <b>DXF</b> puts each part's mesh on its own layer.<br><br>"
       "<b>STEP</b> is for other CAD programs: every part becomes a closed solid with "
       "its own name and colour, in millimetres. Boxes, cylinders, cones, tubes, spheres, domes, rings and capsules go in as "
       "true surfaces, and so do the holes, groups, intersections, repeats and closed hollows made of them: in the other "
       "program you can click a hole and read off its diameter, and fillet its edge. Parts imported from a STEP file go back "
       "out exactly as they came in, and so do rounded edges: a shape's own Fillet and the edges picked with Edge fillet "
       "(Beta), rounded or chamfered, on shapes and on groups (OpenCASCADE rounds the same edges with the same radius). Text "
       "comes from the font's own curves and shapes from the 2D editor from their nodes and handles, so letters and logos "
       "are smooth too. Threads are true helical sweeps, and bends and twists smooth spline surfaces. In generated parts and "
       "imported STLs, the round areas their triangles describe (holes, posts, rounded corners, countersinks: cylinders and "
       "cones) are recognised and made exact; the rest of them, and Fillet surface, hulls, hollows open on a side and SVGs "
       "from elsewhere, go in as flat faces, inside the same solid. A window shows the progress and, per part, whether it came out exact, partly exact or from the mesh, and why "
       "(tick <b>Only open this window when a part is not exact</b> to see it only then).<br><br>"
       "When a build has no STEP support (the menu then reads <b>STEP (faceted)</b>), every part is written from its triangles: "
       "flat areas as single faces with shared edges, round surfaces as bands of narrow flat faces.<br><br>"
       "<b>File &gt; Import STEP</b> puts every solid of a STEP file on the plate, with its names and colours, placed as in "
       "the file and stood on the plate in the middle. They work like imported STLs here (move, turn, group, cut with holes), "
       "and a STEP export writes them exactly again. The file is copied into Imported\\STEP in your DaedalCAD data folder, so "
       "projects keep working when the original moves."},
      {"lay_flat", "Lay flat",
       "Press <b>Lay flat</b>, then click a face of a part: the whole part turns, the short way round, so that face lies on the "
       "plate, and it drops onto the plate by its real lowest point. The part keeps its size and stays where it was on the plate. "
       "Click more faces (of the same part or others) to keep going; press Lay flat again to stop. Ctrl+Z undoes each one."},
      {"workplane", "Workplane",
       "Press <b>Workplane</b> and click a face, also a slanted one or a wall: an orange grid appears on it, and new parts "
       "(shapes, generators, STL and SVG files) stand on that face, upright to it, instead of on the plate. Clicking a part in "
       "the library puts it in the middle of the face; dragging one puts it where you let go. On a wall the parts stand with "
       "their top pointing up the wall.<br><br>"
       "To go back to the plate, press Workplane and click the empty plate (anywhere off the parts). Moving a part afterwards "
       "works as usual, along the plate."},
      {"section", "Section view",
       "<b>Section</b> cuts the view open to see inside: walls, holes, threads, the O-ring groove of a sealed box. A bar appears "
       "under the view: pick the direction of the cut (X, Y or Z), drag the slider to move it through the model (it runs across "
       "the whole print area, in plate millimetres, and starts in the middle of your parts) or type where it should be, and <b>Flip</b> to keep the other side. Where a part is cut, the cut face shows hatched red, like on a "
       "drawing; a dashed red frame shows where the cut is.<br><br>"
       "Only the view is cut: the parts, the export and printing are untouched. You can keep working while it is on."},
      {"threads", "Threads on cylinders",
       "Any cylinder can be threaded: select it and pick a size under <b>Thread</b> (M2 to M36, or Custom for its own "
       "diameter). The size sets the diameter; <b>Pitch</b> is the standard coarse pitch unless you set one.<br><br>"
       "A solid cylinder becomes a threaded rod with chamfered ends. Made a <b>hole</b>, it cuts a threaded hole that a bolt of "
       "that size screws into, with a lead-in at both ends. <b>Thread play</b> is how much wider the hole is cut all round: "
       "it starts at a quarter of the pitch (0.1 to 0.25 mm), which suits most printers; the Thread fit test generator tells "
       "what yours likes."},
      {"measure", "Measure",
       "Press Measure, then click two points. The distance and the X, Y and Z difference show on the view and in the status bar. "
       "A line follows the cursor after the first click.<br><br>"
       "Clicks near a corner or the middle of an edge snap to it. A click on empty bed snaps to the grid when Snap is on. "
       "Esc clears the points. A third click starts a new measurement.<br><br>"
       "If something is selected, the readout also shows its size: width (X), depth (Y) and height (Z)."},
      {"bend", "Bend",
       "Curves a whole solid into an arc. Pick the axis the bend runs along, the direction it bends toward, and how far it bends.<br><br>"
       "<b>Twist</b> wrings the part around one axis: <b>Twist deg</b> is how far one end turns against the other (180 is a half "
       "turn, 720 two full turns) and <b>Twist around</b> is the axis, normally the long side of a strip, which is picked for you. "
       "The middle stays put. It works on any part or group, so cut holes into a strip first (group them), then twist the group. "
       "Twist is made before the bend, so a strip can be twisted and curved at once.<br><br>"
       "A bend of exactly <b>360</b> degrees closes the part into a ring: a flat strip with text on it (group them first) "
       "becomes a bracelet or a napkin ring, its ends joined into one piece.<br><br>"
       "<b>Taper end</b> makes one end narrower (or wider) than the other: the far end, on the high side of <b>Taper "
       "along</b>, is that percent as wide as the near end, and the slices between follow evenly. 50 % gives a box that "
       "narrows to half, a cylinder becomes a cone. Taper is made first, so a tapered part can still be twisted and bent."},
      {"bend_face", "Bend face",
       "Click a flat face. A new bent piece grows out of that face; the original part is left alone. "
       "Angle, radius and steps shape the bend; Rise and Spiral turn it into a helix.<br><br>"
       "<b>Radius</b> is the inside radius of the bend: from the face's inner edge to the line it turns round, as on bent "
       "sheet metal. The whole face turns, so the end is always as long as the face it grew from. (Bends from projects "
       "saved before 0.9.7's fix keep their old shape.)"},
      {"fillet", "Fillet edge",
       "Rounds edges of a basic shape such as a box, cylinder or prism. Click edges to pick them (Shift adds or removes), "
       "set the radius, then Apply. All edges rounds every edge at once."},
      {"fillet_surface", "Fillet surface",
       "Click a flat face. Every outline on that face, the outer edge and each hole, gets its own fillet object, so you can delete "
       "the ones you do not want.<br><br>"
       "<b>Inner</b> rounds an edge off: group the fillet with the solid to cut it. <b>Outer</b> fills an inside corner: group it to add. "
       "<b>Up/Down</b> picks which side of the face it works on. More segments give a smoother curve; 12 is a good default."},
      {"edge_fillet_beta", "Detected Edge Filleting (Beta)",
       "Rounds or chamfers sharp edges of <b>any</b> object: a merged group, an STL, text, an extruded 2D shape. Select one object (a part on "
       "its own or a whole group) and open the tool: its sharp edges are drawn as thin lines. Click an edge to pick it (it turns "
       "orange); click again to drop it. A hole rim or the line where a post stands on a plate is one edge.<br><br>"
       "Outside edges are rounded off; inside edges (where two faces meet in a valley) are filled. Where two picked edges meet "
       "they are mitred, and three square outside edges meeting at a corner get a ball corner. Set the radius, then <b>Apply</b>. "
       "Choose <b>Flat (chamfer)</b> to cut the edges off flat instead: a 1 mm chamfer on a square edge goes 1 mm back along "
       "both faces (45 degrees). A chamfer under an overhanging edge prints without support. "
       "Apply again after changing the picks or the radius; <b>Remove all</b> takes the fillets off.<br><br>"
       "The picks remember where the edge was, so moving or resizing a part of the group a little keeps them. If an edge "
       "changes too much it is reported and left sharp; pick it again. This is a beta: very tight curves or a radius bigger than "
       "the face next to the edge can be skipped with a message."},
      {"extrude_face", "Extrude face",
       "Set the depth, then click a flat face. A new solid grows out of that face. A negative depth grows inward."},
      {"texture", "Texture",
       "<b>Texture</b> puts a pattern on the selected part's surface as real geometry, so it prints: <b>Knurling</b> for "
       "grip, <b>Ribs</b>, <b>Hexagons</b>, <b>Dots</b>, <b>Wood grain</b>, <b>Leather</b>, <b>Carbon weave</b>, or "
       "<b>Picture…</b> to use your own image (bright parts stand out, dark parts stay down).<br><br>"
       "<b>Size</b> is one repeat of the pattern, <b>Depth</b> how far it stands out (or is cut in, with <b>Cut in</b>); "
       "two or three layer heights shows well. <b>Wrap</b>: <b>From every side</b> suits boxy parts, <b>Round the "
       "part</b> wraps it seamlessly round cylinders, handles and knobs. <b>Where</b>: <b>All but the bottom</b> keeps the "
       "part flat on the plate. <b>Turn</b> turns the pattern, and <b>Variation</b> picks another random wood or leather. "
       "<b>Detail</b> sets the triangle size (Automatic follows the pattern).<br><br>"
       "<b>Preview</b> builds it (with a progress bar and Stop); <b>Apply</b> puts it in place of the part or beside it. "
       "Fine textures on large parts make a lot of triangles: the estimate under the settings says how many."},
      {"wrap", "Wrap",
       "<b>Wrap</b> puts text or a logo onto a curved part: a name round a mug or a pen holder, a logo on a ball. Select "
       "the text (or any thin part, such as a 2D shape) and the part to wrap it onto, then click Wrap.<br><br>"
       "<b>Round the part</b> bends it round the part's upright axis without stretching it; set <b>Where round</b> "
       "(0° the front, 90° the right side) and the <b>Height</b> of its middle. <b>Straight on</b> presses it onto "
       "the surface right where it stands: put it in front of (or above) the spot first. Text lying flat is read from "
       "above; standing up, from the side it faces.<br><br>"
       "<b>Raised</b> makes it a part of its own, standing out by the <b>Depth</b> (give it another colour for a "
       "multi-colour print). <b>Cut in</b> engraves it: a hole grouped with the part. Apply asks whether to remove the "
       "flat text."},
      {"articulate", "Articulate",
       "<b>Articulate</b> makes the selected part a print-in-place flexi: it is cut across its length into segments "
       "joined by joints that print in one go and wiggle as soon as they come off the plate. Great for dragons, snakes, "
       "fish and lizards.<br><br>"
       "Each joint is a double cone (45°, so it needs no support) turning in a fork of the next segment, with V-shaped "
       "gaps between the segments so they swing side to side by the <b>Swing</b> you set. <b>Joints</b>, <b>First "
       "joint at</b> and <b>Last joint at</b> say how many and where (keep a big head in one piece by moving the first). "
       "The <b>Joint gap</b> is the gap between moving faces: 0.3 to 0.4 mm suits most printers.<br><br>"
       "Joints need about 9 mm of thickness; thinner spots are reported. Print it flat as it lies, then work each "
       "joint loose."},
      {"hole", "Hole",
       "Turns the selection into a hole (or back into a solid). A hole cuts the solids it is grouped with. Press H, or use the Hole button, "
       "then group the hole with the part to cut.<br><br>"
       "<b>Clearance</b> (in the Move window while a hole is selected) makes the hole cut a little wider than it is drawn, so a "
       "part of the same size fits into it. <b>None</b> cuts it exactly as drawn (the default). <b>Printer clearance</b> uses the "
       "value under Settings &gt; Printer, and follows it when you change it there. <b>Own clearance</b> is a value for this hole "
       "only. <b>Sides only</b> grows it around its own height axis and leaves the depth alone, for a pocket that must stay "
       "exactly as deep as drawn; a hole turned on its side grows around its own axis, which suits a pin. "
       "The size boxes keep showing the size you set; the view shows the hole at the size it cuts. Works on any hole: "
       "shapes, STLs, text, 2D shapes and groups."},
      {"group", "Group and Ungroup",
       "Group (Ctrl+G) joins the selected parts into one: solids merge and holes cut them. Ungroup splits a group back into its parts.<br><br>"
       "<b>Smooth join</b> (in the group's settings) makes the parts flow into each other where they meet, like clay or a "
       "cast part, with a soft blend that many mm wide; holes in the group cut with the same soft edge. Faces away from "
       "the joins stay as they were. Sharp is a plain join."},
      {"merge", "Merge and Unmerge",
       "<b>Merge</b> makes a group (or the selected parts, grouped first) one solid part. A plain group is drawn as its parts "
       "laid over each other; a merged one as its finished shape, so Fillet surface, Detected Edge Filleting, Measure and Lay "
       "flat only see its real outer faces. Its parts are hidden in the Model Tree.<br><br>"
       "Group a merged part with holes and the holes go inside it: it stays one merged part, now cut. "
       "Select it and click Merge again to <b>unmerge</b>: the parts come back exactly as they were, the holes among them. "
       "Exports use the parts, so a STEP file keeps their exact curved surfaces."},
      {"hull", "Hull",
       "<b>Hull</b> shrink-wraps the selected parts into one solid without dents, like a rubber skin stretched around them. "
       "Two spheres become a capsule, a small circle and a large square become a smooth taper, four corner cylinders a "
       "box with rounded corners. One part is enough: it gets filled in to its convex shape.<br><br>"
       "The parts stay inside the hull, like in a group: move or resize one and the hull follows. In the panel, "
       "<b>Chain</b> wraps every part with the next one (in the order you picked them) and joins those, for bent bars "
       "and rounded paths; <b>Join</b> makes it a plain group. Holes inside a hull cut it. Ungroup gives the parts back."},
      {"intersect", "Intersect and part colours",
       "<b>Intersect</b> groups the selected parts and keeps only what they all have in common: a cube and a sphere make a "
       "cube with rounded corners, two crossing cylinders the shape where they overlap. As in any group, the parts stay "
       "inside: move one and the result follows, holes inside cut it, and Ungroup gives the parts back. Any group can switch "
       "between Join, Hull, Chain hull and Intersect in its panel.<br><br>"
       "<b>Keep part colours</b> (in a group's panel) shows the group in its parts' colours instead of its own, and exports "
       "it one closed piece per colour: in a 3MF as one model made of those pieces with their colours, in an OBJ with a "
       "material file. The slicer can then give each colour its own filament. Where parts of different colours overlap, the "
       "one higher in the list keeps its colour; what no part covers (a hull's skin) takes the group's own colour."},
      {"hollow", "Hollow",
       "<b>Hollow</b> turns the selected part or group into a shell with walls of one thickness all round, measured at its "
       "real size, so a stretched part keeps even walls. Tick the sides to leave open, in the part's own directions: the top "
       "for a cup or a box, top and bottom for a sleeve. Open sides that meet are cut together, with no strip of wall left "
       "along their edge. <b>Make solid</b> fills it in again.<br><br>"
       "The inside follows the outside exactly on flat walls and to within 2 % on round ones. Hollowing a detailed part (text, "
       "threads, an STL) takes a while; it is worked out once and kept. A wall as thick as the part leaves it solid."},
      {"split", "Split",
       "<b>Split</b> cuts the selected part or group in two with a plane: level (across Z) or upright (across X or Y) at the "
       "height or position you type, or along the face you set the <b>Workplane</b> on, slanted ones too. The halves are "
       "moved apart by the distance you set, so you can see and print them.<br><br>"
       "Each half is a group of the whole part and a large cutter, so nothing is lost: ungroup a half to move the cutter "
       "and change the cut, or to get the part back whole.<br><br>"
       "<b>Connectors</b> make the halves go back together lined up, for big prints split to fit the plate: "
       "<b>Pins</b> drills matching holes in both halves and puts loose pins beside the part to print and glue in; "
       "<b>Pegs on one half</b> grows pegs on part 1 with holes for them in part 2; <b>Dovetail</b> gives part 1 a key "
       "that slides into a slot in part 2 from the side. They are placed on the cut face where there is room, sized "
       "to it (or to the Size you set), and fit with the printer clearance from Settings."},
      {"overhangs", "Overhangs",
       "<b>Overhangs</b> stripes red every face that hangs over more steeply than your printer manages without support "
       "(45 degrees from upright unless you set another angle under Settings &gt; Printer). Faces lying on the plate are left "
       "alone. Turn parts (Lay flat), add a chamfer under a ledge, or plan supports where it shows red. The overhang test "
       "generator finds your printer's angle."},
      {"repeat", "Repeat",
       "Makes copies of the selection. Each copy moves, turns and resizes by the same amount from the one before it, so a row, a ring "
       "or a spiral takes one step. Resize 1 means no change."},
      {"copy", "Copy, Delete and Drop",
       "<b>Copy</b> makes a copy beside the original; Ctrl+D copies in place. <b>Delete</b> removes the selection; Esc deselects it. "
       "<b>Drop</b> lowers the selected part straight down onto the plate, through anything in the way: use it to put a "
       "hole down into the part it has to cut.<br><br>"
       "<b>Drop onto part</b> lowers it until it touches the first part under it, exactly (a part sunk into it comes up "
       "to rest on top). Parts that touch exactly merge when grouped, so Drop onto part then Group is the way to stack "
       "parts for a weld or an inside fillet."},
      {"text", "Text",
       "Add Text from the shape library. Type in the Text box; pick any font installed on this computer and one of its styles.<br><br>"
       "<b>Font size</b> is the height of a capital letter in millimetres. <b>Height Z</b> is how thick the letters are. "
       "<b>Smoothness</b> sets how closely curves follow the font, from Draft (0.05 mm) to Ultra (0.001 mm)."},
      {"2d_shape", "2D shape editor",
       "Draw a flat outline and save it as an SVG; place it on the bed from the 2D Shapes library and it becomes a solid you can size "
       "like any other part. Placed copies keep their own version: after editing a shape the app asks whether to update them."},
      {"sculpt", "Sculpt",
       "Push and pull a shape's surface with brushes, like Blender's Sculpt Mode. Click <b>Sculpt</b> with nothing selected to "
       "start from a ball, cube, cylinder, slab or capsule; with one part selected it sculpts that part (a group or a part with "
       "holes becomes one surface first).<br><br>"
       "<b>Add and remove:</b> <b>Draw</b> raises hills, <b>Clay</b> builds up in flat layers, <b>Layer</b> raises an even "
       "plateau of a set height, <b>Inflate</b> puffs out, <b>Blob</b> pushes out round blobs, <b>Crease</b> cuts sharp "
       "grooves. <b>Smooth and flatten:</b> <b>Smooth</b> irons bumps away, <b>Flatten</b> presses flat, <b>Fill</b> fills "
       "dips, <b>Scrape</b> shaves peaks. <b>Move and shape:</b> <b>Grab</b> pulls a piece along with the mouse, "
       "<b>Snake hook</b> draws out horns and tentacles, <b>Pinch</b> pulls into a sharp crease, <b>Nudge</b> pushes the "
       "surface along the stroke, <b>Rotate</b> twists a spot. <b>Mask</b> paints over what to protect (it turns dark grey "
       "and every other brush leaves it alone; Clear mask and Invert mask are beside it). <b>Stamp</b> presses a picture "
       "into the surface or raises it: rivets, rings, stars, hearts, scales, knurling, bricks, or a picture of your own "
       "(a logo, a texture); click for one, drag for a row.<br><br>"
       "Hold <b>Ctrl</b> for the other way (dig a hole, carve a groove) and <b>Shift</b> to smooth with any brush. "
       "<b>[</b> and <b>]</b> change the size; <b>1</b>&ndash;<b>0</b> and <b>G K P N R</b> pick a brush. <b>Mirror</b> "
       "X/Y/Z repeats every stroke on the other side; the mirror plane shows in the view (X red, Y green, Z blue) with "
       "the line where it cuts the shape. <b>Radial</b> repeats every stroke 2 to 16 times around an axis, for knobs, "
       "flowers and rosettes; its axis and slices show in the view. Brushes only touch the side facing you, so the back of a thin wall stays "
       "put.<br><br>"
       "The shape always stays a closed, printable solid: a stroke that would push the surface through itself is held back. "
       "<b>Remesh</b> rebuilds the surface as even triangles at the <b>Detail</b> you set (smaller holds finer detail); use it "
       "after pulling far with Grab. <b>Done</b> puts the result in the workspace (for a selected part: in place of it, or "
       "as a new part beside it); double-click a sculpted part to sculpt it again.<br><br>"
       "<b>Save</b> keeps the sculpt in the <b>Sculpts</b> library (with a picture of it), to use or work on later, apart "
       "from any project. In the Sculpts tab, click or drag one onto the bed to place a copy, double-click it to keep "
       "sculpting it (Save then writes back to it), and right-click to rename or delete it."},
      {"lattice", "Lattice",
       "<b>Lattice</b> turns the selected part (or group, or sculpt) into a light, open structure that prints well. "
       "<b>Voronoi skin</b>: the outer shell, cut into an organic web of cells, hollow inside. <b>Gyroid</b>: one smooth, "
       "wavy wall fills the part; strong and needs no supports. <b>Honeycomb</b>: upright hexagon cells. <b>Voronoi "
       "lattice</b>: a 3D web of struts, like coral. <b>Sponge</b>: a random, grown-looking network of smooth struts "
       "and holes, like coral, pumice or bone; walls near half the cell size make it dense, thinner walls more open. "
       "<b>Sponge walls</b>: thin, wavy random walls with big open pores, for any cell size; light, with few triangles. "
       "<b>Bubbles</b>: a solid with round air bubbles of mixed sizes blown in, like pumice or foamed glass; the cell "
       "size is the bubble size."
       "<br><br>"
       "<b>Cell size</b> sets how open it is, <b>Walls</b> how thick the walls or struts are (two nozzle widths or more "
       "prints best), <b>Solid skin</b> closes the outside with a skin of that thickness (for a Voronoi skin it is the "
       "shell), and <b>Pattern</b> picks another random Voronoi, sponge or bubble pattern. "
       "<b>Detail</b> sets the triangle size: a larger detail makes far fewer triangles (half as many at 1.4 times "
       "the detail). <b>Preview</b> shows it, with how much of the "
       "solid part's weight it keeps; <b>Apply</b> puts it in place of the part or beside it. Loose crumbs cut off at the "
       "surface are left out, so everything that comes out prints as one piece."},
      {"mold", "Mold",
       "<b>Mold</b> makes a two-piece casting mould of the selected part (or group, or sculpt), for resin, silicone, "
       "plaster, wax or chocolate. The part's shape is hollowed out of a block <b>Walls</b> bigger than it all round, "
       "and the block is split in two. <b>Split</b> picks which way the halves come apart, <b>Split at</b> where they "
       "meet (Widest slice finds the place the part comes out of both halves best; a part that is wider further in, "
       "like a mushroom, cannot come out of a rigid mould).<br><br>"
       "A <b>Pour hole</b> with a funnel runs from the part's highest point up through the top, <b>Air vents</b> let the "
       "air out of other high points, and round <b>Keys</b> on the parting face (bumps on one half, sockets with the "
       "<b>Key gap</b> on the other) line the halves up. <b>Print</b> lays both halves face up, so the cavities open "
       "upward like bowls and need no supports; <b>Closed</b> shows the mould put together. The line under the "
       "settings says how much the part holds, so you know how much to mix.<br><br>"
       "For silicone or resin, a mould printed in PETG sanded smooth (or sealed with a coat of varnish) releases best; "
       "a little release agent helps."},
      {"simplify", "Simplify",
       "<b>Simplify</b> gives the selected part far fewer triangles for the same shape, so a heavy sculpt or a "
       "detailed STL is quicker to move, group, cut and export. <b>Keep within</b> is how far the surface may move: "
       "0.02 mm is far below what a printer shows, and often leaves a tenth of the triangles or fewer. Flat and gently "
       "curved areas lose the most; sharp edges and fine detail stay.<br><br><b>Preview</b> shows the result and how "
       "many triangles are left; <b>Apply</b> puts it in place of the part or beside it."},
      {"restyle", "Restyle",
       "<b>Restyle</b> gives the selected part (or group) a new look. <b>Low-poly</b> rebuilds it from big flat facets, "
       "like a cut gem or a papercraft model: <b>Facet size</b> sets how big they are, <b>Roughness</b> nudges the "
       "corners about for a hand-cut look and <b>Variation</b> picks another nudge. <b>Voxel</b> builds it from cubes, "
       "like pixel art in 3D: <b>Cube size</b> sets how big. Every voxel face is flat and every overhang a square step, "
       "so it prints cleanly.<br><br><b>Preview</b> shows it; <b>Apply</b> puts it in place of the part or beside it."},
      {"2d_draw", "2D: Draw",
       "In <b>Draw</b> mode, clicks draw with the shape picked in the list. <b>Freehand</b> places nodes one by one; click the first node "
       "to close the loop, and drag a node's round handles to curve a side.<br><br>"
       "The other shapes take two clicks: <b>Corner to corner</b>, <b>From centre</b>, or <b>Side to side</b> (for a circle, its left "
       "and right edge). Shift keeps it square, or exactly level. New outlines are shapes; use Make hole to cut one out."},
      {"2d_select", "2D: Select",
       "In <b>Select</b> mode, click a node to move it, or click inside a shape to move the whole shape. Arrow keys nudge by one grid "
       "step (Shift: ten), Delete removes, Ctrl+D duplicates.<br><br>"
       "The Selection panel shows the exact centre and size; type values there. A circle's width is its diameter. "
       "<b>Make hole</b> / <b>Make shape</b> switches a shape between solid and cut-out. Ctrl+Z and Ctrl+Y undo and redo."},
      {"2d_measure", "2D: Measure",
       "Press Measure and click two points. Nodes and the centres of shapes and holes snap, on every layer, so measuring between two "
       "screw holes is two clicks. The distance and the X and Y difference show on the drawing. Esc clears."},
      {"2d_layers", "2D: Layers",
       "A hole only cuts the shapes on its own layer. To keep a shape inside a hole, put it on another layer: select it and press "
       "<b>To new layer</b>. Merge shows layers together and still keeps their cuts apart; Unmerge splits them again. "
       "A shape with several layers is placed as one part per layer."},
      {"2d_levels", "2D: Levels",
       "Levels stack: level 1 stands on the plate and each next level sits on top of the one below, for example a floor with bolt "
       "holes on level 1 and the walls and tubes on level 2. Press <b>New level</b>, set its <b>Thickness</b> (1 mm by default), "
       "and draw. The level below shows faded, and its nodes and centres snap, so a tube ring lands exactly around a hole. "
       "<b>Copy up a level</b> puts the selected outline on the level above, on the same spot. A hole with <b>Through all levels</b> "
       "cuts every level, so a bolt hole only has to be drawn once.<br><br>"
       "A shape with levels is placed as a group with one part per level, already stacked. Change a level's height in 3D like any part "
       "and move the parts above with the move tool. Layers still work inside each level."},
      {"updates", "Updates and your files",
       "<b>Installed</b> (with the setup.exe): DaedalCAD lives in your user folder and keeps your projects, parts, 2D shapes and "
       "imported files in <b>Documents\\DaedalCAD</b>. Installing, updating or uninstalling never touches that folder. On the first "
       "start it offers to copy the files of an earlier unzipped DaedalCAD there.<br><br>"
       "<b>Portable</b> (unzipped): everything stays in the appdata folder next to DaedalCAD.exe, as before.<br><br>"
       "<b>Updates:</b> a few seconds after start-up DaedalCAD looks for a newer version (File → Check for updates does it right "
       "away) in DaedalCAD's releases on GitHub. An installed copy downloads the new installer, checks it, offers to save your "
       "project, closes, installs and starts again. A portable copy opens the release page instead. Skip this version keeps it "
       "quiet until the next one."},
      {"generators", "Generators",
       "The <b>Generators</b> tab in the library makes parts from a few settings: a box with a lid that fits, Gridfinity bins and "
       "baseplates, a screw jar, an electronics enclosure, a battery holder, bolts and nuts, standoffs for circuit boards, gears, "
       "a rack and pinion, a print-in-place hinge, a ball joint, pulleys, bearing housings, shaft couplers and collars, springs, "
       "a snap-fit, a living hinge, a cable chain, tube connectors, a hose adapter, a pattern panel (holes "
       "as hexagons, circles, squares, diamonds, triangles, stars or slots, in a square or round plate) and a pattern tube (the same "
       "holes all the way round a straight or tapered tube: a vase, a pot, a lamp shade). Both can make <b>holes only</b>: just the "
       "holes, as a hole shape, to group with a part and cut the pattern into it (the tube's holes wrap round a round part sized "
       "like it). <b>Workshop</b> has bit "
       "and tool holders, drawer dividers, cable clips, grommets and labels, and name tags. <b>Testing and calibration</b> has the "
       "fit test, a hole size test, a thread fit test and an overhang and bridge test. Drag one onto the bed or click it.<br><br>"
       "Select a placed generator with the Move tool and its settings show in the tool window; the part rebuilds when you change one "
       "and keeps standing where it was. <b>Clearance</b> is the gap between parts that go together. 0.2 mm suits most printers; "
       "print the <b>Fit test</b> first, find the tightest hole the peg slides into (count the dots) and use that clearance.<br><br>"
       "Parts that print separately (a box and its lid, a jar and its lid, a bolt and its nut) are laid out side by side, lids "
       "upside down, ready for the printer. <b>Assembled</b> shows them put together to check the fit. The hinge prints as one "
       "piece that moves straight off the plate, and so does the cable chain. A V-belt pulley prints as two halves split in "
       "the groove; glue or bolt them together. Text on labels, name tags and the drill index uses the system font.<br><br>"
       "<b>O-ring seals</b> (box: Lid = Sealed; enclosure and jar: O-ring seal) make them watertight: a groove in the top of the "
       "walls for an O-ring or cord, and a tongue on the lid that squeezes it by the Squeeze you set (25 % by default). The walls "
       "grow to fit the groove if they are too thin. Box and enclosure screws sit outside the seal, so no screw hole crosses it: "
       "in ears standing out from the walls, or (Screws sit: In the walls) inside the wall at the corners, with the groove "
       "curving inward round each screw and the inside corners filled, so the outside stays a plain box. Long sides get extra "
       "screws so the lid presses evenly. The jar's lid presses the ring as you screw it "
       "on. No cord at hand? Turn on the gasket and print it in TPU.<br><br>"
       "<b>Use printer clearance</b> on the box, enclosure, ball joint, jar, bolt, nut and the mechanical parts takes the clearance from Settings &gt; Printer (on for "
       "boxes, off for threads, which often like a little more play); switch it off to set one of their own. "
       "Scaling a generator scales its clearances and threads too, so change the size in its settings instead. "
       "<b>Convert to parts</b> turns it into plain parts (one per piece, in a group) that every other tool can work on; its "
       "settings are gone after that. <b>Defaults</b> puts every setting back.<br><br>"
       "<b>Standard parts</b> are real ISO and DIN fasteners at their nominal sizes, M2 to M24: screws (socket head ISO 4762 / "
       "DIN 912, button head ISO 7380, countersunk ISO 10642 and DIN 7991, hex head ISO 4017 and DIN 933, pan head ISO 7045, set "
       "screws ISO 4026), nuts (ISO 4032, DIN 934, thin ISO 4035, nylon locking DIN 985, flange DIN 6923, square DIN 562), "
       "washers (ISO 7089 / DIN 125, ISO 7093, spring DIN 127), dowel pins, threaded rod and heat-set inserts. If a standard "
       "has no part of the size you pick, the nearest one is used and the note says so. Threads are plain by default, which "
       "keeps big assemblies quick; pick Modelled for the real thread.<br><br>"
       "Every standard part can also be its <b>matching hole</b> (Make: Its matching hole): a clearance hole to ISO 273 (fine, "
       "medium or coarse) with a counterbore or countersink so the head sits flush, a tapped hole for set screws, a nut trap, "
       "a washer recess, a pin hole or an insert pocket with a lead-in. The part and its hole share one origin (the underside "
       "of a screw head, the top of a countersunk screw, the bottom of a nut or washer, the top of an insert), so switching "
       "between them leaves it in place. Put the fastener where it goes (for a flush head, sunk until the head's top is level "
       "with the surface) and press <b>Add matching hole</b>: the hole appears "
       "lined up with it and is grouped with the part it sits in, so it cuts that part and not the fastener."},
      {"snap_grid", "Snap and grid",
       "Snap and Grid sit at the bottom right, next to Focus. Snap pulls a move onto a step; the millimetre box next to Snap is that "
       "step. Grid draws the 1 mm lines, with major lines every 10 mm. Focus shows the point the camera turns and zooms around."},
      {"libraries", "Libraries",
       "Drag a shape, an STL part, a 2D shape or a generator onto the bed, or click it. The built-in Parts and 2D shapes folders cannot be renamed "
       "or removed; folders you add yourself can."},
      {"cloudsync", "CloudSync",
       "Personal: your own backups. Put sends a project, Fetch downloads one; unchanged projects are skipped, and if both copies differ "
       "DaedalCAD asks first. Delete from server removes the server copy only.<br><br>"
       "Public: projects everyone on the server can see, with author, version, tags and a picture. Copy project locally gives you a plain "
       "copy. Copy project for update gives you a linked copy: change it, save, then press Send update request with a short message. "
       "The author sees it under Update requests (and in a popup when DaedalCAD starts) and accepts or declines it; an accepted request "
       "becomes the next version. As the author, use Publish a local project, Publish new version, Edit details and Delete from public.<br><br>"
       "Nothing is sent until you press a button. Turn off \"Enable CloudSync\" to stop the start-up check."},
  };
  const QString folder = resourcePath(QStringLiteral("help"));

  QDialog help(this);
  help.setWindowTitle(QStringLiteral("DaedalCAD Help"));
  help.resize(1020, 700);
  auto* layout = new QHBoxLayout(&help);
  auto* pages = new QListWidget(&help);
  pages->setFixedWidth(190);
  for (const Topic& topic : topics) pages->addItem(QString::fromUtf8(topic.title));
  auto* scroll = new QScrollArea(&help);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* page = new QWidget(scroll);
  auto* pageLayout = new QVBoxLayout(page);
  pageLayout->setContentsMargins(14, 8, 14, 8);
  auto* title = new QLabel(page);
  title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600; color: #e87400;"));
  auto* picture = new QLabel(page);
  picture->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  auto* body = new QLabel(page);
  body->setWordWrap(true);
  body->setTextFormat(Qt::RichText);
  body->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  body->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto* note = new QLabel(page);
  note->setStyleSheet(QStringLiteral("color: #8a8074; font-size: 11px;"));
  note->setTextInteractionFlags(Qt::TextSelectableByMouse);
  pageLayout->addWidget(title);
  pageLayout->addWidget(picture);
  pageLayout->addWidget(body);
  pageLayout->addStretch(1);
  pageLayout->addWidget(note);
  scroll->setWidget(page);
  auto show = [&, folder](int row) {
    if (row < 0 || row >= static_cast<int>(std::size(topics))) return;
    const Topic& topic = topics[row];
    const QString key = QString::fromUtf8(topic.key);
    title->setText(QString::fromUtf8(topic.title));
    body->setText(QString::fromUtf8(topic.text));
    QPixmap image;
    for (const QString& ext : {QStringLiteral(".png"), QStringLiteral(".jpg"), QStringLiteral(".jpeg")}) {
      const QString path = QDir(folder).absoluteFilePath(QStringLiteral("help_") + key + ext);
      if (QFileInfo::exists(path) && image.load(path)) break;
    }
    if (!image.isNull()) {
      // Fit the page width (the scroll area is not laid out yet the first time a page shows).
      const int maxWidth = std::max(320, help.width() - pages->width() - 90);
      const qreal ratio = help.devicePixelRatioF();
      // Fit the width, and keep the picture short enough that the text below stays in view.
      const QSize box(static_cast<int>(maxWidth * ratio), static_cast<int>(430 * ratio));
      QPixmap shown = image.width() > box.width() || image.height() > box.height()
                          ? image.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                          : image;
      shown.setDevicePixelRatio(ratio);
      picture->setPixmap(shown);
      picture->show();
      note->clear();
    } else {
      picture->clear();
      picture->hide();
      note->setText(QStringLiteral("Picture for this page: appdata/help/help_%1.png").arg(key));
    }
  };
  connect(pages, &QListWidget::currentRowChanged, &help, show);
  layout->addWidget(pages);
  layout->addWidget(scroll, 1);
  pages->setCurrentRow(0);
  help.exec();
}

void MainWindow::showAlignTool() {
  if (!alignPopup) return;
  if (alignPopup->isVisible()) {
    alignPopup->hide();
    syncActions();
    loadInspector();
    return;
  }
  if (repeatPopup) repeatPopup->hide();
  if (filletPopup) filletPopup->hide();
  if (faceFilletPopup) faceFilletPopup->hide();
  if (edgeBetaPopup) edgeBetaPopup->hide();
  if (tool == 3 || tool == 4 || tool == 6 || tool == 7 || tool == 8 || tool == 9 || tool == 12 || tool == 13) {
    tool = 0;
    panel = 0;
    if (view) view->setTool(0);
    filletTarget.clear();
  }
  alignPopup->show();
  syncActions();
  loadInspector();
  alignAnchorId.clear();
  updateAlignOverlay();
  status(selection.size() < 2 ? QStringLiteral("Select two or more objects to align.")
                              : QStringLiteral("Click a dot to line the parts up. Click a part first to keep it in place."));
}

// The parts the Align tool works on: selected nodes whose parent is not also selected.
std::vector<MainWindow::AlignEntry> MainWindow::alignEntries() const {
  std::vector<AlignEntry> entries;
  for (const QString& id : selection) {
    const Node* node = doc.find(id);
    if (!node) continue;
    bool parentSelected = false;
    const Node* walk = node;
    int guard = 0;
    while (walk && !walk->parentId.isEmpty() && guard++ < 64) {
      if (selection.contains(walk->parentId)) {
        parentSelected = true;
        break;
      }
      walk = doc.find(walk->parentId);
    }
    if (parentSelected) continue;
    QVector3D mn;
    QVector3D mx;
    if (!alignmentWorldBounds(doc, *node, &mn, &mx)) continue;
    entries.push_back({node->id, mn, mx});
  }
  return entries;
}

// The aligned part a clicked id belongs to (it may be a child inside a selected group).
QString MainWindow::alignRootFor(const QString& id) const {
  for (const AlignEntry& entry : alignEntries()) {
    const Node* walk = doc.find(id);
    int guard = 0;
    while (walk && guard++ < 64) {
      if (walk->id == entry.id) return entry.id;
      if (walk->parentId.isEmpty()) break;
      walk = doc.find(walk->parentId);
    }
  }
  return {};
}

void MainWindow::updateAlignOverlay() {
  if (!view) return;
  if (!alignPopup || !alignPopup->isVisible() || selection.size() < 2) {
    view->setAlignBoxes({}, {});
    return;
  }
  const std::vector<AlignEntry> entries = alignEntries();
  bool anchorPresent = false;
  std::vector<Viewport::AlignBox> boxes;
  for (const AlignEntry& entry : entries) {
    boxes.push_back({entry.id, entry.lo, entry.hi});
    if (entry.id == alignAnchorId) anchorPresent = true;
  }
  if (!anchorPresent) alignAnchorId.clear();
  view->setAlignBoxes(std::move(boxes), alignAnchorId);
}

void MainWindow::alignSelection(int axis, int mode) {
  if (selection.size() < 2) {
    status(QStringLiteral("Select two or more objects to align."));
    return;
  }
  axis = std::clamp(axis, 0, 2);
  mode = std::clamp(mode, 0, 2);
  const std::vector<AlignEntry> entries = alignEntries();
  if (entries.size() < 2) {
    status(QStringLiteral("Select two independent objects to align."));
    return;
  }
  auto component = [](const QVector3D& p, int a) { return a == 0 ? p.x() : a == 1 ? p.y() : p.z(); };
  auto valueOf = [&](const QVector3D& lo, const QVector3D& hi) {
    return mode == 0 ? component(lo, axis) : mode == 2 ? component(hi, axis) : (component(lo, axis) + component(hi, axis)) * 0.5f;
  };
  // Line up with the part that stays, or with the selection as a whole when none was picked.
  const AlignEntry* anchor = nullptr;
  for (const AlignEntry& entry : entries)
    if (!alignAnchorId.isEmpty() && entry.id == alignAnchorId) anchor = &entry;
  QVector3D lo = entries.front().lo;
  QVector3D hi = entries.front().hi;
  for (const AlignEntry& entry : entries) {
    lo = QVector3D(std::min(lo.x(), entry.lo.x()), std::min(lo.y(), entry.lo.y()), std::min(lo.z(), entry.lo.z()));
    hi = QVector3D(std::max(hi.x(), entry.hi.x()), std::max(hi.y(), entry.hi.y()), std::max(hi.z(), entry.hi.z()));
  }
  const float target = anchor ? valueOf(anchor->lo, anchor->hi) : valueOf(lo, hi);
  pushUndo();
  int moved = 0;
  for (const AlignEntry& entry : entries) {
    if (anchor && entry.id == anchor->id) continue;
    const float current = valueOf(entry.lo, entry.hi);
    if (std::abs(target - current) < 1e-6f) continue;
    QVector3D delta(0, 0, 0);
    if (axis == 0) delta.setX(target - current);
    else if (axis == 1) delta.setY(target - current);
    else delta.setZ(target - current);
    Node* node = doc.find(entry.id);
    if (!node) continue;
    const QMatrix4x4 parentWorld = alignmentParentWorldMatrix(doc, *node);
    bool invertible = true;
    const QMatrix4x4 inv = parentWorld.inverted(&invertible);
    const QVector3D localDelta = invertible ? inv.mapVector(delta) : delta;
    node->pos[0] += localDelta.x();
    node->pos[1] += localDelta.y();
    node->pos[2] += localDelta.z();
    tidyPlacement(*node);
    ++moved;
  }
  refreshScene(true);
  loadInspector();
  static const char* sides[3] = {"low side", "centre", "high side"};
  const QString axisName = axis == 0 ? QStringLiteral("X") : axis == 1 ? QStringLiteral("Y") : QStringLiteral("Z");
  const Node* kept = anchor ? doc.find(anchor->id) : nullptr;
  status(kept ? QStringLiteral("Lined up %1 object%2 with %3 on %4 (%5).")
                    .arg(moved)
                    .arg(moved == 1 ? QString() : QStringLiteral("s"))
                    .arg(kept->name)
                    .arg(axisName)
                    .arg(QLatin1String(sides[mode]))
              : QStringLiteral("Lined up %1 object%2 on %3 (%4) of the selection.")
                    .arg(moved)
                    .arg(moved == 1 ? QString() : QStringLiteral("s"))
                    .arg(axisName)
                    .arg(QLatin1String(sides[mode])));
}

void MainWindow::openSettings() {
  const QString defaultParts = defaultAppdataPath(QStringLiteral("parts"));
  const QString defaultShapes = defaultAppdataPath(QStringLiteral("2d_shapes"));
  QDir().mkpath(defaultParts);
  QDir().mkpath(defaultShapes);
  const double savedWorkplane = doc.workplaneZ;
  const double savedSnap = doc.snap;
  const double savedBedX = doc.bedX;
  const double savedBedY = doc.bedY;
  LibrarySettingsDialog dialog(this, partsDirectories, partsDirectoryNames, twoDShapesDirectories, twoDShapesDirectoryNames, defaultParts, defaultShapes, platePage);
  QDoubleSpinBox* clearanceSpin = nullptr;
  QDoubleSpinBox* overhangSpin = nullptr;
  if (auto* tabs = dialog.findChild<QTabWidget*>()) {
    auto* printerTab = new QWidget(tabs);
    auto* printerLayout = new QVBoxLayout(printerTab);
    auto* form = new QFormLayout;
    clearanceSpin = new QDoubleSpinBox(printerTab);
    clearanceSpin->setRange(0, 2);
    clearanceSpin->setDecimals(2);
    clearanceSpin->setSingleStep(0.01);
    clearanceSpin->setSuffix(QStringLiteral(" mm"));
    clearanceSpin->setValue(printerClearance());
    form->addRow(QStringLiteral("Printer clearance"), clearanceSpin);
    overhangSpin = new QDoubleSpinBox(printerTab);
    overhangSpin->setRange(10, 80);
    overhangSpin->setDecimals(0);
    overhangSpin->setSuffix(QStringLiteral("°"));
    overhangSpin->setValue(QSettings().value(QStringLiteral("overhangAngle"), 45.0).toDouble());
    overhangSpin->setToolTip(QStringLiteral("How far from upright a wall can lean out before it needs support. 45° suits most "
                                            "printers; print the Overhang test generator to find yours."));
    form->addRow(QStringLiteral("Overhang angle"), overhangSpin);
    printerLayout->addLayout(form);
    auto* explain = new QLabel(QStringLiteral(
        "The gap your printer needs between two parts that fit together, on each side. Print the Fit test generator "
        "and use the clearance of the tightest hole the peg slides into.<br><br>"
        "Holes set to <b>Printer clearance</b> cut this much wider, and generators with <b>Use printer clearance</b> "
        "on use it for their lids and threads. Change it here and they all follow; holes and generators with a "
        "clearance of their own keep theirs."), printerTab);
    explain->setWordWrap(true);
    explain->setTextFormat(Qt::RichText);
    printerLayout->addWidget(explain);
    printerLayout->addStretch(1);
    tabs->addTab(printerTab, QStringLiteral("Printer"));
  }
  QComboBox* smoothBox = nullptr;
  if (auto* tabs = dialog.findChild<QTabWidget*>()) {
    auto* displayTab = new QWidget(tabs);
    auto* displayLayout = new QVBoxLayout(displayTab);
    auto* form = new QFormLayout;
    smoothBox = new QComboBox(displayTab);
    smoothBox->addItem(QStringLiteral("Off"), 0);
    smoothBox->addItem(QStringLiteral("2× (light)"), 2);
    smoothBox->addItem(QStringLiteral("4× (default)"), 4);
    smoothBox->addItem(QStringLiteral("8× (smoothest)"), 8);
    smoothBox->setCurrentIndex(std::max(0, smoothBox->findData(smoothingSamples())));
    smoothBox->setToolTip(QStringLiteral("Anti-aliasing: smooth edges on parts and lines in the 3D views."));
    form->addRow(QStringLiteral("Smooth edges"), smoothBox);
    displayLayout->addLayout(form);
    auto* explain = new QLabel(QStringLiteral(
        "Smooth edges (anti-aliasing) draws each pixel from several samples, so edges and lines in the 3D view and the "
        "sculpt window have no stairs. More samples look smoother and ask more of the graphics card: turn it down or "
        "off if the view feels slow on an older or built-in GPU.<br><br>"
        "<b>The main view picks up a change the next time DaedalCAD starts</b>; sculpt windows opened after the "
        "change use it straight away."), displayTab);
    explain->setWordWrap(true);
    explain->setTextFormat(Qt::RichText);
    displayLayout->addWidget(explain);
    displayLayout->addStretch(1);
    tabs->addTab(displayTab, QStringLiteral("Display"));
  }
  std::function<void()> commitKeys;
  if (auto* tabs = dialog.findChild<QTabWidget*>()) tabs->addTab(buildKeysPage(tabs, &commitKeys), QStringLiteral("Keyboard"));
  const int result = dialog.exec();
  if (result == QDialog::Accepted && commitKeys) commitKeys();
  if (result == QDialog::Accepted && smoothBox) {
    const int samples = smoothBox->currentData().toInt();
    if (samples != smoothingSamples()) {
      QSettings().setValue(QStringLiteral("display/smoothSamples"), samples);
      status(QStringLiteral("Smooth edges set to %1: the main view picks it up the next time DaedalCAD starts.")
                 .arg(samples ? QStringLiteral("%1×").arg(samples) : QStringLiteral("off")));
    }
  }
  if (platePage) {
    platePage->setParent(this);
    platePage->hide();
  }
  if (result != QDialog::Accepted) {
    const bool plateChanged = doc.workplaneZ != savedWorkplane || doc.snap != savedSnap || doc.bedX != savedBedX ||
                              doc.bedY != savedBedY;
    if (plateChanged) {
      doc.workplaneZ = savedWorkplane;
      doc.snap = savedSnap;
      doc.bedX = savedBedX;
      doc.bedY = savedBedY;
      if (view) {
        view->setBed(doc.bedX, doc.bedY);
        view->setSnap(doc.snapOn, doc.snap);
      }
      loadInspector();
      refreshScene();
    }
    return;
  }
  partsDirectories = dialog.partsDirectories();
  partsDirectoryNames = dialog.partsDirectoryNames();
  twoDShapesDirectories = dialog.twoDShapesDirectories();
  twoDShapesDirectoryNames = dialog.twoDShapesDirectoryNames();
  alignLibraryNames(partsDirectories, partsDirectoryNames, defaultParts, QStringLiteral("Parts"));
  alignLibraryNames(twoDShapesDirectories, twoDShapesDirectoryNames, defaultShapes, QStringLiteral("2D shapes"));
  QSettings settings;
  settings.setValue(QStringLiteral("partsDirectories"), partsDirectories);
  settings.setValue(QStringLiteral("partsDirectoryNames"), partsDirectoryNames);
  settings.setValue(QStringLiteral("twoDShapesDirectories"), twoDShapesDirectories);
  settings.setValue(QStringLiteral("twoDShapesDirectoryNames"), twoDShapesDirectoryNames);
  refreshPartsLibrary();
  refresh2DShapeLibrary();
  if (overhangSpin) {
    settings.setValue(QStringLiteral("overhangAngle"), overhangSpin->value());
    if (view && view->overhangsShown()) view->setOverhangs(true, overhangSpin->value());
  }
  if (clearanceSpin && std::abs(clearanceSpin->value() - printerClearance()) > 1e-9) {
    setPrinterClearance(clearanceSpin->value());
    settings.setValue(QStringLiteral("printerClearance"), printerClearance());
    genFormFor.clear();
    loadInspector();
    refreshScene();
    status(QStringLiteral("Settings saved. Printer clearance is now %1 mm.").arg(printerClearance(), 0, 'f', 2));
    return;
  }
  status(QStringLiteral("Library settings saved."));
}

void MainWindow::refresh2DShapeLibrary() {
  if (!shapes2d) return;
  shapes2d->clear();
  QStringList seenRoots;
  for (int index = 0; index < twoDShapesDirectories.size(); ++index) {
    const QString rootPath = QDir::cleanPath(QFileInfo(twoDShapesDirectories[index]).absoluteFilePath());
    if (rootPath.isEmpty() || seenRoots.contains(rootPath)) continue;
    seenRoots << rootPath;
    QString name = index < twoDShapesDirectoryNames.size() ? twoDShapesDirectoryNames[index].trimmed() : QString();
    if (name.isEmpty()) name = QFileInfo(rootPath).fileName();
    if (name.isEmpty()) name = rootPath;
    auto* header = new QListWidgetItem(QStringLiteral("▾ ") + name, shapes2d);
    header->setData(Qt::UserRole, QStringLiteral("header"));
    header->setData(Qt::UserRole + 1, true);
    header->setData(Qt::UserRole + 2, name);
    header->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    header->setForeground(QColor("#e87400"));
    header->setBackground(QColor("#2a1c10"));
    QFont font = header->font();
    font.setBold(true);
    header->setFont(font);
    header->setToolTip(QDir::toNativeSeparators(rootPath));
    if (!QDir(rootPath).exists()) {
      auto* missing = new QListWidgetItem(QStringLiteral("    Folder not found"), shapes2d);
      missing->setFlags(Qt::NoItemFlags);
      missing->setForeground(QColor("#8a8074"));
      continue;
    }
    int count = 0;
    const QHash<QString, QStringList> rootTags = loadLibraryTags(rootPath);
    QDirIterator it(rootPath, QStringList{QStringLiteral("*.svg"), QStringLiteral("*.SVG")},
                    QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      const QFileInfo info(it.next());
      if (info.fileName().startsWith(QLatin1String("layer-")) || info.fileName().startsWith(QLatin1String("level-"))) continue;
      auto* item = new QListWidgetItem(shapeIcon(QStringLiteral("2d")), QStringLiteral("    ") + info.fileName(), shapes2d);
      item->setData(Qt::UserRole, QStringLiteral("svg:") + info.absoluteFilePath());
      const QString rel = relativeLibraryPath(rootPath, info.absoluteFilePath());
      const QStringList tags = rootTags.value(rel);
      item->setData(Qt::UserRole + 3, tags);
      QString tip = QDir::toNativeSeparators(info.absoluteFilePath());
      if (!tags.isEmpty()) tip += QStringLiteral("\nTags: ") + tags.join(QStringLiteral(", "));
      item->setToolTip(tip);
      ++count;
    }
    if (count == 0) {
      auto* empty = new QListWidgetItem(QStringLiteral("    No files"), shapes2d);
      empty->setFlags(Qt::NoItemFlags);
      empty->setForeground(QColor("#8a8074"));
    }
  }
  const QString importedRoot = QDir::cleanPath(defaultAppdataPath(QStringLiteral("Imported")));
  if (!seenRoots.contains(importedRoot)) {
    auto* header = new QListWidgetItem(QStringLiteral("▾ Imported"), shapes2d);
    header->setData(Qt::UserRole, QStringLiteral("header"));
    header->setData(Qt::UserRole + 1, true);
    header->setData(Qt::UserRole + 2, QStringLiteral("Imported"));
    header->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    header->setForeground(QColor("#e87400"));
    header->setBackground(QColor("#2a1c10"));
    QFont font = header->font();
    font.setBold(true);
    header->setFont(font);
    header->setToolTip(QDir::toNativeSeparators(importedRoot));
    int count = 0;
    if (QDir(importedRoot).exists()) {
      const QHash<QString, QStringList> rootTags = loadLibraryTags(importedRoot);
      QDirIterator it(importedRoot, QStringList{QStringLiteral("*.svg"), QStringLiteral("*.SVG")},
                      QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
      while (it.hasNext()) {
        const QFileInfo info(it.next());
        if (info.fileName().startsWith(QLatin1String("layer-")) || info.fileName().startsWith(QLatin1String("level-"))) continue;
        auto* item = new QListWidgetItem(shapeIcon(QStringLiteral("2d")), QStringLiteral("    ") + info.fileName(), shapes2d);
        item->setData(Qt::UserRole, QStringLiteral("svg:") + info.absoluteFilePath());
        const QStringList tags = rootTags.value(relativeLibraryPath(importedRoot, info.absoluteFilePath()));
        item->setData(Qt::UserRole + 3, tags);
        QString tip = QDir::toNativeSeparators(info.absoluteFilePath());
        if (!tags.isEmpty()) tip += QStringLiteral("\nTags: ") + tags.join(QStringLiteral(", "));
        item->setToolTip(tip);
        ++count;
      }
    }
    if (count == 0) {
      auto* empty = new QListWidgetItem(QStringLiteral("    No files"), shapes2d);
      empty->setFlags(Qt::NoItemFlags);
      empty->setForeground(QColor("#8a8074"));
    }
  }
  // A search typed before the list was rebuilt still applies.
  if (auto* search = findChild<QLineEdit*>(QStringLiteral("shapes2dSearch")); search && !search->text().isEmpty())
    filterLibraryList(shapes2d, search->text());
}

// Placed 2D shapes get their own frozen copy of the SVG. Editing the shape in the library later then
// does not change parts already on the bed; edit2DShape offers to update them instead. Copies are named
// by content, so placing the same version twice shares one file.
QString freezePlaced2D(const QString& source) {
  QFile in(source);
  if (!in.open(QIODevice::ReadOnly)) return {};
  const QByteArray bytes = in.readAll();
  const QString hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex().left(12));
  const QFileInfo info(source);
  QString shapeName = info.absoluteDir().dirName();
  shapeName.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\s]+")), QStringLiteral("_"));
  // Layer files keep their "layer-" prefix: older layer files without a stored centre rely on it.
  // Level files get the shape's name too, like layer files.
  const QString name = info.completeBaseName().startsWith(QLatin1String("layer-")) ||
                               info.completeBaseName().startsWith(QLatin1String("level-"))
                           ? QStringLiteral("%1-%2-%3.svg").arg(info.completeBaseName(), shapeName, hash)
                           : QStringLiteral("%1-%2.svg").arg(info.completeBaseName(), hash);
  const QString folder = defaultAppdataPath(QStringLiteral("placed_2d"));
  if (!QDir().mkpath(folder)) return {};
  const QString dest = QDir(folder).absoluteFilePath(name);
  if (QFileInfo::exists(dest)) return dest;
  QFile out(dest);
  if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()) return {};
  return dest;
}

void MainWindow::create2DShape() {
  const QString folder = defaultAppdataPath(QStringLiteral("2d_shapes"));
  QDir().mkpath(folder);
  Shape2DEditorDialog dialog(this, folder);
  const int result = dialog.exec();
  if (dialog.savedThisSession()) refresh2DShapeLibrary();
  if (result != QDialog::Accepted) return;
  const QString path = dialog.savedPathValue();
  if (path.isEmpty()) return;
  importSvgShape(path, 0, 0);
}

namespace {

// A sculpt as a binary STL (its own facet normals), in a fresh file under the sculpts folder: each
// Done writes a new file, so undo in the workspace can go back to the one before.
QString writeSculptStl(const sculpt::Mesh& mesh, const QString& name) {
  const QString folder = defaultAppdataPath(QStringLiteral("sculpts"));
  QDir().mkpath(folder);
  QString stem = sanitizeProjectName(name).replace(QLatin1Char(' '), QLatin1Char('_'));
  if (stem.isEmpty()) stem = QStringLiteral("sculpt");
  const QString path = QDir(folder).filePath(
      QStringLiteral("%1-%2.stl").arg(stem, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))));
  QSaveFile file(path);
  const std::string bytes = sculpt::stlBytes(mesh);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes.data(), static_cast<qint64>(bytes.size())) != qint64(bytes.size()) ||
      !file.commit())
    return {};
  return path;
}

// The Sculpts library: an STL per sculpt, and a picture of it beside it (same name, .png).
QString sculptLibraryFolder() {
  const QString folder = defaultAppdataPath(QStringLiteral("sculpt_library"));
  QDir().mkpath(folder);
  return folder;
}

bool readSculptFile(const QString& path, sculpt::Mesh* mesh, QString* error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (error) *error = QStringLiteral("Could not open %1.").arg(QDir::toNativeSeparators(path));
    return false;
  }
  const QByteArray data = file.readAll();
  std::string why;
  if (!sculpt::parseStl(std::string(data.constData(), static_cast<size_t>(data.size())), mesh, &why)) {
    if (error) *error = QString::fromStdString(why);
    return false;
  }
  return true;
}

// Where an STL part made from this mesh sits: the middle of its box (the STL loader centres the
// mesh on that), worked out from the coordinates as the file stores them.
sculpt::vec3 sculptCentre(const sculpt::Mesh& mesh) {
  sculpt::vec3 lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300);
  for (const sculpt::vec3& p : mesh.pos) {
    const sculpt::vec3 f(float(p.x), float(p.y), float(p.z));
    lo = manifold::la::min(lo, f);
    hi = manifold::la::max(hi, f);
  }
  return (lo + hi) * 0.5;
}

}  // namespace

void MainWindow::openSculpt() {
  if (selection.size() == 1 && doc.find(selection.front())) {
    sculptPart(selection.front());
    return;
  }
  if (selection.size() > 1) {
    status(QStringLiteral("Select one part to sculpt it, or nothing to start a new sculpt."));
    return;
  }
  newSculpt();
}

void MainWindow::newSculpt() {
  SculptDialog dialog(this);
  dialog.setLibrary(sculptLibraryFolder());
  const bool accepted = dialog.exec() == QDialog::Accepted;
  if (dialog.savedToLibrary()) refreshSculptLibrary();
  if (accepted) addSculptToPlate(dialog.result(), dialog.name());
}

void MainWindow::addSculptToPlate(const sculpt::Mesh& mesh, const QString& name, double x, double y) {
  QString path;
  runBusyOrNow([&] { path = writeSculptStl(mesh, name); }, mesh.tri.size() > 200000);
  if (path.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Sculpt"), QStringLiteral("Could not write the sculpt to disk."));
    return;
  }
  pushUndo();
  Node node;
  node.id = newId();
  node.kind = Node::Kind::Shape;
  node.shape = QStringLiteral("stl");
  node.sculpt = true;
  node.meshPath = path;
  node.name = name.isEmpty() ? QStringLiteral("Sculpt") : name;
  node.color = QColor("#d7c4a3");
  node.pos[0] = doc.snapOn ? std::round(x / doc.snap) * doc.snap : x;
  node.pos[1] = doc.snapOn ? std::round(y / doc.snap) * doc.snap : y;
  node.pos[2] = doc.workplaneZ;
  doc.nodes.push_back(node);
  if (Node* added = doc.find(node.id)) {
    double lo[3];
    double hi[3];
    if (localBounds(doc, *added, lo, hi)) added->pos[2] += doc.workplaneZ - lo[2];
    tidyPlacement(*added);
  }
  standOnWorkplane(node.id, placingDropped ? placingU : 0, placingDropped ? placingV : 0);
  selection = QStringList{node.id};
  view->focusOn(selection);  // the view turns around the new part
  refreshTree();
  loadInspector();
  refreshScene(false);
  status(QStringLiteral("%1 is on the plate. Double-click it to sculpt it again.").arg(node.name));
}

void MainWindow::placeLibrarySculpt(const QString& path, double x, double y) {
  sculpt::Mesh mesh;
  QString error;
  if (!readSculptFile(path, &mesh, &error)) {
    status(error);
    return;
  }
  addSculptToPlate(mesh, QFileInfo(path).completeBaseName(), x, y);
}

void MainWindow::editLibrarySculpt(const QString& path) {
  sculpt::Mesh mesh;
  QString error;
  if (!readSculptFile(path, &mesh, &error)) {
    QMessageBox::warning(this, QStringLiteral("Sculpt"), error);
    return;
  }
  if (!sculpt::closedManifold(mesh)) {
    // Not one closed surface (an STL dropped into the folder from elsewhere): rebuild it first.
    sculpt::Mesh even;
    std::string why;
    sculpt::vec3 lo, hi;
    sculpt::bounds(mesh, &lo, &hi);
    const double detail = std::clamp(std::round(manifold::la::length(hi - lo) / 120 * 20) / 20, 0.15, 3.0);
    bool ok = false;
    runBusy([&] { ok = sculpt::remesh(mesh, detail, &even, &why); }, true);
    if (!ok) {
      QMessageBox::warning(this, QStringLiteral("Sculpt"), QString::fromStdString(why));
      return;
    }
    mesh = std::move(even);
  }
  SculptDialog dialog(this, std::move(mesh), QFileInfo(path).completeBaseName(), false);
  dialog.setLibrary(sculptLibraryFolder(), path);
  const bool accepted = dialog.exec() == QDialog::Accepted;
  if (dialog.savedToLibrary()) refreshSculptLibrary();
  if (accepted) {
    const int n = static_cast<int>(doc.nodes.size());
    addSculptToPlate(dialog.result(), dialog.name(), ((n % 4) - 1.5) * 45.0, (n / 4 % 3) * 45.0);
  }
}

void MainWindow::refreshSculptLibrary() {
  if (!sculptsList) return;
  sculptsList->clear();
  const QString folder = sculptLibraryFolder();
  QDirIterator it(folder, QStringList{QStringLiteral("*.stl"), QStringLiteral("*.STL")}, QDir::Files | QDir::Readable);
  QStringList files;
  while (it.hasNext()) files << it.next();
  std::sort(files.begin(), files.end(), [](const QString& a, const QString& b) {
    return QFileInfo(a).completeBaseName().compare(QFileInfo(b).completeBaseName(), Qt::CaseInsensitive) < 0;
  });
  for (const QString& file : files) {
    const QFileInfo info(file);
    const QString picture = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".png");
    const QIcon icon = QFileInfo::exists(picture) ? QIcon(QPixmap(picture)) : toolIcon(QStringLiteral("Sculpt"));
    auto* item = new QListWidgetItem(icon, QStringLiteral("  ") + info.completeBaseName(), sculptsList);
    item->setData(Qt::UserRole, QStringLiteral("sculpt:") + info.absoluteFilePath());
    item->setToolTip(QStringLiteral("%1\n\nClick or drag onto the bed to place a copy; double-click to keep sculpting it.")
                         .arg(QDir::toNativeSeparators(info.absoluteFilePath())));
  }
  if (files.isEmpty()) {
    auto* empty = new QListWidgetItem(QStringLiteral("    No sculpts yet. In the Sculpt window, Save keeps one here."), sculptsList);
    empty->setFlags(Qt::NoItemFlags);
    empty->setForeground(QColor("#8a8074"));
  }
}

void MainWindow::sculptPart(const QString& id) {
  const Node* node = doc.find(id);
  if (!node) return;
  const bool again = node->kind == Node::Kind::Shape && node->shape == QLatin1String("stl") && node->sculpt;
  // The part as it stands in the world (a hole as a solid), one mesh even if it is a group.
  Document shapeOnly = doc;
  if (Node* self = shapeOnly.find(id)) self->hole = false;
  std::vector<double> verts;
  std::vector<uint64_t> tris;
  bool ok = false;
  runBusy([&] { ok = worldMesh64(shapeOnly, QStringList{id}, &verts, &tris); }, true);
  if (!ok || tris.size() < 12) {
    status(QStringLiteral("%1 has no surface to sculpt.").arg(node->name));
    return;
  }
  sculpt::Mesh mesh;
  mesh.pos.reserve(verts.size() / 3);
  for (size_t i = 0; i + 2 < verts.size(); i += 3) mesh.pos.emplace_back(verts[i], verts[i + 1], verts[i + 2]);
  for (size_t i = 0; i + 2 < tris.size(); i += 3)
    mesh.tri.push_back({static_cast<uint32_t>(tris[i]), static_cast<uint32_t>(tris[i + 1]), static_cast<uint32_t>(tris[i + 2])});
  if (!again) {
    // Even triangles to push around: about 120 along the part's diagonal.
    sculpt::vec3 lo, hi;
    sculpt::bounds(mesh, &lo, &hi);
    const double detail = std::clamp(std::round(manifold::la::length(hi - lo) / 120 * 20) / 20, 0.15, 3.0);
    sculpt::Mesh even;
    std::string error;
    bool done = false;
    status(QStringLiteral("Preparing %1 for sculpting…").arg(node->name));
    runBusy([&] { done = sculpt::remesh(mesh, detail, &even, &error); }, true);
    if (!done) {
      QMessageBox::warning(this, QStringLiteral("Sculpt"), QString::fromStdString(error));
      return;
    }
    mesh = std::move(even);
  } else if (!sculpt::closedManifold(mesh)) {
    // Cut by a hole since, or the like: rebuild it at its own detail.
    sculpt::Mesh even;
    std::string error;
    const double detail = std::max(0.1, sculpt::averageEdge(mesh));
    runBusy([&] { ok = sculpt::remesh(mesh, detail, &even, &error); }, true);
    if (ok) mesh = std::move(even);
  }
  const QString name = node->name;
  SculptDialog dialog(this, std::move(mesh), name, true);
  dialog.setLibrary(sculptLibraryFolder());
  const bool accepted = dialog.exec() == QDialog::Accepted;
  if (dialog.savedToLibrary()) refreshSculptLibrary();
  if (!accepted) return;
  node = doc.find(id);
  if (!node) return;
  const bool replace = dialog.replaceOriginal();
  const QString newName = replace ? name : name + QStringLiteral(" (sculpted)");
  if (placeMeshForPart(id, dialog.result(), replace, newName, true))
    status(replace ? QStringLiteral("%1 is sculpted. Double-click it to sculpt it again.").arg(newName)
                   : QStringLiteral("%1 added beside the original.").arg(newName));
}

QString MainWindow::addMeshNode(sculpt::Mesh mesh, const QString& name, const QString& parentId, const QColor& color, bool hole) {
  if (!parentId.isEmpty()) {
    bool invertible = false;
    const QMatrix4x4 toParent = nodeWorld(doc, parentId).inverted(&invertible);
    if (invertible)
      for (sculpt::vec3& p : mesh.pos) {
        const QVector3D local = toParent.map(QVector3D(float(p.x), float(p.y), float(p.z)));
        p = sculpt::vec3(local.x(), local.y(), local.z());
      }
  }
  QString path;
  runBusyOrNow([&] { path = writeSculptStl(mesh, name); }, mesh.tri.size() > 200000);
  if (path.isEmpty()) return {};
  Node made;
  made.id = newId();
  made.kind = Node::Kind::Shape;
  made.shape = QStringLiteral("stl");
  made.meshPath = path;
  made.name = name;
  made.parentId = parentId;
  made.color = color;
  made.hole = hole;
  const sculpt::vec3 centre = sculptCentre(mesh);
  made.pos[0] = centre.x;
  made.pos[1] = centre.y;
  made.pos[2] = centre.z;
  doc.nodes.push_back(made);
  return made.id;
}

bool MainWindow::placeMeshForPart(const QString& id, sculpt::Mesh result, bool replace, const QString& newName, bool sculptPart) {
  const Node* node = doc.find(id);
  if (!node) return false;
  // Moving and writing out a result of millions of triangles takes a while: under the busy bar, with
  // the window still drawing. (The part is then loaded in the background by refreshScene.)
  const QString parentId = node->parentId;
  bool invertible = false;
  const QMatrix4x4 toParent = parentId.isEmpty() ? QMatrix4x4() : nodeWorld(doc, parentId).inverted(&invertible);
  QString path;
  sculpt::vec3 centre;
  runBusyOrNow([&] {
    if (!replace) {
      // Beside the original, not on top of it.
      sculpt::vec3 lo, hi;
      sculpt::bounds(result, &lo, &hi);
      for (sculpt::vec3& p : result.pos) p.x += (hi.x - lo.x) + 5;
    }
    // Into the frame of the group the part lives in.
    if (invertible)
      for (sculpt::vec3& p : result.pos) {
        const QVector3D local = toParent.map(QVector3D(float(p.x), float(p.y), float(p.z)));
        p = sculpt::vec3(local.x(), local.y(), local.z());
      }
    path = writeSculptStl(result, newName);
    centre = sculptCentre(result);
  }, result.tri.size() > 200000);
  if (path.isEmpty()) {
    QMessageBox::warning(this, newName, QStringLiteral("Could not write the result to disk."));
    return false;
  }
  pushUndo();
  node = doc.find(id);
  Node made;
  made.id = newId();
  made.kind = Node::Kind::Shape;
  made.shape = QStringLiteral("stl");
  made.sculpt = sculptPart;
  made.meshPath = path;
  made.name = newName;
  made.parentId = parentId;
  made.color = node ? node->color : QColor("#d7c4a3");
  made.hole = replace && node && node->hole;
  made.pos[0] = centre.x;
  made.pos[1] = centre.y;
  made.pos[2] = centre.z;
  if (replace) {
    // In the original's place in the list, so the parts tree keeps its order.
    size_t at = doc.nodes.size();
    for (size_t i = 0; i < doc.nodes.size(); ++i)
      if (doc.nodes[i].id == id) at = i;
    doc.remove(id);
    doc.nodes.insert(doc.nodes.begin() + static_cast<std::ptrdiff_t>(std::min(at, doc.nodes.size())), made);
  } else {
    doc.nodes.push_back(made);
  }
  selection = QStringList{made.id};
  view->focusOn(selection);  // the view turns around the new part
  refreshTree();
  loadInspector();
  refreshScene(false);
  return true;
}

bool MainWindow::partMesh(const QString& id, sculpt::Mesh* mesh) {
  // The part as it stands in the world (a hole as a solid), one mesh even if it is a group.
  Document shapeOnly = doc;
  if (Node* self = shapeOnly.find(id)) self->hole = false;
  std::vector<double> verts;
  std::vector<uint64_t> tris;
  bool ok = false;
  runBusy([&] { ok = worldMesh64(shapeOnly, QStringList{id}, &verts, &tris); }, true);
  if (!ok || tris.size() < 12) return false;
  mesh->pos.clear();
  mesh->tri.clear();
  mesh->pos.reserve(verts.size() / 3);
  for (size_t i = 0; i + 2 < verts.size(); i += 3) mesh->pos.emplace_back(verts[i], verts[i + 1], verts[i + 2]);
  for (size_t i = 0; i + 2 < tris.size(); i += 3)
    mesh->tri.push_back({static_cast<uint32_t>(tris[i]), static_cast<uint32_t>(tris[i + 1]), static_cast<uint32_t>(tris[i + 2])});
  return true;
}

void MainWindow::openLattice() {
  if (selection.size() != 1 || !doc.find(selection.front())) {
    status(QStringLiteral("Select one part (or group) to turn into a lattice."));
    return;
  }
  const QString id = selection.front();
  const QString name = doc.find(id)->name;
  sculpt::Mesh mesh;
  if (!partMesh(id, &mesh)) {
    status(QStringLiteral("%1 has no surface to make a lattice of.").arg(name));
    return;
  }
  LatticeDialog dialog(this, std::move(mesh), name);
  if (dialog.exec() != QDialog::Accepted) return;
  const bool replace = dialog.replaceOriginal();
  const QString newName = replace ? name + QStringLiteral(" lattice") : name + QStringLiteral(" (lattice)");
  if (placeMeshForPart(id, dialog.result(), replace, newName, false))
    status(replace ? QStringLiteral("%1 is a lattice now (undo brings the solid back).").arg(name)
                   : QStringLiteral("%1 added beside the original.").arg(newName));
}

void MainWindow::openTexture() {
  if (selection.size() != 1 || !doc.find(selection.front())) {
    status(QStringLiteral("Select one part (or group) to put a texture on."));
    return;
  }
  const QString id = selection.front();
  const QString name = doc.find(id)->name;
  sculpt::Mesh mesh;
  if (!partMesh(id, &mesh)) {
    status(QStringLiteral("%1 has no surface to texture.").arg(name));
    return;
  }
  EffectDialog dialog(this, std::move(mesh), QStringLiteral("Texture – %1").arg(name), QStringLiteral("the textured part"));
  QFormLayout* form = dialog.form();
  auto* kind = new QComboBox(&dialog);
  kind->addItems({QStringLiteral("Knurling"), QStringLiteral("Ribs"), QStringLiteral("Hexagons"), QStringLiteral("Dots"),
                  QStringLiteral("Wood grain"), QStringLiteral("Leather"), QStringLiteral("Carbon weave"), QStringLiteral("Picture…")});
  form->addRow(QStringLiteral("Pattern"), kind);
  auto mm = [&](double lo, double hi, double value, double step, const QString& tip) {
    auto* s = new QDoubleSpinBox(&dialog);
    s->setRange(lo, hi);
    s->setDecimals(2);
    s->setSingleStep(step);
    s->setValue(value);
    s->setSuffix(QStringLiteral(" mm"));
    s->setToolTip(tip);
    return s;
  };
  auto* size = mm(0.5, 100, 3, 0.5, QStringLiteral("How big one repeat of the pattern is (a picture: how wide)."));
  auto* depth = mm(0.1, 10, 0.6, 0.1, QStringLiteral("How far it stands out of the surface, or is cut into it. Two or three "
                                                     "layer heights shows well."));
  form->addRow(QStringLiteral("Size"), size);
  form->addRow(QStringLiteral("Depth"), depth);
  auto* way = new QComboBox(&dialog);
  way->addItems({QStringLiteral("Raised"), QStringLiteral("Cut in")});
  form->addRow(QStringLiteral("Texture is"), way);
  auto* wrap = new QComboBox(&dialog);
  wrap->addItems({QStringLiteral("From every side"), QStringLiteral("Round the part")});
  wrap->setToolTip(QStringLiteral("From every side suits boxy parts. Round the part wraps it seamlessly round anything "
                                  "round: handles, cups, knobs."));
  form->addRow(QStringLiteral("Wrap"), wrap);
  auto* where = new QComboBox(&dialog);
  where->addItems({QStringLiteral("All but the bottom"), QStringLiteral("Everywhere"), QStringLiteral("The sides only")});
  where->setToolTip(QStringLiteral("All but the bottom keeps the part flat on the plate."));
  form->addRow(QStringLiteral("Where"), where);
  auto* angle = new QDoubleSpinBox(&dialog);
  angle->setRange(-180, 180);
  angle->setSuffix(QStringLiteral("°"));
  angle->setSingleStep(15);
  angle->setToolTip(QStringLiteral("Turns the pattern on the surface."));
  form->addRow(QStringLiteral("Turn"), angle);
  auto* seed = new QSpinBox(&dialog);
  seed->setRange(1, 9999);
  seed->setToolTip(QStringLiteral("Another number gives another random pattern."));
  auto* seedLabel = new QLabel(QStringLiteral("Variation"), &dialog);
  form->addRow(seedLabel, seed);
  auto* detail = new QDoubleSpinBox(&dialog);
  detail->setRange(0, 2);
  detail->setDecimals(2);
  detail->setSingleStep(0.05);
  detail->setSpecialValueText(QStringLiteral("Automatic"));
  detail->setSuffix(QStringLiteral(" mm"));
  detail->setToolTip(QStringLiteral("Triangle size. Automatic follows the pattern; smaller is sharper but slower."));
  form->addRow(QStringLiteral("Detail"), detail);
  auto picture = std::make_shared<sculpt::StampImage>();
  auto pictureName = std::make_shared<QString>();
  auto settings = [=] {
    fx::TextureSettings s;
    s.kind = static_cast<fx::Texture>(kind->currentIndex());
    s.size = size->value();
    s.depth = depth->value();
    s.raised = way->currentIndex() == 0;
    s.wrap = wrap->currentIndex();
    s.where = where->currentIndex();
    s.angle = angle->value();
    s.seed = seed->value();
    s.detail = detail->value();
    s.picture = *picture;
    return s;
  };
  dialog.setBuild([settings] {
    const fx::TextureSettings s = settings();
    return EffectDialog::Build([s](const sculpt::Mesh& part, sculpt::Mesh* out, std::string* error, lattice::Report* report,
                                   lattice::Progress* progress) { return fx::texture(part, s, out, error, report, progress); });
  });
  EffectDialog* d = &dialog;
  dialog.setSummary([d, settings, pictureName] {
    const fx::TextureSettings s = settings();
    const double triangles = fx::textureTriangles(d->part(), s);
    QString text = QStringLiteral("Detail %1 mm, about %L2 triangles.")
                       .arg(fx::textureDetail(d->part(), s), 0, 'f', 2)
                       .arg(static_cast<qlonglong>(std::round(triangles / 1000) * 1000));
    if (triangles > 3e6) text += QStringLiteral(" A lot: a larger size or coarser detail is quicker.");
    if (s.kind == fx::Texture::Picture)
      text += pictureName->isEmpty() ? QStringLiteral(" Choose a picture: bright parts stand out.")
                                     : QStringLiteral(" Picture: %1 (bright parts stand out).").arg(*pictureName);
    return text;
  });
  auto refresh = [d, kind, seed, seedLabel] {
    const auto k = static_cast<fx::Texture>(kind->currentIndex());
    const bool random = k == fx::Texture::Wood || k == fx::Texture::Leather;
    seed->setVisible(random);
    seedLabel->setVisible(random);
    d->changed();
  };
  connect(kind, &QComboBox::currentIndexChanged, &dialog, [this, kind, picture, pictureName, refresh](int index) {
    if (static_cast<fx::Texture>(index) == fx::Texture::Picture) {
      const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Texture picture"), QString(),
                                                        QStringLiteral("Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.webp)"));
      QImage image(path);
      if (!path.isEmpty() && !image.isNull()) {
        image = image.convertToFormat(QImage::Format_Grayscale8);
        if (std::max(image.width(), image.height()) > 512) image = image.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        picture->width = image.width();
        picture->height = image.height();
        picture->value.assign(static_cast<size_t>(image.width()) * image.height(), 0.f);
        for (int y = 0; y < image.height(); ++y) {
          const uchar* line = image.constScanLine(y);
          for (int x = 0; x < image.width(); ++x) picture->value[static_cast<size_t>(y) * image.width() + x] = line[x] / 255.f;
        }
        *pictureName = QFileInfo(path).fileName();
      } else if (picture->width == 0) {
        kind->setCurrentIndex(0);
        return;
      }
    }
    refresh();
  });
  for (QDoubleSpinBox* s : {size, depth, angle, detail}) connect(s, &QDoubleSpinBox::valueChanged, &dialog, [d] { d->changed(); });
  for (QComboBox* c : {way, wrap, where}) connect(c, &QComboBox::currentIndexChanged, &dialog, [d] { d->changed(); });
  connect(seed, &QSpinBox::valueChanged, &dialog, [d] { d->changed(); });
  // Round parts (a footprint as wide as deep, with many facets) start wrapped round.
  {
    sculpt::vec3 lo, hi;
    sculpt::bounds(dialog.part(), &lo, &hi);
    const double w = hi.x - lo.x, dpt = hi.y - lo.y;
    if (std::abs(w - dpt) < 0.05 * std::max(w, dpt) && dialog.part().tri.size() > 60) wrap->setCurrentIndex(1);
  }
  refresh();
  if (dialog.exec() != QDialog::Accepted) return;
  const bool replace = dialog.replaceOriginal();
  const QString newName = replace ? name : name + QStringLiteral(" (textured)");
  if (placeMeshForPart(id, dialog.result(), replace, newName, false))
    status(replace ? QStringLiteral("%1 has its texture now (undo brings the smooth one back).").arg(name)
                   : QStringLiteral("%1 added beside the original.").arg(newName));
}

void MainWindow::openWrap() {
  if (selection.size() != 2 || !doc.find(selection[0]) || !doc.find(selection[1])) {
    status(QStringLiteral("Select the text (or logo) and the part to wrap it onto: two parts."));
    return;
  }
  sculpt::Mesh first, second;
  if (!partMesh(selection[0], &first) || !partMesh(selection[1], &second)) {
    status(QStringLiteral("Both need a surface to wrap."));
    return;
  }
  // The thinner of the two is the one to wrap.
  auto thinness = [](const sculpt::Mesh& m) {
    sculpt::vec3 lo, hi;
    sculpt::bounds(m, &lo, &hi);
    return std::min({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
  };
  const bool firstIsDecal = thinness(first) <= thinness(second);
  const QString decalId = firstIsDecal ? selection[0] : selection[1];
  const QString targetId = firstIsDecal ? selection[1] : selection[0];
  auto decal = std::make_shared<sculpt::Mesh>(firstIsDecal ? std::move(first) : std::move(second));
  auto target = std::make_shared<sculpt::Mesh>(firstIsDecal ? std::move(second) : std::move(first));
  const QString decalName = doc.find(decalId)->name, targetName = doc.find(targetId)->name;
  const QColor decalColour = doc.find(decalId)->color;
  sculpt::vec3 dlo, dhi, tlo, thi;
  sculpt::bounds(*decal, &dlo, &dhi);
  sculpt::bounds(*target, &tlo, &thi);
  auto both = [](const sculpt::Mesh& a, const sculpt::Mesh& b) {
    sculpt::Mesh m = a;
    const uint32_t base = static_cast<uint32_t>(m.pos.size());
    m.pos.insert(m.pos.end(), b.pos.begin(), b.pos.end());
    for (sculpt::Tri t : b.tri) m.tri.push_back({t[0] + base, t[1] + base, t[2] + base});
    return m;
  };
  EffectDialog dialog(this, both(*target, *decal), QStringLiteral("Wrap – %1 onto %2").arg(decalName, targetName),
                      QStringLiteral("the wrapped text"));
  dialog.setChoice(QStringLiteral("Put %1 on %2. Remove the flat one, or keep it too?").arg(decalName, targetName),
                   QStringLiteral("Remove the flat one"), QStringLiteral("Keep it too"));
  QFormLayout* form = dialog.form();
  const int facing = fx::wrapFacing(*decal);
  auto* reads = new QLabel(facing == 0   ? QStringLiteral("%1 lies flat: it is read from above.").arg(decalName)
                           : facing == 1 ? QStringLiteral("%1 stands facing the front.").arg(decalName)
                                         : QStringLiteral("%1 stands facing the right.").arg(decalName),
                           &dialog);
  reads->setWordWrap(true);
  reads->setStyleSheet(QStringLiteral("color: #c3cad4;"));
  form->addRow(reads);
  auto* mode = new QComboBox(&dialog);
  mode->addItems({QStringLiteral("Round the part"), QStringLiteral("Straight on")});
  mode->setToolTip(QStringLiteral("Round the part bends it round the part's upright axis without stretching: mugs, pen "
                                  "holders, bottles, balls. Straight on presses it onto the surface right where it stands "
                                  "(put it in front of, or above, the spot first)."));
  form->addRow(QStringLiteral("Wrap"), mode);
  auto* angle = new QDoubleSpinBox(&dialog);
  angle->setRange(-180, 180);
  angle->setDecimals(0);
  angle->setSingleStep(15);
  angle->setSuffix(QStringLiteral("°"));
  angle->setToolTip(QStringLiteral("Where round the part: 0° the front, 90° the right side, 180° the back."));
  auto* angleLabel = new QLabel(QStringLiteral("Where round"), &dialog);
  form->addRow(angleLabel, angle);
  auto* height = new QDoubleSpinBox(&dialog);
  height->setRange(-1000, 1000);
  height->setDecimals(1);
  height->setSuffix(QStringLiteral(" mm"));
  height->setToolTip(QStringLiteral("The middle of the text, above the part's bottom."));
  auto* heightLabel = new QLabel(QStringLiteral("Height"), &dialog);
  form->addRow(heightLabel, height);
  auto* kind = new QComboBox(&dialog);
  kind->addItems({QStringLiteral("Raised"), QStringLiteral("Cut in")});
  kind->setToolTip(QStringLiteral("Raised stands out as a part of its own (give it another colour for a multi-colour print). "
                                  "Cut in engraves it: a hole grouped with the part."));
  form->addRow(QStringLiteral("Text is"), kind);
  auto* depth = new QDoubleSpinBox(&dialog);
  depth->setRange(0.2, 20);
  depth->setDecimals(2);
  depth->setSingleStep(0.2);
  depth->setSuffix(QStringLiteral(" mm"));
  depth->setValue(std::clamp(std::round(std::min({dhi.x - dlo.x, dhi.y - dlo.y, dhi.z - dlo.z}) * 5) / 5, 0.4, 5.0));
  form->addRow(QStringLiteral("Depth"), depth);
  // Start where it is: beside the part, round towards it, at its height.
  {
    const double dx = (dlo.x + dhi.x) * 0.5 - (tlo.x + thi.x) * 0.5, dy = (dlo.y + dhi.y) * 0.5 - (tlo.y + thi.y) * 0.5;
    if (std::hypot(dx, dy) > 0.25 * std::max(thi.x - tlo.x, thi.y - tlo.y))
      angle->setValue(std::round(std::atan2(dy, dx) * 180 / 3.14159265358979 + 90));
    const double mid = (dlo.z + dhi.z) * 0.5;
    height->setValue(facing != 0 && mid > tlo.z && mid < thi.z ? mid - tlo.z : (thi.z - tlo.z) * 0.5);
    if (angle->value() > 180) angle->setValue(angle->value() - 360);
  }
  auto wrapped = std::make_shared<sculpt::Mesh>();
  dialog.setBuild([=] {
    fx::WrapSettings ws;
    ws.mode = mode->currentIndex();
    ws.angle = angle->value();
    ws.height = height->value();
    ws.depth = depth->value();
    ws.cut = kind->currentIndex() == 1;
    return EffectDialog::Build([=](const sculpt::Mesh&, sculpt::Mesh* out, std::string* error, lattice::Report*,
                                   lattice::Progress* progress) {
      sculpt::Mesh made;
      if (!fx::wrap(*decal, *target, ws, &made, error, progress)) return false;
      *wrapped = made;
      *out = both(*target, made);
      return true;
    });
  });
  EffectDialog* d = &dialog;
  auto refresh = [=] {
    const bool round = mode->currentIndex() == 0;
    for (QWidget* w : std::initializer_list<QWidget*>{angle, angleLabel, height, heightLabel}) w->setVisible(round);
    d->changed();
  };
  connect(mode, &QComboBox::currentIndexChanged, &dialog, [refresh] { refresh(); });
  connect(kind, &QComboBox::currentIndexChanged, &dialog, [d] { d->changed(); });
  for (QDoubleSpinBox* s : {angle, height, depth}) connect(s, &QDoubleSpinBox::valueChanged, &dialog, [d] { d->changed(); });
  refresh();
  if (dialog.exec() != QDialog::Accepted || wrapped->tri.empty()) return;
  const bool cut = kind->currentIndex() == 1;
  const Node* targetNode = doc.find(targetId);
  if (!targetNode) return;
  const QString parentId = targetNode->parentId;
  pushUndo();
  const QString made = addMeshNode(*wrapped, cut ? decalName + QStringLiteral(" (cut in)") : decalName + QStringLiteral(" (wrapped)"),
                                   parentId, cut ? targetNode->color : decalColour, cut);
  if (made.isEmpty()) {
    undoStack.pop_back();
    status(QStringLiteral("Could not write the wrapped text to disk."));
    return;
  }
  if (Node* m = doc.find(made); m && cut) m->clearanceMode = 0;
  QString note;
  if (dialog.replaceOriginal()) doc.remove(decalId);
  if (cut) {
    QString error;
    if (doc.groupSelected(QStringList{targetId, made}, &error)) {
      doc.nodes.back().name = targetName;
      selection = QStringList{doc.nodes.back().id};
    } else {
      selection = QStringList{made};
      note = QStringLiteral(" Group it with %1 to cut it in.").arg(targetName);
    }
  } else {
    selection = QStringList{made};
  }
  refreshTree();
  loadInspector();
  refreshScene(false);
  status(cut ? QStringLiteral("%1 is cut into %2.%3").arg(decalName, targetName, note)
             : QStringLiteral("%1 is wrapped onto %2, as a part of its own (give it its own colour if you like).").arg(decalName, targetName));
}

void MainWindow::openArticulate() {
  if (selection.size() != 1 || !doc.find(selection.front())) {
    status(QStringLiteral("Select one part (or group) to make into a flexi."));
    return;
  }
  const QString id = selection.front();
  const QString name = doc.find(id)->name;
  sculpt::Mesh mesh;
  if (!partMesh(id, &mesh)) {
    status(QStringLiteral("%1 has no surface to cut into segments.").arg(name));
    return;
  }
  sculpt::vec3 lo, hi;
  sculpt::bounds(mesh, &lo, &hi);
  EffectDialog dialog(this, std::move(mesh), QStringLiteral("Articulate – %1").arg(name), QStringLiteral("the flexi"));
  QFormLayout* form = dialog.form();
  auto* about = new QLabel(QStringLiteral("Cuts the part into segments joined by print-in-place joints, so it prints flat in "
                                          "one go and wiggles off the plate: dragons, snakes, fish. Joints need about 9 mm of "
                                          "thickness."),
                           &dialog);
  about->setWordWrap(true);
  about->setStyleSheet(QStringLiteral("color: #c3cad4;"));
  form->addRow(about);
  auto* along = new QComboBox(&dialog);
  along->addItems({QStringLiteral("Along X"), QStringLiteral("Along Y")});
  along->setCurrentIndex(hi.y - lo.y > hi.x - lo.x ? 1 : 0);
  form->addRow(QStringLiteral("Direction"), along);
  auto* joints = new QSpinBox(&dialog);
  joints->setRange(1, 40);
  const double longest = std::max(hi.x - lo.x, hi.y - lo.y);
  joints->setValue(std::clamp(static_cast<int>(std::round(longest * 0.76 / std::max(14.0, (hi.z - lo.z) * 1.6))), 1, 12));
  joints->setToolTip(QStringLiteral("How many joints; there is one more segment than joints."));
  form->addRow(QStringLiteral("Joints"), joints);
  auto percent = [&](int value, const QString& tip) {
    auto* s = new QSpinBox(&dialog);
    s->setRange(0, 100);
    s->setValue(value);
    s->setSuffix(QStringLiteral(" %"));
    s->setToolTip(tip);
    return s;
  };
  auto* from = percent(12, QStringLiteral("Where the first joint is, along the part. Keep a big head in one piece by moving it."));
  auto* to = percent(88, QStringLiteral("Where the last joint is, along the part."));
  form->addRow(QStringLiteral("First joint at"), from);
  form->addRow(QStringLiteral("Last joint at"), to);
  auto* swing = new QSpinBox(&dialog);
  swing->setRange(5, 60);
  swing->setValue(15);
  swing->setSuffix(QStringLiteral("°"));
  swing->setToolTip(QStringLiteral("How far each joint swings each way. More swing makes wider V-shaped gaps between segments."));
  form->addRow(QStringLiteral("Swing"), swing);
  auto* gap = new QDoubleSpinBox(&dialog);
  gap->setRange(0.1, 1.5);
  gap->setDecimals(2);
  gap->setSingleStep(0.05);
  gap->setValue(0.35);
  gap->setSuffix(QStringLiteral(" mm"));
  gap->setToolTip(QStringLiteral("The gap between faces that move. Print-in-place needs more than a fit: 0.3 to 0.4 mm works "
                                 "on most printers; raise it if the joints print stuck."));
  form->addRow(QStringLiteral("Joint gap"), gap);
  EffectDialog* d = &dialog;
  dialog.setBuild([=] {
    fx::ArticulateSettings as;
    as.axis = along->currentIndex();
    as.joints = joints->value();
    as.from = from->value() / 100.0;
    as.to = std::max(to->value(), from->value()) / 100.0;
    as.swing = swing->value();
    as.gap = gap->value();
    return EffectDialog::Build([as](const sculpt::Mesh& part, sculpt::Mesh* out, std::string* error, lattice::Report*,
                                    lattice::Progress* progress) { return fx::articulate(part, as, out, error, progress); });
  });
  dialog.setSummary([=] {
    const double length = along->currentIndex() == 0 ? hi.x - lo.x : hi.y - lo.y;
    const double span = length * std::max(0, to->value() - from->value()) / 100.0;
    return QStringLiteral("%1 segments; joints %2 mm apart.")
        .arg(joints->value() + 1)
        .arg(joints->value() > 1 ? span / (joints->value() - 1) : 0.0, 0, 'f', 1);
  });
  connect(along, &QComboBox::currentIndexChanged, &dialog, [d] { d->changed(); });
  for (QSpinBox* s : {joints, from, to, swing}) connect(s, &QSpinBox::valueChanged, &dialog, [d] { d->changed(); });
  connect(gap, &QDoubleSpinBox::valueChanged, &dialog, [d] { d->changed(); });
  if (dialog.exec() != QDialog::Accepted) return;
  const bool replace = dialog.replaceOriginal();
  const QString newName = replace ? name : name + QStringLiteral(" (flexi)");
  if (placeMeshForPart(id, dialog.result(), replace, newName, false))
    status(QStringLiteral("%1 is a flexi now: print it flat as it is, and work the joints loose once it is off the plate.").arg(newName));
}

void MainWindow::openMold() {
  if (selection.size() != 1 || !doc.find(selection.front())) {
    status(QStringLiteral("Select one part (or group) to make a mould of."));
    return;
  }
  const QString id = selection.front();
  const QString name = doc.find(id)->name;
  sculpt::Mesh mesh;
  if (!partMesh(id, &mesh)) {
    status(QStringLiteral("%1 has no surface to make a mould of.").arg(name));
    return;
  }
  sculpt::vec3 lo, hi;
  sculpt::bounds(mesh, &lo, &hi);
  const double volume = std::abs(sculpt::volume(mesh));
  EffectDialog dialog(this, mesh, QStringLiteral("Mold – %1").arg(name), QStringLiteral("the mould"));
  dialog.setChoice(QStringLiteral("Keep %1 beside the mould, or put the mould in its place?").arg(name),
                   QStringLiteral("Mould in its place"), QStringLiteral("Keep the part too"));
  QFormLayout* form = dialog.form();
  auto* about = new QLabel(QStringLiteral("A two-piece mould for casting resin, silicone, plaster, wax or chocolate. The "
                                          "part's shape is hollowed out of a block, split in two, with a pour hole (and a "
                                          "funnel) at the top, air vents from other high points, and round keys that line "
                                          "the halves up. The part has to come out of each half: the split goes through its "
                                          "widest slice unless you pick another place."),
                           &dialog);
  about->setWordWrap(true);
  about->setStyleSheet(QStringLiteral("color: #c3cad4;"));
  form->addRow(about);
  auto* axis = new QComboBox(&dialog);
  axis->addItems({QStringLiteral("Across X (left and right)"), QStringLiteral("Across Y (front and back)"),
                  QStringLiteral("Across Z (bottom and top)")});
  axis->setCurrentIndex(2);
  axis->setToolTip(QStringLiteral("Which way the mould comes apart. Pick the one the part slides out along most easily: "
                                  "for a figure standing up, front and back is often best."));
  form->addRow(QStringLiteral("Split"), axis);
  auto* at = new QSpinBox(&dialog);
  at->setRange(4, 96);
  at->setSuffix(QStringLiteral(" %"));
  at->setSpecialValueText(QStringLiteral("Widest slice"));
  at->setMinimum(4);
  at->setValue(4);
  at->setToolTip(QStringLiteral("Where the halves meet, across the part. Widest slice finds the place the part comes out "
                                "of both halves best."));
  form->addRow(QStringLiteral("Split at"), at);
  auto mm = [&](double lowest, double highest, double value, double stepSize, const QString& tip) {
    auto* s = new QDoubleSpinBox(&dialog);
    s->setRange(lowest, highest);
    s->setDecimals(2);
    s->setSingleStep(stepSize);
    s->setValue(value);
    s->setSuffix(QStringLiteral(" mm"));
    s->setToolTip(tip);
    return s;
  };
  const double size = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
  auto* wall = mm(2, 60, std::clamp(std::round(size * 0.15), 5.0, 15.0), 1,
                  QStringLiteral("How much mould there is round the part. Thicker is stiffer, thinner uses less filament."));
  form->addRow(QStringLiteral("Walls"), wall);
  auto* pour = mm(0, 30, std::clamp(std::round(size * 0.08), 4.0, 10.0), 0.5,
                  QStringLiteral("Width of the pour hole at the part's highest point. 0 leaves it out."));
  pour->setSpecialValueText(QStringLiteral("None"));
  form->addRow(QStringLiteral("Pour hole"), pour);
  auto* vents = new QSpinBox(&dialog);
  vents->setRange(0, 8);
  vents->setValue(2);
  vents->setToolTip(QStringLiteral("Thin channels from other high points, so trapped air can escape."));
  form->addRow(QStringLiteral("Air vents"), vents);
  auto* vent = mm(0.5, 6, 2, 0.25, QStringLiteral("Width of each air vent."));
  form->addRow(QStringLiteral("Vent width"), vent);
  auto* keys = new QCheckBox(QStringLiteral("Round keys line the halves up"), &dialog);
  keys->setChecked(true);
  form->addRow(QStringLiteral("Keys"), keys);
  auto* clearance = mm(0, 1, std::max(0.1, printerClearance()), 0.05,
                       QStringLiteral("Gap round each key in its socket, so the halves close without forcing."));
  form->addRow(QStringLiteral("Key gap"), clearance);
  auto* layout = new QComboBox(&dialog);
  layout->addItems({QStringLiteral("Print: both halves face up"), QStringLiteral("Closed, to look at")});
  layout->setToolTip(QStringLiteral("Face up, the cavity opens like a bowl: neither half needs supports. Closed shows the "
                                    "mould as it goes together."));
  form->addRow(QStringLiteral("Layout"), layout);
  EffectDialog* d = &dialog;
  auto settingsNow = [=] {
    fx::MoldSettings ms;
    ms.axis = axis->currentIndex();
    ms.at = at->value() <= at->minimum() ? -1 : at->value() / 100.0;
    ms.wall = wall->value();
    ms.pour = pour->value();
    ms.vents = vents->value();
    ms.vent = vent->value();
    ms.keys = keys->isChecked();
    ms.clearance = clearance->value();
    ms.printLayout = layout->currentIndex() == 0;
    return ms;
  };
  dialog.setBuild([=] {
    const fx::MoldSettings ms = settingsNow();
    return EffectDialog::Build([ms](const sculpt::Mesh& part, sculpt::Mesh* out, std::string* error, lattice::Report*,
                                    lattice::Progress* progress) { return fx::mold(part, ms, out, error, progress); });
  });
  dialog.setSummary([=] {
    const double w = wall->value();
    return QStringLiteral("Block %1 × %2 × %3 mm; about %4 ml to fill.")
        .arg(hi.x - lo.x + 2 * w, 0, 'f', 0)
        .arg(hi.y - lo.y + 2 * w, 0, 'f', 0)
        .arg(hi.z - lo.z + 2 * w, 0, 'f', 0)
        .arg(volume / 1000.0, 0, 'f', volume < 10000 ? 1 : 0);
  });
  connect(axis, &QComboBox::currentIndexChanged, &dialog, [d] { d->changed(); });
  connect(layout, &QComboBox::currentIndexChanged, &dialog, [d] { d->changed(); });
  connect(keys, &QCheckBox::toggled, &dialog, [d, clearance](bool on) {
    clearance->setEnabled(on);
    d->changed();
  });
  for (QSpinBox* s : {at, vents}) connect(s, &QSpinBox::valueChanged, &dialog, [d] { d->changed(); });
  for (QDoubleSpinBox* s : {wall, pour, vent, clearance}) connect(s, &QDoubleSpinBox::valueChanged, &dialog, [d] { d->changed(); });
  if (dialog.exec() != QDialog::Accepted) return;
  const bool replace = dialog.replaceOriginal();
  const QString newName = name + QStringLiteral(" mould");
  if (placeMeshForPart(id, dialog.result(), replace, newName, false))
    status(QStringLiteral("%1 added: print both halves as they lie, close them on the keys, and pour.").arg(newName));
}

void MainWindow::openSimplify() {
  if (selection.size() != 1 || !doc.find(selection.front())) {
    status(QStringLiteral("Select one part (or group) to simplify."));
    return;
  }
  const QString id = selection.front();
  const QString name = doc.find(id)->name;
  sculpt::Mesh mesh;
  if (!partMesh(id, &mesh)) {
    status(QStringLiteral("%1 has no surface to simplify.").arg(name));
    return;
  }
  const size_t before = mesh.tri.size();
  EffectDialog dialog(this, std::move(mesh), QStringLiteral("Simplify – %1").arg(name), QStringLiteral("the simplified part"));
  QFormLayout* form = dialog.form();
  auto* about = new QLabel(QStringLiteral("Fewer triangles for the same shape, so a heavy sculpt or STL is quicker to "
                                          "work with, group and export. Flat and gently curved areas lose the most; "
                                          "sharp detail stays."),
                           &dialog);
  about->setWordWrap(true);
  about->setStyleSheet(QStringLiteral("color: #c3cad4;"));
  form->addRow(about);
  auto* tolerance = new QDoubleSpinBox(&dialog);
  tolerance->setRange(0.002, 1);
  tolerance->setDecimals(3);
  tolerance->setSingleStep(0.005);
  tolerance->setValue(0.02);
  tolerance->setSuffix(QStringLiteral(" mm"));
  tolerance->setToolTip(QStringLiteral("How far the surface may move. 0.02 mm is far below what a printer shows (a layer is "
                                       "about 0.2 mm); larger is lighter still."));
  form->addRow(QStringLiteral("Keep within"), tolerance);
  EffectDialog* d = &dialog;
  dialog.setBuild([tolerance] {
    const double tol = tolerance->value();
    return EffectDialog::Build([tol](const sculpt::Mesh& part, sculpt::Mesh* out, std::string* error, lattice::Report*,
                                     lattice::Progress* progress) { return fx::simplify(part, tol, out, error, progress); });
  });
  dialog.setSummary([before] { return QStringLiteral("Now %L1 triangles. Preview shows how many are left.").arg(static_cast<qlonglong>(before)); });
  connect(tolerance, &QDoubleSpinBox::valueChanged, &dialog, [d] { d->changed(); });
  if (dialog.exec() != QDialog::Accepted) return;
  const bool replace = dialog.replaceOriginal();
  const size_t after = dialog.result().tri.size();
  const QString newName = replace ? name : name + QStringLiteral(" (simplified)");
  if (placeMeshForPart(id, dialog.result(), replace, newName, false))
    status(QStringLiteral("%1: %L2 triangles instead of %L3 (%4 % of them).")
               .arg(newName)
               .arg(static_cast<qlonglong>(after))
               .arg(static_cast<qlonglong>(before))
               .arg(before ? 100.0 * after / before : 0.0, 0, 'f', 1));
}

void MainWindow::openRestyle() {
  if (selection.size() != 1 || !doc.find(selection.front())) {
    status(QStringLiteral("Select one part (or group) to restyle."));
    return;
  }
  const QString id = selection.front();
  const QString name = doc.find(id)->name;
  sculpt::Mesh mesh;
  if (!partMesh(id, &mesh)) {
    status(QStringLiteral("%1 has no surface to restyle.").arg(name));
    return;
  }
  sculpt::vec3 lo, hi;
  sculpt::bounds(mesh, &lo, &hi);
  const double longest = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
  EffectDialog dialog(this, std::move(mesh), QStringLiteral("Low-poly / Voxel – %1").arg(name), QStringLiteral("the restyled part"));
  dialog.setFlatPreview(true);
  QFormLayout* form = dialog.form();
  auto* style = new QComboBox(&dialog);
  style->addItems({QStringLiteral("Low-poly"), QStringLiteral("Voxel")});
  form->addRow(QStringLiteral("Style"), style);
  auto* about = new QLabel(&dialog);
  about->setWordWrap(true);
  about->setStyleSheet(QStringLiteral("color: #c3cad4;"));
  form->addRow(about);
  auto* facet = new QDoubleSpinBox(&dialog);
  facet->setRange(0.5, 200);
  facet->setDecimals(1);
  facet->setSuffix(QStringLiteral(" mm"));
  facet->setValue(std::max(1.0, std::round(longest / 5)));
  facet->setToolTip(QStringLiteral("How big the flat facets (or the cubes) are. Larger: chunkier."));
  auto* facetLabel = new QLabel(QStringLiteral("Facet size"), &dialog);
  form->addRow(facetLabel, facet);
  auto* jitter = new QSpinBox(&dialog);
  jitter->setRange(0, 100);
  jitter->setValue(25);
  jitter->setSuffix(QStringLiteral(" %"));
  jitter->setToolTip(QStringLiteral("Nudges the corners about for a hand-cut look. 0 keeps the facets even."));
  auto* jitterLabel = new QLabel(QStringLiteral("Roughness"), &dialog);
  form->addRow(jitterLabel, jitter);
  auto* seed = new QSpinBox(&dialog);
  seed->setRange(1, 9999);
  auto* seedLabel = new QLabel(QStringLiteral("Variation"), &dialog);
  form->addRow(seedLabel, seed);
  EffectDialog* d = &dialog;
  dialog.setBuild([style, facet, jitter, seed] {
    const bool voxels = style->currentIndex() == 1;
    const double size = facet->value(), rough = jitter->value() / 100.0;
    const int variation = seed->value();
    return EffectDialog::Build([=](const sculpt::Mesh& part, sculpt::Mesh* out, std::string* error, lattice::Report*,
                                   lattice::Progress* progress) {
      return voxels ? fx::voxel(part, size, out, error, progress) : fx::lowPoly(part, size, rough, variation, out, error, progress);
    });
  });
  dialog.setSummary([d, style, facet] {
    if (style->currentIndex() == 1) {
      const size_t cells = fx::voxelCount(d->part(), facet->value());
      return QStringLiteral("A grid of %L1 cubes over the part's box.%2")
          .arg(static_cast<qlonglong>(cells))
          .arg(cells > 4000000 ? QStringLiteral(" Too many: make the cubes larger.") : QString());
    }
    return QStringLiteral("Facets about %1 mm across.").arg(facet->value(), 0, 'f', 1);
  });
  auto refresh = [d, style, about, facetLabel, jitter, jitterLabel, seed, seedLabel] {
    const bool voxels = style->currentIndex() == 1;
    about->setText(voxels ? QStringLiteral("Built from cubes, like a pixel-art sculpture. Prints well: every face is flat "
                                           "and every overhang a square step.")
                          : QStringLiteral("Big flat facets, like a cut gem or a papercraft model. Rough nudges the "
                                           "corners about for a hand-cut look."));
    facetLabel->setText(voxels ? QStringLiteral("Cube size") : QStringLiteral("Facet size"));
    for (QWidget* w : std::initializer_list<QWidget*>{jitter, jitterLabel, seed, seedLabel}) w->setVisible(!voxels);
    d->changed();
  };
  connect(style, &QComboBox::currentIndexChanged, &dialog, [refresh] { refresh(); });
  connect(facet, &QDoubleSpinBox::valueChanged, &dialog, [d] { d->changed(); });
  for (QSpinBox* s : {jitter, seed}) connect(s, &QSpinBox::valueChanged, &dialog, [d] { d->changed(); });
  refresh();
  if (dialog.exec() != QDialog::Accepted) return;
  const bool replace = dialog.replaceOriginal();
  const QString styleName = style->currentIndex() == 1 ? QStringLiteral("voxel") : QStringLiteral("low-poly");
  const QString newName = replace ? name : QStringLiteral("%1 (%2)").arg(name, styleName);
  if (placeMeshForPart(id, dialog.result(), replace, newName, false))
    status(replace ? QStringLiteral("%1 is %2 now (undo brings it back).").arg(name, styleName)
                   : QStringLiteral("%1 added beside the original.").arg(newName));
}

void MainWindow::edit2DShape(const QString& path) {
  if (path.isEmpty()) return;
  if (pathIsInside(path, defaultAppdataPath(QStringLiteral("Imported")))) {
    status(QStringLiteral("Imported 2D shapes cannot be edited."));
    return;
  }
  Shape2DEditorDialog dialog(this, defaultAppdataPath(QStringLiteral("2d_shapes")), path);
  dialog.exec();
  if (!dialog.savedThisSession()) return;
  refresh2DShapeLibrary();
  const QString saved = dialog.savedPathValue();
  const QString source = QDir::cleanPath(QFileInfo(saved).absoluteFilePath());
  int linked = 0;
  for (const Node& node : doc.nodes)
    if (node.shape == QLatin1String("svg") && !node.meshSource.isEmpty() && QDir::cleanPath(node.meshSource) == source) ++linked;
  if (linked == 0) return;
  const QString question = QStringLiteral("%1 part%2 on the bed use%3 the earlier version of “%4”. Update %5 to the new version?")
                               .arg(linked)
                               .arg(linked == 1 ? QString() : QStringLiteral("s"))
                               .arg(linked == 1 ? QStringLiteral("s") : QString())
                               .arg(QFileInfo(saved).completeBaseName())
                               .arg(linked == 1 ? QStringLiteral("it") : QStringLiteral("them"));
  if (QMessageBox::question(this, QStringLiteral("Update placed parts"), question) != QMessageBox::Yes) {
    status(QStringLiteral("Placed parts keep the earlier version."));
    return;
  }
  pushUndo();
  const QColor ink = readSvgShapeColor(saved);
  int updated = 0;
  QStringList kept;
  for (Node& node : doc.nodes) {
    if (node.shape != QLatin1String("svg") || node.meshSource.isEmpty() || QDir::cleanPath(node.meshSource) != source) continue;
    const QDir folder = QFileInfo(saved).absoluteDir();
    const QString file = node.meshLevel > 0   ? folder.absoluteFilePath(QStringLiteral("level-%1.svg").arg(node.meshLevel))
                         : node.meshLayer > 0 ? folder.absoluteFilePath(QStringLiteral("layer-%1.svg").arg(node.meshLayer))
                                              : saved;
    const QString frozen = QFileInfo::exists(file) ? freezePlaced2D(file) : QString();
    if (frozen.isEmpty()) {
      kept << node.name;
      continue;
    }
    // Follow the shape's new color unless this part was recolored on the bed.
    if (node.color == readSvgShapeColor(node.meshPath)) node.color = ink;
    node.meshPath = frozen;
    ++updated;
  }
  refreshScene(false);
  loadInspector();
  if (kept.isEmpty())
    status(QStringLiteral("Updated %1 placed part%2.").arg(updated).arg(updated == 1 ? QString() : QStringLiteral("s")));
  else
    status(QStringLiteral("Updated %1. Left as they were, because their layer or level is gone from the shape: %2.").arg(updated).arg(kept.join(", ")));
}

void MainWindow::importSvgShape(const QString& path, double x, double y) {
  if (path.isEmpty()) return;
  QFileInfo info(path);
  if (!info.exists() || !info.isFile()) {
    status(QStringLiteral("SVG shape not found: ") + path);
    return;
  }
  QVector<QPointF> points;
  if (!readSimpleSvgPolygon(info.absoluteFilePath(), &points)) {
    status(QStringLiteral("That SVG has no polygon or rectangle this importer can use."));
    return;
  }
  const double placeX = doc.snapOn ? std::round(x / doc.snap) * doc.snap : x;
  const double placeY = doc.snapOn ? std::round(y / doc.snap) * doc.snap : y;
  QVector<Shape2DContour> steps;
  QList<int> layers;
  QMap<int, double> levelHeights;
  QList<int> levels{1};
  if (readSynthvg(synthvgPathFor(info.absoluteFilePath()), &steps, nullptr, nullptr, nullptr, nullptr, &levelHeights)) {
    levels = shape2DLevels(steps);
    for (const Shape2DContour& contour : steps)
      if (!layers.contains(contour.layer)) layers.push_back(contour.layer);
    std::sort(layers.begin(), layers.end());
  }
  // Only shapes from the library are frozen and linked; project and Imported copies are already private.
  const bool library = !pathIsInside(info.absoluteFilePath(), defaultAppdataPath(QStringLiteral("Imported"))) &&
                       !pathIsInside(info.absoluteFilePath(), defaultAppdataPath(QStringLiteral("placed_2d"))) &&
                       (projectDirectory.isEmpty() || !pathIsInside(info.absoluteFilePath(), projectDirectory));
  auto placedFile = [&](const QString& file) {
    if (!library) return file;
    const QString frozen = freezePlaced2D(file);
    return frozen.isEmpty() ? file : frozen;
  };
  const QString source = library ? QDir::cleanPath(info.absoluteFilePath()) : QString();
  pushUndo();
  if (levels.size() > 1) {
    // Stacked levels: one part per level, each standing exactly on the one below. An empty level
    // in between leaves a gap of its thickness.
    Node parent;
    parent.id = newId();
    parent.kind = Node::Kind::Group;
    parent.name = info.completeBaseName();
    const QString parentId = parent.id;
    doc.nodes.push_back(std::move(parent));
    double bottom = doc.workplaneZ;
    int placed = 0;
    for (int level = 1; level <= levels.back(); ++level) {
      const double height = shape2DLevelHeight(levelHeights, level);
      const QString levelPath = info.absolutePath() + QDir::separator() + QStringLiteral("level-%1.svg").arg(level);
      if (levels.contains(level) && QFileInfo::exists(levelPath)) {
        Node node;
        node.id = newId();
        node.parentId = parentId;
        node.kind = Node::Kind::Shape;
        node.shape = QStringLiteral("svg");
        node.meshPath = placedFile(levelPath);
        node.meshSource = source;
        node.meshLevel = level;
        node.name = info.completeBaseName() + QStringLiteral(" level %1").arg(level);
        node.size[2] = height;
        node.pos[0] = placeX;
        node.pos[1] = placeY;
        node.pos[2] = bottom + height * 0.5;
        node.color = readSvgShapeColor(levelPath);
        node.pivotBaked = true;
        doc.nodes.push_back(std::move(node));
        ++placed;
      }
      bottom += height;
    }
    standOnWorkplane(parentId, placingDropped ? placingU : 0, placingDropped ? placingV : 0);
    selection = QStringList{parentId};
    view->focusOn(selection);  // the view turns around the new part
    refreshTree();
    loadInspector();
    refreshScene();
    status(QStringLiteral("Added %1 as %2 stacked levels, one part each.").arg(info.completeBaseName()).arg(placed));
    return;
  }
  if (layers.size() > 1) {
    Node parent;
    parent.id = newId();
    parent.kind = Node::Kind::Group;
    parent.name = info.completeBaseName();
    const QString parentId = parent.id;
    doc.nodes.push_back(std::move(parent));
    for (int layer : layers) {
      const QString layerPath = info.absolutePath() + QDir::separator() + QStringLiteral("layer-%1.svg").arg(layer);
      if (!QFileInfo::exists(layerPath)) continue;
      Node layerGroup;
      layerGroup.id = newId();
      layerGroup.kind = Node::Kind::Group;
      layerGroup.parentId = parentId;
      layerGroup.name = info.completeBaseName() + QStringLiteral(" layer %1").arg(layer);
      const QString layerId = layerGroup.id;
      doc.nodes.push_back(std::move(layerGroup));
      Node node;
      node.id = newId();
      node.parentId = layerId;
      node.kind = Node::Kind::Shape;
      node.shape = QStringLiteral("svg");
      node.meshPath = placedFile(layerPath);
      node.meshSource = source;
      node.meshLayer = layer;
      node.name = info.completeBaseName() + QStringLiteral(" %1").arg(layer);
      node.size[2] = 1.0;
      node.pos[0] = placeX;
      node.pos[1] = placeY;
      node.pos[2] = doc.workplaneZ + 0.5;
      node.color = readSvgShapeColor(layerPath);
      node.pivotBaked = true;
      doc.nodes.push_back(std::move(node));
    }
    standOnWorkplane(parentId, placingDropped ? placingU : 0, placingDropped ? placingV : 0);
    selection = QStringList{parentId};
    view->focusOn(selection);  // the view turns around the new part
    refreshTree();
    loadInspector();
    refreshScene();
    status(QStringLiteral("Added %1 as one group per layer. A hole stays inside its own layer.").arg(info.completeBaseName()));
    return;
  }
  Node node;
  node.id = newId();
  node.kind = Node::Kind::Shape;
  node.shape = QStringLiteral("svg");
  node.meshPath = placedFile(info.absoluteFilePath());
  node.meshSource = source;
  node.meshLayer = 0;
  node.name = info.completeBaseName();
  node.size[2] = 1.0;
  node.pos[0] = placeX;
  node.pos[1] = placeY;
  node.pos[2] = doc.workplaneZ + 0.5;
  node.color = readSvgShapeColor(info.absoluteFilePath());
  node.pivotBaked = true;
  doc.nodes.push_back(std::move(node));
  standOnWorkplane(doc.nodes.back().id, placingDropped ? placingU : 0, placingDropped ? placingV : 0);
  selection = QStringList{doc.nodes.back().id};
  view->focusOn(selection);  // the view turns around the new part
  refreshTree();
  loadInspector();
  refreshScene();
  status(QStringLiteral("Added 2D shape ") + info.completeBaseName() + QStringLiteral(" at 1 mm height."));
}

void MainWindow::chooseObjectColor() {
  Node* first = nullptr;
  for (const QString& id : selection) {
    Node* candidate = doc.find(id);
    if (candidate && candidate->kind != Node::Kind::Pattern) { first = candidate; break; }
  }
  if (!first) return;
  const QColor chosen = QColorDialog::getColor(first->color, this, QStringLiteral("Object color"), QColorDialog::ShowAlphaChannel);
  if (!chosen.isValid()) return;
  pushUndo();
  for (const QString& id : selection) {
    if (Node* node = doc.find(id)) {
      if (node->kind != Node::Kind::Pattern) node->color = chosen;
    }
  }
  loadInspector();
  refreshScene(true);
}

bool pathIsInside(const QString& path, const QString& root) {
  const QString file = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  const QString base = QDir::cleanPath(root);
#if defined(Q_OS_WIN)
  const Qt::CaseSensitivity sense = Qt::CaseInsensitive;
#else
  const Qt::CaseSensitivity sense = Qt::CaseSensitive;
#endif
  if (file.compare(base, sense) == 0) return true;
  return file.startsWith(base + QLatin1Char('/'), sense);
}

QString writeUniqueFile(const QString& directory, const QString& fileName, const QByteArray& data) {
  QDir dir(directory);
  if (!dir.mkpath(QStringLiteral("."))) return {};
  QFileInfo incoming(fileName);
  QString stem = incoming.completeBaseName();
  QString suffix = incoming.suffix();
  if (stem.isEmpty()) stem = QStringLiteral("part");
  if (suffix.isEmpty()) suffix = QStringLiteral("bin");
  stem.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
  for (int n = 1; n < 10000; ++n) {
    const QString name = n == 1 ? stem + QLatin1Char('.') + suffix
                                : QStringLiteral("%1_%2.%3").arg(stem).arg(n).arg(suffix);
    const QString dest = QDir::cleanPath(dir.absoluteFilePath(name));
    if (QFileInfo::exists(dest)) continue;
    QFile out(dest);
    if (!out.open(QIODevice::WriteOnly)) return {};
    if (out.write(data) != data.size()) {
      out.close();
      QFile::remove(dest);
      return {};
    }
    out.close();
    return QFileInfo(dest).absoluteFilePath();
  }
  return {};
}

bool sameFileContent(const QString& a, const QString& b) {
  QFile fa(a);
  QFile fb(b);
  if (!fa.open(QIODevice::ReadOnly) || !fb.open(QIODevice::ReadOnly)) return false;
  while (true) {
    const QByteArray ca = fa.read(1 << 20);
    const QByteArray cb = fb.read(1 << 20);
    if (ca != cb) return false;
    if (ca.isEmpty()) return true;
  }
}

QString copyStlBeside(const QString& sourcePath, const QString& directory) {
  const QFileInfo source(sourcePath);
  if (!source.exists() || !source.isFile()) return {};
  QDir dir(directory);
  if (!dir.mkpath(QStringLiteral("."))) return {};
  const QString sourceAbs = QDir::cleanPath(source.absoluteFilePath());
  if (QDir::cleanPath(source.absolutePath()) == QDir::cleanPath(dir.absolutePath())) return sourceAbs;
  const QString stem = source.completeBaseName().isEmpty() ? QStringLiteral("part") : source.completeBaseName();
  QString suffix = source.suffix();
  if (suffix.isEmpty()) suffix = QStringLiteral("stl");
  for (int n = 1; n < 1000; ++n) {
    const QString name = n == 1 ? stem + QStringLiteral(".") + suffix
                                : QStringLiteral("%1_%2.%3").arg(stem).arg(n).arg(suffix);
    const QString dest = QDir::cleanPath(dir.absoluteFilePath(name));
    if (!QFileInfo::exists(dest)) {
      if (!QFile::copy(sourceAbs, dest)) return {};
      return dest;
    }
    // Reuse an earlier copy only when it is the same file, not just the same size: an edited
    // shape often keeps its byte count.
    if (QFileInfo(dest).size() == source.size() && sameFileContent(sourceAbs, dest)) return dest;
  }
  return {};
}

QString sanitizeProjectName(QString name) {
  name = name.trimmed();
  name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
  name.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
  if (name == QLatin1String(".") || name == QLatin1String("..")) return {};
  return name;
}

void MainWindow::exportSlice() {
  if (slicePicking) return;
  auto* dialog = new QDialog(this);
  dialog->setWindowTitle(QStringLiteral("Export 2D slice"));
  dialog->setModal(false);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  auto* layout = new QVBoxLayout(dialog);
  auto* which = new QLabel(selection.isEmpty() ? QStringLiteral("Slices the whole plate.") : QStringLiteral("Slices the selection."), dialog);
  auto* height = new QRadioButton(QStringLiteral("At height Z"), dialog);
  auto* face = new QRadioButton(QStringLiteral("On a face"), dialog);
  height->setChecked(true);
  auto* zSpin = new QDoubleSpinBox(dialog);
  zSpin->setDecimals(2);
  zSpin->setSuffix(QStringLiteral(" mm"));
  zSpin->setRange(-1000, 1000);
  zSpin->setValue(doc.workplaneZ);
  QString warning;
  double lo = 1e9;
  double hi = -1e9;
  for (const DrawItem& item : buildEditView(doc, selection, &warning)) {
    if (item.hole || item.gizmo) continue;
    const CpuMesh& mesh = meshOf(item.mesh);
    if (!mesh.hasBounds) continue;
    const float corner[2][3] = {{mesh.lo[0], mesh.lo[1], mesh.lo[2]}, {mesh.hi[0], mesh.hi[1], mesh.hi[2]}};
    for (int xi = 0; xi < 2; ++xi) {
      for (int yi = 0; yi < 2; ++yi) {
        for (int zi = 0; zi < 2; ++zi) {
          const double z = item.model.map(QVector3D(corner[xi][0], corner[yi][1], corner[zi][2])).z();
          lo = std::min(lo, z);
          hi = std::max(hi, z);
        }
      }
    }
  }
  if (lo <= hi) {
    zSpin->setRange(lo, hi);
    zSpin->setValue((lo + hi) * 0.5);
  }
  auto* faceLabel = new QLabel(QStringLiteral("Click a face in the view."), dialog);
  faceLabel->setWordWrap(true);
  faceLabel->hide();
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, dialog);
  buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("Export"));
  layout->addWidget(which);
  layout->addWidget(height);
  layout->addWidget(zSpin);
  layout->addWidget(face);
  layout->addWidget(faceLabel);
  layout->addWidget(buttons);
  const int savedTool = tool;
  slicePicking = true;
  sliceHaveFace = false;
  sliceFaceLabel = faceLabel;
  sliceToolRestore = savedTool;
  connect(height, &QRadioButton::toggled, dialog, [zSpin, faceLabel](bool on) {
    zSpin->setEnabled(on);
    faceLabel->setVisible(!on);
  });
  // Clicks in the view pick a face only while "On a face" is chosen; with a height the view works as usual.
  connect(face, &QRadioButton::toggled, this, [this](bool on) {
    if (!slicePicking || !view) return;
    if (on) {
      if (tool != 10) sliceToolRestore = tool;
      tool = 10;
      view->setTool(10);
      status(QStringLiteral("Click a face. Right-drag still orbits."));
    } else if (tool == 10) {
      tool = sliceToolRestore == 10 ? 0 : sliceToolRestore;
      view->setTool(tool);
      syncActions();
    }
  });
  connect(dialog, &QDialog::destroyed, this, [this] {
    slicePicking = false;
    sliceFaceLabel = nullptr;
    sliceHaveFace = false;
    if (tool == 10) {
      tool = sliceToolRestore == 10 ? 0 : sliceToolRestore;
      if (view) view->setTool(tool);
      syncActions();
    }
  });
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
  connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked, this, [this, dialog, height, zSpin] {
    double origin[3] = {0, 0, zSpin->value()};
    double normal[3] = {0, 0, 1};
    if (!height->isChecked()) {
      if (!sliceHaveFace) {
        status(QStringLiteral("Click a face in the view first."));
        return;
      }
      origin[0] = slicePoint.x();
      origin[1] = slicePoint.y();
      origin[2] = slicePoint.z();
      normal[0] = sliceNormal.x();
      normal[1] = sliceNormal.y();
      normal[2] = sliceNormal.z();
    }
    const QStringList only = selection;
    std::vector<std::vector<double>> loops;
    QString error;
    QString note;
    status(QStringLiteral("Slicing…"));
    const Document snapshot = doc;  // the slice runs on another thread; it must not read the live document
    runBusy([&] { loops = sliceContours(snapshot, only, origin, normal, &error, &note); }, true);
    if (loops.empty()) {
      QMessageBox::warning(dialog, QStringLiteral("Export slice"), error.isEmpty() ? QStringLiteral("Nothing on that plane.") : error);
      return;
    }
    QString path = QFileDialog::getSaveFileName(dialog, QStringLiteral("Export 2D slice"), QStringLiteral("daedalcad-slice.dxf"),
                                                QStringLiteral("AutoCAD DXF (*.dxf)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".dxf"), Qt::CaseInsensitive)) path += QStringLiteral(".dxf");
    if (!writeDxfSlice(loops, path, &error)) {
      QMessageBox::warning(dialog, QStringLiteral("Export slice"), error.isEmpty() ? QStringLiteral("Could not write the file.") : error);
      return;
    }
    status((note.isEmpty() ? QString() : note + QStringLiteral(" ")) + QStringLiteral("Exported %1 · %2 outlines.").arg(path).arg(loops.size()));
  });
  dialog->resize(360, 210);
  dialog->show();
  status(QStringLiteral("Slice stays open. Pick a height, or choose On a face and click one, then Export. Holes go on their own layer."));
}

// --- Section view -----------------------------------------------------------------------------------

void MainWindow::setSection(bool on) {
  sectionOn = on;
  for (QAction* action : findChildren<QAction*>())
    if (action->text() == QLatin1String("Section") && action->isChecked() != on) {
      action->blockSignals(true);
      action->setChecked(on);
      action->blockSignals(false);
    }
  if (sectionBar) sectionBar->setVisible(on);
  updateSection(true);
  placeOverlays();
  status(on ? QStringLiteral("Section view: drag the slider to move the cut; Flip keeps the other side. Cut faces are hatched red.")
            : QStringLiteral("Section view off."));
}

// rescan: set the slider's range to the print area along the cut's axis (the plate's width and
// depth, and as high as the plate is wide), widened to take in any part outside it, and start the
// cut through the middle of the parts.
void MainWindow::updateSection(bool rescan) {
  if (!view || !sectionAxisBox) return;
  const int axis = sectionAxisBox->currentIndex();
  if (rescan) {
    QVector3D partsLo, partsHi;
    bool any = false;
    for (const Node& node : doc.nodes) {
      if (!node.parentId.isEmpty()) continue;
      QVector3D a, b;
      if (!alignmentWorldBounds(doc, node, &a, &b)) continue;
      if (!any) {
        partsLo = a;
        partsHi = b;
        any = true;
      }
      for (int i = 0; i < 3; ++i) {
        partsLo[i] = std::min(partsLo[i], a[i]);
        partsHi[i] = std::max(partsHi[i], b[i]);
      }
    }
    const float bx = static_cast<float>(doc.bedX * 0.5), by = static_cast<float>(doc.bedY * 0.5);
    QVector3D lo(-bx, -by, static_cast<float>(std::min(0.0, doc.workplaneZ)));
    QVector3D hi(bx, by, static_cast<float>(std::max(doc.bedX, doc.bedY)));
    if (any) {
      for (int i = 0; i < 3; ++i) {
        lo[i] = std::min(lo[i], partsLo[i]);
        hi[i] = std::max(hi[i], partsHi[i]);
      }
    }
    sectionLo = lo;
    sectionHi = hi;
    const double start = any ? (partsLo[axis] + partsHi[axis]) * 0.5 : 0.0;
    sectionSpin->blockSignals(true);
    sectionSlider->blockSignals(true);
    sectionSpin->setRange(std::floor(lo[axis]), std::ceil(hi[axis]));
    sectionSlider->setRange(static_cast<int>(std::floor(lo[axis] * 10)), static_cast<int>(std::ceil(hi[axis] * 10)));
    sectionSlider->setPageStep(100);  // 10 mm a click beside the handle
    sectionSpin->setValue(start);
    sectionSlider->setValue(static_cast<int>(std::round(start * 10)));
    sectionSlider->blockSignals(false);
    sectionSpin->blockSignals(false);
    sectionSlider->setToolTip(QStringLiteral("Drag the cut through the print area: %1 to %2 mm.").arg(lo[axis], 0, 'f', 0).arg(hi[axis], 0, 'f', 0));
  }
  view->setSection(sectionOn, axis, sectionSpin->value(), sectionFlipButton->isChecked(), sectionLo, sectionHi);
}

// --- Lay flat and the workplane on a face -----------------------------------------------------------

void MainWindow::showFaceTool(int which) {
  if (!view) return;
  if (tool == which) {
    tool = 0;
    panel = 0;
    view->setTool(0);
    syncActions();
    loadInspector();
    status(which == 12 ? QStringLiteral("Lay flat closed.") : QStringLiteral("Workplane tool closed."));
    return;
  }
  if (alignPopup) alignPopup->hide();
  if (repeatPopup) repeatPopup->hide();
  if (filletPopup) filletPopup->hide();
  if (faceFilletPopup) faceFilletPopup->hide();
  if (edgeBetaPopup) edgeBetaPopup->hide();
  tool = which;
  panel = 0;
  view->setTool(which);
  syncActions();
  loadInspector();
  status(which == 12 ? QStringLiteral("Lay flat: click the face of a part that should lie on the plate.")
                     : QStringLiteral("Workplane: click a face and new parts stand on it. Click the empty plate to go back to it."));
}

void MainWindow::layFlat(const QString& id, QVector3D normal) {
  // The whole object the face belongs to turns, about its middle, the short way round.
  Node* node = doc.find(id);
  int guard = 0;
  while (node && !node->parentId.isEmpty() && guard++ < 64) node = doc.find(node->parentId);
  if (!node || normal.lengthSquared() < 1e-8f) return;
  const QVector3D down(0, 0, -1);
  if (QVector3D::dotProduct(normal.normalized(), down) > 0.99999f) {
    status(QStringLiteral("That face already lies flat."));
    return;
  }
  pushUndo();
  QVector3D lo, hi;
  const QVector3D middle = alignmentWorldBounds(doc, *node, &lo, &hi) ? (lo + hi) * 0.5f : QVector3D();
  QMatrix4x4 turn;
  turn.translate(middle);
  turn.rotate(QQuaternion::rotationTo(normal.normalized(), down));
  turn.translate(-middle);
  const double zero[3] = {0, 0, 0};
  const double keepScale[3] = {node->scale[0], node->scale[1], node->scale[2]};
  const double keepRot[3] = {node->rot[0], node->rot[1], node->rot[2]};
  decomposePlacementNear(turn * placementMatrix(node->pos, node->rot, zero, node->scale), keepRot, node->pos, node->rot, node->scale);
  for (int i = 0; i < 3; ++i) node->scale[i] = keepScale[i];
  // Then down onto the plate, by its real lowest point (a turned box is not its bounding box).
  {
    Document shapeOnly = doc;
    if (Node* self = shapeOnly.find(node->id)) self->hole = false;
    std::vector<double> verts;
    std::vector<uint64_t> tris;
    double bottom = 1e300;
    if (worldMesh64(shapeOnly, QStringList{node->id}, &verts, &tris))
      for (size_t i = 2; i < verts.size(); i += 3) bottom = std::min(bottom, verts[i]);
    if (bottom < 1e299) node->pos[2] += doc.workplaneZ - bottom;
  }
  tidyPlacement(*node);
  selection = QStringList{node->id};
  refreshTree();
  loadInspector();
  refreshScene();
  status(QStringLiteral("%1 now lies on that face. Click another face, or press Lay flat again to stop.").arg(node->name));
}

void MainWindow::setWorkplaneOnFace(QVector3D centre, QVector3D normal) {
  normal.normalize();
  // Upright on a wall: the plane's Y follows the world's up. Flat faces keep the world's X.
  QVector3D x, y;
  const QVector3D Z(0, 0, 1), X(1, 0, 0);
  if (std::abs(QVector3D::dotProduct(normal, Z)) < 0.99f) {
    y = (Z - normal * QVector3D::dotProduct(normal, Z)).normalized();
    x = QVector3D::crossProduct(y, normal);
  } else {
    x = (X - normal * QVector3D::dotProduct(normal, X)).normalized();
    y = QVector3D::crossProduct(normal, x);
  }
  doc.faceWorkplane = true;
  for (int i = 0; i < 3; ++i) {
    doc.workplaneOrigin[i] = centre[i];
    doc.workplaneNormal[i] = normal[i];
    doc.workplaneX[i] = x[i];
  }
  if (view) view->setFaceWorkplane(true, centre, normal, x);
  tool = 0;
  if (view) view->setTool(0);
  syncActions();
  loadInspector();
  status(QStringLiteral("Workplane on that face: new parts stand on it. Workplane, then a click on the empty plate, goes back to the plate."));
}

void MainWindow::clearFaceWorkplane() {
  const bool was = doc.faceWorkplane;
  doc.faceWorkplane = false;
  if (view) view->setFaceWorkplane(false, {}, {0, 0, 1}, {1, 0, 0});
  tool = 0;
  if (view) view->setTool(0);
  syncActions();
  loadInspector();
  status(was ? QStringLiteral("Workplane back on the plate.") : QStringLiteral("The workplane is on the plate."));
}

void MainWindow::standOnWorkplane(const QString& id, double u, double v) {
  Node* node = doc.find(id);
  if (!node || !doc.faceWorkplane || !node->parentId.isEmpty()) return;
  double lo[3], hi[3];
  if (!localBounds(doc, *node, lo, hi)) return;
  const QVector3D origin(doc.workplaneOrigin[0], doc.workplaneOrigin[1], doc.workplaneOrigin[2]);
  const QVector3D n = QVector3D(doc.workplaneNormal[0], doc.workplaneNormal[1], doc.workplaneNormal[2]).normalized();
  const QVector3D x = QVector3D(doc.workplaneX[0], doc.workplaneX[1], doc.workplaneX[2]).normalized();
  const QVector3D y = QVector3D::crossProduct(n, x);
  if (doc.snapOn && doc.snap > 0) {
    u = std::round(u / doc.snap) * doc.snap;
    v = std::round(v / doc.snap) * doc.snap;
  }
  // The part's own X, Y and up become the plane's; the middle of its underside goes on the spot.
  QMatrix4x4 frame(x.x(), y.x(), n.x(), 0, x.y(), y.y(), n.y(), 0, x.z(), y.z(), n.z(), 0, 0, 0, 0, 1);
  const double zero[3] = {0, 0, 0};
  const double unit[3] = {1, 1, 1};
  double ignored[3], rot[3], scale[3];
  decomposePlacement(frame * placementMatrix(zero, node->rot, zero, unit), ignored, rot, scale);
  for (int i = 0; i < 3; ++i) node->rot[i] = rot[i];
  const QVector3D base(static_cast<float>((lo[0] + hi[0]) * 0.5 * node->scale[0]), static_cast<float>((lo[1] + hi[1]) * 0.5 * node->scale[1]),
                       static_cast<float>(lo[2] * node->scale[2]));
  const QVector3D at = origin + x * static_cast<float>(u) + y * static_cast<float>(v);
  const QVector3D pos = at - placementMatrix(zero, node->rot, zero, unit).map(base);
  node->pos[0] = pos.x();
  node->pos[1] = pos.y();
  node->pos[2] = pos.z();
  tidyPlacement(*node);
}

// A sheet with front, top and side views, dimensions and a filament estimate of the selection (or of
// everything when nothing is selected).
void MainWindow::openDrawing() {
  const QStringList only = selection;
  const Document snapshot = doc;  // built on another thread; it must not read the live document
  drawing::Model model;
  QString error;
  status(QStringLiteral("Making the drawing…"));
  runBusy([&] {
    const CpuMesh mesh = evaluateSolid(snapshot, only, &error);
    model = drawing::analyse(mesh);
  }, true);
  if (!model.valid) {
    QMessageBox::warning(this, QStringLiteral("Technical drawing"), error.isEmpty() ? QStringLiteral("There is nothing to draw.") : error);
    status(QString());
    return;
  }
  QString project = projectDirectory.isEmpty() ? QString() : QFileInfo(projectDirectory).fileName();
  QString title, subject;
  if (only.size() == 1 && doc.find(only.front())) {
    title = doc.find(only.front())->name;
    subject = title;
  } else if (!only.isEmpty()) {
    title = project.isEmpty() ? QStringLiteral("Assembly") : project;
    subject = QStringLiteral("%1 selected parts").arg(only.size());
  } else {
    title = project.isEmpty() ? QStringLiteral("Untitled") : project;
    subject = QStringLiteral("everything on the plate");
  }
  const QString folder = projectDirectory.isEmpty() ? defaultAppdataRoot() : projectDirectory;
  auto* dialog = new DrawingDialog(this, std::move(model), title, subject, folder);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->show();
  status(QStringLiteral("Drawing ready. Change the paper, scale and filament on the right; save it as PDF, SVG, DXF or PNG."));
}

// The STEP export window: a progress bar and a log of what became of every part (exact, partly
// exact, or from its mesh, and why). Stays open after the export until closed.
namespace {
QString stepLogHtml(const QString& line) {
  QString html = line.toHtmlEscaped();
  html.replace(QLatin1Char(' '), QStringLiteral("&nbsp;"));
  auto mark = [&](const QString& word, const char* colour) {
    html.replace(word, QStringLiteral("<span style='color:%1;font-weight:bold'>%2</span>").arg(QLatin1String(colour), word));
  };
  mark(QStringLiteral("PARTLY&nbsp;EXACT"), "#e3b341");
  if (!html.contains(QStringLiteral("PARTLY"))) mark(QStringLiteral("EXACT"), "#56d364");
  mark(QStringLiteral("FROM&nbsp;THE&nbsp;MESH"), "#f0883e");
  if (line.startsWith(QStringLiteral("      "))) html = QStringLiteral("<span style='color:#9aa7b2'>%1</span>").arg(html);
  if (line.startsWith(QStringLiteral("Export failed")) || line.startsWith(QStringLiteral("Not written")))
    html = QStringLiteral("<span style='color:#ff7b72'>%1</span>").arg(html);
  return html;
}
}  // namespace

void MainWindow::appendStepLogLine(QPlainTextEdit* text, const QString& line) {
  text->appendHtml(stepLogHtml(line));
}

MainWindow::StepLog MainWindow::openStepLog(const QString& path) {
  StepLog log;
  QSettings settings;
  const bool onlyProblems = settings.value(QStringLiteral("stepLogOnlyProblems"), false).toBool();
  auto* dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(QStringLiteral("STEP export"));
  dialog->resize(760, 460);
  auto* layout = new QVBoxLayout(dialog);
  auto* title = new QLabel(QStringLiteral("Exporting <b>%1</b>").arg(QFileInfo(path).fileName().toHtmlEscaped()), dialog);
  layout->addWidget(title);
  auto* bar = new QProgressBar(dialog);
  bar->setRange(0, 0);
  bar->setTextVisible(true);
  layout->addWidget(bar);
  auto* text = new QPlainTextEdit(dialog);
  text->setReadOnly(true);
  text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  mono.setPointSizeF(std::max(9.0, mono.pointSizeF()));
  text->setFont(mono);
  text->setStyleSheet(QStringLiteral("QPlainTextEdit { background: #0d1117; color: #d6dde6; border: 1px solid #30363d; }"));
  layout->addWidget(text, 1);
  auto* row = new QHBoxLayout;
  auto* quietBox = new QCheckBox(QStringLiteral("Only open this window when a part is not exact"), dialog);
  quietBox->setChecked(onlyProblems);
  connect(quietBox, &QCheckBox::toggled, dialog,
          [](bool on) { QSettings().setValue(QStringLiteral("stepLogOnlyProblems"), on); });
  row->addWidget(quietBox);
  row->addStretch(1);
  auto* copy = new QPushButton(QStringLiteral("Copy log"), dialog);
  connect(copy, &QPushButton::clicked, dialog, [text] { QGuiApplication::clipboard()->setText(text->toPlainText()); });
  row->addWidget(copy);
  auto* close = new QPushButton(QStringLiteral("Close"), dialog);
  close->setEnabled(false);
  connect(close, &QPushButton::clicked, dialog, &QDialog::close);
  row->addWidget(close);
  layout->addLayout(row);
  log.dialog = dialog;
  log.bar = bar;
  log.text = text;
  log.title = title;
  log.close = close;
  if (!onlyProblems) dialog->show();
  return log;
}

void MainWindow::finishStepLog(const StepLog& log, bool ok, const QString& error, const exact::Report& report) {
  if (!log.dialog) return;
  if (log.bar) {
    log.bar->setRange(0, 1);
    log.bar->setValue(1);
  }
  if (!ok && log.text) appendStepLogLine(log.text, QStringLiteral("Export failed: %1").arg(error));
  if (log.title) {
    log.title->setText(ok ? QStringLiteral("Exported <b>%1</b>").arg(log.title->text().section(QStringLiteral("<b>"), 1).section(QStringLiteral("</b>"), 0, 0))
                          : QStringLiteral("<span style='color:#ff7b72'>The export failed.</span>"));
  }
  if (log.close) {
    log.close->setEnabled(true);
    log.close->setDefault(true);
    log.close->setFocus();
  }
  const bool problems = !ok || report.partly > 0 || report.faceted > 0;
  if (!log.dialog->isVisible()) {
    if (problems) log.dialog->show();
    else log.dialog->deleteLater();
  }
}

void MainWindow::exportMesh(bool selectedOnly, const QString& extension) {
  if (selectedOnly && selection.isEmpty()) {
    status("Select an object first.");
    return;
  }
  const QString title = selectedOnly ? QStringLiteral("Export selected object") : QStringLiteral("Export");
  QString filter = QStringLiteral("STL (*.stl)");
  if (extension == QLatin1String("obj")) filter = QStringLiteral("Wavefront OBJ (*.obj)");
  else if (extension == QLatin1String("3mf")) filter = QStringLiteral("3MF (*.3mf)");
  else if (extension == QLatin1String("dxf")) filter = QStringLiteral("AutoCAD DXF (*.dxf)");
  else if (extension == QLatin1String("step")) filter = QStringLiteral("STEP (*.step *.stp)");
  QString path = QFileDialog::getSaveFileName(this, title, "daedalcad." + extension, filter);
  if (path.isEmpty()) return;
  if (!path.endsWith("." + extension, Qt::CaseInsensitive) && !(extension == QLatin1String("step") && path.endsWith(".stp", Qt::CaseInsensitive)))
    path += "." + extension;
  exportMeshTo(selectedOnly, extension, path);
}

void MainWindow::exportMeshTo(bool selectedOnly, const QString& extension, const QString& path) {
  status("Building the printable solid…");
  Document snapshot = doc;
  const QStringList only = selectedOnly ? selection : QStringList();
  QString error;
  // STL holds one mesh, so everything is merged. OBJ, 3MF, DXF and STEP keep every part (a part on the
  // plate, or a whole group) as its own object, named after it; loose holes still cut them.
  if (extension == QLatin1String("stl")) {
    CpuMesh mesh;
    runBusy([&] { mesh = evaluateSolid(snapshot, only, &error); }, true);
    if (mesh.triangles <= 0) {
      status(QStringLiteral("Nothing exported."));
      QMessageBox::warning(this, "Export", error.isEmpty() ? "Nothing to export." : error);
      return;
    }
    if (!writeStl(mesh, path, &error)) {
      status(QStringLiteral("Export failed: %1").arg(error));
      QMessageBox::warning(this, "Export", error);
      return;
    }
    status(QString("Exported %1 · %2 triangles · %3 cm³.")
               .arg(path)
               .arg(mesh.triangles)
               .arg(mesh.volume / 1000.0, 0, 'f', 2));
    return;
  }
  std::vector<ExportObject> objects;
  runBusy([&] { objects = evaluateObjects(snapshot, only, &error); }, true);
  if (objects.empty()) {
    status(QStringLiteral("Nothing exported."));
    QMessageBox::warning(this, "Export", error.isEmpty() ? "Nothing to export." : error);
    return;
  }
  bool ok = false;
  step::Stats stepStats;
  exact::Report exactReport;
  StepLog stepLog;
  if (extension == "step" && exact::available()) {
    stepLog = openStepLog(path);
    const QPointer<QProgressBar> bar = stepLog.bar;
    const QPointer<QPlainTextEdit> text = stepLog.text;
    exactReport.progress = [bar, text](int done, int total, const QString& line) {
      QMetaObject::invokeMethod(
          qApp,
          [bar, text, done, total, line] {
            if (bar) {
              bar->setRange(0, std::max(1, total));
              bar->setValue(done);
            }
            if (text && !line.isEmpty()) appendStepLogLine(text, line);
          },
          Qt::QueuedConnection);
    };
    runBusy([&] { ok = exact::writeStep(snapshot, only, objects, path, &error, &exactReport); }, true);
    QApplication::processEvents();  // the last queued lines
    finishStepLog(stepLog, ok, error, exactReport);
  }
  else if (extension == "step") runBusy([&] { ok = writeStep(objects, path, &error, &stepStats); }, true);
  else if (extension == "obj") ok = writeObj(objects, path, &error);
  else if (extension == "3mf") ok = write3mf(objects, path, &error);
  else ok = writeDxf(objects, path, &error);
  if (!ok) {
    status(QStringLiteral("Export failed: %1").arg(error));
    if (!stepLog.dialog) QMessageBox::warning(this, "Export", error);
    return;
  }
  int triangles = 0;
  double volume = 0;
  for (const ExportObject& object : objects) {
    triangles += object.mesh.triangles;
    volume += object.mesh.volume;
  }
  if (extension == "step" && exact::available()) {
    QStringList parts;
    if (exactReport.exact) parts << QStringLiteral("%1 exact").arg(exactReport.exact);
    if (exactReport.partly) parts << QStringLiteral("%1 partly exact (some pieces from the mesh)").arg(exactReport.partly);
    if (exactReport.faceted) parts << QStringLiteral("%1 from the mesh (flat faces)").arg(exactReport.faceted);
    status(QStringLiteral("Exported %1 · %2 · %3 cm³.%4")
               .arg(path, parts.join(QStringLiteral(", ")))
               .arg(volume / 1000.0, 0, 'f', 2)
               .arg(exactReport.notes.isEmpty() ? QString() : QStringLiteral(" ") + exactReport.notes.join(QLatin1Char(' '))));
    return;
  }
  if (extension == "step") {
    status(QStringLiteral("Exported %1 · %2 %3 · %4 flat faces · %5 cm³. Round surfaces are made of narrow flat faces.")
               .arg(path)
               .arg(objects.size())
               .arg(objects.size() == 1 ? QStringLiteral("solid") : QStringLiteral("solids"))
               .arg(stepStats.faces)
               .arg(volume / 1000.0, 0, 'f', 2));
    return;
  }
  status(QString("Exported %1 · %2 %3 · %4 triangles · %5 cm³.")
             .arg(path)
             .arg(objects.size())
             .arg(objects.size() == 1 ? QStringLiteral("object") : QStringLiteral("objects"))
             .arg(triangles)
             .arg(volume / 1000.0, 0, 'f', 2));
}

namespace {
quint32 zipCrc32(const QByteArray& data) {
  quint32 crc = 0xffffffffu;
  for (char raw : data) {
    crc ^= static_cast<unsigned char>(raw);
    for (int bit = 0; bit < 8; ++bit) crc = (crc & 1u) ? (0xedb88320u ^ (crc >> 1)) : (crc >> 1);
  }
  return crc ^ 0xffffffffu;
}
void write16(QIODevice& file, quint16 value) {
  const char bytes[2] = {static_cast<char>(value & 255), static_cast<char>((value >> 8) & 255)};
  file.write(bytes, 2);
}
void write32(QIODevice& file, quint32 value) {
  const char bytes[4] = {static_cast<char>(value & 255), static_cast<char>((value >> 8) & 255),
                         static_cast<char>((value >> 16) & 255), static_cast<char>((value >> 24) & 255)};
  file.write(bytes, 4);
}
struct ZipBlob {
  QString name;
  QByteArray data;
};
quint16 read16(const QByteArray& bytes, int offset) {
  const auto* raw = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
  return static_cast<quint16>(raw[0] | (raw[1] << 8));
}
quint32 read32(const QByteArray& bytes, int offset) {
  const auto* raw = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
  return static_cast<quint32>(raw[0] | (raw[1] << 8) | (raw[2] << 16) | (raw[3] << 24));
}
bool writeStoredZipTo(QIODevice& out, const std::vector<ZipBlob>& files, QString* error) {
  if (files.empty()) {
    if (error) *error = QStringLiteral("The project has nothing to export.");
    return false;
  }
  if (!out.isOpen() || !out.isWritable()) {
    if (error) *error = QStringLiteral("Could not write the project.");
    return false;
  }
  const QDateTime now = QDateTime::currentDateTime();
  const quint16 dosTime = static_cast<quint16>((now.time().hour() << 11) | (now.time().minute() << 5) | (now.time().second() / 2));
  const quint16 dosDate = static_cast<quint16>(((now.date().year() - 1980) << 9) | (now.date().month() << 5) | now.date().day());
  struct Entry {
    QByteArray name;
    quint32 crc = 0;
    quint32 size = 0;
    quint32 offset = 0;
  };
  std::vector<Entry> entries;
  for (const ZipBlob& file : files) {
    if (static_cast<quint64>(file.data.size()) > 0xffffffffu) {
      if (error) *error = QStringLiteral("“%1” is too large to store.").arg(file.name);
      return false;
    }
    Entry entry;
    entry.name = file.name.toUtf8();
    entry.crc = zipCrc32(file.data);
    entry.size = static_cast<quint32>(file.data.size());
    entry.offset = static_cast<quint32>(out.pos());
    write32(out, 0x04034b50u);
    write16(out, 20);
    write16(out, 0);
    write16(out, 0);
    write16(out, dosTime);
    write16(out, dosDate);
    write32(out, entry.crc);
    write32(out, entry.size);
    write32(out, entry.size);
    write16(out, static_cast<quint16>(entry.name.size()));
    write16(out, 0);
    out.write(entry.name);
    out.write(file.data);
    entries.push_back(entry);
  }
  const quint32 centralOffset = static_cast<quint32>(out.pos());
  for (const Entry& entry : entries) {
    write32(out, 0x02014b50u);
    write16(out, 20);
    write16(out, 20);
    write16(out, 0);
    write16(out, 0);
    write16(out, dosTime);
    write16(out, dosDate);
    write32(out, entry.crc);
    write32(out, entry.size);
    write32(out, entry.size);
    write16(out, static_cast<quint16>(entry.name.size()));
    write16(out, 0);
    write16(out, 0);
    write16(out, 0);
    write16(out, 0);
    write16(out, 0);
    write32(out, 0);
    write32(out, entry.offset);
    out.write(entry.name);
  }
  const quint32 centralSize = static_cast<quint32>(out.pos()) - centralOffset;
  write32(out, 0x06054b50u);
  write16(out, 0);
  write16(out, 0);
  write16(out, static_cast<quint16>(entries.size()));
  write16(out, static_cast<quint16>(entries.size()));
  write32(out, centralSize);
  write32(out, centralOffset);
  write16(out, 0);
  return true;
}
bool writeStoredZip(const QString& zipPath, const std::vector<ZipBlob>& files, QString* error) {
  QFile out(zipPath);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (error) *error = out.errorString();
    return false;
  }
  return writeStoredZipTo(out, files, error);
}
bool writeStoredZipBytes(const std::vector<ZipBlob>& files, QByteArray* out, QString* error) {
  if (!out) return false;
  out->clear();
  QBuffer buffer(out);
  if (!buffer.open(QIODevice::WriteOnly)) {
    if (error) *error = QStringLiteral("Could not write the project.");
    return false;
  }
  return writeStoredZipTo(buffer, files, error);
}
bool readStoredZip(const QString& zipPath, std::vector<ZipBlob>* files, QString* error) {
  QFile in(zipPath);
  if (!in.open(QIODevice::ReadOnly)) {
    if (error) *error = in.errorString();
    return false;
  }
  while (!in.atEnd()) {
    const QByteArray signature = in.read(4);
    if (signature.size() < 4) break;
    const quint32 magic = read32(signature, 0);
    if (magic == 0x02014b50u || magic == 0x06054b50u) break;
    if (magic != 0x04034b50u) {
      if (error) *error = QStringLiteral("That file is not a DaedalCAD project.");
      return false;
    }
    const QByteArray header = in.read(26);
    if (header.size() < 26) {
      if (error) *error = QStringLiteral("The project archive is incomplete.");
      return false;
    }
    const quint16 flags = read16(header, 2);
    const quint16 method = read16(header, 4);
    const quint32 compressed = read32(header, 14);
    const quint16 nameLength = read16(header, 22);
    const quint16 extraLength = read16(header, 24);
    const QByteArray nameBytes = in.read(nameLength);
    if (nameBytes.size() != nameLength || in.read(extraLength).size() != extraLength) {
      if (error) *error = QStringLiteral("The project archive is incomplete.");
      return false;
    }
    const QByteArray data = in.read(compressed);
    if (static_cast<quint32>(data.size()) != compressed) {
      if (error) *error = QStringLiteral("The project archive is incomplete.");
      return false;
    }
    if ((flags & 0x8u) != 0 || method != 0) {
      if (error) *error = QStringLiteral("This archive is compressed. Export it again from DaedalCAD.");
      return false;
    }
    const QString name = QString::fromUtf8(nameBytes);
    if (name.endsWith(QLatin1Char('/'))) continue;
    files->push_back(ZipBlob{name, data});
  }
  if (files->empty()) {
    if (error) *error = QStringLiteral("The project archive is empty.");
    return false;
  }
  return true;
}
}  // namespace

bool MainWindow::saveDocument() {
  const QString projects = defaultAppdataPath(QStringLiteral("projects"));
  QDir().mkpath(projects);
  QString directory = projectDirectory;
  if (directory.isEmpty()) {
    const QString name = sanitizeProjectName(QInputDialog::getText(this, QStringLiteral("Save project"),
                                                                    QStringLiteral("Project name"), QLineEdit::Normal,
                                                                    QStringLiteral("Project")));
    if (name.isEmpty()) return false;
    directory = QDir(projects).absoluteFilePath(name);
    if (!projectFileIn(directory).isEmpty()) {
      if (QMessageBox::question(this, QStringLiteral("Save project"),
                                QStringLiteral("“%1” already exists. Overwrite its project file and its copied STL and SVG files?")
                                    .arg(name)) != QMessageBox::Yes)
        return false;
    }
  }
  if (!QDir().mkpath(directory)) {
    QMessageBox::warning(this, QStringLiteral("Save"), QStringLiteral("Could not create the project folder."));
    return false;
  }
  const QString importedRoot = defaultAppdataPath(QStringLiteral("Imported"));
  QStringList missing;
  for (Node& node : doc.nodes) {
    const bool mesh = node.shape == QLatin1String("stl") || node.shape == QLatin1String("svg");
    if (!mesh || node.meshPath.isEmpty()) continue;
    if (pathIsInside(node.meshPath, importedRoot)) continue;
    const QString copied = copyStlBeside(node.meshPath, directory);
    if (copied.isEmpty()) {
      missing << node.name;
      continue;
    }
    node.meshPath = copied;
  }
  QJsonObject json = toJson(doc);
  QJsonArray nodes = json["nodes"].toArray();
  for (int i = 0; i < nodes.size(); ++i) {
    QJsonObject obj = nodes[i].toObject();
    const QString shape = obj["shape"].toString();
    if (shape != QLatin1String("stl") && shape != QLatin1String("svg")) continue;
    const QString path = obj["meshPath"].toString();
    if (path.isEmpty()) continue;
    if (pathIsInside(path, importedRoot)) {
      obj["meshPath"] = QFileInfo(path).absoluteFilePath();
    } else if (pathIsInside(path, directory)) {
      obj["meshPath"] = QFileInfo(path).fileName();
    }
    nodes[i] = obj;
  }
  json["nodes"] = nodes;
  const QString filePath = QDir(directory).absoluteFilePath(projectFileName());
  // Written next to the old file and swapped in when complete: a crash or a full disk halfway
  // through leaves the last good save in place instead of half a project.
  QSaveFile file(filePath);
  const QByteArray data = QJsonDocument(json).toJson(QJsonDocument::Indented);
  if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
    QMessageBox::warning(this, "Save", QStringLiteral("Could not save the project: %1").arg(file.errorString()));
    return false;
  }
  dropLegacyProjectFile(directory);
  projectDirectory = QDir(directory).absolutePath();
  rememberProject(filePath);
  markSaved();
  if (missing.isEmpty())
    status(QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(filePath)));
  else
    status(QStringLiteral("Saved %1. Could not copy: %2").arg(QDir::toNativeSeparators(filePath), missing.join(", ")));
  return true;
}

QString resolveMeshFile(const QString& meshPath, const QString& projectDir) {
  const QFileInfo info(meshPath);
  if (info.isAbsolute() && info.exists() && info.isFile()) return info.absoluteFilePath();
  if (!projectDir.isEmpty()) {
    const QString beside = QFileInfo(QDir(projectDir).absoluteFilePath(info.fileName())).absoluteFilePath();
    if (QFileInfo::exists(beside) && QFileInfo(beside).isFile()) return beside;
    const QString relative = QFileInfo(QDir(projectDir).absoluteFilePath(meshPath)).absoluteFilePath();
    if (QFileInfo::exists(relative) && QFileInfo(relative).isFile()) return relative;
  }
  if (info.exists() && info.isFile()) return info.absoluteFilePath();
  return {};
}

void MainWindow::exportProjectZip(bool selectedOnly) {
  if (selectedOnly && selection.isEmpty()) {
    status(QStringLiteral("Select an object first."));
    return;
  }
  if (!saveDocument()) return;
  const QString folder = QDir::cleanPath(projectDirectory);
  if (folder.isEmpty() || projectFileIn(folder).isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Export project"), QStringLiteral("Save the project before exporting it."));
    return;
  }
  Document exported = doc;
  if (selectedOnly) {
    QStringList keep = selection;
    for (int i = 0; i < keep.size(); ++i) {
      for (const Node* child : doc.children(keep.at(i)))
        if (!keep.contains(child->id)) keep << child->id;
    }
    std::vector<Node> kept;
    kept.reserve(keep.size());
    for (const Node& node : doc.nodes) {
      if (!keep.contains(node.id)) continue;
      Node copy = node;
      if (!copy.parentId.isEmpty() && !keep.contains(copy.parentId)) {
        const QMatrix4x4 world = nodeWorld(doc, copy.id);
        decomposePlacement(world, copy.pos, copy.rot, copy.scale);
        copy.parentId.clear();
      }
      kept.push_back(std::move(copy));
    }
    if (kept.empty()) {
      status(QStringLiteral("Select an object first."));
      return;
    }
    exported.nodes = std::move(kept);
  }
  const QString name = QFileInfo(folder).fileName();
  const QString projects = defaultAppdataPath(QStringLiteral("projects"));
  const QString title = selectedOnly ? QStringLiteral("Export selected object") : QStringLiteral("Export");
  QString path = QFileDialog::getSaveFileName(this, title,
                                              QDir(projects).absoluteFilePath(name + QLatin1Char('.') + projectArchiveSuffix()),
                                              QStringLiteral("DaedalCAD project (*.%1)").arg(projectArchiveSuffix()));
  if (path.isEmpty()) return;
  if (!path.endsWith(QLatin1Char('.') + projectArchiveSuffix(), Qt::CaseInsensitive)) path += QLatin1Char('.') + projectArchiveSuffix();

  QJsonObject json = toJson(exported);
  QJsonArray nodes = json["nodes"].toArray();
  QHash<QString, QString> packed;
  QStringList usedNames;
  QStringList missing;
  auto claim = [&](const QString& source) {
    const QString key = QDir::cleanPath(source);
    if (packed.contains(key)) return packed.value(key);
    const QFileInfo info(source);
    QString stem = info.completeBaseName().isEmpty() ? QStringLiteral("part") : info.completeBaseName();
    QString suffix = info.suffix().isEmpty() ? QStringLiteral("bin") : info.suffix();
    QString entryName;
    for (int n = 1; n < 10000; ++n) {
      entryName = n == 1 ? stem + QLatin1Char('.') + suffix : QStringLiteral("%1_%2.%3").arg(stem).arg(n).arg(suffix);
      bool taken = false;
      for (const QString& have : usedNames) {
        if (have.compare(entryName, Qt::CaseInsensitive) == 0) taken = true;
      }
      if (!taken) break;
    }
    usedNames << entryName;
    packed.insert(key, entryName);
    return entryName;
  };
  for (int i = 0; i < nodes.size(); ++i) {
    QJsonObject obj = nodes[i].toObject();
    const QString shape = obj["shape"].toString();
    if (shape != QLatin1String("stl") && shape != QLatin1String("svg")) continue;
    const QString meshPath = obj["meshPath"].toString();
    if (meshPath.isEmpty()) continue;
    const QString source = resolveMeshFile(meshPath, folder);
    if (source.isEmpty()) {
      missing << obj["name"].toString();
      continue;
    }
    obj["meshPath"] = claim(source);
    nodes[i] = obj;
  }
  json["nodes"] = nodes;
  std::vector<ZipBlob> files;
  files.push_back(ZipBlob{projectFileName(), QJsonDocument(json).toJson(QJsonDocument::Indented)});
  for (auto it = packed.cbegin(); it != packed.cend(); ++it) {
    QFile input(it.key());
    if (!input.open(QIODevice::ReadOnly)) {
      QMessageBox::warning(this, QStringLiteral("Export project"), QStringLiteral("Could not read %1.").arg(it.key()));
      return;
    }
    files.push_back(ZipBlob{it.value(), input.readAll()});
  }
  QString error;
  if (!writeStoredZip(path, files, &error)) {
    QMessageBox::warning(this, QStringLiteral("Export project"), error.isEmpty() ? QStringLiteral("Could not write the project.") : error);
    return;
  }
  if (missing.isEmpty())
    status(QStringLiteral("Exported %1").arg(QDir::toNativeSeparators(path)));
  else
    status(QStringLiteral("Exported %1. Could not pack: %2").arg(QDir::toNativeSeparators(path), missing.join(", ")));
}

QByteArray packProjectFolder(const QString& folder, QStringList* missing, QString* error) {
  QFile file(projectFileIn(folder));
  if (!file.open(QIODevice::ReadOnly)) {
    if (error) *error = QStringLiteral("That folder has no project file.");
    return {};
  }
  const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll());
  if (!parsed.isObject()) {
    if (error) *error = QStringLiteral("That folder is not a DaedalCAD project.");
    return {};
  }
  QJsonObject json = parsed.object();
  QJsonArray nodes = json[QStringLiteral("nodes")].toArray();
  QHash<QString, QString> packed;
  QStringList usedNames;
  auto claim = [&](const QString& source) {
    const QString key = QDir::cleanPath(source);
    if (packed.contains(key)) return packed.value(key);
    const QFileInfo info(source);
    QString stem = info.completeBaseName().isEmpty() ? QStringLiteral("part") : info.completeBaseName();
    QString suffix = info.suffix().isEmpty() ? QStringLiteral("bin") : info.suffix();
    QString entryName;
    for (int n = 1; n < 10000; ++n) {
      entryName = n == 1 ? stem + QLatin1Char('.') + suffix : QStringLiteral("%1_%2.%3").arg(stem).arg(n).arg(suffix);
      bool taken = false;
      for (const QString& have : usedNames) {
        if (have.compare(entryName, Qt::CaseInsensitive) == 0) taken = true;
      }
      if (!taken) break;
    }
    usedNames << entryName;
    packed.insert(key, entryName);
    return entryName;
  };
  for (int i = 0; i < nodes.size(); ++i) {
    QJsonObject obj = nodes[i].toObject();
    const QString shape = obj[QStringLiteral("shape")].toString();
    if (shape != QLatin1String("stl") && shape != QLatin1String("svg")) continue;
    const QString meshPath = obj[QStringLiteral("meshPath")].toString();
    if (meshPath.isEmpty()) continue;
    const QString source = resolveMeshFile(meshPath, folder);
    if (source.isEmpty()) {
      if (missing) *missing << obj[QStringLiteral("name")].toString();
      continue;
    }
    obj[QStringLiteral("meshPath")] = claim(source);
    nodes[i] = obj;
  }
  json[QStringLiteral("nodes")] = nodes;
  std::vector<ZipBlob> files;
  files.push_back(ZipBlob{projectFileName(), QJsonDocument(json).toJson(QJsonDocument::Indented)});
  for (auto it = packed.cbegin(); it != packed.cend(); ++it) {
    QFile input(it.key());
    if (!input.open(QIODevice::ReadOnly)) {
      if (error) *error = QStringLiteral("Could not read %1.").arg(it.key());
      return {};
    }
    files.push_back(ZipBlob{it.value(), input.readAll()});
  }
  QByteArray bytes;
  if (!writeStoredZipBytes(files, &bytes, error)) return {};
  return bytes;
}

bool MainWindow::installProjectZip(const QString& path, const QString& forcedName, bool openAfter, bool askOverwrite, QString* error) {
  const QString projects = defaultAppdataPath(QStringLiteral("projects"));
  const QString imported = defaultAppdataPath(QStringLiteral("Imported"));
  QDir().mkpath(projects);
  QDir().mkpath(imported);
  std::vector<ZipBlob> entries;
  QString readError;
  if (!readStoredZip(path, &entries, &readError)) {
    if (error) *error = readError.isEmpty() ? QStringLiteral("Could not read that project.") : readError;
    return false;
  }
  const ZipBlob* jsonEntry = nullptr;
  for (const ZipBlob& entry : entries) {
    if (isProjectFileName(QFileInfo(entry.name).fileName())) {
      jsonEntry = &entry;
      break;
    }
  }
  if (!jsonEntry) {
    if (error) *error = QStringLiteral("That archive has no project file (daedalcad.json).");
    return false;
  }
  const QJsonDocument parsed = QJsonDocument::fromJson(jsonEntry->data);
  if (!parsed.isObject()) {
    if (error) *error = QStringLiteral("That file is not a DaedalCAD project.");
    return false;
  }
  QString name = sanitizeProjectName(forcedName.isEmpty() ? QFileInfo(path).completeBaseName() : forcedName);
  if (name.isEmpty()) name = QStringLiteral("Project");
  const QString directory = QDir(projects).absoluteFilePath(name);
  if (!projectFileIn(directory).isEmpty() && askOverwrite) {
    if (QMessageBox::question(this, QStringLiteral("Import project"),
                              QStringLiteral("“%1” already exists. Overwrite its project file? STL and SVG files already in Imported are left alone.")
                                  .arg(name)) != QMessageBox::Yes)
      return false;
  }
  if (!QDir().mkpath(directory)) {
    if (error) *error = QStringLiteral("Could not create the project folder.");
    return false;
  }
  Document loaded = fromJson(parsed.object());
  auto findEntry = [&](const QString& meshPath) -> const ZipBlob* {
    const QString slash = QString(meshPath).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString base = QFileInfo(slash).fileName();
    const ZipBlob* byName = nullptr;
    for (const ZipBlob& entry : entries) {
      const QString entryName = QString(entry.name).replace(QLatin1Char('\\'), QLatin1Char('/'));
      if (entryName.compare(slash, Qt::CaseInsensitive) == 0) return &entry;
      if (!byName && QFileInfo(entryName).fileName().compare(base, Qt::CaseInsensitive) == 0) byName = &entry;
    }
    return byName;
  };
  QHash<QString, QString> placed;
  QStringList missing;
  for (Node& node : loaded.nodes) {
    if ((node.shape != QLatin1String("stl") && node.shape != QLatin1String("svg")) || node.meshPath.isEmpty()) continue;
    const ZipBlob* mesh = findEntry(node.meshPath);
    if (!mesh) {
      missing << node.name;
      continue;
    }
    const QString key = QString(mesh->name).replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (placed.contains(key)) {
      node.meshPath = placed.value(key);
      continue;
    }
    const QString dest = writeUniqueFile(imported, QFileInfo(key).fileName(), mesh->data);
    if (dest.isEmpty()) {
      missing << node.name;
      continue;
    }
    placed.insert(key, dest);
    node.meshPath = dest;
  }
  const QString filePath = QDir(directory).absoluteFilePath(projectFileName());
  QSaveFile out(filePath);
  const QByteArray projectBytes = QJsonDocument(toJson(loaded)).toJson(QJsonDocument::Indented);
  if (!out.open(QIODevice::WriteOnly) || out.write(projectBytes) != projectBytes.size() || !out.commit()) {
    if (error) *error = out.errorString();
    return false;
  }
  dropLegacyProjectFile(directory);
  const bool opening = openAfter || QDir::cleanPath(directory) == QDir::cleanPath(projectDirectory);
  if (opening) {
    pushUndo();
    doc = std::move(loaded);
    projectDirectory = QDir(directory).absolutePath();
    rememberProject(filePath);
    selection.clear();
    refreshTree();
    loadInspector();
    refreshScene();
  }
  refresh2DShapeLibrary();
  if (missing.isEmpty())
    status(QStringLiteral("Imported %1. STL and SVG files are in the Imported folder.").arg(name));
  else
    status(QStringLiteral("Imported %1. Could not copy: %2").arg(name, missing.join(", ")));
  return true;
}

void MainWindow::importProject() {
  const QString projects = defaultAppdataPath(QStringLiteral("projects"));
  QDir().mkpath(projects);
  const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import project"), projects,
                                                    projectArchiveFilter());
  if (path.isEmpty()) return;
  QString error;
  if (!installProjectZip(path, QString(), true, true, &error) && !error.isEmpty())
    QMessageBox::warning(this, QStringLiteral("Import project"), error);
}

void MainWindow::openProjectsFolder() {
  const QString projects = defaultAppdataPath(QStringLiteral("projects"));
  QDir().mkpath(projects);
  QDesktopServices::openUrl(QUrl::fromLocalFile(projects));
}

CloudSyncHooks MainWindow::cloudSyncHooks() {
  CloudSyncHooks hooks;
  hooks.appdataPath = [](const QString& name) { return defaultAppdataPath(name); };
  hooks.currentProjectFolder = [this] { return QDir::cleanPath(projectDirectory); };
  hooks.saveCurrent = [this] { return saveDocument(); };
  hooks.packFolder = [](const QString& folder, QString* error) { return packProjectFolder(folder, nullptr, error); };
  hooks.renderPreview = [](const QString& folder) -> QImage {
    QFile file(projectFileIn(folder));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll());
    if (!parsed.isObject()) return {};
    Document project = fromJson(parsed.object());
    for (Node& node : project.nodes) {
      if (node.meshPath.isEmpty()) continue;
      const QString found = resolveMeshFile(node.meshPath, folder);
      if (!found.isEmpty()) node.meshPath = found;
    }
    QString warning;
    try {
      return renderPreviewImage(buildEditView(project, {}, &warning));
    } catch (...) {
      return {};
    }
  };
  hooks.installZip = [this](const QString& zipPath, const QString& name, bool openAfter, QString* error) {
    return installProjectZip(zipPath, name, openAfter, false, error);
  };
  hooks.status = [this](const QString& text) { status(text); };
  return hooks;
}

void MainWindow::openCloudSync() {
  openCloudSyncWindow(this, cloudSyncHooks(), 0);
}

void MainWindow::openDocument() {
  const QString projects = defaultAppdataPath(QStringLiteral("projects"));
  QDir().mkpath(projects);
  const QString path = QFileDialog::getOpenFileName(this, "Open", projects, "DaedalCAD project (daedalcad.json shapesynth.json *.json)");
  if (path.isEmpty()) return;
  loadProjectFile(path);
}

bool MainWindow::loadProjectFile(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(this, "Open", file.errorString());
    return false;
  }
  const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll());
  if (!parsed.isObject()) {
    QMessageBox::warning(this, "Open", "That file is not a DaedalCAD model.");
    return false;
  }
  pushUndo();
  doc = fromJson(parsed.object());
  const QString directory = QFileInfo(path).absolutePath();
  for (Node& node : doc.nodes) {
    if (node.meshPath.isEmpty()) continue;
    const QFileInfo info(node.meshPath);
    if (info.isAbsolute() && info.exists()) continue;
    const QString beside = QDir(directory).absoluteFilePath(info.fileName());
    if (QFileInfo::exists(beside)) {
      node.meshPath = QFileInfo(beside).absoluteFilePath();
      continue;
    }
    // Imported or placed 2D files from a data folder that has moved (portable -> Documents, or
    // Documents\DaedalCAD -> Documents\DaedalCAD when the program was renamed).
    QString moved = movedDataPath(node.meshPath, QSettings().value(QStringLiteral("movedDataFrom")).toString(), userDataRoot());
    if (moved.isEmpty() || !QFileInfo::exists(moved)) moved = movedDataPath(node.meshPath, renamedDataFrom(), userDataRoot());
    if (!moved.isEmpty() && QFileInfo::exists(moved)) node.meshPath = moved;
    else if (!info.isAbsolute()) node.meshPath = QDir(directory).absoluteFilePath(node.meshPath);
  }
  projectDirectory = isProjectFileName(QFileInfo(path).fileName()) ? directory : QString();
  selection.clear();
  refreshTree();
  loadInspector();
  refreshScene();
  rememberProject(path);
  markSaved();
  status("Opened " + path);
  return true;
}

void MainWindow::rememberProject(const QString& filePath) {
  const QFileInfo info(filePath);
  if (!info.exists() || !info.isFile()) return;
  const QString clean = QDir::cleanPath(info.absoluteFilePath());
  QSettings settings;
  QStringList list;
  list << clean;
  for (const QString& raw : settings.value(QStringLiteral("recentProjects")).toStringList()) {
    const QString path = QDir::cleanPath(QFileInfo(raw).absoluteFilePath());
    if (path.compare(clean, Qt::CaseInsensitive) == 0 || !QFileInfo::exists(path)) continue;
    list << path;
    if (list.size() >= 10) break;
  }
  settings.setValue(QStringLiteral("recentProjects"), list);
  refreshRecentMenu();
}

void MainWindow::refreshRecentMenu() {
  if (!recentMenu) return;
  recentMenu->clear();
  QSettings settings;
  QStringList live;
  for (const QString& raw : settings.value(QStringLiteral("recentProjects")).toStringList()) {
    const QString path = QDir::cleanPath(QFileInfo(raw).absoluteFilePath());
    if (!QFileInfo::exists(path) || live.contains(path, Qt::CaseInsensitive)) continue;
    live << path;
  }
  settings.setValue(QStringLiteral("recentProjects"), live);
  if (live.isEmpty()) {
    QAction* none = recentMenu->addAction(QStringLiteral("No recent projects"));
    none->setEnabled(false);
    return;
  }
  for (const QString& path : live) {
    const QFileInfo info(path);
    const QString name = isProjectFileName(info.fileName())
                             ? info.dir().dirName()
                             : info.completeBaseName();
    QAction* action = recentMenu->addAction(name);
    action->setToolTip(QDir::toNativeSeparators(path));
    connect(action, &QAction::triggered, this, [this, path] { loadProjectFile(path); });
  }
}

UpdateHooks MainWindow::updateHooks() {
  UpdateHooks hooks;
  hooks.beforeQuit = [this] {
    if (doc.nodes.empty()) return true;
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Update DaedalCAD"));
    box.setText(QStringLiteral("DaedalCAD closes to install the update and starts again by itself."));
    box.setInformativeText(QStringLiteral("Save the open project first?"));
    QPushButton* save = box.addButton(QStringLiteral("Save and update"), QMessageBox::AcceptRole);
    QPushButton* discard = box.addButton(QStringLiteral("Update without saving"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(save);
    box.exec();
    if (box.clickedButton() == save) return saveDocument();
    return box.clickedButton() == discard;
  };
  return hooks;
}

void MainWindow::offerDataMove() {
  if (!isInstalledCopy()) return;
  QSettings settings;
  if (settings.value(QStringLiteral("dataMoveAsked")).toBool()) return;
  const QString oldRoot = QDir::cleanPath(settings.value(QStringLiteral("portableDataRoot")).toString());
  const QString newRoot = QDir::cleanPath(userDataRoot());
  if (oldRoot.isEmpty() || oldRoot.compare(newRoot, Qt::CaseInsensitive) == 0 || !hasUserData(oldRoot)) return;
  settings.setValue(QStringLiteral("dataMoveAsked"), true);
  QMessageBox box(this);
  box.setWindowTitle(QStringLiteral("Your projects and libraries"));
  box.setIcon(QMessageBox::Question);
  box.setText(QStringLiteral("An earlier DaedalCAD kept your projects, parts and 2D shapes in:\n%1\n\n"
                             "This installed DaedalCAD keeps them in:\n%2\n\nCopy them over now?")
                  .arg(QDir::toNativeSeparators(oldRoot), QDir::toNativeSeparators(newRoot)));
  box.setInformativeText(QStringLiteral("The old folder is left as it is. Files already in Documents are not overwritten."));
  QPushButton* copy = box.addButton(QStringLiteral("Copy them"), QMessageBox::AcceptRole);
  box.addButton(QStringLiteral("Not now"), QMessageBox::RejectRole);
  box.setDefaultButton(copy);
  box.exec();
  if (box.clickedButton() != copy) {
    status(QStringLiteral("Your old data stays in %1. Copy it into %2 yourself whenever you like.")
               .arg(QDir::toNativeSeparators(oldRoot), QDir::toNativeSeparators(newRoot)));
    return;
  }
  int copied = 0;
  bool failed = false;
  for (const QString& folder : userDataFolders()) {
    const int n = copyMissingFiles(QDir(oldRoot).absoluteFilePath(folder), QDir(newRoot).absoluteFilePath(folder));
    if (n < 0) failed = true;
    else copied += n;
  }
  // Point the library lists and the recent projects at the copies.
  settings.setValue(QStringLiteral("movedDataFrom"), oldRoot);
  auto remap = [&](QStringList list) {
    for (QString& path : list) {
      const QString moved = movedDataPath(path, oldRoot, newRoot);
      if (!moved.isEmpty()) path = moved;
    }
    list.removeDuplicates();
    return list;
  };
  const QString defaultParts = defaultAppdataPath(QStringLiteral("parts"));
  const QString default2DShapes = defaultAppdataPath(QStringLiteral("2d_shapes"));
  partsDirectories = remap(partsDirectories);
  twoDShapesDirectories = remap(twoDShapesDirectories);
  alignLibraryNames(partsDirectories, partsDirectoryNames, defaultParts, QStringLiteral("Parts"));
  alignLibraryNames(twoDShapesDirectories, twoDShapesDirectoryNames, default2DShapes, QStringLiteral("2D shapes"));
  settings.setValue(QStringLiteral("partsDirectories"), partsDirectories);
  settings.setValue(QStringLiteral("partsDirectoryNames"), partsDirectoryNames);
  settings.setValue(QStringLiteral("twoDShapesDirectories"), twoDShapesDirectories);
  settings.setValue(QStringLiteral("twoDShapesDirectoryNames"), twoDShapesDirectoryNames);
  settings.setValue(QStringLiteral("recentProjects"), remap(settings.value(QStringLiteral("recentProjects")).toStringList()));
  refreshPartsLibrary();
  refresh2DShapeLibrary();
  refreshSculptLibrary();
  refreshRecentMenu();
  if (failed)
    QMessageBox::warning(this, QStringLiteral("Your projects and libraries"),
                         QStringLiteral("Copied %1 files, but some could not be copied. They are still in %2.")
                             .arg(copied)
                             .arg(QDir::toNativeSeparators(oldRoot)));
  else
    status(QStringLiteral("Copied %1 files to %2.").arg(copied).arg(QDir::toNativeSeparators(newRoot)));
}

void MainWindow::offerRecentProjects(bool asked) {
  QSettings settings;
  QStringList live;
  for (const QString& raw : settings.value(QStringLiteral("recentProjects")).toStringList()) {
    const QString path = QDir::cleanPath(QFileInfo(raw).absoluteFilePath());
    if (QFileInfo::exists(path)) live << path;
  }
  if (live.isEmpty()) {
    if (asked) status(QStringLiteral("No recent projects yet."));
    return;
  }
  if (!doc.nodes.empty() && !asked) return;
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("Recent projects"));
  dialog.resize(560, 380);
  auto* layout = new QVBoxLayout(&dialog);
  auto* hint = new QLabel(QStringLiteral("Open a project, or start with an empty bed."), &dialog);
  hint->setWordWrap(true);
  layout->addWidget(hint);
  auto* list = new QListWidget(&dialog);
  for (const QString& path : live) {
    const QFileInfo info(path);
    const QString name = isProjectFileName(info.fileName())
                             ? info.dir().dirName()
                             : info.completeBaseName();
    const QString when = info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    auto* item = new QListWidgetItem(QStringLiteral("%1    %2").arg(name, when), list);
    item->setData(Qt::UserRole, path);
    item->setToolTip(QDir::toNativeSeparators(path));
  }
  list->setCurrentRow(0);
  layout->addWidget(list, 1);
  auto* buttons = new QHBoxLayout();
  auto* open = new QPushButton(QStringLiteral("Open"), &dialog);
  auto* empty = new QPushButton(QStringLiteral("Empty bed"), &dialog);
  buttons->addStretch(1);
  buttons->addWidget(empty);
  buttons->addWidget(open);
  layout->addLayout(buttons);
  const auto openChosen = [&] {
    QListWidgetItem* item = list->currentItem();
    if (!item) return;
    const QString path = item->data(Qt::UserRole).toString();
    dialog.accept();
    loadProjectFile(path);
  };
  connect(open, &QPushButton::clicked, &dialog, openChosen);
  connect(list, &QListWidget::itemDoubleClicked, &dialog, [&](QListWidgetItem*) { openChosen(); });
  connect(empty, &QPushButton::clicked, &dialog, &QDialog::reject);
  dialog.exec();
}

void MainWindow::backToMove(bool quiet) {
  if (alignPopup) alignPopup->hide();
  if (repeatPopup) repeatPopup->hide();
  if (filletPopup) filletPopup->hide();
  if (faceFilletPopup) faceFilletPopup->hide();
  if (edgeBetaPopup) edgeBetaPopup->hide();
  if (view && (tool == 4 || tool == 9)) view->setFilletPick({}, {}, false);
  filletTarget.clear();
  edgeBetaTarget.clear();
  tool = 0;
  panel = 0;
  if (view) view->setTool(0);
  syncActions();
  loadInspector();
  if (!quiet) status(QStringLiteral("Move. Drag the body on X/Y. White corners scale all sides. Black squares scale one side."));
}

void MainWindow::showMeasureTool() {
  if (!view) return;
  if (tool == 8) {
    tool = 0;
    panel = 0;
    view->setTool(0);
    syncActions();
    loadInspector();
    status(QStringLiteral("Measure closed."));
    return;
  }
  if (alignPopup) alignPopup->hide();
  if (repeatPopup) repeatPopup->hide();
  if (filletPopup) filletPopup->hide();
  if (faceFilletPopup) faceFilletPopup->hide();
  if (edgeBetaPopup) edgeBetaPopup->hide();
  tool = 8;
  panel = 0;
  view->setTool(8);
  syncActions();
  loadInspector();
  status(QStringLiteral("Click a point. Vertices and edge centres snap. Click a second point for the distance. Esc clears."));
}

void MainWindow::updateMeasureOverlay() {
  if (!view) return;
  QVector3D size;
  const bool haveSize = tool == 8 && selectionSize(&size);
  view->setMeasureReadout(tool == 8 && measureHaveA, measureA, tool == 8 && measureHaveB, measureB, haveSize, size);
}

bool MainWindow::selectionSize(QVector3D* size) const {
  if (!size) return false;
  bool any = false;
  QVector3D lo;
  QVector3D hi;
  for (const QString& id : selection) {
    const Node* node = doc.find(id);
    if (!node) continue;
    bool parentSelected = false;
    const Node* walk = node;
    int guard = 0;
    while (walk && !walk->parentId.isEmpty() && guard++ < 64) {
      if (selection.contains(walk->parentId)) {
        parentSelected = true;
        break;
      }
      walk = doc.find(walk->parentId);
    }
    if (parentSelected) continue;
    QVector3D partLo;
    QVector3D partHi;
    if (!alignmentWorldBounds(doc, *node, &partLo, &partHi)) continue;
    if (!any) {
      lo = partLo;
      hi = partHi;
      any = true;
    } else {
      lo = QVector3D(std::min(lo.x(), partLo.x()), std::min(lo.y(), partLo.y()), std::min(lo.z(), partLo.z()));
      hi = QVector3D(std::max(hi.x(), partHi.x()), std::max(hi.y(), partHi.y()), std::max(hi.z(), partHi.z()));
    }
  }
  if (!any) return false;
  *size = hi - lo;
  return true;
}

void MainWindow::fillFontStyles(const QString& family, const QString& current) {
  if (!fontStyleBox) return;
  const QSignalBlocker blocker(fontStyleBox);
  QStringList styles = QFontDatabase::styles(family);
  if (styles.isEmpty()) styles << QStringLiteral("Regular");
  bool same = fontStyleBox->count() == styles.size();
  for (int i = 0; same && i < styles.size(); ++i) same = fontStyleBox->itemText(i) == styles[i];
  if (!same) {
    fontStyleBox->clear();
    fontStyleBox->addItems(styles);
  }
  int index = styles.indexOf(current);
  for (const QString& fallback : {QStringLiteral("Regular"), QStringLiteral("Normal"), QStringLiteral("Book"),
                                  QStringLiteral("Roman"), QStringLiteral("Medium")}) {
    if (index >= 0) break;
    index = styles.indexOf(fallback);
  }
  fontStyleBox->setCurrentIndex(std::max(0, index));
}

// How far `node` can move straight down before it rests on another object (world mm): NaN when
// nothing is under it, negative when it is sunk into it and has to come up. Measured down from the
// node's points onto upward faces, and up from the other objects' points onto the node's downward
// faces, so a flat bottom on a dome works too.
double MainWindow::dropDistanceBelow(const Node& node, QString* landedOn) const {
  const Node* root = &node;
  while (!root->parentId.isEmpty()) {
    const Node* parent = doc.find(root->parentId);
    if (!parent) break;
    root = parent;
  }
  // Exact geometry (double precision), so the part lands ON the face, not a rounding step off it.
  // A hole drops by its own shape too (onto a part, it rests on top, cutting nothing yet).
  Document shapeOnly = doc;
  if (Node* self = shapeOnly.find(node.id)) self->hole = false;
  std::vector<double> mv;
  std::vector<uint64_t> mt;
  if (!worldMesh64(shapeOnly, QStringList{node.id}, &mv, &mt) || mt.size() < 3) return std::nan("");
  double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
  double moverBottom = 1e300, moverTop = -1e300;
  for (size_t i = 0; i + 2 < mv.size(); i += 3) {
    lo[0] = std::min(lo[0], mv[i]);
    hi[0] = std::max(hi[0], mv[i]);
    lo[1] = std::min(lo[1], mv[i + 1]);
    hi[1] = std::max(hi[1], mv[i + 1]);
    moverBottom = std::min(moverBottom, mv[i + 2]);
    moverTop = std::max(moverTop, mv[i + 2]);
  }
  // Height of triangle a b c above (px, py), when that point is inside it seen from above.
  auto heightAt = [](const double* a, const double* b, const double* c, double px, double py, double* z) {
    const double d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
    if (std::abs(d) < 1e-14) return false;
    const double w0 = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) / d;
    const double w1 = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) / d;
    const double w2 = 1.0 - w0 - w1;
    const double eps = -1e-9;
    if (w0 < eps || w1 < eps || w2 < eps) return false;
    *z = w0 * a[2] + w1 * b[2] + w2 * c[2];
    return true;
  };
  auto facing = [](const double* a, const double* b, const double* c) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);  // > 0: faces up
  };
  // A part already sunk into the one below counts too (negative gap): it is lifted to rest on it.
  const double reach = std::max(1e-3, moverTop - moverBottom);
  double best = 1e300;
  QString bestName;
  for (const Node* other : doc.children(QString())) {
    if (other->id == root->id || other->hole) continue;
    std::vector<double> gv;
    std::vector<uint64_t> gt;
    if (!worldMesh64(doc, QStringList{other->id}, &gv, &gt) || gt.size() < 3) continue;
    double found = 1e300;
    for (size_t t = 0; t + 2 < gt.size(); t += 3) {
      const double* a = &gv[gt[t] * 3];
      const double* b = &gv[gt[t + 1] * 3];
      const double* c = &gv[gt[t + 2] * 3];
      if (facing(a, b, c) <= 1e-12) continue;
      const double tlo0 = std::min({a[0], b[0], c[0]}), thi0 = std::max({a[0], b[0], c[0]});
      const double tlo1 = std::min({a[1], b[1], c[1]}), thi1 = std::max({a[1], b[1], c[1]});
      if (thi0 < lo[0] || tlo0 > hi[0] || thi1 < lo[1] || tlo1 > hi[1]) continue;
      for (size_t i = 0; i + 2 < mv.size(); i += 3) {
        const double* v = &mv[i];
        if (v[0] < tlo0 || v[0] > thi0 || v[1] < tlo1 || v[1] > thi1) continue;
        double z = 0;
        if (!heightAt(a, b, c, v[0], v[1], &z)) continue;
        const double gap = v[2] - z;
        if (gap > -reach) found = std::min(found, gap);
      }
    }
    for (size_t t = 0; t + 2 < mt.size(); t += 3) {
      const double* a = &mv[mt[t] * 3];
      const double* b = &mv[mt[t + 1] * 3];
      const double* c = &mv[mt[t + 2] * 3];
      if (facing(a, b, c) >= -1e-12) continue;  // the mover's downward faces only
      const double tlo0 = std::min({a[0], b[0], c[0]}), thi0 = std::max({a[0], b[0], c[0]});
      const double tlo1 = std::min({a[1], b[1], c[1]}), thi1 = std::max({a[1], b[1], c[1]});
      for (size_t i = 0; i + 2 < gv.size(); i += 3) {
        const double* g = &gv[i];
        if (g[0] < tlo0 || g[0] > thi0 || g[1] < tlo1 || g[1] > thi1) continue;
        double z = 0;
        if (!heightAt(a, b, c, g[0], g[1], &z)) continue;
        const double gap = z - g[2];
        if (gap > -reach) found = std::min(found, gap);
      }
    }
    if (found < best) {
      best = found;
      bestName = other->name;
    }
  }
  if (best > 1e299 || moverBottom - best < doc.workplaneZ - 1e-6) return std::nan("");  // nothing under it above the bed
  if (landedOn) *landedOn = bestName.isEmpty() ? QStringLiteral("the part below") : bestName;
  return best;
}

void MainWindow::placeShape(const QString& shape, double x, double y, double fillet) {
  pushUndo();
  const QString id = doc.addShape(shape, x, y, fillet);
  if (Node* node = doc.find(id); node && shape == QLatin1String("text")) {
    const QStringList installed = QFontDatabase::families();
    for (const QString& candidate : {QStringLiteral("Arial"), QStringLiteral("Segoe UI"), QStringLiteral("Helvetica"),
                                     QStringLiteral("DejaVu Sans"), QStringLiteral("Liberation Sans")}) {
      if (installed.contains(candidate)) {
        node->fontFamily = candidate;
        break;
      }
    }
    if (node->fontFamily.isEmpty()) node->fontFamily = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    const QStringList styles = QFontDatabase::styles(node->fontFamily);
    node->fontStyle = styles.contains(QStringLiteral("Regular")) ? QStringLiteral("Regular") : styles.value(0);
  }
  standOnWorkplane(id, placingDropped ? placingU : 0, placingDropped ? placingV : 0);
  selection = QStringList{id};
  view->focusOn(selection);  // the view turns around the new part
  refreshTree();
  loadInspector();
  refreshScene();
  if (shape == QLatin1String("text")) {
    status(QStringLiteral("Added text. Type yours in the Text box, then pick a font and size."));
    if (textEdit && textEdit->isVisible()) {
      textEdit->setFocus();
      textEdit->selectAll();
    }
    return;
  }
  status(fillet > 0 ? QString("Added %1 with a %2 mm edge fillet.").arg(shape).arg(fillet) : "Added " + shape + ".");
}

void MainWindow::refreshPartsLibrary() {
  if (!parts) return;
  parts->clear();
  QStringList seenRoots;
  for (int index = 0; index < partsDirectories.size(); ++index) {
    const QString rootPath = QDir::cleanPath(QFileInfo(partsDirectories[index]).absoluteFilePath());
    if (rootPath.isEmpty() || seenRoots.contains(rootPath)) continue;
    seenRoots << rootPath;
    QString name = index < partsDirectoryNames.size() ? partsDirectoryNames[index].trimmed() : QString();
    if (name.isEmpty()) name = QFileInfo(rootPath).fileName();
    if (name.isEmpty()) name = rootPath;
    auto* header = new QListWidgetItem(QStringLiteral("▾ ") + name, parts);
    header->setData(Qt::UserRole, QStringLiteral("header"));
    header->setData(Qt::UserRole + 1, true);
    header->setData(Qt::UserRole + 2, name);
    header->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    header->setForeground(QColor("#e87400"));
    header->setBackground(QColor("#2a1c10"));
    QFont font = header->font();
    font.setBold(true);
    header->setFont(font);
    header->setToolTip(QDir::toNativeSeparators(rootPath));
    if (!QDir(rootPath).exists()) {
      auto* missing = new QListWidgetItem(QStringLiteral("    Folder not found"), parts);
      missing->setFlags(Qt::NoItemFlags);
      missing->setForeground(QColor("#8a8074"));
      continue;
    }
    int count = 0;
    const QHash<QString, QStringList> rootTags = loadLibraryTags(rootPath);
    QDirIterator it(rootPath, QStringList{QStringLiteral("*.stl"), QStringLiteral("*.STL")},
                    QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      const QFileInfo info(it.next());
      auto* item = new QListWidgetItem(shapeIcon(QStringLiteral("stl")), QStringLiteral("    ") + info.fileName(), parts);
      item->setData(Qt::UserRole, QStringLiteral("stl:") + info.absoluteFilePath());
      const QString rel = relativeLibraryPath(rootPath, info.absoluteFilePath());
      const QStringList tags = rootTags.value(rel);
      item->setData(Qt::UserRole + 3, tags);
      QString tip = QDir::toNativeSeparators(info.absoluteFilePath());
      if (!tags.isEmpty()) tip += QStringLiteral("\nTags: ") + tags.join(QStringLiteral(", "));
      item->setToolTip(tip);
      ++count;
    }
    if (count == 0) {
      auto* empty = new QListWidgetItem(QStringLiteral("    No files"), parts);
      empty->setFlags(Qt::NoItemFlags);
      empty->setForeground(QColor("#8a8074"));
    }
  }
  // A search typed before the list was rebuilt still applies.
  if (auto* search = findChild<QLineEdit*>(QStringLiteral("partsSearch")); search && !search->text().isEmpty())
    filterLibraryList(parts, search->text());
}

// File > Import STEP: every solid of the file becomes a part on the plate, in the file's names and
// colours and where the file puts it (the whole set stood on the plate, in the middle). Each is
// drawn from a mesh made of it, and remembers the file, so the STEP export writes it exactly as it
// came in. The file is copied to Documents\DaedalCAD\Imported\STEP, so it stays available.
void MainWindow::importStep() {
  QSettings settings;
  const QString start = settings.value(QStringLiteral("stepImportDir"), QDir::homePath()).toString();
  const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import STEP"), start, QStringLiteral("STEP (*.step *.stp *.STEP *.STP)"));
  if (path.isEmpty()) return;
  settings.setValue(QStringLiteral("stepImportDir"), QFileInfo(path).absolutePath());
  importStepFile(path);
}

void MainWindow::importStepFile(const QString& path) {
  const QString stem = sanitizeProjectName(QFileInfo(path).completeBaseName()).replace(QLatin1Char(' '), QLatin1Char('_'));
  const QString folder = QDir(defaultAppdataPath(QStringLiteral("Imported"))).filePath(
      QStringLiteral("STEP/%1-%2").arg(stem, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
  if (!QDir().mkpath(folder)) {
    QMessageBox::warning(this, QStringLiteral("Import STEP"), QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(folder)));
    return;
  }
  const QString kept = QDir(folder).filePath(QFileInfo(path).fileName());
  QFile::copy(path, kept);
  std::vector<exact::ImportedPart> parts;
  QString error;
  bool ok = false;
  status(QStringLiteral("Reading %1…").arg(QFileInfo(path).fileName()));
  runBusy([&] { ok = exact::readStep(kept, &parts, &error); }, true);
  if (!ok) {
    QMessageBox::warning(this, QStringLiteral("Import STEP"), error.isEmpty() ? QStringLiteral("Could not read that file.") : error);
    return;
  }
  // The whole set: its box, to stand it on the plate in the middle.
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (const auto& part : parts)
    for (size_t i = 0; i + 2 < part.triangles.size(); i += 3)
      for (int a = 0; a < 3; ++a) {
        lo[a] = std::min(lo[a], part.triangles[i + static_cast<size_t>(a)] + part.centre[a]);
        hi[a] = std::max(hi[a], part.triangles[i + static_cast<size_t>(a)] + part.centre[a]);
      }
  const double shift[3] = {-(lo[0] + hi[0]) * 0.5, -(lo[1] + hi[1]) * 0.5, doc.workplaneZ - lo[2]};
  pushUndo();
  QStringList made;
  for (const auto& part : parts) {
    const QString stl = QDir(folder).filePath(QStringLiteral("part-%1.stl").arg(part.index + 1));
    QFile file(stl);
    if (!file.open(QIODevice::WriteOnly)) continue;
    QByteArray header(80, 0);
    file.write(header);
    const quint32 count = static_cast<quint32>(part.triangles.size() / 9);
    file.write(reinterpret_cast<const char*>(&count), 4);
    const float normal[3] = {0, 0, 0};
    for (size_t i = 0; i + 8 < part.triangles.size(); i += 9) {
      file.write(reinterpret_cast<const char*>(normal), 12);
      file.write(reinterpret_cast<const char*>(&part.triangles[i]), 36);
      file.write("\0\0", 2);
    }
    file.close();
    Node node;
    node.id = newId();
    node.kind = Node::Kind::Shape;
    node.shape = QStringLiteral("stl");
    node.meshPath = stl;
    node.name = part.name;
    node.color = part.hasColor ? part.color : QColor("#9aa7b2");
    node.stepSource = kept;
    node.stepIndex = part.index;
    for (int a = 0; a < 3; ++a) {
      node.stepOffset[a] = part.centre[a];
      node.pos[a] = part.centre[a] + shift[a];
    }
    tidyPlacement(node);
    doc.nodes.push_back(node);
    made << node.id;
  }
  if (made.isEmpty()) {
    undo();
    status(QStringLiteral("Nothing could be imported from that file."));
    return;
  }
  selection = made;
  refreshTree();
  loadInspector();
  refreshScene(false);
  status(QStringLiteral("Imported %1 part%2 from %3. They are exact again in a STEP export; Group them to move them as one.")
             .arg(made.size())
             .arg(made.size() == 1 ? QString() : QStringLiteral("s"))
             .arg(QFileInfo(path).fileName()));
}

void MainWindow::importStlPart(const QString& path, double x, double y) {
  if (path.isEmpty()) return;
  QFileInfo info(path);
  if (!info.exists() || !info.isFile()) {
    status("STL file not found: " + path);
    return;
  }
  pushUndo();
  Node node;
  node.id = newId();
  node.kind = Node::Kind::Shape;
  node.shape = QStringLiteral("stl");
  node.meshPath = info.absoluteFilePath();
  node.name = info.completeBaseName();
  node.pos[0] = doc.snapOn ? std::round(x / doc.snap) * doc.snap : x;
  node.pos[1] = doc.snapOn ? std::round(y / doc.snap) * doc.snap : y;
  node.pos[2] = doc.workplaneZ;
  node.color = QColor("#9aa7b2");
  doc.nodes.push_back(node);
  if (Node* imported = doc.find(node.id)) {
    double lo[3];
    double hi[3];
    if (localBounds(doc, *imported, lo, hi)) imported->pos[2] += doc.workplaneZ - lo[2];
    tidyPlacement(*imported);
  }
  standOnWorkplane(node.id, placingDropped ? placingU : 0, placingDropped ? placingV : 0);
  selection = QStringList{node.id};
  view->focusOn(selection);  // the view turns around the new part
  refreshTree();
  loadInspector();
  refreshScene(false);
  sculpt::Mesh imported;
  if (partMesh(node.id, &imported) && imported.tri.size() > 600000)
    status(QStringLiteral("Imported STL: %1 (%L2 triangles: Simplify can make it much lighter to work with).")
               .arg(node.name)
               .arg(static_cast<qlonglong>(imported.tri.size())));
  else
    status("Imported STL: " + node.name);
}

void MainWindow::extrudePickedFace(const QString& id, const QVector<QVector3D>& loop, QVector3D normal, QVector3D axis) {
  const Node* source = doc.find(id);
  if (!source || source->kind == Node::Kind::Pattern || source->hole) {
    status("Select a solid face to extrude.");
    return;
  }
  if (loop.size() < 3) {
    status("That face has no usable boundary.");
    return;
  }
  QVector3D n = normal;
  if (n.lengthSquared() < 1e-8f) {
    status("That face has no valid normal.");
    return;
  }
  n.normalize();
  QVector3D centroid;
  int used = 0;
  for (const QVector3D& point : loop) {
    if (std::abs(point.x()) > 1e19f) continue;
    centroid += point;
    ++used;
  }
  if (used < 3) {
    status("That face has no usable boundary.");
    return;
  }
  centroid /= static_cast<float>(used);

  const double zero[3] = {0, 0, 0};
  // faceLoop/normal/axis are expressed in the selected item's local frame.
  // Build the extrusion at the selected face centroid in that same frame,
  // then keep the extrusion as a sibling so nested groups retain their
  // existing placement exactly once.
  const QMatrix4x4 sourcePlacement = placementMatrix(source->pos, source->rot, zero, source->scale);
  QMatrix4x4 extrudePlacement = sourcePlacement;
  extrudePlacement.translate(centroid);
  double localPos[3];
  double localRot[3];
  double localScale[3];
  decomposePlacement(extrudePlacement, localPos, localRot, localScale);

  QVector3D localAxis = axis;
  if (localAxis.lengthSquared() < 1e-8f) localAxis = QVector3D(1, 0, 0);
  localAxis -= n * QVector3D::dotProduct(localAxis, n);
  if (localAxis.lengthSquared() < 1e-8f) {
    localAxis = std::abs(n.z()) < 0.9f ? QVector3D(0, 0, 1) : QVector3D(1, 0, 0);
    localAxis -= n * QVector3D::dotProduct(localAxis, n);
  }
  localAxis.normalize();

  pushUndo();
  Node extrude;
  extrude.id = newId();
  extrude.kind = Node::Kind::Shape;
  extrude.shape = QStringLiteral("extrude");
  extrude.name = QString("Extrusion %1").arg(doc.nodes.size() + 1);
  extrude.faceExtrude = true;
  extrude.extrudeDepth = extrudeDepthSpin ? extrudeDepthSpin->value() : 10;
  // Made at the part's real size, as Bend face is: a stretched part's scale goes into the face,
  // so the depth is the depth asked for, not stretched with the part.
  const QVector3D stretch{static_cast<float>(localScale[0]), static_cast<float>(localScale[1]), static_cast<float>(localScale[2])};
  auto safe = [](float v) { return std::abs(v) > 1e-6f ? v : 1e-6f; };
  n = QVector3D(n.x() / safe(stretch.x()), n.y() / safe(stretch.y()), n.z() / safe(stretch.z())).normalized();
  localAxis = QVector3D(localAxis.x() * stretch.x(), localAxis.y() * stretch.y(), localAxis.z() * stretch.z());
  localAxis -= n * QVector3D::dotProduct(localAxis, n);
  if (localAxis.lengthSquared() > 1e-12f) localAxis.normalize();
  for (int i = 0; i < 3; ++i) {
    extrude.pos[i] = localPos[i];
    extrude.rot[i] = localRot[i];
    extrude.scale[i] = 1;
  }
  extrude.faceNormal[0] = n.x();
  extrude.faceNormal[1] = n.y();
  extrude.faceNormal[2] = n.z();
  extrude.faceAxis[0] = localAxis.x();
  extrude.faceAxis[1] = localAxis.y();
  extrude.faceAxis[2] = localAxis.z();
  extrude.faceLoop.reserve(static_cast<size_t>(loop.size()) * 3);
  for (const QVector3D& point : loop) {
    if (std::abs(point.x()) > 1e19f) {
      extrude.faceLoop.push_back(1e20);
      extrude.faceLoop.push_back(1e20);
      extrude.faceLoop.push_back(1e20);
      continue;
    }
    const QVector3D local = (point - centroid) * stretch;
    extrude.faceLoop.push_back(local.x());
    extrude.faceLoop.push_back(local.y());
    extrude.faceLoop.push_back(local.z());
  }
  extrude.color = source->color;
  extrude.parentId = source->parentId;
  doc.nodes.push_back(std::move(extrude));
  selection = QStringList{doc.nodes.back().id};
  refreshTree();
  loadInspector();
  refreshScene(false);
  status("Face extruded as its own object.");
}

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  if (view && view->parentWidget()) {
    QWidget* host = view->parentWidget();
    view->setGeometry(0, 0, host->width(), host->height());
  }
  placeOverlays();
}

void MainWindow::markSaved() { savedState = QJsonDocument(toJson(doc)).toJson(QJsonDocument::Compact); }

bool MainWindow::hasUnsavedChanges() const {
  if (doc.nodes.empty() && savedState.isEmpty()) return false;
  return QJsonDocument(toJson(doc)).toJson(QJsonDocument::Compact) != savedState;
}

void MainWindow::closeEvent(QCloseEvent* event) {
  // Changes since the last save (or since the project was opened): save them, leave without,
  // or stay.
  if (hasUnsavedChanges()) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("Exit DaedalCAD"));
    box.setText(projectDirectory.isEmpty() ? QStringLiteral("This project has not been saved.")
                                           : QStringLiteral("“%1” has changes that are not saved.")
                                                 .arg(QFileInfo(projectDirectory).fileName()));
    box.setInformativeText(QStringLiteral("Save them before you exit?"));
    QPushButton* save = box.addButton(QStringLiteral("Save and exit"), QMessageBox::AcceptRole);
    QPushButton* leave = box.addButton(QStringLiteral("Exit without saving"), QMessageBox::DestructiveRole);
    QPushButton* stay = box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(save);
    box.setEscapeButton(stay);
    box.exec();
    if (box.clickedButton() == save) {
      if (!saveDocument()) {
        event->ignore();
        return;
      }
    } else if (box.clickedButton() != leave) {
      event->ignore();
      return;
    }
  }
  QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);
  if (view && view->parentWidget()) {
    QWidget* host = view->parentWidget();
    view->setGeometry(0, 0, host->width(), host->height());
  }
  placeOverlays();
  if (!recentOffered) {
    recentOffered = true;
    // The start-up questions wait until the splash has faded, so nothing pops up over it.
    auto afterSplash = [this] {
      offerDataMove();
      // Recent projects are in File > Recent (and File > Recent projects…); nothing pops up.
      // Look for update requests and new versions a moment later, once the window is settled.
      QTimer::singleShot(1500, this, [this] { checkCloudSyncAtStartup(this, cloudSyncHooks()); });
      QTimer::singleShot(4000, this, [this] { checkForUpdatesAtStartup(this, updateHooks()); });
    };
    const QPixmap splash(resourcePath(QStringLiteral("splash.png")));
    if (splash.isNull() || qEnvironmentVariableIsSet("DAEDALCAD_NO_SPLASH")) {
      QTimer::singleShot(0, this, afterSplash);
    } else {
      new SplashOverlay(this, splash, 2200, 900, [this, afterSplash] { QTimer::singleShot(0, this, afterSplash); });
    }
  }
}

void MainWindow::placeOverlays() {
  QWidget* host = view ? view->parentWidget() : nullptr;
  if (!host || !filletPopup || !busyWrap) return;
  auto placeAt = [&](QFrame* popup, int y) {
    if (!popup) return y;
    const bool paired = popup == colorPopup || popup == toolPopup;
    const int popupWidth = paired ? 280 : std::max(248, popup->sizeHint().width());
    popup->setFixedWidth(popupWidth);
    popup->adjustSize();
    const int popupHeight = std::max(popup->sizeHint().height(), 36);
    popup->resize(popupWidth, popupHeight);
    popup->move(std::max(8, host->width() - popupWidth - 12), y);
    popup->raise();
    return y + popup->height() + 8;
  };
  int y = 12;
  if (colorPopup) {
    colorPopup->show();
    y = placeAt(colorPopup, y);
  }
  QFrame* options = nullptr;
  if (filletPopup->isVisible()) options = filletPopup;
  else if (faceFilletPopup && faceFilletPopup->isVisible()) options = faceFilletPopup;
  else if (edgeBetaPopup && edgeBetaPopup->isVisible()) options = edgeBetaPopup;
  else if (repeatPopup && repeatPopup->isVisible()) options = repeatPopup;
  else if (alignPopup && alignPopup->isVisible()) options = alignPopup;
  else if (toolPopup && toolPopup->isVisible()) options = toolPopup;
  if (options == toolPopup && genScroll && genPage && genPage->isVisible() && genInner) {
    // Long generator settings scroll inside the tool window instead of running off the view.
    const int content = genInner->sizeHint().height() + 4;
    genScroll->setFixedHeight(std::max(1, std::min(content, 400)));
    // Layouts update lazily; measure with the new height, not the old one, or the difference ends
    // up as empty bands above and below the list.
    genPage->layout()->activate();
    toolPopup->layout()->activate();
    const int others = toolPopup->sizeHint().height() - genScroll->height();
    const int room = host->height() - y - 24 - others;
    genScroll->setFixedHeight(std::clamp(content, std::min(content, 140), std::max(140, room)));
    genPage->layout()->activate();
    toolPopup->layout()->activate();
  }
  if (options) placeAt(options, y);
  if (sectionBar && sectionBar->isVisible()) {
    sectionBar->adjustSize();
    const QSize size = sectionBar->sizeHint();
    sectionBar->resize(size);
    sectionBar->move(std::max(8, (host->width() - size.width()) / 2), std::max(8, host->height() - size.height() - 12));
    sectionBar->raise();
  }
  busyWrap->adjustSize();
  busyWrap->move(std::max(8, host->width() - busyWrap->sizeHint().width() - 12),
                 std::max(8, host->height() - busyWrap->sizeHint().height() - 12));
  busyWrap->raise();
}

bool MainWindow::sceneIsHeavy() const {
  for (const Node& node : doc.nodes) {
    if (node.kind == Node::Kind::Group) return true;
    if (node.kind == Node::Kind::Pattern && node.count > 2) return true;
    if (node.shape == QLatin1String("stl")) return true;
    if (node.shape == QLatin1String("generator")) return true;
    if (node.hole && node.clearanceMode != 0) return true;
    if (std::abs(node.bend) > 0.05) return true;
    if (std::abs(node.twist) > 0.05) return true;
    if (node.shape == QLatin1String("elbow")) return true;
    if (node.shape == QLatin1String("facefillet")) return true;
    if (node.surfaceFillet > 0.02) return true;
    if (!node.edgeBeta.empty()) return true;
    if (node.edgeFillet > 0.02 && node.kind == Node::Kind::Shape &&
        (node.shape == QLatin1String("prism") || node.shape == QLatin1String("wedge") ||
         node.shape == QLatin1String("pyramid")))
      return true;
  }
  return false;
}

void MainWindow::runBusyOrNow(const std::function<void()>& work, bool immediate) {
  if (rebuilding) work();
  else runBusy(work, immediate);
}

void MainWindow::runBusy(const std::function<void()>& work, bool immediate) {
  if (rebuilding) return;
  rebuilding = true;
  std::atomic<bool> done{false};
  std::exception_ptr error;
  std::thread worker([&] {
    try {
      work();
    } catch (...) {
      error = std::current_exception();
    }
    done.store(true);
  });
  const auto start = std::chrono::steady_clock::now();
  bool shown = false;
  auto reveal = [&] {
    if (shown || !busyWrap) return;
    busy->setRange(0, 0);
    busyWrap->show();
    placeOverlays();
    shown = true;
  };
  if (immediate) reveal();
  while (!done.load()) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (elapsed > 120) reveal();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
    if (!done.load()) QThread::msleep(16);
  }
  worker.join();
  if (busyWrap) busyWrap->hide();
  rebuilding = false;
  if (error) {
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& ex) {
      status(QString::fromUtf8(ex.what()));
    } catch (...) {
      status("That edit failed.");
    }
  }
}

void MainWindow::showFilletTool() {
  auto primitive = [](const Node* node) {
    if (!node || node->kind != Node::Kind::Shape) return false;
    const QString& shape = node->shape;
    return shape == QLatin1String("box") || shape == QLatin1String("cylinder") || shape == QLatin1String("cone") ||
           shape == QLatin1String("tube") || shape == QLatin1String("hemisphere") || shape == QLatin1String("prism") ||
           shape == QLatin1String("diamond") || shape == QLatin1String("wedge") || shape == QLatin1String("pyramid");
  };
  if (faceFilletPopup) faceFilletPopup->hide();
  if (edgeBetaPopup) edgeBetaPopup->hide();
  if (alignPopup) alignPopup->hide();
  if (repeatPopup) repeatPopup->hide();
  if (tool == 4 && filletPopup && filletPopup->isVisible()) {
    tool = 0;
    panel = 0;
    view->setTool(0);
    syncActions();
    loadInspector();
    status("Fillet closed.");
    return;
  }
  const Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
  if (!node || !primitive(node)) {
    status("Edge fillet is only for basic shapes. Boxes, cylinders, cones, tubes, prisms, diamonds, wedges, and pyramids.");
    return;
  }
  tool = 4;
  panel = 0;
  view->setTool(4);
  filletTarget.clear();
  syncActions();
  loadInspector();
  const std::vector<PickEdge> edges = pickEdges(doc, *node);
  if (edges.empty()) {
    status("This shape has no sharp edge. Spheres, toruses, and capsules stay smooth.");
  } else {
    status("Click an edge. Shift adds or removes. Set the radius in the popup, then Apply.");
  }
}

void MainWindow::syncFilletChrome() {
  if (!filletPopup || !view) return;
  if (tool != 4) {
    filletPopup->hide();
    view->setFilletPick({}, {}, false);
    filletTarget.clear();
    return;
  }
  const QString id = selection.size() == 1 ? selection.front() : QString();
  const Node* node = id.isEmpty() ? nullptr : doc.find(id);
  if (!node || node->kind == Node::Kind::Pattern) {
    filletTarget.clear();
    pendingEdges.clear();
    pendingAll = false;
    view->setFilletPick({}, {}, false);
    if (filletCount) filletCount->setText(QStringLiteral("No object"));
    if (filletHint)
      filletHint->setText(QStringLiteral("Select one basic shape. Then click an edge. Shift adds or removes. Apply rounds that selection."));
    filletPopup->show();
    placeOverlays();
    return;
  }
  if (id != filletTarget) {
    filletTarget = id;
    pendingEdges = node->filletEdges;
    pendingAll = node->filletEdges.empty() && node->edgeFillet > 0.02;
    const bool was = loading;
    loading = true;
    edgeSpin->setValue(node->edgeFillet);
    filletSegSpin->setValue(std::clamp(doc.filletSegments, 8, 72));
    loading = was;
  }
  if (surfaceSpin) surfaceSpin->hide();
  for (QLabel* label : filletPopup->findChildren<QLabel*>()) {
    if (label->text().startsWith(QLatin1String("Surface"))) label->hide();
  }
  const std::vector<PickEdge> edges = pickEdges(doc, *node);
  int edgeCount = 0;
  std::vector<int> seen;
  for (const PickEdge& edge : edges) {
    if (std::find(seen.begin(), seen.end(), edge.key) == seen.end()) {
      seen.push_back(edge.key);
      ++edgeCount;
    }
  }
  view->setFilletPick(edges, pendingEdges, pendingAll);
  if (edgeCount == 0) filletCount->setText("No sharp edges");
  else if (pendingAll) filletCount->setText(QString("All %1 edges").arg(edgeCount));
  else if (pendingEdges.empty()) filletCount->setText("No edges yet");
  else filletCount->setText(QString("%1 edge%2").arg(pendingEdges.size()).arg(pendingEdges.size() == 1 ? "" : "s"));
  filletHint->setText(edgeCount == 0
                          ? "Spheres, toruses, and capsules have no sharp edge to pick."
                          : "Click an edge. Shift adds or removes. Apply rounds that selection. All edges rounds every edge. "
                            "Corners where edges meet are cut into the solid, not glued on.");
  filletPopup->show();
  placeOverlays();
}

void MainWindow::applyFillet(bool allEdges) {
  auto primitive = [](const Node* node) {
    if (!node || node->kind != Node::Kind::Shape) return false;
    const QString& shape = node->shape;
    return shape == QLatin1String("box") || shape == QLatin1String("cylinder") || shape == QLatin1String("cone") ||
           shape == QLatin1String("tube") || shape == QLatin1String("hemisphere") || shape == QLatin1String("prism") ||
           shape == QLatin1String("diamond") || shape == QLatin1String("wedge") || shape == QLatin1String("pyramid");
  };
  Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
  if (!primitive(node)) {
    status("Edge fillet is only for basic shapes.");
    return;
  }
  const std::vector<PickEdge> catalog = pickEdges(doc, *node);
  const bool edged = !catalog.empty();
  if (edgeSpin->value() > 0.02 && edged && !allEdges && pendingEdges.empty() && !pendingAll) {
    status("Click one or more edges, or choose All edges.");
    return;
  }
  if (!edged && node->kind == Node::Kind::Shape && node->edgeFillet <= 0.02 && edgeSpin->value() > 0.02 &&
      node->shape != QLatin1String("tube") && node->shape != QLatin1String("torus")) {
    status("That shape has no sharp edge to fillet.");
    return;
  }
  pushUndo();
  node->edgeFillet = std::max(0.0, edgeSpin->value());
  node->surfaceFillet = 0;
  doc.filletSegments = std::clamp(filletSegSpin->value(), 8, 72);
  QString extra;
  if (node->shape == QLatin1String("box")) {
    const double limit = 0.5 * std::min(node->size[0], std::min(node->size[1], node->size[2])) - 0.08;
    if (limit > 0.02 && node->edgeFillet > limit) {
      node->edgeFillet = limit;
      const bool was = loading;
      loading = true;
      edgeSpin->setValue(limit);
      loading = was;
      extra = " Radius was capped to the smallest side.";
    }
  }
  if (node->edgeFillet <= 0.02 && node->surfaceFillet <= 0.02) {
    node->filletEdges.clear();
    pendingEdges.clear();
    pendingAll = false;
  } else if (!edged || allEdges || pendingEdges.empty()) {
    node->filletEdges.clear();
    pendingEdges.clear();
    pendingAll = node->edgeFillet > 0.02;
  } else {
    std::sort(pendingEdges.begin(), pendingEdges.end());
    pendingEdges.erase(std::unique(pendingEdges.begin(), pendingEdges.end()), pendingEdges.end());
    node->filletEdges = pendingEdges;
    pendingAll = false;
  }
  QString done = QStringLiteral("Fillet applied. Segments stay at 72 or below.");
  if (edged) done = QStringLiteral("Edge fillet applied. Meeting corners are cut in, not glued on.");
  status(done + extra);
  refreshScene(false);
  syncFilletChrome();
}

void MainWindow::clearFillet() {
  Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
  if (!node || node->kind == Node::Kind::Pattern) return;
  pushUndo();
  node->edgeFillet = 0;
  node->surfaceFillet = 0;
  node->filletEdges.clear();
  pendingEdges.clear();
  pendingAll = false;
  const bool was = loading;
  loading = true;
  edgeSpin->setValue(0);
  surfaceSpin->setValue(0);
  loading = was;
  refreshScene(false);
  syncFilletChrome();
  status("Fillet cleared.");
}

void MainWindow::showSurfaceFilletTool() {
  if (alignPopup && alignPopup->isVisible()) alignPopup->hide();
  if (repeatPopup && repeatPopup->isVisible()) repeatPopup->hide();
  if (filletPopup) filletPopup->hide();
  if (!faceFilletPopup || !view) return;
  if (tool == 7) {
    tool = 0;
    panel = 0;
    view->setTool(0);
    faceFilletPopup->hide();
    syncActions();
    loadInspector();
    status("Fillet surface closed.");
    return;
  }
  tool = 7;
  panel = 0;
  view->setTool(7);
  faceFilletPopup->show();
  placeOverlays();
  syncActions();
  loadInspector();
  status("Click a face. Each outline of that face gets its own fillet object. Group the ones you want with the solid.");
}

bool MainWindow::edgeBetaTargetOk(const Node* node, QString* why) const {
  if (!node) {
    *why = QStringLiteral("Select one object: a part, a group or an STL. Its sharp edges show up as lines to click.");
    return false;
  }
  if (node->kind == Node::Kind::Pattern) {
    *why = QStringLiteral("Edge fillet works on a part or a group, not on a repeat. Group the repeat first.");
    return false;
  }
  if (node->shape == QLatin1String("facefillet")) {
    *why = QStringLiteral("That is a surface-fillet object. Select the solid instead.");
    return false;
  }
  const Node* cursor = node;
  while (!cursor->parentId.isEmpty()) {
    const Node* parent = doc.find(cursor->parentId);
    if (!parent) break;
    if (parent->kind != Node::Kind::Shape) {
      *why = QStringLiteral("That part is inside a group. Select the whole group to round its edges.");
      return false;
    }
    cursor = parent;
  }
  return true;
}

void MainWindow::showEdgeBetaTool() {
  if (!edgeBetaPopup || !view) return;
  if (tool == 9) {
    tool = 0;
    panel = 0;
    view->setTool(0);
    view->setFilletPick({}, {}, false);
    edgeBetaPopup->hide();
    edgeBetaTarget.clear();
    syncActions();
    loadInspector();
    status(QStringLiteral("Detected Edge Filleting closed."));
    return;
  }
  if (alignPopup) alignPopup->hide();
  if (repeatPopup) repeatPopup->hide();
  if (filletPopup) filletPopup->hide();
  if (faceFilletPopup) faceFilletPopup->hide();
  tool = 9;
  panel = 0;
  view->setTool(9);
  edgeBetaTarget.clear();
  edgeBetaSignature.clear();
  edgeBetaPopup->show();
  placeOverlays();
  syncActions();
  loadInspector();
  status(edgeBetaChains.empty()
             ? QStringLiteral("Select one object. Its sharp edges show up as lines to click.")
             : QStringLiteral("Click edges to pick them (click again to drop). Set the radius, then Apply."));
}

void MainWindow::syncEdgeBetaChrome() {
  if (!edgeBetaPopup || !view) return;
  if (tool != 9) {
    edgeBetaPopup->hide();
    return;
  }
  const Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
  QString why;
  if (!edgeBetaTargetOk(node, &why)) {
    edgeBetaTarget.clear();
    edgeBetaSignature.clear();
    edgeBetaPending.clear();
    edgeBetaChains.clear();
    edgeBetaConcave.clear();
    edgeBetaEdges.clear();
    view->setFilletPick({}, {}, false);
    edgeBetaCount->setText(QStringLiteral("No object"));
    edgeBetaHint->setText(why);
    edgeBetaPopup->show();
    placeOverlays();
    return;
  }
  // Recomputed when another object is picked or anything in the document changed (an Apply, an
  // undo, a part moved inside the group).
  const QByteArray signature = QJsonDocument(toJson(doc)).toJson(QJsonDocument::Compact);
  const bool fresh = node->id != edgeBetaTarget;
  if (fresh || signature != edgeBetaSignature) {
    EdgeBetaView result;
    QString warning;
    const Document snapshot = doc;
    const Node copy = *node;
    auto work = [&] { result = edgeBetaView(snapshot, copy, &warning); };
    if (rebuilding || !sceneIsHeavy()) work();
    else runBusy(work, false);
    edgeBetaEdges = std::move(result.edges);
    edgeBetaChains = std::move(result.chains);
    edgeBetaConcave = std::move(result.concave);
    edgeBetaLost = result.lost;
    edgeBetaPieces = result.pieces;
    edgeBetaPending = result.picked;
    if (fresh) {
      const bool was = loading;
      loading = true;
      if (result.radius > 0) {
        edgeBetaRadius->setValue(result.radius);
        if (edgeBetaShape) edgeBetaShape->setCurrentIndex(result.chamfer ? 1 : 0);
      }
      edgeBetaSegSpin->setValue(std::clamp(node->edgeBetaSegs, 2, 48));
      loading = was;
    }
    edgeBetaTarget = node->id;
    edgeBetaSignature = signature;
  }
  view->setFilletPick(edgeBetaEdges, edgeBetaPending, false);
  const int total = static_cast<int>(edgeBetaChains.size());
  if (total == 0) edgeBetaCount->setText(QStringLiteral("No sharp edges"));
  else edgeBetaCount->setText(QStringLiteral("%1 of %2 edges picked").arg(edgeBetaPending.size()).arg(total));
  QString hint = total == 0
                     ? QStringLiteral("This object has no sharp edge: every face meets its neighbours smoothly.")
                     : QStringLiteral("Click an edge to pick it, click again to drop it. Outside edges are rounded or cut off, "
                                      "inside edges are filled. Choose round or chamfer and the size, then Apply.");
  if (edgeBetaPieces > 1)
    hint += QStringLiteral(" Note: this object is %1 separate pieces that do not touch. Edges at a gap between them are "
                           "outside edges and get rounded off, not welded. Move the parts so they touch or overlap.")
                .arg(edgeBetaPieces);
  if (edgeBetaLost > 0)
    hint += QStringLiteral(" %1 earlier fillet%2 lost %3 edge after a change; pick %4 again.")
                .arg(edgeBetaLost)
                .arg(edgeBetaLost == 1 ? QString() : QStringLiteral("s"))
                .arg(edgeBetaLost == 1 ? QStringLiteral("its") : QStringLiteral("their"))
                .arg(edgeBetaLost == 1 ? QStringLiteral("it") : QStringLiteral("them"));
  edgeBetaHint->setText(hint);
  edgeBetaPopup->show();
  placeOverlays();
}

void MainWindow::applyEdgeBetaFillet() {
  Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
  QString why;
  if (!edgeBetaTargetOk(node, &why) || node->id != edgeBetaTarget) {
    status(why.isEmpty() ? QStringLiteral("Select the object again, then pick its edges.") : why);
    return;
  }
  const double radius = edgeBetaRadius->value();
  const bool chamfer = edgeBetaShape && edgeBetaShape->currentIndex() == 1;
  std::vector<double> stored;
  int count = 0;
  for (int key : edgeBetaPending) {
    if (key < 0 || key >= static_cast<int>(edgeBetaChains.size())) continue;
    const std::vector<double>& line = edgeBetaChains[static_cast<size_t>(key)];
    if (line.size() < 6) continue;
    stored.push_back(chamfer ? -radius : radius);  // a chamfer is stored negative
    stored.push_back(static_cast<double>(line.size() / 3));
    stored.insert(stored.end(), line.begin(), line.end());
    ++count;
  }
  if (count == 0 && node->edgeBeta.empty()) {
    status(QStringLiteral("Click one or more edges first."));
    return;
  }
  pushUndo();
  node->edgeBeta = std::move(stored);
  node->edgeBetaSegs = edgeBetaSegSpin->value();
  status(count == 0 ? QStringLiteral("Edge fillets removed.")
                    : QStringLiteral("%1 %2 edge%3 at %4 mm.")
                          .arg(chamfer ? QStringLiteral("Chamfered") : QStringLiteral("Rounded"))
                          .arg(count)
                          .arg(count == 1 ? QString() : QStringLiteral("s"))
                          .arg(radius));
  refreshScene(false);
  loadInspector();
}

void MainWindow::clearEdgeBetaFillet() {
  Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
  if (!node || node->edgeBeta.empty()) {
    edgeBetaPending.clear();
    syncEdgeBetaChrome();
    status(QStringLiteral("This object has no edge fillets."));
    return;
  }
  pushUndo();
  node->edgeBeta.clear();
  edgeBetaPending.clear();
  status(QStringLiteral("Edge fillets removed."));
  refreshScene(false);
  loadInspector();
}

void MainWindow::showFilletLog(const QStringList& lines) {
  if (!consoleDialog) {
    consoleDialog = new QDialog(this);
    consoleDialog->setWindowTitle(QStringLiteral("Console"));
    consoleDialog->resize(760, 460);
    auto* layout = new QVBoxLayout(consoleDialog);
    consoleLog = new QPlainTextEdit(consoleDialog);
    consoleLog->setReadOnly(true);
    consoleLog->setLineWrapMode(QPlainTextEdit::NoWrap);
    consoleLog->setStyleSheet(QStringLiteral("font-family: Consolas, monospace; font-size: 12px;"));
    layout->addWidget(consoleLog);
  }
  if (!lines.isEmpty()) {
    consoleLog->appendPlainText(lines.join(QLatin1Char('\n')));
    consoleLog->appendPlainText(QString());
  }
  consoleDialog->show();
  consoleDialog->raise();
  consoleDialog->activateWindow();
}

void MainWindow::filletPickedSurface(const QString& pickedId, const QVector<QVector3D>& loop, const QVector<QVector3D>& normals,
                                     const QVector3D& hit) {
  const Node* picked = doc.find(pickedId);
  if (!picked || picked->shape == QLatin1String("facefillet") || loop.size() < 3) {
    status("Click a solid face to round its edges.");
    return;
  }
  // A part inside a group is filleted as the group: its face is the group's face there (cut short
  // where another part of the group stands on it), not the part's own outline.
  QString id = pickedId;
  for (const Node* cursor = picked; cursor && !cursor->parentId.isEmpty();) {
    const Node* parent = doc.find(cursor->parentId);
    if (!parent || parent->kind != Node::Kind::Group) break;
    id = parent->id;
    cursor = parent;
  }
  const bool inGroup = id != pickedId;
  const Node* source = doc.find(id);  // (a group is drawn as one item, so its id comes straight from the picker)
  const QString pickedFrameId = pickedId;
  const double radius = faceFilletRadius ? faceFilletRadius->value() : 2.0;
  const int segs = faceFilletSegs ? faceFilletSegs->value() : 12;
  const bool outer = faceFilletSide && faceFilletSide->currentIndex() == 1;
  const bool up = faceFilletDir && faceFilletDir->currentIndex() == 1;
  if (radius < 0.1) {
    status("Set a fillet radius first.");
    return;
  }
  QStringList extraTargets;
  for (const QString& selectedId : selection) {
    if (selectedId.isEmpty() || selectedId == id || extraTargets.contains(selectedId)) continue;
    const Node* node = doc.find(selectedId);
    if (!node || node->shape == QLatin1String("facefillet")) continue;
    extraTargets << selectedId;
  }
  // The viewport already walked the clicked face and handed back its exact outlines (outer outline
  // first, then holes, separated by a far-away marker point). They are in the item's local frame.
  std::vector<std::vector<QVector3D>> localRings(1);
  for (const QVector3D& point : loop) {
    if (std::abs(point.x()) > 1e19f) {
      if (!localRings.back().empty()) localRings.emplace_back();
      continue;
    }
    localRings.back().push_back(point);
  }
  while (!localRings.empty() && localRings.back().size() < 3) localRings.pop_back();
  if (localRings.empty()) {
    status("That face has no usable outline.");
    return;
  }
  const QVector3D localPoint = localRings.front().front();
  QVector3D localNormal(0, 0, 1);
  if (!normals.isEmpty()) localNormal = normals.front();
  if (localNormal.lengthSquared() < 1e-12f) localNormal = QVector3D(0, 0, 1);
  const QString op = newId();
  const QColor color = source->color;
  const Document snapshot = doc;
  status("Filleting that face...");
  QPointer<MainWindow> self(this);
  const double hitPt[3] = {hit.x(), hit.y(), hit.z()};
  std::thread([self, snapshot, extraTargets, id, pickedFrameId, inGroup, hitPt = std::array<double, 3>{hitPt[0], hitPt[1], hitPt[2]},
               localRings, localPoint, localNormal, radius, segs, outer, up, op, color]() mutable {
    QString warning;
    clearFilletLog();
    // The picker's outline and normal are in the frame of the part that was clicked.
    const QMatrix4x4 world = nodeWorld(snapshot, pickedFrameId);
    bool invertible = false;
    const QMatrix4x4 inverse = world.inverted(&invertible);
    QVector3D normal = invertible ? inverse.transposed().mapVector(localNormal) : localNormal;
    if (normal.lengthSquared() < 1e-12f) normal = QVector3D(0, 0, 1);
    normal.normalize();
    const QVector3D origin = world.map(localPoint);
    QVector3D axis = std::abs(normal.z()) < 0.9f ? QVector3D(0, 0, 1) : QVector3D(1, 0, 0);
    axis = axis - normal * QVector3D::dotProduct(axis, normal);
    if (axis.lengthSquared() < 1e-12f) axis = QVector3D(1, 0, 0);
    axis.normalize();
    const QVector3D bitangent = QVector3D::crossProduct(normal, axis);
    double originPt[3] = {origin.x(), origin.y(), origin.z()};
    const double faceN[3] = {normal.x(), normal.y(), normal.z()};
    const double faceA[3] = {axis.x(), axis.y(), axis.z()};

    struct Outline {
      QString target;
      std::vector<double> points;
      std::vector<double> walls;   // the solid's face across each edge; empty means assume square walls
    };
    std::vector<Outline> outlines;
    // The picked outline, in world space. The picker welds vertices within 0.05 mm and snaps them
    // onto the plane, so this is only an approximation of the face: good enough to say WHICH face
    // was clicked, not good enough to cut with. Rings are forced to the convention the cutter
    // expects (outer counter-clockwise about the normal, holes clockwise); a mirrored item flips it.
    std::vector<std::vector<double>> pickedRings;
    for (size_t r = 0; r < localRings.size(); ++r) {
      std::vector<double> points;
      double area = 0;
      std::vector<QVector3D> ring;
      for (const QVector3D& local : localRings[r]) ring.push_back(world.map(local));
      for (size_t i = 0; i < ring.size(); ++i) {
        const QVector3D a = ring[i] - origin;
        const QVector3D b = ring[(i + 1) % ring.size()] - origin;
        area += double(QVector3D::dotProduct(a, axis)) * double(QVector3D::dotProduct(b, bitangent)) -
                double(QVector3D::dotProduct(b, axis)) * double(QVector3D::dotProduct(a, bitangent));
      }
      if ((r == 0) != (area > 0)) std::reverse(ring.begin(), ring.end());
      for (const QVector3D& point : ring) {
        points.push_back(point.x());
        points.push_back(point.y());
        points.push_back(point.z());
      }
      pickedRings.push_back(std::move(points));
    }
    // Cut with the solid's own outline instead, found by matching the picked one against the flat
    // faces on this plane. Falls back to the picked outline when nothing matches.
    // The face under the click on the finished solid. A group is drawn as its parts laid on top of
    // each other, so the outline the picker walked may be one part's own face running on inside
    // another part; the solid's face there is the real one. Matching the picked outline is the
    // fallback for a click the point lookup misses.
    std::vector<std::vector<double>> exactWalls;
    std::vector<std::vector<double>> exact = planarFaceRingsAt(snapshot, id, originPt, faceN, faceA, hitPt.data(), &warning, &exactWalls);
    if (exact.empty()) exact = planarFaceRings(snapshot, id, originPt, faceN, faceA, &warning, &pickedRings.front(), &exactWalls);
    const Node* target = snapshot.find(id);
    if ((inGroup || (target && target->kind == Node::Kind::Group)) && exact.empty()) {
      // The part's own outline would fillet the part as if it stood alone: better nothing.
      QStringList log = takeFilletLog();
      QMetaObject::invokeMethod(self.data(), [self, log] {
        if (!self) return;
        self->showFilletLog(log);
        self->status(QStringLiteral("Could not find that face on the group. Try clicking nearer its middle."));
      }, Qt::QueuedConnection);
      return;
    }
    if (!exact.empty() && exact.front().size() >= 3) {
      // Anchor the plane on a real vertex of that face, so the outline sits exactly on it.
      for (int i = 0; i < 3; ++i) originPt[i] = exact.front()[static_cast<size_t>(i)];
    }
    if (!exact.empty()) {
      for (size_t k = 0; k < exact.size(); ++k)
        outlines.push_back({id, std::move(exact[k]), k < exactWalls.size() ? std::move(exactWalls[k]) : std::vector<double>{}});
    } else {
      for (std::vector<double>& ring : pickedRings) outlines.push_back({id, std::move(ring), {}});
    }
    // Other selected solids: every flat face of theirs that lies on the same plane.
    for (const QString& targetId : extraTargets) {
      std::vector<std::vector<double>> walls;
      std::vector<std::vector<double>> rings =
          planarFaceRings(snapshot, targetId, originPt, faceN, faceA, &warning, nullptr, &walls);
      for (size_t k = 0; k < rings.size(); ++k)
        outlines.push_back({targetId, std::move(rings[k]), k < walls.size() ? std::move(walls[k]) : std::vector<double>{}});
    }

    std::vector<Node> cutters;
    int nameIndex = 1;
    std::map<QString, int> ringIndex;
    for (Outline& outline : outlines) {
      const Node* target = snapshot.find(outline.target);
      Node cutter;
      cutter.id = newId();
      cutter.kind = Node::Kind::Shape;
      cutter.shape = QStringLiteral("facefillet");
      cutter.hole = !outer;
      cutter.name = QStringLiteral("fillet_%1").arg(nameIndex++);
      cutter.radius = radius;
      cutter.sides = std::clamp(segs, 1, 20);
      cutter.color = target ? target->color : color;
      cutter.filletOf = outline.target;
      cutter.filletOp = op;
      cutter.filletIndex = ringIndex[outline.target]++;
      cutter.filletOuter = outer;
      cutter.filletUp = up;
      cutter.faceLoop = std::move(outline.points);
      cutter.facePointNormals = std::move(outline.walls);
      for (int i = 0; i < 3; ++i) {
        cutter.pos[i] = 0;
        cutter.rot[i] = 0;
        cutter.scale[i] = 1;
        cutter.faceOrigin[i] = originPt[i];
        cutter.faceNormal[i] = faceN[i];
        cutter.faceAxis[i] = faceA[i];
      }
      warmShape(cutter, snapshot, &warning);
      cutters.push_back(std::move(cutter));
    }
    if (!self) return;
    QStringList log = takeFilletLog();
    QMetaObject::invokeMethod(self.data(), [self, cutters = std::move(cutters), warning, log]() mutable {
      if (!self) return;
      if (cutters.empty()) {
        self->showFilletLog(log);
        self->status(warning.isEmpty() ? QStringLiteral("That face did not produce a fillet cutter.") : warning);
        return;
      }
      self->pushUndo();
      QStringList ids;
      for (Node& cutter : cutters) {
        ids.push_back(cutter.id);
        self->doc.nodes.push_back(std::move(cutter));
      }
      self->selection = ids;
      self->refreshTree();
      self->loadInspector();
      self->refreshScene(false);
      self->status(warning.isEmpty() ? QStringLiteral("Fillet objects created, one for each outline. Group one with the solid to apply it, or delete the ones you do not want.")
                                     : warning);
    }, Qt::QueuedConnection);
  }).detach();
}
void MainWindow::showRepeatTool() {
  if (!repeatPopup) return;
  if (repeatPopup->isVisible()) {
    repeatPopup->hide();
    syncActions();
    loadInspector();
    status("Repeat closed.");
    return;
  }
  if (alignPopup) alignPopup->hide();
  if (filletPopup) filletPopup->hide();
  if (faceFilletPopup) faceFilletPopup->hide();
  if (edgeBetaPopup) edgeBetaPopup->hide();
  if (tool == 3 || tool == 4 || tool == 6 || tool == 7 || tool == 8 || tool == 9 || tool == 12 || tool == 13) {
    tool = 0;
    panel = 0;
    if (view) view->setTool(0);
    if (view) view->setFilletPick({}, {}, false);
    filletTarget.clear();
  }
  repeatPopup->show();
  syncActions();
  loadInspector();
  status(selection.isEmpty() ? "Select something to repeat. Set copies, move, rotate, and resize, then Run."
                             : "Set copies, move, rotate, and resize, then Run. Each copy is its own object.");
}

void MainWindow::runRepeat() {
  if (selection.isEmpty()) {
    status("Select something to repeat.");
    return;
  }
  const int copies = std::max(1, repeatCount ? repeatCount->value() : 1);
  double move[3];
  double drot[3];
  double resize[3];
  for (int axis = 0; axis < 3; ++axis) {
    move[axis] = repeatMove[axis] ? repeatMove[axis]->value() : 0;
    drot[axis] = repeatRot[axis] ? repeatRot[axis]->value() : 0;
    resize[axis] = repeatResize[axis] ? repeatResize[axis]->value() : 1;
    if (std::abs(resize[axis]) < 1e-4) resize[axis] = resize[axis] < 0 ? -1e-4 : 1e-4;
  }
  struct Pose {
    QString id;
    QString name;
    double pos[3];
    double rot[3];
    double scale[3];
    double center[3];
    bool haveCenter = false;
  };
  std::vector<Pose> poses;
  for (const QString& id : selection) {
    bool parentAlso = false;
    const Node* walk = doc.find(id);
    for (int guard = 0; walk && !walk->parentId.isEmpty() && guard < 32; ++guard) {
      if (selection.contains(walk->parentId)) {
        parentAlso = true;
        break;
      }
      walk = doc.find(walk->parentId);
    }
    if (parentAlso) continue;
    const Node* node = doc.find(id);
    if (!node) continue;
    Pose pose;
    pose.id = node->id;
    pose.name = node->name;
    for (int axis = 0; axis < 3; ++axis) {
      pose.pos[axis] = node->pos[axis];
      pose.rot[axis] = node->rot[axis];
      pose.scale[axis] = node->scale[axis];
    }
    double lo[3];
    double hi[3];
    if (localBounds(doc, *node, lo, hi)) {
      pose.haveCenter = true;
      for (int axis = 0; axis < 3; ++axis) pose.center[axis] = (lo[axis] + hi[axis]) * 0.5;
    }
    poses.push_back(pose);
  }
  if (poses.empty()) {
    status("Select the parent. Children are copied with it.");
    return;
  }
  pushUndo();
  QStringList made;
  for (const Pose& pose : poses) {
    for (int i = 1; i <= copies; ++i) {
      const int before = static_cast<int>(doc.nodes.size());
      doc.duplicate(pose.id);
      if (static_cast<int>(doc.nodes.size()) <= before) continue;
      Node& copy = doc.nodes[static_cast<size_t>(before)];
      copy.name = pose.name + QString(" %1").arg(i);
      double scale[3];
      for (int axis = 0; axis < 3; ++axis) {
        const double factor = std::pow(resize[axis], i);
        scale[axis] = pose.scale[axis] * (std::abs(factor) < 1e-4 ? (factor < 0 ? -1e-4 : 1e-4) : factor);
        copy.rot[axis] = pose.rot[axis] + drot[axis] * static_cast<double>(i);
        copy.scale[axis] = scale[axis];
      }
      // Spin and resize around the local center. Adding the angle onto the
      // node origin left SVG and other off-center meshes orbiting a corner.
      const double zero[3] = {0, 0, 0};
      double rot[3] = {copy.rot[0], copy.rot[1], copy.rot[2]};
      QVector3D placed = QVector3D(pose.pos[0], pose.pos[1], pose.pos[2]) +
                         QVector3D(move[0], move[1], move[2]) * static_cast<float>(i);
      if (pose.haveCenter) {
        const QVector3D localCenter(pose.center[0], pose.center[1], pose.center[2]);
        const QVector3D worldCenter = placementMatrix(pose.pos, pose.rot, zero, pose.scale).map(localCenter);
        const QVector3D shifted = placementMatrix(zero, rot, zero, scale).map(localCenter);
        placed = worldCenter + QVector3D(move[0], move[1], move[2]) * static_cast<float>(i) - shifted;
      }
      copy.pos[0] = placed.x();
      copy.pos[1] = placed.y();
      copy.pos[2] = placed.z();
      tidyPlacement(copy);
      made << copy.id;
    }
  }
  if (!made.isEmpty()) selection = made;
  refreshTree();
  loadInspector();
  refreshScene();
  status(QString("Made %1 separate %2. The original stays.").arg(made.size()).arg(made.size() == 1 ? "copy" : "copies"));
}

// ---------------------------------------------------------------------------------------------
// Generators

Node* MainWindow::selectedGenerator() {
  if (selection.size() != 1) return nullptr;
  Node* node = doc.find(selection.front());
  return node && node->kind == Node::Kind::Shape && node->shape == QLatin1String("generator") ? node : nullptr;
}

void MainWindow::placeGenerator(const QString& id, double x, double y) {
  const gen::Spec* spec = gen::spec(id);
  if (!spec) {
    status(QStringLiteral("Unknown generator: %1").arg(id));
    return;
  }
  pushUndo();
  const QString nodeId = doc.addShape(QStringLiteral("generator"), x, y);
  Node* node = doc.find(nodeId);
  if (!node) return;
  node->genType = id;
  node->genParams = gen::defaults(id);
  node->name = spec->name;
  // Build once (with the busy bar for the slower ones) to learn the size, then stand it on the plate.
  const Document snapshot = doc;
  const Node copy = *node;
  double lo[3];
  double hi[3];
  bool known = false;
  runBusy([&] { known = localBounds(snapshot, copy, lo, hi); }, false);
  if (!known) known = localBounds(doc, *node, lo, hi);
  node = doc.find(nodeId);
  if (node && known) {
    for (int axis = 0; axis < 3; ++axis) node->size[axis] = hi[axis] - lo[axis];
    node->pos[2] = doc.workplaneZ - lo[2];
    tidyPlacement(*node);
  }
  standOnWorkplane(nodeId, placingDropped ? placingU : 0, placingDropped ? placingV : 0);
  selection = QStringList{nodeId};
  view->focusOn(selection);  // the view turns around the new part
  if (tool != 0 || panel != 0) {
    tool = 0;
    panel = 0;
    if (view) view->setTool(0);
    syncActions();
  }
  refreshTree();
  loadInspector();
  refreshScene();
  placeOverlays();
  status(QStringLiteral("Added %1. Its settings are in the tool window; Convert to parts makes plain parts of it.").arg(spec->name));
}

void MainWindow::buildGeneratorPage() {
  genPage = new QWidget(this);
  auto* layout = new QVBoxLayout(genPage);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(6);
  genScroll = new QScrollArea(genPage);
  genScroll->setWidgetResizable(true);
  genScroll->setFrameShape(QFrame::NoFrame);
  genScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  genScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
  genInner = new QWidget;
  genInner->setObjectName(QStringLiteral("genInner"));
  genInner->setStyleSheet(QStringLiteral("QWidget#genInner { background: #0b0d11; }"));
  genForm = new QFormLayout(genInner);
  genForm->setContentsMargins(0, 0, 6, 0);
  genForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  // A long label puts its field on the next line instead of pushing it out of the window.
  genForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
  genScroll->setWidget(genInner);
  genScroll->viewport()->setAutoFillBackground(false);
  layout->addWidget(genScroll);
  genScaleNote = new QLabel(genPage);
  genScaleNote->setWordWrap(true);
  genScaleNote->setStyleSheet(QStringLiteral("color: #e8b060;"));
  genResetScale = new QPushButton(QStringLiteral("Reset scale"), genPage);
  layout->addWidget(genScaleNote);
  layout->addWidget(genResetScale);
  auto* buttons = new QHBoxLayout;
  auto* defaultsButton = new QPushButton(QStringLiteral("Defaults"), genPage);
  defaultsButton->setToolTip(QStringLiteral("Put every setting of this generator back to its default."));
  auto* convertButton = new QPushButton(QStringLiteral("Convert to parts"), genPage);
  convertButton->setToolTip(QStringLiteral("Replace the generator by plain parts (one per piece, grouped). "
                                           "Its settings cannot be changed after that."));
  buttons->addWidget(defaultsButton);
  buttons->addWidget(convertButton, 1);
  genMatchHole = new QPushButton(QStringLiteral("Add matching hole"), genPage);
  genMatchHole->setToolTip(QStringLiteral("Add this part's hole, lined up with it, and group it with the part it sits in so "
                                          "it cuts only that part."));
  layout->addWidget(genMatchHole);
  layout->addLayout(buttons);
  genTimer = new QTimer(this);
  genTimer->setSingleShot(true);
  genTimer->setInterval(180);
  connect(genTimer, &QTimer::timeout, this, [this] { applyGeneratorChange(); });
  connect(defaultsButton, &QPushButton::clicked, this, [this] {
    Node* node = selectedGenerator();
    if (!node) return;
    pushUndo();
    QVector3D lo;
    QVector3D hi;
    if (!genBottomKnown && alignmentWorldBounds(doc, *node, &lo, &hi)) {
      genBottom = lo.z();
      genBottomKnown = true;
    }
    genPendingId = node->id;
    node->genParams = gen::defaults(node->genType);
    genFormFor.clear();
    loadGeneratorPage(node);
    applyGeneratorChange();
  });
  connect(genResetScale, &QPushButton::clicked, this, [this] {
    Node* node = selectedGenerator();
    if (!node) return;
    pushUndo();
    QVector3D lo;
    QVector3D hi;
    const bool before = alignmentWorldBounds(doc, *node, &lo, &hi);
    const float bottom = lo.z();
    for (double& s : node->scale) s = s < 0 ? -1.0 : 1.0;
    if (before && alignmentWorldBounds(doc, *node, &lo, &hi)) node->pos[2] += bottom - lo.z();
    tidyPlacement(*node);
    refreshScene();
    loadInspector();
    placeOverlays();
  });
  connect(convertButton, &QPushButton::clicked, this, [this] { convertGeneratorToParts(); });
  connect(genMatchHole, &QPushButton::clicked, this, [this] { addMatchingHole(); });
}

void MainWindow::loadGeneratorPage(const Node* node) {
  if (!genForm || !node) return;
  const gen::Spec* spec = gen::spec(node->genType);
  const QString key = node->id + QLatin1Char('|') + node->genType;
  if (key != genFormFor) {
    genFormFor = key;
    genRows.clear();
    while (genForm->rowCount() > 0) genForm->removeRow(0);
    if (!spec) {
      auto* missing = new QLabel(QStringLiteral("This generator is not part of this version of DaedalCAD."), genInner);
      missing->setWordWrap(true);
      genForm->addRow(missing);
    } else {
      for (const gen::Param& p : spec->params) {
        const QString pkey = p.key;
        QWidget* field = nullptr;
        switch (p.type) {
          case gen::Param::Heading: {
            auto* label = new QLabel(p.label, genInner);
            label->setStyleSheet(QStringLiteral("font-weight: 600; color: #e87400; padding-top: 4px;"));
            genForm->addRow(label);
            genRows.push_back({pkey, label});
            continue;
          }
          case gen::Param::Number: {
            auto* box = new QDoubleSpinBox(genInner);
            box->setRange(p.min, p.max);
            box->setSingleStep(p.step);
            box->setDecimals(p.decimals);
            box->setSuffix(p.suffix);
            box->setKeyboardTracking(true);
            box->installEventFilter(this);
            connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [this, pkey](double value) { setGeneratorValue(pkey, value); });
            field = box;
            break;
          }
          case gen::Param::Integer: {
            auto* box = new QSpinBox(genInner);
            box->setRange(static_cast<int>(p.min), static_cast<int>(p.max));
            box->setSuffix(p.suffix);
            box->installEventFilter(this);
            connect(box, qOverload<int>(&QSpinBox::valueChanged), this,
                    [this, pkey](int value) { setGeneratorValue(pkey, value); });
            field = box;
            break;
          }
          case gen::Param::Choice: {
            auto* box = new QComboBox(genInner);
            for (const auto& option : p.choices) box->addItem(option.second, option.first);
            connect(box, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, pkey, box](int) {
              if (loading) return;
              pushUndo();
              setGeneratorValue(pkey, box->currentData().toString());
            });
            field = box;
            break;
          }
          case gen::Param::Text: {
            auto* line = new QLineEdit(genInner);
            line->setMaxLength(static_cast<int>(p.max));
            line->installEventFilter(this);
            connect(line, &QLineEdit::textEdited, this, [this, pkey](const QString& text) { setGeneratorValue(pkey, text); });
            field = line;
            break;
          }
          case gen::Param::Picture: {
            auto* button = new QPushButton(QStringLiteral("Choose a picture…"), genInner);
            button->setProperty("picture", true);
            button->setIconSize(QSize(64, 48));
            connect(button, &QPushButton::clicked, this, [this, pkey] {
              const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Picture"), QString(),
                                                                QStringLiteral("Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.webp)"));
              if (path.isEmpty()) return;
              const QString value = gen::pictureValue(path);
              if (value.isEmpty()) {
                status(QStringLiteral("That picture could not be read."));
                return;
              }
              pushUndo();
              setGeneratorValue(pkey, value);
              status(QStringLiteral("Picture: %1.").arg(QFileInfo(path).fileName()));
            });
            field = button;
            break;
          }
          case gen::Param::Toggle: {
            auto* box = new QCheckBox(p.label, genInner);
            connect(box, &QCheckBox::toggled, this, [this, pkey](bool on) {
              if (loading) return;
              pushUndo();
              setGeneratorValue(pkey, on);
            });
            field = box;
            break;
          }
        }
        if (!field) continue;
        field->setToolTip(p.tip);
        if (p.type == gen::Param::Toggle) genForm->addRow(QString(), field);
        else genForm->addRow(p.label, field);
        if (QWidget* label = genForm->labelForField(field)) label->setToolTip(p.tip);
        genRows.push_back({pkey, field});
      }
    }
  }
  syncGeneratorRows(node);
}

// Values into the fields (without firing them), rows shown or hidden by the other settings, and
// the note about scaling.
void MainWindow::syncGeneratorRows(const Node* node) {
  const gen::Spec* spec = node ? gen::spec(node->genType) : nullptr;
  if (!spec || !genForm) return;
  const QJsonObject values = gen::normalized(node->genType, node->genParams);
  const bool wasLoading = loading;
  loading = true;
  bool headingShown = false;
  QWidget* lastHeading = nullptr;
  bool anyUnderHeading = false;
  auto closeHeading = [&] {
    if (lastHeading) genForm->setRowVisible(lastHeading, anyUnderHeading);
  };
  for (const auto& [key, field] : genRows) {
    const gen::Param* param = nullptr;
    for (const gen::Param& p : spec->params)
      if (p.key == key) param = &p;
    if (!param) continue;
    if (param->type == gen::Param::Heading) {
      closeHeading();
      lastHeading = field;
      anyUnderHeading = false;
      headingShown = true;
      continue;
    }
    const bool shown = gen::visible(*param, values);
    genForm->setRowVisible(field, shown);
    anyUnderHeading = anyUnderHeading || shown;
    const QJsonValue value = values.value(key);
    if (auto* box = qobject_cast<QDoubleSpinBox*>(field)) {
      // Leave the box alone while it is being typed in.
      if (!box->hasFocus() && std::abs(box->value() - value.toDouble()) > 1e-9) {
        const QSignalBlocker blocker(box);
        box->setValue(value.toDouble());
      }
    } else if (auto* box = qobject_cast<QSpinBox*>(field)) {
      if (!box->hasFocus() && box->value() != value.toInt()) {
        const QSignalBlocker blocker(box);
        box->setValue(value.toInt());
      }
    } else if (auto* box = qobject_cast<QComboBox*>(field)) {
      const int index = box->findData(value.toString());
      if (index >= 0 && index != box->currentIndex()) {
        const QSignalBlocker blocker(box);
        box->setCurrentIndex(index);
      }
    } else if (auto* button = qobject_cast<QPushButton*>(field); button && button->property("picture").toBool()) {
      // The picture as a small thumbnail on the button.
      const QString data = value.toString();
      if (button->property("shown").toString() != data) {
        button->setProperty("shown", data);
        QImage image = QImage::fromData(QByteArray::fromBase64(data.toLatin1()));
        button->setIcon(image.isNull() ? QIcon() : QIcon(QPixmap::fromImage(image.scaled(64, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
        button->setText(image.isNull() ? QStringLiteral("Choose a picture…") : QStringLiteral("Change…"));
      }
    } else if (auto* line = qobject_cast<QLineEdit*>(field)) {
      if (!line->hasFocus() && line->text() != value.toString()) {
        const QSignalBlocker blocker(line);
        line->setText(value.toString());
      }
    } else if (auto* box = qobject_cast<QCheckBox*>(field)) {
      if (key == QLatin1String("usePrinter"))
        box->setText(QStringLiteral("Use printer clearance (%1 mm)").arg(printerClearance(), 0, 'f', 2));
      if (box->isChecked() != value.toBool()) {
        const QSignalBlocker blocker(box);
        box->setChecked(value.toBool());
      }
    }
  }
  if (headingShown) closeHeading();
  loading = wasLoading;
  const bool scaled = std::abs(std::abs(node->scale[0]) - 1) > 1e-6 || std::abs(std::abs(node->scale[1]) - 1) > 1e-6 ||
                      std::abs(std::abs(node->scale[2]) - 1) > 1e-6;
  if (genScaleNote) {
    genScaleNote->setText(QStringLiteral("Scaled to %1 x %2 x %3 %. Clearances and threads are scaled too, so parts may no "
                                         "longer fit. Change the size in the settings instead.")
                              .arg(std::abs(node->scale[0]) * 100, 0, 'f', 0)
                              .arg(std::abs(node->scale[1]) * 100, 0, 'f', 0)
                              .arg(std::abs(node->scale[2]) * 100, 0, 'f', 0));
    genScaleNote->setVisible(scaled);
  }
  if (genResetScale) genResetScale->setVisible(scaled);
  if (genMatchHole) genMatchHole->setVisible(node->genType.startsWith(QLatin1String("std")) && !gen::makesHole(node->genType, values));
}

void MainWindow::setGeneratorValue(const QString& key, const QJsonValue& value) {
  if (loading) return;
  Node* node = selectedGenerator();
  if (!node) return;
  if (key == QLatin1String("make") && node->genType.startsWith(QLatin1String("std"))) {
    switchStandardPart(value.toString());
    return;
  }
  if (!genBottomKnown || genPendingId != node->id) {
    // Remember where its bottom is now, so the rebuilt part keeps standing on the same spot.
    QVector3D lo;
    QVector3D hi;
    genBottomKnown = alignmentWorldBounds(doc, *node, &lo, &hi);
    genBottom = lo.z();
    genPendingId = node->id;
  }
  QJsonObject params = node->genParams;
  params[key] = value;
  node->genParams = gen::normalized(node->genType, params);
  if (key == QLatin1String("make")) {
    // A pattern's holes only: a hole shape, which cuts the parts in its group.
    const bool hole = gen::makesHole(node->genType, node->genParams);
    if (hole != node->hole) {
      node->hole = hole;
      refreshTree();
      status(hole ? QStringLiteral("Holes only: group it with a part to cut the pattern into that part.")
                  : QStringLiteral("A solid part again."));
    }
  }
  syncGeneratorRows(node);
  placeOverlays();
  genTimer->start();
}

void MainWindow::applyGeneratorChange() {
  if (rebuilding) {
    // A rebuild is running; come back when it is done.
    genTimer->start();
    return;
  }
  Node* node = doc.find(genPendingId);
  const bool keepBottom = genBottomKnown;
  const double bottom = genBottom;
  genBottomKnown = false;
  if (!node || node->shape != QLatin1String("generator")) return;
  const Document snapshot = doc;
  const Node copy = *node;
  double lo[3];
  double hi[3];
  bool known = false;
  runBusy([&] { known = localBounds(snapshot, copy, lo, hi); }, false);
  node = doc.find(genPendingId);
  if (!node) return;
  if (known) {
    for (int axis = 0; axis < 3; ++axis) node->size[axis] = hi[axis] - lo[axis];
    QVector3D wlo;
    QVector3D whi;
    if (keepBottom && alignmentWorldBounds(doc, *node, &wlo, &whi)) node->pos[2] += bottom - wlo.z();
    tidyPlacement(*node);
  }
  refreshScene();
  if (posSpin[2] && selection.size() == 1 && selection.front() == node->id) {
    const QSignalBlocker blocker(posSpin[2]);
    posSpin[2]->setValue(node->pos[2]);
  }
  placeOverlays();
}

// The local offset that keeps a standard part's own origin (the underside of a screw head, the
// bottom of a nut...) in place when the part is rebuilt with other settings: its new position.
namespace {
QVector3D frameShift(const Node& node, const manifold::vec3& before, const manifold::vec3& after) {
  QVector3D shift(static_cast<float>(after.x - before.x), static_cast<float>(after.y - before.y),
                  static_cast<float>(after.z - before.z));
  for (int axis = 0; axis < 3; ++axis)
    if (node.mirror[axis]) shift[axis] = -shift[axis];
  const double zero[3] = {0, 0, 0};
  return placementMatrix(node.pos, node.rot, zero, node.scale).map(shift);
}
}  // namespace

// Part <-> matching hole: the hole takes the part's place exactly, so it can be switched back too.
void MainWindow::switchStandardPart(const QString& make) {
  Node* node = selectedGenerator();
  if (!node) return;
  const QString nodeId = node->id;
  const QString type = node->genType;
  const QJsonObject before = gen::normalized(type, node->genParams);
  QJsonObject after = before;
  after[QStringLiteral("make")] = make;
  after = gen::normalized(type, after);
  if (after == before) return;
  manifold::vec3 c0, c1;
  bool known = false;
  const double clearance = printerClearance();
  runBusy([&] { known = gen::frameCentre(type, before, clearance, &c0) && gen::frameCentre(type, after, clearance, &c1); }, false);
  node = doc.find(nodeId);
  if (!node) return;
  if (known) {
    const QVector3D at = frameShift(*node, c0, c1);
    for (int axis = 0; axis < 3; ++axis) node->pos[axis] = at[axis];
  }
  node->genParams = after;
  node->hole = gen::makesHole(type, after);
  genBottomKnown = false;
  genPendingId = nodeId;
  syncGeneratorRows(node);
  refreshTree();
  loadInspector();
  genTimer->start();
  status(node->hole ? QStringLiteral("Now the matching hole, in the part's place. It cuts the parts in its group.")
                    : QStringLiteral("Now the part again."));
}

// Adds the selected standard part's hole where the part is, and puts it in a group with the part it
// sits in (the one its box overlaps most), so it cuts that part and not the fastener itself.
void MainWindow::addMatchingHole() {
  Node* node = selectedGenerator();
  if (!node || !node->genType.startsWith(QLatin1String("std"))) return;
  if (genTimer && genTimer->isActive()) {
    genTimer->stop();
    applyGeneratorChange();
    node = selectedGenerator();
    if (!node) return;
  }
  const Node source = *node;
  const QJsonObject partValues = gen::normalized(source.genType, source.genParams);
  QJsonObject holeValues = partValues;
  holeValues[QStringLiteral("make")] = QStringLiteral("hole");
  holeValues = gen::normalized(source.genType, holeValues);
  manifold::vec3 c0, c1;
  bool known = false;
  std::vector<gen::Piece> pieces;
  const double clearance = printerClearance();
  runBusy([&] {
    known = gen::frameCentre(source.genType, partValues, clearance, &c0);
    pieces = gen::build(source.genType, holeValues, nullptr, clearance, &c1);
  }, true);
  if (!known || pieces.empty()) {
    status(QStringLiteral("Could not make the matching hole with these settings."));
    return;
  }
  pushUndo();
  Node hole = source;
  hole.id = newId();
  hole.genParams = holeValues;
  hole.hole = true;
  hole.name = source.name + QStringLiteral(" hole");
  const QVector3D at = frameShift(source, c0, c1);
  for (int axis = 0; axis < 3; ++axis) hole.pos[axis] = at[axis];
  double lo[3] = {1e18, 1e18, 1e18}, hi[3] = {-1e18, -1e18, -1e18};
  for (const gen::Piece& piece : pieces) {
    const auto box = piece.solid.BoundingBox();
    lo[0] = std::min(lo[0], box.min.x), lo[1] = std::min(lo[1], box.min.y), lo[2] = std::min(lo[2], box.min.z);
    hi[0] = std::max(hi[0], box.max.x), hi[1] = std::max(hi[1], box.max.y), hi[2] = std::max(hi[2], box.max.z);
  }
  for (int axis = 0; axis < 3; ++axis) hole.size[axis] = hi[axis] - lo[axis];
  tidyPlacement(hole);
  doc.nodes.push_back(hole);
  // The part it goes into: the solid next to the fastener whose box overlaps the hole's most.
  QVector3D hlo, hhi;
  const Node* target = nullptr;
  double best = 0;
  if (alignmentWorldBounds(doc, hole, &hlo, &hhi)) {
    for (const Node& other : doc.nodes) {
      if (other.id == hole.id || other.id == source.id || other.parentId != source.parentId || other.hole) continue;
      if (other.kind != Node::Kind::Shape && other.kind != Node::Kind::Group) continue;
      QVector3D olo, ohi;
      if (!alignmentWorldBounds(doc, other, &olo, &ohi)) continue;
      double overlap = 1;
      for (int axis = 0; axis < 3; ++axis) overlap *= std::max(0.0f, std::min(hhi[axis], ohi[axis]) - std::max(hlo[axis], olo[axis]));
      if (overlap > best) {
        best = overlap;
        target = &other;
      }
    }
  }
  QString message;
  QString select = hole.id;
  if (target && target->kind == Node::Kind::Group) {
    // Into that group, keeping its place in the world.
    const QString targetId = target->id;
    Node* added = doc.find(hole.id);
    const QMatrix4x4 world = alignmentWorldMatrix(doc, *added);
    added->parentId = targetId;
    const QMatrix4x4 local = alignmentParentWorldMatrix(doc, *added).inverted() * world;
    const double keep[3] = {added->rot[0], added->rot[1], added->rot[2]};
    decomposePlacementNear(local, keep, added->pos, added->rot, added->scale);
    tidyPlacement(*added);
    message = QStringLiteral("Added the matching hole to \"%1\"; it cuts only that group.").arg(target->name);
  } else if (target) {
    const QString targetName = target->name;
    QString error;
    if (doc.groupSelected(QStringList{target->id, hole.id}, &error)) {
      doc.nodes.back().name = targetName;
      message = QStringLiteral("Added the matching hole and grouped it with \"%1\", so it cuts only that part.").arg(targetName);
    } else {
      message = error;
    }
  } else {
    // Nothing to cut: beside the fastener, so it does not cut the fastener itself.
    Node* added = doc.find(hole.id);
    QVector3D slo, shi;
    if (added && alignmentWorldBounds(doc, source, &slo, &shi)) {
      const double step = (shi.x() - slo.x()) + (hhi.x() - hlo.x()) * 0.5 + 5;
      const double centre = (slo.x() + shi.x()) * 0.5;
      added->pos[0] += centre + step - (hlo.x() + hhi.x()) * 0.5;
      tidyPlacement(*added);
    }
    message = QStringLiteral("Added the matching hole beside the part: nothing is under the fastener to cut. Move it into "
                             "place and group it with the part it should cut.");
  }
  selection = QStringList{select};
  genFormFor.clear();
  refreshTree();
  loadInspector();
  refreshScene();
  placeOverlays();
  status(message);
}

void MainWindow::convertGeneratorToParts() {
  Node* node = selectedGenerator();
  if (!node) return;
  if (genTimer && genTimer->isActive()) {
    genTimer->stop();
    applyGeneratorChange();
    node = selectedGenerator();
    if (!node) return;
  }
  const Node source = *node;
  QString warning;
  std::vector<gen::Piece> pieces;
  hookGeneratorText();
  runBusy([&] { pieces = gen::build(source.genType, source.genParams, &warning, printerClearance()); }, true);
  if (pieces.empty()) {
    status(warning.isEmpty() ? QStringLiteral("Nothing to convert.") : warning);
    return;
  }
  const QString folder = defaultAppdataPath(QStringLiteral("Generated"));
  QDir().mkpath(folder);
  const QString stem = sanitizeProjectName(source.name).replace(QLatin1Char(' '), QLatin1Char('_'));
  const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
  pushUndo();
  const double zero[3] = {0, 0, 0};
  const QMatrix4x4 placed = placementMatrix(source.pos, source.rot, zero, source.scale);
  QStringList made;
  for (size_t i = 0; i < pieces.size(); ++i) {
    manifold::Manifold solid = pieces[i].solid;
    if (source.mirror[0]) solid = solid.Mirror(manifold::vec3(1, 0, 0));
    if (source.mirror[1]) solid = solid.Mirror(manifold::vec3(0, 1, 0));
    if (source.mirror[2]) solid = solid.Mirror(manifold::vec3(0, 0, 1));
    const auto box = solid.BoundingBox();
    const QVector3D center(static_cast<float>((box.min.x + box.max.x) * 0.5), static_cast<float>((box.min.y + box.max.y) * 0.5),
                           static_cast<float>((box.min.z + box.max.z) * 0.5));
    QString pieceName = pieces[i].name;
    const QString path = QDir(folder).absoluteFilePath(
        QStringLiteral("%1-%2-%3.stl").arg(stem, QString(pieceName).replace(QLatin1Char(' '), QLatin1Char('_')), stamp));
    QString error;
    if (!gen::saveStl(solid, path, &error)) {
      status(QStringLiteral("Could not write %1: %2").arg(QDir::toNativeSeparators(path), error));
      undo();
      return;
    }
    Node part;
    part.id = newId();
    part.kind = Node::Kind::Shape;
    part.shape = QStringLiteral("stl");
    part.meshPath = path;
    part.parentId = source.parentId;
    part.name = pieces.size() == 1 ? source.name : pieceName;
    part.hole = source.hole;
    part.color = source.color;
    const QVector3D at = placed.map(center);
    part.pos[0] = at.x();
    part.pos[1] = at.y();
    part.pos[2] = at.z();
    for (int axis = 0; axis < 3; ++axis) {
      part.rot[axis] = source.rot[axis];
      part.scale[axis] = source.scale[axis];
    }
    part.twist = source.twist;
    part.twistAxis = source.twistAxis;
    part.bend = source.bend;
    part.along = source.along;
    part.toward = source.toward;
    tidyPlacement(part);
    doc.nodes.push_back(part);
    made << part.id;
  }
  doc.remove(source.id);
  QString groupError;
  if (made.size() > 1 && doc.groupSelected(made, &groupError)) {
    Node& group = doc.nodes.back();
    group.name = source.name;
    selection = QStringList{group.id};
  } else {
    selection = made;
  }
  genFormFor.clear();
  refreshTree();
  loadInspector();
  refreshScene();
  QString message = made.size() > 1 ? QStringLiteral("Converted to %1 parts in a group. Ungroup to move them apart.").arg(made.size())
                                    : QStringLiteral("Converted to a plain part.");
  if (std::abs(source.twist) > 0.05 || std::abs(source.bend) > 0.05)
    message += QStringLiteral(" Twist and bend now apply to each part on its own.");
  status(message);
}

// ---------------------------------------------------------------------------------------------
// Hole clearance

std::vector<Node*> MainWindow::selectedHoles() {
  std::vector<Node*> holes;
  for (const QString& id : selection) {
    Node* node = doc.find(id);
    if (!node || node->kind == Node::Kind::Pattern || !node->hole) return {};
    holes.push_back(node);
  }
  return holes;
}

void MainWindow::buildHolePage() {
  holePage = new QWidget(this);
  auto* form = new QFormLayout(holePage);
  form->setContentsMargins(0, 4, 0, 0);
  auto* title = new QLabel(QStringLiteral("Hole clearance"), holePage);
  title->setStyleSheet(QStringLiteral("font-weight: 600; color: #e87400;"));
  form->addRow(title);
  holeClearanceBox = new QComboBox(holePage);
  holeClearanceBox->addItem(QStringLiteral("None"));
  holeClearanceBox->addItem(QStringLiteral("Printer clearance"));
  holeClearanceBox->addItem(QStringLiteral("Own clearance"));
  holeClearanceBox->setToolTip(QStringLiteral("Cut the hole wider than it is drawn, so a part made to the same size "
                                              "fits into it. Printer clearance is set under Settings > Printer."));
  form->addRow(QStringLiteral("Clearance"), holeClearanceBox);
  holeClearanceSpin = new QDoubleSpinBox(holePage);
  holeClearanceSpin->setRange(0, 2);
  holeClearanceSpin->setDecimals(2);
  holeClearanceSpin->setSingleStep(0.01);
  holeClearanceSpin->setSuffix(QStringLiteral(" mm"));
  holeClearanceSpin->installEventFilter(this);
  form->addRow(QStringLiteral("Own clearance"), holeClearanceSpin);
  holeSidesBox = new QCheckBox(QStringLiteral("Sides only"), holePage);
  holeSidesBox->setToolTip(QStringLiteral("Grow the hole around its own height axis only, so its depth stays as "
                                          "drawn (a pocket for a magnet). A tilted hole grows around its own axis."));
  form->addRow(QString(), holeSidesBox);
  holeClearanceNote = new QLabel(holePage);
  holeClearanceNote->setWordWrap(true);
  holeClearanceNote->setStyleSheet(QStringLiteral("color: #8a8074;"));
  form->addRow(holeClearanceNote);
  holeTimer = new QTimer(this);
  holeTimer->setSingleShot(true);
  holeTimer->setInterval(200);
  connect(holeTimer, &QTimer::timeout, this, [this] {
    if (rebuilding) {
      holeTimer->start();
      return;
    }
    refreshScene();
  });
  connect(holeClearanceBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    if (loading) return;
    const auto holes = selectedHoles();
    if (holes.empty()) return;
    pushUndo();
    for (Node* node : selectedHoles()) node->clearanceMode = std::clamp(index, 0, 2);
    loadHolePage();
    placeOverlays();
    refreshScene();
  });
  connect(holeClearanceSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
    if (loading) return;
    for (Node* node : selectedHoles()) node->holeClearance = value;
    holeTimer->start();
  });
  connect(holeSidesBox, &QCheckBox::toggled, this, [this](bool on) {
    if (loading) return;
    if (selectedHoles().empty()) return;
    pushUndo();
    for (Node* node : selectedHoles()) node->clearanceSidesOnly = on;
    refreshScene();
  });
}

// --- Hollow -------------------------------------------------------------------------------------

void MainWindow::buildHollowPage() {
  hollowPage = new QWidget(this);
  auto* form = new QFormLayout(hollowPage);
  form->setContentsMargins(0, 4, 0, 0);
  hollowWallSpin = new QDoubleSpinBox(hollowPage);
  hollowWallSpin->setRange(0.2, 100);
  hollowWallSpin->setDecimals(2);
  hollowWallSpin->setSingleStep(0.4);
  hollowWallSpin->setSuffix(QStringLiteral(" mm"));
  hollowWallSpin->setValue(2);
  hollowWallSpin->setToolTip(QStringLiteral("Wall thickness all round, at the part's real size. A few nozzle widths (1.2 to 2 mm) prints well."));
  form->addRow(QStringLiteral("Wall"), hollowWallSpin);
  auto* open = new QLabel(QStringLiteral("Open sides (the part's own directions):"), hollowPage);
  open->setWordWrap(true);
  form->addRow(open);
  auto* grid = new QGridLayout;
  const char* names[6] = {"Left (-X)", "Right (+X)", "Front (-Y)", "Back (+Y)", "Bottom (-Z)", "Top (+Z)"};
  for (int i = 0; i < 6; ++i) {
    hollowOpenBox[i] = new QCheckBox(QString::fromLatin1(names[i]), hollowPage);
    grid->addWidget(hollowOpenBox[i], i / 2, i % 2);
  }
  form->addRow(grid);
  auto* buttons = new QHBoxLayout;
  auto* apply = new QPushButton(QStringLiteral("Hollow"), hollowPage);
  auto* remove = new QPushButton(QStringLiteral("Make solid"), hollowPage);
  apply->setToolTip(QStringLiteral("Hollow the selected part with this wall and these open sides."));
  remove->setToolTip(QStringLiteral("Fill it in again."));
  buttons->addWidget(apply, 1);
  buttons->addWidget(remove);
  form->addRow(buttons);
  hollowNote = new QLabel(hollowPage);
  hollowNote->setWordWrap(true);
  hollowNote->setStyleSheet(QStringLiteral("color: #8a8074;"));
  form->addRow(hollowNote);
  auto target = [this]() -> Node* {
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    if (!node || node->kind == Node::Kind::Pattern || node->hole) return nullptr;
    return node;
  };
  connect(apply, &QPushButton::clicked, this, [this, target] {
    Node* node = target();
    if (!node) {
      status(QStringLiteral("Select one solid part or group to hollow."));
      return;
    }
    pushUndo();
    node->shellWall = hollowWallSpin->value();
    node->shellOpen = 0;
    for (int i = 0; i < 6; ++i)
      if (hollowOpenBox[i]->isChecked()) node->shellOpen |= 1 << i;
    refreshScene(false);
    loadHollowPage();
    status(QStringLiteral("Hollowed with %1 mm walls.").arg(node->shellWall, 0, 'f', 2));
  });
  connect(remove, &QPushButton::clicked, this, [this, target] {
    Node* node = target();
    if (!node || node->shellWall <= 0) {
      status(QStringLiteral("That part is not hollow."));
      return;
    }
    pushUndo();
    node->shellWall = 0;
    refreshScene(false);
    loadHollowPage();
    status(QStringLiteral("Solid again."));
  });
}

void MainWindow::loadHollowPage() {
  if (!hollowPage) return;
  const Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
  const bool usable = node && node->kind != Node::Kind::Pattern && !node->hole;
  if (usable && node->shellWall > 0) {
    hollowWallSpin->setValue(node->shellWall);
    for (int i = 0; i < 6; ++i) hollowOpenBox[i]->setChecked(node->shellOpen & (1 << i));
  }
  hollowNote->setText(!usable              ? QStringLiteral("Select one solid part or group.")
                      : node->shellWall > 0 ? QStringLiteral("Hollow now: %1 mm walls. Change the settings and press Hollow again.")
                                                  .arg(node->shellWall, 0, 'f', 2)
                                            : QStringLiteral("Solid now. Hollowing a detailed part (text, threads, an STL) takes a "
                                                             "while; it is worked out once and kept."));
}

// --- Split --------------------------------------------------------------------------------------

void MainWindow::buildSplitPage() {
  splitPage = new QWidget(this);
  auto* form = new QFormLayout(splitPage);
  form->setContentsMargins(0, 4, 0, 0);
  splitAxisBox = new QComboBox(splitPage);
  splitAxisBox->addItem(QStringLiteral("Across X (an upright cut)"));
  splitAxisBox->addItem(QStringLiteral("Across Y (an upright cut)"));
  splitAxisBox->addItem(QStringLiteral("Across Z (a level cut)"));
  splitAxisBox->addItem(QStringLiteral("On the workplane"));
  splitAxisBox->setCurrentIndex(2);
  splitAxisBox->setToolTip(QStringLiteral("The plane to cut along. \"On the workplane\" cuts along the face you set the "
                                          "workplane on with the Workplane tool, slanted ones too."));
  form->addRow(QStringLiteral("Plane"), splitAxisBox);
  splitAtSpin = new QDoubleSpinBox(splitPage);
  splitAtSpin->setRange(-100000, 100000);
  splitAtSpin->setDecimals(2);
  splitAtSpin->setSuffix(QStringLiteral(" mm"));
  splitAtSpin->setToolTip(QStringLiteral("Where the plane cuts, on the bed's axis. It starts in the middle of the part."));
  form->addRow(QStringLiteral("At"), splitAtSpin);
  splitGapSpin = new QDoubleSpinBox(splitPage);
  splitGapSpin->setRange(0, 1000);
  splitGapSpin->setDecimals(1);
  splitGapSpin->setSuffix(QStringLiteral(" mm"));
  splitGapSpin->setValue(10);
  splitGapSpin->setToolTip(QStringLiteral("Move the halves this far apart, so you can see the cut. 0 leaves them in place."));
  form->addRow(QStringLiteral("Move apart"), splitGapSpin);
  splitJoinBox = new QComboBox(splitPage);
  splitJoinBox->addItem(QStringLiteral("None"));
  splitJoinBox->addItem(QStringLiteral("Pins (printed apart)"));
  splitJoinBox->addItem(QStringLiteral("Pegs on one half"));
  splitJoinBox->addItem(QStringLiteral("Dovetail"));
  splitJoinBox->setToolTip(QStringLiteral("Something on the cut faces so the halves go back together lined up. Pins: holes in "
                                          "both halves and loose pins to glue in. Pegs: pegs on one half, holes in the other. "
                                          "Dovetail: a key on one half that slides into a slot in the other. They fit with "
                                          "the printer clearance from Settings."));
  form->addRow(QStringLiteral("Connectors"), splitJoinBox);
  splitJoinSize = new QDoubleSpinBox(splitPage);
  splitJoinSize->setRange(0, 60);
  splitJoinSize->setDecimals(1);
  splitJoinSize->setSingleStep(0.5);
  splitJoinSize->setSuffix(QStringLiteral(" mm"));
  splitJoinSize->setSpecialValueText(QStringLiteral("Automatic"));
  splitJoinSize->setToolTip(QStringLiteral("The pins' or pegs' diameter, or the dovetail's width. Automatic sizes them to the cut."));
  form->addRow(QStringLiteral("Size"), splitJoinSize);
  splitJoinCount = new QSpinBox(splitPage);
  splitJoinCount->setRange(1, 8);
  splitJoinCount->setValue(2);
  splitJoinCount->setToolTip(QStringLiteral("How many pins or pegs, spread over the cut face."));
  form->addRow(QStringLiteral("How many"), splitJoinCount);
  auto syncJoin = [this] {
    const int kind = splitJoinBox->currentIndex();
    splitJoinSize->setEnabled(kind != 0);
    splitJoinCount->setEnabled(kind == 1 || kind == 2);
  };
  connect(splitJoinBox, qOverload<int>(&QComboBox::currentIndexChanged), this, syncJoin);
  syncJoin();
  auto* apply = new QPushButton(QStringLiteral("Split"), splitPage);
  form->addRow(apply);
  splitNote = new QLabel(splitPage);
  splitNote->setWordWrap(true);
  splitNote->setStyleSheet(QStringLiteral("color: #8a8074;"));
  form->addRow(splitNote);
  connect(splitAxisBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
    if (!loading) loadSplitPage(true);
  });
  connect(apply, &QPushButton::clicked, this, [this] { splitSelected(); });
}

void MainWindow::loadSplitPage(bool recentre) {
  if (!splitPage) return;
  const Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
  const int axis = splitAxisBox->currentIndex();
  auto* model = qobject_cast<QStandardItemModel*>(splitAxisBox->model());
  if (model && model->item(3)) model->item(3)->setEnabled(doc.faceWorkplane);
  if (axis == 3 && !doc.faceWorkplane) splitAxisBox->setCurrentIndex(2);
  QVector3D lo, hi;
  const bool usable = node && node->kind != Node::Kind::Pattern && alignmentWorldBounds(doc, *node, &lo, &hi);
  splitAtSpin->setEnabled(usable && splitAxisBox->currentIndex() < 3);
  if (!usable) {
    splitNote->setText(QStringLiteral("Select one part or group."));
    return;
  }
  if (splitAxisBox->currentIndex() < 3) {
    const int a = splitAxisBox->currentIndex();
    if (recentre || node->id != splitFor || splitAtSpin->value() < lo[a] - 1e-6 || splitAtSpin->value() > hi[a] + 1e-6) {
      const bool was = loading;
      loading = true;
      splitAtSpin->setValue((lo[a] + hi[a]) * 0.5);
      loading = was;
    }
    splitNote->setText(QStringLiteral("The part runs from %1 to %2 mm on %3.")
                           .arg(lo[a], 0, 'f', 2)
                           .arg(hi[a], 0, 'f', 2)
                           .arg(QChar("XYZ"[a])));
  } else {
    splitNote->setText(QStringLiteral("Cuts along the workplane's face."));
  }
  splitFor = node->id;
}

void MainWindow::splitSelected() {
  Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
  QVector3D lo, hi;
  if (!node || node->kind == Node::Kind::Pattern || !alignmentWorldBounds(doc, *node, &lo, &hi)) {
    status(QStringLiteral("Select one part or group to split."));
    return;
  }
  // The plane, in world coordinates.
  QVector3D point, normal;
  const int axis = splitAxisBox->currentIndex();
  if (axis < 3) {
    normal[axis] = 1;
    point = (lo + hi) * 0.5f;
    point[axis] = static_cast<float>(splitAtSpin->value());
  } else {
    if (!doc.faceWorkplane) {
      status(QStringLiteral("Set a workplane on a face first (Workplane tool)."));
      return;
    }
    point = QVector3D(doc.workplaneOrigin[0], doc.workplaneOrigin[1], doc.workplaneOrigin[2]);
    normal = QVector3D(doc.workplaneNormal[0], doc.workplaneNormal[1], doc.workplaneNormal[2]).normalized();
  }
  // It has to go through the part: corners of its box on both sides.
  int above = 0, below = 0;
  for (int i = 0; i < 8; ++i) {
    const QVector3D corner((i & 1) ? hi.x() : lo.x(), (i & 2) ? hi.y() : lo.y(), (i & 4) ? hi.z() : lo.z());
    const float side = QVector3D::dotProduct(corner - point, normal);
    if (side > 1e-4f) ++above;
    if (side < -1e-4f) ++below;
  }
  if (!above || !below) {
    status(QStringLiteral("The plane misses the part. Move it into the part's range."));
    return;
  }
  const QString id = node->id;
  const QString parentId = node->parentId;
  const QString name = node->name;
  const QColor colour = node->color;
  const float size = 2.f * (hi - lo).length() + 20.f;
  // The cutter's frame: its Z along the normal.
  QVector3D xAxis = QVector3D::crossProduct(std::abs(normal.z()) < 0.9f ? QVector3D(0, 0, 1) : QVector3D(1, 0, 0), normal).normalized();
  const QVector3D yAxis = QVector3D::crossProduct(normal, xAxis);
  // Connectors: where they go on the cut face (worked out before anything changes).
  const int joinKind = splitJoinBox ? splitJoinBox->currentIndex() : 0;
  struct Pin {
    QVector3D at;  // on the cut plane, world
    double radius, depth;
  };
  std::vector<Pin> pins;
  sculpt::Mesh tongue, slot;  // dovetail, world
  if (joinKind != 0) {
    sculpt::Mesh mesh;
    QString why;
    if (!partMesh(id, &mesh)) why = QStringLiteral("the part has no surface");
    const manifold::Manifold solid = why.isEmpty() ? fx::toManifold(mesh) : manifold::Manifold();
    if (why.isEmpty() && solid.IsEmpty()) why = QStringLiteral("the part is not one closed solid");
    // Into the plane's frame: x, y along the cut, z along the normal, 0 on the plane.
    using manifold::vec3;
    const vec3 X(xAxis.x(), xAxis.y(), xAxis.z()), Y(yAxis.x(), yAxis.y(), yAxis.z()), N(normal.x(), normal.y(), normal.z());
    const vec3 P(point.x(), point.y(), point.z());
    const manifold::mat3x4 toPlane({X.x, Y.x, N.x}, {X.y, Y.y, N.y}, {X.z, Y.z, N.z},
                                   {-manifold::la::dot(X, P), -manifold::la::dot(Y, P), -manifold::la::dot(N, P)});
    const manifold::mat3x4 fromPlane(X, Y, N, P);
    const manifold::Manifold local = why.isEmpty() ? solid.Transform(toPlane) : manifold::Manifold();
    const manifold::Polygons section = why.isEmpty() ? local.Slice(0) : manifold::Polygons();
    if (why.isEmpty() && section.empty()) why = QStringLiteral("the cut face is empty");
    // Distance from a point to the cut face's outline, and whether it is inside.
    auto inside = [&](double x, double y) {
      int winding = 0;
      for (const auto& ring : section)
        for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
          const auto& a = ring[j];
          const auto& b = ring[i];
          if ((a.y <= y) != (b.y <= y)) {
            const double cross = (b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y);
            winding += (b.y > a.y) == (cross > 0) ? (b.y > a.y ? 1 : -1) : 0;
          }
        }
      return winding != 0;
    };
    auto edgeDistance = [&](double x, double y) {
      double best = 1e300;
      for (const auto& ring : section)
        for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
          const double ax = ring[j].x, ay = ring[j].y, bx = ring[i].x, by = ring[i].y;
          const double dx = bx - ax, dy = by - ay, len = dx * dx + dy * dy;
          const double t = len > 0 ? std::clamp(((x - ax) * dx + (y - ay) * dy) / len, 0.0, 1.0) : 0.0;
          best = std::min(best, std::hypot(x - ax - t * dx, y - ay - t * dy));
        }
      return best;
    };
    struct Spot {
      double x, y, room;
    };
    std::vector<Spot> spots;
    double sx0 = 1e300, sx1 = -1e300, sy0 = 1e300, sy1 = -1e300;
    if (why.isEmpty()) {
      for (const auto& ring : section)
        for (const auto& p : ring) {
          sx0 = std::min(sx0, p.x), sx1 = std::max(sx1, p.x);
          sy0 = std::min(sy0, p.y), sy1 = std::max(sy1, p.y);
        }
      const double step = std::max(0.25, std::max(sx1 - sx0, sy1 - sy0) / 70);
      for (double y = sy0 + step * 0.5; y < sy1; y += step)
        for (double x = sx0 + step * 0.5; x < sx1; x += step)
          if (inside(x, y)) spots.push_back({x, y, edgeDistance(x, y)});
      if (spots.empty()) why = QStringLiteral("the cut face is too small");
    }
    // How deep the part goes on each side of the cut at a spot, through a column `radius` wide.
    auto depthAt = [&](double x, double y, double radius, double* below, double* above) {
      const manifold::Manifold column =
          local ^ manifold::Manifold::Cylinder(4000, radius, radius, 24, true).Translate(vec3(x, y, 0));
      const auto box = column.BoundingBox();
      *below = column.IsEmpty() ? 0 : -box.min.z;
      *above = column.IsEmpty() ? 0 : box.max.z;
    };
    if (why.isEmpty() && (joinKind == 1 || joinKind == 2)) {
      double deepest = 0;
      for (const Spot& s : spots) deepest = std::max(deepest, s.room);
      const int count = splitJoinCount->value();
      // Automatic: about a third of the room there is, rounded to half millimetres, 3 to 8 mm across.
      double radius = splitJoinSize->value() > 0
                          ? splitJoinSize->value() * 0.5
                          : std::clamp(std::round(deepest * 0.36 * (count > 2 ? 0.85 : 1.0) * 2) / 2, 3.0, 8.0) * 0.5;
      const double wall = std::max(1.2, radius * 0.5);
      std::vector<Spot> room;
      for (const Spot& s : spots)
        if (s.room >= radius + wall) room.push_back(s);
      if (room.empty()) {
        why = QStringLiteral("the cut face has no room for %1 mm pins").arg(radius * 2, 0, 'f', 1);
      } else {
        // Spread out: the first one farthest from the middle, then each farthest from the others.
        double mx = 0, my = 0;
        for (const Spot& s : room) mx += s.x, my += s.y;
        mx /= room.size(), my /= room.size();
        std::vector<Spot> chosen;
        if (count == 1) {
          // One: the roomiest spot.
          chosen.push_back(*std::max_element(room.begin(), room.end(), [](const Spot& a, const Spot& b) { return a.room < b.room; }));
        } else {
          chosen.push_back(*std::max_element(room.begin(), room.end(), [&](const Spot& a, const Spot& b) {
            return std::hypot(a.x - mx, a.y - my) < std::hypot(b.x - mx, b.y - my);
          }));
          while (static_cast<int>(chosen.size()) < count) {
            const Spot* best = nullptr;
            double bestGap = 0;
            for (const Spot& s : room) {
              double gap = 1e300;
              for (const Spot& c : chosen) gap = std::min(gap, std::hypot(s.x - c.x, s.y - c.y));
              if (gap > bestGap) bestGap = gap, best = &s;
            }
            if (!best || bestGap < 2 * radius + wall) break;  // no room for more
            chosen.push_back(*best);
          }
        }
        for (const Spot& c : chosen) {
          double below = 0, above = 0;
          depthAt(c.x, c.y, radius, &below, &above);
          const double depth = std::min({below, above, radius * 2 * 1.5 + 2}) - 1.0;
          if (depth < radius) continue;
          const vec3 w = P + X * c.x + Y * c.y;
          pins.push_back({QVector3D(float(w.x), float(w.y), float(w.z)), radius, depth});
        }
        if (pins.empty()) why = QStringLiteral("the part is too thin by the cut for pins");
      }
    }
    if (why.isEmpty() && joinKind == 3) {
      // Along the cut face's longer side, through its roomiest spot, open at both ends.
      const Spot& best = *std::max_element(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) { return a.room < b.room; });
      const bool alongX = (sx1 - sx0) >= (sy1 - sy0);
      double width = splitJoinSize->value() > 0 ? splitJoinSize->value() : std::clamp(std::round(best.room * 0.55), 4.0, 20.0);
      double below = 0, above = 0;
      depthAt(best.x, best.y, width * 0.5, &below, &above);
      const double depth = std::min({below - 1.0, above - 1.0, width * 0.5 + 1});
      if (depth < 1.5) {
        why = QStringLiteral("the part is too thin by the cut for a dovetail");
      } else {
        const double flare = std::tan(14.0 * 3.14159265358979 / 180.0);
        auto keyShape = [&](double extraDepth) {
          // Across the slide (t) and up from the cut (n): narrow at the cut face, wider at the far end,
          // so it locks; from a hair below the cut (so it joins its half) to its depth.
          auto half = [&](double n) { return width * 0.5 + n * flare; };
          const double top = depth + extraDepth;
          const manifold::SimplePolygon outline{{-half(-0.5), -0.5}, {half(-0.5), -0.5}, {half(top), top}, {-half(top), top}};
          const double length = std::max(sx1 - sx0, sy1 - sy0) + 40;
          // Drawn in (t, n) and pushed along z, then turned so z runs along the slide and n along the normal.
          manifold::Manifold bar = manifold::Manifold::Extrude(manifold::Polygons{outline}, length).Translate(vec3(0, 0, -length * 0.5));
          bar = alongX ? bar.Transform(manifold::mat3x4(vec3(0, 1, 0), vec3(0, 0, 1), vec3(1, 0, 0), vec3(0, best.y, 0)))
                       : bar.Transform(manifold::mat3x4(vec3(1, 0, 0), vec3(0, 0, 1), vec3(0, -1, 0), vec3(best.x, 0, 0)));
          return bar;
        };
        // The key: on half 1, reaching into half 2's side, kept to where the part is.
        const manifold::Manifold key = keyShape(0) ^ local;
        // The slot: a hole in half 2, a little deeper (its clearance comes from the hole's own setting).
        const manifold::Manifold groove = keyShape(0.3);
        if (key.IsEmpty()) why = QStringLiteral("the dovetail missed the part");
        else {
          tongue = sculpt::fromManifold(key.Transform(fromPlane));
          slot = sculpt::fromManifold(groove.Transform(fromPlane));
        }
      }
    }
    if (!why.isEmpty()) {
      status(QStringLiteral("No connectors: %1. Choose None, or a smaller size.").arg(why));
      return;
    }
  }
  const QMatrix4x4 parentWorld = parentId.isEmpty() ? QMatrix4x4() : alignmentWorldMatrix(doc, *doc.find(parentId));
  const QMatrix4x4 toParent = parentWorld.inverted();
  pushUndo();
  const size_t before = doc.nodes.size();
  doc.duplicateInPlace(id);
  if (doc.nodes.size() <= before) {
    undoStack.pop_back();
    return;
  }
  const QString copyId = doc.nodes[before].id;
  QStringList halves;
  const QVector3D shift = normal * static_cast<float>(splitGapSpin->value() * 0.5);
  for (int half = 0; half < 2; ++half) {
    // Half 1 keeps the side below the plane: its cutter takes everything above. Half 2 the other way.
    const float sign = half == 0 ? 1.f : -1.f;
    const QString cutterId = doc.addShape(QStringLiteral("box"), 0, 0);
    Node* cutter = doc.find(cutterId);
    cutter->hole = true;
    cutter->name = QStringLiteral("Split cut");
    cutter->parentId = parentId;
    for (double& s : cutter->size) s = size;
    if (axis < 3 && parentId.isEmpty()) {
      // A level or upright cut on the bed: placed in doubles, so the cut is exactly where asked.
      const double at = splitAtSpin->value();
      for (int k = 0; k < 3; ++k) cutter->pos[k] = (static_cast<double>(lo[k]) + static_cast<double>(hi[k])) * 0.5;
      cutter->pos[axis] = at + sign * std::ceil(size) * 0.5;
      for (double& s : cutter->size) s = std::ceil(size);
    } else {
      QMatrix4x4 world;
      world.setColumn(0, QVector4D(xAxis, 0));
      world.setColumn(1, QVector4D(yAxis, 0));
      world.setColumn(2, QVector4D(normal, 0));
      world.setColumn(3, QVector4D(point + normal * (sign * size * 0.5f), 1));
      const double zero[3] = {0, 0, 0};
      decomposePlacementNear(toParent * world, zero, cutter->pos, cutter->rot, cutter->scale);
      for (double& s : cutter->scale) s = 1;
    }
    tidyPlacement(*cutter);
    // Connectors: holes cut with the half, and pegs or the dovetail key joined to half 1 outside
    // its cutter (in a group around it), so the cutter does not take them off again.
    QStringList members{half == 0 ? id : copyId, cutterId};
    QStringList joined;
    auto cylinder = [&](const QVector3D& centre, double radius, double length, bool hole, const QString& what) {
      const QString cid = doc.addShape(QStringLiteral("cylinder"), 0, 0);
      Node* c = doc.find(cid);
      c->name = what;
      c->parentId = parentId;
      c->radius = radius;
      c->size[2] = length;
      c->sides = 48;
      c->hole = hole;
      c->color = colour;
      if (hole) c->clearanceMode = 1;  // the printer clearance
      QMatrix4x4 world;
      world.setColumn(0, QVector4D(xAxis, 0));
      world.setColumn(1, QVector4D(yAxis, 0));
      world.setColumn(2, QVector4D(normal, 0));
      world.setColumn(3, QVector4D(centre, 1));
      const double zero[3] = {0, 0, 0};
      decomposePlacementNear(toParent * world, zero, c->pos, c->rot, c->scale);
      for (double& s : c->scale) s = 1;
      tidyPlacement(*c);
      return cid;
    };
    for (const Pin& pin : pins) {
      if (joinKind == 1 || half == 1)
        members << cylinder(pin.at, pin.radius, 2 * (pin.depth + 0.3), true, QStringLiteral("Connector hole"));
      else
        joined << cylinder(pin.at + normal * float((pin.depth - 0.8) * 0.5), pin.radius, pin.depth + 0.2, false,
                           QStringLiteral("Connector peg"));
    }
    if (!tongue.tri.empty()) {
      if (half == 1) members << addMeshNode(slot, QStringLiteral("Dovetail slot"), parentId, colour, true);
      else joined << addMeshNode(tongue, QStringLiteral("Dovetail key"), parentId, colour, false);
    }
    if (half == 1 && !slot.tri.empty())
      if (Node* hole = doc.find(members.back())) hole->clearanceMode = 1;
    QString error;
    if (!doc.groupSelected(members, &error)) {
      undo();
      status(error);
      return;
    }
    if (!joined.isEmpty()) {
      joined.prepend(doc.nodes.back().id);
      if (!doc.groupSelected(joined, &error)) {
        undo();
        status(error);
        return;
      }
    }
    Node& group = doc.nodes.back();
    group.name = QStringLiteral("%1 (%2)").arg(name, half == 0 ? QStringLiteral("part 1") : QStringLiteral("part 2"));
    // Apart along the normal, in the parent's frame.
    const QVector3D move = toParent.mapVector(shift * -sign);
    group.pos[0] += move.x();
    group.pos[1] += move.y();
    group.pos[2] += move.z();
    tidyPlacement(group);
    halves << group.id;
  }
  // Loose pins, standing on the plate beside the part, a little shorter than the two holes.
  if (joinKind == 1) {
    double x = hi.x() + 8;
    for (const Pin& pin : pins) {
      const QString pid = doc.addShape(QStringLiteral("cylinder"), 0, 0);
      Node* p = doc.find(pid);
      p->name = QStringLiteral("Connector pin");
      p->radius = pin.radius;
      p->size[2] = 2 * pin.depth - 0.6;
      p->sides = 48;
      p->edgeFillet = std::min(0.4, pin.radius * 0.3);  // eased ends slide in
      p->color = colour;
      p->pos[0] = x + pin.radius;
      p->pos[1] = lo.y() + pin.radius;
      p->pos[2] = doc.workplaneZ + p->size[2] * 0.5;
      x += 2 * pin.radius + 4;
    }
  }
  selection = halves;
  showPanel(8);  // close the panel
  refreshTree();
  loadInspector();
  refreshScene();
  const QString joins = joinKind == 1   ? QStringLiteral(" With %1 pin holes in each half and %1 pins beside it to glue in.").arg(pins.size())
                        : joinKind == 2 ? QStringLiteral(" With %1 pegs on part 1 and holes for them in part 2.").arg(pins.size())
                        : joinKind == 3 ? QStringLiteral(" With a dovetail: part 1's key slides into part 2's slot.")
                                        : QString();
  status(QStringLiteral("Split in two. Each half is a group with a cutter: ungroup it to change the cut, or Convert/export as it is.") + joins);
}

void MainWindow::buildHullPage() {
  hullPage = new QWidget(this);
  auto* form = new QFormLayout(hullPage);
  form->setContentsMargins(0, 4, 0, 0);
  auto* title = new QLabel(QStringLiteral("Group"), hullPage);
  title->setStyleSheet(QStringLiteral("font-weight: 600; color: #e87400;"));
  form->addRow(title);
  hullModeBox = new QComboBox(hullPage);
  hullModeBox->addItem(QStringLiteral("Join (plain group)"));
  hullModeBox->addItem(QStringLiteral("Hull (wrap all)"));
  hullModeBox->addItem(QStringLiteral("Chain hull"));
  hullModeBox->addItem(QStringLiteral("Intersect (overlap)"));
  hullModeBox->setToolTip(QStringLiteral("How the parts make one solid. Join puts them together. A hull shrink-wraps them into "
                                         "one solid without dents; a chain wraps every part with the next one, for bars and "
                                         "paths. Intersect keeps only where all the parts overlap."));
  form->addRow(QStringLiteral("Combine"), hullModeBox);
  blendBox = new QDoubleSpinBox(hullPage);
  blendBox->setRange(0, 50);
  blendBox->setDecimals(1);
  blendBox->setSingleStep(0.5);
  blendBox->setSuffix(QStringLiteral(" mm"));
  blendBox->setSpecialValueText(QStringLiteral("Sharp"));
  blendBox->setKeyboardTracking(false);
  blendBox->setToolTip(QStringLiteral("Smooth join: the parts flow into each other where they meet, like clay, with a soft "
                                      "fillet this wide. Holes in the group cut with the same soft edge. Sharp is a plain join."));
  form->addRow(QStringLiteral("Smooth join"), blendBox);
  multiColorBox = new QCheckBox(QStringLiteral("Keep part colours"), hullPage);
  multiColorBox->setToolTip(QStringLiteral("Show the group in its parts' colours, and export it (3MF, OBJ) as one piece per colour, "
                                           "so the slicer can give each colour its own filament."));
  form->addRow(multiColorBox);
  hullNote = new QLabel(hullPage);
  hullNote->setWordWrap(true);
  hullNote->setStyleSheet(QStringLiteral("color: #8a8074;"));
  form->addRow(hullNote);
  connect(hullModeBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    if (loading) return;
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    if (!node || node->kind != Node::Kind::Group) return;
    pushUndo();
    node->hull = index == 1 || index == 2;
    node->hullMode = index == 2 ? 1 : 0;
    node->intersect = index == 3;
    // Keep a name the user gave; follow along when it is still one of ours.
    const QStringList ours = {QStringLiteral("Group"), QStringLiteral("Hull"), QStringLiteral("Chain hull"), QStringLiteral("Intersection")};
    if (ours.contains(node->name)) node->name = ours[index];
    loadHullPage();
    refreshTree();
    refreshScene();
  });
  connect(blendBox, &QDoubleSpinBox::valueChanged, this, [this](double value) {
    if (loading) return;
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    if (!node || node->kind != Node::Kind::Group) return;
    pushUndo();
    node->blend = value < 0.05 ? 0 : value;
    loadHullPage();
    refreshScene();
    status(node->blend > 0 ? QStringLiteral("%1 is joined smoothly, with a %2 mm blend where its parts meet.")
                                 .arg(node->name)
                                 .arg(node->blend, 0, 'f', 1)
                           : QStringLiteral("%1 is a plain, sharp join again.").arg(node->name));
  });
  connect(multiColorBox, &QCheckBox::toggled, this, [this](bool on) {
    if (loading) return;
    Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
    if (!node || node->kind != Node::Kind::Group) return;
    pushUndo();
    node->multiColor = on;
    loadHullPage();
    refreshScene();
    status(on ? QStringLiteral("The group shows its parts' colours, and 3MF and OBJ export it one piece per colour.")
              : QStringLiteral("The group is one colour again."));
  });
}

void MainWindow::loadHullPage() {
  const Node* node = selection.size() == 1 ? doc.find(selection.front()) : nullptr;
  if (!node || !hullModeBox) return;
  const bool wasLoading = loading;
  loading = true;
  hullModeBox->setCurrentIndex(node->intersect ? 3 : !node->hull ? 0 : node->hullMode == 1 ? 2 : 1);
  multiColorBox->setChecked(node->multiColor);
  multiColorBox->setEnabled(!node->hole);
  blendBox->setValue(node->blend);
  blendBox->setEnabled(!node->hull && !node->intersect);
  loading = wasLoading;
  QString note;
  if (node->intersect) note = QStringLiteral("Only what all the parts have in common is kept. Holes inside cut it. Ungroup gives the parts back.");
  else if (node->hull && node->hullMode == 1)
    note = QStringLiteral("Runs through the parts in the order of the list (the order you picked them). Holes inside cut the result.");
  else if (node->hull) note = QStringLiteral("Holes inside cut the hull. Ungroup gives the parts back.");
  else if (node->blend > 0)
    note = QStringLiteral("Smooth join: the parts flow together with a %1 mm blend; flat faces stay flat away from the joins. "
                          "Big groups take a moment to build.").arg(node->blend, 0, 'f', 1);
  if (node->multiColor) note += (note.isEmpty() ? QString() : QStringLiteral(" ")) +
                                QStringLiteral("Where parts of different colours overlap, the one higher in the list keeps its colour.");
  hullNote->setText(note);
  hullNote->setVisible(!note.isEmpty());
}

void MainWindow::loadHolePage() {
  const auto holes = selectedHoles();
  if (holes.empty() || !holeClearanceBox) return;
  const Node* first = holes.front();
  const bool wasLoading = loading;
  loading = true;
  holeClearanceBox->setItemText(1, QStringLiteral("Printer clearance (%1 mm)").arg(printerClearance(), 0, 'f', 2));
  holeClearanceBox->setCurrentIndex(std::clamp(first->clearanceMode, 0, 2));
  if (!holeClearanceSpin->hasFocus()) holeClearanceSpin->setValue(first->holeClearance);
  holeSidesBox->setChecked(first->clearanceSidesOnly);
  loading = wasLoading;
  auto* form = qobject_cast<QFormLayout*>(holePage->layout());
  const bool on = first->clearanceMode != 0;
  if (form) {
    form->setRowVisible(holeClearanceSpin, first->clearanceMode == 2);
    form->setRowVisible(holeSidesBox, on);
    form->setRowVisible(holeClearanceNote, on);
  }
  const double gap = first->clearanceMode == 1 ? printerClearance() : first->holeClearance;
  const QString how = first->clearanceSidesOnly
                          ? QStringLiteral("Cuts %1 mm wider all around its sides; the depth stays as drawn.")
                          : QStringLiteral("Cuts %1 mm wider on every side.");
  holeClearanceNote->setText(on ? how.arg(gap, 0, 'f', 2) +
                                      QStringLiteral(" Its size stays the size you set; the view shows what it cuts.")
                                : QString());
}
