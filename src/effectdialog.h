#pragma once

// A window for an effect on one part (a texture, a low-poly or voxel restyle): settings on the left,
// built in the background with a progress bar and shown on the right, then put in place of the
// part or beside it. The caller adds its settings to form() and says how to build.

#include "lattice.h"
#include "sculpt.h"

#include <QDialog>

#include <functional>
#include <memory>
#include <thread>

class QFormLayout;
class QGroupBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
class SculptView;

class EffectDialog : public QDialog {
  Q_OBJECT

 public:
  // Builds from the part with the settings as they were when it started; runs on another thread.
  using Build = std::function<bool(const sculpt::Mesh& part, sculpt::Mesh* out, std::string* error, lattice::Report* report,
                                   lattice::Progress* progress)>;

  // noun: what the result is called in the Replace / Add question ("the textured part").
  EffectDialog(QWidget* parent, sculpt::Mesh part, const QString& title, const QString& noun);
  ~EffectDialog() override;

  QFormLayout* form() const { return form_; }
  const sculpt::Mesh& part() const { return part_; }
  // makeBuild is asked for a Build each time one starts (so it captures the settings then);
  // summary gives the line under the settings (an estimate), refreshed by changed().
  void setBuild(std::function<Build()> makeBuild) { makeBuild_ = std::move(makeBuild); }
  void setSummary(std::function<QString()> summary) { summary_ = std::move(summary); }
  // Show the result with hard edges between the triangles (for flat facets and cubes).
  void setFlatPreview(bool on) { flat_ = on; }
  // The question Apply asks, and its two answers (replaceOriginal() is true for the first).
  void setChoice(const QString& question, const QString& first, const QString& second) {
    question_ = question;
    first_ = first;
    second_ = second;
  }
  // Call when a setting changes: the preview is out of date.
  void changed();

  sculpt::Mesh result() const { return result_; }
  bool replaceOriginal() const { return replace_; }
  // Builds with the settings as they are and shows it, waiting (the window keeps working).
  bool preview();
  void reject() override;

 private:
  struct Job;
  void apply();
  void start(bool thenApply);
  void stop();
  void poll();
  void finish();
  void setBuilding(bool building);
  void askAndAccept();

  sculpt::Mesh part_;
  sculpt::Mesh result_;
  QString noun_;
  std::function<Build()> makeBuild_;
  std::function<QString()> summary_;
  bool fresh_ = false;
  bool replace_ = true;
  bool applyAfter_ = false;
  bool lastOk_ = false;
  bool flat_ = false;
  QString question_, first_, second_;
  sculpt::Sculptor shown_;
  SculptView* view_ = nullptr;
  QGroupBox* settingsBox_ = nullptr;
  QFormLayout* form_ = nullptr;
  QLabel* estimate_ = nullptr;
  QLabel* state_ = nullptr;
  QPushButton* previewButton_ = nullptr;
  QPushButton* applyButton_ = nullptr;
  QProgressBar* bar_ = nullptr;
  QTimer* poll_ = nullptr;
  std::shared_ptr<Job> job_;
  std::thread worker_;
};
