#pragma once

// STEP export (ISO 10303-21, AP214): every object becomes a closed solid made of flat faces.
//
// DaedalCAD's solids are triangle meshes, so this is a faceted STEP, the same as other mesh
// modellers write: triangles that lie in one plane are joined into one face (with holes where it
// has them), and the straight edges between faces are shared, so CAD programs read a proper solid
// they can measure, cut and use in assemblies. Round surfaces stay as bands of narrow flat faces;
// a hole is not a true cylinder.

#include "evaluator.h"

#include <QByteArray>
#include <QString>

#include <array>
#include <utility>
#include <vector>

namespace step {

// A closed mesh as flat faces: triangles in one plane joined into one face, the straight edges
// between faces shared (every edge is used by two faces, once each way), points along a straight
// edge between the same two faces dropped. What both STEP writers build their solids from.
struct Faceted {
  std::vector<std::array<double, 3>> points;  // the corners
  struct Edge {
    int a, b;  // corner indices; the edge runs from a to b
  };
  std::vector<Edge> edges;
  struct Bound {
    std::vector<std::pair<int, bool>> edges;  // edge index, and whether this loop runs it from a to b
  };
  struct Face {
    std::array<double, 3> origin, normal, u;  // a point on the face's plane, its outward normal, a direction in it
    std::vector<Bound> bounds;                // the first is the outline, the rest are holes in it
  };
  std::vector<Face> faces;
  int triangles = 0;
};

// Works out the faces of a closed mesh (x y z per point, three point indices per triangle). False,
// with a reason, when the mesh is not a closed solid.
bool analyseSolid(const std::vector<double>& points, const std::vector<uint32_t>& triangles, Faceted* out, QString* error);

struct Stats {
  int solids = 0;
  int faces = 0;
  int edges = 0;
  int vertices = 0;
  int triangles = 0;
};

// The whole file. fileName goes in the header; timestamp as "2026-10-22T19:00:00". Uses
// ExportObject::points and triangles (full precision). Empty with an error if an object's mesh is
// not closed.
QByteArray write(const std::vector<ExportObject>& objects, const QString& fileName, const QString& timestamp, Stats* stats,
                 QString* error);

}  // namespace step

bool writeStep(const std::vector<ExportObject>& objects, const QString& path, QString* error, step::Stats* stats = nullptr);
