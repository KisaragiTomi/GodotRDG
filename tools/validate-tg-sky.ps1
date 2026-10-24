param([Parameter(Mandatory = $true)][string]$Python)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
$outputDirectory = Join-Path $projectRoot 'artifacts/tg-sky'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$runLog = Join-Path $outputDirectory 'run.log'
& $godotExe --path $projectRoot --rendering-driver vulkan --max-fps 60 --quit-after 4000 --script (Join-Path $PSScriptRoot 'validate-tg-sky.gd') -- --tg --atmosphere "--sky-output=$outputDirectory" *> $runLog
if ($LASTEXITCODE -ne 0) { throw "Sky validation failed; see $runLog" }
if (Select-String -LiteralPath $runLog -Pattern 'SCRIPT ERROR:|ERROR:' -Quiet) { throw "Godot reported errors; see $runLog" }
& $Python (Join-Path $PSScriptRoot 'compare-tg-sky.py') $outputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Sky comparison failed.' }
