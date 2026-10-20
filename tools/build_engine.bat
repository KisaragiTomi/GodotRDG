@echo off
REM Builds a separate Godot editor binary (godot.windows.editor.x86_64.rdg.exe) that contains the RDG module.
REM The engine source is the rdg-engine worktree (engine-level RDG support: placed resources / memory aliasing),
REM next to %GODOT_SOURCE% by default; set RDG_ENGINE_SOURCE to build from somewhere else.
REM extra_suffix=rdg keeps object files and the exe apart from the regular editor build.
REM d3d12=no because the D3D12 build deps (%LOCALAPPDATA%\Godot\build_deps) are not installed on this machine.
REM Extra SCons arguments are forwarded, e.g. `build_engine.bat dev_build=yes`.
for %%I in ("%~dp0..\modules") do set "RDG_MODULES=%%~fI"
if not defined RDG_ENGINE_SOURCE for %%I in ("%GODOT_SOURCE%\..\godot-rdg") do set "RDG_ENGINE_SOURCE=%%~fI"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 > nul 2>&1
cd /d "%RDG_ENGINE_SOURCE%"
scons -j15 target=editor platform=windows d3d12=no extra_suffix=rdg scu_build=yes "custom_modules=%RDG_MODULES%" %*
