# Observe an owned Windows client log until real decoded/displayed counters reach
# a target, then cancel through the owned fixture and require its native receipt.
# File notifications gate observation; deadlines are failure bounds, not success.
param(
    [Parameter(Mandatory=$true)][string]$Log,
    [Parameter(Mandatory=$true)][string]$StopFile,
    [Parameter(Mandatory=$true)][string]$DoneFile,
    [int]$TargetFrames=6000,
    [int]$TimeoutSeconds=1200
)
$ErrorActionPreference='Stop'
$paths=@($Log,$StopFile,$DoneFile) | ForEach-Object {[IO.Path]::GetFullPath($_)}
$Log,$StopFile,$DoneFile=$paths
$root=[IO.Path]::GetDirectoryName($Log)
if($TargetFrames -lt 1 -or $TimeoutSeconds -lt 1 -or
   [IO.Path]::GetDirectoryName($StopFile) -ne $root -or
   [IO.Path]::GetDirectoryName($DoneFile) -ne $root -or
   (Test-Path $StopFile) -or (Test-Path $DoneFile)) {
    throw 'Use positive bounds and fresh control files in the owned log directory'
}
$out=[IO.File]::Open($Log+'.observed.jsonl',[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
$writer=[IO.StreamWriter]::new($out,[Text.UTF8Encoding]::new($false))
$writer.AutoFlush=$true
$watcher=[IO.FileSystemWatcher]::new($root,'*')
$watcher.NotifyFilter=[IO.NotifyFilters]::FileName -bor [IO.NotifyFilters]::LastWrite -bor [IO.NotifyFilters]::Size
$tag='md-l7-'+[guid]::NewGuid().ToString('N')
$ids=@("${tag}-changed","${tag}-created","${tag}-renamed")
$clock=[Diagnostics.Stopwatch]::StartNew()
$offset=0L;$partial='';$decoded=0L;$displayed=0L;$losses=0;$stopped=$false;$completed=$false
function Emit($record) {
    $record.utc=[DateTime]::UtcNow.ToString('o')
    $record.elapsed=$clock.Elapsed.TotalSeconds
    $writer.WriteLine(($record | ConvertTo-Json -Compress))
}
function Drain-NativeLog {
    if(!(Test-Path $Log)){return}
    $stream=[IO.File]::Open($Log,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try {
        if($stream.Length -lt $script:offset){throw 'Owned log was truncated'}
        [void]$stream.Seek($script:offset,[IO.SeekOrigin]::Begin)
        $reader=[IO.StreamReader]::new($stream,[Text.UTF8Encoding]::new($false))
        $text=$reader.ReadToEnd();$script:offset=$stream.Position
    } finally {$stream.Dispose()}
    $lines=@(($script:partial+$text) -split "`n");$script:partial=$lines[-1]
    for($i=0;$i -lt $lines.Count-1;$i++) {
        $line=$lines[$i].TrimEnd("`r")
        if($line -match '^client: packets=(\d+) decoded=(\d+) displayed=(\d+).*rx=([\d.]+)MB') {
            $script:decoded=[long]$Matches[2];$script:displayed=[long]$Matches[3]
            Emit @{event='stats';decoded=$decoded;displayed=$displayed;rx_mib=[double]$Matches[4]}
        } elseif($line -eq 'client: connection lost') {
            $script:losses++;Emit @{event='unexpected_disconnect';count=$losses}
        } elseif($line -match '^client: done\. decoded (\d+) frames, displayed (\d+)') {
            $script:decoded=[long]$Matches[1];$script:displayed=[long]$Matches[2]
            Emit @{event='native_summary';decoded=$decoded;displayed=$displayed}
        }
    }
}
try {
    Emit @{event='begin';target=$TargetFrames;scope='counter/exit observation, not full E2E acceptance'}
    Register-ObjectEvent $watcher Changed -SourceIdentifier $ids[0] | Out-Null
    Register-ObjectEvent $watcher Created -SourceIdentifier $ids[1] | Out-Null
    Register-ObjectEvent $watcher Renamed -SourceIdentifier $ids[2] | Out-Null
    $watcher.EnableRaisingEvents=$true
    while($true) {
        Drain-NativeLog
        if(!$stopped -and $decoded -ge $TargetFrames -and $displayed -ge $TargetFrames) {
            New-Item -ItemType File -Path $StopFile | Out-Null
            $stopped=$true;Emit @{event='target_reached';target=$TargetFrames}
        }
        if(Test-Path $DoneFile) {
            $native=Get-Content -Raw $DoneFile | ConvertFrom-Json
            # Publication is after child exit: its remaining log tail is stable.
            Drain-NativeLog
            $pass=$stopped -and $decoded -ge $TargetFrames -and $displayed -ge $TargetFrames -and
                  $losses -eq 0 -and $native.native_exit -eq 0 -and
                  $native.helper_result -eq 0 -and !$native.deadline
            Emit @{event='result';pass=$pass;decoded=$decoded;displayed=$displayed;losses=$losses;native=$native}
            if(!$pass){throw 'Owned native stream observation failed; inspect receipt'}
            $completed=$true
            break
        }
        $remaining=$TimeoutSeconds-$clock.Elapsed.TotalSeconds
        if($remaining -le 0){throw 'Owned observation failure deadline'}
        # Like Get-Content -Wait: Windows regular-file metadata notifications
        # can lag a writer. Tail only unread bytes, never poll relay state or
        # infer success from this wait; counters plus fresh exit receipt decide.
        $events=@(Wait-Event -Timeout ([int][Math]::Min(1,[Math]::Ceiling($remaining))))
        foreach($event in $events){Remove-Event -EventIdentifier $event.EventIdentifier}
    }
} catch {
    Emit @{event='error';message=$_.Exception.Message}
    throw
} finally {
    if(!(Test-Path $DoneFile) -and !(Test-Path $StopFile)) {
        New-Item -ItemType File -Path $StopFile | Out-Null
    }
    foreach($id in $ids){Unregister-Event -SourceIdentifier $id -ErrorAction SilentlyContinue;Get-Event -SourceIdentifier $id -ErrorAction SilentlyContinue | Remove-Event}
    $watcher.Dispose();$writer.Dispose()
}
if(!$completed){throw 'Observer ended without validated native completion'}
