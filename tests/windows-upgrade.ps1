# SPDX-License-Identifier: GPL-3.0-or-later
# Upgrade / second-launch / uninstall test for the Windows installer, run by GitHub Actions
# (as an administrator, no one at the screen).
#
#   tests\windows-upgrade.ps1 <previous release setup.exe> <new setup.exe> <new version>
#
# Installs the previous release, updates it the way the app's Update button does, starts the app
# again a few times, then uninstalls. After each step it checks for leftovers: installers still
# running, extra copies of the app or service, files and settings left behind.
param([string]$Old, [string]$New, [string]$NewVersion, [string]$Usbip)
$ErrorActionPreference = 'Continue'
$script:Fail = $false
$AppKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{6D2C1B54-7E7A-4F1B-9B5D-3E1F0A9C5B21}_is1'
$Logs = Join-Path $env:RUNNER_TEMP 'usbeam-test-logs'
New-Item -ItemType Directory -Force $Logs | Out-Null

function Say($m) { Write-Host ''; Write-Host "== $m" }
function Fail($m) { Write-Host "FAIL: $m"; $script:Fail = $true }
function Procs($name) { @(Get-Process -Name $name -ErrorAction SilentlyContinue) }
function Show {
    Get-Process | Where-Object { $_.Name -match '^(USBeam|usbeam-service|USBeam-Setup.*|_iu.*|unins.*|USBip.*)$' } |
        Format-Table Id, Name, StartTime, Path -AutoSize | Out-String | Write-Host
}
function WaitFor([string]$What, [int]$Seconds, [scriptblock]$Cond) {
    for ($i = 0; $i -lt $Seconds; $i++) { if (& $Cond) { return $true }; Start-Sleep 1 }
    Fail "$What (waited $Seconds s)"; return $false
}
function Installed { (Get-ItemProperty -Path $AppKey -ErrorAction SilentlyContinue).DisplayVersion }
function AppDir { (Get-ItemProperty -Path $AppKey -ErrorAction SilentlyContinue).InstallLocation }
function RunSetup([string]$Exe, [string]$ArgList, [string]$LogName) {
    # Wait for the installer only (Start-Process -Wait would also wait for the app it opens).
    $p = Start-Process -FilePath $Exe -ArgumentList "$ArgList /LOG=`"$Logs\$LogName`"" -PassThru
    $null = $p.Handle                 # keeps the exit code readable after it ends
    if (-not $p.WaitForExit(900000)) { Fail "$(Split-Path $Exe -Leaf) still running after 15 minutes"; return }
    Write-Host "$(Split-Path $Exe -Leaf) exit code $($p.ExitCode)"
    if ($p.ExitCode -ne 0) { Fail "$(Split-Path $Exe -Leaf) ended with exit code $($p.ExitCode)" }
}
function CheckClean {
    Start-Sleep 15                    # anything left over has had time to end
    Show
    $n = (Procs 'usbeam-service').Count; if ($n -ne 1) { Fail "$n USBeam services running, expected 1" }
    $n = (Procs 'USBeam').Count;         if ($n -ne 1) { Fail "$n USBeam apps running, expected 1" }
    $left = @(Get-Process | Where-Object { $_.Name -like 'USBeam-Setup*' -or $_.Name -like 'USBip-*' })
    if ($left.Count) { Fail "installers still running: $($left.Name -join ', ')" }
    if ((Get-Service USBeam -ErrorAction SilentlyContinue).Status -ne 'Running') { Fail 'the USBeam service is not running' }
}

Say "0. Install the usbip-win2 driver first, silently"
# The USBeam installer shows a message before it installs the driver itself, which would wait for a
# click here. With the driver already in place it skips that (and so does the previous release).
$p = Start-Process -FilePath $Usbip -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /TYPE=compact /TASKS="vcredist"' -PassThru
$null = $p.Handle
if (-not $p.WaitForExit(600000)) { Fail 'the usbip-win2 installer did not finish in 10 minutes'; exit 1 }
$ctl = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'ROOT\USBIP_WIN2\UDE' })
Write-Host "usbip-win2 installer exit code $($p.ExitCode); virtual USB controllers: $($ctl.Count)"
if ($ctl.Count -ne 1) { Fail "usbip-win2 isn't working on this test machine ($($ctl.Count) controllers), so USBeam can't be installed here"; exit 1 }

Say "1. Install the previous release ($(Split-Path $Old -Leaf))"
RunSetup $Old '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' 'old-install.log'
if (-not (Installed)) { Fail 'the previous release did not install'; Get-Content "$Logs\old-install.log" -Tail 40 | Write-Host; exit 1 }
Write-Host "Installed: $(Installed)"
Show

Say "2. Update to $NewVersion the way the app does (installer started with the same options)"
# The app saves the download in the temporary folder as USBeam-Setup-<version>.exe and runs it.
$dl = Join-Path $env:TEMP "USBeam-Setup-$NewVersion.exe"
Copy-Item $New $dl -Force
Copy-Item $Old (Join-Path $env:TEMP 'USBeam-Setup-0.0.1.exe') -Force     # an older download, to be cleaned up
RunSetup $dl '/SILENT /SUPPRESSMSGBOXES /NORESTART' 'update.log'
if ((Installed) -ne $NewVersion) { Fail "installed version is $(Installed), expected $NewVersion" }
CheckClean
if (Test-Path (Join-Path $env:TEMP 'USBeam-Setup-0.0.1.exe')) { Fail 'an old downloaded installer was left in the temporary folder' }

Say "3. Update again over the same version"
Copy-Item $New $dl -Force
RunSetup $dl '/SILENT /SUPPRESSMSGBOXES /NORESTART' 'update2.log'
CheckClean

Say "4. Start the app four more times (three at once): still one copy"
$app = Join-Path (AppDir) 'USBeam.exe'
1..3 | ForEach-Object { Start-Process $app }
Start-Sleep 2; Start-Process $app -ArgumentList '--tray'
CheckClean

Say "5. Uninstall"
$dir = AppDir
$env:USBEAM_REMOVE_DRIVER = '1'     # remove the driver too, without asking (no one to click)
RunSetup (Join-Path $dir 'unins000.exe') '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' 'uninstall.log'
# The uninstaller restarts itself from the temporary folder: wait until it is really done.
WaitFor 'the uninstaller did not finish' 300 { -not (Test-Path $AppKey) -and -not (Get-Process | Where-Object { $_.Name -like '_iu*' }) } | Out-Null
Start-Sleep 10
Show
foreach ($n in 'USBeam', 'usbeam-service') { if ((Procs $n).Count) { Fail "$n still running after uninstalling" } }
if (Get-Service USBeam -ErrorAction SilentlyContinue) { Fail 'the USBeam service still exists after uninstalling' }
if (Test-Path $dir) { Fail "left behind after uninstalling: $dir" }
if (Test-Path "$env:ProgramData\USBeam") { Fail "left behind after uninstalling: $env:ProgramData\USBeam" }
if (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' -Name USBeam -ErrorAction SilentlyContinue) { Fail 'the sign-in entry was left behind' }
if (Test-Path 'HKLM:\SOFTWARE\USBeam') { Fail 'USBeam''s settings key was left behind' }
if (@(Get-ChildItem $env:TEMP -Filter 'USBeam-Setup-*.exe' -ErrorAction SilentlyContinue).Count) { Fail 'downloaded installers left in the temporary folder' }
if (Test-Path "$env:TEMP\USBeam-uninstall.log") { Get-Content "$env:TEMP\USBeam-uninstall.log" | Write-Host }

if ($script:Fail) { Write-Host ''; Write-Host 'Windows upgrade test FAILED'; exit 1 }
Write-Host ''; Write-Host 'PASS: Windows upgrade test'; exit 0
