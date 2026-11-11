; DaedalCAD installer (Inno Setup 6). package.ps1 builds it; by hand:
;   ISCC.exe /DAppVersion=0.9.7 /DSourceDir=C:\path\DaedalCAD-0.9.7 /DOutputDir=C:\out DaedalCAD.iss
;
; Installs for the current user only, in %LOCALAPPDATA%\Programs\DaedalCAD: no admin prompt, now or
; on updates. Your projects and libraries live in Documents\DaedalCAD and are never touched by
; installing, updating or uninstalling.
;
; Updates: DaedalCAD downloads the next setup.exe and runs it with /VERYSILENT; it closes, the
; files are replaced, and DaedalCAD starts again (the second [Run] entry).
;
; DaedalCAD used to be called ShapeSynth. It keeps ShapeSynth's AppId, so installing it replaces
; ShapeSynth instead of sitting next to it: the old program folder (Programs\ShapeSynth) and its
; shortcuts are removed, and DaedalCAD moves Documents\ShapeSynth to Documents\DaedalCAD itself on
; its first start.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #error Pass the packaged folder: /DSourceDir=...\DaedalCAD-<version>
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef Publisher
  #define Publisher "DaedalCAD"
#endif

[Setup]
; Never change AppId: it is how Windows (and updates) recognise DaedalCAD as the same program,
; including copies installed under its old name, ShapeSynth.
AppId={{A5F990AE-EB53-4C31-AAE6-3F794B7779E9}
AppName=DaedalCAD
AppVersion={#AppVersion}
AppVerName=DaedalCAD {#AppVersion}
AppPublisher={#Publisher}
VersionInfoVersion={#AppVersion}
VersionInfoProductName=DaedalCAD
PrivilegesRequired=lowest
; The previous folder, unless that is the old Programs\ShapeSynth (see InstallDir below).
DefaultDirName={code:InstallDir}
UsePreviousAppDir=no
DisableDirPage=auto
DisableProgramGroupPage=yes
DisableWelcomePage=no
OutputDir={#OutputDir}
OutputBaseFilename=DaedalCAD-{#AppVersion}-setup
#if FileExists(AddBackslash(SourceDir) + "appdata\icons\application.ico")
SetupIconFile={#SourceDir}\appdata\icons\application.ico
#endif
UninstallDisplayIcon={app}\DaedalCAD.exe
UninstallDisplayName=DaedalCAD
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; A running DaedalCAD is closed before its files are replaced (updates close it themselves first).
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; Shortcuts from when it was called ShapeSynth.
Type: files; Name: "{autoprograms}\ShapeSynth.lnk"
Type: files; Name: "{autodesktop}\ShapeSynth.lnk"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[INI]
; Tells DaedalCAD it is installed, so it keeps your data in Documents\DaedalCAD.
Filename: "{app}\installed.ini"; Section: "DaedalCAD"; Key: "installed"; String: "{#AppVersion}"

[Icons]
Name: "{autoprograms}\DaedalCAD"; Filename: "{app}\DaedalCAD.exe"
Name: "{autodesktop}\DaedalCAD"; Filename: "{app}\DaedalCAD.exe"; Tasks: desktopicon

[Run]
; Normal install: the "Launch DaedalCAD" tick box on the last page.
Filename: "{app}\DaedalCAD.exe"; Description: "{cm:LaunchProgram,DaedalCAD}"; Flags: nowait postinstall skipifsilent
; Silent update from inside DaedalCAD: start it again afterwards.
Filename: "{app}\DaedalCAD.exe"; Flags: nowait skipifnotsilent

[UninstallDelete]
Type: files; Name: "{app}\installed.ini"

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A5F990AE-EB53-4C31-AAE6-3F794B7779E9}_is1';

var
  PreviousDir: String;

// A folder that holds an installed ShapeSynth (the old name), and nothing else of ours.
function IsOldShapeSynthDir(Dir: String): Boolean;
begin
  Result := (Dir <> '') and
            (CompareText(ExtractFileName(RemoveBackslashUnlessRoot(Dir)), 'ShapeSynth') = 0) and
            FileExists(AddBackslash(Dir) + 'ShapeSynth.exe') and
            FileExists(AddBackslash(Dir) + 'installed.ini');
end;

function InitializeSetup(): Boolean;
begin
  // Read before this install overwrites it.
  if not RegQueryStringValue(HKA, UninstallKey, 'Inno Setup: App Path', PreviousDir) then
    PreviousDir := '';
  Result := True;
end;

function InstallDir(Param: String): String;
begin
  if (PreviousDir = '') or IsOldShapeSynthDir(PreviousDir) then
    Result := ExpandConstant('{autopf}\DaedalCAD')
  else
    Result := PreviousDir;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Tries: Integer;
begin
  if CurStep <> ssPostInstall then Exit;
  if not IsOldShapeSynthDir(PreviousDir) then Exit;
  if CompareText(RemoveBackslashUnlessRoot(PreviousDir), RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) = 0 then Exit;
  // ShapeSynth may still be closing after it started this update: try for a few seconds.
  for Tries := 1 to 10 do
  begin
    if DelTree(PreviousDir, True, True, True) then Exit;
    Sleep(500);
  end;
end;
