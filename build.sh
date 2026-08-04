#!/bin/bash
# build.sh — Cross-platform build script for FORGE Engine (Linux / macOS)
# Usage: ./build.sh [debug|release] [target]
#   target: all, test, demo, demo_ui, demo_window, demo_sandbox, clean

set -e

# Detect platform
UNAME_S=$(uname -s)
if [ "$UNAME_S" = "Linux" ]; then
    PLATFORM="linux"
    PLATFORM_CFLAGS="-DFGE_PLATFORM_LINUX=1"
    PLATFORM_LDFLAGS="-lpthread -lm -lrt -ldl"
    PLATFORM_SRC="src/platform/platform_linux.c"
elif [ "$UNAME_S" = "Darwin" ]; then
    PLATFORM="macos"
    PLATFORM_CFLAGS="-DFGE_PLATFORM_MACOS=1"
    PLATFORM_LDFLAGS="-lpthread -lm -framework Cocoa -framework CoreFoundation"
    PLATFORM_SRC="src/platform/platform_macos.c"
else
    echo "Unsupported platform: $UNAME_S"
    exit 1
fi

# Detect compiler (prefer clang)
if command -v clang >/dev/null 2>&1; then
    CC="clang"
else
    CC="gcc"
fi

# Detect C23 support
if $CC -std=c23 -E - </dev/null >/dev/null 2>&1; then
    CSTD="-std=c23"
else
    CSTD="-std=c2x"
fi

# Build mode
MODE="${1:-release}"
TARGET="${2:-all}"

if [ "$MODE" = "debug" ]; then
    BUILD_CFLAGS="-g -O0 -DDEBUG -DFGE_ENABLE_HEAP_TRACKING=1 -fsanitize=address,undefined -fno-omit-frame-pointer"
    BUILD_LDFLAGS="-fsanitize=address,undefined"
    echo "=== FORGE Engine Build (DEBUG) [$PLATFORM] ==="
else
    BUILD_CFLAGS="-O3 -DNDEBUG -ffast-math -flto -fomit-frame-pointer"
    BUILD_LDFLAGS="-flto"
    echo "=== FORGE Engine Build (RELEASE) [$PLATFORM] ==="
fi

# Base flags
CFLAGS="$CSTD -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration \
    -Werror=return-type -Werror=uninitialized \
    -Wstrict-overflow=2 -Wformat=2 -Wshadow -Wmissing-prototypes \
    -Wmissing-declarations -Wredundant-decls -fno-strict-aliasing \
    -fno-common -fvisibility=hidden \
    -Iinclude -D_DEFAULT_SOURCE \
    $PLATFORM_CFLAGS $BUILD_CFLAGS"

LDFLAGS="$PLATFORM_LDFLAGS $BUILD_LDFLAGS"

# Source files
SRC_CORE="src/core/log.c src/core/memory.c src/core/time.c src/core/telemetry.c src/core/dev_mode.c"
SRC_MATH="src/math/math.c"
SRC_NET="src/net/net.c"
SRC_ECS="src/ecs/ecs.c"
SRC_PHYSICS="src/physics/physics.c src/physics/collision.c"
SRC_SIM="src/simulation/simulation.c"
SRC_RENDERER="src/renderer/renderer_2d.c src/renderer/debug_overlay.c"

# UI sources (exclude platform-specific backends not for this platform)
SRC_UI_COMMON="\
    src/ui/canvas.c src/ui/widget.c src/ui/layout.c src/ui/app.c \
    src/ui/window.c src/ui/popup.c src/ui/a11y.c src/ui/clipboard.c \
    src/ui/ime.c src/ui/gamepad.c src/ui/audio.c src/ui/renderer_sw.c \
    src/ui/font_builtin.c src/ui/font_ttf.c src/ui/font_sdf.c \
    src/ui/image_bmp.c src/ui/image_png.c src/ui/image_jpeg.c \
    src/ui/image_gif.c src/ui/widget_ordl.c src/ui/backend_term.c"

if [ "$PLATFORM" = "linux" ]; then
    SRC_UI="$SRC_UI_COMMON \
        src/ui/backend_drm.c src/ui/backend_wayland.c \
        src/ui/backend_x11.c src/ui/backend_gl.c \
        src/ui/backend_gl_x11.c src/ui/backend_gl_wayland.c \
        src/ui/backend_win32.c"
else
    SRC_UI="$SRC_UI_COMMON src/ui/backend_gl.c src/ui/backend_win32.c"
fi

SRC_ALL="$SRC_CORE $SRC_MATH $SRC_NET $SRC_ECS $SRC_PHYSICS $SRC_SIM $PLATFORM_SRC $SRC_RENDERER $SRC_UI"

OBJ_DIR="build/obj"
mkdir -p "$OBJ_DIR/core" "$OBJ_DIR/math" "$OBJ_DIR/net" "$OBJ_DIR/ecs"
mkdir -p "$OBJ_DIR/physics" "$OBJ_DIR/simulation" "$OBJ_DIR/platform"
mkdir -p "$OBJ_DIR/renderer" "$OBJ_DIR/ui"

# Compile object files
echo "Compiler: $CC"
echo "C std:    $CSTD"
echo ""

compile_file() {
    local src="$1"
    local obj="$OBJ_DIR/${src%.c}.o"
    mkdir -p "$(dirname "$obj")"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        echo "  CC $src"
        $CC $CFLAGS -c "$src" -o "$obj"
    fi
}

for src in $SRC_ALL; do
    compile_file "$src"
done

# Collect objects
OBJS=""
for src in $SRC_ALL; do
    OBJS="$OBJS $OBJ_DIR/${src%.c}.o"
done

# Build library
echo ""
echo "  AR build/libforge.a"
ar rcs build/libforge.a $OBJS

# Build targets
build_target() {
    local name="$1"
    local src="$2"
    local out="$3"
    echo "  CC $out"
    $CC $CFLAGS "$src" -Lbuild -lforge $LDFLAGS -o "$out"
}

case "$TARGET" in
    all|test)
        build_target "test" "tests/test_forge.c" "build/test_forge"
        ;;&
    all|demo)
        build_target "hello" "examples/hello_forge/main.c" "build/demo_hello"
        ;;&
    all|demo_ui)
        build_target "ui" "examples/ui_demo/main.c" "build/demo_ui"
        ;;&
    all|demo_window)
        build_target "window" "examples/window_demo/main.c" "build/demo_window"
        ;;&
    all|demo_sandbox)
        build_target "sandbox" "examples/sandbox_demo/main.c" "build/demo_sandbox"
        ;;&
    clean)
        rm -rf build/
        echo "Build directory cleaned."
        exit 0
        ;;
esac

echo ""
echo "=== Build Complete ==="
echo "Targets:"
ls -la build/demo_* build/test_forge 2>/dev/null | awk '{print "  " $9 " (" $5 " bytes)"}'
echo ""
echo "Run examples:"
echo "  ./build/demo_sandbox     — Pixel simulation sandbox"
echo "  ./build/demo_window      — Physics + pixel-perfect collision"
echo "  ./build/demo_hello       — Hello world"
echo "  ./build/demo_ui          — UI toolkit demo"
echo "  ./build/test_forge       — Unit test suite"
