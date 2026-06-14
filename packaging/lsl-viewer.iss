; Inno Setup script for the LSL Viewer Windows installer.
; Built in CI (the `windows` build leg) with: ISCC.exe packaging\lsl-viewer.iss
; Installs the self-contained (static) binaries — no portable.txt, so the installed app uses
; the standard per-user locations (%APPDATA% for config, Documents for recordings).

#define MyAppName "LSL Viewer"
#define MyAppExe  "lsl_viewer.exe"
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif

[Setup]
AppId={{C7A3E1F2-9D4B-4A6E-B8C1-2F3D4E5A6B7C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Alex Forrence
DefaultDirName={autopf}\LSL Viewer
DefaultGroupName=LSL Viewer
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
UninstallDisplayIcon={app}\{#MyAppExe}
OutputDir=..
OutputBaseFilename=lsl-viewer-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

[Files]
Source: "..\build\Release\lsl_viewer.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\xdf_record.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";                       DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_LICENSES";          DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\LSL Viewer";           Filename: "{app}\{#MyAppExe}"
Name: "{group}\Uninstall LSL Viewer"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "Launch LSL Viewer"; Flags: nowait postinstall skipifsilent
