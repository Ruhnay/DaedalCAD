// Times the heavy geometry work (booleans, field meshing, simplify, picking) so builds with different
// compiler settings (say, with and without AVX2) can be compared on the same machine. Each job runs
// a few times; the fastest run counts (the least disturbed by anything else on the machine).
// Build with -DDAEDALCAD_BENCHMARK=ON; run daedal_benchmark [runs] [only jobs containing this].
#include "effects.h"
#include "evaluator.h"
#include "lattice.h"
#include "pickindex.h"
#include "sculpt.h"

#include <manifold/manifold.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

using manifold::Manifold;

namespace {
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

double best(int runs, const std::function<void()>& job) {
  double fastest = 1e30;
  for (int i = 0; i < runs; ++i) {
    const double t0 = now();
    job();
    fastest = std::min(fastest, now() - t0);
  }
  return fastest;
}

CpuMesh drawMesh(const Manifold& solid) {
  const manifold::MeshGL gl = solid.GetMeshGL();
  CpuMesh m;
  for (size_t i = 0; i < gl.vertProperties.size(); i += gl.numProp)
    for (int a = 0; a < 3; ++a) m.vertices.push_back(gl.vertProperties[i + a]);
  m.indices.assign(gl.triVerts.begin(), gl.triVerts.end());
  m.triangles = static_cast<int>(m.indices.size() / 3);
  m.updateBounds();
  return m;
}

volatile double sink = 0;  // keeps results from being optimised away
}  // namespace

int main(int argc, char** argv) {
  const int runs = argc > 1 ? std::max(1, std::atoi(argv[1])) : 3;
  const std::string only = argc > 2 ? argv[2] : "";  // run only the jobs whose name contains this
  struct Row {
    std::string name;
    double seconds;
  };
  std::vector<Row> rows;
  auto run = [&](const std::string& name, const std::function<void()>& job) {
    if (!only.empty() && name.find(only) == std::string::npos) return;
    const double s = best(runs, job);
    rows.push_back({name, s});
    std::printf("%-44s %8.3f s\n", name.c_str(), s);
    std::fflush(stdout);
  };

  // Booleans: a ball drilled by 60 holes; 300 small balls joined; a big sculpt-like mesh with holes.
  run("boolean: ball minus 60 cylinders", [] {
    std::vector<Manifold> holes;
    for (int i = 0; i < 60; ++i) {
      const double a = i * 2.39996, z = -30 + i;
      holes.push_back(Manifold::Cylinder(120, 2.5, 2.5, 48, true).Rotate(90, 0, a * 57.3).Translate({0, 0, z}));
    }
    const Manifold r = Manifold::Sphere(40, 400) - Manifold::BatchBoolean(holes, manifold::OpType::Add);
    sink = sink + r.Volume();
  });
  run("boolean: 300 balls joined", [] {
    std::vector<Manifold> balls;
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(-30, 30);
    for (int i = 0; i < 300; ++i) balls.push_back(Manifold::Sphere(6, 48).Translate({u(rng), u(rng), u(rng)}));
    sink = sink + Manifold::BatchBoolean(balls, manifold::OpType::Add).Volume();
  });
  const Manifold sculptLike = Manifold::Sphere(35, 1500);
  run("boolean: 1.1M-triangle part minus 3 holes", [&] {
    Manifold r = sculptLike;
    for (int i = 0; i < 3; ++i) r = r - Manifold::Cylinder(90, 6, 6, 64, true).Translate({-15.0 + 15 * i, 0, 0});
    sink = sink + r.Volume();
  });
  // Field meshing: a gyroid lattice, a knurled cylinder, a smooth join.
  const sculpt::Mesh block = sculpt::fromManifold(Manifold::Cube({40, 40, 40}, true));
  run("lattice: gyroid in a 40 mm cube", [&] {
    lattice::Settings s;
    s.cell = 8;
    s.wall = 1.2;
    sculpt::Mesh out;
    std::string error;
    lattice::build(block, s, &out, &error);
    sink = sink + out.tri.size();
  });
  const sculpt::Mesh can = sculpt::fromManifold(Manifold::Cylinder(30, 12, 12, 96));
  run("texture: knurling round a cylinder", [&] {
    fx::TextureSettings s;
    s.kind = fx::Texture::Knurl;
    s.wrap = 1;
    s.size = 3;
    s.depth = 0.6;
    sculpt::Mesh out;
    std::string error;
    fx::texture(can, s, &out, &error);
    sink = sink + out.tri.size();
  });
  const std::vector<sculpt::Mesh> parts = {sculpt::fromManifold(Manifold::Cube({40, 40, 8}, true)),
                                           sculpt::fromManifold(Manifold::Cylinder(30, 7, 7, 64).Translate({0, 0, 3})),
                                           sculpt::fromManifold(Manifold::Sphere(10, 96).Translate({0, 0, 36}))};
  run("smooth join: slab, post and ball", [&] {
    sculpt::Mesh out;
    std::string error;
    fx::blend(parts, {}, 4, &out, &error);
    sink = sink + out.tri.size();
  });
  // Simplify, the pick index, and turning solids into meshes.
  const sculpt::Mesh dense = sculpt::fromManifold(Manifold::Sphere(30, 1400));
  run("simplify: 1M triangles at 0.02 mm", [&] {
    sculpt::Mesh out;
    std::string error;
    fx::simplify(dense, 0.02, &out, &error);
    sink = sink + out.tri.size();
  });
  const CpuMesh big = drawMesh(sculptLike);
  run("pick index: build on 1.1M triangles", [&] {
    PickIndex index(big);
    sink = sink + index.triangles();
  });
  const PickIndex index(big);
  run("pick index: 200k rays", [&] {
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(-30, 30);
    for (int i = 0; i < 200000; ++i) {
      const float o[3] = {u(rng), u(rng), 100}, d[3] = {0, 0, -1};
      float t = 0;
      int tri = -1;
      if (index.cast(o, d, 1e30f, &t, &tri)) sink = sink + t;
    }
  });
  run("convert: 1.1M-triangle solid to a mesh and back", [&] {
    const sculpt::Mesh m = sculpt::fromManifold(sculptLike);
    sink = sink + fx::toManifold(m).NumTri();
  });
  double total = 0;
  for (const Row& r : rows) total += r.seconds;
  std::printf("%-44s %8.3f s\n", "total", total);
  return 0;
}
