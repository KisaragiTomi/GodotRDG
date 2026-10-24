param([string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
$output = Join-Path $projectRoot 'artifacts/city-fog'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$log = Join-Path $output 'run.log'
& $godotExe --path $projectRoot --rendering-driver vulkan --max-fps 60 --script (Join-Path $PSScriptRoot 'validate-city-fog.gd') *> $log
if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $log -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet)) { throw "City fog run failed; see $log" }
& $Python (Join-Path $PSScriptRoot 'compare-city-fog.py')
if ($LASTEXITCODE -ne 0) { throw 'City fog reconstruction check failed.' }
$coverageOutput = Join-Path $projectRoot 'artifacts/city-coverage/after'
New-Item -ItemType Directory -Path $coverageOutput -Force | Out-Null
$coverageLog = Join-Path $coverageOutput 'run.log'
& $godotExe --path $projectRoot --rendering-driver vulkan --max-fps 60 --script (Join-Path $PSScriptRoot 'validate-city-coverage.gd') *> $coverageLog
if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $coverageLog -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet)) { throw "Distant city capture failed; see $coverageLog" }
& $Python (Join-Path $PSScriptRoot 'compare-city-coverage.py')
if ($LASTEXITCODE -ne 0) { throw 'Distant fog coverage check failed.' }
