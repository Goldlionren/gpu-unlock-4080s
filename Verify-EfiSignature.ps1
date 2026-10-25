[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SignedEfi,
    [Parameter(Mandatory=$true)][string]$UnsignedEfi,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$CertificateThumbprint
)
$ErrorActionPreference='Stop'
if (-not ('GpuUnlock.EfiSignature' -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot 'tools\EfiSignature.cs') -ReferencedAssemblies System.dll,System.Security.dll
}
$original=[IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $UnsignedEfi).Path)
$signed=[IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $SignedEfi).Path)
[GpuUnlock.EfiSignature]::Unchanged($original,$signed)
$result=[GpuUnlock.EfiSignature]::Verify($signed,$CertificateThumbprint)
[pscustomobject]@{
    signer_thumbprint=$result.Thumbprint
    authenticode_image_sha256=$result.ImageDigestSha256
    cms_signature_verified=$true
    executable_bytes_unchanged=$true
    firmware_trust_verified=$false
}
