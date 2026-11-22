param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference='Stop'
$script=Join-Path $root 'host-task.ps1'
$exe=Join-Path $root 'test_host_lifecycle.exe'
$readyName='Global\md-w17-ready-'+[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$ready=New-Object Threading.EventWaitHandle $false,([Threading.EventResetMode]::ManualReset),$readyName
try {
  & $script -Action Install -TaskName 'md-w17-host' -HostExe $exe -HostArguments "--ready-event $readyName" -LogDirectory "$root\logs"
  & $script -Action Install -TaskName 'md-w17-stop' -HostExe $exe -HostArguments '--managed-stop' -LogDirectory "$root\stop-logs"
  Start-ScheduledTask -TaskName 'md-w17-host'
  if (!$ready.WaitOne(30000)) { throw 'Host ready-event watchdog expired' }
  $child=Get-CimInstance Win32_Process | Where-Object ExecutablePath -EQ $exe
  if (@($child).Count -ne 1) { throw 'Expected exactly one fixture process' }
  $process=Get-Process -Id $child.ProcessId
  $wrapper=Get-Process -Id $child.ParentProcessId
  $null=$process.Handle; $null=$wrapper.Handle
  Start-ScheduledTask -TaskName 'md-w17-stop'
  if (!$process.WaitForExit(30000) -or !$wrapper.WaitForExit(30000)) { throw 'Graceful stop watchdog expired' }
  if ($process.ExitCode -ne 0 -or $wrapper.ExitCode -ne 0) { throw "Bad child/wrapper exit: $($process.ExitCode)/$($wrapper.ExitCode)" }
  & $exe --config
  if ($LASTEXITCODE -ne 78) { throw 'Config exit must be 78' }
  & $exe --failure
  if ($LASTEXITCODE -ne 1) { throw 'Runtime exit must be 1' }
  'PASS: InteractiveToken per-user tasks installed/started; same-session named-event stop; child/wrapper exit 0; config 78/runtime 1'
  Get-Content "$root\logs\host.stderr.log"
} finally {
  $ready.Dispose()
  foreach ($name in 'md-w17-host','md-w17-stop') {
    $t=Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
    if ($t) { & $script -Action Uninstall -TaskName $name -HostExe $exe }
  }
}
if (Get-ScheduledTask -TaskName 'md-w17-*' -ErrorAction SilentlyContinue) { throw 'Task leaked' }
if (Get-CimInstance Win32_Process | Where-Object ExecutablePath -EQ $exe) { throw 'Process leaked' }
'CLEANUP: zero W17 scheduled tasks or fixture processes'
