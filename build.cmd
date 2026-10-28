@echo off
setlocal EnableExtensions

cd /d "%~dp0"
set "ROOT=%CD%"
set "TOOLS=%ROOT%\tools"
set "DOWNLOADS=%TOOLS%\downloads"
set "BUILD=%ROOT%\build"
set "BIN=%ROOT%\bin"

set "LLVM_VERSION=20261020"
set "LLVM_ARCHIVE=llvm-mingw-20261020-ucrt-x86_64.zip"
set "LLVM_URL=https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip"
set "LLVM_SHA256=fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd"
set "LLVM_DIR=%TOOLS%\llvm-mingw"
set "LLVM_EXTRACTED=%TOOLS%\llvm-mingw-20261020-ucrt-x86_64"
set "LLVM_ZIP=%DOWNLOADS%\%LLVM_ARCHIVE%"

set "SQLITE_VERSION=3.54.1"
set "SQLITE_SOURCE_ID=555e07ef33f69a0957d24d8e555b702464a131d6f1607f4c3c1e4551e8e6ca76"
set "SQLITE_ARCHIVE=sqlite-amalgamation-3540100.zip"
set "SQLITE_URL=https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip"
set "SQLITE_ZIP=%DOWNLOADS%\%SQLITE_ARCHIVE%"
set "SQLITE_DIR=%TOOLS%\sqlite"
set "SQLITE_EXTRACTED=%TOOLS%\sqlite-amalgamation-3540100"

if /I "%~1"=="clean" (
	if exist "%BUILD%" rmdir /s /q "%BUILD%"
	if exist "%BIN%" rmdir /s /q "%BIN%"
	echo Clean complete. Portable tools were kept.
	exit /b 0
)

if /I not "%PROCESSOR_ARCHITECTURE%"=="AMD64" if /I not "%PROCESSOR_ARCHITEW6432%"=="AMD64" (
	echo error: Nylon's portable build currently targets x64 Windows only.
	exit /b 1
)

for %%T in (curl.exe tar.exe certutil.exe) do (
	where %%T >nul 2>nul
	if errorlevel 1 (
		echo error: %%T is required. It is included with Windows 11.
		exit /b 1
	)
)

if not exist "%TOOLS%" mkdir "%TOOLS%"
if not exist "%DOWNLOADS%" mkdir "%DOWNLOADS%"
if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%BIN%" mkdir "%BIN%"

call :ensure_llvm
if errorlevel 1 exit /b 1

call :ensure_sqlite
if errorlevel 1 exit /b 1

set "CC=%LLVM_DIR%\bin\x86_64-w64-mingw32-clang.exe"
set "CXX=%LLVM_DIR%\bin\x86_64-w64-mingw32-clang++.exe"
if not exist "%CXX%" (
	set "CC=%LLVM_DIR%\bin\clang.exe"
	set "CXX=%LLVM_DIR%\bin\clang++.exe"
)

if not exist "%CXX%" (
	echo error: LLVM-MinGW was extracted but clang++ was not found.
	exit /b 1
)

set "SQLITE_OBJ=%BUILD%\sqlite3.o"
set "RSP=%BUILD%\nylon.rsp"

rem Clang response files treat backslashes as escape characters. Convert every
rem path written to the response file to forward slashes so the build works
rem from any Windows directory.
set "ROOT_RSP=%ROOT:\=/%"
set "SQLITE_DIR_RSP=%SQLITE_DIR:\=/%"
set "SQLITE_OBJ_RSP=%SQLITE_OBJ:\=/%"
set "NYLON_EXE_RSP=%BIN:\=/%/nylon.exe"

echo [1/2] Compiling SQLite %SQLITE_VERSION%...
"%CC%" --target=x86_64-w64-mingw32 -O2 -DNDEBUG -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -fstack-protector-strong -ffunction-sections -fdata-sections -mguard=cf -c "%SQLITE_DIR%\sqlite3.c" -o "%SQLITE_OBJ%"
if errorlevel 1 (
	echo error: SQLite compilation failed.
	exit /b 1
)

> "%RSP%" echo --target=x86_64-w64-mingw32
>> "%RSP%" echo -std=c++20
>> "%RSP%" echo -O2
>> "%RSP%" echo -DNDEBUG
>> "%RSP%" echo -DUNICODE
>> "%RSP%" echo -D_UNICODE
>> "%RSP%" echo -DWIN32_LEAN_AND_MEAN
>> "%RSP%" echo -DNOMINMAX
>> "%RSP%" echo -D_WIN32_WINNT=0x0A00
>> "%RSP%" echo -Wall
>> "%RSP%" echo -Wextra
>> "%RSP%" echo -Wpedantic
>> "%RSP%" echo -Werror
>> "%RSP%" echo -fstack-protector-strong
>> "%RSP%" echo -ffunction-sections
>> "%RSP%" echo -fdata-sections
>> "%RSP%" echo -mguard=cf
>> "%RSP%" echo -fuse-ld=lld
>> "%RSP%" echo -municode
>> "%RSP%" echo -static
>> "%RSP%" echo -Wl,--gc-sections
>> "%RSP%" echo -Wl,--dynamicbase
>> "%RSP%" echo -Wl,--nxcompat
>> "%RSP%" echo -Wl,--high-entropy-va
>> "%RSP%" echo -I"%ROOT_RSP%/src"
>> "%RSP%" echo -I"%SQLITE_DIR_RSP%"
for /r "%ROOT%\src" %%F in (*.cpp) do call :append_rsp_path "%%~fF"
>> "%RSP%" echo "%SQLITE_OBJ_RSP%"
>> "%RSP%" echo -o
>> "%RSP%" echo "%NYLON_EXE_RSP%"

echo [2/2] Building Nylon...
"%CXX%" @"%RSP%"
if errorlevel 1 (
	echo error: Nylon build failed.
	exit /b 1
)

if not exist "%BIN%\nylon.exe" (
	echo error: Build finished without producing nylon.exe.
	exit /b 1
)

echo.
echo Build complete: %BIN%\nylon.exe
echo Run: bin\nylon.exe scan ^<expanded-windows-image^>
exit /b 0

:append_rsp_path
set "RSP_PATH=%~1"
set "RSP_PATH=%RSP_PATH:\=/%"
>> "%RSP%" echo "%RSP_PATH%"
exit /b 0

:ensure_llvm
if exist "%LLVM_DIR%\bin\x86_64-w64-mingw32-clang++.exe" exit /b 0
if exist "%LLVM_DIR%\bin\clang++.exe" exit /b 0

if exist "%LLVM_ZIP%" (
	certutil.exe -hashfile "%LLVM_ZIP%" SHA256 | findstr /I /C:"%LLVM_SHA256%" >nul
	if errorlevel 1 del /q "%LLVM_ZIP%"
)

if not exist "%LLVM_ZIP%" (
	echo Downloading LLVM-MinGW %LLVM_VERSION%...
	curl.exe --fail --location --retry 3 --output "%LLVM_ZIP%.tmp" "%LLVM_URL%"
	if errorlevel 1 (
		del /q "%LLVM_ZIP%.tmp" >nul 2>nul
		echo error: LLVM-MinGW download failed.
		exit /b 1
	)
	move /y "%LLVM_ZIP%.tmp" "%LLVM_ZIP%" >nul
)

certutil.exe -hashfile "%LLVM_ZIP%" SHA256 | findstr /I /C:"%LLVM_SHA256%" >nul
if errorlevel 1 (
	echo error: LLVM-MinGW SHA-256 verification failed.
	del /q "%LLVM_ZIP%" >nul 2>nul
	exit /b 1
)

if exist "%LLVM_EXTRACTED%" rmdir /s /q "%LLVM_EXTRACTED%"
if exist "%LLVM_DIR%" rmdir /s /q "%LLVM_DIR%"

echo Extracting LLVM-MinGW...
tar.exe -xf "%LLVM_ZIP%" -C "%TOOLS%"
if errorlevel 1 (
	echo error: LLVM-MinGW extraction failed.
	exit /b 1
)

if not exist "%LLVM_EXTRACTED%" (
	echo error: LLVM-MinGW archive layout was not recognized.
	exit /b 1
)

move "%LLVM_EXTRACTED%" "%LLVM_DIR%" >nul
if errorlevel 1 (
	echo error: Unable to place LLVM-MinGW in tools\llvm-mingw.
	exit /b 1
)

exit /b 0

:ensure_sqlite
if exist "%SQLITE_DIR%\sqlite3.c" if exist "%SQLITE_DIR%\sqlite3.h" (
	findstr /I /C:"%SQLITE_SOURCE_ID%" "%SQLITE_DIR%\sqlite3.h" >nul
	if not errorlevel 1 exit /b 0
)

if exist "%SQLITE_DIR%" rmdir /s /q "%SQLITE_DIR%"
if exist "%SQLITE_EXTRACTED%" rmdir /s /q "%SQLITE_EXTRACTED%"

if not exist "%SQLITE_ZIP%" (
	echo Downloading SQLite %SQLITE_VERSION% amalgamation...
	curl.exe --fail --location --retry 3 --output "%SQLITE_ZIP%.tmp" "%SQLITE_URL%"
	if errorlevel 1 (
		del /q "%SQLITE_ZIP%.tmp" >nul 2>nul
		echo error: SQLite download failed.
		exit /b 1
	)
	move /y "%SQLITE_ZIP%.tmp" "%SQLITE_ZIP%" >nul
)

echo Extracting SQLite...
tar.exe -xf "%SQLITE_ZIP%" -C "%TOOLS%"
if errorlevel 1 (
	del /q "%SQLITE_ZIP%" >nul 2>nul
	echo error: SQLite extraction failed.
	exit /b 1
)

if not exist "%SQLITE_EXTRACTED%\sqlite3.c" (
	echo error: SQLite archive layout was not recognized.
	exit /b 1
)

mkdir "%SQLITE_DIR%" >nul 2>nul
copy /y "%SQLITE_EXTRACTED%\sqlite3.c" "%SQLITE_DIR%\sqlite3.c" >nul
copy /y "%SQLITE_EXTRACTED%\sqlite3.h" "%SQLITE_DIR%\sqlite3.h" >nul
rmdir /s /q "%SQLITE_EXTRACTED%"

findstr /I /C:"%SQLITE_SOURCE_ID%" "%SQLITE_DIR%\sqlite3.h" >nul
if errorlevel 1 (
	echo error: SQLite source identity verification failed.
	rmdir /s /q "%SQLITE_DIR%"
	exit /b 1
)

exit /b 0
