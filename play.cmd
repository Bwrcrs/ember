@echo off
rem ============================================================
rem  Ember - double-click launcher
rem
rem  Type-safe usage:
rem    1. Double-click this file        -> plays the default test ROM
rem    2. Drag a .ch8 file onto this    -> plays that ROM
rem    3. From cmd:  play.cmd myrom.ch8
rem
rem  NOTE: this file must stay pure ASCII. cmd.exe reads .bat/.cmd
rem  using the system OEM codepage (GBK on this machine), so UTF-8
rem  Chinese bytes get mis-decoded and can eat line breaks.
rem ============================================================

cd /d "%~dp0"

set "ROM=%~1"
if "%ROM%"=="" set "ROM=tests\run\keyecho.ch8"

echo.
echo  ============================================================
echo    Ember  -  a CHIP-8 computer you built yourself
echo  ============================================================
echo.
echo    ROM : %ROM%
echo.
echo    Keys:  1 2 3 4  /  Q W E R  /  A S D F  /  Z X C V
echo    Quit:  Esc
echo.
pause

ember.exe play "%ROM%"

echo.
echo  [ember exited with code %ERRORLEVEL%]
echo.
pause
