[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Install','Run','Stop','Uninstall')][string]$Action,
    [string]$HostExe = "$PSScriptRoot\metadesk-host.exe",
    [string]$TaskName = ('Metadesk Host ' + [Security.Principal.WindowsIdentity]::GetCurrent().User.Value),
    [string]$HostArguments = '--managed',
    [string]$LogDirectory = (Join-Path $env:LOCALAPPDATA 'Metadesk\Logs')
)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
if ($Action -eq 'Install') {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) { throw 'Task exists; uninstall it explicitly first' }
    $HostExe = (Resolve-Path $HostExe).Path
    # No password, elevation, SYSTEM principal, unlock trigger, or session-0 service.
    $principal = New-ScheduledTaskPrincipal -UserId $identity -LogonType Interactive -RunLevel Limited
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $identity
    $command = New-ScheduledTaskAction -Execute "$PSHOME\powershell.exe" -Argument "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Action Run -HostExe `"$HostExe`" -HostArguments `"$HostArguments`" -LogDirectory `"$LogDirectory`""
    $settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -DisallowHardTerminate
    Register-ScheduledTask -TaskName $TaskName -Action $command -Trigger $trigger -Principal $principal -Settings $settings | Out-Null
} elseif ($Action -eq 'Run') {
    $logs = $LogDirectory
    New-Item -ItemType Directory -Force $logs | Out-Null
    $p = Start-Process -FilePath $HostExe -ArgumentList $HostArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\host.stdout.log" -RedirectStandardError "$logs\host.stderr.log"
    $null = $p.Handle
    $p.WaitForExit()
    $code = $p.ExitCode
    if ($code -eq 78) { exit 0 } # diagnostic is in stderr; no restart storm
    exit $code
} else {
    $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if (!$task) { return }
    if ($task.Principal.UserId -match '^S-1-') { $sid = $task.Principal.UserId }
    else {
        $owner = New-Object Security.Principal.NTAccount $task.Principal.UserId
        $sid = $owner.Translate([Security.Principal.SecurityIdentifier]).Value
    }
    if ($sid -ne [Security.Principal.WindowsIdentity]::GetCurrent().User.Value) { throw 'Task belongs to another user' }
    if ($task.State -eq 'Running') {
        # Must be invoked in the same interactive session as the task. Task
        # Scheduler End/Stop-ScheduledTask would forcibly terminate the host.
        $session = (Get-Process -Id $PID).SessionId
        $children = @(Get-Process | Where-Object { $_.Path -eq $HostExe -and $_.SessionId -eq $session })
        foreach ($child in $children) { $null = $child.Handle }
        & $HostExe --managed-stop
        if ($LASTEXITCODE) { throw 'No managed host in this session; do not force-terminate it' }
        foreach ($child in $children) {
            if (!$child.WaitForExit(30000)) { throw 'Host stop watchdog expired; refusing forced termination' }
        }
    }
    if ($Action -eq 'Uninstall') { Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false }
}
