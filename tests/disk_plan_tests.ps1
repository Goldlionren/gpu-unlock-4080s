# Runs the real plan path against mocked disk/firmware cmdlets and local package.
# Never passes -Apply. Removing #Requires here affects only this test scriptblock.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$scratch=Join-Path $repo ('LOCAL-tests\disk-plan-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
$guid='{11111111-2222-3333-4444-555555555555}'
$fixture=Join-Path $scratch 'preflight.json'
@{partitions=@(@{is_system=$true;gpt_type='{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}';guid=$guid})} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $fixture -Encoding utf8
$script:case='success'
function Get-Partition {
    [pscustomobject]@{Guid=$(if($script:case -eq 'identity') {'different'} else {$guid}); IsSystem=$true; GptType='{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'; PartitionNumber=1}
}
function Get-Disk {
    param([Parameter(ValueFromPipeline=$true)]$InputObject)
    process { [pscustomobject]@{PartitionStyle='GPT';BusType=$(if($script:case -eq 'transport') {'USB'} else {'NVMe'});IsSystem=$true;Number=0;FriendlyName='Mock NVMe'} }
}
function Get-Volume {
    param([Parameter(ValueFromPipeline=$true)]$InputObject)
    process { [pscustomobject]@{FileSystem='FAT32';HealthStatus='Healthy';SizeRemaining=$(if($script:case -eq 'space') {1} else {2GB})} }
}
function Confirm-SecureBootUEFI { return ($script:case -eq 'secure') }
function Get-BitLockerVolume {
    param($MountPoint)
    [pscustomobject]@{ProtectionStatus=$(if($script:case -eq 'bitlocker') {1} else {0});VolumeStatus=0}
}
$source=[IO.File]::ReadAllText((Join-Path $repo 'Install-InternalEfi.ps1'))
$source=$source -replace '(?m)^#Requires -RunAsAdministrator\r?\n',''
$source=$source.Replace('$PSScriptRoot',("'"+$repo.Replace("'","''")+"'"))
$entry=[scriptblock]::Create($source)
foreach ($name in @('success','identity','transport','space','secure','bitlocker')) {
    $script:case=$name; $failed=$false; $output=$null
    try { $output=& $entry -PreflightReport $fixture } catch { $failed=$true }
    if ($name -eq 'success') {
        if ($failed -or ($output -join '') -notmatch 'PLAN ONLY') { throw 'Plan with real pinned package did not succeed.' }
    } elseif (-not $failed) { throw ('Unsafe plan accepted: '+$name) }
}
Write-Output 'PASS: 6 real plan-path scenarios with mocked system state and real package hashes; no ESP/NVRAM access.'
