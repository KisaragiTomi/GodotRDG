param([string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
foreach ($scene in @('courtyard', 'sponza')) {
    $outputDirectory = Join-Path $projectRoot "artifacts/atmosphere-quality/$scene"
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $log = Join-Path $outputDirectory 'run.log'
    $arguments = @('--path', $projectRoot, '--rendering-driver', 'vulkan', '--max-fps', '60', '--script', (Join-Path $PSScriptRoot 'validate-atmosphere-quality.gd'))
    if ($scene -eq 'sponza') { $arguments += @('--', '--tg', '--atmosphere') }
    & $godotExe @arguments *> $log
    if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $log -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet)) {
        throw "Atmosphere quality GPU run failed; see $log"
    }
}
& $Python (Join-Path $PSScriptRoot 'compare-atmosphere-quality.py')
if ($LASTEXITCODE -ne 0) { throw 'Atmosphere reconstruction checks failed.' }
