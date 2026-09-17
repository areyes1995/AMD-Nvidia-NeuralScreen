@echo off
rem amd_nr_host.exe -- AMD backend worker (full-input since Phase 2b-prep).
rem Same stdin/stdout protocol as nvidia_mode/native (D5V3/FRM1/OUT1);
rem color + DIS motion + PaperWhite exposure + reset are real, and the neural
rem pass runs for real when the DLSS-NR HIP runtime sits next to the exe
rem (dlssnr_engine.cpp; see docs/AMD_HIP_HOSTING.md). Without it the worker
rem serves byte-identical passthrough, so this build has no hard dependency
rem on any of it: D3D12/DXGI are system DLLs, the runtime and the FidelityFX
rem upscaler are loaded at run time and may be absent.
rem nr_host_echo.cpp (Phase 2a) is kept for protocol regression tests.
cd /d "%~dp0"
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [amd] MSVC 2022 Build Tools not found - install them first.
    exit /b 1
)

cl /nologo /O2 /EHsc /W3 /MD /std:c++17 nr_host_full.cpp dlssnr_engine.cpp ^
   /Fe:amd_nr_host.exe ^
   /link kernel32.lib user32.lib d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
endlocal
echo amd full-input worker built.
