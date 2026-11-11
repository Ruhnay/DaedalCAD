// Shape effects: textures, smooth merge, low-poly and voxel come out closed, about the right size,
// and do what they say (raised adds, engraved takes away, the bottom stays flat).
#include "effects.h"

#include <manifold/cross_section.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using sculpt::vec3;
using manifold::Manifold;

static int fails = 0;
static const double kHalfPi = 1.5707963267948966;
#define CHECK(c, ...)                   \
  do {                                  \
    if (!(c)) {                         \
      ++fails;                          \
      std::printf("FAIL: ");            \
      std::printf(__VA_ARGS__);         \
      std::printf("\n");                \
    }                                   \
  } while (0)

static double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int main() {
  const double started = now();
  const sculpt::Mesh cylinder = sculpt::fromManifold(Manifold::Cylinder(30, 15, 15, 96));
  const double cylVolume = sculpt::volume(cylinder);
  const sculpt::Mesh cube = sculpt::fromManifold(Manifold::Cube({20, 20, 20}, true).Translate({0, 0, 10}));

  // Knurling round a cylinder, raised and engraved.
  {
    fx::TextureSettings s;
    s.kind = fx::Texture::Knurl;
    s.size = 3;
    s.depth = 0.6;
    s.wrap = 1;
    sculpt::Mesh up, down;
    std::string error;
    lattice::Report report;
    const double t0 = now();
    const bool ok = fx::texture(cylinder, s, &up, &error, &report);
    std::printf("knurl, raised: %zu triangles in %.1f s at %.2f mm, clean %d\n", up.tri.size(), now() - t0, report.detail, int(report.clean));
    CHECK(ok, "knurl raised: %s", error.c_str());
    if (ok) {
      CHECK(sculpt::closedManifold(up), "knurl raised: closed");
      const double grow = sculpt::volume(up) / cylVolume - 1;
      CHECK(grow > 0.01 && grow < 0.2, "knurl raised adds a little (%.3f)", grow);
      vec3 lo, hi;
      sculpt::bounds(up, &lo, &hi);
      CHECK(std::abs(lo.z) < 0.05, "knurl raised: the bottom stays on the plate (%.3f)", lo.z);
      CHECK(hi.x > 15.3 && hi.x < 15.75, "knurl raised: stands out about 0.6 mm (%.3f)", hi.x);
    }
    s.raised = false;
    CHECK(fx::texture(cylinder, s, &down, &error), "knurl engraved: %s", error.c_str());
    if (!down.tri.empty()) {
      const double shrink = 1 - sculpt::volume(down) / cylVolume;
      CHECK(shrink > 0.01 && shrink < 0.2, "knurl engraved takes a little away (%.3f)", shrink);
    }
  }
  // Every texture on a cube, from every side.
  for (int kind = 0; kind <= static_cast<int>(fx::Texture::Weave); ++kind) {
    fx::TextureSettings s;
    s.kind = static_cast<fx::Texture>(kind);
    s.size = 4;
    s.depth = 0.5;
    sculpt::Mesh out;
    std::string error;
    lattice::Report report;
    const double t0 = now();
    const bool ok = fx::texture(cube, s, &out, &error, &report);
    std::printf("texture %d on a cube: %zu triangles in %.1f s, clean %d\n", kind, out.tri.size(), now() - t0, int(report.clean));
    CHECK(ok && sculpt::closedManifold(out), "texture %d: %s", kind, error.c_str());
    CHECK(ok && sculpt::volume(out) > 8000, "texture %d raised adds", kind);
  }
  // A picture.
  {
    fx::TextureSettings s;
    s.kind = fx::Texture::Picture;
    s.picture.width = s.picture.height = 8;
    for (int i = 0; i < 64; ++i) s.picture.value.push_back(((i / 8 + i % 8) & 1) ? 1.f : 0.f);
    s.size = 8;
    s.depth = 0.5;
    sculpt::Mesh out;
    std::string error;
    CHECK(fx::texture(cube, s, &out, &error) && sculpt::closedManifold(out), "picture: %s", error.c_str());
  }
  // Smooth merge: two boxes across each other get a soft fillet in their inside corners (more
  // than the plain union), and a hole cuts with a soft edge.
  {
    const Manifold a = Manifold::Cube({30, 10, 10}, true), b = Manifold::Cube({10, 10, 30}, true).Translate({0, 0, 10});
    const double plain = (a + b).Volume();
    sculpt::Mesh out;
    std::string error;
    const double t0 = now();
    const bool ok = fx::blend({sculpt::fromManifold(a), sculpt::fromManifold(b)}, {}, 3, &out, &error);
    std::printf("smooth merge: %zu triangles in %.1f s, %.0f vs %.0f mm3\n", out.tri.size(), now() - t0, sculpt::volume(out), plain);
    CHECK(ok && sculpt::closedManifold(out), "smooth merge: %s", error.c_str());
    CHECK(ok && sculpt::volume(out) > plain + 5 && sculpt::volume(out) < plain * 1.15, "smooth merge fills the inside corners");
    sculpt::Mesh cut;
    const bool cutOk = fx::blend({sculpt::fromManifold(a)}, {sculpt::fromManifold(Manifold::Cylinder(20, 3, 3, 48).Translate({8, 0, -10}))}, 1.5,
                                 &cut, &error);
    CHECK(cutOk && sculpt::closedManifold(cut) && sculpt::volume(cut) < a.Volume() - 200, "smooth merge with a hole cuts it: %s", error.c_str());
  }
  // Low-poly: few big facets, still about the same shape.
  {
    const sculpt::Mesh ball = sculpt::fromManifold(Manifold::Sphere(20, 128));
    sculpt::Mesh out;
    std::string error;
    CHECK(fx::lowPoly(ball, 7, 0.6, 3, &out, &error), "low-poly: %s", error.c_str());
    std::printf("low-poly ball: %zu triangles, %.0f vs %.0f mm3\n", out.tri.size(), sculpt::volume(out), sculpt::volume(ball));
    CHECK(out.tri.size() > 50 && out.tri.size() < 2000, "low-poly: big facets (%zu triangles)", out.tri.size());
    CHECK(std::abs(sculpt::volume(out) / sculpt::volume(ball) - 1) < 0.15, "low-poly: about the same size");
    CHECK(sculpt::closedManifold(out) && !sculpt::selfIntersects(out), "low-poly: closed, no folds");
  }
  // Voxel: a cube a whole number of voxels wide stays exactly itself; a ball comes out close.
  {
    sculpt::Mesh out;
    std::string error;
    CHECK(fx::voxel(cube, 2, &out, &error), "voxel cube: %s", error.c_str());
    CHECK(std::abs(sculpt::volume(out) - 8000) < 1e-3, "voxel cube: exactly the cube (%.3f)", sculpt::volume(out));
    const sculpt::Mesh ball = sculpt::fromManifold(Manifold::Sphere(20, 128));
    const double t0 = now();
    CHECK(fx::voxel(ball, 2, &out, &error), "voxel ball: %s", error.c_str());
    std::printf("voxel ball: %zu triangles in %.1f s, %.0f vs %.0f mm3\n", out.tri.size(), now() - t0, sculpt::volume(out), sculpt::volume(ball));
    CHECK(std::abs(sculpt::volume(out) / sculpt::volume(ball) - 1) < 0.1, "voxel ball: about the same size");
    CHECK(sculpt::closedManifold(out), "voxel ball: closed");
    CHECK(!fx::voxel(ball, 0.05, &out, &error) && !error.empty(), "too many voxels refused");
  }
  // Wrap: a thin strip standing in front of a cylinder, wrapped round it, hugs the surface: from a
  // hair inside it to `depth` outside, and round by its width over the radius.
  {
    const sculpt::Mesh can = sculpt::fromManifold(Manifold::Cylinder(40, 20, 20, 128));
    const sculpt::Mesh strip = sculpt::fromManifold(Manifold::Cube({30, 1.2, 8}).Translate({-15, -26, 16}));
    CHECK(fx::wrapFacing(strip) == 1, "wrap: a strip standing at the front faces the front");
    fx::WrapSettings ws;
    ws.mode = 0;
    ws.angle = 0;
    ws.height = 20;
    ws.depth = 1.5;
    sculpt::Mesh out;
    std::string error;
    const double t0 = now();
    const bool ok = fx::wrap(strip, can, ws, &out, &error);
    CHECK(ok, "wrap round: %s", error.c_str());
    if (ok) {
      double rmin = 1e9, rmax = 0, amin = 1e9, amax = -1e9, zmin = 1e9, zmax = -1e9;
      for (const auto& p : out.pos) {
        const double r = std::hypot(p.x, p.y), a = std::atan2(p.y, p.x);
        rmin = std::min(rmin, r), rmax = std::max(rmax, r);
        amin = std::min(amin, a), amax = std::max(amax, a);
        zmin = std::min(zmin, p.z), zmax = std::max(zmax, p.z);
      }
      std::printf("wrap round: %zu triangles in %.1f s, radius %.2f .. %.2f, %.2f rad round, z %.1f .. %.1f\n", out.tri.size(),
                  now() - t0, rmin, rmax, amax - amin, zmin, zmax);
      CHECK(rmin > 19.5 && rmin < 19.8 && std::abs(rmax - 21.5) < 0.05, "wrap round: hugs the surface");
      CHECK(std::abs((amax - amin) - 30.0 / 20) < 0.05, "wrap round: as long round as it was wide");
      CHECK(std::abs(zmin - 16) < 0.05 && std::abs(zmax - 24) < 0.05, "wrap round: at the height asked");
      CHECK(amin > -kHalfPi - 1 && amax < -kHalfPi + 1, "wrap round: at the front");
      CHECK(sculpt::closedManifold(out), "wrap round: closed");
    }
    ws.cut = true;
    CHECK(fx::wrap(strip, can, ws, &out, &error), "wrap cut in: %s", error.c_str());
    ws.cut = false;
    ws.mode = 1;  // straight on, along -Y from where it stands
    CHECK(fx::wrap(strip, can, ws, &out, &error), "wrap straight: %s", error.c_str());
    ws.mode = 0;
    ws.height = 80;
    CHECK(!fx::wrap(strip, can, ws, &out, &error) && !error.empty(), "wrap above the part refused");
  }

  // Articulate: a long bar cut into jointed segments. Each joint turns side to side by its swing
  // without the segments touching, and holds them together: pulled apart or lifted, they collide.
  {
    const Manifold bar = Manifold::Cube({120, 24, 12}, true).Translate({0, 0, 6});
    fx::ArticulateSettings as;
    as.joints = 4;
    as.from = 0.2;
    as.to = 0.8;
    as.gap = 0.35;
    as.swing = 25;
    sculpt::Mesh out;
    std::string error;
    const double t0 = now();
    const bool ok = fx::articulate(sculpt::fromManifold(bar), as, &out, &error);
    CHECK(ok, "articulate: %s", error.c_str());
    if (ok) {
      std::vector<Manifold> pieces = fx::toManifold(out).Decompose();
      std::sort(pieces.begin(), pieces.end(), [](const Manifold& a, const Manifold& b) {
        return a.BoundingBox().Center().x < b.BoundingBox().Center().x;
      });
      std::printf("articulate: %zu segments, %zu triangles in %.1f s\n", pieces.size(), out.tri.size(), now() - t0);
      CHECK(pieces.size() == 5, "articulate: 4 joints make 5 segments (got %zu)", pieces.size());
      for (size_t j = 0; j + 1 < pieces.size() && pieces.size() == 5; ++j) {
        const double c = -60 + 120 * (0.2 + 0.6 * j / 3.0);
        auto turned = [&](double degrees) {
          return pieces[j].Translate({-c, 0, 0}).Rotate(0, 0, degrees).Translate({c, 0, 0});
        };
        const double still = (pieces[j] ^ pieces[j + 1]).Volume();
        const double swingL = (turned(22) ^ pieces[j + 1]).Volume(), swingR = (turned(-22) ^ pieces[j + 1]).Volume();
        const double pulled = (pieces[j].Translate({-2, 0, 0}) ^ pieces[j + 1]).Volume();
        const double lifted = (pieces[j].Translate({0, 0, 2}) ^ pieces[j + 1]).Volume();
        std::printf("  joint %zu: touching %.3f, swung %.3f / %.3f mm3; pulled %.1f, lifted %.1f mm3\n", j + 1, still, swingL, swingR,
                    pulled, lifted);
        CHECK(still < 1e-6 && swingL < 1e-3 && swingR < 1e-3, "articulate joint %zu: swings freely", j + 1);
        CHECK(pulled > 1 && lifted > 1, "articulate joint %zu: holds together", j + 1);
      }
    }
    as.joints = 30;
    CHECK(!fx::articulate(sculpt::fromManifold(bar), as, &out, &error) && !error.empty(), "too many joints refused");
    const Manifold sheet = Manifold::Cube({120, 24, 4}, true);
    as.joints = 3;
    CHECK(!fx::articulate(sculpt::fromManifold(sheet), as, &out, &error) && !error.empty(), "too thin refused");
  }
  // Simplify: a dense sphere keeps its shape within the tolerance with a fraction of the triangles.
  {
    const Manifold ball = Manifold::Sphere(30, 1000);
    sculpt::Mesh out;
    std::string error;
    const double t0 = now();
    const bool ok = fx::simplify(sculpt::fromManifold(ball), 0.02, &out, &error);
    CHECK(ok, "simplify: %s", error.c_str());
    if (ok) {
      const Manifold lighter = fx::toManifold(out);
      double worst = 0;
      for (const auto& p : out.pos) worst = std::max(worst, std::abs(std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z) - 30));
      std::printf("simplify: %zu -> %zu triangles in %.1f s, volume %.0f -> %.0f, farthest vertex off by %.3f mm\n", ball.NumTri(),
                  out.tri.size(), now() - t0, ball.Volume(), lighter.Volume(), worst);
      CHECK(out.tri.size() * 5 < ball.NumTri(), "simplify: under a fifth of the triangles");
      CHECK(std::abs(lighter.Volume() - ball.Volume()) < 0.005 * ball.Volume(), "simplify: same volume");
      CHECK(sculpt::closedManifold(out), "simplify: closed");
    }
  }
  // Mold: a ball's mould is two halves that close round the ball exactly (with only the channels
  // and the key clearance left over), parted through its middle, with a pour hole and vents that
  // reach the cavity, and laid out for printing both faces up.
  {
    const Manifold ball = Manifold::Sphere(15, 96).Translate(vec3(0, 0, 15));
    const sculpt::Mesh part = sculpt::fromManifold(ball);
    CHECK(std::abs(fx::moldParting(part, 2) - 0.5) < 0.02, "mold: a ball parts through its middle (%.3f)", fx::moldParting(part, 2));
    fx::MoldSettings ms;
    ms.printLayout = false;
    sculpt::Mesh out;
    std::string error;
    const double t0 = now();
    const bool ok = fx::mold(part, ms, &out, &error);
    CHECK(ok, "mold: %s", error.c_str());
    if (ok) {
      const Manifold closed = fx::toManifold(out);
      const auto halves = closed.Decompose();
      CHECK(halves.size() == 2, "mold: two halves (%zu)", halves.size());

      if (halves.size() == 2) {
        CHECK((halves[0] ^ halves[1]).Volume() < 1e-3, "mold: the closed halves do not overlap (keys fit their sockets)");
        CHECK((halves[0] ^ ball).Volume() < 1e-3 && (halves[1] ^ ball).Volume() < 1e-3, "mold: the cavity is the ball");
      }
      const double block = std::pow(30 + 2 * ms.wall, 3);
      const double left = block - closed.Volume() - ball.Volume();  // channels and key clearance
      std::printf("mold: %.1f s, %zu triangles, %.0f mm3 of mould, %.0f mm3 in channels and clearances\n", now() - t0, out.tri.size(),
                  closed.Volume(), left);
      CHECK(left > 50 && left < 1500, "mold: only the channels and clearances are left open (%.0f)", left);
      // The pour hole and the vents reach the cavity: joined, the halves are one shell (a sealed
      // cavity would be a second one) with a handle for each channel after the first.
      if (halves.size() == 2) {
        // (The key clearances are small closed gaps of their own: four more shells, left out.)
        const auto shells = (halves[0] + halves[1]).Decompose();
        Manifold outer;
        for (const Manifold& m : shells)
          if (m.Volume() > outer.Volume()) outer = m;
        CHECK(shells.size() == 5 && outer.Genus() == ms.vents, "mold: pour hole and %d vents reach the cavity (%zu shells, genus %d)",
              ms.vents, shells.size(), outer.Genus());
      }
    }
    ms.printLayout = true;
    sculpt::Mesh laid;
    CHECK(fx::mold(part, ms, &laid, &error), "mold laid out: %s", error.c_str());
    const auto pieces = fx::toManifold(laid).Decompose();
    CHECK(pieces.size() == 2, "mold laid out: two pieces");
    if (pieces.size() == 2) {
      const auto a = pieces[0].BoundingBox(), b = pieces[1].BoundingBox();
      CHECK(std::abs(a.min.z - b.min.z) < 1e-6 && std::abs(a.min.z) < 1e-6, "mold laid out: both on the plate");
      CHECK(a.max.x < b.min.x || b.max.x < a.min.x, "mold laid out: side by side");
      // Faces up: the cavity is a bowl open at the top, so a slice just under the top cuts the
      // cavity (a hole in the slice) in both pieces.
      for (const Manifold& piece : pieces) {
        const auto box = piece.BoundingBox();
        double faceTop = box.max.z;
        // The keys stand above the face on one of them: slice below their height.
        const auto slice = manifold::CrossSection(piece.Slice(faceTop - std::clamp(ms.wall * 0.3, 1.0, 5.0) - 0.5));
        CHECK(slice.ToPolygons().size() >= 2, "mold laid out: the cavity opens upward");
      }
    }
    // Split on the side, through X.
    ms.axis = 0;
    ms.printLayout = false;
    sculpt::Mesh side;
    CHECK(fx::mold(part, ms, &side, &error) && fx::toManifold(side).Decompose().size() == 2, "mold split on X: two halves (%s)",
          error.c_str());
  }
  std::printf("effects checks took %.1f s\n", now() - started);
  std::printf(fails ? "%d failure(s)\n" : "all effects checks passed\n", fails);
  return fails ? 1 : 0;
}
