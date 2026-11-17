@echo off
rem Runs a command inside the Visual Studio 2022 x64 developer environment (the Windows SDK and
rem the MSVC libraries; clang++ and lld come from LLVM 20, REXNATIVE_LLVM), found with vswhere:
rem any edition or the Build Tools, with the C++ workload. From pgr4-native's
rem scripts/windows/vcenv.cmd.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vcenv: vswhere.exe not found: install Visual Studio 2022 or its Build Tools & exit /b 1)
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo vcenv: no Visual Studio with the C++ x64 tools & exit /b 1)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
%*
