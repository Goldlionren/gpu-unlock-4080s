$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\tools\DiskBoot.Common.ps1')
$volume='\\?\Volume{11111111-2222-3333-4444-555555555555}\'
$name='Volume{11111111-2222-3333-4444-555555555555}'
$script:checks=0
function Assert($Condition,$Message) { $script:checks++; if (-not $Condition) {throw $Message} }
# Regression: the mounted ESP has no MSFT_Partition DriveLetter. That query
# must never be used, even when the native DOS mapping is valid.
function Get-Partition { throw 'DriveLetter lookup must not be called.' }
$query={
    param($Name)
    $fake.calls.Add($Name)
    if ($Name -like 'Volume*') {
        if ($fake.mode -eq 'expectedFailure') {throw 'Volume mapping query failed'}
        if ($fake.mode -eq 'empty') {return ''}
        if ($fake.mode -eq 'nonVolume') {return '\??\C:\folder'}
        return '\Device\HarddiskVolume1'
    }
    if ($fake.mode -eq 'mountFailure') {throw 'Mount mapping query failed'}
    if ($fake.mode -eq 'wrongVolume') {return '\Device\HarddiskVolume2'}
    if ($fake.mode -eq 'case') {return '\device\harddiskvolume1'}
    return '\Device\HarddiskVolume1'
}
foreach ($mode in @('success','case','expectedFailure','mountFailure','wrongVolume','empty','nonVolume')) {
    $fake=@{mode=$mode;calls=[Collections.Generic.List[string]]::new()}
    $thrown=$false; $result=$null
    try {$result=Assert-GpuEspMount -Mount 'S:' -ExpectedVolumePath $volume -QueryDevice $query} catch {$thrown=$true}
    if ($mode -in @('success','case')) {
        Assert (-not $thrown -and $result.nt_device -eq '\Device\HarddiskVolume1') 'Valid DOS mapping rejected'
        Assert (($fake.calls -join '|') -eq ($name+'|S:')) 'Wrong identity queries'
    } else {Assert $thrown ('Unsafe mapping accepted: '+$mode)}
}
foreach ($bad in @('S:\','{11111111-2222-3333-4444-555555555555}','\\?\Volume{invalid}\','\\?\Volume{11111111-2222-3333-4444-555555555555}\folder')) {
    $fake=@{mode='success';calls=[Collections.Generic.List[string]]::new()}
    $thrown=$false
    try {Assert-GpuEspMount -Mount 'S:' -ExpectedVolumePath $bad -QueryDevice $query | Out-Null} catch {$thrown=$true}
    Assert ($thrown -and $fake.calls.Count -eq 0) 'Malformed volume path queried'
}
Write-Output ("PASS: $script:checks assertions; 11 mount identity scenarios; no mount, file copy or firmware writes.")
