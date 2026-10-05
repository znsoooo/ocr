@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
REM ===========================================================================
REM  一键编译 Windows 版 (MinGW-w64)
REM
REM  前置条件:
REM    1) MinGW-w64 工具链 (g++ 和 mingw32-make, 都要在 PATH 里)
REM         推荐 A: MSYS2 (https://www.msys2.org/)
REM                 打开 "MSYS2 UCRT64" 终端, 执行:
REM                     pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake
REM                 然后在该终端里执行本脚本
REM         推荐 B: w64devkit (https://github.com/skeeto/w64devkit/releases)
REM                 免安装, 解压后运行里面的 w64devkit.exe,
REM                 在它打开的终端里执行本脚本 (g++/make/cmake 都自带)
REM    2) CMake 3.16+
REM
REM  ONNX Runtime 的头文件和导入库已随本仓库附带 (根目录 onnxruntime-win-x64-1.30.0),
REM  不需要额外下载。
REM
REM  用法:  build_windows.bat
REM         build_windows.bat D:\别的路径\onnxruntime-win-x64-1.30.0
REM
REM  备选(本项目不使用, 仅作参考) —— 如果想用 MSVC:
REM         cmake -S . -B build-msvc -DONNXRUNTIME_ROOT=..\onnxruntime-win-x64-1.30.0 -A x64
REM         cmake --build build-msvc --config Release
REM ===========================================================================

set HERE=%~dp0
set ROOT=%~dp0..

if "%~1"=="" (
    set ORT=%ROOT%\onnxruntime-win-x64-1.30.0
) else (
    set ORT=%~1
)

echo ============================================================
echo   编译 ocr  Windows 版 (MinGW-w64)
echo ============================================================
echo   ONNX Runtime : %ORT%
echo   源码目录     : %HERE%
echo.

REM ---------- 检查工具链 ----------
where g++ >nul 2>nul
if errorlevel 1 (
    echo [错误] 找不到 g++，说明 MinGW-w64 没装或没加入 PATH。
    echo.
    echo   两个最简单的办法:
    echo     A. w64devkit  ^(免安装^): https://github.com/skeeto/w64devkit/releases
    echo        解压后运行 w64devkit.exe，在它打开的终端里执行本脚本。
    echo     B. MSYS2: https://www.msys2.org/
    echo        打开 "MSYS2 UCRT64" 终端，执行:
    echo            pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake
    echo        然后在该终端里执行本脚本。
    echo.
    pause
    exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo [错误] 找不到 cmake。请安装 CMake 并加入 PATH。
    echo        (w64devkit 和 MSYS2 都自带 cmake)
    pause
    exit /b 1
)

if not exist "%ORT%\include\onnxruntime_cxx_api.h" (
    echo [错误] 在 %ORT% 下找不到 ONNX Runtime 头文件。
    echo        请确认路径正确，或从
    echo        https://github.com/microsoft/onnxruntime/releases
    echo        下载 onnxruntime-win-x64-1.30.0.zip 解压后把目录传进来。
    pause
    exit /b 1
)

echo 工具链版本:
for /f "delims=" %%v in ('g++ --version 2^>^&1') do (
    echo   %%v
    goto :ver_done
)
:ver_done
for /f "delims=" %%v in ('cmake --version 2^>^&1') do (
    echo   %%v
    goto :cmake_done
)
:cmake_done
echo.

cd /d "%HERE%"
if exist build rmdir /s /q build

echo [1/2] 配置 (生成器: MinGW Makefiles)...
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++ -DONNXRUNTIME_ROOT="%ORT%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo.
    echo [失败] cmake 配置出错。
    echo        若提示找不到编译器，说明 g++ 不在 PATH。
    pause
    exit /b 1
)

echo.
echo [2/2] 编译...
cmake --build build -j
if errorlevel 1 ( echo. & echo [失败] 编译出错。 & pause & exit /b 1 )

echo.
echo ============================================================
echo   编译完成！产物在： %HERE%build\
echo.
echo   ocr.exe          图片/视频识别
echo   onnxruntime.dll  推理引擎 (已由 CMake 自动拷到 exe 旁边)
echo.
echo   直接试用：
echo       build\ocr.exe --image ..\samples\sample_ui.png
echo       build\ocr.exe --video ..\samples\test_video.mp4 --every-seconds 0.5
echo.
echo   模型会自动向上找到 (在仓库根目录 models\)。
echo ============================================================
pause
