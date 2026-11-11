#pragma once

// The Sculpt window: start from a ball, cube, cylinder, slab or capsule, or from the selected part,
// and push and pull its surface with brushes. Done hands the result back to the workspace.

#include "sculpt.h"

#include <QDialog>
#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPointF>

#include <vector>

class QButtonGroup;
class QToolButton;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QStackedWidget;

class SculptView : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
  Q_OBJECT

 public:
  explicit SculptView(sculpt::Sculptor* sculptor, QWidget* parent = nullptr);
  ~SculptView() override;

  void setBrush(sculpt::Brush brush) { brush_ = brush; }
  sculpt::Brush brush() const { return brush_; }
  void setRadiusPx(int px);
  int radiusPx() const { return radiusPx_; }
  void setStrength(double strength) { strength_ = strength; }
  void setFalloff(sculpt::Falloff falloff) { falloff_ = falloff; }
  void setMirror(int axis, bool on) {
    mirror_[axis] = on;
    update();
  }
  void setMirrorCenter(const sculpt::vec3& center) { mirrorCenter_ = center; }
  // Only for looking at (no brush): left-drag turns the view too.
  void setReadOnly(bool on) {
    readOnly_ = on;
    setCursor(on ? Qt::OpenHandCursor : Qt::CrossCursor);
  }
  void setRadial(int copies, int axis) {
    radial_ = copies;
    radialAxis_ = axis;
    update();
  }
  void setStamp(std::shared_ptr<const sculpt::StampImage> stamp, const QImage& preview) {
    stamp_ = std::move(stamp);
    stampPreview_ = preview;
  }
  void frameAll();
  // Orbit angles in degrees (for a fixed view in screenshots).
  void setView(double yawDegrees, double pitchDegrees);
  // A square picture of the whole shape from the current direction (for the library).
  QImage thumbnail(int size);

 signals:
  void edited();
  void radiusChanged(int px);

 protected:
  void initializeGL() override;
  void resizeGL(int w, int h) override;
  void paintGL() override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private:
  QVector3D eye() const;
  QMatrix4x4 viewMatrix() const;
  QMatrix4x4 projection() const;
  bool ray(const QPointF& pos, sculpt::vec3* origin, sculpt::vec3* dir) const;
  double worldPerPixel(const sculpt::vec3& at) const;
  bool beginStroke(const QPointF& pos, Qt::KeyboardModifiers mods);
  void continueStroke(const QPointF& pos);
  void finishStroke();
  void upload();
  void drawMirror(const QMatrix4x4& mvp);

  sculpt::Sculptor* sculptor_;
  sculpt::Brush brush_ = sculpt::Brush::Draw;
  int radiusPx_ = 50;
  double strength_ = 0.5;
  sculpt::Falloff falloff_ = sculpt::Falloff::Smooth;
  bool mirror_[3] = {false, false, false};
  sculpt::vec3 mirrorCenter_{0, 0, 0};
  int radial_ = 1;
  int radialAxis_ = 2;
  bool readOnly_ = false;
  std::shared_ptr<const sculpt::StampImage> stamp_;
  QImage stampPreview_;

  QVector3D target_;
  float distance_ = 100;
  float yaw_ = -0.9f;
  float pitch_ = 0.45f;
  float fov_ = 30;

  enum class Drag { None, Stroke, Orbit, Pan };
  Drag drag_ = Drag::None;
  QPointF last_;
  QPointF lastDab_;
  QPointF cursor_;
  bool cursorIn_ = false;
  bool grabbing_ = false;
  sculpt::vec3 grabPoint_{0, 0, 0};
  sculpt::vec3 grabNormal_{0, 0, 1};

  GLuint program_ = 0, vao_ = 0, vbo_ = 0, ebo_ = 0;
  // The mirror planes: a faint sheet through the middle and the line where it cuts the surface.
  GLuint lineProgram_ = 0, lineVao_ = 0, lineVbo_ = 0;
  std::vector<float> mirrorCut_[3], mirrorSheet_[3], mirrorRim_[3], radialAxisLine_, radialCut_;
  uint64_t mirrorVersion_ = 0;
  int mirrorMask_ = -1;
  uint64_t uploaded_ = 0;
  size_t uploadedTris_ = 0, uploadedVerts_ = 0;
  bool glReady_ = false;
};

class SculptDialog : public QDialog {
  Q_OBJECT

 public:
  // A new sculpt: pick a start shape first.
  explicit SculptDialog(QWidget* parent);
  // Sculpt this mesh (in world millimetres); hasOriginal: Done asks to replace it or add a copy.
  SculptDialog(QWidget* parent, sculpt::Mesh mesh, const QString& name, bool hasOriginal);
  ~SculptDialog() override;

  sculpt::Mesh result() const { return sculptor_.mesh(); }
  bool replaceOriginal() const { return replace_; }
  SculptView* view() const { return view_; }
  sculpt::Sculptor& sculptor() { return sculptor_; }
  // Picks a start shape as the buttons would (for scripted use).
  bool startWith(const QString& kind, double size, double detail);
  // The Sculpts library folder; path: the library sculpt this window has open (Save writes back to
  // it, Done reads "Place on bed").
  void setLibrary(const QString& folder, const QString& path = QString());
  bool savedToLibrary() const { return savedAny_; }
  QString libraryPath() const { return libraryPath_; }
  QString name() const { return name_; }
  // Writes the sculpt to the library; askName: always ask for a name (Save as).
  bool saveToLibrary(bool askName);

 public slots:
  void reject() override;

 private:
  void build(bool startPage);
  void begin(sculpt::Mesh mesh);
  void refresh();
  void finish();
  void selectBrush(sculpt::Brush brush);
  void updateStamp();
  void chooseStampPicture();

  bool unsaved() const { return sculptor_.version() != savedVersion_; }

  sculpt::Sculptor sculptor_;
  QString name_;
  QString libraryFolder_;
  QString libraryPath_;
  uint64_t savedVersion_ = 0;
  bool savedAny_ = false;
  QPushButton* save_ = nullptr;
  QPushButton* saveAs_ = nullptr;
  QPushButton* done_ = nullptr;
  QPushButton* cancel_ = nullptr;
  bool hasOriginal_ = false;
  bool replace_ = false;
  QStackedWidget* pages_ = nullptr;
  QWidget* startPage_ = nullptr;
  QWidget* sculptPage_ = nullptr;
  SculptView* view_ = nullptr;
  QButtonGroup* shapes_ = nullptr;
  QDoubleSpinBox* startSize_ = nullptr;
  QDoubleSpinBox* startDetail_ = nullptr;
  QLabel* startEstimate_ = nullptr;
  QButtonGroup* brushes_ = nullptr;
  QSlider* radius_ = nullptr;
  QLabel* radiusText_ = nullptr;
  QSlider* strength_ = nullptr;
  QLabel* strengthText_ = nullptr;
  QComboBox* falloff_ = nullptr;
  QToolButton* mirror_[3] = {nullptr, nullptr, nullptr};
  QDoubleSpinBox* detail_ = nullptr;
  QPushButton* undo_ = nullptr;
  QPushButton* redo_ = nullptr;
  QLabel* info_ = nullptr;
  class QGroupBox* stampBox_ = nullptr;
  QComboBox* stampPicker_ = nullptr;
  class QCheckBox* stampDark_ = nullptr;
  QImage ownPicture_;
  class QSpinBox* radial_ = nullptr;
  QComboBox* radialAxis_ = nullptr;
};
