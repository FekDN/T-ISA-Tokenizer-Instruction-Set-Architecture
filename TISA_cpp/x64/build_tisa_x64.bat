@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul 2>&1
:: ============================================================================
:: build_tisa_x64.bat -- TISA VM x64 Test Runner Build Script
::
:: Runs from ordinary cmd.exe -- no special environment needed.
::
:: Compiler priority (MinGW first):
::   1. MinGW-w64  -- g++.exe already in PATH
::   2. MinGW-w64  -- MSYS2 found in standard install locations
::   3. MinGW-w64  -- MSYS2 installed automatically (winget or PowerShell)
::   4. MSVC       -- cl.exe already in PATH (Developer Command Prompt)
::   5. MSVC       -- Visual Studio found via vswhere
::
:: Usage:
::   build_tisa_x64.bat              -- Release (default)
::   build_tisa_x64.bat debug        -- Debug
::   build_tisa_x64.bat release      -- Release (explicit)
::   build_tisa_x64.bat clean        -- delete build folder
:: ============================================================================

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "BUILD_DIR=%SCRIPT_DIR%\build_windows"
set "BUILD_TYPE=Release"

set "ARG=%~1"
if /I "%ARG%"==""        goto :args_ok
if /I "%ARG%"=="release" set "BUILD_TYPE=Release" & goto :args_ok
if /I "%ARG%"=="debug"   set "BUILD_TYPE=Debug"   & goto :args_ok
if /I "%ARG%"=="clean" (
    echo [build] Removing %BUILD_DIR% ...
    if exist "%BUILD_DIR%" rd /s /q "%BUILD_DIR%"
    echo [build] Done.
    exit /b 0
)
echo Usage: build_tisa_x64.bat [release ^| debug ^| clean]
exit /b 1
:args_ok

echo.
echo ============================================================
echo  TISA VM x64 Test Runner -- Windows Build  [%BUILD_TYPE%]
echo ============================================================
echo.

set "CMAKE_EXE="
set "CMAKE_GENERATOR="
set "CMAKE_EXTRA="
set "TOOLCHAIN_LABEL="
set "MSYS2_ROOT="

:: ============================================================================
:: 1. FIND CMAKE
:: ============================================================================
echo [build] --- Step 1/2: Locating cmake ---

where cmake.exe >nul 2>&1
if not errorlevel 1 (
    for /f "delims=" %%C in ('where cmake.exe 2^>nul') do (
        if not defined CMAKE_EXE set "CMAKE_EXE=%%C"
    )
)

if not defined CMAKE_EXE (
    for %%P in (
        "%ProgramFiles%\CMake\bin\cmake.exe"
        "%ProgramFiles(x86)%\CMake\bin\cmake.exe"
        "C:\CMake\bin\cmake.exe"
        "D:\CMake\bin\cmake.exe"
        "%LOCALAPPDATA%\Programs\CMake\bin\cmake.exe"
        "%USERPROFILE%\cmake\bin\cmake.exe"
    ) do (
        if not defined CMAKE_EXE if exist "%%~P" (
            set "CMAKE_EXE=%%~P"
            set "PATH=%%~dpP;%PATH%"
        )
    )
)

if not defined CMAKE_EXE (
    for %%R in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
        if not defined CMAKE_EXE if exist "%%~R\Microsoft Visual Studio" (
            for /f "delims=" %%F in (
                'dir /s /b "%%~R\Microsoft Visual Studio\CMake\bin\cmake.exe" 2^>nul'
            ) do (
                if not defined CMAKE_EXE (
                    set "CMAKE_EXE=%%F"
                    set "PATH=%%~dpF;%PATH%"
                )
            )
        )
    )
)

if not defined CMAKE_EXE (
    where winget.exe >nul 2>&1
    if not errorlevel 1 (
        echo [build] cmake not found -- installing via winget...
        winget install --id Kitware.CMake --silent ^
              --accept-package-agreements --accept-source-agreements
        for %%P in (
            "%ProgramFiles%\CMake\bin\cmake.exe"
            "%ProgramFiles(x86)%\CMake\bin\cmake.exe"
        ) do (
            if not defined CMAKE_EXE if exist "%%~P" (
                set "CMAKE_EXE=%%~P"
                set "PATH=%%~dpP;%PATH%"
            )
        )
    )
)

if not defined CMAKE_EXE (
    echo.
    echo [build] ERROR: cmake not found.
    echo   Install: winget install Kitware.CMake
    echo        or: https://cmake.org/download/
    exit /b 1
)
echo [build] cmake: %CMAKE_EXE%

:: ============================================================================
:: 2. FIND / INSTALL C++ COMPILER
:: ============================================================================
echo [build] --- Step 2/2: Locating C++ compiler ---

where g++.exe >nul 2>&1
if not errorlevel 1 (
    set "CMAKE_GENERATOR=MinGW Makefiles"
    set "TOOLCHAIN_LABEL=MinGW-w64 (g++ in PATH)"
    where ninja.exe >nul 2>&1
    if not errorlevel 1 (
        set "CMAKE_GENERATOR=Ninja"
        set "TOOLCHAIN_LABEL=MinGW-w64 + Ninja"
    )
    goto :compile
)

for %%P in ("C:\msys64" "C:\msys2" "D:\msys64" "D:\msys2") do (
    if not defined MSYS2_ROOT if exist "%%~P\usr\bin\bash.exe" set "MSYS2_ROOT=%%~P"
)
if not defined MSYS2_ROOT if exist "%USERPROFILE%\msys64\usr\bin\bash.exe" set "MSYS2_ROOT=%USERPROFILE%\msys64"
if not defined MSYS2_ROOT if exist "%LOCALAPPDATA%\msys64\usr\bin\bash.exe" set "MSYS2_ROOT=%LOCALAPPDATA%\msys64"
if defined MSYS2_ROOT goto :msys2_use

echo [build] MinGW-w64 not found. Installing MSYS2...
where winget.exe >nul 2>&1
if not errorlevel 1 (
    echo [build] Installing MSYS2 via winget...
    winget install --id MSYS2.MSYS2 --silent ^
          --accept-package-agreements --accept-source-agreements
    if exist "C:\msys64\usr\bin\bash.exe" (
        set "MSYS2_ROOT=C:\msys64"
        goto :msys2_use
    )
)
echo [build] Downloading MSYS2 installer...
set "INST=%TEMP%\msys2-installer.exe"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Invoke-WebRequest 'https://github.com/msys2/msys2-installer/releases/latest/download/msys2-x86_64-latest.exe' -OutFile '!INST!' -UseBasicParsing"
if exist "!INST!" (
    "!INST!" install --root "C:\msys64" --confirm-command --accept-messages
    if exist "C:\msys64\usr\bin\bash.exe" (
        set "MSYS2_ROOT=C:\msys64"
        goto :msys2_use
    )
)

echo [build] MSYS2 install failed. Checking for MSVC...
where cl.exe >nul 2>&1
if not errorlevel 1 (
    set "TOOLCHAIN_LABEL=MSVC (cl.exe in PATH)"
    set "CMAKE_GENERATOR=NMake Makefiles"
    goto :compile
)

set "VSWHERE="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"      set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if defined VSWHERE (
    for /f "usebackq delims=" %%I in (
        `"!VSWHERE!" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`
    ) do set "VS_DIR=%%I"
    if defined VS_DIR (
        for %%V in (18 17 16 15) do (
            if not defined CMAKE_GENERATOR (
                if "%%V"=="18" set "_YR=2026"
                if "%%V"=="17" set "_YR=2022"
                if "%%V"=="16" set "_YR=2019"
                if "%%V"=="15" set "_YR=2017"
                if defined _YR (
                    "%CMAKE_EXE%" --help >"%TEMP%\cmake_help.txt" 2>nul
                    findstr /i "Visual Studio %%V !_YR!" "%TEMP%\cmake_help.txt" >nul 2>&1
                    if not errorlevel 1 (
                        set "CMAKE_GENERATOR=Visual Studio %%V !_YR!"
                        set "CMAKE_EXTRA=-A x64"
                        set "TOOLCHAIN_LABEL=MSVC (Visual Studio %%V !_YR!)"
                    )
                    del "%TEMP%\cmake_help.txt" >nul 2>&1
                )
            )
        )
        if defined CMAKE_GENERATOR goto :compile
    )
)

echo.
echo [build] ERROR: No C++ compiler found.
echo.
echo  Option A -- MinGW-w64 via MSYS2 (recommended):
echo    1. Install MSYS2: https://www.msys2.org/
echo    2. Open 'MSYS2 MinGW 64-bit' and run:
echo         pacman -Sy --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake
echo    3. Re-run this script.
echo.
echo  Option B -- Microsoft C++ Build Tools:
echo    https://aka.ms/vs/17/release/vs_BuildTools.exe
exit /b 1

:: ============================================================================
:: MSYS2
:: ============================================================================
:msys2_use
echo [build] MSYS2: %MSYS2_ROOT%
if not exist "%MSYS2_ROOT%\ucrt64\bin\g++.exe" (
    echo [build] Installing MinGW-w64 packages...
    "%MSYS2_ROOT%\usr\bin\bash.exe" -l -c ^
      "pacman -Sy --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja"
    if errorlevel 1 (
        echo.
        echo [build] ERROR: pacman failed.
        echo   Open 'MSYS2 MinGW 64-bit' and run:
        echo     pacman -Sy --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake
        exit /b 1
    )
)
set "PATH=%MSYS2_ROOT%\ucrt64\bin;%PATH%"
set "CMAKE_GENERATOR=MinGW Makefiles"
set "TOOLCHAIN_LABEL=MinGW-w64/UCRT (MSYS2: %MSYS2_ROOT%)"
if exist "%MSYS2_ROOT%\ucrt64\bin\ninja.exe" (
    set "CMAKE_GENERATOR=Ninja"
    set "TOOLCHAIN_LABEL=MinGW-w64/UCRT + Ninja (MSYS2: %MSYS2_ROOT%)"
)

:: ============================================================================
:: 3. BUILD
:: ============================================================================
:compile
echo.
echo [build] Toolchain  : %TOOLCHAIN_LABEL%
echo [build] Generator  : %CMAKE_GENERATOR%
echo [build] cmake      : %CMAKE_EXE%
echo [build] Build type : %BUILD_TYPE%
echo [build] Source     : %SCRIPT_DIR%
echo [build] Output     : %BUILD_DIR%
echo.

set "NEED_CLEAN=0"
set "CACHED_GEN="
set "CACHED_CXX="

if exist "%BUILD_DIR%\CMakeCache.txt" (
    for /f "tokens=2 delims==" %%V in (
        'findstr /i "CMAKE_GENERATOR:INTERNAL" "%BUILD_DIR%\CMakeCache.txt" 2^>nul'
    ) do set "CACHED_GEN=%%V"
    for /f "tokens=2 delims==" %%V in (
        'findstr /i "CMAKE_CXX_COMPILER:FILEPATH" "%BUILD_DIR%\CMakeCache.txt" 2^>nul'
    ) do set "CACHED_CXX=%%V"
)

if defined CACHED_GEN (
    set "CACHED_GEN=!CACHED_GEN: =!"
    if /I not "!CACHED_GEN!"=="%CMAKE_GENERATOR%" (
        echo [build] Generator changed: !CACHED_GEN! -^> %CMAKE_GENERATOR%
        set "NEED_CLEAN=1"
    )
)

set "GPP_TMP=%TEMP%\tisa_gpp.tmp"
set "CXX_TMP=%TEMP%\tisa_cxx.tmp"
where g++.exe >"%GPP_TMP%" 2>nul

if defined CACHED_CXX (
    set "CACHED_CXX=!CACHED_CXX: =!"
    echo !CACHED_CXX!>%CXX_TMP%
    findstr /i "mingw64" "%CXX_TMP%" >nul 2>&1
    if not errorlevel 1 (
        findstr /i "ucrt64" "%GPP_TMP%" >nul 2>&1
        if not errorlevel 1 ( echo [build] Compiler changed: mingw64 -^> ucrt64 & set "NEED_CLEAN=1" )
    )
    findstr /i "ucrt64" "%CXX_TMP%" >nul 2>&1
    if not errorlevel 1 (
        findstr /i "mingw64" "%GPP_TMP%" >nul 2>&1
        if not errorlevel 1 ( echo [build] Compiler changed: ucrt64 -^> mingw64 & set "NEED_CLEAN=1" )
    )
)
del "%GPP_TMP%" >nul 2>&1
del "%CXX_TMP%" >nul 2>&1

if "!NEED_CLEAN!"=="1" (
    echo [build] Clearing stale build cache: %BUILD_DIR%
    rd /s /q "%BUILD_DIR%"
)

"%CMAKE_EXE%" -G "%CMAKE_GENERATOR%" %CMAKE_EXTRA% ^
    -S "%SCRIPT_DIR%" ^
    -B "%BUILD_DIR%" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

if errorlevel 1 (
    echo.
    echo [build] ERROR: cmake configure failed.
    exit /b 1
)

set "JOBS=4"
for /f "tokens=2 delims==" %%N in (
    'wmic cpu get NumberOfLogicalProcessors /value 2^>nul ^| findstr "="'
) do (
    set "J=%%N"
    set "J=!J: =!"
    if not "!J!"=="" set "JOBS=!J!"
)

echo [build] Compiling with %JOBS% parallel jobs...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config %BUILD_TYPE% --parallel %JOBS%

if errorlevel 1 (
    echo.
    echo [build] ERROR: Build failed.
    exit /b 1
)

set "BIN=%BUILD_DIR%\tisa_test_x64.exe"
if not exist "%BIN%" if exist "%BUILD_DIR%\%BUILD_TYPE%\tisa_test_x64.exe" (
    set "BIN=%BUILD_DIR%\%BUILD_TYPE%\tisa_test_x64.exe"
)
if not exist "%BIN%" (
    echo [build] ERROR: tisa_test_x64.exe not found after build.
    exit /b 1
)

echo.
echo ============================================================
echo  Build successful!
echo  %BIN%
echo ============================================================
echo.
echo Required file layout (next to the .exe, or pass as args):
echo   models\model_map.txt
echo   models\^<hash^>\vocab.b
echo   models\^<hash^>\vocab_idx.b
echo   models\^<hash^>\merges.b   (BPE only)
echo   tisa_test_suite.bin
echo.
echo Usage:
echo   %BIN%
echo   %BIN% models tisa_test_suite.bin
echo.
echo Variants: build_tisa_x64.bat debug / clean

endlocal
exit /b 0
