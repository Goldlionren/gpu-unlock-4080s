param()
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    & python tools\fetch_assets.py --check-only
    if ($LASTEXITCODE -ne 0) { throw 'Run python tools\fetch_assets.py first' }
    $manifest = Get-Content evidence\tested-stage3-0.1.2\manifest.json -Raw | ConvertFrom-Json
    $efi = 'artifacts\stage3-0.1.2\BOOTX64.EFI'
    if ((Get-FileHash -LiteralPath $efi -Algorithm SHA256).Hash.ToLowerInvariant() -ne $manifest.efi_sha256) { throw 'Tested EFI hash mismatch' }
    New-Item -ItemType Directory -Path USB\EFI\BOOT,USB\RTX4080S-UNLOCK\ASSETS -Force | Out-Null
    Copy-Item -LiteralPath $efi -Destination USB\EFI\BOOT\BOOTX64.EFI -Force
    $assets = Get-Content assets\verified-manifest.json -Raw | ConvertFrom-Json
    foreach ($asset in $assets.PSObject.Properties) {
        Copy-Item -LiteralPath ('assets\' + $asset.Name) -Destination USB\RTX4080S-UNLOCK\ASSETS -Force
    }
    Copy-Item -LiteralPath evidence\tested-stage3-0.1.2\manifest.json -Destination USB\RTX4080S-UNLOCK\manifest.json -Force
    Copy-Item -LiteralPath assets\verified-manifest.json -Destination USB\RTX4080S-UNLOCK\assets-manifest.json -Force
    Copy-Item -LiteralPath LICENSES -Destination USB\RTX4080S-UNLOCK -Recurse -Force
    Write-Output 'Assembled USB with the hardware-tested EFI. No GPU access or boot changes performed.'
} finally { Pop-Location }
