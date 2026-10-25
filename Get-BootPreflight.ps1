#Requires -RunAsAdministrator
[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $PSScriptRoot ('LOCAL-boot-preflight\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$report = [ordered]@{
    schema = 1
    utc = [DateTime]::UtcNow.ToString('o')
    purpose = 'Read-only disk, firmware and public Secure Boot variable inspection'
    changes_boot_configuration = $false
    errors = [Collections.Generic.List[string]]::new()
}
try { $report.bios = Get-CimInstance Win32_BIOS | Select-Object Manufacturer, SMBIOSBIOSVersion, ReleaseDate }
catch { $report.errors.Add('BIOS: ' + $_.Exception.Message) }
try { $report.disks = @(Get-Disk | Select-Object Number, FriendlyName, BusType, PartitionStyle, Size, IsBoot, IsSystem, OperationalStatus) }
catch { $report.errors.Add('Disk: ' + $_.Exception.Message) }
try {
    $report.partitions = @(Get-Partition | ForEach-Object {
        $p = $_
        $entry = [ordered]@{ disk=$p.DiskNumber; partition=$p.PartitionNumber; drive_letter=[string]$p.DriveLetter; size=$p.Size; gpt_type=[string]$p.GptType; guid=[string]$p.Guid; is_system=$p.IsSystem; is_boot=$p.IsBoot; access_paths=@($p.AccessPaths) }
        try { $entry.volumes = @($p | Get-Volume -ErrorAction Stop | Select-Object FileSystem, Size, SizeRemaining, Path, HealthStatus) }
        catch { $entry.volume_error = $_.Exception.Message }
        [pscustomobject]$entry
    })
} catch { $report.errors.Add('Partition: ' + $_.Exception.Message) }
try { $report.secure_boot = Confirm-SecureBootUEFI }
catch { $report.errors.Add('SecureBoot: ' + $_.Exception.Message) }
$report.variables = @(
    foreach ($name in @('SecureBoot','SetupMode','PK','KEK','db','dbx','PKDefault','KEKDefault','dbDefault','dbxDefault')) {
        try {
            $v = Get-SecureBootUEFI -Name $name -ErrorAction Stop
            $dest = Join-Path $OutputDirectory ($name + '.bin')
            [IO.File]::WriteAllBytes($dest, [byte[]]$v.Bytes)
            [pscustomobject]@{ name=$name; bytes=$v.Bytes.Length; attributes=[string]$v.Attributes; sha256=(Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash.ToLowerInvariant(); file=($name + '.bin') }
        } catch { [pscustomobject]@{ name=$name; error=$_.Exception.Message } }
    }
)
try {
    $report.bitlocker = @(Get-BitLockerVolume | Select-Object MountPoint, VolumeType, VolumeStatus, ProtectionStatus, LockStatus, EncryptionMethod)
} catch { $report.errors.Add('BitLocker status: ' + $_.Exception.Message) }
foreach ($entry in @(@('firmware','firmware'),@('bootmgr','{bootmgr}'))) {
    try {
        $text = & "$env:SystemRoot\System32\bcdedit.exe" /enum $entry[1] /v 2>&1
        $exitCode = $LASTEXITCODE
        $text | Out-File -LiteralPath (Join-Path $OutputDirectory ('bcd-' + $entry[0] + '.txt')) -Encoding utf8
        $report[('bcd_' + $entry[0] + '_exit_code')] = $exitCode
    } catch { $report.errors.Add('BCD ' + $entry[0] + ': ' + $_.Exception.Message) }
}
$package = Join-Path $PSScriptRoot 'Disk\EFI\GPUUnlock'
if (Test-Path -LiteralPath $package) {
    $report.candidate_package_bytes = (Get-ChildItem -LiteralPath $package -File -Recurse | Measure-Object -Property Length -Sum).Sum
    $report.suggested_free_bytes = $report.candidate_package_bytes + 32MB
}
$report.notes = @(
    'No ESP mounted, partition changed, NVRAM written or certificate enrolled.',
    'Exported PK/KEK/db/dbx are public firmware trust data, not private signing keys.',
    'Raw variable backups alone are NOT guaranteed to be valid authenticated restore packages.',
    'A missing ESP volume/free-space result requires a separate read-only inspection before installation.'
)
$dest = Join-Path $OutputDirectory 'preflight.json'
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $dest -Encoding utf8
Write-Output ('Report: ' + $dest)
Write-Output 'This script changed no boot settings. Keep this LOCAL folder outside public Git commits.'
