[CmdletBinding()]
param(
    [string]$Python = 'F:\Comfy-Desktop\ComfyUI-Installs\default\ComfyUI\.venv\Scripts\python.exe'
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    throw 'Python not found. Supply -Python with the original PyTorch environment python.exe.'
}
& $Python (Join-Path $PSScriptRoot 'tools\check_unlock_status.py')
if ($LASTEXITCODE -ne 0) { throw 'Status measurement failed; no unlock state can be inferred. See the saved log.' }
