param([string]$Python = 'python', [ValidateSet('sah', 'median')][string]$Bvh = 'sah')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
# A fresh download has no Godot import cache yet.
& $godotExe --headless --editor --path $projectRoot --import --quit
if ($LASTEXITCODE -ne 0) { throw 'Sponza source asset import failed.' }
& $godotExe --headless --path $projectRoot --script res://scripts/prepare_sponza.gd
if ($LASTEXITCODE -ne 0) { throw 'Sponza mesh export failed.' }
& $Python (Join-Path $PSScriptRoot 'prepare-sponza.py') --bvh $Bvh
if ($LASTEXITCODE -ne 0) { throw 'Sponza BVH preparation failed. Python requires numpy.' }
& $godotExe --headless --editor --path $projectRoot --import --quit
if ($LASTEXITCODE -ne 0) { throw 'Sponza generated resource import failed.' }
