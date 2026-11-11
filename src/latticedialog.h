#pragma once

// The Lattice window: the selected part as a Voronoi skin, a gyroid, a honeycomb or a Voronoi
// lattice, with a preview, then in place of the part or beside it.

#include "lattice.h"
#include "sculpt.h"

#include <QDialog>

#include <memory>
#include <thread>

class QComboBox;
class QGroupBox;
class QProgressBar;
class QTimer;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class SculptView;

class LatticeDialog : public QDialog {
  Q_OBJECT

 public:
  LatticeDialog(QWidget* parent, sculpt::Mesh part, const QString& name);
  ~LatticeDialog() override;

  sculpt::Mesh result() const { return result_; }
  bool replaceOriginal() const { return replace_; }
  lattice::Settings settings() const;
  // Builds the lattice with the settings as they are and shows it, waiting until it is done (the
  // window keeps working meanwhile). False if it failed or was stopped.
  bool preview();

  void reject() override;

 private:
  struct Job;
  void changed();
  void apply();
  // Starts building on another thread; the progress bar follows it. thenApply: ask Replace or Add
  // once it is done.
  void start(bool thenApply);
  void stop();
  void poll();
  void finish();
  void setBuilding(bool building);
  void askAndAccept();

  sculpt::Mesh part_;
  sculpt::Mesh result_;
  bool fresh_ = false;  // result_ matches the settings
  bool replace_ = true;
  sculpt::Sculptor shown_;
  SculptView* view_ = nullptr;
  QComboBox* kind_ = nullptr;
  QDoubleSpinBox* cell_ = nullptr;
  QDoubleSpinBox* wall_ = nullptr;
  QDoubleSpinBox* skin_ = nullptr;
  QLabel* skinLabel_ = nullptr;
  QSpinBox* seed_ = nullptr;
  QLabel* seedLabel_ = nullptr;
  QDoubleSpinBox* detail_ = nullptr;
  QLabel* about_ = nullptr;
  QLabel* estimate_ = nullptr;
  QLabel* state_ = nullptr;
  QGroupBox* settingsBox_ = nullptr;
  QPushButton* previewButton_ = nullptr;
  QPushButton* applyButton_ = nullptr;
  QProgressBar* bar_ = nullptr;
  QTimer* poll_ = nullptr;
  std::shared_ptr<Job> job_;
  std::thread worker_;
  bool applyAfter_ = false;
  bool lastOk_ = false;
};
