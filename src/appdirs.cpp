#include "appdirs.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

QString programFolder() { return QCoreApplication::applicationDirPath(); }

bool isInstalledCopy() { return QFileInfo::exists(programFolder() + QStringLiteral("/installed.ini")); }

QString resourcePath(const QString& name) {
  return programFolder() + QDir::separator() + QStringLiteral("appdata") + QDir::separator() + name;
}

QString userDataRoot() {
  if (isInstalledCopy()) {
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!documents.isEmpty()) return QDir::cleanPath(documents + QStringLiteral("/DaedalCAD"));
  }
  return programFolder() + QDir::separator() + QStringLiteral("appdata");
}

QStringList userDataFolders() {
  return {QStringLiteral("projects"), QStringLiteral("parts"),   QStringLiteral("2d_shapes"),     QStringLiteral("Imported"),
          QStringLiteral("placed_2d"), QStringLiteral("sculpts"), QStringLiteral("sculpt_library")};
}

bool hasUserData(const QString& root) {
  for (const QString& folder : userDataFolders()) {
    QDirIterator it(QDir(root).absoluteFilePath(folder), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    if (it.hasNext()) return true;
  }
  return false;
}

int copyMissingFiles(const QString& from, const QString& to) {
  const QDir source(from);
  if (!source.exists()) return 0;
  int copied = 0;
  QDirIterator it(from, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString file = it.next();
    const QString target = QDir(to).absoluteFilePath(source.relativeFilePath(file));
    if (QFileInfo::exists(target)) continue;
    if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(file, target)) return -1;
    ++copied;
  }
  return copied;
}

QString movedDataPath(const QString& path, const QString& oldRoot, const QString& newRoot) {
  if (path.isEmpty() || oldRoot.isEmpty() || newRoot.isEmpty()) return {};
  const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
  const QString base = QDir::cleanPath(QDir::fromNativeSeparators(oldRoot));
  if (!clean.startsWith(base + QLatin1Char('/'), Qt::CaseInsensitive)) return {};
  const QString rest = clean.mid(base.size() + 1);
  const QString top = rest.section(QLatin1Char('/'), 0, 0);
  bool data = false;
  for (const QString& folder : userDataFolders())
    if (top.compare(folder, Qt::CaseInsensitive) == 0) data = true;
  if (!data) return {};
  return QDir::cleanPath(QDir::fromNativeSeparators(newRoot) + QLatin1Char('/') + rest);
}

// --- project files ------------------------------------------------------------------------------

namespace {
const QString kLegacyProjectFile = QStringLiteral("shapesynth.json");
}

QString projectFileName() { return QStringLiteral("daedalcad.json"); }

bool isProjectFileName(const QString& fileName) {
  return fileName.compare(projectFileName(), Qt::CaseInsensitive) == 0 ||
         fileName.compare(kLegacyProjectFile, Qt::CaseInsensitive) == 0;
}

QString projectFileIn(const QString& folder) {
  if (folder.isEmpty()) return {};
  for (const QString& name : {projectFileName(), kLegacyProjectFile}) {
    const QString path = QDir(folder).absoluteFilePath(name);
    if (QFileInfo(path).isFile()) return path;
  }
  return {};
}

void dropLegacyProjectFile(const QString& folder) {
  const QString legacy = QDir(folder).absoluteFilePath(kLegacyProjectFile);
  if (QFileInfo(QDir(folder).absoluteFilePath(projectFileName())).isFile() && QFileInfo::exists(legacy)) QFile::remove(legacy);
}

QString projectArchiveSuffix() { return QStringLiteral("daedal"); }

QString projectArchiveFilter() {
  return QStringLiteral("DaedalCAD project (*.daedal *.shapezip *.zip)");
}

// --- the rename from ShapeSynth -----------------------------------------------------------------

namespace {

// `path` under `from` (case-insensitive, either slash) moved to `to`; unchanged otherwise.
QString rebase(const QString& path, const QString& from, const QString& to) {
  const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
  const QString base = QDir::cleanPath(QDir::fromNativeSeparators(from));
  if (clean.compare(base, Qt::CaseInsensitive) == 0) return QDir::cleanPath(to);
  if (!clean.startsWith(base + QLatin1Char('/'), Qt::CaseInsensitive)) return path;
  return QDir::cleanPath(QDir::fromNativeSeparators(to) + QLatin1Char('/') + clean.mid(base.size() + 1));
}

}  // namespace

void migrateFromShapeSynth() {
  QSettings settings;
  if (settings.value(QStringLiteral("migratedFromShapeSynth")).toBool()) return;

  // 1. Settings: copy them once when this name has none of its own yet.
  QSettings legacy(QStringLiteral("ShapeSynth"), QStringLiteral("ShapeSynth"));
  if (settings.allKeys().isEmpty()) {
    for (const QString& key : legacy.allKeys()) settings.setValue(key, legacy.value(key));
  }

  // 2. Data: an installed copy kept it in Documents\ShapeSynth.
  if (isInstalledCopy()) {
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString oldRoot = documents.isEmpty() ? QString() : QDir::cleanPath(documents + QStringLiteral("/ShapeSynth"));
    const QString newRoot = QDir::cleanPath(userDataRoot());
    if (!oldRoot.isEmpty() && oldRoot.compare(newRoot, Qt::CaseInsensitive) != 0 && hasUserData(oldRoot)) {
      bool moved = false;
      if (!QFileInfo::exists(newRoot)) moved = QDir().rename(oldRoot, newRoot);
      if (!moved) {
        // The folder is in use or the new one exists already: copy what is missing, keep the old one.
        for (const QString& folder : userDataFolders())
          copyMissingFiles(QDir(oldRoot).absoluteFilePath(folder), QDir(newRoot).absoluteFilePath(folder));
      }
      auto remapList = [&](const QString& key) {
        QStringList list = settings.value(key).toStringList();
        for (QString& path : list) path = rebase(path, oldRoot, newRoot);
        list.removeDuplicates();
        settings.setValue(key, list);
      };
      for (const QString& key : {QStringLiteral("partsDirectories"), QStringLiteral("twoDShapesDirectories"),
                                 QStringLiteral("recentProjects")})
        if (settings.contains(key)) remapList(key);
      settings.setValue(QStringLiteral("renamedDataFrom"), oldRoot);
    }
  }
  settings.setValue(QStringLiteral("migratedFromShapeSynth"), true);
}

QString renamedDataFrom() { return QSettings().value(QStringLiteral("renamedDataFrom")).toString(); }
