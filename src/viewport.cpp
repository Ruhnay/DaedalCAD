#include "viewport.h"

#include <QSettings>

#include "pickindex.h"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QDateTime>
#include <QLineEdit>
#include <QLineF>
#include <QPolygonF>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTimer>
#include <QSet>

#include <QDragEnterEvent>
#include <QKeyEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QMatrix3x3>
#include <QQuaternion>
#include <QSurfaceFormat>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace {

// Meshes come as shared corners only (no normals): each pixel works out its face's normal from how
// the position changes across the screen, which is exactly the flat facet normal. A quarter of the
// memory of a copy per corner with a normal, on the graphics card and in RAM.
const char* kMeshVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMvp;
uniform mat4 uModel;
out vec3 vWorld;
void main() {
  vWorld = (uModel * vec4(aPos, 1.0)).xyz;
  gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

const char* kMeshFrag = R"(#version 330 core
in vec3 vWorld;
uniform vec3 uEye;       // the camera, to turn each face's normal towards it
uniform vec3 uColor;
uniform float uHole;
uniform float uSelected;
uniform vec4 uClip;      // section view: everything with dot(p, xyz) > w is cut away
uniform float uClipOn;
uniform vec2 uOverhang;  // overhang shading: x > 0.5 on, y = -sin(the printer's overhang angle from vertical)
uniform float uPlate;    // height of the plate: faces lying on it need no support
out vec4 frag;
void main() {
  if (uClipOn > 0.5 && dot(vWorld, uClip.xyz) > uClip.w) discard;
  // Where a part is cut open its inside shows: paint that as the cut face, hatched like a drawing.
  if (uClipOn > 0.5 && !gl_FrontFacing && uHole < 0.5) {
    float hatch = mod(gl_FragCoord.x + gl_FragCoord.y, 9.0) < 1.6 ? 0.55 : 1.0;
    frag = vec4(mix(uColor, vec3(0.86, 0.22, 0.16), 0.55) * 0.82 * hatch, 1.0);
    return;
  }
  // The face's normal, towards the camera (as a front face's outward normal is).
  vec3 n = cross(dFdx(vWorld), dFdy(vWorld));
  n = dot(n, n) > 0.0 ? normalize(n) : vec3(0.0, 0.0, 1.0);
  if (dot(n, uEye - vWorld) < 0.0) n = -n;
  vec3 key = normalize(vec3(0.45, -0.55, 0.85));
  vec3 fill = normalize(vec3(-0.7, 0.45, 0.25));
  float light = 0.32 + 0.18 * (n.z * 0.5 + 0.5) + max(dot(n, key), 0.0) * 0.72 + max(dot(n, fill), 0.0) * 0.22;
  vec3 color = uColor * light;
  if (uSelected > 0.5) color = mix(color, vec3(0.910, 0.455, 0.0), 0.42);
  // Faces that hang over steeper than the printer manages (and do not lie on the plate): red stripes.
  if (uOverhang.x > 0.5 && uHole < 0.5 && gl_FrontFacing && n.z < uOverhang.y && vWorld.z > uPlate + 0.05) {
    float stripe = mod(gl_FragCoord.x - gl_FragCoord.y, 12.0) < 6.0 ? 1.0 : 0.62;
    color = mix(color, vec3(0.93, 0.16, 0.12) * stripe, 0.62);
  }
  if (uHole > 0.5) {
    color = mix(color, vec3(0.831, 0.396, 0.310), 0.55);
    frag = vec4(color, 0.42);
  } else {
    frag = vec4(color, 1.0);
  }
}
)";

const char* kLineVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMvp;
void main() { gl_Position = uMvp * vec4(aPos, 1.0); }
)";

const char* kLineFrag = R"(#version 330 core
uniform vec3 uColor;
out vec4 frag;
void main() { frag = vec4(uColor, 1.0); }
)";

// The grid: each line knows which plate coordinate it holds (x or y) and how far apart its
// neighbours are. Where they come closer on screen than a few pixels (far away, or seen at a low
// angle) they fade out, so the plate shows no moire of crowded lines.
const char* kGridVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMvp;
out vec2 vPlane;
void main() {
  vPlane = aPos.xy;
  gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

const char* kGridFrag = R"(#version 330 core
uniform vec3 uColor;
uniform float uSpacing;  // mm between neighbouring lines; 0: never fades
uniform int uAxis;       // 0: lines at a fixed x, 1: at a fixed y
in vec2 vPlane;
out vec4 frag;
void main() {
  float alpha = 1.0;
  if (uSpacing > 0.0) {
    float c = uAxis == 0 ? vPlane.x : vPlane.y;
    float perPixel = length(vec2(dFdx(c), dFdy(c)));
    float pixels = uSpacing / max(perPixel, 1e-6);
    alpha = smoothstep(3.0, 9.0, pixels);
  }
  if (alpha <= 0.01) discard;
  frag = vec4(uColor, alpha);
}
)";

GLuint compileShader(QOpenGLFunctions_3_3_Core* gl, GLenum type, const char* source) {
  GLuint shader = gl->glCreateShader(type);
  gl->glShaderSource(shader, 1, &source, nullptr);
  gl->glCompileShader(shader);
  GLint ok = 0;
  gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    gl->glDeleteShader(shader);
    return 0;
  }
  return shader;
}

GLuint linkProgram(QOpenGLFunctions_3_3_Core* gl, const char* vert, const char* frag) {
  GLuint vs = compileShader(gl, GL_VERTEX_SHADER, vert);
  GLuint fs = compileShader(gl, GL_FRAGMENT_SHADER, frag);
  if (!vs || !fs) return 0;
  GLuint program = gl->glCreateProgram();
  gl->glAttachShader(program, vs);
  gl->glAttachShader(program, fs);
  gl->glLinkProgram(program);
  gl->glDeleteShader(vs);
  gl->glDeleteShader(fs);
  GLint ok = 0;
  gl->glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    gl->glDeleteProgram(program);
    return 0;
  }
  return program;
}

bool hitTriangle(const QVector3D& origin, const QVector3D& dir, const QVector3D& a, const QVector3D& b, const QVector3D& c, float* t) {
  const QVector3D e1 = b - a;
  const QVector3D e2 = c - a;
  const QVector3D p = QVector3D::crossProduct(dir, e2);
  const float det = QVector3D::dotProduct(e1, p);
  if (std::abs(det) < 1e-7f) return false;
  const float inv = 1.f / det;
  const QVector3D s = origin - a;
  const float u = inv * QVector3D::dotProduct(s, p);
  if (u < 0.f || u > 1.f) return false;
  const QVector3D q = QVector3D::crossProduct(s, e1);
  const float v = inv * QVector3D::dotProduct(dir, q);
  if (v < 0.f || u + v > 1.f) return false;
  const float dist = inv * QVector3D::dotProduct(e2, q);
  if (dist < 0.f) return false;
  *t = dist;
  return true;
}

float axisParam(const QVector3D& origin, const QVector3D& dir, const QVector3D& pivot, const QVector3D& axis) {
  const QVector3D w0 = origin - pivot;
  const float a = QVector3D::dotProduct(dir, dir);
  const float b = QVector3D::dotProduct(dir, axis);
  const float c = QVector3D::dotProduct(axis, axis);
  const float d = QVector3D::dotProduct(dir, w0);
  const float e = QVector3D::dotProduct(axis, w0);
  const float denom = a * c - b * b;
  if (std::abs(denom) < 1e-8f) return 0;
  return (a * e - b * d) / denom;
}

void push3(std::vector<float>& out, const QVector3D& p) {
  out.push_back(p.x());
  out.push_back(p.y());
  out.push_back(p.z());
}

void pushTri(std::vector<float>& out, const QVector3D& a, const QVector3D& b, const QVector3D& c) {
  push3(out, a);
  push3(out, b);
  push3(out, c);
}

void axisBasis(const QMatrix4x4& model, const QVector3D& pivot, int axis, QVector3D* dir, QVector3D* u, QVector3D* v) {
  QVector3D local(axis == 0 ? 1.f : 0.f, axis == 1 ? 1.f : 0.f, axis == 2 ? 1.f : 0.f);
  QVector3D other(axis == 2 ? 1.f : 0.f, axis == 0 ? 1.f : 0.f, axis == 1 ? 1.f : 0.f);
  *dir = (model.map(local) - pivot);
  if (dir->lengthSquared() < 1e-8f) *dir = local;
  else *dir = dir->normalized();
  *u = (model.map(other) - pivot);
  *u = *u - *dir * QVector3D::dotProduct(*u, *dir);
  if (u->lengthSquared() < 1e-8f) *u = QVector3D::crossProduct(*dir, QVector3D(0, 0, 1));
  if (u->lengthSquared() < 1e-8f) *u = QVector3D::crossProduct(*dir, QVector3D(0, 1, 0));
  *u = u->normalized();
  *v = QVector3D::crossProduct(*dir, *u).normalized();
}

void addRod(std::vector<float>& out, const QVector3D& from, const QVector3D& to, float radius) {
  const QVector3D axis = to - from;
  const float len = axis.length();
  if (len < 1e-4f) return;
  const QVector3D n = axis / len;
  QVector3D side = QVector3D::crossProduct(n, QVector3D(0, 0, 1));
  if (side.lengthSquared() < 1e-6f) side = QVector3D::crossProduct(n, QVector3D(0, 1, 0));
  side.normalize();
  const QVector3D up = QVector3D::crossProduct(n, side).normalized();
  const QVector3D a = from + side * radius;
  const QVector3D b = from - side * radius;
  const QVector3D c = from + up * radius;
  const QVector3D d = from - up * radius;
  const QVector3D e = to + side * radius;
  const QVector3D f = to - side * radius;
  const QVector3D g = to + up * radius;
  const QVector3D h = to - up * radius;
  pushTri(out, a, e, b);
  pushTri(out, b, e, f);
  pushTri(out, c, g, d);
  pushTri(out, d, g, h);
}

void addArrow(std::vector<float>& out, const QVector3D& from, const QVector3D& to) {
  const QVector3D axis = to - from;
  const float len = axis.length();
  if (len < 1e-4f) return;
  const float head = len * 0.22f;
  const float radius = len * 0.035f;
  const QVector3D n = axis / len;
  const QVector3D base = to - n * head;
  addRod(out, from, base, radius);
  QVector3D side = QVector3D::crossProduct(n, QVector3D(0, 0, 1));
  if (side.lengthSquared() < 1e-6f) side = QVector3D::crossProduct(n, QVector3D(0, 1, 0));
  side.normalize();
  const QVector3D up = QVector3D::crossProduct(n, side).normalized();
  const float hr = radius * 2.8f;
  const QVector3D p0 = base + side * hr;
  const QVector3D p1 = base - side * hr;
  const QVector3D p2 = base + up * hr;
  const QVector3D p3 = base - up * hr;
  pushTri(out, p0, to, p1);
  pushTri(out, p2, to, p0);
  pushTri(out, p1, to, p3);
  pushTri(out, p3, to, p2);
}

void addCube(std::vector<float>& out, const QVector3D& center, float half) {
  const QVector3D x(half, 0, 0);
  const QVector3D y(0, half, 0);
  const QVector3D z(0, 0, half);
  const QVector3D c[8] = {center - x - y - z, center + x - y - z, center + x + y - z, center - x + y - z,
                          center - x - y + z, center + x - y + z, center + x + y + z, center - x + y + z};
  const int faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 3, 7, 4}};
  for (const auto& face : faces) {
    pushTri(out, c[face[0]], c[face[1]], c[face[2]]);
    pushTri(out, c[face[0]], c[face[2]], c[face[3]]);
  }
}

// A flat round dot facing the camera.
void addDisc(std::vector<float>& out, const QVector3D& center, const QVector3D& right, const QVector3D& up, float radius) {
  const int segs = 28;
  QVector3D prev = center + right * radius;
  for (int i = 1; i <= segs; ++i) {
    const float a = 6.28318530718f * static_cast<float>(i) / segs;
    const QVector3D next = center + (right * std::cos(a) + up * std::sin(a)) * radius;
    pushTri(out, center, prev, next);
    prev = next;
  }
}

void addRing(std::vector<float>& out, const QVector3D& pivot, const QVector3D& u, const QVector3D& v, float radius, float tube,
             bool withHead) {
  const int segs = 36;
  const float sweep = 5.6f;
  const float start = -0.5f;
  QVector3D prev = pivot + u * std::cos(start) * radius + v * std::sin(start) * radius;
  for (int i = 1; i <= segs; ++i) {
    const float a = start + sweep * (static_cast<float>(i) / segs);
    const QVector3D next = pivot + u * std::cos(a) * radius + v * std::sin(a) * radius;
    addRod(out, prev, next, tube);
    if (withHead && i == segs) {
      const QVector3D tangent = (next - prev).normalized();
      addArrow(out, next - tangent * radius * 0.18f, next + tangent * radius * 0.02f);
    }
    prev = next;
  }
}

}  // namespace

namespace {

QVector3D edgeOutward(const PickEdge& edge) {
  const QVector3D mid = (edge.a + edge.b) * 0.5f;
  const QVector3D dir = edge.b - edge.a;
  QVector3D radial(mid.x(), mid.y(), 0);
  if (radial.lengthSquared() > 1e-4f && dir.lengthSquared() > 1e-6f &&
      std::abs(QVector3D::dotProduct(dir.normalized(), radial.normalized())) < 0.35f) {
    return radial.normalized();
  }
  QVector3D normal(0, 0, 0);
  if (std::abs(edge.a.x() - edge.b.x()) > 1e-3f) normal = QVector3D(0, mid.y(), mid.z());
  else if (std::abs(edge.a.y() - edge.b.y()) > 1e-3f) normal = QVector3D(mid.x(), 0, mid.z());
  else normal = QVector3D(mid.x(), mid.y(), 0);
  if (normal.lengthSquared() < 1e-8f) return QVector3D(0, 0, 1);
  return normal.normalized();
}

}  // namespace

int smoothingSamples() {
  const int asked = QSettings().value(QStringLiteral("display/smoothSamples"), 4).toInt();
  return asked >= 8 ? 8 : asked >= 4 ? 4 : asked >= 2 ? 2 : 0;
}

Viewport::Viewport(QWidget* parent) : QOpenGLWidget(parent) {
  QSurfaceFormat format;
  // Several samples per pixel (Settings > Display): smooth edges on parts, outlines and grid
  // lines instead of stairs.
  format.setSamples(smoothingSamples());
  format.setDepthBufferSize(24);
  format.setStencilBufferSize(8);
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  setFormat(format);
  setAcceptDrops(true);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setCursor(Qt::ArrowCursor);
  // Space anywhere in the window while the cursor is over the view (the keyboard focus is often
  // in the side panel).
  qApp->installEventFilter(this);
  setOrbitAngles(-0.75f, 0.92f);
  cameraGoal = camera;
  easeTimer = new QTimer(this);
  easeTimer->setInterval(8);
  easeTimer->setTimerType(Qt::PreciseTimer);
  connect(easeTimer, &QTimer::timeout, this, [this] { easeStep(); });
}

QVector3D Viewport::clampFocus(QVector3D p) const {
  const float hx = static_cast<float>(bedW * 1.5);  // half the bed plus one bed past each edge
  const float hy = static_cast<float>(bedD * 1.5);
  const float size = static_cast<float>(std::max(bedW, bedD));
  const float z = static_cast<float>(workplaneZ);
  p.setX(std::clamp(p.x(), -hx, hx));
  p.setY(std::clamp(p.y(), -hy, hy));
  p.setZ(std::clamp(p.z(), z - size * 0.5f, z + size));
  return p;
}

bool Viewport::focusOnBed(const QPointF& pos, QVector3D* hit) const {
  QVector3D origin;
  QVector3D dir;
  cameraRay(pos, &origin, &dir);
  if (std::abs(dir.z()) < 1e-5f) return false;
  const float t = (static_cast<float>(workplaneZ) - origin.z()) / dir.z();
  if (t <= 0.f) return false;
  *hit = clampFocus(origin + dir * t);
  return true;
}

void Viewport::beginNavigation() {
  if (easeTimer && easeTimer->isActive()) return;
  cameraGoal = camera;
  targetGoal = target;
  radiusGoal = radius;
}

void Viewport::stopEase() {
  if (easeTimer) easeTimer->stop();
  cameraGoal = camera;
  targetGoal = target;
  radiusGoal = radius;
}

void Viewport::easeStep() {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const double dt = easeLast ? std::clamp((now - easeLast) / 1000.0, 0.0, 0.1) : 1.0 / 120.0;
  easeLast = now;
  // About 90% of the way in 0.1 s: responsive, but steps and jitter are smoothed out.
  const float k = static_cast<float>(1.0 - std::exp(-dt / 0.045));
  camera = QQuaternion::slerp(camera, cameraGoal, k).normalized();
  target += (targetGoal - target) * k;
  radius = std::exp(std::log(radius) + (std::log(radiusGoal) - std::log(radius)) * k);
  const bool done = QQuaternion::dotProduct(camera, cameraGoal) > 0.9999999f && (targetGoal - target).length() < radius * 1e-5f &&
                    std::abs(radiusGoal - radius) < radius * 1e-5f;
  if (done) {
    camera = cameraGoal;
    target = targetGoal;
    radius = radiusGoal;
    easeTimer->stop();
    easeLast = 0;
  }
  update();
}

Viewport::~Viewport() {
  qApp->removeEventFilter(this);
  if (!glReady) return;
  makeCurrent();
  destroyGpu();
  if (gridVbo) glDeleteBuffers(1, &gridVbo);
  if (gridVao) glDeleteVertexArrays(1, &gridVao);
  if (gizmoVbo) glDeleteBuffers(1, &gizmoVbo);
  if (gizmoVao) glDeleteVertexArrays(1, &gizmoVao);
  if (meshProgram) glDeleteProgram(meshProgram);
  if (lineProgram) glDeleteProgram(lineProgram);
  if (gridProgram) glDeleteProgram(gridProgram);
  doneCurrent();
}

void Viewport::setItems(std::vector<DrawItem> next, double plane) {
  items = std::move(next);
  // Pick indexes: let go of meshes no longer shown; start the big new ones building.
  {
    std::unordered_map<const CpuMesh*, PickEntry> kept;
    for (const DrawItem& item : items) {
      if (!item.mesh || kept.count(item.mesh.get())) continue;
      auto old = picks.find(item.mesh.get());
      if (old != picks.end() && old->second.mesh == item.mesh) {
        kept.emplace(item.mesh.get(), std::move(old->second));
      } else if (item.mesh->triCount() > 20000) {
        PickEntry entry;
        entry.mesh = item.mesh;
        std::shared_ptr<const CpuMesh> mesh = item.mesh;
        entry.pending = std::async(std::launch::async, [mesh] { return std::shared_ptr<const PickIndex>(std::make_shared<PickIndex>(*mesh)); }).share();
        kept.emplace(item.mesh.get(), std::move(entry));
      }
    }
    picks = std::move(kept);
  }
  workplaneZ = plane;
  preview = false;
  dirty = true;
  applyFocusOn();
  update();
}

void Viewport::focusOn(const QStringList& ids) {
  focusIds = ids;
  applyFocusOn();
}

bool Viewport::applyFocusOn() {
  if (focusIds.isEmpty()) return false;
  QVector3D lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
  bool any = false;
  for (const DrawItem& item : items) {
    if (item.copyIndex != 0 || !focusIds.contains(item.id)) continue;
    QVector3D a, b;
    if (!itemAabb(item, &a, &b)) continue;
    lo = QVector3D(std::min(lo.x(), a.x()), std::min(lo.y(), a.y()), std::min(lo.z(), a.z()));
    hi = QVector3D(std::max(hi.x(), b.x()), std::max(hi.y(), b.y()), std::max(hi.z(), b.z()));
    any = true;
  }
  if (!any) return false;
  focusIds.clear();
  const QVector3D centre = clampFocus((lo + hi) * 0.5f);
  if (dragging) return true;  // not while a part is under the mouse
  beginNavigation();
  targetGoal = centre;
  if (!easeTimer->isActive()) easeTimer->start();
  return true;
}

void Viewport::setPreviewMesh(CpuMesh mesh) {
  previewMesh = std::move(mesh);
  preview = true;
  dirty = true;
  update();
}

void Viewport::setPreview(bool on) {
  preview = on;
  dirty = true;
  update();
}

void Viewport::setTool(int next) {
  tool = next;
  hoverAxis = -1;
  hoverEdge = -1;
  if (next != 8) measureHover = false;
  setCursor(next == 3 || next == 4 || next == 8 || next == 9 || next == 10 ? Qt::CrossCursor : Qt::ArrowCursor);
  update();
}

void Viewport::setBed(double width, double depth) {
  width = std::clamp(width, 20.0, 2000.0);
  depth = std::clamp(depth, 20.0, 2000.0);
  if (std::abs(width - bedW) < 0.01 && std::abs(depth - bedD) < 0.01) return;
  bedW = width;
  bedD = depth;
  gridDirty = true;
  update();
}

void Viewport::setSnap(bool enabled, double step) {
  snapOn = enabled;
  snap = step > 0 ? step : 1;
}

void Viewport::setGrid(bool on) {
  showGrid = on;
  update();
}

void Viewport::setFocusMarker(bool on) {
  showFocus = on;
  update();
}

void Viewport::setRotationSnap(double degrees) {
  rotationSnap = degrees > 0.0 ? std::clamp(degrees, 0.1, 180.0) : 0.0;
}

void Viewport::setFilletPick(std::vector<PickEdge> edges, std::vector<int> selected, bool all) {
  filletPick = std::move(edges);
  filletOn = std::move(selected);
  filletAll = all;
  update();
}

void Viewport::setAlignBoxes(std::vector<AlignBox> boxes, const QString& anchorId) {
  alignBoxes = std::move(boxes);
  alignAnchorId = anchorId;
  if (alignBoxes.size() < 2) {
    alignBoxes.clear();
    alignAnchorId.clear();
  }
  alignHover = -1;
  update();
}

// The whole selection's box, and the box the others line up with: the part that stays, or the
// whole selection when none was picked.
bool Viewport::alignFrame(QVector3D* unionLo, QVector3D* unionHi, QVector3D* refLo, QVector3D* refHi) const {
  if (alignBoxes.size() < 2) return false;
  QVector3D lo = alignBoxes.front().lo;
  QVector3D hi = alignBoxes.front().hi;
  for (const AlignBox& box : alignBoxes) {
    lo = QVector3D(std::min(lo.x(), box.lo.x()), std::min(lo.y(), box.lo.y()), std::min(lo.z(), box.lo.z()));
    hi = QVector3D(std::max(hi.x(), box.hi.x()), std::max(hi.y(), box.hi.y()), std::max(hi.z(), box.hi.z()));
  }
  *unionLo = lo;
  *unionHi = hi;
  *refLo = lo;
  *refHi = hi;
  for (const AlignBox& box : alignBoxes) {
    if (!alignAnchorId.isEmpty() && box.id == alignAnchorId) {
      *refLo = box.lo;
      *refHi = box.hi;
    }
  }
  return true;
}

// Dot index = axis * 3 + mode (0 low side, 1 centre, 2 high side). Each axis gets a row of dots just
// outside the selection, on the side facing the camera, at the coordinate it lines things up on.
bool Viewport::alignHandlePoint(int index, QVector3D* point) const {
  QVector3D ulo, uhi, rlo, rhi;
  if (!alignFrame(&ulo, &uhi, &rlo, &rhi)) return false;
  const int axisIndex = index / 3;
  const int mode = index % 3;
  const float size = (uhi - ulo).length();
  const float off = std::max(4.f, size * 0.08f);
  const QVector3D eye = eyePosition();
  const QVector3D centre = (ulo + uhi) * 0.5f;
  const float sideY = eye.y() < centre.y() ? ulo.y() - off : uhi.y() + off;
  const float sideX = eye.x() > centre.x() ? uhi.x() + off : ulo.x() - off;
  auto pick = [&](float a, float b) { return mode == 0 ? a : mode == 2 ? b : (a + b) * 0.5f; };
  if (axisIndex == 0) *point = QVector3D(pick(rlo.x(), rhi.x()), sideY, ulo.z());
  else if (axisIndex == 1) *point = QVector3D(sideX, pick(rlo.y(), rhi.y()), ulo.z());
  else *point = QVector3D(sideX, sideY, pick(rlo.z(), rhi.z()));
  return true;
}

int Viewport::alignHandleAt(const QPointF& pos) const {
  int best = -1;
  double bestDistance = 16.0;
  for (int i = 0; i < 9; ++i) {
    QVector3D p;
    if (!alignHandlePoint(i, &p)) return -1;
    const QPointF s = toScreen(p);
    const double d = std::hypot(s.x() - pos.x(), s.y() - pos.y());
    if (d < bestDistance) {
      bestDistance = d;
      best = i;
    }
  }
  return best;
}

// The dots themselves are drawn in GL (paintGL); this only adds the label of the hovered one.
void Viewport::paintAlign(QPainter& painter) const {
  if (alignBoxes.empty() || alignHover < 0) return;
  QVector3D p;
  if (!alignHandlePoint(alignHover, &p)) return;
  static const char* axisNames[3] = {"X", "Y", "Z"};
  static const char* modeNames[3] = {"low side", "centre", "high side"};
  const QString text = QStringLiteral("Line up %1 %2").arg(QLatin1String(axisNames[alignHover / 3]), QLatin1String(modeNames[alignHover % 3]));
  painter.save();
  QFont font = painter.font();
  font.setPointSize(9);
  font.setBold(true);
  painter.setFont(font);
  const QFontMetrics metrics(font);
  QRectF box = metrics.boundingRect(text).adjusted(-7, -4, 7, 4);
  box.moveTopLeft(toScreen(p) + QPointF(16, -30));
  painter.fillRect(box, QColor(24, 28, 34));
  painter.setPen(QColor(255, 224, 138));
  painter.drawText(box, Qt::AlignCenter, text);
  painter.restore();
}

int Viewport::pickFilletEdge(const QPointF& pos) const {
  const DrawItem* item = gizmoItem();
  if (!item || filletPick.empty()) return -1;
  int best = -1;
  double bestDist = 14.0;
  for (const PickEdge& edge : filletPick) {
    if (tool == 9 && !betaEdgeFacing(*item, edge)) continue;
    const QVector3D worldNudge = tool == 9 ? betaEdgeNudge(*item, edge) : QVector3D();
    const QVector3D nudge = tool == 9 ? QVector3D() : edgeOutward(edge) * 0.35f;
    const QPointF sa = toScreen(item->model.map(edge.a + nudge) + worldNudge);
    const QPointF sb = toScreen(item->model.map(edge.b + nudge) + worldNudge);
    const QPointF span = sb - sa;
    const double len2 = span.x() * span.x() + span.y() * span.y();
    double t = 0;
    if (len2 > 1e-6) {
      t = ((pos.x() - sa.x()) * span.x() + (pos.y() - sa.y()) * span.y()) / len2;
      t = std::clamp(t, 0.0, 1.0);
    }
    const QPointF closest(sa.x() + span.x() * t, sa.y() + span.y() * t);
    const double distance = std::hypot(pos.x() - closest.x(), pos.y() - closest.y());
    if (distance < bestDist) {
      bestDist = distance;
      best = edge.key;
    }
  }
  return best;
}

// Edge fillet (beta): an edge is shown when at least one of its faces looks toward the camera.
static QVector3D mapNormal(const QMatrix4x4& model, const QVector3D& n) {
  const QMatrix3x3 m = model.normalMatrix();
  return QVector3D(m(0, 0) * n.x() + m(0, 1) * n.y() + m(0, 2) * n.z(), m(1, 0) * n.x() + m(1, 1) * n.y() + m(1, 2) * n.z(),
                   m(2, 0) * n.x() + m(2, 1) * n.y() + m(2, 2) * n.z());
}

bool Viewport::betaEdgeFacing(const DrawItem& item, const PickEdge& edge) const {
  if (edge.nA.isNull() && edge.nB.isNull()) return true;
  const QVector3D toEye = eyePosition() - item.model.map((edge.a + edge.b) * 0.5f);
  return QVector3D::dotProduct(mapNormal(item.model, edge.nA), toEye) > 0 ||
         QVector3D::dotProduct(mapNormal(item.model, edge.nB), toEye) > 0;
}

// Lifted a little off the surface, outward between its two faces, so the rod is not buried.
QVector3D Viewport::betaEdgeNudge(const DrawItem& item, const PickEdge& edge) const {
  const QVector3D world = mapNormal(item.model, edge.nA.normalized()) + mapNormal(item.model, edge.nB.normalized());
  if (world.lengthSquared() < 1e-10f) return QVector3D();
  const QVector3D mid = item.model.map((edge.a + edge.b) * 0.5f);
  return world.normalized() * std::max(0.05f, gizmoLength(mid) * 0.006f);
}

void Viewport::rebuildGrid() {
  if (!glReady || !gridVao) return;
  std::vector<float> lines;
  auto push = [&](float x, float y) {
    lines.push_back(x);
    lines.push_back(y);
    lines.push_back(0);
  };
  const float hx = static_cast<float>(bedW * 0.5);
  const float hy = static_cast<float>(bedD * 0.5);
  const float fine = (bedW > 420 || bedD > 420) ? 5.f : 1.f;
  const float majorStep = fine * 10.f;
  for (float x = -hx; x <= hx + fine * 0.25f; x += fine) {
    if (std::fmod(std::abs(x + hx) + fine * 0.2f, majorStep) < fine * 0.6f) continue;
    push(std::min(x, hx), -hy);
    push(std::min(x, hx), hy);
  }
  gridMinorX = static_cast<GLsizei>(lines.size() / 3);
  for (float y = -hy; y <= hy + fine * 0.25f; y += fine) {
    if (std::fmod(std::abs(y + hy) + fine * 0.2f, majorStep) < fine * 0.6f) continue;
    push(-hx, std::min(y, hy));
    push(hx, std::min(y, hy));
  }
  gridMinor = static_cast<GLsizei>(lines.size() / 3);
  gridFine = fine;
  for (float x = -hx; x <= hx + majorStep * 0.25f; x += majorStep) {
    push(std::min(x, hx), -hy);
    push(std::min(x, hx), hy);
  }
  gridMajorX = static_cast<GLsizei>(lines.size() / 3) - gridMinor;
  for (float y = -hy; y <= hy + majorStep * 0.25f; y += majorStep) {
    push(-hx, std::min(y, hy));
    push(hx, std::min(y, hy));
  }
  gridMajor = static_cast<GLsizei>(lines.size() / 3) - gridMinor;
  auto tick = [&](float x, float y, float dx, float dy, float len) {
    push(x, y);
    push(x + dx * len, y + dy * len);
  };
  for (float x = -hx; x <= hx + majorStep * 0.25f; x += majorStep) {
    const float gx = std::min(x, hx);
    const bool big = std::fmod(std::abs(gx + hx) + 0.1f, majorStep * 5.f) < majorStep * 0.6f;
    tick(gx, -hy, 0, -1, big ? 8.f : 4.f);
    tick(gx, hy, 0, 1, big ? 8.f : 4.f);
  }
  for (float y = -hy; y <= hy + majorStep * 0.25f; y += majorStep) {
    const float gy = std::min(y, hy);
    const bool big = std::fmod(std::abs(gy + hy) + 0.1f, majorStep * 5.f) < majorStep * 0.6f;
    tick(-hx, gy, -1, 0, big ? 8.f : 4.f);
    tick(hx, gy, 1, 0, big ? 8.f : 4.f);
  }
  gridTicks = static_cast<GLsizei>(lines.size() / 3) - gridMinor - gridMajor;
  push(-hx, -hy);
  push(hx, -hy);
  push(hx, -hy);
  push(hx, hy);
  push(hx, hy);
  push(-hx, hy);
  push(-hx, hy);
  push(-hx, -hy);
  push(-8, 0);
  push(8, 0);
  push(0, -8);
  push(0, 8);
  gridBorder = static_cast<GLsizei>(lines.size() / 3) - gridMinor - gridMajor - gridTicks;
  gridCount = static_cast<GLsizei>(lines.size() / 3);
  glBindVertexArray(gridVao);
  glBindBuffer(GL_ARRAY_BUFFER, gridVbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(lines.size() * sizeof(float)), lines.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
  glBindVertexArray(0);
  gridDirty = false;
}

QVector3D Viewport::eyePosition() const {
  // Orthographic: far back along the same line (so everything in view is in front of it); how
  // much is seen comes from the radius, in projection().
  return target + camera.rotatedVector(QVector3D(0, 0, 1)) * (ortho ? 4000.f : radius);
}

float Viewport::gizmoLength(const QVector3D& pivot) const {
  const float dist = ortho ? radius : std::max(20.f, (eyePosition() - pivot).length());
  const float worldPerPx = 2.f * dist * std::tan(20.f * 3.14159265f / 180.f) / static_cast<float>(std::max(height(), 1));
  return std::max(12.f, worldPerPx * 120.f);
}

bool Viewport::compile() {
  meshProgram = linkProgram(this, kMeshVert, kMeshFrag);
  lineProgram = linkProgram(this, kLineVert, kLineFrag);
  gridProgram = linkProgram(this, kGridVert, kGridFrag);
  return meshProgram && lineProgram && gridProgram;
}

void Viewport::initializeGL() {
  initializeOpenGLFunctions();
  glReady = true;
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDisable(GL_CULL_FACE);
  compile();
  glGenVertexArrays(1, &gridVao);
  glGenBuffers(1, &gridVbo);
  rebuildGrid();

  glGenVertexArrays(1, &gizmoVao);
  glGenBuffers(1, &gizmoVbo);
  glBindVertexArray(gizmoVao);
  glBindBuffer(GL_ARRAY_BUFFER, gizmoVbo);
  glBufferData(GL_ARRAY_BUFFER, 6 * 6 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
  glBindVertexArray(0);
}

void Viewport::resizeGL(int w, int h) {
  glViewport(0, 0, std::max(w, 1), std::max(h, 1));
}

void Viewport::destroyGpu() {
  for (auto& entry : resident) {
    if (entry.second.vbo) glDeleteBuffers(1, &entry.second.vbo);
    if (entry.second.ebo) glDeleteBuffers(1, &entry.second.ebo);
    if (entry.second.vao) glDeleteVertexArrays(1, &entry.second.vao);
  }
  resident.clear();
  gpu.clear();
}

void Viewport::upload() {
  if (!glReady) return;
  std::vector<DrawItem> previewDraw;
  const std::vector<DrawItem>* draw = &items;
  if (preview) {
    DrawItem solid;
    solid.mesh = std::make_shared<CpuMesh>(previewMesh);
    solid.model.setToIdentity();
    solid.color = QColor("#e87400");
    previewDraw.push_back(std::move(solid));
    draw = &previewDraw;
  }
  std::unordered_map<const CpuMesh*, int> used;
  std::vector<Gpu> next;
  next.reserve(draw->size());
  for (const DrawItem& item : *draw) {
    if (!item.mesh || item.mesh->indices.empty() || item.mesh->vertices.empty()) continue;
    const CpuMesh* source = item.mesh.get();
    auto found = resident.find(source);
    if (found == resident.end()) {
      GpuBuffer buffer;
      buffer.count = static_cast<GLsizei>(item.mesh->indices.size());
      glGenVertexArrays(1, &buffer.vao);
      glGenBuffers(1, &buffer.vbo);
      glGenBuffers(1, &buffer.ebo);
      glBindVertexArray(buffer.vao);
      glBindBuffer(GL_ARRAY_BUFFER, buffer.vbo);
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(item.mesh->vertices.size() * sizeof(float)),
                   item.mesh->vertices.data(), GL_STATIC_DRAW);
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer.ebo);  // (part of the vertex array's state)
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(item.mesh->indices.size() * sizeof(uint32_t)),
                   item.mesh->indices.data(), GL_STATIC_DRAW);
      glBindVertexArray(0);
      found = resident.emplace(source, buffer).first;
    }
    used[source] += 1;
    Gpu instance;
    instance.vao = found->second.vao;
    instance.vbo = found->second.vbo;
    instance.count = found->second.count;
    instance.hole = item.hole && !preview;
    instance.selected = item.selected && !preview;
    instance.color = QVector3D(item.color.redF(), item.color.greenF(), item.color.blueF());
    instance.model = item.model;
    instance.id = item.id;
    instance.copy = item.copyIndex;
    next.push_back(std::move(instance));
  }
  for (auto it = resident.begin(); it != resident.end();) {
    if (used.find(it->first) == used.end()) {
      if (it->second.vbo) glDeleteBuffers(1, &it->second.vbo);
      if (it->second.ebo) glDeleteBuffers(1, &it->second.ebo);
      if (it->second.vao) glDeleteVertexArrays(1, &it->second.vao);
      it = resident.erase(it);
    } else {
      ++it;
    }
  }
  gpu = std::move(next);
  dirty = false;
}

QMatrix4x4 Viewport::projection() const {
  QMatrix4x4 proj;
  const float aspect = height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.f;
  if (ortho) {
    // As much as the perspective view shows at the focus point.
    const float half = radius * std::tan(20.f * 3.14159265f / 180.f);
    proj.ortho(-half * aspect, half * aspect, -half, half, 1.f, 9000.f);
  } else {
    proj.perspective(40.f, aspect, 0.1f, 4000.f);
  }
  return proj;
}

QVector3D Viewport::cameraUp() const {
  return camera.rotatedVector(QVector3D(0, 1, 0));
}

QVector3D Viewport::cameraRight() const {
  const QVector3D forward = target - eyePosition();
  QVector3D right = QVector3D::crossProduct(forward, cameraUp());
  if (right.lengthSquared() < 1e-8f) return camera.rotatedVector(QVector3D(1, 0, 0));
  return right.normalized();
}

void Viewport::setOrbitAngles(float azimuth, float polar) {
  const float sinPhi = std::sin(polar);
  QVector3D back(sinPhi * std::cos(azimuth), sinPhi * std::sin(azimuth), std::cos(polar));
  if (back.lengthSquared() < 1e-8f) back = QVector3D(0, 0, 1);
  back.normalize();
  QVector3D up(0, 0, 1);
  if (std::abs(QVector3D::dotProduct(back, up)) > 0.95f) up = QVector3D(0, 1, 0);
  QVector3D right = QVector3D::crossProduct(up, back);
  if (right.lengthSquared() < 1e-8f) right = QVector3D(1, 0, 0);
  right.normalize();
  up = QVector3D::crossProduct(back, right).normalized();
  QMatrix3x3 basis;
  basis(0, 0) = right.x();
  basis(1, 0) = right.y();
  basis(2, 0) = right.z();
  basis(0, 1) = up.x();
  basis(1, 1) = up.y();
  basis(2, 1) = up.z();
  basis(0, 2) = back.x();
  basis(1, 2) = back.y();
  basis(2, 2) = back.z();
  camera = QQuaternion::fromRotationMatrix(basis).normalized();
  stopEase();
}

QMatrix4x4 Viewport::viewMatrix() const {
  QMatrix4x4 view;
  view.lookAt(eyePosition(), target, cameraUp());
  return view;
}

namespace {
struct CubeFace {
  int c[4];
  const char* label;
  float theta;
  float phi;
};
const QVector3D kViewCorner[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                  {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
const CubeFace kViewFaces[6] = {
    {{1, 2, 6, 5}, "Right", 0.f, 1.5707963f},
    {{0, 4, 7, 3}, "Left", 3.14159265f, 1.5707963f},
    {{2, 3, 7, 6}, "Back", 1.5707963f, 1.5707963f},
    {{0, 1, 5, 4}, "Front", -1.5707963f, 1.5707963f},
    {{4, 5, 6, 7}, "Top", -1.5707963f, 0.02f},
    {{0, 3, 2, 1}, "Bottom", -1.5707963f, 3.12f},
};
struct ProjectedFace {
  QPointF p[4];
  QPointF mid;
  float depth = 0;
  bool facing = false;
  float towards = 0;  // how squarely it faces us, 0..1
};
void projectViewCube(const QMatrix4x4& view, const QPointF& origin, float scale, ProjectedFace out[6]) {
  QMatrix4x4 rot = view;
  rot.setColumn(3, QVector4D(0, 0, 0, 1));
  QVector3D corner[8];
  for (int i = 0; i < 8; ++i) corner[i] = rot.map(kViewCorner[i]);
  for (int face = 0; face < 6; ++face) {
    const QVector3D a = corner[kViewFaces[face].c[0]];
    const QVector3D b = corner[kViewFaces[face].c[1]];
    const QVector3D c = corner[kViewFaces[face].c[2]];
    const QVector3D d = corner[kViewFaces[face].c[3]];
    const QVector3D normal = QVector3D::crossProduct(b - a, c - b);
    out[face].facing = normal.z() > 0.08f;
    out[face].towards = normal.length() > 0 ? normal.z() / normal.length() : 0.f;
    out[face].depth = (a.z() + b.z() + c.z() + d.z()) * 0.25f;
    out[face].mid = QPointF();
    for (int k = 0; k < 4; ++k) {
      const QVector3D point = corner[kViewFaces[face].c[k]];
      out[face].p[k] = origin + QPointF(point.x(), -point.y()) * scale;
      out[face].mid += out[face].p[k];
    }
    out[face].mid /= 4.0;
  }
}
bool faceContains(const QPointF& pos, const ProjectedFace& face) {
  bool inside = false;
  for (int i = 0, j = 3; i < 4; j = i++) {
    const QPointF& a = face.p[i];
    const QPointF& b = face.p[j];
    const float dy = b.y() - a.y();
    if (((a.y() > pos.y()) != (b.y() > pos.y())) &&
        (pos.x() < (b.x() - a.x()) * (pos.y() - a.y()) / (std::abs(dy) < 1e-4f ? 1e-4f : dy) + a.x()))
      inside = !inside;
  }
  return inside;
}
}  // namespace

namespace {
// The view cube's panel: its middle, the cube's size, and the arrows round it (up, right, down,
// left) that turn the view to the face on that side.
const QPointF kCubeOrigin(68, 76);
constexpr float kCubeScale = 24.f;
constexpr float kArrowReach = 53.f;
const QPointF kArrowDir[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};

// Under the cube: Home (0) and Perspective / Orthographic (1).
QRectF cubeButton(int index) {
  const double top = kCubeOrigin.y() + 64 + 6, width = 61;
  return QRectF(kCubeOrigin.x() - 64 + index * (width + 6), top, width, 24);
}

QPolygonF cubeArrow(int index) {
  const QPointF d = kArrowDir[index];
  const QPointF side(-d.y(), d.x());
  const QPointF tip = kCubeOrigin + d * (kArrowReach + 5);
  const QPointF base = kCubeOrigin + d * (kArrowReach - 3);
  return QPolygonF{tip, base + side * 7, base - side * 7};
}
}  // namespace

int Viewport::viewCubeFace(const QPointF& pos) const {
  if (cubeButton(0).contains(pos)) return 10;
  if (cubeButton(1).contains(pos)) return 11;
  for (int i = 0; i < 4; ++i) {
    const QPointF centre = kCubeOrigin + kArrowDir[i] * (kArrowReach + 1);
    if (QLineF(centre, pos).length() < 9) return 6 + i;
  }
  ProjectedFace faces[6];
  projectViewCube(viewMatrix(), kCubeOrigin, kCubeScale, faces);
  int best = -1;
  float bestDepth = -1e9f;
  for (int i = 0; i < 6; ++i) {
    if (!faces[i].facing || faces[i].depth < bestDepth || !faceContains(pos, faces[i])) continue;
    best = i;
    bestDepth = faces[i].depth;
  }
  return best;
}

void Viewport::glideTo(float azimuth, float polar, const QVector3D& focus, float distance) {
  beginNavigation();
  const QQuaternion from = camera;
  setOrbitAngles(azimuth, polar);
  const QQuaternion to = camera;
  camera = from;
  cameraGoal = QQuaternion::dotProduct(from, to) < 0 ? -to : to;
  targetGoal = focus;
  radiusGoal = std::clamp(distance, 20.f, 1600.f);
  if (!easeTimer->isActive()) easeTimer->start();
  update();
}

void Viewport::goHome() {
  // From the front-right corner, half the plate's width above it, and far enough back that the
  // whole plate is in view.
  const float width = static_cast<float>(std::max(bedW, bedD));
  const float rise = width * 0.5f, back = width * 1.5f;
  glideTo(-0.78539816f, std::atan2(back, rise), QVector3D(0, 0, static_cast<float>(workplaneZ)), std::hypot(back, rise));
}

void Viewport::isometricView() {
  setOrthographic(true);
  const float width = static_cast<float>(std::max(bedW, bedD));
  // Looking down 35.26° (atan(1/sqrt 2)): the three axes equally foreshortened.
  glideTo(-0.78539816f, 0.95531662f, QVector3D(0, 0, static_cast<float>(workplaneZ)), width * 1.5f);
}

void Viewport::setOrthographic(bool on) {
  if (ortho == on) return;
  ortho = on;
  emit projectionChanged(on);
  update();
}

void Viewport::lookFace(int face) {
  if (face == 10) {
    goHome();
    return;
  }
  if (face == 11) {
    setOrthographic(!ortho);
    return;
  }
  const bool arrow = face >= 6 && face < 10;
  if (arrow) face = neighbourFace(face - 6);
  if (face < 0 || face > 5) return;
  // Glide round to the face, rather than jump.
  beginNavigation();
  const QQuaternion from = camera;
  const QVector3D goalTarget = targetGoal;
  const float goalRadius = radiusGoal;
  setOrbitAngles(kViewFaces[face].theta, kViewFaces[face].phi);
  const QQuaternion to = camera;
  camera = from;
  cameraGoal = QQuaternion::dotProduct(from, to) < 0 ? -to : to;
  targetGoal = goalTarget;
  radiusGoal = goalRadius;
  // A face of the cube: back to the middle of the plate, a little above it. An arrow keeps the
  // focus where it is, but never at or below the plate.
  const float floor = static_cast<float>(workplaneZ);
  if (!arrow) targetGoal = QVector3D(0, 0, floor + 10);
  else if (targetGoal.z() < floor + 1) targetGoal.setZ(floor + 10);
  if (!easeTimer->isActive()) easeTimer->start();
  update();
}

// The face beside the one most facing us, on that side of the screen (0 up, 1 right, 2 down,
// 3 left).
int Viewport::neighbourFace(int arrow) const {
  ProjectedFace faces[6];
  projectViewCube(viewMatrix(), kCubeOrigin, kCubeScale, faces);
  int front = 0;
  for (int i = 1; i < 6; ++i)
    if (faces[i].depth > faces[front].depth) front = i;
  // Faces are listed in opposite pairs (Right/Left, Back/Front, Top/Bottom).
  const int opposite = front ^ 1;
  int best = -1;
  double bestAlong = -1e9;
  for (int i = 0; i < 6; ++i) {
    if (i == front || i == opposite) continue;
    const QPointF away = faces[i].mid - kCubeOrigin;
    const double along = away.x() * kArrowDir[arrow].x() + away.y() * kArrowDir[arrow].y();
    if (along > bestAlong) {
      bestAlong = along;
      best = i;
    }
  }
  return best;
}

void Viewport::setMeasureReadout(bool haveA, const QVector3D& a, bool haveB, const QVector3D& b, bool haveSize, const QVector3D& size) {
  measureHaveA = haveA;
  measureHaveB = haveB;
  measureHaveSize = haveSize;
  measureA = a;
  measureB = b;
  measureSize = size;
  update();
}

bool Viewport::measurePointAt(const QPointF& pos, QVector3D* hit) const {
  if (!hit) return false;
  QVector3D origin;
  QVector3D dir;
  cameraRay(pos, &origin, &dir);
  float best = 1e9f;
  bool found = false;
  bool onMesh = false;
  QVector3D corners[3];
  QVector3D point;
  auto rayMisses = [](const QVector3D& origin, const QVector3D& dir, const QVector3D& lo, const QVector3D& hi, float maxT) {
    float tMin = 0;
    float tMax = maxT;
    const float o[3] = {origin.x(), origin.y(), origin.z()};
    const float d[3] = {dir.x(), dir.y(), dir.z()};
    const float b0[3] = {lo.x(), lo.y(), lo.z()};
    const float b1[3] = {hi.x(), hi.y(), hi.z()};
    for (int axis = 0; axis < 3; ++axis) {
      if (std::abs(d[axis]) < 1e-8f) {
        if (o[axis] < b0[axis] || o[axis] > b1[axis]) return true;
        continue;
      }
      float t0 = (b0[axis] - o[axis]) / d[axis];
      float t1 = (b1[axis] - o[axis]) / d[axis];
      if (t0 > t1) std::swap(t0, t1);
      tMin = std::max(tMin, t0);
      tMax = std::min(tMax, t1);
      if (tMin > tMax) return true;
    }
    return false;
  };
  for (const DrawItem& item : items) {
    if (item.gizmo || meshOf(item.mesh).indices.empty()) continue;
    const CpuMesh& mesh = meshOf(item.mesh);
    if (mesh.hasBounds) {
      QVector3D boxLo(1e9f, 1e9f, 1e9f);
      QVector3D boxHi(-1e9f, -1e9f, -1e9f);
      for (int corner = 0; corner < 8; ++corner) {
        const QVector3D local((corner & 1) ? mesh.hi[0] : mesh.lo[0], (corner & 2) ? mesh.hi[1] : mesh.lo[1],
                              (corner & 4) ? mesh.hi[2] : mesh.lo[2]);
        const QVector3D world = item.model.map(local);
        boxLo = QVector3D(std::min(boxLo.x(), world.x()), std::min(boxLo.y(), world.y()), std::min(boxLo.z(), world.z()));
        boxHi = QVector3D(std::max(boxHi.x(), world.x()), std::max(boxHi.y(), world.y()), std::max(boxHi.z(), world.z()));
      }
      if (rayMisses(origin, dir, boxLo, boxHi, best)) continue;
    }
    float dist = 0;
    int tri = -1;
    if (!hitItem(item, origin, dir, best, &dist, &tri) || dist < 1e-3f) continue;
    auto world = [&](int k) {
      const float* a = mesh.corner(static_cast<size_t>(tri), k);
      return item.model.map(QVector3D(a[0], a[1], a[2]));
    };
    best = dist;
    found = true;
    onMesh = true;
    point = origin + dir * dist;
    corners[0] = world(0);
    corners[1] = world(1);
    corners[2] = world(2);
  }
  if (std::abs(dir.z()) > 1e-5f) {
    const float t = (static_cast<float>(workplaneZ) - origin.z()) / dir.z();
    if (t > 1e-3f && t < best) {
      found = true;
      onMesh = false;
      point = origin + dir * t;
    }
  }
  if (!found) return false;
  if (onMesh) {
    auto pixels = [&](const QVector3D& world) {
      const QPointF screen = toScreen(world);
      return std::hypot(screen.x() - pos.x(), screen.y() - pos.y());
    };
    int bestCorner = -1;
    double bestPx = 14.0;
    for (int i = 0; i < 3; ++i) {
      const double distance = pixels(corners[i]);
      if (distance < bestPx) {
        bestPx = distance;
        bestCorner = i;
      }
    }
    if (bestCorner >= 0) {
      *hit = corners[bestCorner];
      return true;
    }
    const QVector3D mid[3] = {(corners[0] + corners[1]) * 0.5f, (corners[1] + corners[2]) * 0.5f,
                              (corners[2] + corners[0]) * 0.5f};
    int bestMid = -1;
    bestPx = 12.0;
    for (int i = 0; i < 3; ++i) {
      const double distance = pixels(mid[i]);
      if (distance < bestPx) {
        bestPx = distance;
        bestMid = i;
      }
    }
    *hit = bestMid >= 0 ? mid[bestMid] : point;
    return true;
  }
  if (snapOn && snap > 1e-6) {
    point.setX(static_cast<float>(std::round(static_cast<double>(point.x()) / snap) * snap));
    point.setY(static_cast<float>(std::round(static_cast<double>(point.y()) / snap) * snap));
  }
  point.setZ(static_cast<float>(workplaneZ));
  *hit = point;
  return true;
}

void Viewport::paintMeasure(QPainter& painter) const {
  if (tool != 8) return;
  auto dot = [&](const QVector3D& world, const QColor& fill, int radius) {
    painter.setPen(QPen(QColor(20, 16, 12), 1));
    painter.setBrush(fill);
    painter.drawEllipse(toScreen(world), radius, radius);
  };
  if (measureHaveA) dot(measureA, QColor(232, 116, 0), 5);
  if (measureHaveB) dot(measureB, QColor(232, 116, 0), 5);
  if (measureHover) dot(measureHoverPoint, QColor(232, 116, 0, 90), 4);
  painter.setBrush(Qt::NoBrush);
  if (measureHaveA && measureHaveB) {
    painter.setPen(QPen(QColor(232, 116, 0), 1.6));
    painter.drawLine(toScreen(measureA), toScreen(measureB));
  } else if (measureHaveA && measureHover) {
    painter.setPen(QPen(QColor(232, 116, 0, 190), 1.4, Qt::DashLine));
    painter.drawLine(toScreen(measureA), toScreen(measureHoverPoint));
  }

  QStringList rows;
  if (measureHaveSize)
    rows << QStringLiteral("Selection   X %1    Y %2    Z %3 mm")
                .arg(measureSize.x(), 0, 'f', 2)
                .arg(measureSize.y(), 0, 'f', 2)
                .arg(measureSize.z(), 0, 'f', 2);
  if (measureHaveA && measureHaveB) {
    const QVector3D delta = measureB - measureA;
    rows << QStringLiteral("A to B   %1 mm").arg(static_cast<double>(delta.length()), 0, 'f', 2);
    rows << QStringLiteral("ΔX %1    ΔY %2    ΔZ %3")
                .arg(delta.x(), 0, 'f', 2)
                .arg(delta.y(), 0, 'f', 2)
                .arg(delta.z(), 0, 'f', 2);
  } else if (measureHaveA && measureHover) {
    const QVector3D delta = measureHoverPoint - measureA;
    rows << QStringLiteral("A to cursor   %1 mm").arg(static_cast<double>(delta.length()), 0, 'f', 2);
  } else if (measureHaveA) {
    rows << QStringLiteral("Point A   %1, %2, %3")
                .arg(measureA.x(), 0, 'f', 2)
                .arg(measureA.y(), 0, 'f', 2)
                .arg(measureA.z(), 0, 'f', 2);
  } else if (!measureHaveSize) {
    rows << QStringLiteral("Click a point. Vertices and edge centres snap.");
  }
  if (rows.isEmpty()) return;
  QFont font = painter.font();
  font.setPixelSize(13);
  painter.setFont(font);
  const int lineH = 18;
  const int pad = 8;
  int textWidth = 280;
  for (const QString& row : rows) textWidth = std::max(textWidth, painter.fontMetrics().horizontalAdvance(row));
  const QRect box(12, height() - 16 - pad * 2 - lineH * rows.size(), textWidth + pad * 2, pad * 2 + lineH * rows.size());
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(24, 28, 34, 220));
  painter.drawRoundedRect(box, 6, 6);
  painter.setPen(QColor(245, 239, 229));
  for (int i = 0; i < rows.size(); ++i)
    painter.drawText(QRect(box.left() + pad, box.top() + pad + i * lineH, textWidth, lineH), Qt::AlignLeft | Qt::AlignVCenter, rows[i]);
}

void Viewport::paintFocusMarker(QPainter& painter) const {
  if (!showFocus) return;
  const QVector3D at = focusDragging ? pendingFocus : target;
  const QPointF screen = toScreen(at);
  const QPointF bed = toScreen(QVector3D(at.x(), at.y(), static_cast<float>(workplaneZ)));
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  if ((screen - bed).manhattanLength() > 6) {
    painter.setPen(QPen(QColor(232, 116, 0, 150), 1, Qt::DashLine));
    painter.drawLine(bed, screen);
  }
  painter.setPen(QPen(QColor("#e87400"), 1.6));
  painter.setBrush(QColor(232, 116, 0, 36));
  painter.drawEllipse(screen, 10, 10);
  painter.drawLine(screen + QPointF(-16, 0), screen + QPointF(16, 0));
  painter.drawLine(screen + QPointF(0, -16), screen + QPointF(0, 16));
  painter.setBrush(QColor("#e87400"));
  painter.drawEllipse(bed, 3.5, 3.5);
  painter.restore();
}

bool Viewport::focusHit(const QPointF& pos) const {
  if (!showFocus) return false;
  const QPointF screen = toScreen(target);
  const double dx = pos.x() - screen.x();
  const double dy = pos.y() - screen.y();
  return dx * dx + dy * dy <= 16.0 * 16.0;
}

// Click on the bed with Focus on: the view glides to centre on that spot.
void Viewport::moveFocusTo(const QPointF& pos) {
  QVector3D hit;
  if (!focusOnBed(pos, &hit)) return;
  beginNavigation();
  targetGoal = hit;
  if (!easeTimer->isActive()) easeTimer->start();
}

void Viewport::paintViewCube(QPainter& painter) const {
  const QPointF origin = kCubeOrigin;
  ProjectedFace faces[6];
  projectViewCube(viewMatrix(), origin, kCubeScale, faces);
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(18, 21, 26, 170));
  painter.drawRoundedRect(QRectF(origin.x() - 64, origin.y() - 64, 128, 128), 9, 9);
  for (int i = 0; i < 4; ++i) {
    const bool hot = hoverView == 6 + i;
    painter.setPen(QPen(QColor("#e87400"), 1.0));
    painter.setBrush(hot ? QColor(232, 116, 0) : QColor(37, 44, 54));
    painter.drawPolygon(cubeArrow(i));
  }
  // Home and the projection, under the cube.
  for (int i = 0; i < 2; ++i) {
    const QRectF box = cubeButton(i);
    const bool hot = hoverView == 10 + i;
    // A plain rounded button, like the rest of the app's: one border, set on the half pixel so
    // it stays a crisp single line.
    painter.setPen(QPen(QColor("#e87400"), 1.0));
    painter.setBrush(hot ? QColor(232, 116, 0) : QColor(37, 44, 54));
    painter.drawRoundedRect(QRectF(std::round(box.x()) + 0.5, std::round(box.y()) + 0.5, std::round(box.width()) - 1,
                                   std::round(box.height()) - 1),
                            4, 4);
    const QColor ink = hot ? QColor(24, 18, 10) : QColor(236, 230, 218);
    if (i == 0) {
      // A little house.
      const QPointF c = box.center() + QPointF(-14, 0);
      painter.setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.setBrush(Qt::NoBrush);
      painter.drawPolyline(QPolygonF{c + QPointF(-6, -0.5), c + QPointF(0, -6), c + QPointF(6, -0.5)});
      painter.drawPolyline(QPolygonF{c + QPointF(-4, -2), c + QPointF(-4, 5), c + QPointF(4, 5), c + QPointF(4, -2)});
      painter.setFont(QFont(QStringLiteral("Segoe UI"), 7, QFont::DemiBold));
      painter.setPen(ink);
      painter.drawText(box.adjusted(18, 0, 0, 0), Qt::AlignCenter, QStringLiteral("Home"));
    } else {
      painter.setFont(QFont(QStringLiteral("Segoe UI"), 7, QFont::DemiBold));
      painter.setPen(ink);
      painter.drawText(box, Qt::AlignCenter, ortho ? QStringLiteral("Ortho") : QStringLiteral("Persp"));
    }
  }
  int order[6] = {0, 1, 2, 3, 4, 5};
  std::sort(order, order + 6, [&](int a, int b) { return faces[a].depth < faces[b].depth; });
  painter.setFont(QFont(QStringLiteral("Segoe UI"), 7, QFont::DemiBold));
  for (int slot = 0; slot < 6; ++slot) {
    const int index = order[slot];
    if (!faces[index].facing) continue;
    const bool hot = index == hoverView;
    painter.setPen(QPen(QColor("#e87400"), hot ? 1.6 : 1.0));
    painter.setBrush(hot ? QColor(232, 116, 0) : QColor(37, 44, 54));
    painter.drawPolygon(faces[index].p, 4);
    if (faces[index].towards < 0.3f) continue;  // seen almost edge-on: no room for its name
    painter.setPen(hot ? QColor(24, 18, 10) : QColor(236, 230, 218));
    const QFontMetrics metrics(painter.font());
    const QString label = QString::fromLatin1(kViewFaces[index].label);
    const QSize text = metrics.size(Qt::TextSingleLine, label);
    painter.drawText(QRectF(faces[index].mid.x() - text.width() * 0.5, faces[index].mid.y() - text.height() * 0.5,
                            text.width(), text.height()),
                     Qt::AlignCenter, label);
  }
  painter.restore();
}

void Viewport::cameraRay(const QPointF& pos, QVector3D* origin, QVector3D* direction) const {
  const QMatrix4x4 inv = (projection() * viewMatrix()).inverted();
  const float x = static_cast<float>((2.0 * pos.x() / std::max(width(), 1)) - 1.0);
  const float y = static_cast<float>(1.0 - (2.0 * pos.y() / std::max(height(), 1)));
  auto unproject = [&](float z) {
    QVector4D clip(x, y, z, 1.f);
    QVector4D world = inv * clip;
    if (std::abs(world.w()) < 1e-6f) return QVector3D();
    return world.toVector3D() / world.w();
  };
  *origin = unproject(-1.f);
  *direction = (unproject(1.f) - *origin).normalized();
}

QPointF Viewport::toScreen(const QVector3D& world) const {
  const QVector4D clip = projection() * viewMatrix() * QVector4D(world, 1.f);
  if (std::abs(clip.w()) < 1e-6f) return {};
  const QVector3D ndc = clip.toVector3D() / clip.w();
  return QPointF((ndc.x() * 0.5 + 0.5) * width(), (1.0 - (ndc.y() * 0.5 + 0.5)) * height());
}

const DrawItem* Viewport::gizmoItem() const {
  if (preview) return nullptr;
  for (const DrawItem& item : items) {
    if (item.gizmo) return &item;
  }
  return nullptr;
}

int Viewport::gizmoAxis(const QPointF& pos) const {
  const DrawItem* item = gizmoItem();
  if (!item || tool == 3 || tool == 9) return -1;
  const QVector3D pivot(item->model(0, 3), item->model(1, 3), item->model(2, 3));
  const float len = gizmoLength(pivot);
  int best = -1;
  float bestDist = 16.f;
  for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
    QVector3D dir;
    QVector3D u;
    QVector3D v;
    axisBasis(item->model, pivot, axisIndex, &dir, &u, &v);
    if (tool == 1) {
      const float ring = len * 0.72f;
      const float sweep = 5.6f;
      const float start = -0.5f;
      for (int sample = 0; sample <= 28; ++sample) {
        const float a = start + sweep * (static_cast<float>(sample) / 28.f);
        const QPointF screen = toScreen(pivot + u * std::cos(a) * ring + v * std::sin(a) * ring);
        const float dx = static_cast<float>(pos.x() - screen.x());
        const float dy = static_cast<float>(pos.y() - screen.y());
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist < bestDist) {
          bestDist = dist;
          best = axisIndex;
        }
      }
    } else {
      const QPointF origin = toScreen(pivot);
      const QPointF tip = toScreen(pivot + dir * len);
      const QPointF ab = tip - origin;
      const float len2 = ab.x() * ab.x() + ab.y() * ab.y();
      if (len2 < 1.f) continue;
      float t = ((pos.x() - origin.x()) * ab.x() + (pos.y() - origin.y()) * ab.y()) / len2;
      t = std::clamp(t, 0.f, 1.f);
      const QPointF closest = origin + ab * t;
      const float dx = static_cast<float>(pos.x() - closest.x());
      const float dy = static_cast<float>(pos.y() - closest.y());
      const float dist = std::sqrt(dx * dx + dy * dy);
      if (dist < bestDist) {
        bestDist = dist;
        best = axisIndex;
      }
    }
  }
  return best;
}

const PickIndex* Viewport::pickIndexFor(const std::shared_ptr<const CpuMesh>& mesh, bool wait) const {
  if (!mesh || mesh->indices.empty()) return nullptr;
  auto found = picks.find(mesh.get());
  if (found == picks.end() || found->second.mesh != mesh) {
    PickEntry entry;
    entry.mesh = mesh;
    if (mesh->triCount() > 20000 && !wait) {
      std::shared_ptr<const CpuMesh> keep = mesh;
      entry.pending = std::async(std::launch::async, [keep] { return std::shared_ptr<const PickIndex>(std::make_shared<PickIndex>(*keep)); }).share();
    } else {
      entry.index = std::make_shared<PickIndex>(*mesh);
    }
    found = picks.insert_or_assign(mesh.get(), std::move(entry)).first;
  }
  PickEntry& entry = found->second;
  if (!entry.index && entry.pending.valid() &&
      (wait || entry.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready))
    entry.index = entry.pending.get();
  return entry.index.get();
}

bool Viewport::hitItem(const DrawItem& item, const QVector3D& origin, const QVector3D& dir, float maxT, float* dist,
                       int* triangle) const {
  if (!item.mesh || item.mesh->indices.empty()) return false;
  const CpuMesh& mesh = *item.mesh;
  // The ray in the mesh's own frame (t stays the same: the map is affine).
  bool invertible = false;
  const QMatrix4x4 toLocal = item.model.inverted(&invertible);
  if (!invertible) return false;
  const QVector3D o = toLocal.map(origin), d = toLocal.mapVector(dir);
  const float lo[3] = {o.x(), o.y(), o.z()}, ld[3] = {d.x(), d.y(), d.z()};
  if (mesh.hasBounds) {
    // Quick miss: the ray against the mesh's box.
    float t0 = 0.f, t1 = maxT;
    for (int a = 0; a < 3; ++a) {
      if (std::abs(ld[a]) < 1e-20f) {
        if (lo[a] < mesh.lo[a] || lo[a] > mesh.hi[a]) return false;
        continue;
      }
      float n = (mesh.lo[a] - lo[a]) / ld[a], f = (mesh.hi[a] - lo[a]) / ld[a];
      if (n > f) std::swap(n, f);
      t0 = std::max(t0, n);
      t1 = std::min(t1, f);
      if (t0 > t1) return false;
    }
  }
  if (const PickIndex* index = pickIndexFor(item.mesh)) return index->cast(lo, ld, maxT, dist, triangle);
  // Still being indexed: every triangle, in its own frame.
  float best = maxT;
  int hit = -1;
  for (size_t t = 0; t < mesh.triCount(); ++t) {
    const float* a = mesh.corner(t, 0);
    const float* b = mesh.corner(t, 1);
    const float* c = mesh.corner(t, 2);
    float along = 0;
    if (hitTriangle(o, d, QVector3D(a[0], a[1], a[2]), QVector3D(b[0], b[1], b[2]), QVector3D(c[0], c[1], c[2]), &along) &&
        along < best) {
      best = along;
      hit = static_cast<int>(t);
    }
  }
  if (hit < 0) return false;
  *dist = best;
  *triangle = hit;
  return true;
}

QString Viewport::pickId(const QPointF& pos) const {
  QVector3D origin;
  QVector3D dir;
  cameraRay(pos, &origin, &dir);
  float best = 1e9f;
  QString id;
  for (const DrawItem& item : items) {
    float dist = 0;
    int tri = -1;
    if (hitItem(item, origin, dir, best, &dist, &tri)) {
      best = dist;
      id = item.id;
    }
  }
  return id;
}

bool Viewport::solidHit(const QPointF& pos, QVector3D* hit) const {
  QVector3D origin;
  QVector3D dir;
  cameraRay(pos, &origin, &dir);
  float best = 1e9f;
  for (const DrawItem& item : items) {
    if (item.hole) continue;
    float dist = 0;
    int tri = -1;
    if (hitItem(item, origin, dir, best, &dist, &tri) && dist > 0) best = dist;
  }
  if (best >= 1e9f) return false;
  *hit = origin + dir * best;
  return true;
}

bool Viewport::pickFace(const QPointF& pos, QString* id, QVector<QVector3D>* loop, QVector3D* localNormal,
                        QVector3D* localAxis, bool smooth, QVector<QVector3D>* pointNormals, QMatrix4x4* modelOut) const {
  QVector3D origin;
  QVector3D dir;
  cameraRay(pos, &origin, &dir);
  float best = 1e9f;
  bool found = false;
  const DrawItem* hitItem = nullptr;
  int hitTri = -1;
  QVector3D hitNormal;
  QVector3D hitAxis;
  for (const DrawItem& item : items) {
    if (item.hole) continue;
    float dist = 0;
    int t = -1;
    if (!this->hitItem(item, origin, dir, best, &dist, &t)) continue;
    const CpuMesh& m = meshOf(item.mesh);
    const float* a = m.corner(static_cast<size_t>(t), 0);
    const float* b = m.corner(static_cast<size_t>(t), 1);
    const float* c = m.corner(static_cast<size_t>(t), 2);
    const QVector3D lp0(a[0], a[1], a[2]), lp1(b[0], b[1], b[2]), lp2(c[0], c[1], c[2]);
    QVector3D n = QVector3D::crossProduct(lp1 - lp0, lp2 - lp0);
    if (n.lengthSquared() < 1e-12f) continue;
    n.normalize();
    QVector3D hint(0, 0, 1);
    if (std::abs(QVector3D::dotProduct(n, hint)) > 0.85f) hint = QVector3D(1, 0, 0);
    QVector3D axis = QVector3D::crossProduct(hint, n);
    if (axis.lengthSquared() < 1e-8f) axis = QVector3D(1, 0, 0);
    axis.normalize();
    best = dist;
    found = true;
    hitItem = &item;
    hitTri = t;
    hitNormal = n;
    hitAxis = axis;
  }
  if (!found || !hitItem) return false;
  const CpuMesh& hitMesh = meshOf(hitItem->mesh);
  const PickIndex* index = pickIndexFor(hitItem->mesh, true);
  if (!index) return false;
  const int tris = static_cast<int>(hitMesh.triCount());
  auto vertex = [&](int tri, int corner) {
    const float* p = hitMesh.corner(static_cast<size_t>(tri), corner);
    return QVector3D(p[0], p[1], p[2]);
  };
  auto normalAt = [&](int tri) {
    const QVector3D p0 = vertex(tri, 0);
    const QVector3D n = QVector3D::crossProduct(vertex(tri, 1) - p0, vertex(tri, 2) - p0);
    if (n.lengthSquared() < 1e-12f) return QVector3D();
    return n.normalized();
  };
  const QVector3D hitPoint = vertex(hitTri, 0);
  auto onHitPlane = [&](const QVector3D& point) {
    return std::abs(QVector3D::dotProduct(point - hitPoint, hitNormal)) <= 0.4f;
  };
  std::vector<char> seen(static_cast<size_t>(std::max(tris, 0)), 0);
  struct Seed {
    int tri = 0;
    QVector3D normal;
  };
  std::vector<Seed> stack{{hitTri, hitNormal}};
  seen[static_cast<size_t>(hitTri)] = 1;
  std::vector<int> faceTris;
  faceTris.reserve(64);
  std::vector<int> next;
  QVector3D islandLo = hitPoint, islandHi = hitPoint;
  while (!stack.empty() && faceTris.size() < 8000) {
    const Seed cur = stack.back();
    stack.pop_back();
    faceTris.push_back(cur.tri);
    for (int c = 0; c < 3; ++c) {
      const QVector3D p = vertex(cur.tri, c);
      islandLo = QVector3D(std::min(islandLo.x(), p.x()), std::min(islandLo.y(), p.y()), std::min(islandLo.z(), p.z()));
      islandHi = QVector3D(std::max(islandHi.x(), p.x()), std::max(islandHi.y(), p.y()), std::max(islandHi.z(), p.z()));
    }
    // Across each shared edge.
    index->neighbours(cur.tri, &next);
    for (int other : next) {
      if (seen[static_cast<size_t>(other)]) continue;
      const QVector3D n = normalAt(other);
      if (n.lengthSquared() < 0.5f) continue;
      if (!smooth) {
        // Stay on the flat face. A fillet facet is about 7 degrees at 12 segments,
        // and 0.985 was loose enough to walk onto that round and lose the outline.
        if (QVector3D::dotProduct(n, hitNormal) < 0.998f) continue;
      } else {
        // Follow a gentle bend (about 3 degrees a step) and a flat face.
        // Stop at a fillet, which turns about 11 degrees a facet.
        const float along = QVector3D::dotProduct(n, cur.normal);
        const float flat = QVector3D::dotProduct(n, hitNormal);
        if (along < 0.994f && flat < 0.985f) continue;
      }
      seen[static_cast<size_t>(other)] = 1;
      stack.push_back(Seed{other, n});
    }
  }
  // A same-height union leaves a hairline step or a T-junction, so the strict
  // shared-edge walk only sees one island. Pull in triangles that sit on this
  // plane and nearly touch the island (looked for only around the island).
  std::vector<int> coplanar;
  coplanar.reserve(64);
  {
    const float boxLo[3] = {islandLo.x() - 0.5f, islandLo.y() - 0.5f, islandLo.z() - 0.5f};
    const float boxHi[3] = {islandHi.x() + 0.5f, islandHi.y() + 0.5f, islandHi.z() + 0.5f};
    std::vector<int> near;
    index->inBox(boxLo, boxHi, &near);
    for (int other : near) {
      if (seen[static_cast<size_t>(other)]) continue;
      const QVector3D n = normalAt(other);
      if (n.lengthSquared() < 0.5f || QVector3D::dotProduct(n, hitNormal) < 0.998f) continue;
      if (!onHitPlane(vertex(other, 0)) || !onHitPlane(vertex(other, 1)) || !onHitPlane(vertex(other, 2))) continue;
      coplanar.push_back(other);
    }
  }
  bool grew = !coplanar.empty();
  int growGuard = 0;
  while (grew && growGuard++ < 16 && faceTris.size() < 8000) {
    grew = false;
    const int known = static_cast<int>(faceTris.size());
    for (int other : coplanar) {
      if (seen[static_cast<size_t>(other)] || faceTris.size() >= 8000) continue;
      const QVector3D q[3] = {vertex(other, 0), vertex(other, 1), vertex(other, 2)};
      bool touch = false;
      for (int k = 0; k < known && !touch; ++k) {
        const int tri = faceTris[static_cast<size_t>(k)];
        const QVector3D c[3] = {vertex(tri, 0), vertex(tri, 1), vertex(tri, 2)};
        for (int i = 0; i < 3 && !touch; ++i) {
          for (int j = 0; j < 3; ++j) {
            if ((q[i] - c[j]).lengthSquared() <= 0.45f * 0.45f) {
              touch = true;
              break;
            }
          }
          if (touch) break;
          for (int e = 0; e < 3; ++e) {
            const QVector3D ab = c[(e + 1) % 3] - c[e];
            const float ab2 = ab.lengthSquared();
            if (ab2 < 1e-8f) continue;
            float t = QVector3D::dotProduct(q[i] - c[e], ab) / ab2;
            if (t < 0.f) t = 0.f;
            if (t > 1.f) t = 1.f;
            if ((q[i] - (c[e] + ab * t)).lengthSquared() <= 0.45f * 0.45f) {
              touch = true;
              break;
            }
          }
        }
      }
      if (!touch) continue;
      seen[static_cast<size_t>(other)] = 1;
      faceTris.push_back(other);
      grew = true;
    }
  }
  if (faceTris.empty()) return false;

  std::vector<QVector3D> weldPts;
  std::vector<QVector3D> weldNormal;
  std::map<std::tuple<int, int, int>, std::vector<int>> buckets;
  auto weld = [&](const QVector3D& point) {
    const int cx = static_cast<int>(std::lround(point.x() / 0.05f));
    const int cy = static_cast<int>(std::lround(point.y() / 0.05f));
    const int cz = static_cast<int>(std::lround(point.z() / 0.05f));
    for (int dz = -1; dz <= 1; ++dz) {
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const auto found = buckets.find(std::make_tuple(cx + dx, cy + dy, cz + dz));
          if (found == buckets.end()) continue;
          for (int index : found->second) {
            if ((weldPts[static_cast<size_t>(index)] - point).lengthSquared() < 0.05f * 0.05f) return index;
          }
        }
      }
    }
    const int index = static_cast<int>(weldPts.size());
    weldPts.push_back(point);
    weldNormal.push_back(QVector3D());
    buckets[std::make_tuple(cx, cy, cz)].push_back(index);
    return index;
  };
  std::map<std::pair<int, int>, int> edgeCount;
  auto addEdge = [&](int a, int b) {
    if (a == b) return;
    const auto reverse = edgeCount.find(std::make_pair(b, a));
    if (reverse != edgeCount.end()) {
      if (--reverse->second <= 0) edgeCount.erase(reverse);
      return;
    }
    edgeCount[std::make_pair(a, b)]++;
  };
  for (int tri : faceTris) {
    QVector3D p0 = vertex(tri, 0);
    QVector3D p1 = vertex(tri, 1);
    QVector3D p2 = vertex(tri, 2);
    const QVector3D geometric = QVector3D::crossProduct(p1 - p0, p2 - p0);
    if (QVector3D::dotProduct(geometric, hitNormal) < 0) std::swap(p1, p2);
    auto projectFace = [&](QVector3D point) {
      const float dist = QVector3D::dotProduct(point - hitPoint, hitNormal);
      if (std::abs(dist) <= 0.4f) point -= hitNormal * dist;
      return point;
    };
    const int i0 = weld(projectFace(p0));
    const int i1 = weld(projectFace(p1));
    const int i2 = weld(projectFace(p2));
    const QVector3D n = normalAt(tri);
    if (i0 >= 0) weldNormal[static_cast<size_t>(i0)] += n;
    if (i1 >= 0) weldNormal[static_cast<size_t>(i1)] += n;
    if (i2 >= 0) weldNormal[static_cast<size_t>(i2)] += n;
    addEdge(i0, i1);
    addEdge(i1, i2);
    addEdge(i2, i0);
  }
  // One side of a seam can be a single edge and the other side two edges that
  // meet in the middle. Split so those cancel and the outline is one loop.
  for (int guard = 0; guard < 64; ++guard) {
    int splitA = -1;
    int splitB = -1;
    int splitV = -1;
    for (const auto& entry : edgeCount) {
      if (entry.second <= 0) continue;
      const int a = entry.first.first;
      const int b = entry.first.second;
      const QVector3D ab = weldPts[static_cast<size_t>(b)] - weldPts[static_cast<size_t>(a)];
      const float ab2 = ab.lengthSquared();
      if (ab2 < 1e-6f) continue;
      for (int v = 0; v < static_cast<int>(weldPts.size()); ++v) {
        if (v == a || v == b) continue;
        const float t = QVector3D::dotProduct(weldPts[static_cast<size_t>(v)] - weldPts[static_cast<size_t>(a)], ab) / ab2;
        if (t <= 0.02f || t >= 0.98f) continue;
        const QVector3D closest = weldPts[static_cast<size_t>(a)] + ab * t;
        if ((weldPts[static_cast<size_t>(v)] - closest).lengthSquared() > 0.08f * 0.08f) continue;
        splitA = a;
        splitB = b;
        splitV = v;
        break;
      }
      if (splitV >= 0) break;
    }
    if (splitV < 0) break;
    auto it = edgeCount.find(std::make_pair(splitA, splitB));
    if (it == edgeCount.end()) break;
    if (--it->second <= 0) edgeCount.erase(it);
    addEdge(splitA, splitV);
    addEdge(splitV, splitB);
  }
  struct Edge {
    int a = 0;
    int b = 0;
  };
  std::vector<Edge> edges;
  std::map<int, std::vector<int>> startAt;
  for (const auto& entry : edgeCount) {
    for (int copy = 0; copy < entry.second; ++copy) {
      startAt[entry.first.first].push_back(static_cast<int>(edges.size()));
      edges.push_back(Edge{entry.first.first, entry.first.second});
    }
  }
  std::vector<char> used(edges.size(), 0);
  std::vector<std::vector<int>> loops;
  for (int start = 0; start < static_cast<int>(edges.size()); ++start) {
    if (used[static_cast<size_t>(start)]) continue;
    const int origin = edges[static_cast<size_t>(start)].a;
    int cursor = origin;
    std::vector<int> ring;
    int guard = 0;
    while (guard++ < static_cast<int>(edges.size()) + 2) {
      int next = -1;
      const auto out = startAt.find(cursor);
      if (out != startAt.end()) {
        for (int index : out->second) {
          if (!used[static_cast<size_t>(index)]) {
            next = index;
            break;
          }
        }
      }
      if (next < 0) break;
      used[static_cast<size_t>(next)] = 1;
      ring.push_back(cursor);
      cursor = edges[static_cast<size_t>(next)].b;
      if (cursor == origin) break;
    }
    if (cursor == origin && ring.size() >= 3) loops.push_back(std::move(ring));
  }
  if (loops.empty()) return false;
  const QVector3D bitangent = QVector3D::crossProduct(hitNormal, hitAxis).normalized();
  auto loopArea = [&](const std::vector<int>& ring) {
    double area = 0;
    for (int i = 0; i < static_cast<int>(ring.size()); ++i) {
      const QVector3D& p = weldPts[static_cast<size_t>(ring[static_cast<size_t>(i)])];
      const QVector3D& q = weldPts[static_cast<size_t>(ring[static_cast<size_t>((i + 1) % ring.size())])];
      const double ax = QVector3D::dotProduct(p, hitAxis);
      const double ay = QVector3D::dotProduct(p, bitangent);
      const double bx = QVector3D::dotProduct(q, hitAxis);
      const double by = QVector3D::dotProduct(q, bitangent);
      area += ax * by - bx * ay;
    }
    return area;
  };
  std::sort(loops.begin(), loops.end(), [&](const std::vector<int>& a, const std::vector<int>& b) {
    return std::abs(loopArea(a)) > std::abs(loopArea(b));
  });
  loop->clear();
  if (pointNormals) pointNormals->clear();
  for (int ringIndex = 0; ringIndex < static_cast<int>(loops.size()); ++ringIndex) {
    if (ringIndex > 0) {
      loop->push_back(QVector3D(1e20f, 1e20f, 1e20f));
      if (pointNormals) pointNormals->push_back(QVector3D());
    }
    for (int index : loops[static_cast<size_t>(ringIndex)]) {
      loop->push_back(weldPts[static_cast<size_t>(index)]);
      if (!pointNormals) continue;
      QVector3D n = weldNormal[static_cast<size_t>(index)];
      if (n.lengthSquared() < 1e-8f) n = hitNormal;
      pointNormals->push_back(n.normalized());
    }
  }
  *id = hitItem->id;
  *localNormal = hitNormal;
  *localAxis = hitAxis;
  if (modelOut) *modelOut = hitItem->model;
  return true;
}

bool Viewport::pickWorldFace(const QPointF& pos, QString* id, QVector3D* centre, QVector3D* normal) const {
  QVector<QVector3D> loop;
  QVector3D localNormal, localAxis;
  QMatrix4x4 model;
  if (!pickFace(pos, id, &loop, &localNormal, &localAxis, false, nullptr, &model)) return false;
  // The middle of the face's outline (weighted by length, so long edges count for what they are).
  QVector3D sum;
  double total = 0;
  int n = 0;
  while (n < loop.size() && loop[n].x() < 1e19f) ++n;
  for (int i = 0; i < n; ++i) {
    const QVector3D& a = loop[i];
    const QVector3D& b = loop[(i + 1) % n];
    const double length = (b - a).length();
    sum += (a + b) * static_cast<float>(0.5 * length);
    total += length;
  }
  if (n < 3 || total <= 1e-9) return false;
  *centre = model.map(sum / static_cast<float>(total));
  *normal = model.inverted().transposed().mapVector(localNormal).normalized();
  return true;
}

void Viewport::setOverhangs(bool on, double angleDegrees) {
  overhangOn = on;
  overhangAngle = std::clamp(angleDegrees, 0.0, 89.0);
  update();
}

void Viewport::setSection(bool on, int axis, double at, bool flip, QVector3D lo, QVector3D hi) {
  sectionOn = on;
  sectionAxis = std::clamp(axis, 0, 2);
  sectionAt = static_cast<float>(at);
  sectionFlip = flip;
  sectionLo = lo;
  sectionHi = hi;
  update();
}

// Points away from the kept side: -axis cuts away the low side of the axis, +axis (flipped) the high.
QVector3D Viewport::sectionNormal() const {
  QVector3D n;
  n[sectionAxis] = sectionFlip ? 1.f : -1.f;
  return n;
}

// The section plane's outline round the scene, so it is clear where the cut is.
void Viewport::paintSection(QPainter& painter) const {
  if (!sectionOn) return;
  const int a = sectionAxis, b = (a + 1) % 3, c = (a + 2) % 3;
  const float pad = 4;
  auto corner = [&](float u, float v) {
    QVector3D p;
    p[a] = sectionAt;
    p[b] = u;
    p[c] = v;
    return toScreen(p);
  };
  const float u0 = sectionLo[b] - pad, u1 = sectionHi[b] + pad, v0 = sectionLo[c] - pad, v1 = sectionHi[c] + pad;
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(QColor(220, 60, 50, 200), 1.6, Qt::DashLine);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  painter.drawPolygon(QPolygonF({corner(u0, v0), corner(u1, v0), corner(u1, v1), corner(u0, v1)}));
  painter.restore();
}

void Viewport::setFaceWorkplane(bool on, QVector3D origin, QVector3D normal, QVector3D xAxis) {
  faceWorkplane = on;
  workplaneOrigin = origin;
  workplaneNormal = normal.normalized();
  workplaneX = xAxis.normalized();
  update();
}

// The workplane on a face: a 100 mm grid in orange with its X (red) and Y (green) directions.
void Viewport::paintFaceWorkplane(QPainter& painter) const {
  if (!faceWorkplane) return;
  const QVector3D yAxis = QVector3D::crossProduct(workplaneNormal, workplaneX);
  auto at = [&](double u, double v) { return toScreen(workplaneOrigin + workplaneX * float(u) + yAxis * float(v) + workplaneNormal * 0.02f); };
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  const double half = 50;
  for (int i = -5; i <= 5; ++i) {
    painter.setPen(QPen(QColor(232, 116, 0, i == 0 ? 0 : (i % 5 == 0 ? 170 : 90)), i % 5 == 0 ? 1.4 : 1.0));
    painter.drawLine(at(i * 10, -half), at(i * 10, half));
    painter.drawLine(at(-half, i * 10), at(half, i * 10));
  }
  painter.setPen(QPen(QColor(220, 70, 60), 2.2));
  painter.drawLine(at(0, 0), at(20, 0));
  painter.setPen(QPen(QColor(90, 190, 90), 2.2));
  painter.drawLine(at(0, 0), at(0, 20));
  painter.setPen(QPen(QColor(232, 116, 0), 1.8));
  painter.setBrush(Qt::NoBrush);
  painter.drawPolygon(QPolygonF({at(-half, -half), at(half, -half), at(half, half), at(-half, half)}));
  painter.restore();
}

bool Viewport::itemAabb(const DrawItem& item, QVector3D* minOut, QVector3D* maxOut) const {
  const std::vector<float>& v = meshOf(item.mesh).vertices;
  if (v.size() < 3) return false;
  QVector3D lo(1e9f, 1e9f, 1e9f);
  QVector3D hi(-1e9f, -1e9f, -1e9f);
  for (size_t i = 0; i + 2 < v.size(); i += 3) {
    const QVector3D world = item.model.map(QVector3D(v[i], v[i + 1], v[i + 2]));
    lo.setX(std::min(lo.x(), world.x()));
    lo.setY(std::min(lo.y(), world.y()));
    lo.setZ(std::min(lo.z(), world.z()));
    hi.setX(std::max(hi.x(), world.x()));
    hi.setY(std::max(hi.y(), world.y()));
    hi.setZ(std::max(hi.z(), world.z()));
  }
  if (lo.x() > hi.x()) return false;
  *minOut = lo;
  *maxOut = hi;
  return true;
}

bool Viewport::meshBox(const CpuMesh& mesh, QVector3D* minOut, QVector3D* maxOut) const {
  if (mesh.hasBounds) {
    *minOut = QVector3D(mesh.lo[0], mesh.lo[1], mesh.lo[2]);
    *maxOut = QVector3D(mesh.hi[0], mesh.hi[1], mesh.hi[2]);
    return true;
  }
  const std::vector<float>& v = mesh.vertices;
  if (v.size() < 3) return false;
  QVector3D lo(1e9f, 1e9f, 1e9f);
  QVector3D hi(-1e9f, -1e9f, -1e9f);
  for (size_t i = 0; i + 2 < v.size(); i += 3) {
    const QVector3D p(v[i], v[i + 1], v[i + 2]);
    lo.setX(std::min(lo.x(), p.x()));
    lo.setY(std::min(lo.y(), p.y()));
    lo.setZ(std::min(lo.z(), p.z()));
    hi.setX(std::max(hi.x(), p.x()));
    hi.setY(std::max(hi.y(), p.y()));
    hi.setZ(std::max(hi.z(), p.z()));
  }
  if (lo.x() > hi.x()) return false;
  *minOut = lo;
  *maxOut = hi;
  return true;
}

int Viewport::boxHandleAt(const QPointF& pos) const {
  if (tool == 3 || tool == 9) return -1;
  const DrawItem* item = gizmoItem();
  if (!item) return -1;
  QVector3D lo;
  QVector3D hi;
  if (!meshBox(meshOf(item->mesh), &lo, &hi)) return -1;
  auto corner = [&](int index) {
    return item->model.map(QVector3D((index & 1) ? hi.x() : lo.x(), (index & 2) ? hi.y() : lo.y(), (index & 4) ? hi.z() : lo.z()));
  };
  auto dist2 = [&](const QVector3D& world) {
    const QPointF screen = toScreen(world);
    const float dx = static_cast<float>(pos.x() - screen.x());
    const float dy = static_cast<float>(pos.y() - screen.y());
    return std::sqrt(dx * dx + dy * dy);
  };
  int best = -1;
  float bestDist = 14.f;
  for (int i = 0; i < 8; ++i) {
    const float dist = dist2(corner(i));
    if (dist < bestDist) {
      bestDist = dist;
      best = i;
    }
  }
  if (best >= 0) return best;
  const QVector3D mid((lo.x() + hi.x()) * 0.5f, (lo.y() + hi.y()) * 0.5f, (lo.z() + hi.z()) * 0.5f);
  const QVector3D faces[6] = {QVector3D(lo.x(), mid.y(), mid.z()), QVector3D(hi.x(), mid.y(), mid.z()),
                              QVector3D(mid.x(), lo.y(), mid.z()), QVector3D(mid.x(), hi.y(), mid.z()),
                              QVector3D(mid.x(), mid.y(), lo.z()), QVector3D(mid.x(), mid.y(), hi.z())};
  for (int i = 0; i < 6; ++i) {
    const float dist = dist2(item->model.map(faces[i]));
    if (dist < bestDist) {
      bestDist = dist;
      best = 8 + i;
    }
  }
  return best;
}

QStringList Viewport::marqueeIds(const QRectF& rect, bool crossing) const {
  const QRectF selectionRect = rect.normalized();
  if (selectionRect.width() < 3.0 || selectionRect.height() < 3.0) return {};

  QSet<QString> seen;
  QStringList ids;
  auto overlaps = [&](const QRectF& box) {
    const QRectF candidate = box.normalized();
    if (crossing) {
      // Right-to-left drag: crossing selection. Use explicit axis-aligned
      // overlap rather than QRectF::intersects(), so all drag directions
      // behave identically even at zero/negative coordinate boundaries.
      return selectionRect.left() <= candidate.right() &&
             selectionRect.right() >= candidate.left() &&
             selectionRect.top() <= candidate.bottom() &&
             selectionRect.bottom() >= candidate.top();
    }
    // Left-to-right drag: fully contained selection.
    return selectionRect.left() <= candidate.left() &&
           selectionRect.right() >= candidate.right() &&
           selectionRect.top() <= candidate.top() &&
           selectionRect.bottom() >= candidate.bottom();
  };

  const QVector3D cornersUnit[8] = {
      QVector3D(0, 0, 0), QVector3D(1, 0, 0), QVector3D(0, 1, 0), QVector3D(1, 1, 0),
      QVector3D(0, 0, 1), QVector3D(1, 0, 1), QVector3D(0, 1, 1), QVector3D(1, 1, 1)};

  for (const DrawItem& item : items) {
    if (item.gizmo || meshOf(item.mesh).triangles <= 0 || item.copyIndex != 0 || item.id.isEmpty() || seen.contains(item.id)) continue;
    QVector3D lo;
    QVector3D hi;
    if (!meshBox(meshOf(item.mesh), &lo, &hi)) continue;

    QRectF box;
    bool first = true;
    for (const QVector3D& unit : cornersUnit) {
      const QVector3D local(lo.x() + (hi.x() - lo.x()) * unit.x(),
                            lo.y() + (hi.y() - lo.y()) * unit.y(),
                            lo.z() + (hi.z() - lo.z()) * unit.z());
      const QPointF screen = toScreen(item.model.map(local));
      if (first) {
        box = QRectF(screen, QSizeF(0, 0));
        first = false;
      } else {
        box = box.united(QRectF(screen, QSizeF(0, 0)));
      }
    }
    if (overlaps(box)) {
      seen.insert(item.id);
      ids << item.id;
    }
  }
  return ids;
}

void Viewport::paintGL() {
  if (gridDirty) rebuildGrid();
  if (dirty) upload();
  // QPainter at the end of the previous frame leaves depth writes and the depth test off, and
  // multisampling as its own hint left it: on again, so edges stay smooth.
  glEnable(GL_MULTISAMPLE);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glClearColor(0.071f, 0.082f, 0.102f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (!meshProgram || !lineProgram) return;
  const QMatrix4x4 vp = projection() * viewMatrix();
  const int lineColor = glGetUniformLocation(lineProgram, "uColor");

  glUseProgram(meshProgram);
  const int mvpLoc = glGetUniformLocation(meshProgram, "uMvp");
  const int modelLoc = glGetUniformLocation(meshProgram, "uModel");
  glUniform3f(glGetUniformLocation(meshProgram, "uEye"), eyePosition().x(), eyePosition().y(), eyePosition().z());
  const int colorLoc = glGetUniformLocation(meshProgram, "uColor");
  const int holeLoc = glGetUniformLocation(meshProgram, "uHole");
  const int selectedLoc = glGetUniformLocation(meshProgram, "uSelected");
  glUniform1f(glGetUniformLocation(meshProgram, "uClipOn"), sectionOn ? 1.f : 0.f);
  glUniform2f(glGetUniformLocation(meshProgram, "uOverhang"), overhangOn ? 1.f : 0.f,
              static_cast<float>(-std::sin(overhangAngle * 3.14159265358979 / 180.0)));
  glUniform1f(glGetUniformLocation(meshProgram, "uPlate"), static_cast<float>(workplaneZ));
  if (sectionOn) {
    const QVector3D n = sectionNormal();
    // Cut where dot(p, n) > w: unflipped n = -axis, w = -at (p < at goes); flipped n = axis, w = at.
    glUniform4f(glGetUniformLocation(meshProgram, "uClip"), n.x(), n.y(), n.z(), sectionFlip ? sectionAt : -sectionAt);
  }
  const QVector3D eye = eyePosition();
  std::vector<const Gpu*> opaque;
  std::vector<const Gpu*> holes;
  opaque.reserve(gpu.size());
  for (const Gpu& item : gpu) (item.hole ? holes : opaque).push_back(&item);
  auto farther = [&](const Gpu* a, const Gpu* b) {
    const QVector3D ca(a->model(0, 3), a->model(1, 3), a->model(2, 3));
    const QVector3D cb(b->model(0, 3), b->model(1, 3), b->model(2, 3));
    return (eye - ca).lengthSquared() > (eye - cb).lengthSquared();
  };
  std::sort(opaque.begin(), opaque.end(), farther);
  std::sort(holes.begin(), holes.end(), farther);
  auto drawMesh = [&](const Gpu* item) {
    glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, (vp * item->model).constData());
    glUniformMatrix4fv(modelLoc, 1, GL_FALSE, item->model.constData());
    if (preview) glUniform3f(colorLoc, 0.910f, 0.455f, 0.0f);
    else glUniform3f(colorLoc, item->color.x(), item->color.y(), item->color.z());
    glUniform1f(holeLoc, item->hole ? 1.f : 0.f);
    glUniform1f(selectedLoc, item->selected ? 1.f : 0.f);
    glBindVertexArray(item->vao);
    glDrawElements(GL_TRIANGLES, item->count, GL_UNSIGNED_INT, nullptr);
  };
  glDepthMask(GL_TRUE);
  for (const Gpu* item : opaque) drawMesh(item);
  if (!holes.empty()) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for (const Gpu* item : holes) drawMesh(item);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
  }

  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(gridProgram);
  QMatrix4x4 grid;
  grid.translate(0, 0, static_cast<float>(workplaneZ));
  glUniformMatrix4fv(glGetUniformLocation(gridProgram, "uMvp"), 1, GL_FALSE, (vp * grid).constData());
  const int gridColor = glGetUniformLocation(gridProgram, "uColor");
  const int gridSpacing = glGetUniformLocation(gridProgram, "uSpacing");
  const int gridAxis = glGetUniformLocation(gridProgram, "uAxis");
  glBindVertexArray(gridVao);
  if (showGrid) {
    glUniform3f(gridColor, 0.16f, 0.18f, 0.22f);
    glUniform1f(gridSpacing, gridFine);
    glUniform1i(gridAxis, 0);
    glDrawArrays(GL_LINES, 0, gridMinorX);
    glUniform1i(gridAxis, 1);
    glDrawArrays(GL_LINES, gridMinorX, gridMinor - gridMinorX);
    glUniform3f(gridColor, 0.30f, 0.36f, 0.42f);
    glUniform1f(gridSpacing, gridFine * 10.f);
    glUniform1i(gridAxis, 0);
    glDrawArrays(GL_LINES, gridMinor, gridMajorX);
    glUniform1i(gridAxis, 1);
    glDrawArrays(GL_LINES, gridMinor + gridMajorX, gridMajor - gridMajorX);
    glUniform3f(gridColor, 0.45f, 0.50f, 0.56f);
    glUniform1f(gridSpacing, 0.f);
    glDrawArrays(GL_LINES, gridMinor + gridMajor, gridTicks);
  }
  glUniform3f(gridColor, 0.78f, 0.55f, 0.32f);
  glUniform1f(gridSpacing, 0.f);
  glDrawArrays(GL_LINES, gridMinor + gridMajor + gridTicks, gridBorder);
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);

  const DrawItem* gizmo = gizmoItem();
  std::vector<QPointF> labelPoints;
  std::vector<QString> labelText;
  const float step = bedW > 400 || bedD > 400 ? 50.f : 20.f;
  const float hx = static_cast<float>(bedW * 0.5);
  const float hy = static_cast<float>(bedD * 0.5);
  const float z = static_cast<float>(workplaneZ);
  const QMatrix4x4 clipMatrix = projection() * viewMatrix();
  auto projectFront = [&](const QVector3D& world, QPointF* screen) {
    const QVector4D clip = clipMatrix * QVector4D(world, 1.f);
    if (clip.w() <= 1e-3f) return false;
    const QVector3D ndc = clip.toVector3D() / clip.w();
    if (ndc.z() < -1.f || ndc.z() > 1.f) return false;
    *screen = QPointF((ndc.x() * 0.5 + 0.5) * width(), (1.0 - (ndc.y() * 0.5 + 0.5)) * height());
    return true;
  };
  for (float x = -hx; x <= hx + 0.1f; x += step) {
    QPointF screen;
    if (!projectFront(QVector3D(std::min(x, hx), -hy, z), &screen)) continue;
    labelPoints.push_back(screen + QPointF(0, 16));
    labelText.push_back(QString::number(static_cast<int>(std::lround(std::min(x, hx) + hx))));
  }
  for (float y = -hy; y <= hy + 0.1f; y += step) {
    QPointF screen;
    if (!projectFront(QVector3D(-hx, std::min(y, hy), z), &screen)) continue;
    labelPoints.push_back(screen + QPointF(-28, 4));
    labelText.push_back(QString::number(static_cast<int>(std::lround(std::min(y, hy) + hy))));
  }
  int selectedCount = 0;
  for (const DrawItem& item : items)
    if (item.selected && meshOf(item.mesh).triangles > 0) ++selectedCount;
  if (!preview && selectedCount > 1) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(gizmoVao);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoVbo);
    glUseProgram(lineProgram);
    glUniformMatrix4fv(glGetUniformLocation(lineProgram, "uMvp"), 1, GL_FALSE, vp.constData());
    const int links[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                              {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const DrawItem& item : items) {
      if (!item.selected || (item.gizmo && alignBoxes.empty()) || meshOf(item.mesh).triangles <= 0) continue;
      QVector3D lo;
      QVector3D hi;
      if (!meshBox(meshOf(item.mesh), &lo, &hi)) continue;
      auto cornerOf = [&](int index) {
        return item.model.map(QVector3D((index & 1) ? hi.x() : lo.x(),
                                        (index & 2) ? hi.y() : lo.y(),
                                        (index & 4) ? hi.z() : lo.z()));
      };
      const QVector3D pivot = item.model.map((lo + hi) * 0.5f);
      const float len = gizmoLength(pivot);
      const float edgeRadius = std::max(0.15f, len * 0.012f);
      std::vector<float> edges;
      edges.reserve(12 * 18);
      for (const auto& link : links) addRod(edges, cornerOf(link[0]), cornerOf(link[1]), edgeRadius);
      if (edges.empty()) continue;
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(edges.size() * sizeof(float)), edges.data(), GL_DYNAMIC_DRAW);
      glUniform3f(lineColor, 0.20f, 0.86f, 0.96f);
      glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(edges.size() / 3));
    }
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
  }
  // While aligning nothing can be dragged, so the move handles are hidden.
  if (gizmo && tool != 3 && tool != 4 && tool != 9 && alignBoxes.empty()) {
    const QVector3D pivot(gizmo->model(0, 3), gizmo->model(1, 3), gizmo->model(2, 3));
    const float len = gizmoLength(pivot);
    const int hot = dragging ? axis : hoverAxis;
    const float colors[3][3] = {{0.93f, 0.33f, 0.26f}, {0.42f, 0.82f, 0.36f}, {0.32f, 0.58f, 0.95f}};
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(gizmoVao);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoVbo);
    glUseProgram(lineProgram);
    glUniformMatrix4fv(glGetUniformLocation(lineProgram, "uMvp"), 1, GL_FALSE, vp.constData());
    QVector3D boxLo;
    QVector3D boxHi;
    if (meshBox(meshOf(gizmo->mesh), &boxLo, &boxHi)) {
      auto cornerOf = [&](int index) {
        return gizmo->model.map(QVector3D((index & 1) ? boxHi.x() : boxLo.x(), (index & 2) ? boxHi.y() : boxLo.y(),
                                           (index & 4) ? boxHi.z() : boxLo.z()));
      };
      std::vector<float> edges;
      const int links[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
      const float edgeRadius = std::max(0.15f, len * 0.012f);
      for (const auto& link : links) addRod(edges, cornerOf(link[0]), cornerOf(link[1]), edgeRadius);
      if (!edges.empty()) {
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(edges.size() * sizeof(float)), edges.data(), GL_DYNAMIC_DRAW);
        glUniform3f(lineColor, 0.22f, 0.78f, 0.86f);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(edges.size() / 3));
      }
      const int activeHandle = scaleDrag ? scaleHandle : hoverHandle;
      const QVector3D mid((boxLo.x() + boxHi.x()) * 0.5f, (boxLo.y() + boxHi.y()) * 0.5f, (boxLo.z() + boxHi.z()) * 0.5f);
      const QVector3D faces[6] = {QVector3D(boxLo.x(), mid.y(), mid.z()), QVector3D(boxHi.x(), mid.y(), mid.z()),
                                  QVector3D(mid.x(), boxLo.y(), mid.z()), QVector3D(mid.x(), boxHi.y(), mid.z()),
                                  QVector3D(mid.x(), mid.y(), boxLo.z()), QVector3D(mid.x(), mid.y(), boxHi.z())};
      auto drawHandle = [&](const QVector3D& world, float half, int handle, bool face) {
        std::vector<float> cube;
        addCube(cube, world, half * (handle == activeHandle ? 1.35f : 1.f));
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(cube.size() * sizeof(float)), cube.data(), GL_DYNAMIC_DRAW);
        if (handle == activeHandle) glUniform3f(lineColor, 0.95f, 0.78f, 0.28f);
        else if (face) glUniform3f(lineColor, 0.12f, 0.13f, 0.15f);
        else glUniform3f(lineColor, 0.94f, 0.96f, 0.97f);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(cube.size() / 3));
      };
      const float cornerSize = std::max(0.6f, len * 0.045f);
      for (int i = 0; i < 8; ++i) drawHandle(cornerOf(i), cornerSize, i, false);
      const float faceSize = cornerSize * 0.72f;
      for (int i = 0; i < 6; ++i) drawHandle(gizmo->model.map(faces[i]), faceSize, 8 + i, true);
    }
    for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
      QVector3D dir;
      QVector3D u;
      QVector3D v;
      axisBasis(gizmo->model, pivot, axisIndex, &dir, &u, &v);
      std::vector<float> tris;
      const bool active = axisIndex == hot;
      const float scale = active ? 1.18f : 1.f;
      if (active) addRod(tris, pivot - dir * len * 4.f, pivot + dir * len * 4.f, len * 0.012f);
      if (tool == 1) {
        addRing(tris, pivot, u, v, len * 0.72f * scale, len * (active ? 0.03f : 0.02f), true);
      } else {
        addArrow(tris, pivot, pivot + dir * len * scale);
      }
      if (tris.empty()) continue;
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tris.size() * sizeof(float)), tris.data(), GL_DYNAMIC_DRAW);
      float r = colors[axisIndex][0];
      float g = colors[axisIndex][1];
      float b = colors[axisIndex][2];
      if (active) {
        r = std::min(1.f, r + 0.35f);
        g = std::min(1.f, g + 0.35f);
        b = std::min(1.f, b + 0.35f);
      } else if (dragging) {
        r *= 0.35f;
        g *= 0.35f;
        b *= 0.35f;
      }
      glUniform3f(lineColor, r, g, b);
      glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(tris.size() / 3));
    }
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
  }
  if (tool == 9 && gizmo && !filletPick.empty()) {
    // Edge fillet (beta): every sharp edge as a thin rod, picked ones thick orange, the one under
    // the mouse bright. Edges whose faces both point away are left out. One draw call per colour.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(gizmoVao);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoVbo);
    glUseProgram(lineProgram);
    glUniformMatrix4fv(glGetUniformLocation(lineProgram, "uMvp"), 1, GL_FALSE, vp.constData());
    const QVector3D pivot(gizmo->model(0, 3), gizmo->model(1, 3), gizmo->model(2, 3));
    const float radius = std::max(0.08f, gizmoLength(pivot) * 0.0075f);
    std::vector<float> plain, chosen, hot;
    for (const PickEdge& edge : filletPick) {
      if (!betaEdgeFacing(*gizmo, edge)) continue;
      const QVector3D nudge = betaEdgeNudge(*gizmo, edge);
      const QVector3D a = gizmo->model.map(edge.a) + nudge;
      const QVector3D b = gizmo->model.map(edge.b) + nudge;
      const bool on = std::find(filletOn.begin(), filletOn.end(), edge.key) != filletOn.end();
      if (edge.key == hoverEdge) addRod(hot, a, b, radius * 2.2f);
      else if (on) addRod(chosen, a, b, radius * 1.9f);
      else addRod(plain, a, b, radius);
    }
    auto draw = [&](const std::vector<float>& tris, float r, float g, float b) {
      if (tris.empty()) return;
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tris.size() * sizeof(float)), tris.data(), GL_DYNAMIC_DRAW);
      glUniform3f(lineColor, r, g, b);
      glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(tris.size() / 3));
    };
    draw(plain, 0.62f, 0.78f, 0.92f);
    draw(chosen, 0.95f, 0.55f, 0.12f);
    draw(hot, 1.f, 0.93f, 0.55f);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
  }
  if (tool == 4 && gizmo && !filletPick.empty()) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(gizmoVao);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoVbo);
    glUseProgram(lineProgram);
    glUniformMatrix4fv(glGetUniformLocation(lineProgram, "uMvp"), 1, GL_FALSE, vp.constData());
    const QVector3D pivot(gizmo->model(0, 3), gizmo->model(1, 3), gizmo->model(2, 3));
    const float len = gizmoLength(pivot);
    const float radius = std::max(0.18f, len * 0.016f);
    for (const PickEdge& edge : filletPick) {
      const QVector3D nudge = edgeOutward(edge) * 0.35f;
      const QVector3D a = gizmo->model.map(edge.a + nudge);
      const QVector3D b = gizmo->model.map(edge.b + nudge);
      const bool on = filletAll || std::find(filletOn.begin(), filletOn.end(), edge.key) != filletOn.end();
      const bool hot = edge.key == hoverEdge;
      std::vector<float> tris;
      addRod(tris, a, b, radius * ((on || hot) ? 1.85f : 1.f));
      if (tris.empty()) continue;
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tris.size() * sizeof(float)), tris.data(), GL_DYNAMIC_DRAW);
      if (hot) glUniform3f(lineColor, 1.f, 0.93f, 0.55f);
      else if (on) glUniform3f(lineColor, 0.95f, 0.72f, 0.28f);
      else glUniform3f(lineColor, 0.78f, 0.86f, 0.92f);
      glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(tris.size() / 3));
    }
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
  }
  // Align: the part that stays gets an orange box; each axis gets a line of three round dots
  // (low side, centre, high side). Drawn in GL like the other handles, always on top.
  QVector3D alignLo, alignHi, alignRefLo, alignRefHi;
  if (!preview && alignFrame(&alignLo, &alignHi, &alignRefLo, &alignRefHi)) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(gizmoVao);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoVbo);
    glUseProgram(lineProgram);
    glUniformMatrix4fv(glGetUniformLocation(lineProgram, "uMvp"), 1, GL_FALSE, vp.constData());
    auto draw = [&](const std::vector<float>& tris, float r, float g, float b) {
      if (tris.empty()) return;
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tris.size() * sizeof(float)), tris.data(), GL_DYNAMIC_DRAW);
      glUniform3f(lineColor, r, g, b);
      glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(tris.size() / 3));
    };
    const int links[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    if (!alignAnchorId.isEmpty()) {
      const float len = gizmoLength((alignRefLo + alignRefHi) * 0.5f);
      std::vector<float> rods;
      auto corner = [&](int i) {
        return QVector3D((i & 1) ? alignRefHi.x() : alignRefLo.x(), (i & 2) ? alignRefHi.y() : alignRefLo.y(),
                         (i & 4) ? alignRefHi.z() : alignRefLo.z());
      };
      for (const auto& link : links) addRod(rods, corner(link[0]), corner(link[1]), std::max(0.25f, len * 0.022f));
      draw(rods, 0.91f, 0.45f, 0.0f);
    }
    if (alignHover >= 0) {
      // Where the hovered dot lines things up: an outline through the whole selection.
      QVector3D p;
      alignHandlePoint(alignHover, &p);
      const int a = alignHover / 3;
      QVector3D c[4];
      for (int i = 0; i < 4; ++i) {
        const float u = (i == 1 || i == 2) ? 1.f : 0.f;
        const float v = (i >= 2) ? 1.f : 0.f;
        const QVector3D lo = alignLo, hi = alignHi;
        if (a == 0) c[i] = QVector3D(p.x(), lo.y() + (hi.y() - lo.y()) * u, lo.z() + (hi.z() - lo.z()) * v);
        else if (a == 1) c[i] = QVector3D(lo.x() + (hi.x() - lo.x()) * u, p.y(), lo.z() + (hi.z() - lo.z()) * v);
        else c[i] = QVector3D(lo.x() + (hi.x() - lo.x()) * u, lo.y() + (hi.y() - lo.y()) * v, p.z());
      }
      const float len = gizmoLength(p);
      std::vector<float> rods;
      for (int i = 0; i < 4; ++i) addRod(rods, c[i], c[(i + 1) % 4], std::max(0.15f, len * 0.012f));
      draw(rods, 1.f, 0.88f, 0.54f);
    }
    const QVector3D right = cameraRight().normalized();
    const QVector3D up = cameraUp().normalized();
    const float axisColor[3][3] = {{0.93f, 0.33f, 0.26f}, {0.42f, 0.82f, 0.36f}, {0.32f, 0.58f, 0.95f}};
    for (int a = 0; a < 3; ++a) {
      QVector3D first, last;
      alignHandlePoint(a * 3, &first);
      alignHandlePoint(a * 3 + 2, &last);
      std::vector<float> line;
      addRod(line, first, last, std::max(0.12f, gizmoLength((first + last) * 0.5f) * 0.01f));
      draw(line, axisColor[a][0] * 0.8f, axisColor[a][1] * 0.8f, axisColor[a][2] * 0.8f);
      for (int m = 0; m < 3; ++m) {
        const int index = a * 3 + m;
        QVector3D p;
        alignHandlePoint(index, &p);
        const bool hot = index == alignHover;
        const float r = std::max(0.9f, gizmoLength(p) * 0.09f) * (hot ? 1.3f : 1.f);
        std::vector<float> rim;
        addDisc(rim, p, right, up, r);
        draw(rim, axisColor[a][0], axisColor[a][1], axisColor[a][2]);
        std::vector<float> fill;
        addDisc(fill, p + (eyePosition() - p).normalized() * r * 0.01f, right, up, r * 0.62f);
        if (hot) draw(fill, 1.f, 0.88f, 0.54f);
        else draw(fill, 0.96f, 0.96f, 0.96f);
      }
    }
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
  }
  glBindVertexArray(0);
  // Qt's painter leaves face culling alone: left on (the gizmo and the align dots turn it on), it
  // drops the filled shapes it draws next (the view cube's faces, label boxes), keeping only text.
  glDisable(GL_CULL_FACE);

  QPainter painter(this);
  painter.setPen(QColor(232, 116, 0));
  painter.setFont(QFont(QStringLiteral("Segoe UI"), 8));
  for (int i = 0; i < labelPoints.size() && i < labelText.size(); ++i) {
    const QPointF point = labelPoints[static_cast<int>(i)];
    if (point.x() < -40 || point.y() < -20 || point.x() > width() + 40 || point.y() > height() + 20) continue;
    painter.drawText(point, labelText[static_cast<int>(i)]);
  }
  painter.drawText(12, height() - 12, QString("Plate %1 × %2 mm").arg(bedW, 0, 'f', 0).arg(bedD, 0, 'f', 0));
  // The size labels show while a resize handle is dragged, and already while the mouse rests on one,
  // so the current length, width and height can be read before changing them.
  const bool resizing = liveScaleDimensions && scaleHandle >= 0;
  const int shownHandle = resizing ? scaleHandle : (!dragging && hoverHandle >= 0 ? hoverHandle : -1);
  if (gizmo && shownHandle >= 0) {
    QVector3D boxLo;
    QVector3D boxHi;
    if (meshBox(meshOf(gizmo->mesh), &boxLo, &boxHi)) {
      double shownDimensions[3] = {liveDimensions[0], liveDimensions[1], liveDimensions[2]};
      if (!resizing) {
        double pos[3];
        double rot[3];
        double scale[3];
        decomposePlacement(gizmo->parentWorld.inverted() * gizmo->model, pos, rot, scale);
        shownDimensions[0] = std::abs(boxHi.x() - boxLo.x()) * std::abs(scale[0]);
        shownDimensions[1] = std::abs(boxHi.y() - boxLo.y()) * std::abs(scale[1]);
        shownDimensions[2] = std::abs(boxHi.z() - boxLo.z()) * std::abs(scale[2]);
      }
      const QVector3D mid((boxLo.x() + boxHi.x()) * 0.5f, (boxLo.y() + boxHi.y()) * 0.5f, (boxLo.z() + boxHi.z()) * 0.5f);
      auto localPointForHandle = [&](int handle) {
        if (handle < 8) {
          return QVector3D((handle & 1) ? boxHi.x() : boxLo.x(), (handle & 2) ? boxHi.y() : boxLo.y(),
                           (handle & 4) ? boxHi.z() : boxLo.z());
        }
        const int axisIndex = (handle - 8) / 2;
        const bool positive = ((handle - 8) % 2) == 1;
        QVector3D p = mid;
        const float value = positive ? (axisIndex == 0 ? boxHi.x() : axisIndex == 1 ? boxHi.y() : boxHi.z())
                                     : (axisIndex == 0 ? boxLo.x() : axisIndex == 1 ? boxLo.y() : boxLo.z());
        if (axisIndex == 0) p.setX(value);
        else if (axisIndex == 1) p.setY(value);
        else p.setZ(value);
        return p;
      };
      auto drawDimension = [&](int axisIndex, const QPointF& anchor, double value, const QPointF& offset) {
        const QString text = axisIndex == 0
                                  ? QString("L %1 mm").arg(value, 0, 'f', 2)
                                  : axisIndex == 1 ? QString("W %1 mm").arg(value, 0, 'f', 2)
                                                   : QString("H %1 mm").arg(value, 0, 'f', 2);
        const QFontMetrics metrics(painter.font());
        const QRectF textBox = metrics.boundingRect(text).adjusted(-6, -3, 6, 3);
        QPointF topLeft = anchor + offset;
        if (topLeft.x() + textBox.width() > width() - 4) topLeft.setX(width() - textBox.width() - 4);
        if (topLeft.y() + textBox.height() > height() - 24) topLeft.setY(height() - textBox.height() - 24);
        if (topLeft.x() < 4) topLeft.setX(4);
        if (topLeft.y() < 4) topLeft.setY(4);
        const QRectF rect(topLeft, textBox.size());
        painter.save();
        painter.setPen(QPen(QColor(235, 228, 214), 1));
        painter.setBrush(QColor(24, 28, 34, 225));
        painter.drawRoundedRect(rect, 4, 4);
        painter.setPen(QColor(245, 239, 229));
        painter.drawText(rect.adjusted(6, 3, -6, -3), Qt::AlignCenter, text);
        painter.restore();
      };
      if (shownHandle < 8) {
        const QPointF anchor = toScreen(gizmo->model.map(localPointForHandle(shownHandle)));
        drawDimension(0, anchor, shownDimensions[0], QPointF(10, -25));
        drawDimension(1, anchor, shownDimensions[1], QPointF(10, 0));
        drawDimension(2, anchor, shownDimensions[2], QPointF(10, 25));
      } else {
        const int changedAxis = (shownHandle - 8) / 2;
        const QPointF anchor = toScreen(gizmo->model.map(localPointForHandle(shownHandle)));
        const QPointF offset = changedAxis == 0 ? QPointF(12, -22) : changedAxis == 1 ? QPointF(12, -8) : QPointF(12, 8);
        drawDimension(changedAxis, anchor, shownDimensions[changedAxis], offset);
      }
    }
  }
  if (marqueeing) {
    const QRectF rect(marqueeStart, marqueeCurrent);
    const QRectF normalized = rect.normalized();
    painter.setBrush(QColor(232, 116, 0, 36));
    painter.setPen(QPen(QColor(232, 116, 0, 220), 1, Qt::DashLine));
    painter.drawRect(normalized);
  }
  paintFaceWorkplane(painter);
  paintSection(painter);
  paintAlign(painter);
  paintMeasure(painter);
  paintViewCube(painter);
  paintFocusMarker(painter);
}

bool Viewport::slicePlaneAt(const QPointF& pos, QVector3D* point, QVector3D* normal) const {
  if (!point || !normal) return false;
  QVector3D origin;
  QVector3D dir;
  cameraRay(pos, &origin, &dir);
  float best = 1e9f;
  bool found = false;
  for (const DrawItem& item : items) {
    if (item.hole || item.gizmo) continue;
    float dist = 0;
    int t = -1;
    if (!hitItem(item, origin, dir, best, &dist, &t)) continue;
    const CpuMesh& m = meshOf(item.mesh);
    QVector3D w[3];
    for (int k = 0; k < 3; ++k) {
      const float* a = m.corner(static_cast<size_t>(t), k);
      w[k] = item.model.map(QVector3D(a[0], a[1], a[2]));
    }
    const QVector3D worldN = QVector3D::crossProduct(w[1] - w[0], w[2] - w[0]);
    if (worldN.lengthSquared() < 1e-10f) continue;
    best = dist;
    found = true;
    *point = origin + dir * dist;
    *normal = worldN.normalized();
  }
  return found;
}

void Viewport::mousePressEvent(QMouseEvent* event) {
  focusIds.clear();  // the user takes over
  lastPos = event->position();
  pressPos = event->pos();
  marqueeStart = event->pos();
  marqueeCurrent = event->pos();
  if (spaceHeld && event->button() == Qt::LeftButton) {
    beginNavigation();
    panning = true;
    setCursor(Qt::ClosedHandCursor);
    return;
  }
  if (event->button() == Qt::LeftButton && !event->modifiers().testFlag(Qt::AltModifier)) {
    const int face = viewCubeFace(event->position());
    if (face >= 0) {
      lookFace(face);
      return;
    }
  }
  if (event->button() == Qt::RightButton || (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier))) {
    beginNavigation();
    orbiting = true;
    return;
  }
  alignConsumed = false;
  if (!alignBoxes.empty() && event->button() == Qt::LeftButton && !event->modifiers().testFlag(Qt::ShiftModifier)) {
    const int handle = alignHandleAt(event->position());
    if (handle >= 0) {
      alignConsumed = true;
      emit alignHandleClicked(handle / 3, handle % 3);
      return;
    }
    // Clicking a part picks the one that stays (the window decides on release); it never drags.
    if (!pickId(event->position()).isEmpty()) return;
    // Empty space: a box drag still selects; the move handles are off while aligning.
    if (!showFocus || !focusHit(event->position())) {
      marqueeing = true;
      return;
    }
  }
  if (tool == 4 && event->button() == Qt::LeftButton) {
    const int key = pickFilletEdge(event->position());
    if (key >= 0) emit edgePicked(key, event->modifiers());
    return;
  }
  if (tool == 9 && event->button() == Qt::LeftButton) {
    // Edge fillet (beta): an edge toggles; anything else only changes which object is picked.
    const int key = pickFilletEdge(event->position());
    if (key >= 0) {
      emit edgePicked(key, event->modifiers());
    } else {
      const QString id = pickId(event->position());
      if (!id.isEmpty()) emit picked(id, Qt::NoModifier);
    }
    return;
  }
  if (tool == 8 && event->button() == Qt::LeftButton) {
    QVector3D hit;
    if (measurePointAt(event->position(), &hit)) emit measurePicked(hit);
    return;
  }
  if ((tool == 12 || tool == 13) && event->button() == Qt::LeftButton && !event->modifiers().testFlag(Qt::ShiftModifier)) {
    QString id;
    QVector3D centre, normal;
    if (pickWorldFace(event->position(), &id, &centre, &normal)) emit worldFacePicked(id, centre, normal);
    else emit worldFacePicked(QString(), QVector3D(), QVector3D());
    return;
  }
  if (tool == 10 && event->button() == Qt::LeftButton) {
    QVector3D point;
    QVector3D normal;
    if (slicePlaneAt(event->position(), &point, &normal)) emit sliceFacePicked(point, normal);
    return;
  }
  if (event->button() == Qt::MiddleButton ||
      (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ShiftModifier))) {
    beginNavigation();
    panning = true;
    return;
  }
  if (showFocus && event->button() == Qt::LeftButton && focusHit(event->position())) {
    // The view holds still while the marker follows the cursor; it glides over on release.
    pendingFocus = target;
    focusDragging = true;
    return;
  }
  if (event->button() != Qt::LeftButton) return;
  if (tool == 3 || tool == 6 || tool == 7) {
    QString id;
    QVector<QVector3D> loop;
    QVector3D normal;
    QVector3D faceAxis;
    if (tool == 7) {
      QVector<QVector3D> normals;
      QVector3D hit;
      if (pickFace(event->position(), &id, &loop, &normal, &faceAxis, true, &normals) && solidHit(event->position(), &hit))
        emit smoothFaceChosen(id, loop, normals, hit);
    } else if (pickFace(event->position(), &id, &loop, &normal, &faceAxis)) {
      emit faceChosen(id, loop, normal, faceAxis);
    }
    return;
  }
  // Axis arrows and corner boxes sit off the mesh. Testing the empty-pick
  // marquee first ate those clicks, so the drag never started.
  scaleDrag = false;
  freeArmed = false;
  freeDrag = false;
  int box = boxHandleAt(event->position());
  axis = gizmoAxis(event->position());
  if (tool == 1 && axis >= 0) box = -1;
  else if (box >= 0) axis = -1;
  if (box < 0 && axis < 0 && pickId(event->position()).isEmpty() && tool != 3 && tool != 4 && tool != 6 && tool != 7 &&
      tool != 8 && tool != 9 && tool != 10) {
    marqueeing = true;
    return;
  }
  const DrawItem* item = (box >= 0 || axis >= 0) ? gizmoItem() : nullptr;
  if (!item && tool == 0) {
    const QString id = pickId(event->position());
    if (!id.isEmpty()) {
      for (const DrawItem& candidate : items) {
        if (candidate.gizmo && candidate.id == id && candidate.copyIndex == 0) {
          item = &candidate;
          freeArmed = true;
          break;
        }
      }
    }
  }
  if (!item) return;
  if (box >= 0) {
    QVector3D lo;
    QVector3D hi;
    if (!meshBox(meshOf(item->mesh), &lo, &hi)) return;
    scaleDrag = true;
    scaleLo = lo;
    scaleHi = hi;
    liveScaleDimensions = true;
    scaleHandle = box;
    auto corner = [&](int index) {
      return QVector3D((index & 1) ? hi.x() : lo.x(), (index & 2) ? hi.y() : lo.y(), (index & 4) ? hi.z() : lo.z());
    };
    if (box < 8) {
      scaleGrab = corner(box);
      scaleFix = corner(box ^ 7);
    } else {
      const int axisIndex = (box - 8) / 2;
      const bool positive = (box - 8) % 2 == 1;
      const QVector3D mid((lo.x() + hi.x()) * 0.5f, (lo.y() + hi.y()) * 0.5f, (lo.z() + hi.z()) * 0.5f);
      const float grab = positive ? (axisIndex == 0 ? hi.x() : axisIndex == 1 ? hi.y() : hi.z())
                                  : (axisIndex == 0 ? lo.x() : axisIndex == 1 ? lo.y() : lo.z());
      const float fix = positive ? (axisIndex == 0 ? lo.x() : axisIndex == 1 ? lo.y() : lo.z())
                                 : (axisIndex == 0 ? hi.x() : axisIndex == 1 ? hi.y() : hi.z());
      scaleGrab = mid;
      scaleFix = mid;
      if (axisIndex == 0) {
        scaleGrab.setX(grab);
        scaleFix.setX(fix);
      } else if (axisIndex == 1) {
        scaleGrab.setY(grab);
        scaleFix.setY(fix);
      } else {
        scaleGrab.setZ(grab);
        scaleFix.setZ(fix);
      }
    }
  }
  // Grabbing a part while the view still glides (to a part just picked): the view stops where it
  // is, so the part does not slide out from under the cursor.
  if (easeTimer && easeTimer->isActive()) stopEase();
  dragging = true;
  dragAnnounced = false;
  dragId = item->id;
  dragParent = item->parentWorld;
  dragPivot = QVector3D(item->model(0, 3), item->model(1, 3), item->model(2, 3));
  if (!itemAabb(*item, &dragMin, &dragMax)) {
    dragMin = dragPivot;
    dragMax = dragPivot;
  }
  if (!scaleDrag && freeArmed) {
    // Dragged flat, at its own height: the point grabbed stays under the cursor, sliding level.
    QVector3D origin;
    QVector3D dir;
    cameraRay(event->position(), &origin, &dir);
    freeGrab = dragPivot;
    float level = dragMin.z();
    float nearest = 1e9f;
    float dist = 0;
    int tri = -1;
    if (hitItem(*item, origin, dir, nearest, &dist, &tri) && dist > 0) level = (origin + dir * dist).z();
    if (std::abs(dir.z()) > 1e-5f) {
      const float t = (level - origin.z()) / dir.z();
      if (t > 0) freeGrab = origin + dir * t;
    }
  } else if (!scaleDrag) {
    QVector3D local(axis == 0 ? 1.f : 0.f, axis == 1 ? 1.f : 0.f, axis == 2 ? 1.f : 0.f);
    dragAxis = (item->model.map(local) - dragPivot).normalized();
  }
  QMatrix4x4 localMatrix = item->parentWorld.inverted() * item->model;
  double pos[3];
  double rot[3];
  double scale[3];
  decomposePlacement(localMatrix, pos, rot, scale);
  for (int i = 0; i < 3; ++i) {
    startPos[i] = pos[i];
    startRot[i] = rot[i];
    startScale[i] = scale[i];
  }
  if (scaleDrag) {
    QVector3D lo;
    QVector3D hi;
    if (meshBox(meshOf(item->mesh), &lo, &hi)) {
      scaleLo = lo;
      scaleHi = hi;
      scaleBaseExtents[0] = std::abs(hi.x() - lo.x());
      scaleBaseExtents[1] = std::abs(hi.y() - lo.y());
      scaleBaseExtents[2] = std::abs(hi.z() - lo.z());
      liveDimensions[0] = scaleBaseExtents[0] * std::abs(startScale[0]);
      liveDimensions[1] = scaleBaseExtents[1] * std::abs(startScale[1]);
      liveDimensions[2] = scaleBaseExtents[2] * std::abs(startScale[2]);
    }
  }
  QVector3D origin;
  QVector3D dir;
  cameraRay(event->position(), &origin, &dir);
  if (!freeArmed && !scaleDrag) dragParam = axisParam(origin, dir, dragPivot, dragAxis);
  if (tool == 1 && !scaleDrag) {
    const float denom = QVector3D::dotProduct(dir, dragAxis);
    if (std::abs(denom) > 1e-5f) {
      const float t = QVector3D::dotProduct(dragPivot - origin, dragAxis) / denom;
      dragHit = origin + dir * t;
    } else {
      dragHit = dragPivot + QVector3D(1, 0, 0);
    }
  }
  emit transformStarted();
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
  // Sub-pixel positions, so slow movements are not rounded into steps.
  const QPointF delta = event->position() - lastPos;
  lastPos = event->position();
  // Hold Ctrl while orbiting or panning for fine control (a quarter of the speed).
  const float fine = event->modifiers().testFlag(Qt::ControlModifier) ? 0.25f : 1.f;
  if (marqueeing && (event->buttons() & Qt::LeftButton)) {
    marqueeCurrent = event->pos();
    update();
    return;
  }
  if (orbiting) {
    const float degrees = 0.3f * fine;  // per pixel
    const QVector3D right = cameraGoal.rotatedVector(QVector3D(1, 0, 0)).normalized();
    const QQuaternion yaw = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), static_cast<float>(-delta.x()) * degrees);
    const QQuaternion pitch = QQuaternion::fromAxisAndAngle(right, static_cast<float>(-delta.y()) * degrees);
    cameraGoal = (yaw * pitch * cameraGoal).normalized();
    if (!easeTimer->isActive()) easeTimer->start();
    return;
  }
  if (panning) {
    // The point under the cursor follows the cursor: one pixel is this much at the focus point.
    const float perPixel = 2.f * radiusGoal * std::tan(20.f * 3.14159265f / 180.f) / std::max(1, height()) * fine;
    const QVector3D right = cameraGoal.rotatedVector(QVector3D(1, 0, 0)).normalized();
    const QVector3D up = cameraGoal.rotatedVector(QVector3D(0, 1, 0)).normalized();
    targetGoal = clampFocus(targetGoal + right * (static_cast<float>(-delta.x()) * perPixel) +
                            up * (static_cast<float>(delta.y()) * perPixel));
    if (!easeTimer->isActive()) easeTimer->start();
    return;
  }
  if (spaceHeld && !dragging) {
    setCursor(Qt::OpenHandCursor);
    return;
  }
  if (focusDragging && (event->buttons() & Qt::LeftButton)) {
    QVector3D hit;
    if (focusOnBed(event->position(), &hit)) {
      pendingFocus = hit;
      update();
    }
    return;
  }
  if (!dragging && !orbiting && !panning) {
    const int face = viewCubeFace(event->position());
    if (face != hoverView) {
      hoverView = face;
      setToolTip(face == 10   ? QStringLiteral("Home: the whole plate from its front-right corner")
                 : face == 11 ? (ortho ? QStringLiteral("Orthographic: click for perspective")
                                       : QStringLiteral("Perspective: click for orthographic (no perspective)"))
                              : QString());
      update();
    }
    if (face >= 0) {
      setCursor(Qt::PointingHandCursor);
      return;
    }
    if (showFocus && focusHit(event->position())) {
      setCursor(Qt::SizeAllCursor);
      return;
    }
    if (tool == 8) {
      QVector3D hit;
      const bool hover = measurePointAt(event->position(), &hit);
      if (hover != measureHover || (hover && (hit - measureHoverPoint).lengthSquared() > 1e-6f)) {
        measureHover = hover;
        if (hover) measureHoverPoint = hit;
        update();
      }
      setCursor(Qt::CrossCursor);
      return;
    }
    if (tool == 10 || tool == 12 || tool == 13) {
      setCursor(Qt::CrossCursor);
      return;
    }
  }
  if (!alignBoxes.empty() && !dragging) {
    // Align: dots and parts are clickable; nothing is dragged.
    const int hover = alignHandleAt(event->position());
    if (hover != alignHover) {
      alignHover = hover;
      update();
    }
    setCursor(hover >= 0 || !pickId(event->position()).isEmpty() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    return;
  }
  if ((tool == 4 || tool == 9) && !dragging) {
    const int key = pickFilletEdge(event->position());
    if (key != hoverEdge) {
      hoverEdge = key;
      update();
    }
    setCursor(key >= 0 ? Qt::PointingHandCursor : Qt::CrossCursor);
    return;
  }
  if (!dragging || (axis < 0 && !freeArmed && !scaleDrag)) {
    int box = boxHandleAt(event->position());
    int hit = gizmoAxis(event->position());
    if (tool == 1 && hit >= 0) box = -1;
    else if (box >= 0) hit = -1;
    if (hit != hoverAxis || box != hoverHandle) {
      hoverAxis = hit;
      hoverHandle = box;
      update();
    }
    if (tool == 3 || tool == 6 || tool == 7) setCursor(Qt::CrossCursor);
    else if (box >= 0) setCursor(box < 8 ? Qt::SizeFDiagCursor : Qt::SizeAllCursor);
    else if (hit >= 0) setCursor(Qt::PointingHandCursor);
    else if (tool == 0 && !pickId(event->position()).isEmpty()) setCursor(Qt::SizeAllCursor);
    else if (!pickId(event->position()).isEmpty()) setCursor(Qt::ArrowCursor);
    else setCursor(Qt::ArrowCursor);
    return;
  }
  setCursor(scaleDrag ? (scaleHandle < 8 ? Qt::SizeFDiagCursor : Qt::SizeAllCursor) : Qt::ClosedHandCursor);
  QVector3D origin;
  QVector3D dir;
  cameraRay(event->position(), &origin, &dir);
  double pos[3] = {startPos[0], startPos[1], startPos[2]};
  double rot[3] = {startRot[0], startRot[1], startRot[2]};
  double scale[3] = {startScale[0], startScale[1], startScale[2]};
  if (scaleDrag) {
    auto placed = [](const double at[3], const double rot[3], const double sc[3], const QVector3D& p) {
      const QQuaternion q = QQuaternion::fromAxisAndAngle(0, 0, 1, rot[2]) * QQuaternion::fromAxisAndAngle(0, 1, 0, rot[1]) *
                            QQuaternion::fromAxisAndAngle(1, 0, 0, rot[0]);
      return QVector3D(at[0], at[1], at[2]) + q.rotatedVector(QVector3D(p.x() * sc[0], p.y() * sc[1], p.z() * sc[2]));
    };
    const QVector3D fixed = placed(startPos, startRot, startScale, scaleFix);
    const QVector3D startAt = placed(startPos, startRot, startScale, scaleGrab);
    const QVector3D span = startAt - fixed;
    const float len2 = span.lengthSquared();
    if (len2 < 1e-8f) return;

    // Project the current mouse ray onto the scale line. This keeps corner/axis
    // scaling stable regardless of camera angle and avoids relying on a 3D hit
    // point that may not exist for this drag mode.
    const float dirLen2 = dir.lengthSquared();
    float factor = 1.0f;
    if (dirLen2 > 1e-8f) {
      const QVector3D fromFixed = origin - fixed;
      const float b = QVector3D::dotProduct(dir, span);
      const float c = dirLen2;
      const float d = QVector3D::dotProduct(dir, fromFixed);
      const float e = QVector3D::dotProduct(span, fromFixed);
      const float denom = c * len2 - b * b;
      if (std::abs(denom) > 1e-8f) factor = (c * e - b * d) / denom;
      else factor = e / len2;
    }
    factor = std::max(0.05f, factor);
    auto component = [](const QVector3D& v, int index) -> double {
      return index == 0 ? v.x() : index == 1 ? v.y() : v.z();
    };
    if (scaleHandle < 8) {
      const double spanX = std::abs((component(scaleGrab, 0) - component(scaleFix, 0)) * startScale[0]);
      if (snapOn && spanX > 1e-4) {
        double snapped = std::round((spanX * factor) / snap) * snap;
        if (snapped < snap) snapped = snap;
        factor = static_cast<float>(snapped / spanX);
      }
      for (int i = 0; i < 3; ++i) scale[i] = startScale[i] * factor;
    } else {
      const int axisIndex = (scaleHandle - 8) / 2;
      const double spanA = std::abs((component(scaleGrab, axisIndex) - component(scaleFix, axisIndex)) * startScale[axisIndex]);
      if (snapOn && spanA > 1e-4) {
        double snapped = std::round((spanA * factor) / snap) * snap;
        if (snapped < snap) snapped = snap;
        factor = static_cast<float>(snapped / spanA);
      }
      scale[axisIndex] = startScale[axisIndex] * factor;
    }
    const double zero[3] = {0.0, 0.0, 0.0};
    const QVector3D shifted = placed(zero, startRot, scale, scaleFix);
    const QVector3D newPos = fixed - shifted;
    pos[0] = newPos.x();
    pos[1] = newPos.y();
    pos[2] = newPos.z();
    auto lowestZ = [&](const double at[3], const double rt[3], const double sc[3]) {
      double minZ = 1e9;
      const QQuaternion turned = QQuaternion::fromAxisAndAngle(0, 0, 1, rt[2]) * QQuaternion::fromAxisAndAngle(0, 1, 0, rt[1]) *
                                 QQuaternion::fromAxisAndAngle(1, 0, 0, rt[0]);
      for (int corner = 0; corner < 8; ++corner) {
        const QVector3D p((corner & 1) ? scaleHi.x() : scaleLo.x(), (corner & 2) ? scaleHi.y() : scaleLo.y(),
                          (corner & 4) ? scaleHi.z() : scaleLo.z());
        const QVector3D world = QVector3D(at[0], at[1], at[2]) + turned.rotatedVector(QVector3D(p.x() * sc[0], p.y() * sc[1], p.z() * sc[2]));
        minZ = std::min(minZ, static_cast<double>(world.z()));
      }
      return minZ;
    };
    const double oldBottom = lowestZ(startPos, startRot, startScale);
    pos[2] += oldBottom - lowestZ(pos, rot, scale);
    if (liveScaleDimensions) {
      liveDimensions[0] = scaleBaseExtents[0] * std::abs(scale[0]);
      liveDimensions[1] = scaleBaseExtents[1] * std::abs(scale[1]);
      liveDimensions[2] = scaleBaseExtents[2] * std::abs(scale[2]);
    }
  } else if (freeArmed) {
    if (std::abs(dir.z()) < 1e-5f) return;
    const float t = (freeGrab.z() - origin.z()) / dir.z();
    if (t < 0) return;
    const QVector3D hit = origin + dir * t;
    QVector3D world(dragPivot.x() + (hit.x() - freeGrab.x()), dragPivot.y() + (hit.y() - freeGrab.y()), dragPivot.z());
    if (snapOn) {
      world.setX(static_cast<float>(std::round(world.x() / snap) * snap));
      world.setY(static_cast<float>(std::round(world.y() / snap) * snap));
    }
    const QVector3D local = dragParent.inverted().map(world);
    pos[0] = local.x();
    pos[1] = local.y();
    pos[2] = local.z();
    freeDrag = true;
  } else if (tool == 0) {
    const float param = axisParam(origin, dir, dragPivot, dragAxis);
    QVector3D world = dragPivot + dragAxis * (param - dragParam);
    if (snapOn) {
      // Only along the arrow: the other two stay exactly where they were.
      for (int c = 0; c < 3; ++c)
        if (std::abs(dragAxis[c]) > 1e-3f) world[c] = static_cast<float>(std::round(world[c] / snap) * snap);
    }
    const QVector3D local = dragParent.inverted().map(world);
    pos[0] = local.x();
    pos[1] = local.y();
    pos[2] = local.z();
  } else if (tool == 1) {
    const float denom = QVector3D::dotProduct(dir, dragAxis);
    if (std::abs(denom) < 1e-5f) return;
    const float t = QVector3D::dotProduct(dragPivot - origin, dragAxis) / denom;
    const QVector3D hit = origin + dir * t;
    QVector3D a = dragHit - dragPivot;
    QVector3D b = hit - dragPivot;
    if (a.lengthSquared() < 1e-6f || b.lengthSquared() < 1e-6f) return;
    a.normalize();
    b.normalize();
    float angle = std::atan2(QVector3D::dotProduct(dragAxis, QVector3D::crossProduct(a, b)), QVector3D::dotProduct(a, b));
    if (rotationSnap > 0.0) {
      const double degrees = angle * 180.0 / 3.14159265358979323846;
      angle = static_cast<float>(std::round(degrees / rotationSnap) * rotationSnap * 3.14159265358979323846 / 180.0);
    }
    QQuaternion qx = QQuaternion::fromAxisAndAngle(1, 0, 0, rot[0]);
    QQuaternion qy = QQuaternion::fromAxisAndAngle(0, 1, 0, rot[1]);
    QQuaternion qz = QQuaternion::fromAxisAndAngle(0, 0, 1, rot[2]);
    QQuaternion local = qz * qy * qx;
    QVector3D localAxis(axis == 0 ? 1.f : 0.f, axis == 1 ? 1.f : 0.f, axis == 2 ? 1.f : 0.f);
    local = local * QQuaternion::fromAxisAndAngle(localAxis, angle * 180.f / 3.14159265f);
    QMatrix4x4 spun;
    spun.rotate(local);
    double ignoredPos[3];
    double ignoredScale[3];
    decomposePlacementNear(spun, startRot, ignoredPos, rot, ignoredScale);
  } else {
    const float param = axisParam(origin, dir, dragPivot, dragAxis);
    const double factor = std::max(0.05, 1.0 + static_cast<double>(param - dragParam) / 40.0);
    scale[axis] = startScale[axis] * factor;
  }
  // Moving (dragged or along an arrow) or sizing, not turning: the first step tells the window.
  if (!dragAnnounced && (freeArmed || scaleDrag || tool == 0)) {
    dragAnnounced = true;
    emit moveDragStarted();
  }
  emit transformed(dragId, QVector3D(pos[0], pos[1], pos[2]), QVector3D(rot[0], rot[1], rot[2]),
                  QVector3D(scale[0], scale[1], scale[2]));
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* event) {
  QOpenGLWidget::mouseDoubleClickEvent(event);  // a press, as before
  if (event->button() == Qt::LeftButton) pendingDoubleClick = pickId(event->position());
}

void Viewport::mouseReleaseEvent(QMouseEvent* event) {
  // A double-click is handed on after this release is done with, so whatever it opens does not
  // start with the button still down here.
  struct AfterRelease {
    Viewport* view;
    ~AfterRelease() {
      const QString id = std::exchange(view->pendingDoubleClick, QString());
      if (!id.isEmpty()) QTimer::singleShot(0, view, [v = view, id] { emit v->doubleClicked(id); });
    }
  } afterRelease{this};
  const bool wasDragging = dragging;
  const bool wasPanning = panning;
  const bool wasMarqueeing = marqueeing;
  const bool shortMove = (event->pos() - pressPos).manhattanLength() < 5;
  const bool wasFocus = focusDragging;
  const bool click = shortMove && event->button() == Qt::LeftButton && !wasDragging && !orbiting && !wasPanning && !wasMarqueeing && !wasFocus;
  const bool marqueeDrag = wasMarqueeing && event->button() == Qt::LeftButton && !shortMove;
  const bool marqueeTiny = wasMarqueeing && event->button() == Qt::LeftButton && shortMove;
  orbiting = false;
  panning = false;
  marqueeing = false;
  if (wasPanning) setCursor(spaceHeld ? Qt::OpenHandCursor : Qt::ArrowCursor);
  dragging = false;
  freeArmed = false;
  freeDrag = false;
  scaleDrag = false;
  focusDragging = false;
  liveScaleDimensions = false;
  axis = -1;
  if (wasFocus) {
    // Now glide the view over to the marker's new spot.
    beginNavigation();
    targetGoal = pendingFocus;
    if (!easeTimer->isActive()) easeTimer->start();
    return;
  }
  if (alignConsumed) {
    alignConsumed = false;
    return;
  }
  if (tool == 4 || tool == 6 || tool == 7 || tool == 8 || tool == 9 || tool == 10 || tool == 12 || tool == 13 || preview) return;
  if (showFocus && marqueeTiny) {
    moveFocusTo(event->position());
    return;
  }
  if (marqueeDrag) {
    marqueeCurrent = event->pos();
    const bool crossing = marqueeCurrent.x() < marqueeStart.x();
    emit marqueeSelected(marqueeIds(QRectF(marqueeStart, marqueeCurrent), crossing), event->modifiers());
    update();
    return;
  }
  if (click || marqueeTiny) emit picked(pickId(event->position()), event->modifiers());
}

// The mouse left the view: nothing is hovered any more (the size labels of a handle go away).
void Viewport::leaveEvent(QEvent* event) {
  if (!dragging && (hoverHandle >= 0 || hoverAxis >= 0)) {
    hoverHandle = -1;
    hoverAxis = -1;
    update();
  }
  QOpenGLWidget::leaveEvent(event);
}

void Viewport::wheelEvent(QWheelEvent* event) {
  // A notch zooms 8% (Ctrl: 2%), eased; touchpads and smooth wheels send smaller steps.
  focusIds.clear();
  beginNavigation();
  const float notches = event->angleDelta().y() / 120.f;
  const float step = event->modifiers().testFlag(Qt::ControlModifier) ? 0.02f : 0.08f;
  radiusGoal = std::clamp(radiusGoal * std::exp(-notches * step), 20.f, 1600.f);
  if (!easeTimer->isActive()) easeTimer->start();
  event->accept();
}

void Viewport::keyPressEvent(QKeyEvent* event) {
  if (event->modifiers() == Qt::NoModifier) {
    switch (event->key()) {
      case Qt::Key_Escape:
        if (tool == 8) {
          emit measureCleared();
          event->accept();
          return;
        }
        break;
      case Qt::Key_Left: emit nudgeRequested(-1, 0); event->accept(); return;
      case Qt::Key_Right: emit nudgeRequested(1, 0); event->accept(); return;
      case Qt::Key_Up: emit nudgeRequested(0, 1); event->accept(); return;
      case Qt::Key_Down: emit nudgeRequested(0, -1); event->accept(); return;
      default: break;
    }
  }
  QOpenGLWidget::keyPressEvent(event);
}

bool Viewport::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type type = event->type();
  if (type == QEvent::KeyPress || type == QEvent::KeyRelease) {
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->key() != Qt::Key_Space) return QOpenGLWidget::eventFilter(watched, event);
    if (type == QEvent::KeyPress) {
      // Typing a space in a text box stays a space; a dialog in front is left alone.
      QWidget* focus = QApplication::focusWidget();
      const bool typing = focus && (qobject_cast<QLineEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus) ||
                                    qobject_cast<QTextEdit*>(focus) || qobject_cast<QPlainTextEdit*>(focus));
      const bool ours = window()->isActiveWindow() && !QApplication::activeModalWidget();
      if (!spaceHeld && (typing || !ours || !underMouse())) return QOpenGLWidget::eventFilter(watched, event);
      if (!spaceHeld) {
        spaceHeld = true;
        if (!dragging) setCursor(Qt::OpenHandCursor);
      }
      return true;  // and not a click on whatever button has the focus
    }
    if (spaceHeld && !key->isAutoRepeat()) {
      spaceHeld = false;
      if (!panning) setCursor(Qt::ArrowCursor);
      return true;
    }
    return spaceHeld;
  }
  if (type == QEvent::ApplicationDeactivate && spaceHeld) {
    spaceHeld = false;
    if (!panning) setCursor(Qt::ArrowCursor);
  }
  return QOpenGLWidget::eventFilter(watched, event);
}

void Viewport::dragEnterEvent(QDragEnterEvent* event) {
  if (event->mimeData()->hasFormat("application/x-daedalcad")) event->acceptProposedAction();
}

void Viewport::dropEvent(QDropEvent* event) {
  const QString payload = QString::fromUtf8(event->mimeData()->data("application/x-daedalcad"));
  const QStringList parts = payload.split('|');
  if (parts.isEmpty()) return;
  const double fillet = (!parts.isEmpty() && parts[0].startsWith("stl:")) ? 0 : (parts.size() > 1 ? parts[1].toDouble() : 0);
  QVector3D origin;
  QVector3D dir;
  cameraRay(event->position(), &origin, &dir);
  double x = target.x();
  double y = target.y();
  if (faceWorkplane) {
    // On the workplane: where the ray meets its plane, as u, v along its axes.
    const float facing = QVector3D::dotProduct(dir, workplaneNormal);
    x = y = 0;
    if (std::abs(facing) > 1e-5f) {
      const float t = QVector3D::dotProduct(workplaneOrigin - origin, workplaneNormal) / facing;
      if (t > 0) {
        const QVector3D hit = origin + dir * t - workplaneOrigin;
        x = QVector3D::dotProduct(hit, workplaneX);
        y = QVector3D::dotProduct(hit, QVector3D::crossProduct(workplaneNormal, workplaneX));
      }
    }
  } else if (std::abs(dir.z()) > 1e-5f) {
    const double t = (workplaneZ - origin.z()) / dir.z();
    if (t > 0) {
      x = origin.x() + dir.x() * t;
      y = origin.y() + dir.y() * t;
    }
  }
  emit dropped(parts[0], x, y, fillet);
  event->acceptProposedAction();
}
