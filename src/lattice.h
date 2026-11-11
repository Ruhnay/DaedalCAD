#pragma once

// Lattice: a part turned into an open, light structure that prints well. A Voronoi skin (the outer
// shell, cut into an organic web of cells), a gyroid, a honeycomb, a Voronoi lattice, a random
// sponge or a bubble foam inside, with an optional solid skin around it. Built as a distance field (the part's surface and the pattern's)
// and meshed with Manifold, so the result is always one closed, printable solid; loose crumbs too
// small to print are left out.

#include "sculpt.h"

#include <atomic>
#include <functional>
#include <string>

namespace lattice {

enum class Kind { VoronoiSkin, Gyroid, Honeycomb, VoronoiLattice, Sponge, SpongeWalls, Bubbles };

struct Settings {
  Kind kind = Kind::Gyroid;
  double cell = 8;    // size of one cell, mm
  double wall = 1.2;  // thickness of the walls or struts, mm
  double skin = 0;    // a solid outer skin this thick (0: open); for a Voronoi skin, the shell
  int seed = 1;       // which random pattern (Voronoi, sponge, bubbles)
  double detail = 0;  // mesh edge length, mm; 0 picks one from the wall and cell
};

double autoDetail(const Settings& settings);
// The detail actually used for this part (coarser than asked if the part is very big).
double usedDetail(const sculpt::Mesh& part, const Settings& settings);
// About how many triangles the result will have.
double estimateTriangles(const sculpt::Mesh& part, const Settings& settings);

struct Report {
  double detail = 0;
  int droppedCrumbs = 0;  // loose pieces left out
  int keptRough = 0;      // points the clean-up had to relax (folds and crossings untangled)
  bool clean = true;      // no folds or crossings left
};

// Shared with the window while it builds (build runs on another thread): how far it is, which step,
// and a request to stop.
struct Progress {
  std::atomic<double> fraction{0};
  std::atomic<int> stage{0};
  std::atomic<bool> cancel{false};
};
// The steps, for Progress::stage.
const char* stageName(int stage);

// part: a closed mesh (millimetres). The result is a closed mesh in the same place, smoothed onto
// the true surface, with its edges rounded and folds and self-intersections untangled (Report::clean
// says if a few could not be).
bool build(const sculpt::Mesh& part, const Settings& settings, sculpt::Mesh* out, std::string* error,
           Report* report = nullptr, Progress* progress = nullptr);

// The shared mesher (lattices, textures, smooth merges): the solid where field(p) > 0 inside the box
// lo..hi, in triangles about `edge` long, smoothed onto the field and untangled like a lattice.
// field must be safe to call from several threads. Pieces smaller than `crumbs` of the largest are
// left out (0 keeps everything). error: "empty", "open" or "stopped".
bool meshField(const std::function<double(const sculpt::vec3&)>& field, const sculpt::vec3& lo, const sculpt::vec3& hi,
               double edge, sculpt::Mesh* out, std::string* error, Report* report = nullptr, Progress* progress = nullptr,
               double crumbs = 0.02);

// The pattern on its own, positive inside its walls (for tests).
double patternField(const Settings& settings, const sculpt::vec3& p);
// The sponge's solid share of the volume (0..1) for these settings.
double spongeSolidShare(const Settings& settings);
// The cell a strut sponge really gets: the one asked for, or larger if the walls are too thin for it
// (struts thinner than a quarter of the cell would fall apart into crumbs).
double spongeCell(const Settings& settings);

}  // namespace lattice
