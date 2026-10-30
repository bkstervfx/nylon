@echo off
setlocal EnableExtensions

cd /d "%~dp0"
set "ROOT=%CD%"

if "%LOCALAPPDATA%"=="" (
    echo error: LOCALAPPDATA is not available.
    exit /b 1
)

set "NYLON_HOME=%LOCALAPPDATA%\Temp\nylon"
set "TOOLS=%NYLON_HOME%\tools"
set "LLVM_DIR=%TOOLS%\llvm-mingw"
set "SQLITE_DIR=%TOOLS%\sqlite"
set "BUILD=%ROOT%\build"
set "BIN=%ROOT%\bin"

set "CC=%LLVM_DIR%\bin\x86_64-w64-mingw32-clang.exe"
set "CXX=%LLVM_DIR%\bin\x86_64-w64-mingw32-clang++.exe"
if not exist "%CXX%" (
    set "CC=%LLVM_DIR%\bin\clang.exe"
    set "CXX=%LLVM_DIR%\bin\clang++.exe"
)

if not exist "%CXX%" (
    echo error: Cached LLVM-MinGW was not found.
    echo Run build.cmd once first so Nylon can prepare its portable toolchain.
    exit /b 1
)
if not exist "%SQLITE_DIR%\sqlite3.c" (
    echo error: Cached SQLite sources were not found.
    echo Run build.cmd once first so Nylon can prepare SQLite.
    exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%BIN%" mkdir "%BIN%"
set "SQLITE_OBJ=%BUILD%\semantic-sqlite3.o"
set "OLD_MARKER=%BIN%\nylon-semantic.request-v3"
set "SEMANTIC_MARKER=%BIN%\nylon-semantic.identity-v1"
if exist "%OLD_MARKER%" del /q "%OLD_MARKER%" >nul 2>nul
if exist "%SEMANTIC_MARKER%" del /q "%SEMANTIC_MARKER%" >nul 2>nul

echo [1/4] Compiling SQLite for Nylon Semantic Lifter...
"%CC%" --target=x86_64-w64-mingw32 -O2 -DNDEBUG -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -fstack-protector-strong -ffunction-sections -fdata-sections -mguard=cf -c "%SQLITE_DIR%\sqlite3.c" -o "%SQLITE_OBJ%"
if errorlevel 1 (
    echo error: Semantic SQLite compilation failed.
    exit /b 1
)

echo [2/4] Building Nylon Semantic Lifter performance runtime...
"%CXX%" --target=x86_64-w64-mingw32 -std=c++20 -O2 -DNDEBUG -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -Wall -Wextra -Wpedantic -Werror -Wno-unused-function -fstack-protector-strong -ffunction-sections -fdata-sections -mguard=cf -fuse-ld=lld -municode -static -pthread -Wl,--gc-sections -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va -I"%SQLITE_DIR%" -include "%ROOT%\semantic\compiler_compat.hpp" "%ROOT%\semantic\semantic_v4.cpp" "%SQLITE_OBJ%" "%ROOT%\src\platform\hash.cpp" "%ROOT%\src\platform\win32.cpp" "%ROOT%\src\platform\file_lock.cpp" -lwinhttp -lbcrypt -lws2_32 -o "%BIN%\nylon-semantic.exe"
if errorlevel 1 (
    echo error: Nylon Semantic Lifter build failed.
    exit /b 1
)

echo [3/4] Building deterministic semantic preclassifier...
"%CXX%" --target=x86_64-w64-mingw32 -std=c++20 -O2 -DNDEBUG -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -Wall -Wextra -Wpedantic -Werror -Wno-unused-function -fstack-protector-strong -ffunction-sections -fdata-sections -mguard=cf -fuse-ld=lld -municode -static -Wl,--gc-sections -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va -I"%SQLITE_DIR%" -include "%ROOT%\semantic\compiler_compat.hpp" "%ROOT%\semantic\preclassify_v2.cpp" "%SQLITE_OBJ%" "%ROOT%\src\platform\hash.cpp" "%ROOT%\src\platform\win32.cpp" "%ROOT%\src\platform\file_lock.cpp" -lbcrypt -o "%BIN%\nylon-semantic-classify.exe"
if errorlevel 1 (
    echo error: Semantic preclassifier build failed.
    exit /b 1
)

echo [4/4] Building linear-time semantic hierarchy linker...
"%CXX%" --target=x86_64-w64-mingw32 -std=c++20 -O2 -DNDEBUG -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -Wall -Wextra -Wpedantic -Werror -Wno-unused-function -fstack-protector-strong -ffunction-sections -fdata-sections -mguard=cf -fuse-ld=lld -municode -static -Wl,--gc-sections -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va -I"%SQLITE_DIR%" -include "%ROOT%\semantic\compiler_compat.hpp" "%ROOT%\semantic\postprocess_v2.cpp" "%SQLITE_OBJ%" "%ROOT%\src\platform\hash.cpp" "%ROOT%\src\platform\win32.cpp" "%ROOT%\src\platform\file_lock.cpp" -lbcrypt -o "%BIN%\nylon-semantic-link.exe"
if errorlevel 1 (
    echo error: Semantic hierarchy linker build failed.
    exit /b 1
)

if not exist "%BIN%\nylon-semantic.exe" (
    echo error: Semantic build finished without producing nylon-semantic.exe.
    exit /b 1
)
if not exist "%BIN%\nylon-semantic-classify.exe" (
    echo error: Semantic build finished without producing nylon-semantic-classify.exe.
    exit /b 1
)
if not exist "%BIN%\nylon-semantic-link.exe" (
    echo error: Semantic build finished without producing nylon-semantic-link.exe.
    exit /b 1
)

>"%SEMANTIC_MARKER%" echo request-contract-v3-performance-v1-raw-db-sha256-v1
if errorlevel 1 (
    echo error: Unable to mark optimized semantic build.
    exit /b 1
)

echo.
echo Semantic build complete:
echo   %BIN%\nylon-semantic.exe
echo   %BIN%\nylon-semantic-classify.exe
echo   %BIN%\nylon-semantic-link.exe
exit /b 0
