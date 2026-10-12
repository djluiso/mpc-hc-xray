@echo off
rem Builds the X-Ray plugin (XRay.dll) for MpcPluginHost
rem   build.bat [x64^|x86] [PluginsFolder]
rem Output: bin\<arch>\XRay\XRay.dll. When PluginsFolder is given (the Plugins folder next to
rem MpcPluginHost.exe), the XRay folder is copied there too (changed files only); a running host reloads it automatically.
rem Optional: set FFMPEG_DIR to a folder with the LAV Filters FFmpeg DLLs (avcodec-lav-*.dll, ...) matching
rem the major versions of third_party\ffmpeg (see third_party\README.md) to ship them in XRay\ffmpeg.
rem Needs Visual Studio 2022 (or its Build Tools) with the C++ workload.

setlocal
set ARCH=%~1
if "%ARCH%"=="" set ARCH=x64
if /i "%ARCH%"=="x64" (set VCVARS=vcvars64.bat) else (set VCVARS=vcvars32.bat)
set INSTALL=%~2

set ROOT=%~dp0
set OUT=%ROOT%bin\%ARCH%\XRay
set OBJ=%ROOT%bin\obj\%ARCH%

if defined VSINSTALLDIR goto :vsready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VSDIR=
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" (echo Visual Studio with C++ tools not found& exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\%VCVARS%" >nul || exit /b 1
:vsready

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"

echo Building XRay.dll
cl /nologo /std:c++17 /EHsc /O2 /W3 /MT /MP /Zi /DNOMINMAX /DUNICODE /D_UNICODE /LD ^
   /I"%ROOT%third_party" /I"%ROOT%third_party\ffmpeg" /Fo"%OBJ%\\" /Fd"%OBJ%\\" "%ROOT%src\*.cpp" /Fe"%OUT%\XRay.dll" ^
   /link /DEBUG /OPT:REF /OPT:ICF /PDB:"%OUT%\XRay.pdb" user32.lib gdi32.lib advapi32.lib ole32.lib || exit /b 1
del "%OUT%\XRay.lib" "%OUT%\XRay.exp" 2>nul

if defined FFMPEG_DIR (
    if not exist "%OUT%\ffmpeg" mkdir "%OUT%\ffmpeg"
    for %%d in (avutil avcodec avformat swscale swresample) do copy /y "%FFMPEG_DIR%\%%d-lav-*.dll" "%OUT%\ffmpeg\" >nul || exit /b 1
)

if not "%INSTALL%"=="" (
    echo Installing to %INSTALL%\XRay
    xcopy /e /i /y /q /d "%OUT%" "%INSTALL%\XRay" >nul || exit /b 1
)

echo Done: %OUT%
endlocal