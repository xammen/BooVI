@echo off
rem Runs a command inside the portable MSVC environment made by setup-dev.ps1.
rem Usage: scripts\fork\vsenv.cmd cl /nologo ...
if "%VEYRA_MSVC%"=="" set "VEYRA_MSVC=%USERPROFILE%\dev\toolchain\msvc"
call "%VEYRA_MSVC%\setup_x64.bat" >nul || exit /b 1
rem The portable layout sets no WindowsSdkDir; tools that look for the SDK (dxc) need it.
if "%WindowsSdkDir%"=="" set "WindowsSdkDir=%VEYRA_MSVC%\Windows Kits\10\"
%*
