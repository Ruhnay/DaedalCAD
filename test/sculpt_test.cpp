// Sculpting: start shapes and remeshes are closed, even and the right size; every brush keeps the
// mesh closed and never pushes it through itself; mirror, undo and redo work.

#include "sculpt.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using namespace sculpt;
namespace la = manifold::la;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

std::string num(double v) {
  char text[64];
  std::snprintf(text, sizeof text, "%.4g", v);
  return text;
}

bool sound(const Mesh& m, const std::string& what) {
  const bool closed = closedManifold(m);
  const bool crossing = selfIntersects(m);
  check(closed, what + ": closed");
  check(!crossing, what + ": does not pass through itself");
  return closed && !crossing;
}

double maxAlong(const Mesh& m, const vec3& dir) {
  double best = -1e300;
  for (const vec3& p : m.pos) best = std::max(best, la::dot(p, dir));
  return best;
}

// A stroke of `dabs` dabs straight down onto the surface at (x, y), from above.
void strokeAt(Sculptor& s, Stroke stroke, double x, double y, int dabs) {
  vec3 hit;
  if (!s.raycast(vec3(x, y, 1000), vec3(0, 0, -1), &hit)) {
    check(false, "the stroke hits the surface");
    return;
  }
  s.beginStroke(stroke, hit, vec3(0, 0, -1));
  for (int i = 1; i < dabs; ++i) {
    if (s.raycast(vec3(x, y, 1000), vec3(0, 0, -1), &hit)) s.dab(hit);
  }
  s.endStroke();
}

}  // namespace

int main() {
  const double pi = 3.14159265358979;
  const auto started = std::chrono::steady_clock::now();

  // Start shapes.
  struct Want {
    const char* kind;
    double volume;
  };
  const double S = 40;
  const Want wants[] = {{"ball", 4.0 / 3 * pi * 20 * 20 * 20},
                        {"cube", S * S * S},
                        {"cylinder", pi * 20 * 20 * S},
                        {"slab", S * S * 5},
                        {"capsule", pi * 10 * 10 * (S - 20) + 4.0 / 3 * pi * 1000}};
  for (const Want& want : wants) {
    Mesh m;
    std::string error;
    const bool ok = startShape(want.kind, S, 1.0, &m, &error);
    check(ok, std::string(want.kind) + ": made (" + error + ")");
    if (!ok) continue;
    sound(m, want.kind);
    const double v = volume(m);
    // Rounded edges from the voxel remesh take a little off the boxes.
    check(std::abs(v - want.volume) < want.volume * 0.04, std::string(want.kind) + ": volume " + num(v) + " vs " + num(want.volume));
    const double edge = averageEdge(m);
    check(edge > 0.5 && edge < 1.6, std::string(want.kind) + ": edges about 1 mm (" + num(edge) + ")");
  }

  Mesh ball;
  std::string error;
  startShape("ball", 40, 1.0, &ball, &error);
  const double ballVolume = volume(ball);

  // Raycast from above lands on the top of the ball.
  Sculptor s;
  s.setMesh(ball);
  {
    vec3 hit, normal;
    const bool ok = s.raycast(vec3(0, 0, 100), vec3(0, 0, -1), &hit, &normal);
    check(ok && std::abs(hit.z - 20) < 0.1 && normal.z > 0.95, "raycast hits the top of the ball (" + num(hit.z) + ")");
    check(!s.raycast(vec3(50, 50, 100), vec3(0, 0, -1), &hit), "raycast beside the ball misses");
  }

  // Draw raises a hill; Ctrl (invert) digs; both stay sound.
  Stroke draw;
  draw.brush = Brush::Draw;
  draw.radius = 6;
  draw.strength = 0.6;
  strokeAt(s, draw, 0, 0, 30);
  check(maxAlong(s.mesh(), vec3(0, 0, 1)) > 23, "Draw raised a hill (" + num(maxAlong(s.mesh(), vec3(0, 0, 1))) + ")");
  check(volume(s.mesh()) > ballVolume, "Draw added volume");
  sound(s.mesh(), "after Draw");
  check(s.canUndo(), "a stroke can be undone");
  check(s.undo(), "undo");
  {
    bool same = true;
    for (size_t i = 0; i < ball.pos.size(); ++i) same = same && s.mesh().pos[i] == ball.pos[i];
    check(same, "undo puts every point back exactly");
  }
  check(s.redo() && maxAlong(s.mesh(), vec3(0, 0, 1)) > 23, "redo brings the hill back");
  s.undo();

  Stroke dig = draw;
  dig.invert = true;
  strokeAt(s, dig, 0, 0, 30);
  {
    vec3 bottom;
    s.raycast(vec3(0, 0, 100), vec3(0, 0, -1), &bottom);
    check(bottom.z < 17, "Draw with Ctrl dug a hole (" + num(bottom.z) + ")");
  }
  check(volume(s.mesh()) < ballVolume, "digging took volume away");
  sound(s.mesh(), "after digging");
  s.undo();

  // Mirror: a dab at +X makes the same bump at -X.
  {
    Stroke mirrored = draw;
    mirrored.mirror[0] = true;
    vec3 hit;
    s.raycast(vec3(100, 0, 0), vec3(-1, 0, 0), &hit);
    s.beginStroke(mirrored, hit, vec3(-1, 0, 0));
    for (int i = 0; i < 20; ++i)
      if (s.raycast(vec3(100, 0, 0), vec3(-1, 0, 0), &hit)) s.dab(hit);
    s.endStroke();
    const double plus = maxAlong(s.mesh(), vec3(1, 0, 0)), minus = maxAlong(s.mesh(), vec3(-1, 0, 0));
    check(plus > 22 && std::abs(plus - minus) < 0.05, "mirror X: the bump at +X (" + num(plus) + ") is at -X too (" + num(minus) + ")");
    sound(s.mesh(), "after a mirrored stroke");
    s.undo();
  }

  // Every other brush keeps it sound and does what it says.
  {
    Stroke clay = draw;
    clay.brush = Brush::Clay;
    strokeAt(s, clay, 0, 0, 30);
    check(volume(s.mesh()) > ballVolume, "Clay builds up");
    sound(s.mesh(), "after Clay");
    s.undo();

    Stroke inflate = draw;
    inflate.brush = Brush::Inflate;
    strokeAt(s, inflate, 0, 0, 30);
    check(volume(s.mesh()) > ballVolume, "Inflate puffs up");
    sound(s.mesh(), "after Inflate");
    s.undo();

    // Smooth takes a hill down again.
    strokeAt(s, draw, 0, 0, 30);
    const double hill = maxAlong(s.mesh(), vec3(0, 0, 1));
    Stroke smooth = draw;
    smooth.brush = Brush::Smooth;
    smooth.radius = 9;
    smooth.strength = 1;
    strokeAt(s, smooth, 0, 0, 30);
    const double after = maxAlong(s.mesh(), vec3(0, 0, 1));
    check(after < hill - 0.5, "Smooth flattens a hill (" + num(hill) + " -> " + num(after) + ")");
    sound(s.mesh(), "after Smooth");
    s.undo();
    s.undo();

    // Flatten makes the top of the ball flatter: the very top comes down.
    Stroke flatten = draw;
    flatten.brush = Brush::Flatten;
    flatten.radius = 10;
    flatten.strength = 1;
    strokeAt(s, flatten, 0, 0, 30);
    check(maxAlong(s.mesh(), vec3(0, 0, 1)) < 19.9, "Flatten flattens the top");
    sound(s.mesh(), "after Flatten");
    s.undo();

    Stroke pinch = draw;
    pinch.brush = Brush::Pinch;
    pinch.strength = 1;
    strokeAt(s, pinch, 0, 0, 30);
    sound(s.mesh(), "after Pinch");
    s.undo();
  }

  // Grab pulls a lump up with the cursor; undo puts it back.
  {
    Stroke grab = draw;
    grab.brush = Brush::Grab;
    grab.radius = 8;
    vec3 hit;
    s.raycast(vec3(0, 0, 100), vec3(0, 0, -1), &hit);
    s.beginStroke(grab, hit, vec3(1, 0, 0));
    for (int i = 1; i <= 10; ++i) s.grabTo(hit + vec3(0, 0, i));
    s.endStroke();
    check(std::abs(maxAlong(s.mesh(), vec3(0, 0, 1)) - 30) < 0.01, "Grab lifted the top by 10 mm");
    sound(s.mesh(), "after Grab");
    s.undo();
    check(std::abs(maxAlong(s.mesh(), vec3(0, 0, 1)) - 20) < 0.01, "undo puts the grabbed lump back");
  }

  // The new brushes: each does what it says, and keeps the mesh sound.
  {
    const double top0 = maxAlong(s.mesh(), vec3(0, 0, 1));
    auto topHit = [&]() {
      vec3 h;
      s.raycast(vec3(0, 0, 100), vec3(0, 0, -1), &h);
      return h.z;
    };

    // Layer: up to a set height and no further, however long the stroke.
    Stroke layer = draw;
    layer.brush = Brush::Layer;
    layer.radius = 6;
    layer.strength = 0.5;  // height 6 * 0.3 * 0.5 = 0.9 mm
    strokeAt(s, layer, 0, 0, 10);
    const double after10 = topHit();
    s.undo();
    strokeAt(s, layer, 0, 0, 60);
    const double after60 = topHit();
    check(after60 > top0 + 0.8 && after60 < top0 + 0.95, "Layer rises 0.9 mm (" + num(after60 - top0) + ")");
    check(std::abs(after60 - after10) < 0.05, "Layer stops at its height (" + num(after10 - top0) + " after 10 dabs, " +
                                                 num(after60 - top0) + " after 60)");
    sound(s.mesh(), "after Layer");
    s.undo();

    Stroke blob = draw;
    blob.brush = Brush::Blob;
    strokeAt(s, blob, 0, 0, 30);
    check(volume(s.mesh()) > ballVolume && topHit() > top0 + 1, "Blob bulges out");
    sound(s.mesh(), "after Blob");
    s.undo();

    Stroke crease = draw;
    crease.brush = Brush::Crease;
    strokeAt(s, crease, 0, 0, 30);
    check(topHit() < top0 - 0.5, "Crease cuts in (" + num(topHit() - top0) + ")");
    sound(s.mesh(), "after Crease");
    s.undo();
    crease.invert = true;
    strokeAt(s, crease, 0, 0, 30);
    check(topHit() > top0 + 0.5, "Crease with Ctrl makes a ridge");
    sound(s.mesh(), "after a Crease ridge");
    s.undo();

    // Fill lifts a dent back up; Scrape shaves a bump down.
    strokeAt(s, dig, 0, 0, 20);
    const double dent = topHit();
    Stroke fill = draw;
    fill.brush = Brush::Fill;
    fill.radius = 9;
    fill.strength = 1;
    strokeAt(s, fill, 0, 0, 30);
    check(topHit() > dent + 0.3, "Fill lifts a dent (" + num(dent) + " -> " + num(topHit()) + ")");
    sound(s.mesh(), "after Fill");
    s.undo();
    s.undo();
    strokeAt(s, draw, 0, 0, 20);
    const double bump = topHit();
    Stroke scrape = fill;
    scrape.brush = Brush::Scrape;
    strokeAt(s, scrape, 0, 0, 30);
    check(topHit() < bump - 0.3, "Scrape shaves a bump (" + num(bump) + " -> " + num(topHit()) + ")");
    sound(s.mesh(), "after Scrape");
    s.undo();
    s.undo();

    // Snake hook pulls a horn far out, much further than the brush is wide.
    {
      Stroke hook = draw;
      hook.brush = Brush::SnakeHook;
      hook.radius = 4;
      hook.strength = 1;
      vec3 hit;
      s.raycast(vec3(0, 0, 100), vec3(0, 0, -1), &hit);
      s.beginStroke(hook, hit, vec3(1, 0, 0));
      for (int i = 1; i <= 30; ++i) s.grabTo(hit + vec3(0, 0, i * 0.5));
      s.endStroke();
      check(maxAlong(s.mesh(), vec3(0, 0, 1)) > 20 + 8, "Snake hook pulled a horn out (" + num(maxAlong(s.mesh(), vec3(0, 0, 1)) - 20) + " mm)");
      sound(s.mesh(), "after Snake hook");
      s.undo();
      check(std::abs(maxAlong(s.mesh(), vec3(0, 0, 1)) - 20) < 0.01, "undo takes the horn back");
    }

    // Nudge moves the surface along with the stroke.
    {
      Stroke nudge = draw;
      nudge.brush = Brush::Nudge;
      nudge.strength = 1;
      strokeAt(s, draw, 0, 0, 20);  // a hill to push
      auto hillX = [&] {
        double best = -1, x = 0;
        for (const vec3& p : s.mesh().pos)
          if (p.z > best) best = p.z, x = p.x;
        return x;
      };
      const double before = hillX();
      vec3 hit;
      s.raycast(vec3(-3, 0, 100), vec3(0, 0, -1), &hit);
      s.beginStroke(nudge, hit, vec3(0, 0, -1));
      for (int i = 1; i <= 12; ++i)
        if (s.raycast(vec3(-3 + i * 0.5, 0, 100), vec3(0, 0, -1), &hit)) s.dab(hit);
      s.endStroke();
      check(hillX() > before + 0.3, "Nudge pushed the hill along (" + num(before) + " -> " + num(hillX()) + ")");
      sound(s.mesh(), "after Nudge");
      s.undo();
      s.undo();
    }

    // Rotate turns a spot about the view line; with mirror X the other side turns the other way.
    {
      strokeAt(s, draw, 6, 0, 20);  // an off-centre bump on top, to see turning
      Stroke turn = draw;
      turn.brush = Brush::Rotate;
      turn.radius = 12;
      turn.falloff = Falloff::Flat;
      vec3 hit;
      s.raycast(vec3(0, 0, 100), vec3(0, 0, -1), &hit);
      s.beginStroke(turn, hit, vec3(0, 0, -1));
      for (int i = 0; i <= 9; ++i) {
        const double a = i * 5 * 3.14159265 / 180;
        s.grabTo(hit + vec3(std::cos(a), std::sin(a), 0) * 5.0);
      }
      s.endStroke();
      double best = -1;
      vec3 peak;
      for (const vec3& p : s.mesh().pos)
        if (p.z > best) best = p.z, peak = p;
      const double angle = std::atan2(peak.y, peak.x) * 180 / 3.14159265;
      check(angle > 38 && angle < 52, "Rotate turned the bump about 45 degrees (" + num(angle) + ")");
      sound(s.mesh(), "after Rotate");
      s.undo();
      s.undo();
    }
    check(std::abs(volume(s.mesh()) - ballVolume) < 1e-6 * ballVolume, "all undone: the ball is back");
  }

  // Radial symmetry: a dab on the side repeats 6 times around Z.
  {
    Stroke six = draw;
    six.radial = 6;
    six.radialAxis = 2;
    check(symmetries(six).size() == 6, "radial 6: six copies");
    Stroke both = six;
    both.mirror[0] = true;
    check(symmetries(both).size() == 12, "radial 6 with mirror X: twelve copies");
    vec3 hit;
    s.raycast(vec3(100, 0, 0), vec3(-1, 0, 0), &hit);
    s.beginStroke(six, hit, vec3(-1, 0, 0));
    for (int i = 0; i < 20; ++i)
      if (s.raycast(vec3(100, 0, 0), vec3(-1, 0, 0), &hit)) s.dab(hit);
    s.endStroke();
    double lo = 1e9, hi = -1e9;
    for (int k = 0; k < 6; ++k) {
      const double a = k * 3.14159265358979 / 3;
      const double reach = maxAlong(s.mesh(), vec3(std::cos(a), std::sin(a), 0));
      lo = std::min(lo, reach);
      hi = std::max(hi, reach);
    }
    check(lo > 22 && hi - lo < 0.1, "radial: six equal bumps (" + num(lo) + " .. " + num(hi) + ")");
    check(maxAlong(s.mesh(), vec3(std::cos(0.5236), std::sin(0.5236), 0)) < lo - 0.5, "radial: nothing between them");
    sound(s.mesh(), "after a radial stroke");
    s.undo();
  }

  // Mask: a masked spot does not move; inverted, only it moves; undo and remesh keep it.
  {
    auto topHit = [&]() {
      vec3 h;
      s.raycast(vec3(0, 0, 100), vec3(0, 0, -1), &h);
      return h.z;
    };
    Stroke mask = draw;
    mask.brush = Brush::Mask;
    mask.radius = 8;
    mask.strength = 1;
    mask.falloff = Falloff::Flat;
    strokeAt(s, mask, 0, 0, 10);
    check(s.hasMask(), "the mask brush paints a mask");
    strokeAt(s, draw, 0, 0, 30);
    check(std::abs(topHit() - 20) < 0.02, "a masked spot stays put under Draw (" + num(topHit() - 20) + ")");
    check(!s.canRedo(), "a stroke that moved nothing leaves no undo step");
    s.invertMask();
    strokeAt(s, draw, 0, 0, 30);
    check(topHit() > 21, "inverted: the spot moves now");
    {
      vec3 h;
      s.raycast(vec3(100, 0, 0), vec3(-1, 0, 0), &h);
      s.beginStroke(draw, h, vec3(-1, 0, 0));
      for (int i = 0; i < 20; ++i)
        if (s.raycast(vec3(100, 0, 0), vec3(-1, 0, 0), &h)) s.dab(h);
      s.endStroke();
      check(maxAlong(s.mesh(), vec3(1, 0, 0)) < 20.02, "inverted: the rest is held");
    }
    s.undo();  // the Draw on top
    s.undo();  // the invert
    check(s.hasMask() && std::abs(topHit() - 20) < 1e-9, "undo brings the first mask back");
    std::string why;
    check(s.remeshTo(0.9, &why), "remesh with a mask");
    double topMask = 0, bottomMask = 1;
    for (size_t v = 0; v < s.mesh().pos.size(); ++v) {
      if (s.mesh().pos[v].z > 19.5) topMask = std::max(topMask, double(s.mask()[v]));
      if (s.mesh().pos[v].z < -19.5) bottomMask = std::min(bottomMask, double(1 - s.mask()[v]));
    }
    check(topMask > 0.9 && bottomMask > 0.99, "remesh carries the mask over (" + num(topMask) + ")");
    s.undo();
    s.undo();
    check(!s.hasMask(), "all the mask steps undo");
  }

  // Stamp: a disc picture raises a round plateau as high as the picture says, and only under it.
  {
    auto disc = std::make_shared<StampImage>();
    disc->width = disc->height = 64;
    disc->value.resize(64 * 64);
    for (int y = 0; y < 64; ++y)
      for (int x = 0; x < 64; ++x)
        disc->value[y * 64 + x] = std::hypot(x - 31.5, y - 31.5) < 16 ? 1.0f : 0.0f;
    Stroke stamp = draw;
    stamp.brush = Brush::Stamp;
    stamp.radius = 8;
    stamp.strength = 1;  // height 8 * 0.5 = 4 mm
    stamp.stamp = disc;
    stamp.up = vec3(0, 1, 0);
    vec3 hit;
    s.raycast(vec3(0, 0, 100), vec3(0, 0, -1), &hit);
    s.beginStroke(stamp, hit, vec3(0, 0, -1));
    s.dab(hit);  // a second dab at the same spot: no higher
    s.endStroke();
    vec3 top, beside;
    s.raycast(vec3(0, 0, 100), vec3(0, 0, -1), &top);
    s.raycast(vec3(6, 0, 100), vec3(0, 0, -1), &beside);
    check(std::abs(top.z - 24) < 0.5, "Stamp raised the disc 4 mm (" + num(top.z - 20) + ")");
    check(beside.z < std::sqrt(400.0 - 36) + 0.05, "outside the picture nothing rises");
    sound(s.mesh(), "after Stamp");
    s.undo();
    stamp.invert = true;
    s.raycast(vec3(0, 0, 100), vec3(0, 0, -1), &hit);
    s.beginStroke(stamp, hit, vec3(0, 0, -1));
    for (int i = 0; i < 4; ++i) s.dab(hit);  // the guard may take a deep press in steps
    s.endStroke();
    s.raycast(vec3(0, 0, 100), vec3(0, 0, -1), &top);
    check(top.z < 17, "Stamp with Ctrl presses it in (" + num(top.z - 20) + ")");
    sound(s.mesh(), "after a pressed stamp");
    s.undo();
  }

  // The guard: digging hard into a thin slab never goes through the floor.
  {
    Mesh slab;
    startShape("slab", 40, 0.8, &slab, &error);  // 5 mm thick
    Sculptor thin;
    thin.setMesh(slab);
    Stroke hard;
    hard.brush = Brush::Draw;
    hard.invert = true;
    hard.radius = 5;
    hard.strength = 1;
    for (int i = 0; i < 6; ++i) strokeAt(thin, hard, 0, 0, 40);
    vec3 lo, hi;
    bounds(thin.mesh(), &lo, &hi);
    sound(thin.mesh(), "a slab dug into hard");
    check(thin.heldBack() > 0, "the guard held the brush back (" + std::to_string(thin.heldBack()) + " dabs)");
    // And pulling one side hard through the other with Grab is stopped too.
    Stroke grab;
    grab.brush = Brush::Grab;
    grab.radius = 6;
    vec3 hit;
    thin.raycast(vec3(10, 0, 100), vec3(0, 0, -1), &hit);
    thin.beginStroke(grab, hit, vec3(0, 1, 0));
    for (int i = 1; i <= 20; ++i) thin.grabTo(hit + vec3(0, 0, -i));
    thin.endStroke();
    sound(thin.mesh(), "a slab top grabbed down through its floor");
  }

  // Remesh: of a sculpted ball, even again, closed, same volume.
  {
    strokeAt(s, draw, 0, 0, 30);
    Stroke grab = draw;
    grab.brush = Brush::Grab;
    grab.radius = 8;
    vec3 hit;
    s.raycast(vec3(100, 0, 0), vec3(-1, 0, 0), &hit);
    s.beginStroke(grab, hit, vec3(0, 0, -1));
    s.grabTo(hit + vec3(15, 0, 0));
    s.endStroke();
    const double before = volume(s.mesh());
    std::string why;
    check(s.remeshTo(0.8, &why), "remesh (" + why + ")");
    sound(s.mesh(), "remeshed");
    const double after = volume(s.mesh());
    check(std::abs(after - before) < before * 0.02, "remesh keeps the volume (" + num(before) + " -> " + num(after) + ")");
    const double edge = s.averageEdge();
    check(edge > 0.45 && edge < 1.2, "remesh makes edges about 0.8 mm (" + num(edge) + ")");
    check(maxAlong(s.mesh(), vec3(1, 0, 0)) > 33, "the grabbed spike survives the remesh");
    check(s.undo() && std::abs(volume(s.mesh()) - before) < 1e-9, "a remesh can be undone");
  }

  // Remesh joins parts that overlap (two balls in one mesh, one through the other).
  {
    Mesh a, b;
    startShape("ball", 20, 1.0, &a, &error);
    startShape("ball", 20, 1.0, &b, &error);
    Mesh both = a;
    const uint32_t offset = static_cast<uint32_t>(both.pos.size());
    for (const vec3& p : b.pos) both.pos.push_back(p + vec3(10, 0, 0));
    for (const Tri& t : b.tri) both.tri.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
    Mesh joined;
    check(remesh(both, 0.7, &joined, &error), "remesh two overlapping balls");
    sound(joined, "two balls joined");
    // Two r=10 balls 10 apart: each 4188.8, the lens they share 2 * (pi/12)(4r+d)(2r-d)^2 = 1308.99.
    const double want = 2 * 4188.79 - 1308.997;
    check(std::abs(volume(joined) - want) < want * 0.03, "joined volume " + num(volume(joined)) + " vs " + num(want));
  }

  // Saved and read back: the same closed mesh, point for point (to float precision).
  {
    Mesh dented;
    startShape("ball", 30, 1.0, &dented, &error);
    Sculptor dent;
    dent.setMesh(dented);
    strokeAt(dent, draw, 0, 0, 20);
    const std::string path = "sculpt_test_roundtrip.stl";
    check(writeStl(dent.mesh(), path), "writeStl");
    Mesh back;
    check(readStl(path, &back, &error), "readStl (" + error + ")");
    check(back.tri.size() == dent.mesh().tri.size() && back.pos.size() == dent.mesh().pos.size(),
          "read back with as many points and triangles");
    check(closedManifold(back), "read back closed");
    check(std::abs(volume(back) - volume(dent.mesh())) < 1e-3 * volume(dent.mesh()), "read back with the same volume");
    std::remove(path.c_str());
  }

  // Too fine is refused, not attempted.
  {
    Mesh out;
    std::string why;
    check(!remesh(ball, 0.01, &out, &why) && !why.empty(), "a far too fine remesh is refused (" + why + ")");
  }

  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("sculpt checks took %.1f s\n", seconds);
  std::printf(failures ? "%d failure(s)\n" : "all sculpt checks passed\n", failures);
  return failures ? 1 : 0;
}
