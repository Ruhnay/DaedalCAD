#include "edgefillet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <numeric>
#include <unordered_map>

using manifold::Manifold;
using manifold::MeshGL64;
using manifold::vec3;

namespace ef {
namespace {

constexpr double kPi = 3.14159265358979323846;

vec3 add(const vec3& a, const vec3& b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
vec3 sub(const vec3& a, const vec3& b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
vec3 mul(const vec3& a, double s) { return vec3(a.x * s, a.y * s, a.z * s); }
double dot(const vec3& a, const vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
vec3 cross(const vec3& a, const vec3& b) { return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
double len(const vec3& a) { return std::sqrt(dot(a, a)); }
vec3 unit(const vec3& a, const vec3& fallback = vec3(0, 0, 0)) {
  const double l = len(a);
  return l < 1e-15 ? fallback : mul(a, 1.0 / l);
}
vec3 at(const std::vector<double>& v, size_t i) { return vec3(v[i * 3], v[i * 3 + 1], v[i * 3 + 2]); }
void push(std::vector<double>& v, const vec3& p) {
  v.push_back(p.x);
  v.push_back(p.y);
  v.push_back(p.z);
}
// Rodrigues: p rotated by `angle` about the line through `origin` along unit `axis`.
vec3 rotateAbout(const vec3& p, const vec3& origin, const vec3& axis, double angle) {
  const vec3 d = sub(p, origin);
  const double c = std::cos(angle), s = std::sin(angle);
  const vec3 r = add(add(mul(d, c), mul(cross(axis, d), s)), mul(axis, dot(axis, d) * (1 - c)));
  return add(origin, r);
}

void say(std::string* log, const std::string& text) {
  if (!log) return;
  log->append(text);
  log->push_back('\n');
}
void warn(std::string* warning, const std::string& text) {
  if (warning && warning->find(text) == std::string::npos) {
    if (!warning->empty()) warning->push_back(' ');
    warning->append(text);
  }
}

bool good(const Manifold& m) { return !m.IsEmpty() && m.Status() == Manifold::Error::NoError; }

// The solid as welded vertices, triangles, outward normals, and directed edge -> triangle.
struct Mesh {
  std::vector<vec3> p;
  std::vector<std::array<uint32_t, 3>> t;
  std::vector<vec3> n;
  std::unordered_map<uint64_t, uint32_t> dir;
  static uint64_t key(uint32_t a, uint32_t b) { return (static_cast<uint64_t>(a) << 32) | b; }
  int triOf(uint32_t a, uint32_t b) const {
    const auto it = dir.find(key(a, b));
    return it == dir.end() ? -1 : static_cast<int>(it->second);
  }
};

Mesh readMesh(const Manifold& solid) {
  Mesh mesh;
  const MeshGL64 gl = solid.GetMeshGL64();
  const size_t stride = std::max<size_t>(3, gl.numProp);
  const size_t count = gl.vertProperties.size() / stride;
  std::vector<uint32_t> root(count);
  std::iota(root.begin(), root.end(), 0u);
  auto find = [&](uint32_t x) {
    while (root[x] != x) {
      root[x] = root[root[x]];
      x = root[x];
    }
    return x;
  };
  for (size_t i = 0; i < gl.mergeFromVert.size() && i < gl.mergeToVert.size(); ++i) {
    const uint64_t a = gl.mergeFromVert[i], b = gl.mergeToVert[i];
    if (a >= count || b >= count) continue;
    const uint32_t ra = find(static_cast<uint32_t>(a)), rb = find(static_cast<uint32_t>(b));
    if (ra != rb) root[ra] = rb;
  }
  std::vector<int64_t> compact(count, -1);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t r = find(static_cast<uint32_t>(i));
    if (compact[r] < 0) {
      compact[r] = static_cast<int64_t>(mesh.p.size());
      mesh.p.push_back(vec3(gl.vertProperties[r * stride], gl.vertProperties[r * stride + 1], gl.vertProperties[r * stride + 2]));
    }
    compact[i] = compact[r];
  }
  std::vector<char> bad;
  for (size_t i = 0; i + 2 < gl.triVerts.size(); i += 3) {
    const uint32_t a = static_cast<uint32_t>(compact[gl.triVerts[i]]);
    const uint32_t b = static_cast<uint32_t>(compact[gl.triVerts[i + 1]]);
    const uint32_t c = static_cast<uint32_t>(compact[gl.triVerts[i + 2]]);
    if (a == b || b == c || a == c) continue;
    const uint32_t index = static_cast<uint32_t>(mesh.t.size());
    mesh.t.push_back({a, b, c});
    const vec3 raw = cross(sub(mesh.p[b], mesh.p[a]), sub(mesh.p[c], mesh.p[a]));
    const double longest = std::max({len(sub(mesh.p[b], mesh.p[a])), len(sub(mesh.p[c], mesh.p[b])), len(sub(mesh.p[a], mesh.p[c]))});
    // A sliver (height far below its length) has a normal that is mostly rounding noise.
    const bool sliver = len(raw) < 1e-7 * std::max(1e-6, longest * longest);
    mesh.n.push_back(unit(raw));
    bad.push_back(sliver ? 1 : 0);
    mesh.dir[Mesh::key(a, b)] = index;
    mesh.dir[Mesh::key(b, c)] = index;
    mesh.dir[Mesh::key(c, a)] = index;
  }
  // Slivers take the normal of the face they belong to: the neighbour across their longest edge.
  for (int pass = 0; pass < 6; ++pass) {
    bool changed = false;
    for (size_t i = 0; i < mesh.t.size(); ++i) {
      if (!bad[i]) continue;
      const auto& tri = mesh.t[i];
      int best = -1;
      double bestLen = -1;
      for (int k = 0; k < 3; ++k) {
        const uint32_t a = tri[static_cast<size_t>(k)], b = tri[static_cast<size_t>((k + 1) % 3)];
        const int other = mesh.triOf(b, a);
        if (other < 0 || bad[static_cast<size_t>(other)]) continue;
        const double l = len(sub(mesh.p[b], mesh.p[a]));
        if (l > bestLen) {
          bestLen = l;
          best = other;
        }
      }
      if (best >= 0) {
        mesh.n[i] = mesh.n[static_cast<size_t>(best)];
        bad[i] = 0;
        changed = true;
      }
    }
    if (!changed) break;
  }
  return mesh;
}

// Point in solid, by counting crossings of a ray in an odd direction.
bool inside(const Mesh& mesh, const vec3& q) {
  const vec3 d = unit(vec3(0.5773502, 0.5781234, 0.5765321));
  int hits = 0;
  for (const auto& tri : mesh.t) {
    const vec3& a = mesh.p[tri[0]];
    const vec3 e1 = sub(mesh.p[tri[1]], a), e2 = sub(mesh.p[tri[2]], a);
    const vec3 h = cross(d, e2);
    const double det = dot(e1, h);
    if (std::abs(det) < 1e-14) continue;
    const double f = 1.0 / det;
    const vec3 s = sub(q, a);
    const double u = f * dot(s, h);
    if (u < 0 || u > 1) continue;
    const vec3 qq = cross(s, e1);
    const double v = f * dot(d, qq);
    if (v < 0 || u + v > 1) continue;
    if (f * dot(e2, qq) > 1e-9) ++hits;
  }
  return (hits & 1) == 1;
}

double chainLength(const std::vector<double>& pts) {
  double total = 0;
  for (size_t i = 1; i < pts.size() / 3; ++i) total += len(sub(at(pts, i), at(pts, i - 1)));
  return total;
}

// Evenly spaced points along a polyline.
std::vector<vec3> samples(const std::vector<double>& pts, int wanted) {
  std::vector<vec3> out;
  const size_t n = pts.size() / 3;
  if (n == 0) return out;
  if (n == 1) return {at(pts, 0)};
  const double total = chainLength(pts);
  if (total < 1e-12) return {at(pts, 0)};
  size_t seg = 1;
  double before = 0;
  for (int k = 0; k < wanted; ++k) {
    const double target = total * (static_cast<double>(k) + 0.5) / wanted;
    while (seg < n - 1 && before + len(sub(at(pts, seg), at(pts, seg - 1))) < target) {
      before += len(sub(at(pts, seg), at(pts, seg - 1)));
      ++seg;
    }
    const vec3 a = at(pts, seg - 1), b = at(pts, seg);
    const double l = len(sub(b, a));
    const double t = l < 1e-12 ? 0 : std::clamp((target - before) / l, 0.0, 1.0);
    out.push_back(add(a, mul(sub(b, a), t)));
  }
  return out;
}

double distanceToPolyline(const vec3& q, const std::vector<double>& pts) {
  const size_t n = pts.size() / 3;
  if (n == 0) return 1e300;
  if (n == 1) return len(sub(q, at(pts, 0)));
  double best = 1e300;
  for (size_t i = 1; i < n; ++i) {
    const vec3 a = at(pts, i - 1), b = at(pts, i);
    const vec3 ab = sub(b, a);
    const double l2 = dot(ab, ab);
    const double t = l2 < 1e-24 ? 0 : std::clamp(dot(sub(q, a), ab) / l2, 0.0, 1.0);
    best = std::min(best, len(sub(q, add(a, mul(ab, t)))));
  }
  return best;
}

// A chain's points as an open polyline (a loop repeats its first point at the end).
std::vector<double> polylineOf(const Chain& chain) {
  std::vector<double> out = chain.pts;
  if (chain.closed && chain.pts.size() >= 3) {
    out.push_back(chain.pts[0]);
    out.push_back(chain.pts[1]);
    out.push_back(chain.pts[2]);
  }
  return out;
}

// ---- the cross-section -------------------------------------------------------------------------
// At edge point p with outward face normals nA, nB: the rolling ball of radius r touches both faces
// at tA and tB. An outside edge loses the region between the edge and the arc; an inside edge gets
// it filled. The arc runs a hair past both tangent points, and the section's two other corners sit
// `over` outside (cut) or inside (fill) each face, so the section's walls cross the solid's faces
// cleanly instead of lying in them.
struct Section {
  std::vector<vec3> q;  // closed outline; q[0] is the corner past the edge
  vec3 nub;             // a point well inside the region
  vec3 centre;
};

bool section(const vec3& p, vec3 nA, vec3 nB, bool concave, double r, int segs, double over, Section* out, bool chamfer = false) {
  nA = unit(nA);
  nB = unit(nB);
  const double k = dot(nA, nB);
  if (!(k > -0.9 && k < 0.999)) return false;
  const double phi = std::acos(std::clamp(k, -1.0, 1.0));
  const double s = concave ? 1.0 : -1.0;
  const vec3 bis = mul(add(nA, nB), 1.0 / (1.0 + k));
  const vec3 c = add(p, mul(bis, s * r));
  // The arc runs past each tangent point by `delta`, which dips its end 0.01 um into the face:
  // enough for the booleans to see a clean crossing, small enough to vanish when the result is
  // cleaned up (a bigger dip left a visible micron step along the tangent lines).
  const double delta = std::min(0.05, std::sqrt(2e-5 / std::max(r, 1e-3)));
  // A chamfer is the same section with the arc as one straight cut between its ends.
  const int arcs = chamfer ? 1 : std::max(2, static_cast<int>(std::ceil(segs * phi / (kPi / 2))));
  out->q.clear();
  out->q.push_back(sub(p, mul(bis, s * over)));
  std::vector<vec3> arc;
  for (int i = 0; i <= arcs; ++i) {
    // A chamfer ends exactly on the lines a round would start at, dipped a hair into the solid
    // instead of run on (run on, a straight cut would shift and take too much).
    const double th = chamfer ? phi * i / arcs : -delta + (phi + 2 * delta) * i / arcs;
    const vec3 d = mul(add(mul(nA, std::sin(phi - th)), mul(nB, std::sin(th))), 1.0 / std::sin(phi));
    vec3 a = sub(c, mul(d, s * r));
    if (chamfer) a = sub(a, mul(i == 0 ? nA : nB, 1e-5));
    arc.push_back(a);
  }
  const double hA = dot(sub(arc.front(), p), nA);
  const double hB = dot(sub(arc.back(), p), nB);
  out->q.push_back(add(arc.front(), mul(nA, -s * over - hA)));
  for (const vec3& a : arc) out->q.push_back(a);
  out->q.push_back(add(arc.back(), mul(nB, -s * over - hB)));
  const double reach = len(sub(c, p));
  out->nub = add(p, mul(sub(c, p), 0.5 * (1.0 - r / std::max(reach, 1e-12))));
  out->centre = c;
  return true;
}

// Stitches stations (same point count each) into a closed solid. Open tubes get flat end caps,
// fanned from point 0, which sees the whole section.
Manifold tube(const std::vector<std::vector<vec3>>& st, bool closed) {
  const size_t S = st.size();
  if (S < 2 || st[0].size() < 3) return Manifold();
  const size_t M = st[0].size();
  for (const auto& station : st)
    if (station.size() != M) return Manifold();
  MeshGL64 mesh;
  mesh.numProp = 3;
  for (const auto& station : st)
    for (const vec3& q : station) {
      mesh.vertProperties.push_back(q.x);
      mesh.vertProperties.push_back(q.y);
      mesh.vertProperties.push_back(q.z);
    }
  auto tri = [&](size_t a, size_t b, size_t c) {
    mesh.triVerts.push_back(static_cast<uint64_t>(a));
    mesh.triVerts.push_back(static_cast<uint64_t>(b));
    mesh.triVerts.push_back(static_cast<uint64_t>(c));
  };
  const size_t rings = closed ? S : S - 1;
  for (size_t s = 0; s < rings; ++s) {
    const size_t s2 = (s + 1) % S;
    for (size_t j = 0; j < M; ++j) {
      const size_t j2 = (j + 1) % M;
      const size_t a = s * M + j, b = s * M + j2, c = s2 * M + j2, d = s2 * M + j;
      tri(a, b, c);
      tri(a, c, d);
    }
  }
  if (!closed) {
    for (size_t j = 1; j + 1 < M; ++j) tri(0, j + 1, j);
    const size_t base = (S - 1) * M;
    for (size_t j = 1; j + 1 < M; ++j) tri(base, base + j, base + j + 1);
  }
  Manifold solid(mesh);
  if (!good(solid)) return Manifold();
  if (solid.Volume() < 0) {
    for (size_t i = 0; i + 2 < mesh.triVerts.size(); i += 3) std::swap(mesh.triVerts[i + 1], mesh.triVerts[i + 2]);
    solid = Manifold(mesh);
    if (!good(solid)) return Manifold();
  }
  return solid;
}

// Removes from `piece` what lies behind the plane through p (against n), but only within `reach`
// of p.
Manifold localTrim(const Manifold& piece, const vec3& n, const vec3& p, double reach) {
  const Manifold box = Manifold::Cube(vec3(2 * reach, 2 * reach, 2 * reach), true).Translate(p);
  const vec3 back = mul(unit(n), -1.0);
  const Manifold behind = box.TrimByPlane(back, dot(p, back));
  if (!good(behind)) return piece;
  const Manifold out = piece - behind;
  return good(out) ? out : piece;
}

}  // namespace

// ---- sharp edges -------------------------------------------------------------------------------

std::vector<Chain> findChains(const Manifold& solid, double minAngleDeg) {
  std::vector<Chain> chains;
  if (!good(solid)) return chains;
  const Mesh mesh = readMesh(solid);
  const double cosMin = std::cos(minAngleDeg * kPi / 180.0);

  struct Edge {
    uint32_t a, b;
    bool convex;
  };
  std::vector<Edge> edges;
  for (const auto& entry : mesh.dir) {
    const uint32_t a = static_cast<uint32_t>(entry.first >> 32), b = static_cast<uint32_t>(entry.first & 0xffffffffu);
    if (a > b) continue;
    const int left = static_cast<int>(entry.second);
    const int right = mesh.triOf(b, a);
    if (right < 0) continue;
    const vec3& nl = mesh.n[static_cast<size_t>(left)];
    const vec3& nr = mesh.n[static_cast<size_t>(right)];
    if (dot(nl, nr) > cosMin) continue;
    uint32_t q = mesh.t[static_cast<size_t>(right)][0];
    for (uint32_t v : mesh.t[static_cast<size_t>(right)])
      if (v != a && v != b) q = v;
    edges.push_back({a, b, dot(sub(mesh.p[q], mesh.p[a]), nl) < 0});
  }
  std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });

  std::unordered_map<uint64_t, bool> convexOf;
  for (const Edge& e : edges) convexOf[Mesh::key(e.a, e.b)] = e.convex;
  std::unordered_map<uint32_t, std::vector<int>> around;
  for (size_t i = 0; i < edges.size(); ++i) {
    around[edges[i].a].push_back(static_cast<int>(i));
    around[edges[i].b].push_back(static_cast<int>(i));
  }
  auto other = [&](int e, uint32_t v) { return edges[static_cast<size_t>(e)].a == v ? edges[static_cast<size_t>(e)].b : edges[static_cast<size_t>(e)].a; };
  // Where a chain must stop even with only two sharp edges at the vertex.
  const double cosTurn = std::cos(40.0 * kPi / 180.0);
  auto isBreak = [&](uint32_t v) {
    const auto it = around.find(v);
    if (it == around.end() || it->second.size() != 2) return true;
    const int e1 = it->second[0], e2 = it->second[1];
    if (edges[static_cast<size_t>(e1)].convex != edges[static_cast<size_t>(e2)].convex) return true;
    const vec3 d1 = unit(sub(mesh.p[other(e1, v)], mesh.p[v]));
    const vec3 d2 = unit(sub(mesh.p[other(e2, v)], mesh.p[v]));
    return dot(d1, d2) > -cosTurn;  // straight on is -1
  };

  std::vector<char> used(edges.size(), 0);
  auto finish = [&](std::vector<uint32_t> verts, bool closed) {
    if (verts.size() < 2) return;
    Chain chain;
    chain.closed = closed && verts.size() >= 3;
    const size_t n = verts.size();
    std::vector<vec3> segA, segB;
    const size_t segs = chain.closed ? n : n - 1;
    bool concave = false;
    double angleSum = 0;
    for (size_t i = 0; i < segs; ++i) {
      const uint32_t u = verts[i], w = verts[(i + 1) % n];
      const int ta = mesh.triOf(u, w), tb = mesh.triOf(w, u);
      const vec3 na = ta >= 0 ? mesh.n[static_cast<size_t>(ta)] : vec3(0, 0, 1);
      const vec3 nb = tb >= 0 ? mesh.n[static_cast<size_t>(tb)] : vec3(0, 0, 1);
      segA.push_back(na);
      segB.push_back(nb);
      angleSum += std::acos(std::clamp(dot(na, nb), -1.0, 1.0));
    }
    // Convexity of the chain: every edge in it agrees (isBreak splits where it changes).
    {
      const auto it = convexOf.find(Mesh::key(std::min(verts[0], verts[1]), std::max(verts[0], verts[1])));
      if (it != convexOf.end()) concave = !it->second;
    }
    chain.concave = concave;
    chain.angle = angleSum / static_cast<double>(segs) * 180.0 / kPi;
    for (size_t i = 0; i < n; ++i) {
      push(chain.pts, mesh.p[verts[i]]);
      vec3 a(0, 0, 0), b(0, 0, 0);
      if (chain.closed || i > 0) {
        const size_t s = (i + segs - 1) % segs;
        a = add(a, segA[s]);
        b = add(b, segB[s]);
      }
      if (chain.closed || i < segs) {
        a = add(a, segA[i % segs]);
        b = add(b, segB[i % segs]);
      }
      push(chain.nA, unit(a, segA[std::min(i, segs - 1)]));
      push(chain.nB, unit(b, segB[std::min(i, segs - 1)]));
    }
    chain.length = chainLength(polylineOf(chain));
    chains.push_back(std::move(chain));
  };
  auto walk = [&](uint32_t start, int firstEdge) {
    std::vector<uint32_t> verts{start};
    uint32_t cur = start;
    int e = firstEdge;
    while (e >= 0) {
      used[static_cast<size_t>(e)] = 1;
      cur = other(e, cur);
      if (cur == start && !isBreak(start)) return finish(verts, true);
      verts.push_back(cur);
      if (cur == start) break;  // a loop through a corner: an open chain whose ends meet
      if (isBreak(cur)) break;
      int next = -1;
      for (int candidate : around[cur])
        if (!used[static_cast<size_t>(candidate)]) next = candidate;
      e = next;
    }
    finish(verts, false);
  };
  // Open chains first, from every stopping point; what is left are loops.
  std::vector<uint32_t> starts;
  for (const auto& entry : around)
    if (isBreak(entry.first)) starts.push_back(entry.first);
  std::sort(starts.begin(), starts.end());
  for (uint32_t v : starts)
    for (int e : around[v])
      if (!used[static_cast<size_t>(e)]) walk(v, e);
  for (size_t i = 0; i < edges.size(); ++i)
    if (!used[i]) walk(edges[i].a, static_cast<int>(i));
  return chains;
}

std::vector<int> matchPick(const std::vector<Chain>& chains, const std::vector<double>& pickPts) {
  std::vector<int> out;
  if (pickPts.size() < 6 || chains.empty()) return out;
  const double pickLen = chainLength(pickPts);
  const std::vector<vec3> pickSamples = samples(pickPts, 24);
  int best = -1;
  double bestScore = 1e300;
  for (size_t i = 0; i < chains.size(); ++i) {
    const std::vector<double> line = polylineOf(chains[i]);
    double toChain = 0;
    for (const vec3& q : pickSamples) toChain += distanceToPolyline(q, line);
    toChain /= static_cast<double>(pickSamples.size());
    const std::vector<vec3> chainSamples = samples(line, 24);
    double toPick = 0;
    for (const vec3& q : chainSamples) toPick += distanceToPolyline(q, pickPts);
    toPick /= static_cast<double>(std::max<size_t>(1, chainSamples.size()));
    const double score = std::max(toChain, toPick);
    if (score < bestScore) {
      bestScore = score;
      best = static_cast<int>(i);
    }
    // A chain lying right on the pick: the picked edge was split by a later change.
    if (toPick < 0.02) out.push_back(static_cast<int>(i));
  }
  const double tolerance = std::max(0.5, 0.2 * pickLen);
  if (best >= 0 && bestScore < tolerance && std::find(out.begin(), out.end(), best) == out.end()) out.push_back(best);
  std::sort(out.begin(), out.end());
  return out;
}

// ---- rounding ----------------------------------------------------------------------------------

Manifold applyEdgeFillets(const Manifold& solid, const std::vector<Pick>& picks, int segments, std::string* warning,
                          std::string* log, int* lost) {
  if (lost) *lost = 0;
  if (!good(solid) || picks.empty()) return solid;
  const int segs = std::clamp(segments, 2, 48);
  const std::vector<Chain> chains = findChains(solid);
  say(log, "Edge fillet: " + std::to_string(chains.size()) + " sharp edge chain(s) on the solid.");

  std::vector<double> radius(chains.size(), 0.0);
  std::vector<char> flat(chains.size(), 0);
  int missing = 0;
  for (const Pick& pick : picks) {
    const std::vector<int> found = matchPick(chains, pick.pts);
    if (found.empty()) ++missing;
    for (int i : found)
      if (radius[static_cast<size_t>(i)] <= 0) {
        radius[static_cast<size_t>(i)] = std::max(0.05, pick.radius);
        flat[static_cast<size_t>(i)] = pick.chamfer ? 1 : 0;
      }
  }
  if (lost) *lost = missing;
  if (missing > 0)
    warn(warning, std::to_string(missing) + " filleted edge" + (missing == 1 ? " was" : "s were") +
                      " not found any more (the shape changed too much). Pick them again in Edge fillet (beta).");

  const Mesh mesh = readMesh(solid);
  double far = 1.0;
  for (const vec3& p : mesh.p) far = std::max({far, std::abs(p.x), std::abs(p.y), std::abs(p.z)});
  const double over = std::max(0.02, far * 2e-5);

  struct Built {
    int chain;
    std::vector<vec3> pts;
    std::vector<Section> sec;
    bool closed;
    bool concave;
    double r;
    bool chamfer = false;
  };
  std::vector<Built> built;
  for (size_t i = 0; i < chains.size(); ++i) {
    if (radius[i] <= 0) continue;
    const Chain& chain = chains[i];
    Built b;
    b.chain = static_cast<int>(i);
    b.closed = chain.closed;
    b.concave = chain.concave;
    b.r = radius[i];
    b.chamfer = flat[i] != 0;
    const size_t n = chain.pts.size() / 3;
    bool ok = n >= 2;
    for (size_t k = 0; k < n && ok; ++k) {
      Section s;
      ok = section(at(chain.pts, k), at(chain.nA, k), at(chain.nB, k), chain.concave, b.r, segs, over, &s, b.chamfer);
      b.pts.push_back(at(chain.pts, k));
      b.sec.push_back(std::move(s));
    }
    if (!ok) {
      warn(warning, "An edge that is almost flat or knife-sharp was skipped.");
      continue;
    }
    // Neighbouring sections must not cross, or the sweep folds over itself: the radius is too
    // big for how tightly the edge curves there.
    const size_t steps = b.closed ? n : n - 1;
    for (size_t k = 0; k < steps && ok; ++k) {
      const size_t k2 = (k + 1) % n;
      const vec3 t = unit(sub(b.pts[k2], b.pts[k]));
      for (size_t j = 0; j < b.sec[k].q.size() && ok; ++j)
        if (dot(sub(b.sec[k2].q[j], b.sec[k].q[j]), t) <= 1e-9) ok = false;
    }
    if (!ok) {
      warn(warning, "The radius is too big for a tightly curved edge, so that edge was skipped. Try a smaller radius.");
      say(log, "  chain " + std::to_string(i) + ": sections cross, skipped.");
      continue;
    }
    built.push_back(std::move(b));
  }

  // Chain ends, grouped by the vertex they sit on.
  struct End {
    size_t b;     // index into built
    bool start;   // first point (else last)
    vec3 v;
    vec3 away;    // along the chain, away from this end
  };
  std::map<std::array<long long, 3>, std::vector<End>> ends;
  auto keyOf = [](const vec3& p) {
    return std::array<long long, 3>{std::llround(p.x * 1e5), std::llround(p.y * 1e5), std::llround(p.z * 1e5)};
  };
  for (size_t i = 0; i < built.size(); ++i) {
    const Built& b = built[i];
    if (b.closed) continue;
    const size_t n = b.pts.size();
    ends[keyOf(b.pts.front())].push_back({i, true, b.pts.front(), unit(sub(b.pts[1], b.pts[0]))});
    ends[keyOf(b.pts.back())].push_back({i, false, b.pts.back(), unit(sub(b.pts[n - 2], b.pts[n - 1]))});
  }

  // How far each open end runs on, and the plane it is trimmed by (normal zero: no trim).
  std::vector<double> extStart(built.size(), 0), extEnd(built.size(), 0);
  std::vector<vec3> trimStartN(built.size(), vec3(0, 0, 0)), trimEndN(built.size(), vec3(0, 0, 0));
  std::vector<vec3> trimStartP(built.size(), vec3(0, 0, 0)), trimEndP(built.size(), vec3(0, 0, 0));
  std::vector<Manifold> cuts, fills;
  int mitres = 0, sweeps = 0, balls = 0;
  // An inside fillet swept around an outside corner joins the two chains' sweeps into one tube:
  // `stations` runs from chain a's end (replacing its end section) to chain b's end.
  struct Link {
    size_t a = 0;
    bool aStart = false;
    size_t b = 0;
    bool bStart = false;
    double shortA = 0;
    double shortB = 0;
    std::vector<std::vector<vec3>> stations;
  };
  std::vector<Link> links;
  // An inside fillet ending at an outside edge that is rounded (or chamfered) too, where that edge runs across it
  // (a cove along a wall ending at the wall's rounded front edge, or at the floor's rounded front
  // edge): the cove carries on round that rounding, so it follows the curve to the face beyond
  // instead of ending flat and sticking out past it. `stations` start with the cove's own section
  // where the rounding begins, `cut` back from the corner.
  struct Tail {
    double cut = 0;
    std::vector<std::vector<vec3>> stations;
  };
  std::vector<Tail> tails;
  std::vector<int> tailAt(built.size() * 2, -1);
  int wraps = 0;
  for (auto& entry : ends) {
    std::vector<End>& here = entry.second;
    for (End& e : here) {
      const Built& b = built[e.b];
      // Others of the same kind at this vertex.
      std::vector<End*> mates;
      for (End& o : here)
        if (&o != &e && built[o.b].concave == b.concave) mates.push_back(&o);
      const Section& sec = e.start ? b.sec.front() : b.sec.back();
      const double ext = b.r + over;
      auto setExt = [&](double value, const vec3& trimN, const vec3& trimP) {
        if (e.start) {
          extStart[e.b] = value;
          trimStartN[e.b] = trimN;
          trimStartP[e.b] = trimP;
        } else {
          extEnd[e.b] = value;
          trimEndN[e.b] = trimN;
          trimEndP[e.b] = trimP;
        }
      };
      if (mates.size() == 1) {
        const End& o = *mates.front();
        const vec3 split = sub(e.away, o.away);
        if (len(split) < 1e-6) {
          setExt(0, vec3(0, 0, 0), e.v);  // the two run straight on into each other
          continue;
        }
        // Where the corner opens: away from both chains.
        const vec3 open = unit(mul(add(e.away, o.away), -1.0), vec3(0, 0, 0));
        const vec3 probe = add(sec.nub, mul(open, 0.3 * b.r));
        if (b.concave && !inside(mesh, probe)) {
          // An inside fillet wrapping an outside corner (a box standing on a plate): sweep the
          // section around the corner instead of mitring, once per pair. When the upright edge
          // of that corner is rounded too, the sweep goes around ITS axis, and both straight
          // pieces stop where the rounding starts, so the foot follows the rounded corner.
          const vec3 in = mul(e.away, -1.0);
          const vec3 axis = unit(cross(in, o.away));
          vec3 pivot = e.v;
          for (const End& up : here) {
            if (!built[up.b].concave && len(axis) > 0.5 && std::abs(dot(up.away, axis)) > 0.95) {
              pivot = (up.start ? built[up.b].sec.front() : built[up.b].sec.back()).centre;
              break;
            }
          }
          const double shortE = std::max(0.0, dot(sub(pivot, e.v), e.away));
          const double shortO = std::max(0.0, dot(sub(pivot, o.v), o.away));
          setExt(0, vec3(0, 0, 0), e.v);
          if (e.b < o.b || (e.b == o.b && e.start)) {
            const double turn = std::acos(std::clamp(dot(in, o.away), -1.0, 1.0));
            if (len(axis) > 0.5 && turn > 1e-3) {
              const int steps = std::max(2, static_cast<int>(std::ceil(segs * turn / (kPi / 2))));
              const vec3 back = mul(e.away, shortE);
              (void)shortO;
              // Section points lying on the turning axis (the post's own sharp corner) would stay
              // put at every step and pinch the sweep into overlapping sheets there: non-manifold
              // edges in the STL. Move them a hair into the post, where the union swallows them.
              std::vector<vec3> base;
              for (const vec3& q0 : sec.q) {
                const vec3 q = add(q0, back);
                const vec3 off = sub(q, pivot);
                const vec3 radial = sub(off, mul(axis, dot(off, axis)));
                if (len(radial) < 0.5 * over) {
                  vec3 in = sub(q, sec.nub);
                  in = unit(sub(in, mul(axis, dot(in, axis))), vec3(0, 0, 0));
                  base.push_back(add(q, mul(in, over)));
                } else {
                  base.push_back(q);
                }
              }
              Link link;
              link.a = e.b;
              link.aStart = e.start;
              link.b = o.b;
              link.bStart = o.start;
              link.shortA = shortE;
              link.shortB = shortO;
              for (int k = 0; k <= steps; ++k) {
                std::vector<vec3> ring;
                for (const vec3& q : base) ring.push_back(rotateAbout(q, pivot, axis, turn * k / steps));
                link.stations.push_back(std::move(ring));
              }
              links.push_back(std::move(link));
              ++sweeps;
            }
          }
          continue;
        }
        // Mitre: both run on and are trimmed on the bisecting plane.
        setExt(2.0 * b.r + over, unit(split), e.v);
        ++mitres;
        continue;
      }
      if (b.concave) {
        const Chain& own = chains[static_cast<size_t>(b.chain)];
        const size_t ownAt = e.start ? 0 : own.pts.size() / 3 - 1;
        const vec3 ownA = unit(at(own.nA, ownAt)), ownB = unit(at(own.nB, ownAt));
        const End* round = nullptr;
        vec3 shared, other;
        for (const End& u : here) {
          if (built[u.b].concave || std::abs(dot(u.away, e.away)) > 0.2) continue;
          const Chain& edge = chains[static_cast<size_t>(built[u.b].chain)];
          const size_t edgeAt = u.start ? 0 : edge.pts.size() / 3 - 1;
          const vec3 eA = unit(at(edge.nA, edgeAt)), eB = unit(at(edge.nB, edgeAt));
          // The two edges share a face: the cove runs along it, into the rounded edge.
          for (const vec3& face : {ownA, ownB}) {
            if (dot(face, eA) > 0.98) shared = eA, other = eB;
            else if (dot(face, eB) > 0.98) shared = eB, other = eA;
            else continue;
            round = &u;
            break;
          }
          if (round) break;
        }
        if (round) {
          const Section& roundSec = round->start ? built[round->b].sec.front() : built[round->b].sec.back();
          const vec3 pivot = roundSec.centre;
          const vec3 axis = unit(cross(shared, other), vec3(0, 0, 0));
          const double turn = std::acos(std::clamp(dot(shared, other), -1.0, 1.0));
          const double cut = std::max(0.0, dot(sub(pivot, e.v), e.away));
          if (len(axis) > 0.5 && turn > 1e-3 && cut > 0) {
            // Round a chamfered edge in one straight step: the cove then runs along the flat cut.
            const int steps = built[round->b].chamfer ? 1 : std::max(2, static_cast<int>(std::ceil(segs * turn / (kPi / 2))));
            Tail tail;
            tail.cut = cut;
            const vec3 back = mul(e.away, cut);
            std::vector<vec3> base;
            for (const vec3& q : sec.q) base.push_back(add(q, back));
            for (int k = 0; k <= steps; ++k) {
              std::vector<vec3> ring;
              for (const vec3& q : base) ring.push_back(rotateAbout(q, pivot, axis, turn * k / steps));
              tail.stations.push_back(std::move(ring));
            }
            tailAt[e.b * 2 + (e.start ? 0 : 1)] = static_cast<int>(tails.size());
            tails.push_back(std::move(tail));
            setExt(0, vec3(0, 0, 0), e.v);
            ++wraps;
            continue;
          }
        }
      }
      // A free end, or three or more meeting: run on only into air (cut) or into the solid (fill),
      // where the extra length touches nothing.
      const vec3 probe = add(sec.nub, mul(e.away, -0.5 * ext));
      const bool solidThere = inside(mesh, probe);
      setExt(b.concave == solidThere ? ext : 0.0, vec3(0, 0, 0), e.v);
    }
    // Three square outside edges meeting: a ball corner.
    std::vector<End*> convex;
    for (End& e : here)
      if (!built[e.b].concave) convex.push_back(&e);
    // Three chamfers need no corner piece: their flat cuts already meet in a point, as on a
    // chamfered cube.
    const bool allFlat = convex.size() == 3 && built[convex[0]->b].chamfer && built[convex[1]->b].chamfer && built[convex[2]->b].chamfer;
    if (convex.size() == 3 && !allFlat) {
      const vec3 u1 = convex[0]->away, u2 = convex[1]->away, u3 = convex[2]->away;
      const double r = std::min({built[convex[0]->b].r, built[convex[1]->b].r, built[convex[2]->b].r});
      if (std::abs(dot(u1, u2)) < 0.15 && std::abs(dot(u2, u3)) < 0.15 && std::abs(dot(u1, u3)) < 0.15 &&
          std::abs(built[convex[0]->b].r - r) < 1e-6 && std::abs(built[convex[1]->b].r - r) < 1e-6 &&
          std::abs(built[convex[2]->b].r - r) < 1e-6) {
        const vec3 v = convex[0]->v;
        const vec3 c = add(v, mul(add(add(u1, u2), u3), r));
        const vec3 o = sub(v, mul(add(add(u1, u2), u3), over));
        const double side = r + over;
        manifold::mat3x4 m;
        m[0] = mul(u1, side);
        m[1] = mul(u2, side);
        m[2] = mul(u3, side);
        m[3] = o;
        Manifold box = Manifold::Cube(vec3(1, 1, 1)).Transform(m);
        Manifold ball = Manifold::Sphere(r, std::max(16, segs * 4)).Translate(c);
        Manifold corner = box - ball;
        if (good(corner)) {
          cuts.push_back(std::move(corner));
          ++balls;
        }
      }
    }
    // Three square inside edges meeting (the inside corner of a pocket): the same ball corner,
    // filled. The three coves only reach the corner as cylinders, which leaves a notch there.
    std::vector<End*> inside3;
    for (End& e : here)
      if (built[e.b].concave) inside3.push_back(&e);
    const bool insideFlat = inside3.size() == 3 && built[inside3[0]->b].chamfer && built[inside3[1]->b].chamfer && built[inside3[2]->b].chamfer;
    if (inside3.size() == 3 && !insideFlat) {
      const vec3 u1 = inside3[0]->away, u2 = inside3[1]->away, u3 = inside3[2]->away;
      const double r = std::min({built[inside3[0]->b].r, built[inside3[1]->b].r, built[inside3[2]->b].r});
      if (std::abs(dot(u1, u2)) < 0.15 && std::abs(dot(u2, u3)) < 0.15 && std::abs(dot(u1, u3)) < 0.15 &&
          std::abs(built[inside3[0]->b].r - r) < 1e-6 && std::abs(built[inside3[1]->b].r - r) < 1e-6 &&
          std::abs(built[inside3[2]->b].r - r) < 1e-6) {
        const vec3 v = inside3[0]->v;
        const vec3 c = add(v, mul(add(add(u1, u2), u3), r));
        // The box runs `over` back into the three faces, so the fill joins them cleanly.
        const vec3 o = sub(v, mul(add(add(u1, u2), u3), over));
        const double side = r + over;
        manifold::mat3x4 m;
        m[0] = mul(u1, side);
        m[1] = mul(u2, side);
        m[2] = mul(u3, side);
        m[3] = o;
        Manifold box = Manifold::Cube(vec3(1, 1, 1)).Transform(m);
        Manifold ball = Manifold::Sphere(r, std::max(16, segs * 4)).Translate(c);
        Manifold corner = box - ball;
        if (good(corner)) {
          fills.push_back(std::move(corner));
          ++balls;
        }
      }
    }
  }

  // Each chain's stations in walking order, with its free ends run on (extensions) and its linked
  // ends shortened to where the corner sweep takes over.
  auto endKey = [](size_t chain, bool start) { return chain * 2 + (start ? 0 : 1); };
  std::vector<int> linkAt(built.size() * 2, -1);
  for (size_t li = 0; li < links.size(); ++li) {
    linkAt[endKey(links[li].a, links[li].aStart)] = static_cast<int>(li);
    linkAt[endKey(links[li].b, links[li].bStart)] = static_cast<int>(li);
  }
  auto chainStations = [&](size_t i) {
    const Built& b = built[i];
    std::vector<std::vector<vec3>> own;
    for (const Section& sec : b.sec) own.push_back(sec.q);
    if (b.closed) return own;
    const size_t n = b.pts.size();
    // The chain's own sections kept (lo..hi), and what goes before and after them. Worked out on
    // indices, so changing one end never shifts the other.
    size_t lo = 0, hi = n - 1;
    std::vector<std::vector<vec3>> before, after;
    for (int side = 0; side < 2; ++side) {
      const bool start = side == 0;
      const vec3 v = start ? b.pts.front() : b.pts.back();
      // Drops the own sections within `cut` of this end (always at least `least` of them).
      auto drop = [&](double cut, size_t least) {
        size_t dropped = 0;
        if (start) {
          while (lo < hi && (dropped < least || len(sub(b.pts[lo], v)) < cut + 1e-9)) ++lo, ++dropped;
        } else {
          while (hi > lo && (dropped < least || len(sub(b.pts[hi], v)) < cut + 1e-9)) --hi, ++dropped;
        }
      };
      const int li = linkAt[endKey(i, start)];
      if (li >= 0) {
        const Link& L = links[static_cast<size_t>(li)];
        const bool isA = L.a == i && L.aStart == start;
        const double cut = isA ? L.shortA : L.shortB;
        std::vector<vec3> replacement = isA ? L.stations.front() : L.stations.back();
        // The corner's sections run round in the other chain's order; match this chain's.
        {
          const std::vector<vec3>& end = start ? own.front() : own.back();
          const size_t m = end.size();
          if (replacement.size() == m && m >= 3) {
            double same = 0, flipped = 0;
            for (size_t j = 1; j < m; ++j) {
              same += len(sub(replacement[j], end[j]));
              flipped += len(sub(replacement[m - j], end[j]));
            }
            if (flipped < same) std::reverse(replacement.begin() + 1, replacement.end());
          }
        }
        // Drop the sections the corner sweep covers, then end on the sweep's first section.
        drop(cut, 1);
        (start ? before : after).push_back(std::move(replacement));
        continue;
      }
      if (const int ti = tailAt[endKey(i, start)]; ti >= 0) {
        // Drop the sections the wrap replaces, then carry on round the rounded edge.
        const Tail& T = tails[static_cast<size_t>(ti)];
        drop(T.cut, 0);
        if (start) before.assign(T.stations.rbegin(), T.stations.rend());
        else after = T.stations;
        continue;
      }
      const double ext = start ? extStart[i] : extEnd[i];
      if (ext <= 0) continue;
      const vec3 shift = start ? mul(unit(sub(b.pts[0], b.pts[1])), ext) : mul(unit(sub(b.pts[n - 1], b.pts[n - 2])), ext);
      std::vector<vec3> ring;
      for (const vec3& q : start ? own.front() : own.back()) ring.push_back(add(q, shift));
      (start ? before : after).push_back(std::move(ring));
    }
    std::vector<std::vector<vec3>> st = std::move(before);
    for (size_t k = lo; k <= hi; ++k) st.push_back(own[k]);
    for (auto& ring : after) st.push_back(std::move(ring));
    return st;
  };
  // Point order of a section (corner point first, then round the outline) can run either way
  // depending on how a chain was walked; turn a block to match the one before it.
  auto align = [](std::vector<std::vector<vec3>>& block, const std::vector<vec3>& previous) {
    if (block.empty() || block.front().size() != previous.size() || previous.size() < 3) return;
    const size_t m = previous.size();
    double same = 0, flipped = 0;
    for (size_t j = 1; j < m; ++j) {
      same += len(sub(block.front()[j], previous[j]));
      flipped += len(sub(block.front()[m - j], previous[j]));
    }
    if (flipped < same)
      for (auto& ring : block) std::reverse(ring.begin() + 1, ring.end());
  };
  std::vector<char> used(built.size(), 0);
  auto emit = [&](std::vector<std::vector<vec3>> st, bool closed, bool concave, const std::vector<size_t>& members) {
    Manifold piece = tube(st, closed);
    if (!good(piece)) {
      warn(warning, "One edge did not give a clean fillet and was skipped.");
      say(log, "  a sweep of " + std::to_string(members.size()) + " chain(s) was not a valid solid.");
      return;
    }
    // Trims only near their corner: an endless plane would also slice a strongly curved chain
    // (the bowl of a letter) somewhere else along its length.
    for (size_t i : members) {
      const double reach = 3.0 * built[i].r + 4.0 * over;
      if (len(trimStartN[i]) > 0.5) piece = localTrim(piece, trimStartN[i], trimStartP[i], reach + extStart[i]);
      if (len(trimEndN[i]) > 0.5) piece = localTrim(piece, trimEndN[i], trimEndP[i], reach + extEnd[i]);
    }
    if (good(piece)) (concave ? fills : cuts).push_back(std::move(piece));
  };
  for (size_t first = 0; first < built.size(); ++first) {
    if (used[first]) continue;
    if (built[first].closed) {
      used[first] = 1;
      emit(chainStations(first), true, built[first].concave, {first});
      continue;
    }
    // Walk back to where this run of linked chains starts (or find that it closes on itself).
    size_t cur = first;
    bool enterStart = true;  // walked from its start to its end
    bool loop = false;
    for (size_t guard = 0; guard <= built.size(); ++guard) {
      const int li = linkAt[endKey(cur, enterStart)];
      if (li < 0) break;
      const Link& L = links[static_cast<size_t>(li)];
      const bool fromA = L.a == cur && L.aStart == enterStart;
      const size_t other = fromA ? L.b : L.a;
      const bool otherEnd = fromA ? L.bStart : L.aStart;
      cur = other;
      enterStart = !otherEnd;
      if (cur == first && enterStart) {
        loop = true;
        break;
      }
    }
    // Walk forward, joining chains and corner sweeps into one run of stations.
    std::vector<std::vector<vec3>> run;
    std::vector<size_t> members;
    bool fits = true;
    const size_t startChain = cur;
    const bool startEnter = enterStart;
    for (size_t guard = 0; guard <= built.size(); ++guard) {
      if (used[cur]) break;
      used[cur] = 1;
      members.push_back(cur);
      std::vector<std::vector<vec3>> block = chainStations(cur);
      if (!enterStart) std::reverse(block.begin(), block.end());
      if (!run.empty()) {
        align(block, run.back());
        // The chain's first section is the corner sweep's last one, already in the run.
        block.erase(block.begin());
      }
      for (auto& ring : block) {
        if (!run.empty() && ring.size() != run.back().size()) fits = false;
        run.push_back(std::move(ring));
      }
      const bool exitStart = !enterStart;
      const int li = linkAt[endKey(cur, exitStart)];
      if (li < 0) break;
      const Link& L = links[static_cast<size_t>(li)];
      const bool fromA = L.a == cur && L.aStart == exitStart;
      std::vector<std::vector<vec3>> corner = L.stations;
      if (!fromA) std::reverse(corner.begin(), corner.end());
      align(corner, run.back());
      corner.erase(corner.begin());  // equals this chain's end section, already in the run
      for (auto& ring : corner) {
        if (ring.size() != run.back().size()) fits = false;
        run.push_back(std::move(ring));
      }
      cur = fromA ? L.b : L.a;
      enterStart = fromA ? L.bStart : L.aStart;
      if (cur == startChain && enterStart == startEnter) break;  // all the way round
    }
    if (fits) {
      if (loop) run.pop_back();  // the last corner section is the first chain's start section
      emit(std::move(run), loop, built[first].concave, members);
    } else {
      // Sections of different sizes (chains meeting at different angles) cannot be stitched:
      // each chain on its own, each corner on its own.
      for (size_t i : members) emit(chainStations(i), false, built[i].concave, {i});
      for (const Link& L : links)
        if (std::find(members.begin(), members.end(), L.a) != members.end()) emit(L.stations, false, true, {});
    }
  }
  say(log, "  " + std::to_string(built.size()) + " edge(s) rounded: " + std::to_string(cuts.size()) + " cut piece(s), " +
               std::to_string(fills.size()) + " fill piece(s), " + std::to_string(mitres / 2) + " mitre(s), " +
               std::to_string(sweeps) + " swept corner(s), " + std::to_string(balls) + " ball corner(s), " +
               std::to_string(wraps) + " wrap(s) round a rounded edge.");
  if (cuts.empty() && fills.empty()) return solid;

  // Where a fillet runs into a curved face the booleans can leave zero-volume flakes as separate
  // shells. They print as nothing but upset slicers and the hole count; drop them.
  int flakes = 0;
  auto clean = [&](Manifold m) {
    if (!good(m)) return m;
    std::vector<Manifold> parts = m.Decompose();
    if (parts.size() < 2) return m;
    std::vector<Manifold> kept;
    for (Manifold& part : parts)
      if (std::abs(part.Volume()) > 1e-3) kept.push_back(std::move(part));
    if (kept.empty() || kept.size() == parts.size()) return m;
    flakes += static_cast<int>(parts.size() - kept.size());
    Manifold joined = kept.size() == 1 ? kept.front() : Manifold::BatchBoolean(kept, manifold::OpType::Add);
    return good(joined) ? joined : m;
  };
  auto combine = [&](const std::vector<Manifold>& cutList, const std::vector<Manifold>& fillList) {
    Manifold out = solid;
    try {
      if (!cutList.empty()) out = out - Manifold::BatchBoolean(cutList, manifold::OpType::Add);
      if (!fillList.empty()) out = out + Manifold::BatchBoolean(fillList, manifold::OpType::Add);
    } catch (const std::exception&) {
      out = Manifold();
    }
    return clean(out);
  };
  Manifold result = combine(cuts, fills);
  if (!good(result)) {
    warn(warning, "The edge fillet did not work on this solid, so it is shown without it.");
    return solid;
  }
  // Rounding edges never adds or closes a hole through the part. When the result says otherwise, a
  // piece cut through a thin wall or bridged a gap (or the booleans tripped over a curved face):
  // find the pieces that do that on their own and leave them out.
  const int genus = solid.Genus();
  if (result.Genus() != genus) {
    // One piece at a time onto the solid, keeping each only if the hole count stays the same.
    Manifold running = solid;
    int dropped = 0;
    auto step = [&](const Manifold& piece, bool fill) {
      Manifold next;
      try {
        next = clean(fill ? running + piece : running - piece);
      } catch (const std::exception&) {
        next = Manifold();
      }
      if (good(next) && next.Genus() == genus) running = std::move(next);
      else ++dropped;
    };
    for (const Manifold& piece : cuts) step(piece, false);
    for (const Manifold& piece : fills) step(piece, true);
    result = running;
    if (dropped > 0)
      warn(warning, std::to_string(dropped) + (dropped == 1 ? " fillet piece was left out because it would"
                                                            : " fillet pieces were left out because they would") +
                        " cut through the part or close a gap. Try a smaller radius there.");
    say(log, "  hole count would change, so pieces were added one by one; " + std::to_string(dropped) + " left out.");
  }
  if (flakes > 0) say(log, "  dropped " + std::to_string(flakes) + " empty flake(s).");
  return result;
}

std::vector<Pick> decodePicks(const std::vector<double>& data) {
  std::vector<Pick> picks;
  size_t i = 0;
  while (i + 2 <= data.size()) {
    Pick pick;
    pick.radius = std::abs(data[i]);
    pick.chamfer = data[i] < 0;
    const double countValue = data[i + 1];
    if (!(countValue >= 0) || countValue > 1e7) break;
    const size_t count = static_cast<size_t>(countValue);
    i += 2;
    if (i + count * 3 > data.size()) break;
    pick.pts.assign(data.begin() + static_cast<std::ptrdiff_t>(i), data.begin() + static_cast<std::ptrdiff_t>(i + count * 3));
    i += count * 3;
    if (count >= 2 && pick.radius > 0) picks.push_back(std::move(pick));
  }
  return picks;
}

std::vector<double> encodePicks(const std::vector<Pick>& picks) {
  std::vector<double> out;
  for (const Pick& pick : picks) {
    if (pick.pts.size() < 6) continue;
    out.push_back(pick.chamfer ? -pick.radius : pick.radius);
    out.push_back(static_cast<double>(pick.pts.size() / 3));
    out.insert(out.end(), pick.pts.begin(), pick.pts.end());
  }
  return out;
}

}  // namespace ef
