#include "lattice.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lattice {

namespace la = manifold::la;
using sculpt::vec3;

namespace {

constexpr double kPi = 3.14159265358979323846;

// A repeatable random number 0 .. 1 for a cell of the Voronoi grid.
double hash01(int x, int y, int z, int seed, int salt) {
  uint64_t h = 1469598103934665603ULL;
  for (int64_t v : {int64_t(x), int64_t(y), int64_t(z), int64_t(seed), int64_t(salt)}) {
    h ^= static_cast<uint64_t>(v) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h *= 1099511628211ULL;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  return static_cast<double>(h >> 11) / static_cast<double>(1ULL << 53);
}

// The seed of one Voronoi cell: somewhere in the middle half of its grid cube. Random enough to
// look grown, regular enough that cells do not meet at very flat angles (those make wide, flat
// struts instead of round ones).
vec3 seedOf(int x, int y, int z, const Settings& s) {
  return vec3((x + 0.25 + 0.5 * hash01(x, y, z, s.seed, 1)) * s.cell, (y + 0.25 + 0.5 * hash01(x, y, z, s.seed, 2)) * s.cell,
              (z + 0.25 + 0.5 * hash01(x, y, z, s.seed, 3)) * s.cell);
}

// Distance from p to the Voronoi faces (the walls between cells) and to the edges (where three
// cells meet), from the three nearest seeds.
// slopes: if given, which way each grows ([0] toFace, [1] toEdge).
void voronoi(const Settings& s, const vec3& p, double* toFace, double* toEdge, vec3* slopes = nullptr) {
  // The 4 x 4 x 4 grid cubes nearest p (on each axis the two either side of the nearest cube
  // boundary): enough to always find the three nearest seeds, as each seed keeps a quarter of a
  // cube from its walls.
  const int cx = static_cast<int>(std::floor(p.x / s.cell - 0.5)), cy = static_cast<int>(std::floor(p.y / s.cell - 0.5)),
            cz = static_cast<int>(std::floor(p.z / s.cell - 0.5));
  vec3 best[3];
  double dist[3] = {1e300, 1e300, 1e300};
  for (int dz = -1; dz <= 2; ++dz)
    for (int dy = -1; dy <= 2; ++dy)
      for (int dx = -1; dx <= 2; ++dx) {
        const vec3 q = seedOf(cx + dx, cy + dy, cz + dz, s);
        const double d = la::dot(q - p, q - p);
        if (d < dist[2]) {
          int at = 2;
          while (at > 0 && d < dist[at - 1]) {
            dist[at] = dist[at - 1];
            best[at] = best[at - 1];
            --at;
          }
          dist[at] = d;
          best[at] = q;
        }
      }
  // Walls are where the second-nearest seed is as near as the nearest; edges are where the third
  // is too. Half the gap between those distances never jumps where seeds change places (an exact
  // distance to a wall or edge would, and leave fins), and is exact right across a wall or edge (it
  // grows at most twice as fast as the distance does).
  const double r[3] = {std::sqrt(dist[0]), std::sqrt(dist[1]), std::sqrt(dist[2])};
  *toFace = (r[1] - r[0]) * 0.5;
  *toEdge = (r[2] - r[0]) * 0.5;
  if (slopes) {
    vec3 away[3];
    for (int i = 0; i < 3; ++i) away[i] = r[i] > 0 ? (p - best[i]) / r[i] : vec3(0, 0, 0);
    slopes[0] = (away[1] - away[0]) * 0.5;
    slopes[1] = (away[2] - away[0]) * 0.5;
  }
}

// Sponge: a random, coral- or bone-like network, the solid where a smooth random field is above a
// level. The field is many waves of about one length running every way with random phases (a
// Gaussian random field, as spinodal foams form), sampled once per seed into a tile that repeats
// every 8 wavelengths and read back with a smooth (Catmull-Rom) blend, so the mesher pays a few
// dozen look-ups per point and not hundreds of waves.
struct SpongeTile {
  static constexpr int kWaves = 8;      // wavelengths across the tile
  static constexpr int kPerWave = 10;   // samples per wavelength
  static constexpr int kN = kWaves * kPerWave;
  double rms = 1;                       // root-mean-square wave number, in waves per wavelength
  std::vector<float> value;             // kN^3, x fastest; mean 0, spread 1
};

const SpongeTile& spongeTile(int seed) {
  thread_local int lastSeed = 0;
  thread_local const SpongeTile* last = nullptr;
  if (last && lastSeed == seed) return *last;
  static std::mutex lock;
  static std::unordered_map<int, std::unique_ptr<SpongeTile>> tiles;
  std::lock_guard<std::mutex> hold(lock);
  std::unique_ptr<SpongeTile>& slot = tiles[seed];
  if (!slot) {
    auto tile = std::make_unique<SpongeTile>();
    constexpr int L = SpongeTile::kWaves, N = SpongeTile::kN;
    // Whole wave numbers (so the tile repeats seamlessly) from 0.75 to 1.25 wavelengths: a little
    // spread in size looks grown, not machined. One of each +/- pair.
    const int reach = static_cast<int>(std::floor(1.25 * L));
    std::vector<std::array<int, 3>> all;
    for (int c = 0; c <= reach; ++c)
      for (int b = -reach; b <= reach; ++b)
        for (int a = -reach; a <= reach; ++a) {
          const double n = std::sqrt(double(a * a + b * b + c * c));
          if (n < 0.75 * L || n > 1.25 * L) continue;
          if (c == 0 && (b < 0 || (b == 0 && a <= 0))) continue;
          all.push_back({a, b, c});
        }
    // A repeatable shuffle (hash01, not std::shuffle, which differs between libraries).
    for (size_t i = all.size(); i > 1; --i) {
      const size_t j = static_cast<size_t>(hash01(int(i), 0, 0, seed, 71) * double(i)) % i;
      std::swap(all[i - 1], all[j]);
    }
    const size_t count = std::min<size_t>(all.size(), 240);
    std::vector<double> phase(count);
    double sum2 = 0;
    for (size_t w = 0; w < count; ++w) {
      phase[w] = hash01(int(w), 1, 0, seed, 73) * 2 * kPi;
      sum2 += all[w][0] * all[w][0] + all[w][1] * all[w][1] + all[w][2] * all[w][2];
    }
    tile->rms = std::sqrt(sum2 / double(count)) / L;
    // cos(a x + b y + c z + phase), summed: the y and z parts are multiplied in once per row.
    std::vector<double> cosT((2 * reach + 1) * N), sinT((2 * reach + 1) * N);
    for (int a = -reach; a <= reach; ++a)
      for (int x = 0; x < N; ++x) {
        const double angle = 2 * kPi * a * x / N;
        cosT[(a + reach) * N + x] = std::cos(angle);
        sinT[(a + reach) * N + x] = std::sin(angle);
      }
    tile->value.assign(size_t(N) * N * N, 0.f);
    const double scale = std::sqrt(2.0 / double(count));
    auto rows = [&](int z0, int z1) {
      std::vector<double> row(N);
      for (int z = z0; z < z1; ++z)
        for (int y = 0; y < N; ++y) {
          std::fill(row.begin(), row.end(), 0.0);
          for (size_t w = 0; w < count; ++w) {
            const int a = all[w][0] + reach, b = all[w][1] + reach, c = all[w][2] + reach;
            // e^(i(phase + b y + c z)), then times e^(i a x) for each x.
            const double angle = phase[w];
            double re = std::cos(angle), im = std::sin(angle);
            const double by = cosT[b * N + y], sy = sinT[b * N + y], cz = cosT[c * N + z], sz = sinT[c * N + z];
            double r2 = re * by - im * sy, i2 = re * sy + im * by;
            re = r2 * cz - i2 * sz, im = r2 * sz + i2 * cz;
            const double* cx = &cosT[a * N];
            const double* sx = &sinT[a * N];
            for (int x = 0; x < N; ++x) row[x] += re * cx[x] - im * sx[x];
          }
          float* out = &tile->value[(size_t(z) * N + y) * N];
          for (int x = 0; x < N; ++x) out[x] = static_cast<float>(row[x] * scale);
        }
    };
    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(rows, N * t / threads, N * (t + 1) / threads);
    for (std::thread& t : pool) t.join();
    slot = std::move(tile);
  }
  lastSeed = seed;
  last = slot.get();
  return *last;
}

// The level, the wavelength (mm) and the solid share for the settings. The walls are the solid's
// mean thickness, the cell the mean repeat (one strut and one hole across): the share is
// wall / cell, kept between 0.25 (thinner falls apart into crumbs) and 0.6 (a block with dents).
// Along a line through a Gaussian field the level is crossed upwards rms/sqrt(3) * e^(-t^2/2)
// times per wavelength (Rice), which gives the mean strut thickness for a level and so the
// wavelength that makes it the wall.
struct SpongeShape {
  double level, wavelength, share, rms;
};
SpongeShape spongeShape(const Settings& s) {
  // Asked for at every point: worked out once per thread for the same settings.
  thread_local double lastWall = -1, lastCell = -1;
  thread_local int lastSeed = 0;
  thread_local SpongeShape last{};
  if (s.wall == lastWall && s.cell == lastCell && s.seed == lastSeed) return last;
  SpongeShape shape;
  shape.share = std::clamp(s.wall / s.cell, 0.25, 0.6);
  shape.rms = spongeTile(s.seed).rms;
  // The level above which `share` of a unit Gaussian lies: 0.5 erfc(t / sqrt 2) = share.
  double lo = -4, hi = 4;
  for (int i = 0; i < 60; ++i) {
    const double mid = (lo + hi) / 2;
    (0.5 * std::erfc(mid / std::sqrt(2.0)) > shape.share ? lo : hi) = mid;
  }
  shape.level = (lo + hi) / 2;
  const double crossings = shape.rms / std::sqrt(3.0) * std::exp(-shape.level * shape.level / 2);
  const double strut = shape.share / crossings;  // in wavelengths
  shape.wavelength = s.wall / strut;
  lastWall = s.wall, lastCell = s.cell, lastSeed = s.seed, last = shape;
  return shape;
}

// The tile's value at p (mm) for a wavelength, and its slope per mm.
double spongeValue(const SpongeTile& tile, const vec3& p, double wavelength, vec3* grad) {
  constexpr int N = SpongeTile::kN;
  const double per = SpongeTile::kPerWave / wavelength;  // samples per mm
  const double u[3] = {p.x * per, p.y * per, p.z * per};
  int base[3];
  double w[3][4], d[3][4];
  for (int a = 0; a < 3; ++a) {
    const double f = std::floor(u[a]);
    const double t = u[a] - f;
    base[a] = static_cast<int>(static_cast<long long>(f) % N);
    if (base[a] < 0) base[a] += N;
    const double t2 = t * t, t3 = t2 * t;
    w[a][0] = (-t3 + 2 * t2 - t) / 2;
    w[a][1] = (3 * t3 - 5 * t2 + 2) / 2;
    w[a][2] = (-3 * t3 + 4 * t2 + t) / 2;
    w[a][3] = (t3 - t2) / 2;
    d[a][0] = (-3 * t2 + 4 * t - 1) / 2;
    d[a][1] = (9 * t2 - 10 * t) / 2;
    d[a][2] = (-9 * t2 + 8 * t + 1) / 2;
    d[a][3] = (3 * t2 - 2 * t) / 2;
  }
  int ix[4], iy[4], iz[4];
  for (int k = 0; k < 4; ++k) {
    ix[k] = (base[0] + k - 1 + N) % N;
    iy[k] = (base[1] + k - 1 + N) % N;
    iz[k] = (base[2] + k - 1 + N) % N;
  }
  double v = 0, gx = 0, gy = 0, gz = 0;
  for (int c = 0; c < 4; ++c)
    for (int b = 0; b < 4; ++b) {
      const float* row = &tile.value[(size_t(iz[c]) * N + iy[b]) * N];
      double rv = 0, rd = 0;
      for (int a = 0; a < 4; ++a) {
        const double x = row[ix[a]];
        rv += w[0][a] * x;
        rd += d[0][a] * x;
      }
      const double wb = w[1][b], wc = w[2][c];
      v += rv * wb * wc;
      if (grad) {
        gx += rd * wb * wc;
        gy += rv * d[1][b] * wc;
        gz += rv * wb * d[2][c];
      }
    }
  if (grad) *grad = vec3(gx, gy, gz) * per;
  return v;
}

// Bubbles: a solid with round bubbles of mixed sizes blown into it, as in a foamed glass or an aerated
// bar. Each grid cube (the cell) holds one bubble anywhere in it, from a quarter smaller to a
// little larger than the cell, and often a small one too. Bubbles that meet open into each other with a
// rounded rim; those at the part's surface open to the outside. Positive in the solid.
double bubbles(const Settings& s, const vec3& p, vec3* slope) {
  const double c = s.cell;
  const double big = c * 0.5;
  const int cx = static_cast<int>(std::floor(p.x / c)), cy = static_cast<int>(std::floor(p.y / c)),
            cz = static_cast<int>(std::floor(p.z / c));
  double d[2] = {1e300, 1e300};  // the two nearest bubbles' surfaces (negative inside)
  vec3 away[2] = {vec3(0, 0, 0), vec3(0, 0, 0)};
  auto consider = [&](const vec3& centre, double r) {
    const vec3 v = p - centre;
    const double l = la::length(v);
    const double u = l - r;
    const vec3 dir = l > 1e-12 ? v / l : vec3(0, 0, 1);
    if (u < d[0]) {
      d[1] = d[0], away[1] = away[0];
      d[0] = u, away[0] = dir;
    } else if (u < d[1]) {
      d[1] = u, away[1] = dir;
    }
  };
  for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) {
        const int x = cx + dx, y = cy + dy, z = cz + dz;
        const vec3 centre((x + hash01(x, y, z, s.seed, 11)) * c, (y + hash01(x, y, z, s.seed, 12)) * c,
                          (z + hash01(x, y, z, s.seed, 13)) * c);
        consider(centre, big * (0.75 + 0.4 * hash01(x, y, z, s.seed, 14)));
        if (hash01(x, y, z, s.seed, 15) < 0.6) {
          const vec3 small((x + hash01(x, y, z, s.seed, 16)) * c, (y + hash01(x, y, z, s.seed, 17)) * c,
                           (z + hash01(x, y, z, s.seed, 18)) * c);
          consider(small, big * (0.35 + 0.25 * hash01(x, y, z, s.seed, 19)));
        }
      }
  // Two bubbles closer than about half a wall do not leave a skin between them that would tear (or
  // not print): they open into each other, as the films in a real foam break. Across a gap the
  // bubbles' distances add up to its width, so (d0 + d1 - thinnest) / 2 goes below 0 where the skin
  // would be too thin. Rims are rounded over about half a wall (a sharp rim meshes as a sawtooth,
  // and would print as a knife edge).
  const double k = std::max(s.wall * 0.5, 1e-6);
  auto smoothMin = [k](double a, double b, double* share) {
    const double h = std::max(k - std::abs(a - b), 0.0) / k;
    *share = a < b ? 0.5 * h : 1 - 0.5 * h;  // how much of the result follows b
    return std::min(a, b) - h * h * k * 0.25;
  };
  double second = 0, thin = 0;
  const double nearest = smoothMin(d[0], d[1], &second);
  const double skin = (d[0] + d[1] - 0.6 * s.wall) * 0.5;
  const double value = smoothMin(nearest, skin, &thin);
  if (slope) {
    const vec3 gNearest = away[0] * (1 - second) + away[1] * second;
    *slope = gNearest * (1 - thin) + (away[0] + away[1]) * 0.5 * thin;
  }
  return value;
}

// The pattern, positive inside its walls, and (if slope is given) which way it grows.
double pattern(const Settings& s, const vec3& p, vec3* slope) {
  const double half = s.wall * 0.5;
  switch (s.kind) {
    case Kind::Gyroid: {
      // The gyroid surface sin x cos y + sin y cos z + sin z cos x = 0, thickened to the wall:
      // the value over its slope is close to the distance from the surface.
      const double k = 2 * kPi / s.cell;
      const double x = p.x * k, y = p.y * k, z = p.z * k;
      const double sx = std::sin(x), cx = std::cos(x), sy = std::sin(y), cy = std::cos(y), sz = std::sin(z), cz = std::cos(z);
      const double g = sx * cy + sy * cz + sz * cx;
      const vec3 grad(k * (cx * cy - sz * sx), k * (-sx * sy + cy * cz), k * (-sy * sz + cz * cx));
      const double steep = std::max(la::length(grad), 0.35 * k);
      if (slope) *slope = grad * ((g > 0 ? -1.0 : 1.0) / steep);
      return half - std::abs(g) / steep;
    }
    case Kind::Honeycomb: {
      // Upright hexagonal cells, cell = across the flats: walls stand straight up, no overhangs.
      const double c = s.cell;
      const double r3 = std::sqrt(3.0);
      // Centres on the lattice u (c, 0) and v (c/2, c*sqrt3/2): try the few around p.
      const double v = p.y / (c * r3 / 2);
      const double u = p.x / c - v / 2;
      double best = 1e300;
      vec3 centre(0, 0, 0);
      for (int dv = -1; dv <= 2; ++dv)
        for (int du = -1; du <= 2; ++du) {
          const double iu = std::floor(u) + du, iv = std::floor(v) + dv;
          const double qx = iu * c + iv * c / 2, qy = iv * c * r3 / 2;
          const double d = (p.x - qx) * (p.x - qx) + (p.y - qy) * (p.y - qy);
          if (d < best) {
            best = d;
            centre = vec3(qx, qy, 0);
          }
        }
      const double lx = p.x - centre.x, ly = p.y - centre.y;
      // How far out from the centre, towards the nearest wall; the cell's inside corners rounded
      // (a sharp crease there meshes as a sawtooth).
      double reach = -1;
      const double round = s.wall * 0.6;
      for (int i = 0; i < 3; ++i) {
        const double a = i * kPi / 3;
        const double along = lx * std::cos(a) + ly * std::sin(a);
        if (slope && std::abs(along) >= reach) *slope = vec3(std::cos(a), std::sin(a), 0) * (along < 0 ? -1.0 : 1.0);
        if (reach < 0) reach = std::abs(along);
        else {
          const double h = std::max(round - std::abs(reach - std::abs(along)), 0.0) / round;
          reach = std::max(reach, std::abs(along)) + h * h * round * 0.25;
        }
      }
      return half - (c / 2 - reach);
    }
    case Kind::Sponge: {
      // Above the level, over the slope: about the distance into the strut. The slope's floor keeps
      // flat spots in the field (saddles, tops) from reading as far from the surface.
      const SpongeShape shape = spongeShape(s);
      vec3 grad;
      const double v = spongeValue(spongeTile(s.seed), p, shape.wavelength, &grad);
      const double typical = 2 * kPi * shape.rms / shape.wavelength;  // the slope's usual size
      const double steep = std::max(la::length(grad), 0.35 * typical);
      if (slope) *slope = grad / steep;
      return (v - shape.level) / steep;
    }
    case Kind::SpongeWalls: {
      // The field's zero surface, thickened to the wall (like the gyroid): one wavy wall between two
      // networks of pores. The level 0 splits space in half, so the pores' mean width is fixed by the
      // wavelength alone (Rice again): the cell (pore and wall) sets it.
      const double rms = spongeTile(s.seed).rms;
      const double wavelength = s.cell * rms / (0.5 * std::sqrt(3.0));
      vec3 grad;
      const double v = spongeValue(spongeTile(s.seed), p, wavelength, &grad);
      const double steep = std::max(la::length(grad), 0.35 * 2 * kPi * rms / wavelength);
      if (slope) *slope = grad * ((v > 0 ? -1.0 : 1.0) / steep);
      return half - std::abs(v) / steep;
    }
    case Kind::Bubbles: return bubbles(s, p, slope);
    case Kind::VoronoiSkin:
    case Kind::VoronoiLattice: {
      double face, edge;
      vec3 slopes[2];
      voronoi(s, p, &face, &edge, slope ? slopes : nullptr);
      const bool skin = s.kind == Kind::VoronoiSkin;
      if (slope) *slope = -slopes[skin ? 0 : 1];
      return half - (skin ? face : edge);
    }
  }
  if (slope) *slope = vec3(0, 0, 0);
  return -1;
}

}  // namespace

double patternField(const Settings& s, const vec3& p) { return pattern(s, p, nullptr); }

double spongeSolidShare(const Settings& s) { return spongeShape(s).share; }

double spongeCell(const Settings& s) { return s.wall / spongeShape(s).share; }

double autoDetail(const Settings& s) { return std::clamp(std::min(s.wall / 3.2, s.cell / 10), 0.15, 1.5); }

double usedDetail(const sculpt::Mesh& part, const Settings& s) {
  double edge = s.detail > 0 ? s.detail : autoDetail(s);
  vec3 lo, hi;
  sculpt::bounds(part, &lo, &hi);
  const vec3 size = hi - lo;
  // Manifold samples two points per grid cube; keep that under about 60 million.
  while ((size.x / edge + 5) * (size.y / edge + 5) * (size.z / edge + 5) > 30e6) edge *= 1.15;
  return edge;
}

double estimateTriangles(const sculpt::Mesh& part, const Settings& s) {
  const double edge = usedDetail(part, s);
  double volume = std::abs(sculpt::volume(part)), area = 0;
  for (const sculpt::Tri& t : part.tri)
    area += la::length(la::cross(part.pos[t[1]] - part.pos[t[0]], part.pos[t[2]] - part.pos[t[0]])) * 0.5;
  // Surface of the pattern inside the part: about two sides per wall per cell.
  double inner = 0;
  switch (s.kind) {
    case Kind::Gyroid: inner = volume * 2 * 3.1 / s.cell; break;
    case Kind::Honeycomb: inner = volume * 2 * 2.31 / s.cell; break;
    case Kind::VoronoiLattice: inner = volume * 3.0 * kPi * s.wall / (s.cell * s.cell); break;
    case Kind::VoronoiSkin: inner = area * 2 * s.wall * 3 / s.cell; break;
    case Kind::Sponge: {
      // Surface per volume: twice the crossings per length of a line (stereology).
      const SpongeShape shape = spongeShape(s);
      inner = volume * 4 * shape.rms / std::sqrt(3.0) * std::exp(-shape.level * shape.level / 2) / shape.wavelength;
      break;
    }
    case Kind::Bubbles: {
      // Every bubble's surface, less the part of it inside the others (about a third).
      const double r = s.cell * 0.5;
      const double mean2 = r * r * (0.95 * 0.95 + 0.6 * 0.47 * 0.47);
      inner = volume * 0.65 * 4 * kPi * mean2 / (s.cell * s.cell * s.cell);
      break;
    }
    case Kind::SpongeWalls: {
      // Both sides of the zero surface; its area per volume is 4 rms / (sqrt 3 wavelength).
      const double rms = spongeTile(s.seed).rms;
      const double wavelength = s.cell * rms / (0.5 * std::sqrt(3.0));
      inner = volume * 2 * 4 * rms / (std::sqrt(3.0) * wavelength);
      break;
    }
  }
  const double skinArea = s.kind == Kind::VoronoiSkin ? area * 2 * std::min(1.0, s.wall * 3.5 / s.cell) : (s.skin > 0 ? area * 2 : area * 0.4);
  // The mesh comes out a little finer than an even mesh at that edge.
  return 1.25 * (inner + skinArea) / (0.433 * edge * edge);
}

const char* stageName(int stage) {
  switch (stage) {
    case 0: return "Measuring the part";
    case 1: return "Building the lattice";
    case 2: return "Leaving out loose crumbs";
    case 3: return "Smoothing the surface";
    case 4: return "Checking for folds";
    default: return "";
  }
}

namespace {

// Runs f(i) for i in 0 .. count on every core.
template <typename F>
void parallelFor(size_t count, F&& f) {
  const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 64u);
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (;;) {
      const size_t from = next.fetch_add(2048);
      if (from >= count) return;
      const size_t to = std::min(count, from + 2048);
      for (size_t i = from; i < to; ++i) f(i);
    }
  };
  std::vector<std::thread> pool;
  for (unsigned i = 1; i < threads; ++i) pool.emplace_back(work);
  work();
  for (std::thread& t : pool) t.join();
}

// Per-point lists (the neighbours of each point, or the triangles at it) in two flat arrays: one
// allocation instead of one per point, which on a million-point mesh is most of the cost.
struct Adjacency {
  std::vector<uint32_t> start, items;
  struct Range {
    const uint32_t *b, *e;
    const uint32_t* begin() const { return b; }
    const uint32_t* end() const { return e; }
    size_t size() const { return static_cast<size_t>(e - b); }
    bool empty() const { return b == e; }
  };
  Range operator[](size_t v) const { return {items.data() + start[v], items.data() + start[v + 1]}; }
};

// The triangles at each point.
Adjacency trianglesAt(const sculpt::Mesh& m) {
  Adjacency a;
  a.start.assign(m.pos.size() + 1, 0);
  for (const sculpt::Tri& t : m.tri)
    for (uint32_t v : t) ++a.start[v + 1];
  for (size_t v = 0; v < m.pos.size(); ++v) a.start[v + 1] += a.start[v];
  a.items.resize(a.start.back());
  std::vector<uint32_t> fill(a.start.begin(), a.start.end() - 1);
  for (uint32_t t = 0; t < m.tri.size(); ++t)
    for (uint32_t v : m.tri[t]) a.items[fill[v]++] = t;
  return a;
}

// The points joined to each point by an edge, sorted, each once.
Adjacency neighboursOf(const sculpt::Mesh& m) {
  Adjacency raw;
  raw.start.assign(m.pos.size() + 1, 0);
  for (const sculpt::Tri& t : m.tri)
    for (uint32_t v : t) raw.start[v + 1] += 2;
  for (size_t v = 0; v < m.pos.size(); ++v) raw.start[v + 1] += raw.start[v];
  raw.items.resize(raw.start.back());
  std::vector<uint32_t> fill(raw.start.begin(), raw.start.end() - 1);
  for (const sculpt::Tri& t : m.tri)
    for (int i = 0; i < 3; ++i) {
      raw.items[fill[t[i]]++] = t[(i + 1) % 3];
      raw.items[fill[t[i]]++] = t[(i + 2) % 3];
    }
  // Each point's list sorted and made unique where it is (on every core), then packed.
  std::vector<uint32_t> unique(m.pos.size(), 0);
  parallelFor(m.pos.size(), [&](size_t v) {
    uint32_t* b = raw.items.data() + raw.start[v];
    uint32_t* e = raw.items.data() + raw.start[v + 1];
    std::sort(b, e);
    unique[v] = static_cast<uint32_t>(std::unique(b, e) - b);
  });
  Adjacency a;
  a.start.assign(m.pos.size() + 1, 0);
  for (size_t v = 0; v < m.pos.size(); ++v) a.start[v + 1] = a.start[v] + unique[v];
  a.items.resize(a.start.back());
  parallelFor(m.pos.size(), [&](size_t v) {
    std::copy_n(raw.items.data() + raw.start[v], unique[v], a.items.data() + a.start[v]);
  });
  return a;
}

// Back onto the surface (where the field is 0), stepping along the field's slope.
template <typename Field>
vec3 projectOnto(const Field& field, vec3 q, double edge) {
  // Newton steps along the slope. The slope (three more calls) is worked out once and kept while the
  // steps stay short: a point already on a smooth surface costs one call, a typical one four or five
  // (it was twelve).
  const double h = edge * 0.05, close = edge * 1e-3;
  double f = field(q);
  if (std::abs(f) < close) return q;
  auto slope = [&] {
    return vec3((field(q + vec3(h, 0, 0)) - f) / h, (field(q + vec3(0, h, 0)) - f) / h, (field(q + vec3(0, 0, h)) - f) / h);
  };
  vec3 g = slope();
  for (int step = 0; step < 4; ++step) {
    const double gg = la::dot(g, g);
    if (gg < 1e-8) break;
    vec3 move = g * (-f / gg);
    const double len = la::length(move);
    const bool far = len > edge * 0.5;
    if (far) move *= edge * 0.5 / len;
    q += move;
    const double before = f;
    f = field(q);
    if (std::abs(f) < close) break;
    // Moved a long way, or not settling fast (a crease, a tight curve): a fresh slope.
    if (far || std::abs(f) > 0.25 * std::abs(before)) g = slope();
  }
  return q;
}

// The grid mesh made smooth: each pass evens the triangles out (each point slides towards the
// middle of its neighbours, along the surface) and then puts every point back on the true surface.
// Then the few spots where the mesher left triangles folded, collapsed or cutting through each
// other are relaxed until they untangle. left: the triangles still bad at the end (normally none);
// without it, only the smoothing is done. focus: only the mesh within a few triangles of these
// points is smoothed and looked at (the repairs touch a few spots of a mesh of millions; going over
// all of it for each try took most of a minute).
template <typename Field>
int smoothOnto(sculpt::Mesh& m, const Field& field, double edge, int passes, Progress* progress, double from, double to,
               std::vector<uint32_t>* left, const std::vector<vec3>* focus = nullptr) {
  const size_t n = m.pos.size();
  const Adjacency ring = neighboursOf(m);
  auto onto = [&](vec3 q) { return projectOnto(field, q, edge); };
  // The points near the focus, found through a grid of the reach.
  const bool focused = focus && !focus->empty();
  std::vector<uint32_t> near;
  std::vector<char> isNear;
  if (focused) {
    const double reach = 8 * edge;
    struct Cell {
      long long x, y, z;
      bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct CellHash {
      size_t operator()(const Cell& c) const { return size_t(c.x) * 73856093u ^ size_t(c.y) * 19349663u ^ size_t(c.z) * 83492791u; }
    };
    auto cellOf = [&](const vec3& p) {
      return Cell{static_cast<long long>(std::floor(p.x / reach)), static_cast<long long>(std::floor(p.y / reach)),
                  static_cast<long long>(std::floor(p.z / reach))};
    };
    std::unordered_map<Cell, std::vector<vec3>, CellHash> spots;
    for (const vec3& p : *focus) spots[cellOf(p)].push_back(p);
    isNear.assign(n, 0);
    for (uint32_t v = 0; v < n; ++v) {
      const Cell c = cellOf(m.pos[v]);
      bool close = false;
      for (long long dz = -1; dz <= 1 && !close; ++dz)
        for (long long dy = -1; dy <= 1 && !close; ++dy)
          for (long long dx = -1; dx <= 1 && !close; ++dx) {
            const auto it = spots.find({c.x + dx, c.y + dy, c.z + dz});
            if (it == spots.end()) continue;
            for (const vec3& p : it->second)
              if (la::dot(p - m.pos[v], p - m.pos[v]) <= reach * reach) {
                close = true;
                break;
              }
          }
      if (close) {
        isNear[v] = 1;
        near.push_back(v);
      }
    }
  }
  for (int pass = 0; pass < passes; ++pass) {
    if (progress && progress->cancel) return 0;
    // Vertex normals of the current mesh, for sliding along the surface.
    std::vector<vec3> normal(n, vec3(0, 0, 0));
    for (const sculpt::Tri& t : m.tri) {
      const vec3 c = la::cross(m.pos[t[1]] - m.pos[t[0]], m.pos[t[2]] - m.pos[t[0]]);
      for (uint32_t v : t) normal[v] += c;
    }
    std::vector<vec3> next = focused ? m.pos : std::vector<vec3>(n);
    std::atomic<size_t> done{0};
    const size_t count = focused ? near.size() : n;
    parallelFor(count, [&](size_t i) {
      const size_t v = focused ? near[i] : i;
      vec3 p = m.pos[v];
      if (!ring[v].empty()) {
        vec3 mid(0, 0, 0);
        for (uint32_t u : ring[v]) mid += m.pos[u];
        mid /= static_cast<double>(ring[v].size());
        const double l = la::length(normal[v]);
        const vec3 nv = l > 0 ? normal[v] / l : vec3(0, 0, 0);
        const vec3 move = mid - p;
        p += (move - nv * la::dot(move, nv)) * 0.5;
      }
      next[v] = onto(p);
      if ((done.fetch_add(1) & 8191) == 0 && progress)
        progress->fraction = from + (to - from) * 0.9 * (pass + double(done) / count) / passes;
    });
    m.pos = std::move(next);
  }
  // No folds, no slivers and no crossings (the mesher itself leaves a few hundred of those in a
  // million triangles: points on top of each other, slivers folded through a neighbour). Each bad
  // spot is relaxed on its own, a little wider each round, until it untangles. A spot that stays
  // tangled is dealt with afterwards (collapseShortest, cutOut).
  if (!left) return 0;  // smoothing only
  if (progress) progress->stage = 4;
  const Adjacency around = trianglesAt(m);  // the triangles at each point
  std::vector<char> rough(n, 0);
  std::vector<uint32_t> region, candidates;
  std::vector<char> inRegion(n, 0), seen(m.tri.size(), 0);
  auto triCross = [&](uint32_t t) {
    const sculpt::Tri& tri = m.tri[t];
    return la::cross(m.pos[tri[1]] - m.pos[tri[0]], m.pos[tri[2]] - m.pos[tri[0]]);
  };
  const int rounds = 12;
  left->clear();
  if (focused) {  // the first round looks only near the focus, too
    for (uint32_t v : near)
      for (uint32_t t : around[v])
        if (!seen[t]) {
          seen[t] = 1;
          candidates.push_back(t);
        }
    for (uint32_t t : candidates) seen[t] = 0;
  }
  for (int round = 0; round <= rounds; ++round) {
    if (progress && progress->cancel) return 0;
    if (progress) progress->fraction = from + (to - from) * (0.9 + 0.1 * round / rounds);
    // The first round looks at everything; after that only around the points that moved.
    const bool listed = round > 0 || focused;
    std::vector<uint32_t> bad = sculpt::crossingTriangles(m, listed ? &candidates : nullptr);
    const size_t count = listed ? candidates.size() : m.tri.size();
    std::mutex badGate;
    parallelFor(count, [&](size_t i) {
      const uint32_t t = listed ? candidates[i] : static_cast<uint32_t>(i);
      const vec3 c = triCross(t);
      vec3 near(0, 0, 0);
      for (uint32_t v : m.tri[t])
        for (uint32_t u : around[v]) near += triCross(u);
      // Collapsed, or turned right against the surface around it (a spike folded over). Near a sharp
      // node or a thin strut a triangle may stand at a steep angle to its neighbours: that is fine.
      const double area = la::length(c);
      if (area < 1e-6 * edge * edge || la::dot(c, near) < -0.5 * area * la::length(near)) {
        std::lock_guard<std::mutex> lock(badGate);
        bad.push_back(t);
      }
    });
    std::sort(bad.begin(), bad.end());  // (the same order whichever core found them)
    if (bad.empty()) break;
    if (round == rounds) {
      std::sort(bad.begin(), bad.end());
      bad.erase(std::unique(bad.begin(), bad.end()), bad.end());
      *left = std::move(bad);
      break;
    }
    // The bad triangles' points and, round by round, more and more of their neighbours.
    region.clear();
    for (uint32_t t : bad)
      for (uint32_t v : m.tri[t])
        if (!inRegion[v]) {
          inRegion[v] = 1;
          region.push_back(v);
        }
    for (int grow = 0; grow < std::min(round / 3 + 1, 4); ++grow) {
      const size_t size = region.size();
      for (size_t i = 0; i < size; ++i)
        for (uint32_t u : ring[region[i]])
          if (!inRegion[u]) {
            inRegion[u] = 1;
            region.push_back(u);
          }
    }
    for (int sweep = 0; sweep < 3; ++sweep) {
      std::vector<vec3> moved(region.size());
      for (size_t i = 0; i < region.size(); ++i) {
        const uint32_t v = region[i];
        vec3 mid(0, 0, 0);
        for (uint32_t u : ring[v]) mid += m.pos[u];
        mid /= static_cast<double>(std::max<size_t>(1, ring[v].size()));
        moved[i] = m.pos[v] + (mid - m.pos[v]) * 0.5;
      }
      for (size_t i = 0; i < region.size(); ++i) m.pos[region[i]] = moved[i];
    }
    candidates.clear();
    for (uint32_t v : region) {
      rough[v] = 1;
      inRegion[v] = 0;
      for (uint32_t t : around[v])
        if (!seen[t]) {
          seen[t] = 1;
          candidates.push_back(t);
        }
    }
    for (uint32_t t : candidates) seen[t] = 0;
  }
  if (progress) progress->fraction = to;
  return static_cast<int>(std::count(rough.begin(), rough.end(), 1));
}

// The level-set mesher leaves triangles about half the size asked for (its grid cuts each cube into
// several): twice the triangles for no more shape. Short edges are collapsed until they are about
// `edge` long, each to its middle put back on the surface, and only where nothing folds, the
// surface keeps its topology (the two points share just the two corners across the edge) and no
// edge grows past 4/3 of `edge`. Each pass works on spots apart from each other (the points round
// a collapse are locked until the next pass), so the lookups stay right without rebuilding them.
template <typename Field>
size_t coarsen(sculpt::Mesh& m, const Field& field, double edge, Progress* progress, double from, double to) {
  const double shortest = 0.75 * edge, longest = 4.0 / 3.0 * edge;
  constexpr uint32_t kDead = UINT32_MAX;
  size_t total = 0;
  for (int pass = 0; pass < 4; ++pass) {
    if (progress && progress->cancel) return total;
    const Adjacency around = trianglesAt(m);
    std::vector<char> locked(m.pos.size(), 0);
    size_t collapsed = 0;
    std::vector<uint32_t> ringA, ringB;
    for (uint32_t t = 0; t < m.tri.size(); ++t) {
      if (progress && (t & 65535) == 0)
        progress->fraction = from + (to - from) * (pass + double(t) / m.tri.size()) / 4;
      const sculpt::Tri tri = m.tri[t];
      if (tri[0] == kDead || locked[tri[0]] || locked[tri[1]] || locked[tri[2]]) continue;
      // Its shortest edge, if short enough.
      int k = 0;
      double best = 1e300;
      for (int i = 0; i < 3; ++i) {
        const double l = la::length(m.pos[tri[i]] - m.pos[tri[(i + 1) % 3]]);
        if (l < best) best = l, k = i;
      }
      if (best >= shortest) continue;
      const uint32_t a = tri[k], b = tri[(k + 1) % 3];
      // The two triangles on the edge, and the points round a and b.
      int across = 0;
      uint32_t corners[2] = {kDead, kDead};
      ringA.clear();
      ringB.clear();
      for (uint32_t u : around[a]) {
        const sculpt::Tri& o = m.tri[u];
        if (o[0] == kDead) continue;
        bool hasB = false;
        for (uint32_t v : o) hasB |= v == b;
        if (hasB) {
          if (across < 2)
            for (uint32_t v : o)
              if (v != a && v != b) corners[across] = v;
          ++across;
        }
        for (uint32_t v : o)
          if (v != a) ringA.push_back(v);
      }
      if (across != 2) continue;
      for (uint32_t u : around[b]) {
        const sculpt::Tri& o = m.tri[u];
        if (o[0] == kDead) continue;
        for (uint32_t v : o)
          if (v != b) ringB.push_back(v);
      }
      std::sort(ringA.begin(), ringA.end());
      ringA.erase(std::unique(ringA.begin(), ringA.end()), ringA.end());
      std::sort(ringB.begin(), ringB.end());
      ringB.erase(std::unique(ringB.begin(), ringB.end()), ringB.end());
      int shared = 0;
      for (uint32_t v : ringA)
        if (std::binary_search(ringB.begin(), ringB.end(), v)) ++shared;
      if (shared != 2) continue;  // a handle would pinch shut
      const vec3 mid = (m.pos[a] + m.pos[b]) * 0.5;
      const vec3 at = projectOnto(field, mid, edge);
      if (la::length(at - mid) > 0.5 * best + 0.1 * edge) continue;
      // Every other triangle round a and b, with a and b moved to `at`: turned no more than 60
      // degrees, not squashed, and no edge too long.
      bool fine = true;
      for (const uint32_t p : {a, b}) {
        for (uint32_t u : around[p]) {
          const sculpt::Tri& o = m.tri[u];
          if (o[0] == kDead) continue;
          bool hasA = false, hasB = false;
          for (uint32_t v : o) hasA |= v == a, hasB |= v == b;
          if (hasA && hasB) continue;
          vec3 q[3], was[3];
          for (int i = 0; i < 3; ++i) {
            was[i] = m.pos[o[i]];
            q[i] = (o[i] == a || o[i] == b) ? at : was[i];
          }
          const vec3 nOld = la::cross(was[1] - was[0], was[2] - was[0]);
          const vec3 nNew = la::cross(q[1] - q[0], q[2] - q[0]);
          const double lNew = la::length(nNew), lOld = la::length(nOld);
          if (lNew < 1e-3 * edge * edge || la::dot(nNew, nOld) < 0.5 * lNew * lOld) {
            fine = false;
            break;
          }
          for (int i = 0; i < 3 && fine; ++i)
            if (la::length(q[i] - q[(i + 1) % 3]) > longest) fine = false;
          if (!fine) break;
        }
        if (!fine) break;
      }
      if (!fine) continue;
      // b goes into a.
      for (uint32_t u : around[b]) {
        sculpt::Tri& o = m.tri[u];
        if (o[0] == kDead) continue;
        bool hasA = false;
        for (uint32_t v : o) hasA |= v == a;
        if (hasA) {
          o = {kDead, kDead, kDead};
          continue;
        }
        for (uint32_t& v : o)
          if (v == b) v = a;
      }
      m.pos[a] = at;
      locked[a] = locked[b] = 1;
      for (uint32_t v : ringA) locked[v] = 1;
      for (uint32_t v : ringB) locked[v] = 1;
      ++collapsed;
    }
    // Packed: dead triangles and unused points out.
    std::vector<uint32_t> renumber(m.pos.size(), kDead);
    sculpt::Mesh packed;
    packed.tri.reserve(m.tri.size());
    for (const sculpt::Tri& o : m.tri) {
      if (o[0] == kDead) continue;
      sculpt::Tri n;
      for (int i = 0; i < 3; ++i) {
        if (renumber[o[i]] == kDead) {
          renumber[o[i]] = static_cast<uint32_t>(packed.pos.size());
          packed.pos.push_back(m.pos[o[i]]);
        }
        n[i] = renumber[o[i]];
      }
      packed.tri.push_back(n);
    }
    m = std::move(packed);
    total += collapsed;
    if (collapsed < m.tri.size() / 200) break;
  }
  return total;
}

// Spots that relaxing cannot untangle are tiny handles or tubes, far thinner than the triangles,
// where a wall leaves the part almost along its surface. Each is cut out with a couple of rings of
// triangles around it, and every hole left is closed with a fan around a new point on the surface.
// Returns false if there was nothing it could cut.
template <typename Field>
bool cutOut(sculpt::Mesh& m, const std::vector<uint32_t>& bad, const Field& field, double edge) {
  const Adjacency around = trianglesAt(m);
  std::vector<char> inPatch(m.tri.size(), 0);
  std::vector<uint32_t> patch;
  auto add = [&](uint32_t t) {
    if (!inPatch[t]) {
      inPatch[t] = 1;
      patch.push_back(t);
    }
  };
  for (uint32_t t : bad) add(t);
  // The holes' rims, as loops of directed edges (a to b, as the cut triangles had them).
  std::vector<std::vector<uint32_t>> loops;
  for (int grow = 0; grow < 6; ++grow) {
    const size_t size = patch.size();
    for (size_t i = 0; i < size; ++i)
      for (uint32_t v : m.tri[patch[i]])
        for (uint32_t u : around[v]) add(u);
    if (grow < 1) continue;
    std::unordered_map<uint64_t, int> uses;
    auto key = [](uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); };
    for (uint32_t t : patch)
      for (int i = 0; i < 3; ++i) ++uses[key(m.tri[t][i], m.tri[t][(i + 1) % 3])];
    std::unordered_map<uint32_t, uint32_t> nextOf;
    bool pinched = false;
    for (uint32_t t : patch)
      for (int i = 0; i < 3; ++i) {
        const uint32_t a = m.tri[t][i], b = m.tri[t][(i + 1) % 3];
        if (uses[key(a, b)] != 1) continue;
        if (!nextOf.emplace(a, b).second) pinched = true;  // a rim that touches itself: cut wider
      }
    if (pinched) continue;
    loops.clear();
    std::unordered_map<uint32_t, char> done;
    bool broken = false;
    for (const auto& [start, unused] : nextOf) {
      if (done[start]) continue;
      std::vector<uint32_t> loop;
      uint32_t v = start;
      while (!done[v]) {
        done[v] = 1;
        loop.push_back(v);
        const auto it = nextOf.find(v);
        if (it == nextOf.end()) {
          broken = true;
          break;
        }
        v = it->second;
      }
      if (broken || v != start) {
        broken = true;
        break;
      }
      loops.push_back(std::move(loop));
    }
    if (broken) continue;
    // Close each hole with a fan around its middle, put on the surface.
    std::vector<char> drop(m.tri.size(), 0);
    for (uint32_t t : patch) drop[t] = 1;
    std::vector<sculpt::Tri> tris;
    tris.reserve(m.tri.size());
    for (uint32_t t = 0; t < m.tri.size(); ++t)
      if (!drop[t]) tris.push_back(m.tri[t]);
    for (const auto& loop : loops) {
      if (loop.size() < 3) return false;
      vec3 mid(0, 0, 0);
      for (uint32_t v : loop) mid += m.pos[v];
      mid /= static_cast<double>(loop.size());
      const uint32_t c = static_cast<uint32_t>(m.pos.size());
      m.pos.push_back(projectOnto(field, mid, edge));
      for (size_t i = 0; i < loop.size(); ++i) tris.push_back({loop[i], loop[(i + 1) % loop.size()], c});
    }
    m.tri = std::move(tris);
    // Leave out the points that were only inside the cut.
    std::vector<int64_t> remap(m.pos.size(), -1);
    std::vector<vec3> pos;
    for (sculpt::Tri& t : m.tri)
      for (uint32_t& v : t) {
        if (remap[v] < 0) {
          remap[v] = static_cast<int64_t>(pos.size());
          pos.push_back(m.pos[v]);
        }
        v = static_cast<uint32_t>(remap[v]);
      }
    m.pos = std::move(pos);
    return true;
  }
  return false;
}

// The mesher's triangles cross a rounded edge (where a wall meets the part's surface) at a slant, and
// it shows as a sawtooth along it. Flipping the diagonal of two triangles where that bends the
// surface less lines the edges up along it. Returns how many were flipped.
int flipAlong(sculpt::Mesh& m, int passes) {
  auto key = [](uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); };
  std::unordered_map<uint64_t, std::array<uint32_t, 2>> edges;
  edges.reserve(m.tri.size() * 2);
  for (uint32_t t = 0; t < m.tri.size(); ++t)
    for (int i = 0; i < 3; ++i) {
      auto [it, added] = edges.try_emplace(key(m.tri[t][i], m.tri[t][(i + 1) % 3]), std::array<uint32_t, 2>{t, t});
      if (!added) it->second[1] = t;
    }
  auto normal = [&](uint32_t a, uint32_t b, uint32_t c) { return la::cross(m.pos[b] - m.pos[a], m.pos[c] - m.pos[a]); };
  auto unit = [](const vec3& v) {
    const double l = la::length(v);
    return l > 0 ? v / l : v;
  };
  // The smallest angle in a triangle, as its sine-ish measure (area over the longest edge squared).
  auto shape = [&](uint32_t a, uint32_t b, uint32_t c) {
    const double longest = std::max({la::length2(m.pos[b] - m.pos[a]), la::length2(m.pos[c] - m.pos[b]), la::length2(m.pos[a] - m.pos[c])});
    return longest > 0 ? la::length(normal(a, b, c)) / longest : 0.0;
  };
  const double bentEnough = std::cos(8 * kPi / 180);
  int flipped = 0;
  const bool timing = std::getenv("DAEDAL_TIMING") != nullptr;
  auto t0 = std::chrono::steady_clock::now();
  auto lapf = [&](const char* what, long n) {
    if (!timing) return;
    const auto t = std::chrono::steady_clock::now();
    std::fprintf(stderr, "    flips %-14s %7.3f s  %ld\n", what, std::chrono::duration<double>(t - t0).count(), n);
    t0 = t;
  };
  lapf("edge map", (long)edges.size());
  // The first pass looks at every edge. Whether an edge can flip depends only on its two triangles
  // and on whether the edge across them exists, and a flip changes those only for edges with one of
  // its four points: later passes look at just those edges (in the same order), and flip exactly
  // what looking at every edge would.
  std::vector<char> touched(m.pos.size(), 0), touchedNext(m.pos.size(), 0);
  for (int pass = 0; pass < passes; ++pass) {
    std::vector<uint64_t> bent;
    for (const auto& [k, tris] : edges) {
      if (tris[0] == tris[1]) continue;
      const sculpt::Tri& t1 = m.tri[tris[0]];
      const sculpt::Tri& t2 = m.tri[tris[1]];
      if (pass > 0 && !(touched[t1[0]] || touched[t1[1]] || touched[t1[2]] || touched[t2[0]] || touched[t2[1]] || touched[t2[2]]))
        continue;
      if (la::dot(unit(normal(t1[0], t1[1], t1[2])), unit(normal(t2[0], t2[1], t2[2]))) < bentEnough) bent.push_back(k);
    }
    std::fill(touchedNext.begin(), touchedNext.end(), 0);
    lapf("find bent", (long)bent.size());
    int now = 0;
    for (uint64_t k : bent) {
      const auto found = edges.find(k);
      if (found == edges.end()) continue;
      uint32_t t1 = found->second[0], t2 = found->second[1];
      if (t1 == t2) continue;
      // t1 runs a -> b -> c, t2 runs b -> a -> d.
      const uint32_t u = static_cast<uint32_t>(k >> 32), w = static_cast<uint32_t>(k & 0xffffffffu);
      int i1 = 0;
      while (i1 < 3 && !(m.tri[t1][i1] == u && m.tri[t1][(i1 + 1) % 3] == w) && !(m.tri[t1][i1] == w && m.tri[t1][(i1 + 1) % 3] == u)) ++i1;
      if (i1 == 3) continue;
      const uint32_t a = m.tri[t1][i1], b = m.tri[t1][(i1 + 1) % 3], c = m.tri[t1][(i1 + 2) % 3];
      uint32_t d = a;
      for (uint32_t v : m.tri[t2])
        if (v != a && v != b) d = v;
      if (d == a || d == c || edges.count(key(c, d))) continue;
      const vec3 n1 = unit(normal(a, b, c)), n2 = unit(normal(b, a, d));
      const vec3 m1 = unit(normal(a, d, c)), m2 = unit(normal(d, b, c));
      // Bends less, faces the same way, and no thinner than before (or than about 15 degrees).
      if (la::dot(m1, m2) <= la::dot(n1, n2) + 1e-3) continue;
      if (la::dot(m1, n1 + n2) <= 0 || la::dot(m2, n1 + n2) <= 0) continue;
      const double before = std::min(shape(a, b, c), shape(b, a, d));
      if (std::min(shape(a, d, c), shape(d, b, c)) < std::min(before, 0.2)) continue;
      m.tri[t1] = {a, d, c};
      m.tri[t2] = {d, b, c};
      for (uint32_t v : {a, b, c, d}) touchedNext[v] = 1;
      edges.erase(found);
      edges[key(c, d)] = {t1, t2};
      for (auto& t : edges[key(a, d)])
        if (t == t2) t = t1;
      for (auto& t : edges[key(b, c)])
        if (t == t1) t = t2;
      ++now;
    }
    lapf("flip", now);
    std::swap(touched, touchedNext);
    flipped += now;
    if (!now) break;
  }
  return flipped;
}

// Collapses the shortest edge of each of these triangles into its middle, where that keeps the mesh
// a closed surface (the two ends share exactly the two neighbours across the edge). Returns false
// if no edge could go.
bool collapseShortest(sculpt::Mesh& m, const std::vector<uint32_t>& bad) {
  const size_t n = m.pos.size();
  const Adjacency ring = neighboursOf(m);
  std::vector<char> touched(n, 0);
  std::vector<int64_t> into(n, -1);
  int collapsed = 0;
  for (uint32_t t : bad) {
    const sculpt::Tri& tri = m.tri[t];
    int best = -1;
    double shortest = 1e300;
    for (int i = 0; i < 3; ++i) {
      const double l = la::length(m.pos[tri[i]] - m.pos[tri[(i + 1) % 3]]);
      if (l < shortest) {
        shortest = l;
        best = i;
      }
    }
    const uint32_t a = tri[best], b = tri[(best + 1) % 3];
    if (touched[a] || touched[b] || ring[a].size() <= 3 || ring[b].size() <= 3) continue;
    std::vector<uint32_t> common;
    std::set_intersection(ring[a].begin(), ring[a].end(), ring[b].begin(), ring[b].end(), std::back_inserter(common));
    if (common.size() != 2) continue;
    m.pos[a] = (m.pos[a] + m.pos[b]) * 0.5;
    into[b] = a;
    // Nothing next to this collapse moves this time (the rings above stay true).
    for (uint32_t v : {a, b}) {
      touched[v] = 1;
      for (uint32_t u : ring[v]) touched[u] = 1;
    }
    ++collapsed;
  }
  if (!collapsed) return false;
  std::vector<sculpt::Tri> tris;
  tris.reserve(m.tri.size());
  for (sculpt::Tri t : m.tri) {
    for (uint32_t& v : t)
      if (into[v] >= 0) v = static_cast<uint32_t>(into[v]);
    if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2]) tris.push_back(t);
  }
  m.tri = std::move(tris);
  std::vector<int64_t> remap(m.pos.size(), -1);
  std::vector<vec3> pos;
  for (sculpt::Tri& t : m.tri)
    for (uint32_t& v : t) {
      if (remap[v] < 0) {
        remap[v] = static_cast<int64_t>(pos.size());
        pos.push_back(m.pos[v]);
      }
      v = static_cast<uint32_t>(remap[v]);
    }
  m.pos = std::move(pos);
  return true;
}

}  // namespace

bool build(const sculpt::Mesh& part, const Settings& in, sculpt::Mesh* out, std::string* error, Report* report,
           Progress* progress) {
  auto fail = [&](const char* text) {
    if (error) *error = text;
    return false;
  };
  auto stage = [&](int which, double fraction) {
    if (!progress) return;
    progress->stage = which;
    progress->fraction = fraction;
  };
  auto cancelled = [&] { return progress && progress->cancel.load(); };
  Settings s = in;
  if (part.tri.empty()) return fail("There is no part to turn into a lattice.");
  if (!(s.cell > 0) || !(s.wall > 0)) return fail("The cell size and the wall must be more than 0 mm.");
  if (s.wall >= s.cell * 0.9) return fail("The walls are as thick as the cells: make the walls thinner or the cells larger.");
  const bool skinKind = s.kind == Kind::VoronoiSkin;
  if (skinKind && s.skin <= 0) s.skin = std::max(1.6, s.wall);
  const double edge = usedDetail(part, s);
  if (report) report->detail = edge;
  stage(0, 0.0);
  const double band = std::max({s.skin, s.wall, edge}) * 1.5 + 3 * edge;
  const sculpt::SurfaceField inside(part, band);
  // Edges rounded with a radius of about two triangles, where a wall meets the part's surface or the
  // skin: a crease sharper than the triangles comes out as a sawtooth. Not more than half a wall,
  // or the walls would come out as round rods.
  const double r = std::min(std::max(2 * edge, s.wall * 0.4), s.wall * 0.5);
  auto smoothMin = [r](double a, double b) {
    if (a >= r || b >= r) return std::min(a, b);
    return r - std::sqrt((r - a) * (r - a) + (r - b) * (r - b));
  };
  auto smoothMax = [&](double a, double b) { return -smoothMin(-a, -b); };
  // Where a wall leaves the part almost along its surface, the two meet at a knife edge that tapers
  // to nothing, far thinner than the triangles (the mesher tangles there). Such a wedge is cut off
  // where it gets thinner than blunt (a + b is about its thickness), and a hairline gap between two
  // pieces is filled the same way. Only narrow wedges (the two surfaces facing each other, more than
  // 60 degrees past square) are touched: ordinary corners keep their shape.
  const double blunt = edge * 1.5;
  // How much two surfaces face each other at p, from their slopes: 1 in a thin wedge, 0 at a square
  // corner or wider.
  auto narrowness = [](const vec3& ga, const vec3& gb) {
    const double la2 = la::dot(ga, ga), lb2 = la::dot(gb, gb);
    if (la2 <= 0 || lb2 <= 0) return 0.0;
    const double facing = -la::dot(ga, gb) / std::sqrt(la2 * lb2);
    return std::clamp((facing - 0.5) * 2, 0.0, 1.0);
  };
  // The solid inside both (a, slope ga) and (b, slope gb); slopes are only worked out (slope()) near
  // a thin wedge, where the cut could matter.
  auto meet = [&](double a, double b, auto&& slopes) {
    const double rounded = smoothMin(a, b);
    // The cut only takes away solid (both inside) where the wedge is thin: a + b below 2 blunt. It
    // is also worked out a little outside, so the value has no step near the surface.
    if (std::min(a, b) <= -blunt || std::max(a, b) >= blunt) return rounded;
    const auto [ga, gb] = slopes();
    return std::min(rounded, a + b - blunt * narrowness(ga, gb));
  };
  // The solid inside either.
  auto join = [&](double a, double b, auto&& slopes) {
    const double rounded = smoothMax(a, b);
    if (std::min(a, b) <= -blunt || std::max(a, b) >= blunt) return rounded;
    const auto [ga, gb] = slopes();
    return std::max(rounded, a + b + blunt * narrowness(ga, gb));
  };
  auto field = [&](const vec3& p) {
    const double body = inside(p);  // > 0 inside the part
    if (body < -2 * edge) return body;  // well outside: nothing there
    const double walls = pattern(s, p, nullptr);
    // The slopes of the part's distance and of the pattern, worked out once and only if asked.
    bool sloped = false;
    vec3 gBody, gPattern;
    auto slope = [&] {
      if (!sloped) {
        inside(p, &gBody);
        pattern(s, p, &gPattern);
        sloped = true;
      }
    };
    const double shell = std::min(body, s.skin - body);
    auto gShell = [&] { return body < s.skin - body ? gBody : -gBody; };
    if (skinKind) {  // the shell, cut into the web
      return meet(shell, walls, [&] {
        slope();
        return std::pair<vec3, vec3>(gShell(), gPattern);
      });
    }
    const double core = meet(body, walls, [&] {
      slope();
      return std::pair<vec3, vec3>(gBody, gPattern);
    });
    if (s.skin <= 0) return core;
    return join(core, shell, [&] {
      slope();
      return std::pair<vec3, vec3>(body < walls ? gBody : gPattern, gShell());
    });
  };
  vec3 lo, hi;
  sculpt::bounds(part, &lo, &hi);
  const vec3 pad(2 * edge, 2 * edge, 2 * edge);
  std::string why;
  if (!meshField(field, lo - pad, hi + pad, edge, out, &why, report, progress, 0.02)) {
    if (why == "empty") return fail("Nothing came out: make the walls thicker or the cells smaller than the part.");
    if (why == "open") return fail("The lattice did not come out closed.");
    return fail("Stopped.");
  }
  return true;
}

bool meshField(const std::function<double(const vec3&)>& field, const vec3& lo, const vec3& hi, double edge, sculpt::Mesh* out,
               std::string* error, Report* report, Progress* progress, double crumbs) {
  auto fail = [&](const char* text) {
    if (error) *error = text;
    return false;
  };
  auto stage = [&](int which, double fraction) {
    if (!progress) return;
    progress->stage = which;
    progress->fraction = fraction;
  };
  auto cancelled = [&] { return progress && progress->cancel.load(); };
  const bool timing = std::getenv("DAEDAL_TIMING") != nullptr;
  auto tick = std::chrono::steady_clock::now();
  auto lap = [&](const char* what) {
    if (!timing) return;
    const auto t = std::chrono::steady_clock::now();
    std::fprintf(stderr, "  %-22s %7.3f s\n", what, std::chrono::duration<double>(t - tick).count());
    tick = t;
  };
  if (report) report->detail = edge;
  const vec3 span = hi - lo;
  const double samples = 2.0 * (span.x / edge + 1) * (span.y / edge + 1) * (span.z / edge + 1);
  std::atomic<uint64_t> calls{0}, exact{0};
  stage(1, 0.02);
  // Far from the surface only the field's sign matters, and the sign is known without asking: a
  // coarse grid (every `step`) is worked out first, and a point whose nearest coarse value is
  // further from 0 than the field can change over the distance to that node plus a couple of fine
  // steps keeps that value. Points near the surface, the only ones the mesher reads values from
  // (and every edge of theirs that crosses it), are worked out exactly, so the mesh is the same.
  const double step = 3 * edge;
  const int cx = static_cast<int>(std::ceil(span.x / step)) + 1, cy = static_cast<int>(std::ceil(span.y / step)) + 1,
            cz = static_cast<int>(std::ceil(span.z / step)) + 1;
  const bool coarse = static_cast<double>(cx) * cy * cz < 2e7;
  std::vector<float> grid(coarse ? static_cast<size_t>(cx) * cy * cz : 0);
  double reach = 0;  // how far from 0 a coarse value must be for its neighbourhood to be skipped
  if (coarse) {
    parallelFor(grid.size(), [&](size_t i) {
      const int x = static_cast<int>(i % cx), y = static_cast<int>((i / cx) % cy), z = static_cast<int>(i / (size_t(cx) * cy));
      grid[i] = static_cast<float>(field(lo + vec3(x, y, z) * step));
    });
    // How fast the field changes, from neighbouring coarse values (with room to spare).
    std::atomic<uint32_t> steepest{0};
    parallelFor(grid.size(), [&](size_t i) {
      const int x = static_cast<int>(i % cx), y = static_cast<int>((i / cx) % cy), z = static_cast<int>(i / (size_t(cx) * cy));
      float most = 0;
      if (x + 1 < cx) most = std::max(most, std::abs(grid[i + 1] - grid[i]));
      if (y + 1 < cy) most = std::max(most, std::abs(grid[i + cx] - grid[i]));
      if (z + 1 < cz) most = std::max(most, std::abs(grid[i + size_t(cx) * cy] - grid[i]));
      const uint32_t bits = static_cast<uint32_t>(most / step * 1000);
      uint32_t seen = steepest.load();
      while (bits > seen && !steepest.compare_exchange_weak(seen, bits)) {
      }
    });
    const double slope = std::clamp(1.5 * steepest.load() / 1000.0, 1.0, 6.0);
    reach = slope * (step * 0.8661 + 2 * edge);
  }
  auto sdf = [&](vec3 p) {
    const uint64_t c = calls.fetch_add(1, std::memory_order_relaxed);
    if (progress) {
      if (progress->cancel.load(std::memory_order_relaxed)) return -1.0;  // empty from here on: it finishes at once
      if ((c & 16383) == 0) progress->fraction = 0.02 + 0.5 * std::min(1.0, c / samples);
    }
    if (coarse) {
      const vec3 q = (p - lo) / step;
      const int x = std::clamp(static_cast<int>(std::lround(q.x)), 0, cx - 1), y = std::clamp(static_cast<int>(std::lround(q.y)), 0, cy - 1),
                z = std::clamp(static_cast<int>(std::lround(q.z)), 0, cz - 1);
      const double near = grid[x + size_t(cx) * (y + size_t(cy) * z)];
      if (std::abs(near) > reach && la::length(p - (lo + vec3(x, y, z) * step)) <= step * 0.8661 + 1e-9) return near;
    }
    exact.fetch_add(1, std::memory_order_relaxed);
    return field(p);
  };
  manifold::Manifold solid = manifold::Manifold::LevelSet(sdf, manifold::Box(lo, hi), edge, 0, -1);
  if (timing)
    std::fprintf(stderr, "  field calls %.2f M, worked out %.2f M (+%.2f M coarse), reach %.2f, %zu triangles\n", calls.load() / 1e6,
                 exact.load() / 1e6, grid.size() / 1e6, reach, solid.NumTri());
  lap("level set");
  if (cancelled()) return fail("stopped");
  if (solid.Status() != manifold::Manifold::Error::NoError || solid.IsEmpty()) return fail("empty");
  // Loose crumbs (smaller than `crumbs` of the largest piece) are left out. Done on the plain mesh,
  // in one pass: pieces are found by the points their triangles share, and dropping whole pieces
  // keeps every other one closed. (Splitting the solid and joining the kept pieces with a union
  // took minutes on one core for a big sponge, with every piece copied once more.)
  stage(2, 0.53);
  sculpt::Mesh mesh = sculpt::fromManifold(solid);
  solid = manifold::Manifold();  // its memory is needed for what follows
  if (crumbs > 0 && !mesh.tri.empty()) {
    std::vector<uint32_t> parent(mesh.pos.size());
    for (uint32_t v = 0; v < parent.size(); ++v) parent[v] = v;
    auto root = [&](uint32_t v) {
      while (parent[v] != v) v = parent[v] = parent[parent[v]];
      return v;
    };
    for (const sculpt::Tri& t : mesh.tri)
      for (int k = 1; k < 3; ++k) {
        const uint32_t x = root(t[0]), y = root(t[k]);
        if (x != y) parent[std::max(x, y)] = std::min(x, y);
      }
    // Each piece's volume, by its root.
    std::unordered_map<uint32_t, double> volume;
    for (const sculpt::Tri& t : mesh.tri)
      volume[root(t[0])] += la::dot(mesh.pos[t[0]], la::cross(mesh.pos[t[1]], mesh.pos[t[2]])) / 6.0;
    if (volume.size() > 1) {
      double largest = 0;
      for (const auto& [piece, v] : volume) largest = std::max(largest, v);
      int dropped = 0;
      for (const auto& [piece, v] : volume) dropped += v < largest * crumbs;
      if (dropped) {
        sculpt::Mesh kept;
        std::vector<uint32_t> renumber(mesh.pos.size(), UINT32_MAX);
        for (const sculpt::Tri& t : mesh.tri) {
          if (volume[root(t[0])] < largest * crumbs) continue;
          sculpt::Tri n;
          for (int k = 0; k < 3; ++k) {
            if (renumber[t[k]] == UINT32_MAX) {
              renumber[t[k]] = static_cast<uint32_t>(kept.pos.size());
              kept.pos.push_back(mesh.pos[t[k]]);
            }
            n[k] = renumber[t[k]];
          }
          kept.tri.push_back(n);
        }
        mesh = std::move(kept);
      }
      if (report) report->droppedCrumbs = dropped;
    }
  }
  lap("crumbs + convert");
  if (cancelled()) return fail("stopped");
  stage(3, 0.56);
  std::vector<uint32_t> left;
  smoothOnto(mesh, field, edge, 2, progress, 0.56, 0.8, nullptr);
  lap("smooth x2");
  {
    const size_t gone = coarsen(mesh, field, edge, progress, 0.8, 0.85);
    if (timing) std::fprintf(stderr, "  coarsen: %zu collapses, %zu triangles\n", gone, mesh.tri.size());
    smoothOnto(mesh, field, edge, 1, progress, 0.85, 0.88, nullptr);
    lap("coarsen + smooth");
  }
  flipAlong(mesh, 4);
  lap("flips");
  int rough = smoothOnto(mesh, field, edge, 1, progress, 0.8, 0.93, &left);
  lap("smooth + check");
  for (int attempt = 0; attempt < 16 && !left.empty() && !cancelled(); ++attempt) {
    const sculpt::Mesh before = mesh;
    const std::vector<uint32_t> badBefore = left;
    // Each try only changes the mesh around the bad spots: only there is it smoothed and checked
    // (the whole mesh is checked once more at the end).
    std::vector<vec3> spots;
    for (uint32_t t : left) spots.push_back((mesh.pos[mesh.tri[t][0]] + mesh.pos[mesh.tri[t][1]] + mesh.pos[mesh.tri[t][2]]) / 3.0);
    bool better = false;
    if (collapseShortest(mesh, left)) {
      const int relaxed = smoothOnto(mesh, field, edge, 0, progress, 0.93, 0.98, &left, &spots);
      better = left.size() < badBefore.size();
      if (better) rough += relaxed;
      else {
        mesh = before;
        left = badBefore;
      }
    }
    if (!better && cutOut(mesh, left, field, edge)) {
      const int relaxed = smoothOnto(mesh, field, edge, 1, progress, 0.93, 0.98, &left, &spots);
      better = left.size() < badBefore.size();
      if (better) rough += relaxed;
      else {
        mesh = before;
        left = badBefore;
      }
    }
    if (!better) break;
  }
  lap("repairs");
  if (cancelled()) return fail("stopped");
  if (report) report->keptRough = rough;
  if (report) report->clean = left.empty() && sculpt::crossingTriangles(mesh).empty();
  lap("final check");
  if (!sculpt::closedManifold(mesh)) return fail("open");
  *out = std::move(mesh);
  if (progress) progress->fraction = 1.0;
  return true;
}

}  // namespace lattice
