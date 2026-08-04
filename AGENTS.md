# FORGE Game Engine — Project Agent Guide

## Project Overview

FORGE is a complete game engine built from scratch in pure native C23 with **zero external dependencies**.
No SDL, no GLFW, no OpenGL, no Vulkan SDK, no stb_image, no third-party libraries of any kind.
Only standard C library, OS system calls, and hardware interfaces accessed directly.

## Foundational Reference

The ORDL UI toolkit (`/devops/ordl-infercli/native-c23`) is our primary reference for:
- Cross-platform window creation (X11, Wayland, Win32, DRM, terminal)
- Raw input handling (keyboard, mouse, gamepad)
- Software rendering pipeline (RGBA8888 framebuffer, triangle rasterization, SDF fonts)
- Event system architecture
- Image decoding (PNG, JPEG, BMP, GIF — from scratch)
- Font rendering (bitmap, TTF parser, SDF atlas generation)

When implementing platform or rendering code, study and adapt patterns from `ordl_ui.h` and its backends.

## Coding Standards

- **Language**: C23 (`-std=c23`)
- **Compiler**: GCC 14+ or Clang 18+
- **Style**: Match ORDL UI conventions — snake_case, 4-space indent, brace on same line for functions
- **Headers**: Use `#pragma once` or include guards. Prefer include guards for portability.
- **Types**: Use fixed-width integers (`uint32_t`, etc.). Use `float` for graphics, `double` for physics.
- **Memory**: Arena allocators for frame/scratch data. Pools for fixed-size objects (entities, particles). `malloc` acceptable for level assets with explicit ownership.
- **Math**: Column-major matrices. Right-handed coordinate system (Y up, -Z forward). Radians for angles.
- **Error Handling**: Return `bool` for success/failure. Use `[[nodiscard]]` on resource-returning functions.
- **Assertions**: `static_assert` for compile-time invariants. Runtime assertions for debug builds only.

## Architecture Principles

1. **Layered**: Core → Math → Memory → Platform → Renderer → Audio → Physics → Scene → Game
2. **Data-Oriented**: Cache-friendly structures, SoA where beneficial, batch processing
3. **Immediate Mode**: Renderer uses immediate-mode command buffers
4. **Deterministic**: Fixed timestep physics, reproducible random sequences for replays
5. **Hot-Reload**: Asset pipeline supports live reloading during development

## Build System

- Primary: `Makefile` with GCC/Clang
- `build.sh` wrapper script for convenience
- Target platforms: Linux (x86_64, ARM64), Windows (MinGW-w64), macOS (Clang)
- Debug builds: `-g -O0 -DDEBUG -fsanitize=address,undefined`
- Release builds: `-O3 -DNDEBUG -ffast-math -flto`

## Directory Structure

```
include/forge/    — Public engine headers
src/              — Implementation, one dir per subsystem
examples/         — Demo applications showing engine features
tests/            — Unit and integration tests
docs/             — Architecture and design documents
tools/            — Build scripts and asset pipeline tools
```

## Key Subsystems (in dependency order)

| Layer | Files | Description |
|---|---|---|
| Core | `core.h`, `math.h` | Types, macros, vector/matrix math |
| Memory | `memory.h` | Arena, pool, scratch allocators |
| Time | `time.h` | Clocks, timers, delta time, profiling |
| Log | `log.h` | Structured logging with levels |
| Platform | `platform.h` | Window, input, events, file I/O |
| Renderer | `renderer.h`, `sprite.h` | 2D/3D rendering, sprite batching |
| Audio | `audio.h` | Synthesis, mixing, WAV/OGG playback |
| Physics | `physics.h` | 2D rigid body physics |
| Scene | `scene.h` | Entity-component system |
| Asset | `asset.h` | Hot-reloadable asset pipeline |
| Engine | `engine.h` | Main loop, configuration |

## Testing

Every subsystem must have corresponding tests in `tests/`. Run with `make test`.

## Documentation

- `docs/GAME_DESIGN_DOCUMENT.md` — Vision, target game, technical requirements
- `docs/ARCHITECTURE.md` — Detailed engine architecture
- `docs/RENDERER.md` — Rendering pipeline documentation
- `docs/PHYSICS.md` — Physics engine design
- This file — Agent guidance and conventions

## Current Status

See todo list for active work items.
