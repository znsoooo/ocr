cmake_minimum_required(VERSION 3.16)
project(ocr CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# 编译工具链: MinGW-w64 (g++)  —— 见 build_windows.bat
#
#   Windows : 需要 MinGW-w64 (g++ / mingw32-make) + CMake
#             推荐用 https://www.msys2.org/ 的 UCRT64 环境:
#                 pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake
#             或者用免安装的 https://github.com/skeeto/w64devkit (自带 g++ 和 make)
#
#   Linux   : g++ + CMake (用系统自带的即可)
#
# ONNX Runtime 的头文件和导入库随本仓库附带, 在仓库根目录:
#     onnxruntime-win-x64-1.30.0/{include, lib}
# lib/ 下同时放了两个导入库, 按工具链各取所需:
#     libonnxruntime.a   MinGW 用
#     onnxruntime.lib    MSVC 用 (参考, 本项目默认不用 MSVC)
#
#   指定方式:
#     cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++ \
#           -DONNXRUNTIME_ROOT=..\onnxruntime-win-x64-1.30.0
# ---------------------------------------------------------------------------
set(ONNXRUNTIME_ROOT "" CACHE PATH "onnxruntime 解压目录 (包含 include/ 和 lib/)")
if(NOT ONNXRUNTIME_ROOT)
    file(GLOB _cand
         "${CMAKE_CURRENT_SOURCE_DIR}/onnxruntime-*"
         "${CMAKE_CURRENT_SOURCE_DIR}/../onnxruntime-*"
         "${CMAKE_CURRENT_SOURCE_DIR}/../../onnxruntime-*"
         "$ENV{HOME}/onnxruntime-*")
    if(_cand)
        list(GET _cand 0 ONNXRUNTIME_ROOT)
        message(STATUS "自动找到 ONNXRUNTIME_ROOT = ${ONNXRUNTIME_ROOT}")
    endif()
endif()

find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
          HINTS "${ONNXRUNTIME_ROOT}/include" /usr/include /usr/local/include)
find_library(ONNXRUNTIME_LIB NAMES onnxruntime
             HINTS "${ONNXRUNTIME_ROOT}/lib" /usr/lib /usr/local/lib)

if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIB)
    message(FATAL_ERROR
        "找不到 ONNX Runtime。请先下载并解压, 然后用 -DONNXRUNTIME_ROOT=<目录> 指定。\n"
        "  下载: https://github.com/microsoft/onnxruntime/releases")
endif()
message(STATUS "ONNX Runtime include: ${ONNXRUNTIME_INCLUDE_DIR}")
message(STATUS "ONNX Runtime lib    : ${ONNXRUNTIME_LIB}")

# ---------------------------------------------------------------------------
# 单一命令行工具 ocr, 图片与视频两种模式共用同一套检测/识别代码:
#   ocr --image PATH   静态图片 -> 文字 + 四点坐标
#   ocr --video PATH   视频逐帧/抽帧 -> 时间点 + 帧序号 + 文字 + 四点坐标
# ---------------------------------------------------------------------------
add_executable(ocr main.cpp)
set(OCR_TARGETS ocr)

foreach(_t IN LISTS OCR_TARGETS)
    target_include_directories(${_t} PRIVATE ${ONNXRUNTIME_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${_t} PRIVATE ${ONNXRUNTIME_LIB})

    if(WIN32)
        # Windows 下把 onnxruntime.dll 拷到可执行文件旁边。
        # 注意: 随包附带的 ONNX Runtime 开发包里 lib/ 只有 .lib 导入库、没有 .dll
        # (dll 在 windows/ 目录下), 所以这里必须先判断文件存在, 否则 POST_BUILD
        # 会因为找不到源文件而让整个编译失败。
        get_filename_component(_ort_libdir "${ONNXRUNTIME_LIB}" DIRECTORY)
        if(EXISTS "${_ort_libdir}/onnxruntime.dll")
            add_custom_command(TARGET ${_t} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_ort_libdir}/onnxruntime.dll" "$<TARGET_FILE_DIR:${_t}>"
                COMMENT "复制 onnxruntime.dll")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../windows/onnxruntime.dll")
            # 退回用 windows/ 目录下那份
            add_custom_command(TARGET ${_t} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CMAKE_CURRENT_SOURCE_DIR}/../windows/onnxruntime.dll"
                        "$<TARGET_FILE_DIR:${_t}>"
                COMMENT "复制 onnxruntime.dll (来自 windows/)")
        else()
            message(WARNING
                "找不到 onnxruntime.dll，编译出的 exe 需要自己把 dll 放到 exe 旁边才能运行。")
        endif()
    else()
        # Linux/macOS: 让可执行文件能直接找到 .so/.dylib
        set_target_properties(${_t} PROPERTIES
            BUILD_RPATH "${ONNXRUNTIME_ROOT}/lib"
            INSTALL_RPATH "${ONNXRUNTIME_ROOT}/lib")
    endif()

    if(MSVC)
        target_compile_options(${_t} PRIVATE /utf-8 /W3)
    else()
        target_compile_options(${_t} PRIVATE -O2 -Wall)
    endif()

    # MinGW 下静态链接 gcc/stdc++/winpthread, 产出的 exe 不依赖任何 MinGW 的 DLL
    # (否则别人机器上要额外带 libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll)
    if(MINGW)
        target_link_options(${_t} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endforeach()

