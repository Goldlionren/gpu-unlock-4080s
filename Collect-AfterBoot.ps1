param(
    [Parameter(Mandatory=$true)][string]$Report,
    [Parameter(Mandatory=$true)][string]$Python
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Report)) { throw 'Report not found. Copy the latest report from USB RUNxx or pass its full USB path.' }
$reportPath = (Resolve-Path -LiteralPath $Report).Path
$reportText = Get-Content -LiteralPath $reportPath -Raw
if ($reportText -notmatch 'TRANSACTION OUTCOME: result=0 .*selectors_before_flr=1 selectors_after_flr=1 recovered=1' -or
    $reportText -notmatch 'FINAL status=0 .*attempted=1 firmware_started=1 restored_entry_command=0' -or
    $reportText -match 'STOP phase=') { throw 'EFI report does not show a completed successful transaction; preserve the report for analysis instead.' }
if (-not (Test-Path -LiteralPath $Python)) { throw 'Original ComfyUI Python environment not found' }
$benchmark = Join-Path $PSScriptRoot 'rtx4080s_tensor_baseline.py'
if (-not (Test-Path -LiteralPath $benchmark)) { throw 'Original benchmark not found' }
$outputDirectory = Join-Path $PSScriptRoot ('results\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $outputDirectory | Out-Null
Copy-Item -LiteralPath $reportPath -Destination (Join-Path $outputDirectory 'REPORT.TXT')
$context = [ordered]@{ state='after_stage3_attempt'; collected=[DateTime]::Now.ToString('o'); report_sha256=(Get-FileHash -LiteralPath $reportPath -Algorithm SHA256).Hash.ToLowerInvariant(); python=$Python; benchmark=$benchmark; benchmark_sha256=(Get-FileHash -LiteralPath $benchmark -Algorithm SHA256).Hash.ToLowerInvariant(); original_benchmark_stock_label_is_not_state=$true; same_boot_requires_user_procedure=$true; performance_verified=$false }
$context | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputDirectory 'context.json') -Encoding utf8
& nvidia-smi --query-gpu=name,pci.device_id,driver_version,vbios_version,memory.total,power.limit --format=csv 2>&1 | Tee-Object -FilePath (Join-Path $outputDirectory 'nvidia-smi.txt')
if ($LASTEXITCODE -ne 0) { throw 'nvidia-smi failed' }
Push-Location (Split-Path $benchmark -Parent)
try {
    for ($trial=1; $trial -le 2; $trial++) {
        $benchmarkOutput = & $Python $benchmark 2>&1
        $benchmarkExit = $LASTEXITCODE
        $benchmarkOutput | Tee-Object -FilePath (Join-Path $outputDirectory ('benchmark-' + $trial + '.log'))
        if ($benchmarkExit -ne 0) { throw ('Benchmark failed on trial ' + $trial) }
        $resultLine = $benchmarkOutput | ForEach-Object { $_.ToString() } | Where-Object { $_ -match '^RESULT FILE: ' } | Select-Object -Last 1
        if (-not $resultLine) { throw 'No benchmark result JSON reported' }
        $resultPath = $resultLine.Substring(13).Trim()
        Copy-Item -LiteralPath $resultPath -Destination (Join-Path $outputDirectory ('benchmark-' + $trial + '.json'))
    }
} finally { Pop-Location }
Write-Output ('Results saved to ' + $outputDirectory)
