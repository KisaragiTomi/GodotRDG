param()
$ErrorActionPreference = 'Stop'
# Opens this project in the rdg engine build's editor. RDG (modules/rdg) and the TG GI / fog classes (modules/tg_gi) are
# engine classes, so any other Godot editor (e.g. the one Windows associates with project.godot) reports parse errors.
$projectRoot = Split-Path -Parent $PSScriptRoot
$godotExe = & (Join-Path $PSScriptRoot 'resolve-godot.ps1')
# resolve-godot.ps1 returns the console wrapper the scripts need; the editor itself runs without a console window.
$editorExe = $godotExe -replace '\.console\.exe$', '.exe'
if (-not (Test-Path -LiteralPath $editorExe)) { $editorExe = $godotExe }
Start-Process -FilePath $editorExe -ArgumentList @('--editor', '--path', "`"$projectRoot`"")
