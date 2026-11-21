@echo off
rem Builds and runs the fork's standalone tests (tests\fork). Run from the repository root.
setlocal
if not exist build-fork mkdir build-fork
set "FLAGS=/nologo /std:c++20 /EHsc /W4 /WX /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /Iinclude /Fo:build-fork\"
set FAILED=0
for %%T in (PadPolicyTests PadPresenceTests ControllerQueueTests KbmMapperTests FocusReparentTest LoginPinStoreTests StreamCadenceTests GuestProtocolTests GuestQualityTests GuestAccessTests RoomModelTests LifelineTests InboundBudgetTests GuestNotifyTests CameraAnticipationTests UserDataMigrationTests InputLifetimeTests CaptureAudioDriftTests) do (
  call "%~dp0vsenv.cmd" cl %FLAGS% tests\fork\%%T.cpp /Fe:build-fork\%%T.exe user32.lib crypt32.lib bcrypt.lib >build-fork\%%T.build.txt 2>&1 || (echo BUILD FAILED %%T & type build-fork\%%T.build.txt & set FAILED=1)
  if exist build-fork\%%T.exe (
    echo === %%T
    build-fork\%%T.exe || set FAILED=1
  )
)
rem The guest page's Steam Controller parser, when node is installed.
where node >nul 2>&1 && (echo === GuestSteamPadTests & node tests\fork\GuestSteamPadTests.mjs || set FAILED=1)
if %FAILED%==1 (echo SOME TESTS FAILED & exit /b 1)
echo ALL FORK TESTS PASSED
