# Loading this file makes no system changes.
function Get-GpuFirmwareOrder {
    param([string]$Text)
    $guid = '\{[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\}'
    $order = [Collections.Generic.List[string]]::new()
    $inOrder = $false
    foreach ($line in ($Text -split '\r?\n')) {
        if ($line -match '^\s*bootsequence\s+') { throw 'An existing one-time firmware boot sequence is pending; inspect first.' }
        if ($line -match ('^\s*displayorder\s+(' + $guid + ')\s*$')) { $inOrder=$true; $order.Add($Matches[1].ToLowerInvariant()); continue }
        if ($inOrder -and $line -match ('^\s+(' + $guid + ')\s*$')) { $order.Add($Matches[1].ToLowerInvariant()); continue }
        $inOrder=$false
    }
    if (-not $order.Count -or $order[0] -ne '{9dea862c-5cdd-4e70-acc1-f32b344d4795}' -or @($order | Select-Object -Unique).Count -ne $order.Count) {
        throw 'Cannot prove a unique firmware order with Windows first. English BCDEdit output is required.'
    }
    return $order.ToArray()
}
function Register-GpuFirmwareEntry {
    param(
        [Parameter(Mandatory=$true)][scriptblock]$Bcd,
        [Parameter(Mandatory=$true)][string[]]$OriginalOrder,
        [Parameter(Mandatory=$true)][string]$OriginalBootManager,
        [Parameter(Mandatory=$true)][ValidatePattern('^partition=[S-Z]:$')][string]$Device,
        [Parameter(Mandatory=$true)][scriptblock]$OnCreated
    )
    $created = $null
    try {
        $copy = & $Bcd -Arguments @('/copy','{bootmgr}','/d','GPUUnlock 4080S (experimental)')
        $matches = [regex]::Matches(($copy -join [Environment]::NewLine), '\{[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\}')
        if ($matches.Count -ne 1) { throw 'Cannot identify the new entry; inspect the BCDEdit log before retrying.' }
        $created = $matches[0].Value.ToLowerInvariant()
        if ($created -in $OriginalOrder -or $created -eq '{a5a30fa2-3d06-4e9f-b5f4-a01df9d1fcba}') { $created=$null; throw 'Copy returned an existing/system identifier.' }
        & $OnCreated $created
        & $Bcd -Arguments @('/set',$created,'device',$Device) | Out-Null
        & $Bcd -Arguments @('/set',$created,'path','\EFI\GPUUnlock\GPUUnlock.efi') | Out-Null
        & $Bcd -Arguments (@('/set','{fwbootmgr}','displayorder') + $OriginalOrder + @($created)) | Out-Null
        $after = (& $Bcd -Arguments @('/enum','{bootmgr}','/v')) -join [Environment]::NewLine
        if ($after.Trim() -ne $OriginalBootManager.Trim()) { throw 'Original Windows Boot Manager changed unexpectedly.' }
        $order = @(Get-GpuFirmwareOrder ((& $Bcd -Arguments @('/enum','{fwbootmgr}','/v')) -join [Environment]::NewLine))
        if (($order -join ',') -ne (($OriginalOrder + @($created)) -join ',')) { throw 'Firmware boot order readback mismatch.' }
        $entry = (& $Bcd -Arguments @('/enum',$created,'/v')) -join [Environment]::NewLine
        if ($entry -notmatch '(?im)^\s*path\s+\\EFI\\GPUUnlock\\GPUUnlock\.efi\s*$' -or $entry -notmatch [regex]::Escape($created)) { throw 'New EFI path/identifier readback mismatch.' }
        $beforeDevice = [regex]::Match($OriginalBootManager, '(?im)^\s*device[ \t]+([^\r\n]+)').Groups[1].Value.Trim()
        $afterDevice = [regex]::Match($entry, '(?im)^\s*device[ \t]+([^\r\n]+)').Groups[1].Value.Trim()
        if (-not $beforeDevice -or $afterDevice -ne $beforeDevice) { throw 'New entry is not on the original Windows ESP.' }
        $firmware = (& $Bcd -Arguments @('/enum','firmware','/v')) -join [Environment]::NewLine
        if ($firmware -notmatch [regex]::Escape($created)) { throw 'New entry is not enumerated as firmware.' }
        return $created
    } catch {
        $reason = $_.Exception.Message
        if ($created) {
            try { & $Bcd -Arguments @('/delete',$created) | Out-Null } catch { $reason += '; rollback delete failed: ' + $_.Exception.Message }
            try { & $Bcd -Arguments (@('/set','{fwbootmgr}','displayorder') + $OriginalOrder) | Out-Null } catch { $reason += '; rollback order failed: ' + $_.Exception.Message }
        }
        throw $reason
    }
}
