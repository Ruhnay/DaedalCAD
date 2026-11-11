#pragma once
// Surface fillet kernel. Qt-free on purpose. Geometry lives in fillet_geom.h.

#include <manifold/manifold.h>

#include <string>
#include <vector>

namespace sf {

struct Frame {
  double origin[3] = {0, 0, 0};  // any point on the face plane
  double normal[3] = {0, 0, 1};  // outward normal of the face
  double axis[3] = {1, 0, 0};    // any in-plane direction; only fixes the 2D frame
};

// One cutter for one face outline. `ring` is x y z triples in world space with the face on the
// left of the walking direction (outer outline counter-clockwise seen from the normal, hole
// outline clockwise). below = the cutter sits under the face (rounds an edge); otherwise it sits
// on top of the face (fills an inside corner). Returns the cutter in world space, or empty.
//
// `walls`, optional, holds for each outline edge (point i to point i+1) the outward normal of the
// solid's face on the far side of that edge, x y z per edge, world space. With it the fillet is
// built tangent to the real wall (a drafted pocket, a sloped side); without it walls are assumed
// to stand square to the face.
manifold::Manifold buildCutter(const std::vector<double>& ring, const Frame& frame, double radius, int segments,
                               bool below, std::string* warning, std::string* log,
                               const std::vector<double>& walls = {});

// One flat face: rings[0] is its outer outline, the rest are its holes. walls[k] parallels rings[k]:
// for each edge, the outward normal of the solid's face across that edge (x y z per edge).
struct FaceRings {
  std::vector<std::vector<double>> rings;
  std::vector<std::vector<double>> walls;
};

// Every flat face of `solid` that faces `frame.normal` and lies on the plane. The outlines come
// straight off the mesh, so they carry the solid's own vertices with no welding or snapping.
std::vector<FaceRings> coplanarFaces(const manifold::Manifold& solid, const Frame& frame, std::string* log);

// Every outline of every such face, flattened, in the ring convention above.
std::vector<std::vector<double>> coplanarRings(const manifold::Manifold& solid, const Frame& frame, std::string* log,
                                               std::vector<std::vector<double>>* walls = nullptr);

// The outlines of the single face that best matches `nearRing` (a rough outline of the clicked
// face). Empty when nothing on that plane is close to it.
std::vector<std::vector<double>> faceRingsNear(const manifold::Manifold& solid, const Frame& frame,
                                               const std::vector<double>& nearRing, std::string* log,
                                               std::vector<std::vector<double>>* walls = nullptr);

// The outlines of the face on that plane that contains `point` (world, on or near the plane): the
// one clicked, when the outline the picker saw belongs to one part of a group and the group's own
// face is smaller. Empty when no face there is within 0.5 mm of the point.
std::vector<std::vector<double>> faceRingsAt(const manifold::Manifold& solid, const Frame& frame, const double point[3],
                                             std::string* log, std::vector<std::vector<double>>* walls = nullptr);

}  // namespace sf
