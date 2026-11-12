#pragma once

// Updates from GitHub releases.
//
// DaedalCAD asks api.github.com for the latest release of DAEDALCAD_GITHUB_REPO (no login; the
// repository must be public). The release's tag is the version ("v0.9.8" or "0.9.8"), its text the
// notes, and its DaedalCAD-<version>-setup.exe asset the installer. An installed copy downloads the
// installer, checks it against the SHA-256 GitHub lists for it, runs it silently and closes; the
// installer replaces the program files and starts DaedalCAD again. A portable copy only says a new
// version is out and opens the release page.

#include <QString>
#include <QStringList>
#include <functional>

class QWidget;

struct UpdateInfo {
  QString version;  // empty: the release tag is not a version
  QString notes;
  QString added;
  bool hasInstaller = false;
  qint64 installerBytes = 0;
  QString installerSha256;
  QString installerUrl;  // where the installer is
  QString pageUrl;       // the release page
  bool hasZip = false;
};

// -1, 0 or 1, like strcmp: "1.10.0" is newer than "1.9.2".
int compareVersions(const QString& a, const QString& b);
QString currentAppVersion();

struct UpdateHooks {
  // Called before DaedalCAD closes for an update; false cancels (e.g. the user wants to save first and cancels).
  std::function<bool()> beforeQuit;
  // Starts the installer. Default: QProcess::startDetached.
  std::function<bool(const QString& program, const QStringList& arguments)> launch;
  // Closes DaedalCAD. Default: QApplication::quit.
  std::function<void()> quit;
};

// The Updates window: connection, "check at start", check now, notes, install.
void openUpdatesDialog(QWidget* parent, const UpdateHooks& hooks);
// A quiet check a few seconds after start-up: only speaks up when a newer version is there.
void checkForUpdatesAtStartup(QWidget* parent, const UpdateHooks& hooks);
