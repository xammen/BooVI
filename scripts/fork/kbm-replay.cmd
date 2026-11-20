@echo off
rem Builds tests\fork\KbmReplay.cpp against the KbmMapper.h in include\ and, when given a folder
rem holding another veyra\remoteplay\KbmMapper.h (old API: `sensitivity`), against that one too,
rem and prints both tables. Usage: scripts\fork\kbm-replay.cmd [old include folder]
setlocal
if not exist build-fork mkdir build-fork
set "FLAGS=/nologo /std:c++20 /EHsc /O2 /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE"
call "%~dp0vsenv.cmd" cl %FLAGS% /Iinclude tests\fork\KbmReplay.cpp /Fo:build-fork\ /Fe:build-fork\KbmReplay.exe user32.lib >build-fork\KbmReplay.build.txt 2>&1 || (type build-fork\KbmReplay.build.txt & exit /b 1)
if not "%~1"=="" (
  call "%~dp0vsenv.cmd" cl %FLAGS% /DKBM_OLD_API /I"%~1" /Iinclude tests\fork\KbmReplay.cpp /Fo:build-fork\ /Fe:build-fork\KbmReplayOld.exe user32.lib >build-fork\KbmReplayOld.build.txt 2>&1 || (type build-fork\KbmReplayOld.build.txt & exit /b 1)
  echo === %~1
  build-fork\KbmReplayOld.exe
)
echo === include
build-fork\KbmReplay.exe
