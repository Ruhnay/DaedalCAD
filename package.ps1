# DaedalCAD: zip the Release build so other people can run it.
#
#   .\package.ps1                         finds build\...\Release\DaedalCAD.exe by itself
#   .\package.ps1 -ReleaseDir C:\x\Release use that folder
#   .\package.ps1 -IncludeLibraries       also ship your parts and 2d_shapes libraries
#   .\package.ps1 -NoDeploy               do not run windeployqt (the folder already has the Qt DLLs)
#   .\package.ps1 -OutDir D:\share        put the zip (and installer) in that folder instead
#   .\package.ps1 -NoInstaller            only the zip
#
# Result, next to this script (the version is the one in CMakeLists.txt):
#   DaedalCAD-<version>-windows-x64.zip  portable: unzip anywhere, one DaedalCAD-<version> folder inside
#   DaedalCAD-<version>-setup.exe        installer, when Inno Setup 6 is installed (winget install JRSoftware.InnoSetup)
# Files of the same version are replaced.
# Your own projects, imported files and placed 2D shapes are never included.
# Works in Windows PowerShell 5.1 and PowerShell 7. If scripts are blocked, run:
#   powershell -ExecutionPolicy Bypass -File .\package.ps1

param(
  [string]$ReleaseDir = "",
  [string]$OutDir = "",
  [switch]$IncludeLibraries,
  [switch]$NoDeploy,
  [switch]$NoInstaller,
  [string]$Publisher = "DaedalCAD"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutDir) { $OutDir = $root }
# The package is put together in a temporary folder, so nothing but the zip is left behind.
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("DaedalCAD-package-" + [guid]::NewGuid().ToString("N").Substring(0, 8))

function Fail([string]$message) {
  if (Test-Path $work) { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }
  Write-Host ""
  Write-Host $message -ForegroundColor Red
  exit 1
}

# --- 1. The Release folder ----------------------------------------------------------------------
if ($ReleaseDir) {
  if (-not (Test-Path (Join-Path $ReleaseDir "DaedalCAD.exe"))) { Fail "No DaedalCAD.exe in $ReleaseDir" }
  $ReleaseDir = (Resolve-Path $ReleaseDir).Path
} else {
  $found = Get-ChildItem -Path $root -Filter "DaedalCAD.exe" -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Directory.Name -like "*Release*" } |
    Sort-Object LastWriteTime -Descending
  if (-not $found) { Fail "Could not find a Release\DaedalCAD.exe under $root. Build Release first, or pass -ReleaseDir." }
  $ReleaseDir = $found[0].DirectoryName
  if (@($found).Count -gt 1) { Write-Host "Several Release builds found; using the newest one." -ForegroundColor Yellow }
}
Write-Host "Release folder: $ReleaseDir"

# --- 2. Version from CMakeLists.txt -------------------------------------------------------------
$version = "dev"
$cmake = Join-Path $root "CMakeLists.txt"
if (Test-Path $cmake) {
  $match = Select-String -Path $cmake -Pattern 'project\(DaedalCAD\s+VERSION\s+([0-9.]+)' | Select-Object -First 1
  if ($match) { $version = $match.Matches[0].Groups[1].Value }
}
$name = "DaedalCAD-$version"
$zipName = "$name-windows-x64.zip"
Write-Host "Version:        $version"

# --- 3. Copy into a clean staging folder --------------------------------------------------------
$stage = Join-Path $work $name
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -Path (Join-Path $ReleaseDir "*") -Destination $stage -Recurse -Force
# The licenses travel with every copy (GPL for DaedalCAD, LGPL for the Qt DLLs, and the rest).
foreach ($file in @("LICENSE", "THIRD_PARTY_LICENSES.md")) {
  $source = Join-Path $root $file
  if (Test-Path $source) { Copy-Item $source (Join-Path $stage $file) -Force }
  else { Write-Host "Missing:        $file" -ForegroundColor Yellow }
}

# Build leftovers and test programs nobody needs.
$junkFiles = @("*.pdb", "*.ilk", "*.exp", "*.lib", "*.iobj", "*.ipdb", "*_test.exe", "*.log")
foreach ($pattern in $junkFiles) {
  Get-ChildItem -Path $stage -Filter $pattern -Recurse -File -ErrorAction SilentlyContinue | Remove-Item -Force
}
foreach ($dir in @("CMakeFiles", "DaedalCAD.dir", "x64")) {
  $path = Join-Path $stage $dir
  if (Test-Path $path) { Remove-Item $path -Recurse -Force }
}

# Your own data stays at home.
$appdata = Join-Path $stage "appdata"
$private = @("projects", "Imported", "placed_2d")
if (-not $IncludeLibraries) { $private += @("parts", "2d_shapes") }
foreach ($dir in $private) {
  $path = Join-Path $appdata $dir
  if (Test-Path $path) {
    Remove-Item $path -Recurse -Force
    Write-Host "Left out:       appdata\$dir"
  }
}

# --- 4. Qt DLLs ---------------------------------------------------------------------------------
$exe = Join-Path $stage "DaedalCAD.exe"
if (-not $NoDeploy) {
  $deploy = $null
  $candidates = @()
  if ($env:QTDIR) { $candidates += (Join-Path $env:QTDIR "bin\windeployqt.exe") }
  if ($env:Qt6_DIR) { $candidates += (Join-Path $env:Qt6_DIR "..\..\..\bin\windeployqt.exe") }
  $onPath = Get-Command "windeployqt.exe" -ErrorAction SilentlyContinue
  if ($onPath) { $candidates += $onPath.Source }
  $candidates += @(Get-ChildItem -Path "C:\Qt\*\msvc*_64\bin\windeployqt.exe", "$env:USERPROFILE\Qt\*\msvc*_64\bin\windeployqt.exe" -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | ForEach-Object { $_.FullName })
  foreach ($candidate in $candidates) {
    if ($candidate -and (Test-Path $candidate)) { $deploy = (Resolve-Path $candidate).Path; break }
  }
  if ($deploy) {
    Write-Host "windeployqt:    $deploy"
    & $deploy --release --no-translations $exe | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "windeployqt failed (exit code $LASTEXITCODE)." }
  } else {
    Write-Host "windeployqt not found (set QTDIR, e.g. C:\Qt\6.8.3\msvc2022_64). Using the DLLs already in the Release folder." -ForegroundColor Yellow
  }
}

# Visual C++ runtime next to the exe, so people do not need to install the redistributable.
$system = if ($env:SystemRoot) { Join-Path $env:SystemRoot "System32" } else { "" }
if ($system) { foreach ($dll in @("msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll")) {
  $source = Join-Path $system $dll
  $target = Join-Path $stage $dll
  if ((Test-Path $source) -and -not (Test-Path $target)) { Copy-Item $source $target }
} }

# --- 5. Sanity check ----------------------------------------------------------------------------
$missing = @()
foreach ($required in @("DaedalCAD.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6OpenGL.dll",
                        "Qt6OpenGLWidgets.dll", "Qt6Network.dll", "platforms\qwindows.dll", "vcruntime140.dll", "msvcp140.dll")) {
  if (-not (Test-Path (Join-Path $stage $required))) { $missing += $required }
}
if ($missing.Count -gt 0) {
  Fail ("The package would not start on another PC. Missing: " + ($missing -join ", ") +
        "`nRun windeployqt (set QTDIR) or build with the deploy step, then try again.")
}
if (-not (Test-Path (Join-Path $stage "tls"))) {
  Write-Host "Note: no tls\ plugin folder, so https:// CloudSync hosts will not work on other PCs (http:// does)." -ForegroundColor Yellow
}

# --- 6. Zip -------------------------------------------------------------------------------------
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$zip = Join-Path (Resolve-Path $OutDir).Path $zipName
if (Test-Path $zip) {
  try { Remove-Item $zip -Force } catch { Fail "Could not replace $zip. Is it open somewhere?" }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)

$files = (Get-ChildItem -Path $stage -Recurse -File).Count
$size = "{0:N1} MB" -f ((Get-Item $zip).Length / 1MB)

# --- 7. Installer (Inno Setup) ------------------------------------------------------------------
$setup = $null
if (-not $NoInstaller) {
  $iss = Join-Path $root "installer\DaedalCAD.iss"
  $iscc = $null
  $candidates = @()
  if ($env:LOCALAPPDATA) { $candidates += (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe") }
  if (${env:ProgramFiles(x86)}) { $candidates += (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe") }
  if ($env:ProgramFiles) { $candidates += (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe") }
  $onPath = Get-Command "ISCC.exe" -ErrorAction SilentlyContinue
  if ($onPath) { $candidates += $onPath.Source }
  foreach ($candidate in $candidates) { if ($candidate -and (Test-Path $candidate)) { $iscc = $candidate; break } }
  if (-not (Test-Path $iss)) {
    Write-Host "No installer\DaedalCAD.iss next to this script; only the zip was made." -ForegroundColor Yellow
  } elseif (-not $iscc) {
    Write-Host "Inno Setup 6 not found, so only the zip was made. Install it with: winget install JRSoftware.InnoSetup" -ForegroundColor Yellow
  } else {
    Write-Host "Inno Setup:     $iscc"
    $outFull = (Resolve-Path $OutDir).Path
    $setup = Join-Path $outFull "$name-setup.exe"
    if (Test-Path $setup) {
      try { Remove-Item $setup -Force } catch { Fail "Could not replace $setup. Is it open somewhere?" }
    }
    & $iscc /Q "/DAppVersion=$version" "/DSourceDir=$stage" "/DOutputDir=$outFull" "/DPublisher=$Publisher" $iss
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $setup)) { Fail "Inno Setup could not build the installer (exit code $LASTEXITCODE)." }
  }
}

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ""
Write-Host "Done: $zip ($files files, $size)" -ForegroundColor Green
if ($setup) { Write-Host ("      $setup ({0:N1} MB)" -f ((Get-Item $setup).Length / 1MB)) -ForegroundColor Green }
Write-Host "Version $version. Upload both on the admin page; people unzip the zip anywhere, or run the setup."
