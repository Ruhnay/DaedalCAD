#pragma once

// The Technical drawing window: the sheet on the left, its settings and the export buttons on
// the right. Everything rebuilds as soon as a setting changes.

#include "drawing.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class DrawingPreview;

class DrawingDialog : public QDialog {
 public:
  // subject: what is drawn, in words ("Wall", "3 selected parts"). folder: where saving starts.
  DrawingDialog(QWidget* parent, drawing::Model model, const QString& title, const QString& subject, const QString& folder);
  ~DrawingDialog() override;

  const drawing::Sheet& sheet() const { return current.front(); }
  const std::vector<drawing::Sheet>& sheets() const { return current; }
  double scale() const { return scaleUsed; }

 private:
  void rebuild();
  drawing::Options options() const;
  void save(const QString& kind);
  void showPage(int index);

  drawing::Model model;
  std::vector<drawing::Sheet> current;
  int page = 0;
  double scaleUsed = 1;
  QString folder;
  DrawingPreview* preview = nullptr;
  QWidget* pageBar = nullptr;
  QPushButton* prevButton = nullptr;
  QPushButton* nextButton = nullptr;
  QLabel* pageLabel = nullptr;
  QLineEdit* titleEdit = nullptr;
  QComboBox* paperBox = nullptr;
  QComboBox* projectionBox = nullptr;
  QComboBox* scaleBox = nullptr;
  QCheckBox* hiddenBox = nullptr;
  QCheckBox* dimensionsBox = nullptr;
  QCheckBox* isoBox = nullptr;
  QCheckBox* tableBox = nullptr;
  QComboBox* materialBox = nullptr;
  QDoubleSpinBox* densitySpin = nullptr;
  QDoubleSpinBox* infillSpin = nullptr;
  QDoubleSpinBox* wallsSpin = nullptr;
  QDoubleSpinBox* priceSpin = nullptr;
  QLineEdit* currencyEdit = nullptr;
  QLabel* summary = nullptr;
  bool building = true;
};
