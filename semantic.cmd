@echo off
setlocal EnableExtensions

cd /d "%~dp0"
set "ROOT=%CD%"
set "DB=%~1"
if "%DB%"=="" set "DB=%ROOT%\output\image.db"
set "SEMANTIC=%ROOT%\bin\nylon-semantic.exe"
set "CLASSIFIER=%ROOT%\bin\nylon-semantic-classify.exe"
set "LINKER=%ROOT%\bin\nylon-semantic-link.exe"
set "SEMANTIC_MARKER=%ROOT%\bin\nylon-semantic.identity-v1"
set "VIEWER=%ROOT%\bin\nylon-viewer.exe"
set "VIEWER_MARKER=%ROOT%\bin\nylon-viewer.atlas-v2"

if not exist "%SEMANTIC%" goto build_semantic
if not exist "%CLASSIFIER%" goto build_semantic
if not exist "%LINKER%" goto build_semantic
if not exist "%SEMANTIC_MARKER%" goto build_semantic
goto semantic_built

:build_semantic
echo Nylon Semantic Lifter is missing or predates the optimized runtime. Building it now...
call "%ROOT%\build-semantic.cmd"
if errorlevel 1 exit /b 1

:semantic_built
if not exist "%VIEWER%" goto build_viewer
if not exist "%VIEWER_MARKER%" goto build_viewer
goto viewer_built

:build_viewer
echo Nylon Viewer is missing or predates the optimized full Atlas interface. Rebuilding it now...
call "%ROOT%\build-viewer.cmd"
if errorlevel 1 exit /b 1

:viewer_built
rem First invocation creates the work plan only when one does not already exist.
rem Compatible resumptions reuse the persisted plan instead of rescanning image.db.
"%SEMANTIC%" "%DB%" --plan-only
if errorlevel 1 exit /b 1

rem Mark additional high-volume Windows namespaces as deterministic.
"%CLASSIFIER%" "%DB%"
if errorlevel 1 exit /b 1

rem Materialize newly classified work without rebuilding the plan or loading the model.
"%SEMANTIC%" "%DB%" --deterministic-only
if errorlevel 1 exit /b 1

rem Atlas opens with every free deterministic unit already committed.
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
rem The optimized semantic runtime keeps llama.cpp resident while it performs
rem its retry rounds. There is deliberately no outer retry loop here.
"%SEMANTIC%" "%DB%"
if errorlevel 1 exit /b %ERRORLEVEL%

echo Building deterministic semantic hierarchy...
"%LINKER%" "%DB%"
exit /b %ERRORLEVEL%
