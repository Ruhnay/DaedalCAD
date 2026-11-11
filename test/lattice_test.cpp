// Lattice: every kind comes out closed, in the part's place, about as full as its walls make it,
// and skins and patterns behave as described.

#include "lattice.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using namespace lattice;
using sculpt::vec3;
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

}  // namespace

int main() {
  const auto started = std::chrono::steady_clock::now();
  const double pi = 3.14159265358979;

  // The patterns on their own.
  {
    Settings g;
    g.kind = Kind::Gyroid;
    g.cell = 10;
    g.wall = 1;
    check(std::abs(patternField(g, vec3(0, 0, 0)) - 0.5) < 1e-9, "gyroid: the origin lies on the surface (half a wall inside)");
    check(patternField(g, vec3(1.25, 1.25, 1.25)) < -1, "gyroid: open space between the walls");
    Settings h = g;
    h.kind = Kind::Honeycomb;
    check(patternField(h, vec3(0, 0, 0)) < -4, "honeycomb: a cell's middle is open");
    check(patternField(h, vec3(5, 0, 7)) > 0.4, "honeycomb: half a cell from the middle is wall, at any height");
  }

  const sculpt::Mesh cube = sculpt::fromManifold(manifold::Manifold::Cube(vec3(30, 30, 30), true));
  const double cubeVolume = 27000;

  // Gyroid in a cube: open, then with a skin.
  {
    Settings s;
    s.kind = Kind::Gyroid;
    s.cell = 10;
    s.wall = 1.5;
    sculpt::Mesh out;
    std::string error;
    Report report;
    const bool ok = build(cube, s, &out, &error, &report);
    check(ok, "gyroid builds (" + error + ")");
    if (ok) {
      check(sculpt::closedManifold(out), "gyroid: closed");
      check(sculpt::crossingTriangles(out).empty() && report.clean, "gyroid: nothing folded or crossing");
      const double fill = sculpt::volume(out) / cubeVolume;
      check(fill > 0.15 && fill < 0.45, "gyroid: about a quarter full (" + num(fill) + ")");
      vec3 lo, hi;
      sculpt::bounds(out, &lo, &hi);
      check(la::length(lo - vec3(-15, -15, -15)) < 0.5 && la::length(hi - vec3(15, 15, 15)) < 0.5, "gyroid: in the cube's place");
      std::printf("gyroid: %zu triangles at %.2f mm (estimated %.0f)\n", out.tri.size(), report.detail, estimateTriangles(cube, s));
      Settings skinned = s;
      skinned.skin = 1.2;
      sculpt::Mesh closedOut;
      check(build(cube, skinned, &closedOut, &error), "gyroid with a skin builds");
      check(sculpt::closedManifold(closedOut), "gyroid with a skin: closed");
      check(sculpt::volume(closedOut) > sculpt::volume(out) + 0.8 * (cubeVolume - 27.6 * 27.6 * 27.6) * 0.6,
            "the skin adds a shell (" + num(sculpt::volume(closedOut)) + " vs " + num(sculpt::volume(out)) + ")");
    }
  }

  // Honeycomb: upright walls, about as full as wall / cell says.
  {
    Settings s;
    s.kind = Kind::Honeycomb;
    s.cell = 8;
    s.wall = 1.2;
    sculpt::Mesh out;
    std::string error;
    Report report;
    check(build(cube, s, &out, &error, &report), "honeycomb builds (" + error + ")");
    check(sculpt::closedManifold(out), "honeycomb: closed");
    check(sculpt::crossingTriangles(out).empty(), "honeycomb: nothing crossing");
    // A hexagon of width c with walls w: about 2w/c of the area (each wall shared by two cells).
    const double fill = sculpt::volume(out) / cubeVolume;
    const double want = 1 - std::pow((s.cell - s.wall) / s.cell, 2);
    check(std::abs(fill - want) < 0.06, "honeycomb: fill " + num(fill) + " vs about " + num(want));
  }

  // Voronoi skin on a ball: a shell cut into a web, nothing deeper than the shell.
  {
    sculpt::Mesh ball;
    std::string error;
    sculpt::startShape("ball", 40, 1.0, &ball, &error);
    Settings s;
    s.kind = Kind::VoronoiSkin;
    s.cell = 8;
    s.wall = 1.4;
    s.skin = 1.6;
    sculpt::Mesh out;
    Report report;
    check(build(ball, s, &out, &error, &report), "Voronoi skin builds (" + error + ")");
    check(sculpt::closedManifold(out), "Voronoi skin: closed");
    check(sculpt::crossingTriangles(out).empty(), "Voronoi skin: nothing crossing");
    double nearest = 1e9, farthest = 0;
    for (const vec3& p : out.pos) {
      nearest = std::min(nearest, la::length(p));
      farthest = std::max(farthest, la::length(p));
    }
    check(nearest > 20 - 1.6 - report.detail && farthest < 20 + report.detail, "Voronoi skin: only the outer 1.6 mm (" +
                                                                                   num(nearest) + " .. " + num(farthest) + ")");
    const double shell = 4.0 / 3 * pi * (std::pow(20.0, 3) - std::pow(18.4, 3));
    const double fill = sculpt::volume(out) / shell;
    check(fill > 0.15 && fill < 0.75, "Voronoi skin: a web with holes, not a closed shell (" + num(fill) + " of the shell)");
    // Another seed: another pattern.
    Settings other = s;
    other.seed = 2;
    sculpt::Mesh second;
    build(ball, other, &second, &error);
    check(std::abs(sculpt::volume(second) - sculpt::volume(out)) > 1e-3, "a different seed makes a different web");
  }

  // Voronoi lattice in a cube: struts, closed, crumbs left out.
  {
    Settings s;
    s.kind = Kind::VoronoiLattice;
    s.cell = 9;
    s.wall = 1.6;
    sculpt::Mesh out;
    std::string error;
    Report report;
    check(build(cube, s, &out, &error, &report), "Voronoi lattice builds (" + error + ")");
    check(sculpt::closedManifold(out), "Voronoi lattice: closed");
    check(sculpt::crossingTriangles(out).empty(), "Voronoi lattice: nothing crossing");
    const double fill = sculpt::volume(out) / cubeVolume;
    check(fill > 0.03 && fill < 0.3, "Voronoi lattice: light (" + num(fill) + ")");
    std::printf("Voronoi lattice: %zu triangles, %d crumbs left out\n", out.tri.size(), report.droppedCrumbs);
  }

  // Sponge: as full as wall / cell, struts about a wall thick, one closed piece; the tile repeats
  // without a seam and another pattern number gives another sponge.
  {
    Settings s;
    s.kind = Kind::Sponge;
    s.cell = 4;
    s.wall = 1.6;
    const double share = spongeSolidShare(s);
    check(std::abs(share - 0.4) < 1e-9, "sponge: solid share is wall / cell");
    // Along lines: how much is solid, and how long the solid runs are.
    double solid = 0, samples = 0, runs = 0;
    const double step = 0.05;
    for (int line = 0; line < 60; ++line) {
      const double y = line * 1.37, z = line * 2.11;
      bool was = false;
      for (double x = 0; x < 120; x += step) {
        const bool in = patternField(s, vec3(x, y, z)) > 0;
        solid += in;
        samples += 1;
        runs += in && !was;
        was = in;
      }
    }
    const double fill = solid / samples, strut = solid * step / std::max(runs, 1.0);
    check(std::abs(fill - share) < 0.05, "sponge field: solid share " + num(fill) + " vs " + num(share));
    check(std::abs(strut - s.wall) < 0.25 * s.wall, "sponge field: struts about a wall thick (" + num(strut) + " mm)");
    // One wavelength is wall / strut-in-wavelengths; the tile repeats every 8. Find it from the field:
    // the value at p and p + period along x agree only for the true period.
    double period = 0;
    for (double guess = 5; guess < 60 && period == 0; guess += 0.001) {
      bool same = true;
      for (int i = 0; i < 5 && same; ++i) {
        const vec3 p(i * 0.71, i * 1.3, i * 0.37);
        same = std::abs(patternField(s, p) - patternField(s, p + vec3(guess, 0, 0))) < 1e-4;
      }
      if (same) period = guess;
    }
    check(period > 0, "sponge: the tile repeats without a seam (period " + num(period) + " mm)");
    Settings other = s;
    other.seed = 2;
    int differ = 0;
    for (int i = 0; i < 20; ++i) differ += (patternField(s, vec3(i, 0.5 * i, 0)) > 0) != (patternField(other, vec3(i, 0.5 * i, 0)) > 0);
    check(differ > 3, "sponge: another pattern number gives another sponge");
    sculpt::Mesh out;
    std::string error;
    Report report;
    const auto t0 = std::chrono::steady_clock::now();
    check(build(cube, s, &out, &error, &report), "sponge builds (" + error + ")");
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(sculpt::closedManifold(out), "sponge: closed");
    check(sculpt::crossingTriangles(out).empty() && report.clean, "sponge: nothing folded or crossing");
    const double built = sculpt::volume(out) / cubeVolume;
    check(std::abs(built - share) < 0.08, "sponge: fill " + num(built) + " vs about " + num(share));
    std::printf("sponge: %zu triangles at %.2f mm (estimated %.0f), %d crumbs left out, %.1f s\n", out.tri.size(),
                report.detail, estimateTriangles(cube, s), report.droppedCrumbs, took);
  }

  // Sponge walls: walls about as thick as asked, pores about the cell, any ratio; and a strut sponge
  // with walls too thin for its cell says which cell it really gets.
  {
    Settings s;
    s.kind = Kind::SpongeWalls;
    s.cell = 12;
    s.wall = 1.2;
    double solid = 0, samples = 0, runs = 0, holes = 0;
    const double step = 0.02;
    for (int line = 0; line < 40; ++line) {
      const double y = line * 3.37, z = line * 5.11;
      bool was = false;
      for (double x = 0; x < 200; x += step) {
        const bool in = patternField(s, vec3(x, y, z)) > 0;
        solid += in;
        samples += 1;
        runs += in && !was;
        was = in;
      }
    }
    holes = (samples - solid) * step / std::max(runs, 1.0);
    const double wallSeen = solid * step / std::max(runs, 1.0);
    // Lines in every direction cross a wall at a slant: on average over twice its thickness.
    check(std::abs(wallSeen / 2 - s.wall) < 0.2 * s.wall, "sponge walls: walls about as thick as asked (" + num(wallSeen / 2) + " mm)");
    check(holes > 0.6 * s.cell && holes < 1.4 * s.cell, "sponge walls: pores about a cell across (" + num(holes) + " mm)");
    sculpt::Mesh out;
    std::string error;
    Report report;
    s.cell = 8;
    check(build(cube, s, &out, &error, &report), "sponge walls build (" + error + ")");
    check(sculpt::closedManifold(out), "sponge walls: closed");
    check(sculpt::crossingTriangles(out).empty() && report.clean, "sponge walls: nothing folded or crossing");
    const double estimate = estimateTriangles(cube, s);
    check(out.tri.size() > 0.6 * estimate && out.tri.size() < 1.5 * estimate,
          "sponge walls: about as many triangles as estimated (" + std::to_string(out.tri.size()) + " vs " + num(estimate) + ")");
    Settings thin;
    thin.kind = Kind::Sponge;
    thin.cell = 12;
    thin.wall = 1.2;
    check(std::abs(spongeCell(thin) - 4.8) < 1e-9, "a thin-walled strut sponge says its cells come out 4.8 mm");
  }

  // Bubbles: about half solid, closed and clean, no skin between bubbles thinner than the walls
  // allow, and about as many triangles as estimated.
  {
    Settings s;
    s.kind = Kind::Bubbles;
    s.cell = 6;
    s.wall = 1.2;
    // Between two bubbles the solid is either gone or at least 0.6 of a wall thick: along lines, no
    // solid run shorter than a few tenths of a millimetre except where a line just grazes a rim.
    int solidRuns = 0, hairline = 0;
    for (int line = 0; line < 40; ++line) {
      bool was = false;
      double start = 0;
      for (double x = 0; x < 120; x += 0.01) {
        const bool in = patternField(s, vec3(x, line * 1.73, line * 2.41)) > 0;
        if (in && !was) start = x;
        if (!in && was) {
          ++solidRuns;
          hairline += x - start < 0.1;
        }
        was = in;
      }
    }
    check(solidRuns > 100 && hairline < solidRuns * 0.05,
          "bubbles: hardly any hairline skins (" + std::to_string(hairline) + " of " + std::to_string(solidRuns) + ")");
    sculpt::Mesh out;
    std::string error;
    Report report;
    check(build(cube, s, &out, &error, &report), "bubbles build (" + error + ")");
    check(sculpt::closedManifold(out), "bubbles: closed");
    check(sculpt::crossingTriangles(out).empty() && report.clean, "bubbles: nothing folded or crossing");
    const double fill = sculpt::volume(out) / cubeVolume;
    check(fill > 0.35 && fill < 0.7, "bubbles: about half solid (" + num(fill) + ")");
    const double estimate = estimateTriangles(cube, s);
    check(out.tri.size() > 0.6 * estimate && out.tri.size() < 1.5 * estimate,
          "bubbles: about as many triangles as estimated (" + std::to_string(out.tri.size()) + " vs " + num(estimate) + ")");
    Settings other = s;
    other.seed = 2;
    int differ = 0;
    for (int i = 0; i < 40; ++i) differ += (patternField(s, vec3(i * 0.7, 1, 2)) > 0) != (patternField(other, vec3(i * 0.7, 1, 2)) > 0);
    check(differ > 5, "bubbles: another pattern number gives other bubbles");
  }

  // Stopped part way: no result, and it says so.
  {
    Settings s;
    s.kind = Kind::Gyroid;
    s.cell = 10;
    s.wall = 1.5;
    Progress progress;
    progress.cancel = true;
    sculpt::Mesh out;
    std::string error;
    check(!build(cube, s, &out, &error, nullptr, &progress) && out.tri.empty(), "a stopped build gives nothing");
    Progress running;
    build(cube, s, &out, &error, nullptr, &running);
    check(running.fraction > 0.99 && !out.tri.empty(), "progress reaches the end (" + num(running.fraction) + ")");
  }

  // Refused, not attempted.
  {
    Settings s;
    s.cell = 2;
    s.wall = 2;
    sculpt::Mesh out;
    std::string error;
    check(!build(cube, s, &out, &error) && !error.empty(), "walls as thick as the cells are refused");
  }

  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("lattice checks took %.1f s\n", seconds);
  std::printf(failures ? "%d failure(s)\n" : "all lattice checks passed\n", failures);
  return failures ? 1 : 0;
}
