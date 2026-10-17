@echo off
rem Use the 64-bit compiler host, targeting x86: generated artwork exceeds x86 compiler heap limits.
if defined VSCMD_ARG_TGT_ARCH if /I "%VSCMD_ARG_TGT_ARCH%"=="x86" if /I "%VSCMD_ARG_HOST_ARCH%"=="x64" exit /b 0
if defined BS220_VCVARS goto configure
set "BS220_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%BS220_VSWHERE%" goto missing
for /f "usebackq tokens=*" %%i in (`"%BS220_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "BS220_VCVARS=%%i\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
:configure
if not exist "%BS220_VCVARS%" goto missing
call "%BS220_VCVARS%" >nul
exit /b %errorlevel%
:missing
echo Install Visual Studio C++ Build Tools and Windows SDK, or set BS220_VCVARS to vcvarsamd64_x86.bat.
exit /b 1
