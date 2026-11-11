# nativera — Detailed Plan

**Goal:** a Batocera-class bootable console distro (x86 / ARM / RPi / handheld) whose "games" are **natively recompiled** (launch-once native executable + BYO assets + manifest) instead of emulated cores. nativera is a **distribution** that hosts the already-growing corpus of community recompiled games; we only build an engine where a real gap exists.

**North-star proof (end of Phase 1):** boot your Batocera HDD → a native game (Asura's Wrath, already proven) shows up in the menu, launches natively, saves persist. No emulation in that path.

---

## 0. Ground truth (your install, verified today)

`/run/media/dio/BATOCERA` = the **FAT boot partition**:
- `boot/linux` (23 MB kernel), `boot/initrd.gz` (754 KB)
- `boot/batocera` (3.39 GB) = **root filesystem archive** (OS + EmulationStation + all cores)
- `boot/rufomaculata` (912 MB) = second component archive (verify exact role in Phase 0)
- `boot/EFI/`, `boot/syslinux/`, `boot/grub/` = UEFI + legacy bootloaders
- `batocera-boot.conf` = boot config: `sharedevice=INTERNAL`, nvidia/amdgpu/prime driver knobs, your wifi (`JioFiber_5G`)

**Not yet visible:** the ext4 **root / userdata partition** (where ROMs, saves, `/roms`, `/userdata` live). Only the FAT boot partition is auto-mounted. **First action: mount & inventory the root partition.**

Implication: nativera inherits Batocera's 3-tier model:
1. **boot partition (FAT32)** — kernel, initrd, bootloaders, `batocera-boot.conf` → this is what makes it boot on "all sorts of devices."
2. **root partition (ext4)** — the OS + ES + our **native runtimes** (we add these here).
3. **userdata/share partition** — `/roms`, saves, and our **recomp packages** (BYO-disc-friendly, survives updates).

---

## 1. Decision register (need your call where marked ⚠)

| # | Decision | Chosen | Why |
|---|----------|--------|-----|
| D1 | Chassis | **Derive from Batocera** (keep boot+root model+ES), don't build a new OS | You want "light enough to boot everywhere" — Batocera already solves that. Cheapest path. |
| D2 | Front-end | **Reuse EmulationStation**; add native capability in two stages: (a) stock `program` system first, (b) fork/patch a "recomp" provider later | Stock program system = zero patching for MVP; patch only when we need manifest-grade depth. |
| D3 | Native-title mechanism | **Phase 1: ES `program` launcher. Phase 2: a dedicated `recomp` system fed by `recomplauncher.json`** | Graduated; each stage is independently shippable. |
| D4 | Catalogue model | **BYO-disc on `/userdata/recomp/<title>/`** + RecompStore-installable zips (both, manifest is the shared spine) | Matches your Asura package model + the upstream "bring your own disc" ethos. |
| D5 | Console coverage | **No emulator forks — author our own native ports** (or adopt a proven community recomp). Gaps are *not* listed, not filled with an emu core. | Stronger native-first stance: every listed title is native. ⚠ confirmed |
| D6 | Legal posture | **Ship no game files or discs; user brings their own** (like Batocera). RecompStore carries only the native runtime + metadata. | Clean, matches RecompLauncher rules. **Confirmed.** |

---

## 2. The five layers (refined)

```
BOOT        kernel+initrd+grub/EFI+batocera-boot.conf     (inherit from Batocera)
FRONT-END   EmulationStation (+ a "recomp" provider in Ph.2)
CATALOGUE   /userdata/recomp/<title>/  = native binary + recomplauncher.json
            + assets (BYO) + saves/
ENGINE      per-console recompiler+runtime installed in root:
            PS3 .ppumod hydrate engine (proven), then N64/GC/WiiU/PS1-2/X360…
DISTRIBUTION build native zips → GitHub Releases → RecompStore; on-device
             update overwrites install, preserves saves/ per manifest
```

The **manifest is the spine** (full spec: `docs/MANIFEST.md`). A folder of files becomes a *title* only when `recomplauncher.json` declares: game-copy check, start command, config write semantics, save/user-data globs, pre-start step, exit codes.

**Critical:** RecompLauncher is already a *consumer* of this manifest. If nativera's front-end speaks the same manifest, **every upstream-submitted recomp drops into our catalogue and works** — we build the consumer once. Our per-console engines (Phase 3) are only for families with *no* upstream recomp yet.

---

## 3. Phased delivery

### Phase 0 — Recon & inventory (do first, ~1–2 sittings)
- [x] Mount the ext4 root/userdata partition (mounted at `/run/media/dio/SHARE`); ES data tree is `SHARE/roms` + `SHARE/system`.
- [x] Inventory ROM families → **done, `docs/GAP-MATRIX.md`** (Tier A already-native x86 → Tier B GB/GBA flagship port → Tier C → Tier D PS3 proven).
- [ ] Confirm `boot/rufomaculata` role; snapshot `batocera-boot.conf` as our reference config.
- [x] Lock the manifest field list from the SDK page → **done, `docs/MANIFEST.md`** (verbatim spec + nativera mapping + Asura reference).
- **Deliverable:** `docs/ARCHITECTURE.md` + `docs/GAP-MATRIX.md`.

### Phase 1 — "Program-as-recomp" MVP  ← the north star
Goal: **a native game boots and plays on a real Batocera image, zero ES patching.**
Two candidate first titles, in order of value-per-effort:
- **A. Tier-A free win:** package an existing x86 `ports/`/`windows/`/`model3` title as a manifest-driven program (no porting — the exe is already native). Fastest possible "it's a native console distro" proof.
- **B. The flagship:** the **Game Boy / GBA native port** (our own, no emulator fork) — 1,961 `gb` + 285 `gba` ROMs as the regression battery. The real "native, not emulated" headline.
Both share the same seam: ES `program` launcher + `recomplauncher.json` + `/userdata/recomp/<title>/`.
- [ ] **Pick the first title.** Quick proof = an x86 `ports/`/`windows/`/`model3` exe (zero porting). Reference-engine proof = the already-proven **Asura's Wrath PS3 package**.
- [ ] Copy it into `userdata` as a **program**; write the thin **wrapper** the program launcher execs (for Asura: hydrate PPU cache, set `PPU Modules Mode: Prefer` + `PPU Modules Dir:` idempotently, `exec engine/rpcs3`).
- [ ] Add program **metadata** (art, save dir, launch cmd) so it renders in the ES grid.
- [ ] Boot the image (physical HDD or a disk-image in QEMU), confirm: appears in menu → launches natively → a save writes to `saves/` → reload works.
- [ ] **Flagship (Phase 1B):** author the **Game Boy + GBA native port** (our own, no emulator fork); drive it via `recomplauncher.json` + `prepare`; test against the `gb`/`gba` ROM battery.
- **Deliverable:** `BOOT-PROOF.md` with the exact menu→launch→save trace, + the first GB/GBA titles running natively.

### Phase 2 — The `recomp` system (generalize)
Goal: **any number of native titles plug in via manifest; no per-game ES code.**
- [ ] Define the **`recomp` system + folder convention** (`/userdata/recomp/<title>/`).
- [ ] Fork/patch EmulationStation to add a **native provider**: reads `recomplauncher.json` → registers title, binds save dir, execs the binary, surfaces config, runs pre-start, maps exit codes. (Falls back to an ES **external-provider XML + `batocera` command** if a full patch is too heavy.)
- [ ] Build the **catalogue scanner** (`/userdata/recomp/*` → inject into ES).
- [ ] Prove **two** different console families in the menu at once.
- **Deliverable:** a second native title ships manifest-only.

### Phase 3 — Per-console engines (fill the gap matrix)
Goal: native catalogue grows, family by family.
- [ ] For each target (N64, GameCube, WiiU, PS1/PS2, Xbox 360, Dreamcast…): adopt/port a proven native recompiler or existing community recomp; wrap it in a manifest; install the runtime into the root; add to `launchers/` + `manifests/`.
- [ ] PS3 is the reference (Asura). Each engine = root install + one `manifests/*.json` + one `launchers/<console>.sh`.
- **Deliverable:** updated GAP-MATRIX with native coverage; catalogue grows.

### Phase 4 — Distribution & updates
Goal: the "distro, not toolchain" story; on-device updates.
- [ ] Build native zips with `recomplauncher.json` → GitHub Releases → **RecompStore** listing.
- [ ] On-device **update tool**: overwrite the install, **preserve `saves/`** per manifest `folders`/`saves` globs (your ensure/always/new config discipline applies here).
- **Deliverable:** an on-device update that keeps user data, from a RecompStore/GH source.

### Phase 5 — Multi-device & polish
- [ ] Verify boot across device classes: x86 laptop, ARM, **Raspberry Pi**, handheld.
- [ ] GPU driver handling (nvidia/amdgpu/prime) from `batocera-boot.conf`.
- [ ] Theming, save management, controller config, power.
- **Deliverable:** `docs/DEVICES.md` — which native titles run on which class.

---

## 4. Console / gap matrix

**Authoritative version: `docs/GAP-MATRIX.md`** (built from the real ROM tree). Build order: Tier A (already-native x86: `ports/`, `windows/`, `model3/`, `naomi/`) → **Tier B flagship: our own Game Boy + GBA native port, tested against ~2,246 ROMs** → Tier C (PS1/Dreamcast/N64/DS/3DS) → Tier D (PS3, proven). No emu forks; gaps stay out of the menu.

---

## 5. Risks & mitigations

- **ES patch depth** — mitigated by Phase 1 using the *stock* program system (no patch); only patch in Phase 2 when needed, with an external-provider fallback.
- **Native runtimes are big** (GBs, vs. KB ROMs). HDD/x86 fine; SD/RPi tight → tier devices by native-library size; that's the point of "native" (fewer, real native titles, not thousands of ROMs).
- **BYO-disc / licensing** — platform ships no game files or discs; RecompStore carries only runtime+metadata. Clean by design.
- **Native gaps** — no emu-fork fallback (D5): a console without a native path is simply *not listed*. Tier A x86 wins keep the menu non-empty on day one, so an empty menu never ships.
- **Install verification** — only the FAT boot partition is visible now; Phase 0 mounts the root before we touch anything.

---

## 6. Immediate next steps (today)
1. **Ground the Phase-1 seam:** read how Batocera's `program` system + `ports/` (`Celeste64.sh` + `.keys`) actually register/launch an exe → design the `/userdata/recomp/<title>/` + `recomplauncher.json` drop-in precisely.
2. **Start Phase 1A (quick win):** package one existing x86 `ports/`/`model3` title as a manifest-driven program → boot proof.
3. **Then Phase 1B (flagship):** author the Game Boy + GBA native port, test against the `gb`/`gba` battery.

All inputs confirmed: D5 = no emu forks / own native ports; D6 = BYO-disc; manifest spec locked (`docs/MANIFEST.md`); ROM inventory done (`docs/GAP-MATRIX.md`).
