#include "appdirs.h"
#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QIcon>
#include <QSurfaceFormat>


int main(int argc, char** argv) {
  QSurfaceFormat format;
  format.setSamples(4);  // smooth edges in the 3D views
  format.setDepthBufferSize(24);
  format.setStencilBufferSize(8);
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  QSurfaceFormat::setDefaultFormat(format);

  QApplication app(argc, argv);
  QApplication::setApplicationName("DaedalCAD");
  QApplication::setOrganizationName("DaedalCAD");
  // DaedalCAD was called ShapeSynth: carry its settings and data folder over, once.
  migrateFromShapeSynth();

  const QString appDir = QCoreApplication::applicationDirPath();
  const QString svgIcon = appDir + QStringLiteral("/appdata/icons/application.svg");
  const QString pngIcon = appDir + QStringLiteral("/appdata/icons/application.png");
  QIcon windowIcon;
  if (QFileInfo::exists(svgIcon)) windowIcon = QIcon(svgIcon);
  if (windowIcon.isNull() && QFileInfo::exists(pngIcon)) windowIcon = QIcon(pngIcon);
  if (!windowIcon.isNull()) QApplication::setWindowIcon(windowIcon);
  // The splash is drawn inside the main window and fades out there (see SplashOverlay).
  MainWindow window;
  window.showMaximized();
  return app.exec();
}
