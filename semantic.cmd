@echo off
setlocal EnableExtensions

cd /d "%~dp0"
set "ROOT=%CD%"
set "DB=%~1"
if "%DB%"=="" set "DB=%ROOT%\output\image.db"
set "SEMANTIC=%ROOT%\bin\nylon-semantic.exe"

if not exist "%SEMANTIC%" (
    echo Nylon Semantic Lifter is not built yet. Building it now...
    call "%ROOT%\build-semantic.cmd"
    if errorlevel 1 exit /b 1
)

if "%LOCALAPPDATA%"=="" (
    echo error: LOCALAPPDATA is not available.
    exit /b 1
)
set "SEMTOOLS=%LOCALAPPDATA%\Temp\nylon\tools\semantic"
if not exist "%SEMTOOLS%\llama\b11146\llama-server.exe" goto setup
if not exist "%SEMTOOLS%\models\qwen3.5-4b-q8_0\Qwen3.5-4B-Q8_0.gguf" goto setup
if not exist "%SEMTOOLS%\models\qwen3.5-4b-q8_0\.nylon-verified" goto setup
goto run

:setup
call "%ROOT%\semantic-setup.cmd"
if errorlevel 1 exit /b 1

:run
"%SEMANTIC%" "%DB%"
exit /b %ERRORLEVEL%
