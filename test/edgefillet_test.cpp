// Edge fillet (beta) against the real Manifold kernel: finds the sharp edges of solids that are not
// primitives, rounds picked ones and checks the volume taken away or added against the exact
// rolling-ball value, plus points that must (or must not) be left. The arc is a polygon (12 sides per
// quarter turn), which cuts or fills about 1% more than a true circle; tolerances allow 2%.
//
// Build: cmake -DDAEDALCAD_FILLET_TESTS=ON ..  then run edgefillet_test.
#include "edgefillet.h"

#include <manifold/manifold.h>

#include <array>
#include <chrono>
#include <map>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace manifold;

static int fails = 0;
#define CHECK(c, ...)                   \
  do {                                  \
    if (!(c)) {                         \
      ++fails;                          \
      std::printf("FAIL: ");            \
      std::printf(__VA_ARGS__);         \
      std::printf("\n");                \
    }                                   \
  } while (0)

constexpr double kPiT = 3.14159265358979323846;
const double kNub = 1.0 - kPiT / 4.0;                      // area of the cut-off corner, per r^2
const double kNubCentroid = (5.0 / 6.0 - kPiT / 4.0) / kNub;  // its centroid from the edge, per r

static bool contains(const Manifold& solid, const vec3& p) {
  // Tiny cube at p: inside when intersecting keeps all of it.
  const Manifold probe = Manifold::Cube(vec3(0.02), true).Translate(p);
  return std::abs((solid ^ probe).Volume() - probe.Volume()) < 1e-9;
}

// The chain whose every point satisfies `pred`.
template <class F>
static int chainWhere(const std::vector<ef::Chain>& chains, F pred) {
  for (size_t i = 0; i < chains.size(); ++i) {
    bool all = true;
    for (size_t k = 0; k < chains[i].pts.size() / 3; ++k)
      all = all && pred(vec3(chains[i].pts[k * 3], chains[i].pts[k * 3 + 1], chains[i].pts[k * 3 + 2]));
    if (all) return static_cast<int>(i);
  }
  return -1;
}

static ef::Pick pickOf(const ef::Chain& chain, double r) {
  ef::Pick pick;
  pick.pts = chain.pts;
  if (chain.closed) pick.pts.insert(pick.pts.end(), chain.pts.begin(), chain.pts.begin() + 3);
  pick.radius = r;
  return pick;
}

static bool near(double a, double b, double rel) { return std::abs(a - b) <= rel * std::abs(b); }

int main() {
  const double r = 3.0;

  // 1. A plain box: twelve straight outside edges.
  const Manifold box = Manifold::Cube(vec3(30, 30, 30));
  const auto boxChains = ef::findChains(box);
  CHECK(boxChains.size() == 12, "box has %zu chains, expected 12", boxChains.size());
  int concave = 0;
  for (const auto& c : boxChains) concave += c.concave ? 1 : 0;
  CHECK(concave == 0, "box chains marked inside: %d", concave);
  const int topFront = chainWhere(boxChains, [](vec3 p) { return std::abs(p.y) < 1e-9 && std::abs(p.z - 30) < 1e-9; });
  const int topRight = chainWhere(boxChains, [](vec3 p) { return std::abs(p.x - 30) < 1e-9 && std::abs(p.z - 30) < 1e-9; });
  const int frontRight = chainWhere(boxChains, [](vec3 p) { return std::abs(p.x - 30) < 1e-9 && std::abs(p.y) < 1e-9; });
  CHECK(topFront >= 0 && topRight >= 0 && frontRight >= 0, "box edges not found");

  // 2. One edge.
  {
    std::string warning, log;
    const Manifold out = ef::applyEdgeFillets(box, {pickOf(boxChains[topFront], r)}, 12, &warning, &log);
    const double removed = box.Volume() - out.Volume();
    const double expected = 30 * r * r * kNub;
    std::printf("one edge: removed %.3f, exact %.3f\n", removed, expected);
    CHECK(near(removed, expected, 0.02), "one edge removed %.3f, expected %.3f", removed, expected);
    CHECK(!contains(out, vec3(15, 0.2, 29.8)), "the sharp edge is still there");
    CHECK(contains(out, vec3(15, r + 0.3, 29.9)), "the top face past the fillet was cut");
    CHECK(contains(out, vec3(15, 0.1, 30 - r - 0.3)), "the front face below the fillet was cut");
    CHECK(contains(out, vec3(29.9, 0.1, 29.9 - r)), "the right end was cut past the edge");
    CHECK(warning.empty(), "unexpected warning: %s", warning.c_str());
  }

  // 3. Two top edges meeting at a corner: mitred, nothing else touched.
  {
    std::string warning, log;
    const Manifold out =
        ef::applyEdgeFillets(box, {pickOf(boxChains[topFront], r), pickOf(boxChains[topRight], r)}, 12, &warning, &log);
    const double removed = box.Volume() - out.Volume();
    const double single = 30 * r * r * kNub;
    std::printf("two edges: removed %.3f (two singles %.3f)\n%s", removed, 2 * single, log.c_str());
    CHECK(removed < 2 * single && removed > 2 * single - r * r * r, "two edges removed %.3f", removed);
    CHECK(!contains(out, vec3(29.8, 0.2, 29.8)), "corner not rounded");
    CHECK(contains(out, vec3(29.9, 0.1, 30 - r - 0.3)), "vertical edge below the corner was cut");
    CHECK(warning.empty(), "unexpected warning: %s", warning.c_str());
  }

  // 4. Three edges at one corner: a ball corner.
  {
    std::string warning, log;
    const Manifold out = ef::applyEdgeFillets(
        box, {pickOf(boxChains[topFront], r), pickOf(boxChains[topRight], r), pickOf(boxChains[frontRight], r)}, 12,
        &warning, &log);
    const vec3 c(30 - r, r, 30 - r);
    const vec3 d = vec3(1, -1, 1) / std::sqrt(3.0);
    CHECK(contains(out, c + d * (r - 0.1)), "ball corner cut too deep");
    CHECK(!contains(out, c + d * (r + 0.1)), "ball corner left material outside the ball");
    std::printf("ball corner: %s", log.c_str());
  }

  // 5. A box standing on a plate: the four foot edges are inside edges; filling them must wrap
  //    the box's corners too (swept corners), with no notch.
  {
    const Manifold plate = Manifold::Cube(vec3(60, 60, 5));
    const Manifold post = Manifold::Cube(vec3(20, 20, 20)).Translate(vec3(20, 20, 5));
    const Manifold both = plate + post;
    const auto chains = ef::findChains(both);
    std::vector<ef::Pick> picks;
    for (const auto& c : chains) {
      bool foot = c.concave;
      for (size_t k = 0; k < c.pts.size() / 3; ++k) foot = foot && std::abs(c.pts[k * 3 + 2] - 5) < 1e-9;
      if (foot) picks.push_back(pickOf(c, 2.0));
    }
    CHECK(picks.size() == 4, "box on plate: %zu foot edges, expected 4", picks.size());
    std::string warning, log;
    const Manifold out = ef::applyEdgeFillets(both, picks, 12, &warning, &log);
    const double added = out.Volume() - both.Volume();
    const double rr = 2.0;
    const double expected = 4 * 20 * rr * rr * kNub + 4 * (kPiT / 2) * rr * rr * rr * (5.0 / 6.0 - kPiT / 4.0);
    std::printf("foot fillet: added %.3f, exact %.3f\n%s", added, expected, log.c_str());
    CHECK(near(added, expected, 0.02), "foot fillet added %.3f, expected %.3f", added, expected);
    // Right in the corner's swept part, low down: filled.
    const vec3 corner(40, 40, 5);
    CHECK(contains(out, corner + vec3(0.25, 0.25, 0.1)), "notch at the swept corner");
    CHECK(contains(out, vec3(30, 19.8, 5.1)), "foot fillet missing along an edge");
    CHECK(!contains(out, vec3(30, 19.2, 5 + rr - 0.1)), "foot fillet too big");
    CHECK(warning.empty(), "unexpected warning: %s", warning.c_str());
  }

  // 6. Cylinder rim: one closed outside loop.
  {
    const double R = 10, rr = 2;
    const Manifold cyl = Manifold::Cylinder(10, R, R, 128);
    const auto chains = ef::findChains(cyl);
    const int top = chainWhere(chains, [](vec3 p) { return std::abs(p.z - 10) < 1e-9; });
    CHECK(chains.size() == 2, "cylinder has %zu chains, expected 2", chains.size());
    CHECK(top >= 0 && chains[static_cast<size_t>(top)].closed, "cylinder top rim is not one loop");
    if (top >= 0) {
      std::string warning, log;
      const Manifold out = ef::applyEdgeFillets(cyl, {pickOf(chains[static_cast<size_t>(top)], rr)}, 12, &warning, &log);
      const double removed = cyl.Volume() - out.Volume();
      const double expected = 2 * kPiT * (R - kNubCentroid * rr) * rr * rr * kNub;
      std::printf("cylinder rim: removed %.3f, exact %.3f\n", removed, expected);
      CHECK(near(removed, expected, 0.02), "rim removed %.3f, expected %.3f", removed, expected);
    }
  }

  // 7. Hole rim in a plate.
  {
    const double Rh = 6, rr = 1.5;
    const Manifold plate = Manifold::Cube(vec3(40, 40, 8)) - Manifold::Cylinder(20, Rh, Rh, 128).Translate(vec3(20, 20, -5));
    const auto chains = ef::findChains(plate);
    const int rim = chainWhere(chains, [Rh](vec3 p) { return std::abs(p.z - 8) < 1e-9 && std::hypot(p.x - 20, p.y - 20) < Rh + 0.01; });
    CHECK(rim >= 0, "hole rim not found");
    if (rim >= 0) {
      CHECK(!chains[static_cast<size_t>(rim)].concave, "hole rim should be an outside edge");
      std::string warning, log;
      const Manifold out = ef::applyEdgeFillets(plate, {pickOf(chains[static_cast<size_t>(rim)], rr)}, 12, &warning, &log);
      const double removed = plate.Volume() - out.Volume();
      const double expected = 2 * kPiT * (Rh + kNubCentroid * rr) * rr * rr * kNub;
      std::printf("hole rim: removed %.3f, exact %.3f\n", removed, expected);
      CHECK(near(removed, expected, 0.02), "hole rim removed %.3f, expected %.3f", removed, expected);
    }
  }

  // 8. The pick still finds its edge after the solid changed a little (box made longer).
  {
    const Manifold longer = Manifold::Cube(vec3(34, 30, 30));
    const auto chains = ef::findChains(longer);
    const std::vector<int> found = ef::matchPick(chains, pickOf(boxChains[topFront], r).pts);
    CHECK(found.size() == 1, "after a resize the edge matched %zu chains", found.size());
    if (found.size() == 1) {
      const auto& c = chains[static_cast<size_t>(found[0])];
      bool ok = true;
      for (size_t k = 0; k < c.pts.size() / 3; ++k) ok = ok && std::abs(c.pts[k * 3 + 1]) < 1e-9 && std::abs(c.pts[k * 3 + 2] - 30) < 1e-9;
      CHECK(ok, "after a resize the wrong edge matched");
    }
    // And an edge that is gone is reported, not moved somewhere else.
    ef::Pick far;
    far.pts = {0, 100, 100, 30, 100, 100};
    far.radius = 1;
    int lost = 0;
    std::string warning;
    ef::applyEdgeFillets(longer, {far}, 12, &warning, nullptr, &lost);
    CHECK(lost == 1 && !warning.empty(), "a vanished edge was not reported");
  }

  // 10. An outside edge that ends on a plate (a post's vertical edge): the cut must not run on into
  //     the plate below, only up into the air above.
  {
    const Manifold both = Manifold::Cube(vec3(60, 60, 5)) + Manifold::Cube(vec3(20, 20, 20)).Translate(vec3(20, 20, 5));
    const auto chains = ef::findChains(both);
    const int edge = chainWhere(chains, [](vec3 p) { return std::abs(p.x - 40) < 1e-9 && std::abs(p.y - 20) < 1e-9; });
    CHECK(edge >= 0, "post edge not found");
    if (edge >= 0) {
      std::string warning, log;
      const Manifold out = ef::applyEdgeFillets(both, {pickOf(chains[static_cast<size_t>(edge)], 2.0)}, 12, &warning, &log);
      CHECK(contains(out, vec3(39.9, 20.1, 4.95)), "the post's fillet cut into the plate");
      CHECK(!contains(out, vec3(39.9, 20.1, 15)), "the post's edge was not rounded");
      CHECK(!contains(out, vec3(39.9, 20.1, 24.95)), "the post's edge was not rounded up to the top");
    }
  }

  // 12. Rounded uprights AND foot fillets on a post: the foot must follow the rounded corner,
  //     i.e. straight foot pieces shortened by the upright's radius plus a quarter torus around
  //     the upright's axis (Pappus), with no fins left at the old sharp corner.
  {
    const Manifold both = Manifold::Cube(vec3(60, 60, 5)) + Manifold::Cube(vec3(20, 20, 20)).Translate(vec3(20, 20, 5));
    const auto chains = ef::findChains(both);
    std::vector<ef::Pick> uprights, feet;
    for (const auto& c : chains) {
      bool foot = c.concave, upright = !c.concave;
      for (size_t k = 0; k < c.pts.size() / 3; ++k) {
        const double x = c.pts[k * 3], y = c.pts[k * 3 + 1], z = c.pts[k * 3 + 2];
        foot = foot && std::abs(z - 5) < 1e-9;
        upright = upright && std::abs(x - c.pts[0]) < 1e-9 && std::abs(y - c.pts[1]) < 1e-9 &&
                  (std::abs(x - 20) < 1e-9 || std::abs(x - 40) < 1e-9) && (std::abs(y - 20) < 1e-9 || std::abs(y - 40) < 1e-9);
      }
      if (foot) feet.push_back(pickOf(c, 2.0));
      if (upright) uprights.push_back(pickOf(c, 2.0));
    }
    CHECK(uprights.size() == 4 && feet.size() == 4, "post: %zu uprights, %zu feet", uprights.size(), feet.size());
    std::string warning, log;
    const Manifold cutOnly = ef::applyEdgeFillets(both, uprights, 12, &warning, &log);
    std::vector<ef::Pick> all = uprights;
    all.insert(all.end(), feet.begin(), feet.end());
    log.clear();
    const Manifold out = ef::applyEdgeFillets(both, all, 12, &warning, &log);
    const double fill = out.Volume() - cutOnly.Volume();
    const double area = 4 * kNub, xbar = kNubCentroid * 2;
    const double expected = 4 * 16 * area + 4 * (kPiT / 2) * (2 + xbar) * area;
    std::printf("rounded post foot: filled %.3f, exact %.3f\n%s", fill, expected, log.c_str());
    CHECK(near(fill, expected, 0.02), "rounded post foot filled %.3f, expected %.3f", fill, expected);
    // Old sharp corner, just above the foot fillet: air now.
    CHECK(!contains(out, vec3(39.9, 20.1, 7.6)), "fin left at the rounded corner");
    // Next to the rounded corner, low down: filled.
    CHECK(contains(out, vec3(39.6, 20.8, 5.1)), "foot fillet missing at the rounded corner");
  }

  // 11. An L-shaped block: the two top edges at the inside (reflex) corner are mitred, so they
  //     meet in a clean valley instead of leaving a spike in the corner.
  {
    const Manifold ell = Manifold::Cube(vec3(20, 10, 10)) + Manifold::Cube(vec3(10, 20, 10));
    const auto chains = ef::findChains(ell);
    const int a = chainWhere(chains, [](vec3 p) { return std::abs(p.y - 10) < 1e-9 && std::abs(p.z - 10) < 1e-9 && p.x > 9.99; });
    const int b = chainWhere(chains, [](vec3 p) { return std::abs(p.x - 10) < 1e-9 && std::abs(p.z - 10) < 1e-9 && p.y > 9.99; });
    CHECK(a >= 0 && b >= 0, "L-block top edges not found");
    if (a >= 0 && b >= 0) {
      std::string warning, log;
      const Manifold out = ef::applyEdgeFillets(
          ell, {pickOf(chains[static_cast<size_t>(a)], 2.0), pickOf(chains[static_cast<size_t>(b)], 2.0)}, 12, &warning, &log);
      CHECK(!contains(out, vec3(9.0, 9.6, 9.95)), "spike left at the inside corner");
      CHECK(!contains(out, vec3(9.6, 9.0, 9.95)), "spike left at the inside corner (other side)");
      CHECK(contains(out, vec3(5, 5, 9.95)), "the inside corner's mitre cut too far");
      CHECK(contains(out, vec3(7.5, 7.5, 9.95)), "the mitre cut the top face beyond the fillet");
    }
  }

  // 13. Robustness: every sharp edge of a messier solid at once (holes, a boss, a slot, a
  //     sphere bump). It must stay one valid solid and not take long.
  {
    Manifold part = Manifold::Cube(vec3(50, 40, 8));
    part = part - Manifold::Cylinder(20, 4, 4, 64).Translate(vec3(10, 10, -5));
    part = part - Manifold::Cylinder(20, 3, 3, 64).Translate(vec3(40, 30, -5));
    part = part + Manifold::Cylinder(10, 6, 6, 64).Translate(vec3(30, 12, 8));
    part = part - Manifold::Cube(vec3(20, 4, 10)).Translate(vec3(5, 28, 4));
    part = part + Manifold::Sphere(5, 48).Translate(vec3(15, 30, 8));
    const auto chains = ef::findChains(part);
    std::vector<ef::Pick> picks;
    for (const auto& c : chains) picks.push_back(pickOf(c, 0.8));
    std::string warning, log;
    const auto t0 = std::chrono::steady_clock::now();
    const Manifold out = ef::applyEdgeFillets(part, picks, 8, &warning, &log);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("messy part: %zu chains, %.0f ms, volume %.1f -> %.1f%s%s\n", chains.size(), ms, part.Volume(), out.Volume(),
                warning.empty() ? "" : ", warning: ", warning.c_str());
    std::printf("%s", log.c_str());
    for (size_t i = 0; i < chains.size(); ++i) std::printf("  chain %zu: %zu pts, %s, %s, len %.2f, angle %.1f\n", i, chains[i].pts.size() / 3, chains[i].closed ? "loop" : "open", chains[i].concave ? "inside" : "outside", chains[i].length, chains[i].angle);
    CHECK(out.Status() == Manifold::Error::NoError && !out.IsEmpty(), "messy part: result is not a valid solid");
    CHECK(std::abs(out.Volume() - part.Volume()) < 0.05 * part.Volume(), "messy part: volume changed too much");
    std::printf("messy genus %d -> %d, %zu component(s)\n", part.Genus(), out.Genus(), out.Decompose().size());
    CHECK(out.Genus() == part.Genus(), "messy part: genus changed from %d to %d", part.Genus(), out.Genus());
    CHECK(ms < 20000, "messy part took %.0f ms", ms);
  }

  // 14. A weld exported as STL (32-bit floats) must stay manifold: the weld around a post's
  //     sharp corners is one continuous sweep, and what is finer than a float step is cleaned
  //     away first (as the app's exporter does). Counted the way a slicer does.
  {
    const Manifold both = (Manifold::Cube(vec3(44.00001, 44.00001, 44.00001), true).Translate(vec3(0, 0, -15)) +
                           Manifold::Cube(vec3(30, 30, 30), true).Translate(vec3(4, -3, 22)))
                              .Translate(vec3(-61, 7, 37));
    const auto chains = ef::findChains(both);
    std::vector<ef::Pick> feet;
    for (const auto& c : chains)
      if (c.concave) feet.push_back(pickOf(c, 5.0));
    std::string warning, log;
    const Manifold out = ef::applyEdgeFillets(both, feet, 48, &warning, &log);
    const manifold::Box box = out.BoundingBox();
    double far = 1.0;
    for (double v : {box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z}) far = std::max(far, std::abs(v));
    const Manifold ready = out.Simplify(std::max(1e-5, far * 6e-7));
    const auto gl = ready.GetMeshGL64();
    std::map<std::array<float, 3>, int> ids;
    std::map<std::pair<int, int>, int> uses;
    int collapsed = 0;
    for (size_t t = 0; t + 2 < gl.triVerts.size(); t += 3) {
      int v[3];
      for (int k = 0; k < 3; ++k) {
        const size_t i = gl.triVerts[t + k] * gl.numProp;
        const std::array<float, 3> key{float(gl.vertProperties[i]), float(gl.vertProperties[i + 1]), float(gl.vertProperties[i + 2])};
        auto it = ids.find(key);
        v[k] = it == ids.end() ? (ids[key] = static_cast<int>(ids.size())) : it->second;
      }
      if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) {
        ++collapsed;
        continue;
      }
      for (int k = 0; k < 3; ++k) uses[{std::min(v[k], v[(k + 1) % 3]), std::max(v[k], v[(k + 1) % 3])}]++;
    }
    int bad = 0;
    for (const auto& e : uses) bad += e.second != 2 ? 1 : 0;
    std::printf("weld as STL: %zu feet, %d collapsed triangles, %d non-manifold edges\n", feet.size(), collapsed, bad);
    CHECK(feet.size() == 4 && collapsed == 0 && bad == 0, "weld STL: %d collapsed, %d non-manifold", collapsed, bad);
  }

  // 9. Storage round trip.
  {
    std::vector<ef::Pick> picks{pickOf(boxChains[topFront], 1.5), pickOf(boxChains[topRight], 2.5)};
    const auto back = ef::decodePicks(ef::encodePicks(picks));
    CHECK(back.size() == 2 && back[0].radius == 1.5 && back[1].radius == 2.5 && back[1].pts == picks[1].pts,
          "picks did not survive encode/decode");
  }

  // Chamfers: the same picks cut flat. One edge takes a triangle of r x r / 2 along its length; all
  // twelve edges of a cube make the chamfered cube (each corner where three cuts meet in a point),
  // which is the cube cut by the twelve 45 degree planes; an inside edge is filled with a triangle.
  {
    auto chamferOf = [](const ef::Chain& chain, double size) {
      ef::Pick pick = pickOf(chain, size);
      pick.chamfer = true;
      return pick;
    };
    std::string warning, log;
    const Manifold one = ef::applyEdgeFillets(box, {chamferOf(boxChains[topFront], r)}, 12, &warning, &log);
    const double removed = box.Volume() - one.Volume();
    std::printf("chamfer one edge: removed %.4f, exact %.4f\n", removed, 30 * r * r / 2);
    CHECK(near(removed, 30 * r * r / 2, 1e-3), "one chamfer removed %.4f, expected %.4f", removed, 30 * r * r / 2);
    CHECK(!contains(one, vec3(15, r * 0.45, 30 - r * 0.45)), "the chamfer cut the corner");
    CHECK(contains(one, vec3(15, r * 0.55, 30 - r * 0.55)), "and stops at its 45 degree plane");

    std::vector<ef::Pick> all;
    for (const auto& chain : boxChains) all.push_back(chamferOf(chain, r));
    const Manifold cube = ef::applyEdgeFillets(box, all, 12, &warning, &log);
    Manifold exact = box;
    for (int a = 0; a < 3; ++a)
      for (int b = a + 1; b < 3; ++b)
        for (int sa = 0; sa < 2; ++sa)
          for (int sb = 0; sb < 2; ++sb) {
            // Keep the side of the plane t_a + t_b = r away from each of the four edges along the third axis.
            vec3 n(0, 0, 0);
            n[a] = sa ? -1 : 1;
            n[b] = sb ? -1 : 1;
            const double offset = r + (sa ? -30 : 0) + (sb ? -30 : 0);
            exact = exact.TrimByPlane(n * (1 / std::sqrt(2.0)), offset / std::sqrt(2.0));
          }
    std::printf("chamfered cube: %.3f, exact %.3f, %zu faces\n", cube.Volume(), exact.Volume(), cube.NumTri());
    CHECK(near(cube.Volume(), exact.Volume(), 1e-4), "chamfered cube %.3f, expected %.3f", cube.Volume(), exact.Volume());
    CHECK(std::abs((cube - exact).Volume()) < 0.05 && std::abs((exact - cube).Volume()) < 0.05, "the chamfered cube has the exact shape");

    // Storage keeps the chamfer apart from a round of the same size.
    const auto stored = ef::encodePicks({chamferOf(boxChains[topFront], 2.5), pickOf(boxChains[topRight], 1.5)});
    const auto back = ef::decodePicks(stored);
    CHECK(back.size() == 2 && back[0].chamfer && std::abs(back[0].radius - 2.5) < 1e-12 && !back[1].chamfer &&
              std::abs(back[1].radius - 1.5) < 1e-12,
          "chamfer survives saving");

    // An inside edge: an L of two blocks, the edge where they meet filled flat.
    const Manifold l = Manifold::Cube(vec3(30, 30, 10)) + Manifold::Cube(vec3(30, 10, 30));
    const auto lChains = ef::findChains(l);
    const int inner = chainWhere(lChains, [](vec3 p) { return std::abs(p.y - 10) < 1e-9 && std::abs(p.z - 10) < 1e-9; });
    CHECK(inner >= 0 && lChains[static_cast<size_t>(inner)].concave, "the L's inside edge is found");
    if (inner >= 0) {
      const Manifold filled = ef::applyEdgeFillets(l, {chamferOf(lChains[static_cast<size_t>(inner)], r)}, 12, &warning, &log);
      const double added = filled.Volume() - l.Volume();
      std::printf("inside chamfer: added %.4f, exact %.4f\n", added, 30 * r * r / 2);
      CHECK(near(added, 30 * r * r / 2, 1e-3), "inside chamfer added %.4f, expected %.4f", added, 30 * r * r / 2);
    }
  }

  // A cove ending at a rounded (or chamfered) upright: two blocks with a lower floor between them,
  // the left block's inner front edge rounded and the cove where its inner wall meets the floor.
  // The cove must wrap round the upright's rounding to the front face, not end flat and stick out
  // in front of it (where the rounding took the wall away).
  {
    const Manifold trench = Manifold::Cube({20, 20, 20}).Translate({-40, -10, 0}) + Manifold::Cube({20, 20, 20}).Translate({20, -10, 0}) +
                            Manifold::Cube({80, 20, 10}).Translate({-40, -10, 0});
    const auto chains = ef::findChains(trench);
    const int upright = chainWhere(chains, [](const vec3& p) { return std::abs(p.x + 20) < 1e-6 && std::abs(p.y + 10) < 1e-6 && p.z >= 10 - 1e-6; });
    const int cove = chainWhere(chains, [](const vec3& p) { return std::abs(p.x + 20) < 1e-6 && std::abs(p.z - 10) < 1e-6; });
    const int front = chainWhere(chains, [](const vec3& p) { return std::abs(p.y + 10) < 1e-6 && std::abs(p.z - 10) < 1e-6 && p.x >= -20 - 1e-6 && p.x <= 20 + 1e-6; });
    CHECK(upright >= 0 && cove >= 0 && front >= 0, "trench: upright %d, cove %d, front %d", upright, cove, front);
    if (upright >= 0 && cove >= 0 && front >= 0) {
      for (int chamfer = 0; chamfer < 2; ++chamfer) {
        ef::Pick up = pickOf(chains[static_cast<size_t>(upright)], r);
        up.chamfer = chamfer == 1;
        std::string warning, log;
        const Manifold out = ef::applyEdgeFillets(trench, {up, pickOf(chains[static_cast<size_t>(cove)], r)}, 12, &warning, &log);
        const char* kind = chamfer ? "chamfered" : "rounded";
        CHECK(out.Genus() == 0 && out.Decompose().size() == 1, "trench, %s upright: one solid", kind);
        CHECK(log.find("1 wrap") != std::string::npos, "trench, %s upright: the cove wraps round it (%s)", kind, log.c_str());
        // Just in front of where the wall was, a little above the floor: the old cove's flat end
        // stuck out here.
        CHECK(!contains(out, vec3(-19.5, -9.9, 11)), "trench, %s upright: the cove sticks out past it", kind);
        // The cove is still there along the wall.
        CHECK(contains(out, vec3(-19.7, 5, 10.3)), "trench, %s upright: the cove is gone", kind);
      }
      // A cove ending at the floor's rounded front edge follows it down instead of overhanging it.
      std::string warning, log;
      const Manifold out = ef::applyEdgeFillets(trench, {pickOf(chains[static_cast<size_t>(front)], r), pickOf(chains[static_cast<size_t>(cove)], r)}, 12,
                                                &warning, &log);
      CHECK(log.find("1 wrap") != std::string::npos, "trench, rounded front: the cove wraps round it (%s)", log.c_str());
      // Under where the cove used to end flat in mid-air: now the cove carries on down there.
      CHECK(contains(out, vec3(-19.7, -9.9, 9.8)), "trench, rounded front: the cove overhangs the rounded floor edge");
    }
  }

  // The inside corner of a pocket with all its inside edges rounded: a filled ball corner, not the
  // notch the three coves leave between them.
  {
    const Manifold pocket = Manifold::Cube({60, 40, 20}, true).Translate({0, 0, 10}) - Manifold::Cube({40, 20, 20}, true).Translate({0, 0, 15});
    const auto chains = ef::findChains(pocket);
    std::vector<ef::Pick> picks;
    for (const auto& chain : chains)
      if (chain.concave) picks.push_back(pickOf(chain, r));
    std::string warning, log;
    const Manifold out = ef::applyEdgeFillets(pocket, picks, 12, &warning, &log);
    CHECK(picks.size() == 8, "pocket: %zu inside edges", picks.size());
    CHECK(log.find("4 ball corner") != std::string::npos, "pocket: ball corners (%s)", log.c_str());
    // 0.4 r in from the corner along all three edges: outside every cove, inside the ball's fill.
    CHECK(contains(out, vec3(-20 + 0.4 * r, -10 + 0.4 * r, 5 + 0.4 * r)), "pocket: notch left in the inside corner");
    CHECK(!contains(out, vec3(-20 + 1.2 * r, -10 + 1.2 * r, 5 + 1.2 * r)), "pocket: the corner is filled too far");
    CHECK(out.Genus() == 0 && out.Decompose().size() == 1, "pocket: one solid");
  }

  std::printf(fails ? "edgefillet_test: %d failure(s)\n" : "edgefillet_test: all passed\n", fails);
  return fails ? 1 : 0;
}
