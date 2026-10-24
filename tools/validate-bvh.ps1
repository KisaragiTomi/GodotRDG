param([string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
$outputRoot = Join-Path $projectRoot 'artifacts/bvh'
& $Python (Join-Path $PSScriptRoot 'test_triangle_bvh.py')
if ($LASTEXITCODE -ne 0) { throw 'BVH geometry checks failed.' }
foreach ($method in @('median', 'sah')) {
    & $Python (Join-Path $PSScriptRoot 'triangle_bvh.py') --method $method --output (Join-Path $outputRoot $method)
    if ($LASTEXITCODE -ne 0) { throw "BVH build failed: $method" }
}
# ABBA order limits warmup/order bias. No parallel GPU workloads.
foreach ($case in @('median-run', 'sah-run', 'sah-repeat', 'median-repeat')) {
    $method = $case.Split('-')[0]
    $geometry = Join-Path $outputRoot $method
    $output = Join-Path $outputRoot $case
    $log = Join-Path $outputRoot "$case.log"
    & $godotExe --path $projectRoot --rendering-driver vulkan --max-fps 60 --script (Join-Path $PSScriptRoot 'benchmark-bvh.gd') -- --tg --atmosphere "--tg-bvh-dir=$geometry" "--benchmark-output=$output" *> $log
    if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $log -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet)) { throw "BVH GPU run failed; see $log" }
}
& $Python (Join-Path $PSScriptRoot 'compare-bvh.py')
if ($LASTEXITCODE -ne 0) { throw 'BVH shading comparison failed.' }
