param()
$ErrorActionPreference = 'Stop'
# Override per shell; no machine-specific paths are written into the project.
if ($env:GODOT_EXE) {
    $command = Get-Command $env:GODOT_EXE -CommandType Application -ErrorAction SilentlyContinue
    if (-not $command) { throw 'GODOT_EXE must point to the rdg engine build (the console executable on Windows).' }
    return $command.Source
}
# TGScreenGI / TGSurfaceFog are engine classes (modules/tg_gi): the scenes need the binary built by tools/build_engine.bat.
$engineRoots = @()
if ($env:RDG_ENGINE_SOURCE) { $engineRoots += $env:RDG_ENGINE_SOURCE }
if ($env:GODOT_SOURCE) { $engineRoots += (Join-Path (Split-Path -Parent $env:GODOT_SOURCE) 'godot-rdg') }
foreach ($root in $engineRoots) {
    $build = Join-Path $root 'bin/godot.windows.editor.x86_64.rdg.console.exe'
    if (Test-Path -LiteralPath $build) { return (Resolve-Path -LiteralPath $build).Path }
}
throw 'The rdg engine build was not found. Build it with tools/build_engine.bat (see README) or set GODOT_EXE.'
