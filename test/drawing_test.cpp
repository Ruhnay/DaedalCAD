// Technical drawings: views, hidden lines, holes as circles, dimensions, the filament estimate and
// the files they are written to.
#include <QFile>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>
#include "document.h"
#include "drawing.h"
#include "evaluator.h"

static int failures = 0;

static void expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) ++failures;
}

static bool hasText(const drawing::Sheet& sheet, const QString& text) {
  for (const auto& t : sheet.texts)
    if (t.text == text) return true;
  return false;
}

static bool hasPart(const drawing::Sheet& sheet, const QString& text) {
  for (const auto& t : sheet.texts)
    if (t.text.contains(text)) return true;
  return false;
}

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  QString error;
  // A wall 40 wide, 6 deep and 30 high with a 10 mm hole through it at x 28, z 18.
  Document doc;
  const QString wall = doc.addShape("box", 0, 0);
  Node* w = doc.find(wall);
  w->size[0] = 40;
  w->size[1] = 6;
  w->size[2] = 30;
  w->pos[0] = 20;
  w->pos[2] = 15;
  const QString hole = doc.addShape("cylinder", 0, 0);
  Node* h = doc.find(hole);
  h->radius = 5;
  h->size[2] = 16;
  h->rot[0] = 90;
  h->pos[0] = 28;
  h->pos[2] = 18;
  h->hole = true;
  const CpuMesh mesh = evaluateSolid(doc, {}, &error);
  const drawing::Model model = drawing::analyse(mesh);
  expect(model.valid, "analysed");
  const double hollow = 40 * 6 * 30 - M_PI * 25 * 6;
  expect(std::abs(model.volume - hollow) / hollow < 0.01, "volume");
  expect(std::abs(model.front.bounds.width() - 40) < 1e-3 && std::abs(model.front.bounds.height() - 30) < 1e-3, "front view 40 x 30");
  expect(std::abs(model.top.bounds.height() - 6) < 1e-3, "top view 6 deep");
  int visibleCircles = 0;
  for (const auto& c : model.front.circles)
    if (c.visible && std::abs(c.radius - 5) < 0.01 && std::abs(c.centre.x() - 28) < 0.01 && std::abs(c.centre.y() - 18) < 0.01) ++visibleCircles;
  expect(visibleCircles == 1, "the hole is one visible Ø10 circle at 28, 18 in the front view");
  // The bore seen from above: two hidden lines at x 23 and 33, as deep as the wall.
  int bore = 0;
  for (const auto& s : model.top.hidden) {
    const double length = std::hypot(s.second.x() - s.first.x(), s.second.y() - s.first.y());
    if (std::abs(s.first.x() - s.second.x()) < 1e-3 && std::abs(length - 6) < 0.05 &&
        (std::abs(s.first.x() - 23) < 0.01 || std::abs(s.first.x() - 33) < 0.01))
      ++bore;
  }
  std::printf("  top view: %zu visible, %zu hidden lines\n", model.top.visible.size(), model.top.hidden.size());
  expect(bore == 2, "the bore shows dashed in the top view");
  // The front outline joins into four lines (plus the circle's short ones).
  int longLines = 0;
  for (const auto& s : model.front.visible)
    if (std::hypot(s.second.x() - s.first.x(), s.second.y() - s.first.y()) > 25) ++longLines;
  expect(longLines == 4, "the front outline is four whole lines");
  expect(model.front.hidden.empty(), "nothing hidden in the front view");

  // A block with rounded corners: its sides are seen edge on, and still make the outline.
  {
    Document round;
    const QString id = round.addShape("box", 0, 0, 3);
    Node* n = round.find(id);
    n->size[0] = 50;
    n->size[1] = 20;
    n->size[2] = 10;
    n->pos[2] = 5;
    const drawing::Model m = drawing::analyse(evaluateSolid(round, {}, &error));
    for (const drawing::View* v : {&m.front, &m.top}) {
      const QRectF b = v->bounds;
      bool leftEdge = false, rightEdge = false;
      for (const auto& s : v->visible) {
        const bool upright = std::abs(s.first.x() - s.second.x()) < 1e-3 && std::abs(s.first.y() - s.second.y()) > b.height() * 0.3;
        if (upright && std::abs(s.first.x() - b.left()) < 0.05) leftEdge = true;
        if (upright && std::abs(s.first.x() - b.right()) < 0.05) rightEdge = true;
      }
      expect(leftEdge && rightEdge, v == &m.front ? "rounded block, front view: both sides outlined" : "rounded block, top view: both sides outlined");
    }
  }

  // Filament: 1 cm3 cube, all shell (walls thicker than half of it): 1.24 g of PLA.
  drawing::Filament pla;
  pla.walls = 2;
  const auto solid = drawing::estimate(1000, 600, pla);
  expect(std::abs(solid.grams - 1.24) < 1e-6, "a small cube prints solid");
  pla.walls = 0.8;
  pla.infill = 20;
  const auto box = drawing::estimate(1e6, 6e4, pla);  // a 100 mm cube
  const double expected = (6e4 * 0.8 + 0.2 * (1e6 - 6e4 * 0.8)) / 1000 * 1.24;
  expect(std::abs(box.grams - expected) < 1e-6, "a big cube: shell plus infill");
  expect(std::abs(box.cost - box.grams / 1000 * 20) < 1e-9, "cost from the price per kg");
  expect(std::abs(box.metres - (box.grams / 1.24 * 1000) / (M_PI * 1.75 * 1.75 / 4) / 1000) < 1e-6, "length of 1.75 mm filament");

  // The sheet.
  drawing::Options options;
  options.title = QStringLiteral("Wall");
  options.date = QStringLiteral("2026-10-20");
  double scale = 0;
  const drawing::Sheet sheet = drawing::layout(model, options, &scale);
  std::printf("  A3 sheet at %s: %zu lines, %zu texts\n", qPrintable(drawing::scaleText(scale)), sheet.lines.size(), sheet.texts.size());
  expect(scale == 2, "a 40 mm part on A3 comes out at 2:1");
  expect(hasText(sheet, "40") && hasText(sheet, "30") && hasText(sheet, "6"), "overall sizes 40, 30 and 6");
  expect(hasText(sheet, QStringLiteral("Ø10")), "the hole's diameter");
  expect(hasText(sheet, "28") && hasText(sheet, "18"), "the hole's position, 28 and 18");
  expect(hasText(sheet, "2:1") && hasText(sheet, "First angle") && hasText(sheet, "Wall"), "title block");
  expect(hasText(sheet, "Filament estimate"), "filament table");
  bool inside = true;
  for (const auto& l : sheet.lines)
    for (QPointF p : {l.a, l.b})
      if (p.x() < 9.99 || p.y() < 9.99 || p.x() > 410.01 || p.y() > 287.01) inside = false;
  expect(inside, "everything inside the frame");

  QTemporaryDir dir;
  const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : dir.path();
  expect(drawing::writeSvg(sheet, out + "/wall.svg", &error), "SVG written");
  expect(drawing::writeDxf(sheet, out + "/wall.dxf", &error), "DXF written");
  expect(drawing::writePdf(sheet, out + "/wall.pdf", &error), "PDF written");
  expect(drawing::writePng(sheet, out + "/wall.png", 150, &error), "PNG written");
  QFile dxf(out + "/wall.dxf");
  dxf.open(QIODevice::ReadOnly);
  const QByteArray dxfText = dxf.readAll();
  expect(dxfText.contains("%%c10") && dxfText.contains("HIDDEN") && dxfText.endsWith("EOF\n"), "DXF: diameter, hidden layer, complete");
  QFile pdf(out + "/wall.pdf");
  expect(pdf.size() > 1000, "PDF has content");
  pdf.open(QIODevice::ReadOnly);
  const QString pdfText = QString::fromLatin1(pdf.readAll());
  const auto page = QRegularExpression(QStringLiteral("MediaBox \\[0 0 ([0-9.]+) ([0-9.]+)\\]")).match(pdfText);
  const bool rotated = pdfText.contains(QStringLiteral("/Rotate 90")) || pdfText.contains(QStringLiteral("/Rotate 270"));
  const bool wide = page.hasMatch() && page.captured(1).toDouble() > page.captured(2).toDouble();
  std::printf("  PDF page %s x %s%s\n", qPrintable(page.captured(1)), qPrintable(page.captured(2)), rotated ? ", turned" : "");
  expect(wide != rotated && page.hasMatch() && std::abs(std::max(page.captured(1).toDouble(), page.captured(2).toDouble()) - 1190.55) < 1,
         "PDF: an A3 page, landscape");

  // Third angle and A4 still fit.
  options.thirdAngle = true;
  options.paper = QStringLiteral("A4");
  const drawing::Sheet a4 = drawing::layout(model, options, &scale);
  std::printf("  A4 third angle at %s\n", qPrintable(drawing::scaleText(scale)));
  expect(hasText(a4, "Third angle") && scale >= 1, "A4, third angle");
  drawing::writePng(a4, out + "/wall_a4.png", 150, &error);

  // Too big at the scale asked for: an overview first, then a sheet per view, in pieces when needed.
  {
    auto allInside = [](const drawing::Sheet& s) {
      for (const auto& l : s.lines)
        for (QPointF p : {l.a, l.b})
          if (p.x() < 9.99 || p.y() < 9.99 || p.x() > s.size.width() - 9.99 || p.y() > s.size.height() - 9.99) return false;
      for (const auto& t : s.texts)
        if (t.at.x() < 9.99 || t.at.y() < 9.99 || t.at.x() > s.size.width() - 9.99 || t.at.y() > s.size.height() - 9.99) return false;
      return true;
    };
    options.thirdAngle = false;
    options.paper = QStringLiteral("A3");
    options.scale = 5;
    double used = 0;
    const auto sheets = drawing::layoutSheets(model, options, &used);
    std::printf("  A3 at 5:1: %zu sheets\n", sheets.size());
    expect(sheets.size() == 4 && used == 5, "40 mm at 5:1 on A3: an overview and three views");
    expect(hasPart(sheets[0], "Too big for one sheet") && hasText(sheets[0], "1 of 4"), "the overview says where the views are");
    expect(hasPart(sheets[1], "Front view, 5:1") && hasText(sheets[1], "2 of 4") && hasText(sheets[1], "5:1"), "sheet 2: the front view at 5:1");
    expect(hasText(sheets[1], "40") && hasText(sheets[1], "30") && !hasText(sheets[1], "200"), "its sizes are the real ones");
    bool all = true;
    for (const auto& s : sheets) all = all && allInside(s);
    expect(all, "every sheet inside its frame");
    expect(drawing::writePdf(sheets, out + "/wall_5to1.pdf", &error), "multi-sheet PDF written");
    QFile multi(out + "/wall_5to1.pdf");
    multi.open(QIODevice::ReadOnly);
    const QByteArray multiText = multi.readAll();
    expect(multiText.count("/Type /Page\n") + multiText.count("/Type /Page ") + multiText.count("/Type /Page/") == 4 ||
               QString::fromLatin1(multiText).count(QRegularExpression(QStringLiteral("/Type\\s*/Page[^s]"))) == 4,
           "the PDF has four pages");
    for (size_t i = 0; i < sheets.size(); ++i) drawing::writePng(sheets[i], out + QStringLiteral("/wall_5to1_%1.png").arg(i + 1), 100, &error);
    options.paper = QStringLiteral("A4");
    options.scale = 20;
    const auto tiled = drawing::layoutSheets(model, options);
    std::printf("  A4 at 20:1: %zu sheets\n", tiled.size());
    expect(tiled.size() > 4, "at 20:1 the views come in pieces");
    bool pieces = false;
    for (const auto& s : tiled) pieces = pieces || hasPart(s, "piece 2 of");
    all = true;
    for (const auto& s : tiled) all = all && allInside(s);
    expect(pieces && all, "pieces are labelled and stay inside their frames");
    drawing::writePng(tiled[1], out + "/wall_20to1_piece.png", 100, &error);
    options.scale = 1;
    expect(drawing::layoutSheets(model, options).size() == 1, "at a scale that fits: one sheet");
  }

  std::printf(failures ? "%d failure(s)\n" : "all drawing checks passed\n", failures);
  return failures ? 1 : 0;
}
