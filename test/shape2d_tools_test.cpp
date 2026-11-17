// Run with QT_QPA_PLATFORM=offscreen. The geometry behind the 2D editor's tools: turning and flipping,
// combining shapes, rounding corners, holes in imported drawings, text, the stamped shapes and SVG import.
#include "shape2d.h"

#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTransform>
#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(c, ...)                                  \
  do {                                                 \
    if (!(c)) {                                        \
      ++fails;                                         \
      std::printf("FAIL line %d: ", __LINE__);         \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

static Shape2DContour box(double x0, double y0, double x1, double y1, bool clockwise = false) {
  Shape2DContour c;
  c.closed = true;
  QVector<QPointF> pts{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
  if (clockwise) std::reverse(pts.begin(), pts.end());
  for (const QPointF& p : pts) c.nodes.push_back(Shape2DNode{p, {}, {}});
  return c;
}

// Area of an outline, its curves traced finely.
static double area(const Shape2DContour& c) {
  QVector<QPointF> pts;
  for (int i = 0; i < c.nodes.size(); ++i) {
    const Shape2DNode& a = c.nodes[i];
    const Shape2DNode& b = c.nodes[(i + 1) % c.nodes.size()];
    for (int s = 0; s < 64; ++s) {
      const double t = s / 64.0, u = 1 - t;
      const QPointF c1 = a.pos + a.outHandle, c2 = b.pos + b.inHandle;
      pts.push_back(u * u * u * a.pos + 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t * b.pos);
    }
  }
  double s = 0;
  for (int i = 0; i < pts.size(); ++i) {
    const QPointF& p = pts[i];
    const QPointF& q = pts[(i + 1) % pts.size()];
    s += p.x() * q.y() - q.x() * p.y();
  }
  return std::abs(s) / 2;
}

// What the 3D side builds: per cut number, shapes minus holes.
static double filledArea(const QVector<Shape2DContour>& cs) {
  double total = 0;
  for (const Shape2DContour& c : cs) total += c.hole ? -area(c) : area(c);
  return total;
}

static int holes(const QVector<Shape2DContour>& cs) {
  int n = 0;
  for (const Shape2DContour& c : cs) n += c.hole ? 1 : 0;
  return n;
}

static QString writeSvg(const QTemporaryDir& dir, const char* name, const char* body) {
  const QString path = dir.filePath(QString::fromLatin1(name));
  QFile f(path);
  f.open(QIODevice::WriteOnly);
  f.write(body);
  return path;
}

static bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);

  // --- turning and flipping keep curves exact ---
  {
    QVector<Shape2DContour> ring = shape2d::shapesInBox(8, QPointF(-10, -10), QPointF(10, 10), {});
    const double before = filledArea(ring);
    QTransform turn;
    turn.rotate(37);
    for (Shape2DContour& c : ring) shape2d::transform(c, turn);
    CHECK(near(filledArea(ring), before, 1e-6), "a turned ring keeps its area: %f vs %f", filledArea(ring), before);
    Shape2DContour l = box(0, 0, 10, 2);
    l.nodes.push_back(Shape2DNode{QPointF(2, 6), {}, {}});
    l.nodes.swapItemsAt(3, 4);
    shape2d::transform(l, QTransform::fromScale(-1, 1));
    const QRectF b = shape2d::bounds({l});
    CHECK(near(b.left(), -10, 1e-9) && near(b.right(), 0, 1e-9), "a flip mirrors left to right");
  }

  // --- combining ---
  {
    const Shape2DContour a = box(0, 0, 10, 10), b = box(5, 0, 15, 10);
    const auto merged = shape2d::combine({a, b}, shape2d::Combine::Merge);
    const auto cut = shape2d::combine({a, b}, shape2d::Combine::Cut);
    const auto overlap = shape2d::combine({a, b}, shape2d::Combine::Overlap);
    CHECK(merged.size() == 1 && near(filledArea(merged), 150, 1e-3), "merge: one outline of 150 mm² (%d, %f)", int(merged.size()), filledArea(merged));
    CHECK(cut.size() == 1 && near(filledArea(cut), 50, 1e-3), "cut: 50 mm² left");
    CHECK(overlap.size() == 1 && near(filledArea(overlap), 50, 1e-3), "overlap: 50 mm²");
    CHECK(merged[0].nodes.size() == 4, "merge of two boxes has 4 corners, not %d", int(merged[0].nodes.size()));
    // Cutting a small box out of the middle makes a frame: a shape and a hole that share a cut number.
    const auto frame = shape2d::combine({box(0, 0, 30, 30), box(10, 10, 20, 20)}, shape2d::Combine::Cut);
    CHECK(frame.size() == 2 && holes(frame) == 1 && frame[0].cut == frame[1].cut, "cut out of the middle: shape and hole");
    CHECK(near(filledArea(frame), 800, 1e-3), "frame area 800, got %f", filledArea(frame));
    // Merge counts the holes in the selection: a box with a hole over it is that frame.
    QVector<Shape2DContour> boxAndHole{box(0, 0, 30, 30), box(10, 10, 20, 20)};
    boxAndHole[1].hole = true;
    const auto seen = shape2d::combine(boxAndHole, shape2d::Combine::Merge);
    CHECK(seen.size() == 2 && holes(seen) == 1 && near(filledArea(seen), 800, 1e-3), "merge subtracts the selected holes");
    // Holes alone merge into one hole.
    QVector<Shape2DContour> twoHoles{box(0, 0, 10, 10), box(5, 0, 15, 10)};
    for (auto& c : twoHoles) c.hole = true;
    const auto oneHole = shape2d::combine(twoHoles, shape2d::Combine::Merge);
    CHECK(oneHole.size() == 1 && oneHole[0].hole && near(area(oneHole[0]), 150, 1e-3), "two holes merge into one hole");
    // Curves survive as fine straight segments.
    const auto circles = shape2d::combine({shape2d::shapesInBox(4, QPointF(0, 0), QPointF(20, 20), {}).front(),
                                           shape2d::shapesInBox(4, QPointF(10, 0), QPointF(30, 20), {}).front()},
                                          shape2d::Combine::Merge);
    const double lens = 2 * 100 * std::acos(0.5) - 5 * std::sqrt(300.0);
    CHECK(circles.size() == 1 && near(filledArea(circles), 2 * 3.14159265 * 100 - lens, 0.5),
          "two merged circles: %f", filledArea(circles));
  }

  // --- rounding corners ---
  {
    Shape2DContour square = box(0, 0, 10, 10);
    CHECK(shape2d::roundAllCorners(square, 2) == 4 && square.nodes.size() == 8, "four corners rounded");
    CHECK(near(area(square), 100 - 4 * (4 - 3.14159265), 0.02), "rounded square area %f", area(square));
    Shape2DContour big = box(0, 0, 10, 10);
    double used = 0;
    CHECK(shape2d::roundCorner(big, 0, 20, &used) && used < 20 && used > 4, "a radius too big is made to fit: %f", used);
    Shape2DContour line;
    line.nodes = {Shape2DNode{QPointF(0, 0), {}, {}}, Shape2DNode{QPointF(5, 0), {}, {}}, Shape2DNode{QPointF(10, 0), {}, {}}};
    CHECK(!shape2d::roundCorner(line, 1, 1) && !shape2d::roundCorner(line, 0, 1), "no corner on a straight or open end");
  }

  // --- holes in a drawing ---
  {
    QVector<Shape2DContour> nested{box(0, 0, 30, 30), box(5, 5, 25, 25), box(10, 10, 20, 20)};
    shape2d::assignHoles(nested, true);
    CHECK(!nested[0].hole && nested[1].hole && !nested[2].hole, "even-odd: shape, hole, island");
    CHECK(nested[0].cut == nested[1].cut && nested[2].cut != nested[1].cut, "the island is not cut by the hole");
    QVector<Shape2DContour> same{box(0, 0, 30, 30), box(5, 5, 25, 25)};
    shape2d::assignHoles(same, false);
    CHECK(!same[1].hole, "non-zero: an inner outline drawn the same way round fills");
    QVector<Shape2DContour> opposite{box(0, 0, 30, 30), box(5, 5, 25, 25, true)};
    shape2d::assignHoles(opposite, false);
    CHECK(opposite[1].hole, "non-zero: drawn the other way round it is a hole");
  }

  // --- text ---
  {
    const auto o = shape2d::textOutline(QStringLiteral("O"), QFont(QFont().family(), 100));
    CHECK(o.size() == 2 && holes(o) == 1, "an O is a shape with a hole (%d outlines)", int(o.size()));
    bool curved = false;
    for (const auto& c : o)
      for (const auto& n : c.nodes) curved = curved || n.outHandle != QPointF();
    CHECK(curved, "letters keep their curves");
    shape2d::StampOptions options;
    options.text = QStringLiteral("Hi");
    const auto fitted = shape2d::shapesInBox(13, QPointF(0, 0), QPointF(40, 10), options);
    const QRectF b = shape2d::bounds(fitted);
    CHECK(!fitted.isEmpty() && b.width() <= 40.01 && b.height() <= 10.01 && (near(b.width(), 40, 0.05) || near(b.height(), 10, 0.05)),
          "text fills its box keeping proportions: %f x %f", b.width(), b.height());
  }

  // --- stamped shapes ---
  {
    shape2d::StampOptions options;
    options.sides = 5;
    options.inner = 50;
    const auto star = shape2d::shapesInBox(6, QPointF(-10, -10), QPointF(10, 10), options);
    CHECK(star.size() == 1 && star[0].nodes.size() == 10, "a 5-point star has 10 corners");
    const auto slot = shape2d::shapesInBox(7, QPointF(0, 0), QPointF(20, 10), options);
    CHECK(slot.size() == 1 && near(area(slot[0]), 100 + 3.14159265 * 25, 0.05), "slot area %f", slot.empty() ? 0 : area(slot[0]));
    options.inner = 25;
    const auto ring = shape2d::shapesInBox(8, QPointF(-10, -10), QPointF(10, 10), options);
    CHECK(ring.size() == 2 && holes(ring) == 1 && near(filledArea(ring), 3.14159265 * (100 - 56.25), 0.1), "ring");
    options.angle = 180;
    const auto arc = shape2d::shapesInBox(9, QPointF(-10, -10), QPointF(10, 10), options);
    const QRectF ab = shape2d::bounds(arc);
    CHECK(arc.size() == 1 && near(ab.bottom(), 10, 1e-6) && near(ab.top(), 0, 1e-6) && near(filledArea(arc), 3.14159265 * 43.75 / 2, 0.1),
          "a half arc sits in the top half: %f..%f", ab.top(), ab.bottom());
    for (int kind : {10, 11, 12}) {
      options.sides = 3;
      options.inner = 15;
      const auto shapes = shape2d::shapesInBox(kind, QPointF(0, 0), QPointF(30, 20), options);
      const QRectF b = shape2d::bounds(shapes);
      CHECK(!shapes.isEmpty() && b.left() >= -0.01 && b.right() <= 30.01 && b.top() >= -0.01 && b.bottom() <= 20.01,
            "shape %d stays inside its box", kind);
    }
  }

  // --- SVG import ---
  {
    QTemporaryDir dir;
    auto load = [&](const char* name, const char* body) {
      QString error;
      const auto out = shape2d::readSvg(writeSvg(dir, name, body), &error);
      if (out.isEmpty()) std::printf("  %s: %s\n", name, qPrintable(error));
      return out;
    };
    const auto evenodd = load("evenodd.svg",
                              "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><path fill-rule='evenodd' "
                              "d='M10 10 h80 v80 h-80 z m20 20 v40 h40 v-40 z'/></svg>");
    CHECK(evenodd.size() == 2 && holes(evenodd) == 1 && near(filledArea(evenodd), 6400 - 1600, 1e-6), "relative path with a hole");
    const auto mm = load("mm.svg",
                         "<svg xmlns='http://www.w3.org/2000/svg' width='100mm' height='50mm' viewBox='0 0 200 100'>"
                         "<rect width='200' height='100'/></svg>");
    CHECK(mm.size() == 1 && near(shape2d::bounds(mm).width(), 100, 1e-6) && near(shape2d::bounds(mm).height(), 50, 1e-6),
          "a size in mm is kept");
    const auto circle = load("circle.svg", "<svg xmlns='http://www.w3.org/2000/svg'><circle cx='5' cy='5' r='10'/></svg>");
    CHECK(circle.size() == 1 && near(area(circle[0]), 3.14159265 * 100, 0.2), "circle area %f", circle.empty() ? 0 : area(circle[0]));
    const auto line = load("line.svg",
                           "<svg xmlns='http://www.w3.org/2000/svg'><line x1='0' y1='0' x2='10' y2='0' stroke='black' "
                           "stroke-width='2'/></svg>");
    CHECK(line.size() == 1 && near(shape2d::bounds(line).width(), 12, 0.01) && near(shape2d::bounds(line).height(), 2, 0.005),
          "a stroked line becomes a bar with round ends: %d, %f x %f", int(line.size()), shape2d::bounds(line).width(),
          shape2d::bounds(line).height());
    const auto outline = load("outline.svg",
                              "<svg xmlns='http://www.w3.org/2000/svg'><circle r='10' fill='none' stroke='#000' style='stroke-width:1'/></svg>");
    CHECK(outline.size() == 2 && holes(outline) == 1 && near(filledArea(outline), 3.14159265 * (10.5 * 10.5 - 9.5 * 9.5), 0.3),
          "an outlined circle becomes a ring");
    const auto arc = load("arc.svg", "<svg xmlns='http://www.w3.org/2000/svg'><path d='M0 0 A10 10 0 0 1 20 0 Z'/></svg>");
    CHECK(arc.size() == 1 && near(area(arc[0]), 3.14159265 * 50, 0.2) && near(shape2d::bounds(arc).height(), 10, 0.01),
          "an arc: half a circle");
    const auto large = load("large.svg", "<svg xmlns='http://www.w3.org/2000/svg'><path d='M0 0 a10 10 0 1 0 0.001 0 z'/></svg>");
    CHECK(large.size() == 1 && near(area(large[0]), 3.14159265 * 100, 0.5), "a large arc is nearly a full circle");
    const auto moved = load("moved.svg",
                            "<svg xmlns='http://www.w3.org/2000/svg'><g transform='translate(10,0) scale(2)'><rect width='5' height='5'/>"
                            "</g><g style='display:none'><rect width='500' height='5'/></g><defs><rect width='900' height='9'/></defs></svg>");
    CHECK(moved.size() == 1 && near(shape2d::bounds(moved).width(), 10, 1e-6), "transforms apply, hidden and defs are skipped");
    const auto quad = load("quad.svg", "<svg xmlns='http://www.w3.org/2000/svg'><path d='M0 0Q10 10 20 0T40 0Z'/></svg>");
    CHECK(quad.size() == 1 && near(shape2d::bounds(quad).width(), 40, 1e-6), "quadratic curves");
    const auto packed = load("packed.svg", "<svg xmlns='http://www.w3.org/2000/svg'><path d='M0,0L10-5.5.5.5z'/></svg>");
    CHECK(packed.size() == 1 && packed[0].nodes.size() == 3, "numbers written without spaces");
    const auto parts = load("parts.svg",
                            "<svg xmlns='http://www.w3.org/2000/svg'><path fill-rule='evenodd' d='M0 0h30v30h-30z M5 5h20v20h-20z'/>"
                            "<rect x='10' y='10' width='10' height='10'/></svg>");
    int ringCut = -1, dotCut = -2;
    for (const auto& c : parts) {
      if (c.hole) ringCut = c.cut;
      else if (shape2d::bounds({c}).width() < 11) dotCut = c.cut;
    }
    CHECK(parts.size() == 3 && ringCut != dotCut, "a dot drawn inside another element's hole is not cut by it");
    QString error;
    CHECK(shape2d::readSvg(writeSvg(dir, "empty.svg", "<svg xmlns='http://www.w3.org/2000/svg'><text>hi</text></svg>"), &error).isEmpty() &&
              !error.isEmpty(),
          "nothing usable: empty, with a reason");
    const QRectF centred = shape2d::bounds(circle);
    CHECK(near(centred.center().x(), 0, 1e-6) && near(centred.center().y(), 0, 1e-6), "imports are centred on 0,0");
  }

  std::printf("%s\n", fails ? "shape2d_tools: FAILED" : "shape2d_tools: all passed");
  return fails ? 1 : 0;
}
