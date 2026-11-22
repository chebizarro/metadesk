[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Stage,
    [Parameter(Mandatory)][string]$Output,
    [string]$Version = '0.1.0',
    [string]$Wix = 'wix',
    [string]$Python = 'python',
    [switch]$Sign,
    [string]$CertificateThumbprint,
    [string]$TimestampUrl,
    [string]$SignTool = 'signtool'
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Use a three-part MSI version' }
$parsedVersion = [version]$Version
if ($parsedVersion.Major -gt 255 -or $parsedVersion.Minor -gt 255 -or $parsedVersion.Build -gt 65535) { throw 'Version exceeds MSI field limits' }
if (Test-Path $Output) { throw 'Output directory must be new' }
$Stage = (Resolve-Path $Stage).Path
$Output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
if ($Output.StartsWith($Stage.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Output must be outside stage' }
foreach ($required in 'metadesk-host.exe','metadesk-client.exe','LICENSE','THIRD_PARTY_NOTICES.md','LGPL-SOURCE-OFFER.md','ffmpeg-corresponding-source.tar.gz','runtime-manifest.json') {
    if (!(Test-Path (Join-Path $Stage $required) -PathType Leaf)) { throw "Incomplete stage: $required" }
}
$manifestPath = Join-Path $Stage 'runtime-manifest.json'
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$peFiles = @(Get-ChildItem $Stage -File -Recurse | Where-Object { $_.Extension -in '.exe','.dll' })
if ($peFiles.Count -ne @($manifest.files.PSObject.Properties).Count) { throw 'PE inventory count mismatch' }
foreach ($pe in $peFiles) {
    $relative = $pe.FullName.Substring($Stage.Length + 1)
    $entry = $manifest.files.PSObject.Properties[$relative.ToLowerInvariant()]
    if (!$entry -or (Get-FileHash $pe.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value.sha256) { throw "Uninventoried or modified PE: $relative" }
}
New-Item -ItemType Directory $Output | Out-Null
if ($Sign) {
    if (!$CertificateThumbprint -or !$TimestampUrl) { throw 'Signing requires certificate thumbprint and RFC3161 timestamp URL' }
    # Sign the shipped PE files first; the manifest records the final payload hashes.
    Get-ChildItem $Stage -File | Where-Object { $_.Extension -in '.exe','.dll' } | ForEach-Object {
        & $SignTool sign /sha1 $CertificateThumbprint /s My /fd SHA256 /tr $TimestampUrl /td SHA256 $_.FullName
        if ($LASTEXITCODE) { throw "PE signing failed: $($_.Name)" }
    }
    $manifestPath = Join-Path $Stage 'runtime-manifest.json'
    $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
    $manifest.files.PSObject.Properties | ForEach-Object {
        $_.Value.sha256 = (Get-FileHash (Join-Path $Stage $_.Name) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $manifest | ConvertTo-Json -Depth 20 | Set-Content $manifestPath -Encoding UTF8
}
& $Python "$PSScriptRoot\harvest.py" $Stage "$Output\Payload.wxs"
if ($LASTEXITCODE) { throw 'Harvest failed' }
& $Wix build -arch arm64 -d "Version=$Version" "$PSScriptRoot\Metadesk.wxs" "$Output\Payload.wxs" -o "$Output\metadesk-$Version-arm64.msi"
if ($LASTEXITCODE) { throw 'WiX build failed' }
$msi = "$Output\metadesk-$Version-arm64.msi"
& $Wix msi validate $msi *> "$Output\msi-validation.log"
if ($LASTEXITCODE) { throw "MSI ICE validation failed; see $Output\msi-validation.log" }
Write-Host 'MSI ICE validation passed; validation warnings retained in output log.'
if ($Sign) {
    & $SignTool sign /sha1 $CertificateThumbprint /s My /fd SHA256 /tr $TimestampUrl /td SHA256 $msi
    if ($LASTEXITCODE) { throw 'MSI signing failed' }
    & $SignTool verify /pa /all /v $msi
    if ($LASTEXITCODE) { throw 'MSI signature verification failed' }
} else {
    Write-Host 'DRY RUN: unsigned payload and MSI; no signing store accessed.'
}
Get-FileHash $msi -Algorithm SHA256
