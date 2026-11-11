// Text shape checks: run with QT_QPA_PLATFORM=offscreen. Builds text the way the app does (outlines,
// non-zero fill, extrude) and checks the solids are closed, have the right holes, the right size, and
// that curves stay within the chosen smoothness.
#include "textshape.h"

#include <manifold/cross_section.h>
#include <manifold/manifold.h>

#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <cstdio>

using manifold::CrossSection;
using manifold::Manifold;
using manifold::Polygons;
using manifold::SimplePolygon;
using manifold::vec2;

static int failures = 0;
#define CHECK(cond, ...)                                   \
  do {                                                     \
    if (!(cond)) {                                         \
      ++failures;                                          \
      std::printf("FAIL %s:%d  ", __FILE__, __LINE__);     \
      std::printf(__VA_ARGS__);                            \
      std::printf("\n");                                   \
    }                                                      \
  } while (0)

static CrossSection section(const std::vector<TextLoop>& loops) {
  Polygons polys;
  for (const TextLoop& loop : loops) {
    SimplePolygon poly;
    for (size_t i = 0; i + 1 < loop.size(); i += 2) poly.push_back(vec2(loop[i], loop[i + 1]));
    polys.push_back(poly);
  }
  return CrossSection(polys, CrossSection::FillRule::NonZero);
}

static Manifold solidFor(const TextSpec& spec, double depth, QString* warning = nullptr) {
  std::vector<TextLoop> loops;
  QString why;
  if (!textOutlines(spec, &loops, &why)) return {};
  if (warning) *warning = why;
  return Manifold::Extrude(section(loops).ToPolygons(), depth);
}

static void flattenAccuracy() {
  // Quarter circle of radius 10 as a cubic; the polyline must stay within tolerance of the cubic.
  const double k = 0.5522847498 * 10;
  const double p[4][2] = {{10, 0}, {10, k}, {k, 10}, {0, 10}};
  auto bez = [&](double t, double* x, double* y) {
    const double u = 1 - t;
    *x = u * u * u * p[0][0] + 3 * u * u * t * p[1][0] + 3 * u * t * t * p[2][0] + t * t * t * p[3][0];
    *y = u * u * u * p[0][1] + 3 * u * u * t * p[1][1] + 3 * u * t * t * p[2][1] + t * t * t * p[3][1];
  };
  for (double tol : {0.05, 0.02, 0.005, 0.001}) {
    std::vector<double> out{p[0][0], p[0][1]};
    flattenCubic(p[0][0], p[0][1], p[1][0], p[1][1], p[2][0], p[2][1], p[3][0], p[3][1], tol, &out);
    double worst = 0;
    for (int s = 0; s <= 4000; ++s) {
      double x, y;
      bez(s / 4000.0, &x, &y);
      double best = 1e9;
      for (size_t i = 0; i + 3 < out.size(); i += 2) {
        const double ax = out[i], ay = out[i + 1], bx = out[i + 2], by = out[i + 3];
        const double dx = bx - ax, dy = by - ay;
        double t = ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy);
        t = std::clamp(t, 0.0, 1.0);
        best = std::min(best, std::hypot(ax + dx * t - x, ay + dy * t - y));
      }
      worst = std::max(worst, best);
    }
    std::printf("flatten tol %.3f: %zu segments, worst %.5f mm\n", tol, out.size() / 2 - 1, worst);
    CHECK(worst <= tol, "flatten tol %g worst %g", tol, worst);
    CHECK(out.size() / 2 - 1 <= 64, "too many segments %zu", out.size() / 2 - 1);
  }
}

static void glyphs(const QString& family) {
  TextSpec spec;
  spec.family = family;
  spec.capHeight = 20;
  struct Case {
    const char* text;
    int genus;
  } cases[] = {{"H", 0}, {"O", 1}, {"A", 1}, {"B", 2}, {"8", 2}, {"D", 1}};
  for (const Case& c : cases) {
    spec.text = QString::fromLatin1(c.text);
    const Manifold solid = solidFor(spec, 5);
    CHECK(solid.Status() == Manifold::Error::NoError, "%s '%s' status", qPrintable(family), c.text);
    CHECK(!solid.IsEmpty(), "%s '%s' empty", qPrintable(family), c.text);
    CHECK(solid.Genus() == c.genus, "%s '%s' genus %d want %d", qPrintable(family), c.text, solid.Genus(), c.genus);
  }
  // Capital height is what the user typed.
  spec.text = QStringLiteral("H");
  const auto box = solidFor(spec, 5).BoundingBox();
  const double h = box.max.y - box.min.y;
  std::printf("%-14s H height %.4f mm (asked 20)\n", qPrintable(family), h);
  CHECK(std::abs(h - 20) < 0.002, "%s cap height %g", qPrintable(family), h);
}

static void detailLevels() {
  TextSpec spec;
  spec.family = QStringLiteral("DejaVu Serif");
  spec.text = QStringLiteral("Smooth @ 3D");
  spec.capHeight = 10;
  double ultraVol = 0;
  for (int d = textDetailCount() - 1; d >= 0; --d) {
    spec.tolerance = textDetailTolerance(d);
    const Manifold solid = solidFor(spec, 2);
    CHECK(solid.Status() == Manifold::Error::NoError, "detail %d status", d);
    const double vol = solid.Volume();
    if (d == textDetailCount() - 1) ultraVol = vol;
    std::printf("%-22s %7zu triangles  volume %.3f\n", qPrintable(textDetailName(d)), solid.NumTri(), vol);
    CHECK(std::abs(vol - ultraVol) / ultraVol < 0.01, "detail %d volume off by %g", d, (vol - ultraVol) / ultraVol);
  }
}

static void layout() {
  TextSpec spec;
  spec.family = QStringLiteral("DejaVu Sans");
  spec.capHeight = 10;
  spec.text = QStringLiteral("AB\nAAAAAA");
  for (int align = 0; align < 3; ++align) {
    spec.align = align;
    std::vector<TextLoop> loops;
    QString why;
    CHECK(textOutlines(spec, &loops, &why), "two lines");
    // Split points by line (first line sits above y = -3 mm, the second below).
    double lo[2] = {1e9, 1e9}, hi[2] = {-1e9, -1e9};
    for (const TextLoop& loop : loops)
      for (size_t i = 0; i + 1 < loop.size(); i += 2) {
        const int line = loop[i + 1] > -3 ? 0 : 1;
        lo[line] = std::min(lo[line], loop[i]);
        hi[line] = std::max(hi[line], loop[i]);
      }
    const double a = align == 0 ? lo[0] - lo[1] : align == 2 ? hi[0] - hi[1] : (lo[0] + hi[0] - lo[1] - hi[1]) / 2;
    std::printf("align %d: line offset %.3f mm\n", align, a);
    CHECK(std::abs(a) < 1.2, "align %d off by %g", align, a);
  }
  // Letter spacing widens the text by (letters - 1) * spacing, roughly.
  spec.text = QStringLiteral("IIIII");
  spec.align = 0;
  spec.spacing = 0;
  const double w0 = solidFor(spec, 1).BoundingBox().Size().x;
  spec.spacing = 2;
  const double w2 = solidFor(spec, 1).BoundingBox().Size().x;
  std::printf("spacing 0 -> %.3f mm, spacing 2 -> %.3f mm\n", w0, w2);
  CHECK(std::abs((w2 - w0) - 8) < 0.5, "spacing grew %g", w2 - w0);
}

static void edgeCases() {
  TextSpec spec;
  std::vector<TextLoop> loops;
  QString why;
  spec.text = QStringLiteral("   \n ");
  CHECK(!textOutlines(spec, &loops, &why), "blank text should not build");
  CHECK(!why.isEmpty(), "blank text should say why");
  why.clear();
  spec.text = QStringLiteral("Hi");
  spec.family = QStringLiteral("No Such Font 123");
  CHECK(textOutlines(spec, &loops, &why), "missing font falls back");
  CHECK(why.contains(QStringLiteral("not installed")), "missing font warning: %s", qPrintable(why));
  // Styles from the font database.
  spec.family = QStringLiteral("DejaVu Sans");
  spec.style = QStringLiteral("Bold");
  spec.text = QStringLiteral("l");
  why.clear();
  const double bold = solidFor(spec, 1).BoundingBox().Size().x;
  spec.style = QStringLiteral("Book");
  const double book = solidFor(spec, 1).BoundingBox().Size().x;
  std::printf("'l' stem: book %.3f mm, bold %.3f mm\n", book, bold);
  CHECK(bold > book * 1.2, "bold not bolder");
  // Overlapping contours (script/variable fonts) must merge, not cancel.
  spec.style.clear();
  spec.text = QStringLiteral("ÅÄÖ&");
  const Manifold solid = solidFor(spec, 3);
  CHECK(solid.Status() == Manifold::Error::NoError && !solid.IsEmpty(), "accents");
}

static void picture(const char* path) {
  // Left: Draft. Right: Ultra. Same letters, drawn big so the facets are visible.
  const int W = 1400, H = 520;
  QImage img(W, H, QImage::Format_RGB32);
  img.fill(QColor(250, 248, 244));
  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing);
  TextSpec spec;
  spec.family = QStringLiteral("DejaVu Serif");
  spec.text = QStringLiteral("Sg@");
  spec.capHeight = 10;
  const int levels[2] = {0, textDetailCount() - 1};
  for (int k = 0; k < 2; ++k) {
    spec.tolerance = textDetailTolerance(levels[k]);
    std::vector<TextLoop> loops;
    QString why;
    textOutlines(spec, &loops, &why);
    const auto polys = section(loops).ToPolygons();
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (const auto& poly : polys)
      for (const vec2& v : poly) {
        minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
        minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
      }
    const double scale = std::min((W / 2 - 60) / (maxX - minX), (H - 110) / (maxY - minY));
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const auto& poly : polys) {
      QPolygonF q;
      for (const vec2& v : poly) q << QPointF(k * W / 2 + 30 + (v.x - minX) * scale, H - 50 - (v.y - minY) * scale);
      path.addPolygon(q);
      path.closeSubpath();
    }
    p.setPen(QPen(QColor(120, 70, 20), 1.2));
    p.setBrush(QColor(232, 116, 0, 120));
    p.drawPath(path);
    p.setPen(QColor(40, 40, 40));
    QFont f(QStringLiteral("DejaVu Sans"));
    f.setPixelSize(22);
    p.setFont(f);
    p.drawText(k * W / 2 + 30, 36, textDetailName(levels[k]));
  }
  p.end();
  img.save(QString::fromLatin1(path));
}

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  flattenAccuracy();
  for (const char* family : {"DejaVu Sans", "DejaVu Serif", "FreeSans", "Poppins"}) {
    if (QFontDatabase::families().contains(QString::fromLatin1(family))) glyphs(QString::fromLatin1(family));
  }
  detailLevels();
  layout();
  edgeCases();
  if (argc > 1) picture(argv[1]);
  std::printf(failures ? "%d FAILED\n" : "all text checks passed\n", failures);
  return failures ? 1 : 0;
}
