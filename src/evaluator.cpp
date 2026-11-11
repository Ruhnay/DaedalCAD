#include "evaluator.h"
#include "edgefillet.h"
#include "effects.h"
#include "generators.h"
#include "surfacefillet.h"
#include "textshape.h"

#include <manifold/cross_section.h>
#include <manifold/manifold.h>

#include <QQuaternion>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>
#include <functional>
#include <array>
#include <limits>
#include <map>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <numeric>
#include <atomic>
#include <exception>
#include <future>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <utility>

using manifold::CrossSection;
using manifold::Manifold;
using manifold::Polygons;
using manifold::SimplePolygon;
using manifold::vec2;
using manifold::vec3;

namespace {
std::mutex gFilletLogMutex;
QStringList gFilletLog;
void logFillet(const QString& line) {
  std::lock_guard<std::mutex> lock(gFilletLogMutex);
  gFilletLog.append(line);
}
}

void clearFilletLog() {
  std::lock_guard<std::mutex> lock(gFilletLogMutex);
  gFilletLog.clear();
}

QStringList takeFilletLog() {
  std::lock_guard<std::mutex> lock(gFilletLogMutex);
  QStringList out = gFilletLog;
  gFilletLog.clear();
  return out;
}

namespace {

constexpr double kPi = 3.14159265358979323846;

QString gWarning;

double comp(const vec3& v, int axis) {
  if (axis == 0) return v.x;
  if (axis == 1) return v.y;
  return v.z;
}

void setComp(vec3& v, int axis, double value) {
  if (axis == 0) v.x = value;
  else if (axis == 1) v.y = value;
  else v.z = value;
}

void note(QString* warning, const QString& text) {
  gWarning = text;
  if (warning && warning->isEmpty()) *warning = text;
}

bool healthy(const Manifold& solid) {
  return !solid.IsEmpty() && solid.Status() == Manifold::Error::NoError;
}

// Shells that touch. A solid can hold several closed shells that meet without being joined, or a
// shell that meets itself: each is closed and valid here, but in a file the shared points weld
// together and slicers count edges used by four triangles as non-manifold. Two cases are repaired:
//  - shells that come within a few micrometres of each other are joined with a real union;
//  - a sealed pocket (an inside-out shell) whose wall touches itself is filled, since no slicer
//    can read it and filling it is what a print of it would come closest to. (Imported models do
//    this: a hollow hat whose inner wall folds onto itself at the brim, say.)
// Everything else is left exactly as it was: separate parts, and clean pockets such as Hollow's.
Manifold joinTouchingShells(const Manifold& solid) {
  if (!healthy(solid)) return solid;
  const manifold::MeshGL mesh = solid.GetMeshGL();
  const size_t verts = mesh.NumVert();
  const size_t tris = mesh.NumTri();
  if (verts == 0 || tris == 0) return solid;
  auto pointOf = [&](uint32_t v) { return &mesh.vertProperties[size_t(v) * mesh.numProp]; };
  // Union-find, with path halving; the smaller index becomes the root.
  auto rootIn = [](std::vector<uint32_t>& parent, uint32_t v) {
    while (parent[v] != v) v = parent[v] = parent[parent[v]];
    return v;
  };
  auto joinIn = [&](std::vector<uint32_t>& parent, uint32_t a, uint32_t b) {
    a = rootIn(parent, a), b = rootIn(parent, b);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);
  };
  // The same vertex, as the mesh's merge list splits some (by their properties).
  std::vector<uint32_t> same(verts);
  std::iota(same.begin(), same.end(), 0u);
  for (size_t i = 0; i < mesh.mergeFromVert.size() && i < mesh.mergeToVert.size(); ++i)
    joinIn(same, mesh.mergeFromVert[i], mesh.mergeToVert[i]);
  // Shells: vertices joined by triangles.
  std::vector<uint32_t> parent = same;
  for (size_t i = 0; i + 2 < mesh.triVerts.size(); i += 3) {
    joinIn(parent, mesh.triVerts[i], mesh.triVerts[i + 1]);
    joinIn(parent, mesh.triVerts[i], mesh.triVerts[i + 2]);
  }
  std::vector<uint32_t> shellOf(verts), vertexOf(verts);
  bool several = false;
  for (uint32_t v = 0; v < verts; ++v) {
    shellOf[v] = rootIn(parent, v), vertexOf[v] = rootIn(same, v);
    several |= shellOf[v] != 0;
  }
  if (!several) return solid;  // one shell: nothing to join, and it is no pocket
  // Each shell's box, to look only where two shells could meet.
  std::unordered_map<uint32_t, size_t> shellIndex;
  std::vector<std::array<float, 6>> shellBox;
  std::vector<uint32_t> indexOf(verts);
  for (uint32_t v = 0; v < verts; ++v) {
    const auto [it, fresh] = shellIndex.emplace(shellOf[v], shellBox.size());
    if (fresh) shellBox.push_back({1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f});
    indexOf[v] = static_cast<uint32_t>(it->second);
    std::array<float, 6>& b = shellBox[it->second];
    const float* p = pointOf(v);
    for (int a = 0; a < 3; ++a) b[a] = std::min(b[a], p[a]), b[3 + a] = std::max(b[3 + a], p[a]);
  }
  // Each shell's signed volume (negative: a pocket).
  std::unordered_map<uint32_t, double> volume;
  for (size_t t = 0; t < tris; ++t) {
    const float* a = pointOf(mesh.triVerts[t * 3]);
    const float* b = pointOf(mesh.triVerts[t * 3 + 1]);
    const float* c = pointOf(mesh.triVerts[t * 3 + 2]);
    volume[shellOf[mesh.triVerts[t * 3]]] += (double(a[0]) * (double(b[1]) * c[2] - double(b[2]) * c[1]) -
                                              double(a[1]) * (double(b[0]) * c[2] - double(b[2]) * c[0]) +
                                              double(a[2]) * (double(b[0]) * c[1] - double(b[1]) * c[0])) / 6.0;
  }
  // Points closer than a few micrometres (or a few float steps, far out): written as floats and
  // welded, they become one point. Found through a grid of that size; a cell keeps one point per
  // shell, which is enough to see two shells (or two sheets of one) meet there.
  const manifold::Box box = solid.BoundingBox();
  double far = 1.0;
  for (double v : {box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z}) far = std::max(far, std::abs(v));
  const double reach = std::max(1e-3, far * 4e-6);
  struct Cell {
    long long x, y, z;
    bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
  };
  struct CellHash {
    size_t operator()(const Cell& c) const {
      return size_t(c.x) * 73856093u ^ size_t(c.y) * 19349663u ^ size_t(c.z) * 83492791u;
    }
  };
  std::unordered_map<Cell, std::vector<uint32_t>, CellHash> grid;  // shells with a point in the cell
  std::vector<uint32_t> group(verts);
  std::iota(group.begin(), group.end(), 0u);
  bool touching = false;
  const float margin = static_cast<float>(2 * reach);
  auto nearAnother = [&](uint32_t v) {
    if (shellBox.size() > 64) return true;
    const float* p = pointOf(v);
    for (size_t k = 0; k < shellBox.size(); ++k) {
      if (k == indexOf[v]) continue;
      const std::array<float, 6>& b = shellBox[k];
      if (p[0] >= b[0] - margin && p[1] >= b[1] - margin && p[2] >= b[2] - margin && p[0] <= b[3] + margin &&
          p[1] <= b[4] + margin && p[2] <= b[5] + margin)
        return true;
    }
    return false;
  };
  for (uint32_t v = 0; v < verts; ++v) {
    if (!nearAnother(v)) continue;
    const float* p = pointOf(v);
    const Cell cell{std::llround(p[0] / reach), std::llround(p[1] / reach), std::llround(p[2] / reach)};
    for (long long dx = -1; dx <= 1; ++dx)
      for (long long dy = -1; dy <= 1; ++dy)
        for (long long dz = -1; dz <= 1; ++dz) {
          const auto it = grid.find({cell.x + dx, cell.y + dy, cell.z + dz});
          if (it == grid.end()) continue;
          for (uint32_t other : it->second)
            if (rootIn(group, other) != rootIn(group, shellOf[v])) {
              joinIn(group, other, shellOf[v]);
              touching = true;
            }
        }
    std::vector<uint32_t>& here = grid[cell];
    if (std::find(here.begin(), here.end(), shellOf[v]) == here.end()) here.push_back(shellOf[v]);
  }
  // A shell that meets itself: two different vertices of it at exactly the same place.
  std::unordered_map<uint32_t, bool> pinched;
  {
    struct Key {
      float x, y, z;
      bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash {
      size_t operator()(const Key& k) const {
        uint32_t a, b, c;
        std::memcpy(&a, &k.x, 4), std::memcpy(&b, &k.y, 4), std::memcpy(&c, &k.z, 4);
        return size_t(a) * 73856093u ^ size_t(b) * 19349663u ^ size_t(c) * 83492791u;
      }
    };
    std::unordered_map<Key, uint32_t, KeyHash> at;
    at.reserve(verts);
    for (uint32_t v = 0; v < verts; ++v) {
      if (volume[shellOf[v]] >= 0) continue;  // only pockets are looked at
      const float* p = pointOf(v);
      const auto [it, fresh] = at.emplace(Key{p[0] + 0.f, p[1] + 0.f, p[2] + 0.f}, v);  // (+0: -0 is 0)
      if (!fresh && vertexOf[it->second] != vertexOf[v] && shellOf[it->second] == shellOf[v]) pinched[shellOf[v]] = true;
    }
  }
  // Pockets that meet themselves or each other, touching no solid shell, are filled (left out): a
  // union cannot part them, and their welded edges would still be non-manifold.
  std::unordered_map<uint32_t, int> pocketsIn, solidsIn, pinchedIn;  // per group
  for (const auto& [shell, index] : shellIndex) {
    const uint32_t g = rootIn(group, shell);
    ++(volume[shell] < 0 ? pocketsIn : solidsIn)[g];
    if (pinched.count(shell)) ++pinchedIn[g];
  }
  std::unordered_map<uint32_t, bool> drop;
  for (const auto& [shell, index] : shellIndex) {
    const uint32_t g = rootIn(group, shell);
    if (!solidsIn.count(g) && (pocketsIn[g] > 1 || pinchedIn.count(g))) drop[shell] = true;
  }
  if (!touching && drop.empty()) return solid;
  // One mesh per group (a shell that touches nothing is a group of one); a group of several is
  // joined with a union. The groups are then put back into one solid as they are (not with a
  // union, which would fill the clean pockets).
  std::unordered_map<uint32_t, int> shellsIn;  // group root -> how many shells
  for (const auto& [shell, index] : shellIndex)
    if (!drop.count(shell)) ++shellsIn[rootIn(group, shell)];
  std::unordered_map<uint32_t, size_t> slot;
  std::vector<std::vector<uint32_t>> trisOf;  // groups of several shells only
  std::vector<uint32_t> loose;                // triangles of shells kept as they are
  for (size_t t = 0; t < tris; ++t) {
    const uint32_t shell = shellOf[mesh.triVerts[t * 3]];
    if (drop.count(shell)) continue;
    const uint32_t g = rootIn(group, shell);
    if (shellsIn[g] < 2) {
      loose.push_back(static_cast<uint32_t>(t));
      continue;
    }
    const auto [it, fresh] = slot.emplace(g, trisOf.size());
    if (fresh) trisOf.emplace_back();
    trisOf[it->second].push_back(static_cast<uint32_t>(t));
  }
  // The listed triangles of `mesh` as a mesh of their own (points renumbered, merges kept).
  std::vector<uint32_t> renumber(verts, UINT32_MAX);
  auto meshOf = [&](const std::vector<uint32_t>& list) {
    manifold::MeshGL part;
    part.numProp = 3;
    std::vector<uint32_t> used;
    for (uint32_t t : list)
      for (int c = 0; c < 3; ++c) {
        const uint32_t v = mesh.triVerts[size_t(t) * 3 + c];
        if (renumber[v] == UINT32_MAX) {
          renumber[v] = static_cast<uint32_t>(used.size());
          used.push_back(v);
          for (int a = 0; a < 3; ++a) part.vertProperties.push_back(pointOf(v)[a]);
        }
        part.triVerts.push_back(renumber[v]);
      }
    for (size_t i = 0; i < mesh.mergeFromVert.size() && i < mesh.mergeToVert.size(); ++i) {
      const uint32_t from = renumber[mesh.mergeFromVert[i]], to = renumber[mesh.mergeToVert[i]];
      if (from != UINT32_MAX && to != UINT32_MAX) {
        part.mergeFromVert.push_back(from);
        part.mergeToVert.push_back(to);
      }
    }
    for (uint32_t v : used) renumber[v] = UINT32_MAX;
    return part;
  };
  manifold::MeshGL whole = meshOf(loose);
  for (const std::vector<uint32_t>& list : trisOf) {
    const Manifold piece(meshOf(list));
    if (!healthy(piece)) return solid;
    const Manifold joined = Manifold::BatchBoolean(piece.Decompose(), manifold::OpType::Add);
    if (!healthy(joined)) return solid;
    const manifold::MeshGL part = joined.GetMeshGL();
    const uint32_t base = static_cast<uint32_t>(whole.vertProperties.size() / 3);
    for (size_t v = 0; v < part.NumVert(); ++v)
      for (int a = 0; a < 3; ++a) whole.vertProperties.push_back(part.vertProperties[v * part.numProp + a]);
    for (uint32_t i : part.triVerts) whole.triVerts.push_back(base + i);
    for (size_t i = 0; i < part.mergeFromVert.size() && i < part.mergeToVert.size(); ++i) {
      whole.mergeFromVert.push_back(base + part.mergeFromVert[i]);
      whole.mergeToVert.push_back(base + part.mergeToVert[i]);
    }
  }
  if (whole.triVerts.empty()) return solid;
  Manifold result(whole);
  return healthy(result) ? result : solid;
}

int segmentsOf(int quality) {
  return std::clamp(quality, 8, 256);
}

QQuaternion eulerXYZ(const double euler[3]) {
  const QQuaternion qx = QQuaternion::fromAxisAndAngle(1, 0, 0, euler[0]);
  const QQuaternion qy = QQuaternion::fromAxisAndAngle(0, 1, 0, euler[1]);
  const QQuaternion qz = QQuaternion::fromAxisAndAngle(0, 0, 1, euler[2]);
  return qz * qy * qx;
}

CpuMesh meshFrom(const Manifold& solid) {
  CpuMesh mesh;
  if (!healthy(solid)) return mesh;
  const auto gl = solid.GetMeshGL();
  const uint32_t stride = std::max<uint32_t>(3, gl.numProp);
  if (gl.triVerts.size() < 3 || gl.vertProperties.size() < stride) return mesh;

  mesh.triangles = static_cast<int>(gl.triVerts.size() / 3);
  mesh.volume = solid.Volume();

  // Keep the exact Manifold topology for indexed exports. MeshGL can duplicate
  // vertices when vertex properties (for example normals) need different values;
  // mergeFromVert/mergeToVert explicitly describe which of those duplicates are
  // the same topological vertex. Resolve those merges with a tiny union-find.
  const size_t vertexCount = gl.vertProperties.size() / stride;
  std::vector<uint32_t> parent(vertexCount);
  for (uint32_t i = 0; i < static_cast<uint32_t>(vertexCount); ++i) parent[i] = i;

  auto findRoot = [&](uint32_t value) {
    uint32_t root = value;
    while (root < parent.size() && parent[root] != root) root = parent[root];
    while (value < parent.size() && parent[value] != value) {
      const uint32_t next = parent[value];
      parent[value] = root;
      value = next;
    }
    return root;
  };
  auto unite = [&](uint32_t a, uint32_t b) {
    if (a >= parent.size() || b >= parent.size()) return;
    a = findRoot(a);
    b = findRoot(b);
    if (a != b) parent[a] = b;
  };
  const size_t mergeCount = std::min(gl.mergeFromVert.size(), gl.mergeToVert.size());
  for (size_t i = 0; i < mergeCount; ++i) unite(gl.mergeFromVert[i], gl.mergeToVert[i]);
  for (uint32_t i = 0; i < static_cast<uint32_t>(vertexCount); ++i) parent[i] = findRoot(i);

  std::vector<int32_t> canonicalIndex(vertexCount, -1);
  mesh.vertices.reserve(vertexCount * 3);
  for (uint32_t i = 0; i < static_cast<uint32_t>(vertexCount); ++i) {
    const uint32_t root = parent[i];
    if (canonicalIndex[root] >= 0) continue;
    const size_t base = static_cast<size_t>(root) * stride;
    canonicalIndex[root] = static_cast<int32_t>(mesh.vertices.size() / 3);
    mesh.vertices.push_back(gl.vertProperties[base]);
    mesh.vertices.push_back(gl.vertProperties[base + 1]);
    mesh.vertices.push_back(gl.vertProperties[base + 2]);
  }

  mesh.indices.reserve(gl.triVerts.size());
  for (uint32_t index : gl.triVerts) {
    if (index >= parent.size()) {
      mesh.indices.clear();
      mesh.vertices.clear();
      mesh.triangles = 0;
      return mesh;
    }
    const int32_t mapped = canonicalIndex[parent[index]];
    if (mapped < 0) {
      mesh.indices.clear();
      mesh.vertices.clear();
      mesh.triangles = 0;
      return mesh;
    }
    mesh.indices.push_back(static_cast<uint32_t>(mapped));
  }

  mesh.updateBounds();

  return mesh;
}

struct StlTriangle {
  vec3 a;
  vec3 b;
  vec3 c;
};

bool parseStl(const QString& path, std::vector<StlTriangle>* tris, QString* warning) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    note(warning, QString("Could not open STL: %1").arg(file.errorString()));
    return false;
  }
  const QByteArray data = file.readAll();
  if (data.size() < 84) {
    note(warning, QStringLiteral("STL file is too small."));
    return false;
  }
  auto readU32 = [&](int off) -> uint32_t {
    return static_cast<uint32_t>(static_cast<unsigned char>(data[off])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[off + 3])) << 24);
  };
  auto readF32 = [&](int off) -> float {
    uint32_t bits = readU32(off);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  };
  const uint32_t count = readU32(80);
  const qint64 expected = 84 + static_cast<qint64>(count) * 50;
  bool binary = expected == data.size() && count > 0;
  if (binary) {
    tris->reserve(count);
    int off = 84;
    for (uint32_t i = 0; i < count; ++i, off += 50) {
      const vec3 a(readF32(off + 12), readF32(off + 16), readF32(off + 20));
      const vec3 b(readF32(off + 24), readF32(off + 28), readF32(off + 32));
      const vec3 c(readF32(off + 36), readF32(off + 40), readF32(off + 44));
      if (std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z) &&
          std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z) &&
          std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z))
        tris->push_back({a, b, c});
    }
  } else {
    QString text = QString::fromUtf8(data);
    QTextStream in(&text, QIODevice::ReadOnly);
    std::vector<vec3> verts;
    while (!in.atEnd()) {
      const QString line = in.readLine().trimmed();
      if (!line.startsWith(QStringLiteral("vertex"), Qt::CaseInsensitive)) continue;
      const QStringList p = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
      if (p.isEmpty() || p[0].compare(QStringLiteral("vertex"), Qt::CaseInsensitive) != 0) continue;
      if (p.size() < 4) continue;
      bool okx = false, oky = false, okz = false;
      const double x = p[1].toDouble(&okx);
      const double y = p[2].toDouble(&oky);
      const double z = p[3].toDouble(&okz);
      if (okx && oky && okz) verts.push_back(vec3(x, y, z));
      if (verts.size() == 3) {
        tris->push_back({verts[0], verts[1], verts[2]});
        verts.clear();
      }
    }
  }
  if (tris->empty()) {
    note(warning, QStringLiteral("No triangles were found in that STL."));
    return false;
  }
  return true;
}

Manifold loadStlSolid(const QString& path, QString* warning) {
  std::vector<StlTriangle> tris;
  if (!parseStl(path, &tris, warning)) return {};
  double minX = 1e300, minY = 1e300, minZ = 1e300;
  double maxX = -1e300, maxY = -1e300, maxZ = -1e300;
  double signedVolume = 0;
  for (const StlTriangle& t : tris) {
    const vec3 pts[3] = {t.a, t.b, t.c};
    for (const vec3& p : pts) {
      minX = std::min(minX, p.x); minY = std::min(minY, p.y); minZ = std::min(minZ, p.z);
      maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y); maxZ = std::max(maxZ, p.z);
    }
    signedVolume += t.a.x * (t.b.y * t.c.z - t.b.z * t.c.y) -
                    t.a.y * (t.b.x * t.c.z - t.b.z * t.c.x) +
                    t.a.z * (t.b.x * t.c.y - t.b.y * t.c.x);
  }
  const vec3 center((minX + maxX) * 0.5, (minY + maxY) * 0.5, (minZ + maxZ) * 0.5);
  if (signedVolume < 0) {
    for (StlTriangle& t : tris) std::swap(t.b, t.c);
  }
  manifold::MeshGL mesh;
  mesh.numProp = 3;
  mesh.vertProperties.reserve(tris.size() * 9);
  mesh.triVerts.reserve(tris.size() * 3);
  struct Key { long long x, y, z; bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; } };
  struct Hash { size_t operator()(const Key& k) const { return std::hash<long long>()(k.x) ^ (std::hash<long long>()(k.y) << 1) ^ (std::hash<long long>()(k.z) << 2); } };
  std::unordered_map<Key, uint32_t, Hash> canonical;
  auto keyOf = [](const vec3& p) -> Key { return {std::llround(p.x * 1e6), std::llround(p.y * 1e6), std::llround(p.z * 1e6)}; };
  for (const StlTriangle& t : tris) {
    const vec3 ps[3] = {t.a, t.b, t.c};
    const uint32_t base = static_cast<uint32_t>(mesh.vertProperties.size() / 3);
    for (int c = 0; c < 3; ++c) {
      const vec3 p = vec3(ps[c].x - center.x, ps[c].y - center.y, ps[c].z - center.z);
      mesh.vertProperties.push_back(static_cast<float>(p.x));
      mesh.vertProperties.push_back(static_cast<float>(p.y));
      mesh.vertProperties.push_back(static_cast<float>(p.z));
      const uint32_t idx = base + static_cast<uint32_t>(c);
      mesh.triVerts.push_back(idx);
      const Key key = keyOf(p);
      const auto it = canonical.find(key);
      if (it == canonical.end()) canonical.emplace(key, idx);
      else { mesh.mergeFromVert.push_back(idx); mesh.mergeToVert.push_back(it->second); }
    }
  }
  Manifold solid(mesh);
  if (!healthy(solid)) {
    note(warning, QStringLiteral("That STL is not a closed manifold solid and could not be imported as a solid."));
    return {};
  }
  return joinTouchingShells(solid);
}

// A cut gem: a flat top (the table), a short crown down to the widest ring (the girdle), and a
// longer pointed pavilion below, like a brilliant seen from the side.
Manifold gemSolid(const Node& node, double z) {
  const int sides = std::clamp(node.sides, 4, 256);
  const double rx = std::max(node.size[0], 0.05) * 0.5;
  const double ry = std::max(node.size[1], 0.05) * 0.5;
  const double hz = std::max(z, 0.05) * 0.5;
  const double table = std::clamp(node.gemTable, 0.05, 0.95);
  const double girdleZ = hz - 2 * hz * 0.35;  // crown is 35% of the height
  manifold::MeshGL64 mesh;
  mesh.numProp = 3;
  auto add = [&](double x, double y, double zz) {
    const uint64_t index = mesh.vertProperties.size() / 3;
    mesh.vertProperties.insert(mesh.vertProperties.end(), {x, y, zz});
    return index;
  };
  const uint64_t topCenter = add(0, 0, hz);
  const uint64_t bottom = add(0, 0, -hz);
  const double phase = sides == 4 ? kPi * 0.25 : 0.0;
  std::vector<uint64_t> tableRing, girdle;
  for (int i = 0; i < sides; ++i) {
    const double a = (kPi * 2.0 * i) / sides + phase;
    tableRing.push_back(add(std::cos(a) * rx * table, std::sin(a) * ry * table, hz));
  }
  for (int i = 0; i < sides; ++i) {
    const double a = (kPi * 2.0 * i) / sides + phase;
    girdle.push_back(add(std::cos(a) * rx, std::sin(a) * ry, girdleZ));
  }
  for (int i = 0; i < sides; ++i) {
    const int j = (i + 1) % sides;
    mesh.triVerts.insert(mesh.triVerts.end(), {topCenter, tableRing[i], tableRing[j]});
    mesh.triVerts.insert(mesh.triVerts.end(), {tableRing[i], girdle[i], girdle[j]});
    mesh.triVerts.insert(mesh.triVerts.end(), {tableRing[i], girdle[j], tableRing[j]});
    mesh.triVerts.insert(mesh.triVerts.end(), {bottom, girdle[j], girdle[i]});
  }
  Manifold solid(mesh);
  return healthy(solid) ? solid : Manifold();
}

// A star with `points` points between the outer radius and the inner radius (the notches). Pointed:
// a flat bottom and faces rising to a peak in the middle. Flat: the outline extruded.
SimplePolygon starOutline(int points, double outer, double inner) {
  SimplePolygon poly;
  for (int i = 0; i < 2 * points; ++i) {
    const double r = (i % 2 == 0) ? outer : inner;
    const double a = kPi / 2 + kPi * i / points;  // first point straight up (+Y)
    poly.push_back(vec2(std::cos(a) * r, std::sin(a) * r));
  }
  return poly;
}

Manifold starSolid(const Node& node, double z, bool pointed) {
  const int points = std::clamp(node.sides, 3, 64);
  const double outer = std::max(node.radius, 0.1);
  const double inner = std::clamp(node.radius2, 0.05, outer * 0.98);
  const SimplePolygon outline = starOutline(points, outer, inner);
  const double hz = std::max(z, 0.05) * 0.5;
  if (!pointed) return Manifold::Extrude(Polygons{outline}, 2 * hz).Translate(vec3(0, 0, -hz));
  manifold::MeshGL64 mesh;
  mesh.numProp = 3;
  auto add = [&](double x, double y, double zz) {
    const uint64_t index = mesh.vertProperties.size() / 3;
    mesh.vertProperties.insert(mesh.vertProperties.end(), {x, y, zz});
    return index;
  };
  const uint64_t peak = add(0, 0, hz);
  const uint64_t base = add(0, 0, -hz);
  std::vector<uint64_t> ring;
  for (const vec2& p : outline) ring.push_back(add(p.x, p.y, -hz));
  const size_t n = ring.size();
  for (size_t i = 0; i < n; ++i) {
    const uint64_t a = ring[i], b = ring[(i + 1) % n];
    mesh.triVerts.insert(mesh.triVerts.end(), {peak, a, b});
    mesh.triVerts.insert(mesh.triVerts.end(), {base, b, a});
  }
  Manifold solid(mesh);
  return healthy(solid) ? solid : Manifold();
}

// A gable roof: a triangle across X (the width) rising to a ridge along Y.
Manifold roofSolid(double x, double y, double z) {
  const SimplePolygon tri{vec2(-x * 0.5, 0), vec2(x * 0.5, 0), vec2(0, z)};
  return Manifold::Extrude(Polygons{tri}, y).Rotate(90, 0, 0).Translate(vec3(0, y * 0.5, -z * 0.5));
}

Manifold diamondSolid(const Node& node, double z) {
  if (node.gemTable > 0.001) return gemSolid(node, z);
  const int sides = std::clamp(node.sides, 4, 256);
  const double rx = std::max(node.size[0], 0.05) * 0.5;
  const double ry = std::max(node.size[1], 0.05) * 0.5;
  const double hz = std::max(z, 0.05) * 0.5;

  // A faceted biconical/gem solid: the side count controls the number of
  // facets around the girdle. Four sides produces a classic diamond outline;
  // higher counts make the girdle increasingly round while keeping the two
  // pointed ends.
  manifold::MeshGL mesh;
  mesh.numProp = 3;
  mesh.vertProperties.reserve(static_cast<size_t>(sides + 2) * 3);
  mesh.triVerts.reserve(static_cast<size_t>(sides) * 6);
  auto addVertex = [&](double x, double y, double zz) -> uint32_t {
    const uint32_t index = static_cast<uint32_t>(mesh.vertProperties.size() / 3);
    mesh.vertProperties.push_back(static_cast<float>(x));
    mesh.vertProperties.push_back(static_cast<float>(y));
    mesh.vertProperties.push_back(static_cast<float>(zz));
    return index;
  };
  const uint32_t top = addVertex(0, 0, hz);
  const uint32_t bottom = addVertex(0, 0, -hz);
  std::vector<uint32_t> ring;
  ring.reserve(static_cast<size_t>(sides));
  const double phase = sides == 4 ? kPi * 0.25 : 0.0;
  for (int i = 0; i < sides; ++i) {
    const double angle = (kPi * 2.0 * i) / sides + phase;
    ring.push_back(addVertex(std::cos(angle) * rx, std::sin(angle) * ry, 0));
  }
  for (int i = 0; i < sides; ++i) {
    const uint32_t a = ring[static_cast<size_t>(i)];
    const uint32_t b = ring[static_cast<size_t>((i + 1) % sides)];
    mesh.triVerts.push_back(top);
    mesh.triVerts.push_back(a);
    mesh.triVerts.push_back(b);
    mesh.triVerts.push_back(bottom);
    mesh.triVerts.push_back(b);
    mesh.triVerts.push_back(a);
  }
  Manifold solid(mesh);
  if (!healthy(solid)) return {};
  return solid;
}

Manifold heartSolid(const Node& node, int quality) {
  const double w = std::max(node.size[0], 0.05);
  const double h = std::max(node.size[1], 0.05);
  const double z = std::max(node.size[2], 0.05);
  const int samples = std::clamp(segmentsOf(quality) * 3, 72, 384);
  struct P { double x = 0; double y = 0; };
  std::vector<P> raw;
  raw.reserve(static_cast<size_t>(samples));
  double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
  for (int i = 0; i < samples; ++i) {
    const double t = (kPi * 2.0 * i) / samples;
    const double s = std::sin(t);
    const P point{16.0 * s * s * s,
                   13.0 * std::cos(t) - 5.0 * std::cos(2.0 * t) - 2.0 * std::cos(3.0 * t) - std::cos(4.0 * t)};
    raw.push_back(point);
    minX = std::min(minX, point.x);
    maxX = std::max(maxX, point.x);
    minY = std::min(minY, point.y);
    maxY = std::max(maxY, point.y);
  }
  const double spanX = std::max(maxX - minX, 1e-6);
  const double spanY = std::max(maxY - minY, 1e-6);
  const double midX = (minX + maxX) * 0.5;
  const double midY = (minY + maxY) * 0.5;
  SimplePolygon poly;
  poly.reserve(raw.size());
  for (const P& point : raw) {
    poly.push_back(vec2((point.x - midX) / spanX * w, (point.y - midY) / spanY * h));
  }
  double area = 0.0;
  for (size_t i = 0; i < poly.size(); ++i) {
    const vec2& a = poly[i];
    const vec2& b = poly[(i + 1) % poly.size()];
    area += a.x * b.y - b.x * a.y;
  }
  if (area < 0.0) std::reverse(poly.begin(), poly.end());
  try {
    const Manifold solid = Manifold::Extrude(Polygons{poly}, z, 0, 0.0, vec2(1, 1));
    return healthy(solid) ? solid.Translate(vec3(0, 0, -z / 2)) : Manifold();
  } catch (const std::exception&) {
    return {};
  }
}

CrossSection filletInsideCorners(CrossSection shape, double radius) {
  if (shape.IsEmpty() || !(radius > 0.02)) return shape;
  const Polygons rings = shape.ToPolygons();
  if (rings.empty()) return shape;
  const int arcSegs = 16;
  auto ringArea = [](const SimplePolygon& ring) {
    double area = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
      const vec2& a = ring[i];
      const vec2& c = ring[(i + 1) % ring.size()];
      area += a.x * c.y - c.x * a.y;
    }
    return std::abs(area) * 0.5;
  };
  Polygons out;
  out.reserve(rings.size());
  for (const SimplePolygon& ring : rings) {
    SimplePolygon source;
    source.reserve(ring.size());
    for (const vec2& point : ring) {
      if (!source.empty()) {
        const vec2& last = source.back();
        if (std::hypot(point.x - last.x, point.y - last.y) < 0.2) continue;
      }
      source.push_back(point);
    }
    if (source.size() >= 2) {
      const vec2& a = source.front();
      const vec2& b = source.back();
      if (std::hypot(a.x - b.x, a.y - b.y) < 0.2) source.pop_back();
    }
    SimplePolygon simplified;
    const int rawCount = static_cast<int>(source.size());
    for (int i = 0; i < rawCount; ++i) {
      const vec2& prev = source[static_cast<size_t>((i + rawCount - 1) % rawCount)];
      const vec2& cur = source[static_cast<size_t>(i)];
      const vec2& next = source[static_cast<size_t>((i + 1) % rawCount)];
      double ix = cur.x - prev.x;
      double iy = cur.y - prev.y;
      double ox = next.x - cur.x;
      double oy = next.y - cur.y;
      const double il = std::hypot(ix, iy);
      const double ol = std::hypot(ox, oy);
      if (il < 1e-6 || ol < 1e-6) continue;
      ix /= il;
      iy /= il;
      ox /= ol;
      oy /= ol;
      const double turn = ix * oy - iy * ox;
      const double ahead = ix * ox + iy * oy;
      if (std::abs(turn) < 0.06 && ahead > 0.0) continue;
      simplified.push_back(cur);
    }
    if (simplified.size() >= 3) source = std::move(simplified);
    const int count = static_cast<int>(source.size());
    if (count < 3) {
      out.push_back(ring);
      continue;
    }
    struct Arc {
      bool live = false;
      double ang0 = 0;
      double sweep = 0;
      double rad = 0;
      double cx = 0;
      double cy = 0;
    };
    std::vector<Arc> arcs(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      const vec2 prev = source[static_cast<size_t>((i + count - 1) % count)];
      const vec2 cur = source[static_cast<size_t>(i)];
      const vec2 next = source[static_cast<size_t>((i + 1) % count)];
      double ix = cur.x - prev.x;
      double iy = cur.y - prev.y;
      double ox = next.x - cur.x;
      double oy = next.y - cur.y;
      const double il = std::hypot(ix, iy);
      const double ol = std::hypot(ox, oy);
      if (il < 1e-6 || ol < 1e-6) continue;
      ix /= il;
      iy /= il;
      ox /= ol;
      oy /= ol;
      const double cross = ix * oy - iy * ox;
      // Every ring is walked with the solid on the left, holes included.
      // A right turn is an inside corner. Shallow turns are arc facets
      // from the offset pass and must stay as they are.
      if (cross > -0.50) continue;
      if (il < radius * 0.35 && ol < radius * 0.35) continue;
      double r = radius;
      for (int attempt = 0; attempt < 6; ++attempt) {
        const double n0x = -iy;
        const double n0y = ix;
        const double n1x = -oy;
        const double n1y = ox;
        const double nd = std::clamp(n0x * n1x + n0y * n1y, -0.92, 0.995);
        if (1.0 + nd < 0.08) break;
        const double k = r / (1.0 + nd);
        const double cx = cur.x - (n0x + n1x) * k;
        const double cy = cur.y - (n0y + n1y) * k;
        const double t0x = cx + n0x * r;
        const double t0y = cy + n0y * r;
        const double t1x = cx + n1x * r;
        const double t1y = cy + n1y * r;
        const double a0 = std::atan2(t0y - cy, t0x - cx);
        const double a1 = std::atan2(t1y - cy, t1x - cx);
        double sweep = a1 - a0;
        while (sweep > kPi) sweep -= 2.0 * kPi;
        while (sweep <= -kPi) sweep += 2.0 * kPi;
        if (sweep > -0.02 || sweep < -(kPi + 0.08)) {
          r *= 0.5;
          if (r < 0.08) break;
          continue;
        }
        const double need0 = std::hypot(cur.x - t0x, cur.y - t0y);
        const double need1 = std::hypot(cur.x - t1x, cur.y - t1y);
        if (need0 > il * 0.45 || need1 > ol * 0.45) {
          r *= 0.5;
          if (r < 0.08) break;
          continue;
        }
        Arc arc;
        arc.live = true;
        arc.ang0 = a0;
        arc.sweep = sweep;
        arc.rad = r;
        arc.cx = cx;
        arc.cy = cy;
        arcs[static_cast<size_t>(i)] = arc;
        break;
      }
    }
    bool any = false;
    for (const Arc& arc : arcs)
      if (arc.live) any = true;
    if (!any) {
      out.push_back(ring);
      continue;
    }
    SimplePolygon poly;
    for (int i = 0; i < count; ++i) {
      const Arc& arc = arcs[static_cast<size_t>(i)];
      if (!arc.live) {
        poly.push_back(source[static_cast<size_t>(i)]);
        continue;
      }
      const int steps = std::clamp(static_cast<int>(std::lround(arcSegs * std::abs(arc.sweep) / (kPi * 0.5))), 3, arcSegs);
      for (int step = 0; step <= steps; ++step) {
        const double ang = arc.ang0 + arc.sweep * (static_cast<double>(step) / steps);
        const vec2 point(arc.cx + std::cos(ang) * arc.rad, arc.cy + std::sin(ang) * arc.rad);
        if (!poly.empty()) {
          const vec2& last = poly.back();
          if (std::hypot(point.x - last.x, point.y - last.y) < 1e-5) continue;
        }
        poly.push_back(point);
      }
    }
    if (poly.size() >= 2) {
      const vec2& first = poly.front();
      const vec2& last = poly.back();
      if (std::hypot(first.x - last.x, first.y - last.y) < 1e-5) poly.pop_back();
    }
    const double before = std::max(ringArea(source), 1e-6);
    const double after = poly.size() >= 3 ? ringArea(poly) : 0;
    if (poly.size() < 3 || after < before * 0.5 || after > before * 2.0) {
      out.push_back(ring);
      continue;
    }
    out.push_back(std::move(poly));
  }
  try {
    CrossSection built(out);
    if (built.IsEmpty()) return shape;
    return built;
  } catch (const std::exception&) {
    return shape;
  }
}

CrossSection roundSection(CrossSection shape, double edgeR, double surfaceR, int segs, QString* warning) {
  if (shape.IsEmpty()) return shape;
  // A surface fillet: roll one circle around each
  // contour of the face, holes included. OpenSCAD writes that as an offset
  // pair, and the direction picks the corner.
  //   outside tips: shrink with a sharp corner, then grow back round
  //   inside valleys and every hole corner: grow sharp, then shrink back round
  // Outside runs first, so those arcs stay put while the inside pass fills
  // the valleys. A per-vertex arc used to throw the whole ring away when one
  // tip did not fit, which left the valley sharp. The lip then mitered that
  // sharp valley into a square step.
  const double insideR = surfaceR > 0.02 ? surfaceR : edgeR;
  if (!(edgeR > 0.02) && !(insideR > 0.02)) return shape;
  const int arcSegs = std::clamp(std::max(segs, 16), 16, 32);
  const CrossSection saved = shape;
  const int originalRings = static_cast<int>(saved.ToPolygons().size());
  bool reduced = false;
  auto fits = [](const CrossSection& back, const CrossSection& before) {
    if (back.IsEmpty()) return false;
    const double beforeArea = std::max(before.Area(), 1e-6);
    const double area = back.Area();
    if (area < beforeArea * 0.4 || area > beforeArea * 1.8) return false;
    return static_cast<int>(back.ToPolygons().size()) == static_cast<int>(before.ToPolygons().size());
  };
  auto roundTrip = [&](CrossSection profile, double radius, bool inside) {
    if (!(radius > 0.02) || profile.IsEmpty()) return profile;
    double r = radius;
    for (int attempt = 0; attempt < 6; ++attempt) {
      try {
        const CrossSection mid = inside ? profile.Offset(r, CrossSection::JoinType::Miter, 8.0, arcSegs)
                                        : profile.Offset(-r, CrossSection::JoinType::Miter, 8.0, arcSegs);
        if (mid.IsEmpty() || mid.Area() < std::max(profile.Area(), 1e-6) * 0.02) {
          r *= 0.5;
          if (r < 0.08) break;
          continue;
        }
        const CrossSection backRaw = inside ? mid.Offset(-r, CrossSection::JoinType::Round, 2.0, arcSegs)
                                            : mid.Offset(r, CrossSection::JoinType::Round, 2.0, arcSegs);
        CrossSection back = backRaw;
        try {
          CrossSection clean = backRaw.Simplify(0.05);
          if (!clean.IsEmpty() && clean.Area() > std::max(profile.Area(), 1e-6) * 0.2) back = std::move(clean);
        } catch (const std::exception&) {
        }
        if (!fits(back, profile)) {
          r *= 0.5;
          if (r < 0.08) break;
          continue;
        }
        if (r + 0.05 < radius) reduced = true;
        return back;
      } catch (const std::exception&) {
        r *= 0.5;
        if (r < 0.08) break;
      }
    }
    note(warning, inside ? QStringLiteral("Inside corner fillet did not fit, so those corners were left sharp.")
                         : QStringLiteral("Outside corner fillet did not fit, so those corners were left sharp."));
    return profile;
  };
  try {
    if (edgeR > 0.02) shape = roundTrip(std::move(shape), edgeR, false);
    if (insideR > 0.02) shape = roundTrip(std::move(shape), insideR, true);
    shape = filletInsideCorners(std::move(shape), insideR);
  } catch (const std::exception&) {
    note(warning, QStringLiteral("Profile fillet did not fit this outline, so it was left sharp."));
    return saved;
  }
  if (shape.IsEmpty() || static_cast<int>(shape.ToPolygons().size()) < originalRings ||
      shape.Area() < std::max(saved.Area(), 1e-6) * 0.25) {
    note(warning, QStringLiteral("Profile fillet did not fit this outline, so it was left sharp."));
    return saved;
  }
  if (reduced) note(warning, QStringLiteral("Fillet radius was reduced so it would fit this profile."));
  return shape;
}

Manifold extrudeLip(const CrossSection& profileIn, double height, double lipR, int segs) {
  CrossSection profile = profileIn;
  if (lipR > 0.02) {
    try {
      CrossSection rounded = filletInsideCorners(profile, lipR);
      if (!rounded.IsEmpty() && rounded.Area() > std::max(profile.Area(), 1e-6) * 0.25) profile = std::move(rounded);
    } catch (const std::exception&) {
    }
  }
  const Polygons plain = profile.ToPolygons();
  if (plain.empty() || !(height > 0.02)) return {};
  auto plainExtrude = [&]() { return Manifold::Extrude(plain, height, 0, 0.0, vec2(1, 1)); };
  if (!(lipR > 0.02) || height < lipR * 2.4) return plainExtrude();
  double r = std::min(lipR, height * 0.45);
  int guard = 0;
  // The outline fillet has already rounded inside and outside corners.
  // A round join here would run the outside arc through any corner that is
  // still sharp and cut a notch. A miter follows the rounded profile instead.
  auto insetProfile = [&](double amount) {
    return profile.Offset(-amount, CrossSection::JoinType::Miter, 8.0, std::clamp(segs, 4, 72));
  };
  while (guard++ < 5) {
    try {
      const CrossSection trial = insetProfile(r);
      if (!trial.IsEmpty() && trial.Area() > profile.Area() * 0.02 && height >= r * 2.4) break;
    } catch (const std::exception&) {
    }
    r *= 0.5;
    if (r < 0.08) return plainExtrude();
  }
  if (!(r > 0.04) || height < r * 2.4) return plainExtrude();
  const int bands = std::clamp(std::max(16, segs), 16, 72);
  const double overlap = std::min(0.05, r / bands * 0.35);
  std::vector<Manifold> parts;
  auto addSlice = [&](const CrossSection& slice, double z0, double thick) {
    if (!(thick > 1e-4) || slice.IsEmpty()) return;
    try {
      const Polygons poly = slice.ToPolygons();
      if (poly.empty()) return;
      Manifold slab = Manifold::Extrude(poly, thick, 0, 0.0, vec2(1, 1)).Translate(vec3(0, 0, z0));
      if (healthy(slab)) parts.push_back(std::move(slab));
    } catch (const std::exception&) {
    }
  };
  addSlice(profile, r - overlap, height - 2.0 * r + overlap * 2.0);
  for (int i = 0; i < bands; ++i) {
    const double h0 = r * static_cast<double>(i) / bands;
    const double h1 = r * static_cast<double>(i + 1) / bands;
    const double rise = r - h0;
    const double inset = r - std::sqrt(std::max(0.0, r * r - rise * rise));
    CrossSection slice = profile;
    if (inset > 0.01) {
      try {
        CrossSection shrunk = insetProfile(inset);
        if (!shrunk.IsEmpty()) slice = std::move(shrunk);
      } catch (const std::exception&) {
      }
    }
    const double thick = (h1 - h0) + overlap;
    addSlice(slice, height - h1, thick);
    addSlice(slice, std::max(0.0, h0 - overlap), thick);
  }
  if (parts.empty()) return plainExtrude();
  if (parts.size() == 1) return parts.front();
  try {
    Manifold joined = Manifold::BatchBoolean(parts, manifold::OpType::Add);
    if (healthy(joined)) return joined;
  } catch (const std::exception&) {
  }
  return plainExtrude();
}

Manifold extrudeFaceSolid(const Node& node, QString* warning, double edgeR = 0, double surfaceR = 0, int arcSegs = 8) {
  if (node.faceLoop.size() < 9 || std::abs(node.extrudeDepth) < 0.02) return {};
  vec3 n(node.faceNormal[0], node.faceNormal[1], node.faceNormal[2]);
  n = manifold::la::normalize(n);
  vec3 t(node.faceAxis[0], node.faceAxis[1], node.faceAxis[2]);
  t = t - n * manifold::la::dot(t, n);
  if (manifold::la::length(t) < 1e-5) t = std::abs(n.z) < 0.9 ? vec3(0,0,1) : vec3(1,0,0);
  t = manifold::la::normalize(t);
  vec3 b = manifold::la::normalize(manifold::la::cross(n, t));
  const double depth = std::abs(node.extrudeDepth);
  if (node.extrudeDepth < 0) n = n * -1.0;
  SimplePolygon poly;
  std::vector<SimplePolygon> contours;
  const int count = static_cast<int>(node.faceLoop.size() / 3);
  auto flush = [&] {
    if (contours.empty() && poly.size() < 3) {
      poly.clear();
      return;
    }
    if (poly.size() >= 3) contours.push_back(std::move(poly));
    poly.clear();
  };
  for (int i = 0; i < count; ++i) {
    const vec3 p(node.faceLoop[i * 3], node.faceLoop[i * 3 + 1], node.faceLoop[i * 3 + 2]);
    if (std::abs(p.x) > 1e19 || std::abs(p.y) > 1e19 || std::abs(p.z) > 1e19) {
      flush();
      continue;
    }
    poly.push_back(vec2(manifold::la::dot(p, t), manifold::la::dot(p, b)));
  }
  flush();
  if (contours.empty()) {
    note(warning, QStringLiteral("Could not build that face extrusion."));
    return {};
  }
  auto signedArea = [](const SimplePolygon& ring) {
    double area = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
      const vec2& a = ring[i];
      const vec2& c = ring[(i + 1) % ring.size()];
      area += a.x * c.y - c.x * a.y;
    }
    return area * 0.5;
  };
  try {
    double outerSign = signedArea(contours.front());
    if (std::abs(outerSign) < 1e-6) outerSign = 1;
    CrossSection shape;
    bool started = false;
    for (SimplePolygon ring : contours) {
      const double area = signedArea(ring);
      if (std::abs(area) < 1e-4 || ring.size() < 3) continue;
      const bool solid = area * outerSign > 0;
      if (area < 0) std::reverse(ring.begin(), ring.end());
      const CrossSection piece(Polygons{ring});
      if (!started) {
        shape = solid ? piece : CrossSection();
        started = solid;
        if (!solid) shape = shape - piece;
        continue;
      }
      shape = solid ? shape + piece : shape - piece;
    }
    shape = roundSection(std::move(shape), edgeR, surfaceR, arcSegs, warning);
    const Polygons profile = shape.ToPolygons();
    if (profile.empty()) {
      note(warning, QStringLiteral("Could not build that face extrusion."));
      return {};
    }
    Manifold solid = extrudeLip(shape, depth, edgeR, arcSegs);
    solid = solid.Warp([&](vec3& p) { const vec3 mapped = t * p.x + b * p.y + n * p.z; p = mapped; });
    if (!healthy(solid)) { note(warning, QStringLiteral("Could not build that face extrusion.")); return {}; }
    return solid;
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return {};
  }
}

struct SvgXform {
  double a = 1;
  double b = 0;
  double c = 0;
  double d = 1;
  double e = 0;
  double f = 0;
  SvgXform multiplied(const SvgXform& inner) const {
    return {a * inner.a + c * inner.b, b * inner.a + d * inner.b, a * inner.c + c * inner.d, b * inner.c + d * inner.d,
            a * inner.e + c * inner.f + e, b * inner.e + d * inner.f + f};
  }
  vec2 map(double x, double y) const { return vec2(a * x + c * y + e, b * x + d * y + f); }
};

std::vector<double> svgNumbers(const QString& text) {
  std::vector<double> values;
  const QRegularExpression numberRe(QStringLiteral(R"([-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?)"));
  QRegularExpressionMatchIterator it = numberRe.globalMatch(text);
  while (it.hasNext()) values.push_back(it.next().captured(0).toDouble());
  return values;
}

SvgXform svgTransform(const QString& text) {
  SvgXform result;
  const QRegularExpression callRe(QStringLiteral(R"((rotate|translate|scale|matrix)\s*\(([^)]*)\))"),
                                   QRegularExpression::CaseInsensitiveOption);
  QRegularExpressionMatchIterator it = callRe.globalMatch(text);
  while (it.hasNext()) {
    const QRegularExpressionMatch match = it.next();
    const QString op = match.captured(1).toLower();
    const std::vector<double> n = svgNumbers(match.captured(2));
    SvgXform step;
    if (op == QLatin1String("translate") && !n.empty()) {
      step.e = n[0];
      step.f = n.size() > 1 ? n[1] : 0;
    } else if (op == QLatin1String("scale") && !n.empty()) {
      step.a = n[0];
      step.d = n.size() > 1 ? n[1] : n[0];
    } else if (op == QLatin1String("rotate") && !n.empty()) {
      const double ang = n[0] * kPi / 180.0;
      const double cx = n.size() > 1 ? n[1] : 0;
      const double cy = n.size() > 2 ? n[2] : 0;
      const double co = std::cos(ang);
      const double sn = std::sin(ang);
      const SvgXform to{1, 0, 0, 1, cx, cy};
      const SvgXform rot{co, sn, -sn, co, 0, 0};
      const SvgXform from{1, 0, 0, 1, -cx, -cy};
      step = to.multiplied(rot).multiplied(from);
    } else if (op == QLatin1String("matrix") && n.size() >= 6) {
      step = {n[0], n[1], n[2], n[3], n[4], n[5]};
    } else {
      continue;
    }
    result = result.multiplied(step);
  }
  return result;
}

QString svgAttr(const QString& tag, const QString& name) {
  const QRegularExpression re(QStringLiteral("\\b%1\\s*=\\s*[\"']([^\"']*)[\"']").arg(QRegularExpression::escape(name)),
                              QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch match = re.match(tag);
  return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

bool svgTagIs(const QString& tag, const QString& name, bool closing) {
  int i = 1;
  if (i >= tag.size()) return false;
  if (closing) {
    if (tag[i] != '/') return false;
    ++i;
  } else if (tag[i] == '/') {
    return false;
  }
  if (tag.mid(i, name.size()).compare(name, Qt::CaseInsensitive) != 0) return false;
  const int after = i + name.size();
  if (after >= tag.size()) return false;
  const QChar mark = tag[after];
  return mark.isSpace() || mark == '>' || mark == '/';
}

SimplePolygon svgRectPoly(const SvgXform& world, double x, double y, double w, double h, double inset) {
  const double x0 = x + inset;
  const double y0 = y + inset;
  const double x1 = x + w - inset;
  const double y1 = y + h - inset;
  if (x1 - x0 < 0.05 || y1 - y0 < 0.05) return {};
  const vec2 p0 = world.map(x0, y0);
  const vec2 p1 = world.map(x1, y0);
  const vec2 p2 = world.map(x1, y1);
  const vec2 p3 = world.map(x0, y1);
  SimplePolygon poly{vec2(p0.x, -p0.y), vec2(p1.x, -p1.y), vec2(p2.x, -p2.y), vec2(p3.x, -p3.y)};
  double area = 0;
  for (size_t i = 0; i < poly.size(); ++i) {
    const vec2& a = poly[i];
    const vec2& b = poly[(i + 1) % poly.size()];
    area += a.x * b.y - b.x * a.y;
  }
  if (area < 0) std::reverse(poly.begin(), poly.end());
  return poly;
}

bool svgRectShape(const QString& text, CrossSection* out, std::vector<CrossSection>* frames) {
  if (!out) return false;
  std::vector<SvgXform> stack{SvgXform{}};
  std::vector<CrossSection> parts;
  int cursor = 0;
  while (cursor < text.size()) {
    const int start = text.indexOf('<', cursor);
    if (start < 0) break;
    const int end = text.indexOf('>', start);
    if (end < 0) break;
    const QString tag = text.mid(start, end - start + 1);
    cursor = end + 1;
    if (svgTagIs(tag, QStringLiteral("g"), true)) {
      if (stack.size() > 1) stack.pop_back();
      continue;
    }
    if (svgTagIs(tag, QStringLiteral("g"), false)) {
      stack.push_back(stack.back().multiplied(svgTransform(svgAttr(tag, QStringLiteral("transform")))));
      continue;
    }
    if (!svgTagIs(tag, QStringLiteral("rect"), false)) continue;
    bool widthOk = false;
    bool heightOk = false;
    const double x = svgAttr(tag, QStringLiteral("x")).toDouble();
    const double y = svgAttr(tag, QStringLiteral("y")).toDouble();
    const double w = svgAttr(tag, QStringLiteral("width")).toDouble(&widthOk);
    const double h = svgAttr(tag, QStringLiteral("height")).toDouble(&heightOk);
    if (!widthOk || !heightOk || w <= 0 || h <= 0) continue;
    const QString fill = svgAttr(tag, QStringLiteral("fill")).toLower();
    const QString stroke = svgAttr(tag, QStringLiteral("stroke")).toLower();
    const double strokeWidth = svgAttr(tag, QStringLiteral("stroke-width")).toDouble();
    const bool filled = !fill.isEmpty() && fill != QLatin1String("none") && fill != QLatin1String("transparent");
    const bool stroked = strokeWidth > 0.05 && stroke != QLatin1String("none");
    if (!filled && !stroked) continue;
    const SvgXform world = stack.back().multiplied(svgTransform(svgAttr(tag, QStringLiteral("transform"))));
    if (filled) {
      const SimplePolygon poly = svgRectPoly(world, x, y, w, h, 0);
      if (poly.size() >= 3) {
        CrossSection filledPart(Polygons{poly});
        if (frames) frames->push_back(filledPart);
        parts.push_back(std::move(filledPart));
      }
      continue;
    }
    const SimplePolygon outer = svgRectPoly(world, x, y, w, h, -strokeWidth * 0.5);
    const SimplePolygon inner = svgRectPoly(world, x, y, w, h, strokeWidth * 0.5);
    if (outer.size() < 3) continue;
    CrossSection frame(Polygons{outer});
    if (inner.size() >= 3) frame = frame - CrossSection(Polygons{inner});
    if (frames) frames->push_back(frame);
    parts.push_back(std::move(frame));
  }
  if (parts.empty()) return false;
  CrossSection shape = parts.front();
  for (size_t i = 1; i < parts.size(); ++i) shape = shape + parts[i];
  const Polygons profile = shape.ToPolygons();
  if (profile.empty()) return false;
  double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
  for (const SimplePolygon& ring : profile) {
    for (const vec2& point : ring) {
      minX = std::min(minX, point.x);
      maxX = std::max(maxX, point.x);
      minY = std::min(minY, point.y);
      maxY = std::max(maxY, point.y);
    }
  }
  *out = shape.Translate(vec2(-(minX + maxX) * 0.5, -(minY + maxY) * 0.5));
  return true;
}

CrossSection centerSection(CrossSection shape) {
  const Polygons profile = shape.ToPolygons();
  if (profile.empty()) return shape;
  double minX = 1e300;
  double maxX = -1e300;
  double minY = 1e300;
  double maxY = -1e300;
  for (const SimplePolygon& ring : profile) {
    for (const vec2& point : ring) {
      minX = std::min(minX, point.x);
      maxX = std::max(maxX, point.x);
      minY = std::min(minY, point.y);
      maxY = std::max(maxY, point.y);
    }
  }
  if (!(maxX > minX) || !(maxY > minY)) return shape;
  return shape.Translate(vec2(-(minX + maxX) * 0.5, -(minY + maxY) * 0.5));
}

Manifold svgSolid(const Node& node, QString* warning, double edgeR = 0, double surfaceR = 0, int arcSegs = 8) {
  QFile file(node.meshPath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    note(warning, QStringLiteral("Could not open that SVG shape."));
    return {};
  }
  const QString text = QString::fromUtf8(file.readAll());
  QRegularExpression polygonRe(QStringLiteral(R"(<polygon\b[^>]*\bpoints\s*=\s*[\"']([^\"']+)[\"'][^>]*>)"),
                               QRegularExpression::CaseInsensitiveOption);
  const QRegularExpression numberRe(QStringLiteral(R"([-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?)"));
  std::map<int, std::pair<std::vector<SimplePolygon>, std::vector<SimplePolygon>>> layers;
  QRegularExpressionMatchIterator matches = polygonRe.globalMatch(text);
  while (matches.hasNext()) {
    const QRegularExpressionMatch match = matches.next();
    QRegularExpressionMatchIterator it = numberRe.globalMatch(match.captured(1));
    std::vector<double> values;
    while (it.hasNext()) values.push_back(it.next().captured(0).toDouble());
    if (values.size() < 6 || values.size() % 2 != 0) continue;
    SimplePolygon poly;
    poly.reserve(values.size() / 2);
    for (size_t i = 0; i < values.size(); i += 2) poly.push_back(vec2(values[i], -values[i + 1]));
    if (poly.size() > 1) {
      const vec2& first = poly.front();
      const vec2& last = poly.back();
      if (std::abs(first.x - last.x) < 1e-9 && std::abs(first.y - last.y) < 1e-9) poly.pop_back();
    }
    if (poly.size() < 3) continue;
    double area = 0.0;
    for (size_t i = 0; i < poly.size(); ++i) {
      const vec2& a = poly[i];
      const vec2& b = poly[(i + 1) % poly.size()];
      area += a.x * b.y - b.x * a.y;
    }
    if (area < 0.0) std::reverse(poly.begin(), poly.end());
    const QRegularExpression layerRe(QStringLiteral(R"(data-layer\s*=\s*[\"'](\d+)[\"'])"),
                                     QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch layerMatch = layerRe.match(match.captured(0));
    const int layer = layerMatch.hasMatch() ? layerMatch.captured(1).toInt() : 0;
    auto& bucket = layers[layer];
    if (match.captured(0).contains(QStringLiteral("data-hole"))) bucket.second.push_back(std::move(poly));
    else bucket.first.push_back(std::move(poly));
  }
  CrossSection shape;
  bool anyLayer = false;
  for (const auto& layer : layers) {
    const std::vector<SimplePolygon>& solids = layer.second.first;
    const std::vector<SimplePolygon>& holes = layer.second.second;
    if (solids.empty()) continue;
    CrossSection layerShape(Polygons{solids.front()});
    for (size_t i = 1; i < solids.size(); ++i) layerShape = layerShape + CrossSection(Polygons{solids[i]});
    for (const SimplePolygon& hole : holes) layerShape = layerShape - CrossSection(Polygons{hole});
    if (layerShape.IsEmpty()) continue;
    shape = anyLayer ? shape + layerShape : std::move(layerShape);
    anyLayer = true;
  }
  if (!anyLayer) {
    CrossSection stroked;
    if (svgRectShape(text, &stroked, nullptr) && !stroked.IsEmpty()) {
      try {
        const double height = std::max(node.size[2], 0.05);
        stroked = roundSection(std::move(stroked), edgeR, surfaceR, arcSegs, warning);
        Manifold body = extrudeLip(stroked, height, edgeR, arcSegs).Translate(vec3(0, 0, -height * 0.5));
        if (healthy(body)) return body;
        note(warning, QStringLiteral("That SVG did not produce a shape."));
        return {};
      } catch (const std::exception& ex) {
        note(warning, QString::fromUtf8(ex.what()));
        return {};
      }
    }
    note(warning, QStringLiteral("That SVG has no polygon or rectangle this importer can use."));
    return {};
  }
  try {
    // Layers in one file share one centre, so a hole stays lined up with its shape.
    QRegularExpression centerRe(QStringLiteral(R"(data-center\s*=\s*[\"']([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)[\"'])"),
                                 QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch centerMatch = centerRe.match(text);
    const bool layerFile = QFileInfo(node.meshPath).fileName().startsWith(QLatin1String("layer-"), Qt::CaseInsensitive);
    if (centerMatch.hasMatch()) {
      shape = shape.Translate(vec2(-centerMatch.captured(1).toDouble(), -centerMatch.captured(2).toDouble()));
    } else if (!layerFile) {
      shape = centerSection(std::move(shape));
    }
    shape = roundSection(std::move(shape), edgeR, surfaceR, arcSegs, warning);
    const Polygons profile = shape.ToPolygons();
    if (profile.empty()) {
      note(warning, QStringLiteral("That hole removed the whole shape."));
      return {};
    }
    const double height = std::max(node.size[2], 0.05);
    return extrudeLip(shape, height, edgeR, arcSegs).Translate(vec3(0, 0, -height * 0.5));
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return {};
  }
}

// Text bent onto a circle of radius r: the middle of the letters runs along it, over the top (or
// along the bottom, still upright), each letter standing on the circle as it would on a coin. Long
// straight edges are split first so they follow the curve.
CrossSection textOnArc(const CrossSection& flat, double r, bool below, QString* warning) {
  const manifold::Rect box = flat.Bounds();
  const double width = box.max.x - box.min.x;
  if (width / r > 2 * kPi * 0.97)
    note(warning, QStringLiteral("The text is longer than the circle: it runs into itself. Make the curve radius larger."));
  const double step = std::clamp(r * kPi / 180.0, 0.1, 1.0);  // about one degree of the circle
  Polygons polys = flat.ToPolygons();
  for (SimplePolygon& poly : polys) {
    SimplePolygon fine;
    for (size_t i = 0; i < poly.size(); ++i) {
      const vec2 a = poly[i], b = poly[(i + 1) % poly.size()];
      const int pieces = std::max(1, static_cast<int>(std::ceil(manifold::la::length(b - a) / step)));
      for (int k = 0; k < pieces; ++k) {
        const vec2 p = a + (b - a) * (double(k) / pieces);
        const double angle = p.x / r;
        const double radius = below ? r - p.y : r + p.y;
        fine.push_back(below ? vec2(radius * std::sin(angle), r - radius * std::cos(angle))
                             : vec2(radius * std::sin(angle), radius * std::cos(angle) - r));
      }
    }
    poly = std::move(fine);
  }
  return CrossSection(polys, CrossSection::FillRule::NonZero);
}

// Text in a system font, extruded to size[2]. Centred on its outline box like the SVG shapes.
Manifold textSolid(const Node& node, QString* warning) {
  const double depth = std::max(node.size[2], 0.05);
  const TextSpec spec = textSpecFor(node);
  std::vector<TextLoop> loops;
  QString why;
  const bool ok = textOutlines(spec, &loops, &why);
  if (!why.isEmpty()) note(warning, why);
  try {
    if (!ok) {
      // Nothing to show yet (empty text, missing font): a thin caret keeps the part visible and clickable.
      const CrossSection caret = CrossSection::Square(vec2(spec.capHeight * 0.08, spec.capHeight), true);
      return Manifold::Extrude(caret.ToPolygons(), depth).Translate(vec3(0, 0, -depth * 0.5));
    }
    Polygons polys;
    polys.reserve(loops.size());
    for (const TextLoop& loop : loops) {
      SimplePolygon poly;
      poly.reserve(loop.size() / 2);
      for (size_t i = 0; i + 1 < loop.size(); i += 2) poly.push_back(vec2(loop[i], loop[i + 1]));
      polys.push_back(std::move(poly));
    }
    // Non-zero fill: holes in letters wind the other way, and overlapping strokes (common in variable
    // and script fonts) merge instead of punching holes.
    CrossSection shape(polys, CrossSection::FillRule::NonZero);
    if (shape.IsEmpty()) {
      note(warning, QStringLiteral("That text has no shape to extrude."));
      return {};
    }
    shape = centerSection(std::move(shape));
    if (node.textArc > 0.01) shape = centerSection(textOnArc(shape, node.textArc, node.textArcBelow, warning));
    return Manifold::Extrude(shape.ToPolygons(), depth).Translate(vec3(0, 0, -depth * 0.5));
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return {};
  }
}

Manifold primitiveOf(const Node& node, int) {
  const int segs = segmentsOf(node.segments);
  const double x = std::max(node.size[0], 0.05);
  const double y = std::max(node.size[1], 0.05);
  const double z = std::max(node.size[2], 0.05);
  const QString& shape = node.shape;
  if (shape == QLatin1String("sphere")) return Manifold::Sphere(std::max(node.radius, 0.05), segs);
  if (shape == QLatin1String("cylinder"))
    return Manifold::Cylinder(z, std::max(node.radius, 0.05), std::max(node.radius, 0.05), std::clamp(node.sides, 3, 256), true);
  if (shape == QLatin1String("cone"))
    return Manifold::Cylinder(z, std::max(node.radius, 0.05), std::max(node.radius2, 0.0), segs, true);
  if (shape == QLatin1String("hemisphere")) {
    const double radius = std::max(node.radius, 0.05);
    return Manifold::Sphere(radius, segs).TrimByPlane(vec3(0, 0, 1), 0).Translate(vec3(0, 0, -radius / 2));
  }
  if (shape == QLatin1String("torus")) {
    const double major = std::max(node.radius, 0.2);
    const double minor = std::min(std::max(node.radius2, 0.05), major * 0.95);
    const CrossSection profile = CrossSection::Circle(minor, segs).Translate(vec2(major, 0));
    return Manifold::Revolve(profile.ToPolygons(), segs);
  }
  if (shape == QLatin1String("prism")) {
    const int sides = std::clamp(node.sides, 3, 256);
    const double radius = std::max(node.radius, 0.05);
    SimplePolygon poly;
    poly.reserve(static_cast<size_t>(sides));
    for (int i = 0; i < sides; ++i) {
      const double a = (kPi * 2 * i) / sides + kPi / sides;
      poly.push_back(vec2(std::cos(a) * radius, std::sin(a) * radius));
    }
    return Manifold::Extrude(Polygons{poly}, z, 0, 0.0, vec2(1, 1)).Translate(vec3(0, 0, -z / 2));
  }
  if (shape == QLatin1String("diamond")) return diamondSolid(node, z);
  if (shape == QLatin1String("star")) return starSolid(node, z, true);
  if (shape == QLatin1String("starflat")) return starSolid(node, z, false);
  if (shape == QLatin1String("roof")) return roofSolid(x, y, z);
  if (shape == QLatin1String("heart")) return heartSolid(node, node.segments);
  if (shape == QLatin1String("stl")) return loadStlSolid(node.meshPath, nullptr);
  if (shape == QLatin1String("svg")) return svgSolid(node, nullptr);
  if (shape == QLatin1String("extrude")) return extrudeFaceSolid(node, nullptr);
  if (shape == QLatin1String("text")) return textSolid(node, nullptr);
  if (shape == QLatin1String("pyramid")) {
    const CrossSection profile = CrossSection::Square(vec2(x, y), true);
    Manifold solid = Manifold::Extrude(profile.ToPolygons(), z, 0, 0.0, vec2(0, 0));
    if (!healthy(solid)) solid = Manifold::Extrude(profile.ToPolygons(), z, 0, 0.0, vec2(0.02, 0.02));
    return solid.Translate(vec3(0, 0, -z / 2));
  }
  if (shape == QLatin1String("wedge")) {
    SimplePolygon tri;
    tri.push_back(vec2(0, 0));
    tri.push_back(vec2(x, 0));
    tri.push_back(vec2(0, y));
    Manifold solid = Manifold::Extrude(Polygons{tri}, z, 0, 0.0, vec2(1, 1));
    const auto box = solid.BoundingBox();
    const vec3 center((box.min.x + box.max.x) / 2, (box.min.y + box.max.y) / 2, (box.min.z + box.max.z) / 2);
    return solid.Translate(vec3(-center.x, -center.y, -center.z));
  }
  if (shape == QLatin1String("tube")) {
    const double outer = std::max(node.radius, 0.4);
    const double inner = std::min(std::max(node.radius2, 0.05), outer - 0.05);
    const CrossSection ring = CrossSection::Circle(outer, segs) - CrossSection::Circle(inner, segs);
    const Polygons profile = ring.ToPolygons();
    if (profile.empty()) return Manifold::Cylinder(z, outer, outer, segs, true);
    return Manifold::Extrude(profile, z, 0, 0.0, vec2(1, 1)).Translate(vec3(0, 0, -z / 2));
  }
  if (shape == QLatin1String("capsule")) {
    const double radius = std::max(node.radius, 0.05);
    const double length = z;
    const Manifold top = Manifold::Sphere(radius, segs).Translate(vec3(0, 0, length / 2));
    const Manifold bottom = Manifold::Sphere(radius, segs).Translate(vec3(0, 0, -length / 2));
    const Manifold body = Manifold::Cylinder(length, radius, radius, segs, true);
    return top + bottom + body;
  }
  return Manifold::Cube(vec3(x, y, z), true);
}

Manifold filletOne(Manifold source, double radius, bool edge, int ballSegs, QString* warning) {
  if (!(radius > 0.02) || !healthy(source)) return source;
  try {
    const Manifold ball = Manifold::Sphere(radius, std::clamp(ballSegs, 8, 72));
    if (edge) {
      const Manifold eroded = source.MinkowskiDifference(ball);
      if (!healthy(eroded)) {
        note(warning, QStringLiteral("Edge fillet is larger than this part. Left it sharp."));
        return source;
      }
      const Manifold opened = eroded.MinkowskiSum(ball);
      if (!healthy(opened)) {
        note(warning, QStringLiteral("Edge fillet failed. Left it sharp."));
        return source;
      }
      return opened;
    }
    const Manifold dilated = source.MinkowskiSum(ball);
    const Manifold closed = dilated.MinkowskiDifference(ball);
    if (!healthy(closed)) {
      note(warning, QStringLiteral("Surface fillet failed. Left the inside corners sharp."));
      return source;
    }
    return closed;
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return source;
  }
}

bool convexPrimitive(const QString& shape) {
  return shape == QLatin1String("box") || shape == QLatin1String("sphere") || shape == QLatin1String("cylinder") ||
         shape == QLatin1String("cone") || shape == QLatin1String("hemisphere") || shape == QLatin1String("wedge") ||
         shape == QLatin1String("pyramid") || shape == QLatin1String("prism") || shape == QLatin1String("diamond") ||
         shape == QLatin1String("capsule");
}

bool listedEdge(const std::vector<int>& keys, int key) {
  return std::find(keys.begin(), keys.end(), key) != keys.end();
}

double fitRadius(double sx, double sy, double sz, double radius) {
  const double limit = 0.5 * std::min(sx, std::min(sy, sz)) - 0.08;
  if (!(limit > 0.02)) return 0;
  return std::min(std::max(radius, 0.0), limit);
}

Manifold cylinderOnAxis(int axis, double length, double radius, int segs) {
  Manifold solid = Manifold::Cylinder(std::max(length, 0.04), radius, radius, std::max(segs, 8), true);
  if (axis == 0) return solid.Rotate(0, 90, 0);
  if (axis == 1) return solid.Rotate(90, 0, 0);
  return solid;
}

Manifold aabb(double x0, double x1, double y0, double y1, double z0, double z1) {
  const double minx = std::min(x0, x1);
  const double maxx = std::max(x0, x1);
  const double miny = std::min(y0, y1);
  const double maxy = std::max(y0, y1);
  const double minz = std::min(z0, z1);
  const double maxz = std::max(z0, z1);
  return Manifold::Cube(vec3(std::max(maxx - minx, 0.02), std::max(maxy - miny, 0.02), std::max(maxz - minz, 0.02)), false)
      .Translate(vec3(minx, miny, minz));
}

// All-edge box fillet. Three slabs, twelve cylinders, eight spheres. The corner is a
// real sphere, so three edges do not collapse into the flat dent a faceted Minkowski ball leaves.
Manifold roundedBox(double sx, double sy, double sz, double radius, int segs) {
  const double r = fitRadius(sx, sy, sz, radius);
  if (!(r > 0.02)) return Manifold::Cube(vec3(sx, sy, sz), true);
  const double ix = sx - 2 * r;
  const double iy = sy - 2 * r;
  const double iz = sz - 2 * r;
  const double hx = sx * 0.5;
  const double hy = sy * 0.5;
  const double hz = sz * 0.5;
  std::vector<Manifold> parts;
  parts.reserve(23);
  parts.push_back(Manifold::Cube(vec3(sx, iy, iz), true));
  parts.push_back(Manifold::Cube(vec3(ix, sy, iz), true));
  parts.push_back(Manifold::Cube(vec3(ix, iy, sz), true));
  for (int s0 = 0; s0 < 2; ++s0) {
    for (int s1 = 0; s1 < 2; ++s1) {
      const double sygn = s0 ? 1.0 : -1.0;
      const double szgn = s1 ? 1.0 : -1.0;
      parts.push_back(cylinderOnAxis(0, ix, r, segs).Translate(vec3(0, sygn * (hy - r), szgn * (hz - r))));
    }
  }
  for (int s0 = 0; s0 < 2; ++s0) {
    for (int s1 = 0; s1 < 2; ++s1) {
      const double szgn = s0 ? 1.0 : -1.0;
      const double sxgn = s1 ? 1.0 : -1.0;
      parts.push_back(cylinderOnAxis(1, iy, r, segs).Translate(vec3(sxgn * (hx - r), 0, szgn * (hz - r))));
    }
  }
  for (int s0 = 0; s0 < 2; ++s0) {
    for (int s1 = 0; s1 < 2; ++s1) {
      const double sxgn = s0 ? 1.0 : -1.0;
      const double sygn = s1 ? 1.0 : -1.0;
      parts.push_back(cylinderOnAxis(2, iz, r, segs).Translate(vec3(sxgn * (hx - r), sygn * (hy - r), 0)));
    }
  }
  for (int x = 0; x < 2; ++x) {
    for (int y = 0; y < 2; ++y) {
      for (int z = 0; z < 2; ++z) {
        parts.push_back(Manifold::Sphere(r, segs).Translate(
            vec3((x ? 1.0 : -1.0) * (hx - r), (y ? 1.0 : -1.0) * (hy - r), (z ? 1.0 : -1.0) * (hz - r))));
      }
    }
  }
  try {
    Manifold solid = Manifold::BatchBoolean(parts, manifold::OpType::Add);
    if (!healthy(solid)) return Manifold::Cube(vec3(sx, sy, sz), true);
    Manifold clipped = solid ^ Manifold::Cube(vec3(sx, sy, sz), true);
    return healthy(clipped) ? clipped : solid;
  } catch (const std::exception&) {
    return Manifold::Cube(vec3(sx, sy, sz), true);
  }
}

Manifold filletBoxEdges(double sx, double sy, double sz, double radius, int segs, const std::vector<int>& keys,
                          QString* warning) {
  bool any = false;
  bool all = true;
  for (int key = 0; key < 12; ++key) {
    if (listedEdge(keys, key)) any = true;
    else all = false;
  }
  if (!any || all) return roundedBox(sx, sy, sz, radius, segs);
  const double r = fitRadius(sx, sy, sz, radius);
  if (!(r > 0.02)) return Manifold::Cube(vec3(sx, sy, sz), true);
  const double hx = sx * 0.5;
  const double hy = sy * 0.5;
  const double hz = sz * 0.5;
  auto sign = [](int side) { return side ? 1.0 : -1.0; };
  auto vertex = [](int x, int y, int z) { return x * 4 + y * 2 + z; };
  int meet[8] = {};
  auto mark = [&](int key, int x0, int y0, int z0, int x1, int y1, int z1) {
    if (!listedEdge(keys, key)) return;
    meet[vertex(x0, y0, z0)]++;
    meet[vertex(x1, y1, z1)]++;
  };
  for (int y = 0; y < 2; ++y)
    for (int z = 0; z < 2; ++z) mark(y * 2 + z, 0, y, z, 1, y, z);
  for (int z = 0; z < 2; ++z)
    for (int x = 0; x < 2; ++x) mark(4 + z * 2 + x, x, 0, z, x, 1, z);
  for (int x = 0; x < 2; ++x)
    for (int y = 0; y < 2; ++y) mark(8 + x * 2 + y, x, y, 0, x, y, 1);

  // Notch stops just inside a multi-edge corner so the sphere owns that corner.
  // 0.15 mm of overlap keeps the two cuts from leaving a step.
  auto span = [&](double h, int negativeVertex, int positiveVertex, double* lo, double* hi) {
    *lo = meet[negativeVertex] >= 2 ? -(h - r + 0.15) : -(h + 0.35);
    *hi = meet[positiveVertex] >= 2 ? (h - r + 0.15) : (h + 0.35);
    if (*hi - *lo < 0.2) {
      *lo = -(h + 0.35);
      *hi = h + 0.35;
    }
  };

  std::vector<Manifold> bites;
  auto pushBite = [&](Manifold notch, Manifold round) {
    try {
      Manifold bite = notch - round;
      if (healthy(bite)) bites.push_back(std::move(bite));
    } catch (const std::exception&) {
    }
  };

  for (int y = 0; y < 2; ++y) {
    for (int z = 0; z < 2; ++z) {
      if (!listedEdge(keys, y * 2 + z)) continue;
      const double sygn = sign(y);
      const double szgn = sign(z);
      double lo = 0;
      double hi = 0;
      span(hx, vertex(0, y, z), vertex(1, y, z), &lo, &hi);
      const double cy = sygn * (hy - r);
      const double cz = szgn * (hz - r);
      const double mid = (lo + hi) * 0.5;
      pushBite(aabb(lo, hi, cy, sygn * (hy + 0.45), cz, szgn * (hz + 0.45)),
               cylinderOnAxis(0, (hi - lo) + 0.5, r, segs).Translate(vec3(mid, cy, cz)));
    }
  }
  for (int z = 0; z < 2; ++z) {
    for (int x = 0; x < 2; ++x) {
      if (!listedEdge(keys, 4 + z * 2 + x)) continue;
      const double szgn = sign(z);
      const double sxgn = sign(x);
      double lo = 0;
      double hi = 0;
      span(hy, vertex(x, 0, z), vertex(x, 1, z), &lo, &hi);
      const double cx = sxgn * (hx - r);
      const double cz = szgn * (hz - r);
      const double mid = (lo + hi) * 0.5;
      pushBite(aabb(cx, sxgn * (hx + 0.45), lo, hi, cz, szgn * (hz + 0.45)),
               cylinderOnAxis(1, (hi - lo) + 0.5, r, segs).Translate(vec3(cx, mid, cz)));
    }
  }
  for (int x = 0; x < 2; ++x) {
    for (int y = 0; y < 2; ++y) {
      if (!listedEdge(keys, 8 + x * 2 + y)) continue;
      const double sxgn = sign(x);
      const double sygn = sign(y);
      double lo = 0;
      double hi = 0;
      span(hz, vertex(x, y, 0), vertex(x, y, 1), &lo, &hi);
      const double cx = sxgn * (hx - r);
      const double cy = sygn * (hy - r);
      const double mid = (lo + hi) * 0.5;
      pushBite(aabb(cx, sxgn * (hx + 0.45), cy, sygn * (hy + 0.45), lo, hi),
               cylinderOnAxis(2, (hi - lo) + 0.5, r, segs).Translate(vec3(cx, cy, mid)));
    }
  }
  for (int x = 0; x < 2; ++x) {
    for (int y = 0; y < 2; ++y) {
      for (int z = 0; z < 2; ++z) {
        if (meet[vertex(x, y, z)] < 2) continue;
        const double sxgn = sign(x);
        const double sygn = sign(y);
        const double szgn = sign(z);
        const double cx = sxgn * (hx - r);
        const double cy = sygn * (hy - r);
        const double cz = szgn * (hz - r);
        pushBite(aabb(cx, sxgn * (hx + 0.45), cy, sygn * (hy + 0.45), cz, szgn * (hz + 0.45)),
                 Manifold::Sphere(r, segs).Translate(vec3(cx, cy, cz)));
      }
    }
  }
  if (bites.empty()) {
    note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
    return Manifold::Cube(vec3(sx, sy, sz), true);
  }
  try {
    Manifold cutter = bites.size() == 1 ? std::move(bites.front()) : Manifold::BatchBoolean(bites, manifold::OpType::Add);
    if (!healthy(cutter)) {
      note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
      return Manifold::Cube(vec3(sx, sy, sz), true);
    }
    Manifold solid = Manifold::Cube(vec3(sx, sy, sz), true) - cutter;
    if (!healthy(solid)) {
      note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
      return Manifold::Cube(vec3(sx, sy, sz), true);
    }
    return solid;
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return Manifold::Cube(vec3(sx, sy, sz), true);
  }
}

Manifold filletBox(const Node& node, int segs, QString* warning) {
  // Stretch is a scale on the finished solid. Build the fillet at the real
  // size, then scale it back, so the arc stays a circle of the set radius.
  const double ax = std::max(std::abs(node.scale[0]), 1e-4);
  const double ay = std::max(std::abs(node.scale[1]), 1e-4);
  const double az = std::max(std::abs(node.scale[2]), 1e-4);
  const double sx = std::max(node.size[0] * ax, 0.05);
  const double sy = std::max(node.size[1] * ay, 0.05);
  const double sz = std::max(node.size[2] * az, 0.05);
  Manifold solid = node.filletEdges.empty() ? roundedBox(sx, sy, sz, node.edgeFillet, segs)
                                             : filletBoxEdges(sx, sy, sz, node.edgeFillet, segs, node.filletEdges, warning);
  if (std::abs(ax - 1.0) < 1e-6 && std::abs(ay - 1.0) < 1e-6 && std::abs(az - 1.0) < 1e-6) return solid;
  return solid.Scale(vec3(1.0 / ax, 1.0 / ay, 1.0 / az));
}

struct V2 {
  double x = 0;
  double y = 0;
};
struct V3 {
  double x = 0;
  double y = 0;
  double z = 0;
};

V2 v2add(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
V2 v2sub(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
V2 v2mul(V2 a, double s) { return {a.x * s, a.y * s}; }
double v2dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
double v2cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
double v2len(V2 a) { return std::sqrt(v2dot(a, a)); }
V2 v2norm(V2 a) {
  const double len = v2len(a);
  if (len < 1e-12) return {0, 0};
  return v2mul(a, 1.0 / len);
}
V2 v2left(V2 d) { return {-d.y, d.x}; }

V3 v3add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 v3sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 v3mul(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double v3dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 v3cross(V3 a, V3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double v3len(V3 a) { return std::sqrt(v3dot(a, a)); }
V3 v3norm(V3 a) {
  const double len = v3len(a);
  if (len < 1e-12) return {0, 0, 0};
  return v3mul(a, 1.0 / len);
}
vec3 toVec(V3 p) { return vec3(p.x, p.y, p.z); }

bool edgeWanted(const std::vector<int>& keys, int key) {
  if (key < 0) return false;
  if (keys.empty()) return true;
  return listedEdge(keys, key);
}

Manifold revolveProfile(const SimplePolygon& source, int segs) {
  SimplePolygon poly = source;
  for (vec2& point : poly) {
    if (point.x < 0) point.x = 0;
  }
  if (poly.size() < 3) return {};
  const Polygons polygons = CrossSection(poly).ToPolygons();
  if (polygons.empty()) return {};
  return Manifold::Revolve(polygons, std::max(segs, 8));
}

SimplePolygon roundProfile(const std::vector<V2>& verts, const std::vector<int>& keys, const std::vector<int>& selected,
                           double radius, int arcSegs, bool* rounded, QString* warning) {
  *rounded = false;
  SimplePolygon plain;
  plain.reserve(verts.size());
  for (const V2& point : verts) plain.push_back(vec2(point.x, point.y));
  if (!(radius > 0.02) || verts.size() < 3) return plain;

  struct Corner {
    bool live = false;
    double ang0 = 0;
    double sweep = 0;
    double radius = 0;
    V2 center;
  };
  const int count = static_cast<int>(verts.size());
  double used = radius;
  std::vector<Corner> corners(static_cast<size_t>(count));
  for (int attempt = 0; attempt < 6; ++attempt) {
    corners.assign(static_cast<size_t>(count), {});
    std::vector<double> trim(static_cast<size_t>(count), 0);
    for (int i = 0; i < count; ++i) {
      if (!edgeWanted(selected, keys[static_cast<size_t>(i)])) continue;
      const V2 prev = verts[static_cast<size_t>((i + count - 1) % count)];
      const V2 cur = verts[static_cast<size_t>(i)];
      const V2 next = verts[static_cast<size_t>((i + 1) % count)];
      const V2 d0 = v2norm(v2sub(cur, prev));
      const V2 d1 = v2norm(v2sub(next, cur));
      if (v2dot(d0, d0) < 0.5 || v2dot(d1, d1) < 0.5) continue;
      if (v2cross(d0, d1) <= 1e-3) continue;
      const V2 n0 = v2left(d0);
      const V2 n1 = v2left(d1);
      const double nd = std::clamp(v2dot(n0, n1), -0.92, 0.995);
      if (1.0 + nd < 0.08) continue;
      const double k = used / (1.0 + nd);
      const V2 center = v2add(cur, v2mul(v2add(n0, n1), k));
      const V2 t0 = v2sub(center, v2mul(n0, used));
      const V2 t1 = v2sub(center, v2mul(n1, used));
      double sweep = std::atan2(t1.y - center.y, t1.x - center.x) - std::atan2(t0.y - center.y, t0.x - center.x);
      if (sweep <= 0) sweep += 2 * kPi;
      if (sweep > kPi + 0.08) continue;
      Corner corner;
      corner.live = true;
      corner.ang0 = std::atan2(t0.y - center.y, t0.x - center.x);
      corner.sweep = sweep;
      corner.radius = used;
      corner.center = center;
      corners[static_cast<size_t>(i)] = corner;
      trim[static_cast<size_t>(i)] = std::max(v2len(v2sub(cur, t0)), v2len(v2sub(cur, t1)));
    }
    double scale = 1;
    for (int i = 0; i < count; ++i) {
      const int j = (i + 1) % count;
      const double edge = v2len(v2sub(verts[static_cast<size_t>(j)], verts[static_cast<size_t>(i)]));
      const double need = trim[static_cast<size_t>(i)] + trim[static_cast<size_t>(j)];
      if (need > edge - 0.04 && need > 1e-6) scale = std::min(scale, std::max(0.0, (edge - 0.04) / need));
    }
    if (scale > 0.98) break;
    used *= scale;
    if (!(used > 0.04)) {
      note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
      return plain;
    }
  }

  bool any = false;
  for (const Corner& corner : corners)
    if (corner.live) any = true;
  if (!any) return plain;
  if (used < radius - 0.05) note(warning, QStringLiteral("Fillet radius was reduced so it would fit."));

  SimplePolygon poly;
  const int cap = std::clamp(arcSegs, 4, 72);
  for (int i = 0; i < count; ++i) {
    const Corner& corner = corners[static_cast<size_t>(i)];
    if (!corner.live) {
      poly.push_back(vec2(verts[static_cast<size_t>(i)].x, verts[static_cast<size_t>(i)].y));
      continue;
    }
    const int steps = std::clamp(static_cast<int>(std::lround(cap * corner.sweep / (kPi * 0.5))), 2, cap);
    for (int step = 0; step <= steps; ++step) {
      const double ang = corner.ang0 + corner.sweep * (static_cast<double>(step) / steps);
      poly.push_back(vec2(corner.center.x + std::cos(ang) * corner.radius, corner.center.y + std::sin(ang) * corner.radius));
    }
  }
  *rounded = poly.size() >= 3;
  return *rounded ? poly : plain;
}

Manifold filletRevolution(const Node& node, int revolveSegs, int arcSegs, QString* warning) {
  const QString& shape = node.shape;
  const double height = std::max(node.size[2], 0.05);
  const double hz = height * 0.5;
  const double radius = std::max(node.radius, 0.05);
  const double radius2 = std::max(node.radius2, 0.0);
  std::vector<V2> verts;
  std::vector<int> keys;
  double limit = radius * 0.45;
  if (shape == QLatin1String("cylinder")) {
    verts = {{0, -hz}, {radius, -hz}, {radius, hz}, {0, hz}};
    keys = {-1, 0, 1, -1};
    limit = std::min(radius * 0.92, hz - 0.05);
  } else if (shape == QLatin1String("cone")) {
    const double top = radius2;
    if (top > 0.2) {
      verts = {{0, -hz}, {radius, -hz}, {top, hz}, {0, hz}};
      keys = {-1, 0, 1, -1};
    } else {
      verts = {{0, -hz}, {radius, -hz}, {0, hz}};
      keys = {-1, 0, -1};
    }
    limit = std::min(std::min(radius, std::max(top, radius)) * 0.85, hz * 0.85);
  } else if (shape == QLatin1String("tube")) {
    const double outer = std::max(radius, 0.4);
    const double inner = std::min(std::max(radius2, 0.05), outer - 0.08);
    verts = {{outer, -hz}, {outer, hz}, {inner, hz}, {inner, -hz}};
    keys = {0, 1, 3, 2};
    limit = std::min((outer - inner) * 0.45, hz - 0.05);
  } else if (shape == QLatin1String("hemisphere")) {
    const double used = std::min(std::max(node.edgeFillet, 0.0), radius * 0.45);
    if (!(used > 0.02) || !edgeWanted(node.filletEdges, 0)) return primitiveOf(node, std::max(revolveSegs, 8));
    const int dome = std::max(12, revolveSegs / 2);
    SimplePolygon poly;
    for (int i = 0; i <= dome; ++i) {
      const double ang = -kPi * 0.5 + (kPi * 0.5) * (static_cast<double>(i) / dome);
      poly.push_back(vec2(std::cos(ang) * radius, -radius * 0.5 + std::sin(ang) * radius));
    }
    poly.push_back(vec2(0, -radius * 0.5));
    SimplePolygon notch;
    const double rimZ = -radius * 0.5;
    notch.push_back(vec2(radius - used, rimZ - used));
    notch.push_back(vec2(radius + 0.45, rimZ - used));
    notch.push_back(vec2(radius + 0.45, rimZ + 0.45));
    notch.push_back(vec2(radius - used, rimZ + 0.45));
    try {
      const CrossSection bite =
          CrossSection(notch) - CrossSection::Circle(used, arcSegs).Translate(vec2(radius - used, rimZ - used));
      const Polygons rounded = (CrossSection(poly) - bite).ToPolygons();
      if (rounded.empty()) return primitiveOf(node, std::max(revolveSegs, 8));
      Manifold solid = Manifold::Revolve(rounded, std::max(revolveSegs, 8));
      if (!healthy(solid)) {
        note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
        return primitiveOf(node, std::max(revolveSegs, 8));
      }
      return solid;
    } catch (const std::exception& ex) {
      note(warning, QString::fromUtf8(ex.what()));
      return primitiveOf(node, std::max(revolveSegs, 8));
    }
  } else {
    return primitiveOf(node, std::max(revolveSegs, 8));
  }
  const double used = std::min(std::max(node.edgeFillet, 0.0), std::max(limit, 0.0));
  if (!(used > 0.02)) return primitiveOf(node, std::max(revolveSegs, 8));
  bool rounded = false;
  const SimplePolygon poly = roundProfile(verts, keys, node.filletEdges, used, arcSegs, &rounded, warning);
  if (!rounded) return primitiveOf(node, std::max(revolveSegs, 8));
  try {
    Manifold solid = revolveProfile(poly, revolveSegs);
    if (!healthy(solid)) {
      note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
      return primitiveOf(node, std::max(revolveSegs, 8));
    }
    return solid;
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return primitiveOf(node, std::max(revolveSegs, 8));
  }
}

struct EdgeSeg {
  int key = 0;
  V3 a;
  V3 b;
  V3 n0;
  V3 n1;
  bool concave = false;
};

void addSeg(std::vector<EdgeSeg>& out, int key, V3 a, V3 b, V3 n0, V3 n1) {
  n0 = v3norm(n0);
  n1 = v3norm(n1);
  if (v3len(v3sub(b, a)) < 1e-4 || v3len(n0) < 0.5 || v3len(n1) < 0.5) return;
  out.push_back(EdgeSeg{key, a, b, n0, n1});
}

V3 faceOut(V3 p0, V3 p1, V3 p2) {
  V3 n = v3norm(v3cross(v3sub(p1, p0), v3sub(p2, p0)));
  if (v3dot(n, v3sub(V3{0, 0, 0}, p0)) > 0) n = v3mul(n, -1);
  return n;
}

void straightSegs(const Node& node, std::vector<EdgeSeg>& out) {
  const QString& shape = node.shape;
  if (shape == QLatin1String("prism")) {
    const int sides = std::clamp(node.sides, 3, 256);
    const double radius = std::max(node.radius, 0.05);
    const double hz = std::max(node.size[2], 0.05) * 0.5;
    std::vector<V3> bottom(static_cast<size_t>(sides));
    std::vector<V3> top(static_cast<size_t>(sides));
    for (int i = 0; i < sides; ++i) {
      const double ang = (kPi * 2 * i) / sides + kPi / sides;
      bottom[static_cast<size_t>(i)] = {std::cos(ang) * radius, std::sin(ang) * radius, -hz};
      top[static_cast<size_t>(i)] = {std::cos(ang) * radius, std::sin(ang) * radius, hz};
    }
    auto side = [&](int i) {
      const V3& p = bottom[static_cast<size_t>(i % sides)];
      const V3& q = bottom[static_cast<size_t>((i + 1) % sides)];
      return v3norm(V3{q.y - p.y, p.x - q.x, 0});
    };
    for (int i = 0; i < sides; ++i) {
      const V3 nA = side((i + sides - 1) % sides);
      const V3 nB = side(i);
      addSeg(out, i, bottom[static_cast<size_t>(i)], top[static_cast<size_t>(i)], nA, nB);
      addSeg(out, sides + i, bottom[static_cast<size_t>(i)], bottom[static_cast<size_t>((i + 1) % sides)], V3{0, 0, -1}, nB);
      addSeg(out, 2 * sides + i, top[static_cast<size_t>(i)], top[static_cast<size_t>((i + 1) % sides)], V3{0, 0, 1}, nB);
    }
    return;
  }
  if (shape == QLatin1String("wedge")) {
    const double x = std::max(node.size[0], 0.05);
    const double y = std::max(node.size[1], 0.05);
    const double z = std::max(node.size[2], 0.05);
    const double hx = x * 0.5;
    const double hy = y * 0.5;
    const double hz = z * 0.5;
    const V3 b0{-hx, -hy, -hz};
    const V3 b1{hx, -hy, -hz};
    const V3 b2{-hx, hy, -hz};
    const V3 t0{-hx, -hy, hz};
    const V3 t1{hx, -hy, hz};
    const V3 t2{-hx, hy, hz};
    const V3 nx{-1, 0, 0};
    const V3 ny{0, -1, 0};
    const V3 hyp = v3norm(V3{y, x, 0});
    const V3 down{0, 0, -1};
    const V3 up{0, 0, 1};
    addSeg(out, 0, b0, t0, nx, ny);
    addSeg(out, 1, b1, t1, ny, hyp);
    addSeg(out, 2, b2, t2, hyp, nx);
    addSeg(out, 3, b0, b1, down, ny);
    addSeg(out, 4, b1, b2, down, hyp);
    addSeg(out, 5, b2, b0, down, nx);
    addSeg(out, 6, t0, t1, up, ny);
    addSeg(out, 7, t1, t2, up, hyp);
    addSeg(out, 8, t2, t0, up, nx);
    return;
  }
  if (shape == QLatin1String("pyramid")) {
    const double hx = std::max(node.size[0], 0.05) * 0.5;
    const double hy = std::max(node.size[1], 0.05) * 0.5;
    const double hz = std::max(node.size[2], 0.05) * 0.5;
    const V3 b0{-hx, -hy, -hz};
    const V3 b1{hx, -hy, -hz};
    const V3 b2{hx, hy, -hz};
    const V3 b3{-hx, hy, -hz};
    const V3 apex{0, 0, hz};
    const V3 base{0, 0, -1};
    const V3 f0 = faceOut(b0, b1, apex);
    const V3 f1 = faceOut(b1, b2, apex);
    const V3 f2 = faceOut(b2, b3, apex);
    const V3 f3 = faceOut(b3, b0, apex);
    addSeg(out, 0, b0, b1, base, f0);
    addSeg(out, 1, b1, b2, base, f1);
    addSeg(out, 2, b2, b3, base, f2);
    addSeg(out, 3, b3, b0, base, f3);
    addSeg(out, 4, b0, apex, f3, f0);
    addSeg(out, 5, b1, apex, f0, f1);
    addSeg(out, 6, b2, apex, f1, f2);
    addSeg(out, 7, b3, apex, f2, f3);
  }
}

Manifold cylinderAlong(V3 a, V3 b, double radius, int segs) {
  const V3 delta = v3sub(b, a);
  const double length = v3len(delta);
  const V3 mid = v3mul(v3add(a, b), 0.5);
  Manifold cyl = Manifold::Cylinder(std::max(length, 0.04), radius, radius, std::max(segs, 8), true);
  if (length < 1e-4) return Manifold::Sphere(radius, std::max(segs, 8)).Translate(toVec(mid));
  const V3 dir = v3mul(delta, 1.0 / length);
  if (dir.z > 0.9995) return cyl.Translate(toVec(mid));
  if (dir.z < -0.9995) return cyl.Rotate(180, 0, 0).Translate(toVec(mid));
  const double yaw = std::atan2(dir.y, dir.x) * 180.0 / kPi;
  const double tilt = std::atan2(std::sqrt(dir.x * dir.x + dir.y * dir.y), dir.z) * 180.0 / kPi;
  return cyl.Rotate(0, tilt, yaw).Translate(toVec(mid));
}

bool solveNormal(V3 r0, V3 r1, V3 r2, V3 b, V3* out) {
  const double det = v3dot(r0, v3cross(r1, r2));
  if (std::abs(det) < 1e-8) return false;
  const V3 c0 = v3cross(r1, r2);
  const V3 c1 = v3cross(r2, r0);
  const V3 c2 = v3cross(r0, r1);
  *out = v3mul(v3add(v3add(v3mul(c0, b.x), v3mul(c1, b.y)), v3mul(c2, b.z)), 1.0 / det);
  return true;
}

struct PrismLine {
  V2 outward;
  double c = 0;
};

PrismLine prismEdgeLine(int edge, int sides, double radius, double inset) {
  const double mid = (2.0 * kPi * (edge + 1)) / sides;
  const V2 outward{std::cos(mid), std::sin(mid)};
  return {outward, radius * std::cos(kPi / sides) - inset};
}

bool prismLineHit(PrismLine a, PrismLine b, V2* out) {
  const double det = a.outward.x * b.outward.y - a.outward.y * b.outward.x;
  if (std::abs(det) < 1e-10) return false;
  out->x = (a.c * b.outward.y - b.c * a.outward.y) / det;
  out->y = (a.outward.x * b.c - b.outward.x * a.c) / det;
  return true;
}

std::vector<vec2> prismSamples(int sides, double radius, const std::vector<double>& inset, const std::vector<double>& corner,
                              int arcSteps) {
  const int steps = std::max(arcSteps, 2);
  const int stride = steps + 1;
  std::vector<vec2> points(static_cast<size_t>(sides * stride));
  for (int i = 0; i < sides; ++i) {
    const int prev = (i + sides - 1) % sides;
    const double rho = corner[static_cast<size_t>(i)];
    auto put = [&](int sample, double x, double y) {
      points[static_cast<size_t>(i * stride + sample)] = vec2(x, y);
    };
    if (rho < 0.02) {
      V2 point;
      if (!prismLineHit(prismEdgeLine(prev, sides, radius, inset[static_cast<size_t>(prev)]),
                        prismEdgeLine(i, sides, radius, inset[static_cast<size_t>(i)]), &point))
        return {};
      for (int sample = 0; sample <= steps; ++sample) put(sample, point.x, point.y);
      continue;
    }
    const PrismLine left = prismEdgeLine(prev, sides, radius, inset[static_cast<size_t>(prev)] + rho);
    const PrismLine right = prismEdgeLine(i, sides, radius, inset[static_cast<size_t>(i)] + rho);
    V2 center;
    if (!prismLineHit(left, right, &center)) return {};
    const double a0 = std::atan2(left.outward.y, left.outward.x);
    const double a1 = std::atan2(right.outward.y, right.outward.x);
    double sweep = a1 - a0;
    if (sweep <= 0) sweep += 2 * kPi;
    if (sweep > kPi + 0.05) return {};
    for (int sample = 0; sample <= steps; ++sample) {
      const double ang = a0 + sweep * (static_cast<double>(sample) / steps);
      put(sample, center.x + std::cos(ang) * rho, center.y + std::sin(ang) * rho);
    }
  }
  return points;
}

Manifold loftPrismSamples(const std::vector<std::vector<vec2>>& loops, const std::vector<double>& heights, QString* warning) {
  if (loops.size() < 2 || loops.size() != heights.size()) return {};
  const int rings = static_cast<int>(loops.size());
  const int count = static_cast<int>(loops.front().size());
  if (count < 3) return {};
  for (const std::vector<vec2>& loop : loops)
    if (static_cast<int>(loop.size()) != count) return {};

  struct Built {
    std::vector<vec3> verts;
    std::vector<int> index;
  };
  std::vector<Built> built(static_cast<size_t>(rings));
  std::vector<int> base(static_cast<size_t>(rings), 0);
  int total = 0;
  for (int ring = 0; ring < rings; ++ring) {
    Built& out = built[static_cast<size_t>(ring)];
    out.index.assign(static_cast<size_t>(count), 0);
    for (int i = 0; i < count; ++i) {
      const vec2& sample = loops[static_cast<size_t>(ring)][static_cast<size_t>(i)];
      const vec3 point(sample.x, sample.y, heights[static_cast<size_t>(ring)]);
      if (i > 0) {
        const vec3& previous = out.verts[static_cast<size_t>(out.index[static_cast<size_t>(i - 1)])];
        if (std::hypot(point.x - previous.x, point.y - previous.y) < 1e-4 && std::abs(point.z - previous.z) < 1e-4) {
          out.index[static_cast<size_t>(i)] = out.index[static_cast<size_t>(i - 1)];
          continue;
        }
      }
      out.index[static_cast<size_t>(i)] = static_cast<int>(out.verts.size());
      out.verts.push_back(point);
    }
    if (out.verts.size() < 3) return {};
    base[static_cast<size_t>(ring)] = total;
    total += static_cast<int>(out.verts.size());
  }

  std::vector<vec3> verts;
  verts.reserve(static_cast<size_t>(total));
  for (const Built& ring : built) verts.insert(verts.end(), ring.verts.begin(), ring.verts.end());
  auto gid = [&](int ring, int sample) {
    return base[static_cast<size_t>(ring)] + built[static_cast<size_t>(ring)].index[static_cast<size_t>(sample)];
  };
  auto area2 = [&](int a, int b, int c) {
    const vec3 ab(verts[static_cast<size_t>(b)].x - verts[static_cast<size_t>(a)].x,
                  verts[static_cast<size_t>(b)].y - verts[static_cast<size_t>(a)].y,
                  verts[static_cast<size_t>(b)].z - verts[static_cast<size_t>(a)].z);
    const vec3 ac(verts[static_cast<size_t>(c)].x - verts[static_cast<size_t>(a)].x,
                  verts[static_cast<size_t>(c)].y - verts[static_cast<size_t>(a)].y,
                  verts[static_cast<size_t>(c)].z - verts[static_cast<size_t>(a)].z);
    const double cx = ab.y * ac.z - ab.z * ac.y;
    const double cy = ab.z * ac.x - ab.x * ac.z;
    const double cz = ab.x * ac.y - ab.y * ac.x;
    return cx * cx + cy * cy + cz * cz;
  };
  struct Tri {
    int a, b, c;
  };
  std::vector<Tri> tris;
  auto pushTri = [&](int a, int b, int c) {
    if (a == b || b == c || a == c) return;
    if (area2(a, b, c) < 1e-10) return;
    tris.push_back(Tri{a, b, c});
  };
  for (int step = 0; step < rings - 1; ++step) {
    for (int i = 0; i < count; ++i) {
      const int j = (i + 1) % count;
      const int a = gid(step, i);
      const int b = gid(step, j);
      const int c = gid(step + 1, i);
      const int d = gid(step + 1, j);
      pushTri(a, d, c);
      pushTri(a, b, d);
    }
  }
  auto unique = [&](int ring) {
    std::vector<int> ids;
    for (int i = 0; i < count; ++i) {
      const int id = gid(ring, i);
      if (ids.empty() || ids.back() != id) ids.push_back(id);
    }
    if (ids.size() > 1 && ids.front() == ids.back()) ids.pop_back();
    return ids;
  };
  const std::vector<int> bottom = unique(0);
  const std::vector<int> top = unique(rings - 1);
  for (int i = 1; i + 1 < static_cast<int>(bottom.size()); ++i) pushTri(bottom[0], bottom[static_cast<size_t>(i + 1)], bottom[static_cast<size_t>(i)]);
  for (int i = 1; i + 1 < static_cast<int>(top.size()); ++i) pushTri(top[0], top[static_cast<size_t>(i)], top[static_cast<size_t>(i + 1)]);
  if (tris.size() < 4) return {};

  double volume = 0;
  for (const Tri& tri : tris) {
    const vec3& a = verts[static_cast<size_t>(tri.a)];
    const vec3& b = verts[static_cast<size_t>(tri.b)];
    const vec3& c = verts[static_cast<size_t>(tri.c)];
    volume += a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x);
  }
  if (volume < 0) {
    for (Tri& tri : tris) std::swap(tri.b, tri.c);
  }

  try {
    manifold::MeshGL mesh;
    mesh.numProp = 3;
    mesh.vertProperties.reserve(verts.size() * 3);
    mesh.triVerts.reserve(tris.size() * 3);
    for (const vec3& point : verts) {
      mesh.vertProperties.push_back(static_cast<float>(point.x));
      mesh.vertProperties.push_back(static_cast<float>(point.y));
      mesh.vertProperties.push_back(static_cast<float>(point.z));
    }
    for (const Tri& tri : tris) {
      mesh.triVerts.push_back(static_cast<uint32_t>(tri.a));
      mesh.triVerts.push_back(static_cast<uint32_t>(tri.b));
      mesh.triVerts.push_back(static_cast<uint32_t>(tri.c));
    }
    Manifold solid(mesh);
    if (!healthy(solid)) {
      note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
      return {};
    }
    return solid;
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return {};
  }
}

Manifold filletPrism(const Node& node, int segs, QString* warning) {
  const int sides = std::clamp(node.sides, 3, 256);
  const double radius = std::max(node.radius, 0.05);
  const double height = std::max(node.size[2], 0.05);
  const double hz = height * 0.5;
  const double apothem = radius * std::cos(kPi / sides);
  const double edgeLen = 2.0 * radius * std::sin(kPi / sides);
  double limit = std::min(apothem * 0.82, height * 0.45);
  const double turn = std::tan(kPi / sides);
  if (turn > 1e-4) limit = std::min(limit, edgeLen * 0.46 / turn);
  double r = std::min(std::max(node.edgeFillet, 0.0), std::max(limit, 0.0));
  if (node.edgeFillet > r + 0.05) note(warning, QStringLiteral("Fillet radius was reduced so it would fit."));
  if (!(r > 0.04)) {
    note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
    return primitiveOf(node, 48);
  }

  const bool every = node.filletEdges.empty();
  auto wanted = [&](int key) { return every || listedEdge(node.filletEdges, key); };
  std::vector<char> vertical(static_cast<size_t>(sides), 0);
  std::vector<char> bottom(static_cast<size_t>(sides), 0);
  std::vector<char> top(static_cast<size_t>(sides), 0);
  int verticalCount = 0;
  int bottomCount = 0;
  int topCount = 0;
  for (int i = 0; i < sides; ++i) {
    vertical[static_cast<size_t>(i)] = wanted(i) ? 1 : 0;
    bottom[static_cast<size_t>(i)] = wanted(sides + i) ? 1 : 0;
    top[static_cast<size_t>(i)] = wanted(2 * sides + i) ? 1 : 0;
    verticalCount += vertical[static_cast<size_t>(i)];
    bottomCount += bottom[static_cast<size_t>(i)];
    topCount += top[static_cast<size_t>(i)];
  }
  if (verticalCount + bottomCount + topCount == 0) return primitiveOf(node, 48);

  const int arcSegs = std::clamp(segs, 4, 72);
  if (verticalCount == sides && bottomCount == sides && topCount == sides && height > 2.0 * r + 0.2) {
    try {
      SimplePolygon sharp;
      sharp.reserve(static_cast<size_t>(sides));
      for (int i = 0; i < sides; ++i) {
        const double ang = (kPi * 2.0 * i) / sides + kPi / sides;
        sharp.push_back(vec2(std::cos(ang) * radius, std::sin(ang) * radius));
      }
      const Polygons core = CrossSection(sharp).Offset(-r, CrossSection::JoinType::Miter, 4.0).ToPolygons();
      if (!core.empty()) {
        const double body = height - 2.0 * r;
        Manifold solid = Manifold::Extrude(core, body, 0, 0.0, vec2(1, 1)).Translate(vec3(0, 0, -body * 0.5));
        solid = solid.MinkowskiSum(Manifold::Sphere(r, std::max(arcSegs, 8)));
        if (healthy(solid)) return solid;
      }
    } catch (const std::exception&) {
    }
  }

  auto sectionAt = [&](double fromTop, double fromBottom) -> std::vector<vec2> {
    std::vector<double> inset(static_cast<size_t>(sides), 0.0);
    std::vector<double> corner(static_cast<size_t>(sides), 0.0);
    const bool inTop = fromTop >= 0.0;
    const bool inBottom = fromBottom >= 0.0;
    if (inTop) {
      const double down = std::clamp(r - fromTop, 0.0, r);
      const double d = r - std::sqrt(std::max(0.0, r * r - down * down));
      for (int i = 0; i < sides; ++i)
        if (top[static_cast<size_t>(i)]) inset[static_cast<size_t>(i)] = d;
    } else if (inBottom) {
      const double down = std::clamp(r - fromBottom, 0.0, r);
      const double d = r - std::sqrt(std::max(0.0, r * r - down * down));
      for (int i = 0; i < sides; ++i)
        if (bottom[static_cast<size_t>(i)]) inset[static_cast<size_t>(i)] = d;
    }
    for (int i = 0; i < sides; ++i) {
      if (!vertical[static_cast<size_t>(i)]) continue;
      const int prev = (i + sides - 1) % sides;
      const bool sphereTop = inTop && top[static_cast<size_t>(prev)] && top[static_cast<size_t>(i)];
      const bool sphereBottom = inBottom && bottom[static_cast<size_t>(prev)] && bottom[static_cast<size_t>(i)];
      if (sphereTop || sphereBottom) {
        const double fromCap = sphereTop ? fromTop : fromBottom;
        const double down = std::clamp(r - fromCap, 0.0, r);
        corner[static_cast<size_t>(i)] = std::sqrt(std::max(0.0, r * r - down * down));
      } else {
        corner[static_cast<size_t>(i)] = r;
      }
    }
    return prismSamples(sides, radius, inset, corner, arcSegs);
  };

  if (bottomCount == 0 && topCount == 0) {
    const std::vector<vec2> loop = sectionAt(-1.0, -1.0);
    SimplePolygon poly;
    for (const vec2& point : loop) {
      if (!poly.empty() && std::hypot(point.x - poly.back().x, point.y - poly.back().y) < 1e-4) continue;
      poly.push_back(point);
    }
    if (poly.size() >= 2 && std::hypot(poly.front().x - poly.back().x, poly.front().y - poly.back().y) < 1e-4) poly.pop_back();
    if (poly.size() >= 3) {
      try {
        Manifold solid = Manifold::Extrude(Polygons{poly}, height, 0, 0.0, vec2(1, 1)).Translate(vec3(0, 0, -hz));
        if (healthy(solid)) return solid;
      } catch (const std::exception&) {
      }
    }
  }

  struct Station {
    double z = 0;
    double fromTop = -1;
    double fromBottom = -1;
  };
  std::vector<Station> stations;
  auto pushStation = [&](double z, double fromTop, double fromBottom) {
    if (!stations.empty() && std::abs(stations.back().z - z) < 1e-4) return;
    stations.push_back(Station{z, fromTop, fromBottom});
  };
  const int riseSteps = std::clamp(arcSegs * 2, 20, 48);
  if (bottomCount > 0) {
    for (int step = 0; step <= riseSteps; ++step) {
      const double ang = (kPi * 0.5) * (1.0 - static_cast<double>(step) / riseSteps);
      const double fromCap = r * std::cos(ang);
      pushStation(-hz + fromCap, -1.0, fromCap);
    }
  } else {
    pushStation(-hz, -1.0, -1.0);
  }
  if (topCount > 0) {
    pushStation(hz - r, -1.0, -1.0);
    for (int step = 0; step <= riseSteps; ++step) {
      const double ang = (kPi * 0.5) * static_cast<double>(step) / riseSteps;
      const double fromCap = r * std::cos(ang);
      pushStation(hz - fromCap, fromCap, -1.0);
    }
  } else {
    pushStation(hz, -1.0, -1.0);
  }

  const int sampleCount = sides * (arcSegs + 1);
  std::vector<std::vector<vec2>> loops;
  std::vector<double> heights;
  loops.reserve(stations.size());
  heights.reserve(stations.size());
  for (const Station& station : stations) {
    const std::vector<vec2> loop = sectionAt(station.fromTop, station.fromBottom);
    if (static_cast<int>(loop.size()) != sampleCount) {
      note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
      return primitiveOf(node, 48);
    }
    loops.push_back(loop);
    heights.push_back(station.z);
  }
  Manifold solid = loftPrismSamples(loops, heights, warning);
  if (!healthy(solid)) {
    note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
    return primitiveOf(node, 48);
  }
  return solid;
}

// The radius the shape's own Fillet really gets (each kind caps it so it fits), in the frame the
// mesh rounds it in: the real size for a box, the unscaled size for the others. 0 when nothing is
// rounded. The exact STEP export rounds the same edges with this radius.
double filletRadiusUsed(const Node& node) {
  if (!(node.edgeFillet > 0.02)) return 0;
  const QString& shape = node.shape;
  if (shape == QLatin1String("box")) {
    const double ax = std::max(std::abs(node.scale[0]), 1e-4), ay = std::max(std::abs(node.scale[1]), 1e-4),
                 az = std::max(std::abs(node.scale[2]), 1e-4);
    return fitRadius(std::max(node.size[0] * ax, 0.05), std::max(node.size[1] * ay, 0.05), std::max(node.size[2] * az, 0.05),
                     node.edgeFillet);
  }
  const double height = std::max(node.size[2], 0.05), hz = height * 0.5;
  const double radius = std::max(node.radius, 0.05), radius2 = std::max(node.radius2, 0.0);
  double limit = -1;
  if (shape == QLatin1String("cylinder")) limit = std::min(radius * 0.92, hz - 0.05);
  else if (shape == QLatin1String("cone")) limit = std::min(std::min(radius, std::max(radius2, radius)) * 0.85, hz * 0.85);
  else if (shape == QLatin1String("tube")) {
    const double outer = std::max(radius, 0.4), inner = std::min(std::max(radius2, 0.05), outer - 0.08);
    limit = std::min((outer - inner) * 0.45, hz - 0.05);
  } else if (shape == QLatin1String("hemisphere")) limit = radius * 0.45;
  else if (shape == QLatin1String("prism") || (shape == QLatin1String("diamond") && node.gemTable <= 0.001)) {
    const int sides = std::clamp(node.sides, 3, 256);
    const double apothem = radius * std::cos(kPi / sides), edgeLen = 2.0 * radius * std::sin(kPi / sides);
    limit = std::min(apothem * 0.82, height * 0.45);
    const double turn = std::tan(kPi / sides);
    if (turn > 1e-4) limit = std::min(limit, edgeLen * 0.46 / turn);
    const double r = std::min(node.edgeFillet, std::max(limit, 0.0));
    return r > 0.04 ? r : 0;
  } else if (shape == QLatin1String("wedge") || shape == QLatin1String("pyramid")) {
    std::vector<EdgeSeg> all;
    straightSegs(node, all);
    double shortest = 1e9;
    bool any = false;
    for (const EdgeSeg& edge : all)
      if (edgeWanted(node.filletEdges, edge.key)) {
        shortest = std::min(shortest, v3len(v3sub(edge.b, edge.a)));
        any = true;
      }
    if (!any) return 0;
    const double cap = 0.4 * std::min(shortest, std::min(node.size[0], std::min(node.size[1], node.size[2])));
    const double r = std::min(node.edgeFillet, std::max(cap, 0.0));
    return r > 0.04 ? r : 0;
  }
  if (limit < 0) return 0;
  const double used = std::min(node.edgeFillet, std::max(limit, 0.0));
  return used > 0.02 ? used : 0;
}

Manifold filletStraight(const Node& node, int segs, QString* warning) {
  Manifold solid = primitiveOf(node, 48);

  std::vector<EdgeSeg> all;
  straightSegs(node, all);
  std::vector<EdgeSeg> use;
  for (const EdgeSeg& edge : all)
    if (edgeWanted(node.filletEdges, edge.key)) use.push_back(edge);
  if (use.empty()) return solid;
  double shortest = 1e9;
  for (const EdgeSeg& edge : use) shortest = std::min(shortest, v3len(v3sub(edge.b, edge.a)));
  double cap = shortest * 0.4;
  if (node.shape == QLatin1String("prism") || node.shape == QLatin1String("diamond")) cap = std::min(cap, std::min(node.size[2], node.radius) * 0.4);
  else cap = std::min(cap, 0.4 * std::min(node.size[0], std::min(node.size[1], node.size[2])));
  const double r = std::min(std::max(node.edgeFillet, 0.0), std::max(cap, 0.0));
  if (!(r > 0.04) || !healthy(solid)) {
    if (node.edgeFillet > 0.04) note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
    return solid;
  }

  struct Corner {
    V3 pos;
    int meet = 0;
    V3 normals[3];
    int ncount = 0;
    bool sphere = false;
    V3 center;
  };
  std::vector<Corner> corners;
  auto keepNormal = [](Corner& corner, V3 n) {
    n = v3norm(n);
    if (v3len(n) < 0.5 || corner.ncount > 3) return;
    for (int i = 0; i < std::min(corner.ncount, 3); ++i) {
      if (v3dot(corner.normals[i], n) > 0.985) return;
    }
    if (corner.ncount >= 3) {
      corner.ncount = 4;
      return;
    }
    corner.normals[corner.ncount++] = n;
  };
  auto cornerIndex = [&](V3 p) {
    for (int i = 0; i < static_cast<int>(corners.size()); ++i) {
      if (v3len(v3sub(corners[static_cast<size_t>(i)].pos, p)) < 0.05) return i;
    }
    Corner corner;
    corner.pos = p;
    corners.push_back(corner);
    return static_cast<int>(corners.size()) - 1;
  };
  for (const EdgeSeg& edge : use) {
    const int i0 = cornerIndex(edge.a);
    const int i1 = cornerIndex(edge.b);
    corners[static_cast<size_t>(i0)].meet++;
    corners[static_cast<size_t>(i1)].meet++;
    keepNormal(corners[static_cast<size_t>(i0)], edge.n0);
    keepNormal(corners[static_cast<size_t>(i0)], edge.n1);
    keepNormal(corners[static_cast<size_t>(i1)], edge.n0);
    keepNormal(corners[static_cast<size_t>(i1)], edge.n1);
  }
  for (Corner& corner : corners) {
    if (corner.meet < 2 || corner.ncount != 3) continue;
    V3 delta;
    if (!solveNormal(corner.normals[0], corner.normals[1], corner.normals[2], V3{-r, -r, -r}, &delta)) continue;
    if (v3len(delta) > r * 8) continue;
    corner.sphere = true;
    corner.center = v3add(corner.pos, delta);
  }

  auto grow = [](V3* lo, V3* hi, V3 p) {
    lo->x = std::min(lo->x, p.x);
    lo->y = std::min(lo->y, p.y);
    lo->z = std::min(lo->z, p.z);
    hi->x = std::max(hi->x, p.x);
    hi->y = std::max(hi->y, p.y);
    hi->z = std::max(hi->z, p.z);
  };
  std::vector<Manifold> bites;
  auto pushBite = [&](Manifold region, Manifold round) {
    try {
      if (!healthy(region) || !healthy(round)) return;
      Manifold bite = region - round;
      if (healthy(bite)) bites.push_back(std::move(bite));
    } catch (const std::exception&) {
    }
  };

  for (const EdgeSeg& edge : use) {
    const double nd = std::clamp(v3dot(edge.n0, edge.n1), -0.92, 0.98);
    if (1.0 + nd < 0.08) {
      note(warning, QStringLiteral("One edge was too sharp to fillet."));
      continue;
    }
    const double k = r / (1.0 + nd);
    const V3 inset = v3mul(v3add(edge.n0, edge.n1), k);
    const V3 dir = v3norm(v3sub(edge.b, edge.a));
    const double length = v3len(v3sub(edge.b, edge.a));
    const Corner* end0 = nullptr;
    const Corner* end1 = nullptr;
    for (const Corner& corner : corners) {
      if (v3len(v3sub(corner.pos, edge.a)) < 0.05) end0 = &corner;
      if (v3len(v3sub(corner.pos, edge.b)) < 0.05) end1 = &corner;
    }
    double s0 = 0;
    double s1 = length;
    if (end0 && end0->sphere) s0 = std::max(0.0, v3dot(v3sub(end0->center, edge.a), dir) - 0.15);
    if (end1 && end1->sphere) s1 = std::min(length, v3dot(v3sub(end1->center, edge.a), dir) + 0.15);
    if (s1 - s0 < 0.12) {
      s0 = 0;
      s1 = length;
    }
    const V3 p0 = v3add(edge.a, v3mul(dir, s0));
    const V3 p1 = v3add(edge.a, v3mul(dir, s1));
    V3 lo = p0;
    V3 hi = p0;
    grow(&lo, &hi, p0);
    grow(&lo, &hi, p1);
    grow(&lo, &hi, v3sub(p0, inset));
    grow(&lo, &hi, v3sub(p1, inset));
    grow(&lo, &hi, v3add(p0, v3mul(edge.n0, r + 0.5)));
    grow(&lo, &hi, v3add(p0, v3mul(edge.n1, r + 0.5)));
    Manifold region = aabb(lo.x - 0.4, hi.x + 0.4, lo.y - 0.4, hi.y + 0.4, lo.z - 0.4, hi.z + 0.4);
    const double o0 = v3dot(edge.a, edge.n0);
    const double o1 = v3dot(edge.a, edge.n1);
    // TrimByPlane keeps the side the normal points to: the region is the slab between r inside
    // each face and a little outside it, along the edge from p0 to p1.
    region = region.TrimByPlane(toVec(v3mul(edge.n0, -1)), -(o0 + 0.4));
    region = region.TrimByPlane(toVec(edge.n0), o0 - r);
    region = region.TrimByPlane(toVec(v3mul(edge.n1, -1)), -(o1 + 0.4));
    region = region.TrimByPlane(toVec(edge.n1), o1 - r);
    region = region.TrimByPlane(toVec(v3mul(dir, -1)), -(v3dot(p1, dir) + 0.001));
    region = region.TrimByPlane(toVec(dir), v3dot(p0, dir) - 0.001);
    const V3 c0 = v3sub(v3sub(p0, v3mul(dir, 0.28)), inset);
    const V3 c1 = v3sub(v3add(p1, v3mul(dir, 0.28)), inset);
    pushBite(std::move(region), cylinderAlong(c0, c1, r, segs));
  }

  for (const Corner& corner : corners) {
    if (!corner.sphere) continue;
    V3 lo = corner.pos;
    V3 hi = corner.pos;
    grow(&lo, &hi, corner.center);
    for (int i = 0; i < corner.ncount; ++i) grow(&lo, &hi, v3add(corner.pos, v3mul(corner.normals[i], r + 0.5)));
    Manifold region = aabb(lo.x - 0.4, hi.x + 0.4, lo.y - 0.4, hi.y + 0.4, lo.z - 0.4, hi.z + 0.4);
    for (int i = 0; i < 3; ++i) {
      const double offset = v3dot(corner.pos, corner.normals[i]);
      region = region.TrimByPlane(toVec(v3mul(corner.normals[i], -1)), -(offset + 0.45));
      region = region.TrimByPlane(toVec(corner.normals[i]), offset - r);
    }
    pushBite(std::move(region), Manifold::Sphere(r, segs).Translate(toVec(corner.center)));
  }

  if (bites.empty()) {
    note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
    return solid;
  }
  try {
    Manifold cut = solid;
    for (auto& bite : bites) {
      Manifold next = cut - bite;
      if (!healthy(next)) {
        note(warning, QStringLiteral("That edge fillet did not fit. Try a smaller radius."));
        continue;
      }
      cut = std::move(next);
    }
    return healthy(cut) ? cut : solid;
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return solid;
  }
}

struct ScanKey {
  int64_t x = 0;
  int64_t y = 0;
  int64_t z = 0;
  bool operator==(const ScanKey& other) const { return x == other.x && y == other.y && z == other.z; }
};

struct ScanHash {
  size_t operator()(const ScanKey& key) const {
    size_t h = std::hash<int64_t>{}(key.x);
    h ^= std::hash<int64_t>{}(key.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= std::hash<int64_t>{}(key.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
  }
};

ScanKey scanKeyOf(V3 p) {
  return {std::llround(p.x * 50.0), std::llround(p.y * 50.0), std::llround(p.z * 50.0)};
}

bool pointInsideMesh(const std::vector<V3>& verts, const std::vector<int>& tris, V3 point) {
  const V3 dirs[3] = {{1, 0.173, 0.081}, {0.137, 1, 0.241}, {0.211, 0.093, 1}};
  for (const V3& raw : dirs) {
    const V3 dir = v3norm(raw);
    int hits = 0;
    bool edge = false;
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
      const V3& a = verts[static_cast<size_t>(tris[i])];
      const V3& b = verts[static_cast<size_t>(tris[i + 1])];
      const V3& c = verts[static_cast<size_t>(tris[i + 2])];
      const V3 e1 = v3sub(b, a);
      const V3 e2 = v3sub(c, a);
      const V3 pvec = v3cross(dir, e2);
      const double det = v3dot(e1, pvec);
      if (std::abs(det) < 1e-9) continue;
      const double inv = 1.0 / det;
      const V3 s = v3sub(point, a);
      const double u = inv * v3dot(s, pvec);
      const V3 q = v3cross(s, e1);
      const double v = inv * v3dot(dir, q);
      const double t = inv * v3dot(e2, q);
      if (t <= 1e-5) continue;
      if (u < -1e-5 || v < -1e-5 || u + v > 1.0 + 1e-5) continue;
      if (u < 1e-4 || v < 1e-4 || u + v > 1.0 - 1e-4) {
        edge = true;
        break;
      }
      ++hits;
    }
    if (!edge) return (hits % 2) == 1;
  }
  return false;
}

Manifold flattenCoplanarSteps(Manifold solid) {
  // A boolean union of two same-height parts leaves a hairline step on the
  // shared top. Face and edge detection then stop at that riser. Snap vertices
  // that already sit on a large face onto that face when the gap is under 0.4mm.
  if (!healthy(solid)) return solid;
  const auto gl = solid.GetMeshGL();
  const uint32_t stride = std::max<uint32_t>(3, gl.numProp);
  if (gl.triVerts.size() < 3 || gl.vertProperties.size() < stride) return solid;
  const size_t rawCount = gl.vertProperties.size() / stride;
  std::vector<uint32_t> topo(rawCount);
  std::iota(topo.begin(), topo.end(), 0u);
  auto findTopo = [&](uint32_t value) {
    uint32_t root = value;
    while (root < topo.size() && topo[root] != root) root = topo[root];
    while (value < topo.size() && topo[value] != value) {
      const uint32_t next = topo[value];
      topo[value] = root;
      value = next;
    }
    return root;
  };
  const size_t mergeCount = std::min(gl.mergeFromVert.size(), gl.mergeToVert.size());
  for (size_t i = 0; i < mergeCount; ++i) {
    uint32_t a = gl.mergeFromVert[i];
    uint32_t b = gl.mergeToVert[i];
    if (a >= topo.size() || b >= topo.size()) continue;
    a = findTopo(a);
    b = findTopo(b);
    if (a != b) topo[a] = b;
  }
  std::unordered_map<ScanKey, int, ScanHash> weld;
  std::vector<V3> verts;
  std::vector<int> map(rawCount, -1);
  for (size_t i = 0; i < rawCount; ++i) {
    const uint32_t root = findTopo(topo[i]);
    const size_t base = static_cast<size_t>(root) * stride;
    const V3 p{gl.vertProperties[base], gl.vertProperties[base + 1], gl.vertProperties[base + 2]};
    const ScanKey key = scanKeyOf(p);
    const auto found = weld.find(key);
    if (found != weld.end()) {
      map[i] = found->second;
      continue;
    }
    map[i] = static_cast<int>(verts.size());
    weld.emplace(key, map[i]);
    verts.push_back(p);
  }
  std::vector<int> tris;
  tris.reserve(gl.triVerts.size());
  for (size_t t = 0; t + 2 < gl.triVerts.size(); t += 3) {
    int id[3];
    bool ok = true;
    for (int k = 0; k < 3; ++k) {
      const uint32_t index = gl.triVerts[t + static_cast<size_t>(k)];
      if (index >= map.size()) {
        ok = false;
        break;
      }
      id[k] = map[index];
    }
    if (!ok || id[0] == id[1] || id[1] == id[2] || id[0] == id[2]) continue;
    tris.push_back(id[0]);
    tris.push_back(id[1]);
    tris.push_back(id[2]);
  }
  const int faceCount = static_cast<int>(tris.size() / 3);
  if (faceCount < 1 || verts.empty()) return solid;
  struct Plane {
    V3 nSum;
    double oSum = 0;
    double area = 0;
    V3 seedN;
    double seedO = 0;
  };
  std::vector<Plane> planes;
  std::vector<int> facePlane(static_cast<size_t>(faceCount), -1);
  std::vector<V3> faceNormal(static_cast<size_t>(faceCount));
  for (int face = 0; face < faceCount; ++face) {
    const V3& a = verts[static_cast<size_t>(tris[static_cast<size_t>(face * 3)])];
    const V3& b = verts[static_cast<size_t>(tris[static_cast<size_t>(face * 3 + 1)])];
    const V3& c = verts[static_cast<size_t>(tris[static_cast<size_t>(face * 3 + 2)])];
    const V3 raw = v3cross(v3sub(b, a), v3sub(c, a));
    const double area = 0.5 * v3len(raw);
    const V3 n = area > 1e-8 ? v3norm(raw) : V3{0, 0, 0};
    faceNormal[static_cast<size_t>(face)] = n;
    if (!(area > 1e-6) || v3len(n) < 0.5) continue;
    const double offset = v3dot(n, a);
    int foundPlane = -1;
    for (int p = 0; p < static_cast<int>(planes.size()); ++p) {
      if (v3dot(n, planes[static_cast<size_t>(p)].seedN) > 0.998 &&
          std::abs(offset - planes[static_cast<size_t>(p)].seedO) < 0.4) {
        foundPlane = p;
        break;
      }
    }
    if (foundPlane < 0) {
      Plane plane;
      plane.nSum = v3mul(n, area);
      plane.oSum = offset * area;
      plane.area = area;
      plane.seedN = n;
      plane.seedO = offset;
      planes.push_back(plane);
      foundPlane = static_cast<int>(planes.size()) - 1;
    } else {
      Plane& plane = planes[static_cast<size_t>(foundPlane)];
      plane.nSum = v3add(plane.nSum, v3mul(n, area));
      plane.oSum += offset * area;
      plane.area += area;
    }
    facePlane[static_cast<size_t>(face)] = foundPlane;
  }
  if (planes.empty()) return solid;
  std::vector<std::vector<int>> incident(verts.size());
  for (int face = 0; face < faceCount; ++face) {
    for (int k = 0; k < 3; ++k) incident[static_cast<size_t>(tris[static_cast<size_t>(face * 3 + k)])].push_back(face);
  }
  int snaps = 0;
  for (int v = 0; v < static_cast<int>(verts.size()); ++v) {
    int best = -1;
    double bestArea = 0;
    for (int face : incident[static_cast<size_t>(v)]) {
      const int planeIndex = facePlane[static_cast<size_t>(face)];
      if (planeIndex < 0) continue;
      const Plane& plane = planes[static_cast<size_t>(planeIndex)];
      if (plane.area <= bestArea) continue;
      const V3 n = v3norm(plane.nSum);
      if (v3len(n) < 0.5 || !(plane.area > 1e-6)) continue;
      const double offset = plane.oSum / plane.area;
      if (std::abs(v3dot(n, verts[static_cast<size_t>(v)]) - offset) > 0.4) continue;
      best = planeIndex;
      bestArea = plane.area;
    }
    if (best < 0 || bestArea < 1.0) continue;
    const Plane& plane = planes[static_cast<size_t>(best)];
    const V3 n = v3norm(plane.nSum);
    const double offset = plane.oSum / plane.area;
    bool aligned = false;
    bool blocked = false;
    for (int face : incident[static_cast<size_t>(v)]) {
      const double d = std::abs(v3dot(faceNormal[static_cast<size_t>(face)], n));
      if (d > 0.95) aligned = true;
      else if (d > 0.25) blocked = true;
    }
    if (blocked || !aligned) continue;
    const double dist = v3dot(n, verts[static_cast<size_t>(v)]) - offset;
    if (std::abs(dist) < 1e-5 || std::abs(dist) > 0.4) continue;
    verts[static_cast<size_t>(v)] = v3sub(verts[static_cast<size_t>(v)], v3mul(n, dist));
    ++snaps;
  }
  if (!snaps) return solid;
  try {
    manifold::MeshGL mesh;
    mesh.numProp = 3;
    std::unordered_map<ScanKey, uint32_t, ScanHash> canon;
    std::vector<uint32_t> indices;
    indices.reserve(tris.size());
    auto idOf = [&](V3 p) {
      const ScanKey key{std::llround(p.x * 20.0), std::llround(p.y * 20.0), std::llround(p.z * 20.0)};
      const auto found = canon.find(key);
      if (found != canon.end()) return found->second;
      const uint32_t id = static_cast<uint32_t>(mesh.vertProperties.size() / 3);
      mesh.vertProperties.push_back(static_cast<float>(p.x));
      mesh.vertProperties.push_back(static_cast<float>(p.y));
      mesh.vertProperties.push_back(static_cast<float>(p.z));
      canon.emplace(key, id);
      return id;
    };
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
      const V3& a = verts[static_cast<size_t>(tris[t])];
      const V3& b = verts[static_cast<size_t>(tris[t + 1])];
      const V3& c = verts[static_cast<size_t>(tris[t + 2])];
      if (v3len(v3cross(v3sub(b, a), v3sub(c, a))) < 1e-6) continue;
      const uint32_t ia = idOf(a);
      const uint32_t ib = idOf(b);
      const uint32_t ic = idOf(c);
      if (ia == ib || ib == ic || ia == ic) continue;
      indices.push_back(ia);
      indices.push_back(ib);
      indices.push_back(ic);
    }
    if (indices.size() < 12) return solid;
    mesh.triVerts = std::move(indices);
    Manifold built(mesh);
    if (!healthy(built)) return solid;
    const double before = std::abs(solid.Volume());
    const double after = std::abs(built.Volume());
    if (before > 1e-4 && std::abs(after - before) > before * 0.15) return solid;
    return built;
  } catch (const std::exception&) {
    return solid;
  }
}

std::vector<EdgeSeg> scanSharpEdges(const Manifold& solid) {
  std::vector<EdgeSeg> none;
  if (!healthy(solid)) return none;
  const auto gl = solid.GetMeshGL();
  const uint32_t stride = std::max<uint32_t>(3, gl.numProp);
  if (gl.triVerts.size() < 3 || gl.vertProperties.size() < stride) return none;

  const size_t rawCount = gl.vertProperties.size() / stride;
  std::vector<uint32_t> topo(rawCount);
  std::iota(topo.begin(), topo.end(), 0u);
  auto findTopo = [&](uint32_t value) {
    uint32_t root = value;
    while (root < topo.size() && topo[root] != root) root = topo[root];
    while (value < topo.size() && topo[value] != value) {
      const uint32_t next = topo[value];
      topo[value] = root;
      value = next;
    }
    return root;
  };
  const size_t mergeCount = std::min(gl.mergeFromVert.size(), gl.mergeToVert.size());
  for (size_t i = 0; i < mergeCount; ++i) {
    uint32_t a = gl.mergeFromVert[i];
    uint32_t b = gl.mergeToVert[i];
    if (a >= topo.size() || b >= topo.size()) continue;
    a = findTopo(a);
    b = findTopo(b);
    if (a != b) topo[a] = b;
  }
  for (uint32_t i = 0; i < static_cast<uint32_t>(rawCount); ++i) topo[i] = findTopo(i);

  std::unordered_map<ScanKey, int, ScanHash> weld;
  std::vector<V3> verts;
  std::vector<int> map(rawCount, -1);
  for (size_t i = 0; i < rawCount; ++i) {
    const uint32_t root = topo[i];
    const size_t base = static_cast<size_t>(root) * stride;
    const V3 p{gl.vertProperties[base], gl.vertProperties[base + 1], gl.vertProperties[base + 2]};
    const ScanKey key = scanKeyOf(p);
    const auto found = weld.find(key);
    if (found != weld.end()) {
      map[i] = found->second;
      continue;
    }
    map[i] = static_cast<int>(verts.size());
    weld.emplace(key, map[i]);
    verts.push_back(p);
  }

  struct Half {
    int face = -1;
    V3 n;
    int count = 1;
  };
  struct EdgeRef {
    int a = 0;
    int b = 0;
    bool operator==(const EdgeRef& other) const { return a == other.a && b == other.b; }
  };
  struct EdgeRefHash {
    size_t operator()(const EdgeRef& edge) const {
      return std::hash<int>{}(edge.a) ^ (std::hash<int>{}(edge.b) + 0x9e3779b9 + (std::hash<int>{}(edge.a) << 6));
    }
  };
  std::unordered_map<EdgeRef, Half, EdgeRefHash> halves;
  std::vector<int> tris;
  tris.reserve(gl.triVerts.size());
  for (size_t t = 0; t + 2 < gl.triVerts.size(); t += 3) {
    int id[3];
    bool ok = true;
    for (int k = 0; k < 3; ++k) {
      const uint32_t index = gl.triVerts[t + static_cast<size_t>(k)];
      if (index >= map.size()) {
        ok = false;
        break;
      }
      id[k] = map[index];
    }
    if (!ok || id[0] == id[1] || id[1] == id[2] || id[0] == id[2]) continue;
    const V3 n = v3norm(v3cross(v3sub(verts[static_cast<size_t>(id[1])], verts[static_cast<size_t>(id[0])]),
                                v3sub(verts[static_cast<size_t>(id[2])], verts[static_cast<size_t>(id[0])])));
    if (v3len(n) < 0.5) continue;
    const int face = static_cast<int>(tris.size() / 3);
    tris.push_back(id[0]);
    tris.push_back(id[1]);
    tris.push_back(id[2]);
    for (int k = 0; k < 3; ++k) {
      EdgeRef edge{id[k], id[(k + 1) % 3]};
      const auto existing = halves.find(edge);
      if (existing == halves.end()) halves.emplace(edge, Half{face, n, 1});
      else existing->second.count++;
    }
  }

  bool normalsPointIn = false;
  {
    int outCount = 0;
    int inCount = 0;
    const int faces = static_cast<int>(tris.size() / 3);
    const int step = std::max(1, faces / 16);
    for (int face = 0; face < faces && inCount + outCount < 16; face += step) {
      const int base = face * 3;
      const V3& a = verts[static_cast<size_t>(tris[static_cast<size_t>(base)])];
      const V3& b = verts[static_cast<size_t>(tris[static_cast<size_t>(base + 1)])];
      const V3& c = verts[static_cast<size_t>(tris[static_cast<size_t>(base + 2)])];
      const V3 n = v3norm(v3cross(v3sub(b, a), v3sub(c, a)));
      if (v3len(n) < 0.5) continue;
      const V3 mid = v3mul(v3add(a, v3add(b, c)), 1.0 / 3.0);
      if (pointInsideMesh(verts, tris, v3add(mid, v3mul(n, 0.05)))) ++inCount;
      else ++outCount;
    }
    normalsPointIn = inCount > outCount;
  }

  struct Raw {
    V3 a;
    V3 b;
    V3 n0;
    V3 n1;
    bool concave = false;
    bool used = false;
  };
  std::vector<Raw> raws;
  for (const auto& item : halves) {
    if (item.first.a > item.first.b) continue;
    const auto back = halves.find(EdgeRef{item.first.b, item.first.a});
    if (back == halves.end()) continue;
    if (item.second.count != 1 || back->second.count != 1) continue;
    V3 n0 = item.second.n;
    V3 n1 = back->second.n;
    if (normalsPointIn) {
      n0 = v3mul(n0, -1);
      n1 = v3mul(n1, -1);
    }
    const double nd = std::clamp(v3dot(n0, n1), -1.0, 1.0);
    if (nd > 0.951) continue;
    const V3 a = verts[static_cast<size_t>(item.first.a)];
    const V3 b = verts[static_cast<size_t>(item.first.b)];
    if (v3len(v3sub(b, a)) < 0.05) continue;
    auto opposite = [&](int face) {
      const int base = face * 3;
      for (int k = 0; k < 3; ++k) {
        const int id = tris[static_cast<size_t>(base + k)];
        if (id != item.first.a && id != item.first.b) return verts[static_cast<size_t>(id)];
      }
      return a;
    };
    const double side0 = v3dot(n0, v3sub(opposite(back->second.face), a));
    const double side1 = v3dot(n1, v3sub(opposite(item.second.face), a));
    bool convexEdge = false;
    bool concaveEdge = false;
    // Same side of both faces is a real edge. Opposite signs are a crossing
    // or a sliver, and guessing there carved the star open. A tiny same-sign
    // side still counts, or a long frame edge next to a sliver disappears.
    if (side0 * side1 > 0.0) {
      if (side0 < 0.0) convexEdge = true;
      else concaveEdge = true;
    }
    if (convexEdge) raws.push_back(Raw{a, b, n0, n1, false, false});
    else if (concaveEdge) raws.push_back(Raw{a, b, n0, n1, true, false});
  }

  std::unordered_map<ScanKey, std::vector<int>, ScanHash> at;
  for (int i = 0; i < static_cast<int>(raws.size()); ++i) {
    at[scanKeyOf(raws[static_cast<size_t>(i)].a)].push_back(i);
    at[scanKeyOf(raws[static_cast<size_t>(i)].b)].push_back(i);
  }
  auto sameSide = [](V3 a, V3 b) { return v3dot(a, b) > 0.92; };
  auto normalsMatch = [&](const Raw& left, const Raw& right) {
    return (sameSide(left.n0, right.n0) && sameSide(left.n1, right.n1)) ||
           (sameSide(left.n0, right.n1) && sameSide(left.n1, right.n0));
  };
  std::vector<EdgeSeg> chained;
  for (int start = 0; start < static_cast<int>(raws.size()); ++start) {
    if (raws[static_cast<size_t>(start)].used) continue;
    raws[static_cast<size_t>(start)].used = true;
    V3 endA = raws[static_cast<size_t>(start)].a;
    V3 endB = raws[static_cast<size_t>(start)].b;
    const V3 n0 = raws[static_cast<size_t>(start)].n0;
    const V3 n1 = raws[static_cast<size_t>(start)].n1;
    const bool concave = raws[static_cast<size_t>(start)].concave;
    auto extend = [&](V3* end, V3 prev) {
      for (int guard = 0; guard < 64; ++guard) {
        const V3 dir = v3norm(v3sub(*end, prev));
        int next = -1;
        V3 nextPoint;
        const auto found = at.find(scanKeyOf(*end));
        if (found == at.end()) return;
        for (int candidate : found->second) {
          Raw& raw = raws[static_cast<size_t>(candidate)];
          if (raw.used || raw.concave != concave || !normalsMatch(raws[static_cast<size_t>(start)], raw)) continue;
          const bool atA = v3len(v3sub(raw.a, *end)) < 0.08;
          const bool atB = v3len(v3sub(raw.b, *end)) < 0.08;
          if (!atA && !atB) continue;
          const V3 other = atA ? raw.b : raw.a;
          if (v3dot(v3norm(v3sub(other, *end)), dir) < 0.996) continue;
          if (at[scanKeyOf(*end)].size() != 2) return;
          next = candidate;
          nextPoint = other;
          break;
        }
        if (next < 0) return;
        raws[static_cast<size_t>(next)].used = true;
        prev = *end;
        *end = nextPoint;
      }
    };
    extend(&endA, endB);
    extend(&endB, endA);
    if (v3len(v3sub(endB, endA)) < 0.35) continue;
    chained.push_back(EdgeSeg{0, endA, endB, n0, n1, concave});
  }

  constexpr int kCap = 256;
  if (static_cast<int>(chained.size()) > kCap) {
    std::sort(chained.begin(), chained.end(), [](const EdgeSeg& left, const EdgeSeg& right) {
      return v3len(v3sub(left.b, left.a)) > v3len(v3sub(right.b, right.a));
    });
    chained.resize(static_cast<size_t>(kCap));
  }
  std::sort(chained.begin(), chained.end(), [](const EdgeSeg& left, const EdgeSeg& right) {
    const V3 ml = v3mul(v3add(left.a, left.b), 0.5);
    const V3 mr = v3mul(v3add(right.a, right.b), 0.5);
    if (std::abs(ml.x - mr.x) > 0.01) return ml.x < mr.x;
    if (std::abs(ml.y - mr.y) > 0.01) return ml.y < mr.y;
    return ml.z < mr.z;
  });
  for (int i = 0; i < static_cast<int>(chained.size()); ++i) chained[static_cast<size_t>(i)].key = i;
  return chained;
}

Manifold filletScanned(Manifold solid, const std::vector<EdgeSeg>& edges, double radius, int segs, bool concave,
                       const std::vector<int>& onlyKeys, QString* warning) {
  if (!(radius > 0.02) || !healthy(solid)) return solid;
  std::vector<EdgeSeg> use;
  for (const EdgeSeg& edge : edges) {
    if (edge.concave != concave) continue;
    if (!onlyKeys.empty() && !listedEdge(onlyKeys, edge.key)) continue;
    use.push_back(edge);
  }
  if (use.empty()) {
    note(warning, concave ? QStringLiteral("No sharp inside corner to round.")
                          : QStringLiteral("No sharp outside edge to fillet."));
    return solid;
  }
  double shortest = 1e9;
  for (const EdgeSeg& edge : use) shortest = std::min(shortest, v3len(v3sub(edge.b, edge.a)));
  double r = std::min(radius, std::max(0.0, shortest * 0.45));
  if (!(r > 0.04)) {
    note(warning, QStringLiteral("That fillet did not fit. Try a smaller radius."));
    return solid;
  }
  if (r < radius - 0.05) note(warning, QStringLiteral("Fillet radius was reduced so it would fit the shortest edge."));

  struct Corner {
    V3 pos;
    int meet = 0;
    V3 normals[6];
    int ncount = 0;
    bool sphere = false;
    V3 center;
  };
  std::vector<Corner> corners;
  auto keepNormal = [](Corner& corner, V3 n) {
    n = v3norm(n);
    if (v3len(n) < 0.5 || corner.ncount > 6) return;
    for (int i = 0; i < std::min(corner.ncount, 6); ++i) {
      if (v3dot(corner.normals[i], n) > 0.985) return;
    }
    if (corner.ncount >= 6) {
      corner.ncount = 7;
      return;
    }
    corner.normals[corner.ncount++] = n;
  };
  auto cornerIndex = [&](V3 p) {
    for (int i = 0; i < static_cast<int>(corners.size()); ++i) {
      if (v3len(v3sub(corners[static_cast<size_t>(i)].pos, p)) < 0.08) return i;
    }
    Corner corner;
    corner.pos = p;
    corners.push_back(corner);
    return static_cast<int>(corners.size()) - 1;
  };
  for (const EdgeSeg& edge : use) {
    const int i0 = cornerIndex(edge.a);
    const int i1 = cornerIndex(edge.b);
    corners[static_cast<size_t>(i0)].meet++;
    corners[static_cast<size_t>(i1)].meet++;
    keepNormal(corners[static_cast<size_t>(i0)], edge.n0);
    keepNormal(corners[static_cast<size_t>(i0)], edge.n1);
    keepNormal(corners[static_cast<size_t>(i1)], edge.n0);
    keepNormal(corners[static_cast<size_t>(i1)], edge.n1);
  }
  for (Corner& corner : corners) {
    if (corner.meet < 2 || corner.ncount < 3 || corner.ncount > 6) continue;
    const V3 goal = concave ? V3{r, r, r} : V3{-r, -r, -r};
    double bestDet = 0;
    V3 bestDelta;
    V3 picked[3];
    bool foundCorner = false;
    for (int i = 0; i < corner.ncount; ++i) {
      for (int j = i + 1; j < corner.ncount; ++j) {
        for (int k = j + 1; k < corner.ncount; ++k) {
          const V3& n0 = corner.normals[i];
          const V3& n1 = corner.normals[j];
          const V3& n2 = corner.normals[k];
          if (v3dot(n0, n1) > 0.92 || v3dot(n0, n2) > 0.92 || v3dot(n1, n2) > 0.92) continue;
          const double det = std::abs(v3dot(n0, v3cross(n1, n2)));
          if (det < 0.15 || det <= bestDet) continue;
          V3 delta;
          if (!solveNormal(n0, n1, n2, goal, &delta)) continue;
          if (v3len(delta) > r * 8) continue;
          bestDet = det;
          bestDelta = delta;
          picked[0] = n0;
          picked[1] = n1;
          picked[2] = n2;
          foundCorner = true;
        }
      }
    }
    if (!foundCorner) continue;
    corner.normals[0] = picked[0];
    corner.normals[1] = picked[1];
    corner.normals[2] = picked[2];
    corner.ncount = 3;
    corner.sphere = true;
    corner.center = v3add(corner.pos, bestDelta);
  }

  auto grow = [](V3* lo, V3* hi, V3 p) {
    lo->x = std::min(lo->x, p.x);
    lo->y = std::min(lo->y, p.y);
    lo->z = std::min(lo->z, p.z);
    hi->x = std::max(hi->x, p.x);
    hi->y = std::max(hi->y, p.y);
    hi->z = std::max(hi->z, p.z);
  };

  std::vector<Manifold> bites;
  std::vector<Manifold> fills;
  auto pushBite = [&](Manifold region, Manifold round, double limit) {
    try {
      if (!healthy(region) || !healthy(round)) return;
      const double regionVolume = region.Volume();
      Manifold bite = region - round;
      if (!healthy(bite)) return;
      const double biteVolume = bite.Volume();
      if (biteVolume <= 1e-6) return;
      if (regionVolume > 1e-6 && biteVolume > regionVolume * 0.82) return;
      if (limit > 0.0 && biteVolume > limit) return;
      bites.push_back(std::move(bite));
    } catch (const std::exception&) {
    }
  };
  auto pushFill = [&](Manifold piece) {
    if (healthy(piece)) fills.push_back(std::move(piece));
  };

  for (const EdgeSeg& edge : use) {
    const double nd = std::clamp(v3dot(edge.n0, edge.n1), -0.92, 0.98);
    if (1.0 + nd < 0.08) {
      note(warning, QStringLiteral("One corner was too sharp to fillet."));
      continue;
    }
    const double k = r / (1.0 + nd);
    const V3 shift = v3mul(v3add(edge.n0, edge.n1), k);
    const V3 inset = concave ? v3mul(shift, -1) : shift;
    const V3 dir = v3norm(v3sub(edge.b, edge.a));
    const double length = v3len(v3sub(edge.b, edge.a));
    const Corner* end0 = nullptr;
    const Corner* end1 = nullptr;
    for (const Corner& corner : corners) {
      if (v3len(v3sub(corner.pos, edge.a)) < 0.08) end0 = &corner;
      if (v3len(v3sub(corner.pos, edge.b)) < 0.08) end1 = &corner;
    }
    double s0 = 0;
    double s1 = length;
    const bool sphere0 = end0 && end0->sphere;
    const bool sphere1 = end1 && end1->sphere;
    if (sphere0) s0 = std::max(0.0, v3dot(v3sub(end0->center, edge.a), dir) - 0.15);
    else if (!concave) s0 = std::min(r, length * 0.45);
    if (sphere1) s1 = std::min(length, v3dot(v3sub(end1->center, edge.a), dir) + 0.15);
    else if (!concave) s1 = length - std::min(r, length * 0.45);
    if (s1 - s0 < 0.12) {
      if (concave || (sphere0 && sphere1)) {
        s0 = 0;
        s1 = length;
      } else {
        continue;
      }
    }
    const V3 p0 = v3add(edge.a, v3mul(dir, s0));
    const V3 p1 = v3add(edge.a, v3mul(dir, s1));
    const V3 c0 = v3sub(v3sub(p0, v3mul(dir, 0.2)), inset);
    const V3 c1 = v3sub(v3add(p1, v3mul(dir, 0.2)), inset);
    Manifold round = cylinderAlong(c0, c1, r, segs);
    const double o0 = v3dot(edge.a, edge.n0);
    const double o1 = v3dot(edge.a, edge.n1);
    if (concave) {
      V3 lo = p0;
      V3 hi = p0;
      auto cover = [&](V3 p) { grow(&lo, &hi, p); };
      cover(p0);
      cover(p1);
      cover(v3add(p0, v3mul(edge.n0, r + 0.4)));
      cover(v3add(p1, v3mul(edge.n0, r + 0.4)));
      cover(v3add(p0, v3mul(edge.n1, r + 0.4)));
      cover(v3add(p1, v3mul(edge.n1, r + 0.4)));
      cover(v3sub(p0, v3mul(edge.n0, 0.4)));
      cover(v3sub(p1, v3mul(edge.n0, 0.4)));
      cover(v3sub(p0, v3mul(edge.n1, 0.4)));
      cover(v3sub(p1, v3mul(edge.n1, 0.4)));
      try {
        Manifold region = aabb(lo.x - 0.4, hi.x + 0.4, lo.y - 0.4, hi.y + 0.4, lo.z - 0.4, hi.z + 0.4);
        auto airBand = [&](V3 n, double o) {
          region = region.TrimByPlane(toVec(v3mul(n, -1)), -(o - 0.12));
          region = region.TrimByPlane(toVec(n), o + r + 0.02);
        };
        airBand(edge.n0, o0);
        airBand(edge.n1, o1);
        region = region.TrimByPlane(toVec(dir), v3dot(p1, dir) + 0.02);
        region = region.TrimByPlane(toVec(v3mul(dir, -1)), -(v3dot(p0, dir) - 0.02));
        if (healthy(region) && healthy(round)) {
          Manifold fill = region - round;
          const double span = std::max(r, v3len(v3sub(p1, p0)));
          const double limit = std::max(6.0 * r * r, 1.5 * k * k) * span;
          if (healthy(fill) && fill.Volume() > 1e-6 && fill.Volume() < limit) pushFill(std::move(fill));
        }
      } catch (const std::exception&) {
      }
      continue;
    }
    V3 lo = p0;
    V3 hi = p0;
    grow(&lo, &hi, p0);
    grow(&lo, &hi, p1);
    grow(&lo, &hi, v3sub(p0, inset));
    grow(&lo, &hi, v3sub(p1, inset));
    grow(&lo, &hi, v3add(p0, v3mul(edge.n0, r + 0.5)));
    grow(&lo, &hi, v3add(p0, v3mul(edge.n1, r + 0.5)));
    Manifold region = aabb(lo.x - 0.4, hi.x + 0.4, lo.y - 0.4, hi.y + 0.4, lo.z - 0.4, hi.z + 0.4);
    region = region.TrimByPlane(toVec(edge.n0), o0 + 0.4);
    region = region.TrimByPlane(toVec(v3mul(edge.n0, -1)), -(o0 - r));
    region = region.TrimByPlane(toVec(edge.n1), o1 + 0.4);
    region = region.TrimByPlane(toVec(v3mul(edge.n1, -1)), -(o1 - r));
    region = region.TrimByPlane(toVec(dir), v3dot(p1, dir) + 0.001);
    region = region.TrimByPlane(toVec(v3mul(dir, -1)), -(v3dot(p0, dir) - 0.001));
    const double span = std::max(0.05, s1 - s0);
    pushBite(std::move(region), std::move(round), std::max(4.0 * r * r, 2.0 * k * k) * span);
  }

  for (const Corner& corner : corners) {
    if (!corner.sphere) continue;
    if (concave) {
      V3 lo = corner.pos;
      V3 hi = corner.pos;
      grow(&lo, &hi, corner.center);
      for (int i = 0; i < 3; ++i) {
        grow(&lo, &hi, v3add(corner.pos, v3mul(corner.normals[i], r + 0.4)));
        grow(&lo, &hi, v3sub(corner.pos, v3mul(corner.normals[i], 0.4)));
      }
      try {
        Manifold region = aabb(lo.x - 0.4, hi.x + 0.4, lo.y - 0.4, hi.y + 0.4, lo.z - 0.4, hi.z + 0.4);
        Manifold ball = Manifold::Sphere(r, segs).Translate(toVec(corner.center));
        for (int i = 0; i < 3; ++i) {
          const double offset = v3dot(corner.pos, corner.normals[i]);
          region = region.TrimByPlane(toVec(v3mul(corner.normals[i], -1)), -(offset - 0.12));
          region = region.TrimByPlane(toVec(corner.normals[i]), offset + r + 0.02);
        }
        if (healthy(region) && healthy(ball)) {
          Manifold fill = region - ball;
          const double reach = std::max(r, v3len(v3sub(corner.center, corner.pos)));
          if (healthy(fill) && fill.Volume() > 1e-6 && fill.Volume() < std::max(6.0 * r * r * r, 1.5 * reach * reach * reach))
            pushFill(std::move(fill));
        }
      } catch (const std::exception&) {
      }
      continue;
    }
    V3 lo = corner.pos;
    V3 hi = corner.pos;
    grow(&lo, &hi, corner.center);
    for (int i = 0; i < corner.ncount; ++i) grow(&lo, &hi, v3add(corner.pos, v3mul(corner.normals[i], r + 0.5)));
    Manifold region = aabb(lo.x - 0.4, hi.x + 0.4, lo.y - 0.4, hi.y + 0.4, lo.z - 0.4, hi.z + 0.4);
    for (int i = 0; i < 3; ++i) {
      const double offset = v3dot(corner.pos, corner.normals[i]);
      region = region.TrimByPlane(toVec(corner.normals[i]), offset + 0.45);
      region = region.TrimByPlane(toVec(v3mul(corner.normals[i], -1)), -(offset - r));
    }
    const double reach = std::max(r, v3len(v3sub(corner.center, corner.pos)));
    pushBite(std::move(region), Manifold::Sphere(r, segs).Translate(toVec(corner.center)),
             std::max(4.0 * r * r * r, 2.0 * reach * reach * reach));
  }

  if (!concave && bites.empty())
    note(warning, QStringLiteral("Outside edges were left sharp. Try a smaller radius."));
  try {
    Manifold source = solid;
    if (!bites.empty()) {
      for (auto& bite : bites) {
        Manifold next = solid - bite;
        if (!healthy(next)) {
          note(warning, QStringLiteral("One outside edge was left sharp."));
          continue;
        }
        solid = std::move(next);
      }
    }
    if (fills.empty()) {
      if (concave) note(warning, QStringLiteral("Inside corners could not be rounded. Try a smaller radius."));
    } else {
      for (auto& fill : fills) {
        try {
          Manifold next = solid + fill;
          if (!healthy(next)) {
            note(warning, QStringLiteral("One inside corner was left sharp."));
            continue;
          }
          solid = std::move(next);
        } catch (const std::exception&) {
          note(warning, QStringLiteral("One inside corner was left sharp."));
        }
      }
    }
    return healthy(solid) ? solid : source;
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return solid;
  }
}

void addPick(std::vector<PickEdge>& edges, int key, V3 a, V3 b) {
  PickEdge edge;
  edge.key = key;
  edge.a = QVector3D(static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z));
  edge.b = QVector3D(static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z));
  edges.push_back(edge);
}

void addCircle(std::vector<PickEdge>& edges, int key, double radius, double z) {
  if (radius < 0.15) return;
  const int steps = 40;
  for (int i = 0; i < steps; ++i) {
    const double a0 = (kPi * 2 * i) / steps;
    const double a1 = (kPi * 2 * (i + 1)) / steps;
    addPick(edges, key, V3{std::cos(a0) * radius, std::sin(a0) * radius, z},
            V3{std::cos(a1) * radius, std::sin(a1) * radius, z});
  }
}

std::vector<PickEdge> collectPickEdges(const Node& node) {
  std::vector<PickEdge> edges;
  if (node.kind != Node::Kind::Shape) return edges;
  const QString& shape = node.shape;
  if (shape == QLatin1String("box")) {
    const double hx = std::max(node.size[0], 0.05) * 0.5;
    const double hy = std::max(node.size[1], 0.05) * 0.5;
    const double hz = std::max(node.size[2], 0.05) * 0.5;
    for (int y = 0; y < 2; ++y)
      for (int z = 0; z < 2; ++z)
        addPick(edges, y * 2 + z, V3{-hx, y ? hy : -hy, z ? hz : -hz}, V3{hx, y ? hy : -hy, z ? hz : -hz});
    for (int z = 0; z < 2; ++z)
      for (int x = 0; x < 2; ++x)
        addPick(edges, 4 + z * 2 + x, V3{x ? hx : -hx, -hy, z ? hz : -hz}, V3{x ? hx : -hx, hy, z ? hz : -hz});
    for (int x = 0; x < 2; ++x)
      for (int y = 0; y < 2; ++y)
        addPick(edges, 8 + x * 2 + y, V3{x ? hx : -hx, y ? hy : -hy, -hz}, V3{x ? hx : -hx, y ? hy : -hy, hz});
    return edges;
  }
  const double hz = std::max(node.size[2], 0.05) * 0.5;
  if (shape == QLatin1String("cylinder")) {
    const double radius = std::max(node.radius, 0.05);
    addCircle(edges, 0, radius, -hz);
    addCircle(edges, 1, radius, hz);
    return edges;
  }
  if (shape == QLatin1String("cone")) {
    addCircle(edges, 0, std::max(node.radius, 0.05), -hz);
    addCircle(edges, 1, node.radius2, hz);
    return edges;
  }
  if (shape == QLatin1String("tube")) {
    const double outer = std::max(node.radius, 0.4);
    const double inner = std::min(std::max(node.radius2, 0.05), outer - 0.05);
    addCircle(edges, 0, outer, -hz);
    addCircle(edges, 1, outer, hz);
    addCircle(edges, 2, inner, -hz);
    addCircle(edges, 3, inner, hz);
    return edges;
  }
  if (shape == QLatin1String("hemisphere")) {
    addCircle(edges, 0, std::max(node.radius, 0.05), -std::max(node.radius, 0.05) * 0.5);
    return edges;
  }
  // (A cut gem has other edges than the pointed diamond; Edge fillet (Beta) rounds those.)
  const bool gem = shape == QLatin1String("diamond") && node.gemTable > 0.001;
  if (!gem && (shape == QLatin1String("prism") || shape == QLatin1String("diamond") || shape == QLatin1String("wedge") ||
               shape == QLatin1String("pyramid"))) {
    std::vector<EdgeSeg> segs;
    straightSegs(node, segs);
    for (const EdgeSeg& edge : segs) addPick(edges, edge.key, edge.a, edge.b);
  }
  return edges;
}

// Turns every slice across `axis` around that axis, by an angle that grows evenly from one end to
// the other (the middle stays put), like wringing a strip. Works at the part's real size: a box
// stretched by its scale is twisted as the strip it looks like, then handed back unscaled, so the
// part's own scale puts it where it was.
Manifold applyTwist(Manifold source, double degrees, int axis, const double scale[3], QString* warning) {
  if (std::abs(degrees) < 0.05 || !healthy(source)) return source;
  axis = std::clamp(axis, 0, 2);
  const vec3 s(std::abs(scale[0]) > 1e-9 ? scale[0] : 1.0, std::abs(scale[1]) > 1e-9 ? scale[1] : 1.0,
               std::abs(scale[2]) > 1e-9 ? scale[2] : 1.0);
  try {
    Manifold real = source.Scale(s);
    const auto box = real.BoundingBox();
    const double lo = comp(box.min, axis);
    const double len = comp(box.max, axis) - lo;
    if (len < 1e-4) return source;
    const int b = (axis + 1) % 3;
    const int c = (axis + 2) % 3;
    const double cb = (comp(box.min, b) + comp(box.max, b)) * 0.5;
    const double cc = (comp(box.min, c) + comp(box.max, c)) * 0.5;
    // About one step per 1.5 degrees of turn (at least 48 along the length), no finer than 0.2 mm:
    // coarser steps let a thin strip's faces sag between them and the strip gets thinner.
    const double target = std::max(0.2, len / std::max(48.0, std::abs(degrees) / 1.5));
    if (real.NumTri() < 400000) {
      // A slice across the axis turns as one piece, so it is mostly the edges along the axis that
      // need splitting. Across, a flat triangle between two rings bows off the true surface by about
      // edge x step angle / 8. Keep that under 2% of the wall (2 x volume / area is the thickness of
      // a plate) and never over 0.05 mm, a quarter of a print layer: a 1 mm strip comes out at most
      // about 0.02 mm thinner, far below what a printer shows. Squash the cross directions by that ratio, refine and stretch back: rings along
      // the length instead of a fine mesh everywhere.
      const double area = real.SurfaceArea();
      const double wall = area > 1e-9 ? 2.0 * real.Volume() / area : 1.0;
      const double tolerance = std::clamp(wall * 0.02, 0.005, 0.05);
      const double stepAngle = std::abs(degrees) * kPi / 180.0 * target / len;
      const double across = std::max(target, 8.0 * tolerance / std::max(stepAngle, 1e-9));
      vec3 squash(1, 1, 1);
      for (int d : {b, c})
        if (comp(box.max, d) - comp(box.min, d) > across) setComp(squash, d, target / across);
      Manifold refined = real.Scale(squash).RefineToLength(target).Scale(vec3(1.0 / squash.x, 1.0 / squash.y, 1.0 / squash.z));
      if (healthy(refined)) real = std::move(refined);
    }
    const double total = degrees * kPi / 180.0;
    Manifold twisted = real.Warp([&](vec3& v) {
      const double t = ((comp(v, axis) - lo) / len - 0.5) * total;
      const double ct = std::cos(t);
      const double st = std::sin(t);
      const double pb = comp(v, b) - cb;
      const double pc = comp(v, c) - cc;
      setComp(v, b, cb + pb * ct - pc * st);
      setComp(v, c, cc + pb * st + pc * ct);
    });
    twisted = twisted.Scale(vec3(1.0 / s.x, 1.0 / s.y, 1.0 / s.z));
    if (!healthy(twisted)) {
      note(warning, QStringLiteral("Twist failed. Try a smaller angle."));
      return source;
    }
    return twisted;
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return source;
  }
}

// Taper: every slice across `axis` scaled around the part's middle line, from 100% at the low end
// to `percent` at the high end, evenly in between. Flat faces stay flat (a box becomes a frustum, a
// cylinder a cone), so no extra triangles are needed; curved ones already have their own.
Manifold applyTaper(Manifold source, double percent, int axis, QString* warning) {
  if (std::abs(percent - 100) < 0.05 || !healthy(source)) return source;
  axis = std::clamp(axis, 0, 2);
  const double end = std::clamp(percent, 1.0, 1000.0) / 100.0;
  try {
    const auto box = source.BoundingBox();
    const double lo = comp(box.min, axis);
    const double len = comp(box.max, axis) - lo;
    if (len < 1e-4) return source;
    const int b = (axis + 1) % 3;
    const int c = (axis + 2) % 3;
    const double cb = (comp(box.min, b) + comp(box.max, b)) * 0.5;
    const double cc = (comp(box.min, c) + comp(box.max, c)) * 0.5;
    Manifold tapered = source.Warp([&](vec3& v) {
      const double t = std::clamp((comp(v, axis) - lo) / len, 0.0, 1.0);
      const double f = 1 + (end - 1) * t;
      setComp(v, b, cb + (comp(v, b) - cb) * f);
      setComp(v, c, cc + (comp(v, c) - cc) * f);
    });
    if (!healthy(tapered)) {
      note(warning, QStringLiteral("Taper failed. Try a gentler taper."));
      return source;
    }
    return tapered;
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return source;
  }
}

// Bend: the part curved into an arc along `along`, toward `toward`, its middle line keeping its
// length. A bend of a full turn (or more) closes into a ring: the two halves are bent apart and
// joined, overlapping a hair, so the ends weld into one solid instead of touching.
Manifold applyBend(Manifold source, double degrees, int along, int toward, QString* warning) {
  if (std::abs(degrees) < 0.05 || !healthy(source)) return source;
  along = std::clamp(along, 0, 2);
  toward = std::clamp(toward, 0, 2);
  if (along == toward) {
    note(warning, QStringLiteral("Bend axes must differ. Pick a length axis and a different direction."));
    return source;
  }
  try {
    const auto box = source.BoundingBox();
    const double minA = comp(box.min, along);
    const double maxA = comp(box.max, along);
    const double len = maxA - minA;
    if (len < 1e-4) return source;
    const bool ring = std::abs(degrees) >= 359.95 && std::abs(degrees) <= 360.05;
    double angle = degrees * kPi / 180.0;
    // A closing ring overlaps its ends by 0.02 mm along the middle line.
    if (ring) angle *= 1 + 0.02 / len;
    // About one step per 1.5 degrees of curve, no finer than 0.2 mm: coarser steps show as facets
    // on a bracelet. Only the length is split finer (squashed across, refined, stretched back), so
    // the part gets rings of points along the bend, not a fine mesh everywhere.
    const double target = std::max(0.2, len / std::max(48.0, std::abs(degrees) / 1.5));
    Manifold dense = source;
    if (source.NumTri() < 400000) {
      vec3 squash(1, 1, 1);
      for (int d = 0; d < 3; ++d)
        if (d != along && comp(box.max, d) - comp(box.min, d) > target) setComp(squash, d, target / (comp(box.max, d) - comp(box.min, d)));
      Manifold refined = source.Scale(squash).RefineToLength(target).Scale(vec3(1.0 / squash.x, 1.0 / squash.y, 1.0 / squash.z));
      if (healthy(refined)) dense = std::move(refined);
    }
    const double mid = (minA + maxA) / 2;
    const double neutral = (comp(box.min, toward) + comp(box.max, toward)) / 2;
    const double radius0 = len / angle;
    auto bend = [&](Manifold piece) {
      return piece.Warp([&](vec3& v) {
        const double va = comp(v, along);
        const double vt = comp(v, toward);
        const double a = ((va - mid) / len) * angle;
        const double radius = radius0 + (vt - neutral);
        setComp(v, along, mid + std::sin(a) * radius);
        setComp(v, toward, neutral + std::cos(a) * radius - radius0);
      });
    };
    Manifold bent;
    if (ring) {
      vec3 normal(0, 0, 0);
      setComp(normal, along, 1);
      const auto halves = dense.SplitByPlane(normal, mid);
      const Manifold a = bend(halves.first), b = bend(halves.second);
      bent = healthy(a) && healthy(b) ? a + b : bend(dense);
    } else {
      bent = bend(dense);
    }
    if (!healthy(bent)) {
      note(warning, QStringLiteral("Bend failed. Try a smaller angle."));
      return source;
    }
    return bent;
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return source;
  }
}

struct CapTri {
  int a = 0;
  int b = 0;
  int c = 0;
};

std::vector<CapTri> clipEars(const std::vector<vec2>& poly) {
  const int n = static_cast<int>(poly.size());
  std::vector<CapTri> tris;
  if (n < 3 || n > 500) return tris;
  if (n == 3) {
    tris.push_back(CapTri{0, 1, 2});
    return tris;
  }
  double area = 0;
  for (int i = 0; i < n; ++i) {
    const vec2& a = poly[static_cast<size_t>(i)];
    const vec2& b = poly[static_cast<size_t>((i + 1) % n)];
    area += a.x * b.y - b.x * a.y;
  }
  const bool ccw = area >= 0;
  std::vector<int> idx(static_cast<size_t>(n));
  std::iota(idx.begin(), idx.end(), 0);
  auto at = [&](int slot) -> const vec2& { return poly[static_cast<size_t>(idx[static_cast<size_t>(slot)])]; };
  auto turn = [&](int i0, int i1, int i2) {
    const vec2& a = at(i0);
    const vec2& b = at(i1);
    const vec2& c = at(i2);
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  };
  auto containsPt = [&](int i0, int i1, int i2, int ip) {
    const vec2& a = at(i0);
    const vec2& b = at(i1);
    const vec2& c = at(i2);
    const vec2& d = at(ip);
    auto side = [](const vec2& u, const vec2& v, const vec2& w) {
      return (v.x - u.x) * (w.y - u.y) - (v.y - u.y) * (w.x - u.x);
    };
    const double s0 = side(a, b, d);
    const double s1 = side(b, c, d);
    const double s2 = side(c, a, d);
    return (s0 > 1e-8 && s1 > 1e-8 && s2 > 1e-8) || (s0 < -1e-8 && s1 < -1e-8 && s2 < -1e-8);
  };
  int guard = 0;
  while (static_cast<int>(idx.size()) > 3 && guard++ < n * n) {
    bool clipped = false;
    const int m = static_cast<int>(idx.size());
    for (int i = 0; i < m; ++i) {
      const int i0 = (i + m - 1) % m;
      const int i2 = (i + 1) % m;
      const double orient = turn(i0, i, i2);
      if (ccw ? orient <= 1e-8 : orient >= -1e-8) continue;
      bool hit = false;
      for (int j = 0; j < m; ++j) {
        if (j == i0 || j == i || j == i2) continue;
        if (containsPt(i0, i, i2, j)) {
          hit = true;
          break;
        }
      }
      if (hit) continue;
      tris.push_back(CapTri{idx[static_cast<size_t>(i0)], idx[static_cast<size_t>(i)], idx[static_cast<size_t>(i2)]});
      idx.erase(idx.begin() + i);
      clipped = true;
      break;
    }
    if (!clipped) return {};
  }
  if (idx.size() == 3) tris.push_back(CapTri{idx[0], idx[1], idx[2]});
  if (static_cast<int>(tris.size()) != n - 2) return {};
  return tris;
}

Manifold growElbow(const Node& node, QString* warning) {
  if (node.faceLoop.size() < 9) {
    note(warning, QStringLiteral("Bend needs a face."));
    return {};
  }
  const double nx = node.faceNormal[0];
  const double ny = node.faceNormal[1];
  const double nz = node.faceNormal[2];
  const double nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (nlen < 1e-6) return {};
  const double n0 = nx / nlen;
  const double n1 = ny / nlen;
  const double n2 = nz / nlen;

  double ax = node.faceAxis[0];
  double ay = node.faceAxis[1];
  double az = node.faceAxis[2];
  if (node.faceAxisMode == 1) { ax = 1; ay = 0; az = 0; }
  else if (node.faceAxisMode == 2) { ax = 0; ay = 1; az = 0; }
  else if (node.faceAxisMode == 3) { ax = 0; ay = 0; az = 1; }

  const double axial = ax * n0 + ay * n1 + az * n2;
  ax -= n0 * axial;
  ay -= n1 * axial;
  az -= n2 * axial;
  double alen = std::sqrt(ax * ax + ay * ay + az * az);
  if (alen < 1e-5) {
    ax = n1;
    ay = -n0;
    az = 0;
    alen = std::sqrt(ax * ax + ay * ay + az * az);
    if (alen < 1e-5) {
      ax = 1;
      ay = 0;
      az = 0;
      alen = 1;
    }
  }
  ax /= alen;
  ay /= alen;
  az /= alen;

  // The hinge axis lies in the selected face. a x n points to the centreline side of a positive turn.
  double bx = ay * n2 - az * n1;
  double by = az * n0 - ax * n2;
  double bz = ax * n1 - ay * n0;
  const double blen = std::sqrt(bx * bx + by * by + bz * bz);
  if (blen < 1e-6) return {};
  bx /= blen;
  by /= blen;
  bz /= blen;

  std::vector<std::vector<vec3>> contours(1);
  const int rawCount = static_cast<int>(node.faceLoop.size() / 3);
  for (int i = 0; i < rawCount; ++i) {
    const double x = node.faceLoop[static_cast<size_t>(i) * 3];
    const double y = node.faceLoop[static_cast<size_t>(i) * 3 + 1];
    const double z = node.faceLoop[static_cast<size_t>(i) * 3 + 2];
    if (std::abs(x) > 1e19 || std::abs(y) > 1e19 || std::abs(z) > 1e19) {
      if (!contours.back().empty()) contours.emplace_back();
      continue;
    }
    contours.back().push_back(vec3(x, y, z));
  }
  if (!contours.empty() && contours.back().empty()) contours.pop_back();
  contours.erase(std::remove_if(contours.begin(), contours.end(),
                                [](const std::vector<vec3>& ring) { return ring.size() < 3; }),
                 contours.end());
  if (contours.empty()) {
    note(warning, QStringLiteral("Bend needs a face."));
    return {};
  }
  const std::vector<vec3>* active = &contours.front();
  int count = static_cast<int>(active->size());
  double cx = 0;
  double cy = 0;
  double cz = 0;
  for (const vec3& p : *active) {
    cx += p.x;
    cy += p.y;
    cz += p.z;
  }
  cx /= count;
  cy /= count;
  cz /= count;

  const double angle = node.faceAngle * kPi / 180.0;
  const double absAngle = std::abs(angle);
  const double turnSign = angle < 0 ? -1.0 : 1.0;
  const double baseRadius = std::max(0.5, node.faceRadius);
  // Steps are per turn. A 3600° coil keeps the same smoothness as one turn.
  const double turnCount = std::max(1.0, absAngle / (2.0 * kPi));
  const int steps = std::clamp(static_cast<int>(std::lround(node.faceSteps * turnCount)), 2, 4096);
  const double radialPerTurn = node.faceSpiral ? node.faceRadialStep : 0.0;

  // The sweep: z = Z-per * (angle/360) / steps, then
  // rotY(anglePerStep).movey(z). Total hinge travel is Z-per * (angle/360),
  // not Z-per * steps, and it follows the hinge rather than world Z.
  // A zero turn has no angle fraction, so the same millimetres slide straight
  // along the hinge instead of climbing world Z.
  const double riseTotal = absAngle < 1e-8 ? node.faceRise : node.faceRise * (absAngle / (2.0 * kPi));
  // Spiral is a helix along the face normal, not a change of radius.
  // At 360° the end sits this many millimetres off the face it was taken from.
  const double pitchTotal = radialPerTurn * (absAngle / (2.0 * kPi));

  auto makeSection = [&](double t, std::vector<vec3>* out) {
    out->clear();
    out->reserve(static_cast<size_t>(count));
    const double slide = riseTotal * t;
    const double pitch = pitchTotal * t;
    if (absAngle < 1e-8) {
      const double distance = baseRadius * t;
      for (int i = 0; i < count; ++i) {
        const vec3 p = (*active)[static_cast<size_t>(i)];
        out->push_back(vec3(p.x + n0 * (distance + pitch) + ax * slide,
                            p.y + n1 * (distance + pitch) + ay * slide,
                            p.z + n2 * (distance + pitch) + az * slide));
      }
      return;
    }

    const double theta = angle * t;
    const double radius = std::max(0.2, baseRadius);
    // Slide stays on the hinge, after the turn. Pitch walks along the face
    // normal, so a full turn ends above or below the face instead of on it.
    const double ox = cx + turnSign * bx * radius;
    const double oy = cy + turnSign * by * radius;
    const double oz = cz + turnSign * bz * radius;
    const double c = std::cos(theta);
    const double sn = std::sin(theta);
    for (int i = 0; i < count; ++i) {
      const vec3 p = (*active)[static_cast<size_t>(i)];
      const double px = p.x;
      const double py = p.y;
      const double pz = p.z;
      const double vx = px - ox;
      const double vy = py - oy;
      const double vz = pz - oz;
      const double dot = vx * ax + vy * ay + vz * az;
      const double rx = ay * vz - az * vy;
      const double ry = az * vx - ax * vz;
      const double rz = ax * vy - ay * vx;
      out->push_back(vec3(ox + vx * c + rx * sn + ax * dot * (1.0 - c) + ax * slide + n0 * pitch,
                          oy + vy * c + ry * sn + ay * dot * (1.0 - c) + ay * slide + n1 * pitch,
                          oz + vz * c + rz * sn + az * dot * (1.0 - c) + az * slide + n2 * pitch));
    }
  };

  auto sweepActive = [&]() -> Manifold {
  try {
    // One loft of the swept face loops. The old path hulled 0.2 mm cubes
    // between sections, which left a lumpy sausage instead of an elbow.
    const bool closedTurn = std::abs(std::abs(node.faceAngle) - 360.0) < 0.05 && std::abs(radialPerTurn) < 1e-6 &&
                            std::abs(node.faceRise) < 1e-6;
    const int rings = closedTurn ? steps : steps + 1;
    std::vector<std::vector<vec3>> sections(static_cast<size_t>(rings));
    for (int step = 0; step < rings; ++step) makeSection(static_cast<double>(step) / steps, &sections[static_cast<size_t>(step)]);

    std::vector<CapTri> caps;
    if (!closedTurn) {
      std::vector<vec2> flat(static_cast<size_t>(count));
      for (int i = 0; i < count; ++i) {
        const vec3& p = (*active)[static_cast<size_t>(i)];
        flat[static_cast<size_t>(i)] = vec2(p.x * ax + p.y * ay + p.z * az, p.x * bx + p.y * by + p.z * bz);
      }
      caps = clipEars(flat);
      if (static_cast<int>(caps.size()) != count - 2) {
        caps.clear();
        for (int i = 1; i + 1 < count; ++i) caps.push_back(CapTri{0, i, i + 1});
      }
    }

    auto signedVolume = [&](bool flip) {
      double volume = 0;
      auto add = [&](const vec3& a, const vec3& b, const vec3& c) {
        const vec3& p1 = flip ? c : b;
        const vec3& p2 = flip ? b : c;
        volume += a.x * (p1.y * p2.z - p1.z * p2.y) - a.y * (p1.x * p2.z - p1.z * p2.x) +
                  a.z * (p1.x * p2.y - p1.y * p2.x);
      };
      const std::vector<vec3>& start = sections.front();
      const std::vector<vec3>& end = sections.back();
      if (!closedTurn) {
        for (const CapTri& cap : caps) {
          add(start[static_cast<size_t>(cap.a)], start[static_cast<size_t>(cap.b)], start[static_cast<size_t>(cap.c)]);
          add(end[static_cast<size_t>(cap.a)], end[static_cast<size_t>(cap.c)], end[static_cast<size_t>(cap.b)]);
        }
      }
      const int bands = closedTurn ? rings : steps;
      for (int step = 0; step < bands; ++step) {
        const int next = closedTurn ? (step + 1) % rings : step + 1;
        const std::vector<vec3>& a = sections[static_cast<size_t>(step)];
        const std::vector<vec3>& b = sections[static_cast<size_t>(next)];
        for (int i = 0; i < count; ++i) {
          const int j = (i + 1) % count;
          add(a[static_cast<size_t>(i)], b[static_cast<size_t>(i)], b[static_cast<size_t>(j)]);
          add(a[static_cast<size_t>(i)], b[static_cast<size_t>(j)], a[static_cast<size_t>(j)]);
        }
      }
      return volume;
    };
    const bool flip = signedVolume(false) < 0;

    manifold::MeshGL mesh;
    mesh.numProp = 3;
    mesh.vertProperties.reserve(static_cast<size_t>(rings * count) * 3);
    const int bands = closedTurn ? rings : steps;
    const size_t sideTris = static_cast<size_t>(bands) * static_cast<size_t>(count) * 2;
    const size_t capTris = closedTurn ? 0 : static_cast<size_t>(std::max(0, count - 2)) * 2;
    mesh.triVerts.reserve((sideTris + capTris) * 3);
    auto addVertex = [&](const vec3& p) -> uint32_t {
      const uint32_t index = static_cast<uint32_t>(mesh.vertProperties.size() / 3);
      mesh.vertProperties.push_back(static_cast<float>(p.x));
      mesh.vertProperties.push_back(static_cast<float>(p.y));
      mesh.vertProperties.push_back(static_cast<float>(p.z));
      return index;
    };
    std::vector<uint32_t> index;
    index.reserve(static_cast<size_t>(rings * count));
    for (const std::vector<vec3>& ring : sections) {
      for (const vec3& point : ring) index.push_back(addVertex(point));
    }
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
      if (flip) std::swap(b, c);
      mesh.triVerts.push_back(a);
      mesh.triVerts.push_back(b);
      mesh.triVerts.push_back(c);
    };
    auto at = [&](int ring, int corner) { return index[static_cast<size_t>(ring * count + corner)]; };
    if (!closedTurn) {
      for (const CapTri& cap : caps) {
        tri(at(0, cap.a), at(0, cap.b), at(0, cap.c));
        tri(at(rings - 1, cap.a), at(rings - 1, cap.c), at(rings - 1, cap.b));
      }
    }
    for (int step = 0; step < bands; ++step) {
      const int next = closedTurn ? (step + 1) % rings : step + 1;
      for (int i = 0; i < count; ++i) {
        const int j = (i + 1) % count;
        tri(at(step, i), at(next, i), at(next, j));
        tri(at(step, i), at(next, j), at(step, j));
      }
    }
    Manifold grown(mesh);
    if (!healthy(grown)) {
      note(warning, QStringLiteral("Could not grow a bend off that face."));
      return {};
    }
    return grown;
  } catch (const std::exception& ex) {
    note(warning, QString::fromUtf8(ex.what()));
    return {};
  }
  };

  auto areaOf = [&](const std::vector<vec3>& ring) {
    double area = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
      const vec3& p = ring[i];
      const vec3& q = ring[(i + 1) % ring.size()];
      const double pu = p.x * ax + p.y * ay + p.z * az;
      const double pv = p.x * bx + p.y * by + p.z * bz;
      const double qu = q.x * ax + q.y * ay + q.z * az;
      const double qv = q.x * bx + q.y * by + q.z * bz;
      area += pu * qv - qu * pv;
    }
    return area * 0.5;
  };
  auto contains = [&](const std::vector<vec3>& ring, const vec3& point) {
    const double u = point.x * ax + point.y * ay + point.z * az;
    const double v = point.x * bx + point.y * by + point.z * bz;
    bool inside = false;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
      const vec3& a = ring[i];
      const vec3& b = ring[j];
      const double ua = a.x * ax + a.y * ay + a.z * az;
      const double va = a.x * bx + a.y * by + a.z * bz;
      const double ub = b.x * ax + b.y * ay + b.z * az;
      const double vb = b.x * bx + b.y * by + b.z * bz;
      if ((va > v) != (vb > v) && u < (ub - ua) * (v - va) / (vb - va + 1e-18) + ua) inside = !inside;
    }
    return inside;
  };

  Manifold body = sweepActive();
  if (!healthy(body)) return {};
  const double outerSign = areaOf(contours.front());
  for (size_t ringIndex = 1; ringIndex < contours.size(); ++ringIndex) {
    const std::vector<vec3>& ring = contours[ringIndex];
    vec3 mid(0, 0, 0);
    for (const vec3& p : ring) mid = vec3(mid.x + p.x, mid.y + p.y, mid.z + p.z);
    const double scale = 1.0 / static_cast<double>(ring.size());
    mid = vec3(mid.x * scale, mid.y * scale, mid.z * scale);
    const double area = areaOf(ring);
    const bool hole = contains(contours.front(), mid) || area * outerSign < 0;
    active = &ring;
    count = static_cast<int>(ring.size());
    Manifold piece = sweepActive();
    if (!healthy(piece)) {
      note(warning, QStringLiteral("An inner boundary of that bend was left solid."));
      continue;
    }
    if (hole) {
      try {
        Manifold cut = body - piece;
        if (!healthy(cut)) {
          note(warning, QStringLiteral("An inner boundary could not be cut from the bend."));
          continue;
        }
        body = std::move(cut);
      } catch (const std::exception&) {
        note(warning, QStringLiteral("An inner boundary could not be cut from the bend."));
      }
    } else {
      try {
        Manifold joined = body + piece;
        if (healthy(joined)) body = std::move(joined);
      } catch (const std::exception&) {
      }
    }
  }
  return body;
}

Manifold applyMirror(Manifold source, const Node& node) {
  if (node.mirror[0]) source = source.Mirror(vec3(1, 0, 0));
  if (node.mirror[1]) source = source.Mirror(vec3(0, 1, 0));
  if (node.mirror[2]) source = source.Mirror(vec3(0, 0, 1));
  return source;
}

// A threaded cylinder: a rod, or the cutter of a threaded hole when it is a hole.
Manifold threadSolid(const Node& node, QString* warning) {
  const double diameter = std::max(node.radius, 0.3) * 2;
  const double pitch = node.threadPitch > 0 ? node.threadPitch : gen::coarsePitch(diameter);
  Manifold solid = gen::threadedCylinder(diameter, pitch, std::max(node.size[2], 0.5), node.hole,
                                         node.hole ? std::clamp(node.threadClearance, 0.0, 2.0) : 0.0);
  if (!healthy(solid)) {
    note(warning, QStringLiteral("Could not cut a thread with these settings; the cylinder is plain."));
    return primitiveOf(node, 64);
  }
  return solid;
}

bool threaded(const Node& node) { return node.shape == QLatin1String("cylinder") && !node.thread.isEmpty(); }

std::string cacheKey(const Node& node, const Document& doc) {
  auto n = [](double v) { return QString::number(v, 'g', 8); };
  std::vector<int> ids = node.filletEdges;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  QString edgeList;
  for (int id : ids) {
    if (!edgeList.isEmpty()) edgeList += QLatin1Char(',');
    edgeList += QString::number(id);
  }
  auto mixLoop = [](const std::vector<double>& values) {
    uint64_t hash = 1469598103934665603ull;
    for (double value : values) {
      uint64_t bits = static_cast<uint64_t>(std::llround(value * 1e5));
      hash ^= bits;
      hash *= 1099511628211ull;
    }
    return QString::asprintf("%llx", static_cast<unsigned long long>(hash));
  };
  // A generator's size only records what it built; leave it out so storing it costs no rebuild.
  const bool generated = node.shape == QLatin1String("generator");
  QStringList parts{node.shape,
                     n(node.shape == QLatin1String("diamond") ? node.gemTable : 0),
                     n(generated ? 0.0 : node.size[0]),
                     n(generated ? 0.0 : node.size[1]),
                     n(generated ? 0.0 : node.size[2]),
                     n(node.radius),
                     n(node.radius2),
                     QString::number(node.sides),
                     QString::fromUtf8(node.meshPath.toUtf8()),
                     QString::number(node.shape == QLatin1String("svg")
                                         ? QFileInfo(node.meshPath).lastModified().toMSecsSinceEpoch()
                                         : 0),
                     node.faceExtrude ? QStringLiteral("1") : QStringLiteral("0"),
                     n(node.extrudeDepth),
                     n(node.edgeFillet),
                     n(node.surfaceFillet),
                     n(node.shape == QLatin1String("box") && node.edgeFillet > 0.02 ? node.scale[0] : 0),
                     n(node.shape == QLatin1String("box") && node.edgeFillet > 0.02 ? node.scale[1] : 0),
                     n(node.shape == QLatin1String("box") && node.edgeFillet > 0.02 ? node.scale[2] : 0),
                     edgeList,
                     n(node.bend),
                     QString::number(node.along),
                     QString::number(node.toward),
                     n(node.twist),
                     QString::number(node.twistAxis),
                     std::abs(node.taper - 100) > 0.05 ? n(node.taper) + QLatin1Char('|') + QString::number(node.taperAxis) : QString(),
                     // A twist is made at the part's real size, so its scale shapes the result.
                     n(std::abs(node.twist) > 0.05 ? node.scale[0] : 0),
                     n(std::abs(node.twist) > 0.05 ? node.scale[1] : 0),
                     n(std::abs(node.twist) > 0.05 ? node.scale[2] : 0),
                     node.faceBend ? QStringLiteral("1") : QStringLiteral("0"),
                     n(node.faceAngle),
                     n(node.faceRadius),
                     QString::number(node.faceSteps),
                     n(node.faceRise),
                     node.faceSpiral ? QStringLiteral("1") : QStringLiteral("0"),
                     n(node.faceRadialStep),
                     QString::number(node.faceAxisMode),
                     n(node.faceOrigin[0]),
                     n(node.faceOrigin[1]),
                     n(node.faceOrigin[2]),
                     n(node.faceNormal[0]),
                     n(node.faceNormal[1]),
                     n(node.faceNormal[2]),
                     n(node.faceAxis[0]),
                     n(node.faceAxis[1]),
                     n(node.faceAxis[2]),
                     QString::number(static_cast<int>(node.faceLoop.size())),
                     mixLoop(node.faceLoop),
                     QString::number(static_cast<int>(node.facePointNormals.size())),
                     mixLoop(node.facePointNormals),
                     mixLoop(node.edgeBeta),
                     node.shellWall > 0.01 && !node.hole
                         ? QStringLiteral("shell|%1|%2|%3|%4|%5")
                               .arg(n(node.shellWall), QString::number(node.shellOpen), n(node.scale[0]), n(node.scale[1]), n(node.scale[2]))
                         : QString(),
                     QString::number(node.edgeBeta.empty() ? 0 : node.edgeBetaSegs),
                     n(node.edgeBeta.empty() ? 0 : node.scale[0]),
                     n(node.edgeBeta.empty() ? 0 : node.scale[1]),
                     n(node.edgeBeta.empty() ? 0 : node.scale[2]),
                     node.mirror[0] ? QStringLiteral("1") : QStringLiteral("0"),
                     node.mirror[1] ? QStringLiteral("1") : QStringLiteral("0"),
                     node.mirror[2] ? QStringLiteral("1") : QStringLiteral("0"),
                     QString::number(node.segments),
                     QString::number(doc.filletSegments),
                     node.filletOf,
                     node.filletOp,
                     QString::number(node.filletIndex),
                     node.filletOuter ? QStringLiteral("1") : QStringLiteral("0"),
                     node.filletUp ? QStringLiteral("1") : QStringLiteral("0"),
                     node.shape == QLatin1String("text") ? node.text : QString(),
                     node.shape == QLatin1String("text") ? node.fontFamily + QLatin1Char('|') + node.fontStyle : QString(),
                     n(node.fontSize),
                     n(node.letterSpacing),
                     QString::number(node.textAlign),
                     QString::number(node.textDetail),
                     node.shape == QLatin1String("text") && node.textArc > 0.01
                         ? QStringLiteral("arc|%1|%2").arg(n(node.textArc), node.textArcBelow ? QStringLiteral("b") : QStringLiteral("t"))
                         : QString(),
                     threaded(node) ? QStringLiteral("thr|%1|%2|%3|%4").arg(node.thread, n(node.threadPitch), n(node.threadClearance),
                                                                          node.hole ? QStringLiteral("in") : QStringLiteral("out"))
                                    : QString(),
                     node.shape == QLatin1String("generator") ? node.genType : QString(),
                     node.shape == QLatin1String("generator")
                         ? QString::fromUtf8(QJsonDocument(node.genParams).toJson(QJsonDocument::Compact)) +
                               QStringLiteral("|pc") + n(printerClearance())
                         : QString()};
  if (node.shape == QLatin1String("facefillet") && node.faceLoop.size() < 9) {
    std::function<void(const QString&)> walk = [&](const QString& id) {
      const Node* src = doc.find(id);
      if (!src || src->id == node.id) return;
      parts << src->id << src->shape << (src->hole ? QStringLiteral("1") : QStringLiteral("0")) << n(src->size[0])
            << n(src->size[1]) << n(src->size[2]) << n(src->radius) << n(src->radius2) << QString::number(src->sides)
            << QString::number(src->segments) << src->meshPath << (src->faceExtrude ? QStringLiteral("1") : QStringLiteral("0"))
            << n(src->extrudeDepth) << n(src->edgeFillet) << n(src->surfaceFillet) << n(src->bend) << QString::number(src->along)
            << n(src->twist) << QString::number(src->twistAxis) << n(src->taper) << QString::number(src->taperAxis)
            << n(std::abs(src->twist) > 0.05 ? src->scale[0] : 0) << n(std::abs(src->twist) > 0.05 ? src->scale[1] : 0)
            << n(std::abs(src->twist) > 0.05 ? src->scale[2] : 0)
            << QString::number(src->toward) << (src->faceBend ? QStringLiteral("1") : QStringLiteral("0")) << n(src->faceAngle)
            << n(src->faceRadius) << QString::number(src->faceSteps) << n(src->faceRise)
            << (src->faceSpiral ? QStringLiteral("1") : QStringLiteral("0")) << n(src->faceRadialStep)
            << QString::number(src->faceAxisMode) << mixLoop(src->faceLoop) << mixLoop(src->facePointNormals)
            << (src->mirror[0] ? QStringLiteral("1") : QStringLiteral("0")) << (src->mirror[1] ? QStringLiteral("1") : QStringLiteral("0"))
            << (src->mirror[2] ? QStringLiteral("1") : QStringLiteral("0")) << n(src->pos[0]) << n(src->pos[1]) << n(src->pos[2])
            << n(src->rot[0]) << n(src->rot[1]) << n(src->rot[2]) << n(src->scale[0]) << n(src->scale[1]) << n(src->scale[2])
            << QString::number(src->count) << n(src->move[0]) << n(src->move[1]) << n(src->move[2]) << n(src->drot[0])
            << n(src->drot[1]) << n(src->drot[2]) << n(src->resize[0]) << n(src->resize[1]) << n(src->resize[2])
            << src->filletOf << QString::number(src->filletIndex) << (src->filletOuter ? QStringLiteral("1") : QStringLiteral("0"))
            << (src->filletUp ? QStringLiteral("1") : QStringLiteral("0"));
      for (int key : src->filletEdges) parts << QString::number(key);
      for (const Node* child : doc.children(id)) walk(child->id);
    };
    walk(node.filletOf);
    const Node* cursor = doc.find(node.filletOf);
    while (cursor && !cursor->parentId.isEmpty()) {
      cursor = doc.find(cursor->parentId);
      if (!cursor || cursor->id == node.id) break;
      walk(cursor->id);
    }
    parts << QString::number(doc.quality);
  }
  return parts.join(QLatin1Char('|')).toStdString();
}

struct CacheEntry {
  Manifold solid = Manifold();
  std::shared_ptr<CpuMesh> mesh;
  size_t triangles = 0;
  uint64_t used = 0;
};
// Single shapes: at most 128 of them and about 3 million triangles together (big STLs, sculpts and
// lattices are a million or more each).
constexpr size_t kShapeCacheTriangles = 3000000;

uint64_t& cacheClock() {
  static uint64_t clock = 0;
  return clock;
}
// The clock when the picture now on screen started being built. What it used is never dropped from
// the caches, however big: only older results make way for the size limits. (Dropping a big sculpt
// group's mesh to make room for a small one rebuilt it on every click.)
uint64_t& shapePassStart() {
  static uint64_t start = 0;
  return start;
}
std::atomic<uint64_t>& groupPassStart() {
  static std::atomic<uint64_t> start{0};
  return start;
}
// The least recently used entry from before the current picture, or end() if all are in use.
template <typename Store>
typename Store::iterator oldestUnused(Store& store, uint64_t passStart) {
  auto oldest = store.end();
  for (auto it = store.begin(); it != store.end(); ++it)
    if (it->second.used < passStart && (oldest == store.end() || it->second.used < oldest->second.used)) oldest = it;
  return oldest;
}

std::unordered_map<std::string, CacheEntry>& cache() {
  static std::unordered_map<std::string, CacheEntry> store;
  return store;
}

std::mutex& shapeCacheMutex() {
  static std::mutex gate;
  return gate;
}

void rememberShape(const std::string& key, Manifold solid) {
  std::lock_guard<std::mutex> lock(shapeCacheMutex());
  auto& store = cache();
  if (store.find(key) != store.end()) return;
  const size_t triangles = solid.NumTri();
  size_t total = triangles;
  for (const auto& [k, entry] : store) total += entry.triangles;
  while (!store.empty() && (store.size() >= 128 || total > kShapeCacheTriangles)) {
    const auto oldest = oldestUnused(store, shapePassStart());
    if (oldest == store.end()) break;  // all in the current picture
    total -= oldest->second.triangles;
    store.erase(oldest);
  }
  CacheEntry entry;
  entry.used = ++cacheClock();
  entry.triangles = triangles;
  entry.solid = std::move(solid);
  entry.mesh = std::make_shared<CpuMesh>(meshFrom(entry.solid));
  store.emplace(key, std::move(entry));
}

Manifold localShape(const Node& node, const Document& doc, QString* warning);
Manifold worldSolid(const Document& doc, const Node& node, QString* warning);

std::vector<std::vector<double>> sliceSolidImpl(const Document& doc, const QString& sourceId, const double originPt[3],
                                                      const double normal[3], const double axisPt[3], QString* warning) {
  std::vector<std::vector<double>> outlines;
  const Node* source = doc.find(sourceId);
  if (!source) return outlines;
  Manifold solid = worldSolid(doc, *source, warning);
  if (!healthy(solid)) return outlines;
  const auto gl = solid.GetMeshGL();
  const uint32_t stride = std::max<uint32_t>(3, gl.numProp);
  const size_t count = gl.vertProperties.size() / stride;
  if (count < 4 || gl.triVerts.size() < 12) return outlines;

  vec3 n(normal[0], normal[1], normal[2]);
  const double nlen = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
  if (nlen < 1e-8) n = vec3(0, 0, 1);
  else n = vec3(n.x / nlen, n.y / nlen, n.z / nlen);
  vec3 axisU(axisPt[0], axisPt[1], axisPt[2]);
  double ulen = std::sqrt(axisU.x * axisU.x + axisU.y * axisU.y + axisU.z * axisU.z);
  if (ulen < 0.5) {
    axisU = std::abs(n.z) < 0.9 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    const double undot = axisU.x * n.x + axisU.y * n.y + axisU.z * n.z;
    axisU = vec3(axisU.x - n.x * undot, axisU.y - n.y * undot, axisU.z - n.z * undot);
    ulen = std::sqrt(axisU.x * axisU.x + axisU.y * axisU.y + axisU.z * axisU.z);
  }
  if (ulen < 1e-8) return outlines;
  axisU = vec3(axisU.x / ulen, axisU.y / ulen, axisU.z / ulen);
  const vec3 axisV(n.y * axisU.z - n.z * axisU.y, n.z * axisU.x - n.x * axisU.z, n.x * axisU.y - n.y * axisU.x);
  const vec3 origin(originPt[0], originPt[1], originPt[2]);

  auto toPlane = [&](vec3 p) {
    const vec3 d(p.x - origin.x, p.y - origin.y, p.z - origin.z);
    // FilletChamfer: transformed(workplane.inverse()).movez(0.001)
    return vec3(d.x * axisU.x + d.y * axisU.y + d.z * axisU.z, d.x * axisV.x + d.y * axisV.y + d.z * axisV.z,
                d.x * n.x + d.y * n.y + d.z * n.z + 0.001);
  };

  manifold::MeshGL moved;
  moved.numProp = 3;
  moved.vertProperties.reserve(count * 3);
  for (size_t i = 0; i < count; ++i) {
    const vec3 p = toPlane(vec3(gl.vertProperties[i * stride + 0], gl.vertProperties[i * stride + 1],
                                 gl.vertProperties[i * stride + 2]));
    moved.vertProperties.push_back(static_cast<float>(p.x));
    moved.vertProperties.push_back(static_cast<float>(p.y));
    moved.vertProperties.push_back(static_cast<float>(p.z));
  }
  moved.triVerts = gl.triVerts;
  moved.mergeFromVert = gl.mergeFromVert;
  moved.mergeToVert = gl.mergeToVert;
  Manifold placed(moved);
  if (!healthy(placed)) return outlines;
  const Manifold incoming = placed;

  // Slice.slice: if the solid misses z = 0, drop or lift it, then cut a 0.00001 mm slab.
  const double sliceThick = 0.00001;
  auto boundsZ = [](const Manifold& mesh, double* minZ, double* maxZ) {
    const auto meshGl = mesh.GetMeshGL();
    const uint32_t meshStride = std::max<uint32_t>(3, meshGl.numProp);
    *minZ = 1e300;
    *maxZ = -1e300;
    const size_t n = meshGl.vertProperties.size() / meshStride;
    for (size_t i = 0; i < n; ++i) {
      const double z = meshGl.vertProperties[i * meshStride + 2];
      *minZ = std::min(*minZ, z);
      *maxZ = std::max(*maxZ, z);
    }
  };
  double minZ = 0, maxZ = 0;
  boundsZ(placed, &minZ, &maxZ);
  if (maxZ - minZ < sliceThick) return outlines;
  if (maxZ < sliceThick) placed = placed.Translate(vec3(0, 0, sliceThick - maxZ));
  boundsZ(placed, &minZ, &maxZ);
  if (minZ > sliceThick) placed = placed.Translate(vec3(0, 0, -minZ));
  boundsZ(placed, &minZ, &maxZ);
  double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
  {
    const auto meshGl = placed.GetMeshGL();
    const uint32_t meshStride = std::max<uint32_t>(3, meshGl.numProp);
    const size_t n = meshGl.vertProperties.size() / meshStride;
    for (size_t i = 0; i < n; ++i) {
      minX = std::min(minX, static_cast<double>(meshGl.vertProperties[i * meshStride]));
      minY = std::min(minY, static_cast<double>(meshGl.vertProperties[i * meshStride + 1]));
      maxX = std::max(maxX, static_cast<double>(meshGl.vertProperties[i * meshStride]));
      maxY = std::max(maxY, static_cast<double>(meshGl.vertProperties[i * meshStride + 1]));
    }
  }
  const double totalX = std::max(maxX - minX, 1e-6);
  const double totalY = std::max(maxY - minY, 1e-6);
  Manifold slab = Manifold::Cube(vec3(totalX + 2.0, totalY + 2.0, sliceThick), true)
                      .Translate(vec3((minX + maxX) * 0.5, (minY + maxY) * 0.5, sliceThick * 0.5));
  std::vector<Manifold> cutPair;
  cutPair.push_back(placed);
  cutPair.push_back(slab);
  Manifold sliced = Manifold::BatchBoolean(cutPair, manifold::OpType::Intersect);
  if (!healthy(sliced)) return outlines;
  const auto sliceGl = sliced.GetMeshGL();
  const uint32_t sliceStride = std::max<uint32_t>(3, sliceGl.numProp);
  const int polyCount = static_cast<int>(sliceGl.triVerts.size() / 3);
  if (polyCount < 1) return outlines;
  minX = 1e300;
  minY = 1e300;
  maxX = -1e300;
  maxY = -1e300;
  const size_t sliceVerts = sliceGl.vertProperties.size() / sliceStride;
  std::vector<vec3> slicePoints(sliceVerts);
  for (size_t i = 0; i < sliceVerts; ++i) {
    slicePoints[i] = vec3(sliceGl.vertProperties[i * sliceStride], sliceGl.vertProperties[i * sliceStride + 1],
                          sliceGl.vertProperties[i * sliceStride + 2]);
    minX = std::min(minX, slicePoints[i].x);
    minY = std::min(minY, slicePoints[i].y);
    maxX = std::max(maxX, slicePoints[i].x);
    maxY = std::max(maxY, slicePoints[i].y);
  }
  const double spanX = std::max(maxX - minX, 1e-6);
  const double spanY = std::max(maxY - minY, 1e-6);
  double ratio = spanY / spanX;
  const bool ratioOrientation = spanX > spanY;
  if (ratioOrientation) ratio = spanX / spanY;
  ratio = 1.0 / ratio;
  const double mySize = std::max(spanX, spanY);
  double size = 1500.0 * (mySize / 200.0) * (static_cast<double>(polyCount) / 300.0);
  if (size < 1000.0) size = 1000.0;
  if (size > 3000.0) size = 3000.0;
  const double xPix = size * (ratioOrientation ? 1.0 : ratio);
  const double yPix = size * (!ratioOrientation ? 1.0 : ratio);
  const double scaleX = spanX / xPix;
  const double scaleY = spanY / yPix;
  const double imageOffset = 180.0;
  const double imageOffsetMotion = imageOffset * scaleX / 2.0;
  const int imgX = std::max(1, static_cast<int>(xPix + imageOffset));
  const int imgY = std::max(1, static_cast<int>(yPix + imageOffset));
  const double xOffset = minX - imageOffsetMotion;
  const double yOffset = minY - imageOffsetMotion;

  std::vector<uint8_t> pix(static_cast<size_t>(imgX) * static_cast<size_t>(imgY), 0);
  auto occupied = [&](int x, int y) {
    if (x < 0 || y < 0 || x >= imgX || y >= imgY) return false;
    return pix[static_cast<size_t>(y * imgX + x)] != 0;
  };
  auto paint = [&](double x, double y) {
    const int px = static_cast<int>((x - xOffset) / scaleX);
    const int py = static_cast<int>((y - yOffset) / scaleY);
    if (px >= 0 && py >= 0 && px < imgX && py < imgY) pix[static_cast<size_t>(py * imgX + px)] = 1;
  };
  for (size_t t = 0; t + 2 < sliceGl.triVerts.size(); t += 3) {
    const vec3& a = slicePoints[sliceGl.triVerts[t]];
    const vec3& b = slicePoints[sliceGl.triVerts[t + 1]];
    const vec3& c = slicePoints[sliceGl.triVerts[t + 2]];
    const double loX = std::min(a.x, std::min(b.x, c.x));
    const double hiX = std::max(a.x, std::max(b.x, c.x));
    const double loY = std::min(a.y, std::min(b.y, c.y));
    const double hiY = std::max(a.y, std::max(b.y, c.y));
    const int x0 = std::max(0, static_cast<int>((loX - xOffset) / scaleX));
    const int x1 = std::min(imgX - 1, static_cast<int>((hiX - xOffset) / scaleX));
    const int y0 = std::max(0, static_cast<int>((loY - yOffset) / scaleY));
    const int y1 = std::min(imgY - 1, static_cast<int>((hiY - yOffset) / scaleY));
    const double den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    for (int y = y0; y <= y1; ++y) {
      const double wy = yOffset + (static_cast<double>(y) + 0.5) * scaleY;
      for (int x = x0; x <= x1; ++x) {
        const double wx = xOffset + (static_cast<double>(x) + 0.5) * scaleX;
        if (std::abs(den) < 1e-12) {
          paint(wx, wy);
          continue;
        }
        const double w0 = ((b.y - c.y) * (wx - c.x) + (c.x - b.x) * (wy - c.y)) / den;
        const double w1 = ((c.y - a.y) * (wx - c.x) + (a.x - c.x) * (wy - c.y)) / den;
        const double w2 = 1.0 - w0 - w1;
        if (w0 >= -1e-4 && w1 >= -1e-4 && w2 >= -1e-4) pix[static_cast<size_t>(y * imgX + x)] = 1;
      }
    }
  }
  auto pixelEdge = [&](int x, int y) {
    for (int i = -1; i <= 1; ++i) {
      for (int j = -1; j <= 1; ++j) {
        const int px = x + i;
        const int py = y + j;
        if (px < 0 || py < 0 || px >= imgX || py >= imgY) continue;
        if (!occupied(px, py)) return true;
      }
    }
    return false;
  };
  auto toPix = [&](double x, double y) {
    return std::pair<int, int>{static_cast<int>((x - xOffset) / scaleX), static_cast<int>((y - yOffset) / scaleY)};
  };
  std::vector<std::pair<int, int>> seeds;
  for (size_t t = 0; t + 2 < sliceGl.triVerts.size(); t += 3) {
    const vec3& a = slicePoints[sliceGl.triVerts[t]];
    const vec3& b = slicePoints[sliceGl.triVerts[t + 1]];
    const vec3& c = slicePoints[sliceGl.triVerts[t + 2]];
    if (!(a.z > -0.001 && a.z < 0.001 && b.z > -0.001 && b.z < 0.001 && c.z > -0.001 && c.z < 0.001)) continue;
    const vec3 pts[3] = {a, b, c};
    for (const vec3& p : pts) {
      const auto at = toPix(p.x, p.y);
      if (pixelEdge(at.first, at.second)) seeds.push_back(at);
    }
  }
  std::vector<std::pair<int, int>> filtered;
  auto within = [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
    return std::abs(a.first - b.first) <= 2 && std::abs(a.second - b.second) <= 2;
  };
  for (const auto& seed : seeds) {
    bool seen = false;
    for (const auto& keep : filtered)
      if (within(seed, keep)) seen = true;
    if (!seen) filtered.push_back(seed);
  }
  bool abortSlice = false;
  std::vector<uint8_t> used(pix.size(), 0);
  auto searchNext = [&](std::pair<int, int> start, int lastIndex) -> std::pair<std::pair<int, int>, int> {
    auto depth = [&](double searchSize, int from) -> std::pair<std::pair<int, int>, int> {
      std::vector<std::pair<int, int>> locations;
      if (searchSize > 2) {
        const double inc = std::atan2(1.0, searchSize) * 180.0 / kPi;
        for (double deg = 0; deg < 360.0 + inc; deg += inc) {
          const double r = deg * kPi / 180.0;
          locations.push_back({start.first + static_cast<int>(std::lround(std::cos(r) * searchSize)),
                               start.second + static_cast<int>(std::lround(std::sin(r) * searchSize))});
        }
      } else {
        for (int i = static_cast<int>(-searchSize); i < searchSize + 1; ++i)
          locations.push_back({start.first + static_cast<int>(searchSize), start.second + i});
        for (int i = static_cast<int>(searchSize - 1); i > -searchSize - 1; --i)
          locations.push_back({start.first + i, start.second + static_cast<int>(searchSize)});
        for (int i = static_cast<int>(searchSize - 1); i > -searchSize - 1; --i)
          locations.push_back({start.first - static_cast<int>(searchSize), start.second + i});
        for (int i = static_cast<int>(-searchSize + 1); i < searchSize + 1; ++i)
          locations.push_back({start.first + i, start.second - static_cast<int>(searchSize)});
      }
      const int n = static_cast<int>(locations.size());
      if (n < 1) return {{0, 0}, -1};
      if (from >= n) from = 0;
      int end = from - 1;
      if (end < 0) end = n - 1;
      for (int i = from; i != end; i = (i + 1 >= n ? 0 : i + 1)) {
        int ccw = i - 1;
        if (ccw < 0) ccw = n - 1;
        const auto self = locations[static_cast<size_t>(i)];
        const auto prev = locations[static_cast<size_t>(ccw)];
        auto blackAt = [&](int x, int y, bool* ok) {
          if (x < 0 || y < 0 || x >= imgX || y >= imgY) {
            *ok = false;
            return false;
          }
          *ok = true;
          return occupied(x, y);
        };
        bool selfOk = false;
        bool prevOk = false;
        const bool white = !blackAt(self.first, self.second, &selfOk);
        const bool black = blackAt(prev.first, prev.second, &prevOk);
        if (!selfOk || !prevOk) {
          abortSlice = true;
          return {{0, 0}, -1};
        }
        const size_t id = static_cast<size_t>(self.second * imgX + self.first);
        if (white && black && !used[id]) {
          used[id] = 1;
          return {self, i};
        }
      }
      return {{0, 0}, -1};
    };
    auto found = depth(1, lastIndex);
    double index = 1;
    while (!abortSlice && found.second < 0 && index < 10) {
      index += 0.5;
      found = depth(index, 0);
    }
    return found;
  };

  auto addOutline = [&](const std::vector<std::pair<int, int>>& loop) {
    if (loop.size() < 3) return;
    std::vector<double> poly;
    poly.reserve(loop.size() * 3);
    for (const auto& at : loop) {
      const double x0 = static_cast<double>(at.first) * scaleX + xOffset;
      const double y0 = static_cast<double>(at.second) * scaleY + yOffset;
      const vec3 local(origin.x + axisU.x * x0 + axisV.x * y0, origin.y + axisU.y * x0 + axisV.y * y0,
                       origin.z + axisU.z * x0 + axisV.z * y0);
      poly.push_back(local.x);
      poly.push_back(local.y);
      poly.push_back(local.z);
    }
    outlines.push_back(std::move(poly));
  };

  const bool hadSeeds = !filtered.empty();
  if (hadSeeds) {
    std::pair<int, int> pixStart = filtered.front();
    filtered.erase(filtered.begin());
    std::pair<int, int> nextPoint = pixStart;
    std::vector<std::pair<int, int>> loop;
    loop.push_back(pixStart);
    int lastSearch = 0;
    int guard = imgX * imgY;
    while ((!filtered.empty() || !loop.empty()) && guard-- > 0 && !abortSlice) {
      const auto found = searchNext(nextPoint, lastSearch);
      if (found.second < 0) {
        loop.clear();
        if (!filtered.empty()) {
          pixStart = filtered.front();
          filtered.erase(filtered.begin());
          nextPoint = pixStart;
          loop = {nextPoint};
        }
        continue;
      }
      nextPoint = found.first;
      lastSearch = found.second;
      std::vector<std::pair<int, int>> hit;
      for (const auto& it : filtered)
        if (within(nextPoint, it)) hit.push_back(it);
      if (!hit.empty()) {
        for (const auto& d : hit) {
          filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
                                        [&](const std::pair<int, int>& item) { return item == d; }),
                         filtered.end());
          loop.push_back(d);
        }
      } else if (loop.size() > 2 && within(nextPoint, pixStart)) {
        addOutline(loop);
        loop.clear();
        if (!filtered.empty()) {
          pixStart = filtered.front();
          filtered.erase(filtered.begin());
          nextPoint = pixStart;
          loop = {nextPoint};
        }
      }
    }
    if (loop.size() > 2) addOutline(loop);
  }
  if (abortSlice || !hadSeeds) {
    logFillet(abortSlice ? QString("Slice search left the picture. Falling back to every triangle of the solid.")
                         : QString("Slice found no seed. Falling back to every triangle of the solid."));
    outlines.clear();
    const auto meshGl = incoming.GetMeshGL();
    const uint32_t meshStride = std::max<uint32_t>(3, meshGl.numProp);
    const size_t meshCount = meshGl.vertProperties.size() / meshStride;
    auto toWorld = [&](float x, float y, float z) {
      const double pz = static_cast<double>(z) - 0.001;
      return vec3(origin.x + axisU.x * x + axisV.x * y + n.x * pz, origin.y + axisU.y * x + axisV.y * y + n.y * pz,
                  origin.z + axisU.z * x + axisV.z * y + n.z * pz);
    };
    for (size_t t = 0; t + 2 < meshGl.triVerts.size(); t += 3) {
      const uint32_t ia = meshGl.triVerts[t];
      const uint32_t ib = meshGl.triVerts[t + 1];
      const uint32_t ic = meshGl.triVerts[t + 2];
      if (ia >= meshCount || ib >= meshCount || ic >= meshCount) continue;
      std::vector<double> poly;
      const uint32_t ids[3] = {ia, ib, ic};
      for (uint32_t id : ids) {
        const vec3 p = toWorld(meshGl.vertProperties[id * meshStride], meshGl.vertProperties[id * meshStride + 1],
                               meshGl.vertProperties[id * meshStride + 2]);
        poly.push_back(p.x);
        poly.push_back(p.y);
        poly.push_back(p.z);
      }
      outlines.push_back(std::move(poly));
    }
  }
  logFillet(QString("Slice kept %1 outline(s).").arg(outlines.size()));
  return outlines;
}

Manifold faceFilletSolid(const Node& node, const Document& doc, QString* warning) {
  if (node.faceLoop.size() >= 9) {
    // Exact outline stored on the cutter itself (see surfacefillet.cpp). No slicing, no raster.
    sf::Frame frame;
    for (int i = 0; i < 3; ++i) {
      frame.origin[i] = node.faceOrigin[i];
      frame.normal[i] = node.faceNormal[i];
      frame.axis[i] = node.faceAxis[i];
    }
    std::string warn;
    std::string log;
    // facePointNormals holds the solid's wall across each outline edge, so the fillet stays tangent
    // to the real wall (a drafted pocket) when it is rebuilt. Older files without it get square walls.
    Manifold cutter = sf::buildCutter(node.faceLoop, frame, node.radius > 0.0 ? node.radius : 0.1, node.sides,
                                      !node.filletUp, &warn, &log, node.facePointNormals);
    if (!log.empty()) logFillet(QString::fromStdString(log).trimmed());
    if (!warn.empty()) note(warning, QString::fromStdString(warn));
    if (!healthy(cutter)) note(warning, QStringLiteral("That face did not produce a fillet cutter."));
    return healthy(cutter) ? cutter : Manifold();
  }
  const double radius = node.radius > 0.0 ? node.radius : 0.1;
  const int segs = std::clamp(node.sides, 1, 20);
  const std::vector<std::vector<double>> outlines =
      sliceSolidImpl(doc, node.filletOf, node.faceOrigin, node.faceNormal, node.faceAxis, warning);
  if (node.filletIndex < 0 || node.filletIndex >= static_cast<int>(outlines.size())) {
    note(warning, QStringLiteral("That outline is not on the slice."));
    return {};
  }
  const std::vector<double>& loopPts = outlines[static_cast<size_t>(node.filletIndex)];
  const vec3 fallbackN(node.faceNormal[0], node.faceNormal[1], node.faceNormal[2]);
  struct Pnt {
    vec3 p;
    vec3 n;
  };
  std::vector<std::vector<Pnt>> contours(1);
  for (size_t i = 0; i + 2 < loopPts.size(); i += 3) {
    Pnt point;
    point.p = vec3(loopPts[i], loopPts[i + 1], loopPts[i + 2]);
    point.n = fallbackN;
    contours.back().push_back(point);
  }
  auto length = [](const vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
  auto dot = [](const vec3& a, const vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
  auto cross = [](const vec3& a, const vec3& b) {
    return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
  };
  auto add = [](const vec3& a, const vec3& b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); };
  auto sub = [](const vec3& a, const vec3& b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); };
  auto mul = [](const vec3& a, double s) { return vec3(a.x * s, a.y * s, a.z * s); };
  auto norm = [&](const vec3& v) {
    const double len = length(v);
    return len < 1e-8 ? vec3(0, 0, 0) : mul(v, 1.0 / len);
  };
  const vec3 faceN = length(norm(fallbackN)) > 0.5 ? norm(fallbackN) : vec3(0, 0, 1);
  const double eps = 0.001;
  const bool outer = node.filletOuter;
  const bool down = !node.filletUp;
  logFillet(QString("Outline %1 of %2. Radius %3, faces %4, %5, %6, normal %7 %8 %9")
                .arg(node.filletIndex)
                .arg(outlines.size())
                .arg(radius, 0, 'f', 3)
                .arg(segs)
                .arg(outer ? QStringLiteral("outer") : QStringLiteral("inner"))
                .arg(down ? QStringLiteral("down") : QStringLiteral("up"))
                .arg(faceN.x, 0, 'f', 3)
                .arg(faceN.y, 0, 'f', 3)
                .arg(faceN.z, 0, 'f', 3));
  vec3 axisU(node.faceAxis[0], node.faceAxis[1], node.faceAxis[2]);
  if (length(axisU) < 0.5) {
    axisU = std::abs(faceN.z) < 0.9 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    axisU = norm(sub(axisU, mul(faceN, dot(axisU, faceN))));
  } else {
    axisU = norm(axisU);
  }
  const vec3 axisV = norm(cross(faceN, axisU));
  const vec3 origin(node.faceOrigin[0], node.faceOrigin[1], node.faceOrigin[2]);
  auto uv = [&](const vec3& point) { return vec2(dot(point, axisU), dot(point, axisV)); };
  auto signedArea = [&](const std::vector<Pnt>& loop) {
    double area = 0;
    const int count = static_cast<int>(loop.size());
    for (int i = 0; i < count; ++i) {
      const vec2 a = uv(loop[static_cast<size_t>(i)].p);
      const vec2 b = uv(loop[static_cast<size_t>((i + 1) % count)].p);
      area += a.x * b.y - b.x * a.y;
    }
    return area * 0.5;
  };
  auto solidOf = [&](std::vector<vec3> verts, std::vector<uint32_t> tris) -> Manifold {
    if (verts.size() < 4 || tris.size() < 12) return {};
    auto build = [&](bool flip) {
      manifold::MeshGL mesh;
      mesh.numProp = 3;
      mesh.vertProperties.reserve(verts.size() * 3);
      for (const vec3& point : verts) {
        mesh.vertProperties.push_back(static_cast<float>(point.x));
        mesh.vertProperties.push_back(static_cast<float>(point.y));
        mesh.vertProperties.push_back(static_cast<float>(point.z));
      }
      mesh.triVerts = tris;
      if (flip) {
        for (size_t t = 0; t + 2 < mesh.triVerts.size(); t += 3)
          std::swap(mesh.triVerts[t + 1], mesh.triVerts[t + 2]);
      }
      return Manifold(mesh);
    };
    Manifold solid = build(false);
    return healthy(solid) ? solid : Manifold();
  };
  auto takeMesh = [&](const Manifold& solid, std::vector<vec3>& verts, std::vector<uint32_t>& tris) {
    verts.clear();
    tris.clear();
    if (!healthy(solid)) return;
    const auto gl = solid.GetMeshGL();
    const uint32_t stride = std::max<uint32_t>(3, gl.numProp);
    const size_t count = gl.vertProperties.size() / stride;
    verts.resize(count);
    for (size_t i = 0; i < count; ++i) {
      verts[i] = vec3(gl.vertProperties[i * stride + 0], gl.vertProperties[i * stride + 1],
                      gl.vertProperties[i * stride + 2]);
    }
    tris.assign(gl.triVerts.begin(), gl.triVerts.end());
  };
  auto mapVerts = [&](Manifold solid, const auto& fn) -> Manifold {
    std::vector<vec3> verts;
    std::vector<uint32_t> tris;
    takeMesh(solid, verts, tris);
    if (verts.empty()) return {};
    for (vec3& point : verts) point = fn(point);
    return solidOf(verts, tris);
  };
  auto triangulateFace = [&](const std::vector<vec3>& poly) {
    std::vector<std::array<int, 3>> faces;
    const int count = static_cast<int>(poly.size());
    if (count < 3) return faces;
    vec3 normal(0, 0, 0);
    for (int i = 0; i < count; ++i) {
      const vec3& a = poly[static_cast<size_t>(i)];
      const vec3& b = poly[static_cast<size_t>((i + 1) % count)];
      normal = add(normal, vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)));
    }
    const vec3 axisZ = norm(normal);
    if (length(axisZ) < 0.5) return faces;
    vec3 axisX = std::abs(axisZ.z) < 0.9 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    axisX = norm(sub(axisX, mul(axisZ, dot(axisX, axisZ))));
    const vec3 axisY = norm(cross(axisZ, axisX));
    std::vector<vec2> flat(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
      flat[static_cast<size_t>(i)] = vec2(dot(poly[static_cast<size_t>(i)], axisX), dot(poly[static_cast<size_t>(i)], axisY));
    double area = 0;
    for (int i = 0; i < count; ++i) {
      const vec2& a = flat[static_cast<size_t>(i)];
      const vec2& b = flat[static_cast<size_t>((i + 1) % count)];
      area += a.x * b.y - b.x * a.y;
    }
    const bool ccw = area > 0;
    auto turnOf = [&](int ia, int ib, int ic) {
      const vec2& a = flat[static_cast<size_t>(ia)];
      const vec2& b = flat[static_cast<size_t>(ib)];
      const vec2& c = flat[static_cast<size_t>(ic)];
      return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    auto inside = [&](int ia, int ib, int ic, int ip) {
      const double ab = turnOf(ia, ib, ip);
      const double bc = turnOf(ib, ic, ip);
      const double ca = turnOf(ic, ia, ip);
      const double tol = 1e-10;
      if (ccw) return ab > tol && bc > tol && ca > tol;
      return ab < -tol && bc < -tol && ca < -tol;
    };
    std::vector<int> left(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) left[static_cast<size_t>(i)] = i;
    int guard = count * count;
    while (static_cast<int>(left.size()) > 3 && guard-- > 0) {
      bool clipped = false;
      const int size = static_cast<int>(left.size());
      for (int i = 0; i < size; ++i) {
        const int ia = left[static_cast<size_t>((i + size - 1) % size)];
        const int ib = left[static_cast<size_t>(i)];
        const int ic = left[static_cast<size_t>((i + 1) % size)];
        const double turn = turnOf(ia, ib, ic);
        if (std::abs(turn) <= 1e-12) continue;
        if (ccw ? turn < 0 : turn > 0) continue;
        bool ear = true;
        for (int j = 0; j < size; ++j) {
          const int ip = left[static_cast<size_t>(j)];
          if (ip == ia || ip == ib || ip == ic) continue;
          if (inside(ia, ib, ic, ip)) {
            ear = false;
            break;
          }
        }
        if (!ear) continue;
        faces.push_back({ia, ib, ic});
        left.erase(left.begin() + i);
        clipped = true;
        break;
      }
      if (!clipped) break;
    }
    if (left.size() == 3) faces.push_back({left[0], left[1], left[2]});
    return faces;
  };
  auto pushTri = [](std::vector<uint32_t>& tris, int a, int b, int c) {
    tris.push_back(static_cast<uint32_t>(a));
    tris.push_back(static_cast<uint32_t>(b));
    tris.push_back(static_cast<uint32_t>(c));
  };
  auto addCap = [&](const std::vector<vec3>& ring, int base, bool flip, std::vector<uint32_t>& tris) {
    std::vector<vec3> poly = ring;
    if (flip) std::reverse(poly.begin(), poly.end());
    const int count = static_cast<int>(ring.size());
    for (const std::array<int, 3>& face : triangulateFace(poly)) {
      int a = face[0];
      int b = face[1];
      int c = face[2];
      if (flip) {
        a = count - 1 - a;
        b = count - 1 - b;
        c = count - 1 - c;
      }
      pushTri(tris, base + a, base + b, base + c);
    }
  };
  auto addWalls = [&](const std::vector<vec3>& prev, const std::vector<vec3>& next, int basePrev, int baseNext,
                       std::vector<uint32_t>& tris) {
    const int count = static_cast<int>(prev.size());
    const double tol = 1e-8;
    for (int i = 0; i < count; ++i) {
      const int k = (i + 1) % count;
      const vec3& bottomV1 = prev[static_cast<size_t>(i)];
      const vec3& topV1 = next[static_cast<size_t>(i)];
      const vec3& bottomV2 = prev[static_cast<size_t>(k)];
      const vec3& topV2 = next[static_cast<size_t>(k)];
      if (length(sub(bottomV1, bottomV2)) > tol && length(sub(topV1, bottomV1)) > tol)
        pushTri(tris, basePrev + k, baseNext + i, basePrev + i);
      if (length(sub(topV2, topV1)) > tol && length(sub(topV2, bottomV2)) > tol)
        pushTri(tris, basePrev + k, baseNext + k, baseNext + i);
    }
  };
  auto edgeRaw = [&](double rad, double len, int nArc) -> Manifold {
    // Fillet.toCSG: profile in the XZ plane, Extrude.extrude along +Y, then toZMin/movey/movex/movez.
    std::vector<vec3> profile;
    for (int i = nArc - 1; i >= 1; --i) {
      const double a = (270.0 - 90.0 * static_cast<double>(i) / nArc) * kPi / 180.0;
      profile.push_back(vec3(rad + rad * std::cos(a) + eps, 0, rad + rad * std::sin(a) + eps));
    }
    profile.push_back(vec3(rad, 0, 0));
    profile.push_back(vec3(rad, 0, -eps));
    profile.push_back(vec3(-eps, 0, -eps));
    profile.push_back(vec3(-eps, 0, 0));
    profile.push_back(vec3(-eps, 0, rad));
    const double lengthY = len + 2.0 * eps;
    const int samples = static_cast<int>(profile.size());
    std::vector<vec3> top = profile;
    for (vec3& point : top) point.y = lengthY;
    std::vector<vec3> verts(static_cast<size_t>(samples * 2));
    for (int i = 0; i < samples; ++i) {
      verts[static_cast<size_t>(i)] = profile[static_cast<size_t>(i)];
      verts[static_cast<size_t>(samples + i)] = top[static_cast<size_t>(i)];
    }
    std::vector<uint32_t> tris;
    addCap(profile, 0, true, tris);
    addCap(top, samples, false, tris);
    addWalls(profile, top, 0, samples, tris);
    double minZ = 1e300;
    for (const vec3& point : verts) minZ = std::min(minZ, point.z);
    for (vec3& point : verts) {
      point.x -= eps;
      point.y -= eps;
      point.z -= minZ + eps;
    }
    return solidOf(verts, tris);
  };
  auto toYMin = [&](Manifold solid) {
    std::vector<vec3> verts;
    std::vector<uint32_t> tris;
    takeMesh(solid, verts, tris);
    double minY = 1e300;
    for (const vec3& point : verts) minY = std::min(minY, point.y);
    for (vec3& point : verts) point.y -= minY;
    return solidOf(verts, tris);
  };
  auto toYMax = [&](Manifold solid) {
    std::vector<vec3> verts;
    std::vector<uint32_t> tris;
    takeMesh(solid, verts, tris);
    double maxY = -1e300;
    for (const vec3& point : verts) maxY = std::max(maxY, point.y);
    for (vec3& point : verts) point.y -= maxY;
    return solidOf(verts, tris);
  };
  auto mirrorX = [&](Manifold solid) {
    return mapVerts(solid, [](const vec3& point) { return vec3(-point.x, point.y, point.z); });
  };
  auto rotXpt = [](const vec3& point, double deg) {
    const double r = deg * kPi / 180.0;
    const double c = std::cos(r);
    const double s = std::sin(r);
    return vec3(point.x, c * point.y + s * point.z, -s * point.y + c * point.z);
  };
  auto rotYpt = [](const vec3& point, double deg) {
    const double r = deg * kPi / 180.0;
    const double c = std::cos(r);
    const double s = std::sin(r);
    return vec3(c * point.x - s * point.z, point.y, s * point.x + c * point.z);
  };
  auto rotZpt = [](const vec3& point, double deg) {
    const double r = deg * kPi / 180.0;
    const double c = std::cos(r);
    const double s = std::sin(r);
    return vec3(c * point.x + s * point.y, -s * point.x + c * point.y, point.z);
  };
  auto cornerRaw = [&](double rad, double filletAngleDeg, int nArc) -> Manifold {
    std::vector<vec3> profile;
    profile.push_back(vec3(0, 0, 0));
    profile.push_back(vec3(rad, 0, 0));
    for (int i = 1; i < nArc; ++i) {
      const double a = (270.0 - 90.0 * static_cast<double>(i) / nArc) * kPi / 180.0;
      profile.push_back(vec3(rad + rad * std::cos(a) + eps, rad + rad * std::sin(a) + eps, 0));
    }
    profile.push_back(vec3(0, rad, 0));
    for (vec3& point : profile) point.y += eps;
    const int steps = nArc;
    const int samples = static_cast<int>(profile.size());
    const double step = (filletAngleDeg + 1.0) / static_cast<double>(nArc);
    std::vector<vec3> verts(static_cast<size_t>((steps + 1) * samples));
    for (int s = 0; s <= steps; ++s) {
      for (int j = 0; j < samples; ++j) {
        verts[static_cast<size_t>(s * samples + j)] = rotXpt(profile[static_cast<size_t>(j)], step * s);
      }
    }
    std::vector<uint32_t> tris;
    for (int s = 0; s < steps; ++s) {
      std::vector<vec3> prev(verts.begin() + static_cast<std::ptrdiff_t>(s * samples),
                             verts.begin() + static_cast<std::ptrdiff_t>((s + 1) * samples));
      std::vector<vec3> next(verts.begin() + static_cast<std::ptrdiff_t>((s + 1) * samples),
                             verts.begin() + static_cast<std::ptrdiff_t>((s + 2) * samples));
      addWalls(next, prev, (s + 1) * samples, s * samples, tris);
    }
    std::vector<vec3> first(verts.begin(), verts.begin() + samples);
    std::vector<vec3> last(verts.end() - samples, verts.end());
    addCap(first, 0, false, tris);
    addCap(last, steps * samples, true, tris);
    for (vec3& point : verts) {
      point = rotZpt(rotYpt(point, 90), -0.5);
      point.z -= eps;
    }
    Manifold sweep = solidOf(verts, tris);
    Manifold pin = Manifold::Cylinder(rad, eps * 1.1, eps * 1.1, 16, false).Translate(vec3(0, 0, -eps));
    if (!healthy(sweep)) return healthy(pin) ? pin : Manifold();
    if (!healthy(pin)) return sweep;
    std::vector<Manifold> bothParts;
    bothParts.push_back(sweep);
    bothParts.push_back(pin);
    Manifold both = Manifold::BatchBoolean(bothParts, manifold::OpType::Add);
    return healthy(both) ? both : sweep;
  };
  auto planeOf = [&](const vec3& point) {
    const vec3 d = sub(point, origin);
    return vec3(dot(d, axisU), dot(d, axisV), dot(d, faceN));
  };
  auto degreesBetween = [&](const vec3& a, const vec3& b) {
    const double la = length(a);
    const double lb = length(b);
    if (la < 1e-9 || lb < 1e-9) return 0.0;
    return std::acos(std::clamp(dot(a, b) / (la * lb), -1.0, 1.0)) * 180.0 / kPi;
  };
  auto place = [&](Manifold solid, double rotDeg, const vec3& at) -> Manifold {
    return mapVerts(solid, [&](const vec3& point) {
      const vec3 turned = rotZpt(point, rotDeg);
      const double z = down ? -turned.z : turned.z;
      const vec3 local = add(turned, at);
      const vec3 onPlane = add(origin, add(add(mul(axisU, local.x), mul(axisV, local.y)), mul(faceN, z)));
      return onPlane;
    });
  };
  std::vector<Manifold> pieces;
  for (const std::vector<Pnt>& loop : contours) {
    const int count = static_cast<int>(loop.size());
    if (count < 3) continue;
    const bool isHole = signedArea(loop) < 0;
    logFillet(QString("  contour %1 points, area %2, %3")
                  .arg(count)
                  .arg(signedArea(loop), 0, 'f', 3)
                  .arg(isHole ? QStringLiteral("hole") : QStringLiteral("outside")));
    std::vector<Manifold> parts;
    int edgesKept = 0;
    int edgesDropped = 0;
    int cornersKept = 0;
    int cornersDropped = 0;
    for (int i = 0; i < count; ++i) {
      const int next = (i + 1) % count;
      const int nextNext = (next + 1) % count;
      const vec3 position0 = planeOf(loop[static_cast<size_t>(i)].p);
      const vec3 position1 = planeOf(loop[static_cast<size_t>(next)].p);
      const vec3 position2 = planeOf(loop[static_cast<size_t>(nextNext)].p);
      const vec3 seg1 = sub(position0, position1);
      const vec3 seg2 = sub(position2, position1);
      const double len = length(seg1);
      const double theta = degreesBetween(seg1, seg2);
      const double crossZ = seg1.x * seg2.y - seg1.y * seg2.x;
      const bool isConvex = isHole ? (crossZ > 0) : (crossZ < 0);
      const bool isReflex = !isConvex;
      double filletAngle = 180.0 - theta;
      double baseAngle = degreesBetween(seg1, vec3(0, 1, 0));
      if (seg1.x < 0) baseAngle = 360.0 - baseAngle;
      double edgeAngleAbs = baseAngle;
      if (isHole) {
        edgeAngleAbs += 180.0;
        baseAngle -= 90.0;
        if ((!isReflex) && outer) {
          filletAngle -= 90.0;
          if (filletAngle < 90.0) filletAngle = 0;
        }
        if (isReflex && outer) baseAngle -= filletAngle;
        if (isReflex && (!outer)) {
          filletAngle -= 90.0;
          if (filletAngle < 90.0) filletAngle = 0;
          baseAngle += 90.0;
        }
        if ((!isReflex) && (!outer)) baseAngle += 180.0;
      } else {
        if (isReflex && outer) {
          filletAngle = 180.0 - filletAngle;
          if (filletAngle < 90.0) filletAngle = 0;
          baseAngle += 90.0;
        }
        if (isReflex && (!outer)) baseAngle += 90.0;
        if ((!isReflex) && (!outer)) {
          filletAngle = theta - 90.0;
          if (filletAngle < 90.0) filletAngle = 0;
          baseAngle += 90.0;
        }
      }
      if (len > eps * 2.0) {
        Manifold edgeFillet = edgeRaw(radius, len, segs);
        if (healthy(edgeFillet)) {
          edgeFillet = isHole ? toYMax(edgeFillet) : mirrorX(toYMin(edgeFillet));
          if (!outer && healthy(edgeFillet)) edgeFillet = mirrorX(edgeFillet);
          edgeFillet = place(edgeFillet, edgeAngleAbs, vec3(position1.x, position1.y, 0));
          if (healthy(edgeFillet)) {
            parts.push_back(std::move(edgeFillet));
            ++edgesKept;
          } else {
            ++edgesDropped;
          }
        } else {
          ++edgesDropped;
        }
      }
      double cornerAngleAbs = baseAngle;
      if (!isHole && outer) cornerAngleAbs = cornerAngleAbs - filletAngle - 90.0;
      if (filletAngle > 0.01 && filletAngle < 359.99) {
        Manifold cornerPiece = cornerRaw(radius, filletAngle, segs);
        cornerPiece = place(cornerPiece, cornerAngleAbs, vec3(position1.x, position1.y, 0));
        if (healthy(cornerPiece)) {
          parts.push_back(std::move(cornerPiece));
          ++cornersKept;
        } else {
          ++cornersDropped;
        }
      }
    }
    logFillet(QString("  edges kept %1, dropped %2. Corners kept %3, dropped %4.")
                  .arg(edgesKept)
                  .arg(edgesDropped)
                  .arg(cornersKept)
                  .arg(cornersDropped));
    if (parts.empty()) continue;
    if (parts.size() == 1) {
      pieces.push_back(std::move(parts.front()));
      continue;
    }
    Manifold joined = Manifold::BatchBoolean(parts, manifold::OpType::Add);
    if (!healthy(joined)) {
      joined = parts.front();
      for (size_t i = 1; i < parts.size(); ++i) {
        std::vector<Manifold> pair;
        pair.push_back(joined);
        pair.push_back(parts[i]);
        Manifold next = Manifold::BatchBoolean(pair, manifold::OpType::Add);
        if (healthy(next)) joined = std::move(next);
      }
    }
    if (healthy(joined)) pieces.push_back(std::move(joined));
  }
  if (pieces.empty()) {
    note(warning, QStringLiteral("That face did not produce a fillet cutter."));
    return {};
  }
  if (pieces.size() == 1) return pieces.front();
  Manifold joined = Manifold::BatchBoolean(pieces, manifold::OpType::Add);
  if (!healthy(joined)) {
    note(warning, QStringLiteral("The fillet cutters did not join."));
    return {};
  }
  return joined;
}

Manifold filletPrismContours(Manifold solid, double edgeR, double surfaceR, int segs, QString* warning);

// STL, OBJ and 3MF store points as 32-bit floats: about 4 micrometres apart at 60 mm from the
// origin. Booleans (a weld, a fillet) can leave edges and slivers far finer than that; written out,
// their corners round onto each other and slicers report non-manifold edges. Simplify collapses
// everything below a few float steps first, keeping the solid closed and manifold.
Manifold exportReady(const Manifold& input) {
  if (!healthy(input)) return input;
  const Manifold solid = joinTouchingShells(input);
  const manifold::Box box = solid.BoundingBox();
  double far = 1.0;
  for (double v : {box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z}) far = std::max(far, std::abs(v));
  const double tolerance = std::max(1e-5, far * 6e-7);
  Manifold simple = solid.Simplify(tolerance);
  if (!healthy(simple) || std::abs(simple.Volume() - solid.Volume()) > 1e-4 * std::max(1.0, std::abs(solid.Volume())))
    return solid;
  return simple;
}

// Edge fillet (beta). The picks are stored before the node's scale; the rounding is done at the
// node's real size (scaled), so a stretched part still gets round, not oval, fillets.
vec3 edgeBetaScale(const Node& node) {
  auto safe = [](double v) { return std::max(1e-4, std::abs(v)); };
  return vec3(safe(node.scale[0]), safe(node.scale[1]), safe(node.scale[2]));
}

Manifold applyEdgeBeta(Manifold solid, const Node& node, QString* warning) {
  if (node.edgeBeta.empty() || !healthy(solid)) return solid;
  std::vector<ef::Pick> picks = ef::decodePicks(node.edgeBeta);
  if (picks.empty()) return solid;
  const vec3 s = edgeBetaScale(node);
  for (ef::Pick& pick : picks)
    for (size_t i = 0; i + 2 < pick.pts.size(); i += 3) {
      pick.pts[i] *= s.x;
      pick.pts[i + 1] *= s.y;
      pick.pts[i + 2] *= s.z;
    }
  std::string warn;
  std::string log;
  Manifold rounded = ef::applyEdgeFillets(solid.Scale(s), picks, node.edgeBetaSegs, &warn, &log);
  if (!log.empty()) logFillet(QString::fromStdString(log).trimmed());
  if (!warn.empty()) note(warning, QString::fromStdString(warn));
  if (!healthy(rounded)) return solid;
  rounded = rounded.Scale(vec3(1.0 / s.x, 1.0 / s.y, 1.0 / s.z));
  // The fillet's booleans leave slivers far below a float step; clear them here, so the view
  // (which draws in floats) shows no specks along the tangent lines, not only the exported file.
  return healthy(rounded) ? exportReady(rounded) : solid;
}

// Hollow: the solid less itself shrunk by the wall all round (a Minkowski difference with a ball),
// at the part's real size, so walls are the same thickness however it is stretched. An open side
// also cuts away the wall over the inside, seen along that axis: the inside's outline, from its
// middle out through that side.
Manifold applyShell(Manifold solid, double wall, int open, const double scale[3], QString* warning) {
  if (!(wall > 0.01) || !healthy(solid)) return solid;
  const vec3 s(std::max(1e-4, std::abs(scale[0])), std::max(1e-4, std::abs(scale[1])), std::max(1e-4, std::abs(scale[2])));
  const Manifold real = solid.Scale(s);
  // A 16-sided ball reaches the full wall straight out along the axes, so flat walls come out
  // exact; between its corners it falls short by at most 2 %.
  const int segs = 16;
  const Manifold ball = Manifold::Sphere(wall, segs);
  Manifold inner = real.MinkowskiDifference(ball);
  if (!healthy(inner) || inner.IsEmpty() || inner.Volume() < 1e-6) {
    note(warning, QStringLiteral("The wall is as thick as the part, so there is nothing to hollow out."));
    return solid;
  }
  // Each opening is the outline of the inside plus the openings before it, so two open sides
  // that meet leave no bar of wall along their shared edge.
  Manifold cut = inner;
  for (int bit = 0; bit < 6; ++bit) {
    if (!(open & (1 << bit))) continue;
    const int axis = bit / 2;
    const bool plus = bit % 2 == 1;
    // Turn the axis onto Z, take the inside's outline seen along it, extrude it from the inside's
    // middle past that side, and turn it back.
    auto toZ = [axis](const Manifold& m, bool back) {
      if (axis == 2) return m;
      if (axis == 0) return m.Rotate(0, back ? 90 : -90, 0);  // +X onto +Z (and back)
      return m.Rotate(back ? -90 : 90, 0, 0);                  // +Y onto +Z (and back)
    };
    const Manifold turned = toZ(cut, false);
    const manifold::Box tb = toZ(inner, false).BoundingBox();
    const manifold::Box solidTurned = toZ(real, false).BoundingBox();
    const CrossSection outline(turned.Project(), CrossSection::FillRule::Positive);
    if (outline.IsEmpty()) continue;
    const double mid = (tb.min.z + tb.max.z) * 0.5;
    const double from = plus ? mid : solidTurned.min.z - 1;
    const double to = plus ? solidTurned.max.z + 1 : mid;
    Manifold chimney = Manifold::Extrude(outline.ToPolygons(), to - from).Translate(vec3(0, 0, from));
    cut = cut + toZ(chimney, true);
  }
  Manifold shell = real - cut;
  if (!healthy(shell) || shell.IsEmpty()) {
    note(warning, QStringLiteral("Could not hollow this part."));
    return solid;
  }
  return shell.Scale(vec3(1.0 / s.x, 1.0 / s.y, 1.0 / s.z));
}

// All pieces of a generator as one solid, centered like any other part.
Manifold generatorSolid(const Node& node, QString* warning) {
  hookGeneratorText();
  const std::vector<gen::Piece> pieces = gen::build(node.genType, node.genParams, warning, printerClearance());
  if (pieces.empty()) return {};
  if (pieces.size() == 1) return pieces.front().solid;
  std::vector<Manifold> solids;
  for (const gen::Piece& piece : pieces) solids.push_back(piece.solid);
  return Manifold::BatchBoolean(solids, manifold::OpType::Add);
}

Manifold localShape(const Node& node, const Document& doc, QString* warning) {
  const std::string key = cacheKey(node, doc);
  {
    std::lock_guard<std::mutex> lock(shapeCacheMutex());
    auto& store = cache();
    if (const auto hit = store.find(key); hit != store.end()) {
      hit->second.used = ++cacheClock();
      return hit->second.solid;
    }
  }
  if (node.shape == QLatin1String("facefillet")) {
    Manifold solid = faceFilletSolid(node, doc, warning);
    if (!healthy(solid)) return {};
    rememberShape(key, std::move(solid));
    std::lock_guard<std::mutex> lock(shapeCacheMutex());
    return cache().at(key).solid;
  }
  if (node.shape == QLatin1String("elbow")) {
    Manifold solid = growElbow(node, warning);
    if (!healthy(solid)) {
      note(warning, QStringLiteral("Could not build that bend."));
      solid = Manifold::Cube(vec3(4, 4, 4), true);
    }
    rememberShape(key, std::move(solid));
    std::lock_guard<std::mutex> lock(shapeCacheMutex());
    return cache().at(key).solid;
  }
  const int arcSegs = std::clamp(doc.filletSegments, 8, 72);
  const QString& shape = node.shape;
  Manifold solid;
  if (shape == QLatin1String("stl")) {
    solid = loadStlSolid(node.meshPath, warning);
  } else if (shape == QLatin1String("svg")) {
    solid = svgSolid(node, warning, 0, 0, arcSegs);
  } else if (shape == QLatin1String("extrude")) {
    solid = extrudeFaceSolid(node, warning, 0, 0, arcSegs);
  } else if (shape == QLatin1String("text")) {
    solid = textSolid(node, warning);
  } else if (shape == QLatin1String("generator")) {
    solid = generatorSolid(node, warning);
  }
  const bool imported = shape == QLatin1String("stl") || shape == QLatin1String("svg");
  const bool special = shape == QLatin1String("extrude") || shape == QLatin1String("text") ||
                       shape == QLatin1String("generator");
  const bool revolved = shape == QLatin1String("cylinder") || shape == QLatin1String("cone") ||
                         shape == QLatin1String("tube") || shape == QLatin1String("hemisphere");
  const bool straight = shape == QLatin1String("wedge") || shape == QLatin1String("pyramid");
  if (imported || special) {
    if (!healthy(solid)) {
      note(warning, shape == QLatin1String("stl") ? QStringLiteral("Could not import that STL as a solid.")
                       : shape == QLatin1String("svg")  ? QStringLiteral("Could not import that SVG shape as a solid.")
                       : shape == QLatin1String("text") ? QStringLiteral("Could not build that text.")
                       : shape == QLatin1String("generator") ? QStringLiteral("Could not build this generator with these settings.")
                                                         : QStringLiteral("Could not build that face extrusion."));
    }
  } else if (shape == QLatin1String("cylinder") && !node.thread.isEmpty()) {
    solid = threadSolid(node, warning);
  } else if (shape == QLatin1String("box") && node.edgeFillet > 0.02) {
    solid = filletBox(node, arcSegs, warning);
  } else if (revolved && node.edgeFillet > 0.02) {
    const int roundSegs = shape == QLatin1String("cylinder") ? node.sides : node.segments;
    solid = filletRevolution(node, segmentsOf(roundSegs), arcSegs, warning);
  } else if ((shape == QLatin1String("prism") || (shape == QLatin1String("diamond") && node.gemTable <= 0.001)) &&
             node.edgeFillet > 0.02) {
    solid = filletPrism(node, arcSegs, warning);
  } else if (straight && node.edgeFillet > 0.02) {
    solid = filletStraight(node, arcSegs, warning);
  } else {
    solid = primitiveOf(node, doc.quality);
  }
  if (!healthy(solid)) {
    note(warning, QStringLiteral("This shape could not be built."));
    solid = Manifold::Cube(vec3(10, 10, 10), true);
  } else {
    solid = applyTaper(std::move(solid), node.taper, node.taperAxis, warning);
    solid = applyTwist(std::move(solid), node.twist, node.twistAxis, node.scale, warning);
    solid = applyBend(std::move(solid), node.bend, node.along, node.toward, warning);
    solid = applyMirror(std::move(solid), node);
    if (!healthy(solid)) {
      note(warning, QStringLiteral("This shape could not be built."));
      solid = primitiveOf(node, doc.quality);
    }
    solid = applyEdgeBeta(std::move(solid), node, warning);
    if (!node.hole) solid = applyShell(std::move(solid), node.shellWall, node.shellOpen, node.scale, warning);
  }
  rememberShape(key, std::move(solid));
  std::lock_guard<std::mutex> lock(shapeCacheMutex());
  return cache().at(key).solid;
}

std::shared_ptr<const CpuMesh> localMesh(const Node& node, const Document& doc, QString* warning) {
  const std::string key = cacheKey(node, doc);
  {
    std::lock_guard<std::mutex> lock(shapeCacheMutex());
    auto& store = cache();
    if (const auto hit = store.find(key); hit != store.end() && hit->second.mesh) {
      hit->second.used = ++cacheClock();
      return hit->second.mesh;
    }
  }
  localShape(node, doc, warning);
  std::lock_guard<std::mutex> lock(shapeCacheMutex());
  auto& store = cache();
  if (const auto hit = store.find(key); hit != store.end() && hit->second.mesh) return hit->second.mesh;
  static const auto empty = std::make_shared<const CpuMesh>();
  return empty;
}

// Hole clearance: the cutter grown by `clearance` mm on every side (or only around its own Z axis),
// measured at the part's real size, so a non-uniform scale still gives the same gap all round.
// A Minkowski sum with a small ball (or a flat disc for sides only). Its facets are fine enough that
// the gap is never more than 0.004 mm short.
// Fast offset for a small clearance: every face moves out by its distance (all round: c; sides only:
// c times how much the face looks sideways), and every corner goes where its faces meet again
// (least squares over the faces around it). Exact for flat-faced shapes with up to three faces at a
// corner, very close on curved ones, and O(n). Returns nothing when the offset would fold the
// surface over (detail finer than the clearance); the caller then uses the exact Minkowski sum.
Manifold offsetFaces(const Manifold& solid, double c, bool sidesOnly) {
  const manifold::MeshGL64 mesh = solid.GetMeshGL64();
  const size_t stride = mesh.numProp;
  const size_t verts = mesh.vertProperties.size() / stride;
  const size_t tris = mesh.triVerts.size() / 3;
  if (stride != 3 || verts == 0 || tris == 0) return {};
  auto pos = [&](size_t v) {
    return vec3(mesh.vertProperties[v * 3], mesh.vertProperties[v * 3 + 1], mesh.vertProperties[v * 3 + 2]);
  };
  std::vector<vec3> normals(tris);
  // Normal equations per corner: sum of n n^T and of n * (distance that face moves).
  std::vector<std::array<double, 9>> A(verts, std::array<double, 9>{});
  std::vector<vec3> b(verts, vec3(0, 0, 0));
  std::vector<double> weight(verts, 0);
  for (size_t t = 0; t < tris; ++t) {
    const size_t i0 = mesh.triVerts[t * 3], i1 = mesh.triVerts[t * 3 + 1], i2 = mesh.triVerts[t * 3 + 2];
    vec3 n = manifold::la::cross(pos(i1) - pos(i0), pos(i2) - pos(i0));
    const double len = manifold::la::length(n);
    if (len < 1e-14) continue;
    n /= len;
    normals[t] = n;
    vec3 dir = n;
    double move = c;
    if (sidesOnly) {
      const double side = std::sqrt(n.x * n.x + n.y * n.y);
      if (side < 1e-6) continue;  // a level face stays where it is
      dir = vec3(n.x, n.y, 0);
      move = c * side;
    }
    for (size_t v : {i0, i1, i2}) {
      auto& m = A[v];
      m[0] += dir.x * dir.x, m[1] += dir.x * dir.y, m[2] += dir.x * dir.z;
      m[3] += dir.y * dir.x, m[4] += dir.y * dir.y, m[5] += dir.y * dir.z;
      m[6] += dir.z * dir.x, m[7] += dir.z * dir.y, m[8] += dir.z * dir.z;
      b[v] += dir * move;
      weight[v] += 1;
    }
  }
  std::vector<double> out(mesh.vertProperties.begin(), mesh.vertProperties.end());
  for (size_t v = 0; v < verts; ++v) {
    if (weight[v] == 0) continue;
    // A touch of damping keeps flat and edge corners (one or two plane directions) solvable; it
    // pulls toward no extra movement along directions no face asks for.
    const double eps = 1e-6 * weight[v];
    manifold::mat3 m({A[v][0] + eps, A[v][3], A[v][6]}, {A[v][1], A[v][4] + eps, A[v][7]}, {A[v][2], A[v][5], A[v][8] + eps});
    if (sidesOnly) m[2][2] = 1;  // no movement up or down
    const double det = manifold::la::determinant(m);
    if (std::abs(det) < 1e-18) return {};
    vec3 d = manifold::la::mul(manifold::la::inverse(m), b[v]);
    if (sidesOnly) d.z = 0;
    const double reach = manifold::la::length(d);
    if (reach > 5 * c) d *= 5 * c / reach;  // a needle tip: don't shoot it off
    out[v * 3] += d.x, out[v * 3 + 1] += d.y, out[v * 3 + 2] += d.z;
  }
  // Folded over? Any face that turned more than about 80° means the offset crossed itself.
  for (size_t t = 0; t < tris; ++t) {
    const size_t i0 = mesh.triVerts[t * 3], i1 = mesh.triVerts[t * 3 + 1], i2 = mesh.triVerts[t * 3 + 2];
    const vec3 p0(out[i0 * 3], out[i0 * 3 + 1], out[i0 * 3 + 2]), p1(out[i1 * 3], out[i1 * 3 + 1], out[i1 * 3 + 2]),
        p2(out[i2 * 3], out[i2 * 3 + 1], out[i2 * 3 + 2]);
    vec3 n = manifold::la::cross(p1 - p0, p2 - p0);
    const double len = manifold::la::length(n);
    if (len < 1e-14) continue;
    if (manifold::la::dot(n / len, normals[t]) < 0.17) return {};
  }
  manifold::MeshGL64 grown = mesh;
  grown.vertProperties = std::move(out);
  Manifold result(grown);
  return healthy(result) ? result : Manifold();
}

Manifold growHole(const Manifold& source, double clearance, bool sidesOnly, const double scale[3], QString* warning) {
  if (!(clearance > 1e-6) || !healthy(source)) return source;
  const vec3 s(std::max(std::abs(scale[0]), 1e-4), std::max(std::abs(scale[1]), 1e-4), std::max(std::abs(scale[2]), 1e-4));
  {
    // Fast path first; the exact Minkowski sum below is slow on detailed non-convex shapes
    // (a minute and more for an STL of some tens of thousands of triangles).
    const Manifold fast = offsetFaces(source.Scale(s), clearance, sidesOnly);
    if (healthy(fast)) return fast.Scale(vec3(1.0 / s.x, 1.0 / s.y, 1.0 / s.z));
  }
  const double sag = std::min(0.004, clearance * 0.1);
  int segments = static_cast<int>(std::ceil(kPi / std::acos(1.0 - sag / clearance)));
  segments = std::clamp((segments + 3) / 4 * 4, 8, 48);
  const Manifold ball = sidesOnly ? Manifold::Cylinder(1e-4, clearance, clearance, segments, true)
                                  : Manifold::Sphere(clearance, segments);
  Manifold grown = source.Scale(s).MinkowskiSum(ball).Scale(vec3(1.0 / s.x, 1.0 / s.y, 1.0 / s.z));
  if (!healthy(grown)) {
    note(warning, QStringLiteral("Could not add clearance to this hole; it cuts at its own size."));
    return source;
  }
  return grown;
}

double holeClearanceOf(const Node& node) {
  if (!node.hole || node.clearanceMode == 0) return 0;
  return std::clamp(node.clearanceMode == 1 ? printerClearance() : node.holeClearance, 0.0, 5.0);
}

std::string holeKey(const Node& node, const Document& doc, double clearance) {
  return cacheKey(node, doc) + "|grow|" + QString::number(clearance, 'g', 8).toStdString() +
         (node.clearanceSidesOnly ? "|sides|" : "|all|") + QString::number(node.scale[0], 'g', 8).toStdString() + "," +
         QString::number(node.scale[1], 'g', 8).toStdString() + "," + QString::number(node.scale[2], 'g', 8).toStdString();
}

// What a hole shape actually cuts: its own solid, grown by its clearance. Cached like the shape.
Manifold holeShape(const Node& node, const Document& doc, QString* warning) {
  const double clearance = holeClearanceOf(node);
  if (clearance <= 1e-6 || threaded(node)) return localShape(node, doc, warning);  // a thread has its own
  const std::string key = holeKey(node, doc, clearance);
  {
    std::lock_guard<std::mutex> lock(shapeCacheMutex());
    auto& store = cache();
    if (const auto hit = store.find(key); hit != store.end()) {
      hit->second.used = ++cacheClock();
      return hit->second.solid;
    }
  }
  Manifold grown = growHole(localShape(node, doc, warning), clearance, node.clearanceSidesOnly, node.scale, warning);
  rememberShape(key, grown);
  return grown;
}

std::shared_ptr<const CpuMesh> holeMesh(const Node& node, const Document& doc, QString* warning) {
  const double clearance = holeClearanceOf(node);
  if (clearance <= 1e-6 || threaded(node)) return localMesh(node, doc, warning);
  const std::string key = holeKey(node, doc, clearance);
  holeShape(node, doc, warning);
  std::lock_guard<std::mutex> lock(shapeCacheMutex());
  auto& store = cache();
  if (const auto hit = store.find(key); hit != store.end() && hit->second.mesh) return hit->second.mesh;
  return localMesh(node, doc, warning);
}

struct Parts {
  Manifold solid;
  Manifold hole;
  bool hasSolid = false;
  bool hasHole = false;
};

Parts transformed(Parts parts, const double pos[3], const double rot[3], const double extra[3], const double scale[3]) {
  auto safe = [](double value) {
    if (std::abs(value) < 1e-4) return value < 0 ? -1e-4 : 1e-4;
    return value;
  };
  const vec3 scales(safe(scale[0]), safe(scale[1]), safe(scale[2]));
  // Every point lands on a 0.000001 mm grid once placed. Two faces that should touch (a part
  // stacked on a resized one) otherwise end up 0.000000000000004 mm apart from rounding, and
  // the union keeps them as two solids: no merge, no weld, no inside fillet between them.
  auto apply = [&](Manifold mesh) {
    return mesh.Scale(scales)
        .Rotate(rot[0], rot[1], rot[2])
        .Rotate(extra[0], extra[1], extra[2])
        .Translate(vec3(pos[0], pos[1], pos[2]))
        .Warp([](vec3& p) {
          p.x = std::round(p.x * 1e6) / 1e6;
          p.y = std::round(p.y * 1e6) / 1e6;
          p.z = std::round(p.z * 1e6) / 1e6;
        });
  };
  if (parts.hasSolid) parts.solid = apply(std::move(parts.solid));
  if (parts.hasHole) parts.hole = apply(std::move(parts.hole));
  return parts;
}

Parts combine(std::vector<Parts> children) {
  Parts out;
  std::vector<Manifold> solids;
  std::vector<Manifold> holes;
  for (auto& child : children) {
    if (child.hasSolid && healthy(child.solid)) solids.push_back(std::move(child.solid));
    if (child.hasHole && healthy(child.hole)) holes.push_back(std::move(child.hole));
  }
  if (solids.size() == 1) {
    out.solid = std::move(solids[0]);
    out.hasSolid = true;
  } else if (solids.size() > 1) {
    out.solid = Manifold::BatchBoolean(solids, manifold::OpType::Add);
    out.hasSolid = healthy(out.solid);
  }
  if (holes.size() == 1) {
    out.hole = std::move(holes[0]);
    out.hasHole = true;
  } else if (holes.size() > 1) {
    out.hole = Manifold::BatchBoolean(holes, manifold::OpType::Add);
    out.hasHole = healthy(out.hole);
  }
  if (out.hasSolid && out.hasHole) {
    out.solid = out.solid - out.hole;
    out.hasHole = false;
    out.hasSolid = healthy(out.solid);
  }
  return out;
}

// A hull group: the convex hull of its solid parts (all together, or each with the next one in a
// chain, joined), with its holes cut out of that.
Parts hullOf(std::vector<Parts> children, bool chain, QString* warning) {
  std::vector<Manifold> solids;
  std::vector<Parts> holes;
  for (auto& child : children) {
    if (child.hasSolid && healthy(child.solid)) solids.push_back(std::move(child.solid));
    if (child.hasHole && healthy(child.hole)) {
      Parts hole;
      hole.hole = std::move(child.hole);
      hole.hasHole = true;
      holes.push_back(std::move(hole));
    }
  }
  Parts out;
  if (!solids.empty()) {
    if (!chain || solids.size() < 3) {
      out.solid = Manifold::Hull(solids);
    } else {
      std::vector<Manifold> links;
      for (size_t i = 0; i + 1 < solids.size(); ++i) links.push_back(Manifold::Hull({solids[i], solids[i + 1]}));
      out.solid = Manifold::BatchBoolean(links, manifold::OpType::Add);
    }
    out.hasSolid = healthy(out.solid);
    if (!out.hasSolid) note(warning, QStringLiteral("Could not make that hull."));
  }
  if (holes.empty()) return out;
  holes.push_back(std::move(out));
  return combine(std::move(holes));
}

// An intersection group: only what all its solid parts have in common, with its holes cut out.
Parts intersectionOf(std::vector<Parts> children, QString* warning) {
  std::vector<Manifold> solids;
  std::vector<Parts> holes;
  for (auto& child : children) {
    if (child.hasSolid && healthy(child.solid)) solids.push_back(std::move(child.solid));
    if (child.hasHole && healthy(child.hole)) {
      Parts hole;
      hole.hole = std::move(child.hole);
      hole.hasHole = true;
      holes.push_back(std::move(hole));
    }
  }
  Parts out;
  if (!solids.empty()) {
    out.solid = solids.size() == 1 ? solids.front() : Manifold::BatchBoolean(solids, manifold::OpType::Intersect);
    out.hasSolid = healthy(out.solid) && !out.solid.IsEmpty();
    if (!out.hasSolid) note(warning, QStringLiteral("These parts have nothing in common, so the intersection is empty."));
  }
  if (holes.empty()) return out;
  holes.push_back(std::move(out));
  return combine(std::move(holes));
}

double integerPow(double base, int exponent) {
  double result = 1;
  for (int i = 0; i < exponent; ++i) result *= base;
  return result;
}

const double kZero[3] = {0, 0, 0};

Parts evalNode(const Document& doc, const Node& node, bool applyOwn, QString* warning);

Parts evalShape(const Document& doc, const Node& node, bool applyOwn, QString* warning) {
  Parts parts;
  Manifold solid = node.hole ? holeShape(node, doc, warning) : localShape(node, doc, warning);
  if (node.hole) {
    parts.hole = std::move(solid);
    parts.hasHole = healthy(parts.hole);
  } else {
    parts.solid = std::move(solid);
    parts.hasSolid = healthy(parts.solid);
  }
  if (!applyOwn) return parts;
  return transformed(std::move(parts), node.pos, node.rot, kZero, node.scale);
}

Manifold filletPrismContours(Manifold solid, double edgeR, double surfaceR, int segs, QString* warning) {
  if (!healthy(solid) || (!(edgeR > 0.02) && !(surfaceR > 0.02))) return {};
  solid = flattenCoplanarSteps(std::move(solid));
  if (!healthy(solid)) return {};
  const auto gl = solid.GetMeshGL();
  const uint32_t stride = std::max<uint32_t>(3, gl.numProp);
  if (gl.triVerts.size() < 12 || gl.vertProperties.size() < stride) return {};
  const size_t rawCount = gl.vertProperties.size() / stride;
  std::vector<uint32_t> topo(rawCount);
  std::iota(topo.begin(), topo.end(), 0u);
  auto findTopo = [&](uint32_t value) {
    uint32_t root = value;
    while (root < topo.size() && topo[root] != root) root = topo[root];
    while (value < topo.size() && topo[value] != value) {
      const uint32_t next = topo[value];
      topo[value] = root;
      value = next;
    }
    return root;
  };
  const size_t mergeCount = std::min(gl.mergeFromVert.size(), gl.mergeToVert.size());
  for (size_t i = 0; i < mergeCount; ++i) {
    uint32_t a = gl.mergeFromVert[i];
    uint32_t b = gl.mergeToVert[i];
    if (a >= topo.size() || b >= topo.size()) continue;
    a = findTopo(a);
    b = findTopo(b);
    if (a != b) topo[a] = b;
  }
  std::unordered_map<ScanKey, int, ScanHash> weld;
  std::vector<V3> verts;
  std::vector<int> map(rawCount, -1);
  auto pointOf = [&](uint32_t index) {
    const uint32_t root = findTopo(topo[index]);
    const size_t base = static_cast<size_t>(root) * stride;
    return V3{gl.vertProperties[base], gl.vertProperties[base + 1], gl.vertProperties[base + 2]};
  };
  for (size_t i = 0; i < rawCount; ++i) {
    const V3 p = pointOf(static_cast<uint32_t>(i));
    const ScanKey key{std::llround(p.x * 20.0), std::llround(p.y * 20.0), std::llround(p.z * 20.0)};
    const auto found = weld.find(key);
    if (found != weld.end()) {
      map[i] = found->second;
      continue;
    }
    map[i] = static_cast<int>(verts.size());
    weld.emplace(key, map[i]);
    verts.push_back(p);
  }
  struct Face {
    int id[3] = {0, 0, 0};
    V3 n;
    double area = 0;
  };
  std::vector<Face> faces;
  struct Plane {
    V3 n;
    double o = 0;
    double area = 0;
  };
  std::vector<Plane> planes;
  for (size_t t = 0; t + 2 < gl.triVerts.size(); t += 3) {
    int id[3];
    bool ok = true;
    for (int k = 0; k < 3; ++k) {
      const uint32_t index = gl.triVerts[t + static_cast<size_t>(k)];
      if (index >= map.size()) {
        ok = false;
        break;
      }
      id[k] = map[index];
    }
    if (!ok || id[0] == id[1] || id[1] == id[2] || id[0] == id[2]) continue;
    const V3 rawN = v3cross(v3sub(verts[static_cast<size_t>(id[1])], verts[static_cast<size_t>(id[0])]),
                            v3sub(verts[static_cast<size_t>(id[2])], verts[static_cast<size_t>(id[0])]));
    const double area = v3len(rawN) * 0.5;
    if (area < 1e-4) continue;
    const V3 n = v3norm(rawN);
    const double o = v3dot(n, verts[static_cast<size_t>(id[0])]);
    int found = -1;
    for (int p = 0; p < static_cast<int>(planes.size()); ++p) {
      if (v3dot(n, planes[static_cast<size_t>(p)].n) > 0.998 && std::abs(o - planes[static_cast<size_t>(p)].o) < 0.45) {
        found = p;
        break;
      }
    }
    if (found < 0) {
      planes.push_back(Plane{n, o, area});
    } else {
      planes[static_cast<size_t>(found)].area += area;
    }
    Face face;
    face.id[0] = id[0];
    face.id[1] = id[1];
    face.id[2] = id[2];
    face.n = n;
    face.area = area;
    faces.push_back(face);
  }
  int capA = -1;
  int capB = -1;
  double bestCaps = 0;
  for (int i = 0; i < static_cast<int>(planes.size()); ++i) {
    for (int j = i + 1; j < static_cast<int>(planes.size()); ++j) {
      if (v3dot(planes[static_cast<size_t>(i)].n, planes[static_cast<size_t>(j)].n) > -0.998) continue;
      const double score = std::min(planes[static_cast<size_t>(i)].area, planes[static_cast<size_t>(j)].area);
      if (score > bestCaps) {
        bestCaps = score;
        capA = i;
        capB = j;
      }
    }
  }
  if (capA < 0 || bestCaps < 1.0) return {};
  V3 up = v3norm(planes[static_cast<size_t>(capA)].n);
  if (v3len(up) < 0.5) return {};
  double topAlong = -1e300;
  double bottomAlong = 1e300;
  double totalArea = 0;
  double badArea = 0;
  for (const Face& face : faces) {
    totalArea += face.area;
    const double along = v3dot(up, verts[static_cast<size_t>(face.id[0])]);
    const double align = v3dot(face.n, up);
    if (align > 0.98) topAlong = std::max(topAlong, along);
    else if (align < -0.98) bottomAlong = std::min(bottomAlong, along);
  }
  const double height = topAlong - bottomAlong;
  if (!(height > 0.25) || !(totalArea > 1.0)) return {};
  for (const Face& face : faces) {
    const double along = v3dot(up, verts[static_cast<size_t>(face.id[0])]);
    const double align = v3dot(face.n, up);
    const bool top = align > 0.98 && topAlong - along < 0.45;
    const bool bottom = align < -0.98 && along - bottomAlong < 0.45;
    if (!top && !bottom && std::abs(align) > 0.22) badArea += face.area;
  }
  if (badArea > totalArea * 0.45) return {};

  std::vector<int> topFaces;
  for (int i = 0; i < static_cast<int>(faces.size()); ++i) {
    const Face& face = faces[static_cast<size_t>(i)];
    if (v3dot(face.n, up) < 0.98) continue;
    if (topAlong - v3dot(up, verts[static_cast<size_t>(face.id[0])]) > 0.45) continue;
    topFaces.push_back(i);
  }
  if (topFaces.empty()) return {};
  V3 hint = std::abs(up.z) < 0.9 ? V3{0, 0, 1} : V3{1, 0, 0};
  const V3 tangent = v3norm(v3sub(hint, v3mul(up, v3dot(hint, up))));
  const V3 bitangent = v3norm(v3cross(up, tangent));
  if (v3len(tangent) < 0.5 || v3len(bitangent) < 0.5) return {};
  V3 topPoint = verts[static_cast<size_t>(faces[static_cast<size_t>(topFaces.front())].id[0])];
  topPoint = v3sub(topPoint, v3mul(up, v3dot(up, topPoint) - topAlong));

  // Union the top triangles in 2D. A mesh walk breaks at the T-junctions where
  // two frames cross, and the edge fillet then chops those corners. The union
  // is one outer outline plus one ring per hole, and each ring is rounded alone.
  std::vector<CrossSection> pieces;
  pieces.reserve(topFaces.size());
  for (int index : topFaces) {
    const Face& face = faces[static_cast<size_t>(index)];
    SimplePolygon poly;
    for (int k = 0; k < 3; ++k) {
      const V3 delta = v3sub(verts[static_cast<size_t>(face.id[k])], topPoint);
      poly.push_back(vec2(v3dot(delta, tangent), v3dot(delta, bitangent)));
    }
    double area = 0;
    for (int k = 0; k < 3; ++k) {
      const vec2& a = poly[static_cast<size_t>(k)];
      const vec2& b = poly[static_cast<size_t>((k + 1) % 3)];
      area += a.x * b.y - b.x * a.y;
    }
    if (std::abs(area) < 2e-4) continue;
    if (area < 0) std::reverse(poly.begin(), poly.end());
    try {
      CrossSection piece(Polygons{poly});
      if (!piece.IsEmpty() && piece.Area() > 1e-4) pieces.push_back(std::move(piece));
    } catch (const std::exception&) {
    }
  }
  if (pieces.empty()) return {};
  CrossSection shape;
  try {
    shape = pieces.size() == 1 ? pieces.front() : CrossSection::BatchBoolean(pieces, manifold::OpType::Add);
  } catch (const std::exception&) {
    shape = pieces.front();
    for (size_t i = 1; i < pieces.size(); ++i) {
      try {
        CrossSection next = shape + pieces[i];
        if (!next.IsEmpty()) shape = std::move(next);
      } catch (const std::exception&) {
      }
    }
  }
  if (shape.IsEmpty() || shape.Area() < 0.5) return {};
  const double before = std::abs(solid.Volume());
  const double silhouette = shape.Area() * height;
  if (before > 1e-3 && (silhouette < before * 0.5 || silhouette > before * 1.5)) return {};
  const int ringCount = static_cast<int>(shape.ToPolygons().size());
  try {
    QString localNote;
    shape = roundSection(std::move(shape), edgeR, surfaceR, segs, &localNote);
    Manifold body = extrudeLip(shape, height, edgeR, segs);
    if (!healthy(body)) return {};
    const V3 origin = v3sub(topPoint, v3mul(up, height));
    body = body.Warp([&](vec3& point) {
      const V3 out = v3add(origin, v3add(v3mul(tangent, point.x), v3add(v3mul(bitangent, point.y), v3mul(up, point.z))));
      point = vec3(out.x, out.y, out.z);
    });
    if (!healthy(body)) return {};
    const double after = std::abs(body.Volume());
    if (before > 1e-3 && (after < before * 0.2 || after > before * 2.5)) return {};
    if (warning && warning->isEmpty()) {
      if (!localNote.isEmpty())
        *warning = localNote;
      else if (ringCount > 1)
        *warning = QStringLiteral("Rounded outside tips, then each inside corner and every hole.");
      else
        *warning = QStringLiteral("Rounded outside tips, then each inside corner.");
    }
    return body;
  } catch (const std::exception&) {
    return {};
  }
}

// The plate's height in this group's own frame, or NaN when the group (or one it sits in) is
// tilted or repeated, so the plate is no longer one level plane there.
double plateInGroup(const Document& doc, const Node& group) {
  std::vector<const Node*> chain;
  for (const Node* c = &group; c; c = c->parentId.isEmpty() ? nullptr : doc.find(c->parentId)) chain.push_back(c);
  double z = doc.workplaneZ;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const Node* n = *it;
    if (n->kind == Node::Kind::Pattern || std::abs(n->rot[0]) > 1e-9 || std::abs(n->rot[1]) > 1e-9 ||
        std::abs(n->scale[2]) < 1e-9)
      return std::numeric_limits<double>::quiet_NaN();
    z = (z - n->pos[2]) / n->scale[2];
  }
  return z;
}

// Parts meant to stand on the plate end up a hair above or below it after a resize or a rounded
// move. A hole whose bottom stops 0.00005 mm above a solid's bottom leaves a paper-thin floor in
// the cut; a solid 0.00005 mm up does not merge with its neighbour. Anything within 0.001 mm of
// the plate is put exactly on it before the parts are combined.
void settleOnPlate(Parts& parts, double plate) {
  if (std::isnan(plate)) return;
  auto settle = [plate](const Manifold& m) {
    const manifold::Box box = m.BoundingBox();
    if (box.min.z > plate + 1e-3 || box.max.z < plate - 1e-3) return m;  // nowhere near it
    return m.Warp([plate](vec3& p) {
      if (std::abs(p.z - plate) < 1e-3) p.z = plate;
    });
  };
  if (parts.hasSolid) parts.solid = settle(parts.solid);
  if (parts.hasHole) parts.hole = settle(parts.hole);
}

// Everything a group's merged solid depends on: its whole subtree (shapes, settings, the placement
// of everything under it, not its own) and where the plate is. Two groups with the same key have
// the same solid.
std::string groupKey(const Document& doc, const Node& group) {
  auto n = [](double v) { return QString::number(v, 'g', 6); };
  QStringList parts{QStringLiteral("grp"), QString::number(doc.filletSegments)};
  std::function<void(const Node&, bool)> walk = [&](const Node& cur, bool xform) {
    uint64_t loopHash = 1469598103934665603ull;
    for (double value : cur.faceLoop) {
      loopHash ^= static_cast<uint64_t>(std::llround(value * 1e5));
      loopHash *= 1099511628211ull;
    }
    if (cur.hole && cur.clearanceMode != 0) {
      parts << QStringLiteral("cl") << n(holeClearanceOf(cur)) << (cur.clearanceSidesOnly ? QStringLiteral("s") : QStringLiteral("a"))
            << n(cur.scale[0]) << n(cur.scale[1]) << n(cur.scale[2]);
    }
    if (cur.shape == QLatin1String("generator")) {
      parts << cur.genType << QString::fromUtf8(QJsonDocument(cur.genParams).toJson(QJsonDocument::Compact)) << n(printerClearance());
    }
    parts << cur.id << cur.shape << (cur.hole ? QStringLiteral("1") : QStringLiteral("0")) << n(cur.size[0]) << n(cur.size[1])
          << n(cur.size[2]) << n(cur.radius) << n(cur.radius2) << QString::number(cur.sides) << QString::number(cur.segments)
          << n(cur.gemTable) << n(cur.edgeFillet)
          << n(cur.surfaceFillet);
    for (int key : cur.filletEdges) parts << QString::number(key);
    parts << n(cur.bend) << QString::number(cur.along) << QString::number(cur.toward) << n(cur.twist)
          << QString::number(cur.twistAxis);
    if (std::abs(cur.taper - 100) > 0.05) parts << QStringLiteral("taper") << n(cur.taper) << QString::number(cur.taperAxis);
    if (std::abs(cur.twist) > 0.05) parts << n(cur.scale[0]) << n(cur.scale[1]) << n(cur.scale[2]);
    if (cur.hull) parts << QStringLiteral("hull") << QString::number(cur.hullMode);
    if (cur.intersect) parts << QStringLiteral("isect");
    if (cur.merged) parts << QStringLiteral("merged");
    if (cur.blend > 0.01) parts << QStringLiteral("blend") << n(cur.blend);
    if (cur.shellWall > 0.01 && !cur.hole)
      parts << QStringLiteral("shell") << n(cur.shellWall) << QString::number(cur.shellOpen) << n(cur.scale[0]) << n(cur.scale[1])
            << n(cur.scale[2]);
    if (!cur.thread.isEmpty()) parts << QStringLiteral("thr") << cur.thread << n(cur.threadPitch) << n(cur.threadClearance);
    parts
          << (cur.faceBend ? QStringLiteral("1") : QStringLiteral("0")) << n(cur.faceAngle) << n(cur.faceRadius)
          << QString::number(cur.faceSteps) << n(cur.faceRise) << QString::number(cur.faceAxisMode)
          << QString::asprintf("%llx", static_cast<unsigned long long>(loopHash));
    if (!cur.edgeBeta.empty()) {
      uint64_t edgeHash = 1469598103934665603ull;
      for (double value : cur.edgeBeta) {
        edgeHash ^= static_cast<uint64_t>(std::llround(value * 1e5));
        edgeHash *= 1099511628211ull;
      }
      parts << QStringLiteral("eb") << QString::asprintf("%llx", static_cast<unsigned long long>(edgeHash))
            << QString::number(cur.edgeBetaSegs);
      if (!xform) parts << n(cur.scale[0]) << n(cur.scale[1]) << n(cur.scale[2]);
    }
    if (cur.kind == Node::Kind::Pattern) {
      parts << QString::number(cur.count) << n(cur.move[0]) << n(cur.move[1]) << n(cur.move[2]) << n(cur.drot[0])
            << n(cur.drot[1]) << n(cur.drot[2]);
    }
    if (xform) {
      parts << n(cur.pos[0]) << n(cur.pos[1]) << n(cur.pos[2]) << n(cur.rot[0]) << n(cur.rot[1]) << n(cur.rot[2])
            << n(cur.scale[0]) << n(cur.scale[1]) << n(cur.scale[2]);
    }
    for (const Node* child : doc.children(cur.id)) walk(*child, true);
  };
  walk(group, false);
  parts << QStringLiteral("plate") << QString::number(plateInGroup(doc, group), 'g', 12);
  return parts.join(QLatin1Char('|')).toStdString();
}

// Merged group solids, so a group inside a new group is not built again (grouping 36 finished
// ribs only has to join them). Kept to about 4 million triangles in all, least recently used out.
struct GroupSolid {
  Parts parts;
  size_t triangles = 0;
  uint64_t used = 0;
};
std::mutex& groupCacheMutex() {
  static std::mutex gate;
  return gate;
}
std::unordered_map<std::string, GroupSolid>& groupCache() {
  static std::unordered_map<std::string, GroupSolid> store;
  return store;
}
// Finished group solids kept for undo, redo and going back and forth. Each triangle costs about
// 150 bytes here and 100 more in the drawing cache below, and every step of moving a hole inside a
// big group is a new result: with 4 and 12 million this grew to 2 GB within a minute of editing.
constexpr size_t kGroupCacheTriangles = 1000000;
uint64_t groupClock() {
  static std::atomic<uint64_t> clock{0};
  return ++clock;
}

bool cachedGroupSolid(const std::string& key, Parts* out) {
  std::lock_guard<std::mutex> lock(groupCacheMutex());
  auto& store = groupCache();
  const auto hit = store.find(key);
  if (hit == store.end()) return false;
  hit->second.used = groupClock();
  *out = hit->second.parts;
  return true;
}

void rememberGroupSolid(const std::string& key, const Parts& parts) {
  size_t triangles = 0;
  if (parts.hasSolid) triangles += parts.solid.NumTri();
  if (parts.hasHole) triangles += parts.hole.NumTri();
  std::lock_guard<std::mutex> lock(groupCacheMutex());
  auto& store = groupCache();
  if (store.count(key)) return;
  size_t total = triangles;
  for (const auto& [k, entry] : store) total += entry.triangles;
  while (total > kGroupCacheTriangles && !store.empty()) {
    const auto oldest = oldestUnused(store, groupPassStart().load());
    if (oldest == store.end()) break;  // all in the current picture
    total -= oldest->second.triangles;
    store.erase(oldest);
  }
  store.emplace(key, GroupSolid{parts, triangles, groupClock()});
}

// The children of a group are built side by side, one per spare CPU core (Manifold also spreads
// each boolean over the cores). A child that finds no free core is built where it is.
std::atomic<int>& spareWorkers() {
  static std::atomic<int> spare{std::max(0, static_cast<int>(std::thread::hardware_concurrency()) - 1)};
  return spare;
}
bool takeWorker() {
  int spare = spareWorkers().load();
  while (spare > 0)
    if (spareWorkers().compare_exchange_weak(spare, spare - 1)) return true;
  return false;
}
void giveWorker() { spareWorkers().fetch_add(1); }

// Text is built with Qt's font engine, which is kept on one thread at a time.
bool hasText(const Document& doc, const Node& node) {
  if (node.kind == Node::Kind::Shape) return node.shape == QLatin1String("text");
  for (const Node* child : doc.children(node.id))
    if (hasText(doc, *child)) return true;
  return false;
}

// Smooth join: the solids flow into each other with a soft fillet `radius` wide, holes cut with the
// same soft edge (fx::blend). Falls back to a plain join if that cannot be built.
Parts blendOf(std::vector<Parts> kids, double radius, QString* warning) {
  std::vector<sculpt::Mesh> solids, holes;
  for (const Parts& kid : kids) {
    if (kid.hasSolid && healthy(kid.solid)) solids.push_back(sculpt::fromManifold(kid.solid));
    if (kid.hasHole && healthy(kid.hole)) holes.push_back(sculpt::fromManifold(kid.hole));
  }
  if (!solids.empty()) {
    sculpt::Mesh blended;
    std::string error;
    if (fx::blend(solids, holes, radius, &blended, &error)) {
      Manifold solid = fx::toManifold(blended);
      if (healthy(solid)) {
        Parts out;
        out.solid = std::move(solid);
        out.hasSolid = true;
        return out;
      }
      error = "the blended surface did not close";
    }
    note(warning, QStringLiteral("Smooth join left sharp: %1.").arg(QString::fromStdString(error)));
  }
  return combine(std::move(kids));
}

Parts evalGroup(const Document& doc, const Node& node, bool applyOwn, QString* warning) {
  const std::string key = groupKey(doc, node);
  Parts parts;
  if (cachedGroupSolid(key, &parts)) {
    if (!applyOwn) return parts;
    return transformed(std::move(parts), node.pos, node.rot, kZero, node.scale);
  }
  const std::vector<const Node*> children = doc.children(node.id);
  std::vector<Parts> kids(children.size());
  std::vector<QString> notes(children.size());
  const double plate = plateInGroup(doc, node);
  auto build = [&](size_t i) {
    kids[i] = evalNode(doc, *children[i], true, &notes[i]);
    settleOnPlate(kids[i], plate);
  };
  const bool sideBySide = children.size() > 1 && !hasText(doc, node);
  std::vector<std::future<void>> pending;
  for (size_t i = 0; i < children.size(); ++i) {
    if (sideBySide && i + 1 < children.size() && takeWorker()) {
      pending.push_back(std::async(std::launch::async, [&build, i] {
        struct Release {
          ~Release() { giveWorker(); }
        } release;
        build(i);
      }));
    } else {
      build(i);
    }
  }
  std::exception_ptr failure;
  for (auto& job : pending) {
    try {
      job.get();
    } catch (...) {
      if (!failure) failure = std::current_exception();
    }
  }
  if (failure) std::rethrow_exception(failure);
  for (const QString& text : notes) note(warning, text);
  parts = node.hull              ? hullOf(std::move(kids), node.hullMode == 1, warning)
          : node.intersect       ? intersectionOf(std::move(kids), warning)
          : node.blend > 0.01    ? blendOf(std::move(kids), node.blend, warning)
                                 : combine(std::move(kids));
  auto finish = [&](Manifold mesh) {
    mesh = applyTaper(std::move(mesh), node.taper, node.taperAxis, warning);
    mesh = applyTwist(std::move(mesh), node.twist, node.twistAxis, node.scale, warning);
    return applyBend(std::move(mesh), node.bend, node.along, node.toward, warning);
  };
  if (parts.hasSolid) parts.solid = finish(std::move(parts.solid));
  if (parts.hasHole) parts.hole = finish(std::move(parts.hole));
  if (parts.hasSolid && !node.edgeBeta.empty()) {
    parts.solid = applyEdgeBeta(std::move(parts.solid), node, warning);
    parts.hasSolid = healthy(parts.solid);
  }
  if (parts.hasSolid && node.shellWall > 0.01) {
    parts.solid = applyShell(std::move(parts.solid), node.shellWall, node.shellOpen, node.scale, warning);
    parts.hasSolid = healthy(parts.solid);
  }
  if (node.hole && parts.hasSolid) {
    parts.hole = growHole(std::move(parts.solid), holeClearanceOf(node), node.clearanceSidesOnly, node.scale, warning);
    parts.hasHole = healthy(parts.hole);
    parts.hasSolid = false;
  }
  rememberGroupSolid(key, parts);
  if (!applyOwn) return parts;
  return transformed(std::move(parts), node.pos, node.rot, kZero, node.scale);
}

Parts evalPattern(const Document& doc, const Node& node, bool applyOwn, QString* warning) {
  const auto kids = doc.children(node.id);
  if (kids.empty()) return {};
  const Node& child = *kids.front();
  std::vector<Parts> copies;
  const int count = std::max(1, node.count);
  copies.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    Parts local = evalNode(doc, child, false, warning);
    double pos[3];
    double extra[3];
    double scale[3];
    for (int axis = 0; axis < 3; ++axis) {
      pos[axis] = child.pos[axis] + node.move[axis] * i;
      extra[axis] = node.drot[axis] * i;
      scale[axis] = child.scale[axis] * integerPow(node.resize[axis], i);
    }
    copies.push_back(transformed(std::move(local), pos, child.rot, extra, scale));
  }
  Parts parts = combine(std::move(copies));
  if (!applyOwn) return parts;
  return transformed(std::move(parts), node.pos, node.rot, kZero, node.scale);
}

Parts evalNode(const Document& doc, const Node& node, bool applyOwn, QString* warning) {
  if (node.kind == Node::Kind::Shape) return evalShape(doc, node, applyOwn, warning);
  if (node.kind == Node::Kind::Pattern) return evalPattern(doc, node, applyOwn, warning);
  return evalGroup(doc, node, applyOwn, warning);
}

Manifold worldSolid(const Document& doc, const Node& node, QString* warning) {
  Parts parts = evalNode(doc, node, true, warning);
  std::vector<const Node*> chain;
  const Node* cursor = node.parentId.isEmpty() ? nullptr : doc.find(node.parentId);
  while (cursor) {
    chain.push_back(cursor);
    cursor = cursor->parentId.isEmpty() ? nullptr : doc.find(cursor->parentId);
  }
  const double zero[3] = {0, 0, 0};
  for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    parts = transformed(std::move(parts), (*it)->pos, (*it)->rot, zero, (*it)->scale);
  if (parts.hasSolid && healthy(parts.solid)) return parts.solid;
  if (parts.hasHole && healthy(parts.hole)) return parts.hole;
  return {};
}

bool listed(const QStringList& ids, const QString& id) { return ids.contains(id); }

bool ancestorListed(const Document& doc, const QStringList& ids, const QString& id) {
  const Node* node = doc.find(id);
  while (node && !node->parentId.isEmpty()) {
    if (listed(ids, node->parentId)) return true;
    node = doc.find(node->parentId);
  }
  return false;
}

void exportWalk(const Document& doc, const Node& node, const QStringList& only, std::vector<const Node*> ancestors,
                std::vector<Parts>& out, QString* warning) {
  const bool chosen = only.isEmpty() || (listed(only, node.id) && !ancestorListed(doc, only, node.id));
  if (only.isEmpty()) {
    out.push_back(evalNode(doc, node, true, warning));
    return;
  }
  if (chosen) {
    Parts parts = evalNode(doc, node, true, warning);
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
      const Node* ancestor = *it;
      parts = transformed(std::move(parts), ancestor->pos, ancestor->rot, kZero, ancestor->scale);
    }
    out.push_back(std::move(parts));
    return;
  }
  ancestors.push_back(&node);
  for (const Node* child : doc.children(node.id)) exportWalk(doc, *child, only, ancestors, out, warning);
}

// The merged meshes the view draws for groups, by groupKey. Kept to about 12 million triangles in
// all (a few hundred MB), least recently used out.
struct GroupMesh {
  std::shared_ptr<CpuMesh> mesh;
  bool hole = false;
  uint64_t used = 0;
};
std::mutex& groupMeshMutex() {
  static std::mutex gate;
  return gate;
}
std::unordered_map<std::string, GroupMesh>& groupMeshStore() {
  static std::unordered_map<std::string, GroupMesh> store;
  return store;
}
constexpr size_t kGroupMeshTriangles = 1500000;  // (what is on screen stays drawn whatever is dropped here)

// Runs build(0) .. build(count - 1), side by side on spare CPU cores when `together` allows it; the
// first exception is passed on once all of them have finished.
void buildSideBySide(size_t count, bool together, const std::function<void(size_t)>& build) {
  std::vector<std::future<void>> pending;
  for (size_t i = 0; i < count; ++i) {
    if (together && i + 1 < count && takeWorker()) {
      pending.push_back(std::async(std::launch::async, [&build, i] {
        struct Release {
          ~Release() { giveWorker(); }
        } release;
        build(i);
      }));
    } else {
      build(i);
    }
  }
  std::exception_ptr failure;
  for (auto& job : pending) {
    try {
      job.get();
    } catch (...) {
      if (!failure) failure = std::current_exception();
    }
  }
  if (failure) std::rethrow_exception(failure);
}

// A group that only puts parts together - no part inside it is a hole, and nothing is done to the
// group itself (twist, bend, rounded edges, being a hole) - looks the same drawn as its parts side
// by side as merged into one solid: the faces where they overlap are inside and never seen. Drawn
// that way it is ready at once; the real merge, which can take a minute for dozens of overlapping
// parts, is only made where it matters: export, a slice, a hole or rounding in an outer group.
// (Groups inside it are drawn as their own merged result, holes and all.)
bool onlyAddsParts(const Document& doc, const Node& group) {
  if (group.hole || group.hull || group.intersect || group.blend > 0.01 || group.shellWall > 0.01 || group.multiColor || std::abs(group.twist) > 0.05 || std::abs(group.bend) > 0.05 || std::abs(group.taper - 100) > 0.05 || !group.edgeBeta.empty() ||
      group.edgeFillet > 0.02 || group.surfaceFillet > 0.02)
    return false;
  const auto children = doc.children(group.id);
  if (children.size() < 2) return false;
  for (const Node* child : children) {
    if (child->hole || child->kind == Node::Kind::Pattern) return false;
    if (child->kind == Node::Kind::Shape && child->shape == QLatin1String("facefillet")) return false;
  }
  return true;
}

// `part` placed by `placement`, appended to `out`.
void appendPlaced(CpuMesh& out, const CpuMesh& part, const QMatrix4x4& placement) {
  float m[12];  // the affine part, row by row
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) m[r * 4 + c] = placement(r, c);
  auto map = [&m](const float* p, float* q) {
    for (int r = 0; r < 3; ++r) q[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
  };
  const bool mirrored = placement.determinant() < 0;
  const uint32_t base = static_cast<uint32_t>(out.vertices.size() / 3);
  const size_t v0 = out.vertices.size();
  out.vertices.resize(v0 + part.vertices.size());
  for (size_t i = 0; i + 2 < part.vertices.size(); i += 3) map(&part.vertices[i], &out.vertices[v0 + i]);
  const size_t i0 = out.indices.size();
  out.indices.resize(i0 + part.indices.size());
  for (size_t i = 0; i + 2 < part.indices.size(); i += 3) {
    out.indices[i0 + i] = base + part.indices[i];
    out.indices[i0 + i + 1] = base + part.indices[mirrored ? i + 2 : i + 1];
    out.indices[i0 + i + 2] = base + part.indices[mirrored ? i + 1 : i + 2];
  }
  out.triangles += part.triangles;
  out.volume += part.volume * std::abs(placement.determinant());
}

// Keep part colours: the group's finished solid cut into one closed piece per colour of its parts,
// in the group's own frame. Each colour takes the part of the group its parts cover, in the order
// they come in the list (so where parts overlap, the first one's colour wins); what no part
// covers (a hull's skin, a fillet's fill) takes the group's own colour.
struct ColourPiece {
  QColor color;
  Manifold solid;
  std::shared_ptr<const CpuMesh> mesh;
};

std::string colourKey(const Document& doc, const Node& group) {
  std::string key = groupKey(doc, group) + "|mc|" + group.color.name().toStdString();
  std::function<void(const Node&)> walk = [&](const Node& cur) {
    for (const Node* child : doc.children(cur.id)) {
      key += child->id.toStdString() + ":" + child->color.name().toStdString() + (child->multiColor ? "m" : "") + ",";
      walk(*child);
    }
  };
  walk(group);
  return key;
}

std::mutex& colourMutex() {
  static std::mutex mutex;
  return mutex;
}
std::map<std::string, std::vector<ColourPiece>>& colourStore() {
  static std::map<std::string, std::vector<ColourPiece>> store;
  return store;
}

std::vector<ColourPiece> colourPieces(const Document& doc, const Node& group, QString* warning) {
  const std::string key = colourKey(doc, group);
  {
    std::lock_guard<std::mutex> lock(colourMutex());
    if (const auto hit = colourStore().find(key); hit != colourStore().end()) return hit->second;
  }
  std::vector<ColourPiece> pieces;
  const Parts merged = evalNode(doc, group, false, warning);
  if (!merged.hasSolid || !healthy(merged.solid)) return pieces;
  // The parts' solids in the group's frame, by colour, in list order.
  std::vector<std::pair<QColor, std::vector<Manifold>>> byColour;
  auto add = [&](const QColor& color, Manifold solid) {
    if (!healthy(solid) || solid.IsEmpty()) return;
    for (auto& entry : byColour)
      if (entry.first.rgb() == color.rgb()) {
        entry.second.push_back(std::move(solid));
        return;
      }
    byColour.push_back({color, {std::move(solid)}});
  };
  const double plate = plateInGroup(doc, group);
  for (const Node* child : doc.children(group.id)) {
    if (child->hole) continue;
    if (child->kind == Node::Kind::Group && child->multiColor && !child->hole) {
      for (const ColourPiece& inner : colourPieces(doc, *child, warning)) {
        Parts placed;
        placed.solid = inner.solid;
        placed.hasSolid = true;
        placed = transformed(std::move(placed), child->pos, child->rot, kZero, child->scale);
        settleOnPlate(placed, plate);
        add(inner.color, placed.solid);
      }
      continue;
    }
    Parts part = evalNode(doc, *child, true, warning);
    settleOnPlate(part, plate);
    if (part.hasSolid) add(child->color, part.solid);
  }
  if (byColour.size() <= 1) {
    pieces.push_back({byColour.empty() ? group.color : byColour.front().first, merged.solid, nullptr});
  } else {
    Manifold assigned;
    bool any = false;
    for (auto& [color, solids] : byColour) {
      const Manifold cover = solids.size() == 1 ? solids.front() : Manifold::BatchBoolean(solids, manifold::OpType::Add);
      Manifold piece = merged.solid ^ cover;
      if (any) piece = piece - assigned;
      if (!healthy(piece) || piece.IsEmpty() || piece.Volume() < 1e-6) continue;
      assigned = any ? assigned + piece : piece;
      any = true;
      pieces.push_back({color, std::move(piece), nullptr});
    }
    Manifold rest = any ? merged.solid - assigned : merged.solid;
    if (healthy(rest) && !rest.IsEmpty() && rest.Volume() > 1e-6) {
      bool joined = false;
      for (ColourPiece& piece : pieces)
        if (piece.color.rgb() == group.color.rgb()) {
          piece.solid = piece.solid + rest;
          joined = true;
        }
      if (!joined) pieces.push_back({group.color, std::move(rest), nullptr});
    }
  }
  for (ColourPiece& piece : pieces) piece.mesh = std::make_shared<CpuMesh>(meshFrom(piece.solid));
  std::lock_guard<std::mutex> lock(colourMutex());
  // A few multi-colour groups' pieces; it never let go of any before, so every edit of one stayed in memory.
  auto& store = colourStore();
  while (store.size() >= 16) store.erase(store.begin());
  store[key] = pieces;
  return pieces;
}

std::shared_ptr<const CpuMesh> cachedGroupMesh(const Document& doc, const Node& group, bool* holeOnly, QString* warning) {
  const std::string key = groupKey(doc, group);
  auto& store = groupMeshStore();
  {
    std::lock_guard<std::mutex> lock(groupMeshMutex());
    if (const auto hit = store.find(key); hit != store.end()) {
      hit->second.used = groupClock();
      if (holeOnly) *holeOnly = hit->second.hole;
      return hit->second.mesh;
    }
  }
  auto mesh = std::make_shared<CpuMesh>();
  bool asHole = false;
  // Parts that only add up are drawn laid over each other (quick); a merged group as its real,
  // finished shape, so every face picked is a face of the solid.
  if (onlyAddsParts(doc, group) && !group.merged) {
    const auto children = doc.children(group.id);
    std::vector<std::shared_ptr<const CpuMesh>> meshes(children.size());
    std::vector<QString> notes(children.size());
    buildSideBySide(children.size(), !hasText(doc, group), [&](size_t i) {
      const Node& child = *children[i];
      meshes[i] = child.kind == Node::Kind::Shape ? localMesh(child, doc, &notes[i]) : cachedGroupMesh(doc, child, nullptr, &notes[i]);
    });
    for (size_t i = 0; i < children.size(); ++i) {
      note(warning, notes[i]);
      if (meshes[i] && meshes[i]->triangles > 0)
        appendPlaced(*mesh, *meshes[i], placementMatrix(children[i]->pos, children[i]->rot, kZero, children[i]->scale));
    }
    mesh->updateBounds();
  } else {
    Parts partsMesh = evalGroup(doc, group, false, warning);
    if (partsMesh.hasSolid && healthy(partsMesh.solid)) *mesh = meshFrom(partsMesh.solid);
    else if (partsMesh.hasHole && healthy(partsMesh.hole)) {
      *mesh = meshFrom(partsMesh.hole);
      asHole = true;
    }
  }
  if (holeOnly) *holeOnly = asHole;
  std::lock_guard<std::mutex> lock(groupMeshMutex());
  if (store.count(key)) return store.at(key).mesh;  // built by another thread meanwhile
  size_t total = static_cast<size_t>(mesh->triangles);
  for (const auto& [k, entry] : store) total += static_cast<size_t>(entry.mesh->triangles);
  while (total > kGroupMeshTriangles && !store.empty()) {
    const auto oldest = oldestUnused(store, groupPassStart().load());
    if (oldest == store.end()) break;  // all in the current picture
    total -= static_cast<size_t>(oldest->second.mesh->triangles);
    store.erase(oldest);
  }
  store.emplace(key, GroupMesh{mesh, asHole, groupClock()});
  return mesh;
}

// Live editing (see buildEditView): set for the duration of one build.
struct LiveDraw {
  bool on = false;
  bool deferred = false;
};
thread_local LiveDraw gLive;

// Whether `group` holds one of `ids` somewhere inside.
bool holdsAny(const Document& doc, const Node& group, const QStringList& ids) {
  for (const Node* child : doc.children(group.id)) {
    if (ids.contains(child->id)) return true;
    if (child->kind != Node::Kind::Shape && holdsAny(doc, *child, ids)) return true;
  }
  return false;
}

// About how many triangles go into a group's result (its shapes' meshes, as cached).
size_t groupWeight(const Document& doc, const Node& group, QString* warning) {
  size_t total = 0;
  for (const Node* child : doc.children(group.id)) {
    if (child->kind == Node::Kind::Shape) total += meshOf(localMesh(*child, doc, warning)).triCount();
    else total += groupWeight(doc, *child, warning);
  }
  return total;
}

void drawWalk(const Document& doc, const Node& node, const QMatrix4x4& parentWorld, bool applyOwn, int copyIndex,
              const QStringList& selected, bool sole, std::vector<DrawItem>& out, QString* warning) {
  const QMatrix4x4 world =
      applyOwn ? parentWorld * placementMatrix(node.pos, node.rot, kZero, node.scale) : parentWorld;
  // Editing a part inside a heavy group: its parts as they are, the holes see-through, until the
  // edit pauses and the group is cut once (about 150 000 triangles take long enough to notice).
  if (gLive.on && node.kind == Node::Kind::Group && !node.hole && holdsAny(doc, node, selected) && !::groupMeshReady(doc, node) &&
      groupWeight(doc, node, warning) > 150000) {
    gLive.deferred = true;
    for (const Node* child : doc.children(node.id))
      drawWalk(doc, *child, world, true, copyIndex, selected, sole, out, warning);
    return;
  }
  if (node.kind == Node::Kind::Shape) {
    DrawItem item;
    item.id = node.id;
    item.copyIndex = copyIndex;
    item.parentWorld = parentWorld;
    item.model = world;
    item.color = node.color;
    item.hole = node.hole;
    item.selected = selected.contains(node.id);
    item.gizmo = sole && copyIndex == 0 && item.selected;
    item.mesh = node.hole ? holeMesh(node, doc, warning) : localMesh(node, doc, warning);
    out.push_back(std::move(item));
    return;
  }
  if (node.kind == Node::Kind::Pattern) {
    const auto kids = doc.children(node.id);
    if (kids.empty()) return;
    const Node& child = *kids.front();
    const int count = std::max(1, node.count);
    for (int i = 0; i < count; ++i) {
      double pos[3];
      double extra[3];
      double scale[3];
      for (int axis = 0; axis < 3; ++axis) {
        pos[axis] = child.pos[axis] + node.move[axis] * i;
        extra[axis] = node.drot[axis] * i;
        scale[axis] = child.scale[axis] * integerPow(node.resize[axis], i);
      }
      const QMatrix4x4 local = placementMatrix(pos, child.rot, extra, scale);
      if (child.kind == Node::Kind::Shape) {
        DrawItem item;
        item.id = child.id;
        item.copyIndex = i;
        item.parentWorld = world;
        item.model = world * local;
        item.color = child.color;
        item.hole = child.hole;
        item.selected = selected.contains(child.id);
        item.gizmo = sole && i == 0 && item.selected;
        item.mesh = child.hole ? holeMesh(child, doc, warning) : localMesh(child, doc, warning);
        out.push_back(std::move(item));
      } else {
        drawWalk(doc, child, world * local, false, i, selected, sole, out, warning);
      }
    }
    return;
  }
  if (node.kind == Node::Kind::Group && node.multiColor && !node.hole) {
    // One draw item per colour; they share the group's id, so a click on any picks the group.
    bool first = true;
    for (const ColourPiece& piece : colourPieces(doc, node, warning)) {
      if (!piece.mesh || piece.mesh->triangles <= 0) continue;
      DrawItem item;
      item.id = node.id;
      item.copyIndex = copyIndex;
      item.parentWorld = parentWorld;
      item.model = world;
      item.color = piece.color;
      item.selected = selected.contains(node.id);
      item.gizmo = first && sole && copyIndex == 0 && item.selected;
      item.mesh = piece.mesh;
      out.push_back(std::move(item));
      first = false;
    }
    return;
  }
  if (node.kind == Node::Kind::Group) {
    DrawItem item;
    item.id = node.id;
    item.copyIndex = copyIndex;
    item.parentWorld = parentWorld;
    item.model = world;
    item.color = node.color;
    item.selected = selected.contains(node.id);
    item.gizmo = sole && copyIndex == 0 && item.selected;
    bool asHole = false;
    item.mesh = cachedGroupMesh(doc, node, &asHole, warning);
    item.hole = asHole;
    if (meshOf(item.mesh).triangles > 0) out.push_back(std::move(item));
    return;
  }
  for (const Node* child : doc.children(node.id))
    drawWalk(doc, *child, world, true, copyIndex, selected, sole, out, warning);
}

}  // namespace

bool svgPivotOffset(const QString& path, double* x, double* y) {
  if (x) *x = 0;
  if (y) *y = 0;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
  const QString text = QString::fromUtf8(file.readAll());
  const QRegularExpression centerRe(QStringLiteral(R"(data-center\s*=\s*[\"']([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)[\"'])"),
                                     QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch centerMatch = centerRe.match(text);
  if (centerMatch.hasMatch()) {
    if (x) *x = centerMatch.captured(1).toDouble();
    if (y) *y = centerMatch.captured(2).toDouble();
    return true;
  }
  if (QFileInfo(path).fileName().startsWith(QLatin1String("layer-"), Qt::CaseInsensitive)) return true;
  const QRegularExpression polygonRe(QStringLiteral(R"(<polygon\b[^>]*\bpoints\s*=\s*[\"']([^\"']+)[\"'][^>]*>)"),
                                     QRegularExpression::CaseInsensitiveOption);
  const QRegularExpression numberRe(QStringLiteral(R"([-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?)"));
  double minX = 1e300;
  double maxX = -1e300;
  double minY = 1e300;
  double maxY = -1e300;
  bool any = false;
  QRegularExpressionMatchIterator matches = polygonRe.globalMatch(text);
  while (matches.hasNext()) {
    QRegularExpressionMatchIterator it = numberRe.globalMatch(matches.next().captured(1));
    std::vector<double> values;
    while (it.hasNext()) values.push_back(it.next().captured(0).toDouble());
    if (values.size() < 6 || values.size() % 2 != 0) continue;
    for (size_t i = 0; i + 1 < values.size(); i += 2) {
      const double px = values[i];
      const double py = -values[i + 1];
      minX = std::min(minX, px);
      maxX = std::max(maxX, px);
      minY = std::min(minY, py);
      maxY = std::max(maxY, py);
      any = true;
    }
  }
  if (!any) return true;
  if (x) *x = (minX + maxX) * 0.5;
  if (y) *y = (minY + maxY) * 0.5;
  return true;
}

void warmShape(const Node& node, const Document& doc, QString* warning) {
  localShape(node, doc, warning);
}

std::vector<std::vector<double>> sliceSolidForFillet(const Document& doc, const QString& sourceId, const double originPt[3],
                                                      const double normal[3], const double axisPt[3], QString* warning) {
  return sliceSolidImpl(doc, sourceId, originPt, normal, axisPt, warning);
}

std::vector<std::vector<double>> planarFaceRings(const Document& doc, const QString& sourceId, const double originPt[3],
                                                 const double normal[3], const double axisPt[3], QString* warning,
                                                 const std::vector<double>* nearRing,
                                                 std::vector<std::vector<double>>* walls) {
  std::vector<std::vector<double>> rings;
  if (walls) walls->clear();
  const Node* source = doc.find(sourceId);
  if (!source) return rings;
  const Manifold solid = worldSolid(doc, *source, warning);
  if (!healthy(solid)) return rings;
  sf::Frame frame;
  for (int i = 0; i < 3; ++i) {
    frame.origin[i] = originPt[i];
    frame.normal[i] = normal[i];
    frame.axis[i] = axisPt[i];
  }
  std::string log;
  rings = nearRing && nearRing->size() >= 9 ? sf::faceRingsNear(solid, frame, *nearRing, &log, walls)
                                            : sf::coplanarRings(solid, frame, &log, walls);
  if (!log.empty()) logFillet(QString::fromStdString(log).trimmed());
  return rings;
}

std::vector<std::vector<double>> planarFaceRingsAt(const Document& doc, const QString& sourceId, const double originPt[3],
                                                   const double normal[3], const double axisPt[3], const double point[3],
                                                   QString* warning, std::vector<std::vector<double>>* walls) {
  if (walls) walls->clear();
  const Node* source = doc.find(sourceId);
  if (!source) return {};
  const Manifold solid = worldSolid(doc, *source, warning);
  if (!healthy(solid)) return {};
  sf::Frame frame;
  for (int i = 0; i < 3; ++i) {
    frame.origin[i] = originPt[i];
    frame.normal[i] = normal[i];
    frame.axis[i] = axisPt[i];
  }
  std::string log;
  std::vector<std::vector<double>> rings = sf::faceRingsAt(solid, frame, point, &log, walls);
  if (!log.empty()) logFillet(QString::fromStdString(log).trimmed());
  return rings;
}

QMatrix4x4 placementMatrix(const double pos[3], const double rot[3], const double extra[3], const double scale[3]) {
  QMatrix4x4 matrix;
  matrix.translate(pos[0], pos[1], pos[2]);
  matrix.rotate(eulerXYZ(extra));
  matrix.rotate(eulerXYZ(rot));
  matrix.scale(scale[0], scale[1], scale[2]);
  return matrix;
}

void decomposePlacement(const QMatrix4x4& matrix, double pos[3], double rot[3], double scale[3]) {
  pos[0] = matrix(0, 3);
  pos[1] = matrix(1, 3);
  pos[2] = matrix(2, 3);
  QVector3D x(matrix(0, 0), matrix(1, 0), matrix(2, 0));
  QVector3D y(matrix(0, 1), matrix(1, 1), matrix(2, 1));
  QVector3D z(matrix(0, 2), matrix(1, 2), matrix(2, 2));
  double sx = x.length();
  double sy = y.length();
  double sz = z.length();
  if (sx < 1e-8) sx = 1;
  if (sy < 1e-8) sy = 1;
  if (sz < 1e-8) sz = 1;
  if (QVector3D::dotProduct(x, QVector3D::crossProduct(y, z)) < 0) sx = -sx;
  x /= sx;
  y /= sy;
  z /= sz;
  scale[0] = sx;
  scale[1] = sy;
  scale[2] = sz;
  const double m20 = x.z();
  const double m21 = y.z();
  const double m22 = z.z();
  const double m10 = x.y();
  const double m00 = x.x();
  // placementMatrix is Rz * Ry * Rx: X, then Y, then Z.
  const double pitch = std::asin(std::clamp(-m20, -1.0, 1.0));
  double roll = 0;
  double yaw = 0;
  if (std::abs(m20) < 0.9999999) {
    roll = std::atan2(m21, m22);
    yaw = std::atan2(m10, m00);
  } else {
    roll = std::atan2(-z.y(), y.y());
  }
  rot[0] = roll * 180.0 / kPi;
  rot[1] = pitch * 180.0 / kPi;
  rot[2] = yaw * 180.0 / kPi;
}

void decomposePlacementNear(const QMatrix4x4& matrix, const double referenceRot[3], double pos[3], double rot[3], double scale[3]) {
  decomposePlacement(matrix, pos, rot, scale);
  auto wrap180 = [](double a) {
    while (a > 180.0) a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
  };
  auto distance = [&](const double candidate[3]) {
    double d2 = 0;
    for (int i = 0; i < 3; ++i) {
      const double d = wrap180(candidate[i] - referenceRot[i]);
      d2 += d * d;
    }
    return d2;
  };
  const double primary[3] = {rot[0], rot[1], rot[2]};
  const double alternate[3] = {wrap180(rot[0] + 180.0), wrap180(180.0 - rot[1]), wrap180(rot[2] + 180.0)};
  if (distance(alternate) + 1e-6 < distance(primary)) {
    rot[0] = alternate[0];
    rot[1] = alternate[1];
    rot[2] = alternate[2];
  }
}

std::vector<PickEdge> pickEdges(const Document&, const Node& node) {
  return collectPickEdges(node);
}

std::vector<DrawItem> buildEditView(const Document& doc, const QStringList& selected, QString* warning, bool live,
                                    bool* deferred) {
  gLive.on = live;
  gLive.deferred = false;
  struct Done {
    bool* deferred;
    ~Done() {
      if (deferred) *deferred = gLive.deferred;
      gLive = LiveDraw();
    }
  } done{deferred};
  // A new picture: what the last one used may now make way, what this one uses stays.
  groupPassStart() = groupClock();
  {
    std::lock_guard<std::mutex> lock(shapeCacheMutex());
    shapePassStart() = ++cacheClock();
  }
  gWarning.clear();
  if (warning) warning->clear();
  std::vector<DrawItem> items;
  for (const Node* node : doc.children(QString()))
    drawWalk(doc, *node, QMatrix4x4(), true, 0, selected, true, items, warning);
  for (DrawItem& item : items) item.gizmo = false;
  for (DrawItem& item : items) {
    if (item.selected && item.copyIndex == 0) {
      item.gizmo = true;
      break;
    }
  }
  return items;
}

CpuMesh evaluateSolid(const Document& doc, const QStringList& only, QString* error) {
  gWarning.clear();
  QString warning;
  std::vector<Parts> roots;
  if (only.isEmpty()) {
    for (const Node* node : doc.children(QString())) roots.push_back(evalNode(doc, *node, true, &warning));
  } else {
    for (const Node* node : doc.children(QString())) exportWalk(doc, *node, only, {}, roots, &warning);
  }
  Parts all = combine(std::move(roots));
  if (!all.hasSolid || !healthy(all.solid)) {
    if (error) {
      *error = warning.isEmpty()
                   ? QStringLiteral("Nothing solid to export. Add a solid, or the holes ate the whole part.")
                   : warning;
    }
    return {};
  }
  CpuMesh mesh = meshFrom(exportReady(all.solid));
  if (mesh.triangles == 0 && error) *error = QStringLiteral("The solid produced an empty mesh.");
  return mesh;
}

// The solid's mesh in doubles, with the vertices Manifold keeps apart for their properties joined.
static void exactMesh(const Manifold& solid, std::vector<double>& points, std::vector<uint32_t>& triangles) {
  points.clear();
  triangles.clear();
  const manifold::MeshGL64 gl = solid.GetMeshGL64();
  const size_t stride = std::max<size_t>(3, gl.numProp);
  const size_t count = gl.vertProperties.size() / stride;
  std::vector<uint64_t> parent(count);
  for (size_t i = 0; i < count; ++i) parent[i] = i;
  auto root = [&](uint64_t v) {
    while (parent[v] != v) v = parent[v] = parent[parent[v]];
    return v;
  };
  for (size_t i = 0; i < std::min(gl.mergeFromVert.size(), gl.mergeToVert.size()); ++i) {
    const uint64_t a = gl.mergeFromVert[i], b = gl.mergeToVert[i];
    if (a < count && b < count) parent[root(a)] = root(b);
  }
  std::vector<int64_t> index(count, -1);
  for (size_t i = 0; i < count; ++i) {
    const uint64_t r = root(i);
    if (index[r] < 0) {
      index[r] = static_cast<int64_t>(points.size() / 3);
      for (int k = 0; k < 3; ++k) points.push_back(gl.vertProperties[r * stride + static_cast<size_t>(k)]);
    }
  }
  triangles.reserve(gl.triVerts.size());
  for (uint64_t v : gl.triVerts) {
    if (v >= count) {
      points.clear();
      triangles.clear();
      return;
    }
    triangles.push_back(static_cast<uint32_t>(index[root(v)]));
  }
}

std::vector<ExportObject> evaluateObjects(const Document& doc, const QStringList& only, QString* error) {
  gWarning.clear();
  QString warning;
  struct Item {
    QString name;
    Parts parts;
    QColor color;
    std::vector<std::pair<QColor, Manifold>> pieces;  // a multi-colour group: one solid per colour
    QString id;
  };
  std::vector<Item> items;

  std::function<void(const Node&, std::vector<const Node*>)> walk;
  walk = [&](const Node& node, std::vector<const Node*> ancestors) {
    const bool chosen = only.isEmpty() || (listed(only, node.id) && !ancestorListed(doc, only, node.id));
    if (only.isEmpty() || chosen) {
      Item item;
      item.id = node.id;
      item.name = node.name;
      item.color = node.color;
      item.parts = evalNode(doc, node, true, &warning);
      if (node.kind == Node::Kind::Group && node.multiColor && !node.hole) {
        const auto pieces = colourPieces(doc, node, &warning);
        if (pieces.size() > 1)
          for (const ColourPiece& piece : pieces) {
            Parts placed;
            placed.solid = piece.solid;
            placed.hasSolid = true;
            placed = transformed(std::move(placed), node.pos, node.rot, kZero, node.scale);
            item.pieces.push_back({piece.color, placed.solid});
          }
      }
      if (!only.isEmpty()) {
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
          item.parts = transformed(std::move(item.parts), (*it)->pos, (*it)->rot, kZero, (*it)->scale);
          for (auto& piece : item.pieces) {
            Parts placed;
            placed.solid = piece.second;
            placed.hasSolid = true;
            piece.second = transformed(std::move(placed), (*it)->pos, (*it)->rot, kZero, (*it)->scale).solid;
          }
        }
      }
      items.push_back(std::move(item));
      return;
    }
    ancestors.push_back(&node);
    for (const Node* child : doc.children(node.id)) walk(*child, ancestors);
  };
  for (const Node* node : doc.children(QString())) walk(*node, {});

  // Holes left loose on the bed (or picked along with the parts) cut every part they reach, the
  // same as in the view and in the one-piece export.
  std::vector<Manifold> looseHoles;
  for (auto& item : items)
    if (item.parts.hasHole && healthy(item.parts.hole)) looseHoles.push_back(item.parts.hole);
  Manifold cutter;
  const bool cutting = !looseHoles.empty();
  if (cutting) cutter = looseHoles.size() == 1 ? looseHoles.front() : Manifold::BatchBoolean(looseHoles, manifold::OpType::Add);

  std::vector<ExportObject> out;
  QStringList used;
  auto cleanName = [&](QString name) {
    name.replace(QLatin1Char('\n'), QLatin1Char(' '));
    name.replace(QLatin1Char('\r'), QLatin1Char(' '));
    name.replace(QLatin1Char('#'), QLatin1Char(' '));
    name = name.simplified();
    if (name.isEmpty()) name = QStringLiteral("Part");
    QString unique = name;
    for (int n = 2; used.contains(unique); ++n) unique = name + QLatin1Char(' ') + QString::number(n);
    used << unique;
    return unique;
  };
  QString currentId;
  auto addObject = [&](Manifold solid, const QString& name, const QColor& color, const QString& assembly) {
    if (cutting) {
      solid = solid - cutter;
      if (!healthy(solid) || solid.IsEmpty()) return;  // the holes took all of it
    }
    const Manifold ready = exportReady(solid);
    CpuMesh mesh = meshFrom(ready);
    if (mesh.triangles <= 0) return;
    ExportObject object;
    object.name = name;
    object.mesh = std::move(mesh);
    exactMesh(ready, object.points, object.triangles);
    object.color = color;
    object.assembly = assembly;
    object.nodeId = currentId;
    out.push_back(std::move(object));
  };
  for (auto& item : items) {
    if (!item.parts.hasSolid || !healthy(item.parts.solid)) continue;
    currentId = item.id;
    if (item.pieces.size() > 1) {
      // One object per colour, kept together as the parts of one model.
      const QString assembly = cleanName(item.name);
      int index = 0;
      for (auto& [color, solid] : item.pieces)
        addObject(solid, cleanName(QStringLiteral("%1 colour %2").arg(assembly).arg(++index)), color, assembly);
      continue;
    }
    addObject(item.parts.solid, cleanName(item.name), item.color, QString());
  }
  if (out.empty() && error) {
    *error = warning.isEmpty() ? QStringLiteral("Nothing solid to export. Add a solid, or the holes ate the whole part.") : warning;
  }
  return out;
}

double edgeFilletUsed(const Node& node) { return filletRadiusUsed(node); }

TextSpec textSpecFor(const Node& node) {
  TextSpec spec;
  spec.text = node.text;
  spec.family = node.fontFamily;
  spec.style = node.fontStyle;
  spec.capHeight = std::max(node.fontSize, 0.1);
  spec.spacing = node.letterSpacing;
  spec.align = std::clamp(node.textAlign, 0, 2);
  spec.tolerance = textDetailTolerance(node.textDetail);
  return spec;
}

bool nodeLocalMesh(const Document& doc, const Node& node, std::vector<double>* points, std::vector<uint32_t>* triangles, bool* hole) {
  QString warning;
  const Parts parts = evalNode(doc, node, false, &warning);
  const bool solid = parts.hasSolid && healthy(parts.solid);
  if (!solid && !(parts.hasHole && healthy(parts.hole))) return false;
  if (hole) *hole = !solid;
  exactMesh(exportReady(solid ? parts.solid : parts.hole), *points, *triangles);
  return !triangles->empty();
}

std::vector<std::vector<double>> sliceContours(const Document& doc, const QStringList& only, const double origin[3],
                                                const double normal[3], QString* error, QString* note) {
  if (note) note->clear();
  gWarning.clear();
  QString warning;
  std::vector<Parts> roots;
  if (only.isEmpty()) {
    for (const Node* node : doc.children(QString())) roots.push_back(evalNode(doc, *node, true, &warning));
  } else {
    for (const Node* node : doc.children(QString())) exportWalk(doc, *node, only, {}, roots, &warning);
  }
  Parts all = combine(std::move(roots));
  if (!all.hasSolid || !healthy(all.solid)) {
    if (error) {
      *error = warning.isEmpty() ? QStringLiteral("Nothing solid to slice. Add a solid, or the holes ate the whole part.")
                                 : warning;
    }
    return {};
  }
  Manifold solid = exportReady(all.solid);
  if (!healthy(solid)) solid = all.solid;

  vec3 n(normal[0], normal[1], normal[2]);
  const double nlen = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
  n = nlen < 1e-8 ? vec3(0, 0, 1) : vec3(n.x / nlen, n.y / nlen, n.z / nlen);
  // Which way is into the solid, for the small nudge below when the plane lies on a face.
  vec3 inward(-n.x, -n.y, -n.z);
  // A flat face (top or bottom) is drawn as seen from above, in the bed's own X and Y: picking
  // the underside must not mirror the drawing.
  if (std::abs(n.z) > 0.999) n = vec3(0, 0, 1);
  const vec3 hint = std::abs(n.z) < 0.9 ? vec3(0, 0, 1) : vec3(1, 0, 0);
  const double ndot = hint.x * n.x + hint.y * n.y + hint.z * n.z;
  vec3 axisU(hint.x - n.x * ndot, hint.y - n.y * ndot, hint.z - n.z * ndot);
  const double ulen = std::sqrt(axisU.x * axisU.x + axisU.y * axisU.y + axisU.z * axisU.z);
  if (ulen < 1e-8) {
    if (error) *error = QStringLiteral("That plane has no direction.");
    return {};
  }
  axisU = vec3(axisU.x / ulen, axisU.y / ulen, axisU.z / ulen);
  const vec3 axisV(n.y * axisU.z - n.z * axisU.y, n.z * axisU.x - n.x * axisU.z, n.x * axisU.y - n.y * axisU.x);
  // Only the plane's offset along the normal counts: the drawing's zero is the bed origin seen on
  // the plane, not the spot that was clicked, so two clicks on one face give the same file.
  const double along = origin[0] * n.x + origin[1] * n.y + origin[2] * n.z;
  const vec3 o(n.x * along, n.y * along, n.z * along);
  manifold::mat3x4 plane;
  plane[0] = vec3(axisU.x, axisV.x, n.x);
  plane[1] = vec3(axisU.y, axisV.y, n.y);
  plane[2] = vec3(axisU.z, axisV.z, n.z);
  plane[3] = vec3(-(axisU.x * o.x + axisU.y * o.y + axisU.z * o.z), -(axisV.x * o.x + axisV.y * o.y + axisV.z * o.z),
                  -(n.x * o.x + n.y * o.y + n.z * o.z));
  const Manifold placed = solid.Transform(plane);

  auto ringsOf = [](const Polygons& polys) {
    std::vector<std::vector<double>> rings;
    for (const SimplePolygon& poly : polys) {
      if (poly.size() < 3) continue;
      std::vector<double> ring;
      ring.reserve(poly.size() * 2);
      for (const vec2& p : poly) {
        if (!ring.empty() && std::abs(p.x - ring[ring.size() - 2]) < 1e-4 && std::abs(p.y - ring.back()) < 1e-4) continue;
        ring.push_back(p.x);
        ring.push_back(p.y);
      }
      if (ring.size() >= 6 && std::abs(ring[0] - ring[ring.size() - 2]) < 1e-4 && std::abs(ring[1] - ring.back()) < 1e-4) {
        ring.pop_back();
        ring.pop_back();
      }
      if (ring.size() >= 6) rings.push_back(std::move(ring));
    }
    return rings;
  };

  std::vector<std::vector<double>> rings = ringsOf(placed.Slice(0));
  if (rings.empty()) {
    // On a face: step 0.05 mm into the solid (and if that side is empty, the other way).
    const double into = (inward.x * n.x + inward.y * n.y + inward.z * n.z) < 0 ? -0.05 : 0.05;
    for (double step : {into, -into}) {
      rings = ringsOf(placed.Slice(step));
      if (rings.empty()) continue;
      if (note) *note = QStringLiteral("The plane sat on the surface, so the cut is 0.05 mm inside.");
      break;
    }
  }
  if (rings.empty() && error) {
    const manifold::Box box = solid.BoundingBox();
    if (std::abs(n.z) > 0.999) {
      *error = QStringLiteral("Nothing at Z %1 mm. The solid runs from %2 to %3.")
                   .arg(origin[2], 0, 'f', 2)
                   .arg(box.min.z, 0, 'f', 2)
                   .arg(box.max.z, 0, 'f', 2);
    } else {
      *error = QStringLiteral("That plane misses the solid.");
    }
  }
  return rings;
}

namespace {

bool meshBounds(const CpuMesh& mesh, double lo[3], double hi[3]) {
  if (mesh.hasBounds) {
    for (int a = 0; a < 3; ++a) {
      lo[a] = mesh.lo[a];
      hi[a] = mesh.hi[a];
    }
    return true;
  }
  if (mesh.vertices.size() < 3) return false;
  for (int a = 0; a < 3; ++a) {
    lo[a] = 1e9;
    hi[a] = -1e9;
  }
  for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3) {
    for (int a = 0; a < 3; ++a) {
      const double value = mesh.vertices[i + static_cast<size_t>(a)];
      lo[a] = std::min(lo[a], value);
      hi[a] = std::max(hi[a], value);
    }
  }
  return lo[0] <= hi[0];
}

void includePoint(double lo[3], double hi[3], const QVector3D& point, bool* any) {
  const double p[3] = {point.x(), point.y(), point.z()};
  for (int a = 0; a < 3; ++a) {
    lo[a] = std::min(lo[a], p[a]);
    hi[a] = std::max(hi[a], p[a]);
  }
  *any = true;
}

QMatrix4x4 worldOf(const Document& doc, const Node& node) {
  std::vector<const Node*> chain;
  const Node* cursor = &node;
  while (cursor) {
    chain.push_back(cursor);
    if (cursor->parentId.isEmpty()) break;
    const Node* next = doc.find(cursor->parentId);
    if (!next) break;
    cursor = next;
  }
  QMatrix4x4 world;
  const double zero[3] = {0, 0, 0};
  for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    world = world * placementMatrix((*it)->pos, (*it)->rot, zero, (*it)->scale);
  return world;
}

bool underId(const Document& doc, const QString& root, const QString& id) {
  const Node* node = doc.find(id);
  while (node) {
    if (node->id == root) return true;
    if (node->parentId.isEmpty()) return false;
    node = doc.find(node->parentId);
  }
  return false;
}

}  // namespace

bool groupMeshReady(const Document& doc, const Node& group) {
  if (group.kind != Node::Kind::Group) return true;
  const std::string key = groupKey(doc, group);
  std::lock_guard<std::mutex> lock(groupMeshMutex());
  return groupMeshStore().count(key) > 0;
}

bool meshReady(const Document& doc, const Node& node) {
  if (node.kind == Node::Kind::Group) return groupMeshReady(doc, node);
  if (node.kind != Node::Kind::Shape || node.shape != QLatin1String("stl")) return true;
  const std::string key = cacheKey(node, doc);
  std::lock_guard<std::mutex> lock(shapeCacheMutex());
  const auto hit = cache().find(key);
  return hit != cache().end() && hit->second.mesh;
}

bool localBounds(const Document& doc, const Node& node, double lo[3], double hi[3]) {
  QString warning;
  if (node.kind == Node::Kind::Shape) {
    const auto mesh = localMesh(node, doc, &warning);
    return meshBounds(meshOf(mesh), lo, hi);
  }
  if (node.kind == Node::Kind::Group) {
    const auto items = buildEditView(doc, QStringList{node.id}, &warning);
    for (const DrawItem& item : items)
      if (item.id == node.id) return meshBounds(meshOf(item.mesh), lo, hi);
    return false;
  }
  bool ok = true;
  const QMatrix4x4 local = worldOf(doc, node).inverted(&ok);
  if (!ok) return false;
  for (int a = 0; a < 3; ++a) {
    lo[a] = 1e9;
    hi[a] = -1e9;
  }
  bool any = false;
  const auto items = buildEditView(doc, {}, &warning);
  for (const DrawItem& item : items) {
    if (item.id == node.id || !underId(doc, node.id, item.id)) continue;
    const std::vector<float>& v = meshOf(item.mesh).vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3)
      includePoint(lo, hi, local.map(item.model.map(QVector3D(v[i], v[i + 1], v[i + 2]))), &any);
  }
  return any;
}

QMatrix4x4 nodeWorld(const Document& doc, const QString& id) {
  const Node* node = doc.find(id);
  if (!node) return {};
  return worldOf(doc, *node);
}

bool worldMesh64(const Document& doc, const QStringList& only, std::vector<double>* vertices, std::vector<uint64_t>* triangles) {
  QString warning;
  std::vector<Parts> roots;
  for (const Node* node : doc.children(QString())) exportWalk(doc, *node, only, {}, roots, &warning);
  Parts all = combine(std::move(roots));
  if (!all.hasSolid || !healthy(all.solid)) return false;
  const manifold::MeshGL64 gl = all.solid.GetMeshGL64();
  const size_t stride = std::max<size_t>(3, gl.numProp);
  vertices->clear();
  for (size_t i = 0; i + 2 < gl.vertProperties.size(); i += stride) {
    vertices->push_back(gl.vertProperties[i]);
    vertices->push_back(gl.vertProperties[i + 1]);
    vertices->push_back(gl.vertProperties[i + 2]);
  }
  triangles->assign(gl.triVerts.begin(), gl.triVerts.end());
  return true;
}

EdgeBetaView edgeBetaView(const Document& doc, const Node& node, QString* warning) {
  EdgeBetaView view;
  if (node.kind == Node::Kind::Pattern) return view;
  Node bare = node;
  bare.edgeBeta.clear();
  Manifold base;
  if (node.kind == Node::Kind::Shape) {
    base = localShape(bare, doc, warning);
  } else {
    // evalGroup reads the children through the document, so a copy with the same id works.
    Parts parts = evalGroup(doc, bare, false, warning);
    if (parts.hasSolid) base = parts.solid;
  }
  if (!healthy(base)) return view;
  view.pieces = static_cast<int>(base.Decompose().size());
  const vec3 s = edgeBetaScale(node);
  const std::vector<ef::Chain> chains = ef::findChains(base.Scale(s));
  auto unscale = [&](double x, double y, double z) { return QVector3D(float(x / s.x), float(y / s.y), float(z / s.z)); };
  auto unscaleNormal = [&](double x, double y, double z) { return QVector3D(float(x * s.x), float(y * s.y), float(z * s.z)).normalized(); };
  for (size_t c = 0; c < chains.size(); ++c) {
    const ef::Chain& chain = chains[c];
    const size_t n = chain.pts.size() / 3;
    std::vector<double> line;
    for (size_t k = 0; k <= n; ++k) {
      if (k == n && !chain.closed) break;
      const size_t i = k % n;
      const QVector3D p = unscale(chain.pts[i * 3], chain.pts[i * 3 + 1], chain.pts[i * 3 + 2]);
      line.push_back(p.x());
      line.push_back(p.y());
      line.push_back(p.z());
    }
    const size_t segs = chain.closed ? n : n - 1;
    for (size_t k = 0; k < segs; ++k) {
      const size_t i = k, j = (k + 1) % n;
      PickEdge edge;
      edge.key = static_cast<int>(c);
      edge.a = unscale(chain.pts[i * 3], chain.pts[i * 3 + 1], chain.pts[i * 3 + 2]);
      edge.b = unscale(chain.pts[j * 3], chain.pts[j * 3 + 1], chain.pts[j * 3 + 2]);
      edge.nA = unscaleNormal(chain.nA[i * 3] + chain.nA[j * 3], chain.nA[i * 3 + 1] + chain.nA[j * 3 + 1],
                              chain.nA[i * 3 + 2] + chain.nA[j * 3 + 2]);
      edge.nB = unscaleNormal(chain.nB[i * 3] + chain.nB[j * 3], chain.nB[i * 3 + 1] + chain.nB[j * 3 + 1],
                              chain.nB[i * 3 + 2] + chain.nB[j * 3 + 2]);
      view.edges.push_back(edge);
    }
    view.chains.push_back(std::move(line));
    view.concave.push_back(chain.concave);
  }
  for (ef::Pick pick : ef::decodePicks(node.edgeBeta)) {
    if (view.radius <= 0) {
      view.radius = pick.radius;
      view.chamfer = pick.chamfer;
    }
    for (size_t i = 0; i + 2 < pick.pts.size(); i += 3) {
      pick.pts[i] *= s.x;
      pick.pts[i + 1] *= s.y;
      pick.pts[i + 2] *= s.z;
    }
    const std::vector<int> found = ef::matchPick(chains, pick.pts);
    if (found.empty()) ++view.lost;
    for (int index : found)
      if (std::find(view.picked.begin(), view.picked.end(), index) == view.picked.end()) view.picked.push_back(index);
  }
  return view;
}

// Labels in generators (name tags, the drill index, test pieces) in the system font, bold.
void hookGeneratorText() {
  static std::once_flag once;
  std::call_once(once, [] {
    gen::setTextOutlines([](const QString& text, double capHeight) -> CrossSection {
      TextSpec spec;
      spec.text = text;
      spec.style = QStringLiteral("Bold");
      spec.capHeight = capHeight;
      spec.tolerance = 0.02;
      std::vector<TextLoop> loops;
      QString why;
      if (!textOutlines(spec, &loops, &why)) return {};
      Polygons polys;
      for (const TextLoop& loop : loops) {
        SimplePolygon poly;
        for (size_t i = 0; i + 1 < loop.size(); i += 2) poly.push_back(vec2(loop[i], loop[i + 1]));
        polys.push_back(std::move(poly));
      }
      const CrossSection shape(polys, CrossSection::FillRule::NonZero);
      if (shape.IsEmpty()) return {};
      const manifold::Rect box = shape.Bounds();
      return shape.Translate(vec2(-(box.min.x + box.max.x) * 0.5, -(box.min.y + box.max.y) * 0.5));
    });
  });
}
