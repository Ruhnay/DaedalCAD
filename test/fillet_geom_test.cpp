// Checks the cutter pieces for one outline against the cross-section they are meant to sweep,
// and the cross-section against an ideal rolling-ball fillet.
#include "../src/fillet_geom.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <vector>
using namespace fg;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double overOf(double far) { return std::max(0.02, far * 2e-5); }
static double biteOf(double far) { return std::max(0.005, far * 5e-6); }

// ---- mesh checks -----------------------------------------------------------
struct Chk { bool closed; double volume; };
static Chk check(const RawMesh& m) {
  std::map<std::pair<uint32_t,uint32_t>, int> e;
  for (size_t i = 0; i < m.t.size(); i += 3)
    for (int k = 0; k < 3; ++k) e[{m.t[i+k], m.t[i+(k+1)%3]}]++;
  bool closed = true;
  for (auto& kv : e) {
    if (kv.second != 1) closed = false;
    auto r = e.find({kv.first.second, kv.first.first});
    if (r == e.end() || r->second != 1) closed = false;
  }
  double vol = 0;
  for (size_t i = 0; i < m.t.size(); i += 3) {
    const double* a = &m.v[m.t[i]*3]; const double* b = &m.v[m.t[i+1]*3]; const double* c = &m.v[m.t[i+2]*3];
    vol += (a[0]*(b[1]*c[2]-b[2]*c[1]) - a[1]*(b[0]*c[2]-b[2]*c[0]) + a[2]*(b[0]*c[1]-b[1]*c[0])) / 6.0;
  }
  return {closed, vol};
}

static bool inside(const RawMesh& m, double px, double py, double pz) {
  const double dir[3] = {0.5773, 0.3511, 0.7337};
  int hits = 0;
  for (size_t i = 0; i < m.t.size(); i += 3) {
    const double* a = &m.v[m.t[i]*3]; const double* b = &m.v[m.t[i+1]*3]; const double* c = &m.v[m.t[i+2]*3];
    double e1[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]}, e2[3] = {c[0]-a[0], c[1]-a[1], c[2]-a[2]};
    double h[3] = {dir[1]*e2[2]-dir[2]*e2[1], dir[2]*e2[0]-dir[0]*e2[2], dir[0]*e2[1]-dir[1]*e2[0]};
    double det = e1[0]*h[0]+e1[1]*h[1]+e1[2]*h[2];
    if (std::abs(det) < 1e-14) continue;
    double s[3] = {px-a[0], py-a[1], pz-a[2]};
    double u = (s[0]*h[0]+s[1]*h[1]+s[2]*h[2]) / det;
    if (u < 0 || u > 1) continue;
    double q[3] = {s[1]*e1[2]-s[2]*e1[1], s[2]*e1[0]-s[0]*e1[2], s[0]*e1[1]-s[1]*e1[0]};
    double v = (dir[0]*q[0]+dir[1]*q[1]+dir[2]*q[2]) / det;
    if (v < 0 || u + v > 1) continue;
    double t = (e2[0]*q[0]+e2[1]*q[1]+e2[2]*q[2]) / det;
    if (t > 0) ++hits;
  }
  return hits & 1;
}

// ---- geometry helpers ------------------------------------------------------
static double segDist(const Vec2& p, const Vec2& a, const Vec2& b) {
  double dx = b.x-a.x, dy = b.y-a.y, len2 = dx*dx+dy*dy;
  double t = len2 < 1e-18 ? 0.0 : ((p.x-a.x)*dx + (p.y-a.y)*dy) / len2;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(p.x-(a.x+t*dx), p.y-(a.y+t*dy));
}
static double ringDist(const Vec2& p, const std::vector<Vec2>& r) {
  double best = 1e9;
  for (size_t i = 0; i < r.size(); ++i) best = std::min(best, segDist(p, r[i], r[(i+1)%r.size()]));
  return best;
}
static bool inPoly(const Vec2& p, const std::vector<Vec2>& r) {
  bool in = false;
  for (size_t i = 0, j = r.size()-1; i < r.size(); j = i++)
    if (((r[i].y > p.y) != (r[j].y > p.y)) && (p.x < (r[j].x-r[i].x)*(p.y-r[i].y)/(r[j].y-r[i].y)+r[i].x)) in = !in;
  return in;
}

// How far below the face the cross-section reaches, at distance d into the face.
static double depthAt(const Shape& sh, double d) {
  std::vector<std::array<double,2>> c = sh.chain;      // (radius, height), outermost first
  for (auto& q : c) q[1] = std::abs(q[1]);             // same both ways up
  if (d >= c.front()[0]) return 0;
  if (d <= c.back()[0]) return c.back()[1];
  for (size_t i = 0; i + 1 < c.size(); ++i) {
    if (d <= c[i][0] && d >= c[i+1][0]) {
      const double span = c[i][0] - c[i+1][0];
      if (span < 1e-12) return std::max(c[i][1], c[i+1][1]);
      const double f = (c[i][0] - d) / span;
      return c[i][1] + f * (c[i+1][1] - c[i][1]);
    }
  }
  return 0;
}

static std::vector<RawMesh> pieces(const std::vector<Vec2>& ring, const Shape& sh, double r, double over) {
  std::vector<RawMesh> out;
  const size_t N = ring.size();
  std::vector<double> turn(N);
  for (size_t i = 0; i < N; ++i) turn[i] = turnAngle(ring[(i+N-1)%N], ring[i], ring[(i+1)%N]);
  for (size_t i = 0; i < N; ++i) {
    const size_t j = (i+1)%N;
    out.push_back(stripMesh(ring[i], ring[j], sh, cornerExtend(turn[i], r, over), cornerExtend(turn[j], r, over)));
    if (needsDome(turn[i], r)) out.push_back(domeMesh(ring[i].x, ring[i].y, sh, 32));
  }
  return out;
}

// ---- the cross-section must track an ideal rolling ball --------------------
static void checkShape(double r, int n, double far) {
  const double over = overOf(far), bite = biteOf(far);
  for (bool below : {true, false}) {
    const Shape sh = crossSection(r, n, over, bite, below);
    // the fan the end caps use is only valid if the profile is star-shaped from vertex 0
    const auto& p = sh.profile;
    for (size_t j = 1; j + 1 < p.size(); ++j) {
      const double ax = p[j][0]-p[0][0], ay = p[j][1]-p[0][1], bx = p[j+1][0]-p[0][0], by = p[j+1][1]-p[0][1];
      CHECK(ax*by - ay*bx > -1e-12, "r=%g n=%d below=%d: profile not star-shaped at %zu", r, n, (int)below, j);
    }
    if (!below) continue;

    // Every point of the faceted arc sits on the true fillet circle, so each facet strays from it
    // by exactly that facet's sagitta and no more.
    const auto& c = sh.chain;
    CHECK(c.size() >= 4, "r=%g n=%d: chain too short", r, n);
    double offCircle = 0, widestFacet = 0;
    double prevAngle = 0;
    for (size_t i = 1; i + 1 < c.size(); ++i) {       // interior entries are the arc
      const double dx = c[i][0] - r, dz = c[i][1] + r;
      offCircle = std::max(offCircle, std::abs(std::hypot(dx, dz) - r));
      const double ang = std::atan2(dz, dx);
      if (i > 1) widestFacet = std::max(widestFacet, std::abs(ang - prevAngle));
      prevAngle = ang;
    }
    CHECK(offCircle < 1e-9, "r=%g n=%d: arc points sit %.3g mm off the fillet circle", r, n, offCircle);
    CHECK(widestFacet <= kPi * 0.5 / n + 1e-9, "r=%g n=%d: a facet spans %.4f rad, budget %.4f",
          r, n, widestFacet, kPi * 0.5 / n);

    // The chain must fall monotonically inward and downward, or the dome that revolves it would
    // fold through itself.
    for (size_t i = 0; i + 1 < c.size(); ++i) {
      CHECK(c[i+1][0] <= c[i][0] + 1e-12, "r=%g n=%d: chain radius grows at %zu", r, n, i);
      CHECK(c[i+1][1] <= c[i][1] + 1e-12, "r=%g n=%d: chain height grows at %zu", r, n, i);
    }

    // It may cut past each tangent line, but only by the bite we asked for.
    CHECK(depthAt(sh, sh.reach - 1e-9) <= bite * 1.001, "r=%g n=%d: cuts %.4f mm into the face, budget %.4f",
          r, n, depthAt(sh, sh.reach - 1e-9), bite);
    CHECK(c.back()[0] <= bite * 1.001 && c.back()[0] > 0, "r=%g n=%d: cuts %.4f mm into the wall, budget %.4f",
          r, n, c.back()[0], bite);
    // and in exchange it starts a hair inside where a true fillet would, by the width of that bite
    const double shortfall = r - sh.reach;
    CHECK(shortfall <= std::sqrt(2.0 * r * bite) + 1e-9, "r=%g n=%d: fillet starts %.4f mm late, budget %.4f",
          r, n, shortfall, std::sqrt(2.0 * r * bite));
  }
}

// ---- the pieces must sweep exactly that cross-section ----------------------
static void run(const char* name, std::vector<Vec2> ring, bool isHole, double r, int n) {
  ring = cleanRing(ring);
  if (isHole != (signedArea(ring) < 0)) std::reverse(ring.begin(), ring.end());
  double far = 1.0;
  for (auto& p : ring) far = std::max(far, std::max(std::abs(p.x), std::abs(p.y)));
  const double over = overOf(far), bite = biteOf(far);
  const Shape sh = crossSection(r, n, over, bite, true);
  auto ps = pieces(ring, sh, r, over);
  int domes = 0;
  for (auto& pc : ps) {
    Chk c = check(pc);
    CHECK(c.closed, "%s: a piece is not a closed manifold", name);
    CHECK(c.volume > 0, "%s: a piece has volume %g", name, c.volume);
    if (pc.v.size() / 3 > 200) ++domes;
  }
  double lo[2] = {1e9,1e9}, hi[2] = {-1e9,-1e9};
  for (auto& p : ring) { lo[0]=std::min(lo[0],p.x); lo[1]=std::min(lo[1],p.y); hi[0]=std::max(hi[0],p.x); hi[1]=std::max(hi[1],p.y); }
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> ux(lo[0]-1, hi[0]+1), uy(lo[1]-1, hi[1]+1), uz(0, r*1.05);
  int ridge = 0, overcut = 0, tested = 0, removed = 0;
  for (int it = 0; it < 20000; ++it) {
    const Vec2 p{ux(rng), uy(rng)};
    const double t = uz(rng);                       // depth below the face
    if (isHole ? inPoly(p, ring) : !inPoly(p, ring)) continue;   // must be on the face side
    const double d = ringDist(p, ring);
    if (d < bite + 0.01) continue;                  // the deliberate step beside the wall
    const double want = depthAt(sh, d);
    if (std::abs(t - want) < 0.01) continue;        // right on the surface
    const bool got = [&]{ for (auto& pc : ps) if (inside(pc, p.x, p.y, -t)) return true; return false; }();
    ++tested; if (got) ++removed;
    // A point the cross-section says to remove must be gone: anything left is a standing ridge.
    if (!got && t < want - 1e-9) ++ridge;
    // A point it says to keep may still go, but only within the corner budget.
    if (got && t > depthAt(sh, std::max(0.0, d - kCornerTol)) + 1e-9) ++overcut;
  }
  CHECK(ridge == 0, "%s: %d of %d samples left a ridge the cutter should have taken", name, ridge, tested);
  CHECK(overcut == 0, "%s: %d of %d samples cut past the %.3f mm corner budget", name, overcut, tested, kCornerTol);
  printf("%-24s pieces=%2zu domes=%d  samples=%d (cut=%d) ridges=%d overcuts=%d\n",
         name, ps.size(), domes, tested, removed, ridge, overcut);
}

// ---- the cutter must cross the wall, never stop on it ----------------------
static void checkOverhang(const char* name, std::vector<Vec2> ring, bool isHole, double r, int n) {
  ring = cleanRing(ring);
  if (isHole != (signedArea(ring) < 0)) std::reverse(ring.begin(), ring.end());
  double far = 1.0;
  for (auto& p : ring) far = std::max(far, std::max(std::abs(p.x), std::abs(p.y)));
  const double over = overOf(far);
  const Shape sh = crossSection(r, n, over, biteOf(far), true);
  auto ps = pieces(ring, sh, r, over);
  const double probe = 0.008;  // fixed, not a fraction of the overhang: this is the property that matters
  int missed = 0, tested = 0;
  for (size_t i = 0; i < ring.size(); ++i) {
    const Vec2& a = ring[i]; const Vec2& b = ring[(i+1)%ring.size()];
    const double dx = b.x-a.x, dy = b.y-a.y, len = std::hypot(dx,dy);
    if (len < 1e-9) continue;
    const double nx = dy/len, ny = -dx/len;      // face is on the left, so this points outward
    for (double f : {0.25, 0.5, 0.75}) {
      const Vec2 q{a.x+dx*f + nx*probe, a.y+dy*f + ny*probe};
      for (double t : {r*0.25, r*0.75}) {
        const bool got = [&]{ for (auto& pc : ps) if (inside(pc, q.x, q.y, -t)) return true; return false; }();
        ++tested; if (!got) ++missed;
      }
    }
  }
  CHECK(missed == 0, "%s: cutter stops short of the wall at %d of %d spots", name, missed, tested);
  printf("%-24s reaches %.3f mm past the wall at %d/%d spots\n", name, probe, tested-missed, tested);
}

int main() {
  for (double r : {0.3, 1.0, 2.0, 8.0})
    for (int n : {1, 2, 4, 12, 20})
      for (double far : {10.0, 200.0, 2000.0}) checkShape(r, n, far);

  std::vector<Vec2> sq = {{0,0},{20,0},{20,20},{0,20}};
  std::vector<Vec2> L = {{0,0},{30,0},{30,10},{10,10},{10,30},{0,30}};
  std::vector<Vec2> hole = {{5,5},{5,15},{15,15},{15,5}};
  std::vector<Vec2> tri = {{0,0},{30,0},{4,6}};
  std::vector<Vec2> tee = {{0,0},{10,0},{20,0},{20,20},{0,20}};
  std::vector<Vec2> circ, fine;
  for (int i = 0; i < 40; ++i) circ.push_back({15+8*std::cos(2*kPi*i/40), 15+8*std::sin(2*kPi*i/40)});
  for (int i = 0; i < 128; ++i) fine.push_back({40+20*std::cos(2*kPi*i/128), 40+20*std::sin(2*kPi*i/128)});

  run("square r2 n4", sq, false, 2, 4);
  run("square r2 n1 chamfer", sq, false, 2, 1);
  run("L (reflex) r2 n6", L, false, 2, 6);
  run("square hole r1.5", hole, true, 1.5, 4);
  run("circle hole r1 n8", circ, true, 1, 8);
  run("128-gon hole r4 n12", fine, true, 4, 12);
  run("acute triangle r1", tri, false, 1, 4);
  run("T-junction square", tee, false, 2, 4);
  printf("\n");
  checkOverhang("square overhang", sq, false, 2, 4);
  checkOverhang("L overhang", L, false, 2, 6);
  checkOverhang("square hole overhang", hole, true, 1.5, 4);
  checkOverhang("128-gon overhang", fine, true, 4, 12);
  checkOverhang("acute tri overhang", tri, false, 1, 4);
  printf(fails ? "\n%d FAILURES\n" : "\nALL PASSED\n", fails);
  return fails != 0;
}
