#Requires -RunAsAdministrator
[CmdletBinding()]
param([string]$Bundle)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools\SecureBoot.Common.ps1')
if(-not $Bundle) {
    $latest=Get-ChildItem -Path (Join-Path $PSScriptRoot 'LOCAL-disk-install\*\install-state.json') -File |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
    if(-not $latest) {throw 'No installation state; specify -Bundle'}
    $install=Get-Content -LiteralPath $latest -Raw | ConvertFrom-Json
    if($install.phase -ne 'installed' -or -not $install.signature_bundle) {throw 'Latest installation is not a completed signed installation'}
    $Bundle=$install.signature_bundle
}
$b=Get-GpuSigningBundle $Bundle
$out=Join-Path $PSScriptRoot ('LOCAL-secureboot-checks\'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
& (Join-Path $PSScriptRoot 'Get-BootPreflight.ps1') -OutputDirectory $out
$capture=Get-Content -LiteralPath (Join-Path $out 'preflight.json') -Raw | ConvertFrom-Json
$read={
    param($Name)
    $entry=@($capture.variables | Where-Object name -eq $Name)
    if($entry.Count -ne 1) {throw ('Missing captured variable '+$Name)}
    if($entry[0].error) {
        if($entry[0].error -match '0xC0000100') {return}
        throw ('Variable read failed: '+$Name+' '+$entry[0].error)
    }
    $path=Join-Path $out ($Name+'.bin')
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry[0].sha256) {throw ('Capture hash mismatch: '+$Name)}
    return [IO.File]::ReadAllBytes($path)
}
$original=@{}
foreach($name in @('PK','KEK','db','dbx')) {$original[$name]=[IO.File]::ReadAllBytes((Join-Path $b.Root ('before\'+$name+'.bin')))}
$state=Get-GpuTrustState -Read $read -Original $original -Addition ([IO.File]::ReadAllBytes((Join-Path $b.Root 'db-append.esl')))
$state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'verification.json') -Encoding utf8
$state | ConvertTo-Json -Depth 5 | Write-Output
Write-Output ('Verification: '+(Join-Path $out 'verification.json'))
Write-Output 'Read-only verification. No key write, boot change or reboot was requested.'
