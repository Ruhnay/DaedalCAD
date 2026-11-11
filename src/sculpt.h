#pragma once

// Freeform sculpting: a closed triangle mesh pushed and pulled with brushes, as in Blender's Sculpt
// Mode. The mesh stays closed (no brush changes which points connect) and a guard holds a brush
// back where it would push the surface through itself, so the result is always a printable solid.
// Remesh rebuilds the surface as an even mesh at a chosen detail (a voxel remesh, like Blender's).

#include <manifold/manifold.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sculpt {

using vec3 = manifold::vec3;
using Tri = std::array<uint32_t, 3>;

struct Mesh {
  std::vector<vec3> pos;
  std::vector<Tri> tri;  // counter-clockwise seen from outside
};

enum class Brush {
  Draw, Clay, Inflate, Smooth, Grab, Pinch, Flatten, Layer, Blob, Crease, Fill, Scrape, SnakeHook, Nudge, Rotate,
  Mask,   // paints a mask: masked surface is held still by every other brush (Ctrl: unmask)
  Stamp,  // presses a picture into the surface, or raises it (see Stroke::stamp)
};

// Brushes that follow the mouse over a plane through the first point (facing the view) instead of
// dabbing on the surface: Grab, Snake hook, Rotate. Their strokes go through grabTo.
bool dragBrush(Brush brush);
enum class Falloff { Smooth, Sharp, Flat };

// A picture for the Stamp brush: brightness 0 (no change) .. 1 (full height), row by row from the
// top. Sampled with bilinear filtering; outside it is 0.
struct StampImage {
  int width = 0;
  int height = 0;
  std::vector<float> value;
  double sample(double x, double y) const;  // x, y in 0 .. 1, y down
};

struct Stroke {
  Brush brush = Brush::Draw;
  double radius = 5;      // mm, on the surface
  double strength = 0.5;  // 0 .. 1
  Falloff falloff = Falloff::Smooth;
  bool invert = false;    // Draw digs, Clay carves, Inflate shrinks, Pinch spreads, Flatten sharpens
  bool mirror[3] = {false, false, false};
  vec3 mirrorCenter{0, 0, 0};  // the middle for mirror and radial symmetry
  int radial = 1;              // the stroke repeated this many times around radialAxis (1: off)
  int radialAxis = 2;          // 0 X, 1 Y, 2 Z
  std::shared_ptr<const StampImage> stamp;  // Stamp: the picture
  vec3 up{0, 0, 1};                          // Stamp: which way is up on the screen, for the picture
};

// The copies of a stroke under mirror and radial symmetry, as orthogonal matrices about the middle
// (the first one is the stroke itself).
std::vector<manifold::mat3> symmetries(const Stroke& stroke);

// How much a point at distance t (0 at the middle, 1 at the rim) of the brush takes.
double falloff(Falloff kind, double t);

// Start shapes: closed and evenly triangulated, edges about `edge` mm long, centred on the origin.
// kind: "ball", "cube", "cylinder", "slab", "capsule"; size is the overall size in mm.
bool startShape(const std::string& kind, double size, double edge, Mesh* out, std::string* error);

// The surface rebuilt as an even mesh with edges about `edge` mm (a voxel remesh). It is closed and
// does not pass through itself; parts that overlapped are joined.
bool remesh(const Mesh& in, double edge, Mesh* out, std::string* error);
// Roughly how many triangles remesh would make (to warn before a very fine detail).
double remeshTriangleEstimate(const Mesh& in, double edge);

Mesh fromManifold(const manifold::Manifold& solid);
// Binary STL (its own facet normals). readStl takes binary or ASCII and joins corners at the same
// spot into one point, so a closed STL comes back as a closed mesh.
bool writeStl(const Mesh& mesh, const std::string& path, const std::string& title = "DaedalCAD sculpt, millimetres");
bool readStl(const std::string& path, Mesh* out, std::string* error);
// The same, as the bytes of the file (for callers that open files themselves).
std::string stlBytes(const Mesh& mesh, const std::string& title = "DaedalCAD sculpt, millimetres");
bool parseStl(const std::string& data, Mesh* out, std::string* error);
bool closedManifold(const Mesh& mesh);  // every edge shared by exactly two triangles, consistently
bool selfIntersects(const Mesh& mesh);
// The triangles that cut through another one (not counting neighbours that share a point). With
// only: just those triangles are checked, and the ones they cut are listed too.
std::vector<uint32_t> crossingTriangles(const Mesh& mesh, const std::vector<uint32_t>* only = nullptr);
double volume(const Mesh& mesh);
double averageEdge(const Mesh& mesh);
void bounds(const Mesh& mesh, vec3* lo, vec3* hi);

// Signed distance to a closed mesh, positive inside. Exact within `band` of the surface; further
// away it gives +band or -band (only the side is right there). Safe to call from several threads.
class SurfaceField {
 public:
  SurfaceField(const Mesh& mesh, double band);
  ~SurfaceField();
  double operator()(const vec3& p) const;
  // The same, and which way it grows fastest (unit length; zero beyond the band).
  double operator()(const vec3& p, vec3* slope) const;
  bool inside(const vec3& p) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Sculptor {
 public:
  Sculptor();
  ~Sculptor();
  Sculptor(const Sculptor&) = delete;
  Sculptor& operator=(const Sculptor&) = delete;

  void setMesh(Mesh mesh);  // a new mesh; clears undo
  const Mesh& mesh() const { return mesh_; }
  const std::vector<vec3>& normals() const { return normals_; }
  double averageEdge() const { return avgEdge_; }

  // The first point of the surface along the ray (dir need not be unit length).
  bool raycast(const vec3& origin, const vec3& dir, vec3* hit, vec3* normal = nullptr) const;

  // A stroke: beginStroke at the first surface point, then dab at each next surface point (Grab:
  // grabTo with the cursor on the plane through the first point, facing the view), endStroke.
  void beginStroke(const Stroke& stroke, const vec3& hit, const vec3& viewDir);
  void dab(const vec3& hit);
  void grabTo(const vec3& planePoint);  // Grab, Snake hook, Rotate (see dragBrush)
  void endStroke();
  bool stroking() const { return stroking_; }
  int heldBack() const { return heldBack_; }  // dabs the guard held back in the last stroke

  bool remeshTo(double edge, std::string* error);  // undoable (keeps the mask)

  // The mask: 0 free, 1 held still. Clear and Invert are undoable.
  const std::vector<float>& mask() const { return mask_; }
  bool hasMask() const;
  void clearMask();
  void invertMask();

  bool undo();
  bool redo();
  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  uint64_t version() const { return version_; }  // changes with every edit

 private:
  struct Grid;
  struct Step {
    bool topology = false;
    bool maskOnly = false;
    Mesh before, after;
    std::vector<uint32_t> idx;
    std::vector<vec3> from, to;
    std::vector<float> maskBefore, maskAfter;  // topology and mask steps
  };
  struct Region {
    std::vector<uint32_t> verts;
    std::vector<double> weight;
    vec3 areaCenter{0, 0, 0};
    vec3 areaNormal{0, 0, 1};
    vec3 center{0, 0, 0};
    manifold::mat3 xf{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // the symmetry copy this region belongs to
  };

  void rebuild();
  void updateNormals(const std::vector<uint32_t>& verts);
  vec3 triNormal(uint32_t t) const;
  // view: only points facing against it (zero: all points in reach).
  void gather(const vec3& center, double radius, const vec3& view, Region* region) const;
  void applyDab(const vec3& center);
  bool commitMoves(const std::vector<std::pair<uint32_t, vec3>>& moves);
  bool movesOk(const std::vector<uint32_t>& moved, const std::vector<uint32_t>& tris);
  void touch(uint32_t v);
  void pushStep(Step step);
  std::vector<manifold::mat3> variants() const { return symmetries(stroke_); }
  vec3 place(const manifold::mat3& m, const vec3& p) const;  // p moved by that symmetry copy
  void setMaskStep(std::vector<float> before);

  Mesh mesh_;
  std::vector<vec3> normals_;
  std::vector<float> mask_;
  std::vector<float> strokeMask_;  // the mask when a Mask stroke began
  std::vector<uint32_t> vtStart_, vt_;  // vertex -> triangles
  std::vector<uint32_t> vvStart_, vv_;  // vertex -> neighbours
  std::unique_ptr<Grid> grid_;
  double avgEdge_ = 1;

  bool stroking_ = false;
  Stroke stroke_;
  vec3 viewDir_{0, 0, -1};
  vec3 grabStart_{0, 0, 0};
  vec3 lastHit_{0, 0, 0};    // Nudge: where the previous dab was
  vec3 snakeAt_{0, 0, 0};    // Snake hook: where the hook is now
  vec3 rotateFrom_{0, 0, 0};  // Rotate: the first drag direction, for the angle
  bool rotateSet_ = false;
  std::vector<Region> grabRegions_;
  std::vector<vec3> grabOrigin_;  // per vertex of the grab regions, in order
  std::vector<uint32_t> strokeStamp_;
  std::vector<vec3> strokeFrom_;
  std::vector<vec3> strokeNormal_;  // Layer: each point's normal when the stroke first touched it
  std::vector<uint32_t> strokeVerts_;
  uint32_t strokeId_ = 0;
  int heldBack_ = 0;
  mutable std::vector<uint32_t> stamp_;
  mutable uint32_t stampId_ = 0;

  std::vector<Step> undo_, redo_;
  uint64_t version_ = 1;
};

}  // namespace sculpt
