#pragma once

// Updates, from GitHub releases (the default) or from your own DaedalSync server.
//
// GitHub: DaedalCAD asks api.github.com for the latest release of DAEDALCAD_GITHUB_REPO (no login;
// the repository must be public). The release's tag is the version ("v0.9.8" or "0.9.8"), its text
// the notes, and its DaedalCAD-<version>-setup.exe asset the installer, checked against the SHA-256
// GitHub lists for it. DaedalSync: with the CloudSync key when there is one (/v1/app/latest),
// otherwise with a download login (/downloads/latest).
//
// An installed copy then downloads the installer, checks its SHA-256, runs it silently and closes;
// the installer replaces the program files and starts DaedalCAD again. A portable copy only says a
// new version is out and opens the release or download page.

#include <QString>
#include <QStringList>
#include <functional>

class QWidget;

struct UpdateInfo {
  QString version;  // empty: the server has no versions
  QString notes;
  QString added;
  bool hasInstaller = false;
  qint64 installerBytes = 0;
  QString installerSha256;
  QString installerUrl;  // GitHub: where the installer is; empty for DaedalSync
  QString pageUrl;       // GitHub: the release page
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
