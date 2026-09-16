@echo off
rem NeuralScreen - DLSS 5 Neural Rendering overlay for the Windows desktop.
rem Consoles stays open: main.py logs FPS and pipeline timings into it.
setlocal
cd /d "%~dp0"

rem --- Python: env var -> bundled runtime -> usual installs -> launcher -----
rem NOTE: plain "python" on a fresh Windows is a Store stub, not an
rem interpreter - it must stay the LAST resort, otherwise the program
rem never starts (no UI, the console just sits there).
set "NS_PY=%NEURALSCREEN_PYTHON%"
if not defined NS_PY if exist "%~dp0runtime\python.exe" set "NS_PY=%~dp0runtime\python.exe"
if not defined NS_PY if exist "%LOCALAPPDATA%\Programs\Python\Python313\python.exe" set "NS_PY=%LOCALAPPDATA%\Programs\Python\Python313\python.exe"
if not defined NS_PY if exist "C:\Program Files\Python313\python.exe" set "NS_PY=C:\Program Files\Python313\python.exe"
if not defined NS_PY (
    where py >nul 2>&1 && py -3.13 -c "import sys" >nul 2>&1
    if not errorlevel 1 set "NS_PY=py -3.13"
)
if not defined NS_PY set "NS_PY=python"
echo [NeuralScreen] Python: %NS_PY%

rem --- NGX runtime: 165 MB redistributable, ships in the release archive -----
rem Degraded mode: without it the program still opens (menu/tray/capture)
rem with the neural pass disabled.
if not exist "%~dp0nvidia_mode\native\nvngx_dlssnr.dll" (
    echo [NeuralScreen] nvidia_mode\native\nvngx_dlssnr.dll not found - degraded mode,
    echo neural pass disabled. Re-download the release archive for full
    echo functionality, or see README.md, section "What you need".
)

rem --- Worker: build artefact, not stored in git ----------------------------
rem Degraded mode: without it the program still opens (menu/tray/capture)
rem with the neural pass disabled.
if not exist "%~dp0nvidia_mode\native\nvngx.dll" (
    echo [NeuralScreen] nvidia_mode\native\nvngx.dll not found - building it.
    call "%~dp0nvidia_mode\native\build-host.bat"
    if errorlevel 1 (
        echo [NeuralScreen] Worker build failed - degraded mode,
        echo neural pass disabled. See nvidia_mode\native\build-host.bat.
    )
)

rem NS_PY may be the launcher plus a version ("py -3.13"), which must
rem be called unquoted; a plain path is called quoted.
if "%NS_PY%"=="py -3.13" (
    py -3.13 -u "%~dp0main.py" %*
) else (
    "%NS_PY%" -u "%~dp0main.py" %*
)
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo.
    echo [NeuralScreen] exit code %RC%
    pause
)
exit /b %RC%
