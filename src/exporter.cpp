#include "exporter.h"

#include <QByteArray>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QList>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace {

bool validIndexedMesh(const CpuMesh& mesh) {
  return mesh.triangles > 0 &&
         mesh.indices.size() == static_cast<size_t>(mesh.triangles) * 3 &&
         mesh.vertices.size() >= 3 && mesh.vertices.size() % 3 == 0;
}

// Written to a temporary file and swapped in when complete, so a failed export never leaves a
// half-written file behind (or destroys the one it was replacing).
bool writeBytes(const QString& path, const QByteArray& data, QString* error) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    if (error) *error = file.errorString();
    return false;
  }
  if (file.write(data) != data.size() || !file.commit()) {
    if (error) *error = file.errorString().isEmpty() ? QStringLiteral("Could not write the whole file.") : file.errorString();
    return false;
  }
  return true;
}

uint32_t crc32(const QByteArray& data) {
  uint32_t crc = 0xffffffffu;
  for (unsigned char byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (crc & 1u ? 0xedb88320u : 0u);
  }
  return crc ^ 0xffffffffu;
}

void appendU16(QByteArray& out, uint16_t value) {
  out.append(static_cast<char>(value & 0xff));
  out.append(static_cast<char>((value >> 8) & 0xff));
}

void appendU32(QByteArray& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.append(static_cast<char>((value >> (8 * i)) & 0xff));
}

// qCompress is a 4-byte size plus a zlib wrapper. ZIP method 8 wants the raw deflate stream.
QByteArray deflateRaw(const QByteArray& data) {
  const QByteArray wrapped = qCompress(data, 6);
  if (wrapped.size() < 10) return {};
  if (static_cast<unsigned char>(wrapped.at(4)) != 0x78) return {};
  return wrapped.mid(6, wrapped.size() - 10);
}

QByteArray zipArchive(const QList<QPair<QString, QByteArray>>& files) {
  QByteArray archive;
  QByteArray central;
  uint32_t offset = 0;
  // 2026-10-06. A zero DOS date is rejected by some unzippers.
  const uint16_t dosTime = 0;
  const uint16_t dosDate = static_cast<uint16_t>(((2026 - 1980) << 9) | (10 << 5) | 6);
  for (const auto& file : files) {
    const QByteArray name = file.first.toUtf8();
    const QByteArray& raw = file.second;
    QByteArray payload = deflateRaw(raw);
    const uint16_t method = payload.isEmpty() ? 0 : 8;
    if (payload.isEmpty()) payload = raw;
    const uint32_t crc = crc32(raw);
    QByteArray local;
    appendU32(local, 0x04034b50u);
    appendU16(local, 20);
    appendU16(local, 0);
    appendU16(local, method);
    appendU16(local, dosTime);
    appendU16(local, dosDate);
    appendU32(local, crc);
    appendU32(local, static_cast<uint32_t>(payload.size()));
    appendU32(local, static_cast<uint32_t>(raw.size()));
    appendU16(local, static_cast<uint16_t>(name.size()));
    appendU16(local, 0);
    local.append(name);
    archive.append(local);
    archive.append(payload);

    QByteArray dir;
    appendU32(dir, 0x02014b50u);
    appendU16(dir, 20);
    appendU16(dir, 20);
    appendU16(dir, 0);
    appendU16(dir, method);
    appendU16(dir, dosTime);
    appendU16(dir, dosDate);
    appendU32(dir, crc);
    appendU32(dir, static_cast<uint32_t>(payload.size()));
    appendU32(dir, static_cast<uint32_t>(raw.size()));
    appendU16(dir, static_cast<uint16_t>(name.size()));
    appendU16(dir, 0);
    appendU16(dir, 0);
    appendU16(dir, 0);
    appendU16(dir, 0);
    appendU32(dir, 0);
    appendU32(dir, offset);
    dir.append(name);
    central.append(dir);
    offset += static_cast<uint32_t>(local.size() + payload.size());
  }
  const uint32_t centralSize = static_cast<uint32_t>(central.size());
  archive.append(central);
  QByteArray end;
  appendU32(end, 0x06054b50u);
  appendU16(end, 0);
  appendU16(end, 0);
  appendU16(end, static_cast<uint16_t>(files.size()));
  appendU16(end, static_cast<uint16_t>(files.size()));
  appendU32(end, centralSize);
  appendU32(end, offset);
  appendU16(end, 0);
  archive.append(end);
  return archive;
}

QByteArray xmlNum(float value) {
  QString text = QString::number(static_cast<double>(value), 'f', 5);
  while (text.contains('.') && (text.endsWith('0') || text.endsWith('.'))) text.chop(1);
  if (text.isEmpty() || text == QLatin1String("-") || text == QLatin1String("-0")) return QByteArray("0");
  return text.toUtf8();
}

struct WeldedMesh {
  std::vector<float> vertices;
  std::vector<uint32_t> indices;
};

struct WeldCell {
  int64_t x = 0;
  int64_t y = 0;
  int64_t z = 0;
  bool operator==(const WeldCell& other) const { return x == other.x && y == other.y && z == other.z; }
};

struct WeldHash {
  size_t operator()(const WeldCell& key) const {
    size_t h = std::hash<int64_t>{}(key.x);
    h ^= std::hash<int64_t>{}(key.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= std::hash<int64_t>{}(key.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
  }
};

int64_t snapExport(float value) {
  return std::llround(static_cast<double>(value) * 100000.0);
}

// The file writes 5 decimal places. Vertices that become the same number must be
// one vertex, or a zero-area sliver is stored twice and the slicer reports a
// non-manifold edge. A coarser grid leaves pairs sitting on either side of a
// cell wall, which is what produced the 7 bad edges.
WeldedMesh weldForExport(const CpuMesh& mesh) {
  WeldedMesh out;
  std::unordered_map<WeldCell, uint32_t, WeldHash> lookup;
  auto add = [&](float x, float y, float z) {
    const WeldCell key{snapExport(x), snapExport(y), snapExport(z)};
    const auto found = lookup.find(key);
    if (found != lookup.end()) return found->second;
    const auto id = static_cast<uint32_t>(out.vertices.size() / 3);
    out.vertices.push_back(static_cast<float>(key.x / 100000.0));
    out.vertices.push_back(static_cast<float>(key.y / 100000.0));
    out.vertices.push_back(static_cast<float>(key.z / 100000.0));
    lookup.emplace(key, id);
    return id;
  };
  auto pushFace = [&](uint32_t a, uint32_t b, uint32_t c) {
    if (a == b || b == c || a == c) return;
    out.indices.push_back(a);
    out.indices.push_back(b);
    out.indices.push_back(c);
  };

  if (validIndexedMesh(mesh)) {
    const int count = static_cast<int>(mesh.indices.size() / 3);
    for (int t = 0; t < count; ++t) {
      uint32_t corner[3];
      bool ok = true;
      for (int i = 0; i < 3; ++i) {
        const uint32_t index = mesh.indices[static_cast<size_t>(t) * 3 + i];
        const size_t base = static_cast<size_t>(index) * 3;
        if (base + 2 >= mesh.vertices.size()) {
          ok = false;
          break;
        }
        corner[i] = add(mesh.vertices[base], mesh.vertices[base + 1], mesh.vertices[base + 2]);
      }
      if (ok) pushFace(corner[0], corner[1], corner[2]);
    }
  }
  return out;
}

// Weight of each exact height among the flat faces: the area of the triangles lying exactly at it.
// The triangles tilted by a nanometre step do not vote (they are long slivers across the join and
// would pull towards whichever side has more of their corners); they are only moved.
void addFlatHeights(const float* a, const float* b, const float* c, std::unordered_map<float, double>* weight) {
  if (a[2] != b[2] || b[2] != c[2]) return;
  const double ux = double(b[0]) - a[0], uy = double(b[1]) - a[1];
  const double vx = double(c[0]) - a[0], vy = double(c[1]) - a[1];
  const double area = std::abs(ux * vy - uy * vx) * 0.5;
  if (area > 0) (*weight)[a[2]] += area;
}

}  // namespace

size_t levelFlatHeights(const std::vector<CpuMesh*>& meshes, double tolerance) {
  std::unordered_map<float, double> weight;
  for (const CpuMesh* mesh : meshes) {
    if (!mesh) continue;
    if (validIndexedMesh(*mesh)) {
      const auto& v = mesh->vertices;
      bool ok = true;
      for (uint32_t index : mesh->indices) ok = ok && size_t(index) * 3 + 2 < v.size();
      if (ok) {
        for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3)
          addFlatHeights(&v[mesh->indices[t] * size_t(3)], &v[mesh->indices[t + 1] * size_t(3)],
                         &v[mesh->indices[t + 2] * size_t(3)], &weight);
        continue;
      }
    }
  }
  if (weight.empty()) return 0;
  // The heaviest height of each cluster is where the cluster goes.
  std::vector<std::pair<float, double>> byWeight(weight.begin(), weight.end());
  std::sort(byWeight.begin(), byWeight.end(), [](const auto& p, const auto& q) {
    return p.second != q.second ? p.second > q.second : p.first < q.first;
  });
  std::vector<float> levels;
  for (const auto& [z, w] : byWeight) {
    (void)w;
    const auto at = std::lower_bound(levels.begin(), levels.end(), z);
    const bool near = (at != levels.end() && std::abs(double(*at) - z) <= tolerance) ||
                      (at != levels.begin() && std::abs(double(*(at - 1)) - z) <= tolerance);
    if (!near) levels.insert(at, z);
  }
  auto level = [&](float z) {
    const auto at = std::lower_bound(levels.begin(), levels.end(), z);
    float best = z;
    double gap = tolerance;
    if (at != levels.end() && std::abs(double(*at) - z) <= gap) {
      best = *at;
      gap = std::abs(double(*at) - z);
    }
    if (at != levels.begin() && std::abs(double(*(at - 1)) - z) < gap) best = *(at - 1);
    return best;
  };

  size_t moved = 0;
  for (CpuMesh* mesh : meshes) {
    if (!mesh) continue;
    for (size_t i = 2; i < mesh->vertices.size(); i += 3) {
      const float z = level(mesh->vertices[i]);
      if (z != mesh->vertices[i]) {
        mesh->vertices[i] = z;
        ++moved;
      }
    }
    if (mesh->hasBounds && moved) {
      mesh->lo[2] = level(mesh->lo[2]);
      mesh->hi[2] = level(mesh->hi[2]);
    }
  }
  return moved;
}

bool writeStl(const CpuMesh& source, const QString& path, QString* error) {
  if (source.triCount() == 0 || !validIndexedMesh(source)) {
    if (error) *error = QStringLiteral("Nothing to export.");
    return false;
  }
  CpuMesh mesh = source;
  levelFlatHeights({&mesh});
  QByteArray data;
  QDataStream stream(&data, QIODevice::WriteOnly);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  QByteArray header(80, '\0');
  const QByteArray title = QByteArrayLiteral("DaedalCAD millimetres");
  for (int i = 0; i < title.size() && i < 80; ++i) header[i] = title[i];
  stream.writeRawData(header.constData(), 80);
  stream << static_cast<quint32>(mesh.triCount());
  for (size_t t = 0; t < mesh.triCount(); ++t) {
    const float* a = mesh.corner(t, 0);
    const float* b = mesh.corner(t, 1);
    const float* c = mesh.corner(t, 2);
    // The facet normal, worked out from the corners (after the heights were levelled).
    const double ux = double(b[0]) - a[0], uy = double(b[1]) - a[1], uz = double(b[2]) - a[2];
    const double vx = double(c[0]) - a[0], vy = double(c[1]) - a[1], vz = double(c[2]) - a[2];
    double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (length > 1e-12) nx /= length, ny /= length, nz /= length;
    else nx = ny = 0, nz = 1;
    stream << static_cast<float>(nx) << static_cast<float>(ny) << static_cast<float>(nz);
    stream << a[0] << a[1] << a[2];
    stream << b[0] << b[1] << b[2];
    stream << c[0] << c[1] << c[2];
    stream << static_cast<quint16>(0);
  }
  return writeBytes(path, data, error);
}

namespace {

QByteArray xmlAttr(const QString& text) {
  QByteArray out;
  const QByteArray utf8 = text.toUtf8();
  for (unsigned char c : utf8) {
    if (c == '&') { out += '&'; out += "amp;"; }
    else if (c == '<') { out += '&'; out += "lt;"; }
    else if (c == '>') { out += '&'; out += "gt;"; }
    else if (c == '"') { out += '&'; out += "quot;"; }
    else out.append(static_cast<char>(c));
  }
  return out;
}

}  // namespace

static std::vector<ExportObject> oneObject(const CpuMesh& mesh) {
  ExportObject object;
  object.name = QStringLiteral("DaedalCAD");
  object.mesh = mesh;
  return {object};
}

// The objects' meshes levelled together (see levelFlatHeights), in the same order.
static std::vector<CpuMesh> levelledMeshes(const std::vector<ExportObject>& objects) {
  std::vector<CpuMesh> meshes;
  meshes.reserve(objects.size());
  for (const ExportObject& object : objects) meshes.push_back(object.mesh);
  std::vector<CpuMesh*> all;
  for (CpuMesh& mesh : meshes) all.push_back(&mesh);
  levelFlatHeights(all);
  return meshes;
}

bool writeObj(const std::vector<ExportObject>& objects, const QString& path, QString* error) {
  if (objects.empty()) {
    if (error) *error = QStringLiteral("Nothing to export.");
    return false;
  }
  // Colours go in a material file beside it (name.mtl), one material per colour.
  const QFileInfo info(path);
  const QString mtlName = info.completeBaseName() + QStringLiteral(".mtl");
  QByteArray materials = "# DaedalCAD colours\n";
  std::vector<QRgb> colours;
  auto materialOf = [&](const QColor& color) {
    for (size_t i = 0; i < colours.size(); ++i)
      if (colours[i] == color.rgb()) return QByteArray("colour") + QByteArray::number(static_cast<qulonglong>(i + 1));
    colours.push_back(color.rgb());
    const QByteArray name = QByteArray("colour") + QByteArray::number(static_cast<qulonglong>(colours.size()));
    materials += "newmtl " + name + "\nKd " + xmlNum(color.redF()) + ' ' + xmlNum(color.greenF()) + ' ' + xmlNum(color.blueF()) +
                 "\nKa 0 0 0\nKs 0 0 0\nd 1\nillum 1\n";
    return name;
  };
  QByteArray text = "# DaedalCAD millimetres\nmtllib " + mtlName.toUtf8() + "\n";
  size_t vertexCount = 0;
  const std::vector<CpuMesh> levelled = levelledMeshes(objects);
  for (size_t index = 0; index < objects.size(); ++index) {
    const ExportObject& object = objects[index];
    const WeldedMesh welded = weldForExport(levelled[index]);
    if (welded.indices.size() < 3 || welded.vertices.size() < 9) continue;
    text += "o ";
    text += object.name.toUtf8();
    text += '\n';
    text += "usemtl " + materialOf(object.color) + '\n';
    for (size_t i = 0; i + 2 < welded.vertices.size(); i += 3) {
      text += "v ";
      text += xmlNum(welded.vertices[i]);
      text += ' ';
      text += xmlNum(welded.vertices[i + 1]);
      text += ' ';
      text += xmlNum(welded.vertices[i + 2]);
      text += '\n';
    }
    const size_t base = vertexCount;
    vertexCount += welded.vertices.size() / 3;
    for (size_t t = 0; t + 2 < welded.indices.size(); t += 3) {
      text += "f ";
      text += QByteArray::number(static_cast<qulonglong>(base + welded.indices[t] + 1));
      text += ' ';
      text += QByteArray::number(static_cast<qulonglong>(base + welded.indices[t + 1] + 1));
      text += ' ';
      text += QByteArray::number(static_cast<qulonglong>(base + welded.indices[t + 2] + 1));
      text += '\n';
    }
  }
  if (vertexCount == 0) {
    if (error) *error = QStringLiteral("Nothing to export.");
    return false;
  }
  if (!writeBytes(info.dir().filePath(mtlName), materials, error)) return false;
  return writeBytes(path, text, error);
}

bool writeObj(const CpuMesh& mesh, const QString& path, QString* error) {
  return writeObj(oneObject(mesh), path, error);
}

QByteArray model3mf(const std::vector<ExportObject>& objects) {
  // Colours: one base material per colour (id 1), each object pointing at its own. The colours of
  // a multi-colour group become the parts of one model (an object made of components), which
  // slicers load as one object whose parts can each get a filament.
  QByteArray colourList;
  std::vector<QRgb> colours;
  auto colourIndex = [&](const QColor& color) {
    for (size_t i = 0; i < colours.size(); ++i)
      if (colours[i] == color.rgb()) return static_cast<int>(i);
    colours.push_back(color.rgb());
    colourList += "<base name=\"Colour " + QByteArray::number(static_cast<qulonglong>(colours.size())) + "\" displaycolor=\"" +
                  color.name(QColor::HexRgb).toUpper().toUtf8() + "FF\"/>\n";
    return static_cast<int>(colours.size() - 1);
  };
  QByteArray bodies;
  QByteArray items;
  int id = 1;  // 1 is the colours
  std::vector<std::pair<QString, std::vector<int>>> assemblies;
  const std::vector<CpuMesh> levelled = levelledMeshes(objects);
  for (size_t index = 0; index < objects.size(); ++index) {
    const ExportObject& object = objects[index];
    const WeldedMesh welded = weldForExport(levelled[index]);
    if (welded.indices.size() < 3 || welded.vertices.size() < 9) continue;
    ++id;
    bodies += "<object id=\"";
    bodies += QByteArray::number(id);
    bodies += "\" type=\"model\" name=\"";
    bodies += xmlAttr(object.name);
    bodies += "\" pid=\"1\" pindex=\"";
    bodies += QByteArray::number(colourIndex(object.color));
    bodies += "\">\n<mesh>\n<vertices>\n";
    for (size_t i = 0; i + 2 < welded.vertices.size(); i += 3) {
      bodies += "<vertex x=\"";
      bodies += xmlNum(welded.vertices[i]);
      bodies += "\" y=\"";
      bodies += xmlNum(welded.vertices[i + 1]);
      bodies += "\" z=\"";
      bodies += xmlNum(welded.vertices[i + 2]);
      bodies += "\"/>\n";
    }
    bodies += "</vertices>\n<triangles>\n";
    for (size_t t = 0; t + 2 < welded.indices.size(); t += 3) {
      bodies += "<triangle v1=\"";
      bodies += QByteArray::number(static_cast<qulonglong>(welded.indices[t]));
      bodies += "\" v2=\"";
      bodies += QByteArray::number(static_cast<qulonglong>(welded.indices[t + 1]));
      bodies += "\" v3=\"";
      bodies += QByteArray::number(static_cast<qulonglong>(welded.indices[t + 2]));
      bodies += "\"/>\n";
    }
    bodies += "</triangles>\n</mesh>\n</object>\n";
    if (!object.assembly.isEmpty()) {
      auto it = std::find_if(assemblies.begin(), assemblies.end(), [&](const auto& a) { return a.first == object.assembly; });
      if (it == assemblies.end()) assemblies.push_back({object.assembly, {id}});
      else it->second.push_back(id);
      continue;
    }
    items += "<item objectid=\"";
    items += QByteArray::number(id);
    items += "\" transform=\"1 0 0 0 1 0 0 0 1 0 0 0\"/>\n";
  }
  for (const auto& [name, parts] : assemblies) {
    ++id;
    bodies += "<object id=\"" + QByteArray::number(id) + "\" type=\"model\" name=\"" + xmlAttr(name) + "\">\n<components>\n";
    for (int part : parts) bodies += "<component objectid=\"" + QByteArray::number(part) + "\"/>\n";
    bodies += "</components>\n</object>\n";
    items += "<item objectid=\"" + QByteArray::number(id) + "\" transform=\"1 0 0 0 1 0 0 0 1 0 0 0\"/>\n";
  }
  if (id == 1) return {};

  QByteArray model;
  model += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  model += "<model unit=\"millimeter\" xml:lang=\"en-US\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n";
  model += "<metadata name=\"Application\">DaedalCAD</metadata>\n";
  model += "<metadata name=\"Title\">DaedalCAD Model</metadata>\n";
  model += "<resources>\n";
  model += "<basematerials id=\"1\">\n" + colourList + "</basematerials>\n";
  model += bodies;
  model += "</resources>\n<build>\n";
  model += items;
  model += "</build>\n</model>\n";
  return model;
}

bool write3mf(const std::vector<ExportObject>& objects, const QString& path, QString* error) {
  const QByteArray model = objects.empty() ? QByteArray() : model3mf(objects);
  if (model.isEmpty()) {
    if (error) *error = QStringLiteral("Nothing to export.");
    return false;
  }

  const QByteArray types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
      "<Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n"
      "<Override PartName=\"/3D/3dmodel.model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n"
      "</Types>\n";
  const QByteArray rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
      "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
      "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>\n"
      "</Relationships>\n";

  const QByteArray zip = zipArchive({
      {QStringLiteral("[Content_Types].xml"), types},
      {QStringLiteral("_rels/.rels"), rels},
      {QStringLiteral("3D/3dmodel.model"), model},
  });
  return writeBytes(path, zip, error);
}

bool write3mf(const CpuMesh& mesh, const QString& path, QString* error) {
  return write3mf(oneObject(mesh), path, error);
}

// ---------------------------------------------------------------------------------------------
// DXF, written as AutoCAD R12 (AC1009): the oldest flavour every CAD and laser program still reads,
// and the only one that needs no handles, block records or objects section. Closed outlines are
// POLYLINE + VERTEX + SEQEND (LWPOLYLINE only exists from R2000 on); the mesh is 3DFACEs.

namespace {

void dxfPair(QByteArray& out, int code, const QByteArray& value) {
  out += QByteArray::number(code);
  out += "\r\n";
  out += value;
  out += "\r\n";
}

QByteArray dxfNum(double value) { return xmlNum(static_cast<float>(value)); }

// R12 layer names: at most 31 characters, upper case letters, digits, $, - and _.
QByteArray dxfLayerName(const QString& name, QList<QByteArray>* taken) {
  QByteArray layer;
  for (const QChar c : name.toUpper()) {
    const ushort u = c.unicode();
    const bool ok = (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '$' || u == '-' || u == '_';
    layer.append(ok ? static_cast<char>(u) : '_');
  }
  while (layer.contains("__")) layer.replace("__", "_");
  while (layer.startsWith('_')) layer.remove(0, 1);
  while (layer.endsWith('_')) layer.chop(1);
  if (layer.isEmpty()) layer = "PART";
  layer.truncate(28);
  QByteArray unique = layer;
  for (int n = 2; taken->contains(unique); ++n) unique = layer.left(28 - QByteArray::number(n).size() - 1) + '_' + QByteArray::number(n);
  taken->append(unique);
  return unique;
}

struct DxfLayer {
  QByteArray name;
  int color = 7;
};

QByteArray dxfDocument(const QList<DxfLayer>& layers, const QByteArray& entities) {
  QByteArray out;
  out.reserve(entities.size() + 600 + layers.size() * 64);
  dxfPair(out, 999, "DaedalCAD, millimetres");
  dxfPair(out, 0, "SECTION");
  dxfPair(out, 2, "HEADER");
  dxfPair(out, 9, "$ACADVER");
  dxfPair(out, 1, "AC1009");
  // Millimetres. Old readers skip variables they do not know; laser and CAM programs use these.
  dxfPair(out, 9, "$INSUNITS");
  dxfPair(out, 70, "4");
  dxfPair(out, 9, "$MEASUREMENT");
  dxfPair(out, 70, "1");
  dxfPair(out, 0, "ENDSEC");
  dxfPair(out, 0, "SECTION");
  dxfPair(out, 2, "TABLES");
  dxfPair(out, 0, "TABLE");
  dxfPair(out, 2, "LTYPE");
  dxfPair(out, 70, "1");
  dxfPair(out, 0, "LTYPE");
  dxfPair(out, 2, "CONTINUOUS");
  dxfPair(out, 70, "0");
  dxfPair(out, 3, "Solid line");
  dxfPair(out, 72, "65");
  dxfPair(out, 73, "0");
  dxfPair(out, 40, "0.0");
  dxfPair(out, 0, "ENDTAB");
  dxfPair(out, 0, "TABLE");
  dxfPair(out, 2, "LAYER");
  dxfPair(out, 70, QByteArray::number(layers.size() + 1));
  QList<DxfLayer> all{{"0", 7}};
  all += layers;
  for (const DxfLayer& layer : all) {
    dxfPair(out, 0, "LAYER");
    dxfPair(out, 2, layer.name);
    dxfPair(out, 70, "0");
    dxfPair(out, 62, QByteArray::number(layer.color));
    dxfPair(out, 6, "CONTINUOUS");
  }
  dxfPair(out, 0, "ENDTAB");
  dxfPair(out, 0, "ENDSEC");
  dxfPair(out, 0, "SECTION");
  dxfPair(out, 2, "ENTITIES");
  out += entities;
  dxfPair(out, 0, "ENDSEC");
  dxfPair(out, 0, "EOF");
  return out;
}

void dxfFace(QByteArray& out, const QByteArray& layer, const float* v, uint32_t a, uint32_t b, uint32_t c) {
  dxfPair(out, 0, "3DFACE");
  dxfPair(out, 8, layer);
  const uint32_t corner[4] = {a, b, c, c};
  const int xCode[4] = {10, 11, 12, 13};
  for (int i = 0; i < 4; ++i) {
    const float* p = v + static_cast<size_t>(corner[i]) * 3;
    dxfPair(out, xCode[i], xmlNum(p[0]));
    dxfPair(out, xCode[i] + 10, xmlNum(p[1]));
    dxfPair(out, xCode[i] + 20, xmlNum(p[2]));
  }
}

void dxfClosedPolyline(QByteArray& out, const QByteArray& layer, const std::vector<double>& loop) {
  dxfPair(out, 0, "POLYLINE");
  dxfPair(out, 8, layer);
  dxfPair(out, 66, "1");
  dxfPair(out, 10, "0");
  dxfPair(out, 20, "0");
  dxfPair(out, 30, "0");
  dxfPair(out, 70, "1");
  for (size_t i = 0; i + 1 < loop.size(); i += 2) {
    dxfPair(out, 0, "VERTEX");
    dxfPair(out, 8, layer);
    dxfPair(out, 10, dxfNum(loop[i]));
    dxfPair(out, 20, dxfNum(loop[i + 1]));
    dxfPair(out, 30, "0");
  }
  dxfPair(out, 0, "SEQEND");
  dxfPair(out, 8, layer);
}

double signedArea(const std::vector<double>& loop) {
  double area = 0;
  const size_t n = loop.size() / 2;
  for (size_t i = 0; i < n; ++i) {
    const size_t j = (i + 1) % n;
    area += loop[2 * i] * loop[2 * j + 1] - loop[2 * j] * loop[2 * i + 1];
  }
  return area * 0.5;
}

}  // namespace

bool writeDxf(const std::vector<ExportObject>& objects, const QString& path, QString* error) {
  QByteArray entities;
  QList<DxfLayer> layers;
  QList<QByteArray> taken{"0"};
  int faces = 0;
  for (const ExportObject& object : objects) {
    const WeldedMesh welded = weldForExport(object.mesh);
    if (welded.indices.size() < 3 || welded.vertices.size() < 9) continue;
    // One layer per part, each in its own colour (1 to 6, then round again).
    const DxfLayer layer{dxfLayerName(object.name, &taken), 1 + static_cast<int>(layers.size()) % 6};
    layers << layer;
    const float* v = welded.vertices.data();
    for (size_t t = 0; t + 2 < welded.indices.size(); t += 3) {
      const uint32_t a = welded.indices[t], b = welded.indices[t + 1], c = welded.indices[t + 2];
      if (a == b || b == c || a == c) continue;
      dxfFace(entities, layer.name, v, a, b, c);
      ++faces;
    }
  }
  if (faces == 0) {
    if (error) *error = QStringLiteral("Nothing to export.");
    return false;
  }
  return writeBytes(path, dxfDocument(layers, entities), error);
}

bool writeDxf(const CpuMesh& mesh, const QString& path, QString* error) {
  return writeDxf(oneObject(mesh), path, error);
}

bool writeDxfSlice(const std::vector<std::vector<double>>& loops, const QString& path, QString* error) {
  // Outer outlines run counter clockwise and holes clockwise (as Manifold slices them), so holes
  // can go on their own layer: laser software often cuts inner contours first.
  QByteArray entities;
  int written = 0;
  int holes = 0;
  for (const std::vector<double>& loop : loops) {
    if (loop.size() < 6 || loop.size() % 2 != 0) continue;
    const bool hole = signedArea(loop) < 0;
    dxfClosedPolyline(entities, hole ? QByteArray("HOLES") : QByteArray("OUTLINE"), loop);
    ++written;
    holes += hole ? 1 : 0;
  }
  if (written == 0) {
    if (error) *error = QStringLiteral("Nothing to export.");
    return false;
  }
  QList<DxfLayer> layers{{"OUTLINE", 7}};
  if (holes > 0) layers << DxfLayer{"HOLES", 1};
  return writeBytes(path, dxfDocument(layers, entities), error);
}
