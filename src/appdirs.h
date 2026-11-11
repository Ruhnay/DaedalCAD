#pragma once

// Where DaedalCAD keeps things.
//
// Program resources (icons, help pictures, the splash) always ship in appdata/ next to the exe.
//
// Your own data (projects, parts and 2D shape libraries, Imported, placed 2D shapes):
//  - an installed copy (the installer leaves installed.ini next to the exe): Documents\DaedalCAD,
//    so installing, updating or uninstalling never touches it;
//  - a portable copy (unzipped or built): appdata/ next to the exe, as before.

#include <QString>
#include <QStringList>

QString programFolder();
bool isInstalledCopy();
QString resourcePath(const QString& name);  // e.g. resourcePath("icons")
QString userDataRoot();
QStringList userDataFolders();              // projects, parts, 2d_shapes, Imported, placed_2d, sculpts, sculpt_library
bool hasUserData(const QString& root);      // any file in one of those folders
// Copies every file under `from` into `to`, keeping the layout and leaving files that already
// exist alone. Returns the number of files copied, or -1 when one could not be copied.
int copyMissingFiles(const QString& from, const QString& to);
// The same file under another data root: <old>/Imported/x.stl -> <new>/Imported/x.stl, or empty
// when `path` is not inside one of the user data folders of `oldRoot`.
QString movedDataPath(const QString& path, const QString& oldRoot, const QString& newRoot);

// Project files. A project folder holds daedalcad.json and an exported project is a .daedal file.
// DaedalCAD used to be called ShapeSynth, which wrote shapesynth.json and .shapezip; both are still
// read, so old projects and old CloudSync uploads keep opening.
QString projectFileName();                         // "daedalcad.json", what is written
bool isProjectFileName(const QString& fileName);   // the new or the old name, any case
QString projectFileIn(const QString& folder);      // the project file in a folder (new name first), or empty
void dropLegacyProjectFile(const QString& folder); // after a save: remove an old shapesynth.json beside it
QString projectArchiveSuffix();                    // "daedal"
QString projectArchiveFilter();                    // file dialog filter that opens new and old archives

// Once, at start-up, before anything reads settings: copies the settings of ShapeSynth (the old
// name) and, for an installed copy, moves Documents\ShapeSynth to Documents\DaedalCAD and points the
// remembered library folders and recent projects there. Does nothing on later starts.
void migrateFromShapeSynth();
// Where migrateFromShapeSynth moved user data from (empty when it did not), for paths stored inside
// projects that still point into the old folder.
QString renamedDataFrom();
