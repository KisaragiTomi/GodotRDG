param([Parameter(Mandatory = $true)][string]$Python)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
$outputRoot = Join-Path $projectRoot 'artifacts/tg-adaptive'
foreach ($variant in @('sparse', 'uniform', 'adaptive', 'reference')) {
    $profile = @{sparse = 0; uniform = 3; adaptive = 3; reference = 2}[$variant]
    $outputDirectory = Join-Path $outputRoot $variant
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $arguments = @('--path', $projectRoot, '--rendering-driver', 'vulkan', '--max-fps', '60', 'res://scenes/sponza.tscn', '--', '--tg', "--tg-sampling=$profile", "--tg-camera-capture=$outputDirectory")
    if ($variant -eq 'uniform') { $arguments += '--tg-uniform-budget' }
    $runLog = Join-Path $outputDirectory 'run.log'
    & $godotExe @arguments *> $runLog
    if ($LASTEXITCODE -ne 0) { throw "TG adaptive $variant failed (exit $LASTEXITCODE); see $runLog" }
    if (Select-String -LiteralPath $runLog -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet) { throw "Godot reported errors; see $runLog" }
    Write-Output "Captured $variant"
}
& $Python (Join-Path $PSScriptRoot 'compare-tg-adaptive.py') $outputRoot
if ($LASTEXITCODE -ne 0) { throw 'TG adaptive comparison failed.' }
foreach ($test in @('runtime', 'regression', 'fog')) {
    $outputDirectory = Join-Path $outputRoot $test
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $arguments = @('--path', $projectRoot, '--rendering-driver', 'vulkan', '--max-fps', '60')
    if ($test -eq 'runtime') {
        $arguments += @('--script', (Join-Path $PSScriptRoot 'validate-tg-adaptive-runtime.gd'), '--', '--tg')
    } else {
        $arguments += @('res://scenes/sponza.tscn', '--', '--tg', '--tg-sampling=3')
        $arguments += if ($test -eq 'fog') { @('--atmosphere', "--atmosphere-capture=$outputDirectory") } else { @("--sponza-capture=$outputDirectory") }
    }
    $runLog = Join-Path $outputDirectory 'run.log'
    & $godotExe @arguments *> $runLog
    if ($LASTEXITCODE -ne 0) { throw "TG adaptive $test failed; see $runLog" }
    if (Select-String -LiteralPath $runLog -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet) { throw "Godot reported errors; see $runLog" }
    $result = Get-Content -LiteralPath (Join-Path $outputDirectory 'results.json') -Raw | ConvertFrom-Json
    if (-not $result.passed) { throw "TG adaptive $test assertions failed" }
    Write-Output "Passed $test"
}
