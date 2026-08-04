# FORGE Game Engine — Game Design Document
## Ember Online: A 2D Action-MMORPG

---

## 1. Vision

**Ember Online** is a 2D action-MMORPG built on the FORGE engine — a pure C23, zero-dependency game engine written entirely from scratch. Every pixel, every packet, every sound is ours.

The game emphasizes:
- **Skill-based combat**: Real-time action with combos, dodging, and ability rotations
- **Massive worlds**: Seamless zones supporting 500+ players per zone
- **Player-driven economy**: Supply chains, crafting professions, open markets
- **Social depth**: Guilds, housing, world events, instanced dungeons
- **Accessibility**: Full keyboard/mouse, gamepad, and terminal (TUI) support

---

## 2. Core Gameplay Loop

```
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│  EXPLORE │ → │  COMBAT  │ → │  LOOT    │ → │  UPGRADE │
│  (world) │    │  (mobs)  │    │  (gear)  │    │  (craft) │
└──────────┘    └──────────┘    └──────────┘    └────┬─────┘
      ↑                                              │
      └──────────────────────────────────────────────┘
              (repeat with stronger character)
```

### 2.1 Combat System
- **Real-time action**: No tab-targeting; aim skills manually
- **Stamina bar**: Dodging, sprinting, heavy attacks consume stamina
- **Skill rotations**: 8 ability slots with cooldowns
- **Elemental system**: Fire, Ice, Lightning, Earth, Wind, Void
- **Status effects**: Burn, freeze, shock, poison, bleed, stun

### 2.2 Progression
- **Level cap**: 100 (soft cap), infinite paragon levels beyond
- **Skill trees**: 3 branches per class (e.g., Warrior: Berserker/Tank/Commander)
- **Crafting professions**: Blacksmith, Alchemist, Enchanter, Cook, Scribe
- **Equipment tiers**: Common → Uncommon → Rare → Epic → Legendary → Mythic
- **Socket system**: Gems with elemental affinities

### 2.3 World Structure
- **Overworld**: 12 seamless zones, each ~4km²
- **Cities**: 3 major hubs with banks, auction houses, guild halls
- **Dungeons**: 40 instanced dungeons (5-player), 8 raids (20-player)
- **World bosses**: 12 epic encounters on 6-hour respawn timers
- **Dynamic events**: Invasions, escorts, defend-the-point (scale to player count)

---

## 3. Technical Requirements

### 3.1 Performance Targets

| Metric | Target | Stress Test |
|---|---|---|
| Entities per zone | 10,000+ | 50,000 entities |
| Network tick rate | 20 Hz server, 60 Hz client | 100 players in combat |
| Latency | < 100ms global, < 50ms regional | Cross-continent play |
| Memory per zone | < 512 MB server | Full zone loaded |
| Client frame rate | 60 FPS minimum | 2,000 sprites + UI + effects |
| Server tick time | < 5ms per zone | Full physics + AI + net sync |
| Login capacity | 10,000 concurrent | Authentication burst |

### 3.2 Supported Platforms

| Platform | Backend | Status |
|---|---|---|
| Linux terminal | TUI (VT100/xterm256) | ✅ Working |
| Linux framebuffer | DRM/KMS | ✅ Implemented |
| Linux Wayland | Raw wire protocol | ✅ Implemented |
| Linux X11 | Raw wire protocol | ✅ Implemented |
| Linux OpenGL | EGL + GLES2 (runtime load) | ✅ Implemented |
| Windows | Win32 + GDI | ✅ Implemented |
| macOS | Terminal only (for now) | ✅ Terminal works |

---

## 4. Game Systems

### 4.1 Entity Component System (ECS)
- **Sparse sets**: O(1) component lookup, cache-friendly iteration
- **Archetypes**: Entities with the same component set share a storage layout
- **Systems**: Run in parallel where possible (physics, AI, rendering)
- **Replication**: Only sync components marked with `[[network]]`

### 4.2 Networking
- **Authoritative server**: Client predicts, server validates
- **Rollback netcode**: 150ms input buffer for competitive combat
- **Interest management**: Only sync entities within 50m of player
- **Compression**: Varint + delta encoding for position updates
- **Anti-cheat**: Server-side hit validation, speed checks, cooldown enforcement

### 4.3 Economy
- **Open market**: Player-listed auctions, no NPC vendors for gear
- **Supply chains**: Ore → Ingot → Weapon → Enchanted Weapon
- **Resource rarity**: Common (70%) → Uncommon (20%) → Rare (8%) → Epic (2%)
- **Currency**: Gold (standard), Ember Shards (premium), Crafting Tokens (soulbound)

### 4.4 Social
- **Guilds**: Up to 100 members, guild levels, shared bank, guild halls
- **Parties**: 5-player, shared exp bonus, loot distribution rules
- **Chat channels**: Global, Zone, Guild, Party, Trade, Whisper, System
- **Housing**: Instanced plots, furniture crafting, trophy displays

### 4.5 AI
- **State machines**: Patrol → Alert → Combat → Flee → Return
- **Behavior trees**: For bosses and elite mobs
- **Flocking**: Herd animals, swarm insects
- **Pathfinding**: A* on uniform grid with jump point search

---

## 5. Art & Audio Direction

### 5.1 Visual Style
- **2D hand-painted**: Rich, detailed sprites with normal maps for lighting
- **Dynamic lighting**: Point lights, ambient occlusion, day/night cycle
- **Particle effects**: Spell effects, weather, environmental ambiance
- **UI aesthetic**: Dark fantasy — obsidian, gold, crimson accents

### 5.2 Audio
- **Dynamic music**: Layered tracks that intensify in combat
- **Positional audio**: 3D spatial audio for spells, footsteps, ambience
- **UI sounds**: Satisfying clicks, hover tones, notification chimes

---

## 6. Development Milestones

### Phase 0: Foundation ✅
- [x] Core engine modules (core, math, memory, time, log, serialize)
- [x] Build system and test suite
- [x] UI toolkit integration (7 backends, 20+ widgets)

### Phase 1: First Screen 🔄
- [ ] Software renderer: draw a triangle
- [ ] Load and display a sprite
- [ ] Basic game loop with frame timing
- [ ] Title screen with "Press Start"

### Phase 2: Player Character
- [ ] Character creation (class, appearance)
- [ ] Movement (WASD / gamepad)
- [ ] Basic attack (click / button)
- [ ] Collision with world geometry

### Phase 3: World
- [ ] Tilemap loading and rendering
- [ ] Zone transitions
- [ ] Day/night cycle
- [ ] Weather effects

### Phase 4: Combat
- [ ] 3 abilities per class
- [ ] Health / stamina / mana bars
- [ ] Damage numbers (floating text)
- [ ] Death and respawn

### Phase 5: Networking
- [ ] Connect to server
- [ ] See other players
- [ ] Basic chat
- [ ] Server-side validation

### Phase 6: MMORPG Features
- [ ] Quest system
- [ ] Inventory and equipment
- [ ] Crafting
- [ ] Trading
- [ ] Guilds
- [ ] Dungeons

### Phase 7: Polish
- [ ] Particle effects
- [ ] Sound effects and music
- [ ] Settings menu
- [ ] Tutorial
- [ ] Launch!

---

## 7. Monetization (Post-Launch)

- **Base game**: Free-to-play
- **Cosmetics**: Character skins, mounts, housing items
- **Convenience**: Extra bank slots, character slots, exp boosters (non-tradable)
- **Expansion packs**: New zones, classes, level cap increases
- **NO pay-to-win**: All power items are earned in-game

---

## 8. Competitive Analysis

| Game | Strengths | FORGE Differentiator |
|---|---|---|
| **RuneScape** | Deep skilling, economy | Real-time combat, modern netcode |
| **Albion Online** | Full-loot PvP, player economy | PvE focus, instance dungeons |
| **Stardew Valley** | Relaxing, crafting | MMO scale, real-time combat |
| **Terraria** | Exploration, building | Persistent world, MMO social |
| **Old School RuneScape** | Nostalgia, community | Modern engine, from-scratch codebase |

---

## 9. Risk Assessment

| Risk | Mitigation |
|---|---|
| Scope creep | Strict milestone gates, MMORPG features deferred to Phase 6+ |
| Netcode complexity | Start with LAN co-op, scale to MMO |
| Art pipeline | Procedural generation + placeholder art initially |
| Server costs | Efficient C23, horizontal scaling per zone |
| Cheaters | Authoritative server, client is dumb renderer |

---

## 10. Team & Tools

- **Engine**: FORGE (pure C23, zero deps)
- **Build**: Make + Clang
- **Version control**: Git
- **Issue tracking**: GitHub Issues
- **Communication**: Discord
- **Art**: Aseprite (sprites), Tiled (maps)
- **Audio**: Sfxr (synthesis), Audacity (editing)

---

*Document version: 1.0*
*Game codename: Ember Online*
*Engine codename: FORGE*
*Last updated: 2026-07-21*
