@echo off
cd /d "%~dp0"
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /EHsc /W3 /MD /std:c++17 spout_adapter_check.cpp /Fe:spout_adapter_check.exe /link d3d11.lib dxgi.lib
if errorlevel 1 exit /b 1
endlocal
echo built.
