param([switch]$Capture, [ValidateSet('Quality', 'Sparse', 'Adaptive')][string]$Sampling = 'Sparse', [switch]$BasicFilter, [switch]$Atmosphere, [switch]$NoSky, [switch]$WorldCache, [string]$BvhDirectory = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
$arguments = @('--path', $projectRoot, '--rendering-driver', 'vulkan', '--max-fps', '60', 'res://scenes/sponza.tscn', '--', '--tg')
$arguments += "--tg-sampling=$(@{Sparse = 0; Quality = 1; Adaptive = 3}[$Sampling])"
if ($BasicFilter) { $arguments += '--tg-basic-filter' }
if ($Atmosphere) { $arguments += '--atmosphere' }
if ($NoSky) { $arguments += '--tg-no-sky' }
if ($WorldCache) { $arguments += '--tg-world-cache' }
if ($BvhDirectory) { $arguments += "--tg-bvh-dir=$((Resolve-Path -LiteralPath $BvhDirectory).Path)" }
if ($Capture) {
    $captureDirectory = Join-Path $projectRoot $(if ($Atmosphere) { 'artifacts/atmosphere/sponza' } else { 'artifacts/tg' })
    New-Item -ItemType Directory -Path $captureDirectory -Force | Out-Null
    $arguments += if ($Atmosphere) { "--atmosphere-capture=$captureDirectory" } else { "--sponza-capture=$captureDirectory" }
    $captureLog = Join-Path $captureDirectory 'run.log'
    & $godotExe @arguments 2>&1 | Tee-Object -FilePath $captureLog
} else {
    & $godotExe @arguments
}
if ($LASTEXITCODE -ne 0) { throw "TG GI failed (exit $LASTEXITCODE)" }
if ($Capture) {
    if (Select-String -LiteralPath $captureLog -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet) { throw "Godot reported errors; see $captureLog" }
    $result = Get-Content -LiteralPath (Join-Path $captureDirectory 'results.json') -Encoding UTF8 -Raw | ConvertFrom-Json
    if (-not $result.passed) { throw 'TG GI validation failed.' }
}
