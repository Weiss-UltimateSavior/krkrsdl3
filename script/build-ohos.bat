@echo off
REM krkrsdl3 OHOS IDE build wrapper
REM Usage: script\build-ohos.bat [debug|release]

set BUILD_MODE=%1
if "%BUILD_MODE%"=="" set BUILD_MODE=debug

set NODE=C:\D\DevEco Studio\tools\node\node.exe
set HVIGOR=C:\D\DevEco Studio\tools\hvigor\bin\hvigorw.js
set RAW_RES=%~dp0..\ohos\entry\src\main\resources\rawfile\Res

echo === krkrsdl3 OHOS IDE Build ===
echo     Mode: %BUILD_MODE%

REM 1. 同步 Res/ 到 rawfile (IDE 打包进 HAP)
echo [1/3] Syncing Res/ to rawfile...
if exist "%RAW_RES%" rmdir /s /q "%RAW_RES%"
robocopy "%~dp0..\Res" "%RAW_RES%" /E /NFL /NDL /NJH /NJS >nul
echo     OK

REM 2. 清理 IDE 构建缓存
echo [2/3] Cleaning IDE cache...
if exist "%~dp0..\ohos\entry\.cxx" rmdir /s /q "%~dp0..\ohos\entry\.cxx"
if exist "%~dp0..\ohos\entry\build" rmdir /s /q "%~dp0..\ohos\entry\build"
echo     OK

REM 3. 执行 IDE 构建
echo [3/3] Building HAP...
pushd "%~dp0..\ohos"
"%NODE%" "%HVIGOR%" --mode module -p product=default -p buildMode=%BUILD_MODE% assembleHap --analyze=normal --parallel --incremental --daemon
set RC=%ERRORLEVEL%
popd

if %RC% neq 0 (
    echo === BUILD FAILED (check logs above) ===
    exit /b %RC%
)

echo.
echo === BUILD SUCCESSFUL ===
for /r "%~dp0..\ohos\entry\build" %%f in (*.hap) do (
    echo HAP: %%f
    echo Size: %%~zf bytes
)