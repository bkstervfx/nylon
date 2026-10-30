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
if not exist "%SQLITE_DIR%\sqlite3.h" (
    echo error: Cached sqlite3.h was not found.
    exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%BIN%" mkdir "%BIN%"

set "SQLITE_OBJ=%BUILD%\viewer-sqlite3.o"
set "ATLAS_SOURCE=%BUILD%\viewer-atlas-v2.cpp"
set "OLD_SEMANTIC_MARKER=%BIN%\nylon-viewer.semantic-v1"
set "OLD_ATLAS_MARKER=%BIN%\nylon-viewer.atlas-v1"
set "VIEWER_MARKER=%BIN%\nylon-viewer.atlas-v2"
if exist "%OLD_SEMANTIC_MARKER%" del /q "%OLD_SEMANTIC_MARKER%" >nul 2>nul
if exist "%OLD_ATLAS_MARKER%" del /q "%OLD_ATLAS_MARKER%" >nul 2>nul
if exist "%VIEWER_MARKER%" del /q "%VIEWER_MARKER%" >nul 2>nul

echo [1/3] Compiling SQLite for Nylon Viewer...
"%CC%" --target=x86_64-w64-mingw32 -O2 -DNDEBUG -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -fstack-protector-strong -ffunction-sections -fdata-sections -mguard=cf -c "%SQLITE_DIR%\sqlite3.c" -o "%SQLITE_OBJ%"
if errorlevel 1 (
    echo error: Viewer SQLite compilation failed.
    exit /b 1
)

echo [2/3] Generating single Atlas viewer translation unit...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\viewer\build_atlas_source.ps1" -Base "%ROOT%\viewer\viewer_atlas.cpp" -Delta "%ROOT%\viewer\viewer_atlas_delta.cpp" -Output "%ATLAS_SOURCE%"
if errorlevel 1 (
    echo error: Atlas source generation failed.
    exit /b 1
)

echo [3/3] Building Nylon Full Atlas Viewer...
"%CXX%" --target=x86_64-w64-mingw32 -std=c++20 -O2 -DNDEBUG -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -Wall -Wextra -Wpedantic -Werror -fstack-protector-strong -ffunction-sections -fdata-sections -mguard=cf -fuse-ld=lld -municode -static -Wl,--gc-sections -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va -I"%SQLITE_DIR%" -I"%ROOT%\viewer" -include "%ROOT%\viewer\sqlite_prepare.hpp" "%ATLAS_SOURCE%" "%SQLITE_OBJ%" -lws2_32 -lshell32 -o "%BIN%\nylon-viewer.exe"
if errorlevel 1 (
    echo error: Nylon Viewer build failed.
    exit /b 1
)

if not exist "%BIN%\nylon-viewer.exe" (
    echo error: Viewer build finished without producing nylon-viewer.exe.
    exit /b 1
)

>"%VIEWER_MARKER%" echo full-atlas-v2-delta-status
if errorlevel 1 (
    echo error: Unable to mark full-atlas viewer build.
    exit /b 1
)

echo.
echo Viewer build complete: %BIN%\nylon-viewer.exe
echo Run: view.cmd
echo Or:  bin\nylon-viewer.exe ^<path-to-image.db^>
exit /b 0
