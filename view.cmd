@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "VIEWER=%CD%\bin\nylon-viewer.exe"
set "VIEWER_MARKER=%CD%\bin\nylon-viewer.atlas-v1"
set "DB=%~1"
if "%DB%"=="" set "DB=%CD%\output\image.db"

if not exist "%VIEWER%" goto build
if not exist "%VIEWER_MARKER%" goto build
goto built

:build
echo Nylon Viewer is missing or predates the full Atlas interface. Building it now...
call "%CD%\build-viewer.cmd"
if errorlevel 1 exit /b 1

:built
if not exist "%DB%" (
    echo error: Nylon audit database was not found:
    echo   %DB%
    exit /b 1
)

"%VIEWER%" "%DB%"
exit /b %ERRORLEVEL%
