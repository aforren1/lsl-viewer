; Inno Setup script for the LSL Viewer Windows installer.
; Built in CI (the `windows` build leg) with: ISCC.exe packaging\lsl-viewer.iss
; Installs the self-contained (static) binaries — no portable.txt, so the installed app uses
; the standard per-user locations (%APPDATA% for config, Documents for recordings).

#define MyAppName "LSL Viewer"
#define MyAppExe  "lsl_viewer.exe"
#define MyAppRecExe "xdf_record.exe"
; Firewall rule names. Keep these stable across versions: the uninstaller and the next
; installer both find the rules by name, and a renamed rule leaks on upgrade.
#define FwRuleViewer "LSL Viewer"
#define FwRuleRecord "LSL Viewer (xdf_record)"
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
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
SetupIconFile=lsl-viewer.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

[Tasks]
; LSL needs inbound UDP 16571 (resolve queries) and inbound TCP (data, and the remote-control
; port) to work across machines. Without a rule, the first launch raises the Windows Defender
; prompt, and a user who cancels it (or who cannot elevate) gets a persistent BLOCK rule that
; silently breaks stream resolution afterwards. Adding the rule here uses the elevation the
; installer already has. Deselect with /TASKS="" for an unattended install. See docs/network.md.
Name: firewall;       Description: "Allow LSL network discovery (private and domain networks)"; GroupDescription: "Windows Firewall:"
Name: firewall\public; Description: "Also allow on public networks (an isolated lab network is often classified public)"; Flags: unchecked

[Files]
Source: "..\build\Release\lsl_viewer.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\xdf_record.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";                       DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_LICENSES";          DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\LSL Viewer";           Filename: "{app}\{#MyAppExe}"
Name: "{group}\Uninstall LSL Viewer"; Filename: "{uninstallexe}"

[Run]
; Delete before add, because `netsh ... add rule` appends a duplicate rather than replacing,
; and an upgrade over an existing install would otherwise accumulate one rule per version.
; A delete that matches nothing exits nonzero; Inno does not check [Run] exit codes, so that
; is harmless. The rules name the program and set no `protocol=`, thus they cover TCP and UDP
; on every port: liblsl uses a port range, and the control listener can fall back to an
; ephemeral port, so a port-scoped rule would be wrong.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#FwRuleViewer}"""; Flags: runhidden; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#FwRuleRecord}"""; Flags: runhidden; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""{#FwRuleViewer}"" dir=in action=allow enable=yes profile={code:FwProfiles} program=""{app}\{#MyAppExe}"""; Flags: runhidden; Tasks: firewall; StatusMsg: "Adding Windows Firewall rules..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""{#FwRuleRecord}"" dir=in action=allow enable=yes profile={code:FwProfiles} program=""{app}\{#MyAppRecExe}"""; Flags: runhidden; Tasks: firewall; StatusMsg: "Adding Windows Firewall rules..."
Filename: "{app}\{#MyAppExe}"; Description: "Launch LSL Viewer"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#FwRuleViewer}"""; Flags: runhidden; RunOnceId: "DelFwViewer"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#FwRuleRecord}"""; Flags: runhidden; RunOnceId: "DelFwRecord"

[Code]
function FwProfiles(Param: String): String;
begin
  if WizardIsTaskSelected('firewall\public') then
    Result := 'private,domain,public'
  else
    Result := 'private,domain';
end;
