$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\tools\DiskBoot.Common.ps1')
$script:checks=0
function Assert($Condition, $Message) { $script:checks++; if (-not $Condition) { throw $Message } }
$win='{9dea862c-5cdd-4e70-acc1-f32b344d4795}'
$usb='{f0abd367-d040-11f1-9bc4-806e6f6e6963}'
$new='{12345678-1234-1234-1234-123456789abc}'
$original=@($win,$usb)
$nl=[Environment]::NewLine
$bootmgr="identifier $win$($nl)device partition=S:$($nl)path \EFI\Microsoft\Boot\bootmgfw.efi$($nl)description Windows Boot Manager"
function FirmwareText($Order) { return ('identifier {a5a30fa2-3d06-4e9f-b5f4-a01df9d1fcba}'+$nl+'displayorder '+($Order -join ($nl+'    '))+$nl+'timeout 1') }
Assert ((@(Get-GpuFirmwareOrder (FirmwareText $original)) -join ',') -eq ($original -join ',')) 'Parse order'
foreach ($bad in @((FirmwareText @($usb,$win)),(FirmwareText @($win,$win)),('displayorder not-a-guid'),((FirmwareText $original)+$nl+'bootsequence '+$usb))) {
    $thrown=$false; try { Get-GpuFirmwareOrder $bad | Out-Null } catch { $thrown=$true }
    Assert $thrown 'Bad/pending order accepted'
}
$runner = {
    param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Arguments)
    $fake.calls.Add(($Arguments -join '|'))
    $fake.count++
    if ($fake.fail -eq $fake.count) { throw 'Injected native command failure' }
    if ($Arguments[0] -eq '/copy') { return ('Copied to '+$(if ($fake.badCopy) {$win} else {$new})) }
    if ($Arguments[0] -eq '/delete') {
        Assert ($Arguments[1] -eq $new) 'Attempt to delete an original boot entry'
        $fake.deleted=$true; return 'OK'
    }
    if ($Arguments[0] -eq '/set') {
        if ($Arguments[2] -eq 'displayorder') { $fake.order=@($Arguments[3..($Arguments.Count-1)]) }
        Assert ($Arguments[1] -eq $new -or ($Arguments[1] -eq '{fwbootmgr}' -and $Arguments[2] -eq 'displayorder')) 'Unexpected mutation'
        return 'OK'
    }
    if ($Arguments[0] -eq '/enum') {
        switch ($Arguments[1]) {
            '{bootmgr}' { return $(if ($fake.badBootmgr) {$bootmgr+'changed'} else {$bootmgr}) }
            '{fwbootmgr}' { return (FirmwareText $(if ($fake.badOrder) {@($new,$win,$usb)} else {$fake.order})) }
            'firmware' { return $(if ($fake.missingFirmware) {$win} else {$win+$nl+$new}) }
            default {
                Assert ($Arguments[1] -eq $new) 'Unexpected enum target'
                $device=if ($fake.badDevice) {'partition=T:'} else {'partition=S:'}
                $path=if ($fake.badPath) {'\wrong.efi'} else {'\EFI\GPUUnlock\GPUUnlock.efi'}
                return ("identifier $new$($nl)device $device$($nl)path $path")
            }
        }
    }
    throw 'Unexpected command'
}
foreach ($scenario in @('success','badCopy','badBootmgr','badOrder','badPath','badDevice','missingFirmware','fail1','fail2','fail3','fail4','fail5','fail6','fail7','fail8','callbackFail')) {
    $fake=@{ calls=[Collections.Generic.List[string]]::new(); count=0; fail=0; order=$original; deleted=$false; saved=$null }
    if ($scenario -match '^fail(\d+)$') {$fake.fail=[int]$Matches[1]} else {$fake[$scenario]=$true}
    $thrown=$false; $result=$null
    try {
        $result=Register-GpuFirmwareEntry -Bcd $runner -OriginalOrder $original -OriginalBootManager $bootmgr -Device 'partition=S:' -OnCreated {
            param($id)
            if ($fake.callbackFail) {throw 'Injected state persistence failure'}
            $fake.saved=$id
        }
    } catch { $thrown=$true; if ($scenario -eq 'success') { Write-Output $_.Exception.Message; $fake.calls | Write-Output } }
    if ($scenario -eq 'success') {
        Assert (-not $thrown -and $result -eq $new -and $fake.saved -eq $new) 'Registration success failed'
        Assert (-not $fake.deleted -and ($fake.order -join ',') -eq (($original+@($new)) -join ',')) 'Order not preserved'
    } else {
        Assert $thrown ('Failure accepted: '+$scenario)
        if ($scenario -in @('fail1','badCopy')) { Assert (-not $fake.deleted) 'Deleted without a new identifier' }
        else {
            Assert $fake.deleted ('New entry not rolled back: '+$scenario)
            Assert (($fake.order -join ',') -eq ($original -join ',')) 'Original order not restored'
        }
    }
    Assert (-not @($fake.calls | Where-Object { $_ -match 'bootsequence|/delete\|\{9dea|/set\|\{bootmgr\}' }).Count) 'Original boot manager or BootNext mutated'
}
Write-Output ("PASS: $script:checks assertions, 16 registration scenarios; mocked BCDEdit only, no ESP/NVRAM writes.")
