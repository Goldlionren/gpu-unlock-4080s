#Requires -RunAsAdministrator
[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if ($identity.Name -match '(?i)CodexSandbox') { throw 'Run this in your normal Admin account; do not create the persistent signing key in a sandbox account.' }
if (-not $OutputDirectory) {
    $OutputDirectory=Join-Path $PSScriptRoot ('LOCAL-secureboot\'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$capture=Join-Path $OutputDirectory 'before'
& (Join-Path $PSScriptRoot 'Get-BootPreflight.ps1') -OutputDirectory $capture
$preflight=Get-Content -LiteralPath (Join-Path $capture 'preflight.json') -Raw | ConvertFrom-Json
if ($preflight.errors.Count -or $preflight.secure_boot -ne $false) { throw 'Fresh preflight failed or Secure Boot is enabled. No signing key created.' }
foreach ($name in @('PK','KEK','db','dbx','SecureBoot','SetupMode')) {
    $v=@($preflight.variables | Where-Object name -eq $name)
    if ($v.Count -ne 1 -or $v[0].error -or $v[0].bytes -lt 1) { throw ('Incomplete firmware backup: '+$name) }
    if ((Get-FileHash -LiteralPath (Join-Path $capture ($name+'.bin')) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $v[0].sha256) { throw ('Backup hash mismatch: '+$name) }
}
if ([IO.File]::ReadAllBytes((Join-Path $capture 'SetupMode.bin'))[0] -ne 0) { throw 'This preparation step expects the original User Mode with PK still installed. Do not change BIOS keys yet.' }
& (Join-Path $PSScriptRoot 'Inspect-SecureBootDb.ps1') -CaptureDirectory $capture
$hardware=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'evidence\disk-boot-stage3-0.2.0-run00\verification.json') -Raw | ConvertFrom-Json
$binary=Join-Path $PSScriptRoot 'build\RTX4080S-UNLOCK.EFI'
if ((Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hardware.efi_sha256) { throw 'Unsigned candidate is not the hardware-tested disk EFI.' }
$keyRecord=Join-Path $PSScriptRoot 'LOCAL-signing-key.json'
$subject='CN=GPUUnlock 4080S Local UEFI Signing'
if (Test-Path -LiteralPath $keyRecord) {
    $record=Get-Content -LiteralPath $keyRecord -Raw | ConvertFrom-Json
    if ($record.user_sid -ne $identity.User.Value) { throw 'Existing signing key record belongs to another Windows account.' }
    $cert=Get-Item -LiteralPath ('Cert:\CurrentUser\My\'+$record.thumbprint)
} else {
    $existing=@(Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $subject -and $_.HasPrivateKey })
    if ($existing.Count -gt 1) { throw 'Multiple project signing keys exist. Select one explicitly before proceeding.' }
    if ($existing.Count -eq 1) { $cert=$existing[0] }
    else {
        $cert=New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -FriendlyName 'GPUUnlock 4080S UEFI - local signing only' -CertStoreLocation 'Cert:\CurrentUser\My' -Provider 'Microsoft Software Key Storage Provider' -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(10)
    }
    [ordered]@{user_sid=$identity.User.Value;account=$identity.Name;thumbprint=$cert.Thumbprint;store='CurrentUser\My';private_key_exported=$false} |
        ConvertTo-Json | Set-Content -LiteralPath $keyRecord -Encoding utf8
}
if ($cert.Subject -ne $subject -or -not $cert.HasPrivateKey -or $cert.NotAfter -le (Get-Date)) { throw 'Existing signing certificate is unsuitable.' }
$signing=Join-Path $OutputDirectory 'signed'
& (Join-Path $PSScriptRoot 'Sign-EfiCandidate.ps1') -CertificateThumbprint $cert.Thumbprint -OutputDirectory $signing
$signedManifest=Get-Content -LiteralPath (Join-Path $signing 'signing-manifest.json') -Raw | ConvertFrom-Json
if (-not $signedManifest.cryptographic_verification.cms_signature_verified -or -not $signedManifest.cryptographic_verification.executable_bytes_unchanged) { throw 'Signature verification did not complete.' }
$owner=[Guid]::NewGuid()
$timestamp=[DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$append=Join-Path $OutputDirectory 'db-append.esl'
$formatted=Format-SecureBootUEFI -Name db -SignatureOwner $owner -CertificateFilePath (Join-Path $signing 'GPUUnlock-public.cer') -FormatWithCert -AppendWrite -Time $timestamp -ContentFilePath $append -SignableFilePath (Join-Path $OutputDirectory 'db-signable.bin')
if (-not $formatted.AppendWrite -or $formatted.Name -ne 'db') { throw 'Unexpected formatting result.' }
[byte[]]$originalDb=[IO.File]::ReadAllBytes((Join-Path $capture 'db.bin'))
[byte[]]$addition=[IO.File]::ReadAllBytes($append)
if ($addition.Length -lt 44) { throw 'Empty/invalid db append content.' }
$public=[IO.File]::ReadAllBytes((Join-Path $signing 'GPUUnlock-public.cer'))
# FormatWithCert must contain exactly one X.509 entry holding our public cert.
if ([Guid]::new([byte[]]$addition[0..15]).ToString() -ne 'a5c059a1-94e4-4aa7-87b5-ab155c2bf072' -or
    [BitConverter]::ToUInt32($addition,16) -ne $addition.Length -or
    [BitConverter]::ToUInt32($addition,20) -ne 0 -or
    [BitConverter]::ToUInt32($addition,24) -ne 16+$public.Length -or $addition.Length -ne 44+$public.Length -or
    [Convert]::ToBase64String($addition[44..($addition.Length-1)]) -ne [Convert]::ToBase64String($public)) { throw 'Formatted db entry differs from the signing certificate.' }
[IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'db-expected-merged.esl'),[byte[]]($originalDb+$addition))
$files=[ordered]@{}
Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File | Sort-Object FullName | ForEach-Object {
    $relative=$_.FullName.Substring($OutputDirectory.Length+1).Replace('\','/')
    $files[$relative]=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
[ordered]@{
    schema=1
    phase='signed-and-prepared-only'
    account=$identity.Name
    certificate_thumbprint=$cert.Thumbprint
    certificate_store='CurrentUser\My'
    private_key_exported=$false
    signature_owner=$owner.ToString()
    unsigned_efi_sha256=$signedManifest.unsigned_efi_sha256
    signed_efi_sha256=$signedManifest.signed_efi_sha256
    cryptographic_verification=$signedManifest.cryptographic_verification
    file_sha256=$files
    esp_modified=$false
    firmware_keys_modified=$false
    reboot_requested=$false
    secure_boot_enabled_tested=$false
    proposed_enrollment='Install signed byte-equivalent EFI with Secure Boot off; enter firmware-authorized Setup Mode; preserve/restore exact current KEK/db/dbx, add only the new certificate to db, and restore original Intel PK last; verify before enabling Secure Boot.'
    next_step='Review this bundle before changing BIOS keys. No automatic enrollment command is executed by this script.'
} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'bundle.json') -Encoding utf8
Write-Output ('PREPARED: '+(Join-Path $OutputDirectory 'bundle.json'))
Write-Output 'Signed copy, public certificate, fresh public trust backup and planned db append are ready.'
Write-Output 'ESP, boot entries and firmware keys are unchanged. Keep Secure Boot OFF for now.'
