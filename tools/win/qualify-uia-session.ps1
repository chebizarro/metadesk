# Opt-in real OS session-loss qualification, not a synthetic WTS message.
# Run only on an owned VM with a person available to unlock/reconnect its console.
# SYSTEM tscon recovery is NOT assumed to work (Win11 build 26300: error 7045).
# From a synced Windows checkout after building tools/win/build-uia-harness.sh:
# powershell -ExecutionPolicy Bypass -File tools/win/qualify-uia-session.ps1 -Loss Lock -AcknowledgeManualRecovery
# Unlock the UTM console after it returns; repeat with -Loss Disconnect, then
# reconnect/sign in. Verify test_uia_liveness.exe --interactive after each run.
# Collect the printed OutputDirectory and remove it after inspecting the logs.
param(
    [ValidateSet('Lock', 'Disconnect')][string]$Loss = 'Lock',
    [string]$HarnessDirectory = "$PSScriptRoot\..\..\build-win-uia",
    [string]$OutputDirectory = "$env:TEMP\metadesk-session-$([guid]::NewGuid().ToString('N'))",
    [switch]$AcknowledgeManualRecovery,
    [switch]$Worker
)
$ErrorActionPreference = 'Stop'
if (-not $AcknowledgeManualRecovery) {
    throw 'Requires -AcknowledgeManualRecovery: a person must unlock/reconnect the owned console afterwards.'
}
$HarnessDirectory = (Resolve-Path $HarnessDirectory).Path
$exe = Join-Path $HarnessDirectory 'test_uia_liveness.exe'
if (-not (Test-Path $exe)) { throw "Build the UIA harness first: $exe missing" }
$env:PATH = 'C:\msys64\clangarm64\bin;' + $env:PATH
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path $OutputDirectory).Path
if (-not $Worker) {
    # The runner always unregisters the unique Interactive task. Results are left
    # in OutputDirectory for inspection; remove that directory after collecting.
    $cmd = Join-Path $OutputDirectory 'run.cmd'
    @"
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSCommandPath" -Worker -Loss $Loss -HarnessDirectory "$HarnessDirectory" -OutputDirectory "$OutputDirectory" -AcknowledgeManualRecovery > "$OutputDirectory\worker.log" 2>&1
echo %errorlevel% > "$OutputDirectory\exit.txt"
echo done > "$OutputDirectory\done"
"@ | Set-Content -Encoding ASCII $cmd
    & "$PSScriptRoot\run-interactive.ps1" -Command $cmd -DoneFile "$OutputDirectory\done" -TimeoutSeconds 210
    $runnerExit = $LASTEXITCODE
    if (Test-Path "$OutputDirectory\worker.log") { Get-Content "$OutputDirectory\worker.log" }
    Write-Output "Results: $OutputDirectory"
    Write-Output 'If loss was delivered: unlock/reconnect at the UTM console NOW, then verify --interactive passes.'
    if ($runnerExit -ne 0) { exit $runnerExit }
    exit [int](Get-Content "$OutputDirectory\exit.txt")
}

$process = $null
$before = $null
$startup = $null
$watcher = $null
try {
    # Fail before changing the session unless this is a genuinely usable desktop.
    $before = Start-Process -FilePath $exe -ArgumentList '--interactive' -PassThru -NoNewWindow -RedirectStandardOutput "$OutputDirectory\before.log" -RedirectStandardError "$OutputDirectory\before.stderr.log"
    if (-not $before.WaitForExit(45000)) { throw 'Console preflight timed out; no loss action attempted.' }
    if ($before.ExitCode -ne 0) { throw 'Console is not usable; see before.log. No loss action attempted.' }
    $stdout = "$OutputDirectory\loss.stdout.log"
    $stderr = "$OutputDirectory\loss.stderr.log"
    $watcher = New-Object System.IO.FileSystemWatcher $OutputDirectory
    $watcher.EnableRaisingEvents = $true
    $process = Start-Process -FilePath $exe -ArgumentList '--wait-for-loss' -PassThru -NoNewWindow -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $deadline = (Get-Date).AddSeconds(45)
    while ($true) {
        if ((Test-Path $stdout) -and ((Get-Content $stdout -Raw) -match 'READY:')) { break }
        if ($process.HasExited) { throw 'Harness exited before READY; see loss logs.' }
        if ((Get-Date) -ge $deadline) { throw 'Harness never became READY; no loss action attempted.' }
        $null = $watcher.WaitForChanged([IO.WatcherChangeTypes]::All, 1000)
    }
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class MdSessionLoss {
    [DllImport("user32.dll", SetLastError=true)] public static extern bool LockWorkStation();
    [DllImport("wtsapi32.dll", SetLastError=true)] public static extern bool WTSDisconnectSession(IntPtr server, int session, bool wait);
}
'@
    $session = [Diagnostics.Process]::GetCurrentProcess().SessionId
    if ($Loss -eq 'Lock') { $ok = [MdSessionLoss]::LockWorkStation() }
    else { $ok = [MdSessionLoss]::WTSDisconnectSession([IntPtr]::Zero, $session, $false) }
    if (-not $ok) { throw "OS $Loss failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    Write-Output "OS $Loss requested in session=$session at $([DateTime]::UtcNow.ToString('o'))"
    if (-not $process.WaitForExit(45000)) { throw 'Loss harness did not exit within its bound.' }
    if ($process.ExitCode -ne 0) { throw "Loss harness failed exit=$($process.ExitCode); see loss logs." }
    if ((Get-Content $stdout -Raw) -notmatch 'ALL UIA LIVENESS TESTS PASSED') { throw 'Harness success receipt missing.' }
    Get-Content $stdout
    $startup = Start-Process -FilePath $exe -ArgumentList '--expect-unavailable' -PassThru -NoNewWindow -RedirectStandardOutput "$OutputDirectory\unavailable-startup.log" -RedirectStandardError "$OutputDirectory\unavailable-startup.stderr.log"
    if (-not $startup.WaitForExit(45000)) { throw 'Unavailable startup timed out; manual console recovery required.' }
    if ($startup.ExitCode -ne 0) { throw 'Unavailable startup failed; see unavailable-startup.log.' }
    Get-Content "$OutputDirectory\unavailable-startup.log"
    Write-Output 'PASS OS-generated loss and unavailable startup. MANUAL CONSOLE RECOVERY REQUIRED.'
} finally {
    foreach ($child in @($before, $process, $startup)) {
        if ($child -and -not $child.HasExited) { Stop-Process -Id $child.Id -Force -ErrorAction SilentlyContinue }
    }
    if ($watcher) { $watcher.Dispose() }
}
