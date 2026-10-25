$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Add-Type -Path (Join-Path $root 'tools\EfiSignature.cs') -ReferencedAssemblies System.dll,System.Security.dll
$signed=[IO.File]::ReadAllBytes((Join-Path $env:SystemRoot 'Boot\EFI\bootmgfw.efi'))
$good=[GpuUnlock.EfiSignature]::Verify($signed,'')
$opt=[BitConverter]::ToInt32($signed,60)+24
$cert=[BitConverter]::ToInt32($signed,$opt+144)
$size=[BitConverter]::ToInt32($signed,$opt+148)
[byte[]]$unsigned=$signed[0..($cert-1)]
for($i=$opt+144;$i -lt $opt+152;$i++) {$unsigned[$i]=0}
[GpuUnlock.EfiSignature]::Unchanged($unsigned,$signed)
$checks=2
foreach($mode in @('wrongSigner','codeTamper','signatureTamper','truncated','badOffset','alreadySigned')) {
    [byte[]]$bad=$signed.Clone()
    $thrown=$false
    try {
        switch($mode) {
            'wrongSigner' {[GpuUnlock.EfiSignature]::Verify($bad,('0'*40)) | Out-Null}
            'codeTamper' {$bad[2048]=$bad[2048] -bxor 1; [GpuUnlock.EfiSignature]::Verify($bad,'') | Out-Null}
            'signatureTamper' {$bad[$cert+64]=$bad[$cert+64] -bxor 1; [GpuUnlock.EfiSignature]::Verify($bad,'') | Out-Null}
            'truncated' {[GpuUnlock.EfiSignature]::Verify([byte[]]$bad[0..($bad.Length-2)],'') | Out-Null}
            'badOffset' {$bad[$opt+144]=$bad[$opt+144] -bxor 1; [GpuUnlock.EfiSignature]::Verify($bad,'') | Out-Null}
            'alreadySigned' {[GpuUnlock.EfiSignature]::Unchanged($signed,$signed)}
        }
    } catch {$thrown=$true}
    if(-not $thrown){throw ('Accepted bad signature/image: '+$mode)}
    $checks++
}
# A valid Microsoft CMS copied onto our EFI must not authenticate that EFI.
$candidate=[IO.File]::ReadAllBytes((Join-Path $root 'build\RTX4080S-UNLOCK.EFI'))
$candidateOpt=[BitConverter]::ToInt32($candidate,60)+24
[byte[]]$forged=$candidate+$signed[$cert..($signed.Length-1)]
[BitConverter]::GetBytes([int]$candidate.Length).CopyTo($forged,$candidateOpt+144)
[BitConverter]::GetBytes([int]$size).CopyTo($forged,$candidateOpt+148)
$thrown=$false
try {[GpuUnlock.EfiSignature]::Verify($forged,$good.Thumbprint) | Out-Null} catch {$thrown=$true}
if(-not $thrown){throw 'Accepted unrelated valid CMS attached to candidate'}
$checks++
Write-Output ("PASS: $checks signature checks using Microsoft-signed EFI and corrupt/unrelated variants; no private keys or trust changes.")
