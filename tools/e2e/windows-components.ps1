# Native Windows prerequisites/components only; NEVER a host/client pairing PASS.
# Run Prepare over SSH; run Console only through tools/win/run-interactive.ps1.
param(
    [ValidateSet('Prepare', 'Console', 'SelfTest')][string]$Phase = 'Prepare',
    [string]$Root = "$env:USERPROFILE\md-l7"
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path $Root).Path
if ((Split-Path $Root -Leaf) -notmatch '^md-l7(?:-|$)') {
    throw 'Use an isolated md-l7 or md-l7-* source directory'
}
Set-Location $Root
$out = Join-Path $Root 'tools\e2e\output'
New-Item -ItemType Directory -Force $out | Out-Null
$env:PATH = "C:\msys64\clangarm64\bin;C:\msys64\usr\bin;" + $env:PATH
$env:MSYSTEM = 'CLANGARM64'
$env:CHERE_INVOKING = '1'
# A private job's KILL_ON_JOB_CLOSE also covers abrupt PowerShell termination.
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
public sealed class MDL7Job : IDisposable {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetInformationJobObject(IntPtr job, int cls, IntPtr info, uint length);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [StructLayout(LayoutKind.Sequential)] struct Basic {
        public long ProcessTime, JobTime;
        public uint Flags;
        public UIntPtr MinWorking, MaxWorking;
        public uint Active;
        public UIntPtr Affinity;
        public uint Priority, Scheduling;
    }
    [StructLayout(LayoutKind.Sequential)] struct IO {
        public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes;
    }
    [StructLayout(LayoutKind.Sequential)] struct Extended {
        public Basic Basic;
        public IO IO;
        public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
    }
    IntPtr handle;
    public MDL7Job() {
        handle = CreateJobObject(IntPtr.Zero, null);
        if (handle == IntPtr.Zero) throw new Win32Exception();
        var info = new Extended(); info.Basic.Flags = 0x2000;
        int size = Marshal.SizeOf(info); IntPtr ptr = Marshal.AllocHGlobal(size);
        try {
            Marshal.StructureToPtr(info, ptr, false);
            if (!SetInformationJobObject(handle, 9, ptr, (uint)size)) {
                int error = Marshal.GetLastWin32Error(); Dispose(); throw new Win32Exception(error);
            }
        } finally { Marshal.FreeHGlobal(ptr); }
    }
    public void Add(Process process) {
        if (!AssignProcessToJobObject(handle, process.Handle)) throw new Win32Exception();
    }
    public void Dispose() {
        if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; }
    }
}
'@
$results = @()
function Run-Check([string]$Name, [string]$Exe, [string[]]$Arguments, [int]$TimeoutSeconds = 120) {
    $log = Join-Path $out "$Phase-$Name.log"
    $stderr = "$log.stderr"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $job = New-Object MDL7Job
    $process = $null
    $event = $null
    $code = $null
    $timedOut = $false
    $failure = $null
    try {
        if (!(Test-Path -PathType Leaf $Exe)) { throw "Executable does not exist: $Exe" }
        # A gated PowerShell child cannot launch descendants before assignment to
        # the private job. Closing this parent's job handle kills the entire tree.
        $gate = 'Local\md-l7-' + [guid]::NewGuid().ToString('N')
        $event = New-Object Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $gate)
        $payload = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes((@{
            exe = $Exe; arguments = @($Arguments); gate = $gate
        } | ConvertTo-Json -Compress)))
        $child = '$ProgressPreference="SilentlyContinue"; $d=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String("' + $payload + '")) | ConvertFrom-Json; ' +
                 '$g=[Threading.EventWaitHandle]::OpenExisting($d.gate); ' +
                 'if (!$g.WaitOne(30000)) {exit 124}; $g.Dispose(); ' +
                 '$a=@($d.arguments); & $d.exe @a 2>&1 | ForEach-Object { [Console]::WriteLine($_.ToString()) }; exit $LASTEXITCODE'
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($child))
        $process = Start-Process -FilePath "$PSHOME\powershell.exe" -ArgumentList @('-NoProfile','-EncodedCommand',$encoded) `
            -PassThru -NoNewWindow -RedirectStandardOutput $log -RedirectStandardError $stderr
        $job.Add($process)
        $event.Set() | Out-Null
        if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
        } else {
            $process.Refresh()
            $code = $process.ExitCode
        }
    } catch {
        $failure = $_.Exception.Message
    } finally {
        $job.Dispose()
        if ($process) {
            try { if (!$process.HasExited) { $process.Kill() } } catch [InvalidOperationException] {}
            $process.WaitForExit(); $process.Dispose()
        }
        if ($event) { $event.Dispose() }
    }
    $timer.Stop()
    if (Test-Path $stderr) {
        $errors = Get-Content -Raw $stderr
        if ($errors) { Add-Content -Encoding utf8 $log $errors }
        Remove-Item $stderr
    }
    if ($failure) { Add-Content -Encoding utf8 $log "HARNESS: launch/job failure: $failure" }
    if ($timedOut) { Add-Content -Encoding utf8 $log 'HARNESS: deadline exceeded; owned job tree terminated' }
    $script:results += [pscustomobject]@{
        name = $Name; exit_code = $code; seconds = $timer.Elapsed.TotalSeconds
        timed_out = $timedOut; error = $failure; log = (Split-Path $log -Leaf)
    }
    Write-Host "$Name exit=$code timeout=$timedOut seconds=$($timer.Elapsed.TotalSeconds)"
    return $code
}
if ($Phase -eq 'SelfTest') {
    Run-Check 'exit-seven' "$PSHOME\powershell.exe" @('-NoProfile','-Command','exit 7') | Out-Null
    Run-Check 'missing-exe' "$Root\does-not-exist.exe" @() | Out-Null
    $pidFile = Join-Path $out 'timeout-child.pid'
    $hang = '[IO.File]::WriteAllText("' + $pidFile + '", [string]$PID); [Threading.ManualResetEvent]::new($false).WaitOne()'
    $hangEncoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($hang))
    Run-Check 'timeout' "$PSHOME\powershell.exe" @('-NoProfile','-EncodedCommand',$hangEncoded) 5 | Out-Null
    $childGone = (Test-Path $pidFile) -and !(Get-Process -Id ([int](Get-Content $pidFile)) -ErrorAction SilentlyContinue)
    $selfTestOK = $results[0].exit_code -eq 7 -and $null -eq $results[1].exit_code -and
                  $results[2].timed_out -and $childGone
    Write-Host "self-test expected exit, missing launch, deadline and child reaping: $selfTestOK"
} elseif ($Phase -eq 'Prepare') {
    Get-ComputerInfo -Property WindowsProductName,WindowsVersion,OsBuildNumber,OsArchitecture |
        ConvertTo-Json | Set-Content (Join-Path $out 'windows.json')
    query user | Set-Content (Join-Path $out 'sessions.txt')
    Get-Process fips*,metadesk* -ErrorAction SilentlyContinue |
        Select-Object Id,ProcessName,Path | ConvertTo-Json | Set-Content (Join-Path $out 'processes-before.json')
    Get-Service *fips* -ErrorAction SilentlyContinue |
        Select-Object Name,Status | ConvertTo-Json | Set-Content (Join-Path $out 'services.json')
    $bash = 'C:\msys64\usr\bin\bash.exe'
    # Write a script: nested SSH/PowerShell/bash quotation is not reliable.
    @'
set +e
for p in libavcodec libavutil libcjson libb2 libcurl sdl2 libyuv libnostr; do
  printf '%s: ' "$p"
  pkg-config --modversion "$p" 2>&1
done
command -v fips fipsd fipsctl || true
'@ | Set-Content -Encoding ascii (Join-Path $out 'inventory.sh')
    $posix = '/' + $Root.Substring(0,1).ToLower() + $Root.Substring(2).Replace('\','/')
    Run-Check 'dependencies' $bash @('-lc', "bash $posix/tools/e2e/output/inventory.sh") | Out-Null
    Run-Check 'product-configure' $bash @('-lc', "cd $posix && meson setup build-product") | Out-Null
    $media = Run-Check 'build-media' $bash @('-lc', "cd $posix && bash tools/win/build-media-harness.sh")
    $uia = Run-Check 'build-uia' $bash @('-lc', "cd $posix && bash tools/win/build-uia-harness.sh")
    if ($media -eq 0) {
        foreach ($test in @('test_capture_dxgi_win','test_sendinput_win','test_win_dpi_win')) {
            Run-Check "session0-$test" "$Root\build-win-media\$test.exe" @() | Out-Null
        }
    }
    if ($uia -eq 0) {
        foreach ($test in @('test_atspi','test_uia_liveness','test_uia_walk_equiv')) {
            Run-Check "session0-$test" "$Root\build-win-uia\$test.exe" @() | Out-Null
        }
    }
} else {
    # No lock/disconnect tests. The DPI probe injects ONE click in its OWN window.
    $sessions = (query session 2>&1) -join "`n"
    $current = [Diagnostics.Process]::GetCurrentProcess().SessionId
    if ($sessions -notmatch '(?m)^\s*>?console\s+\S+\s+(\d+)\s+Active' -or
        $current -ne [int]$Matches[1]) {
        throw "Run Console in the active console session, not session $current"
    }
    Run-Check 'dpi-manifest' "$Root\build-win-media\test_win_dpi_manifest.exe" @() | Out-Null
    Run-Check 'dxgi' "$Root\build-win-media\test_capture_dxgi_win.exe" @('--interactive') | Out-Null
    Run-Check 'dpi-owned-click' "$Root\build-win-media\test_win_dpi_live.exe" @() | Out-Null
    Run-Check 'uia-liveness' "$Root\build-win-uia\test_uia_liveness.exe" @('--interactive') | Out-Null
    Run-Check 'uia-equivalence' "$Root\build-win-uia\test_uia_walk_equiv.exe" @('--interactive') | Out-Null
    $env:MD_A11Y_LIVE_TESTS = '1'
    Run-Check 'a11y-live' "$Root\build-win-uia\test_atspi.exe" @() | Out-Null
}
[pscustomobject]@{
    scope = 'native components, NOT host/client E2E'
    utc = [DateTime]::UtcNow.ToString('o')
    source_commit = (Get-Content "$Root\SOURCE_COMMIT" -Raw).Trim()
    phase = $Phase; results = $results
} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out "$Phase.json")
# The wrapper must write its done marker after this process exits, never here.
if ($Phase -eq 'SelfTest') { if ($selfTestOK) { exit 0 } else { exit 1 } }
if (@($results | Where-Object { $_.exit_code -ne 0 }).Count) { exit 1 }
exit 0
