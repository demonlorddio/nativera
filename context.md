# nativera — Project Context (authoritative state file)

> **Read this first.** It is the single source of truth for *where the project actually is*.
> It is not a plan, not a vision doc, and not a summary of a chat. Every claim here was
> verified against the repo or the machine on the date shown. Where something is unverified,
> it says so.
>
> **Last verified: 2026-11-15** by Saffron (Perch). Replaces the previous `context.md`,
> which was a hallucinated session summary and has been overwritten.

---

## 1. What nativera is

A bootable console distro in the Batocera shape, where the "cores" are **native executables**
rather than emulators. Boot from a USB stick, get a gamepad-driven home screen, pick a title,
it runs as real host code.

Two layers, and they must never be confused:

| Layer | What it is | Where |
|---|---|---|
| **Boot image (the OS)** | Standalone **Limine UEFI** chain: GPT/EF00 ESP + kernel + busybox initramfs. **Not** a Batocera image. | `flashdrive/` |
| **Catalogue spine** | The **RecompLauncher `format: 1`** manifest. A folder becomes a title only when `recomplauncher.json` describes it. | `docs/MANIFEST.md`, `engine/consumer/` |

**The pitch:** emulation simulates the whole machine every frame. A recomp compiles the game's
code to the host CPU once and runs real native code. nativera is the distribution that hosts
those native titles.

**Legal posture:** ship no game files, no discs, no keys. The user brings their own copy.
This is not negotiable and it is what makes the project shippable.

---

## 2. Decisions (locked — do not relitigate without the owner saying so)

| # | Decision | Value | Status |
|---|---|---|---|
| D1 | Boot chassis | Standalone Limine UEFI. Not a Batocera image. | locked |
| D2 | **Front-end** | **EmulationStation, reused and re-themed.** ES is the home screen; we make it look better, we do not replace it. | **locked 2026-11-15** |
| D3 | Native-title mechanism | Phase 1: ES `program`/`sh` seam (zero patching). Phase 2: a dedicated `recomp` system fed by the manifest. | locked |
| D4 | Catalogue layout | `/userdata/recomp/<title>/` = binary + manifest + BYO assets + `saves/` | locked |
| D5 | Console coverage | Native where a native path exists. **No emulator forks.** A gap is not listed, never filled with an emu core. | locked |
| D6 | Legal | BYO everything. Ship runtime + metadata only. | locked |
| D7 | Language | Rust where it wins (consumer, front-end logic, cores). C where it wins (Limine/kernel/busybox glue). Efficiency and stability over tidy-looking code. | locked |
| D8 | Git policy | Commit freely as savepoints. **Push only when verified.** First public beta when publishable. | locked |
| D9 | Device order | x86 first. ARM/Pi/handheld after x86 is useful. | locked |

### D2 in detail — the ES decision, and why it keeps flip-flopping

**The owner's call (2026-11-15):** EmulationStation stays. It is a good home screen, it is
what Batocera users already know, and rebuilding it is a waste of the project's scarcest
resource. We **re-theme** it: better typography, better layout, console-grouped presentation,
cleaner art pipeline. Inspiration, then improvement.

**Why this needed a ruling:** three documents disagreed, and each new session picked a
different one.

- `docs/PLAN.md` said reuse ES. ✅ correct
- `~/.claude/skills/nativera/SKILL.md` said the ES idea was "superseded" by a custom X11/SDL2
  launcher. ❌ **wrong, and it was poisoning sessions**
- `engine/frontend/` is a Rust SDL2 menu — real, working, tested code. It is **not** the
  shipping home screen under D2.

**What that means for the Rust front-end:** it is not wasted, and it is not deleted. It is
demoted from "the front-end" to a **native launcher/proof harness**: it proves the shell-free
launch contract, the ROM scan semantics, and the SDL event loop in a language we want to keep.
Its `launch.rs` contract (byte-exact `OsString` paths, `0–255` / `128+signum` / `Err`) is
genuinely reusable by whatever eventually execs a title. Keep it, test it, don't grow it into
a competitor to ES until there is a reason.

**Open sub-question (not yet decided):** does the ES theme work happen as (a) a stock ES theme
+ `es_systems.cfg` config, or (b) a fork of ES with a `recomp` provider? (a) is cheaper and
ships sooner. (b) is needed for manifest-grade depth (settings UI, prepare progress, exit-code
messages). **Recommendation: (a) first, (b) only when the manifest features actually block us.**

---

## 3. What exists, verified

### 3.1 Boot layer — `flashdrive/` — **built, does not boot**

Files are correct and present on the stick:

```
/run/media/dio/BA88-0A7A/
├── EFI/BOOT/BOOTX64.EFI      Limine UEFI (12.x)
├── EFI/BOOT/limine.conf      searched first on EFI
├── limine.conf               ESP-root fallback
├── vmlinuz                   17,785,344 bytes (Nov 12)
└── initramfs.cpio.gz         1,659,499 bytes (valid newc+gzip; verified by listing)
```

`flashdrive/rootfs/init` is a clean Phase-1 init: mounts proc/sys/devtmpfs, prints a banner,
`exec /bin/sh`. Correct.

**BLOCKER 1 — partition type (this is why it did not boot).**
```
sda1   29.3G   vfat   W95 FAT32 (LBA)   /run/media/dio/BA88-0A7A
```
Type is **W95 FAT32 (LBA)**, not **EFI System (EF00)**. Firmware only creates a
`UEFI: <device>` boot entry for a GPT partition typed EF00. A legacy FAT32 volume gets at best
a generic "USB HDD" entry, which most firmware will not boot. This is exactly the P0 root cause
already written down in the skill's Skill 1 — it was never applied.

**Verified 2026-11-15:** everything *else* on the stick is correct and would boot.
- `EFI/BOOT/BOOTX64.EFI` md5 `a53618af…` — byte-identical to the system's Limine 12.9.6.
- `limine.conf` syntax checked against `/usr/share/doc/limine/CONFIG.md`: `protocol: linux`,
  `kernel_path`, `module_path`, `cmdline` are all valid v12 options.
- `vmlinuz` is a valid bzImage: `protocol 2.15`, 64-bit EFI handoff entry point, ZST-compressed.
- `initramfs.cpio.gz` is valid newc+gzip; a fresh repack from `flashdrive/build/rootfs`
  produces 69 entries / 1.66 MB and lists cleanly.

So the partition table is the **only** thing standing between the project and a booting console.

**Fix:** `flashdrive/fix-partition.sh` (added 2026-11-15). GPT + ESP type EF00, FAT32, remount.
Refuses any non-USB device and requires the device path typed back as confirmation.
`sgdisk`/`gdisk` are not installed; the script uses `parted` (present). Needs root.

**BLOCKER 2 — the bundled kernel was gone from the host. FIXED 2026-11-15.**
`build.sh` used to hardcode `KERNEL_VERSION=6.18.55-2-cachyos-lts`, which no longer exists on
this machine (the host is now `6.18.60-2-cachyos-lts`, plus `7.3.3-1-cachyos`). It now
auto-detects: newest `*-lts` module dir with a `vmlinuz`, else newest module dir. Verified to
resolve `6.18.60-2-cachyos-lts`. All other build inputs confirmed present: busybox,
`BOOTX64.EFI`, `ld-linux-x86-64.so.2`, `libc.so.6`, `libcrypt.so.2`.

**Minor — FIXED 2026-11-15:** `limine.conf` now sets `default_entry: 1`.

### 3.2 Catalogue / manifest layer — **working**

- `docs/MANIFEST.md` — the spec, with the nativera path mapping.
- `launchers/LAUNCHER.md` — the full RecompLauncher contract, cross-checked against the real
  Asura manifest. This is the best document in the repo.
- `launchers/RECOMP_SEAM.md` — proves the Batocera `sh` generator seam:
  `ES → configgen → /bin/bash run.sh`. A directory ROM is first-class; the script self-locates.
  **This is the zero-patch integration point and it is the reason D3 Phase 1 is cheap.**
- `engine/consumer/` — Rust, serde-typed `format: 1`. Tests pass.
- `engine/recomp-launcher.py` — the Python predecessor; self-test reports 10/10.
- `manifests/recomplauncher.json` — a working GB example.

### 3.3 Front-end — `engine/frontend/` (Rust) and `engine/launcher/` (C)

**Rust** (`nativera_frontend`, bin `nativera-fe`): 724 lines across `launch.rs`, `roms.rs`,
`ui.rs`, `main.rs`. **16 tests pass** (`make fe-tests`): 13 lib + 1 sdl smoke + 2 ui smoke.
Genuinely good work — shell-free `posix_spawn`, byte-exact `OsString` paths with a
shell-injection test, 100-entry scan cap with placeholder, exit-code mapping.

**C** (`engine/launcher/`): the original SDL menu, still built by `make launcher`, still the
spec the Rust port was written against. Retained deliberately.

Under D2, both are **launch-contract references**, not the shipping home screen.

### 3.4 Engine — `engine/gb/` — **an interpreter, not a recompiler**

~2,041 lines of C: `gb_cpu.c` (1,254 lines, a **252-case `switch (opcode)`**), `gb_mmu.c`,
`gb_ppu.c`, `gb_runtime.c`, `gb_main.c`, plus four KAT test binaries. `make tests` passes:
CPU KATs, MMU, runtime integration, PPU timing, launch contract.

**This is the sharpest honesty problem in the project.** The project's entire thesis is
"native, not emulated." A 252-case opcode switch is an emulator. `engine/gb/run.sh` calls
itself a "Batocera Native Port Wrapper", which the code does not back up.

**Ruling:** treat `engine/gb/` as a **correctness oracle and placeholder**, explicitly labelled
as such everywhere it surfaces. It is genuinely useful: it is the reference behaviour the real
recompiler will be validated against, and it lets the boot→menu→launch→save chain be proven
before any recompiler exists. It must **not** be presented as a nativera title.

**Correction to the docs:** `launchers/LAUNCHER.md` line 207 calls
`engine/gb/gbcore.c` a "broken draft". **That file does not exist.** The real file is
`engine/gb/gb_cpu.c`, and it is not broken — its KATs pass. The "broken scaffold" warning is
stale and should be removed or re-pointed.

---

## 4. Test status (run 2026-11-15, this machine)

| Suite | Command | Result |
|---|---|---|
| C: GB core + launcher | `make tests` | **PASS** (CPU KATs, MMU, runtime, PPU timing, launch) |
| Rust: front-end | `make fe-tests` | **PASS** — 16 tests |
| Rust: manifest consumer | `cargo test -p nativera_consumer` | PASS |
| Python consumer self-test | `python3 -I engine/test-recomp-launcher.py` | 10/10 (per doc; not re-run this pass) |

**Note on the old summary's "All green (0 passed, 0 failed)":** zero tests run is not a pass.
It means the filter matched nothing. Real counts are above.

---

## 5. Git state

- Branch: **`main`** (not `master`).
- HEAD: `eb697d9` — *fe: make targets for the Rust front-end (fe, fe-tests); C launcher retained*.
- **6 commits ahead of `origin/main`, unpushed.** The GitHub repo does not have the Rust front-end.
- Untracked: `docs/superpowers/`, `.superpowers/` (gitignored), `context.md` (this file).

```
eb697d9  fe: make targets for the Rust front-end (fe, fe-tests); C launcher retained
315a359  fe: C-faithful SDL event loop, frame render, and shell-free launch wiring
035e216  fe: shell-free launch module (raw OsString arg, 0-255 / 128+sig / Err contract)
9abd892  fe: roms scan module (case-sensitive .gb/.sh, 100-cap, placeholder, OsString paths)
b4ee8f9  fe: scaffold nativera_frontend crate (sdl2 0.38 + use-pkgconfig + ttf, headless smoke test)
bd074c6  Front-end launcher: shell-free game launch via posix_spawn
265e70d  Port RecompLauncher consumer to Rust (serde-typed format:1)
```

Per D8: these are verified (tests pass), so they are **pushable**.

---

## 6. What is NOT done

- ❌ The stick does not boot (partition type; stale kernel).
- ❌ No ES theme work has started. No `es_systems.cfg` for a `recomp` system.
- ❌ No `recomp` system defined on a real Batocera/ES tree.
- ❌ No settings UI, no file manager, no package installer.
- ❌ No recompiler exists for any console. The GB core is an interpreter.
- ❌ No release, no distribution, no update path.
- ❌ No code review has ever been performed on the Rust front-end.
  (`.superpowers/sdd/.../review-bd074c6..eb697d9.diff` is a 1,035-line *review package* —
  commits + diff. It contains no findings. The "review dispatched and pending" claim in the
  old context file was false.)

---

## 7. Next actions, in order

1. **Delete the hallucinated artifacts.** The old `context.md` is already replaced by this file.
   Do not resurrect session summaries as project docs.
2. **Fix the partition table to GPT/EF00** and re-deploy. This is the single thing standing
   between the project and a booting console. **Script ready:** `flashdrive/fix-partition.sh`
   (needs root; refuses non-USB devices). Then `./flashdrive/build.sh /run/media/dio/NATIVERA`.
3. **Fix `build.sh` kernel detection** so it cannot silently bundle a kernel that is not there.
   **DONE 2026-11-15** — now auto-detects the newest `*-lts` module dir.
4. **Push the 6 commits** (D8: verified → pushable).
5. **Correct the stale docs:** `LAUNCHER.md` `gbcore.c` reference; `PLAN.md` D2 row to match
   the ES decision; the skill's plugin table (it routes to 12 plugins, **none installed**) and
   its "ES superseded" claim.
6. **Then** the ES work: stock theme + `es_systems.cfg` for a `recomp` system, prove one title
   appears and launches.

---

## 8. Two-week target (owner's words, made concrete)

Owner wants: *"function as a console — homescreen like Batocera, settings, file, package
installer, and run simple NES/GB recomps."*

That is five products. Ranked by what actually makes it a console:

| # | Goal | Realistic in 2 weeks? |
|---|---|---|
| 1 | Boots to ES home screen on real hardware | **yes** — blocked only on the partition fix |
| 2 | Console-grouped game presentation (theming) | **yes** |
| 3 | Launches a native binary from the menu, save persists | **yes** — the `sh` seam already proves this |
| 4 | Settings screen | **maybe** — ES has a native one; theme it rather than build it |
| 5 | File manager | **cut** — not a console feature, it's a desktop feature |
| 6 | Package installer | **cut** — replace with "drop a folder in `/userdata/recomp/`" for now |
| 7 | Runs GB "recomps" | **reframe** — runs the GB **interpreter placeholder**, labelled honestly, until a recompiler exists |

**The honest two-week demo:** boots on real hardware → ES home screen with console groups →
pick a title → it launches natively → a save persists across reboot. That is a console.
Everything else is a later phase.

---

## 9. Standing rules for anyone working in this repo

1. **Update `docs/` in the same commit as the code.** A decision that is not written down gets
   re-decided wrongly by the next session. This is the single biggest source of drift here.
2. **Never claim a test passed without the count.** "0 passed" is not green.
3. **Never claim a review happened unless findings exist.** A review *package* is not a review.
4. **Never describe the GB core as native.** It is an interpreter. Say so.
5. **Never ship game files, discs, or keys.**
6. **Commit as savepoint, push when verified** (D8).
7. **If a doc and the code disagree, the code wins** — then fix the doc.
8. **Do not route to plugins or skills that are not installed.** Check before citing.

---

## 10. Open questions for the owner

1. **ES integration depth:** stock theme + config first (cheap, ships), or fork ES with a
   `recomp` provider (needed for manifest-grade settings/prepare UI)? *Recommendation: stock first.*
2. **The GB core:** keep as labelled placeholder, or stop work on it entirely until a real
   recompiler design exists? *Recommendation: keep as oracle, stop growing it.*
3. **`/run/media/dio/SHARE` is not mounted right now.** The gap matrix and the ROM battery
   depend on it. Remount before any inventory work.
4. **Root access for the partition fix** — needs `sudo`; the password was not available in the
   last session.
5. **What is `boot/rufomaculata`?** Still unconfirmed from Phase 0. Probably irrelevant now
   that the image is standalone Limine, but it is an open item in `PLAN.md`.
