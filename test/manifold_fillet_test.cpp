// End-to-end surface fillet check against the real Manifold kernel, the way the app uses it:
// a disc with pockets, the top face's exact outlines and walls, one cutter per outline, subtract,
// then measure the finished profile against a true fillet that is tangent to both the top face and
// the (possibly drafted) pocket wall.
//
// Build: cmake -DDAEDALCAD_FILLET_TESTS=ON ..  then run fillet_manifold_test.
#include "surfacefillet.h"
#include <manifold/cross_section.h>
#include <manifold/manifold.h>
#include <algorithm>
#include <cmath>
#include <random>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace manifold;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
constexpr double kPiF = 3.14159265358979323846;  // pi; manifold also exports kPi

static Manifold reuleauxPocket(double side, double depth, double cx, double cy, double floorScale) {
  SimplePolygon poly;
  vec2 V[3];
  for (int c = 0; c < 3; ++c) {
    const double a = (90 + 120.0 * c) * kPiF / 180.0;
    V[c] = vec2(side * floorScale / std::sqrt(3.0) * std::cos(a), side * floorScale / std::sqrt(3.0) * std::sin(a));
  }
  for (int c = 0; c < 3; ++c) {
    const vec2 O = V[c], A = V[(c + 1) % 3], B = V[(c + 2) % 3];
    const double a0 = std::atan2(A.y - O.y, A.x - O.x);
    double sw = std::atan2(B.y - O.y, B.x - O.x) - a0;
    while (sw > kPiF) sw -= 2 * kPiF;
    while (sw < -kPiF) sw += 2 * kPiF;
    for (int i = 0; i < 24; ++i) {
      const double a = a0 + sw * i / 24;
      poly.push_back(vec2(O.x + side * floorScale * std::cos(a), O.y + side * floorScale * std::sin(a)));
    }
  }
  // Extruded from the floor up, growing back to full size at the top face.
  return Manifold::Extrude(Polygons{poly}, depth, 0, 0.0, vec2(1.0 / floorScale, 1.0 / floorScale)).Translate(vec3(cx, cy, 0));
}

struct Part { Manifold solid; double top, depth, roundR, roundCx, roundCy; };

static Part makePart(double draftDeg) {
  Part p{Manifold(), 20, 14, 9, 0, -22};
  const double ta = std::tan(draftDeg * kPiF / 180.0);
  const double side = 30;
  Manifold disc = Manifold::Cylinder(p.top, 60, 60, 128);
  // Every pocket is exactly its nominal size where it meets the top face, drafted below that, and
  // carried 0.02 mm above the face on the same slope so the cut is clean.
  const double ext = 0.02;
  const double floorA = (side - 2 * p.depth * ta) / side, topA = (side + 2 * ext * ta) / side;
  Manifold a = reuleauxPocket(side * topA, p.depth + ext, -22, 18, floorA / topA).Translate(vec3(0, 0, p.top - p.depth));
  Manifold b = Manifold::Cylinder(p.depth + ext, p.roundR - p.depth * ta, p.roundR + ext * ta, 64)
                   .Translate(vec3(p.roundCx, p.roundCy, p.top - p.depth));
  // A box pocket whose walls lean differently: 5 degrees more on the long sides than on the short.
  const double hx = 8, hy = 7;
  const double tx = std::tan(draftDeg * kPiF / 180.0), ty = std::tan((draftDeg + 5) * kPiF / 180.0);
  const double fx = hx - p.depth * tx, fy = hy - p.depth * ty;
  Manifold box = Manifold::Extrude(Polygons{{vec2(-fx, -fy), vec2(fx, -fy), vec2(fx, fy), vec2(-fx, fy)}}, p.depth + ext, 0, 0.0,
                                   vec2((hx + ext * tx) / fx, (hy + ext * ty) / fy))
                     .Translate(vec3(26, -20, p.top - p.depth));
  p.solid = disc - a - b - box;
  return p;
}

// Distance from a point (D into the material, z) to the ideal boundary: top face, fillet arc, wall.
static double distIdeal(double D, double z, double r, double draftDeg) {
  const double a = draftDeg * kPiF / 180.0;
  const double wx = -std::cos(a), wz = std::sin(a);
  const double cx = r * (wz - 1) / wx, cz = -r, bx = cx + r * wx, bz = -r + r * wz;
  double phiB = std::atan2(wz, wx);
  if (phiB < 0) phiB += 2 * kPiF;
  double best = 1e9;
  if (D >= cx) best = std::min(best, std::abs(z));
  double phi = std::atan2(z - cz, D - cx);
  if (phi < 0) phi += 2 * kPiF;
  if (phi <= phiB + 1e-12 && phi >= kPiF / 2 - 1e-12) best = std::min(best, std::abs(std::hypot(D - cx, z - cz) - r));
  best = std::min(best, std::hypot(D - cx, z));
  best = std::min(best, std::hypot(D - bx, z - bz));
  const double qx = -std::sin(a), qz = -std::cos(a);
  if ((D - bx) * qx + (z - bz) * qz > 0) best = std::min(best, std::abs((D - bx) * qz - (z - bz) * qx));
  return best;
}


// Hairline cracks, counted the way a GPU would draw them: vertices snapped to 1/256 pixel, integer
// edge functions, top-left fill rule, so triangles that truly share an edge never leave a gap. A
// pixel that differs sharply from two agreeing neighbours is a crack (a T-junction or a sliver step
// in the mesh), which is what showed up as dotted seams across the fillet band.
static int crackPixels(const Manifold& m, const vec3& eye, const vec3& target, double fovDeg) {
  const int W = 900, H = 600;
  std::vector<double> zb(size_t(W) * H, 1e300), lum(size_t(W) * H, 0.0);
  const vec3 f = linalg::normalize(target - eye), rt = linalg::normalize(linalg::cross(f, vec3(0, 0, 1))), up = linalg::cross(rt, f);
  const double fl = 0.5 * H / std::tan(fovDeg * kPiF / 360.0);
  const vec3 key = linalg::normalize(vec3(0.45, -0.55, 0.85)), fill = linalg::normalize(vec3(-0.7, 0.45, 0.25));
  const auto gl = m.GetMeshGL();
  for (size_t t = 0; t + 2 < gl.triVerts.size(); t += 3) {
    vec3 P[3]; long long X[3], Y[3]; double d[3];
    bool behind = false;
    for (int k = 0; k < 3; ++k) {
      const float* v = &gl.vertProperties[gl.triVerts[t + k] * gl.numProp];
      P[k] = vec3(v[0], v[1], v[2]);
      const vec3 q = P[k] - eye;
      d[k] = linalg::dot(q, f);
      if (d[k] < 0.1) behind = true;
      X[k] = std::llround((W * 0.5 + fl * linalg::dot(q, rt) / d[k]) * 256.0);
      Y[k] = std::llround((H * 0.5 - fl * linalg::dot(q, up) / d[k]) * 256.0);
    }
    if (behind) continue;
    vec3 n = linalg::cross(P[1] - P[0], P[2] - P[0]);
    const double nl = linalg::length(n);
    n = nl > 1e-12 ? n / nl : vec3(0, 0, 1);
    if (linalg::dot(n, eye - P[0]) < 0) n = -n;
    const double light = 0.32 + 0.18 * (n.z * 0.5 + 0.5) + std::max(linalg::dot(n, key), 0.0) * 0.72 + std::max(linalg::dot(n, fill), 0.0) * 0.22;
    long long area = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
    if (area == 0) continue;
    int i1 = 1, i2 = 2;
    if (area < 0) { std::swap(i1, i2); area = -area; }
    const long long ax = X[0], ay = Y[0], bx = X[i1], by = Y[i1], cx = X[i2], cy = Y[i2];
    const double da = d[0], db = d[i1], dc = d[i2];
    auto edge = [](long long x0, long long y0, long long x1, long long y1, long long px, long long py) { return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0); };
    auto bias = [](long long x0, long long y0, long long x1, long long y1) { return ((y0 == y1 && x1 < x0) || y1 < y0) ? 0LL : -1LL; };
    const long long b0 = bias(bx, by, cx, cy), b1 = bias(cx, cy, ax, ay), b2 = bias(ax, ay, bx, by);
    const int x0 = std::max(0, int(std::min({ax, bx, cx}) >> 8)), x1 = std::min(W - 1, int((std::max({ax, bx, cx}) >> 8) + 1));
    const int y0 = std::max(0, int(std::min({ay, by, cy}) >> 8)), y1 = std::min(H - 1, int((std::max({ay, by, cy}) >> 8) + 1));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const long long px = (long long)x * 256 + 128, py = (long long)y * 256 + 128;
        const long long w0 = edge(bx, by, cx, cy, px, py) + b0, w1 = edge(cx, cy, ax, ay, px, py) + b1, w2 = edge(ax, ay, bx, by, px, py) + b2;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        const double l0 = double(w0 - b0) / area, l1 = double(w1 - b1) / area, l2 = 1.0 - l0 - l1;
        const double depth = 1.0 / (l0 / da + l1 / db + l2 / dc);
        const size_t i = size_t(y) * W + x;
        if (depth >= zb[i]) continue;
        zb[i] = depth;
        lum[i] = 255.0 * light * 0.76;
      }
  }
  int cracks = 0;
  for (int y = 1; y + 1 < H; ++y)
    for (int x = 1; x + 1 < W; ++x) {
      const double c = lum[size_t(y) * W + x], l = lum[size_t(y) * W + x - 1], r = lum[size_t(y) * W + x + 1];
      const double u = lum[size_t(y - 1) * W + x], dn = lum[size_t(y + 1) * W + x];
      const bool h = std::abs(l - r) < 8 && std::abs(c - 0.5 * (l + r)) > 25;
      const bool v = std::abs(u - dn) < 8 && std::abs(c - 0.5 * (u + dn)) > 25;
      if (h || v) ++cracks;
    }
  return cracks;
}

static void run(double draftDeg, double r, int n) {
  char label[96];
  std::snprintf(label, sizeof label, "draft %+5.1f  r %.1f  segments %2d", draftDeg, r, n);
  const Part part = makePart(draftDeg);
  CHECK(part.solid.Status() == Manifold::Error::NoError, "%s: test part is broken", label);

  sf::Frame frame;
  frame.origin[2] = part.top;
  frame.normal[2] = 1;
  frame.axis[0] = 1;
  const std::vector<sf::FaceRings> faces = sf::coplanarFaces(part.solid, frame, nullptr);
  CHECK(faces.size() == 1, "%s: expected one top face, got %zu", label, faces.size());
  if (faces.empty()) return;
  CHECK(faces[0].rings.size() == 4, "%s: expected outer outline + 3 pockets, got %zu", label, faces[0].rings.size());
  CHECK(faces[0].walls.size() == faces[0].rings.size(), "%s: walls missing", label);

  std::vector<Manifold> ops{part.solid};
  int sweeps = 0;
  for (size_t k = 0; k < faces[0].rings.size(); ++k) {
    std::string warn, log;
    Manifold c = sf::buildCutter(faces[0].rings[k], frame, r, n, true, &warn, &log, faces[0].walls[k]);
    CHECK(c.Status() == Manifold::Error::NoError && !c.IsEmpty(), "%s: outline %zu gave no cutter (%s)", label, k, warn.c_str());
    if (log.find("one smooth sweep") != std::string::npos) ++sweeps;
    else std::printf("  note: outline %zu used the piecewise cutter: %s", k, log.c_str());
    ops.push_back(c);
  }
  CHECK(sweeps == static_cast<int>(faces[0].rings.size()), "%s: only %d of %zu outlines got the smooth sweep", label,
        sweeps, faces[0].rings.size());
  const Manifold result = Manifold::BatchBoolean(ops, OpType::Subtract);
  CHECK(result.Status() == Manifold::Error::NoError, "%s: subtraction failed", label);

  // No hairline cracks across the fillets: looking into the round pocket and across the rounded
  // triangle's tip, where seams used to show at every station on drafted walls.
  {
    // Cameras sit a few radii from the fillet so it fills the frame.
    const double tipY = 18 + 30 / std::sqrt(3.0);
    const int round = crackPixels(result, vec3(part.roundCx, part.roundCy + part.roundR - 4 * r, part.top + 3 * r),
                                  vec3(part.roundCx, part.roundCy + part.roundR, part.top - r), 40);
    const int side = crackPixels(result, vec3(-22 + 3 * r, tipY - 9 * r, part.top + 3 * r), vec3(-22 + 4 * r, tipY - 3 * r, part.top - r), 40);
    if (std::getenv("CRACKS")) std::printf("  cracks %s: round %d side %d\n", label, round, side);
    CHECK(round <= 3 && side <= 3, "%s: %d crack pixels in the round pocket, %d along the rounded side", label, round, side);
  }
  // Profiles, each measured along a ray from inside a pocket out through its wall, against a true
  // fillet for that wall's own draft: the round pocket, and the box's 'draft' and 'draft + 5' walls.
  struct Ray { const char* what; double cx, cy, ux, uy, rim, draft; };
  const Ray rays[] = {{"round", part.roundCx, part.roundCy, 1, 0, part.roundR, draftDeg},
                      {"box side", 26, -20, 1, 0, 8, draftDeg},
                      {"box long side", 26, -20, 0, 1, 7, draftDeg + 5}};
  for (const Ray& ray : rays) {
    const double a = ray.draft * kPiF / 180.0;
    const double sweep = kPiF / 2 + std::max(0.0, -a) - std::max(0.0, a);   // the arc's angular extent
    const double sagitta = r * (1 - std::cos(sweep / n / 2));
    double worst = 0, worstT = 0;
    int samples = 0;
    for (double t = 0.01; t < r * 1.6; t += 0.01) {
      // nearest crossing of the slice outline with the ray, beyond half the rim distance
      double R = 1e9;
      for (const auto& poly : result.Slice(part.top - t))
        for (size_t k = 0; k < poly.size(); ++k) {
          const vec2 p0 = poly[k], p1 = poly[(k + 1) % poly.size()];
          const double ax = p0.x - ray.cx, ay = p0.y - ray.cy, bx = p1.x - ray.cx, by = p1.y - ray.cy;
          const double sa = ax * ray.uy - ay * ray.ux, sb = bx * ray.uy - by * ray.ux;   // side of the ray line
          if ((sa > 0) == (sb > 0) || sa == sb) continue;
          const double f = sa / (sa - sb);
          const double along = (ax + (bx - ax) * f) * ray.ux + (ay + (by - ay) * f) * ray.uy;
          if (along > ray.rim * 0.5 && along < R) R = along;
        }
      if (R > 1e8) continue;
      ++samples;
      const double e = distIdeal(R - ray.rim, -t, r, ray.draft);
      if (e > worst) { worst = e; worstT = t; }
    }
    const double budget = sagitta + 0.004;
    CHECK(samples > 20, "%s %s: profile could not be measured", label, ray.what);
    CHECK(worst <= budget, "%s %s: profile strays %.4f mm from a true fillet at depth %.2f (budget %.4f)", label, ray.what,
          worst, worstT, budget);
    std::printf("%s  %-13s within %.4f mm of a true fillet (facet sagitta %.4f)\n", label, ray.what, worst, sagitta);
  }
}

static void matching() {
  // Face matching: a rough, welded copy of an outline must find its exact face; a stray one must not.
  const Part part = makePart(8);
  sf::Frame frame;
  frame.origin[2] = part.top;
  frame.normal[2] = 1;
  frame.axis[0] = 1;
  const auto faces = sf::coplanarFaces(part.solid, frame, nullptr);
  if (faces.empty()) { CHECK(false, "matching: no face"); return; }
  std::vector<double> rough = faces[0].rings[0];
  for (size_t i = 0; i < rough.size(); i += 3) { rough[i] += 0.04; rough[i + 1] -= 0.04; }
  std::vector<std::vector<double>> walls;
  const auto got = sf::faceRingsNear(part.solid, frame, rough, nullptr, &walls);
  CHECK(got.size() == faces[0].rings.size(), "matching: expected %zu outlines, got %zu", faces[0].rings.size(), got.size());
  CHECK(walls.size() == got.size(), "matching: walls missing");
  const std::vector<double> stray = {500, 500, 20, 520, 500, 20, 520, 520, 20};
  CHECK(sf::faceRingsNear(part.solid, frame, stray, nullptr).empty(), "matching: a stray outline matched a face");
  // Outer outline counter-clockwise about the normal, pockets clockwise.
  auto area = [](const std::vector<double>& r) {
    double s = 0;
    const size_t n = r.size() / 3;
    for (size_t i = 0; i < n; ++i) { const size_t j = (i + 1) % n; s += r[i * 3] * r[j * 3 + 1] - r[j * 3] * r[i * 3 + 1]; }
    return s / 2;
  };
  CHECK(area(faces[0].rings[0]) > 0, "matching: outer outline winds the wrong way");
  for (size_t k = 1; k < faces[0].rings.size(); ++k) CHECK(area(faces[0].rings[k]) < 0, "matching: pocket %zu winds the wrong way", k);
  std::printf("face matching and winding OK\n");
}


// ---------------------------------------------------------------------------------------------
// Groups of tubes (two overlapping rings, three rings at 120 degrees): the case where the fillet
// showed dotted lines along its tangent lines and differed from hole to hole. Every outline must get
// the smooth sweep, and the result must hold no wrong-shade sliver: a triangle narrower than 0.02 mm
// whose face normal agrees (up to sign; the viewport lights back faces) with none of its neighbours
// to within 15 degrees. Those are what the viewport paints as dots.
static Manifold tube(double R, double r, double h, double x, double y, double spin = 0) {
  const CrossSection ring = CrossSection::Circle(R, 128) - CrossSection::Circle(r, 128);   // primitiveOf("tube")
  return Manifold::Extrude(ring.ToPolygons(), h, 0, 0.0, vec2(1, 1)).Translate(vec3(0, 0, -h / 2))
      .Rotate(0, 0, spin).Translate(vec3(x, y, h / 2));
}

static int wrongShadeSlivers(const Manifold& m) {
  const auto gl = m.GetMeshGL();   // the viewport works from float positions
  const size_t nt = gl.triVerts.size() / 3;
  auto P = [&](size_t v) { const float* p = &gl.vertProperties[v * gl.numProp]; return vec3(p[0], p[1], p[2]); };
  std::vector<vec3> nrm(nt);
  std::vector<double> thin(nt), longest(nt);
  std::map<std::pair<uint32_t, uint32_t>, size_t> edgeTri;
  for (size_t t = 0; t < nt; ++t) {
    const vec3 a = P(gl.triVerts[t * 3]), b = P(gl.triVerts[t * 3 + 1]), c = P(gl.triVerts[t * 3 + 2]);
    const vec3 cr = linalg::cross(b - a, c - a);
    const double l = std::max({linalg::length(b - a), linalg::length(c - b), linalg::length(a - c)});
    longest[t] = l;
    thin[t] = l > 0 ? linalg::length(cr) / l : 0;
    nrm[t] = linalg::length(cr) > 1e-12 ? linalg::normalize(cr) : vec3(0, 0, 1);
    for (int k = 0; k < 3; ++k) edgeTri[{gl.triVerts[t * 3 + k], gl.triVerts[t * 3 + (k + 1) % 3]}] = t;
  }
  int bad = 0;
  for (size_t t = 0; t < nt; ++t) {
    if (thin[t] > 0.02 || longest[t] < 0.05) continue;
    double best = -1;
    for (int k = 0; k < 3; ++k) {
      const auto it = edgeTri.find({gl.triVerts[t * 3 + (k + 1) % 3], gl.triVerts[t * 3 + k]});
      if (it != edgeTri.end()) best = std::max(best, std::abs(linalg::dot(nrm[t], nrm[it->second])));
    }
    if (best < std::cos(15 * kPiF / 180)) ++bad;
  }
  return bad;
}

static void tubeGroup(const char* label, const std::vector<Manifold>& tubes, double top, double r, int n) {
  const Manifold solid = Manifold::BatchBoolean(tubes, OpType::Add);
  sf::Frame frame;
  frame.origin[2] = top;
  frame.normal[2] = 1;
  frame.axis[0] = 1;
  std::vector<Manifold> cutters;
  int outlines = 0, smooth = 0;
  for (const auto& face : sf::coplanarFaces(solid, frame, nullptr))
    for (size_t k = 0; k < face.rings.size(); ++k) {
      std::string warn, log;
      Manifold c = sf::buildCutter(face.rings[k], frame, r, n, true, &warn, &log, face.walls[k]);
      ++outlines;
      if (log.find("one smooth sweep") != std::string::npos) ++smooth;
      if (!c.IsEmpty()) cutters.push_back(c);
    }
  // The app unions a group's fillet objects, then cuts them from the solid.
  const Manifold result = solid - Manifold::BatchBoolean(cutters, OpType::Add);
  const int bad = wrongShadeSlivers(result);
  CHECK(outlines > 0 && smooth == outlines, "%s: only %d of %d outlines got the smooth sweep", label, smooth, outlines);
  CHECK(bad == 0, "%s: %d wrong-shade slivers (dotted lines in the viewport)", label, bad);
  std::printf("%-44s %d outlines, all smooth: %s, wrong-shade slivers %d\n", label, outlines, smooth == outlines ? "yes" : "no", bad);
}

static void tubeGroups() {
  const double h = 28;
  for (double r : {1.0, 2.0})
    for (int n : {4, 12}) {
      char label[96];
      std::snprintf(label, sizeof label, "two rings, fillet %.0f/%d", r, n);
      tubeGroup(label, {tube(14, 10.5, h, -10, 0), tube(14, 10.5, h, 10, 0)}, h, r, n);
      std::snprintf(label, sizeof label, "three rings rotated 120 deg, fillet %.0f/%d", r, n);
      std::vector<Manifold> three;
      for (int k = 0; k < 3; ++k) three.push_back(tube(14, 9, h, 0, 9, 0).Rotate(0, 0, 120.0 * k));
      tubeGroup(label, three, h, r, n);
      std::snprintf(label, sizeof label, "three rings snapped to 1 mm, fillet %.0f/%d", r, n);
      tubeGroup(label, {tube(14, 9, h, 0, 8), tube(14, 9, h, -7, -4), tube(14, 9, h, 7, -4)}, h, r, n);
    }
  // A spread of random overlapping groups: sizes, scales, spins, spacing, snapped or not.
  std::mt19937 rng(2026);
  std::uniform_real_distribution<double> U(0, 1);
  for (int trial = 0; trial < 12; ++trial) {
    const int count = 2 + trial % 2;
    const double R = 12 + 6 * U(rng), wall = 3.5 + 3 * U(rng), rIn = R - wall, height = 10 + 20 * U(rng);
    const double spin = U(rng) < 0.5 ? 0 : 360 * U(rng);
    const double d = (count == 2 ? 0.7 + 0.5 * U(rng) : 0.45 + 0.3 * U(rng)) * rIn;
    std::vector<Manifold> tubes;
    for (int k = 0; k < count; ++k) {
      const double a = (90 + 360.0 / count * k) * kPiF / 180;
      double x = d * std::cos(a), y = d * std::sin(a);
      if (trial % 4 >= 2) { x = std::round(x); y = std::round(y); }
      tubes.push_back(tube(R, rIn, height, x, y, spin));
    }
    const double fr = std::min(0.4 * wall, 1 + 2 * U(rng));   // bands from neighbouring outlines stay apart
    const int n = U(rng) < 0.5 ? 4 : 12;
    char label[96];
    std::snprintf(label, sizeof label, "random group %d (%d tubes), fillet %.2f/%d", trial, count, fr, n);
    tubeGroup(label, tubes, height, fr, n);
  }
}

int main() {
  matching();
  tubeGroups();
  for (double draft : {-10.0, 0.0, 5.0, 15.0})
    for (double r : {1.0, 3.0})
      for (int n : {4, 12}) run(draft, r, n);
  std::printf(fails ? "\n%d FAILURES\n" : "\nALL PASSED\n", fails);
  return fails != 0;
}
