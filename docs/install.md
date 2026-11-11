# DaedalCAD — install, compile, and share

DaedalCAD is a native Qt 6 program. It does not need a browser, Java, or a virtual machine. The steps below are the ones that work on Windows 11 with Visual Studio 2026 Build Tools and Qt 6.8.3.

Manifold (the geometry kernel) and oneTBB (which lets Manifold use every CPU core) are downloaded by CMake the first time you configure, into `build/_deps`. That step needs a network and Git. Later configures reuse that folder, so do not delete `build/_deps` if you want to stay offline. Clipper2 ships in `third_party/Clipper2` and is not downloaded. oneTBB is linked into `DaedalCAD.exe`, so there is no extra DLL. To build without it (booleans on one core, as before 0.9), configure with `-DDAEDALCAD_PARALLEL=OFF`; if you change that on an existing `build` folder, delete `build/CMakeCache.txt` first.

## 1. Visual Studio 2026 Build Tools

You need the C++ toolset **v143** (MSVC 14.44). The generator CMake will use is `Visual Studio 18 2026`.

In an elevated PowerShell:

```powershell
Invoke-WebRequest -Uri "https://aka.ms/vs/stable/vs_BuildTools.exe" -OutFile "$env:TEMP\vs_BuildTools.exe"
Start-Process -FilePath "$env:TEMP\vs_BuildTools.exe" -ArgumentList "--wait","--passive","--norestart","--add","Microsoft.VisualStudio.Workload.VCTools","--add","Microsoft.VisualStudio.Component.VC.Tools.x86.x64","--add","Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64","--includeRecommended" -Wait -Verb RunAs
```

Also install Git (so CMake can clone Manifold) if you do not have it:

```powershell
winget install --id Git.Git -e
```

## 2. CMake 4.2 or newer

The CMake that ships with Visual Studio is too old to name the `Visual Studio 18 2026` generator. Install Kitware's CMake and put it first on PATH.

```powershell
winget install --id Kitware.CMake -e
$env:Path = "C:\Program Files\CMake\bin;" + $env:Path
cmake --version
```

`cmake --version` must be 4.2 or higher. If it is not, the old cmake is still winning. Keep the `C:\Program Files\CMake\bin` prefix.

## 3. Qt 6.8.3 (MSVC 64-bit)

Use aqt. Do **not** pass `-m qtsvg` or `-m qtwayland`. On Windows, SVG is already in the base kit, and Qt Wayland does not exist. Asking for either one aborts the whole install.

```powershell
python -m venv "$env:LOCALAPPDATA\aqt-venv"
& "$env:LOCALAPPDATA\aqt-venv\Scripts\python.exe" -m pip install --upgrade pip aqtinstall
& "$env:LOCALAPPDATA\aqt-venv\Scripts\aqt.exe" install-qt windows desktop 6.8.3 win64_msvc2022_64 -O "$env:USERPROFILE\Qt"
```

That creates:

```text
%USERPROFILE%\Qt\6.8.3\msvc2022_64
```

If a previous install stopped halfway, delete that folder and run the aqt command again.

Save this as `%USERPROFILE%\qt-env.ps1`:

```powershell
$env:QTDIR = Join-Path $env:USERPROFILE "Qt\6.8.3\msvc2022_64"
$env:Path = "$env:QTDIR\bin;C:\Program Files\CMake\bin;" + $env:Path
$env:CMAKE_PREFIX_PATH = $env:QTDIR
```

## 4. Configure and compile

From the DaedalCAD directory (the folder that contains `CMakeLists.txt`):

```powershell
cd C:\path\to\DaedalCAD
$env:Path = "C:\Program Files\CMake\bin;" + $env:Path
. $env:USERPROFILE\qt-env.ps1
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -T v143
cmake --build build --config Release
```

The executable is `build\Release\DaedalCAD.exe`. It will not start yet, because the Qt DLLs are not next to it. Deploy them:

```powershell
. $env:USERPROFILE\qt-env.ps1
windeployqt --release .\build\Release\DaedalCAD.exe
.\build\Release\DaedalCAD.exe
```

Run that exe. Do not start a different `DaedalCAD.exe` from PATH. If Windows says `Qt6OpenGLWidgets.dll` is missing, `windeployqt` was not run on this exe, or you launched a copy that does not sit beside the DLLs `windeployqt` just wrote.

## 5. Rebuild after a source update

If you only replaced `.cpp` / `.h` files and did not add a new source file:

```powershell
cd C:\path\to\DaedalCAD
$env:Path = "C:\Program Files\CMake\bin;" + $env:Path
. $env:USERPROFILE\qt-env.ps1
cmake --build build --config Release
windeployqt --release .\build\Release\DaedalCAD.exe
.\build\Release\DaedalCAD.exe
```

You do not need to reconfigure. If CMakeLists.txt gained a new `.cpp`, run the configure command from section 4 again.

## 6. OpenCASCADE, for exact STEP (optional)

Without it DaedalCAD builds and runs as before: the export menu says **STEP (faceted)** and there
is no STEP import. With it the menu says **STEP** (true cylinders, spheres and holes), and
**File > Import STEP…** appears. Nothing else in the program changes.

OpenCASCADE is installed once with vcpkg, which compiles it for you. That takes 30–60 minutes and
about 6 GB of temporary space; afterwards you can delete the `buildtrees`, `downloads` and
`packages` folders in `C:\vcpkg` (about 400 MB stays).

**a. vcpkg.** Use a fresh clone: older copies do not know Visual Studio 2026.

```powershell
cd C:\
git clone https://github.com/microsoft/vcpkg.git
cd C:\vcpkg
.\bootstrap-vcpkg.bat -disableMetrics
```

**b. A triplet that uses the same compiler as DaedalCAD (v143), release only.**

```powershell
New-Item -ItemType Directory -Force C:\vcpkg-triplets | Out-Null
Set-Content -Path C:\vcpkg-triplets\x64-windows-v143.cmake -Value @(
  'set(VCPKG_TARGET_ARCHITECTURE x64)',
  'set(VCPKG_CRT_LINKAGE dynamic)',
  'set(VCPKG_LIBRARY_LINKAGE dynamic)',
  'set(VCPKG_PLATFORM_TOOLSET v143)',
  'set(VCPKG_BUILD_TYPE release)'
)
Get-Content C:\vcpkg-triplets\x64-windows-v143.cmake
```

The last line must print those five lines. If vcpkg later says *Invalid triplet:
x64-windows-v143*, this file is missing or has another name.

**c. OpenCASCADE.**

```powershell
cd C:\vcpkg
.\vcpkg install opencascade:x64-windows-v143 --overlay-triplets=C:\vcpkg-triplets
```

It ends with a few lines about `find_package(OpenCASCADE CONFIG REQUIRED)`; you do not need to do
anything with them.

**d. Tell DaedalCAD where it is.** Add this line to `%USERPROFILE%\qt-env.ps1`, so every build
finds it:

```powershell
$env:OCCT_DIR = "C:\vcpkg\installed\x64-windows-v143"
```

**e. Configure and build again** (the configure from section 4, with OpenCASCADE required, so a
mistake stops here instead of quietly building without it):

```powershell
cd C:\path\to\DaedalCAD
$env:Path = "C:\Program Files\CMake\bin;" + $env:Path
. $env:USERPROFILE\qt-env.ps1
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -T v143 -DDAEDALCAD_WITH_OCCT=ON
cmake --build build --config Release
windeployqt --release .\build\Release\DaedalCAD.exe
.\build\Release\DaedalCAD.exe
```

The configure prints `OpenCASCADE 8.0.1: exact STEP export and STEP import` (or whichever version
vcpkg installed). The build copies the OpenCASCADE DLLs (`TK*.dll`, plus `freetype.dll`, `zlib1.dll`
and a few others) next to `DaedalCAD.exe`, and their licenses into `build\Release\licenses`, so
`package.ps1` ships them without any change. The zip grows by about 60 MB.

If the configure says *OpenCASCADE not found*, check that
`C:\vcpkg\installed\x64-windows-v143\share\opencascade\OpenCASCADEConfig.cmake` exists and that
`$env:OCCT_DIR` is set in this window. If DaedalCAD will not start and names a missing `TK...dll`,
the post-build copy did not run: build once more.

To build without it again: `-DDAEDALCAD_WITH_OCCT=OFF`.

## 7. Giving it to someone else

Qt is LGPL. The normal, legal, and practical way to ship a Qt Widgets app is **the exe plus the DLLs next to it**. The other person does not install Qt, Visual Studio, or CMake. They unzip a folder and run `DaedalCAD.exe`.

```powershell
cd C:\path\to\DaedalCAD
. $env:USERPROFILE\qt-env.ps1
powershell -ExecutionPolicy Bypass -File .\package.ps1
```

That writes, next to the script, using the version from CMakeLists.txt:

- `DaedalCAD-<version>-windows-x64.zip` — unzip anywhere and run `DaedalCAD.exe`.
- `DaedalCAD-<version>-setup.exe` — the installer, when Inno Setup 6 is installed.

## 8. Visual Studio 2022 instead

With Visual Studio 2022 (workload **Desktop development with C++**) and the Qt 6.8 MSVC 64-bit kit,
`build-windows.bat` in the project folder configures, builds and deploys in one go from a
**Developer PowerShell for VS 2022**:

```bat
set QTDIR=C:\Qt\6.8.3\msvc2022_64
build-windows.bat
```

For OpenCASCADE (section 6), `set OCCT_DIR=C:\vcpkg\installed\x64-windows-v143` before it and run
`build-windows.bat reconfigure` once.

It configures only the first time; after that `cmake --build` configures again by itself when
`CMakeLists.txt` changes. `build-windows.bat reconfigure` starts the configuration over (after
changing QTDIR, for example).

The configure step keeps two kinds of notes out of its output on purpose: Qt's search for the
Vulkan headers (DaedalCAD draws with OpenGL only), and Manifold's and oneTBB's warnings that
oneTBB is linked statically (that is deliberate: no extra DLL to ship) plus oneTBB's old CMake
policy. Every other message prints as usual.

## 9. Licenses

DaedalCAD is GPL-3.0 (`LICENSE`); the server in `webapp/` is AGPL-3.0. Qt (LGPL-3.0), Manifold
(Apache-2.0), Clipper2 (BSL-1.0) and, in builds with it, OpenCASCADE (LGPL-2.1 with an exception)
are listed in `THIRD_PARTY_LICENSES.md`. `package.ps1` puts
both files in the zip and the installer.
