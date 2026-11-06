# SPDX-License-Identifier: GPL-3.0-or-later
# Windows smooth-input test (GitHub Actions). The "other PC" is tests/fakehid.c: a simulated 1000 Hz
# HID device behind a home-Wi-Fi-like delay. usbip-win2 (the driver USBeam uses) attaches it the
# old way (directly) and the new way (through USBeam's relay, tests/relay_win_test.c); Windows' own
# HID driver reads it (tests/winhid_measure.c). Also measured without the delay: the goal.
param([string]$Usbip, [string]$Bin)
$ErrorActionPreference = 'Continue'
$N = 3000
$fails = 0
function Say($m) { Write-Host $m }
$notes = New-Object System.Collections.Generic.List[string]
function Note($m) { Say $m; $notes.Add($m) }

Say "== usbip-win2 driver"
$p = Start-Process -FilePath $Usbip -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /TYPE=compact /TASKS="vcredist"' -PassThru
if (-not $p.WaitForExit(600000)) { Note 'FAIL: the usbip-win2 installer did not finish'; exit 1 }
$u = 'C:\Program Files\USBip\usbip.exe'
if (-not (Test-Path $u)) { Note 'FAIL: usbip.exe missing'; exit 1 }

function Run([string]$label, [bool]$relay, [bool]$delay, [string]$mode = '') {
    $fargs = "$($N + 1000)"; if (-not $delay) { $fargs += ' nodelay' }
    $fake = Start-Process -FilePath "$Bin\fakehid.exe" -ArgumentList $fargs -PassThru -NoNewWindow -RedirectStandardOutput "$env:TEMP\fake-$label.txt"
    Start-Sleep 1
    $via = @()
    $rel = $null
    if ($relay) {
        $rel = Start-Process -FilePath "$Bin\relay_win_test.exe" -PassThru -NoNewWindow -RedirectStandardOutput "$env:TEMP\relay-$label.txt"
        $port = $null
        for ($i = 0; $i -lt 50 -and -not $port; $i++) { Start-Sleep -Milliseconds 100; $m = Select-String -Path "$env:TEMP\relay-$label.txt" -Pattern '^PORT (\d+)' -ErrorAction SilentlyContinue; if ($m) { $port = $m.Matches[0].Groups[1].Value } }
        if (-not $port -or $port -eq '0') { Note "FAIL: ${label}: the relay didn't start"; $script:fails++; return $null }
        $via = @('--tcp-port', $port)
    }
    $rm = @(); if ($mode) { $rm = @('--receive-mode', $mode) }
    $out = & $u @via attach -r 127.0.0.1 -b 1-1 --once @rm 2>&1 | Out-String
    $uport = if ($out -match 'port (\d+)') { $Matches[1] } else { $null }
    if (-not $uport) { Note "FAIL: ${label}: attach failed: $out"; $script:fails++; Stop-Process -Id $fake.Id -Force; return $null }
    $res = & "$Bin\winhid_measure.exe" $N $label 2>&1 | Out-String
    & $u detach -p $uport 2>&1 | Out-Null
    Start-Sleep 2
    Stop-Process -Id $fake.Id -Force -ErrorAction SilentlyContinue
    if ($rel) { Stop-Process -Id $rel.Id -Force -ErrorAction SilentlyContinue; Get-Content "$env:TEMP\relay-$label.txt" | Select-String 'smooth input' | ForEach-Object { Note "  $($_.Line)" } }
    foreach ($l in ($res -split "`r?`n")) { if ($l -and $l -notmatch '^NUMBERS') { Note $l } }
    if ($res -match 'NUMBERS (\S+) (\S+) (\S+) (\d)') { return @{ rate = [double]$Matches[1]; p90 = [double]$Matches[2]; p99 = [double]$Matches[3]; ok = $Matches[4] -eq '1' } }
    Note "FAIL: ${label}: no measurement"; $script:fails++
    return $null
}

$goal = Run 'no delay, directly              ' $false $false
$old  = Run 'old way (directly)              ' $false $true
$new  = Run 'new way (through USBeam relay)  ' $true  $true
# Diagnostics: the driver's low-latency receive mode
$null = Run 'old way, low-latency mode       ' $false $true 'low-latency'
$null = Run 'new way, low-latency mode       ' $true  $true 'low-latency'
if ($goal -and $old -and $new) {
    if ($new.rate -ge 0.9 * $goal.rate) { Note "OK: new way: $($new.rate) reports/s (without delay: $($goal.rate))" } else { Note "FAIL: new way: only $($new.rate) reports/s (without delay: $($goal.rate))"; $fails++ }
    if ($new.p90 -le $goal.p90 + 1) { Note "OK: new way: 90% of reports within $($new.p90) ms (without delay: $($goal.p90))" } else { Note "FAIL: new way: 90% of reports within $($new.p90) ms (without delay: $($goal.p90))"; $fails++ }
    if ($new.ok) { Note 'OK: new way: every report arrived once, in order' } else { Note 'FAIL: new way: reports missing or out of order'; $fails++ }
    if ($old.p90 -ge 2 * $new.p90) { Note "OK: the old way is clearly worse ($($old.p90) ms, $($old.rate) reports/s): the test sees the problem" } else { Note "FAIL: the old way wasn't clearly worse ($($old.p90) ms)"; $fails++ }
} else { $fails++ }
if ($fails) { Note 'Windows smooth-input test FAILED' } else { Note 'PASS: Windows smooth-input test' }
$msg = ($notes -join '%0A')
Write-Host "::notice title=Windows smooth-input test::$msg"
exit [int]($fails -ne 0)
