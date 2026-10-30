# SPDX-License-Identifier: GPL-3.0-or-later
# Signs a public preview package for TEST MODE only. Does not install or trust it.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DriverDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{40}$')][string]$CertificateThumbprint
)
$ErrorActionPreference='Stop'
$certificate=Get-Item -LiteralPath "Cert:\CurrentUser\My\$CertificateThumbprint"
if(-not $certificate.HasPrivateKey -or $certificate.NotAfter -le (Get-Date)) { throw 'A valid local signing key is required.' }
$source=(Resolve-Path -LiteralPath $DriverDirectory).Path
if(Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a fresh release output directory.' }
$build=Get-Content -LiteralPath (Join-Path $source 'build.json') -Raw | ConvertFrom-Json
$inf2cat=Join-Path $build.WdkRoot 'bin\10.0.26100.0\x86\Inf2Cat.exe'
$kit=Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Windows Kits\10'
$signtool=Get-ChildItem -Path "$kit\bin\*\x64\signtool.exe" | Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
if(-not $signtool -or -not(Test-Path -LiteralPath $inf2cat)) { throw 'SDK/WDK signing tools are required on the release BUILD machine.' }
$null=New-Item -ItemType Directory -Path $OutputDirectory
$destination=(Resolve-Path -LiteralPath $OutputDirectory).Path
foreach($name in @('opendrop_ax211_km.sys','opendrop_ax211_km.inf','ax211-probe.exe','ax211-stage.exe','radio-check.exe')) {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination $destination
}
& $signtool sign /sha1 $CertificateThumbprint /fd SHA256 "$destination\opendrop_ax211_km.sys"
if($LASTEXITCODE) { throw 'Driver signing failed.' }
& $inf2cat "/driver:$destination" /os:10_X64 /uselocaltime
if($LASTEXITCODE) { throw 'Catalog generation failed.' }
& $signtool sign /sha1 $CertificateThumbprint /fd SHA256 "$destination\opendrop_ax211_km.cat"
if($LASTEXITCODE) { throw 'Catalog signing failed.' }
$null=Export-Certificate -Cert $certificate -FilePath "$destination\release.cer"
$hashes=[ordered]@{}
foreach($name in @('opendrop_ax211_km.sys','opendrop_ax211_km.inf','opendrop_ax211_km.cat','release.cer')) {
    $hashes[$name]=(Get-FileHash -LiteralPath (Join-Path $destination $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
[ordered]@{version=1;signing='test-only';thumbprint=$certificate.Thumbprint;files=$hashes} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$destination\release-driver.json" -Encoding UTF8
. "$PSScriptRoot\..\native\intel\Release-Driver.ps1"
$null=Get-ReleaseDriverMetadata $destination
Write-Output "Built pre-signed TEST MODE package: $destination"
