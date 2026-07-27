@echo off
rem ci_local.bat - Gleam local gate: clean rebuild + matrix x3 + full suite x1.
rem Usage: ci_local.bat [logdir]
rem Exit code 0 only if every stage passes. Logs archived per commit/timestamp.

setlocal EnableDelayedExpansion
set MSBUILD="C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
set BASH="D:\Program Files\Git\bin\bash.exe"
set ROOT=%~dp0
set LOGDIR=%1
if "%LOGDIR%"=="" set LOGDIR=%ROOT%ci_logs
for /f %%i in ('git -C %ROOT% rev-parse --short HEAD') do set COMMIT=%%i
set STAMP=%DATE:~0,4%%DATE:~5,2%%DATE:~8,2%_%TIME:~0,2%%TIME:~3,2%%TIME:~6,2%
set STAMP=%STAMP: =0%
set OUT=%LOGDIR%\%COMMIT%_%STAMP%
mkdir %OUT% 2>nul
set OUT_BASH=%OUT:\=/%

echo [ci] log dir: %OUT%

rem ---- record the exact source state the results belong to ----
git -C %ROOT% rev-parse HEAD > %OUT%\commit.txt
if errorlevel 1 ( echo [ci] FAIL: git HEAD & exit /b 1 )
git -C %ROOT% rev-parse "HEAD^{tree}" >> %OUT%\commit.txt
if errorlevel 1 ( echo [ci] FAIL: git tree & exit /b 1 )
git -C %ROOT% status --porcelain >> %OUT%\commit.txt
if errorlevel 1 ( echo [ci] FAIL: git status & exit /b 1 )

rem ---- stage 0: kill orphaned test processes (they lock the binaries) ----
powershell -NoProfile -Command "Get-Process -Name TestTarget,ArgvTarget,BoundaryTarget,gleam -ErrorAction SilentlyContinue | Stop-Process -Force" >nul 2>&1

rem ---- stage 1: clean rebuild Debug x64 ----
echo [ci] clean rebuild Debug x64 ...
%MSBUILD% %ROOT%GleeBug.sln -t:Clean -p:Configuration=Debug -p:Platform=x64 -m -v:m > %OUT%\build_clean_debug.log 2>&1
if errorlevel 1 ( echo [ci] FAIL: Debug clean & exit /b 1 )
%MSBUILD% %ROOT%GleeBug.sln -p:Configuration=Debug -p:Platform=x64 -m -v:m >> %OUT%\build_debug.log 2>&1
if errorlevel 1 ( echo [ci] FAIL: Debug build & exit /b 1 )

rem ---- stage 2: clean rebuild Release x64 ----
echo [ci] clean rebuild Release x64 ...
%MSBUILD% %ROOT%GleeBug.sln -t:Clean -p:Configuration=Release -p:Platform=x64 -m -v:m > %OUT%\build_clean_release.log 2>&1
if errorlevel 1 ( echo [ci] FAIL: Release clean & exit /b 1 )
%MSBUILD% %ROOT%GleeBug.sln -p:Configuration=Release -p:Platform=x64 -m -v:m >> %OUT%\build_release.log 2>&1
if errorlevel 1 ( echo [ci] FAIL: Release build & exit /b 1 )

rem ---- stage 3: full suite (Debug binaries) x3 ----
rem Every run gets its own TDIR; artifacts live there from the start, so a
rem failing run leaves its complete evidence behind automatically.
for /l %%i in (1,1,3) do (
    echo [ci] suite run %%i/3 ...
    set TDIR=%OUT_BASH%/work_debug_%%i
    mkdir %OUT%\work_debug_%%i 2>nul
    pushd %ROOT%
    %BASH% -c "TDIR='!TDIR!' bash run_tests.sh" > %OUT%\suite_debug_%%i.log 2>&1
    popd
    if errorlevel 1 ( echo [ci] FAIL: suite run %%i ^(artifacts: !TDIR!^) & exit /b 1 )
)

rem ---- stage 4: full suite (Release binaries) x1 ----
echo [ci] suite run release ...
set TDIR=%OUT_BASH%/work_release
mkdir %OUT%\work_release 2>nul
pushd %ROOT%
%BASH% -c "TDIR='%TDIR%' GLEAM=./bin/Release/x64/Gleam.exe TARGET=bin/Release/x64/TestTarget.exe ATARGET=bin/Release/x64/ArgvTarget.exe BTARGET=bin/Release/x64/BoundaryTarget.exe bash run_tests.sh" > %OUT%\suite_release.log 2>&1
popd
if errorlevel 1 ( echo [ci] FAIL: release suite ^(artifacts: %TDIR%^) & exit /b 1 )

echo [ci] PASS: all stages green. Logs in %OUT%
exit /b 0
