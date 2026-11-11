#pragma once

#include "updater.h"

#include "document.h"
#include "evaluator.h"
#include "exactstep.h"
#include "sculpt.h"

#include <QMainWindow>
#include <QPointer>
#include <QStringList>
#include <QVector3D>

#include <functional>
#include <vector>

struct CloudSyncHooks;
class QDoubleSpinBox;
class QSpinBox;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QTreeWidget;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QFrame;
class QLabel;
class QPushButton;
class QTabWidget;
class QDialog;
class QMenu;
class QPlainTextEdit;
class QFontComboBox;
class QTimer;
class QJsonValue;
class QFormLayout;
class QScrollArea;
class QResizeEvent;
class QAction;
class QShortcut;
class QKeySequence;
class QShowEvent;
class Viewport;

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  void buildUi();
  void refreshTree();
  void refreshScene(bool quick = false);
  void loadInspector();
  void pushUndo();
  void undo();
  void redo();
  void status(const QString& text);
  void exportMesh(bool selectedOnly, const QString& extension);
  void exportMeshTo(bool selectedOnly, const QString& extension, const QString& path);
  void exportSlice();
  void openDrawing();
  // Lay flat (tool 12) and Workplane (tool 13): both start with a click on a face.
  void showFaceTool(int which);
  void layFlat(const QString& id, QVector3D normal);
  void setWorkplaneOnFace(QVector3D centre, QVector3D normal);
  void clearFaceWorkplane();
  // A part just added: on a face workplane, stand it upright on it at u, v (else nothing).
  void standOnWorkplane(const QString& id, double u, double v);
  bool placingDropped = false;
  double placingU = 0, placingV = 0;
  void exportProjectZip(bool selectedOnly = false);
  void importProject();
  bool installProjectZip(const QString& path, const QString& forcedName, bool openAfter, bool askOverwrite, QString* error);
  void openCloudSync();
  CloudSyncHooks cloudSyncHooks();
  bool saveDocument();
  void openProjectsFolder();
  void openDocument();
  bool loadProjectFile(const QString& path);
  void rememberProject(const QString& filePath);
  void refreshRecentMenu();
  void offerRecentProjects();
  // Installed copy, first start: offer to copy the data of an earlier portable copy to Documents.
  void offerDataMove();
  UpdateHooks updateHooks();
  void showMeasureTool();
  // Closes whatever tool, panel or popup is open and goes back to Move. quiet: no status message.
  void backToMove(bool quiet = false);
  void updateMeasureOverlay();
  bool selectionSize(QVector3D* size) const;
  void placeShape(const QString& shape, double x, double y, double fillet);
  void importStlPart(const QString& path, double x, double y);
  void importSvgShape(const QString& path, double x, double y);
  // Generators: the Generators tab places them, the tool window holds their settings.
  void placeGenerator(const QString& id, double x, double y);
  void buildGeneratorPage();
  void loadGeneratorPage(const Node* node);
  void syncGeneratorRows(const Node* node);
  void setGeneratorValue(const QString& key, const QJsonValue& value);
  void applyGeneratorChange();
  void convertGeneratorToParts();
  void switchStandardPart(const QString& make);
  void addMatchingHole();
  void buildHollowPage();
  void loadHollowPage();
  void buildSplitPage();
  void loadSplitPage(bool recentre = false);
  void splitSelected();
  void importStep();
  struct StepLog {
    QPointer<QDialog> dialog;
    QPointer<QProgressBar> bar;
    QPointer<QPlainTextEdit> text;
    QPointer<QLabel> title;
    QPointer<QPushButton> close;
  };
  StepLog openStepLog(const QString& path);
  void finishStepLog(const StepLog& log, bool ok, const QString& error, const exact::Report& report);
  static void appendStepLogLine(QPlainTextEdit* text, const QString& line);
  void importStepFile(const QString& path);
  Node* selectedGenerator();
  // Hole clearance, in the tool window while holes are selected.
  void buildHolePage();
  void buildHullPage();
  void loadHullPage();
  void loadHolePage();
  std::vector<Node*> selectedHoles();
  void refreshPartsLibrary();
  void refresh2DShapeLibrary();
  void create2DShape();
  // The Sculpt window: the selected part (sculpted parts as they are, others remeshed first), or a
  // new shape when nothing is selected.
  void openSculpt();
  void newSculpt();
  void sculptPart(const QString& id);
  // The Sculpts library: place a copy of one (x, y on the workplane), open one to keep sculpting it.
  void placeLibrarySculpt(const QString& path, double x, double y);
  void editLibrarySculpt(const QString& path);
  void refreshSculptLibrary();
  void addSculptToPlate(const sculpt::Mesh& mesh, const QString& name, double x = 0, double y = 0);
  // A mesh (world millimetres) made from part `id`: in its place (replace) or beside it, as an STL
  // part; sculptPart marks it as a sculpt (double-click sculpts it again).
  bool placeMeshForPart(const QString& id, sculpt::Mesh result, bool replace, const QString& newName, bool sculptPart);
  bool partMesh(const QString& id, sculpt::Mesh* mesh);
  void openLattice();
  void updateMergeButton();
  // Keyboard shortcuts: every tool and command, with a key that can be changed in Settings.
  struct Command {
    QString id;        // saved as keys/<id>
    QString name;      // shown in Settings
    QString group;
    QString fallback;  // the default key, portable text ("Ctrl+G"); empty: none
    QPointer<QAction> action;  // a toolbar or menu action, or else:
    QShortcut* shortcut = nullptr;
  };
  std::vector<Command> commands;
  void buildCommands();
  QString keyFor(const Command& command) const;
  void applyKeys();
  QWidget* buildKeysPage(QWidget* parent, std::function<void()>* commit);
  void openTexture();  // a surface texture on the selected part
  void openRestyle();  // the selected part low-poly or in voxels
  void openWrap();     // text or a logo wrapped onto another part
  void openArticulate();  // the selected part as a print-in-place flexi
  void openSimplify();    // fewer triangles for a heavy part
  void openMold();        // a two-piece casting mould of a part
  void edit2DShape(const QString& path);
  void openSettings();
  void chooseObjectColor();
  void showAlignTool();
  void showHelp();
  struct AlignEntry {
    QString id;
    QVector3D lo;
    QVector3D hi;
  };
  std::vector<AlignEntry> alignEntries() const;
  QString alignRootFor(const QString& id) const;
  void updateAlignOverlay();
  QString alignAnchorId;
  void alignSelection(int axis, int mode);
  bool measureHaveA = false;
  bool measureHaveB = false;
  QVector3D measureA;
  QVector3D measureB;
  bool recentOffered = false;
  QMenu* recentMenu = nullptr;
  void extrudePickedFace(const QString& id, const QVector<QVector3D>& loop, QVector3D normal, QVector3D axis);
  void filletPickedSurface(const QString& id, const QVector<QVector3D>& loop, const QVector<QVector3D>& normals,
                           const QVector3D& hit);
  void showFilletLog(const QStringList& lines);
  void showSurfaceFilletTool();
  void showEdgeBetaTool();
  void syncEdgeBetaChrome();
  void applyEdgeBetaFillet();
  void clearEdgeBetaFillet();
  bool edgeBetaTargetOk(const Node* node, QString* why) const;
  double dropDistanceBelow(const Node& node, QString* landedOn) const;
  void showPanel(int next);
  void showFilletTool();
  void syncFilletChrome();
  void applyFillet(bool allEdges);
  void clearFillet();
  void showRepeatTool();
  void runRepeat();
  void nudgeSelection(int dx, int dy);
  void duplicateSelectionInPlace();
  void syncActions();
  void placeOverlays();
  bool sceneIsHeavy() const;
  void runBusy(const std::function<void()>& work, bool immediate);
  // runBusy, or straight away if a busy job is already running (runBusy would skip it).
  void runBusyOrNow(const std::function<void()>& work, bool immediate);
  QDoubleSpinBox* makeSpin(double min, double max, double step);
  void bindSpin(QDoubleSpinBox* box, const std::function<void(double)>& apply, bool quick = false);

  Document doc;
  std::vector<Document> undoStack;
  std::vector<Document> redoStack;
  QStringList selection;
  struct MultiTransformStart {
    QString id;
    double pos[3]{};
    double rot[3]{};
    double scale[3]{1, 1, 1};
  };
  std::vector<MultiTransformStart> multiTransformStart;
  bool loading = false;
  bool inspectorWaitsForScene = false;  // loadInspector skipped a group not merged yet
  bool gesture = false;
  // After a live edit drew a heavy group as loose parts: builds it properly once the edit pauses.
  QTimer* settleTimer = nullptr;
  int tool = 0;
  int panel = 0;
  bool slicePicking = false;
  int sliceToolRestore = 0;
  bool sliceHaveFace = false;
  QVector3D slicePoint;
  QVector3D sliceNormal;
  QLabel* sliceFaceLabel = nullptr;
  bool rebuilding = false;
  QString filletTarget;
  std::vector<int> pendingEdges;
  bool pendingAll = false;
  double draftAngle = 90;
  double draftRadius = 30;
  double draftRise = 0;
  int draftSteps = 12;
  int draftAxis = 0;
  bool updatingFacePreset = false;
  double multiPosLast[3]{};
  double multiRotLast[3]{};
  double multiScaleLast[3]{100.0, 100.0, 100.0};

  Viewport* view = nullptr;
  QTreeWidget* tree = nullptr;
  QLineEdit* nameEdit = nullptr;
  QPushButton* colorButton = nullptr;
  QDoubleSpinBox* posSpin[3]{};
  QDoubleSpinBox* rotSpin[3]{};
  QDoubleSpinBox* sizeSpin[3]{};
  QDoubleSpinBox* radiusSpin = nullptr;
  QDoubleSpinBox* radius2Spin = nullptr;
  QSpinBox* sidesSpin = nullptr;
  QComboBox* threadBox = nullptr;
  QDoubleSpinBox* threadPitchSpin = nullptr;
  QDoubleSpinBox* threadClearanceSpin = nullptr;
  QDoubleSpinBox* gemTableSpin = nullptr;
  QSpinBox* segmentsSpin = nullptr;
  QDoubleSpinBox* edgeSpin = nullptr;
  QDoubleSpinBox* surfaceSpin = nullptr;
  QFrame* faceFilletPopup = nullptr;
  // Edge fillet (beta), tool 9.
  QFrame* edgeBetaPopup = nullptr;
  QDoubleSpinBox* edgeBetaRadius = nullptr;
  QComboBox* edgeBetaShape = nullptr;
  QSpinBox* edgeBetaSegSpin = nullptr;
  QLabel* edgeBetaCount = nullptr;
  QLabel* edgeBetaHint = nullptr;
  QString edgeBetaTarget;
  QByteArray edgeBetaSignature;
  std::vector<int> edgeBetaPending;
  std::vector<PickEdge> edgeBetaEdges;
  std::vector<std::vector<double>> edgeBetaChains;
  std::vector<bool> edgeBetaConcave;
  int edgeBetaLost = 0;
  int edgeBetaPieces = 0;
  QDialog* consoleDialog = nullptr;
  QPlainTextEdit* consoleLog = nullptr;
  QDoubleSpinBox* faceFilletRadius = nullptr;
  QSpinBox* faceFilletSegs = nullptr;
  QComboBox* faceFilletSide = nullptr;
  QComboBox* faceFilletDir = nullptr;
  QComboBox* filletSideBox = nullptr;
  QComboBox* filletDirBox = nullptr;
  // Text shape controls (rows in the shape form, shown for text only).
  QPlainTextEdit* textEdit = nullptr;
  QFontComboBox* fontBox = nullptr;
  QComboBox* fontStyleBox = nullptr;
  QDoubleSpinBox* fontSizeSpin = nullptr;
  QDoubleSpinBox* letterSpacingSpin = nullptr;
  QComboBox* textAlignBox = nullptr;
  QComboBox* textDetailBox = nullptr;
  QDoubleSpinBox* textArcSpin = nullptr;
  QComboBox* textArcBox = nullptr;
  QTimer* textTimer = nullptr;
  QTimer* pending2DTimer = nullptr;
  QString pending2DPlacement;
  QTimer* pendingSculptTimer = nullptr;
  QString pendingSculptPlacement;
  void fillFontStyles(const QString& family, const QString& current);
  QDoubleSpinBox* bendSpin = nullptr;
  QDoubleSpinBox* faceAngleSpin = nullptr;
  QDoubleSpinBox* faceRadiusSpin = nullptr;
  QDoubleSpinBox* extrudeDepthSpin = nullptr;
  QDoubleSpinBox* faceRiseSpin = nullptr;
  QSpinBox* faceStepsSpin = nullptr;
  QComboBox* faceAxisBox = nullptr;
  QComboBox* facePresetBox = nullptr;
  QCheckBox* faceSpiralBox = nullptr;
  QDoubleSpinBox* faceRadialSpin = nullptr;
  QComboBox* alongBox = nullptr;
  QComboBox* towardBox = nullptr;
  QDoubleSpinBox* twistSpin = nullptr;
  QComboBox* twistAxisBox = nullptr;
  QDoubleSpinBox* taperSpin = nullptr;
  QComboBox* taperAxisBox = nullptr;
  QCheckBox* mirrorBox[3]{};
  QSpinBox* countSpin = nullptr;
  QDoubleSpinBox* moveSpin[3]{};
  QDoubleSpinBox* drotSpin[3]{};
  QDoubleSpinBox* resizeSpin[3]{};
  QDoubleSpinBox* workplaneSpin = nullptr;
  QDoubleSpinBox* bedXSpin = nullptr;
  QDoubleSpinBox* bedYSpin = nullptr;
  QComboBox* bedPreset = nullptr;
  QDoubleSpinBox* snapSpin = nullptr;
  QDoubleSpinBox* snapBar = nullptr;
  QSpinBox* filletSegSpin = nullptr;
  QDoubleSpinBox* rotationStepSpin = nullptr;
  QWidget* shapePage = nullptr;
  QWidget* rotPage = nullptr;
  QWidget* bendPage = nullptr;
  QWidget* facePage = nullptr;
  QWidget* extrudePage = nullptr;
  QWidget* mirrorPage = nullptr;
  QWidget* patternPage = nullptr;
  QWidget* platePage = nullptr;
  QFrame* colorPopup = nullptr;
  QFrame* toolPopup = nullptr;
  QLabel* toolTitle = nullptr;
  QLabel* toolHint = nullptr;
  QWidget* movePage = nullptr;
  QFrame* filletPopup = nullptr;
  QFrame* repeatPopup = nullptr;
  QFrame* alignPopup = nullptr;
  QSpinBox* repeatCount = nullptr;
  QDoubleSpinBox* repeatMove[3]{};
  QDoubleSpinBox* repeatRot[3]{};
  QDoubleSpinBox* repeatResize[3]{};
  QFrame* busyWrap = nullptr;
  // Bottom bar: how hard the program works the computer, then the messages.
  QLabel* usageLabel = nullptr;
  QLabel* messageLabel = nullptr;
  class UsageMeter* usageMeter = nullptr;
  void updateUsage();
  // Section view: a bar on the view with the axis, the cut's position and Flip.
  QFrame* sectionBar = nullptr;
  QComboBox* sectionAxisBox = nullptr;
  class QSlider* sectionSlider = nullptr;
  QDoubleSpinBox* sectionSpin = nullptr;
  class QToolButton* sectionFlipButton = nullptr;
  bool sectionOn = false;
  QVector3D sectionLo, sectionHi;
  void setSection(bool on);
  void updateSection(bool rescan);
  QProgressBar* busy = nullptr;
  QLabel* filletHint = nullptr;
  QLabel* filletCount = nullptr;
  class ShapeList* shapes = nullptr;
  class ShapeList* parts = nullptr;
  class ShapeList* shapes2d = nullptr;
  class ShapeList* sculptsList = nullptr;
  class ShapeList* generators = nullptr;
  QWidget* genPage = nullptr;
  QScrollArea* genScroll = nullptr;
  QWidget* genInner = nullptr;
  QFormLayout* genForm = nullptr;
  QLabel* genScaleNote = nullptr;
  QPushButton* genResetScale = nullptr;
  QPushButton* genMatchHole = nullptr;
  QWidget* hollowPage = nullptr;
  QDoubleSpinBox* hollowWallSpin = nullptr;
  QCheckBox* hollowOpenBox[6] = {};
  QLabel* hollowNote = nullptr;
  QWidget* splitPage = nullptr;
  QComboBox* splitAxisBox = nullptr;
  QDoubleSpinBox* splitAtSpin = nullptr;
  QDoubleSpinBox* splitGapSpin = nullptr;
  QLabel* splitNote = nullptr;
  QComboBox* splitJoinBox = nullptr;     // connectors: none, pins, pegs, dovetail
  QDoubleSpinBox* splitJoinSize = nullptr;  // their diameter or width, 0 automatic
  QSpinBox* splitJoinCount = nullptr;
  // A mesh (world millimetres) as a new part in the group `parentId`; its id. No undo step.
  QString addMeshNode(sculpt::Mesh mesh, const QString& name, const QString& parentId, const QColor& color, bool hole);
  QString splitFor;
  QTimer* genTimer = nullptr;
  QString genFormFor;
  QString genPendingId;
  double genBottom = 0;
  bool genBottomKnown = false;
  std::vector<std::pair<QString, QWidget*>> genRows;
  QWidget* holePage = nullptr;
  QWidget* hullPage = nullptr;
  QComboBox* hullModeBox = nullptr;
  QDoubleSpinBox* blendBox = nullptr;  // smooth join, mm
  QCheckBox* multiColorBox = nullptr;
  QLabel* hullNote = nullptr;
  QComboBox* holeClearanceBox = nullptr;
  QDoubleSpinBox* holeClearanceSpin = nullptr;
  QCheckBox* holeSidesBox = nullptr;
  QLabel* holeClearanceNote = nullptr;
  QTimer* holeTimer = nullptr;
  QStringList partsDirectories;
  QStringList partsDirectoryNames;
  QStringList twoDShapesDirectories;
  QStringList twoDShapesDirectoryNames;
  QString projectDirectory;
};
