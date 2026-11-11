#include "effects.h"

#include <manifold/cross_section.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <tuple>

namespace fx {

using sculpt::Mesh;
using sculpt::vec3;
namespace la = manifold::la;

namespace {

constexpr double kPi = 3.14159265358979323846;

double frac(double x) { return x - std::floor(x); }
double tri(double x) { return 1.0 - std::abs(2.0 * frac(x) - 1.0); }  // 0 at whole numbers, 1 halfway
double smooth(double x) {
  x = std::clamp(x, 0.0, 1.0);
  return x * x * (3 - 2 * x);
}

// Value noise: a smooth random 0 .. 1 field on a unit grid.
double hash(int x, int y, int seed) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(seed) * 2147483647u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return ((h ^ (h >> 16)) & 0xffffff) / double(0xffffff);
}
double noise(double x, double y, int seed) {
  const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
  const double tx = smooth(x - x0), ty = smooth(y - y0);
  const double a = hash(x0, y0, seed), b = hash(x0 + 1, y0, seed), c = hash(x0, y0 + 1, seed), d = hash(x0 + 1, y0 + 1, seed);
  return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}
double fbm(double x, double y, int seed) {
  return 0.55 * noise(x, y, seed) + 0.3 * noise(2.03 * x, 2.03 * y, seed + 7) + 0.15 * noise(4.1 * x, 4.1 * y, seed + 13);
}

// The two nearest cell points of a jittered grid (cells `cell` wide): their distances.
void nearestTwo(double u, double v, double cell, int seed, double* d1, double* d2) {
  const int cx = static_cast<int>(std::floor(u / cell)), cy = static_cast<int>(std::floor(v / cell));
  *d1 = *d2 = 1e300;
  for (int j = cy - 1; j <= cy + 1; ++j)
    for (int i = cx - 1; i <= cx + 1; ++i) {
      const double px = (i + 0.15 + 0.7 * hash(i, j, seed)) * cell, py = (j + 0.15 + 0.7 * hash(i, j, seed + 31)) * cell;
      const double d = std::hypot(u - px, v - py);
      if (d < *d1) {
        *d2 = *d1;
        *d1 = d;
      } else if (d < *d2) {
        *d2 = d;
      }
    }
}

}  // namespace

double textureValue(const TextureSettings& s, double u, double v) {
  const double size = std::max(0.05, s.size);
  if (s.angle != 0) {
    const double a = s.angle * kPi / 180, c = std::cos(a), sn = std::sin(a);
    const double ru = u * c - v * sn, rv = u * sn + v * c;
    u = ru;
    v = rv;
  }
  switch (s.kind) {
    case Texture::Knurl:  // diamond knurling: little pyramids
      return std::min(tri((u + v) / size), tri((u - v) / size));
    case Texture::Ribs:
      return 0.5 - 0.5 * std::cos(2 * kPi * u / size);
    case Texture::Hexes: {
      // Raised hexagon tiles with grooves between them; flat sides up and down.
      const double R = size * 0.5;  // inradius of one tile
      const double colStep = R * 2 / std::sqrt(3.0) * 1.5;
      const int col = static_cast<int>(std::floor(u / colStep));
      double best = 1e300;
      for (int c = col - 1; c <= col + 2; ++c) {
        const double cx = c * colStep;
        const double offset = (c & 1) ? R : 0;
        const int row = static_cast<int>(std::floor((v - offset) / (2 * R)));
        for (int r = row - 1; r <= row + 2; ++r) {
          const double qx = std::abs(u - cx), qy = std::abs(v - (offset + r * 2 * R));
          best = std::min(best, std::max(qy, 0.8660254 * qx + 0.5 * qy));
        }
      }
      return smooth((R - best - size * 0.08) / (size * 0.12));
    }
    case Texture::Dots: {
      // Domes on a staggered grid.
      const double r = size * 0.33;
      double best = 1e300;
      const int row = static_cast<int>(std::floor(v / (size * 0.866)));
      for (int j = row - 1; j <= row + 1; ++j) {
        const double off = (j & 1) ? size * 0.5 : 0;
        const int i = static_cast<int>(std::round((u - off) / size));
        for (int k = i - 1; k <= i + 1; ++k) best = std::min(best, std::hypot(u - (k * size + off), v - j * size * 0.866));
      }
      return best < r ? std::sqrt(1 - (best / r) * (best / r)) : 0.0;
    }
    case Texture::Wood: {
      // Grain lines along v, wandering a little, with soft valleys.
      const double wobble = 1.6 * fbm(u / (3 * size), v / (14 * size), s.seed);
      const double g = 0.5 + 0.5 * std::sin(2 * kPi * (u / size + wobble));
      return std::pow(g, 1.6) * (0.75 + 0.25 * noise(u / size, v / (4 * size), s.seed + 3));
    }
    case Texture::Leather: {
      // Pebbled cells with soft grooves, each a slightly different height.
      double d1, d2;
      nearestTwo(u, v, size, s.seed, &d1, &d2);
      const double groove = smooth((d2 - d1) * 0.5 / (size * 0.14));
      return groove * (0.8 + 0.2 * noise(u / (size * 0.7), v / (size * 0.7), s.seed + 5));
    }
    case Texture::Weave: {
      // Basket (carbon) weave: strands one way and the other in a checkerboard, each a rounded band.
      const int i = static_cast<int>(std::floor(u / size)), j = static_cast<int>(std::floor(v / size));
      const double fu = u / size - i, fv = v / size - j;
      const bool along = ((i + j) & 1) == 0;
      const double across = along ? fv : fu, length = along ? fu : fv;
      return std::sqrt(std::max(0.0, std::sin(kPi * across))) * (0.7 + 0.3 * std::sin(kPi * length));
    }
    case Texture::Picture: {
      if (s.picture.width <= 0 || s.picture.height <= 0) return 0;
      const double tall = size * s.picture.height / s.picture.width;
      return s.picture.sample(frac(u / size), frac(-v / tall));
    }
  }
  return 0;
}

double textureDetail(const Mesh& part, const TextureSettings& s) {
  double edge = s.detail > 0 ? s.detail : std::clamp(std::min(s.size / 8, s.depth / 2), 0.1, 1.0);
  // The grid fills the part's whole box: keep it to about 30 million samples.
  vec3 lo, hi;
  sculpt::bounds(part, &lo, &hi);
  const vec3 span = hi - lo + vec3(2, 2, 2) * (s.depth + 1);
  return std::max(edge, std::cbrt(span.x * span.y * span.z / 3.0e7));
}

double textureTriangles(const Mesh& part, const TextureSettings& s) {
  double area = 0;
  for (const sculpt::Tri& t : part.tri) area += 0.5 * la::length(la::cross(part.pos[t[1]] - part.pos[t[0]], part.pos[t[2]] - part.pos[t[0]]));
  const double edge = textureDetail(part, s);
  // A deep, fine pattern has more surface than the part it is on.
  return area / (edge * edge * 0.433) * 1.6 * (1 + 1.5 * std::min(1.0, s.depth / s.size));
}

bool texture(const Mesh& part, const TextureSettings& in, Mesh* out, std::string* error, lattice::Report* report,
             lattice::Progress* progress) {
  auto fail = [&](const char* text) {
    if (error) *error = text;
    return false;
  };
  TextureSettings s = in;
  if (part.tri.empty()) return fail("There is no part to put a texture on.");
  if (!(s.size > 0) || !(s.depth > 0)) return fail("The size and the depth must be more than 0 mm.");
  if (s.kind == Texture::Picture && (s.picture.width <= 0 || s.picture.height <= 0)) return fail("Choose a picture first.");
  const double edge = textureDetail(part, s);
  if (progress) {
    progress->stage = 0;
    progress->fraction = 0;
  }
  const double band = s.depth + 4 * edge;
  const sculpt::SurfaceField body(part, band);
  vec3 lo, hi;
  sculpt::bounds(part, &lo, &hi);
  const vec3 mid = (lo + hi) * 0.5;
  // Round wrap: a whole number of repeats round the part, sized at its average radius.
  const double radius = std::max(0.5, ((hi.x - lo.x) + (hi.y - lo.y)) * 0.25);
  const double turns = std::max(1.0, std::round(2 * kPi * radius / s.size));
  const double perRadian = turns * s.size / (2 * kPi);
  const double sign = s.raised ? 1.0 : -1.0;
  auto field = [&](const vec3& p) {
    vec3 slope;
    const double d = body(p, &slope);
    if (std::abs(d) > s.depth + 2 * edge || la::dot(slope, slope) < 0.5) return s.where == 1 ? d : std::min(d, p.z - lo.z);
    const vec3 n = -slope;  // outward
    // Where the texture goes, faded in over a range of slopes so it has no step.
    double mask = 1;
    if (s.where == 0) mask = smooth((n.z + 0.92) / 0.3);
    else if (s.where == 2) mask = smooth((0.92 - std::abs(n.z)) / 0.3);
    if (mask <= 0) return s.where == 1 ? d : std::min(d, p.z - lo.z);
    double t = 0;
    if (s.wrap == 1) {
      const double side = std::pow(std::max(0.0, 1 - n.z * n.z), 3), top = std::pow(std::abs(n.z), 6);
      const double sum = side + top;
      if (side > 1e-6) t += side / sum * textureValue(s, std::atan2(p.y - mid.y, p.x - mid.x) * perRadian, p.z);
      if (top > 1e-6) t += top / sum * textureValue(s, p.x, n.z > 0 ? p.y : -p.y);
    } else {
      // From every side (triplanar): each axis's picture, blended by how much the surface faces it.
      const double wx = std::pow(std::abs(n.x), 6), wy = std::pow(std::abs(n.y), 6), wz = std::pow(std::abs(n.z), 6);
      const double sum = wx + wy + wz;
      if (wx > 1e-6 * sum) t += wx / sum * textureValue(s, n.x > 0 ? p.y : -p.y, p.z);
      if (wy > 1e-6 * sum) t += wy / sum * textureValue(s, n.y > 0 ? -p.x : p.x, p.z);
      if (wz > 1e-6 * sum) t += wz / sum * textureValue(s, p.x, n.z > 0 ? p.y : -p.y);
    }
    const double f = d + sign * s.depth * mask * t;
    // Unless asked for everywhere, nothing goes below the bottom: it stays flat on the plate (the
    // texture round the bottom rim would otherwise reach under it).
    return s.where == 1 ? f : std::min(f, p.z - lo.z);
  };
  const vec3 pad(band, band, band);
  std::string why;
  if (!lattice::meshField(field, lo - pad, hi + pad, edge, out, &why, report, progress, 0.0)) {
    if (why == "empty") return fail("Nothing came out. Is the depth more than the part is thick?");
    if (why == "open") return fail("The texture did not come out closed. Try a coarser detail.");
    return fail("Stopped.");
  }
  return true;
}

// ---- Smooth merge ------------------------------------------------------------------------------

double blendDetail(const std::vector<Mesh>& solids, double radius) {
  vec3 lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300);
  for (const Mesh& m : solids) {
    vec3 a, b;
    sculpt::bounds(m, &a, &b);
    lo = la::min(lo, a);
    hi = la::max(hi, b);
  }
  const double edge = std::clamp(radius / 6, 0.15, 0.8);
  const vec3 span = hi - lo + vec3(2, 2, 2) * (radius + 1);
  return std::max(edge, std::cbrt(std::max(0.0, span.x * span.y * span.z) / 2.5e7));
}

bool blend(const std::vector<Mesh>& solids, const std::vector<Mesh>& holes, double radius, Mesh* out, std::string* error,
           lattice::Progress* progress) {
  auto fail = [&](const char* text) {
    if (error) *error = text;
    return false;
  };
  if (solids.empty()) return fail("Nothing solid to merge.");
  const double k = std::max(0.05, radius);
  const double edge = blendDetail(solids, k);
  const double band = k * 1.5 + 4 * edge;
  struct Part {
    std::unique_ptr<sculpt::SurfaceField> field;
    vec3 lo, hi;
    bool hole;
  };
  std::vector<Part> parts;
  vec3 lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300);
  for (int pass = 0; pass < 2; ++pass)
    for (const Mesh& m : pass == 0 ? solids : holes) {
      if (m.tri.empty()) continue;
      Part part;
      sculpt::bounds(m, &part.lo, &part.hi);
      part.field = std::make_unique<sculpt::SurfaceField>(m, band);
      part.hole = pass == 1;
      if (!part.hole) {
        lo = la::min(lo, part.lo);
        hi = la::max(hi, part.hi);
      }
      parts.push_back(std::move(part));
    }
  // Round union and cut on distances (here positive outside): where two surfaces meet, a true
  // round fillet of radius k tangent to both. `w` says how much of a blend a point is in: 1 at a
  // join, 0 where the surface is just one part's own.
  auto roundUnion = [k](double a, double b, double* w) {
    const double u = std::max(k - a, 0.0), v = std::max(k - b, 0.0);
    *w = std::max(*w, std::clamp(std::min(u, v) / k, 0.0, 1.0));
    return std::max(k, std::min(a, b)) - std::sqrt(u * u + v * v);
  };
  auto roundCut = [k](double a, double b, double* w) {  // a, less b (b given as the distance to the hole)
    const double c = -b;
    const double u = std::max(k + a, 0.0), v = std::max(k + c, 0.0);
    *w = std::max(*w, std::clamp(std::min(u, v) / k, 0.0, 1.0));
    return std::min(-k, std::max(a, c)) + std::sqrt(u * u + v * v);
  };
  // Inside-positive, like the parts' own fields.
  auto field = [&](const vec3& p, double* w) {
    *w = 0;
    double f = band;
    bool any = false;
    for (const Part& part : parts) {
      if (part.hole) continue;
      // Far outside this part's box: it cannot reach this point.
      const vec3 q = la::max(part.lo - p, p - part.hi);
      if (la::length(la::max(q, vec3(0, 0, 0))) > band) continue;
      const double d = -(*part.field)(p);
      f = any ? roundUnion(f, d, w) : d;
      any = true;
    }
    for (const Part& part : parts) {
      if (!part.hole) continue;
      const vec3 q = la::max(part.lo - p, p - part.hi);
      if (la::length(la::max(q, vec3(0, 0, 0))) > band) continue;
      f = roundCut(f, -(*part.field)(p), w);
    }
    return -f;
  };
  const vec3 pad(band, band, band);
  std::string why;
  auto mesh = [&](double shift, Mesh* into) {
    // Shifted off the parts' own surfaces only; the blends themselves stay where they are.
    auto shifted = [&](const vec3& p) {
      double w = 0;
      const double f = field(p, &w);
      return f - shift * (1 - w);
    };
    if (lattice::meshField(shifted, lo - pad, hi + pad, edge, into, &why, nullptr,
                           progress, 0.0))
      return true;
    if (why == "empty") return fail("Nothing came out of the smooth merge.");
    if (why == "open") return fail("The smooth merge did not come out closed.");
    return fail("Stopped.");
  };
  // A meshed field rounds off every sharp edge a little (a saw-tooth along it). So the parts keep
  // their own exact surfaces, and the field only adds the blends: meshed a hair inside (it then
  // only shows where it stands out of the parts, at the joins) and, for soft-edged holes, a hair
  // outside (it then only cuts where it cuts into them, at the rims).
  const double hair = 0.6 * edge;
  std::vector<manifold::Manifold> exact;
  for (const Mesh& m : solids) {
    if (m.tri.empty()) continue;
    exact.push_back(toManifold(m));
    if (exact.back().IsEmpty()) {
      exact.clear();
      break;
    }
  }
  std::vector<manifold::Manifold> cutters;
  for (const Mesh& m : holes) {
    if (m.tri.empty() || exact.empty()) continue;
    cutters.push_back(toManifold(m));
    if (cutters.back().IsEmpty()) exact.clear();
  }
  if (exact.empty()) return mesh(0, out);  // not closed solids: the meshed field it is
  Mesh inner;
  if (!mesh(hair, &inner)) return false;
  manifold::Manifold joined = manifold::Manifold::BatchBoolean(exact, manifold::OpType::Add);
  const manifold::Manifold fill = toManifold(inner);
  if (fill.IsEmpty()) return mesh(0, out);
  joined = joined + fill;
  if (!cutters.empty()) {
    Mesh outer;
    if (!mesh(-hair, &outer)) return false;
    const manifold::Manifold keep = toManifold(outer);
    if (keep.IsEmpty()) return mesh(0, out);
    joined = (joined - manifold::Manifold::BatchBoolean(cutters, manifold::OpType::Add)) ^ keep;
  }
  if (joined.Status() != manifold::Manifold::Error::NoError || joined.IsEmpty()) return mesh(0, out);
  *out = sculpt::fromManifold(joined);
  return true;
}

// ---- Restyle -----------------------------------------------------------------------------------

bool lowPoly(const Mesh& part, double facet, double jitter, int seed, Mesh* out, std::string* error, lattice::Progress* progress) {
  auto fail = [&](const std::string& text) {
    if (error) *error = text;
    return false;
  };
  if (part.tri.empty()) return fail("There is no part to restyle.");
  if (!(facet > 0)) return fail("The facet size must be more than 0 mm.");
  if (progress) {
    progress->stage = 1;
    progress->fraction = 0.1;
  }
  Mesh coarse;
  std::string why;
  if (!sculpt::remesh(part, facet, &coarse, &why)) return fail(why.empty() ? "The part could not be made low-poly." : why);
  if (progress && progress->cancel) return fail("Stopped.");
  if (progress) progress->fraction = 0.7;
  // Nudge every corner a little, along the surface and in and out, for a hand-cut look; less if
  // that folds the mesh.
  if (jitter > 0) {
    std::vector<vec3> normal(coarse.pos.size(), vec3(0, 0, 0));
    for (const sculpt::Tri& t : coarse.tri) {
      const vec3 n = la::cross(coarse.pos[t[1]] - coarse.pos[t[0]], coarse.pos[t[2]] - coarse.pos[t[0]]);
      for (uint32_t v : t) normal[v] += n;
    }
    std::mt19937 random(static_cast<uint32_t>(seed) * 2654435761u + 1);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    std::vector<vec3> push(coarse.pos.size());
    for (size_t v = 0; v < push.size(); ++v) {
      const double l = la::length(normal[v]);
      const vec3 n = l > 0 ? normal[v] / l : vec3(0, 0, 1);
      push[v] = n * unit(random) + vec3(unit(random), unit(random), unit(random)) * 0.5;
    }
    for (double amount = std::clamp(jitter, 0.0, 1.0) * facet * 0.22; amount > facet * 0.01; amount *= 0.5) {
      Mesh moved = coarse;
      for (size_t v = 0; v < moved.pos.size(); ++v) moved.pos[v] += push[v] * amount;
      if (!sculpt::selfIntersects(moved)) {
        coarse = std::move(moved);
        break;
      }
    }
  }
  if (!sculpt::closedManifold(coarse)) return fail("The low-poly version did not come out closed.");
  *out = std::move(coarse);
  if (progress) progress->fraction = 1;
  return true;
}

size_t voxelCount(const Mesh& part, double cube) {
  if (!(cube > 0)) return 0;
  vec3 lo, hi;
  sculpt::bounds(part, &lo, &hi);
  const vec3 span = hi - lo;
  return static_cast<size_t>(std::ceil(span.x / cube)) * static_cast<size_t>(std::ceil(span.y / cube)) *
         static_cast<size_t>(std::ceil(span.z / cube));
}

bool voxel(const Mesh& part, double cube, Mesh* out, std::string* error, lattice::Progress* progress) {
  auto fail = [&](const char* text) {
    if (error) *error = text;
    return false;
  };
  if (part.tri.empty()) return fail("There is no part to restyle.");
  if (!(cube > 0)) return fail("The cube size must be more than 0 mm.");
  if (voxelCount(part, cube) > 4000000) return fail("That is more than 4 million cubes: make the cubes larger.");
  vec3 lo, hi;
  sculpt::bounds(part, &lo, &hi);
  const int nx = std::max(1, static_cast<int>(std::ceil((hi.x - lo.x) / cube - 1e-9)));
  const int ny = std::max(1, static_cast<int>(std::ceil((hi.y - lo.y) / cube - 1e-9)));
  const int nz = std::max(1, static_cast<int>(std::ceil((hi.z - lo.z) / cube - 1e-9)));
  // Centred on the part's box, so a part a whole number of cubes wide keeps its size.
  const vec3 origin = (lo + hi) * 0.5 - vec3(nx, ny, nz) * (cube * 0.5);
  const sculpt::SurfaceField field(part, cube);
  std::vector<char> filled(static_cast<size_t>(nx) * ny * nz, 0);
  auto at = [&](int x, int y, int z) { return (static_cast<size_t>(z) * ny + y) * nx + x; };
  size_t count = 0;
  for (int z = 0; z < nz; ++z) {
    if (progress) {
      if (progress->cancel) return fail("Stopped.");
      progress->stage = 1;
      progress->fraction = 0.6 * z / nz;
    }
    for (int y = 0; y < ny; ++y)
      for (int x = 0; x < nx; ++x)
        if (field.inside(origin + vec3(x + 0.5, y + 0.5, z + 0.5) * cube)) {
          filled[at(x, y, z)] = 1;
          ++count;
        }
  }
  if (count == 0) return fail("The cubes are bigger than the part: make them smaller.");
  // Cubes merged into boxes: runs along x, runs of equal runs along y, then equal rectangles along
  // z. Far fewer pieces to join.
  std::vector<std::array<int, 6>> boxes;  // x0 x1 y0 y1 z0 z1, ends excluded
  std::map<std::array<int, 4>, int> open;   // rectangle -> index in boxes, if it reached the last layer
  for (int z = 0; z < nz; ++z) {
    std::vector<std::array<int, 4>> rects;  // x0 x1 y0 y1
    std::map<std::pair<int, int>, size_t> growing;  // run -> rect, if it reached the last row
    for (int y = 0; y < ny; ++y) {
      std::map<std::pair<int, int>, size_t> next;
      for (int x = 0; x < nx;) {
        if (!filled[at(x, y, z)]) {
          ++x;
          continue;
        }
        int end = x;
        while (end < nx && filled[at(end, y, z)]) ++end;
        const auto key = std::make_pair(x, end);
        if (auto it = growing.find(key); it != growing.end()) {
          rects[it->second][3] = y + 1;
          next[key] = it->second;
        } else {
          rects.push_back({x, end, y, y + 1});
          next[key] = rects.size() - 1;
        }
        x = end;
      }
      growing = std::move(next);
    }
    std::map<std::array<int, 4>, int> stillOpen;
    for (const auto& r : rects) {
      if (auto it = open.find(r); it != open.end()) {
        boxes[static_cast<size_t>(it->second)][5] = z + 1;
        stillOpen[r] = it->second;
      } else {
        boxes.push_back({r[0], r[1], r[2], r[3], z, z + 1});
        stillOpen[r] = static_cast<int>(boxes.size()) - 1;
      }
    }
    open = std::move(stillOpen);
  }
  if (progress) progress->fraction = 0.65;
  std::vector<manifold::Manifold> pieces;
  pieces.reserve(boxes.size());
  for (const auto& b : boxes) {
    const vec3 a = origin + vec3(b[0], b[2], b[4]) * cube;
    const vec3 size = vec3(b[1] - b[0], b[3] - b[2], b[5] - b[4]) * cube;
    pieces.push_back(manifold::Manifold::Cube(size).Translate(a));
  }
  const manifold::Manifold solid = pieces.size() == 1 ? pieces.front() : manifold::Manifold::BatchBoolean(pieces, manifold::OpType::Add);
  if (solid.Status() != manifold::Manifold::Error::NoError || solid.IsEmpty()) return fail("The cubes did not join into one solid.");
  *out = sculpt::fromManifold(solid);
  if (progress) progress->fraction = 1;
  return true;
}

// ---- Wrap ---------------------------------------------------------------------------------------

namespace {

// Rays against a mesh, through a box tree.
class RayMesh {
 public:
  explicit RayMesh(const Mesh& mesh) : mesh_(mesh) {
    order_.resize(mesh.tri.size());
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = static_cast<uint32_t>(i);
    if (!order_.empty()) build(0, order_.size());
  }
  // The nearest hit along origin + t * dir (t > 0); t and the triangle's normal.
  bool cast(const vec3& origin, const vec3& dir, double* tOut) const {
    double best = 1e300;
    bool hit = false;
    if (nodes_.empty()) return false;
    std::vector<uint32_t> stack{0};
    const vec3 inv(1 / (std::abs(dir.x) > 1e-12 ? dir.x : 1e-12), 1 / (std::abs(dir.y) > 1e-12 ? dir.y : 1e-12),
                   1 / (std::abs(dir.z) > 1e-12 ? dir.z : 1e-12));
    while (!stack.empty()) {
      const Node& n = nodes_[stack.back()];
      stack.pop_back();
      const vec3 t0 = (n.lo - origin) * inv, t1 = (n.hi - origin) * inv;
      const vec3 tn = la::min(t0, t1), tf = la::max(t0, t1);
      const double enter = std::max({tn.x, tn.y, tn.z, 0.0}), leave = std::min({tf.x, tf.y, tf.z, best});
      if (enter > leave) continue;
      if (n.count) {
        for (uint32_t k = n.first; k < n.first + n.count; ++k) {
          const sculpt::Tri& tr = mesh_.tri[order_[k]];
          const vec3 a = mesh_.pos[tr[0]], e1 = mesh_.pos[tr[1]] - a, e2 = mesh_.pos[tr[2]] - a;
          const vec3 p = la::cross(dir, e2);
          const double det = la::dot(e1, p);
          if (std::abs(det) < 1e-14) continue;
          const vec3 s = origin - a;
          const double u = la::dot(s, p) / det;
          if (u < 0 || u > 1) continue;
          const vec3 q = la::cross(s, e1);
          const double v = la::dot(dir, q) / det;
          if (v < 0 || u + v > 1) continue;
          const double t = la::dot(e2, q) / det;
          if (t > 1e-9 && t < best) best = t, hit = true;
        }
      } else {
        stack.push_back(n.left);
        stack.push_back(n.left + 1);
      }
    }
    if (hit) *tOut = best;
    return hit;
  }

 private:
  struct Node {
    vec3 lo, hi;
    uint32_t left = 0, first = 0, count = 0;
  };
  uint32_t build(size_t first, size_t last) {
    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back({});
    vec3 lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300), clo = lo, chi = hi;
    for (size_t k = first; k < last; ++k)
      for (uint32_t v : mesh_.tri[order_[k]]) lo = la::min(lo, mesh_.pos[v]), hi = la::max(hi, mesh_.pos[v]);
    nodes_[index].lo = lo;
    nodes_[index].hi = hi;
    if (last - first <= 6) {
      nodes_[index].first = static_cast<uint32_t>(first);
      nodes_[index].count = static_cast<uint32_t>(last - first);
      return index;
    }
    auto centre = [&](uint32_t t) {
      const sculpt::Tri& tri = mesh_.tri[t];
      return (mesh_.pos[tri[0]] + mesh_.pos[tri[1]] + mesh_.pos[tri[2]]) / 3.0;
    };
    for (size_t k = first; k < last; ++k) {
      const vec3 c = centre(order_[k]);
      clo = la::min(clo, c), chi = la::max(chi, c);
    }
    const vec3 span = chi - clo;
    const int axis = span.x >= span.y && span.x >= span.z ? 0 : span.y >= span.z ? 1 : 2;
    const size_t mid = (first + last) / 2;
    std::nth_element(order_.begin() + first, order_.begin() + mid, order_.begin() + last,
                     [&](uint32_t a, uint32_t b) { return centre(a)[axis] < centre(b)[axis]; });
    const uint32_t left = static_cast<uint32_t>(nodes_.size());
    nodes_[index].left = left;
    // Children are made side by side: reserve both slots first.
    nodes_.push_back({});
    nodes_.push_back({});
    buildInto(left, first, mid);
    buildInto(left + 1, mid, last);
    return index;
  }
  void buildInto(uint32_t slot, size_t first, size_t last) {
    const uint32_t made = build(first, last);
    nodes_[slot] = nodes_[made];
    // The copy at `made` stays unused (a few spare nodes cost nothing).
  }
  const Mesh& mesh_;
  std::vector<uint32_t> order_;
  std::vector<Node> nodes_;
};

}  // namespace

int wrapFacing(const Mesh& decal) {
  vec3 lo, hi;
  sculpt::bounds(decal, &lo, &hi);
  const vec3 size = hi - lo;
  if (size.z <= size.x && size.z <= size.y) return 0;
  return size.y <= size.x ? 1 : 2;
}

bool wrap(const Mesh& decal, const Mesh& target, const WrapSettings& s, Mesh* out, std::string* error, lattice::Progress* progress) {
  auto fail = [&](const char* text) {
    if (error) *error = text;
    return false;
  };
  if (decal.tri.empty() || target.tri.empty()) return fail("Select the text (or logo) and the part to wrap it onto.");
  if (progress) {
    progress->stage = 1;
    progress->fraction = 0.05;
  }
  vec3 dlo, dhi, tlo, thi;
  sculpt::bounds(decal, &dlo, &dhi);
  sculpt::bounds(target, &tlo, &thi);
  const int facing = wrapFacing(decal);
  // The decal in its own terms: u to the right as you read it, v up, w from its back (0) to its face (1).
  auto uvw = [&](const vec3& p) {
    switch (facing) {
      case 0: return vec3(p.x, p.y, (p.z - dlo.z) / std::max(1e-6, dhi.z - dlo.z));
      case 1: return vec3(p.x, p.z, (dhi.y - p.y) / std::max(1e-6, dhi.y - dlo.y));
      default: return vec3(p.y, p.z, (p.x - dlo.x) / std::max(1e-6, dhi.x - dlo.x));
    }
  };
  const vec3 c0 = uvw(dlo), c1 = uvw(dhi);
  const double uc = (c0.x + c1.x) * 0.5, vc = (c0.y + c1.y) * 0.5;
  // Finely divided first, so it can bend.
  manifold::Manifold solid = toManifold(decal);
  if (solid.IsEmpty()) return fail("The text or logo is not one closed solid.");
  const vec3 axisPoint((tlo.x + thi.x) * 0.5, (tlo.y + thi.y) * 0.5, 0);
  const double reach = la::length(thi - tlo) + la::length(dhi - dlo) + 10;
  RayMesh rays(target);
  const double lift = s.cut ? -s.depth : -0.3;  // where its back goes: a little in, or cut-in depth
  const double face = s.cut ? 0.8 : s.depth;     // and its face
  // Round: the decal's middle at angle and height; its width runs round at the radius there.
  const double theta0 = (s.angle - 90) * kPi / 180;
  const double zMid = tlo.z + s.height;
  double radius0 = 0;
  if (s.mode == 0) {
    const vec3 d(std::cos(theta0), std::sin(theta0), 0);
    double t = 0;
    const vec3 from = vec3(axisPoint.x, axisPoint.y, zMid) + d * reach;
    if (!rays.cast(from, -d, &t)) return fail("The middle of the text is off the part: change the height.");
    radius0 = reach - t;
    if (radius0 < 0.5) return fail("The part is too thin there to wrap round.");
  }
  const double span = std::max(c1.x - c0.x, c1.y - c0.y);
  const double edge = std::clamp(s.mode == 0 ? radius0 / 25 : span / 120, 0.15, 0.8);
  solid = solid.RefineToLength(edge);
  Mesh fine = sculpt::fromManifold(solid);
  if (progress) progress->fraction = 0.3;
  // Facing straight on: rays along the decal's thin direction, into the target.
  const vec3 outward = facing == 0 ? vec3(0, 0, 1) : facing == 1 ? vec3(0, -1, 0) : vec3(1, 0, 0);
  size_t missed = 0;
  for (size_t i = 0; i < fine.pos.size(); ++i) {
    const vec3 q = uvw(fine.pos[i]);
    const double r = lift + q.z * (face - lift);
    if (s.mode == 0) {
      const double theta = theta0 + (q.x - uc) / radius0;
      const vec3 d(std::cos(theta), std::sin(theta), 0);
      const vec3 from = vec3(axisPoint.x, axisPoint.y, zMid + (q.y - vc)) + d * reach;
      double t = 0;
      if (!rays.cast(from, -d, &t)) {
        ++missed;
        t = reach - radius0;
      }
      fine.pos[i] = from - d * t + d * r;
    } else {
      // Where it is now, pushed along -outward onto the surface.
      vec3 p = fine.pos[i];
      p = p - outward * la::dot(p - dlo, outward);  // onto its back plane
      const vec3 from = p + outward * reach;
      double t = 0;
      if (!rays.cast(from, -outward, &t)) {
        ++missed;
        continue;
      }
      fine.pos[i] = from - outward * t + outward * r;
    }
    if (progress && (i & 4095) == 0) progress->fraction = 0.3 + 0.6 * double(i) / fine.pos.size();
  }
  if (missed * 50 > fine.pos.size())
    return fail(s.mode == 0 ? "Part of the text is off the part: make it smaller or move it."
                            : "Part of the text is not in front of the part: move it over the part first.");
  const manifold::Manifold bent = toManifold(fine);
  if (bent.IsEmpty()) return fail("The wrapped text did not come out as a clean solid; try a smaller depth.");
  *out = sculpt::fromManifold(bent);
  if (progress) progress->fraction = 1;
  return true;
}

// ---- Articulate ---------------------------------------------------------------------------------

namespace {

manifold::Polygons ccw(manifold::SimplePolygon poly) {
  double area = 0;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) area += poly[j].x * poly[i].y - poly[i].x * poly[j].y;
  if (area < 0) std::reverse(poly.begin(), poly.end());
  return {poly};
}

}  // namespace

bool articulate(const Mesh& part, const ArticulateSettings& s, Mesh* out, std::string* error, lattice::Progress* progress) {
  using manifold::Manifold;
  std::string why;
  auto fail = [&](const std::string& text) {
    if (error) *error = text;
    return false;
  };
  Manifold solid = toManifold(part);
  if (solid.IsEmpty()) return fail("The part is not one closed solid.");
  // Work along X: a part to be jointed along Y is turned a quarter first (and back at the end).
  const manifold::mat3x4 turn(vec3(0, -1, 0), vec3(1, 0, 0), vec3(0, 0, 1), vec3(0, 0, 0));      // (x, y) -> (y, -x)
  const manifold::mat3x4 unturn(vec3(0, 1, 0), vec3(-1, 0, 0), vec3(0, 0, 1), vec3(0, 0, 0));
  if (s.axis == 1) solid = solid.Transform(turn);
  const auto box = solid.BoundingBox();
  const double length = box.max.x - box.min.x;
  const int count = std::clamp(s.joints, 1, 40);
  const double gap = std::clamp(s.gap, 0.1, 1.5);
  const double gz = gap * std::sqrt(2.0);
  const double slope = std::tan(std::clamp(s.swing, 5.0, 60.0) * kPi / 180);
  const double big = la::length(box.max - box.min) + 50;
  const int segs = 48;
  std::vector<Manifold> left, right;  // each joint's two sides, in the part's frame
  for (int j = 0; j < count; ++j) {
    if (progress) {
      progress->stage = 1;
      progress->fraction = 0.8 * j / count;
      if (progress->cancel) return fail("Stopped.");
    }
    const double f = count == 1 ? (s.from + s.to) * 0.5 : s.from + (s.to - s.from) * j / (count - 1);
    const double c = box.min.x + length * f;
    // The part across the cut there: how wide, and how thick on the joint's axis.
    const Manifold slab = solid ^ Manifold::Cube(vec3(0.5, big * 2, big * 2)).Translate(vec3(c - 0.25, -big, -big));
    if (slab.IsEmpty()) return fail("Joint " + std::to_string(j + 1) + " falls in a gap of the part.");
    const auto sb = slab.BoundingBox();
    const double yc = (sb.min.y + sb.max.y) * 0.5, width = sb.max.y - sb.min.y;
    const Manifold column = solid ^ Manifold::Cylinder(big * 2, 0.6, 0.6, 12, true).Translate(vec3(c, yc, 0));
    const auto cb = column.IsEmpty() ? sb : column.BoundingBox();
    const double zm = (cb.min.z + cb.max.z) * 0.5, height = cb.max.z - cb.min.z;
    const double a = std::clamp(0.15 * height, 0.8, 2.0);
    const double wall = std::max(1.0, 0.12 * height);
    const double reach = std::min(height * 0.5 - wall - gz - a, width * 0.42);  // the cone's slope part
    if (reach < 1.5)
      return fail("The part is too thin at joint " + std::to_string(j + 1) + " (" + std::to_string(int(std::round(height))) +
                  " mm): joints need about 9 mm of thickness. Move the joints or use fewer.");
    const double rf = reach;  // the fork's radius: the cone is 2a thick there
    auto cone = [&](double grow, double radius) {
      const double peak = a + grow + reach;
      const double rim = std::max(0.0, peak - radius);
      manifold::SimplePolygon profile{{0, -peak}, {radius, -rim}, {radius, rim}, {0, peak}};
      return Manifold::Revolve(ccw(profile), segs);
    };
    auto cylinder = [&](double radius) { return Manifold::Cylinder(big * 2, radius, radius, segs, true); };
    // The wedges: left of x = -|y| slope - gap/2, right of x = |y| slope + gap/2.
    const double h = gap * 0.5, far = big * 3;
    const Manifold wedgeL = Manifold::Extrude(ccw({{-h, 0}, {-h - big * slope, big}, {-far, big}, {-far, -big}, {-h - big * slope, -big}}), big * 2)
                                .Translate(vec3(0, 0, -big));
    const Manifold wedgeR = Manifold::Extrude(ccw({{h, 0}, {h + big * slope, -big}, {far, -big}, {far, big}, {h + big * slope, big}}), big * 2)
                                .Translate(vec3(0, 0, -big));
    // Left keeps the cone (and the band that joins it to the rest across the ring); right keeps the
    // fork round the cone, all the way round inside the ring, and joined to the rest on its side.
    const Manifold sideL = (wedgeL - cylinder(rf + gap)) + cone(0, rf) + (wedgeL ^ cone(0, rf + gap + 0.05));
    const Manifold sideR = (wedgeR - cylinder(rf + gap)) + (cylinder(rf) - cone(gz, rf + 1)) +
                           (wedgeR ^ (cylinder(rf + gap + 0.05) - cone(gz, rf + gap + 1)));
    const vec3 at(c, yc, zm);
    left.push_back(sideL.Translate(at));
    right.push_back(sideR.Translate(at));
    if (j > 0 && c - (box.min.x + length * (count == 1 ? 0 : s.from + (s.to - s.from) * (j - 1) / (count - 1))) < 2 * (rf + gap) + 2)
      return fail("The joints are too close together for their size: use fewer, or spread them further.");
  }
  std::vector<Manifold> pieces;
  for (int k = 0; k <= count; ++k) {
    Manifold piece = solid;
    if (k > 0) piece = piece ^ right[k - 1];
    if (k < count) piece = piece ^ left[k];
    if (piece.IsEmpty()) return fail("Segment " + std::to_string(k + 1) + " came out empty: the joints are too close.");
    if (piece.Decompose().size() > 1 && k > 0 && k < count)
      ;  // a segment of a branching shape can be in several pieces; they still hang on their joints
    pieces.push_back(piece);
    if (progress) progress->fraction = 0.8 + 0.2 * k / (count + 1);
  }
  Manifold all = Manifold::Compose(pieces);
  if (s.axis == 1) all = all.Transform(unturn);
  if (all.Status() != Manifold::Error::NoError || all.IsEmpty()) return fail("The segments did not come out as solids.");
  *out = sculpt::fromManifold(all);
  return true;
}

bool simplify(const Mesh& part, double tolerance, Mesh* out, std::string* error, lattice::Progress* progress) {
  if (progress) {
    progress->stage = 1;
    progress->fraction = 0.1;
  }
  const manifold::Manifold solid = toManifold(part);
  if (solid.IsEmpty()) {
    if (error) *error = "The part is not one closed solid, so it cannot be simplified safely.";
    return false;
  }
  if (progress && progress->cancel) {
    if (error) *error = "Stopped.";
    return false;
  }
  const manifold::Manifold lighter = solid.Simplify(std::max(tolerance, 1e-4));
  if (lighter.Status() != manifold::Manifold::Error::NoError || lighter.IsEmpty()) {
    if (error) *error = "Simplifying did not come out as a solid; try a smaller tolerance.";
    return false;
  }
  *out = sculpt::fromManifold(lighter);
  if (progress) progress->fraction = 1;
  return true;
}

namespace {

// The solid turned so direction `from` (a unit axis, either sign) points up (+Z). Found by trying
// the few quarter and half turns on a probe, so it does not hang on which way Rotate turns.
manifold::Manifold turnUp(const manifold::Manifold& m, const vec3& from) {
  static const double turns[][3] = {{0, 0, 0}, {90, 0, 0}, {-90, 0, 0}, {0, 90, 0}, {0, -90, 0}, {180, 0, 0}};
  const manifold::Manifold probe = manifold::Manifold::Cube(vec3(1, 1, 1), true).Translate(from * 10.0);
  for (const auto& t : turns) {
    const manifold::Box b = probe.Rotate(t[0], t[1], t[2]).BoundingBox();
    const vec3 c = (b.min + b.max) * 0.5;
    if (la::length(c - vec3(0, 0, 10)) < 1e-6) return m.Rotate(t[0], t[1], t[2]);
  }
  return m;
}

double comp(const vec3& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }

}  // namespace

double moldParting(const Mesh& part, int axis) {
  const manifold::Manifold solid = toManifold(part);
  if (solid.IsEmpty()) return 0.5;
  axis = std::clamp(axis, 0, 2);
  vec3 dir(0, 0, 0);
  dir[axis] = 1;
  const manifold::Manifold up = turnUp(solid, dir);
  const manifold::Box box = up.BoundingBox();
  const double lo = box.min.z, hi = box.max.z;
  if (hi - lo < 1e-6) return 0.5;
  double best = 0.5, bestArea = -1;
  for (int i = 0; i <= 60; ++i) {
    const double t = 0.2 + 0.6 * i / 60.0;
    const double area = manifold::CrossSection(up.Slice(lo + t * (hi - lo))).Area();
    // A slice only wins if it really is wider: on a straight-sided part the middle stays.
    if (area > bestArea * 1.002 + 1e-9) {
      bestArea = area;
      best = t;
    } else if (std::abs(area - bestArea) <= bestArea * 0.002 && std::abs(t - 0.5) < std::abs(best - 0.5)) {
      best = t;
    }
  }
  return best;
}

bool mold(const Mesh& part, const MoldSettings& s, Mesh* out, std::string* error, lattice::Progress* progress) {
  auto fail = [&](const char* text) {
    if (error) *error = text;
    return false;
  };
  auto step = [&](double f) {
    if (progress) {
      progress->stage = 1;
      progress->fraction = f;
    }
    return !(progress && progress->cancel);
  };
  const manifold::Manifold solid = toManifold(part);
  if (solid.IsEmpty()) return fail("The part is not one closed solid, so no mould can be made of it.");
  const int axis = std::clamp(s.axis, 0, 2);
  const double wall = std::max(s.wall, 1.0);
  const manifold::Box box = solid.BoundingBox();
  const double at = s.at >= 0 ? std::clamp(s.at, 0.02, 0.98) : moldParting(part, axis);
  const double cut = comp(box.min, axis) + at * (comp(box.max, axis) - comp(box.min, axis));
  if (!step(0.1)) return fail("Stopped.");
  const vec3 lo = box.min - vec3(wall, wall, wall), hi = box.max + vec3(wall, wall, wall);
  manifold::Manifold cavity = manifold::Manifold::Cube(hi - lo).Translate(lo) - solid;
  // Pour hole and vents run straight up (+Z) from the part's high points.
  const manifold::MeshGL64 gl = solid.GetMeshGL64();
  std::vector<vec3> points;
  for (size_t i = 0; i + 2 < gl.vertProperties.size(); i += gl.numProp)
    points.emplace_back(gl.vertProperties[i], gl.vertProperties[i + 1], gl.vertProperties[i + 2]);
  std::sort(points.begin(), points.end(), [](const vec3& a, const vec3& b) { return a.z > b.z; });
  // A channel from p (starting a little inside the part) up and out of the block; the pour hole
  // gets a funnel twice as wide at the top, so pouring is easy.
  auto channel = [&](const vec3& p, double diameter, bool funnel) {
    const double r = diameter * 0.5;
    const double length = hi.z - p.z + r + 1;
    manifold::Manifold c = manifold::Manifold::Cylinder(length, r, r, 32).Translate(vec3(p.x, p.y, p.z - r));
    if (funnel) {
      const double depth = std::min({wall * 0.6, 3 * diameter, hi.z - p.z});
      // From r at `depth` below the top to 2r at the top (and on 1 mm past it, so it opens cleanly).
      if (depth > 0.5)
        c = c + manifold::Manifold::Cylinder(depth + 1, r, r + r * (depth + 1) / depth, 48).Translate(vec3(p.x, p.y, hi.z - depth));
    }
    return c;
  };
  std::vector<manifold::Manifold> holes;
  std::vector<vec3> used;
  if (!points.empty() && s.pour > 0.05) {
    // The pour hole: at the highest point; with a split on the side (X or Y), the highest point
    // near the parting plane, moved onto it, so the channel runs down the seam through both halves.
    vec3 where = points.front();
    if (axis != 2) {
      const double near = std::max(s.pour, 0.15 * (comp(box.max, axis) - comp(box.min, axis)));
      for (const vec3& p : points)
        if (std::abs(comp(p, axis) - cut) < near) {
          where = p;
          break;
        }
      where[axis] = cut;
    }
    holes.push_back(channel(where, s.pour, true));
    used.push_back(where);
  }
  // Vents at other high points, well apart, in the upper half of the part's height (where air is
  // trapped).
  const double apart = std::max({8.0, 2 * s.pour, 0.2 * la::length(box.max - box.min)});
  const double top = points.empty() ? 0 : points.front().z;
  int vents = 0;
  for (const vec3& p : points) {
    if (vents >= s.vents || top - p.z > 0.5 * (box.max.z - box.min.z)) break;
    bool far = true;
    for (const vec3& q : used) far = far && la::length(p - q) > apart;
    if (!far) continue;
    holes.push_back(channel(p, std::max(s.vent, 0.5), false));
    used.push_back(p);
    ++vents;
  }
  if (!step(0.3)) return fail("Stopped.");
  if (!holes.empty()) cavity = cavity - manifold::Manifold::BatchBoolean(holes, manifold::OpType::Add);
  // The two halves: below and above the parting plane.
  vec3 n(0, 0, 0);
  n[axis] = 1;
  manifold::Manifold lower = cavity.TrimByPlane(-n, -cut), upper = cavity.TrimByPlane(n, cut);
  if (!lower.IsEmpty() && !upper.IsEmpty() && comp(lower.BoundingBox().max, axis) > comp(upper.BoundingBox().max, axis))
    std::swap(lower, upper);
  if (lower.IsEmpty() || upper.IsEmpty()) return fail("The parting plane misses the part: pick another place for it.");
  if (!step(0.6)) return fail("Stopped.");
  // Keys: round bumps on the lower half's face, sockets in the upper one, in the corners of the
  // face (in the middle of the wall round the part, clear of it).
  if (s.keys) {
    const double r = std::clamp(wall * 0.3, 1.0, 5.0);
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    std::vector<manifold::Manifold> bumps, sockets;
    for (int k = 0; k < 4; ++k) {
      vec3 c(0, 0, 0);
      c[axis] = cut;
      c[u] = k & 1 ? comp(hi, u) - wall * 0.5 : comp(lo, u) + wall * 0.5;
      c[v] = k & 2 ? comp(hi, v) - wall * 0.5 : comp(lo, v) + wall * 0.5;
      bumps.push_back(manifold::Manifold::Sphere(r, 48).Translate(c));
      sockets.push_back(manifold::Manifold::Sphere(r + std::max(s.clearance, 0.0), 48).Translate(c));
    }
    lower = lower + manifold::Manifold::BatchBoolean(bumps, manifold::OpType::Add).TrimByPlane(n, cut - 0.01);
    upper = upper - manifold::Manifold::BatchBoolean(sockets, manifold::OpType::Add);
  }
  if (!step(0.8)) return fail("Stopped.");
  if (s.printLayout) {
    // Each half with its parting face up: the cavity opens upward like a bowl, so neither needs
    // supports. Side by side on the plate where the part stood.
    manifold::Manifold a = turnUp(lower, n), b = turnUp(upper, -n);
    const vec3 centre = (box.min + box.max) * 0.5;
    const manifold::Box ab = a.BoundingBox(), bb = b.BoundingBox();
    const double gap = 10;
    const double total = (ab.max.x - ab.min.x) + gap + (bb.max.x - bb.min.x);
    a = a.Translate(vec3(centre.x - total * 0.5 - ab.min.x, centre.y - (ab.min.y + ab.max.y) * 0.5, box.min.z - ab.min.z));
    b = b.Translate(vec3(centre.x - total * 0.5 + (ab.max.x - ab.min.x) + gap - bb.min.x, centre.y - (bb.min.y + bb.max.y) * 0.5,
                         box.min.z - bb.min.z));
    lower = std::move(a);
    upper = std::move(b);
  }
  if (lower.Status() != manifold::Manifold::Error::NoError || upper.Status() != manifold::Manifold::Error::NoError)
    return fail("The mould did not come out as a solid.");
  // The halves side by side in one mesh, as they are: a union would weld closed halves together
  // along their faces.
  *out = sculpt::fromManifold(lower);
  const Mesh second = sculpt::fromManifold(upper);
  const uint32_t base = static_cast<uint32_t>(out->pos.size());
  out->pos.insert(out->pos.end(), second.pos.begin(), second.pos.end());
  for (sculpt::Tri t : second.tri) {
    for (uint32_t& v : t) v += base;
    out->tri.push_back(t);
  }
  if (progress) progress->fraction = 1;
  return true;
}

manifold::Manifold toManifold(const Mesh& mesh) {
  manifold::MeshGL64 gl;
  gl.numProp = 3;
  gl.vertProperties.reserve(mesh.pos.size() * 3);
  for (const vec3& p : mesh.pos) {
    gl.vertProperties.push_back(p.x);
    gl.vertProperties.push_back(p.y);
    gl.vertProperties.push_back(p.z);
  }
  gl.triVerts.reserve(mesh.tri.size() * 3);
  for (const sculpt::Tri& t : mesh.tri)
    for (uint32_t v : t) gl.triVerts.push_back(v);
  manifold::Manifold solid(gl);
  if (solid.Status() != manifold::Manifold::Error::NoError) return {};
  return solid;
}

}  // namespace fx
