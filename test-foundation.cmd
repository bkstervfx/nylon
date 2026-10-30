@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "TOOLS=%LOCALAPPDATA%\Temp\nylon\tools"
set "CXX=%TOOLS%\llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe"
if not exist "%CXX%" set "CXX=%TOOLS%\llvm-mingw\bin\clang++.exe"
if not exist build\sqlite3.o (
    echo Run build.cmd first.
    exit /b 1
)
"%CXX%" --target=x86_64-w64-mingw32 -std=c++20 -O2 -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -Wall -Wextra -Wpedantic -Werror -municode -static -Isrc -I"%TOOLS%\sqlite" tests\foundation.cpp src\evidence\evidence.cpp src\evidence\source_media.cpp src\evidence\inventory.cpp src\media\catalog.cpp src\database\database.cpp src\database\staging.cpp src\platform\win32.cpp src\platform\hash.cpp build\sqlite3.o -lbcrypt -lxmllite -lshlwapi -o bin\nylon-foundation-test.exe
if errorlevel 1 exit /b 1
python tests\foundation.py
exit /b %ERRORLEVEL%
