#include "effectdialog.h"

#include "sculptdialog.h"

#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

// One build, shared by the window and the thread doing it.
struct EffectDialog::Job {
  Build build;
  lattice::Progress progress;
  sculpt::Mesh out;
  std::string error;
  lattice::Report report;
  bool ok = false;
  double seconds = 0;
  std::atomic<bool> done{false};
};

namespace {
QString stageText(int stage) {
  switch (stage) {
    case 0: return QStringLiteral("Measuring the part");
    case 1: return QStringLiteral("Building");
    case 2: return QStringLiteral("Leaving out loose crumbs");
    case 3: return QStringLiteral("Smoothing the surface");
    case 4: return QStringLiteral("Checking for folds");
    default: return QStringLiteral("Working");
  }
}
}  // namespace

EffectDialog::EffectDialog(QWidget* parent, sculpt::Mesh part, const QString& title, const QString& noun)
    : QDialog(parent), part_(std::move(part)), noun_(noun) {
  setWindowTitle(title);
  setWindowFlags(windowFlags() | Qt::WindowMaximizeButtonHint);
  resize(1150, 760);
  auto* row = new QHBoxLayout(this);
  auto* side = new QWidget(this);
  side->setFixedWidth(310);
  auto* panel = new QVBoxLayout(side);
  panel->setContentsMargins(0, 0, 6, 0);
  settingsBox_ = new QGroupBox(title.section(QStringLiteral(" – "), 0, 0), side);
  form_ = new QFormLayout(settingsBox_);
  panel->addWidget(settingsBox_);
  estimate_ = new QLabel(side);
  estimate_->setWordWrap(true);
  estimate_->setStyleSheet(QStringLiteral("color: #c3cad4;"));
  panel->addWidget(estimate_);
  previewButton_ = new QPushButton(QStringLiteral("Preview"), side);
  previewButton_->setToolTip(QStringLiteral("Build it and show it here."));
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
  connect(previewButton_, &QPushButton::clicked, this, [this] {
    if (job_) stop();
    else start(false);
  });
  connect(buttons, &QDialogButtonBox::accepted, this, [this] { apply(); });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  QMetaObject::invokeMethod(this, [this] {
    changed();
    view_->frameAll();
  }, Qt::QueuedConnection);
}

EffectDialog::~EffectDialog() {
  if (job_) job_->progress.cancel = true;
  if (worker_.joinable()) worker_.join();
}

void EffectDialog::changed() {
  fresh_ = false;
  if (summary_) estimate_->setText(summary_());
  if (!job_) state_->setText(QStringLiteral("Preview shows it here; Apply puts it in the workspace."));
}

bool EffectDialog::preview() {
  if (!job_) start(false);
  QEventLoop wait;
  QTimer check;
  connect(&check, &QTimer::timeout, &wait, [&] {
    if (!job_) wait.quit();
  });
  check.start(20);
  wait.exec();
  return lastOk_ && fresh_;
}

void EffectDialog::start(bool thenApply) {
  if (job_ || !makeBuild_) return;
  applyAfter_ = thenApply;
  job_ = std::make_shared<Job>();
  job_->build = makeBuild_();
  std::shared_ptr<Job> job = job_;
  const sculpt::Mesh* part = &part_;
  worker_ = std::thread([job, part] {
    const auto started = std::chrono::steady_clock::now();
    job->ok = job->build(*part, &job->out, &job->error, &job->report, &job->progress);
    job->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    job->done = true;
  });
  setBuilding(true);
  poll();
  poll_->start();
}

void EffectDialog::stop() {
  if (!job_) return;
  job_->progress.cancel = true;
  applyAfter_ = false;
  state_->setText(QStringLiteral("Stopping…"));
}

void EffectDialog::setBuilding(bool building) {
  settingsBox_->setEnabled(!building);
  previewButton_->setText(building ? QStringLiteral("Stop") : QStringLiteral("Preview"));
  applyButton_->setEnabled(!building);
  bar_->setVisible(building);
  if (building) bar_->setValue(0);
}

void EffectDialog::poll() {
  if (!job_) return;
  if (job_->done) {
    finish();
    return;
  }
  if (job_->progress.cancel) return;
  const double fraction = job_->progress.fraction.load();
  bar_->setValue(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 1000));
  state_->setText(QStringLiteral("%1… %2 %").arg(stageText(job_->progress.stage.load())).arg(static_cast<int>(fraction * 100)));
}

void EffectDialog::finish() {
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
  if (flat_) {
    // Every triangle its own corners, so each is lit as the flat face it is.
    sculpt::Mesh faceted;
    faceted.pos.reserve(result_.tri.size() * 3);
    faceted.tri.reserve(result_.tri.size());
    for (const sculpt::Tri& t : result_.tri) {
      const uint32_t base = static_cast<uint32_t>(faceted.pos.size());
      for (uint32_t v : t) faceted.pos.push_back(result_.pos[v]);
      faceted.tri.push_back({base, base + 1, base + 2});
    }
    shown_.setMesh(std::move(faceted));
  } else {
    shown_.setMesh(result_);
  }
  view_->update();
  QString text = QStringLiteral("%L1 triangles in %2 s.").arg(static_cast<qlonglong>(result_.tri.size())).arg(job->seconds, 0, 'f', 1);
  if (!job->report.clean)
    text += QStringLiteral(" A handful of tiny triangles stayed folded; another detail usually clears them.");
  state_->setText(text);
  if (applyAfter_) {
    applyAfter_ = false;
    askAndAccept();
  }
}

void EffectDialog::reject() {
  if (job_) {
    job_->progress.cancel = true;
    if (worker_.joinable()) worker_.join();
    job_.reset();
    poll_->stop();
  }
  QDialog::reject();
}

void EffectDialog::apply() {
  if (job_) return;
  if (!fresh_) {
    start(true);
    return;
  }
  askAndAccept();
}

void EffectDialog::askAndAccept() {
  QMessageBox ask(this);
  ask.setWindowTitle(windowTitle());
  ask.setText(question_.isEmpty() ? QStringLiteral("Put %1 in place of the part, or add it as a new part beside it?").arg(noun_)
                                  : question_);
  QPushButton* replace = ask.addButton(first_.isEmpty() ? QStringLiteral("Replace the part") : first_, QMessageBox::AcceptRole);
  QPushButton* add = ask.addButton(second_.isEmpty() ? QStringLiteral("Add as a new part") : second_, QMessageBox::AcceptRole);
  ask.addButton(QMessageBox::Cancel);
  ask.setDefaultButton(replace);
  ask.exec();
  if (ask.clickedButton() == replace) replace_ = true;
  else if (ask.clickedButton() == add) replace_ = false;
  else return;
  accept();
}
