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
set "DOWNLOADS=%TOOLS%\downloads"
set "BUILD=%ROOT%\build"
set "BIN=%ROOT%\bin"
set "MEDIA_TOOLS=%TOOLS%\media"

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
set "SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9"
set "SQLITE_ZIP=%DOWNLOADS%\%SQLITE_ARCHIVE%"
set "SQLITE_DIR=%TOOLS%\sqlite"
set "SQLITE_EXTRACTED=%TOOLS%\sqlite-amalgamation-3540100"

set "SEVENZIP_VERSION=26.05"
set "SEVENZIP_BOOTSTRAP=7zr.exe"
set "SEVENZIP_BOOTSTRAP_URL=https://github.com/ip7z/7zip/releases/download/26.05/7zr.exe"
set "SEVENZIP_BOOTSTRAP_SHA256=9bf582b894dfa665f852d7e9112b02290a1c303c2d95a21fcdc41ffcb03143b7"
set "SEVENZIP_INSTALLER=7z2605-x64.exe"
set "SEVENZIP_INSTALLER_URL=https://github.com/ip7z/7zip/releases/download/26.05/7z2605-x64.exe"
set "SEVENZIP_INSTALLER_SHA256=fd99e86f57d8f429ef5ed045bf98315af962e2309b0f1ba87a147a44c71647d0"
set "SEVENZIP_BOOTSTRAP_PATH=%DOWNLOADS%\%SEVENZIP_BOOTSTRAP%"
set "SEVENZIP_INSTALLER_PATH=%DOWNLOADS%\%SEVENZIP_INSTALLER%"
set "SEVENZIP_DIR=%MEDIA_TOOLS%\7zip"

set "WIMLIB_VERSION=1.14.5"
set "WIMLIB_ARCHIVE=wimlib-1.14.5-windows-x86_64-bin.zip"
set "WIMLIB_URL=https://wimlib.net/downloads/wimlib-1.14.5-windows-x86_64-bin.zip"
set "WIMLIB_SHA256=2f446d6fa3866582175f1a22a7be198eeee0aec7aba5b4e04ad25c99eae2d265"
set "WIMLIB_EXE_SHA256=34c0c4165591ad1f592837ed99d08273c58d6ed3fe0ed6360cf34e7b0739b353"
set "WIMLIB_DLL_SHA256=ba853ee1e3fc5f5798581f02e8e066ba07a0a2375f0bf444fe981431fd508495"
set "WIMLIB_ZIP=%DOWNLOADS%\%WIMLIB_ARCHIVE%"
set "WIMLIB_DIR=%MEDIA_TOOLS%\wimlib"

if /I "%~1"=="clean" (
	if exist "%BUILD%" rmdir /s /q "%BUILD%"
	if exist "%BIN%" rmdir /s /q "%BIN%"
	echo Clean complete. Persistent tools and caches were kept in %NYLON_HOME%.
	exit /b 0
)

if /I not "%PROCESSOR_ARCHITECTURE%"=="AMD64" if /I not "%PROCESSOR_ARCHITEW6432%"=="AMD64" (
	echo error: Nylon's portable build currently targets x64 Windows only.
	exit /b 1
)

if not exist "%NYLON_HOME%" mkdir "%NYLON_HOME%"
if not exist "%TOOLS%" mkdir "%TOOLS%"
if not exist "%MEDIA_TOOLS%" mkdir "%MEDIA_TOOLS%"
if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%BIN%" mkdir "%BIN%"

call :ensure_llvm
if errorlevel 1 exit /b 1

call :ensure_sqlite
if errorlevel 1 exit /b 1

call :ensure_7zip
if errorlevel 1 exit /b 1

call :ensure_wimlib
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
>> "%RSP%" echo -lbcrypt
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
echo Persistent state: %NYLON_HOME%
echo Scan directory: bin\nylon.exe scan ^<expanded-windows-image^>
echo Scan ISO:       bin\nylon.exe scan ^<windows.iso^> [--index ^<number^>]
exit /b 0

:append_rsp_path
set "RSP_PATH=%~1"
set "RSP_PATH=%RSP_PATH:\=/%"
>> "%RSP%" echo "%RSP_PATH%"
exit /b 0

:require_command
where %~1 >nul 2>nul
if errorlevel 1 (
	echo error: %~1 is required for dependency bootstrap. It is included with Windows 11.
	exit /b 1
)
exit /b 0

:verify_sha256
call :require_command certutil.exe
if errorlevel 1 exit /b 1
certutil.exe -hashfile "%~1" SHA256 | findstr /I /C:"%~2" >nul
exit /b %ERRORLEVEL%

:download_verified
set "DV_PATH=%~1"
set "DV_URL=%~2"
set "DV_SHA=%~3"
set "DV_NAME=%~4"

if exist "%DV_PATH%" (
	call :verify_sha256 "%DV_PATH%" "%DV_SHA%"
	if not errorlevel 1 (
		echo Using cached download: %DV_NAME%
		exit /b 0
	)
	del /q "%DV_PATH%" >nul 2>nul
)

call :require_command curl.exe
if errorlevel 1 exit /b 1
if not exist "%DOWNLOADS%" mkdir "%DOWNLOADS%"

echo Downloading %DV_NAME%...
curl.exe --fail --location --retry 3 --connect-timeout 20 --output "%DV_PATH%.tmp" "%DV_URL%"
if errorlevel 1 (
	del /q "%DV_PATH%.tmp" >nul 2>nul
	echo error: %DV_NAME% download failed.
	exit /b 1
)
move /y "%DV_PATH%.tmp" "%DV_PATH%" >nul

call :verify_sha256 "%DV_PATH%" "%DV_SHA%"
if errorlevel 1 (
	echo error: %DV_NAME% SHA-256 verification failed.
	del /q "%DV_PATH%" >nul 2>nul
	exit /b 1
)
exit /b 0

:ensure_llvm
if exist "%LLVM_DIR%\bin\x86_64-w64-mingw32-clang++.exe" (
	echo Using cached LLVM-MinGW %LLVM_VERSION%.
	exit /b 0
)
if exist "%LLVM_DIR%\bin\clang++.exe" (
	echo Using cached LLVM-MinGW %LLVM_VERSION%.
	exit /b 0
)

call :download_verified "%LLVM_ZIP%" "%LLVM_URL%" "%LLVM_SHA256%" "LLVM-MinGW %LLVM_VERSION%"
if errorlevel 1 exit /b 1

call :require_command tar.exe
if errorlevel 1 exit /b 1

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
	findstr /I /C:"%SQLITE_SOURCE_ID%" "%SQLITE_DIR%\sqlite3.c" >nul
	if not errorlevel 1 (
		findstr /I /C:"%SQLITE_SOURCE_ID%" "%SQLITE_DIR%\sqlite3.h" >nul
		if not errorlevel 1 (
			echo Using cached SQLite %SQLITE_VERSION%.
			exit /b 0
		)
	)
	echo Cached SQLite sources failed identity validation; rebuilding cache.
)

if exist "%SQLITE_DIR%" rmdir /s /q "%SQLITE_DIR%"
if exist "%SQLITE_EXTRACTED%" rmdir /s /q "%SQLITE_EXTRACTED%"

call :download_verified "%SQLITE_ZIP%" "%SQLITE_URL%" "%SQLITE_SHA256%" "SQLite %SQLITE_VERSION% amalgamation"
if errorlevel 1 exit /b 1

call :require_command tar.exe
if errorlevel 1 exit /b 1

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
if not exist "%SQLITE_EXTRACTED%\sqlite3.h" (
	echo error: SQLite archive did not contain sqlite3.h.
	exit /b 1
)

mkdir "%SQLITE_DIR%" >nul 2>nul
copy /y "%SQLITE_EXTRACTED%\sqlite3.c" "%SQLITE_DIR%\sqlite3.c" >nul
copy /y "%SQLITE_EXTRACTED%\sqlite3.h" "%SQLITE_DIR%\sqlite3.h" >nul
rmdir /s /q "%SQLITE_EXTRACTED%"

findstr /I /C:"%SQLITE_SOURCE_ID%" "%SQLITE_DIR%\sqlite3.c" >nul
if errorlevel 1 (
	echo error: SQLite C source identity verification failed.
	rmdir /s /q "%SQLITE_DIR%"
	exit /b 1
)
findstr /I /C:"%SQLITE_SOURCE_ID%" "%SQLITE_DIR%\sqlite3.h" >nul
if errorlevel 1 (
	echo error: SQLite header identity verification failed.
	rmdir /s /q "%SQLITE_DIR%"
	exit /b 1
)
exit /b 0

:ensure_7zip
if exist "%SEVENZIP_DIR%\7z.exe" if exist "%SEVENZIP_DIR%\7z.dll" (
	echo Using cached 7-Zip %SEVENZIP_VERSION%.
	exit /b 0
)

call :download_verified "%SEVENZIP_BOOTSTRAP_PATH%" "%SEVENZIP_BOOTSTRAP_URL%" "%SEVENZIP_BOOTSTRAP_SHA256%" "7-Zip bootstrap %SEVENZIP_VERSION%"
if errorlevel 1 exit /b 1
call :download_verified "%SEVENZIP_INSTALLER_PATH%" "%SEVENZIP_INSTALLER_URL%" "%SEVENZIP_INSTALLER_SHA256%" "7-Zip x64 %SEVENZIP_VERSION%"
if errorlevel 1 exit /b 1

if exist "%SEVENZIP_DIR%" rmdir /s /q "%SEVENZIP_DIR%"
mkdir "%SEVENZIP_DIR%" >nul 2>nul

echo Extracting portable 7-Zip %SEVENZIP_VERSION%...
"%SEVENZIP_BOOTSTRAP_PATH%" x -y -o"%SEVENZIP_DIR%" "%SEVENZIP_INSTALLER_PATH%" >nul
if errorlevel 1 (
	echo error: Portable 7-Zip extraction failed.
	exit /b 1
)

if not exist "%SEVENZIP_DIR%\7z.exe" (
	echo error: Portable 7-Zip archive did not produce 7z.exe.
	exit /b 1
)
if not exist "%SEVENZIP_DIR%\7z.dll" (
	echo error: Portable 7-Zip archive did not produce 7z.dll.
	exit /b 1
)
exit /b 0

:ensure_wimlib
if exist "%WIMLIB_DIR%\wimlib-imagex.exe" if exist "%WIMLIB_DIR%\libwim-15.dll" (
	call :verify_sha256 "%WIMLIB_DIR%\wimlib-imagex.exe" "%WIMLIB_EXE_SHA256%"
	if not errorlevel 1 (
		call :verify_sha256 "%WIMLIB_DIR%\libwim-15.dll" "%WIMLIB_DLL_SHA256%"
		if not errorlevel 1 (
			echo Using cached wimlib %WIMLIB_VERSION%.
			exit /b 0
		)
	)
	echo Cached wimlib failed SHA-256 validation; rebuilding cache.
)

call :download_verified "%WIMLIB_ZIP%" "%WIMLIB_URL%" "%WIMLIB_SHA256%" "wimlib %WIMLIB_VERSION%"
if errorlevel 1 exit /b 1

call :require_command tar.exe
if errorlevel 1 exit /b 1

if exist "%WIMLIB_DIR%" rmdir /s /q "%WIMLIB_DIR%"
mkdir "%WIMLIB_DIR%" >nul 2>nul

echo Extracting portable wimlib %WIMLIB_VERSION%...
tar.exe -xf "%WIMLIB_ZIP%" -C "%WIMLIB_DIR%"
if errorlevel 1 (
	echo error: wimlib extraction failed.
	exit /b 1
)

if not exist "%WIMLIB_DIR%\wimlib-imagex.exe" (
	echo error: wimlib archive did not produce wimlib-imagex.exe.
	exit /b 1
)
if not exist "%WIMLIB_DIR%\libwim-15.dll" (
	echo error: wimlib archive did not produce libwim-15.dll.
	exit /b 1
)

call :verify_sha256 "%WIMLIB_DIR%\wimlib-imagex.exe" "%WIMLIB_EXE_SHA256%"
if errorlevel 1 (
	echo error: wimlib-imagex.exe SHA-256 verification failed after extraction.
	exit /b 1
)
call :verify_sha256 "%WIMLIB_DIR%\libwim-15.dll" "%WIMLIB_DLL_SHA256%"
if errorlevel 1 (
	echo error: libwim-15.dll SHA-256 verification failed after extraction.
	exit /b 1
)
exit /b 0