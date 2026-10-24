param([switch]$Capture)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
if (-not (Test-Path -LiteralPath (Join-Path $projectRoot 'generated/sponza/cache.json'))) {
    throw 'Run ./tools/prepare-sponza.ps1 first to generate the mesh surface cache.'
}
$arguments = @('--path', $projectRoot, '--rendering-driver', 'vulkan', '--max-fps', '60', 'res://scenes/sponza.tscn')
if ($Capture) { $arguments += @('--', "--sponza-capture=$(Join-Path $projectRoot 'artifacts/sponza')") }
& $godotExe @arguments
if ($LASTEXITCODE -ne 0) { throw "Sponza failed (exit $LASTEXITCODE)" }
if ($Capture) {
    $result = Get-Content -LiteralPath (Join-Path $projectRoot 'artifacts/sponza/results.json') -Raw | ConvertFrom-Json
    if (-not $result.passed) { throw 'Sponza GI validation failed.' }
}
