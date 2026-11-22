# Synthetic observer contract tests only; no native pairing/input/capture proof.
param([string]$Observer=(Join-Path $PSScriptRoot 'windows-observe.ps1'))
$ErrorActionPreference='Stop'
$root=Join-Path $PSScriptRoot ('output\observer-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $root -Force | Out-Null
try {
    foreach($case in @('positive','bad-exit','late-loss','stale-receipt','deadline')) {
        $d=Join-Path $root $case;New-Item -ItemType Directory $d | Out-Null
        $log=Join-Path $d 'native.log';$stop=Join-Path $d 'stop';$done=Join-Path $d 'done'
        [IO.File]::WriteAllText($log,'')
        $publisher=$null;$job=$null;$id='md-l7-test-'+[guid]::NewGuid().ToString('N')
        if($case -eq 'stale-receipt') {
            [IO.File]::WriteAllText($done,'{"native_exit":0,"helper_result":0,"deadline":false}')
        } elseif($case -ne 'deadline') {
            # Output creation is after the observer's freshness guard. Publish
            # only then, with a rename like the real native helper uses.
            $data=@{log=$log;done=$done;code=$(if($case -eq 'bad-exit'){7}else{0});loss=($case -eq 'late-loss')}
            $publisher=[IO.FileSystemWatcher]::new($d,'native.log.observed.jsonl')
            $job=Register-ObjectEvent $publisher Created -SourceIdentifier $id -MessageData $data -Action {
                $s=$Event.MessageData
                $line="client: packets=1 decoded=1 displayed=1 decode_avg=1.0ms rtt=0ms rx=0.1MB`n"
                if($s.loss){$line+="client: connection lost`n"}
                [IO.File]::WriteAllText($s.log,$line)
                [IO.File]::WriteAllText($s.done+'.tmp',(@{native_exit=$s.code;helper_result=$s.code;deadline=$false}|ConvertTo-Json -Compress))
                [IO.File]::Move($s.done+'.tmp',$s.done)
            }
            $publisher.EnableRaisingEvents=$true
        }
        $failed=$false;$reason=''
        try { & $Observer -Log $log -StopFile $stop -DoneFile $done -TargetFrames 1 -TimeoutSeconds 5 }
        catch {$failed=$true;$reason=$_.Exception.Message}
        finally {
            if($publisher){Unregister-Event -SourceIdentifier $id;Remove-Job $job -Force;$publisher.Dispose()}
        }
        $expected=@{'bad-exit'='observation failed';'late-loss'='observation failed';'stale-receipt'='fresh control files';'deadline'='failure deadline'}
        if($case -eq 'positive') {
            if($failed){throw "positive case failed: $reason"}
        } elseif(!$failed -or $reason -notmatch $expected[$case]) {throw "wrong negative outcome: $case ($reason)"}
        $records=@()
        if(Test-Path ($log+'.observed.jsonl')){$records=@(Get-Content ($log+'.observed.jsonl')|ForEach-Object {$_|ConvertFrom-Json})}
        $passes=@($records|Where-Object {$_.event -eq 'result' -and $_.pass})
        if(($case -eq 'positive' -and $passes.Count -ne 1) -or ($case -ne 'positive' -and $passes.Count -ne 0)){throw "false/missing PASS: $case"}
        if($case -eq 'stale-receipt') {
            if((Test-Path $stop) -or (Test-Path ($log+'.observed.jsonl'))){throw 'stale input changed control/output files'}
        } elseif(!(Test-Path $stop)){throw "missing cancellation: $case"}
        Write-Output "observer selftest $case PASS (synthetic only)"
    }
} finally {Remove-Item $root -Recurse -Force}
