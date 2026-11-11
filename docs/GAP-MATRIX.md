# nativera — Gap Matrix (from the real Batocera ROM tree)

Source: `/run/media/dio/SHARE/roms/` (253 system folders, ext4 `SHARE` 921 GB).
Most folders are 8 KB ES placeholders; the real game mass is in a dozen families.

**Directives baked in:** (1) **no emulator forks** — where a native path exists we author **our own native port**; (2) test batteries come from the existing ROM tree.

## Tier A — already native x86 (ship as-is, no port work)

The "ROM" *is* the original x86 executable. nativera just launches it. Main requirement = GPU driver on target.

| Family | In install | Native work |
|--------|-----------|-------------|
| `ports/` (Celeste64, Doom, etc. — `.sh` + `.keys`) | 23 wrappers, 121 MB | **zero** — wrapper already exists; adopt as model |
| `windows/`, `prboom`, `mohaa`, `quake*` | ~58 MB | **zero** — x86 exes |
| `model2/`, `model3/`, `naomi/` (arcade, x86+GPU, in `.zip`) | ~58 MB | **near-zero** — unzip + run; needs GPU |

## Tier B — prime "our own native port" target, and we have the test battery

Simple-to-medium CPU ISAs that a native recompiler handles cleanly. This is where we *author ports*, not fork emulators.

| Family | In install | CPU → host | Why first |
|--------|-----------|-----------|-----------|
| **`gb` (Game Boy)** | **1,961 `.gb`, 567 MB** | 8-bit → LLVM | simplest real ISA; 1,961-ROM regression battery |
| **`gba` (Game Boy Advance)** | **285 `.gba`, 3.2 GB** | **ARM7TDMI (32-bit ARM)** → LLVM | clean 32-bit ARM, ports beautifully |
| `gbc`/`sgb`/`lynx` | compat/variants | GB-superset | rides the GB port |

**→ `gb` + `gba` = the #1 native-port target: ~2,246 ROMs to test against, both very tractable, and it's the biggest real "native, not emulated" win.**

## Tier C — tractable but a real lift (adopt/author a proven native recomp)

| Family | In install | CPU → host | Notes |
|--------|-----------|-----------|-------|
| `psx` (PS1) | 9, 2 GB (.bin/.cue + images) | MIPS R3000 | native PS1 ports exist |
| `dreamcast` | 7, 1.2 GB (.gdi/.bin) | SH4 + DSP | tractable-ish |
| `n64` | 301, 4.9 GB (.z64) | MIPS64 R4000 | known N64 recompiler projects; 301-ROM battery |
| `nds` | 97, 4 GB (.zip) | ARM9 + ARM7 | dual-CPU |
| `3ds` | 7, 6.1 GB (.3ds) | ARM11 | bigger lift |
| `gamecube` | stub here | MIPS + GPU | needs GPU |

## Tier D — proven reference (already ours)

| Family | Path | State |
|--------|------|-------|
| `ps3` | RPCS3 PPU-recompile (`.ppumod` hydrate) | **proven** (Asura's Wrath on the host) |

## Gaps — no native path yet (leave out of the menu; do NOT ship an emu fork)

`xbox360` (stub), `switch` (9 files ≈ title-data, not full games), `ps4`/`ps5`, `wii`/`wiiu` (stubs), `xbox` (stub). These stay gaps until we author/adopt a native port. Per the no-fork rule, an empty family is *never listed* rather than listed-with-emulator.

## Build order (matches the phases)

1. **Tier A wins first** (minutes): package `ports/`, `windows/`, `model3/`, `naomi/` as manifest-driven native titles → the menu is non-empty on day one with *zero* port work.
2. **Tier B = the flagship port**: author a **Game Boy + GBA native port** (our own, no emulator fork); use the 2,246 ROMs as the regression battery. This is the flagship "native, not emulated" demonstration.
3. **Tier C** family-by-family as native recompiler/ports mature (N64 next: 301-ROM battery).
4. **Tier D** (PS3) already proven → ship as the reference title.
