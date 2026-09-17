@echo off
rem probe_dlssnr_host.exe -- feasibility probe for hosting the AMD DLSS-NR
rem runtime (the pack's version.dll proxy) inside our own D3D12 process.
rem Experiment, not shipped: probe_dlssnr_host.cpp says what it answers.
cd /d "%~dp0"
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [amd] MSVC 2022 Build Tools not found - install them first.
    exit /b 1
)
cl /nologo /O2 /EHsc /W3 /MD /std:c++17 probe_dlssnr_host.cpp ^
   /Fe:probe_dlssnr_host.exe ^
   /link user32.lib d3d12.lib dxgi.lib version.lib ffx_upscaler_dx12.lib
if errorlevel 1 exit /b 1
endlocal
echo probe built.
