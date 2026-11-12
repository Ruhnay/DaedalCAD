#pragma once

#include "evaluator.h"

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QQuaternion>
#include <QVector>
#include <QVector3D>

#include <future>
#include <memory>
#include <unordered_map>

class PickIndex;

class QKeyEvent;
class QPainter;

// Samples per pixel for smooth edges in the 3D views (0: off; 2, 4 or 8), from Settings > Display.
// Read when a view is made, so a change shows in views opened after it (the main view: after a
// restart).
int smoothingSamples();

class Viewport : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
  Q_OBJECT

 public:
  explicit Viewport(QWidget* parent = nullptr);
  ~Viewport() override;

  void setItems(std::vector<DrawItem> items, double workplaneZ);
  void setBed(double width, double depth);
  void setPreviewMesh(CpuMesh mesh);
  void setPreview(bool preview);
  void setTool(int tool);
  void setSnap(bool enabled, double step);
  void setGrid(bool on);
  void setFocusMarker(bool on);
  // The view glides so these parts' middle (of their box, together) is the focus point: what it
  // turns and zooms around. If they are not drawn yet, it happens as soon as they are.
  void focusOn(const QStringList& ids);
  void setRotationSnap(double degrees);
  void setFilletPick(std::vector<PickEdge> edges, std::vector<int> selected, bool all);

  // Align tool: world boxes of the parts being aligned, and the one that stays put (may be empty).
  // While boxes are set, dots for min / centre / max on each axis are drawn around them.
  struct AlignBox {
    QString id;
    QVector3D lo;
    QVector3D hi;
  };
  void setAlignBoxes(std::vector<AlignBox> boxes, const QString& anchorId);
  // Shown while the measure tool is on. `size` is the selection's width, depth and height.
  // A workplane on a face: drawn as a grid there, and drops land on it (dropped() then gets the
  // spot as u, v along its axes from `origin`). Off: the bed at workplaneZ.
  void setFaceWorkplane(bool on, QVector3D origin, QVector3D normal, QVector3D xAxis);
  // Section view: cut the view at `at` across `axis` (0 X, 1 Y, 2 Z). The low side (of the axis)
  // is cut away, the high side flipped; lo/hi: the scene's box, for the plane's outline.
  // Overhang shading: faces steeper than angleDegrees from vertical, facing down, drawn striped red.
  void setOverhangs(bool on, double angleDegrees);
  bool overhangsShown() const { return overhangOn; }
  void setSection(bool on, int axis, double at, bool flip, QVector3D lo, QVector3D hi);
  // Orthographic: no perspective, parallel lines stay parallel (sizes compare across the view).
  void setOrthographic(bool on);
  bool orthographic() const { return ortho; }
  // Home: the middle of the plate from its front-right corner, raised half the plate's width.
  void goHome();
  // Isometric: orthographic, from the front-right corner at the true isometric angle.
  void isometricView();
  void viewFrom(int face) { lookFace(face); }  // 0 Right, 1 Left, 2 Back, 3 Front, 4 Top, 5 Bottom
  void setMeasureReadout(bool haveA, const QVector3D& a, bool haveB, const QVector3D& b, bool haveSize, const QVector3D& size);

 signals:
  void picked(const QString& id, Qt::KeyboardModifiers mods);
  void marqueeSelected(QStringList ids, Qt::KeyboardModifiers mods);
  void dropped(const QString& shape, double x, double y, double fillet);
  void faceChosen(const QString& id, QVector<QVector3D> loop, QVector3D localNormal, QVector3D localAxis);
  // hit: the point clicked, in world space.
  void smoothFaceChosen(const QString& id, QVector<QVector3D> loop, QVector<QVector3D> normals, QVector3D hit);
  void edgePicked(int key, Qt::KeyboardModifiers mods);
  void transformStarted();
  void transformed(const QString& id, QVector3D pos, QVector3D rot, QVector3D scale);
  void nudgeRequested(int dx, int dy);
  void alignHandleClicked(int axis, int mode);
  void measurePicked(QVector3D world);
  void measureCleared();
  void projectionChanged(bool orthographic);
  void sliceFacePicked(QVector3D worldPoint, QVector3D worldNormal);
  // Lay flat (tool 12) and Workplane (tool 13): the face clicked, in world space (centre of its
  // outline, outward normal). id is empty when the click hit no part.
  void worldFacePicked(const QString& id, QVector3D centre, QVector3D normal);
  // A part has started moving or changing size under the mouse (sent once per drag).
  void moveDragStarted();
  // A left double-click on a part (sent once the button is up again).
  void doubleClicked(const QString& id);

 protected:
  void initializeGL() override;
  void resizeGL(int w, int h) override;
  void paintGL() override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  // Space held over the view: the left button pans, like the middle one.
  bool eventFilter(QObject* watched, QEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

 private:
  struct GpuBuffer {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;   // the triangles' corner numbers
    GLsizei count = 0;  // corner numbers to draw
  };

  struct Gpu {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLsizei count = 0;
    bool hole = false;
    bool selected = false;
    QVector3D color{0.84f, 0.77f, 0.64f};
    QMatrix4x4 model;
    QString id;
    int copy = 0;
  };

  bool compile();
  void upload();
  void destroyGpu();
  QMatrix4x4 projection() const;
  QMatrix4x4 viewMatrix() const;
  void cameraRay(const QPointF& pos, QVector3D* origin, QVector3D* direction) const;
  QPointF toScreen(const QVector3D& world) const;
  int gizmoAxis(const QPointF& pos) const;
  QString pickId(const QPointF& pos) const;
  // The first triangle of `item` along the ray (world), nearer than maxT; its distance and number.
  bool hitItem(const DrawItem& item, const QVector3D& origin, const QVector3D& dir, float maxT, float* dist, int* triangle) const;
  // A mesh's pick index (box tree and edge neighbours): built in the background for big meshes when
  // they arrive, at once for small ones; null while a big one is still being built.
  const PickIndex* pickIndexFor(const std::shared_ptr<const CpuMesh>& mesh, bool wait = false) const;
  struct PickEntry {
    std::shared_ptr<const CpuMesh> mesh;  // keeps the mesh the index points into alive
    std::shared_future<std::shared_ptr<const PickIndex>> pending;
    std::shared_ptr<const PickIndex> index;
  };
  mutable std::unordered_map<const CpuMesh*, PickEntry> picks;
  QString pendingDoubleClick;
  QStringList marqueeIds(const QRectF& rect, bool crossing) const;
  const DrawItem* gizmoItem() const;
  void rebuildGrid();
  float gizmoLength(const QVector3D& pivot) const;
  QVector3D eyePosition() const;
  bool pickFace(const QPointF& pos, QString* id, QVector<QVector3D>* loop, QVector3D* localNormal, QVector3D* localAxis,
                bool smooth = false, QVector<QVector3D>* pointNormals = nullptr, QMatrix4x4* modelOut = nullptr) const;
  bool pickWorldFace(const QPointF& pos, QString* id, QVector3D* centre, QVector3D* normal) const;
  void paintFaceWorkplane(QPainter& painter) const;
  void paintSection(QPainter& painter) const;
  QVector3D sectionNormal() const;
  bool itemAabb(const DrawItem& item, QVector3D* minOut, QVector3D* maxOut) const;
  bool meshBox(const CpuMesh& mesh, QVector3D* minOut, QVector3D* maxOut) const;
  int boxHandleAt(const QPointF& pos) const;
  int pickFilletEdge(const QPointF& pos) const;
  bool betaEdgeFacing(const DrawItem& item, const PickEdge& edge) const;
  QVector3D betaEdgeNudge(const DrawItem& item, const PickEdge& edge) const;
  QVector3D cameraUp() const;
  QVector3D cameraRight() const;
  void setOrbitAngles(float azimuth, float polar);
  bool focusHit(const QPointF& pos) const;
  void moveFocusTo(const QPointF& pos);
  void paintFocusMarker(QPainter& painter) const;
  int viewCubeFace(const QPointF& pos) const;
  bool solidHit(const QPointF& pos, QVector3D* hit) const;  // the nearest solid surface under the cursor
  void paintViewCube(QPainter& painter) const;
  void lookFace(int face);  // 0-5 a face; 6-9 the arrow up, right, down or left; 10 Home; 11 projection
  void glideTo(float azimuth, float polar, const QVector3D& focus, float distance);
  int neighbourFace(int arrow) const;
  bool alignFrame(QVector3D* unionLo, QVector3D* unionHi, QVector3D* refLo, QVector3D* refHi) const;
  bool alignHandlePoint(int index, QVector3D* point) const;
  int alignHandleAt(const QPointF& pos) const;
  void paintAlign(QPainter& painter) const;
  void paintMeasure(QPainter& painter) const;
  bool measurePointAt(const QPointF& pos, QVector3D* hit) const;
  bool slicePlaneAt(const QPointF& pos, QVector3D* point, QVector3D* normal) const;

  std::vector<DrawItem> items;
  CpuMesh previewMesh;
  bool preview = false;
  bool snapOn = true;
  double snap = 1;
  double rotationSnap = 0;
  double workplaneZ = 0;
  double bedW = 256;
  double bedD = 256;
  bool gridDirty = true;
  bool showGrid = true;
  bool showFocus = false;
  int tool = 0;
  bool dirty = true;
  bool glReady = false;
  std::vector<PickEdge> filletPick;
  std::vector<int> filletOn;
  bool filletAll = false;
  int hoverEdge = -1;
  int hoverView = -1;
  bool ortho = false;
  std::vector<AlignBox> alignBoxes;
  QString alignAnchorId;
  int alignHover = -1;
  bool alignConsumed = false;
  bool sectionOn = false;
  bool overhangOn = false;
  double overhangAngle = 45;
  int sectionAxis = 1;
  float sectionAt = 0;
  bool sectionFlip = false;
  QVector3D sectionLo, sectionHi;
  bool faceWorkplane = false;
  QVector3D workplaneOrigin, workplaneNormal{0, 0, 1}, workplaneX{1, 0, 0};
  bool measureHaveA = false;
  bool measureHaveB = false;
  bool measureHaveSize = false;
  bool measureHover = false;
  QVector3D measureA;
  QVector3D measureB;
  QVector3D measureSize;
  QVector3D measureHoverPoint;

  GLuint meshProgram = 0;
  GLuint lineProgram = 0;
  GLuint gridProgram = 0;  // the plate's grid: lines fade out where they crowd closer than a few pixels
  GLuint gridVao = 0;
  GLuint gridVbo = 0;
  GLsizei gridCount = 0;
  GLsizei gridMinor = 0;
  GLsizei gridMinorX = 0;  // of the minor lines, those at a fixed x (the rest are at a fixed y)
  GLsizei gridMajorX = 0;
  float gridFine = 1.f;  // mm between minor grid lines
  GLsizei gridMajor = 0;
  GLsizei gridTicks = 0;
  GLsizei gridBorder = 0;
  GLuint gizmoVao = 0;
  GLuint gizmoVbo = 0;
  std::vector<Gpu> gpu;
  std::unordered_map<const CpuMesh*, GpuBuffer> resident;

  QVector3D target{0, 0, 8};
  QQuaternion camera;
  float radius = 180.f;

  // Smooth navigation: orbit, pan and zoom move these goals, and a 60 fps timer eases the camera
  // toward them, so uneven mouse and wheel steps come out as one smooth motion.
  QQuaternion cameraGoal;
  QVector3D targetGoal{0, 0, 8};
  float radiusGoal = 180.f;
  class QTimer* easeTimer = nullptr;
  qint64 easeLast = 0;
  void beginNavigation();  // goals follow the camera unless an ease is already running
  void easeStep();
  void stopEase();         // something else set the camera: drop the goals
  // The focus point stays within one bed size past the bed's edges (and a sensible height).
  QVector3D clampFocus(QVector3D point) const;
  bool focusOnBed(const QPointF& pos, QVector3D* hit) const;  // where the bed is under the cursor
  QVector3D pendingFocus;  // while dragging the focus marker: where it will go
  QStringList focusIds;    // focusOn still waiting for these parts to be drawn
  bool applyFocusOn();

  QPointF lastPos;
  QPoint pressPos;
  QPoint marqueeStart;
  QPoint marqueeCurrent;
  bool orbiting = false;
  bool panning = false;
  bool focusDragging = false;
  bool marqueeing = false;
  bool dragging = false;
  bool freeArmed = false;
  bool freeDrag = false;
  bool dragAnnounced = false;  // moveDragStarted sent for this drag
  bool spaceHeld = false;  // Space is down with the cursor over the view: left drags pan
  bool scaleDrag = false;
  int axis = -1;
  int hoverAxis = -1;
  int scaleHandle = -1;
  int hoverHandle = -1;
  QString dragId;
  QVector3D dragAxis;
  QVector3D dragPivot;
  float dragParam = 0;
  QVector3D dragHit;
  double startPos[3]{};
  double startRot[3]{};
  double startScale[3]{1, 1, 1};
  QMatrix4x4 dragParent;
  QVector3D freeGrab;
  QVector3D dragMin;
  QVector3D dragMax;
  QVector3D scaleFix;
  QVector3D scaleGrab;
  QVector3D scaleLo;
  QVector3D scaleHi;
  bool liveScaleDimensions = false;
  double liveDimensions[3]{0.0, 0.0, 0.0};
  double scaleBaseExtents[3]{0.0, 0.0, 0.0};
};
