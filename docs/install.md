# DaedalCAD — build it on Windows

DaedalCAD is a native Qt 6 program for Windows. To build it you need Git, the Visual Studio C++
build tools (toolset v143), CMake and Qt 6.8.3. Optionally, OpenCASCADE adds exact STEP export
and STEP import. One script installs whatever is missing and builds the program.

## 1. Quick start

1. **Get the source.** Clone it with Git, or download the repository as a zip and unpack it:

   ```powershell
   git clone https://github.com/Ruhnay/DaedalCAD.git
   ```

2. **Double-click `build-windows.bat`** in the DaedalCAD folder. Or, in PowerShell:

   ```powershell
   cd C:\path\to\DaedalCAD
   powershell -ExecutionPolicy Bypass -File .\setup-windows.ps1
   ```

3. **Run it:** `build\Release\DaedalCAD.exe`.

The first run takes a while: it installs what is missing (see section 2) and downloads Manifold.
Windows asks once for administrator rights, for the Visual Studio Build Tools; everything else goes
into your own user folders. After that, run the same command whenever the source changes: what is
installed is skipped, so it only builds.

Options, as switches for the script or as words after `build-windows.bat`:

| Script | `build-windows.bat` | What it does |
|---|---|---|
| `-WithStep` | `step` | Also OpenCASCADE, for exact STEP export and STEP import. The first time takes 30–60 minutes and about 6 GB of temporary space. |
| `-Package` | `package` | Also make the release zip (and the installer, if Inno Setup is installed). See section 4. |
| `-Run` | `run` | Start DaedalCAD when it is built. |
| `-Reconfigure` | `reconfigure` | Start the CMake configuration over. The Manifold download is kept. |
| `-CheckOnly` | `check` | Only report what is installed and what is missing. |

For example `build-windows.bat step run`, or `.\setup-windows.ps1 -WithStep -Run`. Once
OpenCASCADE is installed, later builds use it without the option.

## 2. What the script installs, and where

| What | How | Where |
|---|---|---|
| Git | winget | Program Files |
| Visual Studio Build Tools with the C++ toolset v143 (skipped if Visual Studio 2022 or 2026 with C++ is already there; v143 is added to a 2026 install that lacks it) | Microsoft's installer | Program Files (x86) |
| CMake 4.2 or newer (3.24 is enough with Visual Studio 2022) | winget | Program Files |
| Python 3, only to run aqt | winget | your user folder |
| Qt 6.8.3, MSVC 64-bit | aqt | `%USERPROFILE%\Qt\6.8.3\msvc2022_64` |
| OpenCASCADE (only with `-WithStep`) | vcpkg | `C:\vcpkg`, with the triplet in `C:\vcpkg-triplets` |

It also writes `%USERPROFILE%\qt-env.ps1`. Run `. $env:USERPROFILE\qt-env.ps1` in any PowerShell
window and Qt, CMake and OpenCASCADE are on PATH there, for typing the commands of section 3 by
hand.

Manifold (the geometry kernel) and oneTBB (which lets Manifold use every CPU core) are downloaded
by CMake into `build\_deps` the first time. Keep that folder if you want to build offline later.
Clipper2 is in `third_party\Clipper2` and is not downloaded.

Other Qt locations: `-QtVersion` and `-QtRoot` (for example `-QtRoot C:\Qt` for a Qt installed
with the Qt installer). Another vcpkg folder: `-VcpkgRoot`.

## 3. By hand

The script does exactly these steps. Use them if you prefer to install things yourself, or to see
what went wrong.

**a. Visual Studio Build Tools with the C++ toolset v143.** In an elevated PowerShell:

```powershell
Invoke-WebRequest -Uri "https://aka.ms/vs/stable/vs_BuildTools.exe" -OutFile "$env:TEMP\vs_BuildTools.exe"
Start-Process -FilePath "$env:TEMP\vs_BuildTools.exe" -ArgumentList "--wait","--passive","--norestart","--add","Microsoft.VisualStudio.Workload.VCTools","--add","Microsoft.VisualStudio.Component.VC.Tools.x86.x64","--add","Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64","--includeRecommended" -Wait -Verb RunAs
```

With Visual Studio 2022 (workload **Desktop development with C++**) you already have v143.

**b. Git and CMake.**

```powershell
winget install --id Git.Git -e
winget install --id Kitware.CMake -e
```

The CMake inside Visual Studio 2026 is too old to name its own generator; use Kitware's, first on
PATH: `$env:Path = "C:\Program Files\CMake\bin;" + $env:Path`. `cmake --version` must say 4.2 or
newer (3.24 with Visual Studio 2022).

**c. Qt 6.8.3 with aqt.** Do not add `-m qtsvg` or `-m qtwayland`: SVG is in the base kit on
Windows and Qt Wayland does not exist there, and asking for either stops the whole install.

```powershell
python -m venv "$env:LOCALAPPDATA\aqt-venv"
& "$env:LOCALAPPDATA\aqt-venv\Scripts\python.exe" -m pip install --upgrade pip aqtinstall
& "$env:LOCALAPPDATA\aqt-venv\Scripts\aqt.exe" install-qt windows desktop 6.8.3 win64_msvc2022_64 -O "$env:USERPROFILE\Qt"
```

If an earlier install stopped halfway, delete `%USERPROFILE%\Qt\6.8.3\msvc2022_64` and run it again.

**d. Configure, build and deploy** (with Visual Studio 2022: `-G "Visual Studio 17 2022"` and no `-T v143`):

```powershell
cd C:\path\to\DaedalCAD
$env:QTDIR = "$env:USERPROFILE\Qt\6.8.3\msvc2022_64"
$env:Path = "$env:QTDIR\bin;C:\Program Files\CMake\bin;" + $env:Path
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -T v143 -DCMAKE_PREFIX_PATH="$env:QTDIR"
cmake --build build --config Release --parallel
windeployqt --release .\build\Release\DaedalCAD.exe
.\build\Release\DaedalCAD.exe
```

Later builds need only the last three lines. Configure again only when you change options;
the build configures again by itself when `CMakeLists.txt` changes.

**e. OpenCASCADE (optional).** Without it the export menu says **STEP (faceted)** and there is no
STEP import; with it, STEP has true cylinders, spheres and holes, and **File > Import STEP…**
appears. Nothing else changes.

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
New-Item -ItemType Directory -Force C:\vcpkg-triplets | Out-Null; Set-Content -Path C:\vcpkg-triplets\x64-windows-v143.cmake -Value @('set(VCPKG_TARGET_ARCHITECTURE x64)','set(VCPKG_CRT_LINKAGE dynamic)','set(VCPKG_LIBRARY_LINKAGE dynamic)','set(VCPKG_PLATFORM_TOOLSET v143)','set(VCPKG_BUILD_TYPE release)')
C:\vcpkg\vcpkg.exe install opencascade:x64-windows-v143 --overlay-triplets=C:\vcpkg-triplets
$env:OCCT_DIR = "C:\vcpkg\installed\x64-windows-v143"
```

Then configure as in d with `-DDAEDALCAD_WITH_OCCT=ON` added, and build. The configure prints
`OpenCASCADE 8.0.1: exact STEP export and STEP import` (or whichever version vcpkg installed). The
build copies the OpenCASCADE DLLs and their licences next to `DaedalCAD.exe`, so packaging needs no
change; the zip grows by about 60 MB. Afterwards you can delete the `buildtrees`, `downloads` and
`packages` folders in `C:\vcpkg` (about 400 MB stays). To build without it again:
`-DDAEDALCAD_WITH_OCCT=OFF`.

## 4. Giving it to someone else

The other person does not install Qt, Visual Studio or CMake: they get the exe with the DLLs next
to it (the normal, legal way to ship a Qt program under the LGPL).

```powershell
.\setup-windows.ps1 -Package
```

or `build-windows.bat package`, or on its own after a build: `powershell -ExecutionPolicy Bypass -File .\package.ps1`.
That writes, next to the script, with the version from `CMakeLists.txt`:

- `DaedalCAD-<version>-windows-x64.zip`: unzip anywhere and run `DaedalCAD.exe`.
- `DaedalCAD-<version>-setup.exe`: the installer, when Inno Setup 6 is installed
  (`winget install JRSoftware.InnoSetup`).

Your own projects, imported files and placed 2D shapes are never included. The top of
`package.ps1` lists its options.

**Publishing a release on GitHub.** DaedalCAD's update check reads the latest release of the
repository (it needs no login, so the repository has to be public):

1. Set the new version in `CMakeLists.txt` (`project(DaedalCAD VERSION …)`), commit and push, then
   run `build-windows.bat step package`.
2. On GitHub: **Releases** > **Draft a new release** > **Choose a tag**, type `v<version>` (for
   example `v0.9.8`) and pick **Create new tag on publish**, on `main`.
3. Write the title and the notes: the notes are what the update window shows.
4. Drag `DaedalCAD-<version>-setup.exe` and `DaedalCAD-<version>-windows-x64.zip` into the box at
   the bottom, wait for both, and press **Publish release**.

Installed copies then update themselves. They use the installer only when its name is
`DaedalCAD-<version>-setup.exe` (or ends in `-setup.exe`) and the tag is the version, with or
without the `v`. Drafts and pre-releases are not offered. A fork builds with its own repository:
`-DDAEDALCAD_UPDATE_REPO=owner/name`.

## 5. When something goes wrong

- **Scripts are blocked** ("running scripts is disabled"): use `build-windows.bat`, or
  `powershell -ExecutionPolicy Bypass -File .\setup-windows.ps1`.
- **winget is missing:** install **App Installer** from the Microsoft Store, or install Git, CMake
  and Python yourself and run the script again.
- **`cmake --version` is older than 4.2** with Visual Studio 2026: the CMake inside Visual Studio is
  winning. Put `C:\Program Files\CMake\bin` first on PATH (the script does this for itself).
- **DaedalCAD will not start and names a missing `Qt6...dll`:** `windeployqt` did not run on this
  exe, or you started a copy that is not next to the DLLs. Start `build\Release\DaedalCAD.exe`.
- **It names a missing `TK...dll`:** the OpenCASCADE copy after the build did not run. Build once more.
- **"Invalid triplet: x64-windows-v143":** `C:\vcpkg-triplets\x64-windows-v143.cmake` is missing or
  has another name.
- **"OpenCASCADE not found"** with `-DDAEDALCAD_WITH_OCCT=ON`: check that
  `C:\vcpkg\installed\x64-windows-v143\share\opencascade\OpenCASCADEConfig.cmake` exists and that
  `OCCT_DIR` is set in that window.
- **A build folder made by another Visual Studio:** the script notices and configures again.
  By hand: delete `build\CMakeCache.txt` and `build\CMakeFiles` (keep `build\_deps`).

The configure step hides two kinds of notes on purpose: Qt's search for the Vulkan headers
(DaedalCAD draws with OpenGL only), and the warnings that oneTBB is linked statically (deliberate:
no extra DLL to ship). To build without oneTBB (booleans on one core), configure with
`-DDAEDALCAD_PARALLEL=OFF` after deleting `build\CMakeCache.txt`.

## 6. Licences

DaedalCAD is GPL-3.0 (`LICENSE`); the server in `webapp/` is AGPL-3.0. Qt (LGPL-3.0), Manifold
(Apache-2.0), Clipper2 (BSL-1.0) and, in builds with it, OpenCASCADE (LGPL-2.1 with an exception)
are listed in `THIRD_PARTY_LICENSES.md`. `package.ps1` puts both files in the zip and the installer.
