@echo off
rem Domdly build script for Windows (MSVC).
rem Usage: build.bat [release ^| debug ^| test ^| cover ^| clean]
rem Requires Visual Studio 2019+ or the free "Build Tools for Visual Studio"
rem with the "Desktop development with C++" workload. No other dependencies.

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "MODE=%~1"
if "%MODE%"=="" set "MODE=release"

if /i "%MODE%"=="clean" (
    if exist build\windows rmdir /s /q build\windows
    echo Cleaned.
    exit /b 0
)

rem --- Locate the compiler -------------------------------------------------
where cl >nul 2>nul
if not errorlevel 1 goto have_cl

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto no_vs
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 goto no_vs
where cl >nul 2>nul
if errorlevel 1 goto no_vs

:have_cl
if not exist build\windows\obj mkdir build\windows\obj

set "CORE=src\core\mdbuf.c src\core\markdown.c src\core\render_html.c src\core\render_rtf.c src\core\md_write.c src\core\md_html.c"
set "WARN=/W4 /wd4100 /wd4996"
set "COMMON=/nologo /std:c11 /utf-8 %WARN% /Isrc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN"

if /i "%MODE%"=="cover" (
    rem Draws docs\images\cover.png, the README banner and social preview.
    cl /nologo /O2 /W4 /utf-8 /DUNICODE /D_UNICODE tools\mkcover.c /Fo:build\windows\obj\ /Fe:build\windows\mkcover.exe gdi32.lib gdiplus.lib >build\windows\mkcover_build.log
    if errorlevel 1 ( type build\windows\mkcover_build.log & echo Cover tool build failed. & exit /b 1 )
    build\windows\mkcover.exe docs\images\cover.png
    if errorlevel 1 exit /b 1
    echo Done: docs\images\cover.png
    exit /b 0
)

if /i "%MODE%"=="debug" (
    set "OPT=/Od /Zi /MTd /D_DEBUG"
    set "LOPT=/DEBUG"
) else (
    set "OPT=/O2 /MT /GS /guard:cf /DNDEBUG"
    set "LOPT=/OPT:REF /OPT:ICF /GUARD:CF"
)

rem --- Tests (portable core only) ------------------------------------------
echo [1/5] Building and running core tests...
cl %COMMON% %OPT% tests\test_markdown.c %CORE% /Fo:build\windows\obj\ /Fe:build\windows\test_markdown.exe /link %LOPT% >build\windows\test_build.log
if errorlevel 1 ( type build\windows\test_build.log & echo Test build failed. & exit /b 1 )
build\windows\test_markdown.exe
if errorlevel 1 ( echo Tests failed. & exit /b 1 )
if /i "%MODE%"=="test" exit /b 0

rem --- Command line tool ----------------------------------------------------
echo [2/5] Building md2html.exe...
cl %COMMON% %OPT% tools\md2html.c %CORE% /Fo:build\windows\obj\ /Fe:build\windows\md2html.exe /link %LOPT% >build\windows\md2html_build.log
if errorlevel 1 ( type build\windows\md2html_build.log & echo md2html build failed. & exit /b 1 )

rem --- Icon -----------------------------------------------------------------
echo [3/5] Generating icon...
cl /nologo /O2 /W4 tools\mkicon.c /Fo:build\windows\obj\ /Fe:build\windows\mkicon.exe >build\windows\mkicon_build.log
if errorlevel 1 ( type build\windows\mkicon_build.log & echo Icon tool build failed. & exit /b 1 )
build\windows\mkicon.exe build\windows\Domdly.ico
if errorlevel 1 ( echo Icon generation failed. & exit /b 1 )

rem --- Resources ------------------------------------------------------------
echo [4/5] Compiling resources...
rc /nologo /i build\windows /i src /fo build\windows\Domdly.res src\platform\win32\Domdly.rc
if errorlevel 1 ( echo Resource compilation failed. & exit /b 1 )

rem --- Application ----------------------------------------------------------
echo [5/5] Building Domdly.exe...
set "APP=src\platform\win32\main.c src\platform\win32\install.c src\platform\win32\settings.c src\platform\win32\util.c src\platform\win32\olecb.c"
set "LIBS=user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib dwmapi.lib uxtheme.lib uuid.lib msimg32.lib winhttp.lib"
cl %COMMON% %OPT% %APP% %CORE% build\windows\Domdly.res /Fo:build\windows\obj\ /Fe:build\windows\Domdly.exe /link %LOPT% /SUBSYSTEM:WINDOWS /MANIFEST:NO %LIBS%
if errorlevel 1 ( echo Build failed. & exit /b 1 )

echo.
echo Done: build\windows\Domdly.exe
exit /b 0

:no_vs
echo error: MSVC was not found.
echo Install "Build Tools for Visual Studio" with the C++ workload, or run this
echo script from a "x64 Native Tools Command Prompt for VS".
exit /b 1
