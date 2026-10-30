@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "TOOLS=%LOCALAPPDATA%\Temp\nylon\tools"
set "CXX=%TOOLS%\llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe"
if not exist "%CXX%" set "CXX=%TOOLS%\llvm-mingw\bin\clang++.exe"
if not exist build\sqlite3.o exit /b 1
"%CXX%" --target=x86_64-w64-mingw32 -std=c++20 -O2 -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -Wall -Wextra -Wpedantic -Werror -municode -static -Isrc -I"%TOOLS%\sqlite" tests\catalog.cpp src\evidence\evidence.cpp src\evidence\source_media.cpp src\database\database.cpp src\database\staging.cpp src\media\media.cpp src\media\cache.cpp src\media\catalog.cpp src\platform\win32.cpp src\platform\hash.cpp src\platform\file_lock.cpp src\platform\process.cpp build\sqlite3.o -lbcrypt -lxmllite -lshlwapi -o bin\nylon-catalog-test.exe
if errorlevel 1 exit /b 1
"%CXX%" --target=x86_64-w64-mingw32 -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -municode -static tests\media_tool_fixture.cpp -o bin\nylon-media-tool-fixture.exe
if errorlevel 1 exit /b 1
python tests\catalog.py
exit /b %ERRORLEVEL%
