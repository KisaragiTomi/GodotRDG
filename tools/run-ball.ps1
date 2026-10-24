param([ValidateSet('vulkan', 'd3d12')][string]$Backend = 'vulkan')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
& $godotExe --path $projectRoot --rendering-driver $Backend --max-fps 60 res://scenes/lab.tscn -- --ball --rays=256
exit $LASTEXITCODE
