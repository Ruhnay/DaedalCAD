#include "surfacefillet.h"

#include "fillet_geom.h"

#include <manifold/cross_section.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <unordered_map>
#include <utility>

using manifold::CrossSection;
using manifold::Manifold;
using manifold::Polygons;
using manifold::SimplePolygon;
using manifold::vec2;
using manifold::vec3;

namespace sf {
namespace {

struct Axes {
  vec3 u;
  vec3 v;
  vec3 n;
  vec3 o;
};

vec3 sub3(const vec3& a, const vec3& b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
double dot3(const vec3& a, const vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
vec3 cross3(const vec3& a, const vec3& b) {
  return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
vec3 unit3(const vec3& a, const vec3& fallback) {
  const double len = std::sqrt(dot3(a, a));
  return len < 1e-12 ? fallback : vec3(a.x / len, a.y / len, a.z / len);
}

Axes axesOf(const Frame& frame) {
  Axes ax;
  ax.n = unit3(vec3(frame.normal[0], frame.normal[1], frame.normal[2]), vec3(0, 0, 1));
  vec3 u(frame.axis[0], frame.axis[1], frame.axis[2]);
  u = sub3(u, vec3(ax.n.x * dot3(u, ax.n), ax.n.y * dot3(u, ax.n), ax.n.z * dot3(u, ax.n)));
  if (dot3(u, u) < 1e-8) {
    u = std::abs(ax.n.z) < 0.9 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    u = sub3(u, vec3(ax.n.x * dot3(u, ax.n), ax.n.y * dot3(u, ax.n), ax.n.z * dot3(u, ax.n)));
  }
  ax.u = unit3(u, vec3(1, 0, 0));
  ax.v = unit3(cross3(ax.n, ax.u), vec3(0, 1, 0));  // u x v = n, so the frame is right handed
  ax.o = vec3(frame.origin[0], frame.origin[1], frame.origin[2]);
  return ax;
}

// Everything here goes through Manifold's double-precision mesh (MeshGL64). The solids the app builds
// live in double precision; reading them back or building cutters in float put every cutter point up
// to a float step (about a micron at part scale) off the surface it was meant to touch. A fillet
// meets the wall and the face tangentially, so those near-misses ran the full length of every
// tangent line and the boolean shredded them into hairline slivers - visible as dotted seams, and
// different from hole to hole depending on how each one's vertices happened to round.
Manifold fromRaw(const fg::RawMesh& raw) {
  if (raw.v.size() < 12 || raw.t.size() < 12) return Manifold();
  manifold::MeshGL64 mesh;
  mesh.numProp = 3;
  mesh.vertProperties = raw.v;
  mesh.triVerts.assign(raw.t.begin(), raw.t.end());
  Manifold solid(mesh);
  if (solid.IsEmpty() || solid.Status() != Manifold::Error::NoError) return Manifold();
  return solid;
}

bool good(const Manifold& solid) { return !solid.IsEmpty() && solid.Status() == Manifold::Error::NoError; }

Manifold unionOf(std::vector<Manifold> parts) {
  if (parts.empty()) return Manifold();
  if (parts.size() == 1) return std::move(parts.front());
  return Manifold::BatchBoolean(parts, manifold::OpType::Add);
}

SimplePolygon toPolygon(const std::vector<fg::Vec2>& ring, bool counterClockwise) {
  SimplePolygon poly;
  poly.reserve(ring.size());
  for (const fg::Vec2& p : ring) poly.push_back(vec2(p.x, p.y));
  const bool isCcw = fg::signedArea(ring) > 0;
  if (isCcw != counterClockwise) std::reverse(poly.begin(), poly.end());
  return poly;
}

Manifold mapToWorld(const Manifold& local, const Axes& ax) {
  const auto gl = local.GetMeshGL64();
  const size_t stride = std::max<size_t>(3, gl.numProp);
  const size_t count = gl.vertProperties.size() / stride;
  manifold::MeshGL64 out;
  out.numProp = 3;
  out.vertProperties.reserve(count * 3);
  for (size_t i = 0; i < count; ++i) {
    const double x = gl.vertProperties[i * stride + 0];
    const double y = gl.vertProperties[i * stride + 1];
    const double z = gl.vertProperties[i * stride + 2];
    out.vertProperties.push_back(ax.o.x + ax.u.x * x + ax.v.x * y + ax.n.x * z);
    out.vertProperties.push_back(ax.o.y + ax.u.y * x + ax.v.y * y + ax.n.y * z);
    out.vertProperties.push_back(ax.o.z + ax.u.z * x + ax.v.z * y + ax.n.z * z);
  }
  out.triVerts = gl.triVerts;
  out.mergeFromVert = gl.mergeFromVert;
  out.mergeToVert = gl.mergeToVert;
  Manifold solid(out);
  return good(solid) ? solid : Manifold();
}

void say(std::string* log, const std::string& text) {
  if (!log) return;
  log->append(text);
  log->push_back('\n');
}

}  // namespace

Manifold buildCutter(const std::vector<double>& ringWorld, const Frame& frame, double radius, int segments, bool below,
                     std::string* warning, std::string* log, const std::vector<double>& wallsWorld) {
  const Axes ax = axesOf(frame);
  std::vector<fg::Vec2> ring;
  std::vector<vec3> ring3;     // the same points, exactly as the solid has them
  std::vector<vec3> wall3;
  const bool haveWalls = wallsWorld.size() == ringWorld.size();
  double offPlane = 0;
  for (size_t i = 0; i + 2 < ringWorld.size(); i += 3) {
    if (std::abs(ringWorld[i]) > 1e19) continue;
    const vec3 world(ringWorld[i], ringWorld[i + 1], ringWorld[i + 2]);
    const vec3 d = sub3(world, ax.o);
    ring.push_back({dot3(d, ax.u), dot3(d, ax.v)});
    ring3.push_back(world);
    if (haveWalls) wall3.push_back(vec3(wallsWorld[i], wallsWorld[i + 1], wallsWorld[i + 2]));
    offPlane = std::max(offPlane, std::abs(dot3(d, ax.n)));
  }
  if (offPlane > 0.1) {
    if (warning) *warning = "That face is curved. Surface fillet only works on flat faces.";
    return Manifold();
  }
  // Each wall normal, seen in its edge's cross-section: (into the face, up from the face).
  std::vector<fg::Vec2> walls;
  if (haveWalls && wall3.size() == ring.size()) {
    for (size_t i = 0; i < ring.size(); ++i) {
      const fg::Vec2& a = ring[i];
      const fg::Vec2& b = ring[(i + 1) % ring.size()];
      const double len = std::hypot(b.x - a.x, b.y - a.y);
      if (len < 1e-12) { walls.push_back({-1, 0}); continue; }
      const fg::Vec2 left{-(b.y - a.y) / len, (b.x - a.x) / len};
      const vec3 into = vec3(ax.u.x * left.x + ax.v.x * left.y, ax.u.y * left.x + ax.v.y * left.y,
                             ax.u.z * left.x + ax.v.z * left.y);
      walls.push_back({dot3(wall3[i], into), dot3(wall3[i], ax.n)});
    }
  }
  std::vector<size_t> keep(ring.size());
  std::iota(keep.begin(), keep.end(), size_t{0});
  fg::cleanRingWithEdges(ring, walls, &keep);
  {
    std::vector<vec3> kept;
    kept.reserve(keep.size());
    for (size_t k : keep) kept.push_back(ring3[k]);
    ring3 = std::move(kept);
  }
  if (ring.size() < 3) {
    if (warning) *warning = "That face outline is too small to fillet.";
    return Manifold();
  }
  const double area = fg::signedArea(ring);
  if (std::abs(area) < 1e-6) {
    if (warning) *warning = "That face outline has no area.";
    return Manifold();
  }
  const bool isHole = area < 0;
  const double r = std::max(radius, 0.05);
  const int n = std::clamp(segments, 1, 20);
  // Both offsets scale with how far the part sits from the origin, because that is what sets the
  // float32 spacing the mesh is stored at. They stay hundreds of times coarser than that noise and
  // tens of times finer than a printed layer. `over` used to be 0.08 mm, sized to absorb the face
  // picker's vertex weld; the outlines are exact now, and that slack was cutting a visible ledge
  // into any wall that is not vertical.
  double far = 1.0;
  for (const fg::Vec2& p : ring) far = std::max(far, std::max(std::abs(p.x), std::abs(p.y)));
  far = std::max(far, std::sqrt(dot3(ax.o, ax.o)));
  const double over = std::max(0.02, far * 2e-5);

  // First choice: one continuous tube around the whole outline. No pieces overlap, so no boolean
  // is needed to build it and nothing can leave seams or slivers between neighbouring segments.
  {
    fg::RawMesh tube;
    const char* why = "";
    if (fg::sweepRing(ring, r, n, over, below, &tube, &why, walls.size() == ring.size() ? &walls : nullptr)) {
      // Into world space, each vertex as its outline point's exact position plus its offset from that
      // point. A vertex with no offset (straight below the rim on a square wall, or on the face itself)
      // then sits exactly on the solid's own vertex or plane, so the boolean sees a true coincidence it
      // resolves cleanly instead of a near-miss it shreds.
      const bool owned = tube.owner.size() * 3 == tube.v.size() && ring3.size() == ring.size();
      for (size_t i = 0; i + 2 < tube.v.size(); i += 3) {
        const double x = tube.v[i], y = tube.v[i + 1], z = tube.v[i + 2];
        const int32_t o = owned ? tube.owner[i / 3] : -1;
        if (o >= 0 && static_cast<size_t>(o) < ring.size()) {
          const vec3& base = ring3[static_cast<size_t>(o)];
          const double dx = x - ring[static_cast<size_t>(o)].x, dy = y - ring[static_cast<size_t>(o)].y;
          tube.v[i] = base.x + ax.u.x * dx + ax.v.x * dy + ax.n.x * z;
          tube.v[i + 1] = base.y + ax.u.y * dx + ax.v.y * dy + ax.n.y * z;
          tube.v[i + 2] = base.z + ax.u.z * dx + ax.v.z * dy + ax.n.z * z;
        } else {
          tube.v[i] = ax.o.x + ax.u.x * x + ax.v.x * y + ax.n.x * z;
          tube.v[i + 1] = ax.o.y + ax.u.y * x + ax.v.y * y + ax.n.y * z;
          tube.v[i + 2] = ax.o.z + ax.u.z * x + ax.v.z * y + ax.n.z * z;
        }
      }
      Manifold solid = fromRaw(tube);
      if (good(solid)) {
        double steepest = 0;
        for (const fg::Vec2& w : walls) steepest = std::max(steepest, std::abs(std::atan2(w.y, -w.x)) * 180.0 / fg::kPi);
        say(log, "Outline " + std::string(isHole ? "hole" : "outside") + ", " + std::to_string(ring.size()) +
                     " corners, radius " + std::to_string(r) + ": one smooth sweep, " +
                     std::to_string(tube.t.size() / 3) + " triangles" +
                     (walls.empty() ? std::string(", walls assumed square")
                                    : ", following the walls (steepest draft " + std::to_string(steepest) + " deg)") +
                     ".");
        return solid;
      }
      say(log, "  smooth sweep was not a valid solid, so the piecewise cutter was used.");
    } else {
      say(log, std::string("  smooth sweep not possible (") + why + "), so the piecewise cutter was used.");
    }
    if (warning && warning->empty())
      *warning = "One outline is too tight for a smooth fillet at this radius, so it used the segmented cutter, which can "
                 "show faint seams. A smaller radius keeps it smooth.";
  }

  // Fallback: a strip per edge and a dome per sharp corner, unioned. Only reached when the outline
  // is too tight for a single sweep; it may leave faint seams but never fails to cut.
  const double bite = std::max(0.005, far * 5e-6);
  const fg::Shape shape = fg::crossSection(r, n, over, bite, below);

  double lo[2] = {1e300, 1e300};
  double hi[2] = {-1e300, -1e300};
  for (const fg::Vec2& p : ring) {
    lo[0] = std::min(lo[0], p.x);
    lo[1] = std::min(lo[1], p.y);
    hi[0] = std::max(hi[0], p.x);
    hi[1] = std::max(hi[1], p.y);
  }
  if (!isHole && r > 0.5 * std::min(hi[0] - lo[0], hi[1] - lo[1]) + 1e-6 && warning)
    *warning = "The radius is more than half this face, so the fillet may cut through it. Try a smaller radius.";

  const size_t count = ring.size();
  std::vector<double> turn(count);
  for (size_t i = 0; i < count; ++i)
    turn[i] = fg::turnAngle(ring[(i + count - 1) % count], ring[i], ring[(i + 1) % count]);

  std::vector<Manifold> parts;
  int strips = 0;
  int domes = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t j = (i + 1) % count;
    // Each strip runs on past a shallow reflex corner to meet its neighbour, which closes the wedge
    // between them without a piece. Left open, that wedge stood up as a hairline ridge along every
    // facet of a curved outline.
    Manifold strip = fromRaw(fg::stripMesh(ring[i], ring[j], shape, fg::cornerExtend(turn[i], r, over),
                                           fg::cornerExtend(turn[j], r, over)));
    if (good(strip)) {
      parts.push_back(std::move(strip));
      ++strips;
    }
    if (fg::needsDome(turn[i], r)) {
      Manifold dome = fromRaw(fg::domeMesh(ring[i].x, ring[i].y, shape, 32));
      if (good(dome)) {
        parts.push_back(std::move(dome));
        ++domes;
      }
    }
  }
  say(log, "Outline " + std::string(isHole ? "hole" : "outside") + ", " + std::to_string(count) + " corners, radius " +
               std::to_string(r) + ": " + std::to_string(strips) + " edge pieces, " + std::to_string(domes) +
               " corner pieces.");
  if (parts.empty()) {
    if (warning) *warning = "That face did not produce a fillet cutter.";
    return Manifold();
  }
  Manifold joined = unionOf(std::move(parts));
  if (!good(joined)) {
    if (warning) *warning = "The fillet pieces did not join. Try a different radius.";
    return Manifold();
  }

  // Bound the cutter to the face side of the outline, so a radius wider than a thin feature cannot
  // reach across it and cut something on the far side. The bound is pushed out past the cutter's own
  // outer wall, so trimming never leaves a face sitting on the solid's wall.
  const double clipOut = over * 3.0;
  const int joinSegs = 16;
  const CrossSection ringCs(Polygons{toPolygon(ring, true)});
  const CrossSection moved = ringCs.Offset(isHole ? -clipOut : clipOut, CrossSection::JoinType::Round, 2.0, joinSegs);
  // No usable bound is better than one sitting on the outline: the cutter already stops `over` past
  // the wall on its own, so leaving it untrimmed is safe, while trimming on the outline is not.
  if (moved.IsEmpty()) {
    say(log, "  outline could not be offset, so the cutter was left untrimmed.");
    return mapToWorld(joined, ax);
  }
  CrossSection region;
  if (!isHole) {
    region = moved;
  } else {
    const double m = r + clipOut + 2.0;
    const std::vector<fg::Vec2> box = {{lo[0] - m, lo[1] - m}, {hi[0] + m, lo[1] - m}, {hi[0] + m, hi[1] + m}, {lo[0] - m, hi[1] + m}};
    region = CrossSection(Polygons{toPolygon(box, true)}) - moved;
  }
  const Polygons regionPolys = region.ToPolygons();
  if (regionPolys.empty()) return mapToWorld(joined, ax);
  const double height = 2.0 * (r + 4.0 * over);
  Manifold clip = Manifold::Extrude(regionPolys, height, 0, 0.0, vec2(1, 1)).Translate(vec3(0, 0, -height * 0.5));
  std::vector<Manifold> pair;
  pair.push_back(std::move(joined));
  pair.push_back(std::move(clip));
  Manifold trimmed = Manifold::BatchBoolean(pair, manifold::OpType::Intersect);
  if (!good(trimmed)) trimmed = pair.front();  // clip failed: the untrimmed cutter is still correct inside the face
  return mapToWorld(trimmed, ax);
}

std::vector<FaceRings> coplanarFaces(const Manifold& solid, const Frame& frame, std::string* log) {
  std::vector<FaceRings> faces;
  if (!good(solid)) return faces;
  // Every directed edge of the whole solid, so a face's boundary edge can find the face across it.
  const Axes ax = axesOf(frame);
  const auto gl = solid.GetMeshGL64();
  const size_t stride = std::max<size_t>(3, gl.numProp);
  const size_t vertCount = gl.vertProperties.size() / stride;
  if (vertCount < 4) return faces;

  std::vector<uint32_t> canon(vertCount);
  std::iota(canon.begin(), canon.end(), 0u);
  for (size_t i = 0; i < gl.mergeFromVert.size() && i < gl.mergeToVert.size(); ++i)
    if (gl.mergeFromVert[i] < vertCount) canon[gl.mergeFromVert[i]] = static_cast<uint32_t>(gl.mergeToVert[i]);
  auto pos = [&](uint32_t v) {
    return vec3(gl.vertProperties[v * stride], gl.vertProperties[v * stride + 1], gl.vertProperties[v * stride + 2]);
  };

  std::unordered_map<uint64_t, uint32_t> allEdges;   // directed (a,b) -> triangle, whole solid
  allEdges.reserve(gl.triVerts.size());
  for (size_t t = 0; t + 2 < gl.triVerts.size(); t += 3)
    for (int k = 0; k < 3; ++k) {
      const uint64_t a = canon[gl.triVerts[t + static_cast<size_t>(k)]];
      const uint64_t b = canon[gl.triVerts[t + static_cast<size_t>((k + 1) % 3)]];
      allEdges[(a << 32) | b] = static_cast<uint32_t>(t / 3);
    }
  auto triNormal = [&](uint32_t tri) {
    const vec3 pa = pos(canon[gl.triVerts[tri * 3]]), pb = pos(canon[gl.triVerts[tri * 3 + 1]]),
               pc = pos(canon[gl.triVerts[tri * 3 + 2]]);
    return unit3(cross3(sub3(pb, pa), sub3(pc, pa)), vec3(0, 0, 0));
  };

  const double planeTol = 0.02;
  std::vector<std::array<uint32_t, 3>> tris;
  for (size_t t = 0; t + 2 < gl.triVerts.size(); t += 3) {
    const uint32_t a = static_cast<uint32_t>(gl.triVerts[t]), b = static_cast<uint32_t>(gl.triVerts[t + 1]),
                   c = static_cast<uint32_t>(gl.triVerts[t + 2]);
    const vec3 pa = pos(a), pb = pos(b), pc = pos(c);
    const vec3 nrm = unit3(cross3(sub3(pb, pa), sub3(pc, pa)), vec3(0, 0, 0));
    if (dot3(nrm, ax.n) < 0.9995) continue;
    if (std::abs(dot3(sub3(pa, ax.o), ax.n)) > planeTol || std::abs(dot3(sub3(pb, ax.o), ax.n)) > planeTol ||
        std::abs(dot3(sub3(pc, ax.o), ax.n)) > planeTol)
      continue;
    tris.push_back({canon[a], canon[b], canon[c]});
  }
  if (tris.empty()) return faces;

  // Connected pieces: triangles that share an edge.
  std::vector<int> parent(tris.size());
  std::iota(parent.begin(), parent.end(), 0);
  std::function<int(int)> find = [&](int x) {
    while (parent[static_cast<size_t>(x)] != x) {
      parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
      x = parent[static_cast<size_t>(x)];
    }
    return x;
  };
  std::map<std::pair<uint32_t, uint32_t>, int> directed;  // (a,b) -> triangle
  for (size_t t = 0; t < tris.size(); ++t)
    for (int k = 0; k < 3; ++k) directed[{tris[t][static_cast<size_t>(k)], tris[t][static_cast<size_t>((k + 1) % 3)]}] = static_cast<int>(t);
  for (const auto& entry : directed) {
    const auto twin = directed.find({entry.first.second, entry.first.first});
    if (twin == directed.end()) continue;
    const int a = find(entry.second), b = find(twin->second);
    if (a != b) parent[static_cast<size_t>(a)] = b;
  }

  struct Loop {
    std::vector<uint32_t> verts;
    double area = 0;
    int piece = 0;
  };
  std::vector<Loop> loops;
  std::map<int, std::vector<std::pair<uint32_t, uint32_t>>> boundary;  // piece -> boundary edges
  for (const auto& entry : directed) {
    if (directed.count({entry.first.second, entry.first.first})) continue;
    boundary[find(entry.second)].push_back(entry.first);
  }
  for (auto& item : boundary) {
    std::map<uint32_t, std::vector<size_t>> from;
    for (size_t i = 0; i < item.second.size(); ++i) from[item.second[i].first].push_back(i);
    std::vector<char> used(item.second.size(), 0);
    for (size_t start = 0; start < item.second.size(); ++start) {
      if (used[start]) continue;
      Loop loop;
      loop.piece = item.first;
      const uint32_t origin = item.second[start].first;
      uint32_t cursor = origin;
      size_t guard = 0;
      while (guard++ <= item.second.size() + 1) {
        size_t next = item.second.size();
        for (size_t idx : from[cursor])
          if (!used[idx]) {
            next = idx;
            break;
          }
        if (next == item.second.size()) break;
        used[next] = 1;
        loop.verts.push_back(cursor);
        cursor = item.second[next].second;
        if (cursor == origin) break;
      }
      if (cursor != origin || loop.verts.size() < 3) continue;
      std::vector<fg::Vec2> flat;
      for (uint32_t v : loop.verts) {
        const vec3 d = sub3(pos(v), ax.o);
        flat.push_back({dot3(d, ax.u), dot3(d, ax.v)});
      }
      loop.area = fg::signedArea(flat);
      loops.push_back(std::move(loop));
    }
  }
  std::stable_sort(loops.begin(), loops.end(), [](const Loop& a, const Loop& b) {
    if (a.piece != b.piece) return a.piece < b.piece;
    return std::abs(a.area) > std::abs(b.area);
  });
  int currentPiece = 0;
  size_t ringCount = 0;
  for (const Loop& loop : loops) {
    std::vector<double> flat;
    std::vector<double> wall;
    for (size_t k = 0; k < loop.verts.size(); ++k) {
      const uint32_t v = loop.verts[k];
      const uint32_t w = loop.verts[(k + 1) % loop.verts.size()];
      const vec3 p = pos(v);
      flat.push_back(p.x);
      flat.push_back(p.y);
      flat.push_back(p.z);
      // The face across this edge is the one holding the reverse edge.
      vec3 nrm(0, 0, 0);
      const auto across = allEdges.find((static_cast<uint64_t>(w) << 32) | v);
      if (across != allEdges.end()) nrm = triNormal(across->second);
      wall.push_back(nrm.x);
      wall.push_back(nrm.y);
      wall.push_back(nrm.z);
    }
    if (faces.empty() || loop.piece != currentPiece) {
      faces.emplace_back();
      currentPiece = loop.piece;
    }
    faces.back().rings.push_back(std::move(flat));
    faces.back().walls.push_back(std::move(wall));
    ++ringCount;
  }
  say(log, "Found " + std::to_string(faces.size()) + " flat face(s) on that plane, " + std::to_string(ringCount) +
               " outline(s) in all.");
  return faces;
}

std::vector<std::vector<double>> coplanarRings(const Manifold& solid, const Frame& frame, std::string* log,
                                               std::vector<std::vector<double>>* walls) {
  std::vector<std::vector<double>> out;
  if (walls) walls->clear();
  for (const FaceRings& face : coplanarFaces(solid, frame, log)) {
    for (const std::vector<double>& ring : face.rings) out.push_back(ring);
    if (walls)
      for (const std::vector<double>& wall : face.walls) walls->push_back(wall);
  }
  return out;
}

std::vector<std::vector<double>> faceRingsAt(const Manifold& solid, const Frame& frame, const double point[3],
                                             std::string* log, std::vector<std::vector<double>>* walls) {
  if (walls) walls->clear();
  const std::vector<FaceRings> faces = coplanarFaces(solid, frame, log);
  if (faces.empty()) return {};
  const Axes ax = axesOf(frame);
  const vec3 d = sub3(vec3(point[0], point[1], point[2]), ax.o);
  const fg::Vec2 at{dot3(d, ax.u), dot3(d, ax.v)};
  auto flatten = [&](const std::vector<double>& world) {
    std::vector<fg::Vec2> out;
    for (size_t i = 0; i + 2 < world.size(); i += 3) {
      const vec3 q = sub3(vec3(world[i], world[i + 1], world[i + 2]), ax.o);
      out.push_back({dot3(q, ax.u), dot3(q, ax.v)});
    }
    return out;
  };
  // Inside the outer outline and outside every hole: an odd number of crossings over all rings.
  // Otherwise how far the point is from the face's edge, for a click right on an edge.
  size_t pick = faces.size();
  double nearest = 0.5;
  for (size_t f = 0; f < faces.size(); ++f) {
    bool inside = false;
    double edge = 1e300;
    for (const std::vector<double>& world : faces[f].rings) {
      const std::vector<fg::Vec2> ring = flatten(world);
      for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const fg::Vec2& a = ring[i];
        const fg::Vec2& b = ring[j];
        if ((a.y > at.y) != (b.y > at.y) && at.x < (b.x - a.x) * (at.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
        const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
        const double t = len2 < 1e-18 ? 0.0 : std::clamp(((at.x - a.x) * dx + (at.y - a.y) * dy) / len2, 0.0, 1.0);
        edge = std::min(edge, std::hypot(at.x - (a.x + t * dx), at.y - (a.y + t * dy)));
      }
    }
    if (inside) {
      pick = f;
      break;
    }
    if (edge < nearest) {
      nearest = edge;
      pick = f;
    }
  }
  if (pick >= faces.size()) {
    say(log, "No face on that plane under the click.");
    return {};
  }
  say(log, "Found the clicked face, " + std::to_string(faces[pick].rings.size()) + " outline(s).");
  if (walls) *walls = faces[pick].walls;
  return faces[pick].rings;
}

std::vector<std::vector<double>> faceRingsNear(const Manifold& solid, const Frame& frame,
                                               const std::vector<double>& nearRing, std::string* log,
                                               std::vector<std::vector<double>>* walls) {
  if (walls) walls->clear();
  const std::vector<FaceRings> faces = coplanarFaces(solid, frame, log);
  if (faces.empty()) return {};
  const Axes ax = axesOf(frame);
  auto flatten = [&](const std::vector<double>& world) {
    std::vector<fg::Vec2> out;
    for (size_t i = 0; i + 2 < world.size(); i += 3) {
      if (std::abs(world[i]) > 1e19) continue;
      const vec3 d = sub3(vec3(world[i], world[i + 1], world[i + 2]), ax.o);
      out.push_back({dot3(d, ax.u), dot3(d, ax.v)});
    }
    return out;
  };
  const std::vector<fg::Vec2> reference = flatten(nearRing);
  if (reference.size() < 3) return {};
  std::vector<fg::Vec2> sample;
  const size_t step = std::max<size_t>(1, reference.size() / 48);
  for (size_t i = 0; i < reference.size(); i += step) sample.push_back(reference[i]);

  auto distanceToRing = [](const fg::Vec2& p, const std::vector<fg::Vec2>& ring) {
    double best = 1e300;
    for (size_t i = 0; i < ring.size(); ++i) {
      const fg::Vec2& a = ring[i];
      const fg::Vec2& b = ring[(i + 1) % ring.size()];
      const double dx = b.x - a.x;
      const double dy = b.y - a.y;
      const double len2 = dx * dx + dy * dy;
      double t = len2 < 1e-18 ? 0.0 : ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
      t = std::clamp(t, 0.0, 1.0);
      best = std::min(best, std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy)));
    }
    return best;
  };

  size_t pick = faces.size();
  double pickScore = 1e300;
  for (size_t f = 0; f < faces.size(); ++f) {
    if (faces[f].rings.empty()) continue;
    const std::vector<fg::Vec2> outer = flatten(faces[f].rings.front());
    if (outer.size() < 3) continue;
    double total = 0;
    for (const fg::Vec2& p : sample) total += distanceToRing(p, outer);
    const double score = total / static_cast<double>(sample.size());
    if (score < pickScore) {
      pickScore = score;
      pick = f;
    }
  }
  // The picker's outline is the same face, only welded and snapped, so a good match sits far
  // inside this bound. Anything looser means we found a different face and should not guess.
  if (pick >= faces.size() || pickScore > 1.0) {
    say(log, "No exact outline matched the clicked face, so the picked outline was used instead.");
    return {};
  }
  say(log, "Matched the clicked face to its exact outline, " + std::to_string(faces[pick].rings.size()) +
               " outline(s), within " + std::to_string(pickScore) + " mm.");
  if (walls) *walls = faces[pick].walls;
  return faces[pick].rings;
}

}  // namespace sf
