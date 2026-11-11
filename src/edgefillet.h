#pragma once
// Edge fillet (beta): round chosen sharp edges of ANY solid - a merged group, an STL, text.
// Qt-free on purpose so it can be tested on its own (test/edgefillet_test.cpp).
//
// 1. findChains: every mesh edge whose two faces meet at more than a set angle is a sharp edge.
//    Sharp edges are joined into chains: a box edge, the rim of a hole, the line where a cylinder
//    stands on a plate. A chain stops where three or more sharp edges meet, where it turns hard,
//    or where it changes from an outside edge to an inside one.
// 2. A picked chain is stored as its polyline. matchPick finds it again on a changed solid, so a
//    fillet keeps its edge when something in the group is moved or resized a little.
// 3. applyEdgeFillets sweeps the rolling-ball cross-section along each chain: outside (convex)
//    edges are cut away, inside (concave) edges are filled. Where two rounded chains meet the
//    pieces are mitred; an inside fillet that wraps an outside corner gets a swept corner; three
//    square outside edges meeting at a corner get a ball corner.

#include <manifold/manifold.h>

#include <string>
#include <vector>

namespace ef {

struct Chain {
  std::vector<double> pts;  // x y z per point, in order along the chain
  std::vector<double> nA;   // per point: outward normal of the face on one side (x y z)
  std::vector<double> nB;   // per point: outward normal of the face on the other side
  bool closed = false;      // a loop: the last point joins the first
  bool concave = false;     // inside edge (gets filled) instead of outside edge (gets cut)
  double length = 0;
  double angle = 0;         // mean angle between the two faces' normals, degrees
};

// Sharp edges of `solid`, joined into chains. Edges whose faces meet at less than minAngleDeg
// (between the normals) count as smooth: the facets of a cylinder or sphere.
std::vector<Chain> findChains(const manifold::Manifold& solid, double minAngleDeg = 25.0);

// A stored pick: the chain's polyline when it was picked, and the radius to round it with.
// chamfer: cut it flat instead, from the same two lines on the faces a round of that radius would
// start at (on a square edge: `radius` back along each face, a 45 degree chamfer).
struct Pick {
  std::vector<double> pts;
  double radius = 1.0;
  bool chamfer = false;
};

// Which chains a stored pick means on this solid: the closest match, plus any chain lying right
// on it (when a later change split it). Empty when nothing is close enough any more.
std::vector<int> matchPick(const std::vector<Chain>& chains, const std::vector<double>& pickPts);

// Rounds the picked chains of `solid`. `lost` (optional) counts picks that matched nothing.
manifold::Manifold applyEdgeFillets(const manifold::Manifold& solid, const std::vector<Pick>& picks, int segments,
                                    std::string* warning, std::string* log, int* lost = nullptr);

// Node storage: [radius, pointCount, x, y, z, ...] per pick; a chamfer stores its size negative.
std::vector<Pick> decodePicks(const std::vector<double>& data);
std::vector<double> encodePicks(const std::vector<Pick>& picks);

}  // namespace ef
