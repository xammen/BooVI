@echo off
rem Compile-checks one source file with the include paths and defines of the QML bridge
rem target, using the dependencies setup-dev.ps1 installs under %USERPROFILE%\dev.
rem Usage: scripts\fork\cc-check.cmd src\ui\bridge\RemotePlay.cpp
rem Extra compiler flags go in CCX (set CCX=/Od).
setlocal
set "DEV=%USERPROFILE%\dev"
for /d %%D in ("%DEV%\deps\ffmpeg\*") do set "FF=%%D\include"
set "Q=%DEV%\Qt\6.8.3\msvc2022_64\include"
set "INC=/Iinclude /Isrc /Iapps\veyra\ui /I"%DEV%\deps\sdl3\include" /I"%FF%" /I"%DEV%\deps\dlss\include""
set "INC=%INC% /I"%Q%" /I"%Q%\QtCore" /I"%Q%\QtGui" /I"%Q%\QtQml" /I"%Q%\QtQuick" /I"%Q%\QtNetwork" /I"%Q%\QtWidgets""
set "DEFS=/DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /DVEYRA_ENABLE_REMOTEPLAY /DVEYRA_ENABLE_MOONLIGHT /DVEYRA_DISPLAY_VERSION=\"2.1.9\""
if not exist build-fork mkdir build-fork
call "%~dp0vsenv.cmd" cl /nologo /std:c++20 /EHsc /W4 /utf-8 /permissive- /Zc:__cplusplus %CCX% %DEFS% /c %INC% %1 /Fo:build-fork\cc.obj
