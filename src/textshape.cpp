#include "textshape.h"

#include <QCoreApplication>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainterPath>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace {

// Outlines are pulled from the font at this pixel size. Large enough that hinting and rounding inside
// the font engine are far below the flattening tolerance; the result is scaled to mm afterwards.
constexpr int kReferencePixels = 400;

std::mutex& fontMutex() {
  static std::mutex mutex;
  return mutex;
}

struct Detail {
  const char* name;
  double tolerance;
};

constexpr Detail kDetails[] = {
    {"Draft", 0.05},
    {"Normal", 0.02},
    {"Fine", 0.005},
    {"Ultra", 0.001},
};

double pointLineDistance(double px, double py, double ax, double ay, double bx, double by) {
  const double dx = bx - ax;
  const double dy = by - ay;
  const double len = std::hypot(dx, dy);
  if (len < 1e-12) return std::hypot(px - ax, py - ay);
  return std::abs((px - ax) * dy - (py - ay) * dx) / len;
}

void flattenCubicRec(double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3,
                     double tolerance, int depth, std::vector<double>* out) {
  // A cubic never strays further from its chord than 3/4 of its control points' distance to it.
  const double d1 = pointLineDistance(x1, y1, x0, y0, x3, y3);
  const double d2 = pointLineDistance(x2, y2, x0, y0, x3, y3);
  if (depth >= 18 || 0.75 * std::max(d1, d2) <= tolerance) {
    out->push_back(x3);
    out->push_back(y3);
    return;
  }
  // de Casteljau split at t = 0.5.
  const double x01 = (x0 + x1) * 0.5, y01 = (y0 + y1) * 0.5;
  const double x12 = (x1 + x2) * 0.5, y12 = (y1 + y2) * 0.5;
  const double x23 = (x2 + x3) * 0.5, y23 = (y2 + y3) * 0.5;
  const double xa = (x01 + x12) * 0.5, ya = (y01 + y12) * 0.5;
  const double xb = (x12 + x23) * 0.5, yb = (y12 + y23) * 0.5;
  const double xm = (xa + xb) * 0.5, ym = (ya + yb) * 0.5;
  flattenCubicRec(x0, y0, x01, y01, xa, ya, xm, ym, tolerance, depth + 1, out);
  flattenCubicRec(xm, ym, xb, yb, x23, y23, x3, y3, tolerance, depth + 1, out);
}

// Drops repeated points and a closing point equal to the first; keeps loops with an area (and
// then its curves too, closed, without segments of no length).
void finishLoop(TextLoop* loop, std::vector<TextLoop>* loops, TextCurveLoop* curve = nullptr,
                std::vector<TextCurveLoop>* curves = nullptr) {
  TextCurveLoop segments;
  if (curve) segments.swap(*curve);
  TextLoop clean;
  clean.reserve(loop->size());
  for (size_t i = 0; i + 1 < loop->size(); i += 2) {
    const double x = (*loop)[i];
    const double y = (*loop)[i + 1];
    if (!clean.empty() && std::abs(clean[clean.size() - 2] - x) < 1e-9 && std::abs(clean.back() - y) < 1e-9) continue;
    clean.push_back(x);
    clean.push_back(y);
  }
  while (clean.size() >= 4 && std::abs(clean[0] - clean[clean.size() - 2]) < 1e-9 &&
         std::abs(clean[1] - clean.back()) < 1e-9) {
    clean.pop_back();
    clean.pop_back();
  }
  loop->clear();
  if (clean.size() < 6) return;
  double area = 0;
  const size_t n = clean.size() / 2;
  for (size_t i = 0; i < n; ++i) {
    const size_t j = (i + 1) % n;
    area += clean[i * 2] * clean[j * 2 + 1] - clean[j * 2] * clean[i * 2 + 1];
  }
  if (std::abs(area) < 1e-12) return;
  loops->push_back(std::move(clean));
  if (!curves) return;
  TextCurveLoop kept;
  for (const TextSegment& segment : segments) {
    const bool still = std::abs(segment.x[0] - segment.x[3]) < 1e-9 && std::abs(segment.y[0] - segment.y[3]) < 1e-9;
    if (still && !segment.cubic) continue;
    if (still) {
      bool moves = false;
      for (int k = 1; k < 3; ++k) moves = moves || std::abs(segment.x[k] - segment.x[0]) > 1e-9 || std::abs(segment.y[k] - segment.y[0]) > 1e-9;
      if (!moves) continue;
    }
    kept.push_back(segment);
  }
  if (!kept.empty() && (std::abs(kept.back().x[3] - kept.front().x[0]) > 1e-9 || std::abs(kept.back().y[3] - kept.front().y[0]) > 1e-9)) {
    TextSegment close;
    close.x[0] = kept.back().x[3];
    close.y[0] = kept.back().y[3];
    close.x[3] = kept.front().x[0];
    close.y[3] = kept.front().y[0];
    kept.push_back(close);
  }
  curves->push_back(std::move(kept));
}

QString defaultFamily() {
  return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
}

}  // namespace

int textDetailCount() { return static_cast<int>(sizeof(kDetails) / sizeof(kDetails[0])); }

QString textDetailName(int detail) {
  const Detail& d = kDetails[std::clamp(detail, 0, textDetailCount() - 1)];
  return QStringLiteral("%1 (%2 mm)").arg(QString::fromLatin1(d.name)).arg(d.tolerance);
}

double textDetailTolerance(int detail) { return kDetails[std::clamp(detail, 0, textDetailCount() - 1)].tolerance; }

void flattenCubic(double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3,
                  double tolerance, std::vector<double>* out) {
  flattenCubicRec(x0, y0, x1, y1, x2, y2, x3, y3, std::max(tolerance, 1e-9), 0, out);
}

bool textOutlines(const TextSpec& spec, std::vector<TextLoop>* loops, QString* warning, std::vector<TextCurveLoop>* curves) {
  loops->clear();
  if (curves) curves->clear();
  if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
    if (warning) *warning = QStringLiteral("Text needs the app's font system, which is not running.");
    return false;
  }
  QString text = spec.text;
  text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
  text.replace(QLatin1Char('\t'), QStringLiteral("    "));
  if (text.trimmed().isEmpty()) {
    if (warning) *warning = QStringLiteral("Type some text.");
    return false;
  }

  std::lock_guard<std::mutex> lock(fontMutex());
  const QString family = spec.family.trimmed().isEmpty() ? defaultFamily() : spec.family.trimmed();
  QFont font;
  if (!spec.style.trimmed().isEmpty() && QFontDatabase::styles(family).contains(spec.style.trimmed()))
    font = QFontDatabase::font(family, spec.style.trimmed(), 12);
  else
    font = QFont(family);
  font.setPixelSize(kReferencePixels);
  font.setHintingPreference(QFont::PreferNoHinting);
  font.setStyleStrategy(QFont::StyleStrategy(QFont::PreferOutline | QFont::ForceOutline | QFont::PreferQuality));
  font.setKerning(true);

  const QString used = QFontInfo(font).family();
  if (warning && !used.isEmpty() && !used.startsWith(family, Qt::CaseInsensitive) &&
      !family.startsWith(used, Qt::CaseInsensitive)) {
    *warning = QStringLiteral("The font “%1” is not installed on this computer, so “%2” was used.")
                   .arg(family, used);
  }

  QFontMetricsF metrics(font);
  // Size means the height of a capital H, measured on the outline itself so 20 mm really is 20 mm.
  double capPx = 0;
  if (QFontMetricsF(font).inFont(QLatin1Char('H'))) {
    QPainterPath probe;
    probe.addText(QPointF(0, 0), font, QStringLiteral("H"));
    capPx = probe.boundingRect().height();
  }
  if (!(capPx > 1e-6)) capPx = metrics.capHeight();
  if (!(capPx > 1e-6)) capPx = metrics.ascent() * 0.7;
  if (!(capPx > 1e-6)) {
    if (warning) *warning = QStringLiteral("That font has no usable letter shapes.");
    return false;
  }
  const double capHeight = std::max(spec.capHeight, 0.1);
  const double mmPerPx = capHeight / capPx;
  if (std::abs(spec.spacing) > 1e-9) {
    font.setLetterSpacing(QFont::AbsoluteSpacing, spec.spacing / mmPerPx);
    metrics = QFontMetricsF(font);
  }

  QPainterPath path;
  const QStringList lines = text.split(QLatin1Char('\n'));
  const double pitch = metrics.lineSpacing();
  for (int i = 0; i < lines.size(); ++i) {
    const QString& line = lines[i];
    if (line.trimmed().isEmpty()) continue;
    const double width = metrics.horizontalAdvance(line);
    const double x = spec.align == 0 ? 0.0 : spec.align == 2 ? -width : -width * 0.5;
    path.addText(QPointF(x, pitch * i), font, line);
  }

  // Walk the outline. Fonts give lines and cubic curves (TrueType quadratics arrive as cubics).
  const double tolPx = std::max(spec.tolerance, 1e-5) / mmPerPx;
  TextLoop loop;
  TextCurveLoop curve;
  double lastX = 0;
  double lastY = 0;
  const int count = path.elementCount();
  for (int i = 0; i < count; ++i) {
    const QPainterPath::Element e = path.elementAt(i);
    if (e.isMoveTo()) {
      finishLoop(&loop, loops, &curve, curves);
      loop.push_back(e.x);
      loop.push_back(e.y);
    } else if (e.isLineTo()) {
      loop.push_back(e.x);
      loop.push_back(e.y);
      if (curves) {
        TextSegment line;
        line.x[0] = lastX, line.y[0] = lastY, line.x[3] = e.x, line.y[3] = e.y;
        curve.push_back(line);
      }
    } else if (e.isCurveTo() && i + 2 < count) {
      const QPainterPath::Element c2 = path.elementAt(i + 1);
      const QPainterPath::Element end = path.elementAt(i + 2);
      flattenCubic(lastX, lastY, e.x, e.y, c2.x, c2.y, end.x, end.y, tolPx, &loop);
      if (curves) {
        TextSegment cubic;
        cubic.cubic = true;
        const double xs[4] = {lastX, e.x, c2.x, end.x}, ys[4] = {lastY, e.y, c2.y, end.y};
        for (int k = 0; k < 4; ++k) cubic.x[k] = xs[k], cubic.y[k] = ys[k];
        curve.push_back(cubic);
      }
      i += 2;
      lastX = end.x;
      lastY = end.y;
      continue;
    }
    lastX = e.x;
    lastY = e.y;
  }
  finishLoop(&loop, loops, &curve, curves);

  // Font units run y down; the bed runs y up.
  for (TextLoop& item : *loops) {
    for (size_t i = 0; i + 1 < item.size(); i += 2) {
      item[i] *= mmPerPx;
      item[i + 1] *= -mmPerPx;
    }
  }
  if (curves)
    for (TextCurveLoop& item : *curves)
      for (TextSegment& segment : item)
        for (int k = 0; k < 4; ++k) {
          segment.x[k] *= mmPerPx;
          segment.y[k] *= -mmPerPx;
        }
  if (loops->empty()) {
    if (warning && warning->isEmpty()) *warning = QStringLiteral("That font has no outlines for this text.");
    return false;
  }
  return true;
}
