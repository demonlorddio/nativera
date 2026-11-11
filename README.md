# nativera

**A Batocera-shaped bootable console distro that runs games *natively* — no emulation cores, no per-frame CPU sim. Each title is a recompiled, launch-once native executable + its disc/assets + a manifest.**

Working title. Rename freely.

## The one-sentence pitch

Batocera = EmulationStation front-end + a pile of emulator cores + config, bootable from USB/SD/HDD. Every game is a ROM that a core *emulates*.
nativera = the same front-end and boot experience, but the "cores" are **native recompiled game binaries** that launch instantly and run at native speed.

- **Emulation** simulates the whole machine every frame → heavy, ceiling-bound, GPU/translation overhead.
- **Recomp (native)** compiles the game's code to the host CPU *once*, hydrates the warm cache, and runs real native code → lighter, faster, cleaner.

We already proved the single-game pattern: a self-contained **PS3 recompiler launcher** for Asura's Wrath (zero-precompile hydration, one executable + bring-your-own-disc + `rom_data` cache). nativera is that pattern promoted to a distro.

## Architecture (five layers)

```
┌─────────────────────────────────────────────────────────────┐
│ 1. BOOT        bootable image (USB/SD/HDD), kernel + initrd  │
│                    same job as Batocera's boot layer         │
├─────────────────────────────────────────────────────────────┤
│ 2. FRONT-END   native gamepad menu (EmulationStation-style)  │
│                    catalogue, saves, settings, power control │
├─────────────────────────────────────────────────────────────┤
│ 3. CATALOGUE   one "recomp" package per title:               │
│                    - native executable (recompiled game)     │
│                    - recomplauncher.json manifest            │
│                    - assets/disc (BYO, not shipped)          │
│                    - user-data dir (saves, settings)         │
├─────────────────────────────────────────────────────────────┤
│ 4. ENGINE(S)   per-console recompiler + runtime:             │
│                    - the .ppumod hydrate engine (PS3, proven)│
│                    - per-console launchers in launchers/     │
├─────────────────────────────────────────────────────────────┤
│ 5. DISTRIBUTION  build → native zips → RecompStore / GH rel  │
│                    (recomplauncher manifest = the contract)  │
└─────────────────────────────────────────────────────────────┘
```

The **manifest** (recomplauncher.com standard, schema v1) is the spine: it's what makes a folder of files into a *title* — game-copy check, start command, config write semantics, save/user-data globs, pre-start step. The front-end just reads these and knows how to launch, save, and update.

## The "figure out a way" part

RecompLauncher's standards table only covers some consoles. The gap matrix is the real work:

| Console | Native path | Status |
|---------|-------------|--------|
| PS3 | RPCS3 PPU-recompile (`.ppumod` hydrate) | **proven** (Asura's Wrath) |
| …     | …           | to map |
| …     | …           | to map |

nativera doubles as the umbrella to **fill that table**: wherever a console lacks a proven native recompiler, the front-end + manifest layer stays put and we add the engine when the recomp works. Consoles with no viable native path yet fall back to… well, we decide.

## Repo layout

- `docs/` — this vision, architecture notes, per-engine writeups
- `manifests/` — `recomplauncher.json` templates + examples per title
- `launchers/` — per-console native launchers (the Asura `asuraswrath` shell is the reference)
- `engine/` — recompiler build glue (points at the RPCS3 fork for PS3 today)
- `assets/` — front-end art/theme (placeholder)

## Relationship to existing work

- **`/home/dio/Games/AsurasWrathRecomp-linux-x86_64/`** — the proven PS3 single-game package. Layer 3+4 for PS3.
- **`/home/dio/Projects/ps3-recomp/rpcs3`** — the custom RPCS3 fork (PPU-modules feature). The engine source.
- **recomplauncher.com** — the distribution/manifest *standard* we adopt (not a toolchain).

## Open questions

1. Base image: build on Batocera directly (reuse its boot + ES) or on a lighter base (Debian + LXDE + a slim ES fork)?
2. Front-end: reuse EmulationStation (knows emulators — must teach it "native titles") or a new thin native menu that only speaks the manifest?
3. Catalogue format: per-title folder on `/userdata` (BYO-disc friendliness) vs. a RecompStore-installed tree?
4. Naming: keep `nativera`?
