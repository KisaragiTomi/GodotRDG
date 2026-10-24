param([Parameter(Mandatory = $true)][string]$Python)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
$outputRoot = Join-Path $projectRoot 'artifacts/tg-camera'
foreach ($profile in 0..2) {
    $outputDirectory = Join-Path $outputRoot @('sparse', 'quality', 'reference')[$profile]
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $arguments = @('--path', $projectRoot, '--rendering-driver', 'vulkan', '--max-fps', '60', 'res://scenes/sponza.tscn', '--', '--tg', "--tg-sampling=$profile", "--tg-camera-capture=$outputDirectory")
    $runLog = Join-Path $outputDirectory 'run.log'
    $arguments += '--tg-basic-filter' # Isolate ray-budget changes in this comparison.
    & $godotExe @arguments 2>&1 | Tee-Object -FilePath $runLog
    if ($LASTEXITCODE -ne 0) { throw "TG camera profile $profile failed (exit $LASTEXITCODE)" }
    if (Select-String -LiteralPath $runLog -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet) { throw "Godot reported errors; see $runLog" }
}
& $Python (Join-Path $PSScriptRoot 'compare-tg-camera.py') $outputRoot
if ($LASTEXITCODE -ne 0) { throw 'TG camera comparison failed.' }
