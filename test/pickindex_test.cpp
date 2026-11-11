// The pick index finds the same first triangle as testing every one, finds edge neighbours, and
// is quick on a big mesh.
#include "evaluator.h"
#include "pickindex.h"

#include <manifold/manifold.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}
bool brute(const CpuMesh& m, const float o[3], const float d[3], float* best, int* tri) {
  *best = 1e30f;
  *tri = -1;
  for (size_t t = 0; t < m.triCount(); ++t) {
    const float* a = m.corner(t, 0);
    const float* b = m.corner(t, 1);
    const float* c = m.corner(t, 2);
    const double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const double p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::abs(det) < 1e-12) continue;
    const double s[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
    const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) / det;
    if (u < 0 || u > 1) continue;
    const double q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
    if (v < 0 || u + v > 1) continue;
    const double t2 = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
    if (t2 >= 0 && t2 < *best) *best = static_cast<float>(t2), *tri = static_cast<int>(t);
  }
  return *tri >= 0;
}
// The solid as a draw mesh (positions only, so MeshGL has no duplicated corners).
CpuMesh meshFrom(const manifold::Manifold& solid) {
  const manifold::MeshGL gl = solid.GetMeshGL();
  CpuMesh m;
  for (size_t i = 0; i < gl.vertProperties.size(); i += gl.numProp)
    for (int a = 0; a < 3; ++a) m.vertices.push_back(gl.vertProperties[i + a]);
  m.indices.assign(gl.triVerts.begin(), gl.triVerts.end());
  m.triangles = static_cast<int>(m.indices.size() / 3);
  m.updateBounds();
  return m;
}
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

int main() {
  // A lumpy solid: a sphere minus a box plus a cylinder, so rays hit several layers.
  const manifold::Manifold shape = (manifold::Manifold::Sphere(20, 96) - manifold::Manifold::Cube({18, 18, 18}).Translate({0, 0, 4})) +
                                   manifold::Manifold::Cylinder(50, 5, 5, 48, true);
  const CpuMesh mesh = meshFrom(shape);
  const PickIndex index(mesh);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(-1, 1);
  int agree = 0, hits = 0;
  for (int i = 0; i < 2000; ++i) {
    const float o[3] = {u(rng) * 60, u(rng) * 60, u(rng) * 60};
    const float aim[3] = {u(rng) * 10, u(rng) * 10, u(rng) * 10};
    const float d[3] = {aim[0] - o[0], aim[1] - o[1], aim[2] - o[2]};  // not unit length on purpose
    float tb = 0, ti = 0;
    int bb = -1, bi = -1;
    const bool hb = brute(mesh, o, d, &tb, &bb);
    const bool hi = index.cast(o, d, 1e30f, &ti, &bi);
    hits += hb;
    if (hb == hi && (!hb || std::abs(tb - ti) < 1e-4f * std::max(1.f, tb))) ++agree;
  }
  std::printf("pick index: %d of 2000 rays agree (%d hits) on %zu triangles\n", agree, hits, mesh.triCount());
  check(agree >= 1998, "the index finds the same first hit as testing every triangle");
  check(hits > 500, "enough rays hit to mean something");
  // Neighbours: a closed mesh has exactly three across its edges for every triangle.
  std::vector<int> next;
  size_t three = 0;
  for (size_t t = 0; t < mesh.triCount(); ++t) {
    index.neighbours(static_cast<int>(t), &next);
    three += next.size() == 3;
  }
  check(three == mesh.triCount(), "every triangle of a closed mesh has three edge neighbours");
  // Big: a 2-million-triangle sphere, built and asked quickly.
  const CpuMesh big = meshFrom(manifold::Manifold::Sphere(30, 2000));
  double t0 = now();
  const PickIndex bigIndex(big);
  const double built = now() - t0;
  t0 = now();
  int found = 0;
  for (int i = 0; i < 10000; ++i) {
    const float o[3] = {u(rng) * 20, u(rng) * 20, 100};
    const float d[3] = {0, 0, -1};
    float t = 0;
    int tri = -1;
    found += bigIndex.cast(o, d, 1e30f, &t, &tri);
  }
  const double asked = (now() - t0) / 10000;
  std::printf("2M triangles (%zu): index built in %.2f s, %.1f microseconds a ray (%d hits)\n", big.triCount(), built,
              asked * 1e6, found);
  check(asked < 50e-6, "a ray on 2M triangles takes under 50 microseconds");
  std::printf(failures ? "%d failure(s)\n" : "all pick index checks passed\n", failures);
  return failures ? 1 : 0;
}
