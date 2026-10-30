@echo off
setlocal EnableExtensions EnableDelayedExpansion

cd /d "%~dp0"
set "ROOT=%CD%"
set "DB=%~1"
if "%DB%"=="" set "DB=%ROOT%\output\image.db"
set "SEMANTIC=%ROOT%\bin\nylon-semantic.exe"
set "CLASSIFIER=%ROOT%\bin\nylon-semantic-classify.exe"
set "LINKER=%ROOT%\bin\nylon-semantic-link.exe"
set "SEMANTIC_MARKER=%ROOT%\bin\nylon-semantic.request-v3"
set "VIEWER=%ROOT%\bin\nylon-viewer.exe"
set "VIEWER_MARKER=%ROOT%\bin\nylon-viewer.atlas-v1"

if not exist "%SEMANTIC%" goto build_semantic
if not exist "%CLASSIFIER%" goto build_semantic
if not exist "%LINKER%" goto build_semantic
if not exist "%SEMANTIC_MARKER%" goto build_semantic
goto semantic_built

:build_semantic
echo Nylon Semantic Lifter is missing or predates request contract v3. Building it now...
call "%ROOT%\build-semantic.cmd"
if errorlevel 1 exit /b 1

:semantic_built
if not exist "%VIEWER%" goto build_viewer
if not exist "%VIEWER_MARKER%" goto build_viewer
goto viewer_built

:build_viewer
echo Nylon Viewer is missing or predates the full Atlas interface. Rebuilding it now...
call "%ROOT%\build-viewer.cmd"
if errorlevel 1 exit /b 1

:viewer_built
rem Build the exact work map, then classify high-volume Windows namespaces
rem deterministically before any model/runtime setup is required.
"%SEMANTIC%" "%DB%" --plan-only
if errorlevel 1 exit /b 1
"%CLASSIFIER%" "%DB%"
if errorlevel 1 exit /b 1

rem Commit every free deterministic lift before opening the Atlas.
"%SEMANTIC%" "%DB%" --plan-only
if errorlevel 1 exit /b 1

rem Atlas opens immediately and represents the complete work map while lifting continues.
start "Nylon Viewer" "%VIEWER%" "%DB%"

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
set /a "SEMANTIC_PASS=0"
:run_pass
set /a "SEMANTIC_PASS+=1"
"%SEMANTIC%" "%DB%"
set "SEMANTIC_RC=!ERRORLEVEL!"
if "!SEMANTIC_RC!"=="2" if !SEMANTIC_PASS! LSS 3 (
    echo Retrying incomplete semantic work ^(pass !SEMANTIC_PASS! of 3 complete^)...
    goto run_pass
)
if not "!SEMANTIC_RC!"=="0" exit /b !SEMANTIC_RC!

echo Building deterministic semantic hierarchy...
"%LINKER%" "%DB%"
exit /b !ERRORLEVEL!
