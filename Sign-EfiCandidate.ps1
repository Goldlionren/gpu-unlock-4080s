[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$CertificateThumbprint,
    [string]$SignToolPath,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$inputEfi = Join-Path $PSScriptRoot 'build\RTX4080S-UNLOCK.EFI'
$manifestPath = Join-Path $PSScriptRoot 'build\manifest.json'
if (-not (Test-Path -LiteralPath $inputEfi) -or -not (Test-Path -LiteralPath $manifestPath)) { throw 'Build and validate the candidate first.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$unsignedHash = (Get-FileHash -LiteralPath $inputEfi -Algorithm SHA256).Hash.ToLowerInvariant()
if ($manifest.version -ne 'stage3-0.2.0-disk-candidate' -or $unsignedHash -ne $manifest.efi_sha256) { throw 'Candidate does not match the build manifest.' }
$cert = Get-Item -LiteralPath ('Cert:\CurrentUser\My\' + $CertificateThumbprint)
if (-not $cert.HasPrivateKey -or $cert.NotAfter -le (Get-Date) -or $cert.NotBefore -gt (Get-Date)) { throw 'A currently valid certificate with a private key is required in CurrentUser\My.' }
if ($cert.PublicKey.Oid.Value -ne '1.2.840.113549.1.1.1') { throw 'Use an RSA certificate for this UEFI signing route.' }
if (-not $SignToolPath) {
    $kits = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Windows Kits\10\bin'
    $SignToolPath = Get-ChildItem -Path (Join-Path $kits '*\x64\signtool.exe') | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $SignToolPath -or -not (Test-Path -LiteralPath $SignToolPath)) { throw 'Windows SDK x64 SignTool not found.' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot ('LOCAL-signed\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory to preserve prior signed results.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$outputEfi = Join-Path $OutputDirectory 'GPUUnlock.efi'
Copy-Item -LiteralPath $inputEfi -Destination $outputEfi
& $SignToolPath sign /fd SHA256 /s My /sha1 $CertificateThumbprint /nph $outputEfi
if ($LASTEXITCODE -ne 0) { throw 'SignTool failed; this output must not be installed.' }
$signature = Get-AuthenticodeSignature -LiteralPath $outputEfi
if (-not $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw 'The signed image does not contain the expected signer certificate.' }
[IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'GPUUnlock-public.cer'), $cert.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
[ordered]@{
    build_id=$manifest.build_id
    unsigned_efi_sha256=$unsignedHash
    signed_efi_sha256=(Get-FileHash -LiteralPath $outputEfi -Algorithm SHA256).Hash.ToLowerInvariant()
    certificate_thumbprint=$cert.Thumbprint
    windows_authenticode_status=[string]$signature.Status
    firmware_db_enrolled=$false
    secure_boot_hardware_tested=$false
    note='A matching signer is not proof of firmware trust. No firmware keys, ESP or boot entries were changed.'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'signing-manifest.json') -Encoding utf8
Write-Output ('Signed candidate: ' + $outputEfi)
Write-Output 'The public certificate still needs an approved firmware db enrollment route. This script did not enroll it.'
