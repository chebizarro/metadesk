[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Stage,
    [Parameter(Mandatory)][string]$Output,
    [Parameter(Mandatory)][string]$Wix,
    [Parameter(Mandatory)][string]$Python,
    [Parameter(Mandatory)][string]$SignTool,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [switch]$Run
)
$ErrorActionPreference = 'Stop'
if (!$Run) { throw 'Explicit -Run required: creates and temporarily trusts a test certificate in CurrentUser only' }
$Stage = (Resolve-Path $Stage).Path
$cert = $null
$trusted = $false
$certificateFile = $null
try {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Metadesk W10 THROWAWAY NOT FOR RELEASE' -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddDays(1)
    $rootPath = "Cert:\CurrentUser\Root\$($cert.Thumbprint)"
    if (Test-Path $rootPath) { throw 'Unexpected pre-existing trust entry' }
    $certificateFile = Join-Path (Split-Path ([IO.Path]::GetFullPath($Stage)) -Parent) ('w10-test-' + [guid]::NewGuid().ToString() + '.cer')
    Export-Certificate -Cert $cert -FilePath $certificateFile -Type CERT | Out-Null
    # Import only public certificate bytes, explicitly into the user store.
    # certutil is the Windows CLI for this operation; no machine store/policy writes.
    $trusted = $true
    & "$env:SystemRoot\System32\certutil.exe" -user -f -addstore Root $certificateFile
    if ($LASTEXITCODE -or !(Test-Path $rootPath)) { throw 'CurrentUser Root import failed; interactive authorization may be required' }
    & "$PSScriptRoot\build-msi.ps1" -Stage $Stage -Output $Output -Wix $Wix -Python $Python -Sign -CertificateThumbprint $cert.Thumbprint -TimestampUrl $TimestampUrl -SignTool $SignTool
    if (!$?) { throw 'Signed build failed' }
    "Test certificate thumbprint: $($cert.Thumbprint)"
} finally {
    if ($cert) {
        $cleanupErrors = @()
        try { if ($trusted -and (Test-Path "Cert:\CurrentUser\Root\$($cert.Thumbprint)")) { Remove-Item "Cert:\CurrentUser\Root\$($cert.Thumbprint)" -Force } } catch { $cleanupErrors += $_ }
        try { Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey -Force } catch { $cleanupErrors += $_ }
        if ((Test-Path "Cert:\CurrentUser\My\$($cert.Thumbprint)") -or (Test-Path "Cert:\CurrentUser\Root\$($cert.Thumbprint)")) { $cleanupErrors += 'Certificate store entry remains' }
        if ($certificateFile) { try { Remove-Item $certificateFile -Force } catch { $cleanupErrors += $_ } }
        if ($cleanupErrors.Count) { throw "Certificate cleanup failed: $cleanupErrors" }
        "Removed test certificate, private key and temporary trust: $($cert.Thumbprint)"
    }
}
