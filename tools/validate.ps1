param([switch]$BakeLightmap, [switch]$Emissive, [switch]$Ball, [ValidateSet('vulkan', 'd3d12')][string]$Backend = 'vulkan', [ValidateRange(16, 512)][int]$Rays = 256)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$projectPath = $projectRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
# The lab is not the project's main scene: every lab run names it.
$labScene = 'res://scenes/lab.tscn'
$artifactPath = Join-Path $projectRoot 'artifacts'
New-Item -ItemType Directory -Force -Path $artifactPath | Out-Null
function Invoke-GodotCheck([string]$Name, [string[]]$Arguments) {
    $logPath = Join-Path $artifactPath "$Name.log"
    & $godotExe @Arguments > $logPath 2>&1
    $code = $LASTEXITCODE
    $errors = Select-String -LiteralPath $logPath -Pattern '^(SCRIPT ERROR|ERROR):'
    if ($code -ne 0 -or $errors) {
        Get-Content -LiteralPath $logPath -Tail 40
        throw "$Name failed (exit $code); see $logPath"
    }
    Write-Host "$Name passed: $logPath"
}
Invoke-GodotCheck 'import' @('--headless', '--editor', '--path', $projectPath, '--import', '--quit')
if ($BakeLightmap) {
    Invoke-GodotCheck 'native-export' @('--path', $projectPath, '--rendering-driver', $Backend, $labScene, '--', '--export-native-scene=res://generated/native_reference.tscn')
    Invoke-GodotCheck 'lightmap-bake' @('--editor', '--path', $projectPath, '--rendering-driver', $Backend, '--', '--native-bake', '--scene=res://generated/native_reference.tscn', '--output=res://generated/native_reference.lmbake')
}
Invoke-GodotCheck 'transport-test' @('--path', $projectPath, '--rendering-driver', $Backend, '--script', 'res://tests/transport_test.gd')
Invoke-GodotCheck 'material-test' @('--path', $projectPath, '--rendering-driver', $Backend, '--script', 'res://tests/material_test.gd')
$labArguments = @('--path', $projectPath, '--rendering-driver', $Backend, $labScene, '--', '--validate', '--voxel', "--rays=$Rays")
if (Test-Path -LiteralPath (Join-Path $projectPath 'generated/native_reference.lmbake')) { $labArguments += '--lightmap' }
Invoke-GodotCheck 'validation-run' $labArguments
$result = Get-Content -LiteralPath (Join-Path $artifactPath 'validation/results.json') -Raw | ConvertFrom-Json
if (-not $result.passed) { throw 'Lab assertions failed.' }
$transport = Get-Content -LiteralPath (Join-Path $artifactPath 'validation/transport_test.json') -Raw | ConvertFrom-Json
if (-not $transport.passed) { throw 'Thin-wall transport assertions failed.' }
if ($Emissive) {
    Invoke-GodotCheck 'emissive-quality-test' @('--path', $projectPath, '--rendering-driver', $Backend, '--script', 'res://tests/emissive_quality_test.gd')
    $quality = Get-Content -LiteralPath (Join-Path $artifactPath 'validation/emissive_quality_test.json') -Raw | ConvertFrom-Json
    if (-not $quality.passed) { throw 'Emissive radiometry and smoothness assertions failed.' }
    Invoke-GodotCheck 'emissive-detail-test' @('--path', $projectPath, '--rendering-driver', $Backend, '--script', 'res://tests/emissive_detail_test.gd')
    $detail = Get-Content -LiteralPath (Join-Path $artifactPath 'validation/emissive_detail_test.json') -Raw | ConvertFrom-Json
    if (-not $detail.passed) { throw 'Emissive near-field reconstruction assertions failed.' }
    $emissivePath = Join-Path $artifactPath 'emissive'
    Invoke-GodotCheck 'emissive-run' @('--path', $projectPath, '--rendering-driver', $Backend, $labScene, '--', "--emissive-capture=$emissivePath", '--rays=512')
    $emission = Get-Content -LiteralPath (Join-Path $emissivePath 'results.json') -Raw | ConvertFrom-Json
    if (-not $emission.passed) { throw 'Emissive assertions failed.' }
}
if ($Ball) {
    Invoke-GodotCheck 'sphere-test' @('--path', $projectPath, '--rendering-driver', $Backend, '--script', 'res://tests/sphere_test.gd')
    $sphere = Get-Content -LiteralPath (Join-Path $artifactPath 'validation/sphere_test.json') -Raw | ConvertFrom-Json
    if (-not $sphere.passed) { throw 'Sphere radiometry and visibility assertions failed.' }
    $ballPath = Join-Path $artifactPath 'ball'
    Invoke-GodotCheck 'ball-run' @('--path', $projectPath, '--rendering-driver', $Backend, $labScene, '--', "--ball-capture=$ballPath", '--rays=256')
    $ballResult = Get-Content -LiteralPath (Join-Path $ballPath 'results.json') -Raw | ConvertFrom-Json
    if (-not $ballResult.passed) { throw 'Ball movement and lighting assertions failed.' }
}
Write-Host 'All checks passed. Captures and numerical results: artifacts/validation'
