@echo off
rem Builds AZS1HD.Foveated.Native.dll (x64) with the MSVC toolchain.
rem Needs Visual Studio 2019 or newer, or the Build Tools, with the "Desktop development with C++" workload.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found: install Visual Studio or the Build Tools. & exit /b 1)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio installation with the C++ x64 tools found. & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
pushd "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /W3 /EHsc /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Ithird_party\nvapi /Ithird_party\openvr ^
   /LD foveated.cpp third_party\nvapi\nvapi64.lib d3d11.lib dxgi.lib ^
   /Fobuild\ /Fe:build\AZS1HD.Foveated.Native.dll /link /INCREMENTAL:NO
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
