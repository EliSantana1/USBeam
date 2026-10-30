; SPDX-License-Identifier: GPL-3.0-or-later
; USBeam installer for Windows (x64 and ARM64), built with Inno Setup 6.
; Installs the USBeam service (starts with Windows) and the USBeam app
; (tray icon at sign-in, window on demand).

#define AppVersion "0.3.9"
; usbip-win2: the driver Windows needs to use devices from other PCs. Bundled and always installed
; (when missing or older), visibly, before USBeam itself. github.com/vadimgrn/usbip-win2,
; BSD-2-Clause, see THIRD-PARTY.txt.
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
; Windows 10 version 2004 or later (needed to repair or remove the driver's virtual controller).
MinVersion=10.0.19041
CloseApplications=force
; Uninstall log in the temporary folder ("Uninstall Log ..."), for finding problems.
UninstallLogging=yes

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
// ============================================================================ usbip-win2 driver
// Installing USBeam always installs the driver (USBeam both shares devices and uses them), with
// a message and its progress window shown. If the driver can't be installed, USBeam isn't either.
// Uninstalling asks whether to remove the driver too; if yes, it is removed completely: by its own
// uninstaller, then anything left over directly (and, for parts Windows keeps in use, once more
// at the next start of Windows).

const
  UsbipKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#UsbipAppId}_is1';
  UsbipHwid = 'ROOT\USBIP_WIN2\UDE';

var
  UsbipInstalledNow, UsbipRemoved: Boolean;

// "0.9.8.1" style versions: <0, 0, >0 like strcmp.
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

function UsbipInstalledVersion(): String;
begin
  if not RegQueryStringValue(HKLM64, UsbipKey, 'DisplayVersion', Result) then Result := '';
end;

function UsbipExe(): String;
begin
  Result := ExpandConstant('{commonpf64}\USBip\usbip.exe');
end;

function PowerShellExe(): String;
begin
  Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

// Runs a PowerShell script (written to a temporary file); returns its exit code, -1 if it didn't run.
function RunPS(const Name, Script: String): Integer;
var
  F: String;
begin
  F := AddBackslash(GetTempDir()) + Name;
  SaveStringToFile(F, Script, False);
  if not Exec(PowerShellExe(), '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + F + '"',
              '', SW_HIDE, ewWaitUntilTerminated, Result) then
    Result := -1;
  Log(Name + ' exit code ' + IntToStr(Result));
end;

// The driver's virtual USB controllers (present ones), as a PowerShell expression.
function PSControllers(): String;
begin
  Result := '@(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains ''' + UsbipHwid + ''' } | Sort-Object InstanceId)';
end;

// Exactly one virtual controller must exist (two make usbip-win2 refuse to work). Removes extra
// ones; returns how many there are afterwards.
function FixControllers(): Integer;
begin
  Result := RunPS('usbeam-controllers.ps1',
    '$d = ' + PSControllers() + #13#10 +
    'if ($d.Count -gt 1) { $d | Select-Object -Skip 1 | ForEach-Object { & pnputil.exe /remove-device $_.InstanceId | Out-Null }; Start-Sleep -Seconds 2 }' + #13#10 +
    'exit (' + PSControllers() + ').Count' + #13#10);
end;

// Installs (or updates) the driver. Returns '' when it is installed, else why not.
function InstallUsbip(): String;
var
  F, Args: String;
  Code: Integer;
begin
  Result := '';
  if IsArm64 then F := 'USBip-{#UsbipVersion}-arm64.exe' else F := 'USBip-{#UsbipVersion}-x64.exe';
  MsgBox('USBeam will now install the usbip-win2 driver ({#UsbipVersion}).' + #13#10 + #13#10 +
         'It lets this PC use devices from other PCs. Its progress window appears next; it takes about a minute.',
         mbInformation, MB_OK);
  ExtractTemporaryFile(F);
  Args := '/SILENT /SUPPRESSMSGBOXES /NORESTART /SP- /TYPE=compact /TASKS="vcredist" /LOG="' + ExpandConstant('{tmp}\usbip-install.log') + '"';
  if not Exec(ExpandConstant('{tmp}\') + F, Args, '', SW_SHOW, ewWaitUntilTerminated, Code) then
  begin
    Result := 'The usbip-win2 driver installer could not be started (' + SysErrorMessage(Code) + ').';
    Exit;
  end;
  Log('usbip-win2 installer exit code ' + IntToStr(Code));
  if (CompareVersions(UsbipInstalledVersion(), '{#UsbipVersion}') < 0) or not FileExists(UsbipExe()) then
  begin
    Result := 'The usbip-win2 driver could not be installed (code ' + IntToStr(Code) + ').';
    Exit;
  end;
  UsbipInstalledNow := True;
  RegWriteDWordValue(HKLM64, 'SOFTWARE\USBeam', 'InstalledUsbip', 1);
  // USBeam shows "Restart Windows to finish installing the USB driver" until Windows restarts.
  ForceDirectories(ExpandConstant('{commonappdata}\USBeam'));
  SaveStringToFile(ExpandConstant('{commonappdata}\USBeam\restart-needed'), 'usbip-win2 {#UsbipVersion}', False);
end;

// Makes sure the driver is installed, up to date and working. '' = fine, else why not.
function EnsureUsbip(): String;
var
  N: Integer;
begin
  Result := '';
  if (CompareVersions(UsbipInstalledVersion(), '{#UsbipVersion}') < 0) or not FileExists(UsbipExe()) then
  begin
    Result := InstallUsbip();
    if Result <> '' then Exit;
  end;
  N := FixControllers();
  if N = 0 then
  begin
    // Installed, but its controller is missing: install it again (it replaces itself).
    Result := InstallUsbip();
    if Result <> '' then Exit;
    N := FixControllers();
  end;
  if N <> 1 then
    Result := 'The usbip-win2 driver is installed but not working (' + IntToStr(N) + ' controllers).';
end;

// ---------------------------------------------------------------------------- installing
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
  Why: String;
begin
  // Stop the running service and app before replacing their files (updates).
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im USBeam.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  if Exec(ExpandConstant('{sys}\sc.exe'), 'stop USBeam', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0) then
    Sleep(5000);
  // The driver first: without it USBeam can't use devices from other PCs, so then nothing is installed.
  Why := EnsureUsbip();
  if Why <> '' then
  begin
    // An update that stops here: the USBeam already installed goes back to work as before.
    Exec(ExpandConstant('{sys}\sc.exe'), 'start USBeam', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Result := Why + #13#10 + #13#10 + 'USBeam was not installed. Please try again; if it keeps failing, restart Windows first.' + #13#10 +
              'Details: ' + ExpandConstant('{tmp}\usbip-install.log');
    if WizardSilent then MsgBox(Result, mbError, MB_OK);
  end
  else
    Result := '';
end;

// A freshly installed driver finishes working after Windows restarts.
function NeedRestart(): Boolean;
begin
  Result := UsbipInstalledNow;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  // Updates from inside the app run without the last page (and its restart question): say it here.
  if (CurStep = ssDone) and UsbipInstalledNow and WizardSilent then
    MsgBox('USBeam is updated and the usbip-win2 driver is installed.' + #13#10 + #13#10 +
           'Restart Windows to finish installing the driver. USBeam reminds you in its window until then.',
           mbInformation, MB_OK);
end;

// ---------------------------------------------------------------------------- uninstalling
// Everything the driver leaves on this PC, removed directly. Exit code = parts still present.
function DriverCleanupScript(): String;
begin
  Result :=
    '$ErrorActionPreference = ''SilentlyContinue''' + #13#10 +
    'Unregister-ScheduledTask -TaskName ''USBip Detach All On Reboot Or Shutdown'' -Confirm:$false' + #13#10 +
    'Get-PnpDevice | Where-Object { $_.HardwareID -contains ''' + UsbipHwid + ''' } | ForEach-Object { & pnputil.exe /remove-device $_.InstanceId | Out-Null }' + #13#10 +
    '$infs = { Get-ChildItem "$env:windir\INF\oem*.inf" | Where-Object { Select-String -Path $_.FullName -Pattern ''usbip2_(ude|filter)'' -Quiet } }' + #13#10 +
    '& $infs | ForEach-Object { & pnputil.exe /delete-driver $_.Name /uninstall /force | Out-Null }' + #13#10 +
    'Remove-Item -LiteralPath ''HKLM:\' + UsbipKey + ''' -Recurse -Force' + #13#10 +
    'Remove-Item -LiteralPath "$env:ProgramFiles\USBip" -Recurse -Force' + #13#10 +
    'Remove-Item -LiteralPath "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\USBip" -Recurse -Force' + #13#10 +
    '$left = @(Get-PnpDevice | Where-Object { $_.HardwareID -contains ''' + UsbipHwid + ''' }).Count + @(& $infs).Count' + #13#10 +
    'if (Test-Path -LiteralPath ''HKLM:\' + UsbipKey + ''') { $left++ }' + #13#10 +
    'if (Test-Path -LiteralPath "$env:ProgramFiles\USBip") { $left++ }' + #13#10;
end;

function UsbipPresent(): Boolean;
begin
  Result := RegKeyExists(HKLM64, UsbipKey) or FileExists(UsbipExe()) or DirExists(ExpandConstant('{commonpf64}\USBip'));
end;

// Parts Windows keeps in use until it restarts: removed by a one-time task at the next start,
// which then deletes itself.
procedure ScheduleCleanupAfterRestart();
var
  F: String;
  Code: Integer;
begin
  F := ExpandConstant('{commonappdata}\USBeam-driver-cleanup.ps1');
  SaveStringToFile(F,
    'try {' + #13#10 + DriverCleanupScript() + '} finally {' + #13#10 +
    '  Unregister-ScheduledTask -TaskName ''USBeam driver cleanup'' -Confirm:$false' + #13#10 +
    '  Remove-Item -LiteralPath $PSCommandPath -Force' + #13#10 + '}' + #13#10, False);
  Exec(ExpandConstant('{sys}\schtasks.exe'),
       '/Create /F /TN "USBeam driver cleanup" /SC ONSTART /RU SYSTEM /RL HIGHEST /TR "' +
       PowerShellExe() + ' -NoProfile -ExecutionPolicy Bypass -File \"' + F + '\""',
       '', SW_HIDE, ewWaitUntilTerminated, Code);
  Log('Scheduled the driver cleanup for the next start of Windows (schtasks exit ' + IntToStr(Code) + ')');
end;

procedure RemoveUsbip();
var
  Unins: String;
  Code, Left: Integer;
begin
  // usbip-win2's own uninstaller first, with its progress window shown.
  if RegQueryStringValue(HKLM64, UsbipKey, 'UninstallString', Unins) then
  begin
    Unins := RemoveQuotes(Unins);
    if FileExists(Unins) then
    begin
      Log('Removing usbip-win2: ' + Unins);
      Exec(Unins, '/SILENT /SUPPRESSMSGBOXES /NORESTART /LOG="' + ExpandConstant('{%TEMP}\USBeam-usbip-uninstall.log') + '"',
           '', SW_SHOW, ewWaitUntilTerminated, Code);
      Log('usbip-win2 uninstaller exit code ' + IntToStr(Code));
    end;
  end;
  // Then anything it left behind, directly (twice: a part may only let go after another is gone).
  Left := RunPS('usbeam-driver-cleanup.ps1', DriverCleanupScript() + 'exit $left' + #13#10);
  if Left <> 0 then Left := RunPS('usbeam-driver-cleanup.ps1', DriverCleanupScript() + 'exit $left' + #13#10);
  if Left <> 0 then ScheduleCleanupAfterRestart();     // what Windows still holds goes at the next start
  UsbipRemoved := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    // Stop USBeam first, so no device is connected through the driver while it's removed.
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im USBeam.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
    if Exec(ExpandConstant('{sys}\sc.exe'), 'stop USBeam', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0) then
      Sleep(5000);
    if UsbipPresent() and
       (MsgBox('Also remove the usbip-win2 driver?' + #13#10 + #13#10 +
               'It lets this PC use devices from other PCs. Choose No if another program uses it.',
               mbConfirmation, MB_YESNO) = IDYES) then
      RemoveUsbip();
  end
  else if CurUninstallStep = usPostUninstall then
    RegDeleteKeyIncludingSubkeys(HKLM64, 'SOFTWARE\USBeam');
end;

// Removing the driver finishes when Windows restarts.
function UninstallNeedRestart(): Boolean;
begin
  Result := UsbipRemoved;
end;
