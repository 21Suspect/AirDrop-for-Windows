# SPDX-License-Identifier: GPL-3.0-or-later
# Validates a prebuilt TEST-signed package. Never changes machine trust.
function Get-ReleaseDriverMetadata([string]$Directory) {
    $names=@('opendrop_ax211_km.sys','opendrop_ax211_km.inf','opendrop_ax211_km.cat','release.cer')
    $metadata=Get-Content -LiteralPath (Join-Path $Directory 'release-driver.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if($metadata.version -ne 1 -or $metadata.signing -ne 'test-only' -or
       $metadata.thumbprint -notmatch '^[A-Fa-f0-9]{40}$') { throw 'Invalid release driver manifest.' }
    if(@($metadata.files.PSObject.Properties).Count -ne $names.Count) { throw 'Invalid release file list.' }
    foreach($name in $names) {
        $expected=$metadata.files.$name
        if($expected -notmatch '^[A-Fa-f0-9]{64}$') { throw "Missing release hash: $name" }
        if((Get-FileHash -LiteralPath (Join-Path $Directory $name) -Algorithm SHA256).Hash -ne $expected) {
            throw "Release driver verification failed: $name"
        }
    }
    $cert=New-Object Security.Cryptography.X509Certificates.X509Certificate2(
        (Join-Path $Directory 'release.cer'))
    if($cert.HasPrivateKey -or $cert.Thumbprint -ne $metadata.thumbprint -or
       $cert.NotBefore.ToUniversalTime() -gt [DateTime]::UtcNow -or
       $cert.NotAfter.ToUniversalTime() -le [DateTime]::UtcNow) {
        throw 'Release test certificate is invalid or expired. Download a newer preview.'
    }
    foreach($name in @('opendrop_ax211_km.sys','opendrop_ax211_km.cat')) {
        $signature=Get-AuthenticodeSignature -LiteralPath (Join-Path $Directory $name)
        # Before importing the public test certificate the chain may be untrusted.
        # After import, Test-IntelDriver requires Status=Valid for both files.
        if(-not $signature.SignerCertificate -or
           $signature.SignerCertificate.Thumbprint -ne $cert.Thumbprint -or
           $signature.Status -notin @('Valid','NotTrusted','UnknownError')) {
            throw "Unexpected or damaged release signature: $name"
        }
    }
    return $metadata
}
