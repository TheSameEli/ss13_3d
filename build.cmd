@echo off
setlocal
call "%~dp0msvc-env.cmd"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not defined BS220_OUTPUT_DIR set "BS220_OUTPUT_DIR=bin"
if not exist "%BS220_OUTPUT_DIR%" mkdir "%BS220_OUTPUT_DIR%"
if /I "%~1"=="--code-only" goto compile
python compile_geometry.py
if errorlevel 1 exit /b 1
:compile
cl /nologo /W4 /O2 /std:c++17 /EHsc /LD /MT probe.cpp volume.cpp first_person.cpp /Fe"%BS220_OUTPUT_DIR%\scene_probe.dll" /Fo"%BS220_OUTPUT_DIR%\\" /link /OUT:"%BS220_OUTPUT_DIR%\scene_probe.dll" d3d9.lib d3dcompiler.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /EHsc /MT launch.cpp /Fe"%BS220_OUTPUT_DIR%\launch.exe" /Fo"%BS220_OUTPUT_DIR%\launch.obj" /link user32.lib
exit /b %errorlevel%
