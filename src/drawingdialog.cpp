#include "drawingdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

// The sheet, fitted into the window on a grey desk.
class DrawingPreview : public QWidget {
 public:
  explicit DrawingPreview(QWidget* parent) : QWidget(parent) { setMinimumSize(480, 340); }
  const drawing::Sheet* sheet = nullptr;

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(43, 48, 56));
    if (!sheet || sheet->size.isEmpty()) return;
    const double margin = 18;
    const double k = std::min((width() - 2 * margin) / sheet->size.width(), (height() - 2 * margin) / sheet->size.height());
    if (k <= 0) return;
    const QSizeF shown = sheet->size * k;
    const QPointF at((width() - shown.width()) / 2, (height() - shown.height()) / 2);
    painter.fillRect(QRectF(at + QPointF(4, 4), shown), QColor(0, 0, 0, 90));
    painter.translate(at);
    painter.scale(k, k);
    drawing::paint(painter, *sheet);
  }
};

DrawingDialog::DrawingDialog(QWidget* parent, drawing::Model modelIn, const QString& title, const QString& subject, const QString& folderIn)
    : QDialog(parent), model(std::move(modelIn)), folder(folderIn) {
  setWindowTitle(QStringLiteral("Technical drawing"));
  setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
  resize(1360, 860);
  auto* root = new QHBoxLayout(this);
  auto* left = new QVBoxLayout();
  preview = new DrawingPreview(this);
  left->addWidget(preview, 1);
  // Sheet 1 of n, when the drawing needs more than one.
  pageBar = new QWidget(this);
  auto* bar = new QHBoxLayout(pageBar);
  bar->setContentsMargins(0, 0, 0, 0);
  prevButton = new QPushButton(QStringLiteral("◀ Previous"), pageBar);
  nextButton = new QPushButton(QStringLiteral("Next ▶"), pageBar);
  pageLabel = new QLabel(pageBar);
  pageLabel->setAlignment(Qt::AlignCenter);
  bar->addStretch(1);
  bar->addWidget(prevButton);
  bar->addWidget(pageLabel);
  bar->addWidget(nextButton);
  bar->addStretch(1);
  left->addWidget(pageBar);
  root->addLayout(left, 1);
  connect(prevButton, &QPushButton::clicked, this, [this] { showPage(page - 1); });
  connect(nextButton, &QPushButton::clicked, this, [this] { showPage(page + 1); });

  auto* panel = new QWidget(this);
  auto* side = new QVBoxLayout(panel);
  side->setContentsMargins(4, 0, 4, 0);
  auto* what = new QLabel(QStringLiteral("Drawing of <b>%1</b>").arg(subject.toHtmlEscaped()), panel);
  what->setWordWrap(true);
  side->addWidget(what);

  QSettings settings;
  settings.beginGroup(QStringLiteral("drawing"));
  auto* sheetForm = new QFormLayout();
  titleEdit = new QLineEdit(title, panel);
  sheetForm->addRow(QStringLiteral("Title"), titleEdit);
  paperBox = new QComboBox(panel);
  paperBox->addItems({QStringLiteral("A4"), QStringLiteral("A3"), QStringLiteral("A2")});
  paperBox->setCurrentText(settings.value(QStringLiteral("paper"), QStringLiteral("A3")).toString());
  paperBox->setToolTip(QStringLiteral("Landscape. A3 suits most parts; the scale follows the paper."));
  sheetForm->addRow(QStringLiteral("Paper"), paperBox);
  projectionBox = new QComboBox(panel);
  projectionBox->addItem(QStringLiteral("First angle (ISO, Europe)"));
  projectionBox->addItem(QStringLiteral("Third angle (US)"));
  projectionBox->setCurrentIndex(settings.value(QStringLiteral("thirdAngle"), false).toBool() ? 1 : 0);
  projectionBox->setToolTip(QStringLiteral("First angle: the top view goes under the front view, the view from the left goes "
                                           "on its right. Third angle: the top view above, the view from the right on the right."));
  sheetForm->addRow(QStringLiteral("Projection"), projectionBox);
  scaleBox = new QComboBox(panel);
  scaleBox->addItem(QStringLiteral("Fit the paper"), 0.0);
  for (double s : {10.0, 5.0, 2.0, 1.0, 0.5, 0.2, 0.1, 0.05, 0.02}) scaleBox->addItem(drawing::scaleText(s), s);
  sheetForm->addRow(QStringLiteral("Scale"), scaleBox);
  side->addLayout(sheetForm);

  hiddenBox = new QCheckBox(QStringLiteral("Hidden lines (dashed)"), panel);
  dimensionsBox = new QCheckBox(QStringLiteral("Dimensions"), panel);
  isoBox = new QCheckBox(QStringLiteral("Isometric view"), panel);
  tableBox = new QCheckBox(QStringLiteral("Filament estimate"), panel);
  hiddenBox->setChecked(settings.value(QStringLiteral("hidden"), true).toBool());
  dimensionsBox->setChecked(settings.value(QStringLiteral("dimensions"), true).toBool());
  isoBox->setChecked(settings.value(QStringLiteral("iso"), true).toBool());
  tableBox->setChecked(settings.value(QStringLiteral("table"), true).toBool());
  dimensionsBox->setToolTip(QStringLiteral("Overall sizes, and the diameter and position of round holes and pegs seen square on."));
  for (QCheckBox* box : {hiddenBox, dimensionsBox, isoBox, tableBox}) side->addWidget(box);

  auto* filamentTitle = new QLabel(QStringLiteral("<b>Filament</b>"), panel);
  side->addSpacing(8);
  side->addWidget(filamentTitle);
  auto* filamentForm = new QFormLayout();
  materialBox = new QComboBox(panel);
  for (const auto& m : drawing::materials()) materialBox->addItem(m.name, m.density);
  materialBox->setCurrentText(settings.value(QStringLiteral("material"), QStringLiteral("PLA")).toString());
  filamentForm->addRow(QStringLiteral("Material"), materialBox);
  densitySpin = new QDoubleSpinBox(panel);
  densitySpin->setRange(0.5, 3);
  densitySpin->setDecimals(2);
  densitySpin->setSingleStep(0.01);
  densitySpin->setSuffix(QStringLiteral(" g/cm³"));
  densitySpin->setValue(settings.value(QStringLiteral("density"), materialBox->currentData().toDouble()).toDouble());
  densitySpin->setToolTip(QStringLiteral("Follows the material; change it for a brand that says otherwise."));
  filamentForm->addRow(QStringLiteral("Density"), densitySpin);
  infillSpin = new QDoubleSpinBox(panel);
  infillSpin->setRange(0, 100);
  infillSpin->setDecimals(0);
  infillSpin->setSuffix(QStringLiteral(" %"));
  infillSpin->setValue(settings.value(QStringLiteral("infill"), 15).toDouble());
  filamentForm->addRow(QStringLiteral("Infill"), infillSpin);
  wallsSpin = new QDoubleSpinBox(panel);
  wallsSpin->setRange(0, 10);
  wallsSpin->setDecimals(2);
  wallsSpin->setSingleStep(0.1);
  wallsSpin->setSuffix(QStringLiteral(" mm"));
  wallsSpin->setValue(settings.value(QStringLiteral("walls"), 0.8).toDouble());
  wallsSpin->setToolTip(QStringLiteral("How thick the solid skin is all round: wall loops x line width (2 x 0.42 is about 0.8), "
                                       "and about the same top and bottom."));
  filamentForm->addRow(QStringLiteral("Walls"), wallsSpin);
  priceSpin = new QDoubleSpinBox(panel);
  priceSpin->setRange(0, 1000);
  priceSpin->setDecimals(2);
  priceSpin->setSuffix(QStringLiteral(" per kg"));
  priceSpin->setValue(settings.value(QStringLiteral("price"), 20).toDouble());
  filamentForm->addRow(QStringLiteral("Price"), priceSpin);
  currencyEdit = new QLineEdit(settings.value(QStringLiteral("currency"), QStringLiteral("€")).toString(), panel);
  currencyEdit->setMaxLength(4);
  filamentForm->addRow(QStringLiteral("Currency"), currencyEdit);
  side->addLayout(filamentForm);
  summary = new QLabel(panel);
  summary->setWordWrap(true);
  summary->setStyleSheet(QStringLiteral("color: #8a8074;"));
  side->addWidget(summary);
  settings.endGroup();

  side->addStretch(1);
  auto* saveTitle = new QLabel(QStringLiteral("<b>Save as</b>"), panel);
  side->addWidget(saveTitle);
  auto* buttons = new QHBoxLayout();
  for (const QString& kind : {QStringLiteral("PDF"), QStringLiteral("SVG"), QStringLiteral("DXF"), QStringLiteral("PNG")}) {
    auto* button = new QPushButton(kind, panel);
    button->setToolTip(kind == QLatin1String("PDF")   ? QStringLiteral("To print or send.")
                       : kind == QLatin1String("SVG") ? QStringLiteral("Lines and text, for Inkscape, Illustrator or a web page.")
                       : kind == QLatin1String("DXF") ? QStringLiteral("For CAD programs and laser cutters; lines on layers (OUTLINE, HIDDEN, DIMENSIONS...).")
                                                      : QStringLiteral("A picture at 300 dpi."));
    connect(button, &QPushButton::clicked, this, [this, kind] { save(kind); });
    buttons->addWidget(button);
  }
  side->addLayout(buttons);
  auto* close = new QPushButton(QStringLiteral("Close"), panel);
  connect(close, &QPushButton::clicked, this, &QDialog::accept);
  side->addWidget(close);

  auto* scroll = new QScrollArea(this);
  scroll->setWidget(panel);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setFixedWidth(300);
  root->addWidget(scroll);

  connect(materialBox, &QComboBox::currentIndexChanged, this, [this] {
    if (!building) densitySpin->setValue(materialBox->currentData().toDouble());
  });
  connect(titleEdit, &QLineEdit::textChanged, this, [this] { rebuild(); });
  connect(currencyEdit, &QLineEdit::textChanged, this, [this] { rebuild(); });
  for (QComboBox* box : {paperBox, projectionBox, scaleBox, materialBox})
    connect(box, &QComboBox::currentIndexChanged, this, [this] { rebuild(); });
  for (QCheckBox* box : {hiddenBox, dimensionsBox, isoBox, tableBox}) connect(box, &QCheckBox::toggled, this, [this] { rebuild(); });
  for (QDoubleSpinBox* spin : {densitySpin, infillSpin, wallsSpin, priceSpin})
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { rebuild(); });
  building = false;
  rebuild();
}

DrawingDialog::~DrawingDialog() {
  // The settings stay for next time (the title belongs to the part, so it does not).
  QSettings settings;
  settings.beginGroup(QStringLiteral("drawing"));
  settings.setValue(QStringLiteral("paper"), paperBox->currentText());
  settings.setValue(QStringLiteral("thirdAngle"), projectionBox->currentIndex() == 1);
  settings.setValue(QStringLiteral("hidden"), hiddenBox->isChecked());
  settings.setValue(QStringLiteral("dimensions"), dimensionsBox->isChecked());
  settings.setValue(QStringLiteral("iso"), isoBox->isChecked());
  settings.setValue(QStringLiteral("table"), tableBox->isChecked());
  settings.setValue(QStringLiteral("material"), materialBox->currentText());
  settings.setValue(QStringLiteral("density"), densitySpin->value());
  settings.setValue(QStringLiteral("infill"), infillSpin->value());
  settings.setValue(QStringLiteral("walls"), wallsSpin->value());
  settings.setValue(QStringLiteral("price"), priceSpin->value());
  settings.setValue(QStringLiteral("currency"), currencyEdit->text());
  settings.endGroup();
}

drawing::Options DrawingDialog::options() const {
  drawing::Options o;
  o.paper = paperBox->currentText();
  o.thirdAngle = projectionBox->currentIndex() == 1;
  o.scale = scaleBox->currentData().toDouble();
  o.hidden = hiddenBox->isChecked();
  o.dimensions = dimensionsBox->isChecked();
  o.iso = isoBox->isChecked();
  o.filamentTable = tableBox->isChecked();
  o.title = titleEdit->text().trimmed();
  o.date = QDate::currentDate().toString(Qt::ISODate);
  o.filament.material = materialBox->currentText();
  o.filament.density = densitySpin->value();
  o.filament.infill = infillSpin->value();
  o.filament.walls = wallsSpin->value();
  o.filament.pricePerKg = priceSpin->value();
  o.filament.currency = currencyEdit->text().trimmed();
  return o;
}

void DrawingDialog::rebuild() {
  if (building) return;
  const drawing::Options o = options();
  current = drawing::layoutSheets(model, o, &scaleUsed);
  scaleBox->setItemText(0, QStringLiteral("Fit the paper (%1)").arg(drawing::scaleText(o.scale > 0 ? o.scale : scaleUsed)));
  const drawing::FilamentEstimate e = drawing::estimate(model.volume, model.area, o.filament);
  summary->setText(QStringLiteral("Solid volume %1 cm³, printed about %2 g. Weight counts the walls solid and the inside "
                                  "at the infill; supports, brims and purge are not in it.")
                       .arg(QString::number(model.volume / 1000.0, 'f', 1), QString::number(e.grams, 'f', e.grams < 10 ? 1 : 0)));
  showPage(page);
}

void DrawingDialog::showPage(int index) {
  if (current.empty()) return;
  page = std::clamp(index, 0, static_cast<int>(current.size()) - 1);
  preview->sheet = &current[static_cast<size_t>(page)];
  preview->update();
  pageBar->setVisible(current.size() > 1);
  pageLabel->setText(QStringLiteral("  Sheet %1 of %2  ").arg(page + 1).arg(current.size()));
  prevButton->setEnabled(page > 0);
  nextButton->setEnabled(page + 1 < static_cast<int>(current.size()));
}

void DrawingDialog::save(const QString& kind) {
  const QString extension = kind.toLower();
  QString name = titleEdit->text().trimmed();
  name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")), QStringLiteral("_"));
  if (name.isEmpty()) name = QStringLiteral("drawing");
  QString start = folder.isEmpty() ? QDir::homePath() : folder;
  QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save drawing as %1").arg(kind), QDir(start).filePath(name + QStringLiteral(".") + extension),
                                              QStringLiteral("%1 (*.%2)").arg(kind, extension));
  if (path.isEmpty()) return;
  if (!path.endsWith(QStringLiteral(".") + extension, Qt::CaseInsensitive)) path += QStringLiteral(".") + extension;
  folder = QFileInfo(path).absolutePath();
  QString error;
  bool ok = true;
  QStringList written;
  if (kind == QLatin1String("PDF")) {
    ok = drawing::writePdf(current, path, &error);  // every sheet a page
    written << path;
  } else {
    // One file per sheet: name.svg, name-2.svg, ...
    for (size_t i = 0; i < current.size() && ok; ++i) {
      const QFileInfo info(path);
      const QString file = i == 0 ? path : info.dir().filePath(QStringLiteral("%1-%2.%3").arg(info.completeBaseName()).arg(i + 1).arg(info.suffix()));
      if (kind == QLatin1String("SVG")) ok = drawing::writeSvg(current[i], file, &error);
      else if (kind == QLatin1String("DXF")) ok = drawing::writeDxf(current[i], file, &error);
      else ok = drawing::writePng(current[i], file, 300, &error);
      if (ok) written << QFileInfo(file).fileName();
    }
    if (ok && written.size() > 1)
      QMessageBox::information(this, QStringLiteral("Technical drawing"),
                               QStringLiteral("The drawing has %1 sheets, saved as %2 files:\n%3").arg(written.size()).arg(kind, written.join(QStringLiteral("\n"))));
  }
  if (!ok) QMessageBox::warning(this, QStringLiteral("Technical drawing"), error.isEmpty() ? QStringLiteral("Could not save the drawing.") : error);
}
