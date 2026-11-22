[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Msi,
    [Parameter(Mandatory)][string]$EvidenceDirectory,
    [string]$UpgradeMsi,
    [string]$InstallDirectory = "$env:LOCALAPPDATA\Programs\Metadesk-W10-Test"
)
$ErrorActionPreference = 'Stop'
if (Test-Path $InstallDirectory) { throw 'Test install directory already exists; refusing to take ownership' }
$menu = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Metadesk"
if (Test-Path $menu) { throw 'Existing Metadesk Start menu; use a private test user' }
if (Test-Path HKCU:\Software\Metadesk.Installer) { throw 'Existing Metadesk installation; use a private test user' }
$before = [string][Environment]::GetEnvironmentVariable('Path','User')
$originalProcessPath = $env:PATH
New-Item -ItemType Directory -Force $EvidenceDirectory | Out-Null
$EvidenceDirectory = (Resolve-Path $EvidenceDirectory).Path
$Msi = (Resolve-Path $Msi).Path
if ($UpgradeMsi) { $UpgradeMsi = (Resolve-Path $UpgradeMsi).Path }
$installer = New-Object -ComObject WindowsInstaller.Installer
$products = @()
foreach ($package in @($Msi, $UpgradeMsi) | Where-Object { $_ }) {
    $db = $installer.OpenDatabase($package, 0)
    $view = $db.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property``='ProductCode'")
    $view.Execute()
    $code = $view.Fetch().StringData(1)
    $view.Close()
    if ($installer.ProductState($code) -ne -1) { throw "Product already registered: $code" }
    $products += $code
}
$failure = $null
$cleanupErrors = @()
function MsiExec([string[]]$Arguments) {
    $p = Start-Process msiexec.exe -ArgumentList $Arguments -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "msiexec exit $($p.ExitCode)" }
}
try {
    MsiExec @('/i', "`"$Msi`"", '/qn', '/norestart', "INSTALLDIR=`"$InstallDirectory`"", '/l*v', "`"$EvidenceDirectory\install.log`"")
    if ($UpgradeMsi) {
        MsiExec @('/i', "`"$UpgradeMsi`"", '/qn', '/norestart', "INSTALLDIR=`"$InstallDirectory`"", '/l*v', "`"$EvidenceDirectory\upgrade.log`"")
    }
    foreach ($name in 'Metadesk Host.lnk','Metadesk Client.lnk') {
        if (!(Test-Path "$menu\$name")) { throw "Missing shortcut $name" }
    }
    $userPath = [Environment]::GetEnvironmentVariable('Path','User')
    if (!($userPath.TrimEnd(';').Split(';').TrimEnd('\') -contains $InstallDirectory.TrimEnd('\'))) { throw 'User PATH entry missing' }
    # SSH and existing Explorer shells retain old environment blocks. Reconstruct a new
    # shell from persisted machine/user environment, without any build-tree DLL lookup.
    $env:PATH = (([Environment]::GetEnvironmentVariable('Path','Machine') + ';' + $userPath).Split(';') | Where-Object { $_ -notmatch '(?i)msys|md-w10|build-product' }) -join ';'
    foreach ($cli in 'metadesk-host','metadesk-client') {
        $resolved = (& "$env:SystemRoot\System32\where.exe" $cli) | Select-Object -First 1
        if ($resolved -ne "$InstallDirectory\$cli.exe") { throw "Wrong CLI resolved: $resolved" }
        # PowerShell 5.1 treats redirected native stderr as ErrorRecords even
        # for exit 0. Capture both native streams directly; judge the exit code.
        $p = Start-Process "$env:SystemRoot\System32\cmd.exe" -ArgumentList @('/d','/c',"$cli --help") -WorkingDirectory $EvidenceDirectory -RedirectStandardOutput "$EvidenceDirectory\$cli-help.txt" -RedirectStandardError "$EvidenceDirectory\$cli-help-stderr.txt" -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw "$cli --help failed: $($p.ExitCode)" }
        "$cli --help: exit 0 from fresh cmd, resolved $resolved"
    }
    $manifest = Get-Content "$InstallDirectory\runtime-manifest.json" -Raw | ConvertFrom-Json
    $manifest.files.PSObject.Properties | ForEach-Object {
        if ((Get-FileHash "$InstallDirectory\$($_.Name)" -Algorithm SHA256).Hash.ToLowerInvariant() -ne $_.Value.sha256) { throw "Installed hash mismatch: $($_.Name)" }
    }
} catch {
    $failure = $_
} finally {
    $env:PATH = $originalProcessPath
    # A failed major upgrade can roll back to the previous product. Check both
    # identities owned by this run; do not mask the original install/smoke error.
    foreach ($code in $products) {
        try {
            if ($installer.ProductState($code) -ne -1) {
                MsiExec @('/x', $code, '/qn', '/norestart', '/l*v', "`"$EvidenceDirectory\uninstall-$code.log`"")
            }
        } catch { $cleanupErrors += $_ }
    }
}
if ($cleanupErrors.Count) { throw "Lifecycle error: $failure; cleanup errors: $cleanupErrors" }
if (Test-Path $InstallDirectory) { throw 'Install directory remains' }
if (Test-Path $menu) { throw 'Start menu remains' }
if (Test-Path HKCU:\Software\Metadesk.Installer) { throw 'Installer registry remains' }
$after = [string][Environment]::GetEnvironmentVariable('Path','User')
if ($before.TrimEnd(';') -ne $after.TrimEnd(';')) { throw "User PATH not restored: before=$before after=$after" }
foreach ($code in $products) {
    if ($installer.ProductState($code) -ne -1) { throw "Product registration remains: $code" }
}
'PASS: files, install directory, shortcuts, installer registry, product registration and user PATH removed/restored'
if ($failure) { throw $failure }
