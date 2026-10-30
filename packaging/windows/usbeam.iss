; SPDX-License-Identifier: GPL-3.0-or-later
; USBeam installer for Windows (x64 and ARM64), built with Inno Setup 6.
; Installs the USBeam service (starts with Windows) and the USBeam app
; (tray icon at sign-in, window on demand).

#define AppVersion "0.3.8"
; usbip-win2: the driver Windows needs to use devices from other PCs. Bundled, installed when
; missing or older (github.com/vadimgrn/usbip-win2, BSD-2-Clause, see THIRD-PARTY.txt).
#define UsbipVersion "0.9.8.1"
#define UsbipAppId "{199505b0-b93d-4521-a8c7-897818e0205a}"

[Setup]
AppId={{6D2C1B54-7E7A-4F1B-9B5D-3E1F0A9C5B21}
AppName=USBeam
AppVersion={#AppVersion}
AppPublisher=USBeam contributors
AppPublisherURL=https://github.com/EliSantana1/USBeam
DefaultDirName={autopf}\USBeam
DefaultGroupName=USBeam
DisableProgramGroupPage=yes
; Always let people choose where to install (another drive, for example).
DisableDirPage=no
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
Source: "THIRD-PARTY.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\build\usbip\USBip-{#UsbipVersion}-arm64.exe"; Flags: dontcopy nocompression
Source: "..\..\build\usbip\USBip-{#UsbipVersion}-x64.exe"; Flags: dontcopy nocompression

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
; "Restart USBeam" in the app ends the service with an error code: Windows then starts it again.
Filename: "{sys}\sc.exe"; Parameters: "failureflag USBeam 1"; Flags: runhidden waituntilterminated
; Signed-in users may start/stop this one service (the app's Quit and restart), nothing else.
Filename: "{sys}\sc.exe"; Parameters: "sdset USBeam D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWRPWPLOCRRC;;;IU)"; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "start USBeam"; Flags: runhidden waituntilterminated
Filename: "{app}\USBeam.exe"; Description: "Open USBeam now"; Flags: postinstall nowait skipifsilent runasoriginaluser
; Updates from inside the app run this installer silently: reopen the app afterwards.
Filename: "{app}\USBeam.exe"; Flags: nowait runasoriginaluser; Check: WizardSilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/f /im USBeam.exe"; Flags: runhidden waituntilterminated; RunOnceId: "KillApp"
Filename: "{sys}\sc.exe"; Parameters: "stop USBeam"; Flags: runhidden waituntilterminated; RunOnceId: "StopSvc"
Filename: "{sys}\timeout.exe"; Parameters: "/t 5 /nobreak"; Flags: runhidden waituntilterminated; RunOnceId: "WaitSvc"
Filename: "{sys}\sc.exe"; Parameters: "delete USBeam"; Flags: runhidden waituntilterminated; RunOnceId: "DeleteSvc"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""USBeam"""; Flags: runhidden waituntilterminated; RunOnceId: "DelFw"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""USBeam (ZeroTier/Tailscale)"""; Flags: runhidden waituntilterminated; RunOnceId: "DelFwVpn"

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\USBeam"
; The app window's own data for the person uninstalling.
Type: filesandordirs; Name: "{localappdata}\USBeam"

[Code]
var
  UsbipInstalledNow: Boolean;

// "0.9.8.1" -> comparable parts; returns <0, 0, >0 like strcmp.
function CompareVersions(A, B: String): Integer;
var
  PA, PB, NA, NB: Integer;
begin
  Result := 0;
  while (Result = 0) and ((A <> '') or (B <> '')) do
  begin
    PA := Pos('.', A); if PA = 0 then PA := Length(A) + 1;
    PB := Pos('.', B); if PB = 0 then PB := Length(B) + 1;
    NA := StrToIntDef(Copy(A, 1, PA - 1), 0);
    NB := StrToIntDef(Copy(B, 1, PB - 1), 0);
    Delete(A, 1, PA); Delete(B, 1, PB);
    if NA < NB then Result := -1 else if NA > NB then Result := 1;
  end;
end;

// usbip-win2 is missing or older than the bundled version. One installed some other way
// (no version known) is left alone.
function UsbipNeeded(): Boolean;
var
  Ver: String;
begin
  if RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#UsbipAppId}_is1', 'DisplayVersion', Ver) then
    Result := CompareVersions(Ver, '{#UsbipVersion}') < 0
  else
    Result := not FileExists(ExpandConstant('{commonpf64}\USBip\usbip.exe'));
end;

procedure InstallUsbip();
var
  F: String;
  Code: Integer;
begin
  if IsArm64 then F := 'USBip-{#UsbipVersion}-arm64.exe' else F := 'USBip-{#UsbipVersion}-x64.exe';
  WizardForm.StatusLabel.Caption := 'Installing the usbip-win2 driver (lets this PC use devices from other PCs)...';
  ExtractTemporaryFile(F);
  // Its own installer, silently: main program + client driver + Visual C++ runtime only.
  Exec(ExpandConstant('{tmp}\') + F, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /TYPE=compact /TASKS="vcredist"',
       '', SW_HIDE, ewWaitUntilTerminated, Code);
  Log('usbip-win2 installer exit code ' + IntToStr(Code));
  if FileExists(ExpandConstant('{commonpf64}\USBip\usbip.exe')) then
  begin
    UsbipInstalledNow := True;
    // Remember that USBeam installed it, so uninstalling USBeam removes it again.
    RegWriteDWordValue(HKLM64, 'SOFTWARE\USBeam', 'InstalledUsbip', 1);
  end
  else
    SuppressibleMsgBox('The usbip-win2 driver could not be installed (code ' + IntToStr(Code) + ').' + #13#13 +
      'USBeam can still share this PC''s devices. To use devices from other PCs here, install usbip-win2 from ' +
      'github.com/vadimgrn/usbip-win2.', mbError, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and UsbipNeeded() then InstallUsbip();
end;

// A freshly installed driver works after Windows restarts.
function NeedRestart(): Boolean;
begin
  Result := UsbipInstalledNow;
end;

// Uninstalling: remove usbip-win2 too, but only if USBeam installed it.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Flag: Cardinal;
  Unins: String;
  Code: Integer;
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  if RegQueryDWordValue(HKLM64, 'SOFTWARE\USBeam', 'InstalledUsbip', Flag) and (Flag = 1) then
  begin
    if RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#UsbipAppId}_is1', 'UninstallString', Unins) then
    begin
      Unins := RemoveQuotes(Unins);
      if FileExists(Unins) then
        Exec(Unins, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, Code);
    end;
  end;
  RegDeleteKeyIncludingSubkeys(HKLM64, 'SOFTWARE\USBeam');
end;

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
