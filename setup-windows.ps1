# DaedalCAD: set up a Windows PC to build DaedalCAD, and build it, in one go.
#
#   .\setup-windows.ps1              install what is missing, then configure, build and deploy
#   .\setup-windows.ps1 -WithStep    also OpenCASCADE, for exact STEP export and STEP import
#                                    (the first time takes 30-60 minutes and ~6 GB of temporary space)
#   .\setup-windows.ps1 -Package     also make the release zip (and installer) with package.ps1
#   .\setup-windows.ps1 -Run         start DaedalCAD when it is built
#   .\setup-windows.ps1 -Reconfigure start the CMake configuration over (keeps the downloaded Manifold)
#   .\setup-windows.ps1 -CheckOnly   only report what is installed and what is missing
#
# Run it again whenever the source changes: what is already installed is skipped, so it only
# builds. It needs an internet connection the first time. Only the Visual Studio Build Tools step
# asks for administrator rights (a Windows prompt); everything else goes in your own user folders.
#
# If scripts are blocked:  powershell -ExecutionPolicy Bypass -File .\setup-windows.ps1
# Or double-click build-windows.bat, which runs this script.
# Works in Windows PowerShell 5.1 and PowerShell 7.

[CmdletBinding(PositionalBinding = $false)]
param(
  [switch]$WithStep,
  [switch]$Package,
  [switch]$Run,
  [switch]$Reconfigure,
  [switch]$CheckOnly,
  [string]$QtVersion = "6.8.3",
  [string]$QtRoot = (Join-Path $env:USERPROFILE "Qt"),
  [string]$VcpkgRoot = "C:\vcpkg",
  # Words instead of switches, for build-windows.bat: step, package, run, reconfigure, check.
  [Parameter(ValueFromRemainingArguments = $true)][string[]]$Words
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # Invoke-WebRequest is many times faster without its progress bar
foreach ($word in @($Words)) {
  switch -Regex ($word) {
    '^(-)?step$' { $WithStep = $true }
    '^(-)?package$' { $Package = $true }
    '^(-)?run$' { $Run = $true }
    '^(-)?reconfigure$' { $Reconfigure = $true }
    '^(-)?check(only)?$' { $CheckOnly = $true }
    default { if ($word) { Write-Host "Unknown option: $word" -ForegroundColor Yellow } }
  }
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"
$started = Get-Date
$problems = @()

function Say([string]$text) { Write-Host $text }
function Step([string]$text) { Write-Host ""; Write-Host "== $text" -ForegroundColor Cyan }
function Good([string]$text) { Write-Host "   $text" -ForegroundColor Green }
function Note([string]$text) { Write-Host "   $text" -ForegroundColor Yellow }
function Fail([string]$text) {
  Write-Host ""
  Write-Host $text -ForegroundColor Red
  Write-Host "Nothing else was changed. Fix the above and run the script again: it carries on where it stopped." -ForegroundColor Red
  exit 1
}

# Programs installed a moment ago are only on PATH in new windows: read PATH again from Windows.
function Update-SessionPath {
  $machine = [Environment]::GetEnvironmentVariable("Path", "Machine")
  $user = [Environment]::GetEnvironmentVariable("Path", "User")
  $env:Path = "$machine;$user"
}

function Install-WithWinget([string]$id, [string]$what) {
  if ($CheckOnly) { $script:problems += "$what is missing"; Note "$what is missing (would install $id)"; return $false }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail ("$what is missing, and winget (App Installer) is not available to install it. Install $what yourself, " +
          "or get App Installer from the Microsoft Store, then run this again.")
  }
  Say "   Installing $what with winget ($id)..."
  & winget install --id $id -e --silent --accept-package-agreements --accept-source-agreements | Out-Host
  # winget returns non-zero for "already installed" too; whether it worked is checked afterwards.
  Update-SessionPath
  return $true
}

if ($env:OS -ne "Windows_NT") { Fail "This script is for Windows. On other systems, follow docs/install.md by hand." }
Set-Location $root
Say "DaedalCAD setup in $root"

# --- 1. Git ---------------------------------------------------------------------------------------
Step "Git (CMake uses it to download Manifold)"
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
  if (Install-WithWinget "Git.Git" "Git") {
    $gitDir = Join-Path $env:ProgramFiles "Git\cmd"
    if (Test-Path $gitDir) { $env:Path = "$gitDir;$env:Path" }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "Git did not install. Install it from https://git-scm.com and run this again." }
  }
}
if (Get-Command git -ErrorAction SilentlyContinue) { Good ((& git --version) -join " ") }

# --- 2. Visual Studio C++ build tools (v143) ------------------------------------------------------
Step "Visual Studio C++ build tools (toolset v143)"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
function Find-VisualStudio {
  if (-not (Test-Path $vswhere)) { return $null }
  $json = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json -utf8
  # (Windows PowerShell 5.1 hands a JSON array on as one object: unpacked here.)
  $parsed = ($json | Out-String | ConvertFrom-Json)
  $all = @($parsed | ForEach-Object { $_ })
  if ($all.Count -eq 0) { return $null }
  # VS 2022 (17) or 2026 (18); the newest first.
  $usable = @($all | Where-Object { [int]($_.installationVersion.Split(".")[0]) -in 17, 18 } |
    Sort-Object { [version]$_.installationVersion } -Descending)
  if ($usable.Count -eq 0) { return $null }
  return $usable[0]
}
function Test-V143([string]$path) {
  $msvc = Join-Path $path "VC\Tools\MSVC"
  if (-not (Test-Path $msvc)) { return $false }
  return [bool](Get-ChildItem $msvc -Directory | Where-Object { $_.Name -match '^14\.(3|4)\d' })
}
$vs = Find-VisualStudio
if (-not $vs) {
  if ($CheckOnly) { $problems += "Visual Studio C++ build tools are missing"; Note "Missing (would install the Visual Studio 2026 Build Tools)" }
  else {
    Say "   Installing the Visual Studio 2026 Build Tools with the C++ toolset v143."
    Say "   Windows asks for administrator rights; the install takes 10-20 minutes."
    $bootstrapper = Join-Path $env:TEMP "vs_BuildTools.exe"
    Invoke-WebRequest -Uri "https://aka.ms/vs/stable/vs_BuildTools.exe" -OutFile $bootstrapper
    $arguments = @("--wait", "--passive", "--norestart", "--add", "Microsoft.VisualStudio.Workload.VCTools",
                   "--add", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                   "--add", "Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64", "--includeRecommended")
    Start-Process -FilePath $bootstrapper -ArgumentList $arguments -Wait -Verb RunAs
    $vs = Find-VisualStudio
    if (-not $vs) { Fail "The Visual Studio Build Tools did not install. Run this script again, or see docs/install.md, section 3a." }
  }
}
if ($vs) {
  $vsMajor = [int]($vs.installationVersion.Split(".")[0])
  if (-not (Test-V143 $vs.installationPath)) {
    # Visual Studio 2026 brings a newer toolset by default; v143 is what Qt's msvc2022 kit and the
    # OpenCASCADE triplet are built with, so it is added next to it.
    if ($CheckOnly) { $problems += "the v143 toolset is missing"; Note "The C++ toolset v143 is missing (would add it)" }
    else {
      Say "   Adding the C++ toolset v143 to $($vs.displayName) (administrator prompt)..."
      $installer = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\setup.exe"
      Start-Process -FilePath $installer -ArgumentList @("modify", "--installPath", "`"$($vs.installationPath)`"",
        "--add", "Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64", "--passive", "--norestart") -Wait -Verb RunAs
      if (-not (Test-V143 $vs.installationPath)) { Fail "Could not add the v143 toolset. Add 'MSVC v143 build tools' in the Visual Studio Installer and run this again." }
    }
  }
  if ($vsMajor -eq 18) { $generator = "Visual Studio 18 2026"; $toolset = @("-T", "v143"); $needCmake = [version]"4.2" }
  else { $generator = "Visual Studio 17 2022"; $toolset = @(); $needCmake = [version]"3.24" }
  Good "$($vs.displayName) $($vs.installationVersion) -> CMake generator `"$generator`""
}

# --- 3. CMake -------------------------------------------------------------------------------------
Step "CMake"
if (-not $needCmake) { $needCmake = [version]"4.2" }
function Find-CMake {
  $candidates = @((Join-Path $env:ProgramFiles "CMake\bin\cmake.exe"))
  $onPath = Get-Command cmake -ErrorAction SilentlyContinue
  if ($onPath) { $candidates += $onPath.Source }
  foreach ($candidate in $candidates) {
    if ($candidate -and (Test-Path $candidate)) {
      $text = (& $candidate --version | Select-Object -First 1)
      if ($text -match '(\d+\.\d+(\.\d+)?)') {
        $found = [version]$Matches[1]
        if ($found -ge $needCmake) { return @{ Path = $candidate; Version = $found } }
      }
    }
  }
  return $null
}
$cmake = Find-CMake
if (-not $cmake) {
  Note "No CMake $needCmake or newer (the one inside Visual Studio is too old for its own newest generator)."
  if (Install-WithWinget "Kitware.CMake" "CMake") {
    $cmake = Find-CMake
    if (-not $cmake) { Fail "CMake did not install. Install it from https://cmake.org/download and run this again." }
  }
}
if ($cmake) {
  $env:Path = (Split-Path $cmake.Path) + ";" + $env:Path
  Good "CMake $($cmake.Version) at $($cmake.Path)"
}

# --- 4. Qt ----------------------------------------------------------------------------------------
Step "Qt $QtVersion (MSVC 64-bit)"
$qtDir = Join-Path $QtRoot "$QtVersion\msvc2022_64"
$qtConfig = Join-Path $qtDir "lib\cmake\Qt6\Qt6Config.cmake"
if (-not (Test-Path $qtConfig)) {
  if ($CheckOnly) { $problems += "Qt $QtVersion is missing"; Note "Missing: $qtDir (would install it with aqt)" }
  else {
    # aqt needs Python. The 'python' that the Microsoft Store puts on PATH is only a pointer to the
    # Store, so it is tried, not trusted.
    $python = $null
    foreach ($candidate in @("py", "python")) {
      if (Get-Command $candidate -ErrorAction SilentlyContinue) {
        $version = ""
        try { $version = & $candidate --version 2>&1 | Out-String } catch { $version = "" }
        if ($version -match 'Python 3\.(\d+)' -and [int]$Matches[1] -ge 9) { $python = $candidate; break }
      }
    }
    if (-not $python) {
      Install-WithWinget "Python.Python.3.12" "Python 3.12" | Out-Null
      foreach ($candidate in @("py", "python")) {
        if (Get-Command $candidate -ErrorAction SilentlyContinue) { $python = $candidate; break }
      }
      if (-not $python) { Fail "Python did not install. Install Python 3 from https://python.org and run this again." }
    }
    $venv = Join-Path $env:LOCALAPPDATA "aqt-venv"
    $aqt = Join-Path $venv "Scripts\aqt.exe"
    if (-not (Test-Path $aqt)) {
      Say "   Setting up aqt (the Qt downloader) in $venv..."
      & $python -m venv $venv
      & (Join-Path $venv "Scripts\python.exe") -m pip install --quiet --upgrade pip aqtinstall
      if (-not (Test-Path $aqt)) { Fail "Could not install aqt." }
    }
    # A half-finished earlier download is cleared first; aqt would stop on it.
    if (Test-Path $qtDir) { Remove-Item $qtDir -Recurse -Force }
    Say "   Downloading Qt $QtVersion into $QtRoot (about 1 GB, a few minutes)..."
    # Not -m qtsvg or qtwayland: SVG is in the base kit on Windows, and asking for either stops aqt.
    & $aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -O $QtRoot
    if (-not (Test-Path $qtConfig)) { Fail "Qt did not install into $qtDir." }
  }
}
if (Test-Path $qtConfig) { Good $qtDir }

# --- 5. OpenCASCADE (optional) --------------------------------------------------------------------
$occtDir = Join-Path $VcpkgRoot "installed\x64-windows-v143"
$occtConfig = Join-Path $occtDir "share\opencascade\OpenCASCADEConfig.cmake"
if ($WithStep) {
  Step "OpenCASCADE (exact STEP)"
  if (-not (Test-Path $occtConfig)) {
    if ($CheckOnly) { $problems += "OpenCASCADE is missing"; Note "Missing (would build it with vcpkg in ${VcpkgRoot}: 30-60 minutes)" }
    else {
      if (-not (Test-Path (Join-Path $VcpkgRoot "vcpkg.exe"))) {
        if (-not (Test-Path $VcpkgRoot)) {
          Say "   Getting vcpkg into $VcpkgRoot..."
          & git clone --depth 1 https://github.com/microsoft/vcpkg.git $VcpkgRoot
          if ($LASTEXITCODE -ne 0) { Fail "Could not clone vcpkg into $VcpkgRoot." }
        }
        & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
        if (-not (Test-Path (Join-Path $VcpkgRoot "vcpkg.exe"))) { Fail "Could not set up vcpkg in $VcpkgRoot." }
      }
      # A triplet that uses the same compiler as DaedalCAD (v143), release only.
      $triplets = Join-Path $VcpkgRoot "..\vcpkg-triplets"
      $triplets = [System.IO.Path]::GetFullPath($triplets)
      New-Item -ItemType Directory -Force $triplets | Out-Null
      Set-Content -Path (Join-Path $triplets "x64-windows-v143.cmake") -Value @(
        'set(VCPKG_TARGET_ARCHITECTURE x64)', 'set(VCPKG_CRT_LINKAGE dynamic)', 'set(VCPKG_LIBRARY_LINKAGE dynamic)',
        'set(VCPKG_PLATFORM_TOOLSET v143)', 'set(VCPKG_BUILD_TYPE release)')
      Say "   Building OpenCASCADE with vcpkg: 30-60 minutes the first time. Leave this window open."
      & (Join-Path $VcpkgRoot "vcpkg.exe") install opencascade:x64-windows-v143 "--overlay-triplets=$triplets"
      if (-not (Test-Path $occtConfig)) { Fail "OpenCASCADE did not install. See docs/install.md, section 3e, to do it by hand." }
      Say "   You can delete the buildtrees, downloads and packages folders in $VcpkgRoot to get ~6 GB back."
    }
  }
}
$useOcct = Test-Path $occtConfig
if ($useOcct) { $env:OCCT_DIR = $occtDir; if ($WithStep) { Good $occtDir } }

if ($CheckOnly) {
  Write-Host ""
  if ($problems.Count -eq 0) { Write-Host "Everything is in place: run without -CheckOnly to build." -ForegroundColor Green }
  else { Write-Host ("Missing: " + ($problems -join "; ") + ". Run without -CheckOnly to install it.") -ForegroundColor Yellow }
  exit 0
}

# A small script for typing commands by hand later (docs/install.md uses it): Qt, CMake and
# OpenCASCADE on PATH in any PowerShell window after  . $env:USERPROFILE\qt-env.ps1
$envScript = Join-Path $env:USERPROFILE "qt-env.ps1"
$lines = @(
  "`$env:QTDIR = `"$qtDir`"",
  "`$env:Path = `"`$env:QTDIR\bin;$(Split-Path $cmake.Path);`" + `$env:Path",
  "`$env:CMAKE_PREFIX_PATH = `$env:QTDIR")
if ($useOcct) { $lines += "`$env:OCCT_DIR = `"$occtDir`"" }
Set-Content -Path $envScript -Value $lines
$env:QTDIR = $qtDir
$env:Path = "$qtDir\bin;" + $env:Path

# --- 6. Configure ---------------------------------------------------------------------------------
Step "Configure"
$cache = Join-Path $build "CMakeCache.txt"
if (Test-Path $cache) {
  $cachedGenerator = (Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' | Select-Object -First 1)
  if ($cachedGenerator -and $cachedGenerator.Matches[0].Groups[1].Value -ne $generator) {
    Note "The build folder was made for another Visual Studio; configuring again (the Manifold download is kept)."
    $Reconfigure = $true
  }
  $cachedOcct = Select-String -Path $cache -Pattern '^DAEDALCAD_WITH_OCCT:STRING=ON' -Quiet
  if ($useOcct -and -not $cachedOcct -and $WithStep) { $Reconfigure = $true }
}
if ($Reconfigure -and (Test-Path $cache)) {
  Remove-Item $cache -Force
  $cmakeFiles = Join-Path $build "CMakeFiles"
  if (Test-Path $cmakeFiles) { Remove-Item $cmakeFiles -Recurse -Force }
}
if (-not (Test-Path $cache)) {
  $arguments = @("-S", $root, "-B", $build, "-G", $generator, "-A", "x64") + $toolset + @("-DCMAKE_PREFIX_PATH=$qtDir")
  if ($WithStep) { $arguments += "-DDAEDALCAD_WITH_OCCT=ON" }
  & $cmake.Path @arguments
  if ($LASTEXITCODE -ne 0) { Fail "CMake could not configure the project (see above)." }
} else {
  Good "Already configured (the build configures again by itself when CMakeLists.txt changes)."
}

# --- 7. Build -------------------------------------------------------------------------------------
Step "Build (Release)"
& $cmake.Path --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { Fail "The build failed (see the first error above)." }
$exe = Join-Path $build "Release\DaedalCAD.exe"
if (-not (Test-Path $exe)) { Fail "The build finished but $exe is not there." }

# --- 8. Qt DLLs next to the exe -------------------------------------------------------------------
Step "Qt DLLs next to DaedalCAD.exe"
& (Join-Path $qtDir "bin\windeployqt.exe") --release --no-translations $exe | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "windeployqt failed." }
Good "Done"

# --- 9. Package and run ---------------------------------------------------------------------------
if ($Package) {
  Step "Package"
  & (Join-Path $root "package.ps1") -ReleaseDir (Split-Path $exe) -NoDeploy
  if ($LASTEXITCODE -ne 0) { Fail "package.ps1 failed." }
}

$minutes = [math]::Round(((Get-Date) - $started).TotalMinutes, 1)
Write-Host ""
Write-Host "Built $exe in $minutes min." -ForegroundColor Green
if ($useOcct) { Write-Host "With OpenCASCADE: exact STEP export and STEP import." -ForegroundColor Green }
else { Write-Host "Without OpenCASCADE (STEP export is faceted). Run with -WithStep to add it." }
if ($Run) { Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) }
