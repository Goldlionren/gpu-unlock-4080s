#Requires -RunAsAdministrator
[CmdletBinding()]
param([string]$InstallState)
$ErrorActionPreference='Stop'
if (-not $InstallState) {
    $InstallState=Get-ChildItem -Path (Join-Path $PSScriptRoot 'LOCAL-disk-install\*\install-state.json') -File |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $InstallState) { throw 'No installation state found.' }
$state=Get-Content -LiteralPath $InstallState -Raw | ConvertFrom-Json
if ($state.phase -ne 'installed') { throw 'The selected installation did not finish; inspect its state first.' }
$partition=@(Get-Partition | Where-Object { $_.Guid -eq $state.plan.partition_guid -and $_.IsSystem })
if ($partition.Count -ne 1) { throw 'Installed system ESP identity not found.' }
$letters=@([IO.Directory]::GetLogicalDrives() | ForEach-Object {$_.Substring(0,1)}) + @(Get-PSDrive -PSProvider FileSystem | Select-Object -ExpandProperty Name)
$letter=@('S','T','U','V','W','X','Y','Z') | Where-Object {$_ -notin $letters} | Select-Object -First 1
if (-not $letter) { throw 'No free temporary ESP drive letter.' }
$mount=$letter+':'
$mounted=$false
try {
    & "$env:SystemRoot\System32\mountvol.exe" $mount /S
    if ($LASTEXITCODE -ne 0) { throw 'ESP mount failed.' }
    $mounted=$true
    if ((Get-Partition -DriveLetter $letter).Guid -ne $partition[0].Guid) { throw 'Mounted ESP identity mismatch.' }
    $root=Join-Path ($mount+'\') 'EFI\GPUUnlock'
    if ((Get-FileHash -LiteralPath (Join-Path $root 'GPUUnlock.efi') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $state.plan.efi_sha256) { throw 'Installed EFI differs from installation state.' }
    $reports=@(Get-ChildItem -LiteralPath $root -Directory | Where-Object Name -Match '^RUN[0-9]{2}$' |
        Sort-Object Name | ForEach-Object { $p=Join-Path $_.FullName 'REPORT.TXT'; if (Test-Path -LiteralPath $p) {Get-Item -LiteralPath $p} })
    if (-not $reports.Count) { throw 'No disk RUN report exists. The EFI may not have run, or it stopped before opening a log.' }
    $output=Join-Path $PSScriptRoot ('LOCAL-disk-reports\'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    foreach ($report in $reports) {
        $dest=Join-Path $output ($report.Directory.Name+'-REPORT.TXT')
        Copy-Item -LiteralPath $report.FullName -Destination $dest
        if ((Get-FileHash -LiteralPath $report.FullName -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash) { throw 'Report copy hash mismatch.' }
    }
    $latest=Join-Path $output ($reports[-1].Directory.Name+'-REPORT.TXT')
    [ordered]@{
        install_state=$InstallState
        last_windows_boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
        secure_boot=(Confirm-SecureBootUEFI)
        latest_report=$latest
        report_sha256=(Get-FileHash -LiteralPath $latest -Algorithm SHA256).Hash.ToLowerInvariant()
        same_boot_verified=$false
        note='Highest RUN number is copied, not proof of execution during the current boot. No benchmark executed.'
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'context.json') -Encoding utf8
    Write-Output ('Report: '+$latest)
    Write-Output 'All disk REPORT.TXT files copied. No raw PROM, benchmark or firmware change.'
} finally {
    if ($mounted) {
        & "$env:SystemRoot\System32\mountvol.exe" $mount /D
        if ($LASTEXITCODE -ne 0) { Write-Warning ('Could not remove temporary mount '+$mount) }
    }
}
