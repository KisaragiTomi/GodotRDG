param([Parameter(Mandatory = $true)][string]$Python)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
$outputRoot = Join-Path $projectRoot 'artifacts/tg-fog-history'
foreach ($variant in @('native', 'separated', 'control')) {
    $outputDirectory = Join-Path $outputRoot $variant
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $arguments = @('--path', $projectRoot, '--rendering-driver', 'vulkan', '--max-fps', '60', '--quit-after', '4000', '--script', (Join-Path $PSScriptRoot 'validate-tg-fog-history.gd'), '--', '--tg', '--atmosphere', "--fog-history-output=$outputDirectory")
    if ($variant -eq 'native') { $arguments += '--tg-native-fog' }
    if ($variant -eq 'control') { $arguments += '--fog-history-control' }
    $runLog = Join-Path $outputDirectory 'run.log'
    & $godotExe @arguments *> $runLog
    if ($LASTEXITCODE -ne 0) { throw "Fog history $variant failed; see $runLog" }
    if (Select-String -LiteralPath $runLog -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet) { throw "Godot reported errors; see $runLog" }
    $result = Get-Content -LiteralPath (Join-Path $outputDirectory 'results.json') -Raw | ConvertFrom-Json
    if (-not $result.passed) { throw "Fog history $variant assertions failed" }
    Write-Output "Captured $variant"
}
& $Python (Join-Path $PSScriptRoot 'compare-tg-fog-history.py') $outputRoot
if ($LASTEXITCODE -ne 0) { throw 'Fog history comparison failed.' }
