@echo off
REM ============================================================
REM  Ember build script
REM
REM  NOTE: keep this file pure ASCII. Do NOT put Chinese (or any
REM  non-ASCII) characters in a .bat file. cmd.exe reads batch
REM  files using the system OEM codepage (GBK on this machine),
REM  but this file is saved as UTF-8. The 3 bytes of a Chinese
REM  character get mis-decoded and can swallow the following
REM  newline or caret, which corrupts line boundaries and makes
REM  cmd try to execute fragments of comments. Chinese docs live
REM  in README.md instead, where UTF-8 is handled correctly.
REM  (Learned the hard way in the Nova project.)
REM
REM  No cmake / make on this machine, so we call g++ directly.
REM
REM  NOTE: `g++ -o ember.exe src\*.cpp` does NOT work -- cmd does
REM  not expand wildcards (that is a Unix shell habit) and MinGW
REM  g++ does not either, so it receives the literal string
REM  "src\*.cpp" and fails with "Invalid argument". Hence the
REM  for-loop below that builds the file list explicitly.
REM
REM  Notably NOT passed: -static-libstdc++ / -static-libgcc.
REM  TDM-GCC already links libstdc++ statically by default (a
REM  TDM-specific deviation from stock MinGW-w64), so those flags
REM  are redundant here.
REM
REM  Flags:
REM    -std=c++17      C++17
REM    -Wall -Wextra   all warnings; zero warnings is a good habit
REM    -g              debug info for stepping in a debugger
REM    -finput-charset / -fexec-charset   read and emit UTF-8, so
REM                    Chinese string literals survive (pairs with
REM                    SetConsoleOutputCP in the platform layer)
REM    -lwinmm         for PlaySound. MUST come AFTER the source
REM                    list: GNU ld resolves libraries left to
REM                    right, and a library named before the
REM                    objects that need it is silently dropped,
REM                    leaving every PlaySound call unresolved.
REM
REM  Modes:
REM    build.bat              normal build, with the Win32 window
REM    build.bat headless     no -DEMBER_GUI; `run` becomes a
REM                           documented error and everything
REM                           else works. Tests use this one.
REM    build.bat release      -O2 -DNDEBUG
REM    build.bat nopause      do not wait for a keypress at the end
REM
REM  The lint below is the mechanical guarantee of Ember's one
REM  hard architectural rule: the VM core is pure standard C++
REM  and never includes <windows.h>. Keeping the core free of
REM  Win32 is what lets the test harness drive it with no window
REM  open, which is what makes 150 automated checks possible.
REM  Two files may include it:
REM    *_win32.*   the platform layer proper (window, sound)
REM    utf8path.*  a portability shim for opening UTF-8 paths;
REM                it has a POSIX #else branch, so the core still
REM                builds anywhere. See utf8path.hpp.
REM  Anything else is a build failure, not a code review note.
REM ============================================================

REM ---- always run from the directory this script lives in ----
cd /d "%~dp0"

setlocal enabledelayedexpansion

set OUT=ember.exe

REM ---- pick options ----
set OPT=-g
set NOPAUSE=0
set GUIFLAG=-DEMBER_GUI
set WINGUI=1
for %%a in (%*) do (
    if /i "%%a"=="release" set OPT=-O2 -DNDEBUG
    if /i "%%a"=="nopause" set NOPAUSE=1
    if /i "%%a"=="headless" set WINGUI=0
)
if "!WINGUI!"=="0" set GUIFLAG=

REM ---- lint: windows.h must not leak out of the platform layer ----
REM Deliberately a dumb literal string search. Two things were tried
REM and rejected:
REM   /r with a regex   -- findstr's built-in regex engine matched
REM                        EVERY file with the pattern used here
REM                        (verified with a probe script), so a lint
REM                        that can never pass is worse than none.
REM   ^ as a line anchor-- a caret inside a for-loop body is cmd's
REM                        escape character and gets eaten before
REM                        findstr ever sees it.
REM So: one literal, no parsing, no exceptions. The consequence is
REM that a COMMENT merely naming the header trips it too. That is
REM accepted on purpose -- the fix is to write it as "windows.h"
REM without the angle brackets when talking about it in prose.
REM A lint with no judgement calls is one nobody has to debug.
for %%f in (src\*.cpp src\*.hpp) do (
    findstr /i /m /c:"<windows.h>" "%%f" >nul 2>&1
    if not errorlevel 1 (
        echo %%f | findstr /i /c:"_win32." /c:"utf8path." >nul
        if errorlevel 1 (
            echo.
            echo [FAIL] %%f includes windows.h
            echo        only *_win32.* and utf8path.* may do that.
            echo        the VM core has to stay pure standard C++
            echo        so the tests can drive it with no window.
            exit /b 1
        )
    )
)

REM ---- collect sources ----
REM enabledelayedexpansion lets us use !SRCS! (evaluated on the
REM spot). With %SRCS% the whole for-loop body would be expanded
REM once up front, when SRCS is still empty -- classic cmd trap.
set SRCS=
for %%f in (src\*.cpp) do set SRCS=!SRCS! %%f

if "!SRCS!"=="" (
    echo [FAIL] no .cpp files found under src\
    exit /b 1
)

echo [BUILD] g++ -std=c++17 -Wall -Wextra !OPT! %GUIFLAG% -o %OUT%
echo.

g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter !OPT! %GUIFLAG% ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
    -o %OUT% !SRCS! -lwinmm

if errorlevel 1 (
    echo.
    echo [FAIL] compile errors, see above
    exit /b 1
)

echo.
if "!WINGUI!"=="0" (
    echo [OK] built %OUT% ^(headless: no window, tests run against this^)
) else (
    echo [OK] built %OUT% ^(with window^)
)

REM ---- keep the window open when double-clicked ----
if "!NOPAUSE!"=="0" pause

endlocal
