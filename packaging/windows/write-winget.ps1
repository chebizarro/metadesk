[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Msi,
    [Parameter(Mandatory)][uri]$InstallerUrl,
    [Parameter(Mandatory)][string]$Output,
    [string]$Version = '0.1.0'
)
$ErrorActionPreference = 'Stop'
if ($InstallerUrl.Scheme -ne 'https') { throw 'HTTPS installer URL required' }
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Three-part version required' }
$Msi = (Resolve-Path $Msi).Path
$installer = New-Object -ComObject WindowsInstaller.Installer
$db = $installer.OpenDatabase($Msi, 0)
$view = $db.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property``='ProductCode'")
$view.Execute()
$productCode = $view.Fetch().StringData(1)
$view.Close()
$hash = (Get-FileHash $Msi -Algorithm SHA256).Hash
New-Item -ItemType Directory -Force $Output | Out-Null
$header = "PackageIdentifier: ChrisDaley.Metadesk`nPackageVersion: $Version"
@"
$header
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.6.0
"@ | Set-Content "$Output\ChrisDaley.Metadesk.yaml" -Encoding UTF8
@"
$header
PackageLocale: en-US
Publisher: Chris Daley
PackageName: Metadesk
License: MIT
ShortDescription: Nostr-native remote desktop host and client
ManifestType: defaultLocale
ManifestVersion: 1.6.0
"@ | Set-Content "$Output\ChrisDaley.Metadesk.locale.en-US.yaml" -Encoding UTF8
@"
$header
InstallerType: wix
Scope: user
MinimumOSVersion: 10.0.22000.0
UpgradeBehavior: install
Commands:
- metadesk-host
- metadesk-client
Installers:
- Architecture: arm64
  InstallerUrl: $($InstallerUrl.AbsoluteUri)
  InstallerSha256: $hash
  ProductCode: '$productCode'
ManifestType: installer
ManifestVersion: 1.6.0
"@ | Set-Content "$Output\ChrisDaley.Metadesk.installer.yaml" -Encoding UTF8
