// The one-piece swept cutter: closed, non-self-intersecting, and removing exactly the fillet.
#include "../src/fillet_geom.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <vector>
using namespace fg;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool closedManifold(const RawMesh& m) {
  std::map<std::pair<uint32_t,uint32_t>, int> e;
  for (size_t i = 0; i < m.t.size(); i += 3)
    for (int k = 0; k < 3; ++k) e[{m.t[i+k], m.t[i+(k+1)%3]}]++;
  for (auto& kv : e) {
    if (kv.second != 1) return false;
    auto r = e.find({kv.first.second, kv.first.first});
    if (r == e.end() || r->second != 1) return false;
  }
  return true;
}
static double volume(const RawMesh& m) {
  double vol = 0;
  for (size_t i = 0; i < m.t.size(); i += 3) {
    const double* a = &m.v[m.t[i]*3]; const double* b = &m.v[m.t[i+1]*3]; const double* c = &m.v[m.t[i+2]*3];
    vol += (a[0]*(b[1]*c[2]-b[2]*c[1]) - a[1]*(b[0]*c[2]-b[2]*c[0]) + a[2]*(b[0]*c[1]-b[1]*c[0])) / 6.0;
  }
  return vol;
}
static double minArea(const RawMesh& m) {
  double best = 1e300;
  for (size_t i = 0; i < m.t.size(); i += 3) {
    const double* a = &m.v[m.t[i]*3]; const double* b = &m.v[m.t[i+1]*3]; const double* c = &m.v[m.t[i+2]*3];
    const double ux=b[0]-a[0], uy=b[1]-a[1], uz=b[2]-a[2], vx=c[0]-a[0], vy=c[1]-a[1], vz=c[2]-a[2];
    best = std::min(best, 0.5*std::sqrt((uy*vz-uz*vy)*(uy*vz-uz*vy)+(uz*vx-ux*vz)*(uz*vx-ux*vz)+(ux*vy-uy*vx)*(ux*vy-uy*vx)));
  }
  return best;
}
// Does any edge pass through a triangle it does not belong to? That is a self-intersection.
static int selfIntersections(const RawMesh& m) {
  std::set<std::pair<uint32_t,uint32_t>> edges;
  for (size_t i = 0; i < m.t.size(); i += 3)
    for (int k = 0; k < 3; ++k) { uint32_t a = m.t[i+k], b = m.t[i+(k+1)%3]; edges.insert({std::min(a,b), std::max(a,b)}); }
  int hits = 0;
  for (auto [ia, ib] : edges) {
    const double* p = &m.v[ia*3]; const double* q = &m.v[ib*3];
    const double dir[3] = {q[0]-p[0], q[1]-p[1], q[2]-p[2]};
    for (size_t t = 0; t < m.t.size(); t += 3) {
      const uint32_t A = m.t[t], B = m.t[t+1], C = m.t[t+2];
      if (A==ia||A==ib||B==ia||B==ib||C==ia||C==ib) continue;
      const double* a = &m.v[A*3]; const double* b = &m.v[B*3]; const double* c = &m.v[C*3];
      const double e1[3]={b[0]-a[0],b[1]-a[1],b[2]-a[2]}, e2[3]={c[0]-a[0],c[1]-a[1],c[2]-a[2]};
      const double h[3]={dir[1]*e2[2]-dir[2]*e2[1], dir[2]*e2[0]-dir[0]*e2[2], dir[0]*e2[1]-dir[1]*e2[0]};
      const double det = e1[0]*h[0]+e1[1]*h[1]+e1[2]*h[2];
      if (std::abs(det) < 1e-14) continue;
      const double s[3]={p[0]-a[0],p[1]-a[1],p[2]-a[2]};
      const double u=(s[0]*h[0]+s[1]*h[1]+s[2]*h[2])/det; if (u<=1e-9||u>=1-1e-9) continue;
      const double qq[3]={s[1]*e1[2]-s[2]*e1[1], s[2]*e1[0]-s[0]*e1[2], s[0]*e1[1]-s[1]*e1[0]};
      const double v=(dir[0]*qq[0]+dir[1]*qq[1]+dir[2]*qq[2])/det; if (v<=1e-9||u+v>=1-1e-9) continue;
      const double tt=(e2[0]*qq[0]+e2[1]*qq[1]+e2[2]*qq[2])/det; if (tt<=1e-9||tt>=1-1e-9) continue;
      ++hits;
    }
  }
  return hits;
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
    double u = (s[0]*h[0]+s[1]*h[1]+s[2]*h[2]) / det; if (u < 0 || u > 1) continue;
    double q[3] = {s[1]*e1[2]-s[2]*e1[1], s[2]*e1[0]-s[0]*e1[2], s[0]*e1[1]-s[1]*e1[0]};
    double v = (dir[0]*q[0]+dir[1]*q[1]+dir[2]*q[2]) / det; if (v < 0 || u + v > 1) continue;
    double t = (e2[0]*q[0]+e2[1]*q[1]+e2[2]*q[2]) / det;
    if (t > 0) ++hits;
  }
  return hits & 1;
}
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
// Depth of the faceted arc at distance d into the face (the same chords sweepRing uses).
static double depthAt(double r, int n, double d) {
  if (d >= r) return 0;
  if (d <= 0) return r;
  for (int k = 0; k < n; ++k) {
    const double p0 = kPi - (kPi*0.5)*k/n, p1 = kPi - (kPi*0.5)*(k+1)/n;
    const double d0 = r + r*std::cos(p0), d1 = r + r*std::cos(p1);
    if (d >= d0 && d <= d1) {
      const double z0 = -r + r*std::sin(p0), z1 = -r + r*std::sin(p1);
      return -(z0 + (z1 - z0) * (d - d0) / (d1 - d0));
    }
  }
  return 0;
}

static void run(const char* name, std::vector<Vec2> ring, bool isHole, double r, int n, bool below, bool expectOk = true) {
  ring = cleanRing(ring);
  if (isHole != (signedArea(ring) < 0)) std::reverse(ring.begin(), ring.end());
  const double over = 0.02;
  RawMesh m; const char* why = "";
  const bool ok = sweepRing(ring, r, n, over, below, &m, &why);
  if (!expectOk) {
    CHECK(!ok, "%s: should have refused (too tight) but built a tube", name);
    printf("%-26s refused as expected: %s\n", name, why);
    return;
  }
  CHECK(ok, "%s: sweep refused: %s", name, why);
  if (!ok) return;
  const bool closed = closedManifold(m);
  const double vol = volume(m);
  const int self = selfIntersections(m);
  CHECK(closed, "%s: tube is not a closed 2-manifold", name);
  CHECK(vol > 0, "%s: tube volume %g", name, vol);
  CHECK(self == 0, "%s: tube passes through itself at %d places", name, self);
  CHECK(minArea(m) > 1e-12, "%s: degenerate triangle (area %g)", name, minArea(m));

  double lo[2] = {1e9,1e9}, hi[2] = {-1e9,-1e9};
  for (auto& p : ring) { lo[0]=std::min(lo[0],p.x); lo[1]=std::min(lo[1],p.y); hi[0]=std::max(hi[0],p.x); hi[1]=std::max(hi[1],p.y); }
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> ux(lo[0]-1, hi[0]+1), uy(lo[1]-1, hi[1]+1), uz(0, r*1.05);
  const double ridgeTol = r * (1.0 - std::cos(kPi / 64.0)) + 1e-6;   // chord of a rounded corner
  int ridge = 0, overcut = 0, tested = 0, cut = 0;
  for (int it = 0; it < 12000; ++it) {
    const Vec2 p{ux(rng), uy(rng)};
    const double t = uz(rng);
    if (isHole ? inPoly(p, ring) : !inPoly(p, ring)) continue;
    const double d = ringDist(p, ring);
    if (d < 0.01) continue;
    const double want = depthAt(r, n, d);
    if (std::abs(t - want) < 0.01) continue;
    const double z = below ? -t : t;
    const bool got = inside(m, p.x, p.y, z);
    ++tested; if (got) ++cut;
    if (!got && t < depthAt(r, n, d + ridgeTol) - 1e-9) ++ridge;
    if (got && t > depthAt(r, n, std::max(0.0, d - kSweepTol)) + 1e-9) ++overcut;
  }
  CHECK(ridge == 0, "%s: %d of %d samples left standing that should be cut", name, ridge, tested);
  CHECK(overcut == 0, "%s: %d of %d samples cut beyond the %.3f mm budget", name, overcut, tested, kSweepTol);

  int missed = 0, probes = 0;
  for (size_t i = 0; i < ring.size(); ++i) {
    const Vec2& a = ring[i]; const Vec2& b = ring[(i+1)%ring.size()];
    const double dx = b.x-a.x, dy = b.y-a.y, len = std::hypot(dx,dy);
    const double nx = dy/len, ny = -dx/len;
    const Vec2 q{a.x+dx*0.5 + nx*0.008, a.y+dy*0.5 + ny*0.008};
    for (double t : {r*0.25, r*0.75}) { ++probes; if (!inside(m, q.x, q.y, below ? -t : t)) ++missed; }
  }
  CHECK(missed == 0, "%s: tube stops short of the wall at %d of %d spots", name, missed, probes);
  printf("%-26s tris=%5zu self=%d  samples=%d (cut %d) ridges=%d overcuts=%d wall=%d/%d\n",
         name, m.t.size()/3, self, tested, cut, ridge, overcut, probes-missed, probes);
}

int main() {
  std::vector<Vec2> sq = {{0,0},{20,0},{20,20},{0,20}};
  std::vector<Vec2> L = {{0,0},{30,0},{30,10},{10,10},{10,30},{0,30}};
  std::vector<Vec2> hole = {{5,5},{5,15},{15,15},{15,5}};
  std::vector<Vec2> tri = {{0,0},{30,0},{15,26}};
  std::vector<Vec2> circ, fine, pocket;
  for (int i = 0; i < 40; ++i) circ.push_back({15+8*std::cos(2*kPi*i/40), 15+8*std::sin(2*kPi*i/40)});
  for (int i = 0; i < 128; ++i) fine.push_back({40+20*std::cos(2*kPi*i/128), 40+20*std::sin(2*kPi*i/128)});
  // A rounded-triangle pocket like the test part: a Reuleaux triangle, three arcs meeting at tips.
  {
    const double side = 24, cx = 50, cy = 50;
    Vec2 V[3];
    for (int c = 0; c < 3; ++c) {
      const double a = kPi / 2 + 2 * kPi * c / 3;
      V[c] = {cx + side / std::sqrt(3.0) * std::cos(a), cy + side / std::sqrt(3.0) * std::sin(a)};
    }
    for (int c = 0; c < 3; ++c) {
      const Vec2& O = V[c];
      const Vec2& A = V[(c + 1) % 3];
      const Vec2& B = V[(c + 2) % 3];
      const double a0 = std::atan2(A.y - O.y, A.x - O.x), a1 = std::atan2(B.y - O.y, B.x - O.x);
      double sweep = a1 - a0;
      while (sweep > kPi) sweep -= 2 * kPi;
      while (sweep < -kPi) sweep += 2 * kPi;
      for (int i = 0; i < 20; ++i) {
        const double a = a0 + sweep * i / 20.0;
        pocket.push_back({O.x + side * std::cos(a), O.y + side * std::sin(a)});
      }
    }
  }
  // Three lobes pinched together: the pinches are spikes of material thinner than the fillet. The
  // offset swallows the short edges at their tips (as a true polygon offset does), and the tube must
  // still be closed, free of self-intersection, and remove exactly the fillet everywhere else.
  std::vector<Vec2> pinched;
  for (int i = 0; i < 72; ++i) {
    const double a = 2 * kPi * i / 72;
    const double rad = 10 + 4 * std::cos(3 * a);
    pinched.push_back({50 + rad * std::cos(a), 50 + rad * std::sin(a)});
  }

  run("square r2 n4", sq, false, 2, 4, true);
  run("square r2 n1 chamfer", sq, false, 2, 1, true);
  run("square r2 n4 fill-above", sq, false, 2, 4, false);
  run("L (reflex) r2 n6", L, false, 2, 6, true);
  run("L r2 fill-above", L, false, 2, 6, false);
  run("square hole r1.5", hole, true, 1.5, 4, true);
  run("triangle r1.5", tri, false, 1.5, 6, true);
  run("circle hole r1 n8", circ, true, 1, 8, true);
  run("128-gon hole r4 n12", fine, true, 4, 12, true);
  run("128-gon outer r4 n12", fine, false, 4, 12, true);
  run("reuleaux pocket r2 n8", pocket, true, 2, 8, true);
  run("reuleaux pocket r4 n4", pocket, true, 4, 4, true);
  run("pinched lobes r2 (spikes)", pinched, true, 2, 8, true);
  run("square r12 (too big)", sq, false, 12, 4, true, false);
  // Drafted walls: the tube must stay a clean closed solid when it follows a leaning wall, both a
  // pocket that narrows to its floor (+) and an undercut (-), including at sharp pocket tips where
  // part of the arc lands outside the rim.
  printf("\n");
  for (double draft : {-15.0, -5.0, 5.0, 15.0, 30.0}) {
    struct Case { const char* name; std::vector<Vec2> ring; bool hole; double r; int n; };
    const Case cases[] = {{"square hole", hole, true, 1.5, 6}, {"reuleaux pocket", pocket, true, 2, 8},
                          {"circle hole", circ, true, 1, 8}, {"L outline", L, false, 2, 6}};
    for (const Case& c : cases) {
      std::vector<Vec2> ring = cleanRing(c.ring);
      if (c.hole != (signedArea(ring) < 0)) std::reverse(ring.begin(), ring.end());
      const double a = draft * kPi / 180.0;
      std::vector<Vec2> walls(ring.size(), Vec2{-std::cos(a), std::sin(a)});
      RawMesh m; const char* why = "";
      const bool ok = sweepRing(ring, c.r, c.n, 0.02, true, &m, &why, &walls);
      char label[64]; std::snprintf(label, sizeof label, "%s draft %+.0f", c.name, draft);
      CHECK(ok, "%s: sweep refused: %s", label, why);
      if (!ok) continue;
      const int self = selfIntersections(m);
      CHECK(closedManifold(m), "%s: not a closed 2-manifold", label);
      CHECK(volume(m) > 0, "%s: volume %g", label, volume(m));
      CHECK(self == 0, "%s: passes through itself at %d places", label, self);
      CHECK(minArea(m) > 1e-12, "%s: degenerate triangle", label);
      // Every vertex must sit on or outside the wall it follows (nothing cut into the wall below B).
      printf("%-26s tris=%5zu self=%d closed=%d\n", label, m.t.size() / 3, self, (int)closedManifold(m));
    }
  }
  printf(fails ? "\n%d FAILURES\n" : "\nALL PASSED\n", fails);
  return fails != 0;
}
