// Run with QT_QPA_PLATFORM=offscreen. Scripted checks of the 2D editor dialog: stamping modes, typed sizes, measure, drag, undo/redo,
// delete-node behaviour, save/origin stability, the unsaved-changes prompt and stacked levels.
#include "shape2d.h"
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QFile>
#include <QRegularExpression>
#include <cstdio>
#include <QImage>
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Rig {
  QDialog* dlg;
  QWidget* canvas = nullptr;
  QLabel* status = nullptr;
  QList<QDoubleSpinBox*> sel;  // X Y W H
  QPointF scr(QPointF w) const { return QPointF(canvas->width() * 0.5 + w.x() * 5, canvas->height() * 0.5 - w.y() * 5); }
  void mouse(QEvent::Type t, QPointF world, Qt::MouseButton b = Qt::LeftButton, Qt::KeyboardModifiers m = Qt::NoModifier) {
    const QPointF p = scr(world);
    QMouseEvent e(t, p, canvas->mapToGlobal(p), b, t == QEvent::MouseButtonRelease ? Qt::NoButton : b, m);
    QApplication::sendEvent(canvas, &e);
  }
  void click(QPointF w, Qt::KeyboardModifiers m = Qt::NoModifier) {
    mouse(QEvent::MouseMove, w, Qt::NoButton, m);
    mouse(QEvent::MouseButtonPress, w, Qt::LeftButton, m);
    mouse(QEvent::MouseButtonRelease, w, Qt::LeftButton, m);
  }
  void drag(QPointF a, QPointF b) {
    mouse(QEvent::MouseButtonPress, a);
    mouse(QEvent::MouseMove, (a + b) * 0.5, Qt::LeftButton);
    mouse(QEvent::MouseMove, b, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, b);
  }
  void key(int k, Qt::KeyboardModifiers m = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, k, m);
    QApplication::sendEvent(canvas, &press);
  }
  QPushButton* button(const QString& text) {
    for (QPushButton* b : dlg->findChildren<QPushButton*>()) if (b->text() == text) return b;
    return nullptr;
  }
  QComboBox* comboWith(const QString& item) {
    for (QComboBox* c : dlg->findChildren<QComboBox*>()) if (c->findText(item) >= 0) return c;
    return nullptr;
  }
  double x() const { return sel[0]->value(); }
  double y() const { return sel[1]->value(); }
  double w() const { return sel[2]->value(); }
  double h() const { return sel[3]->value(); }
};

static void answerNextDialog(const QString& text, const QString& buttonText) {
  QTimer::singleShot(150, [text, buttonText] {
    QWidget* modal = QApplication::activeModalWidget();
    if (auto* input = qobject_cast<QInputDialog*>(modal)) {
      input->setTextValue(text);
      input->accept();
      return;
    }
    if (auto* box = qobject_cast<QMessageBox*>(modal)) {
      for (QAbstractButton* b : box->buttons())
        if (b->text().contains(buttonText)) { b->click(); return; }
      std::printf("no button %s\n", qPrintable(buttonText));
    }
  });
}

static QString svgCenter(const QString& path) {
  QFile f(path);
  f.open(QIODevice::ReadOnly);
  const auto m = QRegularExpression(QStringLiteral("data-center=\"([^\"]+)\"")).match(QString::fromUtf8(f.readAll()));
  return m.captured(1);
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir lib;
  auto* dlg = new Shape2DEditorDialog(nullptr, lib.path());
  dlg->resize(1320, 880);
  dlg->show();
  app.processEvents();
  Rig r{dlg, nullptr, nullptr, {}};
  for (QWidget* w : dlg->findChildren<QWidget*>())
    if (w->focusPolicy() == Qt::StrongFocus && w->minimumWidth() == 760) r.canvas = w;
  for (QLabel* l : dlg->findChildren<QLabel*>()) if (l->objectName() == "shape2dStatus") r.status = l;
  for (QDoubleSpinBox* s : dlg->findChildren<QDoubleSpinBox*>()) if (s->suffix() == " mm" && s->prefix().isEmpty() && s->objectName().isEmpty()) r.sel << s;
  CHECK(r.canvas && r.status && r.sel.size() == 4, "rig: canvas %p status %p spins %d", (void*)r.canvas, (void*)r.status, (int)r.sel.size());
  if (fails) return 1;

  QComboBox* grid = r.comboWith("0.01 mm");
  CHECK(grid != nullptr, "0.01 mm grid offered");
  r.comboWith("Circle")->setCurrentIndex(r.comboWith("Circle")->findText("Circle"));
  QComboBox* from = r.comboWith("Side to side");
  CHECK(from && from->isVisible(), "draw-from visible for circle");
  from->setCurrentIndex(from->findText("Side to side"));

  // Circle from its left side to its right side.
  r.button("Draw")->click();
  r.click({-10, 0});
  r.click({10, 0});
  std::printf("diameter circle: centre %.3f,%.3f size %.3f x %.3f\n", r.x(), r.y(), r.w(), r.h());
  CHECK(std::abs(r.x()) < 1e-6 && std::abs(r.y()) < 1e-6 && std::abs(r.w() - 20) < 1e-3 && std::abs(r.h() - 20) < 1e-3, "diameter circle");
  // Shift keeps side to side level even if the mouse drifts.
  r.click({20, 0});
  r.click({30, 3}, Qt::ShiftModifier);
  std::printf("level circle: centre %.3f,%.3f size %.3f x %.3f\n", r.x(), r.y(), r.w(), r.h());
  CHECK(std::abs(r.x() - 25) < 1e-6 && std::abs(r.y()) < 1e-6 && std::abs(r.w() - 10) < 1e-3, "shift-level diameter");
  // Centre mode.
  from->setCurrentIndex(from->findText("From centre"));
  r.click({0, 30});
  r.click({4, 30});
  std::printf("centre circle: centre %.3f,%.3f size %.3f\n", r.x(), r.y(), r.w());
  CHECK(std::abs(r.x()) < 1e-6 && std::abs(r.y() - 30) < 1e-6 && std::abs(r.w() - 8) < 1e-3, "centre circle");
  // Type an exact diameter: 3.2 mm (M3 clearance), proportions kept.
  r.sel[2]->setValue(3.2);
  std::printf("typed: %.3f x %.3f at %.3f,%.3f\n", r.w(), r.h(), r.x(), r.y());
  CHECK(std::abs(r.w() - 3.2) < 1e-6 && std::abs(r.h() - 3.2) < 1e-6 && std::abs(r.y() - 30) < 1e-6, "typed size");
  r.button("Select")->click();  // tool off

  // 0.01 mm grid and a nudge.
  grid->setCurrentIndex(grid->findText("0.01 mm"));
  // the new circle is still selected after stamping
  CHECK(std::abs(r.w() - 3.2) < 1e-6, "selected the small circle (w=%.3f)", r.w());
  r.key(Qt::Key_Right);
  r.key(Qt::Key_Right);
  r.key(Qt::Key_Up);
  std::printf("nudged to %.3f, %.3f\n", r.x(), r.y());
  CHECK(std::abs(r.x() - 0.02) < 1e-9 && std::abs(r.y() - 30.01) < 1e-9, "nudge on 0.01 grid");
  // Typed position.
  r.sel[0]->setValue(12.34);
  CHECK(std::abs(r.x() - 12.34) < 1e-9, "typed x");

  // Drag a whole shape (the big circle) by 5,5 on a 1 mm grid.
  grid->setCurrentIndex(grid->findText("1 mm"));
  r.drag({0, 5}, {5, 10});
  std::printf("dragged big circle to %.3f, %.3f\n", r.x(), r.y());
  CHECK(std::abs(r.x() - 5) < 1e-9 && std::abs(r.y() - 5) < 1e-9 && std::abs(r.w() - 20) < 1e-3, "shape drag");
  // Undo / redo.
  r.key(Qt::Key_Z, Qt::ControlModifier);
  r.click({0, -5});
  std::printf("after undo: %.3f, %.3f\n", r.x(), r.y());
  CHECK(std::abs(r.x()) < 1e-9 && std::abs(r.y()) < 1e-9, "undo drag");
  r.key(Qt::Key_Y, Qt::ControlModifier);
  r.click({5, 0});
  CHECK(std::abs(r.x() - 5) < 1e-9, "redo drag (x=%.3f)", r.x());

  // Measure centre to centre: big circle (5,5) to the level circle (25,0).
  r.button("Measure")->click();
  r.click({5, 5});
  r.click({25, 0});
  std::printf("measure: %s\n", qPrintable(r.status->text()));
  CHECK(r.status->text().contains("Distance 20.616 mm"), "measure centre to centre");
  r.button("Measure")->click();

  // Square, corner to corner, then delete a node: it stays closed.
  r.comboWith("Square")->setCurrentIndex(r.comboWith("Square")->findText("Square"));
  from->setCurrentIndex(from->findText("Corner to corner"));
  r.button("Draw")->click();
  r.click({40, 40});
  r.click({50, 50});
  r.button("Select")->click();
  r.mouse(QEvent::MouseButtonPress, {50, 50}, Qt::RightButton);
  r.mouse(QEvent::MouseButtonRelease, {50, 50}, Qt::RightButton);
  r.click({43, 42});  // inside the remaining triangle
  std::printf("square minus a node: selection kind w=%.3f h=%.3f\n", r.w(), r.h());
  CHECK(std::abs(r.w() - 10) < 1e-6 && std::abs(r.h() - 10) < 1e-6 && r.sel[2]->isEnabled(), "still a closed shape after node delete");
  r.key(Qt::Key_Delete);
  CHECK(!r.sel[2]->isEnabled(), "shape deleted");

  // Save, then check the origin stays put after the drawing grows.
  answerNextDialog("bracket", "");
  r.button("Save SVG…")->click();
  const QString svg = lib.path() + "/bracket/bracket.svg";
  CHECK(QFile::exists(svg), "saved %s", qPrintable(svg));
  const QString c1 = svgCenter(svg);
  r.comboWith("Square")->setCurrentIndex(r.comboWith("Square")->findText("Square"));
  r.button("Draw")->click();
  r.click({100, 0});
  r.click({120, 20});
  r.button("Select")->click();
  answerNextDialog("bracket", "");
  r.button("Save SVG…")->click();
  const QString c2 = svgCenter(svg);
  std::printf("origin before %s, after growing %s\n", qPrintable(c1), qPrintable(c2));
  CHECK(!c1.isEmpty() && c1 == c2, "origin kept");
  CHECK(dlg->savedThisSession(), "saved flag");

  // Unsaved change + close: prompt, choose Discard.
  r.drag({110, 10}, {111, 10});
  answerNextDialog("", "Discard");
  dlg->reject();
  CHECK(!dlg->isVisible(), "closed after discard");

  // Reopen: layer reuse after merge.
  auto* dlg2 = new Shape2DEditorDialog(nullptr, lib.path(), svg);
  dlg2->show();
  app.processEvents();
  std::printf("reopened, %s\n", dlg2->isVisible() ? "visible" : "hidden");
  delete dlg2;
  // Shapes drawn in opposite directions: the preview must show them added together (as the 3D side
  // builds them), and Make hole / To new layer must give a ring with a disc inside.
  {
    auto* d3 = new Shape2DEditorDialog(nullptr, lib.path());
    d3->resize(1320, 880);
    d3->show();
    app.processEvents();
    Rig q{d3, nullptr, nullptr, {}};
    for (QWidget* w : d3->findChildren<QWidget*>())
      if (w->focusPolicy() == Qt::StrongFocus && w->minimumWidth() == 760) q.canvas = w;
    for (QLabel* l : d3->findChildren<QLabel*>()) if (l->objectName() == "shape2dStatus") q.status = l;
    for (QDoubleSpinBox* s : d3->findChildren<QDoubleSpinBox*>()) if (s->suffix() == " mm" && s->prefix().isEmpty() && s->objectName().isEmpty()) q.sel << s;
    for (QPointF p : {QPointF(-40, 0), QPointF(0, 20), QPointF(40, 0), QPointF(0, -20), QPointF(-40, 0)}) q.click(p);  // clockwise diamond
    q.comboWith("Circle")->setCurrentIndex(q.comboWith("Circle")->findText("Circle"));
    QComboBox* from3 = q.comboWith("Side to side");
    from3->setCurrentIndex(from3->findText("From centre"));
    q.button("Draw")->click();
    q.click({0, 0}); q.click({14, 0});
    q.click({0, 0}); q.click({6, 0});
    q.button("Select")->click();
    app.processEvents();
    const QImage before = d3->grab().toImage();
    if (argc > 1) before.save(QString::fromLocal8Bit(argv[1]) + "/opposite.png");
    // Pixel at world (10, 0): inside the big circle, outside the small one. Must be filled, like 3D.
    const QPointF probe = q.canvas->mapTo(d3, q.scr({10, 0}).toPoint());
    const QPointF bgProbe = q.canvas->mapTo(d3, q.scr({60, 30}).toPoint());
    const QColor ring = before.pixelColor(probe.toPoint());
    const QColor bg = before.pixelColor(bgProbe.toPoint());
    std::printf("ring pixel %s, background %s\n", qPrintable(ring.name()), qPrintable(bg.name()));
    CHECK(std::abs(ring.red() - bg.red()) > 20, "overlapping shapes preview as filled");
    q.click({10, 0});
    CHECK(std::abs(q.w() - 28) < 1e-3, "picked the big circle (w=%.3f)", q.w());
    q.button("Make hole")->click();
    q.click({0, 0});
    CHECK(std::abs(q.w() - 12) < 1e-3, "picked the small circle (w=%.3f)", q.w());
    q.button("To new layer")->click();
    answerNextDialog("ring", "");
    q.button("Save SVG…")->click();
    auto area = [](const QString& path, bool holes) {
      QFile f(path);
      f.open(QIODevice::ReadOnly);
      const QString text = QString::fromUtf8(f.readAll());
      double total = 0;
      auto it = QRegularExpression(QStringLiteral("<polygon points=\"([^\"]+)\"([^>]*)>")).globalMatch(text);
      while (it.hasNext()) {
        const auto m = it.next();
        if (m.captured(2).contains("data-hole") != holes) continue;
        QVector<QPointF> pts;
        for (const QString& pair : m.captured(1).split(' ', Qt::SkipEmptyParts)) {
          const QStringList xy = pair.split(',');
          pts << QPointF(xy[0].toDouble(), xy[1].toDouble());
        }
        double a = 0;
        for (int i = 0; i < pts.size(); ++i) a += pts[i].x() * pts[(i + 1) % pts.size()].y() - pts[(i + 1) % pts.size()].x() * pts[i].y();
        total += std::abs(a) * 0.5;
      }
      return total;
    };
    const QString dir = lib.path() + "/ring/";
    const double plate = area(dir + "layer-1.svg", false), cut = area(dir + "layer-1.svg", true), disc = area(dir + "layer-2.svg", false);
    std::printf("layer 1: %.1f mm² with a %.1f mm² hole; layer 2: %.1f mm² disc\n", plate, cut, disc);
    CHECK(std::abs(plate - 1600) < 1 && std::abs(cut - 615.75) < 2 && std::abs(disc - 113.1) < 1, "ring and disc saved");
    d3->close();
  }
  // Levels: a floor with a bolt hole on level 1, walls and a tube on level 2 (10 mm), the bolt
  // hole drawn once and cutting both.
  {
    auto* d4 = new Shape2DEditorDialog(nullptr, lib.path());
    d4->resize(1320, 880);
    d4->show();
    app.processEvents();
    Rig q{d4, nullptr, nullptr, {}};
    for (QWidget* w : d4->findChildren<QWidget*>())
      if (w->focusPolicy() == Qt::StrongFocus && w->minimumWidth() == 760) q.canvas = w;
    for (QLabel* l : d4->findChildren<QLabel*>()) if (l->objectName() == "shape2dStatus") q.status = l;
    for (QDoubleSpinBox* s : d4->findChildren<QDoubleSpinBox*>()) if (s->suffix() == " mm" && s->prefix().isEmpty() && s->objectName().isEmpty()) q.sel << s;
    QListWidget* levelList = d4->findChild<QListWidget*>("shape2dLevels");
    QDoubleSpinBox* thickness = d4->findChild<QDoubleSpinBox*>("shape2dLevelThickness");
    QCheckBox* through = nullptr;
    for (QCheckBox* box : d4->findChildren<QCheckBox*>()) if (box->text() == "Through all levels") through = box;
    CHECK(levelList && thickness && through, "level controls present");
    if (!levelList || !thickness || !through) return 1;
    CHECK(levelList->count() == 1 && std::abs(thickness->value() - 1.0) < 1e-9, "one level, 1 mm by default");
    QComboBox* grid4 = q.comboWith("0.01 mm");
    grid4->setCurrentIndex(grid4->findText("0.1 mm"));
    QComboBox* from4 = q.comboWith("Side to side");
    // Level 1: the floor, 40 x 40, and a 3.2 mm bolt hole at (14, 14) that goes through every level.
    q.comboWith("Square")->setCurrentIndex(q.comboWith("Square")->findText("Square"));
    from4->setCurrentIndex(from4->findText("Corner to corner"));
    q.button("Draw")->click();
    q.click({-20, -20});
    q.click({20, 20});
    q.comboWith("Circle")->setCurrentIndex(q.comboWith("Circle")->findText("Circle"));
    from4->setCurrentIndex(from4->findText("From centre"));
    q.click({14, 14});
    q.click({15.6, 14});
    q.button("Make hole")->click();
    CHECK(through->isEnabled(), "through offered for a hole");
    through->setChecked(true);
    // Level 2: 10 mm, walls (copied outline minus an inner hole) and a tube around the bolt hole.
    q.button("Select")->click();
    q.click({0, -19});  // the floor
    q.button("Copy up a level")->click();
    CHECK(levelList->count() == 2 && levelList->item(0)->data(Qt::UserRole).toInt() == 2, "copy up made level 2 and switched to it");
    thickness->setValue(10.0);
    q.comboWith("Square")->setCurrentIndex(q.comboWith("Square")->findText("Square"));
    from4->setCurrentIndex(from4->findText("Corner to corner"));
    q.button("Draw")->click();
    q.click({-18, -18});
    q.click({18, 18});
    q.button("Make hole")->click();
    // The tube: clicked a little off the bolt hole's centre; it snaps onto the faded level below.
    q.comboWith("Circle")->setCurrentIndex(q.comboWith("Circle")->findText("Circle"));
    from4->setCurrentIndex(from4->findText("From centre"));
    q.click({14.3, 14.2});
    q.click({17, 14});
    std::printf("tube: centre %.3f,%.3f diameter %.3f\n", q.x(), q.y(), q.w());
    CHECK(std::abs(q.x() - 14) < 1e-9 && std::abs(q.y() - 14) < 1e-9 && std::abs(q.w() - 6) < 1e-3, "tube snapped onto the bolt hole centre");
    q.button("To new layer")->click();  // the wall's inner hole must not cut the tube
    q.button("Select")->click();
    app.processEvents();
    if (argc > 1) d4->grab().save(QString::fromLocal8Bit(argv[1]) + "/levels.png");
    // Undo the thickness, redo it: undo works across levels.
    q.key(Qt::Key_Z, Qt::ControlModifier);  // To new layer
    q.key(Qt::Key_Y, Qt::ControlModifier);
    answerNextDialog("boltbox", "");
    q.button("Save SVG…")->click();
    const QString dir = lib.path() + "/boltbox/";
    CHECK(QFile::exists(dir + "boltbox.svg") && QFile::exists(dir + "level-1.svg") && QFile::exists(dir + "level-2.svg"), "level files saved");
    CHECK(!QFile::exists(dir + "layer-1.svg") && !QFile::exists(dir + "layer-2.svg"), "no layer files for a stacked shape");
    CHECK(svgCenter(dir + "level-1.svg") == svgCenter(dir + "level-2.svg"), "levels share one origin");
    auto polygons = [](const QString& path, bool holes) {
      QFile f(path);
      f.open(QIODevice::ReadOnly);
      int n = 0;
      auto it = QRegularExpression(QStringLiteral("<polygon points=\"([^\"]+)\"([^>]*)>")).globalMatch(QString::fromUtf8(f.readAll()));
      while (it.hasNext()) n += it.next().captured(2).contains("data-hole") == holes ? 1 : 0;
      return n;
    };
    // Level 2: wall and tube; holes: the wall's inner square, plus the bolt hole once per group.
    std::printf("level 1: %d shapes %d holes; level 2: %d shapes %d holes\n", polygons(dir + "level-1.svg", false),
                polygons(dir + "level-1.svg", true), polygons(dir + "level-2.svg", false), polygons(dir + "level-2.svg", true));
    CHECK(polygons(dir + "level-1.svg", false) == 1 && polygons(dir + "level-1.svg", true) == 1, "level 1 contents");
    CHECK(polygons(dir + "level-2.svg", false) == 2 && polygons(dir + "level-2.svg", true) == 3, "level 2 contents");
    QFile steps(dir + "boltbox.synthvg");
    steps.open(QIODevice::ReadOnly);
    const QJsonArray levels = QJsonDocument::fromJson(steps.readAll()).object()["levels"].toArray();
    CHECK(levels.size() == 2 && std::abs(levels[1].toObject()["height"].toDouble() - 10) < 1e-9, "thicknesses saved");
    d4->close();
    // Reopen: both levels and the thickness come back.
    auto* d5 = new Shape2DEditorDialog(nullptr, lib.path(), dir + "boltbox.svg");
    d5->show();
    app.processEvents();
    QListWidget* reopened = d5->findChild<QListWidget*>("shape2dLevels");
    CHECK(reopened && reopened->count() == 2, "reopened with two levels");
    if (reopened) {
      reopened->setCurrentRow(0);
      emit reopened->itemPressed(reopened->item(0));
      CHECK(std::abs(d5->findChild<QDoubleSpinBox*>("shape2dLevelThickness")->value() - 10) < 1e-9, "level 2 is 10 mm after reopening");
    }
    d5->close();
  }
  std::printf(fails ? "%d FAILED\n" : "editor checks passed\n", fails);
  return fails ? 1 : 0;
}
