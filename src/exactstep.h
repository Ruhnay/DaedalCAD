#pragma once

// Exact STEP (OpenCASCADE). The design is built a second time, from the same document, as exact
// solids: boxes, cylinders, cones, spheres, domes, tori, tubes and capsules as true surfaces, flat
// shapes (prisms, pyramids, wedges, stars, roofs, gems) with their flat faces, holes (with their
// clearance), groups, intersections, repeats and closed hollows done with exact booleans, and parts
// that came from a STEP file as they were in it. Whatever cannot be exact (text, 2D shapes, STLs,
// generators, threads, bends, fillets made on the mesh, hulls) goes in as flat faces from the mesh,
// the same as the faceted STEP; a part that is all mesh is written from its mesh. The view, the
// other exports and everything else keep using the meshes.
//
// Without OpenCASCADE (DAEDALCAD_WITH_OCCT off) available() is false and the program writes the
// faceted STEP instead.

#include "document.h"
#include "evaluator.h"

#include <QColor>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace exact {

bool available();

struct Report {
  int exact = 0;    // parts built exactly
  int partly = 0;   // exact, with some pieces from their mesh (text on a box, an STL in a group)
  int faceted = 0;  // written from their mesh
  QStringList notes;
  // Per part, what it became and why: "Bracket: exact (6 planes, 2 cylinders)", then indented
  // lines for each piece that came from its mesh and the reason.
  QStringList log;
  // Called while it works (from the thread writeStep runs on): steps done of total, and a log
  // line when one was added (else empty).
  std::function<void(int done, int total, const QString& line)> progress;
};

// Writes the parts of `objects` (what evaluateObjects made for the same doc and selection) to a
// STEP file (AP214, millimetres) with their names and colours.
bool writeStep(const Document& doc, const QStringList& only, const std::vector<ExportObject>& objects, const QString& path,
               QString* error, Report* report = nullptr);

// A part read from a STEP file: its triangles (x y z per corner, three corners per triangle,
// moved so their box is centred on the origin) and where that centre was in the file.
struct ImportedPart {
  QString name;
  QColor color;
  bool hasColor = false;
  int index = 0;  // which solid of the file, for exactSolidOf
  std::vector<float> triangles;
  double centre[3] = {0, 0, 0};
};

bool readStep(const QString& path, std::vector<ImportedPart>* parts, QString* error);

}  // namespace exact
