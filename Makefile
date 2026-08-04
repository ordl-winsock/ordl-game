# FORGE Game Engine Makefile
# Pure C23, zero external dependencies

# Auto-detect compiler and C23 support
CC := $(shell which clang 2>/dev/null || which gcc 2>/dev/null || echo cc)

# Check for C23 support
C23_TEST := $(shell $(CC) -std=c23 -E - </dev/null >/dev/null 2>&1 && echo yes || echo no)
ifeq ($(C23_TEST),yes)
  CSTD := -std=c23
else
  CSTD := -std=c2x
endif

CFLAGS  := $(CSTD) -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration \
           -Werror=return-type -Werror=uninitialized -Werror=maybe-uninitialized \
           -Wstrict-overflow=2 -Wformat=2 -Wshadow -Wmissing-prototypes \
           -Wmissing-declarations -Wredundant-decls -fno-strict-aliasing \
           -fno-common -fvisibility=hidden \
           -Iinclude -D_DEFAULT_SOURCE

LDFLAGS := -lpthread -lm -lrt

# Debug vs Release
ifeq ($(DEBUG),1)
  CFLAGS += -g -O0 -DDEBUG -DFGE_ENABLE_HEAP_TRACKING=1 \
            -fsanitize=address,undefined -fno-omit-frame-pointer
  LDFLAGS += -fsanitize=address,undefined
else ifeq ($(NO_LTO),1)
  CFLAGS += -O2 -DNDEBUG -ffast-math -fomit-frame-pointer
else
  CFLAGS += -O3 -DNDEBUG -ffast-math -flto -fomit-frame-pointer
  LDFLAGS += -flto
endif

# Verbose logging
ifeq ($(VERBOSE_LOG),1)
  CFLAGS += -DFGE_LOG_COMPILE_LEVEL=FGE_LOG_LEVEL_TRACE
endif

# Platform detection
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  CFLAGS += -DFGE_PLATFORM_LINUX=1
  LDFLAGS += -ldl
  SRC_PLATFORM := src/platform/platform_linux.c
endif
ifeq ($(UNAME_S),Darwin)
  CFLAGS += -DFGE_PLATFORM_MACOS=1
  LDFLAGS += -framework Cocoa -framework CoreFoundation
  SRC_PLATFORM := src/platform/platform_macos.c
endif
ifeq ($(OS),Windows_NT)
  CFLAGS += -DFGE_PLATFORM_WINDOWS=1
  SRC_PLATFORM := src/platform/platform_win32.c
  LDFLAGS += -lkernel32 -luser32 -lgdi32
endif

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------

# Core engine
SRC_CORE     := src/core/log.c src/core/memory.c src/core/time.c src/core/telemetry.c src/core/dev_mode.c src/core/input.c
SRC_MATH     := src/math/math.c
SRC_NET      := src/net/net.c src/net/netcode.c
SRC_ECS      := src/ecs/ecs.c
SRC_RENDERER := src/renderer/renderer_2d.c src/renderer/renderer_gl.c src/renderer/debug_overlay.c

# UI toolkit (ORDL UI — integrated)
# Note: theme.c excluded (needs TOML parser from infercli)
# Note: font_otf.c excluded (incomplete, font_ttf.c handles OTF via delegation)
SRC_UI := \
	src/ui/canvas.c \
	src/ui/widget.c \
	src/ui/layout.c \
	src/ui/app.c \
	src/ui/window.c \
	src/ui/popup.c \
	src/ui/a11y.c \
	src/ui/clipboard.c \
	src/ui/ime.c \
	src/ui/gamepad.c \
	src/ui/audio.c \
	src/ui/renderer_sw.c \
	src/ui/font_builtin.c \
	src/ui/font_ttf.c \
	src/ui/font_sdf.c \
	src/ui/image_bmp.c \
	src/ui/image_png.c \
	src/ui/image_jpeg.c \
	src/ui/image_gif.c \
	src/ui/widget_ordl.c \
	src/ui/backend_term.c \
	src/ui/backend_drm.c \
	src/ui/backend_wayland.c \
	src/ui/backend_x11.c \
	src/ui/backend_gl.c \
	src/ui/backend_gl_x11.c \
	src/ui/backend_gl_wayland.c \
	src/ui/backend_win32.c

SRC_PHYSICS  := src/physics/physics.c src/physics/collision.c
SRC_SIM      := src/simulation/simulation.c

SRC_ALL := $(SRC_CORE) $(SRC_MATH) $(SRC_NET) $(SRC_ECS) $(SRC_PHYSICS) $(SRC_SIM) $(SRC_PLATFORM) $(SRC_RENDERER) $(SRC_UI)

# Object files
OBJ_DIR     := build/obj
OBJ         := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRC_ALL))
DEPS        := $(OBJ:.o=.d)

# Targets
TARGET_LIB      := build/libforge.a
TARGET_TEST     := build/test_forge
TARGET_DEMO     := build/demo_hello
TARGET_DEMO_UI  := build/demo_ui
TARGET_DEMO_WIN := build/demo_window
TARGET_DEMO_SANDBOX := build/demo_sandbox
TARGET_DEMO_GAME := build/demo_game
TARGET_MP_SERVER := build/mp_server
TARGET_MP_CLIENT := build/mp_client

ALL_TARGETS     := $(TARGET_LIB) $(TARGET_TEST) $(TARGET_DEMO) $(TARGET_DEMO_UI) $(TARGET_DEMO_WIN) $(TARGET_DEMO_SANDBOX) $(TARGET_DEMO_GAME) $(TARGET_MP_SERVER) $(TARGET_MP_CLIENT)

# ---------------------------------------------------------------------------
# Build rules
# ---------------------------------------------------------------------------

.PHONY: all clean test demo demo_ui demo_window dirs stats

all: dirs $(ALL_TARGETS)

dirs:
	@mkdir -p $(OBJ_DIR)/core $(OBJ_DIR)/math $(OBJ_DIR)/net $(OBJ_DIR)/ecs $(OBJ_DIR)/physics $(OBJ_DIR)/simulation \
	         $(OBJ_DIR)/platform $(OBJ_DIR)/renderer $(OBJ_DIR)/ui

$(TARGET_LIB): $(OBJ)
	@echo "AR $@"
	@ar rcs $@ $^

$(TARGET_TEST): tests/test_forge.c $(TARGET_LIB)
	@echo "CC $@"
	@$(CC) $(CFLAGS) $< -Lbuild -lforge $(LDFLAGS) -o $@

$(TARGET_DEMO): examples/hello_forge/main.c $(TARGET_LIB)
	@echo "CC $@"
	@$(CC) $(CFLAGS) $< -Lbuild -lforge $(LDFLAGS) -o $@

$(TARGET_DEMO_UI): examples/ui_demo/main.c $(TARGET_LIB)
	@echo "CC $@"
	@$(CC) $(CFLAGS) $< -Lbuild -lforge $(LDFLAGS) -o $@

$(TARGET_DEMO_WIN): examples/window_demo/main.c $(TARGET_LIB)
	@echo "CC $@"
	@$(CC) $(CFLAGS) $< -Lbuild -lforge $(LDFLAGS) -o $@

$(TARGET_DEMO_SANDBOX): examples/sandbox_demo/main.c $(TARGET_LIB)
	@echo "CC $@"
	@$(CC) $(CFLAGS) $< -Lbuild -lforge $(LDFLAGS) -o $@

$(TARGET_DEMO_GAME): examples/game_demo/main.c $(TARGET_LIB)
	@echo "CC $@"
	@$(CC) $(CFLAGS) $< -Lbuild -lforge $(LDFLAGS) -o $@

$(TARGET_MP_SERVER): examples/mp_demo/server.c $(TARGET_LIB)
	@echo "CC $@"
	@$(CC) $(CFLAGS) -Iexamples/mp_demo $< -Lbuild -lforge $(LDFLAGS) -o $@

$(TARGET_MP_CLIENT): examples/mp_demo/client.c $(TARGET_LIB)
	@echo "CC $@"
	@$(CC) $(CFLAGS) -Iexamples/mp_demo $< -Lbuild -lforge $(LDFLAGS) -o $@

# Unified compilation rule with dependency generation
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Include generated dependencies
-include $(DEPS)

test: $(TARGET_TEST)
	@echo "Running tests..."
	@./$(TARGET_TEST)

demo: $(TARGET_DEMO)
	@echo "Running demo..."
	@./$(TARGET_DEMO)

demo_ui: $(TARGET_DEMO_UI)
	@echo "Running UI demo..."
	@./$(TARGET_DEMO_UI)

demo_window: $(TARGET_DEMO_WIN)
	@echo "Running window demo..."
	@./$(TARGET_DEMO_WIN)

demo_sandbox: $(TARGET_DEMO_SANDBOX)
	@echo "Running sandbox demo..."
	@./$(TARGET_DEMO_SANDBOX)

clean:
	@rm -rf build/

# Stats
stats:
	@echo "=== FORGE Engine Source Statistics ==="
	@echo "Core:    $$(wc -l < $(SRC_CORE) | awk '{print $$1}') lines"
	@echo "Math:    $$(wc -l < $(SRC_MATH) | awk '{print $$1}') lines"
	@echo "UI:      $$(cat $(SRC_UI) | wc -l) lines"
	@echo "Total:   $$(cat $(SRC_ALL) | wc -l) lines"
