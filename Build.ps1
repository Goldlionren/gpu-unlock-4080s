param([switch]$CompileOnly, [string]$VcVarsPath, [string]$PromPath)
if (-not $VcVarsPath) {
    $vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($install) { $VcVarsPath = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat' }
    }
}
if ($PromPath) {
    $PromPath = (Resolve-Path -LiteralPath $PromPath -ErrorAction Stop).Path
    if ($PromPath -match '["&|<>%^!\r\n]') { throw 'Unsupported PROM path characters' }
    if ((Get-FileHash -LiteralPath $PromPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'dd5a21fc1434583d3660de35f60799c2cce3b72cd5330dd1d7e5b042a0785f6f') { throw 'Core fixture requires the exact recorded PROM capture' }
}
$ErrorActionPreference = 'Stop'
if (-not $VcVarsPath -or -not (Test-Path -LiteralPath $VcVarsPath)) { throw 'MSVC 2022 missing' }
if ($PSScriptRoot -match '["&|<>%^!\r\n]') { throw 'Unsupported path characters' }
New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot 'build') -Force | Out-Null
$sourceHashes = [ordered]@{}
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'src') -Recurse -File | Sort-Object FullName | ForEach-Object {
    $relativeName = $_.FullName.Substring($PSScriptRoot.Length + 1).Replace('\','/')
    $sourceHashes[$relativeName] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$identity = (($sourceHashes.GetEnumerator() | ForEach-Object { $_.Name + '=' + $_.Value }) -join "`n")
$identity += "`nbuild=" + (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
$identity += "`nheaders=" + (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'vendor\edk2\headers.json') -Algorithm SHA256).Hash
$hasher = [Security.Cryptography.SHA256]::Create()
try { $buildId = [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity))).Replace('-','').ToLowerInvariant() } finally { $hasher.Dispose() }
Set-Content -LiteralPath (Join-Path $PSScriptRoot 'build\build_id.h') -Value ('#define UNLOCK_BUILD_ID "' + $buildId + '"') -Encoding ascii
$sources = @('app','app_io','core','admission','format','sha256','checks','port','compat','vendor\unlock_610','vendor\prep_610','vendor\layout_610','vendor\vbios_610','vendor\gfw_eval')
$commonFlags = '/nologo /std:c11 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /D__KERNEL__ /DGU_UEFI_PORT /DGU_TARGET_PCI_ID=0x270210deU /Isrc\compat /Ivendor\edk2\Include /Ivendor\edk2\Include\X64'
$commands = [Collections.Generic.List[string]]::new()
$commands.Add('@echo off')
$commands.Add('call "' + $vcVarsPath + '" >nul')
$commands.Add('if errorlevel 1 exit /b 1')
$commands.Add('cd /d "' + $PSScriptRoot + '"')
$objects = @()
foreach ($source in $sources) {
    $object = 'build\efi-' + $source.Replace('\','-') + '.obj'
    $objects += $object
    $commands.Add('cl ' + $commonFlags + ' /O1 /Oi /GS- /Gs65536 /Zl /c /FIbuild_id.h /Ibuild /Fo' + $object + ' src\' + $source + '.c')
    $commands.Add('if errorlevel 1 exit /b 1')
}
$commands.Add('cl ' + $commonFlags + ' /Od /Oi- /GS- /Zl /c /Fobuild\efi-runtime.obj src\runtime.c')
$commands.Add('if errorlevel 1 exit /b 1')
$commands.Add('link /nologo /subsystem:efi_application /entry:EfiMain /machine:x64 /nodefaultlib /incremental:no /fixed:no /dynamicbase:no /nxcompat:no /out:build\RTX4080S-UNLOCK.EFI ' + ($objects -join ' ') + ' build\efi-runtime.obj')
$commands.Add('if errorlevel 1 exit /b 1')
if (-not $CompileOnly) {
    $commands.Add('cl ' + $commonFlags + ' tests\port_tests.c src\port.c /Febuild\port-tests.exe /Fobuild\')
    $commands.Add('if errorlevel 1 exit /b 1')
    $commands.Add('build\port-tests.exe > build\port-results.log 2>&1')
    $commands.Add('if errorlevel 1 exit /b 1')
    $commands.Add('cl ' + $commonFlags + ' tests\core_transaction_tests.c src\port.c src\compat.c src\vendor\unlock_610.c src\vendor\prep_610.c src\vendor\layout_610.c src\vendor\vbios_610.c src\vendor\gfw_eval.c /Febuild\core-tests.exe /Fobuild\')
    $commands.Add('if errorlevel 1 exit /b 1')
    if ($PromPath) {
        $commands.Add('build\core-tests.exe "' + $PromPath + '" assets > build\core-results.log 2>&1')
    } else {
        Set-Content -LiteralPath (Join-Path $PSScriptRoot 'build\core-results.log') -Value 'SKIPPED: proprietary board PROM fixture not supplied; core binary compiled only.' -Encoding ascii
    }
    $commands.Add('if errorlevel 1 exit /b 1')
    $commands.Add('cl ' + $commonFlags + ' /DGU_APP_HOST_TEST /FIbuild_id.h /Ibuild tests\app_tests.c src\core.c /Febuild\app-tests.exe /Fobuild\')
    $commands.Add('if errorlevel 1 exit /b 1')
    $commands.Add('build\app-tests.exe > build\app-results.log 2>&1')
    $commands.Add('if errorlevel 1 exit /b 1')
    $commands.Add('cl ' + $commonFlags + ' tests\admission_tests.c src\admission.c src\core.c /Febuild\admission-tests.exe /Fobuild\')
    $commands.Add('if errorlevel 1 exit /b 1')
    $commands.Add('build\admission-tests.exe > build\admission-results.log 2>&1')
    $commands.Add('if errorlevel 1 exit /b 1')
}
Set-Content -LiteralPath (Join-Path $PSScriptRoot 'build\compile.cmd') -Value $commands -Encoding ascii
Push-Location $PSScriptRoot
try {
    & $env:ComSpec /d /c 'build\compile.cmd'
    if ($LASTEXITCODE -ne 0) { throw 'Compiler or host test failure; see build logs' }
    if ($CompileOnly) { Write-Output 'Compiled only; not packaged or hardware validated.'; return }
    & python tools\verify_build.py
    if ($LASTEXITCODE -ne 0) { throw 'Artifact/source/asset verification failure' }
    $binary = Join-Path $PSScriptRoot 'build\RTX4080S-UNLOCK.EFI'
    $manifest = [ordered]@{ version='stage3-0.1.2-experimental'; build_id=$buildId; efi_sha256=(Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant(); source_sha256=$sourceHashes; hardware_tested=$false; performance_validated=$false; unlock_implemented=$true; real_gpu_executed_by_build=$false; tests='port and transaction faults; app flow mocks; PE/assets/source checks'; target='10de:2702/19da:4702/AD103/16GB'; source_reference='51df9c17ec89eb8486df52cc9079dde4bd1f9974'; utc=[DateTime]::UtcNow.ToString('o') }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath build\manifest.json -Encoding utf8
    New-Item -ItemType Directory -Path USB\EFI\BOOT,USB\RTX4080S-UNLOCK\ASSETS -Force | Out-Null
    Copy-Item -LiteralPath $binary -Destination USB\EFI\BOOT\BOOTX64.EFI -Force
    $assets = Get-Content -LiteralPath assets\verified-manifest.json -Raw | ConvertFrom-Json
    foreach ($asset in $assets.PSObject.Properties) { Copy-Item -LiteralPath ('assets\' + $asset.Name) -Destination USB\RTX4080S-UNLOCK\ASSETS -Force }
    Copy-Item -LiteralPath build\manifest.json -Destination USB\RTX4080S-UNLOCK\manifest.json -Force
    Copy-Item -LiteralPath LICENSES -Destination USB\RTX4080S-UNLOCK -Recurse -Force
    Copy-Item -LiteralPath assets\verified-manifest.json -Destination USB\RTX4080S-UNLOCK\assets-manifest.json -Force
    Write-Output ('Validated experimental EFI: ' + $binary)
} finally { Pop-Location }
