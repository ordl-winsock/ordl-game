# FORGE Game Engine — Architecture Document

## 1. Overview

**FORGE** is a complete, from-scratch game engine written in pure C23 with **zero external dependencies**.
No SDL, no GLFW, no OpenGL SDK, no stb_image, no third-party libraries of any kind.
Only standard C library, OS system calls, and hardware interfaces accessed directly.

The engine is purpose-built for **real-time 2D MMORPGs** — massive worlds, thousands of concurrent entities, networked gameplay, and persistent worlds.

## 2. Design Philosophy

| Principle | Implementation |
|---|---|
| **Zero dependencies** | Every line of code is ours. No black boxes. |
| **Data-oriented** | ECS with SoA arrays, cache-friendly iteration. |
| **Deterministic** | Reproducible simulation for rollback netcode. |
| **Lock-free** | Spinlocks and atomics, no mutexes on hot paths. |
| **Verbose debugging** | 19 log categories, compile-time filtering, ring buffers. |
| **Cross-platform** | Linux (primary), macOS, Windows via conditional compilation. |

## 3. Module Hierarchy

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         GAME (Ember Online)                              │
├──────────────┬──────────────┬──────────────┬──────────────┬─────────────┤
│   ECS        │   Scene      │   Combat     │   Economy    │   Guilds    │
│  (sparse)    │  (zones)     │  (skills)    │  (market)    │  (social)   │
├──────────────┼──────────────┼──────────────┼──────────────┼─────────────┤
│  Physics     │   AI         │   Script     │   Net        │   Chat      │
│  (2D body)   │  (state)     │  (hot C23)   │  (TCP+UDP)   │  (channels) │
├──────────────┴──────────────┴──────────────┴──────────────┴─────────────┤
│  Renderer 2D │   Audio      │   UI (ORDL)  │   Platform   │   Input     │
│  (SW batch)  │  (mixer)     │  (widgets)   │  (X11/WL)    │  (pad/key)  │
├──────────────┴──────────────┴──────────────┴──────────────┴─────────────┤
│  Core │ Math │ Memory │ Time │ Log │ Serialize │ Hash │ RNG │ Atomics  │
└─────────────────────────────────────────────────────────────────────────┘
```

## 4. Module Details

### 4.1 Foundation Layer

| Module | Header | LOC | Purpose |
|---|---|---|---|
| **Core** | `forge/core.h` | ~800 | Types, macros, atomics, spinlocks, PRNG, hashes, ring buffers |
| **Math** | `forge/math.h` | ~600 | Vec2/3/4, Mat4, Quat, AABB, Circle, Color, curves |
| **Memory** | `forge/memory.h` | ~500 | Arena, pool, scratch frame, VM, heap tracking |
| **Time** | `forge/time.h` | ~400 | Clock, timer, frame pacing, profiler, FPS counter |
| **Log** | `forge/log.h` | ~900 | 19 categories, async ring buffer, colored output, structured KV |
| **Serialize** | `forge/serialize.h` | ~300 | Binary serialization, varint, endian swap |

### 4.2 UI Layer (ORDL UI Integration)

The UI system is the **ORDL Sovereign UI Toolkit**, a 15,000+ line pure C23 UI framework with zero dependencies. It has been integrated into FORGE under `src/ui/` and `include/forge/ui/`.

| Component | Files | LOC | Description |
|---|---|---|---|
| **Backends** | `backend_*.c` (7 files) | ~4,500 | Terminal (VT100), DRM/KMS, Wayland (raw wire), X11 (raw wire), GL/ES, Win32 |
| **Canvas** | `canvas.c` | ~360 | TUI cell grid + RGBA8888 framebuffer, clip stack, damage tracking |
| **Widgets** | `widget.c`, `widget_ordl.c` | ~2,400 | 20+ widget types: box, label, button, input, scroll, split, tabs, table, tree, etc. |
| **Layout** | `layout.c` | ~280 | Flexbox engine: row/column, justify, align, gap, padding, wrap |
| **Renderer** | `renderer_sw.c` | ~440 | Software rasterizer: rect, triangle, circle, rounded rect, shadow, blit, glyph |
| **Fonts** | `font_*.c` (3 files) | ~1,200 | Embedded bitmap (Unscii), TTF parser, SDF rasterizer, atlas builder |
| **Images** | `image_*.c` (4 files) | ~1,400 | PNG, BMP, JPEG, GIF decoders from scratch |
| **App Shell** | `app.c`, `window.c` | ~630 | Event loop, animation (18 easings), multi-window manager |
| **Input** | `gamepad.c`, `ime.c`, `clipboard.c` | ~800 | Gamepad polling, CJK IME, clipboard |
| **Audio** | `audio.c` | ~620 | Software mixer, WAV playback, synthesis, spatial audio |
| **A11y** | `a11y.c` | ~115 | ARIA roles, screen reader announcements |

**Integration point**: `include/forge/ui.h` provides FORGE-branded aliases (`fge_ui_*`) and bridge helpers for color/math conversion.

### 4.3 Game Layer (Stubs — To Be Implemented)

| Module | Header | Status | Description |
|---|---|---|---|
| **ECS** | `forge/ecs.h` | Stub | Sparse-set entity component system |
| **Renderer 2D** | `forge/renderer.h` | Stub | Sprite batching, tilemap, text, post-FX |
| **Net** | `forge/net.h` | Stub | TCP/UDP sockets, message framing, async I/O |
| **Platform** | `forge/platform.h` | Stub | Window creation, input polling, file I/O |

## 5. Build System

```bash
# Release build (optimized, LTO)
make

# Debug build (ASan, UBSan, heap tracking, no optimization)
make DEBUG=1

# Verbose logging (TRACE level compiled in)
make VERBOSE_LOG=1

# Debug + verbose
make DEBUG=1 VERBOSE_LOG=1

# Run tests
make test

# Run demo
make demo

# Source statistics
make stats
```

### Compiler Requirements

- **Primary**: Clang 21+ with `-std=c2x`
- **Fallback**: GCC 12+ with `-std=c2x` (GCC 11 lacks full C23 support)
- **No C++**: Pure C23 throughout

## 6. Memory Architecture

```
Per-thread scratch arenas (4 MiB, linear bump)
    ↓
Per-zone persistent arenas (growable)
    ↓
Fixed-size object pools (entities, particles, connections)
    ↓
Optional heap tracking (debug builds, file/line leak detection)
```

**Key rule**: No `malloc` on hot paths. All per-frame allocations come from scratch arenas that are reset each frame.

## 7. Networking Architecture (Planned)

```
┌─────────────┐      TCP (reliable)       ┌─────────────┐
│   Client    │ ←→  Command messages      │   Server    │
│             │      (skills, chat, trade) │  (per-zone) │
│             │                            │             │
│             │      UDP (unreliable)      │             │
│             │ ←→  Position updates       │             │
│             │      (20 Hz broadcast)     │             │
└─────────────┘                            └─────────────┘
```

- Raw sockets with epoll/kqueue (no libevent)
- Length-prefixed message framing for TCP
- Unreliable sequenced UDP for position sync
- Lock-free send/receive queues per connection
- Connection pooling with heartbeat and timeout

## 8. Rendering Architecture (Planned)

### Client
- **Primary**: Software renderer (identical pixels everywhere)
- **Optional**: OpenGL ES 2.0 via runtime loading (no link-time dependency)
- Sprite batching: 4096 sprites per draw call
- SDF text rendering with Unicode
- 2D normal-mapped lighting

### Server
- No rendering; headless with spatial partitioning
- Deterministic physics simulation

## 9. Development Roadmap

### Phase 0: Foundation ✅ (COMPLETE)
- [x] Core types, math, memory, time, logging
- [x] Build system (Makefile, Clang/GCC)
- [x] Test suite with 42 passing tests
- [x] Demo application

### Phase 1: UI Integration ✅ (COMPLETE)
- [x] Import ORDL UI toolkit (15,000+ lines)
- [x] Fix build integration (includes, dependencies, stubs)
- [x] Create FORGE wrapper header (`forge/ui.h`)
- [x] All tests pass, library builds

### Phase 2: Render Core
- [ ] Software renderer: triangle rasterization, texture mapping
- [ ] Sprite batching system
- [ ] Tilemap renderer
- [ ] SDF text rendering integration
- [ ] First colored triangle on screen

### Phase 3: Platform & Input
- [ ] X11 backend (raw protocol) — UI already has this
- [ ] Wayland backend (raw protocol) — UI already has this
- [ ] Gamepad support — UI already has this
- [ ] File I/O abstraction
- [ ] Asset loading pipeline

### Phase 4: Networking
- [ ] TCP server with epoll
- [ ] UDP socket management
- [ ] Message serialization
- [ ] Client connection handshake
- [ ] Entity state replication

### Phase 5: ECS & Gameplay
- [ ] Component registration macros
- [ ] System execution
- [ ] Player controller
- [ ] NPC AI (state machines)
- [ ] Combat system

### Phase 6: World & MMORPG Features
- [ ] Zone system with seamless transitions
- [ ] Spatial partitioning (uniform grid)
- [ ] Chat system
- [ ] Trading system
- [ ] Guild system
- [ ] Instance dungeons

## 10. File Organization

```
include/forge/         — Public engine headers (14 headers)
  core.h, math.h, memory.h, time.h, log.h, serialize.h
  ecs.h, net.h, platform.h, renderer.h
  ui.h                  — FORGE UI integration wrapper
  ui/ordl_ui.h          — ORDL UI main header
  ui/ordl_ui_debug.h    — UI debug logging
  ui/ordl_audio.h       — Audio subsystem header

src/                   — Implementation modules
  core/                 — log.c, memory.c, time.c
  math/                 — math.c
  ecs/                  — ecs.c (stub)
  net/                  — net.c (stub)
  platform/             — platform_linux.c (stub)
  renderer/             — renderer_2d.c (stub)
  ui/                   — 28 ORDL UI source files

examples/              — Demo applications
  hello_forge/          — Basic engine demo
  ui_demo/              — UI toolkit demo (NEW)

tests/                 — Unit and integration tests
  test_forge.c          — 42 passing tests

docs/                  — Documentation
  ARCHITECTURE.md       — This document
  GAME_DESIGN_DOCUMENT.md — Game design document
```

## 11. Key Metrics

| Metric | Current | Target |
|---|---|---|
| Total engine LOC | ~22,000 | ~50,000 |
| UI toolkit LOC | ~15,000 | ~15,000 (stable) |
| Foundation LOC | ~4,500 | ~5,000 |
| Tests | 42 | 100+ |
| Build time | <3s | <5s |
| Library size | 620 KB | <1 MB |
| Backend count | 7 | 7 |
| Widget types | 20+ | 25+ |

---

*Document version: 1.0*
*Last updated: 2026-07-21*
*Engine codename: FORGE*
