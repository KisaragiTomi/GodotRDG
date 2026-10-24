param([switch]$Capture)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
$projectPath = $projectRoot
if (-not (Test-Path -LiteralPath (Join-Path $projectPath '.godot/global_script_class_cache.cfg'))) {
    & $godotExe --headless --editor --path $projectPath --import --quit
    if ($LASTEXITCODE -ne 0) { throw 'First-run resource import failed.' }
}
$arguments = @('--path', $projectRoot, '--rendering-driver', 'vulkan', '--max-fps', '60', 'res://scenes/atmosphere.tscn')
if ($Capture) {
    $outputDirectory = Join-Path $projectRoot 'artifacts/atmosphere'
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $arguments += @('--', "--atmosphere-capture=$outputDirectory")
    & $godotExe @arguments 2>&1 | Tee-Object -FilePath (Join-Path $outputDirectory 'run.log')
    if ($LASTEXITCODE -ne 0) { throw "Atmosphere validation failed (exit $LASTEXITCODE)." }
    if (Select-String -LiteralPath (Join-Path $outputDirectory 'run.log') -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet) { throw 'Godot reported errors.' }
    $result = Get-Content -LiteralPath (Join-Path $outputDirectory 'results.json') -Encoding UTF8 -Raw | ConvertFrom-Json
    if (-not $result.passed) { throw 'Atmosphere image checks failed.' }
} else {
    & $godotExe @arguments
    if ($LASTEXITCODE -ne 0) { throw "Atmosphere failed (exit $LASTEXITCODE)." }
}
