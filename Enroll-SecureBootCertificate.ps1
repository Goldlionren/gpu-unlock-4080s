#Requires -RunAsAdministrator
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Bundle,[switch]$Apply)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools\DiskBoot.Common.ps1')
. (Join-Path $PSScriptRoot 'tools\SecureBoot.Common.ps1')
$b=Get-GpuSigningBundle $Bundle
$original=@{}
foreach($name in @('PK','KEK','db','dbx')) {$original[$name]=[IO.File]::ReadAllBytes((Join-Path $b.Root ('before\'+$name+'.bin')))}
$addition=[IO.File]::ReadAllBytes((Join-Path $b.Root 'db-append.esl'))
if(-not (Test-GpuBytesEqual ([byte[]]($original.db+$addition)) ([IO.File]::ReadAllBytes((Join-Path $b.Root 'db-expected-merged.esl'))))) {throw 'Expected db is not exactly original db plus certificate'}
$latest=Get-ChildItem -Path (Join-Path $PSScriptRoot 'LOCAL-disk-install\*\install-state.json') -File |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
$state=Get-Content -LiteralPath $latest -Raw | ConvertFrom-Json
if($state.phase -ne 'installed' -or -not $state.signed_image_verified -or $state.plan.efi_sha256 -ne $b.Record.signed_efi_sha256) {throw 'Install the verified signed EFI before changing firmware keys'}
$partition=@(Get-Partition | Where-Object {$_.Guid -eq $state.plan.partition_guid -and $_.IsSystem})
if($partition.Count -ne 1) {throw 'Signed system ESP identity changed'}
$volume=$partition[0] | Get-Volume -ErrorAction Stop
$letters=@([IO.Directory]::GetLogicalDrives() | ForEach-Object {$_.Substring(0,1)})+@(Get-PSDrive -PSProvider FileSystem | Select-Object -ExpandProperty Name)
$letter=@('S','T','U','V','W','X','Y','Z') | Where-Object {$_ -notin $letters} | Select-Object -First 1
if(-not $letter) {throw 'No temporary ESP letter available'}
$mount=$letter+':';$mounted=$false
try {
    & "$env:SystemRoot\System32\mountvol.exe" $mount /S
    if($LASTEXITCODE -ne 0){throw 'ESP mount failed'}
    $mounted=$true
    $null=Assert-GpuEspMount -Mount $mount -ExpectedVolumePath $volume.Path
    $efi=Join-Path ($mount+'\') 'EFI\GPUUnlock\GPUUnlock.efi'
    if((Get-FileHash -LiteralPath $efi -Algorithm SHA256).Hash.ToLowerInvariant() -ne $b.Record.signed_efi_sha256){throw 'Actual installed signed EFI differs from bundle'}
} finally {
    if($mounted){
        & "$env:SystemRoot\System32\mountvol.exe" $mount /D
        if($LASTEXITCODE -ne 0){throw 'Could not remove temporary ESP mount; enrollment not started'}
    }
}
$run=Join-Path $b.Root ('enrollment-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $run | Out-Null
$read={
    param($Name)
    try {return (Get-SecureBootUEFI -Name $Name -ErrorAction Stop).Bytes}
    catch {
        # Observed exact undefined-variable status. Permission and other read
        # failures must never be treated as empty trust stores.
        if($_.FullyQualifiedErrorId -like 'GetFWVarFailed,*' -and $_.Exception.Message -match '0xC0000100') {return}
        throw
    }
}
$write={
    param([string]$Name,[byte[]]$Content,[bool]$Append)
    if($Name -notin @('PK','KEK','db','dbx') -or -not $Content.Length){throw 'Invalid trust write'}
    $sha=[Security.Cryptography.SHA256]::Create()
    try {$hash=[BitConverter]::ToString($sha.ComputeHash($Content)).Replace('-','').ToLowerInvariant()} finally {$sha.Dispose()}
    [ordered]@{name=$Name;bytes=$Content.Length;append=$Append;sha256=$hash;utc=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $run 'write-intents.jsonl') -Encoding utf8
    Set-SecureBootUEFI -Name $Name -Content $Content -Time ([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')) -AppendWrite:$Append -ErrorAction Stop | Out-Null
}
try {
    $result=Invoke-GpuTrustEnrollment -Read $read -Write $write -Original $original -Addition $addition -Apply:$Apply
    [ordered]@{result=$result;apply_requested=[bool]$Apply;bundle=$b.Path;secure_boot_enabled_tested=$false} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'result.json') -Encoding utf8
    Write-Output $result
    if($Apply){
        & (Join-Path $PSScriptRoot 'Get-BootPreflight.ps1') -OutputDirectory (Join-Path $run 'after')
        & (Join-Path $PSScriptRoot 'Inspect-SecureBootDb.ps1') -CaptureDirectory (Join-Path $run 'after')
    }
} catch {
    $_.Exception.Message | Set-Content -LiteralPath (Join-Path $run 'ERROR.txt') -Encoding utf8
    Write-Warning 'Stopped. Do not enable Secure Boot. No keys are deleted as automatic rollback.'
    throw
} finally {Write-Output ('Enrollment record: '+$run)}
