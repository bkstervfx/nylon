@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "VIEWER=%CD%\bin\nylon-viewer.exe"
set "DB=%~1"
if "%DB%"=="" set "DB=%CD%\output\image.db"

if not exist "%VIEWER%" (
    echo Nylon Viewer is not built yet. Building it now...
    call "%CD%\build-viewer.cmd"
    if errorlevel 1 exit /b 1
)

if not exist "%DB%" (
    echo error: Nylon audit database was not found:
    echo   %DB%
    exit /b 1
)

"%VIEWER%" "%DB%"
exit /b %ERRORLEVEL%
