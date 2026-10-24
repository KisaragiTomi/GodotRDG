param([Parameter(Mandatory = $true)][string]$Python)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
$outputRoot = Join-Path $projectRoot 'artifacts/tg-world-cache'
$probeDirectory = Join-Path $outputRoot 'probe'
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
& $Python (Join-Path $PSScriptRoot 'check-world-cache-probe.py') prepare $probeDirectory
if ($LASTEXITCODE -ne 0) { throw 'Probe preparation failed.' }
& $godotExe --headless --editor --path $projectRoot --import --quit *> (Join-Path $outputRoot 'import.log')
if ($LASTEXITCODE -ne 0) { throw 'Resource import failed.' }
$probeLog = Join-Path $probeDirectory 'run.log'
& $godotExe --path $projectRoot --rendering-driver vulkan --quit-after 600 --script (Join-Path $PSScriptRoot 'validate-world-cache-probe.gd') -- "--world-cache-probe=$probeDirectory" *> $probeLog
if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $probeLog -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet)) { throw "GPU cache probe failed; see $probeLog" }
& $Python (Join-Path $PSScriptRoot 'check-world-cache-probe.py') check $probeDirectory
if ($LASTEXITCODE -ne 0) { throw 'GPU cache probe assertions failed.' }
$stabilityLog = Join-Path $outputRoot 'stability.log'
& $godotExe --path $projectRoot --rendering-driver vulkan --max-fps 60 --quit-after 1600 --script (Join-Path $PSScriptRoot 'validate-world-cache-stability.gd') -- --tg --atmosphere --tg-world-cache *> $stabilityLog
if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $stabilityLog -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet)) { throw "World cache stability failed; see $stabilityLog" }
$stability = Get-Content -LiteralPath (Join-Path $outputRoot 'stability/results.json') -Raw | ConvertFrom-Json
if (-not $stability.passed) { throw 'World cache stability assertions failed.' }
foreach ($variant in @('off', 'on', 'fog')) {
    $outputDirectory = Join-Path $outputRoot $variant
    $arguments = @('--path', $projectRoot, '--rendering-driver', 'vulkan', '--max-fps', '60', '--quit-after', '3000', '--script', (Join-Path $PSScriptRoot 'validate-tg-world-cache.gd'), '--', '--tg', '--atmosphere', "--world-cache-output=$outputDirectory")
    if ($variant -ne 'off') { $arguments += '--tg-world-cache' }
    if ($variant -eq 'fog') { $arguments += '--world-cache-fog' }
    $runLog = Join-Path $outputRoot ($variant + '.log')
    & $godotExe @arguments *> $runLog
    if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $runLog -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet)) { throw "World cache $variant failed; see $runLog" }
    $result = Get-Content -LiteralPath (Join-Path $outputDirectory 'results.json') -Raw | ConvertFrom-Json
    if (-not $result.passed) { throw "World cache $variant assertions failed." }
    Write-Output "Captured $variant"
}
& $Python (Join-Path $PSScriptRoot 'compare-tg-world-cache.py') $outputRoot
if ($LASTEXITCODE -ne 0) { throw 'World cache comparison failed.' }
