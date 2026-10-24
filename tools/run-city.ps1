param([switch]$Capture, [switch]$Rebuild, [switch]$FullFog)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
$projectPath = $projectRoot
# Imports are incremental and also cover fresh copies of the bundled GLB assets.
& $godotExe --headless --editor --path $projectPath --import --quit
if ($LASTEXITCODE -ne 0) { throw 'City resource import failed.' }
if ($Rebuild) {
    & $godotExe --headless --path $projectPath --script (Join-Path $PSScriptRoot 'build-city.gd')
    if ($LASTEXITCODE -ne 0) { throw 'City scene build failed.' }
}
$arguments = @('--path', $projectPath, '--rendering-driver', 'vulkan', '--max-fps', '60', 'res://scenes/city.tscn', '--')
if ($FullFog) { $arguments += '--city-full-fog' }
if ($Capture) {
    $outputDirectory = Join-Path $projectRoot 'artifacts/city'
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $arguments += "--city-capture=$outputDirectory"
    & $godotExe @arguments 2>&1 | Tee-Object -FilePath (Join-Path $outputDirectory 'run.log')
    if ($LASTEXITCODE -ne 0) { throw "City capture failed (exit $LASTEXITCODE)." }
    if (Select-String -LiteralPath (Join-Path $outputDirectory 'run.log') -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet) { throw 'Godot reported errors.' }
} else {
    & $godotExe @arguments
    if ($LASTEXITCODE -ne 0) { throw "City demo failed (exit $LASTEXITCODE)." }
}
