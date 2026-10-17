@echo off
setlocal
call "%~dp0msvc-env.cmd"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not defined BS220_OUTPUT_DIR set "BS220_OUTPUT_DIR=bin"
if not exist "%BS220_OUTPUT_DIR%" mkdir "%BS220_OUTPUT_DIR%"
cl /nologo /W4 /O2 /std:c++17 /EHsc /MT check_first_person.cpp /Fe"%BS220_OUTPUT_DIR%\check_first_person.exe" /Fo"%BS220_OUTPUT_DIR%\\" /link d3d9.lib d3dcompiler.lib user32.lib
if errorlevel 1 exit /b 1
"%BS220_OUTPUT_DIR%\check_first_person.exe"
exit /b %errorlevel%
