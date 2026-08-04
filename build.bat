@echo off
REM build.bat — Windows build script for FORGE Engine
REM Usage: build.bat [debug|release] [target]
REM   target: all, test, demo, demo_ui, demo_window, demo_sandbox, clean
REM Requirements: Visual Studio 2022+ (cl.exe) or Clang for Windows

setlocal EnableDelayedExpansion

set MODE=%1
if "%MODE%"=="" set MODE=release

set TARGET=%2
if "%TARGET%"=="" set TARGET=all

REM Detect compiler
where cl >nul 2>nul
if %ERRORLEVEL% == 0 (
    set CC=cl
    set IS_MSVC=1
    goto :compiler_found
)

where clang >nul 2>nul
if %ERRORLEVEL% == 0 (
    set CC=clang
    set IS_MSVC=0
    goto :compiler_found
)

echo ERROR: No compiler found. Install Visual Studio or Clang.
exit /b 1

:compiler_found
echo === FORGE Engine Build (%MODE%) [Windows] ===
echo Compiler: %CC%

REM Source files
set SRC_CORE=src/core/log.c src/core/memory.c src/core/time.c src/core/telemetry.c src/core/dev_mode.c
set SRC_MATH=src/math/math.c
set SRC_NET=src/net/net.c
set SRC_ECS=src/ecs/ecs.c
set SRC_PHYSICS=src/physics/physics.c src/physics/collision.c
set SRC_SIM=src/simulation/simulation.c
set SRC_RENDERER=src/renderer/renderer_2d.c src/renderer/debug_overlay.c
set SRC_PLATFORM=src/platform/platform_win32.c

set SRC_UI=src/ui/canvas.c src/ui/widget.c src/ui/layout.c src/ui/app.c src/ui/window.c src/ui/popup.c
set SRC_UI=%SRC_UI% src/ui/a11y.c src/ui/clipboard.c src/ui/ime.c src/ui/gamepad.c src/ui/audio.c
set SRC_UI=%SRC_UI% src/ui/renderer_sw.c src/ui/font_builtin.c src/ui/font_ttf.c src/ui/font_sdf.c
set SRC_UI=%SRC_UI% src/ui/image_bmp.c src/ui/image_png.c src/ui/image_jpeg.c src/ui/image_gif.c
set SRC_UI=%SRC_UI% src/ui/widget_ordl.c src/ui/backend_term.c src/ui/backend_win32.c

set SRC_ALL=%SRC_CORE% %SRC_MATH% %SRC_NET% %SRC_ECS% %SRC_PHYSICS% %SRC_SIM% %SRC_RENDERER% %SRC_PLATFORM% %SRC_UI%

if not exist build mkdir build
if not exist build\obj mkdir build\obj

if "%IS_MSVC%"=="1" (
    REM MSVC flags
    if "%MODE%"=="debug" (
        set CFLAGS=/std:c11 /W4 /Zi /Od /DDEBUG /DFGE_PLATFORM_WINDOWS=1 /Iinclude
        set LDFLAGS=/link kernel32.lib user32.lib gdi32.lib winmm.lib
    ) else (
        set CFLAGS=/std:c11 /W3 /O2 /DNDEBUG /DFGE_PLATFORM_WINDOWS=1 /Iinclude
        set LDFLAGS=/link kernel32.lib user32.lib gdi32.lib winmm.lib
    )
    
    REM Compile each source
    for %%f in (%SRC_ALL%) do (
        set OBJ=build\obj\%%~nf.obj
        echo   CC %%f
        cl /c !CFLAGS! /Fo:!OBJ! %%f >nul
    )
    
    REM Link library
    echo   LIB build\libforge.lib
    lib /OUT:build\libforge.lib build\obj\*.obj >nul
    
    REM Build targets
    if "%TARGET%"=="all" (
        echo   CC build\test_forge.exe
        cl !CFLAGS! tests\test_forge.c /Fe:build\test_forge.exe build\libforge.lib !LDFLAGS! >nul
        echo   CC build\demo_sandbox.exe
        cl !CFLAGS! examples\sandbox_demo\main.c /Fe:build\demo_sandbox.exe build\libforge.lib !LDFLAGS! >nul
        echo   CC build\demo_window.exe
        cl !CFLAGS! examples\window_demo\main.c /Fe:build\demo_window.exe build\libforge.lib !LDFLAGS! >nul
        echo   CC build\demo_hello.exe
        cl !CFLAGS! examples\hello_forge\main.c /Fe:build\demo_hello.exe build\libforge.lib !LDFLAGS! >nul
    )
) else (
    REM Clang/GCC flags
    if "%MODE%"=="debug" (
        set CFLAGS=-std=c2x -Wall -Wextra -g -O0 -DDEBUG -DFGE_PLATFORM_WINDOWS=1 -Iinclude
        set LDFLAGS=-lkernel32 -luser32 -lgdi32
    ) else (
        set CFLAGS=-std=c2x -Wall -O3 -DNDEBUG -DFGE_PLATFORM_WINDOWS=1 -Iinclude
        set LDFLAGS=-lkernel32 -luser32 -lgdi32
    )
    
    for %%f in (%SRC_ALL%) do (
        set OBJ=build\obj\%%~nf.o
        echo   CC %%f
        clang -c !CFLAGS! -o !OBJ! %%f
    )
    
    echo   AR build\libforge.a
    llvm-ar rcs build\libforge.a build\obj\*.o
    
    if "%TARGET%"=="all" (
        echo   CC build\test_forge.exe
        clang !CFLAGS! tests\test_forge.c -Lbuild -lforge !LDFLAGS! -o build\test_forge.exe
        echo   CC build\demo_sandbox.exe
        clang !CFLAGS! examples\sandbox_demo\main.c -Lbuild -lforge !LDFLAGS! -o build\demo_sandbox.exe
        echo   CC build\demo_window.exe
        clang !CFLAGS! examples\window_demo\main.c -Lbuild -lforge !LDFLAGS! -o build\demo_window.exe
        echo   CC build\demo_hello.exe
        clang !CFLAGS! examples\hello_forge\main.c -Lbuild -lforge !LDFLAGS! -o build\demo_hello.exe
    )
)

echo.
echo === Build Complete ===
echo Run: build\demo_sandbox.exe
