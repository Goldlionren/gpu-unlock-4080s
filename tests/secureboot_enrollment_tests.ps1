$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\tools\SecureBoot.Common.ps1')
$base=@{PK=[byte[]](1,2);KEK=[byte[]](3,4);db=[byte[]](5,6);dbx=[byte[]](7,8)}
$addition=[byte[]](9,10)
$checks=0
function Check($Value,$Message){$script:checks++;if(-not $Value){throw $Message}}
$read={param($Name)
    if($Name -eq 'db' -and $fake.mode -eq 'concurrent' -and $fake.writes.Count -gt 0){return [byte[]](99)}
    return $fake.vars[$Name]
}
$write={param($Name,[byte[]]$Content,[bool]$Append)
    $fake.writes.Add($Name)
    if($fake.mode -eq 'writeFailure' -and $Name -eq 'db'){throw 'Injected firmware write failure'}
    if($fake.mode -eq 'badReadback' -and $Name -eq 'db'){$fake.vars[$Name]=[byte[]](99);return}
    $fake.vars[$Name]=if($Append){[byte[]]($fake.vars[$Name]+$Content)}else{$Content}
    if($Name -eq 'PK'){$fake.vars.SetupMode=[byte[]](0)}
}
foreach($mode in @('preserved','empty','resumed','userMode','secureOn','changedDbx','writeFailure','badReadback','concurrent','preview')){
    $fake=@{mode=$mode;writes=[Collections.Generic.List[string]]::new();vars=@{SetupMode=[byte[]](1);SecureBoot=[byte[]](0);PK=[byte[]]@();KEK=$base.KEK;db=$base.db;dbx=$base.dbx}}
    if($mode -in @('empty','concurrent')){foreach($name in @('KEK','db','dbx')){$fake.vars[$name]=[byte[]]@()}}
    if($mode -eq 'resumed'){$fake.vars.db=[byte[]]($base.db+$addition)}
    if($mode -eq 'userMode'){$fake.vars.SetupMode=[byte[]](0);$fake.vars.PK=$base.PK}
    if($mode -eq 'secureOn'){$fake.vars.SecureBoot=[byte[]](1)}
    if($mode -eq 'changedDbx'){$fake.vars.dbx=[byte[]](99)}
    $thrown=$false
    try {$result=Invoke-GpuTrustEnrollment -Read $read -Write $write -Original $base -Addition $addition -Apply:($mode -ne 'preview')}
    catch {$thrown=$true}
    if($mode -in @('preserved','empty','resumed')){
        Check (-not $thrown) ('Failed successful scenario '+$mode)
        Check ($fake.writes[-1] -eq 'PK') 'PK was not last'
        foreach($name in @('PK','KEK','dbx')){Check (Test-GpuBytesEqual $fake.vars[$name] $base[$name]) ('Baseline changed '+$name)}
        Check (Test-GpuBytesEqual $fake.vars.db ([byte[]]($base.db+$addition))) 'Unexpected db content'
    } elseif($mode -eq 'preview'){Check (-not $thrown -and $fake.writes.Count -eq 0) 'Preview wrote firmware'}
    else {
        Check $thrown ('Unsafe enrollment accepted '+$mode)
        Check ('PK' -notin $fake.writes) 'PK restored despite a failed dependency'
        if($mode -in @('userMode','secureOn','changedDbx')){Check ($fake.writes.Count -eq 0) 'Write before prerequisite checks'}
    }
}
Write-Output ("PASS: $checks checks across 10 trust-enrollment scenarios; all firmware operations mocked.")
