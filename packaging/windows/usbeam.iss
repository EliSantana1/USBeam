; SPDX-License-Identifier: GPL-3.0-or-later
; USBeam installer for Windows (x64 and ARM64), built with Inno Setup 6.
; Installs the USBeam service (starts with Windows) and the USBeam app
; (tray icon at sign-in, window on demand).

#define AppVersion "0.2.7"

[Setup]
AppId={{6D2C1B54-7E7A-4F1B-9B5D-3E1F0A9C5B21}
AppName=USBeam
AppVersion={#AppVersion}
AppPublisher=USBeam contributors
AppPublisherURL=https://github.com/EliSantana1/USBeam
DefaultDirName={autopf}\USBeam
DefaultGroupName=USBeam
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir=..\..\build
OutputBaseFilename=USBeam-Setup-{#AppVersion}
SetupIconFile=..\..\assets\usbeam.ico
UninstallDisplayIcon={app}\USBeam.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible arm64
ArchitecturesInstallIn64BitMode=x64compatible arm64
MinVersion=10.0.17763
CloseApplications=force

[Files]
Source: "..\..\build\usbeam-service-arm64.exe"; DestDir: "{app}"; DestName: "usbeam-service.exe"; Check: IsArm64; Flags: ignoreversion
Source: "..\..\build\usbeam-app-arm64.exe"; DestDir: "{app}"; DestName: "USBeam.exe"; Check: IsArm64; Flags: ignoreversion
Source: "..\..\build\webview2\arm64\WebView2Loader.dll"; DestDir: "{app}"; Check: IsArm64; Flags: ignoreversion
Source: "..\..\build\usbeam-service-x64.exe"; DestDir: "{app}"; DestName: "usbeam-service.exe"; Check: not IsArm64; Flags: ignoreversion
Source: "..\..\build\usbeam-app-x64.exe"; DestDir: "{app}"; DestName: "USBeam.exe"; Check: not IsArm64; Flags: ignoreversion
Source: "..\..\build\webview2\x64\WebView2Loader.dll"; DestDir: "{app}"; Check: not IsArm64; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\USBeam"; Filename: "{app}\USBeam.exe"

[Registry]
; The app's tray icon starts at sign-in, hidden.
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "USBeam"; ValueData: """{app}\USBeam.exe"" --tray"; Flags: uninsdeletevalue

[Run]
Filename: "{sys}\sc.exe"; Parameters: "create USBeam binPath= ""{app}\usbeam-service.exe"" start= auto DisplayName= ""USBeam"""; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "config USBeam binPath= ""{app}\usbeam-service.exe"" start= auto"; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "description USBeam ""Shares and connects USB devices between your computers."""; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "failure USBeam reset= 86400 actions= restart/3000/restart/3000/restart/10000"; Flags: runhidden waituntilterminated
; Signed-in users may start/stop this one service (the app's Quit and restart), nothing else.
Filename: "{sys}\sc.exe"; Parameters: "sdset USBeam D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWRPWPLOCRRC;;;IU)"; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "start USBeam"; Flags: runhidden waituntilterminated
Filename: "{app}\USBeam.exe"; Description: "Open USBeam now"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/f /im USBeam.exe"; Flags: runhidden waituntilterminated; RunOnceId: "KillApp"
Filename: "{sys}\sc.exe"; Parameters: "stop USBeam"; Flags: runhidden waituntilterminated; RunOnceId: "StopSvc"
Filename: "{sys}\timeout.exe"; Parameters: "/t 5 /nobreak"; Flags: runhidden waituntilterminated; RunOnceId: "WaitSvc"
Filename: "{sys}\sc.exe"; Parameters: "delete USBeam"; Flags: runhidden waituntilterminated; RunOnceId: "DeleteSvc"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""USBeam"""; Flags: runhidden waituntilterminated; RunOnceId: "DelFw"

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\USBeam"

[Code]
// Stop the running service and app before replacing their files (upgrades).
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im USBeam.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  if Exec(ExpandConstant('{sys}\sc.exe'), 'stop USBeam', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0) then
    Sleep(5000);
  Result := '';
end;
