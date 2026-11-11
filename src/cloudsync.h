#pragma once

// CloudSync: personal backups, the public project list, and update requests. See docs/cloudsync.md.

#include "evaluator.h"

#include <QByteArray>
#include <QImage>
#include <QString>

#include <functional>
#include <vector>

class QWidget;

// What the CloudSync window needs from the main window.
struct CloudSyncHooks {
  std::function<QString(const QString& name)> appdataPath;  // e.g. appdataPath("projects")
  std::function<QString()> currentProjectFolder;            // the open project's folder, or empty
  std::function<bool()> saveCurrent;                        // saves the open project; false if cancelled
  std::function<QByteArray(const QString& folder, QString* error)> packFolder;  // .daedal bytes of a project folder
  std::function<QImage(const QString& folder)> renderPreview;                   // picture of a project folder
  // Installs a .daedal (or an old .shapezip) as appdata/projects/<name>; opens it when asked.
  std::function<bool(const QString& zipPath, const QString& name, bool openAfter, QString* error)> installZip;
  std::function<void(const QString& text)> status;
};

// The CloudSync window. page: 0 Personal, 1 Public, 2 Update requests.
void openCloudSyncWindow(QWidget* parent, const CloudSyncHooks& hooks, int page = 0);

// Called once after start-up: when CloudSync is on, checks the server in the background and shows
// a popup for update requests on your projects and decisions on your requests you have not seen.
void checkCloudSyncAtStartup(QWidget* parent, const CloudSyncHooks& hooks);

// A three-quarter view of the parts in `items` (holes left out), drawn on the CPU so it works for
// any project folder without the 3D view.
QImage renderPreviewImage(const std::vector<DrawItem>& items, const QSize& size = QSize(640, 480));
