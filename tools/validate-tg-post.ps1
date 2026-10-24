param([Parameter(Mandatory = $true)][string]$Python)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
$outputRoot = Join-Path $projectRoot 'artifacts/tg-post'
foreach ($variant in @('basic', 'post')) {
    foreach ($test in @('camera', 'noise')) {
        $outputDirectory = Join-Path $outputRoot "$variant-$test"
        New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
        $arguments = @('--path', $projectRoot, '--rendering-driver', 'vulkan', '--max-fps', '60', 'res://scenes/sponza.tscn', '--', '--tg', '--tg-sampling=0', "--tg-$test-capture=$outputDirectory")
        if ($variant -eq 'basic') { $arguments += '--tg-basic-filter' }
        $runLog = Join-Path $outputDirectory 'run.log'
        & $godotExe @arguments 2>&1 | Tee-Object -FilePath $runLog
        if ($LASTEXITCODE -ne 0) { throw "TG post $variant/$test failed (exit $LASTEXITCODE)" }
        if (Select-String -LiteralPath $runLog -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet) { throw "Godot reported errors; see $runLog" }
    }
}
$reference = Join-Path $projectRoot 'artifacts/tg-camera/reference'
if (-not (Test-Path -LiteralPath (Join-Path $reference 'results.json'))) {
    New-Item -ItemType Directory -Path $reference -Force | Out-Null
    & $godotExe --path $projectRoot --rendering-driver vulkan --max-fps 60 res://scenes/sponza.tscn -- --tg --tg-sampling=2 --tg-basic-filter "--tg-camera-capture=$reference" *> (Join-Path $reference 'run.log')
    if ($LASTEXITCODE -ne 0) { throw 'TG high-sample reference failed.' }
}
& $Python (Join-Path $PSScriptRoot 'compare-tg-post.py') $outputRoot $reference
if ($LASTEXITCODE -ne 0) { throw 'TG post denoising comparison failed.' }
