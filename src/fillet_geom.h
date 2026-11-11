#pragma once
// Pure geometry for surface fillets. No Qt and no Manifold in here, so it can be unit tested alone.
//
// Frame: the face plane is z = 0 and the face outline lies in x/y. Walking the outline, the face
// is always on the LEFT (outer ring counter-clockwise, hole ring clockwise).
//
// A fillet cutter for one outline is built from two kinds of piece:
//   strip: the corner cross-section swept along one outline edge (a prism).
//   dome : the same cross-section revolved about a vertex, filling the wedge left between two
//          strips where the outline turns sharply away from the face. Shallow turns need no dome;
//          the strips are simply run on to meet each other (see cornerExtend).
// The union is then clipped to the face side of the outline by the caller.
//
// Two small offsets keep the booleans well conditioned, and both matter:
//   over  pushes the cutter OUT past the wall, so no cutter face ever lands on the solid's wall.
//   bite  pushes the cutter IN past each tangent line by a hair, so no cutter EDGE lies inside the
//         solid's top face or wall either. A fillet meets both surfaces tangentially, and an edge
//         lying exactly in a face is the boolean's worst case: at float precision it resolves
//         differently from facet to facet, which is what stippled the tangent lines with slivers.
// Both are a few microns, far below a printed layer, and far above float32 noise at part scale.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace fg {

constexpr double kPi = 3.14159265358979323846;

struct Vec2 {
  double x = 0;
  double y = 0;
};

struct RawMesh {
  std::vector<double> v;    // x y z per vertex
  std::vector<uint32_t> t;  // triangle indices, counter-clockwise seen from outside
  // For a swept cutter: the outline point each vertex was built from (-1 if none). The caller places
  // the vertex as that point's exact world position plus its offset, so a vertex with no offset lands
  // exactly on the solid's own vertex instead of within rounding of it.
  uint32_t add(double x, double y, double z) {
    v.push_back(x);
    v.push_back(y);
    v.push_back(z);
    return static_cast<uint32_t>(v.size() / 3 - 1);
  }
  void tri(uint32_t a, uint32_t b, uint32_t c) {
    t.push_back(a);
    t.push_back(b);
    t.push_back(c);
  }
  std::vector<int32_t> owner;
};

// The cutter's cross-section. `profile` is the closed outline in (distance into the face, height)
// that a strip sweeps; `chain` is the same inner boundary as a (radius, height) chain that a dome
// revolves, ordered from the outermost radius down to the innermost.
struct Shape {
  std::vector<std::array<double, 2>> profile;
  std::vector<std::array<double, 2>> chain;
  double reach = 0;  // how far into the face the cutter bites at its widest
  double deep = 0;   // how far below the face plane it goes
};

inline Shape crossSection(double r, int n, double over, double bite, bool below) {
  n = n < 1 ? 1 : n;
  bite = bite > r * 0.25 ? r * 0.25 : bite;
  // Trim the quarter arc by this angle at each end: the trimmed ends become short faces that cross
  // the top face and the wall instead of touching them.
  const double trim = std::acos(1.0 - bite / r);
  const double phi0 = kPi - trim;          // wall end
  const double phi1 = kPi * 0.5 + trim;    // face end
  const double faceD = r + r * std::cos(phi1);
  const double bottom = -r - over;

  Shape shape;
  shape.reach = faceD;
  shape.deep = -bottom;
  shape.profile.push_back({-over, over});
  shape.profile.push_back({-over, bottom});
  shape.profile.push_back({bite, bottom});
  for (int k = 0; k <= n; ++k) {
    const double phi = phi0 + (phi1 - phi0) * static_cast<double>(k) / n;
    shape.profile.push_back({r + r * std::cos(phi), -r + r * std::sin(phi)});
  }
  shape.profile.push_back({faceD, over});

  shape.chain.push_back({faceD, over});
  for (int k = n; k >= 0; --k) {
    const double phi = phi0 + (phi1 - phi0) * static_cast<double>(k) / n;
    shape.chain.push_back({r + r * std::cos(phi), -r + r * std::sin(phi)});
  }
  shape.chain.push_back({bite, bottom});

  if (!below) {
    // Mirror in z, then reverse to stay counter-clockwise, keeping the outer-top corner first:
    // the end caps are fans from that vertex, which is only valid from there.
    for (auto& p : shape.profile) p[1] = -p[1];
    std::vector<std::array<double, 2>> flipped;
    flipped.push_back(shape.profile[0]);
    for (size_t i = shape.profile.size() - 1; i >= 1; --i) flipped.push_back(shape.profile[i]);
    shape.profile = std::move(flipped);
    for (auto& p : shape.chain) p[1] = -p[1];
  }
  return shape;
}

inline double signedArea(const std::vector<Vec2>& ring) {
  double a = 0;
  const size_t n = ring.size();
  for (size_t i = 0; i < n; ++i) {
    const Vec2& p = ring[i];
    const Vec2& q = ring[(i + 1) % n];
    a += p.x * q.y - q.x * p.y;
  }
  return a * 0.5;
}

// Drop repeated points and points that sit on the line between their neighbours (T-junctions left
// by triangulation). Keeps the piece count down on long flat edges.
inline std::vector<Vec2> cleanRing(const std::vector<Vec2>& in, double mergeTol = 1e-4, double sinTol = 2e-4) {
  std::vector<Vec2> out;
  for (const Vec2& p : in) {
    if (!out.empty() && std::hypot(p.x - out.back().x, p.y - out.back().y) < mergeTol) continue;
    out.push_back(p);
  }
  while (out.size() > 1 && std::hypot(out.front().x - out.back().x, out.front().y - out.back().y) < mergeTol) out.pop_back();
  bool changed = true;
  while (changed && out.size() > 3) {
    changed = false;
    for (size_t i = 0; i < out.size(); ++i) {
      const Vec2& a = out[(i + out.size() - 1) % out.size()];
      const Vec2& b = out[i];
      const Vec2& c = out[(i + 1) % out.size()];
      const double ux = b.x - a.x, uy = b.y - a.y, wx = c.x - b.x, wy = c.y - b.y;
      const double lu = std::hypot(ux, uy), lw = std::hypot(wx, wy);
      if (lu < mergeTol || lw < mergeTol) continue;
      const double cross = (ux * wy - uy * wx) / (lu * lw);
      const double dot = (ux * wx + uy * wy) / (lu * lw);
      if (std::abs(cross) < sinTol && dot > 0) {
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
        break;
      }
    }
  }
  return out;
}

// cleanRing, carrying one value per edge (edge i runs from point i to point i+1). Merged edges
// average their values, weighted by length; zero-length edges are dropped with their point.
// Points closer than mergeTol are one point. Short edges that survive this (fragments a boolean leaves
// next to a sharp tip) are handled by the sweep itself, which lets the offset swallow them.
inline void cleanRingWithEdges(std::vector<Vec2>& ring, std::vector<Vec2>& edge, std::vector<size_t>* keep = nullptr,
                               double mergeTol = 1e-4, double sinTol = 2e-4) {
  if (edge.size() != ring.size()) {
    // No per-edge data: still go through the loop below so `keep` stays in step with the points.
    edge.assign(ring.size(), Vec2{-1.0, 0.0});
    cleanRingWithEdges(ring, edge, keep, mergeTol, sinTol);
    edge.clear();
    return;
  }
  std::vector<double> len(ring.size());
  for (size_t i = 0; i < ring.size(); ++i) {
    const Vec2& a = ring[i];
    const Vec2& b = ring[(i + 1) % ring.size()];
    len[i] = std::hypot(b.x - a.x, b.y - a.y);
  }
  auto mergeInto = [&](size_t keep, size_t gone) {
    const double w = len[keep] + len[gone];
    if (w > 1e-12) {
      edge[keep] = {(edge[keep].x * len[keep] + edge[gone].x * len[gone]) / w,
                    (edge[keep].y * len[keep] + edge[gone].y * len[gone]) / w};
    }
    len[keep] = w;
  };
  bool changed = true;
  while (changed && ring.size() > 3) {
    changed = false;
    for (size_t i = 0; i < ring.size() && ring.size() > 3; ++i) {
      const size_t n = ring.size();
      const size_t prev = (i + n - 1) % n;
      const Vec2& a = ring[prev];
      const Vec2& b = ring[i];
      const Vec2& c = ring[(i + 1) % n];
      const double ux = b.x - a.x, uy = b.y - a.y, wx = c.x - b.x, wy = c.y - b.y;
      const double lu = std::hypot(ux, uy), lw = std::hypot(wx, wy);
      bool drop = lu < mergeTol;
      if (!drop && lw >= mergeTol) {
        const double cross = (ux * wy - uy * wx) / (lu * lw);
        const double dot = (ux * wx + uy * wy) / (lu * lw);
        drop = std::abs(cross) < sinTol && dot > 0;
      }
      if (!drop) continue;
      // point i goes; edge prev (a->b) absorbs edge i (b->c)
      mergeInto(prev, i);
      ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
      edge.erase(edge.begin() + static_cast<std::ptrdiff_t>(i));
      len.erase(len.begin() + static_cast<std::ptrdiff_t>(i));
      if (keep && keep->size() > i) keep->erase(keep->begin() + static_cast<std::ptrdiff_t>(i));
      changed = true;
      break;
    }
  }
}

// Signed turn at b, in radians. Positive turns toward the face (left), negative away (reflex).
inline double turnAngle(const Vec2& a, const Vec2& b, const Vec2& c) {
  const double ux = b.x - a.x, uy = b.y - a.y, wx = c.x - b.x, wy = c.y - b.y;
  const double lu = std::hypot(ux, uy), lw = std::hypot(wx, wy);
  if (lu < 1e-12 || lw < 1e-12) return 0;
  return std::atan2((ux * wy - uy * wx) / (lu * lw), (ux * wx + uy * wy) / (lu * lw));
}

// How far a reflex corner's wedge runs, between the two strips that meet there. Closing it by
// running both strips on to the bisector costs no extra piece but overcuts by this same amount, so
// the number below is the budget either way: under it, extend; over it, the corner earns a dome.
// Leaving the wedge open is never right — the ridge it leaves stands up as a hairline sliver.
constexpr double kCornerTol = 0.02;

inline double cornerGap(double turn, double radius) {
  if (turn >= 0) return 0;
  return radius * (1.0 - std::cos(std::min(-turn, kPi) * 0.5));
}

inline bool needsDome(double turn, double radius) { return cornerGap(turn, radius) > kCornerTol; }

// How far a strip runs past a corner, measured along its own edge.
inline double cornerExtend(double turn, double radius, double over) {
  if (turn >= 0 || needsDome(turn, radius)) return over;
  return std::max(over, radius * std::tan(-turn * 0.5));
}

inline RawMesh stripMesh(const Vec2& a, const Vec2& b, const Shape& shape, double extA, double extB) {
  RawMesh mesh;
  const double dx = b.x - a.x, dy = b.y - a.y;
  const double len = std::hypot(dx, dy);
  if (len < 1e-9) return mesh;
  const double tx = dx / len, ty = dy / len;
  const double lx = -ty, ly = tx;
  const auto& prof = shape.profile;
  const uint32_t m = static_cast<uint32_t>(prof.size());
  if (m < 3) return mesh;
  for (int end = 0; end < 2; ++end) {
    const double s = end ? len + extB : -extA;
    for (uint32_t k = 0; k < m; ++k)
      mesh.add(a.x + tx * s + lx * prof[k][0], a.y + ty * s + ly * prof[k][0], prof[k][1]);
  }
  for (uint32_t k = 0; k < m; ++k) {
    const uint32_t k2 = (k + 1) % m;
    mesh.tri(k, k2, m + k2);
    mesh.tri(k, m + k2, m + k);
  }
  for (uint32_t j = 1; j + 1 < m; ++j) {
    mesh.tri(m, m + j, m + j + 1);  // far cap, faces +s
    mesh.tri(0, j + 1, j);          // near cap, faces -s
  }
  return mesh;
}

// The cross-section's inner boundary revolved about a vertical axis through (cx, cy).
inline RawMesh domeMesh(double cx, double cy, const Shape& shape, int segments) {
  RawMesh mesh;
  segments = segments < 6 ? 6 : segments;
  const auto& chain = shape.chain;
  if (chain.size() < 3) return mesh;
  const bool below = chain.front()[1] > chain.back()[1];
  const uint32_t top = mesh.add(cx, cy, chain.front()[1]);
  const uint32_t bottom = mesh.add(cx, cy, chain.back()[1]);
  const uint32_t rows = static_cast<uint32_t>(chain.size());
  const uint32_t K = static_cast<uint32_t>(segments);
  for (uint32_t j = 0; j < rows; ++j)
    for (uint32_t k = 0; k < K; ++k) {
      const double phi = 2.0 * kPi * k / K;
      mesh.add(cx + chain[j][0] * std::cos(phi), cy + chain[j][0] * std::sin(phi), chain[j][1]);
    }
  auto at = [&](uint32_t j, uint32_t k) { return 2 + j * K + (k % K); };
  std::vector<uint32_t> tris;
  for (uint32_t k = 0; k < K; ++k) {
    tris.insert(tris.end(), {top, at(0, k), at(0, k + 1)});
    for (uint32_t j = 0; j + 1 < rows; ++j) {
      tris.insert(tris.end(), {at(j, k), at(j + 1, k), at(j, k + 1)});
      tris.insert(tris.end(), {at(j + 1, k), at(j + 1, k + 1), at(j, k + 1)});
    }
    tris.insert(tris.end(), {at(rows - 1, k), bottom, at(rows - 1, k + 1)});
  }
  for (size_t i = 0; i < tris.size(); i += 3) {
    if (below)
      mesh.tri(tris[i], tris[i + 1], tris[i + 2]);
    else
      mesh.tri(tris[i], tris[i + 2], tris[i + 1]);  // mirrored in z, so flip the winding
  }
  return mesh;
}


// ---------------------------------------------------------------------------------------------
// One continuous sweep per outline.
//
// Unioning a strip per edge (above) leaves every pair of neighbours overlapping, and wherever two
// nearly coincident surfaces meet the boolean resolves them into slivers: the dotted seams across
// the fillet band. Here the whole outline is ONE closed tube instead. Every curve of the cross-
// section is the outline offset into the face by that curve's distance, all curves share the same
// stations, and neighbouring curves are stitched quad by quad. The arc bands run unbroken around
// the outline, exactly like a revolved surface, and no boolean is needed to build the cutter.
//
// Corners: an offset INTO the face is exact with a plain mitre where the outline turns toward the
// face. Where it turns away, the true offset is an arc around the corner; a shallow turn keeps the
// mitre (it overcuts by less than kSweepTol), a sharp one gets a fan of stations rotated about
// the corner, the same faceting a revolve would give. The outer wall (offset AWAY from the face, into
// air) is always a mitre. A fan meets a single station by collapsing to it, so station counts may
// differ between curves at a corner; the stitcher handles that without degenerate triangles.
//
// Cross-section, as (distance into the face, height), the profile: the quarter arc runs from
// the wall at (0, -r) to the face at (r, 0) and meets both exactly; the cutter also reaches `over`
// out past the wall and `over` up past the face. Nothing bites into the solid.
// ---------------------------------------------------------------------------------------------

constexpr double kSweepTol = 0.01;   // mm a mitre may overcut at a shallow reflex turn
constexpr double kSharpTurn = 0.35;  // radians (20 degrees): sharper than this is a corner, not a curve

inline bool segmentsCross(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d) {
  auto orient = [](const Vec2& p, const Vec2& q, const Vec2& r) {
    return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
  };
  const double o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
  return ((o1 > 0) != (o2 > 0)) && ((o3 > 0) != (o4 > 0)) && o1 != 0 && o2 != 0 && o3 != 0 && o4 != 0;
}

// Builds the tube for one outline (face on the left, see cleanRing/signedArea). Returns false, with
// a reason, when the outline is too tight for this radius and the offset would fold over itself;
// the caller then falls back to the piecewise builder above.
// `walls`, when given, holds one entry per outline edge: the solid's wall below that edge as its
// outward normal in the edge's cross-section, (into the face, up). A vertical wall is (-1, 0). The
// arc is then built tangent to the real wall instead of an assumed vertical one, and the overhang
// is measured off the real wall, so a drafted pocket is neither ledged nor left with a crease.
inline bool sweepRing(const std::vector<Vec2>& ring, double r, int n, double over, bool below, RawMesh* out,
                      const char** why, const std::vector<Vec2>* walls = nullptr) {
  const size_t N = ring.size();
  if (N < 3) { if (why) *why = "outline too small"; return false; }
  n = n < 1 ? 1 : n;

  // Per-corner frame: incoming/outgoing unit tangents and their left (into-face) normals.
  std::vector<Vec2> tIn(N), tOut(N), lIn(N), lOut(N), miter(N);
  std::vector<double> turn(N);
  std::vector<int> stations(N, 1);
  for (size_t i = 0; i < N; ++i) {
    const Vec2& a = ring[(i + N - 1) % N];
    const Vec2& b = ring[i];
    const Vec2& c = ring[(i + 1) % N];
    const double l1 = std::hypot(b.x - a.x, b.y - a.y), l2 = std::hypot(c.x - b.x, c.y - b.y);
    if (l1 < 1e-9 || l2 < 1e-9) { if (why) *why = "outline has a zero-length edge"; return false; }
    tIn[i] = {(b.x - a.x) / l1, (b.y - a.y) / l1};
    tOut[i] = {(c.x - b.x) / l2, (c.y - b.y) / l2};
    lIn[i] = {-tIn[i].y, tIn[i].x};
    lOut[i] = {-tOut[i].y, tOut[i].x};
    turn[i] = turnAngle(a, b, c);
    const double denom = 1.0 + lIn[i].x * lOut[i].x + lIn[i].y * lOut[i].y;
    const bool reflex = turn[i] < 0;
    // A mitre is exact where the outline turns toward the face. Turning away, it pokes past the
    // true rounded offset by r(1/cos(turn/2) - 1); round the corner once that would show.
    const double miterOvercut = denom > 1e-9 ? r * (std::sqrt(2.0 / denom) - 1.0) : 1e300;
    if (reflex && miterOvercut > kSweepTol) {
      stations[i] = std::max(2, static_cast<int>(std::ceil(-turn[i] / (kPi / 32.0))) + 1);
    } else if (denom < 1e-6) {
      if (why) *why = "outline doubles back on itself";
      return false;
    }
    miter[i] = denom > 1e-9 ? Vec2{(lIn[i].x + lOut[i].x) / denom, (lIn[i].y + lOut[i].y) / denom} : Vec2{0, 0};
  }

  // Wall direction per edge, in (into the face, up). Unknown or implausible walls count as vertical.
  // A wall leaning more than 45 degrees either way is not a wall this fillet should follow.
  std::vector<Vec2> W(N, Vec2{-1.0, 0.0});
  if (below && walls && walls->size() == N) {
    for (size_t e = 0; e < N; ++e) {
      Vec2 w = (*walls)[e];
      const double len = std::hypot(w.x, w.y);
      if (len < 1e-9) continue;
      w = {w.x / len, w.y / len};
      if (w.x < -std::cos(kPi / 4.0) + 1e-9) W[e] = w;
    }
  }
  // The profile for one wall direction, as (into the face, height). The arc's centre sits r below the
  // face and r inside the wall; it meets the wall at B and the face at A, tangent to both.
  struct Prof { double cd; Vec2 B; double phiB; };
  auto profileFor = [&](const Vec2& w) {
    Prof p;
    p.cd = r * (w.y - 1.0) / w.x;
    p.B = {p.cd + r * w.x, -r + r * w.y};
    p.phiB = std::atan2(w.y, w.x);
    if (p.phiB < 0) p.phiB += 2.0 * kPi;   // keep it on the wall side, (90, 270) degrees, so the arc runs the short way
    return p;
  };
  // Where the outward offset of wall `w` (by `over`) sits at height z.
  auto outsideAt = [&](const Vec2& w, double z) { return (over - w.y * z) / w.x; };

  // One point of the cross-section for wall `w`: c = 0 outer top, 1 outer bottom, 2..n+2 the arc
  // from the wall (B) to the face (A), C-1 straight up through the face.
  // The arc meets the wall at B and the face at A exactly, tangent to both.
  const size_t C = 2 + static_cast<size_t>(n + 1) + 1;
  auto section = [&](size_t c, const Vec2& w) -> Vec2 {
    const Prof p = profileFor(w);
    if (c == 0) return {outsideAt(w, over), over};
    if (c == 1) return {outsideAt(w, p.B.y), p.B.y};
    if (c == C - 1) return {p.cd, over};
    const int k = static_cast<int>(c) - 2;
    if (k == 0) return p.B;
    if (k == n) return {p.cd, 0.0};
    const double phi = p.phiB + (kPi * 0.5 - p.phiB) * static_cast<double>(k) / n;
    return {p.cd + r * std::cos(phi), -r + r * std::sin(phi)};
  };
  auto blend = [](const Vec2& a, const Vec2& b, double t) {
    Vec2 w{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    const double l = std::hypot(w.x, w.y);
    return l > 1e-12 ? Vec2{w.x / l, w.y / l} : Vec2{-1.0, 0.0};
  };

  // Horizontal position of wall `w` at height z, measured into the face from the rim.
  auto wallAt = [](const Vec2& w, double z) { return -(w.y / w.x) * z; };

  struct Station { Vec2 q; double z; };
  std::vector<std::vector<std::vector<Station>>> pts(C, std::vector<std::vector<Station>>(N));
  std::vector<std::vector<char>> isFan(C, std::vector<char>(N, 0));   // corner rounded by a fan of stations
  for (size_t c = 0; c < C; ++c) {
    for (size_t i = 0; i < N; ++i) {
      const Vec2& p = ring[i];
      const Vec2& wIn = W[(i + N - 1) % N];
      const Vec2& wOut = W[i];
      const double det = lIn[i].x * lOut[i].y - lIn[i].y * lOut[i].x;
      // The point at distance dIn along lIn and dOut along lOut from the rim corner.
      auto cross = [&](double dIn, double dOut) -> Vec2 {
        if (std::abs(det) > 1e-6)
          return {p.x + (dIn * lOut[i].y - dOut * lIn[i].y) / det, p.y + (lIn[i].x * dOut - lOut[i].x * dIn) / det};
        const double d = 0.5 * (dIn + dOut);
        return {p.x + d * miter[i].x, p.y + d * miter[i].y};
      };
      auto emit = [&](const Vec2& q, double z) { pts[c][i].push_back({q, below ? z : -z}); };
      const Vec2 sIn = section(c, wIn), sOut = section(c, wOut);
      const double hIn = sIn.x - wallAt(wIn, sIn.y);      // how far inside the material, from its own wall
      const double hOut = sOut.x - wallAt(wOut, sOut.y);
      if (stations[i] > 1 && hIn > 1e-9 && hOut > 1e-9) {
        isFan[c][i] = 1;
        const double a0 = std::atan2(lIn[i].y, lIn[i].x);
        for (int j = 0; j < stations[i]; ++j) {
          const double t = static_cast<double>(j) / (stations[i] - 1);
          const Vec2 wj = (j == 0) ? wIn : (j == stations[i] - 1) ? wOut : blend(wIn, wOut, t);
          const Vec2 sj = (j == 0) ? sIn : (j == stations[i] - 1) ? sOut : section(c, wj);
          const double hj = (j == 0) ? hIn : (j == stations[i] - 1) ? hOut : sj.x - wallAt(wj, sj.y);
          const Vec2 k = cross(wallAt(wIn, sj.y), wallAt(wOut, sj.y));   // the pocket's corner edge at this height
          const double a = a0 + turn[i] * t;
          emit({k.x + hj * std::cos(a), k.y + hj * std::sin(a)}, sj.y);
        }
      } else if (std::abs(turn[i]) > kSharpTurn && (std::abs(sIn.y - sOut.y) > 1e-9 || std::abs(hIn - hOut) > 1e-9)) {
        // A real corner between two different walls: each edge keeps its own point, held the same
        // distance off the other wall at the same height (a short step that runs along the corner).
        emit(cross(sIn.x, wallAt(wOut, sIn.y) + hIn), sIn.y);
        emit(cross(wallAt(wIn, sOut.y) + hOut, sOut.x), sOut.y);
      } else {
        // Along a smooth outline neighbouring walls differ only slightly (a cone, a scaled hole), and
        // a step at every vertex would stripe the fillet with slivers. One point at the shared height,
        // the right distance off both walls.
        const double z = 0.5 * (sIn.y + sOut.y);
        emit(cross(wallAt(wIn, z) + hIn, wallAt(wOut, z) + hOut), z);
      }
    }
  }

  // Consumed edges. Offsetting an outline swallows any edge shorter than the offset pushes back at its
  // ends: the station after it lands behind the station before it. That happens at a box corner next
  // to a short edge a union left there, and beside sharp tips where two arcs cross between
  // tessellation points. A true offset drops the swallowed edge and joins its neighbours, so the
  // station behind is merged into the one before it (they share a vertex) and placed where the offset
  // lines of the two surviving edges cross. Next to a rounded corner (a fan) there is no line to cross
  // with; there only fragments far shorter than the radius are swallowed, by merging in place, which
  // moves the fillet by microns. The wall-contact curve has no offset on a square wall and so never
  // folds, which keeps every contact with the wall and the face exact. A fold beyond these cases means
  // the radius really is too big for this outline.
  const double maxFanConsumed = std::max(0.05, 0.1 * r);
  std::vector<std::vector<std::vector<std::pair<size_t, size_t>>>> alias(C, std::vector<std::vector<std::pair<size_t, size_t>>>(N));
  for (size_t c = 0; c < C; ++c) {
    for (size_t i = 0; i < N; ++i) {
      alias[c][i].resize(pts[c][i].size());
      for (size_t j = 0; j < pts[c][i].size(); ++j) alias[c][i][j] = {i, j};
    }
    auto root = [&](std::pair<size_t, size_t> at) {
      for (int guard = 0; guard < 4096 && alias[c][at.first][at.second] != at; ++guard) at = alias[c][at.first][at.second];
      return at;
    };
    std::map<std::pair<size_t, size_t>, size_t> spanEnd;   // root -> last corner merged into it
    auto endOf = [&](std::pair<size_t, size_t> at) {
      const auto it = spanEnd.find(at);
      return it == spanEnd.end() ? at.first : it->second;
    };
    size_t merged = 0;
    for (size_t pass = 0; pass < N + 2; ++pass) {
      bool changed = false;
      for (size_t i = 0; i < N; ++i) {
        const size_t j = (i + 1) % N;
        const auto from = root({i, pts[c][i].size() - 1});
        const auto to = root({j, 0});
        if (from == to) continue;
        const Vec2& qa = pts[c][from.first][from.second].q;
        const Vec2& qb = pts[c][to.first][to.second].q;
        if ((qb.x - qa.x) * tOut[i].x + (qb.y - qa.y) * tOut[i].y > 1e-9) continue;
        const double edge = std::hypot(ring[j].x - ring[i].x, ring[j].y - ring[i].y);
        // A corner with two stations that is not a fan is a step between two different walls; it is
        // mitred like any other corner, so the line rule below applies to it.
        const bool fanBefore = isFan[c][from.first] != 0;
        const bool fanAfter = isFan[c][to.first] != 0;
        auto refuse = [&](const char* what) {
          (void)what;
          if (why) *why = "the radius is too large for this outline";
          return false;
        };
        if (to.first == from.first || merged >= N / 2) return refuse("structure");
        Station joined = pts[c][from.first][from.second];
        auto lerp = [](const Station& a, const Station& b, double fa, double fb) {
          const double t = fa / (fa - fb);
          return Station{{a.q.x + (b.q.x - a.q.x) * t, a.q.y + (b.q.y - a.q.y) * t}, a.z + (b.z - a.z) * t};
        };
        if (fanBefore && fanAfter) {
          if (edge > maxFanConsumed) return refuse("between two rounded corners");
        } else if (fanBefore) {
          // The rounded corner's arc runs on until it meets the offset line of the edge after the
          // swallowed one; the stations beyond that point are swallowed too.
          const size_t f = from.first, last = endOf(to);
          const Vec2 nOut = lOut[last];
          const double dOut = section(c, W[last]).x;
          auto F = [&](const Station& st) { return (st.q.x - ring[last].x) * nOut.x + (st.q.y - ring[last].y) * nOut.y - dOut; };
          auto& fan = pts[c][f];
          size_t k = 0;
          while (k < fan.size() && root({f, k}) == std::make_pair(f, k) && F(fan[k]) >= 0) ++k;
          if (k == 0 || k == fan.size() || root({f, k}) != std::make_pair(f, k)) {
            if (edge > maxFanConsumed) return refuse("beside a rounded corner");
          } else {
            const Station x = lerp(fan[k - 1], fan[k], F(fan[k - 1]), F(fan[k]));
            fan[k] = x;
            for (size_t m = k + 1; m < fan.size(); ++m) { alias[c][f][m] = {f, k}; fan[m] = x; }
            spanEnd[{f, k}] = endOf(to);
            spanEnd.erase(to);
            alias[c][to.first][to.second] = {f, k};
            pts[c][to.first][to.second] = x;
            changed = true;
            ++merged;
            continue;
          }
        } else if (fanAfter) {
          // Mirror case: the arc of the rounded corner after the swallowed edge starts too close to
          // the edge before it; its stations up to where it meets that edge's offset line are swallowed.
          const size_t g = to.first, first = from.first;
          const size_t eIn = (first + N - 1) % N;
          const Vec2 nIn = lIn[first];
          const double dIn = section(c, W[eIn]).x;
          auto G = [&](const Station& st) { return (st.q.x - ring[first].x) * nIn.x + (st.q.y - ring[first].y) * nIn.y - dIn; };
          auto& fan = pts[c][g];
          size_t k = 0;
          while (k < fan.size() && root({g, k}) == std::make_pair(g, k) && G(fan[k]) < 0) ++k;
          if (k == 0 || k == fan.size() || root({g, k}) != std::make_pair(g, k)) {
            if (edge > maxFanConsumed) return refuse("beside a rounded corner");
          } else {
            const Station x = lerp(fan[k - 1], fan[k], G(fan[k - 1]), G(fan[k]));
            pts[c][from.first][from.second] = x;
            for (size_t m = 0; m < k; ++m) { alias[c][g][m] = from; fan[m] = x; }
            spanEnd[from] = g;
            changed = true;
            ++merged;
            continue;
          }
        } else {
          // Cross the offset lines of the edge into the span and the edge out of it.
          const size_t first = from.first, last = endOf(to);
          const size_t eIn = (first + N - 1) % N, eOut = last;
          const double dIn = section(c, W[eIn]).x;
          const double dOut = section(c, W[eOut]).x;
          const Vec2 nIn = lIn[first], nOut = lOut[last];
          const double cr = nIn.x * nOut.y - nIn.y * nOut.x;
          if (std::abs(cr) < 1e-9) return refuse("parallel neighbours");
          const double r1 = dIn + ring[first].x * nIn.x + ring[first].y * nIn.y;
          const double r2 = dOut + ring[last].x * nOut.x + ring[last].y * nOut.y;
          joined.q = {(r1 * nOut.y - r2 * nIn.y) / cr, (nIn.x * r2 - nOut.x * r1) / cr};
          joined.z = 0.5 * (pts[c][from.first][from.second].z + pts[c][to.first][to.second].z);
        }
        spanEnd[from] = endOf(to);
        spanEnd.erase(to);
        alias[c][to.first][to.second] = from;
        pts[c][from.first][from.second] = joined;
        pts[c][to.first][to.second] = joined;
        changed = true;
        ++merged;
      }
      if (!changed) break;
    }
    for (size_t i = 0; i < N; ++i)
      for (size_t j = 0; j < pts[c][i].size(); ++j) {
        alias[c][i][j] = root({i, j});
        pts[c][i][j] = pts[c][alias[c][i][j].first][alias[c][i][j].second];
      }
  }

  RawMesh mesh;
  // groups[c][i] = vertex indices of curve c at corner i, in walking order.
  //
  // Every edge's two end stations use that edge's own wall, so each edge carries its exact profile
  // end to end. At a corner, a drafted pocket's corner edge is not straight under the rim corner: it
  // slides with depth, to where the two real walls meet at that height (K above). Points inside the
  // material round around that line (the rolling ball around the pocket's corner); points on or
  // outside the walls take the crossing of both walls' offsets at their own height.
  std::vector<std::vector<std::vector<uint32_t>>> groups(C, std::vector<std::vector<uint32_t>>(N));
  std::vector<std::vector<Vec2>> flat(C);   // the curve in plan, for the fold checks
  for (size_t c = 0; c < C; ++c) {
    std::vector<std::vector<uint32_t>> index(N);
    for (size_t i = 0; i < N; ++i)
      for (size_t j = 0; j < pts[c][i].size(); ++j) {
        if (alias[c][i][j] != std::make_pair(i, j)) { index[i].push_back(UINT32_MAX); continue; }
        index[i].push_back(mesh.add(pts[c][i][j].q.x, pts[c][i][j].q.y, pts[c][i][j].z));
        mesh.owner.push_back(static_cast<int32_t>(i));
      }
    for (size_t i = 0; i < N; ++i)
      for (size_t j = 0; j < pts[c][i].size(); ++j) {
        const auto at = alias[c][i][j];
        const uint32_t v = index[at.first][at.second];
        if (!groups[c][i].empty() && groups[c][i].back() == v) continue;
        groups[c][i].push_back(v);
        if (flat[c].empty() || flat[c].back().x != pts[c][i][j].q.x || flat[c].back().y != pts[c][i][j].q.y)
          flat[c].push_back(pts[c][i][j].q);
      }
  }

  // Fold checks. Each offset edge must still run the way its outline edge does, or the offset has
  // collapsed it; and the deepest inner curve and the outer wall must not cross themselves.
  for (size_t c = 0; c < C; ++c) {
    for (size_t i = 0; i < N; ++i) {
      const size_t j = (i + 1) % N;
      const uint32_t from = groups[c][i].back(), to = groups[c][j].front();
      if (from == to) continue;   // a consumed edge
      const double dx = mesh.v[to * 3] - mesh.v[from * 3], dy = mesh.v[to * 3 + 1] - mesh.v[from * 3 + 1];
      if (dx * tOut[i].x + dy * tOut[i].y <= 1e-9) {
        if (why) *why = "the radius is too large for this outline";
        return false;
      }
    }
  }
  for (size_t c : {size_t{0}, C - 1}) {
    const auto& f = flat[c];
    const size_t M = f.size();
    for (size_t a = 0; a < M; ++a)
      for (size_t b = a + 2; b < M; ++b) {
        if (a == 0 && b == M - 1) continue;
        if (segmentsCross(f[a], f[(a + 1) % M], f[b], f[(b + 1) % M])) {
          if (why) *why = "the offset outline crosses itself";
          return false;
        }
      }
  }

  // Stitch each curve to the next, corner by corner, then the straight run to the next corner.
  auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
    if (a == b || b == c || a == c) return;
    mesh.tri(a, b, c);
  };
  auto quad = [&](uint32_t a0, uint32_t a1, uint32_t b1, uint32_t b0) {
    tri(a0, a1, b1);
    tri(a0, b1, b0);
  };
  for (size_t c = 0; c < C; ++c) {
    const auto& A = groups[c];
    const auto& B = groups[(c + 1) % C];
    for (size_t i = 0; i < N; ++i) {
      const auto& ga = A[i];
      const auto& gb = B[i];
      const size_t m = std::max(ga.size(), gb.size());
      for (size_t k = 0; k + 1 < m; ++k) {
        const uint32_t a0 = ga[std::min(k, ga.size() - 1)], a1 = ga[std::min(k + 1, ga.size() - 1)];
        const uint32_t b0 = gb[std::min(k, gb.size() - 1)], b1 = gb[std::min(k + 1, gb.size() - 1)];
        quad(a0, a1, b1, b0);
      }
      const size_t j = (i + 1) % N;
      quad(ga.back(), A[j].front(), B[j].front(), gb.back());
    }
  }

  // Normalise the winding so the tube faces out.
  double vol = 0;
  for (size_t t = 0; t + 2 < mesh.t.size(); t += 3) {
    const double* a = &mesh.v[mesh.t[t] * 3];
    const double* b = &mesh.v[mesh.t[t + 1] * 3];
    const double* c = &mesh.v[mesh.t[t + 2] * 3];
    vol += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
  }
  if (vol < 0)
    for (size_t t = 0; t + 2 < mesh.t.size(); t += 3) std::swap(mesh.t[t + 1], mesh.t[t + 2]);
  *out = std::move(mesh);
  return true;
}

}  // namespace fg
