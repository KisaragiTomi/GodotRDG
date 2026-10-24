param([ValidateSet('vulkan', 'd3d12')][string]$Backend = 'vulkan', [ValidateRange(16, 512)][int]$Rays = 256)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
& $godotExe --path $projectRoot --rendering-driver $Backend res://scenes/lab.tscn -- "--rays=$Rays"
exit $LASTEXITCODE
