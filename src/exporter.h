#pragma once

#include "evaluator.h"

#include <QString>

// Flat tops and floors that sit within `tolerance` mm of each other are put at exactly one height
// (the height most of their area has). Two parts meant to be flush can end up a few nanometres
// apart; a slicer cutting a layer right at that height then keeps some of the joins and drops
// others, which prints as grooves. All meshes are levelled together; the STL, OBJ and 3MF writers
// do this themselves. Returns how many vertices moved.
size_t levelFlatHeights(const std::vector<CpuMesh*>& meshes, double tolerance = 0.0002);

bool writeStl(const CpuMesh& mesh, const QString& path, QString* error);
bool writeObj(const CpuMesh& mesh, const QString& path, QString* error);
bool writeObj(const std::vector<ExportObject>& objects, const QString& path, QString* error);
bool write3mf(const CpuMesh& mesh, const QString& path, QString* error);
bool write3mf(const std::vector<ExportObject>& objects, const QString& path, QString* error);
// The 3D model inside a 3MF (3D/3dmodel.model); empty if there is nothing to write.
QByteArray model3mf(const std::vector<ExportObject>& objects);
bool writeDxf(const CpuMesh& mesh, const QString& path, QString* error);
bool writeDxf(const std::vector<ExportObject>& objects, const QString& path, QString* error);
// Flat outlines. Each ring is x y pairs, already closed in meaning (the first point is not repeated).
bool writeDxfSlice(const std::vector<std::vector<double>>& loops, const QString& path, QString* error);
