#include "sculptdialog.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QInputDialog>
#include <QRegularExpression>
#include <QSaveFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QFileDialog>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QStackedWidget>
#include <QSurfaceFormat>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace la = manifold::la;
using sculpt::Brush;
using sculpt::vec3;

namespace {

constexpr double kPi = 3.14159265358979323846;

const char* kVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNor;
layout(location = 2) in float aMask;
uniform mat4 uMvp;
uniform mat3 uView;
out vec3 vN;
out float vMask;
void main() {
  vN = uView * aNor;
  vMask = aMask;
  gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

// Clay, lit from the camera's upper left, with a soft rim: reads shapes the way Blender's sculpt
// view does.
const char* kFrag = R"(#version 330 core
in vec3 vN;
in float vMask;
uniform vec3 uColor;
out vec4 frag;
void main() {
  vec3 n = normalize(vN);
  if (!gl_FrontFacing) n = -n;
  vec3 key = normalize(vec3(-0.45, 0.55, 0.70));
  vec3 fill = normalize(vec3(0.65, -0.25, 0.45));
  float diffuse = max(dot(n, key), 0.0);
  float soft = max(dot(n, fill), 0.0) * 0.22;
  float rim = pow(1.0 - max(n.z, 0.0), 2.6) * 0.22;
  vec3 h = normalize(key + vec3(0.0, 0.0, 1.0));
  float spec = pow(max(dot(n, h), 0.0), 36.0) * 0.18;
  vec3 c = uColor * (0.20 + 0.78 * diffuse + soft) + vec3(rim + spec);
  // Masked: dark grey, as in Blender, so it is plain what the brushes leave alone.
  c = mix(c, c * 0.32, clamp(vMask, 0.0, 1.0));
  frag = vec4(c, 1.0);
}
)";

// Lines and see-through sheets in one colour; uBias pulls them a hair towards the viewer so a line
// lying on the surface is not hidden in it.
const char* kLineVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMvp;
uniform float uBias;
void main() {
  gl_Position = uMvp * vec4(aPos, 1.0);
  gl_Position.z -= uBias * gl_Position.w;
}
)";

const char* kLineFrag = R"(#version 330 core
uniform vec4 uColor;
out vec4 frag;
void main() { frag = uColor; }
)";

GLuint compile(QOpenGLFunctions_3_3_Core* gl, GLenum type, const char* source) {
  const GLuint shader = gl->glCreateShader(type);
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

QVector3D q(const vec3& v) { return QVector3D(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)); }
vec3 v3(const QVector3D& v) { return vec3(v.x(), v.y(), v.z()); }

// Every brush: its name, its key, what it does, and the group it is listed under.
struct BrushInfo {
  Brush brush;
  const char* name;
  Qt::Key key;
  const char* keyText;
  const char* tip;
  int group;  // 0 add and remove, 1 smooth and flatten, 2 move and shape
};

const BrushInfo kBrushes[] = {
    {Brush::Draw, "Draw", Qt::Key_1, "1", "Draw: raises the surface into a soft hill. Ctrl digs a hole or groove.", 0},
    {Brush::Clay, "Clay", Qt::Key_2, "2", "Clay: builds up in flat layers, like adding clay. Ctrl carves it away.", 0},
    {Brush::Layer, "Layer", Qt::Key_3, "3",
     "Layer: raises the surface by a set height and no further, however often you go over it: an even, raised "
     "plateau. Ctrl lowers it by the same height. The height grows with the radius and the strength.", 0},
    {Brush::Inflate, "Inflate", Qt::Key_4, "4", "Inflate: puffs the surface out in every direction. Ctrl shrinks it.", 0},
    {Brush::Blob, "Blob", Qt::Key_5, "5", "Blob: pushes out round, bulging blobs. Ctrl pushes them in.", 0},
    {Brush::Crease, "Crease", Qt::Key_6, "6", "Crease: cuts a sharp groove, pinching the sides together. Ctrl makes a sharp ridge.", 0},
    {Brush::Smooth, "Smooth", Qt::Key_7, "7", "Smooth: irons bumps away. Hold Shift with any brush to smooth.", 1},
    {Brush::Flatten, "Flatten", Qt::Key_8, "8", "Flatten: presses the surface flat. Ctrl makes it more pronounced.", 1},
    {Brush::Fill, "Fill", Qt::Key_9, "9", "Fill: fills in dips and hollows up to the surface around them. Ctrl deepens them.", 1},
    {Brush::Scrape, "Scrape", Qt::Key_0, "0", "Scrape: shaves bumps and peaks down to the surface around them. Ctrl raises them.", 1},
    {Brush::Grab, "Grab", Qt::Key_G, "G", "Grab: pulls a piece of the surface along with the mouse, like putty.", 2},
    {Brush::SnakeHook, "Snake hook", Qt::Key_K, "K",
     "Snake hook: drag out horns, tentacles and tails; the brush moves along with what it pulls. Remesh afterwards "
     "to even out the stretched triangles.", 2},
    {Brush::Pinch, "Pinch", Qt::Key_P, "P", "Pinch: pulls the surface together into a sharp crease or ridge. Ctrl spreads it.", 2},
    {Brush::Nudge, "Nudge", Qt::Key_N, "N", "Nudge: pushes the surface along the way you stroke, over the shape.", 2},
    {Brush::Rotate, "Rotate", Qt::Key_R, "R", "Rotate: twists a spot. Click it and drag round it.", 2},
    {Brush::Mask, "Mask", Qt::Key_M, "M",
     "Mask: paint over the parts to protect; they turn dark grey and every other brush leaves them alone. Ctrl wipes "
     "the mask off again.", 3},
    {Brush::Stamp, "Stamp", Qt::Key_S, "S",
     "Stamp: presses a picture onto the surface, raised (Ctrl: pressed in). Click for one, drag for a row. Pick the "
     "picture below; a finer detail (Remesh) makes it crisper.", 3},
};

// The built-in stamp pictures, 128 px square, white is high.
QImage stampPattern(const QString& key) {
  constexpr int size = 128;
  QImage image(size, size, QImage::Format_ARGB32);
  image.fill(Qt::black);
  auto field = [&](auto&& f) {
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        const double u = (x + 0.5) / size * 2 - 1, v = (y + 0.5) / size * 2 - 1;  // -1 .. 1
        const int g = static_cast<int>(std::clamp(f(u, v), 0.0, 1.0) * 255);
        image.setPixel(x, y, qRgb(g, g, g));
      }
  };
  auto dome = [](double r) { return r < 1 ? std::sqrt(1 - r * r) : 0.0; };
  if (key == QLatin1String("rivet")) {
    field([&](double u, double v) { return dome(std::hypot(u, v) / 0.7); });
  } else if (key == QLatin1String("ring")) {
    field([&](double u, double v) { return dome(std::abs(std::hypot(u, v) - 0.6) / 0.18); });
  } else if (key == QLatin1String("scales")) {
    // Overlapping round scales in rows, each row shifted half a scale.
    field([&](double u, double v) {
      const double cell = 0.5;
      double best = 0;
      for (int row = -3; row <= 3; ++row)
        for (int col = -3; col <= 3; ++col) {
          const double cx = (col + (row & 1 ? 0.5 : 0.0)) * cell, cy = row * cell * 0.75;
          const double r = std::hypot(u - cx, v - cy) / (cell * 0.62);
          best = std::max(best, dome(r) * (0.55 + 0.45 * std::clamp((cy - v) / cell + 0.5, 0.0, 1.0)));
        }
      return best;
    });
  } else if (key == QLatin1String("knurl")) {
    // Diamond knurling: little pyramids on a grid turned 45 degrees.
    field([&](double u, double v) {
      const double cell = 0.4;
      const double a = (u + v) / cell, b = (u - v) / cell;
      const double fa = a - std::floor(a) - 0.5, fb = b - std::floor(b) - 0.5;
      return 1 - 2 * std::max(std::abs(fa), std::abs(fb));
    });
  } else if (key == QLatin1String("bricks")) {
    field([&](double u, double v) {
      const double rowH = 0.5, brickW = 1.0, mortar = 0.06;
      const double y = (v + 1) / rowH;
      const int row = static_cast<int>(std::floor(y));
      const double x = (u + 1 + (row & 1 ? brickW / 2 : 0)) / brickW;
      const double dy = std::min(y - row, row + 1 - y) * rowH, dx = std::min(x - std::floor(x), std::floor(x) + 1 - x) * brickW;
      return std::clamp(std::min(dx, dy) / mortar - 1, 0.0, 1.0);
    });
  }
  if (key == QLatin1String("scales") || key == QLatin1String("knurl") || key == QLatin1String("bricks")) {
    // A tiled pattern fades out towards the edge of the square, so a patch blends into the
    // surface instead of standing up as a plate.
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        const double u = std::abs((x + 0.5) / size * 2 - 1), v = std::abs((y + 0.5) / size * 2 - 1);
        const double edge = std::clamp((1 - std::max(u, v)) / 0.25, 0.0, 1.0);
        const double fade = edge * edge * (3 - 2 * edge);
        const int g = static_cast<int>(qGray(image.pixel(x, y)) * fade);
        image.setPixel(x, y, qRgb(g, g, g));
      }
  }
  if (key == QLatin1String("star") || key == QLatin1String("heart")) {
    // Star and heart: flat, raised shapes with a soft edge.
    QPainterPath path;
    if (key == QLatin1String("heart")) {
      path.moveTo(64, 112);
      path.cubicTo(8, 72, 6, 30, 36, 22);
      path.cubicTo(52, 18, 62, 30, 64, 40);
      path.cubicTo(66, 30, 76, 18, 92, 22);
      path.cubicTo(122, 30, 120, 72, 64, 112);
    } else {
      for (int i = 0; i < 10; ++i) {
        const double angle = -kPi / 2 + i * kPi / 5;
        const double r = i % 2 ? 22 : 56;
        const QPointF p(64 + r * std::cos(angle), 66 + r * std::sin(angle));
        if (i == 0) path.moveTo(p);
        else path.lineTo(p);
      }
      path.closeSubpath();
    }
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    for (int grow = 6; grow >= 0; --grow) {
      // A few soft steps in from the edge, so the sides slope a little instead of standing straight.
      const int g = 255 - grow * 28;
      painter.setBrush(QColor(g, g, g));
      QPainterPathStroker stroker;
      stroker.setWidth(grow * 1.6);
      painter.drawPath(grow ? path.united(stroker.createStroke(path)) : path);
    }
  }
  return image;
}

}  // namespace

// --- The view ------------------------------------------------------------------------------------

SculptView::SculptView(sculpt::Sculptor* sculptor, QWidget* parent) : QOpenGLWidget(parent), sculptor_(sculptor) {
  QSurfaceFormat format;
  format.setDepthBufferSize(24);
  format.setSamples(4);
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  setFormat(format);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setCursor(Qt::CrossCursor);
  setMinimumSize(400, 300);
}

SculptView::~SculptView() {
  if (!glReady_) return;
  makeCurrent();
  glDeleteBuffers(1, &vbo_);
  glDeleteBuffers(1, &ebo_);
  glDeleteVertexArrays(1, &vao_);
  glDeleteProgram(program_);
  glDeleteBuffers(1, &lineVbo_);
  glDeleteVertexArrays(1, &lineVao_);
  glDeleteProgram(lineProgram_);
  doneCurrent();
}

void SculptView::setRadiusPx(int px) {
  radiusPx_ = std::clamp(px, 3, 600);
  update();
}

void SculptView::setView(double yawDegrees, double pitchDegrees) {
  yaw_ = static_cast<float>(yawDegrees * kPi / 180);
  pitch_ = static_cast<float>(std::clamp(pitchDegrees, -89.0, 89.0) * kPi / 180);
  update();
}

QImage SculptView::thumbnail(int size) {
  const QVector3D keepTarget = target_;
  const float keepDistance = distance_;
  const bool keepCursor = cursorIn_;
  cursorIn_ = false;
  frameAll();
  QImage image = grabFramebuffer();
  target_ = keepTarget;
  distance_ = keepDistance;
  cursorIn_ = keepCursor;
  update();
  const int side = std::min(image.width(), image.height());
  image = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side);
  return image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

void SculptView::frameAll() {
  vec3 lo, hi;
  sculpt::bounds(sculptor_->mesh(), &lo, &hi);
  target_ = q((lo + hi) * 0.5);
  const double radius = std::max(la::length(hi - lo) * 0.5, 1.0);
  distance_ = static_cast<float>(radius / std::sin(fov_ * 0.5 * kPi / 180) * 1.08);
  update();
}

QVector3D SculptView::eye() const {
  return target_ + distance_ * QVector3D(std::cos(pitch_) * std::cos(yaw_), std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_));
}

QMatrix4x4 SculptView::viewMatrix() const {
  QMatrix4x4 m;
  m.lookAt(eye(), target_, QVector3D(0, 0, 1));
  return m;
}

QMatrix4x4 SculptView::projection() const {
  QMatrix4x4 m;
  m.perspective(fov_, std::max(1.0f, float(width())) / std::max(1.0f, float(height())), distance_ * 0.01f, distance_ * 20.0f);
  return m;
}

bool SculptView::ray(const QPointF& pos, vec3* origin, vec3* dir) const {
  const QMatrix4x4 inverse = (projection() * viewMatrix()).inverted();
  const float x = static_cast<float>(2.0 * pos.x() / std::max(1, width()) - 1.0);
  const float y = static_cast<float>(1.0 - 2.0 * pos.y() / std::max(1, height()));
  const QVector3D nearPoint = inverse.map(QVector3D(x, y, -1));
  const QVector3D farPoint = inverse.map(QVector3D(x, y, 1));
  *origin = v3(nearPoint);
  *dir = la::normalize(v3(farPoint - nearPoint));
  return true;
}

double SculptView::worldPerPixel(const vec3& at) const {
  const QVector3D e = eye();
  const QVector3D forward = (target_ - e).normalized();
  const double depth = std::max(1e-3, static_cast<double>(QVector3D::dotProduct(q(at) - e, forward)));
  return 2 * depth * std::tan(fov_ * 0.5 * kPi / 180) / std::max(1, height());
}

void SculptView::initializeGL() {
  initializeOpenGLFunctions();
  const GLuint vs = compile(this, GL_VERTEX_SHADER, kVert);
  const GLuint fs = compile(this, GL_FRAGMENT_SHADER, kFrag);
  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glLinkProgram(program_);
  glDeleteShader(vs);
  glDeleteShader(fs);
  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);
  glGenBuffers(1, &ebo_);
  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), nullptr);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), reinterpret_cast<void*>(6 * sizeof(float)));
  glBindVertexArray(0);
  {
    const GLuint lvs = compile(this, GL_VERTEX_SHADER, kLineVert);
    const GLuint lfs = compile(this, GL_FRAGMENT_SHADER, kLineFrag);
    lineProgram_ = glCreateProgram();
    glAttachShader(lineProgram_, lvs);
    glAttachShader(lineProgram_, lfs);
    glLinkProgram(lineProgram_);
    glDeleteShader(lvs);
    glDeleteShader(lfs);
    glGenVertexArrays(1, &lineVao_);
    glGenBuffers(1, &lineVbo_);
    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindVertexArray(0);
  }
  glReady_ = true;
  uploaded_ = 0;
}

void SculptView::resizeGL(int, int) {}

void SculptView::upload() {
  const sculpt::Mesh& mesh = sculptor_->mesh();
  const auto& normals = sculptor_->normals();
  const auto& mask = sculptor_->mask();
  std::vector<float> data(mesh.pos.size() * 7);
  for (size_t i = 0; i < mesh.pos.size(); ++i) {
    float* d = &data[i * 7];
    d[0] = static_cast<float>(mesh.pos[i].x);
    d[1] = static_cast<float>(mesh.pos[i].y);
    d[2] = static_cast<float>(mesh.pos[i].z);
    d[3] = static_cast<float>(normals[i].x);
    d[4] = static_cast<float>(normals[i].y);
    d[5] = static_cast<float>(normals[i].z);
    d[6] = i < mask.size() ? mask[i] : 0.0f;
  }
  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  if (mesh.pos.size() == uploadedVerts_)
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data());
  else
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data(), GL_DYNAMIC_DRAW);
  // The triangles only change with a new mesh (remesh, undo of one), never during a stroke.
  if (!sculptor_->stroking() || mesh.tri.size() != uploadedTris_ || mesh.pos.size() != uploadedVerts_) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.tri.size() * sizeof(sculpt::Tri)), mesh.tri.data(),
                 GL_STATIC_DRAW);
    uploadedTris_ = mesh.tri.size();
  }
  uploadedVerts_ = mesh.pos.size();
  glBindVertexArray(0);
  uploaded_ = sculptor_->version();
}

void SculptView::paintGL() {
  glClearColor(0.105f, 0.125f, 0.150f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (!program_) return;
  if (uploaded_ != sculptor_->version()) upload();
  glEnable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glUseProgram(program_);
  const QMatrix4x4 view = viewMatrix();
  const QMatrix4x4 mvp = projection() * view;
  glUniformMatrix4fv(glGetUniformLocation(program_, "uMvp"), 1, GL_FALSE, mvp.constData());
  const QMatrix3x3 normalView = view.normalMatrix();
  glUniformMatrix3fv(glGetUniformLocation(program_, "uView"), 1, GL_FALSE, normalView.constData());
  glUniform3f(glGetUniformLocation(program_, "uColor"), 0.80f, 0.74f, 0.67f);
  glBindVertexArray(vao_);
  glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(uploadedTris_ * 3), GL_UNSIGNED_INT, nullptr);
  glBindVertexArray(0);
  glUseProgram(0);
  drawMirror(mvp);
  glDisable(GL_DEPTH_TEST);

  // The brush: a ring the size of its reach, coloured by what a click would do now.
  if ((cursorIn_ || drag_ == Drag::Stroke) && !readOnly_) {
    const Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();
    QColor ring(255, 150, 40);
    if (mods.testFlag(Qt::ShiftModifier) || brush_ == Brush::Smooth) ring = QColor(110, 210, 140);
    else if (mods.testFlag(Qt::ControlModifier)) ring = QColor(90, 165, 255);
    else if (brush_ == Brush::Mask) ring = QColor(225, 225, 225);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(ring, 1.6));
    painter.setBrush(Qt::NoBrush);
    if (brush_ == Brush::Stamp && !mods.testFlag(Qt::ShiftModifier)) {
      // The square the picture covers, with the picture faintly in it.
      const QRectF square(cursor_.x() - radiusPx_, cursor_.y() - radiusPx_, radiusPx_ * 2.0, radiusPx_ * 2.0);
      if (!stampPreview_.isNull()) {
        painter.setOpacity(0.35);
        painter.drawImage(square, stampPreview_);
        painter.setOpacity(1.0);
      }
      painter.drawRect(square);
    } else {
      painter.drawEllipse(cursor_, radiusPx_, radiusPx_);
    }
    painter.setPen(QPen(ring, 1.0));
    painter.drawEllipse(cursor_, 1.5, 1.5);
  }
}

void SculptView::drawMirror(const QMatrix4x4& mvp) {
  const bool radial = radial_ > 1;
  const int mask = (mirror_[0] ? 1 : 0) | (mirror_[1] ? 2 : 0) | (mirror_[2] ? 4 : 0) | (radial ? radial_ << 3 : 0) |
                   (radialAxis_ << 10);
  if (!(mask & 7) && !radial) return;
  if (!lineProgram_) return;
  if (mask != mirrorMask_ || mirrorVersion_ != sculptor_->version()) {
    // Where each plane cuts the surface (a segment per triangle it crosses), and a sheet a little
    // larger than the shape, with its rim.
    for (int axis = 0; axis < 3; ++axis) {
      mirrorCut_[axis].clear();
      mirrorSheet_[axis].clear();
      mirrorRim_[axis].clear();
    }
    const sculpt::Mesh& mesh = sculptor_->mesh();
    vec3 lo, hi;
    sculpt::bounds(mesh, &lo, &hi);
    const vec3 pad = (hi - lo) * 0.08 + vec3(1, 1, 1);
    lo -= pad;
    hi += pad;
    auto put = [](std::vector<float>& out, const vec3& p) {
      out.push_back(static_cast<float>(p.x));
      out.push_back(static_cast<float>(p.y));
      out.push_back(static_cast<float>(p.z));
    };
    for (int axis = 0; axis < 3; ++axis) {
      if (!mirror_[axis]) continue;
      const double at = mirrorCenter_[axis];
      for (const sculpt::Tri& t : mesh.tri) {
        vec3 ends[2];
        int found = 0;
        for (int i = 0; i < 3 && found < 2; ++i) {
          const vec3& a = mesh.pos[t[i]];
          const vec3& b = mesh.pos[t[(i + 1) % 3]];
          const double da = a[axis] - at, db = b[axis] - at;
          if ((da < 0) == (db < 0)) continue;
          ends[found++] = a + (b - a) * (da / (da - db));
        }
        if (found == 2) {
          put(mirrorCut_[axis], ends[0]);
          put(mirrorCut_[axis], ends[1]);
        }
      }
      const int u = (axis + 1) % 3, v = (axis + 2) % 3;
      auto corner = [&](bool cu, bool cv) {
        vec3 p;
        p[axis] = at;
        p[u] = cu ? hi[u] : lo[u];
        p[v] = cv ? hi[v] : lo[v];
        return p;
      };
      const vec3 c00 = corner(false, false), c10 = corner(true, false), c11 = corner(true, true), c01 = corner(false, true);
      for (const vec3& p : {c00, c10, c11, c00, c11, c01}) put(mirrorSheet_[axis], p);
      for (const vec3& p : {c00, c10, c10, c11, c11, c01, c01, c00}) put(mirrorRim_[axis], p);
    }
    // Radial: the axis, and the lines where the slices between the copies meet the surface.
    radialAxisLine_.clear();
    radialCut_.clear();
    if (radial) {
      const int axis = radialAxis_;
      vec3 along(0, 0, 0);
      along[axis] = 1;
      vec3 a = mirrorCenter_, b = mirrorCenter_;
      a[axis] = lo[axis];
      b[axis] = hi[axis];
      put(radialAxisLine_, a);
      put(radialAxisLine_, b);
      vec3 first(0, 0, 0);
      first[(axis + 1) % 3] = 1;
      for (int k = 0; k < radial_; ++k) {
        // Half-way between two copies, so the lines frame the slice one stroke covers.
        const double angle = 2 * kPi * (k + 0.5) / radial_;
        const vec3 out = first * std::cos(angle) + la::cross(along, first) * std::sin(angle);
        const vec3 nrm = la::cross(along, out);
        for (const sculpt::Tri& t : mesh.tri) {
          vec3 ends[2];
          int found = 0;
          for (int i = 0; i < 3 && found < 2; ++i) {
            const vec3& p = mesh.pos[t[i]];
            const vec3& q = mesh.pos[t[(i + 1) % 3]];
            const double dp = la::dot(p - mirrorCenter_, nrm), dq = la::dot(q - mirrorCenter_, nrm);
            if ((dp < 0) == (dq < 0)) continue;
            ends[found++] = p + (q - p) * (dp / (dp - dq));
          }
          if (found == 2 && la::dot((ends[0] + ends[1]) * 0.5 - mirrorCenter_, out) > 0) {
            put(radialCut_, ends[0]);
            put(radialCut_, ends[1]);
          }
        }
      }
    }
    mirrorMask_ = mask;
    mirrorVersion_ = sculptor_->version();
  }
  glUseProgram(lineProgram_);
  glUniformMatrix4fv(glGetUniformLocation(lineProgram_, "uMvp"), 1, GL_FALSE, mvp.constData());
  const GLint color = glGetUniformLocation(lineProgram_, "uColor");
  const GLint bias = glGetUniformLocation(lineProgram_, "uBias");
  glBindVertexArray(lineVao_);
  glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
  auto draw = [&](const std::vector<float>& data, GLenum mode) {
    if (data.empty()) return;
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data(), GL_STREAM_DRAW);
    glDrawArrays(mode, 0, static_cast<GLsizei>(data.size() / 3));
  };
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDepthMask(GL_FALSE);
  // In the axis colours of the main view: X red, Y green, Z blue.
  const float tint[3][3] = {{0.95f, 0.36f, 0.32f}, {0.45f, 0.85f, 0.38f}, {0.38f, 0.64f, 1.0f}};
  for (int axis = 0; axis < 3; ++axis) {
    if (!mirror_[axis]) continue;
    const float* c = tint[axis];
    // The sheet: faint, and fainter still where the shape is in front of it.
    glUniform1f(bias, 0.0f);
    glUniform4f(color, c[0], c[1], c[2], 0.10f);
    draw(mirrorSheet_[axis], GL_TRIANGLES);
    glUniform4f(color, c[0], c[1], c[2], 0.55f);
    draw(mirrorRim_[axis], GL_LINES);
    // The line on the surface: bright where it can be seen, dim where it runs round the back.
    glUniform1f(bias, 0.0004f);
    glDepthFunc(GL_GREATER);
    glUniform4f(color, c[0], c[1], c[2], 0.25f);
    draw(mirrorCut_[axis], GL_LINES);
    glDepthFunc(GL_LEQUAL);
    glUniform4f(color, std::min(1.0f, c[0] + 0.1f), std::min(1.0f, c[1] + 0.1f), std::min(1.0f, c[2] + 0.1f), 1.0f);
    draw(mirrorCut_[axis], GL_LINES);
    glDepthFunc(GL_LESS);
  }
  if (radial) {
    const float* c = tint[radialAxis_];
    glUniform1f(bias, 0.0f);
    glUniform4f(color, c[0], c[1], c[2], 0.9f);
    draw(radialAxisLine_, GL_LINES);
    glUniform1f(bias, 0.0004f);
    glDepthFunc(GL_LEQUAL);
    glUniform4f(color, c[0], c[1], c[2], 0.7f);
    draw(radialCut_, GL_LINES);
    glDepthFunc(GL_LESS);
  }
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glBindVertexArray(0);
  glUseProgram(0);
}

bool SculptView::beginStroke(const QPointF& pos, Qt::KeyboardModifiers mods) {
  vec3 origin, dir, hit;
  ray(pos, &origin, &dir);
  if (!sculptor_->raycast(origin, dir, &hit)) return false;
  sculpt::Stroke stroke;
  stroke.brush = mods.testFlag(Qt::ShiftModifier) ? Brush::Smooth : brush_;
  stroke.invert = mods.testFlag(Qt::ControlModifier);
  stroke.radius = radiusPx_ * worldPerPixel(hit);
  stroke.strength = strength_;
  stroke.falloff = falloff_;
  for (int axis = 0; axis < 3; ++axis) stroke.mirror[axis] = mirror_[axis];
  stroke.mirrorCenter = mirrorCenter_;
  stroke.radial = radial_;
  stroke.radialAxis = radialAxis_;
  stroke.stamp = stamp_;
  {
    const QVector3D forward = (target_ - eye()).normalized();
    const QVector3D right = QVector3D::crossProduct(forward, QVector3D(0, 0, 1)).normalized();
    stroke.up = v3(QVector3D::crossProduct(right, forward));
  }
  sculptor_->beginStroke(stroke, hit, dir);
  grabbing_ = sculpt::dragBrush(stroke.brush);
  grabPoint_ = hit;
  grabNormal_ = v3((target_ - eye()).normalized());
  lastDab_ = pos;
  update();
  return true;
}

void SculptView::continueStroke(const QPointF& pos) {
  if (grabbing_) {
    vec3 origin, dir;
    ray(pos, &origin, &dir);
    const double along = la::dot(dir, grabNormal_);
    if (std::abs(along) < 1e-9) return;
    const double t = la::dot(grabPoint_ - origin, grabNormal_) / along;
    sculptor_->grabTo(origin + dir * t);
    update();
    return;
  }
  // Dabs every so far along the mouse path (closer for a small brush), each where the surface
  // is under that point.
  // Stamps go one picture-width apart, so a drag lays a row of them.
  const double spacing = brush_ == Brush::Stamp ? radiusPx_ * 2.0 : std::max(1.5, radiusPx_ * 0.15);
  QPointF from = lastDab_;
  const QPointF step = pos - from;
  double length = std::hypot(step.x(), step.y());
  if (length < spacing) return;
  const QPointF unit = step / length;
  while (length >= spacing) {
    from += unit * spacing;
    length -= spacing;
    vec3 origin, dir, hit;
    ray(from, &origin, &dir);
    if (sculptor_->raycast(origin, dir, &hit)) sculptor_->dab(hit);
  }
  lastDab_ = from;
  update();
}

void SculptView::finishStroke() {
  sculptor_->endStroke();
  grabbing_ = false;
  update();
  emit edited();
}

void SculptView::mousePressEvent(QMouseEvent* event) {
  last_ = event->position();
  cursor_ = event->position();
  setFocus();
  if (event->button() == Qt::LeftButton && !event->modifiers().testFlag(Qt::AltModifier) && !readOnly_) {
    // On the surface it sculpts; beside it, it turns the view (as in Blender).
    drag_ = beginStroke(event->position(), event->modifiers()) ? Drag::Stroke : Drag::Orbit;
    return;
  }
  if (event->button() == Qt::RightButton || event->button() == Qt::LeftButton) drag_ = Drag::Orbit;
  else if (event->button() == Qt::MiddleButton) drag_ = Drag::Pan;
}

void SculptView::mouseMoveEvent(QMouseEvent* event) {
  const QPointF pos = event->position();
  const QPointF delta = pos - last_;
  last_ = pos;
  cursor_ = pos;
  cursorIn_ = true;
  switch (drag_) {
    case Drag::Stroke:
      continueStroke(pos);
      break;
    case Drag::Orbit:
      yaw_ -= static_cast<float>(delta.x()) * 0.008f;
      pitch_ = std::clamp(pitch_ + static_cast<float>(delta.y()) * 0.008f, -1.55f, 1.55f);
      update();
      break;
    case Drag::Pan: {
      const double scale = worldPerPixel(v3(target_));
      const QVector3D forward = (target_ - eye()).normalized();
      const QVector3D right = QVector3D::crossProduct(forward, QVector3D(0, 0, 1)).normalized();
      const QVector3D up = QVector3D::crossProduct(right, forward);
      target_ += (-right * static_cast<float>(delta.x()) + up * static_cast<float>(delta.y())) * static_cast<float>(scale);
      update();
      break;
    }
    case Drag::None:
      update();
      break;
  }
}

void SculptView::mouseReleaseEvent(QMouseEvent*) {
  if (drag_ == Drag::Stroke) finishStroke();
  drag_ = Drag::None;
}

void SculptView::wheelEvent(QWheelEvent* event) {
  const double steps = event->angleDelta().y() / 120.0;
  distance_ = static_cast<float>(std::clamp(distance_ * std::pow(0.87, steps), 1.0, 5000.0));
  update();
}

void SculptView::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_BracketLeft || event->key() == Qt::Key_BracketRight) {
    const int step = std::max(2, radiusPx_ / 8);
    setRadiusPx(radiusPx_ + (event->key() == Qt::Key_BracketRight ? step : -step));
    emit radiusChanged(radiusPx_);
    return;
  }
  if (event->key() == Qt::Key_F) {
    frameAll();
    return;
  }
  update();  // Ctrl / Shift change the ring's colour
  QOpenGLWidget::keyPressEvent(event);
}

void SculptView::keyReleaseEvent(QKeyEvent* event) {
  update();
  QOpenGLWidget::keyReleaseEvent(event);
}

void SculptView::leaveEvent(QEvent* event) {
  cursorIn_ = false;
  update();
  QOpenGLWidget::leaveEvent(event);
}

// --- The window ----------------------------------------------------------------------------------

SculptDialog::SculptDialog(QWidget* parent) : QDialog(parent) { build(true); }

SculptDialog::SculptDialog(QWidget* parent, sculpt::Mesh mesh, const QString& name, bool hasOriginal)
    : QDialog(parent), name_(name), hasOriginal_(hasOriginal) {
  build(false);
  begin(std::move(mesh));
}

SculptDialog::~SculptDialog() = default;

void SculptDialog::build(bool startPage) {
  setWindowTitle(name_.isEmpty() ? QStringLiteral("Sculpt") : QStringLiteral("Sculpt – %1").arg(name_));
  setWindowFlags(windowFlags() | Qt::WindowMaximizeButtonHint | Qt::WindowMinimizeButtonHint);
  resize(1200, 800);
  auto* layout = new QVBoxLayout(this);
  pages_ = new QStackedWidget(this);
  layout->addWidget(pages_);

  // Start page: the shape to begin with.
  startPage_ = new QWidget(pages_);
  {
    auto* outer = new QVBoxLayout(startPage_);
    outer->addStretch(1);
    auto* box = new QGroupBox(QStringLiteral("Start from"), startPage_);
    box->setMaximumWidth(560);
    auto* form = new QVBoxLayout(box);
    auto* row = new QHBoxLayout;
    shapes_ = new QButtonGroup(this);
    const QStringList kinds = {"ball", "cube", "cylinder", "slab", "capsule"};
    const QStringList labels = {"Ball", "Cube", "Cylinder", "Slab", "Capsule"};
    for (int i = 0; i < kinds.size(); ++i) {
      auto* button = new QToolButton(box);
      button->setText(labels[i]);
      button->setCheckable(true);
      button->setMinimumSize(92, 48);
      button->setProperty("kind", kinds[i]);
      shapes_->addButton(button, i);
      row->addWidget(button);
    }
    shapes_->button(0)->setChecked(true);
    form->addLayout(row);
    auto* fields = new QFormLayout;
    startSize_ = new QDoubleSpinBox(box);
    startSize_->setRange(5, 300);
    startSize_->setValue(40);
    startSize_->setSuffix(QStringLiteral(" mm"));
    startDetail_ = new QDoubleSpinBox(box);
    startDetail_->setRange(0.1, 10);
    startDetail_->setDecimals(2);
    startDetail_->setSingleStep(0.1);
    startDetail_->setValue(0.6);
    startDetail_->setSuffix(QStringLiteral(" mm"));
    startDetail_->setToolTip(QStringLiteral("How long the triangles' edges are. Smaller holds finer detail but is slower."));
    fields->addRow(QStringLiteral("Size"), startSize_);
    fields->addRow(QStringLiteral("Detail"), startDetail_);
    form->addLayout(fields);
    startEstimate_ = new QLabel(box);
    form->addWidget(startEstimate_);
    auto* go = new QPushButton(QStringLiteral("Start sculpting"), box);
    go->setDefault(true);
    form->addWidget(go);
    auto* center = new QHBoxLayout;
    center->addStretch(1);
    center->addWidget(box);
    center->addStretch(1);
    outer->addLayout(center);
    outer->addStretch(2);
    auto estimate = [this] {
      const double s = startSize_->value(), e = startDetail_->value();
      const QString kind = shapes_->checkedButton() ? shapes_->checkedButton()->property("kind").toString() : QString();
      const double area = kind == "cube" ? 6 * s * s
                          : kind == "cylinder" ? 1.5 * kPi * s * s
                          : kind == "slab" ? 2.5 * s * s
                          : kind == "capsule" ? 0.5 * kPi * s * s
                                              : kPi * s * s;
      const double tris = area / (0.433 * e * e);
      startEstimate_->setText(QStringLiteral("About %L1 triangles%2").arg(static_cast<qlonglong>(std::round(tris / 1000) * 1000))
                                  .arg(tris > 1.5e6 ? QStringLiteral(": that is a lot, sculpting may feel slow") : QString()));
    };
    connect(startSize_, &QDoubleSpinBox::valueChanged, this, [this, estimate](double size) {
      startDetail_->setValue(std::clamp(std::round(size / 60 * 20) / 20, 0.15, 3.0));
      estimate();
    });
    connect(startDetail_, &QDoubleSpinBox::valueChanged, this, estimate);
    connect(shapes_, &QButtonGroup::idClicked, this, estimate);
    connect(go, &QPushButton::clicked, this, [this] {
      const QString kind = shapes_->checkedButton()->property("kind").toString();
      startWith(kind, startSize_->value(), startDetail_->value());
    });
    estimate();
  }
  pages_->addWidget(startPage_);

  // Sculpt page: the tools on the left, the view on the right.
  sculptPage_ = new QWidget(pages_);
  {
    auto* row = new QHBoxLayout(sculptPage_);
    row->setContentsMargins(0, 0, 0, 0);
    // The tools scroll if the window is short; Save and Done stay put at the bottom.
    auto* column = new QWidget(sculptPage_);
    column->setFixedWidth(282);
    auto* columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    auto* sideScroll = new QScrollArea(column);
    sideScroll->setWidgetResizable(true);
    sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideScroll->setFrameShape(QFrame::NoFrame);
    auto* side = new QWidget(sideScroll);
    auto* panel = new QVBoxLayout(side);
    panel->setContentsMargins(0, 0, 8, 0);
    auto* bottom = new QWidget(column);
    auto* bottomLayout = new QVBoxLayout(bottom);
    bottomLayout->setContentsMargins(0, 4, 8, 0);

    auto* brushBox = new QGroupBox(QStringLiteral("Brush"), side);
    auto* brushLayout = new QVBoxLayout(brushBox);
    brushLayout->setSpacing(3);
    brushes_ = new QButtonGroup(this);
    const char* groups[] = {"Add and remove", "Smooth and flatten", "Move and shape", "Mask and stamp"};
    for (int group = 0; group < 4; ++group) {
      auto* caption = new QLabel(QString::fromLatin1(groups[group]), brushBox);
      caption->setStyleSheet(QStringLiteral("color: #b9c1cc; font-size: 11px; margin-top: 4px;"));
      brushLayout->addWidget(caption);
      auto* grid = new QGridLayout;
      grid->setSpacing(4);
      int at = 0;
      for (const BrushInfo& info : kBrushes) {
        if (info.group != group) continue;
        auto* button = new QToolButton(brushBox);
        button->setText(QStringLiteral("%1  %2").arg(QString::fromLatin1(info.keyText), QString::fromLatin1(info.name)));
        button->setToolTip(QStringLiteral("%1 (key %2)").arg(QString::fromLatin1(info.tip), QString::fromLatin1(info.keyText)));
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(28);
        brushes_->addButton(button, static_cast<int>(info.brush));
        grid->addWidget(button, at / 2, at % 2);
        ++at;
      }
      brushLayout->addLayout(grid);
    }
    {
      auto* maskRow = new QHBoxLayout;
      auto* clearMask = new QPushButton(QStringLiteral("Clear mask"), brushBox);
      auto* invertMask = new QPushButton(QStringLiteral("Invert mask"), brushBox);
      invertMask->setToolTip(QStringLiteral("Masked becomes free and free becomes masked: mask the part to work on, then invert."));
      maskRow->addWidget(clearMask);
      maskRow->addWidget(invertMask);
      brushLayout->addLayout(maskRow);
      connect(clearMask, &QPushButton::clicked, this, [this] {
        sculptor_.clearMask();
        view_->update();
        refresh();
      });
      connect(invertMask, &QPushButton::clicked, this, [this] {
        sculptor_.invertMask();
        view_->update();
        refresh();
      });
    }
    brushes_->button(static_cast<int>(Brush::Draw))->setChecked(true);
    connect(brushes_, &QButtonGroup::idClicked, this, [this](int id) { selectBrush(static_cast<Brush>(id)); });
    panel->addWidget(brushBox);

    // The Stamp brush's picture: shown while Stamp is picked.
    stampBox_ = new QGroupBox(QStringLiteral("Stamp picture"), side);
    {
      auto* stampLayout = new QVBoxLayout(stampBox_);
      stampPicker_ = new QComboBox(stampBox_);
      stampPicker_->setIconSize(QSize(32, 32));
      for (const auto& [key, label] : std::vector<std::pair<QString, QString>>{{"rivet", "Rivet"},
                                                                                {"ring", "Ring"},
                                                                                {"star", "Star"},
                                                                                {"heart", "Heart"},
                                                                                {"scales", "Scales"},
                                                                                {"knurl", "Knurling"},
                                                                                {"bricks", "Bricks"}}) {
        const QImage picture = stampPattern(key);
        stampPicker_->addItem(QIcon(QPixmap::fromImage(picture.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation))),
                              label, key);
      }
      stampLayout->addWidget(stampPicker_);
      auto* own = new QPushButton(QStringLiteral("Your picture…"), stampBox_);
      own->setToolTip(QStringLiteral("A PNG, JPG or SVG: a logo, a texture, a face. Light parts stand out; transparent "
                                     "parts are left alone."));
      stampLayout->addWidget(own);
      stampDark_ = new QCheckBox(QStringLiteral("Dark parts stand out"), stampBox_);
      stampDark_->setToolTip(QStringLiteral("For a dark logo on a light background."));
      stampLayout->addWidget(stampDark_);
      connect(stampPicker_, &QComboBox::currentIndexChanged, this, [this] { updateStamp(); });
      connect(stampDark_, &QCheckBox::toggled, this, [this] { updateStamp(); });
      connect(own, &QPushButton::clicked, this, [this] { chooseStampPicture(); });
    }
    stampBox_->setVisible(false);
    panel->addWidget(stampBox_);

    auto* settings = new QGroupBox(QStringLiteral("Settings"), side);
    auto* form = new QFormLayout(settings);
    radius_ = new QSlider(Qt::Horizontal, settings);
    radius_->setRange(5, 400);
    radius_->setValue(50);
    radiusText_ = new QLabel(settings);
    auto* radiusRow = new QHBoxLayout;
    radiusRow->addWidget(radius_, 1);
    radiusRow->addWidget(radiusText_);
    form->addRow(QStringLiteral("Radius"), radiusRow);
    strength_ = new QSlider(Qt::Horizontal, settings);
    strength_->setRange(1, 100);
    strength_->setValue(50);
    strengthText_ = new QLabel(settings);
    auto* strengthRow = new QHBoxLayout;
    strengthRow->addWidget(strength_, 1);
    strengthRow->addWidget(strengthText_);
    form->addRow(QStringLiteral("Strength"), strengthRow);
    falloff_ = new QComboBox(settings);
    falloff_->addItems({QStringLiteral("Smooth"), QStringLiteral("Sharp"), QStringLiteral("Flat")});
    falloff_->setToolTip(QStringLiteral("How the brush fades towards its rim: smooth hills, sharp points, or flat with a soft edge."));
    form->addRow(QStringLiteral("Falloff"), falloff_);
    auto* mirrorRow = new QHBoxLayout;
    const char* axes[] = {"X", "Y", "Z"};
    for (int axis = 0; axis < 3; ++axis) {
      mirror_[axis] = new QToolButton(settings);
      mirror_[axis]->setText(QString::fromLatin1(axes[axis]));
      mirror_[axis]->setCheckable(true);
      mirror_[axis]->setMinimumSize(40, 26);
      mirror_[axis]->setToolTip(QStringLiteral("Whatever the brush does on one side happens on the other side of the %1 "
                                               "middle too.").arg(QString::fromLatin1(axes[axis])));
      mirrorRow->addWidget(mirror_[axis]);
      connect(mirror_[axis], &QToolButton::toggled, this, [this, axis](bool on) { view_->setMirror(axis, on); });
    }
    mirrorRow->addStretch(1);
    form->addRow(QStringLiteral("Mirror"), mirrorRow);
    auto* radialRow = new QHBoxLayout;
    radial_ = new QSpinBox(settings);
    radial_->setRange(1, 16);
    radial_->setSpecialValueText(QStringLiteral("Off"));
    radial_->setToolTip(QStringLiteral("Repeat every stroke this many times around an axis through the middle: knobs, "
                                       "flowers, rosettes, vases."));
    radialAxis_ = new QComboBox(settings);
    radialAxis_->addItems({QStringLiteral("around X"), QStringLiteral("around Y"), QStringLiteral("around Z")});
    radialAxis_->setCurrentIndex(2);
    radialRow->addWidget(radial_);
    radialRow->addWidget(radialAxis_, 1);
    form->addRow(QStringLiteral("Radial"), radialRow);
    auto applyRadial = [this] { view_->setRadial(radial_->value(), radialAxis_->currentIndex()); };
    connect(radial_, &QSpinBox::valueChanged, this, applyRadial);
    connect(radialAxis_, &QComboBox::currentIndexChanged, this, applyRadial);
    panel->addWidget(settings);

    auto* meshBox = new QGroupBox(QStringLiteral("Mesh"), side);
    auto* meshForm = new QFormLayout(meshBox);
    detail_ = new QDoubleSpinBox(meshBox);
    detail_->setRange(0.1, 10);
    detail_->setDecimals(2);
    detail_->setSingleStep(0.1);
    detail_->setSuffix(QStringLiteral(" mm"));
    detail_->setToolTip(QStringLiteral("Edge length of the triangles after a remesh. Smaller holds finer detail."));
    meshForm->addRow(QStringLiteral("Detail"), detail_);
    auto* remesh = new QPushButton(QStringLiteral("Remesh"), meshBox);
    remesh->setToolTip(QStringLiteral("Rebuild the surface as even triangles at this detail: after heavy pulling, or to add or "
                                      "remove detail. Overlapping parts get joined."));
    meshForm->addRow(remesh);
    info_ = new QLabel(meshBox);
    info_->setWordWrap(true);
    meshForm->addRow(info_);
    panel->addWidget(meshBox);

    auto* history = new QHBoxLayout;
    undo_ = new QPushButton(QStringLiteral("Undo"), side);
    redo_ = new QPushButton(QStringLiteral("Redo"), side);
    history->addWidget(undo_);
    history->addWidget(redo_);
    panel->addLayout(history);

    auto* help = new QLabel(QStringLiteral("<b>Left-drag</b> on the shape: sculpt<br>"
                                           "<b>Ctrl</b>: the other way (dig, carve)<br>"
                                           "<b>Shift</b>: smooth<br>"
                                           "<b>[ ]</b>: smaller / larger brush<br>"
                                           "<b>1</b>–<b>0</b>, <b>G K P N R M S</b>: pick a brush<br>"
                                           "<b>Right-drag</b> or drag beside it: turn<br>"
                                           "<b>Middle-drag</b>: move &nbsp; <b>Wheel</b>: zoom<br>"
                                           "<b>F</b>: fit to view &nbsp; <b>Ctrl+Z</b>: undo"),
                               side);
    help->setWordWrap(true);
    help->setStyleSheet(QStringLiteral("color: #c3cad4;"));
    panel->addWidget(help);
    panel->addStretch(1);

    auto* libraryBox = new QGroupBox(QStringLiteral("Sculpts library"), side);
    auto* libraryRow = new QHBoxLayout(libraryBox);
    save_ = new QPushButton(QStringLiteral("Save"), libraryBox);
    save_->setToolTip(QStringLiteral("Keep this sculpt in the Sculpts library, to place it or work on it again later. "
                                     "The first time it asks for a name."));
    saveAs_ = new QPushButton(QStringLiteral("Save as…"), libraryBox);
    saveAs_->setToolTip(QStringLiteral("Save it in the library under a new name (the one there stays as it is)."));
    libraryRow->addWidget(save_);
    libraryRow->addWidget(saveAs_);
    bottomLayout->addWidget(libraryBox);
    connect(save_, &QPushButton::clicked, this, [this] { saveToLibrary(false); });
    connect(saveAs_, &QPushButton::clicked, this, [this] { saveToLibrary(true); });

    auto* buttons = new QDialogButtonBox(side);
    done_ = buttons->addButton(QStringLiteral("Done"), QDialogButtonBox::AcceptRole);
    cancel_ = buttons->addButton(QDialogButtonBox::Cancel);
    done_->setDefault(false);
    done_->setAutoDefault(false);
    bottomLayout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { finish(); });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { reject(); });

    view_ = new SculptView(&sculptor_, sculptPage_);
    sideScroll->setWidget(side);
    columnLayout->addWidget(sideScroll, 1);
    columnLayout->addWidget(bottom);
    row->addWidget(column);
    row->addWidget(view_, 1);

    connect(radius_, &QSlider::valueChanged, this, [this](int px) {
      view_->setRadiusPx(px);
      radiusText_->setText(QStringLiteral("%1 px").arg(px));
    });
    connect(view_, &SculptView::radiusChanged, this, [this](int px) {
      const QSignalBlocker block(radius_);
      radius_->setValue(px);
      radiusText_->setText(QStringLiteral("%1 px").arg(px));
    });
    connect(strength_, &QSlider::valueChanged, this, [this](int value) {
      view_->setStrength(value / 100.0);
      strengthText_->setText(QString::number(value / 100.0, 'f', 2));
    });
    connect(falloff_, &QComboBox::currentIndexChanged, this,
            [this](int index) { view_->setFalloff(static_cast<sculpt::Falloff>(index)); });
    connect(view_, &SculptView::edited, this, [this] { refresh(); });
    connect(undo_, &QPushButton::clicked, this, [this] {
      sculptor_.undo();
      view_->update();
      refresh();
    });
    connect(redo_, &QPushButton::clicked, this, [this] {
      sculptor_.redo();
      view_->update();
      refresh();
    });
    connect(remesh, &QPushButton::clicked, this, [this] {
      const double estimate = sculpt::remeshTriangleEstimate(sculptor_.mesh(), detail_->value());
      if (estimate > 1.5e6 &&
          QMessageBox::question(this, QStringLiteral("Remesh"),
                                QStringLiteral("That makes about %L1 triangles; sculpting that many can feel slow. Remesh anyway?")
                                    .arg(static_cast<qlonglong>(estimate))) != QMessageBox::Yes)
        return;
      QApplication::setOverrideCursor(Qt::WaitCursor);
      std::string error;
      const bool ok = sculptor_.remeshTo(detail_->value(), &error);
      QApplication::restoreOverrideCursor();
      if (!ok) QMessageBox::warning(this, QStringLiteral("Remesh"), QString::fromStdString(error));
      view_->update();
      refresh();
    });
    radiusText_->setText(QStringLiteral("50 px"));
    strengthText_->setText(QStringLiteral("0.50"));
  }
  pages_->addWidget(sculptPage_);
  pages_->setCurrentWidget(startPage ? startPage_ : sculptPage_);

  auto shortcut = [this](const QKeySequence& keys, auto&& f) {
    auto* s = new QShortcut(keys, this);
    s->setContext(Qt::WindowShortcut);
    connect(s, &QShortcut::activated, this, f);
  };
  for (const BrushInfo& info : kBrushes) {
    const Brush brush = info.brush;
    shortcut(QKeySequence(info.key), [this, brush] {
      if (pages_->currentWidget() == sculptPage_) selectBrush(brush);
    });
  }
  shortcut(QKeySequence::Undo, [this] { undo_->click(); });
  shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), [this] { redo_->click(); });
  shortcut(QKeySequence(Qt::CTRL | Qt::Key_Y), [this] { redo_->click(); });
}

void SculptDialog::selectBrush(Brush brush) {
  view_->setBrush(brush);
  if (QAbstractButton* button = brushes_->button(static_cast<int>(brush))) button->setChecked(true);
  if (stampBox_) stampBox_->setVisible(brush == Brush::Stamp);
  if (brush == Brush::Stamp) updateStamp();
  view_->update();
}

void SculptDialog::updateStamp() {
  if (!stampPicker_) return;
  const QString key = stampPicker_->currentData().toString();
  QImage picture = key == QLatin1String("own") ? ownPicture_ : stampPattern(key);
  if (picture.isNull()) return;
  auto stamp = std::make_shared<sculpt::StampImage>();
  stamp->width = picture.width();
  stamp->height = picture.height();
  stamp->value.resize(static_cast<size_t>(stamp->width) * stamp->height);
  QImage shown(picture.size(), QImage::Format_Grayscale8);
  const bool dark = stampDark_ && stampDark_->isChecked();
  for (int y = 0; y < picture.height(); ++y)
    for (int x = 0; x < picture.width(); ++x) {
      const QRgb pixel = picture.pixel(x, y);
      double value = qGray(pixel) / 255.0;
      if (dark) value = 1 - value;
      value *= qAlpha(pixel) / 255.0;
      stamp->value[static_cast<size_t>(y) * stamp->width + x] = static_cast<float>(value);
      shown.setPixel(x, y, qRgb(int(value * 255), int(value * 255), int(value * 255)));
    }
  view_->setStamp(stamp, shown);
}

void SculptDialog::chooseStampPicture() {
  QSettings settings;
  const QString start = settings.value(QStringLiteral("stampPictureDir"), QDir::homePath()).toString();
  const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Stamp picture"), start,
                                                    QStringLiteral("Pictures (*.png *.jpg *.jpeg *.bmp *.svg *.webp)"));
  if (path.isEmpty()) return;
  settings.setValue(QStringLiteral("stampPictureDir"), QFileInfo(path).absolutePath());
  QImage image(path);
  if (image.isNull()) {
    QMessageBox::warning(this, QStringLiteral("Stamp picture"), QStringLiteral("Could not read that picture."));
    return;
  }
  // Square, the picture in the middle at up to 256 px, the rest left alone (transparent).
  image = image.convertToFormat(QImage::Format_ARGB32).scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  QImage square(256, 256, QImage::Format_ARGB32);
  square.fill(Qt::transparent);
  {
    QPainter painter(&square);
    painter.drawImage((256 - image.width()) / 2, (256 - image.height()) / 2, image);
  }
  ownPicture_ = square;
  // A dark logo on white is the usual case: guess it from the average brightness.
  double sum = 0, weight = 0;
  for (int y = 0; y < square.height(); y += 4)
    for (int x = 0; x < square.width(); x += 4) {
      const QRgb p = square.pixel(x, y);
      sum += qGray(p) * qAlpha(p) / 255.0;
      weight += qAlpha(p) / 255.0;
    }
  if (weight > 0) stampDark_->setChecked(sum / weight > 140);
  const int at = stampPicker_->findData(QStringLiteral("own"));
  const QIcon icon(QPixmap::fromImage(square.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
  const QString label = QFileInfo(path).completeBaseName();
  if (at >= 0) {
    stampPicker_->setItemIcon(at, icon);
    stampPicker_->setItemText(at, label);
    stampPicker_->setCurrentIndex(at);
  } else {
    stampPicker_->addItem(icon, label, QStringLiteral("own"));
    stampPicker_->setCurrentIndex(stampPicker_->count() - 1);
  }
  updateStamp();
}

bool SculptDialog::startWith(const QString& kind, double size, double detail) {
  sculpt::Mesh mesh;
  std::string error;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const bool ok = sculpt::startShape(kind.toStdString(), size, detail, &mesh, &error);
  QApplication::restoreOverrideCursor();
  if (!ok) {
    QMessageBox::warning(this, QStringLiteral("Sculpt"), QString::fromStdString(error));
    return false;
  }
  if (name_.isEmpty()) {
    name_ = kind.left(1).toUpper() + kind.mid(1) + QStringLiteral(" sculpt");
    setWindowTitle(QStringLiteral("Sculpt – %1").arg(name_));
  }
  begin(std::move(mesh));
  return true;
}

void SculptDialog::begin(sculpt::Mesh mesh) {
  sculptor_.setMesh(std::move(mesh));
  savedVersion_ = sculptor_.version();
  vec3 lo, hi;
  sculpt::bounds(sculptor_.mesh(), &lo, &hi);
  view_->setMirrorCenter((lo + hi) * 0.5);
  detail_->setValue(std::round(sculptor_.averageEdge() * 100) / 100);
  pages_->setCurrentWidget(sculptPage_);
  view_->frameAll();
  view_->setFocus();
  refresh();
}

void SculptDialog::refresh() {
  if (!info_) return;
  const auto& mesh = sculptor_.mesh();
  QString text = QStringLiteral("%L1 triangles, edges about %2 mm").arg(static_cast<qlonglong>(mesh.tri.size()))
                     .arg(sculptor_.averageEdge(), 0, 'f', 2);
  if (!libraryPath_.isEmpty())
    text += QStringLiteral("<br>In the library as “%1”%2").arg(QFileInfo(libraryPath_).completeBaseName().toHtmlEscaped(),
                                                            unsaved() ? QStringLiteral(", changed since it was saved")
                                                                      : QStringLiteral(", saved"));
  if (sculptor_.heldBack() > 0)
    text += QStringLiteral("<br><span style='color:#e87400'>The last stroke was held back where it would have pushed the "
                           "surface through itself.</span>");
  info_->setText(text);
  undo_->setEnabled(sculptor_.canUndo());
  redo_->setEnabled(sculptor_.canRedo());
}

void SculptDialog::finish() {
  if (sculptor_.mesh().tri.empty()) return;
  if (hasOriginal_) {
    QMessageBox ask(this);
    ask.setWindowTitle(QStringLiteral("Done sculpting"));
    ask.setText(QStringLiteral("Put the sculpted version in place of the original, or add it as a new part beside it?"));
    QPushButton* replace = ask.addButton(QStringLiteral("Replace the original"), QMessageBox::AcceptRole);
    QPushButton* add = ask.addButton(QStringLiteral("Add as a new part"), QMessageBox::AcceptRole);
    ask.addButton(QMessageBox::Cancel);
    ask.setDefaultButton(replace);
    ask.exec();
    if (ask.clickedButton() == replace) replace_ = true;
    else if (ask.clickedButton() == add) replace_ = false;
    else return;
  }
  accept();
}

void SculptDialog::reject() {
  if (pages_->currentWidget() == sculptPage_ && unsaved()) {
    QMessageBox ask(this);
    ask.setWindowTitle(QStringLiteral("Sculpt"));
    ask.setText(libraryPath_.isEmpty() ? QStringLiteral("Close without keeping this sculpt?")
                                       : QStringLiteral("Save the changes to “%1” in the library before closing?")
                                             .arg(QFileInfo(libraryPath_).completeBaseName()));
    QPushButton* save = libraryFolder_.isEmpty() ? nullptr : ask.addButton(QStringLiteral("Save to library"), QMessageBox::AcceptRole);
    QPushButton* drop = ask.addButton(QStringLiteral("Close without saving"), QMessageBox::DestructiveRole);
    ask.addButton(QMessageBox::Cancel);
    ask.setDefaultButton(save ? save : drop);
    ask.exec();
    if (save && ask.clickedButton() == save) {
      if (!saveToLibrary(false)) return;
    } else if (ask.clickedButton() != drop) {
      return;
    }
  }
  QDialog::reject();
}

void SculptDialog::setLibrary(const QString& folder, const QString& path) {
  libraryFolder_ = folder;
  libraryPath_ = path;
  if (!path.isEmpty()) {
    // Opened from the library: Done puts a copy on the bed, Close leaves.
    if (done_) done_->setText(QStringLiteral("Place on bed"));
    if (cancel_) cancel_->setText(QStringLiteral("Close"));
  }
  if (save_) save_->setEnabled(!folder.isEmpty());
  if (saveAs_) saveAs_->setEnabled(!folder.isEmpty());
  refresh();
}

bool SculptDialog::saveToLibrary(bool askName) {
  if (libraryFolder_.isEmpty() || sculptor_.mesh().tri.empty()) return false;
  QString path = libraryPath_;
  if (askName || path.isEmpty()) {
    QString suggestion = libraryPath_.isEmpty() ? name_ : QFileInfo(libraryPath_).completeBaseName();
    if (askName && !libraryPath_.isEmpty()) suggestion += QStringLiteral(" copy");
    bool ok = false;
    QString name = QInputDialog::getText(this, QStringLiteral("Save to library"), QStringLiteral("Name"), QLineEdit::Normal,
                                         suggestion, &ok)
                       .trimmed();
    name.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
    if (!ok || name.isEmpty()) return false;
    path = QDir(libraryFolder_).filePath(name + QStringLiteral(".stl"));
    if (QFileInfo::exists(path) && QDir::cleanPath(path) != QDir::cleanPath(libraryPath_) &&
        QMessageBox::question(this, QStringLiteral("Save to library"),
                              QStringLiteral("“%1” is already in the library. Replace it?").arg(name)) != QMessageBox::Yes)
      return false;
  }
  QDir().mkpath(libraryFolder_);
  QSaveFile file(path);
  const std::string bytes = sculpt::stlBytes(sculptor_.mesh());
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes.data(), static_cast<qint64>(bytes.size())) != qint64(bytes.size()) ||
      !file.commit()) {
    QMessageBox::warning(this, QStringLiteral("Save to library"), QStringLiteral("Could not save %1.").arg(QDir::toNativeSeparators(path)));
    return false;
  }
  // A picture for the library list, from the way it is turned now.
  const QString picture = QFileInfo(path).absolutePath() + QLatin1Char('/') + QFileInfo(path).completeBaseName() + QStringLiteral(".png");
  view_->thumbnail(128).save(picture);
  libraryPath_ = path;
  name_ = QFileInfo(path).completeBaseName();
  setWindowTitle(QStringLiteral("Sculpt – %1").arg(name_));
  savedVersion_ = sculptor_.version();
  savedAny_ = true;
  refresh();
  return true;
}
