#include "sculpt.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace sculpt {

namespace la = manifold::la;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

vec3 triNormalOf(const vec3& a, const vec3& b, const vec3& c) { return la::cross(b - a, c - a); }

// Closest point of triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
vec3 closestOnTriangle(const vec3& p, const vec3& a, const vec3& b, const vec3& c) {
  const vec3 ab = b - a, ac = c - a, ap = p - a;
  const double d1 = la::dot(ab, ap), d2 = la::dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) return a;
  const vec3 bp = p - b;
  const double d3 = la::dot(ab, bp), d4 = la::dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return b;
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
  const vec3 cp = p - c;
  const double d5 = la::dot(ab, cp), d6 = la::dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return c;
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const double denom = 1.0 / (va + vb + vc);
  return a + ab * (vb * denom) + ac * (vc * denom);
}

// Ray (or segment, maxT = 1) against a triangle; t of the crossing.
bool rayTriangle(const vec3& o, const vec3& d, const vec3& a, const vec3& b, const vec3& c, double maxT, double* t) {
  const vec3 e1 = b - a, e2 = c - a;
  const vec3 p = la::cross(d, e2);
  const double det = la::dot(e1, p);
  const double scale = la::length(e1) * la::length(e2) * la::length(d);
  if (std::abs(det) <= 1e-14 * scale) return false;
  const double inv = 1.0 / det;
  const vec3 s = o - a;
  const double u = la::dot(s, p) * inv;
  if (u < 0 || u > 1) return false;
  const vec3 q = la::cross(s, e1);
  const double v = la::dot(d, q) * inv;
  if (v < 0 || u + v > 1) return false;
  const double hit = la::dot(e2, q) * inv;
  if (hit < 0 || hit > maxT) return false;
  *t = hit;
  return true;
}

bool trianglesCross(const vec3* A, const vec3* B) {
  double t;
  for (int i = 0; i < 3; ++i) {
    if (rayTriangle(A[i], A[(i + 1) % 3] - A[i], B[0], B[1], B[2], 1.0, &t)) return true;
    if (rayTriangle(B[i], B[(i + 1) % 3] - B[i], A[0], A[1], A[2], 1.0, &t)) return true;
  }
  return false;
}

bool boxesOverlap(const vec3* A, const vec3* B) {
  for (int axis = 0; axis < 3; ++axis) {
    const double aLo = std::min({A[0][axis], A[1][axis], A[2][axis]}), aHi = std::max({A[0][axis], A[1][axis], A[2][axis]});
    const double bLo = std::min({B[0][axis], B[1][axis], B[2][axis]}), bHi = std::max({B[0][axis], B[1][axis], B[2][axis]});
    if (aHi < bLo || bHi < aLo) return false;
  }
  return true;
}

bool shareVertex(const Tri& a, const Tri& b) {
  for (uint32_t x : a)
    for (uint32_t y : b)
      if (x == y) return true;
  return false;
}

// A fixed grid of triangles, for distance and overlap queries on a mesh that does not change.
struct StaticGrid {
  vec3 lo{0, 0, 0};
  double cell = 1;
  int n[3] = {1, 1, 1};
  std::vector<uint32_t> start, items;

  int index(double v, int axis) const {
    return std::clamp(static_cast<int>(std::floor((v - lo[axis]) / cell)), 0, n[axis] - 1);
  }
  void build(const Mesh& mesh, double wantedCell) {
    vec3 a, b;
    bounds(mesh, &a, &b);
    cell = std::max(wantedCell, 1e-6);
    // Not more than about 4 million cells.
    for (;;) {
      double total = 1;
      for (int axis = 0; axis < 3; ++axis) total *= std::floor((b[axis] - a[axis]) / cell) + 1;
      if (total <= 4e6) break;
      cell *= 1.5;
    }
    lo = a;
    for (int axis = 0; axis < 3; ++axis) n[axis] = static_cast<int>(std::floor((b[axis] - a[axis]) / cell)) + 1;
    const size_t cells = size_t(n[0]) * n[1] * n[2];
    start.assign(cells + 1, 0);
    auto each = [&](uint32_t t, auto&& f) {
      const Tri& tri = mesh.tri[t];
      int l[3], h[3];
      for (int axis = 0; axis < 3; ++axis) {
        const double mn = std::min({mesh.pos[tri[0]][axis], mesh.pos[tri[1]][axis], mesh.pos[tri[2]][axis]});
        const double mx = std::max({mesh.pos[tri[0]][axis], mesh.pos[tri[1]][axis], mesh.pos[tri[2]][axis]});
        l[axis] = index(mn, axis);
        h[axis] = index(mx, axis);
      }
      for (int z = l[2]; z <= h[2]; ++z)
        for (int y = l[1]; y <= h[1]; ++y)
          for (int x = l[0]; x <= h[0]; ++x) f((size_t(z) * n[1] + y) * n[0] + x);
    };
    for (uint32_t t = 0; t < mesh.tri.size(); ++t) each(t, [&](size_t c) { ++start[c + 1]; });
    for (size_t c = 0; c < cells; ++c) start[c + 1] += start[c];
    items.assign(start[cells], 0);
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (uint32_t t = 0; t < mesh.tri.size(); ++t) each(t, [&](size_t c) { items[fill[c]++] = t; });
  }
  template <typename F>
  void forBox(const vec3& a, const vec3& b, F&& f) const {
    int l[3], h[3];
    for (int axis = 0; axis < 3; ++axis) {
      if (b[axis] < lo[axis] || a[axis] > lo[axis] + n[axis] * cell) return;
      l[axis] = index(a[axis], axis);
      h[axis] = index(b[axis], axis);
    }
    for (int z = l[2]; z <= h[2]; ++z)
      for (int y = l[1]; y <= h[1]; ++y)
        for (int x = l[0]; x <= h[0]; ++x) {
          const size_t c = (size_t(z) * n[1] + y) * n[0] + x;
          for (uint32_t i = start[c]; i < start[c + 1]; ++i) f(items[i]);
        }
  }
};

// Nearest surface point within `band` of p; returns the distance (band if there is none).
double nearestWithin(const Mesh& mesh, const StaticGrid& grid, const vec3& p, double band, vec3* closest,
                     uint32_t* triangle = nullptr) {
  double best = band * band;
  bool found = false;
  const vec3 r(band, band, band);
  grid.forBox(p - r, p + r, [&](uint32_t t) {
    const Tri& tri = mesh.tri[t];
    const vec3 q = closestOnTriangle(p, mesh.pos[tri[0]], mesh.pos[tri[1]], mesh.pos[tri[2]]);
    const vec3 d = q - p;
    const double dd = la::dot(d, d);
    if (dd < best) {
      best = dd;
      found = true;
      if (closest) *closest = q;
      if (triangle) *triangle = t;
    }
  });
  return found ? std::sqrt(best) : band;
}

// Columns of triangles seen from above, for "is this point inside?" by counting the surfaces above
// it (the winding number along +Z: overlapping parts count as inside, so they join).
struct ColumnGrid {
  double x0 = 0, y0 = 0, cell = 1;
  int nx = 1, ny = 1;
  std::vector<uint32_t> start, items;
  std::vector<int8_t> facing;  // +1 the triangle faces up, -1 down, 0 on edge (skipped)

  void build(const Mesh& mesh, double wantedCell) {
    vec3 a, b;
    bounds(mesh, &a, &b);
    cell = std::max(wantedCell, 1e-6);
    while ((std::floor((b.x - a.x) / cell) + 1) * (std::floor((b.y - a.y) / cell) + 1) > 4e6) cell *= 1.5;
    x0 = a.x;
    y0 = a.y;
    nx = static_cast<int>(std::floor((b.x - a.x) / cell)) + 1;
    ny = static_cast<int>(std::floor((b.y - a.y) / cell)) + 1;
    facing.resize(mesh.tri.size());
    start.assign(size_t(nx) * ny + 1, 0);
    auto each = [&](uint32_t t, auto&& f) {
      const Tri& tri = mesh.tri[t];
      const int lx = ix(std::min({mesh.pos[tri[0]].x, mesh.pos[tri[1]].x, mesh.pos[tri[2]].x}));
      const int hx = ix(std::max({mesh.pos[tri[0]].x, mesh.pos[tri[1]].x, mesh.pos[tri[2]].x}));
      const int ly = iy(std::min({mesh.pos[tri[0]].y, mesh.pos[tri[1]].y, mesh.pos[tri[2]].y}));
      const int hy = iy(std::max({mesh.pos[tri[0]].y, mesh.pos[tri[1]].y, mesh.pos[tri[2]].y}));
      for (int y = ly; y <= hy; ++y)
        for (int x = lx; x <= hx; ++x) f(size_t(y) * nx + x);
    };
    for (uint32_t t = 0; t < mesh.tri.size(); ++t) {
      const Tri& tri = mesh.tri[t];
      const double nz = triNormalOf(mesh.pos[tri[0]], mesh.pos[tri[1]], mesh.pos[tri[2]]).z;
      facing[t] = nz > 0 ? 1 : nz < 0 ? -1 : 0;
      if (facing[t]) each(t, [&](size_t c) { ++start[c + 1]; });
    }
    for (size_t c = 0; c + 1 < start.size(); ++c) start[c + 1] += start[c];
    items.assign(start.back(), 0);
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (uint32_t t = 0; t < mesh.tri.size(); ++t)
      if (facing[t]) each(t, [&](size_t c) { items[fill[c]++] = t; });
  }
  int ix(double x) const { return std::clamp(static_cast<int>(std::floor((x - x0) / cell)), 0, nx - 1); }
  int iy(double y) const { return std::clamp(static_cast<int>(std::floor((y - y0) / cell)), 0, ny - 1); }

  bool inside(const Mesh& mesh, const vec3& p) const {
    // Nudged off the grid lines and mesh edges a slicer-style ray would otherwise graze.
    const double px = p.x + cell * 1.2345678e-7, py = p.y + cell * 2.3456789e-7;
    if (px < x0 || py < y0 || px > x0 + nx * cell || py > y0 + ny * cell) return false;
    const size_t c = size_t(iy(py)) * nx + ix(px);
    int winding = 0;
    for (uint32_t i = start[c]; i < start[c + 1]; ++i) {
      const uint32_t t = items[i];
      const Tri& tri = mesh.tri[t];
      const vec3& a = mesh.pos[tri[0]];
      const vec3& b = mesh.pos[tri[1]];
      const vec3& d = mesh.pos[tri[2]];
      const double w0 = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
      const double w1 = (d.x - b.x) * (py - b.y) - (d.y - b.y) * (px - b.x);
      const double w2 = (a.x - d.x) * (py - d.y) - (a.y - d.y) * (px - d.x);
      if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))) continue;
      const double sum = w0 + w1 + w2;
      if (sum == 0) continue;
      const double z = (w1 * a.z + w2 * b.z + w0 * d.z) / sum;
      if (z > p.z) winding += facing[t];
    }
    return winding > 0;
  }
};

std::vector<std::vector<uint32_t>> neighbours(const Mesh& mesh) {
  std::vector<std::vector<uint32_t>> out(mesh.pos.size());
  for (const Tri& t : mesh.tri)
    for (int i = 0; i < 3; ++i) {
      out[t[i]].push_back(t[(i + 1) % 3]);
      out[t[i]].push_back(t[(i + 2) % 3]);
    }
  for (auto& list : out) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
  }
  return out;
}

std::vector<vec3> vertexNormals(const Mesh& mesh) {
  std::vector<vec3> n(mesh.pos.size(), vec3(0, 0, 0));
  for (const Tri& t : mesh.tri) {
    const vec3 f = triNormalOf(mesh.pos[t[0]], mesh.pos[t[1]], mesh.pos[t[2]]);
    for (uint32_t v : t) n[v] += f;
  }
  for (vec3& v : n) {
    const double l = la::length(v);
    v = l > 0 ? v / l : vec3(0, 0, 1);
  }
  return n;
}

Mesh icosphere(double radius, double edge) {
  const double g = (1 + std::sqrt(5.0)) / 2;
  Mesh m;
  m.pos = {{-1, g, 0}, {1, g, 0}, {-1, -g, 0}, {1, -g, 0}, {0, -1, g}, {0, 1, g},
           {0, -1, -g}, {0, 1, -g}, {g, 0, -1}, {g, 0, 1}, {-g, 0, -1}, {-g, 0, 1}};
  m.tri = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
           {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
           {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
  for (vec3& p : m.pos) p = la::normalize(p) * radius;
  double current = 1.0515 * radius;
  while (current > edge * 1.2 && m.tri.size() < 2'000'000) {
    std::unordered_map<uint64_t, uint32_t> mid;
    auto middle = [&](uint32_t a, uint32_t b) {
      const uint64_t key = (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
      const auto it = mid.find(key);
      if (it != mid.end()) return it->second;
      const uint32_t id = static_cast<uint32_t>(m.pos.size());
      m.pos.push_back(la::normalize(m.pos[a] + m.pos[b]) * radius);
      mid.emplace(key, id);
      return id;
    };
    std::vector<Tri> next;
    next.reserve(m.tri.size() * 4);
    for (const Tri& t : m.tri) {
      const uint32_t ab = middle(t[0], t[1]), bc = middle(t[1], t[2]), ca = middle(t[2], t[0]);
      next.push_back({t[0], ab, ca});
      next.push_back({t[1], bc, ab});
      next.push_back({t[2], ca, bc});
      next.push_back({ab, bc, ca});
    }
    m.tri = std::move(next);
    current *= 0.5;
  }
  return m;
}

}  // namespace

double falloff(Falloff kind, double t) {
  if (t >= 1) return 0;
  if (t <= 0) return 1;
  switch (kind) {
    case Falloff::Sharp: return (1 - t) * (1 - t);
    case Falloff::Flat: {
      if (t < 0.8) return 1;
      const double u = (t - 0.8) / 0.2;
      return 1 - u * u * (3 - 2 * u);
    }
    case Falloff::Smooth:
    default: return 1 - t * t * (3 - 2 * t);
  }
}

void bounds(const Mesh& mesh, vec3* lo, vec3* hi) {
  *lo = vec3(kInf, kInf, kInf);
  *hi = vec3(-kInf, -kInf, -kInf);
  for (const vec3& p : mesh.pos) {
    *lo = la::min(*lo, p);
    *hi = la::max(*hi, p);
  }
  if (mesh.pos.empty()) *lo = *hi = vec3(0, 0, 0);
}

double averageEdge(const Mesh& mesh) {
  double sum = 0;
  size_t count = 0;
  for (const Tri& t : mesh.tri)
    for (int i = 0; i < 3; ++i) {
      sum += la::length(mesh.pos[t[i]] - mesh.pos[t[(i + 1) % 3]]);
      ++count;
    }
  return count ? sum / count : 1;
}

double volume(const Mesh& mesh) {
  double v = 0;
  for (const Tri& t : mesh.tri) v += la::dot(mesh.pos[t[0]], la::cross(mesh.pos[t[1]], mesh.pos[t[2]]));
  return v / 6;
}

bool closedManifold(const Mesh& mesh) {
  if (mesh.tri.empty()) return false;
  std::unordered_map<uint64_t, int> edges;
  edges.reserve(mesh.tri.size() * 3);
  for (const Tri& t : mesh.tri) {
    if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) return false;
    for (int i = 0; i < 3; ++i) {
      if (t[i] >= mesh.pos.size()) return false;
      if (++edges[(uint64_t(t[i]) << 32) | t[(i + 1) % 3]] > 1) return false;
    }
  }
  for (const auto& [key, count] : edges) {
    (void)count;
    if (!edges.count((key << 32) | (key >> 32))) return false;
  }
  return true;
}

bool selfIntersects(const Mesh& mesh) {
  StaticGrid grid;
  grid.build(mesh, averageEdge(mesh) * 2);
  std::atomic<bool> found{false};
  for (uint32_t t = 0; t < mesh.tri.size() && !found; ++t) {
    const Tri& a = mesh.tri[t];
    const vec3 A[3] = {mesh.pos[a[0]], mesh.pos[a[1]], mesh.pos[a[2]]};
    const vec3 lo = la::min(A[0], la::min(A[1], A[2])), hi = la::max(A[0], la::max(A[1], A[2]));
    grid.forBox(lo, hi, [&](uint32_t u) {
      if (found || u <= t) return;
      const Tri& b = mesh.tri[u];
      if (shareVertex(a, b)) return;
      const vec3 B[3] = {mesh.pos[b[0]], mesh.pos[b[1]], mesh.pos[b[2]]};
      if (boxesOverlap(A, B) && trianglesCross(A, B)) found = true;
    });
  }
  return found;
}

std::vector<uint32_t> crossingTriangles(const Mesh& mesh, const std::vector<uint32_t>* only) {
  if (only && only->size() * 20 < mesh.tri.size()) {
    // A few spots of a big mesh (the lattice repairs ask this many times over): only the
    // triangles in the grid cells those spots touch are sorted in, not the whole mesh.
    if (only->empty()) return {};
    double sum = 0;
    for (uint32_t t : *only)
      for (int i = 0; i < 3; ++i) sum += la::length(mesh.pos[mesh.tri[t][i]] - mesh.pos[mesh.tri[t][(i + 1) % 3]]);
    const double cell = std::max(2 * sum / (3.0 * only->size()), 1e-6);
    auto key = [&](int x, int y, int z) { return (uint64_t(uint32_t(x) & 0x1fffff) << 42) | (uint64_t(uint32_t(y) & 0x1fffff) << 21) | (uint32_t(z) & 0x1fffff); };
    auto range = [&](uint32_t t, int l[3], int h[3]) {
      const Tri& tri = mesh.tri[t];
      for (int axis = 0; axis < 3; ++axis) {
        const double mn = std::min({mesh.pos[tri[0]][axis], mesh.pos[tri[1]][axis], mesh.pos[tri[2]][axis]});
        const double mx = std::max({mesh.pos[tri[0]][axis], mesh.pos[tri[1]][axis], mesh.pos[tri[2]][axis]});
        l[axis] = static_cast<int>(std::floor(mn / cell));
        h[axis] = static_cast<int>(std::floor(mx / cell));
      }
    };
    // Triangles spanning many cells (a big flat one in a sculpt) are left to the whole-mesh check.
    auto giant = [](const int l[3], const int h[3]) { return (h[0] - l[0] + 1.0) * (h[1] - l[1] + 1.0) * (h[2] - l[2] + 1.0) > 512; };
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
    bool whole = false;
    for (uint32_t t : *only) {
      int l[3], h[3];
      range(t, l, h);
      if (giant(l, h)) {
        whole = true;
        break;
      }
      for (int z = l[2]; z <= h[2]; ++z)
        for (int y = l[1]; y <= h[1]; ++y)
          for (int x = l[0]; x <= h[0]; ++x) cells[key(x, y, z)];
    }
    for (uint32_t t = 0; t < mesh.tri.size() && !whole; ++t) {
      int l[3], h[3];
      range(t, l, h);
      if (giant(l, h)) {
        whole = true;
        break;
      }
      for (int z = l[2]; z <= h[2]; ++z)
        for (int y = l[1]; y <= h[1]; ++y)
          for (int x = l[0]; x <= h[0]; ++x) {
            const auto it = cells.find(key(x, y, z));
            if (it != cells.end()) it->second.push_back(t);
          }
    }
    std::vector<uint32_t> out;
    std::vector<uint32_t> tried;
    for (uint32_t t : *only) {
      if (whole) break;
      const Tri& a = mesh.tri[t];
      const vec3 A[3] = {mesh.pos[a[0]], mesh.pos[a[1]], mesh.pos[a[2]]};
      int l[3], h[3];
      range(t, l, h);
      tried.clear();
      for (int z = l[2]; z <= h[2]; ++z)
        for (int y = l[1]; y <= h[1]; ++y)
          for (int x = l[0]; x <= h[0]; ++x)
            for (uint32_t u : cells[key(x, y, z)]) {
              if (u == t || std::find(tried.begin(), tried.end(), u) != tried.end()) continue;
              tried.push_back(u);
              const Tri& b = mesh.tri[u];
              if (shareVertex(a, b)) continue;
              const vec3 B[3] = {mesh.pos[b[0]], mesh.pos[b[1]], mesh.pos[b[2]]};
              if (boxesOverlap(A, B) && trianglesCross(A, B)) {
                out.push_back(t);
                out.push_back(u);
              }
            }
    }
    if (!whole) {
      std::sort(out.begin(), out.end());
      out.erase(std::unique(out.begin(), out.end()), out.end());
      return out;
    }
  }
  StaticGrid grid;
  grid.build(mesh, averageEdge(mesh) * 2);
  const size_t count = only ? only->size() : mesh.tri.size();
  const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 32u);
  std::vector<std::vector<uint32_t>> found(threads);
  auto work = [&](unsigned slot) {
    for (size_t i = slot; i < count; i += threads) {
      const uint32_t t = only ? (*only)[i] : static_cast<uint32_t>(i);
      const Tri& a = mesh.tri[t];
      const vec3 A[3] = {mesh.pos[a[0]], mesh.pos[a[1]], mesh.pos[a[2]]};
      const vec3 lo = la::min(A[0], la::min(A[1], A[2])), hi = la::max(A[0], la::max(A[1], A[2]));
      grid.forBox(lo, hi, [&](uint32_t u) {
        if (u == t) return;
        const Tri& b = mesh.tri[u];
        if (shareVertex(a, b)) return;
        const vec3 B[3] = {mesh.pos[b[0]], mesh.pos[b[1]], mesh.pos[b[2]]};
        if (boxesOverlap(A, B) && trianglesCross(A, B)) {
          found[slot].push_back(t);
          if (only) found[slot].push_back(u);  // its partner may lie outside the list
        }
      });
    }
  };
  std::vector<std::thread> pool;
  for (unsigned i = 1; i < threads; ++i) pool.emplace_back(work, i);
  work(0);
  for (std::thread& t : pool) t.join();
  std::vector<uint32_t> out;
  for (const auto& list : found) out.insert(out.end(), list.begin(), list.end());
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

Mesh fromManifold(const manifold::Manifold& solid) {
  const manifold::MeshGL64 gl = solid.GetMeshGL64();
  const size_t stride = std::max<size_t>(3, gl.numProp);
  const size_t count = gl.vertProperties.size() / stride;
  std::vector<uint64_t> to(count);
  for (size_t i = 0; i < count; ++i) to[i] = i;
  for (size_t i = 0; i < gl.mergeFromVert.size() && i < gl.mergeToVert.size(); ++i) to[gl.mergeFromVert[i]] = gl.mergeToVert[i];
  std::vector<int64_t> remap(count, -1);
  Mesh m;
  for (size_t t = 0; t + 2 < gl.triVerts.size(); t += 3) {
    Tri tri;
    for (int i = 0; i < 3; ++i) {
      const uint64_t v = to[gl.triVerts[t + i]];
      if (remap[v] < 0) {
        remap[v] = static_cast<int64_t>(m.pos.size());
        m.pos.emplace_back(gl.vertProperties[v * stride], gl.vertProperties[v * stride + 1], gl.vertProperties[v * stride + 2]);
      }
      tri[i] = static_cast<uint32_t>(remap[v]);
    }
    if (tri[0] != tri[1] && tri[1] != tri[2] && tri[0] != tri[2]) m.tri.push_back(tri);
  }
  return m;
}

std::string stlBytes(const Mesh& mesh, const std::string& title) {
  std::string out(84 + mesh.tri.size() * 50, '\0');
  std::memcpy(&out[0], title.data(), std::min<size_t>(title.size(), 79));
  const uint32_t count = static_cast<uint32_t>(mesh.tri.size());
  std::memcpy(&out[80], &count, 4);
  size_t at = 84;
  for (const Tri& t : mesh.tri) {
    const vec3& a = mesh.pos[t[0]];
    const vec3& b = mesh.pos[t[1]];
    const vec3& c = mesh.pos[t[2]];
    vec3 n = triNormalOf(a, b, c);
    const double length = la::length(n);
    if (length > 0) n /= length;
    const float record[12] = {float(n.x), float(n.y), float(n.z), float(a.x), float(a.y), float(a.z),
                              float(b.x), float(b.y), float(b.z), float(c.x), float(c.y), float(c.z)};
    std::memcpy(&out[at], record, sizeof record);
    at += 50;
  }
  return out;
}

bool writeStl(const Mesh& mesh, const std::string& path, const std::string& title) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return false;
  const std::string data = stlBytes(mesh, title);
  file.write(data.data(), static_cast<std::streamsize>(data.size()));
  file.flush();
  return static_cast<bool>(file);
}

bool readStl(const std::string& path, Mesh* out, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error) *error = "The file could not be opened.";
    return false;
  }
  const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return parseStl(data, out, error);
}

bool parseStl(const std::string& data, Mesh* out, std::string* error) {
  auto fail = [&](const char* text) {
    if (error) *error = text;
    return false;
  };
  std::vector<float> corners;  // x y z per corner, three corners per triangle
  uint32_t count = 0;
  if (data.size() >= 84) std::memcpy(&count, data.data() + 80, 4);
  if (data.size() >= 84 && data.size() == 84 + size_t(count) * 50) {
    corners.reserve(size_t(count) * 9);
    for (uint32_t t = 0; t < count; ++t) {
      float record[12];
      std::memcpy(record, data.data() + 84 + size_t(t) * 50, sizeof record);
      corners.insert(corners.end(), record + 3, record + 12);
    }
  } else {
    // ASCII: every "vertex x y z".
    std::istringstream text(data);
    std::string word;
    while (text >> word)
      if (word == "vertex") {
        float x, y, z;
        if (!(text >> x >> y >> z)) return fail("The STL could not be read.");
        corners.insert(corners.end(), {x, y, z});
      }
    if (corners.size() % 9) return fail("The STL could not be read.");
  }
  if (corners.empty()) return fail("The STL holds no triangles.");
  struct Key {
    float x, y, z;
    bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
  };
  struct Hash {
    size_t operator()(const Key& k) const {
      uint32_t a, b, c;
      std::memcpy(&a, &k.x, 4);
      std::memcpy(&b, &k.y, 4);
      std::memcpy(&c, &k.z, 4);
      return (size_t(a) * 73856093u) ^ (size_t(b) * 19349663u) ^ (size_t(c) * 83492791u);
    }
  };
  std::unordered_map<Key, uint32_t, Hash> index;
  Mesh m;
  for (size_t i = 0; i + 8 < corners.size(); i += 9) {
    Tri tri;
    for (int k = 0; k < 3; ++k) {
      const Key key{corners[i + k * 3] + 0.0f, corners[i + k * 3 + 1] + 0.0f, corners[i + k * 3 + 2] + 0.0f};
      const auto it = index.find(key);
      if (it != index.end()) {
        tri[k] = it->second;
      } else {
        tri[k] = static_cast<uint32_t>(m.pos.size());
        index.emplace(key, tri[k]);
        m.pos.emplace_back(key.x, key.y, key.z);
      }
    }
    if (tri[0] != tri[1] && tri[1] != tri[2] && tri[0] != tri[2]) m.tri.push_back(tri);
  }
  // Some writers turn the triangles inside out: then turn them all around.
  if (volume(m) < 0)
    for (Tri& t : m.tri) std::swap(t[1], t[2]);
  *out = std::move(m);
  return true;
}

double remeshTriangleEstimate(const Mesh& in, double edge) {
  double area = 0;
  for (const Tri& t : in.tri) area += la::length(triNormalOf(in.pos[t[0]], in.pos[t[1]], in.pos[t[2]])) * 0.5;
  return area / (0.433 * edge * edge);
}

bool remesh(const Mesh& in, double edge, Mesh* out, std::string* error) {
  auto fail = [&](const char* text) {
    if (error) *error = text;
    return false;
  };
  if (in.tri.empty()) return fail("There is no surface to remesh.");
  if (!(edge > 0)) return fail("The detail must be more than 0 mm.");
  vec3 lo, hi;
  bounds(in, &lo, &hi);
  const vec3 size = hi - lo + vec3(4 * edge, 4 * edge, 4 * edge);
  if ((size.x / edge + 1) * (size.y / edge + 1) * (size.z / edge + 1) > 80e6)
    return fail("That detail is too fine for a part this big. Choose a larger detail (in mm).");
  if (remeshTriangleEstimate(in, edge) > 3e6) return fail("That would make more than 3 million triangles. Choose a larger detail (in mm).");

  // Distances only matter near the surface: the grid points either side of a crossing are less
  // than one spacing from it. Further out only the sign counts.
  const double band = 1.25 * edge;
  const double avg = averageEdge(in);
  StaticGrid grid;
  grid.build(in, std::max(band, avg * 1.5));
  ColumnGrid columns;
  columns.build(in, std::max(avg * 2, edge));
  auto sdf = [&](vec3 p) {
    const double d = nearestWithin(in, grid, p, band, nullptr);
    return columns.inside(in, p) ? d : -d;
  };
  const manifold::Box box(lo - vec3(2 * edge, 2 * edge, 2 * edge), hi + vec3(2 * edge, 2 * edge, 2 * edge));
  const manifold::Manifold solid = manifold::Manifold::LevelSet(sdf, box, edge, 0, -1);
  if (solid.Status() != manifold::Manifold::Error::NoError || solid.IsEmpty())
    return fail("The remesh came out empty. Try a smaller detail (in mm).");
  Mesh result = fromManifold(solid);
  if (!closedManifold(result)) return fail("The remesh did not come out closed.");

  // Even the triangles out (slide each point towards the middle of its neighbours, along the
  // surface) and put the points back on the surface they came from, so no detail is lost.
  Mesh relaxed = result;
  const auto ring = neighbours(relaxed);
  const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
  for (int pass = 0; pass < 3; ++pass) {
    const std::vector<vec3> normal = vertexNormals(relaxed);
    std::vector<vec3> next = relaxed.pos;
    auto work = [&](size_t from, size_t to) {
      for (size_t v = from; v < to; ++v) {
        if (ring[v].empty()) continue;
        vec3 mid(0, 0, 0);
        for (uint32_t u : ring[v]) mid += relaxed.pos[u];
        mid /= static_cast<double>(ring[v].size());
        const vec3 move = mid - relaxed.pos[v];
        vec3 p = relaxed.pos[v] + (move - normal[v] * la::dot(move, normal[v])) * 0.6;
        // Back onto the surface it came from, but only onto a face turned the same way. Along a
        // sharp edge the grid leaves teeth (a point on the edge, then one cut off, in turn), and
        // the nearest face flips between neighbours; those points are averaged right out
        // instead, which turns the teeth into one even, slightly rounded edge.
        vec3 onSurface;
        uint32_t t = 0;
        bool placed = false;
        if (nearestWithin(in, grid, p, edge * 0.75, &onSurface, &t) < edge * 0.75) {
          const Tri& tri = in.tri[t];
          const vec3 n = triNormalOf(in.pos[tri[0]], in.pos[tri[1]], in.pos[tri[2]]);
          const double l = la::length(n);
          if (l > 0 && la::dot(n / l, normal[v]) > 0.9) {
            p = onSurface;
            placed = true;
          }
        }
        if (!placed) p = relaxed.pos[v] + move * 0.6;
        next[v] = p;
      }
    };
    std::vector<std::thread> pool;
    const size_t n = relaxed.pos.size(), chunk = (n + threads - 1) / threads;
    for (unsigned i = 1; i < threads; ++i)
      if (i * chunk < n) pool.emplace_back(work, i * chunk, std::min(n, (i + 1) * chunk));
    work(0, std::min(n, chunk));
    for (std::thread& t : pool) t.join();
    relaxed.pos = std::move(next);
  }
  *out = selfIntersects(relaxed) ? std::move(result) : std::move(relaxed);
  return true;
}

struct SurfaceField::Impl {
  Mesh mesh;
  StaticGrid grid;
  ColumnGrid columns;
  double band = 1;
};

SurfaceField::SurfaceField(const Mesh& mesh, double band) : impl_(std::make_unique<Impl>()) {
  impl_->mesh = mesh;
  impl_->band = band;
  const double avg = averageEdge(mesh);
  impl_->grid.build(impl_->mesh, std::max(band, avg * 1.5));
  impl_->columns.build(impl_->mesh, std::max(avg * 2, band * 0.5));
}

SurfaceField::~SurfaceField() = default;

double SurfaceField::operator()(const vec3& p) const {
  const double d = nearestWithin(impl_->mesh, impl_->grid, p, impl_->band, nullptr);
  return impl_->columns.inside(impl_->mesh, p) ? d : -d;
}

double SurfaceField::operator()(const vec3& p, vec3* slope) const {
  vec3 q = p;
  uint32_t t = 0;
  const double d = nearestWithin(impl_->mesh, impl_->grid, p, impl_->band, &q, &t);
  const bool in = impl_->columns.inside(impl_->mesh, p);
  if (slope) {
    // Straight away from the nearest point (into the part when inside), or on the surface itself,
    // against the triangle's outward normal.
    if (d >= impl_->band) *slope = vec3(0, 0, 0);
    else if (d > 1e-9) *slope = (p - q) * ((in ? 1.0 : -1.0) / d);
    else {
      const Tri& tri = impl_->mesh.tri[t];
      const vec3 n = triNormalOf(impl_->mesh.pos[tri[0]], impl_->mesh.pos[tri[1]], impl_->mesh.pos[tri[2]]);
      const double l = la::length(n);
      *slope = l > 0 ? n * (-1.0 / l) : vec3(0, 0, 0);
    }
  }
  return in ? d : -d;
}

bool SurfaceField::inside(const vec3& p) const { return impl_->columns.inside(impl_->mesh, p); }

bool startShape(const std::string& kind, double size, double edge, Mesh* out, std::string* error) {
  using manifold::Manifold;
  size = std::max(size, 1.0);
  edge = std::clamp(edge, size / 400, size / 4);
  if (kind == "ball") {
    *out = icosphere(size / 2, edge);
    return true;
  }
  Manifold base;
  if (kind == "cube") {
    base = Manifold::Cube(vec3(size, size, size), true);
  } else if (kind == "cylinder") {
    base = Manifold::Cylinder(size, size / 2, -1, 256, true);
  } else if (kind == "slab") {
    base = Manifold::Cube(vec3(size, size, std::max(size / 8, 3 * edge)), true);
  } else if (kind == "capsule") {
    const double r = size / 4;
    base = Manifold::Hull({Manifold::Sphere(r, 128).Translate(vec3(0, 0, size / 2 - r)),
                           Manifold::Sphere(r, 128).Translate(vec3(0, 0, -(size / 2 - r)))});
  } else {
    if (error) *error = "Unknown start shape.";
    return false;
  }
  return remesh(fromManifold(base), edge, out, error);
}

// --- The sculptor -------------------------------------------------------------------------------

// Triangles by cell, kept up to date while points move.
struct Sculptor::Grid {
  double cell = 1;
  std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
  std::vector<std::array<int, 6>> range;
  int lo[3] = {0, 0, 0}, hi[3] = {-1, -1, -1};

  static uint64_t key(int x, int y, int z) {
    auto part = [](int v) { return uint64_t(uint32_t(v + (1 << 20)) & 0x1FFFFF); };
    return (part(x) << 42) | (part(y) << 21) | part(z);
  }
  int idx(double v) const { return static_cast<int>(std::floor(v / cell)); }
  std::array<int, 6> rangeOf(const Mesh& m, uint32_t t) const {
    const Tri& tri = m.tri[t];
    std::array<int, 6> r;
    for (int axis = 0; axis < 3; ++axis) {
      r[axis] = idx(std::min({m.pos[tri[0]][axis], m.pos[tri[1]][axis], m.pos[tri[2]][axis]}));
      r[axis + 3] = idx(std::max({m.pos[tri[0]][axis], m.pos[tri[1]][axis], m.pos[tri[2]][axis]}));
    }
    return r;
  }
  void insert(uint32_t t, const std::array<int, 6>& r) {
    for (int z = r[2]; z <= r[5]; ++z)
      for (int y = r[1]; y <= r[4]; ++y)
        for (int x = r[0]; x <= r[3]; ++x) cells[key(x, y, z)].push_back(t);
    for (int axis = 0; axis < 3; ++axis) {
      lo[axis] = std::min(lo[axis], r[axis]);
      hi[axis] = std::max(hi[axis], r[axis + 3]);
    }
  }
  void erase(uint32_t t, const std::array<int, 6>& r) {
    for (int z = r[2]; z <= r[5]; ++z)
      for (int y = r[1]; y <= r[4]; ++y)
        for (int x = r[0]; x <= r[3]; ++x) {
          auto it = cells.find(key(x, y, z));
          if (it == cells.end()) continue;
          auto& list = it->second;
          const auto at = std::find(list.begin(), list.end(), t);
          if (at != list.end()) {
            *at = list.back();
            list.pop_back();
          }
        }
  }
  void build(const Mesh& m, double wantedCell) {
    cell = std::max(wantedCell, 1e-6);
    cells.clear();
    range.resize(m.tri.size());
    lo[0] = lo[1] = lo[2] = std::numeric_limits<int>::max();
    hi[0] = hi[1] = hi[2] = std::numeric_limits<int>::min();
    for (uint32_t t = 0; t < m.tri.size(); ++t) {
      range[t] = rangeOf(m, t);
      insert(t, range[t]);
    }
  }
  void update(const Mesh& m, uint32_t t) {
    const auto r = rangeOf(m, t);
    if (r == range[t]) return;
    erase(t, range[t]);
    insert(t, r);
    range[t] = r;
  }
  template <typename F>
  void forBox(const vec3& a, const vec3& b, F&& f) const {
    int l[3], h[3];
    for (int axis = 0; axis < 3; ++axis) {
      l[axis] = std::max(idx(a[axis]), lo[axis]);
      h[axis] = std::min(idx(b[axis]), hi[axis]);
      if (l[axis] > h[axis]) return;
    }
    for (int z = l[2]; z <= h[2]; ++z)
      for (int y = l[1]; y <= h[1]; ++y)
        for (int x = l[0]; x <= h[0]; ++x) {
          const auto it = cells.find(key(x, y, z));
          if (it == cells.end()) continue;
          for (uint32_t t : it->second) f(t);
        }
  }
  double cellCount(const vec3& a, const vec3& b) const {
    double total = 1;
    for (int axis = 0; axis < 3; ++axis) total *= std::max(0, std::min(idx(b[axis]), hi[axis]) - std::max(idx(a[axis]), lo[axis]) + 1);
    return total;
  }
};

Sculptor::Sculptor() : grid_(std::make_unique<Grid>()) {}
Sculptor::~Sculptor() = default;

void Sculptor::setMesh(Mesh mesh) {
  mesh_ = std::move(mesh);
  mask_.assign(mesh_.pos.size(), 0.0f);
  undo_.clear();
  redo_.clear();
  rebuild();
}

void Sculptor::rebuild() {
  const size_t n = mesh_.pos.size();
  vtStart_.assign(n + 1, 0);
  for (const Tri& t : mesh_.tri)
    for (uint32_t v : t) ++vtStart_[v + 1];
  for (size_t i = 0; i < n; ++i) vtStart_[i + 1] += vtStart_[i];
  vt_.assign(vtStart_[n], 0);
  {
    std::vector<uint32_t> fill(vtStart_.begin(), vtStart_.end() - 1);
    for (uint32_t t = 0; t < mesh_.tri.size(); ++t)
      for (uint32_t v : mesh_.tri[t]) vt_[fill[v]++] = t;
  }
  const auto ring = neighbours(mesh_);
  vvStart_.assign(n + 1, 0);
  for (size_t i = 0; i < n; ++i) vvStart_[i + 1] = vvStart_[i] + static_cast<uint32_t>(ring[i].size());
  vv_.clear();
  vv_.reserve(vvStart_[n]);
  for (const auto& list : ring) vv_.insert(vv_.end(), list.begin(), list.end());
  normals_ = vertexNormals(mesh_);
  avgEdge_ = sculpt::averageEdge(mesh_);
  grid_->build(mesh_, avgEdge_ * 2);
  stamp_.assign(std::max(n, mesh_.tri.size()) + 1, 0);
  stampId_ = 0;
  strokeStamp_.assign(n, 0);
  strokeFrom_.assign(n, vec3(0, 0, 0));
  strokeNormal_.assign(n, vec3(0, 0, 1));
  mask_.resize(n, 0.0f);
  strokeId_ = 0;
  ++version_;
}

vec3 Sculptor::triNormal(uint32_t t) const {
  const Tri& tri = mesh_.tri[t];
  return triNormalOf(mesh_.pos[tri[0]], mesh_.pos[tri[1]], mesh_.pos[tri[2]]);
}

void Sculptor::updateNormals(const std::vector<uint32_t>& verts) {
  // The moved points and their neighbours: the triangles around both changed.
  if (++stampId_ == 0) {
    std::fill(stamp_.begin(), stamp_.end(), 0);
    stampId_ = 1;
  }
  std::vector<uint32_t> todo;
  for (uint32_t v : verts) {
    if (stamp_[v] != stampId_) {
      stamp_[v] = stampId_;
      todo.push_back(v);
    }
    for (uint32_t i = vvStart_[v]; i < vvStart_[v + 1]; ++i)
      if (stamp_[vv_[i]] != stampId_) {
        stamp_[vv_[i]] = stampId_;
        todo.push_back(vv_[i]);
      }
  }
  for (uint32_t v : todo) {
    vec3 sum(0, 0, 0);
    for (uint32_t i = vtStart_[v]; i < vtStart_[v + 1]; ++i) sum += triNormal(vt_[i]);
    const double l = la::length(sum);
    normals_[v] = l > 0 ? sum / l : vec3(0, 0, 1);
  }
}

bool Sculptor::raycast(const vec3& origin, const vec3& dirIn, vec3* hit, vec3* normal) const {
  if (mesh_.tri.empty() || grid_->hi[0] < grid_->lo[0]) return false;
  const double len = la::length(dirIn);
  if (len <= 0) return false;
  const vec3 d = dirIn / len;
  const double cell = grid_->cell;
  // Into the box of the occupied cells.
  double t0 = 0, t1 = kInf;
  for (int axis = 0; axis < 3; ++axis) {
    const double a = grid_->lo[axis] * cell, b = (grid_->hi[axis] + 1) * cell;
    if (std::abs(d[axis]) < 1e-15) {
      if (origin[axis] < a || origin[axis] > b) return false;
      continue;
    }
    double ta = (a - origin[axis]) / d[axis], tb = (b - origin[axis]) / d[axis];
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
  }
  if (t0 > t1) return false;
  const vec3 start = origin + d * (t0 + 1e-9 * cell);
  int c[3], step[3];
  double tMax[3], tDelta[3];
  for (int axis = 0; axis < 3; ++axis) {
    c[axis] = std::clamp(grid_->idx(start[axis]), grid_->lo[axis], grid_->hi[axis]);
    if (d[axis] > 0) {
      step[axis] = 1;
      tMax[axis] = ((c[axis] + 1) * cell - origin[axis]) / d[axis];
      tDelta[axis] = cell / d[axis];
    } else if (d[axis] < 0) {
      step[axis] = -1;
      tMax[axis] = (c[axis] * cell - origin[axis]) / d[axis];
      tDelta[axis] = -cell / d[axis];
    } else {
      step[axis] = 0;
      tMax[axis] = kInf;
      tDelta[axis] = kInf;
    }
  }
  if (++stampId_ == 0) {
    std::fill(stamp_.begin(), stamp_.end(), 0);
    stampId_ = 1;
  }
  double best = kInf;
  uint32_t bestTri = 0;
  for (int guard = 0; guard < 100000; ++guard) {
    const auto it = grid_->cells.find(Grid::key(c[0], c[1], c[2]));
    if (it != grid_->cells.end()) {
      for (uint32_t t : it->second) {
        if (stamp_[t] == stampId_) continue;
        stamp_[t] = stampId_;
        const Tri& tri = mesh_.tri[t];
        double th;
        if (rayTriangle(origin, d, mesh_.pos[tri[0]], mesh_.pos[tri[1]], mesh_.pos[tri[2]], kInf, &th) && th < best) {
          best = th;
          bestTri = t;
        }
      }
    }
    const int axis = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
    if (best <= tMax[axis]) break;
    c[axis] += step[axis];
    if (c[axis] < grid_->lo[axis] || c[axis] > grid_->hi[axis]) break;
    tMax[axis] += tDelta[axis];
  }
  if (best == kInf) return false;
  *hit = origin + d * best;
  if (normal) {
    const vec3 n = triNormal(bestTri);
    const double l = la::length(n);
    *normal = l > 0 ? n / l : vec3(0, 0, 1);
  }
  return true;
}

void Sculptor::gather(const vec3& center, double radius, const vec3& view, Region* region) const {
  region->verts.clear();
  region->weight.clear();
  region->center = center;
  const double r2 = radius * radius;
  auto consider = [&](uint32_t v) {
    const vec3 d = mesh_.pos[v] - center;
    const double dd = la::dot(d, d);
    if (dd >= r2) return;
    // Only the surface facing the viewer: the far side of a thin wall stays where it is (the
    // brush would otherwise push the floor of a slab out along with its top).
    const bool all = la::dot(view, view) == 0;
    const double facing = all ? 1.0 : -la::dot(normals_[v], view);
    if (facing <= -0.3) return;
    // The mask holds points still (except for the Mask brush itself, which paints it); a stamp
    // gives every point under the picture its full weight, the picture is the shape.
    const double held = stroke_.brush == Brush::Mask || mask_.empty() ? 0.0 : mask_[v];
    const double reach = stroke_.brush == Brush::Stamp ? 1.0 : falloff(stroke_.falloff, std::sqrt(dd) / radius);
    const double w = reach * std::min(1.0, (facing + 0.3) / 0.3) * (1.0 - held);
    if (w <= 0) return;
    region->verts.push_back(v);
    region->weight.push_back(w);
  };
  const vec3 r(radius, radius, radius);
  if (grid_->cellCount(center - r, center + r) * 4 > static_cast<double>(mesh_.pos.size())) {
    for (uint32_t v = 0; v < mesh_.pos.size(); ++v) consider(v);
  } else {
    if (++stampId_ == 0) {
      std::fill(stamp_.begin(), stamp_.end(), 0);
      stampId_ = 1;
    }
    grid_->forBox(center - r, center + r, [&](uint32_t t) {
      for (uint32_t v : mesh_.tri[t])
        if (stamp_[v] != stampId_) {
          stamp_[v] = stampId_;
          consider(v);
        }
    });
  }
  double total = 0;
  vec3 c(0, 0, 0), n(0, 0, 0);
  for (size_t i = 0; i < region->verts.size(); ++i) {
    total += region->weight[i];
    c += mesh_.pos[region->verts[i]] * region->weight[i];
    n += normals_[region->verts[i]] * region->weight[i];
  }
  region->areaCenter = total > 0 ? c / total : center;
  const double l = la::length(n);
  region->areaNormal = l > 0 ? n / l : -la::normalize(viewDir_);
}

bool dragBrush(Brush brush) { return brush == Brush::Grab || brush == Brush::SnakeHook || brush == Brush::Rotate; }

namespace {

// Rotation by `angle` radians about the unit axis (Rodrigues).
vec3 rotated(const vec3& v, const vec3& axis, double angle) {
  const double c = std::cos(angle), s = std::sin(angle);
  return v * c + la::cross(axis, v) * s + axis * (la::dot(axis, v) * (1 - c));
}

}  // namespace

std::vector<manifold::mat3> symmetries(const Stroke& stroke) {
  const manifold::mat3 identity{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  std::vector<manifold::mat3> out{identity};
  for (int axis = 0; axis < 3; ++axis) {
    if (!stroke.mirror[axis]) continue;
    manifold::mat3 flip = identity;
    flip[axis][axis] = -1;
    const size_t count = out.size();
    for (size_t i = 0; i < count; ++i) out.push_back(la::mul(flip, out[i]));
  }
  const int copies = std::clamp(stroke.radial, 1, 64);
  if (copies > 1) {
    const int axis = std::clamp(stroke.radialAxis, 0, 2);
    vec3 along(0, 0, 0);
    along[axis] = 1;
    const size_t count = out.size();
    for (int k = 1; k < copies; ++k) {
      const double angle = 2 * 3.14159265358979323846 * k / copies;
      const double c = std::cos(angle), s = std::sin(angle);
      // Rodrigues as a matrix (columns are the turned axes).
      manifold::mat3 turn;
      for (int col = 0; col < 3; ++col) {
        vec3 e(0, 0, 0);
        e[col] = 1;
        turn[col] = e * c + la::cross(along, e) * s + along * (la::dot(along, e) * (1 - c));
      }
      for (size_t i = 0; i < count; ++i) out.push_back(la::mul(turn, out[i]));
    }
  }
  return out;
}

double StampImage::sample(double x, double y) const {
  if (width <= 0 || height <= 0 || x < 0 || y < 0 || x > 1 || y > 1) return 0;
  const double fx = x * (width - 1), fy = y * (height - 1);
  const int x0 = std::clamp(static_cast<int>(fx), 0, width - 1), y0 = std::clamp(static_cast<int>(fy), 0, height - 1);
  const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
  const double tx = fx - x0, ty = fy - y0;
  auto at = [&](int xx, int yy) { return static_cast<double>(value[static_cast<size_t>(yy) * width + xx]); };
  return (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
}

vec3 Sculptor::place(const manifold::mat3& m, const vec3& p) const {
  return stroke_.mirrorCenter + la::mul(m, p - stroke_.mirrorCenter);
}

void Sculptor::touch(uint32_t v) {
  if (strokeStamp_[v] == strokeId_) return;
  strokeStamp_[v] = strokeId_;
  strokeFrom_[v] = mesh_.pos[v];
  strokeNormal_[v] = normals_[v];
  strokeVerts_.push_back(v);
}

void Sculptor::beginStroke(const Stroke& stroke, const vec3& hit, const vec3& viewDir) {
  stroke_ = stroke;
  stroke_.radius = std::max(stroke_.radius, 1e-3);
  stroke_.strength = std::clamp(stroke_.strength, 0.0, 1.0);
  viewDir_ = la::length(viewDir) > 0 ? la::normalize(viewDir) : vec3(0, 0, -1);
  stroking_ = true;
  heldBack_ = 0;
  if (++strokeId_ == 0) {
    std::fill(strokeStamp_.begin(), strokeStamp_.end(), 0);
    strokeId_ = 1;
  }
  strokeVerts_.clear();
  grabRegions_.clear();
  grabOrigin_.clear();
  lastHit_ = hit;
  snakeAt_ = hit;
  rotateSet_ = false;
  if (stroke_.brush == Brush::Mask) strokeMask_ = mask_;
  if (stroke_.brush == Brush::SnakeHook) {
    grabStart_ = hit;
    return;
  }
  if (stroke_.brush == Brush::Grab || stroke_.brush == Brush::Rotate) {
    grabStart_ = hit;
    for (const manifold::mat3& m : variants()) {
      Region region;
      gather(place(m, hit), stroke_.radius, la::mul(m, viewDir_), &region);
      region.xf = m;
      for (uint32_t v : region.verts) grabOrigin_.push_back(mesh_.pos[v]);
      grabRegions_.push_back(std::move(region));
    }
    return;
  }
  applyDab(hit);
}

void Sculptor::dab(const vec3& hit) {
  if (!stroking_ || dragBrush(stroke_.brush)) return;
  applyDab(hit);
  lastHit_ = hit;
}

void Sculptor::grabTo(const vec3& planePoint) {
  if (!stroking_ || !dragBrush(stroke_.brush)) return;
  auto addMove = [](std::vector<std::pair<uint32_t, vec3>>& moves, std::unordered_map<uint32_t, size_t>& where, uint32_t v,
                    const vec3& from, const vec3& shift) {
    const auto it = where.find(v);
    if (it == where.end()) {
      where.emplace(v, moves.size());
      moves.emplace_back(v, from + shift);
    } else {
      moves[it->second].second += shift;
    }
  };
  if (stroke_.brush == Brush::SnakeHook) {
    // The hook pulls what is under it along, and moves on with it: in short steps, so a fast drag
    // draws out a smooth horn rather than tearing off a lump.
    const vec3 total = planePoint - snakeAt_;
    const double step = stroke_.radius * 0.2;
    const int steps = std::max(1, static_cast<int>(std::ceil(la::length(total) / step)));
    for (int i = 0; i < steps; ++i) {
      const vec3 delta = total / static_cast<double>(steps);
      std::vector<std::pair<uint32_t, vec3>> moves;
      std::unordered_map<uint32_t, size_t> where;
      for (const manifold::mat3& m : variants()) {
        Region region;
        gather(place(m, snakeAt_), stroke_.radius, vec3(0, 0, 0), &region);
        const vec3 shift = la::mul(m, delta) * (0.5 + 0.5 * stroke_.strength);
        for (size_t k = 0; k < region.verts.size(); ++k)
          addMove(moves, where, region.verts[k], mesh_.pos[region.verts[k]], shift * region.weight[k]);
      }
      if (!commitMoves(moves)) break;  // held back: the hook stays where the surface is
      snakeAt_ += delta;
    }
    return;
  }
  std::vector<std::pair<uint32_t, vec3>> moves;
  std::unordered_map<uint32_t, size_t> where;
  size_t k = 0;
  if (stroke_.brush == Brush::Rotate) {
    // Turn the spot about the line of sight through the first point, by the angle the mouse has
    // gone round it.
    const vec3 axis = -viewDir_;
    vec3 now = planePoint - grabStart_;
    now -= axis * la::dot(now, axis);
    if (!rotateSet_) {
      if (la::length(now) < stroke_.radius * 0.15) return;
      rotateFrom_ = la::normalize(now);
      rotateSet_ = true;
      return;
    }
    if (la::length(now) < 1e-9) return;
    now = la::normalize(now);
    const double angle = std::atan2(la::dot(la::cross(rotateFrom_, now), axis), la::dot(rotateFrom_, now));
    for (const Region& region : grabRegions_) {
      const vec3 centre = place(region.xf, grabStart_);
      const manifold::mat3 back = la::transpose(region.xf);
      for (size_t i = 0; i < region.verts.size(); ++i, ++k) {
        // Turned in the frame of the stroke itself, then carried over to this copy (a mirrored
        // copy turns the other way).
        const vec3 local = la::mul(back, grabOrigin_[k] - centre);
        const vec3 shift = la::mul(region.xf, rotated(local, axis, angle * region.weight[i]) - local);
        addMove(moves, where, region.verts[i], grabOrigin_[k], shift);
      }
    }
    commitMoves(moves);
    return;
  }
  // Grab: each point to where its start position plus the drag takes it (summed where copies of
  // the brush overlap).
  const vec3 delta = planePoint - grabStart_;
  for (const Region& region : grabRegions_) {
    const vec3 shift = la::mul(region.xf, delta);
    for (size_t i = 0; i < region.verts.size(); ++i, ++k)
      addMove(moves, where, region.verts[i], grabOrigin_[k], shift * region.weight[i]);
  }
  commitMoves(moves);
}

void Sculptor::applyDab(const vec3& hit) {
  const double R = stroke_.radius;
  const double s = stroke_.strength;
  const double sign = stroke_.invert ? -1.0 : 1.0;
  for (const manifold::mat3& mirror : variants()) {
    Region region;
    const vec3 center = place(mirror, hit);
    const double reach = stroke_.brush == Brush::Stamp ? R * 1.415 : R;  // a stamp is square
    gather(center, reach, la::mul(mirror, viewDir_), &region);
    if (region.verts.empty()) continue;
    const vec3 n = region.areaNormal;
    std::vector<std::pair<uint32_t, vec3>> moves;
    moves.reserve(region.verts.size());
    auto smoothOnce = [&](double amount) {
      moves.clear();
      for (size_t i = 0; i < region.verts.size(); ++i) {
        const uint32_t v = region.verts[i];
        const uint32_t count = vvStart_[v + 1] - vvStart_[v];
        if (!count) continue;
        vec3 mid(0, 0, 0);
        for (uint32_t j = vvStart_[v]; j < vvStart_[v + 1]; ++j) mid += mesh_.pos[vv_[j]];
        mid /= static_cast<double>(count);
        moves.emplace_back(v, mesh_.pos[v] + (mid - mesh_.pos[v]) * (region.weight[i] * amount));
      }
      commitMoves(moves);
    };
    switch (stroke_.brush) {
      case Brush::Draw:
        for (size_t i = 0; i < region.verts.size(); ++i)
          moves.emplace_back(region.verts[i], mesh_.pos[region.verts[i]] + n * (sign * R * 0.05 * s * region.weight[i]));
        commitMoves(moves);
        break;
      case Brush::Inflate:
        for (size_t i = 0; i < region.verts.size(); ++i) {
          const uint32_t v = region.verts[i];
          moves.emplace_back(v, mesh_.pos[v] + normals_[v] * (sign * R * 0.04 * s * region.weight[i]));
        }
        commitMoves(moves);
        break;
      case Brush::Clay: {
        // A plane a little above the surface under the brush; what lies under it comes up to it.
        const vec3 plane = region.areaCenter + n * (sign * R * 0.12 * s);
        for (size_t i = 0; i < region.verts.size(); ++i) {
          const uint32_t v = region.verts[i];
          const double below = la::dot(plane - mesh_.pos[v], n) * sign;
          if (below > 0) moves.emplace_back(v, mesh_.pos[v] + n * (sign * below * region.weight[i] * 0.5));
        }
        commitMoves(moves);
        break;
      }
      case Brush::Flatten:
        for (size_t i = 0; i < region.verts.size(); ++i) {
          const uint32_t v = region.verts[i];
          const double off = la::dot(region.areaCenter - mesh_.pos[v], n);
          moves.emplace_back(v, mesh_.pos[v] + n * (sign * off * region.weight[i] * s * 0.5));
        }
        commitMoves(moves);
        break;
      case Brush::Pinch:
        for (size_t i = 0; i < region.verts.size(); ++i) {
          const uint32_t v = region.verts[i];
          vec3 to = center - mesh_.pos[v];
          to -= n * la::dot(to, n);
          moves.emplace_back(v, mesh_.pos[v] + to * (sign * region.weight[i] * s * 0.25));
        }
        commitMoves(moves);
        break;
      case Brush::Smooth: {
        const int passes = 1 + static_cast<int>(s * 2.999);
        for (int pass = 0; pass < passes; ++pass) smoothOnce(std::min(1.0, 0.35 + s * 0.6));
        break;
      }
      case Brush::Layer: {
        // Up to a set height above where the stroke found each point, and no further, however
        // often the brush passes: an even plateau.
        const double height = R * 0.3 * s;
        for (size_t i = 0; i < region.verts.size(); ++i) {
          const uint32_t v = region.verts[i];
          const bool seen = strokeStamp_[v] == strokeId_;
          const vec3 from = seen ? strokeFrom_[v] : mesh_.pos[v];
          const vec3 up = seen ? strokeNormal_[v] : normals_[v];
          const double now = la::dot(mesh_.pos[v] - from, up) * sign;
          const double cap = height * region.weight[i];
          if (now >= cap) continue;
          const double next = std::min(cap, now + height * 0.35 * region.weight[i] + 1e-12);
          moves.emplace_back(v, mesh_.pos[v] + up * ((next - now) * sign));
        }
        commitMoves(moves);
        break;
      }
      case Brush::Blob:
        // Out along the surface and away from the middle at once: round, bulging blobs.
        for (size_t i = 0; i < region.verts.size(); ++i) {
          const uint32_t v = region.verts[i];
          vec3 away = mesh_.pos[v] - center;
          away -= n * la::dot(away, n);
          const double l = la::length(away);
          const vec3 out = n * 0.75 + (l > 1e-9 ? away / l * (0.5 * std::min(1.0, l / (R * 0.5))) : vec3(0, 0, 0));
          moves.emplace_back(v, mesh_.pos[v] + out * (sign * R * 0.045 * s * region.weight[i]));
        }
        commitMoves(moves);
        break;
      case Brush::Crease:
        // In along the surface and pinched together: a sharp groove (Ctrl: a sharp ridge).
        for (size_t i = 0; i < region.verts.size(); ++i) {
          const uint32_t v = region.verts[i];
          vec3 to = center - mesh_.pos[v];
          to -= n * la::dot(to, n);
          const double w = region.weight[i];
          moves.emplace_back(v, mesh_.pos[v] - n * (sign * R * 0.035 * s * w * w) + to * (w * s * 0.18));
        }
        commitMoves(moves);
        break;
      case Brush::Fill:
      case Brush::Scrape: {
        // The plane through the surface under the brush. Fill lifts what lies below it (Ctrl:
        // deepens it); Scrape shaves off what stands above it (Ctrl: raises it further).
        const bool fill = stroke_.brush == Brush::Fill;
        for (size_t i = 0; i < region.verts.size(); ++i) {
          const uint32_t v = region.verts[i];
          const double above = la::dot(mesh_.pos[v] - region.areaCenter, n);
          if (fill ? above >= 0 : above <= 0) continue;
          const double amount = std::min(std::abs(above), R * 0.25) * region.weight[i] * s * 0.5;
          const double direction = (fill ? 1.0 : -1.0) * (stroke_.invert ? -1.0 : 1.0);
          moves.emplace_back(v, mesh_.pos[v] + n * (direction * amount));
        }
        commitMoves(moves);
        break;
      }
      case Brush::Nudge: {
        // Along the way the stroke moves, over the surface.
        vec3 along = la::mul(mirror, hit - lastHit_);
        along -= n * la::dot(along, n);
        if (la::length(along) <= 0) break;
        for (size_t i = 0; i < region.verts.size(); ++i)
          moves.emplace_back(region.verts[i], mesh_.pos[region.verts[i]] + along * (region.weight[i] * s));
        commitMoves(moves);
        break;
      }
      case Brush::Mask: {
        // Paints (Ctrl: wipes) the mask; nothing moves.
        for (size_t i = 0; i < region.verts.size(); ++i) {
          float& m = mask_[region.verts[i]];
          m = static_cast<float>(std::clamp(m + sign * region.weight[i] * (0.25 + 0.75 * s), 0.0, 1.0));
        }
        ++version_;
        break;
      }
      case Brush::Stamp: {
        // The picture, laid flat on the surface under the brush (upright as on the screen), lifts
        // each point to its brightness times the height. Like Layer, no higher within one stroke,
        // so a dragged row of stamps does not pile up where two overlap.
        if (!stroke_.stamp || stroke_.stamp->width <= 0) break;
        const double height = R * 0.5 * s;
        vec3 up = la::mul(mirror, stroke_.up);
        up -= n * la::dot(up, n);
        if (la::length(up) < 1e-6) {
          up = std::abs(n.z) < 0.9 ? la::cross(n, vec3(0, 0, 1)) : la::cross(n, vec3(1, 0, 0));
        }
        up = la::normalize(up);
        // Mirrored copies get a mirrored picture, as a mirror would show it.
        const vec3 right = la::normalize(la::cross(up, n)) * (la::determinant(mirror) < 0 ? -1.0 : 1.0);
        for (size_t i = 0; i < region.verts.size(); ++i) {
          const uint32_t v = region.verts[i];
          const bool seen = strokeStamp_[v] == strokeId_;
          const vec3 from = seen ? strokeFrom_[v] : mesh_.pos[v];
          const vec3 d = from - center;
          const double x = la::dot(d, right) / R, y = la::dot(d, up) / R;
          if (std::abs(x) > 1 || std::abs(y) > 1) continue;
          const double target = stroke_.stamp->sample((x + 1) * 0.5, (1 - y) * 0.5) * height * region.weight[i];
          const double now = la::dot(mesh_.pos[v] - from, n) * sign;
          if (now >= target) continue;
          moves.emplace_back(v, mesh_.pos[v] + n * ((target - now) * sign));
        }
        commitMoves(moves);
        break;
      }
      case Brush::Grab:
      case Brush::SnakeHook:
      case Brush::Rotate:
        break;
    }
  }
}

bool Sculptor::movesOk(const std::vector<uint32_t>& moved, const std::vector<uint32_t>& tris) {
  (void)moved;
  // No triangle collapsed, and none of them cuts through another one.
  for (uint32_t t : tris)
    if (la::length(triNormal(t)) <= 1e-10 * avgEdge_ * avgEdge_) return false;
  if (++stampId_ == 0) {
    std::fill(stamp_.begin(), stamp_.end(), 0);
    stampId_ = 1;
  }
  const uint32_t affected = stampId_;
  for (uint32_t t : tris) stamp_[t] = affected;
  std::vector<uint32_t> seen;
  for (uint32_t t : tris) {
    const Tri& a = mesh_.tri[t];
    const vec3 A[3] = {mesh_.pos[a[0]], mesh_.pos[a[1]], mesh_.pos[a[2]]};
    const vec3 lo = la::min(A[0], la::min(A[1], A[2])), hi = la::max(A[0], la::max(A[1], A[2]));
    bool bad = false;
    seen.clear();
    grid_->forBox(lo, hi, [&](uint32_t u) {
      if (bad || u == t) return;
      if (stamp_[u] == affected && u < t) return;  // that pair was tested from u's side
      const Tri& b = mesh_.tri[u];
      if (shareVertex(a, b)) return;
      const vec3 B[3] = {mesh_.pos[b[0]], mesh_.pos[b[1]], mesh_.pos[b[2]]};
      if (boxesOverlap(A, B) && trianglesCross(A, B)) bad = true;
    });
    if (bad) return false;
  }
  return true;
}

bool Sculptor::commitMoves(const std::vector<std::pair<uint32_t, vec3>>& moves) {
  if (moves.empty()) return true;
  std::vector<uint32_t> moved;
  std::vector<vec3> before, target;
  moved.reserve(moves.size());
  for (const auto& [v, p] : moves) {
    touch(v);
    moved.push_back(v);
    before.push_back(mesh_.pos[v]);
    target.push_back(p);
  }
  // The triangles around the moved points, and how each faced before.
  std::vector<uint32_t> tris;
  if (++stampId_ == 0) {
    std::fill(stamp_.begin(), stamp_.end(), 0);
    stampId_ = 1;
  }
  for (uint32_t v : moved)
    for (uint32_t i = vtStart_[v]; i < vtStart_[v + 1]; ++i)
      if (stamp_[vt_[i]] != stampId_) {
        stamp_[vt_[i]] = stampId_;
        tris.push_back(vt_[i]);
      }
  std::vector<vec3> faced(tris.size());
  for (size_t i = 0; i < tris.size(); ++i) faced[i] = triNormal(tris[i]);

  auto place = [&](double fraction) {
    for (size_t i = 0; i < moved.size(); ++i) mesh_.pos[moved[i]] = before[i] + (target[i] - before[i]) * fraction;
    for (uint32_t t : tris) grid_->update(mesh_, t);
  };
  auto ok = [&] {
    for (size_t i = 0; i < tris.size(); ++i) {
      const vec3 now = triNormal(tris[i]);
      // Turned more than a right angle in one dab: folded over.
      if (la::dot(now, faced[i]) <= 0) return false;
    }
    return movesOk(moved, tris);
  };
  bool done = false;
  for (double fraction : {1.0, 0.5, 0.25, 0.125}) {
    place(fraction);
    if (ok()) {
      done = true;
      if (fraction < 1) ++heldBack_;
      break;
    }
  }
  if (!done) {
    place(0);
    ++heldBack_;
  }
  updateNormals(moved);
  ++version_;
  return done;
}

void Sculptor::endStroke() {
  if (!stroking_) return;
  stroking_ = false;
  if (stroke_.brush == Brush::Mask) {
    if (strokeMask_ != mask_) setMaskStep(std::move(strokeMask_));
    strokeMask_.clear();
    return;
  }
  Step step;
  for (uint32_t v : strokeVerts_) {
    if (strokeFrom_[v] == mesh_.pos[v]) continue;
    step.idx.push_back(v);
    step.from.push_back(strokeFrom_[v]);
    step.to.push_back(mesh_.pos[v]);
  }
  strokeVerts_.clear();
  grabRegions_.clear();
  grabOrigin_.clear();
  if (!step.idx.empty()) pushStep(std::move(step));
}

void Sculptor::pushStep(Step step) {
  undo_.push_back(std::move(step));
  redo_.clear();
  // Keep the last 60 strokes, and only the last 6 remeshes (those hold whole meshes).
  int topology = 0;
  for (auto it = undo_.rbegin(); it != undo_.rend(); ++it)
    if (it->topology && ++topology > 6) {
      undo_.erase(undo_.begin(), std::next(it).base() + 1);
      break;
    }
  if (undo_.size() > 60) undo_.erase(undo_.begin(), undo_.begin() + static_cast<long>(undo_.size() - 60));
}

bool Sculptor::remeshTo(double edge, std::string* error) {
  Mesh out;
  if (!remesh(mesh_, edge, &out, error)) return false;
  // The mask carries over: each new point takes the mask where it lies on the old surface.
  std::vector<float> carried(out.pos.size(), 0.0f);
  if (hasMask()) {
    StaticGrid grid;
    grid.build(mesh_, std::max(avgEdge_ * 2, edge));
    const double band = std::max(avgEdge_, edge) * 2;
    for (size_t v = 0; v < out.pos.size(); ++v) {
      vec3 q;
      uint32_t t = 0;
      if (nearestWithin(mesh_, grid, out.pos[v], band, &q, &t) >= band) continue;
      const Tri& tri = mesh_.tri[t];
      const vec3 a = mesh_.pos[tri[0]], b = mesh_.pos[tri[1]], c = mesh_.pos[tri[2]];
      const vec3 nrm = la::cross(b - a, c - a);
      const double area = la::dot(nrm, nrm);
      if (area <= 0) continue;
      const double wa = la::dot(la::cross(c - b, q - b), nrm) / area;
      const double wb = la::dot(la::cross(a - c, q - c), nrm) / area;
      const double wc = 1 - wa - wb;
      carried[v] = static_cast<float>(std::clamp(wa * mask_[tri[0]] + wb * mask_[tri[1]] + wc * mask_[tri[2]], 0.0, 1.0));
    }
  }
  Step step;
  step.topology = true;
  step.before = mesh_;
  step.after = out;
  step.maskBefore = mask_;
  step.maskAfter = carried;
  mesh_ = std::move(out);
  rebuild();
  mask_ = std::move(carried);
  pushStep(std::move(step));
  return true;
}

bool Sculptor::hasMask() const {
  for (float m : mask_)
    if (m > 0) return true;
  return false;
}

void Sculptor::setMaskStep(std::vector<float> before) {
  Step step;
  step.maskOnly = true;
  step.maskBefore = std::move(before);
  step.maskAfter = mask_;
  pushStep(std::move(step));
  ++version_;
}

void Sculptor::clearMask() {
  if (!hasMask()) return;
  std::vector<float> before = mask_;
  std::fill(mask_.begin(), mask_.end(), 0.0f);
  setMaskStep(std::move(before));
}

void Sculptor::invertMask() {
  std::vector<float> before = mask_;
  for (float& m : mask_) m = 1.0f - m;
  setMaskStep(std::move(before));
}

bool Sculptor::undo() {
  if (undo_.empty() || stroking_) return false;
  Step step = std::move(undo_.back());
  undo_.pop_back();
  if (step.maskOnly) {
    mask_ = step.maskBefore;
    ++version_;
  } else if (step.topology) {
    mesh_ = step.before;
    rebuild();
    mask_ = step.maskBefore;
  } else {
    for (size_t i = 0; i < step.idx.size(); ++i) mesh_.pos[step.idx[i]] = step.from[i];
    std::vector<uint32_t> tris;
    for (uint32_t v : step.idx)
      for (uint32_t i = vtStart_[v]; i < vtStart_[v + 1]; ++i) tris.push_back(vt_[i]);
    for (uint32_t t : tris) grid_->update(mesh_, t);
    updateNormals(step.idx);
    ++version_;
  }
  redo_.push_back(std::move(step));
  return true;
}

bool Sculptor::redo() {
  if (redo_.empty() || stroking_) return false;
  Step step = std::move(redo_.back());
  redo_.pop_back();
  if (step.maskOnly) {
    mask_ = step.maskAfter;
    ++version_;
  } else if (step.topology) {
    mesh_ = step.after;
    rebuild();
    mask_ = step.maskAfter;
  } else {
    for (size_t i = 0; i < step.idx.size(); ++i) mesh_.pos[step.idx[i]] = step.to[i];
    std::vector<uint32_t> tris;
    for (uint32_t v : step.idx)
      for (uint32_t i = vtStart_[v]; i < vtStart_[v + 1]; ++i) tris.push_back(vt_[i]);
    for (uint32_t t : tris) grid_->update(mesh_, t);
    updateNormals(step.idx);
    ++version_;
  }
  undo_.push_back(std::move(step));
  return true;
}

}  // namespace sculpt
