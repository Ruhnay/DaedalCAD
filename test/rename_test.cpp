// The rename from ShapeSynth to DaedalCAD: an installed ShapeSynth's settings are copied, its
// Documents\ShapeSynth folder is moved to Documents\DaedalCAD, remembered paths follow, old project
// files are still found, and it all happens once. Runs in a throw-away home folder.

#include "appdirs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  } else {
    std::printf("ok: %s\n", what.c_str());
  }
}

void write(const QString& path, const QByteArray& data) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  if (file.open(QIODevice::WriteOnly)) file.write(data);
}

}  // namespace

int main(int argc, char** argv) {
  QTemporaryDir home;
  if (!home.isValid()) return 1;
  qputenv("HOME", home.path().toUtf8());
  qputenv("XDG_CONFIG_HOME", (home.path() + "/.config").toUtf8());
  qputenv("XDG_DATA_HOME", (home.path() + "/.local/share").toUtf8());
  QCoreApplication app(argc, argv);

  // Pretend this is an installed copy.
  const QString marker = QCoreApplication::applicationDirPath() + "/installed.ini";
  write(marker, "[DaedalCAD]\ninstalled=test\n");
  struct Cleanup {
    QString path;
    ~Cleanup() { QFile::remove(path); }
  } cleanup{marker};

  const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  const QString oldRoot = documents + "/ShapeSynth";
  const QString newRoot = documents + "/DaedalCAD";
  write(oldRoot + "/projects/Bracket/shapesynth.json", R"({"nodes":[]})");
  write(oldRoot + "/Imported/logo.svg", "<svg/>");
  write(oldRoot + "/parts/clip.stl", "solid x\nendsolid x\n");
  {
    QSettings legacy("ShapeSynth", "ShapeSynth");
    legacy.setValue("printerClearance", 0.05);
    legacy.setValue("cloudHost", "sync.example.org");
    legacy.setValue("partsDirectories", QStringList{oldRoot + "/parts", "/elsewhere/parts"});
    legacy.setValue("recentProjects", QStringList{oldRoot + "/projects/Bracket/shapesynth.json"});
    legacy.sync();
  }

  QCoreApplication::setOrganizationName("DaedalCAD");
  QCoreApplication::setApplicationName("DaedalCAD");
  check(QDir::cleanPath(userDataRoot()) == QDir::cleanPath(newRoot), "an installed copy keeps its data in Documents/DaedalCAD");
  migrateFromShapeSynth();

  QSettings settings;
  check(settings.value("printerClearance").toDouble() == 0.05, "settings copied: printer clearance");
  check(settings.value("cloudHost").toString() == "sync.example.org", "settings copied: CloudSync host");
  check(QFileInfo::exists(newRoot + "/projects/Bracket/shapesynth.json"), "projects moved to Documents/DaedalCAD");
  check(QFileInfo::exists(newRoot + "/Imported/logo.svg"), "imported files moved");
  check(!QFileInfo::exists(oldRoot), "the old folder is gone after a move");
  const QStringList parts = settings.value("partsDirectories").toStringList();
  check(parts.size() == 2 && QDir::cleanPath(parts.at(0)) == QDir::cleanPath(newRoot + "/parts") && parts.at(1) == "/elsewhere/parts",
        "library folders inside the old data folder follow it, others stay");
  const QStringList recent = settings.value("recentProjects").toStringList();
  check(recent.size() == 1 && QFileInfo::exists(recent.at(0)), "recent projects point at the moved files");
  check(QDir::cleanPath(renamedDataFrom()) == QDir::cleanPath(oldRoot), "remembers where the data came from");
  check(movedDataPath(oldRoot + "/Imported/logo.svg", renamedDataFrom(), userDataRoot()) == QDir::cleanPath(newRoot + "/Imported/logo.svg"),
        "paths saved inside old projects are found again");

  // Old project files are still projects; a save in the new name drops the old file.
  const QString bracket = newRoot + "/projects/Bracket";
  check(projectFileIn(bracket).endsWith("shapesynth.json"), "an old shapesynth.json is found");
  check(isProjectFileName("shapesynth.json") && isProjectFileName("DaedalCAD.json") && !isProjectFileName("other.json"),
        "both project file names are recognised");
  write(bracket + "/" + projectFileName(), R"({"nodes":[]})");
  check(projectFileIn(bracket).endsWith("daedalcad.json"), "the new name wins when both exist");
  dropLegacyProjectFile(bracket);
  check(!QFileInfo::exists(bracket + "/shapesynth.json"), "saving in the new format removes the old file");

  // Once only: a later start changes nothing, even with new ShapeSynth settings or folders around.
  settings.setValue("printerClearance", 0.1);
  {
    QSettings legacy("ShapeSynth", "ShapeSynth");
    legacy.setValue("printerClearance", 0.3);
  }
  write(oldRoot + "/projects/Late/shapesynth.json", "{}");
  migrateFromShapeSynth();
  check(QSettings().value("printerClearance").toDouble() == 0.1, "a second start does not copy settings again");
  check(!QFileInfo::exists(newRoot + "/projects/Late"), "a second start does not move folders again");

  // When Documents/DaedalCAD already exists, the files are copied instead and nothing is overwritten.
  QSettings().remove("migratedFromShapeSynth");
  write(newRoot + "/parts/clip.stl", "mine");
  write(oldRoot + "/parts/clip.stl", "theirs");
  migrateFromShapeSynth();
  check(QFileInfo::exists(newRoot + "/projects/Late/shapesynth.json"), "into an existing folder: missing files are copied");
  QFile clip(newRoot + "/parts/clip.stl");
  check(clip.open(QIODevice::ReadOnly) && clip.readAll() == "mine", "into an existing folder: files already there are kept");
  check(QFileInfo::exists(oldRoot + "/projects/Late/shapesynth.json"), "into an existing folder: the old folder is left alone");

  std::printf(failures ? "%d failure(s)\n" : "all rename checks passed\n", failures);
  return failures ? 1 : 0;
}
