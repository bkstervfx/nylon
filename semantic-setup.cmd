@echo off
setlocal EnableExtensions EnableDelayedExpansion

cd /d "%~dp0"

if "%LOCALAPPDATA%"=="" (
    echo error: LOCALAPPDATA is not available.
    exit /b 1
)

where curl.exe >nul 2>nul
if errorlevel 1 (
    echo error: curl.exe is required to download the local semantic runtime and model.
    exit /b 1
)
where powershell.exe >nul 2>nul
if errorlevel 1 (
    echo error: Windows PowerShell is required to unpack the semantic runtime.
    exit /b 1
)

set "NYLON_HOME=%LOCALAPPDATA%\Temp\nylon"
set "TOOLS=%NYLON_HOME%\tools\semantic"
set "LLAMA_DIR=%TOOLS%\llama\b11146"
set "MODEL_DIR=%TOOLS%\models\qwen3.5-4b-q8_0"
set "DOWNLOADS=%TOOLS%\downloads"
set "RUNTIME_ZIP=%DOWNLOADS%\llama-b11146-bin-win-vulkan-x64.zip"
set "RUNTIME_URL=https://github.com/ggml-org/llama.cpp/releases/download/b11146/llama-b11146-bin-win-vulkan-x64.zip"
set "RUNTIME_SHA=55A378AA095B466979D85075234F66D7655C7A7483222AF0C006C0E55B4D7BD6"
set "MODEL=%MODEL_DIR%\Qwen3.5-4B-Q8_0.gguf"
set "MODEL_URL=https://huggingface.co/SoAIHQ/Qwen3.5-4B-GGUF/resolve/main/Qwen3.5-4B-Q8_0.gguf"
set "MODEL_SHA=5B25856E42534B000B5E06D7CFC10C756931F4DC66A4CA6EF08D835BD233A833"
set "VERIFIED=%MODEL_DIR%\.nylon-verified"

if not exist "%TOOLS%" mkdir "%TOOLS%"
if not exist "%DOWNLOADS%" mkdir "%DOWNLOADS%"
if not exist "%MODEL_DIR%" mkdir "%MODEL_DIR%"

if exist "%LLAMA_DIR%\llama-server.exe" goto runtime_ready

echo Downloading pinned llama.cpp Vulkan runtime b11146...
curl.exe --fail --location --retry 3 --retry-delay 2 --output "%RUNTIME_ZIP%.part" "%RUNTIME_URL%"
if errorlevel 1 (
    del /q "%RUNTIME_ZIP%.part" >nul 2>nul
    echo error: Unable to download llama.cpp runtime.
    exit /b 1
)
move /y "%RUNTIME_ZIP%.part" "%RUNTIME_ZIP%" >nul
for /f "usebackq delims=" %%H in (`powershell.exe -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath '%RUNTIME_ZIP%').Hash"`) do set "ACTUAL_RUNTIME_SHA=%%H"
if /i not "!ACTUAL_RUNTIME_SHA!"=="%RUNTIME_SHA%" (
    echo error: llama.cpp runtime SHA-256 mismatch.
    del /q "%RUNTIME_ZIP%" >nul 2>nul
    exit /b 1
)
if exist "%LLAMA_DIR%" rmdir /s /q "%LLAMA_DIR%"
mkdir "%LLAMA_DIR%"
powershell.exe -NoProfile -Command "Expand-Archive -LiteralPath '%RUNTIME_ZIP%' -DestinationPath '%LLAMA_DIR%' -Force"
if errorlevel 1 (
    echo error: Unable to unpack llama.cpp runtime.
    exit /b 1
)
if not exist "%LLAMA_DIR%\llama-server.exe" (
    echo error: llama-server.exe was not present in the pinned runtime archive.
    exit /b 1
)

:runtime_ready
if exist "%MODEL%" if exist "%VERIFIED%" goto model_ready

if exist "%MODEL%" (
    echo Verifying existing Qwen3.5-4B Q8_0 model...
    for /f "usebackq delims=" %%H in (`powershell.exe -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath '%MODEL%').Hash"`) do set "ACTUAL_MODEL_SHA=%%H"
    if /i "!ACTUAL_MODEL_SHA!"=="%MODEL_SHA%" (
        >"%VERIFIED%" echo %MODEL_SHA%
        goto model_ready
    )
    echo Existing model failed verification; downloading the pinned file again.
    del /q "%MODEL%" >nul 2>nul
)

echo Downloading Qwen3.5-4B Q8_0 ^(about 4.6 GB^)...
curl.exe --fail --location --retry 3 --retry-delay 2 --output "%MODEL%.part" "%MODEL_URL%"
if errorlevel 1 (
    del /q "%MODEL%.part" >nul 2>nul
    echo error: Unable to download the pinned semantic model.
    exit /b 1
)
for /f "usebackq delims=" %%H in (`powershell.exe -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath '%MODEL%.part').Hash"`) do set "ACTUAL_MODEL_SHA=%%H"
if /i not "!ACTUAL_MODEL_SHA!"=="%MODEL_SHA%" (
    echo error: semantic model SHA-256 mismatch.
    del /q "%MODEL%.part" >nul 2>nul
    exit /b 1
)
move /y "%MODEL%.part" "%MODEL%" >nul
>"%VERIFIED%" echo %MODEL_SHA%

:model_ready
echo Semantic runtime ready.
echo runtime: %LLAMA_DIR%\llama-server.exe
echo model:   %MODEL%
exit /b 0
