function Get-GpuSigningBundle {
    param([Parameter(Mandatory=$true)][string]$Path)
    $Path=(Resolve-Path -LiteralPath $Path).Path
    $root=Split-Path $Path -Parent
    $record=Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($record.schema -ne 1 -or $record.phase -ne 'signed-and-prepared-only') {throw 'Unsupported signing bundle'}
    foreach($file in $record.file_sha256.PSObject.Properties) {
        $full=[IO.Path]::GetFullPath((Join-Path $root $file.Name))
        if(-not $full.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)) {throw 'Bundle path escapes its directory'}
        if((Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Value) {throw ('Bundle file changed: '+$file.Name)}
    }
    foreach($required in @('before/PK.bin','before/KEK.bin','before/db.bin','before/dbx.bin','signed/GPUUnlock.efi','signed/GPUUnlock-public.cer','db-append.esl','db-expected-merged.esl')) {
        if(-not $record.file_sha256.PSObject.Properties[$required]) {throw ('Required bundle file missing: '+$required)}
    }
    $repo=Split-Path $PSScriptRoot -Parent
    $original=Join-Path $repo 'build\RTX4080S-UNLOCK.EFI'
    $tested=Get-Content -LiteralPath (Join-Path $repo 'evidence\disk-boot-stage3-0.2.0-run00\verification.json') -Raw | ConvertFrom-Json
    if($record.unsigned_efi_sha256 -ne $tested.efi_sha256 -or (Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash.ToLowerInvariant() -ne $tested.efi_sha256) {throw 'Not the hardware-tested unsigned EFI'}
    $signed=Join-Path $root 'signed\GPUUnlock.efi'
    if((Get-FileHash -LiteralPath $signed -Algorithm SHA256).Hash.ToLowerInvariant() -ne $record.signed_efi_sha256) {throw 'Signed EFI hash mismatch'}
    $null=& (Join-Path $repo 'Verify-EfiSignature.ps1') -SignedEfi $signed -UnsignedEfi $original -CertificateThumbprint $record.certificate_thumbprint
    $certificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new([IO.File]::ReadAllBytes((Join-Path $root 'signed\GPUUnlock-public.cer')))
    try {if($certificate.Thumbprint -ne $record.certificate_thumbprint) {throw 'Bundle certificate mismatch'}} finally {$certificate.Dispose()}
    [pscustomobject]@{Path=$Path;Root=$root;Record=$record;SignedEfi=$signed}
}
function Test-GpuBytesEqual {
    param([byte[]]$Left,[byte[]]$Right)
    return [Convert]::ToBase64String($Left) -eq [Convert]::ToBase64String($Right)
}
function Get-GpuTrustState {
    param([scriptblock]$Read,[hashtable]$Original,[byte[]]$Addition)
    foreach($name in @('PK','KEK','db','dbx')) {if(-not $Original[$name].Length) {throw ('Empty baseline '+$name)}}
    if(-not $Addition.Length) {throw 'Empty db addition'}
    [byte[]]$setup=@(& $Read 'SetupMode'); [byte[]]$secure=@(& $Read 'SecureBoot')
    if($setup.Length -ne 1 -or $secure.Length -ne 1 -or $setup[0] -gt 1 -or $secure[0] -gt 1) {throw 'Invalid SecureBoot/SetupMode readback'}
    $expected=@{PK=$Original.PK;KEK=$Original.KEK;dbx=$Original.dbx;db=[byte[]]($Original.db+$Addition)}
    $matches=[ordered]@{}; $all=$true
    foreach($name in @('PK','KEK','db','dbx')) {
        $matches[$name]=Test-GpuBytesEqual ([byte[]]@(& $Read $name)) $expected[$name]
        if(-not $matches[$name]) {$all=$false}
    }
    $status=if(-not $all) {'KEY_CONTENT_MISMATCH'}
        elseif($setup[0] -eq 1 -and $secure[0] -eq 1) {'INCONSISTENT_FIRMWARE_MODE'}
        elseif($setup[0] -eq 1) {'KEYS_VERIFIED_MODE_PENDING'}
        elseif($secure[0] -eq 1) {'SECURE_BOOT_ENABLED_KEYS_VERIFIED'}
        else {'KEYS_VERIFIED_USER_MODE_SECURE_BOOT_OFF'}
    [pscustomobject]@{status=$status;all_keys_match=$all;matches=[pscustomobject]$matches;setup_mode=[int]$setup[0];secure_boot=[int]$secure[0];secure_boot_efi_execution_verified=$false}
}
function Invoke-GpuTrustEnrollment {
    param([scriptblock]$Read,[scriptblock]$Write,[hashtable]$Original,[byte[]]$Addition,[switch]$Apply)
    [byte[]]$setup=@(& $Read 'SetupMode'); [byte[]]$secure=@(& $Read 'SecureBoot')
    if($setup.Length -ne 1 -or $secure.Length -ne 1 -or $secure[0] -ne 0) {throw 'Expected readable variables and Secure Boot OFF'}
    $existing=Get-GpuTrustState -Read $Read -Original $Original -Addition $Addition
    if($existing.all_keys_match) {
        if($existing.setup_mode -eq 1) {return 'KEYS_VERIFIED_MODE_PENDING: all four key databases match; no write needed. Restart with Secure Boot OFF, then run Check-SecureBootEnrollment.ps1.'}
        return 'ENROLLED: all four key databases already match, SetupMode=0; no write needed'
    }
    if($setup[0] -ne 1) {
        if(-not $Apply) {return 'WAITING_FOR_FIRMWARE_SETUP_MODE: no firmware write attempted'}
        throw 'SetupMode must be 1. No key write attempted; authorize Setup Mode in firmware first.'
    }
    foreach($name in @('PK','KEK','db','dbx')) {if(-not $Original[$name].Length) {throw ('Empty baseline '+$name)}}
    if(-not $Addition.Length) {throw 'Empty db addition'}
    $desired=@{PK=$Original.PK;KEK=$Original.KEK;dbx=$Original.dbx;db=[byte[]]($Original.db+$Addition)}
    $current=@{}
    foreach($name in @('PK','KEK','db','dbx')) {
        [byte[]]$current[$name]=@(& $Read $name)
        if($name -eq 'PK') {
            if($current[$name].Length) {throw 'Setup Mode with a nonempty PK is unexpected'}
        } elseif($current[$name].Length -and
            -not (Test-GpuBytesEqual $current[$name] $Original[$name]) -and
            -not (Test-GpuBytesEqual $current[$name] $desired[$name])) {
            throw ('Unexpected current '+$name+' differs from backup; preserve and inspect it instead of overwriting')
        }
    }
    if(-not $Apply) {return 'READY: restore missing dbx/KEK, add certificate to db, restore original Intel PK last'}
    foreach($name in @('dbx','KEK','db','PK')) {
        # Recheck before every write so a concurrent update cannot silently vanish.
        [byte[]]$now=@(& $Read $name)
        if(-not (Test-GpuBytesEqual $now $current[$name])) {throw ('Concurrent firmware change: '+$name)}
        if(Test-GpuBytesEqual $now $desired[$name]) {continue}
        if($name -eq 'PK') {
            foreach($dependency in @('dbx','KEK','db')) {
                if(-not (Test-GpuBytesEqual ([byte[]]@(& $Read $dependency)) $desired[$dependency])) {throw ('Refuse PK restore: '+$dependency+' not verified')}
            }
        }
        $append=($name -eq 'db' -and $now.Length -gt 0)
        [byte[]]$content=if($append) {$Addition} else {$desired[$name]}
        & $Write -Name $name -Content $content -Append $append | Out-Null
        if(-not (Test-GpuBytesEqual ([byte[]]@(& $Read $name)) $desired[$name])) {throw ('Firmware write readback mismatch: '+$name+'; stopped, do not enable Secure Boot')}
    }
    $final=Get-GpuTrustState -Read $Read -Original $Original -Addition $Addition
    if(-not $final.all_keys_match -or $final.secure_boot -ne 0) {throw 'Final trust/mode readback changed; keep Secure Boot OFF and inspect'}
    if($final.setup_mode -eq 1) {return 'KEYS_VERIFIED_MODE_PENDING: all four key databases match; User Mode not observed. Restart with Secure Boot OFF, then run Check-SecureBootEnrollment.ps1. Do not repeat key writes.'}
    return 'ENROLLED: baseline PK/KEK/dbx verified, baseline db plus one signing certificate verified, SetupMode=0'
}
