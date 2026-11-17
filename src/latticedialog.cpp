#include "latticedialog.h"

#include "sculptdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

using lattice::Kind;

namespace {

const char* kAbout[] = {
    "The outer shell, cut into an organic web of cells, like a Voronoi lamp or a bone. The inside stays hollow. Best on "
    "rounded parts and sculpts.",
    "A gyroid fills the part: one smooth, wavy wall that never closes off a pocket. Strong for its weight and prints "
    "without supports.",
    "Upright hexagon cells, like a bee's comb: walls stand straight up, so it prints fast and needs no supports.",
    "A 3D web of struts between random cells, like a bird's bone or coral. Very light.",
    "A random, grown-looking sponge, like coral, pumice or the inside of a bone: smooth struts and holes of every "
    "shape, all joined up. Walls near half the cell size make it dense; thinner walls make it more open.",
    "A random sponge of thin, wavy walls with big open pores either side, like a random gyroid. Any cell size "
    "with any walls: light and open, and few triangles.",
    "A solid with round air bubbles of mixed sizes blown into it, like foamed glass, pumice or an aerated bar. "
    "Bubbles that meet open into each other, and those at the surface open to the outside. Cell size is the bubble "
    "size; Walls the thinnest skin left between two bubbles (closer ones break through).",
};

// What the starting cell size and walls aim for: quick to build and light in the workspace.
constexpr double kTargetTriangles = 500e3;

}  // namespace

// One build, shared by the window and the thread doing it.
struct LatticeDialog::Job {
  lattice::Settings settings;
  lattice::Progress progress;
  sculpt::Mesh out;
  std::string error;
  lattice::Report report;
  bool ok = false;
  double seconds = 0;
  std::atomic<bool> done{false};
};

LatticeDialog::LatticeDialog(QWidget* parent, sculpt::Mesh part, const QString& name)
    : QDialog(parent), part_(std::move(part)) {
  setWindowTitle(QStringLiteral("Lattice – %1").arg(name));
  setWindowFlags(windowFlags() | Qt::WindowMaximizeButtonHint);
  resize(1150, 760);
  auto* row = new QHBoxLayout(this);

  auto* side = new QWidget(this);
  side->setFixedWidth(300);
  auto* panel = new QVBoxLayout(side);
  panel->setContentsMargins(0, 0, 6, 0);
  auto* box = new QGroupBox(QStringLiteral("Lattice"), side);
  settingsBox_ = box;
  auto* form = new QFormLayout(box);
  kind_ = new QComboBox(box);
  kind_->addItems({QStringLiteral("Voronoi skin"), QStringLiteral("Gyroid"), QStringLiteral("Honeycomb"),
                   QStringLiteral("Voronoi lattice"), QStringLiteral("Sponge"),
                   QStringLiteral("Sponge walls"), QStringLiteral("Bubbles")});
  kind_->setCurrentIndex(1);
  form->addRow(QStringLiteral("Type"), kind_);
  about_ = new QLabel(box);
  about_->setWordWrap(true);
  about_->setStyleSheet(QStringLiteral("color: #c3cad4;"));
  form->addRow(about_);
  auto spin = [&](double lo, double hi, double value, const QString& tip) {
    auto* s = new QDoubleSpinBox(box);
    s->setRange(lo, hi);
    s->setDecimals(1);
    s->setSingleStep(0.2);
    s->setValue(value);
    s->setSuffix(QStringLiteral(" mm"));
    s->setToolTip(tip);
    return s;
  };
  cell_ = spin(2, 60, 8, QStringLiteral("How big one cell is. Larger cells: more open and lighter."));
  wall_ = spin(0.4, 10, 1.2, QStringLiteral("How thick the walls or struts are. At least two nozzle widths prints best."));
  skin_ = spin(0, 10, 0, QStringLiteral("A solid skin around the lattice, this thick. 0 leaves the lattice open."));
  cell_->setDecimals(1);
  form->addRow(QStringLiteral("Cell size"), cell_);
  form->addRow(QStringLiteral("Walls"), wall_);
  skinLabel_ = new QLabel(QStringLiteral("Solid skin"), box);
  form->addRow(skinLabel_, skin_);
  seed_ = new QSpinBox(box);
  seed_->setRange(1, 9999);
  seed_->setToolTip(QStringLiteral("Another number gives another random pattern."));
  seedLabel_ = new QLabel(QStringLiteral("Pattern"), box);
  form->addRow(seedLabel_, seed_);
  detail_ = new QDoubleSpinBox(box);
  detail_->setRange(0, 3);
  detail_->setDecimals(2);
  detail_->setSingleStep(0.05);
  detail_->setSpecialValueText(QStringLiteral("Automatic"));
  detail_->setSuffix(QStringLiteral(" mm"));
  detail_->setToolTip(QStringLiteral("Edge length of the triangles. Automatic follows the walls; smaller is smoother but "
                                     "slower and bigger."));
  form->addRow(QStringLiteral("Detail"), detail_);
  estimate_ = new QLabel(box);
  estimate_->setTextFormat(Qt::RichText);
  estimate_->setWordWrap(true);
  form->addRow(estimate_);
  panel->addWidget(box);

  previewButton_ = new QPushButton(QStringLiteral("Preview"), side);
  previewButton_->setToolTip(QStringLiteral("Build it and show it here. Big parts with small cells take a while."));
  panel->addWidget(previewButton_);
  bar_ = new QProgressBar(side);
  bar_->setRange(0, 1000);
  bar_->setTextVisible(false);
  bar_->setFixedHeight(8);
  bar_->hide();
  panel->addWidget(bar_);
  state_ = new QLabel(side);
  state_->setWordWrap(true);
  panel->addWidget(state_);
  panel->addStretch(1);
  auto* buttons = new QDialogButtonBox(side);
  applyButton_ = buttons->addButton(QStringLiteral("Apply"), QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Cancel);
  applyButton_->setAutoDefault(false);
  panel->addWidget(buttons);
  row->addWidget(side);

  view_ = new SculptView(&shown_, this);
  view_->setReadOnly(true);
  row->addWidget(view_, 1);
  shown_.setMesh(part_);

  poll_ = new QTimer(this);
  poll_->setInterval(100);
  connect(poll_, &QTimer::timeout, this, [this] { poll(); });
  // The same button stops a build under way.
  connect(previewButton_, &QPushButton::clicked, this, [this] {
    if (job_) stop();
    else start(false);
  });
  connect(buttons, &QDialogButtonBox::accepted, this, [this] { apply(); });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(kind_, &QComboBox::currentIndexChanged, this, [this](int index) {
    // A Voronoi skin is a shell: its skin is the shell's thickness and must be there.
    const bool skin = index == static_cast<int>(Kind::VoronoiSkin);
    {
      const QSignalBlocker quiet(skin_);
      skinLabel_->setText(skin ? QStringLiteral("Shell") : QStringLiteral("Solid skin"));
      skin_->setMinimum(skin ? 0.6 : 0);
      if (skin && skin_->value() < 0.6) skin_->setValue(1.6);
      if (!skin && skin_->value() == 1.6) skin_->setValue(0);
    }
    if (!sizeTouched_) {
      // Sizes the user has not set follow the type, so each one starts at about the same weight.
      fitToPart();
    } else if (index == static_cast<int>(Kind::Sponge) && wall_->value() < cell_->value() * 0.25) {
      // A sponge with walls under a quarter of its cell falls apart; start it dense, as it looks best.
      const QSignalBlocker quiet(cell_);
      cell_->setValue(std::max(cell_->minimum(), wall_->value() * 2.5));
    }
    changed();
  });
  for (QDoubleSpinBox* s : {cell_, wall_})
    connect(s, &QDoubleSpinBox::valueChanged, this, [this] {
      sizeTouched_ = true;
      // A detail made coarse only to suit the large starting cells would be far too coarse for the
      // user's own sizes: back to Automatic.
      if (detailFitted_) {
        detailFitted_ = false;
        const QSignalBlocker quiet(detail_);
        detail_->setValue(0);
      }
    });
  connect(detail_, &QDoubleSpinBox::valueChanged, this, [this] {
    sizeTouched_ = true;
    detailFitted_ = false;
  });
  for (QDoubleSpinBox* s : {cell_, wall_, skin_, detail_}) connect(s, &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
  connect(seed_, &QSpinBox::valueChanged, this, [this] { changed(); });
  fitToPart();
  changed();
  QMetaObject::invokeMethod(view_, [this] { view_->frameAll(); }, Qt::QueuedConnection);
}

LatticeDialog::~LatticeDialog() {
  if (job_) job_->progress.cancel = true;
  if (worker_.joinable()) worker_.join();
}

lattice::Settings LatticeDialog::settings() const {
  lattice::Settings s;
  s.kind = static_cast<Kind>(kind_->currentIndex());
  s.cell = cell_->value();
  s.wall = wall_->value();
  s.skin = skin_->value();
  s.seed = seed_->value();
  s.detail = detail_->value();
  return s;
}

void LatticeDialog::fitToPart() {
  lattice::Settings s = settings();
  s.detail = 0;
  // The standard look: 8 mm cells with 1.2 mm walls (a sponge starts dense, as it looks best).
  const double baseCell = s.kind == Kind::Sponge ? 3.0 : 8.0, baseWall = 1.2;
  auto at = [&](double scale) {
    s.cell = std::min(cell_->maximum(), baseCell * scale);
    s.wall = std::min(wall_->maximum(), baseWall * scale);
    return lattice::estimateTriangles(part_, s);
  };
  // Bigger cells and walls together keep the look and give fewer triangles; find the scale that
  // lands on the target (most parts need none).
  double scale = 1;
  if (at(1) > kTargetTriangles) {
    double lo = 1, hi = 2;
    while (at(hi) > kTargetTriangles && hi < 64) hi *= 2;
    for (int i = 0; i < 30; ++i) {
      const double mid = (lo + hi) / 2;
      (at(mid) > kTargetTriangles ? lo : hi) = mid;
    }
    scale = hi;
  }
  const double triangles = at(scale);
  // Round to what the boxes show, up, so the result stays at or under the target.
  const QSignalBlocker quietCell(cell_), quietWall(wall_), quietDetail(detail_);
  cell_->setValue(std::ceil(s.cell * 10 - 1e-6) / 10);
  wall_->setValue(std::ceil(s.wall * 10 - 1e-6) / 10);
  // At the largest cells and walls a very big part can still be over: then coarser triangles too
  // (the count goes with one over the edge length squared), as far as the detail box goes.
  double detail = 0;
  if (triangles > kTargetTriangles * 1.05) {
    const double edge = lattice::usedDetail(part_, s);
    detail = std::min(detail_->maximum(), std::ceil(edge * std::sqrt(triangles / kTargetTriangles) * 20) / 20);
  }
  detail_->setValue(detail);
  detailFitted_ = detail > 0;
}

void LatticeDialog::changed() {
  fresh_ = false;
  const lattice::Settings s = settings();
  const int kind = static_cast<int>(s.kind);
  QString about = QString::fromLatin1(kAbout[kind]);
  if (s.kind == Kind::Sponge && lattice::spongeCell(s) > s.cell * 1.02)
    about += QStringLiteral(" <b>These walls are thin for the cell size:</b> struts thinner than a quarter of the cell "
                            "would fall apart, so the cells come out about %1 mm. For big holes with thin walls, use "
                            "Sponge walls.")
                 .arg(lattice::spongeCell(s), 0, 'f', 1);
  about_->setText(about);
  const bool random = s.kind == Kind::VoronoiSkin || s.kind == Kind::VoronoiLattice || s.kind == Kind::Sponge ||
                      s.kind == Kind::SpongeWalls || s.kind == Kind::Bubbles;
  seed_->setVisible(random);
  seedLabel_->setVisible(random);
  const double triangles = lattice::estimateTriangles(part_, s);
  QString estimate = QStringLiteral("Detail %1 mm, about %L2 triangles")
                         .arg(lattice::usedDetail(part_, s), 0, 'f', 2)
                         .arg(static_cast<qlonglong>(std::round(triangles / 1000) * 1000));
  // Up to a million is quick; up to ten million is slow (orange); more can take a very long time and
  // a lot of memory once it is in the workspace (red).
  if (triangles > 10e6)
    estimate = QStringLiteral("<span style='color:#ff7b72'>%1: a very large amount. It may take 15 minutes or more to "
                              "build, and in the workspace it uses a very large amount of memory. Larger cells, "
                              "thicker walls or a coarser detail are much quicker.</span>")
                   .arg(estimate);
  else if (triangles > 1e6)
    estimate = QStringLiteral("<span style='color:#e8a040'>%1: a lot. Larger cells, thicker walls or a coarser detail "
                              "are quicker.</span>")
                   .arg(estimate);
  estimate_->setText(estimate);
  state_->setText(QStringLiteral("Preview shows it here; Apply puts it in the workspace."));
}

bool LatticeDialog::preview() {
  if (!job_) start(false);
  // Wait for this build, keeping the window alive.
  QEventLoop wait;
  QTimer check;
  connect(&check, &QTimer::timeout, &wait, [&] {
    if (!job_) wait.quit();
  });
  check.start(20);
  wait.exec();
  return lastOk_ && fresh_;
}

void LatticeDialog::start(bool thenApply) {
  if (job_) return;
  applyAfter_ = thenApply;
  job_ = std::make_shared<Job>();
  job_->settings = settings();
  std::shared_ptr<Job> job = job_;
  const sculpt::Mesh* part = &part_;
  worker_ = std::thread([job, part] {
    const auto started = std::chrono::steady_clock::now();
    job->ok = lattice::build(*part, job->settings, &job->out, &job->error, &job->report, &job->progress);
    job->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    job->done = true;
  });
  setBuilding(true);
  poll();
  poll_->start();
}

void LatticeDialog::stop() {
  if (!job_) return;
  job_->progress.cancel = true;
  applyAfter_ = false;
  state_->setText(QStringLiteral("Stopping…"));
}

void LatticeDialog::setBuilding(bool building) {
  settingsBox_->setEnabled(!building);
  previewButton_->setText(building ? QStringLiteral("Stop") : QStringLiteral("Preview"));
  previewButton_->setToolTip(building ? QStringLiteral("Stop building; the settings stay as they are.")
                                      : QStringLiteral("Build it and show it here. Big parts with small cells take a while."));
  applyButton_->setEnabled(!building);
  bar_->setVisible(building);
  if (building) bar_->setValue(0);
}

void LatticeDialog::poll() {
  if (!job_) return;
  if (job_->done) {
    finish();
    return;
  }
  if (job_->progress.cancel) return;
  const double fraction = job_->progress.fraction.load();
  bar_->setValue(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 1000));
  state_->setText(QStringLiteral("%1… %2 %").arg(QString::fromLatin1(lattice::stageName(job_->progress.stage.load())))
                      .arg(static_cast<int>(fraction * 100)));
}

void LatticeDialog::finish() {
  poll_->stop();
  if (worker_.joinable()) worker_.join();
  std::shared_ptr<Job> job = std::move(job_);
  job_.reset();
  setBuilding(false);
  lastOk_ = job->ok && !job->progress.cancel;
  if (job->progress.cancel) {
    state_->setText(QStringLiteral("Stopped. Preview builds it again."));
    applyAfter_ = false;
    return;
  }
  if (!job->ok) {
    state_->setText(QString::fromStdString(job->error));
    applyAfter_ = false;
    return;
  }
  result_ = std::move(job->out);
  fresh_ = true;
  shown_.setMesh(result_);
  view_->update();
  const double partVolume = std::abs(sculpt::volume(part_));
  QString text = QStringLiteral("%L1 triangles in %2 s; %3 % of the solid part's weight.")
                     .arg(static_cast<qlonglong>(result_.tri.size()))
                     .arg(job->seconds, 0, 'f', 1)
                     .arg(partVolume > 0 ? std::abs(sculpt::volume(result_)) / partVolume * 100 : 0, 0, 'f', 0);
  if (job->report.droppedCrumbs > 0)
    text += QStringLiteral(" %1 loose crumb%2 at the surface left out.")
                .arg(job->report.droppedCrumbs)
                .arg(job->report.droppedCrumbs == 1 ? "" : "s");
  if (!job->report.clean)
    text += QStringLiteral(" A handful of tiny triangles stayed folded where struts meet the surface; another Detail "
                           "or Pattern number usually clears them.");
  state_->setText(text);
  if (applyAfter_) {
    applyAfter_ = false;
    askAndAccept();
  }
}

void LatticeDialog::reject() {
  if (job_) {
    job_->progress.cancel = true;
    if (worker_.joinable()) worker_.join();
    job_.reset();
    poll_->stop();
  }
  QDialog::reject();
}

void LatticeDialog::apply() {
  if (job_) return;
  if (!fresh_) {
    start(true);
    return;
  }
  askAndAccept();
}

void LatticeDialog::askAndAccept() {
  QMessageBox ask(this);
  ask.setWindowTitle(QStringLiteral("Lattice"));
  ask.setText(QStringLiteral("Put the lattice in place of the part, or add it as a new part beside it?"));
  QPushButton* replace = ask.addButton(QStringLiteral("Replace the part"), QMessageBox::AcceptRole);
  QPushButton* add = ask.addButton(QStringLiteral("Add as a new part"), QMessageBox::AcceptRole);
  ask.addButton(QMessageBox::Cancel);
  ask.setDefaultButton(replace);
  ask.exec();
  if (ask.clickedButton() == replace) replace_ = true;
  else if (ask.clickedButton() == add) replace_ = false;
  else return;
  accept();
}
