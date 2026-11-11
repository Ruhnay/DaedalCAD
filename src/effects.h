#pragma once

// Shape effects for any part: a surface texture (knurling, wood grain, a picture...) as real
// geometry, a smooth "clay" merge of several solids, and a low-poly or voxel restyle. The texture
// and the merge are distance fields meshed by lattice::meshField, so they come out closed and clean.
// Qt-free, so they can be tested on their own (test/effects_test.cpp).

#include "lattice.h"
#include "sculpt.h"

#include <string>
#include <vector>

namespace fx {

// ---- Surface textures --------------------------------------------------------------------------

enum class Texture { Knurl, Ribs, Hexes, Dots, Wood, Leather, Weave, Picture };

struct TextureSettings {
  Texture kind = Texture::Knurl;
  double size = 3;        // mm, one repeat of the pattern
  double depth = 0.6;     // mm, how far it stands out (or is cut in)
  bool raised = true;     // false: cut into the surface
  int wrap = 0;           // 0 from every side (box), 1 round the part's upright axis
  int where = 0;          // 0 all but the bottom, 1 everywhere, 2 the sides only
  double angle = 0;       // degrees, turns the pattern on the surface
  int seed = 1;           // wood, leather: which random pattern
  sculpt::StampImage picture;  // Picture: 0 .. 1 per pixel, tiled `size` mm wide
  double detail = 0;      // mm, triangle size; 0 picks one from the size and depth
};

// The pattern itself at (u, v) on the surface, in mm: 0 (base) .. 1 (full depth).
double textureValue(const TextureSettings& s, double u, double v);
double textureDetail(const sculpt::Mesh& part, const TextureSettings& s);
double textureTriangles(const sculpt::Mesh& part, const TextureSettings& s);
bool texture(const sculpt::Mesh& part, const TextureSettings& s, sculpt::Mesh* out, std::string* error,
             lattice::Report* report = nullptr, lattice::Progress* progress = nullptr);

// ---- Smooth merge ------------------------------------------------------------------------------

// The solids joined with a soft blend `radius` mm wide where they meet, like clay, and the holes cut
// with the same soft edge. Meshes in world millimetres.
double blendDetail(const std::vector<sculpt::Mesh>& solids, double radius);
bool blend(const std::vector<sculpt::Mesh>& solids, const std::vector<sculpt::Mesh>& holes, double radius,
           sculpt::Mesh* out, std::string* error, lattice::Progress* progress = nullptr);

// ---- Restyle -----------------------------------------------------------------------------------

// Low-poly: the part as large flat facets about `facet` mm across; jitter (0 .. 1) nudges the
// corners for a hand-cut look.
bool lowPoly(const sculpt::Mesh& part, double facet, double jitter, int seed, sculpt::Mesh* out, std::string* error,
             lattice::Progress* progress = nullptr);
// Voxel: the part built from cubes `cube` mm wide, on a grid from its lowest corner.
bool voxel(const sculpt::Mesh& part, double cube, sculpt::Mesh* out, std::string* error, lattice::Progress* progress = nullptr);
size_t voxelCount(const sculpt::Mesh& part, double cube);  // grid cells in the part's box

// ---- Wrap ---------------------------------------------------------------------------------------

// A thin part (text, a logo) laid onto another part's surface. Its flat side is found from its
// thinnest direction: lying flat it is read from above, standing up from the side it faces.
struct WrapSettings {
  int mode = 0;          // 0 round the target's upright axis, 1 straight on (along the thin direction)
  double angle = 0;      // round: where round, degrees (0 the front, 90 the right side)
  double height = 0;     // round: the middle of it, mm above the target's bottom
  double depth = 1;      // how far it stands out (or is cut in)
  bool cut = false;      // cut in: the result is a hole shape, reaching a little outside the surface
};
// The direction the thin part faces (outward), from its shape: 0 +Z (flat), 1 -Y (standing,
// facing the front), 2 +X (standing, facing right).
int wrapFacing(const sculpt::Mesh& decal);
bool wrap(const sculpt::Mesh& decal, const sculpt::Mesh& target, const WrapSettings& s, sculpt::Mesh* out, std::string* error,
          lattice::Progress* progress = nullptr);

// ---- Articulate -----------------------------------------------------------------------------------

// The part cut across its length into segments joined by print-in-place joints: each a 45° double
// cone (self-supporting) turning on an upright axis inside a fork of the next segment, with V-shaped
// gaps between the segments so they can swing side to side. Prints flat in one piece.
struct ArticulateSettings {
  int axis = 0;           // along X (0) or Y (1)
  int joints = 6;
  double from = 0.12;     // where the first and last joint are, as parts of the length
  double to = 0.9;
  double gap = 0.35;      // mm between moving faces
  double swing = 15;      // degrees each joint swings each way
};
bool articulate(const sculpt::Mesh& part, const ArticulateSettings& s, sculpt::Mesh* out, std::string* error,
                lattice::Progress* progress = nullptr);

// ---- Simplify ---------------------------------------------------------------------------------

// Fewer triangles for the same shape: edges are collapsed wherever that moves the surface by no
// more than `tolerance` mm (flat and gently curved areas lose the most, sharp detail stays).
bool simplify(const sculpt::Mesh& part, double tolerance, sculpt::Mesh* out, std::string* error,
              lattice::Progress* progress = nullptr);

// ---- Mold -------------------------------------------------------------------------------------
// A two-piece mould of the part for casting (resin, silicone, plaster, chocolate): a block `wall` mm
// bigger than the part all round, with the part's shape hollowed out, split in two across `axis`.
// A pour hole with a funnel runs from the part's highest point up through the top, thin vents from
// its other high points let the air out, and round keys on the parting face (bumps on one half,
// sockets with `clearance` on the other) line the halves up. The part must come out of each half:
// a parting plane through its widest slice is picked for you.
struct MoldSettings {
  int axis = 2;            // the halves split across X (0), Y (1) or Z (2: a lower and an upper half)
  double at = -1;          // where along the axis, 0 .. 1 of the part; below 0: through its widest slice
  double wall = 8;         // mm of mould round the part
  double pour = 6;         // pour hole diameter, mm (0: none)
  int vents = 2;           // thin air vents from other high points
  double vent = 2;         // vent diameter, mm
  bool keys = true;        // alignment keys on the parting face
  double clearance = 0.1;  // mm round the keys in their sockets
  bool printLayout = true; // both halves on the plate, parting faces up (true) or closed (false)
};
// Where the parting plane goes (0 .. 1 along the axis) when `at` is below 0: the widest slice
// between a fifth and four fifths of the part.
double moldParting(const sculpt::Mesh& part, int axis);
bool mold(const sculpt::Mesh& part, const MoldSettings& s, sculpt::Mesh* out, std::string* error,
          lattice::Progress* progress = nullptr);

// A closed mesh as a Manifold solid (empty if it is not one).
manifold::Manifold toManifold(const sculpt::Mesh& mesh);

}  // namespace fx
