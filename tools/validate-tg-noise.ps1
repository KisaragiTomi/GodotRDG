param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
$outputRoot = Join-Path $projectRoot 'artifacts/tg-noise'
foreach ($variant in @('before', 'after')) {
    $outputDirectory = Join-Path $outputRoot $variant
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $arguments = @('--path', $projectRoot, '--rendering-driver', 'vulkan', '--max-fps', '60', 'res://scenes/sponza.tscn', '--', '--tg', '--tg-sampling=0', "--tg-noise-capture=$outputDirectory")
    $arguments += '--tg-basic-filter' # Keep this historical comparison isolated from the new post filter.
    if ($variant -eq 'before') { $arguments += '--tg-legacy-denoise' }
    $runLog = Join-Path $outputDirectory 'run.log'
    & $godotExe @arguments 2>&1 | Tee-Object -FilePath $runLog
    if ($LASTEXITCODE -ne 0) { throw "TG noise $variant failed (exit $LASTEXITCODE)" }
    if (Select-String -LiteralPath $runLog -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet) { throw "Godot reported errors; see $runLog" }
}
$before = Get-Content -LiteralPath (Join-Path $outputRoot 'before/results.json') -Raw | ConvertFrom-Json
$after = Get-Content -LiteralPath (Join-Path $outputRoot 'after/results.json') -Raw | ConvertFrom-Json
$comparison = [ordered]@{
    static_sh_ratio = $after.static.sh_delta / $before.static.sh_delta
    static_frame_ratio = $after.static.frame_delta / $before.static.frame_delta
    moving_sh_ratio = $after.moving.sh_delta / $before.moving.sh_delta
    settled_sh_ratio = $after.settled_after_move.sh_delta / $before.settled_after_move.sh_delta
    passed = $before.passed -and $after.passed -and ($after.static.sh_delta -lt $before.static.sh_delta * 0.7) -and ($after.moving.sh_delta -lt $before.moving.sh_delta * 0.7)
}
$comparison | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputRoot 'comparison.json')
$comparison | ConvertTo-Json
if (-not $comparison.passed) { throw 'TG noise comparison failed.' }
