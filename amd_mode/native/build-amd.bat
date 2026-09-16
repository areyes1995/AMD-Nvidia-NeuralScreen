@echo off
rem amd_nr_host.exe -- Phase 2a echo worker for the AMD backend.
rem Same stdin/stdout protocol as nvidia_mode/native (D5V3/FRM1/OUT1);
rem the neural pass is a passthrough until the HIP engine lands (2b).
rem CRT only: no NGX, no HIP, no D3D yet.
cd /d "%~dp0"
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [amd] MSVC 2022 Build Tools not found - install them first.
    exit /b 1
)

cl /nologo /O2 /EHsc /W3 /MD /std:c++17 nr_host_echo.cpp ^
   /Fe:amd_nr_host.exe ^
   /link kernel32.lib
if errorlevel 1 exit /b 1
endlocal
echo amd echo worker built.
