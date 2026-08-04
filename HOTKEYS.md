# FORGE Engine — Hotkeys, Features & Functionality

## Build Instructions

### Linux
```bash
# Release build (default)
./build.sh

# Debug build with sanitizers
./build.sh debug

# Build specific target
./build.sh release demo_sandbox

# Run tests
./build.sh release test
./build/test_forge

# Clean
./build.sh release clean
```

### macOS
```bash
# Same as Linux — build.sh auto-detects macOS
./build.sh

# Requires: Xcode Command Line Tools
# Links against: -framework Cocoa -framework CoreFoundation
```

### Windows
```batch
:: Using Visual Studio (cl.exe)
build.bat release all

:: Using Clang
build.bat release all

:: Run
build\demo_sandbox.exe
```

---

## Sandbox Demo (`demo_sandbox` / `demo_sandbox.exe`)

### Resolution & Rendering
- **Display**: 800×600 window
- **Grid**: 400×300 simulation cells
- **Sub-cells**: 2×2 per cell (1 display pixel each)
- **Effective resolution**: 800×600 individually addressable sub-pixels
- **Cell size on screen**: 2×2 pixels (chunky, retro-visible)
- **Frame rate**: Uncapped (~200+ FPS typical)

### Mouse Input
| Input | Action |
|---|---|
| **Left click + hold** | Paint selected material (drag to paint trail) |
| **Right click + hold** | Erase / paint EMPTY (drag to erase trail) |
| **Scroll up** | Increase brush radius (+1) |
| **Scroll down** | Decrease brush radius (−1) |

### Material Hotkeys
| Key | Material | State | Notable Properties |
|---|---|---|---|
| `1` | **Sand** | Powder | Falls, piles, melts → Lava at 1700°C |
| `2` | **Water** | Liquid | Flows, extinguishes fire, freezes → Ice, boils → Steam |
| `3` | **Stone** | Solid | Static barrier, melts → Lava at 1200°C |
| `4` | **Wood** | Solid | Flammable, ignites at 300°C → Fire |
| `5` | **Fire** | Gas | Rises, heats neighbors 50°C/frame, dies → Smoke |
| `6` | **Oil** | Liquid | Flammable, floats on water, ignites at 200°C |
| `7` | **Lava** | Liquid | Emits heat, cools → Stone below 1000°C |
| `8` | **Acid** | Liquid | Corrodes Metal, Stone, Wood, Dirt |
| `9` | **Gunpowder** | Powder | Explodes → Fire on contact with heat |
| `0` | **Ice** | Solid | Melts → Water |
| `P` | **Plant** | Solid | Grows upward when water is nearby, burns |
| `S` | **Smoke** | Gas | Rises, dissipates after ~120 frames |
| `M` | **Metal** | Solid | Conducts heat well, melts → Lava, corroded by Acid |
| `C` | **Coal** | Solid | Slow-burning fuel |
| `D` | **Dirt** | Powder | Basic soil |
| `N` | **Snow** | Powder | Light, melts → Water |

### Control Hotkeys
| Key | Action |
|---|---|
| `ESC` | Exit |
| `Space` | Pause / unpause simulation |
| `R` | Reset grid (clear all, rebuild floor + walls) |

### Simulation Features
- **Thermodynamics**: Heat conduction between neighbors, phase changes (melt/boil/freeze)
- **Chemical reactions**: Water extinguishes fire/lava, acid corrodes materials, plants grow with water
- **Active region optimization**: Only simulates cells within the bounding box of non-empty cells
- **Sub-cell detail**: Each 2×2 cell contains 4 individually rendered sub-cells — fine outlines, edges, and patterns visible at pixel level

---

## Window Demo (`demo_window` / `demo_window.exe`)

Pixel-perfect physics collision demo.

### Controls
| Input | Action |
|---|---|
| **Left click + drag** | Move the blue box |
| `F1` / `1` | Toggle debug overlay |
| `F2` / `2` | Toggle FPS display |
| `F3` / `3` | Toggle renderer telemetry |
| `F4` / `4` | Toggle input telemetry |
| `F5` / `5` | Toggle network telemetry |
| `F6` / `6` | Toggle physics telemetry |
| `F7` / `7` | Toggle ECS telemetry |
| `F8` / `8` | Toggle platform telemetry |
| `ESC` | Exit |

### Features
- Bouncing green ball with circular pixel collider
- Draggable blue box with rectangular pixel collider
- Both turn **red** on pixel-perfect collision
- White AABB outlines visible
- Telemetry overlay with live FPS

---

## UI Demo (`demo_ui` / `demo_ui.exe`)

ORDL UI toolkit demonstration.

### Features
- Widget system (buttons, labels, containers)
- Layout engine (flexbox-like)
- Font rendering (TTF/SDF/builtin)
- Image loading (BMP, PNG, JPEG, GIF)
- Accessibility support
- Gamepad input

---

## Hello Forge (`demo_hello` / `demo_hello.exe`)

Minimal "Hello World" window.

---

## Engine Core Features

### Physics (`src/physics/`)
| Feature | Status |
|---|---|
| Rigid body dynamics (semi-implicit Euler) | ✅ Complete |
| Shapes: AABB, Circle, Capsule | ✅ Complete |
| Pixel-perfect collision (`src/physics/collision.c`) | ✅ Complete |
| Spatial hash broad phase | ✅ Complete |
| Exact capsule collision (segment-segment distance) | ✅ Complete |
| Impulse-based collision response with friction | ✅ Complete |
| Fixed timestep with substeps | ✅ Complete |
| Ray casting (all shapes) | ✅ Complete |
| Contact manifold generation | ✅ Complete |

### Simulation (`src/simulation/`)
| Feature | Status |
|---|---|
| 19 materials with real properties | ✅ Complete |
| Heat transfer & conduction | ✅ Complete |
| Phase changes (melt/boil/freeze/condense) | ✅ Complete |
| Chemical reactions (burn, corrode, grow) | ✅ Complete |
| Sub-cell resolution (2×2 per cell) | ✅ Complete |
| Active region optimization | ✅ Complete |
| Temperature-based color shifting | ✅ Complete |

### Platform (`src/platform/`)
| Platform | Backend | Status |
|---|---|---|
| Linux | X11 raw wire protocol | ✅ Complete |
| Linux | Wayland raw wire protocol | 🟡 Partial |
| macOS | Objective-C runtime (no .m files) | ✅ Complete |
| Windows | Win32 API (minimal types) | ✅ Complete |

### Renderer (`src/renderer/`)
| Feature | Status |
|---|---|
| 2D software renderer (RGBA8888) | ✅ Complete |
| Sprite batching | ✅ Complete |
| Triangle rasterizer with texture mapping | ✅ Complete |
| Debug overlay / telemetry | ✅ Complete |
| Post-processing (tint, alpha blend) | ✅ Complete |

### Other Systems
| Feature | File |
|---|---|
| Atomic counters & 120-sample histogram | `src/core/telemetry.c` |
| Dev mode bitmask toggles | `src/core/dev_mode.c` |
| Debug overlay rendering | `src/renderer/debug_overlay.c` |
| Memory arenas & pools | `src/core/memory.c` |
| Deterministic RNG | `src/math/math.c` |
| Hash tables | `src/core/core_hash.c` |
| Entity Component System | `src/ecs/ecs.c` |

---

## File Structure

```
ordl-game/
├── build.sh              # Unix build script (Linux/macOS)
├── build.bat             # Windows build script
├── Makefile              # Make-based build (legacy, still works)
│
├── include/forge/        # Public headers
│   ├── simulation.h      # Pixel simulation API
│   ├── physics.h         # 2D physics engine API
│   ├── collision.h       # Pixel-perfect collision API
│   ├── platform.h        # Cross-platform window/input API
│   ├── renderer.h        # 2D software renderer API
│   ├── telemetry.h       # Performance telemetry API
│   ├── dev_mode.h        # Debug toggle API
│   └── ...
│
├── src/
│   ├── simulation/       # Pixel simulation engine
│   ├── physics/          # 2D physics engine
│   ├── platform/         # Platform layers (linux, macos, win32)
│   ├── renderer/         # 2D software renderer
│   ├── core/             # Logging, memory, time, telemetry
│   ├── math/             # Vector, matrix, quaternion math
│   ├── ecs/              # Entity Component System
│   ├── net/              # Networking
│   └── ui/               # ORDL UI toolkit
│
├── examples/
│   ├── sandbox_demo/     # Falling sand / Noita-like simulation
│   ├── window_demo/      # Physics + pixel collision demo
│   ├── ui_demo/          # UI toolkit demo
│   └── hello_forge/      # Minimal window
│
└── tests/
    └── test_forge.c      # Unit test suite (42 tests)
```

---

## Compiler Requirements

| Platform | Compiler | C Standard |
|---|---|---|
| Linux | Clang 16+ or GCC 13+ | C23 / C2x |
| macOS | Clang (Xcode) | C23 / C2x |
| Windows | MSVC 2022+ or Clang 16+ | C11 / C2x |

---

## License

Pure C23. Zero external dependencies. Public domain / MIT (project-specific).
