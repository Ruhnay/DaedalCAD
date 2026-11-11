#pragma once

#include <QString>

#include <vector>

// 3D text: turns a line (or lines) of text in any installed font into flat outlines in millimetres,
// ready to extrude. Outlines come straight from the font's curves and are flattened here with a fixed
// maximum error in mm, so text stays smooth at any size instead of picking up a blocky look.
struct TextSpec {
  QString text;
  QString family;          // empty: the system's default font
  QString style;           // a style the family offers ("Bold", "Light Italic", …); empty: regular
  double capHeight = 20;   // height of a capital letter, mm
  double spacing = 0;      // extra space between letters, mm (negative pulls them together)
  int align = 1;           // 0 left, 1 centre, 2 right (matters with more than one line)
  double tolerance = 0.005;  // largest distance a flattened curve may stray from the real one, mm
};

// One closed loop, x/y pairs in mm, y up. Orientation is whatever the font uses: fill with non-zero.
using TextLoop = std::vector<double>;

// The same loop as the font draws it: straight lines and cubic Béziers, in mm, y up. For the exact
// STEP export. A line uses x[0],y[0] to x[3],y[3]; a cubic all four.
struct TextSegment {
  bool cubic = false;
  double x[4] = {0, 0, 0, 0};
  double y[4] = {0, 0, 0, 0};
};
using TextCurveLoop = std::vector<TextSegment>;

// The detail levels shown in the app, coarse to fine.
int textDetailCount();
QString textDetailName(int detail);
double textDetailTolerance(int detail);

// Builds the outlines. Needs a running QGuiApplication (fonts come from the system). Safe to call
// from worker threads. Returns false with a reason when nothing could be built; may also return true
// with a note in *warning, e.g. when the font is not installed on this computer and another was used.
// `curves`, when given, gets each loop of `loops` (the same ones, in the same order) as curves.
bool textOutlines(const TextSpec& spec, std::vector<TextLoop>* loops, QString* warning,
                  std::vector<TextCurveLoop>* curves = nullptr);

// Flattens one cubic Bézier from p0 (not emitted) to p3 (emitted) into *out as x/y pairs, adding
// points until no part of the curve is further than `tolerance` from the polyline.
void flattenCubic(double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3,
                  double tolerance, std::vector<double>* out);
