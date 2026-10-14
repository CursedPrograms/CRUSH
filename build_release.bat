@echo off
REM Builds dist\crush.exe (MSVC, static, with icon + version info), the portable zip and the
REM installer dist\CRUSH-3.0.0-setup.exe (Inno Setup 6).
REM Needs Visual Studio 2022 ("Desktop development with C++") and Inno Setup 6.
setlocal
cd /d "%~dp0"

set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
set ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe
if not exist "%VCVARS%" (echo Visual Studio 2022 not found & goto :fail)
call "%VCVARS%" >nul

if not exist dist mkdir dist
if not exist build mkdir build
rc /nologo /fo build\crush.res installer\crush.rc || goto :fail
cl /nologo /O2 /std:c++17 /EHsc /MT /utf-8 /Fobuild\ /Fedist\crush.exe crush.cpp build\crush.res || goto :fail

powershell -NoProfile -Command "Compress-Archive -Force -Path dist\crush.exe, dist\README.txt, LICENSE -DestinationPath dist\CRUSH-3.0.0-win64-portable.zip" || goto :fail

if not exist "%ISCC%" (echo Inno Setup 6 not found: skipping the installer & goto :done)
"%ISCC%" /Q installer\crush.iss || goto :fail

:done
echo.
echo Done:
echo   dist\crush.exe                       the tool
echo   dist\CRUSH-3.0.0-win64-portable.zip  no install needed
echo   dist\CRUSH-3.0.0-setup.exe           installer
exit /b 0

:fail
echo Build failed.
exit /b 1
