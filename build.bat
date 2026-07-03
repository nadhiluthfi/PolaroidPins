@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
rc /nologo resource.rc
cl /nologo /O2 /W3 /EHsc /DUNICODE /D_UNICODE main.cpp /link resource.res /SUBSYSTEM:WINDOWS user32.lib gdi32.lib /OUT:PolaroidPins.exe
if %errorlevel%==0 echo Build OK: PolaroidPins.exe
