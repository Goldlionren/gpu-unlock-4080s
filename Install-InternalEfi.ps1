#Requires -RunAsAdministrator
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PreflightReport, [switch]$Apply)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools\DiskBoot.Common.ps1')
$preflight = Get-Content -LiteralPath (Resolve-Path -LiteralPath $PreflightReport).Path -Raw | ConvertFrom-Json
$esp = @($preflight.partitions | Where-Object { $_.is_system -and $_.gpt_type -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' })
if ($esp.Count -ne 1) { throw 'Expected exactly one captured system ESP.' }
$partition = @(Get-Partition | Where-Object { $_.Guid -eq $esp[0].guid })
if ($partition.Count -ne 1 -or -not $partition[0].IsSystem -or $partition[0].GptType -ne '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}') { throw 'System ESP identity changed.' }
$partition = $partition[0]
$disk = $partition | Get-Disk
if ($disk.PartitionStyle -ne 'GPT' -or $disk.BusType -notin @('NVMe','SATA') -or -not $disk.IsSystem) { throw 'Unsupported system disk.' }
$volume = $partition | Get-Volume
if ($volume.FileSystem -ne 'FAT32' -or $volume.HealthStatus -ne 'Healthy') { throw 'System ESP must be healthy FAT32.' }
if (Confirm-SecureBootUEFI) { throw 'First disk test requires Secure Boot OFF; candidate is unsigned.' }
$bl = Get-BitLockerVolume -MountPoint $env:SystemDrive
if ([int]$bl.ProtectionStatus -ne 0 -or [int]$bl.VolumeStatus -ne 0) { throw 'Windows encryption state changed; inspect BitLocker before proceeding.' }
$package = Join-Path $PSScriptRoot 'Disk\EFI\GPUUnlock'
$manifest = Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json
$testedCandidate = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'evidence\candidate-stage3-0.2.0\manifest.json') -Raw | ConvertFrom-Json
if ($manifest.version -ne 'stage3-0.2.0-disk-candidate' -or $manifest.efi_sha256 -ne $testedCandidate.efi_sha256 -or $manifest.build_id -ne $testedCandidate.build_id) { throw 'Package is not the host-tested disk candidate.' }
$efiHash = (Get-FileHash -LiteralPath (Join-Path $package 'GPUUnlock.efi') -Algorithm SHA256).Hash.ToLowerInvariant()
if ($efiHash -ne $manifest.efi_sha256) { throw 'Candidate EFI hash mismatch.' }
$assetManifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'assets\verified-manifest.json') -Raw | ConvertFrom-Json
if (@($assetManifest.PSObject.Properties).Count -ne 5) { throw 'Expected five pinned assets.' }
foreach ($asset in $assetManifest.PSObject.Properties) {
    if ($asset.Name -notmatch '^[A-Za-z0-9_.-]+\.bin$') { throw 'Invalid asset name.' }
    $file = Get-Item -LiteralPath (Join-Path $package ('ASSETS\' + $asset.Name))
    if ($file.Length -ne $asset.Value.bytes -or (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $asset.Value.sha256) { throw ('Bad asset: ' + $asset.Name) }
}
$items = @(Get-Item -LiteralPath $package) + @(Get-ChildItem -LiteralPath $package -Recurse -Force)
if (@($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Reparse points in package.' }
$files = @($items | Where-Object { -not $_.PSIsContainer })
$bytes = ($files | Measure-Object Length -Sum).Sum
if ($volume.SizeRemaining -lt $bytes + 32MB) { throw 'Insufficient ESP space including log reserve.' }
$plan = [ordered]@{ disk=$disk.Number; disk_name=$disk.FriendlyName; partition=$partition.PartitionNumber; partition_guid=[string]$partition.Guid; free_bytes=$volume.SizeRemaining; package_bytes=$bytes; efi_sha256=$efiHash; destination='\EFI\GPUUnlock'; original_windows_default_preserved=$true; schedules_reboot=$false; schedules_bootnext=$false; changes_secure_boot=$false }
$plan | ConvertTo-Json | Write-Output
if (-not $Apply) { Write-Output 'PLAN ONLY. Add -Apply to copy files and append a firmware entry.'; return }
$stateDir = Join-Path $PSScriptRoot ('LOCAL-disk-install\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $stateDir | Out-Null
$state = [ordered]@{ plan=$plan; phase='preparing'; entry_id=$null; new_files_retained=$false; mounted_letter=$null; error=$null }
$statePath = Join-Path $stateDir 'install-state.json'
function Save-State { $state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $statePath -Encoding utf8 }
function Invoke-Bcd {
    param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Arguments)
    ('bcdedit ' + ($Arguments -join ' ')) | Add-Content -LiteralPath (Join-Path $stateDir 'bcd-commands.log') -Encoding utf8
    $output = & "$env:SystemRoot\System32\bcdedit.exe" @Arguments 2>&1
    $code = $LASTEXITCODE
    $output | Out-File -LiteralPath (Join-Path $stateDir 'bcd-commands.log') -Append -Encoding utf8
    if ($code -ne 0) { throw ('BCDEdit failed (' + $code + '): ' + ($output -join ' ')) }
    return ($output -join [Environment]::NewLine)
}
$mounted = $false
try {
    $originalBootManager = Invoke-Bcd /enum '{bootmgr}' /v
    if ($originalBootManager -notmatch '(?im)^\s*path\s+\\EFI\\Microsoft\\Boot\\bootmgfw\.efi\s*$') { throw 'Unexpected Windows boot path.' }
    $order = @(Get-GpuFirmwareOrder (Invoke-Bcd /enum '{fwbootmgr}' /v))
    $firmwareAll = Invoke-Bcd /enum firmware /v
    if ($firmwareAll -match '(?i)GPUUnlock') { throw 'An existing GPUUnlock entry needs inspection.' }
    $originalBootManager | Set-Content -LiteralPath (Join-Path $stateDir 'bootmgr-before.txt') -Encoding utf8
    $firmwareAll | Set-Content -LiteralPath (Join-Path $stateDir 'firmware-before.txt') -Encoding utf8
    $state.original_order=$order
    Invoke-Bcd /export (Join-Path $stateDir 'BCD-backup') | Out-Null
    Save-State
    $letters = @([IO.Directory]::GetLogicalDrives() | ForEach-Object { $_.Substring(0,1) }) + @(Get-PSDrive -PSProvider FileSystem | Select-Object -ExpandProperty Name)
    $letter = @('S','T','U','V','W','X','Y','Z') | Where-Object { $_ -notin $letters } | Select-Object -First 1
    if (-not $letter) { throw 'No free temporary ESP letter S: through Z:.' }
    $mount = $letter + ':'
    & "$env:SystemRoot\System32\mountvol.exe" $mount /S
    if ($LASTEXITCODE -ne 0) { throw 'Failed to mount system ESP.' }
    $mounted=$true; $state.mounted_letter=$mount; Save-State
    $mountedPartition = Get-Partition -DriveLetter $letter
    if ($mountedPartition.Guid -ne $partition.Guid) { throw 'Mounted ESP identity mismatch; no files copied.' }
    $root = $mount + '\'
    $efiParent = Join-Path $root 'EFI'
    $target = [IO.Path]::GetFullPath((Join-Path $efiParent 'GPUUnlock'))
    if ($target -ne ($root+'EFI\GPUUnlock') -or -not (Test-Path -LiteralPath (Join-Path $efiParent 'Microsoft\Boot\bootmgfw.efi'))) { throw 'Unexpected destination or missing Windows EFI.' }
    if (Test-Path -LiteralPath $target) { throw 'EFI\GPUUnlock already exists; inspect prior state instead of overwriting.' }
    $windowsPath = Join-Path $efiParent 'Microsoft\Boot\bootmgfw.efi'
    $windowsHash = (Get-FileHash -LiteralPath $windowsPath -Algorithm SHA256).Hash
    $state.windows_efi_sha256_before=$windowsHash
    $state.phase='copying'; $state.new_files_retained=$true; Save-State
    Copy-Item -LiteralPath $package -Destination $target -Recurse
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($package.Length+1)
        if ((Get-FileHash -LiteralPath (Join-Path $target $relative) -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash) { throw ('Copy verification failed: ' + $relative) }
    }
    if ((Get-FileHash -LiteralPath $windowsPath -Algorithm SHA256).Hash -ne $windowsHash) { throw 'Original Windows EFI hash changed.' }
    # Compare snapshots under the same mount: BCDEdit changes its displayed
    # device alias from HarddiskVolume to the temporary drive letter.
    $mountedBootManager = Invoke-Bcd /enum '{bootmgr}' /v
    if (($mountedBootManager -replace '(?im)^\s*device[ \t]+[^\r\n]+','').Trim() -ne ($originalBootManager -replace '(?im)^\s*device[ \t]+[^\r\n]+','').Trim()) { throw 'Windows Boot Manager changed beyond the mounted-device alias.' }
    $mountedBootManager | Set-Content -LiteralPath (Join-Path $stateDir 'bootmgr-mounted.txt') -Encoding utf8
    $state.phase='registering'; Save-State
    $id = Register-GpuFirmwareEntry -Bcd { param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Arguments) Invoke-Bcd -Arguments $Arguments } -OriginalOrder $order -OriginalBootManager $mountedBootManager -Device ('partition='+$mount) -OnCreated {
        param($id)
        $state.entry_id=$id; Save-State
    }
    $state.phase='installed'; Save-State
    Invoke-Bcd /enum firmware /v | Set-Content -LiteralPath (Join-Path $stateDir 'firmware-after.txt') -Encoding utf8
    Write-Output ('INSTALLED: ' + $id)
    Write-Output 'Windows remains first. No BootNext or restart requested.'
    Write-Output 'For the first cold boot, select GPUUnlock 4080S (experimental) in the firmware boot menu.'
    Write-Output ('Disable this entry if needed (files retained): bcdedit /delete "' + $id + '"')
} catch {
    $state.error=$_.Exception.Message; $state.phase='failed'; Save-State
    Write-Warning ('Stopped. Copied files retained for inspection. State: ' + $statePath)
    throw
} finally {
    if ($mounted) {
        & "$env:SystemRoot\System32\mountvol.exe" $mount /D
        $state.unmount_exit_code=$LASTEXITCODE
        if ($LASTEXITCODE -ne 0) { Write-Warning ('Could not remove temporary mount ' + $mount) }
    }
    Save-State
    Write-Output ('State: ' + $statePath)
}
