# metadesk — tools/win/run-interactive.ps1
#
# Run a .cmd file in the logged-in console session (SSH is session 0 and has
# no desktop). Registers a uniquely named one-shot scheduled task with an
# Interactive logon principal, waits for the .cmd to create -DoneFile (or for
# the deadline), and always stops/unregisters the task. The .cmd itself must
# write its outputs and create -DoneFile last.
#
#   powershell -ExecutionPolicy Bypass -File run-interactive.ps1 `
#       -Command C:\path\job.cmd -DoneFile C:\path\job.done [-TimeoutSeconds 300]
#
# Exit codes: 0 done, 2 timed out, 3 no active console session (record the
# interactive cell as pending; do not fall back to session 0 numbers).
param(
    [Parameter(Mandatory = $true)][string]$Command,
    [Parameter(Mandatory = $true)][string]$DoneFile,
    [int]$TimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'

$sessions = (query session 2>&1) -join "`n"
$sessions
if ($sessions -notmatch '(?m)^\s*>?console\s+\S+\s+\d+\s+Active') {
    'run-interactive: no active console session; interactive cell pending'
    exit 3
}

Remove-Item $DoneFile -Force -ErrorAction SilentlyContinue
$name = 'metadesk-uia-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$finished = $false
try {
    $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"$Command`""
    $principal = New-ScheduledTaskPrincipal -UserId (whoami) -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName $name
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not (Test-Path $DoneFile) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
    }
    $finished = Test-Path $DoneFile
    "run-interactive: task $name finished=$finished"
    (Get-ScheduledTaskInfo -TaskName $name |
        Select-Object LastRunTime, LastTaskResult | Format-List | Out-String).Trim()
} finally {
    Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
}
if (-not $finished) { exit 2 }
exit 0
