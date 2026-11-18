@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem Detecta la instalacion de Visual Studio sin fijar BuildTools/Community/Enterprise.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
)
if defined VSROOT (
    call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
    if errorlevel 1 goto :error
) else (
    rem Permite ejecutar el BAT desde una Developer Command Prompt configurada.
    where cl >nul 2>&1
    if errorlevel 1 goto :toolchain_error
)
if not exist dist mkdir dist

cl /nologo /W3 /MT /DZ7_ST /D_7ZIP_ST /DZ7_NO_METHODS_FILTERS /I lzma_sdk /TC /c lzma_sdk\7zAlloc.c lzma_sdk\7zArcIn.c lzma_sdk\7zBuf.c lzma_sdk\7zCrc.c lzma_sdk\7zCrcOpt.c lzma_sdk\7zDec.c lzma_sdk\7zStream.c lzma_sdk\CpuArch.c lzma_sdk\Bcj2.c lzma_sdk\Lzma2Dec.c lzma_sdk\LzmaDec.c /Fo:dist\
if errorlevel 1 goto :error
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /MT /DUNICODE /D_UNICODE native_editor.cpp gzip_codec.cpp report_text.cpp sevenzip_codec.cpp quick_results.cpp dist\7zAlloc.obj dist\7zArcIn.obj dist\7zBuf.obj dist\7zCrc.obj dist\7zCrcOpt.obj dist\7zDec.obj dist\7zStream.obj dist\CpuArch.obj dist\Bcj2.obj dist\Lzma2Dec.obj dist\LzmaDec.obj /link /SUBSYSTEM:WINDOWS user32.lib ole32.lib comctl32.lib comdlg32.lib gdi32.lib /OUT:dist\OCCEdit_v7_x64.exe
if errorlevel 1 goto :error

echo Compilacion terminada: dist\OCCEdit_v7_x64.exe
if /I not "%~1"=="/quiet" pause
exit /b 0

:toolchain_error
echo ERROR: Instala Visual Studio 2022 Build Tools con C++ x64 y Windows SDK, o abre la consola de desarrollador x64.
if /I not "%~1"=="/quiet" pause
exit /b 1

:error
echo ERROR: No se pudo compilar. Revisa el error anterior.
if /I not "%~1"=="/quiet" pause
exit /b 1
