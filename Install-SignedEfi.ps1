#Requires -RunAsAdministrator
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Bundle,[string]$InstallState)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools\DiskBoot.Common.ps1')
. (Join-Path $PSScriptRoot 'tools\SecureBoot.Common.ps1')
$b=Get-GpuSigningBundle $Bundle
if(Confirm-SecureBootUEFI) {throw 'Keep Secure Boot OFF until certificate enrollment has been verified'}
if(-not $InstallState) {
    $InstallState=Get-ChildItem -Path (Join-Path $PSScriptRoot 'LOCAL-disk-install\*\install-state.json') -File |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
$old=Get-Content -LiteralPath $InstallState -Raw | ConvertFrom-Json
if($old.phase -ne 'installed' -or $old.plan.efi_sha256 -ne $b.Record.unsigned_efi_sha256) {throw 'Expected the successful unsigned installation as input'}
$partition=@(Get-Partition | Where-Object {$_.Guid -eq $old.plan.partition_guid -and $_.IsSystem})
if($partition.Count -ne 1) {throw 'Installed ESP identity changed'}
$volume=$partition[0] | Get-Volume -ErrorAction Stop
foreach($name in @('PK','KEK','db','dbx')) {
    [byte[]]$current=(Get-SecureBootUEFI -Name $name -ErrorAction Stop).Bytes
    if(-not (Test-GpuBytesEqual $current ([IO.File]::ReadAllBytes((Join-Path $b.Root ('before\'+$name+'.bin')))))) {throw ('Trust data changed since signing preparation: '+$name)}
}
$dir=Join-Path $PSScriptRoot ('LOCAL-disk-install\'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-signed-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $dir -Force | Out-Null
$state=[ordered]@{phase='preparing-signed-update';parent_install_state=$InstallState;signature_bundle=$b.Path;entry_id=$old.entry_id;plan=$old.plan;error=$null}
$state.plan.PSObject.Properties.Remove('original_windows_default_preserved')
$state.plan | Add-Member -NotePropertyName boot_order_unchanged -NotePropertyValue $true -Force
$statePath=Join-Path $dir 'install-state.json'
function Save-State {$state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $statePath -Encoding utf8}
Save-State
$letters=@([IO.Directory]::GetLogicalDrives() | ForEach-Object {$_.Substring(0,1)})+@(Get-PSDrive -PSProvider FileSystem | Select-Object -ExpandProperty Name)
$letter=@('S','T','U','V','W','X','Y','Z') | Where-Object {$_ -notin $letters} | Select-Object -First 1
if(-not $letter) {throw 'No temporary ESP drive letter available'}
$mount=$letter+':'; $mounted=$false; $replacementStarted=$false
try {
    & "$env:SystemRoot\System32\mountvol.exe" $mount /S
    if($LASTEXITCODE -ne 0) {throw 'ESP mount failed'}
    $mounted=$true
    $state.mount_identity=Assert-GpuEspMount -Mount $mount -ExpectedVolumePath $volume.Path
    $target=Join-Path ($mount+'\') 'EFI\GPUUnlock\GPUUnlock.efi'
    if((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $b.Record.unsigned_efi_sha256) {throw 'Installed EFI differs from the verified unsigned original'}
    $windows=Join-Path ($mount+'\') 'EFI\Microsoft\Boot\bootmgfw.efi'
    $windowsHash=(Get-FileHash -LiteralPath $windows -Algorithm SHA256).Hash
    $backup=Join-Path $dir 'GPUUnlock.unsigned.efi'
    Copy-Item -LiteralPath $target -Destination $backup
    if((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash.ToLowerInvariant() -ne $b.Record.unsigned_efi_sha256) {throw 'Unsigned backup verification failed'}
    $state.phase='replacing-signed-efi'; Save-State
    $replacementStarted=$true
    Copy-Item -LiteralPath $b.SignedEfi -Destination $target -Force
    if((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $b.Record.signed_efi_sha256) {throw 'Signed ESP copy hash mismatch'}
    $null=& (Join-Path $PSScriptRoot 'Verify-EfiSignature.ps1') -SignedEfi $target -UnsignedEfi $backup -CertificateThumbprint $b.Record.certificate_thumbprint
    if((Get-FileHash -LiteralPath $windows -Algorithm SHA256).Hash -ne $windowsHash) {throw 'Original Windows EFI unexpectedly changed'}
    $state.plan.efi_sha256=$b.Record.signed_efi_sha256
    $state.phase='installed'; $state.signed_image_verified=$true; Save-State
    $replacementStarted=$false
    Write-Output ('SIGNED EFI INSTALLED: '+$b.Record.signed_efi_sha256)
    Write-Output 'No boot order or firmware key changed. Secure Boot remains OFF.'
} catch {
    $state.error=$_.Exception.Message
    if($replacementStarted) {
        try {
            Copy-Item -LiteralPath $backup -Destination $target -Force
            if((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $b.Record.unsigned_efi_sha256) {throw 'Rollback hash mismatch'}
            $state.unsigned_restored=$true
        } catch {$state.rollback_error=$_.Exception.Message}
    }
    $state.phase='failed'; Save-State; throw
} finally {
    if($mounted) {
        & "$env:SystemRoot\System32\mountvol.exe" $mount /D
        $state.unmount_exit_code=$LASTEXITCODE
        if($LASTEXITCODE -ne 0) {Write-Warning ('Could not unmount '+$mount)}
    }
    Save-State
    Write-Output ('State: '+$statePath)
}
