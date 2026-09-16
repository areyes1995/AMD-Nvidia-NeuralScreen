@echo off
rem NeuralScreen.exe -- the friendly launcher (icon, no console, message boxes).
rem Output goes to the repository root, next to main.py, because that is where
rem it has to sit to find everything.
cd /d "%~dp0"
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
rc /nologo /fo launcher.res launcher.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /W3 /MD launcher.cpp launcher.res ^
   /Fe:..\..\NeuralScreen.exe ^
   /link /SUBSYSTEM:WINDOWS shlwapi.lib user32.lib kernel32.lib
if errorlevel 1 exit /b 1
del launcher.obj launcher.res >nul 2>&1
endlocal
echo launcher built.
